/* async-ai (research/async-ai/ASYNC-DESIGN.md), build 25480438: chosen enemy types think with HellForge's brain
 * (ai_brain.c) on a worker thread instead of the game's AI on the game thread. Once a frame, inside the game's AiEnemy
 * pass (0x7E63B0), the game thread applies the intents the worker made from the last snapshot, asks the game's ray
 * batch for the lines of sight they want, takes the next snapshot and wakes the worker. An engaged agent's AiEnemy state
 * handler (0xAA9B20) is skipped; its target, movement, facing and attacks go through the game's own setters, so
 * animation, physics and network replication stay the game's. The game's eye scan is turned off for these agents.
 * Only the machine with authority runs it (the AiEnemy records exist only there). HELLFORGE_ASYNC_AI=off turns it off;
 * a list of resources (hex) picks the types; unset is the Devastator. */

#define AA_AI_PASS 0x7E63B0u      /* game: the AiEnemy manager update (manager, dt) */
#define AA_STATE_SWITCH 0xAA9B20u /* game: its per-entity state handler (?, manager, index, state, dt) */
#define AA_AI_ENEMY 0x33266C8u    /* AiEnemy manager: map +0x2330, count +0x2324, infos +0x2348, records +0x2350 */
#define AA_UNITS 0x3326508u       /* UnitManager: map +0x40, records +0x68 stride 0x308 (+0x2D0 rotation, +0x2E0 position) */
#define AA_PERCEPTION 0x3326548u  /* PerceptionManager: map +0x30, records +0x50 stride 0x13F8 */
#define AA_EYES 0x3326520u        /* SensorEyeManager: +0 the ray batch, +8 its settings (+0xBC the sight filter) */
#define AA_ABILITIES 0x3326640u   /* AbilityManager: map +0x18, records +0x38 stride 0xE0 (+0x10 running) */
#define AA_NAVIGATION 0x3326418u  /* Navigation: map +0x30, records +0x50 stride 0xA8 (+0xC status, +0x10 flags, +0x34 the
                                    * path direction, +0x54 the agent's position for its navigator) */
#define AA_CHARACTERS 0x3326D18u  /* AiCharacter (steering): map +0x30, B +0x58 stride 0x84, C +0x60 stride 0x14 */
#define AA_LOCOMOTION 0x3326CF8u  /* Locomotion: map +0x20, wanted velocity +0x40 stride 0xC */
#define AA_ROTATION 0x33264A0u    /* RotationManager: map +0x38 */
#define AA_ALERT 0x3326528u       /* AiAlert: map +0x20, levels +0x50 (u32 per entity, 0..7; replicated 0xEA04D444) */
#define AA_WIELDERS 0x3326420u    /* WielderManager: map +0x30, fire bytes +0x58 (0x20 each), hands +0x60 (0x1D0 each,
                                    * 0x50 a hand: +0 the wielded entity), hand states +0x68 (3 bits a hand) */
#define AA_TARGETING 0x3326D30u   /* WeaponTargeting: map +0x150, network +0x180 (0x18 each: +0 target, +0x10, +0x14 has aim) */
#define AA_COMBAT_FN 0xAA42D0u    /* game: the AiEnemy combat state (13165 bytes) */
#define AA_ENGAGED_ALERT 5u       /* the alert level the game's awareness gives a seen target on engaging (0x7EB000) */
#define AA_STEER 0x7D9FA0u        /* game: the steering pass (unused, start, end, dt) */
#define AA_FIELD_WRITE 0xFD97E0u  /* game: QueueFieldWrite (network id, field hash, value) */
#define AA_VELOCITY_FIELD 0x3A2B4F15u
#define AA_MOVER_FIELD 0x1510FA6Au
#define AA_DEVASTATOR 0xAE63E525853D7044ull
#define AA_TYPES 8
#define AA_DEAD 0x10u
#define AA_COMBAT 9u
#define AA_MAP 8192u
#define AA_RAY_ROOM 3000u         /* the batch has room for about 4,100 rays and no check: ours stop well before */
#define AA_HIT 0x30u
#define AA_EYE_UP 2.2f            /* the Devastator's eye over its feet */
#define AA_CHEST_UP 1.2f

enum { AF_SET_TARGET, AF_FACE, AF_MOVE, AF_INFORM, AF_FIND_ENTRY, AF_START, AF_START_AT, AF_RAY, AF_STATE, AF_SPEEDS,
    AF_DATA, AF_ATTACKS, AF_AVOID, AF_FACING, AF_ALERT, AF_AIM, AF_COUNT };
static const GameFunction kAaFunctions[AF_COUNT] = {
    {0x4AF4E0u, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x41, 0x55, 0x41, 0x56}}, /* BehaviourSetTarget */
    {0x4C1530u, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}}, /* BehaviourFace */
    {0x4D1150u, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x41, 0x54, 0x41}}, /* BehaviourMoveTo */
    {0x885260u, {0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xec, 0x38, 0x44, 0x3b, 0x05, 0xdd, 0xe9, 0xbf, 0x02, 0x4d, 0x8b, 0xe1, 0x4c, 0x8b, 0x2d, 0xcf, 0x12, 0xaa}}, /* inform a perceiver */
    {0x8891C0u, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x41, 0x56, 0x3b, 0x15}}, /* FindPerceptionEntry */
    {0x7CAF40u, {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x18, 0x55, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xec, 0xb0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x05}}, /* StartAbility */
    {0x7CB340u, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30, 0x3b, 0x15, 0xc5}}, /* StartAbility at a target */
    {0x175CA60u, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20}}, /* the ray batch's append */
    {0x7EAC40u, {0x40, 0x53, 0x57, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x42, 0x08, 0x45, 0x8b, 0xf8, 0x3b, 0x05, 0xcb, 0x8f, 0xc9, 0x02, 0x48, 0x8b, 0xf9}}, /* AiEnemy state request */
    {0x7F0180u, {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x6c, 0x24, 0x20, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x40, 0x45, 0x33}}, /* AiEnemy movement block */
    {0x511CC0u, {0x48, 0x85, 0xc9, 0x74, 0x71, 0x48, 0x8b, 0x05, 0xcc, 0xa2, 0xf5, 0x02, 0x44, 0x8b, 0xc1, 0x4c, 0x8b, 0x90, 0xe8, 0x2d, 0xf1, 0x00, 0x48, 0xb8}}, /* AiEnemyComponentData of a type */
    {0x4A0F00u, {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xa1, 0x48, 0x81, 0xec, 0xb0, 0x00, 0x00}}, /* a behaviour's attack list */
    {0x7DDC30u, {0x41, 0x56, 0x48, 0x83, 0xec, 0x20, 0x3b, 0x15, 0xe4, 0x5f, 0xca, 0x02, 0x45, 0x0f, 0xb6, 0xf0, 0x4c, 0x8b, 0x15, 0xd1, 0x90, 0xb4, 0x02, 0x0f}}, /* Havok AI avoidance on / off */
    {0x61DBB0u, {0x41, 0x56, 0x48, 0x83, 0xec, 0x30, 0x3b, 0x15, 0x64, 0x60, 0xe6, 0x02, 0x4d, 0x8b, 0xf0, 0x4c, 0x8b, 0xd9, 0x75, 0x07, 0xba, 0xff, 0xff, 0xff}}, /* facing (and its field) */
    {0x7F0700u, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec}}, /* AiEnemy alert level toward an entry */
    {0x7EEFE0u, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xc0, 0x48, 0x81, 0xec}}, /* the angle (degrees) between an AiEnemy's weapon and a point */
};
/* Diagnostics only (HELLFORGE_ASYNC_DIAG=1): StartAbility and ProjectileFire, hooked to follow attacks. */
static const GameFunction kAaDiagHooked[2] = {
    {0x7CAF40u, {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x18, 0x55, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xec, 0xb0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x05}},
    {0x6128B0u, {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0x18, 0xfb, 0xff}},
};
static const GameFunction kAaHooked[3] = {
    {AA_AI_PASS, {0x48, 0x8b, 0xc4, 0x57, 0x41, 0x54, 0x48, 0x83, 0xec, 0x78, 0x0f, 0x29, 0x78, 0xb8, 0x45, 0x33, 0xe4, 0x44, 0x0f, 0x29, 0x40, 0xa8, 0x41, 0x8b}},
    {AA_STATE_SWITCH, {0x40, 0x56, 0x48, 0x83, 0xec, 0x40, 0x41, 0x83, 0xc1, 0xfe, 0x48, 0x8b, 0xf2, 0x41, 0x83, 0xf9, 0x0d, 0x0f, 0x87, 0x80, 0x02, 0x00, 0x00, 0x48}},
    {AA_FIELD_WRITE, {0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0x05, 0xa3, 0x27, 0x49, 0x02, 0x4d, 0x8b, 0xf0}},
};

typedef void (*AaPassFn)(void* manager, float dt);
typedef void (*AaSwitchFn)(void* unused, void* manager, uint32_t index, uint32_t state, float dt);
typedef void (*AaSetTargetFn)(BehaviourCtx* ctx, uint8_t* entry);
typedef void (*AaVectorFn)(BehaviourCtx* ctx, const float* v);
typedef void (*AaInformFn)(void* unused, uint32_t perceiver, uint32_t entity, const float* position, char seen, char forced);
typedef uint8_t* (*AaFindFn)(void* perception, uint32_t perceiver, uint32_t entity);
typedef void (*AaStartFn)(void* manager, uint32_t entity, uint32_t ability, int mode, float speed, uint64_t a6, uint64_t a7);
typedef void (*AaStartAtFn)(void* unused, uint32_t entity, uint32_t ability, uint32_t target, const float* position, int mode);
typedef uint32_t (*AaRayFn)(void* batch, uint32_t filter, const float* from, const float* dir, float length, uint32_t ignore,
    uint32_t hits, void* buffer);
typedef void (*AaStateFn)(void* manager, uint8_t* info, uint32_t state);
typedef void (*AaSpeedsFn)(void* manager, uint32_t entity, const float* block);
typedef uint8_t* (*AaDataFn)(uint64_t resource);
typedef uint8_t* (*AaAttackListFn)(BehaviourCtx* ctx, uint32_t behaviour, float* scratch, uint32_t* count);
typedef void (*AaAvoidFn)(void* unused, uint32_t entity, char on);
typedef void (*AaFacingFn)(void* rotation, uint32_t entity, const float* dir);
typedef void (*AaSteerFn)(void* unused, uint32_t start, uint32_t end, float dt);
typedef void (*AaFieldFn)(uint32_t network, uint32_t field, void* value);
typedef void (*AaAlertFn)(void* manager, uint8_t* info, uint8_t* entry, uint32_t level);
typedef float (*AaAimFn)(void* manager, uint8_t* info, const float* point, uint32_t target);
typedef void (*AaFireFn)(void* projectiles, uint32_t weapon, uint64_t r8, uint64_t r9, uint64_t s5, uint64_t s6, uint64_t s7,
    uint64_t s8, uint64_t s9, uint64_t s10, uint64_t s11);

/* One registered agent, the game thread's: its held intent, its ray, what was applied. */
typedef struct AaSlot
{
    uint32_t entity, unit, rayUnit, rayFrame;
    int8_t rayPlayer, rayVisible, applied, pad;
    double rayAt;
    AbIntent held;
    uint8_t pendingMove, pendingFire, pendingRay, quiet, attackCount, crowd, faced, fireWait;
    float vel[3], face[3], facedDir[2], push[2];
    uint32_t charIndex;        /* its AiCharacter index when last applied in full */
    uint8_t distant;
    uint8_t avoidOff;          /* Havok AI avoidance taken off it (the hybrid with HELLFORGE_ASYNC_AVOID_OFF) */
    float nearest;             /* metres to the nearest Helldiver at its last snapshot */
    uint8_t neighbours;        /* its Havok AI avoidance neighbour limit was set (HELLFORGE_ASYNC_AVOID_NEIGHBOURS) */
    AbAttack attacks[AB_ATTACKS];
    uint8_t hits[2][AA_HIT];
} AaSlot;
typedef struct AaKey { uint32_t entity, slot; } AaKey;

static void* volatile g_aaPassNext;
static void* volatile g_aaSwitchNext;
static uintptr_t g_aaGame;
static volatile LONG g_aaOn;
static int g_aaState, g_aaTypeCount;
static uint64_t g_aaTypes[AA_TYPES];
static ULONGLONG g_aaFirstTry, g_aaReportedAt;
static AaSlot g_aaSlots[AB_MAX];
static AaKey g_aaMap[2][AA_MAP];
static int g_aaMapNow;
static uint8_t g_aaUsed[AB_MAX];
static AbSnapshot g_aaSnap;
static AbIntent g_aaOut[AB_MAX];
static uint8_t g_aaThought[AB_MAX];
static HANDLE g_aaWake;
static volatile LONG g_aaJob;         /* 0 idle, 1 running, 2 done */
static uint32_t g_aaFrame;
static uint8_t g_aaCanary[2][AA_HIT]; /* a ray straight down each frame: hit means the batch ran */
static uint32_t g_aaCanaryFrame;
static volatile LONG64 g_aaPasses;
static LONG64 g_aaLate;          /* frames the worker was still on its pass */
static double g_aaWorkerMs, g_aaApplyMs, g_aaCaptureMs;
static LONG64 g_aaStat[12];           /* frames, busy, agents, engaged, skipped, moves, fires, rays, visible, answered, thinks, informs */
static LONG64 g_aaLast[12];
static int g_aaListed;
static int g_aaMoveOn, g_aaFieldsOn, g_aaLodOn, g_aaAvoidOff, g_aaAlertOn, g_aaDiag;
static LONG64 g_aaAlerted, g_aaAimWaits, g_aaAimDrops, g_aaMeterFrames;
static LONG64 g_aaRing[5], g_aaDestNear, g_aaDestAll; /* engaged agents by distance to their target (<20, 20-35, 35-50, 50+ m,
                                                       * and seen), and how many have the navigator's destination at our goal */
static int g_aaDestLogged;
static volatile LONG g_aaOurStart;  /* our own StartAbility call runs (the diagnostics tell it apart) */
static DWORD g_aaGameThread;
static volatile LONG g_aaInSwitch;   /* the game's AiEnemy state handler runs (the diagnostics tell its starts apart) */
static double g_aaPathOk, g_aaPathAll, g_aaSpeedSum, g_aaVelSum, g_aaVelN, g_aaModes[8];  /* phase 2 (our movement and avoidance), phase 3 (fields on change) */
static AbMove g_aaMoves[AB_MAX];
static uint32_t g_aaSteerMark[AB_MAX]; /* AiCharacter index -> the frame + 1 phase 2 moved it in */
static uint32_t g_aaSepMark[AB_MAX];   /* the hybrid: AiCharacter index -> the frame + 1 its push is for */
static float g_aaSepPush[AB_MAX][2];
static float g_aaSepVel[AB_MAX][2];     /* the hybrid without Havok AI avoidance: our whole velocity replaces the game's */
static uint8_t g_aaSepReplace[AB_MAX];
static float g_aaAvoidNear = 30.0f;
static int g_aaNeighbours;              /* HELLFORGE_ASYNC_AVOID_NEIGHBOURS: the avoidance character's neighbour limit (+0xB8, 15) */
#define AA_AVOID_API 0x33263A0u        /* game: AiCollisionAvoidanceApi (slot 13 +0x68 sets character +0xB8) */
typedef void (*AaSetB8Fn)(uint32_t character, uint32_t value);
static LONG64 g_aaNeighboursSet;     /* HELLFORGE_ASYNC_AVOID_NEAR: within this of a Helldiver Havok AI avoidance stays */
static LONG64 g_aaReplaced, g_aaAvoidToggles, g_aaAvoidOffNow;
static int g_aaHybrid;
static LONG64 g_aaPushed;
static void* volatile g_aaSteerNext;
static void* volatile g_aaFieldNext;
static LONG64 g_aaSteered, g_aaSteerKept, g_aaFieldSkips, g_aaFieldCalls;
typedef struct AaField { float v[3]; ULONGLONG at, atB; uint8_t has, b, hasB; } AaField;
static AaField g_aaFields[0x8000];

static void* AaFn(int f) { return (void*)(g_aaGame + kAaFunctions[f].rva); }
static uintptr_t AaGlobal(uint32_t rva) { return *(uintptr_t*)(g_aaGame + rva); }
static double AaNow(void) { return (double)*(uint64_t*)(*(uint8_t**)(g_aaGame + BHV_CLOCK) + 0x18) / 1e6; }
static double AaMs(LARGE_INTEGER a, LARGE_INTEGER b)
{
    LARGE_INTEGER hz; QueryPerformanceFrequency(&hz);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)hz.QuadPart;
}

