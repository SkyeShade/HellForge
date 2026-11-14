/* New explosion types (explosions.c) on a synthetic game image: the refusals, one bound not as reviewed moving nothing, the
 * move, the bounds, the rows, and a projectile using one. Included by stats_test.c after live_content_test.c. */

/* The mod content of that kind and id (content()); 0 none. */
static int ContentOfKind(uint32_t kind, uint64_t id, HfContent* out)
{
    for (uint32_t i = 0; i < 4096; i++)
    {
        HfContent c = {0};
        c.size = sizeof c;
        if (!ApiContent(&g_mods[0], i, &c)) return 0;
        if (c.kind == kind && c.id == id) { *out = c; return 1; }
    }
    return 0;
}

/* A mod's row through the stats mods' path: the row's field, nothing past it, nothing for a type not added. */
static void AddedRowStats(const char* kind, uint32_t type, uint32_t stride, uintptr_t row)
{
    StatEdit e = {0};
    snprintf(e.kind, sizeof e.kind, "%s", kind);
    e.record = type;
    e.offset = 8;
    assert(StatsExtraRow(&e) == row + 8);
    e.offset = stride - 4;
    assert(StatsExtraRow(&e) == row + stride - 4);
    e.offset = stride;
    assert(StatsExtraRow(&e) == 0);
    e.offset = 0;
    assert(StatsExtraRow(&e) == 0);
    e.offset = 6;
    assert(StatsExtraRow(&e) == 0);
    e.offset = 8;
    e.record = type + 500;
    assert(StatsExtraRow(&e) == 0);
}

