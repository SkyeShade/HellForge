/* The living world (development switch HELLFORGE_LIVING_WORLD=1; host only; build 25480438). The director's ambient
 * patrols are switched off and replaced by squads the loader keeps as records ("offline") that walk the level's route
 * graph; near the players a squad is spawned as a real squad ("online") from the director's own patrol groups, kept
 * alive against the native despawners, steered along its route, and taken offline again with its losses kept.
 * Included by hellforge_modloader.c after difficulty_world_move.c (the players, the difficulty rows, the world).
 *
 * Every address and rule here is from the static analysis in the knowledge base (living-world-plan, Phase 0 and the
 * Phase 1 static answers); the code pins below refuse to run on code that is not the reviewed one. */
#define LW_DIRECTOR 0x3326D10u      /* [game+]: the director (dir) */
#define LW_AIJOB 0x33264D8u         /* [game+]: AiJob manager: hash +0x20, records +0x48 (0x1C: +0xC job, +0x18 keep) */
#define LW_AISQUAD 0x33265A0u       /* [game+]: AiSquad: hash +0x20, records +0x40 (0x2510: members +0x20 + 24 i, count +0x320) */
#define LW_BUDGET 0x3326838u        /* [game+]: the budget: hash +0x1670, records [+0x1690] + 16 i (+0 player, +4 score) */
#define LW_SEATS 0x3326D88u         /* [game+]: SeatCollection: hash +0x20 */
#define LW_STATE 0x346BF98u         /* [game+]: the entity state the removal 0xFDC310 takes */
#define LW_NONE 0x3483C4Cu          /* [game+]: the "no entity" id the budget's reset writes */
#define LW_INVALID 0x3483C20u       /* [game+]: the invalid id the AiJob lookup tests first */
#define LW_PATROL_FLAGS 0x5Eu       /* 0x02 keep (the squad entity) | 0x04 queue | 0x08 no nav snap | 0x10 no ground ray (LwPlace placed it) | 0x40 the loader enforces the gates */
#define LW_JOB_PATROL 1u
#define LW_JOB_BATTLE 10u           /* state 10 re-reads job_position (0x7FD6FE): the route's waypoints */

typedef uintptr_t (*LwSpawnFn)(uintptr_t dir, uintptr_t group, const float* pos, uint32_t job, uint32_t flags, const float* target,
    const float* quat, const void* spawnData, float delay, uint32_t memberCap);
typedef void (*LwRemoveFn)(uintptr_t state, uint32_t id);
typedef void (*LwUnseatFn)(uintptr_t unused, uint32_t entity);
typedef void (*LwSetJobFn)(uintptr_t jobs, uint32_t entity, uint32_t job);
typedef void (*LwGoalFn)(uint32_t squad, const float* pos);

/* The functions the loader calls and the instructions its rules rest on, as reviewed. */
static const ActPin kLwPins[] = {
    {0, 0x953580u, "40555356574154415541564157488dac24a8fdffff4881ec"}, /* spawn request */
    {0, 0xFDC310u, "48895c2408574883ec303b154c824a028bda488bf90f84d4"}, /* queued removal */
    {0, 0x636240u, "48895c2420564883ec303b15d0d9e4028bda488b352f0bcf"}, /* unseat before removal */
    {0, 0x7F5120u, "405355565741544883ec403b15efeac802458be08bda488b"}, /* set_job */
    {0, 0xA13520u, "4053554883ec383b0df306a702488bea8bd90f8471010000"}, /* squad goal */
    {0, 0x13CA380u, "4c8bdc49895b104989732057"},                         /* the ground ray (z + 500 m down 1000 m) */
    {0, 0x959650u, "40574883ec3083b98006000000488bf9"},                 /* the effective patrol cap */
    {0, 0x8F32B0u, "4883ec28488b05e533a3028378080074"},                 /* the mission is ending */
    {0, 0xF74750u, "48895c2410574883ec2048833900488bd9"},               /* the zone at a position */
    {0, 0x94FF3Au, "448ba55c050000"}, {0, 0x94FEE6u, "448bbd60050000"}, {0, 0x94FF33u, "448bad64050000"}, /* the caps read */
    {0, 0x9513A1u, "4439a014060000"},                                    /* the patrol loop's gate */
    {0, 0xA2DC87u, "8079180075"},                                        /* the removability test reads the keep byte +0x18 */
    {0, 0x954B8Cu, "410fb6442411"},                                      /* keep travels from rec+0x11 (movzx eax, [r12+0x11]) */
};

static struct
{
    int state;              /* 0 not decided, 1 on, -1 off */
    int pinsOk;
    uintptr_t game;
    LwSpawnFn spawn; LwRemoveFn remove; LwUnseatFn unseat; LwSetJobFn setJob; LwGoalFn goal;
    uint32_t saved[DIF_VANILLA + 1][3]; /* each level's patrol caps before they were zeroed (factions 2 / 8 / 4) */
    int savedOk[DIF_VANILLA + 1];
    uint32_t rowsZeroed;
} g_lw;

static int LwEnabled(void)
{
    if (g_lw.state != 0) return g_lw.state > 0;
    char t[4] = "";
    g_lw.state = GetEnvironmentVariableA("HELLFORGE_LIVING_WORLD", t, sizeof t) > 0 && t[0] == '1' ? 1 : -1;
    if (g_lw.state < 0) return 0;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0 || strcmp(g_build, "25480438") || !ActPins(game, kLwPins, sizeof kLwPins / sizeof kLwPins[0]))
    {
        g_lw.state = -1;
        Note("living world: off: the game's code is not the reviewed build");
        return 0;
    }
    g_lw.game = game;
    g_lw.spawn = (LwSpawnFn)(game + 0x953580u); g_lw.remove = (LwRemoveFn)(game + 0xFDC310u);
    g_lw.unseat = (LwUnseatFn)(game + 0x636240u); g_lw.setJob = (LwSetJobFn)(game + 0x7F5120u); g_lw.goal = (LwGoalFn)(game + 0xA13520u);
    g_lw.pinsOk = 1;
    Note("living world: on (HELLFORGE_LIVING_WORLD=1): the director's patrols are replaced by the loader's squads (host only)");
    return 1;
}

/* A write to the game's heap data (records the game keeps, never its image): guarded, no protection change, not a patch. */
static int LwPoke(uintptr_t address, const void* data, size_t size)
{
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)address, data, size, &written) && written == size ? 0 : 1;
}

/* ---- lookups (the game's u32 hash maps; records move, so nothing is kept between ticks) ---------------------------- */

/* An entity's AiJob record, or 0 (as 0xA2DBDE..0xA2DC87: the invalid id first, then the hash; the sentinel -1 refused). */
static uintptr_t LwJobRecord(uint32_t entity)
{
    uintptr_t m; uint32_t invalid = 0, index = 0;
    if (!EvpPtr(g_lw.game + LW_AIJOB, &m) || SafeRead(g_lw.game + LW_INVALID, &invalid, 4) || entity == invalid ||
        !EvpHash(m + 0x20, entity, 8, 4, &index) || index >= EvpU32(m + 0xC, 0)) return 0;
    uintptr_t records;
    return EvpPtr(m + 0x48, &records) ? records + (uintptr_t)index * 0x1C : 0;
}

/* A squad entity's AiSquad record, or 0. */
static uintptr_t LwSquadRecord(uint32_t squad)
{
    uintptr_t m, records; uint32_t index = 0;
    if (!EvpPtr(g_lw.game + LW_AISQUAD, &m) || !EvpHash(m + 0x20, squad, 8, 4, &index) || index > 0x10000 || !EvpPtr(m + 0x40, &records)) return 0;
    return records + (uintptr_t)index * 0x2510;
}

/* A squad's members (looked up afresh; at most 32): their count, -1 when the squad is gone. */
static int LwMembers(uint32_t squad, uint32_t* out)
{
    uintptr_t r = LwSquadRecord(squad);
    if (r == 0) return -1;
    uint32_t count = EvpU32(r + 0x320, 0xFFFFFFFFu);
    if (count > 32) return -1;
    for (uint32_t i = 0; i < count; i++) out[i] = EvpU32(r + 0x20 + 24u * i, 0);
    return (int)count;
}

/* Keeps a member against the budget despawner (conclusion 6): AiJob keep byte +0x18 = 1 (creation-only field: no dirty
 * marking), and its budget score reset to -1.0 with the player field = none when it is >= 0, because the pick 0xA2C030
 * reuses the last score without re-testing removability. */
static void LwKeep(uint32_t member)
{
    uintptr_t job = LwJobRecord(member);
    if (job != 0) { uint8_t keep = 1, was = 0; if (!SafeRead(job + 0x18, &was, 1) && was != 1) LwPoke(job + 0x18, &keep, 1); }
    uintptr_t b, records; uint32_t index = 0, none = 0;
    if (!EvpPtr(g_lw.game + LW_BUDGET, &b) || !EvpHash(b + 0x1670, member, 8, 4, &index) || index >= EvpU32(b + 0x1664, 0) ||
        !EvpPtr(b + 0x1690, &records) || SafeRead(g_lw.game + LW_NONE, &none, 4)) return;
    uintptr_t rec = records + (uintptr_t)index * 16;
    float score = -1.0f;
    if (!SafeRead(rec + 4, &score, 4) && score >= 0.0f) { float reset = -1.0f; LwPoke(rec + 4, &reset, 4); LwPoke(rec, &none, 4); }
}

/* Whether an entity is in SeatCollection (the cull calls 0x636240 first then, 0x95560F..0x95567B). */
static int LwSeated(uint32_t entity)
{
    uintptr_t m; uint32_t value = 0;
    return EvpPtr(g_lw.game + LW_SEATS, &m) && EvpHash(m + 0x20, entity, 8, 4, &value);
}

/* Takes a squad out of the world as the director's cull does: its seated occupants first, then the squad entity (its
 * members follow through the AiSquad destroy callback 0x802EF0; the ring record is freed by the type-0x3E link). */
static void LwRemoveSquad(uint32_t squad)
{
    uintptr_t state;
    if (LwSeated(squad)) g_lw.unseat(0, squad);
    if (EvpPtr(g_lw.game + LW_STATE, &state)) g_lw.remove(state, squad);
}

/* ---- the off switch (Phase 0 conclusion 1; timing "After audit 0f") -------------------------------------------------- */

/* Every row the director copies gets patrol caps 0, from the moment the rows exist, before any mission's first world
 * update; kept for the session. Each level's caps are saved first: the population follows vanilla's numbers. */
static void LwPatrolsOff(void)
{
    static const uint32_t caps[3] = {0x2AC, 0x2B0, 0x2B4};
    uint32_t rows = 0, zero = 0;
    for (uint32_t level = 1; level <= DIF_VANILLA + 1; level++)
    {
        int ready = 0;
        uintptr_t row = DifficultyRowOf(level, &ready);
        if (row == 0) continue;
        uint32_t now[3];
        if (SafeRead(row + caps[0], &now[0], 4) || SafeRead(row + caps[1], &now[1], 4) || SafeRead(row + caps[2], &now[2], 4)) continue;
        if (!g_lw.savedOk[level - 1]) { memcpy(g_lw.saved[level - 1], now, sizeof now); g_lw.savedOk[level - 1] = 1; }
        for (int k = 0; k < 3; k++) if (now[k] != 0) ApiWrite(row + caps[k], &zero, 4);
        rows++;
    }
    if (rows != g_lw.rowsZeroed)
    {
        g_lw.rowsZeroed = rows;
        Note("living world: the director's patrol caps are 0 in %u difficulty rows (level 10's were %u / %u / %u)", rows, g_lw.saved[9][0], g_lw.saved[9][1],
            g_lw.saved[9][2]);
    }
}

/* ---- the route graph (the loader's copy; KB "Phase 1 static answers" 5) ------------------------------------------------
 * G = the generation block (alive the whole mission, overwritten by the next generation); nodes G + g_routeNodes (0x98:
 * +0 x, y in map space, +0x10 + 8 k up to 16 edge pointers, NULL-terminated), count u32 G+0x11A71B4; an edge (0x48)
 * holds its end nodes at +0x10 / +0x18 and is walkable when both route cells +0x20 / +0x28 are set. World = map -
 * [lg+0x1E0] / 2. Copied once the level is generated ([lg+0x1E4] == 3), keyed to the level's seed [lg+0x8]. */
#define LW_NODES_MAX 4096
typedef struct LwNode { float x, y; uint16_t next[16]; uint8_t degree; } LwNode;
static LwNode g_lwNodes[LW_NODES_MAX];
static int g_lwNodeCount;

/* Why the squads wait, noted once per mission (a reason that keeps them from starting is never silent). */
static char g_lwWhy[160];
static void LwWhy(const char* why)
{
    if (!strcmp(g_lwWhy, why)) return;
    snprintf(g_lwWhy, sizeof g_lwWhy, "%s", why);
    Note("living world: squads wait: %s", why);
}

static uintptr_t LwLevel(void)
{
    uintptr_t world, lg;
    return EvpPtr(g_lw.game + 0x3326340u, &world) && EvpPtr(world + 0xAE288, &lg) ? lg : 0;
}

/* Copies the route graph; its node count (0: not ready or unreadable). */
static int LwCopyRoutes(uintptr_t lg)
{
    uintptr_t G = GenerationBlock(g_lw.game);
    uint32_t count = 0;
    float size = 0; /* f32: 0x6287F0.. movss [lg+0x1E0], x 0.5 */
    if (G == 0 || SafeRead(G + 0x11A71B4, &count, 4) || count == 0 || count > LW_NODES_MAX || SafeRead(lg + 0x1E0, &size, 4) || !(size > 0) || size > 65536.0f)
    { LwWhy("the route graph is not readable (block, count or level size)"); return 0; }
    uintptr_t nodes = G + g_routeNodes;
    float half = size * 0.5f;
    for (uint32_t i = 0; i < count; i++)
    {
        uint8_t n[0x98];
        LwNode* o = &g_lwNodes[i];
        memset(o, 0, sizeof *o);
        if (SafeRead(nodes + 0x98u * i, n, sizeof n)) return 0;
        float mx, my; memcpy(&mx, n, 4); memcpy(&my, n + 4, 4);
        o->x = mx - half; o->y = my - half;
        for (int k = 0; k < 16; k++)
        {
            uint64_t e; memcpy(&e, n + 0x10 + 8 * k, 8);
            if (e == 0) break;
            uint64_t ends[4];
            if (SafeRead((uintptr_t)e + 0x10, ends, sizeof ends) || ends[2] == 0 || ends[3] == 0) continue; /* +0x10, +0x18 ends; +0x20, +0x28 cells */
            uint64_t other = ends[0] == nodes + 0x98u * i ? ends[1] : ends[0];
            if (other < nodes || (other - nodes) % 0x98u != 0 || (other - nodes) / 0x98u >= count || o->degree >= 16) continue;
            o->next[o->degree++] = (uint16_t)((other - nodes) / 0x98u);
        }
    }
    g_lwNodeCount = (int)count;
    return (int)count;
}

