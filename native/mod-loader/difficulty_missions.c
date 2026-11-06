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
/* The enemy director's spawners: at mission load the map's 'spawner' locations are appended (0xAD3778) to a 128 x 0x58
 * list in the HiveMind manager (world+0x1FA08, entries +0x82D98, count +0x85998 right after them) without a check: a 2 x
 * map with 5 x outposts had more, the 129th overwrote the count and the next write crashed (0xAD37AB). The list's only
 * reader is GuardForce's builder (0x94A270, its loop 0x94A52D..0x94ABDD; 22 instructions in game.dll use the list, all in
 * the two): for each entry it spawns the AI spawner entity (0xFDC140 at 0x94A96D, host only; that component grows past its
 * 128, 0x534FD1 -> 0x7FB4D0) and adds its position to the GuardForce group of its stamp, so an entry left out is an
 * outpost without its fabricators and, when they were its only members, without a garrison (an operation's three
 * missions on an extra large map: 197 spawners, 27 patrol spawners without a group). The entries go to a list of the
 * loader instead (DIF_SPAWNERS x 0x58; the count stays in the manager, which mission load clears): the append is rebuilt
 * in a stub (key r13, the pose at r14, the type rdi, the unit r15d: the layout 0xAD373F..0xAD37EC stages), and the
 * builder's loop runs with r13 based on that list (its only r13 uses in the loop are the entries and the count; the
 * manager is kept in the stub block and restored for each count check and on leaving). Past DIF_SPAWNERS they are left
 * out (counted in the watch, +0x3C). */
#define DIF_SPAWNERS 1024u
#define DIF_SPAWNER_SITE 0xAD3778u
#define DIF_SPAWNER_DONE 0xAD390Bu
#define DIF_SPAWNER_LOOP_FIRST 0x94A52Du /* cmp [r13+0x85998], r12d; jbe 0x94ABE7 */
#define DIF_SPAWNER_LOOP_NEXT 0x94ABD0u  /* cmp r10d, [r13+0x85998]; jb 0x94A540 */
#define DIF_SPAWNER_LOOP 0x94A540u
#define DIF_SPAWNER_LOOP_EMPTY 0x94ABE7u
#define DIF_SPAWNER_LOOP_DONE 0x94ABDDu
static const uint8_t kSpawnerPin[6] = {0x8B, 0x83, 0x98, 0x59, 0x08, 0x00};
static const uint8_t kSpawnerFirstPin[13] = {0x45, 0x39, 0xA5, 0x98, 0x59, 0x08, 0x00, 0x0F, 0x86, 0xAD, 0x06, 0x00, 0x00};
static const uint8_t kSpawnerNextPin[13] = {0x45, 0x3B, 0x95, 0x98, 0x59, 0x08, 0x00, 0x0F, 0x82, 0x63, 0xF9, 0xFF, 0xFF};
static const uint8_t kSpawnerStub[106] = {
    0x8B, 0x83, 0x98, 0x59, 0x08, 0x00,                               /*   0 mov eax, [rbx+0x85998] (the count) */
    0x3D, 0x00, 0x04, 0x00, 0x00,                                     /*   6 cmp eax, DIF_SPAWNERS */
    0x73, 0x51,                                                       /*  11 jae 94 */
    0x48, 0x6B, 0xC8, 0x58,                                           /*  13 imul rcx, rax, 0x58 */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  17 mov rax, list (imm64 at 19) */
    0x48, 0x01, 0xC1,                                                 /*  27 add rcx, rax */
    0x4C, 0x89, 0x29,                                                 /*  30 mov [rcx], r13 (the key) */
    0x41, 0x0F, 0x10, 0x06, 0x0F, 0x11, 0x41, 0x08,                   /*  33 the pose: [r14..+0x40] -> [rcx+8..+0x48] */
    0x41, 0x0F, 0x10, 0x46, 0x10, 0x0F, 0x11, 0x41, 0x18,
    0x41, 0x0F, 0x10, 0x46, 0x20, 0x0F, 0x11, 0x41, 0x28,
    0x41, 0x0F, 0x10, 0x46, 0x30, 0x0F, 0x11, 0x41, 0x38,
    0x48, 0x89, 0x79, 0x48,                                           /*  68 mov [rcx+0x48], rdi (the type) */
    0x44, 0x89, 0x79, 0x50,                                           /*  72 mov [rcx+0x50], r15d (the unit) */
    0xC7, 0x41, 0x54, 0x00, 0x00, 0x00, 0x00,                         /*  76 mov dword [rcx+0x54], 0 */
    0xFF, 0x83, 0x98, 0x59, 0x08, 0x00,                               /*  83 inc dword [rbx+0x85998] */
    0xE9, 0, 0, 0, 0,                                                 /*  89 jmp 0xAD390B (rel32 at 90) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /*  94 full: lock inc dword [rip+watch+0x3c] (rel32 at 97) */
    0xE9, 0, 0, 0, 0};                                                /* 101 jmp 0xAD390B (rel32 at 102) */
/* In the same block, after kSpawnerStub: at 0x70 the loop's first check, at 0xA0 every next one, the manager at 0xD0. */
/* At 0xE0: each spawner's position goes to its GuardForce group's 16 places (+0x18C, 12 bytes each, count +0x24C right
 * before the alarms' places) without a check (0x94ABAF..0x94ABD0); with every spawner in the list more reach a group, so
 * past 16 the position is left out (counted at 0x120; the spawner entity is still spawned, the record only links the AI
 * spawners within 1 m of its positions). */
#define DIF_SPAWNER_PLACES_SITE 0x94ABAFu
static const uint8_t kSpawnerPlacesPin[33] = {0x41, 0x8B, 0x80, 0x4C, 0x02, 0x00, 0x00, 0x48, 0x83, 0xC0, 0x21, 0x48, 0x8D, 0x0C, 0x40,
                                              0xF2, 0x41, 0x0F, 0x11, 0x24, 0x88, 0x41, 0x89, 0x54, 0x88, 0x08, 0x41, 0xFF, 0x80, 0x4C,
                                              0x02, 0x00, 0x00};
static volatile LONG* g_spawnerPlacesFull;
static const uint8_t kSpawnerFirstStub[35] = {
    0x45, 0x39, 0xA5, 0x98, 0x59, 0x08, 0x00,                         /*   0 cmp [r13+0x85998], r12d */
    0x0F, 0x86, 0, 0, 0, 0,                                           /*   7 jbe 0x94ABE7 (rel32 at 9) */
    0x4C, 0x89, 0x2D, 0, 0, 0, 0,                                     /*  13 mov [rip -> manager], r13 (rel32 at 16) */
    0x49, 0xBD, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  20 mov r13, list - 0x82D98 (imm64 at 22) */
    0xE9, 0, 0, 0, 0};                                                /*  30 jmp 0x94A540 (rel32 at 31) */
