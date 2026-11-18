/* steer-lod (docs/performance-enemies.md), build 25480438. Enemies far from every Helldiver (over 100 m) run the AI
 * steering pass (game.dll 0x7D9FA0) every third frame instead of every frame, staggered by their index so a third of
 * them run each frame; enemies within 100 m of any Helldiver run it every frame. The pass takes a range of instances
 * (start, end, dt), and the game itself has range tasks of 16 for it (0x5710B0, unused in this build), so the hook calls
 * it once for each run of instances due this frame. A skipped enemy keeps the steering it had: its motion pass still
 * moves it every frame. Whether an enemy is far is read for a quarter of them each frame (their unit's position against
 * the Helldivers'). Aim, perception and attacks are other passes and are untouched.
 * Included by perf_all.c after perf_vehicle_box.c (its pin of the pass's start) and actions.c (the players).
 * LoaderHook (hooks.c), PinMatches (packages.c), PerfFixOn, Note, g_build, ApiModuleBase. */

#define SL_STEER 0x7D9FA0u        /* game: the steering pass (unused, start, end, dt in xmm3) */
#define SL_MANAGER 0x3326D18u     /* game: the steering component; +0x20 instances, +0x48 their entity descriptors */
#define SL_DESCRIPTORS_AT 0x48u
#define SL_COUNT_AT 0x20u
#define SL_MAX 4096u
#define SL_EVERY 3u
#define SL_FAR_SQ 10000.0f        /* 100 m */
#define SL_PLAYERS 8

typedef struct SlPin { uint32_t rva; uint16_t size; const char* what; uint8_t bytes[96]; } SlPin;

/* The pass's per-instance start: the descriptor from +0x48, its entity at +8 (the descriptor's unit is at +0xC). The
 * pass's start (the manager, its count at +0x20, the range clamp) is vehicle-box's pin. */
static const SlPin kSlPins[] = {
    {0x7DA061u, 0x4D, "the steering pass's instance", {
        0x8b,0xca,0x48,0x8b,0x54,0x24,0x58,0x4c,0x69,0xe1,0x84,0x00,0x00,0x00,0x48,0x8b,0x42,0x48,0x4c,0x03,0x62,0x58,0x4c,0x8b,
        0x2c,0xc8,0x8b,0xc1,0x48,0xc1,0xe0,0x06,0x48,0x03,0x42,0x50,0x48,0x89,0x45,0x90,0x48,0x8d,0x04,0x89,0x48,0x8b,0x4a,0x60,
        0x48,0x89,0x45,0xa0,0x48,0x89,0x4d,0x98,0x80,0x7c,0x81,0x04,0x00,0x0f,0x84,0x58,0x14,0x00,0x00,0x41,0x8b,0x45,0x08,0x8b,
        0x15,0x72,0x9b,0xca,0x02}},
};
#define SL_PINS (sizeof kSlPins / sizeof kSlPins[0])

typedef void (*SlPassFn)(void* unused, uint32_t start, uint32_t end, float dt);
typedef int (*SlPositionFn)(uint32_t unit, float* position);

static void* volatile g_slNext;
static uintptr_t g_slGame;
static volatile LONG g_slOn;
static int g_slState;
static ULONGLONG g_slFirstTry, g_slReportedAt;
static uint8_t g_slFar[SL_MAX];   /* the game thread's: 1 far from every Helldiver when last read */
static float g_slPlayer[SL_PLAYERS][3];
static int g_slPlayerCount;
static uint32_t g_slFrame;
static LONG64 g_slSkipped, g_slRan, g_slCalls, g_slLast[3];
static SlPositionFn g_slPosition = EvpUnitPosition; /* the fixture's own positions, else the unit registry's */
static int (*g_slPlayers)(float (*out)[3], int room);

static const char* SteerLodMismatch(uintptr_t game)
{
    if (!PinMatches(game + VB_STEER, kVbSteerPin, sizeof kVbSteerPin)) return "the steering pass's start";
    for (size_t i = 0; i < SL_PINS; i++)
        if (!PinMatches(game + kSlPins[i].rva, kSlPins[i].bytes, kSlPins[i].size)) return kSlPins[i].what;
    return NULL;
}

/* Every player's Helldiver's position (living or not): how many. */
static int SteerLodGamePlayers(float (*out)[3], int room)
{
    EvpPlayer players[EVT_MAX_PLAYERS];
    int n = EvpPlayers(g_slGame, players), count = 0;
    for (int i = 0; i < n && count < room; i++)
    {
        ActHealth h;
        if (players[i].avatar && ActHealthOf(g_slGame, players[i].avatar, &h) && h.unit && g_slPosition(h.unit, out[count])) count++;
    }
    return count;
}

