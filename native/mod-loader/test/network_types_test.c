/* The network types RegisterNetworkTypes adds (registry.c): a fake network configuration whose 80-byte entries own two
 * arrays each, as the engine's do (constructor exe 0x175C40, destructor 0x175B60), and a fake engine allocator whose
 * allocate helper is the reviewed one (exe 0x58EC0, byte for byte). After the types are added, the engine's teardown is
 * run over every entry with a free that refuses a block freed twice: a byte copy of the base's entry freed its arrays
 * twice when the game quit (helldivers2.exe+0x5B7A43, 2026-10-25). Included by stats_test.c. */

#define NT_FREED_MAX 64
typedef struct NtAllocator { void** vtable; } NtAllocator;
static void* g_ntFreed[NT_FREED_MAX];
static int g_ntFreedCount;

/* Allocator slot 6: {pointer, size} into `out`, returned. */
static void** NtAllocate(NtAllocator* self, void** out, size_t size, uint32_t align)
{
    out[0] = calloc(1, size);
    out[1] = (void*)size;
    return out;
}

/* Allocator slot 8: a block freed twice fails the fixture. */
static void NtFree(NtAllocator* self, void* block, uint32_t flags)
{
    for (int i = 0; i < g_ntFreedCount; i++) assert(g_ntFreed[i] != block);
    assert(g_ntFreedCount < NT_FREED_MAX);
    g_ntFreed[g_ntFreedCount++] = block;
}

/* An engine array {u32 count, u32 capacity, data, allocator} at `at`, its data from the allocator. */
static void NtArray(uint8_t* at, NtAllocator* allocator, uint32_t count, uint32_t capacity, uint32_t element, uint32_t first)
{
    void* data = NULL;
    if (capacity != 0)
    {
        data = calloc(capacity, element);
        for (uint32_t i = 0; i < count && element == 4; i++) ((uint32_t*)data)[i] = first + i;
    }
    memcpy(at, &count, 4);
    memcpy(at + 4, &capacity, 4);
    memcpy(at + 8, &data, 8);
    memcpy(at + 16, &allocator, 8);
}

/* The engine's entry destructor (exe 0x175B60): each array with a capacity is freed through its allocator. */
static void NtDestroyEntry(uint8_t* entry)
{
    static const uint32_t arrays[2] = {0x30, 0x18};
    for (int i = 0; i < 2; i++)
    {
        uint32_t capacity;
        void* data;
        NtAllocator* allocator;
        memcpy(&capacity, entry + arrays[i] + 4, 4);
        memcpy(&data, entry + arrays[i] + 8, 8);
        memcpy(&allocator, entry + arrays[i] + 16, 8);
        if (capacity != 0) ((void (*)(NtAllocator*, void*, uint32_t))allocator->vtable[8])(allocator, data, 0);
    }
}

static uint32_t NtU32(const uint8_t* at) { uint32_t v; memcpy(&v, at, 4); return v; }
static uint8_t* NtPointer(const uint8_t* at) { uint8_t* v; memcpy(&v, at, 8); return v; }

static uint8_t* g_ntConfigSlot;
static uint8_t** g_ntHolder = &g_ntConfigSlot;

