/* Level 11 on the war table and in the HUD (difficulty.c): the difficulty bar's 11th widget, the level's icon, the
 * mission board's slots, the hologram's scale and labels, and the mission card's map view. Included by
 * hellforge_modloader.c after difficulty.c. */

/* The war table's 3D hologram (0x143062D) places its labels at the playable area's edge from half the playable radius
 * (h, UI +0x8D70C, from the preview record; terrain height lookup 0x1432F10) and clamps it for level 10's maps
 * (h = 0.19): min(h + 0.01, 0.2) (0x1431584) and min(h, 0.17) (0x14319B6), so a larger map's labels sat inside it. The first min takes 1.0 instead (no level-10 map reaches
 * 0.2); the second's cap becomes max(0.17, h x 0.17 / 0.19): 0.17 for every level-10 map, in proportion above. */
#define DIF_MAPVIEW_MIN 0x1431584u    /* minss xmm8, [rip+0.2] (disp32 at 5, 9 bytes) */
#define DIF_MAPVIEW_CAP 0x14319B6u    /* minss xmm8, [rip+0.17] (9 bytes) */
#define DIF_MAPVIEW_CAP_NEXT 0x14319BFu
static const uint8_t kMapViewMinPin[9] = {0xF3, 0x44, 0x0F, 0x5D, 0x05, 0xC7, 0x52, 0xF9, 0x00};
static const uint8_t kMapViewCapPin[9] = {0xF3, 0x44, 0x0F, 0x5D, 0x05, 0x41, 0x4E, 0xF9, 0x00};
static const uint8_t kMapViewStub[33] = {
    0xF3, 0x44, 0x0F, 0x10, 0x0D, 0, 0, 0, 0,                         /*  0 movss xmm9, [rip+0.17/0.19] (rel32 at 5) */
    0xF3, 0x45, 0x0F, 0x59, 0xC8,                                     /*  9 mulss xmm9, xmm8 */
    0xF3, 0x44, 0x0F, 0x5F, 0x0D, 0, 0, 0, 0,                         /* 14 maxss xmm9, [rip+0.17] (rel32 at 19) */
    0xF3, 0x45, 0x0F, 0x5D, 0xC1,                                     /* 23 minss xmm8, xmm9 */
    0xE9, 0, 0, 0, 0};                                                /* 28 jmp 0x14319BF (rel32 at 29) */
/* The war table's mission card shows its minimap (element panel+0x220, 0x18760E0) through a fixed UV rect, 0.275 to
 * 0.725 of the 2048 m texture (+-460 m: level 10's maps are 389 m), set at 0x187629E when the preview is ready, and
 * places its markers with fixed scales: 484.44 px per 1024 m (0x23C7B58: 0x1875B27, 0x1876D1E, 0x1876D26, 0x18773F1,
 * 0x18773F9, and xmm6 for the objective loop), 968.89 px per world (0x23C7D20: 0x1875FA5, 0x18771A0) and squad slots
 * within +-512 m (0x1876AD5, 0x1876AE4). Larger maps: s = clamp(record radius / 0.38, 1, 0.5 / 0.225) (the preview
 * record's radius, [rbx+0x230]); the rect grows by 0.225 (s - 1) each way and the scales follow (/ s, x s), through
 * globals at watch+0x48..+0x54 that the sites read instead of the constants. s is exactly 1 for every map up to 0.38,
 * so level 10's cards are unchanged. */
#define DIF_CARD_VIEW_SITE 0x187629Eu
#define DIF_CARD_VIEW_CALL 0x18762C8u /* call 0x143EEF0 (the element's rect): where the stub goes back */
static const uint8_t kCardViewPin[42] = {0xC7, 0x44, 0x24, 0x48, 0x9A, 0x99, 0x39, 0x3F, 0xC7, 0x44, 0x24, 0x4C, 0x9A, 0x99,
    0x39, 0x3F, 0x4C, 0x8B, 0x44, 0x24, 0x48, 0xC7, 0x44, 0x24, 0x50, 0xCD, 0xCC, 0x8C, 0x3E, 0xC7, 0x44, 0x24, 0x54, 0xCD,
    0xCC, 0x8C, 0x3E, 0x48, 0x8B, 0x54, 0x24, 0x50};
static const uint8_t kCardViewCallPin[5] = {0xE8, 0x23, 0x8C, 0xBC, 0xFF};
/* The marker scale readers: rva, length, disp32 at, the global (watch+0x48 + 4 x), the original bytes. */
static const struct { uint32_t rva; uint8_t length, at, global; uint8_t pin[9]; } kCardScales[9] = {
    {0x1875B27u, 9, 5, 0, {0xF3, 0x44, 0x0F, 0x10, 0x15, 0x28, 0x20, 0xB5, 0x00}},
    {0x1876D1Eu, 8, 4, 0, {0xF3, 0x0F, 0x59, 0x15, 0x32, 0x0E, 0xB5, 0x00}},
    {0x1876D26u, 8, 4, 0, {0xF3, 0x0F, 0x59, 0x1D, 0x2A, 0x0E, 0xB5, 0x00}},
    {0x18773F1u, 8, 4, 0, {0xF3, 0x0F, 0x59, 0x0D, 0x5F, 0x07, 0xB5, 0x00}},
    {0x18773F9u, 8, 4, 0, {0xF3, 0x0F, 0x59, 0x15, 0x57, 0x07, 0xB5, 0x00}},
    {0x1875FA5u, 8, 4, 1, {0xF3, 0x0F, 0x10, 0x25, 0x73, 0x1D, 0xB5, 0x00}},
    {0x18771A0u, 8, 4, 1, {0xF3, 0x0F, 0x10, 0x25, 0x78, 0x0B, 0xB5, 0x00}},
    {0x1876AE4u, 8, 4, 2, {0xF3, 0x0F, 0x10, 0x35, 0x9C, 0x10, 0xB5, 0x00}},
    {0x1876AD5u, 8, 4, 3, {0xF3, 0x0F, 0x10, 0x3D, 0x9F, 0x18, 0xB5, 0x00}}};
static const uint32_t kCardScaleValues[4] = {0x43F238E4u, 0x447238E4u, 0x44000000u, 0xC4000000u}; /* 484.44, 968.89, 512, -512 */
/* The war table's 3D hologram (UI object, 0x142D870 per frame) shows the mission at a fixed scale: +0x8D704, 6.6 (set at
 * 0x142BAD6 and 0x142C8A4; the zoom slider 0x14329B0 blends 6.6..11 at 0x1432A92), the terrain's scale and what every
 * marker divides by; for a larger map it showed only the vanilla area. The scale is kept at the vanilla value x k,
 * k = clamp(0.19 / h, 0.45, 1), h = half the playable radius (+0x8D70C, the preview record's, set at 0x142D99D):
 * the three writers multiply by k, and a new h rescales the scale by k(new) / k(old). Level 10's h is 0.19: k = 1.
 * Each stub keeps its scratch registers on the stack. Rel32 targets: 0.19, 0.45, 1.0, 6.6, the way back. */
