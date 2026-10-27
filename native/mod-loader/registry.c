/* The weapon registry (SDK 2, docs/modding/content.md "Weapons"): new arsenal weapons copied from existing ones, added
 * to the game's entity data in memory right after game.dll loads it (generated_entities.dl_bin, loader 0xFDB440; the
 * file is never changed, so the game's resource check passes). Included by hellforge_modloader.c.
 *
 * game.dll keeps pointers into the loaded data in its entity state ([game.dll + 0x346BF98]):
 *   +0xF12EA0  entity map: 4096 rows of {u64 entity, u16* components, u64 component count, u32 short id, u32 0},
 *              open addressing from entity % 4096 (lookup 0x4F2E70);
 *   +0xF12EA8  short ids: sorted {u32 short id, u32 map row} (28-byte LDLD header before it, byte size at -12); an
 *              entity's index here identifies its type in the engine (0x7C83B0);
 *   +0xF12478 + 8*c  component table c: rows {u64 entity, u32 record, u32 0}, open addressing from entity % slots (the
 *              slot count is compiled into each lookup: kSlots), then the records.
 * A registered weapon is a new entity named after its unit resource (the mod ships that unit). It shares every
 * component record of its base weapon except its loadout entry (component 29, LoadoutEntryComponentData: {u32 id,
 * u32 type, u32, u32 traits[5]}) and its unit (component 88, UnitComponentData: u64 unit resource at +0, 136-byte
 * records), plus any components the mod names (HfWeapon.own_components), which get records of their own in HellForge's
 * larger copies of those tables (OwnRecord: the copy has room after every vanilla record, and the weapon's index row
 * names the new record; component_record finds them through RegisteredRecord); its short id is appended after
 * every vanilla one so no vanilla index moves. The armory's list (0x9F1CC0 builds it from component 29 into a fixed
 * 193-entry object; every reader uses its per-type starts at +0x1B24 and counts at +0x1B40) is rebuilt by HellForge into a
 * larger object whose entries start after those fields, and the game's pointer to it ([game.dll + 0x33269E8], set once
 * at 0x5683B5) is pointed there. The registered weapons are written to <session>/registered-items.json for the
 * backend's item catalogue and inventory. A weapon given text (HfWeapon.name) also gets its own record in component 76
 * (the encyclopedia: 56-byte records with text ids at +0 name, +0x08 upper-case name, +0x14 description, +0x24
 * designation; the armory shows designation + upper-case name, HD2Runtime game-text-api) naming text ids of its own, and
 * the engine's text function answers those ids with the mod's strings: its slot in the engine API table the game calls
 * ([[game.dll + 0x3326308] + 0x10] + 1000: const char* (u32 id), in helldivers2.exe's data) is pointed at HellForge's, which
 * passes every other id on. The slot, not game.dll's wrapper (0x17802E0), as the armory calls the engine directly.
 * The engine's network configuration ([[helldivers2.exe + 0x1A10268]], the plan's network_config) has the same entity
 * types, index-aligned with the short ids: arrays {u32 count, u32 capacity, data, allocator} at +0x28 (u32 own index),
 * +0x40 (a byte per type, zero) and +0x70 (80-byte entries {u32 short id, ...} sorted by short id, binary-searched by the engine,
 * exe 0x34B700: a missing type crashed the game when the weapon was equipped). Each registered weapon gets a copy of its
 * base's entry under its own short id, appended (RegisterNetworkTypes), with the two arrays an entry owns copied too
 * (CopyNetworkEntry). Short ids, and with them the network types, are given
 * in the order of the entities' resource ids, not the mods' load order (WeaponOrder): a joining machine builds the
 * entity type another names by number, so friends who load the same mods in another order must number them alike
 * (2026-10-18: a friend's Pelican arrived on the others' machines as another mod's entity, sitting on the ground).
 * The armory's customization screen (list builder 0x1493770) finds a weapon's mastery package through its
 * WeaponCustomizationComponentData record (component 271, 4872-byte records, package id32 at +0x12FC, read by
 * 0x11F4F00) and matches the package's unlocks to the weapon's attachments by WeaponChild(attachment, weapon id); with
 * the base's record no unlock matched the new id and the screen crashed sorting empty entries (0x1493700). So every
 * weapon gets its own record naming its own package, MasteryPackage(base package), the id the backend gives the copy of
 * the base's package it serves (src/HellForge.LocalBackend/RegisteredItems.cs, the same derivation).
 * A weapon given a projectile (HfWeapon.projectile) gets its own ProjectileWeaponComponentData record (component 321,
 * 616-byte records) naming it at +0 (ProjType), and its customization record loses its default ammunition (slot 6):
 * the record starts with up to 10 default {u32 slot, u32 option} pairs (+0x00..+0x4F; the slots the player may change
 * follow at +0x50, and ammunition is never one of them), and an ammunition option's entity delta sets ProjType again
 * when the weapon is assembled (the Liberator's "5,5x50mm Full Metal Jacket" sets 276; HD2Runtime
 * active-projectile-sources). Without it the weapon fires what its record names, as weapons without ammunition do.
 * A weapon given another weapon's sounds (HfWeapon.sound_from) takes that weapon's firing-sound members into its own
 * ProjectileWeapon record (HD2Runtime weapon-sounds: +237 MIDI flag, +240..+283 MIDI timing, loop start/stop, single
 * shot (+260), fire mode 4/7 and secondary events, +528..+539 silenced events, +556 silenced flag). The engine frees those arrays through their allocator
 * (a 64 KiB-page allocator that reads its page header to size a block: a VirtualAlloc block crashed it at shutdown,
 * exe 0x630146), so the larger ones come from that allocator (vtable slot 7, exe 0x58EC0: void* (size, align)). Values are build 25480438's (game.dll 2E2C3B7C...). */

