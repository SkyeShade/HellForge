/* The navmesh carvers (difficulty_carvers.c) and NavRetile on synthetic nav worlds. The engine's grid creation, insert
 * and removal are written out from their code (0x7AD1D0, 0x7B52A0 -> 0x7AD5E0, 0x7AD8B0); a synthetic exe image holds
 * the pinned sites, the world table and the lock's imports, and the removal runs through the patched call itself.
 * Included by stats_test.c. */

#define NC_IMAGE 0x2800000u /* past the world table (0x27C30C0) */
#define NC_SLOTS 4096u
#define NC_STRIDE 0x80u
#define NC_CELL 0x70u
#define NC_CARVERS 40
#define NC_THUNK 0x1000u

typedef struct { float matrix[16], min[3], max[3]; } NcCarver;
static int g_ncLock, g_ncEnters, g_ncEngineRemoves;

static void NcEnter(void* lock) { assert(lock != NULL && g_ncLock == 0); g_ncLock++; g_ncEnters++; }
static void NcLeave(void* lock) { assert(lock != NULL && g_ncLock == 1); g_ncLock--; }

/* 0x7ACDD0: the column and row of a point, clamped; called only under the world's lock. */
static uint32_t NcCell(const void* grid, float x, float y)
{
    const NavCarverGrid* g = grid;
    assert(g_ncLock == 1);
    int cx = (int)((x - g->min[0]) / (g->max[0] - g->min[0]) * (float)g->cols);
    int cy = (int)((y - g->min[1]) / (g->max[1] - g->min[1]) * (float)g->rows);
    cx = cx <= 0 ? 0 : cx > g->cols - 1 ? g->cols - 1 : cx;
    cy = cy <= 0 ? 0 : cy > g->rows - 1 ? g->rows - 1 : cy;
    return (uint32_t)cy << 16 | (uint32_t)cx;
}

/* A stand-in for 0x9FB5C0 (the box of a matrix's rows applied to a local box), with its alignment demand. */
static void NcBox(const float* m, const float* box, float* out)
{
    assert(((uintptr_t)m & 15) == 0 && ((uintptr_t)box & 15) == 0 && ((uintptr_t)out & 15) == 0);
    for (int a = 0; a < 3; a++)
    {
        float lo = m[12 + a], hi = m[12 + a];
        for (int i = 0; i < 3; i++)
        {
            float p = m[4 * i + a] * box[i], q = m[4 * i + a] * box[4 + i];
            lo += p < q ? p : q;
            hi += p < q ? q : p;
        }
        out[a] = lo; out[4 + a] = hi;
    }
    out[3] = out[7] = 0.0f;
}

static uint64_t* NcBits(const NavCarverGrid* g)
{
    uintptr_t at = (uintptr_t)g->cells + (uintptr_t)g->cols * g->rows * g->cellSize;
    return (uint64_t*)((at + 7) & ~(uintptr_t)7);
}

static size_t NcBitBytes(const NavCarverGrid* g) { return ((size_t)g->cols + g->rows) * g->words * 8; }

/* 0x7AD1D0 with the engine's record and cell sizes and NC_SLOTS slots; the world's lock object at +0x30. */
static uint8_t* NcWorld(float lo, float hi, uint16_t cells)
{
    uint8_t* w = calloc(1, 0x6750);
    NavCarverGrid* g = (NavCarverGrid*)(w + 0x6710);
    uint32_t words = (NC_SLOTS + 63) / 64;
    uint8_t* pool = calloc(1, (size_t)NC_SLOTS * NC_STRIDE + (size_t)cells * cells * NC_CELL + 8 + (size_t)2 * cells * words * 8);
    assert(w != NULL && pool != NULL);
    g->cols = g->rows = cells;
    g->cellBits = cells == 8 ? 3 : 5;
    g->slotBits = 12;
    g->flags = g->slotBits + 4 * g->cellBits <= 32;
    g->tile = (hi - lo) / cells;
    g->min[0] = g->min[1] = lo; g->max[0] = g->max[1] = hi;
    g->slots = NC_SLOTS; g->mask = 0xFFFFFFFFu >> (32 - g->slotBits);
    g->stride = NC_STRIDE; g->cellSize = NC_CELL; g->words = words;
    g->head = 1;
    g->records = pool; g->cells = pool + (size_t)NC_SLOTS * NC_STRIDE;
    for (uint32_t k = 0; k + 1 < NC_SLOTS; k++) *(int32_t*)(pool + (size_t)k * NC_STRIDE) = (int32_t)(k + 1);
    *(int32_t*)(pool + (size_t)(NC_SLOTS - 1) * NC_STRIDE) = -1;
    *(void**)(w + 0x30) = w + 0x40;
    return w;
}

