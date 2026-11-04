/* The generation block's arrays moved (difficulty.c; larger maps and more objectives need them): the locations, their
 * types, the minimap's arrays, the mission stamps, the objective list, the places and the clusters. Included by
 * hellforge_modloader.c after difficulty_maps.c. */

/* The level's locations (objectives, points of interest, outposts, ...): two arrays of 2740 in the generation block G
 * (A: G+0x758E20, 0xE0 each; B: G+0x7EEBA0, 0x40 each), unchecked. Level 10 makes ~824; 2 x maps with Hell Forge's
 * objectives and outposts made 3900..5500 and overran them (GenerateResourceHash crashed at 0xC176B8). Larger maps move
 * both to the end of a larger block, DIF_LOCATIONS places each: the block's size (2 allocations, 4 clears), every
 * instruction addressing them (location_sites.h, from location_sites.py) and the builders' capacities. Blocks are made
 * with a world (0xAB8B70 -> 0xF6E710), after the loader installs this; one made before keeps the old layout, so the
 * move is skipped then. */
/* DIF_LOCATIONS, DIF_LOCATIONS_A/_B and their new places: difficulty.c (the loader's readers use them too). */
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
/* x DIF_MINIMAP_SCALE (8 since 2026-11-04: op 0x55 0xF59D30 appends every location's proxies without a check,
 * 0xF59E01, and op 0x58 0xF5C3F0 one unit each; 16384 locations) */
#define DIF_MINIMAP_SCALE 8u
#define DIF_MINIMAP_P_NEW (DIF_LOCATION_TYPES_C_NEW + DIF_LOCATION_TYPES * 0x10u)
#define DIF_MINIMAP_S_NEW (DIF_MINIMAP_P_NEW + 0x1000u * DIF_MINIMAP_SCALE * 0x30u)
#define DIF_MINIMAP_U_NEW (DIF_MINIMAP_S_NEW + 0x800u * DIF_MINIMAP_SCALE * 0x58u)
#define DIF_MINIMAP_V_NEW (DIF_MINIMAP_U_NEW + 0x1000u * DIF_MINIMAP_SCALE * 4u)
/* The mission stamp list (G+0x8BBD70, 128 x 0x10: set, group, item, objective id), filled by GenerateMissionStampList
 * (0xF8E530) without a check (its pass records' capacity, 0x80, is never read): stamps are made per objective, and
 * level 11's 13 objectives made more than 128, the 129th over the objective list right after it (G+0x8BC570: +0 the
 * mission row; GenerateObjectiveLocations then crashed at 0xF8F670). It moves after V with DIF_STAMPS places; 21
 * instructions address it (lea [r12 + 0x8BBD70] in the op 0x15 pass records). */
#define DIF_STAMPS 1024u
#define DIF_STAMPS_OLD 0x8BBD70u
#define DIF_STAMPS_NEW (DIF_MINIMAP_V_NEW + 0x4000u * DIF_MINIMAP_SCALE * 4u)
/* The objective list S (G+0x8BC570: +0 row, +4 ids, +0x44 used flags, +0x54 stamp starts, +0x94 stamp counts, all 16,
 * +0xD4 count, +0xD8.. tag lists), filled from the mission's descriptor (up to 32 ids) without a check, moves after the
 * stamps with 30 places (research-objective-capacity, layout L30: counts S'-0xF0, starts S'-0x78, row +0, ids +4, used
 * +0x7C, count and tag lists where they were; every consumer's displacement is patched, disp8s included). */
/* Layout L48 (2026-11-04, scratch obj48/obj48.md; 48 places, for an operation's missions on one map): starts S'-0xF0,
 * used S'-0x30, row +0, ids +4 (+0xC4..+0xD4 free), count +0xD4 and tag lists +0xD8..+0x100 where they were, counts
 * +0x100..+0x1C0. The 61 G-relative values are L30's (S' did not move); nine S-relative values changed; the four starts
 * reads that held a disp8 (0xF8E64E, 0xF8E7F5, 0xF8F68D, 0xF8FB67: width 0 in the table) go through stubs with disp32
 * (kObjectiveStartStubs). 0xF8F623 (r10 = 4) and 0xF9007B ([rax+r9-4]) rely on ids at +4. */
#define DIF_OBJLIST_PLACES 48u
#define DIF_OBJLIST_REGION (DIF_STAMPS_NEW + DIF_STAMPS * 0x10u)
#define DIF_OBJLIST_NEW (DIF_OBJLIST_REGION + 0xF0u)
/* The objective and extraction places (G+0x8ABCC0: 16 pointers, count G+0x11A7170; GenerateObjectiveLocations 0xF8F1D0
 * adds one per placement attempt, retries included, AddImportantLocations 0xF90DC0 adds extraction points) and the place
 * set (G+0x8C4A38: 64 x 0x10, count G+0x11A71AC) are filled without a check (their pass records' capacities are never
 * read). The 17th place overwrote the next field, the planet's generation settings (G+0x8ABD40: a level-11 preview with 15
 * objectives crashed in GenerateWaterStart spawning its water unit, #ID[0], research-crash-quarry). Both move after the
 * objective list: 512 places and 256 (29 and 25 instructions: the pass builders' leas and two readers). */
