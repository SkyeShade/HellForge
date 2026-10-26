/* The polled game events of SDK 11 (heal, stats, weapon, bug hole), for game.dll build 25480438. Included by
 * event_sources.c. From HD2Runtime 0.40.1's research (event-combat, event-mission, event-wielder, event-destruction:
 * its runtime/event_sources.lua sources, the same reads, here in the game thread's update). Every read is a SafeRead; a
 * read that fails skips that check. A source starts once its pins (the instructions that use the data it reads) read as
 * reviewed, skipping any HellForge rewrote.
 *
 *   heal     each player's Helldiver: its health record ([game + 0x3326688], the health manager: the entity hash at
 *            +0x1030, records +0x1058 of 0x1B8 bytes, health i32 +0x14, life +0x19C; max health in the ext records
 *            +0x1060 of 0x1C bytes, +0x14), every update. Health up while alive or downed is a heal. The Helldiver of a
 *            player slot: its network id [player manager + 0x3A8 + 0x20 slot] through the entity manager's network map.
 *   stats    the local player's mission stats ([game + 0x3326AE0], the score manager: the player entity's index
 *            through the map at +0x37D58, its record at +0x37D98 + 0x28 index: a stat table, and 64 source blocks of
 *            0x148 bytes at [record + 0x18], each a resource and 16 stat entries), every 100 ms.
 *   weapon   the local Helldiver's hand: slot 0 of its wielder instance ([game + 0x3326420]: the map at +0x30, slots
 *            [+0x60] + 0x1D0 instance) and its inventory selection ([game + 0x3326738]: the map at +0x28, records
 *            [+0x50] + 0x30 record, +0x1C), every 100 ms. A held entity must be alive (the engine's generation, exe).
 *   bug hole the destructibles ([game + 0x3326550]: count +0xC, descriptor pointers +0x4030, world indexes +0x6030)
 *            whose entity is a bug_spawner_*, and their destruction records ([game + 0x346D528] + 0xC5F78 + 12 index:
 *            unit +0, stage u8 +7, registered bit +8), every 250 ms; the stage leaving 0 closes the hole. */

__declspec(dllexport) int hellforge_lua_rewrites(uint32_t rva, uint32_t size); /* lua_bridge.c */

#define EVP_HEALTH 0x3326688u
#define EVP_ENTITIES 0x346BF98u
#define EVP_NETWORK_MAP 0xF22EC8u
#define EVP_NETWORK_ENTITY 0xF32F20u
#define EVP_ENTITY_MAP 0xF1AEB0u
#define EVP_DESCRIPTORS 0xF32F18u
#define EVP_SCORE 0x3326AE0u
#define EVP_WIELDER 0x3326420u
#define EVP_INVENTORY 0x3326738u
#define EVP_DESTRUCTIBLES 0x3326550u
#define EVP_DESTRUCTION 0x346D528u
#define EVP_EXE_ENTITIES 0x1B135B8u
#define EVP_EXE_UNITS 0x1A100F0u
#define EVP_AVATAR 0x4D1C334D294DFA97ull

typedef struct EvpPin { int exe; uint32_t rva; uint8_t size; uint8_t bytes[12]; } EvpPin;
#define EVP_PIN(exe, rva, ...) {exe, rva, sizeof((uint8_t[]){__VA_ARGS__}), {__VA_ARGS__}}
static const EvpPin kEvpPlayerPins[] = {
    EVP_PIN(0, 0x62C71Au, 0x48, 0x8b, 0x0d, 0x47, 0x9d, 0xcf, 0x02), EVP_PIN(0, 0x62C724u, 0x44, 0x8b, 0x91, 0x84, 0x00, 0x00, 0x00),
    EVP_PIN(0, 0xACC66Cu, 0x42, 0x8b, 0x94, 0x30, 0xa8, 0x03, 0x00, 0x00), EVP_PIN(0, 0xFD9BA4u, 0x4c, 0x8b, 0x1d, 0xed, 0x23, 0x49, 0x02),
    EVP_PIN(0, 0xFD9BF9u, 0x49, 0x8b, 0xbb, 0xc8, 0x2e, 0xf2, 0x00), EVP_PIN(0, 0xFD9C50u, 0x48, 0x8d, 0x80, 0xe4, 0x65, 0x1e, 0x00),
    EVP_PIN(0, 0x12A874Au, 0x48, 0x8b, 0x05, 0x9f, 0x47, 0x1d, 0x02), EVP_PIN(0, 0x12A8751u, 0x48, 0x3b, 0x88, 0x98, 0xb3, 0x00, 0x00)};
