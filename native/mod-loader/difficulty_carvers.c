/* The navmesh's carvers (the cuts that units and props make in it) when a larger map changes the main nav world's grid.
 * Included by hellforge_modloader.c before difficulty_maps.c, whose NavRetile re-files through it. */

/* The engine files a carver once: its insert (exe 0x7B52A0 -> 0x7AD5E0) sets the slot's bit in the column and row
 * bitsets of the cells its world box covers, under the grid mapping of that moment, and only the owner's handle keeps
 * those cells. The tile build finds carvers through the same bitsets (0x7B7590 -> 0x7ADA50). So when NavRetile changes
 * the mapping, a carver filed before it is missing from its tiles: the ship's (filed in PrepareShip, game 0xAC022D)
 * after a 256 m mission. A change re-files every live carver from its record, and the nav world's removal (0x7B503B)
 * clears a slot in every cell instead of the handle's, which go stale. */
#define DIF_CARVER_REMOVE_SITE 0x7B5031u  /* mov rdx, rbp (the handle); lea rcx, [r14+0x6710]; call 0x7AD8B0 */
#define DIF_CARVER_REMOVE 0x7AD8B0u       /* the engine's removal: every other grid's, kept */
#define DIF_CARVER_CELL 0x7ACDD0u         /* (grid, x, y) -> row << 16 | column; another mod may stand in front of it */
#define DIF_CARVER_BOX 0x9FB5C0u          /* (matrix, box, out): the world box, a leaf; all three 16-aligned */
#define DIF_CARVER_ENTER 0x13F66B8u       /* the import the insert locks [W+0x30] with, and the one it unlocks with */
#define DIF_CARVER_LEAVE 0x13F66C0u
#define DIF_CARVER_SLOTS 0x100000u        /* slots the free map holds; the engine creates 0x20000 */
static const uint8_t kCarverRemoveSitePin[15] = {0x48, 0x8B, 0xD5, 0x49, 0x8D, 0x8E, 0x10, 0x67, 0x00, 0x00, 0xE8, 0x70, 0x88, 0xFF, 0xFF};
/* 0x7AD95F: the slot is the handle's low dword & +0x20; it is pushed on the free list; the bitsets follow the cells */
static const uint8_t kCarverPushPin[92] = {0x41, 0x8B, 0x56, 0x20, 0x44, 0x8B, 0xCA, 0x41, 0x8B, 0x4E, 0x2C, 0x45, 0x23, 0xCF, 0x45,
    0x0F, 0xB7, 0x46, 0x24, 0x41, 0x23, 0xD1, 0x44, 0x0F, 0xAF, 0xC2, 0x41, 0x8B, 0xD9, 0x49, 0x8B, 0x56, 0x30, 0x45, 0x8B, 0xF9,
    0xC1, 0xEB, 0x06, 0x41, 0x83, 0xE7, 0x3F, 0x41, 0x89, 0x0C, 0x10, 0x41, 0x0F, 0xB7, 0x4E, 0x02, 0x41, 0x0F, 0xB7, 0x56, 0x26,
    0x0F, 0xAF, 0xD1, 0x45, 0x89, 0x4E, 0x2C, 0x45, 0x0F, 0xB7, 0x0E, 0x41, 0x0F, 0xAF, 0xD1, 0x48, 0x63, 0xCA, 0x49, 0x03, 0x4E,
    0x38, 0x4C, 0x8B, 0xC1, 0x49, 0xF7, 0xD8, 0x41, 0x83, 0xE0, 0x07, 0x4C, 0x03, 0xC1};
/* 0x7B536B: the insert's box {+0x58, +0x60 z, 0} {+0x64, +0x6C z, 0} through 0x9FB5C0, its min and max x, y kept */
static const uint8_t kCarverBoxPin[88] = {0xF3, 0x0F, 0x10, 0x57, 0x6C, 0x4C, 0x8D, 0x45, 0xE0, 0xF2, 0x0F, 0x10, 0x67, 0x64, 0x48,
    0x8D, 0x55, 0x80, 0xF2, 0x0F, 0x10, 0x5F, 0x58, 0x48, 0x8D, 0x4D, 0x00, 0x0F, 0x16, 0xE2, 0xF3, 0x0F, 0x10, 0x57, 0x60, 0x0F,
    0x16, 0xDA, 0x0F, 0x29, 0x5D, 0x80, 0x66, 0x0F, 0x7F, 0x65, 0x90, 0xE8, 0x21, 0x62, 0x24, 0x00, 0x0F, 0x28, 0x7D, 0xE0, 0x45,
    0x33, 0xF6, 0x44, 0x0F, 0x28, 0x45, 0xF0, 0x44, 0x0F, 0x28, 0xCF, 0x48, 0x8B, 0x05, 0x02, 0xDD, 0x00, 0x02, 0x45, 0x0F, 0x28,
    0xD0, 0x0F, 0xC6, 0xFF, 0x55, 0x45, 0x0F, 0xC6, 0xC0, 0x55};