static void NetworkTypesFixture(void)
{
    /* the reviewed allocate helper (kAllocatePin, then mov rax, [rax]; add rsp, 0x38; ret), executable */
    static const uint8_t tail[] = {0x48, 0x8b, 0x00, 0x48, 0x83, 0xc4, 0x38, 0xc3};
    uint8_t* helper = VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(helper != NULL);
    memcpy(helper, kAllocatePin, sizeof kAllocatePin);
    memcpy(helper + sizeof kAllocatePin, tail, sizeof tail);
    FlushInstructionCache(GetCurrentProcess(), helper, 64);
    static void* vtable[9];
    vtable[6] = (void*)NtAllocate;
    vtable[7] = helper;
    vtable[8] = (void*)NtFree;
    static NtAllocator allocator = {vtable};

    /* three vanilla types (short ids 10, 20, 30); 20's key list has room beyond its count, 10's is empty */
    uint8_t* config = calloc(1, 0x88);
    NtArray(config + 0x70, &allocator, 3, 3, 80, 0);
    NtArray(config + 0x28, &allocator, 3, 3, 4, 0);
    NtArray(config + 0x40, &allocator, 3, 3, 1, 0);
    uint8_t* entries = NtPointer(config + 0x78);
    for (uint32_t i = 0; i < 3; i++)
    {
        uint8_t* entry = entries + (size_t)i * 80;
        uint32_t id = 10 * (i + 1);
        memcpy(entry, &id, 4);
        NtArray(entry + 0x18, &allocator, 2, 2, 4, 100 * id);
        NtArray(entry + 0x30, &allocator, i == 0 ? 0 : 2, i == 1 ? 4 : 2, 4, 1000 * id);
    }
    g_ntConfigSlot = config;

    char exe[MAX_PATH];
    assert(GetModuleFileNameA(NULL, exe, sizeof exe) != 0);
    const char* name = strrchr(exe, '\\') != NULL ? strrchr(exe, '\\') + 1 : exe;
    char savedModule[sizeof g_networkModule];
    memcpy(savedModule, g_networkModule, sizeof savedModule);
    uint32_t savedRva = g_networkRva, savedTypeCount = g_networkTypeCount, savedVanilla = g_networkVanillaTypes;
    LONG savedAdded = g_networkTypesAdded;
    uint8_t* savedLoadout = g_loadoutTable;
    int savedWeaponCount = g_weaponCount;
    Weapon savedWeapons[2];
    memcpy(savedWeapons, g_weapons, sizeof savedWeapons);
    snprintf(g_networkModule, sizeof g_networkModule, "%s", name);
    g_networkRva = (uint32_t)((uintptr_t)&g_ntHolder - (uintptr_t)GetModuleHandleA(NULL));
    g_loadoutTable = config; /* only checked for NULL: the entity data is loaded */
    g_networkTypesAdded = 0;
    g_networkTypeCount = 0;
    g_weaponCount = 2;
    g_weapons[0] = (Weapon){.entity = 0x100, .state = 1, .shortId = 31, .baseShortId = 20, .name = "fixture/copy-of-20"};
    g_weapons[1] = (Weapon){.entity = 0x200, .state = 1, .shortId = 32, .baseShortId = 10, .name = "fixture/copy-of-10"};

    RegisterNetworkTypes();
    assert(g_networkTypesAdded == 2 && g_networkTypeCount == 2 && g_networkVanillaTypes == 3);
    assert(NtU32(config + 0x70) == 5 && NtU32(config + 0x74) == 5 && NtPointer(config + 0x78) != entries);
    uint8_t* added = NtPointer(config + 0x78);
    uint8_t *base = added + 80, *copy = added + 3 * 80, *emptyBase = added, *emptyCopy = added + 4 * 80;
    assert(NtU32(copy) == 31 && NtU32(emptyCopy) == 32);
    /* the copy owns arrays of its own with its base's values, as many as its base has */
    for (uint32_t at = 0x18; at <= 0x30; at += 0x18)
    {
        assert(NtU32(copy + at) == NtU32(base + at) && NtU32(copy + at + 4) == NtU32(copy + at));
        assert(NtPointer(copy + at + 8) != NtPointer(base + at + 8));
        assert(!memcmp(NtPointer(copy + at + 8), NtPointer(base + at + 8), (size_t)NtU32(base + at) * 4));
        assert(NtPointer(copy + at + 16) == NtPointer(base + at + 16));
    }
    /* an empty list is not shared either: no data, nothing to free */
    assert(NtU32(emptyCopy + 0x30) == 0 && NtU32(emptyCopy + 0x34) == 0 && NtPointer(emptyCopy + 0x38) == NULL);
    assert(NtPointer(emptyCopy + 0x20) != NtPointer(emptyBase + 0x20));
    /* the quit: every entry destroyed, no block freed twice */
    g_ntFreedCount = 0;
    for (uint32_t i = 0; i < 5; i++) NtDestroyEntry(added + (size_t)i * 80);
    assert(g_ntFreedCount == 3 * 2 + 2 + 1); /* the vanilla entries' two lists each, the copies' own lists */

    /* an allocator that is not the reviewed one: the copy keeps its base's lists but does not own them */
    static void* foreignVtable[9];
    static NtAllocator foreign = {foreignVtable};
    foreignVtable[7] = (void*)NtFree; /* not the pinned helper */
    foreignVtable[8] = (void*)NtFree;
    uint8_t foreignBase[80] = {0}, foreignCopy[80];
    NtArray(foreignBase + 0x18, &foreign, 2, 2, 4, 7);
    NtArray(foreignBase + 0x30, &foreign, 2, 2, 4, 9);
    assert(CopyNetworkEntry(foreignCopy, foreignBase) == 2);
    assert(NtU32(foreignCopy + 0x1C) == 0 && NtU32(foreignCopy + 0x34) == 0 && NtU32(foreignCopy + 0x18) == 2);
    assert(NtPointer(foreignCopy + 0x20) == NtPointer(foreignBase + 0x20));
    g_ntFreedCount = 0;
    NtDestroyEntry(foreignBase);
    NtDestroyEntry(foreignCopy);
    assert(g_ntFreedCount == 2);

    memcpy(g_networkModule, savedModule, sizeof savedModule);
    g_networkRva = savedRva;
    g_networkTypeCount = savedTypeCount;
    g_networkVanillaTypes = savedVanilla;
    g_networkTypesAdded = savedAdded;
    g_loadoutTable = savedLoadout;
    g_weaponCount = savedWeaponCount;
    memcpy(g_weapons, savedWeapons, sizeof savedWeapons);
    g_ntConfigSlot = NULL;
}
