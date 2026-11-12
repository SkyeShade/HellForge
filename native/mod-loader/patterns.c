/* Patterns of mods' own (SDK 18, add_pattern; docs/modding/patterns.md, research\pattern-api). Included by attachments.c,
 * whose game-thread update runs PatternsTick. A weapon pattern copies a paint (slot 3) and is an Attachment offered on
 * vanilla weapons too. A unit pattern copies a skin of generated_unit_customization_settings (12 collection records at
 * +0x37C8180, count +0x348E3F8; skins of 0x40 bytes at +0x28, their count at +0x30, read on every call) and joins every
 * collection holding its base, in HellForge's copy of its skins. A pick's add_path keys its deltas (the material
 * overrides) and, in the hash_lookup map at +0x3799810, the package loaded before they apply: so each copy's add_path is
 * mapped to its base's package in HellForge's copy of the map. All of it is made again when the game reloads a table. */

#define PAT_COLLECTIONS 0x37C8180u
#define PAT_COLLECTION_COUNT 0x348E3F8u
#define PAT_SKIN 0x40u
#define PAT_SKINS_CAP 24u /* the hangar list's thumbnails: 6 groups of 4, unchecked past them (0x14DC620, 0x19E4EF0) */
#define PAT_SKIN_LOOKUP 0x11F4260u
#define PAT_PACKAGE_LOOKUP 0xFD25A0u
#define PAT_PACKAGES 0x3799810u /* entries*; +8 u32 count (a power of 2); +0x10 u64 the empty key; +0x18 u32 the multiplier */
#define PAT_OVERRIDE 40u        /* {u32 material, u32, u64 material_lut, decal_sheet, pattern_lut, pattern_masks_array} */

static const uint8_t kPatSkinPin[24] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x44, 0x8b, 0x1d, 0x8c, 0xa1, 0x29, 0x02, 0x45, 0x33, 0xc0, 0x8b,
    0xda, 0x45, 0x8b, 0xc8, 0x45, 0x85, 0xdb, 0x74};
static const uint8_t kPatPackagePin[29] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x7c, 0x24, 0x10, 0x44, 0x8b, 0x05, 0x67, 0x72, 0x7c,
    0x02, 0x33, 0xd2, 0x44, 0x8b, 0x15, 0x6e, 0x72, 0x7c, 0x02, 0x4c, 0x8b, 0xc9};

/* The game's 12 weapon paints (slot 3; every weapon with paints offers all of them). */
static const uint32_t kPatPaints[] = {0x80FBE63Du, 0xBD04CA8Cu, 0x94685290u, 0x5D125795u, 0x82C32409u, 0x46A140E2u, 0x680145DEu,
    0x8F739A74u, 0xC5F23640u, 0x96F3BBDEu, 0xB89373EDu, 0x5B8B77CBu};

typedef struct Pattern
{
    HfMod* mod;
    char name[96];
    char shown[192];        /* its name on the card (the ui name: the hangar shows a key it cannot look up as it is) */
    uint32_t id, base, text;
    DeltaCopy copy;         /* its look (PatLook), which each collection's copy takes */
    /* Each collection's copy: a skin's add_path is the collection's own (the same id has other deltas and another
     * package on each model), so the pattern has an add_path for each collection holding its base. */
    DeltaCopy copies[PAT_COLLECTION_SLOTS];
    uint32_t has;           /* the collections whose copy is in the deltas (a bit each) */
    float colours[HF_PATTERN_COLOURS][3];
    uint32_t colourCount;
    int state;              /* 0 waiting, 1 in the game, -1 refused (logged) */
    uint32_t collections;   /* the collections it joined (a bit each) */
} Pattern;
static Pattern g_patterns[PAT_MAX];

/* HellForge's copy of each collection's skins: the game's, then the mods'. */
typedef struct PatCollection
{
    const uint8_t* record;  /* the record it was made for (a reload makes new ones) */
    const uint8_t* vanilla; /* the game's skins, and how many */
    uint32_t vanillaCount;
    uint32_t built;         /* the generation it holds */
    int mine;               /* its record names HellForge's copy */
    uint8_t rows[PAT_SKINS_CAP * PAT_SKIN];
} PatCollection;
static PatCollection g_patCollections[PAT_COLLECTION_SLOTS];
static uint32_t g_patGeneration = 1; /* changes when patterns go in */
static uint8_t* g_patMap;            /* HellForge's package map, and the game's it copies */
static uint8_t* g_patMapFrom;
static uint32_t g_patMapBuilt;

