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
 *   0x348E3FC  the number of rows loaded (149), which sizes two per-type heap arrays: raised to the new length, and kept
 *              there when the loader runs again (it sets it back to the rows it loaded, 0x11F2230; kCountStub).
 *   picker 0x18D8710  one register is both the bound of its walk over the types (0x18D8AA9 mov edi, 0x96) and the
 *              count of 150 stack records it then clears; the walk's bound grows and the clear's count is set again
 *              in the alignment padding before the clear (0x18D8AE5). Its frame holds 150 candidate records and 150
 *              pointers, so it is sized once for every type HellForge has room for (WidenPicker: six instructions and
 *              the unwind data), and the clear clears them all.
 *   loadout +0x78C u32[150]  a level-package choice per type, written only for types whose row lists level packages
 *              (+0x118 / +0x120): new rows list none. Six readers index it by the type unchecked, so a new type read
 *              past its end, and its decoder and writer would write past it; each goes through a guard (kChoiceGuards).
 *   co-op      a type travels as field type 0xC2EB5C15, 8 bits: past 256 types it travels in 16 (WidenStratagemField).
 * research content-limits\stratagems-wide has the evidence for all of these. What else holds new stratagems: the
 * picker shows 256 cards (the game drops the rest; about 160 new ones next to the vanilla ones) and the item catalogue
 * (each stratagem is two items, shared with mod weapons; RegisteredItems.cs).
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
#define STR_MAX_NEW 1024u /* types 150..1173 */
#define STR_TYPES (STR_VANILLA + STR_MAX_NEW)
_Static_assert(STR_TYPES <= 65536u, "stratagem types travel in at most 16 bits (WidenStratagemField)");
_Static_assert(STR_MAX_NEW * 3 <= REG_STRATAGEM_TEXTS, "each stratagem's three texts have room of their own (registry.c g_texts)");
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
/* mov edi, <the clear's records: STR_TYPES, set at install>, then a 6-byte nop. */
static const uint8_t kPickerPad[11] = {0xbf, 0x96, 0x00, 0x00, 0x00, 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};

/* The picker's frame (research stratagem-limit, content-limits\stratagems-wide picker_frame_model.py): after five pushes
 * it allocates F (0x10D0) bytes holding, above the outgoing space, C 20-byte candidate records at +0x50, C pointers at
 * P = align16(0x50 + 20C), the security cookie at K = P + 8C and 8 bytes of padding: F = K + 0x10. rbx, rbp and rsi
 * are saved in the caller's home area before the allocation, at F + 0x38 / 0x40 / 0x48 once it is made. For C
 * candidates the six instructions take the new P, K and F, and the unwind data (its large allocation, the three
 * saves and the exception handler's cookie offset) the same, or an exception inside the picker would unwind wrong.
 * {rva, the instruction, where its 32-bit value is, which value: 0 F, 1 K, 2 P}. */
#define STR_PICKER_UNWIND 0x257FC5Cu
typedef struct PickerFrameSite { uint32_t rva; uint8_t pin[8], length, at, which; } PickerFrameSite;
static const PickerFrameSite kPickerFrame[6] = {
    {0x18D8728u, {0xB8, 0xD0, 0x10, 0x00, 0x00}, 5, 1, 0},                         /* mov eax, F (the allocation) */
    {0x18D873Fu, {0x48, 0x89, 0x84, 0x24, 0xC0, 0x10, 0x00, 0x00}, 8, 4, 1},       /* mov [rsp+K], rax (the cookie) */
    {0x18D8AC8u, {0x48, 0x89, 0x84, 0xEC, 0x10, 0x0C, 0x00, 0x00}, 8, 4, 2},       /* mov [rsp+rbp*8+P], rax */
    {0x18D8AFDu, {0x48, 0x8D, 0xBC, 0x24, 0x10, 0x0C, 0x00, 0x00}, 8, 4, 2},       /* lea rdi, [rsp+P] */
    {0x18D8D63u, {0x48, 0x8B, 0x8C, 0x24, 0xC0, 0x10, 0x00, 0x00}, 8, 4, 1},       /* mov rcx, [rsp+K] */
    {0x18D8D73u, {0x4C, 0x8D, 0x9C, 0x24, 0xD0, 0x10, 0x00, 0x00}, 8, 4, 0}};      /* lea r11, [rsp+F] */
