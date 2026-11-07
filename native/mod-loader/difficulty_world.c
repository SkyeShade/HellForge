/* Level 11's extra large maps (difficulty.c, a map_scale past what the 2048 m world holds): the world itself grows.
 * Included by hellforge_modloader.c after difficulty_maps.c.
 *
 * Level init (0xF71220) sets the level's size (lg+0x1E0, also +0xB18) from a table per pipeline mode (0x2150008: 2048 for
 * all nine) and keeps the generator's radii as fractions of half of it (lg+0x2D78, +0x2D7C; difficulty_maps.c), so the
 * generator follows a larger size. Much of the game does not: 208 instructions read the size from shared constants
 * (world_sites.h: half 1024.0, 2048.0 and 1 / 2048, the world <-> map space conversions of the locations, zones, routes,
 * minimap and map screen), 11 hold it as an immediate (the map space clamps, the world size passed to the engine at
 * 0xF5DEF9 and 0xF5F3C1, the stamp index's world, the location spreading's centre), and the director refuses spawns past
 * +-2000 m on any axis (0x95363C). For an extra large map the world is 2048 m x a power of two (the scale), large enough for
 * its disk; every one of those reads follows it. They follow the level being set up: level init calls WorldLevel before it
 * stores the size, which writes the constants (a block of the loader's, the instructions' displacements pointed at it)
 * and the immediates for that level, the scale at level 11 and 1 (vanilla) at every other, and the ship's navigation
 * setup puts back 1. A war-table preview is a level too, so the map last set up wins.
 *
 * A prototype ("break it and see", research complex-operations-feasibility): the generator's arrays, the location table
 * and the spacing follow the map_scale as for a 2 x map, the terrain's own resolution (lg+0xB1C, 512) is left, and the
 * engine's own limits (terrain, physics, navmesh) are what the first runs measure. */
#include "world_sites.h"

#define DIF_WORLD_MAP_SCALE 2.0f      /* a map_scale above this needs a world larger than 2048 m (its disk passes 1022 m) */
#define DIF_WORLD_SIZE_SITE 0xF714E8u /* movss [r13+0x1e0], xmm0 (r13 = lg; the descriptor is decoded, lg+0xF9 the level) */
#define DIF_WORLD_SIZE_BACK 0xF714F1u
static volatile uint32_t* g_paintedPeak; /* op 0x7C's painted route-cell list peak (InstallRouteCellLists) */
static const uint8_t kWorldSizePin[9] = {0xF3, 0x41, 0x0F, 0x11, 0x85, 0xE0, 0x01, 0x00, 0x00};
/* The director's spawn gate: movss xmm0, [+2000.0] and movss xmm1, [-2000.0], each compared with x, y and z. */
static const struct { uint32_t rva, constant; } kWorldGate[2] = {{0x953642u, 0x23C7DD0u}, {0x953653u, 0x23C83CCu}};
/* Drop bounds: two box clamps of a world position to vanilla's +-999 m (1024 less 25; movss xmm, [-999.0 0x23C83B8] /
 * [999.0 0x23C7D38], then comiss/minss on x and y), each after the same ground query (0x13CAA80, hash 0xF3B52DB1): the
 * Helldiver's pod target in the spawn sequence 0xA866F0 (0xA87BB2 / 0xA87BBD), and the helper at 0x827500 (0x8275EE /
 * 0x8275FF). Unredirected, a drop at (4257, 4823) on a 16 km map homed onto (999, 999) mid-fall (run i14). */
static const struct { uint32_t rva, constant; uint8_t high; } kWorldDropBounds[4] = {
    {0xA87BB2u, 0x23C83B8u, 0}, {0xA87BBDu, 0x23C7D38u, 1}, {0x8275EEu, 0x23C83B8u, 0}, {0x8275FFu, 0x23C7D38u, 1}};
/* The immediates: {rva, offset of the 32-bit value, vanilla value}: 2048.0 (map space clamps 0xF38EDA/0xF38EFE, the
 * engine's world size 0xF5DEF9.. and 0xF5F3C1.., the stamp index's world 0xF6E9D0), 1024.0 (the centre that
 * SpreadLocations and the outposts' spreading measure from, 0xF7DFEF.. and 0xF7EDDC..) and 2046.0 (the water physics
 * heightfield's extents that GenerateWaterFinalize builds from the 1024^2 water_height readback, 0xF6CD2E -> +0x128 and
 * the qword at 0xF6CD3C -> +0x12C, its high half 0: 2048 - 2048 / 1024, so the world's size - one texel; vanilla kept
 * the whole map's water collision in the central 2 km). */
#define WORLD_IMMEDIATES 13
static const struct { uint32_t rva; uint8_t at; uint32_t value; } kWorldImmediates[WORLD_IMMEDIATES] = {
    {0xF38EDAu, 4, 0x45000000u}, {0xF38EFEu, 4, 0x45000000u}, {0xF5DEF9u, 4, 0x45000000u}, {0xF5DF06u, 4, 0x45000000u},
    {0xF5F3C1u, 4, 0x45000000u}, {0xF5F3CEu, 4, 0x45000000u}, {0xF6E9D0u, 2, 0x45000000u},
    {0xF7DFEFu, 3, 0x44800000u}, {0xF7DFF6u, 3, 0x44800000u}, {0xF7EDDCu, 3, 0x44800000u}, {0xF7EDE3u, 3, 0x44800000u},
    {0xF6CD2Eu, 6, 0x44FFC000u}, {0xF6CD3Cu, 7, 0x44FFC000u}};
/* What the instructions read (the block's second page, writable): half, full, 1 / full, the gate's +-2000 x scale, and
 * the size the stub stores (0: the table's). */
/* maskSpan: 4096.0 (0x23C7E04) where a stratagem's payload clearance is read from the clearing mask (0x6AA0CA; the
 * test 0x6AA0B2..0x6AA100 refuses the beacon when the largest payload radius [payload+0x24] is more): clearance = 2 (s - 0.5) x (4096 / N) x 32 m, s the mask's sample at the beacon
 * through 0x6AA04F..0x6AA080 (kWorldSites: over the whole world), N its width ([G+0x11A0008], 512 from 0xF5E9BB); the
 * division by N puts the encoded distance in texels, 4096 / N = 2 x the world's 2048 m / N, so a texel of a larger world
 * is that much wider: at the vanilla 4096 a payload's clearance came out 8 x too small on a 16 km map and a vehicle
 * (its radius against it) was refused at once. */
/* quarter: 512.0 (0x23C7B88) where SpawnEarthquakes (op 0x4D, 0xF5CE70) bounds a source (0xF5CEE0, compared at 0xF5CFEB and
 * 0xF5D004): it keeps a source when |x - half| and |y - half| are at most 512, the centre and the 1 / full it samples the
 * heightmap with both redirected (kWorldSites 0xF5CF04, 0xF5CEF2), so the bound is the world's central half; at the vanilla
 * 512 a 16 km map kept only the sources in its central kilometre. */
/* goalHigh / goalLow: +-4096.0 (0x23C7E04, 0x23C83DC) where an enemy's movement goal is applied (0x7ED4C0, loads 0x7ED57B and
 * 0x7ED58C): the AiEnemy state machine (0xAA9B20: combat 0xAA42D0, retreat 0xAA80E0, charge 0xAA8680 ...) hands it the goal
 * every update and it returns without setting it when |x|, |y| or |z| is past 4096 m (the jump to 0x7EDB3A), 4 x the
 * vanilla half world; on a 16 km map an enemy whose goal lay more than 4 km out on either axis never moved. The only
 * reader of -4096. */
/* motionHigh / motionLow: +-1900.0 (0x23C7DC8, 0x23C83C8) in the unit motion step 0x59FED0: after every step it compares
 * the new position's x, y and z with both (loads 0x5A01AF and 0x5A0199, compares 0x5A01A6..0x5A01E8, the only readers)
 * and REMOVES the entity past either (0xFDC310 at 0x5A184A): vanilla's 2048 m world plus a 876 m margin. On a 16 km map
 * every motion-driven unit (enemies; I: the Helldiver's mover too) more than 1900 m out on any axis was deleted (static,
 * living-world Phase 0, subagent drop). The bound keeps vanilla's margin past the larger world's edge: 1900 + 1024 x
 * (scale - 1), 9068 m at 8 x (also the depth a falling unit is removed at). */
#define FLAT_BAND_K 2.0f /* op 0x3E flatten zones: the falloff, x vanilla's metres, ending at its quad's edge (FlatBandStub) */
#define PAD_BAND_K 1.5f /* op 0x3F rectangle pads: the falloff, x vanilla's metres (PadBandStubs) */
typedef struct WorldValues { float half, full, inverse, gateHigh, gateLow, size, clearing, negHalf, dropHigh, dropLow, negFull, maskSpan, quarter, goalHigh, goalLow, padPx, padMetre, padQuad, flatPx; uint32_t capMul; float motionHigh, motionLow; } WorldValues;
/* GenerateClearingPoints (op 0xB3, 0xF42BD0) scatters Poisson points over the whole world square, keeps each with the
 * planet's chance (its random number x 2^-32, 0xF42D1E, against [settings+0x114]), and appends the kept ones (16 bytes
 * each, from G+0x11A16FC) without a check: room for about 1450 before the route counts at G+0x11A71B4. Their number grows
 * with the world's area; a 16384 m preview overwrote the counts and the resource hash read past the block (0xF26358).
 * An extra large level keeps 1 / scale^2 as many (the random number scaled by scale^2). */
#define DIF_CLEARING_RANDOM 0xF42D1Eu /* movss xmm2, [rip -> 0x23C6538: 2^-32] (8 bytes) */
#define DIF_CLEARING_CONSTANT 0x23C6538u
static const uint8_t kWorldStub[] = {
    STUB_PUSH_VOLATILE,                                               /*   0 push rax rcx rdx r8 r9 r10 r11 */
    0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,                         /*  11 sub rsp, 0x88 */
    STUB_SAVE_XMM,                                                    /*  18 movdqu [rsp+0x20..0x70], xmm0..xmm5 */
    0x4C, 0x89, 0xE9,                                                 /*  54 mov rcx, r13 (the level) */
    STUB_CALL_IMM64,                                                  /*  57 mov rax, WorldLevel (imm64 at 59); call rax */
    STUB_LOAD_XMM,                                                    /*  69 movdqu xmm0..xmm5, [rsp+0x20..0x70] */
    0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,                         /* 105 add rsp, 0x88 */
    STUB_POP_VOLATILE,                                                /* 112 pop r11 r10 r9 r8 rdx rcx rax */
    0x83, 0x3D, 0, 0, 0, 0, 0x00,                                     /* 123 cmp dword [rip+size], 0 (rel32 at 125) */
    0x74, 0x08,                                                       /* 130 je 140 */
    0xF3, 0x0F, 0x10, 0x05, 0, 0, 0, 0,                               /* 132 movss xmm0, [rip+size] (rel32 at 136) */
    0xF3, 0x41, 0x0F, 0x11, 0x85, 0xE0, 0x01, 0x00, 0x00,             /* 140 movss [r13+0x1e0], xmm0 */
    0xE9, 0, 0, 0, 0};                                                /* 149 jmp 0xF714F1 (rel32 at 150) */
static struct
{
    uint32_t scale;                   /* the world at level 11: 2048 m x scale (1: not extra large) */
    float current;                    /* the scale the constants and immediates hold now */
    WorldValues* values;
    volatile uint32_t* below;         /* the thinning's thresholds the stubs read: [0] the jobs', [1] the placement's */
    uint32_t makeBelow, placeBelow;   /* an extra large level's (a vanilla level's are 0xFFFFFFFF: every stamp kept) */
    uintptr_t game;
    volatile LONG levels, switches;
} g_world = {1, 1.0f};

/* The structured maps' city frame on a larger world: the city is a vanilla city's layout scaled by CITY_FRAME_K, at the
 * world's centre (see WorldSiteTemplate below). The zone template's square (0xFB8DD0: E = W x S+0x50 / s at
 * 0xFB8EF4..0xFB8F22) follows it: x CITY_FRAME_K / map_scale, which is vanilla's 2048 x S+0x50 / s scaled by the frame
 * (level 11's S+0x50 is vanilla's x map_scale / world scale, difficulty.c's distance row). 1 on a vanilla world. */
#define CITY_FRAME_K 1.0f
#define CITY_VANILLA_KEEP 681.12f  /* a vanilla level's keep radius (difficulty_maps.c's radii at map_scale 1: disk + 1) */
#define CITY_VANILLA_FADE 620.12f  /* and its fade (border + 57): S+0x54 x W / 2 */
static volatile float* g_cityZone;
#define DIF_PLACE_LIMIT (DIF_GEN_LOCATIONS - 256u) /* op 0x2B places no location past it (kPlaceSkipStub) */

/* The scale level 11's map needs: the smallest power of two whose world holds its disk (difficulty_maps.c's radii). */
static uint32_t WorldScaleFor(float mapScale)
{
    if (mapScale <= DIF_WORLD_MAP_SCALE) return 1; /* up to 2 the disk is cut at 1022 m, as it always was */
    float disk = 389.12f * mapScale + 174.0f + 117.0f;
    uint32_t scale = 1;
    while (scale < 16 && disk > 1024.0f * (float)scale - 2.0f) scale *= 2;
    return scale;
}

/* The network position range (KB net-position-clamp-2000; subagent, checked by me in the dump): every motion position
 * write goes out as game-object field 0x8BBEB160, and the flush (game 0xFDDF0C / 0xFDE0FC: clamp 1) has exe 0x34BAB0
 * clamp the caller's own value, the host's live motion record, to its type's range, [-2000, 2000] on each axis;
 * the send path (0x34F47E..0x34F4D7) quantises each component with the type descriptor's min +0x10, max +0x14 and
 * bits +0xD. On an extra large map enemies past 2 km were pinned to that line (lw87a: 45 of 52 offline points at
 * +-2000), and clients would see every unit there. For an extra large level the range of every type a 0x8BBEB160
 * field uses is widened to +-1024 (scale + 1) m (9216 at 8x: past the motion bound, 1900 + 1024 (scale - 1)); the bits
 * stay, so the wire layout is the same and a step is about 7 cm instead of 1.5 cm. It runs at level init on every
 * peer (WorldLevel), so all peers of an XL mission quantise alike; vanilla's range comes back on a vanilla level.
 * The config: [[exe+0x1A10268]] (stored at exe 0x1CD8C7), object types [cfg+0x78] x [cfg+0x70] (0x34C4C5), 0x50 bytes:
 * +0x18 field count, +0x20 type indices, +0x38 field hashes (0x34C27E..0x34C2BC); descriptors [cfg+0x18], 24 bytes,
 * +0xC kind (3 = vector3). */
static struct { int state; uint32_t type[8]; int count; float low[8], high[8]; } g_netPos; /* state 0 unread, 1 ok, -1 not the reviewed layout */
static void NetPositionRange(float scale)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL), global = 0, cfg = 0, types = 0, descs = 0;
    if (g_netPos.state == 0)
    {
        g_netPos.state = -1;
        if (!ApiCodeMatches(exe + 0x1CD8C7u, "48 89 05 9A 29 84 01") ||
            !ApiCodeMatches(exe + 0x34C4C5u, "48 8B 05 9C 3D 6C 01 45 33 C9 44 8B DA 48 8B 08 44 8B 41 70 48 8B 59 78") ||
            !ApiCodeMatches(exe + 0x34C27Eu, "4C 8D 0C 89 49 C1 E1 04 4C 03 4D 78 45 8B 41 18") ||
            !ApiCodeMatches(exe + 0x34F47Eu, "F3 0F 10 5A 14 F3 0F 10 52 10"))
        { Note("difficulty: extra large maps: the network position type is not the reviewed code; enemies past 2 km stay at the 2000 m line"); return; }
        uint32_t count = 0;
        if (SafeRead(exe + 0x1A10268u, &global, 8) || global == 0 || SafeRead(global, &cfg, 8) || cfg == 0 || SafeRead(cfg + 0x70, &count, 4) ||
            SafeRead(cfg + 0x78, &types, 8) || types == 0 || SafeRead(cfg + 0x18, &descs, 8) || descs == 0 || count == 0 || count > 8192)
        { Note("difficulty: extra large maps: the network config is not readable; enemies past 2 km stay at the 2000 m line"); return; }
        int fields = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            uintptr_t t = types + 0x50u * i, hashes = 0, idx = 0; uint32_t fc = 0;
            if (SafeRead(t + 0x18, &fc, 4) || fc == 0 || fc > 512 || SafeRead(t + 0x38, &hashes, 8) || SafeRead(t + 0x20, &idx, 8) || !hashes || !idx) continue;
            for (uint32_t f = 0; f < fc; f++)
            {
                uint32_t h = 0, ti = 0;
                if (SafeRead(hashes + 4u * f, &h, 4) || h != 0x8BBEB160u || SafeRead(idx + 4u * f, &ti, 4) || ti > 65535) continue;
                fields++;
                int known = 0;
                for (int k = 0; k < g_netPos.count; k++) known |= g_netPos.type[k] == ti;
                if (known || g_netPos.count >= 8) continue;
                uint8_t kind = 0; float lo = 0, hi = 0; uintptr_t d = descs + 24u * ti;
                if (SafeRead(d + 0xC, &kind, 1) || SafeRead(d + 0x10, &lo, 4) || SafeRead(d + 0x14, &hi, 4) || kind != 3 || !(hi > 0) || lo != -hi)
                { Note("difficulty: extra large maps: network position type %u is not a symmetric vector3 (kind %u, %.1f .. %.1f); left as it is", ti, kind, (double)lo, (double)hi); continue; }
                g_netPos.type[g_netPos.count] = ti; g_netPos.low[g_netPos.count] = lo; g_netPos.high[g_netPos.count] = hi; g_netPos.count++;
            }
        }
        if (g_netPos.count == 0) { Note("difficulty: extra large maps: no network position type found (%d fields); enemies past 2 km stay at the 2000 m line", fields); return; }
        g_netPos.state = 1;
        Note("difficulty: extra large maps: %d network position fields use %d type(s), the first %u at %.0f .. %.0f m", fields, g_netPos.count, g_netPos.type[0], (double)g_netPos.low[0], (double)g_netPos.high[0]);
    }
    if (g_netPos.state != 1 || SafeRead(exe + 0x1A10268u, &global, 8) || SafeRead(global, &cfg, 8) || SafeRead(cfg + 0x18, &descs, 8) || descs == 0) return;
    for (int k = 0; k < g_netPos.count; k++)
    {
        float hi = scale > 1.0f ? fmaxf(g_netPos.high[k], 1024.0f * (scale + 1.0f)) : g_netPos.high[k], lo = -hi;
        uintptr_t d = descs + 24u * g_netPos.type[k];
        ApiWrite(d + 0x10, &lo, 4); ApiWrite(d + 0x14, &hi, 4);
    }
    Note("difficulty: extra large maps: network positions range +-%.0f m (%s)", (double)(scale > 1.0f ? fmaxf(g_netPos.high[0], 1024.0f * (scale + 1.0f)) : g_netPos.high[0]),
        scale > 1.0f ? "an extra large level" : "vanilla");
}

static void WorldWrite(float scale)
{
    NetPositionRange(scale);
    WorldValues* v = g_world.values;
    v->half = 1024.0f * scale;
    v->negHalf = -1024.0f * scale;
    v->full = 2048.0f * scale;
    v->inverse = 1.0f / (2048.0f * scale);
    v->gateHigh = 2000.0f * scale;
    v->gateLow = -2000.0f * scale;
    v->clearing = 2.3283064365386963e-10f * scale * scale;
    v->dropHigh = 1024.0f * scale - 25.0f;
    v->dropLow = -(1024.0f * scale - 25.0f);
    v->negFull = -2048.0f * scale;
    v->maskSpan = 4096.0f * scale;
    v->quarter = 512.0f * scale;
    v->goalHigh = 4096.0f * scale;
    v->goalLow = -4096.0f * scale;
    v->motionHigh = 1900.0f + 1024.0f * (scale - 1.0f);
    v->motionLow = -v->motionHigh;
    /* Op 0x3F's rectangle pads (PadBandStubs): band in heightmap pixels, in metres for the quad, and the quad's read
     * back of the stored pixels; vanilla 1 / 1 / 1 */
    v->padPx = scale > 1.0f ? PAD_BAND_K / scale : 1.0f;
    v->padMetre = scale > 1.0f ? PAD_BAND_K / 2.0f : 1.0f;
    v->padQuad = scale > 1.0f ? scale / 2.0f : 1.0f;
    v->flatPx = scale > 1.0f ? FLAT_BAND_K / scale : 1.0f;
    if (g_world.below != NULL)
    {
        g_world.below[0] = scale > 1.0f ? g_world.makeBelow : 0xFFFFFFFFu;
        g_world.below[1] = scale > 1.0f ? g_world.placeBelow : 0xFFFFFFFFu;
        g_world.below[2] = scale > 1.0f ? DIF_PLACE_LIMIT : 0xFFFFFFFFu;
    }
    for (int i = 0; i < WORLD_IMMEDIATES; i++)
    {
        float value;
        memcpy(&value, &kWorldImmediates[i].value, 4);
        value *= scale;
        ApiWrite(g_world.game + kWorldImmediates[i].rva + kWorldImmediates[i].at, &value, 4);
    }
    /* x g_cityZoneRatio: the playable fraction on an extra large city map is 0.95 (difficulty.c), not vanilla's x
     * map_scale / world scale, and the template's square is a share of it */
    if (g_cityZone != NULL) *g_cityZone = scale > 1.0f ? CITY_FRAME_K / g_difficulty.mapScale * g_cityZoneRatio : 1.0f;
    g_world.current = scale;
    InterlockedIncrement(&g_world.switches);
}

/* The generator's stamp index (difficulty_maps.c; P = [lg+0x2C0], +0 its world, +8 the index, +0x48 its memory,
 * +0x50 0x2000 bytes cleared with it) covers (0, 0) .. (world, world) in cells of world / 64 and clamps every key into it.
 * Its world is set when the generation block is made (0xF6E9D0: 2048) and the index is rebuilt from it after every
 * level (0xF6FF60, at 0xF70AA9), so a larger level's stamps all crowded into its edge cells and a placement query read a
 * slot past them (0xFCCBF4, from 0xF5A1C5: a 4096 m preview). Level init gives it the level's world and rebuilds it as
 * the reset does; the memory is the same size, the cells growing with the world. */
#define DIF_INDEX_SIZE 0xC048E0u
#define DIF_INDEX_INIT 0xC04B80u
static const char kIndexSizePin[] = "48 89 5C 24 08 57 48 83 EC 70 0F 29 74 24 60";
static const char kIndexInitPin[] = "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57";
typedef uint32_t (*WorldIndexSize)(float width, float height, float cell);
typedef void (*WorldIndexInit)(uintptr_t index, float minX, float minY, float maxX, float maxY, float cell,
    uint64_t, uint64_t, uint64_t, uint64_t, uintptr_t memory);
static volatile LONG g_worldIndexRebuilt;
static void WorldIndex(uintptr_t level, float world)
{
    uintptr_t p = 0, memory = 0;
    float now = 0.0f;
    if (SafeRead(level + 0x2C0, &p, 8) || p == 0 || SafeRead(p, &now, 4) || now == world || SafeRead(p + 0x48, &memory, 8) || memory == 0)
        return;
    WorldIndexSize size = (WorldIndexSize)(g_world.game + DIF_INDEX_SIZE);
    uint32_t bytes = size(now, now, now / 64.0f);
    if (bytes != size(world, world, world / 64.0f)) return; /* not as reviewed: the index keeps its world */
    memcpy((void*)p, &world, 4);
    memset((void*)memory, 0, bytes);
    memset((void*)(p + 0x50), 0, 0x2000);
    ((WorldIndexInit)(g_world.game + DIF_INDEX_INIT))(p + 8, 0.0f, 0.0f, world, world, world / 64.0f, 0, 0, 0, 0, memory);
    memset((void*)(p + 4), 0, 4);
    if (InterlockedIncrement(&g_worldIndexRebuilt) <= 4) Note("difficulty: extra large maps: the stamp index now covers %.0f m", (double)world);
}

/* The index holds 16383 stamps (difficulty_maps.c); the generator places them by area, so a larger world fills it (a
 * 4096 m mission crashed at 0xF3516C: the inserter 0xC04EF0 returns null when its free list is empty, at 0xC04F22, and
 * none of its 17 callers checks). With an extra large world a full index hands out a scratch slot instead and counts it:
 * that stamp is placed without its overlap entry (later stamps may overlap it). */
#define DIF_INDEX_FULL 0xC04F22u
static const uint8_t kIndexFullPin[8] = {0x48, 0x8B, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00}; /* mov rax, [rsp+0x88] */
static const uint8_t kIndexFullStub[] = {
    0x48, 0x8B, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00,                   /*  0 mov rax, [rsp+0x88] (the handle out) */
    0x48, 0xC7, 0x00, 0x00, 0x00, 0x00, 0x00,                         /*  8 mov qword [rax], 0 */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 15 lock inc dword [rip+count] (rel32 at 18) */
    0x48, 0x8D, 0x05, 0, 0, 0, 0,                                     /* 22 lea rax, [rip+scratch] (rel32 at 25) */
    0x48, 0x83, 0xC4, 0x48,                                           /* 29 add rsp, 0x48 */
    0x41, 0x5E, 0x41, 0x5C, 0xC3};                                    /* 33 pop r14; pop r12; ret */
static volatile LONG* g_worldIndexFull;
/* Content density. PlaceStampWithWeightRule (op 0x2B, 0xF34CC0) places every stamp instance the weight rules chose
 * ([item+0x20], 0x130 each, [item+0x54] of them; the loop 0xF35030..0xF35176) as a location: their number grows with the
 * map's area (7415 locations of 8192 at map_scale 4, with 131750 of 262144 physics bodies), so a larger map ran past
 * both. An extra large map keeps 0.8 x (4 / map_scale)^2 of them (all up to map_scale 3.6), chosen by a hash of the
 * instance's number and its item's count: the same on every peer, spread over the whole map. */
/* Before placing, op 0x2B runs 8 jobs (0xF322F0) that write the instances into one buffer from the generation arena
 * (0xF34E89: 0x720000 bytes, 24576 x 0x130), 3072 each (slices of 3 x 1024 at 0xF34F2C, capacity 0xC00 at 0xF34F56),
 * unchecked; the thinning comes after them. A 16384 m preview's jobs wrote past it (0xF34962). An extra large world gets
 * 8 times the buffer: 0x3900000 bytes, slices of 3 x 8192, 0x6000 each. */
static const struct { uint32_t rva; uint8_t length, at, width; uint8_t pin[7]; uint32_t value; } kPlaceBuffer[3] = {
    {0xF34E89u, 5, 1, 4, {0xBA, 0x00, 0x00, 0x72, 0x00}, 0x3900000u},                   /* mov edx, bytes */
    {0xF34F2Cu, 3, 2, 1, {0xC1, 0xE0, 0x0A}, 0x0Du},                                    /* shl eax, 10 -> 13 */
    {0xF34F56u, 7, 3, 4, {0xC7, 0x43, 0xEC, 0x00, 0x0C, 0x00, 0x00}, 0x6000u}};         /* mov [rbx-0x14], capacity */
/* GenerateHiveLordGraphPost (0xF43E80) labels the Hive Lord graph's connected regions: from each unlabelled node a flood
 * fill whose list is searched linearly before every push (0xF4405D) and whose nodes are labelled when popped (0xF44038,
 * [r14+0x10][node] = the region, bpl). The graph covers the world, about a million nodes at 16384 m: hours. With an
 * extra large world a node is labelled when pushed and the search is that label's test (the same regions, linear time). */
#define DIF_HIVE_SEARCH 0xF4405Du
static const uint8_t kHiveSearchPin[49] = {
    0x45, 0x8B, 0x01, 0x33, 0xC0, 0x85, 0xD2, 0x74, 0x20, 0x48, 0x8B, 0xCF, 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x44,
    0x39, 0x01, 0x74, 0x0C, 0xFF, 0xC0, 0x48, 0x83, 0xC1, 0x04, 0x3B, 0xC2, 0x72, 0xF1, 0xEB, 0x05, 0x83, 0xF8, 0xFF, 0x75,
    0x08, 0x8B, 0xC2, 0xFF, 0xC2, 0x44, 0x89, 0x04, 0x87};
static const uint8_t kHiveSearch[49] = {
    0x45, 0x8B, 0x01,                                                 /*  0 mov r8d, [r9] (the neighbour) */
    0x49, 0x8B, 0x46, 0x10,                                           /*  3 mov rax, [r14+0x10] (the labels) */
    0x42, 0x38, 0x2C, 0x00,                                           /*  7 cmp [rax+r8], bpl */
    0x74, 0x24,                                                       /* 11 je 49 (in this region already) */
    0x42, 0x88, 0x2C, 0x00,                                           /* 13 mov [rax+r8], bpl */
    0x8B, 0xC2, 0xFF, 0xC2,                                           /* 17 mov eax, edx; inc edx */
    0x44, 0x89, 0x04, 0x87,                                           /* 21 mov [rdi+rax*4], r8d */
    0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,                   /* 25 nop (8) */
    0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,                   /* 33 nop (8) */
    0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};                  /* 41 nop (8), then 0xF4408E */
/* The jobs test every new instance against all those they made before (0xF31010..0xF31130), so their time grows with the
 * square of the instances: at 16384 m op 0x2B took 140 to 680 s. The thinning is therefore made where a job creates an
 * instance (0xF348E4, rbx is not yet the slot): a hash of its loops' counters ([rbp+0x48], [rbp+0x68], [rbp+0xA4]) and of
 * the instances it has made ([rbp-0x7C]) keeps the share; a dropped one takes the job's own rejection (0xF34899). */
#define DIF_INSTANCE_MAKE 0xF348E4u   /* mov eax, [rbp-0x7c]; mov rsi, [rbp+0xb8] */
#define DIF_INSTANCE_MADE 0xF348EEu
#define DIF_INSTANCE_REJECT 0xF34899u
static const uint8_t kInstanceMakePin[10] = {0x8B, 0x45, 0x84, 0x48, 0x8B, 0xB5, 0xB8, 0x00, 0x00, 0x00};
static const uint8_t kInstanceSkipStub[67] = {
    0x8B, 0x45, 0x48,                                                 /*  0 mov eax, [rbp+0x48] */
    0x69, 0xC0, 0xB1, 0x79, 0x37, 0x9E,                               /*  3 imul eax, eax, 0x9E3779B1 */
    0x03, 0x45, 0x68,                                                 /*  9 add eax, [rbp+0x68] */
    0x69, 0xC0, 0x6B, 0xCA, 0xEB, 0x85,                               /* 12 imul eax, eax, 0x85EBCA6B */
    0x03, 0x85, 0xA4, 0x00, 0x00, 0x00,                               /* 18 add eax, [rbp+0xa4] */
    0x69, 0xC0, 0x35, 0xAE, 0xB2, 0xC2,                               /* 24 imul eax, eax, 0xC2B2AE35 */
    0x03, 0x45, 0x84,                                                 /* 30 add eax, [rbp-0x7c] */
    0x69, 0xC0, 0xB1, 0x79, 0x37, 0x9E,                               /* 33 imul eax, eax, 0x9E3779B1 */
    0x3B, 0x05, 0, 0, 0, 0,                                           /* 39 cmp eax, [rip+makeBelow] (rel32 at 41) */
    0x76, 0x05,                                                       /* 45 jbe 52 */
    0xE9, 0, 0, 0, 0,                                                 /* 47 jmp 0xF34899 (rel32 at 48) */
    0x8B, 0x45, 0x84,                                                 /* 52 mov eax, [rbp-0x7c] */
    0x48, 0x8B, 0xB5, 0xB8, 0x00, 0x00, 0x00,                         /* 55 mov rsi, [rbp+0xb8] */
    0xE9, 0, 0, 0, 0};                                                /* 62 jmp 0xF348EE (rel32 at 63) */
/* A global hash table ([game+0x33267D0]: entries at +0, 8 bytes each, slots +0x8, 0x4000 on a 16384 m preview) has an
 * inlined insert (0x861C20) whose full case writes the entry through null (0x861CB0, reached only when no slot is free;
 * a 16384 m preview ended there after its Hive Lord graph). With an extra large world a full table skips the entry. */
/* The same level object list is walked at 0x5D14F0 too: the handle's object (0x5D151A, the guarded call above) is looked
 * up again ([[game+0x3326308]+0xB8] method +0x10) and the result read unchecked (0x5D1549). A 0 handle there (the
 * guard's answer for a stale one) goes to the walk's next record (0x5D1655) instead. */
#define DIF_OBJECT_WALK 0x5D151Cu    /* mov ebx, eax; mov r9d, 0x2aa21bb6 */
#define DIF_OBJECT_WALK_BACK 0x5D1524u
#define DIF_OBJECT_WALK_NEXT 0x5D1655u
static const uint8_t kObjectWalkPin[8] = {0x8B, 0xD8, 0x41, 0xB9, 0xB6, 0x1B, 0xA2, 0x2A};
static const uint8_t kObjectWalkStub[21] = {
    0x8B, 0xD8,                                                       /*  0 mov ebx, eax */
    0x85, 0xC0,                                                       /*  2 test eax, eax */
    0x0F, 0x84, 0, 0, 0, 0,                                           /*  4 jz 0x5D1655 (rel32 at 6) */
    0x41, 0xB9, 0xB6, 0x1B, 0xA2, 0x2A,                               /* 10 mov r9d, 0x2aa21bb6 */
    0xE9, 0, 0, 0, 0};                                                /* 16 jmp 0x5D1524 (rel32 at 17) */
/* The same per-entity pass (0x1389E44, the caller of that insert) has another inlined insert into its own table
 * ([r14+0x20000], 0x4000 slots; 0x1389F40): full, it leaves r8 0 and writes through it (0x1389F72: an 8192 m mission).
 * An extra large world writes slot 0 instead (mov r8, r13 -> mov r8, r11: the table), like the destruction records. */
#define DIF_TABLE2_FULL 0x1389F60u
static const uint8_t kTable2FullPin[10] = {0x4D, 0x8B, 0xC5, 0x4C, 0x8B, 0x8C, 0x24, 0xF8, 0x00, 0x00};
static const uint8_t kTable2Slot0[3] = {0x4D, 0x8B, 0xC3};
#define DIF_TABLE_FULL 0x861CB0u
static const uint8_t kTableFullPin[7] = {0x45, 0x89, 0x16, 0x41, 0x89, 0x6E, 0x04}; /* mov [r14], r10d; mov [r14+4], ebp */
static const uint8_t kTableFullSkip[7] = {0xEB, 0x05, 0x90, 0x90, 0x90, 0x90, 0x90}; /* jmp 0x861CB7 */
#define DIF_PLACE_LOOP 0xF35030u      /* mov rcx, [rdi+0x20]; mov r10d, r13d (esi the instance, rdi the item) */
#define DIF_PLACE_LOOP_BACK 0xF35037u
#define DIF_PLACE_LOOP_END 0xF3517Cu
static const uint8_t kPlaceLoopPin[10] = {0x48, 0x8B, 0x4F, 0x20, 0x45, 0x8B, 0xD5, 0x41, 0xFF, 0xC5};
/* The loop writes location r13d (0xE0 at r15 + r13 x 0xE0 and 0x40 at rbp + r13 x 0x40, 0xF35046 / 0xF35050), bumps it
 * (0xF35037) and stores the final count at 0xF351B5 ([[rsp+0xA0]+0x270]), with no test of the table's room: on a level
 * the stub also skips every instance once r13d reaches the limit (g_world.below[2]: DIF_LOCATIONS - 256 on an extra
 * large world, else none) and counts them (below[3]). */
static const uint8_t kPlaceSkipStub[69] = {
    0x44, 0x3B, 0x2D, 0, 0, 0, 0,                                     /*  0 cmp r13d, [rip+limit] (rel32 at 3) */
    0x73, 0x27,                                                       /*  7 jae 48 (full) */
    0x8B, 0x47, 0x54,                                                 /*  9 mov eax, [rdi+0x54] (the count) */
    0x69, 0xC0, 0x6B, 0xCA, 0xEB, 0x85,                               /* 12 imul eax, eax, 0x85EBCA6B */
    0x01, 0xF0,                                                       /* 18 add eax, esi (the instance) */
    0x69, 0xC0, 0xB1, 0x79, 0x37, 0x9E,                               /* 20 imul eax, eax, 0x9E3779B1 */
    0x3B, 0x05, 0, 0, 0, 0,                                           /* 26 cmp eax, [rip+placeBelow] (rel32 at 28) */
    0x76, 0x17,                                                       /* 32 jbe 57 (kept) */
    0xFF, 0xC6,                                                       /* 34 inc esi (skipped) */
    0x8B, 0x47, 0x54,                                                 /* 36 mov eax, [rdi+0x54] */
    0x3B, 0xF0,                                                       /* 39 cmp esi, eax */
    0x72, 0xD5,                                                       /* 41 jb 0 */
    0xE9, 0, 0, 0, 0,                                                 /* 43 jmp 0xF3517C (the loop's end; rel32 at 44) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 48 lock inc dword [rip+refused] (rel32 at 51) */
    0xEB, 0xE9,                                                       /* 55 jmp 34 */
    0x48, 0x8B, 0x4F, 0x20,                                           /* 57 mov rcx, [rdi+0x20] */
    0x45, 0x8B, 0xD5,                                                 /* 61 mov r10d, r13d */
    0xE9, 0, 0, 0, 0};                                                /* 64 jmp 0xF35037 (rel32 at 65) */
#define DIF_STAMP_PLACE 6.0f /* the placement's share over the jobs' (InstallWorld) */
/* GenerateSubregionSpecial (op 0x76, 0xF65DA0) writes each non-empty location cluster's mean xyz to a 16-slot stack
 * array ([rbp+0x110 + 12 k], 0xF66060..0xF6607C; its cookie at [rbp+0x1D0]), k = the non-empty clusters so far
 * ([rsp+0x50], stored at 0xF6604B), with no bound: vanilla's cluster list held 8. The larger list (DIF_CLUSTERS 32) and
 * an extra large level's clusters of 3 (25 in s75u) overwrote the cookie (fail-fast). The cluster loop's head (0xF65F20:
 * mov r10d, [rdi+0xB8]; test; je 0xF66089, the empty-cluster path) now takes a cluster as empty once 16 are counted;
 * the later reads (0xF664C0.., an index below [rsp+0x50] from 0xF65C70) stay within the written slots. Up to 16 non-empty
 * clusters the op is unchanged. */
#define DIF_SPECIAL_LOOP 0xF65F20u
#define DIF_SPECIAL_EMPTY 0xF66089u
#define DIF_SPECIAL_BODY 0xF65F30u
static const char kSpecialLoopPin[] = "44 8B 97 B8 00 00 00 45 85 D2 0F 84 59 01 00 00";
static const uint8_t kSpecialCapStub[32] = {
    0x83, 0x7C, 0x24, 0x50, 0x10,                                     /*  0 cmp dword [rsp+0x50], 16 (the slots) */
    0x0F, 0x83, 0, 0, 0, 0,                                           /*  5 jae 0xF66089 (rel32 at 7) */
    0x44, 0x8B, 0x97, 0xB8, 0x00, 0x00, 0x00,                         /* 11 mov r10d, [rdi+0xb8] (the members) */
    0x45, 0x85, 0xD2,                                                 /* 18 test r10d, r10d */
    0x0F, 0x84, 0, 0, 0, 0,                                           /* 21 je 0xF66089 (rel32 at 23) */
    0xE9, 0, 0, 0, 0};                                                /* 27 jmp 0xF65F30 (rel32 at 28) */
/* The engine's occlusion test (exe 0x68FAxx) projects a bounding box onto its depth buffer ([rdi+0xB8], width [rdi],
 * height [rdi+4]) and clamps the cell range with minps/maxps, which let a NaN through: cvtps2dq made it 0x80000000, the row
 * loop started at -2^28 and read far before the buffer (a 4096 m mission ended at exe 0x68FD47 while loading; which object
 * projects to NaN is not known yet). With an extra large world a box whose first column or row is negative counts as not
 * occluded (the test's own result 2, 0x68FD65). */
/* The renderer's upload allocator (exe 0x7009C0(alloc rcx, out rdx, size r8, align r9d); callers 0x6EBB30..0x6F8B30, e.g.
 * 0x6EDEB0 through [rsi+0x150]) carves requests from its current block ([alloc], 0x700A46) and, when one does not fit
 * ([alloc+0x88] the block size, 0x700A32), advances to the block's next (0x700A8E) or makes a GPU block (0x700B80) and
 * links it as the current block's next (0x700ADE). A thread whose link lost the race spins at 0x700AF0 until the current
 * block has no next, which only happens once a thread in the main path fills the current block and advances. When every
 * thread that uploads is in that spin, nobody fills it: s75v (planet 157) froze twice right after op 0x42 with 12 workers
 * at 0x700AFB and the frame waiting on their jobs (0x6CBA8C). Requests are never larger than a block (probed: 0). The
 * frame reset (0x700B10) walks each ring's whole chain (+0x20 next, +0x18 offset = 0) and starts again at its head, so a
 * chain of any length is kept and reused. The loser now walks to the chain's tail and links its block there, then goes
 * back to the main path (0x7009F0), which takes room from the current block or advances. */
#define DIF_UPLOAD_SPIN 0x700AF0u
#define DIF_UPLOAD_RETRY 0x7009F0u
static const char kUploadSpinPin[] = "48 8B 0B 33 C0 F0 48 0F B1 71 20 75 F3 E9 EE FE FF FF";
static const uint8_t kUploadTailStub[34] = {
    0x48, 0x8B, 0x0B,                                                 /*  0 mov rcx, [rbx] (the current block) */
    0x48, 0x8B, 0x41, 0x20,                                           /*  3 mov rax, [rcx+0x20] (its next) */
    0x48, 0x85, 0xC0,                                                 /*  7 test rax, rax */
    0x74, 0x09,                                                       /* 10 jz 21 (the tail) */
    0x48, 0x89, 0xC1,                                                 /* 12 mov rcx, rax */
    0x48, 0x8B, 0x41, 0x20,                                           /* 15 mov rax, [rcx+0x20] */
    0xEB, 0xF2,                                                       /* 19 jmp 7 */
    0xF0, 0x48, 0x0F, 0xB1, 0x71, 0x20,                               /* 21 lock cmpxchg [rcx+0x20], rsi (rax 0: link the new block) */
    0x75, 0xEA,                                                       /* 27 jne 7 (someone linked first: rax = that block, walk on) */
    0xE9, 0, 0, 0, 0};                                                /* 29 jmp 0x7009F0 (rel32 at 30) */

static const char* InstallUploadTail(uintptr_t exe)
{
    if (exe == 0 || !ApiCodeMatches(exe + DIF_UPLOAD_SPIN, kUploadSpinPin)) return "the renderer's upload allocator is not the reviewed one (its link race is left as it is)";
    uint8_t* page = FindNearGame(exe, 0x1000, MEM_COMMIT | MEM_RESERVE);
    if (page == NULL) return "no memory near the exe for the upload allocator's link";
    memcpy(page, kUploadTailStub, sizeof kUploadTailStub);
    int32_t rel = (int32_t)(exe + DIF_UPLOAD_RETRY - (uintptr_t)(page + 34)); memcpy(page + 30, &rel, 4);
    DWORD old;
    VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    uint8_t jump[18];
    memset(jump, 0x90, sizeof jump);
    jump[0] = 0xE9; rel = (int32_t)((uintptr_t)page - (exe + DIF_UPLOAD_SPIN + 5)); memcpy(jump + 1, &rel, 4);
    if (ApiWrite(exe + DIF_UPLOAD_SPIN, jump, sizeof jump)) return "the renderer's upload allocator's link could not be written";
    return NULL;
}

#define DIF_CULL_SITE 0x68FD06u    /* exe: mov rdi, [rdi+0xb8]; mov r10d, [rsp+8] (r8d the first row, [rsp] the first column) */
#define DIF_CULL_BACK 0x68FD0Du
#define DIF_CULL_VISIBLE 0x68FD65u
static const uint8_t kCullPin[12] = {0x48, 0x8B, 0xBF, 0xB8, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x54, 0x24, 0x08};
static const uint8_t kCullStub[32] = {
    0x48, 0x8B, 0xBF, 0xB8, 0x00, 0x00, 0x00,                         /*  0 mov rdi, [rdi+0xb8] */
    0x45, 0x85, 0xC0,                                                 /*  7 test r8d, r8d */
    0x0F, 0x88, 0, 0, 0, 0,                                           /* 10 js 0x68FD65 (rel32 at 12) */
    0x8B, 0x04, 0x24,                                                 /* 16 mov eax, [rsp] */
    0x85, 0xC0,                                                       /* 19 test eax, eax */
    0x0F, 0x88, 0, 0, 0, 0,                                           /* 21 js 0x68FD65 (rel32 at 23) */
    0xE9, 0, 0, 0, 0};                                                /* 27 jmp 0x68FD0D (rel32 at 28) */
/* SpeedTree's visible-cell tracking (exe 0x2144E0, strings CVisibleInstances / CCellHeapMgr) clears a cell's bit in a
 * per-base bitmap ([r10+0x28], width [r10+0xA4]) when the cell leaves view, at index (y + w/2) * w + x, unchecked
 * (0x215B6C and 0x215F70). On a 16 km map a cell far from where the camera is now gives a negative index, and the
 * write lands gigabytes off (three landings ended there, 2026-11-02). With an extra large world a clear whose index is
 * not below w * w is skipped: such a bit was never inside the bitmap. The jump replaces the 23 bytes from the index
 * copy to the write; the stub redoes them behind the check. */
#define DIF_TREE_CLEAR_A 0x215B59u    /* mov r8d, r9d ... and [rdx+r8*4], eax; then jmp 0x215B79 */
#define DIF_TREE_CLEAR_A_BACK 0x215B79u
#define DIF_TREE_CLEAR_B 0x215F5Du    /* the same, then add dword [r11+8], -1 at 0x215F74 */
#define DIF_TREE_CLEAR_B_BACK 0x215F74u
static const char kTreeClearPin[] = "45 8B C1 41 83 E1 1F 41 0F B6 C9 49 C1 E8 05 D3 E0 F7 D0 42 21 04 82";
static const uint8_t kTreeClearStub[40] = {
    0x44, 0x8B, 0xC1,                                                 /*  0 mov r8d, ecx (the width) */
    0x44, 0x0F, 0xAF, 0xC1,                                           /*  3 imul r8d, ecx */
    0x45, 0x3B, 0xC8,                                                 /*  7 cmp r9d, r8d */
    0x73, 0x17,                                                       /* 10 jae 35 (outside: skip the write) */
    0x45, 0x8B, 0xC1,                                                 /* 12 mov r8d, r9d */
    0x41, 0x83, 0xE1, 0x1F,                                           /* 15 and r9d, 0x1f */
    0x41, 0x0F, 0xB6, 0xC9,                                           /* 19 movzx ecx, r9b */
    0x49, 0xC1, 0xE8, 0x05,                                           /* 23 shr r8, 5 */
    0xD3, 0xE0,                                                       /* 27 shl eax, cl (eax 1 already) */
    0xF7, 0xD0,                                                       /* 29 not eax */
    0x42, 0x21, 0x04, 0x82,                                           /* 31 and [rdx+r8*4], eax */
    0xE9, 0, 0, 0, 0};                                                /* 35 jmp back (rel32 at 36) */

/* The world's destruction records ([game+0x346D528]+0xC5F48, 0x8000 slots inline; difficulty_maps.c guards the general
 * hash insert 0x172F790) have an inlined insert too (0x13E0340): a full table leaves r8 0 and writes the entry through it
 * (a 4096 m mission ended at 0x13E036A about a minute after landing). With an extra large world a full table writes slot
 * 0 instead (xor r8d, r8d -> mov r8, r11: the table): that slot's destructible loses its record, the game goes on. */
#define DIF_RECORDS_FULL 0x13E0360u
static const uint8_t kRecordsFullPin[10] = {0x45, 0x33, 0xC0, 0x8B, 0xBC, 0x24, 0xC0, 0x00, 0x00, 0x00};
static const uint8_t kRecordsFullSlot0[3] = {0x4D, 0x8B, 0xC3};
static LONG g_worldIndexFullSeen;

/* Diagnostic: the location passes (by the dispatcher's names) wrapped while the world is extra large; each notes the
 * level's locations (G+0x11A7108) and important ones (G+0x11A715C) before and after, and its time. */
typedef uint64_t (*WorldPassFn)(uint64_t, uint64_t, uint64_t, uint64_t);
static uintptr_t g_worldLevel;
static uint32_t g_importantList = 0x8996B0u; /* G + this: the important locations (difficulty_block.c moves them) */
static int32_t phaseBefore66 = -1; /* research: the road units when op 0x66 began */
static uint32_t g_routeNodes = 0x8C4E40u, g_routeEdges = 0x937348u, g_routePoints = 0x9568E4u; /* G + these: the route graph (difficulty_block.c moves it) */
typedef struct TileRunner TileRunner;
static inline TileRunner* TileRunnerSelf(void); /* below: a stamp planning runner on this thread */
static void WorldPassNote(const char* name, uint32_t before[2], uint64_t start)
{
    if (TileRunnerSelf() != NULL) return; /* a runner's tile build: the note is about the level's block, and runners do not log */
    uintptr_t block = 0;
    uint32_t after[2] = {0, 0};
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    if (g_worldLevel == 0 || SafeRead(g_worldLevel + 0x2C8, &block, 8) || block == 0) return;
    SafeRead(block + 0x11A7108, &after[0], 4); SafeRead(block + 0x11A715C, &after[1], 4);
    Note("difficulty: extra large maps: %s at %.0fx: locations %u -> %u, important %u -> %u, %.1f ms", name, (double)g_world.current,
        before[0], after[0], before[1], after[1], (double)(now.QuadPart - (LONGLONG)start) * 1000.0 / (double)frequency.QuadPart);
}
#define WORLD_PASS(fn, rva, label) \
    static WorldPassFn g_pass##fn; \
    static uint64_t WorldPass##fn(uint64_t a, uint64_t b, uint64_t c, uint64_t e) \
    { \
        uintptr_t block = 0; uint32_t before[2] = {0, 0}; LARGE_INTEGER start; \
        if (g_worldLevel != 0 && !SafeRead(g_worldLevel + 0x2C8, &block, 8) && block != 0) \
        { SafeRead(block + 0x11A7108, &before[0], 4); SafeRead(block + 0x11A715C, &before[1], 4); } \
        QueryPerformanceCounter(&start); \
        uint64_t r = g_pass##fn(a, b, c, e); \
        WorldPassNote(label, before, (uint64_t)start.QuadPart); \
        return r; \
    }
WORLD_PASS(Locations, 0xF767A0u, "GenerateLocations")
WORLD_PASS(Discovery, 0xF7D780u, "GenerateDiscoveryLocations")
WORLD_PASS(Outposts, 0xF7E250u, "GenerateOutpostLocations")
WORLD_PASS(ValidSmall, 0xF83EA0u, "CalculateValidSmallLocationsPositions")
WORLD_PASS(Small, 0xF84380u, "GenerateSmallLocations")
WORLD_PASS(Border, 0xF77F30u, "GenerateEmptyBorderLocations")
WORLD_PASS(Crossing, 0xF82A80u, "GenerateCrossingLocations")
WORLD_PASS(Spread, 0xF7C310u, "SpreadLocations")
WORLD_PASS(Satellite, 0xF86DC0u, "AddSatelliteLocations")
WORLD_PASS(Scatter, 0xF40F80u, "InitializeScatter")
WORLD_PASS(Edge, 0xF82710u, "SpreadEdgeOfMapLocations")
WORLD_PASS(ExplorationTimed, 0xF95900u, "GenerateExplorationLocations")
/* Points of interest for an operation's missions on one map (operation_missions): GenerateExplorationLocations (op 0xA0,
 * 0xF95900; mandatory and optional runs) takes its counts from the region's settings R = [[[ctx+0x18]+0x2D0]+0x68]: the
 * quota list R+0x70 (0x14 each, count R+0x78; min +4, max +8; the slot builder 0xF95310 adds min slots per entry in the
 * mandatory run and rand % (max - min + 1) in the optional run, its buffer sized from the maxima) and the category caps
 * R+0x80 ({id, max} 8 bytes, count R+0x88, copied per run at 0xF95B70). Both are multiplied for the pass and put back
 * after it (they are shared region data). The per-item limit item+0x15C stays. */
#define EXPLORATION_SAVE 256
/* Whether [at, at + bytes) is committed, writable heap data (the region's settings are written in place). */
static int WorldDataWritable(uint64_t at, size_t bytes)
{
    uintptr_t p = (uintptr_t)at, end = p + bytes;
    while (p < end)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0) return 0;
        DWORD protect = mbi.Protect & 0xFF;
        if (protect != PAGE_READWRITE && protect != PAGE_EXECUTE_READWRITE) return 0;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= p) return 0;
        p = next;
    }
    return 1;
}
static SRWLOCK g_explorationLock = SRWLOCK_INIT; /* every run of the pass, one at a time: previews generate on other threads */
static uint64_t WorldPassExploration(uint64_t a, uint64_t b, uint64_t c, uint64_t e)
{
    AcquireSRWLockExclusive(&g_explorationLock);
    uint64_t lg = 0, T = 0, R = 0, quotas = 0, caps = 0;
    uint32_t nq = 0, nc = 0, m = g_difficulty.missions;
    uint8_t level = 0;
    static uint32_t saveQ[EXPLORATION_SAVE][2], saveC[EXPLORATION_SAVE];
    int scaled = 0;
    if (m > 1 && g_world.current > 1.0f && !SafeRead(c + 0x18, &lg, 8) && lg != 0 && !SafeRead(lg + 0xF9, &level, 1) && level == DIF_VANILLA + 1 &&
        !SafeRead(lg + 0x2D0, &T, 8) && T != 0 && !SafeRead(T + 0x68, &R, 8) && R != 0 && !SafeRead(R + 0x70, &quotas, 8) && !SafeRead(R + 0x78, &nq, 4) &&
        !SafeRead(R + 0x80, &caps, 8) && !SafeRead(R + 0x88, &nc, 4) && nq <= EXPLORATION_SAVE && nc <= EXPLORATION_SAVE && (nq == 0 || quotas != 0) &&
        (nc == 0 || caps != 0))
    {
        scaled = (nq == 0 || WorldDataWritable(quotas, 0x14ull * nq)) && (nc == 0 || WorldDataWritable(caps, 8ull * nc));
        for (uint32_t i = 0; i < nq && scaled; i++)
            if (SafeRead(quotas + 0x14ull * i + 4, saveQ[i], 8)) scaled = 0;
        for (uint32_t i = 0; i < nc && scaled; i++)
            if (SafeRead(caps + 8ull * i + 4, &saveC[i], 4)) scaled = 0;
        for (uint32_t i = 0; i < nq && scaled; i++)
        {
            uint32_t v[2] = {saveQ[i][0] * m, saveQ[i][1] * m};
            memcpy((void*)(uintptr_t)(quotas + 0x14ull * i + 4), v, 8);
        }
        for (uint32_t i = 0; i < nc && scaled; i++)
        {
            uint32_t v = saveC[i] * m;
            memcpy((void*)(uintptr_t)(caps + 8ull * i + 4), &v, 4);
        }
        static LONG noted;
        if (InterlockedExchange(&noted, 1) == 0)
        {
            /* one mission's expected count: sum of min + (max - min) / 2 per entry (the optional slots are uniform draws) */
            char q[512]; int at = 0; double expect = 0;
            for (uint32_t i = 0; i < nq && at < (int)sizeof q - 32; i++)
            {
                at += snprintf(q + at, sizeof q - at, " %u..%u", saveQ[i][0], saveQ[i][1]);
                expect += saveQ[i][0] + (saveQ[i][1] > saveQ[i][0] ? (saveQ[i][1] - saveQ[i][0]) / 2.0 : 0.0);
            }
            Note("difficulty: an operation's %u missions on one map: points of interest x%u (%u quota entries, %u category caps)%s; one mission's quotas (min..max):%s, "
                "so %.1f expected a mission (the mandatory and optional passes together)", m, m, nq, nc,
                scaled ? "" : "; the region's settings could not be written, so its counts stay", q, expect);
        }
    }
    if (scaled && g_world.values != NULL) g_world.values->capMul = m; /* the items' map-wide caps too (CapStubs) */
    uint64_t r = WorldPassExplorationTimed(a, b, c, e);
    if (g_world.values != NULL) g_world.values->capMul = 1;
    if (scaled)
    {
        for (uint32_t i = 0; i < nq; i++) memcpy((void*)(uintptr_t)(quotas + 0x14ull * i + 4), saveQ[i], 8);
        for (uint32_t i = 0; i < nc; i++) memcpy((void*)(uintptr_t)(caps + 8ull * i + 4), &saveC[i], 4);
    }
    ReleaseSRWLockExclusive(&g_explorationLock);
    return r;
}
/* AddSatelliteLocations (op 0x8E, 0xF86DC0): an operation's missions on one map hold each satellite item's map-wide cap
 * x missions (CapStubs), as GenerateExplorationLocations does; under the same lock, so a preview's pass never sees it. */
static uint64_t WorldPassSatelliteCaps(uint64_t a, uint64_t b, uint64_t c, uint64_t e)
{
    AcquireSRWLockExclusive(&g_explorationLock);
    uint64_t lg = 0; uint8_t level = 0; uint32_t m = g_difficulty.missions;
    int on = m > 1 && g_world.current > 1.0f && g_world.values != NULL && !SafeRead(c + 0x18, &lg, 8) && lg != 0 && !SafeRead(lg + 0xF9, &level, 1) &&
        level == DIF_VANILLA + 1;
    if (on) g_world.values->capMul = m;
    uint64_t r = WorldPassSatellite(a, b, c, e);
    if (g_world.values != NULL) g_world.values->capMul = 1;
    ReleaseSRWLockExclusive(&g_explorationLock);
    static LONG noted;
    if (on && InterlockedExchange(&noted, 1) == 0) Note("difficulty: an operation's %u missions on one map: satellite and point-of-interest items' map-wide caps x%u", m, m);
    return r;
}
/* The map's objective list S is filled at generation setup (0xF7166B -> 0xF70F20: rcx the level, r8 the mission row,
 * r9 the descriptor's ids (level+0x110), the 5th argument their count (+0x10D), nine more through to the 14th). The
 * descriptor holds 32 ids at most (its tail follows at +0xA0; objcap.md), and the selection is held at the objective
 * list's room, so an operation's three missions on one map got fewer objectives than three missions have. Here the list
 * gets more: repeats of the descriptor's major (class 2 in the base row) and then optional (class 3) ids in their order,
 * until each class reaches each mission's count x the missions (g_operationMajor / g_operationOptional) or the list is
 * full. Each id is held at what three missions of the row can hold, the game's own rules for one selection (0x1756730)
 * times the missions: uses <= missions x min(row entry +0x14, definition +0x20), and the ids sharing a bit of the
 * definition's exclusive byte +0x2C at most missions uses per bit (the picker 0x1757870 drops an entry whose bits were
 * picked, 0x175790A). Past that the game's op 0x16 found no free place for the late repeats (s75w, row 137: five
 * once-per-mission optionals filled to 24, 37 places, 33 placed). Only the descriptor and the static tables decide it, so
 * every peer fills the same list. */
typedef uint64_t (*ObjectiveFillFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t);
static ObjectiveFillFn g_objectiveFill;
static uint32_t g_objectiveList[64], g_objectiveListCount; /* the list as filled, for the placement note after op 0x16 */
/* The list as the game selected it, with each id's class in its row (-1: not in the row, a partner's main): each main is
 * followed by its own steps (the selection builds a mission at a time), the only tie between a step and its main
 * (static, subagent hud18: the definitions and mission rows hold no parent). The HUD orders its rows by it. */
static uint32_t g_hudOrder[32], g_hudOrderCount;
static int g_hudOrderClass[32];
static uint32_t ObjectiveIdCap(uintptr_t base, uint32_t id, uint8_t* bits) /* one mission's most uses of id, and its exclusive bits */
{
    uint8_t entries[32 * 0x18];
    uint32_t most = 0xFFFFFFFFu, perMission = 0;
    *bits = 0;
    if (!SafeRead(base + 0x40, entries, sizeof entries))
        for (int i = 0; i < 32; i++)
        {
            uint32_t entry;
            memcpy(&entry, entries + i * 0x18, 4);
            if (entry == id) { memcpy(&most, entries + i * 0x18 + 0x14, 4); break; }
        }
    uintptr_t def = ObjectiveDef(g_difficultyGame, id);
    if (def == 0 || SafeRead(def + 0x20, &perMission, 4) || SafeRead(def + 0x2C, bits, 1)) return 0;
    return perMission < most ? perMission : most;
}
static uint64_t WorldObjectiveFill(uint64_t level, uint64_t b, uint64_t row, uint64_t ids, uint64_t count, uint64_t a6, uint64_t a7, uint64_t a8,
    uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14)
{
    uint32_t n = (uint32_t)count, list[64];
    uint8_t difficulty = 0;
    uint16_t r = (uint16_t)row;
    g_objectiveListCount = 0;
    /* The list as the game selected it (id/class from the row), on every level-11 map, vanilla size too (the census
     * baselines): the placement note after op 0x16 then compares it with what was placed. */
    if (g_difficultyGame != 0 && ids != 0 && n > 0 && n <= 32 && r < DIF_MISSION_ROWS && !SafeRead(level + 0xF9, &difficulty, 1) &&
        difficulty == DIF_VANILLA + 1 && !SafeRead(ids, list, 4ull * n))
    {
        uintptr_t base = g_difficultyGame + DIF_MISSIONS + (uintptr_t)r * DIF_MISSION_ROW;
        char text[1024];
        int at = snprintf(text, sizeof text, "difficulty: the objective list as selected, row %u, %u ids (id/class):", (unsigned)r, n);
        for (uint32_t i = 0; i < n && at > 0 && at < (int)sizeof text - 16; i++) at += snprintf(text + at, sizeof text - at, " %08x/%d", list[i], RowClass(base, list[i]));
        static LONG noted;
        if (InterlockedIncrement(&noted) <= 4) Note("%s", text);
        memcpy(g_objectiveList, list, 4ull * n);
        g_objectiveListCount = n;
        for (uint32_t i = 0; i < n; i++) { g_hudOrder[i] = list[i]; g_hudOrderClass[i] = RowClass(base, list[i]); }
        g_hudOrderCount = n;
    }
    difficulty = 0;
    if (g_difficulty.missions > 1 && g_operationOptional != 0 && g_difficultyGame != 0 && ids != 0 && n > 0 && n <= 32 && r < DIF_MISSION_ROWS &&
        g_objectiveListPlaces > n && g_objectiveListPlaces <= 64 && !SafeRead(level + 0xF9, &difficulty, 1) && difficulty == DIF_VANILLA + 1 &&
        !SafeRead(ids, list, 4ull * n))
    {
        uintptr_t base = g_difficultyGame + DIF_MISSIONS + (uintptr_t)r * DIF_MISSION_ROW;
        uint32_t m = g_difficulty.missions, total = n, uses[64] = {0}, cap[64], bitUses[8] = {0}, have[2] = {0}, before[2] = {0}, capped[2] = {0};
        uint8_t bits[64];
        int cls[64];
        for (uint32_t i = 0; i < n; i++)
        {
            cls[i] = RowClass(base, list[i]);
            cap[i] = ObjectiveIdCap(base, list[i], &bits[i]);
            cap[i] = cap[i] > 0xFFFFFFFFu / m ? 0xFFFFFFFFu : cap[i] * m;
        }
        for (uint32_t i = 0; i < n; i++)
        {
            uint32_t first = i;
            for (uint32_t j = 0; j < i; j++) if (list[j] == list[i]) { first = j; break; }
            uses[first]++;
            for (int t = 0; t < 8; t++) if (bits[i] & (1u << t)) bitUses[t]++;
            if (cls[i] == 2) have[0]++;
            if (cls[i] == 3) have[1]++;
        }
        before[0] = have[0]; before[1] = have[1];
        /* Every mission's main objectives and steps (the grouped HUD holds them, HudGroupStub): a row that mixes no
         * partner (a city's Launch ICBM row 50: ICBM x3 and its three steps at vanilla size) gets the descriptor's class
         * 0 and then class 1 ids again for each mission past the first, a mission's block at a time, before the majors
         * (op 0x16 places the list in order). A mixing row's other missions' primaries come from MixPartner (2 x missions
         * - 1 partners). Their exclusive bits are not counted: they are each mission's own. */
        uint32_t mainsBefore = 0, mainsAfter = 0;
        for (uint32_t i = 0; i < n; i++) mainsBefore += (cls[i] == 0 || cls[i] == 1) && list[i] != DIF_LIVE_EVENT_OBJECTIVE;
        mainsAfter = mainsBefore;
        const int mixes = (g_difficulty.flags & HF_DIFFICULTY_SECOND_PRIMARY) && g_mixBase[r];
        if (g_hudGrouped && !mixes)
            for (uint32_t k = 1; k < m; k++)
                for (int c = 0; c < 2; c++)
                    for (uint32_t i = 0; i < n && total < g_objectiveListPlaces; i++)
                        if (cls[i] == c && list[i] != DIF_LIVE_EVENT_OBJECTIVE) { list[total++] = list[i]; mainsAfter++; }
        /* A mixing row whose layout has fewer partners than 2 x missions - 1 (s93b: row 129 on layout 1 mixed row 118
         * only, so the 16 km map had one primary and its steps for three missions): its primary blocks (a main, class
         * 0 or a partner's -1, and the class 1 steps the selection put right after it) again, in order, until the map
         * holds 2 x missions primaries (a level-11 mission's two, per mission), each block at most once per mission. */
        uint32_t primaries = 0, blocksAdded = 0;
        if (g_hudGrouped && mixes)
        {
            uint32_t start[32], length[32], blocks = 0;
            for (uint32_t i = 0; i < n && blocks < 32; i++)
            {
                if (!((cls[i] == 0 || cls[i] == -1) && list[i] != DIF_LIVE_EVENT_OBJECTIVE)) continue;
                uint32_t j = i + 1;
                while (j < n && cls[j] == 1 && list[j] != DIF_LIVE_EVENT_OBJECTIVE) j++;
                start[blocks] = i; length[blocks] = j - i; blocks++;
            }
            primaries = blocks;
            const uint32_t target = 2 * m < blocks * m ? 2 * m : blocks * m;
            for (uint32_t k = 0; blocks > 0 && primaries < target; k++)
            {
                uint32_t b = k % blocks;
                if (total + length[b] > g_objectiveListPlaces) break;
                for (uint32_t t = 0; t < length[b]; t++) list[total++] = list[start[b] + t];
                mainsAfter += length[b]; primaries++; blocksAdded++;
            }
        }
        const uint32_t want[2] = {g_operationMajor, g_operationOptional};
        for (int c = 0; c < 2; c++) /* majors first: op 0x16 places the list in order */
        {
            for (int progress = 1; progress && have[c] < want[c] && total < g_objectiveListPlaces;)
            {
                progress = 0;
                for (uint32_t i = 0; i < n && have[c] < want[c] && total < g_objectiveListPlaces; i++)
                {
                    if (cls[i] != c + 2) continue;
                    uint32_t first = i;
                    for (uint32_t j = 0; j < i; j++) if (list[j] == list[i]) { first = j; break; }
                    if (first != i) continue; /* each id once per sweep */
                    int open = uses[i] < cap[i];
                    for (int t = 0; t < 8 && open; t++) if ((bits[i] & (1u << t)) && bitUses[t] >= m) open = 0;
                    if (!open) continue;
                    list[total++] = list[i];
                    uses[i]++;
                    for (int t = 0; t < 8; t++) if (bits[i] & (1u << t)) bitUses[t]++;
                    have[c]++;
                    progress = 1;
                }
            }
            if (have[c] < want[c] && total < g_objectiveListPlaces) capped[c] = 1;
        }
        if (total > n)
        {
            memcpy(g_objectiveList, list, 4ull * total);
            g_objectiveListCount = total;
            char mixNote[96] = "";
            if (blocksAdded) snprintf(mixNote, sizeof mixNote, " (%u primaries again, too few partners for this layout: %u of %u)", blocksAdded, primaries, 2 * m);
            static LONG noted;
            if (InterlockedIncrement(&noted) <= 4)
                Note("difficulty: an operation's %u missions on one map: on the map's objective list main objectives and steps %u -> %u%s%s, major %u -> %u of %u%s, "
                    "optional %u -> %u of %u%s (%u -> %u of %u places)", m, mainsBefore, mainsAfter, mixes ? " (the partners' primaries)" : "", blocksAdded ? mixNote : "",
                    before[0], have[0], want[0], capped[0] ? " (the row's ids hold no more for its missions)" : "", before[1], have[1], want[1],
                    capped[1] ? " (the row's ids hold no more for its missions)" : "", n, total, g_objectiveListPlaces);
            return g_objectiveFill(level, b, row, (uint64_t)(uintptr_t)list, total, a6, a7, a8, a9, a10, a11, a12, a13, a14);
        }
        memcpy(g_objectiveList, list, 4ull * n);
        g_objectiveListCount = n;
    }
    return g_objectiveFill(level, b, row, ids, count, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}
/* ---- The HUD's main objectives and steps, a row per id (every mission's mains and steps on one map) -----------------
 * Static (mains16; 0x17CF690 / 0x17CF8A0 / 0x1845E80 / 0x1846C40 re-read by Claude): the HUD's objective list builder
 * 0x17CF690 (once a mission, from the per-frame updater 0x17CF8A0) writes the ids of every class 0 instance and then
 * every class 1 instance (manager [game+0x3326DA0]: count +0x24, instances +0x60 x 0x1078 with the class at +0x1038,
 * id pointers +0x50 -> +8) into a 16-entry stack list at rsp+0x30 with no bound (the 17th overwrites the /GS cookie at
 * rsp+0x70: the game ends at the first HUD update), then adds a row a list entry (0x184BDE0: 16 rows at most). Rows are
 * keyed by the instance's key ([ptr+8], one per instance: the manager's hash [mgr+0x38] maps it to the instance; s80a
 * showed 16 keys for 48 instances), its text from the definition [ptr+0] (u64, the objective settings table, 0x4FA880).
 * The updater finds an instance's row by key (0x17CFA21; none found = row 0) and keeps a near flag per row (rsp+0x30,
 * zeroed at 0x17CF911, stored at 0x17CFA66); a row's state (0x1845E80) is its key's instance, set by 0x1846C40(row,
 * state: 1 open, 2 done, 3 failed, 4 other) at 0x1846177. HudGroupInstall: the builder's list part (0x17CF6E7..0x17CF79A)
 * becomes a call to HudObjectiveRows (one row per definition, keyed by its first instance, mains first, 16 rows at most);
 * the updater's row search (0x17CFA0B..0x17CFA36) becomes HudRowOf (the key's row, else its definition's row) and the
 * flag store an OR over the copies; the row's state covers every copy of the definition (failed if any failed, as the
 * game fails them all; done when all are done). The game's gating is map-wide already: every
 * main waits for every step (0x5D3693), extraction for every class 0/1 instance (0x5D91B0). */
#define HUD_LIST_SITE 0x17CF6E7u
#define HUD_LIST_BACK 0x17CF79Au
#define HUD_ROW_ADD 0x184BDE0u
#define HUD_ROW_STATE 0x1846C40u
#define HUD_OBJECTIVES 0x3326DA0u
static const char kHudListPin[] = "E8 04 A0 E0 FE 45 33 DB 45 33 C0 33 F6 44 8B F0 85 FF 0F 84";
static const char kHudBackPin[] = "0F 57 C9 49 8D 8F F0 6D 00 00 E8";
static const char kHudRowAddPin[] = "40 55 53 57 48 8D 6C 24 B0 48 81 EC";
static const char kHudRowStatePin[] = "4C 8B DC 55 56 57 49 8D 6B A1 48 81 EC D0 00 00 00";
#define HUD_FIND_SITE 0x17CFA0Bu  /* the updater's row search, 43 bytes: ebp = the key's row (0 when none), esi = the key */
#define HUD_FIND_BACK 0x17CFA36u
#define HUD_FLAG_SITE 0x17CFA66u  /* mov byte [rsp+rax+0x30], cl -> or */
static const char kHudFindPin[] = "49 8B 45 50 4A 8B 14 E0 48 8B 44 24 28 48 05 50 62 01 00 8B 72 08 39 30 74 0F FF C1 48 05 68 F0 00 00 83 F9 10 72 EF EB 02 8B E9";
static const char kHudFlagPin[] = "88 4C 04 30";
static uint64_t g_hudRowDef[16];
static uint32_t g_hudRowKey[16], g_hudRows;
typedef void (*HudRowAddFn)(uint64_t widget, uint32_t id);
typedef void (*HudRowStateFn)(uint64_t row, uint32_t state);
static HudRowStateFn g_hudRowState;
static volatile LONG g_hudRowsCut;
static int HudInstances(uint64_t* inst, uint64_t* ids, uint32_t* n)
{
    uint64_t mgr = 0;
    return !SafeRead(g_world.game + HUD_OBJECTIVES, &mgr, 8) && mgr != 0 && !SafeRead(mgr + 0x24, n, 4) && !SafeRead(mgr + 0x60, inst, 8) &&
        !SafeRead(mgr + 0x50, ids, 8) && *inst != 0 && *ids != 0 && *n <= 4096;
}
/* An objective id's definition key (the u64 its instances hold at [p+0]): the objective table entry +0x18
 * (game 0x32EF870, 0xA0 each; static, subagent hud18), 0 if none. */
static uint64_t HudDefOf(uint32_t id)
{
    uint64_t def = 0;
    uintptr_t entry = ObjectiveDef(g_world.game, id);
    if (entry == 0 || SafeRead(entry + 0x18, &def, 8)) return 0;
    return def;
}
/* 0x184BDE0 (the row add) sorts every row by a per-objective value (0xA03530) after each add and puts every step after
 * every main (0x184BFFB..0x184C04E), so the order the rows are added in is lost. After the adds the slots are keyed again
 * in the wanted order with 0x18462C0(row, key), as the game's own sort ends (0x184C14E..0x184C17A); nothing sorts them
 * afterwards (0x184BDE0 is called only from the list builder this replaces; static, subagent hud18). */
#define HUD_ROW_KEY 0x18462C0u
static const char kHudRowKeyPin[] = "4C 8B DC 55 48 81 EC 80 00 00 00 48 8B E9 39 91 60 F0 00 00";
static int g_hudRekey; /* 1 the re-key is the reviewed code, -1 not */
static void HudObjectiveRows(uint64_t hud)
{
    uint64_t inst = 0, ptrs = 0, defs[32];
    uint32_t n = 0, keys[32], m = 0, cut = 0;
    int cls[32];
    g_hudRows = 0;
    if (!HudInstances(&inst, &ptrs, &n)) return;
    for (int32_t c = 0; c < 2; c++)
        for (uint32_t i = 0; i < n; i++)
        {
            int32_t ic = -1; uint64_t p = 0, def = 0; uint32_t id = 0;
            if (SafeRead(inst + 0x1078ull * i + 0x1038, &ic, 4) || ic != c || SafeRead(ptrs + 8ull * i, &p, 8) || p == 0 || SafeRead(p + 8, &id, 4) ||
                SafeRead(p, &def, 8)) continue;
            uint32_t j = 0;
            while (j < m && defs[j] != def) j++;
            if (j < m) continue;
            if (m < 32) { defs[m] = def; keys[m] = id; cls[m] = c; m++; } else cut++;
        }
    /* main -> its steps, in the selection's order; then what the list does not name (mains, then steps) */
    uint32_t order[32], k = 0;
    uint8_t taken[32] = {0};
    for (uint32_t e = 0; e < g_hudOrderCount && e < 32; e++)
    {
        int oc = g_hudOrderClass[e];
        if (oc == 2 || oc == 3) continue;
        uint64_t def = HudDefOf(g_hudOrder[e]);
        for (uint32_t j = 0; j < m && def != 0; j++)
            if (!taken[j] && defs[j] == def) { taken[j] = 1; order[k++] = j; break; }
    }
    for (int32_t c = 0; c < 2; c++)
        for (uint32_t j = 0; j < m; j++) if (!taken[j] && cls[j] == c) { taken[j] = 1; order[k++] = j; }
    if (k > 16) { cut += k - 16; k = 16; }
    uint32_t rows[16];
    for (uint32_t j = 0; j < k; j++) { g_hudRowDef[j] = defs[order[j]]; g_hudRowKey[j] = rows[j] = keys[order[j]]; }
    g_hudRows = k;
    uint64_t w = hud + 0x6DF0;
    for (uint32_t j = 0; j < k; j++) ((HudRowAddFn)(g_world.game + HUD_ROW_ADD))(w, rows[j]);
    if (g_hudRekey == 0) g_hudRekey = ApiCodeMatches(g_world.game + HUD_ROW_KEY, kHudRowKeyPin) == 1 ? 1 : -1;
    uint32_t slots = 0;
    int keyed = 0;
    if (g_hudRekey == 1 && !SafeRead(w + 0xF1884, &slots, 4) && slots == k)
    {
        typedef void (*HudRowKeyFn)(uint64_t row, uint32_t key);
        for (uint32_t j = 0; j < k; j++) ((HudRowKeyFn)(g_world.game + HUD_ROW_KEY))(w + 0x400 + 0xF068ull * j, rows[j]);
        keyed = 1;
    }
    if (cut) InterlockedExchangeAdd(&g_hudRowsCut, (LONG)cut);
    static LONG noted;
    if (InterlockedIncrement(&noted) <= 4)
        Note("difficulty: HUD: %u main objective and step rows (one per objective) for %u instances%s; %s", k, n, cut ? " (objectives past 16 rows not shown)" : "",
            keyed ? "each main followed by its own steps" : g_hudRekey != 1 ? "the row order is the game's (its re-key is not the reviewed code)" : "the row order is the game's (the rows did not all come in)");
}
/* The copy count in a row's title. The title is a text id on the element row+0x8B8 ([elem+0x110]; 0x1441720(elem, id)
 * stores it, frees the cached text and marks the element dirty, and returns at once when it is already that id); the
 * game sets the objective's name id there only on a key change or a state change to 1 (0x1846820, from 0x1846376 /
 * 0x18470B5). Each row gets HUD_TEXT_IDS ids of its own, answered by the engine text hook (registry.c TextDetour ->
 * HudText) with "<name> <done>/<copies>"; a new text goes into an id the row is not showing, so a buffer is never
 * rewritten while it is shown; the name comes from the engine's text for the id the game set ('#' doubled: the text's
 * variables are #NAME, 0x1300B90). Static, subagent hud18. */
#define HUD_TITLE 0x1441720u
#define HUD_TEXT_IDS 4
static const char kHudTitlePin[] = "48 83 EC 28 4C 8B D9 39 91 10 01 00 00 0F 84";
static struct { uint32_t ids[HUD_TEXT_IDS], name; int cur; char text[HUD_TEXT_IDS][192]; } g_hudText[16];
static int g_hudTextOn; /* 1 on, -1 off */
static const char* HudText(uint32_t id)
{
    if ((id & 0xFFFF0000u) != 0x6F420000u) return NULL;
    for (int j = 0; j < 16; j++) for (int t = 0; t < HUD_TEXT_IDS; t++) if (g_hudText[j].ids[t] == id) return g_hudText[j].text[t];
    return NULL;
}
static void HudTextOn(void)
{
    if (g_hudTextOn != 0) return;
    g_hudTextOn = -1;
    if (ApiCodeMatches(g_world.game + HUD_TITLE, kHudTitlePin) != 1) { Note("difficulty: HUD: the row title setter is not the reviewed code; no copy counts"); return; }
    for (int j = 0; j < 16; j++) for (int t = 0; t < HUD_TEXT_IDS; t++) g_hudText[j].ids[t] = 0x6F420000u | (uint32_t)(0x4800 + j * HUD_TEXT_IDS + t);
    g_textDynamic = HudText;
    g_hudTextOn = 1;
}
static void HudRowTitle(uint32_t slot, uint64_t row, uint32_t copies, uint32_t done)
{
    if (g_hudTextOn == 0) HudTextOn();
    if (g_hudTextOn != 1 || g_engineText == NULL || slot >= 16) return;
    typedef void (*TitleFn)(uint64_t elem, uint32_t id);
    TitleFn title = (TitleFn)(g_world.game + HUD_TITLE);
    uint64_t elem = row + 0x8B8;
    uint32_t shown = 0;
    if (SafeRead(elem + 0x110, &shown, 4)) return;
    int ours = HudText(shown) != NULL;
    if (!ours && shown != 0) g_hudText[slot].name = shown;
    uint32_t name = g_hudText[slot].name;
    if (name == 0) return;
    if (copies <= 1) { if (ours) title(elem, name); return; }
    const char* plain = g_engineText(name, NULL, NULL, NULL);
    char want[192];
    int at = 0;
    for (const char* c = plain ? plain : ""; *c && at < (int)sizeof want - 24; c++) { if (*c == '#') want[at++] = '#'; want[at++] = *c; }
    snprintf(want + at, sizeof want - at, "  %u/%u", done, copies);
    int cur = g_hudText[slot].cur;
    if (ours && shown == g_hudText[slot].ids[cur] && strcmp(g_hudText[slot].text[cur], want) == 0) return;
    int next = (cur + 1) % HUD_TEXT_IDS;
    memcpy(g_hudText[slot].text[next], want, strlen(want) + 1);
    g_hudText[slot].cur = next;
    title(elem, g_hudText[slot].ids[next]);
}
/* Each frame per live row (0x1845CA0 -> 0x1846177): the state over every copy, then the title with the copy count. */
static void HudRowState(uint64_t row, uint32_t state)
{
    uint64_t inst = 0, ptrs = 0;
    uint32_t n = 0, id = 0, slot = 16;
    uint64_t def = 0;
    for (uint32_t j = 0; j < g_hudRows && j < 16; j++)
        if (!SafeRead(row + 0xF060, &id, 4) && g_hudRowKey[j] == id) { def = g_hudRowDef[j]; slot = j; break; }
    uint32_t copies = 0, done = 0, failed = 0;
    if (def != 0 && HudInstances(&inst, &ptrs, &n))
        for (uint32_t i = 0; i < n; i++)
        {
            int32_t cls = -1, st = 0; uint64_t p = 0, at = 0;
            if (SafeRead(inst + 0x1078ull * i + 0x1038, &cls, 4) || (cls != 0 && cls != 1) || SafeRead(ptrs + 8ull * i, &p, 8) || p == 0 ||
                SafeRead(p, &at, 8) || at != def || SafeRead(inst + 0x1078ull * i + 0x1018, &st, 4)) continue;
            copies++; done += st == 2; failed += st == 3;
        }
    if (state >= 1 && state <= 3 && copies > 1) state = failed ? 3u : done == copies ? 2u : state == 2 ? 1u : state;
    g_hudRowState(row, state);
    if (slot < 16) HudRowTitle(slot, row, copies, done);
}
/* Called from the updater's row search (HUD_FIND_SITE, main thread, per instance of class 0/1 per frame): the row of the
 * instance's key, else the row of its definition, else row 0 as the game; the key in the high half (esi after). */
static uint64_t HudRowOf(uint64_t hud, uint32_t index)
{
    /* direct reads: the game reads the same manager, pointer and key at this site (0x17CF8E5, 0x17CFA0F, 0x17CFA1E) */
    uint64_t mgr = *(const uint64_t*)(uintptr_t)(g_world.game + HUD_OBJECTIVES), ptrs = mgr ? *(const uint64_t*)(uintptr_t)(mgr + 0x50) : 0;
    uint64_t p = ptrs ? *(const uint64_t*)(uintptr_t)(ptrs + 8ull * index) : 0;
    if (p == 0) return 0;
    uint64_t def = *(const uint64_t*)(uintptr_t)p;
    uint32_t key = *(const uint32_t*)(uintptr_t)(p + 8), found = 0;
    const uint32_t* rowKey = (const uint32_t*)(uintptr_t)(hud + 0x16250); /* widget +0x6DF0, rows +0x400 x 0xF068, key +0xF060 */
    uint32_t want = key;
    for (int pass = 0; pass < 2 && !found; pass++)
    {
        if (pass == 1)
        {
            uint32_t j = 0;
            while (j < g_hudRows && j < 16 && g_hudRowDef[j] != def) j++;
            if (j >= g_hudRows || j >= 16) break;
            want = g_hudRowKey[j];
        }
        for (uint32_t r = 0; r < 16; r++)
            if (*(const uint32_t*)((const uint8_t*)rowKey + 0xF068ull * r) == want) { found = r + 1; break; }
    }
    return ((uint64_t)key << 32) | (found ? found - 1 : 0);
}
static int HudFindStub(uintptr_t game, uint8_t* code) /* before the block is made executable */
{
    if (!ApiCodeMatches(game + HUD_FIND_SITE, kHudFindPin) || !ApiCodeMatches(game + HUD_FLAG_SITE, kHudFlagPin)) return 0;
    static const uint8_t stub[] = {
        0x48, 0x83, 0xEC, 0x40,                   /*  0 sub rsp, 0x40 (16-aligned at the site) */
        0x0F, 0x11, 0x4C, 0x24, 0x20,             /*  4 movups [rsp+0x20], xmm1 (live across) */
        0x0F, 0x11, 0x54, 0x24, 0x30,             /*  9 movups [rsp+0x30], xmm2 (live across) */
        0x48, 0x8B, 0x4C, 0x24, 0x68,             /* 14 mov rcx, [rsp+0x68] (the HUD, rsp+0x28 before) */
        0x44, 0x89, 0xE2,                         /* 19 mov edx, r12d (the instance) */
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,       /* 22 mov rax, HudRowOf (imm64 at 24) */
        0xFF, 0xD0,                               /* 32 call rax */
        0x0F, 0x10, 0x4C, 0x24, 0x20,             /* 34 movups xmm1, [rsp+0x20] */
        0x0F, 0x10, 0x54, 0x24, 0x30,             /* 39 movups xmm2, [rsp+0x30] */
        0x48, 0x83, 0xC4, 0x40,                   /* 44 add rsp, 0x40 */
        0x89, 0xC5,                               /* 48 mov ebp, eax (the row) */
        0x48, 0xC1, 0xE8, 0x20,                   /* 50 shr rax, 32 */
        0x89, 0xC6,                               /* 54 mov esi, eax (the key) */
        0xE9, 0, 0, 0, 0};                        /* 56 jmp 0x17CFA36 (rel32 at 57) */
    _Static_assert(sizeof stub == 61, "the HUD row search stub ends with its jump");
    memcpy(code, stub, sizeof stub);
    uint64_t to = (uint64_t)(uintptr_t)HudRowOf; memcpy(code + 24, &to, 8);
    int32_t rel = (int32_t)((intptr_t)(game + HUD_FIND_BACK) - (intptr_t)(code + sizeof stub)); memcpy(code + 57, &rel, 4);
    return 1;
}
static int HudGroupStub(uintptr_t game, uint8_t* code) /* before the block is made executable */
{
    if (!ApiCodeMatches(game + HUD_LIST_SITE, kHudListPin) || !ApiCodeMatches(game + HUD_LIST_BACK, kHudBackPin) ||
        !ApiCodeMatches(game + HUD_ROW_ADD, kHudRowAddPin) || !ApiCodeMatches(game + HUD_ROW_STATE, kHudRowStatePin))
    { Note("difficulty: HUD: the objective list is not the reviewed code; one mission's main objectives and steps"); return 0; }
    /* mov rcx, r15 (the HUD); mov rax, HudObjectiveRows; call rax; jmp 0x17CF79A (rsp is 16-aligned here, its home area free) */
    uint8_t stub[3 + 10 + 2 + 5] = {0x4C, 0x89, 0xF9, 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0, 0xE9, 0, 0, 0, 0};
    _Static_assert(sizeof stub == 20, "the HUD stub ends with its jump");
    uint64_t to = (uint64_t)(uintptr_t)HudObjectiveRows;
    memcpy(stub + 5, &to, 8);
    int32_t rel = (int32_t)((intptr_t)(game + HUD_LIST_BACK) - (intptr_t)(code + sizeof stub)); memcpy(stub + 16, &rel, 4);
    memcpy(code, stub, sizeof stub);
    if (!HudFindStub(game, code + 0x40)) { Note("difficulty: HUD: the objective rows' update is not the reviewed code; one mission's main objectives and steps"); return 0; }
    return code[13] == 0xFF && code[15] == 0xE9;
}
static void HudGroupSite(uintptr_t game, uint8_t* code)
{
    if (LoaderHook("the HUD's grouped objective rows", game + HUD_ROW_STATE, (void*)HudRowState, (void**)&g_hudRowState) != 0) return;
    uint8_t jump[5] = {0xE9};
    int32_t rel = DifficultyJumpRel(game + HUD_LIST_SITE, (uintptr_t)code); memcpy(jump + 1, &rel, 4);
    uint8_t find[43]; memset(find, 0x90, sizeof find); find[0] = 0xE9;
    rel = DifficultyJumpRel(game + HUD_FIND_SITE, (uintptr_t)(code + 0x40)); memcpy(find + 1, &rel, 4);
    const uint8_t orFlag = 0x08; /* or byte [rsp+rax+0x30], cl: a row is near when any of its copies is */
    if (ApiWrite(game + HUD_FIND_SITE, find, sizeof find) || ApiWrite(game + HUD_FLAG_SITE, &orFlag, 1) ||
        ApiWrite(game + HUD_LIST_SITE, jump, sizeof jump)) { Note("difficulty: HUD: the objective list could not be patched"); return; }
    g_hudGrouped = 1;
    Note("difficulty: HUD: main objective and step rows are one per objective (16 rows), each row's state and near mark over its copies");
}

/* Op 0x16 places an objective only while the map's records of its template item stay under item+0x15C (u8; 0xF8FC87,
 * then cmp r8d, ecx; jae refuses). With every mission's copies (ICBM x9 on a city map) that is the missions x the cap,
 * as for points of interest and satellites (CapStubs): capMul is the missions while op 0x16 runs (WorldDispatch). */
#define CAP_SITE_OBJ 0xF8FC87u
static const uint8_t kCapObj[20] = {
    0x0F, 0xB6, 0x8C, 0x08, 0x5C, 0x01, 0x00, 0x00, /* movzx ecx, byte [rax+rcx+0x15C] */
    0x0F, 0xAF, 0x0D, 0, 0, 0, 0,                   /* imul ecx, [capMul]         @11 */
    0xE9, 0, 0, 0, 0};                              /* jmp 0xF8FC8F               @16 */
static const char* ObjCapStub(uintptr_t game, uint8_t* code)
{
    if (!ApiCodeMatches(game + CAP_SITE_OBJ, "0F B6 8C 08 5C 01 00 00 44 3B C1 0F 83")) return "op 0x16's template caps are not the reviewed code";
    memcpy(code, kCapObj, sizeof kCapObj);
    int32_t rel = (int32_t)((intptr_t)&g_world.values->capMul - (intptr_t)(code + 15)); memcpy(code + 11, &rel, 4);
    rel = (int32_t)((intptr_t)(game + CAP_SITE_OBJ + 8) - (intptr_t)(code + 20)); memcpy(code + 16, &rel, 4);
    return NULL;
}
static const char* ObjCapSite(uintptr_t game, uint8_t* code)
{
    uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    int32_t rel = DifficultyJumpRel(game + CAP_SITE_OBJ, (uintptr_t)code); memcpy(jump + 1, &rel, 4);
    return ApiWrite(game + CAP_SITE_OBJ, jump, sizeof jump) ? "op 0x16's template caps could not be scaled" : NULL;
}

/* After op 0x16: each id on the list against the important locations it placed (records 0x108 at G+g_importantList,
 * count G+0x11A715C; op 0x16 writes kind 0xADC32FAA at +0x8, 0xF8FFF7, and the objective id at +0xD8, 0xF900DB). */
static uint32_t g_unplacedIds[64], g_unplacedCount; /* ids op 0x16 placed none of (WorldObjectiveUnplacedNote) */
static void WorldObjectivePlaced(uintptr_t block)
{
    uint32_t count = 0, ids[64], copies[64], placed[64], k = 0, listed = g_objectiveListCount, done = 0;
    g_unplacedCount = 0;
    if (listed == 0 || SafeRead(block + 0x11A715Cu, &count, 4)) return;
    for (uint32_t i = 0; i < listed; i++)
    {
        uint32_t j = 0;
        while (j < k && ids[j] != g_objectiveList[i]) j++;
        if (j == k) { ids[k] = g_objectiveList[i]; copies[k] = 0; placed[k] = 0; k++; }
        copies[j]++;
    }
    for (uint32_t l = 0; l < count && l < 1024; l++)
    {
        uint32_t kind = 0, id = 0;
        if (SafeRead(block + g_importantList + 0x108ull * l + 0x8, &kind, 4) || kind != 0xADC32FAAu ||
            SafeRead(block + g_importantList + 0x108ull * l + 0xD8, &id, 4)) continue;
        for (uint32_t j = 0; j < k; j++) if (ids[j] == id) { placed[j]++; done++; break; }
    }
    for (uint32_t j = 0; j < k; j++) if (placed[j] == 0 && g_unplacedCount < 64) g_unplacedIds[g_unplacedCount++] = ids[j];
    char text[1600];
    int at = snprintf(text, sizeof text, "difficulty: an operation's missions on one map: op 0x16 placed %u of the list's %u (id copies -> placed):", done, listed);
    for (uint32_t j = 0; j < k && at > 0 && at < (int)sizeof text - 32; j++)
        at += snprintf(text + at, sizeof text - at, " %08x %u->%u", ids[j], copies[j], placed[j]);
    Note("%s", text);
}

/* GenerateLocationClusters (op 0x18, 0xF79520; rcx = its context: clusters [+0x20] (0xE0 each, centre xy at +0/+4,
 * class byte +0xD8), count [[+0x38]]; r8 = the pass: S = [+8], world size u32 S+0x2C, playable fraction S+0x50) puts
 * every cluster on one ring: radius lerp(R+0x14, R+0x18, one draw) x the playable radius W/2 x S+0x50 (0xF799B1..
 * 0xF79A27), slots 2pi/n apart from a random start (0xF79D36..0xF79E5E), and at most one at the exact centre. Every
 * objective, outpost and extraction is later placed at the nearest free point to its cluster's centre (SpreadLocations
 * 0xF7C310 reads only loc+0xE0 -> the centre), so on a 16 km map an operation's content sat in blobs on a ring 4.7-6.2 km
 * out with the middle and the rim empty. On an extra large map each ring cluster keeps its direction from the centre
 * and takes its own distance instead: ordered by direction, the k-th gets the (k * step mod n)-th of n equal-area
 * bands between 10 % and 88 % of the playable radius (step coprime to n, near n x 0.618, so neighbours differ). No new
 * draws and no trigonometry: every peer gets the same centres. The centre cluster stays. */
WORLD_PASS(ClustersTimed, 0xF79520u, "GenerateLocationClusters")
/* The pass also makes at least ceil(locations of a kind / N) clusters (difficulty_block.c's count stubs read {N - 1,
 * shift}): 24 per cluster elsewhere, 6 on an extra large level, so an operation's content is many small groups rather
 * than four large ones (s73i: 64 locations in 4 clusters). The lock keeps a level's N for its whole pass. */
static volatile uint32_t* g_clusterPer;
static SRWLOCK g_clusterLock = SRWLOCK_INIT;
static float ClusterDirection(float dx, float dy) /* a monotonic stand-in for the angle, 0..4 */
{
    float d = fabsf(dx) + fabsf(dy);
    float p = dy / d;
    return dx >= 0 ? (dy >= 0 ? p : 4.0f + p) : 2.0f - p;
}
static void NatureClusters(uint64_t pass, uint64_t clusters, uint32_t n); /* nature variants (c), below */
/* One objective a cluster (audit 15 criterion 4, static obj17). Op 0x16 only writes a placeholder; op 0x18 hands each
 * objective to the same-class cluster with the smallest radius sum (0xF78A80) and SpreadLocations (0x8B) puts every
 * member at the first free point nearest its cluster's centre, at the minimum gap (0xF7B2E0): on s76d 16 of 25 kind-8
 * objectives shared 8 light clusters, 66-240 m apart. After the pass, every cluster keeps one objective (its main,
 * kind 7, else its first) and the others move, with their mode-1/3 kind-0xB children (0xF7A0F0: those follow the
 * parent's cluster), to a cluster of their class with no objective (the one farthest from the clusters holding one),
 * else to a new cluster at the emptiest point of the playable band, while the list has room. The rules are op 0x18's
 * own: class loc+0xF0 == cluster+0xD8, 24 members, one extraction-type member a cluster (0xF789E0: kind 6 or
 * D+0x160 bit 0x80), the radius sum +0xD4 = sum of D+0xD8 (0xF78F00). Op 0x18 makes a cluster by its class byte alone in
 * zeroed room (0xF7996E) and later writes its centre, members, count and sum: a new one is just that. No draws: every
 * peer gets the same clusters. Records: members +0x10 (u64 x 24), count +0xD0, loc+0xE0 = the cluster's address. */
static uint64_t ClusterDef(uint64_t rec) /* 0xF78E2F..0xF78E7F */
{
    uint8_t a1 = 0xFF, a2 = 0xFF; uint32_t kind = 0; uint64_t h = 0, t = 0, base = 0, d = 0;
    if (SafeRead(rec + 0xA2, &a2, 1) || a2 == 0xFF || SafeRead(rec + 8, &kind, 4) || SafeRead(rec, &h, 8) || h == 0) return 0;
    if (kind == 0xADC32FAAu)
    {
        if (SafeRead(rec + 0xA1, &a1, 1) || a1 == 0xFF || SafeRead(h + 0x28, &t, 8) || t == 0 || SafeRead(t + (uint64_t)a1 * 24 + 8, &base, 8) || base == 0) return 0;
        return base + (uint64_t)a2 * 0x1C8;
    }
    return SafeRead(h, &d, 8) ? 0 : d;
}
static int ClusterExtraction(uint64_t rec) /* 0xF78A4D..0xF78A5D */
{
    uint8_t kind = 0, flags = 0; uint64_t d = ClusterDef(rec);
    if (!SafeRead(rec + 0xDC, &kind, 1) && kind == 6) return 1;
    return d != 0 && !SafeRead(d + 0x160, &flags, 1) && (flags & 0x80);
}
static int ClusterObjective(uint64_t rec)
{
    uint32_t kind = 0; uint8_t k = 0;
    return !SafeRead(rec + 8, &kind, 4) && kind == 0xADC32FAAu && !SafeRead(rec + 0xDC, &k, 1) && (k == 7 || k == 8 || k == 9);
}
static uint32_t ClusterObjectives(uint64_t cl)
{
    uint32_t count = 0, objectives = 0; uint64_t m[24];
    if (SafeRead(cl + 0xD0, &count, 4) || count > 24 || SafeRead(cl + 0x10, m, 8ull * count)) return 0;
    for (uint32_t i = 0; i < count; i++) objectives += (uint32_t)ClusterObjective(m[i]);
    return objectives;
}
static int ClusterTakes(uint64_t cl, uint8_t cls, int extraction, uint32_t members)
{
    uint32_t count = 0; uint8_t c = 0; uint64_t m[24];
    if (SafeRead(cl + 0xD0, &count, 4) || count + members > 24 || SafeRead(cl + 0xD8, &c, 1) || c != cls || SafeRead(cl + 0x10, m, 8ull * count)) return 0;
    if (extraction) for (uint32_t i = 0; i < count; i++) if (ClusterExtraction(m[i])) return 0;
    return 1;
}
static void ClusterMove(uint64_t rec, uint64_t from, uint64_t to)
{
    uint32_t count = 0, j = 0; uint64_t m[24]; float sum = 0, r = 0; uint64_t d = ClusterDef(rec);
    if (d == 0 || SafeRead(d + 0xD8, &r, 4) || !isfinite(r)) r = 0;
    SafeRead(from + 0xD0, &count, 4); SafeRead(from + 0x10, m, 8ull * (count > 24 ? 24 : count));
    for (uint32_t i = 0; i < count && i < 24; i++) if (m[i] != rec) m[j++] = m[i];
    for (uint32_t i = j; i < count && i < 24; i++) m[i] = 0;
    memcpy((void*)(uintptr_t)(from + 0x10), m, 8ull * (count > 24 ? 24 : count));
    *(volatile uint32_t*)(uintptr_t)(from + 0xD0) = j;
    SafeRead(from + 0xD4, &sum, 4); *(volatile float*)(uintptr_t)(from + 0xD4) = sum - r;
    SafeRead(to + 0xD0, &count, 4);
    *(volatile uint64_t*)(uintptr_t)(to + 0x10 + 8ull * count) = rec;
    *(volatile uint32_t*)(uintptr_t)(to + 0xD0) = count + 1;
    SafeRead(to + 0xD4, &sum, 4); *(volatile float*)(uintptr_t)(to + 0xD4) = sum + r;
    *(volatile uint64_t*)(uintptr_t)(rec + 0xE0) = to;
}
static void ObjectiveSpread(uint64_t a, uint64_t clusters, uint32_t* n, float centre, float play)
{
    uint64_t countAt = 0; uint32_t cap = 0;
    if (SafeRead(a + 0x38, &countAt, 8) || countAt == 0 || SafeRead(a + 0x40, &cap, 4)) return;
    if (cap > 64) cap = 64;
    uint32_t sharedBefore = 0, mostBefore = 0, moved = 0, children = 0, made = 0, kept = 0;
    for (uint32_t i = 0; i < *n; i++) { uint32_t o = ClusterObjectives(clusters + 0xE0ull * i); if (o > 1) sharedBefore++; if (o > mostBefore) mostBefore = o; }
    for (uint32_t i = 0; i < *n; i++)
    {
        uint64_t from = clusters + 0xE0ull * i;
        uint32_t count = 0; uint64_t m[24];
        if (SafeRead(from + 0xD0, &count, 4) || count > 24 || SafeRead(from + 0x10, m, 8ull * count)) continue;
        uint64_t keep = 0;
        for (uint32_t k = 0; k < count && keep == 0; k++) { uint8_t kind = 0; if (ClusterObjective(m[k]) && !SafeRead(m[k] + 0xDC, &kind, 1) && kind == 7) keep = m[k]; }
        for (uint32_t k = 0; k < count && keep == 0; k++) if (ClusterObjective(m[k])) keep = m[k];
        for (uint32_t k = 0; k < count; k++)
        {
            uint64_t rec = m[k]; uint8_t cls = 0;
            if (rec == keep || !ClusterObjective(rec) || SafeRead(rec + 0xF0, &cls, 1)) continue;
            uint64_t kids[24]; uint32_t nk = 0;
            for (uint32_t q = 0; q < count; q++)
            {
                uint8_t kind = 0, mode = 0; uint64_t parent = 0;
                if (!SafeRead(m[q] + 0xDC, &kind, 1) && kind == 0xB && !SafeRead(m[q] + 0xF8, &parent, 8) && parent == rec &&
                    !SafeRead(m[q] + 0x100, &mode, 1) && (mode == 1 || mode == 3)) kids[nk++] = m[q];
            }
            int extraction = ClusterExtraction(rec);
            /* an existing cluster of the class without an objective, farthest from those holding one */
            uint64_t to = 0; float best = -1.0f;
            for (uint32_t t = 0; t < *n; t++)
            {
                uint64_t cl = clusters + 0xE0ull * t; float xy[2];
                if (t == i || ClusterObjectives(cl) != 0 || !ClusterTakes(cl, cls, extraction, 1 + nk) || SafeRead(cl, xy, 8)) continue;
                float closest2 = 1e30f;
                for (uint32_t u = 0; u < *n; u++)
                {
                    float uv[2]; uint64_t cu = clusters + 0xE0ull * u;
                    if (u == t || ClusterObjectives(cu) == 0 || SafeRead(cu, uv, 8)) continue;
                    float dd = (xy[0] - uv[0]) * (xy[0] - uv[0]) + (xy[1] - uv[1]) * (xy[1] - uv[1]);
                    if (dd < closest2) closest2 = dd;
                }
                if (closest2 > best) { best = closest2; to = cl; }
            }
            if (to == 0 && *n < cap) /* a new cluster at the emptiest point of the band 10..88 % of the playable radius */
            {
                float bx = centre, by = centre, bd = -1.0f;
                for (int gy = 0; gy < 64; gy++)
                    for (int gx = 0; gx < 64; gx++)
                    {
                        float x = (gx + 0.5f) * (2.0f * centre) / 64.0f, y = (gy + 0.5f) * (2.0f * centre) / 64.0f;
                        float dr = sqrtf((x - centre) * (x - centre) + (y - centre) * (y - centre));
                        if (dr < 0.10f * play || dr > 0.88f * play) continue;
                        float closest2 = 1e30f;
                        for (uint32_t u = 0; u < *n; u++)
                        {
                            float uv[2];
                            if (SafeRead(clusters + 0xE0ull * u, uv, 8)) continue;
                            float dd = (x - uv[0]) * (x - uv[0]) + (y - uv[1]) * (y - uv[1]);
                            if (dd < closest2) closest2 = dd;
                        }
                        if (closest2 > bd) { bd = closest2; bx = x; by = y; }
                    }
                to = clusters + 0xE0ull * *n;
                memset((void*)(uintptr_t)to, 0, 0xE0);
                float xy[2] = {bx, by};
                memcpy((void*)(uintptr_t)to, xy, 8);
                *(volatile uint8_t*)(uintptr_t)(to + 0xD8) = cls;
                (*n)++; *(volatile uint32_t*)(uintptr_t)countAt = *n; made++;
            }
            if (to == 0) { kept++; continue; }
            ClusterMove(rec, from, to); moved++;
            for (uint32_t q = 0; q < nk; q++) { ClusterMove(kids[q], from, to); children++; }
        }
    }
    uint32_t sharedAfter = 0, mostAfter = 0;
    for (uint32_t i = 0; i < *n; i++) { uint32_t o = ClusterObjectives(clusters + 0xE0ull * i); if (o > 1) sharedAfter++; if (o > mostAfter) mostAfter = o; }
    static LONG noted;
    if (InterlockedIncrement(&noted) <= 4)
        Note("difficulty: extra large maps: one objective a cluster: %u clusters shared objectives (at most %u) -> %u (at most %u); %u objectives and %u children moved, %u new clusters (%u of %u), %u left sharing",
            sharedBefore, mostBefore, sharedAfter, mostAfter, moved, children, made, *n, cap, kept);
}

static uint64_t WorldPassClusters(uint64_t a, uint64_t b, uint64_t c, uint64_t e)
{
    AcquireSRWLockExclusive(&g_clusterLock);
    int small = g_clusterPer != NULL && g_world.current > 1.0f;
    /* 3 locations a cluster on an extra large level (difficulty_block.c: ceil(n / 3) by 0xAAAAAAAB >> 33; 6 left the
     * content in about a dozen lumps over the map, audit 13), within DIF_CLUSTERS */
    if (small) { g_clusterPer[0] = 2; g_clusterPer[1] = 33; }
    uint64_t r = WorldPassClustersTimed(a, b, c, e);
    if (small) { g_clusterPer[0] = 23; g_clusterPer[1] = 36; }
    ReleaseSRWLockExclusive(&g_clusterLock);
    uint64_t clusters = 0, countAt = 0, S = 0;
    uint32_t n = 0, W = 0;
    float fraction = 0;
    if (!(g_world.current > 1.0f) || SafeRead(a + 0x20, &clusters, 8) || clusters == 0 || SafeRead(a + 0x38, &countAt, 8) || countAt == 0 ||
        SafeRead(countAt, &n, 4) || n < 2 || n > 64 || SafeRead(c + 8, &S, 8) || S == 0 || SafeRead(S + 0x2C, &W, 4) || SafeRead(S + 0x50, &fraction, 4) ||
        !(fraction > 0.0f && fraction <= 1.0f) || W < 4096)
        return r;
    const float centre = 0.5f * (float)W, play = centre * fraction;
    uint32_t ring[64], m = 0;
    float dir[64], dist[64];
    for (uint32_t i = 0; i < n; i++)
    {
        float xy[2];
        if (SafeRead(clusters + 0xE0ull * i, xy, 8) || !isfinite(xy[0]) || !isfinite(xy[1])) return r;
        float dx = xy[0] - centre, dy = xy[1] - centre, d = sqrtf(dx * dx + dy * dy);
        if (d < 1.0f) continue; /* the centre cluster */
        uint32_t k = m++;
        dir[k] = ClusterDirection(dx, dy); dist[k] = d; ring[k] = i;
        while (k > 0 && dir[k - 1] > dir[k]) /* insertion sort by direction */
        {
            float t = dir[k]; dir[k] = dir[k - 1]; dir[k - 1] = t;
            t = dist[k]; dist[k] = dist[k - 1]; dist[k - 1] = t;
            uint32_t u = ring[k]; ring[k] = ring[k - 1]; ring[k - 1] = u;
            k--;
        }
    }
    if (m < 2) return r;
    uint32_t step = (uint32_t)((float)m * 0.618034f + 0.5f);
    if (step < 1) step = 1;
    for (;;) /* coprime to m */
    {
        uint32_t x = step, y = m;
        while (y) { uint32_t t = x % y; x = y; y = t; }
        if (x == 1) break;
        step++;
    }
    const float lo = 0.10f, hi = 0.88f;
    char list[64 * 12 + 1];
    int at = 0;
    for (uint32_t k = 0; k < m; k++)
    {
        float band = ((float)((k * step) % m) + 0.5f) / (float)m;
        float want = play * sqrtf(lo * lo + (hi * hi - lo * lo) * band);
        float xy[2];
        uint64_t centreAt = clusters + 0xE0ull * ring[k];
        if (SafeRead(centreAt, xy, 8)) return r;
        float scale = want / dist[k];
        xy[0] = centre + (xy[0] - centre) * scale;
        xy[1] = centre + (xy[1] - centre) * scale;
        memcpy((void*)(uintptr_t)centreAt, xy, 8);
        if (at < (int)sizeof list - 12) at += snprintf(list + at, sizeof list - at, " %.0f", (double)want);
    }
    static LONG noted;
    if (InterlockedIncrement(&noted) <= 4)
        Note("difficulty: extra large maps: %u location clusters spread over the map (%u on the ring at %.0f m; playable %.0f m), their distances:%s",
            n, m, (double)dist[0], (double)play, list);
    ObjectiveSpread(a, clusters, &n, centre, play);
    NatureClusters(c, clusters, n);
    return r;
}
WORLD_PASS(Voronoi, 0xF3F8E0u, "GenerateVoronoi")
/* The step dispatcher (0xC14950: rcx the pass records, 0x19B0 each, op at +0; r8 the current index; two stack arguments):
 * every op of an extra large level that adds locations or takes over 100 ms. */
typedef uint64_t (*WorldDispatchFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static WorldDispatchFn g_worldDispatch;
/* Bumped at every level init and whenever an extra large level's dispatcher moves to another pass record: the stamp
 * overlap grid (difficulty_world_overlap.c) keeps its per-buffer grids for one run of a pass only, so a later op 0x2B
 * whose arena buffer lands at the same address starts from an empty grid. */
static volatile LONG g_worldOpRun;
/* For the hang monitor (crash_notes.c): every dispatcher call on any level, the op of the record it runs and since when
 * (GetTickCount64), so a game window that stops answering while the generator moves on reads as busy, not stuck. */
static volatile LONG g_genCalls;
static volatile LONG g_genOp = -1, g_genRecord = -1;
static volatile LONG64 g_genOpSince;
static uint32_t g_worldOpAt = 0xFFFFFFFFu;
static void OverlapFlush(void); /* difficulty_world_overlap.c: notes the stamp overlap grid's counters for the run just ended */
static float* g_heightCopy; /* the layer heightfield the loader keeps (below) */
static uint64_t g_heightSource, g_heightBytes;
static volatile LONG g_worldStampNotes;
/* Research (the smooth 16 km terrain): the kept layer heightfield written to the session's logs once per level, when the
 * dispatcher reaches CullUnitsOutsidePlayableArea (op 0x6D, after the layers' heights are final), on any level, so a
 * vanilla map gives a file to compare with. heightfield-<n>.f32 holds the floats row by row; heightfield-<n>.txt its
 * descriptor (origin, extent, columns, rows) and the level's difficulty and world scale. */
static uint8_t g_heightField[0x28];
static LONG g_heightDumped = -1;
static volatile LONG g_heightDumps;
static void WorldDumpHeight(void)
{
    if (g_heightCopy == NULL || g_heightBytes == 0 || g_heightDumped == g_world.levels) return;
    g_heightDumped = g_world.levels;
    LONG n = InterlockedIncrement(&g_heightDumps);
    if (n > 6) return;
    float origin[2], extent[2];
    uint32_t dims[2];
    memcpy(origin, g_heightField + 8, 8); memcpy(extent, g_heightField + 0x14, 8); memcpy(dims, g_heightField + 0x1C, 8);
    uint8_t difficulty = 0;
    if (g_worldLevel != 0) SafeRead(g_worldLevel + 0xF9, &difficulty, 1);
    wchar_t name[64];
    swprintf(name, 64, L"heightfield-%ld.f32", (long)n);
    HANDLE file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    BOOL ok = WriteFile(file, g_heightCopy, (DWORD)g_heightBytes, &wrote, NULL);
    CloseHandle(file);
    swprintf(name, 64, L"heightfield-%ld.txt", (long)n);
    file = OpenLog(name);
    if (file != INVALID_HANDLE_VALUE)
    {
        char text[256];
        int len = snprintf(text, sizeof text, "origin %g %g\nextent %g %g\ncolumns %u\nrows %u\ndifficulty %u\nworld_scale %g\n",
            (double)origin[0], (double)origin[1], (double)extent[0], (double)extent[1], dims[0], dims[1], (unsigned)difficulty, (double)g_world.current);
        WriteFile(file, text, (DWORD)len, &wrote, NULL);
        CloseHandle(file);
    }
    Note("difficulty: extra large maps: wrote heightfield-%ld (%u x %u, %s) for difficulty %u at %.0fx", (long)n, dims[0], dims[1],
        ok ? "complete" : "failed", (unsigned)difficulty, (double)g_world.current);
}

/* Research (terrain resolution): after a terrain pass (SetupTerrainMerge 0x38, RenderHeightmapVoronoi 0x3C, SmoothHeightmap
 * 0x40, RenderZoneHeightmap 0x3D, CaptureHeightmapFinalize 0x5E, SpawnTerrainWork 0x6A) on the first two levels: its pass
 * record and the memory its first pointers reach, hex, into terrain-probe-<level>.txt, with the engine functions the
 * passes call through [game+0x3326308] (+0x10 table +0x190/+0x1D0/+0x1D8/+0x218/+0x470, +8 table +0x1A8) and every
 * dword 4096 / float 4096 / float 0.5 found. */
static int ModuleAt(uintptr_t at, char* out, size_t size); /* crash_notes.c */
static void ProbeHex(HANDLE file, const char* title, uint64_t at, uint32_t bytes)
{
    char line[256];
    DWORD wrote;
    int n = snprintf(line, sizeof line, "%s at 0x%llX (%u bytes)\n", title, (unsigned long long)at, bytes);
    WriteFile(file, line, (DWORD)n, &wrote, NULL);
    for (uint32_t o = 0; o < bytes; o += 32)
    {
        uint8_t row[32];
        if (SafeRead(at + o, row, 32)) { n = snprintf(line, sizeof line, "  +%04X unreadable\n", o); WriteFile(file, line, (DWORD)n, &wrote, NULL); break; }
        n = snprintf(line, sizeof line, "  +%04X", o);
        for (int k = 0; k < 32; k += 4) { uint32_t v; memcpy(&v, row + k, 4); n += snprintf(line + n, sizeof line - n, " %08x", v); }
        for (int k = 0; k < 32; k += 4)
        {
            uint32_t v; float fv; memcpy(&v, row + k, 4); memcpy(&fv, row + k, 4);
            if (v == 4096u || v == 2048u || v == 8192u || v == 16384u || fv == 4096.0f || fv == 2048.0f || fv == 16384.0f || fv == 512.0f || fv == 0.5f || fv == 4.0f)
                n += snprintf(line + n, sizeof line - n, " [+%X=%s%g]", o + k, (v >= 512u && v <= 16384u) ? "u" : "f", (v >= 512u && v <= 16384u) ? (double)v : (double)fv);
        }
        line[n++] = '\n';
        WriteFile(file, line, (DWORD)n, &wrote, NULL);
    }
}

/* Research (the zone grid, KB patrol-destination-behaviour): G = gen + 0x231C18 holds 96 x 96 cells of up to 24 zone
 * seeds (u32 values at G + 4i, positions f32x2 at G + 0xD8000 + 8i, i = cell x 24 + k; u16 counts at G + 0x288000;
 * the grid's size f32 at G + 0x28C800). Once per level, at op 0x6D: total seeds, cells used, the fullest cell, a count
 * histogram, distinct values and the mean spacing, so the grid can be sized for a larger world. */
static LONG g_zoneNoted = -1;
static void WorldGridStats(uintptr_t gen, const char* label)
{
    uintptr_t grid = gen + 0x231C18;
    static uint16_t counts[96 * 96];
    float size = 0.0f;
    if (SafeRead(grid + 0x288000, counts, sizeof counts) || SafeRead(grid + 0x28C800, &size, 4)) return;
    uint32_t total = 0, used = 0, fullest = 0, histogram[26] = {0};
    for (int c = 0; c < 96 * 96; c++)
    {
        uint32_t n = counts[c];
        total += n; if (n) used++; if (n > fullest) fullest = n;
        histogram[n > 25 ? 25 : n]++;
    }
    static uint32_t values[96 * 96 * 24];
    uint32_t distinct = 0;
    if (!SafeRead(grid, values, sizeof values))
    {
        static uint8_t seen[65536];
        memset(seen, 0, sizeof seen);
        for (int c = 0; c < 96 * 96; c++)
            for (uint32_t k = 0; k < counts[c] && k < 24; k++)
            {
                uint32_t v = values[c * 24 + k] & 0xFFFF;
                if (!seen[v]) { seen[v] = 1; distinct++; }
            }
    }
    char text[256];
    int n = 0;
    for (int k = 0; k <= 25 && n < (int)sizeof text - 16; k++) if (histogram[k]) n += snprintf(text + n, sizeof text - n, " %d:%u", k, histogram[k]);
    Note("difficulty: zone grid (%s) at %.0fx: size %.0f m (cells of %.1f m), %u seeds in %u of 9216 cells, fullest %u, %u distinct values (%.0f m between seeds); seeds per cell:%s",
        label, (double)g_world.current, (double)size, (double)(size / 96.0f), total, used, fullest, distinct,
        total ? (double)size / sqrt((double)total) : 0.0, text);
}

static void WorldZoneGridNote(void)
{
    if (g_zoneNoted == g_world.levels || g_worldLevel == 0) return;
    g_zoneNoted = g_world.levels;
    uintptr_t gen = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0) return;
    WorldGridStats(gen, "level");
}

/* Terrain tiles, step 1 (research): one tile's Voronoi at vanilla density, built by the game's own GenerateVoronoi
 * (op 0x25, 0xF3F8E0) on a copy of the generator block, so the level's plan is untouched. The tile runs as a small
 * world in tile-local coordinates: for the call the world size ([settings+0x2C]) is the tile's extent, the lattice box
 * (settings +0x64/+0x68, square) is [0, extent], the keep radius (+0x6C) covers the box, and the lattice spacing
 * ([[[context+0x18]+0x2D0]+0x68]) is 16 m; everything is put back after. Pass records point into the block, so the
 * record is copied and every pointer into the block moved onto the copy. Handlers are called as the dispatcher does:
 * (record + 8, record, context). Runs once per level as SetupTerrainMerge (op 0x38) begins (see WorldDispatch).
 * HELLFORGE_XL_TILE=<extent in m> (256..2048) turns it on; 768 keeps the route cells within the game's arrays. */
#define GEN_BLOCK_SIZE_VANILLA 0x11A7268u
static uint32_t WorldBlockSize(void); /* difficulty_block.c: the enlarged block of a larger map */
#define GEN_BLOCK_SIZE (g_world.current > 1.0f ? WorldBlockSize() : GEN_BLOCK_SIZE_VANILLA)
static uint64_t g_rec25, g_tileContext;
static LONG g_tileDone = -1;
static uint8_t* g_tileBlock;

static void WorldTileRelocateIn(uint8_t* data, size_t bytes, uintptr_t from, uintptr_t to)
{
    uint32_t size = GEN_BLOCK_SIZE;
    for (size_t o = 0; o + 8 <= bytes; o += 8)
    {
        uint64_t v;
        memcpy(&v, data + o, 8);
        if (v >= from && v < from + size) { v = v - from + to; memcpy(data + o, &v, 8); }
    }
}

static void WorldTileRelocate(uint8_t* record, uintptr_t from, uintptr_t to)
{
    for (uint32_t o = 0; o + 8 <= 0x19B0u; o += 8)
    {
        uint64_t v;
        memcpy(&v, record + o, 8);
        if (v >= from && v < from + GEN_BLOCK_SIZE) { v = v - from + to; memcpy(record + o, &v, 8); }
    }
}

/* Research (terrain tiles): which parts of the generator block each pass reads and writes. Once per level at op 0x6D,
 * records-<level>.txt lists every pass record up to the pipeline's end with its op and each field pointing into the
 * block (record offset -> block offset), so a tile run knows which coarse arrays it inherits. */
static LONG g_recordsMapped = -1;
static void WorldRecordMap(uint64_t records, uint32_t end)
{
    if (g_recordsMapped == g_world.levels || g_worldLevel == 0 || end == 0 || end > 1024) return;
    g_recordsMapped = g_world.levels;
    uintptr_t gen = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0) return;
    wchar_t name[64];
    swprintf(name, 64, L"records-%ld.txt", (long)g_world.levels);
    HANDLE file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return;
    static uint8_t record[0x19B0];
    char line[4096];
    DWORD wrote;
    int n = snprintf(line, sizeof line, "generator block 0x%llX (0x%X bytes), %u records\n", (unsigned long long)gen, GEN_BLOCK_SIZE, end);
    WriteFile(file, line, (DWORD)n, &wrote, NULL);
    for (uint32_t i = 0; i < end; i++)
    {
        if (SafeRead(records + (uint64_t)i * 0x19B0u, record, sizeof record)) break;
        uint32_t op;
        memcpy(&op, record, 4);
        n = snprintf(line, sizeof line, "%u op 0x%02X:", i, op);
        for (uint32_t o = 8; o + 8 <= 0x19B0u && n < (int)sizeof line - 40; o += 8)
        {
            uint64_t v;
            memcpy(&v, record + o, 8);
            if (v >= gen && v < gen + GEN_BLOCK_SIZE) n += snprintf(line + n, sizeof line - n, " +%X=G+%llX", o, (unsigned long long)(v - gen));
        }
        n += snprintf(line + n, sizeof line - n, "\n");
        WriteFile(file, line, (DWORD)n, &wrote, NULL);
    }
    CloseHandle(file);
}

/* Step 2: after the tile's Voronoi, the passes that turn route cells into heights run on the copy, through the original
 * dispatcher on a one-record array (index 0, end 1; an op that is not finished leaves the index at 0 and is called
 * again). Only these ops run, in pipeline order from op 0x25 to the first CalculateCornerSlopes (0x2D) after
 * GenerateSubregionHeights (0x04); location placement, packages, collision, stamps and the merge are left out, and the
 * copy's location counts are zero, so the tile is the plan's nature terrain. The tile's Voronoi vertices (position and
 * height) go to tile-vertices-<level>.f32 as x, y, h floats. */
/* FixUnreachableAreas (0x7D) is left out: reachability is a property of the whole map (a tile's border areas connect
 * through its neighbours), so the global plan carries it. Run on a tile it also faulted on 2 of 6 maps (no neighbour
 * region for an area: a write to element (uint32)-1 at 0xF674A5). */
static const uint8_t kTileOps[] = {0x07, 0x12, 0x75, 0x79, 0x7B, 0x76, 0x74, 0x77, 0x78, 0x11, 0x7C, 0x28, 0xA4, 0x7A, 0x7E,
    0x08, 0x29, 0x2A, 0xB6, 0x4E, 0x13, 0x92, 0x0E, 0x05, 0x02, 0x03, 0x04, 0x2C, 0x2E, 0x2D};
/* GenerateBottomFlatLookup (0x93) is left out: it renders on the GPU and waits for the readback, which needs frames to
 * pass; a tile without stamps has nothing for it. GPU passes need the tile run spread over frames. */

static int WorldTileRuns(uint32_t op)
{
    for (size_t k = 0; k < sizeof kTileOps; k++) if (kTileOps[k] == op) return 1;
    return 0;
}

/* Research: route cells' associated-list ranges (+0xA8..+0xAC into the list at G+0x10CB8C0, count G+0x11A71EC; built by
 * BuildRouteCellAssociatedRoutes 0x02, read by GenerateSubregionHeights 0x04) and whether the entries point into the
 * block. Run on the tile's copy before 0x04 and on the level's block as a control. */
static void WorldTileListCheck(const uint8_t* block, uintptr_t base, uint32_t size, const char* label)
{
    uint32_t cells = 0, listCount = 0, badRange = 0, badEntry = 0, firstCell = 0, widest = 0;
    uint32_t firstRange[2] = {0, 0};
    uint64_t firstEntry = 0;
    memcpy(&cells, block + 0x11A71C0, 4); memcpy(&listCount, block + 0x11A71EC, 4);
    if (cells > 1024) cells = 1024;
    for (uint32_t c = 0; c < cells; c++)
    {
        uint32_t r[2];
        memcpy(r, block + 0x8EB328 + (size_t)c * 0x110 + 0xA8, 8);
        if (r[1] < r[0] || r[1] > listCount) { if (badRange++ == 0) { firstCell = c; firstRange[0] = r[0]; firstRange[1] = r[1]; } continue; }
        if (r[1] - r[0] > widest) widest = r[1] - r[0];
        for (uint32_t k = r[0]; k < r[1] && k < 0x10000; k++)
        {
            uint64_t v;
            memcpy(&v, block + 0x10CB8C0 + (size_t)k * 8, 8);
            if (!(v >= base && v < base + size) && badEntry++ == 0) firstEntry = v;
        }
    }
    Note("difficulty: terrain tile: list check (%s): %u route cells, list %u; %u bad ranges (first: cell %u: %u..%u), %u entries outside the block (first 0x%llX), widest %u",
        label, cells, listCount, badRange, firstCell, firstRange[0], firstRange[1], badEntry, (unsigned long long)firstEntry, widest);
}

/* A tile runs the game's passes on data the game never made, and a pass can take a path vanilla never reaches:
 * FixUnreachableAreas (0x7D) on one map found no neighbour for an area and wrote element (uint32)-1 at 0xF674A5. A
 * fault in a tile pass must cost the tile, not the game: the thread's context before the pass is kept, and a fault on
 * that thread while the pass runs resumes there with the thread's guard (TileGuard) holding the fault. The CPU passes work on the tile's copy of the
 * generator block only, so the level's data is untouched; the tile is abandoned and the level goes on. */
/* The guard's state is per thread: the stamp planning's runners (difficulty_world_terrain.c, TerrainStamps) run tile
 * ops on engine workers while the generator thread builds tiles. A runner's own state is in its TileRunner (found
 * through the TLS slot g_tileRunnerTls); every other thread shares g_tileGuardMain (one tile run at a time there). */
typedef struct TileGuard
{
    CONTEXT context;            /* the thread before the pass (16-byte aligned: first in the struct) */
    volatile DWORD thread;      /* the guarded thread while the pass runs */
    /* A tile's FinalizeVoronoi weld that failed on the tile's thread (difficulty_status.c's WeldFinal): the op returns
     * as done instead of asking for the level to be generated again (0xF68118: [lg+0xF954]++,
     * [[ctx+0x58]+0x19B0EA] = 1, read by the game's own loops, 0xF2606F and the level tick 0xF73FA6), and the tile's
     * runner abandons the tile. */
    volatile LONG weldFailed;
    volatile LONG fault;
    volatile uintptr_t at, data;
} TileGuard;

/* One runner of the parallel stamp planning: its guard, its op 0x2B context (a copy of the level's 0xE8 bytes with its
 * own arena +0, settings +8 and stamp index +0x20, as difficulty_world_move.c's MoveStart makes one), the arena's own
 * first chunk (difficulty_world_move.c MoveArena: {head chunk, lock 0, owner -1, growth 0x100000}, chunk {used, data,
 * capacity, next}; the reset 0x173BAB0 frees the chunks the allocator grew and empties ours), its stamp index and op
 * 0x2B's job regions. */
#define TILE_RUNNERS_MAX 16
#define TILE_RUNNER_ARENA (64u << 20)
#define TILE_JOB_REGION (16u << 20)  /* a job's records (0x130 each); a reserved page follows */
typedef struct TileRunner
{
    TileGuard guard;
    int id;
    uint8_t ctx[0xE8];
    uint8_t settings[0x120];    /* lg+0x2D20..0x2E3F (0x120: the build ops are not proven to stop at 0xD0) */
    struct { uint64_t head; LONG lock; int32_t owner; uint64_t growth; } arena;
    struct { uint64_t used, data, capacity, next; } chunk;
    uint8_t* header;            /* the stamp index's header (0x3000, in g_tileHeaders) */
    uint8_t* memory;            /* the index's memory */
    uint32_t memoryBytes;
    uint8_t* regions[8];        /* op 0x2B's jobs' records (TileJobsInline) */
    float scale;                /* the tile's rule scale (TileJobsInline's perm map; 1: none) */
    LONG overlapGeneration;     /* the stamp overlap grid's run for this runner's current tile */
    uint64_t emptyQueries;      /* difficulty_maps.c IndexQueryClamped: queries of its empty index answered at once */
    int decommitted;            /* TileRunnersRelease ran: commit again before use */
    volatile LONG where, op, tile; /* diagnostics: what the runner does now (TILE_AT_*), the op, the tile */
    uint8_t* block;             /* the generator block this runner builds a tile in (TerrainBlockSelf) */
    uint8_t* moved;             /* TerrainBuild's segment bitmap (a bit per 8 bytes of the block) */
    float offset[2];            /* the samplers' world offset while it builds (TerrainOffsetSelf) */
    int offsetOn;
    uint8_t one[0x19B0];
} TileRunner;
enum { TILE_AT_IDLE, TILE_AT_BUILD, TILE_AT_VORONOI, TILE_AT_OP, TILE_AT_SORT_WAIT, TILE_AT_IMAGE_WAIT, TILE_AT_MOVE, TILE_AT_PREPARE,
       TILE_AT_SLOT_WAIT, TILE_AT_2B, TILE_AT_ZONES };
static DWORD g_tileRunnerTls = TLS_OUT_OF_INDEXES;
static TileGuard g_tileGuardMain;

static inline TileRunner* TileRunnerSelf(void)
{
    DWORD k = g_tileRunnerTls;
    if (k == TLS_OUT_OF_INDEXES) return NULL;
    if (k < 64) return (TileRunner*)(uintptr_t)__readgsqword(0x1480 + 8 * k); /* TEB TlsSlots[k]: no SetLastError */
    return (TileRunner*)TlsGetValue(k);
}
static inline TileGuard* TileGuardSelf(void)
{
    TileRunner* r = TileRunnerSelf();
    return r != NULL ? &r->guard : &g_tileGuardMain;
}

static LONG CALLBACK WorldTileFault(EXCEPTION_POINTERS* info)
{
    TileGuard* t = TileGuardSelf();
    if (t->thread == 0 || t->thread != GetCurrentThreadId()) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
        return EXCEPTION_CONTINUE_SEARCH;
    t->thread = 0;
    t->at = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
    t->data = info->ExceptionRecord->NumberParameters > 1 ? (uintptr_t)info->ExceptionRecord->ExceptionInformation[1] : 0;
    t->fault = (LONG)code;
    *info->ContextRecord = t->context;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Ops 0x12, 0x79 and 0xA4 hand qsort its comparator's context through game.dll globals: the route cell's centre at
 * 0x348CE98 (written at 0xFD0376 by 0x12 and 0xF690F6 by 0xA4, read by the comparator 0xFCF7E0) and 0x348C088 (0xF3826D
 * in 0x79, read by 0xF36730). Two threads running them at once sort around each other's centre, so every caller runs
 * them under one lock: the tile runs here, the level's (and previews') in WorldDispatch (static, hellforge-1a). */
static SRWLOCK g_sortContextLock = SRWLOCK_INIT;
static inline int OpUsesSortContext(uint32_t op) { return op == 0x12 || op == 0x79 || op == 0xA4; }

/* One tile step through the dispatcher until its index moves (or `limit` calls), guarded: 0, or the fault's code. */
static __attribute__((noinline)) LONG WorldTileGuarded(uint8_t* record, uint64_t context, volatile uint32_t* index, uint8_t* phase,
    uint64_t g, uint64_t h, int limit, volatile int* calls)
{
    static volatile LONG handler;
    if (handler == 0 && InterlockedCompareExchange(&handler, 1, 0) == 0) { AddVectoredExceptionHandler(1, WorldTileFault); handler = 2; }
    while (handler != 2) SwitchToThread();
    uint32_t op = 0; memcpy(&op, record, 4);
    const int sortLock = OpUsesSortContext(op);
    TileRunner* self = TileRunnerSelf();
    if (self != NULL) { self->op = (LONG)op; self->where = sortLock ? TILE_AT_SORT_WAIT : TILE_AT_OP; }
    if (sortLock) AcquireSRWLockExclusive(&g_sortContextLock);
    if (self != NULL) self->where = TILE_AT_OP;
    TileGuard* t = TileGuardSelf();
    t->fault = 0;
    RtlCaptureContext(&t->context);
    if (t->fault != 0) { if (sortLock) ReleaseSRWLockExclusive(&g_sortContextLock); return t->fault; } /* resumed here from WorldTileFault */
    t->thread = GetCurrentThreadId();
    t->weldFailed = 0;
    while (*index == 0 && *calls < limit)
    {
        g_worldDispatch((uint64_t)record, context, (uint64_t)index, 1, (uint64_t)phase, 0, g, h);
        (*calls)++;
        if (*index == 0 && limit > 1) Sleep(1);
    }
    t->thread = 0;
    if (sortLock) ReleaseSRWLockExclusive(&g_sortContextLock);
    return 0;
}

/* One op's handler called directly as the dispatcher's case does ((record + 8, record, context), one call; ops whose case
 * is only the handler call, e.g. 0x2B at 0xC157E0..0xC1580F), guarded like WorldTileGuarded. A runner of the stamp
 * planning calls op 0x2B so: the dispatcher's path makes three unlocked shared writes on every call (the case's crash
 * context label, exe 0x323E70 -> 0x742B0; the seed text at game+0x37CEDE0, 0xFC3E20; the telemetry append 0xBF4F20),
 * which the generator thread makes at the same time for its own tile ops. */
static __attribute__((noinline)) LONG WorldTileGuardedCall(uintptr_t handler, uint8_t* record, uint64_t context)
{
    static volatile LONG installed;
    if (installed == 0 && InterlockedCompareExchange(&installed, 1, 0) == 0) { AddVectoredExceptionHandler(1, WorldTileFault); installed = 2; }
    while (installed != 2) SwitchToThread();
    TileGuard* t = TileGuardSelf();
    t->fault = 0;
    RtlCaptureContext(&t->context);
    if (t->fault != 0) return t->fault; /* resumed here from WorldTileFault */
    t->thread = GetCurrentThreadId();
    t->weldFailed = 0;
    ((void (*)(uint64_t, uint64_t, uint64_t))handler)((uint64_t)(uintptr_t)record + 8, (uint64_t)(uintptr_t)record, context);
    t->thread = 0;
    return 0;
}

/* The generation arena ([ctx+0]) emptied as the game's step 0xF26140 does after every finished op (0xF261FE..0xF26205,
 * 0x173BAB0: frees every chunk but the oldest and zeroes its use), so every op starts with an empty arena. The tile
 * runs call the dispatcher directly, outside that step: without this every tile's allocations stayed until the level's
 * own op finished (s74w: about 28 MB a composite tile, 5 GB in 3.5 s of the 0x40 wait). Call it only once an op's
 * index has moved, never between the calls of a multi-call op (0x2F keeps [pass+0x50] there until it moves on). */
static void WorldArenaReset(uint64_t context)
{
    static int ok = -1;
    if (ok < 0) ok = ApiCodeMatches(g_world.game + 0x173BAB0u, "40 53 48 83 EC 20 48 8B 01 48 8B D9 48 85 C0 74");
    uint64_t arena = 0;
    if (ok == 1 && context != 0 && !SafeRead(context, &arena, 8) && arena != 0) ((void (*)(uint64_t))(g_world.game + 0x173BAB0u))(arena);
}

/* The runners' stamp index headers, TILE_HEADER_STRIDE apart (difficulty_maps.c's IndexQueryClamped knows an index of
 * a tile run by its address here). */
#define TILE_HEADER_STRIDE 0x3000u
static uint8_t* g_tileHeaders;
static TileRunner* g_tileRunners[TILE_RUNNERS_MAX];

/* Runner `id`, made once and kept (its memory is reserved and committed as the game writes it). NULL without memory. */
static TileRunner* TileRunnerMake(int id)
{
    if (id < 0 || id >= TILE_RUNNERS_MAX) return NULL;
    if (g_tileRunners[id] != NULL)
    {
        TileRunner* r = g_tileRunners[id];
        if (r->decommitted)
        {
            if (VirtualAlloc((void*)(uintptr_t)r->chunk.data, TILE_RUNNER_ARENA, MEM_COMMIT, PAGE_READWRITE) == NULL) return NULL;
            for (int j = 0; j < 8; j++)
                if (r->regions[j] != NULL && VirtualAlloc(r->regions[j], TILE_JOB_REGION, MEM_COMMIT, PAGE_READWRITE) == NULL) return NULL;
            r->decommitted = 0;
        }
        return r;
    }
    if (g_tileHeaders == NULL) g_tileHeaders = (uint8_t*)VirtualAlloc(NULL, TILE_RUNNERS_MAX * TILE_HEADER_STRIDE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_tileHeaders == NULL) return NULL;
    TileRunner* r = (TileRunner*)VirtualAlloc(NULL, sizeof(TileRunner), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (r == NULL) return NULL;
    void* chunk = VirtualAlloc(NULL, TILE_RUNNER_ARENA, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (chunk == NULL) { VirtualFree(r, 0, MEM_RELEASE); return NULL; }
    r->id = id;
    r->header = g_tileHeaders + (size_t)id * TILE_HEADER_STRIDE;
    r->chunk.data = (uint64_t)(uintptr_t)chunk; r->chunk.capacity = TILE_RUNNER_ARENA;
    r->scale = 1.0f;
    for (int j = 0; j < 8; j++)
    {
        uint8_t* region = (uint8_t*)VirtualAlloc(NULL, TILE_JOB_REGION + 0x10000u, MEM_RESERVE, PAGE_NOACCESS);
        if (region != NULL && VirtualAlloc(region, TILE_JOB_REGION, MEM_COMMIT, PAGE_READWRITE) == NULL) { VirtualFree(region, 0, MEM_RELEASE); region = NULL; }
        r->regions[j] = region; /* NULL: TileJobsInline refuses the run */
    }
    g_tileRunners[id] = r;
    return r;
}

/* After a planning section: the runners' memory decommitted (the generation's peak comes later; TileRunnerMake
 * commits it again for the next level). */
static void TileRunnersRelease(void)
{
    for (int id = 0; id < TILE_RUNNERS_MAX; id++)
    {
        TileRunner* r = g_tileRunners[id];
        if (r == NULL) continue;
        VirtualFree((void*)(uintptr_t)r->chunk.data, TILE_RUNNER_ARENA, MEM_DECOMMIT);
        for (int j = 0; j < 8; j++) if (r->regions[j] != NULL) VirtualFree(r->regions[j], TILE_JOB_REGION, MEM_DECOMMIT);
        if (r->memory != NULL) { VirtualFree(r->memory, 0, MEM_RELEASE); r->memory = NULL; r->memoryBytes = 0; }
        r->decommitted = 1;
    }
}

/* The runner's context for one op 0x2B run: the level's context and settings as the section took them, its own arena
 * (empty, as WorldArenaReset leaves one), the settings `settings`, the stamp index `index`. */
static void TileRunnerContext(TileRunner* r, const uint8_t* levelContext, const uint8_t* settings, uintptr_t index)
{
    memcpy(r->settings, settings, sizeof r->settings);
    memcpy(r->ctx, levelContext, sizeof r->ctx);
    r->chunk.used = 0; r->chunk.next = 0;
    r->arena.head = (uint64_t)(uintptr_t)&r->chunk; r->arena.lock = 0; r->arena.owner = -1; r->arena.growth = 0x100000;
    uint64_t v = (uint64_t)(uintptr_t)&r->arena; memcpy(r->ctx, &v, 8);
    v = (uint64_t)(uintptr_t)r->settings; memcpy(r->ctx + 8, &v, 8);
    v = (uint64_t)index; memcpy(r->ctx + 0x20, &v, 8);
}

static void TerrainDiagTileFault(uint32_t op, uintptr_t at, uintptr_t data);

static int WorldTilePasses(uint64_t records, uint32_t end, uint64_t context, uint64_t e, uint64_t f, uint64_t g, uint64_t h, uintptr_t gen)
{
    static uint8_t one[0x19B0];
    uint32_t start = 0xFFFFFFFFu, op = 0;
    for (uint32_t i = 0; i < end; i++)
        if (!SafeRead(records + (uint64_t)i * 0x19B0u, &op, 4) && op == 0x25) { start = i + 1; break; }
    if (start == 0xFFFFFFFFu) { Note("difficulty: terrain tile: no GenerateVoronoi record"); return 0; }
    int seen04 = 0, ran = 0;
    char text[1024];
    int n = 0;
    LARGE_INTEGER hz;
    QueryPerformanceFrequency(&hz);
    for (uint32_t i = start; i < end; i++)
    {
        if (SafeRead(records + (uint64_t)i * 0x19B0u, one, sizeof one)) break;
        memcpy(&op, one, 4);
        if (!WorldTileRuns(op)) continue;
        if (op == 0x04)
        {
            WorldTileListCheck(g_tileBlock, (uintptr_t)g_tileBlock, GEN_BLOCK_SIZE, "tile");
            static uint8_t* level;
            if (level == NULL) level = (uint8_t*)VirtualAlloc(NULL, GEN_BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (level != NULL && !SafeRead(gen, level, GEN_BLOCK_SIZE)) WorldTileListCheck(level, gen, GEN_BLOCK_SIZE, "level");
        }
        WorldTileRelocate(one, gen, (uintptr_t)g_tileBlock);
        { /* research: HELLFORGE_XL_TILE_FAULT_TEST=<op, hex> points that pass's block fields at unmapped memory, so the
           * game faults inside it: a live test of the guard */
            static int read; static uint32_t testOp;
            if (!read) { char t[16] = ""; read = 1; if (GetEnvironmentVariableA("HELLFORGE_XL_TILE_FAULT_TEST", t, sizeof t) > 0) testOp = (uint32_t)strtoul(t, NULL, 16); }
            if (testOp && op == testOp)
            {
                for (uint32_t o = 8; o + 8 <= sizeof one; o += 8)
                {
                    uint64_t v; memcpy(&v, one + o, 8);
                    if (v >= (uint64_t)(uintptr_t)g_tileBlock && v < (uint64_t)(uintptr_t)g_tileBlock + GEN_BLOCK_SIZE) { v = 0x1000; memcpy(one + o, &v, 8); }
                }
                Note("difficulty: terrain tile: fault test: op 0x%02X gets unmapped block fields", op);
            }
        }
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        volatile uint32_t index = 0;
        volatile int calls = 0;
        /* Multi-phase ops keep their phase in the byte the dispatcher's 5th argument points to (0x93: setup 0xF3C870,
         * then 0xF3CF40 until it reports done, 0xC15438): the tile's own byte, from 0, so the level's (its op 0x6D is
         * two-phase too) is never touched; polled across real time, as the phases wait on worker jobs. */
        uint8_t phase = 0;
        LONG fault = WorldTileGuarded(one, context, &index, &phase, g, h, 10000, &calls);
        if (fault)
        {
            Note("difficulty: terrain tile: op 0x%02X faulted (%08lX at game.dll+0x%llX, address 0x%llX): the tile is abandoned, the level goes on",
                op, (unsigned long)fault, (unsigned long long)(TileGuardSelf()->at - (uintptr_t)GetModuleHandleW(L"game.dll")),
                (unsigned long long)TileGuardSelf()->data);
            TerrainDiagTileFault(op, TileGuardSelf()->at, TileGuardSelf()->data);
            return 0;
        }
        if (TileGuardSelf()->weldFailed) { Note("difficulty: terrain tile: op 0x%02X's weld failed: the tile is abandoned, the level goes on", op); return 0; }
        QueryPerformanceCounter(&t1);
        ran++;
        if (index != 0) WorldArenaReset(context);
        { /* research: route cell 0's first edge pointer and the route cell counts after each op (a vertex written over it
           * crashed CellularAutomata at 0xF6D9A0) */
            uint32_t rc[4] = {0};
            memcpy(rc, g_tileBlock + 0x11A71BC, 16);
            uint32_t cellsN = rc[1] < 1024u ? rc[1] : 1024u, bad = 0, firstCell = 0, firstSlot = 0;
            uint64_t firstValue = 0;
            for (uint32_t c = 0; c < cellsN; c++)
                for (uint32_t k = 0; k < 16; k++)
                {
                    uint64_t v;
                    memcpy(&v, g_tileBlock + 0x8EB328 + (size_t)c * 0x110 + 8 + k * 8, 8);
                    if (v != 0 && !(v >= (uint64_t)(uintptr_t)g_tileBlock && v < (uint64_t)(uintptr_t)g_tileBlock + GEN_BLOCK_SIZE))
                    { if (bad++ == 0) { firstCell = c; firstSlot = k; firstValue = v; } }
                }
            uint32_t badBox = 0, boxCell = 0, most = 0, mostCell = 0;
            float box[4] = {0};
            for (uint32_t c = 0; c < cellsN; c++)
            {
                uint8_t* cell = g_tileBlock + 0x8EB328 + (size_t)c * 0x110;
                float b[4];
                memcpy(b, cell + 0x88, 16);
                int ok = 1;
                for (int k = 0; k < 4; k++) if (!(b[k] > -2.0f && b[k] < 1200.0f)) ok = 0;
                if (!ok && badBox++ == 0) { boxCell = c; memcpy(box, b, 16); }
                uint32_t edges = 0;
                for (uint32_t k = 0; k < 16; k++) { uint64_t v; memcpy(&v, cell + 8 + k * 8, 8); if (v) edges++; }
                if (edges > most) { most = edges; mostCell = c; }
            }
            uint64_t merged0 = 0, list0 = 0;
            uint32_t mergedCount = 0, listCount = 0;
            memcpy(&merged0, g_tileBlock + 0x8E9328, 8); memcpy(&list0, g_tileBlock + 0x10CB8C0, 8);
            memcpy(&mergedCount, g_tileBlock + 0x11A71BC, 4); memcpy(&listCount, g_tileBlock + 0x11A71EC, 4);
            uint32_t routeEdges = 0, routeVertices = 0;
            memcpy(&routeEdges, g_tileBlock + 0x11A71B4, 4); memcpy(&routeVertices, g_tileBlock + 0x11A71CC, 4);
            /* only a pass whose route cells look wrong gets its lines (the summary after the passes covers the rest) */
            if (bad || badBox) Note("difficulty: terrain tile: op 0x%02X lists: merged[0] = 0x%llX (count %u), associated[0] = 0x%llX (count %u)", op,
                (unsigned long long)merged0, mergedCount, (unsigned long long)list0, listCount);
            if (bad || badBox) Note("difficulty: terrain tile: op 0x%02X (%d calls): %u bad edge slots (first: cell %u slot %u = 0x%llX), %u bad boxes (first: cell %u: %g %g %g %g), most edges %u (cell %u); route cells %u, edges %u (room for ~978), vertices %u (room for ~1621); counts +1BC/+1C4/+1C8 %u %u %u",
                op, calls, bad, firstCell, firstSlot, (unsigned long long)firstValue, badBox, boxCell, (double)box[0], (double)box[1], (double)box[2], (double)box[3],
                most, mostCell, rc[1], routeEdges, routeVertices, rc[0], rc[2], rc[3]);
        }
        if (n < (int)sizeof text - 32)
            n += snprintf(text + n, sizeof text - n, " %02X:%.1f%s", op, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart, calls > 1 ? "+" : "");
        if (op == 0x04) seen04 = 1;
        if (op == 0x2D && seen04) break;
    }
    uint8_t* T = g_tileBlock;
    uint32_t vertices = 0, cells = 0, routeCells = 0, subCells = 0;
    memcpy(&vertices, T + 0x11A7090, 4); memcpy(&cells, T + 0x11A70B8, 4);
    memcpy(&routeCells, T + 0x11A71C0, 4); memcpy(&subCells, T + 0x11A71C4, 4);
    Note("difficulty: terrain tile: %d passes (op:ms):%s", ran, text);
    Note("difficulty: terrain tile: %u vertices, %u cells, route cells %u / %u", vertices, cells, routeCells, subCells);
    if (vertices == 0 || vertices > 6000) return 1;
    wchar_t name[64];
    swprintf(name, 64, L"tile-vertices-%ld.f32", (long)g_world.levels);
    HANDLE file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return 1;
    float lo = 1e30f, hi = -1e30f;
    for (uint32_t v = 0; v < vertices; v++)
    {
        float xyh[3];
        memcpy(xyh, T + 0x9F310 + (size_t)v * 0x48, 8);
        memcpy(&xyh[2], T + 0x1F8A30 + (size_t)v * 4, 4);
        if (xyh[2] < lo) lo = xyh[2];
        if (xyh[2] > hi) hi = xyh[2];
        DWORD wrote;
        WriteFile(file, xyh, sizeof xyh, &wrote, NULL);
    }
    CloseHandle(file);
    Note("difficulty: terrain tile: wrote %u vertices, heights %.4f..%.4f", vertices, (double)lo, (double)hi);
    { /* the level's own vertex heights at this point, and the settings' height scale (RenderHeightmapVoronoi multiplies
       * vertex heights by [settings+0x30]) */
        uint32_t levelVertices = 0;
        float scale = 0, llo = 1e30f, lhi = -1e30f;
        double sum = 0;
        uint64_t settings = 0;
        SafeRead(gen + 0x11A7090, &levelVertices, 4);
        /* SmoothHeightmap (0x40) hands its GPU job settings+0x74 (16 bytes): a kernel in texels would cover less ground on a
         * tile (4096 texels over 768 m, vanilla's over 2048 m), so the values are logged with the scale */
        uint32_t smooth[4] = {0};
        if (!SafeRead(context + 8, &settings, 8) && settings) { SafeRead(settings + 0x30, &scale, 4); SafeRead(settings + 0x74, smooth, sizeof smooth); }
        for (uint32_t v = 0; v < levelVertices && v < 6000; v++)
        {
            float hv;
            if (SafeRead(gen + 0x1F8A30 + (uint64_t)v * 4, &hv, 4)) break;
            if (hv < llo) llo = hv;
            if (hv > lhi) lhi = hv;
            sum += hv;
        }
        Note("difficulty: terrain tile: the level's %u vertex heights %.4f..%.4f (mean %.4f); height scale [settings+0x30] %.2f; smoothing [settings+0x74] %08X %08X %08X %08X (%g %g %g %g)",
            levelVertices, (double)llo, (double)lhi, levelVertices ? sum / levelVertices : 0.0, (double)scale, smooth[0], smooth[1], smooth[2], smooth[3],
            (double)*(float*)&smooth[0], (double)*(float*)&smooth[1], (double)*(float*)&smooth[2], (double)*(float*)&smooth[3]);
    }
    return 1;
}

/* every count field the pipeline's records name (records-<level>.txt): a tile starts them at zero, as a new level does */
static const uint32_t kTileCounts[] = {
    0x11A7070u, 0x11A7074u, 0x11A7078u, 0x11A707Cu, 0x11A7080u, 0x11A7084u, 0x11A7088u, 0x11A708Cu, 0x11A7090u,
    0x11A7094u, 0x11A7098u, 0x11A709Cu, 0x11A70A8u, 0x11A70ACu, 0x11A70B0u, 0x11A70B4u, 0x11A70B8u, 0x11A70BCu,
    0x11A70C0u, 0x11A70C4u, 0x11A70CCu, 0x11A70D0u, 0x11A70D4u, 0x11A70D8u, 0x11A70E0u, 0x11A70E4u, 0x11A70E8u,
    0x11A70F0u, 0x11A70F4u, 0x11A70F8u, 0x11A70FCu, 0x11A7100u, 0x11A7104u, 0x11A7108u, 0x11A710Cu, 0x11A7118u,
    0x11A711Cu, 0x11A7124u, 0x11A712Cu, 0x11A7130u, 0x11A7134u, 0x11A7138u, 0x11A713Cu, 0x11A7140u, 0x11A7144u,
    0x11A7148u, 0x11A7154u, 0x11A715Cu, 0x11A7160u, 0x11A7164u, 0x11A7168u, 0x11A716Cu, 0x11A7170u, 0x11A7174u,
    0x11A7178u, 0x11A717Cu, 0x11A7180u, 0x11A7184u, 0x11A718Cu, 0x11A7190u, 0x11A7194u, 0x11A7198u, 0x11A719Cu,
    0x11A71A0u, 0x11A71A4u, 0x11A71A8u, 0x11A71ACu, 0x11A71B0u, 0x11A71B4u, 0x11A71B8u, 0x11A71BCu, 0x11A71C0u,
    0x11A71C4u, 0x11A71C8u, 0x11A71CCu, 0x11A71D0u, 0x11A71D4u, 0x11A71D8u, 0x11A71DCu, 0x11A71E0u, 0x11A71E4u,
    0x11A71E8u, 0x11A71ECu, 0x11A71F0u, 0x11A71F4u, 0x11A71F8u, 0x11A71FCu, 0x11A7200u, 0x11A7204u, 0x11A7210u,
    0x11A7218u, 0x11A721Cu, 0x11A7220u, 0x11A7224u, 0x11A7228u, 0x11A722Cu, 0x11A7234u, 0x11A7238u, 0x11A723Cu,
    0x11A7240u, 0x11A7244u, 0x11A7248u, 0x11A724Cu, 0x11A7250u};

static int g_tileExtent;
static LONG g_tileCpuLevel = -1;
static void TerrainDiagTile(const char* label, const void* data, uint32_t w, uint32_t h, uint32_t bpp, double metres);
static void TerrainDiagTilePlan(uintptr_t block, double extent);

static void WorldTileProbe(uint64_t records, uint32_t end, uint64_t context, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    if (g_tileDone == g_world.levels || g_worldLevel == 0 || g_rec25 == 0) return;
    g_tileDone = g_world.levels;
    static int extent = -1;
    if (extent < 0)
    {
        char text[16] = "";
        extent = GetEnvironmentVariableA("HELLFORGE_XL_TILE", text, sizeof text) > 0 ? atoi(text) : 0;
        if (extent < 256 || extent > 2048) extent = 0;
    }
    if (extent == 0) return;
    uintptr_t gen = 0, settings = 0, spacingAt = 0, p = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(context + 8, &settings, 8) || settings == 0 ||
        SafeRead(context + 0x18, &p, 8) || p == 0 || SafeRead(p + 0x2D0, &p, 8) || p == 0 || SafeRead(p + 0x68, &spacingAt, 8) || spacingAt == 0)
    { Note("difficulty: terrain tile: the generator's settings did not read"); return; }
    if (g_tileBlock == NULL) g_tileBlock = (uint8_t*)VirtualAlloc(NULL, GEN_BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    static uint8_t record[0x19B0];
    if (g_tileBlock == NULL || SafeRead(gen, g_tileBlock, GEN_BLOCK_SIZE) || SafeRead(g_rec25, record, sizeof record))
    { Note("difficulty: terrain tile: the generator block did not copy"); return; }
    WorldTileRelocate(record, gen, (uintptr_t)g_tileBlock);
    /* The block points into itself (array descriptors, cell -> edge -> vertex links): the copy's own pointers move too,
     * or passes on the copy write through them into the level's block. */
    WorldTileRelocateIn(g_tileBlock, GEN_BLOCK_SIZE, gen, (uintptr_t)g_tileBlock);
    uint32_t size = 0; float lo = 0, hi = 0, radius = 0, spacing = 0;
    SafeRead(settings + 0x2C, &size, 4); SafeRead(settings + 0x64, &lo, 4); SafeRead(settings + 0x68, &hi, 4);
    SafeRead(settings + 0x6C, &radius, 4); SafeRead(spacingAt, &spacing, 4);
    Note("difficulty: terrain tile: the level's Voronoi settings: world %u m, box %.1f..%.1f, keep radius %.1f, spacing %.2f m",
        size, (double)lo, (double)hi, (double)radius, (double)spacing);
    volatile uint32_t* sizeP = (volatile uint32_t*)settings + 0x2C / 4;
    volatile float* loP = (volatile float*)(settings + 0x64);
    volatile float* hiP = (volatile float*)(settings + 0x68);
    volatile float* radiusP = (volatile float*)(settings + 0x6C);
    volatile float* spacingP = (volatile float*)spacingAt;
    /* difficulty_maps.c's grid stub (0xF3FB04) swaps in the stretched spacing while the level's disk ([lg+0x2D7C], lg =
     * [context+0x18]) is the larger map's; the field is changed for the call so the 16 m setting is used. Nothing
     * else in op 0x25 or jc_voronoi reads it. */
    uintptr_t lg = 0;
    uint32_t disk = 0;
    if (SafeRead(context + 0x18, &lg, 8) || lg == 0 || SafeRead(lg + 0x2D7C, &disk, 4)) { Note("difficulty: terrain tile: the level did not read"); return; }
    volatile uint32_t* diskP = (volatile uint32_t*)(lg + 0x2D7C);
    *diskP = disk ^ 1u;
    *sizeP = (uint32_t)extent; *loP = 0.0f; *hiP = (float)extent; *radiusP = (float)extent; *spacingP = 16.0f;
    /* A new level's block starts zeroed, and several passes append to their lists from the current count
     * (MergeSubRegionCells 0x13, BuildRouteCellAssociatedRoutes 0x02): every count field the pipeline's records name
     * (records-<level>.txt) starts the tile at zero, as for a new level. That also leaves the tile without the level's
     * locations, routes and segments (for now). */

    for (size_t k = 0; k < sizeof kTileCounts / sizeof kTileCounts[0]; k++) memset(g_tileBlock + kTileCounts[k], 0, 4);
    /* The copy holds the level's data, and only the counts start the tile at zero: the route-cell areas ("edge points",
     * G+0xEE10C8, room for 13224 of 0x90 until G+0x10B1F48) kept the level's 16 km entries past the few the tile's
     * passes wrote, and RenderHeightmapVoronoi drew them scaled into the tile: x-streaks and +-800 m heights. Cleared,
     * the tile is vanilla-like terrain (run t9, same map: sd 148 m / 66 deg stale, 8.2 m / 17.8 deg cleared). */
    memset(g_tileBlock + 0xEE10C8u, 0, 0x10B1F48u - 0xEE10C8u);

    typedef void (*Handler)(uint64_t, uint64_t, uint64_t);
    LARGE_INTEGER t0, t1, hz;
    QueryPerformanceCounter(&t0);
    ((Handler)(g_world.game + 0xF3F8E0u))((uint64_t)record + 8, (uint64_t)record, context);
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&hz);
    {
        uint64_t edge0 = 0;
        memcpy(&edge0, g_tileBlock + 0x8EB328 + 8, 8);
        Note("difficulty: terrain tile: before the passes: route cell 0 +8 = 0x%llX (copy at 0x%llX)", (unsigned long long)edge0, (unsigned long long)(uintptr_t)g_tileBlock);
    }
    int passed = WorldTilePasses(records, end, context, e, f, g, h, gen);
    *sizeP = size; *loP = lo; *hiP = hi; *radiusP = radius; *spacingP = spacing; *diskP = disk;
    if (!passed) return;
    g_tileExtent = extent;
    g_tileCpuLevel = g_world.levels;
    uint32_t counts[3] = {0};
    for (int k = 0; k < 3; k++)
    {
        uint64_t at = 0;
        memcpy(&at, record + 8 + 0x20 + 0x10 * k, 8);
        if (at) SafeRead(at, &counts[k], 4);
    }
    Note("difficulty: terrain tile: %d m tile Voronoi in %.1f ms: counts %u / %u / %u", extent,
        (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart, counts[0], counts[1], counts[2]);
    WorldGridStats((uintptr_t)g_tileBlock, "tile");
    TerrainDiagTilePlan((uintptr_t)g_tileBlock, extent);
}

static volatile LONG g_terrainProbes;
static void WorldTerrainProbe(uint64_t records, uint32_t at, uint32_t op)
{
    static LONG levelSeen = -1, levels;
    if (levelSeen != g_world.levels) { levelSeen = g_world.levels; levels++; }
    if (levels > 2) return;
    wchar_t name[64];
    swprintf(name, 64, L"terrain-probe-%ld.txt", (long)g_world.levels);
    HANDLE file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return;
    char title[96];
    DWORD wrote;
    uint64_t record = records + (uint64_t)at * 0x19B0u;
    int n = snprintf(title, sizeof title, "\n==== op 0x%X (record %u), world scale %g\n", op, at, (double)g_world.current);
    WriteFile(file, title, (DWORD)n, &wrote, NULL);
    if (op == 0x38)
    {
        uint64_t engine = 0, table = 0, world = 0;
        SafeRead(g_world.game + 0x3326308, &engine, 8); SafeRead(engine + 0x10, &table, 8); SafeRead(engine + 8, &world, 8);
        static const uint32_t slots[] = {0x190, 0x1D0, 0x1D8, 0x218, 0x470};
        for (size_t k = 0; k < sizeof slots / sizeof slots[0]; k++)
        {
            uint64_t fn = 0; char where[160] = "?";
            SafeRead(table + slots[k], &fn, 8); ModuleAt((uintptr_t)fn, where, sizeof where);
            n = snprintf(title, sizeof title, "engine +0x10 table +0x%X: %s\n", slots[k], where); WriteFile(file, title, (DWORD)n, &wrote, NULL);
        }
        uint64_t fn = 0; char where[160] = "?";
        SafeRead(world + 0x1A8, &fn, 8); ModuleAt((uintptr_t)fn, where, sizeof where);
        n = snprintf(title, sizeof title, "engine +8 table +0x1A8: %s\n", where); WriteFile(file, title, (DWORD)n, &wrote, NULL);
    }
    ProbeHex(file, "pass record", record, 0x200);
    for (uint32_t k = 0; k < 0x80; k += 8)
    {
        uint64_t p = 0, q = 0;
        if (SafeRead(record + 8 + k, &p, 8) || p < 0x10000 || SafeRead(p, &q, 8)) continue;
        snprintf(title, sizeof title, "[pass+8+0x%X]", k);
        ProbeHex(file, title, p, 0x100);
        if (q >= 0x10000 && !SafeRead(q, &p, 8)) { snprintf(title, sizeof title, "[[pass+8+0x%X]]", k); ProbeHex(file, title, q, 0x300); }
    }
    CloseHandle(file);
}

/* Step 3: the tile's heightmap through the game's own terrain merge, as the level's first merge runs it (records from its
 * SetupTerrainMerge on): 0x38 setup (the orthographic camera over the world size, here the tile's; record +0xB4 the
 * samples per side: the level's, see WorldTileGpuPrepare), 0x39 clear, 0x3A, 0x3C the Voronoi cells, 0x40 smoothing, 0x5B, 0x5C the
 * capture (8 GPU readbacks; their descriptors, 0x18 each, at lg + 0x18 k). The minimap's 0x53/0x5A/0x54 are left out.
 * GPU passes need frames between their phases, so the level waits at its first SetupTerrainMerge: while the tile is
 * under way the dispatcher hook runs one tile step per call and returns without the level's record (its generator
 * then yields the frame: an index that did not move is "in progress", 0xF261BD). Each step applies the tile's settings
 * for its call only. Up to 900 frames; then the level goes on. */
static const uint8_t kTileGpuOps[] = {0x38, 0x39, 0x3A, 0x3C, 0x40, 0x5B, 0x5C};
static LONG g_tileGpuLevel = -1;
static int g_tileGpuStep, g_tileGpuDone;
static uint32_t g_tileGpuFrames, g_tileGpuCalls, g_tileGpuRecord[sizeof kTileGpuOps];
/* research A/B: HELLFORGE_XL_TILE_SKIP=<op, hex> leaves that merge pass out of every second tile merge (a run's two
 * finished levels, 7 and 14, are the same map: one with the pass, one without) */
static uint32_t g_tileSkip, g_tileRuns;
static uint8_t g_tileGpuPhase, g_tileGpuOne[0x19B0];

static int WorldTileGpuPrepare(uint64_t records, uintptr_t gen)
{
    if (SafeRead(records + (uint64_t)g_tileGpuRecord[g_tileGpuStep] * 0x19B0u, g_tileGpuOne, sizeof g_tileGpuOne)) return 0;
    WorldTileRelocate(g_tileGpuOne, gen, (uintptr_t)g_tileBlock);
    if (kTileGpuOps[g_tileGpuStep] == 0x38)
    {
        /* the merge's targets keep the level's size (its record's +0xB4, 4096): a tile asking for 2 per metre (1536 for
         * 768 m) drew into a 1536^2 corner of them and the captures, which downsample the whole target, held it in 0.375
         * of their width. The tile takes the level's samples and fills the target (768 m: 0.19 m per sample). */
        float samples = 0;
        memcpy(&samples, g_tileGpuOne + 0xB4, 4);
        if (!(samples >= 256.0f && samples <= 16384.0f)) samples = 4096.0f;
        memcpy(g_tileGpuOne + 0xB4, &samples, 4);
        uint32_t one = 1; /* one merge tile, as vanilla's (see the merge's T in WorldDispatch) */
        memcpy(g_tileGpuOne + 0xB0, &one, 4);
    }
    g_tileGpuPhase = 0;
    g_tileGpuCalls = 0;
    return 1;
}

static void WorldTileGpuCaptures(void)
{
    char summary[512]; int sn = 0;
    for (int k = 0; k < 8; k++)
    {
        uint64_t q[3];
        /* CaptureIntermediateHeightmapFinalize (0xF53870) copies each readback's descriptor {width, height, format,
         * ?, data} to [pass+0x18] = lg + 0x18 k (lg = G+0x8758A8; G+0x119FF68 holds the readback ids) */
        memcpy(q, g_tileBlock + 0x8758A8 + k * 0x18, 24);
        uint32_t w = (uint32_t)q[0], hgt = (uint32_t)(q[0] >> 32), format = (uint32_t)q[1];
        if (q[2] == 0 || w < 16 || w > 8192 || hgt < 16 || hgt > 8192)
        {
            if (sn < (int)sizeof summary - 48) sn += snprintf(summary + sn, sizeof summary - sn, " %d:none", k);
            continue;
        }
        uint64_t bytes = (uint64_t)w * hgt * (format == 0x16 ? 2u : 4u);
        uint8_t* copy = (uint8_t*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (copy == NULL) continue;
        if (!SafeRead(q[2], copy, bytes))
        {
            if (k == 0 && g_tileExtent > 0) TerrainDiagTile("tile-heights", copy, w, hgt, format == 0x16 ? 2u : 4u, (double)g_tileExtent / w);
            wchar_t name[64];
            swprintf(name, 64, L"tile-capture-%ld-%d.bin", (long)g_world.levels, k);
            HANDLE file = OpenLog(name);
            if (file != INVALID_HANDLE_VALUE)
            {
                DWORD wrote;
                WriteFile(file, copy, (DWORD)bytes, &wrote, NULL);
                CloseHandle(file);
                if (sn < (int)sizeof summary - 48) sn += snprintf(summary + sn, sizeof summary - sn, " %d:%ux%u/0x%X", k, w, hgt, format);
            }
        }
        VirtualFree(copy, 0, MEM_RELEASE);
    }
    Note("difficulty: terrain tile: captures written (k:size/format):%s", summary);
}

static int WorldTileStep(uint64_t records, uint32_t end, uint32_t at, uint64_t context, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    if (g_tileGpuLevel != g_world.levels)
    {
        g_tileGpuLevel = g_world.levels;
        g_tileGpuDone = 1;
        WorldTileProbe(records, end, context, e, f, g, h);
        if (g_tileCpuLevel != g_world.levels || g_tileBlock == NULL) return 0;
        uint32_t i = at;
        for (size_t k = 0; k < sizeof kTileGpuOps; k++)
        {
            uint32_t op = 0;
            for (; i < end; i++) if (!SafeRead(records + (uint64_t)i * 0x19B0u, &op, 4) && op == kTileGpuOps[k]) break;
            if (i >= end) { Note("difficulty: terrain tile: no merge record for op 0x%02X", kTileGpuOps[k]); return 0; }
            g_tileGpuRecord[k] = i++;
        }
        uintptr_t gen = 0;
        if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0) return 0;
        g_tileGpuStep = 0;
        g_tileGpuFrames = 0;
        g_tileRuns++;
        static int skipRead;
        if (!skipRead)
        {
            char text[16] = "";
            skipRead = 1;
            if (GetEnvironmentVariableA("HELLFORGE_XL_TILE_SKIP", text, sizeof text) > 0) g_tileSkip = (uint32_t)strtoul(text, NULL, 16);
        }
        if (!WorldTileGpuPrepare(records, gen)) return 0;
        g_tileGpuDone = 0;
        Note("difficulty: terrain tile: the merge starts (records %u..%u), the level waits", g_tileGpuRecord[0], g_tileGpuRecord[sizeof kTileGpuOps - 1]);
        return 1;
    }
    if (g_tileGpuDone) return 0;
    uintptr_t gen = 0, settings = 0, lg = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(context + 8, &settings, 8) || settings == 0 ||
        SafeRead(context + 0x18, &lg, 8) || lg == 0) { g_tileGpuDone = 1; return 0; }
    volatile uint32_t* sizeP = (volatile uint32_t*)(settings + 0x2C);
    volatile float* loP = (volatile float*)(settings + 0x64);
    volatile float* hiP = (volatile float*)(settings + 0x68);
    volatile float* radiusP = (volatile float*)(settings + 0x6C);
    volatile uint32_t* diskP = (volatile uint32_t*)(lg + 0x2D7C);
    uint32_t size = *sizeP, disk = *diskP;
    float lo = *loP, hi = *hiP, radius = *radiusP;
    *diskP = disk ^ 1u; *sizeP = (uint32_t)g_tileExtent; *loP = 0.0f; *hiP = (float)g_tileExtent; *radiusP = (float)g_tileExtent;
    volatile uint32_t index = 0;
    volatile int calls = 0;
    LONG fault = WorldTileGuarded(g_tileGpuOne, context, &index, &g_tileGpuPhase, g, h, 1, &calls);
    *sizeP = size; *loP = lo; *hiP = hi; *radiusP = radius; *diskP = disk;
    if (fault)
    {
        Note("difficulty: terrain tile: merge op 0x%02X faulted (%08lX at game.dll+0x%llX): the tile is abandoned, the level goes on",
            kTileGpuOps[g_tileGpuStep], (unsigned long)fault, (unsigned long long)(TileGuardSelf()->at - (uintptr_t)GetModuleHandleW(L"game.dll")));
        TerrainDiagTileFault(kTileGpuOps[g_tileGpuStep], TileGuardSelf()->at, TileGuardSelf()->data);
        g_tileGpuDone = 1;
        return 0;
    }
    g_tileGpuCalls++;
    g_tileGpuFrames++;
    if (index != 0)
    {
        WorldArenaReset(context);
        Note("difficulty: terrain tile: merge op 0x%02X done (%u calls, frame %u)", kTileGpuOps[g_tileGpuStep], g_tileGpuCalls, g_tileGpuFrames);
        ++g_tileGpuStep;
        if (g_tileGpuStep < (int)sizeof kTileGpuOps && g_tileSkip && kTileGpuOps[g_tileGpuStep] == g_tileSkip && (g_tileRuns & 1) == 0)
        {
            Note("difficulty: terrain tile: op 0x%02X left out (HELLFORGE_XL_TILE_SKIP, tile merge %u)", g_tileSkip, g_tileRuns);
            ++g_tileGpuStep;
        }
        if (g_tileGpuStep == (int)sizeof kTileGpuOps)
        {
            WorldTileGpuCaptures();
            g_tileGpuDone = 1;
            return 0;
        }
        if (!WorldTileGpuPrepare(records, gen)) { g_tileGpuDone = 1; return 0; }
    }
    if (g_tileGpuFrames >= 900)
    {
        Note("difficulty: terrain tile: the merge stopped at op 0x%02X after %u frames; the level goes on", kTileGpuOps[g_tileGpuStep], g_tileGpuFrames);
        g_tileGpuDone = 1;
        return 0;
    }
    return 1;
}

/* Research (XL visuals): the engine's camera interface, [[game+0x3326308]+0x20] (as SetupTerrainMerge's camera setup
 * 0xF487B0 uses it): +0x20 set_local_position(camera, node, const float xyz[3]), +0x28 set_local_rotation(camera, node,
 * const float xyzw[4]), +0x68 / +0x70 near / far range (camera, float). The game sets its gameplay camera natively after
 * Lua's update, so a high vantage over an extra large map needs this. The three entries are swapped for wrappers once
 * generation runs (the interface is set by then):
 *   research: the callers of set_local_position (module + RVA), their camera, call count and last position are noted
 *   every 20 s for 10 minutes from the first call (the first extra large level);
 *   HELLFORGE_XL_VANTAGE=x,y,z,yaw,pitch (world metres, degrees; pitch down from level) with
 *   HELLFORGE_XL_VANTAGE_CALLER=g:<rva> or x:<rva> (game.dll or the exe): that caller's camera gets the vantage pose
 *   (Z up, cameras look along +Y: yaw about Z, then pitch about X) and a 30 km far range. */
typedef void (*CamPoseFn)(uint64_t camera, uint32_t node, const float* v);
typedef void (*CamRangeFn)(uint64_t camera, float v);
static CamPoseFn g_camSetPosition, g_camSetRotation;
static CamRangeFn g_camSetFar;
/* the near-player tile's replay moves the merge cameras' per-job positions (difficulty_world_terrain.c) */
static int TerrainNearCamShift(uint64_t camera, uintptr_t at, const float* v, float* out);
/* and its test eye (HELLFORGE_XL_EYE) takes the gameplay camera's pose */
static int TerrainNearEyePosition(uint64_t camera, uintptr_t at, const float* v, float* out);
static int TerrainNearEyeRotation(uintptr_t at, float* out);
static int TerrainNearEyeFar(uint64_t camera);
typedef struct { uintptr_t at; uint64_t camera; volatile LONG calls; float position[3]; } CamCaller;
static CamCaller g_camCallers[48];
static volatile LONG g_camCallerCount;
static uintptr_t g_vantageCaller;
static float g_vantagePosition[3], g_vantageRotation[4];
static int g_vantageOn;
static LARGE_INTEGER g_camStart;
static LONG g_camNotes;

static int WorldCamFromVantage(uintptr_t at)
{
    return g_vantageOn && at == g_vantageCaller;
}

static void WorldCamNote(void)
{
    LARGE_INTEGER now, hz;
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&hz);
    if (g_camStart.QuadPart == 0) { g_camStart = now; return; }
    double seconds = (double)(now.QuadPart - g_camStart.QuadPart) / (double)hz.QuadPart;
    static int on = -1; /* research: HELLFORGE_XL_CAMERA_NOTES=1 lists the camera callers (up to 48 lines every 20 s) */
    if (on < 0) { char t[4] = ""; on = GetEnvironmentVariableA("HELLFORGE_XL_CAMERA_NOTES", t, sizeof t) > 0 && t[0] == '1'; }
    if (!on || g_camNotes >= 30 || seconds < 20.0 * (g_camNotes + 1)) return;
    g_camNotes++;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    LONG n = g_camCallerCount < 48 ? g_camCallerCount : 48;
    for (LONG i = 0; i < n; i++)
    {
        CamCaller* c = &g_camCallers[i];
        int inGame = c->at >= g_world.game && c->at < g_world.game + 0x4000000u;
        Note("difficulty: camera (%.0f s): set_local_position from %s:%llX on camera 0x%llX, %ld calls, last (%.1f, %.1f, %.1f)%s", seconds,
            inGame ? "g" : "x", (unsigned long long)(c->at - (inGame ? g_world.game : exe)), (unsigned long long)c->camera, (long)c->calls,
            (double)c->position[0], (double)c->position[1], (double)c->position[2], WorldCamFromVantage(c->at) ? " (vantage)" : "");
    }
}

static void WorldCamSetPosition(uint64_t camera, uint32_t node, const float* v)
{
    uintptr_t at = (uintptr_t)__builtin_return_address(0);
    LONG n = g_camCallerCount < 48 ? g_camCallerCount : 48, i;
    for (i = 0; i < n; i++) if (g_camCallers[i].at == at) break;
    if (i == n && n < 48) { i = InterlockedIncrement(&g_camCallerCount) - 1; if (i < 48) g_camCallers[i].at = at; }
    if (i < 48)
    {
        CamCaller* c = &g_camCallers[i];
        c->camera = camera;
        InterlockedIncrement(&c->calls);
        if (v) memcpy(c->position, v, 12);
    }
    if (g_world.current == 8.0f) WorldCamNote();
    if (WorldCamFromVantage(at))
    {
        g_camSetPosition(camera, node, g_vantagePosition);
        if (g_camSetFar) g_camSetFar(camera, 30000.0f);
        return;
    }
    float shifted[3];
    if (v != NULL && TerrainNearCamShift(camera, at, v, shifted)) v = shifted;
    else if (TerrainNearEyePosition(camera, at, v, shifted)) { g_camSetPosition(camera, node, shifted); if (g_camSetFar) g_camSetFar(camera, 20000.0f); return; }
    g_camSetPosition(camera, node, v);
}

static void WorldCamSetRotation(uint64_t camera, uint32_t node, const float* v)
{
    float eye[4];
    if (TerrainNearEyeRotation((uintptr_t)__builtin_return_address(0), eye)) { g_camSetRotation(camera, node, eye); return; }
    /* the rotation is set right after the position by the same code: its caller is the position's caller + a few bytes,
     * so the camera is matched instead (the last camera the vantage caller positioned) */
    uintptr_t at = g_vantageCaller;
    LONG n = g_camCallerCount < 48 ? g_camCallerCount : 48;
    for (LONG i = 0; g_vantageOn && i < n; i++)
        if (g_camCallers[i].at == at && g_camCallers[i].camera == camera) { g_camSetRotation(camera, node, g_vantageRotation); return; }
    g_camSetRotation(camera, node, v);
}

static void WorldCamSetFar(uint64_t camera, float v)
{
    if (TerrainNearEyeFar(camera)) { g_camSetFar(camera, 20000.0f); return; }
    LONG n = g_camCallerCount < 48 ? g_camCallerCount : 48;
    for (LONG i = 0; g_vantageOn && i < n; i++)
        if (g_camCallers[i].at == g_vantageCaller && g_camCallers[i].camera == camera) { g_camSetFar(camera, 30000.0f); return; }
    g_camSetFar(camera, v);
}

static void WorldCamInstall(void)
{
    static int done;
    if (done) return;
    uint64_t api = 0, table = 0;
    if (SafeRead(g_world.game + 0x3326308u, &api, 8) || api == 0 || SafeRead(api + 0x20, &table, 8) || table == 0) return;
    done = 1;
    char text[96] = "";
    if (GetEnvironmentVariableA("HELLFORGE_XL_VANTAGE", text, sizeof text) > 0)
    {
        float p[5] = {0};
        if (sscanf(text, "%f,%f,%f,%f,%f", &p[0], &p[1], &p[2], &p[3], &p[4]) == 5)
        {
            char who[48] = "";
            unsigned long long rva = 0;
            char module = 0;
            if (GetEnvironmentVariableA("HELLFORGE_XL_VANTAGE_CALLER", who, sizeof who) > 0 && sscanf(who, "%c:%llx", &module, &rva) == 2 && (module == 'g' || module == 'x'))
            {
                g_vantageCaller = (module == 'g' ? g_world.game : (uintptr_t)GetModuleHandleW(NULL)) + (uintptr_t)rva;
                memcpy(g_vantagePosition, p, 12);
                float yaw = p[3] * 3.14159265f / 180.0f, pitch = -p[4] * 3.14159265f / 180.0f;
                float sz = sinf(yaw * 0.5f), cz = cosf(yaw * 0.5f), sx = sinf(pitch * 0.5f), cx = cosf(pitch * 0.5f);
                g_vantageRotation[0] = cz * sx; g_vantageRotation[1] = sz * sx; g_vantageRotation[2] = sz * cx; g_vantageRotation[3] = cz * cx;
                g_vantageOn = 1;
            }
        }
    }
    void** slots = (void**)(uintptr_t)table;
    DWORD old = 0;
    if (!VirtualProtect(slots + 4, 0x58, PAGE_READWRITE, &old)) { Note("difficulty: camera: the interface table did not open"); return; }
    g_camSetPosition = (CamPoseFn)slots[0x20 / 8]; g_camSetRotation = (CamPoseFn)slots[0x28 / 8]; g_camSetFar = (CamRangeFn)slots[0x70 / 8];
    InterlockedExchangePointer(&slots[0x20 / 8], (void*)WorldCamSetPosition);
    InterlockedExchangePointer(&slots[0x28 / 8], (void*)WorldCamSetRotation);
    InterlockedExchangePointer(&slots[0x70 / 8], (void*)WorldCamSetFar);
    VirtualProtect(slots + 4, 0x58, old, &old);
    Note("difficulty: camera: the interface's position, rotation and far range are watched%s (vantage %.0f, %.0f, %.0f from caller 0x%llX)",
        g_vantageOn ? " and the vantage is on" : "", (double)g_vantagePosition[0], (double)g_vantagePosition[1], (double)g_vantagePosition[2],
        (unsigned long long)g_vantageCaller);
}

/* Research (scatter): the generator's scatter table after FinalizeScatterInitialization (op 0x96), vanilla and larger:
 * the globals at game+0x3328F60 (+0 a pointer, +8 a count, +0xC (world size x 0.5 x [settings+0x5C])^2, set by op 0xBB)
 * and each type (0x2860 bytes from game+0x3328F90, [game+0x346BF90] of them; +0x2850 its grid: dim, two floats, data at
 * +0x10, allocated dim^2 x 16 bytes by op 0xBB). */
static int g_scatterDump; /* HELLFORGE_XL_SCATTER_DUMP=1: the images too */
static void WorldScatterNote(void)
{
    { static int read; if (!read) { char t[4] = ""; read = 1; g_scatterDump = GetEnvironmentVariableA("HELLFORGE_XL_SCATTER_DUMP", t, sizeof t) > 0 && t[0] == '1'; } }
    static LONG notes;
    if (InterlockedIncrement(&notes) > 4) return;
    uintptr_t base = g_world.game + 0x3328F60u;
    uint32_t head[12] = {0}, count = 0;
    SafeRead(base, head, sizeof head);
    SafeRead(g_world.game + 0x346BF90u, &count, 4);
    float area = 0; memcpy(&area, &head[3], 4);
    Note("difficulty: scatter (%.0fx): %u types, globals %08X%08X count %u area %.0f (sqrt %.0f), %08X %08X %08X %08X %08X %08X %08X %08X",
        (double)g_world.current, count, head[1], head[0], head[2], (double)area, (double)sqrtf(area), head[4], head[5], head[6], head[7], head[8], head[9], head[10], head[11]);
    /* The brush callback (0x137A2A0) samples three images through +0x10/+0x18/+0x20 ({w, h, .., data at +0x10}, 4 bytes
     * a texel, u = x / world + 0.5): written out once per level, with the id table at +0 ([+8] entries) and +0x28. */
    uint32_t table[16] = {0}, tag = 0;
    uint64_t tableAt = ((uint64_t)head[1] << 32) | head[0];
    if (tableAt != 0 && head[2] <= 16) SafeRead(tableAt, table, head[2] * 4u);
    SafeRead(base + 0x28, &tag, 4);
    Note("difficulty: scatter (%.0fx): id table %08X %08X %08X %08X %08X %08X, tag %08X", (double)g_world.current, table[0], table[1], table[2], table[3], table[4], table[5], tag);
    for (int k = 0; k < 3; k++)
    {
        uint64_t at = 0;
        uint32_t desc[6] = {0};
        if (SafeRead(base + 0x10 + 8u * (uintptr_t)k, &at, 8) || at == 0 || SafeRead(at, desc, sizeof desc)) continue;
        uint64_t data = ((uint64_t)desc[5] << 32) | desc[4];
        Note("difficulty: scatter image %d: %u x %u (%08X %08X) data 0x%llX", k, desc[0], desc[1], desc[2], desc[3], (unsigned long long)data);
        if (data == 0 || desc[0] == 0 || desc[1] == 0 || desc[0] > 8192 || desc[1] > 8192 || k == 0 || !g_scatterDump) continue;
        uint64_t bytes = (uint64_t)desc[0] * desc[1] * 4u;
        uint8_t* copy = (uint8_t*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (copy == NULL) continue;
        if (!SafeRead(data, copy, bytes))
        {
            wchar_t name[64];
            swprintf(name, 64, L"scatter-%d-%dx-%ld.bin", k, (int)g_world.current, (long)notes);
            HANDLE file = OpenLog(name);
            if (file != INVALID_HANDLE_VALUE) { DWORD wrote; WriteFile(file, copy, (DWORD)bytes, &wrote, NULL); CloseHandle(file); }
        }
        VirtualFree(copy, 0, MEM_RELEASE);
    }
    for (uint32_t t = 0; t < count && t < 64 && notes == 1; t++)
    {
        uintptr_t type = g_world.game + 0x3328F90u + (uintptr_t)t * 0x2860u;
        float f[16] = {0};
        uint64_t grid = 0;
        uint32_t gd[6] = {0};
        SafeRead(type, f, sizeof f);
        SafeRead(type + 0x2850, &grid, 8);
        if (grid != 0) SafeRead(grid, gd, sizeof gd);
        uint32_t* u = (uint32_t*)f;
        float g1 = 0, g2 = 0; memcpy(&g1, &gd[1], 4); memcpy(&g2, &gd[2], 4);
        Note("difficulty: scatter type %u: %g %g %g %g %g %g %g %g | %08X %08X %08X %08X %08X %08X %08X %08X | grid dim %u, %g, %g, data %08X%08X",
            t, (double)f[0], (double)f[1], (double)f[2], (double)f[3], (double)f[4], (double)f[5], (double)f[6], (double)f[7],
            u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15], gd[0], (double)g1, (double)g2, gd[5], gd[4]);
    }
}

/* Research (ground types): the subregion cells (G+0x92F328, 0x20 each, count G+0x11A71C4; +0xC the type, the zone id the
 * scatter brushes match: 0x41/0x43/0x44/0x46 on planet 157; +0x18 the route cell, its position at +0/+4), written out as
 * subregions-<level>.tsv and summarised, vanilla and larger. */
/* Research (visual checks): a drop point in the vegetated zone (height group 1), on land: the group-1 cell at the 70th
 * percentile of their nearest Voronoi vertex's height (the low ones hold water), written as "x,y" in world metres (the
 * generator's coordinates less half the world, as SpawnStamps places units) to xl-drop.txt when the level's first
 * SetupTerrainMerge (op 0x38) begins. A larger level generates during the briefing; the harness waits for the file
 * before choosing the drop (HD2H_DROP=file:...). */
static int WorldDropCompare(const void* a, const void* b)
{
    float x = ((const float*)a)[2], y = ((const float*)b)[2];
    return x < y ? -1 : x > y;
}

static float g_worldDrop[2];
static LONG g_worldDropLevel = -1;
static void WorldSuggestDrop(uintptr_t gen, uint32_t count)
{
    static LONG suggested = -1;
    if (suggested == g_world.levels || count == 0 || count > 8192) return;
    suggested = g_world.levels;
    /* Heights: the main Voronoi's vertices (G+0x9F310, 0x48 each, x/y at +0; count G+0x11A7090) and their heights
     * (G+0x1F8A30, floats; 0..~0.08 before the settings' x512), set before SetupTerrainMerge (op 0x38). */
    uint32_t vertices = 0;
    if (SafeRead(gen + 0x11A7090u, &vertices, 4) || vertices == 0 || vertices > 65536) return;
    static float vxy[65536][2], vh[65536];
    for (uint32_t v = 0; v < vertices; v++)
        if (SafeRead(gen + 0x9F310u + 0x48u * (uintptr_t)v, vxy[v], 8) || SafeRead(gen + 0x1F8A30u + 4u * (uintptr_t)v, &vh[v], 4)) { vertices = v; break; }
    { /* relief: the vertex heights' spread over the whole level (the terrain's shape before the merge) */
        double sum = 0, sum2 = 0; float lo = 3e38f, hi = -3e38f; uint32_t m = 0;
        for (uint32_t v = 0; v < vertices; v++) if (vh[v] > -1e30f && vh[v] < 1e30f) { sum += vh[v]; sum2 += (double)vh[v] * vh[v]; m++; if (vh[v] < lo) lo = vh[v]; if (vh[v] > hi) hi = vh[v]; }
        double mean = m ? sum / m : 0, sd = m ? sqrt(sum2 / m - mean * mean) : 0;
        Note("difficulty: relief (%.0fx): %u vertices, heights %.4f..%.4f, mean %.4f, sd %.4f (x512: sd %.1f m)", (double)g_world.current, m, (double)lo, (double)hi, mean, sd, sd * 512.0);
    }
    static float picks[8192][3];
    uint32_t n = 0;
    for (uint32_t i = 0; i < count && n < 8192; i++)
    {
        uint8_t entry[0x20];
        uint64_t cell = 0;
        float pos[2];
        if (SafeRead(gen + 0x92F328u + 0x20u * (uintptr_t)i, entry, sizeof entry)) break;
        int32_t group; memcpy(&group, entry + 4, 4); memcpy(&cell, entry + 0x18, 8);
        if (group != 1 || cell == 0 || SafeRead(cell, pos, 8)) continue;
        float best = 3.0e38f, h = 0.0f;
        for (uint32_t v = 0; v < vertices; v++)
        {
            float dx = vxy[v][0] - pos[0], dy = vxy[v][1] - pos[1], d = dx * dx + dy * dy;
            if (d < best) { best = d; h = vh[v]; }
        }
        if (!(h > -1e30f && h < 1e30f)) continue;
        picks[n][0] = pos[0]; picks[n][1] = pos[1]; picks[n][2] = h; n++;
    }
    { /* relief per height group and type: nearest-vertex heights of every subregion cell */
        static double gs[8][3]; static uint32_t gt[8][8]; memset(gs, 0, sizeof gs); memset(gt, 0, sizeof gt);
        static const int32_t kTypes[8] = {0x41, 0x43, 0x44, 0x46, 0x47, 0x1E, -1, -2};
        for (uint32_t i = 0; i < count; i++)
        {
            uint8_t entry[0x20]; uint64_t cell = 0; float pos[2];
            if (SafeRead(gen + 0x92F328u + 0x20u * (uintptr_t)i, entry, sizeof entry)) break;
            int32_t group, type; memcpy(&group, entry + 4, 4); memcpy(&type, entry + 0xC, 4); memcpy(&cell, entry + 0x18, 8);
            if (cell == 0 || SafeRead(cell, pos, 8) || group < -1 || group > 6) continue;
            float best = 3.0e38f, h = 0.0f;
            for (uint32_t v = 0; v < vertices; v++) { float dx = vxy[v][0] - pos[0], dy = vxy[v][1] - pos[1], d = dx * dx + dy * dy; if (d < best) { best = d; h = vh[v]; } }
            gs[group + 1][0] += 1; gs[group + 1][1] += h; gs[group + 1][2] += (double)h * h;
            int t = 6; for (int k = 0; k < 6; k++) if (kTypes[k] == type) t = k; gt[group + 1][t]++;
        }
        for (int g = 0; g < 8; g++) if (gs[g][0] > 0)
        {
            double mean = gs[g][1] / gs[g][0], sd = sqrt(gs[g][2] / gs[g][0] - mean * mean);
            Note("difficulty: relief (%.0fx): group %d: %.0f cells, height mean %.4f sd %.4f; types 41:%u 43:%u 44:%u 46:%u 47:%u 1E:%u other:%u",
                (double)g_world.current, g - 1, gs[g][0], mean, sd, gt[g][0], gt[g][1], gt[g][2], gt[g][3], gt[g][4], gt[g][5], gt[g][6]);
        }
    }
    if (n == 0) { Note("difficulty: drop suggestion: no vegetated cell"); return; }
    qsort(picks, n, sizeof picks[0], WorldDropCompare);
    uint32_t k = (uint32_t)((float)n * 0.7f); if (k >= n) k = n - 1;
    float half = 1024.0f * g_world.current, x = picks[k][0] - half, y = picks[k][1] - half;
    g_worldDrop[0] = x; g_worldDrop[1] = y; g_worldDropLevel = g_world.levels;
    char line[64];
    int len = snprintf(line, sizeof line, "%.1f,%.1f\n", (double)x, (double)y);
    (void)line; (void)len; /* the drop file is written once the final heightmap and the water are known (TerrainNearDrop) */
    Note("difficulty: drop suggestion: (%.0f, %.0f) in the vegetated zone, vertex height %.4f (%u cells, %.4f..%.4f)",
        (double)x, (double)y, (double)picks[k][2], n, (double)picks[0][2], (double)picks[n - 1][2]);
}

static uint64_t g_pass74, g_ctx74, g_pass76;
static void WorldSubregionNote(void)
{
    static LONG noted = -1;
    if (noted == g_world.levels || g_worldLevel == 0) return;
    noted = g_world.levels;
    uintptr_t gen = 0;
    uint32_t count = 0;
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(gen + 0x11A71C4u, &count, 4) || count == 0 || count > 8192) return;
    wchar_t name[48];
    swprintf(name, 48, L"subregions-%ld.tsv", (long)g_world.levels);
    HANDLE file = OpenLog(name);
    uint32_t types[256] = {0};
    static uint32_t byGroup[8][256]; /* height group (0x74/0x77, entry +4; -1..6) x type */
    memset(byGroup, 0, sizeof byGroup);
    char line[160];
    DWORD wrote;
    for (uint32_t i = 0; i < count; i++)
    {
        uint8_t entry[0x20];
        uint64_t cell = 0;
        float pos[2] = {0};
        if (SafeRead(gen + 0x92F328u + 0x20u * (uintptr_t)i, entry, sizeof entry)) break;
        int32_t type, group; memcpy(&type, entry + 0xC, 4); memcpy(&group, entry + 0x4, 4); memcpy(&cell, entry + 0x18, 8);
        if (cell) SafeRead(cell, pos, 8);
        if (type >= 0 && type < 256) types[type]++;
        if (group >= -1 && group < 7 && type >= 0 && type < 256) byGroup[group + 1][type]++;
        if (file != INVALID_HANDLE_VALUE)
        {
            int n = snprintf(line, sizeof line, "%u\t%d\t%d\t%.1f\t%.1f\n", i, type, group, (double)pos[0], (double)pos[1]);
            WriteFile(file, line, (DWORD)n, &wrote, NULL);
        }
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    /* op 0x74's inputs: the kind byte per subregion ([pass+0x10][entry+8]), the region's settings ([[context+8]+0xB0]):
     * the four noise weights (+0xC..+0x18, binned from +0x18), the random share +0x20, kinds at +0x80 (0x1C each: the
     * kind byte at +0, flags at +0x18, flag 2 = group 1). */
    uint64_t kinds = 0, region = 0, contextSettings = 0;
    if (g_pass74 && !SafeRead(g_pass74 + 0x10, &kinds, 8) && kinds && g_ctx74 && !SafeRead(g_ctx74 + 8, &contextSettings, 8) && contextSettings &&
        !SafeRead(contextSettings + 0xB0, &region, 8) && region)
    {
        float w[6] = {0};
        SafeRead(region + 0xC, w, sizeof w);
        uint32_t kindCount[256] = {0}, kindGroup1[256] = {0};
        for (uint32_t i = 0; i < count; i++)
        {
            uint8_t entry[0x20]; uint8_t kind = 0xFF;
            if (SafeRead(gen + 0x92F328u + 0x20u * (uintptr_t)i, entry, sizeof entry)) break;
            uint32_t idx; int32_t group; memcpy(&idx, entry + 8, 4); memcpy(&group, entry + 4, 4);
            if (idx != 0xFFFFFFFFu) SafeRead(kinds + idx, &kind, 1);
            kindCount[kind]++; if (group == 1) kindGroup1[kind]++;
        }
        char ks[300]; int ka = 0;
        for (int k = 0; k < 256 && ka < 260; k++) if (kindCount[k]) ka += snprintf(ks + ka, sizeof ks - ka, " %d:%u(g1 %u)", k == 255 ? -1 : k, kindCount[k], kindGroup1[k]);
        ks[ka] = 0;
        Note("difficulty: subregions (%.0fx): weights %.3f %.3f %.3f %.3f, random %.3f; kinds%s", (double)g_world.current, (double)w[3], (double)w[2], (double)w[1], (double)w[0], (double)w[5], ks);
        ka = 0;
        for (int k = 0; k < 7; k++)
        {
            uint8_t e[0x1C] = {0};
            SafeRead(region + 0x80 + 0x1C * (uintptr_t)k, e, sizeof e);
            ka += snprintf(ks + ka, sizeof ks - ka, " [%u f%02X]", e[0], e[0x18]);
        }
        ks[ka] = 0;
        Note("difficulty: subregions (%.0fx): kind table%s", (double)g_world.current, ks);
        /* op 0x76's gate: its entries ([pass+0x10], 0xE0 each, [[pass+0x50]] of them; up to 24 route cells inline from
         * +0x10, their count at +0xD0): with none holding a cell no kind is painted (0xF6609F); the ring's radius is
         * world / 2 x [settings+0x50] x the kind's fraction (+0x10). */
        uint64_t base76 = 0, countAt = 0, dirAt = 0; uint32_t n76 = 0; float s50 = 0, dir[2] = {0}, frac = 0;
        SafeRead(contextSettings + 0x50, &s50, 4); SafeRead(region + 0x80 + 0x10, &frac, 4);
        if (g_pass76 && !SafeRead(g_pass76 + 0x10, &base76, 8) && !SafeRead(g_pass76 + 0x50, &countAt, 8) && countAt && !SafeRead(countAt, &n76, 4))
        {
            if (!SafeRead(g_pass76 + 0x28, &dirAt, 8) && dirAt) SafeRead(dirAt, dir, 8);
            uint32_t withCells = 0, cellsTotal = 0;
            for (uint32_t k = 0; k < n76 && k < 4096; k++)
            {
                uint32_t c = 0;
                if (SafeRead(base76 + 0xE0u * (uintptr_t)k + 0xD0, &c, 4)) break;
                if (c) withCells++;
                cellsTotal += c;
            }
            Note("difficulty: subregions (%.0fx): op 0x76: %u entries, %u with route cells (%u cells); settings+0x50 %.3f, kind 1 fraction %.3f, direction %.3f %.3f",
                (double)g_world.current, n76, withCells, cellsTotal, (double)s50, (double)frac, (double)dir[0], (double)dir[1]);
        }
    }
    char summary[400]; int at = 0;
    for (int t = 0; t < 256 && at < 360; t++) if (types[t]) at += snprintf(summary + at, sizeof summary - at, " 0x%X:%u", t, types[t]);
    summary[at] = 0;
    Note("difficulty: subregions (%.0fx): %u cells, types%s", (double)g_world.current, count, summary);
    for (int g = 0; g < 8; g++)
    {
        at = 0;
        for (int t = 0; t < 256 && at < 360; t++) if (byGroup[g][t]) at += snprintf(summary + at, sizeof summary - at, " 0x%X:%u", t, byGroup[g][t]);
        summary[at] = 0;
        if (at) Note("difficulty: subregions (%.0fx): group %d:%s", (double)g_world.current, g - 1, summary);
    }
}

/* difficulty_world_diag.c: terrain diagnostics around every dispatcher step (HELLFORGE_TERRAIN_DIAG) */
static void TerrainDiagBefore(uint64_t records, uint64_t index, uint32_t end, uint32_t* at, uint32_t* op);
static void TerrainDiagInstall(uintptr_t game);
static void ScatterBakeInstall(void); /* difficulty_world_diag.c */
/* difficulty_world_terrain.c: the map at vanilla density, as tiles drawn into the level's merge */
static void TerrainInstall(uintptr_t game);
static int TerrainStep(uint64_t records, uint32_t end, uint32_t at, uint64_t context, uint64_t g, uint64_t h);
static void MacroStamps(void); /* difficulty_world_macro.c */
static int TerrainNearStep(uint64_t records, uint32_t end, uint32_t at, uint32_t op, uint64_t context, uint64_t g, uint64_t h);
static void NearOneMipLevel(void);
static void NearOneMipDrop(void);
static int TerrainOn(void);
static int TerrainMatDispatch(uint32_t op, uint64_t context);
static void TerrainMatPatch(int on);
static void TerrainNearPads(uint64_t record, uint64_t context);
static void TerrainNearWater(uint64_t record);
static void TerrainStampsRecord(uint64_t record, uint32_t op);
static void TerrainStamps(uint64_t records, uint32_t end, uint32_t at91, uint64_t context, uint64_t g, uint64_t h, uintptr_t gen);
static void TerrainDiagAfter(uint64_t records, uint64_t index, uint32_t at, uint32_t op);
/* difficulty_world_towns.c: towns at a vanilla town's shape, x3 */
static void TownsArm(uint64_t records, uint32_t at, uint64_t context, uintptr_t gen);
static void TownsAfter(uint64_t records, uint32_t at, uint64_t index, uint64_t context, uint64_t g, uint64_t h, uintptr_t gen);
static void TownsForget(uint64_t pass);

/* The structured maps' route sites on a larger world (the city frame). GenerateRoutes (op 0x07, 0xF35FF0) on map type 2
 * makes its sites with the template (0xFB9610) or the grid (0xC129B0, when Z+0x678 == 1) over the site rect (S+0x64 /
 * S+0x68), keeps those within S+0x6C of the world's centre (0xF361F6..0xF36264), keeps 0.2 of those past S+0x54 x W / 2
 * (0xF36292..0xF3630D) and builds the route cells from them (0xFD03E0). On the 16 km world the template, laid over the
 * whole rect, either spanned the map (a vanilla city blown up 12x) or, with its template-space 1024 retargeted to the
 * world's half (an old world_sites.h entry), sat ~10 km off centre, outside the keep disk: 0 sites, 0 route cells, and
 * LinkLocationsToRouteCells wrote through a null route cell (0xC186AA; runs s73q-s73t). Each maker now gets a vanilla
 * rect (CITY_FRAME_K x vanilla's keep radius about the centre) and its sites vanilla's two disk tests. Each call notes
 * the sites' count, box and how many the level's keep disk holds. */
typedef uint64_t (*SiteTemplateFn)(uint64_t, float, uint64_t, uint64_t, uint64_t, uint64_t, uint32_t*);
typedef uint64_t (*SiteGridFn)(float, int32_t, float, int32_t, float, uint64_t, float, uint64_t, uint64_t, uint32_t*);
static SiteTemplateFn g_siteTemplate;
static SiteGridFn g_siteGrid;
static void SiteNote(const char* maker, uint64_t points, uint32_t* count, const char* how)
{
    uint32_t n = 0;
    if (count == NULL || SafeRead((uint64_t)(uintptr_t)count, &n, 4)) return;
    uint32_t W = 0; float keep = 0.0f, lo = 0.0f, hi = 0.0f;
    SafeRead(g_worldLevel + 0x2D20 + 0x2C, &W, 4); SafeRead(g_worldLevel + 0x2D20 + 0x6C, &keep, 4);
    SafeRead(g_worldLevel + 0x2D20 + 0x64, &lo, 4); SafeRead(g_worldLevel + 0x2D20 + 0x68, &hi, 4);
    float box[4] = {1e30f, 1e30f, -1e30f, -1e30f}, c = 0.5f * (float)W;
    uint32_t inside = 0;
    for (uint32_t i = 0; i < n && i < 65536; i++)
    {
        float p[2];
        if (SafeRead(points + 8ull * i, p, 8)) break;
        if (p[0] < box[0]) box[0] = p[0]; if (p[1] < box[1]) box[1] = p[1]; if (p[0] > box[2]) box[2] = p[0]; if (p[1] > box[3]) box[3] = p[1];
        if ((p[0] - c) * (p[0] - c) + (p[1] - c) * (p[1] - c) <= keep * keep) inside++;
    }
    Note("difficulty: route sites (%s, %s): %u sites, box (%.0f, %.0f)..(%.0f, %.0f), %u within %.0f m of (%.0f, %.0f); the rect %.0f..%.0f, world %u",
        maker, how, n, (double)box[0], (double)box[1], (double)box[2], (double)box[3], inside, (double)keep, (double)c, (double)c, (double)lo, (double)hi, W);
}
/* The city's sites in the frame: kept within CITY_FRAME_K x vanilla's keep radius of the centre, and between the fade and
 * it with chance 0.2 (GenerateRoutes' own two tests, 0xF361F6..0xF3630D, in vanilla units; there the random keep is
 * the level's PCG, here a hash of the site, the same on every peer). The game's tests at the larger radii then keep all. */
static void CityFrameFilter(uint64_t points, uint32_t* count, float cx, float cy)
{
    uint32_t n = 0, kept = 0;
    if (SafeRead((uint64_t)(uintptr_t)count, &n, 4) || n > 65536) return;
    const float keep = CITY_FRAME_K * CITY_VANILLA_KEEP, fade = CITY_FRAME_K * CITY_VANILLA_FADE;
    for (uint32_t i = 0; i < n; i++)
    {
        float p[2]; uint32_t bits[2];
        if (SafeRead(points + 8ull * i, p, 8)) return;
        float d2 = (p[0] - cx) * (p[0] - cx) + (p[1] - cy) * (p[1] - cy);
        if (d2 > keep * keep) continue;
        memcpy(bits, p, 8);
        uint32_t hash = (bits[0] * 0x9E3779B1u + bits[1]) * 0x85EBCA6Bu;
        hash ^= hash >> 13; hash *= 0xC2B2AE35u; hash ^= hash >> 16;
        if (d2 > fade * fade && hash > (uint32_t)(0.2 * 4294967295.0)) continue;
        memcpy((void*)(uintptr_t)(points + 8ull * kept), p, 8);
        kept++;
    }
    *count = kept;
}
/* An operation's three cities on a larger world: three city frames about seed-placed centres at half the playable radius,
 * 120 degrees apart, and open land between them (sites on a jittered grid, clear of the cities). The route graph's
 * nodes and edges (moved by difficulty_block.c: 2304 and 3456, ~2 and ~3 per cell) hold the Voronoi of up to 1024 cells, the
 * route cells' own room; the open land gets what ~CITY_CELLS cells leave. The level's centres stay for its zone layout (WorldCityZones). */
#define CITY_COUNT 3
#define CITY_CELLS 900
static struct { LONG level; int count; float x[CITY_COUNT], y[CITY_COUNT], radius; uint32_t sites, rings, open; float spacing; } g_cities = {-1};
static float g_citySites[2 * 1024];
static void CityCentres(float cx, float cy)
{
    uint32_t seed = 0; float s50 = 0.0f; uint32_t W = 0;
    SafeRead(g_worldLevel + 0x8, &seed, 4); SafeRead(g_worldLevel + 0x2D20 + 0x50, &s50, 4); SafeRead(g_worldLevel + 0x2D20 + 0x2C, &W, 4);
    uint32_t h = seed * 0x9E3779B1u; h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13;
    float a0 = (float)(h & 0xFFFFFF) / 16777216.0f * 6.2831853f, ring = 0.5f * s50 * 0.5f * (float)W;
    g_cities.level = g_world.levels; g_cities.count = CITY_COUNT; g_cities.radius = CITY_FRAME_K * CITY_VANILLA_KEEP;
    for (int i = 0; i < CITY_COUNT; i++)
    {
        float a = a0 + 6.2831853f * (float)i / (float)CITY_COUNT;
        g_cities.x[i] = cx + ring * cosf(a); g_cities.y[i] = cy + ring * sinf(a);
    }
}
/* The city sites gathered so far, each city's transition rings, then the open land, into g_citySites; returns the count.
 * A route cell has 16 edge slots (+0x8..+0x80), filled without a check, and SpreadLocations_Cities walks them to a null
 * (0xFB4365..0xFB4370): an open-land cell ~660 m wide next to a city's ~100-200 m fringe cells had more than 16
 * neighbours, its 17th edge overwrote the cell's box (+0x88) and op 0x8C read that box as an edge (0xFB4320; s73w,
 * s74d). Between the fringe (vanilla's 0.2 fade ring, ~210 m apart) and the open land, two rings of sites ~1.6x and
 * ~2.6x the fringe spacing keep neighbouring cells within ~2x of each other in size. The open land takes what CITY_CELLS
 * leaves, on a jittered grid clear of the rings. */
#define CITY_FRINGE 210.0f
static uint32_t CityRing(uint32_t n, float x, float y, float r, float spacing, uint32_t salt)
{
    int k = (int)(6.2831853f * r / spacing + 0.5f);
    float a0 = (float)((salt * 0x9E3779B1u) >> 8) / 16777216.0f * 6.2831853f;
    for (int i = 0; i < k && n < 1024; i++, n++)
    {
        float a = a0 + 6.2831853f * (float)i / (float)k;
        g_citySites[2 * n] = x + r * cosf(a); g_citySites[2 * n + 1] = y + r * sinf(a);
    }
    return n;
}
static uint32_t CityOpenLand(uint32_t n, float cx, float cy)
{
    float keep = 0.0f, fade = 0.0f, W = 0.0f; uint32_t w = 0, seed = 0;
    SafeRead(g_worldLevel + 0x2D20 + 0x6C, &keep, 4); SafeRead(g_worldLevel + 0x2D20 + 0x54, &fade, 4); SafeRead(g_worldLevel + 0x2D20 + 0x2C, &w, 4);
    SafeRead(g_worldLevel + 0x8, &seed, 4);
    W = (float)w; fade *= 0.5f * W;
    if (fade > keep) fade = keep;
    const float outer = fade - 20.0f;
    /* the rings: spacing s1, s2 at radii half a spacing past the previous edge */
    const float s1 = 1.6f * CITY_FRINGE, s2 = 2.6f * CITY_FRINGE;
    const float r1 = g_cities.radius + 0.5f * (CITY_FRINGE + s1), r2 = r1 + 0.5f * (s1 + s2);
    uint32_t cities = n;
    for (int k = 0; k < g_cities.count; k++)
    {
        n = CityRing(n, g_cities.x[k], g_cities.y[k], r1, s1, seed + 2u * (uint32_t)k);
        n = CityRing(n, g_cities.x[k], g_cities.y[k], r2, s2, seed + 2u * (uint32_t)k + 1u);
    }
    g_cities.rings = n - cities;
    uint32_t want = n < CITY_CELLS ? CITY_CELLS - n : 0;
    g_cities.open = 0;
    if (want < 8 || outer < 1000.0f) return n;
    const float clear = r2 + 0.5f * s2;
    float area = 3.14159265f * outer * outer - (float)g_cities.count * 3.14159265f * clear * clear;
    float sp = sqrtf(area / (float)want);
    if (sp < s2) sp = s2;
    g_cities.spacing = sp;
    uint32_t added = 0;
    int cells = (int)(outer / sp) + 1;
    for (int j = -cells; j <= cells && n < CITY_CELLS + 64u; j++)
        for (int i = -cells; i <= cells && n < CITY_CELLS + 64u; i++)
        {
            uint32_t hash = ((uint32_t)(i + 4096) * 0x9E3779B1u) ^ ((uint32_t)(j + 4096) * 0x85EBCA6Bu) ^ (seed * 0xC2B2AE35u);
            hash ^= hash >> 15; hash *= 0xC2B2AE35u; hash ^= hash >> 13;
            float jx = ((float)(hash & 0xFFFF) / 65536.0f - 0.5f) * 0.6f, jy = ((float)(hash >> 16) / 65536.0f - 0.5f) * 0.6f;
            float x = cx + ((float)i + jx) * sp, y = cy + ((float)j + jy) * sp;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > outer * outer) continue;
            int close = 0;
            for (int k = 0; k < g_cities.count && !close; k++)
            {
                float d = clear + 0.5f * sp;
                close = (x - g_cities.x[k]) * (x - g_cities.x[k]) + (y - g_cities.y[k]) * (y - g_cities.y[k]) < d * d;
            }
            if (close) continue;
            g_citySites[2 * n] = x; g_citySites[2 * n + 1] = y; n++; added++;
        }
    g_cities.open = added;
    return n;
}
/* one city's sites, from the maker's array, in its frame's disk, appended */
static uint32_t CityAppend(uint32_t n, uint64_t points, uint32_t* count, float cx, float cy)
{
    SiteNote("a city's frame", points, count, "as made");
    CityFrameFilter(points, count, cx, cy);
    uint32_t m = 0;
    if (SafeRead((uint64_t)(uintptr_t)count, &m, 4)) return n;
    for (uint32_t i = 0; i < m && n < 1024; i++) if (!SafeRead(points + 8ull * i, &g_citySites[2 * n], 8)) n++;
    return n;
}
/* A terrain tile's route sites on a city map: the tile is the plan's nature terrain (kTileOps), so it takes the sites
 * an open map's tile gets, GenerateRoutes' non-structured maker 0xC126C0 (rcx = map type 1: 0 here, its random path
 * 0xC12974; rdx arena, xmm2 the spacing [[[lg+0x2D0]+0x68]+4], r9 the rect, then rng and the count; the call 0xF36125..
 * 0xF36152), not a city template per 896 m tile: one tile's FinalizeVoronoi failing restarts the whole level (the tile
 * shares the level's context: [lg+0xF954]++, [[ctx+0x58]+0x19B0EA] = 1 at 0xF6811C; s74h). */
typedef uint64_t (*SiteNatureFn)(uint64_t, uint64_t, float, uint64_t, uint64_t, uint32_t*);
static uint64_t CityTileSites(uint64_t arena, uint64_t box, uint64_t rng, uint32_t* count, int grid, uint64_t template, float scale, uint64_t kind, uint64_t region)
{
    static int nature = -1;
    if (nature < 0) nature = ApiCodeMatches(g_world.game + 0xC126C0u, "48 8B C4 53 55 56 48 81 EC 90 00 00 00 48 8B 9C");
    uint64_t holder = 0, zone = 0; float spacing = 0.0f;
    if (g_world.current > 1.0f && nature == 1 && !SafeRead(g_worldLevel + 0x2D0, &holder, 8) && holder != 0 && !SafeRead(holder + 0x68, &zone, 8) && zone != 0 &&
        !SafeRead(zone + 4, &spacing, 4) && spacing > 1.0f && spacing < 1000.0f)
    {
        static LONG noted = -1;
        if (noted != g_world.levels) { noted = g_world.levels; Note("difficulty: extra large maps: a city map's terrain tiles take nature route sites (0xC126C0, %.1f m apart)", (double)spacing); }
        return ((SiteNatureFn)(g_world.game + 0xC126C0u))(0, arena, spacing, box, rng, count);
    }
    if (grid) return 0;
    return ((SiteTemplateFn)(uintptr_t)template)(arena, scale, kind, region, box, rng, count);
}
/* The level's own world (2048 m x the larger maps' factor, 16384 m at 8x) and rect (GenerateRoutes over it: S+0x64..
 * S+0x68), not a terrain tile's: a tile runs the level's records with its own settings (world 896 m, rect 0..896; s74e),
 * where three city frames overran its weld buckets (FinalizeVoronoi failed, the level restarted). Only the level gets
 * the cities; a tile keeps the game's maker. */
static int CityLevelRect(uint64_t box)
{
    float given[4] = {0, 0, 0, 0}; uint32_t w = 0;
    if (SafeRead(box, given, 16) || SafeRead(g_worldLevel + 0x2D20 + 0x2C, &w, 4)) return 0;
    if ((float)w < 0.99f * 2048.0f * g_world.current) return 0;
    return given[2] - given[0] >= 0.5f * (float)w && given[3] - given[1] >= 0.5f * (float)w;
}
static void CityDone(uint32_t n, uint32_t cities, uint32_t* count, const char* maker)
{
    *count = n;
    g_cities.sites = cities;
    Note("difficulty: extra large maps: %d cities (%s) at (%.0f, %.0f) (%.0f, %.0f) (%.0f, %.0f), %u city sites, %u ring sites, %u open-land sites %.0f m apart; %u route sites",
        g_cities.count, maker, (double)g_cities.x[0], (double)g_cities.y[0], (double)g_cities.x[1], (double)g_cities.y[1], (double)g_cities.x[2], (double)g_cities.y[2],
        cities, g_cities.rings, g_cities.open, (double)g_cities.spacing, n);
}
static uint64_t WorldSiteTemplate(uint64_t arena, float scale, uint64_t kind, uint64_t region, uint64_t box, uint64_t rng, uint32_t* count)
{
    if (g_world.current <= 1.0f) return g_siteTemplate(arena, scale, kind, region, box, rng, count);
    if (!CityLevelRect(box)) return CityTileSites(arena, box, rng, count, 0, (uint64_t)(uintptr_t)g_siteTemplate, scale, kind, region);
    /* the template in template space (0..2048 about 1024, x rect width / 2048): a vanilla rect about each city's centre */
    float given[4] = {0, 0, 0, 0};
    SafeRead(box, given, 16);
    const float cx = 0.5f * (given[0] + given[2]), cy = 0.5f * (given[1] + given[3]), r = CITY_FRAME_K * CITY_VANILLA_KEEP;
    CityCentres(cx, cy);
    uint32_t n = 0;
    for (int k = 0; k < g_cities.count; k++)
    {
        float frame[4] = {g_cities.x[k] - r, g_cities.y[k] - r, g_cities.x[k] + r, g_cities.y[k] + r};
        uint64_t points = g_siteTemplate(arena, scale, kind, region, (uint64_t)(uintptr_t)frame, rng, count);
        if (points == 0) return points;
        n = CityAppend(n, points, count, g_cities.x[k], g_cities.y[k]);
    }
    uint32_t cities = n;
    n = CityOpenLand(n, cx, cy);
    CityDone(n, cities, count, "template 0xFB9610");
    return (uint64_t)(uintptr_t)g_citySites;
}
static uint64_t WorldSiteGrid(float a, int32_t b, float c, int32_t d, float e, uint64_t arena, float spacing, uint64_t box, uint64_t rng, uint32_t* count)
{
    if (g_world.current <= 1.0f) return g_siteGrid(a, b, c, d, e, arena, spacing, box, rng, count);
    if (!CityLevelRect(box)) { uint64_t sites = CityTileSites(arena, box, rng, count, 1, 0, 0, 0, 0); return sites != 0 ? sites : g_siteGrid(a, b, c, d, e, arena, spacing, box, rng, count); }
    /* the grid at the structured type's own spacing (Z+0x67C, Z = [S+0xB0]: not the larger maps' cell factor) over each frame */
    float given[4] = {0, 0, 0, 0}, own = 0.0f;
    uint64_t Z = 0;
    SafeRead(box, given, 16);
    if (!SafeRead(g_worldLevel + 0x2D20 + 0xB0, &Z, 8) && Z != 0 && !SafeRead(Z + 0x67C, &own, 4) && own > 1.0f && own < 1000.0f) spacing = own * CITY_FRAME_K;
    const float cx = 0.5f * (given[0] + given[2]), cy = 0.5f * (given[1] + given[3]), r = CITY_FRAME_K * CITY_VANILLA_KEEP;
    CityCentres(cx, cy);
    uint32_t n = 0;
    for (int k = 0; k < g_cities.count; k++)
    {
        float frame[4] = {g_cities.x[k] - r, g_cities.y[k] - r, g_cities.x[k] + r, g_cities.y[k] + r};
        uint64_t points = g_siteGrid(a, b, c, d, e, arena, spacing, (uint64_t)(uintptr_t)frame, rng, count);
        if (points == 0) return points;
        n = CityAppend(n, points, count, g_cities.x[k], g_cities.y[k]);
    }
    uint32_t cities = n;
    n = CityOpenLand(n, cx, cy);
    CityDone(n, cities, count, "grid 0xC129B0");
    return (uint64_t)(uintptr_t)g_citySites;
}

/* The cities' zones (op 0xB9 when Z+0x684 bit 0 is set: 0xFB8DD0 maps each route cell's site, +0, through the zone
 * template about the world's centre, 0xFB901B): each city's cells are moved into the centre for the op and back after;
 * the open land's cells, outside every city, get an open zone (WorldCityOpenZones). */
/* The route cells' edge slots (16 at +0x8, unchecked): the largest count and the cells that filled all 16, once per
 * level after the route graph is made (op 0x07) and after FinalizeVoronoi (0xA4). */
static void WorldCellEdges(uintptr_t gen, uint32_t op)
{
    uint32_t cells = 0, most = 0, full = 0;
    if (SafeRead(gen + 0x11A71C0u, &cells, 4) || cells > 1024) return;
    for (uint32_t i = 0; i < cells; i++)
    {
        uint64_t slots[16];
        if (SafeRead(gen + 0x8EB328u + 0x110ull * i + 8, slots, sizeof slots)) break;
        uint32_t k = 0;
        while (k < 16 && slots[k] != 0) k++;
        if (k > most) most = k;
        if (k == 16) full++;
    }
    Note("difficulty: extra large maps: route cells' edges after op 0x%X: at most %u of 16 slots, %u cells full (%u cells)", op, most, full, cells);
}
/* The location budget on a city map, after the ops that fill it (0xAB road stamps, 0xA7 buildings, 0x2B weight rules):
 * the count, how many lie in each city (their box corner +0xB0/+0xB4 within 1.5 x the frame), and the stamp index's
 * free slots ([level+0x2C0] + 8: free head +0x2C, -1 = full; entry stride u16 +0x24, entries +0x30, a free entry's
 * first u32 the next; the inserter returns a null handle when full, 0xC04F1C, and its callers write through it). */
/* Research (s74n/s74o: purple strips along the roads): every route edge's road points (edge +0x38 start, +0x3C end,
 * exclusive: 0xFC7253 / 0xFC7267 -> 0xFC726D) against the written points (count G+0x11A71D4), and the points
 * themselves (x, y in generator metres, 0..W), after op 0x2B on every level of a larger map. */
static void WorldRoadCheck(uintptr_t gen)
{
    uint32_t edges = 0, points = 0, w = 0;
    if (SafeRead(gen + 0x11A71CCu, &edges, 4) || SafeRead(gen + 0x11A71D4u, &points, 4)) return;
    SafeRead(g_worldLevel + 0x2D20 + 0x2C, &w, 4);
    uint32_t past = 0, backwards = 0, longest = 0, zero = 0, outside = 0, firstPast = 0xFFFFFFFFu, firstOut = 0xFFFFFFFFu;
    uint32_t pastRange[2] = {0, 0}; float outAt[2] = {0, 0};
    for (uint32_t e = 0; e < edges && e < 65536; e++)
    {
        uint32_t r[2];
        if (SafeRead(gen + g_routeEdges + 0x48ull * e + 0x38, r, 8)) break;
        if (r[1] < r[0]) { backwards++; continue; }
        if (r[1] - r[0] > longest) longest = r[1] - r[0];
        if (r[1] > points) { if (past++ == 0) { firstPast = e; pastRange[0] = r[0]; pastRange[1] = r[1]; } }
    }
    for (uint32_t i = 0; i < points && i < 1u << 20; i++)
    {
        float p[2];
        if (SafeRead(gen + g_routePoints + 8ull * i, p, 8)) break;
        if (p[0] == 0.0f && p[1] == 0.0f) zero++;
        if (!(p[0] >= -100.0f && p[0] <= (float)w + 100.0f && p[1] >= -100.0f && p[1] <= (float)w + 100.0f))
        { if (outside++ == 0) { firstOut = i; outAt[0] = p[0]; outAt[1] = p[1]; } }
    }
    Note("difficulty: roads research: %u edges, %u road points (world %u m): %u ranges past the points (first edge %u: %u..%u), %u backwards, longest %u; "
        "%u points at (0, 0), %u outside the world (first %u at %.1f, %.1f)", edges, points, w, past, firstPast, pastRange[0], pastRange[1], backwards, longest,
        zero, outside, firstOut, (double)outAt[0], (double)outAt[1]);
}

/* Research (s74n, static: kind roll 0xF2CA20 over W = MT+0xF0..+0x100, MT = [[ctx+8]+0xC0], limited to the
 * definition's kinds D+0x198 (u8 list) / D+0x1A0 (count); D = [[loc]+0x28] + A1 (+0xA1) * 0x18 -> [+8] + A2 (+0xA2) *
 * 0x1C8 when loc+8 holds 0xADC32FAA, else [[loc]]; 0xF81201..0xF81223): before op 0x18 on a city level, W and each
 * important location outside the cities with its kind and its definition's kinds. */
static void WorldCityDefKinds(uint64_t context, uintptr_t gen)
{
    static LONG level = -1;
    if (g_cities.level != g_world.levels || g_cities.count == 0 || level == g_world.levels) return;
    level = g_world.levels;
    uint64_t settings = 0, mt = 0; float W[5] = {0};
    if (!SafeRead(context + 8, &settings, 8) && settings != 0 && !SafeRead(settings + 0xC0, &mt, 8) && mt != 0) SafeRead(mt + 0xF0, W, sizeof W);
    uint32_t count = 0; SafeRead(gen + 0x11A715Cu, &count, 4);
    if (count > 256) count = 256;
    char text[1600]; int n = 0; uint32_t outside = 0, withOne = 0;
    for (uint32_t i = 0; i < count && n < (int)sizeof text - 80; i++)
    {
        uint64_t at = gen + g_importantList + 0x108ull * i, holder = 0, D = 0, list = 0; uint32_t tag = 0, kinds = 0; uint8_t a1 = 0xFF, a2 = 0xFF, kind = 0; float p[2];
        if (SafeRead(at, &holder, 8) || SafeRead(at + 8, &tag, 4) || SafeRead(at + 0xA1, &a1, 1) || SafeRead(at + 0xA2, &a2, 1) || SafeRead(at + 0xF0, &kind, 1) || SafeRead(at + 0xA4, p, 8)) continue;
        int inCity = 0;
        for (int k = 0; k < g_cities.count && !inCity; k++) { float dx = p[0] - g_cities.x[k], dy = p[1] - g_cities.y[k]; inCity = dx * dx + dy * dy <= g_cities.radius * g_cities.radius * 1.2f; }
        if (inCity || holder == 0 || a2 == 0xFF) continue;
        if (tag == 0xADC32FAAu) { uint64_t t = 0, arr = 0; if (a1 == 0xFF || SafeRead(holder + 0x28, &t, 8) || t == 0 || SafeRead(t + (uint64_t)a1 * 0x18 + 8, &arr, 8) || arr == 0) continue; D = arr + (uint64_t)a2 * 0x1C8; }
        else if (SafeRead(holder, &D, 8)) continue;
        uint8_t k8[8] = {0};
        if (D == 0 || SafeRead(D + 0x198, &list, 8) || SafeRead(D + 0x1A0, &kinds, 4)) continue;
        if (kinds > 8) kinds = 8;
        if (list != 0) SafeRead(list, k8, kinds);
        outside++;
        int one = 0; for (uint32_t k = 0; k < kinds; k++) one |= k8[k] == 1;
        withOne += one;
        n += snprintf(text + n, sizeof text - n, " {%u kind %u D %u/%u kinds", i, kind, a1, a2);
        for (uint32_t k = 0; k < kinds; k++) n += snprintf(text + n, sizeof text - n, " %u", k8[k]);
        n += snprintf(text + n, sizeof text - n, "}");
    }
    Note("difficulty: kinds research: W %.3g %.3g %.3g %.3g %.3g; %u important locations outside the cities, %u whose definition lists kind 1:%s",
        (double)W[0], (double)W[1], (double)W[2], (double)W[3], (double)W[4], outside, withOne, text);
}

/* Research for the open land's nature variants (KB xl-operation-content "Nature variants", static 2026-11-04), once per
 * city level:
 * R1/R2 after GenerateMissionStampList (op 0x15, 0xF8E530; the op gets the pass at record + 8): holders [pass+0] (8-byte
 * pointers, count [[pass+8]]), categories holder+0x28 (0x18 each: +0 id, +8 definitions, +0x10 count; count holder+0x30),
 * definitions 0x1C8 each (kinds u8 list D+0x198, count D+0x1A0; flags byte D+0xE8; D+0x160; weight f32 D+0x68; POI type
 * list D+0x170 / count D+0x178); the objective set [pass+0x20] (count +0xD4, ids +4, first entry +0x54, entries +0x94; at
 * most 16) and its 16-byte entries [pass+0x18] {u16 holder, u16 category, u16 definition, pad, u32, u32 objective id},
 * total [[pass+0x28]]; ctx+0xD0 picks the D+0xE8 bit (2/3/4 -> bit 0/1/2).
 * R3 before op 0x16: the kind quotas M+0xA0 + 16 (k - 1), M = [[[ctx+0x18]+0x2D0]+0x68] (0xF8F9C0..FAC0).
 * R4/R6 (WorldNatureOps): the order the placement ops finish in, and on which thread. */
static uint32_t NatureKindMask(uint64_t D)
{
    uint64_t list = 0; uint32_t kinds = 0, mask = 0; uint8_t k8[8] = {0};
    if (SafeRead(D + 0x198, &list, 8) || SafeRead(D + 0x1A0, &kinds, 4) || list == 0) return 0;
    if (kinds > 8) kinds = 8;
    if (SafeRead(list, k8, kinds)) return 0;
    for (uint32_t k = 0; k < kinds; k++) if (k8[k] < 32) mask |= 1u << k8[k];
    return mask;
}

/* Research (an id op 0x16 placed none of, s75z ece7ef4e; static, subagent obj15): op 0x15's entries for it (pass:
 * holders +0, holder count [+8], entries +0x18 (16 bytes: u16 holder, category, definition), set S' +0x20 in layout L48:
 * ids +4, starts S'-0xF0, counts +0x100, count +0xD4) and, per entry's definition D (holder +0x28 categories, 0x18 each,
 * definitions +8, 0x1C8 each), what op 0x16's tests read: D+8 key, +0x160 bit 3, capacity +0x15C against the important
 * records with the same holder (+0), category (+0xA1) and definition (+0xA2), the level word +0x60 (min bits 2..5, max
 * 6..9, 0xF none) against [lg+0xF9], faction +0xE8 / +0xE9, +0x70, +0xA8 / +0xB0 / +0xC0 / +0xD0 / +0xD4, kinds, weight
 * +0x68. Read only. */
/* Research (an id op 0x16 places none of although op 0x15 gave it entries, s76c 0c3f8046): op 0x16's definition test,
 * KindFilter 0xF292B0, at its op 0x16 call (0xF8F7B6, back at 0xF8F7BB), replayed on the arguments the game passes
 * (static, Claude 2026-11-05): rcx D, rdx ctx; faction ctx+0xD0 (2/3/4 -> D+0xE8 bit 0/1/2); 0xF28C40(ctx, D,
 * ctx+0xD4); the level word D+0x60 (min bits 2..5, max 6..9, 0xF none) against [[ctx+0x18]+0xF9]; the tag D+0x70 (an
 * entry of M+0x170, 0x38 each, count M+0x178, M = [[[ctx+0x18]+0x2D0]+0x68], replaces D's own +0x70..+0xA7): bit lists
 * +8/+0x10 all, +0x18/+0x20 any, +0x28/+0x30 none against the u64 [[ctx+8]]; byte lists D+0xB0/+0xB8 and D+0xC0/+0xC8
 * against arguments 3 and 4 ({u32 count, pointer}): one shared byte; D+0xD0 in argument 5's u32 list; D+0xD4 in
 * argument 6's or 9 there; argument 9 (a byte; 0 here) against D+0x15E; D+0xA8 in argument 7's bytes; one kind of
 * D+0x198/+0x1A0 with a weight above 0 (argument 10's W[k - 1], else [[ctx+8]+0xC0]+0xEC+4k). Read only. */
#define KF_FN 0xF292B0u
#define KF_RET16 0xF8F7BBu
#define KF_F28C40 0xF28C40u
typedef uint8_t (*KindFilterFn)(uint64_t D, uint64_t ctx, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10);
typedef uint8_t (*KfMaskFn)(uint64_t ctx, uint64_t D, uint32_t sel);
static KindFilterFn g_kindFilter;
static volatile LONG g_kfOn, g_kfCount, g_kfMismatch;
typedef struct { uint64_t D; uint16_t pass; uint8_t result, lvl; uint32_t tag; uint64_t mask, all, any, none; uint8_t dB0[8], aB0[8], dC0[8], aC0[8], nB0[2], nC0[2]; float w[8]; uint8_t kinds[8], nk; } KfRecord;
static KfRecord g_kf[512];
static int KfByteLists(uint64_t dp, uint32_t dn, uint64_t arg, uint8_t* dOut, uint8_t* aOut, uint8_t* counts)
{
    uint32_t an = 0; uint64_t ap = 0;
    if (arg == 0 || SafeRead(arg, &an, 4) || SafeRead(arg + 8, &ap, 8)) an = 0;
    counts[0] = (uint8_t)(dn > 255 ? 255 : dn); counts[1] = (uint8_t)(an > 255 ? 255 : an);
    if (dn && dp) SafeRead(dp, dOut, dn < 8 ? dn : 8);
    if (an && ap) SafeRead(ap, aOut, an < 8 ? an : 8);
    if (dn == 0) return 1;
    for (uint32_t j = 0; j < an && j < 4096; j++)
    {
        uint8_t b = 0; if (SafeRead(ap + j, &b, 1)) break;
        for (uint32_t i = 0; i < dn && i < 4096; i++) { uint8_t c = 0; if (!SafeRead(dp + i, &c, 1) && c == b) return 1; }
    }
    return 0;
}
static uint64_t KfBits(uint64_t p, uint32_t n)
{
    uint64_t m = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) { uint32_t b = 0; if (!SafeRead(p + 4ull * i, &b, 4) && b < 64) m |= 1ull << b; }
    return m;
}
static int KfU32In(uint64_t arg, uint32_t v, int nineAny)
{
    uint32_t n = 0; uint64_t p = 0;
    if (arg == 0 || SafeRead(arg, &n, 4) || SafeRead(arg + 8, &p, 8)) return 0;
    for (uint32_t i = 0; i < n && i < 4096; i++) { uint32_t e = 0; if (!SafeRead(p + 4ull * i, &e, 4) && (e == v || (nineAny && e == 9))) return 1; }
    return 0;
}
static uint8_t KindFilterResearch(uint64_t D, uint64_t ctx, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10)
{
    uint8_t r = g_kindFilter(D, ctx, a3, a4, a5, a6, a7, a8, a9, a10);
    if (!g_kfOn || (uintptr_t)__builtin_return_address(0) != g_world.game + KF_RET16) return r;
    LONG n = g_kfCount;
    for (LONG i = 0; i < n && i < 512; i++) if (g_kf[i].D == D) return r;
    if (n >= 512) return r;
    KfRecord k; memset(&k, 0, sizeof k); k.D = D; k.result = r;
    uint32_t sel = 0, selMask = 0, x70 = 0, dn = 0, xD0 = 0, xD4 = 0; uint8_t e8 = 0, a8b = 0, x15E = 0; uint16_t w60 = 0; uint64_t lg = 0, dp = 0, set = 0, mt = 0;
    SafeRead(ctx + 0xD0, &sel, 4); SafeRead(D + 0xE8, &e8, 1);
    int ok = sel == 2 ? (e8 & 1) : sel == 3 ? (e8 & 2) : sel == 4 ? (e8 & 4) : 1;
    if (ok) k.pass |= 1;
    SafeRead(ctx + 0xD4, &selMask, 4);
    if (((KfMaskFn)(g_world.game + KF_F28C40))(ctx, D, selMask)) k.pass |= 2;
    SafeRead(ctx + 0x18, &lg, 8); SafeRead(lg + 0xF9, &k.lvl, 1); SafeRead(D + 0x60, &w60, 2);
    { uint32_t mn = (w60 >> 2) & 0xF, mx = (w60 >> 6) & 0xF; if ((mn == 0xF || k.lvl >= mn) && (mx == 0xF || k.lvl <= mx)) k.pass |= 4; }
    uint8_t tag[0x38]; SafeRead(D + 0x70, tag, 0x38); SafeRead(D + 0x70, &x70, 4); k.tag = x70;
    if (x70)
    {
        uint64_t q = 0, M = 0, list = 0; uint32_t cnt = 0;
        if (!SafeRead(lg + 0x2D0, &q, 8) && !SafeRead(q + 0x68, &M, 8) && !SafeRead(M + 0x170, &list, 8) && !SafeRead(M + 0x178, &cnt, 4))
            for (uint32_t i = 0; i < cnt && i < 4096; i++) { uint32_t id = 0; if (!SafeRead(list + 0x38ull * i, &id, 4) && id == x70) { SafeRead(list + 0x38ull * i, tag, 0x38); break; } }
    }
    { uint64_t p1, p2, p3; uint32_t c1, c2, c3; memcpy(&p1, tag + 8, 8); memcpy(&c1, tag + 0x10, 4); memcpy(&p2, tag + 0x18, 8); memcpy(&c2, tag + 0x20, 4);
      memcpy(&p3, tag + 0x28, 8); memcpy(&c3, tag + 0x30, 4); SafeRead(ctx + 8, &set, 8); SafeRead(set, &k.mask, 8);
      k.all = c1 ? KfBits(p1, c1) : 0; k.any = c2 ? KfBits(p2, c2) : 0; k.none = c3 ? KfBits(p3, c3) : 0;
      if ((!c1 || (k.mask & k.all) == k.all) && (!c2 || (k.mask & k.any)) && (!c3 || !(k.mask & k.none))) k.pass |= 8; }
    SafeRead(D + 0xB0, &dp, 8); SafeRead(D + 0xB8, &dn, 4); if (KfByteLists(dp, dn, a3, k.dB0, k.aB0, k.nB0)) k.pass |= 0x10;
    SafeRead(D + 0xC0, &dp, 8); SafeRead(D + 0xC8, &dn, 4); if (KfByteLists(dp, dn, a4, k.dC0, k.aC0, k.nC0)) k.pass |= 0x20;
    SafeRead(D + 0xD0, &xD0, 4); if (xD0 == 0 || KfU32In(a5, xD0, 0)) k.pass |= 0x40;
    SafeRead(D + 0xD4, &xD4, 4); if (xD4 == 0 || KfU32In(a6, xD4, 1)) k.pass |= 0x80;
    SafeRead(D + 0x15E, &x15E, 1); if ((uint8_t)a9 == 0 || x15E == (uint8_t)a9) k.pass |= 0x100;
    SafeRead(D + 0xA8, &a8b, 1);
    if (a8b == 0) k.pass |= 0x200;
    else { uint32_t an = 0; uint64_t ap = 0; if (a7 && !SafeRead(a7, &an, 4) && !SafeRead(a7 + 8, &ap, 8)) for (uint32_t i = 0; i < an && i < 4096; i++) { uint8_t b = 0; if (!SafeRead(ap + i, &b, 1) && b == a8b) { k.pass |= 0x200; break; } } }
    { uint64_t kp = 0; uint32_t kn = 0; SafeRead(D + 0x198, &kp, 8); SafeRead(D + 0x1A0, &kn, 4); SafeRead(set + 0xC0, &mt, 8);
      k.nk = (uint8_t)(kn > 8 ? 8 : kn); if (kp) SafeRead(kp, k.kinds, k.nk);
      for (uint32_t i = 0; i < k.nk; i++)
      {
          float w = 0; uint8_t kk = k.kinds[i];
          if (a10) SafeRead(a10 + 4ull * kk - 4, &w, 4); else SafeRead(mt + 0xEC + 4ull * kk, &w, 4);
          k.w[i] = w; if (w > 0) k.pass |= 0x400;
      } }
    if (((k.pass & 0x7FF) == 0x7FF) != (r != 0)) InterlockedIncrement(&g_kfMismatch);
    n = InterlockedIncrement(&g_kfCount) - 1;
    if (n < 512) g_kf[n] = k;
    return r;
}
static void KindFilterResearchInstall(uintptr_t game)
{
    if (g_kindFilter != NULL) return;
    if (ApiCodeMatches(game + KF_FN, "40 55 53 56 57 41 55 48 8B EC 48 81 EC 80 00 00 00") != 1 || ApiCodeMatches(game + KF_F28C40, "48 8B 49 18 33 C0 45 8B D8 4C 8B 89 D0 02 00 00") != 1 ||
        ApiCodeMatches(game + KF_RET16 - 5, "E8") != 1)
    { Note("difficulty: objectives research: KindFilter is not the reviewed one; its tests are not noted"); return; }
    LoaderHook("extra large maps' objective definition test (research)", game + KF_FN, (void*)KindFilterResearch, (void**)&g_kindFilter);
}
static void KindFilterNote(uint64_t D)
{
    for (LONG i = 0; i < g_kfCount && i < 512; i++)
    {
        const KfRecord* k = &g_kf[i]; if (k->D != D) continue;
        static const char* names[11] = {"faction", "0xF28C40", "level", "tag", "B0", "C0", "D0", "D4", "15E", "A8", "kinds"};
        char failed[128] = ""; size_t at = 0;
        for (int b = 0; b < 11; b++) if (!(k->pass & (1u << b))) at += (size_t)snprintf(failed + at, sizeof failed - at, "%s%s", at ? " " : "", names[b]);
        Note("difficulty: objectives research:     KindFilter %s (failed: %s); level %u; tag %u mask %llx all %llx any %llx none %llx; B0 %u:%02x%02x%02x%02x vs %u:%02x%02x%02x%02x; C0 %u:%02x%02x%02x%02x vs %u:%02x%02x%02x%02x; kinds %u: %u=%.3g %u=%.3g %u=%.3g",
            k->result ? "passes" : "refuses", at ? failed : "none", k->lvl, k->tag, (unsigned long long)k->mask, (unsigned long long)k->all, (unsigned long long)k->any, (unsigned long long)k->none,
            k->nB0[0], k->dB0[0], k->dB0[1], k->dB0[2], k->dB0[3], k->nB0[1], k->aB0[0], k->aB0[1], k->aB0[2], k->aB0[3],
            k->nC0[0], k->dC0[0], k->dC0[1], k->dC0[2], k->dC0[3], k->nC0[1], k->aC0[0], k->aC0[1], k->aC0[2], k->aC0[3],
            k->nk, k->kinds[0], (double)k->w[0], k->kinds[1], (double)k->w[1], k->kinds[2], (double)k->w[2]);
        return;
    }
    Note("difficulty: objectives research:     KindFilter not reached at op 0x16's test for this definition");
}

static uint64_t g_pass15;
static void WorldObjectiveUnplacedNote(uintptr_t block)
{
    if (g_unplacedCount == 0 || g_pass15 == 0) return;
    uint64_t pass = g_pass15, holders = 0, holderCountAt = 0, set = 0, entries = 0;
    uint32_t holderCount = 0, objectives = 0, important = 0; uint8_t lvl = 0;
    if (SafeRead(pass, &holders, 8) || SafeRead(pass + 8, &holderCountAt, 8) || holderCountAt == 0 || SafeRead(holderCountAt, &holderCount, 4) ||
        SafeRead(pass + 0x18, &entries, 8) || SafeRead(pass + 0x20, &set, 8) || set == 0 || SafeRead(set + 0xD4, &objectives, 4))
    { Note("difficulty: objectives research: op 0x15's pass could not be read"); return; }
    SafeRead(g_worldLevel + 0xF9, &lvl, 1); SafeRead(block + 0x11A715Cu, &important, 4);
    if (objectives > 48) objectives = 48;
    if (important > 1024) important = 1024;
    for (uint32_t u = 0; u < g_unplacedCount; u++)
        for (uint32_t o = 0; o < objectives; o++)
        {
            uint32_t id = 0, first = 0, count = 0;
            if (SafeRead(set + 4 + 4ull * o, &id, 4) || id != g_unplacedIds[u]) continue;
            SafeRead(set - 0xF0 + 4ull * o, &first, 4); SafeRead(set + 0x100 + 4ull * o, &count, 4);
            Note("difficulty: objectives research: %08x (list place %u): op 0x15 gave %u entries from %u; level %u", id, o, count, first, lvl);
            for (uint32_t j = 0; j < count && j < 12; j++)
            {
                uint16_t ix[3] = {0}; uint64_t holder = 0, cats = 0, defs = 0;
                if (entries == 0 || SafeRead(entries + 16ull * (first + j), ix, 6) || ix[0] >= holderCount || SafeRead(holders + 8ull * ix[0], &holder, 8) || holder == 0 ||
                    SafeRead(holder + 0x28, &cats, 8) || cats == 0 || SafeRead(cats + 0x18ull * ix[1] + 8, &defs, 8) || defs == 0)
                { Note("difficulty: objectives research:   entry %u (%u/%u/%u) does not read", j, ix[0], ix[1], ix[2]); continue; }
                uint64_t D = defs + 0x1C8ull * ix[2];
                uint32_t key = 0, x160 = 0, x70 = 0, xD0 = 0, xD4 = 0; uint16_t x60 = 0; uint8_t cap = 0, e8 = 0, e9 = 0; float w = 0;
                uint64_t xA8 = 0, xB0 = 0, xC0 = 0;
                SafeRead(D + 8, &key, 4); SafeRead(D + 0x160, &x160, 4); SafeRead(D + 0x15C, &cap, 1); SafeRead(D + 0x60, &x60, 2);
                SafeRead(D + 0xE8, &e8, 1); SafeRead(D + 0xE9, &e9, 1); SafeRead(D + 0x70, &x70, 4); SafeRead(D + 0xA8, &xA8, 8);
                SafeRead(D + 0xB0, &xB0, 8); SafeRead(D + 0xC0, &xC0, 8); SafeRead(D + 0xD0, &xD0, 4); SafeRead(D + 0xD4, &xD4, 4); SafeRead(D + 0x68, &w, 4);
                uint32_t same = 0;
                for (uint32_t l = 0; l < important; l++)
                {
                    uint64_t h = 0; uint8_t a1 = 0, a2 = 0;
                    if (SafeRead(block + g_importantList + 0x108ull * l, &h, 8) || SafeRead(block + g_importantList + 0x108ull * l + 0xA1, &a1, 1) ||
                        SafeRead(block + g_importantList + 0x108ull * l + 0xA2, &a2, 1)) continue;
                    if (h == holder && a1 == ix[1] && a2 == ix[2]) same++;
                }
                Note("difficulty: objectives research:   entry %u (%u/%u/%u) key %08x: 0x160 bit3 %u, capacity %u (same records %u), level min %u max %u, E8 %02x E9 %02x, +0x70 %08x, +0xA8 %llx +0xB0 %llx +0xC0 %llx +0xD0 %u +0xD4 %u, kinds %08x, weight %.3g",
                    j, ix[0], ix[1], ix[2], key, (x160 >> 3) & 1, cap, same, (x60 >> 2) & 0xF, (x60 >> 6) & 0xF, e8, e9, x70, (unsigned long long)xA8,
                    (unsigned long long)xB0, (unsigned long long)xC0, xD0, xD4, NatureKindMask(D), (double)w);
                KindFilterNote(D);
            }
        }
}

static void WorldNatureResearch15(uint64_t pass, uint64_t context)
{
    static LONG level = -1;
    if (g_cities.level != g_world.levels || g_cities.count == 0 || level == g_world.levels) return;
    level = g_world.levels;
    uint64_t holders = 0, holderCountAt = 0, set = 0, entries = 0, totalAt = 0;
    uint32_t holderCount = 0, total = 0, select = 0, selectArg = 0, objectives = 0;
    if (SafeRead(pass, &holders, 8) || SafeRead(pass + 8, &holderCountAt, 8) || holderCountAt == 0 || SafeRead(holderCountAt, &holderCount, 4) ||
        SafeRead(pass + 0x18, &entries, 8) || SafeRead(pass + 0x20, &set, 8) || SafeRead(pass + 0x28, &totalAt, 8) || totalAt == 0 || SafeRead(totalAt, &total, 4) ||
        set == 0 || SafeRead(set + 0xD4, &objectives, 4))
    { Note("difficulty: nature research: op 0x15's pass could not be read"); return; }
    SafeRead(context + 0xD0, &select, 4); SafeRead(context + 0xD4, &selectArg, 4);
    if (objectives > 48) objectives = 48;
    /* R1: per objective, its entries' definitions by kind list */
    for (uint32_t o = 0; o < objectives; o++)
    {
        uint32_t id = 0, first = 0, count = 0, one = 0, three = 0, both = 0, neither = 0, e8[8] = {0}, b160 = 0; float wmin = 1e30f, wmax = -1e30f;
        SafeRead(set + 4 + 4ull * o, &id, 4); SafeRead(set - 0xF0 + 4ull * o, &first, 4); SafeRead(set + 0x100 + 4ull * o, &count, 4); /* layout L48 (difficulty_block.c) */
        for (uint32_t j = 0; j < count && j < 4096; j++)
        {
            uint16_t ix[3] = {0}; uint64_t holder = 0, cats = 0, defs = 0;
            if (entries == 0 || SafeRead(entries + 16ull * (first + j), ix, 6) || ix[0] >= holderCount || SafeRead(holders + 8ull * ix[0], &holder, 8) || holder == 0 ||
                SafeRead(holder + 0x28, &cats, 8) || cats == 0 || SafeRead(cats + 0x18ull * ix[1] + 8, &defs, 8) || defs == 0) continue;
            uint64_t D = defs + 0x1C8ull * ix[2];
            uint32_t m = NatureKindMask(D); uint8_t f = 0; uint32_t x160 = 0; float w = 0;
            SafeRead(D + 0xE8, &f, 1); SafeRead(D + 0x160, &x160, 4); SafeRead(D + 0x68, &w, 4);
            int k1 = (m >> 1) & 1, k3 = (m >> 3) & 1;
            both += k1 && k3; one += k1 && !k3; three += k3 && !k1; neither += !k1 && !k3;
            for (int b = 0; b < 8; b++) e8[b] += (f >> b) & 1;
            b160 += (x160 >> 3) & 1;
            if (w < wmin) wmin = w; if (w > wmax) wmax = w;
        }
        Note("difficulty: nature research R1 obj %u id %u: first %u, %u defs: kind1-only %u, kind3-only %u, both %u, neither %u; E8 bits %u/%u/%u; 0x160 bit3 %u; weight %.3g..%.3g",
            o, id, first, count, one, three, both, neither, e8[0], e8[1], e8[2], b160, count ? (double)wmin : 0.0, count ? (double)wmax : 0.0);
    }
    Note("difficulty: nature research R1: ctx+0xD0 %u (+0xD4 %u), %u objectives, %u entries (one note per objective above)", select, selectArg, objectives, total);
    /* R2: every holder and category: definitions by kind list, with POI type lists, and the selected E8 bit */
    uint32_t bit = select == 2 ? 0 : select == 3 ? 1 : select == 4 ? 2 : 8, notes = 0, k1All = 0, k3All = 0, bothAll = 0, selAll = 0;
    for (uint32_t h = 0; h < holderCount && h < 512; h++)
    {
        uint64_t holder = 0, cats = 0; uint32_t catCount = 0, key = 0;
        if (SafeRead(holders + 8ull * h, &holder, 8) || holder == 0 || SafeRead(holder + 0x28, &cats, 8) || SafeRead(holder + 0x30, &catCount, 4) || cats == 0) continue;
        SafeRead(holder + 8, &key, 4);
        for (uint32_t c = 0; c < catCount && c < 64; c++)
        {
            uint64_t defs = 0; uint32_t defCount = 0, catId = 0, one = 0, three = 0, both = 0, poi = 0, sel = 0;
            if (SafeRead(cats + 0x18ull * c, &catId, 4) || SafeRead(cats + 0x18ull * c + 8, &defs, 8) || SafeRead(cats + 0x18ull * c + 0x10, &defCount, 4) || defs == 0) continue;
            for (uint32_t d = 0; d < defCount && d < 1024; d++)
            {
                uint64_t D = defs + 0x1C8ull * d; uint32_t m = NatureKindMask(D), poiCount = 0; uint8_t f = 0;
                SafeRead(D + 0x178, &poiCount, 4); SafeRead(D + 0xE8, &f, 1);
                int k1 = (m >> 1) & 1, k3 = (m >> 3) & 1;
                both += k1 && k3; one += k1 && !k3; three += k3 && !k1; poi += poiCount > 0 && poiCount < 64; sel += bit < 8 && ((f >> bit) & 1);
            }
            k1All += one; k3All += three; bothAll += both; selAll += sel;
            if (one + both == 0 && three == 0) continue; /* nothing kind 1 or 3: skip */
            if (++notes <= 80)
                Note("difficulty: nature research R2 h%u (+8 %08X) c%u %08X: %u defs, kind1-only %u, kind3-only %u, both %u, POI lists %u, E8 sel %u", h, key, c, catId, defCount, one, three, both, poi, sel);
        }
    }
    Note("difficulty: nature research R2: %u holders, %u categories with kind 1 or 3 (notes above, at most 80): kind1-only %u, kind3-only %u, both %u, E8 sel %u",
        holderCount, notes, k1All, k3All, bothAll, selAll);
}

static void WorldNatureResearch16(uint64_t context)
{
    static LONG level = -1;
    if (g_cities.level != g_world.levels || g_cities.count == 0 || level == g_world.levels) return;
    level = g_world.levels;
    uint64_t a = 0, b = 0, M = 0; uint32_t q[5][4] = {{0}};
    if (SafeRead(context + 0x18, &a, 8) || a == 0 || SafeRead(a + 0x2D0, &b, 8) || b == 0 || SafeRead(b + 0x68, &M, 8) || M == 0 || SafeRead(M + 0xA0, q, sizeof q))
    { Note("difficulty: nature research R3: the quotas could not be read"); return; }
    char text[600]; int n = 0;
    for (int k = 0; k < 5; k++)
    {
        float f[4]; memcpy(f, q[k], sizeof f);
        n += snprintf(text + n, sizeof text - n, " {kind %d: %08X %08X %08X %08X = %.4g %.4g %.4g %.4g}", k + 1, q[k][0], q[k][1], q[k][2], q[k][3], (double)f[0], (double)f[1], (double)f[2], (double)f[3]);
    }
    Note("difficulty: nature research R3: M %p (ctx+0x18 -> +0x2D0 -> +0x68), quotas M+0xA0 + 16 (k - 1):%s", (void*)(uintptr_t)M, text);
}

/* R4/R6: the placement-related ops in the order they finish on a city level, with the record and the thread */
static void WorldNatureOps(uint32_t op, uint32_t record)
{
    static LONG level = -1, count;
    static const uint8_t kOps[] = {0x13, 0x15, 0x16, 0x17, 0x18, 0x1F, 0x28, 0x78, 0x7C, 0x81, 0x82, 0x84, 0x8B, 0xA0, 0xA5};
    if (g_cities.level != g_world.levels || g_cities.count == 0) return;
    int listed = 0;
    for (size_t i = 0; i < sizeof kOps; i++) listed |= kOps[i] == op;
    if (!listed) return;
    if (level != g_world.levels) { level = g_world.levels; count = 0; }
    if (InterlockedIncrement(&count) > 80) return;
    Note("difficulty: nature research R4: op 0x%X record %u done on thread %lu", op, record, (unsigned long)GetCurrentThreadId());
}

/* ---- Nature variants in the open land (Design A; KB xl-operation-content "Nature variants", static 2026-11-04) ---------
 * A city level's kind weights W (S+0xC0 -> its 0x11C map-type entry, W = +0xF0.., W[k-1] for kind k; S = [ctx+8] =
 * lg+0x2D20, set at level init by 0xF71CD8 / 0xFBF6BF; the entry lives in shared resource data, never written) are
 * 0 0 1 0 0: every objective, outpost and POI takes a city (kind 3) definition, and op 0x7C (AttachLocationsToRouteCells
 * 0xF81020) repaints the route cell under a location whose zone type's kinds (type+0x210, u8[4]) lack the location's
 * kind (0xF815B0..0xF815B7): s75a, 41 of 73 important locations in the open land, 44 open-land cells painted city
 * (67 after op 0x28). The open land's zones (18/19/20, types 3/4/5) list kind 1, the cities' kind 3. Every kind read
 * in placement goes through [S+0xC0]: KindRoll 0xF2CA20 (5 callers: ops 0x06, 0x16, 0x8E, the sub-location maker
 * 0xF8E960 and 0xF479D0) and KindFilter 0xF292B0 (16 callers: 0x13, 0x16, 0x06, 0x17, 0x82, 0x88, 0xA0, 0x81/0x84
 * through 0xF29740, GenerateLocations 0xF767A0, 0xF8EC70), and op 0x18 sizes its clusters per kind from W (0xF798B5).
 * So, on a city level of an extra large map:
 * (a) from op 0x15 to the end of the placement ops (s75a: 0x15 0x16 0x17 0x82 0x18 0x8B 0x78 0x7C 0xA0 0x82 0x8B 0xA0,
 *     then 0x28) S+0xC0 points at a copy of the entry with W[0] = W[2]; op 0x28 or another op of NatureCloseOp puts the
 *     original back before op 0x13 (record ~60) and the mission, which reads S too.
 * (b) The sub-locations an objective makes (0xF8EC70, only caller 0xF90443 in op 0x16; parent [rcx+0x18], its kind
 *     +0xF0 set by 0xF9006D before) roll and filter with only the parent's kind weighted, so a nature objective gets
 *     nature sub-sites.
 * (c) After op 0x18 (and the ring banding), kind-3 clusters move inside the three cities and kind-1 clusters out of
 *     them, so SpreadLocations (nearest free point to the cluster's centre) puts each kind on its own land. */
#define NATURE_SLOTS 4
#define NATURE_ENTRY 0x11Cu
static struct { uint64_t S, original; DWORD thread; LONG level; int open; uint32_t setW; uint8_t copy[NATURE_ENTRY + 4]; } g_nature[NATURE_SLOTS];
static SRWLOCK g_natureLock = SRWLOCK_INIT;
static volatile LONG g_natureSub, g_natureSubOne;
static volatile LONG* g_attachSkipped; /* op 0x7C locations with no zone of their kind (InstallAttachGuard) */
/* Every W reader in generation code (indexed reads of +0xEC/+0xF0 off [S+0xC0], 0xF00000..0x1000000): KindFilter, 0xF29740
 * (ops 0x81 / 0x84), ops 0x13 (0xF636CA), 0x16, 0x17, 0x18, 0x82, 0x88, 0x8E (0xF86412), 0xA0 and GenerateLocations
 * 0xF767A0. The ops between the placement ops do not read W, so the window closes only where the placement ends: op 0x13
 * (MergeSubRegionCells, record ~60, must see the original) or the first of the later ops below. */
static int NatureCloseOp(uint32_t op)
{
    static const uint8_t kOps[] = {0x13, 0x1F, 0x28, 0x2B, 0x6A, 0xA5, 0xA7, 0xAB};
    for (size_t i = 0; i < sizeof kOps; i++) if (kOps[i] == op) return 1;
    return 0;
}
static int NatureSlotOf(uint64_t S)
{
    for (int i = 0; i < NATURE_SLOTS; i++) if (g_nature[i].S == S && S != 0) return i;
    return -1;
}
static void NatureGrid(uintptr_t gen, uint32_t worldSize); /* below */
/* called before every dispatcher step of an extra large level: opens the window at op 0x15, closes it at any op outside */
static void NatureWindow(uint64_t context, uint32_t op, uint32_t record)
{
    uint64_t S = 0;
    if (SafeRead(context + 8, &S, 8) || S == 0) return;
    AcquireSRWLockExclusive(&g_natureLock);
    int s = NatureSlotOf(S);
    if (s >= 0 && g_nature[s].open && NatureCloseOp(op))
    {
        uint64_t now = 0;
        if (!SafeRead(S + 0xC0, &now, 8) && now == (uint64_t)(uintptr_t)g_nature[s].copy) *(volatile uint64_t*)(uintptr_t)(S + 0xC0) = g_nature[s].original;
        g_nature[s].open = 0;
        { uintptr_t gen = 0; uint64_t lg = 0; uint32_t w = 0; if (!SafeRead(context + 0x18, &lg, 8) && lg != 0 && !SafeRead(lg + 0x2C8, &gen, 8) && !SafeRead(S + 0x2C, &w, 4)) NatureGrid(gen, w); }
        Note("difficulty: extra large maps: nature variants: the open land's kind weights end before op 0x%X (record %u); %ld sub-location groups kept to their parent's kind (%ld nature); "
            "%ld locations left on their own zone by op 0x7C (none of their kind to paint)", op, record, (long)g_natureSub, (long)g_natureSubOne, g_attachSkipped ? (long)*g_attachSkipped : -1L);
    }
    else if (op == 0x15 && g_cities.level == g_world.levels && g_cities.count > 0 && (s < 0 || !g_nature[s].open))
    {
        uint64_t mt = 0; float W[5] = {0};
        if (!SafeRead(S + 0xC0, &mt, 8) && mt != 0 && !SafeRead(mt + 0xF0, W, sizeof W) && W[0] == 0.0f && W[2] > 0.0f)
        {
            if (s < 0) for (int i = 0; i < NATURE_SLOTS && s < 0; i++) if (!g_nature[i].open) s = i;
            if (s >= 0 && !SafeRead(mt, g_nature[s].copy, NATURE_ENTRY))
            {
                float w0 = W[2];
                memcpy(g_nature[s].copy + 0xF0, &w0, 4);
                g_nature[s].S = S; g_nature[s].original = mt; g_nature[s].thread = GetCurrentThreadId(); g_nature[s].level = g_world.levels; g_nature[s].open = 1;
                g_natureSub = 0; g_natureSubOne = 0;
                *(volatile uint64_t*)(uintptr_t)(S + 0xC0) = (uint64_t)(uintptr_t)g_nature[s].copy;
                Note("difficulty: extra large maps: nature variants: the open land's kind weights from op 0x15 (record %u): W %.3g %.3g %.3g %.3g %.3g -> nature %.3g (S %p)",
                    record, (double)W[0], (double)W[1], (double)W[2], (double)W[3], (double)W[4], (double)w0, (void*)(uintptr_t)S);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_natureLock);
}
/* The spread of a level's important locations (objectives, outposts, POIs, satellites: G+g_importantList, 0x108 each, count
 * G+0x11A715C, position +0xA4 in corner metres) over 2 km cells of the world: a row per 2 km of y, north last. */
static void NatureGrid(uintptr_t gen, uint32_t worldSize)
{
    uint32_t count = 0, grid[8][8] = {{0}}, kinds[6] = {0}, outside = 0;
    if (gen == 0 || SafeRead(gen + 0x11A715Cu, &count, 4)) return;
    if (count > 1024) count = 1024;
    const float cell = (float)worldSize / 8.0f;
    for (uint32_t i = 0; i < count; i++)
    {
        float p[2]; uint8_t kind = 0;
        uint64_t at = gen + g_importantList + 0x108ull * i;
        if (SafeRead(at + 0xA4, p, 8) || SafeRead(at + 0xF0, &kind, 1) || !(cell > 0.0f)) continue;
        int gx = (int)(p[0] / cell), gy = (int)(p[1] / cell);
        if (gx < 0 || gx > 7 || gy < 0 || gy > 7) { outside++; continue; }
        grid[gy][gx]++; kinds[kind < 6 ? kind : 0]++;
    }
    char text[400]; int n = 0, empty = 0;
    for (int y = 0; y < 8; y++)
    {
        n += snprintf(text + n, sizeof text - n, " |");
        for (int x = 0; x < 8; x++) { n += snprintf(text + n, sizeof text - n, " %u", grid[y][x]); empty += grid[y][x] == 0; }
    }
    Note("difficulty: extra large maps: content spread: %u important locations (kind 1: %u, 3: %u, other %u) over 2 km cells, rows south to north:%s; %d of 64 cells empty, %u outside the world",
        count, kinds[1], kinds[3], kinds[0] + kinds[2] + kinds[4] + kinds[5], text, empty, outside);
}
/* (b) */
typedef uint64_t (*NatureSubFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static NatureSubFn g_natureSubFn;
static uint64_t NatureSubLocations(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    int s = -1;
    DWORD me = GetCurrentThreadId();
    for (int i = 0; i < NATURE_SLOTS && s < 0; i++) if (g_nature[i].open && g_nature[i].thread == me) s = i;
    uint64_t parent = 0; uint8_t kind = 0;
    if (s < 0 || SafeRead(a + 0x18, &parent, 8) || parent == 0 || SafeRead(parent + 0xF0, &kind, 1) || kind < 1 || kind > 5)
        return g_natureSubFn(a, b, c, d, e, f, g, h);
    float W[5], keep[5];
    memcpy(keep, g_nature[s].copy + 0xF0, sizeof keep);
    for (int k = 0; k < 5; k++) W[k] = k == kind - 1 ? (keep[k] > 0.0f ? keep[k] : 1.0f) : 0.0f;
    memcpy(g_nature[s].copy + 0xF0, W, sizeof W);
    uint64_t r = g_natureSubFn(a, b, c, d, e, f, g, h);
    memcpy(g_nature[s].copy + 0xF0, keep, sizeof keep);
    InterlockedIncrement(&g_natureSub);
    if (kind == 1) InterlockedIncrement(&g_natureSubOne);
    return r;
}
/* (c) the clusters of a city level's op 0x18, after the ring banding: kind 3 into the cities (each city in turn, on a
 * ring at 0.45 of its radius), kind 1 out to the open land (at least the radius + the fringe + 160 m from every city
 * centre, pushed straight out from the nearest one). Inputs are the game's clusters and g_cities (seed-derived). */
static void NatureClusters(uint64_t pass, uint64_t clusters, uint32_t n)
{
    uint64_t S = 0;
    if (SafeRead(pass + 8, &S, 8) || S == 0) return;
    int s = NatureSlotOf(S);
    if (s < 0 || !g_nature[s].open || g_cities.count == 0) return;
    uint32_t moved3 = 0, moved1 = 0, kinds[6] = {0}, perCity[CITY_COUNT] = {0};
    const float out = g_cities.radius + CITY_FRINGE + 160.0f;
    /* kind 3: each city's clusters spread over its disc (golden angle, radius sqrt(0.1 + 0.6 (j + 0.5) / m) of the
     * city's, m the city's share), not one ring at 0.45: on one ring a city's content sat in a ~300 m patch beside its
     * centre (audit 13, s75t: centroids 280-360 m off) */
    uint32_t total3 = 0;
    for (uint32_t i = 0; i < n && i < 64; i++)
    {
        uint8_t kind = 0;
        if (!SafeRead(clusters + 0xE0ull * i + 0xD8, &kind, 1) && kind == 3) total3++;
    }
    const uint32_t share = (total3 + (uint32_t)g_cities.count - 1) / (uint32_t)g_cities.count;
    for (uint32_t i = 0; i < n && i < 64; i++)
    {
        uint64_t at = clusters + 0xE0ull * i;
        float xy[2]; uint8_t kind = 0;
        if (SafeRead(at, xy, 8) || SafeRead(at + 0xD8, &kind, 1) || !isfinite(xy[0]) || !isfinite(xy[1])) continue;
        kinds[kind < 6 ? kind : 0]++;
        int closest = 0; float best = 1e30f;
        for (int k = 0; k < g_cities.count; k++)
        {
            float dx = xy[0] - g_cities.x[k], dy = xy[1] - g_cities.y[k], d = dx * dx + dy * dy;
            if (d < best) { best = d; closest = k; }
        }
        if (kind == 3)
        {
            int k = (int)(moved3 % (uint32_t)g_cities.count);
            uint32_t j = perCity[k]++;
            float a = 2.3999632f * (float)j + 0.7f * (float)k; /* golden angle per city */
            float r = g_cities.radius * sqrtf(0.1f + 0.6f * ((float)j + 0.5f) / (float)(share ? share : 1));
            xy[0] = g_cities.x[k] + r * cosf(a); xy[1] = g_cities.y[k] + r * sinf(a);
            memcpy((void*)(uintptr_t)at, xy, 8);
            moved3++;
        }
        else if (kind == 1 && best < out * out)
        {
            float dx = xy[0] - g_cities.x[closest], dy = xy[1] - g_cities.y[closest], d = sqrtf(best);
            if (d < 1.0f) { dx = 1.0f; dy = 0.0f; d = 1.0f; }
            xy[0] = g_cities.x[closest] + dx / d * out; xy[1] = g_cities.y[closest] + dy / d * out;
            memcpy((void*)(uintptr_t)at, xy, 8);
            moved1++;
        }
    }
    Note("difficulty: extra large maps: nature variants: %u clusters by kind (1: %u, 2: %u, 3: %u, 4: %u, 5: %u); %u city clusters into the cities (%u / %u / %u), %u nature clusters moved out of them",
        n, kinds[1], kinds[2], kinds[3], kinds[4], kinds[5], moved3, perCity[0], perCity[1], perCity[2], moved1);
}

static void WorldCityBudget(uintptr_t gen, uint32_t op)
{
    if (g_cities.level != g_world.levels) return;
    uint32_t count = 0, inCity[CITY_COUNT] = {0}, free = 0;
    SafeRead(gen + 0x11A7108u, &count, 4);
    for (uint32_t i = 0; i < count && i < 65536; i++)
    {
        float p[2];
        if (SafeRead(gen + 0x11A7280u + 0xE0ull * i + 0xB0, p, 8)) break;
        for (int k = 0; k < g_cities.count; k++)
        {
            float dx = p[0] - g_cities.x[k], dy = p[1] - g_cities.y[k], r = 1.5f * g_cities.radius;
            if (dx * dx + dy * dy <= r * r) { inCity[k]++; break; }
        }
    }
    uint64_t holder = 0, base = 0; uint32_t head = 0xFFFFFFFFu; uint16_t stride = 0;
    if (!SafeRead(g_worldLevel + 0x2C0, &holder, 8) && holder != 0 && !SafeRead(holder + 8 + 0x2C, &head, 4) &&
        !SafeRead(holder + 8 + 0x24, &stride, 2) && !SafeRead(holder + 8 + 0x30, &base, 8) && base != 0 && stride != 0)
        for (uint32_t at = head; at != 0xFFFFFFFFu && free < 140000; free++)
            if (SafeRead(base + (uint64_t)stride * at, &at, 4)) break;
    Note("difficulty: extra large maps: locations after op 0x%X: %u (cities %u / %u / %u), stamp index %u free slots%s", op, count, inCity[0], inCity[1], inCity[2], free,
        head == 0xFFFFFFFFu ? " (full)" : "");
}
static int g_cityShifted;
static uint32_t g_cityShiftCells;
static float g_cityShiftSaved[1024][2];
static void WorldCityShift(uintptr_t gen, int back)
{
    if (back)
    {
        /* the sites as they were, bit for bit */
        for (uint32_t i = 0; g_cityShifted && i < g_cityShiftCells; i++)
            memcpy((void*)(uintptr_t)(gen + 0x8EB328u + 0x110ull * i), g_cityShiftSaved[i], 8);
        g_cityShifted = 0;
        return;
    }
    uint32_t cells = 0, w = 0;
    g_cityShifted = 0;
    if (g_cities.level != g_world.levels || SafeRead(gen + 0x11A71C0u, &cells, 4) || cells > 1024 || SafeRead(g_worldLevel + 0x2D20 + 0x2C, &w, 4)) return;
    const float c = 0.5f * (float)w;
    uint32_t moved = 0;
    for (uint32_t i = 0; i < cells; i++)
    {
        float* p = (float*)(uintptr_t)(gen + 0x8EB328u + 0x110ull * i);
        memcpy(g_cityShiftSaved[i], p, 8);
        for (int k = 0; k < g_cities.count; k++)
        {
            float dx = p[0] - g_cities.x[k], dy = p[1] - g_cities.y[k];
            if (dx * dx + dy * dy <= g_cities.radius * g_cities.radius * 1.2f) { p[0] = c + dx; p[1] = c + dy; moved++; break; }
        }
    }
    g_cityShiftCells = cells;
    g_cityShifted = 1;
    static LONG noted = -1;
    if (noted != g_world.levels) { noted = g_world.levels; Note("difficulty: extra large maps: the city zones: %u of %u route cells moved into the zone template's square for op 0xB9", moved, cells); }
}

/* The open land's zones: the level's zone list (Z+0x238, 0x44 each to an id of 0: +0 id, +4 type index, +0xC weight,
 * +0x10 height group or -1; Z = [S+0xB0]) gives the non-city zones (the type's +0x170 bits 0..1 clear: not city, not
 * structured; types from 0xB9's record +0x38). Each open-land cell (outside every city, +8 == -1: no special) takes
 * one by weight, from those of its height group (+4) or any group, as 0x78 would (0xF3794F..0xF37CBD; scratch
 * cities/zones.md), picked by a hash of its site and the level seed (the same on every peer). Before 0x78 (every city
 * layout) the zone id, the cell keeping its height group: 0x78 keeps a set +0 (0xF37A80) and 0x11 derives the type. After 0xB9 (template
 * layouts) the type too, as 0x11 does (0xF288E0(ctx, id) ? [r+0xC] : entry +4, 0xC1500D), for 0xA3 reads it first. */
typedef uint64_t (*ZoneTypeFn)(uint64_t, uint32_t);
/* Research (s74l, 2026-11-04): ~75 open-land cells (565 m) end with city zones in clusters of 5-8 outside the three
 * cities. After each op on a city level: the cells outside the city discs whose zone (+0) is a city zone (its type's
 * +0x170 bit 1, the type through 0xF288E0 or E.+4), how many carry the painters' bit (+0x10 bit 0: 0x28 / 0x7C), and the
 * first one's zone and place; noted when the count changes (the first 32 changes). */
static void WorldCityStray(uint64_t records, uint32_t end, uint64_t context, uintptr_t gen, uint32_t op, uint32_t at)
{
    static LONG level = -1; static uint32_t last, notes; static uint64_t table;
    if (g_cities.level != g_world.levels || g_cities.count == 0) return;
    if (level != g_world.levels) { level = g_world.levels; last = 0; notes = 0; table = 0; }
    if (notes >= 32) return;
    if (table == 0)
        for (uint32_t i = 0; i < end && i < 512 && table == 0; i++)
        {
            uint32_t o = 0;
            if (!SafeRead(records + (uint64_t)i * 0x19B0u, &o, 4) && o == 0xB9) SafeRead(records + (uint64_t)i * 0x19B0u + 0x38, &table, 8);
        }
    uint64_t Z = 0;
    if (table == 0 || SafeRead(g_worldLevel + 0x2D20 + 0xB0, &Z, 8) || Z == 0) return;
    static int typeFn = -1;
    if (typeFn < 0) typeFn = ApiCodeMatches(g_world.game + 0xF288E0u, "4C 8B DC 49 89 5B 10 49 89 73 18 57 48 81 EC A0 00 00 00");
    uint32_t cityIds[16], nIds = 0;
    for (uint32_t k = 0; k < 16; k++)
    {
        uint32_t e[2] = {0, 0}; uint64_t type = 0; uint32_t flags = 0;
        if (SafeRead(Z + 0x238 + 0x44ull * k, e, 8) || e[0] == 0) break;
        int32_t t = (int32_t)e[1];
        if (typeFn == 1) { uint64_t r = ((ZoneTypeFn)(g_world.game + 0xF288E0u))(context, e[0]); if (r != 0) SafeRead(r + 0xC, &t, 4); }
        if (t >= 0 && t < 256 && !SafeRead(table + 8ull * (uint32_t)t, &type, 8) && type != 0 && !SafeRead(type + 0x170, &flags, 4) && (flags & 2u)) cityIds[nIds++] = e[0];
    }
    uint32_t cells = 0, stray = 0, painted = 0, firstZone = 0; float first[2] = {0, 0};
    SafeRead(gen + 0x11A71C4u, &cells, 4);
    if (cells > 1024) cells = 1024;
    for (uint32_t i = 0; i < cells; i++)
    {
        uint8_t sub[0x20]; uint64_t route = 0; float p[2]; uint32_t zone = 0, bits = 0;
        if (SafeRead(gen + 0x92F328u + 0x20ull * i, sub, sizeof sub)) break;
        memcpy(&zone, sub, 4); memcpy(&bits, sub + 0x10, 4); memcpy(&route, sub + 0x18, 8);
        if (route == 0 || SafeRead(route, p, 8)) continue;
        int city = 0;
        for (uint32_t k = 0; k < nIds && !city; k++) city = cityIds[k] == zone;
        if (!city) continue;
        int inCity = 0;
        for (int k = 0; k < g_cities.count && !inCity; k++)
        {
            float dx = p[0] - g_cities.x[k], dy = p[1] - g_cities.y[k];
            inCity = dx * dx + dy * dy <= g_cities.radius * g_cities.radius * 1.2f;
        }
        if (inCity) continue;
        if (stray++ == 0) { firstZone = zone; first[0] = p[0]; first[1] = p[1]; }
        if (bits & 1u) painted++;
    }
    if (stray == last) return;
    notes++;
    Note("difficulty: city zones research: after op 0x%02X (record %u) %u cells outside the cities have city zones (was %u; %u with the painters' bit), the first zone %u at (%.0f, %.0f)",
        op, at, stray, last, painted, firstZone, (double)first[0], (double)first[1]);
    last = stray;
}

/* Research (s74m): 0x7C AttachLocationsToRouteCells (0xF81020) paints an important location's cell (list G+g_importantList,
 * 0x108 each, count G+0x11A715C; kind byte +0xF0, position +0xA4/+0xA8) and its route neighbours with a rolled zone
 * unless the cell's zone entry has +0x20 == 0 and its type's kind list (type+0x210, 4 bytes, 0-ended) holds the kind
 * (0xF8154E..0xF815B7). Before the level's first 0x7C on a city map: each zone's type kinds, and each important
 * location outside the cities with its kind and the zone under it (the route cell nearest its position). */
static void WorldCityKinds(uint64_t records, uint32_t end, uint64_t context, uintptr_t gen)
{
    static LONG level = -1;
    if (g_cities.level != g_world.levels || g_cities.count == 0 || level == g_world.levels) return;
    level = g_world.levels;
    uint64_t table = 0, Z = 0;
    for (uint32_t i = 0; i < end && i < 512 && table == 0; i++)
    {
        uint32_t o = 0;
        if (!SafeRead(records + (uint64_t)i * 0x19B0u, &o, 4) && o == 0xB9) SafeRead(records + (uint64_t)i * 0x19B0u + 0x38, &table, 8);
    }
    if (table == 0 || SafeRead(g_worldLevel + 0x2D20 + 0xB0, &Z, 8) || Z == 0) return;
    static int typeFn = -1;
    if (typeFn < 0) typeFn = ApiCodeMatches(g_world.game + 0xF288E0u, "4C 8B DC 49 89 5B 10 49 89 73 18 57 48 81 EC A0 00 00 00");
    char text[1400]; int n = 0;
    for (uint32_t k = 0; k < 16 && n < (int)sizeof text - 80; k++)
    {
        uint8_t e[0x44]; uint64_t type = 0; uint32_t flags = 0; uint8_t kinds[4] = {0};
        if (SafeRead(Z + 0x238 + 0x44ull * k, e, sizeof e)) break;
        uint32_t id; int32_t t; memcpy(&id, e, 4); memcpy(&t, e + 4, 4);
        if (id == 0) break;
        if (typeFn == 1) { uint64_t r = ((ZoneTypeFn)(g_world.game + 0xF288E0u))(context, id); if (r != 0) SafeRead(r + 0xC, &t, 4); }
        if (t >= 0 && t < 256 && !SafeRead(table + 8ull * (uint32_t)t, &type, 8) && type != 0) { SafeRead(type + 0x170, &flags, 4); SafeRead(type + 0x210, kinds, 4); }
        n += snprintf(text + n, sizeof text - n, " {zone %u type %d flags %X +20 %u kinds %u %u %u %u}", id, t, flags & 0xFF, e[0x20], kinds[0], kinds[1], kinds[2], kinds[3]);
    }
    Note("difficulty: city zones research: the zones' location kinds:%s", text);
    uint32_t count = 0, cells = 0;
    SafeRead(gen + 0x11A715Cu, &count, 4); SafeRead(gen + 0x11A71C4u, &cells, 4);
    if (count > 256) count = 256;
    if (cells > 1024) cells = 1024;
    n = 0;
    uint32_t outside = 0, byKind[2][6] = {{0}};
    for (uint32_t i = 0; i < count; i++)
    {
        uint8_t kind = 0; float p[2];
        uint64_t at = gen + g_importantList + 0x108ull * i;
        if (SafeRead(at + 0xF0, &kind, 1) || SafeRead(at + 0xA4, p, 8)) continue;
        int inCity = 0;
        for (int k = 0; k < g_cities.count && !inCity; k++)
        {
            float dx = p[0] - g_cities.x[k], dy = p[1] - g_cities.y[k];
            inCity = dx * dx + dy * dy <= g_cities.radius * g_cities.radius * 1.2f;
        }
        byKind[inCity][kind < 6 ? kind : 0]++;
        if (inCity) continue;
        outside++;
        uint32_t zone = 0; float best = 1e30f;
        for (uint32_t c = 0; c < cells; c++)
        {
            uint8_t sub[0x20]; uint64_t route = 0; float q[2];
            if (SafeRead(gen + 0x92F328u + 0x20ull * c, sub, sizeof sub)) break;
            memcpy(&route, sub + 0x18, 8);
            if (route == 0 || SafeRead(route, q, 8)) continue;
            float d = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]);
            if (d < best) { best = d; memcpy(&zone, sub, 4); }
        }
        if (n < (int)sizeof text - 60) n += snprintf(text + n, sizeof text - n, " {%u kind %u at %.0f,%.0f zone %u}", i, kind, (double)p[0], (double)p[1], zone);
    }
    Note("difficulty: city zones research: %u of %u important locations outside the cities:%s", outside, count, text);
    Note("difficulty: city zones research: important locations by kind: in the cities 1: %u, 3: %u, other %u; in the open land 1: %u, 3: %u, other %u",
        byKind[1][1], byKind[1][3], byKind[1][0] + byKind[1][2] + byKind[1][4] + byKind[1][5], byKind[0][1], byKind[0][3],
        byKind[0][0] + byKind[0][2] + byKind[0][4] + byKind[0][5]);
}

/* The open land's zone choice, spatially coherent: value noise (quintic, 2 octaves, ZONE_WAVE m and half that) at the
 * cell's site. Rolled independently per cell, neighbouring open cells (568 m on the 16 km plan) changed zone at about 59 %
 * of their borders, and every such border is a straight route edge with a hard change of ground (audit 12, s75o). */
#define ZONE_WAVE 3000.0f
static float ZoneNoiseLattice(int32_t x, int32_t y, uint32_t s)
{
    uint32_t h = (uint32_t)x * 0x27D4EB2Du ^ (uint32_t)y * 0x165667B1u ^ s * 0x9E3779B9u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (float)(h >> 8) / 16777216.0f;
}
static float ZoneNoise1(float x, float y, uint32_t s)
{
    float fx = floorf(x), fy = floorf(y), u = x - fx, v = y - fy;
    int32_t xi = (int32_t)fx, yi = (int32_t)fy;
    u = u * u * u * (u * (u * 6 - 15) + 10); v = v * v * v * (v * (v * 6 - 15) + 10);
    float a = ZoneNoiseLattice(xi, yi, s), b = ZoneNoiseLattice(xi + 1, yi, s), c = ZoneNoiseLattice(xi, yi + 1, s), d = ZoneNoiseLattice(xi + 1, yi + 1, s);
    return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
}
static float ZoneNoise(float gx, float gy, uint32_t seed)
{
    return (ZoneNoise1(gx / ZONE_WAVE, gy / ZONE_WAVE, seed) + 0.5f * ZoneNoise1(gx * 2.0f / ZONE_WAVE, gy * 2.0f / ZONE_WAVE, seed + 31u)) / 1.5f;
}
static int ZoneRankCompare(const void* a, const void* b)
{
    float x = ((const float*)a)[0], y = ((const float*)b)[0];
    return x < y ? -1 : x > y;
}

static void WorldCityOpenZones(uint64_t records, uint32_t end, uint64_t context, uintptr_t gen, int withType)
{
    if (g_cities.level != g_world.levels) return;
    uint64_t Z = 0, table = 0;
    for (uint32_t i = 0; i < end && i < 512 && table == 0; i++)
    {
        uint32_t op = 0;
        if (!SafeRead(records + (uint64_t)i * 0x19B0u, &op, 4) && op == 0xB9) SafeRead(records + (uint64_t)i * 0x19B0u + 0x38, &table, 8);
    }
    if (table == 0 || SafeRead(g_worldLevel + 0x2D20 + 0xB0, &Z, 8) || Z == 0) return;
    uint8_t zoned = 0;
    if (withType && (SafeRead(Z + 0x684, &zoned, 1) || !(zoned & 1))) return; /* 0xB9 zoned nothing (0xFB92A7) */
    static int typeFn = -1;
    if (typeFn < 0) typeFn = ApiCodeMatches(g_world.game + 0xF288E0u, "4C 8B DC 49 89 5B 10 49 89 73 18 57 48 81 EC A0 00 00 00");
    struct { uint32_t id; int32_t group, type; float weight; } open[16];
    uint32_t n = 0; float total = 0.0f;
    char seen[512]; int seenAt = 0;
    struct { uint32_t id; int32_t group, type; float weight; } zero[16];
    uint32_t n0 = 0;
    for (uint32_t k = 0; k < 16; k++)
    {
        uint32_t e[5] = {0}; float weight = 0.0f; uint64_t type = 0; uint32_t flags = 0;
        if (SafeRead(Z + 0x238 + 0x44ull * k, e, 20) || e[0] == 0) break;
        memcpy(&weight, &e[3], 4);
        int32_t t = (int32_t)e[1];
        if (typeFn == 1)
        {
            uint64_t r = ((ZoneTypeFn)(g_world.game + 0xF288E0u))(context, e[0]);
            if (r != 0) SafeRead(r + 0xC, &t, 4);
        }
        int typed = !(t < 0 || t >= 256 || SafeRead(table + 8ull * (uint32_t)t, &type, 8) || type == 0 || SafeRead(type + 0x170, &flags, 4));
        if (seenAt < (int)sizeof seen - 48) seenAt += snprintf(seen + seenAt, sizeof seen - seenAt, " {%u: type %d%s flags %X weight %.3g}", e[0], t, typed ? "" : " (none)", flags & 0xFF, (double)weight);
        if (!typed || (flags & 3u) != 0) continue;
        if (!(weight > 0.0f))
        {
            /* a list whose open zones all weigh 0 (the megacity layouts: 0x78 rolls only the city's): the traversable
             * ones (type bit 5) evenly, kept apart from the weighted until none is weighted */
            if (!(flags & 0x20u) || n0 >= 16) continue;
            zero[n0].id = e[0]; zero[n0].group = (int32_t)e[4]; zero[n0].type = t; zero[n0].weight = 1.0f; n0++;
            continue;
        }
        open[n].id = e[0]; open[n].group = (int32_t)e[4]; open[n].type = t; open[n].weight = weight; total += weight; n++;
    }
    int even = n == 0 && n0 > 0;
    if (even) { memcpy(open, zero, sizeof zero); n = n0; total = (float)n0; }
    uint32_t cells = 0, seed = 0, written = 0;
    SafeRead(gen + 0x11A71C4u, &cells, 4); SafeRead(g_worldLevel + 0x8, &seed, 4);
    if (n == 0 || cells > 1024)
    {
        Note("difficulty: extra large maps: the open land: no non-city zone in the level's list (%u of:%s), its cells keep the city's", n, seen);
        return;
    }
    uint32_t perZone[16] = {0}, mismatched = 0;
    /* the noise at each open cell's site, replaced by its rank among them (0..1, uniform): the weights keep their shares */
    static float rankOf[1024], order[1024][2];
    uint32_t ranked = 0;
    for (uint32_t i = 0; i < cells; i++)
    {
        uintptr_t sub = gen + 0x92F328u + 0x20ull * i;
        uint64_t route = 0; float p[2]; int32_t special = 0;
        rankOf[i] = -1.0f;
        if (SafeRead(sub + 0x18, &route, 8) || route == 0 || SafeRead(route, p, 8) || SafeRead(sub + 8, &special, 4) || special != -1) continue;
        order[ranked][0] = ZoneNoise(p[0], p[1], seed); order[ranked][1] = (float)i; ranked++;
    }
    qsort(order, ranked, sizeof order[0], ZoneRankCompare);
    for (uint32_t r = 0; r < ranked; r++) rankOf[(uint32_t)order[r][1]] = ((float)r + 0.5f) / (float)ranked;
    for (uint32_t i = 0; i < cells; i++)
    {
        uintptr_t sub = gen + 0x92F328u + 0x20ull * i;
        uint64_t route = 0; float p[2]; int32_t group = 0, special = 0;
        if (SafeRead(sub + 0x18, &route, 8) || route == 0 || SafeRead(route, p, 8) || SafeRead(sub + 4, &group, 4) || SafeRead(sub + 8, &special, 4)) continue;
        if (special != -1) continue;
        int inCity = 0;
        for (int k = 0; k < g_cities.count && !inCity; k++)
        {
            float dx = p[0] - g_cities.x[k], dy = p[1] - g_cities.y[k];
            inCity = dx * dx + dy * dy <= g_cities.radius * g_cities.radius * 1.2f;
        }
        if (inCity) continue;
        float sum = 0.0f;
        for (uint32_t k = 0; k < n; k++) if (open[k].group == -1 || open[k].group == group) sum += open[k].weight;
        int any = !(sum > 0.0f);
        if (any) sum = total;
        float roll = (rankOf[i] >= 0.0f ? rankOf[i] : 0.5f) * sum;
        if (roll >= sum) roll = sum * 0.999999f;
        uint32_t pick = n - 1;
        for (uint32_t k = 0; k < n; k++)
        {
            if (!any && open[k].group != -1 && open[k].group != group) continue;
            if (roll < open[k].weight) { pick = k; break; }
            roll -= open[k].weight;
        }
        *(volatile uint32_t*)sub = open[pick].id;
        /* The cell keeps its own height group before 0x78: 0x78 reads it (0xF378E1) and writes only +0 and +0x18
         * (0xF37CEE, 0xF37CFA), and GenerateSubregionHeights (0x04) builds the relief from it. Writing the zone's group
         * here flattened the open land (s74n: 673 of 830 cells in group 1, relief sd 1.6 m): the city maps' open zones
         * have groups 0 and 1 only. After 0xB9 (template layouts, cells rebuilt by 0xA3) the zone's group stays. */
        if (withType) *(volatile int32_t*)(sub + 4) = open[pick].group;
        else if (open[pick].group != -1 && open[pick].group != group) mismatched++;
        if (withType) *(volatile int32_t*)(sub + 0xC) = open[pick].type;
        perZone[pick]++; written++;
    }
    char text[256]; int m = 0;
    for (uint32_t k = 0; k < n && m < (int)sizeof text - 32; k++) m += snprintf(text + m, sizeof text - m, " %u (type %d, group %d): %u;", open[k].id, open[k].type, open[k].group, perZone[k]);
    Note("difficulty: extra large maps: the open land (%s%s): %u of %u cells (%u keep a height group their zone does not name), zones%s", withType ? "after 0xB9" : "before 0x78",
        even ? ", unweighted list: traversable zones evenly" : "", written, cells, mismatched, text);
}

/* RevoronoiNatureRoutes (op 0xA3, 0xF691E0; structured maps only) rebuilds the route cells and their subregions: its
 * rebuild 0xFCFA10 zeroes all 1024 subregions (0xFCFA41) and fills the new count, each with its route cell at +0x18.
 * Each main-Voronoi corner's subregion index (u16, G+0x953B30), painted from the old cells by PaintVoronoiCornerByRouteCell
 * (op 0x79, inline in the dispatcher) before 0xA3, is left as it was; the builder repaints it only at 0xC1F966. Until
 * then GenerateObjectiveLocations, SpreadLocations and LinkLocationsToRouteCells (op 0x7B, 0xC1DB3F) read old indices,
 * and one at or past the new count names a zeroed subregion: 0x7B writes through its null route cell at 0xC186AA (run
 * s73q, an XL mega city). Op 0xA3 leaves the corners and the zone grid (op 0x25's) alone, so the level's own first 0x79
 * record, run again right after 0xA3, paints the corners from the new cells, as its later 0x79 does. */
static void WorldRepaintCorners(uint64_t records, uint32_t at, uint64_t context, uint64_t g, uint64_t h, uintptr_t gen)
{
    uint32_t paint = 0xFFFFFFFFu, op = 0;
    for (uint32_t i = 0; i < at; i++)
        if (!SafeRead(records + (uint64_t)i * 0x19B0u, &op, 4) && op == 0x79) { paint = i; break; }
    static uint16_t corner[5849]; /* the array's room: G+0x953B30 .. G+0x9568E2 */
    uint32_t corners = 0, subs = 0, stale[2] = {0, 0};
    if (SafeRead(gen + 0x11A7090u, &corners, 4) || SafeRead(gen + 0x11A71C4u, &subs, 4)) return;
    if (corners > 5849) corners = 5849;
    for (int pass = 0; pass < 2; pass++)
    {
        if (pass == 1)
        {
            if (paint == 0xFFFFFFFFu) break;
            volatile uint32_t one = 0;
            uint8_t phase = 0;
            g_worldDispatch(records + (uint64_t)paint * 0x19B0u, context, (uint64_t)(uintptr_t)&one, 1, (uint64_t)(uintptr_t)&phase, 0, g, h);
        }
        if (SafeRead(gen + 0x953B30u, corner, corners * 2u)) break;
        for (uint32_t c = 0; c < corners; c++) if (corner[c] >= subs) stale[pass]++;
    }
    /* research (the structured maps' types): 0xA3's type table ([record + 8], indexed by a subregion's +0xC; 0xFD0ED0) */
    {
        uint64_t table = 0;
        static uint32_t used[256];
        memset(used, 0, sizeof used);
        for (uint32_t i = 0; i < subs && i < 1024; i++)
        {
            int32_t type = -1;
            if (!SafeRead(gen + 0x92F328u + 0x20ull * i + 0xC, &type, 4) && type >= 0 && type < 256) used[type]++;
        }
        if (!SafeRead(records + (uint64_t)at * 0x19B0u + 8, &table, 8) && table != 0)
        {
            char text[1024]; int n = 0;
            for (uint32_t t = 0; t < 256 && n < (int)sizeof text - 48; t++)
            {
                uint64_t type = 0; uint32_t flags = 0; float size = 0.0f;
                if (SafeRead(table + 8ull * t, &type, 8)) break;
                if (type == 0) { if (used[t]) n += snprintf(text + n, sizeof text - n, " %u:null/%u", t, used[t]); if (t >= 64) break; continue; }
                SafeRead(type + 0x170, &flags, 4); SafeRead(type + 0x18C, &size, 4);
                n += snprintf(text + n, sizeof text - n, " %u:%X/%.0f/%u", t, flags & 0xFF, (double)size, used[t]);
            }
            Note("difficulty: extra large maps: structured types (index:flags+0x170/size+0x18C/cells):%s", text);
        }
    }
    Note("difficulty: extra large maps: after RevoronoiNatureRoutes (record %u): %u subregions, %u of %u corners named one past them; %s",
        at, subs, stale[0], corners, paint == 0xFFFFFFFFu ? "no PaintVoronoiCornerByRouteCell record to repaint with" : "repainted");
    if (paint != 0xFFFFFFFFu && stale[1] != 0) Note("difficulty: extra large maps: %u corners still past the subregions after the repaint", stale[1]);
}

/* Research (the structured maps' zones on a larger world): after ForceStructuredZoneLayout (op 0xB9), the level's type
 * list (ids G+0x18008, count G+0x11A7080) with each type's flags (+0x170: bit 1 city, bits 0..1 structured) and cell size
 * (+0x18C) from op 0x2B's type table ([record + 0x50]), the subregions per type index (+0xC) and the zone ids (+0). */
static void WorldZoneProbe(uint64_t records, uint32_t end, uintptr_t gen)
{
    uint64_t table = 0;
    for (uint32_t i = 0; i < end && i < 512 && table == 0; i++)
    {
        uint32_t op = 0;
        if (!SafeRead(records + (uint64_t)i * 0x19B0u, &op, 4) && op == 0x2B) SafeRead(records + (uint64_t)i * 0x19B0u + 0x50, &table, 8);
    }
    uint32_t types = 0, subs = 0;
    SafeRead(gen + 0x11A7080u, &types, 4); SafeRead(gen + 0x11A71C4u, &subs, 4);
    static uint32_t perType[256], perZone[256];
    memset(perType, 0, sizeof perType); memset(perZone, 0, sizeof perZone);
    for (uint32_t i = 0; i < subs && i < 1024; i++)
    {
        int32_t t = -1; uint32_t z = 0xFFFFFFFFu;
        SafeRead(gen + 0x92F328u + 0x20ull * i, &z, 4); SafeRead(gen + 0x92F328u + 0x20ull * i + 0xC, &t, 4);
        if (t >= 0 && t < 256) perType[t]++;
        if (z < 256) perZone[z]++;
    }
    char text[1536]; int n = 0;
    for (uint32_t k = 0; k < types && k < 64 && n < (int)sizeof text - 64; k++)
    {
        uint32_t id = 0, flags = 0; uint64_t type = 0; float size = 0.0f;
        SafeRead(gen + 0x18008u + 4ull * k, &id, 4);
        if (table != 0 && !SafeRead(table + 8ull * id, &type, 8) && type != 0) { SafeRead(type + 0x170, &flags, 4); SafeRead(type + 0x18C, &size, 4); }
        n += snprintf(text + n, sizeof text - n, " [%u] id %u %X/%.0f cells %u;", k, id, flags & 0xFF, (double)size, id < 256 ? perType[id] : 0);
    }
    Note("difficulty: extra large maps: zone probe: %u types:%s", types, text);
    n = 0;
    for (uint32_t z = 0; z < 256 && n < (int)sizeof text - 24; z++) if (perZone[z]) n += snprintf(text + n, sizeof text - n, " %u:%u", z, perZone[z]);
    Note("difficulty: extra large maps: zone probe: %u subregions by zone id (+0):%s", subs, text);
    /* the level's zone list (Z+0x238, 0x44 each, to an id of 0; Z = [S+0xB0]) and each entry's type in 0xB9's type table
     * (the record's +0x38, indexed by entry +4) */
    uint64_t Z = 0, types38 = 0;
    for (uint32_t i = 0; i < end && i < 512 && types38 == 0; i++)
    {
        uint32_t op = 0;
        if (!SafeRead(records + (uint64_t)i * 0x19B0u, &op, 4) && op == 0xB9) SafeRead(records + (uint64_t)i * 0x19B0u + 0x38, &types38, 8);
    }
    if (SafeRead(g_worldLevel + 0x2D20 + 0xB0, &Z, 8) || Z == 0) return;
    uint8_t bit = 0; SafeRead(Z + 0x684, &bit, 1);
    n = 0;
    for (uint32_t k = 0; k < 16 && n < (int)sizeof text - 80; k++)
    {
        uint32_t e[5] = {0}; uint64_t type = 0; uint32_t flags = 0; float size = 0.0f;
        if (SafeRead(Z + 0x238 + 0x44ull * k, e, 20) || e[0] == 0) break;
        if (types38 != 0 && e[1] < 256 && !SafeRead(types38 + 8ull * e[1], &type, 8) && type != 0) { SafeRead(type + 0x170, &flags, 4); SafeRead(type + 0x18C, &size, 4); }
        n += snprintf(text + n, sizeof text - n, " {id %u type %u flags %X size %.0f +10 %u}", e[0], e[1], flags & 0xFF, (double)size, e[4]);
    }
    Note("difficulty: extra large maps: zone probe: Z+0x684 %u, zone list:%s", bit, text);
}

/* Memory trace (2026-11-04: an extra large level's game reached 15 GB of private memory and the system's OOM killer ended
 * run s74t during its terrain tiles): the process's private bytes and working set, and the system's available memory,
 * after every generation op that grew the commit (private bytes; Wine reports PrivateUsage 0, so PagefileUsage) by 64 MB or more since the last note, at named phases, and
 * every 30 s while a player exists (NearReportPlayer). K32GetProcessMemoryInfo through GetProcAddress (the loader does
 * not link psapi). */
/* ---- Memory attribution (research, HELLFORGE_XL_MEMPROF=1) -------------------------------------------------------------
 * Every commit goes through ntdll's NtAllocateVirtualMemory and every release or decommit through NtFreeVirtualMemory.
 * The hooks keep, per caller, the bytes committed and the bytes still committed (live), and every memory note lists the
 * callers that hold the most. The caller is the first six frames in helldivers2.exe or game.dll above the game's
 * allocator, rpmalloc 1.4 (exe 0x667000..0x66B560: _rpmalloc_mmap_os 0x667850, _rpmalloc_allocate 0x669D20, huge
 * 0x669B90, aligned 0x669EB0, posix_memalign 0x66B4B0), whose wrappers are reached only through pointers set at run
 * time, so their callers cannot be listed statically. s75a (frames inside rpmalloc only): 0x4A's +4.1 GB and 0x0B's
 * +3.0 GB both come through rpmalloc; 0x4A's 140 GB of span maps are churn. A region is found again on its free by
 * its allocation base (VirtualQuery). */
typedef LONG (NTAPI* MemProfAllocFn)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
typedef LONG (NTAPI* MemProfFreeFn)(HANDLE, PVOID*, PSIZE_T, ULONG);
static MemProfAllocFn g_memProfAlloc;
static MemProfFreeFn g_memProfFree;
#define MEMPROF_SLOTS 4096
#define MEMPROF_FRAMES 6
#define MEMPROF_REGIONS (1u << 18)
static struct { volatile LONG64 key; uintptr_t at[MEMPROF_FRAMES]; volatile LONG64 bytes, live, calls, shown; } g_memProf[MEMPROF_SLOTS];
typedef struct { volatile LONG64 base; volatile LONG64 bytes; volatile LONG slot; } MemProfRegion;
static MemProfRegion* g_memProfRegion; /* MEMPROF_REGIONS, allocated when the research is on */
static uintptr_t g_memProfRange[2][2];
static volatile LONG64 g_memProfOther, g_memProfOtherShown, g_memProfLost;
static uintptr_t MemProfImageEnd(uintptr_t base)
{
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    if (base == 0 || SafeRead(base, &dos, sizeof dos) || SafeRead(base + (uintptr_t)dos.e_lfanew, &nt, sizeof nt)) return base;
    return base + nt.OptionalHeader.SizeOfImage;
}
static uintptr_t MemProfAllocationBase(uintptr_t address)
{
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery((void*)address, &m, sizeof m) == sizeof m ? (uintptr_t)m.AllocationBase : 0;
}
static uint32_t MemProfRegionSlot(uintptr_t base, int add)
{
    for (uint32_t probe = 0, i = (uint32_t)((base >> 16) * 0x9E3779B1u) & (MEMPROF_REGIONS - 1); probe < 256; probe++, i = (i + 1) & (MEMPROF_REGIONS - 1))
    {
        LONG64 have = g_memProfRegion[i].base;
        if (have == (LONG64)base) return i;
        if (have == 0 || have == -1)
        {
            if (!add) { if (have == 0) return UINT32_MAX; continue; }
            if (InterlockedCompareExchange64(&g_memProfRegion[i].base, (LONG64)base, have) == have) return i;
            if (g_memProfRegion[i].base == (LONG64)base) return i;
        }
    }
    return UINT32_MAX;
}
static LONG NTAPI MemProfCommit(HANDLE process, PVOID* base, ULONG_PTR zero, PSIZE_T size, ULONG type, ULONG protect)
{
    LONG status = g_memProfAlloc(process, base, zero, size, type, protect);
    if (status >= 0 && (type & MEM_COMMIT) && size != NULL && *size >= 4096 && process == (HANDLE)(intptr_t)-1)
    {
        void* frames[48];
        USHORT n = RtlCaptureStackBackTrace(1, 48, frames, NULL);
        uintptr_t at[MEMPROF_FRAMES] = {0}; int k = 0;
        uintptr_t skipLo = g_memProfRange[0][0] + 0x667000u, skipHi = g_memProfRange[0][0] + 0x66B560u;
        for (USHORT i = 0; i < n && k < MEMPROF_FRAMES; i++)
        {
            uintptr_t f = (uintptr_t)frames[i];
            if (f >= skipLo && f < skipHi) continue;
            for (int m = 0; m < 2; m++) if (f >= g_memProfRange[m][0] && f < g_memProfRange[m][1]) { at[k++] = f; break; }
        }
        if (k == 0) { InterlockedAdd64(&g_memProfOther, (LONG64)*size); return status; }
        uint64_t h = 0x84222325CBF29CE4ull;
        for (int f = 0; f < MEMPROF_FRAMES; f++) h = (h ^ at[f]) * 0x100000001B3ull;
        LONG64 key = (LONG64)(h | 1);
        for (uint32_t probe = 0, slot = (uint32_t)(h >> 20) % MEMPROF_SLOTS; probe < 128; probe++, slot = (slot + 1) % MEMPROF_SLOTS)
        {
            LONG64 have = g_memProf[slot].key;
            if (have == 0 && InterlockedCompareExchange64(&g_memProf[slot].key, key, 0) == 0)
            { memcpy(g_memProf[slot].at, at, sizeof at); have = key; }
            if (have == key || g_memProf[slot].key == key)
            {
                InterlockedAdd64(&g_memProf[slot].bytes, (LONG64)*size); InterlockedAdd64(&g_memProf[slot].live, (LONG64)*size);
                InterlockedIncrement64(&g_memProf[slot].calls);
                uintptr_t region = MemProfAllocationBase((uintptr_t)*base);
                uint32_t r = region ? MemProfRegionSlot(region, 1) : UINT32_MAX;
                if (r != UINT32_MAX) { g_memProfRegion[r].slot = (LONG)slot; InterlockedAdd64(&g_memProfRegion[r].bytes, (LONG64)*size); }
                else InterlockedAdd64(&g_memProfLost, (LONG64)*size);
                break;
            }
        }
    }
    return status;
}
static LONG NTAPI MemProfRelease(HANDLE process, PVOID* base, PSIZE_T size, ULONG type)
{
    uintptr_t address = (process == (HANDLE)(intptr_t)-1 && base != NULL) ? (uintptr_t)*base : 0;
    SIZE_T want = size != NULL ? *size : 0;
    uintptr_t region = address ? MemProfAllocationBase(address) : 0;
    LONG status = g_memProfFree(process, base, size, type);
    if (status < 0 || region == 0) return status;
    uint32_t r = MemProfRegionSlot(region, 0);
    if (r == UINT32_MAX) return status;
    LONG64 had = g_memProfRegion[r].bytes, gone = (type & MEM_RELEASE) ? had : (LONG64)want;
    if (gone > had) gone = had;
    if (gone <= 0) return status;
    InterlockedAdd64(&g_memProfRegion[r].bytes, -gone);
    InterlockedAdd64(&g_memProf[g_memProfRegion[r].slot].live, -gone);
    if (type & MEM_RELEASE) InterlockedExchange64(&g_memProfRegion[r].base, -1);
    return status;
}
static void MemProfInstall(void)
{
    static int done;
    char t[4] = "";
    if (done || !(GetEnvironmentVariableA("HELLFORGE_XL_MEMPROF", t, sizeof t) > 0 && t[0] == '1')) return;
    done = 1;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    g_memProfRange[0][0] = exe; g_memProfRange[0][1] = MemProfImageEnd(exe);
    g_memProfRange[1][0] = g_world.game; g_memProfRange[1][1] = MemProfImageEnd(g_world.game);
    if (!ApiCodeMatches(exe + 0x667850u, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20"))
    { Note("difficulty: memprof: rpmalloc is not the reviewed one; not installed"); return; }
    g_memProfRegion = (MemProfRegion*)VirtualAlloc(NULL, sizeof(MemProfRegion) * MEMPROF_REGIONS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_memProfRegion == NULL) { Note("difficulty: memprof: no room for the region table"); return; }
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    FARPROC alloc = nt ? GetProcAddress(nt, "NtAllocateVirtualMemory") : NULL, release = nt ? GetProcAddress(nt, "NtFreeVirtualMemory") : NULL;
    if (alloc == NULL || release == NULL) { Note("difficulty: memprof: no NtAllocateVirtualMemory or NtFreeVirtualMemory"); return; }
    LoaderHook("extra large maps' memory attribution (research)", (uintptr_t)alloc, (void*)MemProfCommit, (void**)&g_memProfAlloc);
    LoaderHook("extra large maps' memory attribution, frees (research)", (uintptr_t)release, (void*)MemProfRelease, (void**)&g_memProfFree);
    Note("difficulty: memprof: commits and frees attributed to their callers above rpmalloc (exe %llX..%llX, game %llX..%llX)",
        (unsigned long long)g_memProfRange[0][0], (unsigned long long)g_memProfRange[0][1], (unsigned long long)g_memProfRange[1][0],
        (unsigned long long)g_memProfRange[1][1]);
}
static void MemProfReport(void)
{
    if (g_memProfAlloc == NULL) return;
    LONG64 other = g_memProfOther - g_memProfOtherShown;
    if (other >= (8ll << 20)) { Note("difficulty: memprof: %lld MB committed with no game frame on the stack", other >> 20); g_memProfOtherShown += other; }
    static char picked[MEMPROF_SLOTS];
    memset(picked, 0, sizeof picked);
    for (int top = 0; top < 8; top++)
    {
        int best = -1; LONG64 bestLive = 0;
        for (int i = 0; i < MEMPROF_SLOTS; i++)
            if (g_memProf[i].key != 0 && !picked[i] && g_memProf[i].live > bestLive) { bestLive = g_memProf[i].live; best = i; }
        if (best < 0 || bestLive < (64ll << 20)) break;
        picked[best] = 1;
        char where[MEMPROF_FRAMES * 20 + 8] = ""; size_t used = 0;
        for (int f = 0; f < MEMPROF_FRAMES && g_memProf[best].at[f] != 0; f++)
        {
            uintptr_t a = g_memProf[best].at[f];
            int x = a >= g_memProfRange[0][0] && a < g_memProfRange[0][1];
            used += (size_t)snprintf(where + used, sizeof where - used, "%s%s+0x%llX", f ? " < " : "", x ? "x" : "g",
                (unsigned long long)(a - g_memProfRange[x ? 0 : 1][0]));
            if (used >= sizeof where) break;
        }
        LONG64 grew = g_memProf[best].live - g_memProf[best].shown;
        Note("difficulty: memprof: %lld MB live (%+lld) by %s (%lld calls, %lld MB committed in all)", bestLive >> 20, grew >> 20, where,
            (long long)g_memProf[best].calls, (long long)(g_memProf[best].bytes >> 20));
        g_memProf[best].shown = g_memProf[best].live;
    }
    if (g_memProfLost >= (8ll << 20)) Note("difficulty: memprof: %lld MB in regions the table could not hold", (long long)(g_memProfLost >> 20));
}

typedef struct { DWORD cb, faults; SIZE_T peakWs, ws, peakPagedPool, pagedPool, peakNonPaged, nonPaged, page, peakPage, priv; } WorldMemCounters;
static volatile LONG64 g_memLast;
static void WorldMemoryNote(const char* where, uint32_t op, uint32_t record, int force)
{
    static BOOL (WINAPI* info)(HANDLE, void*, DWORD);
    static int looked;
    if (!looked) { looked = 1; HMODULE k = GetModuleHandleA("kernel32.dll"); if (k) info = (BOOL (WINAPI*)(HANDLE, void*, DWORD))(void*)GetProcAddress(k, "K32GetProcessMemoryInfo"); }
    if (info == NULL) return;
    WorldMemCounters c; memset(&c, 0, sizeof c); c.cb = sizeof c;
    if (!info(GetCurrentProcess(), &c, sizeof c)) return;
    LONG64 priv = (LONG64)(c.priv ? c.priv : c.page), last = g_memLast; /* Wine leaves PrivateUsage 0: its commit (PagefileUsage) instead */
    if (!force && priv - last < (64ll << 20) && last - priv < (256ll << 20)) return;
    g_memLast = priv;
    MEMORYSTATUSEX m; m.dwLength = sizeof m; GlobalMemoryStatusEx(&m);
    if (op != 0xFFFFFFFFu)
        Note("difficulty: memory: %s op 0x%X record %u at %.0fx: commit %lld MB (%+lld), working set %llu MB (peak %llu), system available %llu MB",
            where, op, record, (double)g_world.current, priv >> 20, (priv - last) >> 20, (unsigned long long)(c.ws >> 20), (unsigned long long)(c.peakWs >> 20),
            (unsigned long long)(m.ullAvailPhys >> 20));
    else
        Note("difficulty: memory: %s at %.0fx: commit %lld MB (%+lld), working set %llu MB (peak %llu), system available %llu MB",
            where, (double)g_world.current, priv >> 20, (priv - last) >> 20, (unsigned long long)(c.ws >> 20), (unsigned long long)(c.peakWs >> 20),
            (unsigned long long)(m.ullAvailPhys >> 20));
    MemProfReport();
}

static void WorldMemoryTick(void)
{
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (now - last < 30000) return;
    last = now;
    WorldMemoryNote("in play", 0xFFFFFFFFu, 0, 1);
}

/* Evidence (content over the whole map): the op that appended each location (locations are only appended, so an index
 * keeps its op), and locations-<level>.tsv after op 0x2B: index, op, position in world metres (the generator's less half
 * the world; x east, y north), type (+0xD8) and flags (+0xD4) of each 0xE0 record. */
static uint8_t g_locationOp[DIF_LOCATIONS];
static void WorldLocationsNote(uintptr_t block, uint32_t total)
{
    uint32_t offset = 0;
    if (SafeRead(g_world.game + 0xC176A7 + 3, &offset, 4) || offset < 0x90) return;
    wchar_t name[48];
    swprintf(name, 48, L"locations-%ld.tsv", (long)g_world.levels);
    HANDLE file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return;
    float half = 1024.0f * g_world.current;
    char line[128];
    DWORD wrote;
    uint32_t n = total < DIF_LOCATIONS ? total : DIF_LOCATIONS, written = 0, important = 0;
    SafeRead(block + 0x11A715Cu, &important, 4);
    for (uint32_t i = 0; i < n; i++)
    {
        uint8_t rec[0xE0];
        if (SafeRead(block + (offset - 0x90) + (uint64_t)i * 0xE0, rec, sizeof rec)) break;
        float xy[2]; uint32_t type; uint16_t flags;
        memcpy(xy, rec, 8); memcpy(&type, rec + 0xD8, 4); memcpy(&flags, rec + 0xD4, 2);
        /* the objective id for an op 0x16 / 0x17 location: its important record (they are appended together through the
         * placement ops, "locations a -> b, important a -> b") when that is an objective's (kind 0xADC32FAA at +8, id +0xD8) */
        uint32_t objective = 0, kind = 0;
        if ((g_locationOp[i] == 0x16 || g_locationOp[i] == 0x17) && i < important && !SafeRead(block + g_importantList + 0x108ull * i + 8, &kind, 4) && kind == 0xADC32FAAu)
            SafeRead(block + g_importantList + 0x108ull * i + 0xD8, &objective, 4);
        int len = snprintf(line, sizeof line, "%u\t0x%X\t%.1f\t%.1f\t%u\t0x%X\t%08x\n", i, g_locationOp[i], (double)(xy[0] - half), (double)(xy[1] - half),
            type, flags, objective);
        WriteFile(file, line, (DWORD)len, &wrote, NULL);
        written++;
    }
    CloseHandle(file);
    Note("difficulty: extra large maps: locations-%ld.tsv written (%u locations)", (long)g_world.levels, written);
    /* edges-<level>.tsv: each route edge (0x48: ends +0 / +8 in generator metres, cells +0x20 / +0x28, width +0x30, type
     * +0x34, flags +0x44) with the type index of the subregion on each side (subregion +0x18 = its route cell, +0xC its
     * type); types-<level>.tsv: the first 0x240 bytes of each used type descriptor ([[level+0x2D0]] + 8 t), as hex. */
    uint32_t subs = 0, edges = 0, types = 0;
    uint64_t ts = 0, table = 0;
    if (SafeRead(block + 0x11A71C4u, &subs, 4) || SafeRead(block + 0x11A71CCu, &edges, 4) || subs > 8192 || edges > 65536) return;
    static uint64_t cellOf[8192]; static int32_t typeOf[8192];
    for (uint32_t i = 0; i < subs; i++)
    {
        cellOf[i] = 0; typeOf[i] = -1;
        SafeRead(block + 0x92F328u + 0x20ull * i + 0x18, &cellOf[i], 8); SafeRead(block + 0x92F328u + 0x20ull * i + 0xC, &typeOf[i], 4);
    }
    swprintf(name, 48, L"edges-%ld.tsv", (long)g_world.levels);
    file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return;
    uint8_t used[256] = {0};
    for (uint32_t e = 0; e < edges; e++)
    {
        uint8_t rec[0x48];
        if (SafeRead(block + g_routeEdges + 0x48ull * e, rec, sizeof rec)) break;
        float a[2], b[2], width; uint64_t c0, c1; uint16_t flags;
        memcpy(a, rec, 8); memcpy(b, rec + 8, 8); memcpy(&c0, rec + 0x20, 8); memcpy(&c1, rec + 0x28, 8); memcpy(&width, rec + 0x30, 4); memcpy(&flags, rec + 0x44, 2);
        int32_t t0 = -2, t1 = -2;
        for (uint32_t i = 0; i < subs && (t0 == -2 || t1 == -2); i++)
        {
            if (c0 && cellOf[i] == c0) t0 = typeOf[i];
            if (c1 && cellOf[i] == c1) t1 = typeOf[i];
        }
        if (t0 >= 0 && t0 < 256) used[t0] = 1; if (t1 >= 0 && t1 < 256) used[t1] = 1;
        int len = snprintf(line, sizeof line, "%u\t%.1f\t%.1f\t%.1f\t%.1f\t%.2f\t%u\t0x%X\t%d\t%d\n", e, (double)(a[0] - half), (double)(a[1] - half),
            (double)(b[0] - half), (double)(b[1] - half), (double)width, rec[0x34], flags, t0, t1);
        WriteFile(file, line, (DWORD)len, &wrote, NULL);
    }
    CloseHandle(file);
    for (uint32_t i = 0; i < subs; i++) if (typeOf[i] >= 0 && typeOf[i] < 256) used[typeOf[i]] = 1;
    if (SafeRead(g_worldLevel + 0x2D0, &ts, 8) || ts == 0 || SafeRead(ts, &table, 8) || table == 0 || SafeRead(ts + 0xA0, &types, 4)) return;
    swprintf(name, 48, L"types-%ld.tsv", (long)g_world.levels);
    file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return;
    for (uint32_t t = 0; t < types && t < 256; t++)
    {
        uint64_t d = 0; uint8_t raw[0x240];
        if (!used[t] || SafeRead(table + 8ull * t, &d, 8) || d == 0 || SafeRead(d, raw, sizeof raw)) continue;
        char hex[2 * sizeof raw + 32];
        int len = snprintf(hex, 32, "%u\t", t);
        for (size_t k = 0; k < sizeof raw; k++) len += snprintf(hex + len, 3, "%02X", raw[k]);
        hex[len++] = '\n';
        WriteFile(file, hex, (DWORD)len, &wrote, NULL);
    }
    CloseHandle(file);
    Note("difficulty: extra large maps: edges-%ld.tsv (%u edges) and types-%ld.tsv written", (long)g_world.levels, edges, (long)g_world.levels);
}

/* The open land's route edges as a nature map's. On a structured (city) map 0xFD03E0 makes every route edge type 1 with
 * flags 0x104 (0xFD0742..0xFD074C, when [[ctx+8]+0xC0]+0x1D == 2) and its nodes +0x90 |= 6 (0xFD08A3, 0xFD09FF); any
 * other map gets type 3 and no flags (0xFD0731). On a vanilla city map every cell is city, so every edge is a street. On
 * the XL city map the open land's borders became streets too: op 0x41's path B paints the city street material along
 * every flag-0x100 edge (0xF51C20) and op 0xA8 flattens between nodes that both have +0x90 bit 1 (0xFAF6B5, 0xFAF947),
 * straight lines across the open land (audit 12, s75o; static, subagent seams). So once the zones are final (the level's
 * op 0x38, before the first tile copies the block): an edge with no city cell on either side (its cells +0x20 / +0x28,
 * the subregion of each (+0x18 its route cell) and that subregion's type (+0xC) +0x170 bit 1, as 0xA7 tests at 0xFAA2F2)
 * gets type 3 and loses 0x104; a node whose every edge (+0x10, 16 slots) is such an edge loses +0x90 bits 1 and 2. */
static void WorldOpenEdgesPlain(uintptr_t gen)
{
    static LONG done = -1;
    if (done == g_world.levels || g_worldLevel == 0) return;
    done = g_world.levels;
    uint32_t subs = 0, edges = 0, nodes = 0, types = 0;
    uint64_t ts = 0, table = 0;
    if (SafeRead(gen + 0x11A71C4u, &subs, 4) || SafeRead(gen + 0x11A71CCu, &edges, 4) || SafeRead(gen + 0x11A71B4u, &nodes, 4) || subs > 8192 ||
        edges > 65536 || nodes > 65536 || SafeRead(g_worldLevel + 0x2D0, &ts, 8) || ts == 0 || SafeRead(ts, &table, 8) || table == 0 || SafeRead(ts + 0xA0, &types, 4))
        return;
    static uint64_t cellOf[8192]; static uint8_t cityOf[8192];
    uint32_t cityCells = 0;
    for (uint32_t i = 0; i < subs; i++)
    {
        int32_t t = -1; uint64_t d = 0; uint32_t flags = 0;
        cellOf[i] = 0; cityOf[i] = 0;
        SafeRead(gen + 0x92F328u + 0x20ull * i + 0x18, &cellOf[i], 8);
        if (!SafeRead(gen + 0x92F328u + 0x20ull * i + 0xC, &t, 4) && t >= 0 && (uint32_t)t < types && !SafeRead(table + 8ull * (uint32_t)t, &d, 8) && d &&
            !SafeRead(d + 0x170, &flags, 4) && (flags & 2)) { cityOf[i] = 1; cityCells++; }
    }
    if (cityCells == 0) return; /* not a city map */
    uintptr_t E = gen + g_routeEdges, N = gen + g_routeNodes;
    uint32_t streets = 0, plain = 0, unknown = 0, plainNodes = 0;
    static uint8_t openEdge[65536];
    for (uint32_t e = 0; e < edges; e++)
    {
        uint64_t c[2] = {0, 0}; uint16_t flags = 0; uint8_t type = 0;
        openEdge[e] = 0;
        if (SafeRead(E + 0x48ull * e + 0x20, &c[0], 8) || SafeRead(E + 0x48ull * e + 0x28, &c[1], 8) || SafeRead(E + 0x48ull * e + 0x44, &flags, 2) ||
            SafeRead(E + 0x48ull * e + 0x34, &type, 1)) break;
        if (!(flags & 0x100) || type != 1) continue;
        streets++;
        int side[2] = {-1, -1};
        for (int q = 0; q < 2; q++)
            for (uint32_t i = 0; i < subs && c[q]; i++) if (cellOf[i] == c[q]) { side[q] = cityOf[i]; break; }
        if (side[0] < 0 && side[1] < 0) { unknown++; continue; }
        if (side[0] == 1 || side[1] == 1) continue;
        *(volatile uint8_t*)(E + 0x48ull * e + 0x34) = 3;
        *(volatile uint16_t*)(E + 0x48ull * e + 0x44) = (uint16_t)(flags & ~0x104u);
        openEdge[e] = 1; plain++;
    }
    for (uint32_t k = 0; k < nodes; k++)
    {
        uintptr_t node = N + 0x98ull * k;
        uint8_t bits = 0; int any = 0, allOpen = 1;
        if (SafeRead(node + 0x90, &bits, 1)) break;
        if (!(bits & 6)) continue;
        for (int q = 0; q < 16; q++)
        {
            uint64_t ep = 0;
            if (SafeRead(node + 0x10 + 8ull * q, &ep, 8)) { allOpen = 0; break; }
            if (ep == 0) continue;
            if (ep < E || ep >= E + 0x48ull * edges || (ep - E) % 0x48) { allOpen = 0; break; }
            any = 1;
            if (!openEdge[(ep - E) / 0x48]) { allOpen = 0; break; }
        }
        if (any && allOpen) { *(volatile uint8_t*)(node + 0x90) = (uint8_t)(bits & ~6u); plainNodes++; }
    }
    Note("difficulty: extra large maps: the open land's route edges as a nature map's: %u of %u street edges (%u city cells; %u with no known cell), %u of %u nodes",
        plain, streets, cityCells, unknown, plainNodes, nodes);
}

/* No water on the extra large city map, as on a vanilla city map. The level's cell types ([[level+0x2D0]], count +0xA0)
 * carry a water level at +0xE4; op 0x0A (0xF6C610 phase 0) spawns a water body for each used type (ids G+0x18008, the
 * record built at 0xC23F6A / 0xC26AD2: pass+0x10 = that table) whose level is above -500. A vanilla city map's types are
 * at -501, so it has none. The XL city map's open land takes the region's open types (s75t: types 3/4/5/30 at -4 / -3.99
 * / -1.05 / -10), whose basins lie 0.1-0.2 m under their level over 8 km2 (3 % of the map): sheets with a sawtooth
 * shoreline at the 4 m heightmap (audit 13), and WaterLevelAt (0x13D4620) reads the 16 km water map as if it were 2 km
 * (KB xl-world-streaming, water ops). So on such a level each type with a level above -500 is set to -501 before the
 * level's op 0x0A, as soon as the level's map type is set (every generation reader after it, h_macro's wet holds and the
 * tiles see a dry type), and put back at the next level's first op, while the descriptor is still in that level's table and still holds -501 (else it was unloaded or
 * replaced: left alone). */
#define DRY_LEVEL -501.0f
static struct { LONG level; uint32_t count; uint64_t type[256]; float was[256]; } g_dry = {.level = -1};

static int DryWritable(uint64_t at)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)(uintptr_t)at, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return 0;
    return (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0 && !(mbi.Protect & PAGE_GUARD);
}

static void WorldDryCity(uint64_t op)
{
    static LONG restoredFor = -1, decided = -1;
    if (decided == g_world.levels || g_worldLevel == 0) return;
    uint64_t ts = 0, table = 0, mt = 0;
    uint32_t types = 0;
    if (SafeRead(g_worldLevel + 0x2D0, &ts, 8) || ts == 0 || SafeRead(ts, &table, 8) || table == 0 || SafeRead(ts + 0xA0, &types, 4)) return;
    if (types > 256) types = 256;
    if (restoredFor != g_world.levels)
    {
        restoredFor = g_world.levels;
        uint32_t restored = 0, dropped = 0;
        for (uint32_t i = 0; i < g_dry.count; i++)
        {
            int live = 0;
            for (uint32_t t = 0; t < types && !live; t++) { uint64_t d = 0; if (!SafeRead(table + 8ull * t, &d, 8) && d == g_dry.type[i]) live = 1; }
            float now = 0;
            if (live && !SafeRead(g_dry.type[i] + 0xE4, &now, 4) && now == DRY_LEVEL && DryWritable(g_dry.type[i] + 0xE4))
            { *(volatile float*)(uintptr_t)(g_dry.type[i] + 0xE4) = g_dry.was[i]; restored++; }
            else dropped++;
        }
        if (g_dry.count) Note("difficulty: extra large maps: water levels put back on %u types (%u no longer this level's or changed: left alone)", restored, dropped);
        g_dry.count = 0;
    }
    if (!(g_world.current > 1.0f)) { decided = g_world.levels; return; }
    /* the level's map type, [lg+0x2DE0]+0x1D (= [[ctx+8]+0xC0]+0x1D: the settings are lg+0x2D20); set some ops into the
     * level (s75u: not yet at its first op), so asked again at each op until it is */
    uint8_t mapType = 0xFF;
    if (SafeRead(g_worldLevel + 0x2DE0, &mt, 8) || mt == 0 || SafeRead(mt + 0x1D, &mapType, 1))
    {
        if (op == 0x0A) { decided = g_world.levels; Note("difficulty: extra large maps: the level's map type is not set at op 0x0A; its water is left as it is"); }
        return;
    }
    decided = g_world.levels;
    if (mapType != 2) { Note("difficulty: extra large maps: map type %u keeps its water (only the city map, type 2, is made dry)", mapType); return; }
    char text[512]; size_t at = 0; text[0] = 0;
    uint32_t refused = 0;
    for (uint32_t t = 0; t < types; t++)
    {
        uint64_t d = 0; float level = DRY_LEVEL;
        if (SafeRead(table + 8ull * t, &d, 8) || d == 0 || SafeRead(d + 0xE4, &level, 4) || !(level > -500.0f)) continue;
        if (!DryWritable(d + 0xE4)) { refused++; continue; }
        g_dry.type[g_dry.count] = d; g_dry.was[g_dry.count] = level; g_dry.count++;
        *(volatile float*)(uintptr_t)(d + 0xE4) = DRY_LEVEL;
        if (at < sizeof text - 24) at += (size_t)snprintf(text + at, sizeof text - at, " %u (%.2f)", t, (double)level);
    }
    Note("difficulty: extra large maps: the city map's open land has no water (as a vanilla city map), before op 0x%02X: %u types set to %.0f:%s%s",
        (unsigned)op, g_dry.count, (double)DRY_LEVEL, g_dry.count ? text : " none had one", refused ? " (some descriptors not writable: kept)" : "");
}

static void CityGatesNote(uint32_t op); /* difficulty_world_gates.c: the city ops' gates about each city, per level */
/* Load time: the stamp packages queued early (~/xl-work/static/loadtime/packages.md, hellforge-1a, static).
 * QueueStampPackages (op 0x1F, record 94 of a 16 km pipeline, 0xF405E0(pass, record, ctx)) builds its package list
 * from the level's location-type list C and requests each package ([api+0x10]+0x2F0) in one call; the waits after it
 * (0x1D, 0x20 WaitForStampPackages, 0xA6) then sit ~13 s while the engine's loader brings the packages in one at a time
 * (a FIFO ring, one package a frame at most, exe 0x603290). C is final once the last op holding it has run (none of
 * the records between it and 0x2B hold it; 0x2B and the loader's stamp planning never write it). So on the first
 * main-thread step after that op (the dispatcher's mask argument 0, the record's flags +0x1998 bit 1: the generator
 * job is idle) the loader calls 0xF405E0 itself on a copy of record 94 whose output (pass +0x30 the list, +0x38 its
 * count) is its own buffer, with a copy of the context whose arena is its own (0xF405E0 reads only ctx +0, +0x18,
 * +0xD0). The packages load during the coarse terrain and the stamp planning; record 94's own requests then find them
 * loaded ("Package already loaded": no second request, 0x603746) or queued (the flag set again, 0x6036D0), so its
 * list, the waits and the teardown's unload of G+0x757220 are as before. Off with HELLFORGE_XL_EARLY_PACKAGES=0. */
#define EARLY_PACKAGES_MAX 8192u
static struct
{
    LONG level;                  /* g_world.levels the scan is for */
    int state;                   /* 0 waiting, 1 requested, 2 record 94 reached, -1 not on this level */
    uint32_t lastC, at2B, at1F;
    uint32_t count;              /* packages requested early */
    ULONGLONG requested;
    uint64_t list[EARLY_PACKAGES_MAX];
    uint8_t record[0x19B0];
} g_early = {.level = -1};
static const uint8_t kEarlyCHolders[] = {0x9A, 0x16, 0x06, 0x17, 0x82, 0x8B, 0xA0, 0x8E, 0x88, 0x09, 0xAB, 0xA7, 0xA8};
static void EarlyPackages(uint64_t records, uint64_t context, uint64_t index, uint64_t mask)
{
    static int on = -1;
    if (on < 0) { char v[8] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_EARLY_PACKAGES", v, sizeof v) > 0 && v[0] == '0'); }
    if (!on || g_worldLevel == 0 || !(g_world.current > 1.0f)) return;
    uint64_t block = 0, gen = 0; /* the records' block ([level+0x2A8]) and the generator block G ([level+0x2C8]) */
    if (SafeRead(g_worldLevel + 0x2A8, &block, 8) || block != records || SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0) return;
    uint32_t i = 0;
    if (SafeRead(index, &i, 4)) return;
    /* a level that ended (or generated again) before its record 94: the early requests are released as the teardown
     * releases record 94's ([api+0x10]+0x308(list, count), 0xF70B58..0xF70B77), on a main-thread step */
    if (g_early.level != g_world.levels && g_early.state == 1) { g_early.state = 3; }
    if (g_early.state == 3 && mask == 0)
    {
        uint64_t api = 0, table = 0, release = 0;
        if (!SafeRead(g_world.game + 0x3326308u, &api, 8) && api != 0 && !SafeRead(api + 0x10, &table, 8) && table != 0 && !SafeRead(table + 0x308, &release, 8) && release != 0)
            ((void (*)(uint64_t*, uint32_t))(uintptr_t)release)(g_early.list, g_early.count);
        Note("difficulty: early packages: level %ld ended before its record 94: its %u early packages released", (long)g_early.level, g_early.count);
        g_early.state = -1; g_early.level = -1;
    }
    if (g_early.state == 3) return;
    if (g_early.level != g_world.levels)
    {
        g_early.level = g_world.levels; g_early.state = -1; g_early.count = 0;
        g_early.lastC = 0; g_early.at2B = 0xFFFFFFFFu; g_early.at1F = 0xFFFFFFFFu;
        uint32_t end = 0; SafeRead(records + 0x19B000u, &end, 4); /* the record count ([block+0x19B000], 0xF740B0) */
        if (end == 0 || end > 1024) return;
        for (uint32_t k = 0; k < end; k++)
        {
            uint32_t op = 0;
            if (SafeRead(records + (uint64_t)k * 0x19B0u, &op, 4)) return;
            if (op == 0x2B && g_early.at2B == 0xFFFFFFFFu) g_early.at2B = k;
            else if (op == 0x1F && g_early.at2B != 0xFFFFFFFFu && g_early.at1F == 0xFFFFFFFFu) g_early.at1F = k;
            if (g_early.at2B == 0xFFFFFFFFu) for (size_t c = 0; c < sizeof kEarlyCHolders; c++) if (op == kEarlyCHolders[c]) g_early.lastC = k;
        }
        if (g_early.at2B == 0xFFFFFFFFu || g_early.at1F == 0xFFFFFFFFu) return;
        g_early.state = 0;
    }
    if (g_early.state == 1 && i > g_early.at1F) /* record 94 has run (its list is in G now) */
    {
        g_early.state = 2;
        uint32_t n = 0; uint64_t api = 0, table = 0, has = 0; uint32_t loaded = 0;
        SafeRead(gen + 0x11A70F4u, &n, 4);
        uint32_t same = n == g_early.count;
        for (uint32_t k = 0; k < n && k < EARLY_PACKAGES_MAX && same; k++) { uint64_t id = 0; SafeRead(gen + 0x757220u + 8ull * k, &id, 8); same = id == g_early.list[k]; }
        if (!SafeRead(g_world.game + 0x3326308u, &api, 8) && api != 0 && !SafeRead(api + 0x10, &table, 8) && table != 0 && !SafeRead(table + 0x310, &has, 8) && has != 0)
            for (uint32_t k = 0; k < g_early.count; k++) loaded += ((uint8_t (*)(uint64_t))(uintptr_t)has)(g_early.list[k]) != 0;
        Note("difficulty: early packages: record %u (after 0x1F) reached %.1f s after the early request: %u of the %u packages loaded; record %u's own list %s (%u)",
            i, (double)(GetTickCount64() - g_early.requested) / 1000.0, loaded, g_early.count, g_early.at1F, same ? "is the same" : "DIFFERS", n);
        return;
    }
    if (g_early.state != 0 || mask != 0 || i <= g_early.lastC || i >= g_early.at2B) return;
    uint64_t flags = 0;
    if (SafeRead(records + (uint64_t)i * 0x19B0u + 0x1998u, &flags, 8) || !(flags & 2)) return;
    static int pinned = -1;
    if (pinned < 0) pinned = ApiCodeMatches(g_world.game + 0xF40DF9u, "48 8B 40 38 44 89 38 45 85 FF 74 29");
    uint64_t api = 0, table = 0, depthFn = 0;
    uint32_t cCount = 0;
    if (pinned != 1 || SafeRead(records + (uint64_t)g_early.at1F * 0x19B0u, g_early.record, sizeof g_early.record) ||
        SafeRead(g_world.game + 0x3326308u, &api, 8) || api == 0 || SafeRead(api + 0x10, &table, 8) || table == 0 || SafeRead(table + 0x358, &depthFn, 8) ||
        depthFn == 0 || SafeRead(gen + 0x11A70F0u, &cCount, 4) || cCount > 512)
    { g_early.state = -1; Note("difficulty: early packages: not the reviewed op 0x1F or engine table; the packages load at record %u as before", g_early.at1F); return; }
    uint32_t depth = ((uint32_t (*)(void))(uintptr_t)depthFn)();
    /* the ring has 512 places and no full check (0x603777); record 94's own list has 256 places (G+0x757220), so its
     * packages number at most 256 on a level that works today: the next flag-2 step tries again while it is fuller */
    if (depth + 256u >= 512u)
    {
        static LONG noted = -1;
        if (noted != g_world.levels) { noted = g_world.levels; Note("difficulty: early packages: the engine's queue holds %u (%u location types): later", depth, cCount); }
        return;
    }
    uint64_t out = (uint64_t)(uintptr_t)g_early.list, countAt = (uint64_t)(uintptr_t)&g_early.count;
    g_early.count = 0;
    memcpy(g_early.record + 8 + 0x30, &out, 8); memcpy(g_early.record + 8 + 0x38, &countAt, 8);
    TileRunner* r = TileRunnerMake(TILE_RUNNERS_MAX - 1);
    uint8_t settings[0x120]; uint64_t S = 0, P = 0;
    if (r == NULL || SafeRead(context + 8, &S, 8) || S == 0 || SafeRead(S, settings, sizeof settings) || SafeRead(context + 0x20, &P, 8) ||
        SafeRead(context, r->ctx, sizeof r->ctx))
    { g_early.state = -1; return; }
    uint8_t levelContext[0xE8]; memcpy(levelContext, r->ctx, sizeof levelContext);
    TileRunnerContext(r, levelContext, settings, (uintptr_t)P);
    uint32_t opNow = 0; SafeRead(records + (uint64_t)i * 0x19B0u, &opNow, 4);
    LARGE_INTEGER t0, t1, hz; QueryPerformanceCounter(&t0);
    LONG fault = WorldTileGuardedCall(g_world.game + 0xF405E0u, g_early.record, (uint64_t)(uintptr_t)r->ctx);
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&hz);
    WorldArenaReset((uint64_t)(uintptr_t)r->ctx);
    g_early.state = fault ? -1 : 1; g_early.requested = GetTickCount64();
    Note("difficulty: early packages: at record %u (op 0x%02X, after record %u, the last op holding the location types), %u stamp packages requested for record %u in %.2f ms; the engine's queue held %u%s",
        i, opNow, g_early.lastC, g_early.count, g_early.at1F, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart, depth,
        fault ? "; FAULTED" : g_early.count > 256 ? "; more than record 94's 256 places" : "");
}

/* The frame cap while an extra large level loads (load time). Application::update ends with a throttle (exe
 * 0x8AB70..0x8ABC1): while fps = [app+0x470] != 0, it sleeps until 1 / fps has passed since the frame mark; app =
 * [exe+0x1A10208] (the settings call 0x318980, item 1, stores the framerate limit there). A 16 km load has frame-paced
 * parts: the coarse terrain draws a tile a frame (one 0x3C render a frame), the near tile's gui batches, the material
 * pass's queue waits, the main-thread records, the package loader's states. With a 30 or 60 fps cap each such frame is
 * padded to 33 or 17 ms (t77n: 515 tiles in 1032 frames, 34.4 s at the harness's 30 fps). So from the first record of
 * a full load generation (one whose records hold op 0x4A: the briefing's drop map has none) to its last record the
 * cap is 0 (no throttle; vsync, a render-side wait, is untouched), then the player's value is put back; also when
 * another level begins or the ship is set up. The only reader is the main thread's frame end. */
static struct { LONG level; int on; uint32_t saved; uintptr_t app; } g_fpsLift = {.level = -1};
static void FpsLiftEnd(const char* why)
{
    if (!g_fpsLift.on) return;
    g_fpsLift.on = 0;
    uint32_t now = 0;
    if (!SafeRead(g_fpsLift.app + 0x470, &now, 4) && now == 0) *(volatile uint32_t*)(g_fpsLift.app + 0x470) = g_fpsLift.saved;
    Note("difficulty: extra large maps: the frame cap is %u fps again (%s)", g_fpsLift.saved, why);
}
static void FpsLiftStep(uint64_t records, uint64_t index, uint32_t count)
{
    /* the level's own records only ([level+0x2A8]): the tile replays and the runners pass a record of their own */
    uint64_t own = 0;
    if (g_worldLevel == 0 || SafeRead(g_worldLevel + 0x2A8, &own, 8) || own != records) return;
    if (g_fpsLift.level != g_world.levels)
    {
        FpsLiftEnd("another level");
        g_fpsLift.level = g_world.levels;
        if (!(g_world.current > 1.0f) || count == 0 || count > 4096) { if (g_world.current > 1.0f) Note("difficulty: extra large maps: frame cap check: %u records", count); return; }
        int full = 0;
        for (uint32_t i = 0; i < count && !full; i++) { uint32_t op = 0; if (!SafeRead(records + (uint64_t)i * 0x19B0u, &op, 4) && op == 0x4A) full = 1; }
        uintptr_t app = 0; uint32_t fps = 0;
        int bad = SafeRead((uintptr_t)GetModuleHandleW(NULL) + 0x1A10208u, &app, 8) || app == 0 || SafeRead(app + 0x470, &fps, 4);
        Note("difficulty: extra large maps: frame cap check: %u records%s, app %p, [app+0x470] %u%s", count, full ? " with op 0x4A" : " without op 0x4A",
            (void*)app, fps, bad ? " (not read)" : "");
        if (!full || bad || fps == 0 || fps > 1000) return;
        g_fpsLift.app = app; g_fpsLift.saved = fps; g_fpsLift.on = 1;
        *(volatile uint32_t*)(app + 0x470) = 0;
        Note("difficulty: extra large maps: the frame cap (%u fps) is off while the map loads", fps);
    }
    uint32_t i = 0;
    if (g_fpsLift.on && !SafeRead(index, &i, 4) && i + 1 >= count) FpsLiftEnd("the level's last record");
}

static uint64_t WorldDispatch(uint64_t records, uint64_t b, uint64_t index, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    FpsLiftStep(records, index, (uint32_t)d);
    EarlyPackages(records, b, index, f);
    uint32_t diagAt, diagOp;
    TerrainDiagBefore(records, index, (uint32_t)d, &diagAt, &diagOp);
    {
        uint32_t i = 0, o = 0xFFFFFFFFu;
        if (!SafeRead(index, &i, 4) && !SafeRead(records + (uint64_t)i * 0x19B0u, &o, 4)) WorldDryCity(o); /* once a level, before op 0x0A */
    }
    if (g_world.current == 8.0f) WorldCamInstall();
    {
        uint32_t i = 0, o = 0xFFFFFFFFu;
        InterlockedIncrement(&g_genCalls);
        if (!SafeRead(index, &i, 4) && !SafeRead(records + (uint64_t)i * 0x19B0u, &o, 4) && (LONG)i != g_genRecord)
        {
            g_genRecord = (LONG)i; g_genOp = (LONG)o; g_genOpSince = (LONG64)GetTickCount64();
        }
    }
    if (g_heightCopy != NULL)
    {
        uint32_t i = 0, o = 0;
        if (!SafeRead(index, &i, 4) && !SafeRead(records + (uint64_t)i * 0x19B0u, &o, 4) && o == 0x6D) WorldDumpHeight();
    }
    {
        uint32_t i = 0, o = 0;
        if (!SafeRead(index, &i, 4) && !SafeRead(records + (uint64_t)i * 0x19B0u, &o, 4) && o == 0x6D) { WorldZoneGridNote(); WorldScatterNote(); WorldSubregionNote(); if (g_world.current == 8.0f) WorldRecordMap(records, (uint32_t)d); }
        /* The tile runs as the level's SetupTerrainMerge (0x38) begins: after its own heights and slopes, while the
         * generation's resources are alive (GenerateSubregionHeights samples textures whose CPU data is released after
         * generation: a tile at op 0x6D read a null image at exe+0x31CA66). */
        if (o == 0x38 && g_worldLevel != 0 && g_world.current > 1.0f)
        {
            uintptr_t genAt = 0;
            if (!SafeRead(g_worldLevel + 0x2C8, &genAt, 8) && genAt != 0) WorldOpenEdgesPlain(genAt);
        }
        if (o == 0x38 && g_worldLevel != 0)
        {
            uintptr_t genAt = 0; uint32_t subs = 0;
            if (!SafeRead(g_worldLevel + 0x2C8, &genAt, 8) && genAt != 0 && !SafeRead(genAt + 0x11A71C4u, &subs, 4)) WorldSuggestDrop(genAt, subs);
        }
        /* the level's heightmap at one mip before anything writes it (difficulty_world_terrain.c, NearOneMipLevel) */
        if (o == 0x38 && g_world.current == 8.0f && TerrainOn()) NearOneMipLevel();
        if (o == 0x38 && g_world.current == 8.0f && WorldTileStep(records, (uint32_t)d, i, b, e, f, g, h)) { TerrainDiagAfter(records, index, diagAt, diagOp); return 0; }
        /* the map's terrain at vanilla density: the level waits at its SmoothHeightmap while the tiles are drawn */
        if (o == 0x40 && TerrainStep(records, (uint32_t)d, i, b, g, h)) { TerrainDiagAfter(records, index, diagAt, diagOp); return 0; }
        if (o == 0x0A && g_world.current == 8.0f) TerrainNearWater(records + (uint64_t)i * 0x19B0u);
        if (o == 0x6C && g_world.current == 8.0f) MacroStamps(); /* once a level, before its first call */
        if (g_world.current == 8.0f && TerrainNearStep(records, (uint32_t)d, i, o, b, g, h)) { TerrainDiagAfter(records, index, diagAt, diagOp); return 0; }
    }
    uint32_t at = 0, op = 0xFFFFFFFFu, before = 0, after = 0;
    uintptr_t block = 0;
    int watch = g_world.current > 1.0f && g_worldLevel != 0 && !SafeRead(g_worldLevel + 0x2C8, &block, 8) && block != 0 &&
        !SafeRead(index, &at, 4) && !SafeRead(records + (uint64_t)at * 0x19B0u, &op, 4) && !SafeRead(block + 0x11A7108, &before, 4);
    /* Research (HELLFORGE_XL_CENSUS=1): the op attribution and locations-<level>.tsv on a level the loader does not grow
     * too (a vanilla-size difficulty's mission as the per-mission baseline of an extra large map's content); nothing
     * else changes on such a level. */
    static int census = -1;
    if (census < 0) { char v[8] = ""; census = GetEnvironmentVariableA("HELLFORGE_XL_CENSUS", v, sizeof v) > 0 && v[0] == '1'; }
    int counted = watch;
    if (!watch && census && g_worldLevel != 0)
    {
        block = 0; op = 0xFFFFFFFFu; before = 0;
        counted = !SafeRead(g_worldLevel + 0x2C8, &block, 8) && block != 0 && !SafeRead(index, &at, 4) &&
            !SafeRead(records + (uint64_t)at * 0x19B0u, &op, 4) && !SafeRead(block + 0x11A7108, &before, 4);
        if (!counted) { block = 0; op = 0xFFFFFFFFu; before = 0; }
    }
    /* op 0x41's material painters in vanilla space (difficulty_world_terrain.c): the level waits while the job queue
     * drains around it */
    int matPatch = 0;
    if (watch)
    {
        int m = TerrainMatDispatch(op, b);
        if (m == 0) return 0;
        matPatch = m == 2;
    }
    if (watch && at != g_worldOpAt) { g_worldOpAt = at; InterlockedIncrement(&g_worldOpRun); }
    if (watch && op == 0x25) { g_rec25 = records + (uint64_t)at * 0x19B0u; g_tileContext = b; }
    LARGE_INTEGER start, now, frequency;
    static LONG depth; /* the dispatcher on the generating thread only; a nested op is not profiled on its own */
    /* Off: under Wine, SuspendThread/ResumeThread are wineserver round trips, and suspending the generating thread
     * every millisecond kept it parked (generation stalled). Hot spots are sampled from outside with gdb instead. */
    int profile = 0 && watch && InterlockedIncrement(&depth) == 1;
    if (watch && !profile) InterlockedDecrement(&depth);
    if (profile) WorldProfileBegin(op);
    QueryPerformanceCounter(&start);
    /* Research (terrain tiles): SetupTerrainMerge (0xF48DA0) aims the merge's camera with 0xF487B0 / 0xF48990: an
     * orthographic view straight down from (0, 0, 501) over +-size/2, the size being the world's ([[context+8]+0x2C], an
     * integer: 16384 at 8x). With HELLFORGE_XL_MERGE_SIZE=N (256..16384) an extra large level's merge covers the central
     * N m instead (the size is set for the op and put back after it): at 2048 the heightmap holds the middle 2 km at
     * vanilla's 0.5 m per sample, though the terrain still believes it covers the whole world. */
    uint64_t mergeSettings = 0;
    uint32_t mergeSize = 0;
    int mergeSet = 0;
    if (watch && op == 0x38)
    {
        static int size = -1;
        if (size < 0)
        {
            char text[16] = "";
            size = GetEnvironmentVariableA("HELLFORGE_XL_MERGE_SIZE", text, sizeof text) > 0 ? atoi(text) : 0;
            if (size < 256 || size > 16384) size = 0;
        }
        if (size > 0 && !SafeRead(b + 8, &mergeSettings, 8) && mergeSettings != 0 && !SafeRead(mergeSettings + 0x2C, &mergeSize, 4) &&
            mergeSize == (uint32_t)(2048.0f * g_world.current))
        {
            *(volatile uint32_t*)(uintptr_t)(mergeSettings + 0x2C) = (uint32_t)size;
            mergeSet = 1;
            Note("difficulty: extra large maps: the terrain merge covers the central %d m (research; the world is %u m)", size, mergeSize);
        }
    }
    /* The terrain merge renders in T x T tiles, T = its SetupTerrainMerge pass's +0xA8 (record + 8 + 0xA8; 0xF48E2A copies it to
     * [lg+0x124], lg = G+0x8758A8, on every step). T also sizes the material camera (0xF48990: +-world / 2T) and is the
     * shader global level_generation_subdevisions (0xF4917A). Vanilla's merge has T = 1; an extra large level's records
     * give T = 2, and RenderVoronoiMaterials (op 0x41, per frame 0xF52CC0) then kept only the 1024 m of each tile nearest
     * the world's outer corner (HELLFORGE_TERRAIN_DIAG at 4 km: 5% of the disk painted, all in the corners; at 16 km the
     * corners are outside the disk: 0%). With T forced to 1 for op 0x41 alone, the camera made with T = 2 clipped the
     * map to +-world / 4. With no material mix every scatter record (grass, plants, rocks, trees) fails its material
     * test. So the merge runs with T = 1, as vanilla's does: the record's value is set for each 0x38 step and put back
     * after. HELLFORGE_XL_MATERIAL_TILES=keep leaves it alone (A/B). */
    uintptr_t mergeTiles = 0;
    uint32_t mergeT = 0;
    if (watch && op == 0x38)
    {
        static int keep = -1;
        if (keep < 0) { char t[8] = ""; keep = GetEnvironmentVariableA("HELLFORGE_XL_MATERIAL_TILES", t, sizeof t) > 0 && !_stricmp(t, "keep"); }
        uintptr_t field = (uintptr_t)(records + (uint64_t)at * 0x19B0u + 8u + 0xA8u); /* the op gets record + 8 */
        if (!keep && !SafeRead(field, &mergeT, 4) && mergeT > 1 && mergeT <= 16)
        {
            mergeTiles = field;
            *(volatile uint32_t*)mergeTiles = 1;
            static LONG noted = -1;
            if (noted != g_world.levels) { noted = g_world.levels; Note("difficulty: extra large maps: the terrain merge runs with 1 tile (the level's record gives %u) at %.0fx", mergeT, (double)g_world.current); }
        }
    }
    /* Research (terrain detail): RenderZoneHeightmap (op 0x3D, 0xF4A280) blends each zone's height materials over the
     * Voronoi triangles with texture coordinates position * (1.0 / world size), y flipped as 1.0 - v: the materials span
     * the whole world once, so at 16 km each texel covers 8x the ground it does on a 2 km map. With
     * HELLFORGE_XL_ZONE_REPEAT=1 an extra large level's op 0x3D loads 8.0 (game.dll's constant at 0x23C7524) in place of
     * that 1.0 (the movss at 0xF4A39C), so the coordinates run 0..8 and repeat every 2048 m as on a vanilla map (the flip
     * holds modulo 1); the load is put back after the op. */
    static int zoneRepeat = -1;
    int zoneSet = 0;
    uintptr_t zoneLoad = g_world.game + 0xF4A39Cu + 5u;
    if (watch && op == 0x3D && g_world.current == 8.0f)
    {
        if (zoneRepeat < 0)
        {
            char text[8] = "";
            zoneRepeat = GetEnvironmentVariableA("HELLFORGE_XL_ZONE_REPEAT", text, sizeof text) > 0 && atoi(text) == 1;
        }
        int32_t rel = 0;
        if (zoneRepeat && !SafeRead(zoneLoad, &rel, 4) && zoneLoad + 4 + (intptr_t)rel == g_world.game + 0x23C6D70u)
        {
            int32_t eight = (int32_t)((intptr_t)(g_world.game + 0x23C7524u) - (intptr_t)(zoneLoad + 4));
            DWORD old = 0;
            if (VirtualProtect((void*)zoneLoad, 4, PAGE_EXECUTE_READWRITE, &old))
            {
                *(volatile int32_t*)zoneLoad = eight;
                VirtualProtect((void*)zoneLoad, 4, old, &old);
                FlushInstructionCache(GetCurrentProcess(), (void*)zoneLoad, 4);
                zoneSet = rel;
                static LONG told;
                if (InterlockedIncrement(&told) == 1) Note("difficulty: extra large maps: zone height materials repeat every 2048 m (research)");
            }
        }
    }
    uint32_t vanillaBefore = 0, vanillaOp = 0xFFFFFFFFu, vanillaAt = 0;
    uintptr_t vanillaBlock = 0;
    int vanilla2B = !watch && g_worldLevel != 0 && !SafeRead(index, &vanillaAt, 4) && !SafeRead(records + (uint64_t)vanillaAt * 0x19B0u, &vanillaOp, 4) &&
        vanillaOp != 0xFFFFFFFFu && !SafeRead(g_worldLevel + 0x2C8, &vanillaBlock, 8) && vanillaBlock != 0 && !SafeRead(vanillaBlock + 0x11A7108, &vanillaBefore, 4);
    { uint32_t ia = 0, oa = 0; if (!SafeRead(index, &ia, 4) && !SafeRead(records + (uint64_t)ia * 0x19B0u, &oa, 4) && oa == 0x74) { g_pass74 = records + (uint64_t)ia * 0x19B0u + 8; g_ctx74 = b; } if (oa == 0x76) g_pass76 = records + (uint64_t)ia * 0x19B0u + 8; }
    if (matPatch) TerrainMatPatch(1);
    if (watch && g_world.current == 8.0f && op == 0x3F) TerrainNearPads(records + (uint64_t)at * 0x19B0u, b); /* the level's pads (its first 0x3F) */
    /* the stamp rings (difficulty_world_terrain.c) rerun 0x26 / 0x4E on tiles from their records as they were before */
    if (watch && g_world.current == 8.0f && (op == 0x26 || op == 0x4E)) TerrainStampsRecord(records + (uint64_t)at * 0x19B0u, op);
    if (watch && op == 0x2B) TownsArm(records, at, b, block);
    if (watch && op == 0xB9 && !g_cityShifted) WorldCityShift(block, 0); /* each city's cells into the zone template's square */
    if (watch && op == 0x78) WorldCityOpenZones(records, (uint32_t)d, b, block, 0); /* the open land's zones before 0x78 picks the rest */
    if (watch && op == 0x7C) WorldCityKinds(records, (uint32_t)d, b, block);
    if (watch && op == 0x18) WorldCityDefKinds(b, block);
    if (watch && op == 0x16) WorldNatureResearch16(b);
    if (watch && op == 0x16) { g_kfCount = 0; g_kfMismatch = 0; g_kfOn = 1; }
    if (watch) NatureWindow(b, op, at);
    if (vanilla2B && vanillaOp == 0x2B) TownsForget(records + (uint64_t)vanillaAt * 0x19B0u + 8);
    if (g_world.current > 1.0f && op == 0x66 && phaseBefore66 < 0) { uint32_t u = 0; if (!SafeRead(block + 0x11A7178u, &u, 4)) phaseBefore66 = (int32_t)u; }
    uint32_t sortAt = 0, sortOp = 0xFFFFFFFFu;
    const int sortLock = !SafeRead(index, &sortAt, 4) && !SafeRead(records + (uint64_t)sortAt * 0x19B0u, &sortOp, 4) && OpUsesSortContext(sortOp);
    if (sortLock) AcquireSRWLockExclusive(&g_sortContextLock);
    /* op 0x16's template caps x the missions (ObjCapStub), under the lock the other capMul passes take (after the sort
     * lock, their order) */
    const int objCaps = watch && op == 0x16 && g_world.current > 1.0f && g_difficulty.missions > 1 && g_world.values != NULL;
    if (objCaps) { AcquireSRWLockExclusive(&g_explorationLock); g_world.values->capMul = g_difficulty.missions; }
    uint64_t r = g_worldDispatch(records, b, index, d, e, f, g, h);
    if (objCaps) { g_world.values->capMul = 1; ReleaseSRWLockExclusive(&g_explorationLock); }
    if (sortLock) ReleaseSRWLockExclusive(&g_sortContextLock);
    if (g_kfOn) { g_kfOn = 0; if (g_kfMismatch) Note("difficulty: objectives research: the KindFilter replay disagreed with the game %ld times of %ld", (long)g_kfMismatch, (long)g_kfCount); }
    {
        uint32_t now = 0xFFFFFFFFu, done = watch ? at : vanillaAt, doneOp = watch ? op : vanillaOp;
        if ((watch || vanilla2B) && !SafeRead(index, &now, 4) && now != done) WorldMemoryNote("after", doneOp, done, 0);
    }
    if (matPatch) TerrainMatPatch(0);
    if (watch && op == 0x2B) TownsAfter(records, at, index, b, g, h, block); /* before TerrainStamps: it saves below[] */
    if (g_cityShifted) { uint32_t now = at; if (!SafeRead(index, &now, 4) && now != at) { WorldCityShift(block, 1); WorldCityOpenZones(records, (uint32_t)d, b, block, 1); } }
    if (watch) { uint32_t now = at; if (!SafeRead(index, &now, 4) && now != at) WorldCityStray(records, (uint32_t)d, b, block, op, at); }
    if (watch)
    {
        uint32_t now = at;
        if (!SafeRead(index, &now, 4) && now != at)
        {
            WorldNatureOps(op, at);
            if (op == 0x15) { g_pass15 = records + (uint64_t)at * 0x19B0u + 8; WorldNatureResearch15(records + (uint64_t)at * 0x19B0u + 8, b); }
            if (op == 0x16) { WorldObjectivePlaced(block); WorldObjectiveUnplacedNote(block); }
        }
    }
    if (watch && g_world.current == 8.0f && op == 0x2B)
    {
        uint32_t now = at;
        if (!SafeRead(index, &now, 4) && now != at) TerrainStamps(records, (uint32_t)d, at, b, g, h, block);
    }
    TerrainDiagAfter(records, index, diagAt, diagOp);
    if (mergeSet) *(volatile uint32_t*)(uintptr_t)(mergeSettings + 0x2C) = mergeSize;
    if (mergeTiles != 0) *(volatile uint32_t*)mergeTiles = mergeT;
    if (zoneSet)
    {
        DWORD old = 0;
        if (VirtualProtect((void*)zoneLoad, 4, PAGE_EXECUTE_READWRITE, &old))
        {
            *(volatile int32_t*)zoneLoad = zoneSet;
            VirtualProtect((void*)zoneLoad, 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void*)zoneLoad, 4);
        }
    }
    if (vanilla2B) /* research: a vanilla map's locations per op (the baseline the larger maps' densities compare with) */
    {
        uint32_t vanillaAfter = 0;
        SafeRead(vanillaBlock + 0x11A7108, &vanillaAfter, 4);
        if (vanillaAfter != vanillaBefore) Note("difficulty: extra large maps: op 0x%X (record %u) at %.0fx: locations %u -> %u", vanillaOp, vanillaAt, (double)g_world.current, vanillaBefore, vanillaAfter);
    }
    if (profile)
    {
        LARGE_INTEGER end, hz;
        QueryPerformanceCounter(&end); QueryPerformanceFrequency(&hz);
        WorldProfileEnd(op, (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)hz.QuadPart);
        InterlockedDecrement(&depth);
    }
    {
        uint32_t pat = 0, pop = 0xFFFFFFFFu;
        if (!SafeRead(index, &pat, 4) && !SafeRead(records + (uint64_t)pat * 0x19B0u, &pop, 4) &&
            (pop == 0x38 || pop == 0x3C || pop == 0x40 || pop == 0x3D || pop == 0x5E || pop == 0x6A) && InterlockedIncrement(&g_terrainProbes) <= 40)
            WorldTerrainProbe(records, pat, pop);
    }
    {
        uint32_t pat = 0, pop = 0xFFFFFFFFu;
        if (vanillaOp == 0x96 || op == 0x96)
            if (!SafeRead(index, &pat, 4) && !SafeRead(records + (uint64_t)pat * 0x19B0u, &pop, 4) && pop != 0x96) WorldScatterNote();
    }
    if (!watch) return r;
    CityGatesNote(op);
    if (op == 0xB9) WorldZoneProbe(records, (uint32_t)d, block);
    if (op == 0x07 || op == 0xA4) WorldCellEdges(block, op);
    if (op == 0xAB || op == 0xA7 || op == 0x2B) { uint32_t now = at; if (!SafeRead(index, &now, 4) && now != at) WorldCityBudget(block, op); }
    if (op == 0x2B && g_world.current > 1.0f) { uint32_t now = at; if (!SafeRead(index, &now, 4) && now != at) WorldRoadCheck(block); }
    /* research (s74p: purple strips, unit a441ca86ac27360b, in near-identical pairs beside the roads): the generation's
     * road units (array G+0x8ABD70, count G+0x11A7178, cap 0x2000 in the 0xA7 / 0x8F pass records, 0xC22A29 /
     * 0xC2399D), destroyed by 0x66 phase 0 (0xC17B34) */
    if (g_world.current > 1.0f && (op == 0xA7 || op == 0x8F || op == 0x66))
    {
        uint32_t now = at, units = 0;
        SafeRead(block + 0x11A7178u, &units, 4);
        if (!SafeRead(index, &now, 4) && now != at)
        {
            Note("difficulty: road units research: level %ld after op 0x%02X (record %u): %u generation road units (cap 8192)%s", (long)g_world.levels, op, at, units,
                op == 0x66 ? "" : "");
            if (op == 0x66) { Note("difficulty: road units research: op 0x66 began with %d", phaseBefore66); phaseBefore66 = -1; }
        }
    }
    if (op == 0xA3)
    {
        uint32_t now = at;
        if (!SafeRead(index, &now, 4) && now != at) WorldRepaintCorners(records, at, b, g, h, block);
    }
    if (op == 0x2B)
    {
        OverlapFlush();
        /* The locations without a resource (+0x90 of a 0xE0 record; GenerateResourceHash, 0xC176B8, reads it unchecked),
         * at the address the hash pass uses (moved for larger maps: add rbx, A + 0x90 at 0xC176A7). */
        uint32_t offset = 0, total = 0, empty = 0, firstEmpty = 0xFFFFFFFFu;
        if (!SafeRead(g_world.game + 0xC176A7 + 3, &offset, 4) && !SafeRead(block + 0x11A7108, &total, 4))
            for (uint32_t i = 0; i < total && i < 65536; i++)
            {
                uint64_t resource = 1;
                if (SafeRead(block + offset + (uint64_t)i * 0xE0, &resource, 8)) break;
                if (resource == 0) { empty++; if (firstEmpty == 0xFFFFFFFFu) firstEmpty = i; }
            }
        Note("difficulty: extra large maps: after op 0x2B: %u locations, %u without a resource (first %d)", total, empty, (int)firstEmpty);
    }
    if (op == 0x6C && g_heightCopy != NULL && g_heightSource != 0) /* streaming research: keep the heights the layers saw last */
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)(uintptr_t)g_heightSource, &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT &&
            (uintptr_t)mbi.BaseAddress + mbi.RegionSize >= g_heightSource + g_heightBytes)
            memcpy(g_heightCopy, (const void*)(uintptr_t)g_heightSource, g_heightBytes);
    }
    if (op == 0x31 && InterlockedIncrement(&g_worldStampNotes) <= 2) /* streaming research: SpawnStamps' pass and context */
    {
        uint64_t world = 0;
        SafeRead(b + 0x40, &world, 8);
        Note("difficulty: extra large maps: SpawnStamps pass 0x%llX context 0x%llX world 0x%llX", (unsigned long long)(records + (uint64_t)at * 0x19B0u + 8),
            (unsigned long long)b, (unsigned long long)world);
    }
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    double ms = (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)frequency.QuadPart;
    SafeRead(block + 0x11A7108, &after, 4);
    if (counted && after > before)
        for (uint32_t i = before; i < after && i < DIF_LOCATIONS; i++) g_locationOp[i] = (uint8_t)op;
    if (counted && op == 0x2B) WorldLocationsNote(block, after);
    uint32_t route[3] = {0, 0, 0}; /* vertices of 2304, cells of 1024, edges of 3456 (difficulty_block.c moves them) */
    SafeRead(block + 0x11A71B4, &route[0], 4); SafeRead(block + 0x11A71C0, &route[1], 4); SafeRead(block + 0x11A71CC, &route[2], 4);
    if (after != before || ms > 100.0 || route[0] > 2304 || route[1] > 1024 || route[2] > 3456 || op == 0x3 || op == 0xB3 ||
        op == 0x07 || op == 0x12 || op == 0x75 || op == 0x79 || op == 0xB9 || op == 0xA3 || op == 0xA4 || op == 0x78)
        Note("difficulty: extra large maps: op 0x%X (record %u) at %.0fx: locations %u -> %u, route vertices %u, cells %u, edges %u, %.1f ms",
            op, at, (double)g_world.current, before, after, route[0], route[1], route[2], ms);
    if (op == 0x7C && g_paintedPeak != NULL)
    {
        Note("difficulty: extra large maps: op 0x7C: painted route-cell list peak %u (capacity cells %u + locations %u; the game's was %u)",
            *g_paintedPeak, route[1], after, route[1]);
        *g_paintedPeak = 0;
    }
    return r;
}
static const struct { uint32_t rva; void* detour; WorldPassFn* original; } kWorldPasses[] = {
    {0xF767A0u, (void*)WorldPassLocations, &g_passLocations}, {0xF7D780u, (void*)WorldPassDiscovery, &g_passDiscovery},
    {0xF7E250u, (void*)WorldPassOutposts, &g_passOutposts}, {0xF83EA0u, (void*)WorldPassValidSmall, &g_passValidSmall},
    {0xF84380u, (void*)WorldPassSmall, &g_passSmall}, {0xF77F30u, (void*)WorldPassBorder, &g_passBorder},
    {0xF82A80u, (void*)WorldPassCrossing, &g_passCrossing}, {0xF7C310u, (void*)WorldPassSpread, &g_passSpread},
    {0xF86DC0u, (void*)WorldPassSatelliteCaps, &g_passSatellite}, {0xF40F80u, (void*)WorldPassScatter, &g_passScatter},
    {0xF82710u, (void*)WorldPassEdge, &g_passEdge}, {0xF95900u, (void*)WorldPassExploration, &g_passExplorationTimed},
    {0xF3F8E0u, (void*)WorldPassVoronoi, &g_passVoronoi}, {0xF79520u, (void*)WorldPassClusters, &g_passClustersTimed}};

/* Streaming research: who spawns units into which world. Three engine spawners (rcx the world each): exe 0x338840 and
 * 0x338720 (the world interface's +0x380 and +0x378) and 0x1B4380 (no direct callers). Each (spawner, world, caller) is
 * logged at its 1st, 100th, 1000th and 10000th spawn. */
typedef uint64_t (*WorldSpawnFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static const struct { uint32_t rva; const char* pin; } kSpawners[4] = {
    {0x338840u, "4C 8B DC 53 55 56 57 41 56 48 83 EC 50 48 B8 C5 9A FE 48 D3 0F 69 2A 49 89 53 08"},
    {0x338720u, "4C 8B DC 53 55 56 57 41 56 48 83 EC 50 48 B8 C5 9A FE 48 D3 0F 69 2A"},
    {0x1B4380u, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 50 48 8B 81 58 00 23 00"},
    {0x1B4470u, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 50 48 8B 81 58 00 23 00 49 8B F9 49 8B F0 48 8B EA"}};
static WorldSpawnFn g_worldSpawn[4];
static struct { uint64_t world, caller; uint32_t which; volatile LONG count; } g_spawnSeen[128];
static void WorldSpawnSeen(uint32_t which, uint64_t world, uint64_t caller)
{
    for (size_t i = 0; i < sizeof g_spawnSeen / sizeof g_spawnSeen[0]; i++)
    {
        if (g_spawnSeen[i].world == 0 && InterlockedCompareExchange64((volatile LONG64*)&g_spawnSeen[i].world, (LONG64)world, 0) == 0)
        { g_spawnSeen[i].caller = caller; g_spawnSeen[i].which = which; }
        if (g_spawnSeen[i].world != world || g_spawnSeen[i].caller != caller || g_spawnSeen[i].which != which) continue;
        LONG n = InterlockedIncrement(&g_spawnSeen[i].count);
        if (n == 1 || n == 100 || n == 1000 || n == 10000)
        {
            uintptr_t game = g_world.game, exe = ApiModuleBase("helldivers2.exe", NULL);
            int inGame = caller >= game && caller < game + 0x4000000u;
            Note("difficulty: extra large maps: spawner exe+0x%X #%ld into world 0x%llX from %s+0x%llX", kSpawners[which].rva, (long)n,
                (unsigned long long)world, inGame ? "game" : "exe", (unsigned long long)(caller - (inGame ? game : exe)));
        }
        return;
    }
}
#define WORLD_SPAWNER(i) \
    static uint64_t WorldSpawn##i(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) \
    { WorldSpawnSeen(i, a, (uint64_t)__builtin_return_address(0)); return g_worldSpawn[i](a, b, c, d, e, f, g, h); }
WORLD_SPAWNER(0)
WORLD_SPAWNER(1)
WORLD_SPAWNER(2)
WORLD_SPAWNER(3)
static void* const kSpawnDetours[4] = {(void*)WorldSpawn0, (void*)WorldSpawn1, (void*)WorldSpawn2, (void*)WorldSpawn3};

/* Research: op 0x2B's eight jobs (0xF322F0, r9 the job: +0x10 the pass context, whose [+0x170] holds the item count;
 * +0x4C/+0x50 its range of the shuffled items, +0x58 its done flag; 0xF34EE0..0xF34F6A) timed one by one, to see how
 * evenly the work divides. */
static const char kPlaceJobPin[] = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D AC 24 70 F3 FF FF";
typedef uint64_t (*WorldJobFn)(uint64_t, uint64_t, uint64_t, uint64_t);
static WorldJobFn g_placeJob;
static volatile LONG g_stampTileRun; /* TerrainStamps' tile op 0x2B is running: no per-job line (load time; the planning note has the total) */
static uint64_t WorldPlaceJob(uint64_t a, uint64_t b, uint64_t c, uint64_t job)
{
    if (job == 0) return g_placeJob(a, b, c, job); /* every level: a vanilla map gives the numbers to compare with */
    uint32_t range[2] = {0, 0}, items = 0;
    uint64_t context = 0, list = 0;
    SafeRead(job + 0x4C, range, 8);
    if (!SafeRead(job + 0x10, &context, 8) && context != 0 && !SafeRead(context + 0x170, &list, 8) && list != 0) SafeRead(list, &items, 4);
    /* Each job writes its instances at slice + count x 0x130 with no bound (the capacity at +0x38 only sizes the clear
     * at the job's start), and the slices lie end to end in one buffer: a job that placed more than its 24576 ran over
     * the next job's slice, which then read records without a resource (0xF31027) or left locations without one
     * (GenerateResourceHash, 0xC176B8). On a larger map each job gets its own region instead (job+0x20; the merge at
     * 0xF35030 reads every job's records through it, count +0x54): 128 MB, about 440k records, committed by the system
     * only where written, followed by a reserved page so a job past even that faults in place. Jobs split the items in 8
     * equal ranges, so the region is picked by the range's start. */
    if (g_world.current > 1.0f && items != 0)
    {
        static uint8_t* regions[8];
        uint32_t slot = (uint32_t)((uint64_t)range[0] * 8u / items);
        if (slot > 7) slot = 7;
        if (regions[slot] == NULL)
        {
            uint8_t* region = (uint8_t*)VirtualAlloc(NULL, (128u << 20) + 0x10000u, MEM_RESERVE, PAGE_NOACCESS);
            if (region != NULL && VirtualAlloc(region, 128u << 20, MEM_COMMIT, PAGE_READWRITE) != NULL) regions[slot] = region;
        }
        if (regions[slot] != NULL) *(volatile uint64_t*)(uintptr_t)(job + 0x20) = (uint64_t)(uintptr_t)regions[slot];
    }
    LARGE_INTEGER start, end, hz;
    QueryPerformanceCounter(&start);
    uint64_t r = g_placeJob(a, b, c, job);
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&hz);
    if (g_stampTileRun) return r;
    /* The job's slice (+0x20, capacity +0x38 records of 0x130, cleared at the job's start, 0xF3253B): records written
     * (a box or a resource) and those of them without a resource (+0x90), which become locations without one. */
    uint64_t slice = 0;
    uint32_t capacity = 0, written = 0, empty = 0, lastWritten = 0, count = 0;
    SafeRead(job + 0x54, &count, 4);
    if (!SafeRead(job + 0x20, &slice, 8) && !SafeRead(job + 0x38, &capacity, 4) && slice != 0 && capacity <= 0x100000)
        for (uint32_t i = 0; i < count && i < 0x6B000u; i++) /* the job's records (count +0x54); the rest of the slice is its clear */
        {
            uint8_t box[16];
            uint64_t resource = 0;
            if (SafeRead(slice + 8 + (uint64_t)i * 0x130 + 0x90, &resource, 8) || SafeRead(slice + 8 + (uint64_t)i * 0x130 + 0xB0, box, 16)) break;
            int any = resource != 0;
            for (int k = 0; k < 16 && !any; k++) any = box[k] != 0;
            if (!any) continue;
            written++;
            lastWritten = i;
            if (resource == 0) empty++;
        }
    Note("difficulty: extra large maps: op 0x2B job items %u..%u of %u on thread %lu at %.0fx: %.1f ms; %u records (count %u, last %u; the original slice held %u), %u without a resource",
        range[0], range[1], items, (unsigned long)GetCurrentThreadId(), (double)g_world.current, (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)hz.QuadPart,
        written, count, lastWritten, capacity, empty);
    return r;
}

/* Streaming research: the heightfield level layers snap to (exe 0xDFA60, r9: data at +0, origin +8/+0xC, extent
 * +0x14/+0x18, columns +0x1C, rows +0x20; a stack copy of the generator's, whose data is freed after generation). The
 * loader keeps a copy of the data from generation and lends it when a layer is spawned after the drop. */
static const char kLayerSnapPin[] = "48 8B C4 89 50 10 55 53 56 41 55 48 8D A8 78 F6 FF FF";
static WorldSpawnFn g_layerSnap;
static float* g_heightCopy;
static uint64_t g_heightSource, g_heightBytes;
/* g_heightField (above WorldDispatch): the generator's descriptor, its data pointer replaced by the copy */
static volatile LONG g_heightLent;
static uint64_t NearLayerField(uint64_t unit, uint64_t field); /* difficulty_world_terrain.c: the near tile's grid */
static int NearLayerHeight(uint64_t desc, float x, float y, float* height);
static uint64_t WorldLayerSnap(uint64_t a, uint64_t b, uint64_t c, uint64_t field, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    uint8_t now[0x28];
    if (field != 0 && !SafeRead(field, now, sizeof now))
    {
        uint64_t data; uint32_t dims[2];
        memcpy(&data, now, 8); memcpy(dims, now + 0x1C, 8);
        uint64_t bytes = (uint64_t)dims[0] * dims[1] * 4u;
        if (data != 0 && dims[0] != 0 && dims[0] <= 16384 && dims[1] <= 16384 && data != g_heightSource && data != (uint64_t)(uintptr_t)g_heightCopy)
        {
            if (g_heightCopy != NULL && g_heightBytes != bytes) { VirtualFree(g_heightCopy, 0, MEM_RELEASE); g_heightCopy = NULL; }
            if (g_heightCopy == NULL) g_heightCopy = (float*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (g_heightCopy != NULL)
            {
                memcpy(g_heightCopy, (const void*)(uintptr_t)data, bytes);
                g_heightSource = data; g_heightBytes = bytes;
                memcpy(g_heightField, now, sizeof now);
                uint64_t copy = (uint64_t)(uintptr_t)g_heightCopy; memcpy(g_heightField, &copy, 8);
                Note("difficulty: extra large maps: kept the layer heightfield (%u x %u, %llu MB) at 0x%llX", dims[0], dims[1], (unsigned long long)(bytes >> 20),
                    (unsigned long long)(uintptr_t)g_heightCopy);
            }
        }
        else if (data == 0 && g_heightCopy != NULL)
        {
            memcpy((void*)(uintptr_t)field, g_heightField, sizeof g_heightField); /* the caller's stack copy */
            if (InterlockedIncrement(&g_heightLent) <= 3) Note("difficulty: extra large maps: lent the kept heightfield to a layer spawn");
        }
    }
    uint64_t tile = NearLayerField(a, field); /* a stamp on or near the near tile: its 1 m grid (a = the stamp's level unit) */
    return g_layerSnap(a, b, c, tile != 0 ? tile : field, e, f, g, h);
}

/* The engine's other height sampler (exe 0xD5B10: rcx the address of the descriptor pointer, x/y in xmm1/xmm2; it reads
 * [desc] without a check) is reached before 0xDFA60 by some layers: an empty descriptor there gets the kept copy too. */
static const char kLayerSamplePin[] = "4C 8B 19 0F 28 DA 0F 57 C0 4D 85 DB 74 79";
typedef float (*WorldSampleFn)(uint64_t* holder, float x, float y);
static WorldSampleFn g_layerSample;
static float WorldLayerSample(uint64_t* holder, float x, float y)
{
    uint64_t desc = *holder;
    if (desc != 0 && *(volatile uint64_t*)(uintptr_t)desc == 0 && g_heightCopy != NULL)
    {
        memcpy((void*)(uintptr_t)desc, g_heightField, sizeof g_heightField); /* a stack copy, like WorldLayerSnap's */
        if (InterlockedIncrement(&g_heightLent) <= 3) Note("difficulty: extra large maps: lent the kept heightfield to a layer sample");
    }
    float tile;
    if (NearLayerHeight(*holder, x, y, &tile)) return tile; /* on the near tile: its own height */
    return g_layerSample(holder, x, y);
}

/* An engine call through the world interface ([game+0x3326308]+0x168, method 0; exe 0x32F6A0: rcx the world, edx an
 * object handle) looks the handle up (exe 0x9D8C0: 22-bit index, generation in the top byte) and calls through the
 * result unchecked. A level's object list (lg+0x2C60, 0x44 each, a u16 at +0) feeds it from game.dll 0x5D23A8, whose
 * caller already handles 0; on 4096 and 8192 m missions a handle there was stale and the game ended at exe 0x32F6CE as the
 * mission started (not every run; not tied to the object counts). With an extra large world a stale handle returns 0
 * and is counted (the first few logged). */
#define DIF_OBJECT_CALL 0x32F6A0u
#define DIF_OBJECT_LOOKUP 0x9D8C0u
static const char kObjectCallPin[] = "44 89 44 24 18 4C 89 4C 24 20 55 53 56 57 41 56 41 57 48 8D AC 24 48 F8";
static const char kObjectLookupPin[] = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20";
typedef uint64_t (*WorldObjectFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef void* (*WorldLookupFn)(uint32_t handle);
static WorldObjectFn g_worldObject;
static WorldLookupFn g_worldLookup;
static volatile LONG g_worldStale;
static uint64_t WorldObject(uint64_t world, uint64_t handle, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    if (g_worldLookup((uint32_t)handle) == NULL)
    {
        LONG n = InterlockedIncrement(&g_worldStale);
        if (n <= 8) Note("difficulty: extra large maps: a stale object handle 0x%X (index %u, generation %u) skipped (%ld so far)",
            (unsigned)handle, (unsigned)(handle & 0x3FFFFFu), (unsigned)((handle >> 22) & 0x3FFu), (long)n);
        return 0;
    }
    return g_worldObject(world, handle, c, d, e, f, g, h);
}

/* Level init, before it stores the level's size: the world for this level (level 11's scale, else vanilla). */
static void WorldLevel(uintptr_t level)
{
    NearOneMipDrop(); /* the previous extra large level's one-mip heightmap is not this level's */
    uint8_t difficulty = 0;
    float scale = 1.0f;
    if (g_world.scale > 1 && !SafeRead(level + 0xF9, &difficulty, 1) && difficulty == DIF_VANILLA + 1) scale = (float)g_world.scale;
    g_world.values->size = scale > 1.0f ? 2048.0f * scale : 0.0f;
    if (scale != g_world.current) WorldWrite(scale);
    g_worldLevel = level;
    if (g_world.below != NULL) g_world.below[3] = 0; /* op 0x2B's instances refused at the table's limit, this level */
    g_worldOpAt = 0xFFFFFFFFu;
    InterlockedIncrement(&g_worldOpRun);
    WorldIndex(level, 2048.0f * scale);
    LONG full = g_worldIndexFull != NULL ? *g_worldIndexFull : 0;
    if (full != g_worldIndexFullSeen)
    {
        Note("difficulty: extra large maps: %ld stamps found the stamp index full so far (placed without an overlap entry)", (long)full);
        g_worldIndexFullSeen = full;
    }
    InterlockedIncrement(&g_world.levels);
}

/* The ship's navigation setup: the world back to vanilla (the ship's own systems read the same constants). */
static void WorldShip(void)
{
    FpsLiftEnd("the ship");
    NearOneMipDrop();
    if (g_world.values != NULL && g_world.current != 1.0f) WorldWrite(1.0f);
}

/* HELLFORGE_XL_CENSUS=1: the install (and so its checks) on a level the loader does not grow too (InstallWorld). */
static int WorldCensusOn(void)
{
    char v[8] = "";
    return GetEnvironmentVariableA("HELLFORGE_XL_CENSUS", v, sizeof v) > 0 && v[0] == '1';
}

static const char* WorldSitesFailed(uintptr_t game)
{
    if (WorldScaleFor(g_difficulty.mapScale) <= 1 && !WorldCensusOn()) return NULL;
    uint8_t pin[9];
    if (SafeRead(game + DIF_WORLD_SIZE_SITE, pin, 9) || memcmp(pin, kWorldSizePin, 9) != 0)
        return "the level's size store is not the reviewed one (an extra large map needs it)";
    if (!ApiCodeMatches(game + DIF_INDEX_SIZE, kIndexSizePin) || !ApiCodeMatches(game + DIF_INDEX_INIT, kIndexInitPin))
        return "the map generator's stamp index is not the reviewed one (an extra large map needs it)";
    uintptr_t exe = ApiModuleBase("helldivers2.exe", NULL);
    uint8_t cull[12];
    if (exe == 0 || SafeRead(exe + DIF_CULL_SITE, cull, 12) || memcmp(cull, kCullPin, 12) != 0)
        return "the engine's occlusion test is not the reviewed one (an extra large map needs it)";
    int32_t clearingDisp;
    if (SafeRead(game + DIF_CLEARING_RANDOM + 4, &clearingDisp, 4) || game + DIF_CLEARING_RANDOM + 8 + (intptr_t)clearingDisp != game + DIF_CLEARING_CONSTANT)
        return "the clearing points' placement is not the reviewed one (an extra large map needs it)";
    for (int i = 0; i < 3; i++)
    {
        uint8_t bytes[7];
        if (SafeRead(game + kPlaceBuffer[i].rva, bytes, kPlaceBuffer[i].length) || memcmp(bytes, kPlaceBuffer[i].pin, kPlaceBuffer[i].length) != 0)
            return "the stamp instances' buffer is not the reviewed one (an extra large map needs it)";
    }
    uint8_t hive[49];
    if (SafeRead(game + DIF_HIVE_SEARCH, hive, 49) || memcmp(hive, kHiveSearchPin, 49) != 0)
        return "the Hive Lord graph's labelling is not the reviewed one (an extra large map needs it)";
    uint8_t table2[10];
    if (SafeRead(game + DIF_TABLE2_FULL, table2, 10) || memcmp(table2, kTable2FullPin, 10) != 0)
        return "the entity table's insert is not the reviewed one (an extra large map needs it)";
    uint8_t tableFull[7];
    if (SafeRead(game + DIF_TABLE_FULL, tableFull, 7) || memcmp(tableFull, kTableFullPin, 7) != 0)
        return "the global hash table's insert is not the reviewed one (an extra large map needs it)";
    uint8_t walk[8];
    if (SafeRead(game + DIF_OBJECT_WALK, walk, 8) || memcmp(walk, kObjectWalkPin, 8) != 0)
        return "the level object walk is not the reviewed one (an extra large map needs it)";
    uint8_t make[10];
    if (SafeRead(game + DIF_INSTANCE_MAKE, make, 10) || memcmp(make, kInstanceMakePin, 10) != 0)
        return "the stamp instances' creation is not the reviewed one (an extra large map needs it)";
    uint8_t place[10];
    if (SafeRead(game + DIF_PLACE_LOOP, place, 10) || memcmp(place, kPlaceLoopPin, 10) != 0)
        return "the stamp placement is not the reviewed one (an extra large map needs it)";
    if (!ApiCodeMatches(game + DIF_SPECIAL_LOOP, kSpecialLoopPin))
        return "op 0x76's cluster loop is not the reviewed one (an extra large map needs it)";
    uint8_t records[10];
    if (SafeRead(game + DIF_RECORDS_FULL, records, 10) || memcmp(records, kRecordsFullPin, 10) != 0)
        return "the world's destruction records are not the reviewed ones (an extra large map needs them)";
    if (SafeRead(game + DIF_INDEX_FULL, pin, 8) || memcmp(pin, kIndexFullPin, 8) != 0)
        return "the map generator's stamp index is not the reviewed one (an extra large map needs it)";
    for (size_t i = 0; i < sizeof kWorldSites / sizeof kWorldSites[0]; i++)
    {
        const WorldSite* s = &kWorldSites[i];
        int32_t disp;
        if (SafeRead(game + s->rva + s->length - 4, &disp, 4) || game + s->rva + s->length + (intptr_t)disp != game + kWorldConstants[s->which])
            return "a reader of the world's size is not the reviewed one (an extra large map needs it)";
    }
    for (int i = 0; i < 2; i++)
    {
        int32_t disp;
        if (SafeRead(game + kWorldGate[i].rva + 4, &disp, 4) || game + kWorldGate[i].rva + 8 + (intptr_t)disp != game + kWorldGate[i].constant)
            return "the director's spawn bounds are not the reviewed ones (an extra large map needs them)";
    }
    for (int i = 0; i < 4; i++)
    {
        int32_t disp;
        uint8_t op[4];
        if (SafeRead(game + kWorldDropBounds[i].rva, op, 4) || op[0] != 0xF3 || op[1] != 0x0F || op[2] != 0x10 ||
            SafeRead(game + kWorldDropBounds[i].rva + 4, &disp, 4) || game + kWorldDropBounds[i].rva + 8 + (intptr_t)disp != game + kWorldDropBounds[i].constant)
            return "the drop bounds are not the reviewed ones (an extra large map needs them)";
    }
    for (int i = 0; i < WORLD_IMMEDIATES; i++)
    {
        uint32_t value;
        if (SafeRead(game + kWorldImmediates[i].rva + kWorldImmediates[i].at, &value, 4) || value != kWorldImmediates[i].value)
            return "a world size the generator holds is not the reviewed one (an extra large map needs it)";
    }
    return NULL;
}

/* The convoy manager ([game+0x3326B70], 2 records of 0x318 at +0xA0) spawns a convoy's escort squads in 0x868840: a
 * loop of difficulty row +0x328 (5 at every level) spawns (0x953580, job 5) and appends a 0x40-byte entry each to the
 * record's list at +0x108 with its count at +0x308, 8 places, unchecked (0x869492 the only increment). 0x869BF0 calls it
 * every frame until the escorts are placed on the leader's road route (record +0x310), and the list is not reset in
 * between, so a route that does not come at once spawns 5 more a frame: the 9th entry overwrote the count and the next
 * store went wild (run s73b, game+0x869466). On an extra large world the route came back empty because the route request
 * (0x6292A0) drops a destination outside +-2048 (0x62932E, 0x629350: now +-the world's full size, world_sites.h); a
 * deferred or failed path does the same in vanilla. The loop's head (0x8691A3, rdi = the record) leaves the loop when the
 * list holds 8 already, so no squad is spawned without its place; the times are counted at stub +0x40. */
#define DIF_CONVOY_LOOP 0x8691A3u
#define DIF_CONVOY_LOOP_BACK 0x8691AAu
#define DIF_CONVOY_LOOP_EXIT 0x8695A3u
#define DIF_CONVOY_DIRECTOR 0x3326340u
static const uint8_t kEscortLoopPin[7] = {0x48, 0x8B, 0x05, 0x96, 0xD1, 0xAB, 0x02}; /* mov rax, [rip -> game+0x3326340] */
static const uint8_t kEscortStub[33] = {
    0x83, 0xBF, 0x08, 0x03, 0x00, 0x00, 0x08,                         /*  0 cmp dword [rdi+0x308], 8 */
    0x72, 0x0C,                                                       /*  7 jb 21 */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /*  9 lock inc dword [rip -> stub+0x40] (rel32 at 12) */
    0xE9, 0, 0, 0, 0,                                                 /* 16 jmp 0x8695A3 (the loop's end; rel32 at 17) */
    0x48, 0x8B, 0x05, 0, 0, 0, 0,                                     /* 21 mov rax, [rip -> game+0x3326340] (rel32 at 24) */
    0xE9, 0, 0, 0, 0};                                                /* 28 jmp 0x8691AA (rel32 at 29) */
static volatile LONG* g_convoyFull;
static const char* InstallConvoyGuard(uintptr_t game)
{
    uint8_t pin[7];
    if (SafeRead(game + DIF_CONVOY_LOOP, pin, 7) || memcmp(pin, kEscortLoopPin, 7) != 0 ||
        !ApiCodeMatches(game + DIF_CONVOY_LOOP_EXIT, "8B 7C 24 50 4C 8B 74 24 60") || !ApiCodeMatches(game + 0x869492u, "FF 87 08 03 00 00") ||
        !ApiCodeMatches(game + 0x86959Eu, "E9 00 FC FF FF"))
        return "the convoy escorts' loop is not the reviewed one; its list stays unchecked";
    uint8_t* stub = AllocateNearGame(game, 0x80);
    if (stub == NULL) return "no memory within 2 GB of game.dll for the convoy escorts' guard";
    memcpy(stub, kEscortStub, sizeof kEscortStub);
    g_convoyFull = (volatile LONG*)(stub + 0x40);
    int32_t rel = (int32_t)((stub + 0x40) - (stub + 16)); memcpy(stub + 12, &rel, 4);
    rel = (int32_t)(game + DIF_CONVOY_LOOP_EXIT - (uintptr_t)(stub + 21)); memcpy(stub + 17, &rel, 4);
    rel = (int32_t)(game + DIF_CONVOY_DIRECTOR - (uintptr_t)(stub + 28)); memcpy(stub + 24, &rel, 4);
    rel = (int32_t)(game + DIF_CONVOY_LOOP_BACK - (uintptr_t)(stub + 33)); memcpy(stub + 29, &rel, 4);
    DWORD protect;
    VirtualProtect(stub, 0x80, PAGE_EXECUTE_READWRITE, &protect); /* the count at +0x40 is written */
    FlushInstructionCache(GetCurrentProcess(), stub, 0x80);
    uint8_t jump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    rel = DifficultyJumpRel(game + DIF_CONVOY_LOOP, (uintptr_t)stub); memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + DIF_CONVOY_LOOP, jump, 7)) return "the convoy escorts' guard could not be written";
    return NULL;
}

/* The mission's per-name lists: at mission load 0xACF710 hands every level unit to 0xAD2010, which compares its name
 * and appends it to a list in the HiveMind manager (world+0x1FA08; a few in other managers), each with its count right
 * after its last entry and no check (only the director's spawners are guarded, difficulty_missions.c). Three missions'
 * content spawned with the level overran the mine list (run s73d, game+0xAD36C0) and can overrun the others (static
 * survey, scratch ml/ml.md: 2026-11-04). Each append starts with a 6-byte "mov eax, [reg + count]" (only eax written, no
 * flags read before the next flag write): it jumps to a stub that copies it, compares the count with the list's places
 * and, when full, counts the unit and continues where the walk goes after the append (0xAD3DBD; the invasion sites
 * 0xAD390B and the alarms 0xAD394C as the vanilla code does; the guard points still make the call 0xAD3670 does after
 * theirs). The readers either take any count or rely on these capacities (the cache loot, shell and collect-resource
 * lists' 256/320/128-entry stack copies), so a full list is what keeps them safe. */
typedef struct { const char* name; uint32_t pin, places, skip; uint8_t bytes[6]; } MissionList;
static const MissionList kMissionLists[] = {
    {"exploration reward", 0xAD272Au, 64, 0xAD3DBDu, {0x8B, 0x83, 0xD0, 0xD8, 0x07, 0x00}},
    {"cache loot marker", 0xAD279Bu, 256, 0xAD3DBDu, {0x8B, 0x83, 0xCC, 0xC4, 0x07, 0x00}},
    {"sample", 0xAD2B96u, 4096, 0xAD3DBDu, {0x8B, 0x83, 0x48, 0xD7, 0x05, 0x00}},
    {"intel", 0xAD2C7Au, 128, 0xAD3DBDu, {0x8B, 0x83, 0x50, 0x03, 0x06, 0x00}},
    {"pickup", 0xAD2DACu, 512, 0xAD3DBDu, {0x8B, 0x83, 0x58, 0xA3, 0x06, 0x00}},
    {"weapon spawn", 0xAD30D4u, 196, 0xAD3DBDu, {0x8B, 0x83, 0xC0, 0x66, 0x07, 0x00}},
    {"weapon spawn (flag)", 0xAD3085u, 64, 0xAD3DBDu, {0x8B, 0x83, 0xC8, 0x7C, 0x07, 0x00}},
    {"hellpod spawn (flag)", 0xAD3319u, 32, 0xAD3DBDu, {0x8B, 0x83, 0x40, 0x27, 0x08, 0x00}},
    {"hellpod spawn", 0xAD3352u, 196, 0xAD3DBDu, {0x8B, 0x83, 0x38, 0x1C, 0x08, 0x00}},
    {"vehicle spawn (flag)", 0xAD34DCu, 4, 0xAD3DBDu, {0x8B, 0x83, 0x90, 0x2D, 0x08, 0x00}},
    {"vehicle spawn", 0xAD351Cu, 16, 0xAD3DBDu, {0x8B, 0x83, 0x48, 0x2C, 0x08, 0x00}},
    {"enemy guard point", 0xAD364Cu, 128, 0, {0x8B, 0x86, 0x00, 0x06, 0x00, 0x00}}, /* skip: the call after it */
    {"mine", 0xAD36A3u, 512, 0xAD3DBDu, {0x8B, 0x83, 0x5C, 0x23, 0x07, 0x00}},
    {"invasion site", 0xAD3888u, 64, 0xAD390Bu, {0x8B, 0x83, 0xA0, 0x6F, 0x08, 0x00}},
    {"alarm effect", 0xAD3929u, 128, 0xAD394Cu, {0x8B, 0x83, 0xA8, 0x77, 0x08, 0x00}},
    {"outpost patrol spawner", 0xAD3981u, 64, 0xAD3DBDu, {0x8B, 0x83, 0x30, 0x07, 0x00, 0x00}},
    {"enemy encounter", 0xAD39BCu, 128, 0xAD3DBDu, {0x8B, 0x83, 0xAC, 0x97, 0x08, 0x00}},
    {"tutorial dropship (flag)", 0xAD3AD1u, 16, 0xAD3DBDu, {0x8B, 0x81, 0x70, 0x02, 0x00, 0x00}},
    {"tutorial dropship", 0xAD3AF8u, 8, 0xAD3DBDu, {0x8B, 0x81, 0x4C, 0x01, 0x00, 0x00}},
    {"objective shell", 0xAD3B5Fu, 128, 0xAD3DBDu, {0x8B, 0x83, 0x38, 0x2F, 0x00, 0x00}},
    {"objective collect resource", 0xAD3C01u, 128, 0xAD3DBDu, {0x8B, 0x83, 0x40, 0x57, 0x00, 0x00}},
};
#define MISSION_LISTS (sizeof kMissionLists / sizeof kMissionLists[0])
#define MISSION_LIST_STUB 0x40u
static volatile LONG* g_missionListFull; /* per list: units left out */
static const char* InstallMissionListGuards(uintptr_t game)
{
    for (size_t i = 0; i < MISSION_LISTS; i++)
    {
        uint8_t pin[6];
        if (SafeRead(game + kMissionLists[i].pin, pin, 6) || memcmp(pin, kMissionLists[i].bytes, 6) != 0)
            return "the mission's per-name lists are not the reviewed ones; they stay unchecked";
    }
    if (!ApiCodeMatches(game + 0xAD3652u, "49 8B CD") || !ApiCodeMatches(game + 0xAD3670u, "48 8B 50 50 FF 52 10 E9"))
        return "the mission's guard point list is not the reviewed one; the lists stay unchecked";
    uint8_t* block = AllocateNearGame(game, MISSION_LISTS * MISSION_LIST_STUB + 0x100);
    if (block == NULL) return "no memory within 2 GB of game.dll for the mission lists' guards";
    g_missionListFull = (volatile LONG*)(block + MISSION_LISTS * MISSION_LIST_STUB);
    for (size_t i = 0; i < MISSION_LISTS; i++)
    {
        const MissionList* l = &kMissionLists[i];
        uint8_t* s = block + i * MISSION_LIST_STUB;
        int n = 0;
        int32_t rel;
        memcpy(s, l->bytes, 6); n = 6;                                        /* mov eax, [reg + count] */
        s[n++] = 0x3D; memcpy(s + n, &l->places, 4); n += 4;                  /* cmp eax, places */
        s[n++] = 0x73; s[n++] = 0x05;                                         /* jae full */
        s[n++] = 0xE9; rel = (int32_t)(game + l->pin + 6 - (uintptr_t)(s + n + 4)); memcpy(s + n, &rel, 4); n += 4; /* jmp back */
        s[n++] = 0xF0; s[n++] = 0xFF; s[n++] = 0x05;                          /* full: lock inc [counter i] */
        rel = (int32_t)((uint8_t*)(g_missionListFull + i) - (s + n + 4)); memcpy(s + n, &rel, 4); n += 4;
        if (l->skip != 0)
        {
            s[n++] = 0xE9; rel = (int32_t)(game + l->skip - (uintptr_t)(s + n + 4)); memcpy(s + n, &rel, 4); n += 4; /* jmp skip */
        }
        else
        {
            s[n++] = 0x49; s[n++] = 0x8B; s[n++] = 0xCD;                      /* mov rcx, r13 (as the game encodes it) */
            s[n++] = 0x48; s[n++] = 0x8B; s[n++] = 0x05;                      /* mov rax, [game+0x3326308] */
            rel = (int32_t)(game + 0x3326308u - (uintptr_t)(s + n + 4)); memcpy(s + n, &rel, 4); n += 4;
            s[n++] = 0xE9; rel = (int32_t)(game + 0xAD3670u - (uintptr_t)(s + n + 4)); memcpy(s + n, &rel, 4); n += 4; /* the call, then on */
        }
    }
    DWORD protect;
    VirtualProtect(block, MISSION_LISTS * MISSION_LIST_STUB + 0x100, PAGE_EXECUTE_READWRITE, &protect); /* the counters are written */
    FlushInstructionCache(GetCurrentProcess(), block, MISSION_LISTS * MISSION_LIST_STUB);
    for (size_t i = 0; i < MISSION_LISTS; i++)
    {
        uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
        int32_t rel = DifficultyJumpRel(game + kMissionLists[i].pin, (uintptr_t)(block + i * MISSION_LIST_STUB)); memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kMissionLists[i].pin, jump, 6)) return "a mission list's guard could not be written";
    }
    return NULL;
}

/* GuardForce's patrol spawners: the builder (0x94A270, from mission load 0xAD1FD5 -> 0x94DC20) walks the outpost patrol
 * spawners (HiveMind manager +0x730 count, entries from +0x338), finds each one's location (its stamp unit, through the
 * linked locations), looks for the GuardForce record (0x300 each) whose key is that unit, and appends the spawner's id to
 * the record's one place at +0x2D4 (count +0x2D8 right after) without checking either: a spawner whose location has no
 * record gives index -1 and a read 0x300 x 0xFFFFFFFF past the records (run s73f, game+0x94B32B; three missions'
 * outposts), a second spawner in one record would overwrite its count. Both are left out here (counted at stub +0x40):
 * 0x94B317..0x94B32B rebuilt in the stub with the two checks, the skip continues at the loop's test 0x94B33E. */
static const uint8_t kGarrisonPin[20] = {0x8B, 0xC2, 0xFF, 0xC3, 0x48, 0x8D, 0x14, 0x40, 0x43, 0x8B, 0x44, 0xD5, 0x00, 0x48, 0xC1, 0xE2, 0x08, 0x48, 0x03, 0xD7};
static const uint8_t kGarrisonStub[53] = {
    0x83, 0xFA, 0xFF,                                                 /*  0 cmp edx, -1 (no record) */
    0x74, 0x22,                                                       /*  3 je 39 */
    0x8B, 0xC2,                                                       /*  5 mov eax, edx */
    0x48, 0x8D, 0x14, 0x40,                                           /*  7 lea rdx, [rax+rax*2] */
    0x48, 0xC1, 0xE2, 0x08,                                           /* 11 shl rdx, 8 */
    0x48, 0x03, 0xD7,                                                 /* 15 add rdx, rdi (the record) */
    0x83, 0xBA, 0xD8, 0x02, 0x00, 0x00, 0x01,                         /* 18 cmp dword [rdx+0x2d8], 1 (its one place taken) */
    0x73, 0x0C,                                                       /* 25 jae 39 */
    0xFF, 0xC3,                                                       /* 27 inc ebx */
    0x43, 0x8B, 0x44, 0xD5, 0x00,                                     /* 29 mov eax, [r13+r10*8] (the spawner's id) */
    0xE9, 0, 0, 0, 0,                                                 /* 34 jmp 0x94B32B (the append; rel32 at 35) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 39 lock inc dword [rip -> stub+0x40] (rel32 at 42) */
    0xFF, 0xC3,                                                       /* 46 inc ebx */
    0xE9, 0, 0, 0, 0};                                                /* 48 jmp 0x94B33E (the loop's test; rel32 at 49) */
static volatile LONG* g_garrisonSkipped;
static const char* InstallGarrisonGuard(uintptr_t game)
{
    uint8_t pin[20];
    if (SafeRead(game + 0x94B317u, pin, 20) || memcmp(pin, kGarrisonPin, 20) != 0 || !ApiCodeMatches(game + 0x94B32Bu, "8B 8A D8 02 00 00") ||
        !ApiCodeMatches(game + 0x94B33Eu, "41 3B 9D 30 07 00 00 0F 82"))
        return "GuardForce's patrol spawner lookup is not the reviewed one; it stays unchecked";
    uint8_t* stub = AllocateNearGame(game, 0x80);
    if (stub == NULL) return "no memory within 2 GB of game.dll for GuardForce's patrol spawner guard";
    memcpy(stub, kGarrisonStub, sizeof kGarrisonStub);
    g_garrisonSkipped = (volatile LONG*)(stub + 0x40);
    int32_t rel = (int32_t)(game + 0x94B32Bu - (uintptr_t)(stub + 39)); memcpy(stub + 35, &rel, 4);
    rel = (int32_t)((stub + 0x40) - (stub + 46)); memcpy(stub + 42, &rel, 4);
    rel = (int32_t)(game + 0x94B33Eu - (uintptr_t)(stub + 53)); memcpy(stub + 49, &rel, 4);
    DWORD protect;
    VirtualProtect(stub, 0x80, PAGE_EXECUTE_READWRITE, &protect);
    FlushInstructionCache(GetCurrentProcess(), stub, 0x80);
    uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    rel = DifficultyJumpRel(game + 0x94B317u, (uintptr_t)stub); memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + 0x94B317u, jump, 8)) return "GuardForce's patrol spawner guard could not be written";
    return NULL;
}

/* GuardForce's alarms: the same builder walks the alarm effects (HiveMind manager +0x86FA8, 0x10 each, count +0x877A8),
 * maps each one's stamp unit through the linked locations and appends its id to the group's 32 places at +0x250 (count
 * +0x2D0), with the same unchecked index (-1 when no group has that key, 0x94B247) and no check of the 32. Both are left
 * out (counted at stub +0x40): 0x94B247..0x94B25E rebuilt with the two checks, the skip continues at the loop's test
 * 0x94B271. */
static const uint8_t kAlarmPin[23] = {0x8B, 0xC2, 0xFF, 0xC3, 0x48, 0x8D, 0x14, 0x40, 0x43, 0x8B, 0x84, 0xDD, 0xA8, 0x6F, 0x08, 0x00,
                                      0x48, 0xC1, 0xE2, 0x08, 0x48, 0x03, 0xD7};
static const uint8_t kAlarmStub[56] = {
    0x83, 0xFA, 0xFF,                                                 /*  0 cmp edx, -1 (no group) */
    0x74, 0x25,                                                       /*  3 je 42 */
    0x89, 0xD0,                                                       /*  5 mov eax, edx */
    0x48, 0x8D, 0x14, 0x40,                                           /*  7 lea rdx, [rax+rax*2] */
    0x48, 0xC1, 0xE2, 0x08,                                           /* 11 shl rdx, 8 */
    0x48, 0x01, 0xFA,                                                 /* 15 add rdx, rdi (the group) */
    0x83, 0xBA, 0xD0, 0x02, 0x00, 0x00, 0x20,                         /* 18 cmp dword [rdx+0x2d0], 32 (its places full) */
    0x73, 0x0F,                                                       /* 25 jae 42 */
    0xFF, 0xC3,                                                       /* 27 inc ebx */
    0x43, 0x8B, 0x84, 0xDD, 0xA8, 0x6F, 0x08, 0x00,                   /* 29 mov eax, [r13+r11*8+0x86fa8] (the alarm's id) */
    0xE9, 0, 0, 0, 0,                                                 /* 37 jmp 0x94B25E (the append; rel32 at 38) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 42 lock inc dword [rip -> stub+0x40] (rel32 at 45) */
    0xFF, 0xC3,                                                       /* 49 inc ebx */
    0xE9, 0, 0, 0, 0};                                                /* 51 jmp 0x94B271 (the loop's test; rel32 at 52) */
static volatile LONG* g_alarmSkipped;
static const char* InstallAlarmGuard(uintptr_t game)
{
    uint8_t pin[23];
    if (SafeRead(game + 0x94B247u, pin, 23) || memcmp(pin, kAlarmPin, 23) != 0 || !ApiCodeMatches(game + 0x94B25Eu, "8B 8A D0 02 00 00") ||
        !ApiCodeMatches(game + 0x94B271u, "41 3B 9D A8 77 08 00 0F 82"))
        return "GuardForce's alarm lookup is not the reviewed one; it stays unchecked";
    uint8_t* stub = AllocateNearGame(game, 0x80);
    if (stub == NULL) return "no memory within 2 GB of game.dll for GuardForce's alarm guard";
    memcpy(stub, kAlarmStub, sizeof kAlarmStub);
    g_alarmSkipped = (volatile LONG*)(stub + 0x40);
    int32_t rel = (int32_t)(game + 0x94B25Eu - (uintptr_t)(stub + 42)); memcpy(stub + 38, &rel, 4);
    rel = (int32_t)((stub + 0x40) - (stub + 49)); memcpy(stub + 45, &rel, 4);
    rel = (int32_t)(game + 0x94B271u - (uintptr_t)(stub + 56)); memcpy(stub + 52, &rel, 4);
    DWORD protect;
    VirtualProtect(stub, 0x80, PAGE_EXECUTE_READWRITE, &protect);
    FlushInstructionCache(GetCurrentProcess(), stub, 0x80);
    uint8_t jump[23];
    memset(jump, 0x90, sizeof jump);
    jump[0] = 0xE9;
    rel = DifficultyJumpRel(game + 0x94B247u, (uintptr_t)stub); memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + 0x94B247u, jump, 23)) return "GuardForce's alarm guard could not be written";
    return NULL;
}

/* Op 0x7C (AttachLocationsToRouteCells 0xF81020) lists, per location, the zones of the region's zone list (R+0x238, 0x44
 * each, R = [S+0xB0], shared resource data) that may be painted (+0x259 & 2) and whose type lists the location's kind
 * (type+0x210), and picks one by weight (0xF81382..0xF8147A) with no check that it found any: none gives index -1
 * (0xF81477 lea eax,[rdi-1]) and a read far outside the list (s75c: c0000005 at 0xF81482). A city map's open-land zones
 * list kind 1 but are not paintable, so the nature variants' locations (kind 1) find none. Such a location is left
 * unattached, as the game leaves one whose definition opts out (0xF8129F -> 0xF81DDD): its cells keep their zone, which
 * on the open land already lists kind 1. 0xF81382..0xF81387 (xor r8d,r8d / xorps xmm0,xmm0) move into a stub that first
 * tests the count (edi) and on 0 goes to the loop's next location (0xF81DD5: rdi reloaded, r11 the index; counted at
 * stub +0x40). */
static const uint8_t kAttachStub[27] = {
    0x85, 0xFF,                         /*  0 test edi,edi */
    0x75, 0x0C,                         /*  2 jnz 16 */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,       /*  4 lock inc dword [rip -> stub+0x40] (rel32 at 7) */
    0xE9, 0, 0, 0, 0,                   /* 11 jmp 0xF81DD5 (rel32 at 12) */
    0x45, 0x33, 0xC0,                   /* 16 xor r8d,r8d */
    0x0F, 0x57, 0xC0,                   /* 19 xorps xmm0,xmm0 */
    0xE9, 0, 0, 0, 0};                  /* 22 jmp 0xF81388 (rel32 at 23) */
static const char* InstallAttachGuard(uintptr_t game)
{
    if (!ApiCodeMatches(game + 0xF8137Au, "4C 8B 9C 24 B8 00 00 00 45 33 C0 0F 57 C0 83 FF 04 72 62") ||
        !ApiCodeMatches(game + 0xF81DD5u, "48 8B BC 24 98 00 00 00 8B 4C 24 40"))
        return "op 0x7C's zone pick is not the reviewed one; a location with no zone of its kind is not guarded";
    uint8_t* stub = AllocateNearGame(game, 0x80);
    if (stub == NULL) return "no memory within 2 GB of game.dll for op 0x7C's zone pick guard";
    memcpy(stub, kAttachStub, sizeof kAttachStub);
    g_attachSkipped = (volatile LONG*)(stub + 0x40);
    int32_t rel = (int32_t)((stub + 0x40) - (stub + 11)); memcpy(stub + 7, &rel, 4);
    rel = (int32_t)(game + 0xF81DD5u - (uintptr_t)(stub + 16)); memcpy(stub + 12, &rel, 4);
    rel = (int32_t)(game + 0xF81388u - (uintptr_t)(stub + 27)); memcpy(stub + 23, &rel, 4);
    DWORD protect;
    VirtualProtect(stub, 0x80, PAGE_EXECUTE_READWRITE, &protect);
    FlushInstructionCache(GetCurrentProcess(), stub, 0x80);
    uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
    rel = DifficultyJumpRel(game + 0xF81382u, (uintptr_t)stub); memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + 0xF81382u, jump, 6)) return "op 0x7C's zone pick guard could not be written";
    return NULL;
}

/* Op 0x0D RenderRouteSegments (0xF53000) draws every route edge (type != 2, flags & 0x103 == 0) as a gui line into
 * generated_route_distance, which the zone painters threshold for their path materials. Positions are scaled to pixels
 * by xmm6 = lg+0x120 (the texture side) / [settings+0x2C] (the world side) (0xF53086..0xF530A2); the line's thickness is
 * the edge's width [edge+0x30] passed as is (0xF53116 -> [rsp+0x20] at 0xF531C2), and exe 0x260440 builds the quad at
 * +-0.5 x thickness in the same space, before the gui transform. Vanilla (4096 pixels over 2048 m, 2 px/m) draws w pixels
 * = 0.5 w m; a 16 km world (0.25 px/m) drew 4 w m, 8 x as wide. The stub scales the thickness by xmm6 / 2, so the line
 * is 0.5 w m at any world size; vanilla's factor is 1. The near tiles' replay keeps settings+0x2C and scales the gui by
 * 8 (positions and thickness alike), so their lines are 0.5 w m too. */
static const uint8_t kRouteWidthStub[30] = {
    0xF3, 0x0F, 0x10, 0x7B, 0xFC,       /*  0 movss xmm7, [rbx-4] (the edge's width) */
    0xF3, 0x0F, 0x59, 0xFE,             /*  5 mulss xmm7, xmm6 (pixels per metre) */
    0xF3, 0x0F, 0x59, 0x3D, 0, 0, 0, 0, /*  9 mulss xmm7, [rip -> 0.5 at stub+0x20] (rel32 at 13) */
    0xFF, 0xC7,                         /* 17 inc edi */
    0x3B, 0x7B, 0x08,                   /* 19 cmp edi, [rbx+8] */
    0xE9, 0, 0, 0, 0,                   /* 22 jmp 0xF53120 (the jae; rel32 at 23) */
    0xCC, 0xCC, 0xCC};
static const char* InstallRouteWidth(uintptr_t game)
{
    if (!ApiCodeMatches(game + 0xF53086u, "F3 0F 10 B2 20 01 00 00") || !ApiCodeMatches(game + 0xF530A2u, "F3 0F 5E F0") ||
        !ApiCodeMatches(game + 0xF53113u, "8B 7B 04 F3 0F 10 7B FC FF C7 3B 7B 08 0F 83 B1 00 00 00") ||
        !ApiCodeMatches(game + 0xF531C2u, "F3 0F 11 7C 24 20 41 FF 52 48"))
        return "op 0x0D's route lines are not the reviewed ones; their width stays in pixels (8 x wide on a 16 km map)";
    uint8_t* stub = AllocateNearGame(game, 0x40);
    if (stub == NULL) return "no memory within 2 GB of game.dll for op 0x0D's route line width";
    memcpy(stub, kRouteWidthStub, sizeof kRouteWidthStub);
    const float half = 0.5f;
    memcpy(stub + 0x20, &half, 4);
    int32_t rel = (int32_t)((stub + 0x20) - (stub + 17)); memcpy(stub + 13, &rel, 4);
    rel = (int32_t)(game + 0xF53120u - (uintptr_t)(stub + 27)); memcpy(stub + 23, &rel, 4);
    DWORD protect;
    VirtualProtect(stub, 0x40, PAGE_EXECUTE_READ, &protect);
    FlushInstructionCache(GetCurrentProcess(), stub, 0x40);
    uint8_t jump[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    rel = DifficultyJumpRel(game + 0xF53116u, (uintptr_t)stub); memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + 0xF53116u, jump, 10)) return "op 0x0D's route line width could not be written";
    return NULL;
}

/* The samples' groups: a component init (0x79CCA0, from entity spawn 0x581A50 -> 0x79C756) groups the HiveMind
 * manager's samples (+0x5790, 0x58 each, count +0x5D748) by their owner key into a temporary block of 128 groups
 * (0x810 each: key, 256 {sample, type}, count +0x808; group count +0x40800; size 0x40808 at 0x79CDE4 / 0x79CDFD),
 * unchecked: an operation's 39 objectives brought more owners and the 129th group overwrote the count (run s73l,
 * game+0x79CEDD). The block grows to 256 groups (the reader 0x79FD60 copies the matching groups into a 256-entry stack
 * list, so no more), and a 257th owner or a group's 257th sample is left out (counted at stub +0x60 / +0x64). */
static const struct { uint32_t site; uint8_t length, at; uint8_t pin[7]; uint32_t value; } kSampleGroupSites[6] = {
    {0x79CDE4u, 5, 1, {0xBA, 0x08, 0x08, 0x04, 0x00}, 0x81008u},
    {0x79CDFDu, 6, 2, {0x41, 0xB8, 0x08, 0x08, 0x04, 0x00}, 0x81008u},
    {0x79CE77u, 7, 3, {0x45, 0x8B, 0x86, 0x00, 0x08, 0x04, 0x00}, 0x81000u},
    {0x79CEC0u, 7, 3, {0x41, 0x8B, 0x96, 0x00, 0x08, 0x04, 0x00}, 0x81000u},
    {0x79CECAu, 7, 3, {0x41, 0x89, 0x86, 0x00, 0x08, 0x04, 0x00}, 0x81000u},
    {0x79FD88u, 7, 3, {0x41, 0x8B, 0x80, 0x00, 0x08, 0x04, 0x00}, 0x81000u}};
static const uint8_t kSampleGroupPin[8] = {0x4B, 0x8B, 0x84, 0x3B, 0x90, 0x57, 0x00, 0x00};      /* 0x79CEAD mov rax, [r11+r15+0x5790] */
static const uint8_t kSampleEntryPin[10] = {0x8B, 0x82, 0x08, 0x08, 0x00, 0x00, 0x89, 0x7C, 0xC2, 0x08}; /* 0x79CEDD */
static const uint8_t kSampleStubs[0x44] = {
    0x41, 0x81, 0xF8, 0x00, 0x01, 0x00, 0x00,                         /*  0 cmp r8d, 256 (the groups) */
    0x73, 0x0D,                                                       /*  7 jae 0x16 */
    0x4B, 0x8B, 0x84, 0x3B, 0x90, 0x57, 0x00, 0x00,                   /*  9 mov rax, [r11+r15+0x5790] */
    0xE9, 0, 0, 0, 0,                                                 /* 0x11 jmp 0x79CEB5 (rel32 at 0x12) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 0x16 lock inc [rip -> +0x60] (rel32 at 0x19) */
    0xE9, 0, 0, 0, 0,                                                 /* 0x1D jmp 0x79CEFF (the next sample; rel32 at 0x1E) */
    0x8B, 0x82, 0x08, 0x08, 0x00, 0x00,                               /* 0x22 mov eax, [rdx+0x808] (the group's samples) */
    0x3D, 0x00, 0x01, 0x00, 0x00,                                     /* 0x28 cmp eax, 256 */
    0x73, 0x09,                                                       /* 0x2D jae 0x38 */
    0x89, 0x7C, 0xC2, 0x08,                                           /* 0x2F mov [rdx+rax*8+8], edi */
    0xE9, 0, 0, 0, 0,                                                 /* 0x33 jmp 0x79CEE7 (rel32 at 0x34) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 0x38 lock inc [rip -> +0x64] (rel32 at 0x3B) */
    0xE9, 0, 0, 0, 0};                                                /* 0x3F jmp 0x79CEFF (rel32 at 0x40) */
static volatile LONG* g_sampleGroupsFull; /* [0] owners past 256 groups, [1] samples past a group's 256 */
static const char* InstallSampleGroups(uintptr_t game)
{
    uint8_t bytes[10];
    for (int i = 0; i < 6; i++)
        if (SafeRead(game + kSampleGroupSites[i].site, bytes, kSampleGroupSites[i].length) || memcmp(bytes, kSampleGroupSites[i].pin, kSampleGroupSites[i].length) != 0)
            return "the samples' groups are not the reviewed ones; they stay at 128, unchecked";
    if (SafeRead(game + 0x79CEADu, bytes, 8) || memcmp(bytes, kSampleGroupPin, 8) != 0 || SafeRead(game + 0x79CEDDu, bytes, 10) ||
        memcmp(bytes, kSampleEntryPin, 10) != 0)
        return "the samples' groups are not the reviewed ones; they stay at 128, unchecked";
    uint8_t* stub = AllocateNearGame(game, 0x80);
    if (stub == NULL) return "no memory within 2 GB of game.dll for the samples' groups";
    memcpy(stub, kSampleStubs, sizeof kSampleStubs);
    g_sampleGroupsFull = (volatile LONG*)(stub + 0x60);
    int32_t rel;
    rel = (int32_t)(game + 0x79CEB5u - (uintptr_t)(stub + 0x16)); memcpy(stub + 0x12, &rel, 4);
    rel = (int32_t)((stub + 0x60) - (stub + 0x1D)); memcpy(stub + 0x19, &rel, 4);
    rel = (int32_t)(game + 0x79CEFFu - (uintptr_t)(stub + 0x22)); memcpy(stub + 0x1E, &rel, 4);
    rel = (int32_t)(game + 0x79CEE7u - (uintptr_t)(stub + 0x38)); memcpy(stub + 0x34, &rel, 4);
    rel = (int32_t)((stub + 0x64) - (stub + 0x3F)); memcpy(stub + 0x3B, &rel, 4);
    rel = (int32_t)(game + 0x79CEFFu - (uintptr_t)(stub + 0x44)); memcpy(stub + 0x40, &rel, 4);
    DWORD protect;
    VirtualProtect(stub, 0x80, PAGE_EXECUTE_READWRITE, &protect);
    FlushInstructionCache(GetCurrentProcess(), stub, 0x80);
    for (int i = 0; i < 6; i++)
    {
        uint8_t code[7];
        memcpy(code, kSampleGroupSites[i].pin, kSampleGroupSites[i].length);
        memcpy(code + kSampleGroupSites[i].at, &kSampleGroupSites[i].value, 4);
        if (ApiWrite(game + kSampleGroupSites[i].site, code, kSampleGroupSites[i].length)) return "the samples' groups could not grow";
    }
    uint8_t group[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90}, entry[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    rel = DifficultyJumpRel(game + 0x79CEADu, (uintptr_t)stub); memcpy(group + 1, &rel, 4);
    rel = DifficultyJumpRel(game + 0x79CEDDu, (uintptr_t)(stub + 0x22)); memcpy(entry + 1, &rel, 4);
    if (ApiWrite(game + 0x79CEADu, group, 8) || ApiWrite(game + 0x79CEDDu, entry, 10)) return "the samples' groups could not be guarded";
    return NULL;
}

/* The AI's tasks: [game+0x346D5C0] holds 2048 x 0x58 (count +0x2C000), appended by 0x126FAC0 without a check. Mission
 * load adds one per guard-point unit (0xAD25DF, its result unused); the running mission adds more (0xADD5CC, which
 * keeps the entry). Three missions' guard points filled it at load (run s73n: game+0x126FB81, r9 = 2048). The load's
 * call goes through a stub that leaves a guard point out once 1792 are used (the rest stays for the running mission's
 * own), counted at stub +0x40. */
static const uint8_t kTaskStub[0x20] = {
    0x48, 0x8B, 0x05, 0, 0, 0, 0,                                     /*  0 mov rax, [rip -> game+0x346D5C0] (rel32 at 3) */
    0x81, 0xB8, 0x00, 0xC0, 0x02, 0x00, 0x00, 0x07, 0x00, 0x00,       /*  7 cmp dword [rax+0x2C000], 1792 */
    0x73, 0x05,                                                       /* 0x11 jae 0x18 */
    0xE9, 0, 0, 0, 0,                                                 /* 0x13 jmp 0x126FAC0 (rel32 at 0x14; returns to the caller) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 0x18 lock inc [rip -> +0x40] (rel32 at 0x1B) */
    0xC3};                                                            /* 0x1F ret */
static volatile LONG* g_tasksLeftOut;
static uintptr_t g_taskGame;
static const char* InstallTaskGuard(uintptr_t game)
{
    if (!ApiCodeMatches(game + 0xAD25DFu, "E8 DC D4 79 00") || !ApiCodeMatches(game + 0x126FB60u, "41 8B 86 00 C0 02 00 48 8D 4D B0 4C 6B F8 58"))
        return "the AI's task list is not the reviewed one; it stays unchecked";
    uint8_t* stub = AllocateNearGame(game, 0x80);
    if (stub == NULL) return "no memory within 2 GB of game.dll for the AI's task guard";
    memcpy(stub, kTaskStub, sizeof kTaskStub);
    g_tasksLeftOut = (volatile LONG*)(stub + 0x40);
    g_taskGame = game;
    int32_t rel = (int32_t)(game + 0x346D5C0u - (uintptr_t)(stub + 7)); memcpy(stub + 3, &rel, 4);
    rel = (int32_t)(game + 0x126FAC0u - (uintptr_t)(stub + 0x18)); memcpy(stub + 0x14, &rel, 4);
    rel = (int32_t)((stub + 0x40) - (stub + 0x1F)); memcpy(stub + 0x1B, &rel, 4);
    DWORD protect;
    VirtualProtect(stub, 0x80, PAGE_EXECUTE_READWRITE, &protect);
    FlushInstructionCache(GetCurrentProcess(), stub, 0x80);
    uint8_t call[5] = {0xE8, 0, 0, 0, 0};
    rel = DifficultyJumpRel(game + 0xAD25DFu, (uintptr_t)stub); memcpy(call + 1, &rel, 4);
    if (ApiWrite(game + 0xAD25DFu, call, 5)) return "the AI's task guard could not be written";
    return NULL;
}
static uint32_t TasksUsed(void)
{
    uint64_t list = 0;
    uint32_t n = 0;
    if (g_taskGame == 0 || SafeRead(g_taskGame + 0x346D5C0u, &list, 8) || list == 0 || SafeRead(list + 0x2C000, &n, 4)) return 0xFFFFFFFFu;
    return n;
}

/* The lists that left units out so far, as "name n" pairs (empty when none). */
static void MissionListsFull(char* out, size_t size)
{
    size_t n = 0; out[0] = 0;
    for (size_t i = 0; g_missionListFull != NULL && i < MISSION_LISTS && n + 48 < size; i++)
        if (g_missionListFull[i] > 0) n += (size_t)snprintf(out + n, size - n, " %s %ld;", kMissionLists[i].name, (long)g_missionListFull[i]);
}

/* An extra large map's world: the block (stub +0, values +0x1000), the readers pointed at it, the size store through
 * the stub. */
static const char* InstallWorldPlan(uintptr_t game); /* difficulty_world_plan.c: the Hive Lord graph on a grid */
static const char* InstallWorldOverlap(uintptr_t game); /* difficulty_world_overlap.c: the stamp overlap test on a grid */
static const char* InstallWorldPoisson(uintptr_t game); /* difficulty_world_poisson.c: Poisson-disk sets from a periodic tile */
static const char* InstallSpreadSort(uintptr_t game); /* difficulty_world_poisson.c: SpreadLocations' candidate sort */
static const char* InstallTowns(uintptr_t game); /* difficulty_world_towns.c */
/* Op 0x3F's rectangle pads on an extra large level (0xF4DC3C..0xF4DCA9, 0xF4E631): the band e T+0x3C ln(a+1) goes into
 * the record (+0x1C, +0x20) unscaled, but the pad shader reads it in heightmap pixels, and the scale is 0.25 px/m at
 * 16384 m (2 on vanilla), so the falloff stopped at a quarter and a pad standing over the ground left a wall (audit 15
 * criterion 8, s76d objective #8: 42-56 m). The stubs store k band s (padPx = k / scale; s = 2 / scale), pass the
 * falloff's k band / 2 metres to the quad's corners (0xFC8D50) and read the stored pixels back in metres for the second
 * call (padQuad = scale / 2). A vanilla level has 1 / 1 / 1: the same products, bit for bit. xmm1 (site A, before the
 * call to log) and xmm0 (site B, before 0xFC8D50, whose first argument is rcx) are free. Circles store their band in
 * pixels already (0xF4D963). */
#define PAD_BAND_SITE_A 0xF4DC65u
#define PAD_BAND_SITE_B 0xF4DC9Au
#define PAD_BAND_SITE_C 0xF4E631u
static const uint8_t kPadBandA[33] = {
    0xF3, 0x0F, 0x59, 0x36,                         /* mulss xmm6, [rsi]          (e) */
    0x0F, 0x28, 0xCE,                               /* movaps xmm1, xmm6 */
    0xF3, 0x0F, 0x59, 0x0D, 0, 0, 0, 0,             /* mulss xmm1, [padPx]        @11 */
    0xF3, 0x0F, 0x11, 0x4B, 0x1C,                   /* movss [rbx+0x1C], xmm1 */
    0xF3, 0x0F, 0x59, 0x35, 0, 0, 0, 0,             /* mulss xmm6, [padMetre]     @24 */
    0xE9, 0, 0, 0, 0};                              /* jmp 0xF4DC6E               @29 */
static const uint8_t kPadBandB[39] = {
    0xF3, 0x0F, 0x59, 0x16,                         /* mulss xmm2, [rsi] */
    0x0F, 0x28, 0xC2,                               /* movaps xmm0, xmm2 */
    0xF3, 0x0F, 0x59, 0x05, 0, 0, 0, 0,             /* mulss xmm0, [padPx]        @11 */
    0xF3, 0x0F, 0x11, 0x43, 0x20,                   /* movss [rbx+0x20], xmm0 */
    0xF3, 0x0F, 0x59, 0x15, 0, 0, 0, 0,             /* mulss xmm2, [padMetre]     @24 */
    0xF3, 0x0F, 0x11, 0x54, 0x24, 0x20,             /* movss [rsp+0x20], xmm2 */
    0xE9, 0, 0, 0, 0};                              /* jmp 0xF4DCA9               @35 */
static const uint8_t kPadBandC[37] = {
    0xF3, 0x0F, 0x10, 0x43, 0x20,                   /* movss xmm0, [rbx+0x20] */
    0xF3, 0x0F, 0x59, 0x05, 0, 0, 0, 0,             /* mulss xmm0, [padQuad]      @9 */
    0xF3, 0x0F, 0x10, 0x5B, 0x1C,                   /* movss xmm3, [rbx+0x1C] */
    0xF3, 0x0F, 0x59, 0x1D, 0, 0, 0, 0,             /* mulss xmm3, [padQuad]      @22 */
    0xF3, 0x0F, 0x11, 0x44, 0x24, 0x20,             /* movss [rsp+0x20], xmm0 */
    0xE9, 0, 0, 0, 0};                              /* jmp 0xF4E641               @33 */
/* Each displacement ends its instruction: the jump's is the last 4 bytes after its E9 */
_Static_assert(sizeof kPadBandA == 29 + 4 && sizeof kPadBandB == 35 + 4 && sizeof kPadBandC == 33 + 4, "the pad band stubs end with their jumps");
static void PadBandRel(uint8_t* stub, int at, const void* target)
{
    int32_t rel = (int32_t)((intptr_t)target - (intptr_t)(stub + at + 4));
    memcpy(stub + at, &rel, 4);
}
/* Writes the three stubs at code (0xC0 bytes of the world block, before it turns read-only). 1 when the sites are the
 * reviewed ones. */
static int PadBandStubs(uintptr_t game, uint8_t* code)
{
    if (!ApiCodeMatches(game + PAD_BAND_SITE_A, "F3 0F 59 36 F3 0F 11 73 1C") ||
        !ApiCodeMatches(game + PAD_BAND_SITE_B, "F3 0F 59 16 F3 0F 11 53 20 F3 0F 11 54 24 20") ||
        !ApiCodeMatches(game + PAD_BAND_SITE_C, "F3 0F 10 43 20 F3 0F 10 5B 1C F3 0F 11 44 24 20"))
    {
        Note("difficulty: op 0x3F's rectangle pads are not the reviewed ones: their bands stay vanilla on extra large levels");
        return 0;
    }
    if (kPadBandA[28] != 0xE9 || kPadBandB[34] != 0xE9 || kPadBandC[32] != 0xE9 || kPadBandA[10] != 0x0D || kPadBandA[23] != 0x35 ||
        kPadBandB[10] != 0x05 || kPadBandB[23] != 0x15 || kPadBandC[8] != 0x05 || kPadBandC[21] != 0x1D)
    {
        Note("difficulty: op 0x3F's rectangle pad stubs are malformed: their bands stay vanilla on extra large levels");
        return 0;
    }
    WorldValues* v = g_world.values;
    uint8_t *a = code, *b = code + 0x40, *c = code + 0x80;
    memcpy(a, kPadBandA, sizeof kPadBandA);
    PadBandRel(a, 11, &v->padPx); PadBandRel(a, 24, &v->padMetre); PadBandRel(a, 29, (void*)(game + PAD_BAND_SITE_A + 9));
    memcpy(b, kPadBandB, sizeof kPadBandB);
    PadBandRel(b, 11, &v->padPx); PadBandRel(b, 24, &v->padMetre); PadBandRel(b, 35, (void*)(game + PAD_BAND_SITE_B + 15));
    memcpy(c, kPadBandC, sizeof kPadBandC);
    PadBandRel(c, 9, &v->padQuad); PadBandRel(c, 22, &v->padQuad); PadBandRel(c, 33, (void*)(game + PAD_BAND_SITE_C + 16));
    return 1;
}
/* The jumps into the stubs (after the block is executable). NULL or the reason. */
static const char* PadBandSites(uintptr_t game, uint8_t* code)
{
    static const struct { uint32_t site, length, stub; } kSites[3] = {
        {PAD_BAND_SITE_A, 9, 0}, {PAD_BAND_SITE_B, 15, 0x40}, {PAD_BAND_SITE_C, 16, 0x80}};
    for (int i = 0; i < 3; i++)
    {
        uint8_t jump[16];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        int32_t rel = DifficultyJumpRel(game + kSites[i].site, (uintptr_t)(code + kSites[i].stub)); memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kSites[i].site, jump, kSites[i].length)) return "op 0x3F's rectangle pads could not get bands for the larger world";
    }
    return NULL;
}
/* Op 0x3E ForceFlattenZones (0xF4C100) draws each flatten zone with the pad rectangle shader too (material
 * 0x91134400CB00C3F5, stamp_rectangles_data_1/2/3) and stores the zone type's band +0x208 (128) in the record's +0x1C /
 * +0x20 unscaled (0xF4C4C9..0xF4C4DC) while the shader reads pixels: at 0.25 px/m the band is 512 m against a quad of
 * box +- 128 m (0xF4C52B..), so the blend stops at alpha 0.60-0.84 and every flatten zone left a 320 m square of walls
 * 5-40 m high (t78b / s76d: type-31 zones on the 890.5 m lattice of kFinalizeSpacingStub; static seam17). The stub
 * stores band x flatPx (FLAT_BAND_K / scale: 2 x vanilla's 64 m, w = 0 at the quad's edge; vanilla 1, the same bits).
 * xmm0 is reloaded at 0xF4C52B; no branch lands in the 20 bytes. */
#define FLAT_BAND_SITE 0xF4C4C9u
static const uint8_t kFlatBand[33] = {
    0xF3, 0x0F, 0x10, 0x87, 0x08, 0x02, 0x00, 0x00, /* movss xmm0, [rdi+0x208] */
    0xF3, 0x0F, 0x59, 0x05, 0, 0, 0, 0,             /* mulss xmm0, [flatPx]       @12 */
    0xF3, 0x41, 0x0F, 0x11, 0x42, 0x1C,             /* movss [r10+0x1C], xmm0 */
    0xF3, 0x41, 0x0F, 0x11, 0x42, 0x20,             /* movss [r10+0x20], xmm0 */
    0xE9, 0, 0, 0, 0};                              /* jmp 0xF4C4DD               @29 */
_Static_assert(sizeof kFlatBand == 29 + 4, "the flatten band stub ends with its jump");
static int FlatBandStub(uintptr_t game, uint8_t* code)
{
    if (!ApiCodeMatches(game + FLAT_BAND_SITE, "8B 87 08 02 00 00 41 89 42 1C 8B 87 08 02 00 00 41 89 42 20"))
    {
        Note("difficulty: op 0x3E's flatten zones are not the reviewed ones: their bands stay vanilla on extra large levels");
        return 0;
    }
    if (kFlatBand[11] != 0x05 || kFlatBand[28] != 0xE9) { Note("difficulty: op 0x3E's flatten band stub is malformed: bands stay vanilla"); return 0; }
    memcpy(code, kFlatBand, sizeof kFlatBand);
    int32_t rel = (int32_t)((intptr_t)&g_world.values->flatPx - (intptr_t)(code + 16)); memcpy(code + 12, &rel, 4);
    rel = (int32_t)((intptr_t)(game + FLAT_BAND_SITE + 20) - (intptr_t)(code + 33)); memcpy(code + 29, &rel, 4);
    return 1;
}
static const char* FlatBandSite(uintptr_t game, uint8_t* code)
{
    uint8_t jump[20];
    memset(jump, 0x90, sizeof jump);
    jump[0] = 0xE9;
    int32_t rel = DifficultyJumpRel(game + FLAT_BAND_SITE, (uintptr_t)code); memcpy(jump + 1, &rel, 4);
    return ApiWrite(game + FLAT_BAND_SITE, jump, sizeof jump) ? "op 0x3E's flatten zones could not get bands for the larger world" : NULL;
}
/* The items' map-wide caps item+0x15C (u8): op 0xA0 places a point of interest only while the map's records of its
 * item stay under it (0xF9694B, then cmp [r15+0x10], eax; 0 = never), op 0x8E a satellite likewise (0xF8763A, then
 * test al, al: 0 = no cap). The loader scaled the quotas and category caps by the missions but not these, so XL held
 * 35 points of interest and 23 satellites against a vanilla D11 mission's 24 / 23 (t78c vs b78i; static poi17). The
 * stubs multiply the cap by capMul (the missions inside WorldPassExploration / WorldPassSatelliteCaps, else 1); the
 * satellite one clamps at 255 for its byte test. A cap of 0 stays 0 at both. No other reader of +0x15C changes. */
#define CAP_SITE_POI 0xF9694Bu
#define CAP_SITE_SAT 0xF8763Au
static const uint8_t kCapPoi[19] = {
    0x0F, 0xB6, 0x82, 0x5C, 0x01, 0x00, 0x00,       /* movzx eax, byte [rdx+0x15C] */
    0x0F, 0xAF, 0x05, 0, 0, 0, 0,                   /* imul eax, [capMul]         @10 */
    0xE9, 0, 0, 0, 0};                              /* jmp 0xF96952               @15 */
static const uint8_t kCapSat[32] = {
    0x41, 0x0F, 0xB6, 0x81, 0x5C, 0x01, 0x00, 0x00, /* movzx eax, byte [r9+0x15C] */
    0x0F, 0xAF, 0x05, 0, 0, 0, 0,                   /* imul eax, [capMul]         @11 */
    0x3D, 0xFF, 0x00, 0x00, 0x00,                   /* cmp eax, 0xFF */
    0x76, 0x05,                                     /* jbe +5 */
    0xB8, 0xFF, 0x00, 0x00, 0x00,                   /* mov eax, 0xFF */
    0xE9, 0, 0, 0, 0};                              /* jmp 0xF87642               @28 */
_Static_assert(sizeof kCapPoi == 15 + 4 && sizeof kCapSat == 28 + 4, "the cap stubs end with their jumps");
static int CapStubs(uintptr_t game, uint8_t* code)
{
    if (!ApiCodeMatches(game + CAP_SITE_POI, "0F B6 82 5C 01 00 00 0F 28 D5") || !ApiCodeMatches(game + CAP_SITE_SAT, "41 0F B6 81 5C 01 00 00 84 C0"))
    {
        Note("difficulty: the items' map-wide caps are not the reviewed ones: points of interest and satellites keep one mission's caps");
        return 0;
    }
    if (kCapPoi[9] != 0x05 || kCapPoi[14] != 0xE9 || kCapSat[10] != 0x05 || kCapSat[27] != 0xE9) { Note("difficulty: the cap stubs are malformed"); return 0; }
    uint8_t *a = code, *b = code + 0x20;
    memcpy(a, kCapPoi, sizeof kCapPoi);
    int32_t rel = (int32_t)((intptr_t)&g_world.values->capMul - (intptr_t)(a + 14)); memcpy(a + 10, &rel, 4);
    rel = (int32_t)((intptr_t)(game + CAP_SITE_POI + 7) - (intptr_t)(a + 19)); memcpy(a + 15, &rel, 4);
    memcpy(b, kCapSat, sizeof kCapSat);
    rel = (int32_t)((intptr_t)&g_world.values->capMul - (intptr_t)(b + 15)); memcpy(b + 11, &rel, 4);
    rel = (int32_t)((intptr_t)(game + CAP_SITE_SAT + 8) - (intptr_t)(b + 32)); memcpy(b + 28, &rel, 4);
    return 1;
}
static const char* CapSites(uintptr_t game, uint8_t* code)
{
    static const struct { uint32_t site, length, stub; } kSites[2] = {{CAP_SITE_POI, 7, 0}, {CAP_SITE_SAT, 8, 0x20}};
    for (int i = 0; i < 2; i++)
    {
        uint8_t jump[8];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        int32_t rel = DifficultyJumpRel(game + kSites[i].site, (uintptr_t)(code + kSites[i].stub)); memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kSites[i].site, jump, kSites[i].length)) return "the items' map-wide caps could not follow the missions";
    }
    return NULL;
}
/* Op 0xA0's optional slots per quota entry are one draw rand % (max - min + 1) on the slot builder's own PCG (0xF95310:
 * a local copy of rec+0xE0, never written back; 0xF956DF..0xF956F9). With the quotas x missions (WorldPassExploration)
 * that is one draw over 0..m d: the right mean, but three missions draw three times (s: 5 and 16 optional points of
 * interest in t78c / t78f against b78i's 11). The stub sums capMul draws of rand % (d + 1), d = (max - min) / capMul,
 * each on the next state of the same PCG; with capMul 1 it is the original draw. Free here: rax, rcx, rdx, r9 (set at
 * 0xF95706), r10 (set at 0xF956FF); 0xF956FB tests edx. Only 0xF956D8's jae lands at the site's first byte. */
#define DRAW_SITE 0xF956DFu
static const uint8_t kOptionalDraw[64] = {
    0x41, 0x8B, 0x48, 0x08,                         /* mov ecx, [r8+8] */
    0x41, 0x2B, 0x48, 0x04,                         /* sub ecx, [r8+4] */
    0x8B, 0xC1,                                     /* mov eax, ecx */
    0x33, 0xD2,                                     /* xor edx, edx */
    0xF7, 0x35, 0, 0, 0, 0,                         /* div dword [capMul]         @14 */
    0x44, 0x8D, 0x50, 0x01,                         /* lea r10d, [rax+1] */
    0x44, 0x8B, 0x0D, 0, 0, 0, 0,                   /* mov r9d, [capMul]          @25 */
    0x33, 0xC9,                                     /* xor ecx, ecx */
    0x49, 0x0F, 0xAF, 0xDB,                         /* loop: imul rbx, r11 */
    0x48, 0x03, 0xDF,                               /* add rbx, rdi */
    0x48, 0x8B, 0xC3,                               /* mov rax, rbx */
    0x48, 0xC1, 0xE8, 0x20,                         /* shr rax, 32 */
    0x33, 0xD2,                                     /* xor edx, edx */
    0x41, 0xF7, 0xF2,                               /* div r10d */
    0x03, 0xCA,                                     /* add ecx, edx */
    0x41, 0xFF, 0xC9,                               /* dec r9d */
    0x75, 0xE6,                                     /* jnz loop */
    0x8B, 0xD1,                                     /* mov edx, ecx */
    0xE9, 0, 0, 0, 0};                              /* jmp 0xF956FB               @60 */
_Static_assert(sizeof kOptionalDraw == 60 + 4, "the optional draw stub ends with its jump");
static int OptionalDrawStub(uintptr_t game, uint8_t* code)
{
    if (!ApiCodeMatches(game + DRAW_SITE, "41 8B 48 08 33 D2 41 2B 48 04 49 0F AF DB FF C1 48 03 DF 48 8B C3 48 C1 E8 20 F7 F1"))
    {
        Note("difficulty: op 0xA0's optional slot draw is not the reviewed one: one draw over the missions' range");
        return 0;
    }
    memcpy(code, kOptionalDraw, sizeof kOptionalDraw);
    int32_t rel = (int32_t)((intptr_t)&g_world.values->capMul - (intptr_t)(code + 18)); memcpy(code + 14, &rel, 4);
    rel = (int32_t)((intptr_t)&g_world.values->capMul - (intptr_t)(code + 29)); memcpy(code + 25, &rel, 4);
    rel = (int32_t)((intptr_t)(game + DRAW_SITE + 28) - (intptr_t)(code + 64)); memcpy(code + 60, &rel, 4);
    return 1;
}
static const char* OptionalDrawSite(uintptr_t game, uint8_t* code)
{
    uint8_t jump[28];
    memset(jump, 0x90, sizeof jump);
    jump[0] = 0xE9;
    int32_t rel = DifficultyJumpRel(game + DRAW_SITE, (uintptr_t)code); memcpy(jump + 1, &rel, 4);
    return ApiWrite(game + DRAW_SITE, jump, sizeof jump) ? "op 0xA0's optional slots could not be drawn per mission" : NULL;
}
/* ---- The main world's render capacity -----------------------------------------------------------------------------
 * Static (s78a crash, 2026-11-05): c0000005 at exe 0x58E14B on the render thread, writing [pool+0x1C0] + 0xFFFFFFFF*16.
 * 0x58E0D0 adds an object to one of a render world's six culling pools (0x1E6BF6, [scene+0x268], taken when the object's
 * flags [+0x70] have bit 1) and stores through the slot its allocator (0x751A0) returns, with no check: a full pool
 * (used [pool+0x30] == capacity [pool+0x34]) returns -1. The pools are made once, with the world (0x10CDE0 -> 0x1E4080
 * -> 0x58DDB0), from its capacity N (the 8th argument, [rsp+0xC8] at 0x10CDFD): max(N/8, 512) twice, max(N/16, 512)
 * twice, max(N/12, 512), max(N/64, 128); N also sizes the render world's handles and arrays (0x1D83F0: N handles with
 * a 24-bit index, N/8 x 40 B, N/16 x 296 B). The main game world has N = 0x200000 (pools of 262144, 262144, 131072,
 * 131072, 174762, 32768: read from the live s78b process). Measured: the open XL map holds 100-110 thousand objects in
 * pool 0 at its spots (the count falls as well as rises: no leak); the city map, with its three cities' buildings
 * loaded map-wide, filled all 262144 at an open-land spot while a moving tile replayed (3 tiles live). The stub
 * doubles N for that world only (N == 0x200000), so every pool and array keeps the engine's ratio (about 93 MB more:
 * about 35 B a pool slot and 27.5 B per unit of N); other worlds keep theirs. It counts the main worlds it made, so
 * the log shows the world in play was made after the install (the note at the tile-in). */
#define RENDER_CAP_SITE 0x10CDFDu
#define RENDER_CAP_BACK 0x10CE04u
#define RENDER_CAP_MAIN 0x200000u
#define RENDER_CAP_MUL 2u
static struct { volatile LONG* made; uint32_t* mul; volatile uint64_t* scenePtr; int on; LONG noted; } g_renderCap;
/* The main world's culling pool 0 (the one that filled, 0x58E14B): the second stub (RENDER_SCENE_SITE) keeps the main
 * world's scene; pool 0 is [scene+0x268] (0x1E41F5; the add at 0x1E6BE2 takes it), its used count +0x30 and capacity
 * +0x34 (0x751A0). */
static int RenderPoolOf(uint32_t k, uint32_t* used, uint32_t* capacity)
{
    uint64_t scene = 0, pool = 0;
    if (g_renderCap.on != 1 || (scene = *g_renderCap.scenePtr) == 0 || k > 5) return 0;
    if (SafeRead((uintptr_t)scene + 0x268 + 8u * k, &pool, 8) || pool == 0 ||
        SafeRead((uintptr_t)pool + 0x30, used, 4) || SafeRead((uintptr_t)pool + 0x34, capacity, 4) || *capacity == 0 || *used > *capacity) return 0;
    return 1;
}
static int RenderPool0(uint32_t* used, uint32_t* capacity) { return RenderPoolOf(0, used, capacity); }
/* Pool 1 ([scene+0x270]) fills the same way (0x58E0D0); ~200k of 524288 on city maps (farroom). */
static int RenderPool1(uint32_t* used, uint32_t* capacity) { return RenderPoolOf(1, used, capacity); }
static const char kRenderCapPin[] = "8B BC 24 C8 00 00 00 48 8B F1 B9 00 02 00 00 4C 8B FA 8B C7 4D 8B F0 C1 E8 03";
/* Pools 0 and 1 at 2x more (pool 1 since s89a: 437k of 524288 with the far ring on; the far ring on city maps needs ~+90k entries over a 290-340k peak of 524288; audit 18
 * criterion 9): at 0x10CE29 the pool capacities max(N/8, 512) go to [rsp+0x60] (pool 0: 0x1E41ED reads the block's first
 * dword for 0x58DDB0, stored at [scene+0x268]) and [rsp+0x64] (pool 1). The stub doubles [rsp+0x60] for the main world
 * (edi = N x mul there, from the first stub) and keeps N, so the render handles (0x1D83F0, a 24-bit field) keep their
 * generation bit; the pools are self-contained (about 108 B a slot: 0x567130 allocates cap x 0x6C, plus the free list:
 * ~75 MB). eax and the flags are dead at 0x10CE31 (mov eax,edi; shr; cmp). Static, subagent farroom. */
#define RENDER_POOL0_SITE 0x10CE29u /* mov [rsp+0x60],eax; mov [rsp+0x64],eax (8 bytes) */
#define RENDER_POOL0_BACK 0x10CE31u
static const char kRenderPool0Pin[] = "89 44 24 60 89 44 24 64 8B C7 C1 E8 04";
#define RENDER_SCENE_SITE 0x10CEB6u /* mov [r15],rax; lea rdx,[rsp+0x50] (8 bytes) */
#define RENDER_SCENE_BACK 0x10CEBEu
static const char kRenderScenePin[] = "49 89 07 48 8D 54 24 50 48 8B 03";

static void RenderCapacityInstall(void)
{
    if (g_renderCap.on) return;
    g_renderCap.on = -1;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (!ApiCodeMatches(exe + RENDER_CAP_SITE, kRenderCapPin))
    { Note("difficulty: render capacity: the world creation is not the reviewed code; the main world keeps its pools"); return; }
    uint8_t* page = AllocateNearGame(exe, 0x1000);
    if (page == NULL) { Note("difficulty: render capacity: no room near the exe for the stub"); return; }
    /* the stub at +0, the counter at +0x40, the multiplier at +0x44, the main world's scene at +0x48 (the second stub at
     * +0x60): mov edi,[rsp+0xC8]; cmp edi,0x200000; jne done; lock inc dword [count]; imul edi,[mul]; done: jmp back */
    static const uint8_t code[] = {
        0x8B, 0xBC, 0x24, 0xC8, 0x00, 0x00, 0x00,       /* +0  mov edi,[rsp+0xC8] */
        0x81, 0xFF, 0x00, 0x00, 0x20, 0x00,             /* +7  cmp edi,0x200000 */
        0x75, 0x0E,                                     /* +13 jne +29 */
        0xF0, 0xFF, 0x05, 0, 0, 0, 0,                   /* +15 lock inc dword [rip+rel] (ends +22) */
        0x0F, 0xAF, 0x3D, 0, 0, 0, 0,                   /* +22 imul edi,[rip+rel] (ends +29) */
        0xE9, 0, 0, 0, 0};                              /* +29 jmp back (ends +34) */
    _Static_assert(sizeof code == 34, "render capacity stub size");
    /* At 0x10CEB6 the scene 0x1E4080 made (it returns its rcx, r12: 0x1E4CCF) is in rax and edi still holds N (0x1E4080
     * and the allocator call keep rdi): mov [r15],rax; cmp edi,N x mul; jne +7; mov [scene],rax; lea rdx,[rsp+0x50];
     * jmp 0x10CEBE (flags are dead: the next reader is a call). The callers' out-pointer (rdx) is a stack slot
     * (0x76865, 0x317CD0): the scene itself has to be kept. */
    static const uint8_t keep[] = {
        0x49, 0x89, 0x07,                               /* +0  mov [r15],rax */
        0x81, 0xFF, 0, 0, 0, 0,                         /* +3  cmp edi,imm32 (N x mul) */
        0x75, 0x07,                                     /* +9  jne +18 */
        0x48, 0x89, 0x05, 0, 0, 0, 0,                   /* +11 mov [rip+rel],rax (ends +18) */
        0x48, 0x8D, 0x54, 0x24, 0x50,                   /* +18 lea rdx,[rsp+0x50] */
        0xE9, 0, 0, 0, 0};                              /* +23 jmp 0x10CEBE (ends +28) */
    _Static_assert(sizeof keep == 28, "render capacity scene stub size");
    if (!ApiCodeMatches(exe + RENDER_SCENE_SITE, kRenderScenePin))
    { Note("difficulty: render capacity: the world creation's scene store is not the reviewed code; the main world keeps its pools"); return; }
    /* cmp edi,N x mul; jne +2; add eax,eax; mov [rsp+0x60],eax; mov [rsp+0x64],eax; jmp 0x10CE31: pools 0 and 1 (s89a,
     * city, far ring on: pool 1 reached 437k of 524288 while pool 0 stayed at 54 % of its doubled size) */
    static const uint8_t pool0[] = {
        0x81, 0xFF, 0, 0, 0, 0,                         /* +0  cmp edi,imm32 */
        0x75, 0x02,                                     /* +6  jne +10 */
        0x01, 0xC0,                                     /* +8  add eax,eax */
        0x89, 0x44, 0x24, 0x60,                         /* +10 mov [rsp+0x60],eax */
        0x89, 0x44, 0x24, 0x64,                         /* +14 mov [rsp+0x64],eax */
        0xE9, 0, 0, 0, 0};                              /* +18 jmp 0x10CE31 (ends +23) */
    _Static_assert(sizeof pool0 == 23, "render pool 0 stub size");
    const int pool0Ok = ApiCodeMatches(exe + RENDER_POOL0_SITE, kRenderPool0Pin) == 1;
    if (!pool0Ok) Note("difficulty: render capacity: the pool sizes are not the reviewed code; pool 0 keeps the world's ratio");
    memcpy(page, code, sizeof code);
    memcpy(page + 0x60, keep, sizeof keep);
    memcpy(page + 0x80, pool0, sizeof pool0);
    g_renderCap.made = (volatile LONG*)(page + 0x40);
    g_renderCap.mul = (uint32_t*)(page + 0x44);
    g_renderCap.scenePtr = (volatile uint64_t*)(page + 0x48);
    *g_renderCap.made = 0;
    *g_renderCap.mul = RENDER_CAP_MUL;
    *g_renderCap.scenePtr = 0;
    int32_t rel = (int32_t)((intptr_t)(page + 0x40) - (intptr_t)(page + 22)); memcpy(page + 18, &rel, 4);
    rel = (int32_t)((intptr_t)(page + 0x44) - (intptr_t)(page + 29)); memcpy(page + 25, &rel, 4);
    uint32_t mainN = RENDER_CAP_MAIN * RENDER_CAP_MUL; memcpy(page + 0x60 + 5, &mainN, 4); memcpy(page + 0x80 + 2, &mainN, 4);
    intptr_t pool0Back = (intptr_t)(exe + RENDER_POOL0_BACK) - (intptr_t)(page + 0x80 + 23);
    intptr_t pool0Reach = (intptr_t)(page + 0x80) - (intptr_t)(exe + RENDER_POOL0_SITE + 5);
    if (pool0Back != (intptr_t)(int32_t)pool0Back || pool0Reach != (intptr_t)(int32_t)pool0Reach) { Note("difficulty: render capacity: the stub is out of reach"); return; }
    { int32_t r = (int32_t)pool0Back; memcpy(page + 0x80 + 19, &r, 4); }
    rel = (int32_t)((intptr_t)(page + 0x48) - (intptr_t)(page + 0x60 + 18)); memcpy(page + 0x60 + 14, &rel, 4);
    intptr_t sceneBack = (intptr_t)(exe + RENDER_SCENE_BACK) - (intptr_t)(page + 0x60 + 28);
    intptr_t sceneReach = (intptr_t)(page + 0x60) - (intptr_t)(exe + RENDER_SCENE_SITE + 5);
    intptr_t back = (intptr_t)(exe + RENDER_CAP_BACK) - (intptr_t)(page + 34);
    intptr_t reach = (intptr_t)page - (intptr_t)(exe + RENDER_CAP_SITE + 5);
    if (back != (intptr_t)(int32_t)back || reach != (intptr_t)(int32_t)reach || sceneBack != (intptr_t)(int32_t)sceneBack ||
        sceneReach != (intptr_t)(int32_t)sceneReach)
    { Note("difficulty: render capacity: the stub is out of reach"); return; }
    rel = (int32_t)back; memcpy(page + 30, &rel, 4);
    rel = (int32_t)sceneBack; memcpy(page + 0x60 + 24, &rel, 4);
    if (page[13] != 0x75 || page[14] != 29 - 15 || page[15] != 0xF0 || page[22] != 0x0F || page[29] != 0xE9 ||
        page[0x60 + 9] != 0x75 || page[0x60 + 11] != 0x48 || page[0x60 + 23] != 0xE9)
    { Note("difficulty: render capacity: the stub did not assemble"); return; }
    DWORD old = 0;
    if (!VirtualProtect(page, 0x1000, PAGE_EXECUTE_READWRITE, &old)) { Note("difficulty: render capacity: the stub is not executable"); return; }
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    uint8_t jump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    rel = (int32_t)reach; memcpy(jump + 1, &rel, 4);
    uint8_t sceneJump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    rel = (int32_t)sceneReach; memcpy(sceneJump + 1, &rel, 4);
    uint8_t pool0Jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    rel = (int32_t)pool0Reach; memcpy(pool0Jump + 1, &rel, 4);
    if (ApiWrite(exe + RENDER_SCENE_SITE, sceneJump, sizeof sceneJump) || (pool0Ok && ApiWrite(exe + RENDER_POOL0_SITE, pool0Jump, sizeof pool0Jump)) ||
        ApiWrite(exe + RENDER_CAP_SITE, jump, sizeof jump))
    { Note("difficulty: render capacity: the world creation could not be patched"); return; }
    g_renderCap.on = 1;
    Note("difficulty: render capacity: the main world (capacity 0x%X) is made at %ux: culling pools of %u, not %u; pools 0 and 1 of %u", RENDER_CAP_MAIN, RENDER_CAP_MUL,
        RENDER_CAP_MAIN / 8u * RENDER_CAP_MUL, RENDER_CAP_MAIN / 8u, RENDER_CAP_MAIN / 8u * RENDER_CAP_MUL * (pool0Ok ? 2u : 1u));
}

/* At the tile-in: how many main worlds the stub made (0 = the world in play was made before the install, at vanilla size). */
static void RenderCapacityNote(void)
{
    if (g_renderCap.on != 1) return;
    LONG made = *g_renderCap.made;
    if (made == g_renderCap.noted && made != 0) return;
    g_renderCap.noted = made;
    if (made == 0) Note("difficulty: render capacity: no main world was made since the install; the one in play has vanilla pools (262144)");
    else Note("difficulty: render capacity: %ld main world(s) made at %ux since the install", (long)made, RENDER_CAP_MUL);
}

static const char* InstallCityGates(uintptr_t game); /* difficulty_world_gates.c */
/* Op 0x7C AttachLocationsToRouteCells (0xF81020) on an XL map (KB xl-world-streaming, "i3 crash"; static, P): its list
 * of painted route cells L ([rsp+0x48]) and its DFS stack S ([rsp+0x88]) come from the generation bump arena sized
 * cells x 8 (lea rdx, [rbx*8] at 0xF8112B / 0xF810F4; rbx = the route-cell count, 0xF810AE). L's count r12 is zeroed once
 * (0xF81149) and every location that starts a cluster pushes its cell again (0xF81676, no duplicate check), so L can
 * reach cells + locations. At 8x the important locations (326) outnumber the cells (301): L ran into the zone roll's
 * u32[16] D0 that follows it (0xF81151), whose (9, 10) was then read as a cell (0xF81C83, c0000005 at 0xA00000011).
 * Both sizes become (cells + locations) x 8: lea rdx, [rbx+rbp]; shl rdx, 3 (rbp = the location count, 0xF81071; both
 * zero-extended u32 and preserved across the arena calls). A probe at 0xF81BE3 (mov [rsp+0x68], r12d) keeps L's peak
 * (g_paintedPeak, declared with the op wrapper). */
static const char* InstallRouteCellLists(uintptr_t game)
{
    static const uint8_t sized[8] = {0x48, 0x8D, 0x14, 0x2B, 0x48, 0xC1, 0xE2, 0x03};
    if (!ApiCodeMatches(game + 0xF810F4u, "48 8D 14 DD 00 00 00 00") || !ApiCodeMatches(game + 0xF8112Bu, "48 8D 14 DD 00 00 00 00") ||
        !ApiCodeMatches(game + 0xF810AEu, "8B 18") || !ApiCodeMatches(game + 0xF81071u, "8B 28") || !ApiCodeMatches(game + 0xF81149u, "45 33 E4") ||
        !ApiCodeMatches(game + 0xF81BE3u, "44 89 64 24 68 48 C1 E8 3F"))
        return "op 0x7C's route-cell lists are not the reviewed code; an XL map can overrun them (the i3 crash)";
    if (ApiWrite(game + 0xF810F4u, sized, 8) || ApiWrite(game + 0xF8112Bu, sized, 8)) return "op 0x7C's route-cell lists could not be resized";
    uint8_t* block = AllocateNearGame(game, 0x2000);
    if (block == NULL) return "op 0x7C's lists are resized; no memory near game.dll for their probe";
    uint8_t code[26] = {0x44, 0x89, 0x64, 0x24, 0x68,          /* mov [rsp+0x68], r12d (the original) */
        0x44, 0x3B, 0x25, 0, 0, 0, 0,                          /* cmp r12d, [rip+peak] (rel32 at 8, next 12) */
        0x76, 0x07,                                            /* jbe +7 */
        0x44, 0x89, 0x25, 0, 0, 0, 0,                          /* mov [rip+peak], r12d (rel32 at 17, next 21) */
        0xE9, 0, 0, 0, 0};                                     /* jmp 0xF81BE8 (rel32 at 22, next 26); flags are dead there (shr) */
    int32_t rel = 0x1000 - 12; memcpy(code + 8, &rel, 4);
    rel = 0x1000 - 21; memcpy(code + 17, &rel, 4);
    int64_t back = (int64_t)(game + 0xF81BE8u) - (int64_t)(uintptr_t)(block + 26);
    if (!InRel32(back)) return "op 0x7C's lists are resized; their probe is out of reach";
    rel = (int32_t)back; memcpy(code + 22, &rel, 4);
    memcpy(block, code, sizeof code);
    DWORD protect;
    VirtualProtect(block, 0x1000, PAGE_EXECUTE_READ, &protect);
    FlushInstructionCache(GetCurrentProcess(), block, 0x1000);
    uint8_t jump[5] = {0xE9, 0, 0, 0, 0};
    rel = DifficultyJumpRel(game + 0xF81BE3u, (uintptr_t)block); memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + 0xF81BE3u, jump, 5)) return "op 0x7C's lists are resized; their probe could not be written";
    g_paintedPeak = (volatile uint32_t*)(block + 0x1000);
    return NULL;
}
static void LwInstallWallReach(uintptr_t game); /* living_world.c */
static void InstallBeaconDepth(uintptr_t game); /* difficulty_world_terrain.c: a beacon's water depth over the near tiles */
static const char* InstallWorld(DifficultyInstall* d)
{
    uint32_t scale = WorldScaleFor(g_difficulty.mapScale);
    /* Research (HELLFORGE_XL_CENSUS=1): a level the loader does not grow (Hell Forge's difficulty 11 at map_scale 2,
     * Extra Large Maps off) gets the install too, for the dispatcher's census (WorldDispatch: locations-<level>.tsv and
     * the op attribution); its levels stay vanilla-size (WorldLevel: scale 1, as a vanilla level of an extra large
     * session). Without the install no dispatcher hook and no level pointer (g_worldLevel) exist there. */
    if (scale <= 1 && !WorldCensusOn()) return NULL;
    uintptr_t game = d->game;
    uint8_t* block = AllocateNearGame(game, 0x2000);
    if (block == NULL) return "no memory within 2 GB of game.dll for the extra large world";
    g_world.game = game;
    g_world.values = (WorldValues*)(block + 0x1000);
    WorldValues vanilla = {1024.0f, 2048.0f, 1.0f / 2048.0f, 2000.0f, -2000.0f, 0.0f, 2.3283064365386963e-10f, -1024.0f, 999.0f, -999.0f, -2048.0f, 4096.0f, 512.0f, 4096.0f, -4096.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1, 1900.0f, -1900.0f};
    *g_world.values = vanilla;
    void* level = (void*)WorldLevel;
    int32_t rel;
    memcpy(block, kWorldStub, sizeof kWorldStub);
    memcpy(block + 59, &level, 8);
    rel = (int32_t)((uint8_t*)&g_world.values->size - (block + 130)); memcpy(block + 125, &rel, 4);
    rel = (int32_t)((uint8_t*)&g_world.values->size - (block + 140)); memcpy(block + 136, &rel, 4);
    rel = (int32_t)(game + DIF_WORLD_SIZE_BACK - (uintptr_t)(block + 154)); memcpy(block + 150, &rel, 4);
    uint8_t* skip = block + 0x280;
    float keep = 0.25f * (4.0f / g_difficulty.mapScale) * (4.0f / g_difficulty.mapScale);
    /* The whole share where they are placed (every way the jobs make instances), and sqrt(keep) earlier, where the jobs
     * make their main ones (0xF348E4: one of five ways; for the jobs' time): a 16384 m mission's other ways alone made
     * 7748 locations at sqrt(keep) and the resource hash crashed (0xC176B8). */
    /* Placement keeps DIF_STAMP_PLACE x keep of the jobs' instances (1 % kept 713 of s75t's 71172: bare open land, audit
     * 13); it costs no job time (the overlap tests are the jobs' own, at sqrt(keep)), and the loop stops at the table's
     * limit (kPlaceSkipStub) */
    float place = keep * DIF_STAMP_PLACE;
    uint32_t keepBelow = place >= 1.0f ? 0xFFFFFFFFu : (uint32_t)(place * 4294967295.0f);
    float half = keep >= 1.0f ? 1.0f : sqrtf(keep);
    uint32_t halfBelow = half >= 1.0f ? 0xFFFFFFFFu : (uint32_t)(half * 4294967295.0f);
    /* The thresholds live in the values page and follow the level (WorldWrite): a vanilla level keeps every stamp. The
     * immediates these stubs once held thinned every level of the session, vanilla ones too. */
    g_world.below = (volatile uint32_t*)(block + 0x1180);
    g_world.makeBelow = halfBelow;
    g_world.placeBelow = keepBelow;
    g_world.below[0] = g_world.below[1] = 0xFFFFFFFFu;
    g_world.below[2] = 0xFFFFFFFFu; g_world.below[3] = 0;
    memcpy(skip, kPlaceSkipStub, sizeof kPlaceSkipStub);
    rel = (int32_t)((uint8_t*)&g_world.below[2] - (skip + 7)); memcpy(skip + 3, &rel, 4);
    rel = (int32_t)((uint8_t*)&g_world.below[1] - (skip + 32)); memcpy(skip + 28, &rel, 4);
    rel = (int32_t)((uint8_t*)&g_world.below[3] - (skip + 55)); memcpy(skip + 51, &rel, 4);
    uint8_t* make = block + 0x300;
    memcpy(make, kInstanceSkipStub, sizeof kInstanceSkipStub);
    rel = (int32_t)((uint8_t*)&g_world.below[0] - (make + 45)); memcpy(make + 41, &rel, 4);
    rel = (int32_t)(game + DIF_INSTANCE_REJECT - (uintptr_t)(make + 52)); memcpy(make + 48, &rel, 4);
    rel = (int32_t)(game + DIF_INSTANCE_MADE - (uintptr_t)(make + 67)); memcpy(make + 63, &rel, 4);
    rel = (int32_t)(game + DIF_PLACE_LOOP_END - (uintptr_t)(skip + 48)); memcpy(skip + 44, &rel, 4);
    rel = (int32_t)(game + DIF_PLACE_LOOP_BACK - (uintptr_t)(skip + 69)); memcpy(skip + 65, &rel, 4);
    uint8_t* walkStub = block + 0x380;
    memcpy(walkStub, kObjectWalkStub, sizeof kObjectWalkStub);
    rel = (int32_t)(game + DIF_OBJECT_WALK_NEXT - (uintptr_t)(walkStub + 10)); memcpy(walkStub + 6, &rel, 4);
    rel = (int32_t)(game + DIF_OBJECT_WALK_BACK - (uintptr_t)(walkStub + 21)); memcpy(walkStub + 17, &rel, 4);
    uint8_t* special = block + 0x400;
    memcpy(special, kSpecialCapStub, sizeof kSpecialCapStub);
    rel = (int32_t)(game + DIF_SPECIAL_EMPTY - (uintptr_t)(special + 11)); memcpy(special + 7, &rel, 4);
    rel = (int32_t)(game + DIF_SPECIAL_EMPTY - (uintptr_t)(special + 27)); memcpy(special + 23, &rel, 4);
    rel = (int32_t)(game + DIF_SPECIAL_BODY - (uintptr_t)(special + 32)); memcpy(special + 28, &rel, 4);
    uint8_t* full = block + 0x200;
    g_worldIndexFull = (volatile LONG*)(block + 0x1100);
    memcpy(full, kIndexFullStub, sizeof kIndexFullStub);
    rel = (int32_t)((uint8_t*)g_worldIndexFull - (full + 22)); memcpy(full + 18, &rel, 4);
    rel = (int32_t)((block + 0x1200) - (full + 29)); memcpy(full + 25, &rel, 4);
    int padBand = PadBandStubs(game, block + 0x500), flatBand = FlatBandStub(game, block + 0x5C0), caps = CapStubs(game, block + 0x600), draws = OptionalDrawStub(game, block + 0x660);
    const char* objCapWhy = ObjCapStub(game, block + 0x6C0);
    int hud = g_difficulty.missions > 1 && HudGroupStub(game, block + 0x700);
    DWORD protect;
    VirtualProtect(block, 0x1000, PAGE_EXECUTE_READ, &protect);
    if (objCapWhy == NULL) objCapWhy = ObjCapSite(game, block + 0x6C0);
    if (objCapWhy != NULL) Note("difficulty: %s: an objective keeps one mission's most copies", objCapWhy);
    if (hud) HudGroupSite(game, block + 0x700);
    if (padBand) { const char* why = PadBandSites(game, block + 0x500); if (why != NULL) return why; }
    if (flatBand) { const char* why = FlatBandSite(game, block + 0x5C0); if (why != NULL) return why; }
    if (caps) { const char* why = CapSites(game, block + 0x600); if (why != NULL) return why; }
    if (draws) { const char* why = OptionalDrawSite(game, block + 0x660); if (why != NULL) return why; }
    const float* fields[5] = {&g_world.values->half, &g_world.values->full, &g_world.values->inverse, &g_world.values->negHalf, &g_world.values->negFull};
    for (size_t i = 0; i < sizeof kWorldSites / sizeof kWorldSites[0]; i++)
    {
        const WorldSite* s = &kWorldSites[i];
        uintptr_t end = game + s->rva + s->length;
        rel = (int32_t)((intptr_t)(uintptr_t)fields[s->which] - (intptr_t)end);
        if (ApiWrite(end - 4, &rel, 4)) return "a reader of the world's size could not be pointed at the larger world";
    }
    rel = (int32_t)((intptr_t)(uintptr_t)&g_world.values->clearing - (intptr_t)(game + DIF_CLEARING_RANDOM + 8));
    if (ApiWrite(game + DIF_CLEARING_RANDOM + 4, &rel, 4)) return "the clearing points could not follow the larger world";
    const float* gate[2] = {&g_world.values->gateHigh, &g_world.values->gateLow};
    for (int i = 0; i < 2; i++)
    {
        rel = (int32_t)((intptr_t)(uintptr_t)gate[i] - (intptr_t)(game + kWorldGate[i].rva + 8));
        if (ApiWrite(game + kWorldGate[i].rva + 4, &rel, 4)) return "the director's spawn bounds could not follow the larger world";
    }
    for (int i = 0; i < 4; i++)
    {
        const float* bound = kWorldDropBounds[i].high ? &g_world.values->dropHigh : &g_world.values->dropLow;
        rel = (int32_t)((intptr_t)(uintptr_t)bound - (intptr_t)(game + kWorldDropBounds[i].rva + 8));
        if (ApiWrite(game + kWorldDropBounds[i].rva + 4, &rel, 4)) return "the drop bounds could not follow the larger world";
    }
    if (ApiCodeMatches(game + 0x6AA0CAu, "F3 44 0F 10 35 31 DD D1 01"))
    {
        rel = (int32_t)((intptr_t)(uintptr_t)&g_world.values->maskSpan - (intptr_t)(game + 0x6AA0CAu + 9));
        if (ApiWrite(game + 0x6AA0CAu + 5, &rel, 4)) return "a stratagem's payload clearance could not follow the larger world";
    }
    else Note("difficulty: a stratagem's payload clearance is not the reviewed one: vehicles on the larger world keep a vanilla clearance");
    if (ApiCodeMatches(game + 0xF5CEE0u, "F3 44 0F 10 05 9F AC 46 01"))
    {
        rel = (int32_t)((intptr_t)(uintptr_t)&g_world.values->quarter - (intptr_t)(game + 0xF5CEE0u + 9));
        if (ApiWrite(game + 0xF5CEE0u + 5, &rel, 4)) return "the earthquakes' bounds could not follow the larger world";
    }
    else Note("difficulty: the earthquakes' bounds are not the reviewed ones: their sources stay near the world's centre");
    if (ApiCodeMatches(game + 0x7ED57Bu, "F3 0F 10 0D 81 A8 BD 01") && ApiCodeMatches(game + 0x7ED58Cu, "F3 0F 10 05 48 AE BD 01"))
    {
        rel = (int32_t)((intptr_t)(uintptr_t)&g_world.values->goalHigh - (intptr_t)(game + 0x7ED57Bu + 8));
        if (ApiWrite(game + 0x7ED57Bu + 4, &rel, 4)) return "the enemies' movement goals could not follow the larger world";
        rel = (int32_t)((intptr_t)(uintptr_t)&g_world.values->goalLow - (intptr_t)(game + 0x7ED58Cu + 8));
        if (ApiWrite(game + 0x7ED58Cu + 4, &rel, 4)) return "the enemies' movement goals could not follow the larger world";
    }
    else Note("difficulty: the enemies' movement goal bounds are not the reviewed ones: enemies more than 4 km out keep still");
    if (ApiCodeMatches(game + 0x5A01AFu, "F3 0F 10 05 11 7C E2 01") && ApiCodeMatches(game + 0x5A0199u, "F3 0F 10 0D 27 82 E2 01"))
    {
        rel = (int32_t)((intptr_t)(uintptr_t)&g_world.values->motionHigh - (intptr_t)(game + 0x5A01AFu + 8));
        if (ApiWrite(game + 0x5A01AFu + 4, &rel, 4)) return "the units' motion bounds could not follow the larger world";
        rel = (int32_t)((intptr_t)(uintptr_t)&g_world.values->motionLow - (intptr_t)(game + 0x5A0199u + 8));
        if (ApiWrite(game + 0x5A0199u + 4, &rel, 4)) return "the units' motion bounds could not follow the larger world";
    }
    else Note("difficulty: the units' motion bounds are not the reviewed ones: units more than 1900 m out on any axis are removed");
    InstallBeaconDepth(game);
    uint8_t jump[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    rel = DifficultyJumpRel(game + DIF_INDEX_FULL, (uintptr_t)full); memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + DIF_INDEX_FULL, jump, 5)) return "the stamp index's full case could not be guarded";
    if (ApiWrite(game + DIF_RECORDS_FULL, kRecordsFullSlot0, 3)) return "the destruction records' full case could not be guarded";
    for (int i = 0; i < 3; i++)
        if (ApiWrite(game + kPlaceBuffer[i].rva + kPlaceBuffer[i].at, &kPlaceBuffer[i].value, kPlaceBuffer[i].width))
            return "the stamp instances' buffer could not be enlarged";
    if (ApiWrite(game + DIF_HIVE_SEARCH, kHiveSearch, 49)) return "the Hive Lord graph's labelling could not be made linear";
    if (ApiWrite(game + DIF_TABLE_FULL, kTableFullSkip, 7)) return "the global hash table's full case could not be guarded";
    if (ApiWrite(game + DIF_TABLE2_FULL, kTable2Slot0, 3)) return "the entity table's full case could not be guarded";
    uint8_t walkJump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    rel = DifficultyJumpRel(game + DIF_OBJECT_WALK, (uintptr_t)walkStub); memcpy(walkJump + 1, &rel, 4);
    if (ApiWrite(game + DIF_OBJECT_WALK, walkJump, 8)) return "the level object walk could not be guarded";
    uint8_t makeJump[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    rel = DifficultyJumpRel(game + DIF_INSTANCE_MAKE, (uintptr_t)make); memcpy(makeJump + 1, &rel, 4);
    if (ApiWrite(game + DIF_INSTANCE_MAKE, makeJump, 10)) return "the stamp instances could not be thinned";
    uint8_t skipJump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    rel = DifficultyJumpRel(game + DIF_PLACE_LOOP, (uintptr_t)skip); memcpy(skipJump + 1, &rel, 4);
    if (ApiWrite(game + DIF_PLACE_LOOP, skipJump, 7)) return "the stamp placement could not be thinned";
    uint8_t specialJump[16] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    rel = DifficultyJumpRel(game + DIF_SPECIAL_LOOP, (uintptr_t)special); memcpy(specialJump + 1, &rel, 4);
    if (ApiWrite(game + DIF_SPECIAL_LOOP, specialJump, 16)) return "op 0x76's cluster centres could not be bounded";
    Note("difficulty %s: extra large maps: %.1f%% of the weight rules' stamps placed (made %.1f%%; on its levels only)", g_difficulty.name,
        (double)(place >= 1.0f ? 100.0f : place * 100.0f), (double)(half >= 1.0f ? 100.0f : half * 100.0f));
    for (size_t i = 0; i < sizeof kWorldPasses / sizeof kWorldPasses[0]; i++)
        LoaderHook("extra large maps' pass diagnostic", game + kWorldPasses[i].rva, kWorldPasses[i].detour, (void**)kWorldPasses[i].original);
    LoaderHook("extra large maps' op diagnostic", game + 0xC14950u, (void*)WorldDispatch, (void**)&g_worldDispatch);
    KindFilterResearchInstall(game);
    MemProfInstall();
    TerrainDiagInstall(game);
    TerrainInstall(game);
    if (scale > 1) RenderCapacityInstall();
    uintptr_t exe = ApiModuleBase("helldivers2.exe", NULL);
    if (ApiCodeMatches(exe + DIF_OBJECT_CALL, kObjectCallPin) && ApiCodeMatches(exe + DIF_OBJECT_LOOKUP, kObjectLookupPin))
    {
        g_worldLookup = (WorldLookupFn)(exe + DIF_OBJECT_LOOKUP);
        LoaderHook("extra large maps' stale object guard", exe + DIF_OBJECT_CALL, (void*)WorldObject, (void**)&g_worldObject);
    }
    else Note("difficulty: extra large maps: the engine's object call is not the reviewed one; stale handles are not guarded");
    if (ApiCodeMatches(exe + 0xDFA60u, kLayerSnapPin))
        LoaderHook("extra large maps' layer heightfield diagnostic", exe + 0xDFA60u, (void*)WorldLayerSnap, (void**)&g_layerSnap);
    if (ApiCodeMatches(exe + 0xD5B10u, kLayerSamplePin))
        LoaderHook("extra large maps' layer height sampler", exe + 0xD5B10u, (void*)WorldLayerSample, (void**)&g_layerSample);
    for (int i = 0; i < 4; i++)
        if (ApiCodeMatches(exe + kSpawners[i].rva, kSpawners[i].pin))
            LoaderHook("extra large maps' spawn diagnostic", exe + kSpawners[i].rva, kSpawnDetours[i], (void**)&g_worldSpawn[i]);
    if (ApiCodeMatches(game + 0xF322F0u, kPlaceJobPin))
        LoaderHook("extra large maps' stamp job timing", game + 0xF322F0u, (void*)WorldPlaceJob, (void**)&g_placeJob);
    InstallWorldPlan(game); /* it notes its own refusal; the vanilla pass still runs then */
    const char* overlap = InstallWorldOverlap(game);
    if (overlap != NULL) Note("difficulty: extra large maps: the stamp overlap grid is not used: %s", overlap);
    const char* poisson = InstallWorldPoisson(game);
    if (poisson != NULL) Note("difficulty: extra large maps: the tiled Poisson-disk sampler is not used: %s", poisson);
    ScatterBakeInstall(); /* difficulty_world_diag.c: the far-scatter bake's views without grass (op 0x4A) */
    const char* spread = InstallSpreadSort(game);
    if (spread != NULL) Note("difficulty: extra large maps: %s", spread);
    const char* convoy = InstallConvoyGuard(game);
    if (convoy != NULL) Note("difficulty: extra large maps: %s", convoy);
    const char* lists = InstallMissionListGuards(game);
    if (lists != NULL) Note("difficulty: extra large maps: %s", lists);
    if (ApiCodeMatches(game + 0xF70F20u, "48 89 54 24 10 41 56 48 81 EC A0 00 00 00 48 8B 81 C8 02 00 00"))
        LoaderHook("an operation's missions' objective list", game + 0xF70F20u, (void*)WorldObjectiveFill, (void**)&g_objectiveFill);
    else
        Note("difficulty: extra large maps: the map's objective list fill is not the reviewed one; an operation keeps the selection's objectives");
    if (ApiCodeMatches(game + 0xF8EC70u, "48 8B C4 48 89 58 20 44 89 40 18 48 89 50 10 48 89 48 08"))
        LoaderHook("extra large maps' nature sub-locations", game + 0xF8EC70u, (void*)NatureSubLocations, (void**)&g_natureSubFn);
    else
        Note("difficulty: extra large maps: the objectives' sub-location maker is not the reviewed one; sub-locations roll any kind");
    const char* garrison = InstallGarrisonGuard(game);
    if (garrison != NULL) Note("difficulty: extra large maps: %s", garrison);
    const char* tasks = InstallTaskGuard(game);
    if (tasks != NULL) Note("difficulty: extra large maps: %s", tasks);
    const char* samples = InstallSampleGroups(game);
    if (samples != NULL) Note("difficulty: extra large maps: %s", samples);
    const char* alarms = InstallAlarmGuard(game);
    if (alarms != NULL) Note("difficulty: extra large maps: %s", alarms);
    const char* attach = InstallAttachGuard(game);
    if (attach != NULL) Note("difficulty: extra large maps: %s", attach);
    const char* routeWidth = InstallRouteWidth(game);
    if (routeWidth != NULL) Note("difficulty: extra large maps: %s", routeWidth);
    const char* towns = InstallTowns(game);
    if (ApiCodeMatches(game + 0xFB8F08u, "41 0F 28 C4 F3 0F 59 C3"))
    {
        /* movaps xmm0, xmm12; mulss xmm0, xmm3; mulss xmm0, [rip+factor]; jmp 0xFB8F10 (factor at +0x20) */
        uint8_t* zone = AllocateNearGame(game, 0x40);
        if (zone != NULL)
        {
            static const uint8_t code[21] = {0x41, 0x0F, 0x28, 0xC4, 0xF3, 0x0F, 0x59, 0xC3, 0xF3, 0x0F, 0x59, 0x05, 0, 0, 0, 0, 0xE9, 0, 0, 0, 0};
            memcpy(zone, code, sizeof code);
            int32_t r32 = (int32_t)((zone + 0x20) - (zone + 16)); memcpy(zone + 12, &r32, 4);
            r32 = (int32_t)(game + 0xFB8F10u - (uintptr_t)(zone + 21)); memcpy(zone + 17, &r32, 4);
            float one = 1.0f; memcpy(zone + 0x20, &one, 4);
            DWORD old;
            VirtualProtect(zone, 0x40, PAGE_EXECUTE_READWRITE, &old);
            FlushInstructionCache(GetCurrentProcess(), zone, 0x40);
            uint8_t patch[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
            r32 = DifficultyJumpRel(game + 0xFB8F08u, (uintptr_t)zone); memcpy(patch + 1, &r32, 4);
            if (ApiWrite(game + 0xFB8F08u, patch, 8) == 0) g_cityZone = (volatile float*)(zone + 0x20);
            else Note("difficulty: extra large maps: the city zone template's square could not be written; city maps keep the stretched one");
        }
    }
    else Note("difficulty: extra large maps: the city zone template's square is not the reviewed one; city maps keep the stretched one");
    if (ApiCodeMatches(game + 0xFB9610u, "48 8B C4 53 56 57 48 81 EC D0 00 00 00"))
        LoaderHook("extra large maps' city frame (template)", game + 0xFB9610u, (void*)WorldSiteTemplate, (void**)&g_siteTemplate);
    if (ApiCodeMatches(game + 0xC129B0u, "48 8B C4 44 89 48 20 F3 0F 11 50 18 53 55 56 57"))
        LoaderHook("extra large maps' city frame (grid)", game + 0xC129B0u, (void*)WorldSiteGrid, (void**)&g_siteGrid);
    if (towns != NULL) Note("difficulty: extra large maps: %s", towns);
    const char* gates = InstallCityGates(game);
    if (gates != NULL) Note("difficulty: extra large maps: %s", gates);
    const char* painted = InstallRouteCellLists(game);
    Note("difficulty: extra large maps: %s", painted != NULL ? painted : "op 0x7C's painted route-cell list and stack sized cells + locations (was cells)");
    LwInstallWallReach(game);
    uint8_t* cull = exe != 0 ? FindNearGame(exe, 0x1000, MEM_COMMIT | MEM_RESERVE) : NULL;
    if (cull == NULL) return "no memory within 2 GB of the exe for the occlusion test's guard";
    memcpy(cull, kCullStub, sizeof kCullStub);
    rel = (int32_t)(exe + DIF_CULL_VISIBLE - (uintptr_t)(cull + 16)); memcpy(cull + 12, &rel, 4);
    rel = (int32_t)(exe + DIF_CULL_VISIBLE - (uintptr_t)(cull + 27)); memcpy(cull + 23, &rel, 4);
    rel = (int32_t)(exe + DIF_CULL_BACK - (uintptr_t)(cull + 32)); memcpy(cull + 28, &rel, 4);
    VirtualProtect(cull, 0x1000, PAGE_EXECUTE_READ, &protect);
    FlushInstructionCache(GetCurrentProcess(), cull, 0x1000);
    uint8_t cullJump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    rel = (int32_t)((uintptr_t)cull - (exe + DIF_CULL_SITE + 5)); memcpy(cullJump + 1, &rel, 4);
    if (ApiWrite(exe + DIF_CULL_SITE, cullJump, 7)) return "the engine's occlusion test could not be guarded";
    const char* upload = InstallUploadTail(exe);
    if (upload != NULL) Note("difficulty: extra large maps: %s", upload);
    if (ApiCodeMatches(exe + DIF_TREE_CLEAR_A, kTreeClearPin) && ApiCodeMatches(exe + DIF_TREE_CLEAR_B, kTreeClearPin))
    {
        const uintptr_t sites[2] = {DIF_TREE_CLEAR_A, DIF_TREE_CLEAR_B}, backs[2] = {DIF_TREE_CLEAR_A_BACK, DIF_TREE_CLEAR_B_BACK};
        DWORD old;
        VirtualProtect(cull, 0x1000, PAGE_READWRITE, &old);
        for (int i = 0; i < 2; i++)
        {
            uint8_t* stub = cull + 0x100 + 0x40 * i;
            memcpy(stub, kTreeClearStub, sizeof kTreeClearStub);
            rel = (int32_t)(exe + backs[i] - (uintptr_t)(stub + 40)); memcpy(stub + 36, &rel, 4);
        }
        VirtualProtect(cull, 0x1000, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), cull, 0x1000);
        for (int i = 0; i < 2; i++)
        {
            uint8_t treeJump[23];
            memset(treeJump, 0x90, sizeof treeJump);
            treeJump[0] = 0xE9;
            rel = (int32_t)((uintptr_t)(cull + 0x100 + 0x40 * i) - (exe + sites[i] + 5)); memcpy(treeJump + 1, &rel, 4);
            if (ApiWrite(exe + sites[i], treeJump, sizeof treeJump)) return "SpeedTree's cell bitmap could not be guarded";
        }
    }
    else Note("difficulty: extra large maps: SpeedTree's cell bitmap clear is not the reviewed one; it is not guarded");
    rel = DifficultyJumpRel(game + DIF_WORLD_SIZE_SITE, (uintptr_t)block); memcpy(jump + 1, &rel, 4);
    g_world.scale = scale;
    if (ApiWrite(game + DIF_WORLD_SIZE_SITE, jump, 9)) { g_world.scale = 1; return "the level's size could not be made larger"; }
    Note("difficulty %s: extra large maps: a %u m world at map_scale %.2f (%u x 2048 m); %zu readers, %d immediates and the director's bounds follow it",
        g_difficulty.name, 2048u * scale, (double)g_difficulty.mapScale, scale, sizeof kWorldSites / sizeof kWorldSites[0], WORLD_IMMEDIATES);
    return NULL;
}
