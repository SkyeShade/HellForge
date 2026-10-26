/* Level 11's missions (difficulty.c): more objectives (the selection, the telemetry, the HUD's list, the map's marker
 * slots, the badges), two missions in one, the radar station, the city ICBM missions, the mission stratagem and the
 * director's spawner list. Included by hellforge_modloader.c after difficulty_ui.c. */

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

/* The mission rows as MixInstall reads them: their objective entries (32 x 0x18: +0 id, +4 weight, +0xC class), ids
 * and factions, whether each is a standard mission (a base, or a partner) or an Eradicate one (a partner only), and
 * which operations list which missions. */
typedef struct MixRows
{
    uint8_t (*entries)[32 * 0x18];
    uint32_t ids[DIF_MISSION_ROWS];
    uint8_t faction[DIF_MISSION_ROWS], standard[DIF_MISSION_ROWS], eradicate[DIF_MISSION_ROWS];
    uint8_t (*listed)[DIF_MISSION_ROWS];
} MixRows;

static void ReadMixRows(uintptr_t game, MixRows* rows)
{
    for (uint32_t r = 0; r < DIF_MISSION_ROWS; r++)
    {
        uintptr_t row = game + DIF_MISSIONS + r * DIF_MISSION_ROW;
        uint8_t head[0x38], kind = 0, enabled = 0;
        uint64_t delta = 1;
        uint32_t range[2] = {0, 0};
        rows->standard[r] = 0; rows->eradicate[r] = 0; rows->ids[r] = 0; rows->faction[r] = 0;
        if (SafeRead(row, head, sizeof head) || SafeRead(row + 0x40, rows->entries[r], sizeof rows->entries[r]) || SafeRead(row + 0x360, &delta, 8)) continue;
        memcpy(&rows->ids[r], head, 4); enabled = head[4]; rows->faction[r] = head[8]; memcpy(range, head + 0xC, 8); kind = head[0x34];
        rows->standard[r] = kind == 1 && delta == 0 && enabled && range[0] <= DIF_VANILLA + 1 && DIF_VANILLA + 1 <= range[1];
        /* Eradicate missions are partners only (their kill objective counts kills by itself; the game mode's delta, which
         * shrinks the map, is not part of mixing), offered up to 10 at least; never where the difficulty has no Eradicate
         * (its kill objective would be a main objective all the same). */
        rows->eradicate[r] = delta == DIF_ERADICATE_DELTA && enabled && range[0] <= DIF_VANILLA && DIF_VANILLA <= range[1] &&
                             !(g_difficulty.flags & HF_DIFFICULTY_NO_ERADICATE);
    }
}

/* Which operations list which missions. */
static void ListOperationMissions(uintptr_t game, MixRows* rows)
{
    for (uint32_t o = 0; o < DIF_OPERATION_ROWS; o++)
    {
        uint8_t list[DIF_OPERATION_MISSIONS * 8];
        if (SafeRead(game + DIF_OPERATIONS + o * DIF_OPERATION_ROW + 0x38, list, sizeof list)) continue;
        for (uint32_t k = 0; k < DIF_OPERATION_MISSIONS; k++)
        {
            uint32_t id;
            memcpy(&id, list + 8 * k, 4);
            for (uint32_t r = 0; id != 0 && r < DIF_MISSION_ROWS; r++) if (rows->ids[r] == id) rows->listed[o][r] = 1;
        }
    }
}

/* The class of the first of a row's entries naming id, or -1. */
static int EntryClass(const uint8_t* entries, uint32_t id)
{
    int in = -1;
    for (int j = 0; j < 32 && in < 0; j++)
    {
        uint32_t other, otherClass;
        memcpy(&other, entries + j * 0x18, 4); memcpy(&otherClass, entries + j * 0x18 + 0xC, 4);
        if (other == id) in = (int)otherClass;
    }
    return in;
}

/* Partner row r's entry k against base row b: 1 when they conflict (a main objective of r b lists, or a step of r b
 * lists as more than a step); a main objective offered at the added level counts in *mains. */