#define DIF_PLACES 512u
#define DIF_PLACES_NEW (DIF_OBJLIST_REGION + 0x2B0u)
#define DIF_PLACE_SET 256u
#define DIF_PLACE_SET_NEW (DIF_PLACES_NEW + DIF_PLACES * 8u)
/* The location clusters (GenerateLocationClusters, op 0x18: G+0x896FB0, 8 x 0xE0, 24 members each, count G+0x11A7154)
 * hold at most 192 important locations; a larger city map's ran past them (a preview crashed at 0xF78EA5, then at
 * 0xF7A460 through an unclustered location). They move after the place set with 32 places (16 pass builders' leas and
 * their capacities), and the pass makes at least one cluster per 24 locations of a kind (kClusterCountSites). */
#define DIF_CLUSTERS 32u
#define DIF_CLUSTERS_NEW (DIF_PLACE_SET_NEW + DIF_PLACE_SET * 0x10u)
/* The important locations (G+0x8996B0: 256 x 0x108, count G+0x11A715C; objectives, extraction, outposts, points of
 * interest, satellites, crossings) are appended by every location pass without a check (their pass records' 0x100 is
 * never read), and the game reads them all mission long (objective, extraction, marker and garrison links through
 * +0x98). A level 11 mission on an extra large map made 99; three missions' content passes 256. They move after the
 * clusters with DIF_IMPORTANT places: the 124 instructions whose displacement falls in the list (important_sites.h,
 * generated from the dump: generator, builders and the mission-time readers), field offsets kept. SpreadLocations_Cities'
 * flags hold 1024 already (difficulty_cities.c). The loader's own readers use g_importantList. */
#include "important_sites.h"
#define DIF_IMPORTANT 1024u
#define DIF_IMPORTANT_OLD 0x8996B0u
#define DIF_IMPORTANT_NEW (DIF_CLUSTERS_NEW + DIF_CLUSTERS * 0xE0u)
_Static_assert(DIF_IMPORTANT_NEW % 16u == 0, "the important locations' new place is aligned");
/* The linked locations (G+0x8AB6C0: 64 x 0x18, +8 a location, +0x10 its satellite; count G+0x11A716C), appended by
 * AddSatelliteLocations (0xF86DC0, its write 0xF88416) without a check: three missions' satellites pass 64, and past the
 * list (and the old places, now unused) lies the planet's generation settings G+0x8ABD40. They move after the important
 * locations with DIF_LINKED places (7 instructions, kLinkedRefs; the objective init reads them through an add at
 * 0x5D120F). */
#define DIF_LINKED 1024u
#define DIF_LINKED_OLD 0x8AB6C0u
#define DIF_LINKED_NEW (DIF_IMPORTANT_NEW + DIF_IMPORTANT * 0x108u)
/* The route graph (GenerateRoutes 0xFD03E0 and its builders): nodes G+0x8C4E40 (975 x 0x98, count G+0x11A71B4), edges
 * G+0x937348 (1621 x 0x48, count G+0x11A71CC), the edges' class bytes G+0x11A1F98 (op 0xAE, count G+0x11A7250) and the
 * road points G+0x9568E4 (9936 x 8, count G+0x11A71D4) are appended without a check; a larger map's route cells
 * (rebuilt type 1 maps, an operation's three cities: ~507 cells, ~1015 nodes) ran the nodes into the next list (the
 * s70 crashes at 0xF6481C). They move after the linked locations with room for 1024 route cells' Voronoi graph (nodes
 * <= 2n, edges <= 3n): every instruction addressing them (route_sites.h: the builders' leas, the generator and the
 * mission-time readers, all G = [lg+0x2C8]) and the pass records' capacities, which GenerateRoutes clears by and
 * sizes its hash tables from (arena chunks grow to fit, 0x173B98C). Limits kept: RouteNav's 3072 node ids, the road
 * entries' 14-bit edge and u16 point ids. */
