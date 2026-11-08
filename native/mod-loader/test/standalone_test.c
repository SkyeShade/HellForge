/* Standalone weapons (standalone.c): a fake entity state with three weapons of the primary class and one of another
 * loadout type with the same component list. add_weapon's declaration and the class build every record of the new
 * weapon: class values where the members agree, the most common value for handling fields, 0 for identity fields, the
 * declaration over both; a byte that differs outside the named fields refuses the weapon; no member's record changes.
 * Then its network type: the class's entry under its own short id, with arrays of its own. Included by stats_test.c. */

static const ComponentSize* SaSize(uint32_t c)
{
    for (size_t i = 0; i < sizeof kComponentSizes / sizeof kComponentSizes[0]; i++) if (kComponentSizes[i].component == c) return &kComponentSizes[i];
    return NULL;
}

static uint8_t* SaRecord(uint8_t* state, uint32_t c, uint64_t entity)
{
    uint32_t slots = SlotsOf(c), record;
    uint8_t* table = StatePointer(state, REG_TABLES + 8 * c);
    memcpy(&record, FindRow(table, slots, 16, entity) + 8, 4);
    return table + (size_t)slots * 16 + (size_t)record * ComponentStride(state, c);
}

static const ClassField* SaField(uint32_t c, const char* name)
{
    for (int f = 0; f < CLASS_FIELDS; f++) if (kPrimaryFields[f].component == c && !strcmp(kPrimaryFields[f].name, name)) return &kPrimaryFields[f];
    assert(!"a field of weapon_classes.h");
    return NULL;
}

