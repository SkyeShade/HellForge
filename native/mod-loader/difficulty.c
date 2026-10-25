/* A new difficulty level (SDK 5, docs/modding/content.md "A difficulty"): level 11, after Super Helldive, a copy of level
 * 10's settings with its own name, unlock item and enemy budgets. Included by hellforge_modloader.c, after registry.c
 * (text, registered items) and damage.c (AllocateNearGame).
 *
 * The game's difficulties (build 25480438, game.dll 2E2C3B7C...):
 *   A static table of 10 rows of 0x330 bytes, in 19 identical copies (one per source file that includes it, each filled
 *   by its own static initializer when game.dll loads). Data follows every copy, so HellForge moves the table: one
 *   copy with 11 rows (and 4 zero rows the unlock mask reads) replaces all 19. 57 instructions address the copies
 *   (difficulty_sites.h), each `lea` of a copy's first row (or a field read on it) indexed by level - 1.
 *   Row: +0 the unlock item (an inventory item; the backend's catalogue holds it), +4 level, +8 name text (capitals:
 *   the war table's bar), +0xC the level's number as text (the mission's top bar: "10"), +0x10 icon sprite, +0xE4 encounter points, +0xE8 guardforce points, +0xEC tech level (enemy
 *   rosters hold 10 weights: level 11 keeps tech level 10), +0x104 the shared spawner cooldown.
 *   0x11EBB40  the highest level, min(10, a config override): every table read clamps the level to it. With its 10
 *              and the API's other bounds (row by id, level from id, the unlocked mask and highest unlocked level)
 *              raised to 11, level 11 reads the new row and can be unlocked and chosen.
 *   Two (level - 1) / (max - 1) scales keep 10 (kDifficultyKeepTen), so levels 1 .. 10 stay as they are.
 *   Mission package loading clamps the level to 10 itself, so enemy packages load as for level 10.
 *   The war table's difficulty bar is a carousel of one widget per level, and its object holds exactly 10 (bar + 0x598
 *   + i * 0x910; another member follows). Its build, update, destroy and choice loops run to the highest level, and the
 *   five instructions that turn an index into a widget's address (kBarSites) go through stubs: index 10 is the bar's
 *   11th widget, a block of its own (ExtraWidget). The chosen widget shows its level's number, name and icon
 *   (0x19A28D0, which reads the row through the moved table), and a per-level band sprite from a 10-entry array
 *   (0x331B950) read by level - 1, unclamped: it moves too, level 11 taking level 10's. */

typedef struct DifficultySite { uint32_t rva; uint8_t dispAt, size, rip, copy; uint32_t field; } DifficultySite;
typedef struct DifficultyBound { uint32_t rva; uint8_t immAt, width; } DifficultyBound;
#include "difficulty_sites.h"
#include "location_sites.h"

#define DIF_ROW 0x330u
#define DIF_VANILLA 10u
#define DIF_ROWS 15u          /* the unlocked mask reads 5 rows a step: up to row 15 once it reaches 11 */
#define DIF_MAX_FN 0x11EBB40u
#define DIF_BANDS 0x331B950u      /* u64 band sprite per level, read at 0x19A297C (mov rdx, [r14 + rax*8 + disp32]) */
#define DIF_BAND_SITE 0x19A297Cu
/* Which difficulties offer an operation and a mission: the operation settings (25 rows of 0x490) and the missions (162
 * rows of 0x380), each with its lowest and highest difficulty at +0xC / +0x10. The war table's mission board only offers
 * a level operations allow (0x11E4CD0), so every row allowing up to 10 allows 11. */
#define DIF_OPERATIONS 0x32FEF10u
#define DIF_OPERATION_ROWS 25u
#define DIF_OPERATION_ROW 0x490u
#define DIF_MISSIONS 0x3773420u
#define DIF_MISSION_ROWS 162u
#define DIF_MISSION_ROW 0x380u
/* Objectives: 153 definitions of 0xA0, +0x28 the highest difficulty they are
 * drawn at (0x1756D06..0x1756D1A skips the rest; 148 stop at 10, so a level-11 mission had none, not even its primary). */
#define DIF_OBJECTIVES 0x32EF870u
#define DIF_OBJECTIVE_ROWS 153u
#define DIF_OBJECTIVE_ROW 0xA0u
/* Mission distance factors: 10 levels x 3 floats, read by (level - 1) * 3 + kind at 7 sites, unclamped: moved, level 11
 * taking level 10's. */
#define DIF_DISTANCE 0x2143170u
static const uint32_t kDistanceSites[7][2] = {{0xF417C8u, 5}, {0x7BDEE8u, 4}, {0xF62EDBu, 4}, {0xF7D9B6u, 6}, {0xF7E506u, 6},
    {0xF82CF6u, 6}, {0xF84BA6u, 6}};
/* Enemy rosters: six descriptors of {rows, count} and eight composition
 * families (the sixth, one row, is the third of 0x90E2A5's choices with the fourth and fifth, read by the same readers). A roster row (0x80) holds a spawn weight per tech level 1..10 at +0x30; +0x28..+0x2F are unused (zero), so
 * the weights move to +0x2C and level 11's goes to +0x54 (the six readers' displacements follow). Compositions (0x118)
 * allow tech levels +0xEC..+0xF0: those up to 10 allow 11. The director clamps the tech level to 10 at 0x94CA13. */
static const struct { uint32_t descriptor, rows; } kRosters[6] = {{0x21CBD60u, 45}, {0x21CE910u, 46}, {0x21CBCB0u, 61},
    {0x21CB3C0u, 12}, {0x21CB890u, 27}, {0x21CB470u, 1}};
#define DIF_ROSTERS (sizeof kRosters / sizeof kRosters[0])
static const uint32_t kWeightReaders[6][2] = {{0x94C1E9u, 5}, {0x94C26Fu, 6}, {0x94C2E3u, 6}, {0x95321Bu, 5}, {0x9532BDu, 5},
    {0xABD6ACu, 4}};
#define DIF_TECH_CLAMP 0x94CA13u      /* mov eax, 0xa */
#define DIF_COMPOSITIONS_UP_TO_TEN 319u
/* Larger maps. The route-cell generator (0xFD03E0) fills fixed arrays without checking them: vertices 975 (850 at
 * level 10), edges 1621, cells 1024. Its sites are a Poisson disc of the generation settings' spacing (+4, 40 m) in a
 * disk of +0xC (680 m) around the world's centre (the world is 2048 m), so a larger playable radius alone overflows
 * them (1.25 x crashed at 0xF6D9A0, 1.6 x at 0xC182E7). A level above 1 x gets, per mission, a site disk and fade
 * radius that cover its border and a spacing that keeps level 10's number of sites: level init (0xF71220) copies the
 * settings' radii into the level (lg+0x2D78..+0x2D8C) and the two spacing reads follow the level's radius.
 * GenerateVoronoi (op 0x25, 0xF3F8E0) lays a 16 m grid (settings +0) over the same square, keeps the points within the
 * disk and runs jc_voronoi on them: level 10 makes 5672 sites, 5839 vertices and ~11430 edges against arrays of 5680,
 * 5849 and 11528, unchecked. Its spacing grows with the disk, rounded up to a quarter metre: grid points exact in
 * float, as level 10's are. An inexact spacing (24.06 m) gives near-degenerate circles and 7275 vertices; the overrun
 * array then fails the pass's own vertex check (0xF4003F) and the pass retries forever. 2 x: 24.5 m, 5468 sites,
 * 5635 vertices, ~11014 edges (game.dll's jc_voronoi run offline). The structured
 * maps (map type 2: mega cities, factories) grow too: their playable
 * radius is the distance table's (0x2143170, per level and sub-kind), scaled for level 11; their sites are a fixed
 * template stretched over the rect (0xFB9610), and revoronoi (op 0xA3, 0xFD0E00) refills the land around the city with a
 * Poisson disc at the nature cell size, which the revoronoi stubs grow like the route cells' (kReVoronoiStubs). */
#define DIF_MAP_SCALE_MAX 2.0f
#define DIF_MAP_TYPE_SITE 0xF71CD8u    /* mov [r13 + 0x2de0], rdx: the map type, after the radii (r13 = lg) */
#define DIF_SPACING_SITE 0xF3614Du     /* movss xmm2, [rax + 4]: the sites' spacing (r14 = pass, [r14 + 0x18] = lg) */
#define DIF_SPACING_SITE_2 0xF3732Cu   /* subss xmm6, [rax + 4]: a cell's distance less the spacing (lg = [[rsp + 0x60] + 0x18]) */
/* GenerateRouteCellVoronoi (op 3, 0xF93800) fills each route cell with points at its cell type's size (+0x18C) / 2
 * (grid) or / 4 (Poisson) and builds their diagrams: level 10 makes 11954 areas of 13224 (G+0xEE10C8) and 23673
 * vertices of 26205, unchecked, in proportion to the generated area. An overrun crashed GenerateSubregionHeights
 * (0xF64878). The spacing grows with the disk (x 1.04 for room), rounded up to a quarter metre. */
#define DIF_CELL_SPACING_SITE 0xF9420Fu /* divss xmm2, xmm0; mov rdx, [rdx] (rdx = pass, [rdx + 0x18] = lg) */
/* FinalizeVoronoi (op 0xA4, 0xF68050), on map type 1, rebuilds the route cells from points laid over the whole site
 * square at each cell type's size (+0x18C), grid or Poisson: 2.25 x the points of level 10 overran the 1024 route
 * cells (crash in its edge sort, 0xFCF838, at cell 1024). Same scaling as the route cells' areas. */
#define DIF_FINALIZE_SPACING_SITE 0xF68273u /* movss xmm2, [r13 + 0x18c] ([rsp + 0x70] = pass, [pass + 0x18] = lg) */
#define DIF_GRID_SITE 0xF3FB04u        /* mov [rsp+0x20], r14; movss xmm2, [rax]: GenerateVoronoi's grid (rsi = pass, [rsi + 0x18] = lg) */
static const uint8_t kMapTypePin[7] = {0x49, 0x89, 0x95, 0xE0, 0x2D, 0x00, 0x00};
static const uint8_t kSpacingPin[5] = {0xF3, 0x0F, 0x10, 0x50, 0x04};
static const uint8_t kSpacingPin2[5] = {0xF3, 0x0F, 0x5C, 0x70, 0x04};
/* Diagnostics: a pass that fails asks for the level to be generated again (lg+0xF954 counts the restarts, lg+0xF958
 * names the first failed pass). Each such increment notes the level, and the loader's watcher logs the restarts. They
 * run only when a pass fails. FixUnreachableAreas, FinalizeVoronoi, SpreadLocations, GenerateConvoyRoutes. */
static const uint32_t kRestartSites[4] = {0xF67FDBu, 0xF6811Cu, 0xF7CBEAu, 0xF8B2C7u};
static const char* const kRestartPasses[4] = {"FixUnreachableAreas", "FinalizeVoronoi", "SpreadLocations", "GenerateConvoyRoutes"};
static const uint8_t kRestartPin[6] = {0xFF, 0x81, 0x54, 0xF9, 0x00, 0x00}; /* inc dword [rcx + 0xf954] (rcx = lg) */
static const uint8_t kRestartStub[25] = {
    0x48, 0x89, 0x0D, 0, 0, 0, 0,                                     /*  0 mov [rip+last level], rcx (rel32 at 3) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /*  7 lock inc dword [rip+site count] (rel32 at 10) */
    0xFF, 0x81, 0x54, 0xF9, 0x00, 0x00,                               /* 14 inc dword [rcx+0xf954] */
    0xE9, 0, 0, 0, 0};                                                /* 20 jmp back (rel32 at 21) */
/* On a larger map FixUnreachableAreas (0xF67980) can fail on every try: some pocket of route cells has no walkable
 * neighbour to open (0xF66B80), and the level is generated again forever (Sabotage Supply Bases at 2 x: 35+ tries; the
 * preview never came). For a larger map's level, from the 7th try the pocket is left as it is and the pass goes on
 * (0xF67FB7, the path after a fixed area). */
#define DIF_UNREACHABLE_GO_ON 0xF67FB7u
#define DIF_UNREACHABLE_TRIES 6
static const uint8_t kUnreachableStub[58] = {
    0x48, 0x89, 0x0D, 0, 0, 0, 0,                                     /*  0 mov [rip+last level], rcx (rel32 at 3) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /*  7 lock inc dword [rip+site count] (rel32 at 10) */
    0x81, 0xB9, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /* 14 cmp dword [rcx+0x2d7c], disk / 1024 (at 20) */
    0x75, 0x15,                                                       /* 24 jne 47 */
    0x83, 0xB9, 0x54, 0xF9, 0x00, 0x00, DIF_UNREACHABLE_TRIES,        /* 26 cmp dword [rcx+0xf954], tries */
    0x72, 0x0C,                                                       /* 33 jb 47 */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 35 lock inc dword [rip+given up] (rel32 at 38) */
    0xE9, 0, 0, 0, 0,                                                 /* 42 jmp go on (rel32 at 43) */
    0xFF, 0x81, 0x54, 0xF9, 0x00, 0x00,                               /* 47 inc dword [rcx+0xf954] */
    0xE9, 0, 0, 0, 0};                                                /* 53 jmp back (rel32 at 54) */
/* Diagnostic: CreateMinimapTexture (0xF586A0, r8 = pass, [r8 + 8] = lg+0x2D20) notes the radii it captures with. */
#define DIF_CAPTURE_SITE 0xF586A0u
static const uint8_t kCapturePin[11] = {0x4C, 0x8B, 0xDC, 0x55, 0x49, 0x8D, 0xAB, 0x98, 0xFD, 0xFF, 0xFF};
static const uint8_t kCaptureStub[71] = {
    0x50, 0x52,                                                       /*  0 push rax; push rdx */
    0x49, 0x8B, 0x40, 0x08,                                           /*  2 mov rax, [r8+8] (lg+0x2D20) */
    0x8B, 0x50, 0x50,                                                 /*  6 mov edx, [rax+0x50] (lg+0x2D70) */
    0x89, 0x15, 0, 0, 0, 0,                                           /*  9 mov [rip+watch+0x28], edx (rel32 at 11) */
    0x8B, 0x50, 0x54,                                                 /* 15 mov edx, [rax+0x54] (lg+0x2D74) */
    0x89, 0x15, 0, 0, 0, 0,                                           /* 18 mov [rip+watch+0x2c], edx (rel32 at 20) */
    0x8B, 0x50, 0x60,                                                 /* 24 mov edx, [rax+0x60] (lg+0x2D80) */
    0x89, 0x15, 0, 0, 0, 0,                                           /* 27 mov [rip+watch+0x30], edx (rel32 at 29) */
    0x0F, 0xB6, 0x90, 0xD9, 0xD3, 0xFF, 0xFF,                         /* 33 movzx edx, byte [rax-0x2c27] (lg+0xF9) */
    0x89, 0x15, 0, 0, 0, 0,                                           /* 40 mov [rip+watch+0x38], edx (rel32 at 42) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 46 lock inc dword [rip+watch+0x34] (rel32 at 49) */
    0x5A, 0x58,                                                       /* 53 pop rdx; pop rax */
    0x4C, 0x8B, 0xDC, 0x55, 0x49, 0x8D, 0xAB, 0x98, 0xFD, 0xFF, 0xFF, /* 55 mov r11, rsp; push rbp; lea rbp, [r11-0x268] */
    0xE9, 0, 0, 0, 0};                                                /* 66 jmp 0xF586AB (rel32 at 67) */
/* The SelectedMission telemetry line (0x148F2A0, when a mission is chosen on the war table) writes
 * {"difficulty":..,"race":..,"game_mode":..,"objectives":["name",...]} into a 512-byte stack buffer; its appends add
 * vsnprintf's would-be length, so a list past the buffer turns the space left negative and writes past it (the stack
 * cookie ended the game: Hell Forge's objectives on a hive world). It lists the first 8 objectives: the count the
 * objective selection returns (0x148F717) is limited before the loop. */
#define DIF_TELEMETRY_SITE 0x148F717u
static const uint8_t kTelemetryPin[5] = {0x44, 0x8B, 0xE0, 0x85, 0xC0};
static const uint8_t kTelemetryStub[20] = {
    0x83, 0xF8, 0x08,                                                 /*  0 cmp eax, 8 */
    0x76, 0x05,                                                       /*  3 jbe 10 */
    0xB8, 0x08, 0x00, 0x00, 0x00,                                     /*  5 mov eax, 8 */
    0x44, 0x8B, 0xE0,                                                 /* 10 mov r12d, eax */
    0x85, 0xC0,                                                       /* 13 test eax, eax */
    0xE9, 0, 0, 0, 0};                                                /* 15 jmp 0x148F71C (rel32 at 16) */
/* The enemy director's spawners (HiveMind manager +0x82D98, 128 x 0x58, count +0x85998 right after them) are added
 * at mission load from the map's 'spawner' locations (0xAD3778) without a check: a 2 x map with 5 x outposts had more,
 * the 129th overwrote the count and the next write crashed (0xAD37AB). Past the 128th they are left out (counted in
 * the watch, +0x3C): their outposts stay, the director can't spawn from them. */
#define DIF_SPAWNER_SITE 0xAD3778u
#define DIF_SPAWNER_ADD 0xAD377Eu
#define DIF_SPAWNER_DONE 0xAD390Bu
static const uint8_t kSpawnerPin[6] = {0x8B, 0x83, 0x98, 0x59, 0x08, 0x00};
static const uint8_t kSpawnerStub[30] = {
    0x8B, 0x83, 0x98, 0x59, 0x08, 0x00,                               /*  0 mov eax, [rbx+0x85998] (the count) */
    0x3D, 0x80, 0x00, 0x00, 0x00,                                     /*  6 cmp eax, 128 */
    0x73, 0x05,                                                       /* 11 jae 18 */
    0xE9, 0, 0, 0, 0,                                                 /* 13 jmp 0xAD377E (the add; rel32 at 14) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 18 lock inc dword [rip+watch+0x3c] (rel32 at 21) */
    0xE9, 0, 0, 0, 0};                                                /* 25 jmp 0xAD390B (after the add; rel32 at 26) */
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
/* The mission countdown is HiveMindComponent +0x74 (0x95F301 copies it into the countdown slot and the net field;
 * 0x506580's readers take it as the duration). Missions change it through entity deltas, as High Value Asset missions
 * get 1200 s: launch (0xAD0010) lists the mission row's delta (0xAD0DDA) and the modifiers' (0xAD0E10), up to 10
 * resource hashes at rbp+0x8D0 (count) / +0x8D8, and the spawn (0x5740B0, HiveMind case 0x57525C) gives the mission's
 * HiveMind a private copy of the shared definition (0x505F50) with them applied. The level's countdown is a delta of
 * its own, registered in the game's entity delta table (DifficultyDelta) with one HiveMind (0xD4) member, +0x74 = the
 * level's seconds, and appended at 0xAD0E4E when the mission's level (lg+0xF9) is the added one. The shared
 * definition is never written, so every other level keeps the mission's own countdown. The host clamps the countdown
 * every frame to its network definition (207: name 0x72A6AB0E, float, 16 bits, 0..2500 s, at the network config's
 * defs, stride 24: +8 change threshold, +0xC type, +0xD bits, +0x10 min, +0x14 max), so 2500 becomes the level's
 * seconds + 60 (players all run HellForge: the config hash covers it). */
#define DIF_TIMER_SITE 0x95F301u      /* movss xmm0, [rax+0x74]: checked as the countdown's member, not changed */
#define DIF_LAUNCH_SITE 0xAD0E4Eu
#define DIF_LAUNCH_BACK 0xAD0E5Au
#define DIF_DELTA_TABLE 0x346BF98u    /* [game+..]+0xF12E98: the entity delta table (0x5154C0) */
#define DIF_DELTA_HOLDER 0xF12E98u
#define DIF_HIVEMIND 0xD4u            /* HiveMindComponent's component type in deltas */
#define DIF_COUNTDOWN_DEF 0x72A6AB0Eu
static const uint8_t kTimerPin[5] = {0xF3, 0x0F, 0x10, 0x40, 0x74};
static const uint8_t kLaunchPin[12] = {0x49, 0x8B, 0x82, 0x88, 0xE2, 0x0A, 0x00, 0x44, 0x8B, 0x64, 0x24, 0x30};
static const uint8_t kLaunchStub[79] = {
    0x49, 0x8B, 0x82, 0x88, 0xE2, 0x0A, 0x00,                         /*  0 mov rax, [r10+0xae288] (the level) */
    0x80, 0xB8, 0xF9, 0x00, 0x00, 0x00, 0x0B,                         /*  7 cmp byte [rax+0xf9], 11 */
    0x75, 0x35,                                                       /* 14 jne 69 */
    0x80, 0x3D, 0, 0, 0, 0, 0x00,                                     /* 16 cmp byte [rip+watch+0x40], 0 (rel32 at 18) */
    0x74, 0x2C,                                                       /* 23 je 69 (the delta is not registered) */
    0x8B, 0x8D, 0xD0, 0x08, 0x00, 0x00,                               /* 25 mov ecx, [rbp+0x8d0] (deltas listed) */
    0x83, 0xF9, 0x0A,                                                 /* 31 cmp ecx, 10 */
    0x73, 0x21,                                                       /* 34 jae 69 */
    0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,                               /* 36 mov rdx, the delta's hash (imm64 at 38) */
    0x48, 0x89, 0x94, 0xCD, 0xD8, 0x08, 0x00, 0x00,                   /* 46 mov [rbp+rcx*8+0x8d8], rdx */
    0xFF, 0xC1,                                                       /* 54 inc ecx */
    0x89, 0x8D, 0xD0, 0x08, 0x00, 0x00,                               /* 56 mov [rbp+0x8d0], ecx */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 62 lock inc dword [rip+watch+0x44] (rel32 at 65) */
    0x44, 0x8B, 0x64, 0x24, 0x30,                                     /* 69 mov r12d, [rsp+0x30] */
    0xE9, 0, 0, 0, 0};                                                /* 74 jmp 0xAD0E5A (rel32 at 75) */
/* Two displays read the countdown from the shared definition rather than the mission's: the war table's mission card
 * (0x1A12D70: the mode's definition from 0x505F50 with only the mission row's delta, +0x360, applied in a copy; every
 * path ends at 0x1A131CC with the seconds in xmm2, divided by 60 into the card's "40 MINUTES") and the mission's timer
 * widget (0x17BA940: the total at 0x17BAA44, less the countdown left (manager +0x51948), is the elapsed time and its
 * segments). At the added level both take the countdown delta's own value (watch+0x58: its data in the game's delta
 * table, 0 until registered). The card's level is its mission descriptor's (rsi, +9, as lg+0xF0); the widget's the
 * mission's level (manager [game+0x3326D10] +0x650, +0xF9). */
#define DIF_CARD_TIME_SITE 0x1A131CCu
#define DIF_CARD_TIME_BACK 0x1A131D4u
#define DIF_MINUTE 0x23C7740u         /* 60.0 */
#define DIF_WIDGET_TIME_SITE 0x17BAA44u
#define DIF_WIDGET_TIME_BACK 0x17BAA51u
#define DIF_HIVEMIND_MANAGER 0x3326D10u
static const uint8_t kCardTimePin[8] = {0xF3, 0x0F, 0x5E, 0x15, 0x6C, 0x45, 0x9B, 0x00}; /* divss xmm2, [rip+60.0] */
static const uint8_t kCardTimeStub[35] = {
    0x80, 0x7E, 0x09, 0x0B,                                           /*  0 cmp byte [rsi+9], 11 */
    0x75, 0x10,                                                       /*  4 jne 22 */
    0x48, 0x8B, 0x05, 0, 0, 0, 0,                                     /*  6 mov rax, [rip+watch+0x58] (rel32 at 9) */
    0x48, 0x85, 0xC0,                                                 /* 13 test rax, rax */
    0x74, 0x04,                                                       /* 16 je 22 */
    0xF3, 0x0F, 0x10, 0x10,                                           /* 18 movss xmm2, [rax] */
    0xF3, 0x0F, 0x5E, 0x15, 0, 0, 0, 0,                               /* 22 divss xmm2, [rip+60.0] (rel32 at 26) */
    0xE9, 0, 0, 0, 0};                                                /* 30 jmp 0x1A131D4 (rel32 at 31) */
static const uint8_t kWidgetTimePin[13] = {0xF3, 0x44, 0x0F, 0x10, 0x50, 0x74, 0x48, 0x8B, 0x05, 0xBF, 0xC2, 0xB6, 0x01};
static const uint8_t kWidgetTimeStub[75] = {
    0xF3, 0x44, 0x0F, 0x10, 0x50, 0x74,                               /*  0 movss xmm10, [rax+0x74] */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  6 mov rax, game+0x3326D10 (imm64 at 8) */
    0x48, 0x8B, 0x00,                                                 /* 16 mov rax, [rax] (the manager) */
    0x48, 0x8B, 0x80, 0x50, 0x06, 0x00, 0x00,                         /* 19 mov rax, [rax+0x650] (the level) */
    0x48, 0x85, 0xC0,                                                 /* 26 test rax, rax */
    0x74, 0x1A,                                                       /* 29 je 57 */
    0x80, 0xB8, 0xF9, 0x00, 0x00, 0x00, 0x0B,                         /* 31 cmp byte [rax+0xf9], 11 */
    0x75, 0x11,                                                       /* 38 jne 57 */
    0x48, 0x8B, 0x05, 0, 0, 0, 0,                                     /* 40 mov rax, [rip+watch+0x58] (rel32 at 43) */
    0x48, 0x85, 0xC0,                                                 /* 47 test rax, rax */
    0x74, 0x05,                                                       /* 50 je 57 */
    0xF3, 0x44, 0x0F, 0x10, 0x10,                                     /* 52 movss xmm10, [rax] */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /* 57 mov rax, game+0x3326D10 (imm64 at 59) */
    0x48, 0x8B, 0x00,                                                 /* 67 mov rax, [rax] */
    0xE9, 0, 0, 0, 0};                                                /* 70 jmp 0x17BAA51 (rel32 at 71) */
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
/* Per-level arrays of 10 values in component records, read with the mission's level and no upper bound
 * ObjectiveComponent's targets (+0xA88: kill counts, collect counts...),
 * ObjectiveEncounterTrigger's timers, ObjectivePreventInvasion's, WorldLootManager's loot counts, and 0xE3F485C7's spawn
 * multipliers. At level 11 each read the field after its array (targets 0: a kill objective could never complete). The
 * arrays cannot grow in place (a live field follows each), so level 11 takes level 10's values: each reader's level is
 * clamped to 10 where it is loaded. In place: the collect pickups (0xA2A87C: its row lookup, a pure call, becomes the
 * clamp), the encounter triggers (0x5E4FAF, 0x5E503F) and the HUD's target (0x17C6AA3) compute (level - 1) with level 11
 * as 10 (cmp al, 11; adc eax, -2; cmovs: 0 stays 0), the loot's difficulty-bar list caps at 10 (0x11EC08B). The other 15
 * loads (movzx reg, byte [lg + 0xF9]; flags dead after) go to a stub: the load, the clamp, back. */
/* The mission's rating (0x19C4E70, the pause map / mission end: a table of 10 rows of thresholds, 0x21E7630, indexed by
 * level - 1 at +0x1C03C, read at 0x19C7DB9 and 0x19C7FF4) loads the level at 0x19C5084 (movzx eax, byte [r12+9]): level 11
 * read past the table and its "count" (0x9571BF) ran the loop off the image at mission end. */
#define DIF_LEVEL_LOADS 16
static const struct { uint32_t rva; uint8_t length, reg; uint8_t pin[8]; } kLevelLoads[DIF_LEVEL_LOADS] = {
    {0x5D5606u, 8, 13, {0x44, 0x0F, 0xB6, 0xA9, 0xF9, 0x00, 0x00, 0x00}}, {0x5DD668u, 8, 11, {0x44, 0x0F, 0xB6, 0x99, 0xF9, 0x00, 0x00, 0x00}},
    {0x5DD788u, 8, 11, {0x44, 0x0F, 0xB6, 0x99, 0xF9, 0x00, 0x00, 0x00}}, {0xA1FA53u, 7, 1, {0x0F, 0xB6, 0x88, 0xF9, 0x00, 0x00, 0x00}},
    {0xA1FB1Fu, 7, 1, {0x0F, 0xB6, 0x88, 0xF9, 0x00, 0x00, 0x00}}, {0xA1FE36u, 7, 1, {0x0F, 0xB6, 0x88, 0xF9, 0x00, 0x00, 0x00}},
    {0xA1FF77u, 7, 0, {0x0F, 0xB6, 0x81, 0xF9, 0x00, 0x00, 0x00}}, {0xA20288u, 7, 2, {0x0F, 0xB6, 0x91, 0xF9, 0x00, 0x00, 0x00}},
    {0xA20AA4u, 7, 1, {0x0F, 0xB6, 0x88, 0xF9, 0x00, 0x00, 0x00}}, {0xA20D60u, 7, 1, {0x0F, 0xB6, 0x88, 0xF9, 0x00, 0x00, 0x00}},
    {0xA21231u, 7, 2, {0x0F, 0xB6, 0x91, 0xF9, 0x00, 0x00, 0x00}}, {0x79C7C8u, 7, 0, {0x0F, 0xB6, 0x83, 0xF9, 0x00, 0x00, 0x00}},
    {0x79CF35u, 7, 0, {0x0F, 0xB6, 0x80, 0xF9, 0x00, 0x00, 0x00}}, {0x9FE952u, 7, 1, {0x0F, 0xB6, 0x88, 0xF9, 0x00, 0x00, 0x00}},
    {0x9FFA9Fu, 7, 1, {0x0F, 0xB6, 0x88, 0xF9, 0x00, 0x00, 0x00}}, {0x19C5084u, 6, 0, {0x41, 0x0F, 0xB6, 0x44, 0x24, 0x09}}};
static const struct { uint32_t rva; uint8_t length; uint8_t pin[16], bytes[16]; } kLevelInPlace[5] = {
    {0xA2A87Cu, 13, {0x41, 0x8B, 0xCC, 0xE8, 0x9C, 0x13, 0x7C, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x13},
     {0xB8, 0x0A, 0x00, 0x00, 0x00, 0x44, 0x3B, 0xE0, 0x44, 0x0F, 0x47, 0xE0, 0x90}},           /* mov eax, 10; cmp r12d, eax; cmova r12d, eax */
    {0x5E4FAFu, 16, {0x0F, 0xB6, 0x90, 0xF9, 0x00, 0x00, 0x00, 0x85, 0xD2, 0x8D, 0x42, 0xFF, 0x41, 0x0F, 0x44, 0xC3},
     {0x0F, 0xB6, 0x80, 0xF9, 0x00, 0x00, 0x00, 0x3C, 0x0B, 0x83, 0xD0, 0xFE, 0x41, 0x0F, 0x48, 0xC3}}, /* eax = level - 1, 11 as 10, 0 as 0 */
    {0x5E503Fu, 16, {0x0F, 0xB6, 0x88, 0xF9, 0x00, 0x00, 0x00, 0x85, 0xC9, 0x8D, 0x41, 0xFF, 0x41, 0x0F, 0x44, 0xC3},
     {0x0F, 0xB6, 0x80, 0xF9, 0x00, 0x00, 0x00, 0x3C, 0x0B, 0x83, 0xD0, 0xFE, 0x41, 0x0F, 0x48, 0xC3}},
    {0x17C6AA3u, 12, {0x8B, 0x40, 0x3C, 0x85, 0xC0, 0x8D, 0x48, 0xFF, 0x41, 0x0F, 0x44, 0xCF},
     {0x8B, 0x40, 0x3C, 0x3C, 0x0B, 0x83, 0xD0, 0xFE, 0x41, 0x0F, 0x48, 0xC7}},                 /* the movss below indexes with rax */
    {0x11EC08Bu, 5, {0xE8, 0xB0, 0xFA, 0xFF, 0xFF}, {0xB8, 0x0A, 0x00, 0x00, 0x00}}};          /* call 0x11EBB40 (the highest level) -> 10 */
#define DIF_HUD_TARGET_SIB 0x17C6AB4u  /* movss xmm0, [r13 + rcx*4 + 0xa88]: SIB 0x8D -> 0x85 (rax) */
/* A clamp of reg (0..15) to 10: cmp reg, 10; jbe over; mov reg, 10. Returns its length. */
static int LevelClamp(uint8_t* at, uint8_t reg)
{
    int n = 0;
    if (reg >= 8) at[n++] = 0x41;
    at[n++] = 0x83; at[n++] = (uint8_t)(0xF8 | (reg & 7)); at[n++] = DIF_VANILLA;
    at[n++] = 0x76; at[n++] = reg >= 8 ? 6 : 5;
    if (reg >= 8) at[n++] = 0x41;
    at[n++] = (uint8_t)(0xB8 | (reg & 7)); at[n++] = DIF_VANILLA; at[n++] = 0; at[n++] = 0; at[n++] = 0;
    return n;
}

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
static void FieldWatch(uintptr_t field); /* behaviours.c: the preview diagnostic's watchpoints */
static volatile uint8_t* g_restartWatch; /* +0 the last level that restarted, +8 a count per site, +0x18 pockets left,
                                          * +0x20 the last level initialised, +0x40 the countdown delta registered,
                                          * +0x44 level-11 missions launched with it, +0x48..+0x54 the mission
                                          * card's marker scales, +0x58 the countdown delta's value (its data) */
/* The generator's stamp index (0xC04B80: placed stamps, locations and roads by area, for overlap tests) has 10757
 * slots (0x2A05) after a bitset; level 10 uses 4892, and the 17 inserters (0xC04EF0) write through a full index's null
 * (PlaceStampWithWeightRule crashed at 0xF3516C). Larger maps get 16383 slots: the same 14 index bits, so the key
 * layout is unchanged. The size (0xC048E0), the layout check (0xC04AB0) and the index (0xC04B80) agree. Every index is
 * sized at creation; levels are generated well after the loader installs this. */
