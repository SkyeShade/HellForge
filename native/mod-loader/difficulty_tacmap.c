/* The tactical map screen on larger maps (M; KB xl-tactical-map-spec): the loader's half. Included by
 * hellforge_modloader.c after event_polls.c and difficulty_world_terrain.c.
 *
 * The game thread (TacTick) reads the map's sources into a snapshot while the screen is open and sends this machine's
 * waypoint through the game's own ping; the overlay host's render thread draws from a copy of the snapshot
 * (tacmap_draw.c); a worker turns the level's heights into the map pictures (tacmap_terrain.c). Every read follows KB
 * xl-tactical-map-data's reader recipe: the component arrays move when they grow, so each is read again every frame,
 * and an index is good for that frame only. */
#include "tacmap.h"
#include "../overlay/hellforge_overlay_host.h"
#include "tacmap_terrain.c"
#include "tacmap_draw.c"

#define TAC_LEVELS 0x3326340u      /* [game+..] + 0xAE288 = the level lg */
#define TAC_OBJECTIVES 0x3326DA0u
#define TAC_SHOW_ON_MAP 0x3326590u
#define TAC_GUARD_FORCE 0x33265C0u
#define TAC_EXTRACTION 0x33266F0u
#define TAC_AVATARS 0x3326D20u     /* M: active +0x6C, owned +0x70, descriptors M + 0x110 + 8i, records M + 0x547830 + 0x78i */
#define TAC_NAMES 0x347CED8u       /* [..] + 0xC0 j, j < 8: peer +0, name +0x08 */
#define TAC_MISSION_MODE 0x33266A0u /* [[..] + 0x40] == 4: no territory, as the game's map */
#define TAC_GUARD_TYPES 0x32FCDE0u /* rows of 32 bytes; a row whose +0xC is set is never drawn */
#define TAC_MAP 0x346D538u         /* [..] + 0x3EF168 = the HUD map MapPing takes */
#define TAC_PING 0x18B97A0u        /* void MapPing(MapHud*, uint32_t avatar slot, const float pos[3]); game thread only */

typedef struct TacJob { int pending; TacPictureRequest request; LONG level; } TacJob;

static struct
{
    SRWLOCK lock;              /* the published snapshot */
    TacSnapshot shared;
    TacSnapshot draw;          /* the render thread's copy */
    TacSnapshot work;          /* the game thread's */
    volatile LONG open;        /* the screen is open: the game thread keeps the snapshot fresh */
    TacScreen* screen;
    int screenId;
    TacHeights heights;        /* g_near.global, for this level */
    volatile LONG heightsLevel;
    LONG noted;                /* this opening is in the log already */
    /* the waypoint asked for on the render thread, sent on the game thread */
    SRWLOCK pingLock;
    int pingPending, pingRemove;
    float pingAt[2];
    int pingProven;            /* -1 refused, 1 the function reads as reviewed */
    /* pictures: one request at a time on the worker */
    HANDLE wake;
    SRWLOCK jobLock;
    TacJob job;
} g_tac;

static float TacF32(uintptr_t at, float fallback) { float v; return SafeRead(at, &v, 4) || !isfinite(v) ? fallback : v; }
static uint8_t TacU8(uintptr_t at) { uint8_t v = 0; SafeRead(at, &v, 1); return v; }

/* The level, its size and the playable radius; 0 while no level is ready. */
static uintptr_t TacLevel(uintptr_t game, float* size, float* playable)
{
    uintptr_t world, lg;
    if (!EvpPtr(game + TAC_LEVELS, &world) || EvpU32(world + 0xAC21C, 0) != 4 || !EvpPtr(world + 0xAE288, &lg) || EvpU32(lg + 0x1E4, 0) != 3) return 0; /* in a mission, its level ready */
    *size = TacF32(lg + 0x1E0, 0.0f);
    float frac = TacF32(lg + 0x2D70, 0.0f);
    *playable = frac > 0.0f && frac <= 1.0f ? frac * *size * 0.5f : 0.0f;
    return *size > 0.0f ? lg : 0;
}

static void TacMarkAdd(TacSnapshot* s, float x, float y, int kind, int done, int unknown)
{
    if (s->marks < TAC_MARKS && isfinite(x) && isfinite(y)) s->mark[s->marks++] = (TacMark){x, y, (uint8_t)kind, (uint8_t)done, (uint8_t)unknown};
}