static const EvpPin kEvpHealPins[] = {
    EVP_PIN(0, 0x4A441Au, 0x4c, 0x8b, 0x15, 0x67, 0x22, 0xe8, 0x02), EVP_PIN(0, 0x4A445Au, 0x49, 0x8b, 0xba, 0x30, 0x10, 0x00, 0x00),
    EVP_PIN(0, 0x4A44AAu, 0x48, 0x69, 0xc1, 0xb8, 0x01, 0x00, 0x00), EVP_PIN(0, 0x4A44B1u, 0x49, 0x03, 0x82, 0x58, 0x10, 0x00, 0x00),
    EVP_PIN(0, 0x91D646u, 0x48, 0x03, 0xaf, 0x60, 0x10, 0x00, 0x00), EVP_PIN(0, 0x91D69Eu, 0x41, 0x89, 0x45, 0x14),
    EVP_PIN(0, 0x91CF8Bu, 0x41, 0x89, 0x9c, 0x30, 0x9c, 0x01, 0x00, 0x00)};
static const EvpPin kEvpStatPins[] = {
    EVP_PIN(0, 0x62CD3Fu, 0x48, 0x8b, 0x35, 0x9a, 0x9d, 0xcf, 0x02), EVP_PIN(0, 0x62CD70u, 0x48, 0x05, 0x57, 0x16, 0x00, 0x00),
    EVP_PIN(0, 0x62CDA2u, 0x4c, 0x8b, 0x4e, 0x18), EVP_PIN(0, 0x62CDE3u, 0x83, 0xfa, 0x10), EVP_PIN(0, 0x6303AAu, 0x49, 0x81, 0xc5, 0x48, 0x01, 0x00, 0x00),
    EVP_PIN(0, 0x6303B1u, 0x41, 0x83, 0xfc, 0x40), EVP_PIN(0, 0x62C78Cu, 0x49, 0x8b, 0xbb, 0x58, 0x7d, 0x03, 0x00)};
static const EvpPin kEvpWeaponPins[] = {
    EVP_PIN(0, 0x9AAF53u, 0x48, 0x8b, 0x0d, 0xc6, 0xb4, 0x97, 0x02), EVP_PIN(0, 0x785E3Au, 0x4c, 0x8b, 0x59, 0x30),
    EVP_PIN(0, 0x785E9Bu, 0x48, 0x8b, 0x45, 0x60), EVP_PIN(0, 0x785EA3u, 0x4d, 0x69, 0xce, 0xd0, 0x01, 0x00, 0x00),
    EVP_PIN(0, 0x785EBAu, 0x41, 0x89, 0x3c, 0x02), EVP_PIN(0, 0xA96062u, 0x48, 0x8b, 0x0d, 0xcf, 0x06, 0x89, 0x02),
    EVP_PIN(0, 0x9AAA9Du, 0x4c, 0x8b, 0x59, 0x28), EVP_PIN(0, 0x9AAB0Au, 0x4d, 0x8b, 0x7d, 0x50), EVP_PIN(0, 0x9AAC0Fu, 0x41, 0x89, 0x6f, 0x1c),
    EVP_PIN(0, 0xFD9D50u, 0x4c, 0x8b, 0x15, 0x41, 0x22, 0x49, 0x02), EVP_PIN(0, 0xFD9D83u, 0x49, 0x8b, 0x9a, 0xb0, 0xae, 0xf1, 0x00),
    EVP_PIN(0, 0xFD9DDBu, 0x48, 0x8d, 0x80, 0xe3, 0x65, 0x1e, 0x00),
    EVP_PIN(1, 0x86A97u, 0x48, 0x89, 0x05, 0x1a, 0xcb, 0xa8, 0x01), EVP_PIN(1, 0x604755u, 0x48, 0x8b, 0x83, 0x88, 0x00, 0x00, 0x00)};
static const EvpPin kEvpHolePins[] = {
    EVP_PIN(0, 0x526646u, 0x48, 0x8b, 0x1d, 0x03, 0xff, 0xdf, 0x02), EVP_PIN(0, 0x526691u, 0xff, 0x43, 0x0c),
    EVP_PIN(0, 0x5266F0u, 0x4f, 0x89, 0x94, 0xc1, 0x30, 0x40, 0x00, 0x00), EVP_PIN(0, 0x88330Bu, 0x41, 0x89, 0x84, 0xb6, 0x30, 0x60, 0x00, 0x00),
    EVP_PIN(0, 0x13E1B64u, 0x4c, 0x8b, 0x1d, 0xbd, 0xb9, 0x08, 0x02), EVP_PIN(0, 0x13E1BE3u, 0x41, 0x0f, 0xb6, 0x84, 0x83, 0x7f, 0x5f, 0x0c, 0x00),
    EVP_PIN(0, 0x13E2161u, 0x8b, 0x9c, 0x96, 0x78, 0x5f, 0x0c, 0x00),
    EVP_PIN(1, 0x9D8CFu, 0x48, 0x8b, 0x35, 0x1a, 0x28, 0x97, 0x01), EVP_PIN(1, 0x9D909u, 0x48, 0x8b, 0x86, 0x88, 0x00, 0x00, 0x00)};