/* 0x7B551A: [W+0x30] locked, 0x7AD5E0(grid, min x, min y, max x, max y, &handle), unlocked */
static const uint8_t kCarverLockPin[61] = {0x48, 0x8B, 0x5B, 0x30, 0x48, 0x8B, 0xCB, 0xFF, 0x15, 0x91, 0x11, 0xC4, 0x00, 0x48, 0x8D,
    0x44, 0x24, 0x70, 0x41, 0x0F, 0x28, 0xDA, 0x48, 0x89, 0x44, 0x24, 0x28, 0x0F, 0x28, 0xD7, 0x41, 0x0F, 0x28, 0xC9, 0xF3, 0x44,
    0x0F, 0x11, 0x44, 0x24, 0x20, 0x48, 0x8B, 0xCE, 0xE8, 0x95, 0x80, 0xFF, 0xFF, 0x48, 0x8B, 0xCB, 0x48, 0x8B, 0xF0, 0xFF, 0x15,
    0x69, 0x11, 0xC4, 0x00};
/* 0x7B559B: the record keeps that box at +0x48..+0x5F and the matrix at +0x00..+0x3F */
static const uint8_t kCarverRecordPin[72] = {0xF2, 0x0F, 0x10, 0x47, 0x58, 0xF2, 0x0F, 0x11, 0x46, 0x48, 0x8B, 0x47, 0x60, 0x89, 0x46,
    0x50, 0xF2, 0x0F, 0x10, 0x47, 0x64, 0xF2, 0x0F, 0x11, 0x46, 0x54, 0x8B, 0x47, 0x6C, 0x89, 0x46, 0x5C, 0x8B, 0x87, 0x90, 0x00,
    0x00, 0x00, 0x89, 0x46, 0x44, 0x0F, 0x28, 0x45, 0x00, 0x0F, 0x11, 0x06, 0x0F, 0x28, 0x4D, 0x10, 0x0F, 0x11, 0x4E, 0x10, 0x0F,
    0x28, 0x45, 0x20, 0x0F, 0x11, 0x46, 0x20, 0x0F, 0x28, 0x4D, 0x30, 0x0F, 0x11, 0x4E, 0x30};
static const uint8_t kCarverBoxFnPin[32] = {0x48, 0x83, 0xEC, 0x38, 0x0F, 0x28, 0x0A, 0x44, 0x0F, 0x29, 0x04, 0x24, 0x44, 0x0F, 0x28,
    0x42, 0x10, 0x41, 0x0F, 0x28, 0xC0, 0x0F, 0xC2, 0xC1, 0x01, 0x0F, 0x50, 0xC0, 0xA8, 0x07, 0x74, 0x17};
/* from +10: the cell step's first two instructions are where another mod's dispatch may stand */
static const uint8_t kCarverCellPin[32] = {0xF3, 0x0F, 0x5C, 0x49, 0x0C, 0x0F, 0xB7, 0x01, 0xF3, 0x0F, 0x5C, 0x51, 0x10, 0x44, 0x0F,
    0xB7, 0x41, 0x02, 0xF3, 0x0F, 0x5E, 0xC8, 0x66, 0x0F, 0x6E, 0xC0, 0xFF, 0xC8, 0x0F, 0x5B, 0xC0, 0xF3};
static const struct { uint32_t rva; uint8_t length; const uint8_t* pin; } kCarverPins[7] = {
    {DIF_CARVER_REMOVE_SITE, sizeof kCarverRemoveSitePin, kCarverRemoveSitePin}, {0x7AD95Fu, sizeof kCarverPushPin, kCarverPushPin},
    {0x7B536Bu, sizeof kCarverBoxPin, kCarverBoxPin}, {0x7B551Au, sizeof kCarverLockPin, kCarverLockPin},
    {0x7B559Bu, sizeof kCarverRecordPin, kCarverRecordPin}, {DIF_CARVER_BOX, sizeof kCarverBoxFnPin, kCarverBoxFnPin},
    {DIF_CARVER_CELL + 10, sizeof kCarverCellPin, kCarverCellPin}};