/* A* over the copy from a to b (straight-line costs); the path's nodes after a, up to max, into out; its length. */
static int LwPath(int a, int b, uint16_t* out, int max)
{
    static float cost[LW_NODES_MAX]; static int16_t from[LW_NODES_MAX]; static uint8_t closed[LW_NODES_MAX];
    static uint16_t heap[LW_NODES_MAX * 4]; static float key[LW_NODES_MAX * 4];
    int n = g_lwNodeCount, size = 0;
    if (a < 0 || b < 0 || a >= n || b >= n || a == b) return 0;
    for (int i = 0; i < n; i++) { cost[i] = 3.0e38f; from[i] = -1; closed[i] = 0; }
    cost[a] = 0; heap[0] = (uint16_t)a; key[0] = 0; size = 1;
    while (size > 0)
    {
        int u = heap[0];
        size--; heap[0] = heap[size]; key[0] = key[size];
        for (int i = 0;;) /* sift down */
        {
            int l = 2 * i + 1, r = l + 1, m = i;
            if (l < size && key[l] < key[m]) m = l;
            if (r < size && key[r] < key[m]) m = r;
            if (m == i) break;
            uint16_t th = heap[i]; heap[i] = heap[m]; heap[m] = th; float tk = key[i]; key[i] = key[m]; key[m] = tk; i = m;
        }
        if (closed[u]) continue;
        closed[u] = 1;
        if (u == b) break;
        for (int k = 0; k < g_lwNodes[u].degree; k++)
        {
            int v = g_lwNodes[u].next[k];
            float dx = g_lwNodes[v].x - g_lwNodes[u].x, dy = g_lwNodes[v].y - g_lwNodes[u].y, c = cost[u] + sqrtf(dx * dx + dy * dy);
            if (closed[v] || c >= cost[v] || size >= LW_NODES_MAX * 4) continue;
            cost[v] = c; from[v] = (int16_t)u;
            float hx = g_lwNodes[b].x - g_lwNodes[v].x, hy = g_lwNodes[b].y - g_lwNodes[v].y;
            int i = size++;
            heap[i] = (uint16_t)v; key[i] = c + sqrtf(hx * hx + hy * hy);
            while (i > 0) /* sift up */
            {
                int q = (i - 1) / 2;
                if (key[q] <= key[i]) break;
                uint16_t th = heap[i]; heap[i] = heap[q]; heap[q] = th; float tk = key[i]; key[i] = key[q]; key[q] = tk; i = q;
            }
        }
    }
    if (from[b] < 0) return 0;
    static uint16_t back[LW_NODES_MAX];
    int len = 0;
    for (int v = b; v != a && len < LW_NODES_MAX; v = from[v]) back[len++] = (uint16_t)v; /* b .. the node after a */
    int written = 0;
    for (int i = len - 1; i >= 0 && written < max; i--) out[written++] = back[i];
    return written;
}

/* ---- groups (KB "Phase 1 static answers" 2): the director's own main patrol groups ----------------------------------- */
#define LW_GROUP(dir, i) ((dir) + 0x3A538u + 0xD8u * (uintptr_t)(i))

/* A position's zone tags (0xF74750(lg, &pos) -> zone definition, row u32 +0x174; rows 1 Colony, 2 Colony + City,
 * 3 Cave: the callbacks 0x9493D0 / 0x949410 / 0x949450). */
static int LwZoneTags(uintptr_t lg, const float* pos, uint32_t* tags)
{
    typedef uintptr_t (*ZoneFn)(uintptr_t lg, const float* pos);
    uintptr_t def = ((ZoneFn)(g_lw.game + 0xF74750u))(lg, pos);
    uint32_t row = def ? EvpU32(def + 0x174, 0) : 0;
    if (row == 1) { tags[0] = 0x0B92E6D4u; return 1; }
    if (row == 2) { tags[0] = 0x0B92E6D4u; tags[1] = 0x1A061654u; return 2; }
    if (row == 3) { tags[0] = 0x04CB576Fu; return 1; }
    return 0;
}

/* A group's members (its entries' counts, +4 of each 12-byte entry, [g+0x60] entries). */
static uint32_t LwGroupSize(uintptr_t g)
{
    uint32_t n = EvpU32(g + 0x60, 0), total = 0;
    for (uint32_t k = 0; k < n && k < 8; k++) total += EvpU32(g + 12u * k + 4, 0);
    return total > 32 ? 32 : total;
}

static uint32_t g_lwRandom = 0x2545F491u;
static uint32_t LwRand(void) { g_lwRandom ^= g_lwRandom << 13; g_lwRandom ^= g_lwRandom >> 17; g_lwRandom ^= g_lwRandom << 5; return g_lwRandom; }
static float LwUnit(void) { return (float)(LwRand() >> 8) / 16777216.0f; }

/* Chooses a group for a squad at pos as the director's patrol pick does (0x959950 / 0x955E10): a category from the
 * director's list (dir+0x51918, count dir+0x51928; vanilla runs one timer per category: equal shares), the main groups
 * of that category with weight +0xC0 > 0, first those whose definition's location list (+0x80, 8 tags) holds none of
 * the zone's tags, else those that hold one; a weighted draw. Its definition (+0xA8), 0 when none. */
static uintptr_t LwChooseGroup(uintptr_t dir, uintptr_t lg, const float* pos)
{
    uint32_t main = EvpU32(dir + 0x5187C, 0), categories = EvpU32(dir + 0x51928, 0), tags[2], ntags;
    if (main == 0 || main > 96 || categories == 0 || categories > 4) return 0;
    uint32_t category = EvpU32(dir + 0x51918 + 4u * (LwRand() % categories), 0);
    float pos3[3] = {pos[0], pos[1], 0};
    ntags = (uint32_t)LwZoneTags(lg, pos3, tags);
    for (int pass = 0; pass < 2; pass++)
    {
        float total = 0, weights[96];
        for (uint32_t i = 0; i < main; i++)
        {
            uintptr_t g = LW_GROUP(dir, i), def = 0;
            float w = 0; weights[i] = 0;
            if (EvpU32(g + 0xCC, 0) != category && category != 0) continue;
            if (SafeRead(g + 0xC0, &w, 4) || !(w > 0) || !EvpPtr(g + 0xA8, &def) || LwGroupSize(g) == 0) continue;
            int hit = 0;
            for (uint32_t t = 0; t < 8 && !hit; t++) { uint32_t tag = EvpU32(def + 0x80 + 4u * t, 0); for (uint32_t z = 0; z < ntags; z++) if (tag != 0 && tag == tags[z]) hit = 1; }
            if (hit != pass) continue;
            weights[i] = w; total += w;
        }
        if (!(total > 0)) continue;
        float r = LwUnit() * total;
        for (uint32_t i = 0; i < main; i++) { if (weights[i] <= 0) continue; r -= weights[i]; if (r <= 0) { uintptr_t def = 0; EvpPtr(LW_GROUP(dir, i) + 0xA8, &def); return def; } }
    }
    return 0;
}

/* The main group slot that now holds a definition (slots shift when a rebuild filters by players), or 0. */
static uintptr_t LwSlotOf(uintptr_t dir, uintptr_t def)
{
    uint32_t main = EvpU32(dir + 0x5187C, 0);
    for (uint32_t i = 0; i < main && i < 96; i++) { uintptr_t d = 0; if (EvpPtr(LW_GROUP(dir, i) + 0xA8, &d) && d == def) return LW_GROUP(dir, i); }
    return 0;
}

/* ---- squads ----------------------------------------------------------------------------------------------------------
 * Design (KB "Phase 1 design"): N = round(patrols x (R_play / R_on)^2) squads, patrols = the mission's effective patrol
 * cap in units (0x959650(dir, the row's saved cap): it counts job-1 units, 0x9513A1) / the mean group size, so the
 * expected number of squads within R_on of a player matches vanilla's. Online within R_on of a player and inside the
 * playable disc (R = u32 [S+0x2C] x f32 [S+0x50] / 2, S = [[lg+0x2A8]+0x19B010]); offline beyond R_off at alert 0. */
#define LW_SQUADS_MAX 6144
#define LW_PATH_MAX 32
#define LW_R_ON 250.0f
#define LW_R_OFF 350.0f
#define LW_SIGHT 600.0f       /* players farther than this are not checked for sight (D) */
#define LW_R_MIN 150.0f       /* never spawned nearer a player (D): vanilla spawns patrols 75-130 m away */
#define LW_ARRIVE 15.0f
#define LW_FAR_MS 10000       /* beyond R_off and calm this long before a squad goes offline (D) */
#define LW_SPEED_DEFAULT 2.0f /* m/s until a squad's own walking speed was seen online (D) */
#define LW_RESERVE 12u        /* the 70 room's share kept for garrison top-ups and SEAF (KB "Spawn gates and the online budget") */
enum { LW_FREE, LW_OFFLINE, LW_PENDING, LW_ONLINE };
typedef struct LwSquad
{
    uint8_t state, survivors, cap, jobSet, pathLen, pathAt;
    uint16_t node, path[LW_PATH_MAX];  /* walking from node to path[pathAt] */
    float pos[2], speed;
    uintptr_t definition, rec;
    uint32_t squad, seen[32];
    uint8_t seenCount, lost, cycled, alive; /* alive: members listed online (the budget counts living units, as 0x9513A1) */
    ULONGLONG since, moved, farSince;
    float movedFrom[2], reqZ, seenAt[32][3]; /* seenAt: each listed member's last position (for the loss note) */
    uint8_t arrived, viewed; /* viewed: the test trace noted it in a player's view once */
    ULONGLONG retry; /* no request before this (placement or sight refused) */
} LwSquad;
static LwSquad g_lwSquads[LW_SQUADS_MAX];
static struct
{
    uintptr_t dir, lg; uint32_t seed;
    int count, populated, online, onlineUnits, eff, playersAt;
    float players[EVT_MAX_PLAYERS][3], radius; /* feet position (x, y, z) */
    uint32_t units[EVT_MAX_PLAYERS], knownUnit[EVT_MAX_PLAYERS]; /* each player's Helldiver unit (the sight ray leaves it out) */
    float known[EVT_MAX_PLAYERS][3]; ULONGLONG knownAt[EVT_MAX_PLAYERS]; uint64_t knownPeer[EVT_MAX_PLAYERS];
    ULONGLONG last, effAt, noteAt;
    uint32_t spawned, offlined, died, refused;
    uint32_t noFace, moved, overhead, seen, rays, blockUnit, blockTerrain; double rayUs; /* placement and sight refusals, rays cast (30 s) */
} g_lwWorld;

static uint32_t LwAiCount(void) { uintptr_t c; return EvpPtr(g_lw.game + 0x3326D18u, &c) ? EvpU32(c + 0x1C, 0) : 0; }
static float LwDist2(const float* a, float bx, float by) { float dx = a[0] - bx, dy = a[1] - by; return dx * dx + dy * dy; }
static float LwNearestPlayer2(float x, float y)
{
    float best = 3.0e38f;
    for (int i = 0; i < g_lwWorld.playersAt; i++) { float d = LwDist2(g_lwWorld.players[i], x, y); if (d < best) best = d; }
    return best;
}

/* The players' positions (their Helldivers' units), into g_lwWorld. A player whose Helldiver does not read for a while
 * (dead and waiting, or dropping in again: lw2 saw squads near a reinforced player taken offline at once) keeps the
 * last position read for LW_KNOWN_MS, so a gap never takes the squads around them offline. */
#define LW_KNOWN_MS 30000
static void LwPlayers(void)
{
    EvpPlayer list[EVT_MAX_PLAYERS];
    int n = EvpPlayers(g_lw.game, list), k = 0;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < n && i < EVT_MAX_PLAYERS; i++)
    {
        ActHealth h; float at[3];
        int slot = -1;
        for (int j = 0; j < EVT_MAX_PLAYERS; j++) if (g_lwWorld.knownPeer[j] == list[i].peer && g_lwWorld.knownAt[j]) slot = j;
        if (list[i].avatar && ActHealthOf(g_lw.game, list[i].avatar, &h) && h.unit && EvpUnitPosition(h.unit, at))
        {
            if (slot < 0) for (int j = 0; j < EVT_MAX_PLAYERS && slot < 0; j++) if (!g_lwWorld.knownAt[j] || now - g_lwWorld.knownAt[j] > LW_KNOWN_MS) slot = j;
            if (slot >= 0) { g_lwWorld.known[slot][0] = at[0]; g_lwWorld.known[slot][1] = at[1]; g_lwWorld.known[slot][2] = at[2]; g_lwWorld.knownUnit[slot] = h.unit; g_lwWorld.knownAt[slot] = now; g_lwWorld.knownPeer[slot] = list[i].peer; }
        }
    }
    for (int j = 0; j < EVT_MAX_PLAYERS; j++)
        if (g_lwWorld.knownAt[j] && now - g_lwWorld.knownAt[j] <= LW_KNOWN_MS) { memcpy(g_lwWorld.players[k], g_lwWorld.known[j], sizeof g_lwWorld.players[k]); g_lwWorld.units[k] = g_lwWorld.knownUnit[j]; k++; }
    g_lwWorld.playersAt = k;
}

/* The playable disc's radius (0: unreadable). */
static float LwPlayRadius(uintptr_t lg)
{
    uintptr_t gen, S; uint32_t size = 0; float fraction = 0;
    if (!EvpPtr(lg + 0x2A8, &gen) || !EvpPtr(gen + 0x19B010, &S) || SafeRead(S + 0x2C, &size, 4) || SafeRead(S + 0x50, &fraction, 4) || !(fraction > 0) || size == 0) return 0;
    return (float)size * fraction * 0.5f;
}

/* The mission's effective patrol cap in units: 0x959650(dir, the faction's saved row cap at the director's level). */
static int LwEffectiveCap(uintptr_t dir)
{
    uint32_t level = EvpU32(dir + 0x518C8, 0) & 0xFF, faction = EvpU32(dir + 0x518BC, 0);
    int k = faction == 2 ? 0 : faction == 8 ? 1 : faction == 4 ? 2 : -1;
    if (level < 1) level = 1;
    if (level > DIF_VANILLA + 1) level = DIF_VANILLA + 1;
    if (k < 0 || !g_lw.savedOk[level - 1]) return 0;
    typedef uint32_t (*EffFn)(uintptr_t dir, uint32_t cap);
    return (int)((EffFn)(g_lw.game + 0x959650u))(dir, g_lw.saved[level - 1][k]);
}

static int LwNearestNode(float x, float y)
{
    int best = -1; float bd = 3.0e38f;
    for (int i = 0; i < g_lwNodeCount; i++) { float dx = g_lwNodes[i].x - x, dy = g_lwNodes[i].y - y, d = dx * dx + dy * dy; if (d < bd) { bd = d; best = i; } }
    return best;
}

/* A new destination: a random node inside the playable disc, and the path there. */
static void LwPlan(LwSquad* q)
{
    for (int tries = 0; tries < 8; tries++)
    {
        int goal = (int)(LwRand() % (uint32_t)g_lwNodeCount);
        float r = g_lwWorld.radius;
        if (g_lwNodes[goal].x * g_lwNodes[goal].x + g_lwNodes[goal].y * g_lwNodes[goal].y > r * r) continue;
        int n = LwPath(q->node, goal, q->path, LW_PATH_MAX);
        if (n > 0) { q->pathLen = (uint8_t)n; q->pathAt = 0; return; }
    }
    q->pathLen = 0; q->pathAt = 0;
}

