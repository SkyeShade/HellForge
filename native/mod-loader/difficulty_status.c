/* The status-effect volumes' unit registry on larger maps. Included by hellforge_modloader.c after difficulty_maps.c.
 *
 * M = [game + 0x347CF20] (made once at boot, 0x13F2CF6; a sub-object of the gameplay world) maps every flagged unit of
 * every loaded level to its status-effect setting: at mission start (game state 4's enter, 0xAC5D40 -> 0xACF710) each
 * unit is appended to an inline list (M+0x129098, 4096 x {u32 id, u32 setting}, count M+0x131098, no check) and put in
 * an open-addressing hash (ptr M+0x129080 -> inline M+0x121080, cap M+0x129088 = 0x1000, power of two, every reader
 * builds the mask from it at run time); a full hash leaves r8 0 and the insert writes through it (0xACF961 -> 0xACF966).
 * The hit path reads it (0x69C05A; a unit not in it has no status effect on hit) and the destruction callback removes
 * (0x13CC4D0: the last list entry into the hole, 0x13CC56F). An operation's three cities passed 4096 units and the
 * game ended 50 s into the mission (s74h). Scratch m347/m347.md (subagent; sites and bytes re-checked by Claude).
 *
 * Larger maps: the hash gets STATUS_HASH places (a zeroed table, installed while the count is 0: pointer, then cap, as
 * readers load the cap first), the list STATUS_LIST places in loader memory through stubs at its five accesses, and the
 * append checks the list's room (0xACF8F0; full: the unit is skipped, 0xACFA48, the loop's next item, with its
 * destruction callback, which is what removes list entries) and the insert a null slot (0xACF966: the entry taken back,
 * skipped). STATUS_LIST stays under the destruction-callback map in the same loop ([0x346D528]+0x52AF88, 0x8000 slots,
 * its own unchecked insert). Skips are counted and noted. */
#define STATUS_MANAGER 0x347CF20u
#define STATUS_LIST 24576u
#define STATUS_HASH 65536u

static const struct { uint32_t site; uint8_t length; uint8_t pin[18]; } kStatusSites[5] = {
    {0xACF8F0u, 11, {0x45, 0x8B, 0xB8, 0x98, 0x10, 0x13, 0x00, 0x41, 0x8D, 0x47, 0x01}},                       /* count; lea eax, [r15+1] */
    {0xACF907u, 16, {0x43, 0x89, 0x84, 0xF8, 0x9C, 0x90, 0x12, 0x00, 0x43, 0x89, 0x9C, 0xF8, 0x98, 0x90, 0x12, 0x00}}, /* the append */
    {0xACF966u, 6, {0x41, 0x89, 0x18, 0x48, 0x8B, 0xCF}},                                                     /* the insert's slot write */
    {0x69C05Au, 8, {0x41, 0x8B, 0x8C, 0xC1, 0x9C, 0x90, 0x12, 0x00}},                                         /* the hit path's read */
    {0x13CC56Fu, 18, {0x4C, 0x8B, 0x94, 0xC6, 0x98, 0x90, 0x12, 0x00, 0x33, 0xD2, 0x4C, 0x89, 0x94, 0xFE, 0x98, 0x90, 0x12, 0x00}}}; /* the removal's move */

static uint32_t StatusSkips(void);
static uint8_t* g_statusStubs;      /* stubs at +0x00, +0x40, +0x80, +0xC0, +0x100; data at +0x180: cap, skips, list */
static uint64_t* g_statusHash;
static volatile LONG g_statusState; /* 0 not yet, 1 code in, 2 hash in too, -1 off */

static void StatusRel(uint8_t* at, uintptr_t to) { int32_t rel = (int32_t)((intptr_t)to - (intptr_t)(at + 4)); memcpy(at, &rel, 4); }

