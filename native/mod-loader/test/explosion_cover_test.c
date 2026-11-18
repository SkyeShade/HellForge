/* Explosion knockback behind cover (explosion_cover.c) over synthetic queues laid out as the game's: a knockback queued
 * with a damage event that waits for a line of sight is held, goes into the knockback queue when the game's check lets
 * the entry through and is dropped when cover stops it; hits the game does not check keep their knockback. Included by
 * stats_test.c. */

static uint8_t g_coverAnswer;
static uint32_t g_coverChecks;
static uint8_t* g_coverChecked;
static uint8_t CoverFixtureCheck(uint64_t unused, uint8_t* entry, uint32_t ray)
{
    (void)unused;
    g_coverChecks++;
    g_coverChecked = entry;
    return ray != 0 && g_coverAnswer; /* the game's check answers 0 for ray 0 */
}

/* What one ProduceDamage call leaves: with `los`, the damage event of target `target` queued for a line of sight (its
 * flag 1, the kick's ray in +0xA8 as for damage events), and a knockback for `knockTarget`. The counts before it in *los
 * and *knock. */
static void CoverFixtureProduce(uint8_t* explosions, uint8_t* combat, int los, uint32_t target, uint32_t knockTarget, uint32_t* losBefore, uint32_t* knockBefore)
{
    uint32_t n = CoverU32(explosions + COVER_LOS_COUNT), k = CoverU32(combat + COVER_KNOCK_COUNT);
    *losBefore = n;
    *knockBefore = k;
    if (los)
    {
        uint8_t* e = explosions + COVER_LOS_ENTRIES + (uintptr_t)n * COVER_LOS_STRIDE;
        memset(e, 0, COVER_LOS_STRIDE);
        memcpy(e, &target, 4);
        float from[3] = {1.0f, 2.0f, (float)n}, to[3] = {4.0f, 5.0f, 6.0f};
        memcpy(e + 0x74, from, 12); memcpy(e + 0x80, to, 12);
        e[COVER_LOS_DAMAGE] = 1;
        n++;
        memcpy(explosions + COVER_LOS_COUNT, &n, 4);
    }
    uint8_t* o = combat + COVER_KNOCK_ENTRIES + (uintptr_t)k * COVER_KNOCK_STRIDE;
    memset(o, 0xA5, COVER_KNOCK_STRIDE);
    memcpy(o + COVER_KNOCK_TARGET, &knockTarget, 4);
    k++;
    memcpy(combat + COVER_KNOCK_COUNT, &k, 4);
}

/* The next frame: the kick gives every queued entry a ray in +0xA8 and the gather decides them all. */
static void CoverFixtureKick(uint8_t* explosions)
{
    uint32_t n = CoverU32(explosions + COVER_LOS_COUNT), ray = 7;
    for (uint32_t i = 0; i < n; i++) memcpy(explosions + COVER_LOS_ENTRIES + (uintptr_t)i * COVER_LOS_STRIDE + COVER_LOS_RAY_B, &ray, 4);
    memcpy(explosions + COVER_LOS_KICKED, &n, 4);
}

/* What the gather does after its own check: the decided entries leave the queue. */
static void CoverFixtureGathered(uint8_t* explosions)
{
    uint32_t zero = 0;
    memcpy(explosions + COVER_LOS_COUNT, &zero, 4);
    memcpy(explosions + COVER_LOS_KICKED, &zero, 4);
}

