/* New stratagems (SDK 4, docs/modding/content.md "Stratagems"): copies of vanilla StratagemInfo rows under StratagemTypes
 * of their own (150 and up), with their own name, description, input code and icon. Included by hellforge_modloader.c,
 * after registry.c (text) and damage.c (AllocateNearGame).
 *
 * The game's stratagem list (build 25480438, game.dll 2E2C3B7C...; HD2Runtime's research on
 * stratagem identity):
 *   game.dll + 0x37CB600  StratagemType -> 400-byte StratagemInfo row, 8-byte pointers for types 1..149 (0: none; its
 *              default row is the static 0x37CB470 just before). The enum has 150 values, all used, so HellForge moves
 *              the list.
 *   loader 0x11F2080 (generated_stratagem_settings.dl_bin) clears exactly 0x4B0 bytes (150 slots, kStratagemClearPin)
 *              and sets table[row +0] = row; a reload therefore never clears the moved list's new slots.
 *   339 instructions address the list (stratagem_sites.h): 88 indexed [image base + type*8 + 0x37CB600], 251
 *              rip-relative, 61 of them in leaf functions without .pdata. 69 bounds on the type (stratagem_bounds.h:
 *              150, 149 or 148 in walks, guards and sentinels) grow by the number of new types, so walks reach them and
 *              checks pass. Bounds that size fixed 150-entry arrays stay.
 *   0x21D4AA0  the StratagemType names (150, then "Count"), read by type by the mission report: moved too (3 sites).
 *   0x348E3FC  the number of rows loaded (149), which sizes two per-type heap arrays: raised to the new length.
 *   picker 0x18D8710  one register is both the bound of its walk over the types (0x18D8AA9 mov edi, 0x96) and the
 *              count of 150 stack records it then clears; the walk's bound grows and the clear's count is set again
 *              to 150 in the alignment padding before the clear (0x18D8AE5). Its list of available stratagems holds
 *              150, and at most 96 vanilla ones can be available, so mods add at most 48.
 *   loadout +0x78C u32[150]  a level-package choice per type, written only for types whose row lists level packages
 *              (+0x118 / +0x120): new rows list none, so it is never written past its end (its readers only compare the
 *              value with that empty list).
 * Row members HellForge sets (HD2Runtime stratagem research): +0 type, +4 stable id (the catalogue's itemId of the
 * stratagem's kind-10 item and the saved loadout's identity), +0x28 / +0x2C / +0x30 name, cased name and description
 * text ids, +0x40 / +0x48 pointer to the input's u32 directions (1 up, 2 right, 3 down, 4 left) and their count, +0x50
 * uses (0xFFFFFFFF unlimited), +0x68 cooldown (f32 seconds), +0xB0 icon resource name, and when the mod names them
 * +0x98 / +0xA0 the payload entities and their count (u64 resources; a kind-3 Pelican drop spawns payload[1] carrying
 * payload[0], 0x6AD12B), +0xA8 the mission package, +0xF8 a loadout item whose
 * package the mission loads too (the MG-206 stratagem names the MG-206's, 0x1FC9CA61), +0xB8 the loadout section and
 * ping colour (0 red, 2 blue, else yellow), +0xD4 the beam colour (1 red, 2 blue, 3 yellow) and +0x104 loadout flags
 * (every vehicle has 0x20: one per loadout). Everything else (delivery, beacon) stays the base's. The local backend lists each in the catalogue as a
 * copy of its base's items (registered-items.json; RegisteredItems.cs). */

typedef struct TableSite { uint32_t rva; uint8_t dispAt, size, rip; uint16_t slot; } TableSite;
typedef struct TableBound { uint32_t rva; uint8_t immAt, width; uint32_t value; } TableBound;
#include "stratagem_sites.h"
#include "stratagem_bounds.h"

#define STR_TABLE 0x37CB600u
#define STR_VANILLA 150u
#define STR_MAX_NEW 48u
_Static_assert(STR_VANILLA + STR_MAX_NEW <= 256u, "stratagem types travel in 8 bits (co-op research 2026-10-21)");
#define STR_ROW 400u
#define STR_MAX_CODE 9u
#define STR_MAX_PAYLOAD 8u
#define STR_CLEAR 0x11F20FEu /* 41 B8 B0 04 00 00: mov r8d, 0x4B0 (150 slots) */
#define STR_NAMES 0x21D4AA0u
#define STR_ROWS_LOADED 0x348E3FCu
#define STR_PICKER_BOUND 0x18D8AA9u /* BF 96 00 00 00: mov edi, 0x96 */
#define STR_PICKER_PAD 0x18D8AE5u   /* 11-byte nop before the record clear at 0x18D8AF0 */

