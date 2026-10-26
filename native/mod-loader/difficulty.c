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
#define DIF_MAP_SCALE_MAX 2.0f /* the largest map_scale (difficulty_maps.c: what a larger map changes) */

#define DIF_OBJECTIVES_MAX 30u        /* the moved objective list */
#define DIF_OBJECTIVES_OTHER 12u      /* a mission's own objectives beside optional and major ones (up to 5 mandatory,
                                       * the live event's, a partner mission's up to 6) */
#define DIF_RAPID_DELTA 0xF0D6C1FA360B2B05ull     /* Rapid Acquisition's (row 8) */
#define DIF_ERADICATE_DELTA 0xC5DD9A6650C23B4Aull /* the Eradicate game mode's mission delta (rows 7, 40, 56, 65, 110, 120, 145) */
#define DIF_WORLD 0x3326340u /* the world: +0xAE288 its level generator, whose +0x2C8 is G */
#define DIF_ARENA_ALLOCATE 0x173B8D0u  /* (arena, size, alignment) */
#define DIF_MEMSET 0x208AAA0u
#define DIF_BAR_SLOTS 0x598u      /* the bar's widgets: bar + 0x598 + i * 0x910, i < 10 (+0x6038 is another member) */
#define DIF_WIDGET 0x910u
#define DIF_BARS 32u              /* bars given an 11th widget (a bar keeps its own while its memory is reused) */
#define DIF_BAR_SITES 5u          /* the bar's instructions that address a widget (kBarSites, difficulty_ui.c) */

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

static void FieldWatch(uintptr_t field); /* crash_notes.c: the preview diagnostic's watchpoints */
static volatile uint8_t* g_restartWatch; /* +0 the last level that restarted, +8 a count per site, +0x18 pockets left,
                                          * +0x20 the last level initialised, +0x40 the countdown delta registered,
                                          * +0x44 level-11 missions launched with it, +0x48..+0x54 the mission
                                          * card's marker scales, +0x58 the countdown delta's value (its data) */
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

static int LargerMaps(void) { return g_difficulty.mapScale > 1.0f; }  /* a map_scale above 1 */
static int OwnCountdown(void) { return g_difficulty.minutes > 0.0f; } /* mission_minutes set */

/* InstallDifficulty's work in progress (its steps, in difficulty_install.c's order, are in each part's file): the moved
 * table and what follows it, the page of stubs (code) and where each stub sits in it, and a larger map's disk. */
typedef struct DifficultyInstall
{
    uintptr_t game;
    uint8_t* table;                 /* the rows, the band sprites, the distance factors, then the watch */
    uint8_t* widgets;               /* the bars' 11th widgets and their table */
    uint8_t* code;                  /* the page of stubs */
    uint8_t* watch;
    uint8_t* bands;
    uint8_t* distance;
    uint8_t* row;                   /* level 11's */
    uint8_t* extra;                 /* ExtraWidget */
    uint8_t* iconStub;
    uint8_t* hudIconStub;
    uint8_t* iconChecks;
    uint8_t* barStubs[DIF_BAR_SITES];
    uint8_t* objectiveStubs;
    uint8_t* levelStubs;
    uint8_t* markerStubs;
    uint8_t* radiiStub;
    uint8_t* convoyStub;
    uint8_t* spacingStub;
    uint8_t* spacingStub2;
    float* spacingValue;
    float* gridValue;
    uint8_t* gridStub;
    float disk;                     /* a larger map's generated disk, m (else 0) */
    float* cellFactor;              /* x 4, then 0.25 */
    uint8_t* cellStub;
    uint8_t* finalizeStub;
    uint8_t* restartStubs;
    uint8_t* zoneStubs;             /* clear (+0), height (+0x40) */
    uint8_t* launchStub;
    uint8_t* cardTimeStub;
    uint8_t* widgetTimeStub;
    uint8_t* holoStubs;
    uint8_t* cardViewStub;
    float* viewValues;              /* 1.0, 0.17 / 0.19, 0.17 */
    uint8_t* viewStub;
    uint8_t* spawnerStub;
    uint8_t* telemetryStub;
    uint8_t* captureStub;
    uint8_t* unreachableStub;
    uint8_t* structured;            /* revoronoi (+0, +0x40, +0x80), the flags (+0xC0, writes +0x100) */
} DifficultyInstall;