#include "route_sites.h"
#define DIF_ROUTE_NODES 2304u
#define DIF_ROUTE_EDGES 3456u
#define DIF_ROUTE_POINTS 24576u
#define DIF_ROUTE_NODES_OLD 0x8C4E40u
#define DIF_ROUTE_EDGES_OLD 0x937348u
#define DIF_ROUTE_CLASSES_OLD 0x11A1F98u
#define DIF_ROUTE_POINTS_OLD 0x9568E4u
/* each array keeps its old place's phase modulo 16 (nodes 0, edges 8, class bytes 8, points 4) */
#define DIF_PHASE16(at, phase) ((((at) + 0xFu) & ~0xFu) + (phase))
#define DIF_ROUTE_NODES_NEW DIF_PHASE16(DIF_LINKED_NEW + DIF_LINKED * 0x18u, 0u)
#define DIF_ROUTE_EDGES_NEW DIF_PHASE16(DIF_ROUTE_NODES_NEW + DIF_ROUTE_NODES * 0x98u, 8u)
#define DIF_ROUTE_CLASSES_NEW DIF_PHASE16(DIF_ROUTE_EDGES_NEW + DIF_ROUTE_EDGES * 0x48u, 8u)
#define DIF_ROUTE_POINTS_NEW DIF_PHASE16(DIF_ROUTE_CLASSES_NEW + DIF_ROUTE_EDGES, 4u)
_Static_assert(DIF_ROUTE_NODES_NEW % 16u == DIF_ROUTE_NODES_OLD % 16u && DIF_ROUTE_EDGES_NEW % 16u == DIF_ROUTE_EDGES_OLD % 16u &&
    DIF_ROUTE_CLASSES_NEW % 16u == DIF_ROUTE_CLASSES_OLD % 16u && DIF_ROUTE_POINTS_NEW % 16u == DIF_ROUTE_POINTS_OLD % 16u, "the route arrays keep their phase");
#define DIF_BLOCK_NEW DIF_PHASE16(DIF_ROUTE_POINTS_NEW + DIF_ROUTE_POINTS * 8u, 0u)
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
/* The per-cluster number is read at run time from the stub block (+0x80 the divisor - 1, +0x84 the shift after the
 * multiply by 0xAAAAAAAB: 23 / 36 = 24 per cluster, 5 / 34 = 6), so an extra large level can ask for smaller clusters
 * (difficulty_world.c, WorldPassClusters) while the others keep 24. rip-relative reads: {at, rel32 at, end}. */
static const uint8_t kClusterCountStubA[55] = {
    0x41, 0x39, 0xD2, 0x41, 0x0F, 0x42, 0xD2,                         /*  0 cmp r10d, edx; cmovb edx, r10d (as before) */
    0x44, 0x89, 0xD0,                                                 /*  7 mov eax, r10d */
    0x03, 0x05, 0, 0, 0, 0,                                           /* 10 add eax, [rip -> +0x80] (rel32 at 12) */
    0xB9, 0xAB, 0xAA, 0xAA, 0xAA, 0x48, 0x0F, 0xAF, 0xC1,             /* 16 mov ecx, 0xAAAAAAAB; imul rax, rcx */
    0x8B, 0x0D, 0, 0, 0, 0,                                           /* 25 mov ecx, [rip -> +0x84] (rel32 at 27) */
    0x48, 0xD3, 0xE8,                                                 /* 31 shr rax, cl (eax = ceil(r10d / per cluster)) */
    0x44, 0x89, 0xC9, 0x44, 0x29, 0xF9,                               /* 34 mov ecx, r9d; sub ecx, r15d (places left) */
    0x39, 0xC8, 0x0F, 0x47, 0xC1,                                     /* 40 cmp eax, ecx; cmova eax, ecx */
    0x39, 0xD0, 0x0F, 0x47, 0xD0,                                     /* 45 cmp eax, edx; cmova edx, eax */
    0xE9, 0, 0, 0, 0};                                                /* 50 jmp 0xF7995D (rel32 at 51) */
static const uint8_t kClusterCountStubB[54] = {
    0x50, 0x51,                                                       /*  0 push rax (r9d, the capacity); push rcx (the cap) */
    0x44, 0x89, 0xD0,                                                 /*  2 mov eax, r10d */
    0x03, 0x05, 0, 0, 0, 0,                                           /*  5 add eax, [rip -> +0x80] (rel32 at 7) */
    0x41, 0xB8, 0xAB, 0xAA, 0xAA, 0xAA, 0x49, 0x0F, 0xAF, 0xC0,       /* 11 mov r8d, 0xAAAAAAAB; imul rax, r8 */
    0x8B, 0x0D, 0, 0, 0, 0,                                           /* 21 mov ecx, [rip -> +0x84] (rel32 at 23) */
    0x48, 0xD3, 0xE8,                                                 /* 27 shr rax, cl */
    0x41, 0x89, 0xC0, 0x59,                                           /* 30 mov r8d, eax; pop rcx */
    0x44, 0x39, 0xC1, 0x41, 0x0F, 0x42, 0xC8,                         /* 34 cmp ecx, r8d; cmovb ecx, r8d (the cap, at least that) */
    0x58,                                                             /* 41 pop rax */
    0x44, 0x39, 0xD1, 0x44, 0x0F, 0x42, 0xD1,                         /* 42 cmp ecx, r10d; cmovb r10d, ecx (as before) */
    0xE9, 0, 0, 0, 0};                                                /* 49 jmp 0xF7994E (rel32 at 50) */
