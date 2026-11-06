/* Shields that stop grenades (the stats field shield.blocks_grenades, a byte in the Shield record's trailing padding).
 * A sphere shield is a real Havok body and only the collision matrix keeps grenades out of it, so the pair goes in while
 * such a shield stands. The matrix is shared by every shield and the sphere is solid, so a detour on the filter's pair
 * check drops the pair for every other shield, and for a grenade already inside: it leaves as a bullet does.
 *
 * Shields that stop enemies (shield.blocks_enemies, the FX-12's). Enemies have no Havok mover, so the pair can't hold them.
 * A ring of navmesh material 6 outside the bubble makes the game's own breach logic stop them and attack, as it does at a
 * gate. Charges and pounces skip that, so the one write of every object's position puts back whatever crosses the bubble,
 * and a charge held there plays the reaction its hit zone would. */

#define SB_MANAGER 0x3326630u        /* game.dll: the Shield manager */
#define SB_RECORD_OF 0x4FB390u       /* game.dll: a shield's record, the way the game's own readers get it */
#define SB_FILTER_PAIRS 0xCE5A10u    /* exe: slot +0x18 of both collision filters, called for new pairs of nearby bodies */
#define SB_HANDLE_BODY 0x77D6F0u     /* exe: a physics handle's world and body id */
#define SB_PROPERTIES 0x27C5E48u     /* exe: +0xF8 filter count, +0x100 their name hashes */
#define SB_FILTER_A 0x27C5E40u
#define SB_FILTER_B 0x27C5B90u
#define SB_MATRIX 0x28u              /* 16 bytes for each filter, a bit for each other filter */
#define SB_FLAG 0x151u
#define SB_FLAG_ENEMIES 0x152u
#define SB_BUBBLE 0x78u              /* the record's bubble unit: a ring suits only the relay's, which stands still */
#define SB_RELAY_BUBBLE 0xC6E942A840EF3E09ull
#define SB_LIVE 0x14u                /* the manager's +0x58 rows: +2 broken, +4 health */
#define SB_WORLDS 0x27BA890u         /* exe: 0xB0 for each physics world */
#define SB_MOVE 0x59FED0u            /* game.dll: one object's motion for a frame, ending at the single position write */
#define SB_PLAY 0x7CAF40u            /* game.dll: starts an ability */
#define SB_PLAY_MANAGER 0x3326640u   /* game.dll: 0x7CAF40's first argument */
#define SB_NAV_RECORD 0x505B40u      /* game.dll: a resource's Navigation record: +4 its profile */
#define SB_NAV_HUMAN 0xF3B52DB1u     /* SEAF, crew and the two profiles after it pass */
#define SB_NAV_HUMAN_B 0x9AE897D0u
#define SB_NAV_HUMAN_C 0x49B287D9u
#define SB_CUT_CREATE 0x812CC0u      /* exe: a silhouette generator, the navmesh cut */
#define SB_CUT_REMOVE 0x8127A0u
#define SB_CUT_ENABLE 0x812AE0u
#define SB_CUT_LOCKS 0x27C6350u      /* exe: a world's cut lock; zero after teardown, when the cut calls would crash */
#define SB_CUT_MATERIAL 0x400003E6u  /* navmesh material 6: what enemies with a breach ability attack */
#define SB_RING_IN 0.5f              /* past the surface: close enough for a swing to reach the bubble */
#define SB_WALL 0.6f                 /* the hard stop holds a centre this far out */
#define SB_GROUND 3.0f               /* this near the centre's height, a held enemy is on the ground */
#define SB_SETTLE 3.0f               /* a relay's bubble rides the pod down first */
#define SB_HULLS 16
#define SB_HULL_POINTS 8
#define SB_RINGS 8
#define SB_CHARGES 64
#define SB_CHARGE_LIFE 12.0f
#define SB_MAX 32
#define SB_BODY_STRIDE 0xA0u
#define SB_HASH_SHIELD 0xC61AD6E5u
#define SB_HASH_GRENADE 0xB03B6BFFu
#define SB_NO_BODY 0xFFFFFFu
#define SB_BALL 0x16F397CAu          /* a stratagem ball: its resource's top half, as a body holds it at +0x98 */

static const uint8_t kShieldBlockPin[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x41, 0x56, 0x49, 0x8B, 0xE8};