static const struct { uint32_t rva; uint8_t length; uint8_t pin[7], bytes[7]; } kStampIndex[5] = {
    {0xC0498Bu, 5, {0x05, 0x1F, 0xA8, 0x00, 0x00}, {0x05, 0x07, 0x00, 0x01, 0x00}},                     /* add eax, slots x 4 + 7 */
    {0xC04AEAu, 5, {0xB8, 0x05, 0x2A, 0x00, 0x00}, {0xB8, 0xFF, 0x3F, 0x00, 0x00}},                     /* mov eax, slots */
    {0xC04C1Au, 6, {0x41, 0xBD, 0x05, 0x2A, 0x00, 0x00}, {0x41, 0xBD, 0xFF, 0x3F, 0x00, 0x00}},         /* mov r13d, slots */
    {0xC04CB2u, 7, {0x48, 0x8D, 0x81, 0x18, 0xA8, 0x00, 0x00}, {0x48, 0x8D, 0x81, 0x00, 0x00, 0x01, 0x00}}, /* lea rax, [rcx + (slots + 1) x 4] */
    {0xC04CCCu, 7, {0xC7, 0x47, 0x1C, 0x06, 0x2A, 0x00, 0x00}, {0xC7, 0x47, 0x1C, 0x00, 0x40, 0x00, 0x00}}}; /* mov [rdi+0x1c], slots + 1 */
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
static const uint8_t kFinalizeSpacingPin[9] = {0xF3, 0x41, 0x0F, 0x10, 0x95, 0x8C, 0x01, 0x00, 0x00};
static const uint8_t kFinalizeSpacingStub[57] = {
    0xF3, 0x41, 0x0F, 0x10, 0x95, 0x8C, 0x01, 0x00, 0x00,             /*  0 movss xmm2, [r13+0x18c] */
    0x48, 0x8B, 0x44, 0x24, 0x70,                                     /*  9 mov rax, [rsp+0x70] */
    0x48, 0x8B, 0x40, 0x18,                                           /* 14 mov rax, [rax+0x18] */
    0x81, 0xB8, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /* 18 cmp dword [rax+0x2d7c], disk / 1024 (at 24) */
    0x75, 0x16,                                                       /* 28 jne 52 */
    0xF3, 0x0F, 0x59, 0x15, 0, 0, 0, 0,                               /* 30 mulss xmm2, [rip+factor x 4] (rel32 at 34) */
    0x66, 0x0F, 0x3A, 0x0A, 0xD2, 0x02,                               /* 38 roundss xmm2, xmm2, ceil */
    0xF3, 0x0F, 0x59, 0x15, 0, 0, 0, 0,                               /* 44 mulss xmm2, [rip+0.25] (rel32 at 48) */
    0xE9, 0, 0, 0, 0};                                                /* 52 jmp back (rel32 at 53) */
/* The level's locations (objectives, points of interest, outposts, ...): two arrays of 2740 in the generation block G
 * (A: G+0x758E20, 0xE0 each; B: G+0x7EEBA0, 0x40 each), unchecked. Level 10 makes ~824; 2 x maps with Hell Forge's
 * objectives and outposts made 3900..5500 and overran them (GenerateResourceHash crashed at 0xC176B8). Larger maps move
 * both to the end of a larger block, DIF_LOCATIONS places each: the block's size (2 allocations, 4 clears), every
 * instruction addressing them (location_sites.h, from location_sites.py) and the builders' capacities. Blocks are made
 * with a world (0xAB8B70 -> 0xF6E710), after the loader installs this; one made before keeps the old layout, so the
 * move is skipped then. */
#define DIF_LOCATIONS 8192u
#define DIF_LOCATIONS_A 0x758E20u
#define DIF_LOCATIONS_B 0x7EEBA0u
#define DIF_LOCATIONS_A_NEW 0x11A7280u
#define DIF_LOCATIONS_B_NEW (DIF_LOCATIONS_A_NEW + DIF_LOCATIONS * 0xE0u)
/* The level's location types (C: G+0x756C20, 0x10 each: name hash and resource; 96 places, level 10 uses ~44, 2 x maps
 * 73..81, and more bases bring more) move with them, after B, with DIF_LOCATION_TYPES places. */
#define DIF_LOCATION_TYPES 512u
#define DIF_LOCATION_TYPES_C 0x756C20u
#define DIF_LOCATION_TYPES_C_NEW (DIF_LOCATIONS_B_NEW + DIF_LOCATIONS * 0x40u)
/* The minimap's arrays (unchecked, one proxy per location and prop): proxies (G+0x8198A0, 4096 x 0x30), scatter
 * (G+0x8498A0, 2048 x 0x58) and the proxies' units (G+0x118BEFC, 4096 x 4; G+0x118FEFC, 16384 x 4). A 2 x map's proxies
 * ran over the scatter into the terrain merge slots (G+0x8758A8: CleanupTerrainMerge crashed at 0xC17C21). They move
 * after C with 4 x their places (minimap_sites.py). */
#define DIF_MINIMAP_P 0x8198A0u
#define DIF_MINIMAP_S 0x8498A0u
#define DIF_MINIMAP_U 0x118BEFCu
#define DIF_MINIMAP_V 0x118FEFCu
#define DIF_MINIMAP_P_NEW (DIF_LOCATION_TYPES_C_NEW + DIF_LOCATION_TYPES * 0x10u)
#define DIF_MINIMAP_S_NEW (DIF_MINIMAP_P_NEW + 0x4000u * 0x30u)
#define DIF_MINIMAP_U_NEW (DIF_MINIMAP_S_NEW + 0x2000u * 0x58u)
#define DIF_MINIMAP_V_NEW (DIF_MINIMAP_U_NEW + 0x4000u * 4u)
/* The mission stamp list (G+0x8BBD70, 128 x 0x10: set, group, item, objective id), filled by GenerateMissionStampList
 * (0xF8E530) without a check (its pass records' capacity, 0x80, is never read): stamps are made per objective, and
 * level 11's 13 objectives made more than 128, the 129th over the objective list right after it (G+0x8BC570: +0 the
 * mission row; GenerateObjectiveLocations then crashed at 0xF8F670). It moves after V with DIF_STAMPS places; 21
 * instructions address it (lea [r12 + 0x8BBD70] in the op 0x15 pass records). */
#define DIF_STAMPS 1024u
#define DIF_STAMPS_OLD 0x8BBD70u
#define DIF_STAMPS_NEW (DIF_MINIMAP_V_NEW + 0x10000u * 4u)
/* The objective list S (G+0x8BC570: +0 row, +4 ids, +0x44 used flags, +0x54 stamp starts, +0x94 stamp counts, all 16,
 * +0xD4 count, +0xD8.. tag lists), filled from the mission's descriptor (up to 32 ids) without a check, moves after the
 * stamps with 30 places (research-objective-capacity, layout L30: counts S'-0xF0, starts S'-0x78, row +0, ids +4, used
 * +0x7C, count and tag lists where they were; every consumer's displacement is patched, disp8s included). */
#define DIF_OBJLIST_REGION (DIF_STAMPS_NEW + DIF_STAMPS * 0x10u)
#define DIF_OBJLIST_NEW (DIF_OBJLIST_REGION + 0xF0u)
/* The objective and extraction places (G+0x8ABCC0: 16 pointers, count G+0x11A7170; GenerateObjectiveLocations 0xF8F1D0
 * adds one per placement attempt, retries included, AddImportantLocations 0xF90DC0 adds extraction points) and the place
 * set (G+0x8C4A38: 64 x 0x10, count G+0x11A71AC) are filled without a check (their pass records' capacities are never
 * read). The 17th place overwrote the next field, the planet's generation settings (G+0x8ABD40: a level-11 preview with 15
 * objectives crashed in GenerateWaterStart spawning its water unit, #ID[0], research-crash-quarry). Both move after the
 * objective list: 512 places and 256 (29 and 25 instructions: the pass builders' leas and two readers). */
#define DIF_PLACES 512u
#define DIF_PLACES_NEW (DIF_OBJLIST_REGION + 0x280u)
#define DIF_PLACE_SET 256u
#define DIF_PLACE_SET_NEW (DIF_PLACES_NEW + DIF_PLACES * 8u)
/* The location clusters (GenerateLocationClusters, op 0x18: G+0x896FB0, 8 x 0xE0, 24 members each, count G+0x11A7154)
 * hold at most 192 important locations; a larger city map's ran past them (a preview crashed at 0xF78EA5, then at
 * 0xF7A460 through an unclustered location). They move after the place set with 32 places (16 pass builders' leas and
 * their capacities), and the pass makes at least one cluster per 24 locations of a kind (kClusterCountSites). */
#define DIF_CLUSTERS 32u
#define DIF_CLUSTERS_NEW (DIF_PLACE_SET_NEW + DIF_PLACE_SET * 0x10u)
#define DIF_BLOCK_NEW (DIF_CLUSTERS_NEW + DIF_CLUSTERS * 0xE0u)
static const LocationSite kPlaceListRefs[29] = {
    {0xC1AF77u, 0, 0x8ABCC0u}, {0xC1B60Fu, 0, 0x8ABCC0u}, {0xC1B887u, 0, 0x8ABCC0u}, {0xC1BB62u, 0, 0x8ABCC0u},
    {0xC1C201u, 0, 0x8ABCC0u}, {0xC1C49Eu, 0, 0x8ABCC0u}, {0xC1CA53u, 0, 0x8ABCC0u}, {0xC1CCF1u, 0, 0x8ABCC0u},
    {0xC1D469u, 0, 0x8ABCC0u}, {0xC27D3Eu, 0, 0x8ABCC0u}, {0xC28230u, 0, 0x8ABCC0u}, {0xC284ADu, 0, 0x8ABCC0u},
    {0xC28783u, 0, 0x8ABCC0u}, {0xC28B18u, 0, 0x8ABCC0u}, {0xC28DB6u, 0, 0x8ABCC0u}, {0xC2925Du, 0, 0x8ABCC0u},
    {0xC294EEu, 0, 0x8ABCC0u}, {0xC29958u, 0, 0x8ABCC0u}, {0xC2FD41u, 0, 0x8ABCC0u}, {0xC303D5u, 0, 0x8ABCC0u},
    {0xC3064Bu, 0, 0x8ABCC0u}, {0xC3091Du, 0, 0x8ABCC0u}, {0xC30FB8u, 0, 0x8ABCC0u}, {0xC31255u, 0, 0x8ABCC0u},
    {0xC31808u, 0, 0x8ABCC0u}, {0xC31A9Bu, 0, 0x8ABCC0u}, {0xC3220Fu, 0, 0x8ABCC0u}, {0xF737C5u, 0, 0x8ABCC0u},
    {0xF73EB4u, 0, 0x8ABCC0u}};
static const LocationSite kClusterRefs[16] = {
    {0xC1D865u, 4, 0x896FB0u}, {0xC1D9C5u, 4, 0x896FB0u}, {0xC1DC98u, 4, 0x896FB0u}, {0xC1E346u, 4, 0x896FB0u},
    {0xC1E639u, 4, 0x896FB0u}, {0xC1EA86u, 4, 0x896FB0u}, {0xC1EC77u, 4, 0x896FB0u}, {0xC1EE84u, 4, 0x896FB0u},
    {0xC32616u, 4, 0x896FB0u}, {0xC32775u, 4, 0x896FB0u}, {0xC32965u, 4, 0x896FB0u}, {0xC32CB2u, 4, 0x896FB0u},
    {0xC33358u, 4, 0x896FB0u}, {0xC33810u, 4, 0x896FB0u}, {0xC33A00u, 4, 0x896FB0u}, {0xC33C0Cu, 4, 0x896FB0u}};
static const LocationSite kClusterCaps[16] = {
    {0xC1D88Cu, 3, 0x8u}, {0xC1D9E3u, 6, 0x8u}, {0xC1DCACu, 3, 0x8u}, {0xC1E35Au, 3, 0x8u}, {0xC1E657u, 6, 0x8u},
    {0xC1EAA4u, 6, 0x8u}, {0xC1EC95u, 6, 0x8u}, {0xC1EE98u, 3, 0x8u}, {0xC3263Du, 3, 0x8u}, {0xC32793u, 6, 0x8u},
    {0xC32983u, 6, 0x8u}, {0xC32CC6u, 3, 0x8u}, {0xC3336Cu, 3, 0x8u}, {0xC3382Eu, 6, 0x8u}, {0xC33A1Eu, 6, 0x8u},
    {0xC33C20u, 3, 0x8u}};
/* GenerateLocationClusters' cluster count per kind (1..5): the settings' range (path A, 0xF7991F: edx = min(edx, the
 * kind's locations r10d)) or up to 4 (path B, 0xF79947: r10d = min(r10d, ecx)); either could leave more than 24 x the
 * clusters of locations. Both now take at least ceil(locations / 24), within the places left (r9d - r15d; path B's own
 * cap follows). {site, length, pin, stub, size, back rel32 at, back}. */
static const uint8_t kClusterCountPinA[9] = {0x44, 0x3B, 0xD2, 0x41, 0x0F, 0x42, 0xD2, 0xEB, 0x35};
static const uint8_t kClusterCountPinB[7] = {0x41, 0x3B, 0xCA, 0x44, 0x0F, 0x42, 0xD1};
static const uint8_t kClusterCountStubA[45] = {
    0x44, 0x3B, 0xD2, 0x41, 0x0F, 0x42, 0xD2,                         /*  0 cmp r10d, edx; cmovb edx, r10d (as before) */
    0x41, 0x8D, 0x42, 0x17,                                           /*  7 lea eax, [r10+23] */
    0xB9, 0xAB, 0xAA, 0xAA, 0xAA, 0x48, 0x0F, 0xAF, 0xC1,             /* 11 mov ecx, 0xAAAAAAAB; imul rax, rcx */
    0x48, 0xC1, 0xE8, 0x24,                                           /* 20 shr rax, 36 (eax = ceil(r10d / 24)) */
    0x44, 0x89, 0xC9, 0x44, 0x29, 0xF9,                               /* 24 mov ecx, r9d; sub ecx, r15d (places left) */
    0x39, 0xC8, 0x0F, 0x47, 0xC1,                                     /* 30 cmp eax, ecx; cmova eax, ecx */
    0x39, 0xD0, 0x0F, 0x47, 0xD0,                                     /* 35 cmp eax, edx; cmova edx, eax */
    0xE9, 0, 0, 0, 0};                                                /* 40 jmp 0xF7995D (rel32 at 41) */
static const uint8_t kClusterCountStubB[37] = {
    0x50,                                                             /*  0 push rax (r9d, the capacity) */
    0x41, 0x8D, 0x42, 0x17,                                           /*  1 lea eax, [r10+23] */
    0x41, 0xB8, 0xAB, 0xAA, 0xAA, 0xAA, 0x49, 0x0F, 0xAF, 0xC0,       /*  5 mov r8d, 0xAAAAAAAB; imul rax, r8 */
    0x48, 0xC1, 0xE8, 0x24,                                           /* 15 shr rax, 36 */
    0x39, 0xC1, 0x0F, 0x42, 0xC8,                                     /* 19 cmp ecx, eax; cmovb ecx, eax (the cap, at least that) */
    0x58,                                                             /* 24 pop rax */
    0x41, 0x3B, 0xCA, 0x44, 0x0F, 0x42, 0xD1,                         /* 25 cmp ecx, r10d; cmovb r10d, ecx (as before) */
    0xE9, 0, 0, 0, 0};                                                /* 32 jmp 0xF7994E (rel32 at 33) */
static const struct { uint32_t site; uint8_t length; const uint8_t* pin; const uint8_t* stub; uint8_t size, backAt; uint32_t back; } kClusterCountSites[2] = {
    {0xF7991Fu, 9, kClusterCountPinA, kClusterCountStubA, 45, 41, 0xF7995Du},
    {0xF79947u, 7, kClusterCountPinB, kClusterCountStubB, 37, 33, 0xF7994Eu}};
static const LocationSite kPlaceSetRefs[25] = {
    {0xC1AF98u, 0, 0x8C4A38u}, {0xC1B8B1u, 0, 0x8C4A38u}, {0xC1BB83u, 0, 0x8C4A38u}, {0xC1C4BFu, 0, 0x8C4A38u},
    {0xC1CA89u, 0, 0x8C4A38u}, {0xC1CD12u, 0, 0x8C4A38u}, {0xC1D48Au, 0, 0x8C4A38u}, {0xC25D0Bu, 0, 0x8C4A38u},
    {0xC27D5Fu, 0, 0x8C4A38u}, {0xC284D7u, 0, 0x8C4A38u}, {0xC287A4u, 0, 0x8C4A38u}, {0xC28DD7u, 0, 0x8C4A38u},
    {0xC29285u, 0, 0x8C4A38u}, {0xC2950Fu, 0, 0x8C4A38u}, {0xC29979u, 0, 0x8C4A38u}, {0xC2DD25u, 0, 0x8C4A38u},
    {0xC2FD62u, 0, 0x8C4A38u}, {0xC30675u, 0, 0x8C4A38u}, {0xC3093Eu, 0, 0x8C4A38u}, {0xC31276u, 0, 0x8C4A38u},
    {0xC31833u, 0, 0x8C4A38u}, {0xC31ABCu, 0, 0x8C4A38u}, {0xC32230u, 0, 0x8C4A38u}, {0xC39DA5u, 0, 0x8C4A38u},
    {0xC3BE6Au, 0, 0x8C4A38u}};

#define DIF_OBJECTIVES_MAX 30u        /* the moved objective list */
#define DIF_OBJECTIVES_OTHER 12u      /* a mission's own objectives beside optional and major ones (up to 5 mandatory,
                                       * the live event's, a partner mission's up to 6) */
