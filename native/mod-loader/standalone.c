/* Standalone weapons (add_weapon since SDK 17; docs/modding/content.md "Weapons"): copies of no weapon. The entity gets
 * the class's component list (weapon_classes.h) and a record of its own in each, from the class and the declaration
 * only. The class is every vanilla entity with that list and loadout type. A byte they all share is a class value; one
 * that differs must lie in a named field (identity: 0, handling: the most common value, ties the lowest bytes) or the
 * weapon is refused as another build. This runs before stats and field edits, so the class is the game's own data.
 * Its network type is the members' shared entry with arrays of its own, its own short id and the class's
 * initial-state block, which depends on the field list alone (user decision). Included by registry_load.c. */

#define CLASS_MAX_MEMBERS 64
#define CLASS_COMPONENTS ((uint32_t)(sizeof kPrimaryComponents / sizeof kPrimaryComponents[0]))
#define CLASS_FIELDS ((int)(sizeof kPrimaryFields / sizeof kPrimaryFields[0]))
#define CLASS_MAX_STRIDE 8192u

/* One row in each table of the class, for GrowForPending. */
static void ClassRows(uint32_t* rows)
{
    for (uint32_t k = 0; k < CLASS_COMPONENTS; k++) if (kPrimaryComponents[k] < GROW_COMPONENTS) rows[kPrimaryComponents[k]]++;
}

/* The class members' entities and short ids, found once per load of the entity data. */
static uint64_t g_classMembers[CLASS_MAX_MEMBERS];
static uint32_t g_classShortIds[CLASS_MAX_MEMBERS];
static int g_classCount;
static const uint8_t* g_classFrom; /* the loadout index they were found in */

static int ClassModEntity(uint64_t entity)
{
    for (int i = 0; i < g_weaponCount; i++) if (g_weapons[i].entity == entity) return 1;
    return 0;
}

/* Whether a map row's component list is the class's. */
static int ClassList(const uint8_t* row)
{
    uint16_t* components;
    uint64_t count;
    memcpy(&components, row + 8, 8);
    memcpy(&count, row + 16, 8);
    if (components == NULL || count != CLASS_COMPONENTS) return 0;
    uint16_t list[CLASS_COMPONENTS];
    if (SafeRead((uintptr_t)components, list, sizeof list)) return 0;
    return memcmp(list, kPrimaryComponents, sizeof list) == 0;
}

/* The vanilla entities of the class: loadout type CLASS_PRIMARY_LOADOUT_TYPE and the class's component list. */
static int FindClass(const WeaponLoad* L)
{
    if (g_classFrom == L->index && g_classCount > 0) return g_classCount;
    g_classCount = 0;
    for (uint32_t r = 0; r < L->recordCount && g_classCount < CLASS_MAX_MEMBERS; r++)
    {
        uint32_t type;
        memcpy(&type, L->records + (size_t)r * REG_LOADOUT_STRIDE + 4, 4);
        uint64_t entity = type == CLASS_PRIMARY_LOADOUT_TYPE ? LoadoutOwner(L->index, r) : 0;
        uint8_t* row = entity != 0 && !ClassModEntity(entity) ? FindRow(L->map, REG_MAP_SLOTS, 32, entity) : NULL;
        if (row == NULL || !ClassList(row)) continue;
        g_classMembers[g_classCount] = entity;
        memcpy(&g_classShortIds[g_classCount++], row + 24, 4);
    }
    g_classFrom = L->index;
    return g_classCount;
}

/* A member's record of component c (the loadout table's: L's copy), or NULL. */
static const uint8_t* MemberRecord(const WeaponLoad* L, uint32_t c, uint32_t stride, uint64_t entity)
{
    if (c == REG_LOADOUT)
    {
        uint8_t* row = FindRow(L->index, SlotsOf(REG_LOADOUT), 16, entity);
        uint32_t record;
        if (row == NULL) return NULL;
        memcpy(&record, row + 8, 4);
        return L->records + (size_t)record * REG_LOADOUT_STRIDE;
    }
    uint32_t slots = SlotsOf(c);
    uint8_t* table = StatePointer(L->state, REG_TABLES + 8 * c);
    uint8_t* row = table != NULL && slots != 0 ? FindRow(table, slots, 16, entity) : NULL;
    uint32_t record;
    if (row == NULL) return NULL;
    memcpy(&record, row + 8, 4);
    return table + (size_t)slots * 16 + (size_t)record * stride;
}