/* 1 when every pin reads as reviewed (or HellForge rewrote it), 0 not, -1 not decoded yet (try again). */
static int EvpProve(uintptr_t game, uintptr_t exe, const EvpPin* pins, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        const EvpPin* p = &pins[i];
        if (!p->exe && hellforge_lua_rewrites(p->rva, p->size)) continue;
        if (!PinMatches((p->exe ? exe : game) + p->rva, p->bytes, p->size)) return 0;
    }
    return 1;
}
typedef struct EvpSource { uint32_t kind; const EvpPin* pins; size_t count; int state; const char* what; } EvpSource;
static EvpSource g_evpSources[] = {
    {HF_ON_HEAL, kEvpHealPins, sizeof kEvpHealPins / sizeof kEvpHealPins[0], 0, "the health records"},
    {HF_ON_STAT, kEvpStatPins, sizeof kEvpStatPins / sizeof kEvpStatPins[0], 0, "the mission stats"},
    {HF_ON_WEAPON, kEvpWeaponPins, sizeof kEvpWeaponPins / sizeof kEvpWeaponPins[0], 0, "the wielders"},
    {HF_ON_BUGHOLE, kEvpHolePins, sizeof kEvpHolePins / sizeof kEvpHolePins[0], 0, "the destructibles"},
};
static int g_evpPlayers; /* the player list's pins: 1 read as reviewed, -1 not */

/* One polled source a mod listens to, proven (with the player list's pins first). */
static void EvpSourceInstall(EvpSource* s, uintptr_t game, uintptr_t exe, ULONGLONG waited)
{
    if (s->state != 0 || !AnyListener(s->kind)) return;
    if (strcmp(g_build, "25480438")) { s->state = -1; Note("events: %s not read (unsupported build); no %s events", s->what, EventName(s->kind)); return; }
    if (g_evpPlayers == 0 && EvpProve(game, exe, kEvpPlayerPins, sizeof kEvpPlayerPins / sizeof kEvpPlayerPins[0])) g_evpPlayers = 1;
    int proven = g_evpPlayers == 1 && EvpProve(game, exe, s->pins, s->count);
    if (!proven)
    {
        if (waited < 60000) return;
        if (g_evpPlayers == 0) g_evpPlayers = -1;
        s->state = -1;
        Note("events: %s are not the reviewed ones; no %s events", g_evpPlayers < 0 ? "the player list's reads" : s->what, EventName(s->kind));
        return;
    }
    s->state = 1;
    Note("events: reading %s (%s events)", s->what, EventName(s->kind));
}

/* The watch loop: proves each polled source a mod listens to (60 s for the code to decode). */
static void EventPollsInstall(uintptr_t game, ULONGLONG waited)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    for (size_t i = 0; i < sizeof g_evpSources / sizeof g_evpSources[0]; i++) EvpSourceInstall(&g_evpSources[i], game, exe, waited);
}
static int EvpOn(uint32_t kind)
{
    for (size_t i = 0; i < sizeof g_evpSources / sizeof g_evpSources[0]; i++) if (g_evpSources[i].kind == kind) return g_evpSources[i].state == 1 && AnyListener(kind);
    return 0;
}

static int EvpPtr(uintptr_t at, uintptr_t* out)
{
    uint64_t v;
    if (SafeRead(at, &v, 8) || v < 0x10000 || (v >> 32) > 0x7FFF) return 0;
    *out = (uintptr_t)v;
    return 1;
}
static uint32_t EvpU32(uintptr_t at, uint32_t fallback) { uint32_t v; return SafeRead(at, &v, 4) ? fallback : v; }

/* A found key's value at `at`: 1 with *value, 0 unreadable or none (-1). */
static int EvpHashValue(uintptr_t at, uint32_t* value)
{
    uint32_t v;
    if (SafeRead(at, &v, 4) || v == 0xFFFFFFFFu) return 0;
    *value = v;
    return 1;
}

/* The game's u32 hash maps: a header {u64 buckets, u32 capacity, u32 empty key, u32 multiplier}, buckets of `stride`
 * bytes {u32 key, ... the value at valueAt}, probed from key * multiplier. 1 with *value, 0 absent or unreadable. */