/* Whether a difficulty can be added now (one level, from hellforge_mod_init, with a name, a title and its points in
 * range); logged when not. */
static int DifficultyAddable(HfMod* mod, const HfDifficulty* def, const char* name)
{
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
    return 1;
}

/* Its map and objectives, when the mod's HfDifficulty has them; 1 if refused (logged). */
static int DifficultyMap(HfMod* mod, const char* name, const HfDifficulty* def, Difficulty* d)
{
    if (def->size < offsetof(HfDifficulty, outpost_scale) + sizeof def->outpost_scale) return 0;
    if (!(def->map_scale >= 0.0f && def->map_scale <= DIF_MAP_SCALE_MAX) || !(def->outpost_scale >= 0.0f && def->outpost_scale <= 8.0f) ||
        def->optional_objectives > 32 || def->major_objectives > 8)
    {
        ApiLog(mod, "add_difficulty(%s): map_scale is 0 to 2, outpost_scale 0 to 8, optional_objectives 0 to 32, "
            "major_objectives 0 to 8 (0 keeps level 10's)", name);
        return 1;
    }
    if (def->optional_objectives + def->major_objectives > DIF_OBJECTIVES_MAX - DIF_OBJECTIVES_OTHER)
    {
        ApiLog(mod, "add_difficulty(%s): optional_objectives + major_objectives is at most %u (the map generator places %u objectives, "
            "%u of them the mission's own)", name, DIF_OBJECTIVES_MAX - DIF_OBJECTIVES_OTHER, DIF_OBJECTIVES_MAX, DIF_OBJECTIVES_OTHER);
        return 1;
    }
    d->mapScale = def->map_scale;
    d->outpostScale = def->outpost_scale;
    d->optional = def->optional_objectives;
    d->major = def->major_objectives;
    return 0;
}

/* Its missions' countdown, when the mod's HfDifficulty has it; 1 if refused (logged). */
static int DifficultyMinutes(HfMod* mod, const char* name, const HfDifficulty* def, Difficulty* d)
{
    if (def->size < offsetof(HfDifficulty, mission_minutes) + sizeof def->mission_minutes) return 0;
    if (!(def->mission_minutes == 0.0f || (def->mission_minutes >= 1.0f && def->mission_minutes <= 75.0f)))
    {
        ApiLog(mod, "add_difficulty(%s): mission_minutes is 1 to 75 (0 keeps the mission's own)", name);
        return 1;
    }
    d->minutes = def->mission_minutes;
    return 0;
}

/* Its flags, when the mod's HfDifficulty has them; 1 if refused (logged). */
static int DifficultyFlags(HfMod* mod, const char* name, const HfDifficulty* def, Difficulty* d)
{
    if (def->size < offsetof(HfDifficulty, flags) + sizeof def->flags) return 0;
    uint32_t known = HF_DIFFICULTY_SECOND_PRIMARY | HF_DIFFICULTY_NO_ERADICATE | HF_DIFFICULTY_NO_RAPID_ACQUISITION | HF_DIFFICULTY_RADAR |
        HF_DIFFICULTY_NO_BLITZ | HF_DIFFICULTY_CITY_STRONGHOLDS | HF_DIFFICULTY_CITY_ICBM;
    if (def->flags & ~known)
    {
        ApiLog(mod, "add_difficulty(%s): unknown flags 0x%x", name, def->flags & ~known);
        return 1;
    }
    d->flags = def->flags;
    return 0;
}

/* Its patrols, when the mod's HfDifficulty has them; 1 if refused (logged). */
static int DifficultyPatrols(HfMod* mod, const char* name, const HfDifficulty* def, Difficulty* d)
{
    if (def->size < offsetof(HfDifficulty, patrol_cap) + sizeof def->patrol_cap) return 0;
    if (!(def->patrol_frequency >= 0.0f && def->patrol_frequency <= 4.0f) || !(def->patrol_cap >= 0.0f && def->patrol_cap <= 4.0f))
    {
        ApiLog(mod, "add_difficulty(%s): patrol_frequency and patrol_cap are 0 to 4 (0 keeps level 10's)", name);
        return 1;
    }
    d->patrolFrequency = def->patrol_frequency;
    d->patrolCap = def->patrol_cap;
    return 0;
}