/* The class's most common value of a field (ties: the lowest bytes) into out. */
static void ClassMode(const uint8_t* const* records, int count, uint32_t offset, uint32_t size, uint8_t* out)
{
    int best = -1, bestCount = 0;
    for (int i = 0; i < count; i++)
    {
        int same = 0;
        for (int k = 0; k < count; k++) same += memcmp(records[i] + offset, records[k] + offset, size) == 0;
        if (same > bestCount || (same == bestCount && memcmp(records[i] + offset, records[best] + offset, size) < 0)) { best = i; bestCount = same; }
    }
    memcpy(out, records[best] + offset, size);
}

/* Component c's named fields into out (identity 0, handling the class's most common value), marked in named. */
static void ClassFields(const uint8_t* const* records, uint32_t c, uint32_t stride, uint8_t* out, uint8_t* named)
{
    memset(named, 0, stride);
    for (int f = 0; f < CLASS_FIELDS; f++)
    {
        const ClassField* field = &kPrimaryFields[f];
        if (field->component != c || (uint32_t)field->offset + field->size > stride) continue;
        memset(named + field->offset, 1, field->size);
        if (field->kind == CLASS_HANDLING) ClassMode(records, g_classCount, field->offset, field->size, out + field->offset);
        else memset(out + field->offset, 0, field->size);
    }
}

/* The first byte outside the named fields the members do not share, or -1. */
static int ClassUnshared(const uint8_t* const* records, uint32_t stride, const uint8_t* named)
{
    for (uint32_t b = 0; b < stride; b++)
        for (int i = 1; i < g_classCount && !named[b]; i++)
            if (records[i][b] != records[0][b]) return (int)b;
    return -1;
}

/* The class record of component c into out (stride bytes); 0 when a member has none or a byte differs unnamed (logged). */
static int ClassRecord(const WeaponLoad* L, const Weapon* w, uint32_t c, uint32_t stride, uint8_t* out)
{
    const uint8_t* records[CLASS_MAX_MEMBERS];
    for (int i = 0; i < g_classCount; i++)
        if ((records[i] = MemberRecord(L, c, stride, g_classMembers[i])) == NULL)
        {
            ApiLog(w->mod, "%s: class member %016llx has no record of component %u; not registered", w->name, (unsigned long long)g_classMembers[i], c);
            return 0;
        }
    static uint8_t named[CLASS_MAX_STRIDE];
    memcpy(out, records[0], stride);
    ClassFields(records, c, stride, out, named);
    int b = ClassUnshared(records, stride, named);
    if (b >= 0) ApiLog(w->mod, "%s: byte +0x%x of component %u differs between the class's weapons and no class field names it "
        "(another game build?); not registered", w->name, b, c);
    return b < 0;
}

