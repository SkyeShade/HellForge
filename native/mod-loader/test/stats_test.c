/* Real stats/registry code against synthetic data. No game installation or process is opened. */
#include "../hellforge_modloader.c"
#undef NDEBUG
#include <assert.h>
#include "stratagem_categories_test.c"
#include "loadout_preview_test.c"
#include "live_sync_test.c"
#include "director_test.c"
#include "shots_test.c"
#include "rounds_test.c"
#include "network_types_test.c"

static StatEdit Edit(int mod, uint32_t value, uint32_t offset, int own)
{
    StatEdit e = {0};
    e.mod = &g_mods[mod]; e.resource = 1; e.record = 0; e.offset = offset; e.value = value; e.own = own; e.shared = 1;
    strcpy(e.kind, "component"); strcpy(e.component, "Fixture"); strcpy(e.label, "fixture field");
    return e;
}

static void FixtureLoad(uint8_t* state) { StatsBeginComponents(); StatsFinishComponents(state); }

/* The game's selection for the mixing test: the partner row's chain (and the live event's objective, which mixing drops),
 * then the end hook as the stub runs it. */
static uint32_t FakeSelect(void* inputs, uint32_t* ids, int32_t* classes)
{
    uint8_t* in = inputs;
    uint32_t row, n = 0;
    memcpy(&row, in + 8, 4);
    if (row == 21) { ids[0] = 0xB1; classes[0] = 0; ids[1] = 0xB2; classes[1] = 1; ids[2] = DIF_LIVE_EVENT_OBJECTIVE; classes[2] = 2; n = 3; }
    if (row == 20) { ids[0] = 0xA1; classes[0] = 0; n = 1; }
    return n ? MixMission(in, n, ids, classes, 1) : 0;
}

/* The melee hit volumes: found by RVA in game.dll's image, checked by their type hash, written by the stats pass. */
static uint8_t* g_fakeGame;
static uintptr_t FakeGame(size_t* size) { *size = g_fakeGame ? BOOSTER_IMAGE : 0; return (uintptr_t)g_fakeGame; }
static void MeleeFixture(void)
{
    g_fakeGame = calloc(1, BOOSTER_IMAGE);
    g_gameImage = FakeGame;
    uint32_t rva = kMeleeVolumes[1].rva;
    assert(!MeleeVolume(rva));                                       /* game.dll's data not unpacked yet */
    *(uint32_t*)(g_fakeGame + kMeleeVolumes[0].rva) = kMeleeVolumes[0].hash;
    *(uint32_t*)(g_fakeGame + rva) = kMeleeVolumes[1].hash;
    *(float*)(g_fakeGame + rva + 40) = 0.5f;
    assert(MeleeVolume(rva) == (uintptr_t)g_fakeGame + rva && !MeleeVolume(rva + 0x10)); /* not a reviewed volume */
    int ready = 0;
    assert(TableRowOf("MeleeHitVolume", rva, &ready) == (uintptr_t)g_fakeGame + rva && ready && TableKnown("MeleeHitVolume"));
    g_mods[0].loaded = 1;
    g_stats[0] = Edit(0, 0x3F800000u, 40, 0); strcpy(g_stats[0].kind, "MeleeHitVolume"); g_stats[0].record = rva; g_stats[0].floating = 1;
    g_stats[1] = Edit(0, 7, 248, 0); strcpy(g_stats[1].kind, "MeleeHitVolume"); g_stats[1].record = rva;
    g_statCount = 2; g_meleeDone = 0;
    StatsMeleeReady();
    assert(g_meleeDone && g_stats[0].applied && *(float*)(g_fakeGame + rva + 40) == 1.0f && !g_stats[1].applied);
    g_statCount = 0; g_meleeDone = 0; g_gameImage = GameImage;
    free(g_fakeGame); g_fakeGame = NULL;
}