static void TacObjectives(uintptr_t game, TacSnapshot* s)
{
    uintptr_t o, synced, local;
    if (!EvpPtr(game + TAC_OBJECTIVES, &o) || !EvpPtr(o + 0x68, &synced) || !EvpPtr(o + 0x60, &local)) return;
    uint32_t count = EvpU32(o + 0x24, 0), cap = EvpU32(o + 0x18, 0);
    for (uint32_t i = 0; i < count && i < cap && i < 256; i++)
    {
        float p[3];
        if (SafeRead(synced + 0x64 * (uintptr_t)i, p, 12)) break;
        uint32_t state = EvpU32(local + 0x1078 * (uintptr_t)i + 0x1018, 0);
        TacMarkAdd(s, p[0], p[1], TAC_OBJECTIVE, state == 2 || state == 3, EvpU32(synced + 0x64 * (uintptr_t)i + 0x44, 0) == 1);
    }
}

static void TacPointsOfInterest(uintptr_t game, TacSnapshot* s)
{
    uintptr_t m, records, synced;
    if (!EvpPtr(game + TAC_SHOW_ON_MAP, &m) || !EvpPtr(m + 0x48, &records) || !EvpPtr(m + 0x50, &synced)) return;
    uint32_t count = EvpU32(m + 0xC, 0), cap = EvpU32(m + 0x0, 0);
    for (uint32_t i = 0; i < count && i < cap && i < 512; i++)
    {
        float p[3];
        uint8_t seen = TacU8(synced + 24 * (uintptr_t)i), shown = TacU8(synced + 24 * (uintptr_t)i + 1);
        if ((!seen && !shown) || TacU8(synced + 24 * (uintptr_t)i + 0x15) || SafeRead(records + 20 * (uintptr_t)i, p, 12)) continue;
        TacMarkAdd(s, p[0], p[1], TAC_POI, 0, !seen);
    }
}

static void TacGuardForces(uintptr_t game, TacSnapshot* s)
{
    uintptr_t gf, records, synced, mode;
    if (!EvpPtr(game + TAC_GUARD_FORCE, &gf) || !EvpPtr(gf + 0x48, &records) || !EvpPtr(gf + 0x50, &synced)) return;
    int territory = !(EvpPtr(game + TAC_MISSION_MODE, &mode) && EvpU32(mode + 0x40, 0) == 4);
    uint32_t count = EvpU32(gf + 0xC, 0), cap = EvpU32(gf + 0x0, 0);
    for (uint32_t i = 0; i < count && i < cap && i < 1024; i++)
    {
        uintptr_t r = records + 0x2B8 * (uintptr_t)i;
        float p[4];
        if (SafeRead(r, p, 16)) break;
        if (TacU8(game + TAC_GUARD_TYPES + 32 * (uintptr_t)TacU8(r + 0x2A0) + 0xC) != 0) continue; /* a type the map never draws */
        float cleared = TacF32(r + 0x2AC, 0.0f);
        if (territory && cleared < 1.0f && p[3] > 0.0f && s->zones < TAC_ZONES)
            s->zone[s->zones++] = (TacZone){p[0], p[1], p[3] * 1.35f, cleared}; /* the game's radius wobbles 1 .. 1.7 r */
        if (TacU8(r + 0x2AB)) /* an outpost: a marker, once found */
            TacMarkAdd(s, p[0], p[1], TAC_ENEMY, cleared >= 1.0f, !TacU8(synced + 0x40 * (uintptr_t)i + 0x3B));
    }
}

static void TacExtraction(uintptr_t game, TacSnapshot* s)
{
    uintptr_t e, synced;
    if (!EvpPtr(game + TAC_EXTRACTION, &e) || !EvpPtr(e + 0x50, &synced)) return;
    uint32_t count = EvpU32(e + 0x10, 0), cap = EvpU32(e + 0x4, 0);
    for (uint32_t i = 0; i < count && i < cap && i < 16; i++)
    {
        float p[3];
        if (SafeRead(synced + 20 * (uintptr_t)i + 4, p, 12)) break;
        TacMarkAdd(s, p[0], p[1], TAC_EXTRACT, 0, TacU8(synced + 20 * (uintptr_t)i) == 0);
    }
}