/* Its mission stratagem, when the mod's HfDifficulty has one; 1 if refused (logged). */
static int DifficultyGrant(HfMod* mod, const char* name, const HfDifficulty* def, Difficulty* d)
{
    if (def->size < offsetof(HfDifficulty, mission_stratagem) + sizeof def->mission_stratagem) return 0;
    if (def->mission_stratagem > 0xFFFF)
    {
        ApiLog(mod, "add_difficulty(%s): mission_stratagem is a stratagem type (0 none)", name);
        return 1;
    }
    d->grant = def->mission_stratagem;
    return 0;
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
    if (!DifficultyAddable(mod, def, name)) return 0;
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
    UpperCopy(def->title, upper, sizeof upper);
    d.text[0] = AddText(upper, key, 0x21);
    d.text[1] = AddText("11", key, 0x22); /* the mission's top bar shows the level's number, as level 10's "10" */
    d.encounter = def->encounter_points;
    d.guardforce = def->guardforce_points;
    d.cooldown = def->spawner_cooldown;
    if (def->size >= offsetof(HfDifficulty, icon) + sizeof def->icon && def->icon != NULL && def->icon[0])
        d.icon = Murmur64A(def->icon, strlen(def->icon));
    if (DifficultyMap(mod, name, def, &d) || DifficultyMinutes(mod, name, def, &d) || DifficultyFlags(mod, name, def, &d) ||
        DifficultyPatrols(mod, name, def, &d) || DifficultyGrant(mod, name, def, &d))
        return 0;
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

static int RefuseDifficulty(const char* why)
{
    g_difficultyState = -1;
    Note("difficulty: level 11 refused: %s", why);
    if (g_difficultyAdded) ApiLog(g_difficulty.mod, "difficulty %s not added: %s", g_difficulty.name, why);
    return 1;
}

/* ---- the checks (DifficultySitesFailed, difficulty_install.c, runs them in its order) ---------------------------- */

/* NULL when the table's 19 copies are filled and alike, with the reviewed items and levels; 1 in *waiting while one is
 * not filled yet. */
static const char* DifficultyCopiesFailed(uintptr_t game, int* waiting)
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
    return NULL;
}

/* The instructions addressing the table and the bounds on the levels. */
static const char* DifficultyReadersFailed(uintptr_t game)
{
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
    return NULL;
}

/* The callers of the highest level that keep 10 (kDifficultyKeepTen): calls of 0x11EBB40. */
static const char* KeepTenFailed(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kDifficultyKeepTen / sizeof kDifficultyKeepTen[0]; i++)
    {
        uint8_t call[5];
        int32_t rel;
        if (SafeRead(game + kDifficultyKeepTen[i], call, 5)) return "a caller of the highest level is unreadable";
        memcpy(&rel, call + 1, 4);
        if (call[0] != 0xE8 || kDifficultyKeepTen[i] + 5 + rel != DIF_MAX_FN) return "a caller of the highest level is not the reviewed one";
    }
    return NULL;
}

/* The per-level values' readers: the loads clamped in a stub, those clamped in place, and the HUD's target. */
static const char* LevelReadersFailed(uintptr_t game)
{
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
    uint8_t sib = 0;
    if (SafeRead(game + DIF_HUD_TARGET_SIB, &sib, 1) || sib != 0x8D) return "the HUD's objective target is not the reviewed one";
    return NULL;
}

/* The operations' and missions' difficulty ranges (24 and 120 reach 10) and the objectives' (148 stop at 10). */
static const char* RangesFailed(uintptr_t game)
{
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
    return NULL;
}

/* The mission distance reads, the enemy weight reads and the director's tech level clamp. */
static const char* EnemyReadersFailed(uintptr_t game)
{
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
    return NULL;
}