static void ExtraFixtures(void)
{
    for (int kind = 0; kind < (int)STATS_EXTRA_COUNT - 1; kind++)
    {
        const StatsLayout* d = &kStatsLayouts[kind];
        uint8_t* base = calloc(1, d->size);
        *(uint32_t*)base = d->count;
        for (uint32_t i = 0; i < d->count; i++)
        {
            const StatsGroup* g = &d->groups[i];
            uint32_t h[] = {0x444C444C, 1, g->type, g->extent, 1, 0};
            memcpy(base + g->offset, h, sizeof h);
            if (g->root)
            {
                uint64_t rows[] = {g->rowOffset, g->count};
                memcpy(base + g->root, rows, sizeof rows);
            }
        }
        g_statsAllocations[kind] = (uintptr_t)base;
        assert(StatsAllocationValid((uintptr_t)base, kind));
        const StatsGroup* g = &d->groups[0];
        uint8_t* row = base + g->root + g->rowOffset;
        *(uint32_t*)row = 123; *(uint32_t*)(row + 4) = 10;
        StatEdit e = Edit(0, 20, 4, 0); strcpy(e.kind, d->kind); e.record = 123;
        assert(StatsExtraRow(&e) == (uintptr_t)row + 4);
        g_stats[0] = e; StatsWrite(&g_stats[0], StatsExtraRow(&e)); assert(*(uint32_t*)(row + 4) == 20);
        /* Both reviewed pointer representations, ambiguous IDs and corrupt framing. */
        *(uintptr_t*)(base + g->root) = (uintptr_t)row;
        assert(StatsExtraRow(&e) == (uintptr_t)row + 4);
        *(uint32_t*)(row + d->stride) = 123; assert(!StatsExtraRow(&e));
        *(uint32_t*)(row + d->stride) = 0;
        base[g->offset] ^= 1; assert(!StatsExtraRow(&e)); base[g->offset] ^= 1;
        e.own = 1; assert(!StatsExtraRow(&e)); e.own = 0;
        if (!strcmp(d->kind, "StatusEffectSettings"))
        {
            /* A status multiplier: the row's array (+104, count +112) in the allocation, the stat checked, index * 8 + 4. */
            uint8_t* array = base + d->size - 64;
            *(uint64_t*)(row + 104) = (uintptr_t)array; *(uint64_t*)(row + 112) = 2;
            *(uint32_t*)(array + 8) = 2; *(float*)(array + 12) = 0.9f;
            StatEdit m = Edit(0, 0, 12, 0); strcpy(m.kind, "StatusMultiplier"); strcpy(m.component, "2"); m.record = 123;
            assert(StatsExtraKind("StatusMultiplier") == kind && StatsExtraRow(&m) == (uintptr_t)array + 12);
            strcpy(m.component, "3"); assert(!StatsExtraRow(&m)); strcpy(m.component, "2");
            m.offset = 20; assert(!StatsExtraRow(&m)); m.offset = 12;   /* past its count */
            *(uint64_t*)(row + 104) = (uintptr_t)array + 4096; assert(!StatsExtraRow(&m)); /* outside the allocation */
            *(uint64_t*)(row + 104) = 0; *(uint64_t*)(row + 112) = 0;
        }
        e.offset = d->stride; assert(!StatsExtraRow(&e));
        /* Discovery refuses two structurally identical allocations. */
        uint8_t* a = VirtualAlloc(NULL, d->size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        uint8_t* b = VirtualAlloc(NULL, d->size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        assert(a && b);
        *(uintptr_t*)(base + g->root) = g->rowOffset;
        memcpy(a, base, d->size); memcpy(b, base, d->size);
        StatsFindAllocations(); assert(!g_statsAllocations[kind]);
        VirtualFree(b, 0, MEM_RELEASE);
        StatsFindAllocations(); assert(g_statsAllocations[kind] == (uintptr_t)a);
        if (kind == 0)
        {
            g_stats[0] = e; g_stats[0].offset = 4; g_statCount = 1;
            StatsExtraReady(); assert(g_statsExtraDone[kind] && g_stats[0].applied);
            *(uint32_t*)(a + g->root + g->rowOffset + 4) = 7;
            StatsExtraReady(); assert(*(uint32_t*)(a + g->root + g->rowOffset + 4) == 7);
        }
        VirtualFree(a, 0, MEM_RELEASE);
        free(base); g_statsAllocations[kind] = 0;
    }
    const StatsDeltaLayout* d = &kStatsDelta;
    int kind = (int)STATS_EXTRA_COUNT - 1;
    uint8_t* base = calloc(1, d->size);
    uint32_t head[] = {d->type, 0x444C444C, 1, d->type, d->size - 28, 1, 0};
    memcpy(base, head, sizeof head);
    for (int i = 0; i < 5; i++)
    {
        uint64_t array[] = {(uintptr_t)base + d->header + d->offsets[i], d->counts[i]};
        memcpy(base + d->header + i * 16, array, sizeof array);
    }
    uint8_t* hashes = base + d->header + d->offsets[0];
    *(uint64_t*)hashes = 1; *(uint32_t*)(hashes + 8) = 2;
    uint32_t* settings = (uint32_t*)(base + d->header + d->offsets[1] + 2 * 8);
    settings[0] = 1; settings[1] = 3;
    uint32_t* component = (uint32_t*)(base + d->header + d->offsets[2] + 3 * 12);
    component[0] = 5; component[1] = 4; component[2] = 1;
    uint32_t* delta = (uint32_t*)(base + d->header + d->offsets[3] + 4 * 12);
    delta[0] = 136; delta[1] = 4; delta[2] = 7; /* Deliberately unaligned data. */
    StatEdit e = Edit(0, 60, d->header + d->offsets[4] + 7, 0);
    strcpy(e.kind, "EntityDelta"); strcpy(e.component, "5:136"); e.record = 2;
    g_statsAllocations[kind] = (uintptr_t)base;
    uint32_t value = 45; memcpy(base + e.offset, &value, 4);
    assert(StatsExtraRow(&e) == (uintptr_t)base + e.offset);
    g_stats[0] = e; StatsWrite(&g_stats[0], StatsExtraRow(&e)); memcpy(&value, base + e.offset, 4); assert(value == 60);
    delta[2]++; assert(!StatsExtraRow(&e)); delta[2]--;
    component[0]++; assert(!StatsExtraRow(&e)); component[0]--;
    settings[1] = d->counts[2]; assert(!StatsExtraRow(&e)); settings[1] = 3;
    *(uint64_t*)(hashes + 16) = 1; *(uint32_t*)(hashes + 24) = 2;
    assert(!StatsExtraRow(&e)); *(uint64_t*)(hashes + 16) = 0;
    /* Live requests (a co-op joiner getting the host's attachment values): written where the stats pass writes, reset to
     * what it held before, base to what the Local Stats mod's boot edit replaced. */
    {
        char line[256], reply[256], build[sizeof g_build], id[sizeof g_mods[0].id];
        memcpy(build, g_build, sizeof build); strcpy(g_build, "25480438");
        memcpy(id, g_mods[0].id, sizeof id); strcpy(g_mods[0].id, LIVE_MOD_ID);
        g_statsExtraDone[kind] = 1; g_statCount = 1;
        snprintf(line, sizeof line, "set\t1\tEntityDelta\t5:136\t%016llx\t2\t%u\tu32\t0000004b\t1", 1ULL, e.offset);
        LiveRequest(line, reply, sizeof reply); memcpy(&value, base + e.offset, 4); assert(!strcmp(reply, "1\tok\trespawn") && value == 75);
        snprintf(line, sizeof line, "get\t2\tEntityDelta\t5:136\t%016llx\t2\t%u\tu32", 1ULL, e.offset);
        LiveRequest(line, reply, sizeof reply); assert(!strcmp(reply, "2\tok\trespawn\t0000004b"));
        snprintf(line, sizeof line, "reset\t3\tEntityDelta\t5:136\t%016llx\t2\t%u\tu32", 1ULL, e.offset);
        LiveRequest(line, reply, sizeof reply); memcpy(&value, base + e.offset, 4); assert(!strcmp(reply, "3\tok\trespawn") && value == 60);
        snprintf(line, sizeof line, "base\t4\tEntityDelta\t5:136\t%016llx\t2\t%u\tu32", 1ULL, e.offset);
        LiveRequest(line, reply, sizeof reply); memcpy(&value, base + e.offset, 4); assert(!strcmp(reply, "4\tok\trespawn") && value == 45);
        snprintf(line, sizeof line, "reset\t5\tEntityDelta\t5:136\t%016llx\t2\t%u\tu32", 1ULL, e.offset);
        LiveRequest(line, reply, sizeof reply); memcpy(&value, base + e.offset, 4); assert(!strcmp(reply, "5\tok\trespawn") && value == 60);
        /* SDK 9 set_stat (the in-game editor's): the same writes */
        HfStat stat = {"EntityDelta", "5:136", 1, 2, e.offset, HF_TYPE_U32};
        HfValue v80 = {HF_TYPE_U32, 80.0, 0};
        assert(ApiSetStat(&g_mods[0], &stat, &v80, 0) == HF_APPLIES_RESPAWN); memcpy(&value, base + e.offset, 4); assert(value == 80);
        assert(ApiSetStat(&g_mods[0], &stat, NULL, HF_LIVE_BASE) == HF_APPLIES_RESPAWN); memcpy(&value, base + e.offset, 4); assert(value == 45);
        assert(ApiSetStat(&g_mods[0], &stat, NULL, 0) == HF_APPLIES_RESPAWN); memcpy(&value, base + e.offset, 4); assert(value == 60);
        /* stacked as live edits are: the editors' over a Lua mod's over a code mod's; taking one away shows the one under */
        {
            HfMod *lua = &g_mods[1], *code = &g_mods[2];
            char luaId[sizeof lua->id], codeId[sizeof code->id];
            int luaFlag = lua->lua, codeFlag = code->lua;
            memcpy(luaId, lua->id, sizeof luaId); memcpy(codeId, code->id, sizeof codeId);
            strcpy(lua->id, "lua.Railgun"); lua->lua = 1; strcpy(code->id, "skye.code"); code->lua = 0;
            HfValue v70 = {HF_TYPE_U32, 70.0, 0}, v90 = {HF_TYPE_U32, 90.0, 0};
            uint32_t held = 0;
#define DELTA_NOW() (memcpy(&value, base + e.offset, 4), value)
            assert(ApiSetStat(code, &stat, &v70, 0) == HF_APPLIES_RESPAWN && DELTA_NOW() == 70);
            assert(ApiSetStat(lua, &stat, &v90, 0) == HF_APPLIES_RESPAWN && DELTA_NOW() == 90);
            assert(ApiSetStat(code, &stat, &v70, 0) == HF_APPLIES_RESPAWN && DELTA_NOW() == 90); /* under the Lua mod's */
            assert(ApiSetStat(&g_mods[0], &stat, &v80, 0) == HF_APPLIES_RESPAWN && DELTA_NOW() == 80);
            assert(ApiSetStat(lua, &stat, &v90, 0) == HF_APPLIES_RESPAWN && DELTA_NOW() == 80);  /* under the editors' */
            assert(!strcmp(LiveExtraHolder((uintptr_t)base + e.offset, &held), "lua.Railgun") && held == 90);
            assert(ApiSetStat(&g_mods[0], &stat, NULL, 0) == HF_APPLIES_RESPAWN && DELTA_NOW() == 90); /* the Lua mod's shows */
            assert(ApiSetStat(&g_mods[0], &stat, NULL, HF_LIVE_BASE) == HF_APPLIES_RESPAWN && DELTA_NOW() == 90);
            assert(ApiSetStat(code, &stat, NULL, 0) == HF_APPLIES_RESPAWN && DELTA_NOW() == 90);
            assert(ApiSetStat(code, &stat, &v70, 0) == HF_APPLIES_RESPAWN);
            assert(ApiSetStat(lua, &stat, NULL, 0) == HF_APPLIES_RESPAWN && DELTA_NOW() == 70);   /* the code mod's shows */
            assert(!strcmp(LiveExtraHolder((uintptr_t)base + e.offset, &held), "skye.code") && held == 70);
            assert(ApiSetStat(lua, &stat, NULL, 0) == HF_APPLIES_RESPAWN && DELTA_NOW() == 70);   /* none of its own there */
            assert(ApiSetStat(code, &stat, NULL, 0) == HF_APPLIES_RESPAWN && DELTA_NOW() == 60 && g_liveExtraCount == 0);
            assert(LiveExtraHolder((uintptr_t)base + e.offset, &held) == NULL);
#undef DELTA_NOW
            memcpy(lua->id, luaId, sizeof luaId); memcpy(code->id, codeId, sizeof codeId); lua->lua = luaFlag; code->lua = codeFlag;
        }
        HfStat wrong = {"DamageInfo", "", 1, 2, e.offset, HF_TYPE_U32};
        assert(ApiSetStat(&g_mods[0], &wrong, &v80, 0) == 0);
        snprintf(line, sizeof line, "set\t6\tEntityDelta\t5:136\t%016llx\t9\t%u\tu32\t00000001\t1", 1ULL, e.offset);
        LiveRequest(line, reply, sizeof reply); assert(!strncmp(reply, "6\trefused\t", 10)); /* not the entity's delta record */
        g_statsExtraDone[kind] = 0;
        snprintf(line, sizeof line, "set\t7\tEntityDelta\t5:136\t%016llx\t2\t%u\tu32\t00000001\t1", 1ULL, e.offset);
        LiveRequest(line, reply, sizeof reply); assert(!strncmp(reply, "7\trefused\t", 10) && strstr(reply, "not loaded"));
        g_statCount = 0; g_liveExtraCount = 0;
        memcpy(g_build, build, sizeof build); memcpy(g_mods[0].id, id, sizeof id);
    }
    *(uint64_t*)(base + d->header) = 0; assert(!StatsExtraRow(&e));
    free(base); g_statsAllocations[kind] = 0;
}

/* The network apply switch (netapply.c) on a synthetic game.dll: the vanilla table, the pinned instructions and an entity
 * map with two added types, one with its base's component list (it gets its base's case) and one without (it keeps the
 * exit); then the refusals: another switch, and a co-op session already running. */
static void NetSwitchFixture(void)
{
    size_t size = REG_STATE_GLOBAL + 0x1000; /* past the entity state's global */
    uint8_t* game = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t* state = VirtualAlloc(NULL, REG_LIST + 0x100, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(game && state);
    memcpy(game + NET_SWITCH_BOUND, kNetBoundPin, sizeof kNetBoundPin);
    memcpy(game + NET_SWITCH_BASE, kNetBasePin, sizeof kNetBasePin);
    memcpy(game + NET_SWITCH_LOAD, kNetLoadPin, sizeof kNetLoadPin);
    memcpy(game + NET_SWITCH_EXIT, kNetExitPin, sizeof kNetExitPin);
    uint32_t* vanilla = (uint32_t*)(game + NET_SWITCH_TABLE);
    for (uint32_t i = 0; i < NET_VANILLA_TYPES; i++) vanilla[i] = 0xBC3000u + i * 0x10u; /* a distinct case per type */
    memcpy(game + REG_STATE_GLOBAL, &state, 8);
    static uint16_t baseList[] = {1, 2, 3}, otherList[] = {1, 2};
    uint8_t* map = calloc(REG_MAP_SLOTS, 32);
    struct { uint64_t entity; uint16_t* list; uint64_t count; } rows[] = {
        {0x1000, baseList, 3}, {0x2001, baseList, 3}, {0x2002, otherList, 2}};
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
    {
        uint8_t* row = FreeRow(map, REG_MAP_SLOTS, 32, rows[i].entity);
        memcpy(row, &rows[i].entity, 8); memcpy(row + 8, &rows[i].list, 8); memcpy(row + 16, &rows[i].count, 8);
    }
    memcpy(state + REG_MAP, &map, 8);
    uintptr_t savedGame = g_game;
    g_game = (uintptr_t)game;
    LONG savedAdded = g_networkTypesAdded;
    g_networkTypesAdded = 2;
    g_networkVanillaTypes = NET_VANILLA_TYPES;
    g_networkTypes[0] = (NetworkType){NET_VANILLA_TYPES, 783, 0x2001, 0x1000, "fixture/flak", NULL};
    g_networkTypes[1] = (NetworkType){NET_VANILLA_TYPES + 1, 615, 0x2002, 0x1000, "fixture/odd", NULL};
    g_networkTypeCount = 2;

    /* another machine's switch: nothing written until the grace period ends, then refused */
    game[NET_SWITCH_BOUND + 1] = 0x3F;
    g_netSwitchState = 0;
    assert(InstallNetSwitch((uintptr_t)game, 0) == 0 && g_netSwitchState == 0);
    assert(InstallNetSwitch((uintptr_t)game, 1) == 1 && g_netSwitchState == -1);
    game[NET_SWITCH_BOUND + 1] = 0x3E;
    /* a table not decoded yet (an entry outside the switch's cases): waits, then refused, nothing written */
    uint32_t* vanillaTable = (uint32_t*)(game + NET_SWITCH_TABLE);
    uint32_t kept = vanillaTable[100];
    vanillaTable[100] = 0x5A5A5A5Au;
    g_netSwitchState = 0;
    assert(InstallNetSwitch((uintptr_t)game, 0) == 0 && g_netSwitchState == 0 && !memcmp(game + NET_SWITCH_LOAD, kNetLoadPin, sizeof kNetLoadPin));
    assert(InstallNetSwitch((uintptr_t)game, 1) == 1 && g_netSwitchState == -1 && !memcmp(game + NET_SWITCH_LOAD, kNetLoadPin, sizeof kNetLoadPin));
    vanillaTable[100] = kept;
    /* a co-op session already running: refused, nothing written */
    g_netSwitchState = 0;
    g_session.role = HF_SESSION_JOINER;
    assert(InstallNetSwitch((uintptr_t)game, 1) == 1 && g_netSwitchState == -1 && !memcmp(game + NET_SWITCH_LOAD, kNetLoadPin, sizeof kNetLoadPin));
    g_session.role = HF_SESSION_SOLO;

    /* installed: the flak copy takes its base's case, the odd one keeps the exit; displacement then bound */
    g_netSwitchState = 0;
    assert(InstallNetSwitch((uintptr_t)game, 1) == 1 && g_netSwitchState == 1 && g_netSwitchTable != NULL);
    uint32_t* table = g_netSwitchTable;
    int32_t displacement;
    uint32_t bound;
    memcpy(&displacement, game + NET_SWITCH_LOAD + 3, 4);
    memcpy(&bound, game + NET_SWITCH_BOUND + 1, 4);
    assert((uintptr_t)game + (intptr_t)displacement == (uintptr_t)table && bound == NET_VANILLA_TYPES + 1);
    assert(table[0] == vanilla[0] && table[NET_VANILLA_TYPES - 1] == vanilla[NET_VANILLA_TYPES - 1]);
    assert(table[NET_VANILLA_TYPES] == vanilla[783] && table[NET_VANILLA_TYPES + 1] == NET_SWITCH_EXIT && table[NET_TABLE_SLOTS - 1] == NET_SWITCH_EXIT);
    assert(InstallNetSwitch((uintptr_t)game, 1) == 1); /* once */

    g_netSwitchState = 0;
    g_netSwitchTable = NULL;
    g_networkTypeCount = 0;
    g_networkVanillaTypes = 0;
    g_networkTypesAdded = savedAdded;
    g_game = savedGame;
    free(map);
    VirtualFree(state, 0, MEM_RELEASE);
    VirtualFree(game, 0, MEM_RELEASE);
}

/* The difficulty registry against a synthetic game.dll image: the 19 table copies, every pinned instruction, bound and
 * kept caller where the reviewed build has them. */
static void DifficultyFixture(void)
{
    size_t size = 0x37A0000; /* up to the missions table */
    uint8_t* game = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(game);
    uintptr_t base = (uintptr_t)game;
    for (size_t c = 0; c < sizeof kDifficultyCopies / sizeof kDifficultyCopies[0]; c++)
        for (uint32_t i = 0; i < DIF_VANILLA; i++)
        {
            uint8_t* row = game + kDifficultyCopies[c] + i * DIF_ROW;
            uint32_t level = i + 1;
            float points = 60.0f * level, cooldown = 12.0f / level;
            memcpy(row, &kDifficultyItems[i], 4); memcpy(row + 4, &level, 4); memcpy(row + 0xE4, &points, 4);
            memcpy(row + 0xEC, &level, 4); memcpy(row + 0x104, &cooldown, 4);
            float intervals[3] = {65.0f, 78.0f, 75.0f}; uint32_t caps[3] = {41, 39, 48};
            memcpy(row + 0x2A0, intervals, 12); memcpy(row + 0x2AC, caps, 12);
            uint64_t sprite = 0x5A000 + level; memcpy(row + 0x10, &sprite, 8);
            float radius = 1.0f, structured = 0.85f; memcpy(row + 0xF0, &radius, 4); memcpy(row + 0xF8, &structured, 4);
        }
    for (size_t i = 0; i < sizeof kDifficultySites / sizeof kDifficultySites[0]; i++)
    {
        const DifficultySite* t = &kDifficultySites[i];
        int32_t disp = DifficultyDisplacement(base + kDifficultyCopies[t->copy], base, t);
        memcpy(game + t->rva + t->dispAt, &disp, 4);
    }
    for (size_t i = 0; i < sizeof kDifficultyBounds / sizeof kDifficultyBounds[0]; i++) game[kDifficultyBounds[i].rva + kDifficultyBounds[i].immAt] = 10;
    for (size_t i = 0; i < sizeof kDifficultyKeepTen / sizeof kDifficultyKeepTen[0]; i++)
    {
        int32_t rel = (int32_t)(DIF_MAX_FN - (kDifficultyKeepTen[i] + 5));
        game[kDifficultyKeepTen[i]] = 0xE8; memcpy(game + kDifficultyKeepTen[i] + 1, &rel, 4);
    }
    memcpy(game + DIF_BAND_SITE, kBandSitePin, sizeof kBandSitePin);
    memcpy(game + DIF_MAP_TYPE_SITE, kMapTypePin, 7); memcpy(game + DIF_SPACING_SITE, kSpacingPin, 5); memcpy(game + DIF_SPACING_SITE_2, kSpacingPin2, 5); memcpy(game + DIF_GRID_SITE, kGridPin, 9); memcpy(game + DIF_CELL_SPACING_SITE, kCellSpacingPin, 7); memcpy(game + DIF_FINALIZE_SPACING_SITE, kFinalizeSpacingPin, 9);
    for (int i = 0; i < 5; i++) memcpy(game + kStampIndex[i].rva, kStampIndex[i].pin, kStampIndex[i].length);
    memcpy(game + DIF_ZONE_SITE, kZonePin, sizeof kZonePin);
    memcpy(game + DIF_ZONE_CLEAR, kZoneClearPin, 21); memcpy(game + DIF_ZONE_TABLE_LEA, kZoneTableLeaPin, 8); memcpy(game + DIF_ZONE_HEIGHT, kZoneHeightPin, 7);
    for (int i = 0; i < 7; i++) memcpy(game + kCityCapacities[i].rva, kCityCapacities[i].pin, kCityCapacities[i].length);
    memcpy(game + DIF_FOOTPRINT_CHECK, kFootprintPin, 9);
    for (int i = 0; i < 3; i++) memcpy(game + kReVoronoiStubs[i].site, kReVoronoiStubs[i].pin, kReVoronoiStubs[i].length);
    for (int i = 0; i < 8; i++) memcpy(game + kBucketPool[i].rva, kBucketPool[i].pin, kBucketPool[i].length);
    for (int i = 0; i < 4; i++) memcpy(game + kClusterSites[i].site, kClusterSites[i].pin, kClusterSites[i].length);
    memcpy(game + DIF_CULL_RADIUS, kCullRadiusPin, 8);
    memcpy(game + DIF_VISTA_POSE, kVistaPosePin, 7);
    for (int i = 0; i < 4; i++) memcpy(game + kClusterAppends[i].site, kClusterAppends[i].pin, kClusterAppends[i].load + kClusterAppends[i].store + kClusterAppends[i].inc);
    memcpy(game + DIF_FLAGS_CLEAR, kFlagsClearPin, 32); memcpy(game + DIF_FLAGS_READ, kFlagsReadPin, 4);
    for (int i = 0; i < 3; i++) memcpy(game + kFlagsWrites[i].site, kFlagsWrites[i].pin, kFlagsWrites[i].length);
    for (int i = 0; i < 2; i++) memcpy(game + kFlagsChecks[i], kFlagsCheckPins[i], 7);
    memcpy(game + DIF_PREVIEW_RECT_SITE, kPreviewRectPin, sizeof kPreviewRectPin); memcpy(game + DIF_CONVOY_SITE, kConvoyPin, sizeof kConvoyPin);
    memcpy(game + DIF_CAPTURE_SITE, kCapturePin, sizeof kCapturePin);
    memcpy(game + DIF_TELEMETRY_SITE, kTelemetryPin, sizeof kTelemetryPin);
    memcpy(game + DIF_SPAWNER_SITE, kSpawnerPin, sizeof kSpawnerPin);
    memcpy(game + DIF_MARKER_LOOP_SITE, kMarkerLoopPin, sizeof kMarkerLoopPin);
    memcpy(game + DIF_MARKER_ROUTE_SITE, kMarkerRoutePin, sizeof kMarkerRoutePin);
    memcpy(game + DIF_TIMER_SITE, kTimerPin, sizeof kTimerPin);
    memcpy(game + DIF_LAUNCH_SITE, kLaunchPin, sizeof kLaunchPin);
    memcpy(game + DIF_CARD_TIME_SITE, kCardTimePin, sizeof kCardTimePin);
    memcpy(game + DIF_WIDGET_TIME_SITE, kWidgetTimePin, sizeof kWidgetTimePin);
    memcpy(game + DIF_CARD_VIEW_SITE, kCardViewPin, sizeof kCardViewPin); memcpy(game + DIF_CARD_VIEW_CALL, kCardViewCallPin, 5);
    for (int i = 0; i < 9; i++)
    {
        int32_t disp; memcpy(&disp, kCardScales[i].pin + kCardScales[i].at, 4);
        memcpy(game + kCardScales[i].rva, kCardScales[i].pin, kCardScales[i].length);
        memcpy(game + kCardScales[i].rva + kCardScales[i].length + disp, &kCardScaleValues[kCardScales[i].global], 4);
    }
    memcpy(game + DIF_HUD_ICON_READ, kHudIconPin, sizeof kHudIconPin);
    for (int i = 0; i < DIF_LEVEL_LOADS; i++) memcpy(game + kLevelLoads[i].rva, kLevelLoads[i].pin, kLevelLoads[i].length);
    for (int i = 0; i < 5; i++) memcpy(game + kLevelInPlace[i].rva, kLevelInPlace[i].pin, kLevelInPlace[i].length);
    game[DIF_HUD_TARGET_SIB] = 0x8D;
    for (int i = 0; i < 4; i++) memcpy(game + kHoloStubs[i].site, kHoloStubs[i].pin, kHoloStubs[i].length);
    for (int i = 0; i < 3; i++) memcpy(game + kIconSites[i].read, kIconSites[i].pin, 20);
    memcpy(game + DIF_MAPVIEW_MIN, kMapViewMinPin, 9); memcpy(game + DIF_MAPVIEW_CAP, kMapViewCapPin, 9);
    for (size_t i = 0; i < sizeof kLocationRefs / sizeof kLocationRefs[0]; i++) memcpy(game + kLocationRefs[i].rva + kLocationRefs[i].at, &kLocationRefs[i].value, 4);
    for (size_t i = 0; i < sizeof kLocationCaps / sizeof kLocationCaps[0]; i++) memcpy(game + kLocationCaps[i].rva + kLocationCaps[i].at, &kLocationCaps[i].value, 4);
    for (size_t i = 0; i < sizeof kGenerationBlockSize / sizeof kGenerationBlockSize[0]; i++) memcpy(game + kGenerationBlockSize[i].rva + kGenerationBlockSize[i].at, &kGenerationBlockSize[i].value, 4);
    for (size_t i = 0; i < sizeof kLocationTypeRefs / sizeof kLocationTypeRefs[0]; i++) memcpy(game + kLocationTypeRefs[i].rva + kLocationTypeRefs[i].at, &kLocationTypeRefs[i].value, 4);
    for (size_t i = 0; i < sizeof kLocationTypeCaps / sizeof kLocationTypeCaps[0]; i++) memcpy(game + kLocationTypeCaps[i].rva + kLocationTypeCaps[i].at, &kLocationTypeCaps[i].value, 4);
    for (size_t i = 0; i < sizeof kMinimapRefs / sizeof kMinimapRefs[0]; i++) memcpy(game + kMinimapRefs[i].rva + kMinimapRefs[i].at, &kMinimapRefs[i].value, 4);
    for (size_t i = 0; i < sizeof kMinimapCaps / sizeof kMinimapCaps[0]; i++) memcpy(game + kMinimapCaps[i].rva + kMinimapCaps[i].at, &kMinimapCaps[i].value, 4);
    for (size_t i = 0; i < sizeof kStampRefs / sizeof kStampRefs[0]; i++) memcpy(game + kStampRefs[i].rva + kStampRefs[i].at, &kStampRefs[i].value, 4);
    for (size_t i = 0; i < sizeof kPlaceListRefs / sizeof kPlaceListRefs[0]; i++) memcpy(game + kPlaceListRefs[i].rva, &kPlaceListRefs[i].value, 4);
    for (size_t i = 0; i < sizeof kPlaceSetRefs / sizeof kPlaceSetRefs[0]; i++) memcpy(game + kPlaceSetRefs[i].rva, &kPlaceSetRefs[i].value, 4);
    for (int i = 0; i < 16; i++) { memcpy(game + kClusterRefs[i].rva + kClusterRefs[i].at, &kClusterRefs[i].value, 4); memcpy(game + kClusterCaps[i].rva + kClusterCaps[i].at, &kClusterCaps[i].value, 4); }
    memcpy(game + kClusterCountSites[0].site, kClusterCountPinA, 9); memcpy(game + kClusterCountSites[1].site, kClusterCountPinB, 7);
    for (int i = 0; i < 74; i++) memcpy(game + kObjectiveListSites[i].rva, kObjectiveListSites[i].pin, kObjectiveListSites[i].length);
    memcpy(game + DIF_TELEMETRY_COUNT_SITE, kTelemetryCountPin, 10); memcpy(game + DIF_TELEMETRY_LOOP_SITE, kTelemetryLoopPin, 11);
    memcpy(game + DIF_SELECTION_END, kSelectionPin, 7);
    memcpy(game + DIF_GRANT_SITE, kGrantPin, 7);
    memcpy(game + DIF_SPREAD_REFERENCE, kSpreadReferencePin, 10);
    memcpy(game + DIF_HASH_INSERT_END, kHashInsertEndPin, sizeof kHashInsertEndPin);
    memcpy(game + DIF_BOARD_REGION_MAX, kBoardRegionMaxPin, 5); memcpy(game + DIF_BOARD_REGION_SITE, kBoardRegionPin, 6);
    memcpy(game + DIF_BOARD_PLANET_SITE, kBoardPlanetPin, 7); memcpy(game + DIF_BOARD_DEFENCE_SITE, kBoardPlanetPin, 7);
    game[DIF_MISSIONS + 130 * DIF_MISSION_ROW + 0x34] = 6; /* row 130 (allows up to 100): a city Blitz */
    {
        uint32_t sets[8][6] = {{0}, {124, 145, 33, 128}, {124, 145, 33}, {124, 145, 33, 128}, {93, 33, 118, 92}, {124, 145, 33, 128},
            {124, 145, 33, 128}, {124, 145, 33, 123, 128}};
        memcpy(game + DIF_GRANTS, sets, sizeof sets);
    }
    for (int i = 0; i < 3; i++) memcpy(game + kHudListSites[i].site, kHudListSites[i].pin, kHudListSites[i].length);
    memcpy(game + DIF_BADGE_SITE, kBadgePin, 7);
    memcpy(game + DIF_NAV_SITE, kNavPin, 15); memcpy(game + DIF_NAV_SHIP_SITE, kNavShipPin, 10);
    *(uint64_t*)(game + DIF_MISSIONS + 3 * DIF_MISSION_ROW + 0x360) = DIF_ERADICATE_DELTA; /* row 3: Eradicate */
    game[DIF_MISSIONS + 5 * DIF_MISSION_ROW + 0x34] = 6; /* row 5: Blitz */
    for (int i = 0; i < 4; i++) memcpy(game + kRestartSites[i], kRestartPin, 6);
    for (size_t i = 0; i < sizeof kBarSites / sizeof kBarSites[0]; i++) memcpy(game + kBarSites[i].rva, kBarSites[i].original, kBarSites[i].length);
    {
        int32_t rel = (int32_t)(DIF_SET_SPRITE - (DIF_ICON_CALL + 5));
        game[DIF_ICON_CALL] = 0xE8; memcpy(game + DIF_ICON_CALL + 1, &rel, 4);
    }
    for (uint32_t i = 0; i < DIF_OPERATION_ROWS; i++)
    {
        uint32_t range[2] = {1, i == 7 ? 1u : 10u};
        memcpy(game + DIF_OPERATIONS + i * DIF_OPERATION_ROW + 0xC, range, 8);
    }
    for (uint32_t i = 0; i < DIF_MISSION_ROWS; i++)
    {
        uint32_t range[2] = {i % 4 + 1, i < 120 ? 10u : 100u};
        memcpy(game + DIF_MISSIONS + i * DIF_MISSION_ROW + 0xC, range, 8);
    }
    for (uint64_t i = 0; i < DIF_VANILLA; i++) *(uint64_t*)(game + DIF_BANDS + i * 8) = 0xBA000 + i / 3;
    /* Objectives (148 up to 10), mission distance factors and their readers, rosters and compositions. */
    for (uint32_t i = 0; i < DIF_OBJECTIVE_ROWS; i++) *(uint32_t*)(game + DIF_OBJECTIVES + i * DIF_OBJECTIVE_ROW + 0x28) = i < 148 ? 10 : 0;
    for (uint32_t i = 0; i < DIF_VANILLA * 3; i++) *(float*)(game + DIF_DISTANCE + 4 * i) = 0.6f + 0.01f * (float)i;
    for (size_t i = 0; i < 7; i++) *(int32_t*)(game + kDistanceSites[i][0] + kDistanceSites[i][1]) = (int32_t)DIF_DISTANCE;
    for (size_t i = 0; i < 6; i++) game[kWeightReaders[i][0] + kWeightReaders[i][1]] = 0x30;
    memcpy(game + DIF_TECH_CLAMP, kKeepTen, 5);
    uint8_t* rosterRows = game + 0x3400000, *compositions = game + 0x3500000;
    for (size_t r = 0, at = 0; r < DIF_ROSTERS; r++)
    {
        uint64_t rows = (uint64_t)(uintptr_t)(rosterRows + at * 0x80), count = kRosters[r].rows;
        memcpy(game + kRosters[r].descriptor, &rows, 8); memcpy(game + kRosters[r].descriptor + 8, &count, 8);
        for (uint32_t i = 0; i < count; i++)
            for (uint32_t t = 0; t < DIF_VANILLA; t++) *(float*)(rosterRows + (at + i) * 0x80 + 0x30 + 4 * t) = (float)(100 * r + i + t);
        at += count;
    }
    {
        uint64_t family = (uint64_t)(uintptr_t)compositions, families = DIF_COMPOSITIONS_UP_TO_TEN + 2;
        memcpy(game + kRosters[0].descriptor + 0x20, &family, 8); memcpy(game + kRosters[0].descriptor + 0x28, &families, 8);
        for (uint32_t i = 0; i < families; i++) *(uint32_t*)(compositions + i * 0x118 + 0xF0) = i < DIF_COMPOSITIONS_UP_TO_TEN ? 10 : 5;
    }
    HfDifficulty hell = {sizeof hell};
    hell.name = "fixture/hell_forge"; hell.title = "Hell Forge"; hell.encounter_points = 1500.0f; hell.icon = "fixture/icon"; hell.map_scale = 2.0f; hell.optional_objectives = 8; hell.major_objectives = 2; hell.outpost_scale = 2.0f; hell.mission_minutes = 72.0f; hell.flags = HF_DIFFICULTY_SECOND_PRIMARY | HF_DIFFICULTY_NO_ERADICATE | HF_DIFFICULTY_NO_BLITZ | HF_DIFFICULTY_CITY_STRONGHOLDS; hell.mission_stratagem = 105; hell.patrol_frequency = 2.0f; hell.patrol_cap = 1.5f;
    assert(ApiAddDifficulty(&g_mods[0], &hell) == 11 && ApiAddDifficulty(&g_mods[0], &hell) == 0);
    /* An unfilled copy waits, even after the grace period; a changed instruction waits, then is refused. */
    uint32_t item = *(uint32_t*)(game + kDifficultyCopies[7]);
    *(uint32_t*)(game + kDifficultyCopies[7]) = 0;
    assert(InstallDifficulty(base, 1) == 0 && g_difficultyState == 0);
    *(uint32_t*)(game + kDifficultyCopies[7]) = item;
    game[kDifficultySites[3].rva + kDifficultySites[3].dispAt] ^= 1;
    assert(InstallDifficulty(base, 0) == 0 && g_difficultyState == 0);
    game[kDifficultySites[3].rva + kDifficultySites[3].dispAt] ^= 1;
    assert(InstallDifficulty(base, 1) == 1 && g_difficultyState == 1);
    /* Every instruction addresses the moved table; level 11 is level 10 with the mod's values; rows 12..15 are empty. */
    uint8_t* table = g_difficultyTable;
    for (size_t i = 0; i < sizeof kDifficultySites / sizeof kDifficultySites[0]; i++)
    {
        const DifficultySite* t = &kDifficultySites[i];
        int32_t disp; memcpy(&disp, game + t->rva + t->dispAt, 4);
        uintptr_t target = t->rip ? base + t->rva + t->size + disp : base + (uint32_t)disp;
        assert(target == (uintptr_t)table + t->field);
    }
    uint8_t* row = table + DIF_VANILLA * DIF_ROW;
    assert(*(uint32_t*)row == g_difficulty.item && *(uint32_t*)(row + 4) == 11 && *(float*)(row + 0xE4) == 1500.0f &&
        *(uint32_t*)(row + 0xEC) == 11 && *(float*)(row + 0x104) == 1.2f && *(uint32_t*)(row + 8) == g_difficulty.text[0]);
    assert(*(float*)(row + 0x2A0) == 32.5f && *(float*)(row + 0x2A4) == 39.0f && *(float*)(row + 0x2A8) == 37.5f &&
        *(uint32_t*)(row + 0x2AC) == 62 && *(uint32_t*)(row + 0x2B0) == 59 && *(uint32_t*)(row + 0x2B4) == 72);
    for (uint32_t i = DIF_VANILLA + 1; i < DIF_ROWS; i++) assert(*(uint32_t*)(table + i * DIF_ROW) == 0);
    assert(memcmp(table, game + kDifficultyCopies[0], DIF_VANILLA * DIF_ROW) == 0);
    for (size_t i = 0; i < sizeof kDifficultyBounds / sizeof kDifficultyBounds[0]; i++) assert(game[kDifficultyBounds[i].rva + kDifficultyBounds[i].immAt] == 11);
    for (size_t i = 0; i < sizeof kDifficultyKeepTen / sizeof kDifficultyKeepTen[0]; i++) assert(!memcmp(game + kDifficultyKeepTen[i], kKeepTen, 5));
    assert(ApiDifficultyRow(11) == (uintptr_t)row && ApiDifficultyRow(12) == 0);
    assert(*(uint64_t*)(row + 0x10) == 0x5A000 + 10); /* other places draw the level's icon as an atlas sprite */
    /* Objectives up to 10 allow 11; the distance factors moved (level 11 takes level 10's); every roster row has its
     * weights 4 bytes earlier and level 11's (level 10's) after them, read through the moved displacements; compositions
     * up to tech 10 allow 11; the tech clamp is 11; level 11's row has tech 11 and the mod's missions and map. */
    assert(*(uint32_t*)(game + DIF_OBJECTIVES + 0x28) == 11 && *(uint32_t*)(game + DIF_OBJECTIVES + 150 * DIF_OBJECTIVE_ROW + 0x28) == 0);
    {
        int32_t disp = *(int32_t*)(game + kDistanceSites[3][0] + kDistanceSites[3][1]);
        float* distance = (float*)((uint8_t*)base + (uint32_t)disp);
        assert((uint8_t*)distance > table && distance[0] == 0.6f && distance[30] == 2.0f * distance[27] && distance[32] == 2.0f * distance[29]); /* level 11: 2x */
        for (size_t i = 0; i < 7; i++) assert(*(int32_t*)(game + kDistanceSites[i][0] + kDistanceSites[i][1]) == disp);
    }
    for (size_t i = 0; i < 6; i++) assert(game[kWeightReaders[i][0] + kWeightReaders[i][1]] == 0x2C);
    assert(*(float*)(rosterRows + 0x2C) == 0.0f && *(float*)(rosterRows + 0x2C + 4 * 9) == 9.0f && *(float*)(rosterRows + 0x54) == 9.0f);
    assert(*(float*)(rosterRows + 50 * 0x80 + 0x2C) == 105.0f && *(float*)(rosterRows + 50 * 0x80 + 0x54) == 114.0f);
    assert(*(float*)(rosterRows + 191 * 0x80 + 0x2C) == 500.0f && *(float*)(rosterRows + 191 * 0x80 + 0x54) == 509.0f); /* the sixth roster's row */
    assert(*(uint32_t*)(compositions + 0xF0) == 11 && *(uint32_t*)(compositions + (DIF_COMPOSITIONS_UP_TO_TEN + 1) * 0x118 + 0xF0) == 5);
    assert(*(uint32_t*)(game + DIF_TECH_CLAMP + 1) == 11 && row[0xEC] == 11);
    assert(*(uint32_t*)(row + 0x28) == 8 && *(uint32_t*)(row + 0x2C) == 2 && *(float*)(row + 0xF0) == 2.0f && *(float*)(row + 0xF8) == 1.7f);
    /* A map twice level 10's radius: the map type site writes the level's radii (border 952.24 m: disk 1022 m, fade
     * 1009.24 m, site rect 1..2047) at difficulty 11 except structured maps; both spacing reads take 65.75 m (in quarter metres) for a level
     * with that disk. */
    {
        int32_t rel; memcpy(&rel, game + DIF_MAP_TYPE_SITE + 1, 4);
        uint8_t* radii = game + DIF_MAP_TYPE_SITE + 5 + rel;
        assert(game[DIF_MAP_TYPE_SITE] == 0xE9 && game[DIF_MAP_TYPE_SITE + 5] == 0x90 && game[DIF_MAP_TYPE_SITE + 6] == 0x90);
        assert(!memcmp(radii + 7, kRadiiStub + 7, 30) && radii[21] == 11);
        memcpy(&rel, radii + 3, 4); assert(radii + 7 + rel == (uint8_t*)g_restartWatch + 0x20);
        float r[5]; for (int i = 0; i < 5; i++) memcpy(&r[i], radii + 37 + 11 * i, 4);
        assert(r[1] == 1022.0f / 1024.0f && fabsf(r[0] * 1024.0f - 1009.24f) < 0.01f && r[2] == 1.0f && r[3] == 2047.0f && r[4] == 1023.0f);
        memcpy(&rel, radii + 112, 4); assert(radii[111] == 0xE9);
        {
            uint8_t* strongholds = radii + 116 + rel; /* the city strongholds' call, then back */
            assert(!memcmp(strongholds, kStrongholdStub, 60) && strongholds[125] == 0xE9);
            uint64_t install; memcpy(&install, strongholds + 60, 8); assert(install == (uint64_t)(uintptr_t)(void*)StrongholdInstall);
            memcpy(&rel, strongholds + 126, 4); assert(strongholds + 130 + rel == game + DIF_MAP_TYPE_SITE + 7);
        }
        assert(!memcmp(radii + 85, kRadiiStub + 85, 26)); /* the preview's view rect: the whole world */
        for (int i = 0; i < 7; i++) assert(*(uint32_t*)(game + kCityCapacities[i].rva + kCityCapacities[i].at) == kCityCapacities[i].value);
        memcpy(&rel, game + DIF_CONVOY_SITE + 1, 4);
        uint8_t* convoy = game + DIF_CONVOY_SITE + 5 + rel;
        assert(game[DIF_CONVOY_SITE] == 0xE9 && game[DIF_CONVOY_SITE + 11] == 0x90 && !memcmp(convoy, kConvoyStub, 22));
        memcpy(&rel, convoy + 23, 4); assert(convoy + 27 + rel == game + DIF_CONVOY_BACK);
        const uint32_t sites[2] = {DIF_SPACING_SITE, DIF_SPACING_SITE_2};
        const uint8_t* originals[2] = {kSpacingStub, kSpacingStub2};
        const uint32_t diskAt[2] = {15, 16}, spacingAt[2] = {25, 27}, backAt[2] = {30, 32};
        for (int i = 0; i < 2; i++)
        {
            memcpy(&rel, game + sites[i] + 1, 4);
            uint8_t* stub = game + sites[i] + 5 + rel;
            assert(game[sites[i]] == 0xE9 && !memcmp(stub, originals[i], diskAt[i]) && !memcmp(stub + diskAt[i], &r[1], 4));
            memcpy(&rel, stub + spacingAt[i], 4);
            float spacing = *(float*)(stub + spacingAt[i] + 4 + rel);
            assert(spacing == 65.75f);
            memcpy(&rel, stub + backAt[i], 4); assert(stub[backAt[i] - 1] == 0xE9 && stub + backAt[i] + 4 + rel == game + sites[i] + 5);
        }
        memcpy(&rel, game + DIF_SPACING_SITE_2 + 1, 4);
        uint8_t* stub = game + DIF_SPACING_SITE_2 + 5 + rel;
        assert(!memcmp(stub + 36, kSpacingPin2, 5)); memcpy(&rel, stub + 42, 4); assert(stub + 46 + rel == game + DIF_SPACING_SITE_2 + 5);
        /* GenerateVoronoi's grid: 16 m x 1023 / 681, a little coarser, in quarter metres, for a level with that disk. */
        memcpy(&rel, game + DIF_GRID_SITE + 1, 4);
        uint8_t* grid = game + DIF_GRID_SITE + 5 + rel;
        assert(game[DIF_GRID_SITE] == 0xE9 && game[DIF_GRID_SITE + 8] == 0x90 && !memcmp(grid, kGridStub, 19) && !memcmp(grid + 19, &r[1], 4));
        memcpy(&rel, grid + 29, 4);
        float cell = *(float*)(grid + 33 + rel);
        assert(cell == 24.5f);
        memcpy(&rel, grid + 34, 4); assert(grid[33] == 0xE9 && grid + 38 + rel == game + DIF_GRID_SITE + 9);
        /* The route cells' area points: spacing x 1023 / 681 x 1.04, in quarter metres, for a level with that disk. */
        memcpy(&rel, game + DIF_CELL_SPACING_SITE + 1, 4);
        uint8_t* cells = game + DIF_CELL_SPACING_SITE + 5 + rel;
        assert(game[DIF_CELL_SPACING_SITE] == 0xE9 && game[DIF_CELL_SPACING_SITE + 5] == 0x90 && !memcmp(cells, kCellSpacingStub, 15) && !memcmp(cells + 15, &r[1], 4));
        float factor, quarter; memcpy(&rel, cells + 26, 4); factor = *(float*)(cells + 30 + rel); memcpy(&rel, cells + 40, 4); quarter = *(float*)(cells + 44 + rel);
        assert(fabsf(factor - 1023.0f / 681.0f * 1.04f * 4.0f) < 1e-4f && quarter == 0.25f && !memcmp(cells + 44, kCellSpacingStub + 44, 3));
        memcpy(&rel, cells + 48, 4); assert(cells[47] == 0xE9 && cells + 52 + rel == game + DIF_CELL_SPACING_SITE + 7);
        /* FinalizeVoronoi's points: the same factor, read from the same place. */
        memcpy(&rel, game + DIF_FINALIZE_SPACING_SITE + 1, 4);
        uint8_t* fin = game + DIF_FINALIZE_SPACING_SITE + 5 + rel;
        assert(game[DIF_FINALIZE_SPACING_SITE] == 0xE9 && game[DIF_FINALIZE_SPACING_SITE + 8] == 0x90 && !memcmp(fin, kFinalizeSpacingStub, 24) && !memcmp(fin + 24, &r[1], 4));
        memcpy(&rel, fin + 34, 4); assert(*(float*)(fin + 38 + rel) == factor);
        memcpy(&rel, fin + 48, 4); assert(*(float*)(fin + 52 + rel) == 0.25f);
        memcpy(&rel, fin + 53, 4); assert(fin[52] == 0xE9 && fin + 57 + rel == game + DIF_FINALIZE_SPACING_SITE + 9);
        /* The height zones' table moved to the arena: the clear allocates it, the loop loads its pointer, the height read
         * goes through it; the loop start stays vanilla. */
        {
            assert(!memcmp(game + DIF_ZONE_SITE, kZonePin, 14) && game[DIF_ZONE_TABLE_LEA + 1] == 0x8B);
            memcpy(&rel, game + DIF_ZONE_CLEAR + 1, 4);
            uint8_t* clear = game + DIF_ZONE_CLEAR + 5 + rel;
            assert(game[DIF_ZONE_CLEAR] == 0xE9 && game[DIF_ZONE_CLEAR + 20] == 0x90 && !memcmp(clear, kZoneClearStub, 24));
            memcpy(&rel, clear + 25, 4); assert(clear + 29 + rel == game + DIF_ARENA_ALLOCATE);
            memcpy(&rel, clear + 53, 4); assert(clear + 57 + rel == game + DIF_MEMSET);
            memcpy(&rel, clear + 58, 4); uint8_t* note = clear + 62 + rel;
            assert(note == clear + 0x80 && !memcmp(note + 10, kZoneNoteStub + 10, 38));
            memcpy(&rel, note + 49, 4); assert(note + 53 + rel == game + DIF_ZONE_CLEAR_BACK);
            memcpy(&rel, game + DIF_ZONE_HEIGHT + 1, 4);
            uint8_t* cellNote = game + DIF_ZONE_HEIGHT + 5 + rel;
            assert(game[DIF_ZONE_HEIGHT] == 0xE9 && !memcmp(cellNote + 10, kZoneCellStub + 10, 8));
            memcpy(&rel, cellNote + 19, 4);
            uint8_t* height = cellNote + 23 + rel;
            assert(!memcmp(height, kZoneHeightStub, 11));
            memcpy(&rel, height + 12, 4); assert(height + 16 + rel == game + DIF_ZONE_HEIGHT_BACK);
        }
        /* The minimap capture notes its radii, then runs its first instructions and goes on. */
        {
            memcpy(&rel, game + DIF_CAPTURE_SITE + 1, 4);
            uint8_t* capture = game + DIF_CAPTURE_SITE + 5 + rel;
            assert(game[DIF_CAPTURE_SITE] == 0xE9 && game[DIF_CAPTURE_SITE + 10] == 0x90 && !memcmp(capture + 55, kCapturePin, 11));
            memcpy(&rel, capture + 11, 4); assert(capture + 15 + rel == (uint8_t*)g_restartWatch + 0x28);
            memcpy(&rel, capture + 49, 4); assert(capture + 53 + rel == (uint8_t*)g_restartWatch + 0x34);
            memcpy(&rel, capture + 67, 4); assert(capture + 71 + rel == game + DIF_CAPTURE_SITE + 11);
        }
        /* The director's spawners stop at 128: the add goes through the stub, which counts what it leaves out. */
        {
            memcpy(&rel, game + DIF_SPAWNER_SITE + 1, 4);
            uint8_t* spawner = game + DIF_SPAWNER_SITE + 5 + rel;
            assert(game[DIF_SPAWNER_SITE] == 0xE9 && game[DIF_SPAWNER_SITE + 5] == 0x90 && !memcmp(spawner, kSpawnerStub, 14));
            memcpy(&rel, spawner + 14, 4); assert(spawner + 18 + rel == game + DIF_SPAWNER_ADD);
            memcpy(&rel, spawner + 21, 4); assert(spawner + 25 + rel == (uint8_t*)g_restartWatch + 0x3C);
            memcpy(&rel, spawner + 26, 4); assert(spawner + 30 + rel == game + DIF_SPAWNER_DONE);
        }
        /* The map's marker slots: the entity loop and the route marker leave once 16 are taken. */
        {
            const uint32_t sites[2] = {DIF_MARKER_LOOP_SITE, DIF_MARKER_ROUTE_SITE}, leave[2] = {DIF_MARKER_LOOP_EXIT, DIF_MARKER_ROUTE_SKIP};
            const uint8_t* pins[2] = {kMarkerLoopPin, kMarkerRoutePin};
            const uint8_t lengths[2] = {8, 6}, check[5] = {0x83, 0x7C, 0x24, 0x40, 0x10};
            for (int i = 0; i < 2; i++)
            {
                uint8_t n = lengths[i];
                memcpy(&rel, game + sites[i] + 1, 4);
                uint8_t* marker = game + sites[i] + 5 + rel;
                assert(game[sites[i]] == 0xE9 && game[sites[i] + n - 1] == 0x90);
                assert(!memcmp(marker, check, 5) && marker[5] == 0x0F && marker[6] == 0x83 && !memcmp(marker + 11, pins[i], n) && marker[11 + n] == 0xE9);
                memcpy(&rel, marker + 7, 4); assert(marker + 11 + rel == game + leave[i]);
                memcpy(&rel, marker + 12 + n, 4); assert(marker + 16 + n + rel == game + sites[i] + n);
            }
        }
        /* The war table's map view: the first min reads 1.0, the second cap goes through the stub. */
        {
            int32_t disp; memcpy(&disp, game + DIF_MAPVIEW_MIN + 5, 4);
            assert(*(float*)(game + DIF_MAPVIEW_MIN + 9 + disp) == 1.0f && !memcmp(game + DIF_MAPVIEW_MIN, kMapViewMinPin, 5));
            memcpy(&rel, game + DIF_MAPVIEW_CAP + 1, 4);
            uint8_t* view = game + DIF_MAPVIEW_CAP + 5 + rel;
            assert(game[DIF_MAPVIEW_CAP] == 0xE9 && game[DIF_MAPVIEW_CAP + 8] == 0x90 && !memcmp(view + 9, kMapViewStub + 9, 5));
            memcpy(&rel, view + 5, 4); assert(fabsf(*(float*)(view + 9 + rel) - 0.17f / 0.19f) < 1e-6f);
            memcpy(&rel, view + 19, 4); assert(*(float*)(view + 23 + rel) == 0.17f);
            memcpy(&rel, view + 29, 4); assert(view + 33 + rel == game + DIF_MAPVIEW_CAP_NEXT);
        }
        /* The countdown: launch is unchanged until the level's delta is registered in the game's entity delta table;
         * then it appends that delta at level 11. Arrays 1 to 4 move with one row each, the name takes a free hash
         * slot, the game's other deltas are unchanged and a stats edit of a delta's data follows the moved data. */
        {
            const StatsDeltaLayout* d = &kStatsDelta;
            assert(!memcmp(game + DIF_LAUNCH_SITE, kLaunchPin, sizeof kLaunchPin) && !memcmp(game + DIF_TIMER_SITE, kTimerPin, sizeof kTimerPin));
            uint8_t* deltas = calloc(1, d->size), *holder = calloc(1, 16);
            uint32_t head[] = {d->type, 0x444C444C, 1, d->type, d->size - 28, 1, 0};
            memcpy(deltas, head, sizeof head);
            uintptr_t table = (uintptr_t)deltas + d->header;
            for (int i = 0; i < 5; i++)
            {
                uint64_t array[] = {table + d->offsets[i], d->counts[i]};
                memcpy(deltas + d->header + i * 16, array, sizeof array);
            }
            uint8_t* hashes = deltas + d->header + d->offsets[0];
            uint64_t own = DifficultyDeltaHash();
            uint32_t expect = (uint32_t)(own % d->counts[0]);
            *(uint64_t*)hashes = 1; *(uint32_t*)(hashes + 8) = 2;
            for (int i = 0; i < 2; i++) /* the name's own slot and the next are taken: probing goes on to a free one */
            {
                if (*(uint64_t*)(hashes + expect * 16) == 0) *(uint64_t*)(hashes + expect * 16) = 7 + i;
                expect = (expect + 1) % d->counts[0];
            }
            while (*(uint64_t*)(hashes + expect * 16) != 0) expect = (expect + 1) % d->counts[0];
            uint32_t* settings = (uint32_t*)(deltas + d->header + d->offsets[1] + 2 * 8);
            settings[0] = 1; settings[1] = 3;
            uint32_t* component = (uint32_t*)(deltas + d->header + d->offsets[2] + 3 * 12);
            component[0] = 5; component[1] = 4; component[2] = 1;
            uint32_t* member = (uint32_t*)(deltas + d->header + d->offsets[3] + 4 * 12);
            member[0] = 136; member[1] = 4; member[2] = 7;
            float seconds = 45.0f; memcpy(deltas + d->header + d->offsets[4] + 7, &seconds, 4);
            DifficultyDelta(base);
            assert(!memcmp(game + DIF_LAUNCH_SITE, kLaunchPin, sizeof kLaunchPin)); /* no table yet */
            uintptr_t at = (uintptr_t)holder - DIF_DELTA_HOLDER; memcpy(game + DIF_DELTA_TABLE, &at, 8); memcpy(holder, &table, 8);
            DifficultyDelta(base);
            uint64_t arrays[10]; memcpy(arrays, deltas + d->header, sizeof arrays);
            uint32_t data = (d->counts[4] + 3) & ~3u;
            assert(arrays[0] == table + d->offsets[0] && arrays[1] == d->counts[0]);
            assert(arrays[3] == d->counts[1] + 1 && arrays[5] == d->counts[2] + 1 && arrays[7] == d->counts[3] + 1 && arrays[9] == data + 4);
            for (int i = 1; i < 5; i++) assert(arrays[i * 2] == g_deltaArrays[i] && arrays[i * 2] != table + d->offsets[i]);
            uint32_t* row = (uint32_t*)(uintptr_t)(arrays[2] + (uint64_t)d->counts[1] * 8);
            assert(row[0] == 1 && row[1] == d->counts[2]);
            row = (uint32_t*)(uintptr_t)(arrays[4] + (uint64_t)d->counts[2] * 12);
            assert(row[0] == DIF_HIVEMIND && row[1] == d->counts[3] && row[2] == 1);
            row = (uint32_t*)(uintptr_t)(arrays[6] + (uint64_t)d->counts[3] * 12);
            assert(row[0] == 0x74 && row[1] == 4 && row[2] == data && *(float*)(uintptr_t)(arrays[8] + data) == 4320.0f);
            uint8_t* slot = hashes + expect * 16;
            assert(*(uint64_t*)slot == own && *(uint64_t*)(slot + 8) == d->counts[1]);
            assert(!memcmp((void*)(uintptr_t)arrays[2], deltas + d->header + d->offsets[1], (size_t)d->counts[1] * 8) &&
                *(float*)(uintptr_t)(arrays[8] + 7) == 45.0f);
            /* Launch jumps to the stub, which appends the hash at level 11 once registered, then goes back. */
            memcpy(&rel, game + DIF_LAUNCH_SITE + 1, 4);
            uint8_t* launch = game + DIF_LAUNCH_SITE + 5 + rel;
            assert(game[DIF_LAUNCH_SITE] == 0xE9 && game[DIF_LAUNCH_SITE + 11] == 0x90 && !memcmp(launch, kLaunchPin, 7) && !memcmp(launch + 69, kLaunchPin + 7, 5));
            assert(!memcmp(launch + 38, &own, 8) && g_restartWatch[0x40] == 1);
            memcpy(&rel, launch + 18, 4); assert(launch + 23 + rel == g_restartWatch + 0x40);
            memcpy(&rel, launch + 65, 4); assert(launch + 69 + rel == g_restartWatch + 0x44);
            memcpy(&rel, launch + 75, 4); assert(launch + 79 + rel == game + DIF_LAUNCH_BACK);
            /* Polled again: nothing changes. */
            DifficultyDelta(base);
            assert(*(uint64_t*)(deltas + d->header + 16) == g_deltaArrays[1] && *(uint64_t*)(deltas + d->header + 24) == d->counts[1] + 1);
            /* The stats pass still finds the allocation valid and edits the data where the game reads it. */
            int kind = (int)STATS_EXTRA_COUNT - 1;
            assert(StatsAllocationValid((uintptr_t)deltas, kind));
            StatEdit e = Edit(0, 60, d->header + d->offsets[4] + 7, 0);
            strcpy(e.kind, "EntityDelta"); strcpy(e.component, "5:136"); e.record = 2;
            g_statsAllocations[kind] = (uintptr_t)deltas;
            assert(StatsExtraRow(&e) == arrays[8] + 7);
            g_statsAllocations[kind] = 0;
        }
        /* The mission card's duration and the timer widget's total: at level 11 the countdown delta's value. */
        {
            memcpy(&rel, game + DIF_CARD_TIME_SITE + 1, 4);
            uint8_t* card = game + DIF_CARD_TIME_SITE + 5 + rel;
            assert(game[DIF_CARD_TIME_SITE] == 0xE9 && game[DIF_CARD_TIME_SITE + 7] == 0x90 && !memcmp(card, kCardTimeStub, 9) && !memcmp(card + 22, kCardTimePin, 4));
            memcpy(&rel, card + 9, 4); assert(card + 13 + rel == g_restartWatch + 0x58);
            memcpy(&rel, card + 26, 4); assert(card + 30 + rel == game + DIF_MINUTE);
            memcpy(&rel, card + 31, 4); assert(card + 35 + rel == game + DIF_CARD_TIME_BACK);
            memcpy(&rel, game + DIF_WIDGET_TIME_SITE + 1, 4);
            uint8_t* widget = game + DIF_WIDGET_TIME_SITE + 5 + rel;
            uint64_t manager; memcpy(&manager, widget + 8, 8);
            assert(game[DIF_WIDGET_TIME_SITE] == 0xE9 && game[DIF_WIDGET_TIME_SITE + 12] == 0x90 && !memcmp(widget, kWidgetTimePin, 6) && manager == base + DIF_HIVEMIND_MANAGER);
            memcpy(&manager, widget + 59, 8); assert(manager == base + DIF_HIVEMIND_MANAGER);
            memcpy(&rel, widget + 43, 4); assert(widget + 47 + rel == g_restartWatch + 0x58);
            memcpy(&rel, widget + 71, 4); assert(widget + 75 + rel == game + DIF_WIDGET_TIME_BACK);
        }
        /* The mission card's map view goes through its stub; the marker scales read the globals, vanilla until set. */
        {
            memcpy(&rel, game + DIF_CARD_VIEW_SITE + 1, 4);
            uint8_t* view = game + DIF_CARD_VIEW_SITE + 5 + rel;
            assert(game[DIF_CARD_VIEW_SITE] == 0xE9 && game[DIF_CARD_VIEW_SITE + 41] == 0x90 && !memcmp(view, kCardViewStub, 8));
            memcpy(&rel, view + 176, 4); assert(view + 180 + rel == game + DIF_CARD_VIEW_CALL);
            memcpy(&rel, view + 12, 4); assert(*(float*)(view + 16 + rel) == 0.38f);
            memcpy(&rel, view + 55, 4); assert(*(uint32_t*)(view + 59 + rel) == 0x3E8CCCCDu);
            memcpy(&rel, view + 67, 4); assert(*(uint32_t*)(view + 71 + rel) == 0x3F39999Au);
            memcpy(&rel, view + 121, 4); assert(view + 125 + rel == g_restartWatch + 0x48);
            memcpy(&rel, view + 171, 4); assert(view + 175 + rel == g_restartWatch + 0x54);
            for (int i = 0; i < 9; i++)
            {
                int32_t disp; memcpy(&disp, game + kCardScales[i].rva + kCardScales[i].at, 4);
                uint8_t* global = game + kCardScales[i].rva + kCardScales[i].length + disp;
                assert(global == g_restartWatch + 0x48 + 4 * kCardScales[i].global && *(uint32_t*)global == kCardScaleValues[kCardScales[i].global]);
                assert(!memcmp(game + kCardScales[i].rva, kCardScales[i].pin, kCardScales[i].at));
            }
        }
        /* Mission stratagems: the table read jumps to the stub, whose level-11 copy has the type in every set but 0. */
        {
            memcpy(&rel, game + DIF_GRANT_SITE + 1, 4);
            uint8_t* grant = game + DIF_GRANT_SITE + 5 + rel;
            assert(game[DIF_GRANT_SITE] == 0xE9 && !memcmp(grant + 54, kGrantPin, 7));
            uint64_t copy; memcpy(&copy, grant + 41, 8);
            uint32_t (*sets)[6] = (uint32_t (*)[6])(uintptr_t)copy;
            assert(sets[0][0] == 0 && sets[1][4] == 105 && sets[2][3] == 105 && sets[4][4] == 105 && sets[7][5] == 105 && sets[7][4] == 128);
            memcpy(&rel, grant + 50, 4); assert(grant + 54 + rel == game + DIF_GRANT_BACK);
            memcpy(&rel, grant + 62, 4); assert(grant + 66 + rel == game + DIF_GRANT_BACK);
        }
        /* The hologram's mission scale writers go through their stubs; each stub's constants and way back. */
        for (int i = 0; i < 4; i++)
        {
            memcpy(&rel, game + kHoloStubs[i].site + 1, 4);
            uint8_t* stub = game + kHoloStubs[i].site + 5 + rel;
            assert(game[kHoloStubs[i].site] == 0xE9 && game[kHoloStubs[i].site + kHoloStubs[i].length - 1] == 0x90 && !memcmp(stub, kHoloStubs[i].code, 8));
            float expect[4] = {0.19f, 0.45f, 1.0f, 6.6f};
            for (int j = 0; j < 7 && kHoloStubs[i].slots[j].at != 0; j++)
            {
                memcpy(&rel, stub + kHoloStubs[i].slots[j].at, 4);
                uint8_t* target = stub + kHoloStubs[i].slots[j].at + 4 + rel;
                if (kHoloStubs[i].slots[j].target == 4) assert(target == game + kHoloStubs[i].site + kHoloStubs[i].length);
                else assert(*(float*)target == expect[kHoloStubs[i].slots[j].target]);
            }
        }
        /* Per-level readers: the 15 loads go through stubs (the load, a clamp to 10, back); the in-place clamps. */
        for (int i = 0; i < DIF_LEVEL_LOADS; i++)
        {
            memcpy(&rel, game + kLevelLoads[i].rva + 1, 4);
            uint8_t* stub = game + kLevelLoads[i].rva + 5 + rel;
            int n = kLevelLoads[i].length, r = kLevelLoads[i].reg >= 8;
            assert(game[kLevelLoads[i].rva] == 0xE9 && !memcmp(stub, kLevelLoads[i].pin, n));
            assert(stub[n + r] == 0x83 && (stub[n + r + 1] & 7) == (kLevelLoads[i].reg & 7) && stub[n + r + 2] == 10 && stub[n + r + 3] == 0x76);
            int m = n + 10 + 2 * r;
            memcpy(&rel, stub + m + 1, 4); assert(stub[m] == 0xE9 && stub + m + 5 + rel == game + kLevelLoads[i].rva + kLevelLoads[i].length);
        }
        for (int i = 0; i < 5; i++) assert(!memcmp(game + kLevelInPlace[i].rva, kLevelInPlace[i].bytes, kLevelInPlace[i].length));
        assert(game[DIF_HUD_TARGET_SIB] == 0x85);
        /* The HUD's and the other icon readers' calls check the row's level, then the icon stub's body (the HUD's copy
         * passes its own base sprite). */
        {
            uint32_t calls[4] = {DIF_HUD_ICON_CALL, kIconSites[0].call, kIconSites[1].call, kIconSites[2].call};
            for (int i = 0; i < 4; i++)
            {
                memcpy(&rel, game + calls[i] + 1, 4);
                uint8_t* check = game + calls[i] + 5 + rel;
                int n = i == 0 ? 4 : 5;
                assert(game[calls[i]] == 0xE8 && check[n] == 0x0F && check[n + 1] == 0x85 && check[n + 6] == 0xE9);
                assert(i == 0 ? !memcmp(check, "\x83\x7F\x04\x0B", 4) : !memcmp(check, kIconSites[i - 1].check, 5));
                memcpy(&rel, check + n + 2, 4); assert(check + n + 6 + rel == game + (i == 0 ? DIF_SET_IMAGE : DIF_SET_SPRITE));
                memcpy(&rel, check + n + 7, 4);
                uint8_t* body = check + n + 11 + rel;
                uint64_t sprite, texture; memcpy(&sprite, body + 21, 8); memcpy(&texture, body + 11, 8);
                assert(texture == g_difficulty.icon && !memcmp(body, kIconStub + 9, 11));
                assert(i == 0 ? !memcmp(&sprite, kHudIconPin + 13, 8) : sprite == 0x5A000 + DIF_VANILLA);
                memcpy(&rel, body + 33, 4); assert(body + 37 + rel == game + DIF_SET_SPRITE);
                memcpy(&rel, body + 118, 4); assert(body + 122 + rel == game + DIF_REFRESH_IMAGE);
            }
        }
        /* The SelectedMission telemetry lists at most 8 objectives. */
        {
            memcpy(&rel, game + DIF_TELEMETRY_SITE + 1, 4);
            uint8_t* telemetry = game + DIF_TELEMETRY_SITE + 5 + rel;
            assert(game[DIF_TELEMETRY_SITE] == 0xE9 && !memcmp(telemetry, kTelemetryStub, 16));
            memcpy(&rel, telemetry + 16, 4); assert(telemetry + 20 + rel == game + DIF_TELEMETRY_SITE + 5);
        }
        /* The stamp index has 16383 slots: size, layout check and index agree. */
        for (int i = 0; i < 5; i++) assert(!memcmp(game + kStampIndex[i].rva, kStampIndex[i].bytes, kStampIndex[i].length));
        assert(*(uint32_t*)(game + 0xC0498B + 1) == 16384u * 4u + 7u && *(uint32_t*)(game + 0xC04C1A + 2) == 16383u &&
            *(uint32_t*)(game + 0xC04CB2 + 3) == 16384u * 4u && *(uint32_t*)(game + 0xC04CCC + 3) == 16384u);
        /* The locations moved to the end of a larger block, with 8192 places each. */
        for (size_t i = 0; i < sizeof kLocationRefs / sizeof kLocationRefs[0]; i++)
        {
            uint32_t v = *(uint32_t*)(game + kLocationRefs[i].rva + kLocationRefs[i].at), old = kLocationRefs[i].value;
            assert(old >= DIF_LOCATIONS_B ? v == DIF_LOCATIONS_B_NEW + (old - DIF_LOCATIONS_B) : v == DIF_LOCATIONS_A_NEW + (old - DIF_LOCATIONS_A));
        }
        for (size_t i = 0; i < sizeof kLocationCaps / sizeof kLocationCaps[0]; i++) assert(*(uint32_t*)(game + kLocationCaps[i].rva + kLocationCaps[i].at) == 8192u);
        for (size_t i = 0; i < sizeof kGenerationBlockSize / sizeof kGenerationBlockSize[0]; i++)
            assert(*(uint32_t*)(game + kGenerationBlockSize[i].rva + kGenerationBlockSize[i].at) == 0x15B1100u);
        for (size_t i = 0; i < sizeof kPlaceListRefs / sizeof kPlaceListRefs[0]; i++) assert(*(uint32_t*)(game + kPlaceListRefs[i].rva) == 0x15AD500u);
        for (size_t i = 0; i < sizeof kPlaceSetRefs / sizeof kPlaceSetRefs[0]; i++) assert(*(uint32_t*)(game + kPlaceSetRefs[i].rva) == 0x15AE500u);
        for (int i = 0; i < 16; i++)
            assert(*(uint32_t*)(game + kClusterRefs[i].rva + 4) == 0x15AF500u && *(uint32_t*)(game + kClusterCaps[i].rva + kClusterCaps[i].at) == 32);
        for (int i = 0; i < 2; i++)
        {
            int32_t to; memcpy(&to, game + kClusterCountSites[i].site + 1, 4);
            uint8_t* stub = game + kClusterCountSites[i].site + 5 + to;
            assert(game[kClusterCountSites[i].site] == 0xE9 && !memcmp(stub, kClusterCountSites[i].stub, kClusterCountSites[i].backAt - 1));
            memcpy(&to, stub + kClusterCountSites[i].backAt, 4); assert(stub + kClusterCountSites[i].backAt + 4 + to == game + kClusterCountSites[i].back);
        }
        for (size_t i = 0; i < sizeof kStampRefs / sizeof kStampRefs[0]; i++)
            assert(*(uint32_t*)(game + kStampRefs[i].rva + kStampRefs[i].at) == 0x15A9280u);
        for (size_t i = 0; i < sizeof kMinimapRefs / sizeof kMinimapRefs[0]; i++)
            assert(*(uint32_t*)(game + kMinimapRefs[i].rva + kMinimapRefs[i].at) == MinimapMoved(kMinimapRefs[i].value));
        for (size_t i = 0; i < sizeof kMinimapCaps / sizeof kMinimapCaps[0]; i++)
            assert(*(uint32_t*)(game + kMinimapCaps[i].rva + kMinimapCaps[i].at) == kMinimapCaps[i].value * 4u);
        assert(DIF_MINIMAP_P_NEW == 0x13E9280u && DIF_MINIMAP_S_NEW == 0x14A9280u && DIF_MINIMAP_U_NEW == 0x1559280u && DIF_MINIMAP_V_NEW == 0x1569280u);
        for (size_t i = 0; i < sizeof kLocationTypeRefs / sizeof kLocationTypeRefs[0]; i++)
            assert(*(uint32_t*)(game + kLocationTypeRefs[i].rva + kLocationTypeRefs[i].at) == 0x13E7280u);
        for (size_t i = 0; i < sizeof kLocationTypeCaps / sizeof kLocationTypeCaps[0]; i++)
            assert(*(uint32_t*)(game + kLocationTypeCaps[i].rva + kLocationTypeCaps[i].at) == 512u);
        assert(DIF_LOCATIONS_B_NEW == 0x1367280u && DIF_LOCATION_TYPES_C_NEW == 0x13E7280u && DIF_STAMPS_NEW == 0x15A9280u && DIF_OBJLIST_NEW == 0x15AD370u && DIF_PLACES_NEW == 0x15AD500u && DIF_PLACE_SET_NEW == 0x15AE500u && DIF_CLUSTERS_NEW == 0x15AF500u &&
            DIF_BLOCK_NEW == 0x15B1100u);
        /* The objective list moved (every field displacement); the telemetry and selection go through their stubs. */
        for (int i = 0; i < 74; i++)
        {
            uint32_t v = 0; memcpy(&v, game + kObjectiveListSites[i].rva + kObjectiveListSites[i].at, kObjectiveListSites[i].width);
            assert(v == kObjectiveListSites[i].value);
        }
        assert(g_objectivePlaces == 30);
        {
            uint32_t sites[3] = {DIF_TELEMETRY_COUNT_SITE, DIF_TELEMETRY_LOOP_SITE, DIF_SELECTION_END};
            uint32_t backs[3] = {DIF_TELEMETRY_COUNT_SITE + 10, DIF_TELEMETRY_DONE, DIF_SELECTION_BACK};
            uint8_t ends[3] = {25, 25, 78};
            for (int i = 0; i < 3; i++)
            {
                memcpy(&rel, game + sites[i] + 1, 4);
                uint8_t* stub = game + sites[i] + 5 + rel;
                assert(game[sites[i]] == 0xE9);
                memcpy(&rel, stub + ends[i] - 4, 4); assert(stub + ends[i] + rel == game + backs[i]);
            }
            memcpy(&rel, game + DIF_TELEMETRY_LOOP_SITE + 1, 4);
            uint8_t* loop = game + DIF_TELEMETRY_LOOP_SITE + 5 + rel;
            memcpy(&rel, loop + 16, 4); assert(loop + 20 + rel == game + DIF_TELEMETRY_LOOP);
            memcpy(&rel, game + DIF_SELECTION_END + 1, 4);
            uint8_t* sel = game + DIF_SELECTION_END + 5 + rel;
            void* function; memcpy(&function, sel + 52, 8); assert(function == (void*)MixMission);
            /* The HUD list's accesses: lea r11, [rip+list]; the access; back; all three share the list. */
            uint8_t* lists[3];
            for (int i = 0; i < 3; i++)
            {
                memcpy(&rel, game + kHudListSites[i].site + 1, 4);
                uint8_t* stub = game + kHudListSites[i].site + 5 + rel;
                assert(game[kHudListSites[i].site] == 0xE9 && game[kHudListSites[i].site + kHudListSites[i].length - 1] == 0x90);
                assert(stub[0] == 0x4C && stub[1] == 0x8D && stub[2] == 0x1D && !memcmp(stub + 7, kHudListSites[i].access, 4) && stub[11] == 0xE9);
                memcpy(&rel, stub + 3, 4); lists[i] = stub + 7 + rel;
                memcpy(&rel, stub + 12, 4); assert(stub + 16 + rel == game + kHudListSites[i].site + kHudListSites[i].length);
            }
            assert(lists[0] == lists[1] && lists[1] == lists[2]);
            memcpy(&rel, game + DIF_BADGE_SITE + 1, 4);
            uint8_t* badge = game + DIF_BADGE_SITE + 5 + rel;
            assert(game[DIF_BADGE_SITE] == 0xE9 && !memcmp(badge, kBadgeStub, 12));
            memcpy(&rel, badge + 12, 4); assert(badge + 16 + rel == game + DIF_BADGE_SKIP);
            memcpy(&rel, badge + 17, 4); assert(badge + 21 + rel == game + DIF_BADGE_NEXT);
        }
        /* Mixing: base row 20 (main 0xA1, optional 0xA9) and partner row 21 (main 0xB1, step 0xB2), both Automaton,
         * listed by operation 0. The partner's step is registered in the base as class 1, weight 0; the nested selection
         * (a fake here, which runs the end hook like the stub) gives the partner's chain, appended with its classes. */
        {
            uint8_t* base20 = game + DIF_MISSIONS + 20 * DIF_MISSION_ROW, *partner21 = game + DIF_MISSIONS + 21 * DIF_MISSION_ROW;
            static const uint8_t layoutOne = 1;
            uint64_t layouts = (uint64_t)(uintptr_t)&layoutOne;
            uint32_t one = 1, three = 3, idBase = 0xC0FFEE20u, idPartner = 0xC0FFEE21u;
            float w = 1.0f;
            uint8_t* rows[2] = {base20, partner21};
            uint32_t rowIds[2] = {idBase, idPartner};
            for (int r = 0; r < 2; r++)
            {
                memcpy(rows[r], &rowIds[r], 4); rows[r][4] = 1; rows[r][8] = 3; rows[r][0x34] = 1;
                memcpy(rows[r] + 0x368, &layouts, 8); memcpy(rows[r] + 0x370, &one, 4);
            }
            uint32_t entries[4][2] = {{0xA1, 0}, {0xA9, 3}, {0xB1, 0}, {0xB2, 1}};
            for (int e = 0; e < 4; e++)
            {
                uint8_t* at = (e < 2 ? base20 : partner21) + 0x40 + (e % 2) * 0x18;
                memcpy(at, &entries[e][0], 4); memcpy(at + 4, &w, 4); memcpy(at + 0xC, &entries[e][1], 4);
            }
            uint32_t defs[4] = {0xA1, 0xA9, 0xB1, 0xB2}, levels[2] = {1, 11}, most = 3;
            for (int d = 0; d < 4; d++)
            {
                uint8_t* def = game + DIF_OBJECTIVES + (140 + d) * DIF_OBJECTIVE_ROW;
                memcpy(def, &defs[d], 4); memcpy(def + 0x20, &most, 4); memcpy(def + 0x24, levels, 8);
            }
            memcpy(game + DIF_OPERATIONS + 0x38, &idBase, 4); memcpy(game + DIF_OPERATIONS + 0x38 + 8, &idPartner, 4);
            (void)three;
            /* An Eradicate partner is left out under HF_DIFFICULTY_NO_ERADICATE (no kill main objective at 11). */
            uint64_t eradicateDelta = DIF_ERADICATE_DELTA, noDelta = 0;
            assert(g_difficulty.flags & HF_DIFFICULTY_NO_ERADICATE);
            memcpy(partner21 + 0x360, &eradicateDelta, 8);
            MixInstall(base);
            assert(g_mixPartnerCount[20] == 0 && !g_mixBase[20]);
            memcpy(partner21 + 0x360, &noDelta, 8);
            MixInstall(base);
            g_select = FakeSelect;
            assert(g_mixBase[20] && g_mixPartnerCount[20] == 1 && g_mixPartners[20][0] == 21 && g_mixBase[21] && g_mixPartners[21][0] == 20);
            uint32_t registered = 0, cls = 9; float weight = 1.0f;
            for (int j = 0; j < 32; j++)
            {
                uint32_t id; memcpy(&id, base20 + 0x40 + j * 0x18, 4);
                if (id == 0xB2) { registered++; memcpy(&weight, base20 + 0x40 + j * 0x18 + 4, 4); memcpy(&cls, base20 + 0x40 + j * 0x18 + 0xC, 4); }
            }
            assert(registered == 1 && weight == 0.0f && cls == 1 && RowClass((uintptr_t)base20, 0xB2) == 1);
            uint8_t in[0x20] = {0};
            uint32_t seed = 777, row = 20, faction = 3, level = 11;
            memcpy(in, &seed, 4); memcpy(in + 8, &row, 4); memcpy(in + 0xC, &faction, 4); memcpy(in + 0x10, &level, 4);
            uint32_t ids[32] = {0xA1, 0xA9};
            int32_t classes[32] = {0, 3};
            uint32_t logged = g_mixLoggedCount;
            uint32_t n = MixMission(in, 2, ids, classes, 1);
            assert(n == 4 && ids[2] == 0xB1 && classes[2] == 0 && ids[3] == 0xB2 && classes[3] == 1 && in[0x14] == 0);
            assert(g_mixLoggedCount == logged + 1); /* the mix is logged */
            uint32_t again[32] = {0xA1, 0xA9};
            assert(MixMission(in, 2, again, NULL, 1) == 4 && again[2] == 0xB1 && again[3] == 0xB2);
            assert(g_mixLoggedCount == logged + 1); /* the same mix again is not */
            uint32_t other[32] = {0xA1, 0xA9};
            assert(MixMission(in, 2, other, NULL, 2) == 2); /* the partner is not placed on layout 2 */
            assert(g_mixLoggedCount == logged + 1); /* no mix, no line */
            in[0x14] = DIF_MIX_MARK; /* a nested call only reports its layout */
            assert(MixMission(in, 2, other, NULL, 5) == 2 && in[0x15] == 5 && in[0x16] == 1);
            /* The radar station: row 20 gets it as a weight-0 optional entry; the mission keeps it in place of its last
             * optional pick (0xA9), after the partner's chain. */
            {
                uint32_t radar[3] = {DIF_RADAR, 1, 11}, flags = g_difficulty.flags;
                uint8_t* def = game + DIF_OBJECTIVES + 150 * DIF_OBJECTIVE_ROW;
                static const uint8_t layoutsAny[8] = {1, 2, 3, 4};
                memcpy(def, &radar[0], 4); memcpy(def + 0x24, &radar[1], 8); memcpy(def + 0x98, layoutsAny, 8);
                RadarInstall(base);
                assert(RowClass((uintptr_t)base20, DIF_RADAR) == 3);
                g_difficulty.flags |= HF_DIFFICULTY_RADAR;
                uint8_t again2[0x20] = {0}; memcpy(again2, in, 0x20); again2[0x14] = 0;
                uint32_t withRadar[32] = {0xA1, 0xA9};
                int32_t withClasses[32] = {0, 3};
                assert(MixMission(again2, 2, withRadar, withClasses, 1) == 4 && withRadar[1] == DIF_RADAR && withClasses[1] == 3 && withRadar[2] == 0xB1);
                uint32_t onFive[32] = {0xA1, 0xA9};
                assert(RadarKeep(again2, 2, onFive, NULL, 5) == 2 && onFive[1] == 0xA9); /* layout 5: not placed there */
                g_difficulty.flags = flags;
            }
        }
        /* City ICBM missions: Automaton dev row 50 becomes a city row (from row 36) with ICBM row 0's name, 3 silos and the
         * steps, plus the city row's optionals; operation 10 lists it first with weight 1e30. */
        {
            uint8_t* r50 = game + DIF_MISSIONS + 50 * DIF_MISSION_ROW, *r36 = game + DIF_MISSIONS + 36 * DIF_MISSION_ROW, *r0 = game + DIF_MISSIONS;
            uint8_t saved[3][DIF_MISSION_ROW];
            memcpy(saved[0], r50, DIF_MISSION_ROW); memcpy(saved[1], r36, DIF_MISSION_ROW); memcpy(saved[2], r0, DIF_MISSION_ROW);
            uint32_t a50 = 0x88A2290Bu, a36 = 0x01C642ADu, a0 = 0xC8859069u, faction = 3, name = 0xA954D7AAu;
            memset(r50, 0, DIF_MISSION_ROW); memset(r36, 0, DIF_MISSION_ROW); memset(r0, 0, DIF_MISSION_ROW);
            memcpy(r50, &a50, 4); memcpy(r50 + 8, &faction, 4);
            memcpy(r36, &a36, 4); memcpy(r36 + 8, &faction, 4); r36[4] = 1; r36[0x34] = 1;
            uint32_t optional[6] = {0xD1, 0x3F800000u, 0, 3, 0, 0xFFFFFFFFu}, main36[6] = {0xD2, 0x3F800000u, 0, 0, 1, 1};
            memcpy(r36 + 0x40, main36, 24); memcpy(r36 + 0x58, optional, 24);
            memcpy(r0, &a0, 4); memcpy(r0 + 8, &faction, 4); memcpy(r0 + 0x340, &name, 4);
            uint8_t* op10 = game + DIF_OPS + 10 * DIF_OP;
            uint8_t savedOp[0x140]; memcpy(savedOp, op10, sizeof savedOp);
            memset(op10, 0, sizeof savedOp); op10[8] = 3;
            uint32_t list[2][2] = {{0x28657500u, 0x3F800000u}, {a36, 0x3F800000u}};
            memcpy(op10 + 0x38, list, sizeof list);
            CityIcbmInstall(base);
            uint32_t e[3][6]; memcpy(e, r50 + 0x40, sizeof e);
            assert(r50[4] == 1 && *(uint32_t*)(r50 + 0xC) == 11 && *(uint32_t*)(r50 + 0x10) == 11 && *(uint32_t*)(r50 + 0x340) == name && r50[0x34] == 1);
            assert(e[0][0] == DIF_ICBM && e[0][3] == 0 && e[0][4] == 3 && e[0][5] == 3 && e[1][3] == 1 && *(uint32_t*)(r50 + 0x40 + 4 * 0x18) == 0xD1);
            uint32_t got[3][2]; memcpy(got, op10 + 0x38, sizeof got);
            assert(got[0][0] == a50 && *(float*)&got[0][1] == 1e30f && got[1][0] == 0x28657500u && got[2][0] == a36);
            CityIcbmInstall(base); /* idempotent: listed once */
            uint32_t twice[4][2]; memcpy(twice, op10 + 0x38, sizeof twice);
            assert(twice[1][0] == 0x28657500u && twice[3][0] == 0);
            memcpy(r50, saved[0], DIF_MISSION_ROW); memcpy(r36, saved[1], DIF_MISSION_ROW); memcpy(r0, saved[2], DIF_MISSION_ROW);
            memcpy(op10, savedOp, sizeof savedOp);
        }
        /* Eradicate (row 3) stays at 10; the others reaching 10 go to 11. */
        assert(*(uint32_t*)(game + DIF_MISSIONS + 3 * DIF_MISSION_ROW + 0x10) == 10 && *(uint32_t*)(game + DIF_MISSIONS + 4 * DIF_MISSION_ROW + 0x10) == 11);
        assert(*(uint32_t*)(game + DIF_MISSIONS + 5 * DIF_MISSION_ROW + 0x10) == 10); /* Blitz stays at 10 */
        assert(*(uint32_t*)(game + DIF_MISSIONS + 130 * DIF_MISSION_ROW + 0x10) == 10); /* and a 1..100 Blitz goes to 10 */
        /* The mission board: regions run to 11; the three slot sites go through their stubs (back and skip). */
        assert(*(uint32_t*)(game + DIF_BOARD_REGION_MAX + 1) == 11);
        {
            memcpy(&rel, game + DIF_FOOTPRINT_CHECK + 1, 4);
            uint8_t* footprints = game + DIF_FOOTPRINT_CHECK + 5 + rel;
            assert(game[DIF_FOOTPRINT_CHECK] == 0xE9 && !memcmp(footprints, kFootprintStub, 5) && !memcmp(footprints + 13, kFootprintStub + 13, 8));
            memcpy(&rel, footprints + 22, 4); assert(footprints + 26 + rel == game + DIF_FOOTPRINT_FITS);
            memcpy(&rel, footprints + 40, 4); assert(footprints + 44 + rel == game + DIF_FOOTPRINT_OVER);
        }
        /* Structured maps: the revoronoi sizes go through their stubs (gated on the larger disk, the route cells'
         * factor), the bucket pool holds 576, the cities' flags moved to the arena. */
        for (int i = 0; i < 3; i++)
        {
            memcpy(&rel, game + kReVoronoiStubs[i].site + 1, 4);
            uint8_t* stub = game + kReVoronoiStubs[i].site + 5 + rel;
            assert(game[kReVoronoiStubs[i].site] == 0xE9 && game[kReVoronoiStubs[i].site + kReVoronoiStubs[i].length - 1] == 0x90);
            assert(!memcmp(stub, kReVoronoiStubs[i].stub, kReVoronoiStubs[i].diskAt));
            float diskBits; memcpy(&diskBits, stub + kReVoronoiStubs[i].diskAt, 4); assert(diskBits > 0.99f && diskBits < 1.0f);
            memcpy(&rel, stub + kReVoronoiStubs[i].factorAt, 4); assert(*(float*)(stub + kReVoronoiStubs[i].factorAt + 4 + rel) > 6.0f);
            memcpy(&rel, stub + kReVoronoiStubs[i].quarterAt, 4); assert(*(float*)(stub + kReVoronoiStubs[i].quarterAt + 4 + rel) == 0.25f);
            memcpy(&rel, stub + kReVoronoiStubs[i].backAt, 4);
            assert(stub + kReVoronoiStubs[i].backAt + 4 + rel == game + kReVoronoiStubs[i].site + kReVoronoiStubs[i].length);
        }
        for (int i = 0; i < 8; i++) assert(*(uint32_t*)(game + kBucketPool[i].rva + kBucketPool[i].at) == kBucketPool[i].value);
        {
            memcpy(&rel, game + DIF_CULL_RADIUS + 1, 4);
            uint8_t* cull = game + DIF_CULL_RADIUS + 5 + rel;
            float disk; memcpy(&disk, cull + 6, 4);
            assert(game[DIF_CULL_RADIUS] == 0xE9 && !memcmp(cull, kCullRadiusStub, 6) && disk > 0.99f && disk < 1.0f && !memcmp(cull + 10, kCullRadiusStub + 10, 10));
            memcpy(&rel, cull + 21, 4); assert(cull + 25 + rel == game + DIF_CULL_RADIUS + 8);
            memcpy(&rel, cull + 34, 4); assert(cull + 38 + rel == game + DIF_CULL_RADIUS + 8);
        }
        {
            memcpy(&rel, game + DIF_VISTA_POSE + 1, 4);
            uint8_t* vista = game + DIF_VISTA_POSE + 5 + rel;
            float scale; memcpy(&scale, vista + 80, 4);
            assert(game[DIF_VISTA_POSE] == 0xE9 && !memcmp(vista, kVistaPoseStub, 18) && !memcmp(vista + 22, kVistaPoseStub + 22, 42));
            assert(fabsf(scale - 1009.24f / 620.0f) < 0.001f && *(float*)(vista + 84) == scale && *(float*)(vista + 88) == 1.0f);
            memcpy(&rel, vista + 65, 4); assert(vista + 69 + rel == game + DIF_VISTA_POSE + 7);
        }
        for (int i = 0; i < 4; i++)
        {
            uint8_t load = kClusterAppends[i].load, rest = kClusterAppends[i].store + kClusterAppends[i].inc;
            memcpy(&rel, game + kClusterAppends[i].site + 1, 4);
            uint8_t* stub = game + kClusterAppends[i].site + 5 + rel;
            assert(game[kClusterAppends[i].site] == 0xE9 && !memcmp(stub, kClusterAppends[i].pin, load) && stub[load] == 0x83 && stub[load + 2] == 24 &&
                stub[load + 3] == 0x73 && stub[load + 4] == rest && !memcmp(stub + load + 5, kClusterAppends[i].pin + load, rest));
            memcpy(&rel, stub + load + 5 + rest + 1, 4); assert(stub + load + 5 + rest + 5 + rel == game + kClusterAppends[i].site + load + rest);
        }
        for (int i = 0; i < 4; i++)
        {
            memcpy(&rel, game + kClusterSites[i].site + 1, 4);
            uint8_t* stub = game + kClusterSites[i].site + 5 + rel;
            assert(game[kClusterSites[i].site] == 0xE9 && game[kClusterSites[i].site + kClusterSites[i].length - 1] == 0x90);
            assert(!memcmp(stub, kClusterSites[i].stub, kClusterSites[i].backAt - 1));
            memcpy(&rel, stub + kClusterSites[i].backAt, 4); assert(stub + kClusterSites[i].backAt + 4 + rel == game + kClusterSites[i].back);
            memcpy(&rel, stub + kClusterSites[i].skipAt, 4); assert(stub + kClusterSites[i].skipAt + 4 + rel == game + kClusterSites[i].skip);
        }
        {
            memcpy(&rel, game + DIF_FLAGS_CLEAR + 1, 4);
            uint8_t* clear = game + DIF_FLAGS_CLEAR + 5 + rel;
            assert(game[DIF_FLAGS_CLEAR] == 0xE9 && game[DIF_FLAGS_CLEAR + 31] == 0x90 && !memcmp(clear, kFlagsClearStub, 15));
            memcpy(&rel, clear + 16, 4); assert(clear + 20 + rel == game + DIF_ARENA_ALLOCATE);
            memcpy(&rel, clear + 36, 4); assert(clear + 40 + rel == game + DIF_MEMSET);
            memcpy(&rel, clear + 44, 4); assert(clear + 48 + rel == game + DIF_FLAGS_CLEAR_BACK);
            assert(game[DIF_FLAGS_READ + 1] == 0x8B && *(uint32_t*)(game + kFlagsChecks[0] + 3) == 0x400 && *(uint32_t*)(game + kFlagsChecks[1] + 3) == 0x400);
            for (int i = 0; i < 3; i++)
            {
                memcpy(&rel, game + kFlagsWrites[i].site + 1, 4);
                uint8_t* stub = game + kFlagsWrites[i].site + 5 + rel;
                assert(game[kFlagsWrites[i].site] == 0xE9 && !memcmp(stub, kFlagsWrites[i].stub, kFlagsWrites[i].backAt));
                memcpy(&rel, stub + kFlagsWrites[i].backAt, 4); assert(stub + kFlagsWrites[i].backAt + 4 + rel == game + kFlagsWrites[i].back);
            }
        }
        {
            memcpy(&rel, game + DIF_SPREAD_REFERENCE + 1, 4);
            uint8_t* spread = game + DIF_SPREAD_REFERENCE + 5 + rel;
            assert(game[DIF_SPREAD_REFERENCE] == 0xE9 && !memcmp(spread, kSpreadReferenceStub, 19));
            memcpy(&rel, spread + 20, 4); assert(spread + 24 + rel == game + DIF_SPREAD_REFERENCE_BACK);
        }
        {
            /* the hash insert's end: a jump to the guard, the rest int3; run as the game would reach it (rcx the slot or
             * null, esi the value, r9d the key, the caller's rsi at [rsp+0x10]): a slot gets key and value, a full table
             * (no slot) writes nothing and is counted, rsi comes back either way */
            memcpy(&rel, game + DIF_HASH_INSERT_END + 1, 4);
            uint8_t* guard = game + DIF_HASH_INSERT_END + 5 + rel;
            assert(game[DIF_HASH_INSERT_END] == 0xE9 && !memcmp(guard, kHashInsertEndStub, sizeof kHashInsertEndStub));
            for (int i = 5; i < 12; i++) assert(game[DIF_HASH_INSERT_END + i] == 0xCC);
            assert(g_hashFullCount == (volatile LONG*)(guard + 0x1000) && *g_hashFullCount == 0);
            uint32_t entry[2] = {0xAAAAAAAAu, 0xBBBBBBBBu};
            uint64_t kept = 0;
            for (int pass = 0; pass < 2; pass++)
            {
                uint32_t* slot = pass == 0 ? entry : NULL;
                __asm__ volatile(
                    "push %%rsi\n\t"
                    "sub $0x28, %%rsp\n\t"
                    "movabs $0x5151515151515151, %%rsi\n\t"
                    "mov %%rsi, 0x8(%%rsp)\n\t"        /* the caller's rsi, at [rsp+0x10] once the call pushes */
                    "mov %[slot], %%rcx\n\t"
                    "mov $0x22222222, %%esi\n\t"
                    "mov $0x11111111, %%r9d\n\t"
                    "call *%[guard]\n\t"
                    "mov %%rsi, %%rax\n\t"
                    "add $0x28, %%rsp\n\t"
                    "pop %%rsi\n\t"
                    : "=a"(kept)
                    : [slot] "r"(slot), [guard] "r"(guard)
                    : "rcx", "rsi", "r9", "memory", "cc");
                assert(kept == 0x5151515151515151ull);
            }
            assert(entry[0] == 0x11111111u && entry[1] == 0x22222222u && *g_hashFullCount == 1);
        }
        for (int i = 0; i < 3; i++)
        {
            memcpy(&rel, game + kBoardSites[i].site + 1, 4);
            uint8_t* stub = game + kBoardSites[i].site + 5 + rel;
            assert(game[kBoardSites[i].site] == 0xE9 && !memcmp(stub, kBoardSites[i].stub, 8));
            memcpy(&rel, stub + kBoardSites[i].backAt, 4); assert(stub + kBoardSites[i].backAt + 4 + rel == game + kBoardSites[i].back);
            memcpy(&rel, stub + kBoardSites[i].skipAt, 4); assert(stub + kBoardSites[i].skipAt + 4 + rel == game + kBoardSites[i].skip);
        }
        /* Each generation restart notes its level in the watch (after the table) and counts per site, then goes on. */
        for (int i = 0; i < 4; i++)
        {
            memcpy(&rel, game + kRestartSites[i] + 1, 4);
            uint8_t* note = game + kRestartSites[i] + 5 + rel;
            int inc = i == 0 ? 47 : 14; /* site 0 (FixUnreachableAreas) has the larger map's give-up stub */
            assert(game[kRestartSites[i]] == 0xE9 && game[kRestartSites[i] + 5] == 0x90 && !memcmp(note + inc, kRestartPin, 6));
            memcpy(&rel, note + 3, 4); assert(note + 7 + rel == (uint8_t*)g_restartWatch);
            memcpy(&rel, note + 10, 4); assert(note + 14 + rel == (uint8_t*)g_restartWatch + 8 + 4 * i);
            if (i != 0) { memcpy(&rel, note + 21, 4); assert(note + 25 + rel == game + kRestartSites[i] + 6); }
        }
        /* A larger map's FixUnreachableAreas gives up from the 7th try of a level (FixUnreachableAreas site 0 above
         * checks the common start; here the rest). */
        {
            memcpy(&rel, game + kRestartSites[0] + 1, 4);
            uint8_t* give = game + kRestartSites[0] + 5 + rel;
            assert(!memcmp(give + 14, kUnreachableStub + 14, 6) && !memcmp(give + 20, &r[1], 4) && give[32] == DIF_UNREACHABLE_TRIES);
            memcpy(&rel, give + 38, 4); assert(give + 42 + rel == (uint8_t*)g_restartWatch + 0x18);
            memcpy(&rel, give + 43, 4); assert(give + 47 + rel == game + DIF_UNREACHABLE_GO_ON);
            memcpy(&rel, give + 54, 4); assert(give + 58 + rel == game + kRestartSites[0] + 6);
        }
    }
    /* Operations and missions up to 10 now go up to 11; the rest are unchanged. The title is in capitals. */
    assert(*(uint32_t*)(game + DIF_OPERATIONS + 0x10) == 11 && *(uint32_t*)(game + DIF_OPERATIONS + 7 * DIF_OPERATION_ROW + 0x10) == 1);
    assert(*(uint32_t*)(game + DIF_MISSIONS + 119 * DIF_MISSION_ROW + 0x10) == 11 && *(uint32_t*)(game + DIF_MISSIONS + 120 * DIF_MISSION_ROW + 0x10) == 100);
    assert(!strcmp(TextDetour(g_difficulty.text[0], 0, 0, 0), "HELL FORGE"));
    assert(!strcmp(TextDetour(g_difficulty.text[1], 0, 0, 0), "11") && *(uint32_t*)(row + 0xC) == g_difficulty.text[1]);
    /* The band sprites moved (level 11 takes level 10's). */
    int32_t disp; memcpy(&disp, game + DIF_BAND_SITE + 4, 4);
    uint8_t* bands = (uint8_t*)base + (uint32_t)disp;
    assert(bands == table + DIF_ROWS * DIF_ROW && *(uint64_t*)(bands + 80) == *(uint64_t*)(bands + 72) && *(uint64_t*)bands == 0xBA000);
    /* Each bar site jumps to its stub: below 10 its own instructions and back; at 10 the bar's 11th widget, and back. */
    uint8_t* extra = NULL;
    for (size_t i = 0; i < sizeof kBarSites / sizeof kBarSites[0]; i++)
    {
        const BarSite* s = &kBarSites[i];
        int32_t rel; memcpy(&rel, game + s->rva + 1, 4);
        uint8_t* stub = game + s->rva + 5 + rel, *back = game + s->rva + s->length;
        uint8_t rest = (uint8_t)(s->length - s->keep);
        assert(game[s->rva] == 0xE9 && !memcmp(stub, s->original, s->keep));
        uint8_t* at = stub + s->keep;
        assert(at[0] == 0x83 && at[1] == 0xF8 && at[2] == 10 && at[3] == 0x73 && at[4] == rest + 5);
        assert(!memcmp(at + 5, s->original + s->keep, rest));
        at += 5 + rest; memcpy(&rel, at + 1, 4); assert(at[0] == 0xE9 && at + 5 + rel == back);
        at += 5; assert(at[0] == 0x48 && at[1] == 0x89 && at[2] == s->bar);
        at += 3; memcpy(&rel, at + 1, 4); assert(at[0] == 0xE8);
        if (extra == NULL) extra = at + 5 + rel;
        assert(at + 5 + rel == extra);
        at += 5; assert(!memcmp(at, s->after, s->afterLength));
        at += s->afterLength; memcpy(&rel, at + 1, 4); assert(at[0] == 0xE9 && at + 5 + rel == back);
    }
    /* The icon call goes to a stub: other levels to the game's setter; level 11 to Super Helldive's sprite, then the
     * mod's texture in the image slot, the whole-texture UV rect and the element's refresh. */
    {
        int32_t rel; memcpy(&rel, game + DIF_ICON_CALL + 1, 4);
        uint8_t* stub = game + DIF_ICON_CALL + 5 + rel;
        assert(game[DIF_ICON_CALL] == 0xE8 && stub == extra + 0x40 && stub[0] == 0x83 && stub[1] == 0xFE && stub[2] == 11);
        memcpy(&rel, stub + 5, 4); assert(stub[3] == 0x0F && stub[4] == 0x85 && stub + 9 + rel == game + DIF_SET_SPRITE);
        assert(*(uint64_t*)(stub + 20) == Murmur64A("fixture/icon", 12) && *(uint64_t*)(stub + 30) == 0x5A000 + 10);
        memcpy(&rel, stub + 42, 4); assert(stub[41] == 0xE8 && stub + 46 + rel == game + DIF_SET_SPRITE);
        memcpy(&rel, stub + 66, 4); assert(stub[65] == 0xE8 && stub + 70 + rel == game + DIF_SET_SLOT);
        memcpy(&rel, stub + 127, 4); assert(stub[126] == 0xE9 && stub + 131 + rel == game + DIF_REFRESH_IMAGE);
        assert(!memcmp(stub + 9, kIconStub + 9, 11) && !memcmp(stub + 70, kIconStub + 70, 56));
    }
    /* ExtraWidget is a function of the bar (rcx): each bar keeps its own zeroed widget; a full table reuses the last. */
    typedef uint8_t* (*ExtraWidgetFn)(void* bar);
    ExtraWidgetFn widgetOf = (ExtraWidgetFn)extra;
    uint8_t* first = widgetOf((void*)0x1000), *second = widgetOf((void*)0x2000);
    assert(first != NULL && second == first + DIF_WIDGET && widgetOf((void*)0x1000) == first && ((uintptr_t)first & 15) == 0);
    for (size_t i = 0; i < DIF_WIDGET; i++) assert(first[i] == 0);
    uint8_t* last = NULL;
    for (uintptr_t bar = 3; bar <= DIF_BARS + 4; bar++) last = widgetOf((void*)(bar << 12));
    assert(last == first + (DIF_BARS - 1) * DIF_WIDGET && widgetOf((void*)0x2000) == second);
    char text[1024]; int n = DifficultyItems(text, 0, sizeof text, 1); text[n] = 0;
    assert(strstr(text, "\"kind\":\"difficulty\"") && strstr(text, "\"copyOf\":2695783738"));
    VirtualFree(game, 0, MEM_RELEASE);
}

/* SDK 6 named fields: records by (component, entity); ones other entities share are found only for reading. */
typedef struct FakeRecord { const char* component; uint64_t entity; uint8_t* bytes; uint32_t stride; int shared; } FakeRecord;
static FakeRecord g_fakeRecords[16];
static int g_fakeRecordCount;

static uintptr_t FakeFieldRecord(const char* component, uint64_t entity, uint32_t* stride, int shared)
{
    for (int i = 0; i < g_fakeRecordCount; i++)
        if (!strcmp(g_fakeRecords[i].component, component) && g_fakeRecords[i].entity == entity && (shared || !g_fakeRecords[i].shared))
        {
            *stride = g_fakeRecords[i].stride;
            return (uintptr_t)g_fakeRecords[i].bytes;
        }
    return 0;
}

static uint8_t* FakeRecordOf(const char* component, uint64_t entity, uint32_t stride, int shared)
{
    uint8_t* bytes = calloc(1, stride);
    g_fakeRecords[g_fakeRecordCount++] = (FakeRecord){component, entity, bytes, stride, shared};
    return bytes;
}

static void FieldsFixture(void)
{
    static const struct { const char* name; uint32_t index, stride; } tables[] = {
        {"ProjectileWeaponComponentData", 321, 616}, {"WeaponDataComponentData", 236, 1232}, {"WeaponMagazineComponentData", 5, 160},
        {"TagComponentData", 3, 544}, {"MountComponentData", 283, 120}};
    g_componentCount = 0;
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; i++)
    {
        Component* c = &g_components[g_componentCount++];
        memset(c, 0, sizeof *c);
        strcpy(c->name, tables[i].name); c->index = tables[i].index; c->stride = tables[i].stride;
    }
    g_fieldRecord = FakeFieldRecord;
    HfMod* mod = &g_mods[0];
    const uint64_t sentry = 0x5E, hmg = 0x76;

    static const HfBehaviour orbit = {.size = sizeof(HfBehaviour), .base = HF_BEHAVIOUR_PELICAN_TRANSPORT, .hold_seconds = 90.0f};
    static const HfCopy turretCopies[] = {
        {0x76, HF_FIELD(ProjectileWeapon, projectile)},
        HF_COPY_GROUP(0x76, ProjectileWeapon, firing_sounds),
        {0x5E, HF_FIELD(ProjectileWeapon, fire_rate)},
        HF_COPY_GROUP(0x5E, WeaponData, spread),
        {0}};
    static const HfSet turretSet[] = {HF_SET_GROUP(WeaponData, recoil, HF_F32(0)), {HF_FIELD(WeaponMagazine, capacity), HF_U32(2000)}, {0}};
    static const uint64_t noCredit[] = {HF_TAG_NO_KILL_CREDIT, 0}, extra[] = {0x40, 0};
    static const HfEntityDef turret = {.name = "fixture/turret", .base = HF_ENTITY_PELICAN_TURRET, .copy = turretCopies, .set = turretSet,
        .remove_tags = noCredit, .add_tags = extra};
    static const HfMountDef mounts[] = {{.slot = 0, .entity = &turret}, {0}};
    static const HfEntityDef pelican = {.name = "fixture/pelican", .base = HF_ENTITY_PELICAN_TRANSPORT, .behaviour = &orbit, .mount = mounts};
    static const HfEntityDef twin = {.name = "fixture/twin", .base = HF_ENTITY_PELICAN_TRANSPORT, .behaviour = &orbit};

    /* Registration: the carried turret first, one behaviour for both Pelicans, records of their own where they change. */
    uint64_t p = ApiRegisterEntity(mod, &pelican), t = Murmur64A("fixture/turret", 14);
    assert(p == Murmur64A("fixture/pelican", 15) && ApiRegisterEntity(mod, &turret) == t && ApiRegisterEntity(mod, &pelican) == p);
    assert(ApiRegisterEntity(mod, &twin) && g_behaviourCount == 1 && g_weaponCount == 3);
    assert(g_weapons[0].entity == t && g_weapons[0].ownCount == 4 && g_weapons[1].entity == p && g_weapons[1].ownCount == 1 &&
        g_weapons[1].own[0] == 283 && g_weapons[1].behaviour == BHV_FIRST && g_weapons[2].behaviour == BHV_FIRST);
    assert(g_editCount == 1 + 15 + 1 + 2 + 4 + 1 + 1 + 1 + 1);

    /* Refused fields add nothing: another layout, an unmapped offset, a wrong type, a value out of range, an array set. */
    HfField moved = HF_FIELD(WeaponData, horizontal_spread), unmapped = HF_FIELD(WeaponData, horizontal_spread), array = HF_FIELD(ProjectileWeapon, fire_rate);
    moved.layout = 1; unmapped.offset = 0x38;
    HfField capacity = HF_FIELD(WeaponMagazine, capacity);
    HfValue f32 = HF_F32(1), u32 = HF_U32(1), negative = HF_U32(-1), fraction = HF_U32(1.5);
    HfSet bad[][2] = {{{moved, f32}, {0}}, {{unmapped, f32}, {0}}, {{capacity, f32}, {0}}, {{capacity, negative}, {0}}, {{capacity, fraction}, {0}},
        {{array, f32}, {0}}};
    static HfEntityDef refused[6];
    for (int i = 0; i < 6; i++)
    {
        refused[i] = (HfEntityDef){.name = "fixture/refused", .base = HF_ENTITY_PELICAN_TURRET, .set = bad[i]};
        assert(ApiRegisterEntity(mod, &refused[i]) == 0);
    }
    assert(g_weaponCount == 3 && g_editCount == 27);
    /* A short name is the mod's: "<mod id>:<name>"; find resolves catalog names and mod content. */
    static const HfEntityDef shortName = {.name = "short", .base = HF_ENTITY_PELICAN_TRANSPORT};
    uint64_t s = ApiRegisterEntity(mod, &shortName);
    assert(s == Murmur64A("a:short", 7) && ApiFind(HF_KIND_ENTITY, "a:short") == s && ApiFind(HF_KIND_ENTITY, "fixture/turret") == t);
    assert(ApiFind(HF_KIND_ENTITY, "Pelican Transport") == HF_ENTITY_PELICAN_TRANSPORT && ApiFind(HF_KIND_ITEM, "MG-206 Heavy Machine Gun") ==
        HF_ITEM_MG206_HEAVY_MACHINE_GUN && !ApiFind(HF_KIND_ITEM, "Pelican Transport") && !ApiFind(HF_KIND_ENTITY, "pelican transport"));

    /* The data loads: own records are copies of their base's; edits apply, then again on a reload. */
    for (int i = 0; i < 3; i++) g_weapons[i].state = 1;
    uint8_t* hmgWeapon = FakeRecordOf("ProjectileWeaponComponentData", hmg, 616, 1);
    uint8_t* sentryWeapon = FakeRecordOf("ProjectileWeaponComponentData", sentry, 616, 1);
    uint8_t* sentryData = FakeRecordOf("WeaponDataComponentData", sentry, 1232, 1);
    uint8_t* weapon = FakeRecordOf("ProjectileWeaponComponentData", t, 616, 0);
    uint8_t* data = FakeRecordOf("WeaponDataComponentData", t, 1232, 0);
    uint8_t* magazine = FakeRecordOf("WeaponMagazineComponentData", t, 160, 0);
    uint8_t* tags = FakeRecordOf("TagComponentData", t, 544, 0);
    uint8_t* mount = FakeRecordOf("MountComponentData", p, 120, 0);
    HfProjectileWeapon* hmgFields = (HfProjectileWeapon*)hmgWeapon;
    hmgFields->projectile = 275; hmgFields->fire_single_audio_event = 0x825E6711; hmgFields->use_midi_event_system = 1; hmgFields->silenced = 1;
    ((HfProjectileWeapon*)sentryWeapon)->fire_rate[1] = 1600.0f;
    ((HfWeaponData*)sentryData)->horizontal_spread = 10.0f; ((HfWeaponData*)sentryData)->vertical_spread = 10.0f;
    for (int load = 0; load < 2; load++)
    {
        HfWeaponData* d = (HfWeaponData*)data;
        d->recoil_drift_horizontal = 2.5f; d->recoil_climb_vertical = 10.0f;
        const float half = 0.5f; memcpy(data + 0x38, &half, 4); /* the unknown at +0x38 stays */
        memset(weapon, 0, 616); ((HfProjectileWeapon*)weapon)->projectile = 120;
        uint64_t chin[] = {0x4, 0x02000000, HF_TAG_NO_KILL_CREDIT, 0};
        memset(tags, 0, 544); memcpy(tags, chin, sizeof chin);
        memset(mount, 0, 120); ((HfMount*)mount)->slot0_entity = HF_ENTITY_PELICAN_TURRET;
        ApplyFieldEdits();
        HfProjectileWeapon* w = (HfProjectileWeapon*)weapon;
        HfTag* tag = (HfTag*)tags;
        assert(w->projectile == 275 && w->fire_rate[1] == 1600.0f && w->fire_single_audio_event == 0x825E6711 && w->use_midi_event_system == 1 && w->silenced == 1);
        assert(d->recoil_drift_horizontal == 0.0f && d->recoil_climb_vertical == 0.0f && d->horizontal_spread == 10.0f && d->vertical_spread == 10.0f);
        assert(!memcmp(data + 0x38, &half, 4) && ((HfWeaponMagazine*)magazine)->capacity == 2000);
        assert(tag->tags[0] == 0x4 && tag->tags[1] == 0x02000000 && tag->tags[2] == 0x40 && tag->tags[3] == 0 && ((HfMount*)mount)->slot0_entity == t);
        for (int i = 0; i < g_editCount; i++) assert(!g_edits[i].failed);
    }

    /* Typed records check the layout; after the load, edits apply at once but need records the entity owns. */
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 1);
    assert(ApiRecord(mod, t, HfWeaponMagazine_INFO, sizeof(HfWeaponMagazine)) == magazine);
    assert(!ApiRecord(mod, t, "WeaponMagazineComponentData", 1, sizeof(HfWeaponMagazine)) && !ApiRecord(mod, t, HfWeaponMagazine_INFO, 8));
    HfValue more = HF_U32(1500);
    assert(ApiSetField(mod, t, &capacity, &more) == 0 && ((HfWeaponMagazine*)magazine)->capacity == 1500);
    assert(ApiSetField(mod, p, &capacity, &more) == 1);
    /* Vanilla records: edited when no other entity shares them; mods (and stats mods) writing the same field are reported. */
    uint8_t* chinMagazine = FakeRecordOf("WeaponMagazineComponentData", HF_ENTITY_PELICAN_TURRET, 160, 0);
    uint8_t* sharedMagazine = FakeRecordOf("WeaponMagazineComponentData", sentry, 160, 1);
    assert(ApiSetField(mod, HF_ENTITY_PELICAN_TURRET, &capacity, &more) == 0 && ((HfWeaponMagazine*)chinMagazine)->capacity == 1500 && !g_fieldConflicts);
    assert(ApiSetField(&g_mods[1], HF_ENTITY_PELICAN_TURRET, &capacity, &u32) == 0 && ((HfWeaponMagazine*)chinMagazine)->capacity == 1 && g_fieldConflicts == 1);
    g_stats[0] = Edit(1, 5, 136, 0); strcpy(g_stats[0].component, "WeaponMagazineComponentData"); g_stats[0].resource = HF_ENTITY_PELICAN_TURRET;
    g_statCount = 1;
    assert(ApiSetField(mod, HF_ENTITY_PELICAN_TURRET, &capacity, &more) == 0 && g_fieldConflicts == 3);
    g_statCount = 0;
    assert(ApiSetField(mod, sentry, &capacity, &more) == 0 && g_edits[g_editCount - 1].failed && ((HfWeaponMagazine*)sharedMagazine)->capacity == 0);
    /* A missing source is reported, and the rest still applies. */
    g_fakeRecords[1].entity = 0x99; /* the sentry's ProjectileWeapon record is gone */
    ((HfWeaponMagazine*)magazine)->capacity = 1;
    ApplyFieldEdits();
    int failed = 0;
    for (int i = 0; i < g_editCount; i++) failed += g_edits[i].failed;
    assert(failed == 2 && ((HfWeaponMagazine*)magazine)->capacity == 1500 && ((HfWeaponMagazine*)chinMagazine)->capacity == 1500);

    /* A stratagem naming no package loads its payload's (the Pelican's model or base's LoadoutPackage); a named one stays. */
    uint8_t* package = FakeRecordOf("LoadoutPackageComponentData", HF_ENTITY_PELICAN_TRANSPORT, 32, 1);
    ((HfLoadoutPackage*)package)->package = HF_PACKAGE_PELICAN_GUNSHIP;
    uint8_t rows[2][400] = {{0}};
    for (int i = 0; i < 2; i++)
    {
        Stratagem* st = &g_stratagems[i];
        memset(st, 0, sizeof *st);
        st->mod = mod; strcpy(st->name, "a:drop"); st->row = rows[i]; st->payload[1] = p; st->payloadCount = 2;
    }
    g_stratagems[1].package = 7; g_stratagemCount = 2;
    InferStratagemPackages();
    assert(*(uint64_t*)(rows[0] + 0xA8) == HF_PACKAGE_PELICAN_GUNSHIP && *(uint64_t*)(rows[1] + 0xA8) == 0 && g_packagesInferred);
    g_stratagemCount = 0;

    for (int i = 0; i < g_fakeRecordCount; i++) free(g_fakeRecords[i].bytes);
    g_fakeRecordCount = g_componentCount = g_weaponCount = g_editCount = g_entityDefCount = g_behaviourDefCount = 0;
    g_behaviourCount = 0;
    g_fieldConflicts = 0;
    g_fieldRecord = ComponentRecordOf;
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 0);
}