static int SteerLodFar(uintptr_t descriptor)
{
    uint32_t unit;
    float p[3];
    if (descriptor < 0x10000 || SafeRead(descriptor + 0xC, &unit, 4) || !unit || !g_slPosition(unit, p)) return 0;
    for (int k = 0; k < g_slPlayerCount; k++)
    {
        float dx = p[0] - g_slPlayer[k][0], dy = p[1] - g_slPlayer[k][1], dz = p[2] - g_slPlayer[k][2];
        if (dx * dx + dy * dy + dz * dz <= SL_FAR_SQ) return 0;
    }
    return 1;
}

/* The steering component's instance count and descriptors; 0 when they do not read. */
static int SteerLodManager(uint32_t* count, uintptr_t* descriptors)
{
    uintptr_t manager = 0;
    return !SafeRead(g_slGame + SL_MANAGER, &manager, 8) && manager && !SafeRead(manager + SL_COUNT_AT, count, 4) &&
        !SafeRead(manager + SL_DESCRIPTORS_AT, descriptors, 8) && *descriptors;
}

/* The game's pass on each run of instances due this frame: near ones, and far ones on their third frame. */
static void SteerLodRuns(SlPassFn next, void* unused, uint32_t start, uint32_t last, uint32_t end, float dt)
{
    uint32_t run = start;
    for (uint32_t i = start; i < last; i++)
    {
        if (!g_slFar[i] || (i + g_slFrame) % SL_EVERY == 0) continue;
        if (run < i) { next(unused, run, i, dt); g_slCalls++; g_slRan += i - run; }
        run = i + 1;
        g_slSkipped++;
    }
    if (run < end) { next(unused, run, end, dt); g_slCalls++; g_slRan += last > run ? last - run : 0; }
}

/* The pass on the game thread: the world update calls it once a frame for (0, count). */
static void SteerLodPass(void* unused, uint32_t start, uint32_t end, float dt)
{
    SlPassFn next = (SlPassFn)g_slNext;
    uintptr_t descriptors = 0;
    uint32_t count = 0;
    if (!g_slOn || !SteerLodManager(&count, &descriptors)) { next(unused, start, end, dt); return; }
    if (start == 0)
    {
        g_slFrame++;
        g_slPlayerCount = g_slPlayers(g_slPlayer, SL_PLAYERS);
    }
    uint32_t last = end < count ? end : count;
    if (g_slPlayerCount <= 0 || last > SL_MAX) { next(unused, start, end, dt); return; }
    for (uint32_t i = start + ((g_slFrame - start) & 3u); i < last; i += 4)
        g_slFar[i] = (uint8_t)SteerLodFar(((const uintptr_t*)descriptors)[i]);
    SteerLodRuns(next, unused, start, last, end, dt);
}

static int SteerLodTry(uintptr_t game, int lastTry)
{
    const char* differs = SteerLodMismatch(game);
    if (differs != NULL)
    {
        if (!lastTry) return 0;
        Note("performance fix steer-lod: not installed; %s is not the reviewed code", differs);
        return -1;
    }
    if (!PerfFixOn("steer-lod")) return -1;
    g_slGame = game;
    if (g_slPlayers == NULL) g_slPlayers = SteerLodGamePlayers;
    if (LoaderHook("steer-lod: the steering pass", game + SL_STEER, (void*)SteerLodPass, (void**)&g_slNext) != 0)
    {
        Note("performance fix steer-lod: not installed; the steering pass could not be hooked");
        return -1;
    }
    g_slReportedAt = GetTickCount64();
    InterlockedExchange(&g_slOn, 1);
    Note("performance fix steer-lod: installed; enemies over %.0f m from every Helldiver steer every %u frames", sqrt(SL_FAR_SQ), SL_EVERY);
    return 1;
}

static void SteerLodWatch(void)
{
    if (g_slState == 1 && g_slCalls > g_slLast[2] && GetTickCount64() - g_slReportedAt >= 60000)
    {
        LONG64 now[3] = {g_slSkipped, g_slRan, g_slCalls};
        double skipped = (double)(now[0] - g_slLast[0]), ran = (double)(now[1] - g_slLast[1]);
        Note("performance fix steer-lod: in the last %.0f s, %.1f%% of the enemies' steering updates skipped (%.0f skipped, %.0f run, "
            "%lld calls of the pass)", (double)(GetTickCount64() - g_slReportedAt) / 1e3, skipped + ran > 0 ? 100.0 * skipped / (skipped + ran) : 0.0,
            skipped, ran, (long long)(now[2] - g_slLast[2]));
        memcpy(g_slLast, now, sizeof now);
        g_slReportedAt = GetTickCount64();
    }
    if (g_slState != 0) return;
    if (strcmp(g_build, "25480438")) { g_slState = -1; Note("performance fix steer-lod: not installed; build %s is not the reviewed 25480438", g_build); return; }
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0) return;
    if (g_slFirstTry == 0) g_slFirstTry = GetTickCount64();
    g_slState = SteerLodTry(game, GetTickCount64() - g_slFirstTry >= 60000);
}
