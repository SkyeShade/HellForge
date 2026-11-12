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

/* A fake game.dll for the move: every site as reviewed, the state not allocated. */
static void MoveFakeImage(uint8_t* game)
{
    memcpy(game + MOVE_RESET_CALL, kMoveResetCall, 5);
    const struct { const MoveSite* sites; size_t count; } sets[] = {{kMoveStateSizes, MOVE_COUNT(kMoveStateSizes)}, {kMovePackageSites, MOVE_COUNT(kMovePackageSites)},
        {kMoveCatalogueSites, MOVE_COUNT(kMoveCatalogueSites)}, {kMoveCapacitySites, MOVE_COUNT(kMoveCapacitySites)}};
    for (size_t i = 0; i < MOVE_COUNT(sets); i++)
        for (size_t j = 0; j < sets[i].count; j++) memcpy(game + sets[i].sites[j].rva, sets[i].sites[j].code, sets[i].sites[j].length);
    *(uint64_t*)(game + MOVE_STATE) = 0;
}

static void MoveFresh(uint8_t* game)
{
    MoveFakeImage(game);
    g_stateMoves = 0; g_moveWaitFrom = g_moveMovedAt = 0; g_moveTries = g_moveUndecoded = 0; g_moveStateSeen = 0; g_moveResetRel = 0;
    g_catalogue = (CatalogueLayout)MOVE_LAYOUT(MOVE_CATALOGUE_OLD, MOVE_ITEMS_OLD);
    g_gameItems = g_gamePackages = 0;
}

static int MoveImageReviewed(uintptr_t game)
{
    char why[64];
    uint32_t saved = g_moveUndecoded;
    g_moveUndecoded = 0;
    int reviewed = MoveUnread(game, why, sizeof why) == 0;
    g_moveUndecoded = saved;
    return reviewed;
}

/* game.dll still being decoded when its pin reads (bug report 0401): a site above the pin reads as other bytes for a few
 * ticks. The move waits and goes ahead once it reads as reviewed; refuses once the game allocates its state first or the
 * wait runs out, leaving every site as it was; and a write lost before the state comes puts all of it back. */
