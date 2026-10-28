/* HellForge's loadout lookup (registry_load.c LoadoutLookupDetour) against the game's own: the reviewed 143 bytes of
 * game.dll+0x4F5C10 (registry.c kLoadoutLookupPin) run here, placed so their rip-relative read of the entity state lands
 * on this fixture's, over a vanilla-shaped table of 386 slots with colliding and wrapping keys. Then HellForge's copy
 * (CopyLoadoutAndList with its lookup in): REG_LOADOUT_WIDE_SLOTS, every vanilla entity's record found as the game found
 * it, and as many new ones as the registry holds. Included by stats_test.c. */

typedef uint8_t* (*LoadoutLookupFn)(uint64_t entity);

/* `count` entities in a vanilla-shaped table (386 slots, 193 records): keys k*386 + 380 + (k % 3) collide and wrap past
 * the last slot. Record r holds r in every u32. */
static uint64_t LoKey(uint32_t k) { return (uint64_t)k * REG_LOADOUT_SLOTS + 380 + k % 3; }

static void LoadoutIndexFixture(void)
{
    /* the game's code where its read of [rip+0x2F7637C] (at +5, ending at +12) lands on our state global */
    const size_t span = 0x2F76388u + 0x1000u;
    uint8_t* region = VirtualAlloc(NULL, span, MEM_RESERVE, PAGE_NOACCESS);
    assert(region != NULL);
    assert(VirtualAlloc(region, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE) != NULL);
    memcpy(region, kLoadoutLookupPin, sizeof kLoadoutLookupPin);
    FlushInstructionCache(GetCurrentProcess(), region, sizeof kLoadoutLookupPin);
    uintptr_t savedGame = g_game;
    g_game = (uintptr_t)region - REG_LOADOUT_LOOKUP;
    uint8_t** global = (uint8_t**)(g_game + REG_STATE_GLOBAL);
    assert((uint8_t*)global == region + 0x2F76388u);
    assert(VirtualAlloc((void*)((uintptr_t)global & ~(uintptr_t)0xFFF), 0x1000, MEM_COMMIT, PAGE_READWRITE) != NULL);
    uint8_t* state = VirtualAlloc(NULL, REG_TABLES + 8 * 400, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(state != NULL);
    *global = state;
    LoadoutLookupFn game = (LoadoutLookupFn)(void*)region;

    /* a vanilla table: 32-byte header, 386 rows, 193 records */
    size_t vanillaBytes = 32 + (size_t)REG_LOADOUT_SLOTS * 16 + (size_t)REG_LOADOUT_RECORDS * REG_LOADOUT_STRIDE;
    uint8_t* vanilla = calloc(1, vanillaBytes);
    uint8_t* index = vanilla + 32;
    uint8_t* records = index + (size_t)REG_LOADOUT_SLOTS * 16;
    for (uint32_t r = 0; r < REG_LOADOUT_RECORDS; r++)
        for (int w = 0; w < 8; w++) memcpy(records + (size_t)r * REG_LOADOUT_STRIDE + 4 * w, &r, 4);
    for (uint32_t k = 0; k < REG_LOADOUT_RECORDS; k++)
    {
        uint8_t* row = FreeRow(index, REG_LOADOUT_SLOTS, 16, LoKey(k));
        assert(row != NULL);
        uint64_t key = LoKey(k);
        memcpy(row, &key, 8);
        memcpy(row + 8, &k, 4);
    }
    memcpy(state + REG_TABLES + 8 * REG_LOADOUT, &index, 8);
    uint8_t* savedTable = g_loadoutTable;
    uint32_t savedSlots = g_loadoutSlots;
    int savedLookup = g_loadoutLookup;
    g_loadoutTable = NULL;
    g_loadoutSlots = REG_LOADOUT_SLOTS;

    /* the same answer as the game's code for every entity, present or not, and for none */
    assert(game(0) == NULL && LoadoutLookupDetour(0) == NULL);
    for (uint32_t k = 0; k < REG_LOADOUT_RECORDS + 40; k++)
    {
        uint8_t* expected = game(LoKey(k));
        assert(LoadoutLookupDetour(LoKey(k)) == expected);
        assert(k < REG_LOADOUT_RECORDS ? expected == records + (size_t)k * REG_LOADOUT_STRIDE : expected == NULL);
    }

    /* HellForge's copy with its lookup in: wide, every vanilla record where the game found it, and room for the registry */
    uint64_t listEntries[2] = {0};
    uint32_t listBytes = sizeof listEntries;
    uint8_t* list = calloc(1, 32 + sizeof listEntries);
    memcpy(list + 32 - 12, &listBytes, 4);
    WeaponLoad L = {state};
    g_loadoutLookup = 1;
    assert(CopyLoadoutAndList(&L, index, list + 32));
    assert(L.slots == REG_LOADOUT_WIDE_SLOTS && L.records == L.index + (size_t)REG_LOADOUT_WIDE_SLOTS * 16 && L.recordCount == REG_LOADOUT_RECORDS);
    for (uint32_t k = 0; k < REG_LOADOUT_RECORDS + REG_MAX_WEAPONS; k++) /* the registry's worth of new rows */
    {
        uint64_t key = 0x7000000000ull + (uint64_t)k * REG_LOADOUT_WIDE_SLOTS; /* every one on the same first slot */
        uint8_t* row = k < REG_LOADOUT_RECORDS ? NULL : FreeRow(L.index, L.slots, 16, key);
        if (k < REG_LOADOUT_RECORDS) continue;
        assert(row != NULL);
        uint32_t record = L.recordCount++;
        memcpy(row, &key, 8);
        memcpy(row + 8, &record, 4);
        memset(L.records + (size_t)record * REG_LOADOUT_STRIDE, 0xA5, REG_LOADOUT_STRIDE);
    }
    g_loadoutSlots = L.slots;
    g_loadoutTable = L.index;
    memcpy(state + REG_TABLES + 8 * REG_LOADOUT, &L.index, 8);
    assert(LoadoutSlotsOf(L.index) == REG_LOADOUT_WIDE_SLOTS && LoadoutSlotsOf(index) == REG_LOADOUT_SLOTS && SlotsOf(REG_LOADOUT) == REG_LOADOUT_WIDE_SLOTS);
    for (uint32_t k = 0; k < REG_LOADOUT_RECORDS; k++)
    {
        uint8_t* found = LoadoutLookupDetour(LoKey(k));
        assert(found == L.records + (size_t)k * REG_LOADOUT_STRIDE && !memcmp(found, records + (size_t)k * REG_LOADOUT_STRIDE, REG_LOADOUT_STRIDE));
    }
    for (uint32_t k = REG_LOADOUT_RECORDS; k < REG_LOADOUT_RECORDS + REG_MAX_WEAPONS; k++)
    {
        uint8_t* found = LoadoutLookupDetour(0x7000000000ull + (uint64_t)k * REG_LOADOUT_WIDE_SLOTS);
        assert(found == L.records + (size_t)(k) * REG_LOADOUT_STRIDE && found[0] == 0xA5);
    }
    assert(LoadoutLookupDetour(0x6FFFFFFFFFull) == NULL);
    /* the copy's header says how much follows it */
    uint32_t size;
    memcpy(&size, L.table + 32 - 12, 4);
    assert(size == REG_LOADOUT_WIDE_SLOTS * 16u + (REG_LOADOUT_RECORDS + REG_MAX_WEAPONS) * REG_LOADOUT_STRIDE);
    /* the game's own table again (a reload): its 386 slots, as the game's code finds it */
    memcpy(state + REG_TABLES + 8 * REG_LOADOUT, &index, 8);
    assert(LoadoutLookupDetour(LoKey(5)) == game(LoKey(5)) && LoadoutLookupDetour(LoKey(5)) == records + 5 * REG_LOADOUT_STRIDE);

    /* without HellForge's lookup the copy is the game's index byte for byte */
    g_loadoutLookup = 0;
    WeaponLoad plain = {state};
    assert(CopyLoadoutAndList(&plain, index, list + 32));
    assert(plain.slots == REG_LOADOUT_SLOTS && !memcmp(plain.index, index, (size_t)REG_LOADOUT_SLOTS * 16) &&
        !memcmp(plain.records, records, (size_t)REG_LOADOUT_RECORDS * REG_LOADOUT_STRIDE));

    VirtualFree(L.table, 0, MEM_RELEASE);
    VirtualFree(L.newList, 0, MEM_RELEASE);
    VirtualFree(plain.table, 0, MEM_RELEASE);
    VirtualFree(plain.newList, 0, MEM_RELEASE);
    free(list);
    free(vanilla);
    VirtualFree(state, 0, MEM_RELEASE);
    VirtualFree(region, 0, MEM_RELEASE);
    g_loadoutTable = savedTable;
    g_loadoutSlots = savedSlots;
    g_loadoutLookup = savedLookup;
    g_game = savedGame;
}
