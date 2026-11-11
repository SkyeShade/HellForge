/* The generation block's capacities (difficulty_block.c, hf->generation_capacity) on synthetic images. With no request
 * the layout and every byte the move writes are the fixed ones this file keeps; a large request keeps its regions apart
 * and every reference inside its own. Included by stats_test.c. */

#define GC_IMAGE 0x1900000u /* past the last site (0x18B63A3) */

static uint8_t* GcImage(void)
{
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, GC_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(game != NULL);
    #define GC_PLANT(table, count) for (size_t i = 0; i < (count); i++) memcpy(game + table[i].rva + table[i].at, &table[i].value, 4)
    GC_PLANT(kLocationRefs, sizeof kLocationRefs / sizeof kLocationRefs[0]);
    GC_PLANT(kLocationCaps, sizeof kLocationCaps / sizeof kLocationCaps[0]);
    GC_PLANT(kGenerationBlockSize, sizeof kGenerationBlockSize / sizeof kGenerationBlockSize[0]);
    GC_PLANT(kLocationTypeRefs, sizeof kLocationTypeRefs / sizeof kLocationTypeRefs[0]);
    GC_PLANT(kLocationTypeCaps, sizeof kLocationTypeCaps / sizeof kLocationTypeCaps[0]);
    GC_PLANT(kMinimapRefs, sizeof kMinimapRefs / sizeof kMinimapRefs[0]);
    GC_PLANT(kMinimapCaps, sizeof kMinimapCaps / sizeof kMinimapCaps[0]);
    GC_PLANT(kStampRefs, sizeof kStampRefs / sizeof kStampRefs[0]);
    GC_PLANT(kPlaceListRefs, sizeof kPlaceListRefs / sizeof kPlaceListRefs[0]);
    GC_PLANT(kPlaceSetRefs, sizeof kPlaceSetRefs / sizeof kPlaceSetRefs[0]);
    GC_PLANT(kClusterRefs, 16);
    GC_PLANT(kClusterCaps, 16);
    #undef GC_PLANT
    for (int e = 0; e < GEN_ALL - GEN_REGIONS; e++)
        for (uint32_t i = 0; i < kGenerationExtras[e].refCount + kGenerationExtras[e].capCount; i++)
        {
            const GenerationExtra* x = &kGenerationExtras[e];
            const LocationSite* site = i < x->refCount ? &x->refs[i] : &x->caps[i - x->refCount];
            memcpy(game + site->rva + site->at, &site->value, 4);
        }
    memcpy(game + kClusterCountSites[0].site, kClusterCountPinA, 9); memcpy(game + kClusterCountSites[1].site, kClusterCountPinB, 7);
    for (int i = 0; i < 74; i++) memcpy(game + kObjectiveListSites[i].rva, kObjectiveListSites[i].pin, kObjectiveListSites[i].length);
    memcpy(game + DIF_STAMP_APPEND_SITE, kStampAppendPin, sizeof kStampAppendPin);
    return game;
}

/* The fixed layout written out number by number: what a move with no request must equal. */
static void GcFixedMove(uint8_t* game)
{
    #define GC_PUT(table, count, expression) for (size_t i = 0; i < (count); i++) { uint32_t v = (expression); memcpy(game + table[i].rva + table[i].at, &v, 4); }
    GC_PUT(kGenerationBlockSize, 6, 0x15B1100u)
    GC_PUT(kLocationRefs, sizeof kLocationRefs / sizeof kLocationRefs[0], kLocationRefs[i].value >= 0x7EEBA0u ?
        0x1367280u + (kLocationRefs[i].value - 0x7EEBA0u) : 0x11A7280u + (kLocationRefs[i].value - 0x758E20u))
    GC_PUT(kLocationCaps, sizeof kLocationCaps / sizeof kLocationCaps[0], 8192u)
    GC_PUT(kLocationTypeRefs, sizeof kLocationTypeRefs / sizeof kLocationTypeRefs[0], 0x13E7280u)
    GC_PUT(kLocationTypeCaps, sizeof kLocationTypeCaps / sizeof kLocationTypeCaps[0], 512u)
    GC_PUT(kMinimapRefs, sizeof kMinimapRefs / sizeof kMinimapRefs[0], kMinimapRefs[i].value == 0x8198A0u ? 0x13E9280u :
        kMinimapRefs[i].value == 0x8498A0u ? 0x14A9280u : kMinimapRefs[i].value == 0x118BEFCu ? 0x1559280u : 0x1569280u)
    GC_PUT(kMinimapCaps, sizeof kMinimapCaps / sizeof kMinimapCaps[0], kMinimapCaps[i].value * 4u)
    GC_PUT(kStampRefs, sizeof kStampRefs / sizeof kStampRefs[0], 0x15A9280u)
    GC_PUT(kPlaceListRefs, sizeof kPlaceListRefs / sizeof kPlaceListRefs[0], 0x15AD500u)
    GC_PUT(kPlaceSetRefs, sizeof kPlaceSetRefs / sizeof kPlaceSetRefs[0], 0x15AE500u)
    GC_PUT(kClusterRefs, 16, 0x15AF500u)
    GC_PUT(kClusterCaps, 16, 32u)
    #undef GC_PUT
    for (int i = 0; i < 74; i++)
        memcpy(game + kObjectiveListSites[i].rva + kObjectiveListSites[i].at, &kObjectiveListSites[i].value, kObjectiveListSites[i].width);
}