/* 0x7B52A0 and 0x7AD5E0: the world box, the pop, the bits and the handle under the lock; the record after it. */
static uint64_t NcInsert(uint8_t* w, const NcCarver* c)
{
    NavCarverGrid* g = (NavCarverGrid*)(w + 0x6710);
    _Alignas(16) float m[16], box[8] = {c->min[0], c->min[1], c->min[2], 0.0f, c->max[0], c->max[1], c->max[2], 0.0f}, out[8];
    memcpy(m, c->matrix, sizeof m);
    NcBox(m, box, out);
    NcEnter(*(void**)(w + 0x30));
    int32_t k = g->head;
    assert(k > 0);
    g->head = *(int32_t*)(g->records + (size_t)k * NC_STRIDE);
    uint32_t c0 = NcCell(g, out[0], out[1]), c1 = NcCell(g, out[4], out[5]);
    uint64_t* bits = NcBits(g);
    for (uint32_t x = c0 & 0xFFFF; x <= (c1 & 0xFFFF); x++) bits[(size_t)x * g->words + (k >> 6)] |= 1ull << (k & 63);
    for (uint32_t y = c0 >> 16; y <= (c1 >> 16); y++) bits[((size_t)g->cols + y) * g->words + (k >> 6)] |= 1ull << (k & 63);
    unsigned b = g->cellBits, total = (g->flags & 1) ? 32 : 64;
    uint64_t cells = (uint64_t)(c0 & 0xFFFF) << 3 * b | (uint64_t)(c0 >> 16) << 2 * b | (uint64_t)(c1 & 0xFFFF) << b | (c1 >> 16);
    NcLeave(*(void**)(w + 0x30));
    uint8_t* record = g->records + (size_t)k * NC_STRIDE;
    memset(record, 0, NC_STRIDE);
    memcpy(record, m, sizeof m);
    memcpy(record + 0x48, c->min, 12);
    memcpy(record + 0x54, c->max, 12);
    return cells << (total - 4 * b) | (uint32_t)k;
}

/* 0x7AD8B0: the slot pushed, then the bits of the handle's cells cleared. */
static void NcEngineRemove(void* grid, uint64_t h)
{
    NavCarverGrid* g = grid;
    g_ncEngineRemoves++;
    if (h == 0) return;
    unsigned b = g->cellBits, total = (g->flags & 1) ? 32 : 64;
    uint64_t f = (1ull << b) - 1;
    uint32_t x0 = (uint32_t)(h >> (total - b) & f), y0 = (uint32_t)(h >> (total - 2 * b) & f);
    uint32_t x1 = (uint32_t)(h >> (total - 3 * b) & f), y1 = (uint32_t)(h >> (total - 4 * b) & f), k = (uint32_t)h & g->mask;
    *(int32_t*)(g->records + (size_t)k * NC_STRIDE) = g->head;
    g->head = (int32_t)k;
    uint64_t* bits = NcBits(g);
    for (uint32_t x = x0; x <= x1; x++) bits[(size_t)x * g->words + (k >> 6)] &= ~(1ull << (k & 63));
    for (uint32_t y = y0; y <= y1; y++) bits[((size_t)g->cols + y) * g->words + (k >> 6)] &= ~(1ull << (k & 63));
}

/* The nav world's removal as the game runs it: the patched call at 0x7B503B, reached through a thunk that gives it the
 * caller's stack alignment and takes it back from 0x7B5040. */
static void NcRemove(uintptr_t exe, uint8_t* w, uint64_t h) { ((void (*)(void*, uint64_t))(exe + NC_THUNK))(w + 0x6710, h); }

/* Carvers rotated about z, some across several cells, some past both mappings' edges. */
static void NcCarvers(NcCarver* c)
{
    uint32_t seed = 0x2E2E2E2Eu;
    for (int i = 0; i < NC_CARVERS; i++)
    {
        float r[5];
        for (int j = 0; j < 5; j++) { seed = seed * 1664525u + 1013904223u; r[j] = (float)(seed >> 8) / 16777216.0f; }
        float angle = r[0] * 6.2831853f, size = i % 7 == 0 ? 300.0f : 2.0f + 40.0f * r[1];
        memset(&c[i], 0, sizeof c[i]);
        c[i].matrix[0] = cosf(angle); c[i].matrix[1] = sinf(angle); c[i].matrix[4] = -sinf(angle); c[i].matrix[5] = cosf(angle);
        c[i].matrix[10] = 1.0f; c[i].matrix[15] = 1.0f;
        c[i].matrix[12] = -1100.0f + 2200.0f * r[2]; c[i].matrix[13] = -1100.0f + 2200.0f * r[3]; c[i].matrix[14] = 20.0f * r[4];
        c[i].min[0] = c[i].min[1] = -size / 2; c[i].min[2] = -1.0f;
        c[i].max[0] = c[i].max[1] = size / 2; c[i].max[2] = 1.0f;
    }
}

