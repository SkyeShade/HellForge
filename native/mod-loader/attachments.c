/* Weapon attachments of mods' own (SDK 17, add_attachment; docs/modding/content.md "Attachments of your own"). Included
 * by packages.c, whose game-thread update runs AttachmentsTick. game.dll (25480438) keeps items in g_settings
 * (+0x37C5B50): up to 32 pointers to {items*, u32 count} blocks of 88-byte items, the count at +0x348DA98 read on every
 * call. A weapon's chosen item ids become keys (each item's add_path, +0x20) into the entity deltas, [state + 0xF12E98].
 * An attachment copies no item: its item is written from its declaration into HellForge's block, and its deltas (its
 * slot's unit field, then its changes) are added under its own add_path to a grown copy of the delta storage. Both are
 * made again when the game reloads. */

#define ATT_MAX 128
#define ATT_WEAPONS 16
#define ATT_TABLE 0x37C5B50u
#define ATT_COUNT 0x348DA98u
#define ATT_SLOTS 32u
#define ATT_DELTAS 0xF12E98u
#define ATT_ITEM 0x58u
#define ATT_ITEM_LOOKUP 0x11F46F0u
#define ATT_DELTA_LOOKUP 0x5154C0u
#define ATT_CUSTOMIZATION 271u
#define ATT_CHANGES 17 /* its unit, and up to 16 changes */

static const uint8_t kAttItemPin[24] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x7c, 0x24, 0x10, 0x8b, 0x3d, 0x98, 0x93, 0x29, 0x02,
    0x45, 0x33, 0xd2, 0x85, 0xff, 0x74, 0x3b, 0x4c};
static const uint8_t kAttDeltaPin[30] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x8b, 0x05, 0xcc, 0x6a, 0xf5, 0x02, 0x33, 0xd2, 0x45, 0x33,
    0xc0, 0x4c, 0x8b, 0xd9, 0x48, 0x8b, 0x98, 0x98, 0x2e, 0xf1, 0x00, 0x48, 0x8b, 0xc1};

/* One delta: bytes at offset of the weapon's record of component. */
typedef struct AttDelta { uint16_t component, offset, size; uint8_t bytes[16]; } AttDelta;

typedef struct Attachment
{
    HfMod* mod;
    char name[96];
    uint32_t id, slot, level, price;
    uint64_t addPath, unit;
    AttDelta deltas[ATT_CHANGES];
    uint32_t deltaCount;
    uint32_t text[3];
    uint32_t weapons[ATT_WEAPONS];
    uint32_t weaponCount;
    int state;              /* 0 waiting, 1 in the game, -1 refused (logged) */
} Attachment;
static Attachment g_attachments[ATT_MAX];
static int g_attachmentCount;
static uint8_t g_attItems[ATT_MAX * ATT_ITEM];
static struct { uint8_t* items; uint32_t count; uint32_t pad; } g_attBlock = {g_attItems, 0, 0};
static uint8_t* g_attGrownFrom;   /* the game's delta storage the grown one copies */
static uint8_t* g_attGrown;       /* HellForge's, swapped in */

static uint32_t Thin(const char* text) { return (uint32_t)(Murmur64A(text, strlen(text)) >> 32); }

/* A vanilla item by id from the game's blocks (not HellForge's), or NULL. */
static const uint8_t* AttVanillaItem(uintptr_t game, uint32_t id)
{
    uint32_t count = *(volatile uint32_t*)(game + ATT_COUNT);
    for (uint32_t k = 0; k < count && k < ATT_SLOTS; k++)
    {
        const uint8_t* block = *(uint8_t* const*)(game + ATT_TABLE + 8u * k);
        if (block == NULL || block == (const uint8_t*)&g_attBlock) continue;
        const uint8_t* items = *(uint8_t* const*)block;
        uint32_t n = *(const uint32_t*)(block + 8);
        for (uint32_t i = 0; items != NULL && i < n; i++)
            if (*(const uint32_t*)(items + (size_t)i * ATT_ITEM + 8) == id) return items + (size_t)i * ATT_ITEM;
    }
    return NULL;
}

