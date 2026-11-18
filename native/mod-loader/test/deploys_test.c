/* Per-player deployables (deploys.c): the lobby's changes from the pipe, a friend's relay given their values with this
 * machine's own edit undone, two friends' copies of one type at once, and what stays the game's: this machine's own
 * entities, those it has authority over, and types nobody edited. Included by stats_test.c, after rounds_test.c. */

#define DEPLOY_TEST_RELAY 0xED13DDC480EC6910ull
#define DEPLOY_TEST_OTHER 0x1234ull

static uint8_t g_deployTestShield[344];
static uint64_t g_deployTestOwner;
static void* DeployFakeResolve(void* descriptor)
{
    uint64_t resource;
    memcpy(&resource, descriptor, 8);
    return g_deployTestShield;
}
static uint64_t DeployFakeCreator(uint32_t key) { return key >= 100 ? g_deployTestOwner : 0; }
static uint64_t DeployFakeLocal(void) { return 0x110000100000011ull; }

static void DeployTestSpawn(uint32_t entity, uint64_t resource, uint8_t authority)
{
    uint8_t row[0x18] = {0}, info[8] = {0};
    memcpy(row, &resource, 8);
    memcpy(row + 8, &entity, 4);
    info[0] = authority;
    DeployEntity(row, info);
}

/* What a resolver gives for an entity: the radius at +0 */
static float DeployTestRadius(uint32_t entity, uint64_t resource)
{
    uint8_t descriptor[16] = {0};
    memcpy(descriptor, &resource, 8);
    memcpy(descriptor + 8, &entity, 4);
    float radius;
    memcpy(&radius, DeployShield(descriptor), 4);
    return radius;
}

static void DeploysFixture(void)
{
    char line[600], reply[256];
    const char* why;
    const float mine = 20.0f, stock = 15.0f;
    memcpy(g_deployTestShield, &mine, 4); /* this machine's own edit: 20 m over the game's 15 */
    DeployResolveFn original = g_deployKinds[DEPLOY_SHIELD].original;
    uint64_t (*creator)(uint32_t) = g_deployCreator;
    uint64_t (*local)(void) = g_deployLocal;
    g_deployKinds[DEPLOY_SHIELD].original = DeployFakeResolve;
    g_deployCreator = DeployFakeCreator;
    g_deployLocal = DeployFakeLocal;
    HfMod localStats = {0};
    localStats.api = &g_api; strcpy(localStats.id, "zz.local.stats"); localStats.loaded = 1; localStats.log = INVALID_HANDLE_VALUE;
    int stats = g_statCount;
    memset(&g_stats[g_statCount], 0, sizeof g_stats[0]);
    g_stats[g_statCount].mod = &localStats;
    g_stats[g_statCount].applied = 1;
    g_stats[g_statCount].address = (uintptr_t)g_deployTestShield;
    memcpy(&g_stats[g_statCount].original, &stock, 4);
    g_statCount++;

    /* the pipe: the lobby's changes replace the last, "-" none, malformed refused */
    strcpy(line, "deploys\t1\t110000100000022=Eed13ddc480ec6910/S+0=41000000;Eed13ddc480ec6910/S+4c=45fa0000,110000100000033=Eed13ddc480ec6910/S+0=41700000,110000100000011=Eed13ddc480ec6910/S+0=41a00000");
    LiveRequest(line, reply, sizeof reply);
    assert(!strncmp(reply, "1\tok", 4) && g_deployEntryCount == 4);
    assert(!DeploysSetLobby("110000100000022=Eed13ddc480ec6910/X+0=1", &why));
    assert(!DeploysSetLobby("110000100000022=Eed13ddc480ec6910/S+4=", &why));
    assert(!DeploysSetLobby("110000100000022=Eed13ddc480ec6910/S+2=1", &why));
    assert(!DeploysSetLobby("110000100000022=Eed13ddc480ec6910/S+158=1", &why) && g_deployEntryCount == 4);

    /* a friend's relay holds their 8 m on this machine, and another friend's their 15 m, at the same time */
    g_deployTestOwner = 0x110000100000022ull;
    DeployTestSpawn(200, DEPLOY_TEST_RELAY, 0);
    g_deployTestOwner = 0x110000100000033ull;
    DeployTestSpawn(201, DEPLOY_TEST_RELAY, 0);
    assert(DeployTestRadius(200, DEPLOY_TEST_RELAY) == 8.0f);
    assert(DeployTestRadius(201, DEPLOY_TEST_RELAY) == 15.0f);
    /* a friend who edited nothing of it plays with the game's value, not this machine's */
    g_deployTestOwner = 0x110000100000044ull;
    DeployTestSpawn(202, DEPLOY_TEST_RELAY, 0);
    assert(DeployTestRadius(202, DEPLOY_TEST_RELAY) == 15.0f);
    /* the game's record is untouched, and every other entity reads it: this machine's own, with authority, unknown */
    assert(DeployTestRadius(999, DEPLOY_TEST_RELAY) == 20.0f && !memcmp(g_deployTestShield, &mine, 4));
    DeployTestSpawn(203, DEPLOY_TEST_RELAY, 1);
    assert(DeployTestRadius(203, DEPLOY_TEST_RELAY) == 20.0f);
    g_deployTestOwner = DeployFakeLocal();
    DeployTestSpawn(204, DEPLOY_TEST_RELAY, 0);
    assert(DeployTestRadius(204, DEPLOY_TEST_RELAY) == 20.0f);
    g_deployTestOwner = 0; /* no creator on the entity: as the game has it */
    DeployTestSpawn(205, DEPLOY_TEST_RELAY, 0);
    assert(DeployTestRadius(205, DEPLOY_TEST_RELAY) == 20.0f);
    g_deployTestOwner = 0x110000100000022ull;
    DeployTestSpawn(206, DEPLOY_TEST_OTHER, 0); /* a type nobody edited */
    assert(DeployTestRadius(206, DEPLOY_TEST_OTHER) == 20.0f);
    /* an id that comes back is a new entity */
    g_deployTestOwner = DeployFakeLocal();
    DeployTestSpawn(200, DEPLOY_TEST_RELAY, 0);
    assert(DeployTestRadius(200, DEPLOY_TEST_RELAY) == 20.0f);
    /* more of them than the kind keeps: the oldest go, nothing overruns */
    g_deployTestOwner = 0x110000100000022ull;
    for (uint32_t i = 0; i < DEPLOY_COPIES + 3; i++) DeployTestSpawn(300 + i, DEPLOY_TEST_RELAY, 0);
    assert(DeployTestRadius(300, DEPLOY_TEST_RELAY) == 20.0f && DeployTestRadius(300 + DEPLOY_COPIES + 2, DEPLOY_TEST_RELAY) == 8.0f);
    /* the lobby's list ends: every copy goes */
    assert(DeploysSetLobby("-", &why) && g_deployEntryCount == 0 && g_deployKinds[DEPLOY_SHIELD].live == 0);
    assert(DeployTestRadius(300 + DEPLOY_COPIES + 2, DEPLOY_TEST_RELAY) == 20.0f);

    g_statCount = stats;
    g_deployKinds[DEPLOY_SHIELD].original = original;
    g_deployCreator = creator;
    g_deployLocal = local;
}
