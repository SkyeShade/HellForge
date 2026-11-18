/* The entity-table raise (entity_table.c) on a synthetic game image: every site's pin and constant, refused on any
 * difference with nothing written, then every constant moved by one distance that keeps the /8 and /24 forms exact.
 * Included by stats_test.c. */

#define ETT_GAME 0x3490000u /* past the free record's marker (ET_EMPTY) */

static void EttImage(uint8_t* image)
{
    for (size_t i = 0; i < ET_SITES; i++)
    {
        memcpy(image + kEtSites[i].rva - 3, kEtSites[i].pin, 3);
        memcpy(image + kEtSites[i].rva, &kEtSites[i].old, 4);
    }
}

static uint32_t EttAt(const uint8_t* image, size_t i)
{
    uint32_t v;
    memcpy(&v, image + kEtSites[i].rva, 4);
    return v;
}

/* The spawner's body after the hook's moved instruction (mov [rsp+8], rbx): takes the next record as the game does. */
static int g_ettSpawned;
static uint64_t EttSpawnBody(void* manager, uint32_t entity, uint64_t resource, uint8_t* info, uint64_t s5, uint64_t s6)
{
    uintptr_t m = (uintptr_t)manager;
    uint32_t next = *(uint32_t*)(m + ET_NEXT);
    *(uint32_t*)(m + g_etAt + next * ET_RECORD + 8u) = entity;
    g_ettSpawned++;
    return 1;
}

static void EttGuard(uint8_t* image)
{
    uintptr_t game = (uintptr_t)image;
    memcpy(image + ET_SPAWN, kEtSpawnPin, sizeof kEtSpawnPin);
    *(uint32_t*)(image + ET_EMPTY) = 0xEEEEEEEEu;
    EntityTableGuard(game);
    assert(g_etSpawnNext != NULL && g_etCount == ET_GAME_COUNT && g_etAt == ET_RECORDS);
    VbtFar(image + ET_SPAWN + 5, (void*)EttSpawnBody);
    FlushInstructionCache(GetCurrentProcess(), image, ETT_GAME);
    uint8_t* manager = (uint8_t*)VirtualAlloc(NULL, ET_RECORDS + 0x10000u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(manager != NULL);
    g_etCount = 8;
    for (uint32_t i = 0; i < 8; i++) *(uint32_t*)(manager + ET_RECORDS + i * ET_RECORD + 8u) = 0xEEEEEEEEu;
    EtSpawnFn spawn = (EtSpawnFn)(void*)(image + ET_SPAWN);
    /* a free next record: the game's spawn */
    *(uint32_t*)(manager + ET_NEXT) = 2;
    assert(spawn(manager, 100, 0, NULL, 0, 0) == 1 && g_ettSpawned == 1 && *(uint32_t*)(manager + ET_RECORDS + 2 * ET_RECORD + 8u) == 100);
    /* the next record live: the search goes on to the next free one (past the end, from 0) */
    for (uint32_t i = 3; i < 8; i++) *(uint32_t*)(manager + ET_RECORDS + i * ET_RECORD + 8u) = 200 + i;
    assert(spawn(manager, 101, 0, NULL, 0, 0) == 1 && g_ettSpawned == 2 && *(uint32_t*)(manager + ET_NEXT) == 0);
    for (uint32_t i = 0; i < 8; i++) if (i != 2) *(uint32_t*)(manager + ET_RECORDS + i * ET_RECORD + 8u) = 300 + i;
    /* every record live: refused, no record taken */
    LONG refused = g_etRefused;
    assert(spawn(manager, 102, 0, NULL, 0, 0) == 0 && g_ettSpawned == 2 && g_etRefused == refused + 1);
    assert(*(uint32_t*)(manager + ET_RECORDS + 2 * ET_RECORD + 8u) == 100);
    g_etCount = ET_GAME_COUNT;
    VirtualFree(manager, 0, MEM_RELEASE);
}

static void EntityTableFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, ETT_GAME, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    uintptr_t game = (uintptr_t)image;
    EtLayout l = EntityTableLayout();
    /* the records behind the old block, their distance a multiple of 24, the maps after them, all inside the new size */
    assert(ET_MANAGER + l.records >= ET_GAME_SIZE && (l.records - ET_RECORDS) % 24u == 0 && l.records % 8u == 0);
    assert(l.maps >= l.records + ET_COUNT_NEW * ET_RECORD && l.maps % 16u == 0 && ET_MANAGER + l.maps + 4u * ET_MAP_SLOTS * 8u == l.size);
    int kinds[ET_SIZE + 1] = {0};
    for (size_t i = 0; i < ET_SITES; i++) kinds[kEtSites[i].kind]++;
    assert(kinds[ET_REC] && kinds[ET_REC8] && kinds[ET_REC24] && kinds[ET_GAME8] == 1 && kinds[ET_COUNT] == 8 &&
        kinds[ET_MAP] == 7 && kinds[ET_SLOTS] == 7 && kinds[ET_SIZE] == 4);

    EttImage(image);
    assert(EntityTableMismatch(game) == NULL);
    for (size_t i = 0; i < ET_SITES; i++)
    {
        image[kEtSites[i].rva - 1] ^= 1; /* the instruction */
        assert(EntityTableMismatch(game) != NULL);
        image[kEtSites[i].rva - 1] ^= 1;
        image[kEtSites[i].rva] ^= 1;     /* its constant */
        assert(EntityTableMismatch(game) != NULL);
        image[kEtSites[i].rva] ^= 1;
    }
    image[kEtSites[ET_SITES / 2].rva + 1] ^= 1;
    EntityTableRaise(game);
    assert(!g_entityTable && EttAt(image, 0) == kEtSites[0].old); /* refused, nothing written */
    image[kEtSites[ET_SITES / 2].rva + 1] ^= 1;

    EntityTableRaise(game);
    assert(g_entityTable && g_etRecords == l.records);
    for (size_t i = 0; i < ET_SITES; i++)
    {
        const EtSite* s = &kEtSites[i];
        uint32_t v = EttAt(image, i), r = l.records + s->k;
        switch (s->kind)
        {
        case ET_REC: assert(v == r); break;
        case ET_REC8: assert(v * 8u == r); break;
        case ET_REC24: assert(v * 24u == r); break;
        case ET_GAME8: assert(v * 8u == ET_MANAGER + r); break;
        case ET_COUNT: assert(v == ET_COUNT_NEW); break;
        case ET_MAP: assert(v == l.maps + s->k * ET_MAP_SLOTS * 8u); break;
        case ET_SLOTS: assert(v == ET_MAP_SLOTS); break;
        case ET_SIZE: assert(v == l.size); break;
        default: assert(0);
        }
    }
    g_entityTable = 0;
    memset(image, 0, ETT_GAME);
    EttGuard(image); /* the image stays: the guard's hook lives in it */
}