/* The settings entry {count, first} of a delta key, or NULL (0x5154C0's search). */
static const uint32_t* AttDeltaSetting(const uint8_t* s, uint64_t key)
{
    const uint8_t* keys = *(uint8_t* const*)s;
    uint32_t slots = *(const uint32_t*)(s + 8);
    for (uint32_t probe = 0, at = slots ? (uint32_t)(key % slots) : 0; probe < slots; probe++, at = at + 1 == slots ? 0 : at + 1)
    {
        uint64_t k = *(const uint64_t*)(keys + 16u * at);
        if (k == key) return (const uint32_t*)(*(uint8_t* const*)(s + 0x10)) + 2u * *(const uint32_t*)(keys + 16u * at + 8);
        if (k == 0) break;
    }
    return NULL;
}

/* The storage's sizes, from what its keys reach: keys, settings, component settings, deltas and data bytes. */
typedef struct AttSizes { uint32_t keys, settings, comps, deltas, data; } AttSizes;
static AttSizes AttMeasure(const uint8_t* s)
{
    AttSizes z = {0};
    const uint8_t* keys = *(uint8_t* const*)s;
    const uint32_t* settings = *(uint32_t* const*)(s + 0x10);
    const uint32_t* comps = *(uint32_t* const*)(s + 0x20);
    const uint32_t* deltas = *(uint32_t* const*)(s + 0x30);
    for (uint32_t i = 0; i < *(const uint32_t*)(s + 8); i++)
        if (*(const uint64_t*)(keys + 16u * i)) { z.keys++; uint32_t x = *(const uint32_t*)(keys + 16u * i + 8) + 1; if (x > z.settings) z.settings = x; }
    for (uint32_t i = 0; i < z.settings; i++) if (settings[2 * i] + settings[2 * i + 1] > z.comps) z.comps = settings[2 * i] + settings[2 * i + 1];
    for (uint32_t i = 0; i < z.comps; i++) if (comps[3 * i + 1] + comps[3 * i + 2] > z.deltas) z.deltas = comps[3 * i + 1] + comps[3 * i + 2];
    for (uint32_t i = 0; i < z.deltas; i++) if (deltas[3 * i + 2] + deltas[3 * i + 1] > z.data) z.data = deltas[3 * i + 2] + deltas[3 * i + 1];
    return z;
}

/* One attachment's deltas under its add_path: its settings entry, then a component setting per component it changes
 * (in component order) with its deltas, and their bytes. */
static void AttWriteDeltas(const Attachment* a, uint32_t* settings, uint32_t* comps, uint32_t* deltas, uint8_t* data, AttSizes* z)
{
    settings[2 * z->settings + 1] = z->comps;
    uint32_t count = 0;
    for (uint32_t c = 0, last = 0; ; last = c + 1)
    {
        c = 0xFFFFFFFFu; /* the next component in order */
        for (uint32_t d = 0; d < a->deltaCount; d++) if (a->deltas[d].component >= last && a->deltas[d].component < c) c = a->deltas[d].component;
        if (c == 0xFFFFFFFFu) break;
        uint32_t* nc = comps + 3u * z->comps++;
        nc[0] = c; nc[1] = z->deltas; nc[2] = 0;
        for (uint32_t d = 0; d < a->deltaCount; d++)
        {
            const AttDelta* x = &a->deltas[d];
            if (x->component != c) continue;
            uint32_t* nd = deltas + 3u * z->deltas++;
            nd[0] = x->offset; nd[1] = x->size; nd[2] = z->data;
            memcpy(data + z->data, x->bytes, x->size);
            z->data += x->size;
            nc[2]++;
        }
        count++;
    }
    settings[2 * z->settings] = count;
    z->settings++;
}

/* A key in the grown map: open addressing from key % slots, as 0x5154C0 searches. */
static void AttPut(uint8_t* keys, uint32_t slots, uint64_t key, uint32_t index)
{
    uint32_t at = (uint32_t)(key % slots);
    while (*(const uint64_t*)(keys + 16u * at) != 0) at = at + 1 == slots ? 0 : at + 1;
    memcpy(keys + 16u * at, &key, 8);
    memcpy(keys + 16u * at + 8, &index, 4);
}

