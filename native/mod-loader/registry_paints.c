/* Paints on the vanilla secondaries and support weapons (src/HellForge.LocalBackend/RegisteredItems.Paints.cs). The
 * customize screen makes its cards only from a weapon's mastery package (0x1493770), and neither has one. So each gets its own
 * customization record naming its paint package, with the paint slot first in its slots (as on every primary) and the
 * Default paint as its default. Done before the mods' weapons, so their copies inherit it and OwnCustomization renames
 * the package to WeaponChild(seed, copy), the id the backend serves. Included by registry_load.c. */

#define REG_SLOTS_AT 0x50u                /* u32[10]: the slots the player may change, packed, 0 after the last */
#define REG_PAINT_SLOT 3u
#define REG_DEFAULT_PAINT 0x80FBE63Du      /* the Default paint, every primary's default in slot 3 */
#define REG_PAINT_PACKAGE_SEED 0x5EC0DA27u /* RegisteredItems.Paints.cs PaintPackageSeed */
#define REG_MAX_PAINTED 128

static uint32_t g_painted[REG_MAX_PAINTED]; /* the loadout ids of the weapons given a paint package */
static int g_paintCount;

/* The paint slot, the Default paint and the package in a weapon's own record; 0 when it has a package already or no
 * room in its slots or defaults, and it is left as it is. The record starts with 10 default {slot, item} pairs, 0 after
 * the last. */
static int GivePaintSlot(uint8_t* record, uint32_t id)
{
    uint32_t package, slots[10], defaults[20];
    memcpy(&package, record + REG_MASTERY_PACKAGE_AT, 4);
    memcpy(slots, record + REG_SLOTS_AT, sizeof slots);
    memcpy(defaults, record, sizeof defaults);
    int pair = 0;
    while (pair < 10 && (defaults[2 * pair] != 0 || defaults[2 * pair + 1] != 0)) pair++;
    if (package != 0 || slots[9] != 0 || pair == 10) return 0;
    memmove(slots + 1, slots, 9 * sizeof slots[0]);
    slots[0] = REG_PAINT_SLOT;
    defaults[2 * pair] = REG_PAINT_SLOT;
    defaults[2 * pair + 1] = REG_DEFAULT_PAINT;
    package = WeaponChild(REG_PAINT_PACKAGE_SEED, id);
    memcpy(record + REG_SLOTS_AT, slots, sizeof slots);
    memcpy(record, defaults, sizeof defaults);
    memcpy(record + REG_MASTERY_PACKAGE_AT, &package, 4);
    return 1;
}

/* Every secondary and support weapon (loadout types 3 and 2) among the loadout records with a customization row gets
 * its own record and the slot. */
static void PaintWeapons(uint8_t* state, const uint8_t* records, uint8_t* index, uint32_t recordCount)
{
    uint8_t* table = StatePointer(state, REG_TABLES + 8 * REG_CUSTOMIZATION);
    if (table == NULL) return;
    g_paintCount = 0;
    for (uint32_t r = 0; r < recordCount && g_paintCount < REG_MAX_PAINTED; r++)
    {
        uint32_t id, type;
        memcpy(&id, records + (size_t)r * REG_LOADOUT_STRIDE, 4);
        memcpy(&type, records + (size_t)r * REG_LOADOUT_STRIDE + 4, 4);
        uint64_t entity = type == 3 || type == 2 ? LoadoutOwner(index, r) : 0;
        if (entity == 0 || FindRow(table, SlotsOf(REG_CUSTOMIZATION), 16, entity) == NULL) continue;
        uint8_t* record = OwnRecord(state, REG_CUSTOMIZATION, REG_CUSTOMIZATION_STRIDE, entity, entity);
        if (record != NULL && GivePaintSlot(record, id)) g_painted[g_paintCount++] = id;
    }
    Note("registry: %d vanilla secondaries and support weapons given the paint slot%s", g_paintCount,
         g_paintCount == REG_MAX_PAINTED ? " (the most it lists; any after them keep no paints)" : "");
}

/* registered-items.json's "paintItems": the weapons given a paint package, for the backend to serve it. */
static int PaintItems(char* text, int n, int size)
{
    if (g_paintCount == 0) return n;
    n += snprintf(text + n, size - n, ",\"paintItems\":[");
    for (int i = 0; i < g_paintCount && n < size - REG_ITEM_ROOM; i++) n += snprintf(text + n, size - n, "%s%u", i ? "," : "", g_painted[i]);
    return n + snprintf(text + n, size - n, "]");
}