/* The name a peer plays under (the game's name table), "" when it is not there. */
static void TacName(uintptr_t game, uint64_t peer, char* out, size_t room)
{
    uintptr_t table;
    out[0] = 0;
    if (peer == 0 || !EvpPtr(game + TAC_NAMES, &table)) return;
    for (int j = 0; j < 8; j++)
    {
        uint64_t key = 0;
        if (SafeRead(table + 0xC0 * (uintptr_t)j, &key, 8) || key != peer) continue;
        char name[0x81] = {0};
        SafeRead(table + 0xC0 * (uintptr_t)j + 8, name, sizeof name - 1);
        snprintf(out, room, "%s", name);
        return;
    }
}

/* An avatar's heading, life and waypoint, from its unit, its health record and its record in the manager m. */
static void TacAvatarState(uintptr_t game, uintptr_t m, uint32_t i, uint32_t entity, uint32_t unit, TacPlayer* p)
{
    /* heading: the body's forward, the pose's y row (KB xl-tactical-map-data, conventions) */
    uintptr_t graph = EvpUnitGraph(unit), poses;
    float f[3];
    if (graph && EvpPtr(graph + 0x28, &poses) && !SafeRead(poses + 0x10, f, 12) && isfinite(f[0]) && isfinite(f[1]))
        p->yaw = atan2f(f[0], f[1]) * 57.2958f;
    uintptr_t record; int32_t health, max; uint32_t life = 0;
    if (EvpHealthOf(game, entity, &record, &health, &life, &max)) p->life = (uint8_t)(life >= 2 ? 2 : life);
    uintptr_t w = m + 0x547830 + 0x78 * (uintptr_t)i;
    float wp[3];
    if ((EvpU32(w, 0) & 4) && !SafeRead(w + 0x50, wp, 12)) { p->waypoint = 1; p->wx = wp[0]; p->wy = wp[1]; }
}

/* Avatar i of the manager m as the snapshot's next player; NULL when the slot has no avatar with a position. */
static TacPlayer* TacAvatar(uintptr_t game, uintptr_t m, uint32_t i, const EvpPlayer* players, int known, TacSnapshot* s)
{
    uintptr_t descriptor;
    if (!EvpPtr(m + 0x110 + 8 * (uintptr_t)i, &descriptor)) return NULL;
    uint32_t entity = EvpU32(descriptor + 8, 0), unit = EvpU32(descriptor + 0xC, 0);
    float pos[3];
    if (!entity || !EvpUnitPosition(unit, pos)) return NULL;
    TacPlayer* p = &s->player[s->players];
    memset(p, 0, sizeof *p);
    p->present = 1;
    p->x = pos[0]; p->y = pos[1];
    TacAvatarState(game, m, i, entity, unit, p);
    for (int k = 0; k < known; k++)
        if (players[k].avatar == entity)
        {
            p->local = (uint8_t)players[k].local;
            TacName(game, players[k].peer, p->name, sizeof p->name);
        }
    if (!p->name[0]) snprintf(p->name, sizeof p->name, "DIVER %u", s->players + 1);
    s->players++;
    return p;
}

/* The squad from the avatar manager (8 avatars; the player list holds only 4): position, heading, life and waypoint
 * per avatar; the name and "local" through the player list where it has the avatar. Returns the local avatar's slot in
 * the manager when this machine owns it, else -1 (MapPing takes it). */
static int TacSquad(uintptr_t game, TacSnapshot* s)
{
    uintptr_t m;
    EvpPlayer players[EVT_MAX_PLAYERS];
    int known = EvpPlayers(game, players), localSlot = -1;
    if (!EvpPtr(game + TAC_AVATARS, &m)) return -1;
    uint32_t active = EvpU32(m + 0x6C, 0), owned = EvpU32(m + 0x70, 0);
    for (uint32_t i = 0; i < active && i < TAC_PLAYERS; i++)
    {
        const TacPlayer* p = TacAvatar(game, m, i, players, known, s);
        if (p != NULL && p->local && i < owned) localSlot = (int)i;
    }
    return localSlot;
}