/* UNWIND_INFO: version 1 with handlers, a 0x37-byte prolog, 13 codes: save rsi / rbp / rbx at slots 0x223 / 0x222 /
 * 0x221 (x8), alloc large 0x21A (x8), five pushes; then the handler (0x21044A0) and its data, whose first dword is the
 * cookie's offset (0x10C0; the handler masks its low three bits). */
static const uint8_t kPickerUnwindPin[40] = {0x19, 0x37, 0x0D, 0x00, 0x25, 0x64, 0x23, 0x02, 0x25, 0x54, 0x22, 0x02,
    0x25, 0x34, 0x21, 0x02, 0x25, 0x01, 0x1A, 0x02, 0x18, 0xF0, 0x16, 0xE0, 0x14, 0xD0, 0x12, 0xC0, 0x10, 0x70, 0x00, 0x00,
    0xA0, 0x44, 0x10, 0x02, 0xC0, 0x10, 0x00, 0x00};
#define STR_PICKER_MAX 4096u /* a 119 KB frame; the stack is 1 MB */
_Static_assert(STR_TYPES <= STR_PICKER_MAX, "the picker's frame holds one candidate per type");

typedef struct PickerFrame { uint32_t pointers, cookie, frame; } PickerFrame;

static PickerFrame PickerFrameFor(uint32_t candidates)
{
    PickerFrame f;
    f.pointers = (0x50u + 20u * candidates + 15u) & ~15u;
    f.cookie = f.pointers + 8u * candidates;
    f.frame = f.cookie + 0x10u;
    return f;
}

/* The loadout's per-type choice (+0x78C, u32[150]) is indexed by a stratagem type, unchecked, by six readers (a new
 * type, 150 and up, read past it into the loadout's next fields), the level-choice decoder's store and the writer
 * (research stratagem-limit, content-limits\stratagems-wide). Each instruction goes through a stub: for a vanilla
 * type the instruction itself; for a new one a read gives 0 (no choice) and a write is skipped. The flags are dead
 * after every site (a call or a flag write comes first), so the stubs compare freely. {rva, length, the instruction,
 * the type's test (cmp reg, 150), what a read gives instead}. */
typedef struct ChoiceGuard { uint32_t rva; uint8_t length; uint8_t pin[8]; uint8_t test[7], testLength; uint8_t none[3], noneLength; } ChoiceGuard;
#define STR_TEST_EAX {0x3D, 0x96, 0x00, 0x00, 0x00}, 5 /* cmp eax, 150 */
static const ChoiceGuard kChoiceGuards[8] = {
    {0x13649EEu, 8, {0x44, 0x8B, 0x8C, 0x83, 0x8C, 0x07, 0x00, 0x00}, STR_TEST_EAX, {0x45, 0x31, 0xC9}, 3}, /* mov r9d, [rbx+rax*4+0x78c] */
    {0x1364B19u, 7, {0x8B, 0x84, 0x83, 0x8C, 0x07, 0x00, 0x00}, STR_TEST_EAX, {0x31, 0xC0}, 2},             /* mov eax, [rbx+rax*4+0x78c] */
    {0x13691DEu, 8, {0x44, 0x8B, 0x8C, 0x83, 0x8C, 0x07, 0x00, 0x00}, STR_TEST_EAX, {0x45, 0x31, 0xC9}, 3},
    {0x1369309u, 7, {0x8B, 0x84, 0x83, 0x8C, 0x07, 0x00, 0x00}, STR_TEST_EAX, {0x31, 0xC0}, 2},
    {0x175340Au, 8, {0x44, 0x8B, 0x8C, 0x87, 0x8C, 0x07, 0x00, 0x00}, STR_TEST_EAX, {0x45, 0x31, 0xC9}, 3}, /* mov r9d, [rdi+rax*4+0x78c] */
    {0x175610Cu, 8, {0x44, 0x8B, 0x8C, 0x87, 0x8C, 0x07, 0x00, 0x00}, STR_TEST_EAX, {0x45, 0x31, 0xC9}, 3},
    {0x11E9201u, 5, {0x43, 0x89, 0x6C, 0xB5, 0x00}, {0x49, 0x81, 0xFE, 0x96, 0x00, 0x00, 0x00}, 7, {0}, 0},  /* mov [r13+r14*4], ebp (decoder) */
    {0x1467A09u, 8, {0x44, 0x89, 0x8C, 0x88, 0x8C, 0x07, 0x00, 0x00}, {0x81, 0xF9, 0x96, 0x00, 0x00, 0x00}, 6, {0}, 0}}; /* mov [rax+rcx*4+0x78c], r9d */
