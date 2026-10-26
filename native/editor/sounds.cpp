/* The game's menu sounds (sounds.h). game.dll +0x1327F50 is the function the loadout screen posts its sounds with
 * (HD2Runtime's research, runtime-stratagem-ui-F5FEE03DCFDB.json uiSound; this game.dll, SHA-256 2E2C3B7C...): it takes
 * the Wwise plugin's API table and the game context from two globals, asks the API (+0x288) for the object of the
 * context's Game World WwiseWorld (+0x10F8), remaps its key to an event id and posts the event there (+0x338). The editor
 * does the same with an event id of its own choosing, after checking every byte of that function, so another build is
 * silent rather than wrong (the WwiseWorld's default source, +0x4148, is that same object). The events are the HD2R
 * Editor's: generic select on a click, slot select when something opens, picker close on back and on an error,
 * stratagem pick on apply. A sound whose bank is not loaded is not played (the engine ignores it).
 * The thread: the game makes its sound engine calls on its game thread, not the window's (a probe of 2026-10-17: states
 * set on one thread, the window's messages on another), and sounds posted from the window's thread were heard only now
 * and then. So the editor's sounds are posted on the game thread, from HellForge's on_game_thread callback (the loader
 * hooks the menu sound manager's update, game.dll +0x13270B0, every frame on the game thread), or with an older loader
 * from that update hooked here after its first bytes are checked: the queued sounds first, then the game's update.
 * Without either they go from the window's thread as before.
 * The menu effect: what a menu does to the mix, which the editor does too while it is open. A game object (game.dll
 * +0x347CE28) holds its level (+0x4210, 0 to 1) and rate (+0x4214); the update moves the level each frame and sets the
 * global Wwise parameter menu_effect = level x 75. Every menu screen, when it opens, sets the rate to (1 - level) x 2
 * (inlined; game.dll +0x1328070 is the same as a function: open, or rate = level x -2 to close). The editor raises it
 * when it opens and before each sound (also setting menu_effect itself through the API table, +0xA0, after the same
 * readiness check, +0x10), and lowers it when it closes, unless a game menu had it up already. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "sounds.h"

namespace sounds
{
namespace
{
const uintptr_t FUNCTION = 0x1327F50;
/* the function as reviewed: from its first byte to its tail jump */
const unsigned char REVIEWED[] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x05, 0xb2, 0xe3, 0xff, 0x01,
    0x8b, 0xda, 0x48, 0x8b, 0x0d, 0xd1, 0xe3, 0xff, 0x01, 0x48, 0x8b, 0x90, 0x88, 0x02, 0x00, 0x00, 0x48, 0x8b, 0x89, 0xf8, 0x10, 0x00,
    0x00, 0x48, 0x8b, 0xb0, 0x38, 0x03, 0x00, 0x00, 0xff, 0xd2, 0x8b, 0xcb, 0x48, 0x8b, 0xf8, 0xe8, 0x50, 0xd7, 0xf2, 0xff, 0x48, 0x8b,
    0x0d, 0xa9, 0xe3, 0xff, 0x01, 0x45, 0x33, 0xc9, 0x4c, 0x8b, 0xc7, 0x8b, 0xd0, 0x48, 0x8b, 0x89, 0xf8, 0x10, 0x00, 0x00, 0x48, 0x8b,
    0xc6, 0x48, 0x8b, 0x5c, 0x24, 0x30, 0x48, 0x8b, 0x74, 0x24, 0x38, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0x48, 0xff, 0xe0};
const uint32_t API_OBJECT = 0x288, API_POST = 0x338, CONTEXT_WWISE_WORLD = 0x10F8, WORLD_DEFAULT_SOURCE = 0x4148;
/* the menu effect switch as reviewed (game.dll +0x1328070, to its last ret) */
const uintptr_t MENU_SWITCH = 0x1328070;
const unsigned char MENU_REVIEWED[] = {
    0x48, 0x8b, 0x05, 0xb1, 0x4d, 0x15, 0x02, 0xf3, 0x0f, 0x10, 0x88, 0x10, 0x42, 0x00, 0x00, 0x84, 0xd2, 0x74, 0x19, 0xf3, 0x0f, 0x10,
    0x05, 0xe5, 0xec, 0x09, 0x01, 0xf3, 0x0f, 0x5c, 0xc1, 0xf3, 0x0f, 0x58, 0xc0, 0xf3, 0x0f, 0x11, 0x80, 0x14, 0x42, 0x00, 0x00, 0xc3,
    0xf3, 0x0f, 0x59, 0x0d, 0xf4, 0xff, 0x09, 0x01, 0xf3, 0x0f, 0x11, 0x88, 0x14, 0x42, 0x00, 0x00, 0xc3};
