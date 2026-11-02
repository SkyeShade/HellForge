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
/* The immediates: {rva, offset of the 32-bit value, vanilla value}: 2048.0 (map space clamps 0xF38EDA/0xF38EFE, the
 * engine's world size 0xF5DEF9.. and 0xF5F3C1.., the stamp index's world 0xF6E9D0) and 1024.0 (the centre that
 * SpreadLocations and the outposts' spreading measure from, 0xF7DFEF.. and 0xF7EDDC..). */
static const struct { uint32_t rva; uint8_t at; uint32_t value; } kWorldImmediates[11] = {
    {0xF38EDAu, 4, 0x45000000u}, {0xF38EFEu, 4, 0x45000000u}, {0xF5DEF9u, 4, 0x45000000u}, {0xF5DF06u, 4, 0x45000000u},
    {0xF5F3C1u, 4, 0x45000000u}, {0xF5F3CEu, 4, 0x45000000u}, {0xF6E9D0u, 2, 0x45000000u},
    {0xF7DFEFu, 3, 0x44800000u}, {0xF7DFF6u, 3, 0x44800000u}, {0xF7EDDCu, 3, 0x44800000u}, {0xF7EDE3u, 3, 0x44800000u}};
/* What the instructions read (the block's second page, writable): half, full, 1 / full, the gate's +-2000 x scale, and
 * the size the stub stores (0: the table's). */
typedef struct WorldValues { float half, full, inverse, gateHigh, gateLow, size, clearing; } WorldValues;
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
    uintptr_t game;
    volatile LONG levels, switches;
} g_world = {1, 1.0f};

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
    v->full = 2048.0f * scale;
    v->inverse = 1.0f / (2048.0f * scale);
    v->gateHigh = 2000.0f * scale;
    v->gateLow = -2000.0f * scale;
    v->clearing = 2.3283064365386963e-10f * scale * scale;
    for (int i = 0; i < 11; i++)
    {
        float value;
        memcpy(&value, &kWorldImmediates[i].value, 4);
        value *= scale;
        ApiWrite(g_world.game + kWorldImmediates[i].rva + kWorldImmediates[i].at, &value, 4);
    }
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
static const uint8_t kInstanceSkipStub[66] = {
    0x8B, 0x45, 0x48,                                                 /*  0 mov eax, [rbp+0x48] */
    0x69, 0xC0, 0xB1, 0x79, 0x37, 0x9E,                               /*  3 imul eax, eax, 0x9E3779B1 */
    0x03, 0x45, 0x68,                                                 /*  9 add eax, [rbp+0x68] */
    0x69, 0xC0, 0x6B, 0xCA, 0xEB, 0x85,                               /* 12 imul eax, eax, 0x85EBCA6B */
    0x03, 0x85, 0xA4, 0x00, 0x00, 0x00,                               /* 18 add eax, [rbp+0xa4] */
    0x69, 0xC0, 0x35, 0xAE, 0xB2, 0xC2,                               /* 24 imul eax, eax, 0xC2B2AE35 */
    0x03, 0x45, 0x84,                                                 /* 30 add eax, [rbp-0x7c] */
    0x69, 0xC0, 0xB1, 0x79, 0x37, 0x9E,                               /* 33 imul eax, eax, 0x9E3779B1 */
    0x3D, 0, 0, 0, 0,                                                 /* 39 cmp eax, keep x 2^32 (at 40) */
    0x76, 0x05,                                                       /* 44 jbe 51 */
    0xE9, 0, 0, 0, 0,                                                 /* 46 jmp 0xF34899 (rel32 at 47) */
    0x8B, 0x45, 0x84,                                                 /* 51 mov eax, [rbp-0x7c] */
    0x48, 0x8B, 0xB5, 0xB8, 0x00, 0x00, 0x00,                         /* 54 mov rsi, [rbp+0xb8] */
    0xE9, 0, 0, 0, 0};                                                /* 61 jmp 0xF348EE (rel32 at 62) */
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
static const uint8_t kPlaceSkipStub[50] = {
    0x8B, 0x47, 0x54,                                                 /*  0 mov eax, [rdi+0x54] (the count) */
    0x69, 0xC0, 0x6B, 0xCA, 0xEB, 0x85,                               /*  3 imul eax, eax, 0x85EBCA6B */
    0x01, 0xF0,                                                       /*  9 add eax, esi (the instance) */
    0x69, 0xC0, 0xB1, 0x79, 0x37, 0x9E,                               /* 11 imul eax, eax, 0x9E3779B1 */
    0x3D, 0, 0, 0, 0,                                                 /* 17 cmp eax, keep x 2^32 (at 18) */
    0x76, 0x0E,                                                       /* 22 jbe 38 (kept) */
    0xFF, 0xC6,                                                       /* 24 inc esi (skipped) */
    0x8B, 0x47, 0x54,                                                 /* 26 mov eax, [rdi+0x54] */
    0x3B, 0xF0,                                                       /* 29 cmp esi, eax */
    0x72, 0xDF,                                                       /* 31 jb 0 */
    0xE9, 0, 0, 0, 0,                                                 /* 33 jmp 0xF3517C (the loop's end; rel32 at 34) */
    0x48, 0x8B, 0x4F, 0x20,                                           /* 38 mov rcx, [rdi+0x20] */
    0x45, 0x8B, 0xD5,                                                 /* 42 mov r10d, r13d */
    0xE9, 0, 0, 0, 0};                                                /* 45 jmp 0xF35037 (rel32 at 46) */
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
WORLD_PASS(Exploration, 0xF95900u, "GenerateExplorationLocations")
WORLD_PASS(Voronoi, 0xF3F8E0u, "GenerateVoronoi")
/* The step dispatcher (0xC14950: rcx the pass records, 0x19B0 each, op at +0; r8 the current index; two stack arguments):
 * every op of an extra large level that adds locations or takes over 100 ms. */
