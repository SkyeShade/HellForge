/* Attachment cap probe (in-game test only, never shipped; tests/in-game/attachment-cap.json): room for 1,024 attachments.
 * Adds 640 copies of the Liberator's drum magazine, past the old 128, on three AR-23 Liberator copies (213 or 214 on
 * each). On the ship, once the loader has its item block in the game's table, the game's own item lookup
 * (AttachmentItemById) and delta lookup (EntityDelta) must find the 1st, the 128th, the 129th and the 640th. It then puts
 * the 640th on the first copy and that copy in the primary slot, as the armory leaves a pick (game build 25480438):
 *   - the profile P = [game + 0x347CDD8]: the primary's loadout id at +0x40 (native/mod-loader/loadout_scrub.c)
 *   - the customization records at [game + 0x347CEF8] + 0x178A4EC, 512 of 0x94 bytes: +0 the weapon, +8 its template
 *     (0 to 2), +0xC three templates of 10 option ids by slot (BuildCustomizationList 0x14939A1, 0x17516E1)
 *   - the local player's PlayerHistory loadout (loadout_scrub.c): the primary at +0, its slot numbers at +4 and option
 *     ids at +0x2C, 10 of each; the magazine is slot 5 (0x1751632 fills it from the profile and the record)
 * Every attachment's add_path must map to the drum's package in the game's package map (hash_lookup, +0x3799810),
 * which the weapon loads for it. In the mission, the equipped set the game builds for a weapon from those ids (0x1366E50: {u32 count, u64 add_path[]},
 * each id looked up by AttachmentItemById) must hold the 640th's add_path, and the Helldiver must hold the copy.
 * "PROBE <check> ok" or "PROBE <check> FAIL <why>" per check. */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hellforge_mod.h"

#define COUNT 640u
#define WEAPONS 3u
#define DRUM 0xDF4F1322u /* "Rifle 5,5x50mm. Drum", a Liberator magazine (slot 5); its add_path 0xE8DE5E95B9635C21 */
#define DRUM_PATH 0xE8DE5E95B9635C21ull
#define MAGAZINE_SLOT 5u

#define PROFILE 0x347CDD8u
#define PROGRESSION 0x347CEF8u
#define RECORDS 0x178A4ECu
#define RECORD 0x94u
#define RECORD_COUNT 512u
#define HISTORY 0x347CE50u
#define PEER 0x347CEF0u
#define HISTORY_RECORDS 0x2D200u
#define HISTORY_RECORD 0x1690u
#define HISTORY_LOADOUT 0x38u
#define EQUIPPED 0x1366E50u
#define ITEM_PACKAGES 0x3799810u /* hash_lookup: add_path -> package (native/mod-loader/patterns.c PAT_PACKAGES) */

/* The bytes this probe reads its layout from (game.dll 25480438). */
static const struct { uint32_t rva; const char* code; } kPins[] = {
    {0x14939A1u, "48 8B 2D 50 95 FE 01 33 C0 4C 8D 85 EC A4 78 01 49 8B C8 39 11 74 20 FF C0 48 81 C1 94 00 00 00 3D 00 02 00 00"},
    {0x17516E1u, "49 63 50 08"},                            /* the record's template */
    {0x1751632u, "41 8B 4E 40 89 0B 41 8B 46 44 89 43 60"}, /* the profile's primary into the loadout's */
    {0x1366FA0u, "8B 4E 04 8D 41 FF 83 F8 08 77 28 C6 44 0D 37 01 8B 4E 2C 85 C9 74 1C E8"}, /* a slot at +4, its id at +0x2C */
    {EQUIPPED, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24 A9 48 81 EC 90 00 00 00"},
};

static HfMod* g_mod;
static const HfApi* hf;
#define LOG(...) hf->log(g_mod, __VA_ARGS__)
static uint32_t g_weapons[WEAPONS], g_ids[COUNT];
static uint64_t g_paths[COUNT], g_entity;
static int g_proven, g_found, g_set, g_held, g_inMission;
static volatile LONG g_builds, g_built;

typedef uint32_t* (*EquippedFn)(void* system, int64_t peer, int64_t weapon);
static EquippedFn g_equipped;

static uintptr_t Pointer(uintptr_t at) { uintptr_t v = 0; return at != 0 && hf->read(at, &v, 8) == 0 ? v : 0; }
static uint32_t U32(uintptr_t at) { uint32_t v = 0; return at != 0 && hf->read(at, &v, 4) == 0 ? v : 0xFFFFFFFFu; }

/* The local player's loadout in PlayerHistory, or 0. */
static uintptr_t LocalLoadout(uintptr_t game)
{
    uintptr_t history = Pointer(game + HISTORY), peerHolder = Pointer(game + PEER);
    if (history == 0 || peerHolder == 0) return 0;
    uint64_t peer = 0;
    if (hf->read(peerHolder + 0xB398, &peer, 8) || peer == 0) return 0;
    uint32_t records = U32(history + HISTORY_RECORDS);
    for (uint32_t r = 0; r < records && r < 32; r++)
    {
        uint64_t id = 0;
        if (!hf->read(history + (uintptr_t)r * HISTORY_RECORD, &id, 8) && id == peer) return history + (uintptr_t)r * HISTORY_RECORD + HISTORY_LOADOUT;
    }
    return 0;
}

/* The package the game's hash_lookup map gives an add_path, or 0 (0xFD25A0's probe); for the log. */
static uint64_t PackageOf(uintptr_t game, uint64_t key)
{
    uintptr_t entries = Pointer(game + ITEM_PACKAGES);
    uint32_t count = U32(game + ITEM_PACKAGES + 8), multiplier = U32(game + ITEM_PACKAGES + 0x18);
    uint64_t empty = 0;
    if (entries == 0 || count == 0 || (count & (count - 1)) || hf->read(game + ITEM_PACKAGES + 0x10, &empty, 8)) return 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint64_t entry[2];
        if (hf->read(entries + 16u * ((i + (uint64_t)multiplier * key) & (count - 1)), entry, 16)) return 0;
        if (entry[0] == key) return entry[1];
        if (entry[0] == empty) return 0;
    }
    return 0;
}