static const struct { uint32_t site; uint8_t length, size; uint8_t pin[10]; struct { uint8_t at, target; } slots[7]; uint8_t code[120]; } kHoloStubs[4] = {
    {0x1432A92u, 8, 65, {0xF3, 0x0F, 0x11, 0x81, 0x04, 0xD7, 0x08, 0x00}, {{12, 0}, {28, 1}, {36, 2}, {61, 4}}, {
      0x48, 0x83, 0xEC, 0x10, 0x0F, 0x11, 0x1C, 0x24, 0xF3, 0x0F, 0x10, 0x1D, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F,
      0x5E, 0x99, 0x0C, 0xD7, 0x08, 0x00, 0xF3, 0x0F, 0x5F, 0x1D, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x5D, 0x1D,
      0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x59, 0xC3, 0x0F, 0x10, 0x1C, 0x24, 0x48, 0x83, 0xC4, 0x10, 0xF3, 0x0F,
      0x11, 0x81, 0x04, 0xD7, 0x08, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x00}},
    {0x142BAD6u, 10, 69, {0xC7, 0x83, 0x04, 0xD7, 0x08, 0x00, 0x33, 0x33, 0xD3, 0x40}, {{12, 0}, {28, 1}, {36, 2}, {44, 3}, {65, 4}}, {
      0x48, 0x83, 0xEC, 0x10, 0x0F, 0x11, 0x1C, 0x24, 0xF3, 0x0F, 0x10, 0x1D, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F,
      0x5E, 0x9B, 0x0C, 0xD7, 0x08, 0x00, 0xF3, 0x0F, 0x5F, 0x1D, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x5D, 0x1D,
      0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x59, 0x1D, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x9B, 0x04, 0xD7,
      0x08, 0x00, 0x0F, 0x10, 0x1C, 0x24, 0x48, 0x83, 0xC4, 0x10, 0xE9, 0x00, 0x00, 0x00, 0x00}},
    {0x142C8A4u, 10, 69, {0xC7, 0x81, 0x04, 0xD7, 0x08, 0x00, 0x33, 0x33, 0xD3, 0x40}, {{12, 0}, {28, 1}, {36, 2}, {44, 3}, {65, 4}}, {
      0x48, 0x83, 0xEC, 0x10, 0x0F, 0x11, 0x1C, 0x24, 0xF3, 0x0F, 0x10, 0x1D, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F,
      0x5E, 0x99, 0x0C, 0xD7, 0x08, 0x00, 0xF3, 0x0F, 0x5F, 0x1D, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x5D, 0x1D,
      0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x59, 0x1D, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x99, 0x04, 0xD7,
      0x08, 0x00, 0x0F, 0x10, 0x1C, 0x24, 0x48, 0x83, 0xC4, 0x10, 0xE9, 0x00, 0x00, 0x00, 0x00}},
    {0x142D99Du, 8, 119, {0xF3, 0x0F, 0x11, 0x86, 0x0C, 0xD7, 0x08, 0x00}, {{17, 0}, {33, 1}, {41, 2}, {49, 0}, {61, 1}, {69, 2}, {115, 4}}, {
      0x48, 0x83, 0xEC, 0x20, 0x0F, 0x11, 0x1C, 0x24, 0x0F, 0x11, 0x64, 0x24, 0x10, 0xF3, 0x0F, 0x10, 0x1D, 0x00,
      0x00, 0x00, 0x00, 0xF3, 0x0F, 0x5E, 0x9E, 0x0C, 0xD7, 0x08, 0x00, 0xF3, 0x0F, 0x5F, 0x1D, 0x00, 0x00, 0x00,
      0x00, 0xF3, 0x0F, 0x5D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x25, 0x00, 0x00, 0x00, 0x00, 0xF3,
      0x0F, 0x5E, 0xE0, 0xF3, 0x0F, 0x5F, 0x25, 0x00, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x5D, 0x25, 0x00, 0x00, 0x00,
      0x00, 0xF3, 0x0F, 0x5E, 0xE3, 0xF3, 0x0F, 0x59, 0xA6, 0x04, 0xD7, 0x08, 0x00, 0xF3, 0x0F, 0x11, 0xA6, 0x04,
      0xD7, 0x08, 0x00, 0xF3, 0x0F, 0x11, 0x86, 0x0C, 0xD7, 0x08, 0x00, 0x0F, 0x10, 0x64, 0x24, 0x10, 0x0F, 0x10,
      0x1C, 0x24, 0x48, 0x83, 0xC4, 0x20, 0xE9, 0x00, 0x00, 0x00, 0x00}},};
/* kCardViewStub's rel32 slots and what they address: constants c[0..7] after the stub (0.38, 1, 0.5 / 0.225, 0.225,
 * 0.275, 0.725, 484.44, 512), globals g[0..3] (8..11: watch+0x48..), 12 the way back. */
static const struct { uint8_t at, target; } kCardViewSlots[14] = {{12, 0}, {20, 1}, {28, 2}, {39, 1}, {47, 3}, {55, 4},
    {67, 5}, {109, 6}, {121, 8}, {136, 9}, {144, 7}, {156, 10}, {171, 11}, {176, 12}};