typedef uint64_t (*WorldDispatchFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static WorldDispatchFn g_worldDispatch;
/* Bumped at every level init and whenever an extra large level's dispatcher moves to another pass record: the stamp
 * overlap grid (difficulty_world_overlap.c) keeps its per-buffer grids for one run of a pass only, so a later op 0x2B
 * whose arena buffer lands at the same address starts from an empty grid. */
static volatile LONG g_worldOpRun;
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

static uint64_t WorldDispatch(uint64_t records, uint64_t b, uint64_t index, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    if (g_heightCopy != NULL)
    {
        uint32_t i = 0, o = 0;
        if (!SafeRead(index, &i, 4) && !SafeRead(records + (uint64_t)i * 0x19B0u, &o, 4) && o == 0x6D) WorldDumpHeight();
    }
    uint32_t at = 0, op = 0xFFFFFFFFu, before = 0, after = 0;
    uintptr_t block = 0;
    int watch = g_world.current > 1.0f && g_worldLevel != 0 && !SafeRead(g_worldLevel + 0x2C8, &block, 8) && block != 0 &&
        !SafeRead(index, &at, 4) && !SafeRead(records + (uint64_t)at * 0x19B0u, &op, 4) && !SafeRead(block + 0x11A7108, &before, 4);
    if (watch && at != g_worldOpAt) { g_worldOpAt = at; InterlockedIncrement(&g_worldOpRun); }
    LARGE_INTEGER start, now, frequency;
    static LONG depth; /* the dispatcher on the generating thread only; a nested op is not profiled on its own */
    /* Off: under Wine, SuspendThread/ResumeThread are wineserver round trips, and suspending the generating thread
     * every millisecond kept it parked (generation stalled). Hot spots are sampled from outside with gdb instead. */
    int profile = 0 && watch && InterlockedIncrement(&depth) == 1;
    if (watch && !profile) InterlockedDecrement(&depth);
    if (profile) WorldProfileBegin(op);
    QueryPerformanceCounter(&start);
    uint32_t vanillaBefore = 0, vanillaOp = 0xFFFFFFFFu, vanillaAt = 0;
    uintptr_t vanillaBlock = 0;
    int vanilla2B = !watch && g_worldLevel != 0 && !SafeRead(index, &vanillaAt, 4) && !SafeRead(records + (uint64_t)vanillaAt * 0x19B0u, &vanillaOp, 4) &&
        vanillaOp == 0x2B && !SafeRead(g_worldLevel + 0x2C8, &vanillaBlock, 8) && vanillaBlock != 0 && !SafeRead(vanillaBlock + 0x11A7108, &vanillaBefore, 4);
    uint64_t r = g_worldDispatch(records, b, index, d, e, f, g, h);
    if (vanilla2B) /* research: a vanilla map's stamp placement, to compare densities with */
    {
        uint32_t vanillaAfter = 0;
        SafeRead(vanillaBlock + 0x11A7108, &vanillaAfter, 4);
        Note("difficulty: extra large maps: op 0x2B (record %u) at %.0fx: locations %u -> %u", vanillaAt, (double)g_world.current, vanillaBefore, vanillaAfter);
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
    if (!watch) return r;
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
    if (after != before || ms > 100.0 || route[0] > 975 || route[1] > 1024 || route[2] > 1621 || op == 0x3 || op == 0xB3)
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
    {0xF82710u, (void*)WorldPassEdge, &g_passEdge}, {0xF95900u, (void*)WorldPassExploration, &g_passExploration},
    {0xF3F8E0u, (void*)WorldPassVoronoi, &g_passVoronoi}};

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
    LARGE_INTEGER start, end, hz;
    QueryPerformanceCounter(&start);
    uint64_t r = g_placeJob(a, b, c, job);
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&hz);
    /* The job's slice (+0x20, capacity +0x38 records of 0x130, cleared at the job's start, 0xF3253B): records written
     * (a box or a resource) and those of them without a resource (+0x90), which become locations without one. */
    uint64_t slice = 0;
    uint32_t capacity = 0, written = 0, empty = 0, lastWritten = 0;
    if (!SafeRead(job + 0x20, &slice, 8) && !SafeRead(job + 0x38, &capacity, 4) && slice != 0 && capacity <= 0x100000)
        for (uint32_t i = 0; i < capacity; i++)
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
    Note("difficulty: extra large maps: op 0x2B job items %u..%u of %u on thread %lu at %.0fx: %.1f ms; slice %u records written (last %u) of %u, %u without a resource",
        range[0], range[1], items, (unsigned long)GetCurrentThreadId(), (double)g_world.current, (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)hz.QuadPart,
        written, lastWritten, capacity, empty);
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
    return g_layerSnap(a, b, c, field, e, f, g, h);
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
    for (int i = 0; i < 11; i++)
    {
        uint32_t value;
        if (SafeRead(game + kWorldImmediates[i].rva + kWorldImmediates[i].at, &value, 4) || value != kWorldImmediates[i].value)
            return "a world size the generator holds is not the reviewed one (an extra large map needs it)";
    }
    return NULL;
}

