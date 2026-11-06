/* Extra large maps: the stamp window. The map's stamps are planned at load (difficulty_world_terrain.c, TerrainStamps:
 * every tile of the playable disc at vanilla density, the user's choice of 2026-11-05); the generation's room
 * (DIF_GEN_LOCATIONS) holds the nearest and largest, and the rest, the plan (g_plan, g_stamps.cand), is put into the
 * location table's window (slots from the generation's count up to DIF_LOCATIONS) when a player comes near and taken
 * out again when every player has gone. Static basis (KB xl-operation-content, "the stamp window's enter / leave"):
 *
 * - A live slot is its 0xE0 A record (the B record is read by no op after 0x2B), its stamp unit and its entry in the
 *   level's stamp index (P = [level+0x2C0], I = P+8): the insert 0xC04EF0(I, x0, y0, x1, y1 in map metres, u64* handle)
 *   returns the slot's u32, written as the location id (flags 0); the game never removes entries, 0xC051C0(I, handle)
 *   does (it clears the slot's bits, frees it; no lock). The only mission-time query (0x13F8518 -> .. -> 0xFCB1B0) runs
 *   in the late update, as the content streamer's tick, so both are on the game thread, one after the other.
 * - The stamp unit: the content streamer's respawn path (WorldStreamRespawnLocked: SpawnUnit +0x380 with the
 *   resource [[loc+0x90]+8] and op 0x31's pose, then the set-data call with stamp_id = the slot), its layers through
 *   its staged op 0x6C, its removal (animation wait, +0x388) past kStreamOutRadius. A freed slot is zeroed (+0xA8 = 0)
 *   with no index entry: ShowOnMap's scan (0x677B60), 0xF73E50 and the teardown (0xF742ED) skip it, queries miss it.
 * - Co-op: every peer generates its own stamps from the seed and networked entities match by host ids, not location
 *   indices (0xFDC140), so each peer's window is its own.
 * - Not done here (open): op 0x30's extension layers for window stamps (its id is its loop counter, 0xF480C5, so a staged
 *   run is not equivalent), op 0x55's minimap proxies, op 0x2F's terrains (the moving tiles take the plan's,
 *   difficulty_world_move.c). */

#define WINDOW_IN 550.0f          /* a planned stamp enters when a player is this close (inside kStreamOutRadius) */
#define WINDOW_PER_SCAN 384u      /* entries per scan (0.25 s) at most */
#define WINDOW_BODY_SHARE 0.80f   /* and only while the physics bodies in use stay under this share of the store */
/* Past WINDOW_IN, a stamp enters while a player is within the distance its meshes can be drawn at (WindowVisible), at
 * most WINDOW_FAR, and leaves past that + WINDOW_FAR_LEAVE. The engine's view-distance cull (render config
 * ee6b1ba7e22d71ed, "high": near 350, far 1000, min_size_far 0.05; read 0x1EC20E..0x1EC3BE; kernel 0x57B9B0) draws an
 * object while its angular size > K (min_size + 0.05 t^2), t = clamp((d - 350) / 650), K the camera's field of view
 * (cam+0x2C, ~1.2 rad): a 19 m stamp to ~830 m, a 52 m one to ~1.2 km (the footprint's diagonal as the mesh's size, an
 * upper bound; min_size left out: both make the radius larger, never smaller). So the dressing thins out as the
 * engine's own does, with no edge at WINDOW_IN. Far entries: the largest first, WINDOW_FAR_PER_SCAN a scan, only while
 * the physics bodies and the main world's culling pool 0 (RenderPool0) stay under WINDOW_FAR_SHARE (a full pool 0 is
 * the crash 0x58E14B); a far stamp costs ~10 (open) to ~24 (city) bodies and ~21 pool 0 entries (s78a/s80a/s80b,
 * static farstamps). */
#define WINDOW_FAR 1500.0f
#define WINDOW_FAR_LEAVE 150.0f
#define WINDOW_FAR_PER_SCAN 128u
#define WINDOW_FAR_SHARE 0.70f
#define WINDOW_FAR_BIG 1000.0f    /* the stamps visible past this enter in a first pass */
#define WINDOW_INSERT 0xC04EF0u
#define WINDOW_REMOVE 0xC051C0u
static const char kWindowInsertPin[] = "41 54 41 56 48 83 EC 48 44 0F B7 49 06 4C 8B D9 44 8B 61 2C 66 41 F7 D1";
static const char kWindowRemovePin[] = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 41 57";