static const uint8_t kCardViewStub[180] = {
    0xF3, 0x0F, 0x10, 0x83, 0x30, 0x02, 0x00, 0x00,                   /*   0 movss xmm0, [rbx+0x230] (radius / 1024) */
    0xF3, 0x0F, 0x5E, 0x05, 0, 0, 0, 0,                               /*   8 divss xmm0, [0.38] */
    0xF3, 0x0F, 0x5F, 0x05, 0, 0, 0, 0,                               /*  16 maxss xmm0, [1.0] */
    0xF3, 0x0F, 0x5D, 0x05, 0, 0, 0, 0,                               /*  24 minss xmm0, [0.5 / 0.225]: s */
    0x0F, 0x28, 0xC8,                                                 /*  32 movaps xmm1, xmm0 */
    0xF3, 0x0F, 0x5C, 0x0D, 0, 0, 0, 0,                               /*  35 subss xmm1, [1.0] */
    0xF3, 0x0F, 0x59, 0x0D, 0, 0, 0, 0,                               /*  43 mulss xmm1, [0.225]: d */
    0xF3, 0x0F, 0x10, 0x15, 0, 0, 0, 0,                               /*  51 movss xmm2, [0.275] */
    0xF3, 0x0F, 0x5C, 0xD1,                                           /*  59 subss xmm2, xmm1: min */
    0xF3, 0x0F, 0x58, 0x0D, 0, 0, 0, 0,                               /*  63 addss xmm1, [0.725]: max */
    0xF3, 0x0F, 0x11, 0x54, 0x24, 0x50,                               /*  71 movss [rsp+0x50], xmm2 */
    0xF3, 0x0F, 0x11, 0x54, 0x24, 0x54,                               /*  77 movss [rsp+0x54], xmm2 */
    0x48, 0x8B, 0x54, 0x24, 0x50,                                     /*  83 mov rdx, [rsp+0x50] */
    0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x48,                               /*  88 movss [rsp+0x48], xmm1 */
    0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x4C,                               /*  94 movss [rsp+0x4c], xmm1 */
    0x4C, 0x8B, 0x44, 0x24, 0x48,                                     /* 100 mov r8, [rsp+0x48] */
    0xF3, 0x0F, 0x10, 0x35, 0, 0, 0, 0,                               /* 105 movss xmm6, [484.44] */
    0xF3, 0x0F, 0x5E, 0xF0,                                           /* 113 divss xmm6, xmm0 (the objective loop's scale) */
    0xF3, 0x0F, 0x11, 0x35, 0, 0, 0, 0,                               /* 117 movss [g0], xmm6 */
    0x0F, 0x28, 0xCE,                                                 /* 125 movaps xmm1, xmm6 */
    0xF3, 0x0F, 0x58, 0xCE,                                           /* 128 addss xmm1, xmm6 */
    0xF3, 0x0F, 0x11, 0x0D, 0, 0, 0, 0,                               /* 132 movss [g1], xmm1 */
    0xF3, 0x0F, 0x10, 0x0D, 0, 0, 0, 0,                               /* 140 movss xmm1, [512] */
    0xF3, 0x0F, 0x59, 0xC8,                                           /* 148 mulss xmm1, xmm0 */
    0xF3, 0x0F, 0x11, 0x0D, 0, 0, 0, 0,                               /* 152 movss [g2], xmm1 */
    0x0F, 0x57, 0xD2,                                                 /* 160 xorps xmm2, xmm2 */
    0xF3, 0x0F, 0x5C, 0xD1,                                           /* 163 subss xmm2, xmm1 */
    0xF3, 0x0F, 0x11, 0x15, 0, 0, 0, 0,                               /* 167 movss [g3], xmm2 */
    0xE9, 0, 0, 0, 0};                                                /* 175 jmp 0x18762C8 (rel32 at 176) */
/* The war table's mission board: 110 slots of 0x5C per planet: 0..29 the
 * planet's missions (3 per level 1..10), 30..109 regions 0..7 (10 levels each). City and factory maps only come from
 * regions (Super Earth's mega cities), whose generator (0x11E44D0) stopped at 10 (0x11E46F1, mov ebp, 10), and the
 * planet generators (campaign 0x11E3C10, defence 0x11E4060) put level 11 at slots 30..32, region 0's levels 1..3. Level
 * 11 now uses region 7's block: the planet's at 100 + n, region r's at 103 + r. A planet with a region 7 (war state
 * +0x23018: {planet, region, settings} x count +0x26018; only Cyberstan, whose 8 Megafactories offer levels 4 and up and
 * whose planet offers none) gets no level 11 of its own, and region r's goes to its level 1 slot (30 + 10r). Levels
 * 1..10 keep every slot. */
#define DIF_BOARD_REGION_MAX 0x11E46F1u
#define DIF_BOARD_REGION_SITE 0x11E4744u
#define DIF_BOARD_PLANET_SITE 0x11E3E67u
#define DIF_BOARD_DEFENCE_SITE 0x11E4287u
static const uint8_t kBoardRegionMaxPin[5] = {0xBD, 0x0A, 0x00, 0x00, 0x00};
static const uint8_t kBoardRegionPin[6] = {0x8B, 0xC2, 0x48, 0x6B, 0xF8, 0x5C};
static const uint8_t kBoardPlanetPin[7] = {0x41, 0x8B, 0xC3, 0x48, 0x6B, 0xF8, 0x5C};
/* {site, length, stub, size, back rel32 at, back, skip rel32 at, skip} */
/* ebx the level, esi the region (-1: none), [rsp+0x38] the war state, [rsp+0x40] the planet, edx the slot */
static const uint8_t kBoardRegionStub[93] = {
    0x83, 0xFB, 0x0B, 0x72, 0x43,                                     /*  0 cmp ebx, 11; jb 72 */
    0x83, 0xFE, 0x07, 0x77, 0x49,                                     /*  5 cmp esi, 7; ja 83 (none, or past the board) */
    0x48, 0x8B, 0x44, 0x24, 0x38, 0x8B, 0x54, 0x24, 0x40,             /* 10 mov rax, [rsp+0x38]; mov edx, [rsp+0x40] */
    0x44, 0x8B, 0x80, 0x18, 0x60, 0x02, 0x00, 0x48, 0x05, 0x18, 0x30, 0x02, 0x00, /* 19 r8d = the count; rax = the regions */
    0x45, 0x85, 0xC0, 0x74, 0x13,                                     /* 32 test r8d, r8d; je 56 */
    0x39, 0x10, 0x75, 0x06, 0x83, 0x78, 0x04, 0x07, 0x73, 0x13,       /* 37 the planet's region 7: jae 66 */
    0x48, 0x83, 0xC0, 0x18, 0x41, 0xFF, 0xC8, 0xEB, 0xE8,             /* 47 add rax, 0x18; dec r8d; jmp 32 */
    0x83, 0xFE, 0x06, 0x77, 0x16,                                     /* 56 cmp esi, 6; ja 83 */
    0x8D, 0x56, 0x67, 0xEB, 0x06,                                     /* 61 lea edx, [rsi+103]; jmp 72 */
    0x6B, 0xD6, 0x0A, 0x83, 0xC2, 0x1E,                               /* 66 imul edx, esi, 10; add edx, 30 (its level 1) */
    0x8B, 0xC2, 0x48, 0x6B, 0xF8, 0x5C,                               /* 72 mov eax, edx; imul rdi, rax, 0x5c */
    0xE9, 0x00, 0x00, 0x00, 0x00,                                     /* 78 jmp 0x11E474A (rel32 at 79) */
    0x48, 0x8B, 0x44, 0x24, 0x20, 0xE9, 0x00, 0x00, 0x00, 0x00};      /* 83 mov rax, [rsp+0x20]; jmp 0x11E4821 (rel32 at 89) */
static const uint8_t kBoardPlanetStub[70] = {
    0x41, 0x83, 0xFE, 0x0B, 0x72, 0x2F, 0x48, 0x8B, 0x44, 0x24, 0x38, 0x44, 0x8B, 0x80, 0xF8, 0xFF,
    0xFD, 0xFF, 0x48, 0x2D, 0x08, 0x30, 0x02, 0x00, 0x45, 0x85, 0xC0, 0x74, 0x14, 0x44, 0x39, 0x20,
    0x75, 0x06, 0x83, 0x78, 0x04, 0x07, 0x73, 0x19, 0x48, 0x83, 0xC0, 0x18, 0x41, 0xFF, 0xC8, 0xEB,
    0xE7, 0x44, 0x8D, 0x5E, 0x64, 0x41, 0x8B, 0xC3, 0x48, 0x6B, 0xF8, 0x5C, 0xE9, 0x00, 0x00, 0x00,
    0x00, 0xE9, 0x00, 0x00, 0x00, 0x00};