/* An extra large map's world: the block (stub +0, values +0x1000), the readers pointed at it, the size store through
 * the stub. */
static const char* InstallWorldPlan(uintptr_t game); /* difficulty_world_plan.c: the Hive Lord graph on a grid */
static const char* InstallWorldOverlap(uintptr_t game); /* difficulty_world_overlap.c: the stamp overlap test on a grid */
static const char* InstallWorldPoisson(uintptr_t game); /* difficulty_world_poisson.c: Poisson-disk sets from a periodic tile */
static const char* InstallWorld(DifficultyInstall* d)
{
    uint32_t scale = WorldScaleFor(g_difficulty.mapScale);
    if (scale <= 1) return NULL;
    uintptr_t game = d->game;
    uint8_t* block = AllocateNearGame(game, 0x2000);
    if (block == NULL) return "no memory within 2 GB of game.dll for the extra large world";
    g_world.game = game;
    g_world.values = (WorldValues*)(block + 0x1000);
    WorldValues vanilla = {1024.0f, 2048.0f, 1.0f / 2048.0f, 2000.0f, -2000.0f, 0.0f, 2.3283064365386963e-10f};
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
    memcpy(skip, kPlaceSkipStub, sizeof kPlaceSkipStub);
    memcpy(skip + 18, &keepBelow, 4);
    uint8_t* make = block + 0x300;
    memcpy(make, kInstanceSkipStub, sizeof kInstanceSkipStub);
    memcpy(make + 40, &halfBelow, 4);
    rel = (int32_t)(game + DIF_INSTANCE_REJECT - (uintptr_t)(make + 51)); memcpy(make + 47, &rel, 4);
    rel = (int32_t)(game + DIF_INSTANCE_MADE - (uintptr_t)(make + 66)); memcpy(make + 62, &rel, 4);
    rel = (int32_t)(game + DIF_PLACE_LOOP_END - (uintptr_t)(skip + 38)); memcpy(skip + 34, &rel, 4);
    rel = (int32_t)(game + DIF_PLACE_LOOP_BACK - (uintptr_t)(skip + 50)); memcpy(skip + 46, &rel, 4);
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
    const float* fields[3] = {&g_world.values->half, &g_world.values->full, &g_world.values->inverse};
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
    Note("difficulty %s: extra large maps: %.1f%% of the weight rules' stamps placed", g_difficulty.name, (double)(keep >= 1.0f ? 100.0f : keep * 100.0f));
    for (size_t i = 0; i < sizeof kWorldPasses / sizeof kWorldPasses[0]; i++)
        LoaderHook("extra large maps' pass diagnostic", game + kWorldPasses[i].rva, kWorldPasses[i].detour, (void**)kWorldPasses[i].original);
    LoaderHook("extra large maps' op diagnostic", game + 0xC14950u, (void*)WorldDispatch, (void**)&g_worldDispatch);
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
