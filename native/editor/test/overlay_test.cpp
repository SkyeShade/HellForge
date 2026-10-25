/* The editor against a real D3D12 swap chain, without the game: a window that presents frames like the game does (clear,
 * ExecuteCommandLists, Present), the editor DLL loaded with a small stand-in SDK (log, on, hook through MinHook, set_live,
 * get_live, mod_folder), F8 pressed, the window resized and the editor closed and opened again; then, as a player would,
 * Tab to the objects and to the fields, a field picked with the arrows, "1200" typed and applied with F9. It passes when
 * every Present succeeds, the device is never removed, the editor reports it is drawing, its window shows on screen (with
 * a screenshot), the change reached set_live, and Local Stats holds it. With D3D12, an "overlay" has a direct queue of
 * its own on the same device and runs a list on it before and after the game's every frame (as RivaTuner, Steam, OBS or
 * Overwolf can): the editor must draw on the swap chain's queue, not that one. And after the first frames, an "overlay"
 * hooks Present's and Present1's code over the editor's as RivaTuner does (issue #5), calling past the editor: the editor
 * must get in front of it again and still draw, the overlay still called, and the shared method table left alone (with
 * a real RivaTuner in the process, RivaTuner is that overlay). Then the overlay takes its hook off, the code put back as
 * it was before the editor: the editor must put its own hook back.
 * Usage: overlay_test.exe <hellforge_editor.dll> [<screenshot.bmp> [<editor folder: editor-catalog.json, game-icons.bin>]]
 * HELLFORGE_EDITOR_TEST_D3D11=1: the window presents with Direct3D 11 instead, as the game does with --use-d3d11.
 * HELLFORGE_EDITOR_TEST_NO_OVERLAY_QUEUE=1: no overlay queue (a real overlay, RivaTuner say, tried on its own). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "MinHook.h"
#include "hellforge_mod.h"

struct HfMod { const HfApi* api; };
static HfMod g_mod;
static HfApi g_api;
static HfEventCallback g_ready;
static int g_drawing, g_hooked, g_lives, g_presetImports;
static void* g_drawnQueue; /* the queue the editor says it draws on (D3D12) */
static int g_again; /* times the editor got in front of an overlay that hooked over it */
static int g_putBack; /* times the editor put its own hook back after an overlay put the code back as it was */
static int g_modPage, g_modApplied; /* a mod's stratagem: its page made, its saved value sent to its own row */
static char g_modDir[MAX_PATH] = "";
static HfSession g_sessionNow; /* the co-op role the stand-in loader reports */
static void Session(HfMod*, HfSession* out) { *out = g_sessionNow; }
static uint32_t g_statsNow = HF_STATS_HOST; /* whose stats a joined game plays with (SDK 15) */
static uint32_t StatsSource(HfMod*) { return g_statsNow; }
static HfNotice g_noticeNow; /* HellForge's message for the player, as the stand-in loader reports it */
static void Notice(HfMod*, HfNotice* out) { *out = g_noticeNow; }
/* SDK 14, mod sync: the offer the stand-in loader reports, how often the editor read it, and the answers it sent */
static HfSyncState g_syncNow;
static HfSyncMod g_syncList[3];
static int g_syncReads, g_syncOpens, g_syncAnswers[7], g_syncAlone, g_syncGone, g_editorOpens, g_syncWarnings;
static char g_syncBlocked[65], g_syncTrusted[65], g_syncOpened[65];
static int SyncState(HfMod*, HfSyncState* out)
{
    g_syncReads++;
    uint32_t size = out->size;
    memcpy(out, &g_syncNow, size < sizeof g_syncNow ? size : sizeof g_syncNow);
    out->size = size;
    return g_syncNow.state != HF_SYNC_NONE;
}
static int SyncMod(HfMod*, uint32_t index, HfSyncMod* out)
{
    if (g_syncNow.state == HF_SYNC_NONE || index >= g_syncNow.mods || index >= 3) return 0;
    uint32_t size = out->size;
    memcpy(out, &g_syncList[index], size < sizeof(HfSyncMod) ? size : sizeof(HfSyncMod));
    out->size = size;
    return 1;
}
static int SyncAnswer(HfMod*, uint32_t version, uint32_t answer, const char* id)
{
    if (g_syncNow.state == HF_SYNC_NONE || version != g_syncNow.version || answer < 1 || answer > 6) return 0;
    g_syncAnswers[answer]++;
    if (answer == HF_SYNC_BLOCK) snprintf(g_syncBlocked, sizeof g_syncBlocked, "%s", id ? id : "");
    if (answer == HF_SYNC_TRUST) snprintf(g_syncTrusted, sizeof g_syncTrusted, "%s", id ? id : "");
    if (answer == HF_SYNC_OPEN) snprintf(g_syncOpened, sizeof g_syncOpened, "%s", id ? id : "");
    return 1;
}
static void SyncOffer(uint32_t state, uint32_t version, uint64_t bytes, uint64_t done)
{
    g_syncNow = {};
    g_syncNow.size = sizeof g_syncNow; g_syncNow.state = state; g_syncNow.version = version; g_syncNow.mods = 3;
    g_syncNow.bytes = bytes; g_syncNow.done = done;
    snprintf(g_syncNow.host, sizeof g_syncNow.host, "Friend Host");
}
static double g_lastLive;
static char g_folder[MAX_PATH] = "";
static char g_logsFolder[MAX_PATH] = "";

static void Log(HfMod*, const char* format, ...)
{
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    printf("  mod: %s\n", line);
    if (strstr(line, "drawing on the game's queue") || strstr(line, "drawing with Direct3D 11")) g_drawing = 1;
    if (const char* at = strstr(line, "drawing on the game's queue ")) sscanf(at + strlen("drawing on the game's queue "), "%p", &g_drawnQueue);
    if (strstr(line, "hooked the swap chain")) g_hooked = 1;
    if (strstr(line, "the editor runs first again")) g_again++;
    if (strstr(line, "the editor's hook is back in place")) g_putBack++;
    if (strstr(line, "staged (APPLY writes them)")) g_presetImports++;
    if (strstr(line, "A/MG-101 Heavy MG Sentry gets a page")) g_modPage = 1;
    if (strstr(line, "editor: mod sync") && strstr(line, "the dialog opens")) g_syncOpens++;
    if (strstr(line, "the mod sync prompt shows over the game")) g_syncAlone++;
    if (strstr(line, "the mod sync prompt goes")) g_syncGone++;
    if (strstr(line, "the trust warning for")) g_syncWarnings++;
    if (strstr(line, "sound: the editor opens")) g_editorOpens++;
}
static const char* Build() { return "25480438"; }
static const char* ModFolder(HfMod*) { return g_folder; }
static int On(HfMod* mod, HfEvent event, HfEventCallback callback, void* user) { if (event == HF_EVENT_ENTITIES_READY) g_ready = callback; return 0; }
/* the originals the editor got, by target (the test's "overlay" calls past the editor with them) */
static struct { uintptr_t target; void** original; } g_hooks[64];
static int g_hookCount;
static int Hook(HfMod*, uintptr_t target, void* detour, void** original)
{
    if (MH_CreateHook((void*)target, detour, original) != MH_OK) return 1;
    if (g_hookCount < 64) { g_hooks[g_hookCount].target = target; g_hooks[g_hookCount++].original = original; }
    return MH_EnableHook((void*)target) == MH_OK ? 0 : 1;
}
static void* OriginalOf(void* target) { for (int i = 0; i < g_hookCount; i++) if (g_hooks[i].target == (uintptr_t)target) return *g_hooks[i].original; return NULL; }
/* An overlay that hooks Present's and Present1's code after the editor as RivaTuner does: its own jump written over the
 * one at the start of the code (to a stub of its own within 2 GB, which jumps on to its detour), its detour calling the
 * code as it was before the editor, past the editor's detour. */