/* Roster r's eight composition families, those up to tech 10 counted in *toTen. */
static const char* CompositionsFailed(uintptr_t game, size_t r, uint32_t* toTen)
{
    for (uint32_t k = 0; k < 8; k++)
    {
        uint64_t family = 0, families = 0;
        if (SafeRead(game + kRosters[r].descriptor + 0x20 + 0x10 * k, &family, 8) || SafeRead(game + kRosters[r].descriptor + 0x28 + 0x10 * k, &families, 8))
            return "an enemy composition family is unreadable";
        for (uint64_t i = 0; family != 0 && i < families; i++)
        {
            uint32_t highest = 0;
            if (SafeRead(family + i * 0x118 + 0xF0, &highest, 4)) return "an enemy composition is unreadable";
            *toTen += highest == DIF_VANILLA;
        }
    }
    return NULL;
}

/* Roster r: its rows (each with room for an 11th weight) and its compositions. */
static const char* RosterFailed(uintptr_t game, size_t r, uint32_t* toTen)
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
    return CompositionsFailed(game, r, toTen);
}

static const char* RostersFailed(uintptr_t game)
{
    uint32_t compositionsToTen = 0;
    for (size_t r = 0; r < DIF_ROSTERS; r++)
    {
        const char* why = RosterFailed(game, r, &compositionsToTen);
        if (why != NULL) return why;
    }
    if (compositionsToTen != DIF_COMPOSITIONS_UP_TO_TEN) return "the enemy compositions' tech ranges are not the reviewed ones";
    return NULL;
}

/* ---- the install steps (InstallDifficulty, difficulty_install.c, runs them in its order) ------------------------ */

/* The rows and the band sprites; the bars' 11th widgets (zeroed, as the bar's own memory is when it is built) and
 * their table; a page of stubs. */
static const char* AllocateTable(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    size_t bytes = (size_t)DIF_ROWS * DIF_ROW + (DIF_VANILLA + 1) * 8 + (DIF_VANILLA + 1) * 12;
    size_t widgetBytes = (size_t)DIF_BARS * 8 + (size_t)DIF_BARS * DIF_WIDGET;
    d->table = AllocateNearGame(game, bytes + 0x60);
    d->widgets = d->table != NULL ? AllocateNearGame(game, widgetBytes) : NULL;
    d->code = d->widgets != NULL ? AllocateNearGame(game, 0x1000) : NULL;
    if (d->table == NULL || d->widgets == NULL || d->code == NULL) return "no memory within 2 GB of game.dll for the moved table";
    memset(d->table, 0, bytes + 0x60);
    d->watch = d->table + bytes;
    memset(d->widgets, 0, widgetBytes);
    d->bands = d->table + DIF_ROWS * DIF_ROW;
    if (SafeRead(game + DIF_BANDS, d->bands, DIF_VANILLA * 8)) return "the band sprites are unreadable";
    memcpy(d->bands + DIF_VANILLA * 8, d->bands + (DIF_VANILLA - 1) * 8, 8);
    d->distance = d->bands + (DIF_VANILLA + 1) * 8;
    if (SafeRead(game + DIF_DISTANCE, d->distance, DIF_VANILLA * 12)) return "the mission distance factors are unreadable";
    memcpy(d->distance + DIF_VANILLA * 12, d->distance + (DIF_VANILLA - 1) * 12, 12);
    for (int k = 0; g_difficulty.mapScale > 1.0f && k < 3; k++) /* the structured maps' playable radius, per sub-kind */
    {
        float factor;
        memcpy(&factor, d->distance + DIF_VANILLA * 12 + 4 * k, 4);
        factor *= g_difficulty.mapScale;
        memcpy(d->distance + DIF_VANILLA * 12 + 4 * k, &factor, 4);
    }
    return NULL;
}

/* The per-level values' loads: 16 x 0x20, the load, the clamp, back. */
static const char* PrepareLevelStubs(DifficultyInstall* d)
{
    d->levelStubs = d->code + 0xCE0;
    for (int i = 0; i < DIF_LEVEL_LOADS; i++)
    {
        uint8_t* stub = d->levelStubs + 0x20 * i;
        int n = kLevelLoads[i].length;
        memcpy(stub, kLevelLoads[i].pin, n);
        n += LevelClamp(stub + n, kLevelLoads[i].reg);
        stub[n] = 0xE9;
        int32_t rel = (int32_t)(d->game + kLevelLoads[i].rva + kLevelLoads[i].length - (uintptr_t)(stub + n + 5));
        memcpy(stub + n + 1, &rel, 4);
    }
    return NULL;
}