static const uint8_t kStratagemClearPin[6] = {0x41, 0xb8, 0xb0, 0x04, 0x00, 0x00};
static const uint8_t kPickerBoundPin[5] = {0xbf, 0x96, 0x00, 0x00, 0x00};
static const uint8_t kPickerPadPin[11] = {0x66, 0x66, 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
/* mov edi, 0x96 (the clear's 150 records), then a 6-byte nop. */
static const uint8_t kPickerPad[11] = {0xbf, 0x96, 0x00, 0x00, 0x00, 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};

typedef struct Stratagem
{
    HfMod* mod;
    char name[192];
    uint32_t type, base, stable, item;
    uint32_t text[3];          /* name, cased name, description; 0: the base's */
    uint32_t code[STR_MAX_CODE];
    uint32_t codeLength;       /* 0: the base's */
    uint64_t icon;             /* 0: the base's */
    float cooldown;
    int32_t uses;
    uint64_t payload[STR_MAX_PAYLOAD];
    uint32_t payloadCount;     /* 0: the base's */
    uint64_t package;          /* 0: the base's; ~0: none */
    uint32_t loadsItem, category, beam, flags; /* 0: the base's */
    uint8_t* row;              /* installed row */
    char title[96], iconName[192]; /* as the mod gave them ("": the base's), for content */
    uint32_t editorLike;       /* the stratagem whose editor page it takes */
    HfEditorEntity editorEntities[STR_MAX_PAYLOAD + 1]; /* ending in {0} */
    uint32_t selectionCategory;
    int32_t selectionOrder;
} Stratagem;

static Stratagem g_stratagems[STR_MAX_NEW];
static uint32_t g_stratagemCount;
static int g_stratagemState; /* 0 waiting, 1 installed, -1 refused */
static uint8_t* g_stratagemTable;

#include "stratagem_categories.c"

static void WriteRegisteredItems(void);
static int RefusedEntity(uint64_t entity);

/* Once the entity data is in (before HF_EVENT_ENTITIES_READY reaches mods): a stratagem whose payload names an entity a
 * mod added that was not registered calls in its base's payload instead, or the game would spawn an entity it has no
 * row for. */
static void StratagemPayloadsCheck(void)
{
    if (g_stratagemState != 1 || g_stratagemTable == NULL) return;
    for (uint32_t i = 0; i < g_stratagemCount; i++)
    {
        Stratagem* s = &g_stratagems[i];
        int refused = 0;
        for (uint32_t p = 0; p < s->payloadCount; p++) refused |= s->payload[p] != 0 && RefusedEntity(s->payload[p]);
        uintptr_t base = 0;
        if (!refused || s->row == NULL || SafeRead((uintptr_t)g_stratagemTable + (uintptr_t)s->base * 8, &base, 8) || base == 0) continue;
        uint8_t payload[16];
        if (SafeRead(base + 0x98, payload, sizeof payload)) continue;
        memcpy(s->row + 0x98, payload, sizeof payload);
        s->payloadCount = 0;
        ApiLog(s->mod, "%s: its payload names an entity that was not registered (see above); it calls in its base's payload", s->name);
    }
}

/* Whether a stratagem of this name can be added now; logged when not. */
static int StratagemAddable(HfMod* mod, const HfStratagem* def, const char* name)
{
    if (name == NULL || name[0] == 0 || strlen(name) >= sizeof g_stratagems[0].name) { ApiLog(mod, "add_stratagem: it needs a name (under 192 bytes)"); return 0; }
    if (g_stratagemState != 0 || g_fired[HF_EVENT_GAME_LOADED]) { ApiLog(mod, "add_stratagem(%s): too late; call it from hellforge_mod_init", name); return 0; }
    if (g_stratagemCount >= STR_MAX_NEW) { ApiLog(mod, "add_stratagem(%s): mods added %u stratagems already", name, STR_MAX_NEW); return 0; }
    if (def->base == 0 || def->base >= STR_VANILLA) { ApiLog(mod, "add_stratagem(%s): base %u is not a vanilla stratagem type (1 .. 149)", name, def->base); return 0; }
    return 1;
}

/* 1 (logged) when a mod added a stratagem of this stable id or name already. */
static int StratagemTaken(HfMod* mod, const char* name, uint32_t stable)
{
    for (uint32_t i = 0; i < g_stratagemCount; i++)
        if (g_stratagems[i].stable == stable || strcmp(g_stratagems[i].name, name) == 0)
        {
            ApiLog(mod, "add_stratagem(%s): already added by %s", name, g_stratagems[i].mod->id);
            return 1;
        }
    return 0;
}

/* An input code letter's direction (1 up, 2 right, 3 down, 4 left), or 0. */
static uint32_t CodeDirection(char c)
{
    if (c == 'U' || c == 'u') return 1;
    if (c == 'R' || c == 'r') return 2;
    if (c == 'D' || c == 'd') return 3;
    return c == 'L' || c == 'l' ? 4 : 0;
}

/* The mod's input code (NULL: the base's) into s; 1 if refused (logged). */
static int StratagemCode(HfMod* mod, const char* name, const char* code, Stratagem* s)
{
    if (code == NULL) return 0;
    for (const char* c = code; *c; c++)
    {
        uint32_t d = CodeDirection(*c);
        if (d == 0 || s->codeLength >= STR_MAX_CODE) { ApiLog(mod, "add_stratagem(%s): code \"%s\" must be 1 to 9 of U, D, L, R", name, code); return 1; }
        s->code[s->codeLength++] = d;
    }
    if (s->codeLength == 0) { ApiLog(mod, "add_stratagem(%s): code is empty", name); return 1; }
    return 0;
}

/* Its cooldown and uses; 1 if refused (logged). */
static int StratagemUse(HfMod* mod, const char* name, const HfStratagem* def, Stratagem* s)
{
    if (def->cooldown < 0.0f || def->cooldown > 3600.0f) { ApiLog(mod, "add_stratagem(%s): cooldown must be 0 to 3600 seconds", name); return 1; }
    if (def->uses < -1) { ApiLog(mod, "add_stratagem(%s): uses must be -1 (unlimited), 0 (the base's) or a count", name); return 1; }
    s->cooldown = def->cooldown;
    s->uses = def->uses;
    return 0;
}

/* Its payload and package, when the mod's HfStratagem has them; 1 if refused (logged). */
static int StratagemPayload(HfMod* mod, const char* name, const HfStratagem* def, Stratagem* s)
{
    if (def->size < offsetof(HfStratagem, package) + sizeof def->package) return 0;
    if (def->payload != NULL && (def->payload_count == 0 || def->payload_count > STR_MAX_PAYLOAD))
    {
        ApiLog(mod, "add_stratagem(%s): payload_count must be 1 to %u", name, STR_MAX_PAYLOAD);
        return 1;
    }
    if (def->payload != NULL)
    {
        int any = 0;
        for (uint32_t i = 0; i < def->payload_count; i++) any |= def->payload[i] != 0;
        if (!any) { ApiLog(mod, "add_stratagem(%s): its payload names no entity", name); return 1; }
        memcpy(s->payload, def->payload, sizeof s->payload[0] * def->payload_count);
        s->payloadCount = def->payload_count;
    }
    s->package = def->package;
    return 0;
}

/* Its loadout item, category, beam and flags, when the mod's HfStratagem has them; 1 if refused (logged). */
static int StratagemLoadout(HfMod* mod, const char* name, const HfStratagem* def, Stratagem* s)
{
    if (def->size < offsetof(HfStratagem, flags) + sizeof def->flags) return 0;
    if (def->category > 5 || def->beam > 3) { ApiLog(mod, "add_stratagem(%s): category is HF_CATEGORY_..., beam HF_BEAM_...", name); return 1; }
    s->loadsItem = def->loads_item;
    s->category = def->category;
    s->beam = def->beam;
    s->flags = def->flags;
    return 0;
}

/* Its icon and title as the mod gave them, and the editor page it takes (its base's, until the mod names one). */
static void StratagemLooks(const HfStratagem* def, Stratagem* s)
{
    s->icon = def->icon != NULL && def->icon[0] ? Murmur64A(def->icon, strlen(def->icon)) : 0;
    if (def->icon != NULL) snprintf(s->iconName, sizeof s->iconName, "%s", def->icon);
    if (def->title != NULL) snprintf(s->title, sizeof s->title, "%s", def->title);
    s->editorLike = s->base;
}

/* Its selection category and order and its editor page, when the mod's HfStratagem has them; 1 if refused (logged). */
static int StratagemEditor(HfMod* mod, const char* name, const HfStratagem* def, Stratagem* s)
{
    if (def->size >= offsetof(HfStratagem, selection_category) + sizeof def->selection_category)
    {
        if (def->selection_category > g_stratagemCategoryCount)
        {
            ApiLog(mod, "add_stratagem(%s): selection_category is not a registered category handle", name);
            return 1;
        }
        s->selectionCategory = def->selection_category;
    }
    if (def->size >= offsetof(HfStratagem, selection_order) + sizeof def->selection_order)
        s->selectionOrder = def->selection_order;
    if (def->size >= offsetof(HfStratagem, editor_entities) + sizeof def->editor_entities)
    {
        if (def->editor_like != 0 && def->editor_like < STR_VANILLA) s->editorLike = def->editor_like;
        int n = 0;
        for (const HfEditorEntity* e = def->editor_entities; e != NULL && e->entity != 0 && n < (int)STR_MAX_PAYLOAD; e++) s->editorEntities[n++] = *e;
    }
    return 0;
}

/* Its text ids: name, cased name and description (0: the base's). */
static void StratagemText(const char* name, const HfStratagem* def, Stratagem* s)
{
    char key[256];
    snprintf(key, sizeof key, "%s#text", name);
    if (def->title != NULL && def->title[0])
    {
        char upper[192];
        UpperCopy(def->title, upper, sizeof upper);
        s->text[0] = AddText(upper, key, 0x11);
        s->text[1] = AddText(def->title, key, 0x12);
    }
    s->text[2] = AddText(def->description, key, 0x13);
}

static uint32_t ApiAddStratagem(HfMod* mod, const HfStratagem* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfStratagem, uses) + sizeof def->uses)
    {
        ApiLog(mod, "add_stratagem: no stratagem, or its size is not set (HfStratagem s = {sizeof s})");
        return 0;
    }
    char identity[256];
    const char* name = Identity(mod, def->name, identity, sizeof identity);
    if (!StratagemAddable(mod, def, name)) return 0;
    Stratagem s = {0};
    s.mod = mod;
    snprintf(s.name, sizeof s.name, "%s", name);
    s.base = def->base;
    s.stable = (uint32_t)(Murmur64A(name, strlen(name)) >> 32);
    char key[256];
    snprintf(key, sizeof key, "%s#item", name);
    s.item = (uint32_t)(Murmur64A(key, strlen(key)) >> 32);
    if (s.stable == 0 || s.item == 0) { ApiLog(mod, "add_stratagem(%s): its name hashes to 0; choose another", name); return 0; }
    if (StratagemTaken(mod, name, s.stable) || StratagemCode(mod, name, def->code, &s) || StratagemUse(mod, name, def, &s) ||
        StratagemPayload(mod, name, def, &s) || StratagemLoadout(mod, name, def, &s))
        return 0;
    StratagemLooks(def, &s);
    if (StratagemEditor(mod, name, def, &s)) return 0;
    StratagemText(name, def, &s);
    s.type = STR_VANILLA + g_stratagemCount;
    g_stratagems[g_stratagemCount++] = s;
    ApiLog(mod, "adding stratagem %s as type %u (stable id %08x, item %08x): a copy of type %u", s.name, s.type, s.stable, s.item, s.base);
    WriteRegisteredItems();
    return s.type;
}

