/* An extra large city map's city ops gate their content on the distance from each city, not from the world's centre.
 *
 * Seven city generation sites let content stand only within a distance of the WORLD's centre (S/2, S/2), S the pass's
 * world size ([[pass+8]+0x2C], u32) and S+0x50 its playable fraction ([[pass+8]+0x50]):
 *   g = |p - (S/2, S/2)| - S x [S+0x50] / 2, content allowed while g <= the rule's margin.
 * - op 0xAB PlaceCityRoadStamps (0xFB2540), per route edge (r15: x0, y0, x1, y1 at +0/+4/+8/+0xC): the edge's nearest
 *   point to the centre (0xFB2727..0xFB279E), its g in xmm7 (0xFB27A6..0xFB2821), tested against every rule of the
 *   edge's cell type (rbp: rules [rbp+0x10] of 0x70, count [rbp+0x18]) at 0xFB283A `comiss xmm7, [rdi+0x54]; ja`;
 * - its per-point placer 0xFB1DB0: the point (xmm13, xmm14 -> [rsp+0x30]) and its g in xmm0 (0xFB20AF..0xFB2155),
 *   0xFB215A `comiss xmm0, [r13+0x54]; ja` (a retry);
 * - op 0xA7's building placer 0xF993E0: the point ([rsp+0x50], [rsp+0x54], 0xF9954F / 0xF99586) and its g in xmm0
 *   (0xF9957C..0xF995DE), 0xF995E3 `comiss xmm0, [r15+0x54]; jae 0xF99AB7` (refused at g >= the margin);
 * - op 0xAF GenerateCityWalls through 0xFA39D0: the point ([rsp+0xD8], [rsp+0xDC] -> xmm7, xmm8) and its g in xmm0
 *   (0xFA3FC0..0xFA401E), 0xFA4022 `comiss xmm0, [rsi+0x54]; ja`;
 * - op 0xA8 FlattenCityRoads' first stage (0xFAF130): the point ([rbp+0x8], [rbp+0xC]) and its g in xmm0
 *   (0xFB0B71..0xFB0BD8), 0xFB0BDC `comiss xmm0, [rdi+0x280]; ja`;
 * - its second and third stages: the point ([rbp-0x30], [rbp-0x2C], stored at 0xFB143B / 0xFB1445; [rbp-0x38],
 *   [rbp-0x34], 0xFB16CC / 0xFB16D6; each later handed to 0xFCCA50 as rdx) and its g in xmm0 (0xFB1460..0xFB14C0,
 *   0xFB16D1..0xFB1733), the margin [rdi+0x258] in xmm1 (0xFB14B8, 0xFB172B), 0xFB14C4 / 0xFB1737 `comiss xmm1, xmm0;
 *   jbe` (content only while the margin is above g).
 * A vanilla city is at the world's centre: S = 2048, S+0x50 = 0.323 (s74s), so S x S+0x50 / 2 = 331 m and the city's
 * dense core stops 330-400 m from its centre (339 blocks). An extra large level's cities stand 2.6-4 km off the centre
 * (g_cities, difficulty_world.c) and its radius is 6619 m (S = 16384, S+0x50 = 0.808, s75t), so every test passed and
 * each city's blocks filled its cells out to ~700 m (684-862 blocks per city in s75t, audit 13: 2.5 x vanilla).
 *
 * Here, on an extra large level with its cities (g_cities.level == g_world.levels), each site's g is
 *   g = min over the cities |p - c_k| - CITY_FRAME_K x CITY_GATE_RADIUS
 * (for an edge: the distance from c_k to the edge's segment), so each city keeps a vanilla city's gate about its own
 * centre. On every other level the helper hands back the game's own g (bit for bit: the stub runs the original
 * instruction first and writes only the low lane the following comiss reads). Op 0x2B's map-wide gate (0xF347D9) is
 * left alone.
 *
 * Each stub: the replaced instruction(s), the volatile registers and xmm0..xmm5 saved (the difficulty_cities.c stub
 * macros; each site's frame is fixed and 16-byte aligned, so 7 pushes + 0x88 keep the call aligned, and game [rsp+X]
 * is stub [rsp+0xC0+X]), CityGate(site, the point or the edge, g, the margin or the cell type), the answer into the
 * register the comiss reads, everything else as it was, then back. The walls and the three 0xA8 stages replace their
 * comiss too (the subss alone is 4 bytes); the stub runs it last, after the frame is restored (xmm1, the later stages'
 * margin, is restored with xmm0..xmm5), and its flags reach the ja / jbe.
 * Included by hellforge_modloader.c after difficulty_world_towns.c. */