/* The registry slot of an entity in the current map, or -1. */
static int AaSlotOf(uint32_t entity)
{
    const AaKey* map = g_aaMap[g_aaMapNow];
    for (uint32_t k = 0, h = (entity * 2654435761u) & (AA_MAP - 1u); k < AA_MAP; k++, h = (h + 1u) & (AA_MAP - 1u))
    {
        if (map[h].entity == entity) return (int)map[h].slot;
        if (map[h].entity == 0) return -1;
    }
    return -1;
}

static void AaMapPut(AaKey* map, uint32_t entity, uint32_t slot)
{
    for (uint32_t k = 0, h = (entity * 2654435761u) & (AA_MAP - 1u); k < AA_MAP; k++, h = (h + 1u) & (AA_MAP - 1u))
        if (map[h].entity == 0) { map[h].entity = entity; map[h].slot = slot; return; }
}

static int AaType(uint64_t resource)
{
    for (int i = 0; i < g_aaTypeCount; i++) if (g_aaTypes[i] == resource) return 1;
    return 0;
}

/* The behaviour context the game's behaviour setters take, for an entity. */
static int AaCtx(uint32_t entity, BehaviourCtx* ctx)
{
    uintptr_t m = AaGlobal(BHV_MANAGER);
    int64_t i = m ? BhvMapFind(m, 0x40, entity) : -1;
    if (i < 0) return 0;
    ctx->entity = (*(uint8_t***)(m + 0x58))[i];
    ctx->p = *(uint8_t**)(m + 0x60) + (size_t)i * 0x1F8 + 8;
    return ctx->entity != NULL;
}

/* An entity's perceiver record, or NULL. */
static uint8_t* AaPerceiver(uint32_t perceiver)
{
    uintptr_t m = AaGlobal(AA_PERCEPTION);
    int64_t i = m ? BhvMapFind(m, 0x30, perceiver) : -1;
    return i < 0 ? NULL : *(uint8_t**)(m + 0x50) + (size_t)i * 0x13F8;
}

static int AaAbilityRunning(uint32_t entity)
{
    uintptr_t m = AaGlobal(AA_ABILITIES);
    int64_t i = m ? BhvMapFind(m, 0x18, entity) : -1;
    return i >= 0 && (*(uint8_t**)(m + 0x38))[(size_t)i * 0xE0 + 0x10] != 0;
}

/* Position and forward from the UnitManager's pose cache (Stingray: z up, a unit looks along its +y). */
static int AaPose(uint32_t entity, float* pos, float* fwd)
{
    uintptr_t m = AaGlobal(AA_UNITS);
    int64_t i = m ? BhvMapFind(m, 0x40, entity) : -1;
    if (i < 0) return 0;
    const float* r = (const float*)(*(uint8_t**)(m + 0x68) + (size_t)i * 0x308 + 0x2D0);
    float x = r[0], y = r[1], z = r[2], w = r[3];
    memcpy(pos, r + 4, 12);
    fwd[0] = 2.0f * (x * y - w * z); fwd[1] = 1.0f - 2.0f * (x * x + z * z); fwd[2] = 2.0f * (y * z + w * x);
    return isfinite(pos[0]) && isfinite(pos[1]) && isfinite(pos[2]);
}

/* The Helldivers: entity, unit, position, alive (downed and dead are not targets). */
static void AaPlayers(AbSnapshot* s)
{
    EvpPlayer players[EVT_MAX_PLAYERS];
    int n = EvpPlayers(g_aaGame, players);
    s->playerCount = 0;
    for (int i = 0; i < n && s->playerCount < AB_PLAYERS; i++)
    {
        ActHealth h;
        float fwd[3];
        AbPlayerIn* p = &s->players[s->playerCount];
        if (!players[i].avatar || !ActHealthOf(g_aaGame, players[i].avatar, &h) || !AaPose(players[i].avatar, p->pos, fwd)) continue;
        p->entity = players[i].avatar; p->unit = h.unit; p->alive = h.life == 0 && h.health > 0;
        s->playerCount++;
    }
}

/* What the agent's ears hold: the Helldiver it heard last and how long ago. */
static void AaHeard(const AbSnapshot* s, uint8_t* perceiver, AbAgentIn* a)
{
    static const uint32_t lists[3] = {0x310, 0x818, 0xD20};
    uint64_t now = (uint64_t)(s->now * 1e6), best = 0;
    a->heardPlayer = -1; a->heardAge = -1.0f;
    for (int l = 0; perceiver && l < 3; l++)
    {
        uint32_t count = *(uint32_t*)(perceiver + lists[l]);
        for (uint32_t e = 0; e < count && e < 16; e++)
        {
            const uint8_t* entry = perceiver + lists[l] + 8 + e * 0x50;
            uint64_t heard = *(const uint64_t*)(entry + 0x20);
            for (uint32_t p = 0; p < s->playerCount; p++)
                if (*(const uint32_t*)entry == s->players[p].entity && heard > best && heard <= now)
                { best = heard; a->heardPlayer = (int8_t)p; a->heardAge = (float)(now - heard) / 1e6f; }
        }
    }
}

/* The attacks the game gave this agent (its AiEnemy record's list: 0x60 a row, +0 ability, +4 how it starts, +0x14 /
 * +0x18 its range), and its first log line. */
static void AaAttacks(uintptr_t ai, AaSlot* a)
{
    BehaviourCtx ctx;
    float scratch[16] = {0};
    uint32_t count = 0;
    if (!AaCtx(a->entity, &ctx)) return;
    const uint8_t* rows = ((AaAttackListFn)AaFn(AF_ATTACKS))(&ctx, *(uint32_t*)(ctx.p - 8), scratch, &count);
    a->attackCount = 0;
    for (uint32_t k = 0; rows && k < count && a->attackCount < AB_ATTACKS; k++)
    {
        const uint8_t* row = rows + k * 0x60;
        const float* f = (const float*)row;
        AbAttack* at = &a->attacks[a->attackCount++];
        at->ability = *(const uint32_t*)row; at->kind = *(const uint32_t*)(row + 4); at->group = *(const uint32_t*)(row + 0x38);
        at->minRange = f[3]; at->maxRange = f[4]; at->minAim = f[5]; at->maxAim = f[6];
        at->cdFrom = f[7]; at->cdTo = f[8]; at->cdNear = f[9]; at->cdNearMax = f[10]; at->cdFar = f[11]; at->cdFarMax = f[12];
        at->weight = k < 16 ? scratch[k] : 0.0f;
        if (!g_aaListed) Note("async-ai: the Devastator's attack %u: ability %u, start kind %u, type %u, %.1f-%.1f m, aim %.0f-%.0f deg, "
            "weight %.3f, cooldown %.1f-%.1f s near / %.1f-%.1f s far over %.0f-%.0f m, group %u, slot %u, place group %u, flags 0x%X "
            "(budget %u updates a frame)", k, at->ability, at->kind, *(const uint32_t*)(row + 8), at->minRange, at->maxRange,
            at->minAim, at->maxAim, at->weight, at->cdNear, at->cdNearMax, at->cdFar, at->cdFarMax, at->cdFrom, at->cdTo, at->group,
            *(const uint32_t*)(row + 0x44), *(const uint32_t*)(row + 0x54), *(const uint32_t*)(row + 0x5C), *(uint32_t*)(ai + 0x1D10));
    }
    if (a->attackCount) g_aaListed = 1;
}

/* Where steering would send the agent, by its AiCharacter mode (C+8, as 0x7D9FA0 switches): 4 along the navigator's
 * path (+0x34, usable with status 0 or 3 and flags & 0xA), 2 straight to the destination (+0x60), 3 ahead. Other modes
 * (0 none, 1 driven by motion) stay the game's steering. */
static int AaModeDir(uint32_t mode, const uint8_t* r, const AbAgentIn* a, float* dir)
{
    float x = 0.0f, y = 0.0f;
    if (mode == 4)
    {
        uint32_t status = *(const uint32_t*)(r + 0xC), flags = *(const uint32_t*)(r + 0x10);
        if ((status == 0 || status == 3) && (flags & 0xA)) { x = ((const float*)(r + 0x34))[0]; y = ((const float*)(r + 0x34))[1]; }
    }
    else if (mode == 2) { x = ((const float*)(r + 0x60))[0] - a->pos[0]; y = ((const float*)(r + 0x60))[1] - a->pos[1]; if (x * x + y * y < 0.25f) x = y = 0.0f; }
    else if (mode == 3) { x = a->fwd[0]; y = a->fwd[1]; }
    else return 0;
    float l = sqrtf(x * x + y * y);
    if (l > 1e-3f) { dir[0] = x / l; dir[1] = y / l; return 2; }
    return 1;
}

/* Phase 2's inputs: the direction steering would take and the speed the game gave the agent. */
static void AaPath(uint32_t entity, AbAgentIn* a)
{
    uintptr_t nav = AaGlobal(AA_NAVIGATION), ch = AaGlobal(AA_CHARACTERS);
    int64_t n = nav ? BhvMapFind(nav, 0x30, entity) : -1, c = ch ? BhvMapFind(ch, 0x30, entity) : -1;
    a->pathOk = 0; a->speed = 0.0f;
    if (n < 0 || c < 0) return;
    const uint8_t* cr = *(uint8_t**)(ch + 0x60) + (size_t)c * 0x14;
    uint32_t mode = *(const uint32_t*)(cr + 8);
    int got = AaModeDir(mode, *(uint8_t**)(nav + 0x50) + (size_t)n * 0xA8, a, a->pathDir);
    g_aaModes[mode < 8 ? mode : 7] += 1.0;
    if (got == 0) return;
    a->pathOk = (uint8_t)(got == 2);
    a->speed = *(const float*)(cr + 0xC);
    a->crowd = 1;
    g_aaPathAll += 1.0; g_aaPathOk += a->pathOk; g_aaSpeedSum += a->speed;
}

/* Phase 5: over 100 m from every Helldiver (the snapshot's players). */
static int AaFar(const float* p)
{
    for (uint32_t k = 0; k < g_aaSnap.playerCount; k++)
    {
        float dx = p[0] - g_aaSnap.players[k].pos[0], dy = p[1] - g_aaSnap.players[k].pos[1];
        if (dx * dx + dy * dy <= 10000.0f) return 0;
    }
    return g_aaSnap.playerCount > 0;
}

/* A new slot for an entity: its perceiver's eye scan off (the brain sees for it), memory fresh. */
static int AaRegister(uint32_t entity, uint32_t unit)
{
    for (uint32_t s = 0; s < AB_MAX; s++)
    {
        if (g_aaUsed[s]) continue;
        g_aaUsed[s] = 2;
        AaSlot* sl = &g_aaSlots[s];
        memset(sl, 0, sizeof *sl);
        sl->entity = entity; sl->unit = unit; sl->rayPlayer = -1; sl->held.target = -1;
        return (int)s;
    }
    return -1;
}

/* One agent into the snapshot: pose, ears, ray answer, attacks. 0 when it does not read. */
static int AaCapture(uintptr_t ai, uint32_t index, uint8_t* info, AbAgentIn* a, AaKey* next)
{
    uint32_t entity = *(uint32_t*)(info + 8), unit = *(uint32_t*)(info + 0xC);
    if (!(info[0x14] & 1) || *(uint32_t*)(*(uint8_t**)(ai + 0x2358) + (size_t)index * 0xC0) == AA_DEAD) return 0;
    int slot = AaSlotOf(entity), fresh = 0;
    if (slot < 0 && (slot = AaRegister(entity, unit)) < 0) return 0;
    if (g_aaUsed[slot] == 2) { fresh = 1; g_aaUsed[slot] = 1; }
    AaSlot* sl = &g_aaSlots[slot];
    if (!AaPose(entity, a->pos, a->fwd)) return 0;
    if (fresh || (g_aaFrame + (uint32_t)slot) % 60u == 0) AaAttacks(ai, sl);
    a->crowd = 0;
    if (g_aaMoveOn) AaPath(entity, a);
    a->distant = sl->distant = (uint8_t)(g_aaLodOn && AaFar(a->pos));
    sl->nearest = 1e9f;
    for (uint32_t k = 0; k < g_aaSnap.playerCount; k++)
    {
        float dx = a->pos[0] - g_aaSnap.players[k].pos[0], dy = a->pos[1] - g_aaSnap.players[k].pos[1], dd = sqrtf(dx * dx + dy * dy);
        if (dd < sl->nearest) sl->nearest = dd;
    }
    a->attackCount = sl->attackCount;
    memcpy(a->attacks, sl->attacks, sizeof a->attacks);
    AaMapPut(next, entity, (uint32_t)slot);
    sl->quiet = 1;
    uint8_t* perceiver = AaPerceiver(entity);
    if (perceiver) *(uint64_t*)(perceiver + 0x13D8) = ~0ull;
    a->entity = entity; a->slot = (uint32_t)slot; a->fresh = (uint8_t)fresh; a->busy = (uint8_t)AaAbilityRunning(entity);
    a->rayPlayer = sl->rayPlayer; a->rayVisible = (uint8_t)(sl->rayVisible > 0); a->rayAge = (float)(g_aaSnap.now - sl->rayAt);
    if ((g_aaSnap.frame + (uint32_t)slot) % AB_THINK_EVERY == 0 || fresh) AaHeard(&g_aaSnap, perceiver, a);
    else { a->heardPlayer = -1; a->heardAge = -1.0f; }
    return 1;
}

/* The next snapshot, from every agent of our types this machine has authority over; slots of the gone are freed. */
static void AaSnapshotTake(uintptr_t ai)
{
    int next = 1 - g_aaMapNow;
    memset(g_aaMap[next], 0, sizeof g_aaMap[next]);
    g_aaSnap.frame = g_aaFrame; g_aaSnap.now = AaNow(); g_aaSnap.agentCount = 0;
    AaPlayers(&g_aaSnap);
    for (uint32_t s = 0; s < AB_MAX; s++) g_aaSlots[s].quiet = 0;
    uint32_t count = *(uint32_t*)(ai + 0x2324);
    uint8_t** infos = *(uint8_t***)(ai + 0x2348);
    for (uint32_t i = 0; infos && i < count && g_aaSnap.agentCount < AB_MAX; i++)
    {
        uint8_t* info = infos[i];
        if (info == NULL || !AaType(*(uint64_t*)info)) continue;
        if (AaCapture(ai, i, info, &g_aaSnap.agents[g_aaSnap.agentCount], g_aaMap[next])) g_aaSnap.agentCount++;
    }
    for (uint32_t s = 0; s < AB_MAX; s++) if (g_aaUsed[s] && !g_aaSlots[s].quiet) { g_aaUsed[s] = 0; g_aaSlots[s].entity = 0; }
    g_aaMapNow = next;
    g_aaStat[2] += g_aaSnap.agentCount;
}

/* The ray the agent asked for: from its eye to the Helldiver's chest through the game's ray batch, answered into this
 * slot's buffer for this frame's parity; the batch runs at the next frame's query kick. */
static void AaRayAsk(AaSlot* sl, const AbSnapshot* s, int player, const float* from)
{
    uintptr_t eyes = AaGlobal(AA_EYES);
    void* batch = eyes ? *(void**)eyes : NULL;
    uint8_t* settings = eyes ? *(uint8_t**)(eyes + 8) : NULL;
    if (!batch || !settings || player < 0 || (uint32_t)player >= s->playerCount || *(uint32_t*)batch >= AA_RAY_ROOM) return;
    const AbPlayerIn* p = &s->players[player];
    float eye[3] = {from[0], from[1], from[2] + AA_EYE_UP}, to[3] = {p->pos[0], p->pos[1], p->pos[2] + AA_CHEST_UP}, dir[3];
    float d = sqrtf((to[0] - eye[0]) * (to[0] - eye[0]) + (to[1] - eye[1]) * (to[1] - eye[1]) + (to[2] - eye[2]) * (to[2] - eye[2]));
    if (d < 0.5f) return;
    for (int k = 0; k < 3; k++) dir[k] = (to[k] - eye[k]) / d;
    uint8_t* hits = sl->hits[g_aaFrame & 1];
    memset(hits, 0xFF, AA_HIT);
    ((AaRayFn)AaFn(AF_RAY))(batch, *(uint32_t*)(settings + 0xBC), eye, dir, d + 0.5f, sl->unit, 1, hits);
    sl->rayFrame = g_aaFrame; sl->rayUnit = p->unit; sl->pendingRay = (uint8_t)(player + 1);
    g_aaStat[7]++;
}

