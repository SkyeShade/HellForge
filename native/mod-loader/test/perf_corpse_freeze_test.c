/* The corpse freeze (perf_corpse_freeze.c) on synthetic images: game.dll and exe images hold the pinned bytes, so the
 * pins pass and refuse a changed byte. Once on, the exe's five calls become jumps to stubs that keep the units' state,
 * and the real tables, ring and pose sync manager are built in memory. Included by stats_test.c. */

#define CF_GAME_IMAGE 0x3470000u
#define CF_EXE_IMAGE 0x7E2000u

typedef struct CfTestUnit
{
    uint32_t id;
    int alive;
    float pos[3];
    uint32_t actors[3], ragdolls[2];
    int positionCalls, handleCalls;
} CfTestUnit;
static CfTestUnit g_cfUnits[8];
static int g_cfActorCalls, g_cfLevelCalls, g_cfLastValue;
static uint32_t g_cfLastId, g_cfLastCount;

static CfTestUnit* CfTestFind(uint32_t id)
{
    for (int i = 0; i < 8; i++) if (g_cfUnits[i].id == id) return &g_cfUnits[i];
    assert(!"a unit the test never made");
    return NULL;
}

static int CfTestHandles(uint32_t unit, uint32_t mask, uint8_t* out)
{
    CfTestUnit* u = CfTestFind(unit);
    assert(mask == 5 && u->alive);
    u->handleCalls++;
    memset(out, 0, 0x60);
    uint64_t actors = (uintptr_t)u->actors, ragdolls = (uintptr_t)u->ragdolls;
    uint32_t actorCount = 3, ragdollCount = 2;
    memcpy(out, &actors, 8); memcpy(out + 0x38, &actorCount, 4); memcpy(out + 0x10, &ragdolls, 8); memcpy(out + 0x40, &ragdollCount, 4);
    return 1;
}
static uint8_t CfTestAlive(uint32_t unit) { return (uint8_t)CfTestFind(unit)->alive; }
static const float* CfTestPosition(uint32_t unit, int node)
{
    CfTestUnit* u = CfTestFind(unit);
    assert(node == 0 && u->alive); /* never asked of a unit that is gone */
    u->positionCalls++;
    return u->pos;
}
static void CfTestActors(const uint32_t* ids, uint32_t count, int value) { g_cfActorCalls++; g_cfLastId = ids[0]; g_cfLastCount = count; g_cfLastValue = value; }
static void CfTestLevel(const uint32_t* ids, uint32_t count, int value) { g_cfLevelCalls++; g_cfLastId = ids[0]; g_cfLastCount = count; g_cfLastValue = value; }

/* mov rax, to; jmp rax */
static void CfTestJump(uint8_t* at, void* to)
{
    uint64_t target = (uint64_t)(uintptr_t)to;
    at[0] = 0x48; at[1] = 0xb8;
    memcpy(at + 2, &target, 8);
    at[10] = 0xff; at[11] = 0xe0;
}

static void CfTestEntry(uint8_t* service, int i, uint32_t unit, uint32_t state, uint8_t keepActors)
{
    uint8_t* e = service + 0x18 + 0x40 * i;
    memcpy(e, &unit, 4); memcpy(e + 8, &state, 4); e[0x38] = keepActors;
}

static void CfTestUnits(void)
{
    memset(g_cfUnits, 0, sizeof g_cfUnits);
    for (int i = 0; i < 8; i++)
    {
        g_cfUnits[i].id = 0x101 + i;
        g_cfUnits[i].alive = 1;
        for (int k = 0; k < 3; k++) g_cfUnits[i].actors[k] = 0xA000 + 0x10 * i + k;
        for (int k = 0; k < 2; k++) g_cfUnits[i].ragdolls[k] = 0xB000 + 0x10 * i + k;
    }
    g_cfUnits[3].alive = 0; /* 0x104 is gone */
}

/* A frame, with 0x102 a metre further on: it never settles. */
static void CfTestStep(ULONGLONG now)
{
    g_cfUnits[1].pos[0] += 1.0f;
    CfFrame(now);
}

