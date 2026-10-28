/* Growing a component table (table_growth.c): the magic numbers the game's compiler chose, found again; the game's own
 * loadout lookup (game.dll+0x4F5C10, 143 bytes, placed so its rip-relative state read lands on this fixture's) run over a
 * 386-slot table, then grown in place (the table rebuilt, its six constants rewritten through ApplyGrowthPatches) and
 * run again over the larger table: every entity's record found where it was, as many new ones as fit. Included by
 * stats_test.c. */

static const uint8_t kGrowthLookup[143] = {
    0x48, 0x85, 0xc9, 0x74, 0x71, 0x48, 0x8b, 0x05, 0x7c, 0x63, 0xf7, 0x02, 0x44, 0x8b, 0xc1, 0x4c,
    0x8b, 0x90, 0x60, 0x25, 0xf1, 0x00, 0x48, 0xb8, 0xc7, 0xea, 0x0f, 0xf4, 0x48, 0x09, 0x39, 0x15,
    0x48, 0xf7, 0xe1, 0x48, 0xc1, 0xea, 0x05, 0x69, 0xc2, 0x82, 0x01, 0x00, 0x00, 0x44, 0x2b, 0xc0,
    0x45, 0x33, 0xc9, 0x0f, 0x1f, 0x40, 0x00, 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x41, 0x8b, 0xc0, 0x48, 0xc1, 0xe0, 0x04, 0x49, 0x03, 0xc2, 0x48, 0x8b, 0x10, 0x48, 0x3b, 0xd1,
    0x74, 0x27, 0x48, 0x85, 0xd2, 0x74, 0x1f, 0x41, 0x8b, 0xc0, 0x41, 0x8d, 0x50, 0x01, 0x45, 0x33,
    0xc0, 0x41, 0xff, 0xc1, 0x3d, 0x81, 0x01, 0x00, 0x00, 0x44, 0x0f, 0x45, 0xc2, 0x41, 0x81, 0xf9,
    0x82, 0x01, 0x00, 0x00, 0x72, 0xca, 0x33, 0xc0, 0xc3, 0x48, 0x85, 0xc0, 0x74, 0xf8, 0x8b, 0x40,
    0x08, 0x48, 0x05, 0xc1, 0x00, 0x00, 0x00, 0x48, 0xc1, 0xe0, 0x05, 0x49, 0x03, 0xc2, 0xc3};

/* The lookup's six constants: {offset in the function, instruction length, immediate offset, size, formula}. */
static const struct { uint8_t at, length, immOffset, immSize, formula; } kGrowthLookupSites[] = {
    {0x16, 10, 2, 8, GROW_MAGIC},          /* movabs rax, 0x15390948f40feac7 */
    {0x23, 4, 3, 1, GROW_SHIFT},           /* shr rdx, 5 */
    {0x27, 6, 2, 4, GROW_S},               /* imul eax, edx, 0x182 */
    {0x64, 5, 1, 4, GROW_S_MINUS_1},       /* cmp eax, 0x181 */
    {0x6D, 7, 3, 4, GROW_S},               /* cmp r9d, 0x182 */
    {0x81, 6, 2, 4, GROW_INDEX_SCALED}};   /* add rax, 0xc1 (16 * 386 / 32) */

typedef uint8_t* (*GrowthLookupFn)(uint64_t entity);

/* x / d as the 65-bit form computes it (table_growth.c Magic65). */
static uint64_t Divide65(uint64_t x, uint64_t magic, uint32_t shift)
{
    uint64_t hi = (uint64_t)(((unsigned __int128)x * magic) >> 64);
    return (((x - hi) >> 1) + hi) >> shift;
}

/* A busy thread, for the rewrite to pause: counts until told to stop. */
static volatile LONG g_growthSpinStop;
static DWORD WINAPI GrowthSpin(void* counter)
{
    while (!g_growthSpinStop) InterlockedIncrement((volatile LONG*)counter);
    return 0;
}

static uint64_t GrowthKey(uint32_t k) { return (uint64_t)k * 386u + 380 + k % 3; } /* colliding, wrapping keys */