#undef STR_TEST_EAX
#define STR_GUARD_SLOT 32u
/* The loader's last store of the count (mov [rip -> 0x348E3FC], r10d: the rows it loaded) and its stub, which keeps
 * the count at least the moved list's length, read from the block (STR_LENGTH_AT, raised with the list):
 * {store as it was; cmp r10d, [length]; jae back; push r10; mov r10d, [length]; mov [count], r10d; pop r10; jmp back},
 * the rip-relative operands set at install. */
#define STR_COUNT_STORE 0x11F2230u
static const uint8_t kCountStorePin[7] = {0x44, 0x89, 0x15, 0xC5, 0xC1, 0x29, 0x02};
static const uint8_t kCountStub[39] = {
    0x44, 0x89, 0x15, 0, 0, 0, 0,                 /*  0 mov [rip+count], r10d (disp at 3) */
    0x44, 0x3B, 0x15, 0, 0, 0, 0,                 /*  7 cmp r10d, [rip+length] (disp at 10) */
    0x73, 0x12,                                   /* 14 jae 34 */
    0x41, 0x52,                                   /* 16 push r10 */
    0x44, 0x8B, 0x15, 0, 0, 0, 0,                 /* 18 mov r10d, [rip+length] (disp at 21) */
    0x44, 0x89, 0x15, 0, 0, 0, 0,                 /* 25 mov [rip+count], r10d (disp at 28) */
    0x41, 0x5A,                                   /* 32 pop r10 */
    0xE9, 0, 0, 0, 0};                            /* 34 jmp back (rel32 at 35) */

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
static int StratagemAddable(HfMod* mod, const HfStratagem* def, const char* name, int live)
{
    if (name == NULL || name[0] == 0 || strlen(name) >= sizeof g_stratagems[0].name) { ApiLog(mod, "add_stratagem: it needs a name (under 192 bytes)"); return 0; }
    if (!live && (g_stratagemState != 0 || g_fired[HF_EVENT_GAME_LOADED])) { ApiLog(mod, "add_stratagem(%s): too late; call it from hellforge_mod_init", name); return 0; }
    if (!live && g_stratagemCount >= STR_MAX_NEW) { ApiLog(mod, "add_stratagem(%s): mods added %u stratagems already", name, STR_MAX_NEW); return 0; }
    if (def->base == 0 || def->base >= STR_VANILLA) { ApiLog(mod, "add_stratagem(%s): base %u is not a vanilla stratagem type (1 .. 149)", name, def->base); return 0; }
    return 1;
}

/* 1 (logged) when a mod added a stratagem of this stable id or name already (one a live change takes out aside). */
static int StratagemTaken(HfMod* mod, const char* name, uint32_t stable)
{
    for (uint32_t i = 0; i < g_stratagemCount; i++)
        if (!LiveGone(LC_STRATAGEM, i) && (g_stratagems[i].stable == stable || strcmp(g_stratagems[i].name, name) == 0))
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
    int live = g_liveSetup != NULL && g_liveSetup == mod; /* a mod set up while the game runs (live_content.c) */
    if (!StratagemAddable(mod, def, name, live)) return 0;
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
    uint32_t index = g_stratagemCount;
    Stratagem* at = live ? (Stratagem*)LiveEntry(LC_STRATAGEM, mod, name, &index) : &g_stratagems[index];
    if (at == NULL) return 0;
    StratagemText(name, def, &s);
    s.type = STR_VANILLA + index;
    *at = s;
    MemoryBarrier(); /* the entry is whole before the count shows it */
    if (index == g_stratagemCount) g_stratagemCount++;
    ApiLog(mod, "adding stratagem %s as type %u (stable id %08x, item %08x): a copy of type %u", s.name, s.type, s.stable, s.item, s.base);
    if (!live) WriteRegisteredItems(); /* a live change writes it once made */
    return s.type;
}