const uint32_t MENU_LEVEL = 0x4210, MENU_RATE = 0x4214, API_READY = 0x10, API_SET_GLOBAL = 0xA0;
/* the menu sound manager's update, as reviewed (its first 32 bytes) */
const uintptr_t MENU_UPDATE = 0x13270B0;
const unsigned char UPDATE_START[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0x83, 0xb9, 0x58, 0x46, 0x00, 0x00, 0x00, 0x48, 0x8b,
    0xd9, 0x0f, 0x29, 0x7c, 0x24, 0x20, 0x0f, 0x28, 0xf9, 0x0f, 0x84, 0x0d, 0x02, 0x00, 0x00, 0x48};
typedef void (*UpdateFn)(void* manager, float dt);
UpdateFn g_update;
volatile LONG g_onGameThread; /* the update hook runs: Flush is called from there */
const float MENU_FULL = 75.0f;
typedef bool (*ReadyFn)(void);
typedef void (*SetGlobalFn)(const char* name, float value);

typedef void* (*ObjectFn)(void* wwiseWorld);
typedef uint64_t (*PostFn)(void* wwiseWorld, uint32_t event, void* object, uint64_t flags);

const HfApi* g_api;
HfMod* g_mod;
int g_reported; /* the first post is logged in full */
volatile LONG g_requests, g_posts, g_skipped; /* since the last Report */
uintptr_t g_menuGlobal;

volatile LONG g_menuWanted; /* 1 the editor opened, 2 it closed (done on the window's thread) */
bool g_editorOpen, g_weRaised, g_fellBack;
uintptr_t g_lastWorld, g_lastObject;
uintptr_t g_apiGlobal, g_contextGlobal;
volatile LONG g_requested, g_enabled = 1;
ULONGLONG g_last[KIND_COUNT];

/* Wwise's event id of a name: FNV-1, 32 bits, lower case */
uint32_t EventId(const char* name)
{
    uint32_t h = 0x811C9DC5u;
    for (const char* p = name; *p; p++) { h *= 0x01000193u; h ^= (uint32_t)(unsigned char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p); }
    return h;
}

uint32_t g_events[KIND_COUNT];
/* In a mission only the mission's UI bank (content/audio/ui_mission) is loaded: the loadout screen's sounds (the ship's
 * bank, content/audio/ui_ship) are not played there (the engine answers 0), and the generic ones both banks hold are
 * posted instead. */
uint32_t g_fallbacks[KIND_COUNT];

template <typename T> bool Read(uintptr_t at, T* out) { return at != 0 && g_api->read(at, out, sizeof *out) == 0; }


/* The menu effect's level and its object; false when the game has none now. */
bool MenuLevel(uintptr_t* object, float* level)
{
    return g_menuGlobal != 0 && Read(g_menuGlobal, object) && *object != 0 && Read(*object + MENU_LEVEL, level);
}

void MenuRate(uintptr_t object, float rate) { g_api->write(object + MENU_RATE, &rate, sizeof rate); }

/* menu_effect itself, as the game's update sets it */
void SetMenuEffect(float value)
{
    uintptr_t table = 0, ready = 0, set = 0;
    if (!Read(g_apiGlobal, &table) || !table || !Read(table + API_READY, &ready) || !ready || !Read(table + API_SET_GLOBAL, &set) || !set) return;
    if (((ReadyFn)ready)()) ((SetGlobalFn)set)("menu_effect", value);
}

/* Up while the editor is open: as a menu screen does when it opens (again if a game menu turned it down meanwhile). */
void RaiseMenuEffect(bool opening)
{
    uintptr_t object;
    float level, rate = 0;
    if (!MenuLevel(&object, &level)) return;
    Read(object + MENU_RATE, &rate);
    if (opening) g_weRaised = level < 0.5f && rate <= 0;
    if (opening || rate <= 0) MenuRate(object, (1.0f - level) * 2.0f);
    SetMenuEffect(MENU_FULL);
}