static int MixEntryConflicts(uintptr_t game, const MixRows* rows, uint32_t b, uint32_t r, int k, int* mains)
{
    uint32_t id, cls; float weight;
    memcpy(&id, rows->entries[r] + k * 0x18, 4); memcpy(&weight, rows->entries[r] + k * 0x18 + 4, 4); memcpy(&cls, rows->entries[r] + k * 0x18 + 0xC, 4);
    if (id == 0 || !(weight > 0.0f) || cls > 1) return 0;
    int in = EntryClass(rows->entries[b], id);
    if (cls == 0)
    {
        uintptr_t def = ObjectiveDef(game, id);
        uint32_t levels[2] = {0, 0};
        if (in >= 0) return 1;
        if (def && !SafeRead(def + 0x24, levels, 8) && levels[0] <= DIF_VANILLA + 1 && DIF_VANILLA + 1 <= levels[1]) (*mains)++;
        return 0;
    }
    return in >= 0 && in != 1;
}

/* Whether row r partners base row b: a standard or Eradicate mission of b's faction that an operation lists with b,
 * with a main objective offered at the added level, and no conflict with b's entries. */
static int MixPartnerFits(uintptr_t game, const MixRows* rows, uint32_t b, uint32_t r)
{
    if (r == b || !(rows->standard[r] || rows->eradicate[r]) || rows->faction[r] != rows->faction[b]) return 0;
    int together = 0, mains = 0, conflict = 0;
    for (uint32_t o = 0; o < DIF_OPERATION_ROWS; o++) together |= rows->listed[o][b] && rows->listed[o][r];
    if (!together) return 0;
    for (int k = 0; k < 32 && !conflict; k++) conflict = MixEntryConflicts(game, rows, b, r, k, &mains);
    return !conflict && mains > 0;
}

/* Base row b's partners (DIF_MIX_PARTNERS at most), and whether it is a base. */
static void FindMixPartners(uintptr_t game, const MixRows* rows, uint32_t b)
{
    g_mixBase[b] = 0; g_mixPartnerCount[b] = 0;
    if (!rows->standard[b]) return;
    for (uint32_t r = 0; r < DIF_MISSION_ROWS && g_mixPartnerCount[b] < DIF_MIX_PARTNERS; r++)
        if (MixPartnerFits(game, rows, b, r)) g_mixPartners[b][g_mixPartnerCount[b]++] = (uint8_t)r;
    g_mixBase[b] = g_mixPartnerCount[b] > 0;
}

/* Whether a row's entries list id, and the first free (all-zero) one in *free (-1: none). */
static int EntryListed(const uint8_t* entries, uint32_t id, int* free)
{
    int in = 0;
    *free = -1;
    for (int j = 0; j < 32; j++)
    {
        uint32_t other;
        static const uint8_t zero[0x18];
        memcpy(&other, entries + j * 0x18, 4);
        if (other == id) in = 1;
        if (*free < 0 && !memcmp(entries + j * 0x18, zero, 0x18)) *free = j;
    }
    return in;
}

/* Partner row r's steps (class 1, drawn) that base row b does not list, in b's free entries as class 1 of weight 0
 * (never drawn): how many. */
static uint32_t RegisterPartnerSteps(uintptr_t game, MixRows* rows, uint32_t b, uint32_t r)
{
    uint32_t steps = 0;
    for (int k = 0; k < 32; k++)
    {
        uint32_t id, cls; float weight;
        memcpy(&id, rows->entries[r] + k * 0x18, 4); memcpy(&weight, rows->entries[r] + k * 0x18 + 4, 4); memcpy(&cls, rows->entries[r] + k * 0x18 + 0xC, 4);
        if (id == 0 || !(weight > 0.0f) || cls != 1) continue;
        int free;
        if (EntryListed(rows->entries[b], id, &free) || free < 0) continue;
        uint32_t entry[6] = {id, 0, 0, 1, 0, 0}; /* weight 0.0: never drawn */
        if (ApiWrite(game + DIF_MISSIONS + b * DIF_MISSION_ROW + 0x40 + free * 0x18, entry, sizeof entry)) continue;
        memcpy(rows->entries[b] + free * 0x18, entry, sizeof entry);
        steps++;
    }
    return steps;
}

/* Partner lists and the steps' registration, once the ranges are widened. */
static void MixInstall(uintptr_t game)
{
    static uint8_t entries[DIF_MISSION_ROWS][32 * 0x18];
    static uint8_t listed[DIF_OPERATION_ROWS][DIF_MISSION_ROWS];
    MixRows rows;
    rows.entries = entries;
    rows.listed = listed;
    ReadMixRows(game, &rows);
    memset(listed, 0, sizeof listed);
    ListOperationMissions(game, &rows);
    uint32_t bases = 0, steps = 0;
    for (uint32_t b = 0; b < DIF_MISSION_ROWS; b++)
    {
        FindMixPartners(game, &rows, b);
        bases += g_mixBase[b];
    }
    /* The partners' steps, as class 1 (weight 0) in the base rows' free entries. */
    for (uint32_t b = 0; b < DIF_MISSION_ROWS; b++)
        for (uint32_t p = 0; p < g_mixPartnerCount[b]; p++) steps += RegisterPartnerSteps(game, &rows, b, g_mixPartners[b][p]);
    g_select = (SelectFn)(game + DIF_SELECTION);
    Note("difficulty: %u missions can be mixed with a partner mission (%u partner steps registered as steps)", bases, steps);
}

