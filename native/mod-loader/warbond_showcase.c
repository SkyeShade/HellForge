/* The Helldiver and background beside a warbond's card (static analysis of build 25480438).
 * On a card change the game finds the card's id32 in 0x32DE600 (27 rows of 0x20: +0 id32, +8 the level beside the card,
 * +0x10 the level on its pages, +0x18 a character; row 0 the default) and spawns the character from 0x32B1E60 (27 rows
 * of 0x28: +0 id, +4 stage, +0xC/+0x10/+0x14 armor/helmet/cape kit, 0 for the player's own, +0x18 pose, +0x20 weapon
 * entity) with 0x13050B0. Both tables are copied near game.dll with a row per mod warbond, from the game warbond it
 * names, and their nine lookups (bounds `cmp reg,0x1B`) pointed at the copies. An item's kit is found as the preset path
 * 0x139CCD0 does (catalogue hierarchy {index, mixId, kit}, kit type +0x28 in [game+0x33264F8]), when the character first
 * spawns: the catalogue is filled after start. Included after warbond_texts.c. */
#define WB_SHOW_TABLE 0x32DE600u
#define WB_SHOW_ROW 0x20u
#define WB_CHAR_TABLE 0x32B1E60u
#define WB_CHAR_ROW 0x28u
#define WB_SHOW_ROWS 27u /* in each table */
#define WB_SHOW_DEFAULT 0xF2760503u
#define WB_SHOW_SPAWN 0x13050B0u
#define WB_KITS 0x33264F8u
static const WbLea kWbShowLeas[] = { /* the warbond rows' lookups */
    {0x1304DC0u, 7, {0x4c,0x8d,0x05,0x39,0x98,0xfd,0x01}, 0}, {0x145F047u, 7, {0x4c,0x8d,0x05,0xb2,0xf5,0xe7,0x01}, 0},
    {0x14975FFu, 7, {0x4c,0x8d,0x05,0xfa,0x6f,0xe4,0x01}, 0}, {0x1961FC7u, 7, {0x4c,0x8d,0x05,0x32,0xc6,0x97,0x01}, 0},
    {0x1962F3Au, 7, {0x4c,0x8d,0x05,0xbf,0xb6,0x97,0x01}, 0}, {0x19653B6u, 7, {0x4c,0x8d,0x05,0x43,0x92,0x97,0x01}, 0}};
static const WbBound kWbShowBounds[] = {
    {0x1304DDAu, 3, {0x83,0xf8,0x1b}}, {0x145F06Au, 3, {0x83,0xf9,0x1b}}, {0x149761Bu, 3, {0x83,0xf9,0x1b}},
    {0x1961FEAu, 3, {0x83,0xf9,0x1b}}, {0x1962F5Au, 3, {0x83,0xf9,0x1b}}, {0x19653D2u, 3, {0x83,0xf9,0x1b}}};
static const WbLea kWbCharLeas[] = { /* the character rows' lookups */
    {0x1304D90u, 7, {0x4c,0x8d,0x05,0xc9,0xd0,0xfa,0x01}, 0}, {0x13050D6u, 7, {0x4c,0x8d,0x05,0x83,0xcd,0xfa,0x01}, 0},
    {0x14CE342u, 7, {0x4c,0x8d,0x3d,0x17,0x3b,0xde,0x01}, 0}};
static const WbBound kWbCharBounds[] = {{0x1304DAAu, 3, {0x83,0xf8,0x1b}}, {0x13050EEu, 3, {0x83,0xf8,0x1b}}, {0x14CE35Du, 3, {0x83,0xf8,0x1b}}};
static const uint8_t kWbShowSpawnPin[13] = {0x48,0x89,0x6c,0x24,0x20,0x56,0x48,0x81,0xec,0xd0,0x00,0x00,0x00};
static const char* const kWbKitNames[3] = {"armor", "helmet", "cape"};
static uint8_t* g_wbChars; /* the character rows' copy */
static uint8_t g_wbShowKits[WB_MAX]; /* 1 once a mod warbond's items are looked up */
static void (*g_wbShowSpawn)(void* unused, uint32_t character);

/* The row with an id among count rows of stride bytes; NULL for none (and for 0, the empty character row's). */
static const uint8_t* ShowcaseRow(const uint8_t* rows, size_t count, size_t stride, uint32_t id)
{
    for (size_t i = 0; i < count && id != 0; i++)
        if (*(const uint32_t*)(rows + i * stride) == id) return rows + i * stride;
    return NULL;
}

/* An item's kit of a type (0 armor, 1 helmet, 2 cape) as the game's preset path finds it: 0 when the item has none of
 * that type, -1 while the catalogue or the kit list is not filled yet. */
