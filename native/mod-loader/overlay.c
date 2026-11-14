/* SDK 17 screens (docs/modding/overlay.md): the loader passes them to HellForge's overlay host, hellforge_overlay.dll
 * beside the loader. The host is loaded at the first add_screen and starts drawing once the game's entities are ready.
 * Each screen goes to the host as a stand-in whose callbacks check that its mod still runs: a mod taken out while the
 * game runs leaves its screen closed, and its next version takes the same screen over. */
#include "../overlay/hellforge_overlay_host.h"

#define OVERLAY_SCREENS 64

typedef struct OverlayScreen
{
    HfMod* mod;     /* NULL once the mod is taken out */
    char modId[80]; /* who may take it over */
    HfScreen def;   /* the mod's own */
    int id;         /* the host's */
} OverlayScreen;

static OverlayScreen g_overlayScreens[OVERLAY_SCREENS];
static int g_overlayScreenCount;
static SRWLOCK g_overlayLock = SRWLOCK_INIT;
static HfOverlayAddScreenFn g_overlayAdd;
static HfOverlayShowFn g_overlayShow;
static HfOverlayStartFn g_overlayStart;
static int g_overlayLoaded; /* 0 not tried, 1 loaded, -1 missing */
static volatile LONG g_overlayStarted;
static char g_overlayFonts[2][MAX_PATH * 3];

/* The screen's mod, when it still runs; g_overlayLock need not be held (a pointer read). */
static HfMod* OverlayOwner(const OverlayScreen* s)
{
    HfMod* mod = s->mod;
    return mod != NULL && !mod->removing ? mod : NULL;
}

static int OverlayDraw(void* user, const HfDraw* draw, const HfInput* input, float dt)
{
    OverlayScreen* s = user;
    return OverlayOwner(s) != NULL ? s->def.draw(s->def.user, draw, input, dt) : 0;
}

static void OverlayOpened(void* user, int open)
{
    OverlayScreen* s = user;
    if (OverlayOwner(s) != NULL && s->def.opened != NULL) s->def.opened(s->def.user, open);
}

static int OverlayCanOpen(void* user)
{
    OverlayScreen* s = user;
    if (OverlayOwner(s) == NULL) return 0;
    return s->def.can_open == NULL || s->def.can_open(s->def.user);
}

static void OverlayLog(void* user, const char* line) { Note("%s", line); }

static int OverlayHook(void* user, uintptr_t target, void* detour, void** original)
{
    return LoaderHook("overlay host", target, detour, original);
}

/* The game's own mouse pointer, which its menus show: the engine makes it from the game's mouse_cursor resource (Lua
 * Window.set_cursor, exe 0x3285E4) and keeps it in its first window at +0x68, the HWND at +0x50 and the arrow it falls
 * back to at +0x70 (exe 0x6167F0). The engine is the global the code at exe 0x328508 reads. */
#define OVERLAY_ENGINE 0x1A10210u
static const char kOverlayEngineCode[] = "4C 8B 0D 01 7D 6E 01 33 FF 48 8B F2 48 8B D9 48 85 C9 74 2E 49 8B 89 C0 03 00 00 41 8B 81 B8 03 00 00";
static const char kOverlaySetCode[] = "48 8B 48 28 48 89 4B 68";
static const char kOverlayShowCode[] = "80 79 41 00 75 0D 48 8B 79 68 48 85 FF 75 04 48 8B 79 70 48 8B CF FF 15 ?? ?? ?? ?? 48 8B 4B 50 4C 8B C7 BA F4 FF FF FF";