static uint32_t GcRead(const uint8_t* game, const LocationSite* site) { uint32_t v; memcpy(&v, game + site->rva + site->at, 4); return v; }
static int GcInside(const GenerationLayout* l, int region, uint32_t value) { return value - l->region[region].at < l->region[region].bytes; }

/* After the fixture's install with no request (stats_test.c DifficultyFixture, game its image). */
static void GenerationDefaultFixture(const uint8_t* game)
{
    static const uint32_t fixed[GEN_REGIONS] = {0x11A7280u, 0x1367280u, 0x13E7280u, 0x13E9280u, 0x14A9280u, 0x1559280u, 0x1569280u, 0x15A9280u,
        0x15AD280u, 0x15AD500u, 0x15AE500u, 0x15AF500u};
    HfGenerationCapacity none = {sizeof none};
    GenerationLayout l;
    GenerationLayoutOf(&none, &l);
    for (int i = 0; i < GEN_REGIONS; i++) assert(l.region[i].at == fixed[i]);
    assert(l.block == 0x15B1100u && l.objectiveList == 0x15AD370u && l.minimapScale == 4);
    assert(l.caps.locations == 8192 && l.caps.minimap_proxies == 16384 && l.caps.mission_stamps == 1024 && l.caps.places == 512 && l.caps.place_set == 256);
    assert(GenerationLayoutFailed(&l) == NULL && !g_generationAsked && !memcmp(Generation(), &l, sizeof l));
    for (int i = 0, kind; i < 74; i++) assert(ObjectiveListValue(&l, i, &kind) == kObjectiveListSites[i].value);
    /* the lists moved only on request stay where the game has them, with its places */
    uint32_t places = 0;
    for (int i = GEN_REGIONS; i < GEN_ALL; i++) assert(l.region[i].at == 0 && l.region[i].bytes == 0);
    assert(l.caps.important_locations == 256 && l.caps.linked_locations == 64 && ImportantLocations(&places) == 0x8996B0u && places == 256);
    assert(l.caps.route_nodes == 975 && l.caps.route_edges == 1621 && l.caps.route_points == 9936);
    /* the install's own steps write none of those lists' sites (this image holds zeros there): a request can move them
     * after every other step */
    for (int e = 0; e < GEN_ALL - GEN_REGIONS; e++)
        for (uint32_t i = 0; i < kGenerationExtras[e].refCount + kGenerationExtras[e].capCount; i++)
        {
            const GenerationExtra* x = &kGenerationExtras[e];
            const LocationSite* site = i < x->refCount ? &x->refs[i] : &x->caps[i - x->refCount];
            static const uint8_t zeros[16];
            assert(!memcmp(game + site->rva, zeros, site->at + 4u));
        }
    /* nothing of a request's is installed, and the mod reads the moved block's capacities */
    static const uint8_t untouched[7];
    assert(!memcmp(game + DIF_STAMP_APPEND_SITE, untouched, 7) && g_stampsLeftOut == NULL);
    HfGenerationCapacity have = {sizeof have}, want = {sizeof want};
    assert(ApiGenerationCapacity(&g_mods[0], NULL, &have) == 0 && have.moved == 1 && have.block_bytes == 0x15B1100u && have.locations == 8192 && have.size == sizeof have);
    /* a request after the game loaded is refused and changes nothing */
    want.locations = 40000;
    assert(ApiGenerationCapacity(&g_mods[0], &want, &have) == 1 && have.locations == 8192 && !g_generationAsked && g_generationWant.locations == 0);
    assert(strstr(ApiLastLog(&g_mods[0]), "too late") != NULL);
}