/* The world's squads for this mission: on route nodes inside the playable disc, away from the players. */
static void LwPopulate(void)
{
    uintptr_t dir = g_lwWorld.dir;
    uint32_t main = EvpU32(dir + 0x5187C, 0);
    float sum = 0, weight = 0;
    for (uint32_t i = 0; i < main && i < 96; i++) { float w = 0; if (!SafeRead(LW_GROUP(dir, i) + 0xC0, &w, 4) && w > 0) { sum += w * (float)LwGroupSize(LW_GROUP(dir, i)); weight += w; } }
    int eff = LwEffectiveCap(dir);
    if (g_lwWorld.radius <= 0) g_lwWorld.radius = LwPlayRadius(g_lwWorld.lg);
    if (g_lwWorld.radius <= 0) { LwWhy("no playable radius yet"); return; } /* read again next tick (audit 1: never give up on a 0 read) */
    if (!(weight > 0) || eff <= 0)
    {
        Note("living world: no squads this mission (the director builds no patrol groups here, or the patrol cap is 0: vanilla has no patrols either)");
        g_lwWorld.populated = -1;
        return;
    }
    float mean = sum / weight, patrols = (float)eff / mean, ratio = g_lwWorld.radius / LW_R_ON;
    int n = (int)(patrols * ratio * ratio + 0.5f);
    if (n < 1) n = 1;
    if (n > LW_SQUADS_MAX) n = LW_SQUADS_MAX;
    int made = 0;
    /* Start beyond R_off from every player; on a map whose playable disc is smaller than that (lw7b: 292 m, no node
     * 350 m from the drop) beyond R_min instead, so such squads come online at once at vanilla-like distances. */
    float least = g_lwWorld.radius < LW_R_OFF ? LW_R_MIN : LW_R_OFF;
    for (int tries = 0; made < n && tries < n * 20; tries++)
    {
        int node = (int)(LwRand() % (uint32_t)g_lwNodeCount);
        float x = g_lwNodes[node].x, y = g_lwNodes[node].y, r = g_lwWorld.radius;
        if (x * x + y * y > r * r || LwNearestPlayer2(x, y) < least * least || g_lwNodes[node].degree == 0) continue;
        float pos[2] = {x, y};
        uintptr_t def = LwChooseGroup(dir, g_lwWorld.lg, pos);
        uintptr_t slot = def ? LwSlotOf(dir, def) : 0;
        if (slot == 0) continue;
        LwSquad* q = &g_lwSquads[made++];
        memset(q, 0, sizeof *q);
        q->state = LW_OFFLINE; q->node = (uint16_t)node; q->pos[0] = x; q->pos[1] = y; q->definition = def;
        q->survivors = (uint8_t)LwGroupSize(slot); q->speed = LW_SPEED_DEFAULT * (0.75f + 0.5f * LwUnit()); /* own pace: no lockstep (audit 1, 11) */
        LwPlan(q);
    }
    g_lwWorld.count = made; g_lwWorld.eff = eff; g_lwWorld.populated = 1;
    Note("living world: %d squads on %d route nodes (patrol cap %d units, mean group %.1f, playable radius %.0f m, online within %.0f m)", made, g_lwNodeCount, eff,
        (double)mean, (double)g_lwWorld.radius, (double)LW_R_ON);
}

/* Offline walking along the path at the squad's speed; a new destination at the end. */
static void LwWalk(LwSquad* q, float dt)
{
    if (q->pathAt >= q->pathLen) { LwPlan(q); if (q->pathLen == 0) return; }
    float step = q->speed * dt;
    while (step > 0 && q->pathAt < q->pathLen)
    {
        LwNode* t = &g_lwNodes[q->path[q->pathAt]];
        float dx = t->x - q->pos[0], dy = t->y - q->pos[1], d = sqrtf(dx * dx + dy * dy);
        if (d <= step) { q->pos[0] = t->x; q->pos[1] = t->y; q->node = q->path[q->pathAt++]; step -= d; }
        else { q->pos[0] += dx / d * step; q->pos[1] += dy / d * step; step = 0; }
    }
}

/* The spawn gates the loader enforces itself (flag 0x40 skips 0x952EB0): a player record with bits 4 and 11 clear, the
 * mission not ending (0x8F32B0), the 70 room ([[game+0x3326D18]+0x1C] + [dir+0x36C0] - [dir+0x620] - [dir+0x61C], as
 * 0x952F3C) with the reserve, AiJob active + members + 1 below 448 - 32, the effective cap and the online ceiling. */
static int LwRoomFor(uint32_t members)
{
    uintptr_t players, chars, jobs;
    if (!EvpPtr(g_lw.game + 0x3326468u, &players)) return 0;
    uint32_t count = EvpU32(players + 0x84, 0), ok = 0;
    for (uint32_t i = 0; i < count && i < 16; i++) { uint32_t f = EvpU32(players + 0x3AC + 0x20u * i, 0xFFFFFFFFu); if (!(f & (1u << 11)) && !(f & (1u << 4))) ok = 1; }
    typedef uint8_t (*EndingFn)(void);
    if (!ok || ((EndingFn)(g_lw.game + 0x8F32B0u))()) return 0;
    uintptr_t dir = g_lwWorld.dir;
    if (!EvpPtr(g_lw.game + 0x3326D18u, &chars) || !EvpPtr(g_lw.game + LW_AIJOB, &jobs)) return 0;
    int room = (int)EvpU32(chars + 0x1C, 999) + (int)EvpU32(dir + 0x36C0, 999) - (int)EvpU32(dir + 0x620, 0) - (int)EvpU32(dir + 0x61C, 0);
    if (room + (int)members + (int)LW_RESERVE > 70) return 0;
    if (EvpU32(jobs + 0xC, 9999) + members + 1 >= 448 - 32) return 0;
    int ceiling = g_lwWorld.playersAt > 1 ? 64 : 48;
    if (g_lwWorld.onlineUnits + (int)members > g_lwWorld.eff || g_lwWorld.onlineUnits + (int)members > ceiling) return 0;
    return 1;
}

static int LwGround(float x, float y, float* z);
static int LwPlace(uintptr_t slot, float* pos);
static int LwHidden(const float* p);

/* Brings a squad online: its group's slot (found again by definition), the request at its position, heading for its
 * next node. The loader places the point itself (LwPlace: on the navmesh, nothing overhead) and spawns only out of
 * every player's sight (LwHidden), so the request skips both of the game's snaps, as vanilla's patrols (0x5C) do:
 * flags 0x02 keep | 0x04 queue | 0x08 no nav snap | 0x10 no ground ray | 0x40 the gates above (KB "Spawn placement
 * and sight"). A refused point is tried again 3 s later (the squad walks on offline). */
#define LW_RECENT 16
static struct { float p[LW_RECENT][2]; ULONGLONG at[LW_RECENT]; int next; } g_lwRecent;
static void LwRecentAdd(const float* p) { int k = g_lwRecent.next++ % LW_RECENT; g_lwRecent.p[k][0] = p[0]; g_lwRecent.p[k][1] = p[1]; g_lwRecent.at[k] = GetTickCount64(); }
/* Spacing (D; audit 1 criterion 11, lw16i: two squads came online at one spot 14 s apart): no squad comes online within
 * LW_SPACING of a squad that is online or pending (its live position) or of a request in the last 60 s. */
#define LW_SPACING 60.0f
static int LwRecentNear(const float* p)
{
    ULONGLONG now = GetTickCount64();
    for (int k = 0; k < LW_RECENT; k++) if (g_lwRecent.at[k] && now - g_lwRecent.at[k] < 60000 && LwDist2(g_lwRecent.p[k], p[0], p[1]) < LW_SPACING * LW_SPACING) return 1;
    for (int i = 0; i < g_lwWorld.count; i++)
        if ((g_lwSquads[i].state == LW_ONLINE || g_lwSquads[i].state == LW_PENDING) && LwDist2(g_lwSquads[i].pos, p[0], p[1]) < LW_SPACING * LW_SPACING) return 1;
    return 0;
}

static void LwOnline(LwSquad* q)
{
    uintptr_t slot = LwSlotOf(g_lwWorld.dir, q->definition);
    if (slot == 0)
    {
        q->definition = LwChooseGroup(g_lwWorld.dir, g_lwWorld.lg, q->pos);
        slot = q->definition ? LwSlotOf(g_lwWorld.dir, q->definition) : 0;
        if (slot == 0) return;
    }
    uint32_t cap = q->survivors, size = LwGroupSize(slot);
    if (cap > size) cap = size;
    if (cap == 0 || !LwRoomFor(cap)) { g_lwWorld.refused++; return; }
    float pos[3] = {q->pos[0], q->pos[1], 0}, target[3] = {q->pos[0], q->pos[1], 0};
    if (q->pathAt < q->pathLen) { target[0] = g_lwNodes[q->path[q->pathAt]].x; target[1] = g_lwNodes[q->path[q->pathAt]].y; }
    if (!LwGround(pos[0], pos[1], &pos[2]) || !LwGround(target[0], target[1], &target[2])) { g_lwWorld.refused++; return; }
    float wanted[2] = {pos[0], pos[1]};
    if (!LwPlace(slot, pos) || !LwHidden(pos)) { q->retry = GetTickCount64() + 3000; return; }
    uintptr_t rec = g_lw.spawn(g_lwWorld.dir, slot, pos, LW_JOB_PATROL, LW_PATROL_FLAGS, target, NULL, NULL, 0.0f, cap);
    if (rec == 0) { g_lwWorld.refused++; return; }
    uint8_t used = 0;
    if (SafeRead(rec + 0x12, &used, 1) || used != 1) { g_lwWorld.refused++; return; } /* the queue was full: nothing queued */
    Note("living world: squad %d requested at (%.0f, %.0f, %.1f) (snapped %.1f m), %.0f m from the nearest player, out of sight, %u of %u members (group 0x%llx)",
        (int)(q - g_lwSquads), (double)pos[0], (double)pos[1], (double)pos[2], (double)sqrtf(LwDist2(wanted, pos[0], pos[1])),
        (double)sqrtf(LwNearestPlayer2(pos[0], pos[1])), cap, size, (unsigned long long)q->definition);
    q->reqZ = pos[2]; q->arrived = 0; q->viewed = 0;
    q->state = LW_PENDING; q->rec = rec; q->cap = (uint8_t)cap; q->since = GetTickCount64(); q->jobSet = 0; q->farSince = 0; q->seenCount = 0; q->lost = 0; q->squad = 0;
    g_lwWorld.onlineUnits += (int)cap; q->alive = (uint8_t)cap;
    LwRecentAdd(pos);
    g_lwWorld.spawned++;
}

/* The leader's (members[0]) position, 0 when it does not read. */
static int LwLeader(const uint32_t* members, int n, float* at)
{
    for (int i = 0; i < n; i++) { ActHealth h; float p[3]; if (ActHealthOf(g_lw.game, members[i], &h) && h.unit && EvpUnitPosition(h.unit, p)) { at[0] = p[0]; at[1] = p[1]; return 1; } }
    return 0;
}

/* A unit's alert level (AiAlert [game+0x3326528]: hash +0x20, states [+0x50] + 4 idx); 0 when it has none. */
static uint32_t LwAlert(uint32_t unit)
{
    uintptr_t m, states; uint32_t index = 0, invalid = 0;
    if (!EvpPtr(g_lw.game + 0x3326528u, &m) || SafeRead(g_lw.game + LW_INVALID, &invalid, 4) || unit == invalid || !EvpHash(m + 0x20, unit, 8, 4, &index) ||
        index >= EvpU32(m + 0xC, 0) || !EvpPtr(m + 0x50, &states)) return 0;
    return EvpU32(states + 4u * index, 0);
}

/* The ground's height at (x, y): the game's ground ray (0x13CA380, as the spawn request's ground snap calls it: from
 * z + 500 m down 1000 m; z written only on a hit). 1 with *z. */
static int LwGround(float x, float y, float* z)
{
    typedef uint8_t (*GroundFn)(uintptr_t world, const float* in, float* outZ, uint8_t reject, float* normal, uint8_t mode);
    uintptr_t world;
    if (!EvpPtr(g_lw.game + 0x346D508u, &world)) return 0;
    float in[3] = {x, y, 0}, out = NAN;
    ((GroundFn)(g_lw.game + 0x13CA380u))(world, in, &out, 0, NULL, 0);
    if (!isfinite(out)) return 0;
    *z = out;
    return 1;
}

/* Placement (audit 1, criterion 4; KB "Spawn placement and sight", checked in the dump: 0x95374C..0x9537E8). The
 * game's own nav snap, called as the spawn request calls it when 0x08 is clear: [[game+0x3326330]+0x10] = exe
 * 0x7D89F0(u32 nav id [dir+0x648], float p[3] in and out (one buffer), 10.0f (0x23C7554), NULL, u32 layer =
 * [0x505B40([slot+0xB0]) + 4] or 0); 0 = no face within 10 m. Refused when there is no face, when the face point is more
 * than 2 m from the route point across, or when the ground ray (the topmost surface) is more than 2 m off the face's
 * height: something stands over the navmesh there (a roof, an overhang). 1 with pos at the face point. */
static int LwPlace(uintptr_t slot, float* pos)
{
    typedef int (*SnapFn)(uint32_t nav, float* p, float radius, float* out, float* normal, uint32_t layer);
    typedef uintptr_t (*LayerFn)(uintptr_t key);
    uintptr_t table = 0, fn = 0, key = 0, entry;
    uint32_t layer = 0, nav = EvpU32(g_lwWorld.dir + 0x648, 0);
    if (!EvpPtr(g_lw.game + 0x3326330u, &table) || !EvpPtr(table + 0x10, &fn) || fn == 0) { g_lwWorld.noFace++; return 0; }
    if (EvpPtr(slot + 0xB0, &key) && key && (entry = ((LayerFn)(g_lw.game + 0x505B40u))(key)) != 0) layer = EvpU32(entry + 4, 0);
    float p[3] = {pos[0], pos[1], pos[2]}, top = NAN;
    if (((SnapFn)fn)(nav, p, 10.0f, p, NULL, layer) == 0 || !isfinite(p[2])) { g_lwWorld.noFace++; return 0; }
    if (LwDist2(pos, p[0], p[1]) > 2.0f * 2.0f) { g_lwWorld.moved++; return 0; }
    if (!LwGround(p[0], p[1], &top) || fabsf(top - p[2]) > 2.0f) { g_lwWorld.overhead++; return 0; }
    memcpy(pos, p, sizeof p);
    return 1;
}

