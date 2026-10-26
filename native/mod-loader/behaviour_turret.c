/* HellForge's version of the Pelican's chin turret (645; behaviours.c's header has what it does): its target choice,
 * aim, lowered aim point and refill, run by BehaviourUpdate. Included by hellforge_modloader.c right after
 * behaviours.c. */

/* ---- the turret ------------------------------------------------------------------------------------------------ */

#define T_GRACE 1000000ull          /* not sensed this long (its sensors re-test by rays, so sight flickers): let go */
#define T_RECENT 10000000ull        /* a target let go this recently still guides the next choice (the nearest to it) */
#define T_AIM_MIN 300000ull         /* aiming at least this long before firing (645: 1.5 s) */
#define T_AIM_GATE 2.0f             /* firing within this many degrees of the aim point (645's) */
#define T_RELEASE 3000000ull        /* fired at this long without it taking damage: let go */
#define T_AIM_TIMEOUT 3000000ull    /* aimed at this long without getting within 2 degrees: let go */
#define T_REJECT 5000000ull         /* a target let go for those reasons is not chosen again for this long */
#define T_PICK 250000ull            /* how often an idle turret looks for a target */
#define T_SEARCH 1000000ull         /* how long an aiming turret that lost its target looks before it idles */
#define T_HOLD_RESCORE 3600000000ull
#define T_RANGE 120.0f
#define T_FIRE_TRAVERSE 12.0f       /* a new target this close keeps the trigger pulled */
#define T_REFILL_BELOW 200
#define T_MINDS 16

typedef char (*BhvValidFn)(void* unused, uint32_t entity, void* info);
typedef uint8_t* (*BhvFindEntryFn)(uintptr_t perception, uint32_t perceiver, uint32_t entity);
typedef float (*BhvDegreesFn)(void* unused, uint32_t entity, const float position[3]);
typedef int32_t (*BhvRoundsFn)(void* manager, uint32_t entity);
typedef void (*BhvRefillFn)(void* unused, uint32_t entity, int32_t add);
typedef char (*BhvFireGateFn)(BehaviourCtx* ctx, const float position[3], float maxDegrees);

/* What a turret has decided (per turret, on the machine that runs its behaviour). */
typedef struct TurretMind
{
    uint32_t turret, lock, sighted; /* lock: P's target (the setter may redirect it); sighted: the perceiver entry picked */
    float lockPosition[3];
    int hasLockPosition;
    uint64_t seenAt, progressAt, searchUntil, nextPick, used, lockedAt; /* lockedAt: when the lock was taken (its aim time) */
    int32_t health;
    float sinceDamage;
    struct { uint32_t entity; uint64_t until; } rejected[8];
    int32_t reserve;
    int reserveSet, fullSeen, aiming, releases;
    uint64_t lostAt;
} TurretMind;

/* Targets whose own aim point the gun keeps (big enough that the lowered one would aim under their body): the Bile
 * Titans (content/fac_bugs/cha_strider/cha_strider, cha_strider_gloom), as HD2Runtime's Pelican Gatling did. */
static const uint64_t kOwnAim[] = {0x9E2E17F2CCCCAFDDull, 0xEF04CB84D097A497ull};

static TurretMind g_minds[T_MINDS];

static TurretMind* MindOf(uint32_t turret, uint64_t now)
{
    TurretMind* oldest = &g_minds[0];
    for (int i = 0; i < T_MINDS; i++)
    {
        if (g_minds[i].turret == turret) { g_minds[i].used = now; return &g_minds[i]; }
        if (g_minds[i].used < oldest->used) oldest = &g_minds[i];
    }
    memset(oldest, 0, sizeof *oldest);
    oldest->turret = turret;
    oldest->used = now;
    return oldest;
}

static int Rejected(const TurretMind* m, uint32_t entity, uint64_t now)
{
    for (int i = 0; i < 8; i++) if (m->rejected[i].entity == entity && now < m->rejected[i].until) return 1;
    return 0;
}

