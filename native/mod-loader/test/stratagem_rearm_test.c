/* Rearms after uses (stratagem_rearm.c): the tick the stats layers write, the cooldown passed on while uses are left,
 * the refill once the cooldown has ended, and Eagles, Eagle Rearm and team cooldowns left alone. Included by
 * stats_test.c. */

static uint8_t* g_rearmRows[STR_VANILLA];
static uintptr_t RearmFixtureRow(uint32_t type) { return type < STR_VANILLA ? (uintptr_t)g_rearmRows[type] : 0; }
static uint32_t g_rearmFixtureUses;
static uint32_t RearmFixtureUsesFor(uintptr_t customization, int64_t player, uint32_t type, char flag)
{
    (void)customization; (void)player; (void)type; (void)flag;
    return g_rearmFixtureUses;
}

static void RearmFixtureStratagem(uint32_t type, uint32_t stable, uint32_t delivery, uint32_t cooldownType)
{
    uint8_t* row = calloc(1, STR_ROW);
    memcpy(row, &type, 4); memcpy(row + 4, &stable, 4); memcpy(row + 60, &delivery, 4); memcpy(row + 148, &cooldownType, 4);
    g_rearmRows[type] = row;
}

/* A players' record list with player 42's slot 3 holding type with `left` uses, its cooldown ending at `end`. */
static uint8_t* RearmFixturePlayers(uint32_t type, uint32_t left, uint64_t end)
{
    uint8_t* players = calloc(1, 0x2D210);
    int64_t who = 42; uint32_t count = 2;
    uint8_t* entry = players + 0x1690 + 3 * 0x30 + 0x1C0;
    memcpy(players + 0x1690, &who, 8);
    memcpy(entry, &type, 4); memcpy(entry + 4, &left, 4); memcpy(entry + 0x18, &end, 8);
    memcpy(players + 0x2D200, &count, 4);
    return players;
}

static void StratagemRearmFixture(void)
{
    uintptr_t (*row)(uint32_t) = RearmRow;
    RearmUsesFn usesFor = g_rearmUsesFor;
    int state = g_rearmState;
    RearmRow = RearmFixtureRow;
    g_rearmUsesFor = RearmFixtureUsesFor;
    g_rearmState = 1;
    RearmFixtureStratagem(5, 0x55u, 1, 0);           /* an orbital */
    RearmFixtureStratagem(6, 0x66u, 0, 0);           /* an Eagle */
    RearmFixtureStratagem(7, 0x77u, 2, 2);           /* a team cooldown */
    RearmFixtureStratagem(SREARM_EAGLE_REARM, 0x31u, 7, 0);

    /* the field: by stable id; Eagles, Eagle Rearm and team cooldowns have none */
    StatEdit edit = {&g_mods[0], "StratagemRearm", "", "fixture rearm"};
    edit.record = 0x55u; edit.size = 4; edit.ref = -1;
    const char* why = NULL;
    uintptr_t at = RearmField(&edit, &why);
    assert(at == (uintptr_t)&g_stratagemRearm[5] && !RearmOn(5));
    edit.record = 0x66u;
    assert(RearmField(&edit, &why) == 0 && strstr(why, "Eagles"));
    edit.record = 0x77u;
    assert(RearmField(&edit, &why) == 0 && strstr(why, "team cooldown"));
    edit.record = 0x31u;
    assert(RearmField(&edit, &why) == 0);
    int ready = 0;
    assert(RearmRowOf(5, &ready) == (uintptr_t)&g_stratagemRearm[5] && ready && RearmRowOf(6, &ready) == 0);

    /* the stats pass: 2 refused, 1 taken */
    edit.record = 0x55u; edit.value = 2;
    StatsWrite(&edit, at);
    assert(!edit.applied && !RearmOn(5));
    edit.value = 1;
    StatsWrite(&edit, at);
    assert(edit.applied && edit.original == 0 && RearmOn(5));

    /* while uses are left (or unlimited) the burst cooldown, spent the game's and the slot noted; not ticked, the game's */
    memset(g_rearmPending, 0, sizeof g_rearmPending);
    uint8_t* players = RearmFixturePlayers(5, 2, 0);
    uintptr_t entry = RearmSlot((uintptr_t)players, 42, 3);
    assert(entry == (uintptr_t)(players + 0x1690 + 3 * 0x30 + 0x1C0) && RearmSlot((uintptr_t)players, 43, 3) == 0);
    assert(RearmCooldownFor(entry, (uintptr_t)players, 42, 3, -1.0f) == SREARM_BURST_COOLDOWN && g_rearmPending[0].players == 0);
    uint32_t left = 0xFFFFFFFFu;
    memcpy((void*)(entry + 4), &left, 4);
    assert(RearmCooldownFor(entry, (uintptr_t)players, 42, 3, -1.0f) == SREARM_BURST_COOLDOWN); /* unlimited: no cooldown */
    left = 0;
    memcpy((void*)(entry + 4), &left, 4);
    assert(RearmCooldownFor(entry, (uintptr_t)players, 42, 3, -1.0f) == -1.0f);
    assert(g_rearmPending[0].players == (uintptr_t)players && g_rearmPending[0].player == 42 && g_rearmPending[0].slot == 3 && g_rearmPending[0].type == 5);
    assert(RearmCooldownFor(entry, (uintptr_t)players, 42, 3, -1.0f) == -1.0f && g_rearmPending[1].players == 0); /* noted once */

    /* the refill: not while the cooldown runs; after it, the game's count; a slot that changed stratagem is let go */
    uint64_t end = 5000000;
    memcpy((void*)(entry + 0x18), &end, 8);
    g_rearmFixtureUses = 3;
    assert(RearmRefill(0x1000, 0, 4999999) == 0);
    memcpy(&left, (void*)(entry + 4), 4);
    assert(left == 0);
    assert(RearmRefill(0x1000, 0, 5000000) == 1);
    memcpy(&left, (void*)(entry + 4), 4);
    assert(left == 3);
    left = 0;
    memcpy((void*)(entry + 4), &left, 4);
    uint32_t other = 6;
    memcpy((void*)entry, &other, 4);
    assert(RearmRefill(0x1000, 0, 9000000) == 1);
    memcpy(&left, (void*)(entry + 4), 4);
    assert(left == 0);
    assert(RearmCooldownFor(entry, (uintptr_t)players, 42, 3, 12.0f) == 12.0f); /* an Eagle there now: the game's */
    g_stratagemRearm[5] = 0;
    memcpy((void*)entry, &(uint32_t){5}, 4);
    assert(RearmCooldownFor(entry, (uintptr_t)players, 42, 3, 30.0f) == 30.0f); /* not ticked: the game's own */
    free(players);

    for (uint32_t t = 0; t < STR_VANILLA; t++) { free(g_rearmRows[t]); g_rearmRows[t] = NULL; }
    memset(g_stratagemRearm, 0, sizeof g_stratagemRearm);
    memset(g_rearmPending, 0, sizeof g_rearmPending);
    RearmRow = row;
    g_rearmUsesFor = usesFor;
    g_rearmState = state;
}
