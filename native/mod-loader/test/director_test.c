/* The spawn director's tables (director.c): roster rows by id with their weights where level 11 left them, compositions
 * by roster, family and row, every write checked (a squad within the queue's 32, ranges in order, finite weights), live
 * and from the boot stats pass; and the difficulty rows without level 11: the game's own copy, each write copied to every
 * copy that held the same bytes. On a fake game.dll image. Included by stats_test.c. */

static uint8_t* g_dirGame;
static uintptr_t DirFakeImage(size_t* size) { if (size) *size = DIR_IMAGE; return (uintptr_t)g_dirGame; }

static void DirectorFixture(void)
{
    g_dirGame = (uint8_t*)VirtualAlloc(NULL, DIR_IMAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(g_dirGame != NULL);
    uintptr_t game = (uintptr_t)g_dirGame;
    uint8_t* rows[5];
    for (int r = 0; r < 5; r++)
    {
        rows[r] = (uint8_t*)calloc(kRosters[r].rows, DIR_ROSTER_ROW);
        uint64_t at = (uint64_t)(uintptr_t)rows[r], count = kRosters[r].rows;
        memcpy(g_dirGame + kRosters[r].descriptor, &at, 8);
        memcpy(g_dirGame + kRosters[r].descriptor + 8, &count, 8);
    }
    /* roster 0's family 1 (the second): two compositions; group 0 draws from a roster group, group 1 from none */
    uint8_t* compositions = (uint8_t*)calloc(2, DIR_COMPOSITION_ROW);
    uint64_t at = (uint64_t)(uintptr_t)compositions, two = 2;
    memcpy(g_dirGame + kRosters[0].descriptor + 0x20 + 0x10 * 1, &at, 8);
    memcpy(g_dirGame + kRosters[0].descriptor + 0x28 + 0x10 * 1, &two, 8);
    uint8_t* second = compositions + DIR_COMPOSITION_ROW;
    uint32_t key = 0xCF576442u, seven = 7, one = 1, ten = 10, four = 4;
    memcpy(second + 0, &key, 4); memcpy(second + 4, &seven, 4);
    memcpy(second + 0xEC, &one, 4); memcpy(second + 0xF0, &ten, 4); memcpy(second + 0xF4, &one, 4); memcpy(second + 0xF8, &four, 4);
    g_dirGame[kWeightReaders[0][0] + kWeightReaders[0][1]] = 0x30;
    for (size_t c = 0; c < sizeof kDifficultyCopies / sizeof kDifficultyCopies[0]; c++)
        for (uint32_t level = 1; level <= DIF_VANILLA; level++)
            memcpy(g_dirGame + kDifficultyCopies[c] + (level - 1) * DIF_ROW + 4, &level, 4);
    g_directorImage = DirFakeImage;
    int added = g_difficultyAdded, state = g_difficultyState;
    g_difficultyAdded = 0; g_difficultyState = 1;
    int ready;

    /* rows by id: the weights at +0x30, no level 11 yet; a composition by roster, family and row */
    assert(DirectorRow("EnemyRoster", (2u << 16) | 3, &ready) == (uintptr_t)rows[2] + 3 * DIR_ROSTER_ROW + 0x30 && ready);
    assert(DirectorRow("EnemyRosterLevel11", 3, &ready) == 0 && ready);
    assert(DirectorRow("EnemyRoster", (2u << 16) | 61, &ready) == 0 && DirectorRow("EnemyRoster", 5u << 16, &ready) == 0);
    assert(DirectorRow("EnemyComposition", (0u << 16) | (1u << 12) | 1, &ready) == (uintptr_t)second);
    assert(DirectorRow("EnemyComposition", (0u << 16) | (1u << 12) | 2, &ready) == 0 && DirectorRow("EnemyComposition", 2u << 12, &ready) == 0);
    /* a roster whose count is not the reviewed one: none of its rows */
    uint64_t wrong = 44;
    memcpy(g_dirGame + kRosters[0].descriptor + 8, &wrong, 8);
    assert(DirectorRow("EnemyRoster", 1, &ready) == 0 && ready);
    wrong = kRosters[0].rows; memcpy(g_dirGame + kRosters[0].descriptor + 8, &wrong, 8);
    /* level 11 added: the weights at +0x2C, its own at +0x54; tech levels up to 11 */
    g_dirGame[kWeightReaders[0][0] + kWeightReaders[0][1]] = 0x2C;
    assert(DirectorRow("EnemyRoster", 3, &ready) == (uintptr_t)rows[0] + 3 * DIR_ROSTER_ROW + 0x2C);
    assert(DirectorRow("EnemyRosterLevel11", 3, &ready) == (uintptr_t)rows[0] + 3 * DIR_ROSTER_ROW + 0x54);
    g_difficultyAdded = 1; g_difficultyState = 0; /* its move is still to come: wait */
    assert(DirectorRow("EnemyRoster", 3, &ready) == 0 && !ready);
    g_difficultyAdded = 0; g_difficultyState = 1;

    /* the checks: a squad within 32, a group that names enemies, ranges in order, weights finite */
    uint32_t v = 32, total = 26;
    assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 4, &v, 4) == NULL);
    v = 33; assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 4, &v, 4), "32"));
    memcpy(second + 12, &key, 4); memcpy(second + 16, &total, 4); /* group 1: 26 of the 32 */
    v = 7; assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 4, &v, 4), "32"));
    v = 6; assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 4, &v, 4) == NULL);
    v = 1; assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 28, &v, 4), "no enemies")); /* group 2 has no key */
    v = 11; assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xF0, &v, 4) == NULL); /* level 11 is there */
    g_dirGame[kWeightReaders[0][0] + kWeightReaders[0][1]] = 0x30;
    assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xF0, &v, 4) != NULL); /* ... and now it is not */
    v = 0; assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xF0, &v, 4), "1 to 10"));
    v = 4; assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xEC, &v, 4) == NULL);
    v = 11; assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xEC, &v, 4), "1 to 10"));
    v = 3; memcpy(second + 0xF0, &v, 4); v = 4;
    assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xEC, &v, 4), "exceed"));
    memcpy(second + 0xF0, &ten, 4);
    float f = -1;
    assert(DirectorRefuses("EnemyRoster", 0, 0, &f, 4) != NULL && DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xE0, &f, 4) != NULL);
    f = 0.001f; assert(strstr(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0xE4, &f, 4), "0.01"));
    f = 2.5f; assert(DirectorRefuses("EnemyRoster", 0, 0, &f, 4) == NULL);
    assert(DirectorRefuses("EnemyComposition", (uintptr_t)second, 0, &key, 4) != NULL); /* a group's key is not edited */

    /* demolition templates: waiting for the destruction system, then a row by index once all eleven read as reviewed;
     * "none" and "Count" stay, and only a force of 1 to 255 is written */
    uint8_t* system = (uint8_t*)calloc(1, 0x40);
    uint8_t* templates = (uint8_t*)calloc(DIR_DEMOLITION_ROWS, DIR_DEMOLITION_ROW);
    for (uint32_t i = 0; i < DIR_DEMOLITION_ROWS; i++)
    {
        memcpy(templates + i * DIR_DEMOLITION_ROW, &i, 4);
        memcpy(templates + i * DIR_DEMOLITION_ROW + 8, kDemolitionTail[i], 8);
    }
    assert(DirectorRow("DemolitionTemplate", 3, &ready) == 0 && !ready);
    uint64_t pointer = (uint64_t)(uintptr_t)system;
    memcpy(g_dirGame + DIR_DESTRUCTION, &pointer, 8);
    assert(DirectorRow("DemolitionTemplate", 3, &ready) == 0 && !ready);
    pointer = (uint64_t)(uintptr_t)templates;
    memcpy(system + 0x30, &pointer, 8);
    assert(DirectorRow("DemolitionTemplate", 3, &ready) == (uintptr_t)templates + 3 * DIR_DEMOLITION_ROW && ready);
    assert(DirectorRow("DemolitionTemplate", 0, &ready) == 0 && DirectorRow("DemolitionTemplate", 10, &ready) == 0 && ready);
    templates[7 * DIR_DEMOLITION_ROW + 12] = 0; /* not as reviewed: nothing */
    assert(DirectorRow("DemolitionTemplate", 3, &ready) == 0 && ready);
    templates[7 * DIR_DEMOLITION_ROW + 12] = 1;
    v = 45; assert(DirectorRefuses("DemolitionTemplate", (uintptr_t)templates + 3 * DIR_DEMOLITION_ROW, 4, &v, 4) == NULL);
    v = 256; assert(strstr(DirectorRefuses("DemolitionTemplate", (uintptr_t)templates + 3 * DIR_DEMOLITION_ROW, 4, &v, 4), "255"));
    v = 0; assert(DirectorRefuses("DemolitionTemplate", (uintptr_t)templates + 3 * DIR_DEMOLITION_ROW, 4, &v, 4) != NULL);
    v = 45; assert(DirectorRefuses("DemolitionTemplate", (uintptr_t)templates + 3 * DIR_DEMOLITION_ROW, 8, &v, 4) != NULL);

    /* live: the editor's writes go through the same checks */
    HfMod* mod = &g_mods[0];
    HfField count = HF_FIELD(EnemyComposition, group_0_count), weight = HF_FIELD_AT(EnemyRoster, weights, 4);
    HfValue fourty = HF_U32(40), five = HF_U32(5), half = HF_F32(0.5f);
    HfRef composition = (0u << 16) | (1u << 12) | 1;
    assert(ApiSetLive(mod, composition, &count, &fourty, HF_LIVE_SHARED) == 0);
    memcpy(&v, second + 4, 4); assert(v == 7);
    assert(ApiSetLive(mod, composition, &count, &five, HF_LIVE_SHARED) != 0);
    memcpy(&v, second + 4, 4); assert(v == 5);
    assert(ApiSetLive(mod, (1u << 16) | 2, &weight, &half, HF_LIVE_SHARED) != 0);
    memcpy(&f, rows[1] + 2 * DIR_ROSTER_ROW + 0x30 + 16, 4); assert(f == 0.5f);
    HfField force = HF_FIELD(DemolitionTemplate, force);
    HfValue strong = HF_U32(45), crushing = HF_U32(300);
    assert(ApiSetLive(mod, 3, &force, &strong, HF_LIVE_SHARED) != 0);
    memcpy(&v, templates + 3 * DIR_DEMOLITION_ROW + 4, 4); assert(v == 45);
    assert(ApiSetLive(mod, 3, &force, &crushing, HF_LIVE_SHARED) == 0 && ApiSetLive(mod, 10, &force, &strong, HF_LIVE_SHARED) == 0);

    /* the game's own difficulty table: level 3's encounter points in its copy and in every copy that held the same */
    float budget = 100, other = 999;
    for (size_t c = 0; c < sizeof kDifficultyCopies / sizeof kDifficultyCopies[0]; c++)
        memcpy(g_dirGame + kDifficultyCopies[c] + 2 * DIF_ROW + 0xE4, c == 7 ? &other : &budget, 4);
    assert(TableRowOf("DifficultySettings", 3, &ready) == game + kDifficultyCopies[DIR_DIFFICULTY_COPY] + 2 * DIF_ROW);
    assert(TableRowOf("DifficultySettings", 11, &ready) == 0 && ready);
    HfField encounter = HF_FIELD(DifficultySettings, encounter_points);
    HfValue more = HF_F32(250);
    assert(ApiSetLive(mod, 3, &encounter, &more, HF_LIVE_SHARED) != 0);
    for (size_t c = 0; c < sizeof kDifficultyCopies / sizeof kDifficultyCopies[0]; c++)
    {
        memcpy(&f, g_dirGame + kDifficultyCopies[c] + 2 * DIF_ROW + 0xE4, 4);
        assert(f == (c == 7 ? 999.0f : 250.0f)); /* a copy that differed is left as it was */
    }

    /* the boot pass: a stats file's director and difficulty lines, checked as live ones are */
    int saved = g_statCount;
    StatEdit e = {0};
    e.mod = mod; e.shared = 1; e.unverified = 1; e.size = 4;
    StatEdit squad = e, roster = e, level = e;
    strcpy(squad.kind, "EnemyComposition"); squad.record = (int64_t)composition; squad.offset = 4; squad.value = 99; strcpy(squad.label, "squad");
    strcpy(roster.kind, "EnemyRoster"); roster.record = (2 << 16) | 5; roster.offset = 8; roster.floating = 1; strcpy(roster.label, "roster");
    f = 3; memcpy(&roster.value, &f, 4);
    strcpy(level.kind, "DifficultySettings"); level.record = 5; level.offset = 0xE8; level.floating = 1; strcpy(level.label, "level 5");
    f = 640; memcpy(&level.value, &f, 4);
    g_stats[g_statCount++] = squad; g_stats[g_statCount++] = roster; g_stats[g_statCount++] = level;
    g_directorDone = 0;
    StatsDirectorReady();
    assert(g_directorDone && !g_stats[saved].applied && g_stats[saved + 1].applied && g_stats[saved + 2].applied);
    memcpy(&f, rows[2] + 5 * DIR_ROSTER_ROW + 0x30 + 8, 4); assert(f == 3);
    memcpy(&f, g_dirGame + kDifficultyCopies[0] + 4 * DIF_ROW + 0xE8, 4); assert(f == 640); /* copied to the others */
    g_statCount = saved;

    g_editCount = 0;
    g_directorImage = DirectorGame;
    g_difficultyAdded = added; g_difficultyState = state;
    for (int r = 0; r < 5; r++) free(rows[r]);
    free(compositions);
    VirtualFree(g_dirGame, 0, MEM_RELEASE);
}