static const struct { uint32_t rva; uint8_t length, at, width; uint32_t old, value; uint8_t pin[8]; } kObjectiveListSites[74] = {
    {0xC1AD93u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC1B57Du, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC1B67Bu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC1B97Eu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC1C16Fu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC1C2BAu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC1C847u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC1CB0Du, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC1D285u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC1E327u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC1E863u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC1EE65u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC1F084u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC20993u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC20AB8u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC25D72u, 7, 3, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x85, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC25F73u, 7, 3, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x85, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC27B31u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC2819Eu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC2829Cu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC28576u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC28A86u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC28BA9u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC2904Cu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC292E1u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC2974Bu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC29F76u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC2AC64u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC2DFACu, 7, 3, 4, 0x8BC570u, 0x15AD370u, {0x48, 0x8D, 0x85, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC2FB35u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC30343u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC30441u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC30711u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC30F26u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC31049u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC315FEu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC3188Fu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC32003u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x4D, 0x8D, 0xBC, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC33339u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC335EEu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC33BEDu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC33E08u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC354EDu, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC35603u, 8, 4, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x84, 0x24, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC39E18u, 7, 3, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0x85, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0xC3BEDEu, 7, 3, 4, 0x8BC570u, 0x15AD370u, {0x48, 0x8D, 0x83, 0x70, 0xC5, 0x8B, 0x00}}, /* builder */
    {0x177DECDu, 7, 3, 4, 0x8BC570u, 0x15AD370u, {0x49, 0x8D, 0xBA, 0x70, 0xC5, 0x8B, 0x00}}, /* direct */
    {0xAD4CEFu, 8, 3, 4, 0x8BC660u, 0x15AD460u, {0x41, 0x83, 0xB8, 0x60, 0xC6, 0x8B, 0x00, 0x00}}, /* direct */
    {0xAD4D0Au, 8, 4, 4, 0x8BC654u, 0x15AD454u, {0x41, 0x8B, 0x84, 0x80, 0x54, 0xC6, 0x8B, 0x00}}, /* direct */
    {0xAD4D22u, 7, 3, 4, 0x8BC660u, 0x15AD460u, {0x41, 0x3B, 0x90, 0x60, 0xC6, 0x8B, 0x00}}, /* direct */
    {0xF70F41u, 7, 3, 4, 0x8BC570u, 0x15AD370u, {0x44, 0x89, 0x80, 0x70, 0xC5, 0x8B, 0x00}}, /* filler */
    {0xF70F57u, 7, 3, 4, 0x8BC644u, 0x15AD444u, {0x44, 0x89, 0x80, 0x44, 0xC6, 0x8B, 0x00}}, /* filler */
    {0xF70F63u, 5, 1, 4, 0x8BC574u, 0x15AD374u, {0xBA, 0x74, 0xC5, 0x8B, 0x00}}, /* filler */
    {0xF70F70u, 8, 4, 4, 0xFF743A8Cu, 0xFEA52C8Cu, {0x41, 0x8B, 0x84, 0x11, 0x8C, 0x3A, 0x74, 0xFF}}, /* filler */
    {0xF70FC3u, 7, 3, 4, 0x8BC647u, 0x15AD447u, {0x88, 0x84, 0x0A, 0x47, 0xC6, 0x8B, 0x00}}, /* filler */
    {0xF70FD7u, 7, 3, 4, 0x8BC650u, 0x15AD450u, {0x44, 0x89, 0x90, 0x50, 0xC6, 0x8B, 0x00}}, /* filler */
    {0xF70FF4u, 6, 2, 4, 0x8BC654u, 0x15AD454u, {0x41, 0xB8, 0x54, 0xC6, 0x8B, 0x00}}, /* filler */
    {0xF71024u, 7, 3, 4, 0x8BC660u, 0x15AD460u, {0x44, 0x89, 0x90, 0x60, 0xC6, 0x8B, 0x00}}, /* filler */
    {0xF71063u, 7, 3, 4, 0x8BC663u, 0x15AD463u, {0x88, 0x84, 0x0A, 0x63, 0xC6, 0x8B, 0x00}}, /* filler */
    {0xF7107Fu, 7, 3, 4, 0x8BC668u, 0x15AD468u, {0x44, 0x89, 0x90, 0x68, 0xC6, 0x8B, 0x00}}, /* filler */
    {0xF7108Du, 6, 2, 4, 0x8BC66Cu, 0x15AD46Cu, {0x89, 0x88, 0x6C, 0xC6, 0x8B, 0x00}}, /* filler */
    {0xF8E64Eu, 5, 4, 1, 0x54u, 0x88u, {0x47, 0x89, 0x64, 0x91, 0x54}}, /* stamps */
    {0xF8E7F5u, 5, 4, 1, 0x54u, 0x88u, {0x41, 0x8B, 0x4C, 0xB9, 0x54}}, /* stamps */
    {0xF8E806u, 8, 4, 4, 0x94u, 0xFFFFFF10u, {0x41, 0x89, 0x84, 0xB9, 0x94, 0x00, 0x00, 0x00}}, /* stamps */
    {0xF8F626u, 7, 3, 4, 0xFFFFFF6Cu, 0xF0u, {0x49, 0xC7, 0xC5, 0x6C, 0xFF, 0xFF, 0xFF}}, /* place */
    {0xF8F63Bu, 8, 4, 4, 0x94u, 0xFFFFFF10u, {0x4D, 0x8D, 0xBC, 0x24, 0x94, 0x00, 0x00, 0x00}}, /* place */
    {0xF8F68Du, 4, 3, 1, 0xC0u, 0x78u, {0x45, 0x8B, 0x67, 0xC0}}, /* place */
    {0xF8F696u, 8, 4, 4, 0xFFFFFF6Cu, 0xF0u, {0x43, 0x8B, 0x94, 0x3A, 0x6C, 0xFF, 0xFF, 0xFF}}, /* place */
    {0xF8FB60u, 7, 3, 4, 0x90u, 0xFFFFFF0Cu, {0x44, 0x8B, 0xA0, 0x90, 0x00, 0x00, 0x00}}, /* place */
    {0xF8FB67u, 3, 2, 1, 0x50u, 0x84u, {0x8B, 0x58, 0x50}}, /* place */
    {0xF46CC9u, 4, 3, 1, 0x44u, 0x7Cu, {0x48, 0x8D, 0x4E, 0x44}}, /* layers */
    {0xF46E13u, 5, 3, 1, 0x44u, 0x7Cu, {0xC6, 0x44, 0x30, 0x44, 0x01}}, /* layers */
    {0xF46EAFu, 5, 4, 1, 0x44u, 0x7Cu, {0x44, 0x38, 0x7C, 0x31, 0x44}}, /* layers */
    {0xF46EE3u, 5, 3, 1, 0x44u, 0x7Cu, {0xC6, 0x44, 0x31, 0x44, 0x01}}, /* layers */};
/* The mission-end telemetry (0x1347A10: [0x347CE18]+0x31820) keeps 16 objective instances (count +0x319A0) and its reader
 * (0x13462C0) two 16-place stack arrays: the count and the loop stop at 16. */
#define DIF_TELEMETRY_COUNT_SITE 0x1347D77u
#define DIF_TELEMETRY_LOOP_SITE 0x1347E4Du
#define DIF_TELEMETRY_LOOP 0x1347D91u
#define DIF_TELEMETRY_DONE 0x1347E58u
static const uint8_t kTelemetryCountPin[10] = {0x8B, 0x43, 0x24, 0x41, 0x89, 0x86, 0xA0, 0x19, 0x03, 0x00};
static const uint8_t kTelemetryLoopPin[11] = {0xFF, 0xC7, 0x3B, 0x7B, 0x24, 0x0F, 0x82, 0x39, 0xFF, 0xFF, 0xFF};
static const uint8_t kTelemetryCountStub[25] = {
    0x8B, 0x43, 0x24,                                                 /*  0 mov eax, [rbx+0x24] (objective instances) */
    0x83, 0xF8, 0x10, 0x76, 0x05, 0xB8, 0x10, 0x00, 0x00, 0x00,       /*  3 eax = min(eax, 16) */
    0x41, 0x89, 0x86, 0xA0, 0x19, 0x03, 0x00,                         /* 13 mov [r14+0x319a0], eax */
    0xE9, 0, 0, 0, 0};                                                /* 20 jmp 0x1347D81 (rel32 at 21) */
static const uint8_t kTelemetryLoopStub[25] = {
    0xFF, 0xC7,                                                       /*  0 inc edi */
    0x83, 0xFF, 0x10, 0x0F, 0x83, 0, 0, 0, 0,                         /*  2 cmp edi, 16; jae 0x1347E58 (rel32 at 7) */
    0x3B, 0x7B, 0x24, 0x0F, 0x82, 0, 0, 0, 0,                         /* 11 cmp edi, [rbx+0x24]; jb 0x1347D91 (rel32 at 16) */
    0xE9, 0, 0, 0, 0};                                                /* 20 jmp 0x1347E58 (rel32 at 21) */
/* The in-mission HUD's updater 0x18B42F0 copies every objective instance's id (objective manager [game+0x3326DA0], +0x24
 * instances) into a 16-entry stack list (rbp+0xC0, its /GS cookie at rbp+0x100) and reads it back to number the mission's
 * objectives, unchecked: a mixed mission's 17th instance ended the game. Its two writes (0x18B4EA0, 0x18B4EB4) and its read
 * (0x18B4F00) use a 1024-entry list of the loader instead (r11 is free at each; flags are untouched). */
#define DIF_HUD_LIST 1024u
static const struct { uint32_t site; uint8_t length; uint8_t pin[8]; uint8_t access[4]; } kHudListSites[3] = {
    {0x18B4EA0u, 7, {0x89, 0x8C, 0x95, 0xC0, 0x00, 0x00, 0x00}, {0x41, 0x89, 0x0C, 0x93}},        /* mov [r11+rdx*4], ecx */
    {0x18B4EB4u, 7, {0x89, 0x84, 0x95, 0xC0, 0x00, 0x00, 0x00}, {0x41, 0x89, 0x04, 0x93}},        /* mov [r11+rdx*4], eax */
    {0x18B4F00u, 8, {0x42, 0x8B, 0x9C, 0xAD, 0xC0, 0x00, 0x00, 0x00}, {0x43, 0x8B, 0x1C, 0xAB}}}; /* mov ebx, [r11+r13*4] */
/* The same updater, run every frame while the map is open (0x18A7C60), gives each map marker one of the map object's 16
 * marker slots (+0xF958, 0x2B8 each, built by 0x18A5580; the tracker list follows at +0x12518), counted in [rsp+0x40]:
 * objective instances (checked at 16), then objective-linked entities (loop 0x18B51B0) and the route marker (0x18B5FF4),
 * both unchecked. With 16 or more instances a route mission's marker took a 17th slot over the trackers (float 1.0 as a
 * UI element's child) and the map ended the game (research-minimap-crash). Both leave once the slots are full:
 * the loop to its exit, the route marker to the path without one. */
#define DIF_MARKER_SLOTS 16u
#define DIF_MARKER_LOOP_SITE 0x18B51B0u
#define DIF_MARKER_LOOP_EXIT 0x18B5A17u
#define DIF_MARKER_ROUTE_SITE 0x18B5FF4u
#define DIF_MARKER_ROUTE_SKIP 0x18B5E22u
static const uint8_t kMarkerLoopPin[8] = {0x48, 0x8B, 0x40, 0x38, 0x4C, 0x8B, 0x1C, 0xC8};  /* mov rax,[rax+0x38]; mov r11,[rax+rcx*8] */
static const uint8_t kMarkerRoutePin[6] = {0x45, 0x8B, 0xE7, 0x41, 0x8B, 0xDF};              /* mov r12d,r15d; mov ebx,r15d */
/* The mission result (0x62EC20: a record on 0x1380840's stack) lists badges (+0x14C8: {id, amount} x 32, count +0x15C8):
 * up to 11 before its objective loop, one per objective instance with a badge (def +0x54), up to 5 after, unchecked (the
 * 33rd overwrites the count). In the loop it appends only while fewer than 27 are listed (HellForge's backend ignores
 * badges). */
#define DIF_BADGE_SITE 0x62FAB3u      /* movsxd rax, [rdi+0x15c8] */
#define DIF_BADGE_NEXT 0x62FABAu
#define DIF_BADGE_SKIP 0x62FB4Cu      /* the objective loop's next instance */
static const uint8_t kBadgePin[7] = {0x48, 0x63, 0x87, 0xC8, 0x15, 0x00, 0x00};
static const uint8_t kBadgeStub[21] = {
    0x48, 0x63, 0x87, 0xC8, 0x15, 0x00, 0x00,                         /*  0 movsxd rax, [rdi+0x15c8] */
    0x83, 0xF8, 0x1B,                                                 /*  7 cmp eax, 27 */
    0x0F, 0x8D, 0, 0, 0, 0,                                           /* 10 jge 0x62FB4C (rel32 at 12) */
    0xE9, 0, 0, 0, 0};                                                /* 16 jmp 0x62FABA (rel32 at 17) */
/* The navigation world (research-beacons): the engine sets it up once at startup (exe 0x7B45C0) as 8 x 8 tiles of 128 m,
 * +-512 m around the centre, and every mission reuses it. Hellpods and mission beacons stick only where the navmesh is
 * (0x6A0D6D), and enemies walk only there, so a larger map's far side (up to 778 m out) had none: blue stratagems would
 * not stick. The engine's block for it is sized for 8 x 8 tiles, so the grid keeps them and their size changes: NavRetile
 * writes 256 m tiles (the whole 2048 m world) before a larger map's navmesh grid is set up (setup_navmeshgrid, pass 0x68,
 * handler 0xC17DFC: the nav world id in ecx, the pass context in r15, its level at +0x18) and the vanilla 128 m before
 * any other level's and before the ship's (0xADA075). Fields of the nav world W = [[exe+0x27C30B8] + 8 id] (id < [exe+
 * 0x27C30C0]): +0x08/+0x0C min x, y, +0x14/+0x18 max x, y, +0x6710/+0x6712 the grid (u16, 8 x 8), +0x6718 the tile,
 * +0x671C..+0x6728 the grid's min and max; the engine reads them afresh. */
#define DIF_NAV_SITE 0xC17E2Eu         /* mov rax, [0x33263C8]; call [rax+8]; jmp 0xC19E1C (15 bytes) */
#define DIF_NAV_BACK 0xC19E1Cu
#define DIF_NAV_SHIP_SITE 0xADA075u    /* mov ecx, [r14+0x10f0]; call [rax+8] (10 bytes) */
#define DIF_NAV_SHIP_BACK 0xADA07Fu
#define DIF_NAV_SYSTEM 0x33263C8u
#define DIF_NAV_WORLDS 0x27C30B8u      /* exe: the address of the nav world pointers */
#define DIF_NAV_COUNT 0x27C30C0u       /* exe: their count */
#define DIF_NAV_PIN 0x7B73F0u          /* exe: a reader of both, checked */
static const uint8_t kNavPin[15] = {0x48, 0x8B, 0x05, 0x93, 0xE5, 0x70, 0x02, 0xFF, 0x50, 0x08, 0xE9, 0xDF, 0x1F, 0x00, 0x00};
static const uint8_t kNavShipPin[10] = {0x41, 0x8B, 0x8E, 0xF0, 0x10, 0x00, 0x00, 0xFF, 0x50, 0x08};
static const uint8_t kNavExePin[23] = {0x48, 0x83, 0xEC, 0x28, 0x39, 0x0D, 0xC6, 0xBC, 0x00, 0x02, 0x0F, 0x86, 0xE7, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x05, 0xB1, 0xBC, 0x00, 0x02};
static const uint8_t kNavStub[145] = {
    0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, /*   0 push rax rcx rdx r8 r9 r10 r11 */
    0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,                         /*  11 sub rsp, 0x88 (aligned: 7 pushes + 0x88) */
    0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30, /*  18 movdqu [rsp+0x20..0x70], xmm0..xmm5 */
    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,
    0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,
    0x49, 0x8B, 0x57, 0x18,                                           /*  54 mov rdx, [r15+0x18] (the level; ecx = the id) */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  58 mov rax, NavRetile (imm64 at 60) */
    0xFF, 0xD0,                                                       /*  68 call rax */
    0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30, /* movdqu xmm0..xmm5, [rsp+0x20..0x70] */
    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
    0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,
    0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,                         /* add rsp, 0x88 */
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, /* pop r11 r10 r9 r8 rdx rcx rax */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /* 124 mov rax, game+0x33263C8 (imm64 at 126) */
    0x48, 0x8B, 0x00,                                                 /* 134 mov rax, [rax] */
    0xFF, 0x50, 0x08,                                                 /* 137 call [rax+8] */
    0xE9, 0, 0, 0, 0};                                                /* 140 jmp 0xC19E1C (rel32 at 141) */
static const uint8_t kNavShipStub[144] = {
    0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, /*   0 push rax rcx rdx r8 r9 r10 r11 */
    0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,                         /*  11 sub rsp, 0x88 (aligned: 7 pushes + 0x88) */
    0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30, /*  18 movdqu [rsp+0x20..0x70], xmm0..xmm5 */
    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,
    0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,
    0x41, 0x8B, 0x8E, 0xF0, 0x10, 0x00, 0x00,                         /*  54 mov ecx, [r14+0x10f0] (the id) */
    0x31, 0xD2,                                                       /*  61 xor edx, edx (no level: vanilla) */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  63 mov rax, NavRetile (imm64 at 65) */
    0xFF, 0xD0,                                                       /*  73 call rax */
    0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30, /* movdqu xmm0..xmm5, [rsp+0x20..0x70] */
    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
    0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,
    0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,                         /* add rsp, 0x88 */
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, /* pop r11 r10 r9 r8 rdx rcx rax */
    0x41, 0x8B, 0x8E, 0xF0, 0x10, 0x00, 0x00,                         /* 129 mov ecx, [r14+0x10f0] */
    0xFF, 0x50, 0x08,                                                 /* 136 call [rax+8] */
    0xE9, 0, 0, 0, 0};                                                /* 139 jmp 0xADA07F (rel32 at 140) */
/* The tiles' terrain build (exe 0x89A6C0, Recast: research-navtiles) sizes its heightfield from a hard-coded 128 m
 * (0x89A85E: movss xmm0, [rip+128.0], cells = 128 / 0.5) while it rasterises the tile's real bounds: a 256 m tile wrote
 * past its rows (a level-11 load crashed in addSpan, exe+0x8BCE66). Its displacement is pointed at the exe's own 256.0
 * (0x16AA7C0) for 256 m tiles and back at 128.0 (0x16A9DB0, also read elsewhere: not written) for 128 m ones. */
#define DIF_NAV_TILE_SIZE_READ 0x89A85Eu
static const uint8_t kNavTileSizeRead[8] = {0xF3, 0x0F, 0x10, 0x05, 0x4A, 0xF5, 0xE0, 0x00};
static const uint8_t kNavTileSize256[4] = {0x5A, 0xFF, 0xE0, 0x00};
static int g_navTiles256; /* the exe's terrain build and its 256.0 are as reviewed */
static uintptr_t g_navExe;
static volatile LONG g_navLarger, g_navVanilla;
/* Before a navmesh grid is set up: 256 m tiles for a larger map (its generated disk, lg+0x2D8C, past vanilla's 681 m),
 * else 128 m. Only an 8 x 8 grid is touched. */
static void NavRetile(uint32_t id, uintptr_t level)
{
    uintptr_t exe = g_navExe, worlds = 0, world = 0;
    uint32_t count = 0;
    uint16_t grid[2] = {0, 0};
    float disk = 0.0f;
    if (exe == 0 || SafeRead(exe + DIF_NAV_COUNT, &count, 4) || id >= count || SafeRead(exe + DIF_NAV_WORLDS, &worlds, 8) || worlds == 0 ||
        SafeRead(worlds + 8 * (uintptr_t)id, &world, 8) ||
        world == 0 || SafeRead(world + 0x6710, grid, 4) || grid[0] != 8 || grid[1] != 8) return;
    int larger = g_navTiles256 && level != 0 && !SafeRead(level + 0x2D8C, &disk, 4) && disk > 682.0f;
    if (g_navTiles256 && ApiWrite(exe + DIF_NAV_TILE_SIZE_READ + 4, larger ? kNavTileSize256 : kNavTileSizeRead + 4, 4)) return;
    float lo = larger ? -1023.75f : -511.75f, hi = larger ? 1024.25f : 512.25f;
    float min[2] = {lo, lo}, max[2] = {hi, hi}, tiles[5] = {larger ? 256.0f : 128.0f, lo, lo, hi, hi};
    if (ApiWrite(world + 0x08, min, sizeof min) || ApiWrite(world + 0x14, max, sizeof max) || ApiWrite(world + 0x6718, tiles, sizeof tiles)) return;
    InterlockedIncrement(larger ? &g_navLarger : &g_navVanilla);
}
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
static void StrongholdInstall(uintptr_t level)
{
    uint64_t holder = 0, sets = 0;
    uint32_t count = 0;
    if (level == 0 || SafeRead(level + 0x2D0, &holder, 8) || holder == 0 || SafeRead(holder + 0x38, &sets, 8) || sets == 0 ||
        SafeRead(holder + 0xBC, &count, 4) || count == 0 || count > 64) return;
    for (uint32_t s = 0; s < count; s++)
    {
        uint64_t set = 0, groups = 0;
        uint32_t id = 0, groupCount = 0;
        if (SafeRead(sets + 8 * (uint64_t)s, &set, 8) || set == 0 || SafeRead(set, &id, 4) || (id != DIF_STRONGHOLD_SET && id != DIF_OBJECTIVE_SET) ||
            SafeRead(set + 0x28, &groups, 8) || SafeRead(set + 0x30, &groupCount, 4) || groups == 0 || groupCount > 64) continue;
        for (uint32_t g = 0; g < groupCount; g++)
        {
            uint64_t group = groups + 0x18 * (uint64_t)g, items = 0;
            uint32_t gid = 0, itemCount = 0;
            int which = -1;
            if (SafeRead(group, &gid, 4) || SafeRead(group + 8, &items, 8) || SafeRead(group + 0x10, &itemCount, 4)) continue;
            for (int k = 0; k < 3; k++)
                if (gid == kCityCopyGroups[k].group && id == kCityCopyGroups[k].set && (g_cityCopyFlags & kCityCopyGroups[k].flag)) which = k;
            if (which < 0 || items == 0 || itemCount == 0 || itemCount > 256 || items == g_strongholdItems[which]) continue;
            uint8_t* copy = VirtualAlloc(NULL, (size_t)itemCount * 2 * 0x1C8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (copy == NULL || SafeRead(items, copy, (size_t)itemCount * 0x1C8)) { if (copy) VirtualFree(copy, 0, MEM_RELEASE); continue; }
            uint32_t added = 0;
            for (uint32_t i = 0; i < itemCount; i++)
            {
                uint8_t* item = copy + (size_t)i * 0x1C8;
                uint16_t word;
                if (!CityCopyWanted(which, item)) continue;
                uint8_t* clone = copy + (size_t)(itemCount + added) * 0x1C8;
                memcpy(clone, item, 0x1C8);
                uint64_t classes = (uint64_t)(uintptr_t)&g_strongholdClass;
                uint32_t one = 1;
                memcpy(clone + 0x198, &classes, 8);
                memcpy(clone + 0x1A0, &one, 4);
                memcpy(&word, clone + 0x60, 2);
                word = (uint16_t)((word & ~0x3Cu) | ((DIF_VANILLA + 1) << 2)); /* minimum: the added level */
                memcpy(clone + 0x60, &word, 2);
                added++;
            }
            if (added == 0) { VirtualFree(copy, 0, MEM_RELEASE); continue; }
            uint64_t array = (uint64_t)(uintptr_t)copy;
            uint32_t total = itemCount + added;
            if (ApiWrite(group + 8, &array, 8) || ApiWrite(group + 0x10, &total, 4)) continue; /* the array first: a reader sees old or new, both valid */
            g_strongholdItems[which] = (uintptr_t)copy;
            if (InterlockedIncrement(&g_strongholdLogged) <= 8)
                Note("difficulty: %s offered on city maps at level 11 (%u of %u items copied, group %08x at %#llx)",
                    kCityCopyGroups[which].name, added, itemCount, gid, (unsigned long long)group);
        }
    }
}
static const uint8_t kStrongholdStub[126] = {
    0x9C,                                                             /*   0 pushfq */
    0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, /*   1 push rax rcx rdx r8 r9 r10 r11 */
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,                         /*  12 sub rsp, 0x80 (8 pushes + 0x80: aligned) */
    0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30, /*  19 movdqu [rsp+0x20..0x70], xmm0..xmm5 */
    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,
    0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,
    0x4C, 0x89, 0xE9,                                                 /*  55 mov rcx, r13 (the level) */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  58 mov rax, StrongholdInstall (imm64 at 60) */
    0xFF, 0xD0,                                                       /*  68 call rax */
    0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30, /*  70 movdqu xmm0..xmm5, [rsp+0x20..0x70] */
    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
    0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,
    0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00,                         /* 106 add rsp, 0x80 */
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, /* 113 pop r11 r10 r9 r8 rdx rcx rax */
    0x9D,                                                             /* 124 popfq */
    0xC3};                                                            /* 125 (replaced by jmp back: the stub is copied with 4 more bytes) */
/* The physics pools: game.dll's plugin setup (0x4EE160, once per process,
 * about 5 s after the loader starts) fills the engine's physics config with fixed sizes, and the exe (0x795630) builds
 * every world's pools from it in one block. Every enemy has a ragdoll (corpses keep theirs) of 25..34 actors, and the
 * ragdoll actor store (8192 per world) filled at about 230 enemies: its allocator then returns -1, which the ragdoll
 * builder stores as a handle unchecked (exe 0x7A307F) and the game ends. Pool index widths follow their sizes (the static
 * pool already has 131072), so the sizes are raised before the setup runs: ragdoll actors 8192 -> 32768, ragdolls
 * 512 -> 1024, and the Havok world's motions 36864 -> 73728 (about one per non-static actor). A larger mega city
 * used up the world's 131072 bodies while spawning: the allocator's "none"
 * (0xFFFFFF, 0xD4588C) became a building's body and its navmesh cutter read it (exe 0xCB304C). Bodies and static actors
 * get 262144 (body ids are 24-bit; 0x814F00 keeps flags in bits 21..23, so below 2^21), navmesh cutters 65536. The
 * Game World's render objects ("max render objects", 0xAB189D in the same
 * world creation) size its culling systems (N/8 for the main visibility set, 98304, which then ran out at exe
 * 0x58E14B): 0xC0000 -> 0x200000 (handles keep 24 bits, 2 generation bits left; ~+120 MB). Dynamic actors 32768 ->
 * 131072, keyframed 8192 -> 32768 and motions 73728 -> 131072 follow the same 4 x units.
 * [game+0x3326380] is set by the setup's first call (0x4EDC62): while it is 0, the setup has not started. */
static const struct { uint32_t rva; uint8_t length, at; uint8_t pin[10]; uint32_t value; } kPhysicsPools[9] = {
    {0x4EE32Bu, 7, 3, {0xC7, 0x45, 0x9C, 0x00, 0x20, 0x00, 0x00}, 0x8000u},                        /* config +0x2C ragdoll actors */
    {0x4EE35Cu, 7, 3, {0xC7, 0x45, 0xAC, 0x00, 0x02, 0x00, 0x00}, 0x400u},                         /* config +0x3C ragdolls */
    {0xAB19F8u, 10, 6, {0xC7, 0x85, 0x20, 0x05, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00}, 0x20000u},   /* Havok world motions */
    {0xAB19EEu, 10, 6, {0xC7, 0x85, 0x1C, 0x05, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00}, 0x40000u},   /* Havok world bodies */
    {0x4EE324u, 7, 3, {0xC7, 0x45, 0x98, 0x00, 0x00, 0x02, 0x00}, 0x40000u},                       /* config +0x28 static actors */
    {0x4EE371u, 7, 3, {0xC7, 0x45, 0xBC, 0x00, 0x80, 0x00, 0x00}, 0x10000u},                       /* config +0x4C navmesh cutters */
    {0x4EE31Du, 7, 3, {0xC7, 0x45, 0x94, 0x00, 0x80, 0x00, 0x00}, 0x20000u},                       /* config +0x24 dynamic actors */
    {0x4EE332u, 7, 3, {0xC7, 0x45, 0x90, 0x00, 0x20, 0x00, 0x00}, 0x8000u},                        /* config +0x20 keyframed actors */
    {0xAB189Du, 8, 4, {0xC7, 0x44, 0x24, 0x58, 0x00, 0x00, 0x0C, 0x00}, 0x200000u}};             /* Game World render objects */
#define DIF_PHYSICS_SETUP 0x3326380u
static int g_physicsPools; /* 0 waiting, 1 enlarged, -1 left as they are */
/* Polled from the loader's thread once game.dll is decoded. */
static void PhysicsPools(uintptr_t game)
{
    uint64_t setup = 1;
    if (g_physicsPools != 0 || game == 0 || SafeRead(game + DIF_PHYSICS_SETUP, &setup, 8)) return;
    if (setup != 0)
    {
        g_physicsPools = -1;
        Note("physics: the engine's pools were already built: ragdolls keep 8192 actors (about 230 enemies at once)");
        return;
    }
    for (int i = 0; i < 9; i++)
    {
        uint8_t bytes[10];
        if (SafeRead(game + kPhysicsPools[i].rva, bytes, kPhysicsPools[i].length) || memcmp(bytes, kPhysicsPools[i].pin, kPhysicsPools[i].length) != 0)
        {
            g_physicsPools = -1;
            Note("physics: the physics config is not the reviewed one: its pools keep their sizes");
            return;
        }
    }
    for (int i = 0; i < 9; i++)
        if (ApiWrite(game + kPhysicsPools[i].rva + kPhysicsPools[i].at, &kPhysicsPools[i].value, 4))
        {
            g_physicsPools = -1;
            Note("physics: a physics pool could not be enlarged");
            return;
        }
    g_physicsPools = 1;
    Note("physics: pools enlarged before the engine built them: ragdoll actors 32768, ragdolls 1024, motions 131072, bodies and static actors 262144, dynamic actors 131072, keyframed 32768, navmesh cutters 65536 per world; render objects 2097152");
}
/* SpreadLocations (op 0x8B, 0xF7C310) picks the nearest location of a kind as a reference (r12d, -1 when none
 * qualifies) and sorts its candidates by their distance from it (0xF7A430) without checking: a level-11 city map (normal
 * size, Hell Forge's extra outposts and objectives) found none, and the sort read location -1 (a crash on the preview).
 * When none was found, location 0 is the reference; every other case is unchanged. */
#define DIF_SPREAD_REFERENCE 0xF7CEC8u   /* mov eax, r12d; imul r12, rax, 0xe0 */
#define DIF_SPREAD_REFERENCE_BACK 0xF7CED2u
static const uint8_t kSpreadReferencePin[10] = {0x41, 0x8B, 0xC4, 0x4C, 0x69, 0xE0, 0xE0, 0x00, 0x00, 0x00};
static const uint8_t kSpreadReferenceStub[24] = {
    0x41, 0x83, 0xFC, 0xFF, 0x75, 0x03,                               /*  0 cmp r12d, -1; jne 9 */
    0x45, 0x31, 0xE4,                                                 /*  6 xor r12d, r12d (none: location 0) */
    0x41, 0x8B, 0xC4, 0x4C, 0x69, 0xE0, 0xE0, 0x00, 0x00, 0x00,       /*  9 mov eax, r12d; imul r12, rax, 0xe0 */
    0xE9, 0, 0, 0, 0};                                                /* 19 jmp 0xF7CED2 (rel32 at 20) */
/* The war table's mission board: 110 slots of 0x5C per planet: 0..29 the
 * planet's missions (3 per level 1..10), 30..109 regions 0..7 (10 levels each). City and factory maps only come from
 * regions (Super Earth's mega cities), whose generator (0x11E44D0) stopped at 10 (0x11E46F1, mov ebp, 10), and the
 * planet generators (campaign 0x11E3C10, defence 0x11E4060) put level 11 at slots 30..32, region 0's levels 1..3. Level
 * 11 now uses region 7's block: the planet's at 100 + n, region r's at 103 + r; a planet with a region 7 (war state
 * +0x23018: {planet, region, settings} x count +0x26018) gets no level 11. Levels 1..10 keep every slot. */
#define DIF_BOARD_REGION_MAX 0x11E46F1u
#define DIF_BOARD_REGION_SITE 0x11E4744u
#define DIF_BOARD_PLANET_SITE 0x11E3E67u
#define DIF_BOARD_DEFENCE_SITE 0x11E4287u
static const uint8_t kBoardRegionMaxPin[5] = {0xBD, 0x0A, 0x00, 0x00, 0x00};
static const uint8_t kBoardRegionPin[6] = {0x8B, 0xC2, 0x48, 0x6B, 0xF8, 0x5C};
static const uint8_t kBoardPlanetPin[7] = {0x41, 0x8B, 0xC3, 0x48, 0x6B, 0xF8, 0x5C};
/* {site, length, stub, size, back rel32 at, back, skip rel32 at, skip} */
static const uint8_t kBoardRegionStub[80] = {
    0x83, 0xFB, 0x0B, 0x72, 0x36, 0x83, 0xFE, 0x06, 0x77, 0x3C, 0x48, 0x8B, 0x44, 0x24, 0x38, 0x8B,
    0x54, 0x24, 0x40, 0x44, 0x8B, 0x80, 0x18, 0x60, 0x02, 0x00, 0x48, 0x05, 0x18, 0x30, 0x02, 0x00,
    0x45, 0x85, 0xC0, 0x74, 0x13, 0x39, 0x10, 0x75, 0x06, 0x83, 0x78, 0x04, 0x07, 0x73, 0x17, 0x48,
    0x83, 0xC0, 0x18, 0x41, 0xFF, 0xC8, 0xEB, 0xE8, 0x8D, 0x56, 0x67, 0x8B, 0xC2, 0x48, 0x6B, 0xF8,
    0x5C, 0xE9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x44, 0x24, 0x20, 0xE9, 0x00, 0x00, 0x00, 0x00};
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
    {DIF_BOARD_REGION_SITE, 6, kBoardRegionStub, sizeof kBoardRegionStub, 66, 0x11E474Au, 76, 0x11E4821u},
    {DIF_BOARD_PLANET_SITE, 7, kBoardPlanetStub, sizeof kBoardPlanetStub, 61, 0x11E3E6Eu, 66, 0x11E4002u},
    {DIF_BOARD_DEFENCE_SITE, 7, kBoardDefenceStub, sizeof kBoardDefenceStub, 62, 0x11E428Eu, 67, 0x11E4429u}};
/* Mission stratagems (HfDifficulty.mission_stratagem): every player's mission
 * group (Reinforce, SOS, Resupply...; no loadout slot) starts from a table of 8 sets of 6 types (0x213F550), one per
 * mission kind, read only at 0x6AE35D (lea r14, [r15+table]; r15 the image base). At the added level it reads a copy
 * with the level's type in each set's first empty place (sets 1..7; set 0 is empty and unused). Uses and cooldown are
 * the type's own; any type is accepted (galactic war effects already grant vehicles this way). */
#define DIF_GRANTS 0x213F550u
#define DIF_GRANT_SITE 0x6AE35Du
#define DIF_GRANT_BACK 0x6AE364u
static const uint8_t kGrantPin[7] = {0x4D, 0x8D, 0xB7, 0x50, 0xF5, 0x13, 0x02};
static const uint8_t kGrantStub[66] = {
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  0 mov rax, game+0x3326340 (imm64 at 2) */
    0x48, 0x8B, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x24,                   /* 10 mov rax, [rax] (the world); test; je 54 */
    0x48, 0x8B, 0x80, 0x88, 0xE2, 0x0A, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x18, /* 18 mov rax, [rax+0xae288] (the level); test; je 54 */
    0x80, 0xB8, 0xF9, 0x00, 0x00, 0x00, DIF_VANILLA + 1, 0x75, 0x0F,  /* 30 cmp byte [rax+0xf9], 11; jne 54 */
    0x49, 0xBE, 0, 0, 0, 0, 0, 0, 0, 0,                               /* 39 mov r14, the level's copy (imm64 at 41) */
    0xE9, 0, 0, 0, 0,                                                 /* 49 jmp 0x6AE364 (rel32 at 50) */
    0x4D, 0x8D, 0xB7, 0x50, 0xF5, 0x13, 0x02,                         /* 54 lea r14, [r15+0x213F550] */
    0xE9, 0, 0, 0, 0};                                                /* 61 jmp 0x6AE364 (rel32 at 62) */
/* The end of objective selection (0x1756730; every caller: war table, descriptor, preview, telemetry): eax = the count,
 * [rbp-0x70] the ids (32 places), [rbp-0x78] the classes or 0, [rsp+0x60] the inputs (+0 seed, +8 mission row, +0xC
 * faction, +0x10 difficulty, +0x14 a byte the selection never reads), [rsp+0x44] the planet's map layout. The count is
 * capped at the objective list's places, and at the added level MixMission adds a second mission's main objectives. */
#define DIF_SELECTION_END 0x175784Fu
#define DIF_SELECTION_BACK 0x1757856u
static const uint8_t kSelectionPin[7] = {0x48, 0x8B, 0x8D, 0xE0, 0x0D, 0x00, 0x00}; /* mov rcx, [rbp+0xde0] */
static const uint8_t kSelectionStub[78] = {
    0x83, 0xF8, DIF_OBJECTIVES_MAX, 0x76, 0x05, 0xB8, DIF_OBJECTIVES_MAX, 0x00, 0x00, 0x00, /*  0 eax = min(eax, 30) */
    0x85, 0xC0, 0x74, 0x34,                                           /* 10 test eax, eax; je 66 */
    0x48, 0x8B, 0x4C, 0x24, 0x60,                                     /* 14 mov rcx, [rsp+0x60] (the inputs) */
    0x83, 0x79, 0x10, DIF_VANILLA + 1, 0x75, 0x29,                    /* 19 cmp dword [rcx+0x10], 11; jne 66 */
    0x89, 0xC2,                                                       /* 25 mov edx, eax */
    0x4C, 0x8B, 0x45, 0x90,                                           /* 27 mov r8, [rbp-0x70] (ids) */
    0x4C, 0x8B, 0x4D, 0x88,                                           /* 31 mov r9, [rbp-0x78] (classes) */
    0x44, 0x0F, 0xB6, 0x54, 0x24, 0x44,                               /* 35 movzx r10d, byte [rsp+0x44] (layout) */
    0x48, 0x83, 0xEC, 0x30,                                           /* 41 sub rsp, 0x30 */
    0x4C, 0x89, 0x54, 0x24, 0x20,                                     /* 45 mov [rsp+0x20], r10 */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /* 50 mov rax, MixMission (imm64 at 52) */
    0xFF, 0xD0,                                                       /* 60 call rax (eax = the new count) */
    0x48, 0x83, 0xC4, 0x30,                                           /* 62 add rsp, 0x30 */
    0x48, 0x8B, 0x8D, 0xE0, 0x0D, 0x00, 0x00,                         /* 66 mov rcx, [rbp+0xde0] */
    0xE9, 0, 0, 0, 0};                                                /* 73 jmp 0x1757856 (rel32 at 74) */
#define DIF_RAPID_DELTA 0xF0D6C1FA360B2B05ull     /* Rapid Acquisition's (row 8) */
#define DIF_ERADICATE_DELTA 0xC5DD9A6650C23B4Aull /* the Eradicate game mode's mission delta (rows 7, 40, 56, 65, 110, 120, 145) */
/* Two missions in one (HF_DIFFICULTY_SECOND_PRIMARY): a level-11 mission is
 * its own mission plus a partner mission's main objectives with their steps (Launch ICBM: codes, power, launch), as the
 * game selects them for the partner. Partners come from game data at install: the same faction, a standard mission (kind 1,
 * no game-mode delta, enabled, offered at 11) that an operation lists with the base mission (the board's planet-type
 * gating; Eradicate missions are partners too, never bases), whose main objectives the base mission does not list and
 * whose steps it lists only as steps. At the end of the
 * base's selection the hook picks one (seeded by the mission) whose map layouts include the base's, runs the game's own
 * selection for it (nested; marked through the inputs' +0x14 byte, so its own end hook only reports the layout it chose,
 * retried with derived seeds until that is the base's) and appends its class 0 and 1 objectives. An objective a mission's row
 * does not list counts as main, so the partners' steps are registered in the base rows as class 1 with weight 0: the
 * selection never draws weight 0 (levels 1..10 unchanged), and the game's first-match class lookups gate every main
 * objective on all steps of both missions. */
#define DIF_SELECTION 0x1756730u
#define DIF_LIVE_EVENT_OBJECTIVE 0x68BFBB59u
#define DIF_MIX_MARK 0xA5u
/* The radar station (HF_DIFFICULTY_RADAR): Objective_gen_radar_station, optional
 * (class 3) in 104 rows, any faction, one per mission, layouts {1,2,3,4} (def +0x98). The selection end hook keeps one in
 * every level-11 mission whose row classes it optional (it replaces the last optional pick, so the optional count stays),
 * where the def allows the map's layout, except Automaton city and factory maps (no vanilla mission has it there); rows
 * offered at 11 with optional objectives but without it get it as an optional entry of weight 0 (never drawn; the class
 * lookup then says optional). An objective's network record +0x44, discovered_state, starts at 1 (unknown: not on the
 * map) for optional objectives (0x5DEF00 from the class); at level 11 the radar's starts at 0, like a major objective's. */
#define DIF_RADAR 0xF1969B14u
#define DIF_RADAR_STATE_SITE 0x5DF35Bu /* mov rax, [rbp+0x68]; mov [rax+r14+0x44], ecx (ecx = the state, r8d = the def id) */
#define DIF_RADAR_STATE_BACK 0x5DF364u
static const uint8_t kRadarStatePin[30] = {0x8B, 0x41, 0x0C, 0x83, 0xF8, 0x02, 0x76, 0x0A, 0xB9, 0x01, 0x00, 0x00, 0x00, 0x83, 0xF8,
    0x04, 0x75, 0x03, 0x41, 0x8B, 0xCC, 0x48, 0x8B, 0x45, 0x68, 0x42, 0x89, 0x4C, 0x30, 0x44}; /* from 0x5DF346 */
static const uint8_t kRadarStateStub[69] = {
    0x83, 0xF9, 0x01, 0x75, 0x32,                                     /*  0 cmp ecx, 1; jne 55 */
    0x41, 0x81, 0xF8, 0x14, 0x9B, 0x96, 0xF1, 0x75, 0x29,             /*  5 cmp r8d, radar; jne 55 */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /* 14 mov rax, game+0x3326340 (imm64 at 16) */
    0x48, 0x8B, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x17,                   /* 24 mov rax, [rax] (the world); test; je 55 */
    0x48, 0x8B, 0x80, 0x88, 0xE2, 0x0A, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x0B, /* 32 mov rax, [rax+0xae288] (the level); test; je 55 */
    0x80, 0xB8, 0xF9, 0x00, 0x00, 0x00, DIF_VANILLA + 1, 0x75, 0x02,  /* 44 cmp byte [rax+0xf9], 11; jne 55 */
    0x31, 0xC9,                                                       /* 53 xor ecx, ecx (discovered) */
    0x48, 0x8B, 0x45, 0x68,                                           /* 55 mov rax, [rbp+0x68] */
    0x42, 0x89, 0x4C, 0x30, 0x44,                                     /* 59 mov [rax+r14+0x44], ecx */
    0xE9, 0, 0, 0, 0};                                                /* 64 jmp 0x5DF364 (rel32 at 65) */
#define DIF_MANDATORY_MAX 16u     /* the HUD's list of class 0 and 1 objectives (0x17CF690) */
#define DIF_MIX_PARTNERS 16u
#define DIF_OPERATION_MISSIONS 32u /* an operation's missions: +0x38, {u32 mission id (row +0), f32 weight} */
static const LocationSite kStampRefs[21] = {
    {0xC1AD5Bu, 4, 0x8BBD70u}, {0xC1B673u, 4, 0x8BBD70u}, {0xC1B946u, 4, 0x8BBD70u}, {0xC1C282u, 4, 0x8BBD70u},
    {0xC1C83Fu, 4, 0x8BBD70u}, {0xC1CAD5u, 4, 0x8BBD70u}, {0xC1D24Du, 4, 0x8BBD70u}, {0xC27B21u, 4, 0x8BBD70u},
    {0xC282CCu, 4, 0x8BBD70u}, {0xC28566u, 4, 0x8BBD70u}, {0xC28B99u, 4, 0x8BBD70u}, {0xC2907Cu, 4, 0x8BBD70u},
    {0xC292D1u, 4, 0x8BBD70u}, {0xC2973Bu, 4, 0x8BBD70u}, {0xC2FB25u, 4, 0x8BBD70u}, {0xC30439u, 4, 0x8BBD70u},
    {0xC30701u, 4, 0x8BBD70u}, {0xC31039u, 4, 0x8BBD70u}, {0xC315F6u, 4, 0x8BBD70u}, {0xC3187Fu, 4, 0x8BBD70u},
    {0xC31FF3u, 4, 0x8BBD70u}};
static uint32_t MinimapMoved(uint32_t value)
{
    return value == DIF_MINIMAP_P ? DIF_MINIMAP_P_NEW : value == DIF_MINIMAP_S ? DIF_MINIMAP_S_NEW : value == DIF_MINIMAP_U ? DIF_MINIMAP_U_NEW : DIF_MINIMAP_V_NEW;
}
#define DIF_WORLD 0x3326340u /* the world: +0xAE288 its level generator, whose +0x2C8 is G */
static uint32_t LocationMoved(uint32_t value)
{
    if (value >= DIF_LOCATIONS_A && value < DIF_LOCATIONS_A + 0xE0u) return DIF_LOCATIONS_A_NEW + (value - DIF_LOCATIONS_A);
    return DIF_LOCATIONS_B_NEW + (value - DIF_LOCATIONS_B);
}
/* The height zones (force_flatten_zones, 0xF4C100, pass op 0x3E): each group of connected cells whose type sets a height
 * (+0x208) gets an entry of a 16-entry table on the stack (rbp+0x60, 12 bytes each, unchecked), and the second loop
 * flattens every zone cell to its group's height (0xF4C3E6). Level 10 stays below 16; a 2 x map made more and the stack
 * cookie check ended the game (0xC0000409). The table moves to the generation arena, sized by the route cells (r12d;
 * groups can never outnumber them): the clear at 0xF4C1CD becomes the
 * allocation (its pointer kept at rbp+0x60), the loop's lea of the table (0xF4C29C) a load of that pointer, and the
 * height read (0xF4C3E6) goes through it. (Capping the groups at 16 left later cells with group 0's height: the outer
 * cities sank under a flat plain, roads and buildings buried.) */
#define DIF_ZONE_SITE 0xF4C260u
static const uint8_t kZonePin[14] = {0x41, 0x80, 0x3C, 0x24, 0x00, 0x45, 0x8B, 0xC6, 0x0F, 0x85, 0xC7, 0x00, 0x00, 0x00};
#define DIF_ZONE_CLEAR 0xF4C1CDu       /* xor edx,edx; mov [rbp-0x48],rax; mov r8d,0xc0; lea rcx,[rbp+0x60]; call memset */
#define DIF_ZONE_CLEAR_BACK 0xF4C1E2u
#define DIF_ZONE_TABLE_LEA 0xF4C29Cu   /* lea rbx, [rbp+0x60] (byte +1: 8D -> 8B, a load) */
#define DIF_ZONE_HEIGHT 0xF4C3E6u      /* movss xmm9, [rbp+rcx*4+0x68] */
#define DIF_ZONE_HEIGHT_BACK 0xF4C3EDu
#define DIF_ARENA_ALLOCATE 0x173B8D0u  /* (arena, size, alignment) */
#define DIF_MEMSET 0x208AAA0u
static const uint8_t kZoneClearPin[21] = {0x33, 0xD2, 0x48, 0x89, 0x45, 0xB8, 0x41, 0xB8, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x4D, 0x60,
    0xE8, 0xBE, 0xE8, 0x13, 0x01};
static const uint8_t kZoneTableLeaPin[8] = {0x48, 0x8D, 0x5D, 0x60, 0x48, 0x8D, 0x1C, 0x8B};
static const uint8_t kZoneHeightPin[7] = {0xF3, 0x44, 0x0F, 0x10, 0x4C, 0x8D, 0x68};
static const uint8_t kZoneClearStub[62] = {
    0x48, 0x89, 0x45, 0xB8,                                           /*  0 mov [rbp-0x48], rax (the replaced store) */
    0x44, 0x89, 0xE2, 0xFF, 0xC2, 0x8D, 0x14, 0x52, 0xC1, 0xE2, 0x02, /*  4 edx = (route cells + 1) x 12 */
    0x48, 0x8B, 0x0E,                                                 /* 15 mov rcx, [rsi] (the generation arena) */
    0x41, 0xB8, 0x08, 0x00, 0x00, 0x00,                               /* 18 mov r8d, 8 */
    0xE8, 0, 0, 0, 0,                                                 /* 24 call 0x173B8D0 (rel32 at 25) */
    0x48, 0x89, 0x45, 0x60,                                           /* 29 mov [rbp+0x60], rax (the table) */
    0x48, 0x89, 0xC1,                                                 /* 33 mov rcx, rax */
    0x45, 0x89, 0xE0, 0x41, 0xFF, 0xC0, 0x47, 0x8D, 0x04, 0x40, 0x41, 0xC1, 0xE0, 0x02, /* 36 r8d = (route cells + 1) x 12 */
    0x31, 0xD2,                                                       /* 50 xor edx, edx */
    0xE8, 0, 0, 0, 0,                                                 /* 52 call memset (rel32 at 53) */
    0xE9, 0, 0, 0, 0};                                                /* 57 jmp 0xF4C1E2 (rel32 at 58) */
/* Diagnostic (small cities on larger maps): the clear stub ends through kZoneNoteStub (the table, the pass record, the
 * level, the route cell count into g_zoneDiag), the height read starts with kZoneCellStub (the cell's group marker and
 * info cursors); DifficultyWatch writes zones-<n>.tsv (cell types, groups, heights) once the pass is done. */
static const uint8_t kZoneNoteStub[53] = {
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  0 mov rax, g_zoneDiag (imm64 at 2) */
    0x48, 0x8B, 0x4D, 0x60, 0x48, 0x89, 0x08,                         /* 10 mov rcx, [rbp+0x60]; mov [rax], rcx (the table) */
    0x4C, 0x89, 0x78, 0x08,                                           /* 17 mov [rax+8], r15 (the pass record) */
    0x48, 0x8B, 0x4E, 0x18, 0x48, 0x89, 0x48, 0x10,                   /* 21 mov rcx, [rsi+0x18]; mov [rax+0x10], rcx (the level) */
    0x44, 0x89, 0x60, 0x18,                                           /* 29 mov [rax+0x18], r12d (route cells) */
    0x49, 0x8B, 0x0F, 0x48, 0x89, 0x48, 0x30,                         /* 33 mov rcx, [r15]; mov [rax+0x30], rcx (the cells' info) */
    0x49, 0x8B, 0x4F, 0x20, 0x48, 0x89, 0x48, 0x38,                   /* 40 mov rcx, [r15+0x20]; mov [rax+0x38], rcx (the cell types) */
    0xE9, 0, 0, 0, 0};                                                /* 48 jmp 0xF4C1E2 (rel32 at 49) */
static const uint8_t kZoneCellStub[23] = {
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  0 mov rax, g_zoneDiag (imm64 at 2) */
    0x4C, 0x89, 0x70, 0x20,                                           /* 10 mov [rax+0x20], r14 (the cell's marker + 4) */
    0x48, 0x89, 0x58, 0x28,                                           /* 14 mov [rax+0x28], rbx (its info + 0x18) */
    0xE9, 0, 0, 0, 0};                                                /* 18 jmp kZoneHeightStub (rel32 at 19) */
static volatile uint64_t g_zoneDiag[8];
static const uint8_t kZoneHeightStub[16] = {
    0x48, 0x8B, 0x45, 0x60,                                           /*  0 mov rax, [rbp+0x60] (the table) */
    0xF3, 0x44, 0x0F, 0x10, 0x4C, 0x88, 0x08,                         /*  4 movss xmm9, [rax+rcx*4+8] */
    0xE9, 0, 0, 0, 0};                                                /* 11 jmp 0xF4C3ED (rel32 at 12) */
static const uint8_t kCellSpacingPin[7] = {0xF3, 0x0F, 0x5E, 0xD0, 0x48, 0x8B, 0x12};
static const uint8_t kCellSpacingStub[52] = {
    0xF3, 0x0F, 0x5E, 0xD0,                                           /*  0 divss xmm2, xmm0 */
    0x50,                                                             /*  4 push rax */
    0x48, 0x8B, 0x42, 0x18,                                           /*  5 mov rax, [rdx+0x18] */
    0x81, 0xB8, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /*  9 cmp dword [rax+0x2d7c], disk / 1024 (at 15) */
    0x58,                                                             /* 19 pop rax */
    0x75, 0x16,                                                       /* 20 jne 44 */
    0xF3, 0x0F, 0x59, 0x15, 0, 0, 0, 0,                               /* 22 mulss xmm2, [rip+factor x 4] (rel32 at 26) */
    0x66, 0x0F, 0x3A, 0x0A, 0xD2, 0x02,                               /* 30 roundss xmm2, xmm2, ceil */
    0xF3, 0x0F, 0x59, 0x15, 0, 0, 0, 0,                               /* 36 mulss xmm2, [rip+0.25] (rel32 at 40) */
    0x48, 0x8B, 0x12,                                                 /* 44 mov rdx, [rdx] */
    0xE9, 0, 0, 0, 0};                                                /* 47 jmp back (rel32 at 48) */
static const uint8_t kGridPin[9] = {0x4C, 0x89, 0x74, 0x24, 0x20, 0xF3, 0x0F, 0x10, 0x10};
/* Level init (0xF71220) gives a war-table preview (lg+0x2D48 set, at 0xF715BE) a view rect of (0.175, 0.175, 0.65,
 * 0.65) of the world (0xF7178C), +-666 m around the centre, missions (0, 0, 1, 1): lg+0x2D94 (0xF717A8), which every
 * render pass of the generator copies into its command, so the preview's terrain (the mission card's minimap and the
 * war table's hologram) was drawn only in that square, and a larger map's corners were cut off. The radii stub, for the
 * added level's larger maps, also writes the missions' (0, 0, 1, 1) there; level 10 never takes the branch. */
#define DIF_PREVIEW_RECT_SITE 0xF717A8u
static const uint8_t kPreviewRectPin[8] = {0x41, 0x0F, 0x11, 0x85, 0x94, 0x2D, 0x00, 0x00}; /* movups [r13+0x2d94], xmm0 */
static const uint8_t kRadiiStub[116] = {
    0x4C, 0x89, 0x2D, 0, 0, 0, 0,                                     /*  0 mov [rip+watch+0x20], r13 (the level; rel32 at 3) */
    0x49, 0x89, 0x95, 0xE0, 0x2D, 0x00, 0x00,                         /*  7 mov [r13+0x2de0], rdx */
    0x41, 0x80, 0xBD, 0xF9, 0x00, 0x00, 0x00, DIF_VANILLA + 1,        /* 14 cmp byte [r13+0xf9], 11 (the difficulty) */
    0x75, 0x57,                                                       /* 22 jne 111 */
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90,                               /* 24 (every map type) */
    0x41, 0xC7, 0x85, 0x78, 0x2D, 0x00, 0x00, 0, 0, 0, 0,             /* 30 mov dword [r13+0x2d78], fade / 1024 (at 37) */
    0x41, 0xC7, 0x85, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,             /* 41 mov dword [r13+0x2d7c], disk / 1024 (at 48) */
    0x41, 0xC7, 0x85, 0x84, 0x2D, 0x00, 0x00, 0, 0, 0, 0,             /* 52 mov dword [r13+0x2d84], rect low (at 59) */
    0x41, 0xC7, 0x85, 0x88, 0x2D, 0x00, 0x00, 0, 0, 0, 0,             /* 63 mov dword [r13+0x2d88], rect high (at 70) */
    0x41, 0xC7, 0x85, 0x8C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,             /* 74 mov dword [r13+0x2d8c], disk + 1 (at 81) */
    0x31, 0xC0,                                                       /* 85 xor eax, eax */
    0x49, 0x89, 0x85, 0x94, 0x2D, 0x00, 0x00,                         /* 87 mov [r13+0x2d94], rax (the view rect: x, y 0) */
    0x48, 0xB8, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F,       /* 94 mov rax, (1.0, 1.0) */
    0x49, 0x89, 0x85, 0x9C, 0x2D, 0x00, 0x00,                         /* 104 mov [r13+0x2d9c], rax (width, height 1) */
    0xE9, 0, 0, 0, 0};                                                /* 111 jmp back (rel32 at 112) */
/* The preview record copies the convoy route's vertices (G+0x8E9128, count G+0x11A71B8, up to 256) into its own list
 * (record +0x2644, 80 places, then its count at +0x28CC) without a check (0x1003D57): a larger map's longer route ran
 * over the count. The count is capped at 80 where it is stored. */
/* City buildings (generate_city_roads, op 0xA7, 0xFA9E10): the pass record's
 * footprint list (0xFA8FE0: 0x800 x 0x60 from the generation arena, capacity at +0x284) takes a footprint per road edge
 * first (~1250), then the city cells' buildings in cell order, and the cell loop stops when it would overflow (0xFAAED1):
 * later cells keep their flattened plot (flatten_city_regions) with no buildings, mostly a larger map's outer ones (cells
 * grow outward from a random seed). For larger maps the list has 0x2000 places and the per-cell list 0x800 (was 0x200).
 * {rva, instruction length, value at, width, pin, new value}. */
static const struct { uint32_t rva; uint8_t length, at, width; uint8_t pin[10]; uint32_t value; } kCityCapacities[7] = {
    {0xFA904Du, 5, 1, 4, {0xBA, 0x00, 0x00, 0x03, 0x00}, 0xC0000u},                                     /* allocation: 0x800 x 0x60 */
    {0xFA909Fu, 10, 6, 4, {0xC7, 0x81, 0x84, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00}, 0x2000u},       /* capacity (+0x284) */
    {0xFA90BEu, 6, 2, 4, {0x41, 0xB8, 0x00, 0x00, 0x03, 0x00}, 0xC0000u},                               /* clear size */
    {0xFAAD3Eu, 5, 1, 4, {0xBA, 0x00, 0xC0, 0x00, 0x00}, 0x30000u},                                     /* per-cell list: allocation */
    {0xFAAD4Du, 6, 2, 4, {0x41, 0xB8, 0x00, 0xC0, 0x00, 0x00}, 0x30000u},                               /* clear size */
    {0xFAAD67u, 7, 3, 4, {0xC7, 0x45, 0x04, 0x00, 0x02, 0x00, 0x00}, 0x800u},                           /* capacity */
    {0xFAAD84u, 7, 3, 4, {0x41, 0x81, 0xF8, 0x00, 0x02, 0x00, 0x00}, 0x800u}};                          /* its check */
/* Diagnostic: the footprint list's check (0xFAAECE `cmp edi, [rbp-0x10]; ja 0xFAAC94`, edi = the count with the cell's
 * entries) goes through a stub that keeps the most used and counts the overflows (the cell loop's early exit), which
 * DifficultyWatch logs: whether a level-11 city still loses its outer cells' buildings. Same behaviour otherwise. */
#define DIF_FOOTPRINT_CHECK 0xFAAECEu
#define DIF_FOOTPRINT_FITS 0xFAAED7u
#define DIF_FOOTPRINT_OVER 0xFAAC94u
static const uint8_t kFootprintPin[9] = {0x3B, 0x7D, 0xF0, 0x0F, 0x87, 0xBD, 0xFD, 0xFF, 0xFF};
static const uint8_t kFootprintStub[44] = {
    0x3B, 0x7D, 0xF0,                                                 /*  0 cmp edi, [rbp-0x10] (the capacity) */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  3 mov rax, g_cityFootprints (imm64 at 5) */
    0x77, 0x0B,                                                       /* 13 ja 26 */
    0x3B, 0x38, 0x76, 0x02, 0x89, 0x38,                               /* 15 cmp edi, [rax]; jbe 21; mov [rax], edi (most used) */
    0xE9, 0, 0, 0, 0,                                                 /* 21 jmp 0xFAAED7 (rel32 at 22) */
    0xF0, 0xFF, 0x40, 0x04,                                           /* 26 lock inc dword [rax+4] (overflows) */
    0x8B, 0x4D, 0xF0, 0x89, 0x48, 0x08,                               /* 30 mov ecx, [rbp-0x10]; mov [rax+8], ecx */
    0x89, 0x78, 0x0C,                                                 /* 36 mov [rax+0xc], edi (wanted) */
    0xE9, 0, 0, 0, 0};                                                /* 39 jmp 0xFAAC94 (rel32 at 40) */
static volatile LONG g_cityFootprints[4]; /* most used, overflows, capacity, wanted */
#define DIF_CONVOY_SITE 0x1003D57u
#define DIF_CONVOY_BACK 0x1003D63u
static const uint8_t kConvoyPin[12] = {0x8B, 0x88, 0xB8, 0x71, 0x1A, 0x01, 0x89, 0x8E, 0xCC, 0x28, 0x00, 0x00};
static const uint8_t kConvoyStub[27] = {
    0x8B, 0x88, 0xB8, 0x71, 0x1A, 0x01,                               /*  0 mov ecx, [rax+0x11a71b8] */
    0x83, 0xF9, 0x50,                                                 /*  6 cmp ecx, 80 */
    0x76, 0x05,                                                       /*  9 jbe 16 */
    0xB9, 0x50, 0x00, 0x00, 0x00,                                     /* 11 mov ecx, 80 */
    0x89, 0x8E, 0xCC, 0x28, 0x00, 0x00,                               /* 16 mov [rsi+0x28cc], ecx */
    0xE9, 0, 0, 0, 0};                                                /* 22 jmp 0x1003D63 (rel32 at 23) */
/* The spacing reads: the level's disk (lg+0x2D7C) is the one the stub above wrote, so the spacing is the level's. */
static const uint8_t kSpacingStub[34] = {
    0xF3, 0x0F, 0x10, 0x50, 0x04,                                     /*  0 movss xmm2, [rax+4] */
    0x49, 0x8B, 0x46, 0x18,                                           /*  5 mov rax, [r14+0x18] */
    0x81, 0xB8, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /*  9 cmp dword [rax+0x2d7c], disk / 1024 (at 15) */
    0x75, 0x08,                                                       /* 19 jne 29 */
    0xF3, 0x0F, 0x10, 0x15, 0, 0, 0, 0,                               /* 21 movss xmm2, [rip+spacing] (rel32 at 25) */
    0xE9, 0, 0, 0, 0};                                                /* 29 jmp back (rel32 at 30) */
static const uint8_t kSpacingStub2[46] = {
    0x51,                                                             /*  0 push rcx */
    0x48, 0x8B, 0x4C, 0x24, 0x68,                                     /*  1 mov rcx, [rsp+0x68] */
    0x48, 0x8B, 0x49, 0x18,                                           /*  6 mov rcx, [rcx+0x18] */
    0x81, 0xB9, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /* 10 cmp dword [rcx+0x2d7c], disk / 1024 (at 16) */
    0x59,                                                             /* 20 pop rcx */
    0x75, 0x0D,                                                       /* 21 jne 36 */
    0xF3, 0x0F, 0x5C, 0x35, 0, 0, 0, 0,                               /* 23 subss xmm6, [rip+spacing] (rel32 at 27) */
    0xE9, 0, 0, 0, 0,                                                 /* 31 jmp back (rel32 at 32) */
    0xF3, 0x0F, 0x5C, 0x70, 0x04,                                     /* 36 subss xmm6, [rax+4] */
    0xE9, 0, 0, 0, 0};                                                /* 41 jmp back (rel32 at 42) */
static const uint8_t kGridStub[38] = {
    0x4C, 0x89, 0x74, 0x24, 0x20,                                     /*  0 mov [rsp+0x20], r14 */
    0xF3, 0x0F, 0x10, 0x10,                                           /*  5 movss xmm2, [rax] */
    0x48, 0x8B, 0x46, 0x18,                                           /*  9 mov rax, [rsi+0x18] */
    0x81, 0xB8, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /* 13 cmp dword [rax+0x2d7c], disk / 1024 (at 19) */
    0x75, 0x08,                                                       /* 23 jne 33 */
    0xF3, 0x0F, 0x10, 0x15, 0, 0, 0, 0,                               /* 25 movss xmm2, [rip+grid] (rel32 at 29) */
    0xE9, 0, 0, 0, 0};                                                /* 33 jmp back (rel32 at 34) */
#define DIF_BAR_SLOTS 0x598u      /* the bar's widgets: bar + 0x598 + i * 0x910, i < 10 (+0x6038 is another member) */
#define DIF_WIDGET 0x910u
#define DIF_BARS 32u              /* bars given an 11th widget (a bar keeps its own while its memory is reused) */

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

static const uint32_t kDifficultyItems[DIF_VANILLA] = {0xF96CE664u, 0x159D9650u, 0xA5E5CE1Au, 0x84DA1DECu, 0x6A828CA3u,
    0x8205BAA9u, 0x2FCC5936u, 0x4197851Bu, 0xD51072F6u, 0xA0AE653Au};
static const uint8_t kKeepTen[5] = {0xB8, 0x0A, 0x00, 0x00, 0x00}; /* mov eax, 0xa */

typedef struct Difficulty
{
    HfMod* mod;
    char name[192];
    uint32_t item, text[2];
    float encounter, guardforce, cooldown, patrolFrequency, patrolCap;
    uint32_t grant;
    uint64_t icon;
    float mapScale, outpostScale, minutes;
    uint32_t optional, major, flags;
} Difficulty;
static Difficulty g_difficulty;
static int g_difficultyAdded;
static int g_difficultyState; /* 0 waiting, 1 installed (or none added), -1 refused */
static uint8_t* g_difficultyTable;
static uintptr_t g_difficultyGame;
static uint32_t g_objectivePlaces = 16;  /* the objective list's places: 30 once moved */
static volatile LONG g_secondPrimaries, g_secondPrimaryMisses;

/* The first class the mission row gives an id (its 32 entries: +0 id, +0xC class), or -1. */
static int RowClass(uintptr_t row, uint32_t id)
{
    uint8_t entries[32 * 0x18];
    if (SafeRead(row + 0x40, entries, sizeof entries)) return -1;
    for (int i = 0; i < 32; i++)
    {
        uint32_t entry, cls;
        memcpy(&entry, entries + i * 0x18, 4); memcpy(&cls, entries + i * 0x18 + 0xC, 4);
        if (entry == id) return (int)cls;
    }
    return -1;
}

typedef uint32_t (*SelectFn)(void* inputs, uint32_t* ids, int32_t* classes);
static SelectFn g_select;          /* the game's selection (0x1756730); the fixture sets its own */
static uint8_t g_mixBase[DIF_MISSION_ROWS], g_mixPartnerCount[DIF_MISSION_ROWS], g_mixPartners[DIF_MISSION_ROWS][DIF_MIX_PARTNERS];
static volatile LONG g_mixSteps;

static uint64_t MixHash(uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t h = a * 0x5851F42D4C957F2Dull + 0x14057B7EF767814Full;
    h ^= (b << 32) | c;
    return h * 0x5851F42D4C957F2Dull + 0x14057B7EF767814Full;
}

/* The definition of an objective id (0x32EF870, +0 id, +0x20 most uses per mission, +0x24 / +0x28 levels), or 0. */
static uintptr_t ObjectiveDef(uintptr_t game, uint32_t id)
{
    for (uint32_t d = 1; d < DIF_OBJECTIVE_ROWS; d++)
    {
        uint32_t at = 0;
        if (!SafeRead(game + DIF_OBJECTIVES + d * DIF_OBJECTIVE_ROW, &at, 4) && at == id) return game + DIF_OBJECTIVES + d * DIF_OBJECTIVE_ROW;
    }
    return 0;
}

/* A row's map layouts (+0x368 pointer, +0x370 count; a first 0 means any). */
static int RowHasLayout(uintptr_t row, uint32_t layout)
{
    uint64_t list = 0;
    uint32_t n = 0;
    uint8_t layouts[16];
    if (SafeRead(row + 0x368, &list, 8) || SafeRead(row + 0x370, &n, 4) || list == 0 || n == 0) return 1;
    if (n > sizeof layouts || SafeRead((uintptr_t)list, layouts, n)) return 0;
    if (layouts[0] == 0) return 1;
    for (uint32_t i = 0; i < n; i++) if (layouts[i] == layout) return 1;
    return 0;
}

/* Partner lists and the steps' registration, once the ranges are widened. */
static void MixInstall(uintptr_t game)
{
    static uint8_t entries[DIF_MISSION_ROWS][32 * 0x18];
    uint32_t ids[DIF_MISSION_ROWS];
    uint8_t faction[DIF_MISSION_ROWS], standard[DIF_MISSION_ROWS], eradicate[DIF_MISSION_ROWS];
    for (uint32_t r = 0; r < DIF_MISSION_ROWS; r++)
    {
        uintptr_t row = game + DIF_MISSIONS + r * DIF_MISSION_ROW;
        uint8_t head[0x38], kind = 0, enabled = 0;
        uint64_t delta = 1;
        uint32_t range[2] = {0, 0};
        standard[r] = 0; eradicate[r] = 0; ids[r] = 0; faction[r] = 0;
        if (SafeRead(row, head, sizeof head) || SafeRead(row + 0x40, entries[r], sizeof entries[r]) || SafeRead(row + 0x360, &delta, 8)) continue;
        memcpy(&ids[r], head, 4); enabled = head[4]; faction[r] = head[8]; memcpy(range, head + 0xC, 8); kind = head[0x34];
        standard[r] = kind == 1 && delta == 0 && enabled && range[0] <= DIF_VANILLA + 1 && DIF_VANILLA + 1 <= range[1];
        /* Eradicate missions are partners only (their kill objective counts kills by itself; the game mode's delta, which
         * shrinks the map, is not part of mixing), offered up to 10 at least; never where the difficulty has no Eradicate
         * (its kill objective would be a main objective all the same). */
        eradicate[r] = delta == DIF_ERADICATE_DELTA && enabled && range[0] <= DIF_VANILLA && DIF_VANILLA <= range[1] &&
                       !(g_difficulty.flags & HF_DIFFICULTY_NO_ERADICATE);
    }
    /* Which operations list which missions. */
    static uint8_t listed[DIF_OPERATION_ROWS][DIF_MISSION_ROWS];
    memset(listed, 0, sizeof listed);
    for (uint32_t o = 0; o < DIF_OPERATION_ROWS; o++)
    {
        uint8_t list[DIF_OPERATION_MISSIONS * 8];
        if (SafeRead(game + DIF_OPERATIONS + o * DIF_OPERATION_ROW + 0x38, list, sizeof list)) continue;
        for (uint32_t k = 0; k < DIF_OPERATION_MISSIONS; k++)
        {
            uint32_t id;
            memcpy(&id, list + 8 * k, 4);
            for (uint32_t r = 0; id != 0 && r < DIF_MISSION_ROWS; r++) if (ids[r] == id) listed[o][r] = 1;
        }
    }
    uint32_t bases = 0, steps = 0;
    for (uint32_t b = 0; b < DIF_MISSION_ROWS; b++)
    {
        g_mixBase[b] = 0; g_mixPartnerCount[b] = 0;
        if (!standard[b]) continue;
        for (uint32_t r = 0; r < DIF_MISSION_ROWS && g_mixPartnerCount[b] < DIF_MIX_PARTNERS; r++)
        {
            if (r == b || !(standard[r] || eradicate[r]) || faction[r] != faction[b]) continue;
            int together = 0, mains = 0, conflict = 0;
            for (uint32_t o = 0; o < DIF_OPERATION_ROWS; o++) together |= listed[o][b] && listed[o][r];
            if (!together) continue;
            for (int k = 0; k < 32 && !conflict; k++)
            {
                uint32_t id, cls; float weight;
                memcpy(&id, entries[r] + k * 0x18, 4); memcpy(&weight, entries[r] + k * 0x18 + 4, 4); memcpy(&cls, entries[r] + k * 0x18 + 0xC, 4);
                if (id == 0 || !(weight > 0.0f) || cls > 1) continue;
                int in = -1;
                for (int j = 0; j < 32 && in < 0; j++)
                {
                    uint32_t other, otherClass;
                    memcpy(&other, entries[b] + j * 0x18, 4); memcpy(&otherClass, entries[b] + j * 0x18 + 0xC, 4);
                    if (other == id) in = (int)otherClass;
                }
                if (cls == 0)
                {
                    uintptr_t def = ObjectiveDef(game, id);
                    uint32_t levels[2] = {0, 0};
                    if (in >= 0) conflict = 1;
                    else if (def && !SafeRead(def + 0x24, levels, 8) && levels[0] <= DIF_VANILLA + 1 && DIF_VANILLA + 1 <= levels[1]) mains++;
                }
                else if (in >= 0 && in != 1) conflict = 1;
            }
            if (!conflict && mains > 0) g_mixPartners[b][g_mixPartnerCount[b]++] = (uint8_t)r;
        }
        g_mixBase[b] = g_mixPartnerCount[b] > 0;
        bases += g_mixBase[b];
    }
    /* The partners' steps, as class 1 (weight 0) in the base rows' free entries. */
    for (uint32_t b = 0; b < DIF_MISSION_ROWS; b++)
        for (uint32_t p = 0; p < g_mixPartnerCount[b]; p++)
        {
            uint32_t r = g_mixPartners[b][p];
            for (int k = 0; k < 32; k++)
            {
                uint32_t id, cls; float weight;
                memcpy(&id, entries[r] + k * 0x18, 4); memcpy(&weight, entries[r] + k * 0x18 + 4, 4); memcpy(&cls, entries[r] + k * 0x18 + 0xC, 4);
                if (id == 0 || !(weight > 0.0f) || cls != 1) continue;
                int in = 0, free = -1;
                for (int j = 0; j < 32; j++)
                {
                    uint32_t other;
                    static const uint8_t zero[0x18];
                    memcpy(&other, entries[b] + j * 0x18, 4);
                    if (other == id) in = 1;
                    if (free < 0 && !memcmp(entries[b] + j * 0x18, zero, 0x18)) free = j;
                }
                if (in || free < 0) continue;
                uint32_t entry[6] = {id, 0, 0, 1, 0, 0}; /* weight 0.0: never drawn */
                if (ApiWrite(game + DIF_MISSIONS + b * DIF_MISSION_ROW + 0x40 + free * 0x18, entry, sizeof entry)) continue;
                memcpy(entries[b] + free * 0x18, entry, sizeof entry);
                steps++;
            }
        }
    g_select = (SelectFn)(game + DIF_SELECTION);
    Note("difficulty: %u missions can be mixed with a partner mission (%u partner steps registered as steps)", bases, steps);
}

/* The radar station kept in a level-11 mission's selection (see DIF_RADAR). */
static uint32_t RadarKeep(uint8_t* inputs, uint32_t count, uint32_t* ids, int32_t* classes, uint32_t layout)
{
    uintptr_t game = g_difficultyGame;
    uint32_t in[4];
    memcpy(in, inputs, sizeof in);
    if (!(g_difficulty.flags & HF_DIFFICULTY_RADAR) || game == 0 || ids == NULL || in[2] >= DIF_MISSION_ROWS || count == 0 || count > 32) return count;
    for (uint32_t i = 0; i < count; i++) if (ids[i] == DIF_RADAR) return count;
    uintptr_t row = game + DIF_MISSIONS + in[2] * DIF_MISSION_ROW, def = ObjectiveDef(game, DIF_RADAR);
    uint8_t layouts[8];
    uint32_t levels[2];
    /* Automaton cities (layout 3) never have one in vanilla; Hell Forge wants it there too (live test). Factory worlds
     * (layout 5) are outside the def's layouts. */
    if (RowClass(row, DIF_RADAR) != 3 || def == 0 || layout == 5 || SafeRead(def + 0x98, layouts, 8) ||
        SafeRead(def + 0x24, levels, 8) || levels[0] > DIF_VANILLA + 1 || levels[1] < DIF_VANILLA + 1) return count;
    int placed = layouts[0] == 0;
    for (int i = 0; i < 8 && layouts[i] != 0; i++) placed |= layouts[i] == layout;
    if (!placed) return count;
    for (uint32_t i = count; i-- > 0;)
        if (RowClass(row, ids[i]) == 3)
        {
            ids[i] = DIF_RADAR;
            if (classes != NULL) classes[i] = 3;
            return count;
        }
    if (count >= g_objectivePlaces) return count;
    ids[count] = DIF_RADAR;
    if (classes != NULL) classes[count] = 3;
    return count + 1;
}

/* Rows offered at the added level with optional objectives but no radar station get it as a weight-0 optional entry. */
static void RadarInstall(uintptr_t game)
{
    uint32_t added = 0;
    for (uint32_t r = 0; r < DIF_MISSION_ROWS; r++)
    {
        uintptr_t row = game + DIF_MISSIONS + r * DIF_MISSION_ROW;
        uint8_t head[0x38], entries[32 * 0x18];
        uint64_t delta = 1;
        uint32_t range[2];
        if (SafeRead(row, head, sizeof head) || SafeRead(row + 0x40, entries, sizeof entries) || SafeRead(row + 0x360, &delta, 8)) continue;
        memcpy(range, head + 0xC, 8);
        /* rows up to level 10 count: the widening (after this) gives them 11 (Sabotage Air Base, Destroy Command Bunkers) */
        if (head[0x34] != 1 || delta != 0 || !head[4] || range[0] > DIF_VANILLA + 1 || range[1] < DIF_VANILLA) continue;
        int optional = 0, has = 0, empty = -1;
        for (int k = 0; k < 32; k++)
        {
            uint32_t id, cls; float weight;
            static const uint8_t zero[0x18];
            memcpy(&id, entries + k * 0x18, 4); memcpy(&weight, entries + k * 0x18 + 4, 4); memcpy(&cls, entries + k * 0x18 + 0xC, 4);
            has |= id == DIF_RADAR;
            optional |= id != 0 && cls == 3 && weight > 0.0f;
            if (empty < 0 && !memcmp(entries + k * 0x18, zero, 0x18)) empty = k;
        }
        if (has || !optional || empty < 0) continue;
        uint32_t entry[6] = {DIF_RADAR, 0, 0, 3, 0, 0xFFFFFFFFu};
        if (!ApiWrite(row + 0x40 + empty * 0x18, entry, sizeof entry)) added++;
    }
    Note("difficulty: the radar station is kept in every level-11 mission (%u rows given it as an optional entry)", added);
}

/* City ICBM missions (HF_DIFFICULTY_CITY_ICBM). Three of the game's mission
 * rows are disabled and listed by no operation (Automaton 50, Terminid 61, Illuminate 133; their ids are kept, each is
 * registered once by the table's static init). Each becomes a Launch ICBM mission on city maps, levels 11..11: the
 * faction's city row (36 / 104 / 139: faction, kind 1, mission key, layout list {3}) with the faction's ICBM row's name
 * and briefing (0 / 59 / 119), gen_icbm x 3 (class 0, min = max = 3), the codes, power and refuelling steps (class 1,
 * 1 each) and the city row's majors and optionals. Its id goes first in the 7 city operations' mission lists with
 * weight 1e30: an operation's first mission is forced to kind 1 and drawn by list weight among the least-used rows, so at
 * level 11 it is always this one (the sum of the weights stays 1e30; levels 1..10 never list the row). Written after
 * MixInstall, which built its bases and partners before, so the rows are neither. */
#define DIF_OPS 0x32FEF10u
#define DIF_OP 0x490u
#define DIF_ICBM 0xB469DB1Cu
static const struct { uint8_t row, city, icbm; uint32_t id; } kCityIcbmRows[3] = {
    {50, 36, 0, 0x88A2290Bu}, {61, 104, 59, 0xF485F086u}, {133, 139, 119, 0xCB1A023Fu}};
static const uint8_t kCityIcbmOps[7][2] = {{2, 1}, {3, 1}, {10, 0}, {11, 0}, {17, 2}, {18, 2}, {22, 2}}; /* {operation, row} */
static void CityIcbmInstall(uintptr_t game)
{
    uint32_t rows = 0, ops = 0;
    int ready[3] = {0};
    for (int f = 0; f < 3; f++)
    {
        static uint8_t row[DIF_MISSION_ROW], city[DIF_MISSION_ROW], icbm[DIF_MISSION_ROW];
        uintptr_t at = game + DIF_MISSIONS + kCityIcbmRows[f].row * DIF_MISSION_ROW;
        uint32_t id = 0;
        if (SafeRead(at, row, DIF_MISSION_ROW) || SafeRead(game + DIF_MISSIONS + kCityIcbmRows[f].city * DIF_MISSION_ROW, city, DIF_MISSION_ROW) ||
            SafeRead(game + DIF_MISSIONS + kCityIcbmRows[f].icbm * DIF_MISSION_ROW, icbm, DIF_MISSION_ROW)) continue;
        memcpy(&id, row, 4);
        if (id != kCityIcbmRows[f].id || row[4] != 0 || memcmp(city + 8, row + 8, 4) != 0 || memcmp(icbm + 8, row + 8, 4) != 0 || city[0x34] != 1)
            continue; /* not the reviewed rows */
        uint8_t made[DIF_MISSION_ROW];
        memcpy(made, city, sizeof made);
        memcpy(made, &id, 4);
        made[4] = 1;
        uint32_t level = DIF_VANILLA + 1, none = 0;
        memcpy(made + 0xC, &level, 4); memcpy(made + 0x10, &level, 4);
        memcpy(made + 0x28, &none, 4); memcpy(made + 0x2C, &none, 4);
        memcpy(made + 0x340, icbm + 0x340, 0x10); /* name, briefing, media */
        memset(made + 0x378, 0, 8);
        uint32_t entries[32][6];
        memset(entries, 0, sizeof entries);
        const uint32_t own[4][2] = {{DIF_ICBM, 3}, {0xBB622974u, 1}, {0xFD5054DAu, 1}, {0x6B7A6B8Au, 1}}; /* icbm, codes, power, refuel */
        float one = 1.0f;
        uint32_t n = 0, weight;
        memcpy(&weight, &one, 4);
        for (int k = 0; k < 4; k++)
        {
            entries[n][0] = own[k][0]; entries[n][1] = weight; entries[n][2] = 0;
            entries[n][3] = k == 0 ? 0 : 1; entries[n][4] = own[k][1]; entries[n][5] = own[k][1];
            n++;
        }
        for (int k = 0; k < 32 && n < 32; k++)
        {
            uint32_t entry[6];
            memcpy(entry, city + 0x40 + k * 0x18, sizeof entry);
            if (entry[0] == 0 || (entry[3] != 2 && entry[3] != 3)) continue;
            int dup = 0;
            for (uint32_t j = 0; j < n; j++) dup |= entries[j][0] == entry[0];
            if (!dup) memcpy(entries[n++], entry, sizeof entry);
        }
        memcpy(made + 0x40, entries, sizeof entries);
        if (ApiWrite(at, made, sizeof made)) continue;
        ready[f] = 1;
        rows++;
    }
    for (int o = 0; o < 7; o++)
    {
        int f = kCityIcbmOps[o][1];
        uintptr_t list = game + DIF_OPS + kCityIcbmOps[o][0] * DIF_OP + 0x38;
        uint32_t entries[32][2], cityId = 0, faction = 0, opFaction = 0;
        if (!ready[f] || SafeRead(list, entries, sizeof entries) || SafeRead(game + DIF_MISSIONS + kCityIcbmRows[f].city * DIF_MISSION_ROW, &cityId, 4) ||
            SafeRead(game + DIF_MISSIONS + kCityIcbmRows[f].city * DIF_MISSION_ROW + 8, &faction, 4) ||
            SafeRead(game + DIF_OPS + kCityIcbmOps[o][0] * DIF_OP + 8, &opFaction, 4) || (opFaction & 0xFF) != (faction & 0xFF)) continue;
        uint32_t used = 0, listed = 0, has = 0;
        while (used < 32 && entries[used][0] != 0) { listed |= entries[used][0] == cityId; has |= entries[used][0] == kCityIcbmRows[f].id; used++; }
        if (has || !listed || used >= 32) continue; /* already in, or not a reviewed city operation */
        memmove(entries[1], entries[0], used * sizeof entries[0]);
        float first = 1e30f;
        entries[0][0] = kCityIcbmRows[f].id;
        memcpy(&entries[0][1], &first, 4);
        if (!ApiWrite(list, entries, (used + 1) * sizeof entries[0])) ops++;
    }
    Note("difficulty: city operations start with a Launch ICBM mission at level 11 (%u of 3 mission rows, %u of 7 operations)", rows, ops);
}

/* A mixed mission's rows and objectives (id/class), once per distinct mix and for the first DIF_MIX_LOGGED a session: the
 * board selects the same missions again and again, and a stuck objective needs only what its mission was made of. */
#define DIF_MIX_LOGGED 48u
static SRWLOCK g_mixLogLock = SRWLOCK_INIT;
static uint64_t g_mixLogged[DIF_MIX_LOGGED];
static uint32_t g_mixLoggedCount;
static void MixLog(uint32_t base, uint32_t partner, uint32_t layout, const uint32_t* ids, const int32_t* classes, uint32_t count)
{
    uintptr_t baseRow = g_difficultyGame + DIF_MISSIONS + base * DIF_MISSION_ROW;
    uint64_t hash = MixHash(base, partner, layout);
    for (uint32_t i = 0; i < count; i++) hash = MixHash(hash, ids[i], i);
    int fresh = 1, full = 0;
    AcquireSRWLockExclusive(&g_mixLogLock);
    for (uint32_t i = 0; i < g_mixLoggedCount && i < DIF_MIX_LOGGED; i++) fresh &= g_mixLogged[i] != hash;
    if (fresh && g_mixLoggedCount < DIF_MIX_LOGGED) g_mixLogged[g_mixLoggedCount] = hash;
    if (fresh && g_mixLoggedCount <= DIF_MIX_LOGGED) full = g_mixLoggedCount++ == DIF_MIX_LOGGED;
    else fresh = 0;
    ReleaseSRWLockExclusive(&g_mixLogLock);
    if (!fresh) return;
    if (full) { Note("difficulty: %u mixed missions logged; later ones are not", DIF_MIX_LOGGED); return; }
    char list[600];
    int at = 0;
    for (uint32_t i = 0; i < count && at < (int)sizeof list - 16; i++)
        at += snprintf(list + at, sizeof list - (size_t)at, " %08x/%d", ids[i], classes != NULL ? classes[i] : RowClass(baseRow, ids[i]));
    list[at] = 0;
    Note("difficulty: mission row %u mixed with row %u on layout %u, objectives (id/class):%s", base, partner, layout, list);
}

/* Called at the end of objective selection at the added level. The nested call (marked) only reports its layout. */
static uint32_t MixPartner(uint8_t* inputs, uint32_t count, uint32_t* ids, int32_t* classes, uint32_t layout, uint32_t* mixedWith);
static uint32_t MixMission(uint8_t* inputs, uint32_t count, uint32_t* ids, int32_t* classes, uint32_t layout)
{
    if (inputs[0x14] == DIF_MIX_MARK) { inputs[0x15] = (uint8_t)layout; inputs[0x16] = 1; return count; }
    uint32_t partner = UINT32_MAX, base;
    memcpy(&base, inputs + 8, 4);
    count = MixPartner(inputs, count, ids, classes, layout, &partner);
    count = RadarKeep(inputs, count, ids, classes, layout);
    if (partner != UINT32_MAX) MixLog(base, partner, layout, ids, classes, count);
    return count;
}

static uint32_t MixPartner(uint8_t* inputs, uint32_t count, uint32_t* ids, int32_t* classes, uint32_t layout, uint32_t* mixedWith)
{
    uintptr_t game = g_difficultyGame;
    uint32_t in[4];
    memcpy(in, inputs, sizeof in);
    uint32_t base = in[2];
    if (!(g_difficulty.flags & HF_DIFFICULTY_SECOND_PRIMARY) || game == 0 || g_select == NULL || ids == NULL || base >= DIF_MISSION_ROWS ||
        !g_mixBase[base] || count == 0 || count >= g_objectivePlaces) return count;
    uintptr_t baseRow = game + DIF_MISSIONS + base * DIF_MISSION_ROW;
    uint8_t candidates[DIF_MIX_PARTNERS];
    uint32_t n = 0;
    for (uint32_t p = 0; p < g_mixPartnerCount[base]; p++)
        if (RowHasLayout(game + DIF_MISSIONS + g_mixPartners[base][p] * DIF_MISSION_ROW, layout)) candidates[n++] = g_mixPartners[base][p];
    if (n == 0) { InterlockedIncrement(&g_secondPrimaryMisses); return count; }
    uint32_t start = (uint32_t)(MixHash(in[0], base, in[3]) >> 32) % n;
    for (uint32_t t = 0; t < n; t++)
    {
        uint32_t partner = candidates[(start + t) % n];
        for (uint32_t attempt = 0; attempt < 4; attempt++)
        {
            uint8_t nested[0x20];
            uint32_t got[32] = {0}, seed = attempt ? (uint32_t)(MixHash(in[0], partner, attempt) >> 32) : in[0];
            int32_t gotClasses[32] = {0};
            memcpy(nested, inputs, sizeof nested);
            memcpy(nested, &seed, 4); memcpy(nested + 8, &partner, 4);
            nested[0x14] = DIF_MIX_MARK; nested[0x16] = 0;
            uint32_t k = g_select(nested, got, gotClasses);
            if (k > 32 || nested[0x16] == 0 || nested[0x15] != layout) continue;
            uint32_t keep[32], keepClasses[32], kept = 0, mains = 0;
            for (uint32_t i = 0; i < k; i++)
            {
                if ((gotClasses[i] != 0 && gotClasses[i] != 1) || got[i] == DIF_LIVE_EVENT_OBJECTIVE) continue;
                uint32_t uses = 0, most = 0;
                uintptr_t def = ObjectiveDef(game, got[i]);
                for (uint32_t j = 0; j < count; j++) uses += ids[j] == got[i];
                for (uint32_t j = 0; j < kept; j++) uses += keep[j] == got[i];
                if (def && !SafeRead(def + 0x20, &most, 4) && most > 0 && uses >= most) continue;
                keep[kept] = got[i]; keepClasses[kept] = (uint32_t)gotClasses[i]; mains += gotClasses[i] == 0; kept++;
            }
            if (mains == 0) break;
            uint32_t mandatory = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                int c = RowClass(baseRow, ids[i]);
                mandatory += ids[i] != DIF_LIVE_EVENT_OBJECTIVE && c <= 1;
            }
            if (mandatory + kept > DIF_MANDATORY_MAX) break;
            while (count + kept > g_objectivePlaces)
            {
                uint32_t at = UINT32_MAX;
                for (uint32_t i = count; i-- > 0 && at == UINT32_MAX;) if (RowClass(baseRow, ids[i]) == 3) at = i;
                if (at == UINT32_MAX) break;
                for (uint32_t i = at; i + 1 < count; i++) { ids[i] = ids[i + 1]; if (classes != NULL) classes[i] = classes[i + 1]; }
                count--;
            }
            if (count + kept > g_objectivePlaces) break;
            for (uint32_t i = 0; i < kept; i++)
            {
                ids[count] = keep[i];
                if (classes != NULL) classes[count] = (int32_t)keepClasses[i];
                count++;
            }
            InterlockedIncrement(&g_secondPrimaries);
            InterlockedExchangeAdd(&g_mixSteps, (LONG)(kept - mains));
            *mixedWith = partner;
            return count;
        }
    }
    InterlockedIncrement(&g_secondPrimaryMisses);
    return count;
}