#define REG_STATE_GLOBAL 0x346BF98u
#define REG_ENTITY_LOADER 0xFDB440u
#define REG_ARMORY_BUILDER 0x9F1CC0u
#define REG_ARMORY_GLOBAL 0x33269E8u
#define REG_TABLES 0xF12478u
#define REG_MAP 0xF12EA0u
#define REG_LIST 0xF12EA8u
#define REG_MAP_SLOTS 4096u
#define REG_LOADOUT 29u
#define REG_LOADOUT_SLOTS 386u
#define REG_LOADOUT_RECORDS 193u
#define REG_LOADOUT_STRIDE 32u
#define REG_ARMORY_FIRST 219u /* the first entry after the armory list's count fields (0x1B24..0x1B5C) */
#define REG_ARMORY_TYPES 7
/* Weapons and entities mods add, together. Each needs a network type, and the game sends a type in 11 bits: build
 * 25480438 has 1855 (NET_VANILLA_TYPES), so 193 more at most. */
#define REG_MAX_WEAPONS 192
#define REG_UNIT 88u
#define REG_UNIT_STRIDE 136u
#define REG_MAX_OWNED 192 /* component tables HellForge keeps its own copies of (an own_all entity: every one it has) */
#define REG_MAX_OWN 8     /* components a weapon may own beyond its unit, text and loadout entry */
#define REG_TEXT_COMPONENT 76u
#define REG_TEXT_STRIDE 56u
#define REG_ENGINE_API 0x3326308u
#define REG_CUSTOMIZATION 271u
#define REG_CUSTOMIZATION_STRIDE 4872u
#define REG_MASTERY_PACKAGE_AT 0x12FCu
#define REG_DEFAULT_PAIRS 10
#define REG_AMMUNITION_SLOT 6u
#define REG_PROJECTILE_WEAPON 321u
#define REG_PROJECTILE_WEAPON_STRIDE 616u
#define REG_BEHAVIOUR 284u
#define REG_BEHAVIOUR_STRIDE 12u /* BehaviorComponentData: +0 behaviour id, +4 task cooldown, +8 task user */

static const uint8_t kLoaderPin[16] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
static const uint8_t kBuilderPin[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x7c, 0x24, 0x10, 0x48, 0x8b, 0x05, 0xc7, 0xa2, 0xa7};

/* Index slot counts of component tables, from the modulo constant in each table's inlined lookup (game.dll scan; 70
 * and 91 divide by multiply-shift, 0x4F7C70 and 0x4FA390, which the scan missed). The rest
 * (from {10, 6} on) from a scan of every table's lookup loop bound (`cmp reg, slots; jb` after the table's load; 2026-10-18),
 * which agrees with every count above and finds each one among the component catalogue's table sizes: the barrage's
 * bombardment (95) and the silo missile (219) were refused for want of theirs. */
typedef struct SlotCount { uint16_t component, slots; } SlotCount;
static const SlotCount kSlots[] = {
    {0, 152}, {1, 114}, {3, 1122}, {5, 540}, {8, 38}, {12, 598}, {15, 58}, {22, 30}, {23, 154}, {25, 14}, {28, 1080},
    {29, 386}, {32, 324}, {35, 258}, {44, 14}, {47, 26}, {50, 298}, {55, 22}, {59, 350}, {61, 26}, {62, 42}, {64, 68},
    {69, 332}, {70, 18}, {76, 1330}, {77, 316}, {79, 118}, {81, 26}, {84, 176}, {86, 38}, {87, 232}, {88, 3382}, {90, 1056},
    {91, 40}, {92, 434},
    {93, 26}, {94, 30}, {96, 44}, {101, 150}, {102, 34}, {103, 594}, {104, 1376}, {105, 948}, {106, 180}, {107, 712},
    {110, 224}, {111, 1010}, {113, 498}, {117, 50}, {120, 150}, {121, 44}, {122, 180}, {124, 60}, {125, 1386}, {129, 310},
    {132, 28}, {133, 22}, {137, 378}, {138, 52}, {146, 114}, {147, 52}, {149, 44}, {150, 42}, {152, 22}, {155, 140},
    {156, 146}, {157, 50}, {158, 1520}, {159, 388}, {160, 240}, {161, 30}, {164, 230}, {174, 14}, {175, 66}, {177, 58},
    {182, 66}, {183, 116}, {184, 90}, {185, 14}, {188, 572}, {191, 626}, {192, 370}, {193, 44}, {196, 58}, {197, 74},
    {198, 544}, {203, 14}, {206, 188}, {207, 494}, {210, 42}, {211, 34}, {212, 38}, {218, 494}, {223, 126}, {224, 1002},
    {225, 28}, {230, 418}, {231, 26}, {233, 688}, {234, 590}, {235, 438}, {236, 730}, {241, 172}, {244, 1114}, {247, 38},
    {248, 146}, {253, 22}, {255, 142}, {257, 182}, {259, 92}, {260, 34}, {261, 28}, {262, 1606}, {263, 54}, {266, 58},
    {268, 22}, {270, 46}, {271, 382}, {273, 386}, {275, 30}, {279, 28}, {280, 390}, {282, 638}, {283, 324}, {284, 2008},
    {285, 46}, {286, 112}, {288, 208}, {289, 22}, {290, 1968}, {296, 120}, {297, 76}, {298, 46}, {299, 54}, {302, 316},
    {303, 398}, {304, 42}, {305, 42}, {307, 648}, {309, 122}, {310, 448}, {311, 408}, {315, 46}, {318, 472}, {321, 542},
    {322, 454},
    {10, 6}, {11, 10}, {18, 8}, {24, 16}, {31, 8}, {36, 4}, {45, 4}, {60, 12}, {68, 12}, {73, 6}, {74, 6}, {75, 18},
    {80, 18}, {85, 24}, {89, 96}, {95, 32}, {97, 12}, {98, 6}, {99, 6}, {108, 72}, {109, 4}, {114, 6}, {123, 16},
    {127, 4}, {130, 16}, {131, 32}, {135, 10}, {139, 4}, {140, 4}, {143, 4}, {144, 4}, {145, 12}, {153, 6}, {154, 4},
    {166, 8}, {168, 8}, {169, 10}, {173, 10}, {176, 16}, {179, 8}, {186, 6}, {187, 4}, {190, 80}, {195, 8}, {199, 16},
    {201, 20}, {202, 6}, {204, 20}, {205, 16}, {215, 4}, {216, 4}, {219, 18}, {221, 12}, {222, 4}, {228, 4}, {229, 12},
    {242, 64}, {245, 18}, {246, 12}, {251, 6}, {252, 6}, {258, 10}, {264, 6}, {265, 20}, {267, 18}, {272, 6}, {281, 4},
    {291, 8}, {293, 4}, {300, 36}, {301, 16}, {308, 4}, {313, 10}, {314, 12}, {317, 6}, {320, 20}, {323, 20}};