/* Last frame's rays, answered at this frame's join: nothing hit, or the Helldiver first, is a line of sight. */
static void AaRayRead(AaSlot* sl, double now, int ran)
{
    if (!sl->pendingRay || sl->rayFrame + 1u != g_aaFrame || !ran) { if (sl->rayFrame + 1u <= g_aaFrame) sl->pendingRay = 0; return; }
    uint32_t hit = *(uint32_t*)(sl->hits[sl->rayFrame & 1] + 0x1C);
    sl->rayPlayer = (int8_t)(sl->pendingRay - 1);
    sl->rayVisible = (int8_t)(hit == 0xFFFFFFFFu || hit == sl->rayUnit);
    sl->rayAt = now; sl->pendingRay = 0;
    g_aaStat[9]++;
    g_aaStat[8] += sl->rayVisible;
}

/* The target in the game's terms: the perceiver's entry for the Helldiver, kept seen and aware, installed in the
 * behaviour (whose target the weapon aims at and replicates); its own re-pick held off. */
static uint8_t* AaTarget(BehaviourCtx* ctx, const AbIntent* in, const AbSnapshot* s, double now)
{
    uint32_t perceiver = *(uint32_t*)(ctx->p + 0x60);
    uint32_t target = s->players[in->target].entity;
    uintptr_t perception = AaGlobal(AA_PERCEPTION);
    uint8_t* entry = ((AaFindFn)AaFn(AF_FIND_ENTRY))((void*)perception, perceiver, target);
    if (entry == NULL)
    {
        ((AaInformFn)AaFn(AF_INFORM))(NULL, perceiver, target, in->targetPos, 1, 0);
        entry = ((AaFindFn)AaFn(AF_FIND_ENTRY))((void*)perception, perceiver, target);
        g_aaStat[11]++;
        if (entry == NULL) return NULL;
    }
    uint64_t us = (uint64_t)(now * 1e6);
    entry[0x30] = in->seen;
    if (in->seen) { memcpy(entry + 4, in->targetPos, 12); *(uint64_t*)(entry + 0x10) = us; *(uint32_t*)(entry + 0x48) |= 3u; }
    *(float*)(entry + 0x40) = 1.0f;
    if (*(float*)(entry + 0x44) < 0.0f) *(float*)(entry + 0x44) = 1.0f;
    if (*(uint32_t*)(ctx->p + 0x10) != target || ctx->p[0x70] == 0) ((AaSetTargetFn)AaFn(AF_SET_TARGET))(ctx, entry);
    *(uint64_t*)(ctx->p + 0x90) = us + 10000000ull;
    return entry;
}

/* An entity's alert level (AiAlert), or -1. */
static int AaAlertOf(uint32_t entity)
{
    uintptr_t m = AaGlobal(AA_ALERT);
    int64_t i = m ? BhvMapFind(m, 0x20, entity) : -1;
    return i < 0 ? -1 : (int)(*(uint32_t**)(m + 0x50))[i];
}

/* Engaging hands the agent from the game's AI to ours: its state becomes combat (replicated) with combat speeds. */
static void AaEngage(uintptr_t ai, uint8_t* info)
{
    ((AaStateFn)AaFn(AF_STATE))((void*)ai, info, AA_COMBAT);
    uint8_t* data = ((AaDataFn)AaFn(AF_DATA))(*(uint64_t*)info);
    if (data) ((AaSpeedsFn)AaFn(AF_SPEEDS))((void*)ai, *(uint32_t*)(info + 8), (const float*)(data + 100));
}

/* Phase 2 for one agent: its navigator learns where it is, Locomotion gets our velocity, the facing ours (with its
 * field, when it turned), Havok AI avoidance stays off, and the steering pass skips it this frame. */
static void AaCrowdApply(AaSlot* sl)
{
    uintptr_t nav = AaGlobal(AA_NAVIGATION), ch = AaGlobal(AA_CHARACTERS), loco = AaGlobal(AA_LOCOMOTION);
    int64_t n = nav ? BhvMapFind(nav, 0x30, sl->entity) : -1, c = ch ? BhvMapFind(ch, 0x30, sl->entity) : -1;
    int64_t l = loco ? BhvMapFind(loco, 0x20, sl->entity) : -1;
    float pos[3], fwd[3];
    if (n < 0 || c < 0 || l < 0 || c >= (int64_t)AB_MAX || !AaPose(sl->entity, pos, fwd)) return;
    if (g_aaNeighbours > 0 && !sl->neighbours)
    {   /* fewer neighbours in its Havok AI avoidance solve (the character stays in the solve: taking it out stalls its path) */
        uint32_t handle = *(uint32_t*)(*(uint8_t**)(ch + 0x58) + (size_t)c * 0x84);
        uintptr_t api = AaGlobal(AA_AVOID_API);
        if (handle != 0xFFFFFFFFu && api) { ((AaSetB8Fn)*(void**)(api + 0x68))(handle, (uint32_t)g_aaNeighbours); g_aaNeighboursSet++; }
        sl->neighbours = 1;
    }
    if (g_aaHybrid)
    {
        /* Havok AI avoidance off for agents away from the Helldivers (on again within the near distance, off past it
         * plus 10 m): the game's steering still runs for them (it feeds their navigator and avoidance character), and
         * after it our velocity (the path at the game's speed plus our separation) replaces the stale avoidance output
         * it moved them with; their facing follows that velocity unless they fight. */
        int off = sl->avoidOff;
        if (!g_aaAvoidOff) off = 0;
        else if (sl->nearest > g_aaAvoidNear + 10.0f) off = 1;
        else if (sl->nearest < g_aaAvoidNear) off = 0;
        if (off != sl->avoidOff) { ((AaAvoidFn)AaFn(AF_AVOID))(NULL, sl->entity, (char)!off); sl->avoidOff = (uint8_t)off; g_aaAvoidToggles++; }
        g_aaSepMark[c] = g_aaFrame + 1u; g_aaSepPush[c][0] = sl->push[0]; g_aaSepPush[c][1] = sl->push[1];
        g_aaSepReplace[c] = (uint8_t)off;
        if (off)
        {
            g_aaSepVel[c][0] = sl->vel[0]; g_aaSepVel[c][1] = sl->vel[1];
            g_aaAvoidOffNow++;
            float vl = sqrtf(sl->vel[0] * sl->vel[0] + sl->vel[1] * sl->vel[1]);
            if (!sl->held.engaged && vl > 0.3f)
            {
                float* face = (float*)(*(uint8_t**)(ch + 0x50) + (size_t)c * 0x40 + 0x24); /* the one-frame face request */
                face[0] = sl->vel[0] / vl; face[1] = sl->vel[1] / vl; face[2] = 0.0f;
            }
        }
        return;
    }
    memcpy(*(uint8_t**)(nav + 0x50) + (size_t)n * 0xA8 + 0x54, pos, 12);
    memcpy(*(uint8_t**)(loco + 0x40) + (size_t)l * 0xC, sl->vel, 12);
    if (g_aaAvoidOff) ((AaAvoidFn)AaFn(AF_AVOID))(NULL, sl->entity, 0);
    if (sl->faced && fabsf(sl->face[0] - sl->facedDir[0]) + fabsf(sl->face[1] - sl->facedDir[1]) > 0.01f)
    {
        ((AaFacingFn)AaFn(AF_FACING))((void*)AaGlobal(AA_ROTATION), sl->entity, sl->face);
        sl->facedDir[0] = sl->face[0]; sl->facedDir[1] = sl->face[1];
    }
    g_aaSteerMark[c] = g_aaFrame + 1u;
    sl->charIndex = (uint32_t)c + 1u;
}

/* Phase 5: a far agent between its full applies (every third frame) keeps its velocity in Locomotion and is only kept
 * out of the steering pass, when its AiCharacter index still holds it. */
static int AaCrowdKeep(AaSlot* sl)
{
    uintptr_t ch = AaGlobal(AA_CHARACTERS);
    uint32_t c = sl->charIndex - 1u;
    if (!g_aaLodOn || !sl->distant || sl->charIndex == 0 || !ch || c >= AB_MAX || (g_aaFrame + c) % 3u == 0) return 0;
    if (c >= *(uint32_t*)(ch + 0x1C) || *(uint32_t*)((*(uint8_t***)(ch + 0x48))[c] + 8) != sl->entity) return 0;
    g_aaSteerMark[c] = g_aaFrame + 1u;
    return 1;
}

/* A pending attack starts, with the game's own start for its kind, unless an ability already runs, and (as the game's
 * combat state checks, 0xAA42D0) only once the weapon points at the target within the row's angle window: until then
 * it stays pending, for up to AA_AIM_WAIT frames. */
#define AA_AIM_WAIT 45u
static void AaFire(uintptr_t ai, uint8_t* info, AaSlot* sl, const AbSnapshot* s)
{
    const AbIntent* in = &sl->held;
    if (!sl->pendingFire) return;
    if (AaAbilityRunning(sl->entity)) { sl->pendingFire = 0; return; }
    uint32_t target = s->players[in->target].entity;
    const AbAttack* at = NULL;
    for (uint32_t k = 0; k < sl->attackCount && k < AB_ATTACKS; k++) if (sl->attacks[k].ability == in->ability) at = &sl->attacks[k];
    if (at && info && (at->minAim > 0.0f || at->maxAim > 0.0f))
    {
        float aim[3] = {in->targetPos[0], in->targetPos[1], in->targetPos[2] + AA_CHEST_UP};
        float angle = ((AaAimFn)AaFn(AF_AIM))((void*)ai, info, aim, target);
        if (angle < at->minAim || (at->maxAim > 0.0f && angle > at->maxAim))
        {
            g_aaAimWaits++;
            if (++sl->fireWait >= AA_AIM_WAIT) { sl->pendingFire = 0; g_aaAimDrops++; }
            return;
        }
    }
    g_aaOurStart = 1;
    if (in->fire == AB_FIRE_TARGETED) ((AaStartAtFn)AaFn(AF_START_AT))(NULL, sl->entity, in->ability, target, in->targetPos, 1);
    else ((AaStartFn)AaFn(AF_START))((void*)AaGlobal(AA_ABILITIES), sl->entity, in->ability, 1, 1.0f, 0, 0);
    g_aaOurStart = 0;
    g_aaStat[6]++;
    sl->pendingFire = 0;
}


/* Goals: an engaged agent's navigation destination is ours. Other writers (the squad's orders, the game's other AI
 * code) go through 0x5B36C0 too and kept sending our engaged Devastators back to their squad's spot (ai-1-on: the
 * destination at our goal for 5% of them); for an engaged agent their calls are dropped (counted by caller). */
#define AA_SET_DEST 0x5B36C0u
static const uint8_t kAaSetDestPin[24] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
    0x48, 0x83, 0xec, 0x50, 0x3b, 0x15, 0x46, 0x05};
typedef void (*AaSetDestFn)(void* unused, uint32_t entity, const float* pos);
static void* volatile g_aaSetDestNext;
static int g_aaGoalsOn;
static volatile LONG g_aaOurMove;
static LONG64 g_aaDestDropped;
static uint32_t g_aaDropFrom[8], g_aaDropCount[8];

static void AaSetDest(void* unused, uint32_t entity, const float* pos)
{
    if (g_aaOn && g_aaGoalsOn && !g_aaOurMove && GetCurrentThreadId() == g_aaGameThread)
    {
        int slot = AaSlotOf(entity);
        if (slot >= 0 && g_aaSlots[slot].held.engaged && g_aaSlots[slot].applied)
        {
            uint32_t from = (uint32_t)((uintptr_t)__builtin_return_address(0) - g_aaGame);
            for (int k = 0; k < 8; k++)
                if (g_aaDropFrom[k] == from || g_aaDropCount[k] == 0) { g_aaDropFrom[k] = from; g_aaDropCount[k]++; break; }
            g_aaDestDropped++;
            return;
        }
    }
    ((AaSetDestFn)g_aaSetDestNext)(unused, entity, pos);
}

/* Applies one slot's held intent; the one-shots once. */
static void AaApplySlot(uintptr_t ai, AaSlot* sl, const AbSnapshot* s, double now)
{
    AbIntent* in = &sl->held;
    BehaviourCtx ctx;
    if (g_aaMoveOn && sl->crowd && !AaCrowdKeep(sl)) AaCrowdApply(sl);
    else if (sl->avoidOff) { ((AaAvoidFn)AaFn(AF_AVOID))(NULL, sl->entity, 1); sl->avoidOff = 0; g_aaAvoidToggles++; }
    if (!in->engaged || in->target < 0 || (uint32_t)in->target >= s->playerCount || !AaCtx(sl->entity, &ctx)) { sl->applied = 0; return; }
    int64_t i = BhvMapFind(ai, 0x2330, sl->entity);
    uint8_t* info = i >= 0 ? (*(uint8_t***)(ai + 0x2348))[i] : NULL;
    if (!sl->applied)
    {
        if (info) AaEngage(ai, info);
        sl->applied = 1;
    }
    uint8_t* entry = AaTarget(&ctx, in, s, now);
    /* the alert level the game's awareness keeps in combat (5: a seen target), which the game's combat reads */
    if (g_aaAlertOn && info && entry && in->seen && AaAlertOf(sl->entity) != (int)AA_ENGAGED_ALERT)
    { ((AaAlertFn)AaFn(AF_ALERT))((void*)ai, info, entry, AA_ENGAGED_ALERT); g_aaAlerted++; }
    if (in->face && !(g_aaMoveOn && !g_aaHybrid && sl->crowd)) ((AaVectorFn)AaFn(AF_FACE))(&ctx, in->faceDir);
    if (sl->pendingMove) { g_aaOurMove = 1; ((AaVectorFn)AaFn(AF_MOVE))(&ctx, in->goal); g_aaOurMove = 0; sl->pendingMove = 0; g_aaStat[5]++; }
    AaFire(ai, info, sl, s);
    g_aaStat[3]++;
    float pos[3], fwd[3];
    if (AaPose(sl->entity, pos, fwd))
    {
        const float* tp = s->players[in->target].pos;
        float d = sqrtf((tp[0] - pos[0]) * (tp[0] - pos[0]) + (tp[1] - pos[1]) * (tp[1] - pos[1]));
        g_aaRing[d < 20.0f ? 0 : d < 35.0f ? 1 : d < 50.0f ? 2 : 3]++;
        g_aaRing[4] += in->seen;
        uintptr_t nav = AaGlobal(AA_NAVIGATION);
        int64_t n = nav ? BhvMapFind(nav, 0x30, sl->entity) : -1;
        if (n >= 0)
        {
            const float* dest = (const float*)(*(uint8_t**)(nav + 0x50) + (size_t)n * 0xA8 + 0x60);
            float gd = sqrtf((dest[0] - in->goal[0]) * (dest[0] - in->goal[0]) + (dest[1] - in->goal[1]) * (dest[1] - in->goal[1]));
            g_aaDestAll++; g_aaDestNear += gd < 3.0f;
            if (g_aaDestLogged < 6 && (g_aaFrame % 600u) == 0u)
            {
                const uint8_t* r = *(uint8_t**)(nav + 0x50) + (size_t)n * 0xA8;
                g_aaDestLogged++;
                Note("async-ai: engaged E%X at %.1f m: our goal (%.1f %.1f), nav destination (%.1f %.1f) status %u flags 0x%X mode %u, "
                    "path dir (%.2f %.2f)", sl->entity, d, in->goal[0] - pos[0], in->goal[1] - pos[1], dest[0] - pos[0], dest[1] - pos[1],
                    *(const uint32_t*)(r + 0xC), *(const uint32_t*)(r + 0x10), *(const uint32_t*)r, ((const float*)(r + 0x34))[0], ((const float*)(r + 0x34))[1]);
            }
        }
    }
}

/* The canary: straight down from the first agent's eye; the ground answers it whenever the batch runs. */
static void AaCanary(void)
{
    uintptr_t eyes = AaGlobal(AA_EYES);
    void* batch = eyes ? *(void**)eyes : NULL;
    uint8_t* settings = eyes ? *(uint8_t**)(eyes + 8) : NULL;
    if (!batch || !settings || g_aaSnap.agentCount == 0 || *(uint32_t*)batch >= AA_RAY_ROOM) return;
    const AbAgentIn* a = &g_aaSnap.agents[0];
    float eye[3] = {a->pos[0], a->pos[1], a->pos[2] + AA_EYE_UP}, down[3] = {0.0f, 0.0f, -1.0f};
    uint8_t* hits = g_aaCanary[g_aaFrame & 1];
    memset(hits, 0xFF, AA_HIT);
    ((AaRayFn)AaFn(AF_RAY))(batch, *(uint32_t*)(settings + 0xBC), eye, down, AA_EYE_UP + 3.0f, g_aaSlots[a->slot % AB_MAX].unit, 1, hits);
    g_aaCanaryFrame = g_aaFrame;
}

