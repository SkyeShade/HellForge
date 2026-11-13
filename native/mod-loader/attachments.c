/* Weapon attachments of mods' own (SDK 17, add_attachment; docs/modding/content.md "Attachments of your own"). Included
 * by packages.c, whose game-thread update runs AttachmentsTick. game.dll (25480438; KB attachment-registration-static)
 * keeps items in g_settings (+0x37C5B50): up to 32 pointers to {items*, u32 count} blocks of 88-byte items, the count at
 * +0x348DA98 read on every call. A weapon's chosen item ids become keys (each item's add_path, +0x20) into the entity
 * deltas, [state + 0xF12E98], reloaded at every use. So an attachment is a copy of its base's item in HellForge's block,
 * appended to g_settings, and of its base's deltas under its own add_path in a grown copy of the delta storage, swapped
 * in; a unit of the mod's own replaces the base's where the deltas name it. Both are made again when the game reloads. */

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
#define ATT_UNIT_CUSTOMIZATION 206u
#define PAT_MAX 128               /* unit patterns (patterns.c) */
#define PAT_COLLECTION_SLOTS 12u  /* the game's unit skin collections */

static const uint8_t kAttItemPin[24] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x7c, 0x24, 0x10, 0x8b, 0x3d, 0x98, 0x93, 0x29, 0x02,
    0x45, 0x33, 0xd2, 0x85, 0xff, 0x74, 0x3b, 0x4c};
static const uint8_t kAttDeltaPin[30] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x8b, 0x05, 0xcc, 0x6a, 0xf5, 0x02, 0x33, 0xd2, 0x45, 0x33,
    0xc0, 0x4c, 0x8b, 0xd9, 0x48, 0x8b, 0x98, 0x98, 0x2e, 0xf1, 0x00, 0x48, 0x8b, 0xc1};

/* One copy of a base's deltas under a new key: an attachment's, or a pattern's (patterns.c). The base's unit becomes
 * `unit` where the Customization delta names it; a pattern's textures take the place of the base's (PatternTexture). */
typedef struct DeltaCopy
{
    uint64_t baseKey, addPath, unit;
    uint64_t patternLut;                 /* every pattern_lut the base's material overrides set (0: the base's) */
    uint64_t swaps[HF_PATTERN_SWAPS][2]; /* {from, to} texture hashes */
    uint32_t swapCount;
    HfMod* log;                          /* the mod told the base's textures, once (a pattern's) */
    const char* name;
    int collection;                      /* a unit pattern's collection, -1 for a weapon's (patterns.c) */
} DeltaCopy;

