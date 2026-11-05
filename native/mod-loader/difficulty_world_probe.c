/* ---- Test tooling for extra large maps (test runs only; nothing here runs unless its switch is set) ---------------
 *
 * The probe (HELLFORGE_XL_EYE=1): xl-probe.txt in the session's logs, one command a write, "ID command args":
 *   ID props X Y R [TOL]  every unit with meshes within R m of (X, Y): its resource hash, its root, its lowest point and
 *                         the drawn ground under it; FLOAT when the whole box's bottom is TOL m (0.3) above the ground
 *                         everywhere under it, BURIED when its top is under the ground.
 *   ID inside X Y H       the units whose mesh boxes hold the point H m above the drawn ground at (X, Y) (with 0.15 m to
 *                         spare): an eye there would be inside them.
 *   ID enemies X Y R      the enemy budget's entities within R m (positions, resources).
 *   ID big X Y R S        every unit whose root is within R m and whose mesh boxes (world space) span more than S m
 *                         on an axis: a mesh drawn kilometres long (a node matrix gone wrong).
 *   ID depth X Y Z        a stratagem beacon's water depth at the point (0x13CA650): the game's coarse ray and, over a
 *                         near tile, with the ground as drawn (difficulty_world_terrain.c, BeaconDepth).
 * The answer goes to xl-probe-out.txt (written whole, then moved in place); its first line is "ID done ...".
 *
 * Where it reads (helldivers2.exe, build 25480438; static analysis in the knowledge base, xl-devtools entry):
 *  - the world [[game+0x3326340]+0x10E8] (game 0x13866F2 passes it); its every unit: count u32 W+0x2305B8, pointers
 *    W+0x2305C0 (Lua World.units 0x3EAF80 reads them, no lock; creation 0x1B5420 stores the index at obj+0xC);
 *  - a unit: +8 id, +0xC its index, +0x30 its resource ([res] the name hash, [res+0x28] class 2 full, 1 lightweight);
 *    full (vtable 0x1676D80): graph obj+0x60, meshes [obj+0xC8] count, [obj+0xD0] 16-byte entries (0x2BD890/0x2BD8A0);
 *    lightweight (0x16770E0): graph obj+0x48, count u32 [res+[res+0x64]] (0 when [res+0x64] is 0), pointers [obj+0xB0]
 *    (0x2CAC70/0x2CAC80); a graph's world matrices at [graph+0x28], 0x40 each, node 0 the root;
 *  - a mesh: node world matrix [[m+0x90]+0x28] + [m+0x98]*0x40 (0x9EE40), its local box min +0xA0, max +0xAC
 *    (Mesh.bounding_volume 0x488FB0); a matrix's rows are the axes (scaled), its translation at +0x30
 *    (world = x r0 + y r1 + z r2 + r3, 0x1FFA78).
 * The walk runs on the game thread (TickDetour), PROBE_PER_FRAME units a frame, every read checked.
 *
 * No enemies (HELLFORGE_XL_NO_ENEMIES=1; the host): for evidence runs with nothing in the way (patrols, the sweep,
 * and the strict hooks below; s71a showed bot drops still came with the first two alone).
 *  - patrols: the patrol caps of every difficulty row the director copies (game 0x94FE30, the site level 11's table
 *    moves; DifficultyRowOf) set to 0: row +0x2AC (faction 2), +0x2B4 (4), +0x2B0 (8) are read at 0x94FF3A/0x94FF33/
 *    0x94FEE6 from the copy, passed through 0x959650 (a multiply) and gate every patrol at 0x9513A1;
 *  - the rest: every half second, each enemy-budget entity within PROBE_CLEAR m of a player is removed by the game's
 *    own removal 0xA2C220(budget [game+0x3326838], index) (its despawn ability when it has one, else the destroy
 *    0xFDC4E0); the budget's entities: count u32 +0x1664 (appends at the end, 0xA2EC60), pointers +0x1688 (the entity id
 *    at +8), a removal marks +0xC of its 16-byte record at [+0x1690] (0xA2C2E6). */

#define PROBE_PER_FRAME 3000u
#define PROBE_UNITS_MAX (1u << 20)
#define PROBE_CLEAR 150.0f
#define PROBE_OUT_MAX (1u << 20)

typedef struct { uint64_t hash; uint32_t id; int cls, meshes; float root[3], lo[3], hi[3], ground[2], groundRoot; } ProbeUnit;

static struct
{
    int on, pinned;
    wchar_t in[MAX_PATH], out[MAX_PATH], tmp[MAX_PATH];
    FILETIME stamp;
    ULONGLONG checked;
    /* the command being walked */
    int busy, kind; /* 1 props, 2 inside, 3 big */
    char id[32];
    float x, y, r, tol, h, point[3];
    uintptr_t world;
    uint32_t next, count, scanned, skipped;
    char* text; size_t used;
    uint32_t hits, camera;
    /* units to leave out of "props": the enemies' and the players' */
    uint32_t others[512]; int otherCount;
} g_probe;