static void StandaloneFixture(void)
{
    uintptr_t was = g_game;
    g_weaponCount = g_ownedCount = g_componentCount = 0; g_loadoutTable = NULL; g_classCount = 0; g_classFrom = NULL;
    uint8_t* game = calloc(1, REG_STATE_GLOBAL + 8);
    uint8_t* state = calloc(1, REG_LIST + 8);
    g_game = (uintptr_t)game; memcpy(game + REG_STATE_GLOBAL, &state, 8);
    static const uint64_t entities[4] = {0xA1, 0xA2, 0xA3, 0xB1}; /* three primaries, then a pistol (loadout type 3) */
    uint8_t* map = calloc(REG_MAP_SLOTS, 32);
    uint8_t* shortIds = calloc(1, 32 + 8 * 4);
    uint32_t listBytes = 8 * 4;
    memcpy(shortIds + 20, &listBytes, 4);
    static uint16_t list[CLASS_COMPONENTS];
    memcpy(list, kPrimaryComponents, sizeof list);
    for (uint32_t k = 0; k < CLASS_COMPONENTS; k++)
    {
        uint32_t c = kPrimaryComponents[k];
        const ComponentSize* z = SaSize(c);
        if (z == NULL) continue; /* a component without a table */
        uint8_t* t = FakeTable(c, z->slots * 16 + z->records * z->size);
        SetStatePointer(state, REG_TABLES + 8 * c, t);
        for (uint32_t e = 0; e < 4; e++)
        {
            FakeRow(t, z->slots, entities[e], e);
            uint8_t* r = t + (size_t)z->slots * 16 + (size_t)e * z->size;
            memset(r, 0x11, z->size); /* a class value outside every field */
            for (int f = 0; f < CLASS_FIELDS; f++)
                if (kPrimaryFields[f].component == c) memset(r + kPrimaryFields[f].offset, e == 2 ? 0x22 : 0x33, kPrimaryFields[f].size);
        }
    }
    uint8_t* loadout = StatePointer(state, REG_TABLES + 8 * REG_LOADOUT);
    for (uint32_t e = 0; e < 4; e++)
    {
        uint8_t* r = loadout + (size_t)REG_LOADOUT_SLOTS * 16 + (size_t)e * REG_LOADOUT_STRIDE;
        uint32_t id = 0x5000 + e, type = e == 3 ? 3 : CLASS_PRIMARY_LOADOUT_TYPE;
        memcpy(r, &id, 4);
        memcpy(r + 4, &type, 4);
        uint8_t* row = FreeRow(map, REG_MAP_SLOTS, 32, entities[e]);
        uint16_t* components = list;
        uint64_t count = CLASS_COMPONENTS;
        uint32_t shortId = 10 + e, mapIndex = (uint32_t)((row - map) / 32);
        memcpy(row, &entities[e], 8); memcpy(row + 8, &components, 8); memcpy(row + 16, &count, 8); memcpy(row + 24, &shortId, 4);
        memcpy(shortIds + 32 + 8 * e, &shortId, 4); memcpy(shortIds + 36 + 8 * e, &mapIndex, 4);
    }
    SetStatePointer(state, REG_MAP, map);
    SetStatePointer(state, REG_LIST, shortIds + 32);
    /* the pistol's handling values would win the vote if it counted */
    for (int f = 0; f < CLASS_FIELDS; f++)
        memset(SaRecord(state, kPrimaryFields[f].component, 0xB1) + kPrimaryFields[f].offset, 0x22, kPrimaryFields[f].size);
    uint8_t before[3][4872];
    for (int e = 0; e < 3; e++) memcpy(before[e], SaRecord(state, REG_CUSTOMIZATION, entities[e]), REG_CUSTOMIZATION_STRIDE);

    /* what a declaration must have, and what it may not name */
    HfWeapon rifle = {sizeof rifle};
    rifle.weapon_class = HF_WEAPON_CLASS_PRIMARY; rifle.unit = "fixture/rifle"; rifle.package = "fixture/rifle_package";
    rifle.designation = "BR-76"; rifle.name = "Adaptive"; rifle.reload_ability = 2785; rifle.projectile = 135; rifle.capacity = 20;
    rifle.rounds_per_minute = 540.0f; rifle.magazine_unit = "fixture/magazine"; rifle.icon = "fixture/icon";
    HfWeapon copy = rifle; copy.base = 0x9D60CB41;
    HfWeapon old = rifle; old.size = offsetof(HfWeapon, weapon_class);
    HfWeapon silent = rifle; silent.sound_from = 0x9D60CB41;
    HfWeapon unreloaded = rifle; unreloaded.reload_ability = 0;
    HfWeapon unpackaged = rifle; unpackaged.package = NULL;
    assert(!ApiAddWeapon(&g_mods[0], &copy) && !ApiAddWeapon(&g_mods[0], &old) && !ApiAddWeapon(&g_mods[0], &silent) &&
        !ApiAddWeapon(&g_mods[0], &unreloaded) && !ApiAddWeapon(&g_mods[0], &unpackaged) && g_weaponCount == 0);
    uint32_t id = ApiAddWeapon(&g_mods[0], &rifle);
    uint64_t entity = Murmur64A("fixture/rifle", 13);
    assert(id == (uint32_t)(entity >> 32) && g_weaponCount == 1 && g_weapons[0].standalone && g_weapons[0].base == 0);
    RegisterWeapons();
    Weapon* w = &g_weapons[0];
    assert(w->state == 1 && g_classCount == 3);
    uint8_t* row = FindRow(map, REG_MAP_SLOTS, 32, entity);
    uint16_t* components;
    uint64_t count;
    assert(row != NULL);
    memcpy(&components, row + 8, 8); memcpy(&count, row + 16, 8);
    assert(count == CLASS_COMPONENTS && !memcmp(components, kPrimaryComponents, sizeof kPrimaryComponents));

    /* class values, votes, zeroed identity, the declaration */
    uint8_t* magazine = SaRecord(state, 5, entity);
    const ClassField* magazines = SaField(5, "Magazines");
    assert(magazine[0] == 0x11 && magazine[magazines->offset] == 0x33 && ((HfWeaponMagazine*)magazine)->capacity == 20);
    uint8_t* wield = SaRecord(state, 111, entity);
    const ClassField* audio = SaField(111, "WieldStateAudioEvent");
    assert(audio->kind == CLASS_IDENTITY && NtU32(wield + audio->offset) == 0);
    uint64_t unit, package, icon, magazineUnit;
    memcpy(&unit, SaRecord(state, REG_UNIT, entity), 8);
    memcpy(&package, SaRecord(state, 28, entity) + 8, 8);
    memcpy(&icon, SaRecord(state, REG_TEXT_COMPONENT, entity) + 0x30, 8);
    uint8_t* custom = SaRecord(state, REG_CUSTOMIZATION, entity);
    memcpy(&magazineUnit, custom + 0x80, 8);
    assert(unit == entity && package == Murmur64A("fixture/rifle_package", 21) && icon == Murmur64A("fixture/icon", 12) &&
        magazineUnit == Murmur64A("fixture/magazine", 16) && NtU32(custom + REG_MASTERY_PACKAGE_AT) == w->mastery && w->mastery != 0);
    uint8_t* fire = SaRecord(state, REG_PROJECTILE_WEAPON, entity);
    float rpm;
    memcpy(&rpm, fire + 8, 4);
    assert(NtU32(fire) == 135 && rpm == 540.0f && NtU32(SaRecord(state, 113, entity) + 4) == 2785);
    uint8_t* text = SaRecord(state, REG_TEXT_COMPONENT, entity);
    assert(NtU32(text) == w->text[0] && w->text[0] != 0 && NtU32(text + 0x24) == w->text[3]);
    uint32_t record;
    memcpy(&record, FindRow(g_loadoutTable, SlotsOf(REG_LOADOUT), 16, entity) + 8, 4);
    uint8_t* entry = g_loadoutTable + (size_t)SlotsOf(REG_LOADOUT) * 16 + (size_t)record * REG_LOADOUT_STRIDE;
    assert(NtU32(entry) == id && NtU32(entry + 4) == CLASS_PRIMARY_LOADOUT_TYPE);
    for (int e = 0; e < 3; e++) assert(!memcmp(before[e], SaRecord(state, REG_CUSTOMIZATION, entities[e]), REG_CUSTOMIZATION_STRIDE));

    /* a byte the members do not share and no field names: another build, refused */
    {
        WeaponLoad L = {state, map, NULL, g_loadoutTable, g_loadoutTable + (size_t)SlotsOf(REG_LOADOUT) * 16, g_loadoutRecords};
        static uint8_t out[CLASS_MAX_STRIDE];
        uint32_t stride = ComponentStride(state, 69);
        assert(ClassRecord(&L, w, 69, stride, out) && out[1] == 0x11);
        SaRecord(state, 69, 0xA2)[1] = 0x44;
        assert(!ClassRecord(&L, w, 69, stride, out));
    }
    /* registered-items.json: a standalone entry, no copyOf */
    {
        char items[16384];
        RegisteredItemsText(items, sizeof items);
        char expected[160];
        snprintf(expected, sizeof expected, "{\"mixId\":%u,\"standalone\":true,\"class\":\"primary\"", id);
        assert(strstr(items, expected) != NULL && strstr(items, "copyOf") == NULL);
    }
    printf("  standalone weapon: built from %d class weapons and its declaration (votes, zeroed identity, declared fields), "
        "its own records only, refused when it names a base or the class's bytes differ unnamed\n", g_classCount);

    /* its network type: the class's entry under its own short id, its arrays its own */
    {
        static const uint8_t tail[] = {0x48, 0x8b, 0x00, 0x48, 0x83, 0xc4, 0x38, 0xc3};
        uint8_t* helper = VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        memcpy(helper, kAllocatePin, sizeof kAllocatePin);
        memcpy(helper + sizeof kAllocatePin, tail, sizeof tail);
        FlushInstructionCache(GetCurrentProcess(), helper, 64);
        static void* vtable[9];
        vtable[6] = (void*)NtAllocate; vtable[7] = helper; vtable[8] = (void*)NtFree;
        static NtAllocator allocator = {vtable};
        uint8_t* config = calloc(1, 0x88);
        NtArray(config + 0x70, &allocator, 4, 4, 80, 0);
        NtArray(config + 0x28, &allocator, 4, 4, 4, 0);
        NtArray(config + 0x40, &allocator, 4, 4, 1, 0);
        uint8_t* entries = NtPointer(config + 0x78);
        for (uint32_t i = 0; i < 4; i++)
        {
            uint8_t* e = entries + (size_t)i * 80;
            uint32_t shortId = 10 + i, size = 0x104, offset = 0x1000 * (i + 1);
            memcpy(e, &shortId, 4); memcpy(e + 4, &size, 4); memcpy(e + 8, &offset, 4);
            NtArray(e + 0x18, &allocator, 26, 26, 4, i == 3 ? 900 : 300); /* the pistol's rows are its own */
            NtArray(e + 0x30, &allocator, 26, 26, 4, 7000);
        }
        NetworkConfig n = {(uintptr_t)config, 4, entries, NtPointer(config + 0x30), NtPointer(config + 0x48)};
        uint8_t* toEntries = calloc(8, 80);
        uint8_t* toOwn = calloc(8, 4);
        uint8_t* toValues = calloc(8, 1);
        NetworkTypeArrays to = {toEntries, toOwn, toValues};
        g_networkTypeCount = 0;
        assert(AddStandaloneNetworkType(&n, &to, w, 4, 13));
        uint8_t* mine = toEntries + 4 * 80;
        assert(NtU32(mine) == w->shortId && NtU32(mine + 4) == 0x104 && NtU32(mine + 8) == 0x1000 && NtU32(mine + 0x18) == 26);
        assert(NtPointer(mine + 0x20) != NtPointer(entries + 0x20) && !memcmp(NtPointer(mine + 0x20), NtPointer(entries + 0x20), 26 * 4));
        assert(g_networkTypeCount == 1 && g_networkTypes[0].standalone && g_networkTypes[0].baseEntity == 0);
        /* members whose entries differ: none */
        ((uint32_t*)NtPointer(entries + 80 + 0x38))[3] = 1;
        assert(!AddStandaloneNetworkType(&n, &to, w, 5, 13));
        g_networkTypeCount = 0;
    }
    printf("  standalone network type: its class's entry and initial-state block under its own short id, arrays of its own\n");
    g_weaponCount = g_ownedCount = g_componentCount = 0; g_loadoutTable = NULL; g_classCount = 0; g_classFrom = NULL; g_game = was;
}