/* A vanilla city's gate radius: S x S+0x50 / 2 on a vanilla-size level of the difficulty the extra large map holds (11,
 * Hell Forge's map_scale 2). b78a (mission 185 type 50, the city run's own mission, Hell Forge D11, extra large maps
 * off): "settings+0x50 0.646", "playable 662 m (factor 1.70)", 544 city blocks and 2308 buildings in its one city, so
 * 1024 x 0.646 = 661.5 m (the three digits: 661.0..662.0 m). SetupMissionDistance (0xF41DC2: the row's factor x 0.38 at
 * 0x23C69D0, then mission modifiers 0xF41A54..0xF41B23) builds it, so the loader cannot rebuild it from its own state.
 * The earlier 330.75 m was a D10 level's (s74s: 0.323, 339 blocks), half a D11 city's radius: s75y's three cities held
 * 775 blocks and 2795 buildings together, under half of one D11 city each. */
#define CITY_GATE_RADIUS 661.5f

enum { GATE_EDGES, GATE_POINTS, GATE_BUILDINGS, GATE_WALLS, GATE_FLATS, GATE_FLATS2, GATE_FLATS3, GATE_SITES };

static struct
{
    volatile LONG level;                         /* g_world.levels the counts are for */
    volatile LONG passed[GATE_SITES], refused[GATE_SITES], turned[GATE_SITES]; /* turned: refused, the world-centred test passed */
    LONG notedLevel, noted[3 * GATE_SITES];
    int installed;
} g_cityGates = {-1, {0}, {0}, {0}, -1, {0}, 0};

static void CityGateCount(uint32_t site, int pass, int vanillaPass)
{
    if (pass) InterlockedIncrement(&g_cityGates.passed[site]);
    else
    {
        InterlockedIncrement(&g_cityGates.refused[site]);
        if (vanillaPass) InterlockedIncrement(&g_cityGates.turned[site]);
    }
}

/* The stubs' call (MS x64: ecx site, rdx p, xmm2 g, r9 test). p: the edge (x0, y0, x1, y1) or the point (x, y), in
 * generator metres as g_cities. test: for the edges the cell type (its rules' margins at +0x54, each tested by the
 * loop at 0xFB2825..0xFB283E), else the margin itself. Returns g for the comiss. */
static float CityGate(uint32_t site, const float* p, float g, const uint8_t* test)
{
    if (!(g_world.current > 1.0f) || g_cities.level != g_world.levels || g_cities.count <= 0 || site >= GATE_SITES) return g;
    if (g_cityGates.level != g_world.levels)
    {
        for (int i = 0; i < GATE_SITES; i++) g_cityGates.passed[i] = g_cityGates.refused[i] = g_cityGates.turned[i] = 0;
        g_cityGates.level = g_world.levels;
    }
    float best = 3.4e38f;
    for (int k = 0; k < g_cities.count && k < CITY_COUNT; k++)
    {
        float dx, dy;
        if (site == GATE_EDGES)
        {
            float ex = p[2] - p[0], ey = p[3] - p[1], length = ex * ex + ey * ey;
            float t = length > 0.0f ? ((g_cities.x[k] - p[0]) * ex + (g_cities.y[k] - p[1]) * ey) / length : 0.0f;
            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            dx = p[0] + t * ex - g_cities.x[k]; dy = p[1] + t * ey - g_cities.y[k];
        }
        else { dx = p[0] - g_cities.x[k]; dy = p[1] - g_cities.y[k]; }
        float d = dx * dx + dy * dy;
        if (d < best) best = d;
    }
    float city = sqrtf(best) - CITY_FRAME_K * CITY_GATE_RADIUS;
    if (site == GATE_EDGES)
    {
        uint32_t rules = 0;
        uint64_t list = 0;
        memcpy(&rules, test + 0x18, 4); memcpy(&list, test + 0x10, 8);
        for (uint32_t i = 0; list != 0 && i < rules && i < 256; i++)
        {
            float margin;
            memcpy(&margin, (const uint8_t*)(uintptr_t)list + 0x70ull * i + 0x54, 4);
            CityGateCount(site, !(city > margin), !(g > margin)); /* ja: refused above the margin */
        }
    }
    else
    {
        float margin;
        memcpy(&margin, test, 4);
        if (site == GATE_BUILDINGS) CityGateCount(site, !(city >= margin), !(g >= margin)); /* jae */
        else if (site == GATE_FLATS2 || site == GATE_FLATS3) CityGateCount(site, margin > city, margin > g); /* comiss xmm1, xmm0; jbe */
        else CityGateCount(site, !(city > margin), !(g > margin));
    }
    return city;
}