/* What the declaration sets in component c's record. */
static void Declare(const Weapon* w, uint32_t c, uint8_t* r)
{
    switch (c)
    {
    case REG_UNIT: memcpy(r, &w->entity, 8); break; /* UnitPath: the unit is the weapon's name */
    case 28: memcpy(r + 0x08, &w->package, 8); memcpy(r + 0x10, &w->audioPackage, 8); break;           /* Package, AudioResource */
    case REG_TEXT_COMPONENT:
        memcpy(r + 0x00, &w->text[0], 4); memcpy(r + 0x08, &w->text[1], 4);                              /* LocName, LocNameUpper */
        memcpy(r + 0x14, &w->text[2], 4); memcpy(r + 0x24, &w->text[3], 4); memcpy(r + 0x30, &w->icon, 8); /* Description, Prefix, Icon */
        break;
    case 113: memcpy(r + 0x04, &w->reloadAbility, 4); break;                                             /* WeaponReload.Ability */
    case 5: if (w->capacity) memcpy(r + 0x88, &w->capacity, 4); break;                                    /* WeaponMagazine.Capacity */
    case REG_PROJECTILE_WEAPON:
        memcpy(r, &w->projectile, 4);                                                                    /* ProjType */
        if (w->roundsPerMinute > 0) memcpy(r + 0x08, &w->roundsPerMinute, 4);                            /* RoundsPerMinute */
        break;
    case REG_CUSTOMIZATION:
        memcpy(r + 0x80, &w->magazineUnit, 8);                                                           /* MagazinePath */
        memcpy(r + REG_MASTERY_PACKAGE_AT, &w->mastery, 4);
        break;
    case REG_LOADOUT: memcpy(r, &w->id, 4); break;
    }
}

/* Each class component's record size into strides (0: no table) and place into offsets; the bytes in all, or 0 (logged). */
static size_t ClassStrides(const WeaponLoad* L, const Weapon* w, uint32_t* offsets, uint32_t* strides)
{
    size_t total = 0;
    for (uint32_t k = 0; k < CLASS_COMPONENTS; k++)
    {
        uint32_t c = kPrimaryComponents[k];
        int table = c == REG_LOADOUT || StatePointer(L->state, REG_TABLES + 8 * c) != NULL;
        strides[k] = c == REG_LOADOUT ? REG_LOADOUT_STRIDE : table ? ComponentStride(L->state, c) : 0;
        if (table && (strides[k] == 0 || strides[k] > CLASS_MAX_STRIDE || SlotsOf(c) == 0))
        {
            ApiLog(w->mod, "%s: component %u's table is not understood; not registered", w->name, c);
            return 0;
        }
        offsets[k] = (uint32_t)total;
        total += strides[k];
    }
    return total;
}

/* Every record it will have, built before anything is written: components[k]'s at records + offsets[k]. NULL (logged)
 * when one cannot be built. */
static uint8_t* BuildClassRecords(const WeaponLoad* L, const Weapon* w, uint32_t* offsets, uint32_t* strides)
{
    size_t total = ClassStrides(L, w, offsets, strides);
    uint8_t* records = total != 0 ? (uint8_t*)calloc(1, total) : NULL;
    if (records == NULL) { if (total != 0) ApiLog(w->mod, "%s: out of memory; not registered", w->name); return NULL; }
    for (uint32_t k = 0; k < CLASS_COMPONENTS; k++)
    {
        if (strides[k] == 0) continue;
        if (!ClassRecord(L, w, kPrimaryComponents[k], strides[k], records + offsets[k])) { free(records); return NULL; }
        Declare(w, kPrimaryComponents[k], records + offsets[k]);
    }
    return records;
}

/* A free row for it in every table it will have a record in (the game compiles each table's slot count into its lookups,
 * so a full one refuses the weapon before anything is written). */
static int ClassRowsFree(const WeaponLoad* L, const Weapon* w, const uint32_t* strides)
{
    for (uint32_t k = 0; k < CLASS_COMPONENTS; k++)
    {
        uint32_t c = kPrimaryComponents[k];
        uint8_t* table = c == REG_LOADOUT || strides[k] == 0 ? NULL : StatePointer(L->state, REG_TABLES + 8 * c);
        if (table == NULL) continue;
        if (FindRow(table, SlotsOf(c), 16, w->entity) != NULL || FreeRow(table, SlotsOf(c), 16, w->entity) == NULL)
        {
            ApiLog(w->mod, "%s: no free row in component %u's table (%u slots); not registered", w->name, c, SlotsOf(c));
            return 0;
        }
    }
    return 1;
}