/* Each stratagem's entry in registered-items.json: its catalogue item (mixId), its stable id (the kind-10 child's
 * itemId) and the stable id of the base it copies (RegisteredItems.cs copies the base's items). The base's stable id is
 * known once the game has loaded its stratagems, so the file is written again then; until then it is 0. */
static int StratagemItems(char* text, int n, int size, int first)
{
    for (uint32_t i = 0; i < g_stratagemCount && n < size - 512; i++, first = 0)
    {
        Stratagem* s = &g_stratagems[i];
        uint32_t baseStable = 0;
        uint64_t base = 0;
        if (g_stratagemTable != NULL) memcpy(&base, g_stratagemTable + s->base * 8, 8);
        if (base != 0) SafeRead(base + 4, &baseStable, 4);
        n += snprintf(text + n, size - n, "%s{\"kind\":\"stratagem\",\"mixId\":%u,\"stable\":%u,\"copyOfType\":%u,\"copyOf\":%u,\"mod\":\"%s\",\"name\":\"%s\"}",
            first ? "" : ",", s->item, s->stable, s->base, baseStable, s->mod->id, s->name);
    }
    return n;
}

static uintptr_t ApiStratagemRow(uint32_t type)
{
    if (g_stratagemState != 1) return 0;
    for (uint32_t i = 0; i < g_stratagemCount; i++) if (g_stratagems[i].type == type) return (uintptr_t)g_stratagems[i].row;
    return 0;
}