#include "component_sizes.h" /* kComponentShapes: every table's slots, records and record size (ComponentStride) */

typedef struct Weapon
{
    HfMod* mod;
    uint32_t base, id;
    uint64_t entity;
    int plain;           /* an entity of add_entity: no loadout record, armory entry, catalogue item or text */
    uint64_t baseEntity; /* add_entity's base */
    uint64_t unit;       /* add_entity: a unit resource of its own, or 0 for the base's */
    uint32_t behaviour;  /* add_entity: the behaviour it runs, or 0 for the base's */
    uint64_t modelFrom;  /* add_entity: the entity whose model (and model-tied records) it takes, or 0 */
    char name[192];
    int state; /* 0 pending, 1 registered, -1 refused */
    uint32_t text[4]; /* text ids of its own: name, upper-case name, description, designation (0: the base's) */
    uint32_t shortId, baseShortId;
    uint32_t projectile, baseProjectile;   /* HfWeapon.projectile, and what its base fires */
    uint32_t soundFrom;                    /* HfWeapon.sound_from */
    uint8_t* projectileRecord;             /* its own ProjectileWeapon record, when it has a projectile */
    int ownCount;
    uint32_t own[REG_MAX_OWN], ownStride[REG_MAX_OWN]; /* components it owns records of (HfWeapon.own_components) */
    int ownAll;          /* add_entity: a record of its own in every component of its base (HfEntity.own_all) */
    uint64_t copies;     /* the entity it was registered as a copy of (its map row's components are that entity's) */
    char title[96];      /* add_entity: its title (HfEntity.title), or "" */
    char designation[64]; /* add_entity: its designation (HfEntity.designation), or "" */
} Weapon;

/* Mod text by id, answered by the text lookup detour. */
typedef struct ModText { uint32_t id; char* text; } ModText;
static ModText g_texts[REG_MAX_WEAPONS * 4];
static int g_textCount;
static int g_textHooked;
static volatile LONG g_networkTypesAdded; /* 0, 1 being added, 2 done */
/* The network types RegisterNetworkTypes added, as the configuration numbered them, with the vanilla type each copies
 * and the two entities (netapply.c gives each its base's case in the network apply switch). */
typedef struct NetworkType { uint32_t index, base; uint64_t entity, baseEntity; const char* name; HfMod* mod; } NetworkType;
static NetworkType g_networkTypes[REG_MAX_WEAPONS];
static uint32_t g_networkTypeCount, g_networkVanillaTypes;

typedef void (*EntityLoaderFn)(void* state);
typedef void (*ArmoryBuilderFn)(void* list);

static Weapon g_weapons[REG_MAX_WEAPONS];
static int g_weaponCount;

/* The registered weapons and entities by resource id (hashed from their unit names): an order every machine with the
 * same mods shares, whatever order it loads them in. */