/* Each stratagem's entry in registered-items.json: its catalogue item (mixId), its stable id (the kind-10 child's
 * itemId) and the stable id of the base it copies (RegisteredItems.cs copies the base's items). The base's stable id is
 * known once the game has loaded its stratagems, so the file is written again then; until then it is 0. */
static int StratagemItems(char* text, int n, int size, int first)
{
    char mod[6 * 80 + 1], name[6 * 192 + 1];
    for (uint32_t i = 0; i < g_stratagemCount && n < size - REG_ITEM_ROOM; i++)
    {
        Stratagem* s = &g_stratagems[i];
        if (LiveTomb(LC_STRATAGEM, i)) continue; /* its mod was taken out: no longer in the catalogue */
        uint32_t baseStable = 0;
        uint64_t base = 0;
        if (g_stratagemTable != NULL) memcpy(&base, g_stratagemTable + s->base * 8, 8);
        if (base != 0) SafeRead(base + 4, &baseStable, 4);
        n += snprintf(text + n, size - n, "%s{\"kind\":\"stratagem\",\"mixId\":%u,\"stable\":%u,\"copyOfType\":%u,\"copyOf\":%u,\"mod\":\"%s\",\"name\":\"%s\"}",
            first ? "" : ",", s->item, s->stable, s->base, baseStable, JsonText(s->mod->id, mod, sizeof mod), JsonText(s->name, name, sizeof name));
        first = 0;
    }
    return n;
}

static uintptr_t ApiStratagemRow(uint32_t type)
{
    if (g_stratagemState != 1) return 0;
    for (uint32_t i = 0; i < g_stratagemCount; i++) if (g_stratagems[i].type == type && !LiveTomb(LC_STRATAGEM, i)) return (uintptr_t)g_stratagems[i].row;
    return 0;
}

static int32_t TableDisplacement(uintptr_t table, uintptr_t game, const TableSite* s)
{
    return s->rip ? (int32_t)(int64_t)(table + s->slot * 8u - (game + s->rva + s->size)) : (int32_t)(int64_t)(table + s->slot * 8u - game);
}

/* NULL when every guarded instruction is the reviewed one. */
static const char* ChoiceGuardsFailed(uintptr_t game)
{
    uint8_t code[8];
    for (size_t i = 0; i < sizeof kChoiceGuards / sizeof kChoiceGuards[0]; i++)
        if (SafeRead(game + kChoiceGuards[i].rva, code, kChoiceGuards[i].length) || memcmp(code, kChoiceGuards[i].pin, kChoiceGuards[i].length) != 0)
            return "an instruction indexing the loadout's choices by type is not the reviewed one";
    if (SafeRead(game + STR_COUNT_STORE, code, sizeof kCountStorePin) || memcmp(code, kCountStorePin, sizeof kCountStorePin) != 0)
        return "the stratagem list's loader does not store its count as reviewed";
    return NULL;
}

static void PutRel32At(uint8_t* at, uintptr_t target) { int32_t rel = (int32_t)(int64_t)(target - ((uintptr_t)at + 4)); memcpy(at, &rel, 4); }

/* Sends the guarded instructions and the count's store through their stubs (the count's floor: the dword at length,
 * within 2 GB of game.dll); NULL, or why not. The stubs are complete before any site jumps to them. */