typedef uint32_t* (*WindowInsertFn)(uintptr_t index, float x0, float y0, float x1, float y1, uint64_t* handle);
typedef void (*WindowRemoveFn)(uintptr_t index, uint64_t handle);

static struct
{
    uintptr_t level, gen, P;           /* the level, its generation block and stamp index the window belongs to */
    uint32_t base, high;               /* the first window slot (the generation's count) and one past the highest used */
    uint32_t freeCount, freeList[DIF_LOCATIONS];
    uint32_t plan[DIF_LOCATIONS];      /* slot -> plan entry */
    uint64_t handle[DIF_LOCATIONS];    /* slot -> its index handle */
    uint16_t* reach;                   /* plan entry -> the distance it enters within (m); kept across levels (not reset) */
    uint32_t reachRoom;                /* reach's room */
    uint32_t entered, left, refusedFull, refusedIndex, refusedBodies, poseChecked, poseEqual, enteredFar, farHeld;
    uint32_t reachFar;                 /* the plan entries whose reach is past WINDOW_IN */
    int on, bad, insertOk, removeOk;
    ULONGLONG nextScan;
} g_win;

/* Op 0x31's pose for a location (0xF449E1..0xF44BF4, transcribed: its own sine and cosine from fmodf, normalised; rows
 * of the 4x4 at rbp-0x60, the translation (x - W/2, y - W/2, 0)). Every operation in its order, in float, so the pose is
 * the game's to the bit (checked against the generator's own units at capture: WindowPoseCheck). */
#define WF(bits) (((union { uint32_t u; float f; }){.u = (bits)}).f)
static void WindowPose(const uint8_t* loc, float worldW, float* m)
{
#pragma STDC FP_CONTRACT OFF
    const float zero = 0.0f, twoPi = WF(0x40C90FDBu), pi = WF(0x40490FDBu), one = 1.0f, minusPi = WF(0xC0490FDBu), minusOne = -1.0f;
    const float four = 4.0f, halfPi = WF(0x3FC90FDBu), sixteen = 16.0f, fivePi2 = WF(0x42456461u), pi2 = WF(0x411DE9E7u), tiny = WF(0x38D1B717u);
    float xy[2], a;
    memcpy(xy, loc, 8); memcpy(&a, loc + 8, 4);
    const float half = worldW * 0.5f;
    float x0 = fmodf(a + zero, twoPi), sign = one;
    if (zero > x0) x0 = x0 + twoPi;
    if (!(x0 < pi)) { x0 = x0 + minusPi; sign = minusOne; }
    float x7 = a + halfPi;
    x0 = x0 - zero;
    float x1 = pi - x0;
    float x6 = x0 * sixteen;
    x0 = x0 * four;
    x6 = x6 * x1;
    x0 = x0 * x1;
    x1 = fivePi2 - x0;
    x6 = x6 / x1;
    x6 = x6 * sign;                      /* the sine */
    float c = fmodf(x7, twoPi), sign2 = one;
    if (zero > c) c = c + twoPi;
    if (!(c < pi)) { c = c + minusPi; sign2 = minusOne; }
    c = c - halfPi;
    c = c * c;
    float c4 = c;
    c = c + pi2;
    c4 = c4 * four;
    x7 = pi2 - c4;
    float s2 = x6 * x6;
    x7 = x7 / c;
    x7 = x7 * sign2;                     /* the cosine */
    float len2 = x7 * x7;
    len2 = len2 + s2;
    len2 = len2 + zero;
    float len = sqrtf(len2), z = zero;
    if (tiny > len) { x7 = zero; x6 = zero; }
    else { x7 = x7 / len; x6 = x6 / len; z = zero / len; }
    float z0 = z * zero;
    m[0] = x6 - z0; m[1] = z0 - x7; m[2] = x7 * zero - x6 * zero; m[3] = 0.0f;
    m[4] = x7; m[5] = x6; m[6] = z; m[7] = 0.0f;
    m[8] = zero; m[9] = zero; m[10] = 1.0f; m[11] = 0.0f;
    m[12] = xy[0] - half; m[13] = xy[1] - half; m[14] = 0.0f; m[15] = 1.0f;
}