static const uint8_t kSbMovePin[] = {0x40, 0x55, 0x53, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D};
static const uint8_t kSbPlayPin[] = {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x18, 0x55, 0x56, 0x57, 0x41, 0x55};
static const uint8_t kSbCutPin[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
static const uint8_t kSbRemovePin[] = {0x85, 0xD2, 0x0F, 0x84, 0x32, 0x01, 0x00, 0x00, 0x41, 0x54, 0x41, 0x55};
static const uint8_t kSbEnablePin[] = {0x85, 0xD2, 0x0F, 0x84, 0xD1, 0x01, 0x00, 0x00, 0x56, 0x41, 0x54, 0x41};

typedef struct SbShield { uint32_t body; float radius, x, y, z; int grenades, wall; } SbShield;
typedef struct SbList { int count, grenades, walls; SbShield shields[SB_MAX]; } SbList;
typedef struct SbRing
{
    uint32_t handle, cut, world;      /* handle: the shield's; 0 a free slot */
    float settled, x, y, z;
    int live, off;
    float points[SB_HULLS * SB_HULL_POINTS * 3];
    uint32_t sizes[SB_HULLS];
} SbRing;
typedef struct SbCharge { volatile uint32_t entity, reaction; volatile float age; } SbCharge;
typedef struct SbCutArgs
{
    uint32_t type, world, material, enable;
    const void* transform;
    uint64_t key;
    const void* points;
    const void* sizes;
    int32_t pointCount, sizeCount;
    uint32_t packed;
} SbCutArgs;
typedef int (*SbPairsFn)(void* filter, uintptr_t world, uint32_t* pairs, int count);
typedef void (*SbMoveFn)(void* motion, uint32_t index, float dt);
typedef uint64_t (*SbPlayFn)(void* manager, uint32_t entity, uint32_t ability, uint32_t a, float b);

static SbPairsFn g_sbPairsOriginal;
static SbList g_sbLists[2];
static SbList* volatile g_sbActive;   /* read on the physics threads; NULL leaves every pair as the game decided */
static int g_sbNext, g_sbState, g_sbShieldRow = -1, g_sbGrenadeRow = -1, g_sbPaired;
static uint32_t g_sbTicks;
static ULONGLONG g_sbFirstTry;
static SbMoveFn g_sbMoveOriginal;
static SbPlayFn g_sbPlayOriginal;
static int g_sbWallState;            /* 1: the enemy hooks are in */
static float g_sbDt;
static SbRing g_sbRings[SB_RINGS];
static SbCharge g_sbCharges[SB_CHARGES];
static volatile uint32_t g_sbHeld[64];
static volatile long g_sbHeldWrite, g_sbHeldRead;

/* Whether a pair the matrix allows stays: not for a shield off the list, a body inside the sphere or a stratagem ball,
 * which shares the grenades' filter but must reach the ground. */
static int ShieldBlockKeeps(const SbList* list, uint32_t shield, const uint8_t* shieldBody, const uint8_t* body)
{
    if (*(const uint32_t*)(body + 0x98) == SB_BALL) return 0;
    const float* shieldAt = (const float*)(shieldBody + 0x30), *grenadeAt = (const float*)(body + 0x30);
    for (int i = 0; i < list->count; i++)
    {
        if (list->shields[i].body != shield) continue;
        if (!list->shields[i].grenades) return 0;
        float x = grenadeAt[0] - shieldAt[0], y = grenadeAt[1] - shieldAt[1], z = grenadeAt[2] - shieldAt[2];
        return x * x + y * y + z * z >= list->shields[i].radius * list->shields[i].radius;
    }
    return 0;
}

static int ShieldBlockPairsDetour(void* filter, uintptr_t world, uint32_t* pairs, int count)
{
    int kept = g_sbPairsOriginal(filter, world, pairs, count);
    const SbList* list = g_sbActive;
    if (list == NULL || kept <= 0) return kept;
    const uint8_t* bodies = *(const uint8_t**)(world + 0x38);
    int out = 0;
    for (int i = 0; i < kept; i++)
    {
        uint32_t a = pairs[i * 2] & SB_NO_BODY, b = pairs[i * 2 + 1] & SB_NO_BODY;
        const uint8_t* bodyA = bodies + (uintptr_t)a * SB_BODY_STRIDE;
        const uint8_t* bodyB = bodies + (uintptr_t)b * SB_BODY_STRIDE;
        int rowA = *(const uint32_t*)(bodyA + 0x6C) & 0x7F, rowB = *(const uint32_t*)(bodyB + 0x6C) & 0x7F, keep = 1;
        if (rowA == g_sbShieldRow && rowB == g_sbGrenadeRow)
            keep = ShieldBlockKeeps(list, a, bodyA, bodyB);
        else if (rowB == g_sbShieldRow && rowA == g_sbGrenadeRow)
            keep = ShieldBlockKeeps(list, b, bodyB, bodyA);
        if (keep)
        {
            pairs[out * 2] = pairs[i * 2];
            pairs[out * 2 + 1] = pairs[i * 2 + 1];
            out++;
        }
    }
    return out;
}

/* Puts a position that crossed into a shield back at its wall. Near ground level sideways only, so walkers aren't lifted
 * up the curve; higher up along the radius, so a pounce from above stops on the dome. */
static int ShieldBlockHold(const SbShield* s, const float* before, float* now)
{
    float wall = s->radius + SB_WALL;
    float ox = before[0] - s->x, oy = before[1] - s->y, oz = before[2] - s->z;
    float nx = now[0] - s->x, ny = now[1] - s->y, nz = now[2] - s->z;
    float was = sqrtf(ox * ox + oy * oy + oz * oz), is = sqrtf(nx * nx + ny * ny + nz * nz), flat = sqrtf(nx * nx + ny * ny);
    if (was < wall - 0.05f || is >= wall || is < 0.01f) return 0;
    if (nz > -SB_GROUND && nz < SB_GROUND && flat > 0.01f)
    {
        float reach = sqrtf(wall * wall - nz * nz) / flat;
        now[0] = s->x + nx * reach;
        now[1] = s->y + ny * reach;
        return 1;
    }
    now[0] = s->x + nx / is * wall;
    now[1] = s->y + ny / is * wall;
    now[2] = s->z + nz / is * wall;
    return 1;
}

/* An object's entity when the wall stops it, 0 for one with no navigation or a human's: they pass. */
static uint32_t ShieldBlockEnemy(void* motion, uint32_t index)
{
    const uint8_t* object = (*(const uint8_t***)((uint8_t*)motion + 0x48B8))[index];
    if (object == NULL) return 0;
    const uint8_t* (*navigation)(uint64_t) = (const uint8_t* (*)(uint64_t))(ApiModuleBase("game.dll", NULL) + SB_NAV_RECORD);
    const uint8_t* nav = navigation(*(const uint64_t*)object);
    if (nav == NULL) return 0;
    uint32_t profile = *(const uint32_t*)(nav + 4);
    return profile == SB_NAV_HUMAN || profile == SB_NAV_HUMAN_B || profile == SB_NAV_HUMAN_C ? 0 : *(const uint32_t*)(object + 8);
}

/* On the motion jobs, for every object: one read and a compare per wall unless something crosses one. */
static void ShieldBlockMoveDetour(void* motion, uint32_t index, float dt)
{
    const SbList* list = g_sbActive;
    if (list == NULL || list->walls == 0) { g_sbMoveOriginal(motion, index, dt); return; }
    float* row = (float*)(*(uint8_t**)((uint8_t*)motion + 0x48D0) + (uintptr_t)index * 0xA4);
    float before[3] = {row[0], row[1], row[2]};
    g_sbMoveOriginal(motion, index, dt);
    for (int i = 0; i < list->count; i++)
    {
        float moved[3] = {row[0], row[1], row[2]};
        if (!list->shields[i].wall || !ShieldBlockHold(&list->shields[i], before, moved)) continue;
        uint32_t entity = ShieldBlockEnemy(motion, index);
        if (entity == 0) return;
        memcpy(row, moved, sizeof moved);
        g_sbHeld[InterlockedIncrement(&g_sbHeldWrite) % 64] = entity;
        return;
    }
}

/* The reaction a charge plays when its zone hits something; 0 for any other ability. */
static uint32_t ShieldBlockReaction(uint32_t ability)
{
    switch (ability)
    {
    case 0xDE: case 0xE3: return 0xE1;
    case 0x1D0: return 0x1D4;
    case 0x61B: return 0x622;
    case 0x6EF: return 0x6F3;
    default: return 0;
    }
}

static int ShieldBlockEndsCharge(uint32_t ability)
{
    return (ability >= 0xDF && ability <= 0xE2) || (ability >= 0x1D1 && ability <= 0x1D5) || ability == 0x61E || ability == 0x622 ||
        ability == 0x623 || ability == 0x6F1 || ability == 0x6F3 || ability == 0x6F7;
}

static void ShieldBlockNoteCharge(uint32_t entity, uint32_t ability)
{
    uint32_t reaction = ShieldBlockReaction(ability);
    SbCharge* room = NULL;
    for (int i = 0; i < SB_CHARGES; i++)
    {
        SbCharge* c = &g_sbCharges[i];
        if (c->entity == entity)
        {
            if (reaction) { c->reaction = reaction; c->age = 0; }
            else if (ShieldBlockEndsCharge(ability)) c->entity = 0;
            return;
        }
        if (c->entity == 0 && room == NULL) room = c;
    }
    if (reaction && room) { room->reaction = reaction; room->age = 0; room->entity = entity; }
}

static uint64_t ShieldBlockPlayDetour(void* manager, uint32_t entity, uint32_t ability, uint32_t a, float b)
{
    const SbList* list = g_sbActive;
    if (list != NULL && list->walls) ShieldBlockNoteCharge(entity, ability);
    return g_sbPlayOriginal(manager, entity, ability, a, b);
}

/* Charges have no wall test of their own, so a held one is told it hit something, on the game thread as the game's calls are. */
static void ShieldBlockHitCharge(uintptr_t game, uint32_t entity)
{
    for (int i = 0; i < SB_CHARGES; i++)
    {
        SbCharge* c = &g_sbCharges[i];
        if (c->entity != entity) continue;
        uint32_t reaction = c->reaction;
        c->entity = 0;
        uintptr_t manager;
        if (EvpPtr(game + SB_PLAY_MANAGER, &manager)) g_sbPlayOriginal((void*)manager, entity, reaction, 1, 1.0f);
        return;
    }
}

static void ShieldBlockCharges(uintptr_t game, float dt)
{
    for (int i = 0; i < SB_CHARGES; i++)
        if (g_sbCharges[i].entity && (g_sbCharges[i].age += dt) > SB_CHARGE_LIFE) g_sbCharges[i].entity = 0;
    while (g_sbHeldRead < g_sbHeldWrite) ShieldBlockHitCharge(game, g_sbHeld[++g_sbHeldRead % 64]);
}

/* The game's own API sets this bit around cut calls. */
static uint32_t ShieldBlockFlush(void)
{
    uint32_t was = __builtin_ia32_stmxcsr();
    __builtin_ia32_ldmxcsr(was | 0x8000);
    return was;
}

static int ShieldBlockLockAlive(uintptr_t exe, uint32_t world)
{
    uint64_t lock = 0;
    return !SafeRead(exe + SB_CUT_LOCKS + (uintptr_t)world * 8, &lock, 8) && lock != 0;
}

/* 16 thin wedges: a filled cut would leave nothing inside for enemies already there to stand on. */
static void ShieldBlockHulls(SbRing* r, float radius)
{
    const float inner = radius + SB_RING_IN, outer = inner + 1.0f, tau = 6.2831853f;
    float* p = r->points;
    for (int h = 0; h < SB_HULLS; h++)
    {
        float a = tau * (float)h / SB_HULLS, b = tau * (float)(h + 1) / SB_HULLS;
        const float corners[4][2] = {{cosf(a) * inner, sinf(a) * inner}, {cosf(b) * inner, sinf(b) * inner},
                                     {cosf(b) * outer, sinf(b) * outer}, {cosf(a) * outer, sinf(a) * outer}};
        for (int i = 0; i < SB_HULL_POINTS; i++)
        {
            *p++ = r->x + corners[i % 4][0];
            *p++ = r->y + corners[i % 4][1];
            *p++ = r->z + (i < 4 ? -5.0f : 5.0f);
        }
        r->sizes[h] = SB_HULL_POINTS;
    }
}

static void ShieldBlockRingMake(uintptr_t exe, SbRing* r, float radius)
{
    uint32_t (*create)(SbCutArgs*) = (uint32_t (*)(SbCutArgs*))(exe + SB_CUT_CREATE);
    r->live = -1;
    if (!ShieldBlockLockAlive(exe, r->world)) return;
    ShieldBlockHulls(r, radius);
    SbCutArgs args = {.type = 2, .world = r->world, .material = SB_CUT_MATERIAL, .enable = 1, .points = r->points, .sizes = r->sizes,
                      .pointCount = SB_HULLS * SB_HULL_POINTS, .sizeCount = SB_HULLS, .packed = 1};
    uint32_t was = ShieldBlockFlush();
    r->cut = create(&args);
    __builtin_ia32_ldmxcsr(was);
    if (r->cut) r->live = 1;
}

/* A broken bubble is no wall: the cut goes off at once and comes back with the bubble. */
static void ShieldBlockRingEnable(uintptr_t exe, SbRing* r, int on)
{
    void (*enable)(uint32_t*, int, int) = (void (*)(uint32_t*, int, int))(exe + SB_CUT_ENABLE);
    if (r->live != 1 || r->off == !on || !ShieldBlockLockAlive(exe, r->world)) return;
    uint32_t was = ShieldBlockFlush();
    enable(&r->cut, 1, on);
    __builtin_ia32_ldmxcsr(was);
    r->off = !on;
}

static void ShieldBlockRingFree(uintptr_t exe, SbRing* r)
{
    void (*remove)(uint32_t*, int) = (void (*)(uint32_t*, int))(exe + SB_CUT_REMOVE);
    if (r->live == 1 && ShieldBlockLockAlive(exe, r->world))
    {
        uint32_t was = ShieldBlockFlush();
        remove(&r->cut, 1);
        __builtin_ia32_ldmxcsr(was);
    }
    r->handle = 0;
}

static SbRing* ShieldBlockRingOf(uint32_t handle, uint32_t world, const SbShield* s)
{
    SbRing* room = NULL;
    for (int i = 0; i < SB_RINGS; i++)
    {
        if (g_sbRings[i].handle == handle) return &g_sbRings[i];
        if (g_sbRings[i].handle == 0 && room == NULL) room = &g_sbRings[i];
    }
    if (room == NULL) return NULL;
    room->handle = handle; room->world = world; room->live = 0; room->off = 0; room->settled = 0;
    room->x = s->x; room->y = s->y; room->z = s->z;
    return room;
}

static void ShieldBlockRingSee(uintptr_t exe, uint32_t handle, uint32_t world, const SbShield* s, int broken)
{
    SbRing* r = ShieldBlockRingOf(handle, world, s);
    if (r == NULL) return;
    float mx = s->x - r->x, my = s->y - r->y, mz = s->z - r->z;
    r->settled += g_sbDt;
    if (r->live == 0 && mx * mx + my * my + mz * mz > 0.01f) { r->x = s->x; r->y = s->y; r->z = s->z; r->settled = 0; }
    if (r->live == 0 && r->settled >= SB_SETTLE) ShieldBlockRingMake(exe, r, s->radius);
    ShieldBlockRingEnable(exe, r, !broken);
}

static void ShieldBlockRingsDrop(uintptr_t exe, const uint32_t* seen, int count)
{
    for (int i = 0; i < SB_RINGS; i++)
    {
        int found = g_sbRings[i].handle == 0;
        for (int k = 0; k < count; k++) found |= seen[k] == g_sbRings[i].handle;
        if (!found) ShieldBlockRingFree(exe, &g_sbRings[i]);
    }
}

/* A filter's row is its place in the physics properties, so rows are found by name. */
static int ShieldBlockRows(uintptr_t exe)
{
    uintptr_t properties, keys;
    uint32_t count, key;
    if (!EvpPtr(exe + SB_PROPERTIES, &properties) || SafeRead(properties + 0xF8, &count, 4) || !EvpPtr(properties + 0x100, &keys)) return 0;
    for (uint32_t i = 0; i < count && i < 128; i++)
    {
        if (SafeRead(keys + i * 4, &key, 4)) return 0;
        if (key == SB_HASH_SHIELD) g_sbShieldRow = (int)i;
        if (key == SB_HASH_GRENADE) g_sbGrenadeRow = (int)i;
    }
    return g_sbShieldRow >= 0 && g_sbGrenadeRow >= 0;
}

static void ShieldBlockBit(uintptr_t filter, int row, int bit, int on)
{
    volatile LONG* word = (volatile LONG*)(filter + SB_MATRIX + (uintptr_t)row * 16 + (uintptr_t)(bit >> 5) * 4);
    if (on) InterlockedOr(word, (LONG)(1u << (bit & 31)));
    else InterlockedAnd(word, (LONG)~(1u << (bit & 31)));
}

static void ShieldBlockPair(uintptr_t exe, int on)
{
    const uint32_t globals[2] = {SB_FILTER_A, SB_FILTER_B};
    for (int i = 0; i < 2; i++)
    {
        uintptr_t filter;
        if (!EvpPtr(exe + globals[i], &filter)) continue;
        ShieldBlockBit(filter, g_sbGrenadeRow, g_sbShieldRow, on);
        ShieldBlockBit(filter, g_sbShieldRow, g_sbGrenadeRow, on);
    }
    g_sbPaired = on;
}

/* A shield's body, world and position; 0 if it has none yet. */
static int ShieldBlockWhere(uintptr_t exe, uint32_t handle, SbShield* out, uint32_t* world)
{
    uint32_t (*bodyOf)(uint32_t, uint32_t*) = (uint32_t (*)(uint32_t, uint32_t*))(exe + SB_HANDLE_BODY);
    uint32_t body = SB_NO_BODY;
    uintptr_t physics, bodies;
    *world = bodyOf(handle, &body);
    body &= SB_NO_BODY; /* an actor's id has a generation in its top byte; the filter's pairs compare the low 24 bits */
    if (body == SB_NO_BODY || *world > 3 || !EvpPtr(exe + SB_WORLDS + (uintptr_t)*world * 0xB0, &physics) || !EvpPtr(physics + 0x38, &bodies)) return 0;
    out->body = body;
    return !SafeRead(bodies + (uintptr_t)body * SB_BODY_STRIDE + 0x30, &out->x, 12);
}

/* Adds the shield at a manager index, if its record has a field on and its body exists. */
static void ShieldBlockAdd(uintptr_t game, uintptr_t exe, uintptr_t manager, uint32_t index, SbList* list, uint32_t* seen, int* seenCount)
{
    void* (*recordOf)(void*) = (void* (*)(void*))(game + SB_RECORD_OF);
    uintptr_t bindings, rows, live, binding;
    uint32_t handle, world;
    uint8_t state[3];
    if (!EvpPtr(manager + 0x48, &bindings) || !EvpPtr(manager + 0x50, &rows) || !EvpPtr(manager + 0x58, &live)) return;
    if (!EvpPtr(bindings + (uintptr_t)index * 8, &binding)) return;
    const uint8_t* record = (const uint8_t*)recordOf((void*)binding);
    if (record == NULL || (record[SB_FLAG] == 0 && record[SB_FLAG_ENEMIES] == 0)) return;
    SbShield s = {.radius = *(const float*)record, .grenades = record[SB_FLAG] != 0};
    int wall = g_sbWallState == 1 && record[SB_FLAG_ENEMIES] != 0 && *(const uint64_t*)(record + SB_BUBBLE) == SB_RELAY_BUBBLE;
    if (s.radius <= 0 || SafeRead(rows + (uintptr_t)index * 0x88, &handle, 4) || handle == 0 || handle == 0xFFFFFFFFu) return;
    if (!ShieldBlockWhere(exe, handle, &s, &world) || SafeRead(live + (uintptr_t)index * SB_LIVE, state, 3)) return;
    s.wall = wall && state[2] == 0;
    if (wall) { seen[(*seenCount)++] = handle; ShieldBlockRingSee(exe, handle, world, &s, state[2] != 0); }
    list->shields[list->count++] = s;
    list->grenades += s.grenades;
    list->walls += s.wall;
}

/* The shields standing now that have a field on. On the game thread, which adds and removes them. */
static void ShieldBlockScan(uintptr_t game, uintptr_t exe, SbList* list)
{
    uint32_t seen[SB_MAX];
    int seenCount = 0;
    list->count = list->grenades = list->walls = 0;
    uintptr_t manager, table;
    uint32_t capacity, empty;
    if (EvpPtr(game + SB_MANAGER, &manager) && EvpPtr(manager + 0x30, &table) && !SafeRead(manager + 0x38, &capacity, 4) &&
        !SafeRead(manager + 0x3C, &empty, 4) && capacity <= 65536)
    {
        const uint32_t* entries = (const uint32_t*)table;
        for (uint32_t i = 0; i < capacity && list->count < SB_MAX; i++)
            if (entries[i * 2] != empty && entries[i * 2 + 1] != 0xFFFFFFFFu)
                ShieldBlockAdd(game, exe, manager, entries[i * 2 + 1], list, seen, &seenCount);
    }
    ShieldBlockRingsDrop(exe, seen, seenCount);
}

static void ShieldBlockTick(float dt)
{
    if (g_sbState != 1) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL), exe = (uintptr_t)GetModuleHandleW(NULL);
    if (g_sbWallState == 1) ShieldBlockCharges(game, dt);
    g_sbDt += dt;
    if (++g_sbTicks & 3) return;
    /* Teardown frees the cuts and their lock, so leaving a mission forgets them; calling them after it would crash. */
    if (ApiGameState(NULL) != HF_STATE_MISSION) memset(g_sbRings, 0, sizeof g_sbRings);
    SbList* next = &g_sbLists[g_sbNext];
    ShieldBlockScan(game, exe, next);
    g_sbDt = 0;
    if (next->grenades && g_sbShieldRow < 0 && !ShieldBlockRows(exe))
    {
        g_sbState = -1;
        g_sbActive = NULL;
        Note("shield block: the physics filters are not the reviewed ones; shields stop no grenades");
        return;
    }
    /* The list goes live before the pair is set, so no shield passes the matrix unchecked. */
    g_sbActive = next->count ? next : NULL;
    g_sbNext ^= 1;
    if ((next->grenades != 0) != g_sbPaired)
    {
        ShieldBlockPair(exe, next->grenades != 0);
        Note("shield block: grenades %s", next->grenades ? "stop at the marked shields" : "pass every shield again");
    }
}