static const char* ProtectStubs(DifficultyInstall* d)
{
    DWORD protect;
    if (!VirtualProtect(d->code, 0x1000, PAGE_EXECUTE_READ, &protect)) return "the bar's stubs could not be made executable";
    FlushInstructionCache(GetCurrentProcess(), d->code, 0x1000);
    return NULL;
}

/* Roaming patrols: per faction (Terminids, Automatons, Illuminate) the
 * interval a channel's timer restarts at (+0x2A0, f32 s; level 10: 65, 78, 75) and the most living patrol enemies
 * (+0x2AC, u32; 41, 39, 48), which the director (0x94EB90) reads from the row every frame. */
static void PatrolLevelRow(uint8_t* row)
{
    for (int f = 0; f < 3; f++)
    {
        float interval;
        uint32_t cap;
        memcpy(&interval, row + 0x2A0 + 4 * f, 4); memcpy(&cap, row + 0x2AC + 4 * f, 4);
        if (g_difficulty.patrolFrequency > 0.0f) { interval /= g_difficulty.patrolFrequency; memcpy(row + 0x2A0 + 4 * f, &interval, 4); }
        if (g_difficulty.patrolCap > 0.0f) { cap = (uint32_t)((float)cap * g_difficulty.patrolCap + 0.5f); memcpy(row + 0x2AC + 4 * f, &cap, 4); }
    }
}

/* Level 11's row: level 10's with the mod's item, text, points, cooldown, patrols, objectives, outposts and map size. */
static const char* MakeLevelRow(DifficultyInstall* d)
{
    if (SafeRead(d->game + kDifficultyCopies[0], d->table, DIF_VANILLA * DIF_ROW)) return "the difficulty table is unreadable";
    uint8_t* row = d->table + DIF_VANILLA * DIF_ROW;
    d->row = row;
    memcpy(row, row - DIF_ROW, DIF_ROW);
    uint32_t level = DIF_VANILLA + 1;
    memcpy(row, &g_difficulty.item, 4);
    memcpy(row + 4, &level, 4);
    if (g_difficulty.text[0] != 0) memcpy(row + 8, &g_difficulty.text[0], 4);
    if (g_difficulty.text[1] != 0) memcpy(row + 0xC, &g_difficulty.text[1], 4);
    if (g_difficulty.encounter > 0.0f) memcpy(row + 0xE4, &g_difficulty.encounter, 4);
    if (g_difficulty.guardforce > 0.0f) memcpy(row + 0xE8, &g_difficulty.guardforce, 4);
    if (g_difficulty.cooldown > 0.0f) memcpy(row + 0x104, &g_difficulty.cooldown, 4);
    PatrolLevelRow(row);
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
    return NULL;
}

/* The callers that keep 10 first, then the instructions (the highest level is still 10: nothing reads row 11
 * yet), then the bounds, the highest level last. */
static const char* MoveTableReaders(DifficultyInstall* d)
{
    for (size_t i = 0; i < sizeof kDifficultyKeepTen / sizeof kDifficultyKeepTen[0]; i++)
        if (ApiWrite(d->game + kDifficultyKeepTen[i], kKeepTen, sizeof kKeepTen))
            return "a caller of the highest level could not keep 10";
    for (size_t i = 0; i < sizeof kDifficultySites / sizeof kDifficultySites[0]; i++)
    {
        int32_t disp = DifficultyDisplacement((uintptr_t)d->table, d->game, &kDifficultySites[i]);
        if (ApiWrite(d->game + kDifficultySites[i].rva + kDifficultySites[i].dispAt, &disp, 4))
        {
            Note("difficulty: could not rewrite the instruction at 0x%x; %zu of %zu moved", kDifficultySites[i].rva, i,
                sizeof kDifficultySites / sizeof kDifficultySites[0]);
            return "an instruction could not be rewritten";
        }
    }
    return NULL;
}