static int EvpHash(uintptr_t header, uint32_t key, uint32_t stride, uint32_t valueAt, uint32_t* value)
{
    uint8_t h[20];
    if (key == 0 || SafeRead(header, h, sizeof h)) return 0;
    uint64_t buckets; uint32_t capacity, empty, multiplier;
    memcpy(&buckets, h, 8); memcpy(&capacity, h + 8, 4); memcpy(&empty, h + 12, 4); memcpy(&multiplier, h + 16, 4);
    if (buckets < 0x10000 || capacity == 0 || capacity > 1048576) return 0;
    uint32_t start = key * multiplier, probes = capacity < 4096 ? capacity : 4096;
    for (uint32_t i = 0; i < probes; i++)
    {
        uint64_t at = buckets + (uint64_t)((start + i) % capacity) * stride;
        uint32_t k;
        if (SafeRead((uintptr_t)at, &k, 4)) return 0;
        if (k == key) return EvpHashValue((uintptr_t)at + valueAt, value);
        if (k == empty) return 0;
    }
    return 0;
}

/* An entity's descriptor through the entity manager's map: its resource (type), 0 when it does not resolve. */
static uint64_t EvpEntityResource(uintptr_t game, uint32_t entity, uint32_t* unit)
{
    uintptr_t em; uint32_t index; uint8_t d[24];
    if (!entity || !EvpPtr(game + EVP_ENTITIES, &em) || !EvpHash(em + EVP_ENTITY_MAP, entity, 8, 4, &index) || index >= 0x100000 ||
        SafeRead(em + EVP_DESCRIPTORS + (uintptr_t)index * 24, d, sizeof d)) return 0;
    uint32_t named; memcpy(&named, d + 8, 4);
    if (named != entity) return 0;
    if (unit) memcpy(unit, d + 12, 4);
    uint64_t resource; memcpy(&resource, d, 8);
    return resource;
}
/* Whether an entity is alive in the engine (its index's generation, helldivers2.exe). */
static int EvpAlive(uint32_t entity)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL), m, gens;
    uint32_t index = entity & 0x3FFFFFu, size;
    uint8_t gen;
    return EvpPtr(exe + EVP_EXE_ENTITIES, &m) && !SafeRead(m + 0x80, &size, 4) && index < size && EvpPtr(m + 0x88, &gens) &&
        !SafeRead(gens + index, &gen, 1) && gen == ((entity >> 22) & 0xFF);
}

typedef struct EvpPlayer { uint64_t peer; uint32_t player, avatar, life; int local; } EvpPlayer;
/* The player list: peer, player entity, Helldiver (0: none), lifecycle, local. Their count, -1 unreadable. */
static int EvpPlayers(uintptr_t game, EvpPlayer* out)
{
    uintptr_t manager, user, em = 0;
    uint64_t local = 0;
    if (!EvpPtr(game + EVT_PLAYERS, &manager)) return -1;
    if (EvpPtr(game + EVT_LOCAL_PEER, &user)) SafeRead(user + EVT_LOCAL_PEER_AT, &local, 8);
    uint32_t count = EvpU32(manager + 0x84, 99);
    if (count > EVT_MAX_PLAYERS) return -1;
    EvpPtr(game + EVP_ENTITIES, &em);
    for (uint32_t i = 0; i < count; i++)
    {
        EvpPlayer* p = &out[i];
        memset(p, 0, sizeof *p);
        uintptr_t descriptor;
        SafeRead(manager + 0x2C8 + 0x38 * i, &p->peer, 8);
        p->life = EvpU32(manager + 0x2E0 + 0x38 * i, 0);
        if (EvpPtr(manager + 0xE8 + 8 * i, &descriptor)) p->player = EvpU32(descriptor + 8, 0);
        p->local = p->peer != 0 && p->peer == local;
        uint32_t network = EvpU32(manager + 0x3A8 + 0x20 * i, 0x7FFF), slot;
        if (em && network != 0x7FFF && EvpHash(em + EVP_NETWORK_MAP, network, 8, 4, &slot))
            p->avatar = EvpU32(em + EVP_NETWORK_ENTITY + (uintptr_t)slot * 24, 0);
    }
    return (int)count;
}

/* ---- heal ---------------------------------------------------------------------------------------------------------- */

typedef struct EvpHealth { uint32_t avatar; uintptr_t record; int32_t health; uint32_t life; int seen; } EvpHealth;
static EvpHealth g_evpHealth[EVT_MAX_PLAYERS * 2];
static int g_evpHealthCount;