static void WeaponOrder(int order[REG_MAX_WEAPONS])
{
    for (int i = 0; i < g_weaponCount; i++) order[i] = i;
    for (int i = 1; i < g_weaponCount; i++)
        for (int j = i; j > 0 && g_weapons[order[j - 1]].entity > g_weapons[order[j]].entity; j--)
        {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
}

static uintptr_t g_game;
static uint8_t* g_loadoutTable; /* HellForge's copy of component 29 (pointer to its index, as the game's) */
static uint32_t g_loadoutRecords;
static uint8_t* g_armory;
/* HellForge's copies of component tables that registered weapons have records of their own in (besides component 29). */
typedef struct OwnedTable { uint32_t component, stride, first, next, end; uint8_t* index; } OwnedTable;
static OwnedTable g_owned[REG_MAX_OWNED];
static int g_ownedCount;
static EntityLoaderFn g_entityLoader;
static ArmoryBuilderFn g_armoryBuilder;
static int g_registryHooked;
static int g_registryActive; /* the hooks are in: weapons get added when the entity data loads */
static int ProjectileUsable(uint32_t type); /* projectiles.c */
static int BehaviourUsable(uint32_t id);    /* behaviours.c */
static void RememberUnitCopy(HfMod* mod, const char* name, uint64_t base, uint64_t copy); /* registry_load.c: unit copies */

static uint32_t SlotsOf(uint32_t component)
{
    for (size_t i = 0; i < sizeof kSlots / sizeof kSlots[0]; i++)
        if (kSlots[i].component == component) return kSlots[i].slots;
    for (int i = 0; i < g_componentCount; i++)
        if (g_components[i].index == component) return g_components[i].indices;
    return 0;
}

/* Open-addressing tables keyed by a u64 at the start of each row. */
static uint8_t* FindRow(uint8_t* rows, uint32_t slots, uint32_t stride, uint64_t key)
{
    uint32_t at = (uint32_t)(key % slots);
    for (uint32_t i = 0; i < slots; i++)
    {
        uint64_t value;
        memcpy(&value, rows + (size_t)at * stride, 8);
        if (value == key) return rows + (size_t)at * stride;
        if (value == 0) return NULL;
        at = at + 1 == slots ? 0 : at + 1;
    }
    return NULL;
}

static uint8_t* FreeRow(uint8_t* rows, uint32_t slots, uint32_t stride, uint64_t key)
{
    uint32_t at = (uint32_t)(key % slots);
    for (uint32_t i = 0; i < slots; i++)
    {
        uint64_t value;
        memcpy(&value, rows + (size_t)at * stride, 8);
        if (value == 0) return rows + (size_t)at * stride;
        if (value == key) return NULL;
        at = at + 1 == slots ? 0 : at + 1;
    }
    return NULL;
}

static uint8_t* EntityState(void)
{
    uintptr_t state = 0;
    if (g_game == 0 || SafeRead(g_game + REG_STATE_GLOBAL, &state, 8)) return NULL;
    return (uint8_t*)state;
}

static uint8_t* StatePointer(uint8_t* state, uint32_t offset)
{
    uintptr_t value = 0;
    return state != NULL && !SafeRead((uintptr_t)state + offset, &value, 8) ? (uint8_t*)value : NULL;
}

static int SetStatePointer(uint8_t* state, uint32_t offset, const void* value)
{
    uintptr_t v = (uintptr_t)value;
    return ApiWrite((uintptr_t)state + offset, &v, 8);
}

/* The loadout table in use and its record count. */
static uint8_t* LoadoutTable(uint8_t* state, uint32_t* records)
{
    uint8_t* table = StatePointer(state, REG_TABLES + 8 * REG_LOADOUT);
    *records = table != NULL && table == g_loadoutTable ? g_loadoutRecords : REG_LOADOUT_RECORDS;
    return table;
}

/* An entity a mod added that was not registered (its base had a component HellForge could not copy): nothing may name
 * it, or the game looks it up when it spawns it and reads a missing row (a hellpod rack's slot, a payload, a mount). */
static int RefusedEntity(uint64_t entity)
{
    for (int i = 0; i < g_weaponCount; i++) if (g_weapons[i].entity == entity && g_weapons[i].state == -1) return 1;
    return 0;
}

static int PlainCopy(uint64_t entity)
{
    for (int i = 0; i < g_weaponCount; i++) if (g_weapons[i].plain && g_weapons[i].entity == entity) return 1;
    return 0;
}

/* The entity owning a loadout record (its only index row besides entity copies of it), or 0. */
static uint64_t LoadoutOwner(uint8_t* table, uint32_t record)
{
    uint64_t owner = 0;
    for (uint32_t i = 0; i < REG_LOADOUT_SLOTS; i++)
    {
        uint64_t entity;
        uint32_t r;
        memcpy(&entity, table + 16 * i, 8);
        memcpy(&r, table + 16 * i + 8, 4);
        if (entity != 0 && r == record && !PlainCopy(entity)) { if (owner != 0) return 0; owner = entity; }
    }
    return owner;
}

static int StratagemItems(char* text, int n, int size, int first);
static int DifficultyItems(char* text, int n, int size, int first);

/* <session>/<name>, created anew for writing. */
static HANDLE CreateSessionFile(const wchar_t* name)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\%ls", g_logs, name);
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void WriteRegisteredItems(void)
{
    if (g_logs[0] == 0) return;
    char text[16384];
    int n = snprintf(text, sizeof text, "{\"format\":1,\"items\":[");
    int listed = 0;
    for (int i = 0; i < g_weaponCount && n < (int)sizeof text - 512; i++)
        if (!g_weapons[i].plain)
            n += snprintf(text + n, sizeof text - n, "%s{\"mixId\":%u,\"copyOf\":%u,\"mod\":\"%s\",\"unit\":\"%s\"}", listed++ ? "," : "",
                g_weapons[i].id, g_weapons[i].base, g_weapons[i].mod->id, g_weapons[i].name);
    int before = n;
    n = StratagemItems(text, n, (int)sizeof text, listed == 0);
    n = DifficultyItems(text, n, (int)sizeof text, listed == 0 && n == before);
    n += snprintf(text + n, sizeof text - n, "]}\n");
    HANDLE file = CreateSessionFile(L"registered-items.json");
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, text, (DWORD)n, &written, NULL);
    CloseHandle(file);
}

/* The client's id of a weapon's child item (an attachment for that weapon), and its inverse (RegisteredItems.cs). */
#define REG_CHILD_M 0x5BD1E995u
static uint32_t WeaponChild(uint32_t attachment, uint32_t weapon)
{
    uint32_t k = attachment * REG_CHILD_M;
    k ^= k >> 24;
    return k * REG_CHILD_M ^ weapon * REG_CHILD_M;
}

static uint32_t Rehome(uint32_t child, uint32_t from, uint32_t to)
{
    uint32_t inverse = REG_CHILD_M;
    for (int i = 0; i < 5; i++) inverse *= 2 - REG_CHILD_M * inverse;
    uint32_t k = (child ^ from * REG_CHILD_M) * inverse;
    k ^= k >> 24;
    return WeaponChild(k * inverse, to);
}

static const Component* ComponentNamed(const char* name)
{
    for (int i = 0; i < g_componentCount && name != NULL; i++)
        if (strcmp(g_components[i].name, name) == 0) return &g_components[i];
    return NULL;
}

static uint32_t AddText(const char* text, const char* key, uint32_t salt);

/* An upper-case copy of text (its ASCII letters; the game's names are capitals), cut to fit. */
static void UpperCopy(const char* text, char* out, size_t size)
{
    size_t n = 0;
    for (; text[n] && n + 1 < size; n++) out[n] = (text[n] >= 'a' && text[n] <= 'z') ? (char)(text[n] - 32) : text[n];
    out[n] = 0;
}