static const uint8_t kBoardDefenceStub[71] = {
    0x41, 0x83, 0xFE, 0x0B, 0x72, 0x30, 0x8B, 0x54, 0x24, 0x38, 0x45, 0x8B, 0x84, 0x24, 0xF8, 0xFF,
    0xFD, 0xFF, 0x49, 0x8D, 0x84, 0x24, 0xF8, 0xCF, 0xFD, 0xFF, 0x45, 0x85, 0xC0, 0x74, 0x13, 0x39,
    0x10, 0x75, 0x06, 0x83, 0x78, 0x04, 0x07, 0x73, 0x19, 0x48, 0x83, 0xC0, 0x18, 0x41, 0xFF, 0xC8,
    0xEB, 0xE8, 0x44, 0x8D, 0x5E, 0x64, 0x41, 0x8B, 0xC3, 0x48, 0x6B, 0xF8, 0x5C, 0xE9, 0x00, 0x00,
    0x00, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x00};
static const struct { uint32_t site; uint8_t length; const uint8_t* stub; uint8_t size, backAt; uint32_t back; uint8_t skipAt; uint32_t skip; } kBoardSites[3] = {
    {DIF_BOARD_REGION_SITE, 6, kBoardRegionStub, sizeof kBoardRegionStub, 79, 0x11E474Au, 89, 0x11E4821u},
    {DIF_BOARD_PLANET_SITE, 7, kBoardPlanetStub, sizeof kBoardPlanetStub, 61, 0x11E3E6Eu, 66, 0x11E4002u},
    {DIF_BOARD_DEFENCE_SITE, 7, kBoardDefenceStub, sizeof kBoardDefenceStub, 62, 0x11E428Eu, 67, 0x11E4429u}};
static const uint8_t kBandSitePin[8] = {0x49, 0x8B, 0x94, 0xC6, 0x50, 0xB9, 0x31, 0x03};
/* A widget's icon (0x19A296A: call 0x144F800, the image setter that takes atlas sprites only). Level 11's icon is a
 * texture of the mod's own (HfDifficulty.icon), so for level 11 (esi) the call goes to 0x1450160, the setter the
 * stratagem icons use (an atlas sprite, else a texture of that name); every other level keeps the game's. */
#define DIF_ICON_CALL 0x19A296Au
#define DIF_SET_SPRITE 0x144F800u
#define DIF_SET_SLOT 0x1449A70u        /* (element, slot, resource, is a sprite, 5th: 0): what 0x1450160 calls for a texture */
#define DIF_REFRESH_IMAGE 0x143F3C0u   /* applies the element's UV rect (+0x134) */
/* The other readers of a row's icon sprite (+0x10) put it in an image the same way, so level 11 gets the mod's
 * texture there too: each call goes to a short stub that checks the row's level (+4) and, for 11, enters kIconStub's
 * body (+9; a copy with the HUD's own base sprite for the HUD). The mission HUD's top bar (0x17CB8E0, rdi = the row)
 * calls 0x1450230 (element, base sprite, image, 0); the end-of-mission summary header (0x1464605), a summary widget
 * (0x18C32F3) and a war-table mission widget (0x1884C0F) call 0x144F800 (rbx = (level - 1) x 0x330 + the table in rsi
 * or rbp). */
#define DIF_SET_IMAGE 0x1450230u
#define DIF_HUD_ICON_READ 0x17CBB74u
#define DIF_HUD_ICON_CALL 0x17CBB8Cu
static const uint8_t kHudIconPin[29] = {0x4C, 0x8B, 0x47, 0x10, 0x48, 0x8D, 0x8E, 0x90, 0x04, 0x00, 0x00, 0x48, 0xBA, 0x80,
    0xE7, 0x6A, 0x2D, 0x6C, 0x53, 0x48, 0x94, 0x45, 0x33, 0xC9, 0xE8, 0x9F, 0x46, 0xC8, 0xFF}; /* r8 = [rdi+0x10]; ...; call 0x1450230 */
static const struct { uint32_t read, call; uint8_t pin[20], check[5]; } kIconSites[3] = {
    {0x14645F6u, 0x1464605u, {0x48, 0x8B, 0x54, 0x33, 0x10, 0x48, 0x8D, 0x8F, 0x88, 0xA2, 0x0A, 0x00, 0x45, 0x33, 0xC0, 0xE8, 0xF6, 0xB1, 0xFE, 0xFF},
     {0x83, 0x7C, 0x33, 0x04, 0x0B}},  /* cmp dword [rbx+rsi+4], 11 */
    {0x18C32E4u, 0x18C32F3u, {0x48, 0x8B, 0x54, 0x33, 0x10, 0x48, 0x8D, 0x8F, 0xD8, 0x04, 0x00, 0x00, 0x45, 0x33, 0xC0, 0xE8, 0x08, 0xC5, 0xB8, 0xFF},
     {0x83, 0x7C, 0x33, 0x04, 0x0B}},
    {0x1884C00u, 0x1884C0Fu, {0x48, 0x8B, 0x54, 0x2B, 0x10, 0x48, 0x8D, 0x8E, 0x10, 0x01, 0x00, 0x00, 0x45, 0x33, 0xC0, 0xE8, 0xEC, 0xAB, 0xBC, 0xFF},
     {0x83, 0x7C, 0x2B, 0x04, 0x0B}}}; /* cmp dword [rbx+rbp+4], 11 */