static const uint8_t kClusterCountReads[2][2][3] = {{{12, 16, 0x80}, {27, 31, 0x84}}, {{7, 11, 0x80}, {23, 27, 0x84}}};
static const struct { uint32_t site; uint8_t length; const uint8_t* pin; const uint8_t* stub; uint8_t size, backAt; uint32_t back; } kClusterCountSites[2] = {
    {0xF7991Fu, 9, kClusterCountPinA, kClusterCountStubA, 55, 51, 0xF7995Du},
    {0xF79947u, 7, kClusterCountPinB, kClusterCountStubB, 54, 50, 0xF7994Eu}};
static const LocationSite kPlaceSetRefs[25] = {
    {0xC1AF98u, 0, 0x8C4A38u}, {0xC1B8B1u, 0, 0x8C4A38u}, {0xC1BB83u, 0, 0x8C4A38u}, {0xC1C4BFu, 0, 0x8C4A38u},
    {0xC1CA89u, 0, 0x8C4A38u}, {0xC1CD12u, 0, 0x8C4A38u}, {0xC1D48Au, 0, 0x8C4A38u}, {0xC25D0Bu, 0, 0x8C4A38u},
    {0xC27D5Fu, 0, 0x8C4A38u}, {0xC284D7u, 0, 0x8C4A38u}, {0xC287A4u, 0, 0x8C4A38u}, {0xC28DD7u, 0, 0x8C4A38u},
    {0xC29285u, 0, 0x8C4A38u}, {0xC2950Fu, 0, 0x8C4A38u}, {0xC29979u, 0, 0x8C4A38u}, {0xC2DD25u, 0, 0x8C4A38u},
    {0xC2FD62u, 0, 0x8C4A38u}, {0xC30675u, 0, 0x8C4A38u}, {0xC3093Eu, 0, 0x8C4A38u}, {0xC31276u, 0, 0x8C4A38u},
    {0xC31833u, 0, 0x8C4A38u}, {0xC31ABCu, 0, 0x8C4A38u}, {0xC32230u, 0, 0x8C4A38u}, {0xC39DA5u, 0, 0x8C4A38u},
    {0xC3BE6Au, 0, 0x8C4A38u}};

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
    {0xF8E64Eu, 5, 4, 0, 0x54u, 0u, {0x47, 0x89, 0x64, 0x91, 0x54}}, /* stamps */
    {0xF8E7F5u, 5, 4, 0, 0x54u, 0u, {0x41, 0x8B, 0x4C, 0xB9, 0x54}}, /* stamps */
    {0xF8E806u, 8, 4, 4, 0x94u, 0x100u, {0x41, 0x89, 0x84, 0xB9, 0x94, 0x00, 0x00, 0x00}}, /* stamps */
    {0xF8F626u, 7, 3, 4, 0xFFFFFF6Cu, 0xFFFFFF00u, {0x49, 0xC7, 0xC5, 0x6C, 0xFF, 0xFF, 0xFF}}, /* place */
    {0xF8F63Bu, 8, 4, 4, 0x94u, 0x100u, {0x4D, 0x8D, 0xBC, 0x24, 0x94, 0x00, 0x00, 0x00}}, /* place */
    {0xF8F68Du, 4, 3, 0, 0xC0u, 0u, {0x45, 0x8B, 0x67, 0xC0}}, /* place */
    {0xF8F696u, 8, 4, 4, 0xFFFFFF6Cu, 0xFFFFFF00u, {0x43, 0x8B, 0x94, 0x3A, 0x6C, 0xFF, 0xFF, 0xFF}}, /* place */
    {0xF8FB60u, 7, 3, 4, 0x90u, 0xFCu, {0x44, 0x8B, 0xA0, 0x90, 0x00, 0x00, 0x00}}, /* place */
    {0xF8FB67u, 3, 2, 0, 0x50u, 0u, {0x8B, 0x58, 0x50}}, /* place */
    {0xF46CC9u, 4, 3, 1, 0x44u, 0xD0u, {0x48, 0x8D, 0x4E, 0x44}}, /* layers */
    {0xF46E13u, 5, 3, 1, 0x44u, 0xD0u, {0xC6, 0x44, 0x30, 0x44, 0x01}}, /* layers */
    {0xF46EAFu, 5, 4, 1, 0x44u, 0xD0u, {0x44, 0x38, 0x7C, 0x31, 0x44}}, /* layers */
    {0xF46EE3u, 5, 3, 1, 0x44u, 0xD0u, {0xC6, 0x44, 0x31, 0x44, 0x01}}, /* layers */};
/* The four starts reads L48 moves out of disp8 reach: {site, replaced bytes, back, body}. Each body is the replaced code
 * with the starts displacement as a disp32 (no register or flag used beyond the original's). */