/* A Helldiver's health record: its address, health, life and maximum (-1 unknown). */
static int EvpHealthOf(uintptr_t game, uint32_t entity, uintptr_t* record, int32_t* health, uint32_t* life, int32_t* max)
{
    uintptr_t manager, records, ext;
    uint32_t index;
    if (!EvpPtr(game + EVP_HEALTH, &manager) || !EvpHash(manager + 0x1030, entity, 8, 4, &index) || index >= EvpU32(manager + 0x1020, 0) ||
        !EvpPtr(manager + 0x1058, &records)) return 0;
    *record = records + (uintptr_t)index * 0x1B8;
    if (SafeRead(*record + 0x14, health, 4) || SafeRead(*record + 0x19C, life, 4)) return 0;
    *max = -1;
    if (EvpPtr(manager + 0x1060, &ext)) SafeRead(ext + (uintptr_t)index * 0x1C + 0x14, max, 4);
    return 1;
}

static EvpHealth* EvpHealthKnown(uint32_t avatar)
{
    EvpHealth* known = NULL;
    for (int k = 0; k < g_evpHealthCount; k++) if (g_evpHealth[k].avatar == avatar) known = &g_evpHealth[k];
    return known;
}

/* A player's Helldiver against the last update: its health up while alive or downed, in the same record, is a heal. */
static void PollHealOf(uintptr_t game, const EvpPlayer* p)
{
    uintptr_t record; int32_t health, max; uint32_t life;
    if (!p->avatar || !EvpHealthOf(game, p->avatar, &record, &health, &life, &max)) return;
    EvpHealth* known = EvpHealthKnown(p->avatar);
    if (known == NULL && g_evpHealthCount < (int)(sizeof g_evpHealth / sizeof g_evpHealth[0]))
    {
        known = &g_evpHealth[g_evpHealthCount++];
        *known = (EvpHealth){p->avatar, record, health, life, 1}; /* the start: no heal on first sight */
        return;
    }
    if (known == NULL) return;
    known->seen = 1;
    if (known->record == record && life < 2 && health > known->health)
    {
        HfHealEvent e = {{sizeof e, HF_ON_HEAL, 0}, p->avatar, health - known->health, health, max, p->peer, (uint32_t)p->local};
        Dispatch(&e.info);
    }
    known->record = record; known->health = health; known->life = life;
}

static void PollHeal(uintptr_t game)
{
    EvpPlayer players[EVT_MAX_PLAYERS];
    int count = EvpPlayers(game, players);
    if (count < 0) return;
    for (int k = 0; k < g_evpHealthCount; k++) g_evpHealth[k].seen = 0;
    for (int i = 0; i < count; i++) PollHealOf(game, &players[i]);
    for (int k = 0; k < g_evpHealthCount;)
        if (!g_evpHealth[k].seen) g_evpHealth[k] = g_evpHealth[--g_evpHealthCount];
        else k++;
}

/* ---- stats --------------------------------------------------------------------------------------------------------- */

static const uint32_t kEvpStats[4] = {HF_STAT_SHOTS, HF_STAT_HITS, HF_STAT_KILLS, HF_STAT_DAMAGE};
#define EVP_SOURCE_BLOCKS 64
typedef struct EvpStats { uint32_t player; int32_t main[4], total[4]; uint64_t source[EVP_SOURCE_BLOCKS]; int32_t value[EVP_SOURCE_BLOCKS][4]; } EvpStats;
static EvpStats g_evpStats;
static int g_evpStatsHeld;
static ULONGLONG g_evpStatsNext;

/* Source block b: its resource, and its part of each stat (added to the totals). */
static void EvpStatsBlock(const uint8_t* block, int b, EvpStats* out)
{
    memcpy(&out->source[b], block, 8);
    if (out->source[b] == 0) return;
    for (int e = 0; e < 16; e++)
    {
        uint32_t key; int32_t value;
        memcpy(&key, block + 8 + e * 0x14, 4);
        if (key == 0) break;
        memcpy(&value, block + 8 + e * 0x14 + 4, 4);
        for (int k = 0; k < 4; k++) if (key == kEvpStats[k]) { out->value[b][k] += value; out->total[k] += value; }
    }
}