/* SDK 6 live edits: table rows by id, waiting tables, replace and reset, shared records, array elements, the pipe's requests. */
static uint8_t g_fakeProjectile[272];
static int g_fakeTablesReady;

static uint8_t g_fakeStratagem[0x120]; /* StratagemDefinition 7, stable id 0xABCD */
static uintptr_t FakeTableRow(const char* component, uint64_t id, int* ready)
{
    *ready = g_fakeTablesReady;
    if (g_fakeTablesReady && !strcmp(component, "StratagemDefinition") && id == 7) return (uintptr_t)g_fakeStratagem;
    return g_fakeTablesReady && !strcmp(component, "ProjectileSettings") && id == 5 ? (uintptr_t)g_fakeProjectile : 0;
}

static void LiveFixture(void)
{
    static const struct { const char* name; uint32_t index, stride; } tables[] = {
        {"ProjectileWeaponComponentData", 321, 616}, {"WeaponMagazineComponentData", 5, 160}};
    g_componentCount = 0;
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; i++)
    {
        Component* c = &g_components[g_componentCount++];
        memset(c, 0, sizeof *c);
        strcpy(c->name, tables[i].name); c->index = tables[i].index; c->stride = tables[i].stride;
    }
    g_fieldRecord = FakeFieldRecord;
    g_tableRow = FakeTableRow;
    HfMod* mod = &g_mods[0];
    /* the editor's live edits (an editor's are never named as a field's holder; a code or Lua mod's are) */
    char modId[80];
    strcpy(modId, mod->id);
    strcpy(mod->id, "hellforge.editor");
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 1);

    /* A table row by id: waits for its table, then applies now; a later live edit replaces it; a reset puts back the old value. */
    HfField velocity = HF_FIELD(ProjectileSettings, velocity);
    HfValue fast = HF_F32(1200), faster = HF_F32(1500);
    float speed = 385.0f, read;
    memcpy(g_fakeProjectile + 0x20, &speed, 4);
    assert(ApiSetLive(mod, 5, &velocity, &fast, 0) == HF_APPLIES_NOW && g_editCount == 1 && g_edits[0].waiting && g_editsWaiting);
    g_fakeTablesReady = 1;
    ApplyWaitingEdits();
    memcpy(&read, g_fakeProjectile + 0x20, 4); assert(read == 1200.0f && !g_edits[0].waiting);
    assert(ApiSetLive(mod, 5, &velocity, &faster, 0) == HF_APPLIES_NOW && g_editCount == 1);
    memcpy(&read, g_fakeProjectile + 0x20, 4); assert(read == 1500.0f);
    assert(ApiSetLive(mod, 5, &velocity, NULL, 0) == HF_APPLIES_NOW && g_editCount == 0);
    memcpy(&read, g_fakeProjectile + 0x20, 4); assert(read == 385.0f);
    assert(ApiSetLive(mod, 6, &velocity, &fast, 0) == 0 && g_editCount == 0); /* no such row: refused, not kept */

    /* An element of an array field; a shared record only with HF_LIVE_SHARED. */
    uint8_t* weapon = FakeRecordOf("ProjectileWeaponComponentData", 0x77, 616, 0);
    uint8_t* magazine = FakeRecordOf("WeaponMagazineComponentData", 0x77, 160, 1);
    HfField rate = HF_FIELD_AT(ProjectileWeapon, fire_rate, 1), capacity = HF_FIELD(WeaponMagazine, capacity);
    HfValue rpm = HF_F32(900), rounds = HF_U32(60);
    assert(ApiSetLive(mod, 0x77, &rate, &rpm, 0) == HF_APPLIES_RESPAWN && ((HfProjectileWeapon*)weapon)->fire_rate[1] == 900.0f &&
        ((HfProjectileWeapon*)weapon)->fire_rate[0] == 0.0f && !strcmp(g_edits[0].field->name, "fire_rate[1]"));
    assert(ApiSetLive(mod, 0x77, &capacity, &rounds, 0) == 0 && g_editCount == 1 && ((HfWeaponMagazine*)magazine)->capacity == 0);
    assert(ApiSetLive(mod, 0x77, &capacity, &rounds, HF_LIVE_SHARED) == HF_APPLIES_RESPAWN && ((HfWeaponMagazine*)magazine)->capacity == 60);
    /* get_live: the value now and the base, without the Local Stats change and live edits (another mod's value stays the
     * base); a reset with HF_LIVE_BASE goes back to the base (an editor removing a saved change). */
    HfLiveValue live;
    float base = 640.0f, other = 700.0f, saved = 1000.0f;
    HfMod* local = &g_mods[2];
    strcpy(local->id, "zz.local.stats");
    ((HfProjectileWeapon*)weapon)->fire_rate[1] = saved; /* (the live edit above is still there: the base is from before it) */
    assert(ApiGetLive(mod, 0x77, &rate, &live) == HF_APPLIES_RESPAWN && live.now.type == HF_TYPE_F32 && live.now.number == 1000.0 &&
        live.base.number == 0.0 && live.mod == NULL);
    g_stats[0] = Edit(2, 0, 8, 0); g_stats[0].applied = 1; g_stats[0].address = (uintptr_t)(weapon + 8); memcpy(&g_stats[0].original, &base, 4);
    g_statCount = 1;
    assert(ApiGetLive(mod, 0x77, &rate, &live) && live.now.number == 1000.0 && live.base.number == 640.0 && live.mod == NULL);
    assert(ApiSetLive(mod, 0x77, &rate, &rpm, 0) && ApiGetLive(mod, 0x77, &rate, &live) && live.now.number == 900.0 && live.base.number == 640.0);
    assert(ApiSetLive(mod, 0x77, &rate, NULL, HF_LIVE_BASE) == HF_APPLIES_RESPAWN && ((HfProjectileWeapon*)weapon)->fire_rate[1] == 640.0f);
    /* Another mod's stats value under Local Stats: it is the base, and stays. */
    g_stats[1] = g_stats[0]; g_stats[0].mod = &g_mods[1]; memcpy(&g_stats[1].original, &other, 4); g_statCount = 2;
    ((HfProjectileWeapon*)weapon)->fire_rate[1] = saved;
    assert(ApiGetLive(mod, 0x77, &rate, &live) && live.base.number == 700.0 && live.mod != NULL && !strcmp(live.mod, g_mods[1].id));
    assert(ApiSetLive(mod, 0x77, &rate, NULL, HF_LIVE_BASE) && ((HfProjectileWeapon*)weapon)->fire_rate[1] == 700.0f);
    /* Without a Local Stats edit HF_LIVE_BASE leaves another mod's value alone. */
    g_stats[0] = g_stats[1]; g_stats[0].mod = &g_mods[1]; g_statCount = 1;
    assert(ApiSetLive(mod, 0x77, &rate, NULL, HF_LIVE_BASE) && ((HfProjectileWeapon*)weapon)->fire_rate[1] == 700.0f);
    assert(ApiGetLive(mod, 0x77, &rate, &live) && live.mod != NULL && live.base.number == 700.0);
    assert(ApiGetLive(mod, 0x78, &rate, &live) == 0); /* no such record */
    g_statCount = 0;
    local->id[0] = 0;
    strcpy(mod->id, modId);

    /* The pipe's requests name fields as the stats schema does. */
    char line[256], reply[512];
    uint32_t bits; float f = 1300.0f; memcpy(&bits, &f, 4);
    snprintf(line, sizeof line, "set\t7\tProjectileSettings\t-\t0\t5\t32\tf32\t%08x\t0", bits);
    LiveRequest(line, reply, sizeof reply); assert(!strcmp(reply, "7\tok\tnow"));
    memcpy(&read, g_fakeProjectile + 0x20, 4); assert(read == 1300.0f);
    strcpy(line, "get\t8\tProjectileSettings\t-\t0\t5\t32\tf32");
    LiveRequest(line, reply, sizeof reply); snprintf(line, sizeof line, "8\tok\tnow\t%08x", bits); assert(!strcmp(reply, line));
    strcpy(line, "set\t9\tcomponent\tWeaponMagazineComponentData\t0000000000000077\t-1\t136\tu32\t0000004b\t1");
    LiveRequest(line, reply, sizeof reply); assert(!strcmp(reply, "9\tok\trespawn") && ((HfWeaponMagazine*)magazine)->capacity == 75);
    strcpy(line, "set\t10\tProjectileSettings\t-\t0\t5\t33\tf32\t00000000\t0");
    LiveRequest(line, reply, sizeof reply); assert(!strncmp(reply, "10\trefused\t", 11) && strstr(reply, "not a reviewed field"));
    strcpy(line, "set\t11\tEntityDelta\t5:136\t1\t2\t40\tu32\t00000001\t0");
    LiveRequest(line, reply, sizeof reply); assert(!strncmp(reply, "11\trefused\t", 11));
    strcpy(line, "reset\t12\tProjectileSettings\t-\t0\t5\t32\tf32");
    LiveRequest(line, reply, sizeof reply); assert(!strcmp(reply, "12\tok\tnow"));
    memcpy(&read, g_fakeProjectile + 0x20, 4); assert(read == 385.0f);
    strcpy(line, "bogus"); LiveRequest(line, reply, sizeof reply); assert(strstr(reply, "refused"));
    /* base: the field without the Local Stats mod's edits (a co-op joiner's own values give way to the host's) */
    snprintf(line, sizeof line, "set\t13\tProjectileSettings\t-\t0\t5\t32\tf32\t%08x\t0", bits);
    LiveRequest(line, reply, sizeof reply); assert(!strcmp(reply, "13\tok\tnow"));
    strcpy(line, "base\t14\tProjectileSettings\t-\t0\t5\t32\tf32");
    LiveRequest(line, reply, sizeof reply); assert(!strcmp(reply, "14\tok\tnow"));
    memcpy(&read, g_fakeProjectile + 0x20, 4); assert(read == 385.0f);
    /* session: the co-op role the backend gives, for the editor; solo again when the pipe closes */
    HfSession session;
    ApiSession(NULL, &session); assert(session.role == HF_SESSION_SOLO);
    unsigned before = session.version;
    strcpy(line, "session\t15\tjoiner\tFriend One");
    LiveRequest(line, reply, sizeof reply); assert(!strcmp(reply, "15\tok\tnow"));
    ApiSession(NULL, &session); assert(session.role == HF_SESSION_JOINER && !strcmp(session.host, "Friend One") && session.version == before + 1);
    assert(ApiStatsSource(NULL) == HF_STATS_HOST); /* a joiner without stats= plays with the host's, as before SDK 15 */
    strcpy(line, "session\t15\tjoiner\tstats=own\tFriend One");
    LiveRequest(line, reply, sizeof reply); ApiSession(NULL, &session);
    assert(!strcmp(reply, "15\tok\tnow") && session.role == HF_SESSION_JOINER && !strcmp(session.host, "Friend One") &&
        session.version == before + 2 && ApiStatsSource(NULL) == HF_STATS_OWN);
    strcpy(line, "session\t15\tjoiner\tstats=host\tFriend One");
    LiveRequest(line, reply, sizeof reply); assert(ApiStatsSource(NULL) == HF_STATS_HOST);
    strcpy(line, "session\t15\thost\tstats=host"); /* hosting, friends play with this machine's */
    LiveRequest(line, reply, sizeof reply); ApiSession(NULL, &session);
    assert(session.role == HF_SESSION_HOST && session.host[0] == 0 && ApiStatsSource(NULL) == HF_STATS_HOST);
    strcpy(line, "session\t15\thost\tstats=own");
    LiveRequest(line, reply, sizeof reply); assert(ApiStatsSource(NULL) == HF_STATS_OWN);
    strcpy(line, "session\t16\tsolo\tstats=host"); /* solo: always this machine's own */
    LiveRequest(line, reply, sizeof reply); ApiSession(NULL, &session);
    assert(session.role == HF_SESSION_SOLO && session.host[0] == 0 && ApiStatsSource(NULL) == HF_STATS_OWN);
    /* notice (SDK 12): the backend's message for the player, a title and its lines, numbered */
    HfNotice notice;
    ApiNotice(NULL, &notice); assert(notice.version == 0);
    strcpy(line, "notice\t17\terror\tCOULDN'T JOIN FRIEND ONE\tYour gameplay mods don't match the host's.\tMissing: Example 1.0");
    LiveRequest(line, reply, sizeof reply); assert(!strcmp(reply, "17\tok\tnow"));
    ApiNotice(NULL, &notice);
    assert(notice.version == 1 && notice.kind == HF_NOTICE_ERROR && !strcmp(notice.title, "COULDN'T JOIN FRIEND ONE") &&
        !strcmp(notice.text, "Your gameplay mods don't match the host's.\nMissing: Example 1.0"));
    strcpy(line, "notice\t18\tinfo\tA title only");
    LiveRequest(line, reply, sizeof reply); ApiNotice(NULL, &notice);
    assert(notice.version == 2 && notice.kind == HF_NOTICE_INFO && !strcmp(notice.title, "A title only") && notice.text[0] == 0);
    strcpy(line, "notice	19	brief	FRIEND TWO couldn't join	Their gameplay mods don't match yours.");
    LiveRequest(line, reply, sizeof reply); ApiNotice(NULL, &notice);
    assert(notice.version == 3 && notice.kind == HF_NOTICE_BRIEF && !strcmp(notice.text, "Their gameplay mods don't match yours."));
    /* the live pipe is named after the run folder: the log folder, or, sandboxed, the run of its game folder */
    wchar_t pipe[MAX_PATH];
    assert(LivePipePath(L"C:\\HellForge\\logs\\sessions\\0123abcd", pipe, MAX_PATH) && !wcscmp(pipe, L"\\\\.\\pipe\\hellforge-live-0123abcd"));
    assert(LivePipePath(L"C:\\HellForge\\logs\\sessions\\0123abcd\\game", pipe, MAX_PATH) && !wcscmp(pipe, L"\\\\.\\pipe\\hellforge-live-0123abcd"));
    assert(LivePipePath(L"C:\\HellForge\\logs\\sessions\\0123abcd\\Game", pipe, MAX_PATH) && !wcscmp(pipe, L"\\\\.\\pipe\\hellforge-live-0123abcd"));
    assert(!LivePipePath(L"game", pipe, MAX_PATH) && !LivePipePath(L"C:\\HellForge\\", pipe, MAX_PATH));

    for (int i = 0; i < g_fakeRecordCount; i++) free(g_fakeRecords[i].bytes);
    g_fakeRecordCount = g_componentCount = g_editCount = 0;
    g_fieldRecord = ComponentRecordOf;
    g_tableRow = TableRowOf;
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 0);
}

