/* Armor perk values (HD2R Editor's Armor perks; HD2Runtime 0.40.1 domains/armor_perk_writes.lua): the bonuses each
 * armor passive gives, edited in place in the customization manager's passive rows, shared by every wearer of the
 * perk. Included by hellforge_modloader.c after passives.c.
 *
 * HellForge names a value as a settings row (perk_data.h, scripts/ExportPerks.py): PassiveModifier (16 bytes: key u32,
 * type u32, value f32 +8) or PassiveStat (12 bytes: stat u32, add f32 +4, mul f32 +8), id = passive * 16 + index. A
 * row is found every time through [game.dll +0x33264F8] (the manager, as in passives.c): +0x20 {pointer to the rows'
 * pointers, u32 count 32, 0}, +0x30 u32[42] passive id -> slot, the passive row (0x34 bytes) checked whole (id, name id,
 * icon, package, both arrays' counts) and the entry's key and type (or stat id) as reviewed; the values must be
 * private read-write data. Nothing is written until the 85 code pins HD2Runtime reviewed read as reviewed. Stat values
 * follow the kit worn at spawn; modifier values are read where the game reads them (not gameplay-tested). */
#include "perk_data.h"

#define PERK_ROW 0x34u
static int g_perksProven;  /* 1 the pins read as reviewed */
static uintptr_t g_perksGame; /* the native fixture's game image (else game.dll) */

static int PerksProve(uintptr_t game)
{
    if (g_perksProven) return 1;
    for (size_t i = 0; i < sizeof kPerkPins / sizeof kPerkPins[0]; i++)
    {
        uint8_t bytes[16];
        if (SafeRead(game + kPerkPins[i].rva, bytes, kPerkPins[i].size) || memcmp(bytes, kPerkPins[i].bytes, kPerkPins[i].size)) return 0;
    }
    return g_perksProven = 1;
}

static const PerkEntry* PerkEntryOf(uint32_t id)
{
    for (size_t i = 0; i < sizeof kPerkEntries / sizeof kPerkEntries[0]; i++) if (kPerkEntries[i].id == id) return &kPerkEntries[i];
    return NULL;
}

static int PerkWritable(uintptr_t at, size_t size)
{
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery((void*)at, &info, sizeof info) && info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && info.Protect == PAGE_READWRITE &&
        at + size <= (uintptr_t)info.BaseAddress + info.RegionSize;
}

/* A passive's row through the manager (its 32 row pointers, the passive's slot); 0 with *ready 0 while it is not there. */
static uintptr_t PerkPassiveRow(uintptr_t game, uint32_t passive, int* ready)
{
    uintptr_t manager = 0, pointers = 0, row = 0;
    uint32_t head[2], slot = 0;
    if (SafeRead(game + PERK_MANAGER, &manager, 8) || !manager) { *ready = 0; return 0; }
    if (SafeRead(manager + PERK_PASSIVES, &pointers, 8) || SafeRead(manager + PERK_PASSIVES + 8, head, 8) || head[0] != PERK_COUNT || head[1] ||
        passive >= PERK_IDS || SafeRead(manager + PERK_INDEX + passive * 4, &slot, 4) || slot >= PERK_COUNT ||
        SafeRead(pointers + slot * 8, &row, 8) || !row) { *ready = 0; return 0; }
    return row;
}

/* Whether a passive row (its PERK_ROW bytes) is the entry's: id, name id, icon, package and both arrays' counts. */
static int PerkRowReviewed(const uint8_t* r, const PerkEntry* e)
{
    uint32_t rid, name, package, mcount, mzero, scount, szero;
    uint64_t icon;
    memcpy(&rid, r, 4); memcpy(&name, r + 4, 4); memcpy(&icon, r + 8, 8); memcpy(&package, r + 0x30, 4);
    memcpy(&mcount, r + 0x18, 4); memcpy(&mzero, r + 0x1C, 4); memcpy(&scount, r + 0x28, 4); memcpy(&szero, r + 0x2C, 4);
    return !(rid != e->id || name != e->nameId || icon != e->icon || package != e->package || mcount != e->modifiers || mzero ||
        scount != e->stats || szero);
}

/* The row's modifier or stat at index: its key and type (or stat id) as reviewed, its values private read-write data;
 * else 0. */
static uintptr_t PerkItem(const uint8_t* r, const PerkEntry* e, int modifier, uint32_t index)
{
    uintptr_t items = 0;
    memcpy(&items, r + (modifier ? 0x10 : 0x20), 8);
    uint32_t stride = modifier ? 16 : 12;
    uintptr_t at = items + index * stride;
    uint32_t item[2];
    const uint32_t* want = kPerkItems[e->first + (modifier ? index : e->modifiers + index)];
    if (!items || SafeRead(at, item, 8) || item[0] != want[0] || (modifier && item[1] != want[1])) return 0;
    if (!PerkWritable(at, stride)) return 0;
    return at;
}

/* A perk value's entry (its modifier or stat), or 0 with *ready 0 while the manager is not there yet. */
static uintptr_t PerkRow(const char* kind, uint64_t id, int* ready)
{
    *ready = 1;
    int modifier = !strcmp(kind, "PassiveModifier");
    uint32_t passive = (uint32_t)(id / 16), index = (uint32_t)(id % 16);
    const PerkEntry* e = PerkEntryOf(passive);
    if (e == NULL || index >= (modifier ? e->modifiers : e->stats)) return 0;
    uintptr_t game = g_perksGame ? g_perksGame : ApiModuleBase("game.dll", NULL);
    if (!game || strcmp(g_build, "25480438")) return 0;
    if (!PerksProve(game)) { *ready = 0; return 0; }
    uintptr_t row = PerkPassiveRow(game, passive, ready);
    if (!row) return 0;
    uint8_t r[PERK_ROW];
    if (SafeRead(row, r, sizeof r) || !PerkRowReviewed(r, e)) return 0;
    return PerkItem(r, e, modifier, index);
}

/* Whether a stats edit or an observation names an armor perk value. */
static int StatsPerksNamed(void)
{
    int any = 0;
    for (int i = 0; i < g_statCount; i++) if (!strncmp(g_stats[i].kind, "Passive", 7)) any = 1;
    for (int i = 0; i < g_statsProbeCount && !any; i++) if (!strncmp(g_statsProbes[i].kind, "Passive", 7)) any = 1;
    return any;
}

static void StatsPerkEdit(StatEdit* e, int* ready)
{
    if (strncmp(e->kind, "Passive", 7) || !e->mod->loaded) return;
    uint32_t stride = !strcmp(e->kind, "PassiveModifier") ? 16 : 12;
    uintptr_t at = e->offset >= 4 && e->offset <= stride - 4 && !e->own && e->record >= 0 ? PerkRow(e->kind, (uint64_t)e->record, ready) : 0;
    if (!at) StatsRefuse(e, "the armor perk's entry is not as reviewed");
    else StatsWrite(e, at + e->offset);
}

/* The stats pass's perk edits, once the passive rows are readable (refused if they never are within 120 s of the
 * entity data). The rows are the game's own data, so they stay edited for the session. */
static void StatsPerksReady(void)
{
    static ULONGLONG first;
    if (g_perksDone) return;
    if (!StatsPerksNamed()) return;
    ULONGLONG now = GetTickCount64();
    if (!first) first = now;
    int ready = 0;
    PerkRow("PassiveModifier", 1 * 16, &ready); /* Extra Padding's armor rating: is the manager there? */
    if (!ready && now - first < 120000) return;
    g_perksDone = 1;
    for (int i = 0; i < g_statCount; i++) StatsPerkEdit(&g_stats[i], &ready);
}