/* The worker's finished pass into the held intents: thinkers replace theirs, with their one-shots pending. */
static void AaMerge(void)
{
    for (uint32_t i = 0; g_aaMoveOn && i < g_aaSnap.agentCount; i++)
    {
        if (g_aaMoves[i].entity == 0)
        {   /* not moved by us this pass (another AiCharacter mode): the game's steering alone */
            AaSlot* gone = &g_aaSlots[g_aaSnap.agents[i].slot % AB_MAX];
            if (gone->entity == g_aaSnap.agents[i].entity) gone->crowd = 0;
            continue;
        }
        AaSlot* sl = &g_aaSlots[g_aaMoves[i].slot % AB_MAX];
        if (sl->entity != g_aaMoves[i].entity) continue;
        memcpy(sl->vel, g_aaMoves[i].vel, 12); memcpy(sl->push, g_aaMoves[i].push, 8); sl->crowd = 1;
        g_aaVelSum += sqrtf(sl->vel[0] * sl->vel[0] + sl->vel[1] * sl->vel[1]); g_aaVelN += 1.0;
        if (g_aaMoves[i].hasFace) { memcpy(sl->face, g_aaMoves[i].face, 12); sl->faced = 1; }
    }
    for (uint32_t i = 0; i < g_aaSnap.agentCount; i++)
    {
        if (!g_aaThought[i]) continue;
        uint32_t slot = g_aaOut[i].slot % AB_MAX;
        AaSlot* sl = &g_aaSlots[slot];
        if (sl->entity != g_aaOut[i].entity) continue;
        sl->held = g_aaOut[i];
        sl->pendingMove = g_aaOut[i].move;
        if (g_aaOut[i].fire != AB_FIRE_NONE) { sl->pendingFire = 1; sl->fireWait = 0; }
        if (g_aaOut[i].ray >= 0) AaRayAsk(sl, &g_aaSnap, g_aaOut[i].ray, g_aaSnap.agents[i].pos);
        g_aaStat[10]++;
    }
}

static DWORD WINAPI AaWorker(LPVOID unused)
{
    (void)unused;
    for (;;)
    {
        WaitForSingleObject(g_aaWake, INFINITE);
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        AbRun(&g_aaSnap, g_aaOut, g_aaThought);
        if (g_aaMoveOn) AbCrowd(&g_aaSnap, g_aaMoves);
        QueryPerformanceCounter(&b);
        g_aaWorkerMs += AaMs(a, b);
        InterlockedIncrement64(&g_aaPasses);
        InterlockedExchange(&g_aaJob, 2);
    }
}

/* The game thread's part, after the game's AiEnemy pass: answers, intents, the next snapshot, the worker's wake. */
static void AaFrame(uintptr_t ai)
{
    LARGE_INTEGER a, b, c;
    QueryPerformanceCounter(&a);
    double now = AaNow();
    g_aaFrame++; g_aaStat[0]++;
    int ran = g_aaCanaryFrame + 1u == g_aaFrame && *(uint32_t*)(g_aaCanary[g_aaCanaryFrame & 1] + 0x1C) != 0xFFFFFFFFu;
    if (g_aaCanaryFrame + 1u == g_aaFrame && !ran) g_aaStat[1]++;
    for (uint32_t s = 0; s < AB_MAX; s++) if (g_aaUsed[s]) AaRayRead(&g_aaSlots[s], now, ran);
    if (g_aaJob == 2) { AaMerge(); InterlockedExchange(&g_aaJob, 0); }
    else if (g_aaJob == 1) g_aaLate++;
    for (uint32_t s = 0; s < AB_MAX; s++) if (g_aaUsed[s]) AaApplySlot(ai, &g_aaSlots[s], &g_aaSnap, now);
    QueryPerformanceCounter(&b);
    g_aaApplyMs += AaMs(a, b);
    if (g_aaJob != 0) return;
    AaCanary();
    AaSnapshotTake(ai);
    InterlockedExchange(&g_aaJob, 1);
    SetEvent(g_aaWake);
    QueryPerformanceCounter(&c);
    g_aaCaptureMs += AaMs(b, c);
}

/* A locomotion set change waits in steering's tail (C+0 against B+4): such an agent steers with the game this frame. */
static int AaSetPending(uintptr_t ch, uint32_t i)
{
    return *(uint32_t*)(*(uint8_t**)(ch + 0x60) + (size_t)i * 0x14) != *(uint32_t*)(*(uint8_t**)(ch + 0x58) + (size_t)i * 0x84 + 4);
}

/* The hybrid, after the game's steering wrote each agent's velocity: our separation added to ours, within its speed
 * and a half (Havok AI avoidance, which did this, is off for them). */
static void AaPushAfter(uintptr_t ch, uint32_t start, uint32_t end)
{
    uintptr_t loco = AaGlobal(AA_LOCOMOTION);
    uint32_t mark = g_aaFrame + 1u, count = *(uint32_t*)(ch + 0x1C);
    for (uint32_t i = start; loco && i < end && i < AB_MAX && i < count; i++)
    {
        if (g_aaSepMark[i] != mark || (!g_aaSepReplace[i] && g_aaSepPush[i][0] == 0.0f && g_aaSepPush[i][1] == 0.0f)) continue;
        int64_t l = BhvMapFind(loco, 0x20, *(uint32_t*)((*(uint8_t***)(ch + 0x48))[i] + 8));
        if (l < 0) continue;
        float* v = (float*)(*(uint8_t**)(loco + 0x40) + (size_t)l * 0xC);
        if (g_aaSepReplace[i]) { v[0] = g_aaSepVel[i][0]; v[1] = g_aaSepVel[i][1]; g_aaReplaced++; continue; }
        float speed = sqrtf(v[0] * v[0] + v[1] * v[1]), cap = speed > 1.0f ? speed * 1.5f : 1.5f;
        float x = v[0] + g_aaSepPush[i][0], y = v[1] + g_aaSepPush[i][1], n = sqrtf(x * x + y * y);
        if (n > cap) { x *= cap / n; y *= cap / n; }
        v[0] = x; v[1] = y;
        g_aaPushed++;
    }
}

/* The steering pass, without the agents phase 2 moved this frame. */
static void AaSteer(void* unused, uint32_t start, uint32_t end, float dt)
{
    AaSteerFn next = (AaSteerFn)g_aaSteerNext;
    uintptr_t ch = AaGlobal(AA_CHARACTERS);
    if (!g_aaOn || !g_aaMoveOn || !ch) { next(unused, start, end, dt); return; }
    if (g_aaHybrid) { next(unused, start, end, dt); AaPushAfter(ch, start, end); return; }
    uint32_t run = start, mark = g_aaFrame + 1u;
    for (uint32_t i = start; i < end && i < AB_MAX; i++)
    {
        if (g_aaSteerMark[i] != mark) continue;
        if (AaSetPending(ch, i)) { g_aaSteerKept++; continue; }
        if (run < i) next(unused, run, i, dt);
        run = i + 1;
        g_aaSteered++;
    }
    if (run < end) next(unused, run, end, dt);
}

static void AaFieldWrite(uint32_t network, uint32_t field, void* value);

/* Phase 4 (HELLFORGE_ASYNC_PAR: on by default, "off" or a list of loco / motion; research\async-ai\notes\phase4-parallel.md): the locomotion pass
 * (0x9C7440) in parallel on the engine's job workers, the way the game's unused range task 0x571150 once ran it: 12
 * records a chunk, taken from a shared cursor by the kicked tasks and by the game thread itself, joined where the pass
 * was called. Per chunk 0x9C77D0 over the range, then 0x9C94C0 and 0x9CB6F0 per record (the order of 0x9C7440 within the
 * chunk). Records that touch another record or send an RPC (a linked entity at +0x5C, the snap flag +0x91, the
 * Helldivers) run on the game thread after the join. QueueFieldWrite calls made on workers are held (it stores the
 * value's pointer, read at the flush) and replayed on the game thread after the join. */
#define AA_LOCO_PASS 0x9C7440u
#define AA_LOCO_RANGE 0x9C77D0u
#define AA_LOCO_TURN 0x9C94C0u
#define AA_LOCO_ANIM 0x9CB6F0u
#define AA_RUN_JOB 0xA40F0u          /* exe: ThreadApi::run_job (function, data, name, ?, priority, count, ?) */
#define AA_PENDING 0xA4A40u          /* exe: ThreadApi::maybe_run_pending_task */
#define AA_PAR_CHUNK 12u
#define AA_PAR_TASKS 24u
#define AA_PAR_HELD 65536u
static const GameFunction kAaParGame[4] = {
    {AA_LOCO_PASS, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x3d, 0x9d}},
    {AA_LOCO_RANGE, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xa0, 0xfe, 0xff, 0xff, 0x48, 0x81, 0xec, 0x60, 0x02, 0x00, 0x00, 0x48, 0x8b}},
    {AA_LOCO_TURN, {0x4c, 0x8b, 0xdc, 0x55, 0x41, 0x56, 0x41, 0x57, 0x49, 0x8d, 0xab, 0x88, 0xfe, 0xff, 0xff, 0x48, 0x81, 0xec, 0x60, 0x02, 0x00, 0x00, 0x48, 0x8b}},
    {AA_LOCO_ANIM, {0x4c, 0x8b, 0xdc, 0x55, 0x41, 0x54, 0x41, 0x55, 0x49, 0x8d, 0x6b, 0x98, 0x48, 0x81, 0xec, 0x50, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x05, 0x06, 0x09}},
};
static const GameFunction kAaParExe[2] = {
    {AA_RUN_JOB, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x44, 0x89, 0x4c, 0x24, 0x20, 0x55, 0x41, 0x54, 0x41}},
    {AA_PENDING, {0x48, 0x8b, 0x05, 0x71, 0xae, 0x96, 0x01, 0xba, 0x02, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x88, 0x18, 0x03, 0x00, 0x00, 0xe9, 0xa8, 0x1e, 0x58, 0x00}},
};
typedef void (*AaLocoPassFn)(void* unused, uint32_t start, uint32_t end, float dt);
typedef void (*AaLocoRangeFn)(void* manager, uint32_t start, uint32_t end, float dt);
typedef void (*AaLocoOneFn)(void* manager, uint32_t index, uint64_t zero);
typedef uint32_t (*AaRunJobFn)(void* fn, void** data, const char* name, uint32_t p4, int32_t priority, uint32_t count, void* p7);
typedef void (*AaPendingFn)(void);
typedef struct AaHeldWrite { uint32_t network, field; void* value; } AaHeldWrite;
static void* volatile g_aaLocoNext;
static int g_aaParLoco;
static uintptr_t g_aaExe;
static volatile LONG g_aaParActive, g_aaParCursor, g_aaParDone, g_aaParHeldCount;
static uint32_t g_aaParStart, g_aaParEnd, g_aaParTasks;
static float g_aaParDt;
static uint8_t g_aaParSerial[AB_MAX * 4];
static AaHeldWrite g_aaParHeld[AA_PAR_HELD];
static void* g_aaParData[AA_PAR_TASKS];
static LONG64 g_aaParWhy[3]; /* serial by: snap flag, linked entity (or linked to), Helldiver */
static LONG64 g_aaParFrames, g_aaParRecords, g_aaParSerialCount, g_aaParHeldTotal, g_aaParOverflow, g_aaParGtChunks;
static double g_aaParMs;
static _Thread_local int t_aaInPar;  /* this worker runs a chunk of a parallel pass: its field writes are held */
#define AA_LOCO_SNAP 0x9CACD0u       /* game: the turn's snap (?, record, duration): SendsRpcSnapRotation, or the rotation row's turn rate */
#define AA_PAR_SNAPS 4096u
typedef void (*AaSnapFn)(void* unused, const void* record, float duration);
typedef struct AaHeldSnap { uint32_t pad, entity; float duration; } AaHeldSnap;
static const uint8_t kAaSnapPin[24] = {0x48, 0x89, 0x5c, 0x24, 0x20, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xec, 0xe0, 0x00, 0x00,
    0x00, 0x44, 0x0f, 0x29, 0x64, 0x24, 0x70, 0x44};
static AaHeldSnap g_aaParSnaps[AA_PAR_SNAPS];
static volatile LONG g_aaParSnapCount;
static LONG64 g_aaParSnapTotal, g_aaParSnapLost;
static void* volatile g_aaSnapNext;
static int g_aaParSnap;  /* snap-flag records on the workers too, their 0x9CACD0 held and replayed (HELLFORGE_ASYNC_OFF=snap: serial) */

/* A chunk at a time from the shared cursor until none is left: the range part, then each record's turn and animation
 * parts (the serial ones are left for the game thread), then the record's +0x60 cleared as 0x9C7440 does. */
static int AaLocoChunks(void)
{
    uintptr_t m = AaGlobal(AA_LOCOMOTION);
    int took = 0;
    for (;;)
    {
        uint32_t a = g_aaParStart + (uint32_t)(InterlockedIncrement(&g_aaParCursor) - 1) * AA_PAR_CHUNK;
        if (a >= g_aaParEnd) return took;
        uint32_t b = a + AA_PAR_CHUNK < g_aaParEnd ? a + AA_PAR_CHUNK : g_aaParEnd;
        ((AaLocoRangeFn)(g_aaGame + AA_LOCO_RANGE))((void*)m, a, b, g_aaParDt);
        for (uint32_t i = a; i < b; i++) if (!(i < sizeof g_aaParSerial && g_aaParSerial[i])) ((AaLocoOneFn)(g_aaGame + AA_LOCO_TURN))((void*)m, i, 0);
        for (uint32_t i = a; i < b; i++)
        {
            if (i < sizeof g_aaParSerial && g_aaParSerial[i]) continue;
            ((AaLocoOneFn)(g_aaGame + AA_LOCO_ANIM))((void*)m, i, 0);
            *(uint32_t*)(*(uint8_t**)(m + 0x50) + (size_t)i * 0xA0 + 0x60) = 0;
        }
        took++;
    }
}

static uint64_t AaLocoTask(void* shed, void* task, void* channel, void* data)
{
    (void)shed; (void)task; (void)channel; (void)data;
    t_aaInPar = 1;
    AaLocoChunks();
    t_aaInPar = 0;
    InterlockedIncrement(&g_aaParDone);
    return 0;
}

/* QueueFieldWrite from a worker while the pass runs: held for the game thread (1 when it was taken). */
static int AaParHold(uint32_t network, uint32_t field, void* value)
{
    if (!t_aaInPar) return 0;
    LONG k = InterlockedIncrement(&g_aaParHeldCount) - 1;
    if (k >= (LONG)AA_PAR_HELD) { InterlockedIncrement64(&g_aaParOverflow); return 1; }
    g_aaParHeld[k].network = network; g_aaParHeld[k].field = field; g_aaParHeld[k].value = value;
    return 1;
}

/* The turn's snap (0x9CACD0, called by 0x9C94C0 when rec+0x91 is set and a turn animation was picked this frame): with
 * a zero duration it sends SendsRpcSnapRotation, else it sets the rotation row's turn rate when the turn is over 20
 * degrees. It reads only the record's entity (+8) and that entity's rotation row. From a worker it is held, and run on
 * the game thread after the join with the same entity and duration. */
static void AaSnap(void* unused, const uint8_t* record, float duration)
{
    if (!t_aaInPar) { ((AaSnapFn)g_aaSnapNext)(unused, record, duration); return; }
    LONG k = InterlockedIncrement(&g_aaParSnapCount) - 1;
    if (k >= (LONG)AA_PAR_SNAPS) { InterlockedIncrement64(&g_aaParSnapLost); return; }
    g_aaParSnaps[k].pad = 0; g_aaParSnaps[k].entity = *(const uint32_t*)(record + 8); g_aaParSnaps[k].duration = duration;
}