/* Armor passives: the local player's record found through the player list and the entity map in a game image of the
 * fixture's own; an override written, kept through a kit change, refused for a foreign value, restored with a second
 * player and on release; packaged and armor-only passives refused; the pins checked. */
/* The Lua bridge: Lua mods by name, any value of a record, live edits stacked editor > Lua > code, a Lua mod named
 * as the holder, Local Stats' saved values, vanilla record owners, the event queue. */
static uint32_t MagazineCapacity(const uint8_t* magazine) { uint32_t v; memcpy(&v, magazine + 136, 4); return v; }

static void LuaBridgeFixture(void)
{
    static const struct { const char* name; uint32_t index, stride; } tables[] = {
        {"ProjectileWeaponComponentData", 321, 616}, {"WeaponMagazineComponentData", 5, 160}};
    g_componentCount = 0;
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; i++)
    {
        Component* c = &g_components[g_componentCount++];
        memset(c, 0, sizeof *c);
        strcpy(c->name, tables[i].name); c->index = tables[i].index; c->stride = tables[i].stride;
    }
    g_fieldRecord = FakeFieldRecord;
    g_tableRow = FakeTableRow;
    g_fakeTablesReady = 1;
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 1);
    int mods = g_modCount;

    /* Lua mods by name: lua.<name>, the same name the same mod */
    HfMod* lua = hellforge_lua_mod("mods/Someone/Fast Guns");
    assert(lua != NULL && lua->lua && lua->loaded && !strcmp(lua->id, "lua.mods-someone-fast-guns") && g_modCount == mods + 1);
    assert(hellforge_lua_mod("mods/Someone/Fast Guns") == lua && hellforge_lua_mod("") == NULL && hellforge_lua_mod(NULL) == NULL);
    HfMod* second = hellforge_lua_mod("second");
    assert(second != NULL && second != lua && !strcmp(second->id, "lua.second"));
    HfMod* code = &g_mods[0];
    HfMod* editor = &g_mods[1];
    char editorId[80];
    strcpy(editorId, editor->id);
    strcpy(editor->id, "hellforge.editor");
    assert(ModRank(code) == RANK_CODE && ModRank(lua) == RANK_LUA && ModRank(editor) == RANK_EDITOR);
    /* SDK 10, mods: the plan's mods, then the Lua mods with the names their runtime gave */
    HfModInfo m0 = {sizeof m0}, mLua = {sizeof mLua}, past = {sizeof past};
    assert(ApiMods(NULL, 0, &m0) && m0.kind == HF_MOD_CODE && !strcmp(m0.id, code->id) && m0.loaded == 1);
    assert(ApiMods(NULL, (uint32_t)(lua - g_mods), &mLua) && mLua.kind == HF_MOD_LUA && !strcmp(mLua.name, "mods/Someone/Fast Guns"));
    assert(ApiMods(NULL, 1, &past) && past.kind == HF_MOD_EDITOR && !ApiMods(NULL, (uint32_t)g_modCount, &past));
    HfLuaInfo info = {sizeof info};
    assert(hellforge_lua_info(&info) && info.bridge == LUA_BRIDGE_VERSION && info.sdk == HF_MOD_SDK_VERSION && !strcmp(info.build, "25480438") &&
        (info.ready & 2));
    assert(hellforge_lua_set(code, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32, 60, 0, 0) == 0); /* a code mod is not a Lua mod */

    /* any aligned value of a record or row for a Lua mod (HD2Runtime's catalogues); a code mod only the mapped ones */
    uint8_t* magazine = FakeRecordOf("WeaponMagazineComponentData", 0x90, 160, 0);
    float speed = 1.0f, read;
    memcpy(g_fakeProjectile + 48, &speed, 4);
    assert(hellforge_lua_set(lua, "ProjectileSettings", 5, 48, HF_TYPE_F32, 2.5, 0, 0) == HF_APPLIES_NOW);
    memcpy(&read, g_fakeProjectile + 48, 4); assert(read == 2.5f);
    HfField raw = {"ProjectileSettings", kSdkComponents[0].layout, 48, 4, HF_TYPE_F32, 0};
    HfValue two = HF_F32(2);
    assert(ApiSetLive(code, 5, &raw, &two, 0) == 0);
    assert(hellforge_lua_set(lua, "ProjectileSettings", 5, 49, HF_TYPE_F32, 1, 0, 0) == 0); /* not aligned */
    assert(hellforge_lua_set(lua, "ProjectileSettings", 5, 270, HF_TYPE_F32, 1, 0, 0) == 0); /* past the row */
    assert(hellforge_lua_set(lua, "NoSuchComponent", 5, 48, HF_TYPE_F32, 1, 0, 0) == 0 && strstr(hellforge_lua_last(lua), "no mappings"));
    HfLuaValue v;
    assert(hellforge_lua_get(lua, "ProjectileSettings", 5, 48, HF_TYPE_F32, &v) == HF_APPLIES_NOW && v.now == 2.5 && v.type == HF_TYPE_F32);
    assert(hellforge_lua_clear(lua, "ProjectileSettings", 5, 48, HF_TYPE_F32) == HF_APPLIES_NOW);
    memcpy(&read, g_fakeProjectile + 48, 4); assert(read == 1.0f);

    /* stacked: a code mod's, then a Lua mod's over it, then the editor's over that; each leaves back to the one under it */
    HfField capacity = HF_FIELD(WeaponMagazine, capacity);
    HfValue c40 = HF_U32(40), c99 = HF_U32(99);
    uint32_t vanilla = 30;
    memcpy(magazine + 136, &vanilla, 4);
    int edits = g_editCount;
    assert(ApiSetLive(code, 0x90, &capacity, &c40, 0) && MagazineCapacity(magazine) == 40);
    assert(hellforge_lua_set(lua, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32, 50, 0, 0) == HF_APPLIES_RESPAWN && MagazineCapacity(magazine) == 50);
    assert(ApiSetLive(editor, 0x90, &capacity, &c99, 0) && MagazineCapacity(magazine) == 99 && g_editCount == edits + 3);
    HfLiveValue live;
    assert(ApiGetLive(editor, 0x90, &capacity, &live) && live.now.number == 99 && live.base.number == 50 && live.mod == lua->id);
    assert(ApiSetLive(editor, 0x90, &capacity, NULL, 0) && MagazineCapacity(magazine) == 50);
    assert(hellforge_lua_get(lua, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32, &v) && v.now == 50 && v.holder == lua->id);
    assert(hellforge_lua_clear(lua, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32) && MagazineCapacity(magazine) == 40);
    assert(ApiGetLive(editor, 0x90, &capacity, &live) && live.mod == code->id && live.base.number == 40); /* a code mod's is named too */
    assert(ApiSetLive(code, 0x90, &capacity, NULL, 0) && MagazineCapacity(magazine) == 30 && g_editCount == edits);

    /* in the other order: the editor's first; a Lua mod's goes under it (kept, not written), then a code mod's under that */
    assert(ApiSetLive(editor, 0x90, &capacity, &c99, 0) && MagazineCapacity(magazine) == 99);
    assert(hellforge_lua_set(lua, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32, 50, 0, 0) == HF_APPLIES_RESPAWN && MagazineCapacity(magazine) == 99);
    assert(ApiSetLive(code, 0x90, &capacity, &c40, 0) && MagazineCapacity(magazine) == 99);
    assert(g_edits[edits].mod == code && g_edits[edits + 1].mod == lua && g_edits[edits + 2].mod == editor); /* rank order */
    assert(ApiGetLive(editor, 0x90, &capacity, &live) && live.base.number == 50 && live.mod == lua->id);
    assert(hellforge_lua_set(lua, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32, 55, 0, 0) && MagazineCapacity(magazine) == 99); /* replaced, still under */
    assert(ApiSetLive(editor, 0x90, &capacity, NULL, 0) && MagazineCapacity(magazine) == 55);
    assert(hellforge_lua_clear(lua, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32) && MagazineCapacity(magazine) == 40);
    assert(ApiSetLive(code, 0x90, &capacity, NULL, 0) && MagazineCapacity(magazine) == 30 && g_editCount == edits);
    /* two Lua mods: the later one's on top */
    assert(hellforge_lua_set(lua, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32, 50, 0, 0) && hellforge_lua_set(second, "WeaponMagazineComponentData", 0x90, 136,
        HF_TYPE_U32, 60, 0, 0) && MagazineCapacity(magazine) == 60);
    assert(hellforge_lua_clear(second, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32) && MagazineCapacity(magazine) == 50);
    assert(hellforge_lua_clear(lua, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32) && MagazineCapacity(magazine) == 30);

    /* a value Local Stats saved and the boot pass wrote: a Lua mod's live edit stays under it while the record holds it */
    uint32_t savedValue = 77;
    memcpy(magazine + 136, &savedValue, 4);
    HfMod localStats = {0};
    localStats.api = &g_api; strcpy(localStats.id, "zz.local.stats"); localStats.loaded = 1; localStats.log = INVALID_HANDLE_VALUE;
    g_stats[0] = Edit(0, 77, 136, 0); g_stats[0].mod = &localStats; g_stats[0].applied = 1; g_stats[0].address = (uintptr_t)(magazine + 136);
    g_stats[0].original = 30; g_statCount = 1;
    assert(hellforge_lua_set(lua, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32, 50, 0, 0) == HF_APPLIES_RESPAWN && MagazineCapacity(magazine) == 77);
    assert(ApiGetLive(editor, 0x90, &capacity, &live) && live.base.number == 50 && live.mod == lua->id);
    vanilla = 30;
    memcpy(magazine + 136, &vanilla, 4); /* the editor reset it: the Lua mod's value is free to apply */
    assert(hellforge_lua_set(lua, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32, 51, 0, 0) && MagazineCapacity(magazine) == 51);
    assert(hellforge_lua_clear(lua, "WeaponMagazineComponentData", 0x90, 136, HF_TYPE_U32) && MagazineCapacity(magazine) == 30 && g_editCount == edits);
    g_statCount = 0;

    /* the entity a vanilla record belongs to, from the entity data's index (two rows naming record 1: a shared record) */
    static uint8_t region[256];
    Component* m = &g_components[1];
    m->offset = 64; m->indices = 3; m->recordOffset = 48; m->records = 4;
    memset(m->header, 0, sizeof m->header);
    memset(region, 0, sizeof region);
    uint32_t index = m->index; memcpy(region + 60, &index, 4);
    uint64_t ids[3] = {0xA1, 0xB2, 0xC3}; uint32_t records[3] = {0, 1, 1};
    for (int i = 0; i < 3; i++) { memcpy(region + 92 + i * 16, &ids[i], 8); memcpy(region + 92 + i * 16 + 8, &records[i], 4); }
    uintptr_t savedRegion = g_region;
    g_region = (uintptr_t)region;
    uint32_t owners = 0;
    assert(hellforge_lua_owner("WeaponMagazineComponentData", 0, &owners) == 0xA1 && owners == 1);
    assert(hellforge_lua_owner("WeaponMagazine", 1, &owners) == 0xB2 && owners == 2);
    assert(hellforge_lua_owner("WeaponMagazineComponentData", 3, &owners) == 0 && owners == 0);
    g_region = savedRegion;

    /* code HellForge may rewrite: the projectile list's instructions, a hooked function; not the rest */
    assert(hellforge_lua_rewrites(0x13A9715u, 7) && hellforge_lua_rewrites(0x13A9718u, 1) && !hellforge_lua_rewrites(0x13A971Cu, 4));
    assert(hellforge_lua_rewrites(REG_ENTITY_LOADER, 1) && !hellforge_lua_rewrites(0x10u, 4));

    /* the event queue the Lua runtime empties from its update */
    HfStateEvent state = {{sizeof state, HF_ON_STATE, 0}, HF_STATE_SHIP, HF_STATE_MISSION};
    LuaListener(lua, &state.info, NULL);
    LuaListener(lua, &state.info, NULL);
    HfLuaEvent got[1];
    uint32_t dropped = 9;
    assert(hellforge_lua_poll(got, 1, &dropped) == 1 && dropped == 0 && ((HfStateEvent*)got[0].data)->to == HF_STATE_MISSION);
    assert(hellforge_lua_poll(got, 1, &dropped) == 1 && hellforge_lua_poll(got, 1, &dropped) == 0);
    for (int i = 0; i < LUA_EVENT_QUEUE + 3; i++) LuaListener(lua, &state.info, NULL);
    HfLuaEvent* all = calloc(LUA_EVENT_QUEUE, sizeof *all);
    assert(hellforge_lua_poll(all, LUA_EVENT_QUEUE, &dropped) == LUA_EVENT_QUEUE && dropped == 3);
    free(all);

    /* a stratagem's input code by its stable id: the loader's array, the row's own back; refusals */
    {
        static uint32_t native[3] = {1, 2, 3};
        uint32_t type = 7, stable = 0xABCD, three = 3, *at = native, count;
        uint64_t pointer;
        memset(g_fakeStratagem, 0, sizeof g_fakeStratagem);
        memcpy(g_fakeStratagem, &type, 4); memcpy(g_fakeStratagem + 4, &stable, 4);
        memcpy(g_fakeStratagem + 0x40, &at, 8); memcpy(g_fakeStratagem + 0x48, &three, 4);
        const uint32_t longer[5] = {4, 4, 3, 2, 1}, shorter[2] = {2, 2}, bad[2] = {1, 5};
        assert(hellforge_lua_set_code(lua, stable, longer, 5) == HF_APPLIES_NOW);
        memcpy(&pointer, g_fakeStratagem + 0x40, 8); memcpy(&count, g_fakeStratagem + 0x48, 4);
        assert(pointer != (uint64_t)(uintptr_t)native && count == 5 && !memcmp((void*)(uintptr_t)pointer, longer, sizeof longer));
        assert(hellforge_lua_set_code(second, stable, shorter, 2) == HF_APPLIES_NOW); /* a later Lua mod's replaces it */
        memcpy(&count, g_fakeStratagem + 0x48, 4);
        assert(count == 2 && ((uint32_t*)(uintptr_t)pointer)[0] == 2 && ((uint32_t*)(uintptr_t)pointer)[2] == 0);
        assert(hellforge_lua_set_code(lua, stable, bad, 2) == 0 && hellforge_lua_set_code(lua, stable, longer, 0) == 0);
        assert(hellforge_lua_set_code(lua, 0x1234, shorter, 2) == 0 && hellforge_lua_set_code(code, stable, shorter, 2) == 0);
        assert(hellforge_lua_set_code(lua, stable, NULL, 0) == HF_APPLIES_NOW);
        memcpy(&pointer, g_fakeStratagem + 0x40, 8); memcpy(&count, g_fakeStratagem + 0x48, 4);
        assert(pointer == (uint64_t)(uintptr_t)native && count == 3 && g_luaCodeCount == 0);
        assert(hellforge_lua_set_code(lua, stable, NULL, 0) == HF_APPLIES_NOW); /* nothing changed: nothing to put back */
    }

    strcpy(editor->id, editorId);
    for (int i = mods; i < g_modCount; i++) memset(&g_mods[i], 0, sizeof g_mods[i]);
    g_modCount = mods;
    for (int i = 0; i < g_fakeRecordCount; i++) free(g_fakeRecords[i].bytes);
    g_fakeRecordCount = g_componentCount = g_editCount = 0;
    g_fakeTablesReady = 0;
    g_fieldRecord = ComponentRecordOf;
    g_tableRow = TableRowOf;
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 0);
}