/* The game's equipped set for a weapon, watched for the last attachment's add_path. */
static uint32_t* EquippedDetour(void* system, int64_t peer, int64_t weapon)
{
    uint32_t* set = g_equipped(system, peer, weapon);
    if (set == NULL || !g_inMission) return set;
    uint32_t count = set[0];
    int has = 0;
    char line[256] = "";
    for (uint32_t i = 0; i < count && i < 10; i++)
    {
        uint64_t path;
        memcpy(&path, (const uint8_t*)set + 8 + 8u * i, 8);
        has |= path == g_paths[COUNT - 1];
        snprintf(line + strlen(line), sizeof line - strlen(line), " %016llx", (unsigned long long)path);
    }
    if (InterlockedIncrement(&g_builds) <= 8) LOG("PROBE equipped set of %016llx: %u path(s):%s", (unsigned long long)weapon, count, line);
    if (has && InterlockedExchange(&g_built, 1) == 0)
        LOG("PROBE built ok: weapon %016llx is built with attachment %08x (add_path %016llx), %u path(s)", (unsigned long long)weapon, g_ids[COUNT - 1],
            (unsigned long long)g_paths[COUNT - 1], count);
    return set;
}

static void GameLoaded(HfMod* mod, HfEvent event, void* user)
{
    (void)event; (void)user;
    uintptr_t game = hf->module_base("game.dll", NULL);
    if (game == 0) { LOG("PROBE pins FAIL no game.dll"); return; }
    for (size_t i = 0; i < sizeof kPins / sizeof kPins[0]; i++)
        if (!hf->code_matches(game + kPins[i].rva, kPins[i].code)) { LOG("PROBE pins FAIL game.dll+0x%X is not the reviewed code", kPins[i].rva); return; }
    if (hf->hook(mod, game + EQUIPPED, (void*)EquippedDetour, (void**)&g_equipped) != 0) { LOG("PROBE hook FAIL the equipped set builder is not hooked"); return; }
    g_proven = 1;
    LOG("PROBE pins ok: the loadout, the customization records and the equipped set builder read as reviewed");
}