/* The components a weapon or entity owns records of (its own_components) into w; 1 if refused (logged as `what`). */
static int OwnComponents(HfMod* mod, const char* what, const char* name, const char* const* own, Weapon* w)
{
    for (; own != NULL && *own != NULL; own++)
    {
        const Component* c = ComponentNamed(*own);
        if (c == NULL || SlotsOf(c->index) == 0 || w->ownCount >= REG_MAX_OWN)
        {
            ApiLog(mod, "%s(%s): can't give it a record of its own in %s (%s)", what, name, *own,
                c == NULL ? "HellForge does not know that component; see docs/sdk-reference.md" : "too many components");
            return 1;
        }
        w->own[w->ownCount] = c->index;
        w->ownStride[w->ownCount++] = c->stride;
    }
    return 0;
}

/* Whether a weapon of this unit can be added now; logged when not. */
static int WeaponAddable(HfMod* mod, const char* unit)
{
    if (unit == NULL || unit[0] == 0 || strlen(unit) >= sizeof g_weapons[0].name) { ApiLog(mod, "add_weapon: it needs a unit name (under 192 bytes)"); return 0; }
    if (g_fired[HF_EVENT_ENTITIES_READY] || g_weaponCount >= REG_MAX_WEAPONS)
    {
        ApiLog(mod, "add_weapon(%s): too late or too many weapons; call it from hellforge_mod_init", unit);
        return 0;
    }
    return 1;
}

/* Its own projectile (HfWeapon.projectile), when the mod's HfWeapon names one; 1 if refused (logged). */
static int WeaponProjectile(HfMod* mod, const char* unit, const HfWeapon* def, Weapon* w)
{
    if (def->size < offsetof(HfWeapon, projectile) + sizeof def->projectile || def->projectile == 0) return 0;
    if (!ProjectileUsable(def->projectile))
    {
        ApiLog(mod, "add_weapon(%s): projectile %u is neither a vanilla type (1 .. 350) nor one a mod added", unit, def->projectile);
        return 1;
    }
    w->projectile = def->projectile;
    return 0;
}

/* Its own text ids (HfWeapon.name and the rest), when the mod names it. */
static void WeaponText(const HfWeapon* def, const char* unit, Weapon* w)
{
    if (def->name == NULL || def->name[0] == 0) return;
    char upper[192];
    UpperCopy(def->name, upper, sizeof upper);
    char key[256];
    snprintf(key, sizeof key, "%s#text", unit);
    w->text[0] = AddText(def->name, key, 0x01);
    w->text[1] = AddText(upper, key, 0x02);
    w->text[2] = AddText(def->description, key, 0x03);
    w->text[3] = AddText(def->designation, key, 0x04);
}

static uint32_t ApiAddWeapon(HfMod* mod, const HfWeapon* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfWeapon, own_components) + sizeof def->own_components)
    {
        ApiLog(mod, "add_weapon: no weapon, or its size is not set (HfWeapon w = {sizeof w})");
        return 0;
    }
    const char* unit = def->unit;
    if (!WeaponAddable(mod, unit)) return 0;
    uint64_t entity = Murmur64A(unit, strlen(unit));
    uint32_t id = (uint32_t)(entity >> 32);
    if (id == 0) id = 1;
    for (int i = 0; i < g_weaponCount; i++)
        if (g_weapons[i].entity == entity || g_weapons[i].id == id)
        {
            ApiLog(mod, "add_weapon(%s): already added by %s", unit, g_weapons[i].mod->id);
            return 0;
        }
    Weapon w = {0};
    w.mod = mod;
    w.base = def->base;
    w.id = id;
    w.entity = entity;
    snprintf(w.name, sizeof w.name, "%s", unit);
    if (OwnComponents(mod, "add_weapon", unit, def->own_components, &w) || WeaponProjectile(mod, unit, def, &w)) return 0;
    if (def->size >= offsetof(HfWeapon, sound_from) + sizeof def->sound_from) w.soundFrom = def->sound_from;
    WeaponText(def, unit, &w);
    g_weapons[g_weaponCount++] = w;
    ApiLog(mod, "adding weapon %s \"%s%s%s\" (entity %016llx, loadout id %08x) as a copy of loadout item %08x, %d record(s) of its own",
        unit, def->designation ? def->designation : "", def->designation ? " " : "", def->name ? def->name : "(the base's name)",
        (unsigned long long)entity, id, def->base, w.ownCount);
    WriteRegisteredItems();
    return id;
}

/* Whether an entity of this name can be added now; logged when not. */
static int EntityAddable(HfMod* mod, const HfEntity* def, const char* name)
{
    if (name == NULL || name[0] == 0 || strlen(name) >= sizeof g_weapons[0].name) { ApiLog(mod, "add_entity: it needs a name (under 192 bytes)"); return 0; }
    if (g_fired[HF_EVENT_ENTITIES_READY] || g_weaponCount >= REG_MAX_WEAPONS)
    {
        ApiLog(mod, "add_entity(%s): too late or too many entities; call it from hellforge_mod_init", name);
        return 0;
    }
    if (def->base == 0) { ApiLog(mod, "add_entity(%s): it needs a base entity", name); return 0; }
    return 1;
}

/* What the mod's HfEntity gives it besides its base: a unit, a behaviour, another entity's model, own_all. */
static void EntityOptions(const HfEntity* def, Weapon* w)
{
    if (def->unit != NULL && def->unit[0] != 0) w->unit = Murmur64A(def->unit, strlen(def->unit));
    if (def->size >= offsetof(HfEntity, behaviour) + sizeof def->behaviour) w->behaviour = def->behaviour;
    if (def->size >= offsetof(HfEntity, model_from) + sizeof def->model_from) w->modelFrom = def->model_from;
    if (def->size >= offsetof(HfEntity, own_all) + sizeof def->own_all) w->ownAll = def->own_all != 0;
}

