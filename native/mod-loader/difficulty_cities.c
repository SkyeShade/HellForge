/* Level 11's larger structured maps and cities (difficulty.c, a map_scale above 1): revoronoi, the vertex buckets, the
 * city locations' flags, the location clusters, the playable area's cull, the vista level, and the city copies of
 * strongholds and objectives. Included by hellforge_modloader.c after difficulty_countdown.c. */

/* Structured maps (map type 2) at 2x. revoronoi (0xFD0E00) takes the nature cell size (+0x18C) for its Poisson disc
 * (0xFD0EF1) and its gap to the kept city sites (0xFD11A5); sub-kind 1 lays a grid at its own spacing (0xF361B4). On a
 * larger map each reads the size grown like the route cells' (cellFactor), so the site count stays level 10's and the
 * unchecked G arrays (975 vertices, 1621 edges) hold. Gate: lg+0x2D7C == the larger disk ([rbp-0x40] / [r14+8] =
 * lg+0x2D20). {site, length, stub, size, disk at, factor rel32 at, quarter rel32 at, back rel32 at}. */
static const uint8_t kReVoronoiSizePin[8] = {0xF3, 0x0F, 0x10, 0x8A, 0x8C, 0x01, 0x00, 0x00};
static const uint8_t kReVoronoiGapPin[11] = {0xF3, 0x0F, 0x10, 0xB1, 0x8C, 0x01, 0x00, 0x00, 0x0F, 0x1F, 0x00};
static const uint8_t kStructuredGridPin[9] = {0xF3, 0x41, 0x0F, 0x10, 0x8A, 0x7C, 0x06, 0x00, 0x00};
static const uint8_t kReVoronoiSizeStub[48] = {
    0xF3, 0x0F, 0x10, 0x8A, 0x8C, 0x01, 0x00, 0x00,                   /*  0 movss xmm1, [rdx+0x18c] */
    0x48, 0x8B, 0x45, 0xC0,                                           /*  8 mov rax, [rbp-0x40] (lg+0x2d20) */
    0x81, 0x78, 0x5C, 0, 0, 0, 0,                                     /* 12 cmp dword [rax+0x5c], disk / 1024 (at 15) */
    0x75, 0x16,                                                       /* 19 jne 43 */
    0xF3, 0x0F, 0x59, 0x0D, 0, 0, 0, 0,                               /* 21 mulss xmm1, [rip+factor x 4] (rel32 at 25) */
    0x66, 0x0F, 0x3A, 0x0A, 0xC9, 0x02,                               /* 29 roundss xmm1, xmm1, ceil */
    0xF3, 0x0F, 0x59, 0x0D, 0, 0, 0, 0,                               /* 35 mulss xmm1, [rip+0.25] (rel32 at 39) */
    0xE9, 0, 0, 0, 0};                                                /* 43 jmp 0xFD0EF9 (rel32 at 44) */
static const uint8_t kReVoronoiGapStub[48] = {
    0xF3, 0x0F, 0x10, 0xB1, 0x8C, 0x01, 0x00, 0x00,                   /*  0 movss xmm6, [rcx+0x18c] */
    0x48, 0x8B, 0x45, 0xC0,                                           /*  8 mov rax, [rbp-0x40] (lg+0x2d20) */
    0x81, 0x78, 0x5C, 0, 0, 0, 0,                                     /* 12 cmp dword [rax+0x5c], disk / 1024 (at 15) */
    0x75, 0x16,                                                       /* 19 jne 43 */
    0xF3, 0x0F, 0x59, 0x35, 0, 0, 0, 0,                               /* 21 mulss xmm6, [rip+factor x 4] (rel32 at 25) */
    0x66, 0x0F, 0x3A, 0x0A, 0xF6, 0x02,                               /* 29 roundss xmm6, xmm6, ceil */
    0xF3, 0x0F, 0x59, 0x35, 0, 0, 0, 0,                               /* 35 mulss xmm6, [rip+0.25] (rel32 at 39) */
    0xE9, 0, 0, 0, 0};                                                /* 43 jmp 0xFD11B0 (rel32 at 44) */
static const uint8_t kStructuredGridStub[49] = {
    0xF3, 0x41, 0x0F, 0x10, 0x8A, 0x7C, 0x06, 0x00, 0x00,             /*  0 movss xmm1, [r10+0x67c] */
    0x49, 0x8B, 0x46, 0x08,                                           /*  9 mov rax, [r14+8] (lg+0x2d20) */
    0x81, 0x78, 0x5C, 0, 0, 0, 0,                                     /* 13 cmp dword [rax+0x5c], disk / 1024 (at 16) */
    0x75, 0x16,                                                       /* 20 jne 44 */
    0xF3, 0x0F, 0x59, 0x0D, 0, 0, 0, 0,                               /* 22 mulss xmm1, [rip+factor x 4] (rel32 at 26) */
    0x66, 0x0F, 0x3A, 0x0A, 0xC9, 0x02,                               /* 30 roundss xmm1, xmm1, ceil */
    0xF3, 0x0F, 0x59, 0x0D, 0, 0, 0, 0,                               /* 36 mulss xmm1, [rip+0.25] (rel32 at 40) */
    0xE9, 0, 0, 0, 0};                                                /* 44 jmp 0xF361BD (rel32 at 45) */
static const struct { uint32_t site; uint8_t length; const uint8_t* pin; const uint8_t* stub; uint8_t size, diskAt, factorAt, quarterAt, backAt; } kReVoronoiStubs[3] = {
    {0xFD0EF1u, 8, kReVoronoiSizePin, kReVoronoiSizeStub, 48, 15, 25, 39, 44},
    {0xFD11A5u, 11, kReVoronoiGapPin, kReVoronoiGapStub, 48, 15, 25, 39, 44},
    {0xF361B4u, 9, kStructuredGridPin, kStructuredGridStub, 49, 16, 26, 40, 45}};
/* FinalizeVoronoi on structured maps (0xFBBB20) welds vertices through a 24 x 24 bucket grid whose blocks come from a
 * pool of 512 (+0xBA60, 0xA08 each, count +0x14CA60), unchecked; a larger map can reach ~530 buckets. The pool gets all
 * 576 in place (its allocation, clears and the count's displacement). {rva, length, value at, pin, value}. */