/* At capture (the generator's records are read, their units are the generator's): the transcription against them. */
static void WindowPoseCheck(const uint8_t* array, uint32_t count, float worldW)
{
    uint32_t checked = 0, equal = 0, first = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < count && checked < 4096; i++)
    {
        if (!g_streamPoseGood[i]) continue;
        uint8_t loc[0xE0];
        if (SafeRead((uintptr_t)array + (size_t)i * kStreamRecord, loc, sizeof loc)) continue;
        float m[16];
        WindowPose(loc, worldW, m);
        checked++;
        if (memcmp(m, g_streamPose[i], sizeof m) == 0) equal++;
        else if (first == 0xFFFFFFFFu) first = i;
    }
    g_win.poseChecked = checked; g_win.poseEqual = equal;
    if (first != 0xFFFFFFFFu)
    {
        float m[16]; uint8_t loc[0xE0];
        SafeRead((uintptr_t)array + (size_t)first * kStreamRecord, loc, sizeof loc);
        WindowPose(loc, worldW, m);
        const float* g = g_streamPose[first];
        Note("difficulty: extra large maps: stamp window: op 0x31's pose, transcribed: %u of %u equal to the bit; first other (location %u): mine %.7g %.7g %.7g / %.7g %.7g %.7g / %.7g %.7g, the game's %.7g %.7g %.7g / %.7g %.7g %.7g / %.7g %.7g",
            equal, checked, first, (double)m[0], (double)m[1], (double)m[2], (double)m[4], (double)m[5], (double)m[6], (double)m[12], (double)m[13],
            (double)g[0], (double)g[1], (double)g[2], (double)g[4], (double)g[5], (double)g[6], (double)g[12], (double)g[13]);
    }
    else Note("difficulty: extra large maps: stamp window: op 0x31's pose, transcribed: %u of %u equal to the bit", equal, checked);
}

/* The window for the captured level: its slots from the generation's count. Called with g_streamLock held, once the
 * generator's op is done. 0 when there is no plan or the calls are not the reviewed ones. */
/* The largest distance at which an object of this size (m) passes the view-distance cull ("high", K 1.2): D/d > K *
 * 0.05 * t^2 (bisection; the test only gets harder with distance). */
