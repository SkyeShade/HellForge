/* The physics step's rate under load, build 25480438 (docs/performance.md, physics-steps). The engine steps each
 * physics world at a fixed 60 Hz, at most 3 steps a frame (game.dll's world settings +0x34 and +0x38; the step
 * function exe 0x79EB10). Below 60 fps a frame pays more than one whole Havok step (2 at 30 fps, 3 below 20 fps), so
 * the frame gets longer as the step gets dearer: at ~1,000 enemies, 28 fps, 2 steps. This keeps the count near one
 * step a frame below 54 fps by lowering the world's rate to 1.1 / frame time, never under 30 Hz; above that the game's
 * own rate stays.
 * Included by hellforge_modloader.c after render_guards.c: PinMatches (packages.c), PerfFixOn, SafeRead, Note. */

#define PST_RECORDS 0x27BA880u /* exe: the engine's 4 physics world records, 0xB0 bytes each */
#define PST_COUNT 4
#define PST_STRIDE 0xB0u
#define PST_WORLD 0x10u        /* the hknpWorld, or 0 for a free record */
#define PST_RATE 0x38u         /* float steps a second; read only by the step (two divss, pinned) */
#define PST_FLOOR 30.0f        /* no step longer than 1/30 s */
#define PST_CEILING 240.0f     /* a rate the game or a mod set outside 30..240 is left alone */
#define PST_STEPS 1.1f         /* steps a frame the lowered rate aims at: a little over one, so no frame goes without */
#define PST_SMOOTH 0.1f        /* the frame time's running mean: this much of each new frame */
#define PST_DT_MAX 0.25f       /* longer frames (loads, pauses) are not counted */

typedef struct PstPin { uint32_t rva; uint8_t size; const char* what; const uint8_t* bytes; } PstPin;

/* 0x79EB2C: the records' base and stride, the "world steps" flag (+0x0C bit 4). 0x79EB9A: 1/rate x max steps caps the
 * frame's time, the step is min(accumulated, 1/rate), the max steps (+0x3C) and the accumulator (+0x88). */
static const uint8_t kPstRecordPin[] = {0x48,0x8d,0x1d,0xcd,0x14,0x86,0xff, 0x49,0x69,0xfc,0xb0,0x00,0x00,0x00, 0x4c,0x89,0x65,0x88,
    0x48,0x89,0x7d,0x90, 0xf6,0x84,0x1f,0x8c,0xa8,0x7b,0x02,0x04};
static const uint8_t kPstStepPin[] = {0xf3,0x44,0x0f,0x10,0x1d,0x65,0xae,0xf0,0x00, 0x4c,0x8b,0x2a, 0x41,0x0f,0x28,0xcb,
    0xf3,0x0f,0x5e,0x8c,0x1f,0xb8,0xa8,0x7b,0x02, 0x44,0x8b,0x3d,0x0a,0xf7,0xbb,0x01, 0x89,0x44,0x24,0x48,
    0x8b,0x84,0x1f,0xbc,0xa8,0x7b,0x02, 0x44,0x89,0x7c,0x24,0x58, 0x4c,0x89,0x6c,0x24,0x50, 0xf3,0x0f,0x10,0xb4,0x1f,0x0c,0xa9,0x7b,0x02,
    0xf3,0x48,0x0f,0x2a,0xc0, 0x89,0xb4,0x1f,0x0c,0xa9,0x7b,0x02, 0xf3,0x0f,0x59,0xc8, 0xf3,0x0f,0x10,0x42,0x0c, 0xe8,0xaa,0xe7,0xb0,0x00,
    0xf3,0x44,0x0f,0x5e,0x9c,0x1f,0xb8,0xa8,0x7b,0x02, 0x44,0x8b,0xb4,0x1f,0xbc,0xa8,0x7b,0x02, 0x4d,0x8b,0xc5,
    0x44,0x89,0xb5,0xe8,0x09,0x00,0x00, 0x41,0x8b,0xcc, 0x0f,0x28,0xf8, 0x0f,0x28,0xce,
    0xf3,0x0f,0x58,0xbc,0x1f,0x08,0xa9,0x7b,0x02, 0xf3,0x0f,0x11,0xbc,0x1f,0x08,0xa9,0x7b,0x02, 0xf3,0x41,0x0f,0x5d,0xfb};

static const PstPin kPstPins[] = {
    {0x79EB2Cu, sizeof kPstRecordPin, "the step's world record", kPstRecordPin},
    {0x79EB9Au, sizeof kPstStepPin, "the step's rate and accumulator", kPstStepPin},
};
#define PST_PINS (sizeof kPstPins / sizeof kPstPins[0])

typedef struct PstWorld { uintptr_t h; float base, ours; } PstWorld;

static volatile LONG g_pstState; /* 0 waiting for the code, 1 on, -1 off; set after g_pstExe, read by the game thread */
static uintptr_t g_pstExe;
static ULONGLONG g_pstLoadedAt, g_pstReportedAt;
static PstWorld g_pstWorlds[PST_COUNT]; /* the game thread's only */
static float g_pstDt;                 /* the frame time's running mean, seconds */
static volatile LONG64 g_pstFrames, g_pstLowered;
static volatile LONG g_pstLowest;     /* the lowest rate set, Hz x 10 */
static LONG64 g_pstLoweredReported;