/* Objectives drawn up to level 10 are drawn at 11 too. */
static const char* WidenObjectives(DifficultyInstall* d)
{
    uint32_t level = DIF_VANILLA + 1;
    for (uint32_t i = 0; i < DIF_OBJECTIVE_ROWS; i++)
    {
        uintptr_t at = d->game + DIF_OBJECTIVES + i * DIF_OBJECTIVE_ROW + 0x28;
        uint32_t highest = 0;
        if (!SafeRead(at, &highest, 4) && highest == DIF_VANILLA && ApiWrite(at, &level, 4))
            return "an objective's difficulty range could not be widened";
    }
    return NULL;
}

/* The mission distance factors' readers address the moved table. */
static const char* MoveDistanceReaders(DifficultyInstall* d)
{
    for (size_t i = 0; i < 7; i++)
    {
        int32_t disp = (int32_t)(int64_t)((uintptr_t)d->distance - d->game);
        if (ApiWrite(d->game + kDistanceSites[i][0] + kDistanceSites[i][1], &disp, 4)) return "a mission distance read could not be moved";
    }
    return NULL;
}

/* Enemies: every roster row's weights move up 4 bytes with level 11's (level 10's to begin with) after them, the
 * readers follow, compositions up to tech 10 allow 11, and the director's tech level clamp becomes 11. The readers
 * change right after the rows: the director only reads them while a mission is set up, not aboard the ship. */
static const char* MoveRosterWeights(DifficultyInstall* d)
{
    for (size_t r = 0; r < DIF_ROSTERS; r++)
    {
        uint64_t rows = 0;
        SafeRead(d->game + kRosters[r].descriptor, &rows, 8);
        for (uint32_t i = 0; i < kRosters[r].rows; i++)
        {
            float weights[DIF_VANILLA + 1];
            if (SafeRead(rows + i * 0x80 + 0x30, weights, DIF_VANILLA * 4)) return "an enemy roster row is unreadable";
            weights[DIF_VANILLA] = weights[DIF_VANILLA - 1];
            if (ApiWrite(rows + i * 0x80 + 0x2C, weights, sizeof weights)) return "an enemy roster row could not be given its 11th weight";
        }
    }
    for (size_t i = 0; i < 6; i++)
    {
        uint8_t disp = 0x2C;
        if (ApiWrite(d->game + kWeightReaders[i][0] + kWeightReaders[i][1], &disp, 1)) return "an enemy weight read could not be moved";
    }
    return NULL;
}

static const char* WidenCompositions(DifficultyInstall* d)
{
    uint32_t level = DIF_VANILLA + 1;
    for (size_t r = 0; r < DIF_ROSTERS; r++)
        for (uint32_t k = 0; k < 8; k++)
        {
            uint64_t family = 0, families = 0;
            SafeRead(d->game + kRosters[r].descriptor + 0x20 + 0x10 * k, &family, 8);
            SafeRead(d->game + kRosters[r].descriptor + 0x28 + 0x10 * k, &families, 8);
            for (uint64_t i = 0; family != 0 && i < families; i++)
            {
                uint32_t highest = 0;
                if (!SafeRead(family + i * 0x118 + 0xF0, &highest, 4) && highest == DIF_VANILLA && ApiWrite(family + i * 0x118 + 0xF0, &level, 4))
                    return "an enemy composition's tech range could not be widened";
            }
        }
    if (ApiWrite(d->game + DIF_TECH_CLAMP + 1, &level, 4)) return "the enemy tech level clamp could not be raised";
    return NULL;
}

/* Whether a mission row is one the level leaves out: Eradicate, Rapid Acquisition or Blitz (kind 6) missions. */
static int ExcludedMission(uintptr_t row)
{
    uint64_t delta = 0;
    uint8_t kind = 0;
    return !SafeRead(row + 0x360, &delta, 8) && !SafeRead(row + 0x34, &kind, 1) &&
        (((g_difficulty.flags & HF_DIFFICULTY_NO_ERADICATE) && delta == DIF_ERADICATE_DELTA) ||
         ((g_difficulty.flags & HF_DIFFICULTY_NO_RAPID_ACQUISITION) && delta == DIF_RAPID_DELTA) ||
         ((g_difficulty.flags & HF_DIFFICULTY_NO_BLITZ) && kind == 6)); /* kind 6: Blitz */
}