/* The local player's stats now: the stat table's value and each source block's part; 0 unreadable. */
static int EvpStatsOf(uintptr_t game, uint32_t player, EvpStats* out)
{
    uintptr_t score, sources;
    uint32_t index;
    memset(out, 0, sizeof *out);
    out->player = player;
    if (!player || !EvpPtr(game + EVP_SCORE, &score) || !EvpHash(score + 0x37D58, player, 8, 4, &index) || index >= 4) return 0;
    uintptr_t record = score + 0x37D98 + (uintptr_t)index * 0x28;
    if (EvpU32(record + 8, 0) == 0) return 0; /* no stat table yet */
    for (int k = 0; k < 4; k++)
    {
        uint32_t v = 0;
        EvpHash(record, kEvpStats[k], 0x14, 4, &v); /* absent: 0 */
        out->main[k] = out->total[k] = (int32_t)v;
    }
    if (!EvpPtr(record + 0x18, &sources)) return 1;
    static uint8_t blocks[EVP_SOURCE_BLOCKS * 0x148];
    if (SafeRead(sources, blocks, sizeof blocks)) return 0;
    for (int b = 0; b < EVP_SOURCE_BLOCKS; b++) EvpStatsBlock(blocks + b * 0x148, b, out);
    return 1;
}

/* A source's value of stat k at the last check (0 when it had none). */
static int32_t EvpStatBefore(uint64_t source, int k)
{
    int32_t before = 0;
    for (int c = 0; c < EVP_SOURCE_BLOCKS; c++) if (g_evpStats.source[c] == source) before = g_evpStats.value[c][k];
    return before;
}

/* A source's growth into the event's sources: the largest growths first. */
static void EvpStatSource(HfStatEvent* e, uint64_t source, int32_t grew)
{
    int at = (int)e->count;
    while (at > 0 && e->sources[at - 1].added < grew) { if (at < HF_STAT_SOURCES) e->sources[at] = e->sources[at - 1]; at--; }
    if (at < HF_STAT_SOURCES) { e->sources[at] = (HfStatSource){source, grew, 0}; if (e->count < HF_STAT_SOURCES) e->count++; }
}

/* Stat k's growth since the last check, if it grew: its event, with the sources that grew. */
static void EvpStatGrew(uint32_t player, int k, const EvpStats* now)
{
    int32_t added = now->total[k] - g_evpStats.total[k];
    if (added <= 0) return;
    HfStatEvent e = {{sizeof e, HF_ON_STAT, 0}, kEvpStats[k], added, now->total[k], 0, player, 0, {{0}}};
    int32_t main = now->main[k] - g_evpStats.main[k];
    e.unattributed = main > 0 ? main : 0;
    for (int b = 0; b < EVP_SOURCE_BLOCKS; b++)
    {
        if (now->source[b] == 0) continue;
        int32_t grew = now->value[b][k] - EvpStatBefore(now->source[b], k);
        if (grew <= 0) continue;
        EvpStatSource(&e, now->source[b], grew);
    }
    Dispatch(&e.info);
}

static void PollStats(uintptr_t game)
{
    ULONGLONG now = GetTickCount64();
    if (now < g_evpStatsNext) return;
    g_evpStatsNext = now + 100;
    EvpPlayer players[EVT_MAX_PLAYERS];
    int count = EvpPlayers(game, players);
    uint32_t player = 0;
    for (int i = 0; i < count; i++) if (players[i].local) player = players[i].player;
    if (!player) { g_evpStatsHeld = 0; return; }
    static EvpStats now_;
    if (!EvpStatsOf(game, player, &now_)) return; /* unreadable: the last one stays the start */
    int reset = !g_evpStatsHeld || g_evpStats.player != player;
    for (int k = 0; k < 4 && !reset; k++) if (now_.total[k] < g_evpStats.total[k]) reset = 1;
    if (!reset)
        for (int k = 0; k < 4; k++) EvpStatGrew(player, k, &now_);
    g_evpStats = now_;
    g_evpStatsHeld = 1;
}

/* ---- weapon -------------------------------------------------------------------------------------------------------- */

static int g_evpWeaponHeld; /* the first check sets the start */
static uint32_t g_evpWeaponAvatar, g_evpWeaponEntity, g_evpWeaponSelection;
static uint64_t g_evpWeaponResource;
static ULONGLONG g_evpWeaponNext;

/* What a Helldiver holds: 1 with *entity (0: nothing), *resource and *selection; 0 unreadable. */
static int EvpHeld(uintptr_t game, uint32_t avatar, uint32_t* entity, uint64_t* resource, uint32_t* selection)
{
    *entity = 0; *resource = 0; *selection = 0;
    uintptr_t wielder, inventory, slots, records;
    uint32_t record, instance, held;
    if (!EvpPtr(game + EVP_WIELDER, &wielder)) return 0;
    if (EvpPtr(game + EVP_INVENTORY, &inventory) && EvpHash(inventory + 0x28, avatar, 8, 4, &record) && record < 0x10000 &&
        EvpPtr(inventory + 0x50, &records)) *selection = EvpU32(records + (uintptr_t)record * 0x30 + 0x1C, 0);
    if (!EvpHash(wielder + 0x30, avatar, 8, 4, &instance) || instance >= 0x10000) return 1;
    if (!EvpPtr(wielder + 0x60, &slots) || SafeRead(slots + (uintptr_t)instance * 0x1D0, &held, 4)) return 0;
    if (held == 0 || held == avatar || !EvpAlive(held)) return 1; /* a turret holds itself */
    uint64_t r = EvpEntityResource(game, held, NULL);
    if (r == 0) return 1;
    *entity = held; *resource = r;
    return 1;
}

