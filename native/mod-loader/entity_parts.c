/* Entities from parts (SDK 17, HfEntity.parts; docs/modding/enemies.md). Included at the end of registry_load.c. */

/* An entity from parts (HfEntity.parts). The game walks a map row's list front to back and nowhere else (0x581320,
 * 0x581780), from any lasting memory, so the entity gets a list of its own. What a list must hold is the network side:
 * each networked component (one with a create-fields function, REG_NET_FIELDS) adds its fields by key to the entity
 * type's network entry, and the apply switch's case for the type writes through each of its components' instances
 * (netapply.c). A key the entry lacks and a component the case has but the entity lacks both crash. So the entity's
 * networked components must be exactly those of one of the game's types, whose entry and case it takes. */
#define REG_NET_FIELDS 0xF0CB70u /* 0x581509 reads [r12 + 8 * c + 0xF0CB30], r12 the entity state + 0x40 (0xFDC250) */
typedef struct ComponentSet { uint64_t bits[(REG_COMPONENT_SLOTS + 63) / 64]; } ComponentSet;

static ComponentSet NetworkedSet(uint8_t* state, const uint16_t* list, uint64_t count)
{
    ComponentSet set = {{0}};
    for (uint64_t k = 0; k < count; k++)
        if (list[k] < REG_COMPONENT_SLOTS && StatePointer(state, REG_NET_FIELDS + 8 * list[k]) != NULL) set.bits[list[k] / 64] |= 1ull << (list[k] % 64);
    return set;
}

/* Whether two map rows' component lists ({pointer, count} each) hold the same networked components. */
static int SameNetworked(uint8_t* state, const uint64_t a[2], const uint64_t b[2])
{
    if (a[0] == b[0] && a[1] == b[1]) return 1;
    ComponentSet x = NetworkedSet(state, (const uint16_t*)(uintptr_t)a[0], a[1]), y = NetworkedSet(state, (const uint16_t*)(uintptr_t)b[0], b[1]);
    return memcmp(&x, &y, sizeof x) == 0;
}

/* The row of a game entity type with the list's networked components: the base when it is one, else the first in the
 * short-id list. NULL when no type has them. */
static uint8_t* NetworkDonor(const WeaponLoad* L, const uint8_t* baseRow, const uint16_t* list, uint64_t count)
{
    uint64_t own[2] = {(uint64_t)(uintptr_t)list, count}, theirs[2];
    if (baseRow != NULL) { memcpy(theirs, baseRow + 8, 16); if (SameNetworked(L->state, own, theirs)) return (uint8_t*)(uintptr_t)baseRow; }
    for (uint32_t i = 0; i < L->listCount; i++)
    {
        uint32_t mapIndex;
        uint64_t entity;
        memcpy(&mapIndex, L->newList + 32 + (size_t)i * 8 + 4, 4);
        if (mapIndex >= REG_MAP_SLOTS) continue;
        uint8_t* row = L->map + (size_t)mapIndex * 32;
        memcpy(&entity, row, 8);
        memcpy(theirs, row + 8, 16);
        int added = entity == 0;
        for (int k = 0; k < g_weaponCount && !added; k++) added = g_weapons[k].entity == entity;
        if (!added && SameNetworked(L->state, own, theirs)) return row;
    }
    return NULL;
}

/* The components the game's inits read another component's record for with no test (0x8700B3: 4 reads Unit's; 319
 * Unit's, 227 Health's, 80 VehicleMotion's): the pair, or NULL when the list keeps them together. */
static const uint16_t* ListUnpaired(const uint16_t* list, uint32_t count)
{
    static const uint16_t pairs[][2] = {{4, 88}, {319, 88}, {227, 224}, {80, 225}};
    for (size_t p = 0; p < sizeof pairs / sizeof pairs[0]; p++)
    {
        int needs = 0, has = 0;
        for (uint32_t k = 0; k < count; k++) { needs |= list[k] == pairs[p][0]; has |= list[k] == pairs[p][1]; }
        if (needs && !has) return pairs[p];
    }
    return NULL;
}

/* Its list: the base's components and the parts', each once, ascending as the game's own lists are. 0 with no memory. */
static uint32_t ComposeList(Weapon* w, const uint8_t* baseRow)
{
    uint8_t listed[REG_COMPONENT_SLOTS] = {0};
    uint16_t* components = NULL;
    uint64_t baseCount = 0;
    if (baseRow != NULL) { memcpy(&components, baseRow + 8, 8); memcpy(&baseCount, baseRow + 16, 8); }
    for (uint64_t k = 0; k < baseCount; k++) if (components[k] < REG_COMPONENT_SLOTS) listed[components[k]] = 1;
    for (int i = 0; i < w->partCount; i++) listed[w->parts[i].component] = 1;
    uint32_t count = 0;
    if (w->list == NULL) w->list = calloc(REG_COMPONENT_SLOTS, sizeof w->list[0]);
    for (uint32_t c = 0; w->list != NULL && c < REG_COMPONENT_SLOTS; c++) if (listed[c]) w->list[count++] = (uint16_t)c;
    return count;
}

/* The list of an entity from parts and the game type whose network type it takes (*netRow), or 0 (logged). */
static uint32_t Compose(const WeaponLoad* L, Weapon* w, const uint8_t* baseRow, uint8_t** netRow)
{
    uint32_t count = ComposeList(w, baseRow);
    const uint16_t* unpaired = count != 0 ? ListUnpaired(w->list, count) : NULL;
    if (count == 0) ApiLog(w->mod, "%s: no memory for its component list; not registered", w->name);
    else if (unpaired != NULL)
        ApiLog(w->mod, "%s: component %u needs component %u beside it (the game reads its record unchecked); not registered", w->name, unpaired[0], unpaired[1]);
    else if ((*netRow = NetworkDonor(L, baseRow, w->list, count)) == NULL)
        ApiLog(w->mod, "%s: no entity of the game has its networked components and no others, so it has no network type to take; "
            "not registered (docs/modding/enemies.md \"An entity from parts\")", w->name);
    else return count;
    return 0;
}

/* A weapon's or entity's component list and count: its base's, or its own from parts. 0 when refused (logged). */
static uint64_t Components(const WeaponLoad* L, Weapon* w, const uint8_t* baseRow, uint16_t** list, uint8_t** netRow)
{
    uint64_t count = 0;
    if (w->partCount == 0) { memcpy(list, baseRow + 8, 8); memcpy(&count, baseRow + 16, 8); return count; }
    count = Compose(L, w, baseRow, netRow);
    *list = w->list;
    return count;
}