/* Why it cannot be registered before anything is built (logged), or 1 when it can. */
static int StandaloneAddable(WeaponLoad* L, const Weapon* w)
{
    for (uint32_t r = 0; r < L->recordCount; r++)
        if (!memcmp(L->records + (size_t)r * REG_LOADOUT_STRIDE, &w->id, 4))
        { ApiLog(w->mod, "%s: loadout id %08x is taken; not registered", w->name, w->id); return 0; }
    if (FindClass(L) == 0) { ApiLog(w->mod, "%s: the entity data has no weapon of its class; not registered", w->name); return 0; }
    if (FindRow(L->map, REG_MAP_SLOTS, 32, w->entity) != NULL)
    { ApiLog(w->mod, "%s: entity %016llx exists already; not registered", w->name, (unsigned long long)w->entity); return 0; }
    if (!L->cardsCounted) ArmoryCards(L);
    if (L->cards[CLASS_PRIMARY_LOADOUT_TYPE] < REG_ARMORY_CARDS) return 1;
    ApiLog(w->mod, "%s: the armory's primary list is full; not registered", w->name);
    return 0;
}

/* Its records in every table but the loadout one: how many it got. */
static int WriteClassRecords(const WeaponLoad* L, const Weapon* w, const uint8_t* records, const uint32_t* offsets, const uint32_t* strides)
{
    int own = 0;
    for (uint32_t k = 0; k < CLASS_COMPONENTS; k++)
    {
        uint32_t c = kPrimaryComponents[k];
        if (c == REG_LOADOUT || strides[k] == 0) continue;
        if (NewOwnRecord(L->state, c, strides[k], w->entity, records + offsets[k]) != NULL) own++;
        else ApiLog(w->mod, "%s: no room for its record of component %u (it is registered without it)", w->name, c);
    }
    return own;
}

/* Its loadout record (the key of its row last, so a live change's row is found whole); the record's index. */
static uint32_t WriteClassLoadout(WeaponLoad* L, const uint8_t* records, const uint32_t* offsets, uint8_t* loadoutRow, uint64_t entity)
{
    uint32_t record = L->recordCount++;
    for (uint32_t k = 0; k < CLASS_COMPONENTS; k++)
        if (kPrimaryComponents[k] == REG_LOADOUT) memcpy(L->records + (size_t)record * REG_LOADOUT_STRIDE, records + offsets[k], REG_LOADOUT_STRIDE);
    L->cards[CLASS_PRIMARY_LOADOUT_TYPE]++;
    memcpy(loadoutRow + 8, &record, 4);
    memset(loadoutRow + 12, 0, 4);
    MemoryBarrier();
    memcpy(loadoutRow, &entity, 8);
    return record;
}

/* Registers a standalone weapon: 1 when it was (w->state 1), else 0 (-1, logged). */
static int RegisterStandalone(WeaponLoad* L, Weapon* w)
{
    w->state = -1;
    uint32_t offsets[CLASS_COMPONENTS], strides[CLASS_COMPONENTS];
    uint8_t* records = StandaloneAddable(L, w) ? BuildClassRecords(L, w, offsets, strides) : NULL;
    if (records == NULL) return 0;
    uint8_t* mapRow = FreeRow(L->map, REG_MAP_SLOTS, 32, w->entity);
    uint8_t* loadoutRow = FreeRow(L->index, SlotsOf(REG_LOADOUT), 16, w->entity);
    if (!RegisterRoom(L, w, mapRow, loadoutRow) || !ClassRowsFree(L, w, strides)) { free(records); return 0; }
    int own = WriteClassRecords(L, w, records, offsets, strides);
    uint32_t record = WriteClassLoadout(L, records, offsets, loadoutRow, w->entity);
    free(records);
    uint32_t shortId = AppendEntity(L, w, NULL, mapRow, (uint16_t*)kPrimaryComponents, CLASS_COMPONENTS);
    w->state = 1;
    ApiLog(w->mod, "%s registered standalone: entity %016llx, loadout record %u (id %08x), short id %u, %d records of its own from "
        "its class (%d weapons) and its declaration", w->name, (unsigned long long)w->entity, record, w->id, shortId, own, g_classCount);
    return 1;
}