static void ExplosionsFixture(void)
{
    ContentEventsSave();
    InterlockedExchange(&g_fired[HF_EVENT_PROJECTILES_READY], 0);
    size_t size = 0x3800000;
    uint8_t* game = ContentGame(size);
    uintptr_t g = (uintptr_t)game;
    memcpy(game + EXP_CLEAR, kExplosionClearPin, sizeof kExplosionClearPin);
    for (size_t i = 0; i < sizeof kExplosionSites / sizeof kExplosionSites[0]; i++)
        SiteAs(game, kExplosionSites[i].rva, kExplosionSites[i].dispAt, SiteDisplacement(g + EXP_TABLE, g, &kExplosionSites[i]));
    for (size_t i = 0; i < sizeof kExplosionBounds / sizeof kExplosionBounds[0]; i++)
        *(uint32_t*)(game + kExplosionBounds[i].rva + kExplosionBounds[i].immAt) = EXP_VANILLA;
    *(uint32_t*)(game + 0x1389C9A + 2) = EXP_VANILLA; /* the name-hash walk, which stays */

    uint32_t explosions = g_explosionCount, damages = g_damageCount;
    int explosionState = g_explosionState, damageState = g_damageState;
    uint8_t* explosionTable = g_explosionTable;
    g_explosionCount = 0; g_explosionState = 0; g_explosionTable = NULL; g_damageCount = 0; g_damageState = 1;
    HfMod* m = &g_mods[0];

    static uint8_t base[EXP_ROW];
    memset(base, 0, sizeof base);
    *(uint32_t*)base = 7; *(uint32_t*)(base + EXP_DAMAGE_AT) = 9;
    float radii[3] = {1.0f, 2.0f, 3.0f};
    memcpy(base + EXP_INNER_AT, radii, sizeof radii);

    HfExplosion wrong = {sizeof wrong, .base = 0, .name = "x"};
    assert(ApiAddExplosion(m, &wrong) == 0);
    wrong.base = EXP_VANILLA;
    assert(ApiAddExplosion(m, &wrong) == 0);
    wrong.base = 7; wrong.damage = DMG_VANILLA;
    assert(ApiAddExplosion(m, &wrong) == 0);
    wrong.damage = 0; wrong.outer_radius = -1.0f;
    assert(ApiAddExplosion(m, &wrong) == 0);
    wrong.size = 8;
    assert(ApiAddExplosion(m, &wrong) == 0 && g_explosionCount == 0);

    HfExplosion big = {sizeof big, .base = 7, .name = "big", .damage = 11, .outer_radius = 12.0f};
    HfExplosion plain = {sizeof plain, .base = 7, .name = "plain"};
    assert(ApiAddExplosion(m, &big) == EXP_VANILLA && ApiAddExplosion(m, &plain) == EXP_VANILLA + 1);
    assert(ApiAddExplosion(m, &big) == 0); /* the name is taken */
    assert(ExplosionBaseOf(EXP_VANILLA) == 7 && ExplosionBaseOf(158) == 158);
    assert(!ExplosionUsable(EXP_VANILLA) && ExplosionUsable(158) && ApiExplosionRow(EXP_VANILLA) == 0);

    /* not loaded yet, then loaded */
    assert(InstallExplosions(g, 1) == 0 && g_explosionState == 0);
    uintptr_t row = (uintptr_t)base, sentinel = 0x1234;
    memcpy(game + EXP_TABLE + 7 * 8, &row, 8);
    memcpy(game + EXP_TABLE + 8, &sentinel, 8);

    /* one bound not as reviewed: waits out the grace period, then nothing moves and the types are refused */
    uint32_t* bound = (uint32_t*)(game + kExplosionBounds[20].rva + kExplosionBounds[20].immAt);
    *bound = 424;
    int32_t site = *(int32_t*)(game + kExplosionSites[0].rva + kExplosionSites[0].dispAt);
    assert(InstallExplosions(g, 0) == 0 && g_explosionState == 0);
    assert(InstallExplosions(g, 1) == 1 && g_explosionState == -1 && g_explosionTable == NULL);
    assert(*(int32_t*)(game + kExplosionSites[0].rva + kExplosionSites[0].dispAt) == site);
    assert(!ExplosionUsable(EXP_VANILLA) && *(uint32_t*)(game + kExplosionBounds[0].rva + kExplosionBounds[0].immAt) == EXP_VANILLA);
    *bound = EXP_VANILLA;

    g_explosionState = 0;
    assert(InstallExplosions(g, 0) == 1 && g_explosionState == 1 && g_explosionTable != NULL);
    uintptr_t table = (uintptr_t)g_explosionTable;
    for (size_t i = 0; i < sizeof kExplosionSites / sizeof kExplosionSites[0]; i++)
        assert(*(int32_t*)(game + kExplosionSites[i].rva + kExplosionSites[i].dispAt) == SiteDisplacement(table, g, &kExplosionSites[i]));
    for (size_t i = 0; i < sizeof kExplosionBounds / sizeof kExplosionBounds[0]; i++)
        assert(*(uint32_t*)(game + kExplosionBounds[i].rva + kExplosionBounds[i].immAt) == EXP_VANILLA + 2);
    assert(*(uint32_t*)(game + 0x1389C9A + 2) == EXP_VANILLA);
    assert(*(uintptr_t*)(table + 7 * 8) == row && *(uintptr_t*)(table + 8) == sentinel);

    uint8_t* made = (uint8_t*)ApiExplosionRow(EXP_VANILLA);
    assert(made != NULL && (uintptr_t)made == *(uintptr_t*)(table + EXP_VANILLA * 8));
    assert(*(uint32_t*)made == EXP_VANILLA && *(uint32_t*)(made + EXP_DAMAGE_AT) == 11);
    assert(*(float*)(made + EXP_INNER_AT) == 1.0f && *(float*)(made + EXP_OUTER_AT) == 12.0f && *(float*)(made + EXP_SHOCKWAVE_AT) == 3.0f);
    uint8_t* same = (uint8_t*)ApiExplosionRow(EXP_VANILLA + 1);
    assert(*(uint32_t*)same == EXP_VANILLA + 1 && *(uint32_t*)(same + EXP_DAMAGE_AT) == 9 && *(float*)(same + EXP_OUTER_AT) == 2.0f);
    assert(ExplosionUsable(EXP_VANILLA + 1) && !ExplosionUsable(EXP_VANILLA + 2) && ApiExplosionRow(EXP_VANILLA + 2) == 0);
    assert(TableRowOf("ExplosionSettings", EXP_VANILLA, &(int){0}) == (uintptr_t)made);
    AddedRowStats("ExplosionSettings", EXP_VANILLA, EXP_ROW, (uintptr_t)made);
    HfContent shown;
    assert(ContentOfKind(HF_KIND_EXPLOSION, EXP_VANILLA, &shown) && shown.base == 7 && shown.link == 11 && !strcmp(shown.name, g_explosions[0].name));
    assert(ContentOfKind(HF_KIND_EXPLOSION, EXP_VANILLA + 1, &shown) && shown.link == 0);
    assert(ExplosionNamed(g_explosions[1].name) == EXP_VANILLA + 1 && ExplosionNamed("none") == 0);

    /* a projectile's own explosions, and the base's when one is not there */
    static uint8_t rowOf[PRJ_ROW];
    Projectile p = {.mod = m, .type = PRJ_VANILLA, .base = 5, .impact = EXP_VANILLA, .expiry = EXP_VANILLA + 1, .row = rowOf};
    const char* why = NULL;
    assert(ProjectileRow(&p, g, &why) == 1);
    assert(*(uint32_t*)(rowOf + PRJ_IMPACT_AT) == EXP_VANILLA && *(uint32_t*)(rowOf + PRJ_EXPIRY_AT) == EXP_VANILLA + 1);
    *(uint32_t*)(*(uint8_t**)(game + PRJ_TABLE + 5 * 8) + PRJ_EXPIRY_AT) = 33;
    p.expiry = EXP_VANILLA + 2;
    assert(ProjectileRow(&p, g, &why) == 1 && *(uint32_t*)(rowOf + PRJ_EXPIRY_AT) == 33);
    *(uint32_t*)(*(uint8_t**)(game + PRJ_TABLE + 5 * 8) + PRJ_EXPIRY_AT) = 0;

    /* the last type the game's 9 bits carry; a bound of 512 is written whole */
    assert(ExplosionRaise(g, EXP_VANILLA + EXP_NARROW_MAX, ApiWrite) == NULL);
    assert(*(uint32_t*)(game + kExplosionBounds[37].rva + kExplosionBounds[37].immAt) == 512);
    g_explosionState = 0; g_explosionCount = EXP_MAX_NEW;
    assert(ApiAddExplosion(m, &wrong) == 0 && g_explosionCount == EXP_MAX_NEW);
    /* too late */
    g_explosionCount = 0;
    InterlockedExchange(&g_fired[HF_EVENT_PROJECTILES_READY], 1);
    assert(ApiAddExplosion(m, &plain) == 0);

    g_explosionCount = explosions; g_explosionState = explosionState; g_explosionTable = explosionTable;
    g_damageCount = damages; g_damageState = damageState;
    ContentEventsRestore();
    VirtualFree(game, 0, MEM_RELEASE);
    puts("explosions: refusals, an unreviewed bound moves nothing, the move, 38 bounds, the rows, a projectile's own explosions");
}