static void StateMovesDecodeFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, 0x3500000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(image != NULL);
    uintptr_t game = (uintptr_t)image;
    const MoveSite* late = NULL; /* 0x190eee7, the site of the report */
    for (size_t i = 0; i < MOVE_COUNT(kMovePackageSites); i++) if (kMovePackageSites[i].rva == 0x190eee7u) late = &kMovePackageSites[i];
    assert(late != NULL);
    SetEnvironmentVariableW(L"HELLFORGE_MOVE_UNDECODED", NULL);

    /* still encoded at the first try, decoded at the next: moved then */
    MoveFresh(image);
    image[late->rva + 2] ^= 0x5A;
    StateMoves(game);
    assert(g_stateMoves == 0 && g_moveTries == 1 && g_moveWaitFrom != 0 && g_gameItems == 0);
    assert(MoveSiteHolds(game, &kMoveCatalogueSites[0], kMoveCatalogueSites[0].value) && MoveSiteHolds(game, &kMoveStateSizes[0], kMoveStateSizes[0].value));
    StateMoves(game);
    assert(g_stateMoves == 0 && g_moveTries == 2);
    image[late->rva + 2] ^= 0x5A;
    StateMoves(game);
    assert(g_stateMoves == 1 && g_gameItems == MOVE_ITEMS && g_gamePackages == MOVE_PACKAGES && g_catalogue.items == MOVE_ITEMS && MoveWrittenLost(game) == 0);
    assert(MoveSiteHolds(game, late, late->value + (int32_t)(MOVE_SLOTS_NEW - MOVE_SLOTS_OLD)));
    /* holding until the state comes, then noted once */
    StateMovesSettle(game);
    assert(g_stateMoves == 1 && !g_moveStateSeen);
    *(uint64_t*)(image + MOVE_STATE) = 1;
    StateMovesSettle(game);
    assert(g_stateMoves == 1 && g_moveStateSeen);

    /* the reviewed image at the first try moves at once, without waiting */
    MoveFresh(image);
    StateMoves(game);
    assert(g_stateMoves == 1 && g_moveTries == 0 && g_moveWaitFrom == 0);

    /* the state allocated while a site is still encoded: nothing written */
    MoveFresh(image);
    image[late->rva] ^= 0xFF;
    StateMoves(game);
    assert(g_stateMoves == 0);
    *(uint64_t*)(image + MOVE_STATE) = 1;
    StateMoves(game);
    assert(g_stateMoves == -1 && g_gameItems == 0 && g_catalogue.items == MOVE_ITEMS_OLD);
    image[late->rva] ^= 0xFF;
    assert(MoveImageReviewed(game));
    StateMoves(game); /* settled: never tried again */
    assert(g_stateMoves == -1);

    /* other code for good: refused once MOVE_DECODE_WAIT has passed, nothing written */
    MoveFresh(image);
    image[kMoveCatalogueSites[100].rva] ^= 0xFF;
    StateMoves(game);
    assert(g_stateMoves == 0);
    g_moveWaitFrom = GetTickCount64() - MOVE_DECODE_WAIT - 1;
    StateMoves(game);
    assert(g_stateMoves == -1 && g_gameItems == 0);
    image[kMoveCatalogueSites[100].rva] ^= 0xFF;
    assert(MoveImageReviewed(game));

    /* a write undone before the state comes (a second decoding): all of it goes back, the backend hears the game's capacity */
    MoveFresh(image);
    StateMoves(game);
    assert(g_stateMoves == 1);
    int32_t original = kMoveCatalogueSites[7].value;
    memcpy(image + kMoveCatalogueSites[7].rva + kMoveCatalogueSites[7].at, &original, 4);
    assert(MoveWrittenLost(game) == kMoveCatalogueSites[7].rva);
    StateMovesSettle(game);
    assert(g_stateMoves == -1 && g_gameItems == 0 && g_gamePackages == 0 && g_catalogue.items == MOVE_ITEMS_OLD && g_catalogue.count == MOVE_CATALOGUE_OLD);
    assert(MoveImageReviewed(game));

    /* tests: HELLFORGE_MOVE_UNDECODED=3, three tries wait on a reviewed image and the fourth moves */
    MoveFresh(image);
    SetEnvironmentVariableW(L"HELLFORGE_MOVE_UNDECODED", L"3");
    for (int i = 0; i < 3; i++) { StateMoves(game); assert(g_stateMoves == 0 && g_moveTries == (uint32_t)i + 1); }
    StateMoves(game);
    assert(g_stateMoves == 1 && g_moveTries == 3);
    SetEnvironmentVariableW(L"HELLFORGE_MOVE_UNDECODED", NULL);

    MoveFresh(image);
    VirtualFree(image, 0, MEM_RELEASE);
    puts("PASS state moves while game.dll decodes: a site still encoded waits and moves once it reads as reviewed; the "
        "state allocated first, or the wait run out, writes nothing; a write lost before the state comes puts all back");
}

/* The armory's room: a list full counting its vanilla records refuses by type, and entity copies take no card. */
static void ArmoryRoomFixture(void)
{
    static uint8_t records[(REG_LOADOUT_RECORDS + 512) * REG_LOADOUT_STRIDE];
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
    OwnItem weapon = {OWN_WEAPON, 0xBEEF, 0, 0, 0xABCD, 0};
    OwnBatch batch = {.items = &weapon, .count = 1};
    OwnAppendAll((uintptr_t)manager, &batch);
    assert(batch.added == 0 && batch.why != NULL && strstr(batch.why, "full") != NULL);
    free(manager);
    g_catalogue = vanilla;
}
