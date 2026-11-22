/* A registered weapon's own loadout record (registry.c OwnLoadoutRecordOf): component_record, hf_record and hf_set reach
 * it in HellForge's copy of table 29, past the vanilla records, so a mod can set the weapon's armory traits. A record it
 * shares with its base, or one in a table that is not HellForge's copy, stays refused. Included by stats_test.c. */

static void LrRow(uint8_t* index, uint64_t entity, uint32_t record)
{
    uint8_t* row = FreeRow(index, SlotsOf(REG_LOADOUT), 16, entity);
    assert(row != NULL);
    memcpy(row, &entity, 8);
    memcpy(row + 8, &record, 4);
}

static void LoadoutRecordFixture(void)
{
    const uint64_t base = 0x37CDE43876BA26BBull, weapon = 0xF186E91F81B6D7B0ull, sharer = 0x1111ull, stranger = 0x2222ull, past = 0x3333ull;
    uint32_t slots = SlotsOf(REG_LOADOUT), records = REG_LOADOUT_RECORDS + 2;
    uint8_t* game = (uint8_t*)calloc(1, REG_STATE_GLOBAL + 8);
    uint8_t* state = (uint8_t*)calloc(1, REG_TABLES + 8 * (REG_LOADOUT + 1));
    uint8_t* copy = (uint8_t*)calloc(1, 32 + (size_t)slots * 16 + (size_t)(records + 2) * REG_LOADOUT_STRIDE);
    uint8_t* vanilla = (uint8_t*)calloc(1, 32 + (size_t)slots * 16 + (size_t)records * REG_LOADOUT_STRIDE);
    assert(game != NULL && state != NULL && copy != NULL && vanilla != NULL);
    uint8_t* index = copy + 32;
    LrRow(index, base, 5);
    LrRow(index, weapon, REG_LOADOUT_RECORDS);         /* its own, the first after the vanilla ones (OwnLoadoutRecord) */
    LrRow(index, sharer, 5);                           /* an entity copy on its base's record */
    LrRow(index, stranger, REG_LOADOUT_RECORDS + 1);   /* not registered */
    LrRow(index, past, records);                       /* past the records the copy holds */
    LrRow(vanilla + 32, weapon, REG_LOADOUT_RECORDS);

    uintptr_t savedGame = g_game;
    uint8_t* savedTable = g_loadoutTable;
    uint32_t savedRecords = g_loadoutRecords;
    int savedCount = g_weaponCount;
    Weapon saved[3];
    memcpy(saved, g_weapons, sizeof saved);
    memset(g_weapons, 0, sizeof saved);
    g_weapons[0].state = 1; g_weapons[0].entity = weapon;
    g_weapons[1].state = 1; g_weapons[1].entity = sharer; g_weapons[1].plain = 1; g_weapons[1].baseEntity = base;
    g_weapons[2].state = 1; g_weapons[2].entity = past;
    g_weaponCount = 3;
    g_game = (uintptr_t)game;
    memcpy(game + REG_STATE_GLOBAL, &state, 8);
    SetStatePointer(state, REG_TABLES + 8 * REG_LOADOUT, index);
    g_loadoutTable = index;
    g_loadoutRecords = records;

    Component c = {0};
    strcpy(c.name, "LoadoutEntryComponentData");
    c.index = REG_LOADOUT;
    c.stride = REG_LOADOUT_STRIDE;
    uint32_t stride = 0;
    uintptr_t own = (uintptr_t)(index + (size_t)slots * 16 + (size_t)REG_LOADOUT_RECORDS * REG_LOADOUT_STRIDE);
    assert(RegisteredRecord(&c, weapon, &stride) == own && stride == REG_LOADOUT_STRIDE);
    assert(RegisteredRecord(&c, sharer, &stride) == 0);   /* its base's: shared */
    assert(RegisteredRecord(&c, stranger, &stride) == 0); /* not a weapon a mod added */
    assert(RegisteredRecord(&c, past, &stride) == 0);
    assert(RegisteredRecord(&c, base, &stride) == 0);
    SetStatePointer(state, REG_TABLES + 8 * REG_LOADOUT, vanilla + 32); /* the game reads a table that is not HellForge's copy */
    assert(RegisteredRecord(&c, weapon, &stride) == 0);

    memcpy(g_weapons, saved, sizeof saved);
    g_weaponCount = savedCount;
    g_game = savedGame;
    g_loadoutTable = savedTable;
    g_loadoutRecords = savedRecords;
    free(vanilla);
    free(copy);
    free(state);
    free(game);
}