static void GrowthMagicFixture(void)
{
    uint64_t magic;
    uint32_t shift;
    /* the compiler's own choices, as the game's lookups hold them: tables 29 (386), 5 (540) and 0 (152) */
    assert(Magic64(386, &magic, &shift) && magic == 0x15390948f40feac7ull && shift == 5);
    assert(Magic64(540, &magic, &shift) && magic == 0xf2b9d6480f2b9d65ull && shift == 9);
    assert(Magic64(152, &magic, &shift) && magic == 0xd79435e50d79435full && shift == 7);
    assert(!Magic64(0, &magic, &shift) && !Magic64(1, &magic, &shift));
    /* the 65-bit form: table 107's 712 (game.dll+0x4FC706, +0x4FC71F), and any divisor, exactly */
    assert(Magic65(712, &magic, &shift) && magic == 0x702e05c0b81702e1ull && shift == 9);
    for (uint64_t d = 2; d < 5000; d++)
    {
        assert(Magic65(d, &magic, &shift));
        uint64_t edges[6] = {0, d - 1, d, ~0ull, ~0ull - 1, (~0ull / d) * d - 1};
        for (int i = 0; i < 6; i++) assert(Divide65(edges[i], magic, shift) == edges[i] / d);
    }
    /* every divisor that has one divides exactly, at the edges and between */
    uint64_t x = 0x9E3779B97F4A7C15ull;
    int with = 0, without = 0;
    for (uint64_t d = 2; d < 20000; d++)
    {
        if (!Magic64(d, &magic, &shift)) { without++; continue; }
        with++;
        uint64_t probes[8] = {0, 1, d - 1, d, d + 1, ~0ull, ~0ull - d, (~0ull / d) * d};
        for (int i = 0; i < 8; i++)
            assert((uint64_t)(((unsigned __int128)probes[i] * magic) >> (64 + shift)) == probes[i] / d);
        for (int i = 0; i < 16; i++)
        {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            assert((uint64_t)(((unsigned __int128)x * magic) >> (64 + shift)) == x / d);
        }
    }
    assert(with > 0 && without > 0); /* some divisors need the 65-bit form */
    /* the slot rules */
    GrowthRules any = {100000, 1, 0, 0}, mask = {128, 1, 1, 0}, records = {100000, 5, 0, 1}, tight = {20, 1, 0, 0};
    assert(GrowthSlots(193, &any) == 386 && GrowthSlots(40, &mask) == 128 && GrowthSlots(70, &mask) == 0);
    uint32_t s = GrowthSlots(97, &records);
    assert(s >= 194 && s % 5 == 0 && Magic64(s, &magic, &shift));
    assert(GrowthSlots(11, &tight) == 0 && GrowthSlots(10, &tight) == 20);
    assert(ImmediateFits(127, 1) && !ImmediateFits(128, 1) && ImmediateFits(0x7FFFFFFF, 4) && !ImmediateFits(0x80000000ull, 4));
}

