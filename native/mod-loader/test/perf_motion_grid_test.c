/* The motion-grid fix (perf_motion_grid.c) on a synthetic game image that holds the reviewed bytes: the game's own
 * set-up, insert, remove, move and slot add run for real (floor behind their calls is the C library's), with the set-up
 * hooked for real. Included by stats_test.c after perf_vehicle_box_test.c (VbtFar). */

#define MGT_GAME 0x23C8000u
#define MGT_FLOOR 0x2108E78u      /* game.dll's floor, called by the insert and the move */
#define MGT_ONE 0x23C6D70u        /* the set-up's 1.0f */
#define MGT_INSERT 0x134A970u     /* (grid base, kind, position, radius in xmm3) -> handle */
#define MGT_REMOVE 0x134A9E0u     /* (grid base, handle) */
#define MGT_MOVE 0x17353E0u       /* (grid header, entry, position) */
#define MGT_KINDS 2u

typedef uint32_t (*MgtInsertFn)(uintptr_t base, int kind, const float* position, float radius);
typedef void (*MgtRemoveFn)(uintptr_t base, uint32_t handle);
typedef void (*MgtMoveFn)(uintptr_t header, uint32_t entry, const float* position);

static uint8_t* g_mgtGame;

static uintptr_t MgtGrid(uintptr_t base, uint32_t kind) { return base + (uintptr_t)kind * 0x2430u; }

/* The probe the clip makes for a position's cell: the entry indexes in it, and the slots looked at. */
static uint32_t MgtCell(uintptr_t grid, const float* p, uint32_t* found, uint32_t room, uint32_t* probes)
{
    uint32_t count = *(uint32_t*)(grid + MG_COUNT_AT), step = *(uint32_t*)(grid + MG_STEP_AT), n = 0;
    float inv = *(float*)(grid + 0x2420);
    uint32_t key = ((uint32_t)(int)floor((double)(inv * p[1])) * 0x25c8b353u ^ (uint32_t)(int)floor((double)(inv * p[0])) * 0x1d60b43fu) & (count - 1u);
    const uint32_t* slots = *(const uint32_t**)(grid + MG_SLOTS_AT);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t at = (i + key * step) & (count - 1u);
        (*probes)++;
        if (slots[at * 2] == 0xFFFFFFFFu) break;
        if (slots[at * 2] == key && n < room) found[n++] = slots[at * 2 + 1];
    }
    return n;
}

static void MgtPosition(uint32_t k, float* p, float shift)
{
    p[0] = (float)(k % 40u) * 1.3f + shift; /* a crowd: 40 by 30 agents 1.3 m apart, so several share a 2 m cell */
    p[1] = (float)(k / 40u) * 1.3f - 7.0f;
    p[2] = 3.0f;
}

/* Inserts `agents` of kind 0, then checks each is in its cell's slots with its own entry and radius. The mean probes. */
static double MgtFill(uintptr_t base, uint32_t agents, uint32_t* handles, float shift)
{
    MgtInsertFn insert = (MgtInsertFn)(void*)(g_mgtGame + MGT_INSERT);
    uintptr_t grid = MgtGrid(base, 0);
    for (uint32_t k = 0; k < agents; k++)
    {
        float p[3];
        MgtPosition(k, p, shift);
        handles[k] = insert(base, 0, p, 0.5f + (float)k * 0.001f);
        assert(handles[k] >> 16 == 1u);
    }
    uint32_t probes = 0;
    for (uint32_t k = 0; k < agents; k++)
    {
        uint32_t entry = handles[k] & 0xFFFFu, found[64];
        float p[3];
        MgtPosition(k, p, shift);
        assert(entry < MG_RADII && *(float*)(grid + (uintptr_t)entry * 8u) == 0.5f + (float)k * 0.001f);
        uint32_t n = MgtCell(grid, p, found, 64, &probes), seen = 0;
        for (uint32_t i = 0; i < n; i++) seen += found[i] == entry;
        assert(seen == 1);
        const float* e = (const float*)(*(uint8_t**)(grid + MG_ENTRIES_AT) + (size_t)entry * MG_ENTRY);
        assert(e[0] == p[0] && e[1] == p[1] && ((const uint32_t*)e)[4] == entry);
    }
    return (double)probes / (double)agents;
}