static int32_t TableDisplacement(uintptr_t table, uintptr_t game, const TableSite* s)
{
    return s->rip ? (int32_t)(int64_t)(table + s->slot * 8u - (game + s->rva + s->size)) : (int32_t)(int64_t)(table + s->slot * 8u - game);
}

/* NULL when every instruction addressing the list and reading the names is the reviewed one. */
static const char* StratagemReadersFailed(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kStratagemSites / sizeof kStratagemSites[0]; i++)
    {
        int32_t disp;
        if (SafeRead(game + kStratagemSites[i].rva + kStratagemSites[i].dispAt, &disp, 4) ||
            disp != TableDisplacement(game + STR_TABLE, game, &kStratagemSites[i]))
            return "an instruction addressing the stratagem list is not the reviewed one";
    }
    for (size_t i = 0; i < sizeof kStratagemNameSites / sizeof kStratagemNameSites[0]; i++)
    {
        int32_t disp;
        if (SafeRead(game + kStratagemNameSites[i].rva + kStratagemNameSites[i].dispAt, &disp, 4) || disp != (int32_t)STR_NAMES)
            return "an instruction reading the stratagem names is not the reviewed one";
    }
    return NULL;
}

static const char* StratagemSitesFailed(uintptr_t game)
{
    uint8_t clear[sizeof kStratagemClearPin];
    if (SafeRead(game + STR_CLEAR, clear, sizeof clear) || memcmp(clear, kStratagemClearPin, sizeof clear) != 0)
        return "the stratagem list's loader does not clear the reviewed 150 slots";
    const char* why = StratagemReadersFailed(game);
    if (why != NULL) return why;
    uint8_t bound[sizeof kPickerBoundPin], pad[sizeof kPickerPadPin];
    if (SafeRead(game + STR_PICKER_BOUND, bound, sizeof bound) || memcmp(bound, kPickerBoundPin, sizeof bound) != 0 ||
        SafeRead(game + STR_PICKER_PAD, pad, sizeof pad) || memcmp(pad, kPickerPadPin, sizeof pad) != 0)
        return "the loadout picker is not the reviewed one";
    uint32_t loaded = 0;
    if (SafeRead(game + STR_ROWS_LOADED, &loaded, 4) || loaded != STR_VANILLA - 1)
        return "the game did not load the reviewed 149 stratagems";
    for (size_t i = 0; i < sizeof kStratagemBounds / sizeof kStratagemBounds[0]; i++)
    {
        uint32_t value = 0;
        if (SafeRead(game + kStratagemBounds[i].rva + kStratagemBounds[i].immAt, &value, kStratagemBounds[i].width) ||
            value != kStratagemBounds[i].value)
            return "a bound on the stratagem types is not the reviewed one";
    }
    return NULL;
}