static const char* StatusCodeInstall(uintptr_t game)
{
    for (int i = 0; i < 5; i++)
    {
        uint8_t bytes[18];
        if (SafeRead(game + kStatusSites[i].site, bytes, kStatusSites[i].length) || memcmp(bytes, kStatusSites[i].pin, kStatusSites[i].length) != 0)
            return "the status-effect volumes' unit registry is not the reviewed one: an extra large map keeps its 4096 units";
    }
    uint8_t* s = AllocateNearGame(game, 0x200);
    uint64_t* list = (uint64_t*)VirtualAlloc(NULL, STATUS_LIST * 8u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (s == NULL || list == NULL) return "no memory for the status-effect volumes' unit registry";
    memset(s, 0xCC, 0x200);
    uint8_t* data = s + 0x180;
    uint32_t cap = STATUS_LIST, skips = 0;
    memcpy(data, &cap, 4); memcpy(data + 4, &skips, 4); memcpy(data + 8, &list, 8);
    /* S1 (0xACF8F0): mov r15d, [r8+0x131098]; cmp r15d, [cap]; jae +9; lea eax, [r15+1]; jmp 0xACF8FB; lock inc [skips]; jmp 0xACFA48 */
    uint8_t* p = s;
    static const uint8_t s1a[] = {0x45, 0x8B, 0xB8, 0x98, 0x10, 0x13, 0x00, 0x44, 0x3B, 0x3D};
    memcpy(p, s1a, sizeof s1a); StatusRel(p + 10, (uintptr_t)data);
    static const uint8_t s1b[] = {0x73, 0x09, 0x41, 0x8D, 0x47, 0x01, 0xE9};
    memcpy(p + 14, s1b, sizeof s1b); StatusRel(p + 21, game + 0xACF8FBu);
    static const uint8_t s1c[] = {0xF0, 0xFF, 0x05};
    memcpy(p + 25, s1c, 3); StatusRel(p + 28, (uintptr_t)(data + 4));
    p[32] = 0xE9; StatusRel(p + 33, game + 0xACFA48u);
    /* S2 (0xACF907): mov r9, [list]; mov [r9+r15*8+4], eax; mov [r9+r15*8], ebx; jmp 0xACF917 */
    p = s + 0x40;
    static const uint8_t s2a[] = {0x4C, 0x8B, 0x0D};
    memcpy(p, s2a, 3); StatusRel(p + 3, (uintptr_t)(data + 8));
    static const uint8_t s2b[] = {0x43, 0x89, 0x44, 0xF9, 0x04, 0x43, 0x89, 0x1C, 0xF9, 0xE9};
    memcpy(p + 7, s2b, sizeof s2b); StatusRel(p + 17, game + 0xACF917u);
    /* S3 (0xACF966): test r8, r8; jz +11; mov [r8], ebx; mov rcx, rdi; jmp 0xACF96C;
     *                mov rax, game+0x347CF20; mov rax, [rax]; dec dword [rax+0x131098]; lock inc [skips]; jmp 0xACFA48 */
    p = s + 0x80;
    static const uint8_t s3a[] = {0x4D, 0x85, 0xC0, 0x74, 0x0B, 0x41, 0x89, 0x18, 0x48, 0x8B, 0xCF, 0xE9};
    memcpy(p, s3a, sizeof s3a); StatusRel(p + 12, game + 0xACF96Cu);
    uint64_t manager = game + STATUS_MANAGER;
    p[16] = 0x48; p[17] = 0xB8; memcpy(p + 18, &manager, 8);
    static const uint8_t s3b[] = {0x48, 0x8B, 0x00, 0xFF, 0x88, 0x98, 0x10, 0x13, 0x00, 0xF0, 0xFF, 0x05};
    memcpy(p + 26, s3b, sizeof s3b); StatusRel(p + 38, (uintptr_t)(data + 4));
    p[42] = 0xE9; StatusRel(p + 43, game + 0xACFA48u);
    /* S4 (0x69C05A): mov rcx, [list]; mov ecx, [rcx+rax*8+4]; jmp 0x69C062 */
    p = s + 0xC0;
    static const uint8_t s4a[] = {0x48, 0x8B, 0x0D};
    memcpy(p, s4a, 3); StatusRel(p + 3, (uintptr_t)(data + 8));
    static const uint8_t s4b[] = {0x8B, 0x4C, 0xC1, 0x04, 0xE9};
    memcpy(p + 7, s4b, sizeof s4b); StatusRel(p + 12, game + 0x69C062u);
    /* S5 (0x13CC56F): mov r9, [list]; mov r10, [r9+rax*8]; xor edx, edx; mov [r9+rdi*8], r10; jmp 0x13CC581 */
    p = s + 0x100;
    static const uint8_t s5a[] = {0x4C, 0x8B, 0x0D};
    memcpy(p, s5a, 3); StatusRel(p + 3, (uintptr_t)(data + 8));
    static const uint8_t s5b[] = {0x4D, 0x8B, 0x14, 0xC1, 0x33, 0xD2, 0x4D, 0x89, 0x14, 0xF9, 0xE9};
    memcpy(p + 7, s5b, sizeof s5b); StatusRel(p + 18, game + 0x13CC581u);
    DWORD old;
    VirtualProtect(s, 0x200, PAGE_EXECUTE_READWRITE, &old); /* +0x180: the skips count, written by the stubs */
    FlushInstructionCache(GetCurrentProcess(), s, 0x200);
    g_statusStubs = s;
    static const uint16_t at[5] = {0x00, 0x40, 0x80, 0xC0, 0x100};
    for (int i = 0; i < 5; i++)
    {
        uint8_t jump[18];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        int32_t rel = DifficultyJumpRel(game + kStatusSites[i].site, (uintptr_t)(s + at[i])); memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + kStatusSites[i].site, jump, kStatusSites[i].length)) return "the status-effect volumes' unit registry could not be enlarged";
    }
    return NULL;
}

