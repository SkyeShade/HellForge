/* The heap trim (perf_heap_trim.c) on synthetic exe and game images: the pins and the slot accept the reviewed code
 * and refuse any changed byte; the installed fix calls a stand-in trim only where a load ends on the ship or a
 * mission, read through the game's state as the update reads it; HELLFORGE_PERF_OFF leaves it off. Included by
 * stats_test.c. */

#define HT_EXE_SIZE 0x670000u
#define HT_GAME_SIZE 0x3330000u
#define HT_STATE_GAME 0x3326340u /* EVT_STATE_GAME: the same page as the API pointer */

static LONG g_htTestTrims;
static void HtTestTrim(void) { g_htTestTrims++; }

static void HtPut(uintptr_t image, uint32_t rva, const uint8_t* bytes, size_t size)
{
    assert(VirtualAlloc((void*)((image + rva) & ~(uintptr_t)0xFFF), ((image + rva + size + 0xFFF) & ~(uintptr_t)0xFFF) - ((image + rva) & ~(uintptr_t)0xFFF),
        MEM_COMMIT, PAGE_READWRITE) != NULL);
    memcpy((void*)(image + rva), bytes, size);
}

static void HeapTrimFixture(void)
{
    uintptr_t exe = (uintptr_t)VirtualAlloc(NULL, HT_EXE_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    uintptr_t game = (uintptr_t)VirtualAlloc(NULL, HT_GAME_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    uint8_t* stateBlock = (uint8_t*)VirtualAlloc(NULL, EVT_STATE_AT + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(exe != 0 && game != 0 && stateBlock != NULL);
    static uintptr_t api[4], table[HT_SLOT / 8 + 1];
    const struct { uintptr_t image; uint32_t rva; const uint8_t* pin; size_t size; } pins[] = {
        {exe, HT_TRIM, kHtTrimPin, sizeof kHtTrimPin}, {exe, HT_WORKERS, kHtWorkersPin, sizeof kHtWorkersPin},
        {exe, HT_RENDER, kHtRenderPin, sizeof kHtRenderPin}, {exe, HT_HEAP, kHtHeapPin, sizeof kHtHeapPin},
        {exe, HT_GLOBAL, kHtGlobalPin, sizeof kHtGlobalPin}, {game, HT_SHIP_ENTER, kHtShipEnterPin, sizeof kHtShipEnterPin},
        {game, HT_MISSION_ENTER, kHtMissionEnterPin, sizeof kHtMissionEnterPin}};
    for (size_t i = 0; i < sizeof pins / sizeof pins[0]; i++) HtPut(pins[i].image, pins[i].rva, pins[i].pin, pins[i].size);
    uintptr_t apiAt = (uintptr_t)api, stateAt = (uintptr_t)stateBlock, call = 0;
    HtPut(game, HT_API, (const uint8_t*)&apiAt, 8);
    HtPut(game, HT_STATE_GAME, (const uint8_t*)&stateAt, 8);
    api[2] = (uintptr_t)table;
    table[HT_SLOT / 8] = exe + HT_TRIM;

    assert(HeapTrimPinned(exe, game, &call) && call == exe + HT_TRIM);
    for (size_t i = 0; i < sizeof pins / sizeof pins[0]; i++) /* another build's code: off */
    {
        uint8_t* at = (uint8_t*)(pins[i].image + pins[i].rva + pins[i].size - 1);
        *at ^= 0x40;
        assert(!HeapTrimPinned(exe, game, &call));
        *at ^= 0x40;
    }
    table[HT_SLOT / 8] = exe + HT_TRIM + 0x10; /* the slot swapped: the game no longer calls this trim */
    assert(!HeapTrimPinned(exe, game, &call) && HeapTrimInstallAt(exe, game) == 0 && g_htState == 0);
    table[HT_SLOT / 8] = exe + HT_TRIM;

    wchar_t path[MAX_PATH];
    static char text[8192];
    GetTempPathW(MAX_PATH, path);
    swprintf(path + wcslen(path), MAX_PATH - wcslen(path), L"hellforge-heap-trim-%lu.log", GetCurrentProcessId());
    DeleteFileW(path);
    HANDLE saved = g_log;
    g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(g_log != INVALID_HANDLE_VALUE);

    SetEnvironmentVariableW(L"HELLFORGE_PERF_OFF", L"safe-read, heap-trim");
    assert(HeapTrimInstallAt(exe, game) == -1 && g_htState == -1);
    HeapTrimStep(HF_STATE_PREPARE_SHIP);
    HeapTrimStep(HF_STATE_SHIP);
    assert(strstr(MfLogged(path, text, sizeof text), "performance fix heap-trim: off (HELLFORGE_PERF_OFF)") && !strstr(text, "heap trim 1"));
    SetEnvironmentVariableW(L"HELLFORGE_PERF_OFF", NULL);

    g_htState = 0;
    assert(HeapTrimInstallAt(exe, game) == 1 && g_htState == 1 && g_htCall == (HtTrimFn)(void*)(exe + HT_TRIM));
    assert(strstr(MfLogged(path, text, sizeof text), "heap trim: on;"));
    g_htCall = HtTestTrim; /* the image's bytes are pins, not code */

    /* the first state only sets the baseline; loads end where the ship or a mission begins */
    static const struct { uint32_t state; LONG trims; } steps[] = {
        {HF_STATE_NONE, 0}, {HF_STATE_SHIP, 0}, {HF_STATE_SHIP, 0}, {HF_STATE_PREPARE_MISSION, 0}, {HF_STATE_NONE, 0},
        {HF_STATE_MISSION, 1}, {HF_STATE_MISSION, 1}, {HF_STATE_PREPARE_SHIP, 1}, {HF_STATE_SHIP, 2},
        {HF_STATE_TITLE, 2}, {HF_STATE_SHIP, 3}, /* a load whose prepare state no update saw still ends */
        {HF_STATE_NONE, 3}, {HF_STATE_SHIP, 3}};
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++)
    {
        HeapTrimStep(steps[i].state);
        assert(g_htTestTrims == steps[i].trims);
    }
    assert(strstr(MfLogged(path, text, sizeof text), "heap trim 3, the ship reached:") && strstr(text, "heap trim 1, a mission reached:"));

    /* the update's own read: [game + 0x3326340] + 0xAC21C */
    uint32_t state = HF_STATE_PREPARE_MISSION;
    memcpy(stateBlock + EVT_STATE_AT, &state, 4);
    HeapTrimTick();
    assert(g_htTestTrims == 3);
    state = HF_STATE_MISSION;
    memcpy(stateBlock + EVT_STATE_AT, &state, 4);
    HeapTrimTick();
    HeapTrimTick();
    assert(g_htTestTrims == 4);

    CloseHandle(g_log);
    g_log = saved;
    DeleteFileW(path);
    g_htState = 0;
    g_htCall = NULL;
    g_htTrims = 0;
    VirtualFree((void*)exe, 0, MEM_RELEASE);
    VirtualFree((void*)game, 0, MEM_RELEASE);
    VirtualFree(stateBlock, 0, MEM_RELEASE);
    printf("heap trim: pins, slot, load ends and the off switch pass\n");
}
