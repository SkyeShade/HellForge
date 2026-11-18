/* The physics step's rate (perf_physics_steps.c) on a synthetic exe image that holds the pinned bytes and the engine's
 * physics world records: the game's rate at 54 fps and up, 1.1 / frame time below it with the 30 Hz floor, the running
 * mean, a rate of someone else's taken as the base, a base out of range left alone, a new and a freed world, the pins
 * refusing changed code and HELLFORGE_PERF_OFF. Included by stats_test.c. */

#define PST_TEST_EXE 0x27BAB00u

static float PstTestRate(const uint8_t* exe, int world) { float f; memcpy(&f, exe + PST_RECORDS + PST_STRIDE * world + PST_RATE, 4); return f; }
static void PstTestSet(uint8_t* exe, int world, uintptr_t h, float rate)
{
    memcpy(exe + PST_RECORDS + PST_STRIDE * world + PST_WORLD, &h, 8);
    memcpy(exe + PST_RECORDS + PST_STRIDE * world + PST_RATE, &rate, 4);
}
static int PstTestNear(float a, float b) { return fabsf(a - b) < 0.01f; }

static void PerfPhysicsStepsFixture(void)
{
    uint8_t* exe = (uint8_t*)VirtualAlloc(NULL, PST_TEST_EXE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(exe != NULL);
    for (size_t i = 0; i < PST_PINS; i++) memcpy(exe + kPstPins[i].rva, kPstPins[i].bytes, kPstPins[i].size);

    /* each pin refuses its own changed byte, until the minute is up */
    assert(PhysicsStepsMismatch((uintptr_t)exe) == NULL);
    for (size_t i = 0; i < PST_PINS; i++)
    {
        uint8_t* at = exe + kPstPins[i].rva + kPstPins[i].size / 2;
        *at ^= 0x10;
        assert(PhysicsStepsMismatch((uintptr_t)exe) == kPstPins[i].what);
        assert(PhysicsStepsInstallAt((uintptr_t)exe, 0) == -1 && PhysicsStepsInstallAt((uintptr_t)exe, 1) == 0);
        *at ^= 0x10;
    }
    SetEnvironmentVariableW(L"HELLFORGE_PERF_OFF", L"safe-read , physics-steps");
    assert(PhysicsStepsInstallAt((uintptr_t)exe, 1) == 0 && g_pstExe == 0);
    SetEnvironmentVariableW(L"HELLFORGE_PERF_OFF", NULL);

    /* the rate itself */
    assert(PhysicsStepsRate(60.0f, 1.0f / 60) == 60.0f && PhysicsStepsRate(60.0f, 1.0f / 55) == 60.0f);
    assert(PstTestNear(PhysicsStepsRate(60.0f, 0.025f), 44.0f) && PstTestNear(PhysicsStepsRate(60.0f, 1.0f / 30), 33.0f));
    assert(PhysicsStepsRate(60.0f, 0.05f) == PST_FLOOR && PhysicsStepsRate(60.0f, 0.0f) == 60.0f && PhysicsStepsRate(40.0f, 0.025f) == 40.0f);

    _Alignas(16) static uint8_t world[64], other[64];
    PstTestSet(exe, 1, (uintptr_t)world, 60.0f);
    PhysicsStepsTick(0.05f);
    assert(PstTestRate(exe, 1) == 60.0f); /* off until the watch turns it on */
    assert(PhysicsStepsInstallAt((uintptr_t)exe, 0) == 1 && g_pstExe == (uintptr_t)exe);
    g_pstState = 1;
    g_pstDt = 0;
    PhysicsStepsTick(1.0f / 60);
    assert(PstTestRate(exe, 1) == 60.0f && g_pstWorlds[1].base == 60.0f);
    for (int i = 0; i < 200; i++) PhysicsStepsTick(0.025f); /* 40 fps: the mean gets there */
    assert(PstTestNear(PstTestRate(exe, 1), 44.0f) && g_pstWorlds[1].base == 60.0f && g_pstLowered > 0 && g_pstLowest <= 441);
    PhysicsStepsTick(0.5f); /* a load's frame is not counted */
    assert(PstTestNear(PstTestRate(exe, 1), 44.0f));
    for (int i = 0; i < 200; i++) PhysicsStepsTick(0.2f);
    assert(PstTestRate(exe, 1) == PST_FLOOR);
    for (int i = 0; i < 200; i++) PhysicsStepsTick(1.0f / 60); /* back at 60 fps: the game's rate */
    assert(PstTestRate(exe, 1) == 60.0f);

    /* someone else's rate becomes the base; one out of range is left alone */
    for (int i = 0; i < 200; i++) PhysicsStepsTick(0.025f);
    PstTestSet(exe, 1, (uintptr_t)world, 50.0f);
    PhysicsStepsTick(0.025f);
    assert(g_pstWorlds[1].base == 50.0f && PstTestNear(PstTestRate(exe, 1), 44.0f));
    PstTestSet(exe, 1, (uintptr_t)world, 500.0f);
    PhysicsStepsTick(0.025f);
    PhysicsStepsTick(0.025f);
    assert(PstTestRate(exe, 1) == 500.0f);

    /* a new world (the game's 60 again) and a freed record */
    PstTestSet(exe, 1, (uintptr_t)other, 60.0f);
    PhysicsStepsTick(0.025f);
    assert(g_pstWorlds[1].h == (uintptr_t)other && g_pstWorlds[1].base == 60.0f && PstTestNear(PstTestRate(exe, 1), 44.0f));
    PstTestSet(exe, 1, 0, 60.0f);
    PhysicsStepsTick(0.025f);
    assert(PstTestRate(exe, 1) == 60.0f && g_pstWorlds[1].h == 0);
    for (int i = 0; i < PST_COUNT; i++) assert(g_pstWorlds[i].h == 0);

    g_pstState = 0;
    g_pstExe = 0;
    g_pstDt = 0;
    g_pstLowered = 0;
    g_pstLowest = 0;
    VirtualFree(exe, 0, MEM_RELEASE);
    puts("PASS physics steps: pins and their refusals, HELLFORGE_PERF_OFF, the game's rate at 54 fps and up, 1.1 / frame time below with the 30 Hz floor, long frames skipped, another rate as the base, one out of range left alone, a new world, a freed record");
}
