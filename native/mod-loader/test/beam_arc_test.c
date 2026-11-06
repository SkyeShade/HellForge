/* New beam and arc types (beam_arc.c) on a synthetic game image: the refusals, one site not as reviewed moving nothing,
 * the move, the rows, and the rule that a type not installed never reaches anything. Included by stats_test.c after
 * explosions_test.c. */

static void RowKindFixture(RowKind* k, uint32_t damageAt)
{
    uint8_t* game = ContentGame(0x3800000);
    uintptr_t g = (uintptr_t)game;
    for (size_t i = 0; i < k->siteCount; i++)
        SiteAs(game, k->sites[i].rva, k->sites[i].dispAt, SiteDisplacement(g + k->table, g, &k->sites[i]));
    uint8_t* base = (uint8_t*)calloc(1, k->stride);
    *(uint32_t*)base = 3;
    *(uint32_t*)(base + damageAt) = 9;
    *(float*)(base + BEAM_RADIUS_AT) = 1.5f; /* a beam's; on an arc only bytes of a copy */

    HfMod* m = &g_mods[0];
    assert(AddRow(k, m, "x", 0, 0, 0, 0) == 0 && AddRow(k, m, "x", k->vanilla, 0, 0, 0) == 0);
    assert(AddRow(k, m, "x", 3, DMG_VANILLA, 0, 0) == 0 && AddRow(k, m, "x", 3, 0, -1.0f, 0) == 0 && k->count == 0);
    uint32_t own = AddRow(k, m, "own", 3, 11, 0, 0), plain = AddRow(k, m, "plain", 3, 0, 0, 0);
    assert(own == k->vanilla && plain == k->vanilla + 1 && AddRow(k, m, "own", 3, 0, 0, 0) == 0);
    assert(BeamArcKnown(k, own) && !BeamArcKnown(k, plain + 1));
    assert(BeamArcUsable(k, 3) && !BeamArcUsable(k, own) && RowOfType(k, own) == 0);

    /* not loaded yet; a site not as reviewed waits out the grace period, then nothing moves and no type is usable */
    assert(InstallRows(k, g, 1) == 0 && k->state == 0);
    uintptr_t row = (uintptr_t)base;
    memcpy(game + k->table + 8, &row, 8);
    memcpy(game + k->table + 3 * 8, &row, 8);
    int32_t* site = (int32_t*)(game + k->sites[k->siteCount - 1].rva + k->sites[k->siteCount - 1].dispAt);
    int32_t kept = *site, first = *(int32_t*)(game + k->sites[0].rva + k->sites[0].dispAt);
    *site = kept + 8;
    assert(InstallRows(k, g, 0) == 0 && k->state == 0);
    assert(InstallRows(k, g, 1) == 1 && k->state == -1 && k->list == NULL && !BeamArcUsable(k, own));
    assert(*(int32_t*)(game + k->sites[0].rva + k->sites[0].dispAt) == first);
    *site = kept;

    /* a base row that does not name its type refuses too */
    k->state = 0;
    *(uint32_t*)base = 4;
    assert(InstallRows(k, g, 1) == 1 && k->state == -1 && !BeamArcUsable(k, own));
    *(uint32_t*)base = 3;

    k->state = 0;
    assert(InstallRows(k, g, 0) == 1 && k->state == 1 && k->list != NULL);
    for (size_t i = 0; i < k->siteCount; i++)
        assert(*(int32_t*)(game + k->sites[i].rva + k->sites[i].dispAt) == SiteDisplacement((uintptr_t)k->list, g, &k->sites[i]));
    assert(*(uintptr_t*)(k->list + 3 * 8) == row);
    uint8_t* made = (uint8_t*)RowOfType(k, own);
    assert(made != NULL && (uintptr_t)made == *(uintptr_t*)(k->list + (size_t)own * 8) && *(uint32_t*)made == own && *(uint32_t*)(made + damageAt) == 11);
    assert(*(uint32_t*)RowOfType(k, plain) == plain && *(uint32_t*)(RowOfType(k, plain) + damageAt) == 9);
    assert(BeamArcUsable(k, own) && BeamArcUsable(k, plain) && !BeamArcUsable(k, plain + 1) && RowOfType(k, plain + 1) == 0);
    assert(*(uintptr_t*)(k->list + ((size_t)plain + 1) * 8) == 0);

    /* the editor's and the stats mods' way to it: content, the row by its id, a field */
    const char* table = k == &g_beams ? "BeamSettings" : "ArcSettings";
    assert(TableRowOf(table, own, &(int){0}) == (uintptr_t)made && TableRowOf(table, plain + 1, &(int){0}) == 0);
    AddedRowStats(table, own, k->stride, (uintptr_t)made);
    HfContent shown;
    assert(ContentOfKind(k == &g_beams ? HF_KIND_WEAPON_BEAM : HF_KIND_ARC, own, &shown) && shown.base == 3 && shown.link == 11);
    assert(RowNamed(k, k->added[0].name) == own && RowNamed(k, "none") == 0);

    /* too late */
    k->state = 0;
    InterlockedExchange(&g_fired[HF_EVENT_PROJECTILES_READY], 1);
    assert(AddRow(k, m, "late", 3, 0, 0, 0) == 0);
    InterlockedExchange(&g_fired[HF_EVENT_PROJECTILES_READY], 0);

    k->count = 0; k->state = 0; k->list = NULL;
    free(base);
    VirtualFree(game, 0, MEM_RELEASE);
}

static void BeamArcFixture(void)
{
    ContentEventsSave();
    InterlockedExchange(&g_fired[HF_EVENT_PROJECTILES_READY], 0);
    uint32_t damages = g_damageCount;
    int damageState = g_damageState;
    RowKind beams = g_beams, arcs = g_arcs;
    g_damageCount = 0; g_damageState = 1;
    g_beams.count = g_arcs.count = 0; g_beams.state = g_arcs.state = 0; g_beams.list = g_arcs.list = NULL;
    RowKindFixture(&g_beams, BEAM_DAMAGE_AT);
    RowKindFixture(&g_arcs, ARC_DAMAGE_AT);

    /* a beam's radius and length */
    g_beams.count = 0;
    assert(AddRow(&g_beams, &g_mods[0], "wide", 3, 0, 2.0f, 40.0f) == BEAM_VANILLA);
    assert(g_beams.added[0].radius == 2.0f && g_beams.added[0].length == 40.0f);

    g_beams = beams; g_arcs = arcs;
    g_damageCount = damages; g_damageState = damageState;
    ContentEventsRestore();
    puts("beams and arcs: refusals, one site not as reviewed moves nothing, the moves, the rows, nothing unfilled is usable");
}