/* Its own text ids, title and designation, when the mod's HfEntity has them. */
static void EntityText(const HfEntity* def, const char* name, Weapon* w)
{
    if (def->size < offsetof(HfEntity, description) + sizeof def->description) return;
    char key[256];
    snprintf(key, sizeof key, "%s#text", name);
    if (def->title != NULL && def->title[0])
    {
        char upper[192];
        UpperCopy(def->title, upper, sizeof upper);
        w->text[0] = AddText(def->title, key, 0x01);
        w->text[1] = AddText(upper, key, 0x02);
        snprintf(w->title, sizeof w->title, "%s", def->title);
    }
    w->text[2] = AddText(def->description, key, 0x03);
    w->text[3] = AddText(def->designation, key, 0x04);
    if (def->designation != NULL) snprintf(w->designation, sizeof w->designation, "%s", def->designation);
}

/* A new entity of any kind (SDK 4, HfEntity): a copy of a vanilla entity under the mod's name, sharing its component
 * records except the ones it owns (own_components) and, given a unit, its unit record. No loadout, armory or catalogue
 * entry: a Pelican, a turret... It is added with the weapons when the entity data loads (RegisterWeapons). */
static uint64_t ApiAddEntity(HfMod* mod, const HfEntity* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfEntity, own_components) + sizeof def->own_components)
    {
        ApiLog(mod, "add_entity: no entity, or its size is not set (HfEntity e = {sizeof e})");
        return 0;
    }
    char identity[256];
    const char* name = Identity(mod, def->name, identity, sizeof identity);
    if (!EntityAddable(mod, def, name)) return 0;
    uint64_t entity = Murmur64A(name, strlen(name));
    for (int i = 0; i < g_weaponCount; i++)
        if (g_weapons[i].entity == entity) { ApiLog(mod, "add_entity(%s): already added by %s", name, g_weapons[i].mod->id); return 0; }
    Weapon w = {0};
    w.mod = mod;
    w.plain = 1;
    w.baseEntity = def->base;
    w.entity = entity;
    w.id = (uint32_t)(entity >> 32);
    snprintf(w.name, sizeof w.name, "%s", name);
    EntityOptions(def, &w);
    EntityText(def, name, &w);
    if (OwnComponents(mod, "add_entity", name, def->own_components, &w)) return 0;
    g_weapons[g_weaponCount++] = w;
    if (w.ownAll) ApiLog(mod, "adding entity %s (%016llx) as a copy of %016llx with records of its own in every component", name,
        (unsigned long long)entity, (unsigned long long)def->base);
    else ApiLog(mod, "adding entity %s (%016llx) as a copy of %016llx, %d record(s) of its own", name, (unsigned long long)entity,
        (unsigned long long)def->base, w.ownCount);
    return entity;
}

static uint64_t ApiWeaponEntity(uint32_t weapon)
{
    for (int i = 0; i < g_weaponCount; i++)
        if (!g_weapons[i].plain && g_weapons[i].id == weapon && g_weapons[i].state != -1) return g_weapons[i].entity;
    return 0;
}

static int WeaponRegistered(uint64_t resource)
{
    for (int i = 0; i < g_weaponCount; i++) if (g_weapons[i].entity == resource && g_weapons[i].state == 1) return 1;
    return 0;
}

/* HellForge's copy of component table c that `index` is (the table the game reads now), or NULL. */
static OwnedTable* OwnedTableOf(uint32_t c, const uint8_t* index)
{
    OwnedTable* owned = NULL;
    for (int i = 0; i < g_ownedCount; i++) if (g_owned[i].component == c && g_owned[i].index == index) owned = &g_owned[i];
    return owned;
}

/* component_record for a registered weapon: its own record of the component, or 0 when it shares the base's. */
static uintptr_t RegisteredRecord(const Component* c, uint64_t resource, uint32_t* stride)
{
    if (!WeaponRegistered(resource)) return 0;
    uint8_t* table = StatePointer(EntityState(), REG_TABLES + 8 * c->index);
    uint32_t slots = SlotsOf(c->index);
    for (int i = 0; i < g_ownedCount; i++)
    {
        OwnedTable* owned = &g_owned[i];
        if (owned->component != c->index || owned->index != table) continue;
        uint8_t* row = FindRow(owned->index, slots, 16, resource);
        uint32_t record;
        if (row == NULL) return 0;
        memcpy(&record, row + 8, 4);
        if (record < owned->first || record >= owned->next) return 0;
        if (stride != NULL) *stride = owned->stride;
        return (uintptr_t)(owned->index + (size_t)slots * 16 + (size_t)record * owned->stride);
    }
    return 0;
}

/* An entity a mod added that shares its base's records: it is meant to behave like its base, so its sharing a record
 * does not make the record shared for an edit of the base (the copy changes with it). */
static int CopiesBase(uint64_t entity)
{
    for (int i = 0; i < g_weaponCount; i++) if (g_weapons[i].entity == entity && g_weapons[i].state != -1) return 1;
    return 0;
}

/* Whether the game's own table of component c has the profile's header (its index, then the reviewed 28 bytes). */
static int TableHeaderMatches(const Component* c, const uint8_t* table)
{
    uint8_t header[32];
    return !SafeRead((uintptr_t)table - 32, header, 32) && !memcmp(header, &c->index, 4) && !memcmp(header + 4, c->header, 28);
}

/* The record the entity's one index row in table names, or -1 when it has none, several or a flagged one, or a row is
 * unreadable. */
static int64_t OnlyRecordOf(const Component* c, const uint8_t* table, uint64_t resource)
{
    int64_t selected = -1;
    int owners = 0;
    for (uint32_t row = 0; row < c->indices; row++)
    {
        uint8_t entry[16];
        uint64_t id;
        uint32_t record, flags;
        if (SafeRead((uintptr_t)table + row * 16u, entry, 16)) return -1;
        memcpy(&id, entry, 8); memcpy(&record, entry + 8, 4); memcpy(&flags, entry + 12, 4);
        if (id == resource) { if (flags) return -1; owners++; selected = record; }
    }
    return owners == 1 ? selected : -1;
}

