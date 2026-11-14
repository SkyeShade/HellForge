/* tree-cell-limits (perf_tree_cell_limits.c) on a synthetic exe image: the pinned code at its build's places, the
 * forest push's start with a small end that counts its calls, and thunks that call the push from the two rebuilds'
 * call sites (as the game does) or from elsewhere. Each case checks the second axis after the push and that the game's
 * push always ran. Also the pins refusing changed bytes and HELLFORGE_PERF_OFF. Included by stats_test.c. */

#define TL_TEST_IMAGE 0x960000u
#define TL_TEST_THUNKS 0x900000u /* free room in the image for the thunks */
#define TL_TEST_RAN 0x390u       /* the end of the push counts its calls in the forest here */

typedef void (*TlTestCall)(uint8_t* forest);

/* A thunk with a 0x28 frame (rsp 16-aligned, home space of its own, as the game's callers have) that calls the push
 * (opcode 0xe8) or jumps to a call site (0xe9), whose return point then undoes the frame. */
static TlTestCall TlTestThunk(uint8_t* image, uint32_t rva, uint8_t opcode, uint32_t to)
{
    uint8_t* c = image + rva;
    static const uint8_t frame[] = {0x48,0x83,0xec,0x28}, unframe[] = {0x48,0x83,0xc4,0x28, 0xc3};
    memcpy(c, frame, sizeof frame);
    c[4] = opcode;
    int32_t rel = (int32_t)((intptr_t)(image + to) - (intptr_t)(c + 9));
    memcpy(c + 5, &rel, 4);
    memcpy(c + 9, unframe, sizeof unframe);
    return (TlTestCall)(void*)c;
}

static void TlTestCell(uint8_t* cells, uint32_t i, int32_t column, int32_t row, int live)
{
    uint8_t* cell = cells + i * TL_CELL_STRIDE;
    memcpy(cell, &column, 4);
    memcpy(cell + 4, &row, 4);
    int32_t next = live ? 0x7FFFFFFF : (int32_t)0xFFFFFFFEu;
    memcpy(cell + 0x60, &next, 4);
}

/* The forest's second axis set to [low, high], the push run through `call`; 1 when it came back as [wantLow, wantHigh]
 * and the game's push ran once. */
static int TlTestPush(TlTestCall call, uint8_t* forest, float low, float high, float wantLow, float wantHigh)
{
    float* axisLow = (float*)(forest + TL_ROW_MIN);
    float* axisHigh = (float*)(forest + TL_ROW_MAX);
    uint32_t* ran = (uint32_t*)(forest + TL_TEST_RAN);
    *axisLow = low;
    *axisHigh = high;
    uint32_t before = *ran;
    call(forest);
    return *ran == before + 1 && *axisLow == wantLow && *axisHigh == wantHigh;
}

