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

static void LwNavTypeDump(void); /* test data dump, below */
static void LwSmashTableUpdate(void); /* obstacle smashes spare squad mates, below */
static volatile LONG g_lwSpared, g_lwBlastSpared;
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
#define LW_NODES_MAX 8192
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

/* The walking grid (audit 1e, criterion 7): on an extra large map the level's route graph is sparse (576-610 nodes,
 * 500-700 m apart, over a 7-7.8 km disc), so offline squads, which live on routes, gathered on a few lines and a player
 * elsewhere had none near for minutes. The level's own height plan (g_near.global, the 4096 x 4096 capture the XL
 * terrain is built from; 4 m a sample; NearGlobalAt, world metres) gives the ground everywhere, so the copy gains a
 * grid of nodes LW_GRID apart inside the disc where the ground is above the highest water + 1 m and no steeper than
 * LW_GRID_SLOPE over 8 m, joined to its 8 neighbours where the straight line between them stays walkable (every 8 m step
 * within the slope, above water), and each level node joined to its nearest grid node within LW_GRID of a walkable line.
 * Squads then start spread evenly over the disc, at the density LwPopulate sets (vanilla's cap within R_on), and walk
 * locally (LwPlan): no squad is steered toward a player. Offline only: online squads still take goals only at nodes
 * whose navmesh exists (LwNodeHasFace). */
#define LW_GRID 200.0f
#define LW_GRID_SLOPE 0.70f /* rise over run: 35 deg */
static int LwGridWalkable(float x, float y, float water)
{
    float h = NearGlobalAt(x, y);
    if (h < -9000.0f || h < water + 1.0f) return 0;
    float hx = NearGlobalAt(x + 8.0f, y), hy = NearGlobalAt(x, y + 8.0f);
    return hx > -9000.0f && hy > -9000.0f && fabsf(hx - h) <= 8.0f * LW_GRID_SLOPE && fabsf(hy - h) <= 8.0f * LW_GRID_SLOPE;
}
static int LwGridLine(float ax, float ay, float bx, float by, float water)
{
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    int steps = (int)(len / 8.0f) + 1;
    float prev = NearGlobalAt(ax, ay);
    for (int k = 1; k <= steps; k++)
    {
        float t = (float)k / (float)steps, h = NearGlobalAt(ax + dx * t, ay + dy * t);
        if (h < -9000.0f || h < water + 0.5f || fabsf(h - prev) > (len / (float)steps) * LW_GRID_SLOPE) return 0;
        prev = h;
    }
    return 1;
}
static void LwLink(int a, int b)
{
    LwNode* na = &g_lwNodes[a]; LwNode* nb = &g_lwNodes[b];
    for (int k = 0; k < na->degree; k++) if (na->next[k] == b) return;
    if (na->degree >= 16 || nb->degree >= 16) return;
    na->next[na->degree++] = (uint16_t)b; nb->next[nb->degree++] = (uint16_t)a;
}
/* 1 when the grid is added (or not needed: a vanilla disc); 0 to wait (the height plan of this level not copied yet). */
static int LwGridRoutes(float radius)
{
    if (!(radius > 0)) { LwWhy("no playable radius yet (the walking grid needs it)"); return 0; }
    if (radius < 1500.0f) return 1; /* vanilla discs: the level's routes cover them */
    if (g_near.global == NULL || g_near.level != g_world.levels || g_near.columns != 4096 || g_near.rows != 4096) { LwWhy("the extra large map's height plan is not copied yet (the walking grid waits for it)"); return 0; }
    float water = g_nearWater.level == g_world.levels ? g_nearWater.high : -500.0f;
    enum { G = 96 };
    static int16_t cell[G][G];
    int base = g_lwNodeCount, half = (int)(radius / LW_GRID);
    if (half > G / 2 - 1) half = G / 2 - 1;
    for (int r = 0; r < G; r++) for (int c = 0; c < G; c++) cell[r][c] = -1;
    for (int r = -half; r <= half; r++) for (int c = -half; c <= half; c++)
    {
        float x = (float)c * LW_GRID, y = (float)r * LW_GRID;
        if (x * x + y * y > (radius - 50.0f) * (radius - 50.0f) || g_lwNodeCount >= LW_NODES_MAX || !LwGridWalkable(x, y, water)) continue;
        LwNode* o = &g_lwNodes[g_lwNodeCount]; memset(o, 0, sizeof *o); o->x = x; o->y = y;
        cell[r + G / 2][c + G / 2] = (int16_t)g_lwNodeCount++;
    }
    int edges = 0, joined = 0;
    static const int dr[4] = {0, 1, 1, 1}, dc[4] = {1, 0, 1, -1};
    for (int r = 0; r < G; r++) for (int c = 0; c < G; c++)
    {
        int a = cell[r][c]; if (a < 0) continue;
        for (int k = 0; k < 4; k++)
        {
            int r2 = r + dr[k], c2 = c + dc[k];
            if (r2 < 0 || c2 < 0 || r2 >= G || c2 >= G || cell[r2][c2] < 0) continue;
            int b = cell[r2][c2];
            if (LwGridLine(g_lwNodes[a].x, g_lwNodes[a].y, g_lwNodes[b].x, g_lwNodes[b].y, water)) { LwLink(a, b); edges++; }
        }
    }
    for (int i = 0; i < base; i++)
    {
        int best = -1; float bd = LW_GRID * LW_GRID * 2.0f;
        int c0 = (int)floorf(g_lwNodes[i].x / LW_GRID + 0.5f) + G / 2, r0 = (int)floorf(g_lwNodes[i].y / LW_GRID + 0.5f) + G / 2;
        for (int r = r0 - 1; r <= r0 + 1; r++) for (int c = c0 - 1; c <= c0 + 1; c++)
        {
            if (r < 0 || c < 0 || r >= G || c >= G || cell[r][c] < 0) continue;
            float ex = g_lwNodes[cell[r][c]].x - g_lwNodes[i].x, ey = g_lwNodes[cell[r][c]].y - g_lwNodes[i].y, d = ex * ex + ey * ey;
            if (d < bd && LwGridLine(g_lwNodes[i].x, g_lwNodes[i].y, g_lwNodes[cell[r][c]].x, g_lwNodes[cell[r][c]].y, water)) { bd = d; best = cell[r][c]; }
        }
        if (best >= 0) { LwLink(i, best); joined++; }
    }
    Note("living world: walking grid: %d nodes %.0f m apart over the %.0f m disc (%d edges; %d of the level's %d route nodes joined to it; water %.1f m)",
        g_lwNodeCount - base, (double)LW_GRID, (double)radius, edges, joined, base, (double)water);
    return 1;
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
#define LW_SQUADS_MAX 8192
#define LW_PATH_MAX 32
#define LW_R_ON 250.0f
#define LW_R_OFF 350.0f
#define LW_SIGHT 1700.0f      /* players farther than this are not checked for sight: the largest drawn distance through a 4x zoom (LwFarBand, r 8 m: 1630 m) */
/* Sightlines (user 2026-11-06; KB living-world-plan "Phase 1 addition: squads online by sightline", subagent viewdist):
 * the render cull (exe 0x57B9B0) has no hard distance: an object is drawn while its angular size exceeds
 * FOV (min_size + min_size_far t^2), so on "high" a 2 m enemy is drawn to ~530 m, ~600-750 m through a scope; AI and
 * replication have no distance cut. Vanilla has no enemies 400-600 m from every player (patrols removed beyond 300 m,
 * 0x95288B; garrisons fill within 150 m). A squad 250-600 m away comes online, still hidden, when the next stretch of its
 * route is in a player's view (D), and stays online while a player within 650 m sees it (D); far squads use at most
 * 40 % of the budget (D), so the near ones keep theirs. */
#define LW_BOOST_FAR 900.0f   /* nav cells within this of a player are boosted too, every 4th tick (D) */
#define LW_FAR_MARGIN 50.0f   /* a squad seen within its band + this stays online (D) */
#define LW_RADIUS_DEFAULT 2.0f /* a group's unit radius until one of its units has been measured (D: a 2 m sphere, 811 m) */
#define LW_FAR_SHARE 0.5f     /* far squads use at most half the budget (audit 1e, 6: 0.4 x 39 left room for one squad of 8) */
#define LW_R_MIN 150.0f       /* never spawned nearer a player (D): vanilla spawns patrols 75-130 m away */
#define LW_ARRIVE 15.0f
#define LW_FAR_MS 10000       /* beyond R_off and calm this long before a squad goes offline (D) */
#define LW_SPEED_DEFAULT 2.0f /* m/s until a squad's own walking speed was seen online (D) */
#define LW_RESERVE 12u        /* the 70 room's share kept for garrison top-ups and SEAF (KB "Spawn gates and the online budget") */
enum { LW_FREE, LW_OFFLINE, LW_PENDING, LW_ONLINE };
enum { LW_PATROL = 0, LW_INVESTIGATE = 1, LW_REINFORCE = 2, LW_RETAKE = 3 }; /* a squad's intent (Phase 2, living_world_intent.c) */
#define LW_RUSH 1.6f /* a reinforcing squad's pace x its walking speed (D) */
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
    uint8_t checkedCount, appearSeen; float appearLo, appearHi, appearGap; uint32_t checked[32]; float checkedAt[32][2]; /* arrival: members checked in their first 5 s */
    uint8_t farOn, inView; /* farOn: came online by sightline; inView: a player sees it (checked at lookAt) */
    uint8_t forceOff; /* test command: go offline now */
    ULONGLONG lookAt;
    ULONGLONG retry; /* no request before this (placement or sight refused) */
    uint32_t groundId[32]; float groundAt[32][3]; /* each member's last 5 s sample on the ground (the fall note) */
    float band; uint32_t bandGen; uintptr_t bandDef; /* LwSquadBand's cache */
    uint8_t pulled;                 /* 2: held in place by the test stage until it comes online (the only value) */
    uint8_t intent, intentStage, intentSent; /* Phase 2 (living_world_intent.c): LW_PATROL / LW_INVESTIGATE; 1 = at the point; goal sent online */
    float target[3]; uint32_t intentSource, intentSearch; ULONGLONG intentAt, intentStageAt, intentFree;
    uint32_t intentPeak; float intentPeakAt, intentFrom; /* members (bit = seen index) at alert 2+ (REINFORCE: 3+) at the point; seconds to the first; m from the point when sent */
    int16_t garrison; /* RETAKE: its g_lwGarrisons index */
    uint16_t home1; /* 2d: its home region + 1 (0 = none yet) */
    int8_t fight; uint8_t rushed, tells; ULONGLONG retargetAt, tellAt; /* REINFORCE: its fight (g_lwFights index); members sped up (0x7EFFC0); fight target tells; next goal refresh, next tell */
} LwSquad;
static int LwIntentArrived(LwSquad* q); static int LwIntentGoal(LwSquad* q); static void LwIntentOnline(LwSquad* q, const uint32_t* members, int n, const float* at);
static void LwIntentOffline(LwSquad* q); static int LwTerritoryGoalOk(const LwSquad* q, int node); static void LwTerritoryTick(void); static void LwIntentTrace(void); static void LwRetakeScan(void); static void LwTestClear(int seq); static void LwRetakeWait(int seq, float s); static void LwNoiseDrain(void); static void LwHearNote(void); static void LwFightScan(void); static void LwRushNote(void); static void LwTestNoise(int seq, int type, float dx, float dy);
static LwSquad g_lwSquads[LW_SQUADS_MAX];
static struct
{
    uintptr_t dir, lg; uint32_t seed;
    int count, populated, online, onlineUnits, eff, playersAt;
    float players[EVT_MAX_PLAYERS][3], radius, rMin; /* feet position (x, y, z); rMin: this map's least spawn distance */
    uint32_t units[EVT_MAX_PLAYERS], knownUnit[EVT_MAX_PLAYERS]; /* each player's Helldiver unit (the sight ray leaves it out) */
    uint32_t avatars[EVT_MAX_PLAYERS], knownAvatar[EVT_MAX_PLAYERS]; uint8_t local[EVT_MAX_PLAYERS], knownLocal[EVT_MAX_PLAYERS]; /* the avatar entity (its aim, LwAimForward); this machine's player */
    float known[EVT_MAX_PLAYERS][3]; ULONGLONG knownAt[EVT_MAX_PLAYERS]; uint64_t knownPeer[EVT_MAX_PLAYERS];
    ULONGLONG last, effAt, noteAt;
    uint32_t spawned, offlined, died, refused, farSpawned, retook; /* retook: squads that became a cleared garrison (2c) */
    int farUnits;
    uint32_t noFace, moved, overhead, seen, rays, blockUnit, blockTerrain; double rayUs; /* placement and sight refusals, rays cast (30 s) */
    uint32_t crowded; /* points where a member's slot cast would not reach its slot (30 s) */
    uint32_t goals, goalNoFace, goalReplans, goalStuck; /* goals sent, next nodes without a face within 25 m, replans, none found (30 s) */
    uint32_t heights, underGround, overGround; float worstUnder, worstOver; /* members against the drawn ground (30 s) */
    uint32_t building, undrawn; /* points refused: in the cell being built; the ground hit not the drawn ground (30 s) */
    int popMade, popPass, popTries, popTicks; float popNearest; double popMs; /* the populate, resumed each tick (LW_POP_MS) */
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
            if (slot >= 0) { g_lwWorld.known[slot][0] = at[0]; g_lwWorld.known[slot][1] = at[1]; g_lwWorld.known[slot][2] = at[2]; g_lwWorld.knownUnit[slot] = h.unit; g_lwWorld.knownAvatar[slot] = list[i].avatar; g_lwWorld.knownLocal[slot] = (uint8_t)(list[i].local != 0); g_lwWorld.knownAt[slot] = now; g_lwWorld.knownPeer[slot] = list[i].peer; }
        }
    }
    for (int j = 0; j < EVT_MAX_PLAYERS; j++)
        if (g_lwWorld.knownAt[j] && now - g_lwWorld.knownAt[j] <= LW_KNOWN_MS) { memcpy(g_lwWorld.players[k], g_lwWorld.known[j], sizeof g_lwWorld.players[k]); g_lwWorld.units[k] = g_lwWorld.knownUnit[j]; g_lwWorld.avatars[k] = g_lwWorld.knownAvatar[j]; g_lwWorld.local[k] = g_lwWorld.knownLocal[j]; k++; }
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

/* A new destination: a random node inside the playable disc, and the path there. On a disc wider than LW_ROAM a node
 * within LW_ROAM of the squad (audit 1e, 7: on XL a squad brought online near a player walked off toward a node anywhere
 * on the 7.8 km disc; squads that roam locally keep the even spread they start with). */
#define LW_ROAM 1200.0f
static void LwPlan(LwSquad* q)
{
    int local = g_lwWorld.radius > LW_ROAM;
    int home = LwTerritoryGoalOk(q, -1) && LwUnit() < 0.7f; /* 2d: most goals stay in the home region or a neighbour (D) */
    for (int tries = 0; tries < (local ? 24 : 8) * (home ? 2 : 1); tries++)
    {
        int goal = (int)(LwRand() % (uint32_t)g_lwNodeCount);
        if (home && tries < (local ? 24 : 8) && !LwTerritoryGoalOk(q, goal)) continue; /* then any goal, so the squad never stands still */
        float r = g_lwWorld.radius;
        if (g_lwNodes[goal].x * g_lwNodes[goal].x + g_lwNodes[goal].y * g_lwNodes[goal].y > r * r) continue;
        if (local && LwDist2(q->pos, g_lwNodes[goal].x, g_lwNodes[goal].y) > LW_ROAM * LW_ROAM) continue;
        int n = LwPath(q->node, goal, q->path, LW_PATH_MAX);
        if (n > 0) { q->pathLen = (uint8_t)n; q->pathAt = 0; return; }
    }
    q->pathLen = 0; q->pathAt = 0;
}

/* The world's squads for this mission: on route nodes inside the playable disc, away from the players. */
#define LW_POP_MS 2.0 /* the populate's time a tick */
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
    /* Spread over ticks (audit 1f pass caveat 10: 6981 squads in one tick made a 56-67 ms frame after the landing):
     * placement goes on where the last tick stopped until LW_POP_MS is spent. The squads are drawn uniformly, and the
     * world stays unpopulated (no squad ticks) until the last is placed, so nothing sees a part of it. */
    if (g_lwWorld.popTicks == 0) { g_lwWorld.popNearest = 3e38f; g_lwWorld.popMade = 0; g_lwWorld.popPass = 0; g_lwWorld.popTries = 0; }
    g_lwWorld.popTicks++;
    LARGE_INTEGER t0, t1, hz; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&hz);
    int made = g_lwWorld.popMade; float nearest = g_lwWorld.popNearest; uint32_t steps = 0;
    /* Start beyond R_off from every player; on a map whose playable disc is smaller than that (lw7b: 292 m, no node
     * 350 m from the drop) beyond R_min instead, so such squads come online at once at vanilla-like distances. */
    /* The least spawn distance fits the map (D, lw15p: a 156 m disc had no node 150 m from the drop, so 0 squads):
     * half the playable radius, at least vanilla's 75 m (colony / city patrols) and at most LW_R_MIN. */
    g_lwWorld.rMin = fminf(LW_R_MIN, fmaxf(75.0f, 0.5f * g_lwWorld.radius));
    /* Two passes (lw32a: a 350 m disc = LW_R_OFF made 0 squads: no node lay 350 m from a player in the middle): beyond
     * R_off first, then what that pass could not place beyond R_min. */
    /* Each squad starts at a random point of a random route edge, walking to the edge's far end (audit 1c criterion 8:
     * on nodes only, XL's 576-591 nodes 500-700 m apart held about 10 squads each, none near the drop: the first squad
     * came online after 2-3 minutes, and they arrived one after another at the same points). Its route on from there
     * is planned when it gets there (LwWalk), not here (the populate's 6144 path searches made a 60-79 ms frame). */
    for (; g_lwWorld.popPass < 2; g_lwWorld.popPass++, g_lwWorld.popTries = 0)
    /* On an extra large map (walking grid) the squads start evenly from R_min out, as they stand everywhere else on the
     * disc (audit 1e, 7: started beyond R_off, the first squads walked in over 2 minutes; lw83a). */
    for (float least = g_lwWorld.popPass == 0 && g_lwWorld.radius < 1500.0f ? LW_R_OFF : g_lwWorld.rMin; made < n && g_lwWorld.popTries < n * 20; g_lwWorld.popTries++)
    {
        if ((++steps & 63) == 0)
        {
            QueryPerformanceCounter(&t1);
            if ((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart > LW_POP_MS)
            {
                g_lwWorld.popMade = made; g_lwWorld.popNearest = nearest;
                g_lwWorld.popMs += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart;
                return; /* on next tick */
            }
        }
        int node = (int)(LwRand() % (uint32_t)g_lwNodeCount);
        if (g_lwNodes[node].degree == 0) continue;
        int to = g_lwNodes[node].next[LwRand() % g_lwNodes[node].degree];
        float t = LwUnit(), x = g_lwNodes[node].x + t * (g_lwNodes[to].x - g_lwNodes[node].x), y = g_lwNodes[node].y + t * (g_lwNodes[to].y - g_lwNodes[node].y), r = g_lwWorld.radius;
        if (x * x + y * y > r * r || LwNearestPlayer2(x, y) < least * least) continue;
        float pos[2] = {x, y};
        uintptr_t def = LwChooseGroup(dir, g_lwWorld.lg, pos);
        uintptr_t slot = def ? LwSlotOf(dir, def) : 0;
        if (slot == 0) continue;
        LwSquad* q = &g_lwSquads[made++];
        memset(q, 0, sizeof *q);
        q->state = LW_OFFLINE; q->node = (uint16_t)node; q->pos[0] = x; q->pos[1] = y; q->definition = def;
        q->survivors = (uint8_t)LwGroupSize(slot); q->speed = LW_SPEED_DEFAULT * (0.75f + 0.5f * LwUnit()); /* own pace: no lockstep (audit 1, 11) */
        q->path[0] = (uint16_t)to; q->pathLen = 1; q->pathAt = 0;
        if (LwNearestPlayer2(x, y) < nearest) nearest = LwNearestPlayer2(x, y);
    }
    QueryPerformanceCounter(&t1);
    g_lwWorld.popMs += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart;
    g_lwWorld.count = made; g_lwWorld.eff = eff; g_lwWorld.populated = 1;
    Note("living world: %d squads on %d route nodes (patrol cap %d units, mean group %.1f, playable radius %.0f m, online within %.0f m); the nearest starts %.0f m from a player; placed over %d ticks, %.1f ms",
        made, g_lwNodeCount, eff, (double)mean, (double)g_lwWorld.radius, (double)LW_R_ON, (double)sqrtf(nearest), g_lwWorld.popTicks, g_lwWorld.popMs);
}

/* Offline walking along the path at the squad's speed; a new destination at the end. A staged squad (pulled 2) stays
 * where the test stage put it. */