/* NULL when the code is not the reviewed one, the engine has no window, or the window is not `window`. */
static void* OverlayGameCursorIn(uintptr_t exe, void* window)
{
    uintptr_t engine = 0, windows = 0, w = 0, hwnd = 0, cursor = 0, arrow = 0;
    uint32_t count = 0;
    if (!ApiCodeMatches(exe + 0x328508u, kOverlayEngineCode) || !ApiCodeMatches(exe + 0x3285E4u, kOverlaySetCode) ||
        !ApiCodeMatches(exe + 0x616815u, kOverlayShowCode)) return NULL;
    if (SafeRead(exe + OVERLAY_ENGINE, &engine, 8) || SafeRead(engine + 0x3B8, &count, 4) || count == 0 ||
        SafeRead(engine + 0x3C0, &windows, 8) || SafeRead(windows, &w, 8) || SafeRead(w + 0x50, &hwnd, 8) ||
        SafeRead(w + 0x68, &cursor, 8) || SafeRead(w + 0x70, &arrow, 8)) return NULL;
    if ((window != NULL && hwnd != (uintptr_t)window) || arrow != (uintptr_t)LoadCursorW(NULL, MAKEINTRESOURCEW(32512))) return NULL; /* IDC_ARROW */
    return (void*)cursor;
}

static void* OverlayGameCursor(void* user, void* window) { return OverlayGameCursorIn((uintptr_t)GetModuleHandleW(NULL), window); }

/* The folder of a file of the loader's, with a trailing backslash; "" when unknown. */
static void LoaderFolder(wchar_t* out, DWORD capacity)
{
    HMODULE self = NULL;
    out[0] = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void*)LoaderFolder, &self)) return;
    DWORD n = GetModuleFileNameW(self, out, capacity);
    if (n == 0 || n >= capacity) { out[0] = 0; return; }
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash != NULL) slash[1] = 0;
}

/* The game's fonts, which HellForge writes into the editor's folder: the plan's hellforge.editor, else
 * <install>\mods\hellforge-editor from the loader's <install>\app\sdk-shims. */
static void OverlayFindFonts(void)
{
    char folder[MAX_PATH * 3] = "";
    for (int i = 0; i < g_modCount && !folder[0]; i++)
        if (!strcmp(g_mods[i].id, "hellforge.editor") && g_mods[i].folder[0]) snprintf(folder, sizeof folder, "%s\\", g_mods[i].folder);
    if (!folder[0])
    {
        wchar_t here[MAX_PATH];
        LoaderFolder(here, MAX_PATH);
        if (here[0]) WideCharToMultiByte(CP_UTF8, 0, here, -1, folder, sizeof folder, NULL, NULL);
        if (folder[0]) strncat(folder, "..\\..\\mods\\hellforge-editor\\", sizeof folder - strlen(folder) - 1);
    }
    snprintf(g_overlayFonts[0], sizeof g_overlayFonts[0], "%sfs-sinclair.ttf", folder);
    snprintf(g_overlayFonts[1], sizeof g_overlayFonts[1], "%sfs-sinclair-medium.ttf", folder);
}

/* hellforge_overlay.dll from the loader's own folder, once; g_overlayLock held. */
static int OverlayLoad(void)
{
    if (g_overlayLoaded != 0) return g_overlayLoaded > 0;
    g_overlayLoaded = -1;
    wchar_t path[MAX_PATH];
    LoaderFolder(path, MAX_PATH);
    if (!path[0] || wcslen(path) + 24 >= MAX_PATH) { Note("overlay: the loader's folder is unknown; screens are not drawn"); return 0; }
    wcscat(path, L"hellforge_overlay.dll");
    HMODULE host = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (host != NULL)
    {
        g_overlayStart = (HfOverlayStartFn)(void*)GetProcAddress(host, "hf_overlay_start");
        g_overlayAdd = (HfOverlayAddScreenFn)(void*)GetProcAddress(host, "hf_overlay_add_screen");
        g_overlayShow = (HfOverlayShowFn)(void*)GetProcAddress(host, "hf_overlay_show");
    }
    if (host == NULL || !g_overlayStart || !g_overlayAdd || !g_overlayShow)
    {
        Note("overlay: hellforge_overlay.dll %s (error %lu); screens are not drawn", host == NULL ? "could not be loaded" : "lacks its exports", GetLastError());
        return 0;
    }
    g_overlayLoaded = 1;
    return 1;
}

