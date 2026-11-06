/* Moving and growing the game state's arrays (state_moves.c): each field's place at the game's capacity is the compiled
 * one, every site lands in its field at the new capacity, at the same distance from its head, the fields do not overlap
 * and keep their alignment; and ownership.c follows the catalogue past 4,096 items. Included by stats_test.c. */

/* A site's offset from P once written. */
static uint64_t MovedOffset(const MoveSite* site)
{
    uint64_t scale = site->kind & MOVE_SITE_SCALED ? 4 : 1;
    int64_t value = (int64_t)site->value + MoveSiteDelta(site->kind);
    assert(value > 0 && value < 0x7FFFFFFFll);
    return (uint64_t)value * scale + (site->kind & MOVE_SITE_OFF_BLOCK ? MOVE_CATALOGUE_NEW : 0);
}

static void StateMovesOffsetsFixture(void)
{
    /* the guard is off unless asked for, exactly */
    SetEnvironmentVariableW(L"HELLFORGE_MOVE_GUARD", NULL);
    assert(!MoveGuardWanted());
    SetEnvironmentVariableW(L"HELLFORGE_MOVE_GUARD", L"0");
    assert(!MoveGuardWanted());
    SetEnvironmentVariableW(L"HELLFORGE_MOVE_GUARD", L"yes");
    assert(!MoveGuardWanted());
    SetEnvironmentVariableW(L"HELLFORGE_MOVE_GUARD", L"1");
    assert(MoveGuardWanted());
    SetEnvironmentVariableW(L"HELLFORGE_MOVE_GUARD", NULL);
    /* the game's capacity is the layout the research read */
    assert(MoveFieldAt(MOVE_F_HIERARCHY, 4096) == 0xB8004 && MoveFieldAt(MOVE_F_STARTS, 4096) == 0xD0004 && MoveFieldAt(MOVE_F_TOTAL, 4096) == 0xD0064 &&
        MoveFieldAt(MOVE_F_INDEX, 4096) == 0xD0068 && MoveFieldAt(MOVE_F_STATUS, 4096) == 0xD4068 && MoveFieldAt(MOVE_F_INVENTORY, 4096) == 0xD406C &&
        MOVE_CATALOGUE_OLD_BYTES == 0xD4070);
    /* each field holds its capacity, and starts as far past a 16-byte boundary as it did */
    static const uint32_t bytes[] = {4, 0xB8u * MOVE_ITEMS, 0x18u * MOVE_ITEMS, 0x60, 4, 4u * MOVE_ITEMS, 4, 4};
    for (uint32_t field = MOVE_F_COUNT; field <= MOVE_F_INVENTORY; field++)
    {
        assert(MoveFieldAt(field, MOVE_ITEMS) % 16 == MoveFieldAt(field, MOVE_ITEMS_OLD) % 16);
        assert(MoveFieldAt(field, MOVE_ITEMS) + bytes[field] == (field == MOVE_F_INVENTORY ? MOVE_CATALOGUE_BYTES : MoveFieldAt(field + 1, MOVE_ITEMS)));
    }
    assert(MOVE_CATALOGUE_NEW % 16 == 0 && MOVE_CATALOGUE_NEW >= MOVE_SLOTS_NEW + MOVE_SLOTS_ROOM * MOVE_SLOT_BYTES && MOVE_SLOTS_NEW >= MOVE_TAIL);
    assert(MOVE_CATALOGUE_NEW + MOVE_CATALOGUE_BYTES <= MOVE_TAIL + MOVE_EXTRA_BYTES);
    /* every site */
    for (size_t i = 0; i < MOVE_COUNT(kMoveCatalogueSites); i++)
    {
        const MoveSite* site = &kMoveCatalogueSites[i];
        uint32_t field = site->kind & 0xFu;
        uint64_t scale = site->kind & MOVE_SITE_SCALED ? 4 : 1;
        uint64_t was = (uint64_t)(uint32_t)site->value * scale - (site->kind & MOVE_SITE_OFF_BLOCK ? 0 : MOVE_CATALOGUE_OLD);
        uint64_t distance = was - MoveFieldAt(field, MOVE_ITEMS_OLD), now = MovedOffset(site);
        assert(was >= MoveFieldAt(field, MOVE_ITEMS_OLD));
        assert(now == MOVE_CATALOGUE_NEW + MoveFieldAt(field, MOVE_ITEMS) + distance);
        assert(now < MOVE_CATALOGUE_NEW + (field == MOVE_F_INVENTORY ? MOVE_CATALOGUE_BYTES : MoveFieldAt(field + 1, MOVE_ITEMS)));
        assert(!(site->kind & MOVE_SITE_SCALED) || MoveSiteDelta(site->kind) * 4ll == (int64_t)MOVE_CATALOGUE_DELTA + MoveFieldAt(field, MOVE_ITEMS) - MoveFieldAt(field, MOVE_ITEMS_OLD));
    }
    /* the capacities */
    assert(MoveCapacity(MOVE_C_ITEMS) == 40960 && MoveCapacity(MOVE_C_ITEMS_PARSE) == 40960 * 0x70 && MoveCapacity(MOVE_C_RESET) == (int32_t)MOVE_CATALOGUE_BYTES - 4 &&
        MoveCapacity(MOVE_C_PACKAGES) == 1024 && MoveCapacity(MOVE_C_PACKAGES_PARSE) == 1024 * 0x30);
    for (size_t i = 0; i < MOVE_COUNT(kMoveCapacitySites); i++)
    {
        uint8_t kind = kMoveCapacitySites[i].kind;
        assert(kMoveCapacitySites[i].value == (kind == MOVE_C_ITEMS ? 4096 : kind == MOVE_C_ITEMS_PARSE ? 0x70000 : kind == MOVE_C_RESET ? 0xD406C : kind == MOVE_C_PACKAGES ? 256 : 0x3000));
    }
}