static void LwWalk(LwSquad* q, float dt)
{
    if (q->pulled) return;
    if (q->pathAt >= q->pathLen) { if (LwIntentArrived(q)) return; LwPlan(q); if (q->pathLen == 0) return; }
    float step = q->speed * (q->intent == LW_REINFORCE && q->intentStage == 0 ? LW_RUSH : 1.0f) * dt; /* reinforcements run (Phase 2b) */
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
static char g_lwRoomWhy[96]; /* the gate that refused the last request (test notes) */
/* Room held for a far squad in a player's sight while a hidden near squad it displaced leaves (LwSwapForFar): other
 * requests count it as used until that squad comes online or the hold lapses (audit 1e, 6: near squads took the room
 * first, so far squads came online only by chance). */
static struct { int idx; uint32_t units; ULONGLONG until; } g_lwHold = {-1, 0, 0};
static int g_lwRequester = -1; /* the squad LwOnline is serving */
static int LwRoomFor(uint32_t members)
{
    uint32_t held = g_lwHold.idx >= 0 && g_lwHold.idx != g_lwRequester && GetTickCount64() < g_lwHold.until ? g_lwHold.units : 0;
    uintptr_t players, chars, jobs;
    if (!EvpPtr(g_lw.game + 0x3326468u, &players)) { snprintf(g_lwRoomWhy, sizeof g_lwRoomWhy, "no player list"); return 0; }
    uint32_t count = EvpU32(players + 0x84, 0), ok = 0;
    for (uint32_t i = 0; i < count && i < 16; i++) { uint32_t f = EvpU32(players + 0x3AC + 0x20u * i, 0xFFFFFFFFu); if (!(f & (1u << 11)) && !(f & (1u << 4))) ok = 1; }
    typedef uint8_t (*EndingFn)(void);
    if (!ok) { snprintf(g_lwRoomWhy, sizeof g_lwRoomWhy, "no player record with bits 4 and 11 clear"); return 0; }
    if (((EndingFn)(g_lw.game + 0x8F32B0u))()) { snprintf(g_lwRoomWhy, sizeof g_lwRoomWhy, "the mission is ending"); return 0; }
    uintptr_t dir = g_lwWorld.dir;
    if (!EvpPtr(g_lw.game + 0x3326D18u, &chars) || !EvpPtr(g_lw.game + LW_AIJOB, &jobs)) { snprintf(g_lwRoomWhy, sizeof g_lwRoomWhy, "no character or job table"); return 0; }
    int room = (int)EvpU32(chars + 0x1C, 999) + (int)EvpU32(dir + 0x36C0, 999) - (int)EvpU32(dir + 0x620, 0) - (int)EvpU32(dir + 0x61C, 0);
    if (room + (int)members + (int)held + (int)LW_RESERVE > 70)
    { snprintf(g_lwRoomWhy, sizeof g_lwRoomWhy, "the game's 70 room: %d used + %u + reserve %d%s", room, members, (int)LW_RESERVE, held ? " + held for a far squad" : ""); return 0; }
    uint32_t active = EvpU32(jobs + 0xC, 9999);
    if (active + members + 1 >= 448 - 32) { snprintf(g_lwRoomWhy, sizeof g_lwRoomWhy, "AiJob: %u active + %u", active, members); return 0; }
    int ceiling = g_lwWorld.playersAt > 1 ? 64 : 48;
    if (g_lwWorld.onlineUnits + (int)members + (int)held > g_lwWorld.eff || g_lwWorld.onlineUnits + (int)members + (int)held > ceiling)
    { snprintf(g_lwRoomWhy, sizeof g_lwRoomWhy, "the cap: %d online + %u over %d (ceiling %d)%s", g_lwWorld.onlineUnits, members, g_lwWorld.eff, ceiling, held ? ", room held for a far squad" : ""); return 0; }
    return 1;
}

static int LwGround(float x, float y, float* z);
static int g_lwTestVisible; /* test command "spawn visible": the point must be in a player's sight instead of out of it */
static uint32_t g_lwTestUnseen; /* "spawn visible" points no player sees */
static int LwSeenByAny(const float* p, float range);
static int LwPlace(uintptr_t slot, float* pos);
static int LwHidden(const float* p);
static int LwHiddenFar(const float* p);
static int LwSeenFromView(int i, const float* point);
static int g_lwFarSpawn; /* LwOnline: a far (sightline) squad's request */
static int LwMembersFit(uintptr_t slot, const float* pos, uint32_t n);

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

/* Test notes: why a staged squad (pulled 2) is not online yet, noted when the reason changes (and every 30 s while it
 * stays), so a run shows the far rule's own gate instead of a silent wait. */
static void LwStagedWhy(LwSquad* q, const char* why, float near2)
{
    static char last[128]; static int lastIdx = -1; static ULONGLONG lastAt;
    ULONGLONG now = GetTickCount64();
    int idx = (int)(q - g_lwSquads);
    if (idx == lastIdx && strcmp(last, why) == 0 && now - lastAt < 30000) return;
    lastIdx = idx; lastAt = now; snprintf(last, sizeof last, "%s", why);
    Note("living world: test: staged squad %d (%.0f m, %d units online of %d, far %d): not online: %s", idx, (double)sqrtf(near2),
        g_lwWorld.onlineUnits, g_lwWorld.eff, g_lwWorld.farUnits, why);
}

static int LwHidden(const float* p);
static uint32_t LwAlert(uint32_t unit);
static int LwMembers(uint32_t squad, uint32_t* out);
static int LwLeader(const uint32_t* members, int n, float* at);
/* A far squad in a player's sight refused for room (the game's 70 room or the budget): the online near squad that no
 * player sees and whose members are all calm, farthest from every player, goes offline with its survivors (as the
 * distance rule takes squads offline) and stays offline a minute; the far squad's room is held for it meanwhile. Near
 * squads out of every player's sight are the world's filler: a squad a player can see takes their place (audit 1e, 6).
 * At most one swap every 5 s. */
static int LwSwapForFar(LwSquad* fq)
{
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (now - last < 5000) return 0;
    LwSquad* pick = NULL; float pickD = -1; float pickAt[2] = {0, 0};
    for (int i = 0; i < g_lwWorld.count; i++)
    {
        LwSquad* q = &g_lwSquads[i];
        if (q == fq || q->state != LW_ONLINE || q->farOn || q->forceOff || q->alive == 0) continue;
        uint32_t members[32]; int n = LwMembers(q->squad, members); float at[3];
        if (n <= 0 || !LwLeader(members, n, at)) continue;
        int calm = 1;
        for (int k = 0; k < n && calm; k++) if (LwAlert(members[k]) != 0) calm = 0;
        if (!calm) continue;
        float d2 = LwNearestPlayer2(at[0], at[1]);
        if (d2 <= pickD) continue;
        at[2] = 0;
        if (!LwGround(at[0], at[1], &at[2]) || !LwHidden(at)) continue; /* a player sees where it stands */
        pick = q; pickD = d2; pickAt[0] = at[0]; pickAt[1] = at[1];
    }
    last = now;
    if (pick == NULL) return 0;
    pick->forceOff = 2;
    g_lwHold.idx = (int)(fq - g_lwSquads); g_lwHold.units = fq->survivors; g_lwHold.until = now + 20000;
    Note("living world: squad %d (hidden, calm, %.0f m from the nearest player) goes offline to make room for squad %d in a player's sight %.0f m away",
        (int)(pick - g_lwSquads), (double)sqrtf(pickD), g_lwHold.idx, (double)sqrtf(LwNearestPlayer2(fq->pos[0], fq->pos[1])));
    return 1;
}

static const char* g_lwOnlineWhy; /* test notes: the step of LwOnline that refused the last request */
static void LwOnline(LwSquad* q)
{
    g_lwOnlineWhy = NULL; g_lwRequester = (int)(q - g_lwSquads);
    uintptr_t slot = LwSlotOf(g_lwWorld.dir, q->definition);
    if (slot == 0)
    {
        q->definition = LwChooseGroup(g_lwWorld.dir, g_lwWorld.lg, q->pos);
        slot = q->definition ? LwSlotOf(g_lwWorld.dir, q->definition) : 0;
        if (slot == 0) { g_lwOnlineWhy = "no group slot"; return; }
    }
    uint32_t cap = q->survivors, size = LwGroupSize(slot);
    if (cap > size) cap = size;
    if (cap == 0 || !LwRoomFor(cap)) { g_lwOnlineWhy = cap ? g_lwRoomWhy : "no survivors"; g_lwWorld.refused++; return; }
    float pos[3] = {q->pos[0], q->pos[1], 0}, target[3] = {q->pos[0], q->pos[1], 0};
    if (q->pathAt < q->pathLen) { target[0] = g_lwNodes[q->path[q->pathAt]].x; target[1] = g_lwNodes[q->path[q->pathAt]].y; }
    if (!LwGround(pos[0], pos[1], &pos[2]) || !LwGround(target[0], target[1], &target[2])) { snprintf(g_lwRoomWhy, sizeof g_lwRoomWhy, "no ground under the point or its target"); g_lwOnlineWhy = g_lwRoomWhy; g_lwWorld.refused++; return; }
    float wanted[2] = {pos[0], pos[1]};
    if (!LwPlace(slot, pos)) { g_lwOnlineWhy = "placement (LwPlace)"; q->retry = GetTickCount64() + 3000; return; }
    if (!LwMembersFit(slot, pos, cap)) { g_lwOnlineWhy = "a member would not reach its slot"; q->retry = GetTickCount64() + 3000; return; }
    if (g_lwTestVisible ? !LwSeenByAny(pos, LW_SIGHT) && (++g_lwTestUnseen, 1) : !(g_lwFarSpawn ? LwHiddenFar(pos) : LwHidden(pos))) { g_lwOnlineWhy = g_lwFarSpawn ? "sight: seen on the host's screen (or no off-screen read)" : "sight: seen"; q->retry = GetTickCount64() + 3000; return; }
    uintptr_t rec = g_lw.spawn(g_lwWorld.dir, slot, pos, LW_JOB_PATROL, LW_PATROL_FLAGS, target, NULL, NULL, 0.0f, cap);
    if (rec == 0) { snprintf(g_lwRoomWhy, sizeof g_lwRoomWhy, "the spawn request returned no record"); g_lwOnlineWhy = g_lwRoomWhy; g_lwWorld.refused++; return; }
    uint8_t used = 0;
    if (SafeRead(rec + 0x12, &used, 1) || used != 1) { snprintf(g_lwRoomWhy, sizeof g_lwRoomWhy, "the spawn queue was full"); g_lwOnlineWhy = g_lwRoomWhy; g_lwWorld.refused++; return; } /* the queue was full: nothing queued */
    Note("living world: squad %d requested at (%.0f, %.0f, %.1f) (snapped %.1f m), %.0f m from the nearest player, out of sight, %u of %u members (group 0x%llx)",
        (int)(q - g_lwSquads), (double)pos[0], (double)pos[1], (double)pos[2], (double)sqrtf(LwDist2(wanted, pos[0], pos[1])),
        (double)sqrtf(LwNearestPlayer2(pos[0], pos[1])), cap, size, (unsigned long long)q->definition);
    q->reqZ = pos[2]; q->arrived = 0; q->viewed = 0; q->checkedCount = 0; q->appearSeen = 0; q->appearGap = 1e9f; q->farOn = 0; q->inView = 0; q->lookAt = 0;
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
    /* Extra large levels with moving tiles (audit 1g, 9; KB living-world-tile-switch-falls section 6): lw92a's squad
     * 1913 was requested in the square of the cell being built (its tile in 10 s later, the coarse lowered under its
     * members) at the navmesh face's height (87.5), while the collision under it was missing. So no point in the
     * building cell's square; the ground hit must be the drawn ground (the live tile's, else the coarse rows) within
     * 0.5 m; and the request goes at the hit's height, not the face's. */
    if (MoveTilesOn())
    {
        int tile; float drawn = MoveDrawnGround(p[0], p[1], &tile);
        if (MoveCellBuilding(p[0], p[1])) { g_lwWorld.building++; return 0; }
        if (drawn < -9000.0f || fabsf(top - drawn) > 0.5f) { g_lwWorld.undrawn++; return 0; }
        p[2] = top;
    }
    memcpy(pos, p, sizeof p);
    return 1;
}

/* A family's possible units (lw40b: squad 11's 8 members all stood on its request point, "nearest two 0.0 m", twice,
 * though LwMembersFit had passed it with the slot's layer and 2.52 m). Checked in the dump: the squad create's member
 * loop (0x801F80..0x8029DF) takes, per family (slot +12k: hash, +4 count; [slot+0x60] families), each member's unit
 * from 0x953150(dir, hash, table = [dir+0x660] (the loader's requests carry no spawn data: [data+0x10] = 0), 0): a
 * WEIGHTED RANDOM pick (the game's LCG 0x3483C38) among the table's entries (stride 0x80, count [table+8]) with
 * [e] == hash, tag test 0x177E3C0(dir+0x518D4, e+0x58) and weight [e+disp+4(tech level [dir+0x518C4] - 1)] > 0 (disp
 * 0x30, 0x2C with difficulty 11's moved weights); then
 * resource = 0x949B80(&out, 0, e, -1, 0) (a pure lookup by difficulty), and per member layer = [0x505B40(res)+4]
 * (0 without), radius = min(2.4, [0x505B40(res)+8] when > 0, else [0x4F95C0(res)+0x10]) x 1.05, else 0.5
 * (0x8025A5..0x8025EE). The layer filters the cast's start face (exe 0x7D8D00: 0x7E6CB0(layer) -> query filter), so a
 * member whose layer has no face at the point stays ON the point. The pick cannot be predicted without moving the
 * game's random state, so the point must suit every unit the pick can give. Up to 8 per family; 0 = none (the game
 * would read entry 0). */
#define LW_VARIANTS 8
typedef struct { float radius; uint32_t layer; } LwVariant;
static int LwVariantsOf(uint32_t hash, LwVariant* out)
{
    typedef char (*TagFn)(uintptr_t tags, uintptr_t want);
    typedef uintptr_t* (*ResFn)(uintptr_t* out, uintptr_t table, uintptr_t entry, int32_t difficulty, uintptr_t tags);
    typedef uintptr_t (*LookFn)(uintptr_t res);
    uintptr_t dir = g_lwWorld.dir, table = 0, base = 0;
    if (!EvpPtr(dir + 0x660, &table) || table == 0 || !EvpPtr(table, &base) || base == 0) return 0;
    uint32_t count = EvpU32(table + 8, 0), diff = EvpU32(dir + 0x518C4, 0);
    /* the weights' offset as the pick reads it now (0x95321B movss xmm0, [rdi+rax*4+disp8]): 0x30, or 0x2C once
     * difficulty 11 has moved every roster row's weights (difficulty.c MoveRosterWeights; lw44a: on XL no unit was
     * found with 0x30 and nothing came online) */
    uint8_t code[6] = {0}; static int told;
    if (SafeRead(g_lw.game + 0x95321Bu, code, 6) || memcmp(code, "\xF3\x0F\x10\x44\x87", 5) != 0)
    { if (!told++) Note("living world: the unit pick's weight read (game 0x95321B) is not the reviewed code: no squad is placed"); return 0; }
    if (diff == 0 || diff > 16 || count > 4096) return 0;
    int n = 0;
    for (uint32_t k = 0; k < count; k++)
    {
        uintptr_t e = base + (uintptr_t)k * 0x80; float w = 0;
        if (EvpU32(e, 0) != hash || SafeRead(e + code[5] + 4u * (diff - 1), &w, 4) || !(w > 0.0f)) continue;
        if (!((TagFn)(g_lw.game + 0x177E3C0u))(dir + 0x518D4, e + 0x58)) continue;
        uintptr_t res = 0;
        ((ResFn)(g_lw.game + 0x949B80u))(&res, 0, e, -1, 0);
        uintptr_t info = res ? ((LookFn)(g_lw.game + 0x505B40u))(res) : 0, alt;
        LwVariant v = {0.5f, 0}; float r = 0;
        if (info) v.layer = EvpU32(info + 4, 0);
        if (info && !SafeRead(info + 8, &r, 4) && r > 0.0f) v.radius = (r < 2.4f ? r : 2.4f) * 1.05f;
        else if (res && (alt = ((LookFn)(g_lw.game + 0x4F95C0u))(res)) != 0 && !SafeRead(alt + 0x10, &r, 4)) v.radius = (r < 2.4f ? r : 2.4f) * 1.05f;
        int same = 0;
        for (int j = 0; j < n; j++) if (out[j].layer == v.layer && out[j].radius == v.radius) same = 1;
        if (!same && n < LW_VARIANTS) out[n++] = v;
    }
    return n;
}

/* Every member's place (audit 1b criterion 6; KB living-world-spawn-losses, checked by me: 0x800270, 0x80253E..0x8025A4,
 * 0x80283F..0x8028DC, 5.0 at 0x23C74A4, 1.05 at 0x23C6DBC): the squad create puts member i (of n = the families'
 * summed counts, capped) at the request point plus r (cos a, sin a, 0), r = 5 sqrt((i + 0.5) / (n - 0.5)),
 * a = (i + 1) 2 pi / 2.618034 (none for n = 1), i counting across families, then casts along the navmesh from the
 * point to that slot: exe 0x7D8C00 = [[game+0x3326330]+0x18](u32 [[game+0x346D508]], from, to, its radius, its layer,
 * out prefilled with the point). A cast that fails or stops at once leaves the member ON the point, and members are
 * created with no overlap test (lw34d, lw36a: members inside each other died within seconds). The point is used only
 * when, for every member and every unit its family's pick can give (LwVariantsOf), the cast reaches the slot within
 * 0.1 m and that slot passes LwPlace's ground check (nothing over it within 2 m). 1 when all fit. */
static uint32_t g_lwVariantNoted[16];
static int LwMembersFit(uintptr_t slot, const float* pos, uint32_t n)
{
    typedef uint64_t (*CastFn)(uint32_t world, const float* from, const float* to, float radius, uint32_t layer, float* out);
    uintptr_t table = 0, fn = 0, wp = 0;
    uint32_t world = 0;
    if (n <= 1) return 1;
    if (!EvpPtr(g_lw.game + 0x3326330u, &table) || !EvpPtr(table + 0x18, &fn) || fn == 0 || !EvpPtr(g_lw.game + 0x346D508u, &wp) || wp == 0 ||
        SafeRead(wp, &world, 4)) return 0;
    uint32_t families = EvpU32(slot + 0x60, 0), i = 0;
    if (families > 8) return 0;
    for (uint32_t f = 0; f < families && i < n; f++)
    {
        uint32_t hash = EvpU32(slot + 12u * f, 0), count = EvpU32(slot + 12u * f + 4, 0);
        LwVariant v[LW_VARIANTS];
        int nv = count ? LwVariantsOf(hash, v) : 0;
        if (count && nv == 0) { g_lwWorld.crowded++; return 0; }
        int noted = 0; /* once per family hash: the units its pick can give, for the run notes */
        for (int k = 0; k < 16 && !noted; k++) if (g_lwVariantNoted[k] == hash) noted = 1;
        if (!noted && count)
        {
            for (int k = 0; k < 16; k++) if (g_lwVariantNoted[k] == 0) { g_lwVariantNoted[k] = hash; break; }
            char line[256]; int at = 0;
            for (int j = 0; j < nv && at < (int)sizeof line - 32; j++) at += snprintf(line + at, sizeof line - at, "%s%.2f m layer %u", j ? ", " : "", (double)v[j].radius, v[j].layer);
            Note("living world: family %08X: its pick gives %d kind%s of unit: %s", hash, nv, nv == 1 ? "" : "s", line);
        }
        for (uint32_t c = 0; c < count && i < n; c++, i++)
        {
            float r = 5.0f * sqrtf(((float)i + 0.5f) / ((float)n - 0.5f)), a = (float)(i + 1) * 6.2831854820f / 2.6180336475f;
            float to[3] = {pos[0] + r * cosf(a), pos[1] + r * sinf(a), pos[2]}, top = NAN;
            if (!LwGround(to[0], to[1], &top)) { g_lwWorld.crowded++; return 0; }
            if (MoveTilesOn()) { int tile; float drawn = MoveDrawnGround(to[0], to[1], &tile); if (drawn < -9000.0f || fabsf(top - drawn) > 0.5f) { g_lwWorld.undrawn++; return 0; } }
            for (int j = 0; j < nv; j++)
            {
                float out[3] = {pos[0], pos[1], pos[2]};
                ((CastFn)fn)(world, pos, to, v[j].radius, v[j].layer, out);
                /* a member starts at the cast's height and its ground ray reaches `up` (1.00 m for these units) over it:
                 * one started deeper under the ground never meets it (audit 1g, 9: the old 2 m tolerance could bury it) */
                if (!isfinite(out[0]) || LwDist2(to, out[0], out[1]) > 0.1f * 0.1f || out[2] < top - 0.9f || out[2] > top + 2.0f) { g_lwWorld.crowded++; return 0; }
            }
        }
    }
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
static int LwRayClear(const float* eye, uint32_t unit, const float* point);
static int LwSeenFrom(const float* feet, uint32_t unit, const float* point)
{
    float eye[3] = {feet[0], feet[1], feet[2] + 1.7f};
    return LwRayClear(eye, unit, point);
}
/* 1 when the static-only sight ray from `eye` reaches `point` (0.5 m short counts), 0 when blocked, -1 with no ray. */
static int LwRayClear(const float* eye, uint32_t unit, const float* point)
{
    typedef uint32_t (*RayFn)(uint32_t handle, const float* from, const float* dir, float length, uint32_t leaveOut, void* hit, uint32_t max);
    uintptr_t table = 0, fn = 0;
    uint32_t handle = LwSightRay(&table);
    if (handle == 0 || !EvpPtr(table + 0x68, &fn) || fn == 0) return -1;
    float d[3] = {point[0] - eye[0], point[1] - eye[1], point[2] - eye[2]};
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
/* Out of sight: from every player within LW_SIGHT, a unit's chest (1.0 m), head-over (2.5 m) and a tall unit's top (4.5 m)
 * all blocked, measured from the higher of the navmesh face and the ground ray's top surface: members stand on the
 * drawn ground, which LwPlace allows up to 2 m over the face (audit 1b criterion 4: on XL, whose faces lie on a 4 m
 * grid, members appeared 1-2.3 m over the requested face height and in a player's sight). */
static int LwHidden(const float* p)
{
    float top = NAN, base = p[2];
    if (LwGround(p[0], p[1], &top) && isfinite(top) && top > base) base = top;
    for (int i = 0; i < g_lwWorld.playersAt; i++)
    {
        if (LwDist2(g_lwWorld.players[i], p[0], p[1]) > LW_SIGHT * LW_SIGHT) continue;
        for (int h = 0; h < 3; h++)
        {
            float t[3] = {p[0], p[1], base + (h == 0 ? 1.0f : h == 1 ? 2.5f : 4.5f)};
            int seen = LwSeenFromView(i, t);
            if (seen != 0) { g_lwWorld.seen++; return 0; } /* seen, or no ray to tell: refused */
        }
    }
    return 1;
}

/* Off the host's screen (audit 1c criterion 6: a far squad standing in open view inside its band never came online,
 * because a far spawn had to be out of every player's sight; the user: "squads are invisible even zoomed in, when at a
 * comparable distance standing they would otherwise be visible"). A squad in a player's line of sight may come online
 * while that player's view looks elsewhere, so it is there, drawn to its band, when the view turns to it. Only the
 * host's own view is known: the game camera struct [game+0x346D560] holds a 4x4 pose at +0xC (rows x, y, z, then the
 * position at +0x3C; KB living-world-scope-draw-distance "Address fix") and its +0 the engine Camera, vertical FOV at
 * +0x34 (radians). Forward is taken as the pose's y row (the engine's camera looks along local +y, I) and used only
 * when it agrees within 30 deg with player - camera across the ground (the third-person view), so a wrong axis gives
 * the line-of-sight test alone; the aim note prints both (lw52 measures it). The cone: the vertical FOV widened by a
 * 2.4 aspect (ultrawide) plus 10 deg, across the ground. The owner is the player within 15 m of the camera (none when
 * dead or in the reinforce view). Other players' views are unknown: for them only line of sight counts. */
static int g_lwViewOwner = -1; static float g_lwViewCam[3], g_lwViewFwd[2], g_lwViewHalf; static ULONGLONG g_lwViewAt;
static int LwRayClear(const float* eye, uint32_t unit, const float* point);
static int LwViewRead(void)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lwViewAt < 200) return g_lwViewOwner;
    g_lwViewAt = now; g_lwViewOwner = -1;
    uintptr_t cm = 0, cam = 0; float pose[12], fov = 0;
    if (!EvpPtr(g_lw.game + 0x346D560u, &cm) || cm == 0 || SafeRead(cm + 0xC, pose, sizeof pose) || !EvpPtr(cm, &cam) || cam == 0 || SafeRead(cam + 0x34, &fov, 4) ||
        !(fov > 0.1f && fov < 3.0f)) return -1;
    float pos[3]; /* the rows are 16 bytes: x at +0xC, y at +0x1C (pose[4..6]), z at +0x2C, the position at +0x3C */
    if (SafeRead(cm + 0x3C, pos, sizeof pos)) return -1;
    float cx = pos[0], cy = pos[1], fx = pose[4], fy = pose[5];
    float fl = sqrtf(fx * fx + fy * fy);
    if (!(fl > 0.2f)) return -1;
    fx /= fl; fy /= fl;
    int owner = -1; float best = 15.0f * 15.0f;
    for (int i = 0; i < g_lwWorld.playersAt; i++) { float d = LwDist2(g_lwWorld.players[i], cx, cy); if (d < best) { best = d; owner = i; } }
    if (owner < 0) return -1;
    float px = g_lwWorld.players[owner][0] - cx, py = g_lwWorld.players[owner][1] - cy, pl = sqrtf(px * px + py * py);
    if (!(pl > 0.2f) || (px * fx + py * fy) / pl < cosf(30.0f * 3.14159265f / 180.0f)) return -1; /* the y row is not the view here */
    g_lwViewCam[0] = cx; g_lwViewCam[1] = cy; g_lwViewCam[2] = pos[2]; g_lwViewFwd[0] = fx; g_lwViewFwd[1] = fy;
    g_lwViewHalf = atanf(tanf(0.5f * fov) * 2.4f) + 10.0f * 3.14159265f / 180.0f;
    g_lwViewOwner = owner;
    return owner;
}
/* A remote player's view on the host (audit 1e, 6: a far squad a client could see never came online for them; KB
 * remote-player-aim-direction, static, subagent): the avatar manager M = [game+0x3326D20] keys avatar entities at
 * +0xF8 (EvpHash layout, as 0x83A729); slot i's aim rotation, a unit quaternion (x, y, z, w), at M + 0x547858 + 0x78 i
 * (network field 0xE3186939, copied from the owner's state by 0x83AB40 each frame; read for every avatar's upper-body
 * aim by 0x6B7720). Its forward is local +y rotated: (2(xy - wz), 1 - 2(x^2 + z^2), 2(yz + wx)), on the mod's axes (x, y
 * ground, z up). The aim runs from the weapon toward the crosshair point: within a degree of the camera's view at 50 m.
 * 1 with the forward across the ground in f. */
static int LwAimForward(int player, float* f)
{
    uintptr_t m = 0; uint32_t slot = 0; float qv[4];
    if (player < 0 || player >= g_lwWorld.playersAt || !g_lwWorld.avatars[player] || !EvpPtr(g_lw.game + 0x3326D20u, &m) || m == 0 ||
        !EvpHash(m + 0xF8, g_lwWorld.avatars[player], 8, 4, &slot) || SafeRead(m + 0x547858u + 0x78u * (uintptr_t)slot, qv, sizeof qv)) return 0;
    float x = qv[0], y = qv[1], z = qv[2], w = qv[3], l = x * x + y * y + z * z + w * w;
    if (!(l > 0.9f && l < 1.1f)) return 0;
    float fx = 2.0f * (x * y - w * z), fy = 1.0f - 2.0f * (x * x + z * z), fl = sqrtf(fx * fx + fy * fy);
    if (!(fl > 0.2f)) return 0;
    f[0] = fx / fl; f[1] = fy / fl;
    return 1;
}
static int LwOffScreen(int player, const float* p)
{
    if (LwViewRead() != player)
    {
        /* another machine's player: off its screen when outside the widest view any setting gives (vertical FOV 90 deg,
         * the game's clamp; 2.4 aspect, 10 deg margin, as below), around its replicated aim */
        float f[2];
        if (g_lwWorld.local[player] || !LwAimForward(player, f)) return 0;
        float dx = p[0] - g_lwWorld.players[player][0], dy = p[1] - g_lwWorld.players[player][1], d = sqrtf(dx * dx + dy * dy);
        if (!(d > 1.0f)) return 0;
        const float half = atanf(tanf(0.5f * 1.5707963f) * 2.4f) + 10.0f * 3.14159265f / 180.0f;
        return (dx * f[0] + dy * f[1]) / d < cosf(half);
    }
    float dx = p[0] - g_lwViewCam[0], dy = p[1] - g_lwViewCam[1], d = sqrtf(dx * dx + dy * dy);
    if (!(d > 1.0f)) return 0;
    return (dx * g_lwViewFwd[0] + dy * g_lwViewFwd[1]) / d < cosf(g_lwViewHalf);
}
/* Sight from the host's game camera (audit 1e, 6: the third-person camera stands about 4 m behind and above the eye, so
 * the eye's ray alone can pass where the camera's is blocked, or the reverse): player i's view counts as seen when
 * either the eye (feet + 1.7 m) or, for the player whose camera this machine renders, the camera reaches the point.
 * Returns 1 seen, 0 not, -1 no ray. */
static int LwSeenFromView(int i, const float* point)
{
    int eye = LwSeenFrom(g_lwWorld.players[i], g_lwWorld.units[i], point);
    if (eye == 1) return 1;
    if (LwViewRead() == i && LwRayClear(g_lwViewCam, g_lwWorld.units[i], point) == 1) return 1;
    return eye;
}
/* LwHidden for a far squad: as LwHidden, but a player whose screen does not hold the point does not count. */
static uint32_t g_lwOffScreen; /* far requests allowed by LwOffScreen (30 s) */
static int LwHiddenFar(const float* p)
{
    float top = NAN, base = p[2];
    if (LwGround(p[0], p[1], &top) && isfinite(top) && top > base) base = top;
    int offUsed = 0;
    for (int i = 0; i < g_lwWorld.playersAt; i++)
    {
        if (LwDist2(g_lwWorld.players[i], p[0], p[1]) > LW_SIGHT * LW_SIGHT) continue;
        for (int h = 0; h < 3; h++)
        {
            float t[3] = {p[0], p[1], base + (h == 0 ? 1.0f : h == 1 ? 2.5f : 4.5f)};
            int seen = LwSeenFromView(i, t);
            if (seen == 1 && LwOffScreen(i, t)) { offUsed = 1; break; }
            if (seen != 0) { g_lwWorld.seen++; return 0; }
        }
    }
    if (offUsed) g_lwOffScreen++;
    return 1;
}

/* 1 when a player within `range` sees the point's chest height (1.0 m over it). */
static int LwSeenByAny(const float* p, float range)
{
    float chest[3] = {p[0], p[1], p[2] + 1.0f};
    for (int i = 0; i < g_lwWorld.playersAt; i++)
        if (LwDist2(g_lwWorld.players[i], p[0], p[1]) <= range * range && LwSeenFromView(i, chest) == 1) return 1;
    return 0;
}
/* The drawn distance through the narrowest zoom (audit 1b criterion 8; KB living-world-scope-draw-distance, P unless
 * marked): the cull (exe 0x57B9B0) draws an object while its bounding sphere's angle 2 asin(r / d) exceeds F (m0 + msf
 * t^2), t = clamp((d - near) / (far - near)); the largest camera zoom in the game data is 4.0 (sniper scopes, "10x"), so
 * F = vertical_fov / 4, and vertical_fov is clamped to 45-90 deg. The host cannot know each player's setting or preset:
 * the band assumes the farthest drawing, F = 45 / 4 deg and the high preset (near 350, far 1000, msf 0.05), m0 = 0.
 * Solved by bisection (the angle falls, the threshold rises with d). r 1 / 2 / 4 / 8 m: about 701 / 811 / 952 / 1630 m. */
static float LwFarBand(float r)
{
    const float F = 45.0f / 4.0f * 3.14159265f / 180.0f, nearD = 350.0f, farD = 1000.0f, msf = 0.05f;
    if (!(r > 0.05f)) r = 0.05f;
    float lo = nearD, hi = 6000.0f;
    for (int k = 0; k < 40; k++)
    {
        float d = 0.5f * (lo + hi), t = (d - nearD) / (farD - nearD);
        if (t > 1) t = 1;
        float angle = 2.0f * asinf(r / d < 1 ? r / d : 1), limit = F * msf * t * t;
        if (angle > limit) lo = d; else hi = d;
    }
    return lo;
}
/* A unit's bounding radius: half its box's diagonal (the Unit box, exe vtable +0x238 (unit, out[0x58], flag), min at
 * +0x40, max at +0x4C, as the Lua Unit box API 0x2059A0 calls it; the object found as EvpUnitPosition finds it). 0 when
 * it does not read or is outside 0.1-30 m. */
static float LwUnitRadius(uint32_t unit)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL), registry, gens, objects, object, vtable, fn;
    uint32_t index = unit & 0x3FFFFFu, count; uint8_t gen;
    if (!unit || !EvpPtr(exe + EVP_EXE_UNITS, &registry) || SafeRead(registry + 0x98, &count, 4) || index >= count ||
        !EvpPtr(registry + 0xA0, &gens) || SafeRead(gens + index, &gen, 1) || gen != ((unit >> 22) & 0xFF) ||
        !EvpPtr(registry + 0x88, &objects) || !EvpPtr(objects + (uintptr_t)index * 8, &object) || EvpU32(object + 8, 0) != unit ||
        !EvpPtr(object, &vtable) || !EvpPtr(vtable + 0x238, &fn) || fn < exe || fn >= exe + 0x4000000u) return 0;
    uint8_t box[0x58] = {0};
    ((void (*)(uintptr_t, uint8_t*, int))fn)(object, box, 0);
    float mn[3], mx[3]; memcpy(mn, box + 0x40, 12); memcpy(mx, box + 0x4C, 12);
    float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2], r = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
    return isfinite(r) && r >= 0.1f && r <= 30.0f ? r : 0;
}
/* Each group definition's largest measured unit radius (learned from its online members), and so its band. */
#define LW_RADII 256
static struct { uintptr_t definition; float radius, band; } g_lwRadii[LW_RADII];
static uint32_t g_lwRadiiGen = 1; /* bumped on every change: a squad's cached band is recomputed (audit 1c criterion 7:
                                   * the band per offline squad per tick cost 6.4-6.9 ms a tick on XL's 6144 squads) */
static void LwGroupRadiusSeen(uintptr_t definition, float r)
{
    int empty = -1;
    for (int k = 0; k < LW_RADII; k++)
    {
        if (g_lwRadii[k].definition == definition)
        {
            if (r > g_lwRadii[k].radius) { g_lwRadii[k].radius = r; g_lwRadii[k].band = LwFarBand(r); g_lwRadiiGen++;
                Note("living world: group 0x%llx: units up to %.2f m radius: drawn to %.0f m through a 4x zoom", (unsigned long long)definition, (double)r, (double)g_lwRadii[k].band); }
            return;
        }
        if (empty < 0 && g_lwRadii[k].definition == 0) empty = k;
    }
    if (empty >= 0) { g_lwRadii[empty].definition = definition; g_lwRadii[empty].radius = r; g_lwRadii[empty].band = LwFarBand(r); g_lwRadiiGen++;
        Note("living world: group 0x%llx: units up to %.2f m radius: drawn to %.0f m through a 4x zoom", (unsigned long long)definition, (double)r, (double)g_lwRadii[empty].band); }
}
/* A squad's band, cached per squad: recomputed only when its group changes or any group's radius is learned. */
static float LwSquadBand(LwSquad* q)
{
    if (q->bandGen != g_lwRadiiGen || q->bandDef != q->definition)
    {
        static float defaultBand;
        if (defaultBand == 0) defaultBand = LwFarBand(LW_RADIUS_DEFAULT);
        q->band = defaultBand;
        for (int k = 0; k < LW_RADII; k++) if (g_lwRadii[k].definition == q->definition) { q->band = g_lwRadii[k].band; break; }
        q->bandGen = g_lwRadiiGen; q->bandDef = q->definition;
    }
    return q->band;
}

/* A far squad's way ahead (40 m toward its next node, on the ground) in a player's view within its band. */
static int LwAboutToBeSeen(LwSquad* q)
{
    if (q->pathAt >= q->pathLen) return 0;
    float nx = g_lwNodes[q->path[q->pathAt]].x - q->pos[0], ny = g_lwNodes[q->path[q->pathAt]].y - q->pos[1], d = sqrtf(nx * nx + ny * ny);
    float k = d > 40.0f ? 40.0f / d : 1.0f, ahead[3] = {q->pos[0] + nx * k, q->pos[1] + ny * k, 0};
    if (!LwGround(ahead[0], ahead[1], &ahead[2])) return 0;
    return LwSeenByAny(ahead, LwSquadBand(q));
}

/* A node the AI can walk to (subagent navlink, KB nav-cell-connections; checked by me: 0x7FD440's 0x7ED200 call): the
 * squad's state 10 snaps its goal with exe 0x7D89F0 within 25 m (0x23C7614) and, finding no face, sets the squad's
 * own position as its destination: it stands still. On XL most cells are not built (only those around the players),
 * so a node there is no goal for an online squad. The same snap as the game's, at the node's ground, cached per node
 * for 20 s (cells get built as players move). */