typedef HRESULT(STDMETHODCALLTYPE* PresentCall)(IDXGISwapChain3*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Call)(IDXGISwapChain3*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PresentCall g_overlayPresentNext;
static Present1Call g_overlayPresent1Next;
static int g_overlayPresents;
static HRESULT STDMETHODCALLTYPE OverlayPresent(IDXGISwapChain3* chain, UINT interval, UINT flags) { g_overlayPresents++; return g_overlayPresentNext(chain, interval, flags); }
static HRESULT STDMETHODCALLTYPE OverlayPresent1(IDXGISwapChain3* chain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{ g_overlayPresents++; return g_overlayPresent1Next(chain, interval, flags, p); }
static bool OverlayHooks(void* code, void* detour)
{
    uint8_t* stub = NULL;
    for (uintptr_t at = ((uintptr_t)code & ~(uintptr_t)0xFFFF) - 0x10000; stub == NULL && at > (uintptr_t)code - 0x70000000; at -= 0x10000)
        stub = (uint8_t*)VirtualAlloc((void*)at, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (stub == NULL) return false; /* its jump goes over the one there (the editor's, or a real overlay's), five bytes */
    stub[0] = 0xFF; stub[1] = 0x25; memset(stub + 2, 0, 4); memcpy(stub + 6, &detour, 8);
    uint8_t jump[5] = {0xE9};
    int32_t rel = (int32_t)(stub - ((uint8_t*)code + 5));
    memcpy(jump + 1, &rel, 4);
    DWORD protect;
    VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &protect);
    memcpy(code, jump, 5);
    VirtualProtect(code, 5, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), code, 5);
    return true;
}
static int g_resets;
static int g_refuseCapacity; /* the big preset's test: magazine capacities refused (its errors) */
static const char* LastLog(HfMod*) { return "set_live: test: magazine capacities are refused here"; }
static const char* LogsFolder(HfMod*) { return g_logsFolder; }
static double g_modCooldown = -2; /* the mod stratagem's cooldown as the editor last set it (-1: its edit undone) */
static int SetLive(HfMod*, HfRef target, const HfField* field, const HfValue* value, uint32_t flags)
{
    if (g_refuseCapacity && value && field && field->offset == 136) return 0;
    if (target == 150 && value && value->number == 150 && field && !strcmp(field->component, "StratagemDefinition")) g_modApplied++;
    if (target == 150 && field && !strcmp(field->component, "StratagemDefinition")) g_modCooldown = value ? value->number : -1;
    g_lives++;
    if (value) g_lastLive = value->number;
    else if (flags & HF_LIVE_BASE) g_resets++;
    return HF_APPLIES_RESPAWN;
}
/* every field holds 640, its base 600; whole-number fields (references among them) hold their base. With
 * HELLFORGE_EDITOR_TEST_VIEW=mods (the MODS view's screenshot), mods hold some: a Lua mod a damage row's armour
 * penetration (+12, 9 where an editor change holds 10), a code mod the projectile settings' velocity (+32). */
static bool g_modsView, g_enemiesView; /* HELLFORGE_EDITOR_TEST_VIEW=enemies: a screenshot of an enemy's category at the end */
/* HELLFORGE_EDITOR_TEST_LIVE_US=<n>: each get_live takes n microseconds, as a slow lookup in the mod loader can */
static LONGLONG g_liveCostUs;
static int GetLive(HfMod*, HfRef, const HfField* field, HfLiveValue* out)
{
    if (g_liveCostUs > 0)
    {
        LARGE_INTEGER f, start, now;
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&start);
        do QueryPerformanceCounter(&now); while ((now.QuadPart - start.QuadPart) * 1000000 / f.QuadPart < g_liveCostUs);
    }
    out->now.type = out->base.type = field->type;
    out->now.number = field->type == HF_TYPE_U32 ? 600 : 640;
    out->base.number = 600;
    out->mod = NULL;
    if (g_modsView && !strcmp(field->component, "DamageInfo") && field->offset == 12)
    { out->mod = "lua.mods-azura-railgun_repenetrated"; out->base.number = 9; out->now.number = 9; }
    if (g_modsView && !strcmp(field->component, "ProjectileSettings") && field->offset == 32)
    { out->mod = "skyeshade.vanilla-plus"; out->base.number = 1200; out->now.number = 1200; }
    return HF_APPLIES_RESPAWN;
}
/* SDK 10, mods: two HellForge mods and a Lua mod */
static int Mods(HfMod*, uint32_t index, HfModInfo* out)
{
    static const HfModInfo list[] = {
        {sizeof(HfModInfo), HF_MOD_CODE, "skyeshade.vanilla-plus", "", "", 1},
        {sizeof(HfModInfo), HF_MOD_STATS, "skyeshade.hell-forge", "", "", 1},
        {sizeof(HfModInfo), HF_MOD_EDITOR, "hellforge.editor", "", "", 1},
        {sizeof(HfModInfo), HF_MOD_LUA, "lua.mods-azura-railgun_repenetrated", "", "mods/azura/railgun_repenetrated", 1}};
    if (index >= sizeof list / sizeof list[0]) return 0;
    *out = list[index];
    return 1;
}
/* fields applied at boot (entity deltas, say): every one holds 1.5 (whole ones 7), its base 1 */
static int g_stats;
static int GetStat(HfMod*, const HfStat* stat, HfLiveValue* out)
{
    g_stats++;
    out->now.type = out->base.type = stat->type;
    out->now.number = stat->type == HF_TYPE_F32 ? 1.5 : 7;
    out->base.number = 1;
    out->mod = NULL;
    return 1;
}
/* A mod's content, as the loader lists it: a copy of the A/MG-43 sentry's stratagem (type 150) dropping a copy of the
 * sentry (its own designation) that fires a copy of its round dealing a copy of its damage. */
static const HfEditorEntity kModEntities[] = {{0xABCDull, 0x37CDE43876BA26BBull}, {0, 0}};
static int Content(HfMod*, uint32_t index, HfContent* out)
{
    HfContent c = {};
    c.size = sizeof c;
    c.mod = "test.mod";
    c.folder = g_modDir;
    switch (index)
    {
    case 0: c.kind = HF_KIND_STRATAGEM; c.id = 150; c.base = 121; c.name = "test/mod/hmg"; c.title = "Heavy MG Sentry";
        c.icon = "test/mod/icons/hmg"; c.editor_like = 121; c.editor_entities = kModEntities; break;
    case 1: c.kind = HF_KIND_ENTITY; c.id = 0xABCD; c.base = 0x37CDE43876BA26BBull; c.name = "test/mod/hmg_entity"; c.designation = "A/MG-101"; break;
    case 2: c.kind = HF_KIND_PROJECTILE; c.id = 400; c.base = 148; c.link = 700; c.name = "test/mod/round"; break;
    case 3: c.kind = HF_KIND_DAMAGE; c.id = 700; c.base = 125; c.name = "test/mod/damage"; break;
    default: return 0;
    }
    *out = c;
    return 1;
}

static void Key(HWND window, WPARAM key) { SendMessageW(window, WM_KEYDOWN, key, 0); SendMessageW(window, WM_KEYUP, key, 0xC0000000); }
static void Type(HWND window, const char* text) { for (; *text; text++) SendMessageW(window, WM_CHAR, (WPARAM)*text, 0); }
/* The screen as it is into a 24-bit BMP (path NULL: none); returns how many pixels in `area` (if given) are not the
 * clear colour. */
static int Screenshot(const char* path, UINT width, UINT height, const RECT* area = NULL)
{
    HDC screen = GetDC(NULL), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, (int)width, (int)height);
    SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, (int)width, (int)height, screen, 0, 0, SRCCOPY);
    BITMAPINFOHEADER header = {sizeof header, (LONG)width, -(LONG)height, 1, 24, BI_RGB};
    DWORD row = ((width * 3 + 3) & ~3u), size = row * height;
    BYTE* pixels = (BYTE*)malloc(size);
    GetDIBits(memory, bitmap, 0, height, pixels, (BITMAPINFO*)&header, DIB_RGB_COLORS);
    BITMAPFILEHEADER file = {0x4D42, (DWORD)(sizeof file + sizeof header + size), 0, 0, (DWORD)(sizeof file + sizeof header)};
    FILE* out = path != NULL ? fopen(path, "wb") : NULL;
    if (out) { fwrite(&file, sizeof file, 1, out); fwrite(&header, sizeof header, 1, out); fwrite(pixels, size, 1, out); fclose(out); }
    int drawn = 0;
    if (area != NULL)
        for (LONG y = area->top; y < area->bottom && y < (LONG)height; y++) for (LONG x = area->left; x < area->right && x < (LONG)width; x++)
        {
            BYTE* p = pixels + y * row + x * 3;
            if (!(p[1] > 50 && p[1] < 80 && p[0] < 30 && p[2] < 30)) drawn++;
        }
    free(pixels); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(NULL, screen);
    return drawn;
}