static void PerfCorpseFreezeFixture(void)
{
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, CF_GAME_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t* exe = (uint8_t*)VirtualAlloc(NULL, CF_EXE_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(game != NULL && exe != NULL);
    for (size_t i = 0; i < CF_PINS; i++) memcpy((kCfPins[i].exe ? exe : game) + kCfPins[i].rva, kCfPins[i].bytes, kCfPins[i].length);
    assert(CfPinned((uintptr_t)game, (uintptr_t)exe));
    for (size_t i = 0; i < CF_PINS; i++) /* every pin refuses another build's byte */
    {
        uint8_t* at = (kCfPins[i].exe ? exe : game) + kCfPins[i].rva + kCfPins[i].length - 1;
        *at ^= 0x40;
        assert(!CfPinned((uintptr_t)game, (uintptr_t)exe));
        *at ^= 0x40;
    }

    memset(&g_cf, 0, sizeof g_cf);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "safe-read, corpse-freeze");
    CfInstallAt((uintptr_t)game, (uintptr_t)exe);
    assert(g_cf.state == -1);
    PerfCorpseFreezeTick(); /* off: reads nothing */
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    memset(&g_cf, 0, sizeof g_cf);
    CfInstallAt((uintptr_t)game, (uintptr_t)exe);
    assert(g_cf.state == 1 && g_cf.game == (uintptr_t)game && g_cf.exe == (uintptr_t)exe);

    CfTestJump(exe + 0x7E1C20u, (void*)CfTestHandles);
    CfTestJump(exe + 0x1FEEB0u, (void*)CfTestAlive);
    CfTestJump(exe + 0x1FD190u, (void*)CfTestPosition);
    CfTestJump(exe + 0x77F4F0u, (void*)CfTestActors);
    CfTestJump(exe + 0x7A3D00u, (void*)CfTestLevel);
    FlushInstructionCache(GetCurrentProcess(), exe, CF_EXE_IMAGE);

    CfFrame(1000);
    assert(g_cf.state == 1 && !g_cf.bound); /* the tables are still empty: tried again later */

    static uintptr_t unitApi[1], engineApi[4], scene[0x100], actorApi[2], ragdollApi[10];
    uintptr_t at;
    at = (uintptr_t)unitApi; memcpy(game + CF_UNIT_API, &at, 8);
    at = (uintptr_t)engineApi; memcpy(game + CF_ENGINE_API, &at, 8);
    at = (uintptr_t)actorApi; memcpy(game + CF_ACTOR_API, &at, 8);
    at = (uintptr_t)ragdollApi; memcpy(game + CF_RAGDOLL_API, &at, 8);
    engineApi[3] = (uintptr_t)scene;
    unitApi[0] = (uintptr_t)exe + 0x7E1C20u;
    scene[0x720 / 8] = (uintptr_t)exe + 0x1FEEB0u;
    scene[0x88 / 8] = (uintptr_t)exe + 0x1FD190u;
    actorApi[1] = (uintptr_t)exe + 0x77F4F1u; /* not the reviewed call */
    ragdollApi[9] = (uintptr_t)exe + 0x7A3D00u;
    CfFrame(1000);
    assert(g_cf.state == -1 && !g_cf.bound);
    actorApi[1] = (uintptr_t)exe + 0x77F4F0u;
    InterlockedExchange(&g_cf.state, 1);
    CfFrame(1000);
    assert(g_cf.bound && g_cf.settled > 0.0999f && g_cf.settled < 0.1001f);

    /* the ring: settled, moving, still converting, gone, settled keeping its actors, settled but still posed */
    static uint8_t service[0x18 + 0x40 * CF_RING];
    memset(service, 0, sizeof service);
    at = (uintptr_t)service; memcpy(game + CF_SERVICE, &at, 8);
    CfTestUnits();
    CfTestEntry(service, 0, 0x101, 2, 0);
    CfTestEntry(service, 1, 0x102, 2, 0);
    CfTestEntry(service, 2, 0x103, 1, 0);
    CfTestEntry(service, 3, 0x104, 2, 0);
    CfTestEntry(service, 4, 0x105, 2, 1);
    CfTestEntry(service, 5, 0x106, 2, 0);
    static uintptr_t entities[3];
    static uint8_t records[3][24], runtime[3 * 0x2BB8], manager[0x60];
    uint32_t unit = 0x106, other = 0x109, count = 3;
    memcpy(records[0] + 0xC, &other, 4); memcpy(records[2] + 0xC, &unit, 4);
    entities[0] = (uintptr_t)records[0]; entities[1] = 0; entities[2] = (uintptr_t)records[2];
    runtime[0 * 0x2BB8 + 0x2BA4] = 1; runtime[2 * 0x2BB8 + 0x2BA4] = 1;
    memcpy(manager + 0x10, &count, 4);
    at = (uintptr_t)entities; memcpy(manager + 0x38, &at, 8);
    at = (uintptr_t)runtime; memcpy(manager + 0x48, &at, 8);
    at = (uintptr_t)manager; memcpy(game + CF_POSE_SYNC, &at, 8);

    CfFrame(5000); /* the first sample of each */
    assert(g_cfActorCalls == 0 && g_cfLevelCalls == 0);
    assert(g_cfUnits[0].positionCalls == 1 && g_cfUnits[2].positionCalls == 0 && g_cfUnits[3].positionCalls == 0);
    g_cfUnits[1].pos[0] += 1.0f; /* the moving one: 1 m^2 in a second */
    g_cfUnits[0].pos[2] += 0.2f; /* 0.04 m^2: settled */
    CfFrame(5500); /* under a second: no sample */
    assert(g_cfUnits[0].positionCalls == 1);
    CfFrame(6000);
    assert(g_cfUnits[0].handleCalls == 1 && g_cfUnits[1].handleCalls == 0 && g_cfUnits[3].handleCalls == 0);
    assert(g_cfUnits[4].handleCalls == 1 && g_cfUnits[5].handleCalls == 0 && g_cf.posed == 1);
    assert(g_cfActorCalls == 1 && g_cfLevelCalls == 2); /* 0x105 keeps its actors */
    assert(g_cfLastId == 0xB040 && g_cfLastCount == 2 && g_cfLastValue == 3);
    assert(g_cf.frozen == 2 && g_cf.slot[0].done && g_cf.slot[4].done);

    runtime[2 * 0x2BB8 + 0x2BA4] = 0; /* the pose sync let go of 0x106 */
    CfTestStep(7000);
    assert(g_cfUnits[5].handleCalls == 1 && g_cf.frozen == 3 && g_cfActorCalls == 2);
    assert(g_cfLastId == 0xB050 && g_cfLastValue == 3);
    CfTestStep(8000); /* a frozen corpse is not frozen again */
    assert(g_cfUnits[0].handleCalls == 1 && g_cfUnits[0].positionCalls == 2 && g_cf.frozen == 3);

    /* the slot's corpse evicted: the next one starts its own samples */
    CfTestEntry(service, 0, 0x107, 2, 0);
    CfTestStep(9000);
    assert(g_cfUnits[6].positionCalls == 1 && g_cfUnits[6].handleCalls == 0);
    CfTestStep(10000);
    assert(g_cfUnits[6].handleCalls == 1 && g_cf.frozen == 4);
    assert(!g_cf.slot[2].unit && g_cfUnits[2].positionCalls == 0); /* converting: never asked */

    /* no service: nothing read */
    memset(game + CF_SERVICE, 0, 8);
    int before = g_cfUnits[1].positionCalls;
    CfFrame(20000);
    assert(g_cfUnits[1].positionCalls == before);

    memset(&g_cf, 0, sizeof g_cf);
    VirtualFree(exe, 0, MEM_RELEASE);
    VirtualFree(game, 0, MEM_RELEASE);
    printf("corpse freeze: pins, the switch, the settle test, gone and posed units, kept actors pass\n");
}