/* After an op on an extra large level (WorldDispatch): the level's counts, when they changed. */
static void CityGatesNote(uint32_t op)
{
    if (!g_cityGates.installed || g_cityGates.level != g_world.levels) return;
    LONG now[3 * GATE_SITES];
    for (int i = 0; i < GATE_SITES; i++) { now[3 * i] = g_cityGates.passed[i]; now[3 * i + 1] = g_cityGates.refused[i]; now[3 * i + 2] = g_cityGates.turned[i]; }
    if (g_cityGates.notedLevel == g_cityGates.level && memcmp(now, g_cityGates.noted, sizeof now) == 0) return;
    g_cityGates.notedLevel = g_cityGates.level;
    memcpy(g_cityGates.noted, now, sizeof now);
    Note("difficulty: extra large maps: city gates (city-relative, radius %.0f m) after op 0x%X: 0xAB edge rules %ld passed %ld refused (%ld the world-centred test passed); "
        "0xAB points %ld / %ld (%ld); 0xA7 buildings %ld / %ld (%ld); 0xAF walls %ld / %ld (%ld); 0xA8 flats %ld / %ld (%ld), stage 2 %ld / %ld (%ld), "
        "stage 3 %ld / %ld (%ld)",
        (double)(CITY_FRAME_K * CITY_GATE_RADIUS), op, now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9], now[10], now[11],
        now[12], now[13], now[14], now[15], now[16], now[17], now[18], now[19], now[20]);
}

#define GATE_IN STUB_PUSH_VOLATILE, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, STUB_SAVE_XMM   /* 54 bytes */
#define GATE_OUT STUB_LOAD_XMM, 0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00, STUB_POP_VOLATILE  /* 54 bytes */

/* 0xFB281F (6 bytes, back 0xFB2825): op 0xAB's per-edge g. */
static const uint8_t kGateEdgeStub[] = {
    0x33, 0xF6,                                                       /*   0 xor esi, esi (the rule loop's index) */
    0xF3, 0x0F, 0x5C, 0xFE,                                           /*   2 subss xmm7, xmm6 (the game's g) */
    GATE_IN,                                                          /*   6 push rax rcx rdx r8 r9 r10 r11; sub rsp, 0x88; xmm0..5 */
    0xB9, GATE_EDGES, 0x00, 0x00, 0x00,                               /*  60 mov ecx, GATE_EDGES */
    0x4C, 0x89, 0xFA,                                                 /*  65 mov rdx, r15 (the edge) */
    0x0F, 0x28, 0xD7,                                                 /*  68 movaps xmm2, xmm7 */
    0x49, 0x89, 0xE9,                                                 /*  71 mov r9, rbp (the cell type) */
    STUB_CALL_IMM64,                                                  /*  74 mov rax, CityGate (imm64 at 76); call rax */
    0xF3, 0x0F, 0x10, 0xF8,                                           /*  86 movss xmm7, xmm0 */
    GATE_OUT,                                                         /*  90 xmm0..5; add rsp, 0x88; pop r11 r10 r9 r8 rdx rcx rax */
    0xE9, 0, 0, 0, 0};                                                /* 144 jmp 0xFB2825 (rel32 at 145) */
_Static_assert(sizeof kGateEdgeStub == 149, "the edge gate stub's offsets");
/* 0xFB2155 (5 bytes, back 0xFB215A): op 0xAB's per-point g. */
static const uint8_t kGatePointStub[] = {
    0xF3, 0x41, 0x0F, 0x5C, 0xC0,                                     /*   0 subss xmm0, xmm8 (the game's g) */
    GATE_IN,                                                          /*   5 */
    0xB9, GATE_POINTS, 0x00, 0x00, 0x00,                              /*  59 mov ecx, GATE_POINTS */
    0x48, 0x8D, 0x94, 0x24, 0xF0, 0x00, 0x00, 0x00,                   /*  64 lea rdx, [rsp+0xf0] (game [rsp+0x30]: the point) */
    0x0F, 0x28, 0xD0,                                                 /*  72 movaps xmm2, xmm0 */
    0x4D, 0x8D, 0x4D, 0x54,                                           /*  75 lea r9, [r13+0x54] (the margin) */
    STUB_CALL_IMM64,                                                  /*  79 mov rax, CityGate (imm64 at 81); call rax */
    0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20,                               /*  91 movss [rsp+0x20], xmm0 (the saved xmm0's low lane) */
    GATE_OUT,                                                         /*  97 */
    0xE9, 0, 0, 0, 0};                                                /* 151 jmp 0xFB215A (rel32 at 152) */