/* The 1st, 128th, 129th and 640th through the game's own lookups: 1 all found, 0 not in yet, -1 failed (logged). */
static int Lookups(void)
{
    HfFn_AttachmentItemById item = hf_fn_AttachmentItemById(g_mod);
    HfFn_EntityDelta delta = hf_fn_EntityDelta(g_mod);
    if (item == NULL || delta == NULL) { LOG("PROBE lookup FAIL the game's lookups are not mapped"); return -1; }
    static const uint32_t probes[] = {0, 127, 128, COUNT - 1};
    for (size_t k = 0; k < sizeof probes / sizeof probes[0]; k++)
    {
        const uint8_t* row = item(g_ids[probes[k]]);
        uint32_t id = 0;
        uint64_t path = 0;
        if (row == NULL || hf->read((uintptr_t)row + 8, &id, 4) || hf->read((uintptr_t)row + 0x20, &path, 8)) return 0; /* not in yet */
        if (id != g_ids[probes[k]] || path != g_paths[probes[k]])
        { LOG("PROBE lookup FAIL attachment %u: item %08x with add_path %016llx", probes[k] + 1, id, (unsigned long long)path); return -1; }
        if (delta(path) == NULL) { LOG("PROBE lookup FAIL attachment %u (%08x) has no entity deltas", probes[k] + 1, id); return -1; }
    }
    LOG("PROBE lookup ok: attachments 1, 128, 129 and %u (%08x) are the game's items, with entity deltas", COUNT, g_ids[COUNT - 1]);
    return 1;
}

/* The last attachment on the first copy, and that copy the primary: its customization record, the profile and the
 * loadout, as the armory leaves them. 1 done (or failed, logged), 0 not yet. */
static int Equip(uintptr_t game)
{
    uintptr_t progression = Pointer(game + PROGRESSION), profile = Pointer(game + PROFILE), loadout = LocalLoadout(game);
    if (progression == 0 || profile == 0 || loadout == 0) return 0;
    uint32_t weapon = g_weapons[0], last = g_ids[COUNT - 1];
    uintptr_t record = 0;
    for (uint32_t r = 0; r < RECORD_COUNT && record == 0; r++)
        if (U32(progression + RECORDS + (uintptr_t)r * RECORD) == weapon) record = progression + RECORDS + (uintptr_t)r * RECORD;
    int made = record == 0;
    for (uint32_t r = 0; r < RECORD_COUNT && record == 0; r++)
        if (U32(progression + RECORDS + (uintptr_t)r * RECORD) == 0) record = progression + RECORDS + (uintptr_t)r * RECORD;
    if (record == 0) { LOG("PROBE loadout FAIL no customization record is free"); return 1; }
    uint32_t zero = 0, slot = MAGAZINE_SLOT, template = made ? 0 : U32(record + 8);
    if (template > 2) template = 0;
    if ((made && (hf->write(record, &weapon, 4) || hf->write(record + 8, &zero, 4))) ||
        hf->write(record + 0xC + 0x28u * template + 4u * MAGAZINE_SLOT, &last, 4) || hf->write(profile + 0x40, &weapon, 4) ||
        hf->write(loadout, &weapon, 4) || hf->write(loadout + 4 + 4u * MAGAZINE_SLOT, &slot, 4) || hf->write(loadout + 0x2C + 4u * MAGAZINE_SLOT, &last, 4))
    { LOG("PROBE loadout FAIL could not write it"); return 1; }
    LOG("PROBE loadout ok: primary %08x with attachment %08x in slot %u (%s customization record, template %u)", U32(loadout),
        U32(loadout + 0x2C + 4u * MAGAZINE_SLOT), U32(loadout + 4 + 4u * MAGAZINE_SLOT), made ? "a new" : "its", template);
    uint64_t drumPackage = PackageOf(game, DRUM_PATH), lastPackage = PackageOf(game, g_paths[COUNT - 1]), firstPackage = PackageOf(game, g_paths[0]);
    LOG("PROBE packages %s: the drum's add_path maps to package %016llx, the 1st attachment's to %016llx, the last one's to %016llx",
        drumPackage != 0 && lastPackage == drumPackage && firstPackage == drumPackage ? "ok" : "FAIL", (unsigned long long)drumPackage,
        (unsigned long long)firstPackage, (unsigned long long)lastPackage);
    return 1;
}

/* On the ship, logged in: the lookups (again each frame until the loader's block is in), then the loadout once. */
static void Tick(HfMod* mod, float dt, void* user)
{
    (void)mod; (void)dt; (void)user;
    if (!g_proven || g_set || g_found < 0 || hf->game_state(g_mod) != HF_STATE_SHIP) return;
    uintptr_t game = hf->module_base("game.dll", NULL);
    uintptr_t manager = Pointer(game + PROGRESSION);
    if (game == 0 || manager == 0 || U32(manager + 0xDDCF8) != 12) return; /* the login (loadout_scrub.c) */
    if (g_found == 0) g_found = Lookups();
    if (g_found > 0) g_set = Equip(game);
}