static const uint8_t kSpawnerNextStub[36] = {
    0x4C, 0x8B, 0x2D, 0, 0, 0, 0,                                     /*   0 mov r13, [rip -> manager] (rel32 at 3) */
    0x45, 0x3B, 0x95, 0x98, 0x59, 0x08, 0x00,                         /*   7 cmp r10d, [r13+0x85998] */
    0x73, 0x0F,                                                       /*  14 jae 31 */
    0x49, 0xBD, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  16 mov r13, list - 0x82D98 (imm64 at 18) */
    0xE9, 0, 0, 0, 0,                                                 /*  26 jmp 0x94A540 (rel32 at 27) */
    0xE9, 0, 0, 0, 0};                                                /*  31 jmp 0x94ABDD (rel32 at 32) */
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
/* The mission result (0x62EC20 fills a 0x15F0 record built by 0x126CAD0 on its callers' stacks) is the backend's
 * Mission/end report (0x1380960 -> 0x12C2700, body 0x11E9680) and the summary screen's rows (0x1461B60). Its objective
 * loop (0x62F93B..0x62FB59) writes one 0x10 row per objective instance at +0xC8 (32 places: key, objective hash,
 * completed byte, progress), then the live event's row at index = instance count (0x62FC77), all unchecked: a 33rd
 * instance writes over the participants (+0x2C8, 32 x 0x40), first participant 0's account id, extracted flag and loadout.
 * Badges (+0x14C8: {id, amount} x 32, count +0x15C8): up to 11 before the loop, one per instance with a badge (def +0x54),
 * up to 5 after (the 33rd overwrites the count). The body sends 32 rows and 32 participants, fixed loops (0x11E9AE1,
 * 0x11E9E55), so a report cannot hold more than 32 objectives. The loop, its totals and the tail up to 0x62FCB7 are
 * rebuilt in ResultObjectives: as the game writes them while the rows fit; past that the 31 or 32 rows kept are the
 * completed objectives first, then main (class 0), then major and optional (2..4), in the game's order; every instance
 * still counts towards the totals' badges. In the loop badges are appended while fewer than 27 are listed, after it
 * while fewer than 32 (HellForge's backend ignores badges; it counts the completed rows, up to 20). */
#define DIF_RESULT_LOOP 0x62F93Bu     /* mov r14, [objective manager]; xor esi, esi; xor ecx, ecx; mov [rsp+0x38], esi */
#define DIF_RESULT_DONE 0x62FCB7u     /* mov rax, [rsp+0x50]: after the live event's row and badge */
#define DIF_RESULT_ROWS 32u
#define DIF_RESULT_SCAN 256u
static const uint8_t kResultLoopPin[15] = {0x4C, 0x8B, 0x35, 0x5E, 0x74, 0xCF, 0x02, 0x33, 0xF6, 0x33, 0xC9, 0x89, 0x74, 0x24, 0x38};
/* The summary screen's objective panel (screen+0xAAEB0; 0x19C4E70, once per visit of the MissionSummary screen) copies
 * every instance into two 32-place tables (+0x1BD50 classes, +0x1BDD0 {key, objective hash}; the count +0x1BED0 right
 * after them), then the live event, unchecked: the 33rd wrote its key over the count and the next read ended the game
 * (s74p, 0x19C4FFA). It shows 6 main (class 0..1) and 6 major or optional (2..4) objectives, class 5 and above never, and
 * looks rows up by key with rows[i] when none matches. PanelObjectives fills the tables as the game does while they fit;
 * past that it keeps up to 15 of class 0..1 and fills the rest with 2..4, in the game's order (31 places with the live event). */