static const struct { uint32_t site; uint8_t length; uint8_t pin[7]; uint8_t size; uint8_t body[12]; } kObjectiveStartStubs[4] = {
    {0xF8E64Eu, 5, {0x47, 0x89, 0x64, 0x91, 0x54}, 8, {0x47, 0x89, 0xA4, 0x91, 0x10, 0xFF, 0xFF, 0xFF}},            /* mov [r9+r10*4-0xF0], r12d */
    {0xF8E7F5u, 5, {0x41, 0x8B, 0x4C, 0xB9, 0x54}, 8, {0x41, 0x8B, 0x8C, 0xB9, 0x10, 0xFF, 0xFF, 0xFF}},            /* mov ecx, [r9+rdi*4-0xF0] */
    {0xF8F68Au, 7, {0x45, 0x8B, 0x37, 0x45, 0x8B, 0x67, 0xC0}, 10, {0x45, 0x8B, 0x37, 0x45, 0x8B, 0xA7, 0x10, 0xFE, 0xFF, 0xFF}}, /* mov r14d, [r15]; mov r12d, [r15-0x1F0] */
    {0xF8FB67u, 5, {0x8B, 0x58, 0x50, 0x8B, 0x10}, 8, {0x8B, 0x98, 0x0C, 0xFF, 0xFF, 0xFF, 0x8B, 0x10}}};          /* mov ebx, [rax-0xF4]; mov edx, [rax] */
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
static uint32_t LocationMoved(uint32_t value)
{
    if (value >= DIF_LOCATIONS_A && value < DIF_LOCATIONS_A + 0xE0u) return DIF_LOCATIONS_A_NEW + (value - DIF_LOCATIONS_A);
    return DIF_LOCATIONS_B_NEW + (value - DIF_LOCATIONS_B);
}

/* ---- the checks (larger maps and more objectives need these) ---------------------------------------------------- */

/* Every instruction addressing the block's moved arrays, and their capacities. */
static const char* LocationRefsFailed(uintptr_t game)
{
    if (!DifficultyNeedsBlock()) return NULL;
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
    return NULL;
}

static const char* ObjectiveListFailed(uintptr_t game)
{
    if (!DifficultyNeedsBlock()) return NULL;
    for (int i = 0; i < 74; i++)
    {
        uint8_t bytes[8];
        if (SafeRead(game + kObjectiveListSites[i].rva, bytes, kObjectiveListSites[i].length) ||
            memcmp(bytes, kObjectiveListSites[i].pin, kObjectiveListSites[i].length) != 0)
            return "the map generator's objective list is not the reviewed one (more objectives need it)";
    }
    for (int i = 0; i < 4; i++)
    {
        uint8_t bytes[7];
        if (SafeRead(game + kObjectiveStartStubs[i].site, bytes, kObjectiveStartStubs[i].length) ||
            memcmp(bytes, kObjectiveStartStubs[i].pin, kObjectiveStartStubs[i].length) != 0)
            return "the map generator's objective list is not the reviewed one (more objectives need it)";
    }
    return NULL;
}

/* ---- the install step ------------------------------------------------------------------------------------------- */

/* The block's size, then the locations' references and capacities. */
static const char* MoveLocations(uintptr_t game)
{
    uint32_t size = DIF_BLOCK_NEW, places = DIF_LOCATIONS;
    for (size_t i = 0; i < sizeof kGenerationBlockSize / sizeof kGenerationBlockSize[0]; i++)
        if (ApiWrite(game + kGenerationBlockSize[i].rva + kGenerationBlockSize[i].at, &size, 4)) return "the generation block could not be enlarged";
    for (size_t i = 0; i < sizeof kLocationRefs / sizeof kLocationRefs[0]; i++)
    {
        uint32_t moved = LocationMoved(kLocationRefs[i].value);
        if (ApiWrite(game + kLocationRefs[i].rva + kLocationRefs[i].at, &moved, 4)) return "a location reference could not be moved";
    }
    for (size_t i = 0; i < sizeof kLocationCaps / sizeof kLocationCaps[0]; i++)
        if (ApiWrite(game + kLocationCaps[i].rva + kLocationCaps[i].at, &places, 4)) return "a location capacity could not be raised";
    return NULL;
}

static const char* MoveLocationTypes(uintptr_t game)
{
    uint32_t types = DIF_LOCATION_TYPES_C_NEW, typePlaces = DIF_LOCATION_TYPES;
    for (size_t i = 0; i < sizeof kLocationTypeRefs / sizeof kLocationTypeRefs[0]; i++)
        if (ApiWrite(game + kLocationTypeRefs[i].rva + kLocationTypeRefs[i].at, &types, 4)) return "a location type reference could not be moved";
    for (size_t i = 0; i < sizeof kLocationTypeCaps / sizeof kLocationTypeCaps[0]; i++)
        if (ApiWrite(game + kLocationTypeCaps[i].rva + kLocationTypeCaps[i].at, &typePlaces, 4)) return "a location type capacity could not be raised";
    return NULL;
}

