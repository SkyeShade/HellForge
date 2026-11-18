/* The entity-table raise (entity_table.c) on a synthetic game image: every site's pin and constant, refused on any
 * difference with nothing written, then every constant moved by one distance that keeps the /8 and /24 forms exact.
 * Included by stats_test.c. */

#define ETT_GAME 0x1400000u

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

static void EntityTableFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, ETT_GAME, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
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
    VirtualFree(image, 0, MEM_RELEASE);
}