static void PollWeapon(uintptr_t game)
{
    ULONGLONG now = GetTickCount64();
    if (now < g_evpWeaponNext) return;
    g_evpWeaponNext = now + 100;
    EvpPlayer players[EVT_MAX_PLAYERS];
    int count = EvpPlayers(game, players);
    if (count < 0) return;
    uint32_t avatar = 0, entity = 0, selection = 0;
    uint64_t resource = 0;
    int local = 0;
    for (int i = 0; i < count; i++) if (players[i].local) { local = 1; avatar = players[i].avatar; }
    if (avatar && !EvpHeld(game, avatar, &entity, &resource, &selection)) return; /* unreadable: the last state stays */
    int changed = g_evpWeaponHeld && local && (avatar != g_evpWeaponAvatar || entity != g_evpWeaponEntity || resource != g_evpWeaponResource);
    if (changed)
    {
        HfWeaponEvent e = {{sizeof e, HF_ON_WEAPON, 0}, avatar, g_evpWeaponAvatar, entity, g_evpWeaponEntity, resource, g_evpWeaponResource, selection,
            g_evpWeaponSelection};
        /* a new Helldiver holding nothing yet after another held nothing is no change */
        if (entity != 0 || g_evpWeaponEntity != 0) Dispatch(&e.info);
    }
    g_evpWeaponHeld = 1;
    g_evpWeaponAvatar = avatar; g_evpWeaponEntity = entity; g_evpWeaponResource = resource; g_evpWeaponSelection = selection;
}

/* ---- bug holes ----------------------------------------------------------------------------------------------------- */

static const uint64_t kEvpHoles[] = {0x095686275A113614ull, 0x0DF874E208040D2Full, 0x36CC8EAD2BB18D78ull, 0x3A2CEF12ED32A088ull,
    0x4776A1CF3F19A13Bull, 0x7E4C6B45BCC45C3Full, 0x8901F188DB366B4Bull, 0x8C31B749759CBD61ull, 0x97DD3178E9F0AB70ull,
    0x9B58C95349D051F9ull, 0x9D8632A79C2D9789ull, 0xB6A181ADCF547AEBull, 0xBC2AF8548C6D5E06ull, 0xD666AA61D804D311ull,
    0xF78BF0FF5C62140Dull}; /* shrieker, base, hiveguard, bile_titan, stalker, warrior_captive, scavenger, warrior, prowler,
                               spitter, warrior_tutorial, warrior_ceiling, hunter, scavenger_captive, boomer */
#define EVP_MAX_DESTRUCTIBLES 1024
typedef struct EvpHole { uintptr_t descriptor; int32_t index; uint64_t resource; uint32_t entity, unit, owned; int hole, stage; } EvpHole;
static EvpHole g_evpHoles[EVP_MAX_DESTRUCTIBLES]; /* by the manager's instance slot */
static ULONGLONG g_evpHolesNext;
static uint32_t g_evpHolesState;

/* A unit's position (its scene graph's root, helldivers2.exe's unit registry); 0 when it does not read. */
static int EvpUnitPosition(uint32_t unit, float* position)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL), registry, gens, objects, object, vtable, method, poses;
    uint32_t index = unit & 0x3FFFFFu, count;
    uint8_t gen, code[5];
    if (!unit || !EvpPtr(exe + EVP_EXE_UNITS, &registry) || SafeRead(registry + 0x98, &count, 4) || index >= count ||
        !EvpPtr(registry + 0xA0, &gens) || SafeRead(gens + index, &gen, 1) || gen != ((unit >> 22) & 0xFF) ||
        !EvpPtr(registry + 0x88, &objects) || !EvpPtr(objects + (uintptr_t)index * 8, &object) || EvpU32(object + 8, 0) != unit ||
        !EvpPtr(object, &vtable) || !EvpPtr(vtable + 0xE8, &method) || SafeRead(method, code, 5)) return 0;
    static const uint8_t level[5] = {0x48, 0x8d, 0x41, 0x48, 0xc3}, plain[5] = {0x48, 0x8d, 0x41, 0x60, 0xc3};
    uintptr_t graph;
    if (vtable == exe + 0x16770E0u && method == exe + 0x2CAC50u && !memcmp(code, level, 5)) graph = object + 0x48;
    else if (method == exe + 0x2BD870u && !memcmp(code, plain, 5)) graph = object + 0x60;
    else return 0;
    if (!EvpPtr(graph + 0x28, &poses) || SafeRead(poses + 0x30, position, 12)) return 0;
    for (int k = 0; k < 3; k++) if (!isfinite(position[k]) || fabsf(position[k]) > 100000.0f) return 0;
    return 1;
}