/* The enemy half needs every part or none: a ring without the hard stop would let charges through. */
static void ShieldBlockInstallWalls(uintptr_t exe)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    g_sbWallState = -1;
    if (!game || !PinMatches(game + SB_MOVE, kSbMovePin, sizeof kSbMovePin) || !PinMatches(game + SB_PLAY, kSbPlayPin, sizeof kSbPlayPin) ||
        !PinMatches(exe + SB_CUT_CREATE, kSbCutPin, sizeof kSbCutPin) || !PinMatches(exe + SB_CUT_REMOVE, kSbRemovePin, sizeof kSbRemovePin) ||
        !PinMatches(exe + SB_CUT_ENABLE, kSbEnablePin, sizeof kSbEnablePin))
    {
        Note("shield block: the movement or navmesh code is not the reviewed one; shields stop no enemies");
        return;
    }
    if (LoaderHook("shield wall play", game + SB_PLAY, (void*)ShieldBlockPlayDetour, (void**)&g_sbPlayOriginal)) return;
    if (LoaderHook("shield wall move", game + SB_MOVE, (void*)ShieldBlockMoveDetour, (void**)&g_sbMoveOriginal)) return;
    g_sbWallState = 1;
}

/* From game load, like the game thread: the exe's code is decoded by then, or soon after. */
static void ShieldBlockInstall(void)
{
    if (g_sbState != 0) return;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    ULONGLONG now = GetTickCount64();
    if (!g_sbFirstTry) g_sbFirstTry = now;
    if (strcmp(g_build, "25480438")) { g_sbState = -1; return; }
    if (!PinMatches(exe + SB_FILTER_PAIRS, kShieldBlockPin, sizeof kShieldBlockPin))
    {
        if (now - g_sbFirstTry < 60000) return;
        g_sbState = -1;
        Note("shield block: the collision filter is not the reviewed one; shields stop no grenades");
        return;
    }
    g_sbState = LoaderHook("shield block", exe + SB_FILTER_PAIRS, (void*)ShieldBlockPairsDetour, (void**)&g_sbPairsOriginal) ? -1 : 1;
    if (g_sbState == 1) ShieldBlockInstallWalls(exe);
}