/* Sight (audit 1, criterion 5; KB living-world-plan "After audit 1", subagent rayhit; checked by me: make_raycast exe
 * 0x77DE60, its release 0x77E030, the game's own handles 0x175BE60..). The game's ray [[game+0x3326328]+0x68] = exe
 * 0x77DEC0(u32 handle, from[3], unit dir[3], length, u32 unit to leave out (the query filter 0x7F1660 rejects every
 * body whose userData high dword is that unit), hit (0x2C bytes: +0x18 metres along the ray, +0x1C the owning unit, 0
 * for terrain), u32 max) -> hits. The loader makes its own handle per physics world, as 0x175BE60 makes the AI's
 * damage_los_query (hash 0x2315515D, mode 1, flags 0x80000009), but with types 3 = static bodies only (filter 0x7F17C4:
 * characters, ragdolls, debris and vehicles are not seen): make_raycast [+0x58](world index [+0]([T]) (exe 0x79F860, 4 =
 * none), 1, 3, 0x2315515D, 0x80000009), T = [[game+0x346D508]+8]; released with [+0x60] when the world changes (it only
 * returns the entry to the free list). A point is seen when the ray from the player's eye (feet + 1.7 m), leaving out
 * that player's unit, meets nothing static 0.5 m short of it. Players beyond LW_SIGHT are not checked (D). Every ray is
 * timed. 1 seen, 0 hidden, -1 no ray. */
static uint32_t g_lwSightHandle; static uintptr_t g_lwSightWorld;
/* At each level change: the handle goes back to the game's free list (exe 0x77E030 only pushes the entry). */
static void LwSightRelease(void)
{
    typedef void (*FreeFn)(uint32_t handle);
    uintptr_t table = 0, fnFree = 0;
    if (g_lwSightHandle && EvpPtr(g_lw.game + 0x3326328u, &table) && table && EvpPtr(table + 0x60, &fnFree) && fnFree) ((FreeFn)fnFree)(g_lwSightHandle);
    g_lwSightHandle = 0; g_lwSightWorld = 0;
}
static uint32_t LwSightRay(uintptr_t* table)
{
    typedef int (*WorldFn)(uintptr_t physics);
    typedef uint32_t (*MakeFn)(int world, uint32_t mode, uint32_t types, uint32_t hash, uint32_t flags);
    typedef void (*FreeFn)(uint32_t handle);
    uint32_t* const h = &g_lwSightHandle; uintptr_t* const w = &g_lwSightWorld;
    uintptr_t fnWorld = 0, fnMake = 0, fnFree = 0, wrapper = 0, t = 0, physics = 0;
    if (!EvpPtr(g_lw.game + 0x3326328u, table) || *table == 0 || !EvpPtr(*table, &fnWorld) || !EvpPtr(*table + 0x58, &fnMake) ||
        !EvpPtr(*table + 0x60, &fnFree) || !fnWorld || !fnMake || !fnFree || !EvpPtr(g_lw.game + 0x346D508u, &wrapper) || !EvpPtr(wrapper + 8, &t) ||
        !EvpPtr(t, &physics) || physics == 0) return 0;
    if (physics == *w && *h) return *h;
    if (*h) { ((FreeFn)fnFree)(*h); *h = 0; }
    *w = physics;
    uint64_t free = 0; /* make_raycast pops its free list unchecked (0x77DE67) */
    if (SafeRead((uintptr_t)GetModuleHandleW(NULL) + 0x27B9438u, &free, 8) || free == 0) return 0;
    int index = ((WorldFn)fnWorld)(physics);
    if (index < 0 || index >= 4) return 0;
    *h = ((MakeFn)fnMake)(index, 1, 3, 0x2315515Du, 0x80000009u);
    Note("living world: sight: a static-only line-of-sight ray for physics world %d (handle %08X)", index, *h);
    return *h;
}
static int LwSeenFrom(const float* feet, uint32_t unit, const float* point)
{
    typedef uint32_t (*RayFn)(uint32_t handle, const float* from, const float* dir, float length, uint32_t leaveOut, void* hit, uint32_t max);
    uintptr_t table = 0, fn = 0;
    uint32_t handle = LwSightRay(&table);
    if (handle == 0 || !EvpPtr(table + 0x68, &fn) || fn == 0) return -1;
    float eye[3] = {feet[0], feet[1], feet[2] + 1.7f}, d[3] = {point[0] - eye[0], point[1] - eye[1], point[2] - eye[2]};
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1.0f) return 1;
    for (int k = 0; k < 3; k++) d[k] /= len;
    uint8_t hit[0x40] = {0};
    LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a);
    uint32_t n = ((RayFn)fn)(handle, eye, d, len, unit, hit, 1);
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    g_lwWorld.rays++; g_lwWorld.rayUs += (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart;
    float dist; uint32_t owner; memcpy(&dist, hit + 0x18, 4); memcpy(&owner, hit + 0x1C, 4);
    int blocked = n >= 1 && dist < len - 0.5f;
    if (blocked) { if (owner) g_lwWorld.blockUnit++; else g_lwWorld.blockTerrain++; }
    return !blocked;
}
/* Out of sight: from every player within LW_SIGHT, both a unit's chest (1.0 m) and head-over height (2.5 m) blocked. */
static int LwHidden(const float* p)
{
    for (int i = 0; i < g_lwWorld.playersAt; i++)
    {
        if (LwDist2(g_lwWorld.players[i], p[0], p[1]) > LW_SIGHT * LW_SIGHT) continue;
        for (int h = 0; h < 2; h++)
        {
            float t[3] = {p[0], p[1], p[2] + (h ? 2.5f : 1.0f)};
            int seen = LwSeenFrom(g_lwWorld.players[i], g_lwWorld.units[i], t);
            if (seen != 0) { g_lwWorld.seen++; return 0; } /* seen, or no ray to tell: refused */
        }
    }
    return 1;
}

static void LwSendGoal(LwSquad* q)
{
    if (q->pathAt >= q->pathLen) LwPlan(q);
    if (q->pathAt >= q->pathLen || LwJobRecord(q->squad) == 0 || LwSquadRecord(q->squad) == 0) return;
    float goal[3] = {g_lwNodes[q->path[q->pathAt]].x, g_lwNodes[q->path[q->pathAt]].y, 0};
    if (!LwGround(goal[0], goal[1], &goal[2])) return; /* the goal's face search needs its height (40 m sphere, 100 m ray up) */
    g_lw.goal(q->squad, goal);
}

/* An online squad: its members kept, losses counted, steered node to node; offline again beyond R_off at alert 0. */
/* Why a member left its squad's list (KB living-world-plan "Member losses", P): only RemoveMember 0x804A00 takes one
 * out, from the death link (0xB48160), the destroy link (0xB4BC28) or a behaviour's leave (0x4D04C0, the unit lives on);
 * removals are flushed before the late tick, so here a removed member has no entity and a killed one keeps its record
 * with life >= 2 (0x927050); a member that left alive has AiSquadMember ([game+0x33264C8], hash +0x18, {squad, link}
 * at [+0x38] + 8 idx) cleared. Read in the first tick after the loss (entity ids are reused). */
/* For a kill, who (subagent killer; checked by me at 0x923BFE..0x923C34): the damage apply 0x9235F0 writes, on every
 * hit, health record +0x30 = the attacker entity (net8 -> 0xFD9BA0) and +0x38 = the creditor (a session peer id, kept
 * from the hit before when 0; 0 = no player). The attacker is a Helldiver when it is in the avatar hash ([game+0x3326D20]
 * +0xF8, as 0x925524). */
static int LwGarrisonUnitsAt(uint32_t unit, uint32_t* net, uint32_t* slot);
static const char* LwLostWhy(uint32_t unit, uint32_t* squad, char* by, size_t size)
{
    ActHealth h; uintptr_t m, links, av; uint32_t index;
    *squad = 0; by[0] = 0;
    if (!EvpAlive(unit)) return "removed (no entity)";
    if (ActHealthOf(g_lw.game, unit, &h) && h.life >= 2)
    {
        uint32_t attacker = EvpU32(h.record + 0x30, 0), slot; uint64_t creditor = 0;
        SafeRead(h.record + 0x38, &creditor, 8);
        int helldiver = attacker && EvpPtr(g_lw.game + 0x3326D20u, &av) && EvpHash(av + 0xF8, attacker, 8, 4, &slot);
        uint32_t net = 0, point = 0;
        int enemy = !helldiver && attacker && attacker != unit && LwGarrisonUnitsAt(attacker, &net, &point) == 1;
        char what[64]; int of = -1;
        for (int k = 0; k < g_lwWorld.count && of < 0; k++)
            for (int m = 0; m < g_lwSquads[k].seenCount && of < 0; m++) if (g_lwSquads[k].seen[m] == attacker) of = k;
        if (of >= 0) snprintf(what, sizeof what, "a member of squad %d", of);
        else if (enemy && net && net != 0x7FFF) snprintf(what, sizeof what, "an enemy of garrison net %08X", net);
        else snprintf(what, sizeof what, "%s", enemy ? "an enemy in no garrison" : "entity");
        snprintf(by, size, " by %s %08X%s", helldiver ? "a Helldiver" : attacker == unit ? "itself" : attacker == 0 || attacker == 0x7FFF ? "nothing named" : what,
            attacker, creditor ? ", credited to a player" : ", no player credited");
        return "killed";
    }
    if (EvpPtr(g_lw.game + 0x33264C8u, &m) && EvpHash(m + 0x18, unit, 8, 4, &index) && EvpPtr(m + 0x38, &links)) *squad = EvpU32(links + 8u * index, 0);
    return "left its squad alive";
}

static int LwSeenFrom(const float* feet, uint32_t unit, const float* point);

static void LwOnlineTick(LwSquad* q, int* removals)
{
    uint32_t members[32];
    int n = LwMembers(q->squad, members);
    if (n < 0) /* the squad entity is gone: it empties itself when its last member leaves */
    {
        Note("living world: squad %d (%08X) is gone: all %u members lost", (int)(q - g_lwSquads), q->squad, q->cap);
        q->state = LW_FREE; g_lwWorld.died++; g_lwWorld.onlineUnits -= q->alive;
        return;
    }
    for (int i = 0; i < q->seenCount; i++) /* a member that left the list is lost (killed or destroyed) */
    {
        int still = 0;
        for (int k = 0; k < n && !still; k++) still = members[k] == q->seen[i];
        if (!still)
        {
            uint32_t other = 0; float ground = NAN, *at = q->seenAt[i]; char by[128];
            const char* why = LwLostWhy(q->seen[i], &other, by, sizeof by);
            LwGround(at[0], at[1], &ground);
            Note("living world: squad %d lost member %08X: %s%s (squad now %08X); last seen at (%.0f, %.0f, %.1f), ground there %.1f, %.0f m from the nearest player",
                (int)(q - g_lwSquads), q->seen[i], why, by, other, (double)at[0], (double)at[1], (double)at[2], (double)ground,
                (double)sqrtf(LwNearestPlayer2(at[0], at[1])));
            --q->seenCount; q->seen[i] = q->seen[q->seenCount]; memcpy(q->seenAt[i], q->seenAt[q->seenCount], sizeof q->seenAt[i]); i--; q->lost++;
        }
    }
    for (int k = 0; k < n; k++)
    {
        LwKeep(members[k]);
        int known = -1;
        for (int i = 0; i < q->seenCount && known < 0; i++) if (q->seen[i] == members[k]) known = i;
        if (known < 0 && q->seenCount < 32) { known = q->seenCount++; q->seen[known] = members[k]; }
        ActHealth h; float p[3];
        if (known >= 0 && ActHealthOf(g_lw.game, members[k], &h) && h.unit && EvpUnitPosition(h.unit, p)) memcpy(q->seenAt[known], p, sizeof p);
    }
    if (!q->arrived && n > 0) /* the members' first positions: their height against the request, and whether a player sees them */
    {
        float lo = 1e9f, hi = -1e9f; int seen = 0, read = 0;
        for (int k = 0; k < n; k++)
        {
            ActHealth h; float p[3];
            if (!ActHealthOf(g_lw.game, members[k], &h) || !h.unit || !EvpUnitPosition(h.unit, p)) continue;
            read++; if (p[2] < lo) lo = p[2]; if (p[2] > hi) hi = p[2];
            float chest[3] = {p[0], p[1], p[2] + 1.0f};
            for (int i = 0; i < g_lwWorld.playersAt; i++)
                if (LwDist2(g_lwWorld.players[i], p[0], p[1]) <= LW_SIGHT * LW_SIGHT && LwSeenFrom(g_lwWorld.players[i], g_lwWorld.units[i], chest) == 1) { seen++; break; }
        }
        if (read > 0)
        {
            q->arrived = 1;
            Note("living world: squad %d members appear: %d read, z %.1f .. %.1f (requested %.1f), %d of them in a player's sight",
                (int)(q - g_lwSquads), read, (double)lo, (double)hi, (double)q->reqZ, seen);
        }
    }
    q->survivors = (uint8_t)(q->cap > q->lost ? q->cap - q->lost : 0);
    if (n == 0) return; /* members still queued */
    q->alive = (uint8_t)n;
    if (q->cycled == 2 && q->since && GetTickCount64() - q->since > 10000)
    { q->cycled = 3; Note("living world: test: squad %d back online with %d members (requested %u, %u lost since)", (int)(q - g_lwSquads), n, q->cap, q->lost); }
    /* test instrument (HELLFORGE_LW_TEST_LOSS=1): one member of each of the first three squads with two or more is taken
     * out (the queued removal the director's cull uses) 20 s after it came online; its loss must carry over offline */
    static int testLoss = -1, testDone;
    if (testLoss < 0) { char t[4] = ""; testLoss = GetEnvironmentVariableA("HELLFORGE_LW_TEST_LOSS", t, sizeof t) > 0 && t[0] == '1'; }
    if (testLoss && testDone < 3 && n >= 2 && q->lost == 0 && q->since && GetTickCount64() - q->since > 20000)
    {
        uintptr_t state;
        if (EvpPtr(g_lw.game + LW_STATE, &state)) { g_lw.remove(state, members[n - 1]); testDone++;
            Note("living world: test: squad %d loses member %08X (of %d)", (int)(q - g_lwSquads), members[n - 1], n); }
    }
    if (!q->jobSet && LwJobRecord(q->squad) != 0 && LwSquadRecord(q->squad) != 0) { uintptr_t jobs; if (EvpPtr(g_lw.game + LW_AIJOB, &jobs)) { g_lw.setJob(jobs, q->squad, LW_JOB_BATTLE); q->jobSet = 1; LwSendGoal(q); } }
    float at[2];
    if (!LwLeader(members, n, at)) return;
    ULONGLONG now = GetTickCount64();
    if (q->moved == 0) { q->moved = now; q->movedFrom[0] = at[0]; q->movedFrom[1] = at[1]; }
    else if (now - q->moved >= 5000) /* its walking speed, from the leader's way over 5 s */
    {
        float v = sqrtf(LwDist2(q->movedFrom, at[0], at[1])) / ((float)(now - q->moved) / 1000.0f);
        if (v > 0.3f && v < 12.0f)
        {
            q->speed = q->speed * 0.5f + v * 0.5f;
            Note("living world: squad %d walks %.2f m/s (its speed now %.2f m/s); %d members, %u lost", (int)(q - g_lwSquads), (double)v, (double)q->speed, n, q->lost);
        }
        q->moved = now; q->movedFrom[0] = at[0]; q->movedFrom[1] = at[1];
    }
    q->pos[0] = at[0]; q->pos[1] = at[1];
    /* test instrument (HELLFORGE_LW_TEST_TRACE=1): once per squad, when its leader is within 120 m of a player and that
     * player's eye sees its chest (the sight ray), a note for a screenshot aimed at it */
    static int testView = -1;
    if (testView < 0) { char t[4] = ""; testView = GetEnvironmentVariableA("HELLFORGE_LW_TEST_TRACE", t, sizeof t) > 0 && t[0] == '1'; }
    if (testView && !q->viewed && (now / 500) % 4 == (ULONGLONG)(q - g_lwSquads) % 4)
        for (int i = 0; i < g_lwWorld.playersAt && !q->viewed; i++)
        {
            float lead[3]; ActHealth h;
            if (LwDist2(g_lwWorld.players[i], at[0], at[1]) > 120.0f * 120.0f || !ActHealthOf(g_lw.game, members[0], &h) || !h.unit || !EvpUnitPosition(h.unit, lead)) continue;
            float chest[3] = {lead[0], lead[1], lead[2] + 1.0f};
            if (LwSeenFrom(g_lwWorld.players[i], g_lwWorld.units[i], chest) == 1)
            {
                q->viewed = 1;
                Note("living world: test: squad %d in view at (%.0f, %.0f, %.1f), %.0f m from player %d, %d members, %u lost", (int)(q - g_lwSquads), (double)lead[0],
                    (double)lead[1], (double)lead[2], (double)sqrtf(LwDist2(g_lwWorld.players[i], lead[0], lead[1])), i + 1, n, q->lost);
            }
        }
    if (q->pathAt < q->pathLen && LwDist2(at, g_lwNodes[q->path[q->pathAt]].x, g_lwNodes[q->path[q->pathAt]].y) < LW_ARRIVE * LW_ARRIVE)
    {
        q->node = q->path[q->pathAt++];
        if (q->jobSet) LwSendGoal(q);
        Note("living world: squad %d reached node %u at (%.0f, %.0f); next %s", (int)(q - g_lwSquads), q->node, (double)at[0], (double)at[1],
            q->pathAt < q->pathLen ? "node on its route" : "a new destination");
    }
    /* test instrument (HELLFORGE_LW_TEST_CYCLE=1): a squad with losses is sent offline once, 45 s after it came online,
     * through the same path as the distance rule (which runs confirmed it), so its return shows the survivor count */
    static int testCycle = -1;
    if (testCycle < 0) { char t[4] = ""; testCycle = GetEnvironmentVariableA("HELLFORGE_LW_TEST_CYCLE", t, sizeof t) > 0 && t[0] == '1'; }
    int forced = testCycle && q->lost > 0 && !q->cycled && now - q->since > 45000 && *removals < 16;
    if (forced) { q->cycled = 1; Note("living world: test: squad %d sent offline with %u of %u alive", (int)(q - g_lwSquads), q->survivors, q->cap); }
    else
    {
        /* offline only once it has been beyond R_off of every player, every member at alert 0, for LW_FAR_MS without a break */
        int calm = g_lwWorld.playersAt > 0 && LwNearestPlayer2(at[0], at[1]) > LW_R_OFF * LW_R_OFF;
        for (int k = 0; k < n && calm; k++) if (LwAlert(members[k]) != 0) calm = 0;
        if (!calm) { q->farSince = 0; return; }
        if (q->farSince == 0) q->farSince = now;
        if (now - q->farSince < LW_FAR_MS || *removals >= 16) return;
    }
    Note("living world: squad %d (%08X) goes offline at (%.0f, %.0f), %.0f m from the nearest player, %u of %u alive", (int)(q - g_lwSquads), q->squad,
        (double)at[0], (double)at[1], (double)sqrtf(LwNearestPlayer2(at[0], at[1])), q->survivors, q->cap);
    LwRemoveSquad(q->squad);
    (*removals)++;
    q->state = LW_OFFLINE; q->squad = 0; g_lwWorld.onlineUnits -= q->alive; g_lwWorld.offlined++;
    int node = LwNearestNode(at[0], at[1]);
    if (node >= 0 && q->pathAt >= q->pathLen) { q->node = (uint16_t)node; LwPlan(q); }
    if (q->survivors == 0) q->state = LW_FREE;
}