/* The grid at W+0x6710, as its creation (0x7AD1D0) writes it. The bitsets follow the cells, 8-aligned: a column's, then
 * a row's, words qwords each. Slot 0 is never on the free list, so never handed out (0x7AD3B7). */
typedef struct NavCarverGrid
{
    uint16_t cols, rows;
    uint8_t cellBits, slotBits;
    uint16_t flags;          /* bit 0: the handle is 32 bits */
    float tile, min[2], max[2];
    uint32_t slots, mask;
    uint16_t stride, cellSize; /* a record's, a cell's */
    uint32_t words;
    volatile int32_t head;   /* the free list's; a free record's first dword links the next, -1 ends it */
    uint8_t* records;
    uint8_t* cells;
} NavCarverGrid;
_Static_assert(offsetof(NavCarverGrid, min) == 0x0C && offsetof(NavCarverGrid, slots) == 0x1C && offsetof(NavCarverGrid, stride) == 0x24 &&
    offsetof(NavCarverGrid, head) == 0x2C && offsetof(NavCarverGrid, cells) == 0x38, "the engine's grid");

typedef uint32_t (*NavCellFn)(const void* grid, float x, float y);
typedef void (*NavBoxFn)(const float* matrix, const float* box, float* out);
typedef void (*NavLockFn)(void* lock);
typedef void (*NavRemoveFn)(void* grid, uint64_t handle);
static struct { NavCellFn cell; NavBoxFn box; NavLockFn enter, leave; NavRemoveFn remove; int ready; } g_navCarvers;
static uint64_t g_navCarverFree[DIF_CARVER_SLOTS / 64]; /* a re-file's free slots: game thread only */

static uint64_t* NavCarverBits(const NavCarverGrid* g)
{
    uintptr_t at = (uintptr_t)g->cells + (uintptr_t)(intptr_t)(int32_t)((uint32_t)g->cols * g->rows * g->cellSize);
    return (uint64_t*)((at + 7) & ~(uintptr_t)7);
}

/* Slot k out of every column and row, wherever it was filed. Atomic: the engine's removals take no lock. */
static void NavCarverClear(const NavCarverGrid* g, uint32_t k)
{
    uint64_t* bits = NavCarverBits(g) + (k >> 6);
    for (uint32_t i = 0; i < (uint32_t)g->cols + g->rows; i++)
        InterlockedAnd64((volatile LONG64*)(bits + (size_t)i * g->words), (LONG64)~(1ull << (k & 63)));
}

/* Slot k into columns c0..c1 and rows c0..c1 (row << 16 | column), as the insert's loops (0x7AD6D0, 0x7AD711). */
static void NavCarverFile(const NavCarverGrid* g, uint32_t k, uint32_t c0, uint32_t c1)
{
    uint64_t* bits = NavCarverBits(g) + (k >> 6);
    const LONG64 bit = (LONG64)(1ull << (k & 63));
    for (uint32_t x = c0 & 0xFFFF; x <= (c1 & 0xFFFF) && x < g->cols; x++) InterlockedOr64((volatile LONG64*)(bits + (size_t)x * g->words), bit);
    for (uint32_t y = c0 >> 16; y <= (c1 >> 16) && y < g->rows; y++)
        InterlockedOr64((volatile LONG64*)(bits + ((size_t)g->cols + y) * g->words), bit);
}

/* The nav world's removal (the call at 0x7B503B): an 8 x 8 grid, the one NavRetile changes, frees by slot; the engine's
 * own removal takes the cells from the handle, which a re-file leaves stale. Every other grid keeps the engine's. */
static void NavCarverRemove(void* grid, uint64_t handle)
{
    NavCarverGrid* g = grid;
    if (g->cols != 8 || g->rows != 8) { g_navCarvers.remove(grid, handle); return; }
    if (handle == 0) return;
    uint32_t k = (uint32_t)handle & g->mask;
    NavCarverClear(g, k); /* before the push, so a slot handed out again has no bit left */
    *(volatile int32_t*)(g->records + (uintptr_t)((uint32_t)g->stride * k)) = g->head;
    g->head = (int32_t)k;
}

/* Live slot k filed again from its record, as a fresh insert would file it now: its matrix and local box rebuild the
 * insert's world box bit for bit. */
static void NavCarverRefileSlot(const NavCarverGrid* g, uint32_t k)
{
    const uint8_t* record = g->records + (uintptr_t)((uint32_t)g->stride * k);
    _Alignas(16) float matrix[16], box[8], out[8];
    memcpy(matrix, record, sizeof matrix);
    memcpy(box, record + 0x48, 12); box[3] = 0.0f;
    memcpy(box + 4, record + 0x54, 12); box[7] = 0.0f;
    g_navCarvers.box(matrix, box, out);
    uint32_t c0 = g_navCarvers.cell(g, out[0], out[1]), c1 = g_navCarvers.cell(g, out[4], out[5]);
    NavCarverClear(g, k);
    NavCarverFile(g, k, c0, c1);
}