/* Strict: every squad request (0x953580; its own refusals return NULL from the top, 0x954079, after only the gate
 * checks 0x952EB0; callers test the result or ignore it) and every encounter admission (0x957140; its three callers
 * 0x8F5DA7, 0x8FAB88, 0x9573E4 test al) refused: bot drops, breaches, waves, garrisons. */
static void* g_noEnemySquadOriginal; static void* g_noEnemyAdmitOriginal;
static volatile LONG g_noEnemySquads, g_noEnemyAdmits;
static uintptr_t NoEnemiesSquad(void) { InterlockedIncrement(&g_noEnemySquads); return 0; }
static uint8_t NoEnemiesAdmit(void) { InterlockedIncrement(&g_noEnemyAdmits); return 0; }

/* props: every box within reach (support checks) and the FLOAT candidates, decided when the walk ends */
#define PROBE_BOXES 32768
#define PROBE_FLOATS 4096
static struct { uint32_t id; float lo[3], hi[3]; } g_probeBoxes[PROBE_BOXES];
static struct { ProbeUnit u; float level; int onLevel; } g_probeFloats[PROBE_FLOATS];
static int g_probeBoxN, g_probeFloatN;

static struct { int state; ULONGLONG next; uint32_t saved[DIF_VANILLA + 1][3]; int savedOk; uint32_t removed, rows; } g_noEnemies;

static void ProbeAppend(const char* format, ...)
{
    if (g_probe.text == NULL) g_probe.text = (char*)VirtualAlloc(NULL, PROBE_OUT_MAX, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_probe.text == NULL || g_probe.used >= PROBE_OUT_MAX - 512) return;
    va_list a; va_start(a, format);
    int n = vsnprintf(g_probe.text + g_probe.used, PROBE_OUT_MAX - g_probe.used, format, a);
    va_end(a);
    if (n > 0) g_probe.used += (size_t)n < PROBE_OUT_MAX - g_probe.used ? (size_t)n : 0;
}

/* The answer: its header line, then the lines gathered, written whole and moved over xl-probe-out.txt. */
static void ProbeFinish(const char* header)
{
    HANDLE f = CreateFileW(g_probe.tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD wrote;
        WriteFile(f, header, (DWORD)strlen(header), &wrote, NULL);
        if (g_probe.text && g_probe.used) WriteFile(f, g_probe.text, (DWORD)g_probe.used, &wrote, NULL);
        CloseHandle(f);
        MoveFileExW(g_probe.tmp, g_probe.out, MOVEFILE_REPLACE_EXISTING);
    }
    g_probe.used = 0; g_probe.busy = 0;
}

/* The ground as drawn there (the eye's: the tile, its band, the coarse terrain); NAN off the map or before the tile. */
static float ProbeGround(float x, float y)
{
    if (g_nearCoarseGround.v == NULL) return NAN;
    float g = NearEyeGround(x, y);
    return g > -9000.0f ? g : NAN;
}

static int ProbeMesh(uintptr_t mesh, float m[16], float lo[3], float hi[3])
{
    uint8_t b[0x30]; uintptr_t graph, poses; int32_t node;
    if (mesh < 0x10000 || SafeRead(mesh + 0x90, b, sizeof b)) return 0;
    memcpy(&graph, b, 8); memcpy(&node, b + 8, 4); memcpy(lo, b + 0x10, 12); memcpy(hi, b + 0x1C, 12);
    if (graph < 0x10000 || node < 0 || node > 4096 || SafeRead(graph + 0x28, &poses, 8) || poses < 0x10000 ||
        SafeRead(poses + (uintptr_t)node * 0x40, m, 64)) return 0;
    for (int k = 0; k < 3; k++) if (!isfinite(lo[k]) || !isfinite(hi[k]) || hi[k] < lo[k] || hi[k] - lo[k] > 4000.0f) return 0;
    for (int k = 0; k < 16; k++) if (!isfinite(m[k])) return 0;
    return 1;
}

/* A point in a mesh's box (its node's world matrix m, the box lo..hi in the mesh's space): how deep (m), or -1 outside. */
static float ProbeInsideBox(const float* m, const float* lo, const float* hi, const float* p)
{
    /* world = R local + t, R's columns the matrix's rows: solve R local = p - t */
    const float* a = m; const float* b = m + 4; const float* c = m + 8;
    float d[3] = {p[0] - m[12], p[1] - m[13], p[2] - m[14]};
    /* R = [a b c] as columns; R^-1 rows are (b x c, c x a, a x b) / det */
    float bc[3] = {b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0]};
    float ca[3] = {c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0]};
    float ab[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    float det = a[0] * bc[0] + a[1] * bc[1] + a[2] * bc[2];
    if (fabsf(det) < 1e-9f) return -1;
    float l[3] = {(bc[0] * d[0] + bc[1] * d[1] + bc[2] * d[2]) / det, (ca[0] * d[0] + ca[1] * d[1] + ca[2] * d[2]) / det,
        (ab[0] * d[0] + ab[1] * d[1] + ab[2] * d[2]) / det};
    float depth = 1e9f;
    for (int k = 0; k < 3; k++)
    {
        const float* axis = k == 0 ? a : k == 1 ? b : c;
        float scale = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        float in = fminf(l[k] - lo[k], hi[k] - l[k]) * scale; /* metres to the nearer face on this axis */
        if (in < -0.15f) return -1;
        if (in < depth) depth = in;
    }
    return depth;
}