/* Every slot but 0 on the free list once, ending at -1. */
static int NcListWhole(const NavCarverGrid* g)
{
    static uint8_t seen[NC_SLOTS];
    memset(seen, 0, sizeof seen);
    uint32_t count = 0;
    for (int32_t at = g->head; at != -1; count++)
    {
        if (at <= 0 || (uint32_t)at >= NC_SLOTS || seen[at] || count >= NC_SLOTS) return 0;
        seen[at] = 1;
        at = *(const int32_t*)(g->records + (size_t)at * NC_STRIDE);
    }
    return count == NC_SLOTS - 1;
}

static int NcBitsZero(const NavCarverGrid* g)
{
    const uint64_t* bits = NcBits(g);
    for (size_t i = 0; i < NcBitBytes(g) / 8; i++) if (bits[i]) return 0;
    return 1;
}

/* One script into a world: 30 inserts, four removals (`patched`: through the game's call, else the engine's), 10 more. */
static void NcScript(uintptr_t exe, uint8_t* w, const NcCarver* c, uint64_t* h, int patched)
{
    for (int i = 0; i < NC_CARVERS; i++)
    {
        for (int j = 3; i == 30 && j < 30; j += 8)
        {
            if (patched) NcRemove(exe, w, h[j]);
            else NcEngineRemove(w + 0x6710, h[j]);
            h[j] = 0;
        }
        h[i] = NcInsert(w, &c[i]);
    }
}

/* Carvers filed under one mapping, NavRetile to the other: the bits of a fresh insert under the new one, and every old
 * handle's removal leaving no bit and a whole free list. Left as they were, the bits differ; re-filed and then removed
 * by their old cells (the engine's removal), bits stay behind. */
static void NcDirection(uintptr_t exe, uintptr_t* worlds, int down)
{
    const float lo[2] = {-511.75f, -1023.75f}, hi[2] = {512.25f, 1024.25f};
    uint8_t *w = NcWorld(lo[down], hi[down], 8), *ref = NcWorld(lo[!down], hi[!down], 8), *stale = NcWorld(lo[down], hi[down], 8);
    NavCarverGrid *g = (NavCarverGrid*)(w + 0x6710), *gr = (NavCarverGrid*)(ref + 0x6710), *gs = (NavCarverGrid*)(stale + 0x6710);
    NcCarver c[NC_CARVERS];
    uint64_t h[NC_CARVERS], hr[NC_CARVERS], hs[NC_CARVERS];
    NcCarvers(c);
    NcScript(exe, w, c, h, 1);
    NcScript(exe, ref, c, hr, 0);
    NcScript(exe, stale, c, hs, 0);
    for (int i = 0; i < NC_CARVERS; i++) assert(((uint32_t)h[i] & g->mask) == ((uint32_t)hr[i] & gr->mask));
    uint8_t level[0x2D90] = {0};
    float disk = 1000.0f;
    memcpy(level + 0x2D8C, &disk, 4);
    worlds[2] = (uintptr_t)w;
    g_navRefiled = -1;
    NavRetile(2, down ? 0 : (uintptr_t)level);
    assert(g->min[0] == gr->min[0] && g->max[1] == gr->max[1] && g->tile == gr->tile && g_ncLock == 0);
    assert(g_navRefiled == NC_CARVERS - 4 && g->head == gr->head && memcmp(NcBits(g), NcBits(gr), NcBitBytes(g)) == 0);
    int removes = g_ncEngineRemoves;
    for (int i = 0; i < NC_CARVERS; i++) if (h[i]) NcRemove(exe, w, h[i]);
    assert(g_ncEngineRemoves == removes && NcBitsZero(g) && NcListWhole(g));
    memcpy(gs->min, gr->min, 16);
    assert(memcmp(NcBits(gs), NcBits(gr), NcBitBytes(gs)) != 0);
    assert(NavCarversRefile(stale) == NC_CARVERS - 4 && memcmp(NcBits(gs), NcBits(gr), NcBitBytes(gs)) == 0);
    for (int i = 0; i < NC_CARVERS; i++) if (hs[i]) NcEngineRemove(gs, hs[i]);
    assert(!NcBitsZero(gs) && NcListWhole(gs));
}