static float WindowVisible(float size)
{
    float lo = 0.0f, hi = 2500.0f;
    for (int i = 0; i < 24; i++)
    {
        float d = 0.5f * (lo + hi), t = (d - 350.0f) / 650.0f;
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        if (d <= 0.0f || size / d > 1.2f * 0.05f * t * t) lo = d; else hi = d;
    }
    return lo;
}
static int WindowReadyLocked(void)
{
    if (g_win.level == g_stream.level && g_win.level != 0) return g_win.on;
    memset(&g_win.entered, 0, sizeof g_win - offsetof(__typeof__(g_win), entered));
    g_win.level = g_stream.level; g_win.on = 0; g_win.bad = 0; g_win.nextScan = 0;
    g_win.freeCount = 0; g_win.high = 0; g_win.base = 0;
    if (g_plan.level != g_world.levels || g_plan.count == 0 || g_plan.slot == NULL) { Note("difficulty: extra large maps: stamp window: no plan for this level"); return 0; }
    if (g_win.insertOk == 0) g_win.insertOk = ApiCodeMatches(g_stream.game + WINDOW_INSERT, kWindowInsertPin) == 1 ? 1 : -1;
    if (g_win.removeOk == 0) g_win.removeOk = ApiCodeMatches(g_stream.game + WINDOW_REMOVE, kWindowRemovePin) == 1 ? 1 : -1;
    if (g_win.insertOk != 1 || g_win.removeOk != 1)
    { Note("difficulty: extra large maps: stamp window off: the stamp index's insert (0x%X) or remove (0x%X) is not the reviewed one", WINDOW_INSERT, WINDOW_REMOVE); return 0; }
    uintptr_t gen = 0, P = 0;
    if (SafeRead(g_stream.level + 0x2C8, &gen, 8) || gen == 0 || SafeRead(g_stream.level + 0x2C0, &P, 8) || P == 0 ||
        (uintptr_t)g_stream.array != gen + 0x11A7280u)
    { Note("difficulty: extra large maps: stamp window off: the level's block or stamp index does not read, or the streamer's records are not the level's table"); return 0; }
    g_win.gen = gen; g_win.P = P;
    g_win.base = g_win.high = g_stream.count;
    if (g_win.base >= DIF_LOCATIONS) { Note("difficulty: extra large maps: stamp window off: the generation filled the table (%u)", g_win.base); return 0; }
    for (uint32_t s = DIF_LOCATIONS; s > g_win.base; s--) g_win.freeList[g_win.freeCount++] = s - 1; /* the lowest pops first */
    memset(g_plan.slot, 0, (size_t)g_plan.count * 2);
    if (g_win.reachRoom < g_plan.count)
    {
        if (g_win.reach != NULL) VirtualFree(g_win.reach, 0, MEM_RELEASE);
        g_win.reach = (uint16_t*)VirtualAlloc(NULL, (size_t)g_plan.count * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_win.reachRoom = g_win.reach != NULL ? g_plan.count : 0;
    }
    g_win.reachFar = 0;
    if (g_win.reach != NULL)
        for (uint32_t e = 0; e < g_plan.count; e++)
        {
            float box[4]; memcpy(box, g_stamps.cand[e].loc + 0xB0, 16);
            float side = fmaxf(fabsf(box[2] - box[0]), fabsf(box[3] - box[1]));
            float r = isfinite(side) ? WindowVisible(side * 1.41421356f) : WINDOW_IN;
            r = r < WINDOW_IN ? WINDOW_IN : r > WINDOW_FAR ? WINDOW_FAR : r;
            g_win.reach[e] = (uint16_t)r;
            if (r > WINDOW_IN) g_win.reachFar++;
        }
    WindowPoseCheck(g_stream.array, g_stream.count, 2.0f * g_stream.half);
    g_win.on = 1;
    /* The index's free entries (I = P + 8: head +0x2C, -1 = none; entries [+0x30], stride u16 +0x24, a free entry's u32 the
     * next; 0xC04B80 / 0xC04F1C) and its size (+0x1C). */
    uint32_t head = 0, slots = 0, freeEntries = 0; uint16_t stride = 0; uint64_t entries = 0;
    if (!SafeRead(P + 8 + 0x2C, &head, 4) && !SafeRead(P + 8 + 0x24, &stride, 2) && !SafeRead(P + 8 + 0x30, &entries, 8) && !SafeRead(P + 8 + 0x1C, &slots, 4))
        while (head != 0xFFFFFFFFu && freeEntries <= slots && entries != 0 && !SafeRead(entries + (uint64_t)head * stride, &head, 4)) freeEntries++;
    Note("difficulty: extra large maps: stamp window: %u planned stamps, slots %u..%u (%u), entered within %.0f m of a player (%u a scan at most) while the physics bodies stay under %.0f %% of the store; the stamp index has %u of %u entries free",
        g_plan.count, g_win.base, DIF_LOCATIONS - 1, g_win.freeCount, (double)WINDOW_IN, WINDOW_PER_SCAN, (double)(WINDOW_BODY_SHARE * 100.0f), freeEntries, slots);
    Note("difficulty: extra large maps: stamp window: %u of the planned stamps can be seen past %.0f m: they enter within that distance (%.0f m at most, %u a scan, the largest first) while the bodies and the culling pool 0 stay under %.0f %%%s",
        g_win.reachFar, (double)WINDOW_IN, (double)WINDOW_FAR, WINDOW_FAR_PER_SCAN, (double)(WINDOW_FAR_SHARE * 100.0f),
        g_win.reach == NULL ? " (no memory for the distances: off)" : g_renderCap.on != 1 ? " (the culling pool is not read: off)" : "");
    return 1;
}

/* One planned stamp into a free slot. */
static int WindowEnterLocked(uint32_t e, float leave)
{
    if (g_win.freeCount == 0) { g_win.refusedFull++; return 0; }
    const StampCandidate* c = &g_stamps.cand[e];
    uint32_t s = g_win.freeList[g_win.freeCount - 1];
    uint8_t loc[0xE0];
    memcpy(loc, c->loc, sizeof loc);
    memset(loc + 0xA8, 0, 8);
    uint64_t holder = 0, resource = 0;
    memcpy(&holder, loc + DIF_STREAM_LOC_RESOURCE, 8);
    if (holder == 0 || SafeRead((uintptr_t)holder + 8, &resource, 8) || resource == 0) { g_plan.slot[e] = 0xFFFF; return 0; } /* never again */
    float box[4]; memcpy(box, loc + 0xB0, 16);
    uint64_t handle = 0;
    uint32_t* entry = ((WindowInsertFn)(g_stream.game + WINDOW_INSERT))(g_win.P + 8, fminf(box[0], box[2]), fminf(box[1], box[3]), fmaxf(box[0], box[2]),
        fmaxf(box[1], box[3]), &handle);
    if (entry == NULL || handle == 0) { g_win.refusedIndex++; return 0; }
    *entry = s; /* the location id, flags 0 */
    g_win.freeCount--;
    memcpy((void*)((uintptr_t)g_stream.array + (size_t)s * kStreamRecord), loc, sizeof loc);
    g_win.plan[s] = e; g_win.handle[s] = handle; g_plan.slot[e] = (uint16_t)s;
    float xy[2]; memcpy(xy, loc, 8);
    g_streamAt[s][0] = xy[0] - g_stream.half; g_streamAt[s][1] = xy[1] - g_stream.half;
    g_streamResource[s] = resource;
    WindowPose(loc, 2.0f * g_stream.half, g_streamPose[s]);
    g_streamUnit[s] = 0;
    g_streamPoseGood[s] = 1; g_streamDeferred[s] = 1; g_streamNeedsUnit[s] = 1; g_streamDetached[s] = 1; g_streamDue[s] = 1;
    g_streamSpawned[s] = 0; g_streamLive[s] = 0; g_streamOutSince[s] = 0; g_streamFormation[s] = 0; g_streamMission[s] = 0;
    g_streamOut2[s] = leave * leave;
    if (s + 1 > g_win.high)
    {
        g_win.high = s + 1;
        g_stream.count = g_win.high;
        *(volatile uint32_t*)(g_win.gen + 0x11A7108u) = g_win.high; /* the teardown's and ShowOnMap's scans reach it (free slots are zeroed) */
        *(volatile uint32_t*)(g_win.gen + 0x11A710Cu) = g_win.high;
    }
    g_stream.deferred++;
    g_win.entered++;
    return 1;
}

/* A window slot the streamer has just removed (its unit destroyed): out of the index, zeroed, free. */
static void WindowLeaveLocked(uint32_t s)
{
    if (!g_win.on || s < g_win.base || s >= DIF_LOCATIONS || g_win.handle[s] == 0) return;
    ((WindowRemoveFn)(g_stream.game + WINDOW_REMOVE))(g_win.P + 8, g_win.handle[s]);
    g_win.handle[s] = 0;
    memset((void*)((uintptr_t)g_stream.array + (size_t)s * kStreamRecord), 0, kStreamRecord);
    uint32_t e = g_win.plan[s];
    if (e < g_plan.count && g_plan.slot[e] == s) g_plan.slot[e] = 0;
    g_streamDeferred[s] = 0; g_streamPoseGood[s] = 0; g_streamNeedsUnit[s] = 0; g_streamDue[s] = 0; g_streamResource[s] = 0; g_streamOut2[s] = 0.0f;
    if (g_stream.deferred > 0) g_stream.deferred--;
    g_win.freeList[g_win.freeCount++] = s;
    g_win.left++;
}

/* Every 0.25 s, after the streamer's scan (the players are fresh): the planned stamps within WINDOW_IN of a player, from
 * the player's bucket outward, into the window while it has slots, the index has room and the bodies stay under budget. */
/* The far ring: the planned stamps between WINDOW_IN and their own reach of a player, the ones visible past
 * WINDOW_FAR_BIG first, under the far budgets. */
static void WindowScanFarLocked(uint32_t bodies, uint32_t slots)
{
    uint32_t used = 0, capacity = 0;
    if (g_win.reach == NULL || g_win.reachFar == 0 || g_win.freeCount == 0) return;
    if ((float)bodies >= WINDOW_FAR_SHARE * (float)slots || !RenderPool0(&used, &capacity) || (float)used >= WINDOW_FAR_SHARE * (float)capacity)
    { g_win.farHeld++; return; }
    const float cell = g_plan.cell, near2 = WINDOW_IN * WINDOW_IN;
    const int reach = (int)ceilf(WINDOW_FAR / cell);
    uint32_t done = 0;
    for (int pass = 0; pass < 2 && done < WINDOW_FAR_PER_SCAN; pass++)
        for (int p = 0; p < g_stream.playerCount && done < WINDOW_FAR_PER_SCAN; p++)
        {
            float px = g_streamPlayerAt[p][0] + g_stream.half, py = g_streamPlayerAt[p][1] + g_stream.half;
            int ci = (int)floorf(px / cell), cj = (int)floorf(py / cell);
            for (int j = cj - reach; j <= cj + reach && done < WINDOW_FAR_PER_SCAN; j++)
                for (int i = ci - reach; i <= ci + reach && done < WINDOW_FAR_PER_SCAN; i++)
                {
                    if (i < 0 || j < 0 || i >= STAMP_PLAN_GRID || j >= STAMP_PLAN_GRID) continue;
                    int b = j * STAMP_PLAN_GRID + i;
                    for (uint32_t e = g_plan.start[b]; e < g_plan.start[b + 1] && done < WINDOW_FAR_PER_SCAN; e++)
                    {
                        float r = (float)g_win.reach[e];
                        if (g_plan.slot[e] != 0 || r <= WINDOW_IN || (pass == 0) != (r > WINDOW_FAR_BIG)) continue;
                        float xy[2]; memcpy(xy, g_stamps.cand[e].loc, 8);
                        float dx = xy[0] - px, dy = xy[1] - py, d2 = dx * dx + dy * dy;
                        if (d2 <= near2 || d2 > r * r) continue;
                        if (!WindowEnterLocked(e, r + WINDOW_FAR_LEAVE)) { if (g_win.freeCount == 0 || g_win.refusedIndex) return; continue; }
                        g_win.enteredFar++;
                        done++;
                    }
                }
        }
}

static void WindowScanLocked(ULONGLONG now)
{
    if (now < g_win.nextScan || !WindowReadyLocked()) return;
    g_win.nextScan = now + 250;
    uint32_t bodies = 0, slots = 0;
    if (!WorldStreamBodyStore(&bodies, &slots) || (float)bodies >= WINDOW_BODY_SHARE * (float)slots) { g_win.refusedBodies++; return; }
    const float cell = g_plan.cell, r2 = WINDOW_IN * WINDOW_IN;
    const int reach = (int)ceilf(WINDOW_IN / cell);
    uint32_t done = 0;
    for (int p = 0; p < g_stream.playerCount && done < WINDOW_PER_SCAN; p++)
    {
        float px = g_streamPlayerAt[p][0] + g_stream.half, py = g_streamPlayerAt[p][1] + g_stream.half; /* map metres */
        int ci = (int)floorf(px / cell), cj = (int)floorf(py / cell);
        for (int ring = 0; ring <= reach && done < WINDOW_PER_SCAN; ring++)
            for (int dj = -ring; dj <= ring && done < WINDOW_PER_SCAN; dj++)
                for (int di = -ring; di <= ring && done < WINDOW_PER_SCAN; di++)
                {
                    if (abs(di) != ring && abs(dj) != ring) continue; /* the ring's border only */
                    int i = ci + di, j = cj + dj;
                    if (i < 0 || j < 0 || i >= STAMP_PLAN_GRID || j >= STAMP_PLAN_GRID) continue;
                    int b = j * STAMP_PLAN_GRID + i;
                    for (uint32_t e = g_plan.start[b]; e < g_plan.start[b + 1] && done < WINDOW_PER_SCAN; e++)
                    {
                        if (g_plan.slot[e] != 0) continue;
                        float xy[2]; memcpy(xy, g_stamps.cand[e].loc, 8);
                        float dx = xy[0] - px, dy = xy[1] - py;
                        if (dx * dx + dy * dy > r2) continue;
                        if (!WindowEnterLocked(e, 0.0f)) { if (g_win.freeCount == 0 || g_win.refusedIndex) return; continue; }
                        done++;
                    }
                }
    }
    WindowScanFarLocked(bodies, slots);
}

static void WindowNoteLocked(void)
{
    if (!g_win.on) return;
    uint32_t used = 0, capacity = 0;
    int pool = RenderPool0(&used, &capacity);
    Note("difficulty: extra large maps: stamp window: %u in (slots used %u of %u, highest %u), %u entered (%u of them far), %u left so far; refused: %u table full, %u index full, %u scans over the body budget, %u far scans held (bodies %u of %u, culling pool 0 %u of %u%s)",
        (DIF_LOCATIONS - g_win.base) - g_win.freeCount, (DIF_LOCATIONS - g_win.base) - g_win.freeCount, DIF_LOCATIONS - g_win.base, g_win.high, g_win.entered,
        g_win.enteredFar, g_win.left, g_win.refusedFull, g_win.refusedIndex, g_win.refusedBodies, g_win.farHeld, g_stream.bodiesUsed, g_stream.bodySlots,
        used, capacity, pool ? "" : ", not read");
}