static void Reject(TurretMind* m, uint32_t entity, uint64_t now)
{
    int slot = 0;
    for (int i = 0; i < 8; i++) if (m->rejected[i].until < m->rejected[slot].until) slot = i;
    m->rejected[slot].entity = entity;
    m->rejected[slot].until = now + T_REJECT;
}

/* A u32 / u64 value an entity has in a game manager's per-index array, through its map. */
static int ManagerValue(uint32_t global, uint32_t mapAt, uint32_t valuesAt, uint32_t size, uint32_t entity, void* out)
{
    uintptr_t manager = *(uintptr_t*)(g_bhvGame + global);
    int64_t index = manager != 0 ? BhvMapFind(manager, mapAt, entity) : -1;
    uint8_t* values = index >= 0 ? *(uint8_t**)(manager + valuesAt) : NULL;
    if (values == NULL) return 0;
    memcpy(out, values + (size_t)index * size, size);
    return 1;
}

/* An entity's health: its pool, and seconds since it last took damage (ApplyDamage resets it). 0 if none, or dead. */
static int Health(uint32_t entity, int32_t* health, float* sinceDamage)
{
    uintptr_t manager = *(uintptr_t*)(g_bhvGame + BHV_HEALTH);
    int64_t index = manager != 0 ? BhvMapFind(manager, 0x1030, entity) : -1;
    if (index < 0 || (uint32_t)index >= *(uint32_t*)(manager + 0x1020)) return 0;
    uint8_t* record = *(uint8_t**)(manager + 0x1058) + (size_t)index * 0x1B8;
    *health = *(int32_t*)(record + 0x14);
    *sinceDamage = *(float*)(record + 0x1A4);
    return *health > 0 && *(uint32_t*)(record + 0x19C) < 2;
}

/* The turret's perceiver record and its sensors' bits (an entry's sense bits name the sensors that see it now). */
static uint8_t* Perceiver(BehaviourCtx* ctx, uint32_t* senses)
{
    uintptr_t manager = *(uintptr_t*)(g_bhvGame + BHV_PERCEPTION);
    int64_t index = manager != 0 ? BhvMapFind(manager, 0x30, *(uint32_t*)(ctx->p + 0x60)) : -1;
    if (index < 0) return NULL;
    uint8_t* record = *(uint8_t**)(manager + 0x50) + (size_t)index * 0x13F8;
    uint32_t sensors = *(uint32_t*)record, mask = 0;
    for (uint32_t k = 0; k < sensors && k < 32; k++) mask |= 1u << (*(uint32_t*)(record + 8 + 8 * k) & 31);
    *senses = mask;
    return record;
}

/* Whether a perceiver entry is something the turret should shoot: seen now, valid and alive, targetable, hostile, in
 * range, not near a player, not rejected. */
static int Acceptable(const uint8_t* perceiver, uint32_t senses, const uint8_t* entry, const float self[3], const TurretMind* m, uint64_t now)
{
    uint32_t entity = *(const uint32_t*)entry, invalid = *(uint32_t*)(g_bhvGame + BHV_INVALID_ENTITY);
    if (entity == 0 || entity == invalid || Rejected(m, entity, now)) return 0;
    if ((*(const uint32_t*)(entry + 0x4C) & senses) == 0) return 0;
    if (!((BhvValidFn)BhvFn(F_VALID))(NULL, entity, NULL)) return 0;
    uint64_t flags = 0;
    if (ManagerValue(BHV_FLAGS, 0x18, 0x38, 8, entity, &flags) && (flags >> 35 & 1)) return 0;
    uint32_t faction = 0;
    if (!ManagerValue(BHV_FACTIONS, 0x12050, 0x12078, 4, entity, &faction) || faction == 0 || (faction & *(const uint32_t*)(perceiver + 0x13E8))) return 0;
    int32_t health;
    float since;
    if (!Health(entity, &health, &since)) return 0;
    const float* at = (const float*)(entry + 4);
    float dx = at[0] - self[0], dy = at[1] - self[1], dz = at[2] - self[2];
    if (dx * dx + dy * dy + dz * dz >= T_RANGE * T_RANGE) return 0;
    return 1;
}

