/* Room for 4096 entities a world, build 25480438 (docs/performance.md, entity-table). SpawnEntity (0xFDC140) gives
 * every entity one of the EntityManager's 2048 records (+0xF32F18, 0x18 bytes) and, with none free, reuses a live one:
 * at about 650 enemies (their parts and the map's objects make about 2,000 entities) a component then missed its
 * entity and crashed (FoleySound's spawn_finish, game.dll+0x8E20DA). The EntityManager lies in the Game block that
 * the plugin setup allocates once a process (0x4EE51C), so the block is made larger and the records and the four entity
 * maps' tables (0x1000 slots each, now 0x2000) move behind it: every constant that names them (entity_table_sites.h,
 * found by research/perf-d11/hunt2/recscan.py) moves by the same distance. Run with the physics pools, before the
 * setup; all or nothing. Included by hellforge_modloader.c before difficulty_maps.c: SafeRead, ApiWrite, Note. */

enum { ET_REC, ET_REC8, ET_REC24, ET_GAME8, ET_COUNT, ET_MAP, ET_SLOTS, ET_SIZE };
typedef struct EtSite { uint32_t rva; uint8_t kind, k; uint8_t pin[3]; uint32_t old; } EtSite;
#include "entity_table_sites.h"
#define ET_SITES (sizeof kEtSites / sizeof kEtSites[0])

#define ET_MANAGER 0xE49F0u      /* the EntityManager in the Game block */
#define ET_GAME_SIZE 0x7AF7628u
#define ET_RECORDS 0xF32F18u     /* from the EntityManager */
#define ET_RECORD 0x18u
#define ET_GAME_COUNT 0x800u
#define ET_COUNT_NEW 4096u
#define ET_MAP_SLOTS 0x2000u     /* each map at most half full */
#define ET_MAPS 4u

static int g_entityTable;        /* 1 raised, 0 the game's */
static uint32_t g_etRecords;     /* where the records went, from the EntityManager */

typedef struct EtLayout { uint32_t records, maps, size; } EtLayout;

/* Behind the Game block: the records where their distance from the old ones is a multiple of 24 (sites address them
 * as base / 8 and base / 24 too), then the maps' tables. */
static EtLayout EntityTableLayout(void)
{
    EtLayout l;
    l.records = ET_GAME_SIZE + 0x40u - ET_MANAGER;
    while ((l.records - ET_RECORDS) % 24u) l.records++;
    l.maps = (l.records + ET_COUNT_NEW * ET_RECORD + 0xFu) & ~0xFu;
    l.size = ET_MANAGER + l.maps + ET_MAPS * ET_MAP_SLOTS * 8u;
    return l;
}

/* The value a site gets; *ok 0 when its old value is not the one its kind says. */
static uint32_t EntityTableValue(const EtSite* s, const EtLayout* l, int* ok)
{
    uint32_t r = l->records + s->k, old = ET_RECORDS + s->k;
    switch (s->kind)
    {
    case ET_REC: *ok = s->old == old; return r;
    case ET_REC8: *ok = s->old * 8u == old; return r / 8u;
    case ET_REC24: *ok = s->old * 24u == old; return r / 24u;
    case ET_GAME8: *ok = s->old * 8u == ET_MANAGER + old; return (ET_MANAGER + r) / 8u;
    case ET_COUNT: *ok = s->old == ET_GAME_COUNT; return ET_COUNT_NEW;
    case ET_MAP: *ok = s->k < ET_MAPS && s->old == 0xF12EB0u + s->k * 0x8018u; return l->maps + s->k * ET_MAP_SLOTS * 8u;
    case ET_SLOTS: *ok = s->old == 0x1000u; return ET_MAP_SLOTS;
    case ET_SIZE: *ok = s->old == ET_GAME_SIZE; return l->size;
    }
    *ok = 0;
    return 0;
}

/* The first site whose code is not the reviewed one, or NULL. */
static const char* EntityTableMismatch(uintptr_t game)
{
    EtLayout l = EntityTableLayout();
    for (size_t i = 0; i < ET_SITES; i++)
    {
        uint8_t code[7];
        uint32_t value;
        int ok = 0;
        EntityTableValue(&kEtSites[i], &l, &ok);
        if (!ok || SafeRead(game + kEtSites[i].rva - 3, code, sizeof code) || memcmp(code, kEtSites[i].pin, 3)) return "a site";
        memcpy(&value, code + 3, 4);
        if (value != kEtSites[i].old) return "a site's constant";
    }
    return NULL;
}

/* With the physics pools (difficulty_maps.c), before the plugin setup allocates the Game block. */
static void EntityTableRaise(uintptr_t game)
{
    const char* differs = EntityTableMismatch(game);
    if (differs != NULL) { Note("entity table: %s is not the reviewed code; the game keeps its 2048 records", differs); return; }
    EtLayout l = EntityTableLayout();
    for (size_t i = 0; i < ET_SITES; i++)
    {
        int ok;
        uint32_t value = EntityTableValue(&kEtSites[i], &l, &ok);
        if (ApiWrite(game + kEtSites[i].rva, &value, 4) == 0) continue;
        while (i-- > 0) ApiWrite(game + kEtSites[i].rva, &kEtSites[i].old, 4);
        Note("entity table: a site could not be written; the game keeps its 2048 records");
        return;
    }
    g_etRecords = l.records;
    g_entityTable = 1;
    Note("entity table: %u records a world (the game's %u) and entity maps of %u slots, behind the Game block (%u KB more)",
        ET_COUNT_NEW, ET_GAME_COUNT, ET_MAP_SLOTS, (l.size - ET_GAME_SIZE) / 1024u);
}