/* The level's heights, once copied (g_near.global, the living world's height plan). */
static void TacTickHeights(void)
{
    if (g_tac.heightsLevel != g_world.levels && g_near.global != NULL && g_near.level == g_world.levels && g_near.columns >= 2)
    {
        /* NearGlobalAt's cells are 16384 / columns wide; their centres span 16384 (columns - 1) / columns */
        g_tac.heights = (TacHeights){g_near.global, g_near.columns, g_near.rows, 16384.0f * (float)(g_near.columns - 1) / (float)g_near.columns, 0, 0};
        TacHeightRange(&g_tac.heights);
        InterlockedExchange(&g_tac.heightsLevel, g_world.levels);
    }
}

/* The squad as the map opens with it, one line a player: what a report of a wrong marker needs. */
static void TacNoteSquad(const TacSnapshot* s)
{
    for (uint32_t i = 0; i < s->players; i++)
    {
        const TacPlayer* p = &s->player[i];
        Note("difficulty: tactical map: player %u \"%s\"%s at %.0f, %.0f heading %.0f, life %u, waypoint %s%.0f, %.0f", i + 1, p->name,
            p->local ? " (this machine)" : "", (double)p->x, (double)p->y, (double)p->yaw, p->life, p->waypoint ? "" : "none ",
            (double)p->wx, (double)p->wy);
    }
}

/* Reads the map's sources into the snapshot and publishes it; returns TacSquad's slot. */
static int TacTickSnapshot(uintptr_t game, float size, float playable)
{
    TacSnapshot* s = &g_tac.work;
    memset(s, 0, offsetof(TacSnapshot, mark));
    s->marks = s->zones = 0;
    s->level = (uint32_t)g_world.levels;
    s->size = size; s->playable = playable;
    snprintf(s->title, sizeof s->title, "SECTOR SURVEY");
    int slot = TacSquad(game, s);
    TacObjectives(game, s);
    TacGuardForces(game, s);
    TacPointsOfInterest(game, s);
    TacExtraction(game, s);
    TacTickHeights();
    s->heights = g_tac.heightsLevel == g_world.levels ? &g_tac.heights : NULL;
    AcquireSRWLockExclusive(&g_tac.lock);
    g_tac.shared = *s;
    ReleaseSRWLockExclusive(&g_tac.lock);
    if (g_tac.open && !g_tac.noted)
        Note("difficulty: tactical map: open: world %.0f m, playable %.0f m, %u player(s), %u marker(s), %u zone(s), heights %s, own slot %d",
            (double)size, (double)playable, s->players, s->marks, s->zones, s->heights ? "ready" : "not yet", slot);
    if (g_tac.open && !g_tac.noted) TacNoteSquad(s);
    g_tac.noted = g_tac.open;
    return slot;
}

/* Sends a waypoint the screen asked for through the game's own ping; one asked for with no level or no own slot is
 * dropped. */
static void TacTickPing(uintptr_t game, uintptr_t lg, int slot)
{
    AcquireSRWLockExclusive(&g_tac.pingLock);
    int pending = g_tac.pingPending, remove = g_tac.pingRemove;
    float at[3] = {g_tac.pingAt[0], g_tac.pingAt[1], 0.0f};
    g_tac.pingPending = 0;
    ReleaseSRWLockExclusive(&g_tac.pingLock);
    if (!pending || !lg || slot < 0) return;
    if (g_tac.pingProven == 0)
        g_tac.pingProven = ApiCodeMatches(game + TAC_PING, "40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 90 E7 FF FF B8 70 19 00 00") == 1 ? 1 : -1;
    uintptr_t hud;
    if (g_tac.pingProven < 0 || !EvpPtr(game + TAC_MAP, &hud)) return;
    /* a ping within 10 m of the waypoint takes it away, so removing is a ping at the waypoint itself */
    ((void (*)(uintptr_t, uint32_t, const float*))(game + TAC_PING))(hud + 0x3EF168, (uint32_t)slot, at);
    uintptr_t m;
    uint32_t flags = EvpPtr(game + TAC_AVATARS, &m) ? EvpU32(m + 0x547830 + 0x78 * (uintptr_t)slot, 0) : 0;
    Note("difficulty: tactical map: waypoint %s at %.0f, %.0f (slot %d): now %s", remove ? "removed" : "asked", (double)at[0], (double)at[1], slot,
        flags & 4 ? "set" : "none");
}