/* A requested squad: online once the director dispatched it (ring rec+8 = the squad id). */
static void LwPendingTick(LwSquad* q)
{
    uint32_t none = 0, id = 0; uint8_t used = 0;
    if (SafeRead(g_lw.game + LW_NONE, &none, 4) || SafeRead(q->rec + 0x12, &used, 1) || SafeRead(q->rec + 8, &id, 4)) return;
    if (used == 1 && id != none && id != 0)
    {
        Note("living world: squad %d is online as %08X after %.1f s", (int)(q - g_lwSquads), id, (double)(GetTickCount64() - q->since) / 1000.0);
        q->squad = id; q->state = LW_ONLINE; q->rec = 0; q->moved = 0;
        /* keep at once (audit 1, criterion 6): the dispatch wrote rec+8 inside the director (0x954F4C), and the members
         * that exist by now must not meet the next despawner pass (0x572C69) with keep 0 */
        uint32_t members[32];
        int n = LwMembers(id, members);
        for (int k = 0; k < n; k++) LwKeep(members[k]);
        return;
    }
    if (used == 0) { q->state = LW_OFFLINE; g_lwWorld.onlineUnits -= q->cap; return; } /* dropped: its group was rebuilt away */
    if (GetTickCount64() - q->since > 60000 && q->since != 0)
    { Note("living world: a squad request was not dispatched in 60 s (ring record 0x%llx); it stays counted", (unsigned long long)q->rec); q->since = 0; }
}

/* Measurement (what statics cannot give): the share of steering wall requests that found no wall segment, counted by a
 * stub at the game's reader 0x134CB8B (LwInstallWallReach); the queue itself ([game+0x347CD70], count +0) is already
 * empty again by the late tick. */
static volatile float* g_lwWallReach;
static volatile LONG* g_lwWallCounts; /* [0] requests read by the steering consumer, [1] those with no wall segment */
static struct { uint32_t requests, empty, ticks; } g_lwWalls;
static void LwWallSample(void)
{
    if (g_lwWallCounts == NULL) return;
    g_lwWalls.requests = (uint32_t)InterlockedExchange(&g_lwWallCounts[0], 0) + g_lwWalls.requests;
    g_lwWalls.empty = (uint32_t)InterlockedExchange(&g_lwWallCounts[1], 0) + g_lwWalls.empty;
}

static void LwSquadsTick(void)
{
    uintptr_t dir, lg = LwLevel();
    uint32_t seed = 0, levelState = 0;
    if (lg == 0 || !EvpPtr(g_lw.game + LW_DIRECTOR, &dir) || SafeRead(lg + 0x1E4, &levelState, 4) || SafeRead(lg + 0x8, &seed, 4)) { LwWhy("no level or director"); return; }
    if (levelState != 3) { LwWhy("the level is still generating"); return; }
    if (dir != g_lwWorld.dir || lg != g_lwWorld.lg || seed != g_lwWorld.seed) /* a new mission */
    {
        LwSightRelease();
        memset(&g_lwWorld, 0, sizeof g_lwWorld);
        g_lwWorld.dir = dir; g_lwWorld.lg = lg; g_lwWorld.seed = seed;
        g_lwRandom ^= seed | 1u;
        g_lwNodeCount = 0;
        g_lwWhy[0] = 0;
    }
    if (g_lwWorld.populated < 0) return;
    LwPlayers();
    if (g_lwNodeCount == 0)
    {
        if (LwCopyRoutes(lg) == 0) return;
        g_lwWorld.radius = LwPlayRadius(lg);
    }
    if (!g_lwWorld.populated) { if (g_lwWorld.playersAt == 0) { LwWhy("no player position yet"); return; } LwPopulate(); if (g_lwWorld.populated <= 0) return; }
    ULONGLONG now = GetTickCount64();
    float dt = g_lwWorld.last ? (float)(now - g_lwWorld.last) / 1000.0f : 0;
    if (dt > 1.0f) dt = 1.0f;
    g_lwWorld.last = now;
    if (now - g_lwWorld.effAt > 5000) { int eff = LwEffectiveCap(dir); if (eff > 0) g_lwWorld.eff = eff; g_lwWorld.effAt = now; }
    int requested = 0, removals = 0, online = 0, waiting = 0, units = 0, attempts = 0;
    float r = g_lwWorld.radius;
    for (int i = 0; i < g_lwWorld.count; i++) /* the budget: living members of online squads, the request of pending ones */
        units += g_lwSquads[i].state == LW_PENDING ? g_lwSquads[i].cap : g_lwSquads[i].state == LW_ONLINE ? g_lwSquads[i].alive : 0;
    g_lwWorld.onlineUnits = units;
    for (int i = 0; i < g_lwWorld.count && !waiting; i++) waiting = g_lwSquads[i].state == LW_OFFLINE && g_lwSquads[i].cycled == 1;
    for (int i = 0; i < g_lwWorld.count; i++)
    {
        LwSquad* q = &g_lwSquads[i];
        if (q->state == LW_OFFLINE)
        {
            LwWalk(q, dt);
            float near2 = LwNearestPlayer2(q->pos[0], q->pos[1]);
            int back = q->cycled == 1 && near2 >= LW_R_MIN * LW_R_MIN; /* test cycle: back at once (the budget still applies) */
            if (waiting && !back) continue; /* test cycle: a squad waiting to come back is served first */
            if (q->retry && now < q->retry) continue; /* its point was refused (placement or sight) a moment ago */
            if (!requested && attempts < 4 && q->pos[0] * q->pos[0] + q->pos[1] * q->pos[1] < r * r && ((near2 < LW_R_ON * LW_R_ON && near2 >= LW_R_MIN * LW_R_MIN) || back))
            {
                if (!back && LwRecentNear(q->pos)) { q->retry = now + 3000; continue; } /* spaced from online squads and recent requests (audit 1, 11) */
                LwOnline(q); requested = q->state == LW_PENDING; attempts++;
                if (back && requested) { q->cycled = 2; Note("living world: test: squad %d comes back with its %u survivors", (int)(q - g_lwSquads), q->cap); }
            }
        }
        else if (q->state == LW_PENDING) { LwPendingTick(q); online++; }
        else if (q->state == LW_ONLINE) { LwOnlineTick(q, &removals); online++; }
    }
    g_lwWorld.online = online;
    LwWallSample();
    if (now - g_lwWorld.noteAt > 30000)
    {
        g_lwWorld.noteAt = now;
        if (g_lwWalls.requests)
            Note("living world: steering walls (30 s): %u requests, %u with no wall segment (%.1f%%), G %.1f m", g_lwWalls.requests,
                g_lwWalls.empty, 100.0 * g_lwWalls.empty / g_lwWalls.requests, (double)(g_lwWallReach ? *g_lwWallReach : 0));
        g_lwWalls.requests = g_lwWalls.empty = 0;
        if (g_lwWorld.rays || g_lwWorld.noFace || g_lwWorld.moved || g_lwWorld.overhead)
            Note("living world: placement (30 s): refused %u with no navmesh face within 10 m, %u snapped more than 2 m, %u under something, %u in a player's sight; "
                "%u sight rays, %.1f us each; blocked by terrain %u, by static scenery %u", g_lwWorld.noFace, g_lwWorld.moved, g_lwWorld.overhead, g_lwWorld.seen,
                g_lwWorld.rays, g_lwWorld.rays ? g_lwWorld.rayUs / g_lwWorld.rays : 0.0, g_lwWorld.blockTerrain, g_lwWorld.blockUnit);
        g_lwWorld.noFace = g_lwWorld.moved = g_lwWorld.overhead = g_lwWorld.seen = g_lwWorld.rays = g_lwWorld.blockUnit = g_lwWorld.blockTerrain = 0; g_lwWorld.rayUs = 0;
        Note("living world: %d squads, %d online (%d units of %d), %u spawned, %u taken offline, %u destroyed, %u request frames refused; job-1 units %u, AI %u; "
            "player 1 at (%.0f, %.0f)", g_lwWorld.count, online, g_lwWorld.onlineUnits, g_lwWorld.eff, g_lwWorld.spawned, g_lwWorld.offlined, g_lwWorld.died,
            g_lwWorld.refused, EvpU32(dir + 0x614, 0), LwAiCount(), (double)(g_lwWorld.playersAt ? g_lwWorld.players[0][0] : 0),
            (double)(g_lwWorld.playersAt ? g_lwWorld.players[0][1] : 0));
    }
}

/* ---- persistent garrisons (KB "Garrisons", Phase 1 approach after audit 0e) ------------------------------------------
 * Native GuardForce already keeps kills per guard point (its synced U mask); on a refill it re-buys every point whose U
 * bit is set, so kills could be replaced at other points. The loader marks guard points "occupied" (flags +0x24 bit 6)
 * that a refill must not fill: after it has seen the garrison active once, while it is inactive (or its refill flag is
 * set), every phase-B point ((flags & 0x340) == 0) never seen taken (bit 6 while active) and not under a structure.
 * Phase A points and taken points are never touched: taken points refill natively by their U bit (survivors, despawned
 * units, owed spawns), killed points stay empty. The game reads bit 6 only in the fill and the structure despawn
 * (all readers: KB "Bit 6"); the mode-0 despawn clears it on every point and the loader marks again in the next tick,
 * always before the next fill (different world updates). Records: GuardForce gf = [game+0x33265C0], owned i < gf+0x10;
 * synced [gf+0x50] + 0x40 i (+4..+0xC U, +0x38 active), runtime [gf+0x40] + 0x1B58 i (+0x1B4F refill flag, structures
 * +0x810 + 24 k, count +0x1110, +0x10 = local point + 1), point list [gf+0x48] + 0x2B8 i (+0x28 + 4 p global indexes,
 * count +0x1A8), descriptor [[gf+0x38] + 8 i] (+8 entity); points [game+0x346D5C0] + 0x58 g, flags +0x24. */
#define LW_GARRISONS_MAX 512
typedef struct LwGarrison { uint32_t entity; uint8_t seen, wasActive, refills; uint32_t taken[3], structure[3]; ULONGLONG checkAt, activeAt; } LwGarrison;

/* Test instrument (HELLFORGE_LW_TEST_KILL=1, host): one unit of the first garrison active for 15 s is killed through the
 * game's own death path, exactly as the game kills drowning and zone-floor units (0xAA88D8 / 0x89C9C9; KB living-world-plan
 * "Member losses" Q4): QueueDamage 0x129F910(NULL, kind 7, 0, unit, 0xFFFFFFFF, 0, net, net, 0, 0, 0, 0, 0, &dir, &pos,
 * actor 0xFFFFFFFF (no limb: zone 0), 0, 0, 1.0); applied in the next frame's damage drain, the death link then clears
 * its point's U bit (0x910070 killed = 1). The point is found by that U bit; after the garrison fills again the note
 * says whether the point stayed empty (no unit: occupied bit 6 clear). Runtime list: [gf+0x40] + 0x1B58 i, +0x204 u32
 * units, count +0x384 (max 96). */
