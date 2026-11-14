/* The AI path budget (perf_ai_path_budget.c) on synthetic exe and game.dll images that hold the pinned bytes and the
 * engine's AI world records: the low budget, the game's back while requests wait and the hysteresis, a value of someone
 * else's left alone, a cleared record and a new world, the pins refusing changed code and HELLFORGE_PERF_OFF.
 * Included by stats_test.c. */

#define AIB_TEST_EXE 0x1910000u
#define AIB_TEST_GAME 0x23C7000u

static float AibTestBudget(const uint8_t* world) { float f; memcpy(&f, world + AIB_BUDGET, 4); return f; }
static void AibTestSet(uint8_t* world, float budget, float waiting, float running)
{
    memcpy(world + AIB_BUDGET, &budget, 4);
    memcpy(world + AIB_WAITED, &waiting, 4);
    memcpy(world + AIB_WAITED + 4, &running, 4);
}

static void PerfAiPathBudgetFixture(void)
{
    uint8_t* exe = (uint8_t*)VirtualAlloc(NULL, AIB_TEST_EXE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, AIB_TEST_GAME, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(exe != NULL && game != NULL);
    for (size_t i = 0; i < AIB_PINS; i++) memcpy((kAibPins[i].game ? game : exe) + kAibPins[i].rva, kAibPins[i].bytes, kAibPins[i].size);
    float stock;
    memcpy(&stock, game + 0x23C65D8u, 4);
    assert(stock == AIB_STOCK); /* the pinned constant is the value the code compares with */

    /* each pin refuses its own changed byte, until the minute is up */
    assert(AiPathBudgetMismatch((uintptr_t)exe, (uintptr_t)game) == NULL);
    for (size_t i = 0; i < AIB_PINS; i++)
    {
        uint8_t* at = (kAibPins[i].game ? game : exe) + kAibPins[i].rva + kAibPins[i].size - 1;
        *at ^= 0x10;
        assert(AiPathBudgetMismatch((uintptr_t)exe, (uintptr_t)game) == kAibPins[i].what);
        if (i == 0) assert(AiPathBudgetInstallAt((uintptr_t)exe, (uintptr_t)game, 0) == -1 && AiPathBudgetInstallAt((uintptr_t)exe, (uintptr_t)game, 1) == 0);
        *at ^= 0x10;
    }
    SetEnvironmentVariableW(L"HELLFORGE_PERF_OFF", L"safe-read, ai-path-budget");
    assert(AiPathBudgetInstallAt((uintptr_t)exe, (uintptr_t)game, 1) == 0 && g_aibExe == 0);
    SetEnvironmentVariableW(L"HELLFORGE_PERF_OFF", NULL);
    assert(AiPathBudgetInstallAt((uintptr_t)exe, (uintptr_t)game, 0) == 1 && g_aibExe == (uintptr_t)exe);

    /* a world in record 1 (the game's slot comes from a ring, not always 2) */
    _Alignas(16) static uint8_t world[0x500], other[0x500];
    uintptr_t* records = (uintptr_t*)(exe + AIB_RECORDS);
    records[AIB_RECORD_SIZE / 8] = (uintptr_t)world;
    AibTestSet(world, AIB_STOCK, 0, 0);
    AiPathBudgetTick();
    assert(AibTestBudget(world) == AIB_STOCK); /* off until the watch turns it on */
    g_aibState = 1;
    LONG raises = g_aibRaises;
    AiPathBudgetTick();
    assert(AibTestBudget(world) == AIB_LOW);
    AibTestSet(world, AIB_LOW, 0.3f, 0.6f); /* the run in flight is 600 ms old */
    AiPathBudgetTick();
    assert(AibTestBudget(world) == AIB_STOCK && g_aibRaises == raises + 1);
    AibTestSet(world, AIB_STOCK, 0.3f, 0); /* 300 ms: between the two, it stays */
    AiPathBudgetTick();
    assert(AibTestBudget(world) == AIB_STOCK && g_aibWorlds[1].raised);
    AibTestSet(world, AIB_STOCK, 0.2f, 0.1f);
    AiPathBudgetTick();
    assert(AibTestBudget(world) == AIB_LOW && !g_aibWorlds[1].raised);
    float nan = NAN;
    AibTestSet(world, AIB_LOW, 0, 0);
    memcpy(world + AIB_WAITED, &nan, 4); /* a wait it cannot read: the game's budget */
    AiPathBudgetTick();
    assert(AibTestBudget(world) == AIB_STOCK && g_aibRaises == raises + 2);
    AibTestSet(world, 0.004f, 0, 0); /* another mod's value */
    AiPathBudgetTick();
    assert(AibTestBudget(world) == 0.004f);

    /* a new world in the same record starts low; a cleared record is forgotten and nothing is written */
    AibTestSet(world, AIB_STOCK, 0.9f, 0);
    AiPathBudgetTick();
    assert(g_aibWorlds[1].raised);
    records[AIB_RECORD_SIZE / 8] = (uintptr_t)other;
    AibTestSet(other, AIB_STOCK, 0, 0);
    AiPathBudgetTick();
    assert(AibTestBudget(other) == AIB_LOW && g_aibWorlds[1].h == (uintptr_t)other && !g_aibWorlds[1].raised);
    records[AIB_RECORD_SIZE / 8] = 0;
    AibTestSet(other, AIB_STOCK, 0, 0);
    AiPathBudgetTick();
    assert(AibTestBudget(other) == AIB_STOCK && g_aibWorlds[1].h == 0);
    for (int i = 0; i < AIB_RECORD_COUNT; i++) assert(g_aibWorlds[i].h == 0);

    g_aibState = 0;
    g_aibExe = 0;
    VirtualFree(exe, 0, MEM_RELEASE);
    VirtualFree(game, 0, MEM_RELEASE);
    puts("PASS ai path budget: pins and their refusals, HELLFORGE_PERF_OFF, the low budget, the game's back over 500 ms and low again under 250 ms, an unreadable wait, another value left alone, a new world, a cleared record");
}