static const struct { uint32_t rva; uint8_t length, at; uint8_t pin[7]; uint32_t value; } kBucketPool[8] = {
    {0xFBBB58u, 5, 1, {0xBA, 0x68, 0xCA, 0x14, 0x00}, 0x174C68u},                    /* allocation */
    {0xFBBB6Fu, 6, 2, {0x41, 0xB8, 0x20, 0xB8, 0x14, 0x00}, 0x173A20u},              /* clear from +0x1248 */
    {0xFBBBEFu, 6, 2, {0x41, 0xB8, 0x00, 0x10, 0x14, 0x00}, 0x169200u},              /* the pool's clear */
    {0xFBBC10u, 6, 2, {0x89, 0xB3, 0x60, 0xCA, 0x14, 0x00}, 0x174C60u},              /* the count */
    {0xFB9E72u, 6, 2, {0x8B, 0x8E, 0x60, 0xCA, 0x14, 0x00}, 0x174C60u},
    {0xFB9E82u, 6, 2, {0x89, 0x86, 0x60, 0xCA, 0x14, 0x00}, 0x174C60u},
    {0xFB9B82u, 7, 3, {0x41, 0x8B, 0x88, 0x60, 0xCA, 0x14, 0x00}, 0x174C60u},
    {0xFB9B93u, 7, 3, {0x41, 0x89, 0x80, 0x60, 0xCA, 0x14, 0x00}, 0x174C60u}};
/* SpreadLocations_Cities (op 0x8C, 0xFB41B0) flags each important location (G+0x11A715C of 256) in a 128-byte stack
 * array (rbp-0x80) and fails fast past 128 (0xFB43DF / 0xFB4415 -> 0x2088C0C): Hell Forge's objectives and outposts
 * on a larger city map pass it. The array moves to the generation arena (0x400 bytes; its pointer in the old array's
 * first qword, as the height zones' table): the clear allocates it, the three writes and the reader's base go through
 * the pointer, the checks allow 0x400. Every level gets the same flags. */
#define DIF_FLAGS_CLEAR 0xFB4260u  /* 8 x movups [rbp-0x80..-0x10], xmm0 */
#define DIF_FLAGS_CLEAR_BACK 0xFB4280u
#define DIF_FLAGS_READ 0xFB4523u   /* lea rsi, [rbp-0x80] */
static const uint8_t kFlagsClearPin[32] = {0x0F, 0x11, 0x45, 0x80, 0x0F, 0x11, 0x45, 0x90, 0x0F, 0x11, 0x45, 0xA0, 0x0F, 0x11, 0x45, 0xB0,
    0x0F, 0x11, 0x45, 0xC0, 0x0F, 0x11, 0x45, 0xD0, 0x0F, 0x11, 0x45, 0xE0, 0x0F, 0x11, 0x45, 0xF0};
static const uint8_t kFlagsReadPin[4] = {0x48, 0x8D, 0x75, 0x80};
static const uint8_t kFlagsClearStub[48] = {
    0x49, 0x8B, 0x0C, 0x24,                                           /*  0 mov rcx, [r12] (the generation arena) */
    0xBA, 0x00, 0x04, 0x00, 0x00,                                     /*  4 mov edx, 0x400 */
    0x41, 0xB8, 0x08, 0x00, 0x00, 0x00,                               /*  9 mov r8d, 8 */
    0xE8, 0, 0, 0, 0,                                                 /* 15 call 0x173B8D0 (rel32 at 16) */
    0x48, 0x89, 0x45, 0x80,                                           /* 20 mov [rbp-0x80], rax (the flags) */
    0x48, 0x89, 0xC1, 0x31, 0xD2,                                     /* 24 mov rcx, rax; xor edx, edx */
    0x41, 0xB8, 0x00, 0x04, 0x00, 0x00,                               /* 29 mov r8d, 0x400 */
    0xE8, 0, 0, 0, 0,                                                 /* 35 call memset (rel32 at 36) */
    0x0F, 0x57, 0xC0,                                                 /* 40 xorps xmm0, xmm0 (as before the clear) */
    0xE9, 0, 0, 0, 0};                                                /* 43 jmp 0xFB4280 (rel32 at 44) */
/* {site, length, pin, stub, size, back rel32 at, back}: push rax; mov rax, [rbp-0x80]; the write; pop rax; (test al, al) */
static const uint8_t kFlagsWritePin0[5] = {0xC6, 0x44, 0x3D, 0x80, 0x00}; /* mov byte [rbp+rdi-0x80], 0 */
static const uint8_t kFlagsWritePin1[5] = {0xC6, 0x44, 0x35, 0x80, 0x00}; /* mov byte [rbp+rsi-0x80], 0 */
static const uint8_t kFlagsWritePin2[6] = {0x88, 0x4C, 0x35, 0x80, 0x84, 0xC0}; /* mov [rbp+rsi-0x80], cl; test al, al */
static const uint8_t kFlagsWriteStub0[15] = {0x50, 0x48, 0x8B, 0x45, 0x80, 0xC6, 0x04, 0x38, 0x00, 0x58, 0xE9, 0, 0, 0, 0};
static const uint8_t kFlagsWriteStub1[15] = {0x50, 0x48, 0x8B, 0x45, 0x80, 0xC6, 0x04, 0x30, 0x00, 0x58, 0xE9, 0, 0, 0, 0};
static const uint8_t kFlagsWriteStub2[16] = {0x50, 0x48, 0x8B, 0x45, 0x80, 0x88, 0x0C, 0x30, 0x58, 0x84, 0xC0, 0xE9, 0, 0, 0, 0};
static const struct { uint32_t site; uint8_t length; const uint8_t* pin; const uint8_t* stub; uint8_t size, backAt; uint32_t back; } kFlagsWrites[3] = {
    {0xFB43ECu, 5, kFlagsWritePin0, kFlagsWriteStub0, 15, 11, 0xFB43F1u},
    {0xFB4422u, 5, kFlagsWritePin1, kFlagsWriteStub1, 15, 11, 0xFB4427u},
    {0xFB44E7u, 6, kFlagsWritePin2, kFlagsWriteStub2, 16, 12, 0xFB44EDu}};