static int LwNodeHasFace(int node)
{
    static uint16_t nodeAt[LW_NODES_MAX]; static uint8_t nodeFace[LW_NODES_MAX]; static uintptr_t level;
    typedef int (*SnapFn)(uint32_t nav, float* p, float radius, float* out, float* normal, uint32_t layer);
    if (level != g_lwWorld.lg) { memset(nodeAt, 0, sizeof nodeAt); level = g_lwWorld.lg; }
    if (node < 0 || node >= g_lwNodeCount || node >= LW_NODES_MAX) return 0;
    uint16_t now = (uint16_t)(GetTickCount64() / 20000u | 1u);
    if (nodeAt[node] == now) return nodeFace[node];
    uintptr_t table = 0, fn = 0;
    float p[3] = {g_lwNodes[node].x, g_lwNodes[node].y, 0};
    int face = EvpPtr(g_lw.game + 0x3326330u, &table) && EvpPtr(table + 0x10, &fn) && fn && LwGround(p[0], p[1], &p[2]) &&
        ((SnapFn)fn)(EvpU32(g_lwWorld.dir + 0x648, 0), p, 25.0f, p, NULL, 0) != 0 && isfinite(p[2]);
    nodeAt[node] = now; nodeFace[node] = (uint8_t)face;
    return face;
}