/* The armory's room: a list full counting its vanilla records refuses by type, and entity copies take no card. */
static void ArmoryRoomFixture(void)
{
    static uint8_t records[(REG_LOADOUT_RECORDS + REG_MAX_WEAPONS) * REG_LOADOUT_STRIDE];
    WeaponLoad L = {0};
    L.records = records;
    uint32_t primary = 1, secondary = 3, support = 2;
    for (uint32_t r = 0; r < REG_ARMORY_CARDS - 1; r++)
    {
        uint32_t id = 0x1000 + r;
        memcpy(records + (size_t)r * REG_LOADOUT_STRIDE, &id, 4);
        memcpy(records + (size_t)r * REG_LOADOUT_STRIDE + 4, &primary, 4);
    }
    L.recordCount = REG_ARMORY_CARDS - 1;
    int saved = g_weaponCount;
    static HfMod mod;
    strcpy(mod.id, "test.armory");
    g_weaponCount = 0;
    Weapon w = {.mod = &mod, .id = 0x7777, .name = "test/one"};
    /* a record of each kind to copy: the first primary, and a secondary and a support weapon after the list */
    memcpy(records + (size_t)L.recordCount * REG_LOADOUT_STRIDE + 4, &secondary, 4);
    memcpy(records + (size_t)(L.recordCount + 1) * REG_LOADOUT_STRIDE + 4, &support, 4);
    uint8_t row[16] = {0};
    int secondaryBase = (int)L.recordCount, supportBase = secondaryBase + 1;
    L.recordCount += 2;
    assert(ArmoryRoom(&L, &w, 0) && L.cards[1] == REG_ARMORY_CARDS - 1 && L.cards[3] == 1);
    L.index = NULL;
    OwnLoadoutRecord(&L, &w, 0, row);
    assert(L.cards[1] == REG_ARMORY_CARDS && !ArmoryRoom(&L, &w, 0)); /* full: refused */
    assert(ArmoryRoom(&L, &w, secondaryBase) && ArmoryRoom(&L, &w, supportBase)); /* another list; a type no picker lists */
    w.plain = 1;
    assert(ArmoryRoom(&L, &w, 0)); /* an entity copy takes no card */
    g_weaponCount = saved;
}

/* ownership.c at the moved layout: a record past 4,096 is found, and the room is the new capacity's. */
static void StateMovesOwnershipFixture(void)
{
    CatalogueLayout vanilla = g_catalogue;
    assert(OWN_MAX == 4096 && OWN_COUNT == 0x1CE0 && OWN_RECORDS == 0x1CE4 && OWN_INDEX == 0xB9CE4 && OWN_STARTS == 0xD1CE4 && OWN_CATALOGUE == 0xD5D48 && OWN_INVENTORY == 0xD5D4C);
    uint32_t moved = MOVE_CATALOGUE_NEW; /* far past the old block: the fixture's block is sized from its offsets */
    g_catalogue = (CatalogueLayout){0, 4, MOVE_AT_HIERARCHY(MOVE_ITEMS), MOVE_AT_STARTS(MOVE_ITEMS), MOVE_AT_STATUS(MOVE_ITEMS), MOVE_AT_INVENTORY(MOVE_ITEMS), MOVE_ITEMS};
    uint8_t* manager = (uint8_t*)calloc(1, MOVE_CATALOGUE_BYTES);
    assert(manager != NULL && moved != 0);
    const uint32_t last = MOVE_ITEMS - 1;
    *(uint32_t*)(manager + OWN_COUNT) = MOVE_ITEMS;
    uint32_t* entry = (uint32_t*)(manager + OWN_INDEX + (uintptr_t)last * 24);
    entry[0] = last; entry[1] = 0xABCD;
    *(uint32_t*)(manager + OWN_RECORDS + (uintptr_t)last * OWN_RECORD + 0xC) = 7;
    assert(OwnEntry((uintptr_t)manager, 0xABCD, 0, 7) == (int)last);
    OwnBatch batch = {.items = {{OWN_WEAPON, 0xBEEF, 0, 0, 0xABCD, 0}}, .count = 1};
    OwnAppendAll((uintptr_t)manager, &batch);
    assert(batch.added == 0 && batch.why != NULL && strstr(batch.why, "full") != NULL);
    free(manager);
    g_catalogue = vanilla;
}