static const uint32_t kFlagsChecks[2] = {0xFB43DFu, 0xFB4415u}; /* cmp rdi / rsi, 0x80 (imm32 at +3) */
static const uint8_t kFlagsCheckPins[2][7] = {{0x48, 0x81, 0xFF, 0x80, 0x00, 0x00, 0x00}, {0x48, 0x81, 0xFE, 0x80, 0x00, 0x00, 0x00}};
/* GenerateLocationClusters (op 0x18, 0xF79520) groups locations into clusters of 24 (0xE0 each: members +0x10, count
 * +0xD0, kind +0xD8), a few per kind (at most 4, or the settings' range). Its four assignments take the chosen cluster
 * unchecked: 0xF78E20 (via 0xF78A80, -1 when every cluster of the kind is full), 0xF78F30 / 0xF79110 (fewest of a
 * kind, -1 when the kind has none, no capacity check) and 0xF7A262 (same). A level-11 map's extra locations ran past them
 * (crash 0xF78EA5 on a mega-city preview). Each now leaves a location with no cluster with room unclustered (+0xE0 stays
 * 0, as the pass's own retry loop expects) and changes nothing else. {site, length, pin, stub, size, back at, back,
 * skip at, skip}. */
static const uint8_t kClusterPinA[13] = {0x8B, 0xC0, 0x4C, 0x69, 0xC0, 0xE0, 0x00, 0x00, 0x00, 0x4C, 0x03, 0x46, 0x08};
static const uint8_t kClusterPinB[11] = {0x4C, 0x69, 0xC0, 0xE0, 0x00, 0x00, 0x00, 0x4C, 0x03, 0x42, 0x08};
static const uint8_t kClusterPinD[9] = {0x8B, 0xC5, 0x4C, 0x69, 0xC0, 0xE0, 0x00, 0x00, 0x00};
static const uint8_t kClusterStubA[38] = {
    0x83, 0xF8, 0xFF, 0x74, 0x1C,                                     /*  0 cmp eax, -1; je 33 */
    0x8B, 0xC0, 0x4C, 0x69, 0xC0, 0xE0, 0x00, 0x00, 0x00,             /*  5 mov eax, eax; imul r8, rax, 0xe0 */
    0x4C, 0x03, 0x46, 0x08,                                           /* 14 add r8, [rsi+8] */
    0x41, 0x83, 0xB8, 0xD0, 0x00, 0x00, 0x00, 0x18, 0x73, 0x05,       /* 18 cmp dword [r8+0xd0], 24; jae 33 */
    0xE9, 0, 0, 0, 0,                                                 /* 28 jmp 0xF78EA5 (rel32 at 29) */
    0xE9, 0, 0, 0, 0};                                                /* 33 jmp 0xF78F11 (the return; rel32 at 34) */
static const uint8_t kClusterStubB[36] = {
    0x83, 0xF8, 0xFF, 0x74, 0x1A,                                     /*  0 cmp eax, -1; je 31 */
    0x4C, 0x69, 0xC0, 0xE0, 0x00, 0x00, 0x00, 0x4C, 0x03, 0x42, 0x08, /*  5 imul r8, rax, 0xe0; add r8, [rdx+8] */
    0x41, 0x83, 0xB8, 0xD0, 0x00, 0x00, 0x00, 0x18, 0x73, 0x05,       /* 16 cmp dword [r8+0xd0], 24; jae 31 */
    0xE9, 0, 0, 0, 0,                                                 /* 26 jmp back (rel32 at 27) */
    0xE9, 0, 0, 0, 0};                                                /* 31 jmp the return (rel32 at 32) */
static const uint8_t kClusterStubD[35] = {
    0x83, 0xFD, 0xFF, 0x74, 0x19,                                     /*  0 cmp ebp, -1; je 30 */
    0x8B, 0xC5, 0x4C, 0x69, 0xC0, 0xE0, 0x00, 0x00, 0x00,             /*  5 mov eax, ebp; imul r8, rax, 0xe0 */
    0x43, 0x83, 0xBC, 0x38, 0xD0, 0x00, 0x00, 0x00, 0x18, 0x73, 0x05, /* 14 cmp dword [r8+r15+0xd0], 24; jae 30 */
    0xE9, 0, 0, 0, 0,                                                 /* 25 jmp 0xF7A26B (rel32 at 26) */
    0xE9, 0, 0, 0, 0};                                                /* 30 jmp 0xF7A2EE (the next location; rel32 at 31) */
static const struct { uint32_t site; uint8_t length; const uint8_t* pin; const uint8_t* stub; uint8_t size, backAt; uint32_t back; uint8_t skipAt; uint32_t skip; } kClusterSites[4] = {
    {0xF78E98u, 13, kClusterPinA, kClusterStubA, 38, 29, 0xF78EA5u, 34, 0xF78F11u},
    {0xF79081u, 11, kClusterPinB, kClusterStubB, 36, 27, 0xF7908Cu, 32, 0xF79101u},
    {0xF79260u, 11, kClusterPinB, kClusterStubB, 36, 27, 0xF7926Bu, 32, 0xF792E0u},
    {0xF7A262u, 9, kClusterPinD, kClusterStubD, 35, 26, 0xF7A26Bu, 31, 0xF7A2EEu}};
/* The other appends to a cluster's members (`mov eax, [c+0xd0]; mov [c+rax*8+0x10], loc; inc dword [c+0xd0]`) take no
 * capacity check either: SpreadLocations (0xF7D395) adds every location it spreads to its reference's cluster, and the
 * 25th member is written over the count (+0xD0), so the next append went far out of bounds (a 2x mega-city preview
 * crashed at 0xF7D39D). Each now skips the member list when it holds 24 (stub: the three instructions with
 * `cmp eax, 24; jae` after the load); the location still gets its cluster (+0xE0) and the weight. {site, load length,
 * store length, increment length, pin}. */