/* The engine's playing id, 0 when nothing played (the event's bank is not loaded). */
uint64_t Post(uint32_t event)
{
    uintptr_t table = 0, context = 0, world = 0, objectFn = 0, postFn = 0;
    bool report = g_reported < 1; /* the first post of the session, in full */
    if (!Read(g_apiGlobal, &table) || !table || !Read(g_contextGlobal, &context) || !context || !Read(context + CONTEXT_WWISE_WORLD, &world) || !world ||
        !Read(table + API_OBJECT, &objectFn) || !objectFn || !Read(table + API_POST, &postFn) || !postFn)
    {
        if (report) { g_reported++; g_api->log(g_mod, "sound %08x not posted: API table %p, context %p, WwiseWorld %p, object fn %p, post fn %p",
            event, (void*)table, (void*)context, (void*)world, (void*)objectFn, (void*)postFn); }
        return 0;
    }
    void* menuObject = ((ObjectFn)objectFn)((void*)world);
    uint32_t source = 0;
    void* object = Read(world + WORLD_DEFAULT_SOURCE, &source) && source != 0 ? (void*)(uintptr_t)source : menuObject;
    uint64_t playing = ((PostFn)postFn)((void*)world, event, object, 0);
    InterlockedIncrement(&g_posts);
    if (world != g_lastWorld || (uintptr_t)object != g_lastObject)
    {
        if (g_lastWorld != 0) g_api->log(g_mod, "sound: the game's WwiseWorld or object changed: %p, %p (was %p, %p)", (void*)world, object,
            (void*)g_lastWorld, (void*)g_lastObject);
        g_lastWorld = world;
        g_lastObject = (uintptr_t)object;
    }
    if (report)
    {
        g_reported++;
        g_api->log(g_mod, "sound %08x posted on thread %lu: WwiseWorld %p, on %p (the default source %08x; the menus' object %p), the engine "
            "answered %llu (0: not played)", event, GetCurrentThreadId(), (void*)world, object, source, menuObject, (unsigned long long)playing);
    }
    return playing;
}

/* Down again when the editor closes, unless a game menu had it up already. */
void LowerMenuEffect()
{
    g_editorOpen = false;
    uintptr_t object;
    float level;
    if (g_weRaised && MenuLevel(&object, &level))
    {
        float none = 0;
        MenuRate(object, 0);
        g_api->write(object + MENU_LEVEL, &none, sizeof none); /* no menu open: as the game has it then */
        SetMenuEffect(0);
    }
    g_weRaised = false;
}

/* A sound of this kind, unless one played less than 40 ms ago (then it is skipped). */
void PlayKind(Kind kind, ULONGLONG now)
{
    if (now - g_last[kind] < 40) { InterlockedIncrement(&g_skipped); return; }
    g_last[kind] = now;
    if (g_editorOpen) RaiseMenuEffect(false);
    if (Post(g_events[kind]) == 0 && g_fallbacks[kind] != 0 && Post(g_fallbacks[kind]) != 0 && !g_fellBack)
    {
        g_fellBack = true;
        g_api->log(g_mod, "sound: the loadout screen's sounds are not loaded here (a mission); the generic ones play instead");
    }
}

/* The menu effect switch (game.dll +0x1328070), when it is the reviewed one: where the game keeps the effect's object. */
void FindMenuEffect(const HfApi* api, HfMod* mod, uintptr_t base, size_t size)
{
    unsigned char menu[sizeof MENU_REVIEWED];
    if (size >= MENU_SWITCH + sizeof menu && !api->read(base + MENU_SWITCH, menu, sizeof menu) && !memcmp(menu, MENU_REVIEWED, sizeof menu))
    {
        int32_t disp;
        memcpy(&disp, menu + 3, 4);
        g_menuGlobal = base + MENU_SWITCH + 7 + (intptr_t)disp;
    }
    else api->log(mod, "editor: the game's menu effect switch is not the reviewed one; the menu sounds are heard only with a game menu open");
}
} // namespace

bool Flush();

void UpdateDetour(void* manager, float dt)
{
    InterlockedExchange(&g_onGameThread, 1);
    Flush();
    g_update(manager, dt);
}

void GameThread(HfMod*, float, void*)
{
    InterlockedExchange(&g_onGameThread, 1);
    Flush();
}

/* The game thread to post the sounds on: HellForge's on_game_thread, else the menu sound manager's update hooked here. */
static void FindGameThread(const HfApi* api, HfMod* mod, uintptr_t base, size_t size)
{
    unsigned char update[sizeof UPDATE_START];
    if (api->size >= offsetof(HfApi, on_game_thread) + sizeof api->on_game_thread && api->on_game_thread != nullptr)
    {
        if (api->on_game_thread(mod, GameThread, nullptr)) api->log(mod, "editor: menu sounds are posted on the game thread (on_game_thread)");
        else api->log(mod, "editor: no game thread callback; menu sounds are posted from the window's thread");
    }
    else if (size >= MENU_UPDATE + sizeof update && !api->read(base + MENU_UPDATE, update, sizeof update) && !memcmp(update, UPDATE_START, sizeof update) &&
        !api->hook(mod, base + MENU_UPDATE, (void*)UpdateDetour, (void**)&g_update))
        api->log(mod, "editor: menu sounds are posted on the game thread (the menu sound manager's update)");
    else api->log(mod, "editor: the menu sound manager's update is not the reviewed one; menu sounds are posted from the window's thread");
}