/* Polled (DifficultyWatch): the code once, then the hash once the manager exists with its inline table and no units. */
static void StatusWatch(uintptr_t game)
{
    if (g_statusState == 2)
    {
        static uint32_t noted;
        uint32_t skips = StatusSkips();
        if (skips != noted) { noted = skips; Note("difficulty: the status-effect volumes' unit registry skipped %u units (full)", skips); }
        return;
    }
    if (g_statusState < 0 || !LargerMaps()) return;
    if (g_statusState == 0)
    {
        const char* why = StatusCodeInstall(game);
        if (why != NULL) { g_statusState = -1; Note("difficulty: %s", why); return; }
        g_statusState = 1;
    }
    uint64_t m = 0, ptr = 0; uint32_t count = 1, cap = 0;
    if (SafeRead(game + STATUS_MANAGER, &m, 8) || m == 0 || SafeRead(m + 0x129080, &ptr, 8) || SafeRead(m + 0x129088, &cap, 4) ||
        SafeRead(m + 0x131098, &count, 4) || count != 0 || ptr != m + 0x121080 || cap != 0x1000) return;
    if (g_statusHash == NULL) g_statusHash = (uint64_t*)VirtualAlloc(NULL, STATUS_HASH * 8u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_statusHash == NULL) { g_statusState = -1; return; }
    *(volatile uint64_t*)(uintptr_t)(m + 0x129080) = (uint64_t)(uintptr_t)g_statusHash;
    MemoryBarrier();
    *(volatile uint32_t*)(uintptr_t)(m + 0x129088) = STATUS_HASH;
    g_statusState = 2;
    Note("difficulty: the status-effect volumes' unit registry holds %u units (list) in a hash of %u (was 4096)", STATUS_LIST, STATUS_HASH);
}

/* The units skipped so far (a full list or hash), for the notes. */
static uint32_t StatusSkips(void)
{
    uint32_t skips = 0;
    if (g_statusStubs != NULL) memcpy(&skips, g_statusStubs + 0x184, 4);
    return skips;
}

/* Research (2026-11-04, s74h/s74i): on a city map FinalizeVoronoi's structured weld (0xFBBB20, from 0xF6810F) fails and
 * the level restarts. Which of its three failing calls fails, and with what (the first 24 notes):
 *   0xFBBB20(rcx ctx, r8 edges {count, cap, ptr: route edges 0x48}, r9 nodes {count, cap, ptr: route nodes 0x98});
 *     the world is [[ctx+8]+0x2C] (896 m in a terrain tile, the level's otherwise).
 *   0xFB9D90(rcx weld, rdx edges, r8d count): the edges' vertices into the 24 x 24 buckets.
 *   0xFB9B10(rcx weld, rdx vertex: x, y at +0, r8 out): one merged vertex; bucket = min(y/W*23, 23)*24 + min(x/W*23, 23)
 *     over W = [weld+0x1238] (float), each bucket's block {count, vertices}.
 *   0xFBB180(rcx weld, r8 edges, r9 nodes): each edge's endpoints through 0xFBAFF0 into the nodes (count reset to 0). */