static void TreeCellLimitsFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, TL_TEST_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    uintptr_t exe = (uintptr_t)image;
    for (size_t i = 0; i < TL_PINS; i++) memcpy(image + kTlPins[i].rva, kTlPins[i].bytes, kTlPins[i].size);
    /* the push's end after its pin: its frame undone, the call counted, back */
    static const uint8_t end[] = {0x48,0x83,0xc4,0x20, 0x41,0x5f, 0x41,0x5e, 0x5f, 0xff,0x81,0x90,0x03,0x00,0x00, 0xc3};
    memcpy(image + TL_PUSH + sizeof kTlPushPin, end, sizeof end);
    assert(TreeLimitsPinned(exe));
    for (size_t i = 0; i < TL_PINS; i++)
    {
        image[kTlPins[i].rva + kTlPins[i].size - 1] ^= 1;
        assert(!TreeLimitsPinned(exe)); /* another build's code is left alone */
        image[kTlPins[i].rva + kTlPins[i].size - 1] ^= 1;
    }

    assert(SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "corpse-freeze, tree-cell-limits"));
    assert(!TreeLimitsInstallAt(exe) && image[TL_PUSH] == 0x48);
    assert(SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL));
    assert(TreeLimitsInstallAt(exe) && image[TL_PUSH] != 0x48);

    /* after the install (the pins hold these bytes): the rebuild sites' calls return through their thunks' frames */
    static const uint8_t unframe[] = {0x48,0x83,0xc4,0x28, 0xc3};
    memcpy(image + TL_REMOVED_AT, unframe, sizeof unframe);
    memcpy(image + TL_MOVED_AT, unframe, sizeof unframe);
    TlTestCall removed = TlTestThunk(image, TL_TEST_THUNKS, 0xe9, TL_REMOVED_AT - 5);
    TlTestCall moved = TlTestThunk(image, TL_TEST_THUNKS + 0x40, 0xe9, TL_MOVED_AT - 5);
    TlTestCall other = TlTestThunk(image, TL_TEST_THUNKS + 0x80, 0xe8, TL_PUSH);
    FlushInstructionCache(GetCurrentProcess(), image, TL_TEST_IMAGE);

    _Alignas(16) uint8_t forest[0x400] = {0};
    _Alignas(16) uint8_t cells[8 * TL_CELL_STRIDE] = {0};
    float size = 64.0f;
    uint32_t slots = 4;
    uint8_t* cellsAt = cells;
    memcpy(forest + TL_CELL_SIZE, &size, 4);
    memcpy(forest + TL_CELLS, &slots, 4);
    memcpy(forest + TL_CELLS + 8, &cellsAt, 8);
    TlTestCell(cells, 0, 7, -3, 1);
    TlTestCell(cells, 1, 2, 0, 1);
    TlTestCell(cells, 2, -4, 5, 1);
    TlTestCell(cells, 3, 1, 40, 0); /* an empty slot's old row is not a cell */

    /* trees taken out: the rebuild left row 0's centre; the rows come back as the cells' centres */
    LONG restored = g_tlRestored;
    assert(TlTestPush(removed, forest, 32.0f, 32.0f, -160.0f, 352.0f) && g_tlRestored == restored + 1);
    assert(TlTestPush(other, forest, 32.0f, 32.0f, 32.0f, 32.0f)); /* another caller: the game's extents */
    assert(TlTestPush(removed, forest, 100.0f, 200.0f, 100.0f, 200.0f)); /* no row 0 centre: not a rebuild's */
    assert(TlTestPush(removed, forest, -160.0f, 32.0f, -160.0f, 352.0f) && g_tlRestored == restored + 2);
    assert(TlTestPush(removed, forest, -500.0f, 32.0f, -160.0f, 352.0f) && g_tlRestored == restored + 3); /* misses row 5 */
    /* a move's rebuild, then the new place widened the range */
    assert(TlTestPush(moved, forest, 32.0f, 500.0f, -160.0f, 352.0f) && g_tlRestored == restored + 4);
    TlTestCell(cells, 0, 7, 3, 1);
    TlTestCell(cells, 2, -4, 7, 1);
    assert(TlTestPush(moved, forest, 32.0f, 500.0f, 32.0f, 500.0f)); /* rows 0..7 held: kept */
    assert(TlTestPush(moved, forest, 32.0f, 400.0f, 32.0f, 480.0f) && g_tlRestored == restored + 5); /* row 7 missed */
    TlTestCell(cells, 0, 7, 0, 1);
    TlTestCell(cells, 2, -4, 0, 1);
    assert(TlTestPush(removed, forest, 32.0f, 32.0f, 32.0f, 32.0f)); /* every tree in row 0: already right */
    for (uint32_t i = 0; i < slots; i++) TlTestCell(cells, i, 0, 9, 0);
    assert(TlTestPush(removed, forest, 32.0f, 32.0f, 32.0f, 32.0f)); /* no cell left */
    TlTestCell(cells, 0, 0, 9, 1);
    float empty = 3.4e38f;
    assert(TlTestPush(removed, forest, empty, -empty, empty, -empty)); /* the empty range the game starts from */
    size = 0.0f;
    memcpy(forest + TL_CELL_SIZE, &size, 4);
    assert(TlTestPush(removed, forest, 0.0f, 0.0f, 0.0f, 0.0f)); /* no cell size: not a forest's */
    assert(g_tlRestored == restored + 5);

    assert(HookRemove(NULL, exe + TL_PUSH, (void*)TreeLimitsPush) == 0);
    VirtualFree(image, 0, MEM_RELEASE);
    puts("PASS tree-cell-limits: pins, HELLFORGE_PERF_OFF, both rebuilds given their rows back as cell centres, a range "
        "holding every row kept, other callers, empty and row-0 forests left as the game has them, the game's push run "
        "every time");
}