/* The perceiver's three lists of entries (a count, then 0x50-byte entries), and the fourth's three slots at +0x1228. */
static const uint32_t kPerceiverLists[3] = {0x310, 0x818, 0xD20};

static uint32_t ListCount(const uint8_t* perceiver, int l)
{
    return l < 3 ? *(const uint32_t*)(perceiver + kPerceiverLists[l]) : 3;
}

/* Entry i of list l, or NULL for an unused slot of the fourth. */
static const uint8_t* ListEntry(const uint8_t* perceiver, int l, uint32_t i)
{
    const uint8_t* entry = l < 3 ? perceiver + kPerceiverLists[l] + 8 + (size_t)i * 0x50 : perceiver + 0x1228 + (size_t)i * 0x50;
    return l == 3 && *(const uint32_t*)(entry + 0x48) == 0 ? NULL : entry;
}

/* An entry's score for Pick (lower is better): its distance from `around` (else the turret), beyond 25 m and 50 m after
 * the nearer ones, and the gun's turn, which only breaks ties. */
static float PickScore(BehaviourCtx* ctx, const uint8_t* entry, const float self[3], const float* around)
{
    const float* at = (const float*)(entry + 4);
    const float* from = around != NULL ? around : self;
    float dx = at[0] - from[0], dy = at[1] - from[1], dz = at[2] - from[2], distance = sqrtf(dx * dx + dy * dy + dz * dz);
    float score = distance + 0.05f * ((BhvDegreesFn)BhvFn(F_DEGREES))(NULL, EntityOf(ctx), at);
    if (around != NULL) score += distance <= 25.0f ? 0.0f : distance <= 50.0f ? 1000.0f : 2000.0f;
    return score;
}

/* The best entry to shoot: given `around` (where a target was just let go), the nearest to it (within 25 m, then 50 m,
 * then any); otherwise the nearest to the turret. The gun's turn only breaks ties. */
static const uint8_t* Pick(BehaviourCtx* ctx, const uint8_t* perceiver, uint32_t senses, const float self[3], const TurretMind* m,
    const float* around, uint64_t now)
{
    const uint8_t* best = NULL;
    float bestScore = 0.0f;
    for (int l = 0; l < 4; l++)
    {
        uint32_t count = ListCount(perceiver, l);
        for (uint32_t i = 0; i < count && i < 16; i++)
        {
            const uint8_t* entry = ListEntry(perceiver, l, i);
            if (entry == NULL) continue;
            STEP(80);
            if (!Acceptable(perceiver, senses, entry, self, m, now)) continue;
            STEP(81);
            float score = PickScore(ctx, entry, self, around);
            if (best == NULL || score < bestScore) { best = entry; bestScore = score; }
        }
    }
    return best;
}

static void StageTurret(BehaviourCtx* ctx, int stage) { ((BhvCtxIntFn)BhvFn(F_645_STAGE))(ctx, stage); }

/* Where the turret's last target was, while that still guides the next choice. */
static const float* Around(const TurretMind* m, uint64_t now)
{
    return m->hasLockPosition && (m->lock != 0 || now < m->lostAt + T_RECENT) ? m->lockPosition : NULL;
}

typedef uint8_t* (*BhvDescriptorFn)(uint32_t entity);

static int OwnAim(uint32_t entity)
{
    const uint8_t* descriptor = ((BhvDescriptorFn)BhvFn(F_DESCRIPTOR))(entity);
    if (descriptor == NULL) return 0;
    uint64_t type = *(const uint64_t*)descriptor;
    for (size_t i = 0; i < sizeof kOwnAim / sizeof kOwnAim[0]; i++) if (type == kOwnAim[i]) return 1;
    return 0;
}

typedef const float* (*BhvNodePositionFn)(uint32_t unit, uint32_t node);

/* An entity's live root (its model's node 0, from the scene graph, as 0x4BC5D0 reads the behaviour's own). */
static int Root(uint32_t entity, float out[3])
{
    const uint8_t* descriptor = ((BhvDescriptorFn)BhvFn(F_DESCRIPTOR))(entity);
    uint32_t unit = descriptor != NULL ? *(const uint32_t*)(descriptor + 0xC) : 0;
    uint8_t* api = *(uint8_t**)(g_bhvGame + BHV_ENGINE_API);
    uint8_t* graph = api != NULL ? *(uint8_t**)(api + 0x18) : NULL;
    if (unit == 0 || graph == NULL) return 0;
    const float* at = ((BhvNodePositionFn)*(void**)(graph + 0x88))(unit, 0);
    if (at == NULL) return 0;
    memcpy(out, at, 12);
    return 1;
}