typedef uint64_t (*WeldFn4)(uint64_t, uint64_t, uint64_t, uint64_t);
static WeldFn4 g_weldFinal, g_weldCells, g_weldVertex, g_weldEdges;
static volatile LONG g_weldNotes;
static float WeldSide(uint64_t weld) { float w = 0; SafeRead(weld + 0x1238, &w, 4); return w; }
static uint64_t WeldFinal(uint64_t ctx, uint64_t b, uint64_t edges, uint64_t nodes)
{
    uint32_t e[2] = {0, 0}, n[2] = {0, 0}, w = 0; uint64_t s = 0;
    SafeRead(edges, e, 8); SafeRead(nodes, n, 8);
    if (!SafeRead(ctx + 8, &s, 8) && s != 0) SafeRead(s + 0x2C, &w, 4);
    uint64_t r = g_weldFinal(ctx, b, edges, nodes);
    if ((uint8_t)r != 0) return r;
    TileGuard* guard = TileGuardSelf();
    int tile = guard->thread != 0 && guard->thread == GetCurrentThreadId();
    if (InterlockedIncrement(&g_weldNotes) <= 24)
        Note("difficulty: weld research: FinalizeVoronoi's weld failed (%s, world %u m): edges %u (cap %u), nodes %u before (cap %u)",
            tile ? "a terrain tile: abandoned, no level restart" : "the level", w, e[0], e[1], n[0], n[1]);
    /* a tile's failure: done for 0xF68114 (it writes the counts back into the tile's own copy of the block), the tile's
     * runner abandons the tile (its guard's weldFailed) */
    if (tile) { InterlockedIncrement(&guard->weldFailed); return (r & ~0xFFull) | 1u; }
    return r;
}
static uint64_t WeldCells(uint64_t weld, uint64_t list, uint64_t count, uint64_t d)
{
    uint64_t r = g_weldCells(weld, list, count, d);
    if ((uint8_t)r == 0 && InterlockedIncrement(&g_weldNotes) <= 24)
    {
        uint32_t used = 0; SafeRead(weld + DIF_WELD_COUNT, &used, 4);
        Note("difficulty: weld research: the edges' vertices did not fit (0xFB9D90; side %.0f m, %u edges, %u of 576 bucket blocks)", (double)WeldSide(weld), (uint32_t)count, used);
    }
    return r;
}
static uint64_t WeldVertex(uint64_t weld, uint64_t vertex, uint64_t out, uint64_t d)
{
    uint64_t r = g_weldVertex(weld, vertex, out, d);
    if ((uint8_t)r == 0 && InterlockedIncrement(&g_weldNotes) <= 24)
    {
        float p[2] = {0, 0}, w = WeldSide(weld); SafeRead(vertex, p, 8);
        uint32_t bx = 0, by = 0, held = 0; uint64_t block = 0;
        if (w > 0.0f) { int64_t x = (int64_t)(p[0] / w * 23.0f), y = (int64_t)(p[1] / w * 23.0f); bx = (uint32_t)x > 23u ? 23u : (uint32_t)x; by = (uint32_t)y > 23u ? 23u : (uint32_t)y; }
        if (!SafeRead(weld + 8ull * (by * 24u + bx), &block, 8) && block != 0) SafeRead(block, &held, 4);
        Note("difficulty: weld research: a vertex did not fit (0xFB9B10; side %.0f m, at %.1f, %.1f: bucket %u, %u holds %u)", (double)w, (double)p[0], (double)p[1], bx, by, held);
    }
    return r;
}
static uint64_t WeldEdges(uint64_t weld, uint64_t b, uint64_t edges, uint64_t nodes)
{
    uint64_t r = g_weldEdges(weld, b, edges, nodes);
    if ((uint8_t)r == 0 && InterlockedIncrement(&g_weldNotes) <= 24)
    {
        uint32_t e[2] = {0, 0}, n[2] = {0, 0}, used = 0;
        SafeRead(edges, e, 8); SafeRead(nodes, n, 8); SafeRead(weld + DIF_WELD_COUNT, &used, 4);
        Note("difficulty: weld research: the edges' endpoints failed (0xFBB180; side %.0f m): edges %u, nodes %u (cap %u), %u of 576 bucket blocks", (double)WeldSide(weld), e[0], n[0], n[1], used);
    }
    return r;
}
static void WeldResearchInstall(uintptr_t game)
{
    static const struct { uint32_t rva; const char* pin; const char* what; void* detour; WeldFn4* original; } k[4] = {
        {0xFBBB20u, "40 55 53 56 57 41 54 41 55 48", "weld research (FinalizeVoronoi's weld)", (void*)WeldFinal, &g_weldFinal},
        {0xFB9D90u, "48 89 5C 24 18 48 89 6C 24 20", "weld research (the edges' vertices)", (void*)WeldCells, &g_weldCells},
        {0xFB9B10u, "48 89 5C 24 10 48 89 74 24 18", "weld research (one vertex)", (void*)WeldVertex, &g_weldVertex},
        {0xFBB180u, "48 89 5C 24 08 48 89 6C 24 10", "weld research (the endpoints)", (void*)WeldEdges, &g_weldEdges}};
    if (!LargerMaps()) return;
    for (int i = 0; i < 4; i++)
        if (ApiCodeMatches(game + k[i].rva, k[i].pin)) LoaderHook(k[i].what, game + k[i].rva, k[i].detour, (void**)k[i].original);
        else Note("difficulty: %s: not the reviewed code", k[i].what);
}