_Static_assert(sizeof kGatePointStub == 156, "the point gate stub's offsets");
/* 0xF995DE (5 bytes, back 0xF995E3): op 0xA7's per-building g. */
static const uint8_t kGateBuildingStub[] = {
    0xF3, 0x41, 0x0F, 0x5C, 0xC0,                                     /*   0 subss xmm0, xmm8 (the game's g) */
    GATE_IN,                                                          /*   5 */
    0xB9, GATE_BUILDINGS, 0x00, 0x00, 0x00,                           /*  59 mov ecx, GATE_BUILDINGS */
    0x48, 0x8D, 0x94, 0x24, 0x10, 0x01, 0x00, 0x00,                   /*  64 lea rdx, [rsp+0x110] (game [rsp+0x50]: the point) */
    0x0F, 0x28, 0xD0,                                                 /*  72 movaps xmm2, xmm0 */
    0x4D, 0x8D, 0x4F, 0x54,                                           /*  75 lea r9, [r15+0x54] (the margin) */
    STUB_CALL_IMM64,                                                  /*  79 mov rax, CityGate (imm64 at 81); call rax */
    0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20,                               /*  91 movss [rsp+0x20], xmm0 */
    GATE_OUT,                                                         /*  97 */
    0xE9, 0, 0, 0, 0};                                                /* 151 jmp 0xF995E3 (rel32 at 152) */
_Static_assert(sizeof kGateBuildingStub == 156, "the building gate stub's offsets");
/* 0xFA401E (8 bytes, back 0xFA4026): op 0xAF's per-point g and its comiss. */
static const uint8_t kGateWallStub[] = {
    0xF3, 0x0F, 0x5C, 0xC6,                                           /*   0 subss xmm0, xmm6 (the game's g) */
    GATE_IN,                                                          /*   4 */
    0xB9, GATE_WALLS, 0x00, 0x00, 0x00,                               /*  58 mov ecx, GATE_WALLS */
    0x48, 0x8D, 0x94, 0x24, 0x98, 0x01, 0x00, 0x00,                   /*  63 lea rdx, [rsp+0x198] (game [rsp+0xd8]: the point) */
    0x0F, 0x28, 0xD0,                                                 /*  71 movaps xmm2, xmm0 */
    0x4C, 0x8D, 0x4E, 0x54,                                           /*  74 lea r9, [rsi+0x54] (the margin) */
    STUB_CALL_IMM64,                                                  /*  78 mov rax, CityGate (imm64 at 80); call rax */
    0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20,                               /*  90 movss [rsp+0x20], xmm0 */
    GATE_OUT,                                                         /*  96 */
    0x0F, 0x2F, 0x46, 0x54,                                           /* 150 comiss xmm0, [rsi+0x54] (the replaced test) */
    0xE9, 0, 0, 0, 0};                                                /* 154 jmp 0xFA4026 (the ja; rel32 at 155) */
_Static_assert(sizeof kGateWallStub == 159, "the wall gate stub's offsets");
/* 0xFB0BD8 (11 bytes, back 0xFB0BE3): op 0xA8's first stage, per-point g and its comiss. */
static const uint8_t kGateFlatStub[] = {
    0xF3, 0x0F, 0x5C, 0xC7,                                           /*   0 subss xmm0, xmm7 (the game's g) */
    GATE_IN,                                                          /*   4 */
    0xB9, GATE_FLATS, 0x00, 0x00, 0x00,                               /*  58 mov ecx, GATE_FLATS */
    0x48, 0x8D, 0x55, 0x08,                                           /*  63 lea rdx, [rbp+0x8] (the point) */
    0x0F, 0x28, 0xD0,                                                 /*  67 movaps xmm2, xmm0 */
    0x4C, 0x8D, 0x8F, 0x80, 0x02, 0x00, 0x00,                         /*  70 lea r9, [rdi+0x280] (the margin) */
    STUB_CALL_IMM64,                                                  /*  77 mov rax, CityGate (imm64 at 79); call rax */
    0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20,                               /*  89 movss [rsp+0x20], xmm0 */
    GATE_OUT,                                                         /*  95 */
    0x0F, 0x2F, 0x87, 0x80, 0x02, 0x00, 0x00,                         /* 149 comiss xmm0, [rdi+0x280] (the replaced test) */
    0xE9, 0, 0, 0, 0};                                                /* 156 jmp 0xFB0BE3 (the ja; rel32 at 157) */
