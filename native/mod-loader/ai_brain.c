/* async-ai's brain (docs/performance-enemies.md "Our own enemy AI"): HellForge's decisions for an enemy type, made on
 * a worker thread from a snapshot the game thread takes once a frame. It reads only the snapshot and its own memory and
 * writes only intents, so it never touches game memory; ai_async.c turns the intents into the game's own calls a frame
 * later. The first type is the plain Devastator. Its features, against the game's (research/async-ai/ARCHITECTURE.md):
 * sight by range and cone with a line of sight the game's ray batch answers, hearing from what the game's ears still
 * record, awareness that builds and fades, a remembered last position, target choice among the Helldivers with
 * stickiness, a fighting distance kept by moving in, backing off or strafing, facing the target, and bursts from the
 * attacks the game gave this enemy, by range. Included by perf_all.c before ai_async.c. */

#define AB_MAX 4096u           /* agents (registry slots) */
#define AB_PLAYERS 4
#define AB_ATTACKS 6
#define AB_THINK_EVERY 6u      /* frames between an agent's thinks (10 a second at 60 fps), staggered by slot */

typedef struct AbAttack { uint32_t ability, kind; float minRange, maxRange; } AbAttack;

/* One enemy in the snapshot. slot is its registry slot: stable while it lives, fresh says it was just given. */
typedef struct AbAgentIn
{
    uint32_t entity, slot;
    float pos[3], fwd[3];
    float heardAge;            /* seconds since its ears last recorded heardPlayer; < 0 none */
    int8_t heardPlayer;
    uint8_t busy;              /* an ability runs on it */
    uint8_t fresh;
    uint8_t attackCount;
    AbAttack attacks[AB_ATTACKS];
    int8_t rayPlayer;          /* the player the last answered ray looked at, -1 none */
    uint8_t rayVisible;        /* its answer: 1 nothing in between */
    float rayAge;              /* seconds since it was answered */
} AbAgentIn;

typedef struct AbPlayerIn { uint32_t entity, unit; float pos[3]; uint8_t alive; } AbPlayerIn;

typedef struct AbSnapshot
{
    uint32_t frame, agentCount, playerCount;
    double now;                /* seconds */
    AbPlayerIn players[AB_PLAYERS];
    AbAgentIn agents[AB_MAX];
} AbSnapshot;

enum { AB_FIRE_NONE = 0, AB_FIRE_PLAIN = 1, AB_FIRE_TARGETED = 2 };

/* What the game thread applies for agent i of the same snapshot. engaged: our brain drives it (the game's state
 * handler is skipped); else the game's own AI runs it. move, fire and ray are one-shot. */
typedef struct AbIntent
{
    uint32_t entity, slot;
    uint8_t engaged, seen, face, move, fire;
    int8_t target, ray;        /* player indexes, -1 none */
    float targetPos[3], goal[3], faceDir[3];
    uint32_t ability;
} AbIntent;

/* The brain's memory of one agent (by registry slot, the worker's alone). */
typedef struct AbMemory
{
    uint32_t entity;
    float awareness[AB_PLAYERS];
    float lastSeen[AB_PLAYERS][3];
    double lastSeenAt[AB_PLAYERS], lastThink, engagedAt, burstUntil, holdUntil, nextShot, strafeUntil, goalAt;
    float goal[3], strafe;
    int8_t target, engaged, rayTurn, hasGoal;
    uint32_t rng;
} AbMemory;

/* The Devastator's numbers. The game's own are per type data we have not all read (Detector settings, eye ranges, the
 * fire rhythm in AiEnemyComponentData +0xA8..+0xC4); these follow what the game shows: about 60 m of sight ahead,
 * engagement at 15-35 m, bursts of 2-3 s with 1-2 s pauses. */