static const struct { uint32_t site; uint8_t load, store, inc; uint8_t pin[21]; } kClusterAppends[4] = {
    {0xF7D395u, 8, 5, 8, {0x41, 0x8B, 0x84, 0x24, 0xD0, 0x00, 0x00, 0x00, 0x4D, 0x89, 0x74, 0xC4, 0x10,
        0x41, 0xFF, 0x84, 0x24, 0xD0, 0x00, 0x00, 0x00}},                                              /* SpreadLocations */
    {0xF79F4Fu, 7, 5, 7, {0x41, 0x8B, 0x81, 0xD0, 0x00, 0x00, 0x00, 0x49, 0x89, 0x4C, 0xC1, 0x10, 0x41, 0xFF, 0x81, 0xD0, 0x00, 0x00, 0x00}},
    {0xF7A272u, 7, 5, 7, {0x41, 0x8B, 0x80, 0xD0, 0x00, 0x00, 0x00, 0x49, 0x89, 0x4C, 0xC0, 0x10, 0x41, 0xFF, 0x80, 0xD0, 0x00, 0x00, 0x00}},
    {0xF793D8u, 7, 5, 7, {0x41, 0x8B, 0x80, 0xD0, 0x00, 0x00, 0x00, 0x49, 0x89, 0x74, 0xC0, 0x10, 0x41, 0xFF, 0x80, 0xD0, 0x00, 0x00, 0x00}}};
/* cull_units_outside_playable_area (op 0x6D, 0xF25A00 per unit) keeps a level's units within the playable radius
 * (1024 x lg+0x2D70) and strips the rest at random, fading out to the border (lg+0x2D74). The small towns are placed
 * at the map's edge as scenery: at level 10 beyond the border, out of reach; on a larger map many sit between the
 * playable radius and the border, where a player can walk, with their buildings stripped. On larger maps (lg+0x2D7C ==
 * the larger disk) units are kept up to the border instead; beyond it the stripping is as before. */
#define DIF_CULL_RADIUS 0xF25B13u   /* mulss xmm6, [rdi+0x2d70] (rdi = lg) */
static const uint8_t kCullRadiusPin[8] = {0xF3, 0x0F, 0x59, 0xB7, 0x70, 0x2D, 0x00, 0x00};
static const uint8_t kCullRadiusStub[38] = {
    0x81, 0xBF, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /*  0 cmp dword [rdi+0x2d7c], disk / 1024 (at 6) */
    0x75, 0x0D,                                                       /* 10 jne 25 */
    0xF3, 0x0F, 0x59, 0xB7, 0x74, 0x2D, 0x00, 0x00,                   /* 12 mulss xmm6, [rdi+0x2d74] (the border) */
    0xE9, 0, 0, 0, 0,                                                 /* 20 jmp 0xF25B1B (rel32 at 21) */
    0xF3, 0x0F, 0x59, 0xB7, 0x70, 0x2D, 0x00, 0x00,                   /* 25 mulss xmm6, [rdi+0x2d70] (the playable radius) */
    0xE9, 0, 0, 0, 0};                                                /* 33 jmp 0xF25B1B (rel32 at 34) */
/* The planet's vista level (the background scenery around the map: terrain-like meshes, rocks, spires; research-vista2)
 * is spawned by SpawnVistaLevel (op 0x32, 0xC15A31..0xC15AB4) with a pose that only rotates it, and its contents sit at
 * fixed distances authored to meet the ground where level 10's terrain blend starts (620 m). On a larger map the blend
 * starts at the fade radius (1009 m at 2x), and walkable ground reaches the border (952 m), so on some planets (oasis,
 * sandy, hive, spires) the scenery rose inside the walkable band: a stretched, collision-less wall. On larger maps
 * (lg+0x2D7C == the larger disk) the pose's rows are scaled horizontally by fade / 620 (x, y; heights kept). */
#define DIF_VISTA_POSE 0xC15A79u /* movaps [rbp+0x90], xmm0 (rbp+0x20/+0x30/+0x40: the pose's rows; r15 = pass ctx) */
static const uint8_t kVistaPosePin[7] = {0x0F, 0x29, 0x85, 0x90, 0x00, 0x00, 0x00};
static const uint8_t kVistaPoseStub[96] = {
    0x0F, 0x29, 0x85, 0x90, 0x00, 0x00, 0x00,                         /*  0 movaps [rbp+0x90], xmm0 (replayed) */
    0x4D, 0x8B, 0x5F, 0x18,                                           /*  7 mov r11, [r15+0x18] (the level) */
    0x41, 0x81, 0xBB, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,             /* 11 cmp dword [r11+0x2d7c], disk / 1024 (at 18) */
    0x75, 0x28,                                                       /* 22 jne 64 */
    0x0F, 0x10, 0x0D, 0x31, 0x00, 0x00, 0x00,                         /* 24 movups xmm1, [rip+0x31] (the scale at 80) */
    0x0F, 0x28, 0x45, 0x20, 0x0F, 0x59, 0xC1, 0x0F, 0x29, 0x45, 0x20, /* 31 row 0 x= (s, s, 1, 1) */
    0x0F, 0x28, 0x45, 0x30, 0x0F, 0x59, 0xC1, 0x0F, 0x29, 0x45, 0x30, /* 42 row 1 */
    0x0F, 0x28, 0x45, 0x40, 0x0F, 0x59, 0xC1, 0x0F, 0x29, 0x45, 0x40, /* 53 row 2 */
    0xE9, 0, 0, 0, 0,                                                 /* 64 jmp 0xC15A80 (rel32 at 65) */
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F}; /* 80: (s, s, 1.0, 1.0) */
/* What the stubs that call into the loader (NavRetile, StrongholdInstall) share: the volatile registers pushed and popped
 * around the call (rax rcx rdx r8 r9 r10 r11), xmm0..xmm5 kept at [rsp+0x20..0x70] (movdqu), and the call itself
 * (mov rax, imm64 at +2; call rax). */
#define STUB_PUSH_VOLATILE 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53
#define STUB_POP_VOLATILE 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58
#define STUB_SAVE_XMM 0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30, \
    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50, \
    0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70
#define STUB_LOAD_XMM 0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30, \
    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50, \
    0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70