/* Whether another entity (not a copy of its base) names the record too; an unreadable row counts as one. */
static int RecordShared(const Component* c, const uint8_t* table, uint64_t resource, uint32_t selected)
{
    for (uint32_t row = 0, sharers = 0; row < c->indices; row++)
    {
        uint8_t entry[16];
        uint64_t id;
        uint32_t record;
        if (SafeRead((uintptr_t)table + row * 16u, entry, 16)) return 1;
        memcpy(&id, entry, 8); memcpy(&record, entry + 8, 4);
        if (id != 0 && id != resource && record == selected && !CopiesBase(id) && ++sharers > 0) return 1;
    }
    return 0;
}

/* An entity's record in the component table the game reads now: HellForge's copy when the registry moved the table
 * (so a vanilla entity's record there is the one the game uses, not the original in the loaded region), else the game's
 * own. Only the entity's one index row counts, and a record other entities share only with `shared`. *used 0 when the
 * entity state is not there (no game.dll: the caller falls back to the profile's region). */
static uintptr_t CurrentTableRecord(const Component* c, uint64_t resource, uint32_t* stride, int shared, int* used)
{
    *used = 0;
    uint8_t* table = StatePointer(EntityState(), REG_TABLES + 8 * c->index);
    if (table == NULL || c->indices == 0 || c->recordOffset != c->indices * 16u) return 0;
    *used = 1;
    const OwnedTable* owned = OwnedTableOf(c->index, table);
    if (owned == NULL && !TableHeaderMatches(c, table)) return 0;
    uint32_t limit = owned != NULL ? owned->next : c->records;
    int64_t selected = OnlyRecordOf(c, table, resource);
    if (selected < 0 || selected >= limit) return 0;
    if (!shared && RecordShared(c, table, resource, (uint32_t)selected)) return 0;
    if (stride != NULL) *stride = c->stride;
    return (uintptr_t)table + (uintptr_t)c->indices * 16u + (uintptr_t)selected * c->stride;
}

/* Whether `records` records of `size` bytes, padded to 16, are the table's record bytes. */
static int RecordsFit(uint32_t records, uint32_t size, uint64_t recordBytes)
{
    return (uint64_t)records * size <= recordBytes && recordBytes - (uint64_t)records * size < 16;
}

/* The record size the profile's component gives table c, or 0 when its layout or size disagree; *known 0 without one. */
static uint32_t ProfileStride(uint32_t c, uint32_t slots, uint64_t recordBytes, int* known)
{
    *known = 1;
    for (int i = 0; i < g_componentCount; i++)
        if (g_components[i].index == c && g_components[i].stride > 0)
            return g_components[i].recordOffset == slots * 16u && RecordsFit(g_components[i].records, g_components[i].stride, recordBytes) ? g_components[i].stride : 0;
    *known = 0;
    return 0;
}

/* The table's own size by its index (the build's component catalogue), when its slots and bytes agree, or 0; *known 0
 * when the catalogue has none. */
static uint32_t CatalogueStride(uint32_t c, uint32_t slots, uint64_t recordBytes, int* known)
{
    *known = 1;
    for (size_t i = 0; i < sizeof kComponentSizes / sizeof kComponentSizes[0]; i++)
        if (kComponentSizes[i].component == c)
            return kComponentSizes[i].slots == slots && RecordsFit(kComponentSizes[i].records, kComponentSizes[i].size, recordBytes) ? kComponentSizes[i].size : 0;
    *known = 0;
    return 0;
}

/* The records a table's index rows reach: one past the highest they name. */
static uint32_t ReachedRecords(const uint8_t* table, uint32_t slots)
{
    uint32_t reached = 0;
    for (uint32_t row = 0; row < slots; row++)
    {
        uint64_t id;
        uint32_t record;
        memcpy(&id, table + 16 * row, 8);
        memcpy(&record, table + 16 * row + 8, 4);
        if (id != 0 && record + 1 > reached) reached = record + 1;
    }
    return reached;
}

/* `size` when it is the only size seen (`seen` 0: none yet), else UINT32_MAX (several). */
static uint32_t OneSize(uint32_t seen, uint32_t size)
{
    return seen == 0 || seen == size ? size : UINT32_MAX;
}

/* The record size by the table's shape. Not exact: a table whose last records no row names fits a larger size
 * (WeaponLinker, four 12-byte records, three named, fits three of 16). */
static uint32_t ShapeStride(const uint8_t* table, uint32_t slots, uint64_t recordBytes)
{
    uint32_t reached = ReachedRecords(table, slots);
    uint32_t fits = 0, reaches = 0; /* the size that fits (UINT32_MAX: several), and among those the records reach */
    for (size_t i = 0; i < sizeof kComponentShapes / sizeof kComponentShapes[0]; i++)
    {
        const ComponentShape* s = &kComponentShapes[i];
        if (s->slots != slots || !RecordsFit(s->records, s->size, recordBytes)) continue;
        fits = OneSize(fits, s->size);
        if (s->records == reached) reaches = OneSize(reaches, s->size);
    }
    if (fits != UINT32_MAX) return fits;
    return reaches != UINT32_MAX ? reaches : 0;
}

/* The record size of component table c, for a copy that owns all its base's records (HfEntity.own_all), or 0 when it
 * is not known for certain: the profile's component, else the table's shape among every table of the build's entity data
 * (component_sizes.h), its index slots and its size (index rows, then whole records padded to 16 bytes; a table with
 * more after its records matches none). When several record sizes fit, the one whose record count is the number of
 * records the index rows reach. */