/* After a join, on the game thread: the held field writes, then the held snaps. */
static void AaParReplay(void)
{
    LONG held = g_aaParHeldCount < (LONG)AA_PAR_HELD ? g_aaParHeldCount : (LONG)AA_PAR_HELD;
    for (LONG k = 0; k < held; k++) ((AaFieldFn)g_aaFieldNext)(g_aaParHeld[k].network, g_aaParHeld[k].field, g_aaParHeld[k].value);
    g_aaParHeldTotal += held;
    LONG snaps = g_aaParSnapCount < (LONG)AA_PAR_SNAPS ? g_aaParSnapCount : (LONG)AA_PAR_SNAPS;
    for (LONG k = 0; k < snaps; k++) ((AaSnapFn)g_aaSnapNext)(NULL, &g_aaParSnaps[k], g_aaParSnaps[k].duration);
    g_aaParSnapTotal += snaps;
    g_aaParHeldCount = 0; g_aaParSnapCount = 0;
}

/* The records that stay on the game thread: a linked entity in the locomotion map (its animation part writes that
 * record), the snap flag (an RPC), the Helldivers. */
static void AaLocoSerial(uintptr_t m, uint32_t start, uint32_t end)
{
    EvpPlayer players[EVT_MAX_PLAYERS];
    int np = EvpPlayers(g_aaGame, players);
    uint8_t* recs = *(uint8_t**)(m + 0x50);
    uint8_t** descs = *(uint8_t***)(m + 0x38);
    for (uint32_t i = start; i < end && i < sizeof g_aaParSerial; i++)
    {
        const uint8_t* r = recs + (size_t)i * 0xA0;
        uint32_t entity = descs && descs[i] ? *(uint32_t*)(descs[i] + 8) : 0, linked = *(const uint32_t*)(r + 0x5C);
        int snap = r[0x91] != 0, serial = snap && !g_aaParSnap;
        g_aaParWhy[0] += snap;
        int64_t li = linked && linked != 0xFFFFFFFFu ? BhvMapFind(m, 0x20, linked) : -1;
        if (li >= 0) { serial = 1; g_aaParWhy[1]++; if ((uint64_t)li < sizeof g_aaParSerial) g_aaParSerial[li] = 1; }
        for (int p = 0; p < np; p++) if (players[p].avatar == entity) { serial = 1; g_aaParWhy[2]++; }
        if (serial) g_aaParSerial[i] = 1;
    }
}

static void AaLoco(void* unused, uint32_t start, uint32_t end, float dt)
{
    uintptr_t m = AaGlobal(AA_LOCOMOTION);
    if (!g_aaOn || !g_aaParLoco || !m || end <= start || end - start < 2u * AA_PAR_CHUNK || end > sizeof g_aaParSerial
        || GetCurrentThreadId() != g_aaGameThread)
    { ((AaLocoPassFn)g_aaLocoNext)(unused, start, end, dt); return; }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    memset(g_aaParSerial + start, 0, end - start);
    AaLocoSerial(m, start, end);
    g_aaParStart = start; g_aaParEnd = end; g_aaParDt = dt;
    g_aaParCursor = 0; g_aaParDone = 0; g_aaParHeldCount = 0; g_aaParSnapCount = 0;
    uint32_t chunks = (end - start + AA_PAR_CHUNK - 1) / AA_PAR_CHUNK;
    g_aaParTasks = chunks - 1 < AA_PAR_TASKS ? chunks - 1 : AA_PAR_TASKS;
    InterlockedExchange(&g_aaParActive, 1);
    if (g_aaParTasks) ((AaRunJobFn)(g_aaExe + AA_RUN_JOB))((void*)AaLocoTask, g_aaParData, "hellforge_locomotion", 0, 1, g_aaParTasks, NULL);
    g_aaParGtChunks += AaLocoChunks();
    while ((uint32_t)g_aaParDone < g_aaParTasks) ((AaPendingFn)(g_aaExe + AA_PENDING))();
    InterlockedExchange(&g_aaParActive, 0);
    AaParReplay();
    /* the serial records, in 0x9C7440's order */
    uint32_t serial = 0;
    for (uint32_t i = start; i < end; i++) if (g_aaParSerial[i]) { ((AaLocoOneFn)(g_aaGame + AA_LOCO_TURN))((void*)m, i, 0); serial++; }
    for (uint32_t i = start; i < end; i++)
        if (g_aaParSerial[i])
        {
            ((AaLocoOneFn)(g_aaGame + AA_LOCO_ANIM))((void*)m, i, 0);
            *(uint32_t*)(*(uint8_t**)(m + 0x50) + (size_t)i * 0xA0 + 0x60) = 0;
        }
    QueryPerformanceCounter(&t1);
    g_aaParMs += AaMs(t0, t1);
    g_aaParFrames++; g_aaParRecords += end - start; g_aaParSerialCount += serial;
}

/* Phase 4, motion (HELLFORGE_ASYNC_PAR=motion): the motion pass (0x59F440: MoveObject 0x59FED0 for the simulated rows,
 * the proxy pass 0x5A18A0 for the rest) in chunks on the job workers the same way. A row runs on a worker when it is
 * simulated and idle (source +0x20 clear) or in mode 2 / 3 / 4 / 7 (straight, clipped, ground-following: own rows, the
 * boundary clip's reads, engine and actor reads, field writes held). Mode 1 (MoverApi moves: the Helldivers, vehicles),
 * mode 5, mode 6 (its end sends an RPC) and the proxy rows run on the game thread after the join, in order. A row that
 * left the map (+-1900 m) asks RemoveEntity (0xFDC310) from inside MoveObject: from a worker that is held and done on
 * the game thread after the join (MoveObject returns right after it). The edge gathers the clip reads are joined
 * before the vehicle collision pass, long before motion. */
#define AA_MOTION_PASS 0x59F440u
#define AA_MOVE_OBJECT 0x59FED0u
#define AA_MOVE_PROXY 0x5A18A0u
#define AA_REMOVE_ENTITY 0xFDC310u
#define AA_MOTION 0x3326558u          /* MotionManager: +0x4890 simulated rows, +0x48D8 sources (0x2C: +0x1C mode, +0x20 on) */
#define AA_PAR_REMOVALS 1024u
static const GameFunction kAaParMotion[4] = {
    {AA_MOTION_PASS, {0x41, 0x3b, 0xd0, 0x73, 0x61, 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0x3d, 0xfd}},
    {AA_MOVE_OBJECT, {0x40, 0x55, 0x53, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x58, 0xff, 0xff, 0xff, 0x48, 0x81, 0xec, 0xa8, 0x01, 0x00}},
    {AA_MOVE_PROXY, {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x48, 0x8d, 0x6c, 0x24, 0x90, 0x48, 0x81, 0xec, 0x70, 0x01, 0x00, 0x00, 0x44, 0x0f, 0x29}},
    {AA_REMOVE_ENTITY, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x3b, 0x15, 0x4c, 0x82, 0x4a, 0x02, 0x8b, 0xda, 0x48, 0x8b, 0xf9, 0x0f, 0x84, 0xd4}},
};
typedef void (*AaMoveFn)(void* motion, uint32_t index, float dt);
typedef void (*AaRemoveFn)(void* entities, uint32_t entity);
static void* volatile g_aaMotionNext;
static void* volatile g_aaRemoveNext;
static int g_aaParMotion;
static uint8_t g_aaMoSerial[AB_MAX * 4];
static void* g_aaRemoveMgr[AA_PAR_REMOVALS];
static uint32_t g_aaRemoveEnt[AA_PAR_REMOVALS];
static volatile LONG g_aaRemoveCount;
static LONG64 g_aaMoFrames, g_aaMoRows, g_aaMoSerialCount, g_aaMoGtChunks, g_aaMoRemoved, g_aaMoModes[8];
static double g_aaMoMs;

static void AaRemove(void* entities, uint32_t entity)
{
    if (!t_aaInPar) { ((AaRemoveFn)g_aaRemoveNext)(entities, entity); return; }
    LONG k = InterlockedIncrement(&g_aaRemoveCount) - 1;
    if (k >= (LONG)AA_PAR_REMOVALS) return; /* the row stays out of the map; it asks again next frame */
    g_aaRemoveMgr[k] = entities; g_aaRemoveEnt[k] = entity;
}

static int AaMotionChunks(void)
{
    uintptr_t mm = AaGlobal(AA_MOTION);
    int took = 0;
    for (;;)
    {
        uint32_t a = g_aaParStart + (uint32_t)(InterlockedIncrement(&g_aaParCursor) - 1) * AA_PAR_CHUNK;
        if (a >= g_aaParEnd) return took;
        uint32_t b = a + AA_PAR_CHUNK < g_aaParEnd ? a + AA_PAR_CHUNK : g_aaParEnd;
        for (uint32_t i = a; i < b; i++) if (!g_aaMoSerial[i]) ((AaMoveFn)(g_aaGame + AA_MOVE_OBJECT))((void*)mm, i, g_aaParDt);
        took++;
    }
}

static uint64_t AaMotionTask(void* shed, void* task, void* channel, void* data)
{
    (void)shed; (void)task; (void)channel; (void)data;
    t_aaInPar = 1;
    AaMotionChunks();
    t_aaInPar = 0;
    InterlockedIncrement(&g_aaParDone);
    return 0;
}

static void AaMotion(void* unused, uint32_t start, uint32_t end, float dt)
{
    uintptr_t mm = AaGlobal(AA_MOTION);
    if (!g_aaOn || !g_aaParMotion || !mm || end <= start || end - start < 2u * AA_PAR_CHUNK || end > sizeof g_aaMoSerial
        || GetCurrentThreadId() != g_aaGameThread)
    { ((AaLocoPassFn)g_aaMotionNext)(unused, start, end, dt); return; }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    uint32_t simulated = *(uint32_t*)(mm + 0x4890);
    const uint8_t* src = *(uint8_t**)(mm + 0x48D8);
    for (uint32_t i = start; i < end; i++)
    {
        int serial = 1;
        if (i < simulated)
        {
            const uint8_t* r = src + (size_t)i * 0x2C;
            uint32_t mode = *(const uint32_t*)(r + 0x1C);
            g_aaMoModes[mode < 8 ? mode : 0] += r[0x20] != 0;
            serial = r[0x20] != 0 && !(mode == 2 || mode == 3 || mode == 4 || mode == 7);
        }
        g_aaMoSerial[i] = (uint8_t)serial;
    }
    g_aaParStart = start; g_aaParEnd = end; g_aaParDt = dt;
    g_aaParCursor = 0; g_aaParDone = 0; g_aaParHeldCount = 0; g_aaParSnapCount = 0; g_aaRemoveCount = 0;
    uint32_t chunks = (end - start + AA_PAR_CHUNK - 1) / AA_PAR_CHUNK;
    g_aaParTasks = chunks - 1 < AA_PAR_TASKS ? chunks - 1 : AA_PAR_TASKS;
    InterlockedExchange(&g_aaParActive, 1);
    if (g_aaParTasks) ((AaRunJobFn)(g_aaExe + AA_RUN_JOB))((void*)AaMotionTask, g_aaParData, "hellforge_motion", 0, 1, g_aaParTasks, NULL);
    g_aaMoGtChunks += AaMotionChunks();
    while ((uint32_t)g_aaParDone < g_aaParTasks) ((AaPendingFn)(g_aaExe + AA_PENDING))();
    InterlockedExchange(&g_aaParActive, 0);
    AaParReplay();
    LONG removals = g_aaRemoveCount < (LONG)AA_PAR_REMOVALS ? g_aaRemoveCount : (LONG)AA_PAR_REMOVALS;
    for (LONG k = 0; k < removals; k++) ((AaRemoveFn)g_aaRemoveNext)(g_aaRemoveMgr[k], g_aaRemoveEnt[k]);
    g_aaMoRemoved += removals; g_aaRemoveCount = 0;
    uint32_t serial = 0;
    for (uint32_t i = start; i < end; i++)
        if (g_aaMoSerial[i])
        {
            if (i < *(uint32_t*)(mm + 0x4890)) ((AaMoveFn)(g_aaGame + AA_MOVE_OBJECT))((void*)mm, i, dt);
            else ((AaMoveFn)(g_aaGame + AA_MOVE_PROXY))((void*)mm, i, dt);
            serial++;
        }
    QueryPerformanceCounter(&t1);
    g_aaMoMs += AaMs(t0, t1);
    g_aaMoFrames++; g_aaMoRows += end - start; g_aaMoSerialCount += serial;
}

static void AaParHooks(uintptr_t game)
{
    g_aaExe = (uintptr_t)GetModuleHandleW(NULL);
    for (int f = 0; f < 2; f++) if (!PinMatches(g_aaExe + kAaParExe[f].rva, kAaParExe[f].pin, 24)) { g_aaParLoco = g_aaParMotion = 0; Note("async-ai: par off; the job runner is not the reviewed code"); return; }
    if (!g_aaFieldNext && LoaderHook("async-ai: QueueFieldWrite", game + AA_FIELD_WRITE, (void*)AaFieldWrite, (void**)&g_aaFieldNext) != 0)
    { g_aaParLoco = g_aaParMotion = 0; Note("async-ai: par off; QueueFieldWrite could not be hooked"); return; }
    if (g_aaParLoco)
    {
        for (int f = 0; f < 4; f++) if (!PinMatches(game + kAaParGame[f].rva, kAaParGame[f].pin, 24)) { g_aaParLoco = 0; Note("async-ai: par loco off; the locomotion pass is not the reviewed code"); break; }
        if (g_aaParLoco && g_aaParSnap && (!PinMatches(game + AA_LOCO_SNAP, kAaSnapPin, 24) ||
            LoaderHook("async-ai: the turn's snap", game + AA_LOCO_SNAP, (void*)AaSnap, (void**)&g_aaSnapNext) != 0))
        { g_aaParSnap = 0; Note("async-ai: par snap off; 0x9CACD0 is not the reviewed code or could not be hooked: snap-flag records stay serial"); }
        if (g_aaParLoco && LoaderHook("async-ai: the locomotion pass", game + AA_LOCO_PASS, (void*)AaLoco, (void**)&g_aaLocoNext) != 0)
        { g_aaParLoco = 0; Note("async-ai: par loco off; the locomotion pass could not be hooked"); }
        if (g_aaParLoco) Note("async-ai: par loco on: the locomotion pass runs in chunks of %u on the job workers; snap-flag records %s", AA_PAR_CHUNK,
            g_aaParSnap ? "too (their snaps held and replayed)" : "on the game thread (HELLFORGE_ASYNC_OFF=snap)");
    }
    if (g_aaParMotion)
    {
        for (int f = 0; f < 4; f++) if (!PinMatches(game + kAaParMotion[f].rva, kAaParMotion[f].pin, 24)) { g_aaParMotion = 0; Note("async-ai: par motion off; the motion pass is not the reviewed code"); break; }
        if (g_aaParMotion && LoaderHook("async-ai: RemoveEntity", game + AA_REMOVE_ENTITY, (void*)AaRemove, (void**)&g_aaRemoveNext) != 0)
        { g_aaParMotion = 0; Note("async-ai: par motion off; RemoveEntity could not be hooked"); }
        if (g_aaParMotion && LoaderHook("async-ai: the motion pass", game + AA_MOTION_PASS, (void*)AaMotion, (void**)&g_aaMotionNext) != 0)
        { g_aaParMotion = 0; Note("async-ai: par motion off; the motion pass could not be hooked"); }
        if (g_aaParMotion) Note("async-ai: par motion on: the motion pass runs in chunks of %u on the job workers (modes 2, 3, 4, 7 and idle rows)", AA_PAR_CHUNK);
    }
}