static int PatIsPaint(uint32_t id)
{
    for (size_t i = 0; i < sizeof kPatPaints / sizeof kPatPaints[0]; i++) if (kPatPaints[i] == id) return 1;
    return 0;
}

/* Which texture of a material override sits at a record offset of a component: 1 material_lut, 2 decal_sheet, 3
 * pattern_lut, 4 pattern_masks_array, 0 none. UnitCustomization's record is 27 overrides (12, then 3 lists of 5; 0x438
 * bytes). WeaponCustomization's material override runs from +200: 10 defaults, then 10 slots of {u32 slot, u32, 10}. */
static int PatTextureAt(uint32_t component, uint32_t at)
{
    uint32_t in;
    if (component == ATT_UNIT_CUSTOMIZATION && at < 27u * PAT_OVERRIDE) in = at % PAT_OVERRIDE;
    else if (component == ATT_CUSTOMIZATION && at >= 200 && at < 600) in = (at - 200) % PAT_OVERRIDE;
    else if (component == ATT_CUSTOMIZATION && at >= 600 && at < 4680 && (at - 600) % 408 >= 8) in = ((at - 600) % 408 - 8) % PAT_OVERRIDE;
    else return 0;
    return in % 8 == 0 && in >= 8 ? (int)(in / 8) : 0;
}

/* The base's texture at a role of a copy's override told to the mod, and what takes its place: the pattern's pattern_lut
 * for that role, a swap's, or the same. */
static uint64_t PatTexture(const DeltaCopy* a, int role, uint64_t value, uint32_t override)
{
    static const char* const kRole[] = {"", "material_lut", "decal_sheet", "pattern_lut", "pattern_masks_array"};
    if (a->log != NULL && a->collection < 0)
        ApiLog(a->log, "add_pattern(%s): its base sets %s %016llx (material override at +0x%x)", a->name, kRole[role], (unsigned long long)value, override);
    else if (a->log != NULL)
        ApiLog(a->log, "add_pattern(%s): on collection %d its base sets %s %016llx (material override at +0x%x)", a->name, a->collection, kRole[role],
            (unsigned long long)value, override);
    if (role == 3 && a->patternLut) return a->patternLut;
    for (uint32_t s = 0; s < a->swapCount; s++) if (a->swaps[s][0] == value) return a->swaps[s][1];
    return value;
}

/* A copy's textures in one delta's data (component, record offset at, size bytes): the pattern_lut and the swaps put in,
 * and the base's textures told to the mod (once). attachments.c calls it for every copied delta. */
static void PatternTextures(const DeltaCopy* a, uint32_t component, uint32_t at, uint8_t* data, uint32_t size)
{
    if (component != ATT_UNIT_CUSTOMIZATION && component != ATT_CUSTOMIZATION) return;
    for (uint32_t b = (8 - at % 8) % 8; b + 8 <= size; b += 8)
    {
        int role = PatTextureAt(component, at + b);
        uint64_t value;
        memcpy(&value, data + b, 8);
        if (role == 0 || value == 0) continue;
        value = PatTexture(a, role, value, at + b - (uint32_t)(role * 8));
        memcpy(data + b, &value, 8);
    }
}

/* A collection's record (by its place in the game's list), or NULL. */
static const uint8_t* PatRecord(uintptr_t game, uint32_t k)
{
    return *(uint8_t* const*)(game + PAT_COLLECTIONS + 8u * k);
}

/* The game's own skins of a collection: those its record names, or the first ones of HellForge's copy. */
static const uint8_t* PatVanillaSkins(uintptr_t game, uint32_t k, uint32_t* count)
{
    const uint8_t* record = PatRecord(game, k);
    if (record == NULL) { *count = 0; return NULL; }
    const PatCollection* c = &g_patCollections[k];
    const uint8_t* skins = *(uint8_t* const*)(record + 0x28);
    if (c->record == record && skins == c->rows) { *count = c->vanillaCount; return c->vanilla; }
    *count = *(const uint32_t*)(record + 0x30);
    return skins;
}