static void GrowthLookupFixture(void)
{
    /* the game's code where its read of [rip+0x2F7637C] (at +5, ending at +12) lands on the state global */
    const size_t span = 0x2F76388u + 0x1000u;
    uint8_t* region = VirtualAlloc(NULL, span, MEM_RESERVE, PAGE_NOACCESS);
    assert(region != NULL);
    assert(VirtualAlloc(region, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE) != NULL);
    memcpy(region, kGrowthLookup, sizeof kGrowthLookup);
    FlushInstructionCache(GetCurrentProcess(), region, sizeof kGrowthLookup);
    uint8_t** global = (uint8_t**)(region + 0x2F76388u);
    assert(VirtualAlloc((void*)((uintptr_t)global & ~(uintptr_t)0xFFF), 0x1000, MEM_COMMIT, PAGE_READWRITE) != NULL);
    uint8_t* state = VirtualAlloc(NULL, 0xF12560 + 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(state != NULL);
    *global = state;
    GrowthLookupFn lookup = (GrowthLookupFn)(void*)region;

    /* a 386-slot table of 193 colliding, wrapping entities; record r holds r */
    const uint32_t oldSlots = 386, records = 193, stride = 32;
    uint8_t* block = calloc(1, 32 + (size_t)oldSlots * 16 + (size_t)records * stride);
    uint8_t* table = block + 32;
    for (uint32_t r = 0; r < records; r++)
        for (int w = 0; w < 8; w++) memcpy(table + (size_t)oldSlots * 16 + (size_t)r * stride + 4 * w, &r, 4);
    for (uint32_t k = 0; k < records; k++)
    {
        uint64_t key = GrowthKey(k);
        uint8_t* row = FreeRow(table, oldSlots, 16, key);
        assert(row != NULL);
        memcpy(row, &key, 8);
        memcpy(row + 8, &k, 4);
    }
    memcpy(state + 0xF12560, &table, 8);
    for (uint32_t k = 0; k < records + 30; k++)
        assert(lookup(GrowthKey(k)) == (k < records ? table + (size_t)oldSlots * 16 + (size_t)k * stride : NULL));

    /* grown for 200 more: the slots the rules give, the table rebuilt, the constants rewritten */
    GrowthRules rules = {100000, 2, 0, 1}; /* records start at 16 * S / 32: S even */
    uint32_t slots = GrowthSlots(records + 200, &rules);
    assert(slots >= 2 * (records + 200));
    uint8_t* grown = RebuildTable(table, oldSlots, slots, (size_t)records * stride, (size_t)200 * stride);
    assert(grown != NULL);
    GrowthPatch patches[6];
    for (int i = 0; i < 6; i++)
    {
        int ok;
        patches[i] = (GrowthPatch){(uintptr_t)region + kGrowthLookupSites[i].at, kGrowthLookupSites[i].length, kGrowthLookupSites[i].immOffset,
            kGrowthLookupSites[i].immSize, {0}, GrowthValue(kGrowthLookupSites[i].formula, slots, stride, records, kGrowthLookupSites[i].formula == GROW_INDEX_SCALED ? stride : 0, &ok)};
        assert(ok);
        memcpy(patches[i].original, kGrowthLookup + kGrowthLookupSites[i].at, kGrowthLookupSites[i].length);
    }

    /* all or nothing: one instruction not as reviewed, or a value too large for its immediate, writes nothing */
    GrowthPatch wrong[6];
    memcpy(wrong, patches, sizeof wrong);
    wrong[4].original[0] ^= 0xFF;
    assert(ApplyGrowthPatches(wrong, 6, (uintptr_t)(state + 0xF12560), (const uint8_t*)1) == 1 && !memcmp(region, kGrowthLookup, sizeof kGrowthLookup));
    memcpy(wrong, patches, sizeof wrong);
    wrong[1].value = 200; /* past an imm8 */
    assert(ApplyGrowthPatches(wrong, 6, (uintptr_t)(state + 0xF12560), (const uint8_t*)1) == 1 && !memcmp(region, kGrowthLookup, sizeof kGrowthLookup));
    assert(*(uint8_t**)(state + 0xF12560) == table); /* refused: the pointer untouched too */

    /* a busy thread meanwhile: paused for the rewrite, running again after */
    static volatile LONG spins;
    spins = 0;
    g_growthSpinStop = 0;
    HANDLE worker = CreateThread(NULL, 0, GrowthSpin, (void*)&spins, 0, NULL);
    assert(worker != NULL);
    while (spins == 0) Sleep(1);
    assert(ApplyGrowthPatches(patches, 6, (uintptr_t)(state + 0xF12560), grown) == 0);
    assert(*(uint8_t**)(state + 0xF12560) == grown); /* swapped in the same pause */
    LONG before = spins;
    Sleep(20);
    assert(spins != before); /* resumed */
    InterlockedExchange(&g_growthSpinStop, 1);
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);

    /* the game's code, rewritten, over the grown table: every entity where it was, the new rows found, none else */

    for (uint32_t k = 0; k < records; k++)
    {
        uint8_t* found = lookup(GrowthKey(k));
        assert(found == grown + (size_t)slots * 16 + (size_t)k * stride && !memcmp(found, table + (size_t)oldSlots * 16 + (size_t)k * stride, stride));
    }
    for (uint32_t k = 0; k < 200; k++) /* new entities, all on one first slot */
    {
        uint64_t key = 0x7100000000ull + (uint64_t)k * slots;
        uint8_t* row = FreeRow(grown, slots, 16, key);
        assert(row != NULL);
        uint32_t record = records + k;
        memcpy(row, &key, 8);
        memcpy(row + 8, &record, 4);
    }
    for (uint32_t k = 0; k < 200; k++)
        assert(lookup(0x7100000000ull + (uint64_t)k * slots) == grown + (size_t)slots * 16 + (size_t)(records + k) * stride);
    assert(lookup(0x70FFFFFFFFull) == NULL && lookup(0) == NULL);
    uint32_t size;
    memcpy(&size, grown - 12, 4);
    assert(size == slots * 16u + (records + 200) * stride);

    VirtualFree(grown - 32, 0, MEM_RELEASE);
    free(block);
    VirtualFree(state, 0, MEM_RELEASE);
    VirtualFree(region, 0, MEM_RELEASE);
}

