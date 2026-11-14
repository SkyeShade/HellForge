/* The Havok AI world's path time budget, build 25480438 (docs/performance.md, ai-path-budget). Each AI step lets the
 * async path threads run for H+0x47C seconds, and the next frame's join waits for the step. A quarter of the game's
 * 0.002 s shortens that wait; paths take more steps instead, so the game's budget comes back while requests wait.
 * Included by hellforge_modloader.c after render_guards.c: PinMatches (packages.c), PerfFixOn, SafeRead, Note. */

#define AIB_RECORDS 0x190D4A0u /* the engine's AI world records: 4 of 0x80 bytes, +0 the hkaiWorld or 0 */
#define AIB_RECORD_COUNT 4
#define AIB_RECORD_SIZE 0x80u
#define AIB_BUDGET 0x47Cu      /* float seconds; the step's only read is at 0xBD198C */
#define AIB_WAITED 0x480u      /* two floats of step time: requests waiting for a run, the age of the run in flight */
#define AIB_STOCK 0.002f      /* the game's budget: its AI world is made from game.dll's constant (pinned) */
#define AIB_LOW 0.0005f
#define AIB_RAISE_S 0.5f       /* a wait this long brings the game's budget back */
#define AIB_DROP_S 0.25f       /* until the wait is under this */

typedef struct AibPin { int game; uint32_t rva; uint8_t size; const char* what; uint8_t bytes[16]; } AibPin;

static const AibPin kAibPins[] = {
    {0, 0xBD198Cu, 16, "the step's drive", {0xf3,0x0f,0x10,0x8b,0x7c,0x04,0x00,0x00, 0x48,0x8b,0xcb, 0xe8,0x04,0xeb,0xff,0xff}},
    {0, 0xBD1916u, 12, "the step's wait clock", {0x48,0x8d,0x8b,0x80,0x04,0x00,0x00, 0xe8,0x0e,0xe7,0x06,0x00}},
    {0, 0xC40030u, 16, "the wait clock's sums", {0x41,0x80,0x78,0x01,0x00,0x74,0x0b,0x0f,0x28,0xc1,0xf3,0x0f,0x58,0x01,0xf3,0x0f}},
    {0, 0xC40040u, 16, "the wait clock's sums", {0x11,0x01,0x41,0x80,0x38,0x00,0x74,0x0a,0xf3,0x0f,0x58,0x49,0x04,0xf3,0x0f,0x11}},
    {0, 0xC400A0u, 8, "the wait clock's commit", {0xc7,0x41,0x04,0x00,0x00,0x00,0x00,0xc3}},
    {0, 0xC400B0u, 12, "the wait clock's start", {0x8b,0x01,0x89,0x41,0x04,0xc7,0x01,0x00,0x00,0x00,0x00,0xc3}},
    {0, 0x7D7D4Cu, 4, "the AI world's create", {0x48,0xc1,0xe6,0x07}},
    {0, 0x7D7DC3u, 8, "the AI world's create", {0x48,0x89,0xbc,0x1e,0xa0,0xd4,0x90,0x01}},
    {0, 0x7D81B5u, 14, "the AI world's budget", {0xf3,0x41,0x0f,0x10,0x4e,0x1c, 0x48,0x8b,0xcf, 0xe8,0xad,0x91,0x3f,0x00}},
    {0, 0xBD1370u, 9, "the AI world's budget", {0xf3,0x0f,0x11,0x89,0x7c,0x04,0x00,0x00,0xc3}},
    {0, 0x7D83CEu, 4, "the AI world's destroy", {0x49,0xc1,0xe5,0x07}},
    {0, 0x7D85A3u, 8, "the AI world's destroy", {0x4b,0x89,0x9c,0x2c,0xa0,0xd4,0x90,0x01}},
    {1, 0xAB1AE6u, 16, "the game's AI settings", {0xf3,0x0f,0x10,0x05,0xea,0x4a,0x91,0x01, 0xf3,0x0f,0x11,0x85,0xac,0x04,0x00,0x00}},
    {1, 0xAB1B1Bu, 7, "the game's AI settings", {0x4c,0x8d,0x85,0x90,0x04,0x00,0x00}},
    {1, 0x23C65D8u, 4, "the game's AI budget", {0x6f,0x12,0x03,0x3b}}, /* AIB_STOCK */
};
#define AIB_PINS (sizeof kAibPins / sizeof kAibPins[0])

typedef struct AibWorld { uintptr_t h; int raised; } AibWorld;

