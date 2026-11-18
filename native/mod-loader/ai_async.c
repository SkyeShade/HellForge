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
    AF_DATA, AF_ATTACKS, AF_AVOID, AF_FACING, AF_COUNT };
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

/* One registered agent, the game thread's: its held intent, its ray, what was applied. */
typedef struct AaSlot
{
    uint32_t entity, unit, rayUnit, rayFrame;
    int8_t rayPlayer, rayVisible, applied, pad;
    double rayAt;
    AbIntent held;
    uint8_t pendingMove, pendingFire, pendingRay, quiet, attackCount, crowd, faced;
    float vel[3], face[3], facedDir[2];
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
static int g_aaMoveOn, g_aaFieldsOn;  /* phase 2 (our movement and avoidance), phase 3 (fields on change) */
static AbMove g_aaMoves[AB_MAX];
static uint32_t g_aaSteerMark[AB_MAX]; /* AiCharacter index -> the frame + 1 phase 2 moved it in */
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
    float scratch[8] = {0};
    uint32_t count = 0;
    if (!AaCtx(a->entity, &ctx)) return;
    const uint8_t* rows = ((AaAttackListFn)AaFn(AF_ATTACKS))(&ctx, *(uint32_t*)(ctx.p - 8), scratch, &count);
    a->attackCount = 0;
    for (uint32_t k = 0; rows && k < count && a->attackCount < AB_ATTACKS; k++)
    {
        AbAttack* at = &a->attacks[a->attackCount++];
        at->ability = *(const uint32_t*)(rows + k * 0x60); at->kind = *(const uint32_t*)(rows + k * 0x60 + 4);
        at->minRange = *(const float*)(rows + k * 0x60 + 0x14); at->maxRange = *(const float*)(rows + k * 0x60 + 0x18);
        if (!g_aaListed) Note("async-ai: the Devastator's attack %u: ability %u, start kind %u, %.1f-%.1f m (budget %u updates a frame)",
            k, at->ability, at->kind, at->minRange, at->maxRange, *(uint32_t*)(ai + 0x1D10));
    }
    if (a->attackCount) g_aaListed = 1;
}

/* The navigator's path direction and the speed the game gave the agent (phase 2's inputs). */
static void AaPath(uint32_t entity, AbAgentIn* a)
{
    uintptr_t nav = AaGlobal(AA_NAVIGATION), ch = AaGlobal(AA_CHARACTERS);
    int64_t n = nav ? BhvMapFind(nav, 0x30, entity) : -1, c = ch ? BhvMapFind(ch, 0x30, entity) : -1;
    a->pathOk = 0; a->speed = 0.0f;
    if (n < 0 || c < 0) return;
    const uint8_t* r = *(uint8_t**)(nav + 0x50) + (size_t)n * 0xA8;
    uint32_t status = *(const uint32_t*)(r + 0xC), flags = *(const uint32_t*)(r + 0x10);
    const float* d = (const float*)(r + 0x34);
    float l = sqrtf(d[0] * d[0] + d[1] * d[1]);
    if ((status == 0 || status == 3) && (flags & 0xA) && l > 1e-3f) { a->pathDir[0] = d[0] / l; a->pathDir[1] = d[1] / l; a->pathOk = 1; }
    a->speed = *(const float*)(*(uint8_t**)(ch + 0x60) + (size_t)c * 0x14 + 0xC);
    a->crowd = 1;
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
static void AaTarget(BehaviourCtx* ctx, const AbIntent* in, const AbSnapshot* s, double now)
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
        if (entry == NULL) return;
    }
    uint64_t us = (uint64_t)(now * 1e6);
    entry[0x30] = in->seen;
    if (in->seen) { memcpy(entry + 4, in->targetPos, 12); *(uint64_t*)(entry + 0x10) = us; *(uint32_t*)(entry + 0x48) |= 3u; }
    *(float*)(entry + 0x40) = 1.0f;
    if (*(float*)(entry + 0x44) < 0.0f) *(float*)(entry + 0x44) = 1.0f;
    if (*(uint32_t*)(ctx->p + 0x10) != target || ctx->p[0x70] == 0) ((AaSetTargetFn)AaFn(AF_SET_TARGET))(ctx, entry);
    *(uint64_t*)(ctx->p + 0x90) = us + 10000000ull;
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
    memcpy(*(uint8_t**)(nav + 0x50) + (size_t)n * 0xA8 + 0x54, pos, 12);
    memcpy(*(uint8_t**)(loco + 0x40) + (size_t)l * 0xC, sl->vel, 12);
    ((AaAvoidFn)AaFn(AF_AVOID))(NULL, sl->entity, 0);
    if (sl->faced && fabsf(sl->face[0] - sl->facedDir[0]) + fabsf(sl->face[1] - sl->facedDir[1]) > 0.01f)
    {
        ((AaFacingFn)AaFn(AF_FACING))((void*)AaGlobal(AA_ROTATION), sl->entity, sl->face);
        sl->facedDir[0] = sl->face[0]; sl->facedDir[1] = sl->face[1];
    }
    g_aaSteerMark[c] = g_aaFrame + 1u;
}