static void NavCarversFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, NC_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    uintptr_t exe = (uintptr_t)image;
    for (int i = 0; i < 7; i++) memcpy(image + kCarverPins[i].rva, kCarverPins[i].pin, kCarverPins[i].length);
    assert(InstallNavCarvers(exe, NC_IMAGE) != NULL && !g_navCarvers.ready); /* the lock's imports not bound */
    NavLockFn enter = NcEnter, leave = NcLeave;
    memcpy(image + DIF_CARVER_ENTER, &enter, 8); memcpy(image + DIF_CARVER_LEAVE, &leave, 8);
    image[0x7B536Bu + 40] ^= 1;
    assert(InstallNavCarvers(exe, NC_IMAGE) != NULL && !g_navCarvers.ready); /* a pin off */
    image[0x7B536Bu + 40] ^= 1;
    assert(InstallNavCarvers(exe, NC_IMAGE) == NULL && g_navCarvers.ready && g_navCarvers.enter == NcEnter);
    int32_t rel;
    memcpy(&rel, image + DIF_CARVER_REMOVE_SITE + 11, 4);
    const uint8_t* jump = image + DIF_CARVER_REMOVE_SITE + 15 + rel;
    void* remove = (void*)NavCarverRemove;
    assert(jump[0] == 0x48 && jump[1] == 0xB8 && memcmp(jump + 2, &remove, 8) == 0 && jump[10] == 0xFF && jump[11] == 0xE0);
    g_navCarvers.cell = NcCell; g_navCarvers.box = NcBox; g_navCarvers.remove = NcEngineRemove;
    /* sub rsp, 8; jmp 0x7B503B | add rsp, 8; ret; and 0x7B5040 jumps back to the second half */
    static const uint8_t thunk[14] = {0x48, 0x83, 0xEC, 0x08, 0xE9, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x08, 0xC3};
    memcpy(image + NC_THUNK, thunk, sizeof thunk);
    rel = (int32_t)(0x7B503Bu - (NC_THUNK + 9)); memcpy(image + NC_THUNK + 5, &rel, 4);
    image[0x7B5040] = 0xE9; rel = (int32_t)((NC_THUNK + 9) - 0x7B5045u); memcpy(image + 0x7B5041, &rel, 4);
    static uintptr_t worlds[4];
    uintptr_t table = (uintptr_t)worlds;
    uint32_t count = 4;
    memcpy(image + DIF_NAV_WORLDS, &table, 8); memcpy(image + DIF_NAV_COUNT, &count, 4);
    g_navExe = exe; g_navTiles256 = 1;
    NcDirection(exe, worlds, 0);
    NcDirection(exe, worlds, 1);
    /* the mapping kept: nothing re-filed, the lock never taken */
    uint8_t* same = NcWorld(-511.75f, 512.25f, 8);
    NcCarver c[NC_CARVERS];
    NcCarvers(c);
    NcInsert(same, &c[0]);
    worlds[2] = (uintptr_t)same;
    int enters = g_ncEnters;
    g_navRefiled = -1;
    NavRetile(2, 0);
    assert(g_ncEnters == enters && g_navRefiled == -1);
    /* another grid (a 32 x 32 one): NavRetile leaves it, and its removal is the engine's */
    uint8_t* wide = NcWorld(-511.75f, 512.25f, 32);
    NavCarverGrid* gw = (NavCarverGrid*)(wide + 0x6710);
    uint64_t hw = NcInsert(wide, &c[1]);
    uint8_t before[0x40];
    memcpy(before, gw, sizeof before);
    worlds[2] = (uintptr_t)wide;
    enters = g_ncEnters;
    uint8_t level[0x2D90] = {0};
    float disk = 1000.0f;
    memcpy(level + 0x2D8C, &disk, 4);
    NavRetile(2, (uintptr_t)level);
    assert(memcmp(before, gw, sizeof before) == 0 && g_ncEnters == enters);
    int removes = g_ncEngineRemoves;
    NcRemove(exe, wide, hw);
    assert(g_ncEngineRemoves == removes + 1 && NcBitsZero(gw) && NcListWhole(gw));
    g_navExe = 0; g_navTiles256 = 0;
    memset(&g_navCarvers, 0, sizeof g_navCarvers);
    puts("PASS navmesh carvers: pins and the lock's imports checked, the removal patched through a jump near the exe; carvers filed at 128 m and at 256 m and retiled to the other match a fresh insert, every old handle's removal leaves no bit and a whole free list; a kept mapping re-files nothing; a 32 x 32 grid is left alone and keeps the engine's removal");
}