/* Every live carver of world W filed again under its grid's mapping, under the world's lock as the insert files: how
 * many, or -1 when the free list does not end within the slots (nothing changed). The result does not depend on when
 * it runs: afterwards every carver is where a fresh insert would put it. */
static int64_t NavCarversRefile(uint8_t* world)
{
    NavCarverGrid* g = (NavCarverGrid*)(world + 0x6710);
    const uint32_t n = g->slots, words = (n + 63) >> 6;
    void* lock = *(void**)(world + 0x30);
    if (!g_navCarvers.ready || n < 2 || n > DIF_CARVER_SLOTS || g->words != words || g->stride < 0x60 || g->records == NULL || lock == NULL)
        return -1;
    g_navCarvers.enter(lock);
    memset(g_navCarverFree, 0, (size_t)words * 8);
    g_navCarverFree[0] = 1; /* slot 0 */
    uint32_t steps = 0;
    for (int32_t at = g->head; at != -1; steps++)
    {
        if (at <= 0 || (uint32_t)at >= n || steps >= n || (g_navCarverFree[at >> 6] >> (at & 63) & 1)) { g_navCarvers.leave(lock); return -1; }
        g_navCarverFree[at >> 6] |= 1ull << (at & 63);
        at = *(const int32_t*)(g->records + (uintptr_t)((uint32_t)g->stride * (uint32_t)at));
    }
    int64_t live = 0;
    for (uint32_t k = 1; k < n; k++)
        if (!(g_navCarverFree[k >> 6] >> (k & 63) & 1)) { NavCarverRefileSlot(g, k); live++; }
    g_navCarvers.leave(lock);
    return live;
}

/* The re-file's engine functions and the removal through a jump near the exe (the call's rel32 reaches +-2 GB), when
 * every pinned site is the reviewed code. NULL, or why not. */
static const char* InstallNavCarvers(uintptr_t exe, size_t image)
{
    uint8_t code[96];
    for (int i = 0; i < 7; i++)
        if (SafeRead(exe + kCarverPins[i].rva, code, kCarverPins[i].length) || memcmp(code, kCarverPins[i].pin, kCarverPins[i].length) != 0)
            return "the navmesh carvers' code is not the reviewed one";
    uintptr_t enter = 0, leave = 0;
    if (SafeRead(exe + DIF_CARVER_ENTER, &enter, 8) || SafeRead(exe + DIF_CARVER_LEAVE, &leave, 8) || enter == 0 || leave == 0)
        return "the navmesh carvers' lock is not bound yet";
    uint8_t* jump = FindNearAbove(exe, image, 0x1000, MEM_COMMIT | MEM_RESERVE);
    if (jump == NULL) jump = FindNearBelow(exe, image, 0x1000, MEM_COMMIT | MEM_RESERVE);
    if (jump == NULL) return "no memory within 2 GB of the exe for the carvers' removal";
    g_navCarvers.cell = (NavCellFn)(exe + DIF_CARVER_CELL);
    g_navCarvers.box = (NavBoxFn)(exe + DIF_CARVER_BOX);
    g_navCarvers.remove = (NavRemoveFn)(exe + DIF_CARVER_REMOVE);
    g_navCarvers.enter = (NavLockFn)enter;
    g_navCarvers.leave = (NavLockFn)leave;
    void* remove = (void*)NavCarverRemove;
    jump[0] = 0x48; jump[1] = 0xB8; memcpy(jump + 2, &remove, 8); /* mov rax, NavCarverRemove (rax is dead at the call) */
    jump[10] = 0xFF; jump[11] = 0xE0;                              /* jmp rax */
    DWORD protect;
    if (!VirtualProtect(jump, 0x1000, PAGE_EXECUTE_READ, &protect)) return "the carvers' removal could not be made to run";
    FlushInstructionCache(GetCurrentProcess(), jump, 0x1000);
    int32_t rel = (int32_t)((intptr_t)jump - (intptr_t)(exe + DIF_CARVER_REMOVE_SITE + 15));
    if (ApiWrite(exe + DIF_CARVER_REMOVE_SITE + 11, &rel, 4)) return "the carvers' removal could not be patched";
    g_navCarvers.ready = 1;
    return NULL;
}