/* A game skin by id: its row in the first collection that has it, or NULL. */
static const uint8_t* PatVanillaSkin(uintptr_t game, uint32_t id)
{
    uint32_t collections = *(volatile uint32_t*)(game + PAT_COLLECTION_COUNT);
    for (uint32_t k = 0; k < collections && k < PAT_COLLECTION_SLOTS; k++)
    {
        uint32_t count;
        const uint8_t* skins = PatVanillaSkins(game, k, &count);
        for (uint32_t i = 0; skins != NULL && i < count; i++)
            if (*(const uint32_t*)(skins + (size_t)i * PAT_SKIN + 8) == id) return skins + (size_t)i * PAT_SKIN;
    }
    return NULL;
}

/* Whether the game's skin and package lookups read as reviewed (once; logged when not). */
static int g_patProven;
static int PatProven(uintptr_t game)
{
    if (!g_patProven)
    {
        g_patProven = PinMatches(game + PAT_SKIN_LOOKUP, kPatSkinPin, sizeof kPatSkinPin) && PinMatches(game + PAT_PACKAGE_LOOKUP, kPatPackagePin, sizeof kPatPackagePin) ? 1 : -1;
        if (g_patProven < 0) Note("patterns: the game's skin or package lookup is not the reviewed one; no patterns");
    }
    return g_patProven > 0;
}

/* A unit pattern's add_path on a collection. */
static uint64_t PatKey(const char* name, uint32_t k)
{
    char key[160];
    snprintf(key, sizeof key, "hellforge/pattern/%s/%u", name, k);
    return Murmur64A(key, strlen(key));
}

/* A collection's row of a skin, among the game's own, or NULL. */
static const uint8_t* PatSkinIn(const uint8_t* skins, uint32_t count, uint32_t id)
{
    for (uint32_t i = 0; skins != NULL && i < count; i++)
        if (*(const uint32_t*)(skins + (size_t)i * PAT_SKIN + 8) == id) return skins + (size_t)i * PAT_SKIN;
    return NULL;
}

/* A unit pattern's copy on collection k, when its base is there with deltas: 1 set, 0 not there, -1 its key is taken. */
static int PatCopyOn(uintptr_t game, const uint8_t* s, Pattern* p, uint32_t k)
{
    uint32_t count;
    const uint8_t* skins = PatVanillaSkins(game, k, &count);
    const uint8_t* base = PatSkinIn(skins, count, p->base);
    uint64_t key = base != NULL ? *(const uint64_t*)(base + 0x10) : 0;
    if (key == 0 || AttDeltaSetting(s, key) == NULL) return 0;
    DeltaCopy* copy = &p->copies[k];
    *copy = p->copy;
    copy->baseKey = key;
    copy->addPath = PatKey(p->name, k);
    copy->collection = (int)k;
    copy->name = p->name;
    return AttDeltaSetting(s, copy->addPath) != NULL ? -1 : 1;
}

/* Why a unit pattern cannot go in, or NULL; its copy for each collection holding its base is set on the way (a
 * collection whose base has no add_path or no deltas is left out). */
static const char* PatWhy(uintptr_t game, const uint8_t* s, Pattern* p)
{
    if (!PatProven(game)) return "this game's skins are not the reviewed ones";
    if (PatVanillaSkin(game, p->base) == NULL) return "its base is neither a weapon paint nor a vehicle, hellpod or Pelican skin of the game";
    if (PatVanillaSkin(game, p->id) != NULL) return "its id is a game skin's";
    p->has = 0;
    uint32_t collections = *(volatile uint32_t*)(game + PAT_COLLECTION_COUNT);
    for (uint32_t k = 0; k < collections && k < PAT_COLLECTION_SLOTS; k++)
    {
        int on = PatCopyOn(game, s, p, k);
        if (on < 0) return "its key is taken in the entity deltas";
        if (on) p->has |= 1u << k;
    }
    return p->has == 0 ? "its base is a default skin, or has no entity deltas" : NULL;
}

/* Unit patterns wait until the game has its skins: 1 when one waits and they are there. */
static int PatWaiting(uintptr_t game)
{
    if (*(volatile uint32_t*)(game + PAT_COLLECTION_COUNT) == 0) return 0;
    for (int i = 0; i < g_patternCount; i++) if (g_patterns[i].state == 0) return 1;
    return 0;
}

/* Every unit pattern in the game is checked again (the deltas are made again). */
static void PatReset(void)
{
    for (int i = 0; i < g_patternCount; i++) if (g_patterns[i].state == 1) g_patterns[i].state = 0;
}