static void MotionGridFixture(void)
{
    g_mgtGame = (uint8_t*)VirtualAlloc(NULL, MGT_GAME, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    uint8_t* component = (uint8_t*)VirtualAlloc(NULL, 0x18u + MGT_KINDS * 0x2430u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(g_mgtGame != NULL && component != NULL);
    uintptr_t game = (uintptr_t)g_mgtGame;
    for (size_t i = 0; i < MG_PINS; i++) memcpy(g_mgtGame + kMgPins[i].rva, kMgPins[i].bytes, kMgPins[i].size);
    float one = 1.0f;
    memcpy(g_mgtGame + MGT_ONE, &one, 4);
    VbtFar(g_mgtGame + MGT_FLOOR, (void*)(double (*)(double))floor);
    FlushInstructionCache(GetCurrentProcess(), g_mgtGame, MGT_GAME);
    assert(MotionGridMismatch(game) == NULL);
    for (size_t i = 0; i < MG_PINS; i++)
    {
        g_mgtGame[kMgPins[i].rva + kMgPins[i].size - 1] ^= 1;
        assert(MotionGridMismatch(game) == kMgPins[i].what);
        g_mgtGame[kMgPins[i].rva + kMgPins[i].size - 1] ^= 1;
    }
    uintptr_t base = (uintptr_t)component + 0x18u;
    uint32_t* handles = (uint32_t*)malloc(1300 * sizeof *handles);
    assert(handles != NULL);

    /* the game's own grid, before the fix: 256 agents fill it, and the 257th takes over an entry and probes every slot */
    ((MgInitFn)(void*)(g_mgtGame + MG_INIT))(MgtGrid(base, 0), 2.0f);
    ((MgInitFn)(void*)(g_mgtGame + MG_INIT))(MgtGrid(base, 1), 10.0f);
    assert(MotionGridFresh(MgtGrid(base, 0)) && MotionGridFresh(MgtGrid(base, 1)));
    double gameProbes = MgtFill(base, 200, handles, 0.0f);
    float away[3] = {500.0f, 500.0f, 0.0f};
    uint32_t found[8], probes = 0;
    for (uint32_t k = 0; k < 100; k++)
    {
        float p[3];
        MgtPosition(k, p, 100.0f);
        ((MgtInsertFn)(void*)(g_mgtGame + MGT_INSERT))(base, 0, p, 1.0f);
    }
    MgtCell(MgtGrid(base, 0), away, found, 8, &probes);
    assert(probes == MG_GAME_COUNT);

    /* refused: switched off, then code not yet decoded and never decoded; nothing is hooked */
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "motion-grid");
    assert(MotionGridTry(game, 0) == -1 && g_mgNext == NULL);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    g_mgtGame[0x134B200u + 3] ^= 1;
    assert(MotionGridTry(game, 0) == 0 && MotionGridTry(game, 1) == -1 && g_mgNext == NULL);
    g_mgtGame[0x134B200u + 3] ^= 1;

    assert(MotionGridTry(game, 0) == 1 && g_mgNext != NULL);
    assert(MotionGridMismatch(game) == NULL); /* the pins read through the hook */

    /* the component's set-up of both kinds: both larger, the header ours, the cell sizes and steps the game's */
    ((MgInitFn)(void*)(g_mgtGame + MG_INIT))(MgtGrid(base, 0), 2.0f);
    ((MgInitFn)(void*)(g_mgtGame + MG_INIT))(MgtGrid(base, 1), 10.0f);
    assert(g_mgGridCount == 2 && g_mgEnlarged == 2);
    for (uint32_t kind = 0; kind < MGT_KINDS; kind++)
    {
        uintptr_t grid = MgtGrid(base, kind);
        assert(*(uint32_t*)(grid + MG_COUNT_AT) == MG_COUNT && *(uintptr_t*)(grid + MG_SLOTS_AT) == (uintptr_t)g_mgGrids[kind].slots);
        assert(*(uintptr_t*)(grid + MG_ENTRIES_AT) == (uintptr_t)g_mgGrids[kind].entries && *(uint32_t*)(grid + MG_STEP_AT) == 2u);
        assert(*(float*)(grid + 0x2420) == (kind ? 0.1f : 0.5f) && *(float*)(grid + 0x2428) == (kind ? 10.0f : 2.0f));
        /* the blocks past the game's 256: no edge query (+4 is 0), where the game's set-up had left its slot keys, -1 */
        for (uint32_t e = MG_GAME_COUNT; e < MG_RADII; e++) assert(((uint32_t*)grid)[e * 2 + 1] == 0 && ((uint32_t*)grid)[e * 2] == 0);
    }

    /* 1,100 agents: each in its cell once with its radius, short probes, no entry past the radii */
    double ourProbes = MgtFill(base, 1100, handles, 0.0f);
    assert(ourProbes <= gameProbes + 1.0 && ourProbes < 8.0);
    probes = 0;
    MgtCell(MgtGrid(base, 0), away, found, 8, &probes);
    assert(probes < 16);

    /* moves: through cells and inside one, every agent still found once */
    MgtMoveFn move = (MgtMoveFn)(void*)(g_mgtGame + MGT_MOVE);
    for (uint32_t k = 0; k < 1100; k++)
    {
        float p[3];
        MgtPosition(k, p, 0.7f);
        move(MgtGrid(base, 0) + MG_SLOTS_AT, handles[k] & 0xFFFFu, p);
    }
    for (uint32_t k = 0; k < 1100; k++)
    {
        float p[3];
        uint32_t list[64], n, seen = 0;
        MgtPosition(k, p, 0.7f);
        probes = 0;
        n = MgtCell(MgtGrid(base, 0), p, list, 64, &probes);
        for (uint32_t i = 0; i < n; i++) seen += list[i] == (handles[k] & 0xFFFFu);
        assert(seen == 1);
    }

    /* removes: half the agents gone from their cells, their entries free again and handed out first */
    MgtRemoveFn remove = (MgtRemoveFn)(void*)(g_mgtGame + MGT_REMOVE);
    for (uint32_t k = 0; k < 1100; k += 2) remove(base, handles[k]);
    for (uint32_t k = 0; k < 1100; k++)
    {
        float p[3];
        uint32_t list[64], n, seen = 0;
        MgtPosition(k, p, 0.7f);
        probes = 0;
        n = MgtCell(MgtGrid(base, 0), p, list, 64, &probes);
        for (uint32_t i = 0; i < n; i++) seen += list[i] == (handles[k] & 0xFFFFu);
        assert(seen == (k % 2 ? 1u : 0u));
    }

    /* past the radii: the game's own overflow (the cursor's entry taken over), never an entry past 1,151 */
    ((MgInitFn)(void*)(g_mgtGame + MG_INIT))(MgtGrid(base, 0), 2.0f);
    assert(g_mgGridCount == 2 && g_mgEnlarged == 3); /* the same grid again: our arrays reused, emptied */
    MgtInsertFn insert = (MgtInsertFn)(void*)(g_mgtGame + MGT_INSERT);
    for (uint32_t k = 0; k < 1300; k++)
    {
        float p[3];
        MgtPosition(k, p, 0.0f);
        uint32_t h = insert(base, 0, p, 1.0f);
        assert((h & 0xFFFFu) < MG_RADII);
        if (k < MG_RADII) assert((h & 0xFFFFu) == k);
    }
    for (uint32_t i = MG_RADII; i < MG_COUNT; i++)
        assert(*(uint32_t*)(g_mgGrids[0].entries + (size_t)i * MG_ENTRY + 0xC) == MG_TAKEN);
    /* the inline region now holds radii only: nothing past them was written */
    assert(*(uint32_t*)(MgtGrid(base, 0) + MG_COUNT_AT) == MG_COUNT && *(int32_t*)(MgtGrid(base, 0) + MG_EMPTY_AT) == -1);

    /* a grid that is not the game's fresh set-up stays as it is */
    LONG refused = g_mgRefused;
    *(uint32_t*)(MgtGrid(base, 1) + MG_CURSOR_AT) = 3;
    MotionGridEnlarge(MgtGrid(base, 1));
    assert(g_mgRefused == refused + 1 && *(uintptr_t*)(MgtGrid(base, 1) + MG_SLOTS_AT) == (uintptr_t)g_mgGrids[1].slots);
    free(handles);
    printf("PASS motion grid: pins, the switch, the game's own grid set-up, insert, move and remove run on 4,096 slots: 1,100 agents "
        "each in its cell once with its radius (%.1f probes a lookup, the game's %.1f at 200 agents), none handed an entry past "
        "the radii, the same grid set up again, a grid not fresh left alone\n", ourProbes, gameProbes);
}