/* Whether the mission row classes the radar station optional (3) and its definition allows the map's layout at the
 * added level. Automaton cities (layout 3) never have one in vanilla; Hell Forge wants it there too (live test).
 * Factory worlds (layout 5) are outside the def's layouts. */
static int RadarAllowed(uintptr_t game, uintptr_t row, uint32_t layout)
{
    uintptr_t def = ObjectiveDef(game, DIF_RADAR);
    uint8_t layouts[8];
    uint32_t levels[2];
    if (RowClass(row, DIF_RADAR) != 3 || def == 0 || layout == 5 || SafeRead(def + 0x98, layouts, 8) ||
        SafeRead(def + 0x24, levels, 8) || levels[0] > DIF_VANILLA + 1 || levels[1] < DIF_VANILLA + 1) return 0;
    int placed = layouts[0] == 0;
    for (int i = 0; i < 8 && layouts[i] != 0; i++) placed |= layouts[i] == layout;
    return placed;
}

/* The radar station kept in a level-11 mission's selection (see DIF_RADAR). */
static uint32_t RadarKeep(uint8_t* inputs, uint32_t count, uint32_t* ids, int32_t* classes, uint32_t layout)
{
    uintptr_t game = g_difficultyGame;
    uint32_t in[4];
    memcpy(in, inputs, sizeof in);
    if (!(g_difficulty.flags & HF_DIFFICULTY_RADAR) || game == 0 || ids == NULL || in[2] >= DIF_MISSION_ROWS || count == 0 || count > 32) return count;
    for (uint32_t i = 0; i < count; i++) if (ids[i] == DIF_RADAR) return count;
    uintptr_t row = game + DIF_MISSIONS + in[2] * DIF_MISSION_ROW;
    if (!RadarAllowed(game, row, layout)) return count;
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

/* A row's entries: whether it lists the radar station (*has) and a drawn optional objective (*optional); its first
 * free entry, or -1. */
static int RadarRowEntries(const uint8_t* entries, int* has, int* optional)
{
    int empty = -1;
    *has = 0;
    *optional = 0;
    for (int k = 0; k < 32; k++)
    {
        uint32_t id, cls; float weight;
        static const uint8_t zero[0x18];
        memcpy(&id, entries + k * 0x18, 4); memcpy(&weight, entries + k * 0x18 + 4, 4); memcpy(&cls, entries + k * 0x18 + 0xC, 4);
        *has |= id == DIF_RADAR;
        *optional |= id != 0 && cls == 3 && weight > 0.0f;
        if (empty < 0 && !memcmp(entries + k * 0x18, zero, 0x18)) empty = k;
    }
    return empty;
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
        int optional, has, empty = RadarRowEntries(entries, &has, &optional);
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
/* A city ICBM mission's entries: gen_icbm x 3 (class 0, min = max = 3), the codes, power and refuelling steps (class 1,
 * 1 each), then the city row's majors and optionals. */
static void CityIcbmEntries(uint8_t* made, const uint8_t* city)
{
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
}

/* Faction f's disabled row made its city Launch ICBM mission at the added level: 1 when it is written. */
static int MakeCityIcbmRow(uintptr_t game, int f)
{
    static uint8_t row[DIF_MISSION_ROW], city[DIF_MISSION_ROW], icbm[DIF_MISSION_ROW];
    uintptr_t at = game + DIF_MISSIONS + kCityIcbmRows[f].row * DIF_MISSION_ROW;
    uint32_t id = 0;
    if (SafeRead(at, row, DIF_MISSION_ROW) || SafeRead(game + DIF_MISSIONS + kCityIcbmRows[f].city * DIF_MISSION_ROW, city, DIF_MISSION_ROW) ||
        SafeRead(game + DIF_MISSIONS + kCityIcbmRows[f].icbm * DIF_MISSION_ROW, icbm, DIF_MISSION_ROW)) return 0;
    memcpy(&id, row, 4);
    if (id != kCityIcbmRows[f].id || row[4] != 0 || memcmp(city + 8, row + 8, 4) != 0 || memcmp(icbm + 8, row + 8, 4) != 0 || city[0x34] != 1)
        return 0; /* not the reviewed rows */
    uint8_t made[DIF_MISSION_ROW];
    memcpy(made, city, sizeof made);
    memcpy(made, &id, 4);
    made[4] = 1;
    uint32_t level = DIF_VANILLA + 1, none = 0;
    memcpy(made + 0xC, &level, 4); memcpy(made + 0x10, &level, 4);
    memcpy(made + 0x28, &none, 4); memcpy(made + 0x2C, &none, 4);
    memcpy(made + 0x340, icbm + 0x340, 0x10); /* name, briefing, media */
    memset(made + 0x378, 0, 8);
    CityIcbmEntries(made, city);
    return !ApiWrite(at, made, sizeof made);
}

/* City operation o's list starts with its faction's ICBM mission (weight 1e30), when that row is made: 1 when written. */
static int ListCityIcbm(uintptr_t game, int o, const int* ready)
{
    int f = kCityIcbmOps[o][1];
    uintptr_t list = game + DIF_OPS + kCityIcbmOps[o][0] * DIF_OP + 0x38;
    uint32_t entries[32][2], cityId = 0, faction = 0, opFaction = 0;
    if (!ready[f] || SafeRead(list, entries, sizeof entries) || SafeRead(game + DIF_MISSIONS + kCityIcbmRows[f].city * DIF_MISSION_ROW, &cityId, 4) ||
        SafeRead(game + DIF_MISSIONS + kCityIcbmRows[f].city * DIF_MISSION_ROW + 8, &faction, 4) ||
        SafeRead(game + DIF_OPS + kCityIcbmOps[o][0] * DIF_OP + 8, &opFaction, 4) || (opFaction & 0xFF) != (faction & 0xFF)) return 0;
    uint32_t used = 0, listed = 0, has = 0;
    while (used < 32 && entries[used][0] != 0) { listed |= entries[used][0] == cityId; has |= entries[used][0] == kCityIcbmRows[f].id; used++; }
    if (has || !listed || used >= 32) return 0; /* already in, or not a reviewed city operation */
    memmove(entries[1], entries[0], used * sizeof entries[0]);
    float first = 1e30f;
    entries[0][0] = kCityIcbmRows[f].id;
    memcpy(&entries[0][1], &first, 4);
    return !ApiWrite(list, entries, (used + 1) * sizeof entries[0]);
}

static void CityIcbmInstall(uintptr_t game)
{
    uint32_t rows = 0, ops = 0;
    int ready[3] = {0};
    for (int f = 0; f < 3; f++)
    {
        ready[f] = MakeCityIcbmRow(game, f);
        rows += ready[f];
    }
    for (int o = 0; o < 7; o++) ops += ListCityIcbm(game, o, ready);
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

/* One end of a base mission's selection at the added level, as MixPartner mixes a partner into it: the selection's
 * inputs (and their first u32s), the objectives so far (ids, classes or NULL, count), the map's layout, the base row. */
typedef struct MixCall
{
    uint8_t* inputs;
    const uint32_t* in;
    uint32_t* ids;
    int32_t* classes;
    uint32_t count, layout;
    uintptr_t game, baseRow;
} MixCall;

#define MIX_NEXT_ATTEMPT UINT32_MAX       /* MixAttempt: another seed */
#define MIX_NEXT_PARTNER (UINT32_MAX - 1) /* MixAttempt, MixWithPartner: another partner */

/* The partner's own selection for the base mission (nested and marked; a derived seed after the first attempt) into
 * got / gotClasses: how many, or MIX_NEXT_ATTEMPT when it is not usable (more than 32, or another layout). */
static uint32_t PartnerSelection(const MixCall* call, uint32_t partner, uint32_t attempt, uint32_t* got, int32_t* gotClasses)
{
    uint8_t nested[0x20];
    uint32_t seed = attempt ? (uint32_t)(MixHash(call->in[0], partner, attempt) >> 32) : call->in[0];
    memcpy(nested, call->inputs, sizeof nested);
    memcpy(nested, &seed, 4); memcpy(nested + 8, &partner, 4);
    nested[0x14] = DIF_MIX_MARK; nested[0x16] = 0;
    uint32_t k = g_select(nested, got, gotClasses);
    if (k > 32 || nested[0x16] == 0 || nested[0x15] != call->layout) return MIX_NEXT_ATTEMPT;
    return k;
}

/* Of the partner's k objectives its main objectives and steps (class 0 and 1) but the live event's, each while the
 * mission has fewer than its definition's most uses: into keep / keepClasses; how many (*mains: of class 0). */
static uint32_t KeepPartnerObjectives(const MixCall* call, const uint32_t* got, const int32_t* gotClasses, uint32_t k,
    uint32_t* keep, uint32_t* keepClasses, uint32_t* mains)
{
    uint32_t kept = 0;
    *mains = 0;
    for (uint32_t i = 0; i < k; i++)
    {
        if ((gotClasses[i] != 0 && gotClasses[i] != 1) || got[i] == DIF_LIVE_EVENT_OBJECTIVE) continue;
        uint32_t uses = 0, most = 0;
        uintptr_t def = ObjectiveDef(call->game, got[i]);
        for (uint32_t j = 0; j < call->count; j++) uses += call->ids[j] == got[i];
        for (uint32_t j = 0; j < kept; j++) uses += keep[j] == got[i];
        if (def && !SafeRead(def + 0x20, &most, 4) && most > 0 && uses >= most) continue;
        keep[kept] = got[i]; keepClasses[kept] = (uint32_t)gotClasses[i]; *mains += gotClasses[i] == 0; kept++;
    }
    return kept;
}

/* The mission's main objectives and steps so far (class 0 or 1 in the base row), but the live event's. */
static uint32_t MandatoryCount(const MixCall* call)
{
    uint32_t mandatory = 0;
    for (uint32_t i = 0; i < call->count; i++)
    {
        int c = RowClass(call->baseRow, call->ids[i]);
        mandatory += call->ids[i] != DIF_LIVE_EVENT_OBJECTIVE && c <= 1;
    }
    return mandatory;
}

/* Room for `kept` more within the objective list's places: the last optional objectives (class 3) go. */
static void MakeObjectiveRoom(MixCall* call, uint32_t kept)
{
    while (call->count + kept > g_objectivePlaces)
    {
        uint32_t at = UINT32_MAX;
        for (uint32_t i = call->count; i-- > 0 && at == UINT32_MAX;) if (RowClass(call->baseRow, call->ids[i]) == 3) at = i;
        if (at == UINT32_MAX) break;
        for (uint32_t i = at; i + 1 < call->count; i++) { call->ids[i] = call->ids[i + 1]; if (call->classes != NULL) call->classes[i] = call->classes[i + 1]; }
        call->count--;
    }
}

/* One attempt at mixing the partner in: the new count, MIX_NEXT_ATTEMPT or MIX_NEXT_PARTNER. */
static uint32_t MixAttempt(MixCall* call, uint32_t partner, uint32_t attempt)
{
    uint32_t got[32] = {0};
    int32_t gotClasses[32] = {0};
    uint32_t k = PartnerSelection(call, partner, attempt, got, gotClasses);
    if (k == MIX_NEXT_ATTEMPT) return MIX_NEXT_ATTEMPT;
    uint32_t keep[32], keepClasses[32], mains;
    uint32_t kept = KeepPartnerObjectives(call, got, gotClasses, k, keep, keepClasses, &mains);
    if (mains == 0) return MIX_NEXT_PARTNER;
    if (MandatoryCount(call) + kept > DIF_MANDATORY_MAX) return MIX_NEXT_PARTNER;
    MakeObjectiveRoom(call, kept);
    if (call->count + kept > g_objectivePlaces) return MIX_NEXT_PARTNER;
    for (uint32_t i = 0; i < kept; i++)
    {
        call->ids[call->count] = keep[i];
        if (call->classes != NULL) call->classes[call->count] = (int32_t)keepClasses[i];
        call->count++;
    }
    InterlockedIncrement(&g_secondPrimaries);
    InterlockedExchangeAdd(&g_mixSteps, (LONG)(kept - mains));
    return call->count;
}

/* Up to 4 attempts with this partner: the new count, or MIX_NEXT_PARTNER. */
static uint32_t MixWithPartner(MixCall* call, uint32_t partner)
{
    for (uint32_t attempt = 0; attempt < 4; attempt++)
    {
        uint32_t result = MixAttempt(call, partner, attempt);
        if (result == MIX_NEXT_PARTNER) break;
        if (result != MIX_NEXT_ATTEMPT) return result;
    }
    return MIX_NEXT_PARTNER;
}

static uint32_t MixPartner(uint8_t* inputs, uint32_t count, uint32_t* ids, int32_t* classes, uint32_t layout, uint32_t* mixedWith)
{
    uintptr_t game = g_difficultyGame;
    uint32_t in[4];
    memcpy(in, inputs, sizeof in);
    uint32_t base = in[2];
    if (!(g_difficulty.flags & HF_DIFFICULTY_SECOND_PRIMARY) || game == 0 || g_select == NULL || ids == NULL || base >= DIF_MISSION_ROWS ||
        !g_mixBase[base] || count == 0 || count >= g_objectivePlaces) return count;
    MixCall call = {inputs, in, ids, classes, count, layout, game, game + DIF_MISSIONS + base * DIF_MISSION_ROW};
    uint8_t candidates[DIF_MIX_PARTNERS];
    uint32_t n = 0;
    for (uint32_t p = 0; p < g_mixPartnerCount[base]; p++)
        if (RowHasLayout(game + DIF_MISSIONS + g_mixPartners[base][p] * DIF_MISSION_ROW, layout)) candidates[n++] = g_mixPartners[base][p];
    if (n == 0) { InterlockedIncrement(&g_secondPrimaryMisses); return count; }
    uint32_t start = (uint32_t)(MixHash(in[0], base, in[3]) >> 32) % n;
    for (uint32_t t = 0; t < n; t++)
    {
        uint32_t partner = candidates[(start + t) % n];
        uint32_t result = MixWithPartner(&call, partner);
        if (result != MIX_NEXT_PARTNER) { *mixedWith = partner; return result; }
    }
    InterlockedIncrement(&g_secondPrimaryMisses);
    return call.count;
}

/* ---- the checks ------------------------------------------------------------------------------------------------- */

/* What more objectives need (a generation block of the new layout): the mission-end telemetry, the selection's end, the
 * HUD's objective list and the mission result's badges. */
static const char* ObjectiveStubSitesFailed(uintptr_t game)
{
    if (!DifficultyNeedsBlock()) return NULL;
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
    return NULL;
}

/* The selected mission's telemetry, the director's spawner list and the map's marker slots. */
static const char* MissionSitesFailed(uintptr_t game)
{
    uint8_t pin[11];
    if (SafeRead(game + DIF_TELEMETRY_SITE, pin, 5) || memcmp(pin, kTelemetryPin, 5) != 0) return "the selected mission's telemetry is not the reviewed one";
    if (SafeRead(game + DIF_SPAWNER_SITE, pin, 6) || memcmp(pin, kSpawnerPin, 6) != 0) return "the director's spawner list is not the reviewed one";
    if (SafeRead(game + DIF_MARKER_LOOP_SITE, pin, 8) || memcmp(pin, kMarkerLoopPin, 8) != 0 ||
        SafeRead(game + DIF_MARKER_ROUTE_SITE, pin, 6) || memcmp(pin, kMarkerRoutePin, 6) != 0)
        return "the map's marker slots are not the reviewed ones";
    return NULL;
}

/* ---- the install steps ------------------------------------------------------------------------------------------ */

/* The objective list's users: the mission-end telemetry's count (+0) and loop (+0x20), the selection's end (+0x40, 78
 * bytes), the HUD list's three accesses (+0xA0..) and the badges (+0xD0). */
static const char* PrepareObjectiveStubs(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    int32_t rel;
    uint8_t* objectiveStubs = d->code + 0xEE0; /* telemetry count (+0), loop (+0x20), selection (+0x40, 78 bytes) */
    d->objectiveStubs = objectiveStubs;
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
    if (list == NULL) return "no memory within 2 GB of game.dll for the HUD's objective list";
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
    return NULL;
}

/* the map's marker slots: cmp [rsp+0x40], 16; jae leave; the replaced instructions; jmp back (0x400 / 0x420) */
static const char* PrepareMarkerStubs(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    d->markerStubs = d->code + 0x400;
    const struct { uint32_t site; const uint8_t* pin; uint8_t length; uint32_t leave; } guards[2] = {
        {DIF_MARKER_LOOP_SITE, kMarkerLoopPin, 8, DIF_MARKER_LOOP_EXIT}, {DIF_MARKER_ROUTE_SITE, kMarkerRoutePin, 6, DIF_MARKER_ROUTE_SKIP}};
    for (int i = 0; i < 2; i++)
    {
        uint8_t* stub = d->markerStubs + 0x20 * i;
        uint8_t n = guards[i].length;
        int32_t rel;
        stub[0] = 0x83; stub[1] = 0x7C; stub[2] = 0x24; stub[3] = 0x40; stub[4] = (uint8_t)DIF_MARKER_SLOTS;
        stub[5] = 0x0F; stub[6] = 0x83;
        rel = (int32_t)(game + guards[i].leave - (uintptr_t)(stub + 11)); memcpy(stub + 7, &rel, 4);
        memcpy(stub + 11, guards[i].pin, n);
        stub[11 + n] = 0xE9;
        rel = (int32_t)(game + guards[i].site + n - (uintptr_t)(stub + 16 + n)); memcpy(stub + 12 + n, &rel, 4);
    }
    return NULL;
}

static const char* PrepareSpawnerStub(DifficultyInstall* d)
{
    uint8_t* spawnerStub = d->code + 0x730;
    d->spawnerStub = spawnerStub;
    memcpy(spawnerStub, kSpawnerStub, sizeof kSpawnerStub);
    int32_t rel = (int32_t)(d->game + DIF_SPAWNER_ADD - (uintptr_t)(spawnerStub + 18)); memcpy(spawnerStub + 14, &rel, 4);
    rel = (int32_t)(d->watch + 0x3C - (spawnerStub + 25)); memcpy(spawnerStub + 21, &rel, 4);
    rel = (int32_t)(d->game + DIF_SPAWNER_DONE - (uintptr_t)(spawnerStub + 30)); memcpy(spawnerStub + 26, &rel, 4);
    return NULL;
}

static const char* PrepareTelemetryStub(DifficultyInstall* d)
{
    uint8_t* telemetryStub = d->code + 0x710;
    d->telemetryStub = telemetryStub;
    memcpy(telemetryStub, kTelemetryStub, sizeof kTelemetryStub);
    int32_t rel = (int32_t)(d->game + DIF_TELEMETRY_SITE + 5 - (uintptr_t)(telemetryStub + 20)); memcpy(telemetryStub + 16, &rel, 4);
    return NULL;
}

/* Two missions in one: the partner lists, once the ranges are widened (HF_DIFFICULTY_SECOND_PRIMARY). */
static const char* MixStep(DifficultyInstall* d)
{
    if (g_difficulty.flags & HF_DIFFICULTY_SECOND_PRIMARY) MixInstall(d->game);
    return NULL;
}

/* The level's type in each mission kind's set's first empty place (sets 1..7): how many sets it was added to. */
static uint32_t GrantSets(uint32_t sets[8][6])
{
    uint32_t added = 0;
    for (int set = 1; set < 8; set++)
        for (int k = 0; k < 6; k++)
        {
            if (sets[set][k] == g_difficulty.grant) break;
            if (sets[set][k] == 0) { sets[set][k] = g_difficulty.grant; added++; break; }
        }
    return added;
}

/* The mission stratagem: the sets' copy (+0x100) the stub (+0) gives the added level. */
static const char* InstallGrant(DifficultyInstall* d)
{
    if (g_difficulty.grant == 0) return NULL;
    uintptr_t game = d->game;
    int32_t rel;
    uint8_t pin[7];
    uint8_t* grant = AllocateNearGame(game, 0x200); /* the stub (+0), the table copy (+0x100) */
    uint32_t sets[8][6];
    if (SafeRead(game + DIF_GRANT_SITE, pin, 7) || memcmp(pin, kGrantPin, 7) != 0 || grant == NULL || SafeRead(game + DIF_GRANTS, sets, sizeof sets))
    {
        ApiLog(g_difficulty.mod, "difficulty %s: the mission stratagems are not the reviewed ones; stratagem %u is not granted", g_difficulty.name, g_difficulty.grant);
        return NULL;
    }
    uint32_t added = GrantSets(sets);
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
    if (ApiWrite(game + DIF_GRANT_SITE, jump, sizeof jump)) return "the mission stratagems could not be extended";
    ApiLog(g_difficulty.mod, "difficulty %s: every mission grants stratagem %u (%u mission kinds)", g_difficulty.name, g_difficulty.grant, added);
    return NULL;
}

/* The radar station (HF_DIFFICULTY_RADAR): discovered from the start, kept in the selection, and the city ICBM missions. */
static const char* InstallRadar(DifficultyInstall* d)
{
    if (!(g_difficulty.flags & HF_DIFFICULTY_RADAR)) return NULL;
    uintptr_t game = d->game;
    uint8_t pin[30];
    uint8_t* radar = AllocateNearGame(game, 0x100);
    if (SafeRead(game + DIF_RADAR_STATE_SITE - 21, pin, 30) || memcmp(pin, kRadarStatePin, 30) != 0 || radar == NULL)
        ApiLog(g_difficulty.mod, "difficulty %s: the objectives' discovered state is not the reviewed one; the radar station is kept but not marked from the start", g_difficulty.name);
    else
    {
        uint64_t world = game + DIF_WORLD;
        memcpy(radar, kRadarStateStub, sizeof kRadarStateStub);
        memcpy(radar + 16, &world, 8);
        int32_t rel = (int32_t)(game + DIF_RADAR_STATE_BACK - (uintptr_t)(radar + 69)); memcpy(radar + 65, &rel, 4);
        DWORD radarProtect;
        VirtualProtect(radar, 0x100, PAGE_EXECUTE_READ, &radarProtect);
        uint8_t jump[9] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
        rel = (int32_t)((uintptr_t)radar - (game + DIF_RADAR_STATE_SITE + 5)); memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + DIF_RADAR_STATE_SITE, jump, sizeof jump)) return "the radar station's discovered state could not be set";
    }
    RadarInstall(game);
    if (g_difficulty.flags & HF_DIFFICULTY_CITY_ICBM) CityIcbmInstall(game);
    return NULL;
}

static const char* JumpToTelemetry(DifficultyInstall* d)
{
    uint8_t limit[5] = {0xE9};
    int32_t rel = (int32_t)((uintptr_t)d->telemetryStub - (d->game + DIF_TELEMETRY_SITE + 5));
    memcpy(limit + 1, &rel, 4);
    if (ApiWrite(d->game + DIF_TELEMETRY_SITE, limit, sizeof limit)) return "the selected mission's telemetry could not be limited";
    return NULL;
}

static const char* JumpToSpawners(DifficultyInstall* d)
{
    uint8_t spawners[6] = {0xE9, 0, 0, 0, 0, 0x90};
    int32_t rel = (int32_t)((uintptr_t)d->spawnerStub - (d->game + DIF_SPAWNER_SITE + 5));
    memcpy(spawners + 1, &rel, 4);
    if (ApiWrite(d->game + DIF_SPAWNER_SITE, spawners, sizeof spawners)) return "the director's spawner list could not be limited";
    return NULL;
}

static const char* JumpToMarkerStubs(DifficultyInstall* d)
{
    const uint32_t markerSites[2] = {DIF_MARKER_LOOP_SITE, DIF_MARKER_ROUTE_SITE};
    const uint8_t markerLengths[2] = {8, 6};
    for (int i = 0; i < 2; i++)
    {
        uint8_t jump[8];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        int32_t rel = (int32_t)((uintptr_t)(d->markerStubs + 0x20 * i) - (d->game + markerSites[i] + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(d->game + markerSites[i], jump, markerLengths[i])) return "the map's marker slots could not be guarded";
    }
    return NULL;
}

/* The objective list's users into their stubs, once the list has its 30 places (the generation block was moved). */
static const char* JumpToObjectiveStubs(DifficultyInstall* d)
{
    if (g_objectivePlaces != DIF_OBJECTIVES_MAX)
    {
        if (g_difficulty.flags & HF_DIFFICULTY_SECOND_PRIMARY)
            ApiLog(g_difficulty.mod, "difficulty %s: the objective list could not grow (a generation block existed already); missions keep one main objective", g_difficulty.name);
        return NULL;
    }
    const struct { uint32_t site; uint8_t length, at; } jumps[7] = {{DIF_TELEMETRY_COUNT_SITE, 10, 0}, {DIF_TELEMETRY_LOOP_SITE, 11, 0x20},
        {DIF_SELECTION_END, 7, 0x40}, {0x18B4EA0u, 7, 0xA0}, {0x18B4EB4u, 7, 0xB0}, {0x18B4F00u, 8, 0xC0}, {DIF_BADGE_SITE, 7, 0xD0}};
    for (int i = 0; i < 7; i++)
    {
        uint8_t jump[11];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        int32_t rel = (int32_t)((uintptr_t)(d->objectiveStubs + jumps[i].at) - (d->game + jumps[i].site + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(d->game + jumps[i].site, jump, jumps[i].length)) return "the objective selection or mission-end telemetry could not be extended";
    }
    return NULL;
}