#define DIF_PANEL_FILL 0x19C4FDFu     /* mov [rsi+0x1bed0], eax; 11 bytes of nop: the manager's count is not 0 */
#define DIF_PANEL_DONE 0x19C5082u     /* xor edi, edi */
static const uint8_t kPanelFillPin[17] = {0x89, 0x86, 0xD0, 0xBE, 0x01, 0x00, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
#define DIF_OBJECTIVE_MANAGER 0x3326DA0u
#define DIF_LIVE_EVENTS 0x33264E0u
#define DIF_NO_INSTANCE 0x3483C34u    /* what 0x5DBAE0 returns past the instances */
#define DIF_NO_ENTITY 0x3483C20u
typedef uint32_t* (*InstanceIdFn)(uintptr_t manager, uint32_t* id, uint32_t index); /* 0x5DBAE0: the i-th in the HUD's order */
typedef uintptr_t (*IdRecordFn)(uint32_t id);                                         /* 0xFD9D40, 0x1255D60 */
typedef uintptr_t (*SettingsFn)(uintptr_t key);                                       /* 0x4FA880 */
typedef float (*ProgressFn)(uintptr_t unused, uint32_t id);                           /* 0x5DD140 (edx only) */
static uintptr_t g_resultGame;
static struct { InstanceIdFn instanceId; IdRecordFn record, objective; SettingsFn settings; ProgressFn progress; } g_resultFns;
static volatile LONG g_resultCuts, g_panelCuts;
/* thunks: mov rcx, rdi | rsi; mov rdx, rsp | r14; sub rsp, 0x20; mov rax, fn; call rax; add rsp, 0x20; jmp back */
static const uint8_t kResultThunk[31] = {0x48, 0x89, 0xF9, 0x48, 0x89, 0xE2, 0x48, 0x83, 0xEC, 0x20, 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
    0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0xE9, 0, 0, 0, 0};
static const uint8_t kPanelThunk[31] = {0x48, 0x89, 0xF1, 0x4C, 0x89, 0xF2, 0x48, 0x83, 0xEC, 0x20, 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
    0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0xE9, 0, 0, 0, 0};
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
/* 1 when the extra large world groups the HUD's class 0 and 1 rows by id (difficulty_world.c HudGroupInstall): the
 * builder's 16-entry stack list no longer bounds the instances, only the 16 rows (distinct ids) do, so an operation's
 * missions on one map may hold every mission's main objectives and steps. */
static int g_hudGrouped;
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
static uint32_t MixPartner(uint8_t* inputs, uint32_t count, uint32_t* ids, int32_t* classes, uint32_t layout, uint32_t* mixedWith, uint32_t* mixed);
static uint32_t MixMission(uint8_t* inputs, uint32_t count, uint32_t* ids, int32_t* classes, uint32_t layout)
{
    if (inputs[0x14] == DIF_MIX_MARK) { inputs[0x15] = (uint8_t)layout; inputs[0x16] = 1; return count; }
    uint32_t partners[DIF_MIX_PARTNERS], mixed = 0, base;
    memcpy(&base, inputs + 8, 4);
    count = MixPartner(inputs, count, ids, classes, layout, partners, &mixed);
    count = RadarKeep(inputs, count, ids, classes, layout);
    for (uint32_t p = 0; p < mixed; p++) MixLog(base, partners[p], layout, ids, classes, count);
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
        /* each mission holds the definition's most uses: on an operation's missions on one map (grouped HUD) the map
         * holds missions x that (a step two missions share, e.g. fd5054da of rows 119 and 118, stays each mission's) */
        uint32_t per = g_hudGrouped && g_difficulty.missions > 1 ? g_difficulty.missions : 1u;
        if (def && !SafeRead(def + 0x20, &most, 4) && most > 0 && uses >= most * per) continue;
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

/* The distinct ids of the mission's main objectives and steps so far plus the kept ones (the grouped HUD's rows). */
static uint32_t MandatoryRows(const MixCall* call, const uint32_t* keep, uint32_t kept)
{
    uint32_t seen[64], n = 0;
    for (uint32_t i = 0; i < call->count + kept; i++)
    {
        uint32_t id = i < call->count ? call->ids[i] : keep[i - call->count];
        if (id == DIF_LIVE_EVENT_OBJECTIVE || (i < call->count && RowClass(call->baseRow, id) > 1)) continue;
        uint32_t j = 0;
        while (j < n && seen[j] != id) j++;
        if (j == n && n < 64) seen[n++] = id;
    }
    return n;
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
    if (g_hudGrouped ? MandatoryRows(call, keep, kept) > DIF_MANDATORY_MAX : MandatoryCount(call) + kept > DIF_MANDATORY_MAX) return MIX_NEXT_PARTNER;
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

/* One partner per extra mission: two missions in one mixes one, an operation's missions on one map (operation_missions)
 * one per mission past the first, each a different partner (mixedWith, *mixed). */
static uint32_t MixPartner(uint8_t* inputs, uint32_t count, uint32_t* ids, int32_t* classes, uint32_t layout, uint32_t* mixedWith, uint32_t* mixed)
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
    /* a level-11 mission mixes one partner (its second primary); an operation's missions on one map one per mission
     * past the first, and with the grouped HUD every mission's second primary too: 2 x missions - 1 */
    uint32_t wanted = g_difficulty.missions > 1 ? (g_hudGrouped ? 2 * g_difficulty.missions - 1 : g_difficulty.missions - 1) : 1;
    uint32_t start = (uint32_t)(MixHash(in[0], base, in[3]) >> 32) % n;
    for (uint32_t t = 0; t < n && *mixed < wanted; t++)
    {
        uint32_t partner = candidates[(start + t) % n];
        uint32_t result = MixWithPartner(&call, partner);
        if (result != MIX_NEXT_PARTNER) mixedWith[(*mixed)++] = partner;
    }
    if (*mixed < wanted) InterlockedIncrement(&g_secondPrimaryMisses);
    return call.count;
}

/* ---- the mission result and the summary panel ------------------------------------------------------------------- */

static void ResultBadge(uint8_t* result, uint32_t id, float amount, int32_t limit)
{
    int32_t count;
    memcpy(&count, result + 0x15C8, 4);
    if (count < 0 || count >= limit) { InterlockedIncrement(&g_resultCuts); return; }
    memcpy(result + 0x14C8 + 8 * count + 4, &amount, 4);
    memcpy(result + 0x14C8 + 8 * count, &id, 4);
    count++;
    memcpy(result + 0x15C8, &count, 4);
}

/* The manager's instance index of an entity (0x62F9C8..0x62FA4E): open addressing over {entity, index} at +0x38, +0x40
 * places (a power of two), +0x44 the empty key, +0x48 the multiplier; -1 when absent. */
static int64_t ResultInstanceIndex(uintptr_t game, uintptr_t manager, uint32_t id)
{
    if (id == *(const uint32_t*)(game + DIF_NO_ENTITY)) return -1;
    uint32_t places = *(const uint32_t*)(manager + 0x40), empty = *(const uint32_t*)(manager + 0x44);
    uint32_t hash = *(const uint32_t*)(manager + 0x48) * id;
    const uint32_t* table = *(const uint32_t* const*)(manager + 0x38);
    for (uint32_t c = 0; c < places && table != NULL; c++)
    {
        uint32_t slot = (c + hash) & (places - 1);
        if (table[2 * slot] == id) return table[2 * slot + 1] == 0xFFFFFFFFu ? -1 : (int64_t)table[2 * slot + 1];
        if (table[2 * slot] == empty) return -1;
    }
    return -1;
}

typedef struct { uint32_t key, hash; float progress; uint8_t valid, done, rank, kept; } ResultRow;

/* The objective loop and the tail of 0x62EC20 (from 0x62F93B to 0x62FCB7); frame is its rsp ([+0x31] the extraction
 * flag, [+0x3C] its count, [+0x4C] the live event's key when its entity has no record). xmm8 there is 0.0 (0x62EEB4). */
static void ResultObjectives(uint8_t* result, uint8_t* frame)
{
    uintptr_t game = g_resultGame;
    InstanceIdFn instanceId = g_resultFns.instanceId;
    IdRecordFn record = g_resultFns.record, objective = g_resultFns.objective;
    SettingsFn settings = g_resultFns.settings;
    ProgressFn progress = g_resultFns.progress;
    uintptr_t manager = *(const uintptr_t*)(game + DIF_OBJECTIVE_MANAGER);
    uint32_t none = *(const uint32_t*)(game + DIF_NO_INSTANCE);
    uintptr_t events = *(const uintptr_t*)(game + DIF_LIVE_EVENTS);
    int live = events != 0 && *(const uint32_t*)(events + 0x24) > 0;
    ResultRow rows[DIF_RESULT_SCAN];
    uint32_t count = *(const uint32_t*)(manager + 0x24), mainDone = 0, othersDone = 0, scanned = 0;
    float mainSum = 0.0f, othersSum = 0.0f;
    for (uint32_t i = 0; i < count; i++)
    {
        ResultRow row = {0};
        uint32_t id = 0;
        instanceId(manager, &id, i);
        manager = *(const uintptr_t*)(game + DIF_OBJECTIVE_MANAGER);
        if (id != none)
        {
            uintptr_t rec = record(id);
            uintptr_t set = rec != 0 ? settings(*(const uintptr_t*)rec) : 0;
            int64_t index = ResultInstanceIndex(game, manager, id);
            if (set != 0 && index >= 0) /* the game reads both unchecked */
            {
                uintptr_t instance = *(const uintptr_t*)(manager + 0x60) + (uintptr_t)index * 0x1078u;
                uintptr_t def = objective(id);
                int32_t cls = *(const int32_t*)(set + 0xAC0);
                if (cls == 5) cls = *(const int32_t*)(instance + 0x1038);
                int32_t state = *(const int32_t*)(instance + 0x1018);
                uint32_t badge = *(const uint32_t*)(def + 0x54);
                row.valid = 1;
                row.key = *(const uint32_t*)(rec + 0x10);
                row.hash = *(const uint32_t*)def;
                row.done = state == 2;
                if (badge != 0)
                {
                    float amount = progress(0, id) * (float)*(const uint32_t*)(result + 0x54);
                    if (amount > 0.0f) ResultBadge(result, badge, amount, 27);
                }
                else if (state == 2)
                {
                    if (cls == 0) mainDone = 1;
                    else if ((uint32_t)(cls - 2) <= 2u) othersDone++;
                }
                else if (*(const uint8_t*)(set + 0xABD) != 0)
                {
                    row.progress = progress(0, id);
                    if (row.progress > 0.0f)
                    {
                        int32_t own = *(const int32_t*)(instance + 0x1038);
                        if (own == 0) mainSum += row.progress;
                        else if ((uint32_t)(own - 2) <= 1u) othersSum += row.progress;
                    }
                }
                row.rank = (uint8_t)(row.done ? 0 : cls == 0 ? 1 : (uint32_t)(cls - 2) <= 2u ? 2 : 3);
            }
        }
        if (scanned < DIF_RESULT_SCAN) rows[scanned++] = row;
        count = *(const uint32_t*)(manager + 0x24);
    }
    uint32_t next = 0;
    if (count + (uint32_t)live <= DIF_RESULT_ROWS)
    {
        for (uint32_t i = 0; i < scanned; i++)
        {
            uint8_t* to = result + 0xC8 + 0x10 * i;
            memcpy(to + 0xC, &rows[i].progress, 4);
            if (!rows[i].valid) continue;
            memcpy(to, &rows[i].key, 4); memcpy(to + 4, &rows[i].hash, 4); to[8] = rows[i].done;
        }
        next = count;
    }
    else
    {
        uint32_t keep = DIF_RESULT_ROWS - (uint32_t)live, kept = 0;
        for (uint8_t rank = 0; rank < 4 && kept < keep; rank++)
            for (uint32_t i = 0; i < scanned && kept < keep; i++)
                if (rows[i].valid && rows[i].rank == rank) { rows[i].kept = 1; kept++; }
        for (uint32_t i = 0; i < scanned; i++)
        {
            if (!rows[i].kept) continue;
            uint8_t* to = result + 0xC8 + 0x10 * next++;
            memcpy(to, &rows[i].key, 4); memcpy(to + 4, &rows[i].hash, 4); to[8] = rows[i].done; memcpy(to + 0xC, &rows[i].progress, 4);
        }
        if (InterlockedIncrement(&g_resultCuts) == 1)
            Note("difficulty: the mission result lists %u of %u objective instances (its report holds 32 rows; the completed first)", next, count);
    }
    float total = (float)mainDone + mainSum, most = *(const float*)(game + 0x23C6D70u);
    if (total > most) total = most;
    if (total > 0.0f) ResultBadge(result, 0x34FFE656u, total, 32);
    if (othersDone != 0 && (float)othersDone + othersSum > 0.0f) ResultBadge(result, 0x6833DD7Au, (float)othersDone + othersSum, 32);
    if (frame[0x31] != 0)
    {
        ResultBadge(result, 0x60D87AD4u, 1.0f, 32);
        float extracted = (float)*(const uint32_t*)(frame + 0x3C);
        if (extracted > 0.0f) ResultBadge(result, 0xECD665ACu, extracted, 32);
    }
    if (live)
    {
        uint32_t id = 0;
        instanceId(manager, &id, *(const uint32_t*)(manager + 0x24));
        uintptr_t rec = record(id);
        if (rec != 0) memcpy(frame + 0x4C, (const void*)(rec + 0x10), 4);
        uint8_t* to = result + 0xC8 + 0x10 * next;
        float one = 1.0f;
        uint32_t hash = DIF_LIVE_EVENT_OBJECTIVE;
        to[8] = 1;
        memcpy(to, frame + 0x4C, 4); memcpy(to + 0xC, &one, 4); memcpy(to + 4, &hash, 4);
        ResultBadge(result, 0xADDD8C40u, 1.0f, 32);
    }
}

/* The summary panel's tables (0x19C4FDF..0x19C5082; the manager's count is not 0 there). */
static void PanelObjectives(uint8_t* panel, uintptr_t manager)
{
    uintptr_t game = g_resultGame;
    IdRecordFn objective = g_resultFns.objective;
    uintptr_t events = *(const uintptr_t*)(game + DIF_LIVE_EVENTS);
    uint32_t live = events != 0 && *(const uint32_t*)(events + 0x24) > 0 ? 1u : 0u;
    uint32_t count = *(const uint32_t*)(manager + 0x24), keep = DIF_RESULT_ROWS - live, n = 0, mainKept = 0, othersKept = 0;
    int fits = count + live <= DIF_RESULT_ROWS;
    const uintptr_t* list = *(const uintptr_t* const*)(manager + 0x50);
    const uint8_t* instances = *(const uint8_t* const*)(manager + 0x60);
    uint32_t* classes = (uint32_t*)(panel + 0x1BD50), *entries = (uint32_t*)(panel + 0x1BDD0);
    uint32_t mainCap = 0;
    for (uint32_t i = 0; i < count && !fits; i++)
        if (list[i] != 0 && *(const uint32_t*)(instances + (size_t)i * 0x1078u + 0x1038u) <= 1) mainCap++;
    if (mainCap > 15) mainCap = 15;
    for (uint32_t i = 0; i < count && n < keep; i++)
    {
        uintptr_t instance = list[i];
        uint32_t cls = *(const uint32_t*)(instances + (size_t)i * 0x1078u + 0x1038u);
        if (instance == 0) continue;
        if (!fits)
        {
            if (cls > 4) continue;
            if (cls <= 1 ? mainKept >= mainCap : othersKept >= keep - mainCap) continue;
            if (cls <= 1) mainKept++; else othersKept++;
        }
        entries[2 * n + 1] = *(const uint32_t*)objective(*(const uint32_t*)(instance + 8));
        entries[2 * n] = *(const uint32_t*)(instance + 0x10);
        classes[n++] = cls;
    }
    if (!fits && InterlockedIncrement(&g_panelCuts) == 1)
        Note("difficulty: the mission summary lists %u of %u objective instances (32 places; it shows 6 + 6)", n, count);
    if (live)
    {
        classes[n] = 2;
        entries[2 * n + 1] = DIF_LIVE_EVENT_OBJECTIVE;
        entries[2 * n] = 0x7FFF;
        n++;
    }
    *(uint32_t*)(panel + 0x1BED0) = n;
}

/* ---- the checks ------------------------------------------------------------------------------------------------- */

/* What more objectives need (a generation block of the new layout): the mission-end telemetry, the selection's end, the
 * HUD's objective list, the mission result's rows and badges, and the summary's objective panel. */
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
    uint8_t sites[17];
    if (SafeRead(game + DIF_RESULT_LOOP, sites, 15) || memcmp(sites, kResultLoopPin, 15) != 0 ||
        SafeRead(game + DIF_PANEL_FILL, sites, 17) || memcmp(sites, kPanelFillPin, 17) != 0)
        return "the mission result's objective rows or the summary's objective panel are not the reviewed ones (more objectives need them)";
    return NULL;
}

/* The selected mission's telemetry, the director's spawner list and the map's marker slots. */
static const char* MissionSitesFailed(uintptr_t game)
{
    uint8_t pin[11];
    if (SafeRead(game + DIF_TELEMETRY_SITE, pin, 5) || memcmp(pin, kTelemetryPin, 5) != 0) return "the selected mission's telemetry is not the reviewed one";
    if (SafeRead(game + DIF_SPAWNER_SITE, pin, 6) || memcmp(pin, kSpawnerPin, 6) != 0) return "the director's spawner list is not the reviewed one";
    uint8_t loop[13];
    if (SafeRead(game + DIF_SPAWNER_LOOP_FIRST, loop, 13) || memcmp(loop, kSpawnerFirstPin, 13) != 0 ||
        SafeRead(game + DIF_SPAWNER_LOOP_NEXT, loop, 13) || memcmp(loop, kSpawnerNextPin, 13) != 0)
        return "GuardForce's spawner loop is not the reviewed one";
    uint8_t places[33];
    if (SafeRead(game + DIF_SPAWNER_PLACES_SITE, places, 33) || memcmp(places, kSpawnerPlacesPin, 33) != 0)
        return "GuardForce's spawner positions are not the reviewed ones";
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
    uint8_t* thunks = objectiveStubs + 0xD0; /* the result's (+0xD0) and the panel's (+0xF0) thunks */
    uint64_t fn = (uint64_t)(uintptr_t)ResultObjectives;
    g_resultGame = game;
    g_resultFns.instanceId = (InstanceIdFn)(game + 0x5DBAE0u);
    g_resultFns.record = (IdRecordFn)(game + 0xFD9D40u);
    g_resultFns.objective = (IdRecordFn)(game + 0x1255D60u);
    g_resultFns.settings = (SettingsFn)(game + 0x4FA880u);
    g_resultFns.progress = (ProgressFn)(game + 0x5DD140u);
    memcpy(thunks, kResultThunk, sizeof kResultThunk); memcpy(thunks + 12, &fn, 8);
    rel = (int32_t)(game + DIF_RESULT_DONE - (uintptr_t)(thunks + 31)); memcpy(thunks + 27, &rel, 4);
    fn = (uint64_t)(uintptr_t)PanelObjectives;
    memcpy(thunks + 0x20, kPanelThunk, sizeof kPanelThunk); memcpy(thunks + 0x20 + 12, &fn, 8);
    rel = (int32_t)(game + DIF_PANEL_DONE - (uintptr_t)(thunks + 0x20 + 31)); memcpy(thunks + 0x20 + 27, &rel, 4);
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
    uint8_t* list = (uint8_t*)VirtualAlloc(NULL, DIF_SPAWNERS * 0x58u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* stub = list != NULL ? AllocateNearGame(d->game, 0x180) : NULL;
    if (stub == NULL) return "no memory for the director's spawner list";
    memset(stub, 0xCC, 0x180);
    d->spawnerStub = stub;
    memcpy(stub, kSpawnerStub, sizeof kSpawnerStub);
    uint64_t at = (uint64_t)(uintptr_t)list; memcpy(stub + 19, &at, 8);
    int32_t rel = (int32_t)(d->game + DIF_SPAWNER_DONE - (uintptr_t)(stub + 94)); memcpy(stub + 90, &rel, 4);
    rel = (int32_t)(d->watch + 0x3C - (stub + 101)); memcpy(stub + 97, &rel, 4);
    rel = (int32_t)(d->game + DIF_SPAWNER_DONE - (uintptr_t)(stub + 106)); memcpy(stub + 102, &rel, 4);
    uint8_t* first = stub + 0x70, *next = stub + 0xA0, *manager = stub + 0xD0;
    uint64_t based = (uint64_t)(uintptr_t)list - 0x82D98u;
    memcpy(first, kSpawnerFirstStub, sizeof kSpawnerFirstStub);
    rel = (int32_t)(d->game + DIF_SPAWNER_LOOP_EMPTY - (uintptr_t)(first + 13)); memcpy(first + 9, &rel, 4);
    rel = (int32_t)(manager - (first + 20)); memcpy(first + 16, &rel, 4);
    memcpy(first + 22, &based, 8);
    rel = (int32_t)(d->game + DIF_SPAWNER_LOOP - (uintptr_t)(first + 35)); memcpy(first + 31, &rel, 4);
    memcpy(next, kSpawnerNextStub, sizeof kSpawnerNextStub);
    rel = (int32_t)(manager - (next + 7)); memcpy(next + 3, &rel, 4);
    memcpy(next + 18, &based, 8);
    rel = (int32_t)(d->game + DIF_SPAWNER_LOOP - (uintptr_t)(next + 31)); memcpy(next + 27, &rel, 4);
    rel = (int32_t)(d->game + DIF_SPAWNER_LOOP_DONE - (uintptr_t)(next + 36)); memcpy(next + 32, &rel, 4);
    uint8_t* places = stub + 0xE0; /* the pin with "cmp eax, 16; jae full" after its first instruction */
    g_spawnerPlacesFull = (volatile LONG*)(stub + 0x120);
    *g_spawnerPlacesFull = 0;
    memcpy(places, kSpawnerPlacesPin, 7);
    static const uint8_t check[5] = {0x83, 0xF8, 0x10, 0x73, 0x1F};
    memcpy(places + 7, check, 5);
    memcpy(places + 12, kSpawnerPlacesPin + 7, 26);
    places[38] = 0xE9; rel = (int32_t)(d->game + DIF_SPAWNER_LOOP_NEXT - (uintptr_t)(places + 43)); memcpy(places + 39, &rel, 4);
    places[43] = 0xF0; places[44] = 0xFF; places[45] = 0x05;      /* full: lock inc dword [rip -> 0x120] */
    rel = (int32_t)((uint8_t*)g_spawnerPlacesFull - (places + 50)); memcpy(places + 46, &rel, 4);
    places[50] = 0xE9; rel = (int32_t)(d->game + DIF_SPAWNER_LOOP_NEXT - (uintptr_t)(places + 55)); memcpy(places + 51, &rel, 4);
    DWORD protect;
    VirtualProtect(stub, 0x180, PAGE_EXECUTE_READWRITE, &protect); /* the manager is written at 0xD0, the count at 0x120 */
    FlushInstructionCache(GetCurrentProcess(), stub, 0x180);
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
    rel = DifficultyJumpRel(game + DIF_GRANT_SITE, (uintptr_t)grant); memcpy(jump + 1, &rel, 4);
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
        rel = DifficultyJumpRel(game + DIF_RADAR_STATE_SITE, (uintptr_t)radar); memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + DIF_RADAR_STATE_SITE, jump, sizeof jump)) return "the radar station's discovered state could not be set";
    }
    RadarInstall(game);
    if (g_difficulty.flags & HF_DIFFICULTY_CITY_ICBM) CityIcbmInstall(game);
    return NULL;
}

