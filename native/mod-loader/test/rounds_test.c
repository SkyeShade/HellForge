/* Per-player round stats (rounds.c): the lobby's changes from the pipe, every other player's rows rebuilt from this
 * machine's with its own changes undone, and a friend's shot spawned with their projectile row and damage while this
 * machine's player's and no player's keep their own. Included by stats_test.c, after shots_test.c (its wielders). */

static uint8_t g_roundProjectile[PRJ_ROW], g_roundDamage[DMG_ROW];
static uintptr_t RoundTableRow(const char* component, uint64_t id, int* ready)
{
    *ready = 1;
    if (!strcmp(component, "ProjectileSettings") && id == 310) return (uintptr_t)g_roundProjectile;
    if (!strcmp(component, "DamageInfo") && id == 107) return (uintptr_t)g_roundDamage;
    return 0;
}

/* SpawnProjectile's stand-in: what it was given, and a shot in slot 3 as the game makes it (the row's damage id, this
 * machine's damage row's lanes, a damage multiplier of 1) */
static uint8_t g_roundPool[ROUND_HITS + 8 * ROUND_HIT];
static uint32_t g_roundSpawnedType;
static float g_roundSpawnedVelocity;
static uint32_t RoundFakeSpawn(uint8_t* pool, uint8_t* descriptor, uint8_t* extra)
{
    (void)extra;
    uint8_t* row;
    memcpy(&row, descriptor + 0x10, 8);
    memcpy(&g_roundSpawnedType, row, 4);
    memcpy(&g_roundSpawnedVelocity, row + 0x20, 4);
    uint8_t* hit = pool + ROUND_HITS + 3 * ROUND_HIT;
    memcpy(hit + 0x0C, row + 0x3C, 4);
    for (int k = 0; k < 4; k++)
    {
        uint32_t lane;
        memcpy(&lane, g_roundDamage + 0x0C + 4 * k, 4);
        uint16_t l = (uint16_t)lane;
        memcpy(hit + 0x18 + 2 * k, &l, 2);
    }
    float one = 1.0f;
    memcpy(hit + 0x34, &one, 4);
    return 3;
}

static void RoundLanes(uint16_t want)
{
    uint16_t lanes[4];
    memcpy(lanes, g_roundPool + ROUND_HITS + 3 * ROUND_HIT + 0x18, 8);
    for (int k = 0; k < 4; k++) assert(lanes[k] == want);
}

static float RoundScale(void)
{
    float scale;
    memcpy(&scale, g_roundPool + ROUND_HITS + 3 * ROUND_HIT + 0x34, 4);
    return scale;
}

