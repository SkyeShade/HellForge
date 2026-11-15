/* The armory's lists as the game reads them (registry_load.c ArmoryFill, registry.c ArmoryEntity): the game's own lookup
 * (game.dll 0x11E7AE0, byte for byte, its Armory global pointed at the fixture's armory) answers which list an entity is
 * in. A hellpod rack that breaks turns on the dropped item's pickup by that answer (0x936530: list 2, a support weapon,
 * turns on interaction 3; no list turns on 7). The Stratagem Compendium's EAT copies (entities of add_entity with a
 * loadout record of their own) were in no list, as their armory entries named no entity: dropped from a broken pod, they
 * could never be picked up (Discord, 2026-11-15). Included by stats_test.c. */

static const uint8_t kArmoryListCode[123] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x8b, 0x1d, 0xfc, 0xee, 0x13, 0x02, 0x41, 0xbb, 0x01, 0x00, 0x00, 0x00,
    0x4c, 0x8d, 0x93, 0x28, 0x1b, 0x00, 0x00, 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00, 0x45, 0x8b, 0x42, 0x1c,
    0x45, 0x85, 0xc0, 0x74, 0x34, 0x45, 0x8b, 0x0a, 0x33, 0xd2, 0x49, 0xc1, 0xe1, 0x05, 0x4c, 0x03, 0xcb, 0x45,
    0x85, 0xc0, 0x74, 0x23, 0x49, 0x8d, 0x41, 0x08, 0x66, 0x90, 0x48, 0x39, 0x08, 0x74, 0x0d, 0xff, 0xc2, 0x48,
    0x83, 0xc0, 0x20, 0x41, 0x3b, 0xd0, 0x72, 0xf0, 0xeb, 0x0b, 0x8b, 0xc2, 0x48, 0xc1, 0xe0, 0x05, 0x49, 0x03,
    0xc1, 0x75, 0x15, 0x41, 0xff, 0xc3, 0x49, 0x83, 0xc2, 0x04, 0x41, 0x83, 0xfb, 0x07, 0x72, 0xb6, 0x33, 0xc0,
    0x48, 0x8b, 0x5c, 0x24, 0x08, 0xc3, 0x48, 0x8b, 0x5c, 0x24, 0x08, 0x41, 0x8b, 0xc3, 0xc3};

#define AL_SLOT_AT 2048u /* the fixture's Armory global, in the code's block (rip-relative from +12) */

/* A loadout record r: {u32 id, u32 type, ...}. */
static void AlRecord(uint8_t* table, uint32_t r, uint32_t id, uint32_t type)
{
    uint8_t* record = table + (size_t)SlotsOf(REG_LOADOUT) * 16 + (size_t)r * REG_LOADOUT_STRIDE;
    memcpy(record, &id, 4);
    memcpy(record + 4, &type, 4);
}

/* An index row naming record r for entity. */
static void AlRow(uint8_t* table, uint64_t entity, uint32_t r)
{
    uint8_t* row = FreeRow(table, SlotsOf(REG_LOADOUT), 16, entity);
    assert(row != NULL);
    memcpy(row, &entity, 8);
    memcpy(row + 8, &r, 4);
}

static void ArmoryListFixture(void)
{
    const uint64_t eat = 0x80932FA0ED6901D3ull, liberator = 0x37CDE43876BA26BBull, copy = 0xF186E91F81B6D7B0ull;
    const uint64_t rack = 0x9A10F3A5EE8BDB93ull, twinA = 0x1111ull, twinB = 0x2222ull, sharer = 0x3333ull, sharedBase = 0x4444ull;
    uint8_t* code = (uint8_t*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(code != NULL);
    memcpy(code, kArmoryListCode, sizeof kArmoryListCode);
    assert(!memcmp(code, kArmoryListPin, sizeof kArmoryListPin)); /* the self-check's pin is the code's start */
    int32_t disp = (int32_t)AL_SLOT_AT - 12;
    memcpy(code + 8, &disp, 4);
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    ArmoryListFn list = (ArmoryListFn)(void*)code;

    uint8_t* table = (uint8_t*)calloc(1, (size_t)SlotsOf(REG_LOADOUT) * 16 + 8 * REG_LOADOUT_STRIDE);
    assert(table != NULL);
    AlRecord(table, 0, 0xCB778090u, 2); AlRow(table, eat, 0);             /* the EAT-17: support (2) */
    AlRecord(table, 1, 0xAA035309u, 1); AlRow(table, liberator, 1);       /* the Liberator: primary (1) */
    AlRecord(table, 2, 0xF186E91Fu, 2); AlRow(table, copy, 2);            /* the EAT copy's own record (PlainLoadoutRecord) */
    AlRecord(table, 3, 0x12345678u, 3); AlRow(table, twinA, 3); AlRow(table, twinB, 3); /* two owners: no one entity */
    AlRecord(table, 4, 0x87654321u, 1); AlRow(table, sharedBase, 4); AlRow(table, sharer, 4); /* a copy on its base's record */

    int savedCount = g_weaponCount;
    assert(WeaponRoom(2));
    Weapon saved[2];
    memcpy(saved, g_weapons, sizeof saved);
    memset(g_weapons, 0, sizeof saved);
    g_weapons[0].plain = 1; g_weapons[0].state = 1; g_weapons[0].entity = copy; g_weapons[0].baseEntity = eat;
    g_weapons[1].plain = 1; g_weapons[1].state = 1; g_weapons[1].entity = sharer; g_weapons[1].baseEntity = sharedBase;
    g_weaponCount = 2;
    /* what the armory named before: the copy's record has no owner, its one row being an entity copy's */
    assert(LoadoutOwner(table, 2) == 0 && LoadoutOwner(table, 0) == eat);
    assert(ArmoryEntity(table, 2) == copy && ArmoryEntity(table, 0) == eat && ArmoryEntity(table, 3) == 0 && ArmoryEntity(table, 4) == sharedBase);

    uint8_t* armory = (uint8_t*)calloc(1, ArmoryBytes());
    assert(armory != NULL);
    ArmoryFill(armory, table, 5);
    memcpy(code + AL_SLOT_AT, &armory, 8);
    assert(list(eat) == 2 && list(copy) == 2);             /* the copy is in its base's list: it drops as an EAT does */
    assert(list(liberator) == 1 && list(sharedBase) == 1); /* the vanilla owners keep theirs */
    assert(list(rack) == 0 && list(sharer) == 0);          /* no record of its own: in no list, as before */
    assert(list(twinA) == 0 && list(twinB) == 0);
    uint32_t starts[REG_ARMORY_TYPES], counts[REG_ARMORY_TYPES];
    memcpy(starts, armory + 0x1B24, sizeof starts);
    memcpy(counts, armory + 0x1B40, sizeof counts);
    assert(counts[1] == 2 && counts[2] == 2 && counts[3] == 1); /* the same entries as before, one more entity named */
    uint64_t named = 0;
    memcpy(&named, armory + (size_t)(starts[2] + 1) * 32 + 8, 8);
    assert(named == copy);

    memcpy(g_weapons, saved, sizeof saved);
    g_weaponCount = savedCount;
    free(armory);
    free(table);
    VirtualFree(code, 0, MEM_RELEASE);
}