_Static_assert(sizeof kGateFlatStub == 161, "the flat gate stub's offsets");
/* 0xFB14C0 (7 bytes, back 0xFB14C7): op 0xA8's second stage, per-point g and its comiss. */
static const uint8_t kGateFlat2Stub[] = {
    0xF3, 0x0F, 0x5C, 0xC6,                                           /*   0 subss xmm0, xmm6 (the game's g) */
    GATE_IN,                                                          /*   4 */
    0xB9, GATE_FLATS2, 0x00, 0x00, 0x00,                              /*  58 mov ecx, GATE_FLATS2 */
    0x48, 0x8D, 0x55, 0xD0,                                           /*  63 lea rdx, [rbp-0x30] (the point) */
    0x0F, 0x28, 0xD0,                                                 /*  67 movaps xmm2, xmm0 */
    0x4C, 0x8D, 0x8F, 0x58, 0x02, 0x00, 0x00,                         /*  70 lea r9, [rdi+0x258] (the margin) */
    STUB_CALL_IMM64,                                                  /*  77 mov rax, CityGate (imm64 at 79); call rax */
    0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20,                               /*  89 movss [rsp+0x20], xmm0 */
    GATE_OUT,                                                         /*  95 (xmm1 back: the margin) */
    0x0F, 0x2F, 0xC8,                                                 /* 149 comiss xmm1, xmm0 (the replaced test) */
    0xE9, 0, 0, 0, 0};                                                /* 152 jmp 0xFB14C7 (the jbe; rel32 at 153) */
_Static_assert(sizeof kGateFlat2Stub == 157, "the second flat gate stub's offsets");
/* 0xFB1733 (7 bytes, back 0xFB173A): op 0xA8's third stage, per-point g and its comiss. */
static const uint8_t kGateFlat3Stub[] = {
    0xF3, 0x0F, 0x5C, 0xC6,                                           /*   0 subss xmm0, xmm6 (the game's g) */
    GATE_IN,                                                          /*   4 */
    0xB9, GATE_FLATS3, 0x00, 0x00, 0x00,                              /*  58 mov ecx, GATE_FLATS3 */
    0x48, 0x8D, 0x55, 0xC8,                                           /*  63 lea rdx, [rbp-0x38] (the point) */
    0x0F, 0x28, 0xD0,                                                 /*  67 movaps xmm2, xmm0 */
    0x4C, 0x8D, 0x8F, 0x58, 0x02, 0x00, 0x00,                         /*  70 lea r9, [rdi+0x258] (the margin) */
    STUB_CALL_IMM64,                                                  /*  77 mov rax, CityGate (imm64 at 79); call rax */
    0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20,                               /*  89 movss [rsp+0x20], xmm0 */
    GATE_OUT,                                                         /*  95 */
    0x0F, 0x2F, 0xC8,                                                 /* 149 comiss xmm1, xmm0 (the replaced test) */
    0xE9, 0, 0, 0, 0};                                                /* 152 jmp 0xFB173A (the jbe; rel32 at 153) */
_Static_assert(sizeof kGateFlat3Stub == 157, "the third flat gate stub's offsets");

/* Each site: where it jumps, how many bytes it replaces (their original bytes are the start of its last pin), where
 * it comes back, its stub and the imm64 of its call. */
static const struct { uint32_t site, length, back; const uint8_t* stub; uint32_t size, call; const char* original; } kCityGateSites[GATE_SITES] = {
    {0xFB281Fu, 6, 0xFB2825u, kGateEdgeStub, sizeof kGateEdgeStub, 76, "33 F6 F3 0F 5C FE"},
    {0xFB2155u, 5, 0xFB215Au, kGatePointStub, sizeof kGatePointStub, 81, "F3 41 0F 5C C0"},
    {0xF995DEu, 5, 0xF995E3u, kGateBuildingStub, sizeof kGateBuildingStub, 81, "F3 41 0F 5C C0"},
    {0xFA401Eu, 8, 0xFA4026u, kGateWallStub, sizeof kGateWallStub, 80, "F3 0F 5C C6 0F 2F 46 54"},
    {0xFB0BD8u, 11, 0xFB0BE3u, kGateFlatStub, sizeof kGateFlatStub, 79, "F3 0F 5C C7 0F 2F 87 80 02 00 00"},
    {0xFB14C0u, 7, 0xFB14C7u, kGateFlat2Stub, sizeof kGateFlat2Stub, 79, "F3 0F 5C C6 0F 2F C8"},
    {0xFB1733u, 7, 0xFB173Au, kGateFlat3Stub, sizeof kGateFlat3Stub, 79, "F3 0F 5C C6 0F 2F C8"}};