static const char* JumpToTelemetry(DifficultyInstall* d)
{
    uint8_t limit[5] = {0xE9};
    int32_t rel = DifficultyJumpRel(d->game + DIF_TELEMETRY_SITE, (uintptr_t)d->telemetryStub);
    memcpy(limit + 1, &rel, 4);
    if (ApiWrite(d->game + DIF_TELEMETRY_SITE, limit, sizeof limit)) return "the selected mission's telemetry could not be limited";
    return NULL;
}

static const char* JumpToSpawners(DifficultyInstall* d)
{
    uint8_t spawners[6] = {0xE9, 0, 0, 0, 0, 0x90};
    int32_t rel = DifficultyJumpRel(d->game + DIF_SPAWNER_SITE, (uintptr_t)d->spawnerStub);
    memcpy(spawners + 1, &rel, 4);
    uint8_t first[13] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, next[13];
    memcpy(next, first, 13);
    rel = DifficultyJumpRel(d->game + DIF_SPAWNER_LOOP_FIRST, (uintptr_t)(d->spawnerStub + 0x70)); memcpy(first + 1, &rel, 4);
    rel = DifficultyJumpRel(d->game + DIF_SPAWNER_LOOP_NEXT, (uintptr_t)(d->spawnerStub + 0xA0)); memcpy(next + 1, &rel, 4);
    /* The append and the builder's loop move together (both before any mission loads); a failed write puts back what
     * was written, so both keep the manager's list. */
    if (ApiWrite(d->game + DIF_SPAWNER_LOOP_FIRST, first, 13)) return "GuardForce's spawner loop could not be moved";
    if (ApiWrite(d->game + DIF_SPAWNER_LOOP_NEXT, next, 13))
    {
        ApiWrite(d->game + DIF_SPAWNER_LOOP_FIRST, kSpawnerFirstPin, 13);
        return "GuardForce's spawner loop could not be moved";
    }
    if (ApiWrite(d->game + DIF_SPAWNER_SITE, spawners, sizeof spawners))
    {
        ApiWrite(d->game + DIF_SPAWNER_LOOP_FIRST, kSpawnerFirstPin, 13);
        ApiWrite(d->game + DIF_SPAWNER_LOOP_NEXT, kSpawnerNextPin, 13);
        return "the director's spawner list could not be moved";
    }
    uint8_t places[33];
    memset(places, 0x90, sizeof places);
    places[0] = 0xE9;
    rel = DifficultyJumpRel(d->game + DIF_SPAWNER_PLACES_SITE, (uintptr_t)(d->spawnerStub + 0xE0)); memcpy(places + 1, &rel, 4);
    if (ApiWrite(d->game + DIF_SPAWNER_PLACES_SITE, places, sizeof places)) return "GuardForce's spawner positions could not be limited";
    return NULL;
}

