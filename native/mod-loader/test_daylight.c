/* Test switch HELLFORGE_TEST_DAYLIGHT=1 (development only; this machine's view only): missions at noon, for test runs
 * that must be well lit. Included by hellforge_modloader.c after living_world.c. Static analysis (KB
 * test-daylight-time-of-day; build 25480438; P unless marked):
 * - The time of day is an output: minutes 0..1440 (noon 720) at [G+0x78AE668], G = [game+0x3326340], recomputed every
 *   frame (0x1017EF0, stored 0x102744A) from the sun's direction, which the per-frame sky job builds in 0x1022270.
 * - Its spin angle comes from 0x1016C20(rcx, rdx, xmm2 = period (double), xmm3 = phase (float), [rsp+0x28] = t):
 *   angle = fmod(t / (period + 0.001) 2 pi + phase, 2 pi), or the phase itself when period < 0 (0x1016CBE..0x1016CC9).
 * - Its one call site 0x102244E (inside 0x1022270, reached from 0x102465F and 0x1023580) is redirected to a stub that
 *   sets xmm2 = -1.0 and xmm3 = X and jumps on: the sun stands at angle X. Everything lit by the sun (night_amount,
 *   sunset_amount, fog and ambient colours, the audio RTPCs, telemetry) is derived from it in the same job.
 * - X is found per mission from the game's own clock: with X = 0 the minutes read m0; minutes are linear in the angle
 *   (I: +-1440 / 2 pi per radian, sign measured), so X = s (720 - m0) 2 pi / 1440, s = +1, else -1.
 * Installed at game load (before any frame runs the site). Host and clients compute their own sky (I: not replicated),
 * so this changes only this machine's view (and night-dependent checks it runs). */
#define DAY_SITE 0x102244Eu
#define DAY_SPIN 0x1016C20u
static volatile float* g_dayAngle; /* X, in the stub's data page; NULL when not installed */

static void InstallDaylight(uintptr_t game)
{
    char t[4] = "";
    if (game == 0 || !(GetEnvironmentVariableA("HELLFORGE_TEST_DAYLIGHT", t, sizeof t) > 0 && t[0] == '1')) return;
    if (!ApiCodeMatches(game + DAY_SITE, "E8 CD 47 FF FF") ||
        !ApiCodeMatches(game + 0x1016C71u, "F2 44 0F 10 05 CE FF 3A 01 0F 28 CA F2 0F 10 84 24 20 01 00 00") ||
        !ApiCodeMatches(game + 0x1016CBAu, "45 0F 57 FF 66 0F 2F CE F2 44 0F 5A F8 76 04 44 0F 28 FF"))
    { Note("test: daylight: the sky's spin is not the reviewed code; missions keep their time of day"); return; }
    uint8_t* block = AllocateNearGame(game, 0x2000);
    if (block == NULL) { Note("test: daylight: no memory within 2 GB of game.dll"); return; }
    uint8_t code[21] = {0xF2, 0x0F, 0x10, 0x15, 0, 0, 0, 0,  /* movsd xmm2, [rip+period] (-1.0; rel32 at 4, next 8) */
        0xF3, 0x0F, 0x10, 0x1D, 0, 0, 0, 0,                  /* movss xmm3, [rip+X] (rel32 at 12, next 16) */
        0xE9, 0, 0, 0, 0};                                   /* jmp 0x1016C20 (rel32 at 17, next 21) */
    int32_t rel = 0x1000 - 8; memcpy(code + 4, &rel, 4);
    rel = 0x1008 - 16; memcpy(code + 12, &rel, 4);
    int64_t on = (int64_t)(game + DAY_SPIN) - (int64_t)(uintptr_t)(block + 21);
    if (!InRel32(on)) { Note("test: daylight: the stub is out of reach of game.dll"); return; }
    rel = (int32_t)on; memcpy(code + 17, &rel, 4);
    memcpy(block, code, sizeof code);
    double period = -1.0; memcpy(block + 0x1000, &period, 8);
    float zero = 0.0f; memcpy(block + 0x1008, &zero, 4);
    DWORD protect;
    VirtualProtect(block, 0x1000, PAGE_EXECUTE_READ, &protect);
    FlushInstructionCache(GetCurrentProcess(), block, 0x1000);
    uint8_t call[5] = {0xE8, 0, 0, 0, 0};
    int64_t to = (int64_t)(uintptr_t)block - (int64_t)(game + DAY_SITE + 5);
    if (!InRel32(to)) { Note("test: daylight: the stub is out of reach of the call site"); return; }
    rel = (int32_t)to; memcpy(call + 1, &rel, 4);
    if (ApiWrite(game + DAY_SITE, call, 5)) { Note("test: daylight: the call site could not be written"); return; }
    g_dayAngle = (volatile float*)(block + 0x1008);
    Note("test: daylight: the sun's spin is fixed (HELLFORGE_TEST_DAYLIGHT=1); each mission is set to noon by its own clock");
}

/* Game thread, every tick: per level, measure the clock at X = 0, then set X for noon and check it (both signs). */
static void DaylightTick(void)
{
    static uintptr_t level; static uint32_t seed; static int step, frames; static float m0, x;
    uintptr_t game = g_lw.game ? g_lw.game : ApiModuleBase("game.dll", NULL), g, lg = 0;
    if (g_dayAngle == NULL || !EvpPtr(game + 0x3326340u, &g)) return;
    EvpPtr(g + 0xAE288u, &lg);
    uint32_t s = lg ? EvpU32(lg + 8, 0) : 0;
    if (lg != level || s != seed) { level = lg; seed = s; step = 0; frames = 0; *g_dayAngle = 0.0f; return; }
    if (lg == 0 || step >= 3 || ++frames < 30) return;
    frames = 0;
    float m = -1, night = -1;
    SafeRead(g + 0x78AE668u, &m, 4); SafeRead(g + 0x78AE5BCu, &night, 4);
    if (!(m >= 0 && m <= 1440)) return;
    const float tau = 6.2831853f;
    if (step == 0) { m0 = m; x = (720.0f - m0) * tau / 1440.0f; *g_dayAngle = x; step = 1;
        Note("test: daylight: the clock reads %.0f min at angle 0 (night %.2f); trying %.3f rad", (double)m0, (double)night, (double)x); }
    else if (step == 1)
    {
        if (fabsf(m - 720.0f) < 30.0f) { step = 3; Note("test: daylight: %.0f min (night %.2f) at %.3f rad", (double)m, (double)night, (double)x); }
        else { x = -x; *g_dayAngle = x; step = 2; Note("test: daylight: %.0f min with the first sign; trying %.3f rad", (double)m, (double)x); }
    }
    else { step = 3; Note("test: daylight: %.0f min (night %.2f) at %.3f rad%s", (double)m, (double)night, (double)x, fabsf(m - 720.0f) < 30.0f ? "" : " (not noon)"); }
}