/* The reviewed code each stub relies on: the inputs' loads, the g computation, the patched bytes and the test after. */
static const struct { uint32_t rva; const char* pin; } kCityGatePins[] = {
    /* 0xAB: the edge r15 (+0/+4/+8/+0xC), the cell type rbp; its nearest point to (S/2, S/2); g into xmm7; the rule loop */
    {0xFB26A0u, "4B 8D 0C E4 33 ED F3 0F 10 5C CF 08 4C 8D 3C CF F3 41 0F 10 2F 4D 8D 47 20 F3 41 0F 10 67 0C 45 33 C9 F3 41 0F 10 77 04 "
                "49 8B 08 48 85 C9 74 41 49 2B CB 48 8B C2 48 F7 E9 48 C1 FA 07 48 8B C2 48 C1 E8 3F 48 03 C2 8B C0 48 C1 E0 05 48 63 4C 18 "
                "0C 49 8B 45 18 48 8B 2C C8 F6 85 70 01 00 00 02 0F 84 12 03 00 00 48 BA 79 78 78 78 78 78 78 78 41 FF C1 49 83 C0 08 41 83 "
                "F9 02 72 AA 48 85 ED 0F 84 F2 02 00 00"},
    {0xFB2727u, "F3 0F 5C DD 41 0F 28 D4 F3 41 0F 59 D2 F3 0F 5C E6 0F 28 C2 F3 0F 5C D6 F3 0F 5C C5 0F 28 CC F3 0F 59 CC F3 0F 59 D4 F3 "
                "0F 59 C3 F3 0F 58 D0 0F 28 C3 F3 0F 59 C3 F3 0F 58 C8 F3 0F 5E D1 44 0F 2F C2 76 05 49 8B 07 EB 36 41 0F 2F D3 76 06 49 8B "
                "47 08 EB 2A F3 0F 59 DA F3 0F 59 E2 F3 0F 58 DD F3 0F 58 E6 F3 0F 11 9C 24 98 00 00 00 F3 0F 11 A4 24 9C 00 00 00 48 8B 84 "
                "24 98 00 00 00"},
    {0xFB27A6u, "49 8B 4A 08 0F 57 C9 48 89 84 24 A0 00 00 00 F3 0F 10 84 24 A0 00 00 00 F3 0F 10 94 24 A4 00 00 00 8B 41 2C F3 48 0F 2A "
                "C8 0F 28 F1 F3 41 0F 59 CA F3 0F 59 71 50 F3 0F 5C D1 F3 0F 5C C1 F3 41 0F 59 F2 F3 0F 59 D2 F3 0F 59 C0 F3 0F 58 D0 41 0F "
                "2E D0 7A 08 75 06 41 0F 28 F8 EB 1C 0F 57 C0 0F 2E C2 77 09 0F 57 FF F3 0F 51 FA EB 0B 0F 28 C2 E8 98 A8 15 01 0F 28 F8 33 "
                "F6 F3 0F 5C FE 39 75 18 0F 86 EB 01 00 00 66 90 8B C6 48 6B F8 70 48 03 7D 10 0F 2F 7F 54 0F 87 9E 00 00 00 F6 47 24 02"},
    /* 0xFB1DB0: the point (xmm13, xmm14 -> [rsp+0x30]); g into xmm0; comiss [r13+0x54]; ja */
    {0xFB20AFu, "49 8B 4C 24 08 F3 45 0F 5C E6 0F 57 C9 8B 41 2C F3 44 0F 59 DA F3 44 0F 59 E2 F3 48 0F 2A C8 F3 41 0F 58 E3 F3 41 0F 58 "
                "DC 44 0F 28 C1 F3 41 0F 59 CA F3 44 0F 59 41 50 F3 44 0F 58 EC F3 44 0F 58 F3 F3 45 0F 59 C2 F3 44 0F 11 6C 24 68 F3 44 0F "
                "11 74 24 6C 48 8B 5C 24 68 48 89 5C 24 30 F3 0F 10 54 24 30 F3 0F 10 44 24 34 F3 0F 5C D1 F3 0F 5C C1 F3 0F 59 D2 F3 0F 59 "
                "C0 F3 0F 58 D0 41 0F 2E D1 7A 08 75 06 41 0F 28 C1 EB 19 0F 57 C0 0F 2E C2 77 09 0F 57 C0 F3 0F 51 C2 EB 08 0F 28 C2 E8 5F "
                "AF 15 01 F3 41 0F 5C C0 41 0F 2F 45 54 0F 87 EC 01 00 00"},
    /* 0xF993E0: the point [rsp+0x50] / [rsp+0x54]; g into xmm0; comiss [r15+0x54]; jae */
    {0xF9954Fu, "F3 0F 11 7C 24 50 F3 0F 58 C6 F3 41 0F 59 C2 E8 C5 F9 16 01 F3 44 0F 10 1D 64 D5 42 01 0F 28 C8 4C 8B 74 24 38 0F 57 C0 "
                "F3 41 0F 5E CA 49 8B 4E 08 8B 41 2C 0F 57 F6 F3 0F 11 4C 24 54 F3 48 0F 2A C0 44 0F 28 C0 F3 41 0F 59 C3 F3 44 0F 59 41 50 "
                "F3 0F 5C C8 F3 0F 5C F8 F3 45 0F 59 C3 F3 0F 59 C9 F3 0F 59 FF F3 0F 58 CF 0F 2E CE 7A 07 75 05 0F 57 C0 EB 19 0F 57 C0 0F "
                "2E C1 77 09 0F 57 C0 F3 0F 51 C1 EB 08 0F 28 C1 E8 D6 3A 17 01 F3 41 0F 5C C0 41 0F 2F 47 54 0F 83 C9 04 00 00"},
    /* 0xFA39D0: the point [rsp+0xd8] / [rsp+0xdc]; g into xmm0; comiss [rsi+0x54]; ja */
    {0xFA3F9Eu, "48 8B 54 24 50 0F 57 C9 F3 0F 10 BC 24 D8 00 00 00 F3 44 0F 10 84 24 DC 00 00 00 0F 28 C7 41 0F 28 D0 48 8B 4A 08 8B 41 "
                "2C F3 48 0F 2A C8 0F 28 F1 F3 41 0F 59 CE F3 0F 59 71 50 F3 0F 5C D1 F3 0F 5C C1 F3 41 0F 59 F6 F3 0F 59 D2 F3 0F 59 C0 F3 "
                "0F 58 D0 41 0F 2E D3 7A 08 75 06 41 0F 28 C3 EB 1E 0F 57 C0 0F 2E C2 77 09 0F 57 C0 F3 0F 51 C2 EB 0D 0F 28 C2 E8 9B 90 16 "
                "01 48 8B 54 24 50 F3 0F 5C C6 0F 2F 46 54 0F 87 9E 03 00 00"},
    /* 0xFAF130 (first stage): the point [rbp+0x8] / [rbp+0xc]; g into xmm0; comiss [rdi+0x280]; ja */
    {0xFB0B56u, "F3 0F 11 AD 5C 01 00 00 F3 0F 11 A5 58 01 00 00 48 8B 85 58 01 00 00 48 89 45 08 8B 41 2C F3 0F 10 55 08 F3 0F 10 45 0C "
                "F3 48 0F 2A C8 0F 28 F9 F3 0F 59 0D 42 5F 41 01 F3 0F 59 79 50 F3 0F 5C D1 F3 0F 5C C1 F3 0F 59 3D 2D 5F 41 01 F3 0F 59 D2 "
                "F3 0F 59 C0 F3 0F 58 D0 41 0F 2E D0 7A 08 75 06 41 0F 28 C0 EB 1B 0F 57 C0 0F 2E C2 77 09 0F 57 C0 F3 0F 51 C2 EB 0A 0F 28 "
                "C2 E8 DE C4 15 01 33 D2 F3 0F 5C C7 0F 2F 87 80 02 00 00 0F 87 64 02 00 00"},
    /* its second stage: the point [rbp-0x30] / [rbp-0x2c]; g into xmm0; the margin [rdi+0x258] in xmm1; comiss xmm1, xmm0; jbe */
    {0xFB13EDu, "F3 0F 10 8F 64 02 00 00 33 C0 49 8B 4D 08 44 0F 28 D9 F3 0F 10 3D C9 56 41 01 0F 28 C1 F3 0F 59 45 F0 66 89 45 80 F3 0F "
                "59 4D E4 8B 41 2C F3 0F 10 55 90 F3 0F 10 5D 20 F3 0F 58 D0 F3 0F 58 D9 F3 45 0F 5E DA F3 44 0F 10 15 35 59 41 01 F3 0F 11 "
                "55 D0 F3 45 0F 5C D3 F3 0F 11 5D D4 41 0F 28 CB F3 0F 59 4D A0 41 0F 28 C2 F3 0F 59 45 84 F3 0F 58 C8 0F 57 C0 F3 48 0F 2A "
                "C0 F3 0F 11 4D 84 0F 28 F0 F3 0F 59 C7 F3 0F 59 71 50 F3 0F 5C D0 F3 0F 5C D8 F3 0F 59 F7 F3 0F 59 D2 F3 0F 59 DB F3 0F 58 "
                "D3 41 0F 2E D0 7A 08 75 06 41 0F 28 C0 EB 19 0F 57 C0 0F 2E C2 77 09 0F 57 C0 F3 0F 51 C2 EB 08 0F 28 C2 E8 FC BB 15 01 F3 "
                "0F 10 8F 58 02 00 00 F3 0F 5C C6 0F 2F C8 0F 86 BA 01 00 00"},
    /* its third stage: the point [rbp-0x38] / [rbp-0x34]; g into xmm0; the margin [rdi+0x258] in xmm1; comiss xmm1, xmm0; jbe */
    {0xFB168Fu, "F3 0F 10 8F 64 02 00 00 49 8B 4D 08 0F 28 C1 F3 0F 59 45 F0 F3 0F 10 55 24 8B 41 2C F3 0F 59 4D E4 F3 0F 5C D0 F3 0F 10 "
                "5D 28 0F 57 C0 F3 44 0F 59 5D 84 F3 44 0F 59 55 A0 F3 0F 5C D9 F3 0F 11 55 C8 F3 48 0F 2A C0 F3 0F 11 5D CC F3 45 0F 58 DA "
                "0F 28 F0 F3 0F 59 C7 F3 0F 59 71 50 F3 0F 5C D0 F3 0F 5C D8 F3 0F 59 F7 F3 0F 59 D2 F3 0F 59 DB F3 0F 58 D3 41 0F 2E D0 7A "
                "08 75 06 41 0F 28 C0 EB 19 0F 57 C0 0F 2E C2 77 09 0F 57 C0 F3 0F 51 C2 EB 08 0F 28 C2 E8 89 B9 15 01 F3 0F 10 8F 58 02 00 "
                "00 F3 0F 5C C6 0F 2F C8 0F 86 4C 01 00 00"}};