/* The waiting unit patterns that can go in, after the n ready already; the others refused (logged), or left waiting
 * while the game has no skins. Returns how many in all. */
static int PatResolve(uintptr_t game, const uint8_t* s, DeltaCopy** ready, int** states, int n)
{
    g_patGeneration++; /* the collections and the package map are made again for what goes in now */
    if (*(volatile uint32_t*)(game + PAT_COLLECTION_COUNT) == 0) return n;
    for (int i = 0; i < g_patternCount; i++)
    {
        Pattern* p = &g_patterns[i];
        if (p->state != 0) continue;
        const char* why = PatWhy(game, s, p);
        if (why != NULL) { p->state = -1; ApiLog(p->mod, "add_pattern(%s): %s", p->name, why); continue; }
        for (uint32_t k = 0; k < PAT_COLLECTION_SLOTS; k++)
            if (p->has & (1u << k)) { states[n] = &p->state; ready[n++] = &p->copies[k]; }
        p->copy.log = NULL; /* told with each collection's copy */
    }
    return n;
}

/* A mod skin's row on collection k: its base's, with its own id, add_path, names and swatch. */
static void PatRow(uint8_t* row, const uint8_t* base, const Pattern* p, uint32_t k)
{
    memcpy(row, base, PAT_SKIN);
    const char* debug = p->name;
    memcpy(row, &debug, 8);
    memcpy(row + 8, &p->id, 4);
    memcpy(row + 0x10, &p->copies[k].addPath, 8);
    if (p->text) memcpy(row + 0x18, &p->text, 4);
    if (p->shown[0]) { const char* shown = p->shown; memcpy(row + 0x20, &shown, 8); }
    if (p->colourCount)
    {
        const void* colours = p->colours;
        uint64_t count = p->colourCount;
        memcpy(row + 0x30, &colours, 8);
        memcpy(row + 0x38, &count, 8);
    }
}

/* The mods' skins after the game's count in a collection's copy (its rows hold the game's once one joins): each unit
 * pattern in the game whose base the collection holds, while the list has room. Returns the count in all. */
static uint32_t PatJoin(PatCollection* c, uint32_t k)
{
    uint32_t n = c->vanillaCount;
    for (int i = 0; i < g_patternCount; i++)
    {
        Pattern* p = &g_patterns[i];
        const uint8_t* base = p->state == 1 && (p->has & (1u << k)) ? PatSkinIn(c->vanilla, c->vanillaCount, p->base) : NULL;
        if (base == NULL) continue;
        if (n == PAT_SKINS_CAP)
        {
            if (!(p->collections & (1u << k))) ApiLog(p->mod, "add_pattern(%s): collection %u has %u skins already, the most its list shows", p->name, k, PAT_SKINS_CAP);
            continue;
        }
        if (n == c->vanillaCount) memcpy(c->rows, c->vanilla, (size_t)c->vanillaCount * PAT_SKIN);
        PatRow(c->rows + (size_t)n++ * PAT_SKIN, base, p, k);
        p->collections |= 1u << k;
    }
    return n;
}

/* A record's skins and count set, the count after the skins when it grows and before them when it shrinks, so a
 * reader's count never passes its skins. */
static void PatSetSkins(uint8_t* record, const uint8_t* skins, uint32_t count)
{
    if (count < *(const uint32_t*)(record + 0x30)) { *(volatile uint32_t*)(record + 0x30) = count; MemoryBarrier(); }
    memcpy(record + 0x28, &skins, 8);
    MemoryBarrier();
    *(volatile uint32_t*)(record + 0x30) = count;
}

/* One collection's skins made again when its record is new or patterns went in: the game's, then the mods' (PatJoin).
 * Its record then names HellForge's copy, or the game's skins again when no pattern joins. */
static void PatCollectionTick(uintptr_t game, uint32_t k)
{
    PatCollection* c = &g_patCollections[k];
    uint8_t* record = (uint8_t*)PatRecord(game, k);
    if (record == NULL) return;
    const uint8_t* skins = *(uint8_t* const*)(record + 0x28);
    if (c->record == record && c->built == g_patGeneration && skins == (c->mine ? c->rows : c->vanilla)) return;
    uint32_t count;
    const uint8_t* vanilla = PatVanillaSkins(game, k, &count);
    if (c->record == record && skins == c->rows && c->mine) PatSetSkins(record, vanilla, count); /* the game's again while the copy is made */
    c->record = record;
    c->vanilla = vanilla;
    c->vanillaCount = count;
    c->built = g_patGeneration;
    c->mine = 0;
    uint32_t n = vanilla == NULL || count == 0 || count >= PAT_SKINS_CAP ? count : PatJoin(c, k);
    if (n == count) return;
    PatSetSkins(record, c->rows, n);
    c->mine = 1;
    Note("patterns: collection %u shows %u skin(s), %u of mods", k, n, n - count);
}