/* Its network type at `index` (as AddNetworkType): the class's entry, which every member shares but for its short id and
 * initial-state offset, with field arrays of its own; 0 (logged) when the members' entries differ or are missing. */
static int AddStandaloneNetworkType(const NetworkConfig* n, const NetworkTypeArrays* to, Weapon* w, uint32_t index, uint32_t lastKey)
{
    int found[CLASS_MAX_MEMBERS], count = 0;
    for (int i = 0; i < g_classCount; i++)
        if ((found[count] = NetworkEntryOf(n->entries, n->count, g_classShortIds[i])) >= 0) count++;
    if (count == 0 || count != g_classCount || w->shortId <= lastKey || !NetworkTypeRoom())
    {
        ApiLog(w->mod, "%s: no network type (%d of %d class entries found); joining games with it may fail", w->name, count, g_classCount);
        return 0;
    }
    /* +0 short id and +8 initial-state offset are each member's; +0x20 and +0x38 point at its own arrays */
    static const uint32_t kOwn[][2] = {{0, 4}, {8, 4}, {0x20, 8}, {0x38, 8}};
    const uint8_t* first = n->entries + (size_t)found[0] * 80;
    for (int i = 1; i < count; i++)
    {
        uint8_t a[80], b[80];
        memcpy(a, first, 80);
        memcpy(b, n->entries + (size_t)found[i] * 80, 80);
        for (size_t k = 0; k < sizeof kOwn / sizeof kOwn[0]; k++) { memset(a + kOwn[k][0], 0, kOwn[k][1]); memset(b + kOwn[k][0], 0, kOwn[k][1]); }
        uint32_t rows, keys;
        memcpy(&rows, first + 0x18, 4);
        memcpy(&keys, first + 0x30, 4);
        uint8_t *ra, *rb, *ka, *kb;
        memcpy(&ra, first + 0x20, 8); memcpy(&rb, n->entries + (size_t)found[i] * 80 + 0x20, 8);
        memcpy(&ka, first + 0x38, 8); memcpy(&kb, n->entries + (size_t)found[i] * 80 + 0x38, 8);
        a[0x1C] = b[0x1C] = a[0x34] = b[0x34] = 0; /* capacities */
        if (memcmp(a, b, 80) != 0 || n->values[found[i]] != n->values[found[0]] ||
            (rows && memcmp(ra, rb, (size_t)rows * 4) != 0) || (keys && memcmp(ka, kb, (size_t)keys * 4) != 0))
        {
            ApiLog(w->mod, "%s: the class's network entries differ (type %d, %d); no network type, joining games with it may fail", w->name, found[0], found[i]);
            return 0;
        }
    }
    if (CopyNetworkEntry(to->entries + (size_t)index * 80, first))
        ApiLog(w->mod, "%s: its network type shares the class's field arrays (no memory for its own)", w->name);
    memcpy(to->entries + (size_t)index * 80, &w->shortId, 4);
    memcpy(to->own + (size_t)index * 4, &index, 4);
    to->values[index] = n->values[found[0]];
    g_networkTypes[g_networkTypeCount++] = (NetworkType){index, (uint32_t)found[0], w->entity, 0, w->name, w->mod, 1};
    ApiLog(w->mod, "%s: network entity type %u from its class (%d entries alike)", w->name, index, count);
    return 1;
}

/* The apply-switch case of a standalone weapon's type: the one every class member's type has, or 0 (the exit). */
static uint32_t ClassNetworkCase(const uint32_t* table)
{
    uint32_t c = 0;
    if (g_netEntries == NULL) return 0;
    for (int i = 0; i < g_classCount; i++)
    {
        int found = NetworkEntryOf(g_netEntries, g_networkVanillaTypes, g_classShortIds[i]);
        if (found < 0) return 0;
        if (i == 0) c = table[found];
        else if (table[found] != c) return 0;
    }
    return c;
}