static void AaParReport(void)
{
    if (g_aaParLoco && g_aaParFrames > 0)
    {
        double f = (double)g_aaParFrames;
        Note("async-ai: par: locomotion %.3f ms a frame on the game thread for %.0f records (%.1f kept on the game thread, %.1f chunks taken by it), "
            "snap flag on %.1f, linked entity %.1f, Helldiver %.1f a frame; %.2f snaps held and replayed a frame (%lld lost)",
            g_aaParMs / f, (double)g_aaParRecords / f, (double)g_aaParSerialCount / f, (double)g_aaParGtChunks / f,
            (double)g_aaParWhy[0] / f, (double)g_aaParWhy[1] / f, (double)g_aaParWhy[2] / f, (double)g_aaParSnapTotal / f, (long long)g_aaParSnapLost);
    }
    if (g_aaParMotion && g_aaMoFrames > 0)
    {
        double f = (double)g_aaMoFrames;
        Note("async-ai: par: motion %.3f ms a frame on the game thread for %.0f rows (%.1f kept on the game thread, %.1f chunks taken by it), "
            "%lld removals held; active rows by mode 0-7: %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f a frame", g_aaMoMs / f, (double)g_aaMoRows / f,
            (double)g_aaMoSerialCount / f, (double)g_aaMoGtChunks / f, (long long)g_aaMoRemoved, g_aaMoModes[0] / f, g_aaMoModes[1] / f,
            g_aaMoModes[2] / f, g_aaMoModes[3] / f, g_aaMoModes[4] / f, g_aaMoModes[5] / f, g_aaMoModes[6] / f, g_aaMoModes[7] / f);
    }
    double frames = (double)(g_aaParFrames > g_aaMoFrames ? g_aaParFrames : g_aaMoFrames);
    if (frames > 0) Note("async-ai: par: %.0f field writes held and replayed a frame, %lld lost", (double)g_aaParHeldTotal / frames, (long long)g_aaParOverflow);
    memset(g_aaParWhy, 0, sizeof g_aaParWhy); memset(g_aaMoModes, 0, sizeof g_aaMoModes);
    g_aaParMs = g_aaMoMs = 0.0;
    g_aaParFrames = g_aaParRecords = g_aaParSerialCount = g_aaParHeldTotal = g_aaParGtChunks = g_aaParSnapTotal = 0;
    g_aaMoFrames = g_aaMoRows = g_aaMoSerialCount = g_aaMoGtChunks = g_aaMoRemoved = 0;
}

/* Phase 3: the per-frame fields (locomotion's velocity, motion's bool) are queued when they change, and at least
 * twice a second, instead of every frame for every enemy. */
static void AaFieldWrite(uint32_t network, uint32_t field, void* value)
{
    AaFieldFn next = (AaFieldFn)g_aaFieldNext;
    if (AaParHold(network, field, value)) return;
    if (g_aaFieldsOn && value && network < 0x7FFFu && (field == AA_VELOCITY_FIELD || field == AA_MOVER_FIELD))
    {
        AaField* f = &g_aaFields[network];
        ULONGLONG now = GetTickCount64();
        g_aaFieldCalls++;
        if (field == AA_VELOCITY_FIELD)
        {
            if (f->has && !memcmp(f->v, value, 12) && now - f->at < 500) { g_aaFieldSkips++; return; }
            memcpy(f->v, value, 12); f->has = 1; f->at = now;
        }
        else
        {
            if (f->hasB && f->b == *(uint8_t*)value && now - f->atB < 500) { g_aaFieldSkips++; return; }
            f->b = *(uint8_t*)value; f->hasB = 1; f->atB = now;
        }
    }
    next(network, field, value);
}


/* The avoidance meter: the CPU time of Havok AI's collision avoidance work (exe 0xBA2910: the task part that gathers
 * each character's neighbours and boundaries and solves its velocity; threads + 4 parts a step, on the Havok task
 * queue's threads, the game thread among them), summed over threads, and its part on the game thread. */
#define AA_AVOID_TASK 0xBA2910u
static const uint8_t kAaAvoidTaskPin[24] = {0x4c, 0x8b, 0xdc, 0x53, 0x55, 0x41, 0x56, 0x48, 0x83, 0xec, 0x70, 0x4c, 0x8b, 0x89, 0x00, 0x09,
    0x00, 0x00, 0x48, 0x8d, 0x69, 0x10, 0x48, 0x63};
typedef void (*AaAvoidTaskFn)(void* task, void* part);
static void* volatile g_aaAvoidTaskNext;
static volatile LONG64 g_aaAvoidTicks, g_aaAvoidTicksGt, g_aaAvoidParts;


static void AaAvoidTask(void* task, void* part)
{
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    ((AaAvoidTaskFn)g_aaAvoidTaskNext)(task, part);
    QueryPerformanceCounter(&b);
    InterlockedAdd64(&g_aaAvoidTicks, b.QuadPart - a.QuadPart);
    if (GetCurrentThreadId() == g_aaGameThread) InterlockedAdd64(&g_aaAvoidTicksGt, b.QuadPart - a.QuadPart);
    InterlockedIncrement64(&g_aaAvoidParts);
}

/* The solve alone (exe 0xBA3A10, once per character a step): its CPU time and count. */
#define AA_AVOID_SOLVE 0xBA3A10u
static const uint8_t kAaAvoidSolvePin[24] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
static void* volatile g_aaAvoidSolveNext;
static volatile LONG64 g_aaSolveTicks, g_aaSolves;
static void AaAvoidSolve(void* a, void* b)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    ((AaAvoidTaskFn)g_aaAvoidSolveNext)(a, b);
    QueryPerformanceCounter(&t1);
    InterlockedAdd64(&g_aaSolveTicks, t1.QuadPart - t0.QuadPart);
    InterlockedIncrement64(&g_aaSolves);
}

static void AaMeterHook(void)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (!PinMatches(exe + AA_AVOID_TASK, kAaAvoidTaskPin, 24)) { Note("async-ai: avoidance meter off; exe 0xBA2910 is not the reviewed code"); return; }
    if (LoaderHook("async-ai: the avoidance meter", exe + AA_AVOID_TASK, (void*)AaAvoidTask, (void**)&g_aaAvoidTaskNext) != 0)
        Note("async-ai: avoidance meter off; it could not be hooked");
    if (PinMatches(exe + AA_AVOID_SOLVE, kAaAvoidSolvePin, 24))
        LoaderHook("async-ai: the avoidance solve meter", exe + AA_AVOID_SOLVE, (void*)AaAvoidSolve, (void**)&g_aaAvoidSolveNext);
}

static void AaMeterReport(double frames)
{
    LARGE_INTEGER hz;
    QueryPerformanceFrequency(&hz);
    LONG64 t = InterlockedExchange64(&g_aaAvoidTicks, 0), g = InterlockedExchange64(&g_aaAvoidTicksGt, 0), n = InterlockedExchange64(&g_aaAvoidParts, 0);
    LONG64 st = InterlockedExchange64(&g_aaSolveTicks, 0), sn = InterlockedExchange64(&g_aaSolves, 0);
    if (frames <= 0.0 || n == 0) return;
    Note("async-ai: avoidance meter: the per-character solve %.3f ms a frame over all threads, %.0f characters a frame (%.1f us each)",
        (double)st * 1000.0 / (double)hz.QuadPart / frames, (double)sn / frames, sn ? (double)st * 1e6 / (double)hz.QuadPart / (double)sn : 0.0);
    Note("async-ai: avoidance meter: Havok AI avoidance work %.3f ms a frame over all threads (%.3f ms of it on the game thread), %.1f task parts a frame",
        (double)t * 1000.0 / (double)hz.QuadPart / frames, (double)g * 1000.0 / (double)hz.QuadPart / frames, (double)n / frames);
}

/* Step 1 of the avoidance work (research\async-ai\README.md): where the frame waits on Havok AI avoidance and what the
 * boundary gather costs, and the edge cache.
 * - The graph (exe 0xB9C8C0, built and submitted on the physics step's substep 0, before the collide graph) runs
 *   InitContext, the character / obstacle AABB gathers and sorts, the neighbour scans, then GatherBoundariesAndSolve
 *   (threads + 4 parts, exe 0xBA2910: per character the boundary gatherer's vfunc +0x28, then the solve 0xBA3A10),
 *   ApplySolverOutput and Finalize (exe 0xBA1D80). The game thread waits for it after the step loop (the queue's wait,
 *   exe 0x8AA260, called from ret 0x79F1E3; the collide wait from 0x79EFC3, the solve wait from 0x79F014).
 * - The gather (DefaultBoundaryGatherer::vf5, exe 0xB9E8D0): the closest navmesh face to the character (kept at
 *   character +0x20), its sensor box (position +-[+0xAC] across, +0xB0 up, +0xB4 down: exe 0xB9DD70), then every
 *   navmesh boundary edge in that box (exe 0xBC31E0), appended to the character's array (0x20 bytes an edge).
 * - The edge cache (HELLFORGE_ASYNC_OFF=edges turns it off): the edges are navmesh geometry, so a character that moved
 *   less than HELLFORGE_ASYNC_EDGE_MOVE m (0.35) since its last gather, with the same sensor and radius, gets the same
 *   edges again for up to HELLFORGE_ASYNC_EDGE_AGE frames (12) instead of a new navmesh walk. Every character keeps its
 *   Havok AI avoidance (the navigator needs it); its solve runs every frame as before. All characters of the world use
 *   it, not only ours: the gather does not know whose character it is. */
#define AA_GATHER 0xB9E8D0u
#define AA_QUEUE_WAIT 0x8AA260u
#define AA_GRAPH_BUILD 0xB9C8C0u
#define AA_GRAPH_FINAL 0xBA1D80u
#define AA_HK_ALLOCATOR 0x1899880u   /* exe: the Havok container allocator (value) */
#define AA_HK_RESERVE 0x99A3B0u      /* exe: hkArrayUtil reserve-more (allocator, array, element size) */
#define AA_WAIT_AVOID 0x79F1E3u
#define AA_WAIT_COLLIDE 0x79EFC3u
#define AA_WAIT_SOLVE 0x79F014u
#define AA_EC_SLOTS 4096u
#define AA_EC_EDGES 128
#define AA_EC_PROBE 8
static const uint8_t kAaGatherPin[24] = {0x48, 0x8b, 0xc4, 0x55, 0x48, 0x8d, 0x68, 0xb8, 0x48, 0x81, 0xec, 0x40, 0x01, 0x00, 0x00, 0x48,
    0x89, 0x58, 0x10, 0x48, 0x8b, 0xda, 0x48, 0x89};
static const uint8_t kAaQueueWaitPin[24] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x01, 0x48, 0x8d, 0x54, 0x24,
    0x38, 0x41, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xff};
static const uint8_t kAaGraphBuildPin[24] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c};
static const uint8_t kAaGraphFinalPin[24] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x8b, 0xfa, 0x48,
    0x8b, 0x49, 0x30, 0x48, 0x85, 0xc9, 0x74, 0x05};
/* The call sites' bytes: call [rax+0x60] / call [r8+0x60] just before the return addresses. */
static const uint8_t kAaWaitAvoidPin[3] = {0xff, 0x50, 0x60};
static const uint8_t kAaWaitCollidePin[4] = {0x41, 0xff, 0x50, 0x60};

typedef struct AaHkArray { uint8_t* data; int32_t size; int32_t cap; } AaHkArray;
typedef struct AaEdgeSlot
{
    void* volatile key;
    volatile LONG busy;
    uint32_t tick;
    void* gatherer;
    float pos[3], sensor, radius;
    int32_t count;
    uint8_t edges[AA_EC_EDGES][32];
} AaEdgeSlot;
typedef void (*AaGatherFn)(void* gatherer, uint8_t* character, AaHkArray* out);
typedef void (*AaQueueWaitFn)(void* queue, void* handle);
typedef int16_t* (*AaGraphBuildFn)(int16_t* out, void* graph, void* context, uint32_t parts, uint64_t after);
typedef void (*AaGraphFinalFn)(void* task, uint32_t part);
typedef void (*AaReserveFn)(void* allocator, AaHkArray* array, int32_t size);

static void* volatile g_aaGatherNext;
static void* volatile g_aaQueueWaitNext;
static void* volatile g_aaGraphBuildNext;
static void* volatile g_aaGraphFinalNext;
static uintptr_t g_aaMeterExe;
static AaEdgeSlot* g_aaEdgeSlots;
static int g_aaEdgesOn;
static float g_aaEdgeMove = 0.35f;
static uint32_t g_aaEdgeAge = 12;
static volatile LONG g_aaTick;     /* AiEnemy passes since start: the frame count for the cache's age */
static volatile LONG64 g_aaGatherTicks, g_aaGathers, g_aaGatherEdges, g_aaEdgeHits, g_aaEdgeMisses, g_aaEdgeBig, g_aaEdgeBusy;
static volatile LONG64 g_aaGatherSensor; /* sum of the sensors across, in mm */
static volatile LONG64 g_aaWaitTicks[3], g_aaWaits[3];
static volatile LONG64 g_aaGraphStart, g_aaGraphTicks, g_aaGraphs, g_aaGraphMax;

static int AaPartOn(const char* name);

static AaEdgeSlot* AaEdgeFind(void* ch, int* fresh)
{
    uint64_t h = ((uint64_t)(uintptr_t)ch >> 4) * 0x9E3779B97F4A7C15ull;
    uint32_t i = (uint32_t)(h >> 52) & (AA_EC_SLOTS - 1);
    AaEdgeSlot* spare = NULL;
    uint32_t now = (uint32_t)g_aaTick;
    for (int p = 0; p < AA_EC_PROBE; p++)
    {
        AaEdgeSlot* s = &g_aaEdgeSlots[(i + (uint32_t)p) & (AA_EC_SLOTS - 1)];
        void* k = s->key;
        if (k == ch) { *fresh = 0; return s; }
        if (!spare && (k == NULL || now - s->tick > g_aaEdgeAge * 8u)) spare = s;
    }
    *fresh = 1;
    return spare;
}

static void AaGatherAppend(AaHkArray* out, const AaEdgeSlot* s)
{
    for (int32_t e = 0; e < s->count; e++)
    {
        if (out->size == (out->cap & 0x3fffffff))
            ((AaReserveFn)(g_aaMeterExe + AA_HK_RESERVE))(*(void**)(g_aaMeterExe + AA_HK_ALLOCATOR), out, 0x20);
        memcpy(out->data + (size_t)out->size * 0x20, s->edges[e], 0x20);
        out->size++;
    }
}

static void AaGatherCount(LARGE_INTEGER a, int32_t edges, float sensor)
{
    LARGE_INTEGER b;
    QueryPerformanceCounter(&b);
    InterlockedAdd64(&g_aaGatherTicks, b.QuadPart - a.QuadPart);
    InterlockedIncrement64(&g_aaGathers);
    InterlockedAdd64(&g_aaGatherEdges, edges);
    InterlockedAdd64(&g_aaGatherSensor, (LONG64)(sensor * 1000.0f));
}

static void AaGather(void* gatherer, uint8_t* ch, AaHkArray* out)
{
    LARGE_INTEGER a;
    QueryPerformanceCounter(&a);
    const float* pos = (const float*)(ch + 0x30);
    float sensor = *(float*)(ch + 0xAC), radius = *(float*)(ch + 0xA0);
    AaEdgeSlot* s = NULL;
    int fresh = 1;
    if (g_aaEdgesOn)
    {
        s = AaEdgeFind(ch, &fresh);
        if (s && InterlockedCompareExchange(&s->busy, 1, 0) != 0) { s = NULL; InterlockedIncrement64(&g_aaEdgeBusy); }
    }
    if (s && !fresh && s->key == ch && s->gatherer == gatherer && s->sensor == sensor && s->radius == radius &&
        (uint32_t)g_aaTick - s->tick <= g_aaEdgeAge)
    {
        float dx = pos[0] - s->pos[0], dy = pos[1] - s->pos[1], dz = pos[2] - s->pos[2];
        if (dx * dx + dy * dy + dz * dz < g_aaEdgeMove * g_aaEdgeMove)
        {
            AaGatherAppend(out, s);
            int32_t n = s->count;
            InterlockedExchange(&s->busy, 0);
            InterlockedIncrement64(&g_aaEdgeHits);
            AaGatherCount(a, n, sensor);
            return;
        }
    }
    int32_t start = out->size;
    ((AaGatherFn)g_aaGatherNext)(gatherer, ch, out);
    int32_t n = out->size - start;
    if (s)
    {
        if (n >= 0 && n <= AA_EC_EDGES)
        {
            s->key = ch; s->gatherer = gatherer; s->tick = (uint32_t)g_aaTick;
            s->pos[0] = pos[0]; s->pos[1] = pos[1]; s->pos[2] = pos[2]; s->sensor = sensor; s->radius = radius;
            memcpy(s->edges, out->data + (size_t)start * 0x20, (size_t)n * 0x20);
            s->count = n;
        }
        else { s->key = NULL; InterlockedIncrement64(&g_aaEdgeBig); }
        InterlockedExchange(&s->busy, 0);
        InterlockedIncrement64(&g_aaEdgeMisses);
    }
    AaGatherCount(a, n, sensor);
}

/* The queue's wait: timed for the step's three waits on the game thread. */
static void AaQueueWait(void* queue, void* handle)
{
    uintptr_t from = (uintptr_t)__builtin_return_address(0) - g_aaMeterExe;
    int k = from == AA_WAIT_AVOID ? 0 : from == AA_WAIT_COLLIDE ? 1 : from == AA_WAIT_SOLVE ? 2 : -1;
    if (k < 0 || GetCurrentThreadId() != g_aaGameThread) { ((AaQueueWaitFn)g_aaQueueWaitNext)(queue, handle); return; }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    ((AaQueueWaitFn)g_aaQueueWaitNext)(queue, handle);
    QueryPerformanceCounter(&b);
    InterlockedAdd64(&g_aaWaitTicks[k], b.QuadPart - a.QuadPart);
    InterlockedIncrement64(&g_aaWaits[k]);
}