/* A destructible's slot (its descriptor pointer p and world index): its descriptor read again when either changed,
 * and the range of the bug holes' indexes widened to it. */
static void EvpHoleScan(EvpHole* h, uint64_t p, int32_t index, int32_t* first, int32_t* last)
{
    if (p < 0x10000 || (p >> 32) > 0x7FFF || index < 0) { h->descriptor = 0; return; }
    if (h->descriptor != (uintptr_t)p || h->index != index)
    {
        uint8_t d[24];
        memset(h, 0, sizeof *h);
        if (SafeRead((uintptr_t)p, d, sizeof d)) return;
        h->descriptor = (uintptr_t)p; h->index = index; h->stage = -1; /* the start */
        memcpy(&h->resource, d, 8); memcpy(&h->entity, d + 8, 4); memcpy(&h->unit, d + 12, 4);
        h->owned = d[0x14] & 1;
        for (size_t k = 0; k < sizeof kEvpHoles / sizeof kEvpHoles[0]; k++) if (kEvpHoles[k] == h->resource) h->hole = 1;
    }
    if (!h->hole) return;
    if (*first < 0 || h->index < *first) *first = h->index;
    if (h->index > *last) *last = h->index;
}

/* A bug hole's destruction record (records: from index `first`): its stage leaving 0 closes it. */
static void EvpHoleStage(EvpHole* h, const uint8_t* records, int32_t first)
{
    if (!h->descriptor || !h->hole) return;
    const uint8_t* r = records + (size_t)(h->index - first) * 12;
    uint32_t unit; memcpy(&unit, r, 4);
    int stage = r[7], registered = r[8] & 1;
    if (unit != h->unit || !registered) { h->descriptor = 0; return; } /* read again next time */
    if (h->stage == 0 && stage > 0 && AnyListener(HF_ON_BUGHOLE))
    {
        HfBugholeEvent e = {{sizeof e, HF_ON_BUGHOLE, 0}, h->entity, h->unit, h->resource, (uint32_t)stage, h->owned, {0}, 0};
        e.has_position = (uint32_t)EvpUnitPosition(h->unit, e.position);
        Dispatch(&e.info);
    }
    h->stage = stage;
}

static void PollBugholes(uintptr_t game)
{
    ULONGLONG now = GetTickCount64();
    if (now < g_evpHolesNext) return;
    g_evpHolesNext = now + 250;
    uint32_t state = ReadState(game);
    if (state != g_evpHolesState) { memset(g_evpHoles, 0, sizeof g_evpHoles); g_evpHolesState = state; } /* a new mission's */
    uintptr_t manager, world;
    if (!EvpPtr(game + EVP_DESTRUCTIBLES, &manager) || !EvpPtr(game + EVP_DESTRUCTION, &world)) return;
    uint32_t count = EvpU32(manager + 0xC, 0xFFFFFFFFu);
    if (count > EVP_MAX_DESTRUCTIBLES) return;
    static uint64_t pointers[EVP_MAX_DESTRUCTIBLES];
    static int32_t indexes[EVP_MAX_DESTRUCTIBLES];
    if (count == 0 || SafeRead(manager + 0x4030, pointers, count * 8) || SafeRead(manager + 0x6030, indexes, count * 4)) return;
    int32_t first = -1, last = -1;
    for (uint32_t i = 0; i < count; i++) EvpHoleScan(&g_evpHoles[i], pointers[i], indexes[i], &first, &last);
    if (first < 0 || last - first >= 65536) return;
    size_t bytes = (size_t)(last - first + 1) * 12;
    uint8_t* records = (uint8_t*)malloc(bytes);
    if (records == NULL) return;
    if (!SafeRead(world + 0xC5F78 + (uintptr_t)first * 12, records, bytes))
        for (uint32_t i = 0; i < count; i++) EvpHoleStage(&g_evpHoles[i], records, first);
    free(records);
}

static void EventPollsTick(uintptr_t game)
{
    if (EvpOn(HF_ON_HEAL)) PollHeal(game);
    if (EvpOn(HF_ON_STAT)) PollStats(game);
    if (EvpOn(HF_ON_WEAPON)) PollWeapon(game);
    if (EvpOn(HF_ON_BUGHOLE)) PollBugholes(game);
}