static const char* GuardStratagemTypes(uintptr_t game, const uint32_t* length, PutFn put)
{
    size_t guards = sizeof kChoiceGuards / sizeof kChoiceGuards[0];
    uint8_t* code = AllocateNearGame(game, guards * STR_GUARD_SLOT + sizeof kCountStub);
    if (code == NULL) return "no memory within 2 GB of game.dll for the type guards";
    for (size_t i = 0; i < guards; i++)
    {
        const ChoiceGuard* g = &kChoiceGuards[i];
        uint8_t* stub = code + i * STR_GUARD_SLOT, *at = stub;
        uintptr_t back = game + g->rva + g->length;
        memcpy(at, g->test, g->testLength); at += g->testLength;
        *at++ = 0x73; *at++ = (uint8_t)(g->length + 5); /* jae: a new type */
        memcpy(at, g->pin, g->length); at += g->length;  /* a vanilla type: the instruction */
        *at++ = 0xE9; PutRel32At(at, back); at += 4;
        memcpy(at, g->none, g->noneLength); at += g->noneLength;
        *at++ = 0xE9; PutRel32At(at, back);
    }
    uint8_t* stub = code + guards * STR_GUARD_SLOT;
    memcpy(stub, kCountStub, sizeof kCountStub);
    PutRel32At(stub + 3, game + STR_ROWS_LOADED);  /* each rip-relative operand ends its instruction: 4 bytes after it */
    PutRel32At(stub + 10, (uintptr_t)length);
    PutRel32At(stub + 21, (uintptr_t)length);
    PutRel32At(stub + 28, game + STR_ROWS_LOADED);
    PutRel32At(stub + 35, game + STR_COUNT_STORE + sizeof kCountStorePin);
    DWORD protect;
    if (!VirtualProtect(code, guards * STR_GUARD_SLOT + sizeof kCountStub, PAGE_EXECUTE_READ, &protect)) return "the type guards could not be made executable";
    for (size_t i = 0; i <= guards; i++)
    {
        uintptr_t site = i < guards ? game + kChoiceGuards[i].rva : game + STR_COUNT_STORE;
        uint8_t length = i < guards ? kChoiceGuards[i].length : (uint8_t)sizeof kCountStorePin;
        uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        int32_t to = (int32_t)(int64_t)((uintptr_t)(code + i * STR_GUARD_SLOT) - (site + 5)); /* from the site, not this copy */
        memcpy(jump + 1, &to, 4);
        if (put(site, jump, length)) return "an instruction indexing by stratagem type could not be guarded";
    }
    return NULL;
}

/* NULL when the picker's frame instructions and unwind data are the reviewed ones. */
static const char* PickerFrameFailed(uintptr_t game)
{
    uint8_t code[sizeof kPickerUnwindPin];
    for (size_t i = 0; i < sizeof kPickerFrame / sizeof kPickerFrame[0]; i++)
        if (SafeRead(game + kPickerFrame[i].rva, code, kPickerFrame[i].length) || memcmp(code, kPickerFrame[i].pin, kPickerFrame[i].length) != 0)
            return "the loadout picker's frame is not the reviewed one";
    if (SafeRead(game + STR_PICKER_UNWIND, code, sizeof code) || memcmp(code, kPickerUnwindPin, sizeof code) != 0)
        return "the loadout picker's unwind data is not the reviewed one";
    return NULL;
}

/* The picker's frame for `candidates` records and pointers, its unwind data first (it describes the frame as it will
 * be, and nothing unwinds the picker while it is not running); NULL, or why not. */
static const char* WidenPicker(uintptr_t game, uint32_t candidates, PutFn put)
{
    if (candidates > STR_PICKER_MAX) return "too many stratagem types for the loadout picker's frame";
    PickerFrame f = PickerFrameFor(candidates);
    uint16_t slots[4] = {(uint16_t)((f.frame + 0x48) / 8), (uint16_t)((f.frame + 0x40) / 8), (uint16_t)((f.frame + 0x38) / 8),
        (uint16_t)(f.frame / 8)};                            /* rsi, rbp, rbx, the allocation: codes 0..3, slot at +2 */
    for (int k = 0; k < 4; k++)
        if (put(game + STR_PICKER_UNWIND + 6 + 4 * k, &slots[k], 2)) return "the loadout picker's unwind data could not be widened";
    uint32_t cookie = f.cookie | (kPickerUnwindPin[36] & 7u); /* the handler data's flag bits stay */
    if (put(game + STR_PICKER_UNWIND + 36, &cookie, 4)) return "the loadout picker's unwind data could not be widened";
    for (size_t i = 0; i < sizeof kPickerFrame / sizeof kPickerFrame[0]; i++)
    {
        uint32_t value = kPickerFrame[i].which == 0 ? f.frame : kPickerFrame[i].which == 1 ? f.cookie : f.pointers;
        if (put(game + kPickerFrame[i].rva + kPickerFrame[i].at, &value, 4)) return "the loadout picker's frame could not be widened";
    }
    return NULL;
}