static int RefuseStratagems(const char* why)
{
    g_stratagemState = -1;
    Note("stratagems: %u new stratagem(s) refused: %s", g_stratagemCount, why);
    for (uint32_t i = 0; i < g_stratagemCount; i++) ApiLog(g_stratagems[i].mod, "stratagem %s (type %u) not added: %s", g_stratagems[i].name, g_stratagems[i].type, why);
    return 1;
}

/* A new stratagem's row: a copy of its base's (its type and stable id its own, and the text, input, uses, cooldown and
 * icon the mod set); NULL, or why it is refused. */
static const char* FillStratagemRow(Stratagem* s, const uint8_t* table, uint8_t* row)
{
    uint64_t base;
    memcpy(&base, table + s->base * 8, 8);
    s->row = row;
    if (base == 0 || SafeRead(base, s->row, STR_ROW) || *(uint32_t*)s->row != s->base)
        return "a base stratagem row does not name its type";
    if (!(s->row[0x80] & 2) || !(s->row[0xC0] & 1))
        return "a base stratagem is not one players can pick";
    memcpy(s->row, &s->type, 4);
    memcpy(s->row + 4, &s->stable, 4);
    for (int t = 0; t < 3; t++) if (s->text[t] != 0) memcpy(s->row + 0x28 + 4 * t, &s->text[t], 4);
    if (s->codeLength != 0)
    {
        uint32_t* code = s->code; /* lives as long as the loader */
        memcpy(s->row + 0x40, &code, 8);
        memcpy(s->row + 0x48, &s->codeLength, 4);
    }
    if (s->uses != 0) { uint32_t uses = s->uses < 0 ? 0xFFFFFFFFu : (uint32_t)s->uses; memcpy(s->row + 0x50, &uses, 4); }
    if (s->cooldown > 0.0f) memcpy(s->row + 0x68, &s->cooldown, 4);
    if (s->icon != 0) memcpy(s->row + 0xB0, &s->icon, 8);
    return NULL;
}