/* The enemies' and the players' units (their health records' units), left out of "props". */
static void ProbeOthers(uintptr_t game)
{
    g_probe.otherCount = 0;
    EvpPlayer players[EVT_MAX_PLAYERS];
    int n = EvpPlayers(game, players);
    for (int i = 0; i < n; i++)
    {
        ActHealth h;
        if (players[i].avatar && ActHealthOf(game, players[i].avatar, &h) && h.unit && g_probe.otherCount < 512) g_probe.others[g_probe.otherCount++] = h.unit;
    }
    uintptr_t budget = 0, list = 0; uint32_t count = 0;
    if (SafeRead(game + 0x3326838u, &budget, 8) || budget == 0 || SafeRead(budget + 0x1664, &count, 4) || count > 4096 ||
        SafeRead(budget + 0x1688, &list, 8) || list == 0) return;
    for (uint32_t i = 0; i < count && g_probe.otherCount < 512; i++)
    {
        uintptr_t e = 0; uint32_t entity = 0; ActHealth h;
        if (SafeRead(list + (uintptr_t)i * 8, &e, 8) || e == 0 || SafeRead(e + 8, &entity, 4)) continue;
        if (ActHealthOf(game, entity, &h) && h.unit) g_probe.others[g_probe.otherCount++] = h.unit;
    }
}

static int ProbeOther(uint32_t id)
{
    for (int i = 0; i < g_probe.otherCount; i++) if (g_probe.others[i] == id) return 1;
    return 0;
}