static bool CopyInto(const char* from, const char* folder, const char* name)
{
    char source[MAX_PATH], target[MAX_PATH];
    snprintf(source, sizeof source, "%s\\%s", from, name);
    snprintf(target, sizeof target, "%s\\%s", folder, name);
    return CopyFileA(source, target, FALSE) != 0;
}
static void* Record(HfMod*, HfRef, const char*, uint32_t, uint32_t size) /* a Liberator record holding 640 rpm */
{
    static HfProjectileWeapon weapon;
    weapon.fire_rate[1] = 640.0f;
    return size == sizeof weapon ? &weapon : NULL;
}

int main(int argc, char** argv)
{
    if (argc < 2) { puts("usage: overlay_test.exe <hellforge_editor.dll> [<screenshot.bmp> [<editor folder>]]"); return 2; }
    SetProcessDPIAware(); /* the screenshot in real pixels, not a scaled copy */
    MH_Initialize();
    g_api.version = HF_MOD_SDK_VERSION; g_api.size = sizeof g_api;
    g_api.log = Log; g_api.build_id = Build; g_api.on = On; g_api.hook = Hook; g_api.set_live = SetLive; g_api.record = Record; g_api.mod_folder = ModFolder;
    g_api.get_live = GetLive;
    g_api.last_log = LastLog; g_api.logs_folder = LogsFolder; g_api.get_stat = GetStat; g_api.session = Session; g_api.stats_source = StatsSource;
    g_api.content = Content;
    g_api.mods = Mods;
    g_api.notice = Notice;
    g_api.sync_state = SyncState; g_api.sync_mod = SyncMod; g_api.sync_answer = SyncAnswer;
    g_modsView = getenv("HELLFORGE_EDITOR_TEST_VIEW") != NULL && !strcmp(getenv("HELLFORGE_EDITOR_TEST_VIEW"), "mods");
    g_enemiesView = getenv("HELLFORGE_EDITOR_TEST_VIEW") != NULL && !strcmp(getenv("HELLFORGE_EDITOR_TEST_VIEW"), "enemies");
    /* the editor's folder in a mods folder of the test's own: its catalogue and icons, and Local Stats next to it */
    char mods[MAX_PATH], stats[MAX_PATH];
    GetTempPathA(MAX_PATH, mods);
    snprintf(mods + strlen(mods), MAX_PATH - strlen(mods), "hellforge-overlay-test-%lu", GetCurrentProcessId());
    snprintf(g_folder, sizeof g_folder, "%s\\hellforge-editor", mods);
    snprintf(stats, sizeof stats, "%s\\zz.local.stats\\hellforge-stats.json", mods);
    bool catalogued = false;
    if (argc > 3)
    {
        CreateDirectoryA(mods, NULL);
        CreateDirectoryA(g_folder, NULL);
        catalogued = CopyInto(argv[3], g_folder, "editor-catalog.json");
        CopyInto(argv[3], g_folder, "game-icons.bin");
        /* the mod: its data names its icon's image (a 4 x 4 BMP here); a cooldown the editor saved for its stratagem */
        snprintf(g_modDir, sizeof g_modDir, "%s\\test-mod", mods);
        CreateDirectoryA(g_modDir, NULL);
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\hellforge-data.json", g_modDir);
        if (FILE* file = fopen(path, "wb"))
        {
            fputs("{\"format\":1,\"patches\":[{\"archives\":[\"007e093ca718ca1a\"],\"resources\":[{\"icon\":\"icon.bmp\",\"name\":\"test/mod/icons/hmg\"}]}]}", file);
            fclose(file);
        }
        snprintf(path, sizeof path, "%s\\icon.bmp", g_modDir);
        if (FILE* file = fopen(path, "wb"))
        {
            /* the stratagem pack's format: red the category layer, green the white one (BMP rows are blue, green, red) */
            BYTE pixels[4 * 4 * 3];
            for (int i = 0; i < 16; i++)
            {
                bool inner = (i % 4 == 1 || i % 4 == 2) && (i / 4 == 1 || i / 4 == 2);
                pixels[3 * i] = 0; pixels[3 * i + 1] = inner ? 255 : 0; pixels[3 * i + 2] = inner ? 0 : 255;
            }
            BITMAPINFOHEADER header = {sizeof header, 4, 4, 1, 24, BI_RGB};
            BITMAPFILEHEADER bmp = {0x4D42, (DWORD)(sizeof bmp + sizeof header + sizeof pixels), 0, 0, (DWORD)(sizeof bmp + sizeof header)};
            fwrite(&bmp, sizeof bmp, 1, file); fwrite(&header, sizeof header, 1, file); fwrite(pixels, sizeof pixels, 1, file);
            fclose(file);
        }
        snprintf(path, sizeof path, "%s\\mod-stats.json", g_folder);
        if (FILE* file = fopen(path, "wb"))
        {
            fputs("{\"format\":1,\"mod:test/mod/hmg\":{\"test/mod/hmg\":{\"stratagem.cooldown\":150}}}", file);
            fclose(file);
        }
        /* the Patriot's left gun swapped to the Emancipator's (mounted weapon 2), its Mounted Weapons and left gun open */
        char settings[MAX_PATH];
        snprintf(settings, sizeof settings, "%s\\editor-settings.json", g_folder);
        if (FILE* file = fopen(settings, "wb"))
        {
            /* HELLFORGE_EDITOR_TEST_SCALE=1.5: the screenshots at that UI scale (the category list then outgrows its panel) */
            const char* scale = getenv("HELLFORGE_EDITOR_TEST_SCALE");
            fprintf(file, "{%s%s%s\"open\":[\"stratagems/EXO-45 Patriot Exosuit|c|mounts\",\"stratagems/EXO-45 Patriot Exosuit|c|mount:EXO-45 Patriot Exosuit / left_gun\"],"
                  "\"mounts\":{\"EXO-45 Patriot Exosuit|mount.slot_0\":2}}", scale ? "\"ui_scale\":" : "", scale ? scale : "", scale ? "," : "");
            fclose(file);
        }
    }
    g_mod.api = &g_api;

    WNDCLASSEXW type = {sizeof type, 0, DefWindowProcW, 0, 0, GetModuleHandleW(NULL), NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"OverlayTest", NULL};
    RegisterClassExW(&type);
    HWND window = CreateWindowExW(WS_EX_TOPMOST, type.lpszClassName, L"HellForge overlay test", WS_POPUP | WS_VISIBLE, 0, 0, 1280, 720, NULL, NULL, type.hInstance, NULL);
    ID3D12Device* device = NULL; ID3D12CommandQueue* queue = NULL; IDXGIFactory2* factory; IDXGISwapChain1* chain1; IDXGISwapChain3* chain;
    const bool d3d11 = getenv("HELLFORGE_EDITOR_TEST_D3D11") != NULL;
    ID3D11Device* device11 = NULL; ID3D11DeviceContext* context11 = NULL;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = 1280; desc.Height = 720; desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM; desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 3; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (d3d11)
    {
        if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device11, NULL, &context11)) ||
            FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->CreateSwapChainForHwnd(device11, window, &desc, NULL, NULL, &chain1)) ||
            FAILED(chain1->QueryInterface(IID_PPV_ARGS(&chain))))
        { puts("FAIL no D3D11 swap chain on this machine"); return 1; }
        puts("  the window presents with Direct3D 11");
    }
    else if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) || FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) ||
        FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->CreateSwapChainForHwnd(queue, window, &desc, NULL, NULL, &chain1)) ||
        FAILED(chain1->QueryInterface(IID_PPV_ARGS(&chain))))
    { puts("FAIL no D3D12 swap chain on this machine"); return 1; }

    /* the code of Present and Present1, as the swap chain's method table gives it before the editor is in */
    void* presentCode = (*(void***)chain)[8];
    void* present1Code = (*(void***)chain)[22];
    uint8_t presentBefore[8]; /* Present's first bytes as they are before the editor */
    memcpy(presentBefore, presentCode, 8);
    HMODULE editor = LoadLibraryA(argv[1]);
    typedef int (*Init)(const HfApi*, HfMod*);
    Init init = editor ? (Init)(void*)GetProcAddress(editor, "hellforge_mod_init") : NULL;
    if (!init || init(&g_api, &g_mod) != 0 || !g_ready) { puts("FAIL the editor did not initialize"); return 1; }
    g_ready(&g_mod, HF_EVENT_ENTITIES_READY, NULL);
    if (catalogued)
    {
        printf("  mod stratagem: page %s, its saved cooldown sent to its own row %d time(s)\n", g_modPage ? "made" : "NOT made", g_modApplied);
        if (!g_modPage || g_modApplied != 1) { puts("FAIL a mod's stratagem got no page, or its saved value did not reach its own row"); return 1; }
    }

    ID3D12CommandAllocator* allocator = NULL; ID3D12GraphicsCommandList* list = NULL; ID3D12Fence* fence = NULL; ID3D12DescriptorHeap* rtvHeap = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC rtv = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (!d3d11)
    {
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, IID_PPV_ARGS(&list));
        list->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&rtvHeap));
    }
    /* the overlay's own queue and an (empty) list it runs */
    ID3D12CommandQueue* overlayQueue = NULL; ID3D12CommandAllocator* overlayAllocator = NULL; ID3D12GraphicsCommandList* overlayList = NULL;
    if (!d3d11 && !getenv("HELLFORGE_EDITOR_TEST_NO_OVERLAY_QUEUE") && (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&overlayQueue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&overlayAllocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, overlayAllocator, NULL, IID_PPV_ARGS(&overlayList)))))
    { puts("FAIL no second queue for the overlay"); return 1; }
    if (overlayList) overlayList->Close();
    auto overlay = [&]() { if (overlayQueue) { ID3D12CommandList* lists[] = {overlayList}; overlayQueue->ExecuteCommandLists(1, lists); } };
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    UINT64 value = 0;
    int failures = 0;
    UINT width = 1280, height = 720;
    ULONGLONG noticeAt = 0;
    const bool rivaTuner = GetModuleHandleW(L"RTSSHooks64.dll") != NULL; /* a real RivaTuner hooks this process too */
    for (int frame = 0; frame < 420; frame++)
    {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        /* an overlay hooks Present's and Present1's code over the editor's after it has started, and never calls the
         * editor's detour (RivaTuner coming in late, issue #5); the editor must get in front of it again. Where a real
         * RivaTuner is in the process it is that overlay (it hooks over the editor on the first frames), and the test's
         * own is left out: two overlays writing over each other's jump (RivaTuner takes its hook off around each call and
         * puts it back after) is a fight the test would make, not what it checks */
        if (frame == 10 && rivaTuner) printf("  overlay: RivaTuner is in the process; it is the overlay that hooks over the editor\n");
        /* later the overlay takes its hook off as RivaTuner does: Present's code put back as it was before the editor
         * (eight bytes at once); the editor must put its own hook back */
        if (frame == 120 && !rivaTuner)
        {
            LONG64 before, now;
            memcpy(&before, presentBefore, 8);
            memcpy(&now, presentCode, 8);
            DWORD protect;
            VirtualProtect(presentCode, 8, PAGE_EXECUTE_READWRITE, &protect);
            InterlockedCompareExchange64((volatile LONG64*)presentCode, before, now);
            VirtualProtect(presentCode, 8, protect, &protect);
            FlushInstructionCache(GetCurrentProcess(), presentCode, 8);
            printf("  overlay: Present's code put back as it was before the editor, as RivaTuner does when it takes its hook off\n");
        }
        if (frame == 10 && !rivaTuner)
        {
            g_overlayPresentNext = (PresentCall)OriginalOf(presentCode);
            g_overlayPresent1Next = (Present1Call)OriginalOf(present1Code);
            bool cut = g_overlayPresentNext && g_overlayPresent1Next && OverlayHooks(presentCode, (void*)OverlayPresent) &&
                OverlayHooks(present1Code, (void*)OverlayPresent1);
            printf("  overlay: Present's code %s\n", cut ? "hooked over the editor's, as RivaTuner does" : "NOT hooked over (the editor's hooks not found)");
            if (!cut) { puts("FAIL the editor's hooks on Present's code were not where the test looked"); failures++; }
        }
        if (frame == 30 || frame == 260 || frame == 300) SendMessageW(window, WM_KEYDOWN, VK_F8, 0);     /* open, close, open */
        /* closed: a friend's lobby refuses this game's join, and the editor shows why over the "game" (its top centre) */
        if (frame == 264)
        {
            g_noticeNow.version = 1;
            g_noticeNow.kind = HF_NOTICE_ERROR;
            snprintf(g_noticeNow.title, sizeof g_noticeNow.title, "COULDN'T JOIN FRIEND HOST");
            snprintf(g_noticeNow.text, sizeof g_noticeNow.text, "Your gameplay mods don't match the host's.\nMissing: Stratagem Compendium 1.2.0 "
                "(the host has it)\nSmarter Guard Dogs & Sentries: you have 5.2.0, the host has 5.1.0\nTurn on the same gameplay mods on the "
                "launcher's MODS page, then start a new session.");
            noticeAt = GetTickCount64();
        }
        /* the editor reads the notice every 250 ms, and on a fast display the 26 frames to the screenshot take less */
        if (frame == 288) while (GetTickCount64() - noticeAt < 300) Sleep(10);
        if (frame == 290)
        {
            char shot[MAX_PATH] = "";
            if (argc > 2) { snprintf(shot, sizeof shot, "%s", argv[2]); if (char* dot = strrchr(shot, '.')) snprintf(dot, (size_t)(shot + sizeof shot - dot), "-notice.bmp"); }
            RECT top = {(LONG)width / 2 - 200, 30, (LONG)width / 2 + 200, 110};
            int drawn = Screenshot(argc > 2 ? shot : NULL, width, height, &top);
            printf("  notice: %d of %d pixels at the top centre are not the clear colour (the editor closed)\n", drawn, 400 * 80);
            if (drawn < 2000) { puts("FAIL the refused join's message is not on screen while the editor is closed"); failures++; }
        }
        /* the host's side: a friend couldn't join, a small note in the top right corner */
        if (frame == 291)
        {
            g_noticeNow.version = 2;
            g_noticeNow.kind = HF_NOTICE_BRIEF;
            snprintf(g_noticeNow.title, sizeof g_noticeNow.title, "Friend Joiner couldn't join");
            snprintf(g_noticeNow.text, sizeof g_noticeNow.text, "Their gameplay mods don't match yours.\nMissing: Stratagem Compendium 1.2.0");
            noticeAt = GetTickCount64();
        }
        if (frame == 296) while (GetTickCount64() - noticeAt < 300) Sleep(10);
        if (frame == 297)
        {
            char shot[MAX_PATH] = "";
            if (argc > 2) { snprintf(shot, sizeof shot, "%s", argv[2]); if (char* dot = strrchr(shot, '.')) snprintf(dot, (size_t)(shot + sizeof shot - dot), "-brief.bmp"); }
            RECT corner = {(LONG)width - 400, 30, (LONG)width - 40, 70};
            int drawn = Screenshot(argc > 2 ? shot : NULL, width, height, &corner);
            printf("  brief notice: %d of %d pixels in the top right corner are not the clear colour\n", drawn, 360 * 40);
            if (drawn < 2000) { puts("FAIL the host's brief notice is not in the top right corner"); failures++; }
        }
        /* to the objects, the next one back, to its fields, three rows down (past the category header, the projectile swap
         * and the part header), 1200, F9 */
        if (frame == 320 || frame == 330) Key(window, VK_TAB);
        if (frame == 324) Key(window, VK_DOWN);
        if (frame == 326) Key(window, VK_UP);
        /* headers start closed: Right opens the category, Down past the projectile swap to the Flight header, Right opens
         * it, Down to its first row */
        if (frame == 332 || frame == 338) Key(window, VK_RIGHT);
        if (frame == 334 || frame == 336 || frame == 340) Key(window, VK_DOWN);
        if (frame == 342) Type(window, "1200");
        if (frame == 350) Key(window, VK_RETURN);
        if (frame == 356) Key(window, VK_F9);
        if (g_modsView && (frame == 362 || frame == 366 || frame == 370)) Key(window, VK_F6); /* BROWSE, SEARCH, CHANGES, MODS */
        if (frame == 150 || frame == 200) /* the game resizes its swap chain */
        {
            if (!d3d11) { fence->SetEventOnCompletion(value, event); queue->Signal(fence, ++value); WaitForSingleObject(event, 2000); }
            width = frame == 150 ? 1600 : 1280; height = frame == 150 ? 900 : 720;
            SetWindowPos(window, NULL, 0, 0, (int)width, (int)height, SWP_NOZORDER);
            if (FAILED(chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) { puts("FAIL ResizeBuffers"); failures++; }
        }
        const float color[4] = {0.05f, 0.25f, 0.05f, 1.0f}; /* green: what the "game" draws */
        if (d3d11)
        {
            ID3D11Texture2D* back = NULL;
            ID3D11RenderTargetView* view = NULL;
            chain->GetBuffer(0, IID_PPV_ARGS(&back));
            if (back && SUCCEEDED(device11->CreateRenderTargetView(back, NULL, &view)))
            {
                context11->OMSetRenderTargets(1, &view, NULL);
                context11->ClearRenderTargetView(view, color);
                view->Release();
            }
            if (back) back->Release();
            HRESULT presented = chain->Present(1, 0);
            if (FAILED(presented)) { printf("FAIL Present 0x%08lx at frame %d\n", (unsigned long)presented, frame); failures++; }
        }
        UINT index = chain->GetCurrentBackBufferIndex();
        ID3D12Resource* buffer = NULL;
        if (!d3d11)
        {
        chain->GetBuffer(index, IID_PPV_ARGS(&buffer));
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += index * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(buffer, NULL, handle);
        allocator->Reset(); list->Reset(allocator, NULL);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.pResource = buffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        list->ClearRenderTargetView(handle, color, 0, NULL);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        overlay(); /* the overlay's queue first, and last before the present */
        queue->ExecuteCommandLists(1, lists);
        overlay();
        HRESULT presented = chain->Present(1, 0);
        if (FAILED(presented)) { printf("FAIL Present 0x%08lx at frame %d\n", (unsigned long)presented, frame); failures++; }
        queue->Signal(fence, ++value);
        fence->SetEventOnCompletion(value, event); WaitForSingleObject(event, 2000);
        buffer->Release();
        }
        if (frame == 400 && argc > 2) /* the screen as it is, editor open */
        {
            HDC screen = GetDC(NULL), memory = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, (int)width, (int)height);
            SelectObject(memory, bitmap);
            BitBlt(memory, 0, 0, (int)width, (int)height, screen, 0, 0, SRCCOPY);
            BITMAPINFOHEADER header = {sizeof header, (LONG)width, -(LONG)height, 1, 24, BI_RGB};
            DWORD row = ((width * 3 + 3) & ~3u), size = row * height;
            BYTE* pixels = (BYTE*)malloc(size);
            GetDIBits(memory, bitmap, 0, height, pixels, (BITMAPINFO*)&header, DIB_RGB_COLORS);
            BITMAPFILEHEADER file = {0x4D42, (DWORD)(sizeof file + sizeof header + size), 0, 0, (DWORD)(sizeof file + sizeof header)};
            FILE* out = fopen(argv[2], "wb");
            if (out) { fwrite(&file, sizeof file, 1, out); fwrite(&header, sizeof header, 1, out); fwrite(pixels, size, 1, out); fclose(out); }
            /* The editor's window is drawn from (60, 60): its pixels there are not the clear colour. */
            int drawn = 0;
            for (int y = 80; y < 200; y++) for (int x = 80; x < 400; x++)
            {
                BYTE* p = pixels + y * row + x * 3;
                if (!(p[1] > 50 && p[1] < 80 && p[0] < 30 && p[2] < 30)) drawn++;
            }
            printf("  screen: %d of %d pixels in the editor's area are not the clear colour\n", drawn, 120 * 320);
            if (drawn < 1000) { puts("FAIL the editor's window is not on screen"); failures++; }
            free(pixels); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(NULL, screen);
        }
    }
    /* HELLFORGE_EDITOR_TEST_VIEW=enemies, a screenshot only: an Illuminate enemy found in the SEARCH view (F6 from BROWSE)
     * and opened in its category, with the faction emblems on the category, the rows and the header; the run ends there */
    if (g_enemiesView && catalogued && argc > 2)
    {
        auto pump = [&](int n) {
            for (int i = 0; i < n; i++)
            {
                MSG message;
                while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                chain->Present(1, 0);
            }
        };
        Key(window, VK_F6); pump(4);
        /* HELLFORGE_EDITOR_TEST_SEARCH: another entity to open (a structure, say) */
        Type(window, getenv("HELLFORGE_EDITOR_TEST_SEARCH") ? getenv("HELLFORGE_EDITOR_TEST_SEARCH") : "overseer"); pump(6);
        /* the enemy opened with its hundreds of fields, then two seconds of frames: with a slow get_live
         * (HELLFORGE_EDITOR_TEST_LIVE_US), none may stall */
        if (const char* cost = getenv("HELLFORGE_EDITOR_TEST_LIVE_US")) g_liveCostUs = atoll(cost);
        Key(window, VK_RETURN);
        LARGE_INTEGER f, before, after;
        QueryPerformanceFrequency(&f);
        double longest = 0;
        int slow = 0;
        for (int i = 0; i < 120; i++)
        {
            QueryPerformanceCounter(&before);
            pump(1);
            QueryPerformanceCounter(&after);
            double ms = (double)(after.QuadPart - before.QuadPart) * 1000.0 / (double)f.QuadPart;
            longest = ms > longest ? ms : longest;
            slow += ms > 50;
        }
        printf("  enemies view: get_live %lld us; 120 frames, the longest %.0f ms, %d over 50 ms\n", g_liveCostUs, longest, slow);
        if (g_liveCostUs > 0 && longest > 100) { puts("FAIL an enemy's page stalls the frame with a slow get_live"); failures++; }
        g_liveCostUs = 0;
        /* HELLFORGE_EDITOR_TEST_EXPAND=1: its page's first header opened (Tab to the fields, Right), for its rows */
        if (getenv("HELLFORGE_EDITOR_TEST_EXPAND")) { Key(window, VK_TAB); pump(4); Key(window, VK_RIGHT); pump(10); }
        char shot[MAX_PATH];
        snprintf(shot, sizeof shot, "%s", argv[2]);
        if (char* dot = strrchr(shot, '.')) snprintf(dot, (size_t)(shot + sizeof shot - dot), "-enemies.bmp");
        Screenshot(shot, width, height);
        printf("%s overlay: the enemies view's screenshot (%s)\n", failures ? "FAIL" : "PASS", shot);
        return failures ? 1 : 0;
    }
    /* the change: applied through set_live, then saved into Local Stats (a second after it, from a frame) */
    if (catalogued)
    {
        bool saved = false;
        for (int wait = 0; wait < 60 && !saved; wait++)
        {
            chain->Present(1, 0);
            FILE* file = fopen(stats, "rb");
            char text[4096] = {};
            if (file) { fread(text, 1, sizeof text - 1, file); fclose(file); saved = strstr(text, ": 1200") != NULL; }
            if (!saved) Sleep(50);
        }
        printf("  applied: set_live %d time(s), last %.0f; Local Stats %s\n", g_lives, g_lastLive, saved ? "holds 1200" : "does not hold it");
        if (g_lives < 1 || g_lastLive != 1200) { puts("FAIL the change did not reach set_live"); failures++; }
        if (!saved) { puts("FAIL Local Stats does not hold the change"); failures++; }
        /* Del stages the field's reset (nothing written yet), F9 applies it: the base back, and the field out of Local Stats */
        auto frames = [&](int n) {
            for (int i = 0; i < n; i++)
            {
                MSG message;
                while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                chain->Present(1, 0);
            }
        };
        /* the MOD SYNC prompt takes answers a second after it appears or changes state: frames for that long, whatever the
         * display's rate */
        auto armed = [&]() { ULONGLONG from = GetTickCount64(); while (GetTickCount64() - from < 1200) frames(1); };
        Key(window, VK_DELETE);
        frames(10);
        int staged = g_resets;
        Key(window, VK_F9);
        frames(10);
        bool gone = false;
        for (int wait = 0; wait < 60 && !gone; wait++)
        {
            frames(1);
            gone = GetFileAttributesA(stats) == INVALID_FILE_ATTRIBUTES;
            if (!gone) Sleep(50);
        }
        printf("  reset: staged without writing %s, applied %d reset(s); Local Stats %s\n", staged == 0 ? "yes" : "NO", g_resets,
            gone ? "no longer holds it (the mod is gone)" : "still holds it");
        if (staged != 0 || g_resets < 1) { puts("FAIL the reset was not staged, then applied"); failures++; }
        if (!gone) { puts("FAIL Local Stats still holds the reset field"); failures++; }
        /* the projectile swap (two rows up): its picker, filtered, a donor staged (the weapon's own rows locked), applied */
        Key(window, VK_UP); frames(4);
        Key(window, VK_UP); frames(4);
        Key(window, VK_RETURN); frames(10);
        char shot[MAX_PATH];
        snprintf(shot, sizeof shot, "%s", argc > 2 ? argv[2] : "");
        char* dot = strrchr(shot, '.');
        if (argc > 2 && dot) { snprintf(dot, (size_t)(shot + sizeof shot - dot), "-picker.bmp"); Screenshot(shot, width, height); }
        Type(window, "eagle");
        frames(6);
        Key(window, VK_DOWN); frames(4);
        Key(window, VK_RETURN); frames(10);
        if (argc > 2 && dot) { snprintf(dot, (size_t)(shot + sizeof shot - dot), "-swapped.bmp"); Screenshot(shot, width, height); }
        int before = g_lives;
        Key(window, VK_F9); frames(10);
        printf("  swap: set_live %d time(s), last %.0f\n", g_lives - before, g_lastLive);
        if (g_lives - before != 1 || g_lastLive == 600 || g_lastLive == 1200 || g_lastLive < 1)
        { puts("FAIL the projectile swap did not reach set_live with a donor"); failures++; }
        Key(window, VK_DELETE); frames(10);
        Key(window, VK_F9); frames(20);
        /* a preset (HD2R Editor's format) imported from the PRESETS view (F6 x4): a number, unlimited uses, no impact
         * explosion and a status staged; an enemy (no HellForge row) skipped, a status no damage applies refused */
        {
            char presets[MAX_PATH], preset[MAX_PATH];
            snprintf(presets, sizeof presets, "%s\\presets", g_folder);
            CreateDirectoryA(presets, NULL);
            snprintf(preset, sizeof preset, "%s\\test.hd2rpreset.json", presets);
            FILE* file = fopen(preset, "wb");
            fputs("{\"format\":\"hd2r-editor-preset\",\"version\":1,\"name\":\"Overlay test\",\"editor\":\"1.0.0\",\"runtime\":\"0.40.0\",\"fields\":["
                "{\"key\":\"pw|AR-11 Arbitrator|damage.durable_damage\",\"value\":41},"
                "{\"key\":\"st|Orbital Precision Strike|stratagem:orbital_precision_strike:orbital_precision_strike_stratagem:stratagem.max_uses\",\"value\":\"unlimited\"},"
                "{\"key\":\"pw|R-36 Eruptor|terminal.primary.impact.explosion\",\"value\":{\"ref\":\"none\"}},"
                "{\"key\":\"pw|R-36 Eruptor|damage.status_1_type\",\"value\":\"fire\"},"
                "{\"key\":\"pw|R-36 Eruptor|damage.status_2_type\",\"value\":\"pure_damage\"},"
                "{\"key\":\"en|Bile Titan|entity::entity.health\",\"value\":7000}]}", file);
            fclose(file);
            for (int i = 0; i < 4; i++) { Key(window, VK_F6); frames(4); }
            Key(window, VK_RETURN); frames(10);
            if (argc > 2 && dot) { snprintf(dot, (size_t)(shot + sizeof shot - dot), "-presets.bmp"); Screenshot(shot, width, height); }
            int lives = g_lives;
            Key(window, VK_F9); frames(20);
            bool saved = false;
            char text[8192] = {};
            for (int wait = 0; wait < 60 && !saved; wait++)
            {
                frames(1);
                FILE* in = fopen(stats, "rb");
                if (in) { fread(text, 1, sizeof text - 1, in); fclose(in); }
                saved = strstr(text, "\"stratagem.max_uses\": -1") && strstr(text, "\"terminal.primary.impact.explosion\": 0") &&
                    strstr(text, "\"damage.status_1_type\": 5") && strstr(text, "\"damage.durable_damage\": 41") &&
                    strstr(text, "\"entity::entity.health\": 7000"); /* an enemy's row (the HD2R Editor's Enemies pages) */
                if (!saved) Sleep(50);
            }
            printf("  preset: set_live %d time(s); Local Stats %s\n", g_lives - lives, saved ? "holds the five values" : "does not hold them");
            if (g_lives - lives != 5 || !saved) { puts("FAIL the preset's five values (an enemy's among them) were not staged and applied"); failures++; }
            Key(window, VK_F6); frames(4); /* LOGS */
            Key(window, VK_F6); frames(4); /* SETTINGS */
            Key(window, VK_F6); frames(4); /* BROWSE */
        }
        /* the stratagems: back to the categories, down to Offensive (past Secondary and Throwables), a screenshot */
        if (argc > 2)
        {
            Key(window, VK_TAB);
            frames(4);
            for (int i = 0; i < 3; i++) { Key(window, VK_DOWN); frames(4); }
            frames(20);
            if (dot) snprintf(dot, (size_t)(shot + sizeof shot - dot), "-stratagems.bmp");
            Screenshot(shot, width, height);
            /* Defensive: the mod's stratagem, last in the list (its own icon), its page with its first category open */
            Key(window, VK_DOWN); frames(4);
            Key(window, VK_TAB); frames(4);
            for (int i = 0; i < 6; i++) { Key(window, VK_NEXT); frames(4); }
            Key(window, VK_TAB); frames(4);
            Key(window, VK_RIGHT); frames(20);
            if (dot) snprintf(dot, (size_t)(shot + sizeof shot - dot), "-mod.bmp");
            Screenshot(shot, width, height);
            Key(window, VK_TAB); frames(4); /* back to the categories, at Defensive */
            /* Vehicles (past Support Weapons, Support Backpacks): the Patriot, its swapped left gun's stats in the left
             * gun's category, below Mounted Weapons */
            for (int i = 0; i < 3; i++) { Key(window, VK_DOWN); frames(4); }
            Key(window, VK_TAB); frames(4);
            Key(window, VK_TAB); frames(20);
            if (dot) snprintf(dot, (size_t)(shot + sizeof shot - dot), "-vehicle.bmp");
            Screenshot(shot, width, height);
            Key(window, VK_TAB); frames(4); /* back to the categories, at Vehicles */
            /* the Helldiver (past Defensive, Support Weapons, Support Backpacks, Vehicles, Mission and Boosters): its armor
             * perks, the armor passive's picker open */
            for (int i = 0; i < 3; i++) { Key(window, VK_DOWN); frames(4); } /* Mission, Boosters, Helldiver */
            Key(window, VK_TAB); frames(4);
            Key(window, VK_DOWN); frames(4);   /* past the Helldiver's page, to its armor perks */
            Key(window, VK_TAB); frames(4);
            Key(window, VK_RIGHT); frames(4);  /* its header open (headers start closed) */
            Key(window, VK_DOWN); frames(4);
            Key(window, VK_RETURN); frames(20);
            if (dot) snprintf(dot, (size_t)(shot + sizeof shot - dot), "-helldiver.bmp");
            Screenshot(shot, width, height);
            Key(window, VK_ESCAPE); frames(4);
        }
        /* a big preset (HF_TEST_BIG_PRESET: HD2R Editor's Black Wolfs balance, ~1900 fields) imported: thousands of pending
         * changes must not slow the editor (frames timed without vsync in PRESETS, CHANGES and BROWSE) */
        const char* big = getenv("HF_TEST_BIG_PRESET");
        if (big != NULL && big[0])
        {
            char target[MAX_PATH];
            snprintf(target, sizeof target, "%s\\presets\\big.hd2rpreset.json", g_folder);
            if (CopyFileA(big, target, FALSE))
            {
                auto timed = [&](const char* where) {
                    LARGE_INTEGER f, a, b;
                    QueryPerformanceFrequency(&f);
                    for (int i = 0; i < 5; i++) { MSG m; while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } chain->Present(0, 0); }
                    QueryPerformanceCounter(&a);
                    for (int i = 0; i < 60; i++) { MSG m; while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } chain->Present(0, 0); }
                    QueryPerformanceCounter(&b);
                    double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart / 60.0;
                    printf("  big preset: %.1f ms a frame in %s\n", ms, where);
                    if (ms > 50) { printf("FAIL the editor is slow with a big preset in %s\n", where); failures++; }
                };
                for (int i = 0; i < 4; i++) { Key(window, VK_F6); frames(4); }   /* PRESETS (big.hd2rpreset.json first) */
                Key(window, VK_HOME); frames(2);
                Key(window, VK_RETURN); frames(4);
                timed("PRESETS");
                for (int i = 0; i < 5; i++) { Key(window, VK_F6); frames(2); }   /* LOGS, SETTINGS, BROWSE, SEARCH, CHANGES */
                timed("CHANGES");
                /* fields applied at boot among them (attachments, boosters): read through get_stat, not left blank */
                printf("  get_stat: %d read(s)\n", g_stats);
                if (g_stats == 0) { puts("FAIL no field applied at boot was read through get_stat"); failures++; }
                for (int i = 0; i < 5; i++) { Key(window, VK_F6); frames(2); }   /* MODS, PRESETS, LOGS, SETTINGS, BROWSE */
                timed("BROWSE");
                /* applied, with refusals: the ERRORS chip opens the errors list; then the LOGS view */
                snprintf(g_logsFolder, sizeof g_logsFolder, "%s", mods);
                char logPath[MAX_PATH];
                snprintf(logPath, sizeof logPath, "%s\\modloader.log", mods);
                FILE* log = fopen(logPath, "wb");
                fputs("10:00:00.000 HellForge mod loader, SDK 6: 9 code mod(s)\n10:00:01.000 conflict: a and b both change X\n"
                      "10:00:02.000 stats x: refused: the record is not as reviewed\n10:00:03.000 entity data ready\n", log);
                fclose(log);
                g_refuseCapacity = 1;
                Key(window, VK_F9); frames(20);
                g_refuseCapacity = 0;
                float unit = (float)height / 1080.0f, ox = ((float)width / unit - 1260) / 2, oy = (1080 - 930) / 2.0f;
                LPARAM at = MAKELPARAM((int)((ox + 1260 - 62 - 50) * unit), (int)((oy + 29) * unit));
                SendMessageW(window, WM_MOUSEMOVE, 0, at); frames(3);
                SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, at); frames(2);
                SendMessageW(window, WM_LBUTTONUP, 0, at); frames(10);
                if (dot) { snprintf(dot, (size_t)(shot + sizeof shot - dot), "-errors.bmp"); Screenshot(shot, width, height); }
                Key(window, VK_ESCAPE); frames(4);
                for (int i = 0; i < 5; i++) { Key(window, VK_F6); frames(3); } /* SEARCH, CHANGES, MODS, PRESETS, LOGS */
                frames(10);
                if (dot) { snprintf(dot, (size_t)(shot + sizeof shot - dot), "-logs.bmp"); Screenshot(shot, width, height); }
                /* joined to a friend's lobby: the editor is greyed and read-only (a preset import or APPLY changes nothing), and
                 * the mod stratagem's cooldown is the host's (host-mod-stats.json, which the session's backend writes) */
                char hostMods[MAX_PATH];
                snprintf(hostMods, sizeof hostMods, "%s\\host-mod-stats.json", g_folder);
                if (FILE* file = fopen(hostMods, "wb"))
                {
                    fputs("{\"format\":1,\"mod:test/mod/hmg\":{\"test/mod/hmg\":{\"stratagem.cooldown\":90}}}", file);
                    fclose(file);
                }
                g_sessionNow.role = HF_SESSION_JOINER; g_sessionNow.version = 1; snprintf(g_sessionNow.host, sizeof g_sessionNow.host, "Friend Host");
                for (int i = 0; i < 2; i++) { Key(window, VK_F6); frames(3); } /* SETTINGS, BROWSE */
                frames(10);
                if (dot) { snprintf(dot, (size_t)(shot + sizeof shot - dot), "-readonly.bmp"); Screenshot(shot, width, height); }
                for (int i = 0; i < 4; i++) { Key(window, VK_F6); frames(3); } /* SEARCH, CHANGES, MODS, PRESETS */
                int imports = g_presetImports, writes = g_lives;
                Key(window, VK_HOME); frames(2); Key(window, VK_RETURN); frames(6); Key(window, VK_F9); frames(6);
                printf("  read-only: %d import(s), %d write(s) while joined\n", g_presetImports - imports, g_lives - writes);
                if (g_presetImports != imports || g_lives != writes) { puts("FAIL a joined editor changed something"); failures++; }
                double joined = g_modCooldown;
                g_sessionNow.role = HF_SESSION_SOLO; g_sessionNow.version = 2; g_sessionNow.host[0] = 0;
                DeleteFileA(hostMods);
                frames(30);
                printf("  co-op mod values: the mod stratagem's cooldown %.0f while joined (the host's 90), %.0f after (its own 150)\n", joined, g_modCooldown);
                if (joined != 90 || g_modCooldown != 150) { puts("FAIL the mod content values did not follow the co-op role"); failures++; }
                /* joined to a host who lets every player keep their own stats (SDK 15 stats_source, the default): the editor
                 * stays editable (a preset import stages), and a host's file left beside it is not applied */
                if (FILE* file = fopen(hostMods, "wb"))
                {
                    fputs("{\"format\":1,\"mod:test/mod/hmg\":{\"test/mod/hmg\":{\"stratagem.cooldown\":90}}}", file);
                    fclose(file);
                }
                g_statsNow = HF_STATS_OWN;
                g_sessionNow.role = HF_SESSION_JOINER; g_sessionNow.version = 3; snprintf(g_sessionNow.host, sizeof g_sessionNow.host, "Friend Host");
                frames(30);
                imports = g_presetImports;
                Key(window, VK_HOME); frames(2); Key(window, VK_RETURN); frames(6);
                printf("  own stats while joined: %d import(s), the mod stratagem's cooldown %.0f (its own 150)\n", g_presetImports - imports, g_modCooldown);
                if (g_presetImports == imports) { puts("FAIL a joined editor keeping its own stats refused an import"); failures++; }
                if (g_modCooldown != 150) { puts("FAIL a joined editor keeping its own stats took the host's values"); failures++; }
                Key(window, VK_ESCAPE); frames(2);
                g_statsNow = HF_STATS_HOST;
                g_sessionNow.role = HF_SESSION_SOLO; g_sessionNow.version = 4; g_sessionNow.host[0] = 0;
                DeleteFileA(hostMods);
                frames(10);
            }
        }
        /* mod sync (SDK 14): the offer is read at most once every 30 frames; one shows its dialog alone over the game, the
         * editor window staying closed. Esc answers no; Enter downloads, and once ready (the prompt back by itself after
         * Esc hid the download) restarts; B blocks the selected mod; a blocked lobby's OK answers no. The keys reaching
         * the prompt show it holds the game's input. */
        for (int i = 0; i < 3; i++) { Key(window, VK_ESCAPE); frames(4); } /* the window closed, whatever was open in it */
        int reads = g_syncReads, editorOpens = g_editorOpens;
        frames(90);
        int idleReads = g_syncReads - reads;
        g_syncList[0] = {sizeof(HfSyncMod), HF_SYNC_MOD_DOWNLOAD, HF_SYNC_CLASS_CONTENT, 0, 3u << 20, "friend.tanks", "Tanks", "2.0"};
        g_syncList[1] = {sizeof(HfSyncMod), HF_SYNC_MOD_MANUAL, HF_SYNC_CLASS_CODE, 0, 200000, "friend.guns", "Guns", "1.1"};
        g_syncList[2] = {sizeof(HfSyncMod), HF_SYNC_MOD_OFF, HF_SYNC_CLASS_CONTENT, 0, 0, "mine.extra", "My Extra Mod", "1.0"};
        /* the refused join's notice comes first, as in game; the dialog takes its place (the screenshot shows the dialog alone) */
        g_noticeNow.version++;
        g_noticeNow.kind = HF_NOTICE_ERROR;
        snprintf(g_noticeNow.title, sizeof g_noticeNow.title, "COULDN'T JOIN FRIEND HOST");
        snprintf(g_noticeNow.text, sizeof g_noticeNow.text, "Your gameplay mods don't match the host's.\nMissing: Tanks 2.0 (the host has it)");
        frames(20);
        SyncOffer(HF_SYNC_OFFER, 1, 3u << 20, 0);
        frames(40);
        if (argc > 2 && dot) { snprintf(dot, (size_t)(shot + sizeof shot - dot), "-sync.bmp"); Screenshot(shot, width, height); }
        Key(window, VK_ESCAPE); frames(4);
        bool declined = g_syncAnswers[HF_SYNC_DECLINE] == 1;
        g_syncNow.state = HF_SYNC_NONE; frames(40);
        SyncOffer(HF_SYNC_OFFER, 2, 3u << 20, 0); armed();
        Key(window, VK_RETURN); frames(4);
        bool accepted = g_syncAnswers[HF_SYNC_ACCEPT] == 1;
        SyncOffer(HF_SYNC_DOWNLOADING, 2, 3u << 20, 1u << 20); frames(40);
        if (argc > 2 && dot) { snprintf(dot, (size_t)(shot + sizeof shot - dot), "-sync-progress.bmp"); Screenshot(shot, width, height); }
        Key(window, VK_ESCAPE); frames(4); /* hides the window: the download goes on */
        bool kept = g_syncAnswers[HF_SYNC_DECLINE] == 1;
        SyncOffer(HF_SYNC_READY, 2, 3u << 20, 3u << 20); armed();
        Key(window, VK_RETURN); frames(4);
        bool restarted = g_syncAnswers[HF_SYNC_RESTART] == 1;
        SyncOffer(HF_SYNC_OFFER, 3, 3u << 20, 0); armed();
        Key(window, 'B'); frames(4);
        bool blocked = g_syncAnswers[HF_SYNC_BLOCK] == 1 && !strcmp(g_syncBlocked, "friend.tanks");
        SyncOffer(HF_SYNC_BLOCKED, 4, 0, 0); armed();
        Key(window, VK_RETURN); frames(4);
        bool acknowledged = g_syncAnswers[HF_SYNC_DECLINE] == 2;
        g_syncNow.state = HF_SYNC_NONE; frames(40);
        printf("  mod sync: %d read(s) in 90 idle frames, %d dialog(s) opened; Esc no %s, Enter download %s, Esc while downloading kept it %s, "
            "ready's Enter restart %s, B block %s (%s), blocked's OK %s\n", idleReads, g_syncOpens, declined ? "yes" : "NO", accepted ? "yes" : "NO",
            kept ? "yes" : "NO", restarted ? "yes" : "NO", blocked ? "yes" : "NO", g_syncBlocked, acknowledged ? "yes" : "NO");
        if (idleReads > 3) { puts("FAIL the editor read the mod sync state more than once every 30 frames"); failures++; }
        if (g_syncOpens != 5 || !declined || !accepted || !kept || !restarted || !blocked || !acknowledged)
        { puts("FAIL the mod sync dialog did not open or answer as expected"); failures++; }
        printf("  mod sync prompt: shown alone %d time(s), gone %d, the editor window opened %d time(s)\n", g_syncAlone, g_syncGone,
            g_editorOpens - editorOpens);
        /* offer, offer (on through the download until Esc), ready, blocked; v3's offer came while ready's dialog showed */
        if (g_syncAlone != 4 || g_syncGone != 4 || g_editorOpens != editorOpens)
        { puts("FAIL the mod sync prompt did not show alone over the game, or the editor window opened behind it"); failures++; }
        /* TRUST and GET IT: a Steam friend's code mod HellForge has not verified waits for the player's trust. T shows the
         * warning in the prompt's place; neither a click before it arms nor Enter trusts, Esc goes back to the list; G opens
         * the selected mod's page (the backend does: the editor only names the mod); a click on TRUST AND DOWNLOAD a second
         * after the warning appears trusts that mod. The warning is 420 units high, centred: its button's right edge is 186
         * units in from the right of its 760, its middle 51 up from the bottom. */
        g_syncList[0] = {sizeof(HfSyncMod), HF_SYNC_MOD_TRUST, HF_SYNC_CLASS_CODE, 0, 200000, "friend.guns", "Guns", "1.1", "https://example.com/guns"};
        g_syncList[1] = {sizeof(HfSyncMod), HF_SYNC_MOD_MANUAL, HF_SYNC_CLASS_RAW, 0, 0, "other.textures", "Textures", "3", "https://example.com/textures"};
        g_syncList[2] = {sizeof(HfSyncMod), HF_SYNC_MOD_OFF, HF_SYNC_CLASS_CONTENT, 0, 0, "mine.extra", "My Extra Mod", "1.0", ""};
        int warnings = g_syncWarnings, declines = g_syncAnswers[HF_SYNC_DECLINE];
        SyncOffer(HF_SYNC_BLOCKED, 6, 0, 0); armed();
        if (argc > 2 && dot) { snprintf(dot, (size_t)(shot + sizeof shot - dot), "-sync-trust.bmp"); Screenshot(shot, width, height); }
        Key(window, 'T'); frames(4);
        bool warned = g_syncWarnings == warnings + 1;
        float unit = (float)height / 1080.0f, ox = (float)(int)(((float)width / unit - 1260) / 2 + 0.5f), oy = (1080 - 930) / 2.0f;
        LPARAM trustAt = MAKELPARAM((int)((ox + 250 + 760 - 206) * unit), (int)((oy + 255 + 420 - 51) * unit));
        auto click = [&](LPARAM at) {
            SendMessageW(window, WM_MOUSEMOVE, 0, at); frames(3);
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, at); frames(2);
            SendMessageW(window, WM_LBUTTONUP, 0, at); frames(4);
        };
        click(trustAt);
        Key(window, VK_RETURN); frames(4);
        bool unarmed = g_syncAnswers[HF_SYNC_TRUST] == 0;
        armed();
        if (argc > 2 && dot) { snprintf(dot, (size_t)(shot + sizeof shot - dot), "-sync-warning.bmp"); Screenshot(shot, width, height); }
        Key(window, VK_RETURN); frames(4);
        bool noEnter = g_syncAnswers[HF_SYNC_TRUST] == 0;
        Key(window, VK_ESCAPE); frames(4);
        bool back = g_syncAnswers[HF_SYNC_DECLINE] == declines && g_syncAnswers[HF_SYNC_TRUST] == 0;
        Key(window, 'G'); frames(4);
        bool opened = g_syncAnswers[HF_SYNC_OPEN] == 1 && !strcmp(g_syncOpened, "friend.guns");
        Key(window, 'T'); frames(4); armed();
        click(trustAt);
        bool trusted = g_syncAnswers[HF_SYNC_TRUST] == 1 && !strcmp(g_syncTrusted, "friend.guns") && g_syncWarnings == warnings + 2;
        g_syncNow.state = HF_SYNC_NONE; frames(40);
        printf("  mod sync trust: T warns %s, an early click and Enter do not trust %s/%s, Esc back to the list %s, G opens %s (%s), "
            "a click after a second trusts %s (%s)\n", warned ? "yes" : "NO", unarmed ? "yes" : "NO", noEnter ? "yes" : "NO", back ? "yes" : "NO",
            opened ? "yes" : "NO", g_syncOpened, trusted ? "yes" : "NO", g_syncTrusted);
        if (!warned || !unarmed || !noEnter || !back || !opened || !trusted) { puts("FAIL the mod sync TRUST warning or GET IT did not answer as expected"); failures++; }
    }
    HRESULT removed = d3d11 ? device11->GetDeviceRemovedReason() : device->GetDeviceRemovedReason();
    if (removed != S_OK) { printf("FAIL device removed 0x%08lx\n", (unsigned long)removed); failures++; }
    if (!g_hooked || !g_drawing) { puts("FAIL the editor did not hook or start drawing"); failures++; }
    /* in front of the overlay again, and the overlay still called (after the editor); the method table every swap chain
     * shares never patched (Steam's overlay hooks whatever it holds, and a table pointing at the editor made it recurse) */
    printf("  overlay: the editor got in front of it %d time(s); its detour ran %d time(s)\n", g_again, g_overlayPresents);
    /* (with a real RivaTuner the test's overlay is left out: RivaTuner's own hooks over the editor's are the ones; should
     * it have hooked before the editor this time, there was nothing to get in front of, and the screenshots still tell) */
    if (rivaTuner && g_again < 1) puts("  overlay: NOTE RivaTuner did not hook over the editor this time");
    if (!rivaTuner && (g_again < 1 || g_overlayPresents < 50)) { puts("FAIL the editor did not get in front of the overlay that hooked over it, or cut it out"); failures++; }
    printf("  overlay: the editor put its own hook back %d time(s)\n", g_putBack);
    if (!rivaTuner && g_putBack < 1) { puts("FAIL the editor did not put its hook back when the code was put back as it was"); failures++; }
    if ((*(void***)chain)[8] != presentCode || (*(void***)chain)[22] != present1Code) { puts("FAIL the swap chains' shared method table was patched"); failures++; }
    if (!d3d11)
    {
        printf("  queues: the swap chain's %p, the overlay's %p; the editor draws on %p\n", (void*)queue, (void*)overlayQueue, g_drawnQueue);
        if (g_drawnQueue != (void*)queue) { puts("FAIL the editor draws on another queue than the swap chain's (the overlay's?)"); failures++; }
    }
    printf("%s overlay: hooked %d, drawing %d, 420 frames, two resizes, F8 open/close/open, device %s\n", failures ? "FAIL" : "PASS",
        g_hooked, g_drawing, removed == S_OK ? "healthy" : "removed");
    return failures ? 1 : 0;
}