/* A package map entry in a map: open addressing from multiplier * key, as 0xFD25A0 searches. 0 when full. */
static int PatMapPut(uint8_t* entries, uint32_t count, uint32_t multiplier, uint64_t empty, uint64_t key, uint64_t package)
{
    for (uint32_t i = 0; i < count; i++)
    {
        uint8_t* entry = entries + 16u * ((i + (uint64_t)multiplier * key) & (count - 1));
        if (*(const uint64_t*)entry != empty) continue;
        memcpy(entry, &key, 8);
        memcpy(entry + 8, &package, 8);
        return 1;
    }
    return 0;
}

/* A key's package in the game's map, or 0 (0xFD25A0). */
static uint64_t PatMapGet(const uint8_t* entries, uint32_t count, uint32_t multiplier, uint64_t empty, uint64_t key)
{
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* entry = entries + 16u * ((i + (uint64_t)multiplier * key) & (count - 1));
        uint64_t k = *(const uint64_t*)entry;
        if (k == key) return *(const uint64_t*)(entry + 8);
        if (k == empty) return 0;
    }
    return 0;
}

/* The game's package map: its entries, their count (a power of 2), the empty key and the probe's multiplier. */
typedef struct PatMap { uint8_t* entries; uint32_t count, multiplier, used; uint64_t empty; } PatMap;

/* The i-th copy a pattern in the game has (weapon patterns', then each unit pattern's on each collection), or NULL. */
static const DeltaCopy* PatCopyAt(int i)
{
    if (i < g_attachmentCount) return g_attachments[i].pattern && g_attachments[i].state == 1 ? &g_attachments[i].copy : NULL;
    const Pattern* p = &g_patterns[(i - g_attachmentCount) / (int)PAT_COLLECTION_SLOTS];
    uint32_t k = (uint32_t)(i - g_attachmentCount) % PAT_COLLECTION_SLOTS;
    return p->state == 1 && (p->has & (1u << k)) ? &p->copies[k] : NULL;
}

/* A copy's add_path put in the map with its base's package (in the game's map, from); 1 put. */
static int PatMapCopy(PatMap* map, const uint8_t* from, const DeltaCopy* copy)
{
    uint64_t package = PatMapGet(from, map->count, map->multiplier, map->empty, copy->baseKey);
    if (package == 0 || PatMapGet(map->entries, map->count, map->multiplier, map->empty, copy->addPath) != 0) return 0;
    if (4u * (map->used + 1) > 3u * map->count || !PatMapPut(map->entries, map->count, map->multiplier, map->empty, copy->addPath, package))
    { Note("patterns: the package map is full; %s's textures may not load", copy->name); return 0; }
    map->used++;
    return 1;
}

/* Each pattern's add_path mapped to its base's package in a copy of the game's map, made again when the map is new or
 * patterns went in. The copy keeps the game's size, so a reader never sees a count that is not its entries'. */