/* The level's countdown delta, registered once the game has loaded its entity delta table (polled). The table
 * (0x5154C0) is {pointer, count} x 5: an open-addressed hash (resource -> delta; 16-byte rows, slot = hash % count,
 * linear probing, key 0 = empty), the deltas ({component count, first}), their components ({type, first member,
 * member count}), the members ({record offset, size, data offset}) and the data. Indices and data offsets are relative
 * to each array's own pointer, so arrays 1 to 4 move to copies with the new rows appended; the hash row goes into a
 * free slot of the game's own hash last, so the delta is complete before its name resolves. */
static uintptr_t g_deltaTable;
static uintptr_t g_deltaArrays[5];
static uint32_t g_deltaAdded[5];
static uint64_t DifficultyDeltaHash(void)
{
    static const char name[] = "hellforge/difficulty/countdown";
    return Murmur64A(name, sizeof name - 1);
}

static uint8_t* g_launchStub; /* prepared by InstallDifficulty when the level has its own countdown */
static void DifficultyDelta(uintptr_t game)
{
    static int refused, patched;
    volatile uint8_t* watch = g_restartWatch;
    uint8_t* launchStub = g_launchStub;
    uintptr_t holder = 0, table = 0;
    uint64_t arrays[10];
    if (refused || watch == NULL || launchStub == NULL || SafeRead(game + DIF_DELTA_TABLE, &holder, 8) || holder == 0 ||
        SafeRead(holder + DIF_DELTA_HOLDER, &table, 8) || table == 0 || SafeRead(table, arrays, sizeof arrays)) return;
    if (table == g_deltaTable && arrays[2] == g_deltaArrays[1] && arrays[8] == g_deltaArrays[4]) return;
    if (g_deltaTable != 0)
    {
        watch[0x40] = 0;
        *(uint8_t* volatile*)(watch + 0x58) = NULL;
        Note("difficulty: the entity delta table changed (0x%llx); the countdown delta is registered again", (unsigned long long)table);
    }
    uint64_t hash = DifficultyDeltaHash();
    uint32_t slots = (uint32_t)arrays[1], deltas = (uint32_t)arrays[3], components = (uint32_t)arrays[5],
             members = (uint32_t)arrays[7], data = (uint32_t)arrays[9], at = (data + 3) & ~3u;
    if (slots == 0 || arrays[1] > 0x100000 || arrays[3] > 0x100000 || arrays[5] > 0x100000 || arrays[7] > 0x100000 || arrays[9] > 0x1000000)
    {
        refused = 1;
        ApiLog(g_difficulty.mod, "difficulty %s: the entity delta table is not as reviewed (%u slots, %u deltas); missions keep their own countdown",
            g_difficulty.name, slots, deltas);
        return;
    }
    uint32_t slot = (uint32_t)(hash % slots), empty = UINT32_MAX;
    for (uint32_t i = 0; i < slots && empty == UINT32_MAX; i++, slot = slot + 1 == slots ? 0 : slot + 1)
    {
        uint64_t key;
        if (SafeRead(arrays[0] + (uintptr_t)slot * 16, &key, 8)) return;
        if (key == hash) { refused = 1; ApiLog(g_difficulty.mod, "difficulty %s: the countdown delta's name is taken; missions keep their own countdown", g_difficulty.name); return; }
        if (key == 0) empty = slot;
    }
    if (empty == UINT32_MAX) { refused = 1; ApiLog(g_difficulty.mod, "difficulty %s: the entity delta table is full; missions keep their own countdown", g_difficulty.name); return; }
    size_t bytes[4] = {((size_t)deltas + 1) * 8, ((size_t)components + 1) * 12, ((size_t)members + 1) * 12, (size_t)at + 4}, total = 0;
    uint8_t* moved[4];
    for (int i = 0; i < 4; i++) { moved[i] = (uint8_t*)total; total += (bytes[i] + 15) & ~(size_t)15; }
    uint8_t* block = (uint8_t*)VirtualAlloc(NULL, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (block == NULL) return;
    for (int i = 0; i < 4; i++) moved[i] = block + (size_t)moved[i];
    if (SafeRead(arrays[2], moved[0], (size_t)deltas * 8) || SafeRead(arrays[4], moved[1], (size_t)components * 12) ||
        SafeRead(arrays[6], moved[2], (size_t)members * 12) || SafeRead(arrays[8], moved[3], data))
    {
        VirtualFree(block, 0, MEM_RELEASE);
        return;
    }
    float seconds = g_difficulty.minutes * 60.0f;
    uint32_t delta[2] = {1, components}, component[3] = {DIF_HIVEMIND, members, 1}, member[3] = {0x74, 4, at};
    memcpy(moved[0] + (size_t)deltas * 8, delta, sizeof delta);
    memcpy(moved[1] + (size_t)components * 12, component, sizeof component);
    memcpy(moved[2] + (size_t)members * 12, member, sizeof member);
    memcpy(moved[3] + at, &seconds, 4);
    DWORD protect;
    VirtualProtect(block, total, PAGE_READONLY, &protect);
    uint32_t counts[4] = {deltas + 1, components + 1, members + 1, at + 4};
    for (int i = 3; i >= 0; i--)
    {
        uint64_t pair[2] = {(uint64_t)(uintptr_t)moved[i], counts[i]};
        if (ApiWrite(table + 16 + (uintptr_t)i * 16, pair, sizeof pair)) { refused = 1; Note("difficulty: the entity delta table could not be extended"); return; }
    }
    uint64_t index = deltas;
    if (ApiWrite(arrays[0] + (uintptr_t)empty * 16 + 8, &index, 8) || ApiWrite(arrays[0] + (uintptr_t)empty * 16, &hash, 8))
    {
        refused = 1;
        Note("difficulty: the countdown delta could not be named");
        return;
    }
    g_deltaTable = table;
    g_deltaArrays[0] = (uintptr_t)arrays[0];
    for (int i = 0; i < 4; i++) g_deltaArrays[i + 1] = (uintptr_t)moved[i];
    g_deltaAdded[1] = g_deltaAdded[2] = g_deltaAdded[3] = 1;
    g_deltaAdded[4] = at + 4 - data;
    watch[0x40] = 1;
    *(uint8_t* volatile*)(watch + 0x58) = moved[3] + at;
    if (!patched)
    {
        uint8_t jump[12] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        int32_t rel = (int32_t)((uintptr_t)launchStub - (game + DIF_LAUNCH_SITE + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + DIF_LAUNCH_SITE, jump, sizeof jump))
        {
            refused = 1;
            watch[0x40] = 0;
            ApiLog(g_difficulty.mod, "difficulty %s: the mission launch could not be changed; missions keep their own countdown", g_difficulty.name);
            return;
        }
        patched = 1;
    }
    Note("difficulty: countdown delta %016llx registered (delta %u: HiveMind +0x74 = %.0f s; table 0x%llx, hash slot %u of %u)",
        (unsigned long long)hash, deltas, (double)seconds, (unsigned long long)table, empty, slots);
}

/* The countdown's network definition: its maximum becomes the level's seconds + 60 once the network config is up
 * (polled; a reloaded config is widened again). */
static void DifficultyNetwork(void)
{
    static uintptr_t widened;
    uintptr_t rows;
    uint32_t count;
    if (g_difficultyState != 1 || g_difficulty.minutes * 60.0f <= 2500.0f || FieldTypes(&rows, &count)) return;
    for (uint32_t i = 0; i < count; i++)
    {
        uint8_t def[24];
        if (SafeRead(rows + (uintptr_t)i * 24, def, sizeof def)) return;
        uint32_t name; float maximum;
        memcpy(&name, def, 4); memcpy(&maximum, def + 0x14, 4);
        if (name != DIF_COUNTDOWN_DEF || def[0xC] != 2) continue;
        if (rows + (uintptr_t)i * 24 == widened && maximum != 2500.0f) return;
        float wider = g_difficulty.minutes * 60.0f + 60.0f, step = wider / 131072.0f;
        if (maximum != 2500.0f && maximum != wider) { Note("difficulty: the countdown's network definition has an unexpected range (max %.1f); left as it is", (double)maximum); widened = rows + (uintptr_t)i * 24; return; }
        if (ApiWrite(rows + (uintptr_t)i * 24 + 0x14, &wider, 4) || ApiWrite(rows + (uintptr_t)i * 24 + 8, &step, 4)) return;
        widened = rows + (uintptr_t)i * 24;
        Note("difficulty: the countdown's network definition (%u) goes to %.0f s for %.0f-minute missions", i, (double)wider, (double)g_difficulty.minutes);
        return;
    }
}

static uint32_t ApiAddDifficulty(HfMod* mod, const HfDifficulty* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfDifficulty, spawner_cooldown) + sizeof def->spawner_cooldown)
    {
        ApiLog(mod, "add_difficulty: no difficulty, or its size is not set (HfDifficulty d = {sizeof d})");
        return 0;
    }
    char identity[256];
    const char* name = Identity(mod, def->name, identity, sizeof identity);
    if (name == NULL || name[0] == 0 || strlen(name) >= sizeof g_difficulty.name) { ApiLog(mod, "add_difficulty: it needs a name (under 192 bytes)"); return 0; }
    if (g_difficultyState != 0 || g_fired[HF_EVENT_GAME_LOADED]) { ApiLog(mod, "add_difficulty(%s): too late; call it from hellforge_mod_init", name); return 0; }
    if (g_difficultyAdded) { ApiLog(mod, "add_difficulty(%s): %s added level 11 already; the game has room for one more level", name, g_difficulty.mod->id); return 0; }
    if (def->title == NULL || def->title[0] == 0) { ApiLog(mod, "add_difficulty(%s): it needs a title", name); return 0; }
    if (!(def->encounter_points >= 0.0f && def->encounter_points <= 100000.0f) || !(def->guardforce_points >= 0.0f && def->guardforce_points <= 100000.0f) ||
        !(def->spawner_cooldown >= 0.0f && def->spawner_cooldown <= 60.0f))
    {
        ApiLog(mod, "add_difficulty(%s): points are 0 to 100000, the cooldown 0 to 60 seconds (0 keeps level 10's)", name);
        return 0;
    }
    Difficulty d = {0};
    d.mod = mod;
    snprintf(d.name, sizeof d.name, "%s", name);
    char key[256];
    snprintf(key, sizeof key, "%s#item", name);
    d.item = (uint32_t)(Murmur64A(key, strlen(key)) >> 32);
    for (uint32_t i = 0; i < DIF_VANILLA; i++) if (d.item == kDifficultyItems[i]) d.item = 0;
    if (d.item == 0) { ApiLog(mod, "add_difficulty(%s): its name hashes to an id in use; choose another", name); return 0; }
    snprintf(key, sizeof key, "%s#text", name);
    char upper[192]; /* the game's difficulty names are capitals ("SUPER HELLDIVE") */
    size_t n = 0;
    for (; def->title[n] && n + 1 < sizeof upper; n++) upper[n] = (def->title[n] >= 'a' && def->title[n] <= 'z') ? (char)(def->title[n] - 32) : def->title[n];
    upper[n] = 0;
    d.text[0] = AddText(upper, key, 0x21);
    d.text[1] = AddText("11", key, 0x22); /* the mission's top bar shows the level's number, as level 10's "10" */
    d.encounter = def->encounter_points;
    d.guardforce = def->guardforce_points;
    d.cooldown = def->spawner_cooldown;
    if (def->size >= offsetof(HfDifficulty, icon) + sizeof def->icon && def->icon != NULL && def->icon[0])
        d.icon = Murmur64A(def->icon, strlen(def->icon));
    if (def->size >= offsetof(HfDifficulty, outpost_scale) + sizeof def->outpost_scale)
    {
        if (!(def->map_scale >= 0.0f && def->map_scale <= DIF_MAP_SCALE_MAX) || !(def->outpost_scale >= 0.0f && def->outpost_scale <= 8.0f) ||
            def->optional_objectives > 32 || def->major_objectives > 8)
        {
            ApiLog(mod, "add_difficulty(%s): map_scale is 0 to 2, outpost_scale 0 to 8, optional_objectives 0 to 32, "
                "major_objectives 0 to 8 (0 keeps level 10's)", name);
            return 0;
        }
        if (def->optional_objectives + def->major_objectives > DIF_OBJECTIVES_MAX - DIF_OBJECTIVES_OTHER)
        {
            ApiLog(mod, "add_difficulty(%s): optional_objectives + major_objectives is at most %u (the map generator places %u objectives, "
                "%u of them the mission's own)", name, DIF_OBJECTIVES_MAX - DIF_OBJECTIVES_OTHER, DIF_OBJECTIVES_MAX, DIF_OBJECTIVES_OTHER);
            return 0;
        }
        d.mapScale = def->map_scale;
        d.outpostScale = def->outpost_scale;
        d.optional = def->optional_objectives;
        d.major = def->major_objectives;
    }
    if (def->size >= offsetof(HfDifficulty, mission_minutes) + sizeof def->mission_minutes)
    {
        if (!(def->mission_minutes == 0.0f || (def->mission_minutes >= 1.0f && def->mission_minutes <= 75.0f)))
        {
            ApiLog(mod, "add_difficulty(%s): mission_minutes is 1 to 75 (0 keeps the mission's own)", name);
            return 0;
        }
        d.minutes = def->mission_minutes;
    }
    if (def->size >= offsetof(HfDifficulty, flags) + sizeof def->flags)
    {
        uint32_t known = HF_DIFFICULTY_SECOND_PRIMARY | HF_DIFFICULTY_NO_ERADICATE | HF_DIFFICULTY_NO_RAPID_ACQUISITION | HF_DIFFICULTY_RADAR |
            HF_DIFFICULTY_NO_BLITZ | HF_DIFFICULTY_CITY_STRONGHOLDS | HF_DIFFICULTY_CITY_ICBM;
        if (def->flags & ~known)
        {
            ApiLog(mod, "add_difficulty(%s): unknown flags 0x%x", name, def->flags & ~known);
            return 0;
        }
        d.flags = def->flags;
    }
    if (def->size >= offsetof(HfDifficulty, patrol_cap) + sizeof def->patrol_cap)
    {
        if (!(def->patrol_frequency >= 0.0f && def->patrol_frequency <= 4.0f) || !(def->patrol_cap >= 0.0f && def->patrol_cap <= 4.0f))
        {
            ApiLog(mod, "add_difficulty(%s): patrol_frequency and patrol_cap are 0 to 4 (0 keeps level 10's)", name);
            return 0;
        }
        d.patrolFrequency = def->patrol_frequency;
        d.patrolCap = def->patrol_cap;
    }
    if (def->size >= offsetof(HfDifficulty, mission_stratagem) + sizeof def->mission_stratagem)
    {
        if (def->mission_stratagem > 0xFFFF)
        {
            ApiLog(mod, "add_difficulty(%s): mission_stratagem is a stratagem type (0 none)", name);
            return 0;
        }
        d.grant = def->mission_stratagem;
    }
    g_difficulty = d;
    g_difficultyAdded = 1;
    ApiLog(mod, "adding difficulty %s (\"%s\") as level 11 (unlock item %08x): a copy of level 10", d.name, def->title, d.item);
    WriteRegisteredItems();
    return DIF_VANILLA + 1;
}

