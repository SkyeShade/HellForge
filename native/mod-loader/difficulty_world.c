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
 * engine's world size 0xF5DEF9.. and 0xF5F3C1.., the stamp index's world 0xF6E9D0) and 1024.0 (the centre that
 * SpreadLocations and the outposts' spreading measure from, 0xF7DFEF.. and 0xF7EDDC..). */
static const struct { uint32_t rva; uint8_t at; uint32_t value; } kWorldImmediates[11] = {
    {0xF38EDAu, 4, 0x45000000u}, {0xF38EFEu, 4, 0x45000000u}, {0xF5DEF9u, 4, 0x45000000u}, {0xF5DF06u, 4, 0x45000000u},
    {0xF5F3C1u, 4, 0x45000000u}, {0xF5F3CEu, 4, 0x45000000u}, {0xF6E9D0u, 2, 0x45000000u},
    {0xF7DFEFu, 3, 0x44800000u}, {0xF7DFF6u, 3, 0x44800000u}, {0xF7EDDCu, 3, 0x44800000u}, {0xF7EDE3u, 3, 0x44800000u}};
/* What the instructions read (the block's second page, writable): half, full, 1 / full, the gate's +-2000 x scale, and
 * the size the stub stores (0: the table's). */
typedef struct WorldValues { float half, full, inverse, gateHigh, gateLow, size, clearing, negHalf, dropHigh, dropLow, negFull; } WorldValues;
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

/* The scale level 11's map needs: the smallest power of two whose world holds its disk (difficulty_maps.c's radii). */
static uint32_t WorldScaleFor(float mapScale)
{
    if (mapScale <= DIF_WORLD_MAP_SCALE) return 1; /* up to 2 the disk is cut at 1022 m, as it always was */
    float disk = 389.12f * mapScale + 174.0f + 117.0f;
    uint32_t scale = 1;
    while (scale < 16 && disk > 1024.0f * (float)scale - 2.0f) scale *= 2;
    return scale;
}

static void WorldWrite(float scale)
{
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
    if (g_world.below != NULL)
    {
        g_world.below[0] = scale > 1.0f ? g_world.makeBelow : 0xFFFFFFFFu;
        g_world.below[1] = scale > 1.0f ? g_world.placeBelow : 0xFFFFFFFFu;
    }
    for (int i = 0; i < 11; i++)
    {
        float value;
        memcpy(&value, &kWorldImmediates[i].value, 4);
        value *= scale;
        ApiWrite(g_world.game + kWorldImmediates[i].rva + kWorldImmediates[i].at, &value, 4);
    }
    if (g_cityZone != NULL) *g_cityZone = scale > 1.0f ? CITY_FRAME_K / g_difficulty.mapScale : 1.0f;
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
static const uint8_t kPlaceSkipStub[51] = {
    0x8B, 0x47, 0x54,                                                 /*  0 mov eax, [rdi+0x54] (the count) */
    0x69, 0xC0, 0x6B, 0xCA, 0xEB, 0x85,                               /*  3 imul eax, eax, 0x85EBCA6B */
    0x01, 0xF0,                                                       /*  9 add eax, esi (the instance) */
    0x69, 0xC0, 0xB1, 0x79, 0x37, 0x9E,                               /* 11 imul eax, eax, 0x9E3779B1 */
    0x3B, 0x05, 0, 0, 0, 0,                                           /* 17 cmp eax, [rip+placeBelow] (rel32 at 19) */
    0x76, 0x0E,                                                       /* 23 jbe 39 (kept) */
    0xFF, 0xC6,                                                       /* 25 inc esi (skipped) */
    0x8B, 0x47, 0x54,                                                 /* 27 mov eax, [rdi+0x54] */
    0x3B, 0xF0,                                                       /* 30 cmp esi, eax */
    0x72, 0xDE,                                                       /* 32 jb 0 */
    0xE9, 0, 0, 0, 0,                                                 /* 34 jmp 0xF3517C (the loop's end; rel32 at 35) */
    0x48, 0x8B, 0x4F, 0x20,                                           /* 39 mov rcx, [rdi+0x20] */
    0x45, 0x8B, 0xD5,                                                 /* 43 mov r10d, r13d */
    0xE9, 0, 0, 0, 0};                                                /* 46 jmp 0xF35037 (rel32 at 47) */
/* The engine's occlusion test (exe 0x68FAxx) projects a bounding box onto its depth buffer ([rdi+0xB8], width [rdi],
 * height [rdi+4]) and clamps the cell range with minps/maxps, which let a NaN through: cvtps2dq made it 0x80000000, the row
 * loop started at -2^28 and read far before the buffer (a 4096 m mission ended at exe 0x68FD47 while loading; which object
 * projects to NaN is not known yet). With an extra large world a box whose first column or row is negative counts as not
 * occluded (the test's own result 2, 0x68FD65). */
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
static void WorldPassNote(const char* name, uint32_t before[2], uint64_t start)
{
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
            Note("difficulty: an operation's %u missions on one map: points of interest x%u (%u quota entries, %u category caps)%s", m, m, nq, nc,
                scaled ? "" : "; the region's settings could not be written, so its counts stay");
    }
    uint64_t r = WorldPassExplorationTimed(a, b, c, e);
    if (scaled)
    {
        for (uint32_t i = 0; i < nq; i++) memcpy((void*)(uintptr_t)(quotas + 0x14ull * i + 4), saveQ[i], 8);
        for (uint32_t i = 0; i < nc; i++) memcpy((void*)(uintptr_t)(caps + 8ull * i + 4), &saveC[i], 4);
    }
    ReleaseSRWLockExclusive(&g_explorationLock);
    return r;
}
/* The map's objective list S is filled at generation setup (0xF7166B -> 0xF70F20: rcx the level, r8 the mission row,
 * r9 the descriptor's ids (level+0x110), the 5th argument their count (+0x10D), nine more through to the 14th). The
 * descriptor holds 32 ids at most (its tail follows at +0xA0; objcap.md), and the selection is held at the objective
 * list's room, so an operation's three missions on one map got fewer optional objectives than three missions have. Here
 * the list gets more: repeats of the descriptor's optional ids (class 3 in the base row) in their order, until the
 * optional count reaches each mission's x the missions (g_operationOptional) or the list is full. Only the descriptor
 * decides it, so every peer fills the same list; repeats are what the selection itself makes (count[i] copies). */
typedef uint64_t (*ObjectiveFillFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t);
static ObjectiveFillFn g_objectiveFill;
static uint64_t WorldObjectiveFill(uint64_t level, uint64_t b, uint64_t row, uint64_t ids, uint64_t count, uint64_t a6, uint64_t a7, uint64_t a8,
    uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14)
{
    uint32_t n = (uint32_t)count, list[64];
    uint8_t difficulty = 0;
    uint16_t r = (uint16_t)row;
    if (g_difficulty.missions > 1 && g_operationOptional != 0 && g_difficultyGame != 0 && ids != 0 && n > 0 && n <= 32 && r < DIF_MISSION_ROWS &&
        g_objectiveListPlaces > n && g_objectiveListPlaces <= 64 && !SafeRead(level + 0xF9, &difficulty, 1) && difficulty == DIF_VANILLA + 1 &&
        !SafeRead(ids, list, 4ull * n))
    {
        uintptr_t base = g_difficultyGame + DIF_MISSIONS + (uintptr_t)r * DIF_MISSION_ROW;
        uint32_t optional[32], k = 0, total = n;
        for (uint32_t i = 0; i < n; i++)
            if (RowClass(base, list[i]) == 3) optional[k++] = list[i];
        uint32_t have = k;
        for (uint32_t i = 0; k > 0 && have < g_operationOptional && total < g_objectiveListPlaces; i++, have++) list[total++] = optional[i % k];
        if (total > n)
        {
            static LONG noted;
            if (InterlockedIncrement(&noted) <= 4)
                Note("difficulty: an operation's %u missions on one map: optional objectives %u -> %u of %u on the map's objective list (%u -> %u of %u places)",
                    g_difficulty.missions, k, k + (total - n), g_operationOptional, n, total, g_objectiveListPlaces);
            return g_objectiveFill(level, b, row, (uint64_t)(uintptr_t)list, total, a6, a7, a8, a9, a10, a11, a12, a13, a14);
        }
    }
    return g_objectiveFill(level, b, row, ids, count, a6, a7, a8, a9, a10, a11, a12, a13, a14);
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
static uint64_t WorldPassClusters(uint64_t a, uint64_t b, uint64_t c, uint64_t e)
{
    AcquireSRWLockExclusive(&g_clusterLock);
    int small = g_clusterPer != NULL && g_world.current > 1.0f;
    if (small) { g_clusterPer[0] = 5; g_clusterPer[1] = 34; }
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
 * that thread while the pass runs resumes there with g_tileFault set. The CPU passes work on the tile's copy of the
 * generator block only, so the level's data is untouched; the tile is abandoned and the level goes on. */
static CONTEXT g_tileGuardContext;
static volatile DWORD g_tileGuardThread;
static volatile LONG g_tileFault;
static volatile uintptr_t g_tileFaultAt, g_tileFaultData;

static LONG CALLBACK WorldTileFault(EXCEPTION_POINTERS* info)
{
    if (g_tileGuardThread == 0 || g_tileGuardThread != GetCurrentThreadId()) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
        return EXCEPTION_CONTINUE_SEARCH;
    g_tileGuardThread = 0;
    g_tileFaultAt = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
    g_tileFaultData = info->ExceptionRecord->NumberParameters > 1 ? (uintptr_t)info->ExceptionRecord->ExceptionInformation[1] : 0;
    g_tileFault = (LONG)code;
    *info->ContextRecord = g_tileGuardContext;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* One tile step through the dispatcher until its index moves (or `limit` calls), guarded: 0, or the fault's code. */
static __attribute__((noinline)) LONG WorldTileGuarded(uint8_t* record, uint64_t context, volatile uint32_t* index, uint8_t* phase,
    uint64_t g, uint64_t h, int limit, volatile int* calls)
{
    static int handler;
    if (!handler) { AddVectoredExceptionHandler(1, WorldTileFault); handler = 1; }
    g_tileFault = 0;
    RtlCaptureContext(&g_tileGuardContext);
    if (g_tileFault != 0) return g_tileFault; /* resumed here from WorldTileFault */
    g_tileGuardThread = GetCurrentThreadId();
    while (*index == 0 && *calls < limit)
    {
        g_worldDispatch((uint64_t)record, context, (uint64_t)index, 1, (uint64_t)phase, 0, g, h);
        (*calls)++;
        if (*index == 0 && limit > 1) Sleep(1);
    }
    g_tileGuardThread = 0;
    return 0;
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
                op, (unsigned long)fault, (unsigned long long)(g_tileFaultAt - (uintptr_t)GetModuleHandleW(L"game.dll")),
                (unsigned long long)g_tileFaultData);
            TerrainDiagTileFault(op, g_tileFaultAt, g_tileFaultData);
            return 0;
        }
        QueryPerformanceCounter(&t1);
        ran++;
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
            kTileGpuOps[g_tileGpuStep], (unsigned long)fault, (unsigned long long)(g_tileFaultAt - (uintptr_t)GetModuleHandleW(L"game.dll")));
        TerrainDiagTileFault(kTileGpuOps[g_tileGpuStep], g_tileFaultAt, g_tileFaultData);
        g_tileGpuDone = 1;
        return 0;
    }
    g_tileGpuCalls++;
    g_tileGpuFrames++;
    if (index != 0)
    {
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
static uint64_t WorldSiteTemplate(uint64_t arena, float scale, uint64_t kind, uint64_t region, uint64_t box, uint64_t rng, uint32_t* count)
{
    if (g_world.current <= 1.0f) return g_siteTemplate(arena, scale, kind, region, box, rng, count);
    /* the template in template space (0..2048 about 1024, x rect width / 2048): a vanilla rect about the given one's centre */
    float given[4] = {0, 0, 0, 0};
    SafeRead(box, given, 16);
    const float cx = 0.5f * (given[0] + given[2]), cy = 0.5f * (given[1] + given[3]), r = CITY_FRAME_K * CITY_VANILLA_KEEP;
    float frame[4] = {cx - r, cy - r, cx + r, cy + r};
    uint64_t points = g_siteTemplate(arena, scale, kind, region, (uint64_t)(uintptr_t)frame, rng, count);
    char how[96];
    snprintf(how, sizeof how, "scale %.3f, kind %u, frame (%.0f, %.0f)..(%.0f, %.0f)", (double)scale, (unsigned)(kind & 0xFF), (double)frame[0], (double)frame[1], (double)frame[2], (double)frame[3]);
    SiteNote("template 0xFB9610", points, count, how);
    CityFrameFilter(points, count, cx, cy);
    SiteNote("template 0xFB9610 in the frame's disk", points, count, how);
    return points;
}
static uint64_t WorldSiteGrid(float a, int32_t b, float c, int32_t d, float e, uint64_t arena, float spacing, uint64_t box, uint64_t rng, uint32_t* count)
{
    if (g_world.current <= 1.0f) return g_siteGrid(a, b, c, d, e, arena, spacing, box, rng, count);
    /* the grid at the structured type's own spacing (Z+0x67C, Z = [S+0xB0]: not the larger maps' cell factor) over the frame */
    float given[4] = {0, 0, 0, 0}, own = 0.0f;
    uint64_t Z = 0;
    SafeRead(box, given, 16);
    if (!SafeRead(g_worldLevel + 0x2D20 + 0xB0, &Z, 8) && Z != 0 && !SafeRead(Z + 0x67C, &own, 4) && own > 1.0f && own < 1000.0f) spacing = own * CITY_FRAME_K;
    const float cx = 0.5f * (given[0] + given[2]), cy = 0.5f * (given[1] + given[3]), r = CITY_FRAME_K * CITY_VANILLA_KEEP;
    float frame[4] = {cx - r, cy - r, cx + r, cy + r};
    uint64_t points = g_siteGrid(a, b, c, d, e, arena, spacing, (uint64_t)(uintptr_t)frame, rng, count);
    char how[128];
    snprintf(how, sizeof how, "spacing %.1f, %g %d %g %d %g, frame (%.0f, %.0f)..(%.0f, %.0f)", (double)spacing, (double)a, b, (double)c, d, (double)e,
        (double)frame[0], (double)frame[1], (double)frame[2], (double)frame[3]);
    SiteNote("grid 0xC129B0", points, count, how);
    CityFrameFilter(points, count, cx, cy);
    SiteNote("grid 0xC129B0 in the frame's disk", points, count, how);
    return points;
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

static uint64_t WorldDispatch(uint64_t records, uint64_t b, uint64_t index, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    uint32_t diagAt, diagOp;
    TerrainDiagBefore(records, index, (uint32_t)d, &diagAt, &diagOp);
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
    if (vanilla2B && vanillaOp == 0x2B) TownsForget(records + (uint64_t)vanillaAt * 0x19B0u + 8);
    uint64_t r = g_worldDispatch(records, b, index, d, e, f, g, h);
    if (matPatch) TerrainMatPatch(0);
    if (watch && op == 0x2B) TownsAfter(records, at, index, b, g, h, block); /* before TerrainStamps: it saves below[] */
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
    if (op == 0xB9) WorldZoneProbe(records, (uint32_t)d, block);
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
    uint32_t route[3] = {0, 0, 0}; /* vertices of 975, cells of 1024, edges of 1621 */
    SafeRead(block + 0x11A71B4, &route[0], 4); SafeRead(block + 0x11A71C0, &route[1], 4); SafeRead(block + 0x11A71CC, &route[2], 4);
    if (after != before || ms > 100.0 || route[0] > 975 || route[1] > 1024 || route[2] > 1621 || op == 0x3 || op == 0xB3 ||
        op == 0x07 || op == 0x12 || op == 0x75 || op == 0x79 || op == 0xB9 || op == 0xA3)
        Note("difficulty: extra large maps: op 0x%X (record %u) at %.0fx: locations %u -> %u, route vertices %u, cells %u, edges %u, %.1f ms",
            op, at, (double)g_world.current, before, after, route[0], route[1], route[2], ms);
    return r;
}
static const struct { uint32_t rva; void* detour; WorldPassFn* original; } kWorldPasses[] = {
    {0xF767A0u, (void*)WorldPassLocations, &g_passLocations}, {0xF7D780u, (void*)WorldPassDiscovery, &g_passDiscovery},
    {0xF7E250u, (void*)WorldPassOutposts, &g_passOutposts}, {0xF83EA0u, (void*)WorldPassValidSmall, &g_passValidSmall},
    {0xF84380u, (void*)WorldPassSmall, &g_passSmall}, {0xF77F30u, (void*)WorldPassBorder, &g_passBorder},
    {0xF82A80u, (void*)WorldPassCrossing, &g_passCrossing}, {0xF7C310u, (void*)WorldPassSpread, &g_passSpread},
    {0xF86DC0u, (void*)WorldPassSatellite, &g_passSatellite}, {0xF40F80u, (void*)WorldPassScatter, &g_passScatter},
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
    /* The job's slice (+0x20, capacity +0x38 records of 0x130, cleared at the job's start, 0xF3253B): records written
     * (a box or a resource) and those of them without a resource (+0x90), which become locations without one. */
    uint64_t slice = 0;
    uint32_t capacity = 0, written = 0, empty = 0, lastWritten = 0, count = 0;
    SafeRead(job + 0x54, &count, 4);
    if (!SafeRead(job + 0x20, &slice, 8) && !SafeRead(job + 0x38, &capacity, 4) && slice != 0 && capacity <= 0x100000)
        for (uint32_t i = 0; i < (count > capacity ? count : capacity) && i < 0x6B000u; i++)
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
    NearOneMipDrop();
    if (g_world.values != NULL && g_world.current != 1.0f) WorldWrite(1.0f);
}

static const char* WorldSitesFailed(uintptr_t game)
{
    if (WorldScaleFor(g_difficulty.mapScale) <= 1) return NULL;
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
    for (int i = 0; i < 11; i++)
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
static const char* InstallTowns(uintptr_t game); /* difficulty_world_towns.c */
static const char* InstallWorld(DifficultyInstall* d)
{
    uint32_t scale = WorldScaleFor(g_difficulty.mapScale);
    if (scale <= 1) return NULL;
    uintptr_t game = d->game;
    uint8_t* block = AllocateNearGame(game, 0x2000);
    if (block == NULL) return "no memory within 2 GB of game.dll for the extra large world";
    g_world.game = game;
    g_world.values = (WorldValues*)(block + 0x1000);
    WorldValues vanilla = {1024.0f, 2048.0f, 1.0f / 2048.0f, 2000.0f, -2000.0f, 0.0f, 2.3283064365386963e-10f, -1024.0f, 999.0f, -999.0f, -2048.0f};
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
    uint32_t keepBelow = keep >= 1.0f ? 0xFFFFFFFFu : (uint32_t)(keep * 4294967295.0f);
    float half = keep >= 1.0f ? 1.0f : sqrtf(keep);
    uint32_t halfBelow = half >= 1.0f ? 0xFFFFFFFFu : (uint32_t)(half * 4294967295.0f);
    /* The thresholds live in the values page and follow the level (WorldWrite): a vanilla level keeps every stamp. The
     * immediates these stubs once held thinned every level of the session, vanilla ones too. */
    g_world.below = (volatile uint32_t*)(block + 0x1180);
    g_world.makeBelow = halfBelow;
    g_world.placeBelow = keepBelow;
    g_world.below[0] = g_world.below[1] = 0xFFFFFFFFu;
    memcpy(skip, kPlaceSkipStub, sizeof kPlaceSkipStub);
    rel = (int32_t)((uint8_t*)&g_world.below[1] - (skip + 23)); memcpy(skip + 19, &rel, 4);
    uint8_t* make = block + 0x300;
    memcpy(make, kInstanceSkipStub, sizeof kInstanceSkipStub);
    rel = (int32_t)((uint8_t*)&g_world.below[0] - (make + 45)); memcpy(make + 41, &rel, 4);
    rel = (int32_t)(game + DIF_INSTANCE_REJECT - (uintptr_t)(make + 52)); memcpy(make + 48, &rel, 4);
    rel = (int32_t)(game + DIF_INSTANCE_MADE - (uintptr_t)(make + 67)); memcpy(make + 63, &rel, 4);
    rel = (int32_t)(game + DIF_PLACE_LOOP_END - (uintptr_t)(skip + 39)); memcpy(skip + 35, &rel, 4);
    rel = (int32_t)(game + DIF_PLACE_LOOP_BACK - (uintptr_t)(skip + 51)); memcpy(skip + 47, &rel, 4);
    uint8_t* walkStub = block + 0x380;
    memcpy(walkStub, kObjectWalkStub, sizeof kObjectWalkStub);
    rel = (int32_t)(game + DIF_OBJECT_WALK_NEXT - (uintptr_t)(walkStub + 10)); memcpy(walkStub + 6, &rel, 4);
    rel = (int32_t)(game + DIF_OBJECT_WALK_BACK - (uintptr_t)(walkStub + 21)); memcpy(walkStub + 17, &rel, 4);
    uint8_t* full = block + 0x200;
    g_worldIndexFull = (volatile LONG*)(block + 0x1100);
    memcpy(full, kIndexFullStub, sizeof kIndexFullStub);
    rel = (int32_t)((uint8_t*)g_worldIndexFull - (full + 22)); memcpy(full + 18, &rel, 4);
    rel = (int32_t)((block + 0x1200) - (full + 29)); memcpy(full + 25, &rel, 4);
    DWORD protect;
    VirtualProtect(block, 0x1000, PAGE_EXECUTE_READ, &protect);
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
    Note("difficulty %s: extra large maps: %.1f%% of the weight rules' stamps placed (on its levels only)", g_difficulty.name, (double)(keep >= 1.0f ? 100.0f : keep * 100.0f));
    for (size_t i = 0; i < sizeof kWorldPasses / sizeof kWorldPasses[0]; i++)
        LoaderHook("extra large maps' pass diagnostic", game + kWorldPasses[i].rva, kWorldPasses[i].detour, (void**)kWorldPasses[i].original);
    LoaderHook("extra large maps' op diagnostic", game + 0xC14950u, (void*)WorldDispatch, (void**)&g_worldDispatch);
    TerrainDiagInstall(game);
    TerrainInstall(game);
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
    const char* convoy = InstallConvoyGuard(game);
    if (convoy != NULL) Note("difficulty: extra large maps: %s", convoy);
    const char* lists = InstallMissionListGuards(game);
    if (lists != NULL) Note("difficulty: extra large maps: %s", lists);
    if (ApiCodeMatches(game + 0xF70F20u, "48 89 54 24 10 41 56 48 81 EC A0 00 00 00 48 8B 81 C8 02 00 00"))
        LoaderHook("an operation's missions' objective list", game + 0xF70F20u, (void*)WorldObjectiveFill, (void**)&g_objectiveFill);
    else
        Note("difficulty: extra large maps: the map's objective list fill is not the reviewed one; an operation keeps the selection's objectives");
    const char* garrison = InstallGarrisonGuard(game);
    if (garrison != NULL) Note("difficulty: extra large maps: %s", garrison);
    const char* tasks = InstallTaskGuard(game);
    if (tasks != NULL) Note("difficulty: extra large maps: %s", tasks);
    const char* samples = InstallSampleGroups(game);
    if (samples != NULL) Note("difficulty: extra large maps: %s", samples);
    const char* alarms = InstallAlarmGuard(game);
    if (alarms != NULL) Note("difficulty: extra large maps: %s", alarms);
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
    Note("difficulty %s: extra large maps: a %u m world at map_scale %.2f (%u x 2048 m); %zu readers, 11 immediates and the director's bounds follow it",
        g_difficulty.name, 2048u * scale, (double)g_difficulty.mapScale, scale, sizeof kWorldSites / sizeof kWorldSites[0]);
    return NULL;
}