/* The game thread, every frame: the snapshot while the screen is open, and a waypoint asked for. */
static int TacStart(void);
static void TacTick(void)
{
    static int started;
    if (!started && g_difficultyGame && LargerMaps()) started = TacStart() ? 1 : -1;
    if (g_tac.screen == NULL) return;
    uintptr_t game = g_difficultyGame;
    float size = 0, playable = 0;
    uintptr_t lg = game ? TacLevel(game, &size, &playable) : 0;
    int slot = -1;
    if (!g_tac.open) g_tac.noted = 0;
    if (lg && (g_tac.open || g_tac.pingPending)) slot = TacTickSnapshot(game, size, playable);
    TacTickPing(game, lg, slot);
}

static void TacPing(void* user, float x, float y, int remove)
{
    AcquireSRWLockExclusive(&g_tac.pingLock);
    g_tac.pingPending = 1; g_tac.pingRemove = remove; g_tac.pingAt[0] = x; g_tac.pingAt[1] = y;
    ReleaseSRWLockExclusive(&g_tac.pingLock);
}

static void TacPicture(void* user, const TacPictureRequest* request)
{
    AcquireSRWLockExclusive(&g_tac.jobLock);
    g_tac.job.pending = 1; g_tac.job.request = *request; g_tac.job.level = g_tac.heightsLevel;
    ReleaseSRWLockExclusive(&g_tac.jobLock);
    SetEvent(g_tac.wake);
}

/* The worker: the latest picture asked for; one finished for a level that has since gone is dropped. */
static DWORD WINAPI TacWorker(void* unused)
{
    for (;;)
    {
        WaitForSingleObject(g_tac.wake, INFINITE);
        AcquireSRWLockExclusive(&g_tac.jobLock);
        int pending = g_tac.job.pending;
        TacJob job = g_tac.job;
        g_tac.job.pending = 0;
        ReleaseSRWLockExclusive(&g_tac.jobLock);
        if (!pending || job.level != g_world.levels || g_tac.heightsLevel != job.level) continue;
        uint8_t* px = (uint8_t*)malloc((size_t)job.request.width * job.request.height * 4);
        if (px == NULL) continue;
        TacTerrain(&g_tac.heights, &job.request, px);
        if (g_world.levels == job.level) TacScreenPicture(g_tac.screen, &job.request, px);
        free(px);
    }
    return 0;
}

/* The overlay host's callbacks, on its render thread. */
static int TacDraw(void* user, const HfDraw* draw, const HfInput* input, float dt)
{
    AcquireSRWLockShared(&g_tac.lock);
    g_tac.draw = g_tac.shared;
    ReleaseSRWLockShared(&g_tac.lock);
    if (g_tac.draw.heights != NULL && g_tac.heightsLevel != (LONG)g_tac.draw.level) g_tac.draw.heights = NULL;
    return TacScreenDraw(g_tac.screen, &g_tac.draw, draw, input, dt);
}

static void TacOpened(void* user, int open)
{
    InterlockedExchange(&g_tac.open, open);
    TacScreenOpened(g_tac.screen, open);
}

/* Only in a mission on a larger map. */
static int TacCanOpen(void* user)
{
    float size = 0, playable = 0;
    return g_difficultyGame && TacLevel(g_difficultyGame, &size, &playable) && size > 2048.5f;
}

/* Sets the screen up with the overlay host (once, after the game loaded); 1 done. */
static int TacInstall(int (*add_screen)(const HfScreen* screen))
{
    if (g_tac.screen != NULL) return 1;
    InitializeSRWLock(&g_tac.lock);
    InitializeSRWLock(&g_tac.pingLock);
    InitializeSRWLock(&g_tac.jobLock);
    TacHost host = {TacPing, TacPicture, NULL};
    g_tac.heightsLevel = -1;
    g_tac.screen = TacScreenNew(&host);
    g_tac.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (g_tac.screen == NULL || g_tac.wake == NULL) { Note("difficulty: tactical map: no memory; no map screen"); return 0; }
    HANDLE worker = CreateThread(NULL, 0, TacWorker, NULL, 0, NULL);
    if (worker == NULL) { Note("difficulty: tactical map: no worker thread; no map screen"); return 0; }
    CloseHandle(worker);
    static const HfScreen screen = {sizeof(HfScreen), "Tactical map", 'M', HF_SCREEN_MODAL, NULL, TacDraw, TacOpened, TacCanOpen};
    g_tac.screenId = add_screen(&screen);
    Note("difficulty: tactical map: %s", g_tac.screenId > 0 ? "on M" : "the overlay host refused the screen");
    return g_tac.screenId > 0;
}