typedef struct AbType
{
    float frontRange, sideRange, rearRange, coneCos, gain, loss, engageAt, keepAt, nearMin, nearMax, holdAt;
    float burstMin, burstMax, pauseMin, pauseMax, shotGap, faceCos, forgetAfter, heardAwareness;
} AbType;
static const AbType kAbDevastator = {
    60.0f, 40.0f, 20.0f, 0.5f, 1.6f, 0.12f, 0.6f, 0.25f, 12.0f, 35.0f, 25.0f,
    2.0f, 3.0f, 1.0f, 2.0f, 0.35f, 0.9f, 20.0f, 0.7f};

static AbMemory g_abMemory[AB_MAX];

static uint32_t AbRand(AbMemory* m) { m->rng = m->rng * 1664525u + 1013904223u; return m->rng >> 8; }
static float AbUniform(AbMemory* m, float lo, float hi) { return lo + (hi - lo) * (float)(AbRand(m) & 0xFFFF) / 65535.0f; }

static float AbDist2D(const float* a, const float* b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1];
    return sqrtf(dx * dx + dy * dy);
}

/* Whether the eye's range and cone hold the point (the game's blend of front, side and rear ranges by the facing; once
 * aware the front range all round, as 0x64C6F0 does). */
static int AbInView(const AbType* t, const AbAgentIn* a, const float* p, int aware)
{
    float dx = p[0] - a->pos[0], dy = p[1] - a->pos[1], d = sqrtf(dx * dx + dy * dy);
    if (d < 1.0f) return 1;
    if (aware) return d <= t->frontRange;
    float fl = sqrtf(a->fwd[0] * a->fwd[0] + a->fwd[1] * a->fwd[1]);
    float c = fl > 1e-3f ? (dx * a->fwd[0] + dy * a->fwd[1]) / (d * fl) : 1.0f;
    float range = c >= 0.0f ? t->sideRange + (t->frontRange - t->sideRange) * c : t->sideRange + (t->sideRange - t->rearRange) * c;
    return d <= range && (c >= t->coneCos || d <= t->rearRange);
}

/* Whether the agent sees the Helldiver now: alive, its last ray clear and fresh, in range and cone. */
static int AbSees(const AbType* t, const AbAgentIn* a, const AbMemory* m, const AbPlayerIn* pl, uint32_t p)
{
    return pl->alive && a->rayPlayer == (int8_t)p && a->rayVisible && a->rayAge < 0.5f &&
        AbInView(t, a, pl->pos, m->awareness[p] > 0.0f);
}

/* What the ears heard in the last second: at least heardAwareness, and where, unless just seen. */
static void AbHear(const AbType* t, const AbSnapshot* s, const AbAgentIn* a, AbMemory* m, uint32_t p)
{
    if (!s->players[p].alive || a->heardPlayer != (int8_t)p || a->heardAge < 0.0f || a->heardAge >= 1.0f) return;
    if (m->awareness[p] < t->heardAwareness) m->awareness[p] = t->heardAwareness;
    if (s->now - m->lastSeenAt[p] > 1.0) { memcpy(m->lastSeen[p], s->players[p].pos, 12); m->lastSeenAt[p] = s->now - 1.0; }
}

/* Perception: awareness of each Helldiver from what the eye can see (the ray's answer) and what the ears heard. */
static void AbPerceive(const AbType* t, const AbSnapshot* s, const AbAgentIn* a, AbMemory* m, float dt)
{
    for (uint32_t p = 0; p < s->playerCount && p < AB_PLAYERS; p++)
    {
        const AbPlayerIn* pl = &s->players[p];
        if (AbSees(t, a, m, pl, p))
        {
            float d = AbDist2D(a->pos, pl->pos), closeness = 1.0f - d / (t->frontRange * 1.5f);
            m->awareness[p] += t->gain * dt * (closeness > 0.2f ? closeness : 0.2f);
            memcpy(m->lastSeen[p], pl->pos, 12);
            m->lastSeenAt[p] = s->now;
        }
        else m->awareness[p] -= t->loss * dt;
        AbHear(t, s, a, m, p);
        float w = pl->alive ? m->awareness[p] : 0.0f;
        m->awareness[p] = w > 1.0f ? 1.0f : w < 0.0f ? 0.0f : w;
    }
}