/* The row's payload, package, loadout item, category, beam and flags the mod set. */
static void FillStratagemLoadout(Stratagem* s)
{
    if (s->payloadCount != 0)
    {
        uint64_t* payload = s->payload; /* lives as long as the loader */
        memcpy(s->row + 0x98, &payload, 8);
        memcpy(s->row + 0xA0, &s->payloadCount, 4);
        memset(s->row + 0xA4, 0, 4);
    }
    if (s->package != 0) { uint64_t package = s->package == ~0ull ? 0 : s->package; memcpy(s->row + 0xA8, &package, 8); }
    if (s->loadsItem != 0) memcpy(s->row + 0xF8, &s->loadsItem, 4);
    if (s->category != 0) { uint32_t category = s->category - 1; memcpy(s->row + 0xB8, &category, 4); }
    if (s->beam != 0) memcpy(s->row + 0xD4, &s->beam, 4);
    if (s->flags != 0) memcpy(s->row + 0x104, &s->flags, 4);
    memset(s->row + 0x118, 0, 12); /* no level packages: the loadout's per-type choice (+0x78C[150]) is never written */
}

/* Points every instruction addressing the list and reading the names at the moved ones; NULL, or why it failed. */
static const char* MoveStratagemReaders(uintptr_t game, const uint8_t* table, const uint8_t* names)
{
    for (size_t i = 0; i < sizeof kStratagemSites / sizeof kStratagemSites[0]; i++)
    {
        int32_t disp = TableDisplacement((uintptr_t)table, game, &kStratagemSites[i]);
        if (ApiWrite(game + kStratagemSites[i].rva + kStratagemSites[i].dispAt, &disp, 4))
        {
            Note("stratagems: could not rewrite the instruction at 0x%x; %zu of %zu moved", kStratagemSites[i].rva, i, sizeof kStratagemSites / sizeof kStratagemSites[0]);
            return "an instruction could not be rewritten";
        }
    }
    for (size_t i = 0; i < sizeof kStratagemNameSites / sizeof kStratagemNameSites[0]; i++)
    {
        int32_t disp = (int32_t)(int64_t)((uintptr_t)names - game);
        if (ApiWrite(game + kStratagemNameSites[i].rva + kStratagemNameSites[i].dispAt, &disp, 4))
            return "an instruction reading the names could not be rewritten";
    }
    return NULL;
}

/* Widens every bound on the stratagem types by the new ones; NULL, or why it failed. */
static const char* WidenStratagemBounds(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kStratagemBounds / sizeof kStratagemBounds[0]; i++)
    {
        uint32_t value = kStratagemBounds[i].value + g_stratagemCount; /* 150 -> count, 149 -> count - 1 */
        if (ApiWrite(game + kStratagemBounds[i].rva + kStratagemBounds[i].immAt, &value, kStratagemBounds[i].width))
        {
            Note("stratagems: could not widen the bound at 0x%x", kStratagemBounds[i].rva);
            return "a bound could not be widened";
        }
    }
    return NULL;
}