#define STUB_CALL_IMM64 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xD0
/* City strongholds (HF_DIFFICULTY_CITY_STRONGHOLDS). Outposts come from the
 * location-group set 0x495D456D ([lg+0x2D0]+0x38 sets, count +0xBC; set +0x28 groups of 0x18 {id, items, count},
 * +0x30 group count; items 0x1C8). Size 4 ("Stronghold") groups: 0x3317F707 (Terminid mega nests) and 0x14602D82
 * (Automaton fortresses). GenerateOutpostLocations (0xF7E250) offers an item only if a class in its list (+0x198,
 * count +0x1A0) has weight on the map's layout and its difficulty word (+0x60: min bits 2..5, max 6..9, 0xF none) admits
 * lg+0xF9; city layouts weight only class 3 and no stronghold has it, so a city's stronghold request is dropped. Each
 * group gets copies of its items up to 100 m (+0xD8) appended (a new array: the originals and every vanilla index
 * stay), with class list {3} and minimum difficulty 11: only cities at the added level take them. Installed at level
 * init (kStrongholdStub, after the radii stub, before the generator runs) and again whenever the resource is reloaded. */
/* The same copies serve the city ICBM missions (HF_DIFFICULTY_CITY_ICBM, research-icbm-city): the ICBM objectives'
 * stamps (gen_icbm, gen_restore_power, gen_refueling_station, gen_retrieve_codes) are items of the generic objectives
 * group 0x13DEAF52 (set 0xF28CFC63) with classes 1 (open) and 2 ("suburban") only, so a city places none; 18 of their
 * items (by stamp key, item +8) get city copies the same way. */
#define DIF_STRONGHOLD_SET 0x495D456Du
#define DIF_OBJECTIVE_SET 0xF28CFC63u
static const struct { uint32_t set, group; uint32_t flag; const char* name; } kCityCopyGroups[3] = {
    {DIF_STRONGHOLD_SET, 0x3317F707u, HF_DIFFICULTY_CITY_STRONGHOLDS, "Terminid strongholds"},
    {DIF_STRONGHOLD_SET, 0x14602D82u, HF_DIFFICULTY_CITY_STRONGHOLDS, "Automaton strongholds"},
    {DIF_OBJECTIVE_SET, 0x13DEAF52u, HF_DIFFICULTY_CITY_ICBM | HF_DIFFICULTY_RADAR, "ICBM and radar objectives"}};
/* The radar station's city stamps (class 3, group 8) are Terminid and Illuminate only (+0xE8 = 5): an Automaton city
 * placed none, so HF_DIFFICULTY_RADAR's radar never appeared there. Its five open stamps (group 6, every faction, no
 * faction layers) get city copies too. */
static const uint64_t kRadarStampKeys[5] = {0x050971891D76E2E3ull, 0x1BCB9A4F3A626704ull, 0x5770F97E6386462Cull,
    0xE11B4ABB0D9AE8D9ull, 0xC2B836E64BF6C099ull};
static const uint64_t kIcbmStampKeys[18] = {
    0x413C46C846CA6482ull, 0x3560869A7D0B04D9ull, 0x991F78887D2B2F58ull, 0x4DEF2887ED8AE8AAull, 0x43894C952FA03700ull,
    0xE339DF02FD103117ull, 0xF26066E22E361880ull,                                                    /* gen_icbm */
    0xADA69CC8B4C5884Dull, /* gen_restore_power: only the suburban stamp; the six open ones carry an Illuminate layer whose
                            * units only Illuminate missions load (research-stamp-resources: purple placeholders on cities) */
    0xE76258D4EDE30FC5ull, 0xBD8053E26AD36F24ull, 0x7EC2FEE502BDE119ull, 0xC0FCCE9FB95C1875ull,      /* gen_refueling_station */
    0xA3053BD400686933ull, 0x1D0EF609BFA41B1Dull, 0xADAD2BF045157644ull, 0x910761B98311348Cull, 0xD03062957B9EC17Cull,
    0x1CC86C10C95C9BBBull};                                                                          /* gen_retrieve_codes */
static uintptr_t g_strongholdItems[3]; /* our arrays: a group whose items are not ours gets them again */
static uint8_t g_strongholdClass = 3;
static uint32_t g_cityCopyFlags; /* the level's HF_DIFFICULTY_CITY_* and _RADAR flags, set at install */
static LONG g_strongholdLogged;
static int CityCopyWanted(int which, const uint8_t* item)
{
    if (which < 2)
    {
        float radius;
        memcpy(&radius, item + 0xD8, 4);
        return item[0x15E] == 4 && radius > 0.0f && radius <= 100.0f;
    }
    uint64_t key;
    memcpy(&key, item + 8, 8);
    for (int k = 0; k < 18 && (g_cityCopyFlags & HF_DIFFICULTY_CITY_ICBM); k++) if (key == kIcbmStampKeys[k]) return 1;
    for (int k = 0; k < 5 && (g_cityCopyFlags & HF_DIFFICULTY_RADAR); k++) if (key == kRadarStampKeys[k]) return 1;
    return 0;
}
/* The city-copy group (kCityCopyGroups) a location group of a set is, when the level wants its copies; else -1. */
static int CityCopyGroup(uint32_t set, uint32_t group)
{
    int which = -1;
    for (int k = 0; k < 3; k++)
        if (group == kCityCopyGroups[k].group && set == kCityCopyGroups[k].set && (g_cityCopyFlags & kCityCopyGroups[k].flag)) which = k;
    return which;
}

/* A city copy of a location item: its class list {3}, its minimum difficulty the added level. */
static void MakeCityCopy(uint8_t* clone, const uint8_t* item)
{
    uint16_t word;
    memcpy(clone, item, 0x1C8);
    uint64_t classes = (uint64_t)(uintptr_t)&g_strongholdClass;
    uint32_t one = 1;
    memcpy(clone + 0x198, &classes, 8);
    memcpy(clone + 0x1A0, &one, 4);
    memcpy(&word, clone + 0x60, 2);
    word = (uint16_t)((word & ~0x3Cu) | ((DIF_VANILLA + 1) << 2)); /* minimum: the added level */
    memcpy(clone + 0x60, &word, 2);
}