/* One unit of the walk: "props" lists it, "inside" checks the point against its meshes. */
static void ProbeUnitStep(uintptr_t exe, uintptr_t obj, uint32_t index)
{
    uint8_t o[0xD8];
    int full;
    if (obj < 0x10000) return;
    if (SafeRead(obj, o, 0xD8)) { if (SafeRead(obj, o, 0xB8)) { g_probe.skipped++; return; } }
    uintptr_t vt, res, poses; uint32_t id, at;
    memcpy(&vt, o, 8); memcpy(&id, o + 8, 4); memcpy(&at, o + 0xC, 4); memcpy(&res, o + 0x30, 8);
    if (vt == exe + 0x1676D80u) full = 1; else if (vt == exe + 0x16770E0u) full = 0; else { g_probe.skipped++; return; }
    uint8_t cls = 0; uint64_t hash = 0;
    if (at != index || res < 0x10000 || SafeRead(res, &hash, 8) || SafeRead(res + 0x28, &cls, 1) || cls != (full ? 2 : 1)) { g_probe.skipped++; return; }
    memcpy(&poses, o + (full ? 0x60 : 0x48) + 0x28, 8);
    float root[16];
    if (poses < 0x10000 || SafeRead(poses, root, 64) || !isfinite(root[12]) || !isfinite(root[13]) || !isfinite(root[14])) { g_probe.skipped++; return; }
    g_probe.scanned++;
    /* the gameplay camera's own unit stands at the camera's position (the test eye's, else the game's) (its box holds points tens of metres away) */
    const float* cam = g_eye.on ? g_eye.pos : g_eye.game;
    if (fabsf(root[12] - cam[0]) < 0.5f && fabsf(root[13] - cam[1]) < 0.5f && fabsf(root[14] - cam[2]) < 0.5f) { g_probe.camera = id; return; }
    float dx = root[12] - g_probe.x, dy = root[13] - g_probe.y;
    float reach = g_probe.kind == 3 ? g_probe.r : g_probe.r + 200.0f; /* the largest boxes reach this far from their root */
    if (dx * dx + dy * dy > reach * reach) return;
    uint32_t meshes = 0; uintptr_t list = 0;
    if (full) { memcpy(&meshes, o + 0xC8, 4); memcpy(&list, o + 0xD0, 8); }
    else
    {
        uint32_t off = 0;
        memcpy(&list, o + 0xB0, 8);
        if (SafeRead(res + 0x64, &off, 4) || (off && (off > 0x100000 || SafeRead(res + off, &meshes, 4)))) meshes = 0;
    }
    if (meshes == 0 || meshes > 256 || list < 0x10000) return;
    ProbeUnit u = {hash, id, full ? 2 : 1, 0, {root[12], root[13], root[14]}, {1e9f, 1e9f, 1e9f}, {-1e9f, -1e9f, -1e9f}, {NAN, NAN}, NAN};
    float deepest = -1; int inMesh = -1;
    float cornersX[2048], cornersY[2048]; int corners = 0;
    for (uint32_t i = 0; i < meshes; i++)
    {
        uintptr_t mesh = 0;
        if (SafeRead(list + (uintptr_t)i * (full ? 16 : 8), &mesh, 8)) continue;
        float m[16], lo[3], hi[3];
        if (!ProbeMesh(mesh, m, lo, hi)) continue;
        u.meshes++;
        if (g_probe.kind == 2)
        {
            float d = ProbeInsideBox(m, lo, hi, g_probe.point);
            if (d > deepest) { deepest = d; inMesh = (int)i; }
            continue;
        }
        for (int c = 0; c < 8; c++)
        {
            float p[3] = {(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2]}, w[3];
            for (int k = 0; k < 3; k++) w[k] = p[0] * m[k] + p[1] * m[4 + k] + p[2] * m[8 + k] + m[12 + k];
            for (int k = 0; k < 3; k++) { if (w[k] < u.lo[k]) u.lo[k] = w[k]; if (w[k] > u.hi[k]) u.hi[k] = w[k]; }
            if (corners < 2048) { cornersX[corners] = w[0]; cornersY[corners] = w[1]; corners++; }
        }
    }
    if (u.meshes == 0) return;
    if (g_probe.kind == 3)
    {
        float ex = u.hi[0] - u.lo[0], ey = u.hi[1] - u.lo[1], ez = u.hi[2] - u.lo[2];
        if (dx * dx + dy * dy > g_probe.r * g_probe.r || !(fmaxf(ex, fmaxf(ey, ez)) > g_probe.tol)) return;
        g_probe.hits++;
        ProbeAppend("big %016llx unit %08X class %d meshes %d root (%.1f, %.1f, %.1f); box x %.1f..%.1f, y %.1f..%.1f, z %.1f..%.1f\n", (unsigned long long)u.hash,
            u.id, u.cls, u.meshes, (double)u.root[0], (double)u.root[1], (double)u.root[2], (double)u.lo[0], (double)u.hi[0], (double)u.lo[1], (double)u.hi[1],
            (double)u.lo[2], (double)u.hi[2]);
        return;
    }
    if (g_probe.kind == 2)
    {
        if (deepest < 0) return;
        g_probe.hits++;
        ProbeAppend("inside %016llx unit %08X class %d at (%.2f, %.2f, %.2f): %.2f m into mesh %d's box\n", (unsigned long long)u.hash, u.id, u.cls,
            (double)u.root[0], (double)u.root[1], (double)u.root[2], (double)deepest, inMesh);
        return;
    }
    /* props: within R of the point by the box, the drawn ground under the root and the box's corners */
    float nx = fmaxf(u.lo[0] - g_probe.x, fmaxf(0, g_probe.x - u.hi[0])), ny = fmaxf(u.lo[1] - g_probe.y, fmaxf(0, g_probe.y - u.hi[1]));
    if (nx * nx + ny * ny > g_probe.r * g_probe.r) return;
    if (u.hi[0] - u.lo[0] > 300.0f || u.hi[1] - u.lo[1] > 300.0f) return; /* terrain-sized: not a prop */
    int other = ProbeOther(u.id);
    u.groundRoot = ProbeGround(u.root[0], u.root[1]);
    float gmin = u.groundRoot, gmax = u.groundRoot;
    for (int c = 0; c < corners; c++)
    {
        float g = ProbeGround(cornersX[c], cornersY[c]);
        if (isnan(g)) continue;
        if (isnan(gmin) || g < gmin) gmin = g;
        if (isnan(gmax) || g > gmax) gmax = g;
    }
    u.ground[0] = gmin; u.ground[1] = gmax;
    const char* flag = "";
    if (g_probeBoxN < PROBE_BOXES) { g_probeBoxes[g_probeBoxN].id = u.id; memcpy(g_probeBoxes[g_probeBoxN].lo, u.lo, 12); memcpy(g_probeBoxes[g_probeBoxN].hi, u.hi, 12); g_probeBoxN++; }
    int floating = !isnan(gmax) && !other && u.lo[2] > gmax + g_probe.tol;
    if (floating) ; /* decided at the walk's end (ProbeFloats): FLOAT, or STACKED on another unit's box */
    else if (!isnan(gmin) && !other && u.hi[2] < gmin - 0.05f) { if (u.hi[2] > gmin - 5.0f) { flag = " BURIED"; g_probe.hits++; } else flag = " parked"; } /* far under: parked, unseen */
    /* the level's own surface there (the coarse heights, ring restored): a unit placed on it rather than on the tile
     * has its bottom at this height (ON-LEVEL) */
    float level = g_nearCoarseGround.v != NULL ? NearGroundAt(&g_nearCoarseGround, u.root[0], u.root[1]) : NAN;
    int onLevel = !isnan(level) && level > -9000.0f && fabsf(u.lo[2] - level) < 0.15f && fabsf(level - u.groundRoot) > 0.3f;
    if (floating)
    {
        if (g_probeFloatN < PROBE_FLOATS) { g_probeFloats[g_probeFloatN].u = u; g_probeFloats[g_probeFloatN].level = level; g_probeFloats[g_probeFloatN].onLevel = onLevel; g_probeFloatN++; }
        return;
    }
    ProbeAppend("%016llx unit %08X class %d%s meshes %d root (%.2f, %.2f, %.2f) ground at root %.2f, level %.2f; box z %.2f..%.2f, x %.1f..%.1f, y %.1f..%.1f; "
        "ground under it %.2f..%.2f; gap %+.2f%s%s\n", (unsigned long long)u.hash, u.id, u.cls, other ? " (enemy or player)" : "", u.meshes,
        (double)u.root[0], (double)u.root[1], (double)u.root[2], (double)u.groundRoot, (double)level, (double)u.lo[2], (double)u.hi[2], (double)u.lo[0], (double)u.hi[0],
        (double)u.lo[1], (double)u.hi[1], (double)gmin, (double)gmax, (double)(u.lo[2] - gmax), flag, onLevel ? " ON-LEVEL" : "");
}