static void PassivesFixture(void)
{
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, 0x3500000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* manager = (uint8_t*)calloc(1, 0x1000);
    uint8_t* players = (uint8_t*)calloc(1, 0x400);
    uint8_t* user = (uint8_t*)calloc(1, PSV_LOCAL_PEER + 16);
    uint8_t* kits[3];
    uintptr_t kitTable[3];
    uint32_t kitIds[3] = {0x111, 0x222, 0x333}, kitPassives[3] = {2, 6, 0}, kitTypes[3] = {0, 0, 1};
    uint8_t* descriptor = (uint8_t*)calloc(1, 16);
    uint8_t* playerDescriptor = (uint8_t*)calloc(1, 16);
    uint32_t entries[4] = {0xFFFFFFFFu, 0, 77, 0}; /* entity 77 in its bucket: 77 x 1 mod 2 */
    assert(game && manager && players && user && descriptor && playerDescriptor);
    for (size_t i = 0; i < sizeof kPassivePins / sizeof kPassivePins[0]; i++) memcpy(game + kPassivePins[i].rva, kPassivePins[i].bytes, kPassivePins[i].size);
    uintptr_t at;
    at = (uintptr_t)manager; memcpy(game + PSV_MANAGER, &at, 8);
    at = (uintptr_t)players; memcpy(game + PSV_PLAYERS, &at, 8);
    at = (uintptr_t)user; memcpy(game + PSV_LOCAL_USER, &at, 8);
    for (int i = 0; i < 3; i++)
    {
        kits[i] = (uint8_t*)calloc(1, 48);
        memcpy(kits[i], &kitIds[i], 4); memcpy(kits[i] + 28, &kitPassives[i], 4); memcpy(kits[i] + 40, &kitTypes[i], 4);
        kitTable[i] = (uintptr_t)kits[i];
    }
    at = (uintptr_t)kitTable; memcpy(manager + PSV_KITS, &at, 8);
    uint32_t value = 3; memcpy(manager + PSV_KIT_COUNT, &value, 4);
    value = 32; memcpy(manager + PSV_PASSIVE_COUNT, &value, 4);
    for (int32_t id = 0; id < (int32_t)PSV_PASSIVE_IDS; id++) { int32_t index = id < 32 ? id : -1; memcpy(manager + PSV_PASSIVE_INDEX + id * 4, &index, 4); }
    value = 1; memcpy(manager + PSV_RECORD_COUNT, &value, 4);
    uint8_t map[20] = {0};
    at = (uintptr_t)entries; memcpy(map, &at, 8);
    value = 2; memcpy(map + 8, &value, 4);
    value = 0xFFFFFFFFu; memcpy(map + 12, &value, 4);
    value = 1; memcpy(map + 16, &value, 4);
    memcpy(manager + PSV_MAP, map, sizeof map);
    value = 77; memcpy(descriptor + 8, &value, 4); memcpy(playerDescriptor + 8, &value, 4);
    at = (uintptr_t)descriptor; memcpy(manager + PSV_DESCRIPTORS, &at, 8);
    at = (uintptr_t)playerDescriptor; memcpy(players + PSV_PLAYER_DESCRIPTORS, &at, 8);
    value = 1; memcpy(players + PSV_PLAYER_COUNT, &value, 4);
    memcpy(players + PSV_PLAYER_PEERS, "PEER0001", 8);
    memcpy(user + PSV_LOCAL_PEER, "PEER0001", 8);
    uint8_t* record = manager + PSV_APPLIED;
    uint32_t* armorKit = (uint32_t*)(record + PSV_ARMOR_KIT), *armor = (uint32_t*)(record + PSV_ARMOR), *second = (uint32_t*)(record + PSV_SECOND);
    *(uint32_t*)(record + PSV_HELMET_KIT) = 0x333;
    *armorKit = 0x111; *armor = 2; *second = 0;
    HfMod* mod = &g_mods[0];
    uintptr_t g = (uintptr_t)game;
    g_passivesEvery = 0;
    g_passivesGame = g;
    HfPassives state;

    assert(ApiGetPassives(mod, &state) == 1 && state.armor == 2 && state.kit_armor == 2 && state.second == 0 && state.players == 1 && !state.held);
    assert(ApiSetPassives(mod, 50, HF_PASSIVE_KIT) == 0); /* no such passive */
    assert(ApiSetPassives(mod, 7, HF_PASSIVE_KIT) == 1);
    PassivesTick(g);
    assert(*armor == 7 && *second == 0 && !strcmp(g_passivesStatus, "held"));
    /* a kit change: the game derives the new kit's passive; the override is written again */
    *armorKit = 0x222; *armor = 6;
    PassivesTick(g);
    assert(*armor == 7);
    /* a value neither the kits nor HellForge put there is left alone */
    *armor = 9;
    PassivesTick(g);
    assert(*armor == 9 && strstr(g_passivesStatus, "neither"));
    *armor = 7;
    /* a second player: the kits' values come back, and the override waits */
    value = 2; memcpy(players + PSV_PLAYER_COUNT, &value, 4);
    PassivesTick(g);
    assert(*armor == 6 && strstr(g_passivesStatus, "solo only"));
    value = 1; memcpy(players + PSV_PLAYER_COUNT, &value, 4);
    PassivesTick(g);
    assert(*armor == 7);
    /* a passive that needs a package is not given (the kits' value instead); the second slot; the armor-only one */
    assert(ApiSetPassives(mod, 17, 13) == 1);
    PassivesTick(g);
    assert(*armor == 6 && *second == 13 && strstr(g_passivesStatus, "package"));
    assert(ApiGetPassives(mod, &state) == 1 && state.armor == 6 && state.second == 13 && state.kit_second == 0 && state.held &&
        state.want_armor == 17 && state.want_second == 13);
    /* released: what the kits give, everywhere */
    assert(ApiSetPassives(mod, HF_PASSIVE_KIT, HF_PASSIVE_KIT) == 1);
    PassivesTick(g);
    assert(*armor == 6 && *second == 0 && !g_passivesWrote[0] && !g_passivesWrote[1]);
    /* other code where the pins are: nothing is written */
    g_passivesProven = 0;
    game[kPassivePins[0].rva] ^= 0xFF;
    assert(ApiSetPassives(mod, 7, HF_PASSIVE_KIT) == 1);
    PassivesTick(g);
    assert(*armor == 6 && strstr(g_passivesStatus, "not the reviewed one"));
    assert(ApiSetPassives(mod, HF_PASSIVE_KIT, HF_PASSIVE_KIT) == 1);
    g_passivesProven = 0;
    g_passivesGame = 0;
    g_passivesEvery = 250;
    VirtualFree(game, 0, MEM_RELEASE);
}

/* References and packages (refs.c, packages.c): an explosion swap waits for its package (requested on the game thread,
 * written once resident), unpackaged and empty values apply at once, unknown types, a shrapnel loop and a status no
 * damage can apply are refused, a donor projectile's package timing out fails its edit, the stats pass's reference
 * waits and is written by StatsRefsTick, load_package. */
static uint8_t g_refRows[4][272];   /* projectile 40 (self), the donor, explosion rows, damage 149 */
static uint8_t g_refExplosions[4][152];
static uint32_t g_refExplosionTypes[4];
static uint64_t g_refResident[8];
static int g_refResidentCount;
static uint64_t g_refRequested[8];
static int g_refRequestedCount;

static uintptr_t RefTableRow(const char* component, uint64_t id, int* ready)
{
    *ready = 1;
    if (!strcmp(component, "ProjectileSettings")) return id == 40 ? (uintptr_t)g_refRows[0] : id == kRefProjectiles[0].type ? (uintptr_t)g_refRows[1] : 0;
    if (!strcmp(component, "DamageInfo")) return id == 149 ? (uintptr_t)g_refRows[3] : 0;
    if (!strcmp(component, "ExplosionSettings"))
        for (int i = 0; i < 4; i++) if (g_refExplosionTypes[i] == id) return (uintptr_t)g_refExplosions[i];
    return 0;
}

static uint64_t RefPackageState(uint64_t id)
{
    for (int i = 0; i < g_refResidentCount; i++) if (g_refResident[i] == id) return PKG_RESIDENT;
    return PKG_WAITING;
}

static void RefRequest(uintptr_t system, const uint64_t* ids, uint32_t count)
{
    assert(count == 1 && system != 0);
    g_refRequested[g_refRequestedCount++] = ids[0];
}

static void RefsFixture(void)
{
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, PKG_SYSTEM + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* exe = (uint8_t*)VirtualAlloc(NULL, PKG_EXE_FIND + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* system = (uint8_t*)calloc(1, 64);
    uint8_t* map = (uint8_t*)calloc(1024, PKG_MAP_STRIDE);
    assert(game && exe && system && map);
    memcpy(game + PKG_REQUEST, kPackageRequestPin, sizeof kPackageRequestPin);
    memcpy(exe + PKG_EXE_HAS_LOADED, kPackageHasLoadedPin, sizeof kPackageHasLoadedPin);
    memcpy(exe + PKG_EXE_FIND, kPackageFindPin, sizeof kPackageFindPin);
    uintptr_t at = (uintptr_t)system; memcpy(game + PKG_SYSTEM, &at, 8);
    at = (uintptr_t)map; memcpy(system + PKG_MAP_ENTRIES_AT, &at, 8);
    uint32_t capacity = 1024; memcpy(system + PKG_MAP_CAPACITY_AT, &capacity, 4);
    g_packagesGame = (uintptr_t)game; g_packagesExe = (uintptr_t)exe;
    g_packageState = RefPackageState; g_packageRequest = RefRequest;
    g_tickState = 1;
    g_tableRow = RefTableRow;
    g_fieldRecord = FakeFieldRecord;
    static const struct { const char* name; uint32_t index, stride; } tables[] = {{"ProjectileWeaponComponentData", 321, 616}};
    g_componentCount = 0;
    Component* c = &g_components[g_componentCount++];
    memset(c, 0, sizeof *c);
    strcpy(c->name, tables[0].name); c->index = tables[0].index; c->stride = tables[0].stride;
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 1);
    HfMod* mod = &g_mods[0];

    /* The catalogue's explosions: one with a package, one without, one with a shrapnel projectile that is the shot it is
     * set on, and a type the catalogue does not know. */
    const RefRow *packaged = NULL, *plain = NULL, *looping = NULL;
    for (size_t i = 0; i < sizeof kRefExplosions / sizeof kRefExplosions[0]; i++)
    {
        const RefRow* r = &kRefExplosions[i];
        if (!packaged && r->package && !r->package2) packaged = r;
        else if (!plain && !r->package && !r->mission) plain = r;
        else if (!looping && !r->package && !r->mission && r != plain) looping = r;
    }
    assert(packaged && plain && looping);
    g_refExplosionTypes[0] = packaged->type; g_refExplosionTypes[1] = plain->type; g_refExplosionTypes[2] = looping->type;
    g_refExplosionTypes[3] = 9999;
    uint32_t one = 1, self = 40;
    memcpy(g_refExplosions[2] + REF_SHRAPNEL_COUNT, &one, 4); memcpy(g_refExplosions[2] + REF_SHRAPNEL_PROJECTILE, &self, 4);
    uint64_t packageId = kRefPackages[packaged->package - 1].id;

    HfField impact = HF_FIELD(ProjectileSettings, impact_explosion), expiry = HF_FIELD(ProjectileSettings, expiry_explosion);
    HfValue v = HF_U32(packaged->type);
    uint32_t read;
    /* waits for the package: asked for, requested on the game thread, written once resident */
    assert(ApiSetLive(mod, 40, &impact, &v, HF_LIVE_SHARED) == HF_APPLIES_NOW && g_editCount == 1 && g_edits[0].waiting && g_editsWaiting);
    memcpy(&read, g_refRows[0] + REF_IMPACT, 4); assert(read == 0);
    assert(g_heldCount == 1 && g_held[0].id == packageId && !g_held[0].requested && g_refRequestedCount == 0);
    PackagesRequest();
    assert(g_held[0].requested && g_refRequestedCount == 1 && g_refRequested[0] == packageId);
    ApplyWaitingEdits();
    memcpy(&read, g_refRows[0] + REF_IMPACT, 4); assert(read == 0 && g_edits[0].waiting);
    g_refResident[g_refResidentCount++] = packageId;
    ApplyWaitingEdits();
    memcpy(&read, g_refRows[0] + REF_IMPACT, 4); assert(read == packaged->type && !g_edits[0].waiting);
    PackagesRequest(); assert(g_refRequestedCount == 1); /* once per package */
    /* an unpackaged one and none apply at once; an unknown type and a shot that spawns itself are refused */
    v = (HfValue)HF_U32(plain->type);
    assert(ApiSetLive(mod, 40, &expiry, &v, HF_LIVE_SHARED) == HF_APPLIES_NOW);
    memcpy(&read, g_refRows[0] + REF_EXPIRY, 4); assert(read == plain->type);
    v = (HfValue)HF_U32(0);
    assert(ApiSetLive(mod, 40, &expiry, &v, HF_LIVE_SHARED) == HF_APPLIES_NOW);
    memcpy(&read, g_refRows[0] + REF_EXPIRY, 4); assert(read == 0);
    v = (HfValue)HF_U32(9999);
    assert(ApiSetLive(mod, 40, &expiry, &v, HF_LIVE_SHARED) == 0);
    v = (HfValue)HF_U32(looping->type);
    assert(ApiSetLive(mod, 40, &expiry, &v, HF_LIVE_SHARED) == 0);
    memcpy(&read, g_refRows[0] + REF_EXPIRY, 4); assert(read == 0);
    v = (HfValue)HF_U32(12345);
    assert(ApiSetLive(mod, 40, &expiry, &v, HF_LIVE_SHARED) == 0); /* no such row */

    /* status slots: an attachable status, one no damage can apply, a slot after an empty one (written, logged) */
    HfField status1 = HF_FIELD(DamageInfo, status_1_type), status3 = HF_FIELD(DamageInfo, status_3_type);
    v = (HfValue)HF_U32(kRefStatuses[0]);
    assert(ApiSetLive(mod, 149, &status1, &v, HF_LIVE_SHARED) == HF_APPLIES_NOW);
    memcpy(&read, g_refRows[3] + REF_STATUS_FIRST, 4); assert(read == kRefStatuses[0]);
    v = (HfValue)HF_U32(24); /* pure damage: not attachable */
    assert(ApiSetLive(mod, 149, &status1, &v, HF_LIVE_SHARED) == 0);
    v = (HfValue)HF_U32(kRefStatuses[1]);
    assert(ApiSetLive(mod, 149, &status3, &v, HF_LIVE_SHARED) == HF_APPLIES_NOW);
    memcpy(&read, g_refRows[3] + REF_STATUS_FIRST + 16, 4); assert(read == kRefStatuses[1]);
    assert(RefStatusGap((uintptr_t)g_refRows[3], REF_STATUS_FIRST + 16, kRefStatuses[1]) && !RefStatusGap((uintptr_t)g_refRows[3], REF_STATUS_FIRST + 8, 5));

    /* a weapon's projectile: the donor's package times out, so the edit fails; back at its original needs nothing */
    uint8_t* weapon = FakeRecordOf("ProjectileWeaponComponentData", 0x77, 616, 0);
    uint32_t own = 40; memcpy(weapon, &own, 4);
    HfField projectile = HF_FIELD(ProjectileWeapon, projectile);
    v = (HfValue)HF_U32(kRefProjectiles[0].type);
    int edits = g_editCount;
    assert(ApiSetLive(mod, 0x77, &projectile, &v, 0) == HF_APPLIES_RESPAWN && g_editCount == edits + 1 && g_edits[edits].waiting);
    PackagesRequest();
    for (int i = 0; i < g_heldCount; i++) g_held[i].since -= PKG_TIMEOUT_MS + 1;
    ApplyWaitingEdits();
    memcpy(&read, weapon, 4); assert(read == 40 && g_edits[edits].failed && !g_edits[edits].waiting);
    v = (HfValue)HF_U32(40);
    assert(ApiSetLive(mod, 0x77, &projectile, &v, 0) == HF_APPLIES_RESPAWN);

    /* the stats pass: a reference waits for its package and StatsRefsTick writes it */
    uint32_t zero = 0; memcpy(g_refRows[0] + REF_IMPACT, &zero, 4);
    g_held[0].requested = 1; g_held[0].failed = 0; g_held[0].since = GetTickCount64(); /* the explosion's package, loading again */
    StatEdit* e = &g_stats[0];
    memset(e, 0, sizeof *e);
    e->mod = mod; strcpy(e->kind, "ProjectileSettings"); strcpy(e->label, "projectiles/40 terminal.primary.impact.explosion");
    e->record = 40; e->offset = REF_IMPACT; e->value = packaged->type; e->ref = -1; e->shared = 1;
    g_statCount = 1;
    g_refResidentCount = 0;
    StatsWrite(e, (uintptr_t)g_refRows[0] + REF_IMPACT);
    memcpy(&read, g_refRows[0] + REF_IMPACT, 4); assert(read == 0 && e->refWaiting && e->ref == REF_EXPLOSION && StatsRefsWaiting());
    g_refResident[g_refResidentCount++] = packageId;
    StatsRefsTick();
    memcpy(&read, g_refRows[0] + REF_IMPACT, 4); assert(read == packaged->type && !e->refWaiting && e->applied);
    /* the reference kinds of stats edits: by component, by table and through an entity delta */
    StatEdit probe = {0};
    strcpy(probe.kind, "component"); strcpy(probe.component, "ProjectileWeaponComponentData"); probe.offset = 0;
    assert(StatRefKind(&probe) == REF_PROJECTILE);
    strcpy(probe.kind, "EntityDelta"); strcpy(probe.component, "321:0"); probe.offset = 162676;
    assert(StatRefKind(&probe) == REF_PROJECTILE);
    strcpy(probe.kind, "DamageInfo"); probe.offset = 60;
    assert(StatRefKind(&probe) == REF_STATUS);
    strcpy(probe.kind, "ProjectileSettings"); probe.offset = 32;
    assert(StatRefKind(&probe) == REF_NONE);

    /* load_package: catalogue packages only */
    assert(ApiLoadPackage(mod, 0x1234) == 0);
    assert(ApiLoadPackage(mod, packageId) == HF_PACKAGE_RESIDENT);

    /* Co-op assets: this machine's own packages go to the session's backend ("packages"); the lobby's come back
     * ("preload") and are asked for as friends', which neither count as this machine's own nor take its last 16. */
    char list[PKG_MAX_HELD * 17 + 2], expect[64], line[4096], reply[512];
    snprintf(expect, sizeof expect, "%016llx", (unsigned long long)packageId);
    PackagesOwnList(list, sizeof list); assert(!strcmp(list, expect));
    uint64_t friendId = 0;
    for (size_t i = 0; i < sizeof kRefPackages / sizeof kRefPackages[0] && !friendId; i++) if (kRefPackages[i].id != packageId) friendId = kRefPackages[i].id;
    InterlockedExchange(&g_packagesDirty, 0);
    int held = g_heldCount;
    snprintf(line, sizeof line, "preload\t21\t%016llx,%016llx", (unsigned long long)friendId, (unsigned long long)packageId);
    LiveRequest(line, reply, sizeof reply);
    assert(!strcmp(reply, "21\tok\tnow") && g_heldCount == held + 1 && !g_held[held].own && g_held[held].id == friendId && !g_packagesDirty);
    PackagesOwnList(list, sizeof list); assert(!strcmp(list, expect)); /* a friend's is not sent on */
    PackagesRequest(); assert(g_held[held].requested); /* requested on the game thread as this machine's own are */
    assert(ApiLoadPackage(mod, friendId) == HF_PACKAGE_LOADING && g_held[held].own && g_packagesDirty); /* now this machine's too */
    PackagesOwnList(list, sizeof list); assert(strstr(list, expect) && strchr(list, ','));
    strcpy(line, "preload\t22\t-"); LiveRequest(line, reply, sizeof reply); assert(!strcmp(reply, "22\tok\tnow"));
    strcpy(line, "preload\t23\tnot-hex-at-all!!"); LiveRequest(line, reply, sizeof reply); assert(!strncmp(reply, "23\trefused\t", 11));
    snprintf(line, sizeof line, "preload\t24\t+%015llx", (unsigned long long)friendId); LiveRequest(line, reply, sizeof reply);
    assert(!strncmp(reply, "24\trefused\t", 11));
    /* friends' packages stop 16 short of the 64; this machine's own still get them */
    size_t next = 0;
    for (int n = 0; n < PKG_MAX_HELD; n++)
    {
        while (next < sizeof kRefPackages / sizeof kRefPackages[0] && (kRefPackages[next].id == packageId || kRefPackages[next].id == friendId)) next++;
        snprintf(line, sizeof line, "preload\t25\t%016llx", (unsigned long long)kRefPackages[next++].id);
        LiveRequest(line, reply, sizeof reply);
    }
    assert(g_heldCount == PKG_MAX_HELD - PKG_OWN_RESERVE);
    while (kRefPackages[next].id == packageId || kRefPackages[next].id == friendId) next++;
    assert(ApiLoadPackage(mod, kRefPackages[next].id) == HF_PACKAGE_LOADING && g_heldCount == PKG_MAX_HELD - PKG_OWN_RESERVE + 1);

    g_statCount = 0;
    for (int i = 0; i < g_fakeRecordCount; i++) free(g_fakeRecords[i].bytes);
    g_fakeRecordCount = g_componentCount = g_editCount = 0;
    g_editsWaiting = 0;
    g_heldCount = 0; g_packagesProven = 0; g_tickState = 0;
    g_packagesGame = g_packagesExe = 0; g_packageState = NULL; g_packageRequest = NULL;
    g_fieldRecord = ComponentRecordOf;
    g_tableRow = TableRowOf;
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 0);
    free(map); free(system);
    VirtualFree(game, 0, MEM_RELEASE); VirtualFree(exe, 0, MEM_RELEASE);
}