/* The graph's wall time: from its build (game thread, substep 0) to the end of its Finalize task. */
static int16_t* AaGraphBuild(int16_t* out, void* graph, void* context, uint32_t parts, uint64_t after)
{
    LARGE_INTEGER a;
    QueryPerformanceCounter(&a);
    InterlockedExchange64(&g_aaGraphStart, a.QuadPart);
    return ((AaGraphBuildFn)g_aaGraphBuildNext)(out, graph, context, parts, after);
}

static void AaGraphFinal(void* task, uint32_t part)
{
    ((AaGraphFinalFn)g_aaGraphFinalNext)(task, part);
    LARGE_INTEGER b;
    QueryPerformanceCounter(&b);
    LONG64 s = InterlockedExchange64(&g_aaGraphStart, 0);
    if (s == 0) return;
    LONG64 d = b.QuadPart - s;
    InterlockedAdd64(&g_aaGraphTicks, d);
    InterlockedIncrement64(&g_aaGraphs);
    if (d > g_aaGraphMax) g_aaGraphMax = d;
}

static float AaEnvFloat(const char* name, float fallback)
{
    char v[16];
    DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    return n > 0 && n < sizeof v ? (float)atof(v) : fallback;
}

/* The meters always (with async-ai on, or HELLFORGE_ASYNC_DIAG=1 with it off); the cache only with async-ai on. */
static void AaEdgeHooks(void)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    g_aaMeterExe = exe;
    if (PinMatches(exe + AA_QUEUE_WAIT, kAaQueueWaitPin, 24) && PinMatches(exe + AA_WAIT_AVOID - 3, kAaWaitAvoidPin, 3) &&
        PinMatches(exe + AA_WAIT_COLLIDE - 4, kAaWaitCollidePin, 4))
        LoaderHook("async-ai: the step's waits meter", exe + AA_QUEUE_WAIT, (void*)AaQueueWait, (void**)&g_aaQueueWaitNext);
    else Note("async-ai: the step's waits meter off; exe 0x8AA260 or its callers are not the reviewed code");
    if (PinMatches(exe + AA_GRAPH_BUILD, kAaGraphBuildPin, 24) && PinMatches(exe + AA_GRAPH_FINAL, kAaGraphFinalPin, 24))
    {
        LoaderHook("async-ai: the avoidance graph's build", exe + AA_GRAPH_BUILD, (void*)AaGraphBuild, (void**)&g_aaGraphBuildNext);
        LoaderHook("async-ai: the avoidance graph's finalize", exe + AA_GRAPH_FINAL, (void*)AaGraphFinal, (void**)&g_aaGraphFinalNext);
    }
    if (!PinMatches(exe + AA_GATHER, kAaGatherPin, 24)) { Note("async-ai: edges off; exe 0xB9E8D0 is not the reviewed code"); return; }
    g_aaEdgesOn = g_aaTypeCount > 0 && AaPartOn("edges");
    g_aaEdgeMove = AaEnvFloat("HELLFORGE_ASYNC_EDGE_MOVE", 0.35f);
    g_aaEdgeAge = (uint32_t)AaEnvFloat("HELLFORGE_ASYNC_EDGE_AGE", 12.0f);
    if (g_aaEdgesOn)
    {
        g_aaEdgeSlots = (AaEdgeSlot*)VirtualAlloc(NULL, sizeof(AaEdgeSlot) * AA_EC_SLOTS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_aaEdgeSlots) { g_aaEdgesOn = 0; Note("async-ai: edges off; no memory for the cache"); }
    }
    if (LoaderHook("async-ai: the boundary gather", exe + AA_GATHER, (void*)AaGather, (void**)&g_aaGatherNext) != 0)
    { g_aaEdgesOn = 0; Note("async-ai: edges off; the boundary gather could not be hooked"); return; }
    if (g_aaEdgesOn) Note("async-ai: edges on: a character's navmesh boundary edges are reused while it moved under %.2f m, for up to %u frames (%u slots of %d edges)",
        g_aaEdgeMove, g_aaEdgeAge, AA_EC_SLOTS, AA_EC_EDGES);
    else Note("async-ai: edges off (the gather metered only)");
}

static void AaEdgeReport(double frames)
{
    LARGE_INTEGER hz;
    QueryPerformanceFrequency(&hz);
    double ms = 1000.0 / (double)hz.QuadPart;
    if (frames <= 0.0) return;
    LONG64 gt = InterlockedExchange64(&g_aaGatherTicks, 0), gn = InterlockedExchange64(&g_aaGathers, 0);
    LONG64 ge = InterlockedExchange64(&g_aaGatherEdges, 0), gs = InterlockedExchange64(&g_aaGatherSensor, 0);
    LONG64 hit = InterlockedExchange64(&g_aaEdgeHits, 0), miss = InterlockedExchange64(&g_aaEdgeMisses, 0);
    LONG64 big = InterlockedExchange64(&g_aaEdgeBig, 0), busy = InterlockedExchange64(&g_aaEdgeBusy, 0);
    if (gn > 0)
        Note("async-ai: avoidance gather: %.3f ms a frame over all threads, %.0f gathers a frame (%.1f us each), %.1f edges each, sensor %.2f m; "
            "edges %s: %.0f%% reused (%lld hits, %lld new, %lld over %d edges, %lld busy)", (double)gt * ms / frames, (double)gn / frames,
            (double)gt * ms * 1000.0 / (double)gn, (double)ge / (double)gn, (double)gs / 1000.0 / (double)gn, g_aaEdgesOn ? "on" : "off",
            hit + miss ? 100.0 * (double)hit / (double)(hit + miss) : 0.0, (long long)hit, (long long)miss, (long long)big, AA_EC_EDGES, (long long)busy);
    LONG64 w[3], wn[3];
    for (int k = 0; k < 3; k++) { w[k] = InterlockedExchange64(&g_aaWaitTicks[k], 0); wn[k] = InterlockedExchange64(&g_aaWaits[k], 0); }
    LONG64 at = InterlockedExchange64(&g_aaGraphTicks, 0), an = InterlockedExchange64(&g_aaGraphs, 0), amax = InterlockedExchange64(&g_aaGraphMax, 0);
    Note("async-ai: step waits on the game thread: avoidance graph %.3f ms a frame (%.2f waits a frame), collide %.3f ms (%.2f), solve %.3f ms (%.2f); "
        "the avoidance graph's wall time %.3f ms on the mean (%.2f graphs a frame, longest %.2f ms)", (double)w[0] * ms / frames, (double)wn[0] / frames,
        (double)w[1] * ms / frames, (double)wn[1] / frames, (double)w[2] * ms / frames, (double)wn[2] / frames,
        an ? (double)at * ms / (double)an : 0.0, (double)an / frames, (double)amax * ms);
}

static void AaPass(void* manager, float dt);
static void AaSwitch(void* unused, void* manager, uint32_t index, uint32_t state, float dt);

/* Diagnostics (HELLFORGE_ASYNC_DIAG=1, also with HELLFORGE_ASYNC_AI=off): the Devastator's attacks followed from their
 * start, by who started them (the game's combat state, ours, another caller), frame by frame: the ability, the hand's
 * state and fire byte, the shots its weapon fired, the weapon's aim block, the behaviour target, the alert level and the
 * AiEnemy state; and a minute's totals of attacks and shots by starter. */
#define AA_TRACES 8
#define AA_TRACE_FRAMES 48
#define AA_TRACE_MAX 16              /* traces logged per starter */
enum { AA_BY_GAME, AA_BY_US, AA_BY_OTHER };
typedef struct AaTrace { uint32_t entity, ability, weapon, frames, shots, lastShots; int by, on; } AaTrace;
static AaTrace g_aaTr[AA_TRACES];
static int g_aaTrLogged[3];
static LONG64 g_aaTrStarts[3], g_aaTrShots[3], g_aaTrDone[3], g_aaTrHit[3];

static void* volatile g_aaStartNext;
static void* volatile g_aaShotNext;
static const char* const kAaBy[3] = {"game", "ours", "other"};

static int AaDevastatorEntity(uint32_t entity)
{
    uintptr_t ai = AaGlobal(AA_AI_ENEMY);
    int64_t i = ai ? BhvMapFind(ai, 0x2330, entity) : -1;
    uint8_t* info = i >= 0 ? (*(uint8_t***)(ai + 0x2348))[i] : NULL;
    return info && *(uint64_t*)info == AA_DEVASTATOR;
}

/* The weapon in an entity's hand 0, its hand state (3 bits) and fire byte. */
static uint32_t AaHand(uint32_t entity, uint32_t* state, uint32_t* fire)
{
    uintptr_t w = AaGlobal(AA_WIELDERS);
    int64_t i = w ? BhvMapFind(w, 0x30, entity) : -1;
    *state = *fire = 0xFF;
    if (i < 0) return 0;
    *state = (*(uint32_t**)(w + 0x68))[i] & 7u;
    *fire = (*(uint8_t**)(w + 0x58))[(size_t)i * 0x20];
    return *(uint32_t*)(*(uint8_t**)(w + 0x60) + (size_t)i * 0x1D0);
}

static void AaStartHook(void* manager, uint32_t entity, uint32_t ability, int mode, float speed, uint64_t a6, uint64_t a7)
{
    uintptr_t from = (uintptr_t)__builtin_return_address(0);
    ((AaStartFn)g_aaStartNext)(manager, entity, ability, mode, speed, a6, a7);
    if (!g_aaDiag || (ability != 956 && ability != 957 && ability != 959 && ability != 962 && ability != 972) || !AaDevastatorEntity(entity)) return;
    int by = g_aaOurStart ? AA_BY_US : g_aaInSwitch ? AA_BY_GAME : AA_BY_OTHER;
    g_aaTrStarts[by]++;
    if (by == AA_BY_OTHER && g_aaTrStarts[by] <= 8) Note("async-ai diag: E%X ability %u started by game.dll+0x%llX", entity, ability, (unsigned long long)(from - g_aaGame));
    for (int k = 0; k < AA_TRACES; k++)
    {
        if (g_aaTr[k].on) continue;
        uint32_t st, fire;
        AaTrace* t = &g_aaTr[k];
        memset(t, 0, sizeof *t);
        t->entity = entity; t->ability = ability; t->by = by; t->on = 1; t->weapon = AaHand(entity, &st, &fire);
        return;
    }
}

static void AaShotHook(void* projectiles, uint32_t weapon, uint64_t r8, uint64_t r9, uint64_t s5, uint64_t s6, uint64_t s7,
    uint64_t s8, uint64_t s9, uint64_t s10, uint64_t s11)
{
    for (int k = 0; g_aaDiag && k < AA_TRACES; k++) if (g_aaTr[k].on && g_aaTr[k].weapon == weapon) g_aaTr[k].shots++;
    ((AaFireFn)g_aaShotNext)(projectiles, weapon, r8, r9, s5, s6, s7, s8, s9, s10, s11);
}

/* Once a frame after the game's AiEnemy pass: each followed attack's line (every third frame and on a shot), its end. */
static void AaDiagFrame(uintptr_t ai)
{
    uintptr_t wt = AaGlobal(AA_TARGETING), ab = AaGlobal(AA_ABILITIES);
    EvpPlayer players[EVT_MAX_PLAYERS];
    int np = EvpPlayers(g_aaGame, players);
    float pp[3] = {0}, fw[3];
    if (np > 0 && players[0].avatar) AaPose(players[0].avatar, pp, fw);
    for (int k = 0; k < AA_TRACES; k++)
    {
        AaTrace* t = &g_aaTr[k];
        if (!t->on) continue;
        uint32_t st, fire, weapon = AaHand(t->entity, &st, &fire);
        if (weapon && !t->weapon) t->weapon = weapon;
        int64_t ai_i = BhvMapFind(ai, 0x2330, t->entity), w = wt ? BhvMapFind(wt, 0x150, t->entity) : -1;
        int64_t a = ab ? BhvMapFind(ab, 0x18, t->entity) : -1;
        const uint8_t* rec = a >= 0 ? *(uint8_t**)(ab + 0x38) + (size_t)a * 0xE0 : NULL;
        const uint8_t* net = w >= 0 ? *(uint8_t**)(wt + 0x180) + (size_t)w * 0x18 : NULL;
        BehaviourCtx ctx;
        int hasCtx = AaCtx(t->entity, &ctx);
        float pos[3] = {0}, fwd[3] = {0};
        int posOk = AaPose(t->entity, pos, fwd);
        float dx = pp[0] - pos[0], dy = pp[1] - pos[1], d = sqrtf(dx * dx + dy * dy), fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
        float facing = d > 0.1f && fl > 1e-3f ? (dx * fwd[0] + dy * fwd[1]) / (d * fl) : 0.0f;
        int running = rec && rec[0x10];
        t->frames++;
        int end = !posOk || ai_i < 0 || t->frames >= AA_TRACE_FRAMES;
        if (g_aaTrLogged[t->by] < AA_TRACE_MAX && (t->frames % 3 == 1 || t->shots != t->lastShots || end))
            Note("async-ai diag: E%X %s ab %u f%u: ability %u tick %u run %d; hand %u fire %u weapon %X shots %u; aim net %X mode %u has %u; "
                "bhv has %u tgt %X; alert %d; state %u/%u; d %.1f facing %.2f",
                t->entity, kAaBy[t->by], t->ability, t->frames, rec ? *(const uint32_t*)rec : 0, rec ? *(const uint32_t*)(rec + 4) : 0, running,
                st, fire, weapon, t->shots, net ? *(const uint32_t*)net : 0, net ? *(const uint32_t*)(net + 0x10) : 0, net ? net[0x14] : 0,
                hasCtx ? ctx.p[0x70] : 0, hasCtx ? *(uint32_t*)(ctx.p + 0x10) : 0, AaAlertOf(t->entity),
                ai_i >= 0 ? *(uint32_t*)(*(uint8_t**)(ai + 0x2358) + (size_t)ai_i * 0xC0) : 0xFFu,
                ai_i >= 0 ? *(uint32_t*)(*(uint8_t**)(ai + 0x2360) + (size_t)ai_i * 0x14) : 0xFFu, d, facing);
        t->lastShots = t->shots;
        if (!end) continue;
        g_aaTrDone[t->by]++; g_aaTrShots[t->by] += t->shots; g_aaTrHit[t->by] += t->shots > 0;
        if (g_aaTrLogged[t->by] < AA_TRACE_MAX) g_aaTrLogged[t->by]++;
        t->on = 0;
    }
}

static void AaDiagReport(void)
{
    if (!g_aaDiag) return;
    Note("async-ai diag: attacks started by the game's combat %lld, ours %lld, other %lld; followed: game %lld (%lld fired, %lld shots), "
        "ours %lld (%lld fired, %lld shots), other %lld (%lld fired, %lld shots); alert levels set by us %lld",
        (long long)g_aaTrStarts[0], (long long)g_aaTrStarts[1], (long long)g_aaTrStarts[2], (long long)g_aaTrDone[0], (long long)g_aaTrHit[0],
        (long long)g_aaTrShots[0], (long long)g_aaTrDone[1], (long long)g_aaTrHit[1], (long long)g_aaTrShots[1], (long long)g_aaTrDone[2],
        (long long)g_aaTrHit[2], (long long)g_aaTrShots[2], (long long)g_aaAlerted);
}

/* The diagnostics' hooks; with HELLFORGE_ASYNC_AI=off also the AiEnemy pass's (for the frame lines only). */
static void AaDiagHooks(uintptr_t game, int passToo)
{
    for (int f = 0; f < 2; f++)
        if (!PinMatches(game + kAaDiagHooked[f].rva, kAaDiagHooked[f].pin, 24)) { Note("async-ai diag: off; a hooked function is not the reviewed code"); return; }
    if (passToo && (!PinMatches(game + kAaHooked[0].rva, kAaHooked[0].pin, 24) || !PinMatches(game + kAaHooked[1].rva, kAaHooked[1].pin, 24)))
    { Note("async-ai diag: off; the AiEnemy pass is not the reviewed code"); return; }
    if (LoaderHook("async-ai diag: StartAbility", game + kAaDiagHooked[0].rva, (void*)AaStartHook, (void**)&g_aaStartNext) != 0 ||
        LoaderHook("async-ai diag: ProjectileFire", game + kAaDiagHooked[1].rva, (void*)AaShotHook, (void**)&g_aaShotNext) != 0)
    { Note("async-ai diag: off; a hook could not be placed"); return; }
    if (passToo && (LoaderHook("async-ai: the AiEnemy pass", game + AA_AI_PASS, (void*)AaPass, (void**)&g_aaPassNext) != 0 ||
        LoaderHook("async-ai: the AiEnemy state handler", game + AA_STATE_SWITCH, (void*)AaSwitch, (void**)&g_aaSwitchNext) != 0))
    { Note("async-ai diag: off; the AiEnemy pass could not be hooked"); return; }
    g_aaDiag = 1;
    Note("async-ai diag: on (attacks of the Devastator followed from their start)");
}