static void ProbeEnemies(uintptr_t game)
{
    uintptr_t budget = 0, list = 0; uint32_t count = 0, within = 0;
    if (SafeRead(game + 0x3326838u, &budget, 8) || budget == 0 || SafeRead(budget + 0x1664, &count, 4) || count > 4096 ||
        SafeRead(budget + 0x1688, &list, 8) || list == 0) { ProbeFinish("enemies: the budget does not read\n"); return; }
    for (uint32_t i = 0; i < count; i++)
    {
        uintptr_t e = 0; uint32_t entity = 0; ActHealth h; float p[3];
        if (SafeRead(list + (uintptr_t)i * 8, &e, 8) || e == 0 || SafeRead(e + 8, &entity, 4)) continue;
        if (!ActHealthOf(game, entity, &h) || !h.unit || !EvpUnitPosition(h.unit, p)) continue;
        float dx = p[0] - g_probe.x, dy = p[1] - g_probe.y;
        if (dx * dx + dy * dy > g_probe.r * g_probe.r) continue;
        within++;
        ProbeAppend("enemy entity %08X resource %016llx%s at (%.1f, %.1f, %.1f), %.0f m, life %u\n", entity, (unsigned long long)h.resource,
            ActEnemy(h.resource) ? "" : " (not an enemy type)", (double)p[0], (double)p[1], (double)p[2], (double)sqrtf(dx * dx + dy * dy), h.life);
    }
    char head[160];
    snprintf(head, sizeof head, "%s done enemies: %u of %u budget entities within %.0f m\n", g_probe.id, within, count, (double)g_probe.r);
    ProbeFinish(head);
}

static void ProbePoll(uintptr_t game)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_probe.checked < 250) return;
    g_probe.checked = now;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(g_probe.in, GetFileExInfoStandard, &a) || CompareFileTime(&a.ftLastWriteTime, &g_probe.stamp) == 0) return;
    g_probe.stamp = a.ftLastWriteTime;
    char text[200] = ""; DWORD got = 0;
    HANDLE f = CreateFileW(g_probe.in, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    ReadFile(f, text, sizeof text - 1, &got, NULL); CloseHandle(f);
    text[got] = 0;
    char cmd[16] = ""; float v[4] = {0, 0, 0, 0.3f};
    int n = sscanf(text, "%31s %15s %f %f %f %f", g_probe.id, cmd, &v[0], &v[1], &v[2], &v[3]);
    g_probe.used = 0;
    if (n < 5) { char h[260]; snprintf(h, sizeof h, "%s done error: \"%.120s\" is not \"ID props|inside|enemies X Y R|H [TOL]\"\n", n > 0 ? g_probe.id : "?", text); ProbeFinish(h); return; }
    g_probe.x = v[0]; g_probe.y = v[1]; g_probe.r = v[2]; g_probe.tol = n >= 6 ? v[3] : 0.3f;
    if (!strcmp(cmd, "enemies")) { ProbeEnemies(game); return; }
    if (!strcmp(cmd, "depth"))
    {
        float at[3] = {v[0], v[1], v[2]}, ground = 0.0f, water = 0.0f;
        uint32_t kind = 0;
        float coarse = g_beaconDepth != NULL ? g_beaconDepth(0, at) : NAN;
        int tile = BeaconDepthParts(at, &ground, &water, &kind);
        char h[360];
        if (tile) snprintf(h, sizeof h, "%s done depth at (%.1f, %.1f, %.1f): %.2f m over a near tile (ground as drawn %.2f, water %.2f); the coarse ray's %.2f m; a beacon is refused past 1 m\n",
            g_probe.id, (double)at[0], (double)at[1], (double)at[2], (double)(water - ground), (double)ground, (double)water, (double)coarse);
        else snprintf(h, sizeof h, "%s done depth at (%.1f, %.1f, %.1f): %.2f m, the game's own (no near tile there, or level +0x1E4 %u, or the water off); drawn ground %.2f\n",
            g_probe.id, (double)at[0], (double)at[1], (double)at[2], (double)coarse, kind, (double)NearVisibleAt(at[0], at[1]));
        ProbeFinish(h);
        return;
    }
    g_probe.kind = !strcmp(cmd, "props") ? 1 : !strcmp(cmd, "inside") ? 2 : !strcmp(cmd, "big") ? 3 : 0;
    if (g_probe.kind == 0) { char h[96]; snprintf(h, sizeof h, "%s done error: unknown command %s\n", g_probe.id, cmd); ProbeFinish(h); return; }
    if (g_probe.kind == 2)
    {
        float g = ProbeGround(v[0], v[1]);
        if (isnan(g)) { char h[96]; snprintf(h, sizeof h, "%s done error: no drawn ground at (%.1f, %.1f)\n", g_probe.id, (double)v[0], (double)v[1]); ProbeFinish(h); return; }
        g_probe.point[0] = v[0]; g_probe.point[1] = v[1]; g_probe.point[2] = g + v[2];
        g_probe.h = v[2]; g_probe.r = 1.0f; /* units whose root is within 200 m (the walk's reach): the largest boxes */
    }
    uintptr_t gameObj = 0;
    if (SafeRead(game + 0x3326340u, &gameObj, 8) || gameObj == 0 || SafeRead(gameObj + 0x10E8, &g_probe.world, 8) || g_probe.world == 0 ||
        SafeRead(g_probe.world + 0x2305B8, &g_probe.count, 4) || g_probe.count > PROBE_UNITS_MAX)
    { char h[96]; snprintf(h, sizeof h, "%s done error: the world's units do not read\n", g_probe.id); ProbeFinish(h); return; }
    if (g_probe.kind == 1) ProbeOthers(game);
    g_probeBoxN = g_probeFloatN = 0;
    g_probe.next = 0; g_probe.scanned = g_probe.skipped = g_probe.hits = g_probe.camera = 0; g_probe.busy = 1;
}