static const char* MoveMinimapAndStamps(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kMinimapRefs / sizeof kMinimapRefs[0]; i++)
    {
        uint32_t moved = MinimapMoved(kMinimapRefs[i].value);
        if (ApiWrite(game + kMinimapRefs[i].rva + kMinimapRefs[i].at, &moved, 4)) return "a minimap reference could not be moved";
    }
    for (size_t i = 0; i < sizeof kMinimapCaps / sizeof kMinimapCaps[0]; i++)
    {
        uint32_t places = kMinimapCaps[i].value * DIF_MINIMAP_SCALE;
        if (ApiWrite(game + kMinimapCaps[i].rva + kMinimapCaps[i].at, &places, 4)) return "a minimap capacity could not be raised";
    }
    uint32_t stamps = DIF_STAMPS_NEW;
    for (size_t i = 0; i < sizeof kStampRefs / sizeof kStampRefs[0]; i++)
        if (ApiWrite(game + kStampRefs[i].rva + kStampRefs[i].at, &stamps, 4)) return "a mission stamp reference could not be moved";
    return NULL;
}

/* The objective list (30 places from now on), the objective and extraction places, the place set and the clusters. */
static const char* MoveObjectivePlaces(uintptr_t game)
{
    uint8_t* starts = AllocateNearGame(game, 0x80);
    if (starts == NULL) return "no memory within 2 GB of game.dll for the objective list's starts reads";
    for (int i = 0; i < 4; i++)
    {
        uint8_t* stub = starts + 0x20 * i;
        memcpy(stub, kObjectiveStartStubs[i].body, kObjectiveStartStubs[i].size);
        stub[kObjectiveStartStubs[i].size] = 0xE9;
        int32_t back = (int32_t)(game + kObjectiveStartStubs[i].site + kObjectiveStartStubs[i].length - (uintptr_t)(stub + kObjectiveStartStubs[i].size + 5));
        memcpy(stub + kObjectiveStartStubs[i].size + 1, &back, 4);
    }
    DWORD startsProtect;
    VirtualProtect(starts, 0x80, PAGE_EXECUTE_READ, &startsProtect);
    FlushInstructionCache(GetCurrentProcess(), starts, 0x80);
    for (int i = 0; i < 74; i++)
        if (kObjectiveListSites[i].width != 0 &&
            ApiWrite(game + kObjectiveListSites[i].rva + kObjectiveListSites[i].at, &kObjectiveListSites[i].value, kObjectiveListSites[i].width))
            return "an objective list reference could not be moved";
    for (int i = 0; i < 4; i++)
    {
        uint8_t jump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
        int32_t to = DifficultyJumpRel(game + kObjectiveStartStubs[i].site, (uintptr_t)(starts + 0x20 * i)); memcpy(jump + 1, &to, 4);
        if (ApiWrite(game + kObjectiveStartStubs[i].site, jump, kObjectiveStartStubs[i].length)) return "an objective list reference could not be moved";
    }
    g_objectivePlaces = DIF_OBJECTIVES_MAX;
    g_objectiveListPlaces = DIF_OBJLIST_PLACES;
    uint32_t placeList = DIF_PLACES_NEW, placeSet = DIF_PLACE_SET_NEW;
    for (size_t i = 0; i < sizeof kPlaceListRefs / sizeof kPlaceListRefs[0]; i++)
        if (ApiWrite(game + kPlaceListRefs[i].rva + kPlaceListRefs[i].at, &placeList, 4)) return "an objective place reference could not be moved";
    for (size_t i = 0; i < sizeof kPlaceSetRefs / sizeof kPlaceSetRefs[0]; i++)
        if (ApiWrite(game + kPlaceSetRefs[i].rva + kPlaceSetRefs[i].at, &placeSet, 4)) return "a place set reference could not be moved";
    uint32_t clusters = DIF_CLUSTERS_NEW, clusterPlaces = DIF_CLUSTERS;
    for (int i = 0; i < 16; i++)
        if (ApiWrite(game + kClusterRefs[i].rva + kClusterRefs[i].at, &clusters, 4) ||
            ApiWrite(game + kClusterCaps[i].rva + kClusterCaps[i].at, &clusterPlaces, 4)) return "a location cluster reference could not be moved";
    return NULL;
}