/* Armor perk values (perks.c): a passive's entries found through the manager's passive pointers and index, checked
 * (row identity, entry key and type or stat id, the code pins), written live and by the stats pass, refused when the
 * row or an entry is not as reviewed. */
static void PerksFixture(void)
{
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, 0x3500000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* manager = (uint8_t*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* data = (uint8_t*)VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(game && manager && data);
    for (size_t i = 0; i < sizeof kPerkPins / sizeof kPerkPins[0]; i++) memcpy(game + kPerkPins[i].rva, kPerkPins[i].bytes, kPerkPins[i].size);
    uintptr_t at = (uintptr_t)manager; memcpy(game + PERK_MANAGER, &at, 8);
    uint64_t* pointers = (uint64_t*)data;                 /* the rows' pointers */
    uint8_t* rows = data + 0x400;                         /* 32 rows of 0x34 */
    uint8_t* items = data + 0x1000;                       /* their arrays */
    at = (uintptr_t)pointers; memcpy(manager + PERK_PASSIVES, &at, 8);
    uint32_t count[2] = {PERK_COUNT, 0}; memcpy(manager + PERK_PASSIVES + 8, count, 8);
    uint32_t* index = (uint32_t*)(manager + PERK_INDEX);
    for (uint32_t i = 0; i < PERK_IDS; i++) index[i] = 0xFFFFFFFFu;
    size_t next = 0;
    for (uint32_t slot = 0; slot < PERK_COUNT; slot++)
    {
        const PerkEntry* e = &kPerkEntries[slot];
        uint8_t* row = rows + slot * PERK_ROW;
        pointers[slot] = (uintptr_t)row;
        index[e->id] = slot;
        memcpy(row, &e->id, 4); memcpy(row + 4, &e->nameId, 4); memcpy(row + 8, &e->icon, 8); memcpy(row + 0x30, &e->package, 4);
        uint32_t m = e->modifiers, s = e->stats;
        at = (uintptr_t)(items + next); memcpy(row + 0x10, &at, 8); memcpy(row + 0x18, &m, 4);
        for (uint32_t k = 0; k < m; k++)
        {
            memcpy(items + next, kPerkItems[e->first + k], 8);
            float v = 1.0f; memcpy(items + next + 8, &v, 4);
            next += 16;
        }
        at = (uintptr_t)(items + next); memcpy(row + 0x20, &at, 8); memcpy(row + 0x28, &s, 4);
        for (uint32_t k = 0; k < s; k++)
        {
            memcpy(items + next, kPerkItems[e->first + m + k], 4);
            float add = 0.0f, mul = 1.0f; memcpy(items + next + 4, &add, 4); memcpy(items + next + 8, &mul, 4);
            next += 12;
        }
    }
    g_perksGame = (uintptr_t)game;
    g_perksProven = 0;
    HfMod* mod = &g_mods[0];
    int ready;
    /* Scout (2): modifier 1 is the enemy detection range; Reinforced Epaulettes (10): stat 0 is primary reload */
    uintptr_t scout = PerkRow("PassiveModifier", 2 * 16 + 1, &ready);
    uintptr_t reload = PerkRow("PassiveStat", 10 * 16 + 0, &ready);
    assert(scout && reload && ready && PerkRow("PassiveModifier", 2 * 16 + 2, &ready) == 0); /* Scout has two modifiers */
    HfField value = HF_FIELD(PassiveModifier, value);
    HfValue v = HF_F32(0.25);
    assert(ApiSetLive(mod, 2 * 16 + 1, &value, &v, HF_LIVE_SHARED) == HF_APPLIES_NOW);
    float read; memcpy(&read, (void*)(scout + 8), 4); assert(read == 0.25f);
    assert(ApiSetLive(mod, 2 * 16 + 1, &value, NULL, 0) == HF_APPLIES_NOW);
    memcpy(&read, (void*)(scout + 8), 4); assert(read == 1.0f);
    /* the stats pass: a stat's multiplier, written once the rows are readable */
    StatEdit* e = &g_stats[0];
    memset(e, 0, sizeof *e);
    e->mod = mod; strcpy(e->kind, "PassiveStat"); strcpy(e->label, "perks/Reinforced Epaulettes perk.stat_13.mul");
    e->record = 10 * 16; e->offset = 8; e->floating = 1; e->ref = -1; e->shared = 1;
    float mul = 1.5f; memcpy(&e->value, &mul, 4);
    g_statCount = 1; g_perksDone = 0;
    StatsPerksReady();
    memcpy(&read, (void*)(reload + 8), 4); assert(g_perksDone && e->applied && read == 1.5f);
    /* not as reviewed: an entry's key, a row's name id, the pins */
    uint32_t key; memcpy(&key, (void*)scout, 4);
    uint32_t wrong = key ^ 1; memcpy((void*)scout, &wrong, 4);
    assert(PerkRow("PassiveModifier", 2 * 16 + 1, &ready) == 0 && ready);
    memcpy((void*)scout, &key, 4);
    uint8_t* row = (uint8_t*)(uintptr_t)pointers[index[2]];
    row[4] ^= 1; assert(PerkRow("PassiveModifier", 2 * 16 + 1, &ready) == 0); row[4] ^= 1;
    assert(PerkRow("PassiveModifier", 2 * 16 + 1, &ready) == scout);
    g_perksProven = 0; game[kPerkPins[0].rva] ^= 0xFF;
    assert(PerkRow("PassiveModifier", 2 * 16 + 1, &ready) == 0 && !ready); /* waits: the code is not as reviewed (yet) */
    g_statCount = 0; g_editCount = 0; g_perksDone = 0; g_perksProven = 0; g_perksGame = 0;
    VirtualFree(game, 0, MEM_RELEASE); VirtualFree(manager, 0, MEM_RELEASE); VirtualFree(data, 0, MEM_RELEASE);
}

/* Arm swaps (refs.c RefMountCheck): a vehicle slot takes a weapon its catalogue allows (live, a u64), refuses another,
 * waits for a packaged weapon's package, and goes back to its vanilla occupant on reset. */
static uint64_t g_mountResident[4];
static uint64_t MountPackageState(uint64_t id)
{
    for (int i = 0; i < 4; i++) if (g_mountResident[i] == id) return PKG_RESIDENT;
    return PKG_WAITING;
}

/* A component table as the entity data has it: 28-byte header (its size after the header at -12), index rows, records. */
static uint8_t* FakeTable(uint32_t component, uint32_t bytes)
{
    uint8_t* block = calloc(1, 32 + (size_t)bytes + 16);
    memcpy(block, &component, 4);
    memcpy(block + 20, &bytes, 4);
    return block + 32;
}

static void FakeRow(uint8_t* table, uint32_t slots, uint64_t entity, uint32_t record)
{
    uint8_t* row = FreeRow(table, slots, 16, entity);
    memcpy(row, &entity, 8);
    memcpy(row + 8, &record, 4);
}

/* own_all: the copy gets a record of its own, equal to its base's, in every component whose record size is certain. */
static void OwnAllFixture(void)
{
    uintptr_t was = g_game;
    g_weaponCount = g_ownedCount = g_componentCount = 0; g_loadoutTable = NULL;
    const uint64_t base = 0x37CDE43876BA26BBull, other = 0x1234;
    uint8_t* game = calloc(1, REG_STATE_GLOBAL + 8);
    uint8_t* state = calloc(1, REG_LIST + 8);
    g_game = (uintptr_t)game; memcpy(game + REG_STATE_GLOBAL, &state, 8);
    /* 284 by its shape; 70 fits two shapes (9 x 40, 10 x 36: the rows reach 10 records); 44 fits none; 321 the profile's */
    static const struct { uint32_t c, slots, records, size, bytes, baseRecord; } tables[] = {
        {284, 2008, 1005, 12, 2008 * 16 + 44192 - 32128, 7}, {70, 18, 10, 36, 18 * 16 + 360, 9}, {44, 14, 0, 0, 14 * 16 + 1000, 0},
        {321, 542, 272, 616, 542 * 16 + 272 * 616, 3}, {REG_LOADOUT, REG_LOADOUT_SLOTS, REG_LOADOUT_RECORDS, REG_LOADOUT_STRIDE,
        REG_LOADOUT_SLOTS * 16 + REG_LOADOUT_RECORDS * REG_LOADOUT_STRIDE, 0}};
    uint8_t* original[5];
    for (int i = 0; i < 5; i++)
    {
        uint8_t* t = original[i] = FakeTable(tables[i].c, tables[i].bytes);
        SetStatePointer(state, REG_TABLES + 8 * tables[i].c, t);
        if (tables[i].c == REG_LOADOUT) continue;
        FakeRow(t, tables[i].slots, base, tables[i].baseRecord);
        FakeRow(t, tables[i].slots, other, 0);
        for (uint32_t b = 0; b < tables[i].size; b++) t[tables[i].slots * 16 + tables[i].baseRecord * tables[i].size + b] = (uint8_t)(b + i);
    }
    Component* profile = &g_components[g_componentCount++];
    memset(profile, 0, sizeof *profile);
    strcpy(profile->name, "ProjectileWeaponComponentData"); profile->index = 321; profile->stride = 616; profile->records = 272;
    profile->recordOffset = 542 * 16; profile->indices = 542;
    assert(ComponentStride(state, 284) == 12 && ComponentStride(state, 70) == 36 && ComponentStride(state, 44) == 0 && ComponentStride(state, 321) == 616);
    memcpy(FindRow(original[1], 18, 16, base) + 8, &(uint32_t){8}, 4);
    assert(ComponentStride(state, 70) == 36); /* by its index: rows reaching 9 records (9 x 40 fits too) do not mislead it */
    memcpy(FindRow(original[1], 18, 16, base) + 8, &(uint32_t){9}, 4);
    /* The WeaponLinker table: four 12-byte records, three named (three of 16 fit its bytes too). */
    uint8_t* linker = FakeTable(114, 6 * 16 + 4 * 12);
    SetStatePointer(state, REG_TABLES + 8 * 114, linker);
    for (uint32_t r = 0; r < 3; r++) FakeRow(linker, 6, 0x100 + r, r);
    assert(ComponentStride(state, 114) == 12);
    SetStatePointer(state, REG_TABLES + 8 * 114, NULL);

    static uint16_t components[] = {REG_LOADOUT, 284, 70, 44, 321};
    uint8_t* map = calloc(REG_MAP_SLOTS, 32);
    uint8_t* row = FreeRow(map, REG_MAP_SLOTS, 32, base);
    uint16_t* list = (uint16_t*)components;
    uint64_t count = 5;
    uint32_t shortId = 77, mapIndex = (uint32_t)((row - map) / 32), listBytes = 8;
    memcpy(row, &base, 8); memcpy(row + 8, &list, 8); memcpy(row + 16, &count, 8); memcpy(row + 24, &shortId, 4);
    uint8_t* shortIds = calloc(1, 32 + 8);
    memcpy(shortIds + 20, &listBytes, 4); memcpy(shortIds + 32, &shortId, 4); memcpy(shortIds + 36, &mapIndex, 4);
    SetStatePointer(state, REG_MAP, map);
    SetStatePointer(state, REG_LIST, shortIds + 32);

    HfEntity e = {sizeof e};
    e.base = base; e.name = "fixture/hmg-sentry"; e.own_all = 1;
    uint64_t hmg = ApiAddEntity(&g_mods[0], &e);
    assert(hmg == Murmur64A("fixture/hmg-sentry", 18) && g_weapons[0].ownAll);
    RegisterWeapons();
    assert(g_weapons[0].state == 1 && FindRow(map, REG_MAP_SLOTS, 32, hmg) != NULL);
    for (int i = 0; i < 4; i++)
    {
        uint32_t slots = tables[i].slots, size = tables[i].size, mine, theirs;
        uint8_t* t = StatePointer(state, REG_TABLES + 8 * tables[i].c);
        memcpy(&mine, FindRow(t, slots, 16, hmg) + 8, 4);
        memcpy(&theirs, FindRow(t, slots, 16, base) + 8, 4);
        assert(theirs == tables[i].baseRecord);
        if (tables[i].c == 44) { assert(t == original[i] && mine == theirs); continue; } /* record size unknown: shared */
        assert(t != original[i] && mine >= tables[i].records && mine != theirs);
        uint8_t* records = t + (size_t)slots * 16;
        assert(!memcmp(records + (size_t)mine * size, records + (size_t)theirs * size, size));
        records[(size_t)theirs * size] ^= 0xFF; /* the base changes; the copy does not */
        assert(records[(size_t)mine * size] != records[(size_t)theirs * size]);
    }
    /* content lists it with its base; a caller's older, smaller struct gets no more than it holds */
    {
        HfContent c = {sizeof c};
        int found = 0;
        for (uint32_t i = 0; ApiContent(&g_mods[0], i, &c); i++, c.size = sizeof c)
            if (c.kind == HF_KIND_ENTITY && c.id == hmg) found = c.base == base && !strcmp(c.name, "fixture/hmg-sentry") && !strcmp(c.mod, g_mods[0].id);
        assert(found);
        uint8_t small[offsetof(HfContent, designation) + 8];
        memset(small, 0xEE, sizeof small);
        ((HfContent*)small)->size = offsetof(HfContent, designation);
        assert(ApiContent(&g_mods[0], 0, (HfContent*)small) && small[offsetof(HfContent, designation)] == 0xEE);
    }
    /* Asking again gives the record it has; nothing more is allocated. */
    uint32_t next = g_owned[0].next;
    uint8_t* again = OwnRecord(state, g_owned[0].component, g_owned[0].stride, base, hmg);
    assert(again == OwnRecord(state, g_owned[0].component, g_owned[0].stride, base, hmg) && g_owned[0].next == next);
    /* A definition block asks for it with its size set; SDK 6's blocks (size 0) have no own_all. */
    static const HfEntityDef whole = {.size = sizeof(HfEntityDef), .name = "fixture/whole", .base = 0x37CDE43876BA26BBull, .own_all = 1};
    static const HfEntityDef older = {.name = "fixture/older", .base = 0x37CDE43876BA26BBull};
    assert(ApiRegisterEntity(&g_mods[0], &whole) && g_weapons[g_weaponCount - 1].ownAll);
    assert(ApiRegisterEntity(&g_mods[0], &older) && !g_weapons[g_weaponCount - 1].ownAll);
    g_weaponCount = g_ownedCount = g_componentCount = 0; g_loadoutTable = NULL; g_game = was;
}

/* Game events: listeners by priority, the queue, and the sources against synthetic game memory and fake originals. */
static char g_heard[64];
static int g_heardCount;
static HfDamageEvent g_lastDamage;
static HfPlayerEvent g_lastPlayer;
static void Heard(HfMod* mod, const HfEventInfo* event, void* user)
{
    if (g_heardCount < 63) g_heard[g_heardCount++] = *(const char*)user;
    if (event->kind == HF_ON_DAMAGE) g_lastDamage = *(const HfDamageEvent*)event;
    if (event->kind == HF_ON_PLAYER_SPAWN || event->kind == HF_ON_PLAYER_DOWN) g_lastPlayer = *(const HfPlayerEvent*)event;
}
static uint8_t* g_consumed;
static uint64_t FakeConsume(void* system, uint8_t* event, uint64_t r8, uint64_t r9, uint64_t s5, uint64_t s6)
{
    g_consumed = event;
    return r9 + 1;
}

static void EventsFixture(void)
{
    HfMod* mod = &g_mods[0];
    g_listenerCount = 0; g_listening = 0; g_heardCount = 0;
    assert(!ApiSubscribe(mod, 0, Heard, "x", 0) && !ApiSubscribe(mod, 99, Heard, "x", 0));
    assert(ApiSubscribe(mod, HF_ON_STATE, Heard, "b", 0) && ApiSubscribe(mod, HF_ON_STATE, Heard, "a", 10) &&
        ApiSubscribe(mod, HF_ON_STATE, Heard, "c", 0) && ApiSubscribe(mod, HF_ON_MISSION_START, Heard, "m", 0) &&
        ApiSubscribe(mod, HF_ON_MISSION_END, Heard, "e", 0) && ApiSubscribe(mod, HF_ON_DAMAGE, Heard, "d", 0) &&
        ApiSubscribe(mod, HF_ON_PLAYER_SPAWN, Heard, "s", 0) && ApiSubscribe(mod, HF_ON_PLAYER_DOWN, Heard, "x", 0));
    assert(AnyListener(HF_ON_STATE) && !AnyListener(HF_ON_KILL));

    /* The state: polled each update; higher priority first, equal ones in subscription order; mission start and end. */
    uint8_t* game = calloc(1, 0x3500000);
    uint8_t* gameState = calloc(1, 0xAC220);
    uint8_t* players = calloc(1, 0x430);
    uint8_t* peers = calloc(1, 0xB3A0);
    uint8_t* player = calloc(1, 16);
    memcpy(game + EVT_STATE_GAME, &gameState, 8); memcpy(game + EVT_PLAYERS, &players, 8); memcpy(game + EVT_LOCAL_PEER, &peers, 8);
    g_eventsGame = (uintptr_t)game;
    *(uint32_t*)(gameState + EVT_STATE_AT) = HF_STATE_SHIP;
    EventsTick();
    assert(g_heardCount == 3 && !memcmp(g_heard, "abc", 3));
    EventsTick(); assert(g_heardCount == 3); /* unchanged: nothing */
    *(uint32_t*)(gameState + EVT_STATE_AT) = HF_STATE_MISSION; EventsTick();
    *(uint32_t*)(gameState + EVT_STATE_AT) = HF_STATE_PREPARE_SHIP; EventsTick();
    assert(!memcmp(g_heard + 3, "abcm", 4) && !memcmp(g_heard + 7, "abce", 4) && g_heardCount == 11);

    /* Players: told apart by peer id; spawned (to 3), down (3 to 2), the local one marked. */
    g_heardCount = 0;
    *(uint32_t*)(players + 0x84) = 2;
    *(uint64_t*)(players + 0x2C8) = 0x1111; *(uint32_t*)(players + 0x2E0) = 3;
    *(uint64_t*)(players + 0x2C8 + 0x38) = 0x2222; *(uint32_t*)(players + 0x2E0 + 0x38) = 2;
    *(uint8_t**)(players + 0xE8) = player; *(uint32_t*)(player + 8) = 0x400123;
    *(uint64_t*)(peers + EVT_LOCAL_PEER_AT) = 0x1111;
    EventsTick();
    assert(g_heardCount == 1 && g_heard[0] == 's' && g_lastPlayer.peer == 0x1111 && g_lastPlayer.local && g_lastPlayer.entity == 0x400123);
    *(uint32_t*)(players + 0x2E0) = 2; EventsTick();
    assert(g_heardCount == 2 && g_heard[1] == 'x' && g_lastPlayer.peer == 0x1111);
    /* The first player leaves: the second moves to slot 0 and is not taken for the first. */
    *(uint32_t*)(players + 0x84) = 1; *(uint64_t*)(players + 0x2C8) = 0x2222; *(uint32_t*)(players + 0x2E0) = 3;
    *(uint8_t**)(players + 0xE8) = NULL;
    EventsTick();
    assert(g_heardCount == 3 && g_heard[2] == 's' && g_lastPlayer.peer == 0x2222 && !g_lastPlayer.local && g_playerCount == 1);

    /* Damage: read from the game's event, passed on unchanged with every argument; off the game thread it waits. */
    uint8_t event[0x70] = {0};
    uint32_t target = 0x400777, kind = 4; int32_t amount = 150; uint64_t creditor = 0x1111; float hit[3] = {1, 2, 3};
    memcpy(event, &target, 4); memcpy(event + 4, &kind, 4); memcpy(event + 0x18, &creditor, 8); memcpy(event + 0x2C, &amount, 4);
    memcpy(event + 0x50, hit, 12);
    g_consumeDamage = FakeConsume; g_heardCount = 0;
    assert(ConsumeDamageDetour(NULL, event, 0, 41, 0, 0) == 42 && g_consumed == event);
    assert(g_heardCount == 1 && g_lastDamage.target == target && g_lastDamage.kind == 4 && g_lastDamage.amount == 150 &&
        g_lastDamage.creditor == 0x1111 && g_lastDamage.position[2] == 3.0f);
    DWORD thread = g_gameThread; g_gameThread = thread + 1; /* another thread: queued for the next update */
    ConsumeDamageDetour(NULL, event, 0, 0, 0, 0);
    assert(g_heardCount == 1 && g_queued == 1);
    EventsTick();
    assert(g_heardCount == 2 && g_queued == 0 && g_lastDamage.info.update == g_updates);

    g_listenerCount = 0; g_listening = 0; g_eventsGame = 0; g_playerCount = 0; g_lastState = 0; g_consumeDamage = NULL;
    free(player); free(peers); free(players); free(gameState); free(game);
}

/* Abilities: a HellForge ability holds its countdown (not done, its base not run), then runs its base from tick 0, and
 * reports the base's end; refusals for a bad base or hold. */
static uint32_t g_baseTicks[8];
static int g_baseCalls;
static uint8_t FakeBaseAbility(void* ctx, uint32_t tick)
{
    if (g_baseCalls < 8) g_baseTicks[g_baseCalls] = tick;
    g_baseCalls++;
    return tick == 120;
}

static void AbilitiesFixture(void)
{
    HfMod* mod = &g_mods[0];
    g_abilityCount = 0; g_abilityState = 0; g_ablGame = 0;
    HfAbility bad = {sizeof bad};
    bad.base = 99999; assert(!ApiAddAbility(mod, &bad));
    bad.base = HF_ABILITY_SILO_LAUNCH; bad.hold_seconds = -1; assert(!ApiAddAbility(mod, &bad));
    HfAbility countdown = {sizeof countdown};
    countdown.base = HF_ABILITY_SILO_LAUNCH;
    countdown.hold_seconds = 10.0f;
    uint32_t id = ApiAddAbility(mod, &countdown);
    assert(id == ABL_FIRST && g_abilities[0].holdTicks == 600);
    g_abilities[0].baseFn = FakeBaseAbility;
    uint8_t ctx[16] = {0};
    for (uint32_t tick = 0; tick < 600; tick++) assert(AbilityTick(ctx, id, tick) == 0);
    assert(g_baseCalls == 0);
    assert(AbilityTick(ctx, id, 600) == 0 && AbilityTick(ctx, id, 602) == 0 && AbilityTick(ctx, id, 720) == 1);
    assert(g_baseCalls == 3 && g_baseTicks[0] == 0 && g_baseTicks[1] == 2 && g_baseTicks[2] == 120);
    assert(AbilityTick(ctx, id + 1, 0) == 0); /* not one of HellForge's */
    HfAbility probe = {sizeof probe}; /* no base: done when its hold ends */
    probe.hold_seconds = 1.0f;
    uint32_t probeId = ApiAddAbility(mod, &probe);
    assert(probeId == ABL_FIRST + 1 && AbilityTick(ctx, probeId, 59) == 0 && AbilityTick(ctx, probeId, 60) == 1 && g_baseCalls == 3);
    HfAbility silo = {sizeof silo}; /* a silo countdown: no base, known flags only */
    silo.hold_seconds = 10.0f; silo.flags = 0x2; assert(!ApiAddAbility(mod, &silo));
    silo.flags = HF_ABILITY_HOLDS_LINKED_LAUNCH; silo.base = HF_ABILITY_SILO_LAUNCH; assert(!ApiAddAbility(mod, &silo));
    silo.base = 0;
    uint32_t siloId = ApiAddAbility(mod, &silo);
    assert(siloId == ABL_FIRST + 2 && g_abilities[2].holdsLaunch && g_abilities[2].holdTicks == 600);
    g_launchReady = 0; assert(AbilityTick(ctx, siloId, 0) == 1); /* its game functions unchecked: done at once, the silo's launch stands */
    HfAbility old = {offsetof(HfAbility, flags)}; /* a mod from before flags */
    old.hold_seconds = 1.0f;
    assert(ApiAddAbility(mod, &old) == ABL_FIRST + 3 && !g_abilities[3].holdsLaunch);
    g_abilityCount = 0; g_baseCalls = 0;
}

/* The loose remote's guard (abilities.c, InstallLooseGuard) against a synthetic game: pinned, the 13 bytes at 0x64270B
 * become a jump to the stub; the stub checks ebx for -1 (on to 0x642731), else runs the 13 bytes and jumps back to
 * 0x642718. A changed function gets no guard. */
static uint8_t g_looseDescriptor[0x20], g_missileDescriptor[0x20];
static uint32_t* FakeHandleEntity(uint32_t* out, uint32_t handle) { *out = handle == 0x1234 ? 42 : 0; return out; }
static const uint8_t* FakeDescriptor(uint32_t entity) { return entity == 42 ? g_looseDescriptor : entity == 7 ? g_missileDescriptor : NULL; }
static void FakeJump(uint8_t* at, void* to) { at[0] = 0x48; at[1] = 0xB8; memcpy(at + 2, &to, 8); at[10] = 0xFF; at[11] = 0xE0; } /* mov rax, to; jmp rax */
static void FakeMap(uint8_t* manager, uint32_t at, uint32_t* table, uint32_t key, uint32_t value)
{
    table[0] = key; table[1] = value; /* one slot: capacity 1, multiplier 0, the empty key 0xFFFFFFFE */
    uint32_t* fields = (uint32_t*)(manager + at + 8);
    *(uint32_t**)(manager + at) = table; fields[0] = 1; fields[1] = 0xFFFFFFFEu; fields[2] = 0;
}

static uintptr_t RelTarget(const uint8_t* at, int size) { int32_t rel; memcpy(&rel, at + size - 4, 4); return (uintptr_t)at + size + rel; }

static void LooseGuardFixture(void)
{
    size_t size = 0x3500000;
    uint8_t* game = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(game);
    assert(!GuardPins((uintptr_t)game)); /* not the reviewed code */
    memcpy(game + ABL_GUARD_LOOKUP, kGuardLookupPin, sizeof kGuardLookupPin);
    memcpy(game + ABL_GUARD_FOUND, kGuardFoundPin, sizeof kGuardFoundPin);
    memcpy(game + ABL_GUARD_SITE, kGuardSitePin, sizeof kGuardSitePin);
    memcpy(game + ABL_GUARD_BACK, kGuardBackPin, sizeof kGuardBackPin);
    memcpy(game + ABL_GUARD_NEXT, kGuardNextPin, sizeof kGuardNextPin);
    assert(!GuardPins((uintptr_t)game)); /* the end still missing */
    memcpy(game + ABL_GUARD_END, kGuardEndPin, sizeof kGuardEndPin);
    assert(GuardPins((uintptr_t)game));
    uint8_t* stub = game + 0x3400000;
    EmitLooseGuard((uintptr_t)game, stub);
    assert(!memcmp(stub, "\x83\xFB\xFF\x0F\x84", 5) && RelTarget(stub + 3, 6) == (uintptr_t)game + ABL_GUARD_END);
    assert(!memcmp(stub + 9, kGuardSitePin, sizeof kGuardSitePin));
    assert(stub[22] == 0xE9 && RelTarget(stub + 22, 5) == (uintptr_t)game + ABL_GUARD_BACK);
    g_looseGuard = 0;
    InstallLooseGuard((uintptr_t)game, stub);
    uint8_t* site = game + ABL_GUARD_SITE;
    assert(g_looseGuard && site[0] == 0xE9 && RelTarget(site, 5) == (uintptr_t)stub);
    assert(!memcmp(site + 5, kGuardSitePin + 5, 8)); /* the rest untouched: the stub returns past it */
    assert(!GuardPins((uintptr_t)game)); /* installed once: the site is no longer the game's */
    g_looseGuard = 0;
    VirtualFree(game, 0, MEM_RELEASE);
}

/* The silo countdown in co-op (abilities.c, co-op fix 2) against a synthetic game: only the machine with authority over
 * the missile launches it, with the silo, owner and laser point it knows, as the remote's link would; every machine
 * holding a countdown marks the remote's link "no link" and gives it back; SiloFrame frees slots whose end will not come. */
static uint32_t g_shadowCalls[10], g_shadowRack, g_shadowOwner, g_shadowEngine, g_startMode;
static float g_shadowPoint[3];
static uint8_t g_linkRecord[12], *g_linkRecordAt = g_linkRecord;
static const Ability* g_startAbility;
enum { SC_RACK, SC_ENGINE, SC_LAUNCH, SC_OWNER, SC_GUIDE, SC_RELEASE, SC_START };
static uint32_t* FakeRackOf(void* linkers, uint32_t* out, uint32_t remote) { g_shadowCalls[SC_RACK]++; if (remote == 42) *out = 55; return out; }
static uint32_t* FakeLinked(void* linkers, uint32_t* out, uint32_t remote) { if (remote == 42) *out = 7; return out; }
static uint8_t* FakeLinkerRecord(void* descriptor) { return g_linkRecordAt; }
static void FakeEngine(void* missiles, uint32_t missile, uint8_t on) { g_shadowCalls[SC_ENGINE]++; g_shadowEngine = missile * 10 + on; }
static void FakeLaunch(void* missiles, uint32_t missile) { g_shadowCalls[SC_LAUNCH]++; }
static void FakeOwner(void* explosives, uint32_t missile, uint32_t owner) { g_shadowCalls[SC_OWNER]++; g_shadowOwner = owner; }
static void FakeGuide(void* unused, uint32_t entity, const float point[3]) { g_shadowCalls[SC_GUIDE]++; memcpy(g_shadowPoint, point, 12); }
static void FakeRelease(void* racks, uint32_t rack, uint32_t item) { g_shadowCalls[SC_RELEASE]++; g_shadowRack = rack; }
/* StartAbility runs the ability's first tick on the missile at once */
static void FakeStart(void* abilities, uint32_t entity, uint32_t ability, uint32_t mode, float speed)
{
    g_shadowCalls[SC_START]++;
    g_startMode = mode;
    LinkedLaunchTick(g_startAbility, entity, 0, AblAuthority(entity));
}
static uint32_t LinkMagic(void) { uint32_t magic; memcpy(&magic, g_linkRecordAt + 4, 4); return magic; }

static void ShadowLaunchFixture(void)
{
    size_t size = 0x3500000;
    uint8_t* game = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(game);
    g_ablGame = g_bhvGame = (uintptr_t)game;
    FakeJump(game + kLaunchFunctions[L_HANDLE].rva, (void*)FakeHandleEntity);
    FakeJump(game + kLaunchFunctions[L_LINKED].rva, (void*)FakeLinked);
    FakeJump(game + kLaunchFunctions[L_RECORD].rva, (void*)FakeLinkerRecord);
    FakeJump(game + kLaunchFunctions[L_RACK].rva, (void*)FakeRackOf);
    FakeJump(game + kLaunchFunctions[L_ENGINE].rva, (void*)FakeEngine);
    FakeJump(game + kLaunchFunctions[L_LAUNCH].rva, (void*)FakeLaunch);
    FakeJump(game + kLaunchFunctions[L_OWNER].rva, (void*)FakeOwner);
    FakeJump(game + kLaunchFunctions[L_GUIDE].rva, (void*)FakeGuide);
    FakeJump(game + kLaunchFunctions[L_RELEASE].rva, (void*)FakeRelease);
    FakeJump(game + kLaunchFunctions[L_START].rva, (void*)FakeStart);
    FakeJump(game + kBhvFunctions[F_DESCRIPTOR].rva, (void*)FakeDescriptor);
    uint32_t yes = 1, no = 0, silo = ABL_SILO_LINK;
    memcpy(g_looseDescriptor + 0x14, &yes, 4);
    memcpy(g_missileDescriptor + 0x14, &yes, 4); /* authority over missile 7 here */
    /* missile 7 names remote 42 (handle 0x1234); 42 is linked (silo 55), owned by 99, its laser on at (10, 20, 30); its
     * linker record is the table's (no copies here) until the copies are given */
    static uint8_t missiles[0x60], linkers[0xA0], owners[0x50], lasers[0x70], explosives[0x50], guidance[0x860], racks[0x40], abilities[0x30];
    static uint8_t instances[0x38], ownerRecords[48], laserInstances[0x38], laserOn[1], copies[24];
    static uint8_t* descriptors[1];
    static uint32_t tables[8][2];
    FakeMap(missiles, 0x20, tables[0], 7, 0);
    *(uint8_t**)(missiles + 0x50) = instances;
    *(uint32_t*)(instances + 8) = 0x1234;
    FakeMap(linkers, 0x20, tables[1], 42, 0);
    *(uint8_t***)(linkers + 0x38) = descriptors;
    memcpy(g_linkRecord + 4, &silo, 4);
    FakeMap(owners, 0x20, tables[2], 42, 0);
    *(uint8_t**)(owners + 0x40) = ownerRecords;
    *(uint32_t*)(ownerRecords + 4) = 99;
    FakeMap(lasers, 0x30, tables[3], 42, 0);
    float point[3] = {10, 20, 30};
    memcpy(laserInstances + 0x2C, point, 12);
    laserOn[0] = 1;
    *(uint8_t**)(lasers + 0x58) = laserInstances;
    *(uint8_t**)(lasers + 0x60) = laserOn;
    FakeMap(explosives, 0x38, tables[4], 7, 0);
    FakeMap(guidance, 0x840, tables[5], 7, 0);
    FakeMap(racks, 0x28, tables[6], 55, 0);
    FakeMap(abilities, 0x18, tables[7], 7, 0);
    *(uint8_t**)(game + ABL_MISSILES) = missiles; *(uint8_t**)(game + ABL_LINKERS) = linkers; *(uint8_t**)(game + ABL_OWNERS) = owners;
    *(uint8_t**)(game + ABL_LASERS) = lasers; *(uint8_t**)(game + ABL_EXPLOSIVES) = explosives; *(uint8_t**)(game + ABL_GUIDANCE) = guidance;
    *(uint8_t**)(game + ABL_RACKS) = racks; *(uint8_t**)(game + ABL_ABILITIES) = abilities;
    memset(g_heldLaunches, 0, sizeof g_heldLaunches);
    memset(g_sharedMarks, 0, sizeof g_sharedMarks);
    g_launchReady = 1;
    Ability a = {0};
    a.id = ABL_FIRST; a.holdTicks = 600; a.holdsLaunch = 1;
    g_startAbility = &a;

    /* set off on another machine, authority here: the link is marked through the countdown, the launch is this machine's */
    for (uint32_t tick = 0; tick < 600; tick++) assert(LinkedLaunchTick(&a, 7, tick, 1) == 0);
    assert(g_shadowCalls[SC_LAUNCH] == 0 && HeldFor(7) != NULL && HeldFor(7)->shadow && HeldFor(7)->rack == 55 && LinkMagic() == 0);
    assert(HeldFor(7)->sharedRecord == g_linkRecord && g_sharedMarks[0].count == 1);
    assert(LinkedLaunchTick(&a, 7, 600, 1) == 1);
    assert(g_shadowCalls[SC_ENGINE] == 1 && g_shadowEngine == 71 && g_shadowCalls[SC_LAUNCH] == 1 && g_shadowCalls[SC_OWNER] == 1 && g_shadowOwner == 99);
    assert(g_shadowCalls[SC_GUIDE] == 1 && !memcmp(g_shadowPoint, point, 12) && g_shadowCalls[SC_RELEASE] == 1 && g_shadowRack == 55);
    assert(HeldFor(7) == NULL && LinkMagic() == ABL_SILO_LINK && g_sharedMarks[0].count == 0); /* the slot is free, the link back */

    /* without authority: the countdown runs and marks the link; its end launches nothing and keeps the mark until the
     * owner's launch arrives (the laser's link would launch this machine's copy otherwise) */
    memcpy(g_missileDescriptor + 0x14, &no, 4);
    int launches = (int)g_shadowCalls[SC_LAUNCH];
    for (uint32_t tick = 0; tick < 600; tick++) assert(LinkedLaunchTick(&a, 7, tick, 0) == 0);
    assert(HeldFor(7) != NULL && LinkMagic() == 0);
    assert(LinkedLaunchTick(&a, 7, 600, 0) == 1 && (int)g_shadowCalls[SC_LAUNCH] == launches && HeldFor(7) != NULL && HeldFor(7)->ended && LinkMagic() == 0);
    instances[0x35] = 1; /* the engine byte alone: the link still launches (it checks +0x36) */
    SiloFrame(NULL, 0, NULL);
    assert(HeldFor(7) != NULL && LinkMagic() == 0);
    instances[0x36] = 1; /* the launch arrived */
    SiloFrame(NULL, 0, NULL);
    assert(HeldFor(7) == NULL && LinkMagic() == ABL_SILO_LINK && g_sharedMarks[0].count == 0 && (int)g_shadowCalls[SC_LAUNCH] == launches);
    instances[0x35] = instances[0x36] = 0;
    /* ... over without authority, then the missile moved here unlaunched (its owner left): launched here */
    for (uint32_t tick = 0; tick <= 600; tick++) LinkedLaunchTick(&a, 7, tick, 0);
    assert(HeldFor(7) != NULL && HeldFor(7)->ended);
    memcpy(g_missileDescriptor + 0x14, &yes, 4);
    SiloFrame(NULL, 0, NULL);
    assert(HeldFor(7) == NULL && (int)g_shadowCalls[SC_LAUNCH] == launches + 1 && LinkMagic() == ABL_SILO_LINK);
    launches++;

    /* migrated here mid-countdown (no slot, authority at the end): the slot is made at the end and the missile launched */
    memcpy(g_missileDescriptor + 0x14, &yes, 4);
    for (uint32_t tick = 300; tick < 600; tick++) assert(LinkedLaunchTick(&a, 7, tick, 1) == 0);
    assert(HeldFor(7) == NULL);
    assert(LinkedLaunchTick(&a, 7, 600, 1) == 1 && (int)g_shadowCalls[SC_LAUNCH] == launches + 1 && HeldFor(7) == NULL && LinkMagic() == ABL_SILO_LINK);

    /* a missile that flies already (its launch reached this machine some other way) is left as it is */
    instances[0x36] = 1;
    for (uint32_t tick = 0; tick <= 600; tick++) LinkedLaunchTick(&a, 7, tick, 1);
    assert((int)g_shadowCalls[SC_LAUNCH] == launches + 1 && HeldFor(7) == NULL && LinkMagic() == ABL_SILO_LINK);
    instances[0x36] = 0;

    /* a slot whose missile is gone (its countdown never ended) is taken again */
    for (uint32_t i = 0; i < ABL_MAX_HELD; i++) g_heldLaunches[i].missile = 1000 + i; /* none of them a missile now */
    assert(LinkedLaunchTick(&a, 7, 0, 1) == 0 && HeldFor(7) != NULL && LinkMagic() == 0);
    SiloMissionEnd(NULL, NULL, NULL); /* the mission ends: every slot freed, the link back */
    assert(HeldFor(7) == NULL && LinkMagic() == ABL_SILO_LINK && g_sharedMarks[0].count == 0);
    for (uint32_t i = 0; i < ABL_MAX_HELD; i++) assert(g_heldLaunches[i].missile == 0);

    /* a laser point that is no place on a map (a joiner's of 2026-10-21) guides nothing: the missile keeps its own */
    int guides = (int)g_shadowCalls[SC_GUIDE];
    float bogus[3] = {9550, -3148, -1273}, sane[3] = {384, -450, 16};
    memcpy(laserInstances + 0x2C, bogus, 12);
    for (uint32_t tick = 0; tick <= 600; tick++) LinkedLaunchTick(&a, 7, tick, 1);
    assert((int)g_shadowCalls[SC_LAUNCH] == launches + 2 && (int)g_shadowCalls[SC_GUIDE] == guides && HeldFor(7) == NULL);
    assert(PlausiblePoint(sane) && PlausiblePoint(point) && !PlausiblePoint(bogus));
    memcpy(laserInstances + 0x2C, point, 12);

    /* SiloFrame: another machine's launch reached the missile mid-countdown (its launched byte): the slot is freed and
     * the link given back; the countdown's end launches nothing more */
    assert(LinkedLaunchTick(&a, 7, 0, 0) == 0 && HeldFor(7) != NULL);
    SiloFrame(NULL, 0, NULL);
    assert(HeldFor(7) != NULL); /* nothing yet */
    instances[0x36] = 1;
    SiloFrame(NULL, 0, NULL);
    assert(HeldFor(7) == NULL && LinkMagic() == ABL_SILO_LINK);
    launches = (int)g_shadowCalls[SC_LAUNCH];
    for (uint32_t tick = 1; tick <= 600; tick++) LinkedLaunchTick(&a, 7, tick, 1);
    assert((int)g_shadowCalls[SC_LAUNCH] == launches);
    instances[0x36] = 0;
    /* ... the missile gone (removed with no last tick) */
    assert(LinkedLaunchTick(&a, 7, 0, 0) == 0 && HeldFor(7) != NULL);
    tables[0][0] = 0xFFFFFFFEu;
    SiloFrame(NULL, 0, NULL);
    assert(HeldFor(7) == NULL && LinkMagic() == ABL_SILO_LINK);
    tables[0][0] = 7;
    /* ... long past its end: without authority 5 s after its hold, with it three holds and 10 s */
    memcpy(g_missileDescriptor + 0x14, &no, 4);
    assert(LinkedLaunchTick(&a, 7, 0, 0) == 0 && HeldFor(7) != NULL);
    HeldFor(7)->since = GetTickCount64() - 14000;
    SiloFrame(NULL, 0, NULL);
    assert(HeldFor(7) != NULL);
    HeldFor(7)->since = GetTickCount64() - 16000;
    SiloFrame(NULL, 0, NULL);
    assert(HeldFor(7) == NULL && LinkMagic() == ABL_SILO_LINK);
    memcpy(g_missileDescriptor + 0x14, &yes, 4);
    assert(LinkedLaunchTick(&a, 7, 0, 1) == 0 && HeldFor(7) != NULL);
    HeldFor(7)->since = GetTickCount64() - 14000;
    SiloFrame(NULL, 0, NULL);
    assert(HeldFor(7) != NULL);
    HeldFor(7)->since = GetTickCount64() - 41000;
    SiloFrame(NULL, 0, NULL);
    assert(HeldFor(7) == NULL && LinkMagic() == ABL_SILO_LINK);

    /* the remote fired here: with authority over the missile, mode 1; without (a friend's silo), mode 2, sent to the
     * machine with authority; either way the link is marked until the countdown ends */
    g_shadowCalls[SC_START] = 0;
    assert(LinkedLaunchTick(&a, 42, 0, 1) == 1 && g_shadowCalls[SC_START] == 1 && g_startMode == 1);
    assert(HeldFor(7) != NULL && !HeldFor(7)->shadow && HeldFor(7)->started && HeldFor(7)->rack == 55 && LinkMagic() == 0);
    assert(LinkedLaunchTick(&a, 42, 0, 1) == 1 && g_shadowCalls[SC_START] == 1); /* counting down already */
    launches = (int)g_shadowCalls[SC_LAUNCH];
    for (uint32_t tick = 1; tick <= 600; tick++) LinkedLaunchTick(&a, 7, tick, 1);
    assert((int)g_shadowCalls[SC_LAUNCH] == launches + 1 && HeldFor(7) == NULL && LinkMagic() == ABL_SILO_LINK);
    memcpy(g_missileDescriptor + 0x14, &no, 4);
    assert(LinkedLaunchTick(&a, 42, 0, 1) == 1 && g_shadowCalls[SC_START] == 2 && g_startMode == 2 && HeldFor(7) != NULL);
    for (uint32_t tick = 1; tick <= 600; tick++) LinkedLaunchTick(&a, 7, tick, 0);
    assert((int)g_shadowCalls[SC_LAUNCH] == launches + 1 && HeldFor(7) != NULL && LinkMagic() == 0); /* waiting for the owner's launch */
    instances[0x36] = 1;
    SiloFrame(NULL, 0, NULL);
    assert(HeldFor(7) == NULL && LinkMagic() == ABL_SILO_LINK);
    instances[0x36] = 0;
    memcpy(g_missileDescriptor + 0x14, &yes, 4);
    /* another machine's copy of the remote's fire (no authority here over the remote) starts nothing */
    assert(LinkedLaunchTick(&a, 42, 0, 0) == 1 && g_shadowCalls[SC_START] == 2 && HeldFor(7) == NULL && LinkMagic() == ABL_SILO_LINK);
    /* a record that is no silo link: no countdown */
    uint32_t other = 0x01020304;
    memcpy(g_linkRecord + 4, &other, 4);
    assert(LinkedLaunchTick(&a, 42, 0, 1) == 1 && g_shadowCalls[SC_START] == 2 && HeldFor(7) == NULL && LinkMagic() == other);
    memcpy(g_linkRecord + 4, &silo, 4);

    /* the table's record is shared by countdowns: given back by the last; a remote's own copy is looked up again */
    HeldLaunch one = {42}, two = {42};
    assert(MarkLink(&one) && MarkLink(&two) && LinkMagic() == 0 && g_sharedMarks[0].count == 2);
    GiveLinkBack(&one);
    assert(LinkMagic() == 0 && !one.linkMarked);
    GiveLinkBack(&one); /* twice: nothing */
    assert(LinkMagic() == 0 && g_sharedMarks[0].count == 1);
    GiveLinkBack(&two);
    assert(LinkMagic() == ABL_SILO_LINK && g_sharedMarks[0].count == 0);
    g_linkRecordAt = copies + 12; /* this machine's copy (the second of two) */
    *(uint8_t**)(linkers + 0x98) = copies; *(uint32_t*)(linkers + 0x90) = 2;
    memcpy(copies + 12 + 4, &silo, 4);
    assert(MarkLink(&one) && one.sharedRecord == NULL && LinkMagic() == 0);
    memcpy(copies, copies + 12, 12); /* the first copy's remote removed: this one moves into its place */
    memset(copies + 12, 0, 12);
    *(uint32_t*)(linkers + 0x90) = 1;
    g_linkRecordAt = copies;
    GiveLinkBack(&one);
    assert(LinkMagic() == ABL_SILO_LINK && *(uint32_t*)(copies + 12 + 4) == 0);
    g_linkRecordAt = g_linkRecord;
    *(uint8_t**)(linkers + 0x98) = NULL; *(uint32_t*)(linkers + 0x90) = 0;

    memset(g_diagMissiles, 0, sizeof g_diagMissiles);
    memset(g_heldLaunches, 0, sizeof g_heldLaunches);
    memset(g_sharedMarks, 0, sizeof g_sharedMarks);
    g_launchReady = 0;
    g_startAbility = NULL;
    VirtualFree(game, 0, MEM_RELEASE);
    g_ablGame = g_bhvGame = 0;
}

/* Rejoining after a crash (netreap.c) against a synthetic exe: after the game's cleanup, a dead connection whose peer has
 * left the game session is destroyed and the session told; a dead one still in the session, a live one, and another
 * transport are left alone. */
static uint8_t* g_reapConnections[4];
static uint32_t g_reapCount, g_reapCleanups, g_reapRemovedCount;
static uint64_t g_reapRemoved[4], g_reapMember;
static void FakeReapCleanup(void* transport, void* wanted, void* removed) { g_reapCleanups++; }
static void FakeReapDestroy(void* transport, void* connection)
{
    uint32_t at = 0;
    while (at < g_reapCount && g_reapConnections[at] != connection) at++;
    assert(at < g_reapCount);
    memmove(g_reapConnections + at, g_reapConnections + at + 1, (g_reapCount - at - 1) * sizeof g_reapConnections[0]);
    g_reapCount--;
    memcpy((uint8_t*)transport + 0xA0, &g_reapCount, 4);
}
static uint8_t FakeReapMember(void* session, uint64_t peer) { return peer == g_reapMember; }
static void FakeReapRemoved(void* session, void* removed)
{
    struct { uint32_t count, capacity; uint64_t* peers; }* list = removed;
    for (uint32_t i = 0; i < list->count && g_reapRemovedCount < 4; i++) g_reapRemoved[g_reapRemovedCount++] = list->peers[i];
}

static void ReaperFixture(void)
{
    uint8_t* exe = VirtualAlloc(NULL, 0x1A20000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(exe);
    static uint8_t manager[0xE0], transport[0xB0], other[0xB0], session[8];
    static uint8_t connections[4][0x18800];
    FakeJump(exe + REAP_DESTROY, (void*)FakeReapDestroy);
    FakeJump(exe + REAP_MEMBER, (void*)FakeReapMember);
    FakeJump(exe + REAP_REMOVED, (void*)FakeReapRemoved);
    *(uint8_t**)(exe + REAP_MANAGER) = manager;
    *(uint8_t**)(manager + 0x98) = transport;
    *(uint8_t**)(manager + 0xA8) = session;
    *(uint8_t**)transport = exe + REAP_TRANSPORT_VT;
    *(uint8_t**)other = exe + REAP_TRANSPORT_VT;
    *(uint8_t**)session = exe + REAP_SESSION_VT;
    *(uint8_t**)(exe + REAP_SESSION_VT + 0x80) = exe + REAP_MEMBER;
    *(uint8_t**)(exe + REAP_SESSION_VT + 0xE0) = exe + REAP_REMOVED;
    /* peer 1 live; 2 dead, gone from the session; 3 dead, still in it; 4 dead, gone */
    for (uint32_t i = 0; i < 4; i++)
    {
        uint64_t peer = i + 1;
        memcpy(connections[i] + REAP_PEER, &peer, 8);
        connections[i][REAP_DEAD] = i != 0;
        g_reapConnections[i] = connections[i];
    }
    g_reapCount = 4;
    g_reapMember = 3;
    memcpy(transport + 0xA0, &g_reapCount, 4);
    *(uint8_t***)(transport + 0xA8) = g_reapConnections;
    g_reapExe = (uintptr_t)exe;
    g_reapCleanup = FakeReapCleanup;

    ReapCleanupDetour(other, NULL, NULL); /* not the manager's transport: the game's cleanup alone */
    assert(g_reapCleanups == 1 && g_reapCount == 4 && g_reapRemovedCount == 0);
    *(uint8_t**)session = exe + 8; /* not the reviewed session */
    ReapCleanupDetour(transport, NULL, NULL);
    assert(g_reapCleanups == 2 && g_reapCount == 4);
    *(uint8_t**)session = exe + REAP_SESSION_VT;
    ReapCleanupDetour(transport, NULL, NULL);
    assert(g_reapCleanups == 3 && g_reapCount == 2 && g_reapConnections[0] == connections[0] && g_reapConnections[1] == connections[2]);
    assert(g_reapRemovedCount == 2 && g_reapRemoved[0] == 2 && g_reapRemoved[1] == 4 && g_reaped == 2);
    ReapCleanupDetour(transport, NULL, NULL); /* nothing more */
    assert(g_reapCount == 2 && g_reapRemovedCount == 2);
    static uint64_t keep[1] = {3}; /* peer 3 leaves the session, but the game's explicit list keeps it */
    *(uint32_t*)(manager + 0xD0) = 1; *(uint64_t**)(manager + 0xD8) = keep;
    g_reapMember = 0;
    ReapCleanupDetour(transport, NULL, NULL);
    assert(g_reapCount == 2 && g_reapRemovedCount == 2);
    *(uint32_t*)(manager + 0xD0) = 0; /* off the list too */
    ReapCleanupDetour(transport, NULL, NULL);
    assert(g_reapCount == 1 && g_reapRemovedCount == 3 && g_reapRemoved[2] == 3);
    /* the first connection dead (the index goes back below 0 and on), and the one after it */
    connections[0][REAP_DEAD] = 1;
    g_reapConnections[1] = connections[1]; connections[1][REAP_DEAD] = 1;
    g_reapCount = 2; memcpy(transport + 0xA0, &g_reapCount, 4);
    ReapCleanupDetour(transport, NULL, NULL);
    assert(g_reapCount == 0 && g_reapRemovedCount == 4 && g_reapRemoved[3] == 1);

    g_reapExe = 0; g_reapCleanup = NULL; g_reaped = 0;
    VirtualFree(exe, 0, MEM_RELEASE);
}

/* Added entities are numbered (short ids, network types) by resource id, not load order (registry.c, WeaponOrder): two
 * machines that registered the same entities in another order number them alike. */
static void WeaponOrderFixture(void)
{
    int savedCount = g_weaponCount;
    Weapon saved[3];
    memcpy(saved, g_weapons, sizeof saved);
    g_weaponCount = 3;
    g_weapons[0].entity = 0x30; g_weapons[1].entity = 0x10; g_weapons[2].entity = 0x20;
    int order[REG_MAX_WEAPONS];
    WeaponOrder(order);
    assert(order[0] == 1 && order[1] == 2 && order[2] == 0);
    g_weapons[0].entity = 0x10; g_weapons[1].entity = 0x20; g_weapons[2].entity = 0x30; /* the same entities, loaded the other way */
    WeaponOrder(order);
    assert(order[0] == 0 && order[1] == 1 && order[2] == 2);
    memcpy(g_weapons, saved, sizeof saved);
    g_weaponCount = savedCount;
}

/* Code mods set up in id order (the same on every machine), their edits queued meanwhile kept in load order, so the top
 * mod still wins a conflict (hellforge_modloader.c SetupOrder, fields.c QueueEdit and FinishSetupEdits). */
static void SetupOrderFixture(void)
{
    int savedCount = g_modCount, savedEdits = g_editCount, savedCapacity = g_editCapacity;
    FieldEdit* savedQueue = g_edits; /* a queue of its own */
    g_edits = NULL; g_editCapacity = 0;
    HfMod saved[3];
    memcpy(saved, g_mods, sizeof saved);
    g_modCount = 3; /* load order: the top mod last */
    strcpy(g_mods[0].id, "giblett.beelzebub-pack");
    strcpy(g_mods[1].id, "skyeshade.stratagem-compendium");
    strcpy(g_mods[2].id, "anne.another-mod");
    int order[MAX_MODS];
    SetupOrder(order);
    assert(order[0] == 2 && order[1] == 0 && order[2] == 1); /* by id, whatever the load order */

    g_editCount = 0;
    g_modsSettingUp = 1;
    for (int k = 0; k < 3; k++) /* each mod queues a tag edit as it sets up, in id order */
    {
        FieldEdit e;
        memset(&e, 0, sizeof e);
        e.mod = &g_mods[order[k]];
        e.kind = EDIT_TAG;
        e.line = (uint32_t)order[k];
        assert(QueueEdit(e) == 0);
    }
    g_modsSettingUp = 0;
    assert(g_editCount == 3 && g_edits[0].mod == &g_mods[0] && g_edits[1].mod == &g_mods[1] && g_edits[2].mod == &g_mods[2]);
    FinishSetupEdits(); /* tags: nothing to report or write here */
    free(g_edits);
    g_edits = savedQueue; g_editCapacity = savedCapacity; g_editCount = savedEdits;
    memcpy(g_mods, saved, sizeof saved);
    g_modCount = savedCount;
}

/* The behaviour stubs against a synthetic game (behaviours.c InstallBehaviours): every pinned function and dispatch site
 * as reviewed, then the resume stub run. A HellForge behaviour resumes as its base does (the transport as 667: its
 * flight target re-sent on other machines), any other id takes the switch's default. */
static void BehaviourStubsFixture(void)
{
    size_t size = 0x1000000;
    uint8_t* game = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(game);
    for (int f = 0; f < F_COUNT; f++) memcpy(game + kBhvFunctions[f].rva, kBhvFunctions[f].pin, 32);
    for (int s = 0; s < S_COUNT; s++)
    {
        int32_t rel = (int32_t)(kBhvSites[s].target - (kBhvSites[s].ja + 6));
        game[kBhvSites[s].ja] = 0x0F; game[kBhvSites[s].ja + 1] = kBhvSites[s].cond ? kBhvSites[s].cond : 0x87;
        memcpy(game + kBhvSites[s].ja + 2, &rel, 4);
    }
    memcpy(game + kBhvSites[S_RESUME].ja - 6, kBhvResumePin, sizeof kBhvResumePin);
    memcpy(game + BHV_RESUME_SWITCH, kBhvResumeSwitchPin, sizeof kBhvResumeSwitchPin);
    HfMod* mod = &g_mods[0];
    g_behaviourCount = 0; g_behaviourState = 0;
    HfBehaviour transport = {sizeof transport}, turret = {sizeof turret};
    transport.base = HF_BEHAVIOUR_PELICAN_TRANSPORT;
    turret.base = HF_BEHAVIOUR_PELICAN_TURRET;
    assert(ApiAddBehaviour(mod, &transport) == BHV_FIRST && ApiAddBehaviour(mod, &turret) == BHV_FIRST + 1);
    assert(InstallBehaviours((uintptr_t)game) == 1 && g_behaviourState == 1);
    /* the switch, resume's end and the branch's own target, after the pins were checked: each answers what it was given */
    static const uint8_t cases[] = {0x89, 0xD0, 0xC3};                          /* mov eax, edx; ret */
    static const uint8_t none[] = {0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3};          /* mov eax, -1; ret */
    static const uint8_t onward[] = {0xB8, 0xFE, 0xFF, 0xFF, 0xFF, 0xC3};        /* mov eax, -2; ret */
    memcpy(game + BHV_RESUME_CASES, cases, sizeof cases);
    memcpy(game + BHV_RESUME_NONE, none, sizeof none);
    memcpy(game + kBhvSites[S_RESUME].target, onward, sizeof onward);
    int32_t rel;
    memcpy(&rel, game + kBhvSites[S_RESUME].ja + 2, 4);
    typedef int (*ResumeFn)(void* ctx, uint32_t index);
    ResumeFn resume = (ResumeFn)(game + kBhvSites[S_RESUME].ja + 6 + rel);
    assert((uint8_t*)resume < game || (uint8_t*)resume >= game + size); /* patched into HellForge's block */
    assert(resume(NULL, BHV_FIRST) == (int)(BHV_PELICAN_TRANSPORT - BHV_RESUME_FIRST));   /* 694 resumes as 667 */
    assert(resume(NULL, BHV_FIRST + 1) == (int)(BHV_PELICAN_TURRET - BHV_RESUME_FIRST)); /* 695 as 645 */
    assert(resume(NULL, BHV_FIRST + 2) == -2 && resume(NULL, 300) == -2 && resume(NULL, BHV_PELICAN_TRANSPORT) == -2); /* every other id as before */
    g_behaviourCount = 0; g_behaviourState = 0; g_bhvGame = 0;
    VirtualFree(game, 0, MEM_RELEASE);
}

static void MountsFixture(void)
{
    const RefMountSlot* s = &kRefMountSlots[0];
    const RefMount* vanilla = &kRefMounts[kRefMountAllowed[s->first]];
    const RefMount *plain = NULL, *packaged = NULL;
    for (uint16_t i = 1; i < s->count; i++)
    {
        const RefMount* m = &kRefMounts[kRefMountAllowed[s->first + i]];
        if (!m->package && !plain) plain = m;
        if (m->package && !packaged) packaged = m;
    }
    assert(plain && packaged);
    g_componentCount = 0;
    Component* c = &g_components[g_componentCount++];
    memset(c, 0, sizeof *c);
    strcpy(c->name, "MountComponentData"); c->index = 283; c->stride = 120;
    g_fieldRecord = FakeFieldRecord;
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 1);
    uint8_t* record = FakeRecordOf("MountComponentData", s->vehicle, 120, 0);
    memcpy(record + s->slot * 24, &vanilla->resource, 8);
    g_packageState = MountPackageState; g_packageRequest = RefRequest; g_tickState = 1;
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, PKG_SYSTEM + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* system = (uint8_t*)calloc(1, 64); uint8_t* map = (uint8_t*)calloc(1024, PKG_MAP_STRIDE);
    memcpy(game + PKG_REQUEST, kPackageRequestPin, sizeof kPackageRequestPin);
    uintptr_t at = (uintptr_t)system; memcpy(game + PKG_SYSTEM, &at, 8);
    at = (uintptr_t)map; memcpy(system + PKG_MAP_ENTRIES_AT, &at, 8);
    uint32_t capacity = 1024; memcpy(system + PKG_MAP_CAPACITY_AT, &capacity, 4);
    g_packagesGame = (uintptr_t)game; g_packagesProven = 1;
    HfMod* mod = &g_mods[0];
    HfField slot0 = HF_FIELD(Mount, slot0_entity);
    HfValue v = {HF_TYPE_U64, 0, plain->resource};
    uint64_t read;
    assert(ApiSetLive(mod, s->vehicle, &slot0, &v, 0) == HF_APPLIES_RESPAWN);
    memcpy(&read, record, 8); assert(read == plain->resource);
    v.u64 = 0x1234567812345678ull; /* not a weapon this slot holds */
    assert(ApiSetLive(mod, s->vehicle, &slot0, &v, 0) == 0);
    memcpy(&read, record, 8); assert(read == plain->resource);
    v.u64 = packaged->resource; /* waits for its package */
    int edits = g_editCount;
    assert(ApiSetLive(mod, s->vehicle, &slot0, &v, 0) == HF_APPLIES_RESPAWN && g_editCount == edits); /* replaces the earlier one */
    memcpy(&read, record, 8); assert(read == plain->resource);
    PackagesRequest();
    g_mountResident[0] = kRefPackages[packaged->package - 1].id;
    ApplyWaitingEdits();
    memcpy(&read, record, 8); assert(read == packaged->resource);
    assert(ApiSetLive(mod, s->vehicle, &slot0, NULL, 0) == HF_APPLIES_RESPAWN); /* reset: the vanilla weapon */
    memcpy(&read, record, 8); assert(read == vanilla->resource);
    for (int i = 0; i < g_fakeRecordCount; i++) free(g_fakeRecords[i].bytes);
    g_fakeRecordCount = g_componentCount = g_editCount = 0; g_editsWaiting = 0;
    g_heldCount = 0; g_packagesProven = 0; g_tickState = 0; g_packagesGame = 0; g_packageState = NULL; g_packageRequest = NULL;
    g_fieldRecord = ComponentRecordOf;
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 0);
    free(map); free(system); VirtualFree(game, 0, MEM_RELEASE);
}