static volatile LONG g_aibState; /* 0 waiting for the code, 1 on, -1 off; set after g_aibExe, read by the game thread */
static ULONGLONG g_aibLoadedAt, g_aibReportedAt;
static uintptr_t g_aibExe;
static AibWorld g_aibWorlds[AIB_RECORD_COUNT]; /* the game thread's only */
static volatile LONG g_aibRaises;
static LONG g_aibRaisesReported;

/* The first pin that differs, or NULL. */
static const char* AiPathBudgetMismatch(uintptr_t exe, uintptr_t game)
{
    for (size_t i = 0; i < AIB_PINS; i++)
        if (!PinMatches((kAibPins[i].game ? game : exe) + kAibPins[i].rva, kAibPins[i].bytes, kAibPins[i].size)) return kAibPins[i].what;
    return NULL;
}

/* One AI world, on the game thread. A budget that is neither the game's nor ours is someone else's and stays. */
static void AiPathBudgetSteer(AibWorld* w, uintptr_t h)
{
    float budget, waited[2];
    if (h != w->h) { w->h = h; w->raised = 0; }
    if (SafeRead(h + AIB_BUDGET, &budget, 4) || (budget != AIB_STOCK && budget != AIB_LOW)) return;
    if (SafeRead(h + AIB_WAITED, waited, 8) || !isfinite(waited[0]) || !isfinite(waited[1])) waited[0] = waited[1] = INFINITY;
    float wait = waited[0] > waited[1] ? waited[0] : waited[1];
    if (wait > AIB_RAISE_S && !w->raised) { w->raised = 1; InterlockedIncrement(&g_aibRaises); }
    else if (wait < AIB_DROP_S) w->raised = 0;
    float want = w->raised ? AIB_STOCK : AIB_LOW;
    if (budget != want) *(volatile float*)(h + AIB_BUDGET) = want;
}

/* The game thread's update (packages.c TickDetour), inside game.dll's frame: after the previous step's join and before
 * the next kick, and on the thread that creates and destroys AI worlds (which clears the record), so a set record is
 * a live world. */
static void AiPathBudgetTick(void)
{
    if (g_aibState != 1) return;
    for (int i = 0; i < AIB_RECORD_COUNT; i++)
    {
        uintptr_t h = 0;
        if (SafeRead(g_aibExe + AIB_RECORDS + (uintptr_t)i * AIB_RECORD_SIZE, &h, 8) || h == 0) { g_aibWorlds[i].h = 0; continue; }
        AiPathBudgetSteer(&g_aibWorlds[i], h);
    }
}

/* 1 on, 0 off for the session, -1 not yet: the game's code may still be decoding. */
static int AiPathBudgetInstallAt(uintptr_t exe, uintptr_t game, int lastTry)
{
    const char* differs = AiPathBudgetMismatch(exe, game);
    if (differs != NULL)
    {
        if (!lastTry) return -1;
        Note("performance fix ai-path-budget: off; %s is not the reviewed code", differs);
        return 0;
    }
    if (!PerfFixOn("ai-path-budget")) return 0;
    g_aibExe = exe;
    Note("performance fix ai-path-budget: on; AI path threads get %.0f us a step, the game's %.0f us while requests wait over %.0f ms",
        (double)AIB_LOW * 1e6, (double)AIB_STOCK * 1e6, (double)AIB_RAISE_S * 1e3);
    return 1;
}

/* From the watch thread, once the game has loaded and its update is hooked; a minute later the code is final. */
static void AiPathBudgetWatch(void)
{
    if (g_aibState == 1 && g_aibRaises != g_aibRaisesReported && GetTickCount64() - g_aibReportedAt >= 60000)
    {
        g_aibRaisesReported = g_aibRaises;
        g_aibReportedAt = GetTickCount64();
        Note("performance fix ai-path-budget: the game's budget came back %ld time(s) so far, for path requests waiting", (long)g_aibRaisesReported);
    }
    if (g_aibState != 0 || !g_fired[HF_EVENT_GAME_LOADED] || g_tickState == 0) return;
    if (strcmp(g_build, "25480438") || g_tickState < 0)
    {
        g_aibState = -1;
        Note("performance fix ai-path-budget: off; %s", g_tickState < 0 ? "the game thread's update is not hooked" : "not the reviewed build");
        return;
    }
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0) return;
    if (g_aibLoadedAt == 0) g_aibLoadedAt = GetTickCount64();
    int state = AiPathBudgetInstallAt((uintptr_t)GetModuleHandleW(NULL), game, GetTickCount64() - g_aibLoadedAt >= 60000);
    if (state >= 0) InterlockedExchange(&g_aibState, state ? 1 : -1);
}
