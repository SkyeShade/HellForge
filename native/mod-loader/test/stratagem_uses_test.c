/* Uses before cooldown (stratagem_uses.c): the field the stats layers write, the calls counted per player's slot, the
 * cooldown passed on, and Eagles left alone. Included by stats_test.c. */

static uint8_t* g_usesRows[STR_VANILLA];
static uintptr_t UsesFixtureRow(uint32_t type) { return type < STR_VANILLA ? (uintptr_t)g_usesRows[type] : 0; }

static uint8_t* UsesFixtureStratagem(uint32_t type, uint32_t stable, uint32_t delivery)
{
    uint8_t* row = calloc(1, STR_ROW);
    memcpy(row, &type, 4); memcpy(row + 4, &stable, 4); memcpy(row + 60, &delivery, 4);
    return g_usesRows[type] = row;
}

static void StratagemUsesFixture(void)
{
    uintptr_t (*row)(uint32_t) = UsesRow;
    int state = g_usesState;
    UsesRow = UsesFixtureRow;
    g_usesState = 1;
    UsesFixtureStratagem(5, 0x55u, 1);           /* an orbital */
    UsesFixtureStratagem(6, 0x66u, 0);           /* an Eagle */
    UsesFixtureStratagem(SUSES_EAGLE_REARM, 0x31u, 7);

    /* the field: by stable id, unset reads 1; Eagles and Eagle Rearm have none */
    StatEdit edit = {&g_mods[0], "StratagemUses", "", "fixture uses"};
    edit.record = 0x55u; edit.size = 4; edit.ref = -1;
    const char* why = NULL;
    uintptr_t at = UsesField(&edit, &why);
    assert(at == (uintptr_t)&g_stratagemUses[5] && g_stratagemUses[5] == 1 && UsesOf(5) == 1);
    edit.record = 0x66u;
    assert(UsesField(&edit, &why) == 0 && strstr(why, "non-Eagle"));
    edit.record = 0x31u;
    assert(UsesField(&edit, &why) == 0);
    int ready = 0;
    assert(UsesRowOf(5, &ready) == (uintptr_t)&g_stratagemUses[5] && ready && UsesRowOf(6, &ready) == 0);

    /* the stats pass: 0 and past 99 refused, 3 taken */
    edit.record = 0x55u; edit.value = 0;
    StatsWrite(&edit, at);
    assert(!edit.applied && g_stratagemUses[5] == 1);
    edit.value = 100;
    StatsWrite(&edit, at);
    assert(!edit.applied);
    edit.value = 3;
    StatsWrite(&edit, at);
    assert(edit.applied && edit.original == 1 && UsesOf(5) == 3);

    /* three calls: the first two pass the burst cooldown, the third the game's, then it starts over; each player's slot
     * counts on its own, a slot that changed stratagem starts over */
    memset(g_usesCounters, 0, sizeof g_usesCounters);
    uintptr_t players = 0x1000;
    assert(UsesCooldownFor(players, 7, 2, 5, -1.0f) == SUSES_BURST_COOLDOWN);
    assert(UsesCooldownFor(players, 8, 2, 5, -1.0f) == SUSES_BURST_COOLDOWN); /* another player */
    assert(UsesCooldownFor(players, 7, 2, 5, -1.0f) == SUSES_BURST_COOLDOWN);
    assert(UsesCooldownFor(players, 7, 2, 5, -1.0f) == -1.0f);
    assert(UsesCooldownFor(players, 7, 2, 5, -1.0f) == SUSES_BURST_COOLDOWN);
    assert(UsesCooldownFor(players, 7, 2, 6, 12.0f) == 12.0f);                 /* now an Eagle there */
    assert(UsesCooldownFor(players, 7, 3, SUSES_EAGLE_REARM, 90.0f) == 90.0f);
    g_stratagemUses[5] = 1;
    assert(UsesCooldownFor(players, 9, 0, 5, 30.0f) == 30.0f);                 /* the game's own: one use */

    /* the slot's type, as the cooldown start finds it: players' records 0x1690 apart, the count at +0x2D200 */
    uint8_t* records = calloc(1, 0x2D210);
    int64_t who = 42; uint32_t count = 2, type = 5;
    memcpy(records + 0x1690, &who, 8);
    memcpy(records + 0x1690 + 3 * 0x30 + 0x1C0, &type, 4);
    memcpy(records + 0x2D200, &count, 4);
    assert(UsesSlotType(records, 42, 3) == 5 && UsesSlotType(records, 43, 3) == 0);
    free(records);

    for (uint32_t t = 0; t < STR_VANILLA; t++) { free(g_usesRows[t]); g_usesRows[t] = NULL; }
    memset(g_stratagemUses, 0, sizeof g_stratagemUses);
    memset(g_usesCounters, 0, sizeof g_usesCounters);
    UsesRow = row;
    g_usesState = state;
}