/* The lowered aim point for P's target (the behaviour's aim height above its live root), or 0 to keep the game's. */
static int Lowered(BehaviourCtx* ctx, const Behaviour* b, float out[3])
{
    uint32_t target = *(uint32_t*)(ctx->p + 0x10);
    if (b->aimHeight <= 0.0f || ctx->p[0x70] == 0 || target == *(uint32_t*)(g_bhvGame + BHV_INVALID_ENTITY) || OwnAim(target) ||
        !Root(target, out))
        return 0;
    out[2] += b->aimHeight;
    return 1;
}

/* The point the gun aims at: the lowered one, or the game's (the target's aim node). */
static void AimPoint(BehaviourCtx* ctx, const Behaviour* b, const float self[3], float out[3])
{
    if (!Lowered(ctx, b, out)) ((BhvTargetFn)BhvFn(F_TARGET))(out, ctx, self);
}

/* Takes the target in P (after the setter, which may redirect it to the entity it belongs to) as the turret's lock;
 * `sighted` is the entity its perceiver lists know (what sight is judged on). */
static void Lock(BehaviourCtx* ctx, TurretMind* m, uint64_t now, uint32_t sighted)
{
    m->lock = *(uint32_t*)(ctx->p + 0x10);
    m->sighted = sighted;
    m->seenAt = m->progressAt = m->lockedAt = now;
    m->health = 0;
    m->sinceDamage = 0.0f;
    Health(m->lock, &m->health, &m->sinceDamage);
    memcpy(m->lockPosition, ctx->p + 0x14, 12);
    m->hasLockPosition = 1;
}

/* What one turret update sees: its perceiver record and its sensors' bits (Perceiver), its position, and the time. */
typedef struct TurretView { const uint8_t* perceiver; uint32_t senses; const float* self; uint64_t now; } TurretView;

static void NoteRelease(BehaviourCtx* ctx, TurretMind* m, uint32_t old, const char* why, const uint8_t* entry)
{
    if (m->releases++ < 40)
        Note("turret %u: let go of %u (%s); %s %u", EntityOf(ctx), old, why, entry != NULL ? "now" : "no other target",
            entry != NULL ? *(const uint32_t*)entry : 0);
}

/* Lets the lock go (rejecting it for a while if asked) for the best target near it, or none. Returns 1 with a new one. */
static int Release(BehaviourCtx* ctx, TurretMind* m, const TurretView* v, int reject, const char* why)
{
    uint64_t now = v->now;
    if (reject && m->lock != 0) Reject(m, m->lock, now);
    uint32_t old = m->lock;
    if (m->lock != 0) m->lostAt = now;
    m->lock = 0;
    const uint8_t* entry = v->perceiver != NULL ? Pick(ctx, v->perceiver, v->senses, v->self, m, Around(m, now), now) : NULL;
    if (old == 0 && entry == NULL) return 0; /* nothing held, nothing found: the search already runs */
    NoteRelease(ctx, m, old, why, entry);
    if (entry != NULL)
    {
        float turn = ((BhvDegreesFn)BhvFn(F_DEGREES))(NULL, EntityOf(ctx), (const float*)(entry + 4));
        ((BhvVectorFn)BhvFn(F_SET_TARGET))(ctx, (const float*)entry);
        Lock(ctx, m, now, *(const uint32_t*)entry);
        if (*(int32_t*)ctx->p == 3 && turn > T_FIRE_TRAVERSE) StageTurret(ctx, 2); /* too far a turn to keep firing */
        return 1;
    }
    ((BhvVectorFn)BhvFn(F_SET_TARGET))(ctx, NULL); /* 645's own clear */
    m->searchUntil = now + T_SEARCH;
    if (*(int32_t*)ctx->p == 3) StageTurret(ctx, 2); /* leaving 3 releases the trigger */
    return 0;
}