/* The first pin that differs, or NULL. */
static const char* PhysicsStepsMismatch(uintptr_t exe)
{
    for (size_t i = 0; i < PST_PINS; i++)
        if (!PinMatches(exe + kPstPins[i].rva, kPstPins[i].bytes, kPstPins[i].size)) return kPstPins[i].what;
    return NULL;
}

/* The rate for a world whose own rate is base, at a mean frame time of dt: base until a frame takes more than one of
 * its steps, then 1.1 / dt, never under the floor. */
static float PhysicsStepsRate(float base, float dt)
{
    if (!(dt > 0.0f)) return base;
    float rate = PST_STEPS / dt;
    if (rate > base - 0.5f) rate = base; /* within half a step a second of the game's: the game's */
    if (rate < PST_FLOOR) rate = PST_FLOOR;
    return rate;
}

/* One world record, on the game thread. A rate that is not the one written last is the game's (a new world) or
 * someone else's: it becomes the base. A base outside 30..240 Hz is left alone. */
static void PhysicsStepsWorld(PstWorld* w, uintptr_t record, uintptr_t h)
{
    float rate;
    if (SafeRead(record + PST_RATE, &rate, 4) || !isfinite(rate)) return;
    if (h != w->h || rate != w->ours) { w->h = h; w->base = rate; }
    if (w->base < PST_FLOOR || w->base > PST_CEILING) { w->ours = NAN; return; }
    float want = PhysicsStepsRate(w->base, g_pstDt);
    if (want != rate) *(volatile float*)(record + PST_RATE) = want;
    w->ours = want;
    if (want < w->base)
    {
        InterlockedIncrement64(&g_pstLowered);
        LONG tenths = (LONG)(want * 10.0f);
        if (g_pstLowest == 0 || tenths < g_pstLowest) InterlockedExchange(&g_pstLowest, tenths);
    }
}

/* The game thread's update (packages.c TickDetour), before the frame's physics step (WorldUpdateScene, later in the
 * same frame) and on the thread that creates and destroys physics worlds, so a set record is a live world. */
static void PhysicsStepsTick(float dt)
{
    if (g_pstState != 1) return;
    if (dt > 0.0f && dt < PST_DT_MAX) g_pstDt = g_pstDt > 0.0f ? g_pstDt + PST_SMOOTH * (dt - g_pstDt) : dt;
    InterlockedIncrement64(&g_pstFrames);
    for (int i = 0; i < PST_COUNT; i++)
    {
        uintptr_t record = g_pstExe + PST_RECORDS + (uintptr_t)i * PST_STRIDE, h = 0;
        if (SafeRead(record + PST_WORLD, &h, 8) || h == 0) { g_pstWorlds[i].h = 0; continue; }
        PhysicsStepsWorld(&g_pstWorlds[i], record, h);
    }
}

/* 1 on, 0 off for the session, -1 not yet: the game's code may still be decoding. */
static int PhysicsStepsInstallAt(uintptr_t exe, int lastTry)
{
    const char* differs = PhysicsStepsMismatch(exe);
    if (differs != NULL)
    {
        if (!lastTry) return -1;
        Note("performance fix physics-steps: off; %s is not the reviewed code", differs);
        return 0;
    }
    if (!PerfFixOn("physics-steps")) return 0;
    g_pstExe = exe;
    Note("performance fix physics-steps: on; below %.0f fps the physics rate follows the frame (%.1f steps a frame, never under %.0f Hz)",
        (double)(59.5f / PST_STEPS), (double)PST_STEPS, (double)PST_FLOOR);
    return 1;
}

/* From the watch thread, once the game has loaded and its update is hooked; a minute later the code is final. */
static void PhysicsStepsWatch(void)
{
    if (g_pstState == 1 && g_pstLowered != g_pstLoweredReported && GetTickCount64() - g_pstReportedAt >= 60000)
    {
        LONG64 frames = g_pstFrames, lowered = g_pstLowered;
        g_pstReportedAt = GetTickCount64();
        Note("performance fix physics-steps: %lld world frame(s) of %lld at a lowered rate so far (lowest %.1f Hz)",
            (long long)lowered, (long long)frames, (double)g_pstLowest / 10.0);
        g_pstLoweredReported = lowered;
    }
    if (g_pstState != 0 || !g_fired[HF_EVENT_GAME_LOADED] || g_tickState == 0) return;
    if (strcmp(g_build, "25480438") || g_tickState < 0)
    {
        g_pstState = -1;
        Note("performance fix physics-steps: off; %s", g_tickState < 0 ? "the game thread's update is not hooked" : "not the reviewed build");
        return;
    }
    if (g_pstLoadedAt == 0) g_pstLoadedAt = GetTickCount64();
    int state = PhysicsStepsInstallAt((uintptr_t)GetModuleHandleW(NULL), GetTickCount64() - g_pstLoadedAt >= 60000);
    if (state >= 0) InterlockedExchange(&g_pstState, state ? 1 : -1);
}