/* GenerateLocationClusters makes at least one cluster per 24 locations of a kind, when its counts are the reviewed ones. */
static const char* RaiseClusterCounts(uintptr_t game)
{
    uint8_t* counts = AllocateNearGame(game, 0x100);
    uint8_t countPin[9];
    if (counts == NULL || SafeRead(game + kClusterCountSites[0].site, countPin, 9) || memcmp(countPin, kClusterCountPinA, 9) ||
        SafeRead(game + kClusterCountSites[1].site, countPin, 7) || memcmp(countPin, kClusterCountPinB, 7))
    {
        Note("difficulty: the location clusters' count is not the reviewed one: a kind keeps its usual clusters");
        return NULL;
    }
    for (int i = 0; i < 2; i++)
    {
        uint8_t* stub = counts + 0x40 * i;
        memcpy(stub, kClusterCountSites[i].stub, kClusterCountSites[i].size);
        int32_t back = (int32_t)(game + kClusterCountSites[i].back - (uintptr_t)(stub + kClusterCountSites[i].backAt + 4));
        memcpy(stub + kClusterCountSites[i].backAt, &back, 4);
        for (int k = 0; k < 2; k++)
        {
            int32_t rel = (int32_t)((counts + kClusterCountReads[i][k][2]) - (stub + kClusterCountReads[i][k][1]));
            memcpy(stub + kClusterCountReads[i][k][0], &rel, 4);
        }
    }
    uint32_t per24[2] = {23, 36};
    memcpy(counts + 0x80, per24, 8);
    g_clusterPer = (volatile uint32_t*)(counts + 0x80);
    DWORD countProtect;
    VirtualProtect(counts, 0x100, PAGE_EXECUTE_READWRITE, &countProtect); /* +0x80: the per-cluster number, set per level */
    for (int i = 0; i < 2; i++)
    {
        uint8_t jump[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
        int32_t to = DifficultyJumpRel(game + kClusterCountSites[i].site, (uintptr_t)(counts + 0x40 * i)); memcpy(jump + 1, &to, 4);
        if (ApiWrite(game + kClusterCountSites[i].site, jump, kClusterCountSites[i].length)) return "the location clusters' count could not be raised";
    }
    return NULL;
}

/* The important locations' references (DIF_IMPORTANT places from now on), each checked against the dump's value first. */
static const char* MoveImportantLocations(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kImportantRefs / sizeof kImportantRefs[0]; i++)
    {
        uint32_t value = 0;
        if (SafeRead(game + kImportantRefs[i].rva + kImportantRefs[i].at, &value, 4) || value != kImportantRefs[i].value)
            return "an important location reference is not the reviewed one (larger operations need them moved)";
    }
    for (size_t i = 0; i < sizeof kImportantRefs / sizeof kImportantRefs[0]; i++)
    {
        uint32_t moved = DIF_IMPORTANT_NEW + (kImportantRefs[i].value - DIF_IMPORTANT_OLD);
        if (ApiWrite(game + kImportantRefs[i].rva + kImportantRefs[i].at, &moved, 4)) return "an important location reference could not be moved";
    }
    g_importantList = DIF_IMPORTANT_NEW;
    Note("difficulty: the important locations have %u places (%zu instructions)", DIF_IMPORTANT, sizeof kImportantRefs / sizeof kImportantRefs[0]);
    return NULL;
}

/* The linked locations' references (DIF_LINKED places from now on), checked first. */
static const char* MoveLinkedLocations(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kLinkedRefs / sizeof kLinkedRefs[0]; i++)
    {
        uint32_t value = 0;
        if (SafeRead(game + kLinkedRefs[i].rva + kLinkedRefs[i].at, &value, 4) || value != kLinkedRefs[i].value)
            return "a linked location reference is not the reviewed one (larger operations need them moved)";
    }
    for (size_t i = 0; i < sizeof kLinkedRefs / sizeof kLinkedRefs[0]; i++)
    {
        uint32_t moved = DIF_LINKED_NEW + (kLinkedRefs[i].value - DIF_LINKED_OLD);
        if (ApiWrite(game + kLinkedRefs[i].rva + kLinkedRefs[i].at, &moved, 4)) return "a linked location reference could not be moved";
    }
    Note("difficulty: the linked (satellite) locations have %u places (%zu instructions)", DIF_LINKED, sizeof kLinkedRefs / sizeof kLinkedRefs[0]);
    return NULL;
}