bool Init(const HfApi* api, HfMod* mod)
{
    g_api = api;
    g_mod = mod;
    size_t size = 0;
    uintptr_t base = api->module_base != nullptr && api->read != nullptr ? api->module_base("game.dll", &size) : 0;
    unsigned char code[sizeof REVIEWED];
    if (strcmp(api->build_id(), "25480438") || base == 0 || size < FUNCTION + sizeof code || api->read(base + FUNCTION, code, sizeof code))
    {
        api->log(mod, "editor: game.dll's UI sound function cannot be read (game.dll %p); the editor is silent", (void*)base);
        return false;
    }
    if (memcmp(code, REVIEWED, sizeof code))
    {
        size_t at = 0;
        while (code[at] == REVIEWED[at]) at++;
        api->log(mod, "editor: game.dll's UI sound function differs from the reviewed one at +0x%zx (%02x, not %02x); the editor is silent",
            at, code[at], REVIEWED[at]);
        return false;
    }
    int32_t apiDisp, contextDisp;
    memcpy(&apiDisp, code + 0x12, 4);
    memcpy(&contextDisp, code + 0x1B, 4);
    g_apiGlobal = base + FUNCTION + 0x16 + (intptr_t)apiDisp;
    g_contextGlobal = base + FUNCTION + 0x1F + (intptr_t)contextDisp;
    FindMenuEffect(api, mod, base, size);
    FindGameThread(api, mod, base, size);
    g_events[CLICK] = EventId("ui_generic_select");
    g_events[OPEN] = 1010367601u;  /* a loadout slot selected */
    g_events[BACK] = 230369812u;   /* the picker closing */
    g_events[APPLY] = 1787078535u; /* a stratagem picked */
    g_events[ERROR_] = 230369812u;
    g_events[SHOW] = g_events[OPEN];
    g_events[HIDE] = g_events[BACK];
    g_fallbacks[OPEN] = g_fallbacks[APPLY] = g_events[CLICK];
    g_fallbacks[BACK] = g_fallbacks[ERROR_] = EventId("ui_generic_back");
    g_fallbacks[SHOW] = EventId("ui_generic_popup_open");
    g_fallbacks[HIDE] = EventId("ui_generic_popup_close");
    return true;
}

void Request(Kind kind)
{
    if (g_apiGlobal == 0 || !g_enabled) return;
    InterlockedIncrement(&g_requests);
    InterlockedOr(&g_requested, 1 << kind);
}

bool Waiting() { return g_requested != 0 || g_menuWanted != 0; }
bool OnGameThread() { return g_onGameThread != 0; }

void EditorOpen(bool open)
{
    if (g_menuGlobal == 0) return;
    InterlockedExchange(&g_menuWanted, open ? 1 : 2);
}

bool Flush()
{
    LONG menu = InterlockedExchange(&g_menuWanted, 0);
    if (menu == 1) { g_editorOpen = true; RaiseMenuEffect(true); }
    if (menu == 2) LowerMenuEffect();
    LONG requested = InterlockedExchange(&g_requested, 0);
    if (requested == 0 || g_apiGlobal == 0) return requested != 0;
    ULONGLONG now = GetTickCount64();
    /* one sound a flush (the most telling), at most one of a kind per 40 ms */
    static const Kind ORDER[] = {HIDE, SHOW, ERROR_, APPLY, BACK, OPEN, CLICK};
    for (Kind kind : ORDER)
        if (requested & (1 << kind))
        {
            PlayKind(kind, now);
            break;
        }
    return true;
}

void SetEnabled(bool on) { InterlockedExchange(&g_enabled, on ? 1 : 0); }

void Report(const char* when)
{
    if (g_api == nullptr) return;
    g_api->log(g_mod, "sound: %s; since the last: %ld requested, %ld posted, %ld skipped; %s, %s", when, InterlockedExchange(&g_requests, 0),
        InterlockedExchange(&g_posts, 0), InterlockedExchange(&g_skipped, 0), g_apiGlobal ? "checked" : "not checked", g_enabled ? "on" : "off");
}
} // namespace sounds