static void ProbeWalk(uintptr_t game)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL), array = 0;
    uint32_t count = 0;
    if (SafeRead(g_probe.world + 0x2305B8, &count, 4) || count > PROBE_UNITS_MAX || SafeRead(g_probe.world + 0x2305C0, &array, 8) || array == 0)
    { char h[96]; snprintf(h, sizeof h, "%s done error: the world's units went away\n", g_probe.id); ProbeFinish(h); return; }
    static uintptr_t chunk[PROBE_PER_FRAME];
    uint32_t end = g_probe.next + PROBE_PER_FRAME < count ? g_probe.next + PROBE_PER_FRAME : count;
    if (end > g_probe.next && !SafeRead(array + (uintptr_t)g_probe.next * 8, chunk, (size_t)(end - g_probe.next) * 8))
        for (uint32_t i = g_probe.next; i < end; i++) ProbeUnitStep(exe, chunk[i - g_probe.next], i);
    g_probe.next = end;
    if (end < count) return;
    uint32_t stacked = 0;
    for (int f = 0; f < g_probeFloatN && g_probe.kind == 1; f++)
    {
        const ProbeUnit* u = &g_probeFloats[f].u;
        int support = -1;
        for (int b = 0; b < g_probeBoxN && support < 0; b++)
        {
            if (g_probeBoxes[b].id == u->id) continue;
            const float* lo = g_probeBoxes[b].lo, *hi = g_probeBoxes[b].hi;
            if (lo[0] < u->hi[0] && hi[0] > u->lo[0] && lo[1] < u->hi[1] && hi[1] > u->lo[1] && lo[2] < u->lo[2] && hi[2] >= u->lo[2] - g_probe.tol) support = b;
        }
        if (support >= 0) stacked++; else g_probe.hits++;
        ProbeAppend("%016llx unit %08X class %d meshes %d root (%.2f, %.2f, %.2f) ground at root %.2f, level %.2f; box z %.2f..%.2f, x %.1f..%.1f, y %.1f..%.1f; "
            "ground under it %.2f..%.2f; gap %+.2f %s%s%s\n", (unsigned long long)u->hash, u->id, u->cls, u->meshes, (double)u->root[0], (double)u->root[1],
            (double)u->root[2], (double)u->groundRoot, (double)g_probeFloats[f].level, (double)u->lo[2], (double)u->hi[2], (double)u->lo[0], (double)u->hi[0],
            (double)u->lo[1], (double)u->hi[1], (double)u->ground[0], (double)u->ground[1], (double)(u->lo[2] - u->ground[1]), support >= 0 ? "STACKED on unit " : "FLOAT",
            "", g_probeFloats[f].onLevel ? " ON-LEVEL" : "");
        if (support >= 0) ProbeAppend("    (supported by unit %08X, its top %.2f)\n", g_probeBoxes[support].id, (double)g_probeBoxes[support].hi[2]);
    }
    char head[320];
    if (g_probe.kind == 3)
        snprintf(head, sizeof head, "%s done big: %u units within %.0f m of (%.1f, %.1f) span more than %.0f m; %u units read, %u skipped\n",
            g_probe.id, g_probe.hits, (double)g_probe.r, (double)g_probe.x, (double)g_probe.y, (double)g_probe.tol, g_probe.scanned, g_probe.skipped);
    else if (g_probe.kind == 2)
        snprintf(head, sizeof head, "%s done inside: point (%.2f, %.2f, %.2f) (%.2f m over the drawn ground): in %u unit boxes; %u units read, %u skipped; the camera's unit %08X left out\n",
            g_probe.id, (double)g_probe.point[0], (double)g_probe.point[1], (double)g_probe.point[2], (double)g_probe.h, g_probe.hits, g_probe.scanned, g_probe.skipped, g_probe.camera);
    else
        snprintf(head, sizeof head, "%s done props: within %.0f m of (%.1f, %.1f), tolerance %.2f m: %u flagged (floating or buried), %u stacked on other units; %u units read, %u skipped; "
            "%d enemy and player units left out, the camera's unit %08X; ground %s\n", g_probe.id, (double)g_probe.r, (double)g_probe.x, (double)g_probe.y, (double)g_probe.tol, g_probe.hits, stacked,
            g_probe.scanned, g_probe.skipped, g_probe.otherCount, g_probe.camera, g_near.state == NEAR_IN ? "the tile and the coarse terrain" : "the coarse terrain (no tile)");
    ProbeFinish(head);
    (void)game;
}