/* The route graph's references and capacities (route_sites.h), all checked against the dump's values first. */
static const char* MoveRouteGraph(uintptr_t game)
{
    static const struct { const RouteSite* refs; size_t count; const RouteSite* caps; size_t capCount; uint32_t old, moved, places; } arrays[4] = {
        {kRouteNodeRefs, sizeof kRouteNodeRefs / sizeof kRouteNodeRefs[0], kRouteNodeCaps, sizeof kRouteNodeCaps / sizeof kRouteNodeCaps[0], DIF_ROUTE_NODES_OLD, DIF_ROUTE_NODES_NEW, DIF_ROUTE_NODES},
        {kRouteEdgeRefs, sizeof kRouteEdgeRefs / sizeof kRouteEdgeRefs[0], kRouteEdgeCaps, sizeof kRouteEdgeCaps / sizeof kRouteEdgeCaps[0], DIF_ROUTE_EDGES_OLD, DIF_ROUTE_EDGES_NEW, DIF_ROUTE_EDGES},
        {kRouteClassRefs, sizeof kRouteClassRefs / sizeof kRouteClassRefs[0], kRouteClassCaps, sizeof kRouteClassCaps / sizeof kRouteClassCaps[0], DIF_ROUTE_CLASSES_OLD, DIF_ROUTE_CLASSES_NEW, DIF_ROUTE_EDGES},
        {kRoutePointRefs, sizeof kRoutePointRefs / sizeof kRoutePointRefs[0], kRoutePointCaps, sizeof kRoutePointCaps / sizeof kRoutePointCaps[0], DIF_ROUTE_POINTS_OLD, DIF_ROUTE_POINTS_NEW, DIF_ROUTE_POINTS}};
    for (int a = 0; a < 4; a++)
    {
        for (size_t i = 0; i < arrays[a].count; i++)
        {
            uint32_t value = 0;
            if (SafeRead(game + arrays[a].refs[i].rva + arrays[a].refs[i].at, &value, 4) || value != arrays[a].refs[i].value)
                return "a route graph reference is not the reviewed one (larger maps' cities need it moved)";
        }
        for (size_t i = 0; i < arrays[a].capCount; i++)
        {
            uint32_t value = 0;
            if (SafeRead(game + arrays[a].caps[i].rva + arrays[a].caps[i].at, &value, 4) || value != arrays[a].caps[i].value)
                return "a route graph capacity is not the reviewed one (larger maps' cities need it raised)";
        }
    }
    size_t refs = 0, caps = 0;
    for (int a = 0; a < 4; a++)
    {
        for (size_t i = 0; i < arrays[a].count; i++, refs++)
        {
            uint32_t moved = arrays[a].moved + (arrays[a].refs[i].value - arrays[a].old);
            if (ApiWrite(game + arrays[a].refs[i].rva + arrays[a].refs[i].at, &moved, 4)) return "a route graph reference could not be moved";
        }
        for (size_t i = 0; i < arrays[a].capCount; i++, caps++)
            if (ApiWrite(game + arrays[a].caps[i].rva + arrays[a].caps[i].at, &arrays[a].places, 4)) return "a route graph capacity could not be raised";
    }
    g_routeNodes = DIF_ROUTE_NODES_NEW; g_routeEdges = DIF_ROUTE_EDGES_NEW; g_routePoints = DIF_ROUTE_POINTS_NEW;
    Note("difficulty: the route graph has %u nodes, %u edges and %u road points (%zu instructions, %zu capacities)", DIF_ROUTE_NODES, DIF_ROUTE_EDGES, DIF_ROUTE_POINTS, refs, caps);
    return NULL;
}

/* The generation block a level generator has now, or 0. */
static uintptr_t GenerationBlock(uintptr_t game)
{
    uintptr_t world = 0, levelGen = 0, block = 0;
    if (!SafeRead(game + DIF_WORLD, &world, 8) && world != 0 && !SafeRead(world + 0xAE288, &levelGen, 8) && levelGen != 0)
        SafeRead(levelGen + 0x2C8, &block, 8);
    return block;
}

/* The locations and the mission stamps: the block's size first (blocks made from now on fit the new layout),
 * then the instructions and capacities. Not while a block exists already (it has the old size). */
static const char* MoveGenerationBlock(DifficultyInstall* d)
{
    if (!DifficultyNeedsBlock()) return NULL;
    uintptr_t block = GenerationBlock(d->game);
    if (block != 0)
    {
        Note("difficulty: a generation block exists already (0x%llx): the locations keep 2740 places and the mission stamps 128; larger maps and more objectives may overrun them", (unsigned long long)block);
        return NULL;
    }
    static const char* (*const moves[])(uintptr_t game) = {MoveLocations, MoveLocationTypes, MoveMinimapAndStamps, MoveObjectivePlaces, RaiseClusterCounts,
        MoveImportantLocations, MoveLinkedLocations, MoveRouteGraph};
    for (size_t i = 0; i < sizeof moves / sizeof moves[0]; i++)
    {
        const char* why = moves[i](d->game);
        if (why != NULL) return why;
    }
    Note("difficulty: the level's locations have %u places, their types %u, the mission stamps %u and the objectives %u (block 0x%X bytes; %zu + %zu + %zu instructions, %zu + %zu capacities)",
        DIF_LOCATIONS, DIF_LOCATION_TYPES, DIF_STAMPS, DIF_OBJECTIVES_MAX, DIF_BLOCK_NEW, sizeof kLocationRefs / sizeof kLocationRefs[0], sizeof kLocationTypeRefs / sizeof kLocationTypeRefs[0],
        sizeof kStampRefs / sizeof kStampRefs[0], sizeof kLocationCaps / sizeof kLocationCaps[0], sizeof kLocationTypeCaps / sizeof kLocationTypeCaps[0]);
    return NULL;
}

/* The generation block's size on a larger map (difficulty_world.c's terrain tiles copy it whole). */
static uint32_t WorldBlockSize(void) { return DIF_BLOCK_NEW; }