/* More map markers (scratch markers/markers.md, 2026-11-04). The map object (HUD + 0x1A0E28, built by 0x18A5580 at
 * each mission HUD build, updated by 0x18A7C60, destroyed by 0x18A7940; one caller each) holds 16 objective marker slots
 * (+0xF958, 0x2B8 each: two widgets built in place by 0x18AA200, a highlight index, an edge flag), their ids (+0x124D8,
 * u32[16]) and 10 trackers (+0x12518, 0x158 each); the per-frame updater 0x18B42F0 hands them out to objective
 * instances, then objective-linked entities, then the route marker, and leaves the instance pass at 16 slots or 10
 * trackers. Every access is the map register plus a 32-bit displacement (or the counts 16 / 10 as immediates), so
 * the three arrays move to a loader buffer within 2 GB of the map object by rewriting those values in place: the
 * game's own builder, updater, hover pick, highlight pass and destructor then build, use and destroy
 * DIF_MARKERS slots and DIF_MARKER_TRACKERS trackers. It happens at the builder's entry (rcx = the map object, nothing
 * built yet), again if the map object is ever somewhere else; the loader's guards (DIF_MARKER_SLOTS) follow. */
#define DIF_MARKERS 64u
#define DIF_MARKER_TRACKERS 32u
#define DIF_MARKER_BUILDER 0x18A5580u
typedef struct { uint32_t site; uint8_t length, at, kind; uint8_t pin[10]; } MarkerSite;
enum { MK_SLOT, MK_ID, MK_TRACKER, MK_N, MK_NT, MK_N8, MK_NT8, MK_N1, MK_NT1, MK_LAST_SLOT, MK_LAST_TRACKER };
static const MarkerSite kMarkerSites[] = {
    {0x18A60BBu, 7, 3, MK_SLOT, {0x49, 0x8D, 0x96, 0x58, 0xF9, 0x00, 0x00}},
    {0x18AAE1Du, 7, 3, MK_SLOT, {0x4D, 0x8D, 0x88, 0xB4, 0xFA, 0x00, 0x00}},
    {0x18B455Cu, 6, 2, MK_SLOT, {0x48, 0x05, 0xB0, 0xFA, 0x00, 0x00}},
    {0x18B46B9u, 9, 4, MK_SLOT, {0x42, 0xC6, 0x84, 0x2E, 0x0C, 0xFC, 0x00, 0x00, 0x00}},
    {0x18B46F1u, 7, 3, MK_SLOT, {0x48, 0x8D, 0xBE, 0xB0, 0xFA, 0x00, 0x00}},
    {0x18B470Fu, 7, 3, MK_SLOT, {0x4C, 0x8D, 0xA6, 0x58, 0xF9, 0x00, 0x00}},
    {0x18B48C6u, 10, 6, MK_SLOT, {0xF3, 0x42, 0x0F, 0x10, 0x8C, 0x2E, 0x9C, 0xF9, 0x00, 0x00}},
    {0x18B49EBu, 8, 4, MK_SLOT, {0x42, 0x88, 0x84, 0x2E, 0x0C, 0xFC, 0x00, 0x00}},
    {0x18B49F3u, 7, 3, MK_SLOT, {0x4C, 0x8D, 0xA6, 0x58, 0xF9, 0x00, 0x00}},
    {0x18B4B1Fu, 7, 3, MK_SLOT, {0x48, 0x81, 0xC1, 0xB0, 0xFA, 0x00, 0x00}},
    {0x18B4B5Bu, 6, 2, MK_SLOT, {0x48, 0x05, 0xB0, 0xFA, 0x00, 0x00}},
    {0x18B52C2u, 7, 3, MK_SLOT, {0x4C, 0x8D, 0xA7, 0xB0, 0xFA, 0x00, 0x00}},
    {0x18B52CCu, 7, 3, MK_SLOT, {0x88, 0x84, 0x3B, 0x0C, 0xFC, 0x00, 0x00}},
    {0x18B59BBu, 7, 3, MK_SLOT, {0x48, 0x81, 0xC2, 0x58, 0xF9, 0x00, 0x00}},
    {0x18B5EEFu, 8, 4, MK_SLOT, {0x49, 0x8D, 0x9C, 0x24, 0xA0, 0xFB, 0x00, 0x00}},
    {0x18B5F10u, 7, 3, MK_SLOT, {0x48, 0x8D, 0x97, 0x08, 0xFC, 0x00, 0x00}},
    {0x18B5F22u, 7, 3, MK_SLOT, {0x48, 0x8D, 0x8F, 0x58, 0xF9, 0x00, 0x00}},
    {0x18B5F35u, 7, 3, MK_SLOT, {0x48, 0x8D, 0x8F, 0xB0, 0xFA, 0x00, 0x00}},
    {0x18B6114u, 8, 4, MK_SLOT, {0x42, 0x88, 0x84, 0x37, 0x0C, 0xFC, 0x00, 0x00}},
    {0x18B6167u, 7, 3, MK_SLOT, {0x49, 0x81, 0xC6, 0xB0, 0xFA, 0x00, 0x00}},
    {0x18B61C0u, 7, 3, MK_SLOT, {0x48, 0x81, 0xC7, 0x58, 0xF9, 0x00, 0x00}},
    {0x18B7732u, 7, 3, MK_SLOT, {0x48, 0x8D, 0x93, 0x58, 0xF9, 0x00, 0x00}},
    {0x18B471Du, 7, 3, MK_ID, {0x89, 0x84, 0x9E, 0xD8, 0x24, 0x01, 0x00}},
    {0x18B4CF8u, 7, 3, MK_ID, {0x89, 0xB4, 0x97, 0xD8, 0x24, 0x01, 0x00}},
    {0x18B6137u, 8, 4, MK_ID, {0x43, 0x89, 0x84, 0xBE, 0xD8, 0x24, 0x01, 0x00}},
    {0x18AAFC7u, 8, 4, MK_ID, {0x41, 0x8B, 0x8C, 0x88, 0xD8, 0x24, 0x01, 0x00}},
    {0x18A7783u, 7, 3, MK_ID, {0x0F, 0x11, 0x87, 0xD8, 0x24, 0x01, 0x00}},
    {0x18A778Au, 7, 3, MK_ID, {0x0F, 0x11, 0x87, 0xE8, 0x24, 0x01, 0x00}},
    {0x18A7791u, 7, 3, MK_ID, {0x0F, 0x11, 0x87, 0xF8, 0x24, 0x01, 0x00}},
    {0x18A7798u, 7, 3, MK_ID, {0x0F, 0x11, 0x87, 0x08, 0x25, 0x01, 0x00}},
    {0x18A6177u, 7, 3, MK_TRACKER, {0x49, 0x8D, 0x96, 0x18, 0x25, 0x01, 0x00}},
    {0x18B50C3u, 7, 3, MK_TRACKER, {0x48, 0x81, 0xC2, 0x18, 0x25, 0x01, 0x00}},
    {0x18B5E89u, 7, 3, MK_TRACKER, {0x48, 0x81, 0xC3, 0x08, 0x26, 0x01, 0x00}},
    {0x18B5EA0u, 7, 3, MK_TRACKER, {0x48, 0x81, 0xC1, 0x18, 0x25, 0x01, 0x00}},
    {0x18B7DCAu, 7, 3, MK_TRACKER, {0x49, 0x8D, 0x9D, 0x18, 0x25, 0x01, 0x00}},
    {0x18A60D7u, 6, 2, MK_N, {0x41, 0xB8, 0x10, 0x00, 0x00, 0x00}},         /* the builder's count */
    {0x18A6195u, 6, 2, MK_NT, {0x41, 0xB8, 0x0A, 0x00, 0x00, 0x00}},        /* the trackers' builder count */
    {0x18B773Cu, 4, 3, MK_N8, {0x45, 0x8D, 0x46, 0x10}},                     /* the highlight pass: lea r8d, [r14+16] (r14 = 0) */
    {0x18B44A3u, 3, 2, MK_N8, {0x83, 0xFB, 0x10}},                           /* the instance pass: cmp ebx, 16 */
    {0x18B5322u, 5, 4, MK_N8, {0x83, 0x7C, 0x24, 0x40, 0x10}},               /* cmp [rsp+0x40], 16 */
    {0x18B44ACu, 3, 2, MK_NT8, {0x83, 0xFF, 0x0A}},                          /* cmp edi, 10 */
    {0x18B532Du, 5, 4, MK_NT8, {0x83, 0x7C, 0x24, 0x6C, 0x0A}},              /* cmp [rsp+0x6c], 10 */
    {0x18A7A9Du, 9, 0, MK_LAST_SLOT, {0x8B, 0xF5, 0x48, 0x8D, 0x9F, 0x78, 0x23, 0x01, 0x00}}, /* destructor: esi = 15, rbx = the last B */
    {0x18A7AD0u, 5, 1, MK_NT1, {0xBB, 0x09, 0x00, 0x00, 0x00}},              /* destructor: ebx = 9 */
    {0x18A7AD5u, 7, 3, MK_LAST_TRACKER, {0x48, 0x8D, 0xB7, 0x30, 0x31, 0x01, 0x00}}};