static int64_t WarbondItemKit(uint32_t mixId, uint32_t type)
{
    const uint8_t* mgr = *(const uint8_t* const*)(g_wbGame + WB_PROGRESSION);
    const uint8_t* list = *(const uint8_t* const*)(g_wbGame + WB_KITS);
    if (mgr == NULL || list == NULL || *(const uint32_t*)(mgr + g_catalogue.count) == 0 || *(const uint32_t*)(list + 8) == 0) return -1;
    uint32_t count = *(const uint32_t*)(mgr + g_catalogue.count), kit = 0;
    const uint8_t* entry = mgr + g_catalogue.index;
    for (uint32_t i = 0; i < count && i < g_catalogue.items && kit == 0; i++, entry += 0x18)
        if (*(const uint32_t*)(entry + 4) == mixId) kit = *(const uint32_t*)(entry + 8);
    const uint8_t* const* records = *(const uint8_t* const* const*)list;
    for (uint32_t i = 0; i < *(const uint32_t*)(list + 8) && kit != 0; i++)
        if (records[i] != NULL && *(const uint32_t*)records[i] == kit) return *(const uint32_t*)(records[i] + 0x28) == type ? kit : 0;
    return 0;
}

/* A mod warbond's items into its character row (+0xC armor, +0x10 helmet, +0x14 cape), once the catalogue is filled. */
static void WarbondShowcaseKits(int i)
{
    const ModWarbond* w = &g_modWarbonds[i];
    uint8_t* row = g_wbChars + (WB_SHOW_ROWS + (size_t)i) * WB_CHAR_ROW;
    int64_t kits[3];
    for (int k = 0; k < 3; k++)
        if ((kits[k] = w->showItem[k] != 0 ? WarbondItemKit(w->showItem[k], (uint32_t)k) : 0) < 0) return;
    for (int k = 0; k < 3; k++)
    {
        uint32_t kit = (uint32_t)kits[k];
        if (kit != 0) memcpy(row + 0xC + 4 * k, &kit, 4);
        else if (w->showItem[k] != 0) Note("warbonds: showcase of warbond %u: item %u is not %s %s; it keeps the one it starts from", w->id, w->showItem[k],
            k == 0 ? "an" : "a", kWbKitNames[k]);
    }
    g_wbShowKits[i] = 1;
}

static void WarbondShowcaseSpawn(void* unused, uint32_t character)
{
    for (int i = 0; i < g_modWarbondCount; i++)
        if (!g_wbShowKits[i] && *(const uint32_t*)(g_wbChars + (WB_SHOW_ROWS + (size_t)i) * WB_CHAR_ROW) == character) WarbondShowcaseKits(i);
    g_wbShowSpawn(unused, character);
}

/* Points count references at a copy and sets their bounds; nonzero when game.dll could not be rewritten. */
static int ShowcaseRepoint(uintptr_t game, const WbLea* leas, const WbBound* bounds, size_t count, const uint8_t* copy, uint8_t rows)
{
    for (size_t i = 0; i < count; i++)
    {
        int32_t disp = (int32_t)((intptr_t)copy - (intptr_t)(game + leas[i].rva + leas[i].len));
        if (ApiWrite(game + leas[i].rva + leas[i].len - 4, &disp, 4)) return 1;
    }
    for (size_t i = 0; i < count; i++) /* the bounds after the bases: never past the copy */
        if (ApiWrite(game + bounds[i].rva + bounds[i].len - 1, &rows, 1)) return 1;
    return 0;
}

/* Whether the nine lookups and the spawn match the reviewed build. */
static int ShowcasePins(uintptr_t game)
{
    for (size_t i = 0; i < 6; i++)
        if (!PinMatches(game + kWbShowLeas[i].rva, kWbShowLeas[i].bytes, 7) || !PinMatches(game + kWbShowBounds[i].rva, kWbShowBounds[i].bytes, 3)) return 0;
    for (size_t i = 0; i < 3; i++)
        if (!PinMatches(game + kWbCharLeas[i].rva, kWbCharLeas[i].bytes, 7) || !PinMatches(game + kWbCharBounds[i].rva, kWbCharBounds[i].bytes, 3)) return 0;
    return PinMatches(game + WB_SHOW_SPAWN, kWbShowSpawnPin, sizeof kWbShowSpawnPin);
}

/* A character id of its own for mod warbond i: never 0, never a game character's or an earlier mod warbond's. */
static uint32_t ShowcaseCharacter(const uint8_t* chars, int i)
{
    uint32_t character = g_modWarbonds[i].id;
    while (character == 0 || ShowcaseRow(chars, WB_SHOW_ROWS, WB_CHAR_ROW, character) != NULL ||
        ShowcaseRow(g_wbChars + WB_SHOW_ROWS * WB_CHAR_ROW, (size_t)i, WB_CHAR_ROW, character) != NULL)
        character = character * 0x9E3779B1u + 1u;
    return character;
}