static void MissionStarted(HfMod* mod, const HfStateEvent* state, void* user)
{
    (void)mod; (void)state; (void)user;
    g_inMission = 1;
    uintptr_t game = hf->module_base("game.dll", NULL), loadout = game ? LocalLoadout(game) : 0;
    if (g_found == 0) LOG("PROBE lookup FAIL the attachments were not found before the mission");
    LOG("PROBE mission started: primary %08x, magazine %08x%s", U32(loadout), U32(loadout + 0x2C + 4u * MAGAZINE_SLOT), g_set ? "" : " FAIL without the loadout set");
}

static void MissionEnded(HfMod* mod, const HfStateEvent* state, void* user)
{
    (void)mod; (void)state; (void)user;
    if (!g_inMission) return;
    g_inMission = 0;
    LOG("PROBE mission ended: %ld equipped set(s) built; the attachment in one: %s; the copy held: %s", (long)g_builds, g_built ? "yes" : "FAIL no",
        g_held ? "yes" : "FAIL no");
}

static void Weapon(HfMod* mod, const HfWeaponEvent* w, void* user)
{
    (void)mod; (void)user;
    if (!g_inMission || w->resource == 0) return;
    if (w->resource == g_entity && !g_held) { g_held = 1; LOG("PROBE holds ok: the copy carrying attachment %u (%016llx)", COUNT, (unsigned long long)w->resource); }
    else if (w->resource != g_entity) LOG("PROBE holds %016llx (selection %u)", (unsigned long long)w->resource, w->selection);
}

static void EntitiesReady(HfMod* mod, HfEvent event, void* user)
{
    (void)mod; (void)event; (void)user;
    g_entity = hf->weapon_entity(g_weapons[0]);
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hf = api;
    g_mod = mod;
    if (api->version < 17 || api->size < offsetof(HfApi, add_attachment) + sizeof(void*))
    { api->log(mod, "PROBE sdk FAIL the loader is SDK %u", api->version); return 1; }
    LOG("PROBE sdk ok (SDK %u)", api->version);
    static char units[WEAPONS][64], names[WEAPONS][32];
    for (uint32_t w = 0; w < WEAPONS; w++)
    {
        snprintf(units[w], sizeof units[w], "hellforge/attachment_cap_probe/liberator_%u", w);
        snprintf(names[w], sizeof names[w], "Cap Liberator %c", 'A' + (char)w);
        HfWeapon copy = {.size = sizeof copy};
        copy.base = HF_ITEM_AR23_LIBERATOR;
        copy.unit = units[w];
        copy.designation = "AR-23C";
        copy.name = names[w];
        g_weapons[w] = api->add_weapon(mod, &copy);
    }
    if (!g_weapons[0] || !g_weapons[1] || !g_weapons[2]) { LOG("PROBE weapons FAIL %08x %08x %08x", g_weapons[0], g_weapons[1], g_weapons[2]); return 0; }
    uint32_t added = 0;
    for (uint32_t i = 0; i < COUNT; i++)
    {
        char name[64], text[32], key[96];
        snprintf(name, sizeof name, "hellforge/attachment_cap_probe/drum_%03u", i + 1);
        snprintf(text, sizeof text, "Cap Drum %u", i + 1);
        snprintf(key, sizeof key, "hellforge/attachment/%s", name);
        HfAttachment drum = {.size = sizeof drum};
        drum.base = DRUM;
        drum.name = name;
        drum.name_text = text;
        drum.weapons = &g_weapons[i % WEAPONS];
        drum.weapon_count = 1;
        g_ids[i] = api->add_attachment(mod, &drum);
        g_paths[i] = api->resource_hash(key); /* the loader's add_path for an attachment without a unit of its own */
        added += g_ids[i] != 0;
    }
    if (added == COUNT) LOG("PROBE added ok: %u attachments on %u weapons, the last %08x on %08x", added, WEAPONS, g_ids[COUNT - 1], g_weapons[(COUNT - 1) % WEAPONS]);
    else LOG("PROBE added FAIL %u of %u attachments (%s)", added, COUNT, api->last_log(mod));
    api->on(mod, HF_EVENT_GAME_LOADED, GameLoaded, NULL);
    api->on(mod, HF_EVENT_ENTITIES_READY, EntitiesReady, NULL);
    LOG(hf_on_game_thread(mod, Tick, NULL) == 0 ? "PROBE game_thread ok" : "PROBE game_thread FAIL refused"); /* the hf_ wrapper: 0 on success */
    hf_on_mission_start(mod, MissionStarted, NULL, 0);
    hf_on_mission_end(mod, MissionEnded, NULL, 0);
    hf_on_weapon(mod, Weapon, NULL, 0);
    return 0;
}