static const uint8_t kIconStub[131] = {
    0x83, 0xFE, DIF_VANILLA + 1,                                /*   0 cmp esi, 11 */
    0x0F, 0x85, 0, 0, 0, 0,                                     /*   3 jne 0x144F800 (rel32 at 5) */
    0x53, 0x57, 0x48, 0x83, 0xEC, 0x28,                         /*   9 push rbx; push rdi; sub rsp, 0x28 */
    0x48, 0x89, 0xCB,                                           /*  15 mov rbx, rcx */
    0x48, 0xBF, 0, 0, 0, 0, 0, 0, 0, 0,                         /*  18 mov rdi, the mod's icon texture (imm64 at 20) */
    0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,                         /*  28 mov rdx, Super Helldive's sprite (imm64 at 30) */
    0x45, 0x31, 0xC0,                                           /*  38 xor r8d, r8d */
    0xE8, 0, 0, 0, 0,                                           /*  41 call 0x144F800 (rel32 at 42) */
    0x48, 0x89, 0xD9,                                           /*  46 mov rcx, rbx */
    0xBA, 0x7E, 0xB8, 0xA8, 0x3A,                               /*  49 mov edx, 0x3AA8B87E (the image slot) */
    0x49, 0x89, 0xF8, 0x45, 0x31, 0xC9,                         /*  54 mov r8, rdi; xor r9d, r9d */
    0xC6, 0x44, 0x24, 0x20, 0x00,                               /*  60 mov byte [rsp+0x20], 0 */
    0xE8, 0, 0, 0, 0,                                           /*  65 call 0x1449A70 (rel32 at 66) */
    0x48, 0x89, 0xBB, 0x50, 0x01, 0x00, 0x00,                   /*  70 mov [rbx+0x150], rdi */
    0xC7, 0x83, 0x34, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /*  77 mov dword [rbx+0x134], 0.0 */
    0xC7, 0x83, 0x38, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /*  87 mov dword [rbx+0x138], 0.0 */
    0xC7, 0x83, 0x3C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, /*  97 mov dword [rbx+0x13C], 1.0 */
    0xC7, 0x83, 0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, /* 107 mov dword [rbx+0x140], 1.0 */
    0x48, 0x89, 0xD9, 0x48, 0x83, 0xC4, 0x28, 0x5F, 0x5B,       /* 117 mov rcx, rbx; add rsp, 0x28; pop rdi; pop rbx */
    0xE9, 0, 0, 0, 0};                                          /* 126 jmp 0x143F3C0 (rel32 at 127) */

/* The five instructions that compute a widget's address from its index (in eax), in the bar's build (0x19CFB40), update
 * (0x19D0FBC), destroy (0x19D1090) and choice (0x19D1110). Each jumps to a stub that runs them for indices below 10
 * and, for index 10, gives the address of the bar's 11th widget (ExtraWidget, rcx = the bar). `keep` leading bytes are
 * other instructions, run on both paths; `after` turns the 11th widget's address (rax) into what the site computes. */
typedef struct BarSite { uint32_t rva; uint8_t length, keep, bar; const uint8_t* original; const uint8_t* after; uint8_t afterLength; } BarSite;
static const uint8_t kBuildSite[] = {0x4C, 0x8B, 0x4C, 0x24, 0x40, 0x48, 0x03, 0xF9};          /* mov r9,[rsp+0x40]; add rdi,rcx */
static const uint8_t kBuildAfter[] = {0x48, 0x89, 0xC7};                                      /* mov rdi, rax */
static const uint8_t kUpdateSite[] = {0x48, 0x69, 0xF8, 0x10, 0x09, 0x00, 0x00, 0x48, 0x03, 0xFE}; /* imul rdi,rax,0x910; add rdi,rsi */
static const uint8_t kUpdateAfter[] = {0x48, 0x8D, 0xB8, 0x68, 0xFA, 0xFF, 0xFF};             /* lea rdi, [rax-0x598] */
static const uint8_t kDestroySite[] = {0x48, 0x8D, 0x8F, 0x98, 0x05, 0x00, 0x00, 0x48, 0x69, 0xD0, 0x10, 0x09, 0x00, 0x00,
    0x48, 0x03, 0xCA};                                                                         /* lea rcx,[rdi+0x598]; imul rdx,rax,0x910; add rcx,rdx */
static const uint8_t kSlotAfter[] = {0x48, 0x89, 0xC1};                                       /* mov rcx, rax */
static const uint8_t kFadeSite[] = {0x48, 0x69, 0xC8, 0x10, 0x09, 0x00, 0x00, 0x48, 0x81, 0xC1, 0x98, 0x05, 0x00, 0x00,
    0x48, 0x03, 0xCF};                                                                         /* imul rcx,rax,0x910; add rcx,0x598; add rcx,rdi */
static const uint8_t kChooseSite[] = {0x48, 0x8D, 0x8F, 0x98, 0x05, 0x00, 0x00, 0x4C, 0x69, 0xF8, 0x10, 0x09, 0x00, 0x00,
    0x8D, 0x53, 0x01, 0x49, 0x03, 0xCF};                                                       /* lea rcx,[rdi+0x598]; imul r15,rax,0x910; lea edx,[rbx+1]; add rcx,r15 */
static const uint8_t kChooseAfter[] = {0x48, 0x89, 0xC1, 0x4C, 0x8D, 0xB8, 0x68, 0xFA, 0xFF, 0xFF, 0x49, 0x29, 0xFF,
    0x8D, 0x53, 0x01};                                                                         /* rcx = rax; r15 = rax - 0x598 - rdi; lea edx,[rbx+1] */
#define BAR_RSI 0xF1u /* mov rcx, rsi: 48 89 F1 */
#define BAR_RDI 0xF9u /* mov rcx, rdi: 48 89 F9 */
static const BarSite kBarSites[] = {
    {0x19D004Au, sizeof kBuildSite, 5, BAR_RSI, kBuildSite, kBuildAfter, sizeof kBuildAfter},
    {0x19D0FD9u, sizeof kUpdateSite, 0, BAR_RSI, kUpdateSite, kUpdateAfter, sizeof kUpdateAfter},
    {0x19D10B2u, sizeof kDestroySite, 0, BAR_RDI, kDestroySite, kSlotAfter, sizeof kSlotAfter},
    {0x19D17FAu, sizeof kFadeSite, 0, BAR_RDI, kFadeSite, kSlotAfter, sizeof kSlotAfter},
    {0x19D1812u, sizeof kChooseSite, 0, BAR_RDI, kChooseSite, kChooseAfter, sizeof kChooseAfter}};

/* ExtraWidget(rcx = bar) -> rax = the bar's 11th widget: the bar's entry in a table of DIF_BARS bars (claimed on first
 * use; the last is reused if all are taken), and the widget block of that entry. Keeps every other register. */
static const uint8_t kExtraWidget[63] = {
    0x52, 0x41, 0x50,                         /*  0 push rdx; push r8 */
    0x4C, 0x8D, 0x05, 0, 0, 0, 0,             /*  3 lea r8, [rip + bars] (rel32 at 6) */
    0x31, 0xC0,                               /* 10 xor eax, eax */
    0x49, 0x8B, 0x14, 0xC0,                   /* 12 mov rdx, [r8 + rax*8] */
    0x48, 0x39, 0xCA,                         /* 16 cmp rdx, rcx */
    0x74, 0x15,                               /* 19 je found (42) */
    0x48, 0x85, 0xD2,                         /* 21 test rdx, rdx */
    0x74, 0x0C,                               /* 24 je claim (38) */
    0xFF, 0xC0,                               /* 26 inc eax */
    0x83, 0xF8, DIF_BARS,                     /* 28 cmp eax, DIF_BARS */
    0x72, 0xEB,                               /* 31 jb 12 */
    0xB8, DIF_BARS - 1, 0x00, 0x00, 0x00,     /* 33 mov eax, DIF_BARS - 1 */
    0x49, 0x89, 0x0C, 0xC0,                   /* 38 claim: mov [r8 + rax*8], rcx */
    0x48, 0x69, 0xC0, 0x10, 0x09, 0x00, 0x00, /* 42 found: imul rax, rax, 0x910 */
    0x4C, 0x8D, 0x05, 0, 0, 0, 0,             /* 49 lea r8, [rip + widgets] (rel32 at 52) */
    0x4C, 0x01, 0xC0,                         /* 56 add rax, r8 */
    0x41, 0x58, 0x5A, 0xC3};                  /* 59 pop r8; pop rdx; ret */