static struct { int on, stage; uint32_t garrison, unit, u[3], point, net, slot; ULONGLONG at; } g_lwKill = {-1};
/* The living units AiEnemy records at a garrison's point (as the game's own list rebuild 0x90AFE2 reads them, P):
 * E = [game+0x33266C8], count [E+0x2320], records [E+0x2360] + 20 i (+0xC the garrison's network id, +0x10 its local
 * point + 1), entity [[E+0x2348] + 8 i] + 8. With unit != 0: fills *net and *slot for that unit instead (1 when found). */
static int LwGarrisonUnitsAt(uint32_t unit, uint32_t* net, uint32_t* slot)
{
    uintptr_t e, records, entities, d; int found = 0;
    if (!EvpPtr(g_lw.game + 0x33266C8u, &e) || !EvpPtr(e + 0x2360, &records) || !EvpPtr(e + 0x2348, &entities)) return -1;
    uint32_t count = EvpU32(e + 0x2320, 0);
    for (uint32_t i = 0; i < count && i < 65536; i++)
    {
        uint32_t r[5]; ActHealth h;
        if (SafeRead(records + 20u * i, r, sizeof r) || !EvpPtr(entities + 8u * i, &d) || d == 0) continue;
        uint32_t entity = EvpU32(d + 8, 0);
        if (unit) { if (entity == unit) { *net = r[3]; *slot = r[4]; return 1; } continue; }
        if (r[3] == *net && r[4] == *slot && ActHealthOf(g_lw.game, entity, &h) && h.life < 2) found++;
    }
    return unit ? 0 : found;
}
static void LwKillTest(LwGarrison* g, uintptr_t r, uintptr_t l, uintptr_t points, const uint32_t* u, uint32_t n, uint8_t active)
{
    typedef void (*QueueDamageFn)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t, uint32_t, uint32_t, uint64_t, uint32_t, float, float, float,
        const float*, const float*, uint32_t, uint32_t, uint32_t, float);
    ULONGLONG now = GetTickCount64();
    if (g_lwKill.on < 0) { char t[4] = ""; g_lwKill.on = GetEnvironmentVariableA("HELLFORGE_LW_TEST_KILL", t, sizeof t) > 0 && t[0] == '1'; }
    if (!g_lwKill.on) return;
    if (g_lwKill.stage == 0 && active && g->activeAt && now - g->activeAt > 15000)
    {
        uint32_t count = EvpU32(r + 0x384, 0), unit = 0, system = 0; uintptr_t sys; ActHealth h; float pos[3];
        for (uint32_t k = 0; k < count && k < 96 && !unit; k++)
        { uint32_t e = EvpU32(r + 0x204 + 4u * k, 0); if (e && ActHealthOf(g_lw.game, e, &h) && h.life < 2 && h.unit && h.network && EvpUnitPosition(h.unit, pos)) unit = e; }
        if (unit == 0 || count < 2) return;
        if (!ApiCodeMatches(g_lw.game + 0x129F910u, "40 55 53 56 57 41 56 48 8D 6C 24 F9 48 81 EC D0 00 00 00"))
        { Note("living world: test: kill: QueueDamage is not the reviewed code"); g_lwKill.stage = 9; return; }
        if (!EvpPtr(g_lw.game + 0x347CF38u, &sys) || (system = EvpU32(sys + 0x201120, 4096)) >= 4096) return;
        static const float down[3] = {0, 0, -1};
        if (LwGarrisonUnitsAt(unit, &g_lwKill.net, &g_lwKill.slot) != 1) g_lwKill.slot = 0;
        memcpy(g_lwKill.u, u, sizeof g_lwKill.u);
        ((QueueDamageFn)(g_lw.game + 0x129F910u))(NULL, 7, 0, unit, 0xFFFFFFFFu, 0, h.network, h.network, 0, 0, 0.0f, 0.0f, 0.0f, down, pos, 0xFFFFFFFFu, 0, 0, 1.0f);
        g_lwKill.garrison = g->entity; g_lwKill.unit = unit; g_lwKill.at = now; g_lwKill.stage = 1;
        Note("living world: test: kill: garrison %08X unit %08X at (%.0f, %.0f, %.1f) killed (QueueDamage kind 7); %u units listed; its AiEnemy record: "
            "garrison net %08X, point %d", g->entity, unit, (double)pos[0], (double)pos[1], (double)pos[2], count, g_lwKill.net, (int)g_lwKill.slot - 1);
        return;
    }
    if (g->entity != g_lwKill.garrison) return;
    if (g_lwKill.stage == 1)
    {
        ActHealth h; uint32_t life = ActHealthOf(g_lw.game, g_lwKill.unit, &h) ? h.life : 99;
        for (uint32_t p = 0; p < n; p++)
        {
            uint32_t bit = 1u << (p % 32);
            if ((g_lwKill.u[p / 32] & bit) && !(u[p / 32] & bit))
            {
                g_lwKill.point = p; g_lwKill.stage = 2;
                Note("living world: test: kill: after %llu ms the unit's life is %u and point %u's U bit is clear (a kill)", (unsigned long long)(now - g_lwKill.at), life, p);
                return;
            }
        }
        if (now - g_lwKill.at > 10000) { Note("living world: test: kill: no U bit cleared in 10 s (unit life %u)", life); g_lwKill.stage = 9; }
    }
}
static void LwKillAfterFill(LwGarrison* g, uintptr_t l, uintptr_t points, const uint32_t* u, uint32_t listed)
{
    if (g_lwKill.stage != 2 || g->entity != g_lwKill.garrison) return;
    uint32_t p = g_lwKill.point, gi = EvpU32(l + 0x28 + 4u * p, 0xFFFFFFFFu), flags = 0xFFFFFFFFu;
    if (gi < 0x100000) SafeRead(points + 0x58u * gi + 0x24, &flags, 4);
    int living = g_lwKill.slot ? LwGarrisonUnitsAt(0, &g_lwKill.net, &g_lwKill.slot) : -1;
    Note("living world: test: kill: garrison %08X filled again with %u units; the killed point %u: U %u, occupied bit %u; living units whose AiEnemy "
        "record names that point: %d (%s)", g->entity, listed, p, (u[p / 32] >> (p % 32)) & 1, (flags >> 6) & 1, living,
        living == 0 ? "stays empty" : living > 0 ? "FILLED" : "unreadable");
    g_lwKill.stage = 3;
}
static LwGarrison g_lwGarrisons[LW_GARRISONS_MAX];
static int g_lwGarrisonCount;
static uintptr_t g_lwGarrisonOwner; /* the GuardForce component the records belong to (a new mission makes a new one) */

static LwGarrison* LwGarrisonOf(uint32_t entity)
{
    for (int i = 0; i < g_lwGarrisonCount; i++) if (g_lwGarrisons[i].entity == entity) return &g_lwGarrisons[i];
    if (g_lwGarrisonCount >= LW_GARRISONS_MAX) return NULL;
    LwGarrison* g = &g_lwGarrisons[g_lwGarrisonCount++];
    memset(g, 0, sizeof *g);
    g->entity = entity;
    return g;
}

/* The survivor mask (KB "Garrisons", Phase 1 approach): notes when a garrison goes inactive (taken and killed points)
 * and, 5 s after it fills again, how many units it listed against the points that had a unit and are not killed. */
static void LwGarrisonsTick(void)
{
    uintptr_t gf, synced, runtime, lists, descriptors, points;
    if (!EvpPtr(g_lw.game + 0x33265C0u, &gf) || !EvpPtr(gf + 0x50, &synced) || !EvpPtr(gf + 0x40, &runtime) || !EvpPtr(gf + 0x48, &lists) ||
        !EvpPtr(gf + 0x38, &descriptors) || !EvpPtr(g_lw.game + 0x346D5C0u, &points)) return;
    if (gf != g_lwGarrisonOwner) { g_lwGarrisonOwner = gf; g_lwGarrisonCount = 0; }
    uint32_t owned = EvpU32(gf + 0x10, 0), marked = 0;
    if (owned > LW_GARRISONS_MAX) owned = LW_GARRISONS_MAX;
    for (uint32_t i = 0; i < owned; i++)
    {
        uintptr_t d, s = synced + 0x40u * i, r = runtime + 0x1B58u * i, l = lists + 0x2B8u * i;
        uint8_t active = 0, refill = 0; uint32_t u[3], n = EvpU32(l + 0x1A8, 0xFFFFFFFFu);
        if (!EvpPtr(descriptors + 8u * i, &d) || n > 96 || SafeRead(s + 0x38, &active, 1) || SafeRead(r + 0x1B4F, &refill, 1) || SafeRead(s + 4, u, sizeof u)) continue;
        LwGarrison* g = LwGarrisonOf(EvpU32(d + 8, 0));
        if (g == NULL || g->entity == 0) continue;
        if (active && g->seen && !g->wasActive) g->checkAt = GetTickCount64() + 5000; /* filled again: compare in 5 s */
        if (active && !g->wasActive) g->activeAt = GetTickCount64();
        LwKillTest(g, r, l, points, u, n, active);
        if (!active && g->wasActive)
        {
            uint32_t taken = 0, empty = 0;
            for (uint32_t p = 0; p < n; p++) { uint32_t bit = 1u << (p % 32); taken += (g->taken[p / 32] & bit) != 0; empty += (g->taken[p / 32] & bit) && !(u[p / 32] & bit); }
            Note("living world: garrison %08X inactive: %u of %u points taken while active, %u of them killed (U clear)", g->entity, taken, n, empty);
        }
        g->wasActive = active;
        if (active && g->checkAt && GetTickCount64() >= g->checkAt)
        {
            uint32_t expect = 0, structures = 0;
            for (uint32_t p = 0; p < n; p++)
            {
                uint32_t bit = 1u << (p % 32);
                if (g->structure[p / 32] & bit) { structures++; continue; }
                if ((g->taken[p / 32] & bit) && (u[p / 32] & bit)) expect++;
            }
            Note("living world: garrison %08X filled again: %u units listed; %u points had a unit and are not killed (%u structure points)", g->entity,
                EvpU32(r + 0x384, 0), expect, structures);
            LwKillAfterFill(g, l, points, u, EvpU32(r + 0x384, 0));
            g->checkAt = 0; g->refills++;
        }
        if (active)
        {
            if (!g->seen) Note("living world: garrison %08X active (%u guard points, %u units listed); its kills stay and only its own points refill", g->entity, n,
                EvpU32(r + 0x384, 0));
            g->seen = 1;
            for (uint32_t p = 0; p < n; p++)
            {
                uint32_t gi = EvpU32(l + 0x28 + 4u * p, 0xFFFFFFFFu), flags = 0;
                if (gi < 0x100000 && !SafeRead(points + 0x58u * gi + 0x24, &flags, 4) && (flags & 0x40)) g->taken[p / 32] |= 1u << (p % 32);
            }
            uint32_t structures = EvpU32(r + 0x1110, 0);
            for (uint32_t k = 0; k < structures && k < 96; k++)
            {
                uint32_t local = EvpU32(r + 0x810 + 24u * k + 0x10, 0);
                if (local >= 1 && local <= n) g->structure[(local - 1) / 32] |= 1u << ((local - 1) % 32);
            }
        }
        if (!g->seen || (active && !refill)) continue; /* the first fill stays native; no marks while a garrison holds */
        for (uint32_t p = 0; p < n; p++)
        {
            uint32_t bit = 1u << (p % 32);
            if ((g->taken[p / 32] | g->structure[p / 32]) & bit) continue;
            uint32_t gi = EvpU32(l + 0x28 + 4u * p, 0xFFFFFFFFu), flags = 0;
            if (gi >= 0x100000) continue;
            uintptr_t at = points + 0x58u * gi + 0x24;
            if (SafeRead(at, &flags, 4) || (flags & 0x340) != 0) continue; /* phase A, objective points and marked ones stay */
            flags |= 0x40;
            if (!LwPoke(at, &flags, 4)) marked++;
        }
    }
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (marked && now - last > 5000) { last = now; Note("living world: garrisons: %u guard points held empty (kills are not replaced elsewhere)", marked); }
}

/* ---- XL: the steering walls' reach and the navmesh under the detailed tiles (KB living-world-plan, "Navmesh vs near
 * tiles: the steering-wall fix" and "Phase 1c static answers"; P unless marked) -----------------------------------------
 * B, the reach: game.dll 0x59CD06 (addss xmm1, xmm0 / movaps xmm0, xmm5 / mulss xmm1, xmm6 / movaps xmm6, xmm2) makes the
 * half-height of the steering unit's wall box (xmm1, then z -/+ xmm1 at 0x59CD18 / 0x59CD1C); the stub adds G after the
 * multiply. G = 0.0 adds exactly nothing (vanilla maps); on an XL map G = 8 m (the Recast cell height and detail error,
 * 4 m each) plus the largest gap between a live tile's ground and the coarse heights the navmesh was built from.
 * A, the navmesh follows each tile: the nav's height copy H = [nav+0x38] (+0x40 u16 N x N, N = +0x50, row j = N-1-ys,
 * metres = [H+0x38] + v ([H+0x60] - [H+0x5C]) / 65535, rasteriser exe 0x89B65F..0x89B840) is the live buffer a cell
 * job reads once at its start (0x7B85D9 -> 0x89AB53), 4 samples into each neighbour cell; the cell tick, the scheduler
 * and completions run on this (the frame) thread before the loader's next tick, at most one rebuild job in flight
 * ([nav+0x90] budget), [nav+0x94] the jobs in flight (+1 0x7B86BA, -1 0x7B8C38). So: when no job is in flight, write the
 * tile's heights into H inside its hidden square (the coarse ones back when it goes) and mark every cell whose read
 * rectangle overlaps the samples written with exe 0x7B6230(id, &xy) (cell +0x5C |= 0x10, priority + 1). */
#define LW_WALL_SITE 0x59CD06u
#define LW_WALL_BACK 0x59CD14u
#define LW_NAV_MARK 0x7B6230u        /* exe: mark the cell under xy dirty (u32 world id, const float xy[2]) */
#define LW_NAV_WORLDS 0x27C30B8u     /* exe: the nav world pointers; their count at +8 */
#define LW_RECAST_ERROR 8.0f
static const ActPin kLwNavPins[] = {
    {1, 0x7B6230u, "4883ec28390d86ce0002765d4c8b1575"},                 /* mark dirty (checks id < [0x27C30C0]) */
    {1, 0x7B86BAu, "ff8794000000"}, {1, 0x7B8C38u, "41ff8d94000000"},   /* the jobs in flight, +1 / -1 */
    {1, 0x7B85D9u, "488b4738"},                                          /* a job takes the live heights pointer */
    {1, 0x7ACDD0u, "f30f104114f30f5c410cf30f"},                          /* the cell lookup: grid +0x0C.. min / max */
    {1, 0x89B65Fu, "8b6950488b79404c8b79480f2970a8f30f1071600f297898f3410f107920440f294088440f28c6f30f5c"}, /* the decode */
};
/* g_lwWallReach (declared with the walls measurement): the stub's G; NULL when the stub is not in */