/* Moves the list (into *moved) with the new rows and widens the code that reads it; NULL, or why it is refused. */
static const char* MoveStratagems(uintptr_t game, uint8_t** moved)
{
    /* The list, the names (one more: "Count") and the rows, in one block. */
    size_t bytes = (size_t)(STR_VANILLA + STR_MAX_NEW) * 8 * 2 + 8 + (size_t)g_stratagemCount * STR_ROW;
    uint8_t* table = AllocateNearGame(game, bytes);
    if (table == NULL) return "no memory within 2 GB of game.dll for the moved list";
    if (SafeRead(game + STR_TABLE, table, STR_VANILLA * 8)) return "the stratagem list is unreadable";
    uint8_t* names = table + (STR_VANILLA + STR_MAX_NEW) * 8;
    uint8_t* rows = names + (STR_VANILLA + STR_MAX_NEW) * 8 + 8;
    if (SafeRead(game + STR_NAMES, names, (STR_VANILLA + 1) * 8)) return "the stratagem names are unreadable";
    memcpy(names + (STR_VANILLA + g_stratagemCount) * 8, names + STR_VANILLA * 8, 8); /* "Count" after the new ones */
    for (uint32_t i = 0; i < g_stratagemCount; i++)
    {
        Stratagem* s = &g_stratagems[i];
        const char* why = FillStratagemRow(s, table, rows + (size_t)i * STR_ROW);
        if (why != NULL) return why;
        FillStratagemLoadout(s);
        const char* name = s->name;
        memcpy(names + s->type * 8, &name, 8);
    }
    /* The new rows first (no code reaches them yet), then the instructions, then the bounds that let code reach them. */
    for (uint32_t i = 0; i < g_stratagemCount; i++) memcpy(table + g_stratagems[i].type * 8, &g_stratagems[i].row, 8);
    const char* why = MoveStratagemReaders(game, table, names);
    if (why != NULL) return why;
    uint32_t count = STR_VANILLA + g_stratagemCount;
    if (ApiWrite(game + STR_ROWS_LOADED, &count, 4)) return "the row count could not be raised";
    if (ApiWrite(game + STR_PICKER_PAD, kPickerPad, sizeof kPickerPad) || ApiWrite(game + STR_PICKER_BOUND + 1, &count, 4))
        return "the loadout picker could not be widened";
    *moved = table;
    return WidenStratagemBounds(game);
}

/* Polled after GAME_LOADED until it returns 1: moves the list and adds the rows once the game has loaded its stratagem
 * settings. pinsReady: the grace period for code still being decoded is over. */
static int InstallStratagems(uintptr_t game, int pinsReady)
{
    if (g_stratagemState != 0) return 1;
    if (g_stratagemCount == 0) { g_stratagemState = 1; return 1; }
    uint64_t first = 0;
    if (SafeRead(game + STR_TABLE + 8, &first, 8) || first == 0) return 0; /* not loaded yet */
    const char* why = StratagemSitesFailed(game);
    if (why != NULL) return pinsReady ? RefuseStratagems(why) : 0;
    uint8_t* table = NULL;
    why = MoveStratagems(game, &table);
    if (why != NULL) return RefuseStratagems(why);
    uint32_t count = STR_VANILLA + g_stratagemCount;
    g_stratagemTable = table;
    g_stratagemState = 1;
    WriteRegisteredItems();
    HookText();
    InstallStratagemCategories(game);
    Note("stratagems: the list moved to 0x%llx (%zu instructions, %zu bounds) with %u new stratagem(s); %u types",
        (unsigned long long)(uintptr_t)table, sizeof kStratagemSites / sizeof kStratagemSites[0],
        sizeof kStratagemBounds / sizeof kStratagemBounds[0], g_stratagemCount, count);
    for (uint32_t i = 0; i < g_stratagemCount; i++)
        ApiLog(g_stratagems[i].mod, "stratagem %s is type %u (stable id %08x, icon %016llx)", g_stratagems[i].name, g_stratagems[i].type,
            g_stratagems[i].stable, (unsigned long long)(g_stratagems[i].icon ? g_stratagems[i].icon : *(uint64_t*)(g_stratagems[i].row + 0xB0)));
    return 1;
}