/* Co-op: a stratagem type travels as field type 0xC2EB5C15 (an 8-bit integer from 0: types up to 255), in seven RPCs
 * and 463 replicated fields, all sharing that one row; each codec reads its width afresh (research
 * content-limits\stratagems-wide C). Past 256 types it travels in 16 bits: at start once the configuration loads, before
 * any session, or in a live change (its gate: no friend in the game). Every peer with the same mods registers the same
 * types, so every one picks the same width. It never narrows again: types a live change takes out stay as tombs. The
 * type's name is not known, so mods cannot ask for it (nettypes.c), and nothing else writes its width. */
#define STR_FIELD_HASH 0xC2EB5C15u
static int g_stratagemField; /* 0 not yet, 1 fine, -1 refused */

/* For `types` types: 1 the field holds them, 0 refused (logged). */
static int StratagemFieldHolds(uint32_t types, PutFn put)
{
    if (types <= 256u) return 1;
    uintptr_t row = FindFieldTypeHash(STR_FIELD_HASH);
    uint8_t bytes[0x18];
    int32_t min = -1;
    if (row != 0 && !SafeRead(row, bytes, sizeof bytes)) memcpy(&min, bytes + 16, 4);
    if (row == 0 || min != 0 || bytes[12] != 1 || (bytes[13] != 8 && bytes[13] != 16))
    {
        Note("stratagems: the stratagem type's network field is not the reviewed one; %u types do not fit its 8 bits", types);
        return 0;
    }
    uint8_t bits = 16;
    if (bytes[13] == 16) return 1;
    if (put(row + 13, &bits, 1)) return 0;
    Note("stratagems: %u types; the stratagem type travels in 16 bits in co-op (8 in vanilla)", types);
    return 1;
}