static uint32_t ComponentStride(uint8_t* state, uint32_t c)
{
    for (int i = 0; i < g_ownedCount; i++) if (g_owned[i].component == c) return g_owned[i].stride;
    uint32_t slots = SlotsOf(c), bytes = 0;
    uint8_t* table = StatePointer(state, REG_TABLES + 8 * c);
    if (slots == 0 || table == NULL || SafeRead((uintptr_t)table - 12, &bytes, 4) || bytes < slots * 16u) return 0;
    uint64_t recordBytes = bytes - slots * 16u;
    int known = 0;
    uint32_t stride = ProfileStride(c, slots, recordBytes, &known);
    if (!known) stride = CatalogueStride(c, slots, recordBytes, &known);
    return known ? stride : ShapeStride(table, slots, recordBytes); /* else by shape */
}

/* HellForge's copy of component table c, made at its first own record (the game's table is pointed at it), or NULL. */
static OwnedTable* OwnedCopy(uint8_t* state, uint32_t c, uint32_t stride, uint32_t slots, uint8_t* table)
{
    OwnedTable* owned = OwnedTableOf(c, table);
    if (owned != NULL) return owned;
    if (g_ownedCount >= REG_MAX_OWNED) return NULL;
    uint32_t bytes;
    memcpy(&bytes, table - 12, 4); /* the table's size after its 28-byte header */
    uint32_t recordBytes = bytes > slots * 16u ? bytes - slots * 16u : 0;
    uint32_t first = (recordBytes + stride - 1) / stride; /* the first record index after every vanilla byte */
    size_t size = 32 + (size_t)slots * 16 + ((size_t)first + REG_MAX_WEAPONS) * stride;
    uint8_t* copy = (uint8_t*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (copy == NULL) return NULL;
    memcpy(copy, table - 32, 32 + (size_t)bytes);
    owned = &g_owned[g_ownedCount++];
    *owned = (OwnedTable){c, stride, first, first, first + REG_MAX_WEAPONS, copy + 32};
    SetStatePointer(state, REG_TABLES + 8 * c, owned->index);
    Note("registry: component %u copied (%u slots, %u-byte records, new records from %u)", c, slots, stride, first);
    return owned;
}

/* A record of the weapon's own in component table c: HellForge's copy of the table (made once per load: the vanilla table,
 * then room for REG_MAX_WEAPONS records after every vanilla byte), a copy of the base's record placed after the vanilla
 * records, and the weapon's index row naming it. Returns the new record (or the one it has already), or NULL. */
static uint8_t* OwnRecord(uint8_t* state, uint32_t c, uint32_t stride, uint64_t baseEntity, uint64_t entity)
{
    uint32_t slots = SlotsOf(c);
    uint8_t* table = StatePointer(state, REG_TABLES + 8 * c);
    if (slots == 0 || table == NULL) return NULL;
    OwnedTable* owned = OwnedCopy(state, c, stride, slots, table);
    if (owned == NULL) return NULL;
    uint8_t* records = owned->index + (size_t)slots * 16;
    uint8_t* baseRow = FindRow(owned->index, slots, 16, baseEntity);
    uint8_t* row = FindRow(owned->index, slots, 16, entity);
    if (baseRow == NULL || row == NULL || owned->stride != stride) return NULL;
    uint32_t mine;
    memcpy(&mine, row + 8, 4);
    if (baseEntity != entity && mine >= owned->first && mine < owned->next) return records + (size_t)mine * stride; /* its own already */
    if (owned->next >= owned->end) return NULL;
    uint32_t baseRecord, record = owned->next++;
    memcpy(&baseRecord, baseRow + 8, 4);
    memcpy(records + (size_t)record * stride, records + (size_t)baseRecord * stride, stride);
    memcpy(row + 8, &record, 4);
    return records + (size_t)record * stride;
}

/* The engine's text function; any further register arguments are passed on untouched. */
typedef const char* (*EngineTextFn)(uint32_t id, void* a, void* b, void* c);
static EngineTextFn g_engineText;

/* The engine's text function with the mods' text first. */
static const char* TextDetour(uint32_t id, void* a, void* b, void* c)
{
    for (int i = 0; i < g_textCount; i++) if (g_texts[i].id == id) return g_texts[i].text;
    return g_engineText != NULL ? g_engineText(id, a, b, c) : "";
}

/* Points the engine API's text slot at TextDetour once the API is set up (retried until then). */
static void HookText(void)
{
    if (g_textHooked || g_textCount == 0 || g_game == 0) return;
    uintptr_t api = 0, table = 0, current = 0;
    if (SafeRead(g_game + REG_ENGINE_API, &api, 8) || api == 0 || SafeRead(api + 0x10, &table, 8) || table == 0 ||
        SafeRead(table + 1000, &current, 8) || current == 0) return;
    uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    size_t exeSize = nt->OptionalHeader.SizeOfImage;
    g_textHooked = 1;
    if (current == (uintptr_t)TextDetour) return;
    if (current < exe || current >= exe + exeSize) { Note("registry: the engine's text function is not the reviewed one; mod text not shown (%d)", g_textCount); return; }
    g_engineText = (EngineTextFn)current;
    void* detour = (void*)TextDetour;
    if (ApiWrite(table + 1000, &detour, 8)) { g_textHooked = 0; return; }
    Note("registry: engine text function hooked (%d mod text(s), exe+0x%llx)", g_textCount, (unsigned long long)(current - exe));
}

static uint32_t AddText(const char* text, const char* key, uint32_t salt)
{
    if (text == NULL || text[0] == 0 || g_textCount >= (int)(sizeof g_texts / sizeof g_texts[0])) return 0;
    size_t length = strlen(text);
    char* copy = (char*)malloc(length + 1);
    if (copy == NULL) return 0;
    memcpy(copy, text, length + 1);
    uint32_t id = (uint32_t)(Murmur64A(key, strlen(key)) >> 32) ^ salt;
    if (id == 0) id = salt;
    g_texts[g_textCount++] = (ModText){id, copy};
    return id;
}