/* What one attachment adds: component settings, deltas and data bytes. */
static AttSizes AttExtra(const Attachment* a)
{
    AttSizes one = {0};
    for (uint32_t d = 0; d < a->deltaCount; d++)
    {
        int first = 1;
        for (uint32_t e = 0; e < d; e++) first &= a->deltas[e].component != a->deltas[d].component;
        one.comps += (uint32_t)first;
        one.deltas++;
        one.data += a->deltas[d].size;
    }
    return one;
}

/* The grown storage's header: the game's 0x50 bytes with its own pointers and slot count; a word after a pointer that
 * equals the old count of that array takes the new count (what those words are is not known). */
static void AttHeader(uint8_t* g, const uint8_t* s, void* const* pointers, uint32_t slots, const AttSizes* old, const AttSizes* now)
{
    memcpy(g, s, 0x50);
    for (int p = 0; p < 5; p++) memcpy(g + 0x10 * p, &pointers[p], 8);
    memcpy(g + 8, &slots, 4);
    const uint32_t before[4] = {old->settings, old->comps, old->deltas, old->data}, after[4] = {now->settings, now->comps, now->deltas, now->data};
    for (int p = 1; p < 5; p++)
        if (*(const uint32_t*)(s + 0x10 * p + 8) == before[p - 1]) memcpy(g + 0x10 * p + 8, &after[p - 1], 4);
}

/* A grown copy of the delta storage with these attachments' deltas (one VirtualAlloc block), or NULL. */
static uint8_t* AttGrow(const uint8_t* s, Attachment* const* list, int n)
{
    AttSizes old = AttMeasure(s), add = {0}, z;
    for (int i = 0; i < n; i++) { AttSizes one = AttExtra(list[i]); add.comps += one.comps; add.deltas += one.deltas; add.data += one.data; }
    uint32_t slots = 2 * (old.keys + (uint32_t)n) + 1;
    const size_t parts[5] = {16u * slots, 8u * (old.settings + (uint32_t)n), 12u * (old.comps + add.comps), 12u * (old.deltas + add.deltas), (size_t)old.data + add.data};
    size_t at[5], size = 0x100;
    for (int p = 0; p < 5; p++) { at[p] = size; size += (parts[p] + 15) & ~(size_t)15; }
    uint8_t* g = (uint8_t*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g == NULL) return NULL;
    void* pointers[5];
    for (int p = 0; p < 5; p++) pointers[p] = g + at[p];
    memcpy(pointers[1], *(uint8_t* const*)(s + 0x10), 8u * old.settings);
    memcpy(pointers[2], *(uint8_t* const*)(s + 0x20), 12u * old.comps);
    memcpy(pointers[3], *(uint8_t* const*)(s + 0x30), 12u * old.deltas);
    memcpy(pointers[4], *(uint8_t* const*)(s + 0x40), old.data);
    const uint8_t* keys = *(uint8_t* const*)s;
    for (uint32_t i = 0; i < *(const uint32_t*)(s + 8); i++)
        if (*(const uint64_t*)(keys + 16u * i)) AttPut(pointers[0], slots, *(const uint64_t*)(keys + 16u * i), *(const uint32_t*)(keys + 16u * i + 8));
    z = old;
    for (int i = 0; i < n; i++)
    {
        AttPut(pointers[0], slots, list[i]->addPath, z.settings);
        AttWriteDeltas(list[i], pointers[1], pointers[2], pointers[3], pointers[4], &z);
    }
    AttHeader(g, s, pointers, slots, &old, &z);
    return g;
}

/* The attachments' items in HellForge's block, written from their declarations: debug name, id, text, add_path and
 * their one slot; no icon, colours or sort group, as the game's optics, muzzles and magazines have none. Returns how
 * many. */
