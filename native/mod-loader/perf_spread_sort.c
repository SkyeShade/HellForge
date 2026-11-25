/* SpreadLocations (op 0x8B, game.dll 0xF7C310) sorts every candidate of the map by its squared distance to a location,
 * once per location, with the game's quicksort (0xC04470) calling the comparator (0xF7A5A0) through a pointer each time.
 * That sort runs here with the comparison inlined, in the same steps, so even equal distances end in the game's order.
 * Included by hellforge_modloader.c after render_guards.c: PinMatches (packages.c), HookedCode and LoaderHook (hooks.c). */

#define PS_SORT 0xC04470u      /* (base, count, width, compare): every qsort of game.dll */
#define PS_SORT_SIZE 0x32Au    /* to its ret; the replica below is this function */
#define PS_SORT_FNV 0x46927241c40eef23ull
#define PS_COMPARE 0xF7A5A0u   /* -1 when b.d > a.d, 1 when a.d > b.d, else 0; d is the float at +8 */
#define PS_RECORD 12u          /* {float x, y, d} */
static const uint8_t kPsComparePin[] = {0xf3,0x0f,0x10,0x41,0x08,0xf3,0x0f,0x10,0x4a,0x08,0x0f,0x2f,0xc8,0x76,0x06,0xb8,0xff,0xff,
    0xff,0xff,0xc3,0x33,0xc0,0x0f,0x2f,0xc1,0x0f,0x97,0xc0,0xc3};

typedef void (*PsSortFn)(void* base, size_t count, size_t width, int (*compare)(const void*, const void*));
static PsSortFn g_psOriginal; /* the next in the sort's hook chain */
static uintptr_t g_psCompare;
static int g_psState;         /* 0 waiting for game.dll's code, 1 installed, -1 not */
static ULONGLONG g_psLoadedAt;
static volatile LONG g_psSorts;

static float PsKey(const uint8_t* a, size_t i)
{
    float d;
    memcpy(&d, a + i * PS_RECORD + 8, 4);
    return d;
}

static void PsSwap(uint8_t* a, size_t i, size_t j)
{
    uint8_t t[PS_RECORD];
    memcpy(t, a + i * PS_RECORD, PS_RECORD);
    memcpy(a + i * PS_RECORD, a + j * PS_RECORD, PS_RECORD);
    memcpy(a + j * PS_RECORD, t, PS_RECORD);
}

/* 0xC04470's partition, step for step: the pivot stays in the array and moves with its swaps, j scans down while
 * a[j].d > pivot, i up while pivot > a[i].d (both false for a NaN, as comiss). The parts are [0, *left) and
 * [*right, n). */
static void PsPartition(uint8_t* a, size_t n, size_t* left, size_t* right)
{
    size_t i = (size_t)-1, j = n, p = n >> 1;
    for (;;)
    {
        float pivot = PsKey(a, p);
        do j--; while (PsKey(a, j) > pivot);
        do i++; while (pivot > PsKey(a, i));
        if (i >= j) break;
        PsSwap(a, i, j);
        if (i == p) p = j;
        else if (j == p) p = i;
    }
    *left = i;
    *right = j + 1;
}

typedef struct { uint8_t* a; size_t n; } PsPart;

/* The game recurses into the left part and loops on the right. The parts never overlap, so sorting the smaller first
 * ends in the same bytes, and the larger waits on a stack of at most log2 n. */
static PsPart PsSplit(PsPart part, PsPart* larger)
{
    size_t left, right;
    PsPartition(part.a, part.n, &left, &right);
    PsPart l = {part.a, left}, r = {part.a + right * PS_RECORD, part.n - right};
    if (l.n < r.n) { *larger = r; return l; }
    *larger = l;
    return r;
}

static void PsSortRecords(uint8_t* a, size_t n)
{
    PsPart stack[64], part = {a, n};
    int depth = 0;
    for (;;)
    {
        while (part.n > 1) part = PsSplit(part, &stack[depth++]);
        if (depth == 0) return;
        part = stack[--depth];
    }
}

/* Last in the sort's chain. Only SpreadLocations' comparator is taken; every other sort goes on to the game's. */
static void PsSortDetour(void* base, size_t count, size_t width, int (*compare)(const void*, const void*))
{
    if (width != PS_RECORD || (uintptr_t)compare != g_psCompare) { g_psOriginal(base, count, width, compare); return; }
    PsSortRecords((uint8_t*)base, count);
    InterlockedIncrement(&g_psSorts);
}

static uint64_t PsFnv(const uint8_t* bytes, size_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; i++) h = (h ^ bytes[i]) * 0x100000001b3ull;
    return h;
}

/* The whole sort as reviewed (read through any hook in front of it) and the comparator. */
static int PsPinned(uintptr_t game)
{
    uint8_t sort[PS_SORT_SIZE];
    return !HookedCode(game + PS_SORT, sort, sizeof sort) && PsFnv(sort, sizeof sort) == PS_SORT_FNV &&
        PinMatches(game + PS_COMPARE, kPsComparePin, sizeof kPsComparePin);
}

static int PerfSpreadSortInstallAt(uintptr_t game)
{
    if (!PerfFixOn("spread-sort")) return 0;
    g_psCompare = game + PS_COMPARE;
    if (LoaderHook("spread-sort: SpreadLocations' candidate sort", game + PS_SORT, (void*)PsSortDetour, (void**)&g_psOriginal))
    {
        Note("performance fix spread-sort: not installed; the game's sort could not be hooked");
        return 0;
    }
    Note("performance fix spread-sort: SpreadLocations' candidate sorts take the game's own steps with the comparison inlined");
    return 1;
}

/* After the game loaded (WatchInstalls). game.dll's code is decoded some time after; a minute later it never will be
 * the reviewed one. */
static void PerfSpreadSortInstall(void)
{
    if (g_psState != 0) return;
    if (strcmp(g_build, "25480438")) { g_psState = -1; return; }
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0 || !PsPinned(game))
    {
        if (g_psLoadedAt == 0) g_psLoadedAt = GetTickCount64();
        if (GetTickCount64() - g_psLoadedAt < 60000) return;
        g_psState = -1;
        Note("performance fix spread-sort: not installed; the game's sort is not the reviewed one");
        return;
    }
    g_psState = PerfSpreadSortInstallAt(game) ? 1 : -1;
}