/* HF_EVENT_NETWORK_READY (WatchNetwork): the field for the types mods added at start. */
static void WidenStratagemField(void)
{
    if (g_stratagemField == 0) g_stratagemField = StratagemFieldHolds(STR_VANILLA + g_stratagemCount, ApiWrite) ? 1 : -1;
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
    if (why == NULL) why = ChoiceGuardsFailed(game);
    if (why == NULL) why = PickerFrameFailed(game);
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

/* The moved list's block, within 2 GB of game.dll where the instructions' 32-bit displacements reach it: the list (the
 * vanilla pointers copied), the names (one more: "Count") and the list's length (the loader's count stub reads it). The
 * new types' rows go anywhere (the list holds 64-bit pointers to them; 400 KB would take much of the near arena every
 * moved table shares): a row for every new type HellForge has room for, StratagemNewRow. NULL, and why. */
#define STR_NAMES_AT (STR_TYPES * 8)
#define STR_LENGTH_AT (STR_NAMES_AT + STR_TYPES * 8 + 8)
static uint64_t g_stratagemCountName; /* the names' "Count", after the last type */
static uint8_t* g_stratagemRows;
static uint8_t* StratagemNewRow(uint32_t index) { return g_stratagemRows + (size_t)index * STR_ROW; }
static uint8_t* StratagemBlock(uintptr_t game, const char** why)
{
    *why = "no memory for the new stratagems' rows";
    if (g_stratagemRows == NULL) g_stratagemRows = (uint8_t*)VirtualAlloc(NULL, (size_t)STR_MAX_NEW * STR_ROW, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_stratagemRows == NULL) return NULL;
    uint8_t* table = AllocateNearGame(game, STR_LENGTH_AT + 8);
    *why = "no memory within 2 GB of game.dll for the moved list";
    if (table == NULL) return NULL;
    *why = "the stratagem list is unreadable";
    if (SafeRead(game + STR_TABLE, table, STR_VANILLA * 8)) return NULL;
    *why = "the stratagem names are unreadable";
    if (SafeRead(game + STR_NAMES, table + STR_NAMES_AT, (STR_VANILLA + 1) * 8)) return NULL;
    memcpy(&g_stratagemCountName, table + STR_NAMES_AT + STR_VANILLA * 8, 8);
    return table;
}

/* A new stratagem's row (its base's, from the list in table, with what the mod gave it), the row's pointers into home,
 * where the entry lives for the session (s itself, or for a live change the entry s stands in for). NULL, or why not. */
static const char* StratagemRow(Stratagem* s, Stratagem* home, const uint8_t* table)
{
    uint64_t base;
    memcpy(&base, table + s->base * 8, 8);
    if (base == 0 || SafeRead(base, s->row, STR_ROW) || *(uint32_t*)s->row != s->base) return "a base stratagem row does not name its type";
    if (!(s->row[0x80] & 2) || !(s->row[0xC0] & 1)) return "a base stratagem is not one players can pick";
    memcpy(s->row, &s->type, 4);
    memcpy(s->row + 4, &s->stable, 4);
    for (int t = 0; t < 3; t++) if (s->text[t] != 0) memcpy(s->row + 0x28 + 4 * t, &s->text[t], 4);
    if (s->codeLength != 0)
    {
        uint32_t* code = home->code; /* lives as long as the loader */
        memcpy(s->row + 0x40, &code, 8);
        memcpy(s->row + 0x48, &s->codeLength, 4);
    }
    if (s->uses != 0) { uint32_t uses = s->uses < 0 ? 0xFFFFFFFFu : (uint32_t)s->uses; memcpy(s->row + 0x50, &uses, 4); }
    if (s->cooldown > 0.0f) memcpy(s->row + 0x68, &s->cooldown, 4);
    if (s->icon != 0) memcpy(s->row + 0xB0, &s->icon, 8);
    if (s->payloadCount != 0)
    {
        uint64_t* payload = home->payload; /* lives as long as the loader */
        memcpy(s->row + 0x98, &payload, 8);
        memcpy(s->row + 0xA0, &s->payloadCount, 4);
        memset(s->row + 0xA4, 0, 4);
    }
    if (s->package != 0) { uint64_t package = s->package == ~0ull ? 0 : s->package; memcpy(s->row + 0xA8, &package, 8); }
    if (s->loadsItem != 0) memcpy(s->row + 0xF8, &s->loadsItem, 4);
    if (s->category != 0) { uint32_t category = s->category - 1; memcpy(s->row + 0xB8, &category, 4); }
    if (s->beam != 0) memcpy(s->row + 0xD4, &s->beam, 4);
    if (s->flags != 0) memcpy(s->row + 0x104, &s->flags, 4);
    memset(s->row + 0x118, 0, 12); /* no level packages: nothing chooses one in the loadout's per-type choice (+0x78C[150]) */
    return NULL;
}

/* The instructions that address the list and its names, pointed at HellForge's block. NULL, or why not. */
static const char* StratagemMove(uintptr_t game, const uint8_t* table, PutFn put)
{
    for (size_t i = 0; i < sizeof kStratagemSites / sizeof kStratagemSites[0]; i++)
    {
        int32_t disp = TableDisplacement((uintptr_t)table, game, &kStratagemSites[i]);
        if (put(game + kStratagemSites[i].rva + kStratagemSites[i].dispAt, &disp, 4))
        {
            Note("stratagems: could not rewrite the instruction at 0x%x; %zu of %zu moved", kStratagemSites[i].rva, i, sizeof kStratagemSites / sizeof kStratagemSites[0]);
            return "an instruction could not be rewritten";
        }
    }
    for (size_t i = 0; i < sizeof kStratagemNameSites / sizeof kStratagemNameSites[0]; i++)
    {
        int32_t disp = (int32_t)(int64_t)((uintptr_t)table + STR_NAMES_AT - game);
        if (put(game + kStratagemNameSites[i].rva + kStratagemNameSites[i].dispAt, &disp, 4)) return "an instruction reading the names could not be rewritten";
    }
    return NULL;
}

/* The rows loaded, the picker and every bound raised to count types (the block's length with them). first, once: the
 * guards, then the picker's frame for every type HellForge has room for, and its clear of that many records. And past
 * 256 types the co-op field. NULL, or why not. */
static const char* StratagemRaise(uintptr_t game, uint8_t* table, uint32_t count, int first, PutFn put)
{
    const char* why;
    /* Before the count and the bounds let code reach a new type: nothing then indexes the choices past their end. */
    if (first && (why = GuardStratagemTypes(game, (const uint32_t*)(table + STR_LENGTH_AT), put)) != NULL) return why;
    if (!StratagemFieldHolds(count, put)) return "their types do not fit the network's 8-bit stratagem field";
    if (put((uintptr_t)table + STR_LENGTH_AT, &count, 4) || put(game + STR_ROWS_LOADED, &count, 4)) return "the row count could not be raised";
    if (first)
    {
        uint8_t pad[sizeof kPickerPad];
        uint32_t records = STR_TYPES;
        memcpy(pad, kPickerPad, sizeof pad);
        memcpy(pad + 1, &records, 4);
        if ((why = WidenPicker(game, STR_TYPES, put)) != NULL) return why;
        if (put(game + STR_PICKER_PAD, pad, sizeof pad)) return "the loadout picker could not be widened";
    }
    if (put(game + STR_PICKER_BOUND + 1, &count, 4)) return "the loadout picker could not be widened";
    for (size_t i = 0; i < sizeof kStratagemBounds / sizeof kStratagemBounds[0]; i++)
    {
        uint32_t value = kStratagemBounds[i].value + (count - STR_VANILLA); /* 150 -> count, 149 -> count - 1 */
        if (put(game + kStratagemBounds[i].rva + kStratagemBounds[i].immAt, &value, kStratagemBounds[i].width))
        {
            Note("stratagems: could not widen the bound at 0x%x", kStratagemBounds[i].rva);
            return "a bound could not be widened";
        }
    }
    return NULL;
}

/* The moved list in use. categories: install the selection categories now (a live change does on the game thread). */
static void StratagemsInstalled(uint8_t* table, uintptr_t game, uint32_t count, int categories)
{
    g_stratagemTable = table;
    g_stratagemState = 1;
    WriteRegisteredItems();
    HookText();
    if (categories) InstallStratagemCategories(game);
    Note("stratagems: the list moved to 0x%llx (%zu instructions, %zu bounds) with %u new stratagem(s); %u types",
        (unsigned long long)(uintptr_t)table, sizeof kStratagemSites / sizeof kStratagemSites[0],
        sizeof kStratagemBounds / sizeof kStratagemBounds[0], g_stratagemCount, count);
    for (uint32_t i = 0; i < g_stratagemCount; i++)
        ApiLog(g_stratagems[i].mod, "stratagem %s is type %u (stable id %08x, icon %016llx)", g_stratagems[i].name, g_stratagems[i].type,
            g_stratagems[i].stable, (unsigned long long)(g_stratagems[i].icon ? g_stratagems[i].icon : *(uint64_t*)(g_stratagems[i].row + 0xB0)));
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
    /* Past 256 types, the co-op field is widened first (WidenStratagemField, when the network configuration loads). */
    if (STR_VANILLA + g_stratagemCount > 256u && g_stratagemField == 0) return 0;
    if (g_stratagemField < 0) return RefuseStratagems("their types do not fit the network's 8-bit stratagem field");
    uint8_t* table = StratagemBlock(game, &why);
    if (table == NULL) return RefuseStratagems(why);
    uint8_t* names = table + STR_NAMES_AT;
    memcpy(names + (STR_VANILLA + g_stratagemCount) * 8, &g_stratagemCountName, 8); /* "Count" after the new ones */
    for (uint32_t i = 0; i < g_stratagemCount; i++)
    {
        Stratagem* s = &g_stratagems[i];
        s->row = StratagemNewRow(i);
        if ((why = StratagemRow(s, s, table)) != NULL) return RefuseStratagems(why);
        const char* name = s->name;
        memcpy(names + s->type * 8, &name, 8);
    }
    /* The new rows first (no code reaches them yet), then the instructions, then the bounds that let code reach them. */
    for (uint32_t i = 0; i < g_stratagemCount; i++) memcpy(table + g_stratagems[i].type * 8, &g_stratagems[i].row, 8);
    uint32_t count = STR_VANILLA + g_stratagemCount;
    if ((why = StratagemMove(game, table, ApiWrite)) != NULL || (why = StratagemRaise(game, table, count, 1, ApiWrite)) != NULL) return RefuseStratagems(why);
    StratagemsInstalled(table, game, count, 1);
    return 1;
}

/* Once the game has loaded its stratagems; code still being decoded gets 20 s. */
static void WatchStratagems(void)
{
    if (!g_fired[HF_EVENT_GAME_LOADED] || g_stratagemState != 0) return;
    static ULONGLONG stratagemsSeen;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    uint64_t first = 0;
    if (game == 0 || SafeRead(game + STR_TABLE + 8, &first, 8) || first == 0) return;
    if (stratagemsSeen == 0) stratagemsSeen = GetTickCount64();
    if (g_game == 0) g_game = game;
    InstallStratagems(game, GetTickCount64() - stratagemsSeen >= 20000);
}