/* The difficulty's entry in registered-items.json: its unlock item, a copy of level 10's (RegisteredItems.cs adds it to
 * the catalogue, and to the inventory of a player who has unlocked level 10). */
static int DifficultyItems(char* text, int n, int size, int first)
{
    if (!g_difficultyAdded || n >= size - 512) return n;
    return n + snprintf(text + n, size - n, "%s{\"kind\":\"difficulty\",\"mixId\":%u,\"copyOf\":%u,\"level\":%u,\"mod\":\"%s\",\"name\":\"%s\"}",
        first ? "" : ",", g_difficulty.item, kDifficultyItems[DIF_VANILLA - 1], DIF_VANILLA + 1, g_difficulty.mod->id, g_difficulty.name);
}

static uintptr_t ApiDifficultyRow(uint32_t level)
{
    return g_difficultyState == 1 && g_difficultyTable != NULL && level >= 1 && level <= DIF_VANILLA + 1 ?
        (uintptr_t)(g_difficultyTable + (level - 1) * DIF_ROW) : 0;
}

/* The generation block grows (locations, minimap, stamps) for larger maps and for more objectives. */
static int DifficultyNeedsBlock(void)
{
    return g_difficulty.mapScale > 1.0f || g_difficulty.optional != 0 || g_difficulty.major != 0 ||
        (g_difficulty.flags & (HF_DIFFICULTY_SECOND_PRIMARY | HF_DIFFICULTY_RADAR));
}

static int32_t DifficultyDisplacement(uintptr_t table, uintptr_t game, const DifficultySite* s)
{
    uintptr_t target = table + s->field;
    return s->rip ? (int32_t)(int64_t)(target - (game + s->rva + s->size)) : (int32_t)(int64_t)(target - game);
}

/* NULL when every copy, instruction, bound and kept caller is the reviewed one; 1 in *waiting while the tables are
 * not filled yet. */
static const char* DifficultySitesFailed(uintptr_t game, int* waiting)
{
    static uint8_t first[DIF_VANILLA * DIF_ROW], other[DIF_VANILLA * DIF_ROW];
    *waiting = 0;
    if (SafeRead(game + kDifficultyCopies[0], first, sizeof first)) return "the difficulty table is unreadable";
    for (uint32_t i = 0; i < DIF_VANILLA; i++)
    {
        uint32_t item, level;
        memcpy(&item, first + i * DIF_ROW, 4);
        memcpy(&level, first + i * DIF_ROW + 4, 4);
        if (item == 0) { *waiting = 1; return "the difficulty table is not filled yet"; }
        if (item != kDifficultyItems[i] || level != i + 1) return "the difficulty table is not the reviewed one";
    }
    for (size_t c = 1; c < sizeof kDifficultyCopies / sizeof kDifficultyCopies[0]; c++)
        if (SafeRead(game + kDifficultyCopies[c], other, sizeof other) || memcmp(first, other, sizeof first) != 0)
        {
            uint32_t item = 0;
            memcpy(&item, other, 4);
            if (item == 0) { *waiting = 1; return "a copy of the difficulty table is not filled yet"; }
            return "the copies of the difficulty table differ";
        }
    for (size_t i = 0; i < sizeof kDifficultySites / sizeof kDifficultySites[0]; i++)
    {
        const DifficultySite* s = &kDifficultySites[i];
        int32_t disp;
        if (SafeRead(game + s->rva + s->dispAt, &disp, 4) || disp != DifficultyDisplacement(game + kDifficultyCopies[s->copy], game, s))
            return "an instruction addressing the difficulty table is not the reviewed one";
    }
    for (size_t i = 0; i < sizeof kDifficultyBounds / sizeof kDifficultyBounds[0]; i++)
    {
        uint32_t value = 0;
        if (SafeRead(game + kDifficultyBounds[i].rva + kDifficultyBounds[i].immAt, &value, kDifficultyBounds[i].width) || value != DIF_VANILLA)
            return "a bound on the difficulty levels is not the reviewed one";
    }
    for (size_t i = 0; i < sizeof kDifficultyKeepTen / sizeof kDifficultyKeepTen[0]; i++)
    {
        uint8_t call[5];
        int32_t rel;
        if (SafeRead(game + kDifficultyKeepTen[i], call, 5)) return "a caller of the highest level is unreadable";
        memcpy(&rel, call + 1, 4);
        if (call[0] != 0xE8 || kDifficultyKeepTen[i] + 5 + rel != DIF_MAX_FN) return "a caller of the highest level is not the reviewed one";
    }
    uint8_t band[sizeof kBandSitePin];
    if (SafeRead(game + DIF_BAND_SITE, band, sizeof band) || memcmp(band, kBandSitePin, sizeof band) != 0)
        return "the band sprites are not the reviewed ones";
    for (int i = 0; i < DIF_LEVEL_LOADS; i++)
    {
        uint8_t load[8];
        if (SafeRead(game + kLevelLoads[i].rva, load, kLevelLoads[i].length) || memcmp(load, kLevelLoads[i].pin, kLevelLoads[i].length) != 0)
            return "a per-level value's reader is not the reviewed one";
    }
    for (int i = 0; i < 5; i++)
    {
        uint8_t bytes[16];
        if (SafeRead(game + kLevelInPlace[i].rva, bytes, kLevelInPlace[i].length) || memcmp(bytes, kLevelInPlace[i].pin, kLevelInPlace[i].length) != 0)
            return "a per-level value's reader is not the reviewed one";
    }
    {
        uint8_t sib = 0;
        if (SafeRead(game + DIF_HUD_TARGET_SIB, &sib, 1) || sib != 0x8D) return "the HUD's objective target is not the reviewed one";
    }
    {
        uint8_t board[7];
        if (SafeRead(game + DIF_BOARD_REGION_MAX, board, 5) || memcmp(board, kBoardRegionMaxPin, 5) != 0 ||
            SafeRead(game + DIF_BOARD_REGION_SITE, board, 6) || memcmp(board, kBoardRegionPin, 6) != 0 ||
            SafeRead(game + DIF_BOARD_PLANET_SITE, board, 7) || memcmp(board, kBoardPlanetPin, 7) != 0 ||
            SafeRead(game + DIF_BOARD_DEFENCE_SITE, board, 7) || memcmp(board, kBoardPlanetPin, 7) != 0)
            return "the war table's mission board is not the reviewed one";
        if (SafeRead(game + DIF_SPREAD_REFERENCE, board, 7) || memcmp(board, kSpreadReferencePin, 7) != 0)
            return "the map generator's location spreading is not the reviewed one";
    }
    uint8_t iconCall[5];
    int32_t iconRel;
    if (SafeRead(game + DIF_ICON_CALL, iconCall, 5)) return "the difficulty icon setter is unreadable";
    memcpy(&iconRel, iconCall + 1, 4);
    if (iconCall[0] != 0xE8 || DIF_ICON_CALL + 5 + iconRel != DIF_SET_SPRITE) return "the difficulty icon setter is not the reviewed one";
    {
        uint8_t icon[29];
        if (SafeRead(game + DIF_HUD_ICON_READ, icon, 29) || memcmp(icon, kHudIconPin, 29) != 0) return "the mission HUD's difficulty icon is not the reviewed one";
        for (int i = 0; i < 3; i++)
            if (SafeRead(game + kIconSites[i].read, icon, 20) || memcmp(icon, kIconSites[i].pin, 20) != 0) return "a difficulty icon reader is not the reviewed one";
    }
    uint32_t operationsToTen = 0, missionsToTen = 0;
    for (uint32_t i = 0; i < DIF_OPERATION_ROWS; i++)
    {
        uint32_t range[2];
        if (SafeRead(game + DIF_OPERATIONS + i * DIF_OPERATION_ROW + 0xC, range, 8) || range[0] < 1 || range[1] > DIF_VANILLA || range[0] > range[1])
            return "the operation settings are not the reviewed ones";
        operationsToTen += range[1] == DIF_VANILLA;
    }
    for (uint32_t i = 0; i < DIF_MISSION_ROWS; i++)
    {
        uint32_t range[2];
        if (SafeRead(game + DIF_MISSIONS + i * DIF_MISSION_ROW + 0xC, range, 8) || range[0] < 1 || range[0] > range[1])
            return "the missions are not the reviewed ones";
        missionsToTen += range[1] == DIF_VANILLA;
    }
    if (operationsToTen != 24 || missionsToTen != 120) return "the operations' and missions' difficulty ranges are not the reviewed ones";
    uint32_t objectivesToTen = 0;
    for (uint32_t i = 0; i < DIF_OBJECTIVE_ROWS; i++)
    {
        uint32_t highest = 0;
        if (SafeRead(game + DIF_OBJECTIVES + i * DIF_OBJECTIVE_ROW + 0x28, &highest, 4)) return "the objectives are unreadable";
        objectivesToTen += highest == DIF_VANILLA;
    }
    if (objectivesToTen != 148) return "the objectives' difficulty ranges are not the reviewed ones";
    for (size_t i = 0; i < 7; i++)
    {
        int32_t disp = 0;
        if (SafeRead(game + kDistanceSites[i][0] + kDistanceSites[i][1], &disp, 4) || disp != (int32_t)DIF_DISTANCE)
            return "a mission distance read is not the reviewed one";
    }
    for (size_t i = 0; i < 6; i++)
    {
        uint8_t disp = 0;
        if (SafeRead(game + kWeightReaders[i][0] + kWeightReaders[i][1], &disp, 1) || disp != 0x30) return "an enemy weight read is not the reviewed one";
    }
    uint8_t clamp[5];
    if (SafeRead(game + DIF_TECH_CLAMP, clamp, 5) || memcmp(clamp, kKeepTen, 5) != 0) return "the enemy tech level clamp is not the reviewed one";
    uint32_t compositionsToTen = 0;
    for (size_t r = 0; r < DIF_ROSTERS; r++)
    {
        uint64_t rows = 0, count = 0;
        if (SafeRead(game + kRosters[r].descriptor, &rows, 8) || SafeRead(game + kRosters[r].descriptor + 8, &count, 8) ||
            rows == 0 || count != kRosters[r].rows)
            return "an enemy roster is not the reviewed one";
        for (uint64_t i = 0; i < count; i++)
        {
            uint64_t spare = 1;
            if (SafeRead(rows + i * 0x80 + 0x28, &spare, 8) || spare != 0) return "an enemy roster row has no room for an 11th weight";
        }
        for (uint32_t k = 0; k < 8; k++)
        {
            uint64_t family = 0, families = 0;
            if (SafeRead(game + kRosters[r].descriptor + 0x20 + 0x10 * k, &family, 8) || SafeRead(game + kRosters[r].descriptor + 0x28 + 0x10 * k, &families, 8))
                return "an enemy composition family is unreadable";
            for (uint64_t i = 0; family != 0 && i < families; i++)
            {
                uint32_t highest = 0;
                if (SafeRead(family + i * 0x118 + 0xF0, &highest, 4)) return "an enemy composition is unreadable";
                compositionsToTen += highest == DIF_VANILLA;
            }
        }
    }
    if (compositionsToTen != DIF_COMPOSITIONS_UP_TO_TEN) return "the enemy compositions' tech ranges are not the reviewed ones";
    for (size_t i = 0; i < sizeof kBarSites / sizeof kBarSites[0]; i++)
    {
        uint8_t bytes[32];
        if (SafeRead(game + kBarSites[i].rva, bytes, kBarSites[i].length) || memcmp(bytes, kBarSites[i].original, kBarSites[i].length) != 0)
            return "the difficulty bar is not the reviewed one";
    }
    if (g_difficulty.mapScale > 1.0f)
    {
        uint8_t pin[9];
        if (SafeRead(game + DIF_MAP_TYPE_SITE, pin, 7) || memcmp(pin, kMapTypePin, 7) != 0 ||
            SafeRead(game + DIF_SPACING_SITE, pin, 5) || memcmp(pin, kSpacingPin, 5) != 0 ||
            SafeRead(game + DIF_SPACING_SITE_2, pin, 5) || memcmp(pin, kSpacingPin2, 5) != 0 ||
            SafeRead(game + DIF_GRID_SITE, pin, 9) || memcmp(pin, kGridPin, 9) != 0 ||
            SafeRead(game + DIF_CELL_SPACING_SITE, pin, 7) || memcmp(pin, kCellSpacingPin, 7) != 0 ||
            SafeRead(game + DIF_FINALIZE_SPACING_SITE, pin, 9) || memcmp(pin, kFinalizeSpacingPin, 9) != 0)
            return "the map generator is not the reviewed one (a map_scale above 1 needs it)";
        uint8_t zone[21];
        if (SafeRead(game + DIF_ZONE_SITE, zone, 14) || memcmp(zone, kZonePin, 14) != 0 ||
            SafeRead(game + DIF_ZONE_CLEAR, zone, 21) || memcmp(zone, kZoneClearPin, 21) != 0 ||
            SafeRead(game + DIF_ZONE_TABLE_LEA, zone, 8) || memcmp(zone, kZoneTableLeaPin, 8) != 0 ||
            SafeRead(game + DIF_ZONE_HEIGHT, zone, 7) || memcmp(zone, kZoneHeightPin, 7) != 0)
            return "the map generator is not the reviewed one (a map_scale above 1 needs it)";
        for (int i = 0; i < 7; i++)
            if (SafeRead(game + kCityCapacities[i].rva, zone, kCityCapacities[i].length) || memcmp(zone, kCityCapacities[i].pin, kCityCapacities[i].length) != 0)
                return "the city generator is not the reviewed one (a map_scale above 1 needs it)";
        if (SafeRead(game + DIF_FOOTPRINT_CHECK, zone, 9) || memcmp(zone, kFootprintPin, 9) != 0)
            return "the city generator is not the reviewed one (a map_scale above 1 needs it)";
        for (int i = 0; i < 3; i++)
            if (SafeRead(game + kReVoronoiStubs[i].site, zone, kReVoronoiStubs[i].length) || memcmp(zone, kReVoronoiStubs[i].pin, kReVoronoiStubs[i].length) != 0)
                return "the structured map generator is not the reviewed one (a map_scale above 1 needs it)";
        for (int i = 0; i < 8; i++)
            if (SafeRead(game + kBucketPool[i].rva, zone, kBucketPool[i].length) || memcmp(zone, kBucketPool[i].pin, kBucketPool[i].length) != 0)
                return "the structured map generator is not the reviewed one (a map_scale above 1 needs it)";
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
        {
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
        }
        if (SafeRead(game + DIF_PREVIEW_RECT_SITE, zone, 8) || memcmp(zone, kPreviewRectPin, 8) != 0 ||
            SafeRead(game + DIF_CONVOY_SITE, zone, 12) || memcmp(zone, kConvoyPin, 12) != 0)
            return "the war-table preview's view or convoy route is not the reviewed one (a map_scale above 1 needs it)";
        for (int i = 0; i < 5; i++)
            if (SafeRead(game + kStampIndex[i].rva, pin, kStampIndex[i].length) || memcmp(pin, kStampIndex[i].pin, kStampIndex[i].length) != 0)
                return "the map generator's stamp index is not the reviewed one (a map_scale above 1 needs it)";
    }
    if (DifficultyNeedsBlock())
    {
        const struct { const LocationSite* sites; size_t count; } groups[12] = {{kLocationRefs, sizeof kLocationRefs / sizeof kLocationRefs[0]},
            {kLocationCaps, sizeof kLocationCaps / sizeof kLocationCaps[0]}, {kGenerationBlockSize, sizeof kGenerationBlockSize / sizeof kGenerationBlockSize[0]},
            {kLocationTypeRefs, sizeof kLocationTypeRefs / sizeof kLocationTypeRefs[0]}, {kLocationTypeCaps, sizeof kLocationTypeCaps / sizeof kLocationTypeCaps[0]},
            {kMinimapRefs, sizeof kMinimapRefs / sizeof kMinimapRefs[0]}, {kMinimapCaps, sizeof kMinimapCaps / sizeof kMinimapCaps[0]},
            {kStampRefs, sizeof kStampRefs / sizeof kStampRefs[0]}, {kPlaceListRefs, sizeof kPlaceListRefs / sizeof kPlaceListRefs[0]},
            {kPlaceSetRefs, sizeof kPlaceSetRefs / sizeof kPlaceSetRefs[0]}, {kClusterRefs, 16}, {kClusterCaps, 16}};
        for (int g = 0; g < 12; g++)
            for (size_t i = 0; i < groups[g].count; i++)
            {
                uint32_t value = 0;
                if (SafeRead(game + groups[g].sites[i].rva + groups[g].sites[i].at, &value, 4) || value != groups[g].sites[i].value)
                    return "the map generator's locations or mission stamps are not the reviewed ones (larger maps and more objectives need them)";
            }
        for (int i = 0; i < 74; i++)
        {
            uint8_t bytes[8];
            if (SafeRead(game + kObjectiveListSites[i].rva, bytes, kObjectiveListSites[i].length) ||
                memcmp(bytes, kObjectiveListSites[i].pin, kObjectiveListSites[i].length) != 0)
                return "the map generator's objective list is not the reviewed one (more objectives need it)";
        }
        uint8_t bytes[11];
        if (SafeRead(game + DIF_TELEMETRY_COUNT_SITE, bytes, 10) || memcmp(bytes, kTelemetryCountPin, 10) != 0 ||
            SafeRead(game + DIF_TELEMETRY_LOOP_SITE, bytes, 11) || memcmp(bytes, kTelemetryLoopPin, 11) != 0 ||
            SafeRead(game + DIF_SELECTION_END, bytes, 7) || memcmp(bytes, kSelectionPin, 7) != 0)
            return "the mission-end telemetry or the objective selection is not the reviewed one (more objectives need them)";
        for (int i = 0; i < 3; i++)
            if (SafeRead(game + kHudListSites[i].site, bytes, kHudListSites[i].length) || memcmp(bytes, kHudListSites[i].pin, kHudListSites[i].length) != 0)
                return "the HUD's objective list is not the reviewed one (more objectives need it)";
        if (SafeRead(game + DIF_BADGE_SITE, bytes, 7) || memcmp(bytes, kBadgePin, 7) != 0)
            return "the mission result's badges are not the reviewed ones (more objectives need them)";
    }
    for (int i = 0; i < 4; i++)
    {
        uint8_t pin[6];
        if (SafeRead(game + kRestartSites[i], pin, 6) || memcmp(pin, kRestartPin, 6) != 0) return "a level generation restart is not the reviewed one";
    }
    {
        uint8_t pin[11];
        if (SafeRead(game + DIF_CAPTURE_SITE, pin, 11) || memcmp(pin, kCapturePin, 11) != 0) return "the minimap capture is not the reviewed one";
        if (SafeRead(game + DIF_TELEMETRY_SITE, pin, 5) || memcmp(pin, kTelemetryPin, 5) != 0) return "the selected mission's telemetry is not the reviewed one";
        if (SafeRead(game + DIF_SPAWNER_SITE, pin, 6) || memcmp(pin, kSpawnerPin, 6) != 0) return "the director's spawner list is not the reviewed one";
        if (SafeRead(game + DIF_MARKER_LOOP_SITE, pin, 8) || memcmp(pin, kMarkerLoopPin, 8) != 0 ||
            SafeRead(game + DIF_MARKER_ROUTE_SITE, pin, 6) || memcmp(pin, kMarkerRoutePin, 6) != 0)
            return "the map's marker slots are not the reviewed ones";
        if (g_difficulty.minutes > 0.0f && (SafeRead(game + DIF_TIMER_SITE, pin, 5) || memcmp(pin, kTimerPin, 5) != 0)) return "the mission countdown is not the reviewed one";
        uint8_t launch[12];
        if (g_difficulty.minutes > 0.0f && (SafeRead(game + DIF_LAUNCH_SITE, launch, 12) || memcmp(launch, kLaunchPin, 12) != 0)) return "the mission launch's delta list is not the reviewed one";
        uint8_t times[13];
        if (g_difficulty.minutes > 0.0f && (SafeRead(game + DIF_CARD_TIME_SITE, times, 8) || memcmp(times, kCardTimePin, 8) != 0)) return "the mission card's duration is not the reviewed one";
        if (g_difficulty.minutes > 0.0f && (SafeRead(game + DIF_WIDGET_TIME_SITE, times, 13) || memcmp(times, kWidgetTimePin, 13) != 0)) return "the mission timer widget is not the reviewed one";
        if (g_difficulty.mapScale > 1.0f)
        {
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
        }
        uint8_t view[9];
        if (SafeRead(game + DIF_MAPVIEW_MIN, view, 9) || memcmp(view, kMapViewMinPin, 9) != 0 ||
            SafeRead(game + DIF_MAPVIEW_CAP, view, 9) || memcmp(view, kMapViewCapPin, 9) != 0) return "the war table's map view is not the reviewed one";
    }
    return NULL;
}

