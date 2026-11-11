/* The generation block's capacities (difficulty_block.c, hf->generation_capacity) on synthetic images. With no request
 * the layout and every byte the move writes are the fixed ones this file keeps; a large request keeps its regions apart
 * and every reference inside its own. Included by stats_test.c. */

#define GC_IMAGE 0x1800000u /* past the last site (0x177DECD) */

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
    GenerationLayout l, bad;
    GenerationLayoutOf(&all, &l);
    assert(l.caps.locations == 65024 && l.caps.minimap_proxies == 20480 && l.minimapScale == 5 && l.caps.mission_stamps == 2000 && l.caps.places == 512 && l.caps.place_set == 256);
    assert(GenerationLayoutFailed(&l) == NULL);
    for (int i = 0; i + 1 < GEN_REGIONS; i++) assert(l.region[i].at + l.region[i].bytes <= l.region[i + 1].at);
    assert(l.region[GEN_A].at >= 0x11A7268u && l.region[GEN_CLUSTERS].at + l.region[GEN_CLUSTERS].bytes == l.block);
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
    /* every limit at once still fits a signed 32-bit displacement */
    HfGenerationCapacity most = {sizeof most};
    for (int i = 0; i < 5; i++) *GenerationField(&most, i) = kGenerationLimits[i][1];
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
    /* a changed append is refused before anything is written */
    game[DIF_STAMP_APPEND_SITE] = 0xE9;
    assert(ObjectiveListFailed(d.game) != NULL);
    g_generationWant = saved; g_generationAsked = 0; g_generation.block = 0;
    VirtualFree(plain, 0, MEM_RELEASE); VirtualFree(fixed, 0, MEM_RELEASE);
    printf("PASS generation capacity: no request writes the fixed layout's bytes, requests combine and are refused late or over a limit, a large layout's regions and references, the negated displacement, the stamps' guard\n");
}