/* The lock's perceiver entry: when its sensors last saw it (now, while they do), and where it is. */
static void SeeLock(TurretMind* m, const uint8_t* entry, uint32_t senses, uint64_t now)
{
    uint64_t sensed = (*(const uint32_t*)(entry + 0x4C) & senses) ? now : *(const uint64_t*)(entry + 0x10);
    if (sensed > m->seenAt) m->seenAt = sensed;
    memcpy(m->lockPosition, entry + 4, 12);
}

/* Keeps the lock while it is worth shooting: returns 1 with a target (the lock, or its replacement), 0 with none. */
static int Track(BehaviourCtx* ctx, TurretMind* m, const TurretView* v, int firing)
{
    uint64_t now = v->now;
    uint32_t target = *(uint32_t*)(ctx->p + 0x10), invalid = *(uint32_t*)(g_bhvGame + BHV_INVALID_ENTITY);
    int32_t health;
    float since;
    if (ctx->p[0x70] == 0 || target == invalid || !((BhvValidFn)BhvFn(F_VALID))(NULL, target, NULL) || !Health(target, &health, &since))
        return Release(ctx, m, v, 0, "dead or gone");
    if (target != m->lock) Lock(ctx, m, now, target);
    uintptr_t perception = *(uintptr_t*)(g_bhvGame + BHV_PERCEPTION);
    const uint8_t* entry = perception != 0 ? ((BhvFindEntryFn)BhvFn(F_FIND_ENTRY))(perception, *(uint32_t*)(ctx->p + 0x60), m->sighted) : NULL;
    if (entry != NULL) SeeLock(m, entry, v->senses, now);
    if (now >= m->seenAt + T_GRACE) return Release(ctx, m, v, 1, "out of sight");
    if (firing)
    {
        if (health < m->health || since < m->sinceDamage) m->progressAt = now;
        m->health = health;
        m->sinceDamage = since;
        if (now >= m->progressAt + T_RELEASE) return Release(ctx, m, v, 1, "not hurting it");
    }
    else m->progressAt = now; /* only firing time counts */
    return 1;
}

/* The weapon's aim point, set lower than the game's for the enemy (its aim node, about 1.5 m above the feet: from a
 * Pelican's height, rounds aimed there pass over): the targeting record's aim override, as the game's own AI action code
 * sets it (the targeting update 0x6BBDC0 aims at the override's curve while its t >= 0: A +0x20 = B +0x2C = the point,
 * mode +0x60 2 (linear), no advance +0x6C, t +0x50 = 1, last; released with t = -1, as 0x6C0EB0 does). Written only
 * when the point moves 5 cm, never while the record's network bits are set or another user holds the override; every
 * machine computes its own (it is not replicated). HD2Runtime's Pelican Gatling: 0.7 m above the target's root. */
static int g_aimLowNotes; /* AimLow's first notes */

/* The override released (t = -1), when this turret holds it. */
static void ReleaseAimOverride(TurretMind* m, uint8_t* record, float t)
{
    if (m->aiming && t >= 0.0f) *(float*)(record + 0x50) = -1.0f;
    m->aiming = 0;
}

/* The override at `point` (moved once the point moves 5 cm), taken when the turret does not hold it yet. */
static void SetAimOverride(BehaviourCtx* ctx, const Behaviour* b, TurretMind* m, uint8_t* record, const float point[3])
{
    const float* a = (const float*)(record + 0x20);
    float dx = a[0] - point[0], dy = a[1] - point[1], dz = a[2] - point[2];
    if (m->aiming && dx * dx + dy * dy + dz * dz < 0.05f * 0.05f) return;
    memcpy(record + 0x20, point, 12);
    memcpy(record + 0x2C, point, 12);
    if (m->aiming) return;
    *(uint32_t*)(record + 0x60) = 2;
    record[0x6C] = 0;
    *(float*)(record + 0x50) = 1.0f; /* the switch, last */
    m->aiming = 1;
    if (g_aimLowNotes++ < 6) Note("turret %u: aims %.1f m above the feet of %u at (%.1f, %.1f, %.1f)", EntityOf(ctx), (double)b->aimHeight,
        *(uint32_t*)(ctx->p + 0x10), (double)point[0], (double)point[1], (double)point[2]);
}