/* The leave crash (lw16i; KB living-world-plan "Leave crash", subagent leavecrash, checked by me in the dump): when the
 * rebuild job of a cell that was already built (flags +0x5C bit 0x8) completes, exe 0x7B9053 unloads its old section
 * and 0x7B905F sets the section id [cell+0x60] = -1; it loads a new one only while the nav runs (([nav+0x20] & 3) == 3,
 * gate 0x7B91B8) and never clears 0x8 (0x7B91AF clears only 0x10). After the leave began (0xAB345B clears bit 0x2), the
 * unload pass 0x7B7C40 then unloads every cell with 0x8 and not 0x1 by its id unchecked (0x7B7CEC..0x7B7D16), so it
 * queues section -1 to the Havok AI world (0xBB2670), whose step (ApplyAsyncWorldUpdatesTask, 0xC3A790 -> 0xBAC380)
 * reads before its section array and crashes at 0x99CCE0. Vanilla builds cells in under a second, the loader's rebuilds
 * of built cells take 20-110 s. The fix keeps the game's state honest: right where it sets -1, it also clears 0x8 (a
 * successful reload sets 0x8 again at 0x7B9252); no branch lands in 0x7B9060..0x7B9065. */
static void LwInstallNavLeaveFix(void)
{
    static int done;
    if (done) return;
    done = 1;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (!ApiCodeMatches(exe + 0x7B905Fu, "C7 43 60 FF FF FF FF 41 F7 47 0C 00 01 00 00") ||
        !ApiCodeMatches(exe + 0x7B9053u, "E8") || !ApiCodeMatches(exe + 0x7B91AFu, "8B 43 5C 83 E0 EF 83 C8 04"))
    { Note("living world: the nav completion is not the reviewed code; rebuilt cells are left as the game keeps them"); return; }
    uint8_t* block = AllocateNearGame(exe, 0x1000);
    if (block == NULL) { Note("living world: no memory within 2 GB of the game's exe for the nav completion fix"); return; }
    uint8_t code[16] = {0xC7, 0x43, 0x60, 0xFF, 0xFF, 0xFF, 0xFF, /* mov dword [rbx+0x60], -1 */
        0x83, 0x63, 0x5C, 0xF7,                                       /* and dword [rbx+0x5C], ~0x8 */
        0xE9, 0, 0, 0, 0};                                            /* jmp 0x7B9066 (rel32 at 12, next 16) */
    int64_t back = (int64_t)(exe + 0x7B9066u) - (int64_t)(uintptr_t)(block + 16), to = (int64_t)(uintptr_t)block - (int64_t)(exe + 0x7B905Fu + 5);
    if (!InRel32(back) || !InRel32(to)) { Note("living world: the nav completion fix is out of reach of the exe"); return; }
    int32_t rel = (int32_t)back; memcpy(code + 12, &rel, 4);
    memcpy(block, code, sizeof code);
    DWORD protect; VirtualProtect(block, 0x1000, PAGE_EXECUTE_READ, &protect); FlushInstructionCache(GetCurrentProcess(), block, 0x1000);
    uint8_t jump[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    rel = (int32_t)to; memcpy(jump + 1, &rel, 4);
    if (ApiWrite(exe + 0x7B905Fu, jump, sizeof jump)) { Note("living world: the nav completion could not be written"); return; }
    Note("living world: a rebuilt cell's completion while the nav is stopping clears its built flag with its section (leave crash fix)");
}

/* From InstallWorld (an extra large world is configured), before any mission: the stub with G = 0. */
static void LwInstallWallReach(uintptr_t game)
{
    LwInstallNavLeaveFix();
    if (!LwEnabled() || g_lwWallReach != NULL) return;
    if (!ApiCodeMatches(game + LW_WALL_SITE, "F3 0F 58 C8 0F 28 C5 F3 0F 59 CE 0F 28 F2") ||
        !ApiCodeMatches(game + 0x59CCFEu, "C7 44 24 78 00 00 80 40"))
    { Note("living world: the steering walls' box is not the reviewed one; its reach stays the game's"); return; }
    uint8_t* block = AllocateNearGame(game, 0x2000); /* the stub's page, then G's (read-write) */
    if (block == NULL) { Note("living world: no memory within 2 GB of game.dll for the walls' reach"); return; }
    static const uint8_t code[27] = {0xF3, 0x0F, 0x58, 0xC8, 0x0F, 0x28, 0xC5, 0xF3, 0x0F, 0x59, 0xCE,
        0xF3, 0x0F, 0x58, 0x0D, 0, 0, 0, 0, /* addss xmm1, [rip+G] (rel32 at 15, next 19) */
        0x0F, 0x28, 0xF2, 0xE9, 0, 0, 0, 0}; /* movaps xmm6, xmm2; jmp 0x59CD14 (rel32 at 23, next 27) */
    memcpy(block, code, sizeof code);
    int32_t rel = 0x1000 - 19; memcpy(block + 15, &rel, 4);
    int64_t back = (int64_t)(game + LW_WALL_BACK) - (int64_t)(uintptr_t)(block + 27);
    if (!InRel32(back)) { Note("living world: the walls' reach stub is out of reach of game.dll"); return; }
    rel = (int32_t)back; memcpy(block + 23, &rel, 4);
    *(float*)(block + 0x1000) = 0.0f;
    DWORD protect;
    VirtualProtect(block, 0x1000, PAGE_EXECUTE_READ, &protect);
    FlushInstructionCache(GetCurrentProcess(), block, 0x1000);
    uint8_t patch[14];
    memset(patch, 0x90, sizeof patch);
    patch[0] = 0xE9; rel = DifficultyJumpRel(game + LW_WALL_SITE, (uintptr_t)block); memcpy(patch + 1, &rel, 4);
    if (ApiWrite(game + LW_WALL_SITE, patch, sizeof patch)) { Note("living world: the walls' reach jump could not be written"); return; }
    g_lwWallReach = (volatile float*)(block + 0x1000);
    Note("living world: the steering walls' reach stub is in (G 0 until an extra large mission)");
    /* Measurement: the steering consumer 0x134AD60 reads each request's segment count at 0x134CB8B (mov r13d,
     * [rdi+rsi+0x60]; test r13d, r13d; je 0x134D306). The queue's count is 0 again by the late tick, so the share of
     * requests with no wall is counted there: lock inc (it may run on worker threads), the test redone before going on. */
    if (!ApiCodeMatches(game + 0x134CB8Bu, "44 8B 6C 37 60 45 85 ED 0F 84 6D 07 00 00")) { Note("living world: the steering walls' reader is not the reviewed one; no wall count"); return; }
    uint8_t* count = AllocateNearGame(game, 0x2000);
    if (count == NULL) return;
    uint8_t c[32] = {0x44, 0x8B, 0x6C, 0x37, 0x60,     /* mov r13d, [rdi+rsi+0x60] (the original) */
        0xF0, 0xFF, 0x05, 0, 0, 0, 0,                   /* lock inc dword [rip+all] (rel32 at 8, next 12) */
        0x45, 0x85, 0xED, 0x75, 0x07,                   /* test r13d, r13d; jnz +7 */
        0xF0, 0xFF, 0x05, 0, 0, 0, 0,                   /* lock inc dword [rip+none] (rel32 at 20, next 24) */
        0x45, 0x85, 0xED,                               /* test r13d, r13d (the flags the je reads) */
        0xE9, 0, 0, 0, 0};                              /* jmp 0x134CB93 (rel32 at 28, next 32) */
    rel = 0x1000 - 12; memcpy(c + 8, &rel, 4);
    rel = 0x1004 - 24; memcpy(c + 20, &rel, 4);
    back = (int64_t)(game + 0x134CB93u) - (int64_t)(uintptr_t)(count + 32);
    if (!InRel32(back)) return;
    rel = (int32_t)back; memcpy(c + 28, &rel, 4);
    memcpy(count, c, sizeof c);
    VirtualProtect(count, 0x1000, PAGE_EXECUTE_READ, &protect);
    FlushInstructionCache(GetCurrentProcess(), count, 0x1000);
    uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    rel = DifficultyJumpRel(game + 0x134CB8Bu, (uintptr_t)count); memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + 0x134CB8Bu, jump, 8)) return;
    g_lwWallCounts = (volatile LONG*)(count + 0x1000);
}

#define LW_NAV_CELLS_MAX 4096
static struct
{
    int pins;                       /* 0 not checked, 1 ok, -1 not the reviewed code */
    uintptr_t heights; LONG level;  /* the height copy written to, for this level */
    uint16_t* orig;                 /* that copy as the game made it (the coarse heights) */
    uint32_t n;
    int applied[NEAR_TILES_MAX]; float centre[NEAR_TILES_MAX][2]; uint64_t handle[NEAR_TILES_MAX];
    float gap[NEAR_TILES_MAX]; uint64_t gapHandle[NEAR_TILES_MAX];
    struct { uint16_t cx, cy; uint32_t section; ULONGLONG at; } watch[64];
    int watching;
    LONG noted;                     /* why it waits, noted once a level (the level + 1) */
    uint32_t busyFrames;
} g_lwNav;

typedef void (*LwMarkFn)(uint32_t id, const float* xy);

/* The level's nav world: its pointer and id, the height copy, and whether it is an XL grid in rebuild mode (flags & 0x17
 * == 0x13: created, enabled, rebuild mode, no unload asked). */
static int LwNavWorld(uintptr_t dir, uintptr_t* nav, uint32_t* id, uintptr_t* heights)
{
    uintptr_t exe = g_navExe, worlds;
    *id = EvpU32(dir + 0x648, 0xFFFFFFFFu);
    if (exe == 0 || *id >= EvpU32(exe + LW_NAV_WORLDS + 8, 0) || !EvpPtr(exe + LW_NAV_WORLDS, &worlds) ||
        !EvpPtr(worlds + 8 * (uintptr_t)*id, nav)) return 0;
    uint32_t flags = EvpU32(*nav + 0x20, 0);
    float tile = 0;
    if ((flags & 0x17) != 0x13 || !EvpPtr(*nav + 0x38, heights) || SafeRead(*nav + 0x6718, &tile, 4) || g_navWorldTile == NULL ||
        fabsf(tile - *g_navWorldTile) > 1.0f) return 0;
    return 1;
}

/* The nav sample (i, j) at its coarse terrain point (the copy is the terrain's own u16 samples: the coarse convention,
 * row 0 at the top, +y). */
static void LwNavPoint(uint32_t i, uint32_t j, float* x, float* y)
{
    const NearGround* c = &g_nearCoarseGround;
    float step = c->size / (float)(c->n - 1);
    *x = c->cx - c->size / 2 + step * (float)i; *y = c->cy + c->size / 2 - step * (float)j;
}

/* The samples whose points lie in the square of side NEAR_HIDE about (cx, cy), half-open [c - h, c + h), clamped:
 * x_i = left + step i in [cx - h, cx + h) <=> i in [ceil((cx - h - left) / step), ceil((cx + h - left) / step) - 1];
 * y_j = top - step j in [cy - h, cy + h) <=> j in [floor((top - cy - h) / step) + 1, floor((top - cy + h) / step)]. */
static int LwNavRect(float cx, float cy, uint32_t* i0, uint32_t* i1, uint32_t* j0, uint32_t* j1)
{
    const NearGround* c = &g_nearCoarseGround;
    float step = c->size / (float)(c->n - 1), h = NEAR_HIDE / 2, left = c->cx - c->size / 2, top = c->cy + c->size / 2, last = (float)(c->n - 1);
    float a = ceilf((cx - h - left) / step), b = ceilf((cx + h - left) / step) - 1;
    float d = floorf((top - cy - h) / step) + 1, e = floorf((top - cy + h) / step);
    if (a < 0) a = 0;
    if (d < 0) d = 0;
    if (b > last) b = last;
    if (e > last) e = last;
    if (a > b || d > e) return 0;
    *i0 = (uint32_t)a; *i1 = (uint32_t)b; *j0 = (uint32_t)d; *j1 = (uint32_t)e;
    return 1;
}

static int LwInSquare(float x, float y, const float* centre)
{
    float h = NEAR_HIDE / 2, dx = x - centre[0], dy = y - centre[1];
    return dx >= -h && dx < h && dy >= -h && dy < h;
}

/* The largest |tile - coarse| over a tile's square (for G). */
static float LwNavGap(int k, float base, float range)
{
    uint32_t i0, i1, j0, j1, n = g_lwNav.n;
    float worst = 0;
    if (!LwNavRect(g_tiles[k].centre[0], g_tiles[k].centre[1], &i0, &i1, &j0, &j1)) return 0;
    for (uint32_t j = j0; j <= j1; j++)
        for (uint32_t i = i0; i <= i1; i++)
        {
            float x, y; LwNavPoint(i, j, &x, &y);
            float t = NearGroundAt(&g_tiles[k].ground, x, y);
            if (t < -9000.0f) continue;
            float d = fabsf(t - (base + g_lwNav.orig[(size_t)j * n + i] * range / 65535.0f));
            if (d > worst) worst = d;
        }
    return worst;
}

/* The cells whose read rectangle (cell bounds +- 4 cell sizes; Recast columns trunc((x + S/2) N / S), ys = N-1-j)
 * overlaps samples [i0..i1] x [j0..j1], into the bitmap (cy W + cx, the index 0x7B6230 uses). The margin is 4 cell
 * sizes plus one sample (cs = the sample spacing S / N here: 4 m). */
static void LwNavCellsOf(uintptr_t nav, float size, uint32_t i0, uint32_t i1, uint32_t j0, uint32_t j1, uint8_t* cells)
{
    uint16_t wh[2]; float g[4];
    if (SafeRead(nav + 0x6710, wh, 4) || SafeRead(nav + 0x671C, g, 16) || wh[0] == 0 || wh[1] == 0 || (uint32_t)wh[0] * wh[1] > LW_NAV_CELLS_MAX) return;
    uint32_t n = g_lwNav.n;
    float s = size / (float)n, m = 5 * s;
    float xa = (float)i0 * s - size / 2 - m, xb = (float)(i1 + 1) * s - size / 2 + m;
    float ya = (float)(n - 1 - j1) * s - size / 2 - m, yb = (float)(n - j0) * s - size / 2 + m;
    int c0 = (int)floorf((xa - g[0]) / (g[2] - g[0]) * wh[0]), c1 = (int)floorf((xb - g[0]) / (g[2] - g[0]) * wh[0]);
    int r0 = (int)floorf((ya - g[1]) / (g[3] - g[1]) * wh[1]), r1 = (int)floorf((yb - g[1]) / (g[3] - g[1]) * wh[1]);
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 > wh[0] - 1) c1 = wh[0] - 1;
    if (r1 > wh[1] - 1) r1 = wh[1] - 1;
    for (int r = r0; r <= r1; r++) for (int c = c0; c <= c1; c++) cells[r * wh[0] + c] = 1;
}

/* A cell's flags +0x5C (0x1 in flight, 0x10 dirty) and its section id +0x60 (cells [nav+0x6748], stride u16 [nav+0x6736]). */
static uint32_t LwCellFlags(uintptr_t nav, uint32_t index, uint32_t* section)
{
    uintptr_t cells;
    uint16_t stride = 0;
    if (!EvpPtr(nav + 0x6748, &cells) || SafeRead(nav + 0x6736, &stride, 2) || stride == 0) return 0xFFFFFFFFu;
    if (section != NULL) *section = EvpU32(cells + (uintptr_t)stride * index + 0x60, 0);
    return EvpU32(cells + (uintptr_t)stride * index + 0x5C, 0xFFFFFFFFu);
}