static uint32_t AttItems(void)
{
    uint32_t n = 0;
    for (int i = 0; i < g_attachmentCount; i++)
    {
        Attachment* a = &g_attachments[i];
        if (a->state != 1) continue;
        uint8_t* row = g_attItems + (size_t)n++ * ATT_ITEM;
        const char* name = a->name;
        const uint32_t* slot = &a->slot;
        uint64_t one = 1;
        memset(row, 0, ATT_ITEM);
        memcpy(row, &name, 8);
        memcpy(row + 8, &a->id, 4);
        memcpy(row + 0x0C, &a->text[1], 4);
        memcpy(row + 0x10, &a->text[0], 4);
        memcpy(row + 0x14, &a->text[2], 4);
        memcpy(row + 0x20, &a->addPath, 8);
        memcpy(row + 0x30, &slot, 8);
        memcpy(row + 0x38, &one, 8);
    }
    return n;
}

/* Why an attachment cannot go in, or NULL. */
static const char* AttWhy(uintptr_t game, const uint8_t* s, const Attachment* a)
{
    if (AttVanillaItem(game, a->id) != NULL) return "its id is a game item's";
    if (AttDeltaSetting(s, a->addPath) != NULL) return "its key is taken in the entity deltas";
    return NULL;
}

/* The waiting attachments that can go in; the others refused (logged). Returns how many. */
static int AttResolve(uintptr_t game, const uint8_t* s, Attachment** ready)
{
    int n = 0;
    for (int i = 0; i < g_attachmentCount; i++)
    {
        Attachment* a = &g_attachments[i];
        if (a->state != 0) continue;
        const char* why = AttWhy(game, s, a);
        if (why != NULL) { a->state = -1; ApiLog(a->mod, "add_attachment(%s): %s", a->name, why); continue; }
        ready[n++] = a;
    }
    return n;
}

/* Whether the game's item and delta lookups read as reviewed (once; logged when not). */
static int AttProven(uintptr_t game)
{
    static int proven;
    if (!proven)
    {
        proven = PinMatches(game + ATT_ITEM_LOOKUP, kAttItemPin, sizeof kAttItemPin) && PinMatches(game + ATT_DELTA_LOOKUP, kAttDeltaPin, sizeof kAttDeltaPin) ? 1 : -1;
        if (proven < 0) Note("attachments: the game's item or delta lookup is not the reviewed one; no attachments");
    }
    return proven > 0;
}

/* A delta storage HellForge has not grown: every attachment checked again and the ready ones' deltas swapped in. */
static void AttDeltasTick(uintptr_t game, uint8_t* state, uint8_t* s)
{
    if (s == g_attGrown || s == g_attGrownFrom) return;
    for (int i = 0; i < g_attachmentCount; i++) if (g_attachments[i].state == 1) g_attachments[i].state = 0; /* a new load: all again */
    Attachment* ready[ATT_MAX];
    int n = AttResolve(game, s, ready);
    uint8_t* grown = n ? AttGrow(s, ready, n) : NULL;
    for (int i = 0; i < n; i++) ready[i]->state = grown ? 1 : -1;
    WriteRegisteredItems(); /* the refused are not for the backend */
    if (grown == NULL) return;
    g_attGrownFrom = s;
    g_attGrown = grown; /* the old storage stays: a reader may hold it */
    SetStatePointer(state, ATT_DELTAS, grown);
    Note("attachments: %d added to the entity deltas", n);
}

/* HellForge's item block appended to g_settings when it is not there (the first time, or after the game's reload). */
static void AttItemsTick(uintptr_t game, uint32_t count)
{
    for (uint32_t k = 0; k < count; k++) if (*(uint8_t* const*)(game + ATT_TABLE + 8u * k) == (uint8_t*)&g_attBlock) return;
    g_attBlock.count = AttItems();
    if (g_attBlock.count == 0) return;
    *(uint8_t**)(game + ATT_TABLE + 8u * count) = (uint8_t*)&g_attBlock;
    MemoryBarrier(); /* the block is in its slot before the count shows it */
    *(volatile uint32_t*)(game + ATT_COUNT) = count + 1;
}

static void AttachmentsStandaloneCheck(void);