/* A pending attack starts, with the game's own start for its kind, unless an ability already runs. */
static void AaFire(AaSlot* sl, const AbSnapshot* s)
{
    const AbIntent* in = &sl->held;
    if (sl->pendingFire && !AaAbilityRunning(sl->entity))
    {
        uint32_t target = s->players[in->target].entity;
        if (in->fire == AB_FIRE_TARGETED) ((AaStartAtFn)AaFn(AF_START_AT))(NULL, sl->entity, in->ability, target, in->targetPos, 1);
        else ((AaStartFn)AaFn(AF_START))((void*)AaGlobal(AA_ABILITIES), sl->entity, in->ability, 1, 1.0f, 0, 0);
        g_aaStat[6]++;
    }
    sl->pendingFire = 0;
}

/* Applies one slot's held intent; the one-shots once. */
static void AaApplySlot(uintptr_t ai, AaSlot* sl, const AbSnapshot* s, double now)
{
    AbIntent* in = &sl->held;
    BehaviourCtx ctx;
    if (g_aaMoveOn && sl->crowd) AaCrowdApply(sl);
    if (!in->engaged || in->target < 0 || (uint32_t)in->target >= s->playerCount || !AaCtx(sl->entity, &ctx)) { sl->applied = 0; return; }
    if (!sl->applied)
    {
        int64_t i = BhvMapFind(ai, 0x2330, sl->entity);
        if (i >= 0) AaEngage(ai, (*(uint8_t***)(ai + 0x2348))[i]);
        sl->applied = 1;
    }
    AaTarget(&ctx, in, s, now);
    if (in->face && !(g_aaMoveOn && sl->crowd)) ((AaVectorFn)AaFn(AF_FACE))(&ctx, in->faceDir);
    if (sl->pendingMove) { ((AaVectorFn)AaFn(AF_MOVE))(&ctx, in->goal); sl->pendingMove = 0; g_aaStat[5]++; }
    AaFire(sl, s);
    g_aaStat[3]++;
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
        AaSlot* sl = &g_aaSlots[g_aaMoves[i].slot % AB_MAX];
        if (g_aaMoves[i].entity == 0 || sl->entity != g_aaMoves[i].entity) continue;
        memcpy(sl->vel, g_aaMoves[i].vel, 12); sl->crowd = 1;
        if (g_aaMoves[i].hasFace) { memcpy(sl->face, g_aaMoves[i].face, 12); sl->faced = 1; }
    }
    for (uint32_t i = 0; i < g_aaSnap.agentCount; i++)
    {
        if (!g_aaThought[i]) continue;
        uint32_t slot = g_aaOut[i].slot % AB_MAX;
        AaSlot* sl = &g_aaSlots[slot];
        if (sl->entity != g_aaOut[i].entity) continue;
        sl->held = g_aaOut[i];
        sl->pendingMove = g_aaOut[i].move; sl->pendingFire = g_aaOut[i].fire != AB_FIRE_NONE;
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

/* The steering pass, without the agents phase 2 moved this frame. */
static void AaSteer(void* unused, uint32_t start, uint32_t end, float dt)
{
    AaSteerFn next = (AaSteerFn)g_aaSteerNext;
    uintptr_t ch = AaGlobal(AA_CHARACTERS);
    if (!g_aaOn || !g_aaMoveOn || !ch) { next(unused, start, end, dt); return; }
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

/* Phase 3: the per-frame fields (locomotion's velocity, motion's bool) are queued when they change, and at least
 * twice a second, instead of every frame for every enemy. */
static void AaFieldWrite(uint32_t network, uint32_t field, void* value)
{
    AaFieldFn next = (AaFieldFn)g_aaFieldNext;
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

static void AaPass(void* manager, float dt)
{
    ((AaPassFn)g_aaPassNext)(manager, dt);
    if (g_aaOn && manager == (void*)AaGlobal(AA_AI_ENEMY)) AaFrame((uintptr_t)manager);
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
    ((AaSwitchFn)g_aaSwitchNext)(unused, manager, index, state, dt);
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

static int AaTry(uintptr_t game, int lastTry)
{
    const char* differs = AaMismatch(game);
    if (differs)
    {
        if (!lastTry) return 0;
        Note("async-ai: not installed; %s is not the reviewed code", differs);
        return -1;
    }
    if (!AaConfigure()) { Note("async-ai: off (HELLFORGE_ASYNC_AI)"); return -1; }
    g_aaMoveOn = AaPartOn("move"); g_aaFieldsOn = AaPartOn("fields");
    g_aaGame = game;
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
    if (g_aaMoveOn && LoaderHook("async-ai: the steering pass", game + AA_STEER, (void*)AaSteer, (void**)&g_aaSteerNext) != 0)
    { g_aaMoveOn = 0; Note("async-ai: move off; the steering pass could not be hooked"); }
    if (g_aaFieldsOn && LoaderHook("async-ai: QueueFieldWrite", game + AA_FIELD_WRITE, (void*)AaFieldWrite, (void**)&g_aaFieldNext) != 0)
    { g_aaFieldsOn = 0; Note("async-ai: fields off; QueueFieldWrite could not be hooked"); }
    g_aaReportedAt = GetTickCount64();
    InterlockedExchange(&g_aaOn, 1);
    Note("async-ai: installed; %d enemy type(s) think on a worker thread (the Devastator's brain); move %s, fields %s",
        g_aaTypeCount, g_aaMoveOn ? "on (our movement, Havok AI avoidance off for them)" : "off", g_aaFieldsOn ? "on (per-frame fields on change)" : "off");
    return 1;
}

static void AaReport(void)
{
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
    g_aaSteered = g_aaSteerKept = g_aaFieldSkips = g_aaFieldCalls = 0;
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