static void RoundsFixture(void)
{
    char line[600], reply[256];
    const char* why;
    /* the pipe: the lobby's changes replace the last, "-" none, malformed refused */
    strcpy(line, "rounds\t1\t110000100000022=D107+c=4;D107+10=4;P310+20=42480000,110000100000011=D107+4=50");
    LiveRequest(line, reply, sizeof reply);
    assert(!strcmp(reply, "1\tok\tnow") && g_roundEntryCount == 4 && g_roundEntries[0].peer == 0x110000100000022ull && g_roundEntries[0].damage &&
        g_roundEntries[0].id == 107 && g_roundEntries[0].offset == 0x0C && g_roundEntries[0].bits == 4 && !g_roundEntries[2].damage &&
        g_roundEntries[2].id == 310 && g_roundEntries[2].offset == 0x20 && g_roundEntries[2].bits == 0x42480000u && g_roundEntries[3].peer == 0x110000100000011ull);
    assert(!RoundsSetLobby("110000100000022=D107+0=4", &why));  /* a row's own id */
    assert(!RoundsSetLobby("110000100000022=P310+10e=4", &why)); /* past the row */
    assert(!RoundsSetLobby("110000100000022=X1+4=1", &why) && !RoundsSetLobby("22=D107+c", &why) && !RoundsSetLobby("=D107+c=4", &why));
    assert(!RoundsSetLobby("110000100000022=D107+6=4", &why) && g_roundEntryCount == 4); /* not a whole field */
    strcpy(line, "rounds\t2\t-"); LiveRequest(line, reply, sizeof reply); assert(!strcmp(reply, "2\tok\tnow") && g_roundEntryCount == 0 && !g_roundsActive);

    /* the rows here: projectile 310 (velocity 100 at +0x20, damage 107 at +0x3C), damage 107 (standard 40, durable 10,
     * AP 3 in every lane: this machine raised it from 2 itself, the Local Stats mod's boot edit of the first lane) */
    memset(g_roundProjectile, 0, sizeof g_roundProjectile);
    memset(g_roundDamage, 0, sizeof g_roundDamage);
    uint32_t u = 310; memcpy(g_roundProjectile, &u, 4);
    float velocity = 100.0f; memcpy(g_roundProjectile + 0x20, &velocity, 4);
    u = 107; memcpy(g_roundProjectile + 0x3C, &u, 4); memcpy(g_roundDamage, &u, 4);
    u = 40; memcpy(g_roundDamage + 4, &u, 4);
    u = 10; memcpy(g_roundDamage + 8, &u, 4);
    u = 3; for (int k = 0; k < 4; k++) memcpy(g_roundDamage + 0x0C + 4 * k, &u, 4);
    HfMod localStats = {0};
    localStats.api = &g_api; strcpy(localStats.id, "zz.local.stats"); localStats.loaded = 1; localStats.log = INVALID_HANDLE_VALUE;
    int stats = g_statCount;
    for (int k = 0; k < 4; k++)
    {
        memset(&g_stats[g_statCount], 0, sizeof g_stats[0]);
        g_stats[g_statCount].mod = &localStats; g_stats[g_statCount].applied = 1;
        g_stats[g_statCount].address = (uintptr_t)(g_roundDamage + 0x0C + 4 * k); g_stats[g_statCount].original = 2;
        g_statCount++;
    }
    /* this machine's player (avatar 100), a friend with changes (F, 200) and one without (G, 999) */
    const uint64_t L = 0x110000100000011ull, F = 0x110000100000022ull, G = 0x110000100000033ull;
    g_shotWielder = ShotFakeWielder;
    g_shotPlayers[0] = (ShotPlayer){L, 100, 1}; g_shotPlayers[1] = (ShotPlayer){F, 200, 0}; g_shotPlayers[2] = (ShotPlayer){G, 999, 0};
    g_shotPlayerCount = 3;
    g_tableRow = RoundTableRow;
    /* F: AP 4 in every lane, standard damage 80, velocity 200; L (this machine) the AP 3 it has */
    assert(RoundsSetLobby("110000100000022=D107+c=4;D107+10=4;D107+14=4;D107+18=4;D107+4=50;P310+20=43480000,"
        "110000100000011=D107+c=3;D107+10=3;D107+14=3;D107+18=3", &why));
    RoundsRebuild();
    /* F gets their projectile row (velocity 200, the stock type kept) and damage row (AP 4, twice the damage); G, who
     * changed nothing, gets the damage row without this machine's change (AP 2) and no projectile row (it is this
     * machine's); this machine's player none */
    float speed;
    memcpy(&speed, g_roundShadows[0].row + 0x20, 4);
    assert(g_roundShadowCount == 1 && g_roundShadows[0].peer == F && g_roundShadows[0].type == 310 && speed == 200.0f &&
        !memcmp(g_roundShadows[0].row, g_roundProjectile, 4) && !memcmp(g_roundShadows[0].row + 0x3C, g_roundProjectile + 0x3C, 4));
    assert(g_roundViewCount == 2 && g_roundsActive);
    for (int i = 0; i < 2; i++)
    {
        const RoundView* v = &g_roundViews[i];
        assert(v->id == 107 && (v->peer == F ? v->lanes[0] == 4 && v->lanes[3] == 4 && v->scale == 2.0f :
            v->peer == G && v->lanes[0] == 2 && v->lanes[3] == 2 && v->scale == 1.0f));
    }

    /* the spawn: F's shot flies with F's row (restored in the descriptor afterwards) and hits with F's AP and damage */
    g_roundSpawn = (uintptr_t)RoundFakeSpawn;
    g_roundPool[ROUND_POOL_ACTIVE] = 1;
    uint8_t descriptor[0x28] = {0};
    uint8_t* row = g_roundProjectile;
    memcpy(descriptor + 0x10, &row, 8);
    uint32_t weapon = 22; memcpy(descriptor + 0x18, &weapon, 4);
    LONG shots = g_roundShots;
    assert(RoundSpawn(g_roundPool, descriptor, NULL) == 3 && g_roundSpawnedType == 310 && g_roundSpawnedVelocity == 200.0f);
    uint8_t* after; memcpy(&after, descriptor + 0x10, 8);
    assert(after == g_roundProjectile && RoundScale() == 2.0f && g_roundShots == shots + 1);
    RoundLanes(4);
    /* this machine's player's: its own row and lanes */
    weapon = 11; memcpy(descriptor + 0x18, &weapon, 4);
    assert(RoundSpawn(g_roundPool, descriptor, NULL) == 3 && g_roundSpawnedVelocity == 100.0f && RoundScale() == 1.0f);
    RoundLanes(3);
    /* G's: this machine's projectile row, the damage without this machine's change */
    weapon = 33; memcpy(descriptor + 0x18, &weapon, 4);
    assert(RoundSpawn(g_roundPool, descriptor, NULL) == 3 && g_roundSpawnedVelocity == 100.0f && RoundScale() == 1.0f);
    RoundLanes(2);
    /* no player's (a weapon nobody wields): left alone */
    weapon = 44; memcpy(descriptor + 0x18, &weapon, 4);
    assert(RoundSpawn(g_roundPool, descriptor, NULL) == 3 && g_roundSpawnedVelocity == 100.0f);
    RoundLanes(3);
    /* an inactive pool spawns nothing: the shot is not touched */
    g_roundPool[ROUND_POOL_ACTIVE] = 0;
    weapon = 22; memcpy(descriptor + 0x18, &weapon, 4);
    RoundSpawn(g_roundPool, descriptor, NULL);
    RoundLanes(3);

    /* the lobby has none: nothing is changed any more */
    assert(RoundsSetLobby("-", &why) && !g_roundsActive && g_roundShadowCount == 0 && g_roundViewCount == 0);
    g_roundPool[ROUND_POOL_ACTIVE] = 1;
    assert(RoundSpawn(g_roundPool, descriptor, NULL) == 3 && g_roundSpawnedVelocity == 100.0f);
    RoundLanes(3);
    g_statCount = stats;
    g_tableRow = TableRowOf;
    g_shotPlayerCount = 0;
    g_shotWielder = ShotWielderOf;
    g_roundSpawn = 0;
}