/* The network field a stand-in configuration holds. */
static uint8_t g_fakeFields[(EXP_FIELD + 2) * 0x18];
static int g_fakeFieldsReady;
static int FakeExplosionFields(uintptr_t* rows, uint32_t* count)
{
    if (!g_fakeFieldsReady) return 1;
    *rows = (uintptr_t)g_fakeFields;
    *count = EXP_FIELD + 2;
    return 0;
}

/* A set past 89 types on one image: wide, or the first 89 when the field is not as reviewed. */
static void ExplosionsWidth(int reviewed)
{
    ContentEventsSave();
    size_t size = 0x3800000;
    uint8_t* game = ContentGame(size);
    uintptr_t g = (uintptr_t)game;
    memcpy(game + EXP_CLEAR, kExplosionClearPin, sizeof kExplosionClearPin);
    for (size_t i = 0; i < sizeof kExplosionSites / sizeof kExplosionSites[0]; i++)
        SiteAs(game, kExplosionSites[i].rva, kExplosionSites[i].dispAt, SiteDisplacement(g + EXP_TABLE, g, &kExplosionSites[i]));
    for (size_t i = 0; i < sizeof kExplosionBounds / sizeof kExplosionBounds[0]; i++)
        *(uint32_t*)(game + kExplosionBounds[i].rva + kExplosionBounds[i].immAt) = EXP_VANILLA;
    static uint8_t base[EXP_ROW];
    *(uint32_t*)base = 7;
    uintptr_t row = (uintptr_t)base;
    memcpy(game + EXP_TABLE + 7 * 8, &row, 8);
    memcpy(game + EXP_TABLE + 8, &row, 8);

    uint32_t explosions = g_explosionCount, in = g_explosionsIn;
    int state = g_explosionState;
    uint8_t* table = g_explosionTable;
    g_explosionCount = 0; g_explosionState = 0; g_explosionTable = NULL;
    InterlockedExchange(&g_fired[HF_EVENT_PROJECTILES_READY], 0);
    for (uint32_t i = 0; i < EXP_NARROW_MAX + 1; i++)
    {
        char name[16];
        snprintf(name, sizeof name, "e%u", i);
        HfExplosion e = {sizeof e, .base = 7, .name = name};
        assert(ApiAddExplosion(&g_mods[0], &e) == EXP_VANILLA + i);
    }
    memset(g_fakeFields, 0, sizeof g_fakeFields);
    uint8_t* field = g_fakeFields + EXP_FIELD * 0x18;
    field[12] = 1; field[13] = reviewed ? EXP_FIELD_BITS : 8;
    g_explosionFields = FakeExplosionFields;

    /* the network configuration comes after the explosion list: it is waited for */
    g_fakeFieldsReady = 0;
    assert(InstallExplosions(g, 0) == 0 && g_explosionState == 0);
    g_fakeFieldsReady = 1;
    assert(InstallExplosions(g, 0) == 1 && g_explosionState == 1);
    uint32_t bound = *(uint32_t*)(game + kExplosionBounds[5].rva + kExplosionBounds[5].immAt);
    if (reviewed)
    {
        assert(field[13] == EXP_FIELD_WIDE && field[11] == 0 && field[14] == 0 && bound == 513);
        assert(g_explosionsIn == EXP_NARROW_MAX + 1 && ExplosionUsable(512) && ApiExplosionRow(512) != 0);
    }
    else
    {
        assert(field[13] == 8 && bound == 512);
        assert(g_explosionsIn == EXP_NARROW_MAX && ExplosionUsable(511) && !ExplosionUsable(512) && ApiExplosionRow(512) == 0);
    }

    g_explosionFields = FieldTypes;
    g_explosionCount = explosions; g_explosionsIn = in; g_explosionState = state; g_explosionTable = table;
    ContentEventsRestore();
    VirtualFree(game, 0, MEM_RELEASE);
}