typedef struct Attachment
{
    HfMod* mod;
    char name[96];
    uint32_t id, base, slot;
    uint32_t text[3];
    uint32_t weapons[ATT_WEAPONS];
    uint32_t weaponCount;
    int state;              /* 0 waiting, 1 in the game, -1 refused (logged) */
    int pattern;            /* a weapon pattern (add_pattern, patterns.c): a paint, offered on vanilla weapons too */
    DeltaCopy copy;
    float colours[HF_PATTERN_COLOURS][3]; /* a pattern's own swatch (colourCount of them), else the base's */
    uint32_t colourCount;
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

static uint32_t AttSlot(const uint8_t* item)
{
    const uint32_t* slots = *(uint32_t* const*)(item + 0x30);
    return slots != NULL && *(const uint32_t*)(item + 0x38) > 0 ? slots[0] : 0;
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

static void PatternTextures(const DeltaCopy* a, uint32_t component, uint32_t at, uint8_t* data, uint32_t size); /* patterns.c */

/* One copy's deltas under its add_path: its settings entry, component settings, deltas and data appended. */
static void AttCopyDeltas(const uint8_t* s, const DeltaCopy* a, uint32_t* settings, uint32_t* comps, uint32_t* deltas, uint8_t* data, AttSizes* z)
{
    const uint32_t* from = AttDeltaSetting(s, a->baseKey);
    const uint32_t* oldComps = *(uint32_t* const*)(s + 0x20);
    const uint32_t* oldDeltas = *(uint32_t* const*)(s + 0x30);
    const uint8_t* oldData = *(uint8_t* const*)(s + 0x40);
    settings[2 * z->settings] = from[0];
    settings[2 * z->settings + 1] = z->comps;
    for (uint32_t c = 0; c < from[0]; c++)
    {
        const uint32_t* oc = oldComps + 3u * (from[1] + c);
        uint32_t* nc = comps + 3u * z->comps++;
        nc[0] = oc[0]; nc[1] = z->deltas; nc[2] = oc[2];
        for (uint32_t d = 0; d < oc[2]; d++)
        {
            const uint32_t* od = oldDeltas + 3u * (oc[1] + d);
            uint32_t* nd = deltas + 3u * z->deltas++;
            nd[0] = od[0]; nd[1] = od[1]; nd[2] = z->data;
            memcpy(data + z->data, oldData + od[2], od[1]);
            for (uint32_t b = 0; a->unit && oc[0] == ATT_CUSTOMIZATION && b + 8 <= od[1]; b++) /* the base's unit, where the record holds it */
                if ((od[0] + b) % 8 == 0 && *(const uint64_t*)(data + z->data + b) == a->baseKey) memcpy(data + z->data + b, &a->unit, 8);
            PatternTextures(a, oc[0], od[0], data + z->data, od[1]);
            z->data += od[1];
        }
    }
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

/* What one copy adds: component settings, deltas and data bytes. */
static AttSizes AttExtra(const uint8_t* s, const DeltaCopy* a)
{
    AttSizes one = {0};
    const uint32_t* from = AttDeltaSetting(s, a->baseKey);
    const uint32_t* comps = *(uint32_t* const*)(s + 0x20);
    const uint32_t* deltas = *(uint32_t* const*)(s + 0x30);
    for (uint32_t c = 0; c < from[0]; c++)
    {
        const uint32_t* comp = comps + 3u * (from[1] + c);
        one.comps++;
        for (uint32_t d = 0; d < comp[2]; d++) { one.deltas++; one.data += deltas[3u * (comp[1] + d) + 1]; }
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

/* A grown copy of the delta storage with these copies' deltas (one VirtualAlloc block), or NULL. */
static uint8_t* AttGrow(const uint8_t* s, DeltaCopy* const* list, int n)
{
    AttSizes old = AttMeasure(s), add = {0}, z;
    for (int i = 0; i < n; i++) { AttSizes one = AttExtra(s, list[i]); add.comps += one.comps; add.deltas += one.deltas; add.data += one.data; }
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
        AttCopyDeltas(s, list[i], pointers[1], pointers[2], pointers[3], pointers[4], &z);
    }
    AttHeader(g, s, pointers, slots, &old, &z);
    return g;
}

/* The attachments' items in HellForge's block: each its base's 88 bytes (debug name, slot and colour lists stay the
 * game's, unless a pattern has a swatch of its own) with its own id, text and add_path. Returns how many. */
static uint32_t AttItems(uintptr_t game)
{
    uint32_t n = 0;
    for (int i = 0; i < g_attachmentCount; i++)
    {
        Attachment* a = &g_attachments[i];
        const uint8_t* base = a->state == 1 ? AttVanillaItem(game, a->base) : NULL;
        if (base == NULL) continue;
        uint8_t* row = g_attItems + (size_t)n++ * ATT_ITEM;
        memcpy(row, base, ATT_ITEM);
        memcpy(row + 8, &a->id, 4);
        if (a->text[0]) { memcpy(row + 0x0C, &a->text[1], 4); memcpy(row + 0x10, &a->text[0], 4); }
        if (a->text[2]) memcpy(row + 0x14, &a->text[2], 4);
        memcpy(row + 0x20, &a->copy.addPath, 8);
        if (a->colourCount)
        {
            const void* colours = a->colours;
            uint64_t count = a->colourCount;
            memcpy(row + 0x40, &colours, 8);
            memcpy(row + 0x48, &count, 8);
        }
    }
    return n;
}

/* Why an attachment cannot go in, or NULL; its base key and slot are set on the way. */
static const char* AttWhy(uintptr_t game, const uint8_t* s, Attachment* a)
{
    const uint8_t* base = AttVanillaItem(game, a->base);
    if (base == NULL) return "its base is not a customization item of the game";
    if (*(const uint64_t*)(base + 0x20) == 0) return "its base changes nothing (no add_path)";
    if (AttVanillaItem(game, a->id) != NULL) return "its id is a game item's";
    a->copy.baseKey = *(const uint64_t*)(base + 0x20);
    a->slot = AttSlot(base);
    if (a->pattern && a->slot != 3) return "its base is not a paint (the pattern slot)";
    if (a->copy.unit && a->slot != 2 && a->slot != 4 && a->slot != 5) return "a unit is for an optic, muzzle or magazine";
    if (AttDeltaSetting(s, a->copy.addPath) != NULL) return "its key is taken in the entity deltas";
    if (AttDeltaSetting(s, a->copy.baseKey) == NULL) return "its base has no entity deltas";
    return NULL;
}

/* The waiting attachments that can go in, after the n ready already; the others refused (logged). Returns how many in
 * all, each with its state in states. */
static int AttResolve(uintptr_t game, const uint8_t* s, DeltaCopy** ready, int** states, int n)
{
    for (int i = 0; i < g_attachmentCount; i++)
    {
        Attachment* a = &g_attachments[i];
        if (a->state != 0) continue;
        const char* why = AttWhy(game, s, a);
        if (why != NULL) { a->state = -1; ApiLog(a->mod, "%s(%s): %s", a->pattern ? "add_pattern" : "add_attachment", a->name, why); continue; }
        states[n] = &a->state;
        ready[n++] = &a->copy;
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
        if (proven < 0) Note("attachments: the game's item or delta lookup is not the reviewed one; no attachments or patterns");
    }
    return proven > 0;
}

static int PatResolve(uintptr_t game, const uint8_t* s, DeltaCopy** ready, int** states, int n); /* patterns.c */
static int PatWaiting(uintptr_t game);                                                          /* patterns.c */
static void PatReset(void);                                                                      /* patterns.c */

/* The deltas made again from the game's storage: on a storage HellForge has not grown (a new load: every copy checked
 * again), or when a pattern that waited for its table can go in now. The ready ones' deltas are swapped in. */
static void AttDeltasTick(uintptr_t game, uint8_t* state, uint8_t* s)
{
    if (s == g_attGrownFrom) return;
    if (s == g_attGrown)
    {
        if (!PatWaiting(game)) return;
        s = g_attGrownFrom; /* the game's own, which the grown one copies */
    }
    for (int i = 0; i < g_attachmentCount; i++) if (g_attachments[i].state == 1) g_attachments[i].state = 0;
    PatReset();
    static DeltaCopy* ready[ATT_MAX + PAT_MAX * PAT_COLLECTION_SLOTS]; /* the game thread's only */
    static int* states[ATT_MAX + PAT_MAX * PAT_COLLECTION_SLOTS];
    int n = AttResolve(game, s, ready, states, 0);
    n = PatResolve(game, s, ready, states, n);
    uint8_t* grown = n ? AttGrow(s, ready, n) : NULL;
    for (int i = 0; i < n; i++) { *states[i] = grown ? 1 : -1; ready[i]->log = NULL; } /* a pattern's textures are told once */
    WriteRegisteredItems(); /* the refused are not for the backend */
    if (grown == NULL) return;
    g_attGrownFrom = s;
    g_attGrown = grown; /* the old storage stays: a reader may hold it */
    SetStatePointer(state, ATT_DELTAS, grown);
    Note("attachments: %d attachment(s) and pattern(s) added to the entity deltas", n);
}

/* HellForge's item block appended to g_settings when it is not there (the first time, or after the game's reload). */
static void AttItemsTick(uintptr_t game, uint32_t count)
{
    for (uint32_t k = 0; k < count; k++) if (*(uint8_t* const*)(game + ATT_TABLE + 8u * k) == (uint8_t*)&g_attBlock) return;
    g_attBlock.count = AttItems(game);
    if (g_attBlock.count == 0) return;
    *(uint8_t**)(game + ATT_TABLE + 8u * count) = (uint8_t*)&g_attBlock;
    MemoryBarrier(); /* the block is in its slot before the count shows it */
    *(volatile uint32_t*)(game + ATT_COUNT) = count + 1;
}

static int g_patternCount;                /* patterns.c: unit skins */
static void PatternsTick(uintptr_t game); /* patterns.c */

/* On the game thread, once the game has its items and entity deltas. */
static void AttachmentsTick(void)
{
    uintptr_t game = g_game;
    if ((g_attachmentCount == 0 && g_patternCount == 0) || game == 0 || !AttProven(game)) return;
    uint8_t* state = EntityState();
    uint8_t* s = StatePointer(state, ATT_DELTAS);
    uint32_t count = *(volatile uint32_t*)(game + ATT_COUNT);
    if (s == NULL || count == 0 || count >= ATT_SLOTS) return;
    AttDeltasTick(game, state, s);
    AttItemsTick(game, count);
    PatternsTick(game);
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

static int PatTaken(HfMod* mod, uint32_t id, uint64_t addPath, const char* name); /* patterns.c */

/* 1 (logged) when another attachment or pattern has its id or add_path. */
static int AttTaken(HfMod* mod, const Attachment* a)
{
    for (int i = 0; i < g_attachmentCount; i++)
        if (g_attachments[i].id == a->id || g_attachments[i].copy.addPath == a->copy.addPath)
        { ApiLog(mod, "%s(%s): its id or unit is %s's already", a->pattern ? "add_pattern" : "add_attachment", a->name, g_attachments[i].name); return 1; }
    return PatTaken(mod, a->id, a->copy.addPath, a->name);
}

/* Its own text ids: name, its upper-case copy and description, those the mod gives. */
static void AttText(const char* nameText, const char* description, const char* key, Attachment* a)
{
    if (nameText != NULL && nameText[0])
    {
        char upper[192];
        UpperCopy(nameText, upper, sizeof upper);
        a->text[0] = AddText(nameText, key, 0x11);
        a->text[1] = AddText(upper, key, 0x12);
    }
    if (description != NULL && description[0]) a->text[2] = AddText(description, key, 0x13);
}

/* An attachment (or a weapon pattern) taken in: its id, or 0 refused (logged). */
static uint32_t AttAdd(HfMod* mod, Attachment* a, const char* nameText, const char* description, const char* key)
{
    if (a->id == 0 || AttTaken(mod, a)) return 0;
    AttText(nameText, description, key, a);
    Attachment* slot = &g_attachments[g_attachmentCount];
    *slot = *a;
    slot->copy.name = slot->name;
    MemoryBarrier();
    g_attachmentCount++;
    WriteRegisteredItems();
    return a->id;
}

static uint32_t ApiAddAttachment(HfMod* mod, const HfAttachment* def)
{
    if (mod == NULL || def == NULL || def->size < sizeof(HfAttachment)) { ApiLog(mod, "add_attachment: no attachment, or its size is not set (HfAttachment a = {sizeof a})"); return 0; }
    if (def->name == NULL || def->name[0] == 0 || strlen(def->name) >= sizeof g_attachments[0].name) { ApiLog(mod, "add_attachment: it needs a name (under 96 bytes)"); return 0; }
    if (g_fired[HF_EVENT_ENTITIES_READY]) { ApiLog(mod, "add_attachment(%s): too late; call it from hellforge_mod_init", def->name); return 0; }
    if (g_attachmentCount == ATT_MAX) { ApiLog(mod, "add_attachment(%s): mods added %d attachments and weapon patterns already", def->name, ATT_MAX); return 0; }
    Attachment a = {0};
    a.mod = mod;
    a.base = def->base;
    a.id = Thin(def->name);
    a.copy.unit = def->unit;
    a.copy.collection = -1;
    snprintf(a.name, sizeof a.name, "%s", def->name);
    char key[160];
    snprintf(key, sizeof key, "hellforge/attachment/%s", def->name);
    a.copy.addPath = def->unit ? def->unit : Murmur64A(key, strlen(key));
    if (a.id == 0 || AttWeapons(mod, def, &a) || AttAdd(mod, &a, def->name_text, def->description, key) == 0) return 0;
    ApiLog(mod, "adding attachment %s (item %08x) as a copy of item %08x on %u weapon(s)", a.name, a.id, a.base, a.weaponCount);
    return a.id;
}

/* The attachments for the backend (RegisteredItems.cs): {"mixId", "copyOf", "mod", "weapons"}, the refused left out (weapon
 * patterns are PatternItems'). */
static int AttachmentItems(char* text, int n, int size)
{
    n += snprintf(text + n, size - n, ",\"attachments\":[");
    int listed = 0;
    for (int i = 0; i < g_attachmentCount && n < size - REG_ITEM_ROOM; i++)
    {
        const Attachment* a = &g_attachments[i];
        if (a->state < 0 || a->pattern) continue;
        char mod[6 * 80 + 1];
        n += snprintf(text + n, size - n, "%s{\"mixId\":%u,\"copyOf\":%u,\"mod\":\"%s\",\"weapons\":[", listed++ ? "," : "", a->id, a->base,
            JsonText(a->mod->id, mod, sizeof mod));
        for (uint32_t w = 0; w < a->weaponCount; w++) n += snprintf(text + n, size - n, "%s%u", w ? "," : "", a->weapons[w]);
        n += snprintf(text + n, size - n, "]}");
    }
    return n + snprintf(text + n, size - n, "]");
}

#include "patterns.c" /* mods' patterns: weapon paints (as attachments) and unit skins */