static void LwSendGoal(LwSquad* q)
{
    if (LwIntentGoal(q)) return; /* an intent picks the destination (Phase 2) */
    if (q->pathAt >= q->pathLen) LwPlan(q);
    if (q->pathAt >= q->pathLen || LwJobRecord(q->squad) == 0 || LwSquadRecord(q->squad) == 0) return;
    if (!LwNodeHasFace(q->path[q->pathAt])) /* the next node has no navmesh: another destination whose next node has */
    {
        g_lwWorld.goalNoFace++;
        int t = 0;
        for (; t < 8; t++) { g_lwWorld.goalReplans++; LwPlan(q); if (q->pathAt < q->pathLen && LwNodeHasFace(q->path[q->pathAt])) break; }
        if (t == 8) { g_lwWorld.goalStuck++; q->pathLen = q->pathAt = 0; return; } /* tried again at the next goal send */
    }
    float goal[3] = {g_lwNodes[q->path[q->pathAt]].x, g_lwNodes[q->path[q->pathAt]].y, 0};
    if (!LwGround(goal[0], goal[1], &goal[2])) return; /* the goal's face search needs its height (40 m sphere, 100 m ray up) */
    g_lw.goal(q->squad, goal);
    g_lwWorld.goals++;
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
static const char* LwLostWhy(uint32_t unit, uint32_t* squad, char* by, size_t size, const float* at)
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
        char what[192]; int of = -1;
        for (int k = 0; k < g_lwWorld.count && of < 0; k++)
            for (int m = 0; m < g_lwSquads[k].seenCount && of < 0; m++) if (g_lwSquads[k].seen[m] == attacker) of = k;
        if (of >= 0) /* lw42b: five members killed by squad mates, damage hash 0, 140-335 m from any player: the mate's
                      * resource and distance, and the victim's, to tell the units and the reach */
        {
            uint32_t au = 0, vu = 0; float ap[3] = {0, 0, 0}; uint64_t res = EvpEntityResource(g_lw.game, attacker, &au), vres = EvpEntityResource(g_lw.game, unit, &vu);
            int placed = au && EvpUnitPosition(au, ap);
            /* hash 0 is a bullet or explosion hit (impacts carry E32E9832; KB living-world-spawn-losses "Mate kills"):
             * the shooter's alert says whether it was engaging something */
            snprintf(what, sizeof what, "a member of squad %d (resource %016llx, alert %u, %s%.1f m away; the dead one %016llx)", of, (unsigned long long)res, LwAlert(attacker),
                placed ? "" : "no position, ", placed ? (double)sqrtf((ap[0] - at[0]) * (ap[0] - at[0]) + (ap[1] - at[1]) * (ap[1] - at[1]) + (ap[2] - at[2]) * (ap[2] - at[2])) : 0.0, (unsigned long long)vres);
        }
        else if (enemy && net && net != 0x7FFF) snprintf(what, sizeof what, "an enemy of garrison net %08X", net);
        else if (enemy) snprintf(what, sizeof what, "an enemy in no garrison");
        else /* not a squad member, garrison enemy or Helldiver (lw34d: entity 00000134 killed 4 members 0.1 s after
              * they appeared): its resource (name hash) and distance, to find what it is */
        {
            uint32_t au = 0; float ap[3] = {0, 0, 0}; uint64_t res = attacker && attacker != 0x7FFF ? EvpEntityResource(g_lw.game, attacker, &au) : 0;
            int placed = au && EvpUnitPosition(au, ap);
            snprintf(what, sizeof what, "entity (resource %016llx, %s%.0f m away)", (unsigned long long)res, placed ? "" : "no position, ",
                placed ? (double)sqrtf((ap[0] - at[0]) * (ap[0] - at[0]) + (ap[1] - at[1]) * (ap[1] - at[1]) + (ap[2] - at[2]) * (ap[2] - at[2])) : 0.0);
        }
        uint32_t hit = EvpU32(h.record + 0x40, 0); /* the damage hash the apply writes with the attacker (0x923C0A..0x923C34; KB
                                                    * living-world-spawn-losses): 0 = the gore fall/impact kill, 49B33778 the event kill */
        snprintf(by, size, " by %s %08X%s, damage %08X", helldiver ? "a Helldiver" : attacker == unit ? "itself" : attacker == 0 || attacker == 0x7FFF ? "nothing named" : what,
            attacker, creditor ? ", credited to a player" : ", no player credited", hit);
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
            uint32_t other = 0; float ground = NAN, *at = q->seenAt[i]; char by[288];
            int twice = 0; /* lw34d: one member noted lost twice in one tick: a repeated seen entry (cause open) */
            for (int j = i + 1; j < q->seenCount; j++) if (q->seen[j] == q->seen[i]) { twice++; --q->seenCount; q->seen[j] = q->seen[q->seenCount]; memcpy(q->seenAt[j], q->seenAt[q->seenCount], sizeof q->seenAt[j]); j--; }
            if (twice) Note("living world: squad %d: member %08X was listed %d more time(s) in its seen list (counted once)", (int)(q - g_lwSquads), q->seen[i], twice);
            const char* why = LwLostWhy(q->seen[i], &other, by, sizeof by, at);
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
    if (!q->arrived && n > 0) /* every member's first position in its first 5 s (audit 1b: the first tick read 2 of 6-9):
                               * its height against the request, and whether a player sees it */
    {
        for (int k = 0; k < n; k++)
        {
            int done = 0;
            for (int c = 0; c < q->checkedCount && !done; c++) done = q->checked[c] == members[k];
            ActHealth h; float p[3];
            if (done || q->checkedCount >= 32 || !ActHealthOf(g_lw.game, members[k], &h) || !h.unit || !EvpUnitPosition(h.unit, p)) continue;
            if (q->checkedCount == 0 || p[2] < q->appearLo) q->appearLo = p[2];
            if (q->checkedCount == 0 || p[2] > q->appearHi) q->appearHi = p[2];
            for (int c = 0; c < q->checkedCount; c++) /* the nearest two members (stacked members: KB living-world-spawn-losses) */
            {
                float gap = sqrtf(LwDist2(q->checkedAt[c], p[0], p[1]));
                if (gap < q->appearGap) q->appearGap = gap;
            }
            q->checkedAt[q->checkedCount][0] = p[0]; q->checkedAt[q->checkedCount][1] = p[1];
            float radius = LwUnitRadius(h.unit); /* the group's band follows its largest unit (audit 1b criterion 8) */
            if (radius > 0) LwGroupRadiusSeen(q->definition, radius);
            q->checked[q->checkedCount++] = members[k];
            float chest[3] = {p[0], p[1], p[2] + 1.0f};
            for (int i = 0; i < g_lwWorld.playersAt; i++)
                if (LwDist2(g_lwWorld.players[i], p[0], p[1]) <= LW_SIGHT * LW_SIGHT && LwSeenFrom(g_lwWorld.players[i], g_lwWorld.units[i], chest) == 1) { q->appearSeen++; break; }
        }
        if (q->checkedCount > 0 && (q->checkedCount >= q->cap || GetTickCount64() - q->since > 5000))
        {
            q->arrived = 1;
            char gap[48] = "one member, no gap"; /* lw50a: a one-member squad read as "nearest two 0.0 m apart" */
            if (q->checkedCount > 1) snprintf(gap, sizeof gap, "nearest two %.1f m apart", (double)q->appearGap);
            Note("living world: squad %d members appear: %d of %u read in 5 s, z %.1f .. %.1f (requested %.1f), %s, %d of them in a player's sight",
                (int)(q - g_lwSquads), q->checkedCount, q->cap, (double)q->appearLo, (double)q->appearHi, (double)q->reqZ, gap, q->appearSeen);
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
        if (v > 0.3f && v < 12.0f && !q->rushed) /* not while sped up for a fight (2b) */
        {
            q->speed = q->speed * 0.5f + v * 0.5f;
            Note("living world: squad %d walks %.2f m/s (its speed now %.2f m/s); %d members, %u lost", (int)(q - g_lwSquads), (double)v, (double)q->speed, n, q->lost);
        }
        q->moved = now; q->movedFrom[0] = at[0]; q->movedFrom[1] = at[1];
        /* each member against the drawn ground under it (audit 1b criterion 11: the steering walls' fix, "squads do not
         * cross walls or cliffs in tiles", was never shown): a member that walks through a cliff or off the navmesh's
         * idea of the ground stands inside it or floats over it. Under: 1.5 m below the ground ray's top; over: 3 m
         * above it (the ray hits the topmost surface, so a member on a roof or bridge reads as on the ground). */
        for (int k = 0; k < n; k++)
        {
            ActHealth h; float p[3], top = NAN;
            if (!ActHealthOf(g_lw.game, members[k], &h) || !h.unit || !EvpUnitPosition(h.unit, p) || !LwGround(p[0], p[1], &top) || !isfinite(top)) continue;
            g_lwWorld.heights++;
            /* against the ground as drawn (the live tile's, else the level's coarse rows) where it reads: the topmost ray
             * hit counts a member beside a rock or under an overhang as under the ground (audit 1g, 9) */
            /* the drawn ground exists only on a level with moving tiles; elsewhere the ground ray's top (audit 1h, 15: on vanilla
             * levels MoveCoarseHere read the XL extent's texels and reported every member 18 m under) */
            int tile; float drawn = MoveTilesOn() ? MoveDrawnGround(p[0], p[1], &tile) : -9999.0f, ref = drawn > -9000.0f ? drawn : top;
            if (p[2] < ref - 1.5f) { g_lwWorld.underGround++; if (ref - p[2] > g_lwWorld.worstUnder) g_lwWorld.worstUnder = ref - p[2]; }
            if (p[2] > top + 3.0f) { g_lwWorld.overGround++; if (p[2] - top > g_lwWorld.worstOver) g_lwWorld.worstOver = p[2] - top; }
            /* a member falling under the world (lw40b: squad 4's 00000470 at z -1898, ground 38.3, removed): where it was
             * last on the ground and whether the navmesh has a face there (the game's 10 m snap, layer 0), once each */
            if (k < 32 && top - p[2] > 10.0f && q->groundId[k] == members[k] && q->groundAt[k][2] - p[2] > 10.0f) /* fell: 10 m lower than its last
                * sample on the ground (lw48a: one under a 17 m overhang, its z unchanged, was noted) */
            {
                typedef int (*SnapFn)(uint32_t nav, float* p, float radius, float* out, float* normal, uint32_t layer);
                uintptr_t table = 0, fn = 0; float face[3] = {q->groundAt[k][0], q->groundAt[k][1], q->groundAt[k][2]}; int has = 0;
                if (EvpPtr(g_lw.game + 0x3326330u, &table) && EvpPtr(table + 0x10, &fn) && fn) has = ((SnapFn)fn)(EvpU32(g_lwWorld.dir + 0x648, 0), face, 10.0f, face, NULL, 0) != 0;
                Note("living world: squad %d member %08X under the ground: at (%.1f, %.1f, %.1f), ground %.1f; 5 s before on the ground at (%.1f, %.1f, %.1f); %s", (int)(q - g_lwSquads),
                    members[k], (double)p[0], (double)p[1], (double)p[2], (double)top, (double)q->groundAt[k][0], (double)q->groundAt[k][1], (double)q->groundAt[k][2],
                    has ? "" : "no navmesh face within 10 m there");
                if (has) Note("living world: squad %d member %08X: the navmesh face nearest that point is %.1f m across, %.1f m up", (int)(q - g_lwSquads), members[k],
                    (double)sqrtf(LwDist2(q->groundAt[k], face[0], face[1])), (double)(face[2] - q->groundAt[k][2]));
                q->groundId[k] = 0;
            }
            else if (k < 32 && fabsf(top - p[2]) <= 1.5f) { q->groundId[k] = members[k]; memcpy(q->groundAt[k], p, sizeof p); }
        }
    }
    q->pos[0] = at[0]; q->pos[1] = at[1];
    LwIntentOnline(q, members, n, at);
    /* test instrument (HELLFORGE_LW_TEST_TRACE=1): once per squad, when its leader is within 120 m of a player and that
     * player's eye sees its chest (the sight ray), a note for a screenshot aimed at it */
    static int testView = -1;
    if (testView < 0) { char t[4] = ""; testView = GetEnvironmentVariableA("HELLFORGE_LW_TEST_TRACE", t, sizeof t) > 0 && t[0] == '1'; }
    if (testView && !q->viewed && (now / 500) % 4 == (ULONGLONG)(q - g_lwSquads) % 4)
        for (int i = 0; i < g_lwWorld.playersAt && !q->viewed; i++)
        {
            float lead[3]; ActHealth h;
            float viewMax = q->farOn ? LwSquadBand(q) + LW_FAR_MARGIN : 120.0f;
            if (LwDist2(g_lwWorld.players[i], at[0], at[1]) > viewMax * viewMax || !ActHealthOf(g_lw.game, members[0], &h) || !h.unit || !EvpUnitPosition(h.unit, lead)) continue;
            float chest[3] = {lead[0], lead[1], lead[2] + 1.0f};
            if (LwSeenFrom(g_lwWorld.players[i], g_lwWorld.units[i], chest) == 1)
            {
                q->viewed = 1;
                Note("living world: test: squad %d in view at (%.0f, %.0f, %.1f), %.0f m from player %d, %d members, %u lost%s", (int)(q - g_lwSquads), (double)lead[0],
                    (double)lead[1], (double)lead[2], (double)sqrtf(LwDist2(g_lwWorld.players[i], lead[0], lead[1])), i + 1, n, q->lost, q->farOn ? " (by sightline)" : "");
            }
        }
    if (q->intent == LW_PATROL && q->pathAt < q->pathLen && LwDist2(at, g_lwNodes[q->path[q->pathAt]].x, g_lwNodes[q->path[q->pathAt]].y) < LW_ARRIVE * LW_ARRIVE)
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
    int forced = (testCycle && q->lost > 0 && !q->cycled && now - q->since > 45000 && *removals < 16) || q->forceOff;
    int swapped = q->forceOff == 2;
    if (q->forceOff) { if (!swapped) Note("living world: test: squad %d sent offline by command with %u of %u alive", (int)(q - g_lwSquads), q->survivors, q->cap); q->forceOff = 0; }
    else
    if (forced && !q->cycled && testCycle && q->lost > 0) { q->cycled = 1; Note("living world: test: squad %d sent offline with %u of %u alive", (int)(q - g_lwSquads), q->survivors, q->cap); }
    else
    {
        /* offline only once it has been beyond R_off of every player, every member at alert 0, for LW_FAR_MS without a break */
        float d2 = LwNearestPlayer2(at[0], at[1]);
        int calm = g_lwWorld.playersAt > 0 && d2 > LW_R_OFF * LW_R_OFF;
        float keep = LwSquadBand(q) + LW_FAR_MARGIN;
        if (calm && d2 < keep * keep) /* in view range: it stays while a player sees it (sightlines), checked once a second */
        {
            if (now >= q->lookAt)
            {
                float p[3] = {at[0], at[1], 0};
                q->lookAt = now + 1000 + (ULONGLONG)((q - g_lwSquads) % 7) * 50;
                /* seen where it stands, or (a squad brought online by sightline) its way ahead is, so it is not taken
                 * offline while still hidden on its way into view (lw16l: offline 10 s after it came online) */
                q->inView = (LwGround(at[0], at[1], &p[2]) && LwSeenByAny(p, keep)) || (q->farOn && LwAboutToBeSeen(q));
            }
            if (q->inView || (q->farOn && now - q->since < 60000)) calm = 0; /* and a minute's grace to walk into view */
        }
        /* a member's alert keeps it online only within a player's view range (band + margin): beyond that no player can
         * see or reach the fight (lw85a: alerted squads 3-6 km behind a player who moved held 48 of the 59 units, and
         * nothing could come online near him; vanilla removes patrols beyond 300 m whatever their alert, 0x95288B) */
        for (int k = 0; k < n && calm && d2 < keep * keep; k++) if (LwAlert(members[k]) != 0) calm = 0;
        if (!calm) { q->farSince = 0; return; }
        if (q->farSince == 0) q->farSince = now;
        if (now - q->farSince < LW_FAR_MS || *removals >= 16) return;
    }
    Note("living world: squad %d (%08X) goes offline at (%.0f, %.0f), %.0f m from the nearest player, %u of %u alive", (int)(q - g_lwSquads), q->squad,
        (double)at[0], (double)at[1], (double)sqrtf(LwNearestPlayer2(at[0], at[1])), q->survivors, q->cap);
    LwRemoveSquad(q->squad);
    (*removals)++;
    q->state = LW_OFFLINE; q->squad = 0; g_lwWorld.onlineUnits -= q->alive; g_lwWorld.offlined++;
    if (swapped) q->retry = now + 60000; /* displaced by a far squad in sight: not straight back */
    int node = LwNearestNode(at[0], at[1]);
    if (node >= 0 && q->intent != LW_PATROL) { q->node = (uint16_t)node; LwIntentOffline(q); }
    else if (node >= 0 && q->pathAt >= q->pathLen) { q->node = (uint16_t)node; LwPlan(q); }
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

/* ---- test commands (development only): the harness's in-game Lua calls this export through LuaJIT's ffi, as it
 * calls harness_input (harness.py INPUT_LUA); the command waits in a queue until the loader's next game-thread tick,
 * which runs it and notes the result ("living world: test command N: ..."). Host only.
 *   spawn near|far|visible D [deg [N]]  an offline squad (squad N) at a placeable point D m from player 1 in the camera's direction
 *                                   (+deg): near / far = through the placement and sight rules (far: as by sightline),
 *                                   visible = in a player's sight (screenshots only; the whole circle,
 *                                   the camera's view first; the note says how far to turn)
 *   offline N        squad N goes offline now (survivors carried)      status   the online squads
 *   kill garrison    the garrison kill test on the next active garrison, without its 15 s wait
 *   teleport X Y     the local Helldiver to the ground at (X, Y) (LwTestTeleport)
 *   stage D          an offline squad's record to a route node D m out in player 1's sight (LwTestStage)
 *   aim X Y          how far the camera's view is from (X, Y), for the harness to turn it (LwTestAim)
 *   look N           how many of squad N's members a ray from the camera reaches (LwTestLook)
 * The camera: CM = [game+0x346D560], [CM+0x3C] its world position (subagent viewdist, P); the third-person camera
 * stands behind the Helldiver, so player - camera is the screen's direction across the ground. */
static SRWLOCK g_lwCmdLock = SRWLOCK_INIT;
static char g_lwCmd[8][128];
static int g_lwCmdCount, g_lwCmdSeq;
__declspec(dllexport) int hellforge_lw_test(const char* cmd)
{
    if (cmd == NULL) return -1;
    int seq = -1;
    AcquireSRWLockExclusive(&g_lwCmdLock);
    if (g_lwCmdCount < 8) { snprintf(g_lwCmd[g_lwCmdCount++], sizeof g_lwCmd[0], "%s", cmd); seq = ++g_lwCmdSeq; }
    ReleaseSRWLockExclusive(&g_lwCmdLock);
    return seq;
}
/* The view's direction across the ground: the camera pose's y row (lw52a / lw53b: it follows player - camera at a
 * steady 8.7 deg, the over-the-shoulder offset: the Helldiver stands left of the screen's centre), else player -
 * camera. */
static int LwCameraForward(float* f)
{
    uintptr_t cm = 0; float cam[3], pose[8];
    if (!EvpPtr(g_lw.game + 0x346D560u, &cm) || cm == 0 || SafeRead(cm + 0x3C, cam, sizeof cam) || g_lwWorld.playersAt == 0) return 0;
    if (!SafeRead(cm + 0xC, pose, sizeof pose)) { float l = sqrtf(pose[4] * pose[4] + pose[5] * pose[5]); if (l > 0.2f) { f[0] = pose[4] / l; f[1] = pose[5] / l; return 1; } }
    float dx = g_lwWorld.players[0][0] - cam[0], dy = g_lwWorld.players[0][1] - cam[1], d = sqrtf(dx * dx + dy * dy);
    if (!(d > 0.2f)) return 0;
    f[0] = dx / d; f[1] = dy / d;
    return 1;
}
static void LwTestSpawn(int seq, const char* mode, float dist, float deg, int which)
{
    /* scan: as visible, but the farthest seen and placeable point from D down to D / 2 in 25 m steps, every 10 deg
     * around (lw42b, lw45a, lw46a: at 350-650 m the visible rings found none on vanilla or XL) */
    int scan = strcmp(mode, "scan") == 0, visible = scan || strcmp(mode, "visible") == 0, asFar = strcmp(mode, "far") == 0;
    float f[2] = {1, 0};
    int fromCamera = LwCameraForward(f);
    float base = atan2f(f[1], f[0]) + deg * 3.14159265f / 180.0f, *pl = g_lwWorld.players[0];
    LwSquad* best = NULL; float bestD = 3e38f, tx = pl[0] + dist * cosf(base), ty = pl[1] + dist * sinf(base);
    for (int i = 0; i < g_lwWorld.count; i++)
    {
        LwSquad* q = &g_lwSquads[i];
        if (q->state != LW_OFFLINE || q->survivors == 0) continue;
        float d = LwDist2(q->pos, tx, ty);
        if (d < bestD) { bestD = d; best = q; }
    }
    if (which >= 0) best = which < g_lwWorld.count && g_lwSquads[which].state == LW_OFFLINE && g_lwSquads[which].survivors ? &g_lwSquads[which] : NULL;
    if (best == NULL) { Note("living world: test command %d: no offline squad%s", seq, which >= 0 ? " by that number" : ""); return; }
    static const float offsets[] = {0, 6, -6, 12, -12, 18, -18, 24, -24, 30, -30, 36, -36}, around[] = {0, 15, -15, 30, -30, 45, -45, 60, -60, 75, -75, 90, -90,
        105, -105, 120, -120, 135, -135, 150, -150, 165, -165, 180};
    const float* offs = visible ? around : offsets; int nOffs = visible ? (int)(sizeof around / sizeof around[0]) : (int)(sizeof offsets / sizeof offsets[0]);
    uint32_t unseen0 = g_lwTestUnseen;
    uint32_t noFace0 = g_lwWorld.noFace, moved0 = g_lwWorld.moved, over0 = g_lwWorld.overhead, seen0 = g_lwWorld.seen, refused0 = g_lwWorld.refused, crowded0 = g_lwWorld.crowded;
    float save[2] = {best->pos[0], best->pos[1]};
    int outside = 0; g_lwRoomWhy[0] = 0;
    /* visible: four distances per direction (lw42b: from 0.75 of the disc's radius every 650-350 m point was outside the
     * disc, unseen or crowded); the others: the distance and 0.9 of it */
    static const float visibleMul[] = {1.0f, 0.92f, 0.84f, 0.76f}, otherMul[] = {1.0f, 0.9f};
    const float* mul = visible ? visibleMul : otherMul; int nMul = visible ? 4 : 2;
    int nScan = scan ? 1 + (int)(dist * 0.5f / 25.0f) : 0, total = scan ? nScan * 36 : nOffs * nMul;
    for (int k = 0; k < total; k++)
    {
        float turn = scan ? (float)(((k % 36) + 1) / 2 * 10 * ((k % 36) % 2 ? -1 : 1)) : offs[k / nMul];
        float a = base + turn * 3.14159265f / 180.0f, d = scan ? dist - 25.0f * (float)(k / 36) : dist * mul[k % nMul];
        float p[2] = {pl[0] + d * cosf(a), pl[1] + d * sinf(a)};
        if (p[0] * p[0] + p[1] * p[1] > g_lwWorld.radius * g_lwWorld.radius) { outside++; continue; }
        best->pos[0] = p[0]; best->pos[1] = p[1];
        int node = LwNearestNode(p[0], p[1]);
        if (node >= 0) { best->node = (uint16_t)node; best->pathLen = best->pathAt = 0; LwPlan(best); }
        if (asFar && node >= 0) /* test only: a far squad's route leads to the player, so it walks into view for an aimed
                                 * shot (lw33b, lw34d, lw35c: far squads on random routes walked away unseen) */
        {
            int home = LwNearestNode(pl[0], pl[1]), len = home >= 0 ? LwPath(node, home, best->path, LW_PATH_MAX) : 0;
            if (len > 0) { best->pathLen = (uint8_t)len; best->pathAt = 0; }
        }
        best->retry = 0;
        g_lwTestVisible = visible;
        LwOnline(best);
        g_lwTestVisible = 0;
        if (best->state == LW_PENDING)
        {
            if (asFar) best->farOn = 1;
            Note("living world: test command %d: squad %d spawn %s at (%.0f, %.0f), %.0f m %s, %+.0f deg from it (try %d), %u members (%u lost before)", seq, (int)(best - g_lwSquads), mode,
                (double)best->pos[0], (double)best->pos[1], (double)sqrtf(LwDist2(pl, best->pos[0], best->pos[1])),
                fromCamera ? "along the camera's view" : "east (no camera)", (double)(deg + turn), k + 1, best->cap, best->lost);
            return;
        }
    }
    best->pos[0] = save[0]; best->pos[1] = save[1];
    Note("living world: test command %d: spawn %s %.0f m refused at %d points: %u not seen (visible), %d outside the playable disc (radius %.0f m), %u no face, %u snapped > 2 m, %u under something, %u members would not reach their slots, %u in sight, %u refused%s%s%s",
        seq, mode, (double)dist, nOffs * 2, g_lwTestUnseen - unseen0, outside, (double)g_lwWorld.radius, g_lwWorld.noFace - noFace0, g_lwWorld.moved - moved0, g_lwWorld.overhead - over0, g_lwWorld.crowded - crowded0, g_lwWorld.seen - seen0,
        g_lwWorld.refused - refused0, g_lwRoomWhy[0] ? " (last: " : "", g_lwRoomWhy, g_lwRoomWhy[0] ? ")" : "");
}
/* Test command "teleport X Y": the local Helldiver to the ground at (X, Y), through the game's own MotionComponent
 * teleport, as the hellpod exit calls it (subagent teleport; checked by me: 0xA8913F..0xA8914F, the pin, the map
 * header): 0x5A2BB0(unused, avatar entity, float pos[3]) writes the motion record, the unit (API +0xC8), the Havok mover
 * body and the replicated position, and bumps the teleport counter so the next frame snaps (0x59F4B0). It has no
 * not-found check, so the entity must be in the motion map ([game+0x3326558] +0x48A0, descriptor [+0x48B8][i] +8), and
 * it stops half-way for an attached avatar (0x4A52B0(_, e) != 0: a vehicle or a pod). Velocity is kept: ground only. */
/* Live AI enemies within r of (x, y) (the AiEnemy records [game+0x33266C8]: count +0x2320, entities +0x2348, as the
 * garrison test reads them), and the nearest one's distance. */
static int LwEnemiesNear(float x, float y, float r, float* nearest)
{
    uintptr_t e, entities, d; int n = 0; *nearest = 1e9f;
    if (!EvpPtr(g_lw.game + 0x33266C8u, &e) || !EvpPtr(e + 0x2348, &entities)) return 0;
    uint32_t count = EvpU32(e + 0x2320, 0);
    for (uint32_t i = 0; i < count && i < 65536; i++)
    {
        ActHealth h; float p[3];
        if (!EvpPtr(entities + 8u * i, &d) || d == 0) continue;
        uint32_t entity = EvpU32(d + 8, 0);
        if (!ActHealthOf(g_lw.game, entity, &h) || h.life >= 2 || !h.unit || !EvpUnitPosition(h.unit, p)) continue;
        float dd = sqrtf((p[0] - x) * (p[0] - x) + (p[1] - y) * (p[1] - y));
        if (dd < *nearest) *nearest = dd;
        if (dd < r) n++;
    }
    return n;
}
/* Test command "stage D": one offline squad's record held at a point D +- 50 m from player 1 that the player sees
 * (LwSeenByAny within the squad's draw band), on the ground and inside the disc; nothing else. Whether and how it comes online is the
 * far rule's own decision (by sightline, off the host's screen), so a run can show the far path without waiting for an
 * offline squad to wander into a player's view (audit 1d, 6: none did in 4 min in lw75/lw76). */
static void LwTestStage(int seq, float d)
{
    if (!g_lwWorld.playersAt) { Note("living world: test command %d: stage: no player", seq); return; }
    const float* pl = g_lwWorld.players[0];
    int pick = -1; float far2 = -1; /* the offline squad farthest from the players (lw77a: none beyond 350 m on a 389 m disc) */
    for (int i = 0; i < g_lwWorld.count; i++)
    {
        LwSquad* q = &g_lwSquads[i]; float d2;
        if (q->state == LW_OFFLINE && q->survivors && !q->cycled && !q->pulled && (d2 = LwNearestPlayer2(q->pos[0], q->pos[1])) > far2) { far2 = d2; pick = i; }
    }
    if (pick < 0) { Note("living world: test command %d: stage: no offline squad", seq); return; }
    LwSquad* q = &g_lwSquads[pick];
    float band = LwSquadBand(q), r = g_lwWorld.radius;
    int tried = 0;
    uint32_t start = LwRand() % 72u;
    /* points every 5 deg on rings d, d - 25, d + 25, d - 50, d + 50 (lw77b: the XL route graph had no node 450 +- 50 m
     * out); the record is held there (pulled 2: walks nowhere, presence leaves it) until it comes online */
    static const float rings[5] = {0, -25, 25, -50, 50};
    for (int ri = 0; ri < 5; ri++)
        for (int k = 0; k < 72; k++)
        {
            float dd = d + rings[ri], a = (float)((start + (uint32_t)k) % 72u) * 0.0872665f, x = pl[0] + dd * cosf(a), y = pl[1] + dd * sinf(a);
            if (dd >= band || dd <= LW_R_ON || x * x + y * y > r * r) continue;
            float here[3] = {x, y, 0};
            tried++;
            if (!LwGround(x, y, &here[2]) || !LwSeenByAny(here, band)) continue;
            q->pos[0] = x; q->pos[1] = y; q->node = (uint16_t)LwNearestNode(x, y); q->pathLen = q->pathAt = 0; q->retry = 0;
            q->pulled = 2;
            Note("living world: test command %d: stage: squad %d (%u alive, band %.0f m) held offline at (%.0f, %.0f), %.0f m from player 1, in its sight (%d points tried)",
                seq, pick, q->survivors, (double)band, (double)x, (double)y, (double)dd, tried);
            return;
        }
    Note("living world: test command %d: stage: no point %.0f +- 50 m out in player 1's sight (%d points tried, band %.0f m)", seq, (double)d, tried, (double)band);
}
/* Test command "vantage": a point in the disc with open views over the far band, so the far rule's natural onlines can
 * be watched in clear line (audit 1e, 6: from most places on the vanilla maps the far band is behind terrain or
 * scenery). 160 random grounded candidates inside 0.9 of the disc; for each, 96 random route nodes 250-700 m away,
 * counted when the sight ray from the candidate's eye (+1.7 m) reaches 1 m over the node's ground; candidates with an
 * enemy within 120 m are skipped (LwEnemiesNear). The best count wins; then teleported there (LwTestTeleport). One
 * pass on the game thread (about 15k rays, ~0.1 s). */
static int g_lwTeleportForce; /* "teleport! X Y": the enemy check skipped (going back to a garrison) */
static void LwTestTeleport(int seq, float x, float y);
static int LwEnemiesNear(float x, float y, float r, float* nearest);
static void LwTestGarrisons(int seq);
static void LwTestVantage(int seq)
{
    if (g_lwNodeCount < 8 || g_lwWorld.radius < 100.0f) { Note("living world: test command %d: vantage: no route graph or disc", seq); return; }
    static uint32_t rng = 0x9E3779B9u;
    #define LWRND() (rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5, (float)(rng & 0xFFFFFF) / 16777216.0f)
    float best[2] = {0, 0}; int bestN = -1, tried = 0;
    for (int c = 0; c < 160; c++)
    {
        float a = LWRND() * 6.2831853f, r = sqrtf(LWRND()) * 0.9f * g_lwWorld.radius, eye[3] = {r * cosf(a), r * sinf(a), 0}, nearest;
        if (!LwGround(eye[0], eye[1], &eye[2]) || LwEnemiesNear(eye[0], eye[1], 120.0f, &nearest) > 0) continue;
        eye[2] += 1.7f; tried++;
        int n = 0;
        for (int k = 0; k < 96; k++)
        {
            LwNode* o = &g_lwNodes[(int)(LWRND() * (float)g_lwNodeCount) % g_lwNodeCount];
            float dx = o->x - eye[0], dy = o->y - eye[1], d2 = dx * dx + dy * dy;
            if (d2 < 250.0f * 250.0f || d2 > 700.0f * 700.0f) continue;
            float t[3] = {o->x, o->y, 0};
            if (!LwGround(t[0], t[1], &t[2])) continue;
            t[2] += 1.0f;
            if (LwRayClear(eye, g_lwWorld.units[0], t) == 1) n++;
        }
        if (n > bestN) { bestN = n; best[0] = eye[0]; best[1] = eye[1]; }
    }
    #undef LWRND
    if (bestN <= 0) { Note("living world: test command %d: vantage: none of %d safe candidates sees a route node 250-700 m out", seq, tried); return; }
    Note("living world: test command %d: vantage: (%.0f, %.0f) sees %d of 96 sampled route nodes 250-700 m out (best of %d safe candidates)", seq, (double)best[0], (double)best[1], bestN, tried);
    LwTestTeleport(seq, best[0], best[1]);
}

static void LwTestTeleport(int seq, float x, float y)
{
    ActHealth h; const char* why = "";
    uintptr_t mgr = 0, descs = 0, d = 0, local = 0; uint32_t i = 0;
    uint32_t e = ActLocalAvatar(g_lw.game, 0, &h, &why);
    float pos[3] = {x, y, 0};
    if (!e) { Note("living world: test command %d: teleport: %s", seq, why); return; }
    if (x * x + y * y > g_lwWorld.radius * g_lwWorld.radius) { Note("living world: test command %d: teleport: (%.0f, %.0f) is outside the playable disc", seq, (double)x, (double)y); return; }
    if (!ApiCodeMatches(g_lw.game + 0x5A2BB0u, "48 89 5C 24 08 48 89 6C 24 10 56 57 41 54 41 56 41 57 48 83 EC 40")) { Note("living world: test command %d: teleport: 0x5A2BB0 is not the reviewed code", seq); return; }
    if (!EvpPtr(g_lw.game + 0x3326558u, &mgr) || !EvpHash(mgr + 0x48A0, e, 8, 4, &i) || !EvpPtr(mgr + 0x48B8, &descs) || !EvpPtr(descs + 8u * i, &d) ||
        EvpU32(d + 8, 0) != e) { Note("living world: test command %d: teleport: the Helldiver %08X has no motion record", seq, e); return; }
    if (((uint8_t (*)(void*, uint32_t))(g_lw.game + 0x4A52B0u))(NULL, e)) { Note("living world: test command %d: teleport: the Helldiver is attached (vehicle or pod)", seq); return; }
    if (!LwGround(x, y, &pos[2])) { Note("living world: test command %d: teleport: no ground at (%.0f, %.0f)", seq, (double)x, (double)y); return; }
    /* XL: the ground ray can hit something below the terrain where the tile's collision is not in yet (lw88a: put at
     * z -36.7 at (2500, 1800), the height plan there about 60 m: the Helldiver died at once and the mission ended) */
    float plan = g_lwWorld.radius > 1500.0f && g_near.global != NULL && g_near.level == g_world.levels ? NearGlobalAt(x, y) : -9999.0f;
    /* under the water a Helldiver drowns (kind 8 damage; lw92a: put at z -40.7 by water at -39.0, dead 9 s later) */
    float water = g_nearWater.level == g_world.levels ? g_nearWater.high : -500.0f;
    if (pos[2] < water + 0.5f) { Note("living world: test command %d: teleport: the ground at (%.0f, %.0f) reads %.1f m, under the water at %.1f m", seq, (double)x, (double)y, (double)pos[2], (double)water); return; }
    if (plan > -9000.0f && pos[2] < plan - 3.0f) { Note("living world: test command %d: teleport: the ground at (%.0f, %.0f) reads %.1f m, %.1f m under the height plan: not streamed in yet", seq, (double)x, (double)y, (double)pos[2], (double)(plan - pos[2])); return; }
    /* a solo Helldiver put among enemies dies and, reinforced on the same spot, dies again until the mission fails
     * (lw67a, lw71a: back on the ship 90 s later); such a point is refused */
    float nearest; int foes = LwEnemiesNear(x, y, 120.0f, &nearest);
    if (foes && !g_lwTeleportForce) { Note("living world: test command %d: teleport: %d enemies within 120 m of (%.0f, %.0f), the nearest %.0f m", seq, foes, (double)x, (double)y, (double)nearest); return; }
    pos[2] += 0.1f;
    ((void (*)(void*, uint32_t, const float*))(g_lw.game + 0x5A2BB0u))(NULL, e, pos);
    float back[3] = {0, 0, 0};
    if (EvpPtr(mgr + 0x48D0, &local)) SafeRead(local + 0xA4u * i, back, sizeof back);
    Note("living world: test command %d: teleported the Helldiver %08X to (%.0f, %.0f, %.1f); its motion record reads (%.0f, %.0f, %.1f)", seq, e,
        (double)pos[0], (double)pos[1], (double)pos[2], (double)back[0], (double)back[1], (double)back[2]);
}

/* Test command "aim X Y": how far the camera's view (player - camera, across the ground) is from the direction to (X, Y),
 * in degrees, counterclockwise positive (atan2 in the world's x/y); the harness turns the camera with its look input
 * until this is small, measuring the look's rate as it goes (closed loop: no assumed sensitivity or sign). */
static void LwTestAim(int seq, float x, float y)
{
    float f[2];
    if (g_lwWorld.playersAt == 0 || !LwCameraForward(f)) { Note("living world: test command %d: aim: no camera or player", seq); return; }
    float cam = atan2f(f[1], f[0]), to = atan2f(y - g_lwWorld.players[0][1], x - g_lwWorld.players[0][0]), off = to - cam;
    while (off > 3.14159265f) off -= 6.2831853f;
    while (off < -3.14159265f) off += 6.2831853f;
    /* pitch: the target's chest (ground + 1 m) seen from the camera against the y row's own elevation */
    uintptr_t cm = 0; float pose[16] = {0}, pitchOff = 0, tz = 0;
    if (EvpPtr(g_lw.game + 0x346D560u, &cm) && cm && !SafeRead(cm + 0xC, pose, sizeof pose) && LwGround(x, y, &tz))
    {
        float yl = sqrtf(pose[4] * pose[4] + pose[5] * pose[5] + pose[6] * pose[6]), dx = x - pose[12], dy = y - pose[13], dz = tz + 1.0f - pose[14];
        if (yl > 0.2f) pitchOff = (atan2f(dz, sqrtf(dx * dx + dy * dy)) - asinf(pose[6] / yl)) * 57.29578f;
    }
    Note("living world: test command %d: aim: camera %.1f deg, target %.1f deg, off by %+.1f deg; pitch off by %+.1f deg (view owner %d)", seq,
        (double)(cam * 57.29578f), (double)(to * 57.29578f), (double)(off * 57.29578f), (double)pitchOff, LwViewRead());
}

/* Test command "zoom K MS" (audit 1b criterion 8: no shot was zoomed; KB living-world-scope-draw-distance, checked by me:
 * FirstPersonVR sets the Camera's +0x34 at the same point live): for MS milliseconds the frame's vertical field of view
 * is the game's divided by K, as a K-times scope gives (the largest zoom in the game data is 4). Set in the engine's
 * script render step (exe 0x689BC0, hooked before it runs: the game's camera code has set its own FOV for the frame by
 * then, and the frame renders and culls with this one) through the Camera's set_vertical_fov (exe 0x30D990, Camera =
 * [[game+0x346D560]]); the game sets its own again the next frame (0x127A11A), so nothing stays behind. */
typedef void (*LwStepFn)(void* app);
static LwStepFn g_lwStep;
static volatile float g_lwZoomK; static volatile ULONGLONG g_lwZoomUntil; static float g_lwZoomBase;
static void LwStepDetour(void* app)
{
    if (g_lwZoomK > 1.0f && GetTickCount64() < g_lwZoomUntil)
    {
        uintptr_t holder = 0, cam = 0; float fov = 0;
        if (EvpPtr(g_lw.game + 0x346D560u, &holder) && holder && EvpPtr(holder, &cam) && cam && !SafeRead(cam + 0x34, &fov, 4) && fov > 0.1f && fov < 3.0f)
        {
            if (g_lwZoomBase == 0) g_lwZoomBase = fov;
            ((void (*)(uintptr_t, float))((uintptr_t)GetModuleHandleW(NULL) + 0x30D990u))(cam, fov / g_lwZoomK);
        }
    }
    g_lwStep(app);
}
static void LwTestZoom(int seq, float k, float ms)
{
    if (g_lwStep == NULL)
    {
        uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
        if (!ApiCodeMatches(exe + 0x30D990u, "F3 0F 11 49 34") || LoaderHook("living world zoom (test)", exe + 0x689BC0u, (void*)LwStepDetour, (void**)&g_lwStep) || g_lwStep == NULL)
        { Note("living world: test command %d: zoom: the render step or set_vertical_fov is not the reviewed code", seq); g_lwStep = NULL; return; }
    }
    g_lwZoomBase = 0; g_lwZoomK = k; g_lwZoomUntil = GetTickCount64() + (ULONGLONG)ms;
    Note("living world: test command %d: zoom %.1fx for %.0f ms", seq, (double)k, (double)ms);
}

/* Test command "look N": how many of squad N's members a ray from the camera's own position ([[game+0x346D560]]+0x3C,
 * as LwCameraForward reads it) reaches at chest height (1.0 m over the feet), through the same static-only sight ray as
 * LwSeenFrom. The spawn's "seen" test is from the player's eye; an aimed or zoomed camera stands elsewhere (lw38b
 * far1-zoom: a rock in front of the shoulder camera filled the frame). The harness takes a shot only when some are clear. */
static void LwTestLook(int seq, int which)
{
    typedef uint32_t (*RayFn)(uint32_t handle, const float* from, const float* dir, float length, uint32_t leaveOut, void* hit, uint32_t max);
    uintptr_t cm = 0, table = 0, fn = 0; float cam[3]; uint32_t members[32];
    if (which < 0 || which >= g_lwWorld.count || g_lwSquads[which].state != LW_ONLINE) { Note("living world: test command %d: look: squad %d is not online", seq, which); return; }
    if (!EvpPtr(g_lw.game + 0x346D560u, &cm) || cm == 0 || SafeRead(cm + 0x3C, cam, sizeof cam) || g_lwWorld.playersAt == 0) { Note("living world: test command %d: look: no camera", seq); return; }
    uint32_t handle = LwSightRay(&table);
    if (handle == 0 || !EvpPtr(table + 0x68, &fn) || fn == 0) { Note("living world: test command %d: look: no sight ray", seq); return; }
    /* where the clear members fall in the 1280 x 720 frame (the tests render at that size): the pose's rows as right (x),
     * forward (y) and up (z) (the y row is the view's axis, lw52a / lw53b; x and z by the same convention, checked by the
     * 60 m visible shots), the vertical FOV [[CM]+0x34], and 4x zoomed (the zoom shot's FOV / 4) */
    uintptr_t camObj = 0; float pose[16] = {0}, vfov = 0; int proj = EvpPtr(cm, &camObj) && camObj && !SafeRead(cm + 0xC, pose, sizeof pose) && !SafeRead(camObj + 0x34, &vfov, 4) && vfov > 0.1f && vfov < 3.0f;
    float box[2][4] = {{1e9f, 1e9f, -1e9f, -1e9f}, {1e9f, 1e9f, -1e9f, -1e9f}}; int inFrame[2] = {0, 0};
    int n = LwMembers(g_lwSquads[which].squad, members), clear = 0, read = 0; float nearest = 3e38f, farthest = 0, elevMin = 90, elevMax = -90;
    for (int k = 0; k < n && k < 32; k++)
    {
        ActHealth h; float p[3];
        if (!ActHealthOf(g_lw.game, members[k], &h) || !h.unit || !EvpUnitPosition(h.unit, p)) continue;
        read++;
        float d[3] = {p[0] - cam[0], p[1] - cam[1], p[2] + 1.0f - cam[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1.0f) { clear++; continue; }
        for (int i = 0; i < 3; i++) d[i] /= len;
        uint8_t hit[0x40] = {0}; float dist;
        uint32_t got = ((RayFn)fn)(handle, cam, d, len, g_lwWorld.units[0], hit, 1);
        memcpy(&dist, hit + 0x18, 4);
        if (got >= 1 && dist < len - 0.5f) continue;
        clear++;
        if (proj)
        {
            float w[3] = {p[0] - cam[0], p[1] - cam[1], p[2] + 1.0f - cam[2]};
            float xr = w[0] * pose[0] + w[1] * pose[1] + w[2] * pose[2], yf = w[0] * pose[4] + w[1] * pose[5] + w[2] * pose[6], zu = w[0] * pose[8] + w[1] * pose[9] + w[2] * pose[10];
            for (int z = 0; z < 2 && yf > 1.0f; z++)
            {
                float t = tanf(0.5f * vfov) / (z ? 4.0f : 1.0f), px = 640.0f + 640.0f * (xr / yf) / (t * 16.0f / 9.0f), py = 360.0f - 360.0f * (zu / yf) / t;
                if (px < box[z][0]) box[z][0] = px;
                if (py < box[z][1]) box[z][1] = py;
                if (px > box[z][2]) box[z][2] = px;
                if (py > box[z][3]) box[z][3] = py;
                if (px >= 0 && px < 1280 && py >= 0 && py < 720) inFrame[z]++;
            }
        }
        float e = asinf(d[2]) * 57.29578f;
        if (len < nearest) nearest = len;
        if (len > farthest) farthest = len;
        if (e < elevMin) elevMin = e;
        if (e > elevMax) elevMax = e;
    }
    if (clear) Note("living world: test command %d: look: squad %d: %d of %d members clear from the camera at (%.0f, %.0f, %.1f), %.0f .. %.0f m, %+.1f .. %+.1f deg up; "
        "in the frame at x %.0f..%.0f y %.0f..%.0f (%d in it), 4x zoomed x %.0f..%.0f y %.0f..%.0f (%d in it)", seq, which,
        clear, read, (double)cam[0], (double)cam[1], (double)cam[2], (double)nearest, (double)farthest, (double)elevMin, (double)elevMax,
        (double)box[0][0], (double)box[0][2], (double)box[0][1], (double)box[0][3], inFrame[0], (double)box[1][0], (double)box[1][2], (double)box[1][1], (double)box[1][3], inFrame[1]);
    else Note("living world: test command %d: look: squad %d: 0 of %d members clear from the camera at (%.0f, %.0f, %.1f)", seq, which, read, (double)cam[0], (double)cam[1], (double)cam[2]);
}

/* ---- drawn in the main view (test instrument, HELLFORGE_LW_DRAWN=1; user 2026-11-07: audits on data, not screenshots;
 * KB living-world-drawn-state, second pass, checked by me) ---------------------------------------------------------------
 * The engine keeps no per-unit "drawn" flag. The view render 0x1EA3E0 (render thread) culls through 0x57A7B0(ctx), which
 * only starts the jobs; the visible lists are complete once 0x1E2F40 (LOD selection, only caller 0x1EDA6E, rdx = P with
 * ctx = [P+0x88], r8 = the render world's objects [rw+0x170]) has waited on its sets {4, 2, 3} (P+0x78; 0x1E2F90). Set 4
 * ([[ctx+0x18]+0x20]) is the meshes' cull set: after the sync job 0x5778D0, set+0x70 is the visible count (0x577E91) and
 * set+0x38 the visible cull indices ci; [cs+0x128][ci] is the object's type (3 = a mesh, constructor 0x510AC0) and
 * [cs+0x130][ci] its slot in the objects. A mesh's +0x44 is its unit's scene-graph render handle (0x510C19..0x510C46:
 * {[M+0xC8], [[M+0xC0]+4]} where M+0xC0 is the unit's scene graph, object+0x48 or +0x60 as EvpUnitGraph reads it). So a
 * unit was in a view's visible list when [graph+4] of its scene graph is among the visible meshes' +0x44: no positions.
 * A unit with LOD groups (enemies: their meshes carry [obj+0xA8] bit 3, no cull entry of their own, 0x1E640B, and bit 9)
 * is culled by its LOD object instead: render type 0xD (constructor 0x353F40), one cull entry per bounding volume of
 * kind 0xD (| 0x80000000 with more than one, 0x1E6A51..0x1E6A90), its +0x18 entries {node, the unit's scene-graph
 * handle [SG+4], ...} (0x3541A1..0x3541CD): its handle is read there.
 * The list is the frustum and small-object cull; the LOD pass after it can still drop a mesh (not counted here).
 * 0x1EA3E0 renders several views a frame; results are kept per view (ctx+0x28, the render camera), and a view counts as
 * the main one when its camera's position (+0xE8, as the cull kernel reads it) is within 0.5 m of the game camera's
 * (CM pose +0x3C) and it is no cubemap face (FOV +0x2C not pi/2). */
#define LW_DRAWN_MAX 64
#define LW_DRAWN_VIEWS 16
static struct { uint32_t handle, unit; } g_lwDrawnT[LW_DRAWN_MAX];
static volatile LONG g_lwDrawnN; static volatile float g_lwDrawnFov;
static struct { uintptr_t view; volatile LONG64 calls; volatile LONG main; LONG64 hit[LW_DRAWN_MAX];
                /* diagnostics: where the last call stopped (0 = it walked the list), the list's count, its meshes,
                 * and the first meshes' handles */
                volatile LONG stage, cnt, meshes; uint32_t sample[4];
                /* the view's render world rw = [ctx]-0x10, its camera's position (+0xE8) and FOV (+0x2C), which cull
                 * set its set 4 is ('A' [rw+0x268]+0x50, 'B' [rw+0x280]+0x50, '?' other) and that set's input count
                 * [cs+0x148] (KB living-world-drawn-state, third pass) */
                uintptr_t rw; float cam[3], fov; char which; uint32_t input; volatile ULONGLONG last; } g_lwDrawnV[LW_DRAWN_VIEWS];
static volatile float g_lwDrawnCam[3];
/* diagnostics (main view, every 2 s): the kinds in its visible list (lw70a: the members' 48 meshes carry flags 0x20A, bit
 * 3 = no cull entry of their own, bit 9 = in a LOD group: the unit is culled by its LOD object, kind 0xD) */
static volatile LONG g_lwDrawnKinds[16];
/* 0x1E2F40 takes nine arguments (the call 0x1EDA23..0x1EDA69 stores five at [rsp+0x20..0x40]) and also writes the
 * tenth slot as scratch ([rbp+0x298] = the caller's [rsp+0x48], 0x1E32C4 before its reads): the detour forwards all nine
 * and passes a tenth so its own frame reserves that slot (lw64a: four forwarded crashed it at 0x1E3BE5). */
typedef void (*LwLodFn)(uintptr_t a, uintptr_t p, uintptr_t objs, uintptr_t d, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t scratch);
static LwLodFn g_lwLod;
static int LwRegionLimit(uintptr_t base, uint64_t* n4)
{
    MEMORY_BASIC_INFORMATION mb;
    if (!base || !VirtualQuery((LPCVOID)base, &mb, sizeof mb) || mb.State != MEM_COMMIT) return 0;
    uint64_t n = ((uintptr_t)mb.BaseAddress + mb.RegionSize - base) / 4; if (n < *n4) *n4 = n; return 1;
}
static void LwLodDetour(uintptr_t a, uintptr_t p, uintptr_t objs, uintptr_t d, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9)
{
    g_lwLod(a, p, objs, d, a5, a6, a7, a8, a9, 0);
    int n = (int)g_lwDrawnN;
    uintptr_t ctx = 0, arr = 0, set = 0, view = 0, cs = 0, idx = 0, K = 0, S = 0, done = 0; uint32_t sets = 0, cnt = 0, fin = 0;
    if (SafeRead(p + 0x88, &ctx, 8) || !ctx || SafeRead(ctx + 0x28, &view, 8) || !view) return;
    /* a slot per render camera; a camera not rendered for 3 s gives its slot up (lw68a: loading-screen and cubemap
     * cameras filled 8 slots before the gameplay one came) */
    ULONGLONG tick = GetTickCount64();
    int v = 0; while (v < LW_DRAWN_VIEWS && g_lwDrawnV[v].view != view) v++;
    if (v == LW_DRAWN_VIEWS)
    {
        for (v = 0; v < LW_DRAWN_VIEWS && g_lwDrawnV[v].view && tick - g_lwDrawnV[v].last < 3000; v++) {}
        if (v == LW_DRAWN_VIEWS) return;
        memset((void*)&g_lwDrawnV[v], 0, sizeof g_lwDrawnV[v]);
        g_lwDrawnV[v].view = view;
    }
    g_lwDrawnV[v].last = tick;
    LONG64 call = InterlockedIncrement64(&g_lwDrawnV[v].calls);
    {   /* the main view: its camera stands where the game camera is (within 0.5 m) */
        uintptr_t ctx0 = 0; float c[3] = {0}, f = 0;
        if (!SafeRead(ctx, &ctx0, 8) && ctx0) g_lwDrawnV[v].rw = ctx0 - 0x10;
        if (!SafeRead(view + 0xE8, c, 12) && !SafeRead(view + 0x2C, &f, 4))
        {
            float dx = c[0] - g_lwDrawnCam[0], dy = c[1] - g_lwDrawnCam[1], dz = c[2] - g_lwDrawnCam[2];
            memcpy(g_lwDrawnV[v].cam, c, sizeof c); g_lwDrawnV[v].fov = f;
            /* and it is no cubemap face (lw68a: six faces, FOV exactly pi/2, stand at the camera too). Not the game
             * camera's FOV itself: a zoom narrows the render camera's only (lw75a: 4x zoomed, no view matched) */
            g_lwDrawnV[v].main = dx * dx + dy * dy + dz * dz < 0.25f && fabsf(f - 1.5707963f) > 0.01f && f > 0.01f && f < 3.0f;
        }
    }
    if (n <= 0 || n > LW_DRAWN_MAX) { g_lwDrawnV[v].stage = 1; return; }
    g_lwDrawnV[v].stage = 2;
    if (SafeRead(ctx + 0x14, &sets, 4) || sets < 5 || SafeRead(ctx + 0x18, &arr, 8) || !arr || SafeRead(arr + 0x20, &set, 8) || !set) return;
    g_lwDrawnV[v].stage = 3;
    if (SafeRead(ctx + 0x60, &done, 8) || !done || SafeRead(done + 16, &fin, 4) || fin != 1) return;
    g_lwDrawnV[v].stage = 4;
    {
        uintptr_t rw = g_lwDrawnV[v].rw, a = 0, b = 0, c = 0; uint32_t in = 0;
        SafeRead(set + 0x68, &c, 8);
        if (rw) { SafeRead(rw + 0x268, &a, 8); SafeRead(rw + 0x280, &b, 8); }
        g_lwDrawnV[v].which = c && a && c == a + 0x50 ? 'A' : c && b && c == b + 0x50 ? 'B' : '?';
        if (c && !SafeRead(c + 0x148, &in, 4)) g_lwDrawnV[v].input = in;
    }
    if (SafeRead(set + 0x68, &cs, 8) || !cs || SafeRead(set + 0x70, &cnt, 4) || SafeRead(set + 0x38, &idx, 8) ||
        SafeRead(cs + 0x128, &K, 8) || SafeRead(cs + 0x130, &S, 8)) return;
    g_lwDrawnV[v].stage = 5;
    uint64_t lim = ~0ull, lc = ~0ull, ls = ~0ull; /* a safety net: nothing read past a committed region */
    if (!LwRegionLimit(K, &lim) || !LwRegionLimit(S, &lim) || !LwRegionLimit(idx, &lc) || !LwRegionLimit(objs, &ls)) return;
    if (cnt > lc) cnt = (uint32_t)lc;
    const uint32_t* list = (const uint32_t*)idx; int meshes = 0;
    for (uint32_t j = 0; j < cnt; j++)
    {
        uint32_t ci = list[j];
        if (ci >= lim) continue;
        uint32_t kind = ((const uint32_t*)K)[ci] & 0x7FFFFFFFu;
        if (kind != 3 && kind != 0xD) continue;
        uint32_t slot = ((const uint32_t*)S)[ci];
        if ((uint64_t)slot * 2 + 1 >= ls) continue;
        uintptr_t obj = ((const uintptr_t*)objs)[slot];
        if (obj < 0x10000 || *(const uint32_t*)(obj + 8) != kind) continue;
        uint32_t h;
        if (kind == 3) h = *(const uint32_t*)(obj + 0x44); /* a mesh with its own cull entry */
        else
        {   /* a unit's LOD object: its bounding volumes' entries {node, scene-graph handle, ...} at +0x18 */
            uintptr_t bv = *(const uintptr_t*)(obj + 0x18);
            if (bv < 0x10000) continue;
            h = *(const uint32_t*)(bv + 4);
        }
        if (meshes < 4) g_lwDrawnV[v].sample[meshes] = h;
        meshes++;
        for (int t = 0; t < n; t++) if (g_lwDrawnT[t].handle == h) g_lwDrawnV[v].hit[t] = call;
    }
    g_lwDrawnV[v].stage = 0; g_lwDrawnV[v].cnt = (LONG)cnt; g_lwDrawnV[v].meshes = meshes;
    static ULONGLONG scanAt;
    if (g_lwDrawnV[v].main && tick - scanAt >= 2000 && g_lwDrawnV[v].rw)
    {
        scanAt = tick;
        LONG kinds[16] = {0};
        for (uint32_t j = 0; j < cnt; j++) { uint32_t ci = list[j]; if (ci < lim) { uint32_t k = ((const uint32_t*)K)[ci] & 0x7FFFFFFFu; kinds[k < 15 ? k : 15]++; } }
        for (int k = 0; k < 16; k++) g_lwDrawnKinds[k] = kinds[k];
    }
}
/* A unit's scene graph (object+0x48 or +0x60 by its class, as EvpUnitPosition reads it), its render handle at +4. */
static uint32_t LwUnitRenderHandle(uint32_t unit)
{
    uintptr_t graph = EvpUnitGraph(unit); uint32_t h = 0;
    return graph && !SafeRead(graph + 4, &h, 4) && h != 0xFFFFFFu ? h : 0;
}
/* Game thread, each tick: the hook (once); the targets every 250 ms (the members of online squads, those beyond 250 m
 * first, the near ones as the control); every 5 s per squad, how many members each view had in its visible list within
 * its last 0.5 s of calls. */
static void LwDrawnTick(void)
{
    static int on = -1, hooked; static ULONGLONG pubAt, noteAt, rateAt; static LONG64 callsAt[LW_DRAWN_VIEWS]; static double rate[LW_DRAWN_VIEWS];
    if (on < 0) { char t[4] = ""; on = GetEnvironmentVariableA("HELLFORGE_LW_DRAWN", t, sizeof t) > 0 && t[0] == '1'; }
    if (!on) return;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (!hooked)
    {
        hooked = -1;
        if (!ApiCodeMatches(exe + 0x1E2F40u, "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57") ||
            !ApiCodeMatches(exe + 0x1E2F6Fu, "48 8B BA 88 00 00 00 48 8D 5A 78 4C 8B F2 4C 8B F9 BE 03 00 00 00") ||
            !ApiCodeMatches(exe + 0x1EDA6Eu, "E8 CD 54 FF FF") || !ApiCodeMatches(exe + 0x577E91u, "45 89 69 70") ||
            !ApiCodeMatches(exe + 0x510C19u, "49 8B 80 C0 00 00 00 41 8B 88 C8 00 00 00 89 4D B7 48 85 C0 74 08 8B 40 04 89 45 BB") ||
            !ApiCodeMatches(exe + 0x510C42u, "48 8B 47 70 0F 11 47 40") ||
            !ApiCodeMatches(exe + 0x1E6405u, "8B 97 A8 00 00 00 F6 C2 08 0F 85") || !ApiCodeMatches(exe + 0x353F54u, "C7 41 08 0D 00 00 00") ||
            !ApiCodeMatches(exe + 0x3541A1u, "49 8B 56 18 8B 41 10 49 03 C2 8B 44 81 14 8B 4C 08 1C 8B 42 04 89 44 24 24") ||
            !ApiCodeMatches(exe + 0x1E6A90u, "0B 57 08"))
        { Note("living world: drawn: the LOD selection or the mesh records are not the reviewed code; no drawn notes"); return; }
        if (LoaderHook("living world drawn (test)", exe + 0x1E2F40u, (void*)LwLodDetour, (void**)&g_lwLod) || g_lwLod == NULL) { Note("living world: drawn: the hook failed"); return; }
        hooked = 1; Note("living world: drawn: the LOD selection is hooked (0x1E2F40): squad members are matched by their scene graph's render handle to the meshes' visible list (set 4), per view");
    }
    if (hooked != 1) return;
    ULONGLONG now = GetTickCount64();
    uintptr_t holder = 0, cam = 0; float fov = 0;
    if (EvpPtr(g_lw.game + 0x346D560u, &holder) && holder && EvpPtr(holder, &cam) && cam && !SafeRead(cam + 0x34, &fov, 4)) g_lwDrawnFov = fov;
    { float c[3]; if (holder && !SafeRead(holder + 0x3C, c, 12)) { g_lwDrawnCam[0] = c[0]; g_lwDrawnCam[1] = c[1]; g_lwDrawnCam[2] = c[2]; } }
    if (now - pubAt >= 250)
    {
        pubAt = now;
        int n = 0;
        for (int pass = 0; pass < 2; pass++) /* the far squads first; then the near ones, as the instrument's control */
        for (int i = 0; i < g_lwWorld.count && n < LW_DRAWN_MAX; i++)
        {
            LwSquad* q = &g_lwSquads[i]; uint32_t members[32];
            if (q->state != LW_ONLINE || (LwNearestPlayer2(q->pos[0], q->pos[1]) < LW_R_ON * LW_R_ON) != (pass == 1)) continue;
            int m = LwMembers(q->squad, members);
            for (int k = 0; k < m && n < LW_DRAWN_MAX; k++)
            {
                ActHealth h; uint32_t rh;
                if (!ActHealthOf(g_lw.game, members[k], &h) || !h.unit || (rh = LwUnitRenderHandle(h.unit)) == 0) continue;
                if (g_lwDrawnT[n].unit != members[k]) for (int v = 0; v < LW_DRAWN_VIEWS; v++) g_lwDrawnV[v].hit[n] = 0;
                g_lwDrawnT[n].handle = rh; g_lwDrawnT[n].unit = members[k];
                n++;
            }
        }
        g_lwDrawnN = n;
    }
    if (now - rateAt >= 5000)
    {
        double dt = rateAt ? (double)(now - rateAt) / 1000.0 : 0; rateAt = now;
        for (int v = 0; v < LW_DRAWN_VIEWS; v++) { LONG64 c = g_lwDrawnV[v].calls; rate[v] = dt > 0 ? (double)(c - callsAt[v]) / dt : 0; callsAt[v] = c; }
    }
    if (now - noteAt < 5000) return;
    noteAt = now;
    char views[1024]; int w = 0;
    for (int v = 0; v < LW_DRAWN_VIEWS; v++)
        if (g_lwDrawnV[v].view && now - g_lwDrawnV[v].last < 3000) w += snprintf(views + w, sizeof views - (size_t)w, "%sview %d %.0f/s%s", v ? ", " : "", v, rate[v], g_lwDrawnV[v].main ? " (main: its camera is the game camera's place)" : "");
    {   /* diagnostics: each view's last walk (stage 0 = walked: its visible count, meshes, first mesh handles) and the
         * first targets' handles */
        char dg[1400]; int dw = 0;
        for (int v = 0; v < LW_DRAWN_VIEWS && dw < 1250; v++)
            if (g_lwDrawnV[v].view && now - g_lwDrawnV[v].last < 3000) dw += snprintf(dg + dw, sizeof dg - (size_t)dw, "%sview %d rw %llX cam (%.1f, %.1f, %.1f) fov %.2f set4 %c in %u: stage %ld, %ld visible, %ld meshes, %06X %06X", v ? "; " : "", v,
                (unsigned long long)g_lwDrawnV[v].rw, (double)g_lwDrawnV[v].cam[0], (double)g_lwDrawnV[v].cam[1], (double)g_lwDrawnV[v].cam[2], (double)g_lwDrawnV[v].fov,
                g_lwDrawnV[v].which ? g_lwDrawnV[v].which : '-', g_lwDrawnV[v].input, (long)g_lwDrawnV[v].stage,
                (long)g_lwDrawnV[v].cnt, (long)g_lwDrawnV[v].meshes, g_lwDrawnV[v].sample[0], g_lwDrawnV[v].sample[1]);
        char tg[128]; int tw = 0;
        for (int t = 0; t < (int)g_lwDrawnN && t < 6; t++) tw += snprintf(tg + tw, sizeof tg - (size_t)tw, "%s%06X", t ? " " : "", g_lwDrawnT[t].handle);
        Note("living world: drawn: the main view's visible kinds 0:%ld 1:%ld 2:%ld 3:%ld 4:%ld 5:%ld 6:%ld 7:%ld 8:%ld 9:%ld 10+:%ld",
            (long)g_lwDrawnKinds[0], (long)g_lwDrawnKinds[1], (long)g_lwDrawnKinds[2], (long)g_lwDrawnKinds[3], (long)g_lwDrawnKinds[4], (long)g_lwDrawnKinds[5],
            (long)g_lwDrawnKinds[6], (long)g_lwDrawnKinds[7], (long)g_lwDrawnKinds[8], (long)g_lwDrawnKinds[9],
            (long)(g_lwDrawnKinds[10] + g_lwDrawnKinds[11] + g_lwDrawnKinds[12] + g_lwDrawnKinds[13] + g_lwDrawnKinds[14] + g_lwDrawnKinds[15]));
        Note("living world: drawn: diagnostics: game camera (%.1f, %.1f, %.1f); %s; targets %d, first handles %s", (double)g_lwDrawnCam[0], (double)g_lwDrawnCam[1], (double)g_lwDrawnCam[2],
            dw ? dg : "no views", (int)g_lwDrawnN, tw ? tg : "none");
    }
    Note("living world: drawn: views %s; vertical FOV %.1f deg%s", w ? views : "none yet", (double)(g_lwDrawnFov * 57.29578f),
        g_lwZoomK > 1.0f && GetTickCount64() < g_lwZoomUntil ? ", zoomed 4x by the test" : "");
    /* each member also against the game camera: inside its frame (the look command's projection: the pose rows, the
     * vertical FOV [[CM]+0x34] divided by the test zoom, 1280 x 720) and clear of the static world (the sight ray) */
    uintptr_t cmP = 0, camObj = 0, table = 0, rayFn = 0; float pose[16] = {0}, camPos[3] = {0}, vf = 0;
    int proj = EvpPtr(g_lw.game + 0x346D560u, &cmP) && cmP && !SafeRead(cmP + 0xC, pose, sizeof pose) && !SafeRead(cmP + 0x3C, camPos, sizeof camPos) &&
        EvpPtr(cmP, &camObj) && camObj && !SafeRead(camObj + 0x34, &vf, 4) && vf > 0.1f && vf < 3.0f;
    float zk = g_lwZoomK > 1.0f && GetTickCount64() < g_lwZoomUntil ? g_lwZoomK : 1.0f;
    typedef uint32_t (*RayFn)(uint32_t handle, const float* from, const float* dir, float length, uint32_t leaveOut, void* hit, uint32_t max);
    uint32_t ray = proj ? LwSightRay(&table) : 0;
    if (ray && (!EvpPtr(table + 0x68, &rayFn) || !rayFn)) ray = 0;
    for (int i = 0; i < g_lwWorld.count; i++)
    {
        LwSquad* q = &g_lwSquads[i]; uint32_t members[32]; int m, total = 0, inMain = 0, inAny = 0, inFrame = 0, clearOf = 0, joint = 0, eyeClear = 0;
        float blockAt = -1, blockLen = 0; uint32_t blockOwner = 0; /* the first in-frame member the camera ray does not reach: where the ray stopped */
        if (q->state != LW_ONLINE) continue;
        m = LwMembers(q->squad, members);
        char per[160]; int pw = 0; int cntv[LW_DRAWN_VIEWS] = {0};
        for (int t = 0; t < (int)g_lwDrawnN; t++)
        {
            int mine = 0;
            for (int k = 0; k < m && !mine; k++) mine = members[k] == g_lwDrawnT[t].unit;
            if (!mine) continue;
            total++; int any = 0, main = 0, fIn = 0, fClear = 0;
            {
                ActHealth hh; float p[3];
                if (proj && ActHealthOf(g_lw.game, g_lwDrawnT[t].unit, &hh) && hh.unit && EvpUnitPosition(hh.unit, p))
                {
                    float w[3] = {p[0] - camPos[0], p[1] - camPos[1], p[2] + 1.0f - camPos[2]};
                    float xr = w[0] * pose[0] + w[1] * pose[1] + w[2] * pose[2], yf = w[0] * pose[4] + w[1] * pose[5] + w[2] * pose[6], zu = w[0] * pose[8] + w[1] * pose[9] + w[2] * pose[10];
                    float tt = tanf(0.5f * vf) / zk;
                    if (yf > 1.0f)
                    {
                        float px = 640.0f + 640.0f * (xr / yf) / (tt * 16.0f / 9.0f), py = 360.0f - 360.0f * (zu / yf) / tt;
                        if (px >= 0 && px < 1280 && py >= 0 && py < 720) { inFrame++; fIn = 1; }
                    }
                    float len = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
                    if (ray && len > 1.0f)
                    {
                        float d[3] = {w[0] / len, w[1] / len, w[2] / len}, dist; uint8_t hit[0x40] = {0};
                        uint32_t got = ((RayFn)rayFn)(ray, camPos, d, len, g_lwWorld.units[0], hit, 1);
                        memcpy(&dist, hit + 0x18, 4);
                        if (!(got >= 1 && dist < len - 0.5f)) { clearOf++; fClear = 1; }
                        else if (fIn && blockAt < 0) { blockAt = dist; blockLen = len; memcpy(&blockOwner, hit + 0x1C, 4); }
                    }
                    if (g_lwWorld.playersAt > 0) { float chest[3] = {p[0], p[1], p[2] + 1.0f}; if (LwSeenFrom(g_lwWorld.players[0], g_lwWorld.units[0], chest) == 1) eyeClear++; }
                }
            }
            for (int v = 0; v < LW_DRAWN_VIEWS && g_lwDrawnV[v].view; v++)
            {
                LONG64 recent = (LONG64)(rate[v] * 0.5 + 1.0), c = g_lwDrawnV[v].calls;
                if (g_lwDrawnV[v].hit[t] && c - g_lwDrawnV[v].hit[t] <= recent) { cntv[v]++; any = 1; if (g_lwDrawnV[v].main) main = 1; }
            }
            inAny += any; inMain += main; joint += main && fIn && fClear;
        }
        if (total == 0) continue;
        for (int v = 0; v < LW_DRAWN_VIEWS && g_lwDrawnV[v].view; v++) pw += snprintf(per + pw, sizeof per - (size_t)pw, "%s%d", v ? " " : "", cntv[v]);
        char blk[96] = "";
        if (blockAt >= 0) snprintf(blk, sizeof blk, "; the camera ray stopped %.1f m out of %.0f (unit %08X)", (double)blockAt, (double)blockLen, blockOwner);
        Note("living world: drawn: squad %d at %.0f m: %d of %d members in the main view's visible list in the last 0.5 s (%d in any view; per view %s); %d in the camera's frame%s, %d clear of the static world from it; %d in the frame, clear and in the list together; %d clear from the eye%s", i,
            (double)sqrtf(LwNearestPlayer2(q->pos[0], q->pos[1])), inMain, total, inAny, per, inFrame, zk > 1.0f ? " (zoomed)" : "", clearOf, joint, eyeClear, blk);
    }
}

/* Test runs (HELLFORGE_LW_TEST_TRACE=1): when the local Helldiver dies, who killed it, read as for a lost member
 * (LwLostWhy: the health record's attacker +0x30, creditor +0x38, damage hash +0x40), with where it stood (lw72a: it died
 * about 10 s after each landing, again and again, with no squad near). */
static void LwAvatarDeathWatch(void)
{
    static int on = -1; static uint32_t lastDead; static float at[3];
    if (on < 0) { char t[4] = ""; on = GetEnvironmentVariableA("HELLFORGE_LW_TEST_TRACE", t, sizeof t) > 0 && t[0] == '1'; }
    if (!on) return;
    ActHealth h; const char* why = ""; float p[3];
    uint32_t e = ActLocalAvatar(g_lw.game, 1, &h, &why); /* 1: downed or dead too */
    if (!e) return;
    if (h.life < 2) { if (h.unit && EvpUnitPosition(h.unit, p)) memcpy(at, p, sizeof p); return; }
    if (e == lastDead) return;
    lastDead = e;
    uint32_t squad; char by[400];
    const char* what = LwLostWhy(e, &squad, by, sizeof by, at);
    float nearest; int foes = LwEnemiesNear(at[0], at[1], 60.0f, &nearest);
    Note("living world: test: the Helldiver %08X died at (%.0f, %.0f, %.1f): %s%s; health %d of %d; %d enemies within 60 m (nearest %.0f m)", e, (double)at[0], (double)at[1], (double)at[2],
        what, by, h.health, h.max, foes, (double)nearest);
}
/* Test runs (HELLFORGE_LW_TEST_TRACE=1): a safe landing. The default drop is the map centre, and on some maps that is
 * inside an outpost (lw73a: 28 enemies within 60 m, the nearest 3 m; a garrison enemy killed the Helldiver, again after
 * every reinforcement until the mission failed). When the Helldiver leaves its pod with live enemies within 80 m, it is
 * moved (the test teleport, which refuses points with enemies within 120 m) to the nearest point 100..300 m away with
 * none within 120 m. */
static void LwSafeLanding(void)
{
    static int on = -1, wasAttached = 1, noted; static ULONGLONG landedAt;
    if (on < 0) { char t[4] = ""; on = GetEnvironmentVariableA("HELLFORGE_LW_TEST_TRACE", t, sizeof t) > 0 && t[0] == '1'; }
    if (!on) return;
    ActHealth h; const char* why = ""; float p[3];
    uint32_t e = ActLocalAvatar(g_lw.game, 0, &h, &why);
    if (!e || !h.unit || !EvpUnitPosition(h.unit, p)) { wasAttached = 1; return; } /* dead or not spawned: the next landing counts */
    int attached = ((uint8_t (*)(void*, uint32_t))(g_lw.game + 0x4A52B0u))(NULL, e) != 0;
    if (wasAttached && !attached) { landedAt = GetTickCount64(); noted = 0; }
    wasAttached = attached;
    /* for 8 s after it leaves the pod: an outpost's guards spawn as a player comes near */
    if (attached || !landedAt || GetTickCount64() - landedAt > 8000) return;
    float nearest; int foes = LwEnemiesNear(p[0], p[1], 80.0f, &nearest);
    if (!foes)
    {
        if (!noted++) Note("living world: test: safe landing: the Helldiver is out of its pod at (%.0f, %.0f), no enemy within 80 m (nearest %.0f m); watched 8 s", (double)p[0], (double)p[1], (double)nearest);
        return;
    }
    landedAt = 0;
    for (float r = 100.0f; r <= 300.0f; r += 50.0f) /* lw74a: none clear of 150 m within 150..300 m on a 350 m disc */
        for (int k = 0; k < 12; k++)
        {
            float a = (float)k * 0.5235988f, x = p[0] + r * cosf(a), y = p[1] + r * sinf(a), z, n2;
            if (x * x + y * y > 0.9f * 0.9f * g_lwWorld.radius * g_lwWorld.radius || !LwGround(x, y, &z) || LwEnemiesNear(x, y, 120.0f, &n2)) continue;
            Note("living world: test: safe landing: %d enemies within 80 m of the pod at (%.0f, %.0f) (nearest %.0f m): moving the Helldiver %.0f m to (%.0f, %.0f)",
                foes, (double)p[0], (double)p[1], (double)nearest, (double)r, (double)x, (double)y);
            LwTestTeleport(0, x, y);
            return;
        }
    Note("living world: test: safe landing: %d enemies within 80 m of the pod at (%.0f, %.0f) and no clear point 100..300 m around", foes, (double)p[0], (double)p[1]);
}
/* Test runs only (HELLFORGE_LW_TEST_ALIVE=1, set by the living-world test scripts): the local Helldiver is healed back to
 * full each tick through the API's heal (ActHealNow: the game's own heal, pinned). The tests put enemy squads 60-170 m
 * from a solo Helldiver on purpose (lw74a: killed by 29 enemies within 60 m); a death ends the run's checks or, after
 * repeated reinforcements, the mission. A hit larger than full health still kills. */
static void LwTestKeepAlive(void)
{
    static int on = -1; static ULONGLONG notedAt;
    if (on < 0) { char t[4] = ""; on = GetEnvironmentVariableA("HELLFORGE_LW_TEST_ALIVE", t, sizeof t) > 0 && t[0] == '1'; if (on) Note("living world: test: the local Helldiver is kept alive (healed to full each tick; test runs only)"); }
    if (!on) return;
    ActHealth h; const char* why = "";
    if (!ActLocalAvatar(g_lw.game, 0, &h, &why) || h.max <= 0 || h.health >= h.max) return;
    int before = h.health;
    ActHealNow(NULL, g_lw.game, (float)h.max);
    if (GetTickCount64() - notedAt > 10000) { notedAt = GetTickCount64(); Note("living world: test: keep alive: the Helldiver healed from %d of %d", before, h.max); }
}
static void LwKillArm(void);
static void LwTestCommands(void)
{
    char cmds[8][128]; int n, seq0;
    static int on = -1; /* test runs only (audit 1d, 11): the exported channel acts only with HELLFORGE_LW_TEST_TRACE=1 */
    if (on < 0) { char t[4] = ""; on = GetEnvironmentVariableA("HELLFORGE_LW_TEST_TRACE", t, sizeof t) > 0 && t[0] == '1'; }
    AcquireSRWLockExclusive(&g_lwCmdLock);
    n = g_lwCmdCount; seq0 = g_lwCmdSeq - n + 1;
    memcpy(cmds, g_lwCmd, sizeof cmds); g_lwCmdCount = 0;
    ReleaseSRWLockExclusive(&g_lwCmdLock);
    if (!on) { if (n) Note("living world: test commands ignored: %d (HELLFORGE_LW_TEST_TRACE is not 1)", n); return; }
    for (int c = 0; c < n; c++)
    {
        char mode[16] = ""; float d = 0, deg = 0; int idx = -1, seq = seq0 + c;
        Note("living world: test command %d: %s", seq, cmds[c]);
        if (sscanf(cmds[c], "spawn %15s %f %f %d", mode, &d, &deg, &idx) >= 2) LwTestSpawn(seq, mode, d, deg, idx);
        else if (sscanf(cmds[c], "offline %d", &idx) == 1 && idx >= 0 && idx < g_lwWorld.count && g_lwSquads[idx].state == LW_ONLINE) g_lwSquads[idx].forceOff = 1;
        else if (strncmp(cmds[c], "kill garrison", 13) == 0) LwKillArm();
        else if (strncmp(cmds[c], "clear garrison", 14) == 0) LwTestClear(seq);
        else if (sscanf(cmds[c], "retake wait %f", &d) == 1) LwRetakeWait(seq, d);
        else if (sscanf(cmds[c], "teleport! %f %f", &d, &deg) == 2) { g_lwTeleportForce = 1; LwTestTeleport(seq, d, deg); g_lwTeleportForce = 0; }
        else if (sscanf(cmds[c], "teleport %f %f", &d, &deg) == 2) LwTestTeleport(seq, d, deg);
        else if (strcmp(cmds[c], "vantage") == 0) LwTestVantage(seq);
        else if (sscanf(cmds[c], "noise %d %f %f", &idx, &d, &deg) >= 1) LwTestNoise(seq, idx, d, deg);
        else if (strcmp(cmds[c], "garrisons") == 0) LwTestGarrisons(seq);
        else if (sscanf(cmds[c], "stage %f", &d) == 1 && d > LW_R_ON && d < 2000.0f) LwTestStage(seq, d);
        else if (sscanf(cmds[c], "aim %f %f", &d, &deg) == 2) LwTestAim(seq, d, deg);
        else if (sscanf(cmds[c], "look %d", &idx) == 1) LwTestLook(seq, idx);
        else if (sscanf(cmds[c], "zoom %f %f", &d, &deg) == 2 && d >= 1.0f && d <= 8.0f && deg > 0 && deg <= 20000) LwTestZoom(seq, d, deg);
        else if (strncmp(cmds[c], "status", 6) == 0)
            for (int i = 0; i < g_lwWorld.count; i++)
            {
                LwSquad* q = &g_lwSquads[i];
                if (q->state != LW_ONLINE && q->state != LW_PENDING) continue;
                uint32_t ms[32]; int mn = q->state == LW_ONLINE ? LwMembers(q->squad, ms) : 0, alerted = 0;
                for (int k = 0; k < mn; k++) alerted += LwAlert(ms[k]) != 0;
                uintptr_t beh = 0, brecs = 0; uint32_t bi = 0, bdet = 0xFFFFFFFFu; /* the awareness update's detector of the first member (Behavior +0x68, 0x7ECF70 / 0x7EB262) */
                if (mn > 0 && EvpPtr(g_lw.game + 0x3326740u, &beh) && EvpHash(beh + 0x40, ms[0], 8, 4, &bi) && EvpPtr(beh + 0x60, &brecs)) bdet = EvpU32(brecs + 0x1F8ull * bi + 0x68, 0xFFFFFFFFu);
                if (mn > 0) Note("living world: test command %d: squad %d member %08X: awareness detector %08X", seq, i, ms[0], bdet);
                Note("living world: test command %d: squad %d %s at (%.0f, %.0f), %.0f m, %u alive of %u, %s%s, %d alerted", seq, i, q->state == LW_ONLINE ? "online" : "pending",
                    (double)q->pos[0], (double)q->pos[1], (double)sqrtf(LwNearestPlayer2(q->pos[0], q->pos[1])), q->alive, q->cap,
                    q->farOn ? "by sightline" : "near", q->inView ? ", in a player's view" : "", alerted);
            }
        else Note("living world: test command %d: not understood", seq);
    }
}

/* (The presence pull, which walked an offline squad toward a player with none near, is gone: on extra large maps the
 * walking grid spreads squads evenly instead; audit 1e, 7.) */

static void LwSquadsTick(void)
{
    LwSmashTableUpdate();
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
        if (!LwGridRoutes(g_lwWorld.radius)) { g_lwNodeCount = 0; return; } /* copied again next tick, then the grid */
    }
    if (!g_lwWorld.populated) { if (g_lwWorld.playersAt == 0) { LwWhy("no player position yet"); return; } LwPopulate(); if (g_lwWorld.populated <= 0) return; }
    ULONGLONG now = GetTickCount64();
    float dt = g_lwWorld.last ? (float)(now - g_lwWorld.last) / 1000.0f : 0;
    if (dt > 1.0f) dt = 1.0f;
    g_lwWorld.last = now;
    if (now - g_lwWorld.effAt > 5000) { int eff = LwEffectiveCap(dir); if (eff > 0) g_lwWorld.eff = eff; g_lwWorld.effAt = now; }
    int requested = 0, removals = 0, online = 0, waiting = 0, units = 0, attempts = 0, farTries = 0; /* separate budgets: far checks never starve near squads (lw16l) */
    float r = g_lwWorld.radius;
    for (int i = 0; i < g_lwWorld.count; i++) /* the budget: living members of online squads, the request of pending ones */
        units += g_lwSquads[i].state == LW_PENDING ? g_lwSquads[i].cap : g_lwSquads[i].state == LW_ONLINE ? g_lwSquads[i].alive : 0;
    g_lwWorld.onlineUnits = units;
    int farUnits = 0;
    for (int i = 0; i < g_lwWorld.count; i++)
        if (g_lwSquads[i].farOn) farUnits += g_lwSquads[i].state == LW_PENDING ? g_lwSquads[i].cap : g_lwSquads[i].state == LW_ONLINE ? g_lwSquads[i].alive : 0;
    g_lwWorld.farUnits = farUnits;
    LwAvatarDeathWatch(); /* test runs: who killed the local Helldiver */
    LwSafeLanding(); /* test runs: out of a pod among enemies, moved clear */
    LwTestKeepAlive(); /* test runs: the Helldiver healed to full */
    LwDrawnTick(); /* the main view's drawn state of far squads (test instrument, HELLFORGE_LW_DRAWN=1) */
    LwTestCommands(); /* test commands from the harness (hellforge_lw_test), run here on the game thread */
    LwNoiseDrain(); /* Phase 2a: noises since the last tick send squads to investigate */
    LwFightScan(); /* Phase 2b: fights and calls for help draw squads in (once a second) */
    LwTerritoryTick(); /* Phase 2d: regions, their strength, and the squads' homes */
    LwRetakeScan(); /* Phase 2c: squads retake cleared plain garrisons (every 10 s) */
    LwIntentTrace(); /* test runs only: intent changes and positions for the evidence graphics */
    for (int i = 0; i < g_lwWorld.count && !waiting; i++) waiting = g_lwSquads[i].state == LW_OFFLINE && g_lwSquads[i].cycled == 1;
    for (int i = 0; i < g_lwWorld.count; i++)
    {
        LwSquad* q = &g_lwSquads[i];
        if (q->state == LW_OFFLINE)
        {
            const char* why = NULL; char whyBuf[128]; /* test notes for a staged squad (pulled 2): the gate it fails */
            LwWalk(q, dt);
            if (q->state != LW_OFFLINE) continue; /* it became a garrison (2c) */
            float near2 = LwNearestPlayer2(q->pos[0], q->pos[1]);
            float rMin = g_lwWorld.rMin > 0 ? g_lwWorld.rMin : LW_R_MIN;
            int back = q->cycled == 1 && near2 >= rMin * rMin; /* test cycle: back at once (the budget still applies) */
            if (waiting && !back) { why = "a test-cycle squad is served first"; goto offlineDone; }
            if (q->retry && now < q->retry) continue; /* its point was refused (placement or sight) a moment ago */
            int nearBand = near2 < LW_R_ON * LW_R_ON && near2 >= rMin * rMin, farBand = !nearBand && near2 >= LW_R_ON * LW_R_ON && near2 < LwSquadBand(q) * LwSquadBand(q);
            if (!(!requested && (farBand && !back ? farTries < 2 : attempts < 4) && q->pos[0] * q->pos[0] + q->pos[1] * q->pos[1] < r * r && (nearBand || back || farBand)))
            {
                if (q->pulled == 2)
                {
                    if (requested) why = "another squad's request this tick";
                    else if (!(nearBand || farBand)) { snprintf(whyBuf, sizeof whyBuf, "in no band: %.0f m (far band to %.0f m)", (double)sqrtf(near2), (double)LwSquadBand(q)); why = whyBuf; }
                    else if (q->pos[0] * q->pos[0] + q->pos[1] * q->pos[1] >= r * r) why = "outside the playable disc";
                    else why = "this tick's tries are used";
                }
            }
            else
            {
                if (!back && LwRecentNear(q->pos)) { why = "within 60 m of an online squad or a request in the last minute"; q->retry = now + 3000; goto offlineDone; } /* spaced from online squads and recent requests (audit 1, 11) */
                if (farBand && !back)
                {
                    if ((float)(farUnits + q->survivors) > LW_FAR_SHARE * (float)g_lwWorld.eff)
                    { snprintf(whyBuf, sizeof whyBuf, "the far share: %d far units + %u over %.0f", farUnits, q->survivors, (double)(LW_FAR_SHARE * (float)g_lwWorld.eff)); why = whyBuf; q->retry = now + 3000; goto offlineDone; }
                    farTries++;
                    float here[3] = {q->pos[0], q->pos[1], 0};
                    int inView = LwGround(here[0], here[1], &here[2]) && LwSeenByAny(here, LwSquadBand(q));
                    if (!inView && !LwAboutToBeSeen(q)) { why = "not in a player's sight (nor its way ahead)"; q->retry = now + 3000; goto offlineDone; }
                }
                uint32_t off0 = g_lwOffScreen;
                g_lwFarSpawn = farBand && !back;
                LwOnline(q); requested = q->state == LW_PENDING;
                if (requested && q->pulled) { q->pulled = 0; q->node = (uint16_t)LwNearestNode(q->pos[0], q->pos[1]); q->pathLen = q->pathAt = 0; } /* back onto the routes */
                g_lwFarSpawn = 0;
                if (farBand && !back) farTries++; else attempts++;
                if (requested && farBand && !back)
                {
                    q->farOn = 1; farUnits += q->cap; g_lwWorld.farSpawned++;
                    Note("living world: squad %d comes online %.0f m from the nearest player by sightline: %s", (int)(q - g_lwSquads), (double)sqrtf(near2),
                        g_lwOffScreen != off0 ? "in a player's line of sight, off the host's screen" : "out of sight, it or its way ahead in a player's view");
                }
                if (back && requested) { q->cycled = 2; Note("living world: test: squad %d comes back with its %u survivors", (int)(q - g_lwSquads), q->cap); }
                if (q->state == LW_OFFLINE) why = g_lwOnlineWhy ? g_lwOnlineWhy : "the request was not taken";
                if (q->state == LW_OFFLINE && farBand && !back && g_lwOnlineWhy == g_lwRoomWhy &&
                    (strncmp(g_lwRoomWhy, "the game's 70 room", 18) == 0 || strncmp(g_lwRoomWhy, "the cap", 7) == 0))
                    LwSwapForFar(q);
                if (q->state != LW_OFFLINE && g_lwHold.idx == (int)(q - g_lwSquads)) g_lwHold.idx = -1; /* served */
            }
        offlineDone:
            if (q->pulled == 2 && why) LwStagedWhy(q, why, near2);
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
        if (g_lwWorld.rays || g_lwWorld.noFace || g_lwWorld.moved || g_lwWorld.overhead || g_lwWorld.crowded || g_lwWorld.building || g_lwWorld.undrawn)
            Note("living world: placement (30 s): refused %u with no navmesh face within 10 m, %u snapped more than 2 m, %u under something, %u where a member would not reach its slot, %u in a player's sight, "
                "%u in the cell being built, %u where the ground hit is not the drawn ground; %u sight rays, %.1f us each; blocked by terrain %u, by static scenery %u", g_lwWorld.noFace, g_lwWorld.moved,
                g_lwWorld.overhead, g_lwWorld.crowded, g_lwWorld.seen, g_lwWorld.building, g_lwWorld.undrawn,
                g_lwWorld.rays, g_lwWorld.rays ? g_lwWorld.rayUs / g_lwWorld.rays : 0.0, g_lwWorld.blockTerrain, g_lwWorld.blockUnit);
        g_lwWorld.building = g_lwWorld.undrawn = 0;
        g_lwWorld.noFace = g_lwWorld.moved = g_lwWorld.overhead = g_lwWorld.seen = g_lwWorld.rays = g_lwWorld.blockUnit = g_lwWorld.blockTerrain = g_lwWorld.crowded = 0; g_lwWorld.rayUs = 0;
        if (g_lwWorld.goals || g_lwWorld.goalNoFace)
            Note("living world: goals (30 s): %u sent, %u next nodes with no navmesh within 25 m, %u replans, %u squads left without a reachable goal",
                g_lwWorld.goals, g_lwWorld.goalNoFace, g_lwWorld.goalReplans, g_lwWorld.goalStuck);
        g_lwWorld.goals = g_lwWorld.goalNoFace = g_lwWorld.goalReplans = g_lwWorld.goalStuck = 0;
        LwNavTypeDump(); /* test (HELLFORGE_LW_TEST_NAV=1): agent types and obstacle areas, twice a level */
        { LONG spared = InterlockedExchange(&g_lwSpared, 0); if (spared) Note("living world: obstacle smashes (30 s): squad mates kept out of the zones %ld times (a mate is tested again every frame of a zone)", (long)spared); }
        LwHearNote(); LwRushNote();
        { LONG spared = InterlockedExchange(&g_lwBlastSpared, 0); if (spared) Note("living world: obstacle smash explosions (30 s): squad mates left out %ld times", (long)spared); }
        if (g_lwWorld.heights)
            Note("living world: ground (30 s): %u member positions, %u more than 1.5 m under the drawn ground (worst %.1f m), %u more than 3 m over it (worst %.1f m)",
                g_lwWorld.heights, g_lwWorld.underGround, (double)g_lwWorld.worstUnder, g_lwWorld.overGround, (double)g_lwWorld.worstOver);
        g_lwWorld.heights = g_lwWorld.underGround = g_lwWorld.overGround = 0; g_lwWorld.worstUnder = g_lwWorld.worstOver = 0;
        Note("living world: %d squads, %d online (%d units of %d), %u spawned, %u taken offline, %u destroyed, %u request frames refused; job-1 units %u, AI %u; "
            "player 1 at (%.0f, %.0f); by sightline: %u spawned (%u in sight, off the host's screen), %d units now", g_lwWorld.count, online, g_lwWorld.onlineUnits, g_lwWorld.eff, g_lwWorld.spawned, g_lwWorld.offlined, g_lwWorld.died,
            g_lwWorld.refused, EvpU32(dir + 0x614, 0), LwAiCount(), (double)(g_lwWorld.playersAt ? g_lwWorld.players[0][0] : 0),
            (double)(g_lwWorld.playersAt ? g_lwWorld.players[0][1] : 0), g_lwWorld.farSpawned, g_lwOffScreen, g_lwWorld.farUnits);
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
typedef struct LwGarrison
{
    uint32_t entity; uint8_t seen, wasActive, refills; uint32_t taken[3], structure[3], marked[3]; ULONGLONG checkAt, activeAt;
    /* Phase 2c (living_world_intent.c, LwRetake*): index = its GuardForce record; plain = safe to retake; retake = armed */
    uint32_t index, allow[3], u[3], points; float center[2]; uint8_t plain, cleared, haveCenter, retake; int16_t retaker; uint8_t lastRefill; ULONGLONG clearedAt, retakeAt, plainAt, fillAt;
} LwGarrison;
/* marked: the points the loader itself set bit 6 on (held empty); kept apart from taken (audit 1f, 4: the loader's own
 * marks, still set in the next active period, were counted as filled points, so after the next mode-0 despawn they were
 * not marked again and the third fill spent its budget there, replacing kills elsewhere) */

/* Test instrument (HELLFORGE_LW_TEST_KILL=1, host): one unit of the first garrison active for 15 s is killed through the
 * game's own death path, exactly as the game kills drowning and zone-floor units (0xAA88D8 / 0x89C9C9; KB living-world-plan
 * "Member losses" Q4): QueueDamage 0x129F910(NULL, kind 7, 0, unit, 0xFFFFFFFF, 0, net, net, 0, 0, 0, 0, 0, &dir, &pos,
 * actor 0xFFFFFFFF (no limb: zone 0), 0, 0, 1.0); applied in the next frame's damage drain, the death link then clears
 * its point's U bit (0x910070 killed = 1). The point is found by that U bit; after the garrison fills again the note
 * says whether the point stayed empty (no unit: occupied bit 6 clear). Runtime list: [gf+0x40] + 0x1B58 i, +0x204 u32
 * units, count +0x384 (max 96). */
static struct { int on, stage, now; uint32_t garrison, unit, u[3], point, net, slot; ULONGLONG at; } g_lwKill = {-1};
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
    if (g_lwKill.stage == 0 && active && g->activeAt && (now - g->activeAt > 15000 || g_lwKill.now))
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
/* Test command "kill garrison": the kill test on the next active garrison, now. */
static void LwKillArm(void) { g_lwKill.on = 1; g_lwKill.now = 1; if (g_lwKill.stage >= 3 || g_lwKill.stage == 9) g_lwKill.stage = 0; }
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
    g->entity = entity; g->index = 0xFFFFFFFFu; g->retaker = -1;
    return g;
}

/* The survivor mask (KB "Garrisons", Phase 1 approach): notes when a garrison goes inactive (taken and killed points)
 * and, 5 s after it fills again, how many units it listed against the points that had a unit and are not killed. */
/* Test command "garrisons": each garrison with points, its centre (the mean x, y of its guard points: list +0x28 point
 * indices, point records [game+0x346D5C0] + 0x58 g with x +0x10, y +0x14, as AiEnemy measures them at 0xAA1D4D..0xAA1D67;
 * lw93a: the descriptor's +0xC is no unit for a garrison), whether it is active, its points and units listed: for going
 * to one on an extra large map. */
static void LwTestGarrisons(int seq)
{
    uintptr_t gf, synced, runtime, lists, descriptors, points;
    if (!EvpPtr(g_lw.game + 0x33265C0u, &gf) || !EvpPtr(gf + 0x50, &synced) || !EvpPtr(gf + 0x40, &runtime) || !EvpPtr(gf + 0x48, &lists) ||
        !EvpPtr(gf + 0x38, &descriptors) || !EvpPtr(g_lw.game + 0x346D5C0u, &points)) { Note("living world: test command %d: garrisons: the garrison manager does not read", seq); return; }
    float pl[2] = {0, 0}; int havePl = g_lwWorld.playersAt > 0;
    if (havePl) { pl[0] = g_lwWorld.players[0][0]; pl[1] = g_lwWorld.players[0][1]; }
    uint32_t owned = EvpU32(gf + 0x10, 0);
    Note("living world: test command %d: garrisons: %u", seq, owned);
    for (uint32_t i = 0; i < owned && i < LW_GARRISONS_MAX; i++)
    {
        uintptr_t d, l = lists + 0x2B8u * i; uint8_t active = 0; double sx = 0, sy = 0; int got = 0;
        uint32_t np = EvpU32(l + 0x1A8, 0);
        if (!EvpPtr(descriptors + 8u * i, &d) || d == 0 || np == 0 || np > 96) continue;
        SafeRead(synced + 0x40u * i + 0x38, &active, 1);
        for (uint32_t p = 0; p < np; p++)
        {
            uint32_t gi = EvpU32(l + 0x28 + 4u * p, 0xFFFFFFFFu); float xy[2];
            if (gi < 0x100000 && !SafeRead(points + 0x58u * gi + 0x10, xy, 8) && isfinite(xy[0]) && isfinite(xy[1])) { sx += xy[0]; sy += xy[1]; got++; }
        }
        if (!got) continue;
        float cx = (float)(sx / got), cy = (float)(sy / got);
        LwGarrison* g = LwGarrisonOf(EvpU32(d + 8, 0));
        /* the retake's plain test, field by field (2c): objective link list +0x2AB, synced spawners +0x1C / objectives +0x24 /
         * counted +0x2C, runtime structures +0x1110, points with 0x200 / 0x20 / 0x100 */
        uint8_t link = 0xFF; uint32_t sp = EvpU32(synced + 0x40u * i + 0x1C, 0xFFFFFFFFu), ob = EvpU32(synced + 0x40u * i + 0x24, 0xFFFFFFFFu), ce = EvpU32(synced + 0x40u * i + 0x2C, 0xFFFFFFFFu);
        uint32_t st = EvpU32(runtime + 0x1B58u * i + 0x1110, 0xFFFFFFFFu), f200 = 0, f20 = 0, f100 = 0;
        SafeRead(l + 0x2AB, &link, 1);
        for (uint32_t p = 0; p < np; p++) { uint32_t gi = EvpU32(l + 0x28 + 4u * p, 0xFFFFFFFFu), fl = 0; if (gi < 0x100000 && !SafeRead(points + 0x58u * gi + 0x24, &fl, 4)) { f200 += (fl & 0x200) != 0; f20 += (fl & 0x20) != 0; f100 += (fl & 0x100) != 0; } }
        Note("living world: test command %d: garrison %08X fields: link %u, spawners %u, objectives %u, counted %u, structures %u, points 0x200 %u, 0x20 %u, 0x100 %u", seq, EvpU32(d + 8, 0),
            link, sp, ob, ce, st, f200, f20, f100);
        Note("living world: test command %d: garrison %08X %s%s%s at (%.0f, %.0f)%s%.0f%s: %u points, %u units listed", seq, EvpU32(d + 8, 0), active ? "active" : "inactive",
            g && g->plain ? ", plain" : "", g && g->cleared ? ", cleared" : "",
            (double)cx, (double)cy, havePl ? ", " : "", havePl ? (double)sqrtf((cx - pl[0]) * (cx - pl[0]) + (cy - pl[1]) * (cy - pl[1])) : 0.0, havePl ? " m from player 1" : "",
            np, EvpU32(runtime + 0x1B58u * i + 0x384, 0));
    }
}

static void LwRetakeFilled(LwGarrison* g, uintptr_t d, uintptr_t r, const uint32_t* u, uint32_t n);
static void LwRetakeGarrison(LwGarrison* g, uintptr_t d, uintptr_t s, uintptr_t r, uintptr_t l, uintptr_t points, uint32_t n, const uint32_t* u, uint8_t active, uint8_t refill);
static void LwGarrisonsTick(void)
{
    uintptr_t gf, synced, runtime, lists, descriptors, points;
    if (!EvpPtr(g_lw.game + 0x33265C0u, &gf) || !EvpPtr(gf + 0x50, &synced) || !EvpPtr(gf + 0x40, &runtime) || !EvpPtr(gf + 0x48, &lists) ||
        !EvpPtr(gf + 0x38, &descriptors) || !EvpPtr(g_lw.game + 0x346D5C0u, &points)) return;
    static uintptr_t level; /* the squads' level too: a new mission's GuardForce can reuse the old address (0 = not placed yet, no reset) */
    int newLevel = g_lwWorld.lg && level && g_lwWorld.lg != level;
    if (g_lwWorld.lg) level = g_lwWorld.lg;
    if (gf != g_lwGarrisonOwner || newLevel) { g_lwGarrisonOwner = gf; g_lwGarrisonCount = 0; }
    uint32_t owned = EvpU32(gf + 0x10, 0), marked = 0;
    if (owned > LW_GARRISONS_MAX) owned = LW_GARRISONS_MAX;
    for (uint32_t i = 0; i < owned; i++)
    {
        uintptr_t d, s = synced + 0x40u * i, r = runtime + 0x1B58u * i, l = lists + 0x2B8u * i;
        uint8_t active = 0, refill = 0; uint32_t u[3], n = EvpU32(l + 0x1A8, 0xFFFFFFFFu);
        if (!EvpPtr(descriptors + 8u * i, &d) || n > 96 || SafeRead(s + 0x38, &active, 1) || SafeRead(r + 0x1B4F, &refill, 1) || SafeRead(s + 4, u, sizeof u)) continue;
        LwGarrison* g = LwGarrisonOf(EvpU32(d + 8, 0));
        if (g == NULL || g->entity == 0) continue;
        g->index = i;
        LwRetakeGarrison(g, d, s, r, l, points, n, u, active, refill);
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
                uint32_t gi = EvpU32(l + 0x28 + 4u * p, 0xFFFFFFFFu), flags = 0, bit = 1u << (p % 32);
                if (gi < 0x100000 && !SafeRead(points + 0x58u * gi + 0x24, &flags, 4) && (flags & 0x40) && !(g->marked[p / 32] & bit)) g->taken[p / 32] |= bit; /* filled by the game, not our mark */
            }
            uint32_t structures = EvpU32(r + 0x1110, 0);
            for (uint32_t k = 0; k < structures && k < 96; k++)
            {
                uint32_t local = EvpU32(r + 0x810 + 24u * k + 0x10, 0);
                if (local >= 1 && local <= n) g->structure[(local - 1) / 32] |= 1u << ((local - 1) % 32);
            }
        }
        if (!g->seen || (active && !refill)) continue; /* the first fill stays native; no marks while a garrison holds */
        if (g->retake) continue; /* a retaken garrison's holds are LwRetakeGarrison's */
        for (uint32_t p = 0; p < n; p++)
        {
            uint32_t bit = 1u << (p % 32);
            if ((g->taken[p / 32] | g->structure[p / 32]) & bit) continue;
            uint32_t gi = EvpU32(l + 0x28 + 4u * p, 0xFFFFFFFFu), flags = 0;
            if (gi >= 0x100000) continue;
            uintptr_t at = points + 0x58u * gi + 0x24;
            if (SafeRead(at, &flags, 4)) continue;
            if ((flags & 0x340) == 0x40 && (g->marked[p / 32] & bit)) continue; /* our mark still set */
            if ((flags & 0x340) != 0) continue; /* phase A, objective points and points the game holds stay */
            flags |= 0x40;
            if (!LwPoke(at, &flags, 4)) { marked++; g->marked[p / 32] |= bit; }
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

/* Section swaps applied at once (audit 1f, crashes 0xCB1223 / 0xC2F662; KB crash-cb1223-navcut-section-swap "Design
 * verified (third pass)", subagent, checked by me in the dump: the call sites, the vanilla flush 0x7B94A3..0x7B959E and
 * the permit 0x7D7740, which takes and releases its owner / count at table +0x70 / +0x74 on every call). A built cell's
 * completion (main thread, 0x7B8A10) queues its old section's unload (0x7B9053 -> 0xBB2670) and its new one's load
 * (0x7B9244 -> 0xBB19B0) for the AI world's next asynchronous step, but connects to its neighbours at once. Another
 * completion next to it before that step applied the swap pairs face keys of one mesh version with the other (no
 * bounds check in the cutter). Both calls go through wrappers here, on the same thread with the original arguments:
 * before the unload, the world's in-flight step is finished and committed and anything still queued (a neighbour's
 * first load) is applied by one synchronous step; after the load, the same again, so this completion's swap is applied
 * before the frame goes on. That is the vanilla teardown flush's sequence: join the threaded AI update (table +0x20),
 * the permit 0x7D7740 (no async query job reads the world; [W+0x478]), wait 0xBD0710 / commit 0xBD05E0 when a step is
 * in flight, then 0xBD0BB0 with its parameter block and a serial runner, and [W+0x478] = 0. Every built-cell connect
 * then runs with no step in flight and an empty queue, in any order of completions (game-initiated builds included).
 * When the permit doesn't come within LW_NAV_PERMIT_MS (query jobs still running), the call goes on as vanilla's and
 * g_lwSwap.unsafe holds the loader's new rebuild marks until the world is idle. */
#define LW_AIW_TABLE 0x190D4A0u
#define LW_NAV_PERMIT_MS 20
typedef uintptr_t (*LwNavUnloadFn)(uintptr_t w, uint32_t* id, uint8_t graph);
typedef uintptr_t (*LwNavLoadFn)(uintptr_t w, uint32_t* out, uintptr_t inst, void* p4, uintptr_t graph, void* p6);
static struct
{
    LwNavUnloadFn unload; LwNavLoadFn load;
    uint8_t (*asyncMode)(uintptr_t), (*inFlight)(uintptr_t), (*stepDone)(uintptr_t);
    void (*wait)(uintptr_t), (*commit)(uintptr_t, void*, int);
    uintptr_t (*step)(uintptr_t, void*, void*, int);
    void (*runnerMake)(void*), (*runnerFree)(void*);
    int (*queued)(uintptr_t);
    int (*permit)(int);
    void (*join)(int);
    uintptr_t table;
    uint8_t swap[4];
    volatile LONG unsafe;            /* a swap went unsynchronised: hold new marks until the world is idle */
    uint32_t swaps, steps, refused;
    double worstMs;
} g_lwSwap;

static int LwAiwIndex(uintptr_t w)
{
    for (int i = 0; i < 4; i++) if (*(volatile uintptr_t*)(g_lwSwap.table + 0x80u * i) == w) return i;
    return -1;
}
/* The vanilla flush's step (0x7B94F0..0x7B959E). */
static void LwNavSyncStep(uintptr_t w)
{
    uint8_t runner[0x18] __attribute__((aligned(16)));
    uint8_t block[0x40] __attribute__((aligned(16)));
    memset(block, 0, sizeof block);
    g_lwSwap.runnerMake(runner);
    block[0] = 1;
    *(uint32_t*)(block + 0x14) = 0x80000000u;
    *(int32_t*)(block + 0x20) = 0x7FFFFFFF; *(int32_t*)(block + 0x28) = 0x7FFFFFFF;
    *(int32_t*)(block + 0x30) = 0x7FFFFFFF; *(int32_t*)(block + 0x34) = 0x7FFFFFFF;
    *(uint32_t*)(block + 0x38) = 0xFFFFFFFFu;
    g_lwSwap.step(w, block, runner, 1);
    *(volatile uint8_t*)(w + 0x478) = 0;
    int32_t cap = *(int32_t*)(block + 0x14); uintptr_t arr = *(uintptr_t*)(block + 8);
    if (cap >= 0 && arr != 0)
    {
        uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL), heap = *(uintptr_t*)(exe + 0x1899880u);
        ((void (*)(uintptr_t, uintptr_t, int, uint32_t))(exe + 0x99A920u))(heap, arr, 4, (uint32_t)cap & 0x3FFFFFFFu);
    }
    g_lwSwap.runnerFree(runner);
    g_lwSwap.steps++;
}
/* Finishes the world's async step and applies what is queued; 0 when the permit never came (nothing done). */
static int LwNavDrain(uintptr_t w, int idx)
{
    if (*(volatile uintptr_t*)(g_lwSwap.table + 0x80u * idx + 0x20)) g_lwSwap.join(idx);
    LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a); QueryPerformanceFrequency(&f);
    for (;;)
    {
        g_lwSwap.permit(idx);
        if (*(volatile uint8_t*)(w + 0x478)) break;
        QueryPerformanceCounter(&b);
        if ((double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart > LW_NAV_PERMIT_MS) return 0;
        SwitchToThread();
    }
    if (g_lwSwap.inFlight(w)) g_lwSwap.wait(w);
    if (g_lwSwap.inFlight(w) && g_lwSwap.stepDone(w)) g_lwSwap.commit(w, NULL, 0);
    uintptr_t q = *(volatile uintptr_t*)(w + 0x4A0);
    if (q && g_lwSwap.queued(q) > 0) LwNavSyncStep(w);
    *(volatile uint8_t*)(w + 0x478) = 0;
    return 1;
}
static uintptr_t LwNavUnload(uintptr_t w, uint32_t* id, uint8_t graph)
{
    int idx = LwAiwIndex(w);
    if (idx >= 0 && g_lwSwap.asyncMode(w))
    {
        if (LwNavDrain(w, idx)) g_lwSwap.swap[idx] = 1;
        else { g_lwSwap.swap[idx] = 0; g_lwSwap.refused++; InterlockedExchange(&g_lwSwap.unsafe, 1); }
    }
    return g_lwSwap.unload(w, id, graph);
}
static uintptr_t LwNavLoad(uintptr_t w, uint32_t* out, uintptr_t inst, void* p4, uintptr_t graph, void* p6)
{
    uintptr_t r = g_lwSwap.load(w, out, inst, p4, graph, p6);
    int idx = LwAiwIndex(w);
    if (idx < 0 || !g_lwSwap.swap[idx]) return r;
    g_lwSwap.swap[idx] = 0;
    LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a);
    uint32_t steps = g_lwSwap.steps;
    int ok = LwNavDrain(w, idx);
    QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
    double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    if (!ok) { g_lwSwap.refused++; InterlockedExchange(&g_lwSwap.unsafe, 1); }
    else
    {
        g_lwSwap.swaps++;
        if (ms > g_lwSwap.worstMs) g_lwSwap.worstMs = ms;
        Note("living world: nav: section %u swapped in at once (world %d): %u step(s), %.1f ms; %u swaps, worst %.1f ms",
            out ? *out : 0u, idx, g_lwSwap.steps - steps, ms, g_lwSwap.swaps, g_lwSwap.worstMs);
    }
    return r;
}
static void LwInstallNavSwapSync(void)
{
    static int done;
    if (done) return;
    done = 1;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (!ApiCodeMatches(exe + 0x7B9053u, "E8 18 96 3F 00 8B 43 60") || !ApiCodeMatches(exe + 0x7B9244u, "E8 67 87 3F 00 8B 85 20 01 00 00") ||
        !ApiCodeMatches(exe + 0xBB2670u, "40 55 53 56 57 41 55 41 56 41 57") || !ApiCodeMatches(exe + 0xBB19B0u, "4C 89 4C 24 20 48 89 54 24 10") ||
        !ApiCodeMatches(exe + 0x7D7930u, "48 89 5C 24 10 48 89 6C 24 18") || !ApiCodeMatches(exe + 0x7D7740u, "48 89 74 24 18 57 48 83 EC 20 83 F9 04") ||
        !ApiCodeMatches(exe + 0xBD1150u, "48 83 B9 A8 04 00 00 00 0F 95 C0 C3") || !ApiCodeMatches(exe + 0xBD1190u, "48 83 B9 B0 04 00 00 00 0F 95 C0 C3") ||
        !ApiCodeMatches(exe + 0xBD1160u, "40 53 48 83 EC 20 48 8B D9 48 8B 89 A8 04 00 00") || !ApiCodeMatches(exe + 0xBD0710u, "40 53 48 83 EC 30 80 B9 78 04 00 00 00") ||
        !ApiCodeMatches(exe + 0xBD05E0u, "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 50") || !ApiCodeMatches(exe + 0xBD0BB0u, "48 89 5C 24 10 48 89 74 24 18 55 57 41 56") ||
        !ApiCodeMatches(exe + 0xA0F4D0u, "48 8D 05 61 CE A1 00") || !ApiCodeMatches(exe + 0xA0F4F0u, "40 53 48 83 EC 20 48 8B D9 48 8D 05 38 CE A1 00") ||
        !ApiCodeMatches(exe + 0xAD4740u, "48 8B 01 8B 40 08 03 41 1C C3") || !ApiCodeMatches(exe + 0x7B94A3u, "8B CF E8 96 E2 01 00 80 BB 78 04 00 00 00") ||
        !ApiCodeMatches(exe + 0x7B921Cu, "4C 8D 35 7D 42 15 01"))
    { Note("living world: the nav section swap is not the reviewed code; swaps stay asynchronous"); return; }
    uint8_t* block = AllocateNearGame(exe, 0x1000);
    if (block == NULL) { Note("living world: no memory within 2 GB of the game's exe for the nav swap"); return; }
    g_lwSwap.unload = (LwNavUnloadFn)(exe + 0xBB2670u); g_lwSwap.load = (LwNavLoadFn)(exe + 0xBB19B0u);
    g_lwSwap.asyncMode = (uint8_t (*)(uintptr_t))(exe + 0xBD1150u); g_lwSwap.inFlight = (uint8_t (*)(uintptr_t))(exe + 0xBD1190u);
    g_lwSwap.stepDone = (uint8_t (*)(uintptr_t))(exe + 0xBD1160u); g_lwSwap.wait = (void (*)(uintptr_t))(exe + 0xBD0710u);
    g_lwSwap.commit = (void (*)(uintptr_t, void*, int))(exe + 0xBD05E0u); g_lwSwap.step = (uintptr_t (*)(uintptr_t, void*, void*, int))(exe + 0xBD0BB0u);
    g_lwSwap.runnerMake = (void (*)(void*))(exe + 0xA0F4D0u); g_lwSwap.runnerFree = (void (*)(void*))(exe + 0xA0F4F0u);
    g_lwSwap.queued = (int (*)(uintptr_t))(exe + 0xAD4740u); g_lwSwap.permit = (int (*)(int))(exe + 0x7D7740u);
    g_lwSwap.join = (void (*)(int))(exe + 0x7D7930u); g_lwSwap.table = exe + LW_AIW_TABLE;
    uint8_t code[24] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0,  /* mov rax, LwNavUnload; jmp rax */
                        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0}; /* mov rax, LwNavLoad; jmp rax */
    uint64_t fn = (uint64_t)(uintptr_t)LwNavUnload; memcpy(code + 2, &fn, 8);
    fn = (uint64_t)(uintptr_t)LwNavLoad; memcpy(code + 14, &fn, 8);
    memcpy(block, code, sizeof code);
    DWORD protect; VirtualProtect(block, 0x1000, PAGE_EXECUTE_READ, &protect); FlushInstructionCache(GetCurrentProcess(), block, 0x1000);
    int64_t r1 = (int64_t)(uintptr_t)block - (int64_t)(exe + 0x7B9058u), r2 = (int64_t)(uintptr_t)(block + 12) - (int64_t)(exe + 0x7B9249u);
    if (!InRel32(r1) || !InRel32(r2)) { Note("living world: the nav swap is out of reach of the exe"); return; }
    uint8_t call[5] = {0xE8, 0, 0, 0, 0}; int32_t r32 = (int32_t)r1; memcpy(call + 1, &r32, 4);
    if (ApiWrite(exe + 0x7B9053u, call, sizeof call)) { Note("living world: the nav unload call could not be written"); return; }
    r32 = (int32_t)r2; memcpy(call + 1, &r32, 4);
    if (ApiWrite(exe + 0x7B9244u, call, sizeof call))
    {
        r32 = 0x003F9618; memcpy(call + 1, &r32, 4); ApiWrite(exe + 0x7B9053u, call, sizeof call); /* back to the original */
        Note("living world: the nav load call could not be written; swaps stay asynchronous"); return;
    }
    Note("living world: a rebuilt nav cell's section swap is applied before the next completion (0xBB2670 at 0x7B9053, 0xBB19B0 at 0x7B9244)");
}
/* The hold after an unsynchronised swap: cleared when no AI update runs (table +0x54), no async step is in flight and
 * nothing is queued, in every world (+0x54 read first and last). */
static int LwNavSwapHeld(void)
{
    if (!g_lwSwap.unsafe || !g_lwSwap.table) return 0;
    for (int i = 0; i < 4; i++)
    {
        uintptr_t t = g_lwSwap.table + 0x80u * i, w = *(volatile uintptr_t*)t, q;
        if (!w) continue;
        if (*(volatile int32_t*)(t + 0x54)) return 1;
        if (*(volatile uintptr_t*)(w + 0x4B0)) return 1;
        q = *(volatile uintptr_t*)(w + 0x4A0);
        if (q && g_lwSwap.queued(q) > 0) return 1;
        if (*(volatile int32_t*)(t + 0x54)) return 1;
    }
    InterlockedExchange(&g_lwSwap.unsafe, 0);
    Note("living world: nav: the world is idle again after %u unsynchronised swap(s); rebuild marks go on", g_lwSwap.refused);
    return 0;
}

/* Test instrument (HELLFORGE_LW_TEST_TRACE=1; KB nav-cell-connections, checked by me: 0xBB5310..0xBB545F): a finished
 * cell joins its built neighbours through 0xBB5310 -> 0xBB5110(out, params, &set, &graphSet), whose streaming set's
 * [set+0x28] (connections) is tested at 0xBB5290: 0 = nothing stored, the two sections stay apart and no path crosses.
 * The call at exe 0xBB5340 goes through LwConnectNote, which calls 0xBB5110 and notes the section uids (params +0x10,
 * +0x14 = the cell ids) and the count. Installed at game load: vanilla's cells (built at level load) are noted too. */
typedef uintptr_t (*LwConnectFn)(void* out, uintptr_t params, uintptr_t* set, uintptr_t* graphSet);
static LwConnectFn g_lwConnect;
static uintptr_t LwConnectNote(void* out, uintptr_t params, uintptr_t* set, uintptr_t* graphSet)
{
    uintptr_t r = g_lwConnect(out, params, set, graphSet), st = set ? *set : 0;
    int32_t n = -1; uint32_t a = 0, b = 0;
    if (st) SafeRead(st + 0x28, &n, 4);
    SafeRead(params + 0x10, &a, 4); SafeRead(params + 0x14, &b, 4);
    Note("living world: nav: sections %u and %u joined by %d connections", a, b, n);
    return r;
}
static void LwInstallNavConnectNote(void)
{
    char t[4] = "";
    if (!(GetEnvironmentVariableA("HELLFORGE_LW_TEST_TRACE", t, sizeof t) > 0 && t[0] == '1') || !LwEnabled()) return;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (!ApiCodeMatches(exe + 0xBB5340u, "E8 CB FD FF FF 48 8B 5C 24 50 48 8B 7C 24 58 80 38 00") ||
        !ApiCodeMatches(exe + 0xBB5290u, "83 7F 28 00 75 1A"))
    { Note("living world: test: the nav connect step is not the reviewed code; not noted"); return; }
    uint8_t* block = AllocateNearGame(exe, 0x1000);
    if (block == NULL) { Note("living world: test: no memory near the exe for the nav connect note"); return; }
    uint8_t code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0}; /* mov rax, LwConnectNote; jmp rax */
    uint64_t to = (uint64_t)(uintptr_t)LwConnectNote; memcpy(code + 2, &to, 8);
    memcpy(block, code, sizeof code);
    DWORD protect; VirtualProtect(block, 0x1000, PAGE_EXECUTE_READ, &protect); FlushInstructionCache(GetCurrentProcess(), block, 0x1000);
    int64_t rel = (int64_t)(uintptr_t)block - (int64_t)(exe + 0xBB5345u);
    if (!InRel32(rel)) { Note("living world: test: the nav connect note is out of reach"); return; }
    g_lwConnect = (LwConnectFn)(exe + 0xBB5110u);
    uint8_t call[5] = {0xE8, 0, 0, 0, 0}; int32_t r32 = (int32_t)rel; memcpy(call + 1, &r32, 4);
    if (ApiWrite(exe + 0xBB5340u, call, sizeof call)) { Note("living world: test: the nav connect call could not be written"); return; }
    Note("living world: test: nav section connections are noted (0xBB5110 at 0xBB5340)");
}

/* Test instrument (HELLFORGE_LW_TEST_HITS=1; audit 1g criterion 6; KB living-world-spawn-losses, subagent, checked by me
 * in game.dll): calm loader members are killed by squad mates (hash 0), and statics cannot single out the source because
 * it lies in unit and ability data. H1: every hit the damage apply 0x9235F0 takes on an online loader member is noted
 * before the apply runs: kind (r8d), amount (arg8), DamageInfo id (arg16), hash (arg17), the damager (arg5) and attacker
 * (arg6) net ids as entities (0xFD9BA0(out, net), as the apply converts them at 0x923BED / 0x923BF9; its prologue copies
 * arg6 / arg5 to [rbp+0] / [rbp+4] at 0x92361E..0x923630), each one's resource, position and alert, and the caller. H2:
 * each explosion the spawner 0x13C0A80 (rdx pos, r8d type, r9d source, arg5 attacker; entity ids) makes within 15 m of an online loader
 * member, with its caller (0x8CB1FF a grenade / explosive, 0x11AD469 an ability's bone explosion). The original is
 * called last with every argument it reads (checked by me: the apply reads up to entry rsp+0x98, 19 arguments; the spawner
 * up to entry rsp+0x78 ([rsp+0xB0] after its push and 0x30), 15; lw95a crashed at 0x13C0CCB on its 13th, a pointer, when
 * only 10 were passed on) and its result returned as is. */
typedef uint64_t (*LwApplyFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*LwBlastFn)(uint64_t, const float*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static LwApplyFn g_lwApply; static LwBlastFn g_lwBlast;
static volatile LONG g_lwHitNotes;
static int LwOnlineMember(uint32_t e, int* squad)
{
    if (e == 0) return 0;
    for (int i = 0; i < g_lwWorld.count && i < LW_SQUADS_MAX; i++)
    {
        const LwSquad* q = &g_lwSquads[i];
        if (q->state != LW_ONLINE) continue;
        for (int k = 0; k < q->seenCount && k < 32; k++) if (q->seen[k] == e) { *squad = i; return 1; }
    }
    return 0;
}
static uint32_t LwNetEntity(uint32_t net)
{
    uint32_t out[8] = {0};
    ((void (*)(uint32_t*, uint32_t))(g_lw.game + 0xFD9BA0u))(out, net);
    return out[0];
}
static void LwHitWho(uint32_t e, char* buf, size_t n)
{
    uint32_t unit = 0; float p[3] = {0, 0, 0}; int sq = -1;
    uint64_t res = e ? EvpEntityResource(g_lw.game, e, &unit) : 0;
    char at[64] = "";
    if (unit && EvpUnitPosition(unit, p)) snprintf(at, sizeof at, ", at (%.1f, %.1f, %.1f)", (double)p[0], (double)p[1], (double)p[2]);
    snprintf(buf, n, "%08X (resource %016llX, alert %u%s%s)", e, (unsigned long long)res, e ? LwAlert(e) : 0u, at, LwOnlineMember(e, &sq) ? ", a loader member" : "");
}
static uint64_t LwHitApply(uint64_t mgr, uint64_t target, uint64_t kind, uint64_t a4, uint64_t damager, uint64_t attacker, uint64_t creditor, uint64_t amount,
    uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14, uint64_t a15, uint64_t info, uint64_t hash, uint64_t a18, uint64_t a19)
{
    int sq = -1;
    if (LwOnlineMember((uint32_t)target, &sq) && InterlockedIncrement(&g_lwHitNotes) <= 4000)
    {
        char who[3][200];
        LwHitWho((uint32_t)target, who[0], sizeof who[0]);
        LwHitWho(LwNetEntity((uint32_t)damager), who[1], sizeof who[1]);
        LwHitWho(LwNetEntity((uint32_t)attacker), who[2], sizeof who[2]);
        Note("living world: test: hit on squad %d member %s: kind %u, amount %d, DamageInfo %08X, hash %08X, creditor %llX; damager %s; attacker %s; caller game+0x%llX",
            sq, who[0], (uint32_t)kind, (int32_t)amount, (uint32_t)info, (uint32_t)hash, (unsigned long long)creditor, who[1], who[2],
            (unsigned long long)((uintptr_t)__builtin_return_address(0) - g_lw.game));
    }
    return g_lwApply(mgr, target, kind, a4, damager, attacker, creditor, amount, a9, a10, a11, a12, a13, a14, a15, info, hash, a18, a19);
}
static uint64_t LwHitBlast(uint64_t self, const float* pos, uint64_t type, uint64_t source, uint64_t attacker, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10,
    uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14, uint64_t a15)
{
    float p[3];
    if (pos && !SafeRead((uintptr_t)pos, p, 12) && isfinite(p[0]))
    {
        int closeTo = -1;
        for (int i = 0; i < g_lwWorld.count && i < LW_SQUADS_MAX && closeTo < 0; i++)
        {
            const LwSquad* q = &g_lwSquads[i];
            if (q->state != LW_ONLINE) continue;
            for (int k = 0; k < q->seenCount && k < 32; k++)
            { float dx = q->seenAt[k][0] - p[0], dy = q->seenAt[k][1] - p[1]; if (dx * dx + dy * dy < 15.0f * 15.0f) { closeTo = i; break; } }
        }
        if (closeTo >= 0 && InterlockedIncrement(&g_lwHitNotes) <= 4000)
        {
            char who[2][200];
            /* entity ids, not net ids: the hit pass hands record +0x10 / +0x14 to 0x12A1210 (0x13C4457), which looks them up in
             * the entity state's id hash (0xFD9D40, +0xF1AEB0); the net conversion printed 00000000 up to lw100a */
            LwHitWho((uint32_t)source, who[0], sizeof who[0]);
            LwHitWho((uint32_t)attacker, who[1], sizeof who[1]);
            /* the spawner's caller can lie in relocated code outside game.dll (audit 1h, 15: game+0x606999C): the unwound chain,
             * each frame named in game.dll or not */
            void* frames[8]; char chain[256]; size_t at = 0; chain[0] = 0;
            USHORT nf = RtlCaptureStackBackTrace(1, 8, frames, NULL);
            for (USHORT k = 0; k < nf && at < sizeof chain - 24; k++)
            {
                uintptr_t f = (uintptr_t)frames[k]; int in = f >= g_lw.game && f < g_lw.game + 0x4744000u;
                at += (size_t)snprintf(chain + at, sizeof chain - at, in ? " g+0x%llX" : " ?%llX", (unsigned long long)(in ? f - g_lw.game : f));
            }
            Note("living world: test: explosion type %08X at (%.1f, %.1f, %.1f) within 15 m of squad %d; source %s; attacker %s; chain%s",
                (uint32_t)type, (double)p[0], (double)p[1], (double)p[2], closeTo, who[0], who[1], chain);
        }
    }
    return g_lwBlast(self, pos, type, source, attacker, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15);
}
/* H3 (lw95b: the hit that killed was kind 6, amount 100, DamageInfo 9, hash 0, damager = attacker = the mate's own body,
 * applied from the damage drain game+0x12A82F0, so the queuer is not on the apply's stack): QueueDamage 0x129F910 (rcx,
 * edx kind, r8d, r9d target, arg5 amount, arg6 / arg7 the attacker's net id; 19 arguments: its frame reads up to
 * [rbp+0xC7] = entry rsp+0x98 after five pushes, rbp = rsp - 7 and 0xD0; its callers write up to [rsp+0x90]). Kind 6 is
 * queued directly at 0x93630E (amount -1) and 0x11AE1EB (amount 15, DamageInfo 9), or with kind / amount / DamageInfo
 * from arguments at 0x4C8433 (a wrapper with many callers): the unwound chain names the origin. Noted for each damage
 * queued on an online loader member. */
static LwApplyFn g_lwQueue;
static uint64_t LwHitQueue(uint64_t rcx, uint64_t kind, uint64_t r8, uint64_t target, uint64_t amount, uint64_t attacker, uint64_t a7, uint64_t a8,
    uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14, uint64_t a15, uint64_t a16, uint64_t a17, uint64_t a18, uint64_t a19)
{
    int sq = -1;
    if (LwOnlineMember((uint32_t)target, &sq) && InterlockedIncrement(&g_lwHitNotes) <= 4000)
    {
        void* frames[12]; char chain[320]; size_t at = 0; chain[0] = 0;
        USHORT n = RtlCaptureStackBackTrace(1, 12, frames, NULL);
        for (USHORT k = 0; k < n && at < sizeof chain - 24; k++)
        {
            uintptr_t f = (uintptr_t)frames[k];
            at += (size_t)snprintf(chain + at, sizeof chain - at, f >= g_lw.game && f < g_lw.game + 0x4744000u ? " g+0x%llX" : " ?%llX",
                (unsigned long long)(f >= g_lw.game && f < g_lw.game + 0x4744000u ? f - g_lw.game : f));
        }
        char who[2][200];
        LwHitWho((uint32_t)target, who[0], sizeof who[0]);
        LwHitWho(LwNetEntity((uint32_t)attacker), who[1], sizeof who[1]);
        Note("living world: test: damage queued on squad %d member %s: kind %u, amount %d, args 7-8 %08X %08X, arg17 %08X; attacker %s; chain%s",
            sq, who[0], (uint32_t)kind, (int32_t)amount, (uint32_t)a7, (uint32_t)a8, (uint32_t)a17, who[1], chain);
    }
    return g_lwQueue(rcx, kind, r8, target, amount, attacker, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19);
}
/* Z / A (lw95c: no kind-6 hit was queued through 0x129F910; KB living-world-spawn-losses, subagent kind6, checked by me):
 * the only builder of kind-6 hits is 0x12A2C40, called four times from the damage-zone processor 0x7D01C0 (DamageInfo =
 * zone descriptor +8, hash = +0xD8). Damage between AI units is applied with no faction test, so a mate inside a unit's own
 * ability zone takes it. Which ability a calm unit plays is game data, not code: Z notes every zone a loader member starts
 * (0x7D38D0: rcx, edx entity, r8 the static descriptor; three arguments, r9 is written at 0x7D390A before any read) with
 * its caller (the ability's step function) and the descriptor's +0 / +8 / +0xD8; A notes every ability a loader member
 * starts (0x7CAF40: rcx manager, edx entity, r8d ability id, r9d mode, the rate a float at entry rsp+0x28, read at
 * 0x7CAFDE as [rsp+0x100] after five pushes and 0xB0; five arguments) with its caller, which names the trigger (0xAA7250
 * combat task; 0xAA9D92 / 0xAA3D9B AiEnemy settings +0x184; 0x7E6BEE +0x18C; 0x7E9988 +0x1D0; 0x4C32xx a brain helper).
 * Both with the unit's alert and the distance to its nearest online mate. */
typedef uint64_t (*LwZoneFn)(uint64_t, uint64_t, uint64_t);
typedef uint64_t (*LwAbilityFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static LwZoneFn g_lwZone; static LwAbilityFn g_lwAbility;
static volatile LONG g_lwZoneNotes;
static float LwMateDistance(uint32_t e, int sq)
{
    const LwSquad* q = &g_lwSquads[sq]; int me = -1; float best = -1.0f;
    for (int k = 0; k < q->seenCount && k < 32; k++) if (q->seen[k] == e) me = k;
    if (me < 0) return -1.0f;
    for (int k = 0; k < q->seenCount && k < 32; k++)
    {
        if (k == me || q->seen[k] == 0) continue;
        float dx = q->seenAt[k][0] - q->seenAt[me][0], dy = q->seenAt[k][1] - q->seenAt[me][1], d = sqrtf(dx * dx + dy * dy);
        if (best < 0 || d < best) best = d;
    }
    return best;
}
static uint64_t LwHitZone(uint64_t ctx, uint64_t entity, uint64_t desc)
{
    int sq = -1;
    if (LwOnlineMember((uint32_t)entity, &sq) && InterlockedIncrement(&g_lwZoneNotes) <= 6000)
    {
        uint32_t d0 = 0, info = 0, hash = 0;
        if (desc) { SafeRead((uintptr_t)desc, &d0, 4); SafeRead((uintptr_t)desc + 8, &info, 4); SafeRead((uintptr_t)desc + 0xD8, &hash, 4); }
        char who[200]; LwHitWho((uint32_t)entity, who, sizeof who);
        Note("living world: test: zone started by squad %d member %s: descriptor game+0x%llX (+0 %08X, DamageInfo %08X, hash %08X); nearest mate %.1f m; caller game+0x%llX",
            sq, who, (unsigned long long)(desc >= g_lw.game && desc < g_lw.game + 0x4000000u ? desc - g_lw.game : desc), d0, info, hash,
            (double)LwMateDistance((uint32_t)entity, sq), (unsigned long long)((uintptr_t)__builtin_return_address(0) - g_lw.game));
    }
    return g_lwZone(ctx, entity, desc);
}
static uint64_t LwHitAbility(uint64_t mgr, uint64_t entity, uint64_t id, uint64_t mode, uint64_t rate)
{
    int sq = -1;
    if (LwOnlineMember((uint32_t)entity, &sq) && InterlockedIncrement(&g_lwZoneNotes) <= 6000)
    {
        float r; uint32_t bits = (uint32_t)rate; memcpy(&r, &bits, 4);
        char who[200]; LwHitWho((uint32_t)entity, who, sizeof who);
        Note("living world: test: ability %u (mode %d, rate %.2f) started by squad %d member %s; nearest mate %.1f m; caller game+0x%llX",
            (uint32_t)id, (int32_t)mode, (double)r, sq, who, (double)LwMateDistance((uint32_t)entity, sq),
            (unsigned long long)((uintptr_t)__builtin_return_address(0) - g_lw.game));
    }
    return g_lwAbility(mgr, entity, id, mode, rate);
}
/* Test data dump (HELLFORGE_LW_TEST_NAV=1; KB living-world-spawn-losses "Follow-up 5", subagent, the reads checked by me at
 * exe 0x82FF30 and game 0x59C093): which agent types may cross the obstacle areas, so that calm smashers can be given a
 * route around them (the masks are heap data, not code). Read only:
 * - navigation settings R = [exe+0x27C5CF8]: areas A = R+[R+0x2C] (count [A], hash [A+4+4i]); agent types T = R+[R+0x34]
 *   (count nT [T], m [T+4], hash [T+8+4t], f[t] [T+8+4nT+4m nT+4t]); word[t] [R+[R+0x3C]+4+4t]; k [R+[R+0x44]].
 * - the mover's filter Fm = [exe+0x27C5BB8] (0x82FF30: area a allowed for type t when mask[a] +0x9C+4a has bit t; else
 *   forbidden when a < N +0x18; else allowed when q[t] +0x1C+4t > the face's byte 7) and the planner's Fp =
 *   [exe+0x27C5E38] (masks +0xA0+4a, N +0x130, k +0x134, e[t] +0x138+4t, q[t] +0x1B8+4t).
 * - each online member's NavigationComponent ([game+0x3326418]: keys {id, index} at [+0x30], places +0x38, empty key
 *   +0x3C, multiplier +0x40, probe (id mult + i) & (places - 1); record [+0x50] + 0xA8 index: radius +0x98, type hash
 *   +0x9C) and AI settings (0x511CC0's table [[game+0x346BF98]+0xF12DE8]: 316 buckets {u64 resource, u32 index}, home
 *   resource % 316, linear probe, key 0 = none; settings = table + 0x13C0 + 0x2DC index: +0x200 / +0x204 the small /
 *   large obstacle ability, +0x208 / +0x20C the box, +0x210 the obstacle walls flag). */
static int LwNavDumpU32(uintptr_t a, uint32_t* v) { return SafeRead(a, v, 4) == 0; }
static void LwNavTypeDump(void)
{
    static LONG level = -1; static int done; static ULONGLONG first;
    char t[4] = "";
    if (!(GetEnvironmentVariableA("HELLFORGE_LW_TEST_NAV", t, sizeof t) > 0 && t[0] == '1')) return;
    if (level != g_world.levels) { level = g_world.levels; done = 0; first = 0; }
    if (done >= 2 || (done == 1 && GetTickCount64() - first < 120000)) return;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL), game = g_lw.game, R = 0, Fm = 0, Fp = 0;
    if (SafeRead(exe + 0x27C5CF8u, &R, 8) || R == 0 || SafeRead(exe + 0x27C5BB8u, &Fm, 8) || SafeRead(exe + 0x27C5E38u, &Fp, 8)) return;
    int members = 0;
    for (int i = 0; i < g_lwWorld.count && i < LW_SQUADS_MAX; i++) if (g_lwSquads[i].state == LW_ONLINE) members += g_lwSquads[i].seenCount;
    if (members == 0) return;
    if (done++ == 0) first = GetTickCount64();
    uint32_t oA = 0, oT = 0, oW = 0, oK = 0, nA = 0, nT = 0, m = 0, k = 0, obst[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    if (!LwNavDumpU32(R + 0x2C, &oA) || !LwNavDumpU32(R + 0x34, &oT) || !LwNavDumpU32(R + 0x3C, &oW) || !LwNavDumpU32(R + 0x44, &oK) ||
        !LwNavDumpU32(R + oA, &nA) || !LwNavDumpU32(R + oT, &nT) || !LwNavDumpU32(R + oT + 4, &m) || !LwNavDumpU32(R + oK, &k) || nA > 64 || nT > 64 || m > 64)
    { Note("living world: test: nav types: the settings are not readable (areas %u, types %u, m %u)", nA, nT, m); return; }
    char line[1200]; size_t at = 0;
    for (uint32_t a = 0; a < nA && at < sizeof line - 40; a++)
    {
        uint32_t h = 0, fm = 0, fp = 0; LwNavDumpU32(R + oA + 4 + 4 * a, &h);
        if (Fm) LwNavDumpU32(Fm + 0x9C + 4 * (a & 31), &fm);
        if (Fp) LwNavDumpU32(Fp + 0xA0 + 4 * (a & 31), &fp);
        if (h == 0x96740A91u) obst[0] = a; if (h == 0xAAE05A4Bu) obst[1] = a;
        at += (size_t)snprintf(line + at, sizeof line - at, " %u:%08X/%08X/%08X", a, h, fm, fp);
    }
    uint32_t fmN = 0, fpN = 0, fpK = 0;
    if (Fm) LwNavDumpU32(Fm + 0x18, &fmN);
    if (Fp) { LwNavDumpU32(Fp + 0x130, &fpN); LwNavDumpU32(Fp + 0x134, &fpK); }
    Note("living world: test: nav types: %u areas (obstacle_small %d, obstacle_large %d), k %u; mover N %u, planner N %u k %u; area:hash/mover mask/planner mask:%s",
        nA, (int)obst[0], (int)obst[1], k, fmN, fpN, fpK, line);
    at = 0; line[0] = 0;
    for (uint32_t ty = 0; ty < nT && at < sizeof line - 60; ty++)
    {
        uint32_t h = 0, fbits = 0, word = 0, qm = 0, qp = 0, ep = 0; float f = 0;
        LwNavDumpU32(R + oT + 8 + 4 * ty, &h); LwNavDumpU32(R + oT + 8 + 4 * nT + 4 * m * nT + 4 * ty, &fbits); memcpy(&f, &fbits, 4);
        LwNavDumpU32(R + oW + 4 + 4 * ty, &word);
        if (Fm) LwNavDumpU32(Fm + 0x1C + 4 * (ty & 31), &qm);
        if (Fp) { LwNavDumpU32(Fp + 0x1B8 + 4 * (ty & 31), &qp); LwNavDumpU32(Fp + 0x138 + 4 * (ty & 31), &ep); }
        at += (size_t)snprintf(line + at, sizeof line - at, " %u:%08X f %.1f word %08X q %u/%u e %08X;", ty, h, (double)f, word, qm, qp, ep);
    }
    Note("living world: test: nav types: %u agent types (type:hash f word q mover/planner e):%s", nT, line);
    uintptr_t nav = 0, aiState = 0, table = 0; uint32_t places = 0, empty = 0, mult = 0; uintptr_t keys = 0, recs = 0;
    SafeRead(game + 0x3326418u, &nav, 8);
    if (nav) { SafeRead(nav + 0x30, &keys, 8); LwNavDumpU32(nav + 0x38, &places); LwNavDumpU32(nav + 0x3C, &empty); LwNavDumpU32(nav + 0x40, &mult); SafeRead(nav + 0x50, &recs, 8); }
    if (!SafeRead(game + 0x346BF98u, &aiState, 8) && aiState) SafeRead(aiState + 0xF12DE8u, &table, 8);
    int shown = 0;
    for (int i = 0; i < g_lwWorld.count && i < LW_SQUADS_MAX && shown < 40; i++)
    {
        const LwSquad* q = &g_lwSquads[i];
        if (q->state != LW_ONLINE) continue;
        for (int j = 0; j < q->seenCount && j < 32 && shown < 40; j++)
        {
            uint32_t e = q->seen[j], idx = 0xFFFFFFFFu, type = 0, unit = 0; float radius = 0;
            for (uint32_t p = 0; keys && places && (places & (places - 1)) == 0 && p < places; p++)
            {
                uint32_t kv[2] = {0, 0};
                if (SafeRead(keys + 8ull * ((e * mult + p) & (places - 1)), kv, 8) || kv[0] == empty) break;
                if (kv[0] == e) { idx = kv[1]; break; }
            }
            if (idx != 0xFFFFFFFFu && recs) { SafeRead(recs + 0xA8ull * idx + 0x98, &radius, 4); LwNavDumpU32(recs + 0xA8ull * idx + 0x9C, &type); }
            uint64_t res = EvpEntityResource(game, e, &unit);
            uint32_t ab[2] = {0, 0}, box[2] = {0, 0}; uint8_t walls = 0; int found = 0;
            for (uint32_t p = 0; table && res && p < 316; p++)
            {
                uint64_t key = 0; uint32_t sidx = 0; uintptr_t b = table + 16ull * ((res + p) % 316);
                if (SafeRead(b, &key, 8) || key == 0) break;
                if (key == res && !SafeRead(b + 8, &sidx, 4))
                {
                    uintptr_t st = table + 0x13C0 + 0x2DCull * sidx;
                    SafeRead(st + 0x200, ab, 8); SafeRead(st + 0x208, box, 8); SafeRead(st + 0x210, &walls, 1); found = 1; break;
                }
            }
            Note("living world: test: nav types: squad %d member %08X (resource %016llX): agent type %08X, radius %.2f; settings %s: obstacle abilities %u / %u, box %08X %08X, walls %u",
                i, e, (unsigned long long)res, type, (double)radius, found ? "found" : "NOT found", ab[0], ab[1], box[0], box[1], walls);
            shown++;
        }
    }
}
/* ---- A loader member's obstacle smash spares its own squad (user decision 2026-11-07; KB living-world-spawn-losses,
 * follow-ups 1-6, the bytes checked by me in game.dll) ----------------------------------------------------------------
 * Calm loader members died to squad mates' obstacle smashes (lw95b/c, lw97a: kind 6, amount 100, DamageInfo 9 at
 * 1.5-2.3 m). The game's obstacle clearing (0x7E63B0, slots filled by 0x7E5480) replays a unit's smash ability (its AI
 * settings +0x200 small / +0x204 large obstacle ability: 958, 1058, 829, 2730) while an obstacle area is in its box; the
 * ability's damage zone is centred on the unit and the zone processor 0x7D01C0 tests no faction: it skips only its
 * owner, the owner's links and targets already hit. The user's choice: "Keep the game's smashing as is (obstacles still
 * get cleared), but a loader squad member's smash zone doesn't damage members of its own squad."
 * The candidate loop (0x7D03A0..0x7D1040) maps each contact to its entity (edi, 0x7D03E1) and compares it with the
 * owner ([[rsp+0x78]+8] into ebx, 0x7D03EF..0x7D03FA: owner -> 0x7D0565). A candidate's only effect in the loop is
 * joining the target list; damage, hit reactions and pushes are applied after the loop from that list. The jump at
 * 0x7D03EF (17 bytes, no branch into 0x7D03F0..0x7D03FF) goes to a stub that redoes those instructions and asks
 * LwSmashSpares(owner, candidate): 1 = the vanilla skip 0x7D1017 (xor r13d, r13d; the loop's own next-contact path),
 * else on at 0x7D0400. Free at that point: rax, rcx, rdx, r8-r11, flags; the body's rsp is 16-aligned (seven pushes,
 * 0x950; movaps at 0x7D01DA), so the stub keeps xmm0-5 and calls C with 0x80 bytes reserved.
 * A zone keeps a copy of its descriptor and no ability id, and descriptors are shared, so the zone is told by its
 * owner's current ability (the record 0x4A40C0 reads: [game+0x3326640] keys {id, index} [+0x18], places +0x20, empty
 * key +0x24, multiplier +0x28, records [+0x38] + 0xE0 index, +0 ability id, +0x10 active): active and equal to the
 * owner's +0x200 or +0x204. The mate never joins the zone's already-hit set (re-tested and skipped each frame); the
 * zone's world hits (+0x334), and so the obstacle's clearing, are untouched. Ids are full 32-bit (generation from bit
 * 22); the table holds members whose index (id & 0x3FFFFF) is under 0x10000. Clients keep an empty table (the squads
 * are the host's): vanilla there. */
#define LW_SMASH_TAB 0x10000u
typedef struct { volatile uint32_t id, tag, ab0, ab1; } LwSmashEntry;
static LwSmashEntry g_lwSmash[LW_SMASH_TAB];
static uint32_t g_lwSmashSet[2048]; static int g_lwSmashSetN;
static volatile LONG g_lwSmashOn;
/* the obstacle abilities of a unit from its AI settings (0x511CC0's table): 1 when found */
static int LwObstacleAbilities(uint32_t e, uint32_t* ab0, uint32_t* ab1)
{
    uintptr_t game = g_lw.game, aiState = 0, table = 0; uint32_t unit = 0;
    uint64_t res = EvpEntityResource(game, e, &unit);
    if (!res || SafeRead(game + 0x346BF98u, &aiState, 8) || !aiState || SafeRead(aiState + 0xF12DE8u, &table, 8) || !table) return 0;
    for (uint32_t p = 0; p < 316; p++)
    {
        uint64_t key = 0; uint32_t sidx = 0; uintptr_t b = table + 16ull * ((res + p) % 316);
        if (SafeRead(b, &key, 8) || key == 0) return 0;
        if (key == res)
        {
            uint32_t ab[2] = {0, 0};
            if (SafeRead(b + 8, &sidx, 4) || SafeRead(table + 0x13C0 + 0x2DCull * sidx + 0x200, ab, 8)) return 0;
            *ab0 = ab[0]; *ab1 = ab[1]; return 1;
        }
    }
    return 0;
}
/* each squads tick: the online members, their squad as the tag; an entry changes only when a member comes or goes */
static void LwSmashTableUpdate(void)
{
    if (!g_lwSmashOn) return;
    static uint32_t want[2048]; int wantN = 0;
    for (int i = 0; i < g_lwWorld.count && i < LW_SQUADS_MAX; i++)
    {
        const LwSquad* q = &g_lwSquads[i];
        if (q->state != LW_ONLINE) continue;
        for (int k = 0; k < q->seenCount && k < 32 && wantN < 2048; k++)
        {
            uint32_t e = q->seen[k], s = e & 0x3FFFFFu, tag = (uint32_t)i + 1;
            if (e == 0 || s >= LW_SMASH_TAB) continue;
            LwSmashEntry* t = &g_lwSmash[s];
            if (t->id != e || t->tag != tag)
            {
                uint32_t a0 = 0, a1 = 0; LwObstacleAbilities(e, &a0, &a1);
                t->tag = 0; MemoryBarrier(); t->ab0 = a0; t->ab1 = a1; t->id = e; MemoryBarrier(); t->tag = tag;
            }
            want[wantN++] = s;
        }
    }
    for (int j = 0; j < g_lwSmashSetN; j++) /* members gone: cleared */
    {
        int still = 0;
        for (int k = 0; k < wantN && !still; k++) still = want[k] == g_lwSmashSet[j];
        if (!still) { g_lwSmash[g_lwSmashSet[j]].tag = 0; MemoryBarrier(); g_lwSmash[g_lwSmashSet[j]].id = 0; }
    }
    memcpy(g_lwSmashSet, want, sizeof want[0] * (size_t)wantN); g_lwSmashSetN = wantN;
}
/* 1 = target is a squad mate of owner and owner's obstacle ability (+0x200 / +0x204) is running */
static int LwSmashMate(uint32_t owner, uint32_t target)
{
    if ((owner & 0x3FFFFFu) >= LW_SMASH_TAB || (target & 0x3FFFFFu) >= LW_SMASH_TAB) return 0;
    const LwSmashEntry* a = &g_lwSmash[owner & 0x3FFFFFu]; const LwSmashEntry* b = &g_lwSmash[target & 0x3FFFFFu];
    uint32_t tag = a->tag;
    if (tag == 0 || a->id != owner || b->id != target || b->tag != tag || (a->ab0 == 0 && a->ab1 == 0)) return 0;
    uintptr_t m = *(volatile uintptr_t*)(g_lw.game + 0x3326640u);
    if (m == 0) return 0;
    uint32_t places = *(volatile uint32_t*)(m + 0x20), empty = *(volatile uint32_t*)(m + 0x24), mult = *(volatile uint32_t*)(m + 0x28);
    uintptr_t keys = *(volatile uintptr_t*)(m + 0x18), recs = *(volatile uintptr_t*)(m + 0x38);
    if (places == 0 || (places & (places - 1)) != 0 || keys == 0 || recs == 0) return 0;
    uint32_t index = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < places; i++)
    {
        const volatile uint32_t* k = (const volatile uint32_t*)(keys + 8ull * ((owner * mult + i) & (places - 1)));
        if (k[0] == empty) break;
        if (k[0] == owner) { index = k[1]; break; }
    }
    if (index == 0xFFFFFFFFu) return 0;
    uint32_t ability = *(volatile uint32_t*)(recs + 0xE0ull * index); uint8_t active = *(volatile uint8_t*)(recs + 0xE0ull * index + 0x10);
    return active && ability != 0 && (ability == a->ab0 || ability == a->ab1);
}
/* from the zone processor's candidate loop: 1 = skip this candidate */
static int LwSmashSpares(uint32_t owner, uint32_t target)
{
    if (!LwSmashMate(owner, target)) return 0;
    InterlockedIncrement(&g_lwSpared);
    return 1;
}
/* The explosion ability 2730 spawns (its step, frame switch 0x112E03F: frame 30 the zone, frame 46 0x11AD240(ctx, type
 * 0x106 at 0x112E0B4, node 0x5FC1CE78), frame 85 the zone ends, frame 105 the ability) goes through the spawner
 * 0x13C0A80 with source and attacker both [ctx+8], the owner's full id (0x11AD412, 0x11AD44F), and an empty ignore
 * list (count 0, 0x11AD438). Its record (mgr + 0x28 + 0x98 i) holds +0xC type (0x13C0ADB), +0x10 source, +0x14 attacker
 * (0x13C0B34), +0x28 ignore count, +0x58 ignored units (0x13C0C7B, 0x13C0C83). The update 0x13C6180 hands the record
 * to the damage pass 0x13C29E0 (lea rdx, [rdi+0x28] at 0x13C622D; kept at [rsp+0x78], 0x13C2A44), whose contact loop
 * (0x13C2D30..0x13C3960) maps each contact unit (edi, +0x1C) to its entity (ebx; the entity state's unit hash +0xF2AEE0,
 * the same the zones' 0xFD9980 reads) and then tests the ignore list at 0x13C2DC7 (only 0x13C2DBF jumps there; nothing
 * branches into 0x13C2DC8..0x13C2DDC); an ignored contact goes to 0x13C2FBA, which clears it and takes the next. The
 * patch puts a jump at 0x13C2DC7 to a stub that asks LwSmashBlastSpares(record, entity) and then either takes that
 * exit or redoes the 22 bytes and goes on at 0x13C2DDD. The damage lands about two frames after frame 46 while 2730 is
 * still active (until frame 105), so the zones' test applies as it is: owner and target in one squad, the owner's
 * obstacle ability active. Only the mate's contact is dropped: props and other units still take the explosion. Among
 * the obstacle abilities only 2730 spawns an explosion (958 0x10D3350, 1058 0x10D8110, 829 0x10CBC80 reach no spawner
 * or hit submitter; KB spawn-losses follow-up 7). The pass's rsp is 16-aligned (r11 = entry rsp, four pushes, 0x508). */
static int LwSmashBlastSpares(uintptr_t record, uint32_t target)
{
    if (*(volatile uint32_t*)(record + 0xC) != 0x106 || !LwSmashMate(*(volatile uint32_t*)(record + 0x14), target)) return 0;
    InterlockedIncrement(&g_lwBlastSpared);
    return 1;
}
static void LwInstallBlastSpares(uintptr_t game)
{
    if (!ApiCodeMatches(game + 0x13C2DBFu, "EB 06") ||
        !ApiCodeMatches(game + 0x13C2DC7u, "48 8B 4C 24 78 41 8B C6 89 44 24 5C 44 39 71 28 0F 86 19 01 00 00") ||
        !ApiCodeMatches(game + 0x13C2DDDu, "8B 15 3D 0E 0C 02 8B 4C 81 58") || !ApiCodeMatches(game + 0x13C2EF6u, "41 F7 45 20 00 00 80 00") ||
        !ApiCodeMatches(game + 0x13C2FBAu, "48 8B 45 80 4C 8B 7C 24 68 44 89 70 1C") ||
        !ApiCodeMatches(game + 0x13C2DB6u, "41 39 38 75 06 41 8B 58 04 EB 06 8B 1D") || !ApiCodeMatches(game + 0x13C2D61u, "48 8B 05 30 92 0A 02") ||
        !ApiCodeMatches(game + 0x13C0ADBu, "44 89 44 0F 34") || !ApiCodeMatches(game + 0x13C0B34u, "89 4C 1F 3C") ||
        !ApiCodeMatches(game + 0x13C622Du, "48 8D 57 28 48 8B CE E8 A7 C7 FF FF") || !ApiCodeMatches(game + 0x13C29E0u, "4C 8B DC 55 53 56 41 56") ||
        !ApiCodeMatches(game + 0x13C29EFu, "48 81 EC 08 05 00 00") || !ApiCodeMatches(game + 0x13C2A44u, "48 89 54 24 78") ||
        !ApiCodeMatches(game + 0x112E0B4u, "BA 06 01 00 00"))
    { Note("living world: the explosions' target test is not the reviewed code; obstacle smash explosions are not kept off squad mates"); return; }
    uint8_t* block = AllocateNearGame(game, 0x1000);
    if (block == NULL) { Note("living world: no memory near game.dll for the explosion stub; obstacle smash explosions are not kept off squad mates"); return; }
    static const uint8_t pre[156] = {
        0x48, 0x8B, 0x4C, 0x24, 0x78,             /* 0: mov rcx, [rsp+0x78] (the record) */
        0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, /* 5: push r8-r11 */
        0x48, 0x81, 0xEC, 0x80, 0, 0, 0,          /* 13: sub rsp, 0x80 */
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30, 0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,
        0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50, 0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70, /* 20: movdqu [rsp+0x20..0x70], xmm0-5 */
        0x89, 0xDA,                               /* 56: mov edx, ebx (the candidate) */
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,       /* 58: mov rax, LwSmashBlastSpares (imm64 at 60) */
        0xFF, 0xD0,                               /* 68: call rax */
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30, 0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,
        0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50, 0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70, /* 70: movdqu xmm0-5, [rsp+0x20..0x70] */
        0x48, 0x81, 0xC4, 0x80, 0, 0, 0,          /* 106: add rsp, 0x80 */
        0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, /* 113: pop r11-r8 */
        0x85, 0xC0,                               /* 121: test eax, eax */
        0x0F, 0x85, 0, 0, 0, 0,                   /* 123: jnz 0x13C2FBA (rel32 at 125) */
        0x48, 0x8B, 0x4C, 0x24, 0x78,             /* 129: mov rcx, [rsp+0x78] */
        0x41, 0x8B, 0xC6,                         /* 134: mov eax, r14d */
        0x89, 0x44, 0x24, 0x5C,                   /* 137: mov [rsp+0x5C], eax */
        0x44, 0x39, 0x71, 0x28,                   /* 141: cmp [rcx+0x28], r14d */
        0x0F, 0x86, 0, 0, 0, 0,                   /* 145: jbe 0x13C2EF6 (rel32 at 147) */
        0xE9, 0, 0, 0, 0 };                       /* 151: jmp 0x13C2DDD (rel32 at 152) */
    memcpy(block, pre, sizeof pre);
    uint64_t fn = (uint64_t)(uintptr_t)LwSmashBlastSpares; memcpy(block + 60, &fn, 8);
    int64_t r1 = (int64_t)(game + 0x13C2FBAu) - (int64_t)(uintptr_t)(block + 129), r2 = (int64_t)(game + 0x13C2EF6u) - (int64_t)(uintptr_t)(block + 151),
        r3 = (int64_t)(game + 0x13C2DDDu) - (int64_t)(uintptr_t)(block + 156);
    if (!InRel32(r1) || !InRel32(r2) || !InRel32(r3)) { Note("living world: the explosion stub is out of reach of game.dll"); return; }
    int32_t v = (int32_t)r1; memcpy(block + 125, &v, 4); v = (int32_t)r2; memcpy(block + 147, &v, 4); v = (int32_t)r3; memcpy(block + 152, &v, 4);
    DWORD protect; VirtualProtect(block, 0x1000, PAGE_EXECUTE_READ, &protect); FlushInstructionCache(GetCurrentProcess(), block, 0x1000);
    uint8_t patch[22]; memset(patch, 0xCC, sizeof patch);
    patch[0] = 0xE9; v = DifficultyJumpRel(game + 0x13C2DC7u, (uintptr_t)block); memcpy(patch + 1, &v, 4);
    if (ApiWrite(game + 0x13C2DC7u, patch, sizeof patch)) { Note("living world: the explosion stub's jump could not be written"); return; }
    Note("living world: a loader member's obstacle smash explosion spares its own squad mates (the explosions' target test, game.dll 0x13C2DC7)");
}
static void LwInstallSmashSpares(void)
{
    if (!LwEnabled() || g_lwSmashOn) return;
    uintptr_t game = g_lw.game;
    if (!ApiCodeMatches(game + 0x7D03E1u, "8B 38 3B 3D 63 38 CB 02") ||
        !ApiCodeMatches(game + 0x7D03EFu, "48 8B 44 24 78 8B 58 08 41 3B DC 0F 84 65 01 00 00") ||
        !ApiCodeMatches(game + 0x7D1017u, "45 33 ED 48 8B 5D A8") || !ApiCodeMatches(game + 0x7D01C0u, "40 55 53 56 57 41 55 41 56 41 57") ||
        !ApiCodeMatches(game + 0x4A40CAu, "4C 8B 15 6F 25 E8 02") || !ApiCodeMatches(game + 0x4A40DAu, "45 8B 4A 20") ||
        !ApiCodeMatches(game + 0x4A40E6u, "41 8B 5A 28") || !ApiCodeMatches(game + 0x4A4104u, "49 8B 7A 18 41 8B 72 24") ||
        !ApiCodeMatches(game + 0x4A414Au, "49 8B 42 38 48 69 D1 E0 00 00 00 80 7C 02 10 00"))
    { Note("living world: the damage zones' target test is not the reviewed code; obstacle smashes are not kept off squad mates"); return; }
    uint8_t* block = AllocateNearGame(game, 0x1000);
    if (block == NULL) { Note("living world: no memory near game.dll for the smash stub; obstacle smashes are not kept off squad mates"); return; }
    static const uint8_t pre[132] = {
        0x48, 0x8B, 0x44, 0x24, 0x78,             /* 0: mov rax, [rsp+0x78] */
        0x8B, 0x58, 0x08,                         /* 5: mov ebx, [rax+8] */
        0x41, 0x3B, 0xDC,                         /* 8: cmp ebx, r12d */
        0x0F, 0x84, 0, 0, 0, 0,                   /* 11: je 0x7D0565 (rel32 at 13) */
        0x48, 0x81, 0xEC, 0x80, 0, 0, 0,          /* 17: sub rsp, 0x80 */
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30, 0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,
        0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50, 0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70, /* 24: movdqu [rsp+0x20..0x70], xmm0-5 */
        0x89, 0xD9,                               /* 60: mov ecx, ebx (the owner) */
        0x89, 0xFA,                               /* 62: mov edx, edi (the candidate) */
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,       /* 64: mov rax, LwSmashSpares (imm64 at 66) */
        0xFF, 0xD0,                               /* 74: call rax */
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30, 0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,
        0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50, 0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70, /* 76: movdqu xmm0-5, [rsp+0x20..0x70] */
        0x48, 0x81, 0xC4, 0x80, 0, 0, 0,          /* 112: add rsp, 0x80 */
        0x85, 0xC0,                               /* 119: test eax, eax */
        0x0F, 0x85, 0, 0, 0, 0,                   /* 121: jnz 0x7D1017 (rel32 at 123) */
        0xE9, 0, 0, 0, 0 };                       /* 127: jmp 0x7D0400 (rel32 at 128) */
    memcpy(block, pre, sizeof pre);
    uint64_t fn = (uint64_t)(uintptr_t)LwSmashSpares; memcpy(block + 66, &fn, 8);
    int64_t r1 = (int64_t)(game + 0x7D0565u) - (int64_t)(uintptr_t)(block + 17), r2 = (int64_t)(game + 0x7D1017u) - (int64_t)(uintptr_t)(block + 127),
        r3 = (int64_t)(game + 0x7D0400u) - (int64_t)(uintptr_t)(block + 132);
    if (!InRel32(r1) || !InRel32(r2) || !InRel32(r3)) { Note("living world: the smash stub is out of reach of game.dll"); return; }
    int32_t v = (int32_t)r1; memcpy(block + 13, &v, 4); v = (int32_t)r2; memcpy(block + 123, &v, 4); v = (int32_t)r3; memcpy(block + 128, &v, 4);
    DWORD protect; VirtualProtect(block, 0x1000, PAGE_EXECUTE_READ, &protect); FlushInstructionCache(GetCurrentProcess(), block, 0x1000);
    uint8_t patch[17]; memset(patch, 0xCC, sizeof patch);
    patch[0] = 0xE9; v = DifficultyJumpRel(game + 0x7D03EFu, (uintptr_t)block); memcpy(patch + 1, &v, 4);
    InterlockedExchange(&g_lwSmashOn, 1); /* the table fills from the next squads tick; until then every test answers 0 */
    if (ApiWrite(game + 0x7D03EFu, patch, sizeof patch)) { g_lwSmashOn = 0; Note("living world: the smash stub's jump could not be written"); return; }
    Note("living world: a loader member's obstacle smash spares its own squad mates (the zone processor's target test, game.dll 0x7D03EF)");
    LwInstallBlastSpares(game);
}
static void LwInstallHitNotes(void)
{
    char t[4] = "";
    if (!(GetEnvironmentVariableA("HELLFORGE_LW_TEST_HITS", t, sizeof t) > 0 && t[0] == '1') || !LwEnabled()) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (!game) return;
    if (!ApiCodeMatches(game + 0x9235F0u, "4C 8B DC 55 53 41 54 49 8D AB 48 F5 FF FF 48 81 EC C0 0B 00 00") ||
        !ApiCodeMatches(game + 0x92361Eu, "8B 85 E0 0A 00 00 4C 8B E1 89 45 04 8B 85 E8 0A 00 00 89 45 00") ||
        !ApiCodeMatches(game + 0x13C0A80u, "40 53 48 83 EC 30 8B 41 20 4C 8B DA 48 8B D9") ||
        !ApiCodeMatches(game + 0xFD9BA0u, "48 83 EC 08 4C 8B 1D ED 23 49 02 44 8B C2"))
    { Note("living world: test: the damage apply / explosion spawner is not the reviewed code; hits are not noted"); return; }
    LoaderHook("living world test: hits on loader members (damage apply)", game + 0x9235F0u, (void*)LwHitApply, (void**)&g_lwApply);
    LoaderHook("living world test: explosions near loader members", game + 0x13C0A80u, (void*)LwHitBlast, (void**)&g_lwBlast);
    if (ApiCodeMatches(game + 0x129F910u, "40 55 53 56 57 41 56 48 8D 6C 24 F9 48 81 EC D0 00 00 00"))
        LoaderHook("living world test: damage queued on loader members", game + 0x129F910u, (void*)LwHitQueue, (void**)&g_lwQueue);
    else Note("living world: test: QueueDamage is not the reviewed code; queued damage is not noted");
    if (ApiCodeMatches(game + 0x7D38D0u, "48 89 5C 24 10 48 89 7C 24 18 55 41 56 41 57 48 8D AC 24 40 FC FF FF 48 81 EC C0 04 00 00") &&
        ApiCodeMatches(game + 0x7D390Au, "45 8B 4A 38") && ApiCodeMatches(game + 0x7CAFDEu, "F3 0F 10 B4 24 00 01 00 00") &&
        ApiCodeMatches(game + 0x7CAF40u, "4C 8B DC 49 89 5B 18 55 56 57 41 55 41 56 48 81 EC B0 00 00 00"))
    {
        LoaderHook("living world test: damage zones started by loader members", game + 0x7D38D0u, (void*)LwHitZone, (void**)&g_lwZone);
        LoaderHook("living world test: abilities started by loader members", game + 0x7CAF40u, (void*)LwHitAbility, (void**)&g_lwAbility);
    }
    else Note("living world: test: the zone / ability starts are not the reviewed code; they are not noted");
    Note("living world: test: hits on online loader members and explosions within 15 m of them are noted (0x9235F0, 0x13C0A80)");
}

/* From InstallWorld (an extra large world is configured), before any mission: the stub with G = 0. */
static void LwInstallWallReach(uintptr_t game)
{
    if (!LwEnabled()) return; /* audit 1b: the leave fix patched the exe with the living world off; only its rebuilds need it */
    LwInstallNavLeaveFix();
    LwInstallNavSwapSync();
    if (g_lwWallReach != NULL) return;
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
#define LW_NAV_COPY_ROWS 512 /* 4 MB a tick */
static struct
{
    int pins;                       /* 0 not checked, 1 ok, -1 not the reviewed code */
    uintptr_t heights; LONG level;  /* the height copy written to, for this level */
    uint16_t* orig;                 /* that copy as the game made it (the coarse heights) */
    uintptr_t copying; LONG copyLevel; uint32_t copyRows; /* orig being taken, LW_NAV_COPY_ROWS rows a tick */
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

/* Marks the cells of a tile change at once, each watched until it completes; how many. Built and never built cells
 * alike: each completion's section swap is applied before the next (LwInstallNavSwapSync). */
static int LwNavMarkCells(uintptr_t nav, uint32_t id, const uint16_t* wh, uint32_t total, const uint8_t* cells)
{
    LwMarkFn mark = (LwMarkFn)((uintptr_t)GetModuleHandleW(NULL) + LW_NAV_MARK);
    float g[4] = {0, 0, 0, 0}; SafeRead(nav + 0x671C, g, 16);
    char list[256]; int len = 0, marked = 0; list[0] = 0;
    for (uint32_t x = 0; x < total; x++)
        if (cells[x])
        {
            uint16_t cx = (uint16_t)(x % wh[0]), cy = (uint16_t)(x / wh[0]);
            float xy[2] = {g[0] + ((float)cx + 0.5f) * (g[2] - g[0]) / wh[0], g[1] + ((float)cy + 0.5f) * (g[3] - g[1]) / wh[1]};
            uint32_t section = 0; LwCellFlags(nav, x, &section);
            mark(id, xy); marked++;
            if (g_lwNav.watching < 64) { g_lwNav.watch[g_lwNav.watching].cx = cx; g_lwNav.watch[g_lwNav.watching].cy = cy;
                g_lwNav.watch[g_lwNav.watching].section = section; g_lwNav.watch[g_lwNav.watching].at = GetTickCount64(); g_lwNav.watching++; }
            if (len < (int)sizeof list - 16) len += snprintf(list + len, sizeof list - len, " (%u, %u)", cx, cy);
        }
    if (marked) Note("living world: nav: %d cells marked:%s", marked, list);
    return marked;
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
    static uint32_t lastList[64]; static int lastCount = -1; static uintptr_t lastNav; /* 64: 512 m cells (KB nav-small-cells) */
    if (!EvpPtr(g_lw.game + 0x33263C8u, &table) || !EvpPtr(table + 0x70, &fn) || fn == 0 || !EvpPtr(g_lw.game + 0x3326340u, &world)) return;
    uint32_t id = EvpU32(world + 0x10F0, 0xFFFFFFFFu);
    float g[4] = {0, 0, 0, 0};
    if (SafeRead(nav + 0x671C, g, 16) || g[2] <= g[0] || g[3] <= g[1]) return;
    float cw = (g[2] - g[0]) / wh[0], ch = (g[3] - g[1]) / wh[1];
    uint32_t list[64]; int count = 0;
    /* Two tiers (512 m cells, KB nav-small-cells): every tick the cells within R_on, and every 4th tick those within
     * LW_BOOST_FAR, so squads seen far out (the drawn-distance band, 700-950 m for most units) find navmesh there,
     * while the cells around the players still come first (the game adds +100 a frame to a player's own cell). */
    static unsigned tick; tick++;
    float reach = (tick & 3) == 0 ? LW_BOOST_FAR : LW_R_ON;
    for (int p = 0; p < g_lwWorld.playersAt; p++)
    {
        const float* at = g_lwWorld.players[p];
        int i0 = (int)floorf((at[0] - reach - g[0]) / cw), i1 = (int)floorf((at[0] + reach - g[0]) / cw);
        int j0 = (int)floorf((at[1] - reach - g[1]) / ch), j1 = (int)floorf((at[1] + reach - g[1]) / ch);
        for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < wh[1]; j++)
            for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < wh[0]; i++)
            {
                uint32_t c = (uint32_t)j * wh[0] + (uint32_t)i; int dup = 0;
                for (int k = 0; k < count && !dup; k++) dup = list[k] == c;
                if (dup || count >= 64) continue;
                list[count++] = c;
                float xyz[3] = {g[0] + ((float)i + 0.5f) * cw, g[1] + ((float)j + 0.5f) * ch, at[2]};
                ((BoostFn)fn)(id, xyz);
            }
    }
    if (reach == LW_R_ON && (nav != lastNav || count != lastCount || memcmp(list, lastList, sizeof(uint32_t) * (size_t)count)))
    {
        char text[640]; int len = 0; text[0] = 0;
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
        if (g_lwNav.orig == NULL) { LwNavWait("no memory for a copy of the nav's heights"); return; }
        /* 32 MB in slices (audit 1f pass caveat 10: the copy and the populate made one 56-67 ms frame after the landing) */
        if (g_lwNav.copying != H || g_lwNav.copyLevel != g_world.levels) { g_lwNav.copying = H; g_lwNav.copyLevel = g_world.levels; g_lwNav.copyRows = 0; }
        uint32_t rows = n - g_lwNav.copyRows < LW_NAV_COPY_ROWS ? n - g_lwNav.copyRows : LW_NAV_COPY_ROWS;
        if (SafeRead(data + (size_t)g_lwNav.copyRows * n * 2, g_lwNav.orig + (size_t)g_lwNav.copyRows * n, (size_t)rows * n * 2))
        { g_lwNav.copying = 0; LwNavWait("no copy of the nav's heights"); return; }
        g_lwNav.copyRows += rows;
        if (g_lwNav.copyRows < n) return;
        g_lwNav.copying = 0;
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
    /* After a swap the hooks could not synchronise: no new marks until the AI world is idle (LwNavSwapHeld) */
    if (LwNavSwapHeld()) { g_lwNav.busyFrames++; return; }
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
    (void)exe;
    Note("living world: nav: tile %d %s: %u samples written (%u from the tile) after %u busy frames; G %.1f m", k,
        want[k] ? "in" : "gone", written, fromTile, g_lwNav.busyFrames, (double)(g_lwWallReach ? *g_lwWallReach : 0));
    g_lwNav.busyFrames = 0;
    LwNavMarkCells(nav, id, wh, total, cells);
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

#include "living_world_intent.c" /* Phase 2: squad intents (investigate noise) */