/* Mod warbond i's two rows in the copies (table: the warbond rows'), from the game's rows and chars; 0 when the game's
 * default character is missing. */
static int ShowcaseModRows(uint8_t* table, const uint8_t* rows, const uint8_t* chars, int i)
{
    const ModWarbond* w = &g_modWarbonds[i];
    const uint8_t* from = ShowcaseRow(rows, WB_SHOW_ROWS, WB_SHOW_ROW, w->showFrom);
    if (from == NULL && w->showFrom != 0) Note("warbonds: showcase of warbond %u: %u is not a game warbond's; it starts from the default", w->id, w->showFrom);
    if (from == NULL) from = rows;
    const uint8_t* fromChar = ShowcaseRow(chars, WB_SHOW_ROWS, WB_CHAR_ROW, *(const uint32_t*)(from + 0x18));
    if (fromChar == NULL) fromChar = ShowcaseRow(chars, WB_SHOW_ROWS, WB_CHAR_ROW, *(const uint32_t*)(rows + 0x18));
    if (fromChar == NULL) return 0;
    uint32_t character = ShowcaseCharacter(chars, i);
    uint8_t* row = table + (WB_SHOW_ROWS + (size_t)i) * WB_SHOW_ROW;
    uint8_t* ch = g_wbChars + (WB_SHOW_ROWS + (size_t)i) * WB_CHAR_ROW;
    memcpy(row, from, WB_SHOW_ROW);
    memcpy(row, &w->id, 4);
    memcpy(row + 0x18, &character, 4);
    memcpy(ch, fromChar, WB_CHAR_ROW);
    memcpy(ch, &character, 4);
    const uint8_t* pose = ShowcaseRow(rows, WB_SHOW_ROWS, WB_SHOW_ROW, w->showPose);
    const uint8_t* poseChar = pose ? ShowcaseRow(chars, WB_SHOW_ROWS, WB_CHAR_ROW, *(const uint32_t*)(pose + 0x18)) : NULL;
    if (poseChar) memcpy(ch + 0x18, poseChar + 0x18, 4);
    if (w->showWeapon != 0) memcpy(ch + 0x20, &w->showWeapon, 8);
    g_wbShowKits[i] = w->showItem[0] == 0 && w->showItem[1] == 0 && w->showItem[2] == 0;
    return 1;
}

/* NULL, or why not. */
static const char* InstallWarbondShowcase(uintptr_t game)
{
    if (!ShowcasePins(game)) return "the showcase code does not match the reviewed build";
    const uint8_t* rows = (const uint8_t*)(game + WB_SHOW_TABLE);
    const uint8_t* chars = (const uint8_t*)(game + WB_CHAR_TABLE);
    if (*(const uint32_t*)rows != WB_SHOW_DEFAULT) return "the showcase table is not the reviewed build's";
    size_t total = WB_SHOW_ROWS + (size_t)g_modWarbondCount; /* 126 at most: the bounds are imm8 */
    uint8_t* table = AllocateNearGame(game, total * (WB_SHOW_ROW + WB_CHAR_ROW));
    if (table == NULL) return "no memory near game.dll";
    g_wbChars = table + total * WB_SHOW_ROW;
    g_wbGame = game;
    memcpy(table, rows, WB_SHOW_ROWS * WB_SHOW_ROW);
    memcpy(g_wbChars, chars, WB_SHOW_ROWS * WB_CHAR_ROW);
    int items = 0;
    for (int i = 0; i < g_modWarbondCount; i++)
    {
        if (!ShowcaseModRows(table, rows, chars, i)) return "the showcase's default Helldiver is not in the reviewed build's table";
        items |= !g_wbShowKits[i];
    }
    /* the characters first: a warbond row names a character only the copy has */
    if (ShowcaseRepoint(game, kWbCharLeas, kWbCharBounds, 3, g_wbChars, (uint8_t)total) ||
        ShowcaseRepoint(game, kWbShowLeas, kWbShowBounds, 6, table, (uint8_t)total))
        return "game.dll could not be rewritten";
    if (items && LoaderHook("warbond showcase", game + WB_SHOW_SPAWN, (void*)WarbondShowcaseSpawn, (void**)&g_wbShowSpawn))
        Note("warbonds: the showcase's Helldivers wear what they start from: its spawn could not be hooked");
    Note("warbonds: %d mod warbond showcase(s)", g_modWarbondCount);
    return NULL;
}