static void PatPackagesTick(uintptr_t game)
{
    uint8_t* entries = *(uint8_t* const*)(game + PAT_PACKAGES);
    PatMap map = {NULL, *(const uint32_t*)(game + PAT_PACKAGES + 8), *(const uint32_t*)(game + PAT_PACKAGES + 0x18), 0, *(const uint64_t*)(game + PAT_PACKAGES + 0x10)};
    if (entries == NULL || map.count == 0 || (map.count & (map.count - 1)) != 0) return;
    if (entries == g_patMap && g_patMapBuilt == g_patGeneration) return;
    const uint8_t* from = entries == g_patMap ? g_patMapFrom : entries;
    for (uint32_t i = 0; i < map.count; i++) map.used += *(const uint64_t*)(from + 16u * i) != map.empty;
    map.entries = (uint8_t*)VirtualAlloc(NULL, 16u * (size_t)map.count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (map.entries == NULL) return;
    memcpy(map.entries, from, 16u * (size_t)map.count);
    int added = 0;
    for (int i = 0; i < g_attachmentCount + g_patternCount * (int)PAT_COLLECTION_SLOTS; i++)
        if (PatCopyAt(i) != NULL) added += PatMapCopy(&map, from, PatCopyAt(i));
    g_patMapBuilt = g_patGeneration;
    if (added == 0 && entries != g_patMap) { VirtualFree(map.entries, 0, MEM_RELEASE); return; }
    g_patMapFrom = (uint8_t*)from;
    g_patMap = map.entries; /* the old copy stays: a reader may hold it */
    memcpy((void*)(game + PAT_PACKAGES), &map.entries, 8);
    Note("patterns: %d pattern(s) mapped to their base's package", added);
}

/* On the game thread after the deltas: the collections and the package map, once the game has them. */
static void PatternsTick(uintptr_t game)
{
    if (!PatProven(game)) return;
    uint32_t collections = *(volatile uint32_t*)(game + PAT_COLLECTION_COUNT);
    for (uint32_t k = 0; g_patternCount > 0 && k < collections && k < PAT_COLLECTION_SLOTS; k++) PatCollectionTick(game, k);
    PatPackagesTick(game);
}

/* 1 (logged) when a unit pattern has this id (its add_paths come from its name, as its id does). */
static int PatTaken(HfMod* mod, uint32_t id, uint64_t addPath, const char* name)
{
    (void)addPath;
    for (int i = 0; i < g_patternCount; i++)
        if (g_patterns[i].id == id)
        { ApiLog(mod, "add_pattern(%s): its id is %s's already", name, g_patterns[i].name); return 1; }
    return 0;
}

/* The look a pattern asks for: its pattern_lut, swaps and swatch. 1 if refused (logged). */
static int PatLook(HfMod* mod, const HfPattern* def, DeltaCopy* copy, float (*colours)[3], uint32_t* colourCount)
{
    if (def->swap_count > HF_PATTERN_SWAPS || (def->swap_count && def->swaps == NULL))
    { ApiLog(mod, "add_pattern(%s): it swaps up to %d textures", def->name, HF_PATTERN_SWAPS); return 1; }
    if (def->colour_count > HF_PATTERN_COLOURS || (def->colours != NULL && def->colour_count == 0) || (def->colour_count && def->colours == NULL))
    { ApiLog(mod, "add_pattern(%s): its swatch takes 1 to %d colours", def->name, HF_PATTERN_COLOURS); return 1; }
    copy->patternLut = def->pattern_lut;
    for (uint32_t i = 0; i < def->swap_count; i++) { copy->swaps[i][0] = def->swaps[i].from; copy->swaps[i][1] = def->swaps[i].to; }
    copy->swapCount = def->swap_count;
    copy->log = mod;
    for (uint32_t i = 0; i < def->colour_count; i++) memcpy(colours[i], def->colours[i], sizeof colours[i]);
    *colourCount = def->colour_count;
    return 0;
}

/* A weapon pattern: an attachment of the paint slot, on the weapons it names (none: all that offer its base). */
static uint32_t PatAddWeapon(HfMod* mod, const HfPattern* def, const char* key)
{
    if (g_attachmentCount == ATT_MAX) { ApiLog(mod, "add_pattern(%s): mods added %d attachments and weapon patterns already", def->name, ATT_MAX); return 0; }
    if (def->weapon_count > ATT_WEAPONS || (def->weapon_count && def->weapons == NULL))
    { ApiLog(mod, "add_pattern(%s): it names up to %d weapons (none: every weapon offering its base)", def->name, ATT_WEAPONS); return 0; }
    Attachment a = {0};
    a.mod = mod;
    a.base = def->base;
    a.id = Thin(def->name);
    a.pattern = 1;
    snprintf(a.name, sizeof a.name, "%s", def->name);
    a.copy.addPath = Murmur64A(key, strlen(key));
    a.copy.collection = -1;
    for (uint32_t i = 0; i < def->weapon_count; i++) a.weapons[a.weaponCount++] = def->weapons[i];
    if (PatLook(mod, def, &a.copy, a.colours, &a.colourCount) || AttAdd(mod, &a, def->name_text, def->description, key) == 0) return 0;
    ApiLog(mod, "adding weapon pattern %s (item %08x) as a copy of paint %08x on %s", a.name, a.id, a.base, a.weaponCount ? "the weapons it names" : "every weapon offering it");
    return a.id;
}

/* A unit pattern: a skin of every collection holding its base. */
static uint32_t PatAddUnit(HfMod* mod, const HfPattern* def, const char* key)
{
    if (g_patternCount == PAT_MAX) { ApiLog(mod, "add_pattern(%s): mods added %d vehicle, hellpod and Pelican patterns already", def->name, PAT_MAX); return 0; }
    Pattern p = {0};
    p.mod = mod;
    p.base = def->base;
    p.id = Thin(def->name);
    snprintf(p.name, sizeof p.name, "%s", def->name);
    p.copy.collection = -1;
    if (p.id == 0 || PatTaken(mod, p.id, 0, p.name)) return 0;
    for (int i = 0; i < g_attachmentCount; i++)
        if (g_attachments[i].id == p.id) { ApiLog(mod, "add_pattern(%s): its id is %s's already", p.name, g_attachments[i].name); return 0; }
    if (PatLook(mod, def, &p.copy, p.colours, &p.colourCount)) return 0;
    if (def->name_text != NULL && def->name_text[0])
    {
        UpperCopy(def->name_text, p.shown, sizeof p.shown); /* the game's skin names are upper case */
        p.text = AddText(p.shown, key, 0x14);
    }
    Pattern* slot = &g_patterns[g_patternCount];
    *slot = p;
    slot->copy.name = slot->name;
    MemoryBarrier();
    g_patternCount++;
    ApiLog(mod, "adding pattern %s (skin %08x) as a copy of skin %08x", p.name, p.id, p.base);
    WriteRegisteredItems();
    return p.id;
}

static uint32_t ApiAddPattern(HfMod* mod, const HfPattern* def)
{
    if (mod == NULL || def == NULL || def->size < sizeof(HfPattern)) { ApiLog(mod, "add_pattern: no pattern, or its size is not set (HfPattern p = {sizeof p})"); return 0; }
    if (def->name == NULL || def->name[0] == 0 || strlen(def->name) >= sizeof g_patterns[0].name) { ApiLog(mod, "add_pattern: it needs a name (under 96 bytes)"); return 0; }
    if (g_fired[HF_EVENT_ENTITIES_READY]) { ApiLog(mod, "add_pattern(%s): too late; call it from hellforge_mod_init", def->name); return 0; }
    if (def->base == 0) { ApiLog(mod, "add_pattern(%s): it needs a base, a vanilla pattern's id", def->name); return 0; }
    char key[160];
    snprintf(key, sizeof key, "hellforge/pattern/%s", def->name);
    return PatIsPaint(def->base) ? PatAddWeapon(mod, def, key) : PatAddUnit(mod, def, key);
}

/* A weapon pattern for the backend: {"mixId", "copyOf", "kind": "weapon", "weapons"} (none: every weapon offering it). */
static int PatWeaponItem(char* text, int n, int size, const Attachment* a, int first)
{
    n += snprintf(text + n, size - n, "%s{\"mixId\":%u,\"copyOf\":%u,\"kind\":\"weapon\",\"weapons\":[", first ? "" : ",", a->id, a->base);
    for (uint32_t w = 0; w < a->weaponCount; w++) n += snprintf(text + n, size - n, "%s%u", w ? "," : "", a->weapons[w]);
    return n + snprintf(text + n, size - n, "]}");
}

/* The patterns for the backend (RegisteredItems.cs): weapon patterns (PatWeaponItem), then unit patterns {"mixId",
 * "copyOf", "kind": "unit"}, the refused left out. */
static int PatternItems(char* text, int n, int size)
{
    n += snprintf(text + n, size - n, ",\"patterns\":[");
    int listed = 0;
    for (int i = 0; i < g_attachmentCount && n < size - REG_ITEM_ROOM; i++)
        if (g_attachments[i].state >= 0 && g_attachments[i].pattern) n = PatWeaponItem(text, n, size, &g_attachments[i], listed++ == 0);
    for (int i = 0; i < g_patternCount && n < size - REG_ITEM_ROOM; i++)
        if (g_patterns[i].state >= 0)
            n += snprintf(text + n, size - n, "%s{\"mixId\":%u,\"copyOf\":%u,\"kind\":\"unit\"}", listed++ ? "," : "", g_patterns[i].id, g_patterns[i].base);
    return n + snprintf(text + n, size - n, "]");
}