/* The generated sites (table_growth_sites.h): every constant's immediate, read from its reviewed bytes, is what its
 * formula gives at the table's own size; table 29's are the lookup's six above; and each table has a size for 200 more
 * rows (but 62, whose one-byte bounds stop at 127), whose rewrites all have values that fit. */
static void GrowthSitesFixture(void)
{
    int checked = 0;
    for (size_t t = 0; t < sizeof kGrowthTables / sizeof kGrowthTables[0]; t++)
    {
        const GrowthTable* table = &kGrowthTables[t];
        assert(SlotsOf(table->component) == table->slots && table->component < GROW_COMPONENTS); /* registry.c's sizes agree */
        for (int i = 0; i < table->count; i++)
        {
            const GrowthConstant* c = &kGrowthConstants[table->first + i];
            uint64_t stored = 0;
            memcpy(&stored, c->bytes + c->immOffset, c->immSize);
            int ok;
            uint64_t value = GrowthValue(c->formula, table->slots, table->stride, table->records, c->divisor, &ok);
            uint64_t mask = c->immSize == 8 ? ~0ull : (1ull << (8 * c->immSize)) - 1;
            assert(ok && (value & mask) == stored);
            checked++;
        }
        uint32_t used = table->slots / 2, more = table->component == 62 ? 40 : 200;
        uint32_t slots = GrowthSlots(used + more, &table->rules);
        assert(slots >= 2 * (used + more) && slots <= table->rules.maxSlots);
        static GrowthPatch patches[64];
        assert(table->count <= 64 && GrowthPatchesFor(table, table->slots, slots, 0x180000000ull, patches) == table->count);
        for (int i = 0; i < table->count; i++) assert(ImmediateFits(patches[i].value, patches[i].immSize));
        if (table->component == 62) assert(GrowthSlots(used + 200, &table->rules) == 0); /* past its one-byte bounds */
    }
    assert(checked == (int)(sizeof kGrowthConstants / sizeof kGrowthConstants[0]));
    /* table 29's constants are the six the lookup fixture rewrites, byte for byte */
    const GrowthTable* loadout = NULL;
    for (size_t t = 0; t < sizeof kGrowthTables / sizeof kGrowthTables[0]; t++) if (kGrowthTables[t].component == 29) loadout = &kGrowthTables[t];
    assert(loadout != NULL && loadout->count == 6 && (loadout->flags & GROWTH_NEEDS_ARMORY));
    for (int i = 0; i < 6; i++)
    {
        const GrowthConstant* c = &kGrowthConstants[loadout->first + i];
        int found = 0;
        for (int k = 0; k < 6; k++)
            if (c->rva - 0x4F5C10u == kGrowthLookupSites[k].at)
                found = c->length == kGrowthLookupSites[k].length && c->formula == kGrowthLookupSites[k].formula &&
                    !memcmp(c->bytes, kGrowthLookup + kGrowthLookupSites[k].at, c->length);
        assert(found);
    }
}

/* A vanilla table of component t at `slots` slots: `used` entities, record k for entity k, and its header. */
static uint8_t* GrowthVanillaTable(const GrowthTable* t, uint32_t used)
{
    uint32_t size = t->slots * 16u + t->records * t->stride;
    uint8_t* block = calloc(1, 32 + (size_t)size);
    uint8_t* table = block + 32;
    memcpy(block, &t->component, 2);
    memcpy(table - 12, &size, 4);
    for (uint32_t k = 0; k < used; k++)
    {
        uint64_t key = GrowthKey(k);
        uint32_t record = k % t->records;
        uint8_t* row = FreeRow(table, t->slots, 16, key);
        assert(row != NULL);
        memcpy(row, &key, 8);
        memcpy(row + 8, &record, 4);
    }
    return table;
}

/* Every constant of t in the image at `game` holds its value for `slots`, the rest of its instruction as reviewed. */
static void GrowthConstantsAre(const GrowthTable* t, uintptr_t game, uint32_t slots)
{
    for (int i = 0; i < t->count; i++)
    {
        const GrowthConstant* c = &kGrowthConstants[t->first + i];
        const uint8_t* code = (const uint8_t*)(game + c->rva);
        int ok;
        uint64_t value = GrowthValue(c->formula, slots, t->stride, t->records, c->divisor, &ok), stored = 0;
        memcpy(&stored, code + c->immOffset, c->immSize);
        uint64_t mask = c->immSize == 8 ? ~0ull : (1ull << (8 * c->immSize)) - 1;
        assert(ok && stored == (value & mask));
        assert(!memcmp(code, c->bytes, c->immOffset) && !memcmp(code + c->immOffset + c->immSize, c->bytes + c->immOffset + c->immSize,
            c->length - c->immOffset - c->immSize));
    }
}