/* Starts the host once there is a screen and the game's entities are ready. */
static void OverlayStartIfReady(void)
{
    if (!g_fired[HF_EVENT_ENTITIES_READY] || g_overlayLoaded <= 0 || InterlockedCompareExchange(&g_overlayStarted, 1, 0) != 0) return;
    OverlayFindFonts();
    HfOverlayStart start = {sizeof start, NULL, OverlayLog, OverlayHook, NULL, g_overlayFonts[0], g_overlayFonts[1], OverlayGameCursor};
    if (g_overlayStart(&start) != 0) Note("overlay: the host did not start; screens are not drawn");
}

/* A taken-out mod's screen with the same key, for its next version; g_overlayLock held. */
static OverlayScreen* OverlayTakeOver(const HfMod* mod, const HfScreen* screen)
{
    for (int i = 0; i < g_overlayScreenCount; i++)
    {
        OverlayScreen* s = &g_overlayScreens[i];
        if (s->mod == NULL && screen->key != 0 && s->def.key == screen->key && !strcmp(s->modId, mod->id)) return s;
    }
    return NULL;
}

/* The mod's screen kept and given to the host; g_overlayLock held. 0 refused. */
static int OverlayKeep(HfMod* mod, const HfScreen* screen)
{
    OverlayScreen* s = OverlayTakeOver(mod, screen);
    if (s != NULL)
    {
        memset(&s->def, 0, sizeof s->def);
        memcpy(&s->def, screen, screen->size < sizeof s->def ? screen->size : sizeof s->def);
        MemoryBarrier(); /* the render thread sees the new callbacks before the mod */
        s->mod = mod;
        return s->id;
    }
    if (g_overlayScreenCount >= OVERLAY_SCREENS) return 0;
    s = &g_overlayScreens[g_overlayScreenCount];
    memset(s, 0, sizeof *s);
    memcpy(&s->def, screen, screen->size < sizeof s->def ? screen->size : sizeof s->def);
    snprintf(s->modId, sizeof s->modId, "%s", mod->id);
    HfScreen stand = s->def;
    stand.size = sizeof stand;
    stand.user = s;
    stand.draw = OverlayDraw;
    stand.opened = OverlayOpened;
    stand.can_open = OverlayCanOpen;
    s->id = g_overlayAdd(&stand);
    if (s->id == 0) return 0;
    s->mod = mod;
    g_overlayScreenCount++;
    return s->id;
}

static int ApiAddScreen(HfMod* mod, const HfScreen* screen)
{
    if (mod == NULL || ModGone(mod)) return 0;
    if (screen == NULL || screen->size < offsetof(HfScreen, opened) || screen->draw == NULL) { ApiLog(mod, "add_screen: no HfScreen or no draw callback"); return 0; }
    AcquireSRWLockExclusive(&g_overlayLock);
    int id = OverlayLoad() ? OverlayKeep(mod, screen) : 0;
    ReleaseSRWLockExclusive(&g_overlayLock);
    if (id == 0) { ApiLog(mod, "add_screen(%s): refused (modloader.log says why)", screen->name ? screen->name : "unnamed"); return 0; }
    OverlayStartIfReady();
    return id;
}

static void ApiScreenShow(HfMod* mod, int id, int open)
{
    if (mod == NULL || ModGone(mod)) return;
    int mine = 0;
    AcquireSRWLockShared(&g_overlayLock);
    for (int i = 0; i < g_overlayScreenCount && !mine; i++) mine = g_overlayScreens[i].id == id && g_overlayScreens[i].mod == mod;
    ReleaseSRWLockShared(&g_overlayLock);
    if (mine) g_overlayShow(id, open);
    else ApiLog(mod, "screen_show(%d): not a screen of this mod", id);
}

/* A mod taken out: its screens close and stay closed until its next version adds them again. */
static void OverlayForget(const HfMod* mod)
{
    AcquireSRWLockExclusive(&g_overlayLock);
    for (int i = 0; i < g_overlayScreenCount; i++)
    {
        if (g_overlayScreens[i].mod != mod) continue;
        g_overlayScreens[i].mod = NULL;
        g_overlayShow(g_overlayScreens[i].id, 0);
    }
    ReleaseSRWLockExclusive(&g_overlayLock);
}
