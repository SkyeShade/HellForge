/* The ai-query-chunks fix (perf_ai_query_chunks.c) on synthetic images that hold the reviewed bytes. The four tasks,
 * the join and the job runner are hooked for real; behind each hook's moved instructions a stand-in runs: a task
 * counts every entry of its row's range, the job runner starts a thread per row, the join waits for the rows' done
 * flags. Included by stats_test.c after perf_vehicle_box_test.c (VbtFar). */

#define AQT_GAME 0x1760000u
#define AQT_EXE 0xA5000u
#define AQT_ENTRIES 4000u

static uint8_t* g_aqtGame;
static uint8_t* g_aqtExe;
static volatile LONG g_aqtSeen[AQ_BATCHES][AQT_ENTRIES];
static uint32_t g_aqtRows[AQ_BATCHES][AQ_ROWS_MAX][3];
static uint32_t* g_aqtRowPointers[AQ_BATCHES][AQ_ROWS_MAX];
static uint32_t g_aqtRowCount[AQ_BATCHES];
static HANDLE g_aqtThreads[AQ_BATCHES * AQ_ROWS_MAX];
static int g_aqtThreadCount;
static HANDLE g_aqtGo;           /* the task threads wait for it: the join's own chunks come first */
static volatile LONG g_aqtJoined;

typedef struct AqtStart { uintptr_t fn; uint32_t* row; } AqtStart;
static AqtStart g_aqtStarts[AQ_BATCHES * AQ_ROWS_MAX];

/* The task stand-ins: each entry of the row's range once, then the row done. */
static uint64_t AqtTaskBody(int b, uint32_t* row)
{
    for (uint32_t i = row[0]; i < row[1]; i++) InterlockedIncrement(&g_aqtSeen[b][i]);
    ((volatile uint32_t*)row)[2] = 1;
    return 0;
}
static uint64_t AqtTask0(void* s, void* t, void* c, uint32_t* row) { (void)s; (void)t; (void)c; return AqtTaskBody(0, row); }
static uint64_t AqtTask1(void* s, void* t, void* c, uint32_t* row) { (void)s; (void)t; (void)c; return AqtTaskBody(1, row); }
static uint64_t AqtTask2(void* s, void* t, void* c, uint32_t* row) { (void)s; (void)t; (void)c; return AqtTaskBody(2, row); }
static uint64_t AqtTask3(void* s, void* t, void* c, uint32_t* row) { (void)s; (void)t; (void)c; return AqtTaskBody(3, row); }

static DWORD WINAPI AqtWorker(void* p)
{
    AqtStart* s = (AqtStart*)p;
    WaitForSingleObject(g_aqtGo, INFINITE);
    ((AqTaskFn)(void*)s->fn)(NULL, NULL, NULL, s->row);
    return 0;
}

/* The job runner's stand-in: a thread per row, each through the task's hooked entry. */
static uint32_t AqtRunJob(void* fn, uint32_t** rows, const char* name, uint32_t p4, int32_t priority, uint32_t count, void* p7)
{
    (void)name; (void)p4; (void)priority; (void)p7;
    for (uint32_t i = 0; i < count; i++)
    {
        AqtStart* s = &g_aqtStarts[g_aqtThreadCount];
        s->fn = (uintptr_t)fn;
        s->row = rows[i];
        g_aqtThreads[g_aqtThreadCount] = CreateThread(NULL, 0, AqtWorker, s, 0, NULL);
        assert(g_aqtThreads[g_aqtThreadCount] != NULL);
        g_aqtThreadCount++;
    }
    return 1;
}

/* The join's stand-in: the workers go, every row's done flag, the threads closed. */
static void AqtJoin(void* entities)
{
    (void)entities;
    SetEvent(g_aqtGo);
    for (int b = 0; b < AQ_BATCHES; b++)
        for (uint32_t i = 0; i < g_aqtRowCount[b]; i++)
            while (((volatile uint32_t*)g_aqtRows[b][i])[2] == 0) Sleep(0);
    WaitForMultipleObjects((DWORD)g_aqtThreadCount, g_aqtThreads, TRUE, INFINITE);
    for (int i = 0; i < g_aqtThreadCount; i++) CloseHandle(g_aqtThreads[i]);
    g_aqtThreadCount = 0;
    InterlockedIncrement(&g_aqtJoined);
}