/* A Helldiver's claim to be the target: 0 when it cannot be one. */
static float AbScore(const AbType* t, const AbSnapshot* s, const AbAgentIn* a, const AbMemory* m, uint32_t p)
{
    float need = m->engaged ? t->keepAt : t->engageAt;
    if (!s->players[p].alive || m->awareness[p] < need || s->now - m->lastSeenAt[p] > t->forgetAfter) return 0.0f;
    float d = AbDist2D(a->pos, m->lastSeen[p]), score = m->awareness[p] * (1.0f - (d < 150.0f ? d : 150.0f) / 200.0f);
    return (int)p == m->target ? score * 1.3f : score;
}

/* Target choice: the most aware-of Helldiver, nearer ones first, the current one held unless another is clearly
 * better. Engages at engageAt, lets go below keepAt or after forgetAfter seconds unseen. */
static void AbChoose(const AbType* t, const AbSnapshot* s, const AbAgentIn* a, AbMemory* m)
{
    int best = -1;
    float bestScore = 0.0f;
    for (uint32_t p = 0; p < s->playerCount && p < AB_PLAYERS; p++)
    {
        float score = AbScore(t, s, a, m, p);
        if (score > bestScore) { bestScore = score; best = (int)p; }
    }
    if (best >= 0 && !m->engaged) { m->engaged = 1; m->engagedAt = s->now; m->holdUntil = s->now + AbUniform(m, 0.3f, 0.8f); }
    if (best < 0) m->engaged = 0;
    m->target = (int8_t)best;
}

/* Where to stand: in to holdAt when far, back off when too near, else strafe sideways now and then; the last known
 * position when the target is out of sight. */
static void AbMove(const AbType* t, const AbSnapshot* s, const AbAgentIn* a, AbMemory* m, AbIntent* out, int recent)
{
    const float* tp = m->lastSeen[m->target];
    float dx = a->pos[0] - tp[0], dy = a->pos[1] - tp[1], d = sqrtf(dx * dx + dy * dy), g[3];
    if (d < 0.5f) { dx = 1.0f; dy = 0.0f; d = 1.0f; }
    if (!recent) memcpy(g, tp, 12);
    else if (d > t->nearMax) { g[0] = tp[0] + dx / d * t->holdAt; g[1] = tp[1] + dy / d * t->holdAt; g[2] = tp[2]; }
    else if (d < t->nearMin) { g[0] = tp[0] + dx / d * (t->nearMin + 6.0f); g[1] = tp[1] + dy / d * (t->nearMin + 6.0f); g[2] = tp[2]; }
    else
    {
        if (s->now >= m->strafeUntil) { m->strafe = (AbRand(m) & 1) ? 5.0f : -5.0f; m->strafeUntil = s->now + AbUniform(m, 3.0f, 5.0f); }
        g[0] = a->pos[0] - dy / d * m->strafe; g[1] = a->pos[1] + dx / d * m->strafe; g[2] = a->pos[2];
    }
    float moved = m->hasGoal ? AbDist2D(g, m->goal) : 1e9f;
    if (moved > 3.0f || s->now - m->goalAt > 4.0)
    {
        memcpy(m->goal, g, 12); m->hasGoal = 1; m->goalAt = s->now;
        out->move = 1; memcpy(out->goal, g, 12);
    }
}