/* Hellpod payloads (refs.c RefPickupCheck): a rack slot takes a reviewed pickup or nothing (live, a u64), refuses another
 * entity and a slot past the four HellForge may change, waits for a packaged pickup's package, and goes back to its
 * vanilla item on reset. */
static void PodsFixture(void)
{
    const RefPodSlot* s = NULL;
    for (size_t i = 0; i < sizeof kRefPodSlots / sizeof kRefPodSlots[0] && s == NULL; i++)
        if (kRefPodSlots[i].slot == 0 && kRefPodSlots[i].vanilla) s = &kRefPodSlots[i];
    const RefPickup *plain = NULL, *packaged = NULL;
    for (size_t i = 0; i < sizeof kRefPickups / sizeof kRefPickups[0]; i++)
    {
        const RefPickup* p = &kRefPickups[i];
        if (s == NULL || p->resource == s->vanilla) continue;
        if (!p->package && !plain) plain = p;
        if (p->package && !packaged) packaged = p;
    }
    assert(s && packaged);
    g_componentCount = 0;
    Component* c = &g_components[g_componentCount++];
    memset(c, 0, sizeof *c);
    strcpy(c->name, "HellpodRackComponentData"); c->index = 155; c->stride = 568;
    g_fieldRecord = FakeFieldRecord;
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 1);
    uint8_t* record = FakeRecordOf("HellpodRackComponentData", s->rack, 568, 0);
    memcpy(record, &s->vanilla, 8);
    g_packageState = MountPackageState; g_packageRequest = RefRequest; g_tickState = 1;
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, PKG_SYSTEM + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* system = (uint8_t*)calloc(1, 64); uint8_t* map = (uint8_t*)calloc(1024, PKG_MAP_STRIDE);
    memcpy(game + PKG_REQUEST, kPackageRequestPin, sizeof kPackageRequestPin);
    uintptr_t at = (uintptr_t)system; memcpy(game + PKG_SYSTEM, &at, 8);
    at = (uintptr_t)map; memcpy(system + PKG_MAP_ENTRIES_AT, &at, 8);
    uint32_t capacity = 1024; memcpy(system + PKG_MAP_CAPACITY_AT, &capacity, 4);
    g_packagesGame = (uintptr_t)game; g_packagesProven = 1;
    memset(g_mountResident, 0, sizeof g_mountResident);
    HfMod* mod = &g_mods[0];
    HfField slot0 = HF_FIELD(HellpodRack, slot0_item), slot4 = HF_FIELD(HellpodRack, slot4_item);
    HfValue v = {HF_TYPE_U64, 0, 0};
    uint64_t read;
    assert(ApiSetLive(mod, s->rack, &slot0, &v, 0) == HF_APPLIES_RESPAWN); /* empty */
    memcpy(&read, record, 8); assert(read == 0);
    if (plain) { v.u64 = plain->resource; assert(ApiSetLive(mod, s->rack, &slot0, &v, 0) == HF_APPLIES_RESPAWN); memcpy(&read, record, 8); assert(read == plain->resource); }
    uint64_t before = read;
    v.u64 = 0x1234567812345678ull; /* not a pickup */
    assert(ApiSetLive(mod, s->rack, &slot0, &v, 0) == 0);
    memcpy(&read, record, 8); assert(read == before);
    v.u64 = packaged->resource;
    assert(ApiSetLive(mod, s->rack, &slot4, &v, 0) == 0); /* a slot HellForge does not change */
    int edits = g_editCount;
    assert(ApiSetLive(mod, s->rack, &slot0, &v, 0) == HF_APPLIES_RESPAWN && g_editCount == edits); /* waits for its package */
    memcpy(&read, record, 8); assert(read == before);
    PackagesRequest();
    g_mountResident[0] = kRefPackages[packaged->package - 1].id;
    ApplyWaitingEdits();
    memcpy(&read, record, 8); assert(read == packaged->resource);
    assert(ApiSetLive(mod, s->rack, &slot0, NULL, 0) == HF_APPLIES_RESPAWN); /* reset: the vanilla item */
    memcpy(&read, record, 8); assert(read == s->vanilla);
    for (int i = 0; i < g_fakeRecordCount; i++) free(g_fakeRecords[i].bytes);
    g_fakeRecordCount = g_componentCount = g_editCount = 0; g_editsWaiting = 0;
    g_heldCount = 0; g_packagesProven = 0; g_tickState = 0; g_packagesGame = 0; g_packageState = NULL; g_packageRequest = NULL;
    g_fieldRecord = ComponentRecordOf;
    memset(g_mountResident, 0, sizeof g_mountResident);
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 0);
    free(map); free(system); VirtualFree(game, 0, MEM_RELEASE);
}


/* SDK 11 (actions.c, event_polls.c): the game's hash maps as the readers probe them, the enemy table, turning a shot,
 * shot rules (ranges, one of a kind per projectile, stopping), and an action off the game thread waiting for its update. */
static void ActionsFixture(void)
{
    /* a map {buckets, capacity, empty, multiplier}: keys probed from key * multiplier, the empty key ends it */
    uint32_t buckets[8][2];
    for (int i = 0; i < 8; i++) { buckets[i][0] = 0xFFFFFFFFu; buckets[i][1] = 0; }
    uint32_t key = 77, multiplier = 3, at = (key * multiplier) % 8;
    buckets[at][0] = 5; buckets[at][1] = 1;          /* another key in its slot: probed past */
    buckets[(at + 1) % 8][0] = key; buckets[(at + 1) % 8][1] = 42;
    uint8_t header[20]; uint64_t b = (uint64_t)(uintptr_t)buckets; uint32_t cap = 8, empty = 0xFFFFFFFFu;
    memcpy(header, &b, 8); memcpy(header + 8, &cap, 4); memcpy(header + 12, &empty, 4); memcpy(header + 16, &multiplier, 4);
    uint32_t value = 0;
    assert(EvpHash((uintptr_t)header, key, 8, 4, &value) && value == 42);
    assert(!EvpHash((uintptr_t)header, 78, 8, 4, &value) && !EvpHash((uintptr_t)header, 0, 8, 4, &value));
    /* the enemy table is sorted, so its search finds each */
    for (size_t i = 1; i < sizeof kActEnemies / sizeof kActEnemies[0]; i++) assert(kActEnemies[i - 1] < kActEnemies[i]);
    assert(ActEnemy(0xFF5CC825B9571052ull) && ActEnemy(0x0002BA767DF856F3ull) && !ActEnemy(ACT_AVATAR) && !ActEnemy(0));
    /* a turn keeps the speed and turns at most the angle */
    float v[3] = {10, 0, 0}, to[3] = {0, 10, 0};
    ActTurn(v, to, 3.14159265f / 4);
    float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    assert(fabsf(speed - 10) < 1e-3f && fabsf(atan2f(v[1], v[0]) - 3.14159265f / 4) < 1e-3f);
    ActTurn(v, to, 10.0f); assert(fabsf(v[0]) < 1e-3f && fabsf(v[1] - 10) < 1e-3f); /* all the way, not past */
    /* shot rules */
    HfMod* mod = &g_mods[0];
    uint64_t sources[] = {0xB6AFF2195568767Full, 0};
    HfShotRule rule = {sizeof rule, 40, sources, 2.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0};
    uint32_t id = ApiModifyShots(mod, &rule);
    assert(id != 0 && g_actRuleCount == 1 && g_actRules[0].sourceCount == 1 && g_actRules[0].damage == 2.0f);
    assert(ApiModifyShots(mod, &rule) == 0);                              /* one of a kind per projectile */
    HfShotRule wild = rule; wild.projectile = 41; wild.damage = 1000;
    assert(ApiModifyShots(mod, &wild) == 0);                              /* outside its range */
    HfHoming homing = {sizeof homing, 40, NULL, 90, 30, 100, 2, 1, 0};
    uint32_t hid = ApiHoming(mod, &homing);
    assert(hid != 0 && hid != id && g_actRuleCount == 2 && g_actRules[1].sourceCount == 0 && fabsf(g_actRules[1].cone - cosf(30 * 3.14159265f / 180)) < 1e-5f);
    assert(!ApiStopShots(&g_mods[1], id) && ApiStopShots(mod, id) && ApiStopShots(mod, hid) && g_actRuleCount == 0 && !ApiStopShots(mod, id));
    /* off the game thread: queued, run (and refused here: no game) at the next update */
    int tick = g_tickState; DWORD thread = g_gameThread;
    g_tickState = 1; g_gameThread = GetCurrentThreadId() + 1;
    float up[3] = {0, 0, 5};
    assert(ApiAddVelocity(mod, up) == HF_ACT_QUEUED && g_actQueued == 1);
    g_gameThread = GetCurrentThreadId();
    ActionsTick();
    assert(g_actQueued == 0);
    assert(ApiHeal(mod, 10) == 0); /* on the game thread: run now, refused (no game.dll here) */
    HfLimbState limb;
    assert(ActLimbState(mod, 1, 9, &limb) == 0);
    g_tickState = tick; g_gameThread = thread; g_actLastTick = 0;
}