/* A group's items with the city copies of those it wants appended, in a new array of HellForge's. */
static void CopyCityItems(uint64_t group, uint32_t gid, int which, uint64_t items, uint32_t itemCount)
{
    uint8_t* copy = VirtualAlloc(NULL, (size_t)itemCount * 2 * 0x1C8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (copy == NULL || SafeRead(items, copy, (size_t)itemCount * 0x1C8)) { if (copy) VirtualFree(copy, 0, MEM_RELEASE); return; }
    uint32_t added = 0;
    for (uint32_t i = 0; i < itemCount; i++)
    {
        uint8_t* item = copy + (size_t)i * 0x1C8;
        if (!CityCopyWanted(which, item)) continue;
        MakeCityCopy(copy + (size_t)(itemCount + added) * 0x1C8, item);
        added++;
    }
    if (added == 0) { VirtualFree(copy, 0, MEM_RELEASE); return; }
    uint64_t array = (uint64_t)(uintptr_t)copy;
    uint32_t total = itemCount + added;
    if (ApiWrite(group + 8, &array, 8) || ApiWrite(group + 0x10, &total, 4)) return; /* the array first: a reader sees old or new, both valid */
    g_strongholdItems[which] = (uintptr_t)copy;
    if (InterlockedIncrement(&g_strongholdLogged) <= 8)
        Note("difficulty: %s offered on city maps at level 11 (%u of %u items copied, group %08x at %#llx)",
            kCityCopyGroups[which].name, added, itemCount, gid, (unsigned long long)group);
}

/* Set s of the level's location-group sets: its groups the level wants city copies of get them (again, when their
 * items are not ours). */
static void StrongholdSet(uint64_t sets, uint32_t s)
{
    uint64_t set = 0, groups = 0;
    uint32_t id = 0, groupCount = 0;
    if (SafeRead(sets + 8 * (uint64_t)s, &set, 8) || set == 0 || SafeRead(set, &id, 4) || (id != DIF_STRONGHOLD_SET && id != DIF_OBJECTIVE_SET) ||
        SafeRead(set + 0x28, &groups, 8) || SafeRead(set + 0x30, &groupCount, 4) || groups == 0 || groupCount > 64) return;
    for (uint32_t g = 0; g < groupCount; g++)
    {
        uint64_t group = groups + 0x18 * (uint64_t)g, items = 0;
        uint32_t gid = 0, itemCount = 0;
        if (SafeRead(group, &gid, 4) || SafeRead(group + 8, &items, 8) || SafeRead(group + 0x10, &itemCount, 4)) continue;
        int which = CityCopyGroup(id, gid);
        if (which < 0 || items == 0 || itemCount == 0 || itemCount > 256 || items == g_strongholdItems[which]) continue;
        CopyCityItems(group, gid, which, items, itemCount);
    }
}

static void StrongholdInstall(uintptr_t level)
{
    uint64_t holder = 0, sets = 0;
    uint32_t count = 0;
    if (level == 0 || SafeRead(level + 0x2D0, &holder, 8) || holder == 0 || SafeRead(holder + 0x38, &sets, 8) || sets == 0 ||
        SafeRead(holder + 0xBC, &count, 4) || count == 0 || count > 64) return;
    for (uint32_t s = 0; s < count; s++) StrongholdSet(sets, s);
}

static const uint8_t kStrongholdStub[126] = {
    0x9C,                                                             /*   0 pushfq */
    STUB_PUSH_VOLATILE,                                               /*   1 push rax rcx rdx r8 r9 r10 r11 */
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,                         /*  12 sub rsp, 0x80 (8 pushes + 0x80: aligned) */
    STUB_SAVE_XMM,                                                    /*  19 movdqu [rsp+0x20..0x70], xmm0..xmm5 */
    0x4C, 0x89, 0xE9,                                                 /*  55 mov rcx, r13 (the level) */
    STUB_CALL_IMM64,                                                  /*  58 mov rax, StrongholdInstall (imm64 at 60); call rax */
    STUB_LOAD_XMM,                                                    /*  70 movdqu xmm0..xmm5, [rsp+0x20..0x70] */
    0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00,                         /* 106 add rsp, 0x80 */
    STUB_POP_VOLATILE,                                                /* 113 pop r11 r10 r9 r8 rdx rcx rax */
    0x9D,                                                             /* 124 popfq */
    0xC3};                                                            /* 125 (replaced by jmp back: the stub is copied with 4 more bytes) */

/* ---- the checks (a map_scale above 1 needs these) --------------------------------------------------------------- */

/* The structured maps' revoronoi reads and vertex bucket pool. */
static const char* StructuredFailed(uintptr_t game)
{
    if (!LargerMaps()) return NULL;
    uint8_t zone[21];
    for (int i = 0; i < 3; i++)
        if (SafeRead(game + kReVoronoiStubs[i].site, zone, kReVoronoiStubs[i].length) || memcmp(zone, kReVoronoiStubs[i].pin, kReVoronoiStubs[i].length) != 0)
            return "the structured map generator is not the reviewed one (a map_scale above 1 needs it)";
    for (int i = 0; i < 8; i++)
        if (SafeRead(game + kBucketPool[i].rva, zone, kBucketPool[i].length) || memcmp(zone, kBucketPool[i].pin, kBucketPool[i].length) != 0)
            return "the structured map generator is not the reviewed one (a map_scale above 1 needs it)";
    return NULL;
}

/* The location clusters' assignments, the playable area's cull, the vista level's spawn and the clusters' appends. */
static const char* ClusterPinsFailed(uintptr_t game)
{
    if (!LargerMaps()) return NULL;
    uint8_t zone[21];
    for (int i = 0; i < 4; i++)
        if (SafeRead(game + kClusterSites[i].site, zone, kClusterSites[i].length) || memcmp(zone, kClusterSites[i].pin, kClusterSites[i].length) != 0)
            return "the location clusters are not the reviewed ones (a map_scale above 1 needs it)";
    if (SafeRead(game + DIF_CULL_RADIUS, zone, 8) || memcmp(zone, kCullRadiusPin, 8) != 0)
        return "the playable area's cull is not the reviewed one (a map_scale above 1 needs it)";
    if (SafeRead(game + DIF_VISTA_POSE, zone, 7) || memcmp(zone, kVistaPosePin, 7) != 0)
        return "the vista level's spawn is not the reviewed one (a map_scale above 1 needs it)";
    for (int i = 0; i < 4; i++)
    {
        uint8_t length = kClusterAppends[i].load + kClusterAppends[i].store + kClusterAppends[i].inc;
        if (SafeRead(game + kClusterAppends[i].site, zone, length) || memcmp(zone, kClusterAppends[i].pin, length) != 0)
            return "the location clusters are not the reviewed ones (a map_scale above 1 needs it)";
    }
    return NULL;
}

/* SpreadLocations_Cities' flags: their clear, read, writes and checks. */
static const char* CityFlagsFailed(uintptr_t game)
{
    if (!LargerMaps()) return NULL;
    uint8_t flags[32];
    if (SafeRead(game + DIF_FLAGS_CLEAR, flags, 32) || memcmp(flags, kFlagsClearPin, 32) != 0 ||
        SafeRead(game + DIF_FLAGS_READ, flags, 4) || memcmp(flags, kFlagsReadPin, 4) != 0)
        return "the city locations' spreading is not the reviewed one (a map_scale above 1 needs it)";
    for (int i = 0; i < 3; i++)
        if (SafeRead(game + kFlagsWrites[i].site, flags, kFlagsWrites[i].length) || memcmp(flags, kFlagsWrites[i].pin, kFlagsWrites[i].length) != 0)
            return "the city locations' spreading is not the reviewed one (a map_scale above 1 needs it)";
    for (int i = 0; i < 2; i++)
        if (SafeRead(game + kFlagsChecks[i], flags, 7) || memcmp(flags, kFlagsCheckPins[i], 7) != 0)
            return "the city locations' spreading is not the reviewed one (a map_scale above 1 needs it)";
    return NULL;
}

/* ---- the install steps (a map_scale above 1) -------------------------------------------------------------------- */

/* The structured maps' block (revoronoi +0, +0x40, +0x80; the flags +0xC0, writes +0x100) and its revoronoi stubs. */
static const char* PrepareStructuredStubs(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    int32_t rel;
    uint8_t* structured = AllocateNearGame(game, 0x200);
    d->structured = structured;
    if (structured == NULL) return "no memory within 2 GB of game.dll for the structured maps";
    float diskBits = d->disk / 1024.0f;
    for (int i = 0; i < 3; i++)
    {
        uint8_t* stub = structured + 0x40 * i;
        memcpy(stub, kReVoronoiStubs[i].stub, kReVoronoiStubs[i].size);
        memcpy(stub + kReVoronoiStubs[i].diskAt, &diskBits, 4);
        rel = (int32_t)((uint8_t*)&d->cellFactor[0] - (stub + kReVoronoiStubs[i].factorAt + 4)); memcpy(stub + kReVoronoiStubs[i].factorAt, &rel, 4);
        rel = (int32_t)((uint8_t*)&d->cellFactor[1] - (stub + kReVoronoiStubs[i].quarterAt + 4)); memcpy(stub + kReVoronoiStubs[i].quarterAt, &rel, 4);
        rel = (int32_t)(game + kReVoronoiStubs[i].site + kReVoronoiStubs[i].length - (uintptr_t)(stub + kReVoronoiStubs[i].backAt + 4));
        memcpy(stub + kReVoronoiStubs[i].backAt, &rel, 4);
    }
    return NULL;
}

/* The vista level's pose scaled by the radii stub's fade over 620 m, through its own stub. */
static const char* InstallVista(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    float diskBits = d->disk / 1024.0f;
    uint8_t* vista = AllocateNearGame(game, 0x80);
    if (vista == NULL) return "no memory within 2 GB of game.dll for the vista level";
    float fade = 389.12f * g_difficulty.mapScale + 174.0f + 57.0f; /* the radii stub's fade */
    if (fade > d->disk) fade = d->disk;
    float scale = fade / 620.0f;
    memcpy(vista, kVistaPoseStub, sizeof kVistaPoseStub);
    memcpy(vista + 18, &diskBits, 4);
    memcpy(vista + 80, &scale, 4); memcpy(vista + 84, &scale, 4);
    int32_t rel = (int32_t)(game + DIF_VISTA_POSE + 7 - (uintptr_t)(vista + 69)); memcpy(vista + 65, &rel, 4);
    DWORD vistaProtect;
    VirtualProtect(vista, 0x80, PAGE_EXECUTE_READ, &vistaProtect);
    uint8_t jump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    rel = (int32_t)((uintptr_t)vista - (game + DIF_VISTA_POSE + 5)); memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + DIF_VISTA_POSE, jump, 7)) return "the vista level could not be widened";
    return NULL;
}