static void GenerationCapacityFixture(void)
{
    /* requests combine to the largest of each; one over a limit or without its size is refused whole */
    HfGenerationCapacity all = {0}, a = {sizeof a}, b = {sizeof b}, over = {sizeof over}, unsized = {8};
    a.locations = 40000; a.mission_stamps = 2000; a.places = 100;
    b.locations = 65024; b.minimap_proxies = 20000;
    over.places = 600; over.locations = 65025;
    assert(GenerationMerge(&all, &a) == NULL && GenerationMerge(&all, &b) == NULL && GenerationMerge(&all, &over) != NULL && GenerationMerge(&all, &unsized) != NULL);
    assert(all.locations == 65024 && all.minimap_proxies == 20000 && all.mission_stamps == 2000 && all.places == 100 && all.place_set == 0);
    /* a mod built with the struct's first fields only: its request counts, the later lists stay, and no more than its
     * size is filled in */
    struct { HfGenerationCapacity c; uint32_t after; } early = {{offsetof(HfGenerationCapacity, important_locations)}, 0xA5A5A5A5u};
    early.c.important_locations = 0xA5A5A5A5u; early.c.linked_locations = 0xA5A5A5A5u; /* not the mod's: past its size */
    early.c.place_set = 300;
    assert(GenerationMerge(&all, &early.c) == NULL && all.place_set == 300 && all.important_locations == 0 && all.linked_locations == 0);
    assert(ApiGenerationCapacity(&g_mods[0], NULL, &early.c) == 0 && early.c.size == offsetof(HfGenerationCapacity, important_locations) &&
        early.c.moved == 1 && early.c.important_locations == 0xA5A5A5A5u && early.c.linked_locations == 0xA5A5A5A5u && early.after == 0xA5A5A5A5u);
    all.place_set = 0;
    over.locations = 0; over.places = 0; over.important_locations = 1025;
    assert(GenerationMerge(&all, &over) != NULL && all.important_locations == 0);
    /* and one built before the route graph's fields */
    struct { HfGenerationCapacity c; uint32_t after; } middle = {{offsetof(HfGenerationCapacity, route_nodes)}, 0xA5A5A5A5u};
    middle.c.route_nodes = 0xA5A5A5A5u; middle.c.route_edges = 0xA5A5A5A5u; middle.c.route_points = 0xA5A5A5A5u;
    middle.c.linked_locations = 70;
    assert(GenerationMerge(&all, &middle.c) == NULL && all.linked_locations == 70 && all.route_nodes == 0 && all.route_edges == 0 && all.route_points == 0);
    assert(ApiGenerationCapacity(&g_mods[0], NULL, &middle.c) == 0 && middle.c.linked_locations == 64 && middle.c.route_nodes == 0xA5A5A5A5u &&
        middle.c.route_points == 0xA5A5A5A5u && middle.after == 0xA5A5A5A5u);
    all.linked_locations = 0;
    for (int i = 0; i < 3; i++)
    {
        HfGenerationCapacity most = {sizeof most};
        *GenerationField(&most, GEN_FIELD_ROUTE_NODES + i) = kGenerationLimits[GEN_FIELD_ROUTE_NODES + i][1] + 1u;
        assert(GenerationMerge(&all, &most) != NULL);
        *GenerationField(&most, GEN_FIELD_ROUTE_NODES + i) -= 1u;
        HfGenerationCapacity alone = {0};
        GenerationLayout one;
        assert(GenerationMerge(&alone, &most) == NULL);
        GenerationLayoutOf(&alone, &one);
        /* one list at its limit moves alone (the edges take their class bytes along); the game's own places move none */
        assert(GenerationLayoutFailed(&one) == NULL && *GenerationField(&one.caps, GEN_FIELD_ROUTE_NODES + i) == kGenerationLimits[GEN_FIELD_ROUTE_NODES + i][1]);
        for (int r = GEN_REGIONS; r < GEN_ALL; r++)
            assert((one.region[r].bytes != 0) == (r == (i == 0 ? GEN_ROUTE_NODES : i == 1 ? GEN_ROUTE_EDGES : GEN_ROUTE_POINTS) || (i == 1 && r == GEN_ROUTE_CLASSES)));
        *GenerationField(&alone, GEN_FIELD_ROUTE_NODES + i) = kGenerationLimits[GEN_FIELD_ROUTE_NODES + i][0];
        GenerationLayoutOf(&alone, &one);
        assert(one.block == 0x15B1100u && one.region[GEN_ROUTE_NODES + (i == 2 ? 3 : i)].bytes == 0);
    }
    assert(all.route_nodes == 0 && all.route_edges == 0 && all.route_points == 0);
    a.important_locations = 1000; a.linked_locations = 100; a.route_nodes = 2000; a.route_edges = 3000; a.route_points = 60000;
    assert(GenerationMerge(&all, &a) == NULL && all.important_locations == 1000 && all.linked_locations == 100 && all.route_edges == 3000);
    GenerationLayout l, bad;
    GenerationLayoutOf(&all, &l);
    assert(l.caps.locations == 65024 && l.caps.minimap_proxies == 20480 && l.minimapScale == 5 && l.caps.mission_stamps == 2000 && l.caps.places == 512 && l.caps.place_set == 256);
    assert(GenerationLayoutFailed(&l) == NULL);
    for (int i = 0; i + 1 < GEN_ALL; i++) assert(l.region[i].bytes != 0 && l.region[i].at + l.region[i].bytes <= l.region[i + 1].at);
    assert(l.region[GEN_A].at >= 0x11A7268u && l.region[GEN_ALL - 1].at + l.region[GEN_ALL - 1].bytes <= l.block && l.block % 16u == 0);
    assert(l.caps.important_locations == 1008 && l.region[GEN_IMPORTANT].bytes == 1008u * 0x108u && l.caps.linked_locations == 112 &&
        l.region[GEN_LINKED].bytes == 112u * 0x18u);
    assert(l.caps.route_nodes == 2000 && l.region[GEN_ROUTE_NODES].bytes == 2000u * 0x98u && l.caps.route_edges == 3008 &&
        l.region[GEN_ROUTE_EDGES].bytes == 3008u * 0x48u && l.region[GEN_ROUTE_CLASSES].bytes == 3008u &&
        l.caps.route_points == 60000 && l.region[GEN_ROUTE_POINTS].bytes == 60000u * 8u);
    for (int e = 0; e < GEN_ALL - GEN_REGIONS; e++) assert(l.region[kGenerationExtras[e].region].at % 16u == kGenerationExtras[e].old % 16u);
    /* the objective list lies past the grown arrays, and the negated displacement follows it */
    assert(l.objectiveList > l.region[GEN_STAMPS].at && l.objectiveList != 0x15AD370u);
    int negated = 0;
    for (int i = 0, kind; i < 74; i++)
    {
        uint32_t value = ObjectiveListValue(&l, i, &kind);
        if (kObjectiveListSites[i].rva == 0xF70F70u) { assert(kind == 2 && value == 0u - (l.objectiveList + 4u)); negated++; }
        else if (kind == 1) assert(value == l.objectiveList + (kObjectiveListSites[i].old - 0x8BC570u));
        else assert(kind == 0 && value == kObjectiveListSites[i].value);
    }
    assert(negated == 1);
    /* the self-check refuses overlapping regions, a list left at its fixed place, and a size that is not the end */
    bad = l; bad.region[GEN_STAMPS].at -= 16; assert(GenerationLayoutFailed(&bad) != NULL);
    bad = l; bad.objectiveList = 0x15AD370u; assert(GenerationLayoutFailed(&bad) != NULL);
    bad = l; bad.block += 16; assert(GenerationLayoutFailed(&bad) != NULL);
    bad = l; bad.region[GEN_A].bytes -= 0xE0u; assert(GenerationLayoutFailed(&bad) != NULL);
    bad = l; bad.region[GEN_IMPORTANT].at = bad.region[GEN_CLUSTERS].at; assert(GenerationLayoutFailed(&bad) != NULL);
    bad = l; bad.region[GEN_LINKED].at = 0x8AB6C0u; assert(GenerationLayoutFailed(&bad) != NULL);
    bad = l; bad.region[GEN_IMPORTANT].bytes = 0x108u; assert(GenerationLayoutFailed(&bad) != NULL);
    /* every limit at once still fits a signed 32-bit displacement */
    HfGenerationCapacity most = {sizeof most};
    for (int i = 0; i < GEN_FIELDS; i++) *GenerationField(&most, i) = kGenerationLimits[i][1];
    assert(GenerationMerge(&all, &most) == NULL);
    GenerationLayoutOf(&all, &bad);
    assert(GenerationLayoutFailed(&bad) == NULL && bad.block < 0x7FFFFFFFu && bad.caps.mission_stamps == 16384 && bad.minimapScale == 8);

    /* no request: the move writes the fixed layout's bytes, and only those (the cluster counts' jumps lead to stubs) */
    DifficultyInstall d = {0};
    uint8_t* plain = GcImage(), *fixed = GcImage();
    assert(!g_generationAsked && DifficultyNeedsBlock());
    g_generation.block = 0;
    d.game = (uintptr_t)plain;
    assert(LocationRefsFailed(d.game) == NULL && ObjectiveListFailed(d.game) == NULL && MoveGenerationBlock(&d) == NULL);
    GcFixedMove(fixed);
    for (int i = 0; i < 2; i++)
    {
        assert(plain[kClusterCountSites[i].site] == 0xE9);
        memcpy(fixed + kClusterCountSites[i].site, plain + kClusterCountSites[i].site, kClusterCountSites[i].length);
    }
    assert(!memcmp(plain, fixed, GC_IMAGE) && g_stampsLeftOut == NULL);

    /* a large request: every site addresses its own region with its capacity, and the stamps' append is guarded */
    uint8_t* game = GcImage();
    HfGenerationCapacity saved = g_generationWant;
    g_generationWant = l.caps; g_generationAsked = 1; g_generation.block = 0;
    d.game = (uintptr_t)game;
    assert(LocationRefsFailed(d.game) == NULL && ObjectiveListFailed(d.game) == NULL && MoveGenerationBlock(&d) == NULL);
    assert(!memcmp(Generation(), &l, sizeof l));
    for (size_t i = 0; i < sizeof kGenerationBlockSize / sizeof kGenerationBlockSize[0]; i++) assert(GcRead(game, &kGenerationBlockSize[i]) == l.block);
    for (size_t i = 0; i < sizeof kLocationRefs / sizeof kLocationRefs[0]; i++)
    {
        int region = kLocationRefs[i].value >= 0x7EEBA0u ? GEN_B : GEN_A;
        uint32_t v = GcRead(game, &kLocationRefs[i]);
        assert(GcInside(&l, region, v) && v - l.region[region].at == kLocationRefs[i].value - (region == GEN_A ? 0x758E20u : 0x7EEBA0u));
    }
    for (size_t i = 0; i < sizeof kLocationCaps / sizeof kLocationCaps[0]; i++) assert(GcRead(game, &kLocationCaps[i]) == 65024u);
    for (size_t i = 0; i < sizeof kLocationTypeRefs / sizeof kLocationTypeRefs[0]; i++) assert(GcRead(game, &kLocationTypeRefs[i]) == l.region[GEN_TYPES].at);
    for (size_t i = 0; i < sizeof kLocationTypeCaps / sizeof kLocationTypeCaps[0]; i++) assert(GcRead(game, &kLocationTypeCaps[i]) == 512u);
    for (size_t i = 0; i < sizeof kMinimapRefs / sizeof kMinimapRefs[0]; i++)
    {
        uint32_t old = kMinimapRefs[i].value;
        assert(GcRead(game, &kMinimapRefs[i]) == l.region[old == 0x8198A0u ? GEN_P : old == 0x8498A0u ? GEN_S : old == 0x118BEFCu ? GEN_U : GEN_V].at);
    }
    for (size_t i = 0; i < sizeof kMinimapCaps / sizeof kMinimapCaps[0]; i++) assert(GcRead(game, &kMinimapCaps[i]) == kMinimapCaps[i].value * 5u);
    for (size_t i = 0; i < sizeof kStampRefs / sizeof kStampRefs[0]; i++) assert(GcRead(game, &kStampRefs[i]) == l.region[GEN_STAMPS].at);
    for (size_t i = 0; i < sizeof kPlaceListRefs / sizeof kPlaceListRefs[0]; i++) assert(GcRead(game, &kPlaceListRefs[i]) == l.region[GEN_PLACES].at);
    for (size_t i = 0; i < sizeof kPlaceSetRefs / sizeof kPlaceSetRefs[0]; i++) assert(GcRead(game, &kPlaceSetRefs[i]) == l.region[GEN_PLACE_SET].at);
    for (int i = 0; i < 16; i++) assert(GcRead(game, &kClusterRefs[i]) == l.region[GEN_CLUSTERS].at && GcRead(game, &kClusterCaps[i]) == 32u);
    for (int i = 0, kind; i < 74; i++)
    {
        uint32_t v = 0, value = ObjectiveListValue(&l, i, &kind);
        memcpy(&v, game + kObjectiveListSites[i].rva + kObjectiveListSites[i].at, kObjectiveListSites[i].width);
        assert(v == (kObjectiveListSites[i].width == 4 ? value : (value & 0xFFu)));
        assert(kind == 0 || GcInside(&l, GEN_OBJLIST, kind == 2 ? 0u - v : v));
    }
    {
        int32_t rel; uint32_t places;
        assert(game[DIF_STAMP_APPEND_SITE] == 0xE9 && game[DIF_STAMP_APPEND_SITE + 5] == 0x90 && game[DIF_STAMP_APPEND_SITE + 6] == 0x90);
        memcpy(&rel, game + DIF_STAMP_APPEND_SITE + 1, 4);
        const uint8_t* stub = game + DIF_STAMP_APPEND_SITE + 5 + rel;
        memcpy(&places, stub + 3, 4);
        assert(places == 2000u && !memcmp(stub, kStampAppendStub, 3) && !memcmp(stub + 7, kStampAppendStub + 7, 10) && stub[21] == 0xF0);
        memcpy(&rel, stub + 17, 4); assert(stub + 21 + rel == game + DIF_STAMP_APPEND_BACK);
        memcpy(&rel, stub + 29, 4); assert(stub + 33 + rel == game + DIF_STAMP_APPEND_SKIP);
        memcpy(&rel, stub + 24, 4); assert(stub + 28 + rel == (const uint8_t*)g_stampsLeftOut && *g_stampsLeftOut == 0);
    }
    /* each list moved on request: every reference inside its region with its field's offset, capacities raised, and the
     * loader's own reader follows */
    for (int e = 0; e < GEN_ALL - GEN_REGIONS; e++)
    {
        const GenerationExtra* x = &kGenerationExtras[e];
        HfGenerationCapacity caps = l.caps;
        for (uint32_t i = 0; i < x->refCount; i++)
        {
            uint32_t v = GcRead(game, &x->refs[i]);
            assert(GcInside(&l, x->region, v) && v - l.region[x->region].at == x->refs[i].value - x->old);
        }
        for (uint32_t i = 0; i < x->capCount; i++) assert(GcRead(game, &x->caps[i]) == *GenerationField(&caps, x->field));
    }
    {
        /* the route graph by name: the tables above could agree with each other and still be the wrong list's */
        const struct { const LocationSite* caps; size_t count; uint32_t places; const LocationSite* ref; int region; uint32_t old; } routes[4] = {
            {kRouteNodeCaps, sizeof kRouteNodeCaps / sizeof kRouteNodeCaps[0], 2000u, kRouteNodeRefs, GEN_ROUTE_NODES, 0x8C4E40u},
            {kRouteEdgeCaps, sizeof kRouteEdgeCaps / sizeof kRouteEdgeCaps[0], 3008u, kRouteEdgeRefs, GEN_ROUTE_EDGES, 0x937348u},
            {kRouteClassCaps, sizeof kRouteClassCaps / sizeof kRouteClassCaps[0], 3008u, kRouteClassRefs, GEN_ROUTE_CLASSES, 0x11A1F98u},
            {kRoutePointCaps, sizeof kRoutePointCaps / sizeof kRoutePointCaps[0], 60000u, kRoutePointRefs, GEN_ROUTE_POINTS, 0x9568E4u}};
        for (int r = 0; r < 4; r++)
        {
            for (size_t i = 0; i < routes[r].count; i++) assert(GcRead(game, &routes[r].caps[i]) == routes[r].places);
            assert(GcRead(game, routes[r].ref) == l.region[routes[r].region].at + (routes[r].ref->value - routes[r].old));
        }
        uint32_t places = 0;
        assert(ImportantLocations(&places) == l.region[GEN_IMPORTANT].at && places == 1008);
    }
    /* a changed append is refused before anything is written */
    game[DIF_STAMP_APPEND_SITE] = 0xE9;
    assert(ObjectiveListFailed(d.game) != NULL);
    memcpy(game + DIF_STAMP_APPEND_SITE, kStampAppendPin, sizeof kStampAppendPin); /* and so is a moved list's changed site */
    assert(ObjectiveListFailed(d.game) != NULL);
    g_generationWant = saved; g_generationAsked = 0; g_generation.block = 0;
    VirtualFree(plain, 0, MEM_RELEASE); VirtualFree(fixed, 0, MEM_RELEASE);
    printf("PASS generation capacity: no request writes the fixed layout's bytes, requests combine and are refused late or over a limit, a large layout's regions and references, the negated displacement, the stamps' guard, lists and the route graph moved on request, shorter structs\n");
}