/* GrowTable end to end on table 62 (42 slots, one-byte bounds), its constants laid out at their places in a stand-in for
 * game.dll: grown for 41 rows to 82 slots (constants rewritten, the state pointed at the new table, SlotsOf and the
 * profile following), built again at 82 when the loader makes a vanilla table anew, grown again from 82; refused past
 * its bounds and when the code is not as reviewed. */
static void GrowthTableFixture(void)
{
    const GrowthTable* t = GrowthTableOf(62);
    assert(t != NULL && t->slots == 42);
    assert(!GrowthWanted(21, 10, 42) && GrowthWanted(21, 11, 42) && !GrowthWanted(40, 0, 42));
    uintptr_t savedGame = g_game;
    int savedComponents = g_componentCount;
    uint8_t* image = VirtualAlloc(NULL, 0x2200000, MEM_RESERVE, PAGE_READWRITE);
    assert(image != NULL);
    for (int i = 0; i < t->count; i++)
    {
        const GrowthConstant* c = &kGrowthConstants[t->first + i];
        assert(VirtualAlloc(image + c->rva, c->length, MEM_COMMIT, PAGE_EXECUTE_READWRITE) != NULL);
        memcpy(image + c->rva, c->bytes, c->length);
    }
    g_game = (uintptr_t)image;
    uint8_t* state = VirtualAlloc(NULL, REG_TABLES + 8 * 400, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* vanilla = GrowthVanillaTable(t, 21);
    memcpy(state + REG_TABLES + 8 * 62, &vanilla, 8);
    Component* profile = &g_components[g_componentCount++];
    memset(profile, 0, sizeof *profile);
    profile->index = 62; profile->indices = 42; profile->recordOffset = 42 * 16; profile->records = t->records; profile->stride = t->stride;
    memcpy(profile->header, vanilla - 28, 28);

    /* a different instruction at one place: nothing written, nothing swapped */
    image[kGrowthConstants[t->first + 3].rva] ^= 0xFF;
    assert(!GrowTable(state, 62, 41) && g_grownCount == 0 && StatePointer(state, REG_TABLES + 8 * 62) == vanilla);
    image[kGrowthConstants[t->first + 3].rva] ^= 0xFF;
    GrowthConstantsAre(t, g_game, 42);

    assert(GrowTable(state, 62, 41) && SlotsOf(62) == 82);
    GrowthConstantsAre(t, g_game, 82);
    uint8_t* grown = StatePointer(state, REG_TABLES + 8 * 62);
    uint32_t size;
    memcpy(&size, grown - 12, 4);
    assert(grown != vanilla && size == 82 * 16u + t->records * t->stride);
    for (uint32_t k = 0; k < 21; k++)
    {
        uint8_t* row = FindRow(grown, 82, 16, GrowthKey(k));
        uint32_t record;
        assert(row != NULL);
        memcpy(&record, row + 8, 4);
        assert(record == k % t->records);
    }
    assert(!memcmp(grown + 82 * 16, vanilla + 42 * 16, (size_t)t->records * t->stride));
    assert(profile->indices == 82 && profile->recordOffset == 82 * 16 && !memcmp(profile->header, grown - 28, 28));

    /* the loader makes the table again at its vanilla size: built at 82 */
    uint8_t* fresh = GrowthVanillaTable(t, 21);
    memcpy(state + REG_TABLES + 8 * 62, &fresh, 8);
    RegrowLoaded(state);
    uint8_t* again = StatePointer(state, REG_TABLES + 8 * 62);
    assert(again != fresh && again != grown && FindRow(again, 82, 16, GrowthKey(20)) != NULL);
    RegrowLoaded(state); /* once */
    assert(StatePointer(state, REG_TABLES + 8 * 62) == again);

    /* grown again from 82, then nothing past its one-byte bounds */
    assert(GrowTable(state, 62, 60) && SlotsOf(62) == 120 && profile->indices == 120);
    GrowthConstantsAre(t, g_game, 120);
    assert(!GrowTable(state, 62, 70) && SlotsOf(62) == 120);

    g_grownCount = 0;
    g_componentCount = savedComponents;
    g_game = savedGame;
    VirtualFree(state, 0, MEM_RELEASE);
    VirtualFree(image, 0, MEM_RELEASE);
    free(vanilla - 32);
    free(fresh - 32);
}