_Static_assert(sizeof kBarSites / sizeof kBarSites[0] == DIF_BAR_SITES, "a stub per bar site (DifficultyInstall.barStubs)");

/* ---- the checks ------------------------------------------------------------------------------------------------- */

static const char* BandFailed(uintptr_t game)
{
    uint8_t band[sizeof kBandSitePin];
    if (SafeRead(game + DIF_BAND_SITE, band, sizeof band) || memcmp(band, kBandSitePin, sizeof band) != 0)
        return "the band sprites are not the reviewed ones";
    return NULL;
}

static const char* BoardFailed(uintptr_t game)
{
    uint8_t board[7];
    if (SafeRead(game + DIF_BOARD_REGION_MAX, board, 5) || memcmp(board, kBoardRegionMaxPin, 5) != 0 ||
        SafeRead(game + DIF_BOARD_REGION_SITE, board, 6) || memcmp(board, kBoardRegionPin, 6) != 0 ||
        SafeRead(game + DIF_BOARD_PLANET_SITE, board, 7) || memcmp(board, kBoardPlanetPin, 7) != 0 ||
        SafeRead(game + DIF_BOARD_DEFENCE_SITE, board, 7) || memcmp(board, kBoardPlanetPin, 7) != 0)
        return "the war table's mission board is not the reviewed one";
    return NULL;
}

/* The bar's icon setter, the mission HUD's icon and the other icon readers. */
static const char* IconsFailed(uintptr_t game)
{
    uint8_t iconCall[5];
    int32_t iconRel;
    if (SafeRead(game + DIF_ICON_CALL, iconCall, 5)) return "the difficulty icon setter is unreadable";
    memcpy(&iconRel, iconCall + 1, 4);
    if (iconCall[0] != 0xE8 || DIF_ICON_CALL + 5 + iconRel != DIF_SET_SPRITE) return "the difficulty icon setter is not the reviewed one";
    uint8_t icon[29];
    if (SafeRead(game + DIF_HUD_ICON_READ, icon, 29) || memcmp(icon, kHudIconPin, 29) != 0) return "the mission HUD's difficulty icon is not the reviewed one";
    for (int i = 0; i < 3; i++)
        if (SafeRead(game + kIconSites[i].read, icon, 20) || memcmp(icon, kIconSites[i].pin, 20) != 0) return "a difficulty icon reader is not the reviewed one";
    return NULL;
}

static const char* BarFailed(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kBarSites / sizeof kBarSites[0]; i++)
    {
        uint8_t bytes[32];
        if (SafeRead(game + kBarSites[i].rva, bytes, kBarSites[i].length) || memcmp(bytes, kBarSites[i].original, kBarSites[i].length) != 0)
            return "the difficulty bar is not the reviewed one";
    }
    return NULL;
}

/* A larger map's hologram scale writers, the mission card's map view and its marker scales. */
static const char* CardViewFailed(uintptr_t game)
{
    if (!LargerMaps()) return NULL;
    for (int i = 0; i < 4; i++)
    {
        uint8_t holo[10];
        if (SafeRead(game + kHoloStubs[i].site, holo, kHoloStubs[i].length) || memcmp(holo, kHoloStubs[i].pin, kHoloStubs[i].length) != 0)
            return "the war table hologram's mission scale is not the reviewed one";
    }
    uint8_t card[42];
    uint32_t scale;
    if (SafeRead(game + DIF_CARD_VIEW_SITE, card, 42) || memcmp(card, kCardViewPin, 42) != 0 ||
        SafeRead(game + DIF_CARD_VIEW_CALL, card, 5) || memcmp(card, kCardViewCallPin, 5) != 0) return "the mission card's map view is not the reviewed one";
    for (int i = 0; i < 9; i++)
    {
        int32_t disp;
        if (SafeRead(game + kCardScales[i].rva, card, kCardScales[i].length) || memcmp(card, kCardScales[i].pin, kCardScales[i].length) != 0)
            return "the mission card's markers are not the reviewed ones";
        memcpy(&disp, card + kCardScales[i].at, 4);
        if (SafeRead(game + kCardScales[i].rva + kCardScales[i].length + disp, &scale, 4) || scale != kCardScaleValues[kCardScales[i].global])
            return "the mission card's marker scales are not the reviewed ones";
    }
    return NULL;
}

static const char* MapViewFailed(uintptr_t game)
{
    uint8_t view[9];
    if (SafeRead(game + DIF_MAPVIEW_MIN, view, 9) || memcmp(view, kMapViewMinPin, 9) != 0 ||
        SafeRead(game + DIF_MAPVIEW_CAP, view, 9) || memcmp(view, kMapViewCapPin, 9) != 0) return "the war table's map view is not the reviewed one";
    return NULL;
}

/* ---- the install steps ------------------------------------------------------------------------------------------ */

/* ExtraWidget, at the page's start, with the bars' table and their widgets. */
static const char* PrepareExtraWidget(DifficultyInstall* d)
{
    uint8_t* extra = d->code;
    d->extra = extra;
    memcpy(extra, kExtraWidget, sizeof kExtraWidget);
    int32_t rel = (int32_t)(d->widgets - (extra + 10));
    memcpy(extra + 6, &rel, 4);
    rel = (int32_t)(d->widgets + DIF_BARS * 8 - (extra + 56));
    memcpy(extra + 52, &rel, 4);
    return NULL;
}

/* Per reader: cmp the row's level, 11; jne its setter; jmp the icon stub's body. */
static void PrepareIconChecks(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    int32_t rel;
    uint8_t* iconChecks = d->code + 0x9D0;
    d->iconChecks = iconChecks;
    uint8_t* c = iconChecks;
    c[0] = 0x83; c[1] = 0x7F; c[2] = 0x04; c[3] = DIF_VANILLA + 1;              /* cmp dword [rdi+4], 11 */
    c[4] = 0x0F; c[5] = 0x85; rel = (int32_t)(game + DIF_SET_IMAGE - (uintptr_t)(c + 10)); memcpy(c + 6, &rel, 4);
    c[10] = 0xE9; rel = (int32_t)(d->hudIconStub + 9 - (c + 15)); memcpy(c + 11, &rel, 4);
    for (int i = 0; i < 3; i++)
    {
        c = iconChecks + 0x10 * (i + 1);
        memcpy(c, kIconSites[i].check, 5);
        c[5] = 0x0F; c[6] = 0x85; rel = (int32_t)(game + DIF_SET_SPRITE - (uintptr_t)(c + 11)); memcpy(c + 7, &rel, 4);
        c[11] = 0xE9; rel = (int32_t)(d->iconStub + 9 - (c + 16)); memcpy(c + 12, &rel, 4);
    }
}

