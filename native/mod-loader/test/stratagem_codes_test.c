/* Stratagem input codes (stratagem_codes.c): the arrays HellForge points the rows at, the packed field the stats layers
 * write, the game-thread update, and the pins that refuse it all on another build. Included by stats_test.c. */

static uint8_t* CodesFixtureGame(void)
{
    static const uint32_t pages[] = {0x66D000u, 0x66E000u, 0xA10000u, 0xA90000u, 0x1836000u, 0x1837000u, 0x17EF000u};
    uint8_t* game = VirtualAlloc(NULL, 0x1840000, MEM_RESERVE, PAGE_NOACCESS);
    assert(game != NULL);
    for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++) assert(VirtualAlloc(game + pages[i], 0x1000, MEM_COMMIT, PAGE_READWRITE) != NULL);
    for (size_t i = 0; i < sizeof kCodePins / sizeof kCodePins[0]; i++) memcpy(game + kCodePins[i].rva, kCodePins[i].bytes, kCodePins[i].length);
    return game;
}

static uint8_t* CodesFixtureRow(uint64_t* table, uint32_t type, uint32_t stable, const uint32_t* code, uint32_t length)
{
    uint8_t* row = calloc(1, STR_ROW);
    uint32_t* directions = calloc(16, 4);
    memcpy(directions, code, length * 4);
    uint64_t at = (uintptr_t)directions;
    memcpy(row, &type, 4); memcpy(row + 4, &stable, 4);
    memcpy(row + SCODE_ROW_CODE, &at, 8); memcpy(row + SCODE_ROW_LENGTH, &length, 4);
    table[type] = (uintptr_t)row;
    return row;
}

static void StratagemCodesFixture(void)
{
    /* packing: a base-5 digit per direction, the first lowest; 1 to 10 directions of 1..4 */
    const uint32_t urd[3] = {1, 2, 3}, ten[10] = {1, 2, 3, 4, 1, 2, 3, 4, 1, 2}, eleven[11] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    uint32_t back[SCODE_MAX];
    assert(CodePack(urd, 3) == 1 + 2 * 5 + 3 * 25 && CodeUnpack(86, back) == 3 && back[0] == 1 && back[2] == 3);
    assert(CodePack(ten, 10) != 0 && CodeUnpack(CodePack(ten, 10), back) == 10 && CodePack(eleven, 11) == 0 && !CodeValid(12207031u) && CodeValid(2441406u));
    uint32_t bad[2] = {1, 5};
    assert(CodePack(bad, 2) == 0 && !CodeValid(0) && !CodeValid(5) && CodeValid(4) && !CodeValid(UINT32_MAX));

    uint8_t* game = CodesFixtureGame();
    uint64_t* table = calloc(STR_VANILLA, 8);
    uint8_t* hellbomb = CodesFixtureRow(table, 5, 0x55u, urd, 3);
    uint8_t* odd = CodesFixtureRow(table, 7, 0x77u, eleven, 11); /* longer than the arcade shows: left as it is */
    uint64_t oddCode; memcpy(&oddCode, odd + SCODE_ROW_CODE, 8);
    int state = g_codesState;

    /* a changed reader refuses every code edit, and no row is touched */
    game[kCodePins[3].rva] ^= 0xFF;
    g_codesState = 0;
    assert(CodesInstallAt((uintptr_t)game, (uintptr_t)table) == -1);
    uint64_t now; memcpy(&now, hellbomb + SCODE_ROW_CODE, 8);
    assert(now != (uintptr_t)g_codes[5].directions);
    StatEdit probe = {&g_mods[0], "StratagemCode", "", "fixture code"};
    probe.record = 0x55u;
    const char* why = NULL;
    assert(CodesField(&probe, &why) == 0 && strstr(why, "refused"));
    game[kCodePins[3].rva] ^= 0xFF;

    /* installed: the row points at HellForge's array, the same directions, and its packed field is the code */
    g_codesState = 0;
    assert(CodesInstallAt((uintptr_t)game, (uintptr_t)table) == 1);
    memcpy(&now, hellbomb + SCODE_ROW_CODE, 8);
    assert(now == (uintptr_t)g_codes[5].directions && g_codes[5].packed == 86 && g_codes[5].directions[1] == 2);
    memcpy(&now, odd + SCODE_ROW_CODE, 8);
    assert(now == oddCode && g_codes[7].row == 0);
    assert(CodesField(&probe, &why) == (uintptr_t)&g_codes[5].packed);
    probe.record = 0x77u;
    assert(CodesField(&probe, &why) == 0);
    int ready = 0;
    assert(CodesRowOf(5, &ready) == (uintptr_t)&g_codes[5].packed && ready && CodesRowOf(7, &ready) == 0);

    /* the stats pass writes the packed field; a value that is not a code is refused */
    StatEdit edit = {&g_mods[0], "StratagemCode", "", "fixture code"};
    edit.record = 0x55u; edit.size = 4; edit.ref = -1;
    edit.value = 5; /* a 0 digit first */
    StatsWrite(&edit, (uintptr_t)&g_codes[5].packed);
    assert(!edit.applied && g_codes[5].packed == 86);
    const uint32_t lldr[4] = {4, 4, 3, 2};
    edit.value = CodePack(lldr, 4);
    StatsWrite(&edit, (uintptr_t)&g_codes[5].packed);
    assert(edit.applied && edit.original == 86 && g_codes[5].packed == edit.value);

    /* the game-thread update: the array, then the row's length */
    CodesTick();
    uint32_t length; memcpy(&length, hellbomb + SCODE_ROW_LENGTH, 4);
    assert(length == 4 && g_codes[5].directions[0] == 4 && g_codes[5].directions[3] == 2 && g_codes[5].shown == edit.value);
    g_codes[5].packed = 25; /* not a code: kept, logged once */
    CodesTick();
    memcpy(&length, hellbomb + SCODE_ROW_LENGTH, 4);
    assert(length == 4 && g_codes[5].shown == edit.value && g_codes[5].refusedLogged == 25);
    g_codes[5].packed = 86; /* the mod taken out: its original back */
    CodesTick();
    memcpy(&length, hellbomb + SCODE_ROW_LENGTH, 4);
    assert(length == 3 && g_codes[5].directions[2] == 3 && g_codes[5].directions[3] == 0);

    /* a code length past the array's directions is refused; within the array it is taken */
    StatEdit len = {&g_mods[0], "StratagemDefinition", "", "fixture length"};
    len.offset = SCODE_ROW_LENGTH; len.size = 4; len.ref = -1; len.value = 11;
    StatsWrite(&len, (uintptr_t)(hellbomb + SCODE_ROW_LENGTH));
    assert(!len.applied);

    memset(g_codes, 0, sizeof g_codes);
    g_codesState = state;
    free(hellbomb); free(odd); free(table);
    VirtualFree(game, 0, MEM_RELEASE);
}