/* Operations and missions offered up to level 10 are offered at 11 too. */
static const char* WidenMissionRanges(DifficultyInstall* d)
{
    uint32_t level = DIF_VANILLA + 1;
    for (uint32_t i = 0; i < DIF_OPERATION_ROWS + DIF_MISSION_ROWS; i++)
    {
        uintptr_t at = i < DIF_OPERATION_ROWS ? d->game + DIF_OPERATIONS + i * DIF_OPERATION_ROW + 0x10
                                              : d->game + DIF_MISSIONS + (i - DIF_OPERATION_ROWS) * DIF_MISSION_ROW + 0x10;
        uint32_t highest = 0;
        if (i >= DIF_OPERATION_ROWS && ExcludedMission(at - 0x10))
        {
            uint32_t ten = DIF_VANILLA; /* rows allowing up to 100 (city Blitz 35, 140): only 11 lies above 10 */
            if (!SafeRead(at, &highest, 4) && highest > DIF_VANILLA && ApiWrite(at, &ten, 4))
                return "an excluded mission's difficulty range could not be narrowed";
            continue;
        }
        if (!SafeRead(at, &highest, 4) && highest == DIF_VANILLA && ApiWrite(at, &level, 4))
            return "an operation's or mission's difficulty range could not be widened";
    }
    return NULL;
}

/* The per-level values' readers: the loads into their stubs, the others clamped in place, and the HUD's target. */
static const char* JumpToLevelStubs(DifficultyInstall* d)
{
    for (int i = 0; i < DIF_LEVEL_LOADS; i++)
    {
        uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        int32_t rel = (int32_t)((uintptr_t)(d->levelStubs + 0x20 * i) - (d->game + kLevelLoads[i].rva + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(d->game + kLevelLoads[i].rva, jump, kLevelLoads[i].length)) return "a per-level value's reader could not be clamped";
    }
    uint8_t sib = 0x85;
    for (int i = 0; i < 5; i++)
        if (ApiWrite(d->game + kLevelInPlace[i].rva, kLevelInPlace[i].bytes, kLevelInPlace[i].length)) return "a per-level value's reader could not be clamped";
    if (ApiWrite(d->game + DIF_HUD_TARGET_SIB, &sib, 1)) return "the HUD's objective target could not be clamped";
    return NULL;
}

/* The bounds on the levels last (in reverse: the highest level last of all): level 11 can be chosen from now on. */
static const char* WidenDifficultyBounds(uintptr_t game)
{
    uint32_t level = DIF_VANILLA + 1;
    for (size_t i = sizeof kDifficultyBounds / sizeof kDifficultyBounds[0]; i-- > 0;)
        if (ApiWrite(game + kDifficultyBounds[i].rva + kDifficultyBounds[i].immAt, &level, kDifficultyBounds[i].width))
            return "a bound could not be widened";
    return NULL;
}

static void NoteDifficulty(const DifficultyInstall* d)
{
    float points[3];
    memcpy(points, d->row + 0xE4, 8);
    memcpy(points + 2, d->row + 0x104, 4);
    Note("difficulty: the table moved to 0x%llx (%zu instructions, %zu bounds, %zu callers keep 10); level 11 is %s",
        (unsigned long long)(uintptr_t)d->table, sizeof kDifficultySites / sizeof kDifficultySites[0],
        sizeof kDifficultyBounds / sizeof kDifficultyBounds[0], sizeof kDifficultyKeepTen / sizeof kDifficultyKeepTen[0], g_difficulty.name);
    ApiLog(g_difficulty.mod, "difficulty %s is level 11 (unlock item %08x): encounter %.0f, guardforce %.0f, spawner cooldown %.2f s",
        g_difficulty.name, g_difficulty.item, (double)points[0], (double)points[1], (double)points[2]);
    if (g_difficulty.mapScale > 1.0f)
        ApiLog(g_difficulty.mod, "difficulty %s: maps %.2f x the radius (structured maps excepted), generated within %.0f m, route cells %.1f m apart, Voronoi grid %.2f m, route cell areas x %.3f",
            g_difficulty.name, (double)g_difficulty.mapScale, (double)d->disk, (double)*d->spacingValue, (double)*d->gridValue, (double)(d->cellFactor[0] / 4.0f));
}