/* The playable area's cull up to the border on larger maps, through its own stub. */
static const char* InstallCull(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    float diskBits = d->disk / 1024.0f;
    int32_t rel;
    uint8_t* cull = AllocateNearGame(game, 0x40);
    if (cull == NULL) return "no memory within 2 GB of game.dll for the playable area's cull";
    memcpy(cull, kCullRadiusStub, sizeof kCullRadiusStub);
    memcpy(cull + 6, &diskBits, 4);
    rel = (int32_t)(game + DIF_CULL_RADIUS + 8 - (uintptr_t)(cull + 25)); memcpy(cull + 21, &rel, 4);
    rel = (int32_t)(game + DIF_CULL_RADIUS + 8 - (uintptr_t)(cull + 38)); memcpy(cull + 34, &rel, 4);
    DWORD cullProtect;
    VirtualProtect(cull, 0x40, PAGE_EXECUTE_READ, &cullProtect);
    uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    rel = (int32_t)((uintptr_t)cull - (game + DIF_CULL_RADIUS + 5)); memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + DIF_CULL_RADIUS, jump, 8)) return "the playable area's cull could not be widened";
    return NULL;
}

/* The city flags' clear and write stubs in the structured maps' block, which is then made executable. */
static const char* PrepareFlagsStubs(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    int32_t rel;
    uint8_t* clear = d->structured + 0xC0;
    memcpy(clear, kFlagsClearStub, sizeof kFlagsClearStub);
    rel = (int32_t)(game + DIF_ARENA_ALLOCATE - (uintptr_t)(clear + 20)); memcpy(clear + 16, &rel, 4);
    rel = (int32_t)(game + DIF_MEMSET - (uintptr_t)(clear + 40)); memcpy(clear + 36, &rel, 4);
    rel = (int32_t)(game + DIF_FLAGS_CLEAR_BACK - (uintptr_t)(clear + 48)); memcpy(clear + 44, &rel, 4);
    for (int i = 0; i < 3; i++)
    {
        uint8_t* stub = d->structured + 0x100 + 0x20 * i;
        memcpy(stub, kFlagsWrites[i].stub, kFlagsWrites[i].size);
        rel = (int32_t)(game + kFlagsWrites[i].back - (uintptr_t)(stub + kFlagsWrites[i].backAt + 4)); memcpy(stub + kFlagsWrites[i].backAt, &rel, 4);
    }
    DWORD structuredProtect;
    VirtualProtect(d->structured, 0x200, PAGE_EXECUTE_READ, &structuredProtect);
    return NULL;
}