/* The game's cut of `count` entries over `tasks` rows (count / tasks + 1 each), handed to the hooked job runner. */
static void AqtKick(int b, uint32_t count, uint32_t tasks)
{
    uint32_t per = count / tasks + 1, used = 0, at = 0;
    for (uint32_t i = 0; i < AQ_ROWS_MAX; i++) g_aqtRows[b][i][2] = 1;
    while (used < tasks && at < count)
    {
        uint32_t n = count - at < per ? count - at : per;
        g_aqtRows[b][used][0] = at;
        g_aqtRows[b][used][1] = at + n;
        g_aqtRows[b][used][2] = 0;
        g_aqtRowPointers[b][used] = g_aqtRows[b][used];
        at += n;
        used++;
    }
    g_aqtRowCount[b] = used;
    ((AqRunJobFn)(void*)(g_aqtExe + AQ_RUN_JOB))((void*)(g_aqtGame + g_aq[b].rva), g_aqtRowPointers[b], "task", 0, 1, used, NULL);
}

static void AqtFrame(const uint32_t* counts, int go)
{
    static const uint32_t tasks[AQ_BATCHES] = {24, 8, 8, 12};
    memset((void*)g_aqtSeen, 0, sizeof g_aqtSeen);
    if (go) SetEvent(g_aqtGo); else ResetEvent(g_aqtGo);
    for (int b = 0; b < AQ_BATCHES; b++) AqtKick(b, counts[b], tasks[b]);
    ((AqJoinFn)(void*)(g_aqtGame + AQ_JOIN))(NULL);
    for (int b = 0; b < AQ_BATCHES; b++)
    {
        for (uint32_t i = 0; i < counts[b]; i++) assert(g_aqtSeen[b][i] == 1);
        for (uint32_t i = counts[b]; i < AQT_ENTRIES; i++) assert(g_aqtSeen[b][i] == 0);
        assert(!g_aq[b].active);
    }
}

