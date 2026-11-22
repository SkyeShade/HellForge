/* The vanilla secondaries' paint slot (registry_paints.c): a customization record without a mastery package gets the
 * paint slot first in its slots, the Default paint in its first free default pair and its own paint package. A record
 * that has a package, full slots or full defaults is left byte for byte. "paintItems" lists the weapons given one.
 * Included by stats_test.c. */

static void SpRecord(uint8_t* record, int pairs, int slots, uint32_t package)
{
    memset(record, 0, REG_CUSTOMIZATION_STRIDE);
    for (int i = 0; i < pairs; i++)
    {
        uint32_t pair[2] = {10u + (uint32_t)i, 0x100u + (uint32_t)i};
        memcpy(record + 8 * i, pair, 8);
    }
    for (int i = 0; i < slots; i++)
    {
        uint32_t slot = 10u + (uint32_t)i;
        memcpy(record + REG_SLOTS_AT + 4 * i, &slot, 4);
    }
    memcpy(record + REG_MASTERY_PACKAGE_AT, &package, 4);
}

static int SpUnchanged(uint8_t* record, int pairs, int slots, uint32_t package)
{
    static uint8_t before[REG_CUSTOMIZATION_STRIDE];
    SpRecord(before, pairs, slots, package);
    SpRecord(record, pairs, slots, package);
    return GivePaintSlot(record, 0xABCu) == 0 && memcmp(record, before, REG_CUSTOMIZATION_STRIDE) == 0;
}

static void SecondaryPaintsFixture(void)
{
    static uint8_t record[REG_CUSTOMIZATION_STRIDE];
    SpRecord(record, 2, 2, 0);
    assert(GivePaintSlot(record, 0xABCu) == 1);
    uint32_t slots[10], defaults[20], package;
    memcpy(slots, record + REG_SLOTS_AT, sizeof slots);
    memcpy(defaults, record, sizeof defaults);
    memcpy(&package, record + REG_MASTERY_PACKAGE_AT, 4);
    assert(slots[0] == REG_PAINT_SLOT && slots[1] == 10 && slots[2] == 11 && slots[3] == 0); /* the paint slot first */
    assert(defaults[0] == 10 && defaults[2] == 11);                                         /* its defaults kept */
    assert(defaults[4] == REG_PAINT_SLOT && defaults[5] == REG_DEFAULT_PAINT && defaults[6] == 0);
    assert(package == WeaponChild(REG_PAINT_PACKAGE_SEED, 0xABCu));

    assert(SpUnchanged(record, 2, 2, 0x1234u)); /* a weapon with a mastery package of its own */
    assert(SpUnchanged(record, 2, 10, 0));      /* no room in its slots */
    assert(SpUnchanged(record, 10, 2, 0));      /* no room in its defaults */

    static char text[REG_ITEM_ROOM + 256]; /* PaintItems stops REG_ITEM_ROOM short of the end */
    int saved = g_paintCount;
    g_paintCount = 0;
    assert(PaintItems(text, 0, sizeof text) == 0);
    g_painted[0] = 5;
    g_painted[1] = 7;
    g_paintCount = 2;
    int n = PaintItems(text, 0, sizeof text);
    assert(n == (int)strlen(",\"paintItems\":[5,7]") && strcmp(text, ",\"paintItems\":[5,7]") == 0);
    g_paintCount = saved;
}