static void AaPass(void* manager, float dt)
{
    ((AaPassFn)g_aaPassNext)(manager, dt);
    if (manager != (void*)AaGlobal(AA_AI_ENEMY)) return;
    g_aaGameThread = GetCurrentThreadId();
    g_aaMeterFrames++;
    g_aaTick++;
    if (g_aaOn) AaFrame((uintptr_t)manager);
    if (g_aaDiag) AaDiagFrame((uintptr_t)manager);
}

/* An engaged agent's state handler is ours; the dead state stays the game's. */
static void AaSwitch(void* unused, void* manager, uint32_t index, uint32_t state, float dt)
{
    if (g_aaOn && state != AA_DEAD)
    {
        uint8_t* info = (*(uint8_t***)((uintptr_t)manager + 0x2348))[index];
        int slot = info ? AaSlotOf(*(uint32_t*)(info + 8)) : -1;
        if (slot >= 0 && g_aaSlots[slot].held.engaged) { g_aaStat[4]++; return; }
    }
    g_aaInSwitch = 1;
    ((AaSwitchFn)g_aaSwitchNext)(unused, manager, index, state, dt);
    g_aaInSwitch = 0;
}

/* HELLFORGE_ASYNC_OFF lists parts that stay the game's: move (phase 2), fields (phase 3). */
static int AaPartOn(const char* name)
{
    char v[128];
    DWORD n = GetEnvironmentVariableA("HELLFORGE_ASYNC_OFF", v, sizeof v);
    return n == 0 || n >= sizeof v || !PerfFixListed(v, name);
}

/* HELLFORGE_ASYNC_AI: off, or the resources (hex, comma separated) to run; unset or "on": the Devastator. */
static int AaConfigure(void)
{
    char v[256];
    DWORD n = GetEnvironmentVariableA("HELLFORGE_ASYNC_AI", v, sizeof v);
    g_aaTypeCount = 0;
    if (n == 0 || n >= sizeof v || _stricmp(v, "on") == 0) { g_aaTypes[g_aaTypeCount++] = AA_DEVASTATOR; return 1; }
    if (_stricmp(v, "off") == 0 || strcmp(v, "0") == 0) return 0;
    for (char* p = v; *p && g_aaTypeCount < AA_TYPES;)
    {
        char* end;
        uint64_t r = _strtoui64(p, &end, 16);
        if (end == p) break;
        if (r == AA_DEVASTATOR) g_aaTypes[g_aaTypeCount++] = r;
        else Note("async-ai: resource %016llX has no brain of ours yet; the game's AI keeps it", (unsigned long long)r);
        p = *end == ',' ? end + 1 : end;
    }
    return g_aaTypeCount > 0;
}

/* A variable set to 1. Havok AI avoidance off (HELLFORGE_ASYNC_AVOID_OFF) stalls the game's navigator, as our full
 * steering (HELLFORGE_ASYNC_MOVE) does: both stay experiments until the navigator is fed another way. */
static int AaFlag(const char* name)
{
    char v[8];
    DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    return n > 0 && n < sizeof v && v[0] == '1';
}

/* Which parts run: HELLFORGE_ASYNC_OFF, and HELLFORGE_ASYNC_MOVE=1 for our steering in place of the game's. */
static void AaParts(void)
{
    g_aaMoveOn = AaPartOn("move"); g_aaFieldsOn = AaPartOn("fields"); g_aaLodOn = AaPartOn("lod"); g_aaAvoidOff = AaFlag("HELLFORGE_ASYNC_AVOID_OFF");
    g_aaHybrid = !AaFlag("HELLFORGE_ASYNC_MOVE");
    g_aaAlertOn = AaPartOn("alert");
    g_aaGoalsOn = AaPartOn("goals");
    g_abShare = AaFlag("HELLFORGE_ASYNC_SHARE");
    char par[64];
    DWORD np = GetEnvironmentVariableA("HELLFORGE_ASYNC_PAR", par, sizeof par);
    /* unset: both (on by default since the an check); a list: those named; off: neither */
    g_aaParLoco = np == 0 || (np < sizeof par && PerfFixListed(par, "loco"));
    g_aaParMotion = np == 0 || (np < sizeof par && PerfFixListed(par, "motion"));
    g_aaParSnap = AaPartOn("snap");
    char nearText[16];
    DWORD nn = GetEnvironmentVariableA("HELLFORGE_ASYNC_AVOID_NEAR", nearText, sizeof nearText);
    if (nn > 0 && nn < sizeof nearText) g_aaAvoidNear = (float)atof(nearText);
    nn = GetEnvironmentVariableA("HELLFORGE_ASYNC_AVOID_NEIGHBOURS", nearText, sizeof nearText);
    if (nn > 0 && nn < sizeof nearText) g_aaNeighbours = atoi(nearText);
}

static const char* AaMismatch(uintptr_t game)
{
    for (int f = 0; f < AF_COUNT; f++)
        if (!PinMatches(game + kAaFunctions[f].rva, kAaFunctions[f].pin, 24)) return "a game function async-ai calls";
    static const char* what[3] = {"the AiEnemy pass", "the AiEnemy state handler", "QueueFieldWrite"};
    for (int f = 0; f < 3; f++)
        if (!PinMatches(game + kAaHooked[f].rva, kAaHooked[f].pin, 24)) return what[f];
    if (!PinMatches(game + VB_STEER, kVbSteerPin, sizeof kVbSteerPin)) return "the steering pass";
    return NULL;
}

/* The parts' own hooks, and the installed line; a part whose hook fails stays the game's. */
static void AaPartHooks(uintptr_t game)
{
    if (g_aaMoveOn && LoaderHook("async-ai: the steering pass", game + AA_STEER, (void*)AaSteer, (void**)&g_aaSteerNext) != 0)
    { g_aaMoveOn = 0; Note("async-ai: move off; the steering pass could not be hooked"); }
    if (g_aaGoalsOn && (!PinMatches(game + AA_SET_DEST, kAaSetDestPin, 24) ||
        LoaderHook("async-ai: navigation destinations", game + AA_SET_DEST, (void*)AaSetDest, (void**)&g_aaSetDestNext) != 0))
    { g_aaGoalsOn = 0; Note("async-ai: goals off; the navigation destination setter could not be hooked"); }
    if (g_aaFieldsOn && LoaderHook("async-ai: QueueFieldWrite", game + AA_FIELD_WRITE, (void*)AaFieldWrite, (void**)&g_aaFieldNext) != 0)
    { g_aaFieldsOn = 0; Note("async-ai: fields off; QueueFieldWrite could not be hooked"); }
    g_aaReportedAt = GetTickCount64();
    InterlockedExchange(&g_aaOn, 1);
    Note("async-ai: installed; %d enemy type(s) think on a worker thread (the Devastator's brain); move %s, fields %s",
        g_aaTypeCount, g_aaMoveOn ? (g_aaHybrid ? "on (the game's steering plus our separation; Havok AI avoidance off past the near distance with HELLFORGE_ASYNC_AVOID_OFF=1)" : "on (our steering replaces the game's; HELLFORGE_ASYNC_MOVE=1)") : "off", g_aaFieldsOn ? "on (per-frame fields on change)" : "off");
    Note("async-ai: share %s", g_abShare ? "on (an agent that sees a Helldiver makes the Devastators within 20 m aware of it)" : "off (HELLFORGE_ASYNC_SHARE=1 turns it on)");
    Note("async-ai: lod %s; alert %s", g_aaLodOn ? "on (over 100 m from every Helldiver: thinks at 2 Hz, moved in full every third frame)" : "off",
        g_aaAlertOn ? "on (engaged agents that see their target get the game's combat alert level 5)" : "off");
}

static int AaTry(uintptr_t game, int lastTry)
{
    const char* differs = AaMismatch(game);
    if (differs)
    {
        if (!lastTry) return 0;
        Note("async-ai: not installed; %s is not the reviewed code", differs);
        return -1;
    }
    g_aaGame = game;
    if (!AaConfigure())
    {
        Note("async-ai: off (HELLFORGE_ASYNC_AI)");
        if (AaFlag("HELLFORGE_ASYNC_DIAG")) { AaDiagHooks(game, 1); if (g_aaDiag) { AaMeterHook(); AaEdgeHooks(); } g_aaReportedAt = GetTickCount64(); return g_aaDiag ? 1 : -1; }
        return -1;
    }
    AaParts();
    g_aaWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE worker = g_aaWake ? CreateThread(NULL, 0, AaWorker, NULL, 0, NULL) : NULL;
    if (!worker) { Note("async-ai: not installed; no worker thread"); return -1; }
    CloseHandle(worker);
    if (LoaderHook("async-ai: the AiEnemy pass", game + AA_AI_PASS, (void*)AaPass, (void**)&g_aaPassNext) != 0 ||
        LoaderHook("async-ai: the AiEnemy state handler", game + AA_STATE_SWITCH, (void*)AaSwitch, (void**)&g_aaSwitchNext) != 0)
    {
        Note("async-ai: not installed; a hook could not be placed");
        return -1;
    }
    AaPartHooks(game);
    if (AaFlag("HELLFORGE_ASYNC_DIAG")) AaDiagHooks(game, 0);
    AaMeterHook();
    AaEdgeHooks();
    if (g_aaParLoco || g_aaParMotion) AaParHooks(game);
    return 1;
}

static void AaReport(void)
{
    AaDiagReport();
    AaMeterReport((double)g_aaMeterFrames);
    AaEdgeReport((double)g_aaMeterFrames);
    AaParReport();
    g_aaMeterFrames = 0;
    if (!g_aaOn) return;
    LONG64 d[12];
    for (int k = 0; k < 12; k++) d[k] = g_aaStat[k] - g_aaLast[k];
    if (d[0] <= 0) return;
    double f = (double)d[0];
    Note("async-ai: %lld frames; agents %.0f, engaged %.0f a frame; thinks %.0f, moves %.1f, attacks %.2f, rays %.1f a frame "
        "(%.0f%% of %lld answers a line of sight); state handlers skipped %.1f a frame; game thread %.3f ms apply + %.3f ms "
        "snapshot a frame; worker %.3f ms a pass (%lld passes, %lld frames it was still busy); frames whose rays did not run %lld; targets informed %lld",
        (long long)d[0], d[2] / f, d[3] / f, d[10] / f, d[5] / f, d[6] / f, d[7] / f, d[9] ? 100.0 * d[8] / d[9] : 0.0,
        (long long)d[9], d[4] / f, g_aaApplyMs / f, g_aaCaptureMs / f, g_aaWorkerMs / (double)(g_aaPasses > 0 ? g_aaPasses : 1),
        (long long)g_aaPasses, (long long)g_aaLate, (long long)d[1], (long long)d[11]);
    g_aaPasses = 0; g_aaLate = 0;
    memcpy(g_aaLast, g_aaStat, sizeof d);
    Note("async-ai: move: %.1f agents a frame steered by us, %lld kept the game's for a locomotion set change; fields: %lld of %lld "
        "per-frame writes unchanged and not queued", (double)g_aaSteered / f, (long long)g_aaSteerKept, (long long)g_aaFieldSkips,
        (long long)g_aaFieldCalls);
    Note("async-ai: hybrid: %.1f agents a frame got our separation after the game's steering; attacks: %lld frames waited for the "
        "weapon's aim, %lld dropped unaimed; alert levels set %lld", (double)g_aaPushed / f, (long long)g_aaAimWaits, (long long)g_aaAimDrops,
        (long long)g_aaAlerted);
    g_aaAimWaits = g_aaAimDrops = g_aaAlerted = 0;
    if (g_abShare) { Note("async-ai: share: %.1f agents a frame told of a Helldiver by a neighbour", (double)g_abShared / f); g_abShared = 0; }
    Note("async-ai: avoidance: %.1f agents a frame without Havok AI avoidance (our velocity replaced the game's for %.1f), %lld "
        "switches on / off (near %.0f m keeps it); neighbour limit %d set on %lld", (double)g_aaAvoidOffNow / f, (double)g_aaReplaced / f,
        (long long)g_aaAvoidToggles, g_aaAvoidNear, g_aaNeighbours, (long long)g_aaNeighboursSet);
    g_aaAvoidOffNow = g_aaReplaced = g_aaAvoidToggles = 0;
    double ring = (double)(g_aaRing[0] + g_aaRing[1] + g_aaRing[2] + g_aaRing[3]);
    if (ring > 0) Note("async-ai: engaged by distance to the target: %.0f%% under 20 m, %.0f%% 20-35, %.0f%% 35-50, %.0f%% over 50; %.0f%% see it; "
        "navigator destination at our goal %.0f%%", 100.0 * g_aaRing[0] / ring, 100.0 * g_aaRing[1] / ring, 100.0 * g_aaRing[2] / ring,
        100.0 * g_aaRing[3] / ring, 100.0 * g_aaRing[4] / ring, g_aaDestAll ? 100.0 * g_aaDestNear / g_aaDestAll : 0.0);
    memset(g_aaRing, 0, sizeof g_aaRing); g_aaDestNear = g_aaDestAll = 0; g_aaDestLogged = 0;
    Note("async-ai: goals %s: %lld other destinations for engaged agents dropped; callers game.dll+0x%X x%u, +0x%X x%u, +0x%X x%u, +0x%X x%u",
        g_aaGoalsOn ? "on" : "off", (long long)g_aaDestDropped, g_aaDropFrom[0], g_aaDropCount[0], g_aaDropFrom[1], g_aaDropCount[1],
        g_aaDropFrom[2], g_aaDropCount[2], g_aaDropFrom[3], g_aaDropCount[3]);
    g_aaDestDropped = 0; memset(g_aaDropFrom, 0, sizeof g_aaDropFrom); memset(g_aaDropCount, 0, sizeof g_aaDropCount);
    g_aaPushed = 0;
    g_aaSteered = g_aaSteerKept = g_aaFieldSkips = g_aaFieldCalls = 0;
    Note("async-ai: move inputs: %.0f%% of crowd agents with a path direction, speed %.2f m/s, our velocity %.2f m/s on the mean; "
        "Havok AI avoidance %s for them", g_aaPathAll > 0 ? 100.0 * g_aaPathOk / g_aaPathAll : 0.0,
        g_aaPathAll > 0 ? g_aaSpeedSum / g_aaPathAll : 0.0, g_aaVelN > 0 ? g_aaVelSum / g_aaVelN : 0.0, g_aaAvoidOff ? "off" : "kept on");
    Note("async-ai: AiCharacter modes of our agents (0 none, 1 motion, 2 straight, 3 ahead, 4 path): %.0f %.0f %.0f %.0f %.0f %.0f a frame",
        g_aaModes[0] / f, g_aaModes[1] / f, g_aaModes[2] / f, g_aaModes[3] / f, g_aaModes[4] / f, (g_aaModes[5] + g_aaModes[6] + g_aaModes[7]) / f);
    g_aaPathOk = g_aaPathAll = g_aaSpeedSum = g_aaVelSum = g_aaVelN = 0.0;
    memset(g_aaModes, 0, sizeof g_aaModes);
    g_aaApplyMs = g_aaCaptureMs = g_aaWorkerMs = 0.0;
}

static void AsyncAiWatch(void)
{
    if (g_aaState == 1 && GetTickCount64() - g_aaReportedAt >= 60000) { AaReport(); g_aaReportedAt = GetTickCount64(); }
    if (g_aaState != 0) return;
    if (strcmp(g_build, "25480438")) { g_aaState = -1; Note("async-ai: not installed; build %s is not the reviewed 25480438", g_build); return; }
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0) return;
    if (g_aaFirstTry == 0) g_aaFirstTry = GetTickCount64();
    g_aaState = AaTry(game, GetTickCount64() - g_aaFirstTry >= 60000);
}