/* copy_row (refs.c ApiCopyRow): the Urchin's explosion row takes a listed donor's bytes once its packages are resident
 * (waiting until then, tried again each tick), refuses another row or donor, and gets its own bytes back; the row that
 * follows it (the final discharge) takes the same copy and its own back with it, and cannot be named itself. */
static uint8_t g_copyRows[4][152];
static uintptr_t CopyTableRow(const char* component, uint64_t id, int* ready)
{
    *ready = 1;
    if (strcmp(component, "ExplosionSettings")) return 0;
    return id == kRefRowCopies[0].row ? (uintptr_t)g_copyRows[0] : id == kRefRowDonors[kRefRowCopies[0].first + 1] ? (uintptr_t)g_copyRows[1] :
        id == kRefRowDonors[kRefRowCopies[0].first + 2] ? (uintptr_t)g_copyRows[2] : id == kRefRowCopies[1].row ? (uintptr_t)g_copyRows[3] : 0;
}
static void RowCopyFixture(void)
{
    const RefRowCopy* spec = &kRefRowCopies[0];
    uint32_t donor = kRefRowDonors[spec->first + 1], other = kRefRowDonors[spec->first + 2];
    const RefRow* row = RefRowOf(kRefExplosions, sizeof kRefExplosions / sizeof kRefExplosions[0], donor);
    assert(row != NULL);
    const RefRowCopy* follower = &kRefRowCopies[1];
    assert(follower->with == spec->row && spec->with == 0);
    for (int r = 0; r < 4; r++) for (int b = 0; b < 152; b++) g_copyRows[r][b] = (uint8_t)(r * 50 + b);
    memcpy(g_copyRows[0], &spec->row, 4); memcpy(g_copyRows[1], &donor, 4); memcpy(g_copyRows[2], &other, 4); memcpy(g_copyRows[3], &follower->row, 4);
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, PKG_SYSTEM + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* system = (uint8_t*)calloc(1, 64); uint8_t* map = (uint8_t*)calloc(1024, PKG_MAP_STRIDE);
    memcpy(game + PKG_REQUEST, kPackageRequestPin, sizeof kPackageRequestPin);
    uintptr_t at = (uintptr_t)system; memcpy(game + PKG_SYSTEM, &at, 8);
    at = (uintptr_t)map; memcpy(system + PKG_MAP_ENTRIES_AT, &at, 8);
    uint32_t capacity = 1024; memcpy(system + PKG_MAP_CAPACITY_AT, &capacity, 4);
    g_packagesGame = (uintptr_t)game; g_packagesProven = 1;
    g_packageState = MountPackageState; g_packageRequest = RefRequest; g_tickState = 1;
    memset(g_mountResident, 0, sizeof g_mountResident);
    g_tableRow = CopyTableRow;
    HfMod* mod = &g_mods[0];
    uint8_t own[152], followerOwn[152]; memcpy(own, g_copyRows[0], 152); memcpy(followerOwn, g_copyRows[3], 152);
    assert(ApiCopyRow(mod, "ExplosionSettings", follower->row, donor, follower->offset, follower->size) == 0); /* follows another row */
    assert(ApiCopyRow(mod, "ExplosionSettings", spec->row, 9999, spec->offset, spec->size) == 0); /* not a listed donor */
    assert(ApiCopyRow(mod, "ExplosionSettings", 1, donor, spec->offset, spec->size) == 0);           /* not a listed row */
    assert(ApiCopyRow(mod, "ExplosionSettings", spec->row, donor, spec->offset, spec->size) == HF_APPLIES_NOW);
    if (row->package || row->package2)
    {
        assert(!memcmp(g_copyRows[0], own, 152) && g_rowCopiesWaiting); /* waits for the donor's packages */
        PackagesRequest();
        if (row->package) g_mountResident[0] = kRefPackages[row->package - 1].id;
        if (row->package2) g_mountResident[1] = kRefPackages[row->package2 - 1].id;
        RowCopiesTick();
    }
    assert(!memcmp(g_copyRows[0] + spec->offset, g_copyRows[1] + spec->offset, spec->size) && !memcmp(g_copyRows[0], own, spec->offset) && !g_rowCopiesWaiting);
    assert(!memcmp(g_copyRows[3] + spec->offset, g_copyRows[1] + spec->offset, spec->size) && !memcmp(g_copyRows[3], followerOwn, spec->offset));
    assert(ApiCopyRow(mod, "ExplosionSettings", spec->row, 0, spec->offset, spec->size) == HF_APPLIES_NOW); /* its own back */
    assert(!memcmp(g_copyRows[0], own, 152) && !memcmp(g_copyRows[3], followerOwn, 152) && g_rowCopyCount == 0);
    g_rowCopyCount = 0; g_rowCopiesWaiting = 0; g_tableRow = TableRowOf;
    g_heldCount = 0; g_packagesProven = 0; g_tickState = 0; g_packagesGame = 0; g_packageState = NULL; g_packageRequest = NULL;
    memset(g_mountResident, 0, sizeof g_mountResident);
    free(map); free(system); VirtualFree(game, 0, MEM_RELEASE);
}

/* FarHook (hellforge_modloader.c): code MinHook cannot reach. An overlay's jump stubs are taken over in place (their
 * address becomes the detour, the old one the original); plain code gets a far jump and a trampoline that still runs
 * it; a conditional branch in the first bytes is refused. */
typedef int (*FarFn)(void);
static FarFn g_farOriginal;
static int FarTarget(void) { return 8; }
static int FarDetour(void) { return g_farOriginal() + 100; }
static void FarHookFixture(void)
{
    uint8_t* code = (uint8_t*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(code);
    const char* how = NULL;
    void* original = NULL;
    /* jmp [rip+0]; address */
    uint64_t to = (uint64_t)(uintptr_t)FarTarget;
    uint8_t stub[14] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(stub + 6, &to, 8);
    memcpy(code, stub, 14);
    assert(FarHook((uintptr_t)code, (void*)FarDetour, &original, &how) == 0 && original == (void*)FarTarget);
    g_farOriginal = (FarFn)original;
    assert(((FarFn)code)() == 108 && strstr(how, "swapped"));
    /* mov rax, imm64; jmp rax */
    uint8_t* stub2 = code + 64;
    stub2[0] = 0x48; stub2[1] = 0xB8; memcpy(stub2 + 2, &to, 8); stub2[10] = 0xFF; stub2[11] = 0xE0;
    assert(FarHook((uintptr_t)stub2, (void*)FarDetour, &original, &how) == 0 && original == (void*)FarTarget);
    assert(((FarFn)stub2)() == 108);
    /* plain code: mov eax, 7; add eax, 1; mov rax, rax; mov rax, rax; ret (8) */
    static const uint8_t body[] = {0xB8, 7, 0, 0, 0, 0x83, 0xC0, 1, 0x48, 0x89, 0xC0, 0x48, 0x89, 0xC0, 0xC3};
    uint8_t* fn = code + 128;
    memcpy(fn, body, sizeof body);
    assert(((FarFn)fn)() == 8);
    assert(FarHook((uintptr_t)fn, (void*)FarDetour, &original, &how) == 0 && strstr(how, "trampoline"));
    g_farOriginal = (FarFn)original;
    assert(((FarFn)fn)() == 108 && g_farOriginal() == 8);
    VirtualFree(original, 0, MEM_RELEASE);
    /* a conditional branch first: refused, the code untouched */
    static const uint8_t branchy[] = {0x74, 0x02, 0xB8, 7, 0, 0, 0, 0x83, 0xC0, 1, 0x48, 0x89, 0xC0, 0x48, 0x89, 0xC0, 0xC3};
    uint8_t* fn2 = code + 256;
    memcpy(fn2, branchy, sizeof branchy);
    assert(FarHook((uintptr_t)fn2, (void*)FarDetour, &original, &how) == 1 && !memcmp(fn2, branchy, sizeof branchy) && strstr(how, "conditional"));
    VirtualFree(code, 0, MEM_RELEASE);
}

/* Managed hooks (hooks.c): one chain per function. Mods' hooks run first, the highest priority first, equal ones the
 * mod later in load order first; HellForge's own run last, the newest first; each calls the next through its original,
 * which is rewritten when a hook joins after it. */
typedef int (*ChainFn)(int);
static __attribute__((noinline)) int ChainTarget(int x)
{
    volatile int y = x;
    y = y * 3 + 1;
    return y;
}
static ChainFn g_chainOwn, g_chainOwn2, g_chainA, g_chainB, g_chainLate;
static char g_trail[32];
static int g_trailAt;
static void Trail(char c) { if (g_trailAt < 31) { g_trail[g_trailAt++] = c; g_trail[g_trailAt] = 0; } }
static int ChainOwn(int x) { Trail('h'); return g_chainOwn(x) + 1; }
static int ChainOwn2(int x) { Trail('i'); return g_chainOwn2(x) + 2; }
static int ChainA(int x) { Trail('a'); return g_chainA(x) + 10; }
static int ChainB(int x) { Trail('b'); return g_chainB(x) + 100; }
static int ChainLate(int x) { Trail('l'); return g_chainLate(x) + 1000; }
static __attribute__((noinline)) int ChainTarget2(int x)
{
    volatile int y = x;
    y = y * 2 + 7;
    return y;
}
static ChainFn g_chainA2, g_chainB2;
static int ChainA2(int x) { Trail('a'); return g_chainA2(x) + 10; }
static int ChainB2(int x) { Trail('b'); return g_chainB2(x) + 100; }
static int CallChain(int x)
{
    g_trailAt = 0;
    g_trail[0] = 0;
    return ((volatile ChainFn)ChainTarget)(x);
}
static void HookChainFixture(void)
{
    uint8_t before[16], seen[16];
    assert(!SafeRead((uintptr_t)ChainTarget, before, sizeof before));
    assert(CallChain(5) == 16 && !strcmp(g_trail, ""));
    wchar_t savedLogs[MAX_PATH];
    wcscpy(savedLogs, g_logs);
    GetTempPathW(MAX_PATH, g_logs);
    /* this process's own folder: another run at the same time writes and deletes its own hooks.tsv */
    swprintf(g_logs + wcslen(g_logs), MAX_PATH - wcslen(g_logs), L"hellforge-hook-chain-%lu", GetCurrentProcessId());
    CreateDirectoryW(g_logs, NULL);

    /* HellForge's own first: the function runs through it */
    assert(LoaderHook("test events", (uintptr_t)ChainTarget, (void*)ChainOwn, (void**)&g_chainOwn) == 0);
    assert(CallChain(5) == 17 && !strcmp(g_trail, "h"));
    /* a hooked function still reads as it was (pins) */
    assert(!HookedCode((uintptr_t)ChainTarget, seen, sizeof seen) && !memcmp(seen, before, sizeof before));
    assert(!SafeRead((uintptr_t)ChainTarget, seen, sizeof seen) && memcmp(seen, before, sizeof before));
    /* a mod's hook after it goes before it; a mod later in load order before an earlier one */
    assert(ApiHook(&g_mods[0], (uintptr_t)ChainTarget, (void*)ChainA, (void**)&g_chainA) == 0);
    assert(CallChain(5) == 27 && !strcmp(g_trail, "ah"));
    assert(ApiHook(&g_mods[1], (uintptr_t)ChainTarget, (void*)ChainB, (void**)&g_chainB) == 0);
    assert(CallChain(5) == 127 && !strcmp(g_trail, "bah"));
    /* a higher priority first, whatever the load order */
    assert(ApiHookEx(&g_mods[0], (uintptr_t)ChainTarget, (void*)ChainLate, (void**)&g_chainLate, 5) == 0);
    assert(CallChain(5) == 1127 && !strcmp(g_trail, "lbah"));
    /* HellForge's next own hook: after every mod's, before its earlier one */
    assert(LoaderHook("test events 2", (uintptr_t)ChainTarget, (void*)ChainOwn2, (void**)&g_chainOwn2) == 0);
    assert(CallChain(5) == 1129 && !strcmp(g_trail, "lbaih"));
    /* the same detour twice is refused, the chain unchanged */
    assert(ApiHook(&g_mods[1], (uintptr_t)ChainTarget, (void*)ChainA, (void**)&g_chainA) == 1);
    assert(ApiHook(&g_mods[1], 0, (void*)ChainB, (void**)&g_chainB) == 1);
    assert(CallChain(5) == 1129 && !strcmp(g_trail, "lbaih"));
    /* the report lists the chain first to last */
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\hooks.tsv", g_logs);
    FILE* report = _wfopen(path, L"rb");
    assert(report);
    char text[2048] = {0};
    fread(text, 1, sizeof text - 1, report);
    fclose(report);
    char* a = strstr(text, "\ta\t5\t");
    char* b = strstr(text, "\tb\t0\t");
    char* own2 = strstr(text, "HellForge (test events 2)");
    assert(a && b && own2 && a < b && b < own2 && strstr(text, "1 of 5") && strstr(text, "5 of 5\tHellForge (test events)\t"));
    DeleteFileW(path);
    RemoveDirectoryW(g_logs);
    wcscpy(g_logs, savedLogs);

    /* code_matches and read_code: two mods each check a function against its reviewed bytes, then hook it; the
     * second's check passes after the first hooked it (its live bytes are a jump), and a wrong pattern still fails */
    uint8_t pin[8];
    char pattern[64] = "", wrong[64] = "";
    assert(!SafeRead((uintptr_t)ChainTarget2, pin, sizeof pin));
    for (int i = 0; i < 8; i++) sprintf(pattern + strlen(pattern), i ? " %02X" : "%02X", pin[i]);
    for (int i = 0; i < 8; i++) sprintf(wrong + strlen(wrong), i ? " %02X" : "%02X", (uint8_t)(i == 3 ? pin[i] ^ 0xFF : pin[i]));
    uint8_t code[8];
    assert(ApiCodeMatches((uintptr_t)ChainTarget2, pattern) == 1 && ApiCodeMatches((uintptr_t)ChainTarget2, wrong) == 0);
    assert(ApiHook(&g_mods[0], (uintptr_t)ChainTarget2, (void*)ChainA2, (void**)&g_chainA2) == 0);
    assert(!SafeRead((uintptr_t)ChainTarget2, code, sizeof code) && memcmp(code, pin, sizeof pin)); /* the live bytes: a jump */
    assert(ApiCodeMatches((uintptr_t)ChainTarget2, pattern) == 1 && ApiCodeMatches((uintptr_t)ChainTarget2, wrong) == 0);
    assert(ApiCodeMatches((uintptr_t)ChainTarget2, "?? ?? ??") == 1 && ApiCodeMatches((uintptr_t)ChainTarget2, "zz") == 0 &&
        ApiCodeMatches(0, pattern) == 0 && ApiCodeMatches((uintptr_t)ChainTarget2, "") == 0);
    assert(ApiReadCode((uintptr_t)ChainTarget2, code, sizeof code) == 0 && !memcmp(code, pin, sizeof pin));
    assert(ApiHook(&g_mods[1], (uintptr_t)ChainTarget2, (void*)ChainB2, (void**)&g_chainB2) == 0);
    g_trailAt = 0;
    assert(((volatile ChainFn)ChainTarget2)(5) == 2 * 5 + 7 + 110 && !strcmp(g_trail, "ba"));
}

int main(int argc, char** argv)
{
    strcpy(g_build, "25480438");
    for (int i = 0; i < 2; i++) { g_mods[i].loaded = 1; g_mods[i].log = INVALID_HANDLE_VALUE; }
    strcpy(g_mods[0].id, "a"); strcpy(g_mods[1].id, "b");
    g_modCount = 2;
    FieldsFixture();
    LiveFixture();
    LiveSyncFixture();
    LuaBridgeFixture();
    ActionsFixture();
    PassivesFixture();
    RefsFixture();
    DirectorFixture();
    ShotsFixture();
    RoundsFixture();
    PerksFixture();
    {
        /* thousands of edits (an editor applying a preset): the list grows, none refused */
        int before = g_editCount;
        for (int i = 0; i < 3000; i++)
        {
            FieldEdit e = {&g_mods[0], EDIT_SET, 0x1000u + (uint64_t)i, 0, &kSdkComponents[0], &kSdkComponents[0].fields[0]};
            e.vanilla = 1;
            assert(QueueEdit(e) == 0);
        }
        assert(g_editCount == before + 3000 && g_editCapacity >= 3000);
        g_editCount = before;
    }
    MountsFixture();
    PodsFixture();
    RowCopyFixture();
    FarHookFixture();
    HookChainFixture();
    EventsFixture();
    AbilitiesFixture();
    LooseGuardFixture();
    ReaperFixture();
    ShadowLaunchFixture();
    WeaponOrderFixture();
    SetupOrderFixture();
    BehaviourStubsFixture();
    ExtraFixtures();
    MeleeFixture();
    Component* c = &g_components[0]; g_componentCount = 1;
    strcpy(c->name, "Fixture"); c->index = 999; c->indices = 4; c->records = 1; c->recordOffset = 64; c->stride = 16;
    uint32_t extent = 80; memcpy(c->header + 16, &extent, 4);
    uint8_t* state = calloc(1, REG_TABLES + 8 * 1000);
    uint8_t* block = calloc(1, 112); uint8_t* table = block + 32;
    memcpy(block, &c->index, 4); memcpy(block + 4, c->header, 28);
    *(uint64_t*)(table + 16) = 1; *(uint64_t*)(table + 32) = 2; /* two owners, record zero */
    assert(!SetStatePointer(state, REG_TABLES + 8 * c->index, table));
    uint32_t* record = (uint32_t*)(table + 64); record[0] = 10; record[1] = 20;
    g_stats[0] = Edit(0, 30, 0, 0); g_stats[0].hasExpect = 1; g_stats[0].expect = 10;
    g_stats[1] = Edit(1, 40, 0, 0); g_stats[1].hasExpect = 1; g_stats[1].expect = 10;
    g_stats[2] = Edit(1, 99, 4, 0); g_stats[2].hasExpect = 1; g_stats[2].expect = 999;
    g_statCount = 3; FixtureLoad(state);
    assert(record[0] == 40 && record[1] == 20 && g_stats[1].original == 10 && !g_stats[2].applied);
    /* A one-byte flag (u8) writes its byte only, at any offset; a value past a byte is refused when the plan is read. */
    {
        uint32_t keep = record[1];
        record[1] = 0x11223344u;
        g_stats[0] = Edit(0, 0xAB, 5, 0); g_stats[0].size = 1; g_statCount = 1;
        FixtureLoad(state);
        assert(g_stats[0].applied && record[1] == 0x1122AB44u && g_stats[0].original == 0x33);
        record[1] = keep;
        char good[] = "stat\ta\tcomponent\tFixture\t0000000000000001\t0\t5\tu8\t000000AB\t-\t0\t1\t0\tflag";
        char wide[] = "stat\ta\tcomponent\tFixture\t0000000000000001\t0\t5\tu8\t00000100\t-\t0\t1\t0\tflag";
        char table[] = "stat\ta\tDamageInfo\t-\t0000000000000000\t54\t5\tu8\t00000001\t-\t0\t1\t0\tflag";
        char* lines[] = {good, wide, table};
        int mods = g_modCount;
        char id[sizeof g_mods[0].id];
        strcpy(id, g_mods[0].id); strcpy(g_mods[0].id, "a"); if (g_modCount < 1) g_modCount = 1;
        for (int k = 0; k < 3; k++)
        {
            char* f[16]; int n = 0;
            for (char* t = strtok(lines[k], "\t"); t && n < 16; t = strtok(NULL, "\t")) f[n++] = t;
            int before = g_statCount;
            int refused = StatsParse(f, n) != 0;
            assert(k == 0 ? !refused && g_statCount == before + 1 && g_stats[before].size == 1 && g_stats[before].offset == 5 : refused && g_statCount == before);
            g_statCount = before;
        }
        strcpy(g_mods[0].id, id); g_modCount = mods;
    }
    /* Unexpected record identity and framing both refuse. */
    g_statCount = 1; g_stats[0] = Edit(0, 70, 0, 0); g_stats[0].record = 1;
    FixtureLoad(state); assert(record[0] == 40 && !g_stats[0].applied);
    g_stats[0].record = 0; block[4] ^= 1;
    FixtureLoad(state); assert(record[0] == 40 && !g_stats[0].applied); block[4] ^= 1;
    /* OwnRecord redirects only entity one; subsequent fields reuse its clone. */
    g_stats[0] = Edit(0, 80, 0, 1); g_stats[1] = Edit(1, 90, 4, 1); g_statCount = 2;
    FixtureLoad(state);
    uint8_t* owned = StatePointer(state, REG_TABLES + 8 * c->index);
    uint32_t one = *(uint32_t*)(owned + 16 + 8), two = *(uint32_t*)(owned + 32 + 8);
    assert(one == 1 && two == 0 && g_owned[0].next == 2);
    assert(*(uint32_t*)(owned + 64 + 16) == 80 && *(uint32_t*)(owned + 64 + 20) == 90);
    assert(*(uint32_t*)(owned + 64) == 40);
    /* Shared -> own -> shared keeps original expected values across the table move. */
    SetStatePointer(state, REG_TABLES + 8 * c->index, table); g_ownedCount = 0; record[0] = 10;
    g_stats[0] = Edit(0, 30, 0, 0); g_stats[1] = Edit(1, 80, 0, 1); g_stats[2] = Edit(1, 50, 0, 0);
    g_stats[2].resource = 2;
    for (int i = 0; i < 3; i++) { g_stats[i].hasExpect = 1; g_stats[i].expect = 10; }
    g_statCount = 3; FixtureLoad(state);
    owned = StatePointer(state, REG_TABLES + 8 * c->index);
    assert(g_stats[0].applied && g_stats[1].applied && g_stats[2].applied);
    assert(*(uint32_t*)(owned + 64 + 16) == 80 && *(uint32_t*)(owned + 64) == 50);
    *(uint32_t*)(owned + 64) = 7; StatsApplyComponents(state);
    assert(*(uint32_t*)(owned + 64) == 7); /* startup catch-up cannot reapply the same load */
    /* The moved damage table wins over the original; settings are not written twice. */
    uint8_t* game = calloc(1, 0x3800000);
    uint8_t* oldRow = calloc(1, DMG_ROW); *(uint32_t*)oldRow = 54; *(uint32_t*)(oldRow + 4) = 5;
    uint8_t* newRow = calloc(1, DMG_ROW); *(uint32_t*)newRow = 54; *(uint32_t*)(newRow + 4) = 10;
    *(uintptr_t*)(game + DMG_TABLE + 54 * 8) = (uintptr_t)oldRow;
    g_damageTable = calloc(DMG_VANILLA, 8); *(uintptr_t*)(g_damageTable + 54 * 8) = (uintptr_t)newRow;
    g_statCount = 1; g_stats[0] = Edit(0, 150, 4, 0); strcpy(g_stats[0].kind, "DamageInfo"); g_stats[0].record = 54;
    StatsApplyTable(1, (uintptr_t)game);
    assert(*(uint32_t*)(newRow + 4) == 150 && *(uint32_t*)(oldRow + 4) == 5);
    *(uint32_t*)(newRow + 4) = 27; StatsApplyTable(1, (uintptr_t)game); assert(*(uint32_t*)(newRow + 4) == 27);
    /* Observations use the redirected owned component and the moved settings table, once per load. */
    g_game = (uintptr_t)game; *(uintptr_t*)(game + REG_STATE_GLOBAL) = (uintptr_t)state;
    g_statsProbeCount = 0;
    *StatsProbeSlot() = Edit(0, 0, 0, 0);
    *StatsProbeSlot() = g_stats[0];
    assert(StatsObservedComponent(&g_statsProbes[0]) == (uintptr_t)owned + 64 + 16);
    /* Records come from the table the game reads now (HellForge's moved copy), not the loaded region's stale one. */
    {
        uintptr_t was = g_region; g_region = 1;
        uint32_t stride = 0;
        assert(ComponentRecordOf("Fixture", 1, &stride, 0) == (uintptr_t)owned + 64 + 16 && stride == 16);
        assert(ComponentRecordOf("Fixture", 2, &stride, 0) == (uintptr_t)owned + 64);
        /* A record two vanilla entities share is refused; shared with a mod's copy of its entity (a weapon added from
         * it, sharing its records), it is the entity's own. */
        SetStatePointer(state, REG_TABLES + 8 * c->index, table);
        assert(ComponentRecordOf("Fixture", 1, &stride, 0) == 0 && ComponentRecordOf("Fixture", 1, &stride, 1) == (uintptr_t)table + 64);
        g_weapons[0] = (Weapon){0}; g_weapons[0].entity = 2; g_weapons[0].state = 1; g_weaponCount = 1;
        assert(ComponentRecordOf("Fixture", 1, &stride, 0) == (uintptr_t)table + 64);
        g_weaponCount = 0;
        SetStatePointer(state, REG_TABLES + 8 * c->index, owned);
        g_region = was;
    }
    StatsSnapshotReady((uintptr_t)game);
    /* An edited field reports its value from before the stats pass. */
    assert(g_statsProbes[0].applied && g_statsProbes[0].value == 80 && g_statsProbes[1].applied && g_statsProbes[1].value == 10);
    *(uint32_t*)(newRow + 4) = 28;
    StatsSnapshotReady((uintptr_t)game); assert(g_statsProbes[1].value == 10);
    /* get_stat: the value now, and its base without the Local Stats mod's edit (another mod's edit is its holder) */
    {
        HfLiveValue out;
        HfStat stat = {"DamageInfo", "Fixture", 1, 54, 4, HF_TYPE_U32};
        char id[sizeof g_mods[0].id];
        strcpy(id, g_mods[0].id); strcpy(g_mods[0].id, "some.mod");
        assert(ApiGetStat(&g_mods[0], &stat, &out) && out.now.number == 28 && out.base.number == 28 && !strcmp(out.mod, "some.mod"));
        strcpy(g_mods[0].id, LOCAL_STATS_ID);
        assert(ApiGetStat(&g_mods[0], &stat, &out) && out.now.number == 28 && out.base.number == 10 && out.mod == NULL);
        HfStat other = {"DamageInfo", "Fixture", 1, 54, 8, HF_TYPE_U32};
        assert(!ApiGetStat(&g_mods[0], &other, &out));
        strcpy(g_mods[0].id, id);
    }
    g_statsProbeCount = 0;
    wchar_t temporary[MAX_PATH], plan[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, temporary) && GetTempFileNameW(temporary, L"hfs", 0, plan));
    FILE* file = _wfopen(plan, L"wb"); assert(file);
    fputs("mod\ta\tfixture\t-\nstat\ta\tcomponent\tFixture\t0000000000000001\t0\t0\tu32\t00000014\t0000000A\t0\t1\t0\tfixture\n", file);
    fclose(file); g_statCount = 0; g_modCount = 0;
    assert(ReadPlan(plan) == 0 && g_statCount == 1 && g_modCount == 1 && !wcscmp(g_mods[0].dll, L"-"));
    assert(g_stats[0].hasExpect && g_stats[0].expect == 10 && g_stats[0].value == 20);
    /* More observations than the loader once held (16384), and one it cannot read: the plan, and its mods, still load. */
    file = _wfopen(plan, L"wb"); assert(file);
    for (int i = 0; i < 20000; i++) fprintf(file, "observe\tDamageInfo\t-\t0\t%d\t4\ti32\n", 1 + i % 600);
    fputs("observe\tNoSuchTable\t-\t0\t1\t4\ti32\nmod\ta\tfixture\t-\n", file);
    fclose(file); g_statCount = 0; g_modCount = 0; g_statsProbeCount = 0; g_statsProbesSkipped = 0;
    assert(ReadPlan(plan) == 0 && g_modCount == 1 && g_statsProbeCount == 20000 && g_statsProbesSkipped == 1);
    g_statsProbeCount = 0;
    DeleteFileW(plan);
    if (argc == 3)
    {
        /* Cross-language contract: consume Core's complete plan and emit an actual native snapshot. */
        g_modCount = g_statCount = g_componentCount = g_statsProbeCount = g_pinCount = 0;
        Utf8ToWide(argv[1], plan, MAX_PATH);
        assert(!ReadPlan(plan) && g_statsProbeCount > 10000 && g_componentCount > 35);
        Utf8ToWide(argv[2], g_logs, MAX_PATH);
        int observed = 0;
        for (int i = 0; i < g_statsProbeCount; i++)
        {
            StatEdit* e = &g_statsProbes[i];
            if (!strcmp(e->kind, "DamageInfo") && e->record == 54 && e->offset == 4)
            { e->value = 150; e->applied = 1; observed++; }
        }
        assert(observed == 1);
        StatsSaveSnapshot();
    }
    OwnAllFixture();
    {
        /* the crash note names modules: an address in kernel32, none for the null page or a heap block */
        char where[160];
        uintptr_t kernel = (uintptr_t)GetModuleHandleW(L"kernel32.dll");
        assert(ModuleAt(kernel + 0x1000, where, sizeof where) && strstr(where, "+0x1000") != NULL && _strnicmp(where, "kernel32.dll", 12) == 0);
        void* heap = malloc(64);
        assert(!ModuleAt(0, where, sizeof where) && !ModuleAt((uintptr_t)heap, where, sizeof where));
        free(heap);
    }
    DifficultyFixture();
    NetSwitchFixture();
    NetworkTypesFixture();
    {
        /* game.dll mapped at the top of the address space (a tester's PC: level 11 refused, "no memory within 2 GB"):
         * nothing fits above it, so the moved tables go below it, still within a signed 32-bit offset */
        uintptr_t top = 0x7FFFFFFF0000ull;
        uint8_t* below = AllocateNearGame(top, 0x3000);
        assert(below != NULL && (uintptr_t)below < top && top - (uintptr_t)below < 0x7FFF0000u);
        VirtualFree(below, 0, MEM_RELEASE);
        /* the space reserved once game.dll is decoded: whole pages of it, in order, each its own to protect; another
         * image (or a full arena) still searches */
        uint8_t* image = (uint8_t*)VirtualAlloc(NULL, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
        uintptr_t game = (uintptr_t)image;
        ReserveNearGame(game);
        assert(g_nearArena != NULL && g_nearGame == game);
        intptr_t offset = (intptr_t)(g_nearArena - image);
        assert(offset > -(intptr_t)0x7FFF0000 && offset + NEAR_ARENA_BYTES < 0x7FFF0000);
        uint8_t* stub = AllocateNearGame(game, 48);
        uint8_t* table = AllocateNearGame(game, 0x1001);
        uint8_t* after = AllocateNearGame(game, 0x100);
        assert(stub == g_nearArena && table == stub + 0x1000 && after == table + 0x2000 && g_nearUsed == 0x4000);
        assert(table[0] == 0 && table[0x1000] == 0);
        DWORD old;
        assert(VirtualProtect(stub, 48, PAGE_EXECUTE_READ, &old));
        table[0] = 1; table[0x1000] = 2; after[0xFF] = 3; /* the pages around a protected stub stay writable */
        uint8_t* elsewhere = AllocateNearGame(top, 0x100);
        assert(elsewhere != NULL && (elsewhere < g_nearArena || elsewhere >= g_nearArena + NEAR_ARENA_BYTES));
        VirtualFree(elsewhere, 0, MEM_RELEASE);
        uint8_t* big = AllocateNearGame(game, NEAR_ARENA_BYTES);
        assert(big != NULL && (big < g_nearArena || big >= g_nearArena + NEAR_ARENA_BYTES) && g_nearUsed == 0x4000);
        VirtualFree(big, 0, MEM_RELEASE);
    }
    StratagemCategoriesFixture();
    LoadoutPreviewFixture();
    puts("PASS stats: shared records, conflicts, guards, own records, moved tables, retained settings, attachment deltas, one-time settings; difficulty 11; "
        "named fields: definition blocks, inferred records, deferred and renewed edits, groups, tags, mounts, refusals, "
        "vanilla edits and conflicts, identities, find, inferred packages; live edits: table rows, waiting tables, replace and reset, "
        "shared records, array elements, pipe requests; mod sync: offers held until published, progress, refusals, cut text, answers over the overlapped pipe; Lua bridge: mods by name, any value of a record, edits stacked editor over Lua over code, Lua holders, saved values, record owners, the event queue; armor passives: record lookup, override kept through kit changes, solo only, "
        "refusals, release, pins; references: package waits, refusals, the stats pass; armor perk values: resolve, live, stats pass, refusals; arm swaps: allowed, refused, package wait, reset; hellpod payloads: pickups, empty, refusals, package wait, reset; row copies: donors, waits, refusals, restore; far hooks: overlay stubs, trampolines, refusals; hook chains: HellForge's and mods' hooks on one function in order, priorities, hooks added later, pins read through a hook, hooks.tsv, code_matches and read_code through another mod's hook; own_all copies: record sizes by shape, own records, idempotence; "
        "game events: priorities, state and mission polling, players by peer, damage hook, the queue; abilities: countdown hold, base from tick 0, refusals, silo countdown flags, the loose remote guard, dead connections closed for a rejoin, silo launches by missile authority only, link marks shared and own, slots freed per frame; behaviours: stubs installed, a new behaviour resumes as its base; spawn director: rosters and compositions by id, weights where level 11 left them, the squad cap and ranges, live and at boot, difficulty copies; per-player shots: the lobby's rules from the pipe, whose round each shot fires; per-player rounds: the lobby's changes, rows rebuilt without this machine's own, the shooter's row and damage at the spawn");
    return 0;
}