/* The icon setter, for level 11 (esi): Super Helldive's sprite through the game's setter first (it gives the image
 * element its resource, +0x148, which a texture needs), then the mod's texture in the image slot, the whole texture
 * as the UV rect (+0x134; the sprite's would crop it) and the element refreshed, as 0x1450160 does for a texture.
 * Other levels: the game's setter (a tail jump: the caller's return address stays). */
static const char* PrepareIconStubs(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    int32_t rel;
    uint8_t* iconStub = d->code + 0x40;
    d->iconStub = iconStub;
    memcpy(iconStub, kIconStub, sizeof kIconStub);
    uint64_t superHelldive = 0;
    if (SafeRead(game + kDifficultyCopies[0] + (DIF_VANILLA - 1) * DIF_ROW + 0x10, &superHelldive, 8) || superHelldive == 0)
        return "Super Helldive's icon is unreadable";
    memcpy(iconStub + 20, &g_difficulty.icon, 8);
    memcpy(iconStub + 30, &superHelldive, 8);
    rel = (int32_t)(game + DIF_SET_SPRITE - (uintptr_t)(iconStub + 9)); memcpy(iconStub + 5, &rel, 4);
    rel = (int32_t)(game + DIF_SET_SPRITE - (uintptr_t)(iconStub + 46)); memcpy(iconStub + 42, &rel, 4);
    rel = (int32_t)(game + DIF_SET_SLOT - (uintptr_t)(iconStub + 70)); memcpy(iconStub + 66, &rel, 4);
    rel = (int32_t)(game + DIF_REFRESH_IMAGE - (uintptr_t)(iconStub + 131)); memcpy(iconStub + 127, &rel, 4);
    uint8_t* hudIconStub = d->code + 0x940; /* kIconStub with the HUD's base sprite (what it passes 0x1450230) */
    d->hudIconStub = hudIconStub;
    {
        uint64_t hudBase;
        memcpy(&hudBase, kHudIconPin + 13, 8);
        memcpy(hudIconStub, iconStub, sizeof kIconStub);
        memcpy(hudIconStub + 30, &hudBase, 8);
        rel = (int32_t)(game + DIF_SET_SPRITE - (uintptr_t)(hudIconStub + 46)); memcpy(hudIconStub + 42, &rel, 4);
        rel = (int32_t)(game + DIF_SET_SLOT - (uintptr_t)(hudIconStub + 70)); memcpy(hudIconStub + 66, &rel, 4);
        rel = (int32_t)(game + DIF_REFRESH_IMAGE - (uintptr_t)(hudIconStub + 131)); memcpy(hudIconStub + 127, &rel, 4);
    }
    PrepareIconChecks(d);
    return NULL;
}

/* Per site: cmp eax, 10; jae 11th; its instructions; jmp back. 11th: mov rcx, bar; call ExtraWidget; after; jmp back. */
static const char* PrepareBarStubs(DifficultyInstall* d)
{
    uint8_t* at = d->code + 0x100;
    for (size_t i = 0; i < sizeof kBarSites / sizeof kBarSites[0]; i++)
    {
        const BarSite* s = &kBarSites[i];
        uintptr_t back = d->game + s->rva + s->length;
        uint8_t rest = (uint8_t)(s->length - s->keep);
        int32_t rel;
        d->barStubs[i] = at;
        memcpy(at, s->original, s->keep); at += s->keep;
        at[0] = 0x83; at[1] = 0xF8; at[2] = (uint8_t)DIF_VANILLA; at[3] = 0x73; at[4] = (uint8_t)(rest + 5); at += 5;
        memcpy(at, s->original + s->keep, rest); at += rest;
        rel = (int32_t)(back - (uintptr_t)(at + 5)); at[0] = 0xE9; memcpy(at + 1, &rel, 4); at += 5;
        at[0] = 0x48; at[1] = 0x89; at[2] = s->bar; at += 3;
        rel = (int32_t)(d->extra - (at + 5)); at[0] = 0xE8; memcpy(at + 1, &rel, 4); at += 5;
        memcpy(at, s->after, s->afterLength); at += s->afterLength;
        rel = (int32_t)(back - (uintptr_t)(at + 5)); at[0] = 0xE9; memcpy(at + 1, &rel, 4); at += 5;
        at = (uint8_t*)(((uintptr_t)at + 15) & ~(uintptr_t)15);
    }
    return NULL;
}

/* A larger map's hologram scale stubs: 4 x 0x80, then the constants. */
static const char* PrepareHoloStubs(DifficultyInstall* d)
{
    d->holoStubs = d->code + 0xA20;
    if (!LargerMaps()) return NULL;
    float* values = (float*)(d->holoStubs + 4 * 0x80);
    values[0] = 0.19f; values[1] = 0.45f; values[2] = 1.0f; values[3] = 6.6f;
    for (int i = 0; i < 4; i++)
    {
        uint8_t* stub = d->holoStubs + 0x80 * i;
        memcpy(stub, kHoloStubs[i].code, kHoloStubs[i].size);
        for (int j = 0; j < 7 && kHoloStubs[i].slots[j].at != 0; j++)
        {
            uintptr_t target = kHoloStubs[i].slots[j].target < 4 ? (uintptr_t)&values[kHoloStubs[i].slots[j].target] : d->game + kHoloStubs[i].site + kHoloStubs[i].length;
            int32_t rel = (int32_t)(target - (uintptr_t)(stub + kHoloStubs[i].slots[j].at + 4));
            memcpy(stub + kHoloStubs[i].slots[j].at, &rel, 4);
        }
    }
    return NULL;
}

/* A larger map's mission card view stub, its constants after it and the marker scales in the watch (+0x48). */
static const char* PrepareCardViewStub(DifficultyInstall* d)
{
    uint8_t* cardViewStub = d->code + 0x860;
    d->cardViewStub = cardViewStub;
    if (!LargerMaps()) return NULL;
    float constants[8] = {0.38f, 1.0f, 0.5f / 0.225f, 0.225f, 0.275f, 0.725f, 0.0f, 512.0f};
    memcpy(&constants[6], &kCardScaleValues[0], 4);
    uint8_t* values = cardViewStub + 0xC0;
    memcpy(values, constants, sizeof constants);
    memcpy(d->watch + 0x48, kCardScaleValues, sizeof kCardScaleValues);
    memcpy(cardViewStub, kCardViewStub, sizeof kCardViewStub);
    for (int i = 0; i < 14; i++)
    {
        uint8_t t = kCardViewSlots[i].target;
        uintptr_t target = t < 8 ? (uintptr_t)(values + 4 * t) : t < 12 ? (uintptr_t)(d->watch + 0x48 + 4 * (t - 8)) : d->game + DIF_CARD_VIEW_CALL;
        int32_t rel = (int32_t)(target - (uintptr_t)(cardViewStub + kCardViewSlots[i].at + 4));
        memcpy(cardViewStub + kCardViewSlots[i].at, &rel, 4);
    }
    return NULL;
}