/* No enemies: the patrol caps (every row the director copies) to 0, once; then the sweep near the players. */
static void NoEnemiesTick(uintptr_t game)
{
    if (g_noEnemies.state < 0) return;
    if (g_noEnemies.state == 0)
    {
        char t[4] = "";
        if (!(GetEnvironmentVariableA("HELLFORGE_XL_NO_ENEMIES", t, sizeof t) > 0 && t[0] == '1')) { g_noEnemies.state = -1; return; }
        if (!ApiCodeMatches(game + 0x94FF3Au, "44 8B A5 5C 05 00 00") || !ApiCodeMatches(game + 0x94FF33u, "44 8B AD 64 05 00 00") ||
            !ApiCodeMatches(game + 0x94FEE6u, "44 8B BD 60 05 00 00") || !ApiCodeMatches(game + 0x9513A1u, "44 39 A0 14 06 00 00") ||
            !ApiCodeMatches(game + 0xA2C220u, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 55 41 56 41 57 48 83 EC 30 48 8B 81 88 16 00 00") ||
            !ApiCodeMatches(game + 0xA2EC60u, "44 8B 83 64 16 00 00"))
        { g_noEnemies.state = -1; Note("difficulty: test: no enemies: the director's code is not the reviewed build; off"); return; }
        g_noEnemies.state = 1;
        int strict = ApiCodeMatches(game + 0x953580u, "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 A8 FD FF FF 48 81 EC 58 03 00 00") &&
            ApiCodeMatches(game + 0x957140u, "48 8B C4 48 89 68 10 48 89 70 20 57 48 81 EC 90 00 00 00") &&
            !LoaderHook("test: no squad requests", game + 0x953580u, (void*)NoEnemiesSquad, &g_noEnemySquadOriginal) &&
            !LoaderHook("test: no encounters", game + 0x957140u, (void*)NoEnemiesAdmit, &g_noEnemyAdmitOriginal);
        Note("difficulty: test: no enemies (HELLFORGE_XL_NO_ENEMIES=1): patrol caps 0, enemies within %.0f m of a player removed, squad requests and encounters %s",
            (double)PROBE_CLEAR, strict ? "refused" : "NOT refused (not the reviewed code or not hooked)");
    }
    int host = 0;
    if (!ActMission(game, &host, NULL) || !host) return;
    ULONGLONG now = GetTickCount64();
    if (now < g_noEnemies.next) return;
    g_noEnemies.next = now + 500;
    static const uint32_t caps[3] = {0x2AC, 0x2B0, 0x2B4};
    uint32_t zero = 0, rows = 0;
    for (uint32_t level = 1; level <= DIF_VANILLA + 1; level++)
    {
        int ready = 0;
        uintptr_t row = DifficultyRowOf(level, &ready);
        if (row == 0) continue;
        for (int k = 0; k < 3; k++)
        {
            uint32_t was = 0;
            if (SafeRead(row + caps[k], &was, 4)) continue;
            if (was != 0) { if (!g_noEnemies.savedOk) g_noEnemies.saved[level - 1][k] = was; ApiWrite(row + caps[k], &zero, 4); }
        }
        rows++;
    }
    if (rows != g_noEnemies.rows) { g_noEnemies.rows = rows; Note("difficulty: test: no enemies: the patrol caps of %u difficulty rows are 0 (level 10's were %u/%u/%u)", rows,
        g_noEnemies.saved[9][0], g_noEnemies.saved[9][1], g_noEnemies.saved[9][2]); }
    g_noEnemies.savedOk = 1;
    /* the sweep: budget indexes within PROBE_CLEAR of a player, removed from the highest down (a removal only marks) */
    float at[EVT_MAX_PLAYERS][3]; int players = 0;
    EvpPlayer list[EVT_MAX_PLAYERS];
    int n = EvpPlayers(game, list);
    for (int i = 0; i < n; i++) { ActHealth h; if (list[i].avatar && ActHealthOf(game, list[i].avatar, &h) && h.unit && EvpUnitPosition(h.unit, at[players])) players++; }
    if (players == 0) return;
    uintptr_t budget = 0, entities = 0, records = 0; uint32_t count = 0;
    if (SafeRead(game + 0x3326838u, &budget, 8) || budget == 0 || SafeRead(budget + 0x1664, &count, 4) || count > 4096 ||
        SafeRead(budget + 0x1688, &entities, 8) || entities == 0 || SafeRead(budget + 0x1690, &records, 8) || records == 0) return;
    uint32_t pick[32]; int picked = 0;
    for (uint32_t i = count; i-- > 0 && picked < 32;)
    {
        uintptr_t e = 0; uint32_t entity = 0; uint8_t gone = 0; ActHealth h; float p[3];
        if (SafeRead(entities + (uintptr_t)i * 8, &e, 8) || e == 0 || SafeRead(e + 8, &entity, 4) || SafeRead(records + (uintptr_t)i * 16 + 0xC, &gone, 1) || gone) continue;
        if (!ActHealthOf(game, entity, &h) || !ActEnemy(h.resource) || h.life >= 2 || !h.unit || !EvpUnitPosition(h.unit, p)) continue;
        for (int k = 0; k < players; k++)
        {
            float dx = p[0] - at[k][0], dy = p[1] - at[k][1];
            if (dx * dx + dy * dy <= PROBE_CLEAR * PROBE_CLEAR) { pick[picked++] = i; break; }
        }
    }
    typedef void (*RemoveFn)(uintptr_t budget, uint32_t index);
    for (int k = 0; k < picked; k++) ((RemoveFn)(game + 0xA2C220u))(budget, pick[k]);
    if (picked) { g_noEnemies.removed += (uint32_t)picked; Note("difficulty: test: no enemies: removed %d near the players (%u so far; %ld squad requests and %ld encounters refused)",
        picked, g_noEnemies.removed, g_noEnemySquads, g_noEnemyAdmits); }
}

/* TickDetour (packages.c), every frame. */
static void TerrainProbeTick(void)
{
    uintptr_t game = g_world.game;
    if (game == 0) return;
    NoEnemiesTick(game);
    { static ULONGLONG at; ULONGLONG now = GetTickCount64(); if (now - at >= 5000) { at = now; ScatterOwnerNote(); } }
    if (g_probe.on == 0)
    {
        char t[4] = "";
        g_probe.on = GetEnvironmentVariableA("HELLFORGE_XL_EYE", t, sizeof t) > 0 && t[0] == '1' && g_logs[0] &&
            swprintf(g_probe.in, MAX_PATH, L"%ls\\xl-probe.txt", g_logs) > 0 && swprintf(g_probe.out, MAX_PATH, L"%ls\\xl-probe-out.txt", g_logs) > 0 &&
            swprintf(g_probe.tmp, MAX_PATH, L"%ls\\xl-probe-out.tmp", g_logs) > 0 ? 1 : -1;
        if (g_probe.on == 1)
        {
            uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
            g_probe.pinned = ApiCodeMatches(exe + 0x3EAFAEu, "44 8B B5 B8 05 23 00") && ApiCodeMatches(exe + 0x3EAFD2u, "48 8B 85 C0 05 23 00") &&
                ApiCodeMatches(exe + 0x9EE40u, "48 63 81 98 00 00 00 48 8B 89 90 00 00 00 48 C1 E0 06 48 03 41 28") &&
                ApiCodeMatches(exe + 0x488FC7u, "48 8D 90 A0 00 00 00") && ApiCodeMatches(exe + 0x2BD890u, "8B 81 C8 00 00 00 C3") &&
                ApiCodeMatches(exe + 0x2BD8A0u, "48 8B 81 D0 00 00 00") && ApiCodeMatches(exe + 0x2CAC70u, "48 8B 51 30 8B 42 64 85 C0 75 01 C3 8B 04 10 C3 48 8B 81 B0 00 00 00") &&
                ApiCodeMatches(exe + 0x1A3945u, "0F B6 42 28") && ApiCodeMatches(game + 0x13866F2u, "49 8B 88 E8 10 00 00");
            if (!g_probe.pinned) g_probe.on = -1;
            Note(g_probe.pinned ? "difficulty: test: probe: watching xl-probe.txt in the session's logs" : "difficulty: test: probe: the unit code is not the reviewed build; off");
        }
    }
    if (g_probe.on != 1) return;
    if (g_probe.busy) ProbeWalk(game); else ProbePoll(game);
}