/* The revoronoi reads, the vertex bucket pool, and the city flags (moved to the arena, 0x400 places). */
static const char* JumpToStructured(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    int32_t rel;
    for (int i = 0; i < 3; i++)
    {
        uint8_t jump[11];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        rel = (int32_t)((uintptr_t)(d->structured + 0x40 * i) - (game + kReVoronoiStubs[i].site + 5)); memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kReVoronoiStubs[i].site, jump, kReVoronoiStubs[i].length)) return "the structured maps could not be given the larger map";
    }
    for (int i = 0; i < 8; i++)
        if (ApiWrite(game + kBucketPool[i].rva + kBucketPool[i].at, &kBucketPool[i].value, 4))
            return "the structured maps' vertex buckets could not be enlarged";
    uint8_t flagsJump[32];
    memset(flagsJump, 0x90, sizeof flagsJump);
    flagsJump[0] = 0xE9;
    rel = (int32_t)((uintptr_t)(d->structured + 0xC0) - (game + DIF_FLAGS_CLEAR + 5)); memcpy(flagsJump + 1, &rel, 4);
    uint8_t load = 0x8B;
    uint32_t flagsCap = 0x400;
    if (ApiWrite(game + DIF_FLAGS_CLEAR, flagsJump, 32) || ApiWrite(game + DIF_FLAGS_READ + 1, &load, 1) ||
        ApiWrite(game + kFlagsChecks[0] + 3, &flagsCap, 4) || ApiWrite(game + kFlagsChecks[1] + 3, &flagsCap, 4))
        return "the city locations' flags could not be moved";
    for (int i = 0; i < 3; i++)
    {
        uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
        rel = (int32_t)((uintptr_t)(d->structured + 0x100 + 0x20 * i) - (game + kFlagsWrites[i].site + 5)); memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kFlagsWrites[i].site, jump, kFlagsWrites[i].length)) return "the city locations' flags could not be moved";
    }
    return NULL;
}

/* The location clusters' four assignments guarded (a location with no cluster with room stays unclustered). */
static const char* InstallClusterGuards(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    int32_t rel;
    uint8_t* clusters = AllocateNearGame(game, 0x100);
    if (clusters == NULL) return "no memory within 2 GB of game.dll for the location clusters";
    for (int i = 0; i < 4; i++)
    {
        uint8_t* stub = clusters + 0x40 * i;
        memcpy(stub, kClusterSites[i].stub, kClusterSites[i].size);
        rel = (int32_t)(game + kClusterSites[i].back - (uintptr_t)(stub + kClusterSites[i].backAt + 4)); memcpy(stub + kClusterSites[i].backAt, &rel, 4);
        rel = (int32_t)(game + kClusterSites[i].skip - (uintptr_t)(stub + kClusterSites[i].skipAt + 4)); memcpy(stub + kClusterSites[i].skipAt, &rel, 4);
    }
    DWORD clusterProtect;
    VirtualProtect(clusters, 0x100, PAGE_EXECUTE_READ, &clusterProtect);
    for (int i = 0; i < 4; i++)
    {
        uint8_t jump[13];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        rel = (int32_t)((uintptr_t)(clusters + 0x40 * i) - (game + kClusterSites[i].site + 5)); memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kClusterSites[i].site, jump, kClusterSites[i].length)) return "the location clusters could not be guarded";
    }
    return NULL;
}

/* The clusters' member appends skip a full list (24): the load, cmp eax, 24; jae over the store and increment. */
static const char* InstallAppendGuards(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    int32_t rel;
    uint8_t* appends = AllocateNearGame(game, 0x100);
    if (appends == NULL) return "no memory within 2 GB of game.dll for the location clusters";
    for (int i = 0; i < 4; i++)
    {
        uint8_t* stub = appends + 0x40 * i;
        uint8_t load = kClusterAppends[i].load, rest = kClusterAppends[i].store + kClusterAppends[i].inc;
        memcpy(stub, kClusterAppends[i].pin, load);
        stub[load] = 0x83; stub[load + 1] = 0xF8; stub[load + 2] = 0x18;   /* cmp eax, 24 */
        stub[load + 3] = 0x73; stub[load + 4] = rest;                       /* jae over the store and increment */
        memcpy(stub + load + 5, kClusterAppends[i].pin + load, rest);
        stub[load + 5 + rest] = 0xE9;
        rel = (int32_t)(game + kClusterAppends[i].site + load + rest - (uintptr_t)(stub + load + 5 + rest + 5));
        memcpy(stub + load + 5 + rest + 1, &rel, 4);
    }
    DWORD appendProtect;
    VirtualProtect(appends, 0x100, PAGE_EXECUTE_READ, &appendProtect);
    for (int i = 0; i < 4; i++)
    {
        uint8_t jump[21];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        rel = (int32_t)((uintptr_t)(appends + 0x40 * i) - (game + kClusterAppends[i].site + 5)); memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kClusterAppends[i].site, jump, kClusterAppends[i].load + kClusterAppends[i].store + kClusterAppends[i].inc))
            return "the location clusters could not be guarded";
    }
    return NULL;
}