static int RefuseDifficulty(const char* why)
{
    g_difficultyState = -1;
    Note("difficulty: level 11 refused: %s", why);
    if (g_difficultyAdded) ApiLog(g_difficulty.mod, "difficulty %s not added: %s", g_difficulty.name, why);
    return 1;
}

/* Polled after GAME_LOADED until it returns 1: moves the table and adds level 11. pinsReady: the grace period for code
 * still being decoded is over. */
static int InstallDifficulty(uintptr_t game, int pinsReady)
{
    if (g_difficultyState != 0) return 1;
    if (!g_difficultyAdded) { g_difficultyState = 1; return 1; }
    int waiting;
    const char* why = DifficultySitesFailed(game, &waiting);
    if (why != NULL) return waiting || !pinsReady ? 0 : RefuseDifficulty(why);
    /* The rows and the band sprites; the bars' 11th widgets (zeroed, as the bar's own memory is when it is built) and
     * their table; a page of stubs. */
    size_t bytes = (size_t)DIF_ROWS * DIF_ROW + (DIF_VANILLA + 1) * 8 + (DIF_VANILLA + 1) * 12;
    size_t widgetBytes = (size_t)DIF_BARS * 8 + (size_t)DIF_BARS * DIF_WIDGET;
    uint8_t* table = AllocateNearGame(game, bytes + 0x60);
    uint8_t* widgets = table != NULL ? AllocateNearGame(game, widgetBytes) : NULL;
    uint8_t* code = widgets != NULL ? AllocateNearGame(game, 0x1000) : NULL;
    if (table == NULL || widgets == NULL || code == NULL) return RefuseDifficulty("no memory within 2 GB of game.dll for the moved table");
    memset(table, 0, bytes + 0x60);
    uint8_t* watch = table + bytes;
    memset(widgets, 0, widgetBytes);
    uint8_t* bands = table + DIF_ROWS * DIF_ROW;
    if (SafeRead(game + DIF_BANDS, bands, DIF_VANILLA * 8)) return RefuseDifficulty("the band sprites are unreadable");
    memcpy(bands + DIF_VANILLA * 8, bands + (DIF_VANILLA - 1) * 8, 8);
    uint8_t* distance = bands + (DIF_VANILLA + 1) * 8;
    if (SafeRead(game + DIF_DISTANCE, distance, DIF_VANILLA * 12)) return RefuseDifficulty("the mission distance factors are unreadable");
    memcpy(distance + DIF_VANILLA * 12, distance + (DIF_VANILLA - 1) * 12, 12);
    for (int k = 0; g_difficulty.mapScale > 1.0f && k < 3; k++) /* the structured maps' playable radius, per sub-kind */
    {
        float factor;
        memcpy(&factor, distance + DIF_VANILLA * 12 + 4 * k, 4);
        factor *= g_difficulty.mapScale;
        memcpy(distance + DIF_VANILLA * 12 + 4 * k, &factor, 4);
    }
    uint8_t* extra = code;
    memcpy(extra, kExtraWidget, sizeof kExtraWidget);
    int32_t rel = (int32_t)(widgets - (extra + 10));
    memcpy(extra + 6, &rel, 4);
    rel = (int32_t)(widgets + DIF_BARS * 8 - (extra + 56));
    memcpy(extra + 52, &rel, 4);
    /* The icon setter, for level 11 (esi): Super Helldive's sprite through the game's setter first (it gives the image
     * element its resource, +0x148, which a texture needs), then the mod's texture in the image slot, the whole texture
     * as the UV rect (+0x134; the sprite's would crop it) and the element refreshed, as 0x1450160 does for a texture.
     * Other levels: the game's setter (a tail jump: the caller's return address stays). */
    uint8_t* iconStub = code + 0x40;
    memcpy(iconStub, kIconStub, sizeof kIconStub);
    uint64_t superHelldive = 0;
    if (SafeRead(game + kDifficultyCopies[0] + (DIF_VANILLA - 1) * DIF_ROW + 0x10, &superHelldive, 8) || superHelldive == 0)
        return RefuseDifficulty("Super Helldive's icon is unreadable");
    memcpy(iconStub + 20, &g_difficulty.icon, 8);
    memcpy(iconStub + 30, &superHelldive, 8);
    rel = (int32_t)(game + DIF_SET_SPRITE - (uintptr_t)(iconStub + 9)); memcpy(iconStub + 5, &rel, 4);
    rel = (int32_t)(game + DIF_SET_SPRITE - (uintptr_t)(iconStub + 46)); memcpy(iconStub + 42, &rel, 4);
    rel = (int32_t)(game + DIF_SET_SLOT - (uintptr_t)(iconStub + 70)); memcpy(iconStub + 66, &rel, 4);
    rel = (int32_t)(game + DIF_REFRESH_IMAGE - (uintptr_t)(iconStub + 131)); memcpy(iconStub + 127, &rel, 4);
    uint8_t* hudIconStub = code + 0x940; /* kIconStub with the HUD's base sprite (what it passes 0x1450230) */
    {
        uint64_t hudBase;
        memcpy(&hudBase, kHudIconPin + 13, 8);
        memcpy(hudIconStub, iconStub, sizeof kIconStub);
        memcpy(hudIconStub + 30, &hudBase, 8);
        rel = (int32_t)(game + DIF_SET_SPRITE - (uintptr_t)(hudIconStub + 46)); memcpy(hudIconStub + 42, &rel, 4);
        rel = (int32_t)(game + DIF_SET_SLOT - (uintptr_t)(hudIconStub + 70)); memcpy(hudIconStub + 66, &rel, 4);
        rel = (int32_t)(game + DIF_REFRESH_IMAGE - (uintptr_t)(hudIconStub + 131)); memcpy(hudIconStub + 127, &rel, 4);
    }
    uint8_t* iconChecks = code + 0x9D0; /* per reader: cmp the row's level, 11; jne its setter; jmp the icon stub's body */
    {
        uint8_t* c = iconChecks;
        c[0] = 0x83; c[1] = 0x7F; c[2] = 0x04; c[3] = DIF_VANILLA + 1;              /* cmp dword [rdi+4], 11 */
        c[4] = 0x0F; c[5] = 0x85; rel = (int32_t)(game + DIF_SET_IMAGE - (uintptr_t)(c + 10)); memcpy(c + 6, &rel, 4);
        c[10] = 0xE9; rel = (int32_t)(hudIconStub + 9 - (c + 15)); memcpy(c + 11, &rel, 4);
        for (int i = 0; i < 3; i++)
        {
            c = iconChecks + 0x10 * (i + 1);
            memcpy(c, kIconSites[i].check, 5);
            c[5] = 0x0F; c[6] = 0x85; rel = (int32_t)(game + DIF_SET_SPRITE - (uintptr_t)(c + 11)); memcpy(c + 7, &rel, 4);
            c[11] = 0xE9; rel = (int32_t)(iconStub + 9 - (c + 16)); memcpy(c + 12, &rel, 4);
        }
    }
    /* Per site: cmp eax, 10; jae 11th; its instructions; jmp back. 11th: mov rcx, bar; call ExtraWidget; after; jmp back. */
    uint8_t* stubs[sizeof kBarSites / sizeof kBarSites[0]];
    uint8_t* at = code + 0x100;
    for (size_t i = 0; i < sizeof kBarSites / sizeof kBarSites[0]; i++)
    {
        const BarSite* s = &kBarSites[i];
        uintptr_t back = game + s->rva + s->length;
        uint8_t rest = (uint8_t)(s->length - s->keep);
        stubs[i] = at;
        memcpy(at, s->original, s->keep); at += s->keep;
        at[0] = 0x83; at[1] = 0xF8; at[2] = (uint8_t)DIF_VANILLA; at[3] = 0x73; at[4] = (uint8_t)(rest + 5); at += 5;
        memcpy(at, s->original + s->keep, rest); at += rest;
        rel = (int32_t)(back - (uintptr_t)(at + 5)); at[0] = 0xE9; memcpy(at + 1, &rel, 4); at += 5;
        at[0] = 0x48; at[1] = 0x89; at[2] = s->bar; at += 3;
        rel = (int32_t)(extra - (at + 5)); at[0] = 0xE8; memcpy(at + 1, &rel, 4); at += 5;
        memcpy(at, s->after, s->afterLength); at += s->afterLength;
        rel = (int32_t)(back - (uintptr_t)(at + 5)); at[0] = 0xE9; memcpy(at + 1, &rel, 4); at += 5;
        at = (uint8_t*)(((uintptr_t)at + 15) & ~(uintptr_t)15);
    }
    /* A larger map's radii and spacing (m, from the centre): border R = playable + 174 (settings +0x48), fade R + 57 and
     * disk R + 117 as at level 10 (620 / 680), the disk within the world; the spacing keeps level 10's sites,
     * 0.8 R^2 + 0.2 (disk + 1)^2 against 0.8 x 563.12^2 + 0.2 x 681^2 (sites past R are thinned to a fifth). */
    uint8_t* objectiveStubs = code + 0xEE0; /* telemetry count (+0), loop (+0x20), selection (+0x40, 78 bytes) */
    {
        void* function = (void*)MixMission;
        uint8_t* count = objectiveStubs, *loop = objectiveStubs + 0x20, *selection = objectiveStubs + 0x40;
        memcpy(count, kTelemetryCountStub, sizeof kTelemetryCountStub);
        rel = (int32_t)(game + DIF_TELEMETRY_COUNT_SITE + 10 - (uintptr_t)(count + 25)); memcpy(count + 21, &rel, 4);
        memcpy(loop, kTelemetryLoopStub, sizeof kTelemetryLoopStub);
        rel = (int32_t)(game + DIF_TELEMETRY_DONE - (uintptr_t)(loop + 11)); memcpy(loop + 7, &rel, 4);
        rel = (int32_t)(game + DIF_TELEMETRY_LOOP - (uintptr_t)(loop + 20)); memcpy(loop + 16, &rel, 4);
        rel = (int32_t)(game + DIF_TELEMETRY_DONE - (uintptr_t)(loop + 25)); memcpy(loop + 21, &rel, 4);
        memcpy(selection, kSelectionStub, sizeof kSelectionStub);
        uint8_t* badge = objectiveStubs + 0xD0;
        memcpy(badge, kBadgeStub, sizeof kBadgeStub);
        rel = (int32_t)(game + DIF_BADGE_SKIP - (uintptr_t)(badge + 16)); memcpy(badge + 12, &rel, 4);
        rel = (int32_t)(game + DIF_BADGE_NEXT - (uintptr_t)(badge + 21)); memcpy(badge + 17, &rel, 4);
        uint8_t* list = AllocateNearGame(game, DIF_HUD_LIST * 4);
        if (list == NULL) return RefuseDifficulty("no memory within 2 GB of game.dll for the HUD's objective list");
        for (int i = 0; i < 3; i++)
        {
            uint8_t* stub = objectiveStubs + 0xA0 + 0x10 * i;
            stub[0] = 0x4C; stub[1] = 0x8D; stub[2] = 0x1D;                  /* lea r11, [rip+list] */
            rel = (int32_t)(list - (stub + 7)); memcpy(stub + 3, &rel, 4);
            memcpy(stub + 7, kHudListSites[i].access, 4);
            stub[11] = 0xE9;
            rel = (int32_t)(game + kHudListSites[i].site + kHudListSites[i].length - (uintptr_t)(stub + 16)); memcpy(stub + 12, &rel, 4);
        }
        memcpy(selection + 52, &function, 8);
        rel = (int32_t)(game + DIF_SELECTION_BACK - (uintptr_t)(selection + 78)); memcpy(selection + 74, &rel, 4);
    }
    uint8_t* levelStubs = code + 0xCE0; /* 16 x 0x20: the load, the clamp, back */
    for (int i = 0; i < DIF_LEVEL_LOADS; i++)
    {
        uint8_t* stub = levelStubs + 0x20 * i;
        int n = kLevelLoads[i].length;
        memcpy(stub, kLevelLoads[i].pin, n);
        n += LevelClamp(stub + n, kLevelLoads[i].reg);
        stub[n] = 0xE9;
        rel = (int32_t)(game + kLevelLoads[i].rva + kLevelLoads[i].length - (uintptr_t)(stub + n + 5));
        memcpy(stub + n + 1, &rel, 4);
    }
    /* the map's marker slots: cmp [rsp+0x40], 16; jae leave; the replaced instructions; jmp back (0x400 / 0x420) */
    uint8_t* markerStubs = code + 0x400;
    {
        const struct { uint32_t site; const uint8_t* pin; uint8_t length; uint32_t leave; } guards[2] = {
            {DIF_MARKER_LOOP_SITE, kMarkerLoopPin, 8, DIF_MARKER_LOOP_EXIT}, {DIF_MARKER_ROUTE_SITE, kMarkerRoutePin, 6, DIF_MARKER_ROUTE_SKIP}};
        for (int i = 0; i < 2; i++)
        {
            uint8_t* stub = markerStubs + 0x20 * i;
            uint8_t n = guards[i].length;
            stub[0] = 0x83; stub[1] = 0x7C; stub[2] = 0x24; stub[3] = 0x40; stub[4] = (uint8_t)DIF_MARKER_SLOTS;
            stub[5] = 0x0F; stub[6] = 0x83;
            rel = (int32_t)(game + guards[i].leave - (uintptr_t)(stub + 11)); memcpy(stub + 7, &rel, 4);
            memcpy(stub + 11, guards[i].pin, n);
            stub[11 + n] = 0xE9;
            rel = (int32_t)(game + guards[i].site + n - (uintptr_t)(stub + 16 + n)); memcpy(stub + 12 + n, &rel, 4);
        }
    }
    uint8_t* radiiStub = code + 0xC40; /* 116 bytes */
    uint8_t* convoyStub = code + 0xCC0;
    uint8_t* spacingStub = code + 0x460;
    uint8_t* spacingStub2 = code + 0x490;
    float* spacingValue = (float*)(code + 0x4C0);
    float* gridValue = (float*)(code + 0x4C4);
    uint8_t* gridStub = code + 0x4D0;
    float disk = 0.0f;
    if (g_difficulty.mapScale > 1.0f)
    {
        float border = 389.12f * g_difficulty.mapScale + 174.0f;
        disk = border + 117.0f < 1022.0f ? border + 117.0f : 1022.0f;
        float fade = border + 57.0f < disk ? border + 57.0f : disk;
        float radii[5] = {fade / 1024.0f, disk / 1024.0f, 1024.0f - disk - 1.0f, 1024.0f + disk + 1.0f, disk + 1.0f};
        /* In quarter metres: map type 1 lays the route sites as a grid (0xC126C0 cl=1), and a grid of inexact spacing
         * splits its 4-way corners (65.7 m: 1004 vertices for 752 sites, past the 975; 65.75 m: 815). */
        *spacingValue = ceilf(160.0f * sqrtf((0.8f * border * border + 0.2f * (disk + 1.0f) * (disk + 1.0f)) /
            (0.8f * 563.12f * 563.12f + 0.2f * 681.0f * 681.0f))) / 4.0f;
        memcpy(radiiStub, kRadiiStub, sizeof kRadiiStub);
        for (int i = 0; i < 5; i++) memcpy(radiiStub + 37 + 11 * i, &radii[i], 4);
        rel = (int32_t)(watch + 0x20 - (radiiStub + 7)); memcpy(radiiStub + 3, &rel, 4);
        rel = (int32_t)(game + DIF_MAP_TYPE_SITE + 7 - (uintptr_t)(radiiStub + 116)); memcpy(radiiStub + 112, &rel, 4);
        if (g_difficulty.flags & (HF_DIFFICULTY_CITY_STRONGHOLDS | HF_DIFFICULTY_CITY_ICBM | HF_DIFFICULTY_RADAR))
        {
            g_cityCopyFlags = g_difficulty.flags & (HF_DIFFICULTY_CITY_STRONGHOLDS | HF_DIFFICULTY_CITY_ICBM | HF_DIFFICULTY_RADAR);
            uint8_t* strongholds = AllocateNearGame(game, 0x100);
            if (strongholds == NULL) return RefuseDifficulty("no memory within 2 GB of game.dll for the city strongholds");
            uint64_t install = (uint64_t)(uintptr_t)(void*)StrongholdInstall;
            memcpy(strongholds, kStrongholdStub, 125);
            memcpy(strongholds + 60, &install, 8);
            strongholds[125] = 0xE9;                                  /* jmp back after the level init's store */
            rel = (int32_t)(game + DIF_MAP_TYPE_SITE + 7 - (uintptr_t)(strongholds + 130)); memcpy(strongholds + 126, &rel, 4);
            DWORD strongholdProtect;
            VirtualProtect(strongholds, 0x100, PAGE_EXECUTE_READ, &strongholdProtect);
            rel = (int32_t)(strongholds - (radiiStub + 116)); memcpy(radiiStub + 112, &rel, 4); /* the radii stub ends through it */
        }
        memcpy(convoyStub, kConvoyStub, sizeof kConvoyStub);
        rel = (int32_t)(game + DIF_CONVOY_BACK - (uintptr_t)(convoyStub + 27)); memcpy(convoyStub + 23, &rel, 4);
        memcpy(spacingStub, kSpacingStub, sizeof kSpacingStub);
        memcpy(spacingStub + 15, &radii[1], 4);
        rel = (int32_t)((uint8_t*)spacingValue - (spacingStub + 29)); memcpy(spacingStub + 25, &rel, 4);
        rel = (int32_t)(game + DIF_SPACING_SITE + 5 - (uintptr_t)(spacingStub + 34)); memcpy(spacingStub + 30, &rel, 4);
        memcpy(spacingStub2, kSpacingStub2, sizeof kSpacingStub2);
        memcpy(spacingStub2 + 16, &radii[1], 4);
        rel = (int32_t)((uint8_t*)spacingValue - (spacingStub2 + 31)); memcpy(spacingStub2 + 27, &rel, 4);
        rel = (int32_t)(game + DIF_SPACING_SITE_2 + 5 - (uintptr_t)(spacingStub2 + 36)); memcpy(spacingStub2 + 32, &rel, 4);
        rel = (int32_t)(game + DIF_SPACING_SITE_2 + 5 - (uintptr_t)(spacingStub2 + 46)); memcpy(spacingStub2 + 42, &rel, 4);
        *gridValue = ceilf(16.0f * (disk + 1.0f) / 681.0f * 1.015f * 4.0f) / 4.0f;
        memcpy(gridStub, kGridStub, sizeof kGridStub);
        memcpy(gridStub + 19, &radii[1], 4);
        rel = (int32_t)((uint8_t*)gridValue - (gridStub + 33)); memcpy(gridStub + 29, &rel, 4);
        rel = (int32_t)(game + DIF_GRID_SITE + 9 - (uintptr_t)(gridStub + 38)); memcpy(gridStub + 34, &rel, 4);
    }
    float* cellFactor = (float*)(code + 0x5C0); /* x 4, then 0.25 */
    uint8_t* cellStub = code + 0x5D0;
    uint8_t* finalizeStub = code + 0x610;
    if (g_difficulty.mapScale > 1.0f)
    {
        cellFactor[0] = (disk + 1.0f) / 681.0f * 1.04f * 4.0f;
        cellFactor[1] = 0.25f;
        float diskBits = disk / 1024.0f;
        memcpy(cellStub, kCellSpacingStub, sizeof kCellSpacingStub);
        memcpy(cellStub + 15, &diskBits, 4);
        rel = (int32_t)((uint8_t*)&cellFactor[0] - (cellStub + 30)); memcpy(cellStub + 26, &rel, 4);
        rel = (int32_t)((uint8_t*)&cellFactor[1] - (cellStub + 44)); memcpy(cellStub + 40, &rel, 4);
        rel = (int32_t)(game + DIF_CELL_SPACING_SITE + 7 - (uintptr_t)(cellStub + 52)); memcpy(cellStub + 48, &rel, 4);
        memcpy(finalizeStub, kFinalizeSpacingStub, sizeof kFinalizeSpacingStub);
        memcpy(finalizeStub + 24, &diskBits, 4);
        rel = (int32_t)((uint8_t*)&cellFactor[0] - (finalizeStub + 38)); memcpy(finalizeStub + 34, &rel, 4);
        rel = (int32_t)((uint8_t*)&cellFactor[1] - (finalizeStub + 52)); memcpy(finalizeStub + 48, &rel, 4);
        rel = (int32_t)(game + DIF_FINALIZE_SPACING_SITE + 9 - (uintptr_t)(finalizeStub + 57)); memcpy(finalizeStub + 53, &rel, 4);
    }
    uint8_t* restartStubs = code + 0x500;
    for (int i = 0; i < 4; i++)
    {
        uint8_t* stub = restartStubs + 0x20 * i;
        memcpy(stub, kRestartStub, sizeof kRestartStub);
        rel = (int32_t)(watch - (stub + 7)); memcpy(stub + 3, &rel, 4);
        rel = (int32_t)(watch + 8 + 4 * i - (stub + 14)); memcpy(stub + 10, &rel, 4);
        rel = (int32_t)(game + kRestartSites[i] + 6 - (uintptr_t)(stub + 25)); memcpy(stub + 21, &rel, 4);
    }
    uint8_t* zoneStubs = g_difficulty.mapScale > 1.0f ? AllocateNearGame(game, 0x100) : NULL; /* clear (+0), height (+0x40) */
    if (g_difficulty.mapScale > 1.0f)
    {
        if (zoneStubs == NULL) return RefuseDifficulty("no memory within 2 GB of game.dll for the height zones");
        memcpy(zoneStubs, kZoneClearStub, sizeof kZoneClearStub);
        rel = (int32_t)(game + DIF_ARENA_ALLOCATE - (uintptr_t)(zoneStubs + 29)); memcpy(zoneStubs + 25, &rel, 4);
        rel = (int32_t)(game + DIF_MEMSET - (uintptr_t)(zoneStubs + 57)); memcpy(zoneStubs + 53, &rel, 4);
        rel = (int32_t)(zoneStubs + 0x80 - (zoneStubs + 62)); memcpy(zoneStubs + 58, &rel, 4);
        memcpy(zoneStubs + 0x40, kZoneHeightStub, sizeof kZoneHeightStub);
        rel = (int32_t)(game + DIF_ZONE_HEIGHT_BACK - (uintptr_t)(zoneStubs + 0x40 + 16)); memcpy(zoneStubs + 0x40 + 12, &rel, 4);
        uint64_t diag = (uint64_t)(uintptr_t)g_zoneDiag;
        memcpy(zoneStubs + 0x80, kZoneNoteStub, sizeof kZoneNoteStub); memcpy(zoneStubs + 0x80 + 2, &diag, 8);
        rel = (int32_t)(game + DIF_ZONE_CLEAR_BACK - (uintptr_t)(zoneStubs + 0x80 + 53)); memcpy(zoneStubs + 0x80 + 49, &rel, 4);
        memcpy(zoneStubs + 0xC0, kZoneCellStub, sizeof kZoneCellStub); memcpy(zoneStubs + 0xC0 + 2, &diag, 8);
        rel = (int32_t)(zoneStubs + 0x40 - (zoneStubs + 0xC0 + 23)); memcpy(zoneStubs + 0xC0 + 19, &rel, 4);
        DWORD zoneProtect;
        VirtualProtect(zoneStubs, 0x100, PAGE_EXECUTE_READ, &zoneProtect);
    }
    uint8_t* launchStub = code + 0x790; /* jumped to once the countdown delta is registered (DifficultyDelta) */
    if (g_difficulty.minutes > 0.0f)
    {
        uint64_t hash = DifficultyDeltaHash();
        memcpy(launchStub, kLaunchStub, sizeof kLaunchStub);
        rel = (int32_t)(watch + 0x40 - (launchStub + 23)); memcpy(launchStub + 18, &rel, 4);
        memcpy(launchStub + 38, &hash, 8);
        rel = (int32_t)(watch + 0x44 - (launchStub + 69)); memcpy(launchStub + 65, &rel, 4);
        rel = (int32_t)(game + DIF_LAUNCH_BACK - (uintptr_t)(launchStub + 79)); memcpy(launchStub + 75, &rel, 4);
        g_launchStub = launchStub;
    }
    uint8_t* cardTimeStub = code + 0x7E0, *widgetTimeStub = code + 0x810;
    if (g_difficulty.minutes > 0.0f)
    {
        uint64_t manager = game + DIF_HIVEMIND_MANAGER;
        memcpy(cardTimeStub, kCardTimeStub, sizeof kCardTimeStub);
        rel = (int32_t)(watch + 0x58 - (cardTimeStub + 13)); memcpy(cardTimeStub + 9, &rel, 4);
        rel = (int32_t)(game + DIF_MINUTE - (uintptr_t)(cardTimeStub + 30)); memcpy(cardTimeStub + 26, &rel, 4);
        rel = (int32_t)(game + DIF_CARD_TIME_BACK - (uintptr_t)(cardTimeStub + 35)); memcpy(cardTimeStub + 31, &rel, 4);
        memcpy(widgetTimeStub, kWidgetTimeStub, sizeof kWidgetTimeStub);
        memcpy(widgetTimeStub + 8, &manager, 8);
        memcpy(widgetTimeStub + 59, &manager, 8);
        rel = (int32_t)(watch + 0x58 - (widgetTimeStub + 47)); memcpy(widgetTimeStub + 43, &rel, 4);
        rel = (int32_t)(game + DIF_WIDGET_TIME_BACK - (uintptr_t)(widgetTimeStub + 75)); memcpy(widgetTimeStub + 71, &rel, 4);
    }
    uint8_t* holoStubs = code + 0xA20; /* 4 x 0x80, then the constants */
    if (g_difficulty.mapScale > 1.0f)
    {
        float* values = (float*)(holoStubs + 4 * 0x80);
        values[0] = 0.19f; values[1] = 0.45f; values[2] = 1.0f; values[3] = 6.6f;
        for (int i = 0; i < 4; i++)
        {
            uint8_t* stub = holoStubs + 0x80 * i;
            memcpy(stub, kHoloStubs[i].code, kHoloStubs[i].size);
            for (int j = 0; j < 7 && kHoloStubs[i].slots[j].at != 0; j++)
            {
                uintptr_t target = kHoloStubs[i].slots[j].target < 4 ? (uintptr_t)&values[kHoloStubs[i].slots[j].target] : game + kHoloStubs[i].site + kHoloStubs[i].length;
                rel = (int32_t)(target - (uintptr_t)(stub + kHoloStubs[i].slots[j].at + 4));
                memcpy(stub + kHoloStubs[i].slots[j].at, &rel, 4);
            }
        }
    }
    uint8_t* cardViewStub = code + 0x860;
    if (g_difficulty.mapScale > 1.0f)
    {
        float constants[8] = {0.38f, 1.0f, 0.5f / 0.225f, 0.225f, 0.275f, 0.725f, 0.0f, 512.0f};
        memcpy(&constants[6], &kCardScaleValues[0], 4);
        uint8_t* values = cardViewStub + 0xC0;
        memcpy(values, constants, sizeof constants);
        memcpy(watch + 0x48, kCardScaleValues, sizeof kCardScaleValues);
        memcpy(cardViewStub, kCardViewStub, sizeof kCardViewStub);
        for (int i = 0; i < 14; i++)
        {
            uint8_t t = kCardViewSlots[i].target;
            uintptr_t target = t < 8 ? (uintptr_t)(values + 4 * t) : t < 12 ? (uintptr_t)(watch + 0x48 + 4 * (t - 8)) : game + DIF_CARD_VIEW_CALL;
            rel = (int32_t)(target - (uintptr_t)(cardViewStub + kCardViewSlots[i].at + 4));
            memcpy(cardViewStub + kCardViewSlots[i].at, &rel, 4);
        }
    }
    float* viewValues = (float*)(code + 0x750); /* 1.0, 0.17 / 0.19, 0.17 */
    viewValues[0] = 1.0f; viewValues[1] = 0.17f / 0.19f; viewValues[2] = 0.17f;
    uint8_t* viewStub = code + 0x760;
    memcpy(viewStub, kMapViewStub, sizeof kMapViewStub);
    rel = (int32_t)((uint8_t*)&viewValues[1] - (viewStub + 9)); memcpy(viewStub + 5, &rel, 4);
    rel = (int32_t)((uint8_t*)&viewValues[2] - (viewStub + 23)); memcpy(viewStub + 19, &rel, 4);
    rel = (int32_t)(game + DIF_MAPVIEW_CAP_NEXT - (uintptr_t)(viewStub + 33)); memcpy(viewStub + 29, &rel, 4);
    uint8_t* spawnerStub = code + 0x730;
    memcpy(spawnerStub, kSpawnerStub, sizeof kSpawnerStub);
    rel = (int32_t)(game + DIF_SPAWNER_ADD - (uintptr_t)(spawnerStub + 18)); memcpy(spawnerStub + 14, &rel, 4);
    rel = (int32_t)(watch + 0x3C - (spawnerStub + 25)); memcpy(spawnerStub + 21, &rel, 4);
    rel = (int32_t)(game + DIF_SPAWNER_DONE - (uintptr_t)(spawnerStub + 30)); memcpy(spawnerStub + 26, &rel, 4);
    uint8_t* telemetryStub = code + 0x710;
    memcpy(telemetryStub, kTelemetryStub, sizeof kTelemetryStub);
    rel = (int32_t)(game + DIF_TELEMETRY_SITE + 5 - (uintptr_t)(telemetryStub + 20)); memcpy(telemetryStub + 16, &rel, 4);
    uint8_t* captureStub = code + 0x6C0;
    {
        static const uint8_t at[5] = {11, 20, 29, 42, 49}, field[5] = {0x28, 0x2C, 0x30, 0x38, 0x34}, end[5] = {15, 24, 33, 46, 53};
        memcpy(captureStub, kCaptureStub, sizeof kCaptureStub);
        for (int i = 0; i < 5; i++) { rel = (int32_t)(watch + field[i] - (captureStub + end[i])); memcpy(captureStub + at[i], &rel, 4); }
        rel = (int32_t)(game + DIF_CAPTURE_SITE + 11 - (uintptr_t)(captureStub + 71)); memcpy(captureStub + 67, &rel, 4);
    }
    uint8_t* unreachableStub = NULL;
    if (g_difficulty.mapScale > 1.0f)
    {
        float diskBits = disk / 1024.0f;
        unreachableStub = code + 0x660;
        memcpy(unreachableStub, kUnreachableStub, sizeof kUnreachableStub);
        rel = (int32_t)(watch - (unreachableStub + 7)); memcpy(unreachableStub + 3, &rel, 4);
        rel = (int32_t)(watch + 8 - (unreachableStub + 14)); memcpy(unreachableStub + 10, &rel, 4);
        memcpy(unreachableStub + 20, &diskBits, 4);
        rel = (int32_t)(watch + 0x18 - (unreachableStub + 42)); memcpy(unreachableStub + 38, &rel, 4);
        rel = (int32_t)(game + DIF_UNREACHABLE_GO_ON - (uintptr_t)(unreachableStub + 47)); memcpy(unreachableStub + 43, &rel, 4);
        rel = (int32_t)(game + kRestartSites[0] + 6 - (uintptr_t)(unreachableStub + 58)); memcpy(unreachableStub + 54, &rel, 4);
    }
    DWORD protect;
    if (!VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &protect)) return RefuseDifficulty("the bar's stubs could not be made executable");
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    if (SafeRead(game + kDifficultyCopies[0], table, DIF_VANILLA * DIF_ROW)) return RefuseDifficulty("the difficulty table is unreadable");
    uint8_t* row = table + DIF_VANILLA * DIF_ROW;
    memcpy(row, row - DIF_ROW, DIF_ROW);
    uint32_t level = DIF_VANILLA + 1;
    memcpy(row, &g_difficulty.item, 4);
    memcpy(row + 4, &level, 4);
    if (g_difficulty.text[0] != 0) memcpy(row + 8, &g_difficulty.text[0], 4);
    if (g_difficulty.text[1] != 0) memcpy(row + 0xC, &g_difficulty.text[1], 4);
    if (g_difficulty.encounter > 0.0f) memcpy(row + 0xE4, &g_difficulty.encounter, 4);
    if (g_difficulty.guardforce > 0.0f) memcpy(row + 0xE8, &g_difficulty.guardforce, 4);
    if (g_difficulty.cooldown > 0.0f) memcpy(row + 0x104, &g_difficulty.cooldown, 4);
    /* Roaming patrols: per faction (Terminids, Automatons, Illuminate) the
     * interval a channel's timer restarts at (+0x2A0, f32 s; level 10: 65, 78, 75) and the most living patrol enemies
     * (+0x2AC, u32; 41, 39, 48), which the director (0x94EB90) reads from the row every frame. */
    for (int f = 0; f < 3; f++)
    {
        float interval;
        uint32_t cap;
        memcpy(&interval, row + 0x2A0 + 4 * f, 4); memcpy(&cap, row + 0x2AC + 4 * f, 4);
        if (g_difficulty.patrolFrequency > 0.0f) { interval /= g_difficulty.patrolFrequency; memcpy(row + 0x2A0 + 4 * f, &interval, 4); }
        if (g_difficulty.patrolCap > 0.0f) { cap = (uint32_t)((float)cap * g_difficulty.patrolCap + 0.5f); memcpy(row + 0x2AC + 4 * f, &cap, 4); }
    }
    /* Its enemies' own tech level (rosters have a level-11 weight below), objectives, outposts and map size. */
    row[0xEC] = (uint8_t)level;
    if (g_difficulty.optional != 0) memcpy(row + 0x28, &g_difficulty.optional, 4);
    if (g_difficulty.major != 0) memcpy(row + 0x2C, &g_difficulty.major, 4);
    for (int k = 0; g_difficulty.outpostScale > 0.0f && k < 20; k++)
    {
        uint32_t count;
        memcpy(&count, row + 0x34 + 4 * k, 4);
        count = (uint32_t)(count * g_difficulty.outpostScale + 0.5f);
        memcpy(row + 0x34 + 4 * k, &count, 4);
    }
    for (int k = 0; g_difficulty.mapScale > 0.0f && k < 4; k++)
    {
        float radius;
        memcpy(&radius, row + 0xF0 + 4 * k, 4);
        radius *= g_difficulty.mapScale;
        memcpy(row + 0xF0 + 4 * k, &radius, 4);
    }
    /* +0x10 keeps Super Helldive's sprite: the game draws a level's icon in other places (the war table's mission
     * markers) through setters that take atlas sprites only. The mod's icon texture goes to the bar's stub. */
    /* The callers that keep 10 first, then the instructions (the highest level is still 10: nothing reads row 11
     * yet), then the bounds, the highest level last. */
    for (size_t i = 0; i < sizeof kDifficultyKeepTen / sizeof kDifficultyKeepTen[0]; i++)
        if (ApiWrite(game + kDifficultyKeepTen[i], kKeepTen, sizeof kKeepTen))
            return RefuseDifficulty("a caller of the highest level could not keep 10");
    for (size_t i = 0; i < sizeof kDifficultySites / sizeof kDifficultySites[0]; i++)
    {
        int32_t disp = DifficultyDisplacement((uintptr_t)table, game, &kDifficultySites[i]);
        if (ApiWrite(game + kDifficultySites[i].rva + kDifficultySites[i].dispAt, &disp, 4))
        {
            Note("difficulty: could not rewrite the instruction at 0x%x; %zu of %zu moved", kDifficultySites[i].rva, i,
                sizeof kDifficultySites / sizeof kDifficultySites[0]);
            return RefuseDifficulty("an instruction could not be rewritten");
        }
    }
    /* Objectives drawn up to level 10 are drawn at 11 too. */
    for (uint32_t i = 0; i < DIF_OBJECTIVE_ROWS; i++)
    {
        uintptr_t at = game + DIF_OBJECTIVES + i * DIF_OBJECTIVE_ROW + 0x28;
        uint32_t highest = 0;
        if (!SafeRead(at, &highest, 4) && highest == DIF_VANILLA && ApiWrite(at, &level, 4))
            return RefuseDifficulty("an objective's difficulty range could not be widened");
    }
    /* The mission distance factors' readers address the moved table. */
    for (size_t i = 0; i < 7; i++)
    {
        int32_t disp = (int32_t)(int64_t)((uintptr_t)distance - game);
        if (ApiWrite(game + kDistanceSites[i][0] + kDistanceSites[i][1], &disp, 4)) return RefuseDifficulty("a mission distance read could not be moved");
    }
    /* Enemies: every roster row's weights move up 4 bytes with level 11's (level 10's to begin with) after them, the
     * readers follow, compositions up to tech 10 allow 11, and the director's tech level clamp becomes 11. The readers
     * change right after the rows: the director only reads them while a mission is set up, not aboard the ship. */
    for (size_t r = 0; r < DIF_ROSTERS; r++)
    {
        uint64_t rows = 0;
        SafeRead(game + kRosters[r].descriptor, &rows, 8);
        for (uint32_t i = 0; i < kRosters[r].rows; i++)
        {
            float weights[DIF_VANILLA + 1];
            if (SafeRead(rows + i * 0x80 + 0x30, weights, DIF_VANILLA * 4)) return RefuseDifficulty("an enemy roster row is unreadable");
            weights[DIF_VANILLA] = weights[DIF_VANILLA - 1];
            if (ApiWrite(rows + i * 0x80 + 0x2C, weights, sizeof weights)) return RefuseDifficulty("an enemy roster row could not be given its 11th weight");
        }
    }
    for (size_t i = 0; i < 6; i++)
    {
        uint8_t disp = 0x2C;
        if (ApiWrite(game + kWeightReaders[i][0] + kWeightReaders[i][1], &disp, 1)) return RefuseDifficulty("an enemy weight read could not be moved");
    }
    for (size_t r = 0; r < DIF_ROSTERS; r++)
        for (uint32_t k = 0; k < 8; k++)
        {
            uint64_t family = 0, families = 0;
            SafeRead(game + kRosters[r].descriptor + 0x20 + 0x10 * k, &family, 8);
            SafeRead(game + kRosters[r].descriptor + 0x28 + 0x10 * k, &families, 8);
            for (uint64_t i = 0; family != 0 && i < families; i++)
            {
                uint32_t highest = 0;
                if (!SafeRead(family + i * 0x118 + 0xF0, &highest, 4) && highest == DIF_VANILLA && ApiWrite(family + i * 0x118 + 0xF0, &level, 4))
                    return RefuseDifficulty("an enemy composition's tech range could not be widened");
            }
        }
    if (ApiWrite(game + DIF_TECH_CLAMP + 1, &level, 4)) return RefuseDifficulty("the enemy tech level clamp could not be raised");
    /* Operations and missions offered up to level 10 are offered at 11 too. */
    for (uint32_t i = 0; i < DIF_OPERATION_ROWS + DIF_MISSION_ROWS; i++)
    {
        uintptr_t at = i < DIF_OPERATION_ROWS ? game + DIF_OPERATIONS + i * DIF_OPERATION_ROW + 0x10
                                              : game + DIF_MISSIONS + (i - DIF_OPERATION_ROWS) * DIF_MISSION_ROW + 0x10;
        uint32_t highest = 0;
        uint64_t delta = 0;
        uint8_t kind = 0;
        if (i >= DIF_OPERATION_ROWS && !SafeRead(at - 0x10 + 0x360, &delta, 8) && !SafeRead(at - 0x10 + 0x34, &kind, 1) &&
            (((g_difficulty.flags & HF_DIFFICULTY_NO_ERADICATE) && delta == DIF_ERADICATE_DELTA) ||
             ((g_difficulty.flags & HF_DIFFICULTY_NO_RAPID_ACQUISITION) && delta == DIF_RAPID_DELTA) ||
             ((g_difficulty.flags & HF_DIFFICULTY_NO_BLITZ) && kind == 6))) /* kind 6: Blitz */
        {
            uint32_t ten = DIF_VANILLA; /* rows allowing up to 100 (city Blitz 35, 140): only 11 lies above 10 */
            if (!SafeRead(at, &highest, 4) && highest > DIF_VANILLA && ApiWrite(at, &ten, 4))
                return RefuseDifficulty("an excluded mission's difficulty range could not be narrowed");
            continue;
        }
        if (!SafeRead(at, &highest, 4) && highest == DIF_VANILLA && ApiWrite(at, &level, 4))
            return RefuseDifficulty("an operation's or mission's difficulty range could not be widened");
    }
    {
        uint8_t* board = AllocateNearGame(game, 0x200);
        if (board == NULL) return RefuseDifficulty("no memory within 2 GB of game.dll for the mission board");
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
            if (ApiWrite(game + kBoardSites[i].site, jump, kBoardSites[i].length)) return RefuseDifficulty("the war table's mission board could not be given level 11");
        }
        if (ApiWrite(game + DIF_BOARD_REGION_MAX + 1, &level, 4)) return RefuseDifficulty("the regions' missions could not reach level 11");
        uint8_t* spread = AllocateNearGame(game, 0x100);
        if (spread == NULL) return RefuseDifficulty("no memory within 2 GB of game.dll for the location spreading");
        memcpy(spread, kSpreadReferenceStub, sizeof kSpreadReferenceStub);
        rel = (int32_t)(game + DIF_SPREAD_REFERENCE_BACK - (uintptr_t)(spread + 24)); memcpy(spread + 20, &rel, 4);
        DWORD spreadProtect;
        VirtualProtect(spread, 0x100, PAGE_EXECUTE_READ, &spreadProtect);
        uint8_t spreadJump[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
        rel = (int32_t)((uintptr_t)spread - (game + DIF_SPREAD_REFERENCE + 5)); memcpy(spreadJump + 1, &rel, 4);
        if (ApiWrite(game + DIF_SPREAD_REFERENCE, spreadJump, 10)) return RefuseDifficulty("the location spreading could not be guarded");
    }
    if (g_difficulty.flags & HF_DIFFICULTY_SECOND_PRIMARY) MixInstall(game);
    if (g_difficulty.grant != 0)
    {
        uint8_t pin[7];
        uint8_t* grant = AllocateNearGame(game, 0x200); /* the stub (+0), the table copy (+0x100) */
        uint32_t sets[8][6];
        if (SafeRead(game + DIF_GRANT_SITE, pin, 7) || memcmp(pin, kGrantPin, 7) != 0 || grant == NULL || SafeRead(game + DIF_GRANTS, sets, sizeof sets))
            ApiLog(g_difficulty.mod, "difficulty %s: the mission stratagems are not the reviewed ones; stratagem %u is not granted", g_difficulty.name, g_difficulty.grant);
        else
        {
            uint32_t added = 0;
            for (int set = 1; set < 8; set++)
                for (int k = 0; k < 6; k++)
                {
                    if (sets[set][k] == g_difficulty.grant) break;
                    if (sets[set][k] == 0) { sets[set][k] = g_difficulty.grant; added++; break; }
                }
            uint64_t world = game + DIF_WORLD, copy = (uint64_t)(uintptr_t)(grant + 0x100);
            memcpy(grant + 0x100, sets, sizeof sets);
            memcpy(grant, kGrantStub, sizeof kGrantStub);
            memcpy(grant + 2, &world, 8); memcpy(grant + 41, &copy, 8);
            rel = (int32_t)(game + DIF_GRANT_BACK - (uintptr_t)(grant + 54)); memcpy(grant + 50, &rel, 4);
            rel = (int32_t)(game + DIF_GRANT_BACK - (uintptr_t)(grant + 66)); memcpy(grant + 62, &rel, 4);
            DWORD grantProtect;
            VirtualProtect(grant, 0x200, PAGE_EXECUTE_READ, &grantProtect);
            uint8_t jump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
            rel = (int32_t)((uintptr_t)grant - (game + DIF_GRANT_SITE + 5)); memcpy(jump + 1, &rel, 4);
            if (ApiWrite(game + DIF_GRANT_SITE, jump, sizeof jump)) return RefuseDifficulty("the mission stratagems could not be extended");
            ApiLog(g_difficulty.mod, "difficulty %s: every mission grants stratagem %u (%u mission kinds)", g_difficulty.name, g_difficulty.grant, added);
        }
    }
    if (g_difficulty.flags & HF_DIFFICULTY_RADAR)
    {
        uint8_t pin[30];
        uint8_t* radar = AllocateNearGame(game, 0x100);
        if (SafeRead(game + DIF_RADAR_STATE_SITE - 21, pin, 30) || memcmp(pin, kRadarStatePin, 30) != 0 || radar == NULL)
            ApiLog(g_difficulty.mod, "difficulty %s: the objectives' discovered state is not the reviewed one; the radar station is kept but not marked from the start", g_difficulty.name);
        else
        {
            uint64_t world = game + DIF_WORLD;
            memcpy(radar, kRadarStateStub, sizeof kRadarStateStub);
            memcpy(radar + 16, &world, 8);
            rel = (int32_t)(game + DIF_RADAR_STATE_BACK - (uintptr_t)(radar + 69)); memcpy(radar + 65, &rel, 4);
            DWORD radarProtect;
            VirtualProtect(radar, 0x100, PAGE_EXECUTE_READ, &radarProtect);
            uint8_t jump[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
            rel = (int32_t)((uintptr_t)radar - (game + DIF_RADAR_STATE_SITE + 5)); memcpy(jump + 1, &rel, 4);
            if (ApiWrite(game + DIF_RADAR_STATE_SITE, jump, sizeof jump)) return RefuseDifficulty("the radar station's discovered state could not be set");
        }
        RadarInstall(game);
        if (g_difficulty.flags & HF_DIFFICULTY_CITY_ICBM) CityIcbmInstall(game);
    }
    int32_t bandDisp = (int32_t)(int64_t)((uintptr_t)bands - game);
    if (ApiWrite(game + DIF_BAND_SITE + 4, &bandDisp, 4)) return RefuseDifficulty("the band sprites could not be moved");
    uint8_t iconCallNew[5] = {0xE8};
    rel = (int32_t)((uintptr_t)iconStub - (game + DIF_ICON_CALL + 5));
    memcpy(iconCallNew + 1, &rel, 4);
    if (g_difficulty.icon != 0 && ApiWrite(game + DIF_ICON_CALL, iconCallNew, 5)) return RefuseDifficulty("the difficulty icon setter could not be extended");
    for (int i = 0; i < 4 && g_difficulty.icon != 0; i++)
    {
        uintptr_t call = i == 0 ? game + DIF_HUD_ICON_CALL : game + kIconSites[i - 1].call;
        rel = (int32_t)((uintptr_t)(iconChecks + 0x10 * i) - (call + 5));
        memcpy(iconCallNew + 1, &rel, 4);
        if (ApiWrite(call, iconCallNew, 5)) return RefuseDifficulty("a difficulty icon reader could not be extended");
    }
    for (size_t i = 0; i < sizeof kBarSites / sizeof kBarSites[0]; i++)
    {
        uint8_t jump[32];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        rel = (int32_t)((uintptr_t)stubs[i] - (game + kBarSites[i].rva + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kBarSites[i].rva, jump, kBarSites[i].length)) return RefuseDifficulty("the bar could not be given its 11th widget");
    }
    if (g_difficulty.mapScale > 1.0f)
    {
        const struct { uint32_t rva, length; uint8_t* stub; } mapSites[6] = {{DIF_SPACING_SITE, 5, spacingStub},
            {DIF_SPACING_SITE_2, 5, spacingStub2}, {DIF_GRID_SITE, 9, gridStub}, {DIF_CELL_SPACING_SITE, 7, cellStub},
            {DIF_FINALIZE_SPACING_SITE, 9, finalizeStub}, {DIF_MAP_TYPE_SITE, 7, radiiStub}};
        for (size_t i = 0; i < 6; i++)
        {
            uint8_t jump[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
            rel = (int32_t)((uintptr_t)mapSites[i].stub - (game + mapSites[i].rva + 5));
            memcpy(jump + 1, &rel, 4);
            if (ApiWrite(game + mapSites[i].rva, jump, mapSites[i].length)) return RefuseDifficulty("the map generator could not be given the larger map");
        }
        for (int i = 0; i < 5; i++)
            if (ApiWrite(game + kStampIndex[i].rva, kStampIndex[i].bytes, kStampIndex[i].length))
                return RefuseDifficulty("the map generator's stamp index could not be enlarged");
        {
            uint8_t jump[14];
            memset(jump, 0x90, sizeof jump);
            jump[0] = 0xE9;
            uint8_t clear[21], height[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90}, load = 0x8B;
            memset(clear, 0x90, sizeof clear);
            clear[0] = 0xE9;
            rel = (int32_t)((uintptr_t)zoneStubs - (game + DIF_ZONE_CLEAR + 5)); memcpy(clear + 1, &rel, 4);
            rel = (int32_t)((uintptr_t)(zoneStubs + 0xC0) - (game + DIF_ZONE_HEIGHT + 5)); memcpy(height + 1, &rel, 4);
            if (ApiWrite(game + DIF_ZONE_CLEAR, clear, sizeof clear) || ApiWrite(game + DIF_ZONE_TABLE_LEA + 1, &load, 1) ||
                ApiWrite(game + DIF_ZONE_HEIGHT, height, sizeof height))
                return RefuseDifficulty("the height zones' table could not be moved");
            memset(jump, 0x90, sizeof jump);
            jump[0] = 0xE9;
            rel = (int32_t)((uintptr_t)convoyStub - (game + DIF_CONVOY_SITE + 5));
            memcpy(jump + 1, &rel, 4);
            if (ApiWrite(game + DIF_CONVOY_SITE, jump, 12)) return RefuseDifficulty("the preview's convoy route could not be limited");
            for (int i = 0; i < 7; i++)
                if (ApiWrite(game + kCityCapacities[i].rva + kCityCapacities[i].at, &kCityCapacities[i].value, kCityCapacities[i].width))
                    return RefuseDifficulty("the city generator could not be enlarged");
            uint8_t* structured = AllocateNearGame(game, 0x200); /* revoronoi (+0, +0x40, +0x80), the flags (+0xC0, writes +0x100) */
            if (structured == NULL) return RefuseDifficulty("no memory within 2 GB of game.dll for the structured maps");
            {
                float diskBits = disk / 1024.0f;
                for (int i = 0; i < 3; i++)
                {
                    uint8_t* stub = structured + 0x40 * i;
                    memcpy(stub, kReVoronoiStubs[i].stub, kReVoronoiStubs[i].size);
                    memcpy(stub + kReVoronoiStubs[i].diskAt, &diskBits, 4);
                    rel = (int32_t)((uint8_t*)&cellFactor[0] - (stub + kReVoronoiStubs[i].factorAt + 4)); memcpy(stub + kReVoronoiStubs[i].factorAt, &rel, 4);
                    rel = (int32_t)((uint8_t*)&cellFactor[1] - (stub + kReVoronoiStubs[i].quarterAt + 4)); memcpy(stub + kReVoronoiStubs[i].quarterAt, &rel, 4);
                    rel = (int32_t)(game + kReVoronoiStubs[i].site + kReVoronoiStubs[i].length - (uintptr_t)(stub + kReVoronoiStubs[i].backAt + 4));
                    memcpy(stub + kReVoronoiStubs[i].backAt, &rel, 4);
                }
                uint8_t* vista = AllocateNearGame(game, 0x80);
                if (vista == NULL) return RefuseDifficulty("no memory within 2 GB of game.dll for the vista level");
                {
                    float fade = 389.12f * g_difficulty.mapScale + 174.0f + 57.0f; /* the radii stub's fade */
                    if (fade > disk) fade = disk;
                    float scale = fade / 620.0f;
                    memcpy(vista, kVistaPoseStub, sizeof kVistaPoseStub);
                    memcpy(vista + 18, &diskBits, 4);
                    memcpy(vista + 80, &scale, 4); memcpy(vista + 84, &scale, 4);
                    rel = (int32_t)(game + DIF_VISTA_POSE + 7 - (uintptr_t)(vista + 69)); memcpy(vista + 65, &rel, 4);
                    DWORD vistaProtect;
                    VirtualProtect(vista, 0x80, PAGE_EXECUTE_READ, &vistaProtect);
                    uint8_t jump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
                    rel = (int32_t)((uintptr_t)vista - (game + DIF_VISTA_POSE + 5)); memcpy(jump + 1, &rel, 4);
                    if (ApiWrite(game + DIF_VISTA_POSE, jump, 7)) return RefuseDifficulty("the vista level could not be widened");
                }
                uint8_t* cull = AllocateNearGame(game, 0x40);
                if (cull == NULL) return RefuseDifficulty("no memory within 2 GB of game.dll for the playable area's cull");
                memcpy(cull, kCullRadiusStub, sizeof kCullRadiusStub);
                memcpy(cull + 6, &diskBits, 4);
                rel = (int32_t)(game + DIF_CULL_RADIUS + 8 - (uintptr_t)(cull + 25)); memcpy(cull + 21, &rel, 4);
                rel = (int32_t)(game + DIF_CULL_RADIUS + 8 - (uintptr_t)(cull + 38)); memcpy(cull + 34, &rel, 4);
                DWORD cullProtect;
                VirtualProtect(cull, 0x40, PAGE_EXECUTE_READ, &cullProtect);
                {
                    uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
                    rel = (int32_t)((uintptr_t)cull - (game + DIF_CULL_RADIUS + 5)); memcpy(jump + 1, &rel, 4);
                    if (ApiWrite(game + DIF_CULL_RADIUS, jump, 8)) return RefuseDifficulty("the playable area's cull could not be widened");
                }
                uint8_t* clear = structured + 0xC0;
                memcpy(clear, kFlagsClearStub, sizeof kFlagsClearStub);
                rel = (int32_t)(game + DIF_ARENA_ALLOCATE - (uintptr_t)(clear + 20)); memcpy(clear + 16, &rel, 4);
                rel = (int32_t)(game + DIF_MEMSET - (uintptr_t)(clear + 40)); memcpy(clear + 36, &rel, 4);
                rel = (int32_t)(game + DIF_FLAGS_CLEAR_BACK - (uintptr_t)(clear + 48)); memcpy(clear + 44, &rel, 4);
                for (int i = 0; i < 3; i++)
                {
                    uint8_t* stub = structured + 0x100 + 0x20 * i;
                    memcpy(stub, kFlagsWrites[i].stub, kFlagsWrites[i].size);
                    rel = (int32_t)(game + kFlagsWrites[i].back - (uintptr_t)(stub + kFlagsWrites[i].backAt + 4)); memcpy(stub + kFlagsWrites[i].backAt, &rel, 4);
                }
                DWORD structuredProtect;
                VirtualProtect(structured, 0x200, PAGE_EXECUTE_READ, &structuredProtect);
                for (int i = 0; i < 3; i++)
                {
                    uint8_t jump[11];
                    memset(jump, 0x90, sizeof jump);
                    jump[0] = 0xE9;
                    rel = (int32_t)((uintptr_t)(structured + 0x40 * i) - (game + kReVoronoiStubs[i].site + 5)); memcpy(jump + 1, &rel, 4);
                    if (ApiWrite(game + kReVoronoiStubs[i].site, jump, kReVoronoiStubs[i].length)) return RefuseDifficulty("the structured maps could not be given the larger map");
                }
                for (int i = 0; i < 8; i++)
                    if (ApiWrite(game + kBucketPool[i].rva + kBucketPool[i].at, &kBucketPool[i].value, 4))
                        return RefuseDifficulty("the structured maps' vertex buckets could not be enlarged");
                uint8_t flagsJump[32];
                memset(flagsJump, 0x90, sizeof flagsJump);
                flagsJump[0] = 0xE9;
                rel = (int32_t)((uintptr_t)clear - (game + DIF_FLAGS_CLEAR + 5)); memcpy(flagsJump + 1, &rel, 4);
                uint8_t load = 0x8B;
                uint32_t flagsCap = 0x400;
                if (ApiWrite(game + DIF_FLAGS_CLEAR, flagsJump, 32) || ApiWrite(game + DIF_FLAGS_READ + 1, &load, 1) ||
                    ApiWrite(game + kFlagsChecks[0] + 3, &flagsCap, 4) || ApiWrite(game + kFlagsChecks[1] + 3, &flagsCap, 4))
                    return RefuseDifficulty("the city locations' flags could not be moved");
                for (int i = 0; i < 3; i++)
                {
                    uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
                    rel = (int32_t)((uintptr_t)(structured + 0x100 + 0x20 * i) - (game + kFlagsWrites[i].site + 5)); memcpy(jump + 1, &rel, 4);
                    if (ApiWrite(game + kFlagsWrites[i].site, jump, kFlagsWrites[i].length)) return RefuseDifficulty("the city locations' flags could not be moved");
                }
            }
            uint8_t* clusters = AllocateNearGame(game, 0x100);
            if (clusters == NULL) return RefuseDifficulty("no memory within 2 GB of game.dll for the location clusters");
            for (int i = 0; i < 4; i++)
            {
                uint8_t* stub = clusters + 0x40 * i;
                memcpy(stub, kClusterSites[i].stub, kClusterSites[i].size);
                rel = (int32_t)(game + kClusterSites[i].back - (uintptr_t)(stub + kClusterSites[i].backAt + 4)); memcpy(stub + kClusterSites[i].backAt, &rel, 4);
                rel = (int32_t)(game + kClusterSites[i].skip - (uintptr_t)(stub + kClusterSites[i].skipAt + 4)); memcpy(stub + kClusterSites[i].skipAt, &rel, 4);
            }
            {
                DWORD clusterProtect;
                VirtualProtect(clusters, 0x100, PAGE_EXECUTE_READ, &clusterProtect);
            }
            for (int i = 0; i < 4; i++)
            {
                uint8_t jump[13];
                memset(jump, 0x90, sizeof jump);
                jump[0] = 0xE9;
                rel = (int32_t)((uintptr_t)(clusters + 0x40 * i) - (game + kClusterSites[i].site + 5)); memcpy(jump + 1, &rel, 4);
                if (ApiWrite(game + kClusterSites[i].site, jump, kClusterSites[i].length)) return RefuseDifficulty("the location clusters could not be guarded");
            }
            uint8_t* appends = AllocateNearGame(game, 0x100);
            if (appends == NULL) return RefuseDifficulty("no memory within 2 GB of game.dll for the location clusters");
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
            {
                DWORD appendProtect;
                VirtualProtect(appends, 0x100, PAGE_EXECUTE_READ, &appendProtect);
            }
            for (int i = 0; i < 4; i++)
            {
                uint8_t jump[21];
                memset(jump, 0x90, sizeof jump);
                jump[0] = 0xE9;
                rel = (int32_t)((uintptr_t)(appends + 0x40 * i) - (game + kClusterAppends[i].site + 5)); memcpy(jump + 1, &rel, 4);
                if (ApiWrite(game + kClusterAppends[i].site, jump, kClusterAppends[i].load + kClusterAppends[i].store + kClusterAppends[i].inc))
                    return RefuseDifficulty("the location clusters could not be guarded");
            }
            uint8_t* footprints = AllocateNearGame(game, 0x100);
            if (footprints == NULL) return RefuseDifficulty("no memory within 2 GB of game.dll for the city diagnostic");
            {
                uint64_t counters = (uint64_t)(uintptr_t)g_cityFootprints;
                memcpy(footprints, kFootprintStub, sizeof kFootprintStub);
                memcpy(footprints + 5, &counters, 8);
                rel = (int32_t)(game + DIF_FOOTPRINT_FITS - (uintptr_t)(footprints + 26)); memcpy(footprints + 22, &rel, 4);
                rel = (int32_t)(game + DIF_FOOTPRINT_OVER - (uintptr_t)(footprints + 44)); memcpy(footprints + 40, &rel, 4);
                DWORD footprintProtect;
                VirtualProtect(footprints, 0x100, PAGE_EXECUTE_READ, &footprintProtect);
                uint8_t check[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
                rel = (int32_t)((uintptr_t)footprints - (game + DIF_FOOTPRINT_CHECK + 5)); memcpy(check + 1, &rel, 4);
                if (ApiWrite(game + DIF_FOOTPRINT_CHECK, check, 9)) return RefuseDifficulty("the city generator's check could not be watched");
            }
            uintptr_t exe = ApiModuleBase("helldivers2.exe", NULL);
            uint8_t navPin[23];
            uint8_t* nav = AllocateNearGame(game, 0x200);
            if (exe == 0 || SafeRead(exe + DIF_NAV_PIN, navPin, sizeof navPin) || memcmp(navPin, kNavExePin, sizeof navPin) != 0 ||
                SafeRead(game + DIF_NAV_SITE, navPin, 15) || memcmp(navPin, kNavPin, 15) != 0 ||
                SafeRead(game + DIF_NAV_SHIP_SITE, navPin, 10) || memcmp(navPin, kNavShipPin, 10) != 0 || nav == NULL)
                ApiLog(g_difficulty.mod, "difficulty %s: the navigation grid is not the reviewed one; larger maps keep a navmesh of +-512 m (mission stratagems will not stick beyond it)", g_difficulty.name);
            else
            {
                void* function = (void*)NavRetile;
                uint64_t system = game + DIF_NAV_SYSTEM;
                g_navExe = exe;
                uint8_t read[8];
                float sizes[2] = {0.0f, 0.0f};
                g_navTiles256 = !SafeRead(exe + DIF_NAV_TILE_SIZE_READ, read, 8) && !memcmp(read, kNavTileSizeRead, 8) &&
                    !SafeRead(exe + 0x16A9DB0, &sizes[0], 4) && !SafeRead(exe + 0x16AA7C0, &sizes[1], 4) && sizes[0] == 128.0f && sizes[1] == 256.0f;
                if (!g_navTiles256)
                    ApiLog(g_difficulty.mod, "difficulty %s: the navmesh's terrain build is not the reviewed one; larger maps keep a navmesh of +-512 m (mission stratagems will not stick beyond it)", g_difficulty.name);
                memcpy(nav, kNavStub, sizeof kNavStub);
                memcpy(nav + 60, &function, 8); memcpy(nav + 126, &system, 8);
                rel = (int32_t)(game + DIF_NAV_BACK - (uintptr_t)(nav + 145)); memcpy(nav + 141, &rel, 4);
                memcpy(nav + 0x100, kNavShipStub, sizeof kNavShipStub);
                memcpy(nav + 0x100 + 65, &function, 8);
                rel = (int32_t)(game + DIF_NAV_SHIP_BACK - (uintptr_t)(nav + 0x100 + 144)); memcpy(nav + 0x100 + 140, &rel, 4);
                DWORD navProtect;
                VirtualProtect(nav, 0x200, PAGE_EXECUTE_READ, &navProtect);
                uint8_t navJump[15];
                memset(navJump, 0x90, sizeof navJump);
                navJump[0] = 0xE9;
                rel = (int32_t)((uintptr_t)nav - (game + DIF_NAV_SITE + 5)); memcpy(navJump + 1, &rel, 4);
                if (ApiWrite(game + DIF_NAV_SITE, navJump, 15)) return RefuseDifficulty("the navigation grid could not be widened");
                rel = (int32_t)((uintptr_t)(nav + 0x100) - (game + DIF_NAV_SHIP_SITE + 5)); memcpy(navJump + 1, &rel, 4);
                if (ApiWrite(game + DIF_NAV_SHIP_SITE, navJump, 10)) return RefuseDifficulty("the navigation grid could not be restored for the ship");
            }
        }
    }
    if (DifficultyNeedsBlock())
    {
        /* The locations and the mission stamps: the block's size first (blocks made from now on fit the new layout),
         * then the instructions and capacities. Not while a block exists already (it has the old size). */
        uintptr_t world = 0, levelGen = 0, block = 0;
        if (!SafeRead(game + DIF_WORLD, &world, 8) && world != 0 && !SafeRead(world + 0xAE288, &levelGen, 8) && levelGen != 0)
            SafeRead(levelGen + 0x2C8, &block, 8);
        if (block != 0)
            Note("difficulty: a generation block exists already (0x%llx): the locations keep 2740 places and the mission stamps 128; larger maps and more objectives may overrun them", (unsigned long long)block);
        else
        {
            uint32_t size = DIF_BLOCK_NEW, places = DIF_LOCATIONS;
            for (size_t i = 0; i < sizeof kGenerationBlockSize / sizeof kGenerationBlockSize[0]; i++)
                if (ApiWrite(game + kGenerationBlockSize[i].rva + kGenerationBlockSize[i].at, &size, 4)) return RefuseDifficulty("the generation block could not be enlarged");
            for (size_t i = 0; i < sizeof kLocationRefs / sizeof kLocationRefs[0]; i++)
            {
                uint32_t moved = LocationMoved(kLocationRefs[i].value);
                if (ApiWrite(game + kLocationRefs[i].rva + kLocationRefs[i].at, &moved, 4)) return RefuseDifficulty("a location reference could not be moved");
            }
            for (size_t i = 0; i < sizeof kLocationCaps / sizeof kLocationCaps[0]; i++)
                if (ApiWrite(game + kLocationCaps[i].rva + kLocationCaps[i].at, &places, 4)) return RefuseDifficulty("a location capacity could not be raised");
            uint32_t types = DIF_LOCATION_TYPES_C_NEW, typePlaces = DIF_LOCATION_TYPES;
            for (size_t i = 0; i < sizeof kLocationTypeRefs / sizeof kLocationTypeRefs[0]; i++)
                if (ApiWrite(game + kLocationTypeRefs[i].rva + kLocationTypeRefs[i].at, &types, 4)) return RefuseDifficulty("a location type reference could not be moved");
            for (size_t i = 0; i < sizeof kLocationTypeCaps / sizeof kLocationTypeCaps[0]; i++)
                if (ApiWrite(game + kLocationTypeCaps[i].rva + kLocationTypeCaps[i].at, &typePlaces, 4)) return RefuseDifficulty("a location type capacity could not be raised");
            for (size_t i = 0; i < sizeof kMinimapRefs / sizeof kMinimapRefs[0]; i++)
            {
                uint32_t moved = MinimapMoved(kMinimapRefs[i].value);
                if (ApiWrite(game + kMinimapRefs[i].rva + kMinimapRefs[i].at, &moved, 4)) return RefuseDifficulty("a minimap reference could not be moved");
            }
            for (size_t i = 0; i < sizeof kMinimapCaps / sizeof kMinimapCaps[0]; i++)
            {
                uint32_t places = kMinimapCaps[i].value * 4u;
                if (ApiWrite(game + kMinimapCaps[i].rva + kMinimapCaps[i].at, &places, 4)) return RefuseDifficulty("a minimap capacity could not be raised");
            }
            uint32_t stamps = DIF_STAMPS_NEW;
            for (size_t i = 0; i < sizeof kStampRefs / sizeof kStampRefs[0]; i++)
                if (ApiWrite(game + kStampRefs[i].rva + kStampRefs[i].at, &stamps, 4)) return RefuseDifficulty("a mission stamp reference could not be moved");
            for (int i = 0; i < 74; i++)
                if (ApiWrite(game + kObjectiveListSites[i].rva + kObjectiveListSites[i].at, &kObjectiveListSites[i].value, kObjectiveListSites[i].width))
                    return RefuseDifficulty("an objective list reference could not be moved");
            g_objectivePlaces = DIF_OBJECTIVES_MAX;
            uint32_t placeList = DIF_PLACES_NEW, placeSet = DIF_PLACE_SET_NEW;
            for (size_t i = 0; i < sizeof kPlaceListRefs / sizeof kPlaceListRefs[0]; i++)
                if (ApiWrite(game + kPlaceListRefs[i].rva + kPlaceListRefs[i].at, &placeList, 4)) return RefuseDifficulty("an objective place reference could not be moved");
            for (size_t i = 0; i < sizeof kPlaceSetRefs / sizeof kPlaceSetRefs[0]; i++)
                if (ApiWrite(game + kPlaceSetRefs[i].rva + kPlaceSetRefs[i].at, &placeSet, 4)) return RefuseDifficulty("a place set reference could not be moved");
            uint32_t clusters = DIF_CLUSTERS_NEW, clusterPlaces = DIF_CLUSTERS;
            for (int i = 0; i < 16; i++)
                if (ApiWrite(game + kClusterRefs[i].rva + kClusterRefs[i].at, &clusters, 4) ||
                    ApiWrite(game + kClusterCaps[i].rva + kClusterCaps[i].at, &clusterPlaces, 4)) return RefuseDifficulty("a location cluster reference could not be moved");
            uint8_t* counts = AllocateNearGame(game, 0x100);
            uint8_t countPin[9];
            if (counts != NULL && !SafeRead(game + kClusterCountSites[0].site, countPin, 9) && !memcmp(countPin, kClusterCountPinA, 9) &&
                !SafeRead(game + kClusterCountSites[1].site, countPin, 7) && !memcmp(countPin, kClusterCountPinB, 7))
            {
                for (int i = 0; i < 2; i++)
                {
                    uint8_t* stub = counts + 0x40 * i;
                    memcpy(stub, kClusterCountSites[i].stub, kClusterCountSites[i].size);
                    int32_t back = (int32_t)(game + kClusterCountSites[i].back - (uintptr_t)(stub + kClusterCountSites[i].backAt + 4));
                    memcpy(stub + kClusterCountSites[i].backAt, &back, 4);
                }
                DWORD countProtect;
                VirtualProtect(counts, 0x100, PAGE_EXECUTE_READ, &countProtect);
                for (int i = 0; i < 2; i++)
                {
                    uint8_t jump[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
                    int32_t to = (int32_t)((uintptr_t)(counts + 0x40 * i) - (game + kClusterCountSites[i].site + 5)); memcpy(jump + 1, &to, 4);
                    if (ApiWrite(game + kClusterCountSites[i].site, jump, kClusterCountSites[i].length)) return RefuseDifficulty("the location clusters' count could not be raised");
                }
            }
            else Note("difficulty: the location clusters' count is not the reviewed one: a kind keeps its usual clusters");
            Note("difficulty: the level's locations have %u places, their types %u, the mission stamps %u and the objectives %u (block 0x%X bytes; %zu + %zu + %zu instructions, %zu + %zu capacities)",
                DIF_LOCATIONS, DIF_LOCATION_TYPES, DIF_STAMPS, DIF_OBJECTIVES_MAX, DIF_BLOCK_NEW, sizeof kLocationRefs / sizeof kLocationRefs[0], sizeof kLocationTypeRefs / sizeof kLocationTypeRefs[0],
                sizeof kStampRefs / sizeof kStampRefs[0], sizeof kLocationCaps / sizeof kLocationCaps[0], sizeof kLocationTypeCaps / sizeof kLocationTypeCaps[0]);
        }
    }
    for (int i = 0; i < 4; i++)
    {
        uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
        rel = (int32_t)((uintptr_t)(i == 0 && unreachableStub != NULL ? unreachableStub : restartStubs + 0x20 * i) - (game + kRestartSites[i] + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kRestartSites[i], jump, 6)) return RefuseDifficulty("a level generation restart could not be noted");
    }
    {
        uint8_t jump[11];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        rel = (int32_t)((uintptr_t)captureStub - (game + DIF_CAPTURE_SITE + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + DIF_CAPTURE_SITE, jump, sizeof jump)) return RefuseDifficulty("the minimap capture could not be noted");
        uint8_t limit[5] = {0xE9};
        rel = (int32_t)((uintptr_t)telemetryStub - (game + DIF_TELEMETRY_SITE + 5));
        memcpy(limit + 1, &rel, 4);
        if (ApiWrite(game + DIF_TELEMETRY_SITE, limit, sizeof limit)) return RefuseDifficulty("the selected mission's telemetry could not be limited");
        uint8_t spawners[6] = {0xE9, 0, 0, 0, 0, 0x90};
        rel = (int32_t)((uintptr_t)spawnerStub - (game + DIF_SPAWNER_SITE + 5));
        memcpy(spawners + 1, &rel, 4);
        if (ApiWrite(game + DIF_SPAWNER_SITE, spawners, sizeof spawners)) return RefuseDifficulty("the director's spawner list could not be limited");
        int32_t toOne = (int32_t)((uintptr_t)&viewValues[0] - (game + DIF_MAPVIEW_MIN + 9));
        uint8_t cap[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
        rel = (int32_t)((uintptr_t)viewStub - (game + DIF_MAPVIEW_CAP + 5));
        memcpy(cap + 1, &rel, 4);
        if (ApiWrite(game + DIF_MAPVIEW_MIN + 5, &toOne, 4) || ApiWrite(game + DIF_MAPVIEW_CAP, cap, sizeof cap))
            return RefuseDifficulty("the war table's map view could not be widened");
        if (g_difficulty.minutes > 0.0f)
        {
            uint8_t card[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90}, widget[13];
            rel = (int32_t)((uintptr_t)cardTimeStub - (game + DIF_CARD_TIME_SITE + 5));
            memcpy(card + 1, &rel, 4);
            memset(widget, 0x90, sizeof widget);
            widget[0] = 0xE9;
            rel = (int32_t)((uintptr_t)widgetTimeStub - (game + DIF_WIDGET_TIME_SITE + 5));
            memcpy(widget + 1, &rel, 4);
            if (ApiWrite(game + DIF_CARD_TIME_SITE, card, sizeof card) || ApiWrite(game + DIF_WIDGET_TIME_SITE, widget, sizeof widget))
                return RefuseDifficulty("the mission card's duration could not be given the level's countdown");
        }
        if (g_difficulty.mapScale > 1.0f)
        {
            for (int i = 0; i < 9; i++)
            {
                uintptr_t site = game + kCardScales[i].rva;
                int32_t disp = (int32_t)((uintptr_t)(watch + 0x48 + 4 * kCardScales[i].global) - (site + kCardScales[i].length));
                if (ApiWrite(site + kCardScales[i].at, &disp, 4)) return RefuseDifficulty("the mission card's markers could not be scaled");
            }
            uint8_t view[42];
            memset(view, 0x90, sizeof view);
            view[0] = 0xE9;
            rel = (int32_t)((uintptr_t)cardViewStub - (game + DIF_CARD_VIEW_SITE + 5));
            memcpy(view + 1, &rel, 4);
            if (ApiWrite(game + DIF_CARD_VIEW_SITE, view, sizeof view)) return RefuseDifficulty("the mission card's map view could not be widened");
            for (int i = 0; i < 4; i++)
            {
                uint8_t jump[10];
                memset(jump, 0x90, sizeof jump);
                jump[0] = 0xE9;
                rel = (int32_t)((uintptr_t)(holoStubs + 0x80 * i) - (game + kHoloStubs[i].site + 5));
                memcpy(jump + 1, &rel, 4);
                if (ApiWrite(game + kHoloStubs[i].site, jump, kHoloStubs[i].length)) return RefuseDifficulty("the war table hologram's mission scale could not be fitted");
            }
        }
    }
    for (int i = 0; i < DIF_LEVEL_LOADS; i++)
    {
        uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        rel = (int32_t)((uintptr_t)(levelStubs + 0x20 * i) - (game + kLevelLoads[i].rva + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kLevelLoads[i].rva, jump, kLevelLoads[i].length)) return RefuseDifficulty("a per-level value's reader could not be clamped");
    }
    {
        uint8_t sib = 0x85;
        for (int i = 0; i < 5; i++)
            if (ApiWrite(game + kLevelInPlace[i].rva, kLevelInPlace[i].bytes, kLevelInPlace[i].length)) return RefuseDifficulty("a per-level value's reader could not be clamped");
        if (ApiWrite(game + DIF_HUD_TARGET_SIB, &sib, 1)) return RefuseDifficulty("the HUD's objective target could not be clamped");
        const uint32_t markerSites[2] = {DIF_MARKER_LOOP_SITE, DIF_MARKER_ROUTE_SITE};
        const uint8_t markerLengths[2] = {8, 6};
        for (int i = 0; i < 2; i++)
        {
            uint8_t jump[8];
            memset(jump, 0x90, sizeof jump);
            jump[0] = 0xE9;
            rel = (int32_t)((uintptr_t)(markerStubs + 0x20 * i) - (game + markerSites[i] + 5));
            memcpy(jump + 1, &rel, 4);
            if (ApiWrite(game + markerSites[i], jump, markerLengths[i])) return RefuseDifficulty("the map's marker slots could not be guarded");
        }
    }
    if (g_objectivePlaces == DIF_OBJECTIVES_MAX)
    {
        const struct { uint32_t site; uint8_t length, at; } jumps[7] = {{DIF_TELEMETRY_COUNT_SITE, 10, 0}, {DIF_TELEMETRY_LOOP_SITE, 11, 0x20},
            {DIF_SELECTION_END, 7, 0x40}, {0x18B4EA0u, 7, 0xA0}, {0x18B4EB4u, 7, 0xB0}, {0x18B4F00u, 8, 0xC0}, {DIF_BADGE_SITE, 7, 0xD0}};
        for (int i = 0; i < 7; i++)
        {
            uint8_t jump[11];
            memset(jump, 0x90, sizeof jump);
            jump[0] = 0xE9;
            rel = (int32_t)((uintptr_t)(objectiveStubs + jumps[i].at) - (game + jumps[i].site + 5));
            memcpy(jump + 1, &rel, 4);
            if (ApiWrite(game + jumps[i].site, jump, jumps[i].length)) return RefuseDifficulty("the objective selection or mission-end telemetry could not be extended");
        }
    }
    else if (g_difficulty.flags & HF_DIFFICULTY_SECOND_PRIMARY)
        ApiLog(g_difficulty.mod, "difficulty %s: the objective list could not grow (a generation block existed already); missions keep one main objective", g_difficulty.name);
    g_difficultyGame = game;
    g_restartWatch = watch;
    for (size_t i = sizeof kDifficultyBounds / sizeof kDifficultyBounds[0]; i-- > 0;)
        if (ApiWrite(game + kDifficultyBounds[i].rva + kDifficultyBounds[i].immAt, &level, kDifficultyBounds[i].width))
            return RefuseDifficulty("a bound could not be widened");
    g_difficultyTable = table;
    g_difficultyState = 1;
    HookText();
    float points[3];
    memcpy(points, row + 0xE4, 8);
    memcpy(points + 2, row + 0x104, 4);
    Note("difficulty: the table moved to 0x%llx (%zu instructions, %zu bounds, %zu callers keep 10); level 11 is %s",
        (unsigned long long)(uintptr_t)table, sizeof kDifficultySites / sizeof kDifficultySites[0],
        sizeof kDifficultyBounds / sizeof kDifficultyBounds[0], sizeof kDifficultyKeepTen / sizeof kDifficultyKeepTen[0], g_difficulty.name);
    ApiLog(g_difficulty.mod, "difficulty %s is level 11 (unlock item %08x): encounter %.0f, guardforce %.0f, spawner cooldown %.2f s",
        g_difficulty.name, g_difficulty.item, (double)points[0], (double)points[1], (double)points[2]);
    if (g_difficulty.mapScale > 1.0f)
        ApiLog(g_difficulty.mod, "difficulty %s: maps %.2f x the radius (structured maps excepted), generated within %.0f m, route cells %.1f m apart, Voronoi grid %.2f m, route cell areas x %.3f",
            g_difficulty.name, (double)g_difficulty.mapScale, (double)disk, (double)*spacingValue, (double)*gridValue, (double)(cellFactor[0] / 4.0f));
    return 1;
}

/* Polled by the loader's thread: logs each level generation restart (which pass failed, at which difficulty, how many
 * times for that level). The first 40. */
/* zones-<n>.tsv (see kZoneNoteStub): per used cell type its flags (+0x170), city mode (+0x1A8), zone margin (+0x208)
 * and raise (+0x20C); per group the table's min / max / height; per route cell its type, position (route cell +0, +4),
 * heights +0xFC / +0x108, visited and group (zone cells). Read-only, from the generation arena while it is alive. */
static void ZoneDump(void)
{
    static int dumps;
    uint64_t table = g_zoneDiag[0], record = g_zoneDiag[1], level = g_zoneDiag[2], markerAt = g_zoneDiag[4], infoAt = g_zoneDiag[5];
    uint32_t cells = (uint32_t)g_zoneDiag[3];
    uint8_t difficulty = 0;
    uint64_t info = g_zoneDiag[6], types = g_zoneDiag[7];
    if (dumps >= 8 || SafeRead(level + 0xF9, &difficulty, 1) || difficulty != DIF_VANILLA + 1 || cells == 0 || cells > 8192 || info == 0) return;
    wchar_t name[32];
    swprintf(name, 32, L"zones-%d.tsv", ++dumps);
    HANDLE file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[512];
    DWORD written;
    float radius = 0.0f;
    SafeRead(level + 0x2D70, &radius, 4);
    uint64_t marker = 0;
    if (markerAt && infoAt > info && (infoAt - 0x18 - info) % 0x20 == 0 && (infoAt - 0x18 - info) / 0x20 < cells)
        marker = markerAt - 4 - 8 * ((infoAt - 0x18 - info) / 0x20);
    int n = snprintf(line, sizeof line, "level\t%#llx\tplayable\t%.0f\tcells\t%u\trecord\t%#llx\tinfo\t%#llx\ttypes\t%#llx\tmarkers\t%#llx\ttable\t%#llx\n",
        (unsigned long long)level, radius * 1024.0, cells, (unsigned long long)record, (unsigned long long)info, (unsigned long long)types,
        (unsigned long long)marker, (unsigned long long)table);
    WriteFile(file, line, (DWORD)n, &written, NULL);
    uint8_t seenType[256] = {0};
    int groups = 0;
    n = snprintf(line, sizeof line, "cell\ttype\tx\ty\th_fc\th_108\tvisited\tgroup\n");
    WriteFile(file, line, (DWORD)n, &written, NULL);
    for (uint32_t i = 0; i < cells; i++)
    {
        uint8_t entry[0x20];
        uint64_t cell = 0;
        float pos[2] = {0}, fc = 0, h = 0;
        if (SafeRead(info + 0x20 * (uint64_t)i, entry, sizeof entry)) break;
        int32_t type;
        memcpy(&type, entry + 0xC, 4); memcpy(&cell, entry + 0x18, 8);
        if (type >= 0 && type < 256) seenType[type] = 1;
        SafeRead(cell, pos, 8); SafeRead(cell + 0xFC, &fc, 4); SafeRead(cell + 0x108, &h, 4);
        uint8_t visited = 0xFF;
        int32_t group = -1;
        if (marker) { SafeRead(marker + 8 * (uint64_t)i, &visited, 1); SafeRead(marker + 8 * (uint64_t)i + 4, &group, 4); }
        if (visited == 1 && group + 1 > groups) groups = group + 1;
        n = snprintf(line, sizeof line, "%u\t%d\t%.1f\t%.1f\t%.2f\t%.2f\t%u\t%d\n", i, type, pos[0], pos[1], fc, h, visited, group);
        WriteFile(file, line, (DWORD)n, &written, NULL);
    }
    n = snprintf(line, sizeof line, "type\tobject\tflags170\tmode1a8\tsize18c\tmargin208\traise20c\troad228\n");
    WriteFile(file, line, (DWORD)n, &written, NULL);
    for (int t = 0; t < 256; t++)
    {
        uint64_t object = 0;
        if (!seenType[t] || SafeRead(types + 8 * (uint64_t)t, &object, 8)) continue;
        uint8_t flags = 0;
        uint32_t mode = 0;
        float size = 0, margin = 0, raise = 0, road = 0;
        SafeRead(object + 0x170, &flags, 1); SafeRead(object + 0x1A8, &mode, 4); SafeRead(object + 0x18C, &size, 4);
        SafeRead(object + 0x208, &margin, 4); SafeRead(object + 0x20C, &raise, 4); SafeRead(object + 0x228, &road, 4);
        n = snprintf(line, sizeof line, "%d\t%#llx\t%#x\t%u\t%.2f\t%g\t%g\t%.2f\n", t, (unsigned long long)object, flags, mode, size, margin, raise, road);
        WriteFile(file, line, (DWORD)n, &written, NULL);
    }
    n = snprintf(line, sizeof line, "group\tmin\tmax\theight\n");
    WriteFile(file, line, (DWORD)n, &written, NULL);
    for (int g = 0; g < groups + 1 && g < 4096; g++)
    {
        float entry[3] = {0};
        if (SafeRead(table + 12 * (uint64_t)g, entry, 12)) break;
        n = snprintf(line, sizeof line, "%d\t%g\t%g\t%g\n", g, entry[0], entry[1], entry[2]);
        WriteFile(file, line, (DWORD)n, &written, NULL);
    }
    CloseHandle(file);
    Note("difficulty: zones-%d.tsv written (%u route cells, %d zone groups, playable %.0f m)", dumps, cells, groups, radius * 1024.0);
}

static void DifficultyWatch(void)
{
    {
        static uint64_t dumpedTable, cursor;
        static ULONGLONG changed;
        if (g_zoneDiag[0] != 0 && g_zoneDiag[0] != dumpedTable)
        {
            if (changed == 0 || g_zoneDiag[4] != cursor) { cursor = g_zoneDiag[4]; changed = GetTickCount64(); }
            else if (GetTickCount64() - changed >= 300) { dumpedTable = g_zoneDiag[0]; changed = 0; ZoneDump(); }
        }
    }
    static uint32_t seen[4], pockets;
    static int logged;
    if (g_restartWatch == NULL) return;
    static ULONGLONG mixLogged;
    static LONG seconds, misses;
    if ((g_secondPrimaries != seconds || g_secondPrimaryMisses != misses) && GetTickCount64() - mixLogged >= 5000)
    {
        mixLogged = GetTickCount64();
        seconds = g_secondPrimaries; misses = g_secondPrimaryMisses;
        Note("difficulty: missions mixed with a partner %ld times (%ld partner steps; %ld selections found no partner)", seconds, (long)g_mixSteps, misses);
    }
    {
        /* The outposts (GuardForce records, [game+0x33265C0]: count +0xC, records +0x48 of 0x2B8, active +0x2AB, size +0x2A8
         * 1..4 = small, medium, large, stronghold; as the mission-end telemetry counts them, 0x62F620). */
        static uint32_t outposts[5];
        static ULONGLONG outpostsChecked;
        uint64_t forces = 0, records = 0;
        uint32_t count = 0, sizes[5] = {0};
        if (g_difficultyGame && GetTickCount64() - outpostsChecked >= 3000)
        {
            outpostsChecked = GetTickCount64();
            if (!SafeRead(g_difficultyGame + 0x33265C0, &forces, 8) && forces && !SafeRead(forces + 0xC, &count, 4) && count <= 4096 &&
                !SafeRead(forces + 0x48, &records, 8) && records)
            {
                for (uint32_t i = 0; i < count; i++)
                {
                    uint8_t state[4];
                    if (SafeRead(records + 0x2B8 * (uint64_t)i + 0x2A8, state, 4)) break;
                    if (state[3] && state[0] >= 1 && state[0] <= 4) sizes[state[0]]++;
                }
                sizes[0] = count;
                if (memcmp(sizes, outposts, sizeof sizes) != 0 && (sizes[1] | sizes[2] | sizes[3] | sizes[4]))
                    Note("difficulty: outposts in the mission: %u small, %u medium, %u large, %u strongholds (%u records)",
                        sizes[1], sizes[2], sizes[3], sizes[4], count);
                memcpy(outposts, sizes, sizeof sizes);
            }
        }
    }
    static LONG footprintsMost, footprintOverflows;
    if (g_cityFootprints[0] != footprintsMost || g_cityFootprints[1] != footprintOverflows)
    {
        footprintsMost = g_cityFootprints[0]; footprintOverflows = g_cityFootprints[1];
        Note("difficulty: city buildings: the footprint list used up to %ld places; %ld cells overflowed it (last: %ld wanted of %ld), losing the later cells' buildings",
            footprintsMost, footprintOverflows, g_cityFootprints[3], g_cityFootprints[2]);
    }
    static LONG navLarger, navVanilla;
    if (g_navLarger != navLarger || g_navVanilla != navVanilla)
    {
        navLarger = g_navLarger; navVanilla = g_navVanilla;
        Note("difficulty: navigation grid retiled: %ld times to 256 m tiles (larger maps), %ld to 128 m", navLarger, navVanilla);
    }
    if (logged >= 40) return;
    /* A difficulty-11 level's map fields, 1.5 s and 6 s after its init (a preview's generator does not live long). */
    static uintptr_t level;
    static ULONGLONG levelSeen;
    static int levelLogs;
    uintptr_t latest = *(volatile uintptr_t*)(g_restartWatch + 0x20);
    if (latest != 0 && latest != level) { level = latest; levelSeen = GetTickCount64(); levelLogs = 0; FieldWatch(latest + 0x2D70); }
    if (level != 0) FieldWatch(level + 0x2D70); /* again once the preview record is known (DR2) */
    ULONGLONG age = GetTickCount64() - levelSeen;
    if (level != 0 && levelLogs < 2 && age >= (levelLogs == 0 ? 1500u : 6000u))
    {
        uint8_t difficulty = 0, mapType = 0xFF, minimap = 0;
        float f[5] = {0};
        uintptr_t type = 0;
        levelLogs++;
        if (!SafeRead(level + 0xF9, &difficulty, 1) && difficulty == DIF_VANILLA + 1 && !SafeRead(level + 0x2D70, f, sizeof f))
        {
            if (!SafeRead(level + 0x2DE0, &type, 8) && type != 0) SafeRead(type + 0x1D, &mapType, 1);
            SafeRead(level + 0xF950, &minimap, 1);
            Note("difficulty: level 0x%llx at %.1f s: map type %u%s, playable %.0f m (factor %.2f), border %.0f m, fade %.0f m, disk %.0f m",
                (unsigned long long)level, (double)age / 1000.0, mapType, minimap ? " (minimap)" : "", (double)(f[0] * 1024.0f),
                (double)f[4], (double)(f[1] * 1024.0f), (double)(f[2] * 1024.0f), (double)(f[3] * 1024.0f));
            logged++;
        }
    }
    static uint32_t captures;
    uint32_t captured = *(volatile uint32_t*)(g_restartWatch + 0x34);
    if (captured != captures)
    {
        captures = captured;
        float playable = *(volatile float*)(g_restartWatch + 0x28), border = *(volatile float*)(g_restartWatch + 0x2C), factor = *(volatile float*)(g_restartWatch + 0x30);
        Note("difficulty: minimap captured (%u) at difficulty %u: playable %.0f m (factor %.2f), border %.0f m", captured,
            *(volatile uint32_t*)(g_restartWatch + 0x38), (double)(playable * 1024.0f), (double)factor, (double)(border * 1024.0f));
        logged++;
    }
    static uint32_t timed;
    uint32_t launched = *(volatile uint32_t*)(g_restartWatch + 0x44);
    if (launched != timed)
    {
        timed = launched;
        Note("difficulty: a level-11 mission launched with the %.0f-minute countdown delta (%u so far)", (double)g_difficulty.minutes, launched);
        uint64_t world = 0, level = 0, block = 0, mapType = 0;
        uint32_t counts[8] = {0};
        uint8_t type = 0xFF;
        if (g_difficultyGame && !SafeRead(g_difficultyGame + DIF_WORLD, &world, 8) && world && !SafeRead(world + 0xAE288, &level, 8) && level &&
            !SafeRead(level + 0x2C8, &block, 8) && block)
        {
            SafeRead(level + 0x2DE0, &mapType, 8);
            if (mapType) SafeRead(mapType + 0x1D, &type, 1);
            SafeRead(block + 0x11A715C, &counts[0], 4); SafeRead(block + 0x11A725C, &counts[1], 4); SafeRead(block + 0x11A7260, &counts[2], 4);
            SafeRead(block + 0x11A7264, &counts[3], 4); SafeRead(block + 0x11A71B4, &counts[4], 4); SafeRead(block + 0x11A71C0, &counts[5], 4);
            SafeRead(block + 0x11A7108, &counts[6], 4); SafeRead(block + 0x11A71CC, &counts[7], 4);
            uintptr_t exe = ApiModuleBase("helldivers2.exe", NULL);
            uint64_t physics = 0;
            uint32_t bodies[3] = {0};
            if (exe && !SafeRead(exe + 0x27BA890 + 0xB0 * 2, &physics, 8) && physics && !SafeRead(physics + 0x4C, &bodies[0], 4) &&
                !SafeRead(physics + 0x50, &bodies[1], 4) && !SafeRead(physics + 0x40, &bodies[2], 4))
                Note("difficulty: the mission's physics bodies: %u in use (highest index %u) of %u", bodies[0], bodies[1], bodies[2] & 0x3FFFFFFFu);
            uint64_t render = 0, store = 0;
            uint32_t objects[2] = {0};
            if (!SafeRead(world + 0x10E8, &render, 8) && render && !SafeRead(render + 0x2301F8, &store, 8) && store && !SafeRead(store + 0x38, objects, 8))
                Note("difficulty: the mission's render objects: %u in use of %u", objects[0], objects[1]);
            Note("difficulty: the mission's map (type %u): important locations %u of 256 (cities flag 1024), city walls %u / %u / %u of 64 / 128 / 128, vertices %u of 975, route cells %u of 1024, edges %u of 1621, locations %u of %u",
                type, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[7], counts[6], DIF_LOCATIONS);
        }
        logged++;
    }
    static uint32_t dropped;
    uint32_t spawnersLeft = *(volatile uint32_t*)(g_restartWatch + 0x3C);
    if (spawnersLeft != dropped)
    {
        dropped = spawnersLeft;
        Note("difficulty: the enemy director's 128 spawners are full: %u more left out so far (their outposts stay)", spawnersLeft);
        logged++;
    }
    uint32_t left = *(volatile uint32_t*)(g_restartWatch + 0x18);
    if (left != pockets)
    {
        pockets = left;
        Note("difficulty: FixUnreachableAreas could not open a pocket of the map after %d tries: the level keeps it (%u so far)", DIF_UNREACHABLE_TRIES, left);
        logged++;
    }
    for (int i = 0; i < 4; i++)
    {
        uint32_t count = *(volatile uint32_t*)(g_restartWatch + 8 + 4 * i);
        if (count == seen[i]) continue;
        seen[i] = count;
        uintptr_t lg = *(volatile uintptr_t*)g_restartWatch, namePtr = 0;
        uint32_t restarts = 0;
        uint8_t difficulty = 0, minimap = 0;
        char name[64] = "?";
        SafeRead(lg + 0xF954, &restarts, 4);
        SafeRead(lg + 0xF9, &difficulty, 1);
        SafeRead(lg + 0xF950, &minimap, 1);
        if (!SafeRead(lg + 0xF958, &namePtr, 8) && namePtr != 0 && !SafeRead(namePtr, name, sizeof name - 1)) name[sizeof name - 1] = 0;
        for (char* c = name; *c; c++) if (*c < 32 || *c > 126) { *c = 0; break; }
        Note("difficulty: level generation restart (%s failed; %u for this level, first failed pass %s) at difficulty %u%s, level 0x%llx",
            kRestartPasses[i], restarts, name, difficulty, minimap ? " (minimap)" : "", (unsigned long long)lg);
        logged++;
    }
}