/* On the game thread, once the game has its items and entity deltas. */
static void AttachmentsTick(void)
{
    if (g_fired[HF_EVENT_ENTITIES_READY]) AttachmentsStandaloneCheck(); /* add_attachment is refused from then on */
    uintptr_t game = g_game;
    if (g_attachmentCount == 0 || game == 0 || !AttProven(game)) return;
    uint8_t* state = EntityState();
    uint8_t* s = StatePointer(state, ATT_DELTAS);
    uint32_t count = *(volatile uint32_t*)(game + ATT_COUNT);
    if (s == NULL || count == 0 || count >= ATT_SLOTS) return;
    AttDeltasTick(game, state, s);
    AttItemsTick(game, count);
}

/* Its weapons: loadout ids add_weapon accepted. 1 if refused (logged). */
static int AttWeapons(HfMod* mod, const HfAttachment* def, Attachment* a)
{
    if (def->weapons == NULL || def->weapon_count == 0 || def->weapon_count > ATT_WEAPONS)
    { ApiLog(mod, "add_attachment(%s): it needs 1 to %d weapons", a->name, ATT_WEAPONS); return 1; }
    for (uint32_t i = 0; i < def->weapon_count; i++)
    {
        int found = 0;
        for (int w = 0; w < g_weaponCount && !found; w++) found = g_weapons[w].id == def->weapons[i] && !g_weapons[w].plain;
        if (!found) { ApiLog(mod, "add_attachment(%s): weapon %08x is not one add_weapon added", a->name, def->weapons[i]); return 1; }
        a->weapons[a->weaponCount++] = def->weapons[i];
    }
    return 0;
}

/* 1 (logged) when another attachment has its id or add_path. */
static int AttTaken(HfMod* mod, const Attachment* a)
{
    for (int i = 0; i < g_attachmentCount; i++)
        if (g_attachments[i].id == a->id || g_attachments[i].addPath == a->addPath)
        { ApiLog(mod, "add_attachment(%s): its id or unit is %s's already", a->name, g_attachments[i].name); return 1; }
    return 0;
}

/* Its own text ids: name, its upper-case copy and description, those the mod gives. */
static void AttText(const HfAttachment* def, const char* key, Attachment* a)
{
    if (def->name_text != NULL && def->name_text[0])
    {
        char upper[192];
        UpperCopy(def->name_text, upper, sizeof upper);
        a->text[0] = AddText(def->name_text, key, 0x11);
        a->text[1] = AddText(upper, key, 0x12);
    }
    if (def->description != NULL && def->description[0]) a->text[2] = AddText(def->description, key, 0x13);
}

/* The weapon record field that draws a slot's unit (WeaponCustomization OpticsPath, MuzzlePath, MagazinePath), or 0. */
static uint16_t AttUnitField(uint32_t slot) { return slot == HF_SLOT_OPTICS ? 0x78 : slot == HF_SLOT_MUZZLE ? 0xA0 : slot == HF_SLOT_MAGAZINE ? 0x80 : 0; }

static int AttChangeBytes(HfMod* mod, const char* name, const HfField* f, const HfValue* v, AttDelta* out); /* fields.c */

/* Its slot, unit and changes as deltas; 1 if refused (logged). */
static int AttDeltas(HfMod* mod, const HfAttachment* def, Attachment* a)
{
    static const uint32_t kSlots[] = {HF_SLOT_OPTICS, HF_SLOT_MUZZLE, HF_SLOT_MAGAZINE, HF_SLOT_INTERNALS, HF_SLOT_TRIGGER};
    int known = 0;
    for (size_t k = 0; k < sizeof kSlots / sizeof kSlots[0]; k++) known |= def->slot == kSlots[k];
    const char* why = !known ? "its slot is not an optic, muzzle, magazine, internals or trigger slot"
        : def->unit && !AttUnitField(def->slot) ? "a unit is for an optic, muzzle or magazine"
        : def->change_count > ATT_CHANGES - 1 || (def->change_count && def->changes == NULL) ? "it has more than 16 changes" : NULL;
    if (why != NULL) { ApiLog(mod, "add_attachment(%s): %s", a->name, why); return 1; }
    a->slot = def->slot;
    if (def->unit)
    {
        AttDelta* d = &a->deltas[a->deltaCount++];
        *d = (AttDelta){ATT_CUSTOMIZATION, AttUnitField(def->slot), 8, {0}};
        memcpy(d->bytes, &def->unit, 8);
    }
    for (uint32_t c = 0; c < def->change_count; c++)
        if (AttChangeBytes(mod, a->name, &def->changes[c].field, &def->changes[c].value, &a->deltas[a->deltaCount++])) return 1;
    return 0;
}