static void LwNavWait(const char* why)
{
    if (g_lwNav.noted == g_world.levels + 1) return;
    g_lwNav.noted = g_world.levels + 1;
    Note("living world: nav: %s", why);
}

/* The cells around the players first (audit 1, criterion 11; KB living-world-plan "XL nav cell order", P): the game's
 * scheduler (0x7B8200) starts one rebuild job a frame at most, the highest priority ([nav+0x6708] {cell, prio}) first;
 * the mission update adds +100 a frame to each player's own cell and +25 to its 8 neighbours (0xACD576..0xACD616:
 * [[game+0x33263C8]+0x70] = exe 0x7DA380(u32 [[world]+0x10F0], float xyz[3]) -> 0x7B63E0, which keeps 0 (built or in
 * flight) as 0). The loader makes the same call, every tick, for every cell within LW_R_ON of a player (a point at its
 * centre), so the cells squads come online in outrank the far neighbours. The world id is the game's own argument
 * (world [game+0x3326340], +0x10F0 written by the nav world's create at 0xAB1B31). */
static void LwNavBoost(uintptr_t nav, const uint16_t* wh, uint32_t marksId)
{
    typedef void (*BoostFn)(uint32_t id, const float* xyz);
    uintptr_t table = 0, fn = 0, world = 0;
    static uint32_t lastList[16]; static int lastCount = -1; static uintptr_t lastNav;
    if (!EvpPtr(g_lw.game + 0x33263C8u, &table) || !EvpPtr(table + 0x70, &fn) || fn == 0 || !EvpPtr(g_lw.game + 0x3326340u, &world)) return;
    uint32_t id = EvpU32(world + 0x10F0, 0xFFFFFFFFu);
    float g[4] = {0, 0, 0, 0};
    if (SafeRead(nav + 0x671C, g, 16) || g[2] <= g[0] || g[3] <= g[1]) return;
    float cw = (g[2] - g[0]) / wh[0], ch = (g[3] - g[1]) / wh[1];
    uint32_t list[16]; int count = 0;
    for (int p = 0; p < g_lwWorld.playersAt; p++)
    {
        const float* at = g_lwWorld.players[p];
        int i0 = (int)floorf((at[0] - LW_R_ON - g[0]) / cw), i1 = (int)floorf((at[0] + LW_R_ON - g[0]) / cw);
        int j0 = (int)floorf((at[1] - LW_R_ON - g[1]) / ch), j1 = (int)floorf((at[1] + LW_R_ON - g[1]) / ch);
        for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < wh[1]; j++)
            for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < wh[0]; i++)
            {
                uint32_t c = (uint32_t)j * wh[0] + (uint32_t)i; int dup = 0;
                for (int k = 0; k < count && !dup; k++) dup = list[k] == c;
                if (dup || count >= 16) continue;
                list[count++] = c;
                float xyz[3] = {g[0] + ((float)i + 0.5f) * cw, g[1] + ((float)j + 0.5f) * ch, at[2]};
                ((BoostFn)fn)(id, xyz);
            }
    }
    if (nav != lastNav || count != lastCount || memcmp(list, lastList, sizeof(uint32_t) * (size_t)count))
    {
        char text[160]; int len = 0; text[0] = 0;
        for (int k = 0; k < count && len < (int)sizeof text - 16; k++) len += snprintf(text + len, sizeof text - len, " (%u, %u)", list[k] % wh[0], list[k] / wh[0]);
        Note("living world: nav: cells within %.0f m of the players boosted (world %u, the director's %u):%s", (double)LW_R_ON, id, marksId, text);
        memcpy(lastList, list, sizeof list); lastCount = count; lastNav = nav;
    }
}

static void LwNavTick(uintptr_t dir)
{
    uintptr_t exe = g_navExe, nav, H;
    uint32_t id;
    if (g_navWorldScale <= 1 || exe == 0) return;
    if (g_lwNav.pins == 0) g_lwNav.pins = ActPins(g_lw.game, kLwNavPins, sizeof kLwNavPins / sizeof kLwNavPins[0]) ? 1 : -1;
    if (g_lwNav.pins < 0) { LwNavWait("the navmesh rebuild code is not the reviewed one; the navmesh keeps the coarse ground"); return; }
    if (!LwNavWorld(dir, &nav, &id, &H)) { if (g_lwWallReach) *g_lwWallReach = 0.0f; LwNavWait("not an extra large nav grid in rebuild mode (G 0)"); return; }
    const NearGround* c = &g_nearCoarseGround;
    uint32_t n = EvpU32(H + 0x50, 0);
    float f[4]; /* +0x54 / +0x58 the size, +0x5C / +0x60 the range */
    uintptr_t data;
    if (c->v == NULL || n != 4096 || c->n != n || SafeRead(H + 0x54, f, 16) || !EvpPtr(H + 0x40, &data)) { LwNavWait("waiting for the coarse heights and the nav's copy"); return; }
    float base = 0, range = f[3] - f[2], size = f[0];
    SafeRead(H + 0x38, &base, 4);
    if (range <= 0 || size <= 0) { LwNavWait("the nav's height range or size is empty"); return; }
    if (H != g_lwNav.heights || g_lwNav.level != g_world.levels) /* a new copy: keep it as the game made it */
    {
        if (g_lwNav.orig == NULL) g_lwNav.orig = (uint16_t*)VirtualAlloc(NULL, (size_t)n * n * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_lwNav.orig == NULL || SafeRead(data, g_lwNav.orig, (size_t)n * n * 2)) { LwNavWait("no copy of the nav's heights"); return; }
        memset(g_lwNav.applied, 0, sizeof g_lwNav.applied); memset(g_lwNav.gapHandle, 0, sizeof g_lwNav.gapHandle);
        g_lwNav.heights = H; g_lwNav.level = g_world.levels; g_lwNav.n = n; g_lwNav.watching = 0; g_lwNav.busyFrames = 0;
        Note("living world: nav: world %u, heights 0x%llx: %u x %u, size %.1f m, base %.2f m, range %.2f m", id, (unsigned long long)H, n, n,
            (double)size, (double)base, (double)range);
    }
    /* What each slot should be (the live tiles of this level), its gap, and G */
    int want[NEAR_TILES_MAX];
    float worst = 0;
    for (int k = 0; k < NEAR_TILES_MAX; k++)
    {
        want[k] = g_tiles[k].live && g_tiles[k].level == g_world.levels && g_tiles[k].ground.v != NULL;
        if (want[k] && g_lwNav.gapHandle[k] != g_tiles[k].handle)
        {
            g_lwNav.gap[k] = LwNavGap(k, base, range); g_lwNav.gapHandle[k] = g_tiles[k].handle;
            Note("living world: nav: tile %d at (%.0f, %.0f): up to %.1f m off the coarse heights", k, (double)g_tiles[k].centre[0],
                (double)g_tiles[k].centre[1], (double)g_lwNav.gap[k]);
        }
        if (want[k] && g_lwNav.gap[k] > worst) worst = g_lwNav.gap[k];
    }
    if (g_lwWallReach) *g_lwWallReach = LW_RECAST_ERROR + worst;
    uint16_t wh[2] = {0, 0};
    if (SafeRead(nav + 0x6710, wh, 4) || (uint32_t)wh[0] * wh[1] > LW_NAV_CELLS_MAX || wh[0] == 0) return;
    uint32_t total = (uint32_t)wh[0] * wh[1];
    LwNavBoost(nav, wh, id);
    /* The rebuilds asked for: done when the cell is neither dirty nor in flight */
    for (int w = 0; w < g_lwNav.watching; )
    {
        uint32_t section = 0, flags = LwCellFlags(nav, (uint32_t)g_lwNav.watch[w].cy * wh[0] + g_lwNav.watch[w].cx, &section);
        if (flags != 0xFFFFFFFFu && !(flags & 0x11))
        {
            Note("living world: nav: cell (%u, %u) rebuilt in %llu ms (section %08X -> %08X, flags %X)", g_lwNav.watch[w].cx, g_lwNav.watch[w].cy,
                (unsigned long long)(GetTickCount64() - g_lwNav.watch[w].at), g_lwNav.watch[w].section, section, flags);
            g_lwNav.watch[w] = g_lwNav.watch[--g_lwNav.watching];
        }
        else w++;
    }
    /* One slot that differs from what was written, when no job is in flight */
    int k = 0;
    for (; k < NEAR_TILES_MAX; k++)
        if (want[k] != g_lwNav.applied[k] || (want[k] && (g_lwNav.handle[k] != g_tiles[k].handle ||
            g_lwNav.centre[k][0] != g_tiles[k].centre[0] || g_lwNav.centre[k][1] != g_tiles[k].centre[1]))) break;
    if (k == NEAR_TILES_MAX) return;
    uint32_t rect[2][4]; int rects = 0;
    if (g_lwNav.applied[k] && LwNavRect(g_lwNav.centre[k][0], g_lwNav.centre[k][1], &rect[rects][0], &rect[rects][1], &rect[rects][2], &rect[rects][3])) rects++;
    if (want[k] && LwNavRect(g_tiles[k].centre[0], g_tiles[k].centre[1], &rect[rects][0], &rect[rects][1], &rect[rects][2], &rect[rects][3])) rects++;
    static uint8_t cells[LW_NAV_CELLS_MAX];
    memset(cells, 0, sizeof cells);
    for (int r = 0; r < rects; r++) LwNavCellsOf(nav, size, rect[r][0], rect[r][1], rect[r][2], rect[r][3], cells);
    int busy = EvpU32(nav + 0x94, 1) != 0;
    for (uint32_t x = 0; x < total && !busy; x++) if (cells[x]) { uint32_t fl = LwCellFlags(nav, x, NULL); busy = fl == 0xFFFFFFFFu || (fl & 1); }
    if (busy) { g_lwNav.busyFrames++; return; }
    /* Write: each sample of the rectangles from the tile whose square holds it (after this change), else the coarse one */
    g_lwNav.applied[k] = want[k];
    if (want[k]) { g_lwNav.handle[k] = g_tiles[k].handle; g_lwNav.centre[k][0] = g_tiles[k].centre[0]; g_lwNav.centre[k][1] = g_tiles[k].centre[1]; }
    uint16_t* v = (uint16_t*)data;
    uint32_t written = 0, fromTile = 0;
    for (int r = 0; r < rects; r++)
        for (uint32_t j = rect[r][2]; j <= rect[r][3]; j++)
            for (uint32_t i = rect[r][0]; i <= rect[r][1]; i++)
            {
                float x, y; LwNavPoint(i, j, &x, &y);
                size_t at = (size_t)j * n + i;
                uint16_t out = g_lwNav.orig[at];
                for (int m = 0; m < NEAR_TILES_MAX; m++)
                    if (g_lwNav.applied[m] && g_tiles[m].ground.v != NULL && LwInSquare(x, y, g_lwNav.centre[m]))
                    {
                        float t = NearGroundAt(&g_tiles[m].ground, x, y);
                        if (t < -9000.0f) break;
                        float q = (t - base) * 65535.0f / range + 0.5f;
                        out = q <= 0 ? 0 : q >= 65535.0f ? 65535 : (uint16_t)q; fromTile++;
                        break;
                    }
                v[at] = out; written++;
            }
    LwMarkFn mark = (LwMarkFn)(exe + LW_NAV_MARK);
    float g[4] = {0, 0, 0, 0}; SafeRead(nav + 0x671C, g, 16);
    char list[256]; int len = 0; list[0] = 0;
    for (uint32_t x = 0; x < total; x++)
        if (cells[x])
        {
            uint16_t cx = (uint16_t)(x % wh[0]), cy = (uint16_t)(x / wh[0]);
            float xy[2] = {g[0] + ((float)cx + 0.5f) * (g[2] - g[0]) / wh[0], g[1] + ((float)cy + 0.5f) * (g[3] - g[1]) / wh[1]};
            uint32_t section = 0; LwCellFlags(nav, x, &section);
            mark(id, xy);
            if (g_lwNav.watching < 64) { g_lwNav.watch[g_lwNav.watching].cx = cx; g_lwNav.watch[g_lwNav.watching].cy = cy;
                g_lwNav.watch[g_lwNav.watching].section = section; g_lwNav.watch[g_lwNav.watching].at = GetTickCount64(); g_lwNav.watching++; }
            if (len < (int)sizeof list - 16) len += snprintf(list + len, sizeof list - len, " (%u, %u)", cx, cy);
        }
    Note("living world: nav: tile %d %s: %u samples written (%u from the tile) after %u busy frames; cells marked:%s; G %.1f m", k,
        want[k] ? "in" : "gone", written, fromTile, g_lwNav.busyFrames, list, (double)(g_lwWallReach ? *g_lwWallReach : 0));
    g_lwNav.busyFrames = 0;
}

/* ---- the tick (the loader's game-thread tick: in a mission the late update, after the world update) ----------------- */

/* Test instrument (HELLFORGE_LW_TEST_TRACE=1; development only): the local Helldiver's position twice a second, for a
 * run script that steers the Helldiver with the harness's input by its measured heading. */
static void LwTraceTick(void)
{
    static int on = -1;
    static ULONGLONG last;
    if (on < 0) { char t[4] = ""; on = GetEnvironmentVariableA("HELLFORGE_LW_TEST_TRACE", t, sizeof t) > 0 && t[0] == '1'; }
    ULONGLONG now = GetTickCount64();
    if (!on || now - last < 500) return;
    last = now;
    ActHealth h; const char* why; float p[3];
    if (ActLocalAvatar(g_lw.game, 0, &h, &why) && h.unit && EvpUnitPosition(h.unit, p))
        Note("living world: test: trace %.1f %.1f %.1f", (double)p[0], (double)p[1], (double)p[2]);
}

static struct { double sum, worst; uint32_t ticks; ULONGLONG at; } g_lwCost;

static void LivingTickBody(void);
static void LivingTick(void)
{
    if (!LwEnabled()) return;
    LARGE_INTEGER a, b, f;
    QueryPerformanceCounter(&a);
    LivingTickBody();
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    g_lwCost.sum += ms; g_lwCost.ticks++; if (ms > g_lwCost.worst) g_lwCost.worst = ms;
    ULONGLONG now = GetTickCount64();
    if (!g_lwCost.at) g_lwCost.at = now;
    if (now - g_lwCost.at >= 30000 && g_lwWorld.count > 0)
    {
        Note("living world: cost: %.3f ms a tick on average, %.2f ms at worst, over %u ticks (%d squads)", g_lwCost.sum / g_lwCost.ticks, g_lwCost.worst,
            g_lwCost.ticks, g_lwWorld.count);
        g_lwCost.sum = g_lwCost.worst = 0; g_lwCost.ticks = 0; g_lwCost.at = now;
    }
}

static void LivingTickBody(void)
{
    LwPatrolsOff();
    int host = 0;
    if (!ActMission(g_lw.game, &host, NULL) || !host) return;
    LwTraceTick();
    uintptr_t dir;
    if (EvpPtr(g_lw.game + LW_DIRECTOR, &dir)) LwNavTick(dir);
    LwGarrisonsTick();
    LwSquadsTick();
}