/* Bursts: fire while the target was seen just now, is in an attack's range and in front; pause between bursts. */
static void AbFire(const AbType* t, const AbSnapshot* s, const AbAgentIn* a, AbMemory* m, AbIntent* out)
{
    if (s->now < m->holdUntil) return;
    if (m->burstUntil <= m->holdUntil && s->now >= m->holdUntil) m->burstUntil = s->now + AbUniform(m, t->burstMin, t->burstMax);
    if (s->now >= m->burstUntil) { m->holdUntil = s->now + AbUniform(m, t->pauseMin, t->pauseMax); return; }
    if (a->busy || s->now < m->nextShot || s->now - m->lastSeenAt[m->target] > 0.7) return;
    const float* tp = m->lastSeen[m->target];
    float dx = tp[0] - a->pos[0], dy = tp[1] - a->pos[1], d = sqrtf(dx * dx + dy * dy);
    float fl = sqrtf(a->fwd[0] * a->fwd[0] + a->fwd[1] * a->fwd[1]);
    if (d > 2.0f && fl > 1e-3f && (dx * a->fwd[0] + dy * a->fwd[1]) / (d * fl) < t->faceCos) return;
    for (uint32_t k = 0; k < a->attackCount && k < AB_ATTACKS; k++)
    {
        const AbAttack* at = &a->attacks[k];
        if (at->kind > 1 || d < at->minRange || d > at->maxRange) continue;
        out->fire = at->kind == 1 ? AB_FIRE_TARGETED : AB_FIRE_PLAIN;
        out->ability = at->ability;
        m->nextShot = s->now + t->shotGap;
        return;
    }
}

/* One agent's think: perception, target choice, then movement, facing, attacks and its next ray. */
static void AbThink(const AbType* t, const AbSnapshot* s, const AbAgentIn* a, AbIntent* out)
{
    AbMemory* m = &g_abMemory[a->slot % AB_MAX];
    if (a->fresh || m->entity != a->entity)
    {
        memset(m, 0, sizeof *m);
        m->entity = a->entity; m->target = -1; m->rng = a->entity * 2654435761u + 1u; m->lastThink = s->now;
        for (int p = 0; p < AB_PLAYERS; p++) m->lastSeenAt[p] = -1e9;
    }
    memset(out, 0, sizeof *out);
    out->entity = a->entity; out->slot = a->slot; out->target = -1; out->ray = -1;
    float dt = (float)(s->now - m->lastThink);
    if (dt < 0.0f || dt > 1.0f) dt = 1.0f / 60.0f;
    m->lastThink = s->now;
    AbPerceive(t, s, a, m, dt);
    AbChoose(t, s, a, m);
    /* the next line of sight: the target's, else the Helldivers in view in turn */
    int ray = m->target;
    for (uint32_t k = 0; ray < 0 && k < s->playerCount; k++)
    {
        int p = (m->rayTurn + (int)k) % (int)s->playerCount;
        if (s->players[p].alive && AbInView(t, a, s->players[p].pos, m->awareness[p] > 0.0f)) ray = p;
    }
    m->rayTurn = (int8_t)((m->rayTurn + 1) % (s->playerCount ? (int)s->playerCount : 1));
    out->ray = (int8_t)ray;
    if (!m->engaged) return;
    int recent = s->now - m->lastSeenAt[m->target] < 3.0;
    out->engaged = 1; out->target = m->target; out->seen = s->now - m->lastSeenAt[m->target] < 0.5;
    memcpy(out->targetPos, m->lastSeen[m->target], 12);
    float fx = out->targetPos[0] - a->pos[0], fy = out->targetPos[1] - a->pos[1], fl = sqrtf(fx * fx + fy * fy);
    if (recent && fl > 1e-3f) { out->face = 1; out->faceDir[0] = fx / fl; out->faceDir[1] = fy / fl; out->faceDir[2] = 0.0f; }
    AbMove(t, s, a, m, out, recent);
    AbFire(t, s, a, m, out);
}

/* The worker's pass over a snapshot: agents whose turn it is think, the others keep their last intent (the game thread
 * holds those). Returns how many thought. */
static uint32_t AbRun(const AbSnapshot* s, AbIntent* out, uint8_t* thought)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->agentCount; i++)
    {
        const AbAgentIn* a = &s->agents[i];
        int due = a->fresh || g_abMemory[a->slot % AB_MAX].entity != a->entity || (s->frame + a->slot) % AB_THINK_EVERY == 0;
        thought[i] = (uint8_t)due;
        if (!due) continue;
        AbThink(&kAbDevastator, s, a, &out[i]);
        n++;
    }
    return n;
}