static void AiQueryChunksFixture(void)
{
    g_aqtGame = (uint8_t*)VirtualAlloc(NULL, AQT_GAME, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    g_aqtExe = (uint8_t*)VirtualAlloc(NULL, AQT_EXE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    g_aqtGo = CreateEventA(NULL, TRUE, FALSE, NULL);
    assert(g_aqtGame != NULL && g_aqtExe != NULL && g_aqtGo != NULL);
    uintptr_t game = (uintptr_t)g_aqtGame, exe = (uintptr_t)g_aqtExe;
    for (size_t i = 0; i < AQ_GAME_PINS; i++) memcpy(g_aqtGame + kAqGamePins[i].rva, kAqGamePins[i].bytes, kAqGamePins[i].size);
    for (size_t i = 0; i < AQ_EXE_PINS; i++) memcpy(g_aqtExe + kAqExePins[i].rva, kAqExePins[i].bytes, kAqExePins[i].size);
    assert(AiQueryChunksMismatch(exe, game) != NULL); /* edge-batch's part of the code still missing */
    for (size_t i = 0; i < EB_PINS; i++) memcpy(g_aqtGame + kEbPins[i].rva, kEbPins[i].bytes, kEbPins[i].size);
    assert(AiQueryChunksMismatch(exe, game) == NULL);
    for (size_t i = 0; i < AQ_GAME_PINS; i++)
    {
        g_aqtGame[kAqGamePins[i].rva + kAqGamePins[i].size - 1] ^= 1;
        assert(AiQueryChunksMismatch(exe, game) == kAqGamePins[i].what);
        g_aqtGame[kAqGamePins[i].rva + kAqGamePins[i].size - 1] ^= 1;
    }
    g_aqtExe[AQ_RUN_JOB + 0x20] ^= 1;
    assert(AiQueryChunksMismatch(exe, game) == kAqExePins[0].what);
    g_aqtExe[AQ_RUN_JOB + 0x20] ^= 1;

    /* refused: switched off, then code not yet decoded and never decoded; nothing is hooked */
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "ai-query-chunks");
    assert(AiQueryChunksTry(exe, game, 0) == -1 && g_aqJoinNext == NULL && !g_aqOn);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    g_aqtGame[AQ_JOIN + 0x40] ^= 1;
    assert(AiQueryChunksTry(exe, game, 0) == 0 && AiQueryChunksTry(exe, game, 1) == -1 && g_aqJoinNext == NULL);
    g_aqtGame[AQ_JOIN + 0x40] ^= 1;

    assert(AiQueryChunksTry(exe, game, 0) == 1 && g_aqOn && g_aqRunNext != NULL && g_aqJoinNext != NULL);
    assert(AiQueryChunksMismatch(exe, game) == NULL); /* the pins read through the hooks */

    /* the stand-ins behind each hook's moved instructions (the overlap task moves 7 bytes, the rest 5) */
    static void* const bodies[AQ_BATCHES] = {(void*)AqtTask0, (void*)AqtTask1, (void*)AqtTask2, (void*)AqtTask3};
    for (int b = 0; b < AQ_BATCHES; b++) VbtFar(g_aqtGame + g_aq[b].rva + (b == 2 ? 7 : 5), bodies[b]);
    VbtFar(g_aqtGame + AQ_JOIN + 5, (void*)AqtJoin);
    VbtFar(g_aqtExe + AQ_RUN_JOB + 5, (void*)AqtRunJob);
    FlushInstructionCache(GetCurrentProcess(), g_aqtGame, AQT_GAME);
    FlushInstructionCache(GetCurrentProcess(), g_aqtExe, AQT_EXE);

    /* the workers held until the join: the game thread runs every chunk itself, each entry once */
    static const uint32_t big[AQ_BATCHES] = {3000, 400, 900, 1100};
    LONG64 stolen = g_aqStolen, chunks = g_aqChunks;
    AqtFrame(big, 0);
    assert(g_aqStolen > stolen && g_aqChunks == chunks);
    assert(g_aqJoins == 1 && g_aqtJoined == 1);

    /* the workers free: chunks shared between them and the game thread, each entry still once */
    for (int frame = 0; frame < 20; frame++) AqtFrame(big, 1);
    assert(g_aqChunks > chunks);

    /* small batches, one entry, none at all (no kick): each entry once */
    static const uint32_t small[AQ_BATCHES] = {5, 1, 30, 0};
    AqtFrame(small, 1);

    /* rows that are not one run from the start: that kick is the game's, its rows run whole */
    LONG left = g_aqLeft;
    memset((void*)g_aqtSeen, 0, sizeof g_aqtSeen);
    ResetEvent(g_aqtGo);
    for (int b = 0; b < AQ_BATCHES; b++) g_aqtRowCount[b] = 0;
    for (uint32_t i = 0; i < AQ_ROWS_MAX; i++) g_aqtRows[1][i][2] = 1;
    uint32_t gap[2][3] = {{0, 10, 0}, {20, 30, 0}};
    memcpy(g_aqtRows[1], gap, sizeof gap);
    g_aqtRowPointers[1][0] = g_aqtRows[1][0];
    g_aqtRowPointers[1][1] = g_aqtRows[1][1];
    g_aqtRowCount[1] = 2;
    ((AqRunJobFn)(void*)(g_aqtExe + AQ_RUN_JOB))((void*)(g_aqtGame + g_aq[1].rva), g_aqtRowPointers[1], "task", 0, 1, 2, NULL);
    assert(g_aqLeft == left + 1 && !g_aq[1].active);
    ((AqJoinFn)(void*)(g_aqtGame + AQ_JOIN))(NULL);
    for (uint32_t i = 0; i < 40; i++) assert(g_aqtSeen[1][i] == (i < 10 || (i >= 20 && i < 30) ? 1 : 0));

    /* another job through the runner (a function that is not one of the four): untouched */
    g_aqtRowCount[1] = 0;
    memset((void*)g_aqtSeen, 0, sizeof g_aqtSeen);
    uint32_t other[3] = {0, 4, 0};
    uint32_t* otherRows[1] = {other};
    SetEvent(g_aqtGo);
    ((AqRunJobFn)(void*)(g_aqtExe + AQ_RUN_JOB))((void*)AqtTask0, otherRows, "other", 0, 1, 1, NULL);
    for (int b = 0; b < AQ_BATCHES; b++) assert(!g_aq[b].active);
    WaitForMultipleObjects((DWORD)g_aqtThreadCount, g_aqtThreads, TRUE, INFINITE);
    for (int i = 0; i < g_aqtThreadCount; i++) CloseHandle(g_aqtThreads[i]);
    g_aqtThreadCount = 0;
    for (uint32_t i = 0; i < 8; i++) assert(g_aqtSeen[0][i] == (i < 4 ? 1 : 0));
    assert(other[2] == 1);
    printf("PASS ai query chunks: pins, the switch, every query of four batches run once over 22 kicks, by the game thread "
        "alone while the workers wait and shared with them (%lld chunks, %lld by the game thread), a kick with a gap and another "
        "job left to the game\n", (long long)(g_aqChunks + g_aqStolen), (long long)g_aqStolen);
}
