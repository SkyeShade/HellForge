/* Two crashes of a weapon's customize screen, build 25480438. Each card entry points to an item whose type sits behind
 * [item+0x30]; the game reads both unchecked. A card with no item (sorting, 0x1493712 reading 0x30) or with an item but
 * no type (the UI's scene pass, 0x1A042DA reading 0x0) has been met after the Armory unloaded packages. The guards keep
 * such a card out of the way instead. Included by render_guards.c: AllocateNearGame,
 * PinMatches, JournalPut, LoaderHook, Note, g_build. */

/* The UI's scene pass over the cards (0x1A04220) skips an entry with no item; the cave skips one with no type too. */
#define CG_LOOP 0x1A042C0u /* the loop's body: the entry, its null check, its type read and the type test */
#define CG_READ 0x1A042D6u /* mov rax, [rax+0x30]; mov edx, [rax]: moved into the cave */
#define CG_BACK 0x1A042DCu /* bt ebp, edx: the type test */
#define CG_NEXT 0x1A04468u /* the loop's next entry: reloads rdx, and rax before any read */
static const uint8_t kCgLoopPin[] = {0x48,0x8d,0x0c,0x7f, 0x48,0x8b,0x44,0xca,0x38, 0x4c,0x8d,0x0c,0xca, 0x48,0x85,0xc0,
    0x0f,0x84,0x92,0x01,0x00,0x00, 0x48,0x8b,0x40,0x30, 0x8b,0x10, 0x0f,0xa3,0xd5, 0x0f,0x83,0x83,0x01,0x00,0x00};

/* The cards' sort order (a, b): by type, then other fields of the item. A card with no item or no type sorts last. */
#define CG_ORDER 0x1493700u
static const uint8_t kCgOrderPin[] = {0x4c,0x8b,0x19, 0x4c,0x8b,0xc9, 0x4c,0x8b,0x12, 0x4c,0x8b,0xc2, 0x49,0x8b,0x43,0x30,
    0x8b,0x08, 0x49,0x8b,0x42,0x30, 0x3b,0x08};

typedef int (*CgOrderFn)(const void* a, const void* b);
static CgOrderFn g_cgOrder;
static int g_cgState; /* 0 waiting for game.dll's code, 1 installed, -1 not */
static ULONGLONG g_cgLoadedAt, g_cgReportedAt;
static volatile LONG g_cgSkipped, g_cgSorted;
static LONG g_cgSkippedReported, g_cgSortedReported;

static uint8_t* CgJump(uint8_t* c, uintptr_t to)
{
    int32_t rel = (int32_t)((intptr_t)to - (intptr_t)(c + 5));
    *c = 0xe9;
    memcpy(c + 1, &rel, 4);
    return c + 5;
}

/* The moved reads with a null check between them; a null type pointer counts and goes on to the next entry. */
static void CgCave(uint8_t* c, uintptr_t game)
{
    static const uint8_t read[] = {0x48,0x8b,0x40,0x30, 0x48,0x85,0xc0, 0x74,0x07, 0x8b,0x10}; /* jz past mov and jmp */
    memcpy(c, read, sizeof read); c += sizeof read;
    c = CgJump(c, game + CG_BACK);
    *c++ = 0x48; *c++ = 0xb8; /* mov rax, imm64: rax is reloaded at the next entry */
    uint64_t counter = (uint64_t)(uintptr_t)&g_cgSkipped;
    memcpy(c, &counter, 8); c += 8;
    static const uint8_t count[] = {0xf0,0xff,0x00}; /* lock inc dword [rax] */
    memcpy(c, count, sizeof count); c += sizeof count;
    CgJump(c, game + CG_NEXT);
}

/* An entry's item, then its type; any read that faults counts as missing. */
static int CgTyped(const void* entry)
{
    uintptr_t item = 0, type = 0;
    return entry && !SafeRead((uintptr_t)entry, &item, 8) && item && !SafeRead(item + 0x30, &type, 8) && type;
}

static int CgOrderDetour(const void* a, const void* b)
{
    int typedA = CgTyped(a), typedB = CgTyped(b);
    if (typedA && typedB) return g_cgOrder(a, b);
    InterlockedIncrement(&g_cgSorted);
    return typedA - typedB ? (typedA ? -1 : 1) : 0;
}

static int CustomizeGuardsInstallAt(uintptr_t game)
{
    int scene = 0, order = 0;
    uint8_t* cave = AllocateNearGame(game, 0x1000);
    if (cave)
    {
        CgCave(cave, game);
        DWORD previous;
        VirtualProtect(cave, 0x1000, PAGE_EXECUTE_READ, &previous);
        FlushInstructionCache(GetCurrentProcess(), cave, 0x1000);
        uint8_t jump[6] = {0xe9, 0, 0, 0, 0, 0x90};
        int32_t rel = (int32_t)((intptr_t)cave - (intptr_t)(game + CG_READ + 5));
        memcpy(jump + 1, &rel, 4);
        scene = !JournalPut(game + CG_READ, jump, sizeof jump);
    }
    if (PinMatches(game + CG_ORDER, kCgOrderPin, sizeof kCgOrderPin))
        order = !LoaderHook("customize guard: a card with no item", game + CG_ORDER, (void*)CgOrderDetour, (void**)&g_cgOrder);
    Note("customize guards: a card with no type is %s in the scene pass, and one with no item or type %s",
        scene ? "skipped" : "not guarded", order ? "sorts last" : "is not guarded in the sort");
    return scene || order;
}

/* After the game loaded. game.dll's code is decoded some time after; a minute later it never will be the reviewed one. */
static void CustomizeGuardsInstall(void)
{
    if (g_cgState != 0) return;
    if (strcmp(g_build, "25480438")) { g_cgState = -1; return; }
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0 || !PinMatches(game + CG_LOOP, kCgLoopPin, sizeof kCgLoopPin))
    {
        if (g_cgLoadedAt == 0) g_cgLoadedAt = GetTickCount64();
        if (GetTickCount64() - g_cgLoadedAt < 60000) return;
        g_cgState = -1;
        Note("customize guards: not installed; the customize screen's code is not the reviewed one");
        return;
    }
    g_cgState = CustomizeGuardsInstallAt(game) ? 1 : -1;
}

/* What the guards caught, at most every 10 s. */
static void CustomizeGuardsWatch(void)
{
    CustomizeGuardsInstall();
    if (g_cgState != 1 || GetTickCount64() - g_cgReportedAt < 10000) return;
    LONG skipped = g_cgSkipped, sorted = g_cgSorted;
    if (skipped == g_cgSkippedReported && sorted == g_cgSortedReported) return;
    g_cgReportedAt = GetTickCount64();
    g_cgSkippedReported = skipped;
    g_cgSortedReported = sorted;
    Note("customize guards: %ld card(s) with no type skipped in the scene pass and %ld sorted last with no item or type so far",
        (long)skipped, (long)sorted);
}