static void ExplosionCoverFixture(void)
{
    uint8_t* explosions = calloc(1, COVER_LOS_ENTRIES + COVER_LOS_MAX * COVER_LOS_STRIDE);
    uint8_t* combat = calloc(1, COVER_KNOCK_COUNT + 0x10);
    uint32_t los, knock;
    memset(g_coverHeld, 0, sizeof g_coverHeld);

    /* behind cover: held, then stopped with its damage */
    CoverFixtureProduce(explosions, combat, 1, 42, 42, &los, &knock);
    assert(CoverHold(explosions, combat, los, knock, 42) == 1 && CoverU32(combat + COVER_KNOCK_COUNT) == 0);
    CoverFixtureKick(explosions);
    g_coverAnswer = 0; g_coverChecks = 0;
    CoverDecide(explosions, explosions, combat, CoverFixtureCheck);
    assert(g_coverChecks == 2 && g_coverChecked == explosions + COVER_LOS_ENTRIES); /* both rays, as the gather asks */
    assert(CoverU32(combat + COVER_KNOCK_COUNT) == 0 && !g_coverHeld[0].used);
    CoverFixtureGathered(explosions);

    /* in the open: held, then let through byte for byte */
    CoverFixtureProduce(explosions, combat, 1, 42, 42, &los, &knock);
    uint8_t expected[COVER_KNOCK_STRIDE];
    memcpy(expected, combat + COVER_KNOCK_ENTRIES, COVER_KNOCK_STRIDE);
    assert(CoverHold(explosions, combat, los, knock, 42) == 1);
    CoverFixtureKick(explosions);
    g_coverAnswer = 1;
    CoverDecide(explosions, explosions, combat, CoverFixtureCheck);
    assert(CoverU32(combat + COVER_KNOCK_COUNT) == 1 && !memcmp(combat + COVER_KNOCK_ENTRIES, expected, COVER_KNOCK_STRIDE));
    CoverFixtureGathered(explosions);
    memset(combat + COVER_KNOCK_COUNT, 0, 4);

    /* two hits of one blast, one behind cover: only the other's knockback comes; an entry found after the queue moved */
    CoverFixtureProduce(explosions, combat, 1, 42, 42, &los, &knock);
    assert(CoverHold(explosions, combat, los, knock, 42));
    CoverFixtureProduce(explosions, combat, 1, 43, 43, &los, &knock);
    assert(CoverHold(explosions, combat, los, knock, 43));
    uint32_t one = 1;
    memcpy(explosions + COVER_LOS_KICKED, &one, 4); /* this frame decides only the first */
    memcpy(explosions + COVER_LOS_ENTRIES + COVER_LOS_RAY_B, &one, 4);
    g_coverAnswer = 0;
    CoverDecide(explosions, explosions, combat, CoverFixtureCheck);
    assert(CoverU32(combat + COVER_KNOCK_COUNT) == 0);
    memmove(explosions + COVER_LOS_ENTRIES, explosions + COVER_LOS_ENTRIES + COVER_LOS_STRIDE, COVER_LOS_STRIDE); /* the gather's move */
    memcpy(explosions + COVER_LOS_COUNT, &one, 4);
    CoverFixtureKick(explosions);
    g_coverAnswer = 1;
    CoverDecide(explosions, explosions, combat, CoverFixtureCheck);
    assert(CoverU32(combat + COVER_KNOCK_COUNT) == 1 && CoverU32(combat + COVER_KNOCK_ENTRIES + COVER_KNOCK_TARGET) == 43);
    CoverFixtureGathered(explosions);
    memset(combat + COVER_KNOCK_COUNT, 0, 4);

    /* the game's ways: no line of sight asked, another target, a second knockback, a full queue */
    CoverFixtureProduce(explosions, combat, 0, 42, 42, &los, &knock);
    assert(CoverHold(explosions, combat, los, knock, 42) == 0 && CoverU32(combat + COVER_KNOCK_COUNT) == 1);
    CoverFixtureProduce(explosions, combat, 1, 42, 44, &los, &knock);
    assert(CoverHold(explosions, combat, los, knock, 42) == 0 && CoverU32(combat + COVER_KNOCK_COUNT) == 2);
    CoverFixtureProduce(explosions, combat, 1, 42, 42, &los, &knock);
    uint32_t extra = CoverU32(combat + COVER_KNOCK_COUNT) + 1;
    memcpy(combat + COVER_KNOCK_COUNT, &extra, 4);
    assert(CoverHold(explosions, combat, los, knock, 42) == 0);
    uint32_t full = COVER_LOS_MAX;
    memcpy(explosions + COVER_LOS_COUNT, &full, 4);
    assert(CoverHold(explosions, combat, COVER_LOS_MAX, 0, 42) == 0);
    CoverFixtureGathered(explosions);
    memset(combat + COVER_KNOCK_COUNT, 0, 4);

    /* a held one whose entry never comes goes stale; one held for another queue is let through */
    CoverFixtureProduce(explosions, combat, 1, 42, 42, &los, &knock);
    assert(CoverHold(explosions, combat, los, knock, 42));
    CoverFixtureGathered(explosions);
    for (uint32_t i = 0; i < COVER_STALE; i++) CoverDecide(explosions, explosions, combat, CoverFixtureCheck);
    assert(!g_coverHeld[0].used && CoverU32(combat + COVER_KNOCK_COUNT) == 0);
    CoverFixtureProduce(explosions, combat, 1, 42, 42, &los, &knock);
    assert(CoverHold(explosions, combat, los, knock, 42));
    CoverDecide(combat, explosions, combat, CoverFixtureCheck);
    assert(CoverU32(combat + COVER_KNOCK_COUNT) == 1 && !g_coverHeld[0].used);

    /* the pins: every one names bytes, the two checks of the gather the ones the decision repeats */
    for (size_t i = 0; i < sizeof kCoverPins / sizeof kCoverPins[0]; i++) assert(kCoverPins[i].length > 0 && kCoverPins[i].length <= 24);
    free(explosions);
    free(combat);
}