static void AimLow(BehaviourCtx* ctx, const Behaviour* b, TurretMind* m, int stage)
{
    if (b->aimHeight <= 0.0f) return;
    uintptr_t manager = *(uintptr_t*)(g_bhvGame + BHV_TARGETING);
    int64_t index = manager != 0 ? BhvMapFind(manager, 0x150, EntityOf(ctx)) : -1;
    if (index < 0) return;
    uint8_t* record = *(uint8_t**)(manager + 0x178) + (size_t)index * 0xD0;
    uint32_t bits = *(uint32_t*)(*(uint8_t**)(manager + 0x180) + (size_t)index * 0x18 + 0x10);
    if (bits != 0) { if (g_aimLowNotes++ < 3) Note("turret %u: aim override waits (network bits %08x)", EntityOf(ctx), bits); return; }
    float t = *(float*)(record + 0x50);
    uint32_t invalid = *(uint32_t*)(g_bhvGame + BHV_INVALID_ENTITY);
    float point[3];
    int want = (stage == 2 || stage == 3) && Lowered(ctx, b, point);
    (void)invalid;
    if (!want) { ReleaseAimOverride(m, record, t); return; }
    if (!m->aiming && t >= 0.0f) { if (g_aimLowNotes++ < 3) Note("turret %u: aim override in use by another (t %.2f)", EntityOf(ctx), (double)t); return; }
    SetAimOverride(ctx, b, m, record, point);
}

/* Refills the magazine from the behaviour's reserve before it runs dry (645 never reloads). */
static void Refill(BehaviourCtx* ctx, const Behaviour* b, TurretMind* m)
{
    if (!m->reserveSet) { m->reserve = b->reserveRounds; m->reserveSet = 1; }
    if (m->reserve <= 0 || !(*(uint32_t*)(ctx->entity + 0x14) & 1)) return; /* the machine with authority only */
    void* magazines = *(void**)(g_bhvGame + BHV_ROUNDS);
    uintptr_t refills = *(uintptr_t*)(g_bhvGame + BHV_MAGAZINES);
    if (magazines == NULL || refills == 0 || BhvMapFind(refills, 0x20, EntityOf(ctx)) < 0) return; /* the refill's own lookup */
    STEP(40);
    int32_t rounds = ((BhvRoundsFn)BhvFn(F_ROUNDS))(magazines, EntityOf(ctx));
    STEP(41);
    int32_t capacity = ((BhvRoundsFn)BhvFn(F_CAPACITY))(NULL, EntityOf(ctx));
    if (rounds > T_REFILL_BELOW) m->fullSeen = 1; /* never at spawn, before its magazine is filled */
    if (!m->fullSeen || rounds > T_REFILL_BELOW || capacity <= rounds) return;
    STEP(42);
    int32_t add = capacity - rounds < m->reserve ? capacity - rounds : m->reserve;
    ((BhvRefillFn)BhvFn(F_REFILL))(NULL, EntityOf(ctx), add);
    m->reserve -= add;
}

/* Stage 1, idle: 645 rests the gun; HellForge looks for a target. */
static void TurretIdle(BehaviourCtx* ctx, TurretMind* m, const TurretView* v, float dt, int active, int ammunition)
{
    STEP(50);
    ((BhvCtxTimeFn)BhvFn(F_645_IDLE))(ctx, dt);
    STEP(51);
    if (*(int32_t*)ctx->p == 1 && active && ammunition && v->perceiver != NULL && v->now >= m->nextPick)
    {
        m->nextPick = v->now + T_PICK;
        const uint8_t* entry = Pick(ctx, v->perceiver, v->senses, v->self, m, Around(m, v->now), v->now);
        if (entry != NULL)
        {
            ((BhvVectorFn)BhvFn(F_SET_TARGET))(ctx, (const float*)entry);
            Lock(ctx, m, v->now, *(const uint32_t*)entry);
            StageTurret(ctx, 2);
        }
    }
}