static void TacHostLog(void* user, const char* line) { Note("overlay: %s", line); }
static int TacHostHook(void* user, uintptr_t target, void* detour, void** original) { return LoaderHook("overlay", target, detour, original); }

/* A path beside the loader (app\sdk-shims), or `up` folders above it, in UTF-8. */
static int TacPath(int up, const wchar_t* tail, char* out, size_t room)
{
    HMODULE self;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void*)TacPath, &self) ||
        !GetModuleFileNameW(self, path, MAX_PATH)) return 0;
    for (int k = 0; k <= up; k++) { wchar_t* slash = wcsrchr(path, L'\\'); if (slash == NULL) return 0; *slash = 0; }
    if (wcslen(path) + wcslen(tail) + 2 > MAX_PATH) return 0;
    wcscat(path, L"\\"); wcscat(path, tail);
    return WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)room, NULL, NULL) > 0;
}

static int TacFileThere(const char* utf8)
{
    wchar_t wide[MAX_PATH];
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, MAX_PATH) && GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
}

/* The first of a file's two places that is there: beside the loader or in the editor's folder, else in the larger
 * maps' own mod folder (a sandboxed session sees only the loader file and the enabled mods' folders). */
static int TacFind(int up, const wchar_t* first, const wchar_t* second, char* out, size_t room)
{
    if (TacPath(up, first, out, room) && TacFileThere(out)) return 1;
    return TacPath(2, second, out, room) && TacFileThere(out);
}

/* The overlay host, started with the game's fonts, and the screen added to it. */
static int TacStart(void)
{
    char host[MAX_PATH * 3], body[MAX_PATH * 3], title[MAX_PATH * 3];
    wchar_t wide[MAX_PATH];
    if (!TacFind(0, L"hellforge_overlay.dll", L"mods\\extra-large-maps\\hellforge_overlay.dll", host, sizeof host) ||
        !MultiByteToWideChar(CP_UTF8, 0, host, -1, wide, MAX_PATH))
    {
        Note("difficulty: tactical map: hellforge_overlay.dll is not beside the loader or in mods\\extra-large-maps; no map screen");
        return 0;
    }
    HMODULE dll = LoadLibraryW(wide);
    if (dll == NULL) { Note("difficulty: tactical map: no overlay host (%s, error %lu); no map screen", host, GetLastError()); return 0; }
    HfOverlayStartFn start = (HfOverlayStartFn)(void*)GetProcAddress(dll, "hf_overlay_start");
    HfOverlayAddScreenFn add = (HfOverlayAddScreenFn)(void*)GetProcAddress(dll, "hf_overlay_add_screen");
    if (start == NULL || add == NULL) { Note("difficulty: tactical map: the overlay host has no hf_overlay_start/add_screen; no map screen"); return 0; }
    int fonts = TacFind(2, L"mods\\hellforge-editor\\fs-sinclair.ttf", L"mods\\extra-large-maps\\fs-sinclair.ttf", body, sizeof body) &&
                TacFind(2, L"mods\\hellforge-editor\\fs-sinclair-medium.ttf", L"mods\\extra-large-maps\\fs-sinclair-medium.ttf", title, sizeof title);
    Note("difficulty: tactical map: overlay host %s, fonts %s", host, fonts ? body : "not found (the host's own)");
    HfOverlayStart s = {sizeof s, NULL, TacHostLog, TacHostHook, NULL, fonts ? body : NULL, fonts ? title : NULL};
    if (!TacInstall(add)) return 0;
    if (start(&s) != 0) { Note("difficulty: tactical map: the overlay host did not start; no map screen"); return 0; }
    return 1;
}