/* Installed by InstallWorld (an extra large world): pins, stubs, then the seven jumps (put back if one fails). A refusal
 * is noted and leaves the game's world-centred gates. */
#define GATE_BLOCK 0x600u /* GATE_SITES stubs of up to 0xC0 bytes */
_Static_assert(GATE_SITES * 0xC0 <= GATE_BLOCK, "the city gates' stub block");
static const char* InstallCityGates(uintptr_t game)
{
    char off[8] = "";
    if (GetEnvironmentVariableA("HELLFORGE_XL_CITY_GATES", off, sizeof off) > 0 && !_stricmp(off, "off"))
        return "city gates about each city are off (HELLFORGE_XL_CITY_GATES=off); the cities keep the world-centred gates";
    for (size_t i = 0; i < sizeof kCityGatePins / sizeof kCityGatePins[0]; i++)
        if (!ApiCodeMatches(game + kCityGatePins[i].rva, kCityGatePins[i].pin))
            return "the city ops' distance gates are not the reviewed ones; the cities keep the world-centred gates";
    for (int i = 0; i < GATE_SITES; i++)
        if (!ApiCodeMatches(game + kCityGateSites[i].site, kCityGateSites[i].original))
            return "the city ops' distance gates are not the reviewed ones; the cities keep the world-centred gates";
    uint8_t* block = AllocateNearGame(game, GATE_BLOCK);
    if (block == NULL) return "no memory within 2 GB of game.dll for the city gates' stubs";
    void* helper = (void*)CityGate;
    for (int i = 0; i < GATE_SITES; i++)
    {
        uint8_t* stub = block + 0xC0 * i;
        memcpy(stub, kCityGateSites[i].stub, kCityGateSites[i].size);
        if (stub[kCityGateSites[i].call - 2] != 0x48 || stub[kCityGateSites[i].call - 1] != 0xB8) return "a city gate stub's layout is wrong";
        memcpy(stub + kCityGateSites[i].call, &helper, 8);
        int64_t distance = (int64_t)(game + kCityGateSites[i].back) - (int64_t)(uintptr_t)(stub + kCityGateSites[i].size);
        if (!InRel32(distance)) return "a city gate stub is out of reach of game.dll";
        int32_t rel = (int32_t)distance;
        memcpy(stub + kCityGateSites[i].size - 4, &rel, 4);
    }
    DWORD protect;
    VirtualProtect(block, GATE_BLOCK, PAGE_EXECUTE_READ, &protect);
    FlushInstructionCache(GetCurrentProcess(), block, GATE_BLOCK);
    for (int i = 0; i < GATE_SITES; i++)
    {
        uint8_t patch[16];
        memset(patch, 0x90, sizeof patch);
        patch[0] = 0xE9;
        int32_t rel = DifficultyJumpRel(game + kCityGateSites[i].site, (uintptr_t)(block + 0xC0 * i)); memcpy(patch + 1, &rel, 4);
        if (ApiWrite(game + kCityGateSites[i].site, patch, kCityGateSites[i].length))
        {
            for (int k = 0; k < i; k++) /* put back the ones already written */
            {
                uint8_t bytes[16]; int n = 0;
                for (const char* p = kCityGateSites[k].original; *p; ) { while (*p == ' ') p++; if (!*p) break; bytes[n++] = (uint8_t)strtoul(p, (char**)&p, 16); }
                ApiWrite(game + kCityGateSites[k].site, bytes, (size_t)n);
            }
            return "the city gates' jumps could not be written; the cities keep the world-centred gates";
        }
    }
    g_cityGates.installed = 1;
    return NULL;
}