/* An explosive round's weapon package: only for a round with an explosion, by its base when a mod copied it. */
static void BlastPackageFixture(void)
{
    uintptr_t (*saved)(const char*, uint64_t, int*) = g_tableRow;
    g_tableRow = RefTableRow;
    uint8_t* round = (uint8_t*)g_refRows[0];
    uint32_t impact = *(uint32_t*)(round + REF_IMPACT), expiry = *(uint32_t*)(round + REF_EXPIRY);
    *(uint32_t*)(round + REF_IMPACT) = 0; *(uint32_t*)(round + REF_EXPIRY) = 0;
    assert(BlastPackage(40) == 0);
    *(uint32_t*)(round + REF_EXPIRY) = 158;
    assert(BlastPackage(40) == 0xE0807831D6F456C8ull);
    *(uint32_t*)(round + REF_IMPACT) = 158; *(uint32_t*)(round + REF_EXPIRY) = 0;
    assert(BlastPackage(40) == 0xE0807831D6F456C8ull && BlastPackage(9999) == 0);
    *(uint32_t*)(round + REF_IMPACT) = impact; *(uint32_t*)(round + REF_EXPIRY) = expiry;
    g_tableRow = saved;
    puts("explosions: an explosive round asks for its weapon's package, a plain one does not");
}

static void ExplosionsWidthFixture(void)
{
    BlastPackageFixture();
    ExplosionsWidth(1);
    ExplosionsWidth(0);
    puts("explosions: past 89 types field 248 widens to 10 bits, or the first 89 go in");
}