/* Stage 2 with a target: fire once aimed long enough and within 2 degrees of the aim point, or let go of a target it
 * cannot aim at. */
static void TurretAimed(BehaviourCtx* ctx, const Behaviour* b, TurretMind* m, const TurretView* v)
{
    float point[3];
    AimPoint(ctx, b, v->self, point);
    /* Aim time counts from the stage or the current lock, whichever is later: a replacement gets its own. */
    uint64_t aimed = *(uint64_t*)(ctx->p + 0x178) > m->lockedAt ? *(uint64_t*)(ctx->p + 0x178) : m->lockedAt;
    if (v->now >= aimed + T_AIM_MIN && ((BhvFireGateFn)BhvFn(F_FIRE_GATE))(ctx, point, T_AIM_GATE)) StageTurret(ctx, 3);
    else if (v->now >= aimed + T_AIM_TIMEOUT) Release(ctx, m, v, 1, "cannot aim at it");
}

/* Stage 2, aim: 645's (0x44FE20) without its 1.5 s wait or its re-score: fire once within 2 degrees of the aim point. */
static void TurretAim(BehaviourCtx* ctx, const Behaviour* b, TurretMind* m, const TurretView* v, int active, int ammunition)
{
    if (!active || !ammunition) { StageTurret(ctx, 4); return; }
    if (!Track(ctx, m, v, 0))
    {
        const uint8_t* entry = v->perceiver != NULL && v->now < m->searchUntil ? Pick(ctx, v->perceiver, v->senses, v->self, m, Around(m, v->now), v->now) : NULL;
        if (entry == NULL) { if (v->now >= m->searchUntil) StageTurret(ctx, 1); return; }
        ((BhvVectorFn)BhvFn(F_SET_TARGET))(ctx, (const float*)entry);
        Lock(ctx, m, v->now, *(const uint32_t*)entry);
    }
    STEP(60);
    TurretAimed(ctx, b, m, v);
    STEP(61);
}

/* Stage 3, fire. */
static void TurretFire(BehaviourCtx* ctx, const Behaviour* b, TurretMind* m, const TurretView* v, int active, int ammunition)
{
    if (!active) StageTurret(ctx, 4);
    else if (!ammunition) StageTurret(ctx, 1);
    else if (b->burstMicroseconds != 0 && v->now >= *(uint64_t*)(ctx->p + 0x180) + b->burstMicroseconds) StageTurret(ctx, 2);
    else { STEP(70); Track(ctx, m, v, 1); }
}

/* 645's update (0x478D9D) with HellForge choosing the target. */
static void TurretUpdate(BehaviourCtx* ctx, const Behaviour* b, float dt)
{
    uint8_t* p = ctx->p;
    uint64_t now = BhvNow();
    TurretMind* m = MindOf(EntityOf(ctx), now);
    *(uint64_t*)(p + 0x90) = now + T_HOLD_RESCORE; /* 645 does not re-score: HellForge picks */
    uint32_t senses = 0;
    STEP(30);
    const uint8_t* perceiver = Perceiver(ctx, &senses);
    STEP(31);
    int active = ((BhvCtxTestFn)BhvFn(F_645_ACTIVE))(ctx) != 0;
    STEP(32);
    int ammunition = ((BhvCtxTestFn)BhvFn(F_645_AMMO))(ctx) != 0;
    Refill(ctx, b, m);
    float self[3];
    STEP(33);
    ((BhvPositionFn)BhvFn(F_POSITION))(self, ctx);
    STEP(34);
    TurretView v = {perceiver, senses, self, now};
    switch (*(int32_t*)p)
    {
    case 1: TurretIdle(ctx, m, &v, dt, active, ammunition); break;
    case 2: TurretAim(ctx, b, m, &v, active, ammunition); break;
    case 3: TurretFire(ctx, b, m, &v, active, ammunition); break;
    case 4: /* inactive */
        if (active) StageTurret(ctx, 1);
        break;
    }
    STEP(90);
    AimLow(ctx, b, m, *(int32_t*)p);
}