static uint32_t ApiAddAttachment(HfMod* mod, const HfAttachment* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfAttachment, change_count) + sizeof def->change_count)
    { ApiLog(mod, "add_attachment: no attachment, or its size is not set (HfAttachment a = {sizeof a})"); return 0; }
    if (def->name == NULL || def->name[0] == 0 || strlen(def->name) >= sizeof g_attachments[0].name) { ApiLog(mod, "add_attachment: it needs a name (under 96 bytes)"); return 0; }
    if (g_fired[HF_EVENT_ENTITIES_READY]) { ApiLog(mod, "add_attachment(%s): too late; call it from hellforge_mod_init", def->name); return 0; }
    if (g_attachmentCount == ATT_MAX) { ApiLog(mod, "add_attachment(%s): mods added %d attachments already", def->name, ATT_MAX); return 0; }
    Attachment a = {0};
    a.mod = mod;
    a.id = Thin(def->name);
    a.unit = def->unit;
    a.level = def->level;
    a.price = def->size >= offsetof(HfAttachment, price) + sizeof def->price ? def->price : 0;
    snprintf(a.name, sizeof a.name, "%s", def->name);
    char key[160];
    snprintf(key, sizeof key, "hellforge/attachment/%s", def->name);
    a.addPath = Murmur64A(key, strlen(key));
    if (a.id == 0 || AttTaken(mod, &a) || AttWeapons(mod, def, &a) || AttDeltas(mod, def, &a)) return 0;
    AttText(def, key, &a);
    g_attachments[g_attachmentCount] = a;
    MemoryBarrier();
    g_attachmentCount++;
    ApiLog(mod, "adding attachment %s (item %08x, slot %u, level %u, %u change(s)) on %u weapon(s)", a.name, a.id, a.slot, a.level, a.deltaCount, a.weaponCount);
    WriteRegisteredItems();
    return a.id;
}

/* The attachments for the backend (RegisteredItems.cs): {"mixId", "standalone", "weapons", "level", "price"}, the refused
 * left out. */
static int AttachmentItems(char* text, int n, int size)
{
    n += snprintf(text + n, size - n, ",\"attachments\":[");
    int listed = 0;
    for (int i = 0; i < g_attachmentCount && n < size - REG_ITEM_ROOM; i++)
    {
        const Attachment* a = &g_attachments[i];
        if (a->state < 0) continue;
        n += snprintf(text + n, size - n, "%s{\"mixId\":%u,\"standalone\":true,\"weapons\":[", listed++ ? "," : "", a->id);
        for (uint32_t w = 0; w < a->weaponCount; w++) n += snprintf(text + n, size - n, "%s%u", w ? "," : "", a->weapons[w]);
        n += snprintf(text + n, size - n, "],\"level\":%u,\"price\":%u}", a->level, a->price);
    }
    return n + snprintf(text + n, size - n, "]");
}

/* Once mods can add no more attachments: a standalone weapon with no attachment above level 1 is logged. The Armory offers
 * CUSTOMIZE only for a weapon whose mastery package sells something (game.dll 0x191d6b0); defaults are not sold. */
static int g_attChecked;
static void AttachmentsStandaloneCheck(void)
{
    if (g_attChecked) return;
    g_attChecked = 1;
    for (int w = 0; w < g_weaponCount; w++)
    {
        const Weapon* weapon = &g_weapons[w];
        if (!weapon->standalone || weapon->plain || weapon->state == -1) continue;
        int sold = 0;
        for (int i = 0; i < g_attachmentCount && !sold; i++)
            for (uint32_t k = 0; k < g_attachments[i].weaponCount && g_attachments[i].state >= 0 && g_attachments[i].level >= 2; k++)
                sold |= g_attachments[i].weapons[k] == weapon->id;
        if (!sold) ApiLog(weapon->mod, "add_weapon(%s): no attachment above level 1, so the Armory shows CANNOT EDIT for it", weapon->name);
    }
}