/* The hologram's label cap, and the constants its stub reads (1.0, 0.17 / 0.19, 0.17). */
static const char* PrepareMapViewStub(DifficultyInstall* d)
{
    float* viewValues = (float*)(d->code + 0x750);
    d->viewValues = viewValues;
    viewValues[0] = 1.0f; viewValues[1] = 0.17f / 0.19f; viewValues[2] = 0.17f;
    uint8_t* viewStub = d->code + 0x760;
    d->viewStub = viewStub;
    memcpy(viewStub, kMapViewStub, sizeof kMapViewStub);
    int32_t rel = (int32_t)((uint8_t*)&viewValues[1] - (viewStub + 9)); memcpy(viewStub + 5, &rel, 4);
    rel = (int32_t)((uint8_t*)&viewValues[2] - (viewStub + 23)); memcpy(viewStub + 19, &rel, 4);
    rel = (int32_t)(d->game + DIF_MAPVIEW_CAP_NEXT - (uintptr_t)(viewStub + 33)); memcpy(viewStub + 29, &rel, 4);
    return NULL;
}

/* The war table's mission board: its three stubs, jumped to, and the regions' highest level 11. */
static const char* InstallBoard(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    uint32_t level = DIF_VANILLA + 1;
    int32_t rel;
    uint8_t* board = AllocateNearGame(game, 0x200);
    if (board == NULL) return "no memory within 2 GB of game.dll for the mission board";
    for (int i = 0; i < 3; i++)
    {
        uint8_t* stub = board + 0x80 * i;
        memcpy(stub, kBoardSites[i].stub, kBoardSites[i].size);
        rel = (int32_t)(game + kBoardSites[i].back - (uintptr_t)(stub + kBoardSites[i].backAt + 4)); memcpy(stub + kBoardSites[i].backAt, &rel, 4);
        rel = (int32_t)(game + kBoardSites[i].skip - (uintptr_t)(stub + kBoardSites[i].skipAt + 4)); memcpy(stub + kBoardSites[i].skipAt, &rel, 4);
    }
    DWORD boardProtect;
    VirtualProtect(board, 0x200, PAGE_EXECUTE_READ, &boardProtect);
    for (int i = 0; i < 3; i++)
    {
        uint8_t jump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
        rel = (int32_t)((uintptr_t)(board + 0x80 * i) - (game + kBoardSites[i].site + 5)); memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kBoardSites[i].site, jump, kBoardSites[i].length)) return "the war table's mission board could not be given level 11";
    }
    if (ApiWrite(game + DIF_BOARD_REGION_MAX + 1, &level, 4)) return "the regions' missions could not reach level 11";
    return NULL;
}

/* The band sprites' read moved, the icon readers into their stubs (a level with an icon), the bar's sites into theirs. */
static const char* MoveBandsAndIcons(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    int32_t rel;
    int32_t bandDisp = (int32_t)(int64_t)((uintptr_t)d->bands - game);
    if (ApiWrite(game + DIF_BAND_SITE + 4, &bandDisp, 4)) return "the band sprites could not be moved";
    uint8_t iconCallNew[5] = {0xE8};
    rel = (int32_t)((uintptr_t)d->iconStub - (game + DIF_ICON_CALL + 5));
    memcpy(iconCallNew + 1, &rel, 4);
    if (g_difficulty.icon != 0 && ApiWrite(game + DIF_ICON_CALL, iconCallNew, 5)) return "the difficulty icon setter could not be extended";
    for (int i = 0; i < 4 && g_difficulty.icon != 0; i++)
    {
        uintptr_t call = i == 0 ? game + DIF_HUD_ICON_CALL : game + kIconSites[i - 1].call;
        rel = (int32_t)((uintptr_t)(d->iconChecks + 0x10 * i) - (call + 5));
        memcpy(iconCallNew + 1, &rel, 4);
        if (ApiWrite(call, iconCallNew, 5)) return "a difficulty icon reader could not be extended";
    }
    for (size_t i = 0; i < sizeof kBarSites / sizeof kBarSites[0]; i++)
    {
        uint8_t jump[32];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        rel = (int32_t)((uintptr_t)d->barStubs[i] - (game + kBarSites[i].rva + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kBarSites[i].rva, jump, kBarSites[i].length)) return "the bar could not be given its 11th widget";
    }
    return NULL;
}

/* The hologram labels' first min takes 1.0, the second goes through its stub. */
static const char* JumpToMapView(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    int32_t toOne = (int32_t)((uintptr_t)&d->viewValues[0] - (game + DIF_MAPVIEW_MIN + 9));
    uint8_t cap[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    int32_t rel = (int32_t)((uintptr_t)d->viewStub - (game + DIF_MAPVIEW_CAP + 5));
    memcpy(cap + 1, &rel, 4);
    if (ApiWrite(game + DIF_MAPVIEW_MIN + 5, &toOne, 4) || ApiWrite(game + DIF_MAPVIEW_CAP, cap, sizeof cap))
        return "the war table's map view could not be widened";
    return NULL;
}

/* A larger map's mission card markers (their scales from the watch), map view and hologram scale. */
static const char* JumpToCardStubs(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    int32_t rel;
    for (int i = 0; i < 9; i++)
    {
        uintptr_t site = game + kCardScales[i].rva;
        int32_t disp = (int32_t)((uintptr_t)(d->watch + 0x48 + 4 * kCardScales[i].global) - (site + kCardScales[i].length));
        if (ApiWrite(site + kCardScales[i].at, &disp, 4)) return "the mission card's markers could not be scaled";
    }
    uint8_t view[42];
    memset(view, 0x90, sizeof view);
    view[0] = 0xE9;
    rel = (int32_t)((uintptr_t)d->cardViewStub - (game + DIF_CARD_VIEW_SITE + 5));
    memcpy(view + 1, &rel, 4);
    if (ApiWrite(game + DIF_CARD_VIEW_SITE, view, sizeof view)) return "the mission card's map view could not be widened";
    for (int i = 0; i < 4; i++)
    {
        uint8_t jump[10];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        rel = (int32_t)((uintptr_t)(d->holoStubs + 0x80 * i) - (game + kHoloStubs[i].site + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kHoloStubs[i].site, jump, kHoloStubs[i].length)) return "the war table hologram's mission scale could not be fitted";
    }
    return NULL;
}