#define MARKER_SITES (sizeof kMarkerSites / sizeof kMarkerSites[0])
static const uint8_t kMarkerPadding[10] = {0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}; /* 0x18A7AA6, before the loop head */
typedef uint64_t (*MarkerBuildFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static MarkerBuildFn g_markerBuild;
static uintptr_t g_markerMap;
static uint8_t* g_markerBuffer;
static uint8_t* g_markerGuards; /* the loader's two guard stubs (their limit byte at +4 and +0x24) */
static SRWLOCK g_markerLock = SRWLOCK_INIT;
static const char* MarkerSitesFailed(uintptr_t game)
{
    for (size_t i = 0; i < MARKER_SITES; i++)
    {
        uint8_t bytes[10];
        if (SafeRead(game + kMarkerSites[i].site, bytes, kMarkerSites[i].length) || memcmp(bytes, kMarkerSites[i].pin, kMarkerSites[i].length) != 0)
            return "the map's marker slots are not the reviewed ones; the map keeps 16 markers";
    }
    uint8_t pad[10];
    if (SafeRead(game + 0x18A7AA6u, pad, 10) || memcmp(pad, kMarkerPadding, 10) != 0) return "the map's marker destructor is not the reviewed one; the map keeps 16 markers";
    return NULL;
}
/* The slots, ids and trackers at their new place for this map object: 0 when done. */
static int MarkerMove(uintptr_t game, uintptr_t map)
{
    const size_t slots = DIF_MARKERS * 0x2B8u, ids = DIF_MARKERS * 4u, trackers = DIF_MARKER_TRACKERS * 0x158u, bytes = slots + ids + trackers;
    int64_t r = g_markerBuffer != NULL ? (int64_t)((uintptr_t)g_markerBuffer - map) : 0;
    if (g_markerBuffer == NULL || r < -0x7F000000ll || r > 0x7F000000ll)
    {
        g_markerBuffer = FindNearGame(map, bytes, MEM_COMMIT | MEM_RESERVE);
        if (g_markerBuffer == NULL) return 1;
        r = (int64_t)((uintptr_t)g_markerBuffer - map);
        if (r < -0x7F000000ll || r > 0x7F000000ll) return 1;
    }
    const int32_t R = (int32_t)r, Rid = (int32_t)(r + (int64_t)slots), Rt = (int32_t)(r + (int64_t)(slots + ids));
    for (size_t i = 0; i < MARKER_SITES; i++)
    {
        const MarkerSite* m = &kMarkerSites[i];
        uint8_t code[10];
        memcpy(code, m->pin, m->length);
        int32_t old;
        memcpy(&old, m->pin + m->at, 4);
        int32_t v = 0;
        switch (m->kind)
        {
        case MK_SLOT: v = R + (old - 0xF958); break;
        case MK_ID: v = Rid + (old - 0x124D8); break;
        case MK_TRACKER: v = Rt + (old - 0x12518); break;
        case MK_N: v = DIF_MARKERS; break;
        case MK_NT: v = DIF_MARKER_TRACKERS; break;
        case MK_NT1: v = DIF_MARKER_TRACKERS - 1; break;
        case MK_LAST_TRACKER: v = Rt + (int32_t)((DIF_MARKER_TRACKERS - 1) * 0x158u); break;
        }
        if (m->kind == MK_N8 || m->kind == MK_NT8) code[m->at] = (uint8_t)(m->kind == MK_N8 ? DIF_MARKERS : DIF_MARKER_TRACKERS);
        else if (m->kind == MK_LAST_SLOT) continue; /* below, with the padding */
        else memcpy(code + m->at, &v, 4);
        if (ApiWrite(game + m->site, code, m->length)) return 1;
    }
    /* the destructor's slot loop: mov esi, N-1; lea rbx, [rdi + last B]; a 7-byte nop to the loop head 0x18A7AB0 */
    uint8_t destroy[19] = {0xBE, 0, 0, 0, 0, 0x48, 0x8D, 0x9F, 0, 0, 0, 0, 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00};
    int32_t last = DIF_MARKERS - 1, lastB = R + (int32_t)((DIF_MARKERS - 1) * 0x2B8u + 0x158u);
    memcpy(destroy + 1, &last, 4); memcpy(destroy + 8, &lastB, 4);
    if (ApiWrite(game + 0x18A7A9Du, destroy, sizeof destroy)) return 1;
    if (g_markerGuards != NULL)
    {
        DWORD protect;
        VirtualProtect(g_markerGuards, 0x40, PAGE_EXECUTE_READWRITE, &protect);
        g_markerGuards[4] = (uint8_t)DIF_MARKERS; g_markerGuards[0x24] = (uint8_t)DIF_MARKERS;
        VirtualProtect(g_markerGuards, 0x40, protect, &protect);
        FlushInstructionCache(GetCurrentProcess(), g_markerGuards, 0x40);
    }
    return 0;
}
static uint64_t MarkerBuild(uint64_t map, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    AcquireSRWLockExclusive(&g_markerLock);
    if ((uintptr_t)map != g_markerMap && g_difficultyGame != 0)
    {
        uintptr_t was = g_markerMap;
        g_markerMap = (uintptr_t)map;
        if (MarkerMove(g_difficultyGame, (uintptr_t)map))
            Note("difficulty: the map's markers could not move near the map object at 0x%llx; it keeps %s", (unsigned long long)map, was ? "the last place's" : "16");
        else
            Note("difficulty: the map shows up to %u objective markers and %u trackers (moved to 0x%llx, the map object at 0x%llx)", DIF_MARKERS,
                DIF_MARKER_TRACKERS, (unsigned long long)(uintptr_t)g_markerBuffer, (unsigned long long)map);
    }
    ReleaseSRWLockExclusive(&g_markerLock);
    return g_markerBuild(map, b, c, d, e);
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
        int32_t rel = DifficultyJumpRel(d->game + markerSites[i], (uintptr_t)(d->markerStubs + 0x20 * i));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(d->game + markerSites[i], jump, markerLengths[i])) return "the map's marker slots could not be guarded";
    }
    const char* more = MarkerSitesFailed(d->game);
    if (more != NULL) { Note("difficulty: %s", more); return NULL; }
    g_markerGuards = d->markerStubs;
    LoaderHook("the map's markers", d->game + DIF_MARKER_BUILDER, (void*)MarkerBuild, (void**)&g_markerBuild);
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
    const struct { uint32_t site; uint8_t length, at; } jumps[8] = {{DIF_TELEMETRY_COUNT_SITE, 10, 0}, {DIF_TELEMETRY_LOOP_SITE, 11, 0x20},
        {DIF_SELECTION_END, 7, 0x40}, {0x18B4EA0u, 7, 0xA0}, {0x18B4EB4u, 7, 0xB0}, {0x18B4F00u, 8, 0xC0}, {DIF_RESULT_LOOP, 15, 0xD0},
        {DIF_PANEL_FILL, 17, 0xF0}};
    for (int i = 0; i < 8; i++)
    {
        uint8_t jump[17];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        int32_t rel = DifficultyJumpRel(d->game + jumps[i].site, (uintptr_t)(d->objectiveStubs + jumps[i].at));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(d->game + jumps[i].site, jump, jumps[i].length)) return "the objective selection or mission-end telemetry could not be extended";
    }
    return NULL;
}
