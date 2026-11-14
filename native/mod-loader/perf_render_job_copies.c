/* Direct3D 12 records a frame's commands in parallel jobs. Each job's context X takes a copy of the render device's
 * resource-version table (24 bytes an entry, the table's whole size, empty buckets too), and the game never shrinks
 * it: a pool of hundreds of contexts keeps its largest copies for the session. The copy is read only by the job's
 * update pass and recording, so it is given back when the recording returns. Build 25480438; docs/performance.md.
 * Included by hellforge_modloader.c after render_guards.c: AllocateNearGame (damage.c), JournalPut (live_code.c),
 * ApiCodeMatches (hooks.c), PerfFixOn, Note, g_build, g_fired. */

#define RJC_NAME "render-job-copies"
#define RJC_CALL 0x6CA31Cu      /* the job 0x6CA0C0's call of the recording, the only one with a copy of its own */
#define RJC_RECORD 0x6FD1B0u
#define RJC_RESIZE 0x6D55E0u    /* the engine's array resize: to 0, with count 0, it frees the block, data and capacity 0 */
#define RJC_LONG 1024u          /* a shorter copy is 24 KB; all of a pool's together stay small */

/* Every instruction the free relies on, as build 25480438 has it. The tail after the call reads only X's barrier
 * lists (+0x60, +0x78, +0x90), then sets the done flag the dispatch waits on before it empties the copy. */
static const struct { uint32_t rva; const char* hex; } kRjcPins[] = {
    /* the recording: 7 arguments, the 7th at entry rsp+0x38 */
    {0x6FD1B0u, "45 85 C9 0F 84 0D 09 00 00 48 8B C4 44 89 48 20 48 89 50 10 48 89 48 08 55 56 57 41 56 48 81 EC E8 00 00 00 4C 8B 9C 24 40 01 00 00 33 FF 44 8B"},
    {0x6FD1E0u, "8C 24 38 01 00"},
    /* the update pass reads its own job context only */
    {0x6C9FF4u, "48 8B 4B 08 44 8B 0C B0 48 8B 81 B0 03 00 00"},
    /* the job, whole: C = [J+0x70] in rbx and r8 up to the call; after it only X's barrier lists, then the done flag */
    {0x6CA0C0u, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 49 8B 31 4D 8D 71 10 65 48 8B 04 25 58 00 00 00 4D"},
    {0x6CA0F0u, "8B E9 49 8B 59 70 45 8B 49 0C BF D8 01 00 00 48 8B 08 48 8B 46 38 48 89 74 24 40 C7 44 24 50 40 00 00 00 44 8B 14 0F C7 44 24 54 80 00 00 00 C7"},
    {0x6CA120u, "44 24 58 00 01 00 00 48 89 5C 24 48 4E 8B 1C D0 41 F6 C1 40 74 5A 4C 8B 46 68 33 D2 41 8B 8D 94 00 00 00 C7 44 24 60 FF FF FF FF 48 C7 44 24 68"},
    {0x6CA150u, "00 00 00 00 49 8B 40 48 0F 10 44 24 60 48 F7 F1 8B C2 48 8D 0C 40 49 8B 40 08 48 8B 4C C8 08 48 8B 01 49 69 CA B0 00 00 00 48 03 48 08 48 89 4C"},
    {0x6CA180u, "24 78 0F 10 4C 24 70 41 0F 11 06 41 0F 11 4E 10 45 84 C9 79 6F 4C 8B 4E 68 33 D2 65 48 8B 04 25 58 00 00 00 C7 44 24 60 FF FF FF FF 48 C7 44 24"},
    {0x6CA1B0u, "68 00 00 00 00 0F 10 44 24 60 48 8B 08 8B 04 39 41 8B 8D 94 00 00 00 4C 69 C0 B0 00 00 00 49 8B 41 48 48 F7 F1 8B C2 48 8D 0C 40 49 8B 41 08 48"},
    {0x6CA1E0u, "8B 4C C8 08 48 8B 41 08 4C 03 40 08 41 0F 11 46 20 4C 89 44 24 78 0F 10 4C 24 70 41 0F 11 4E 30 45 8B 4D 0C 41 0F BA E1 08 73 6B 4C 8B 4E 68 33"},
    {0x6CA210u, "D2 65 48 8B 04 25 58 00 00 00 C7 44 24 60 FF FF FF FF 48 C7 44 24 68 00 00 00 00 0F 10 44 24 60 48 8B 08 8B 04 39 41 8B 8D 94 00 00 00 4C 69 C0"},
    {0x6CA240u, "B0 00 00 00 49 8B 41 48 48 F7 F1 8B C2 48 8D 0C 40 49 8B 41 08 48 8B 4C C8 08 48 8B 41 10 4C 03 40 08 4C 89 44 24 78 0F 10 4C 24 70 41 0F 11 46"},
    {0x6CA270u, "40 41 0F 11 4E 50 48 8B 83 B0 03 00 00 4C 8B C3 4C 89 98 58 01 00 00 49 8B 85 98 00 00 00 48 89 83 E0 03 00 00 C7 83 14 3D 00 00 FF FF FF FF C6"},
    {0x6CA2A0u, "83 22 3D 00 00 00 41 0F B6 85 C0 00 00 00 88 83 23 3D 00 00 49 8B 85 A8 00 00 00 49 8B 8D B0 00 00 00 41 8B 95 BC 00 00 00 F3 41 0F 10 85 B8 00"},
    {0x6CA2D0u, "00 00 48 89 83 E8 03 00 00 49 8D 85 84 00 00 00 48 89 8B F0 03 00 00 F3 0F 11 83 F8 03 00 00 89 93 FC 03 00 00 49 8B D6 45 8B 8D 80 00 00 00 49"},
    {0x6CA300u, "8B 8D A0 00 00 00 48 89 44 24 30 41 8B 45 08 89 44 24 28 49 8B 45 78 48 89 44 24 20 E8 8F 2E 03 00 45 33 E4 48 8D 6C 24 50 49 83 C6 08 45 8D 7C"},
    {0x6CA330u, "24 60 8B 45 00 41 85 45 0C 0F 84 F0 00 00 00 49 8B 56 10 48 85 D2 75 04 33 DB EB 52 41 83 7E F8 FF 74 05 49 8B 1E EB 46 48 8D 8C 24 C8 00 00 00"},
    {0x6CA360u, "E8 DB 47 03 00 49 8B 7E 10 8B 18 48 8D 4F 10 41 89 5E F8 FF 15 3F C3 D2 00 48 8B 47 48 8B CB 81 E1 FF FF 03 00 48 03 C9 48 8B 1C C8 48 8D 4F 10"},
    {0x6CA390u, "FF 15 2A C3 D2 00 48 8B 74 24 40 49 89 1E 48 8B 44 24 48 48 8B B8 B0 03 00 00 41 8B 14 3F 85 D2 74 19 48 8B 03 48 8B CB 4D 8B 44 3F 08 FF 90 D0"},
    {0x6CA3C0u, "00 00 00 41 C7 04 3F 00 00 00 00 49 83 7E 10 00 74 5D 41 83 7E F8 FF 74 56 49 8B 0E 48 8B 01 FF 50 48 48 8B 4E 68 8B D8 48 8B 89 A8 00 00 00 48"},
    {0x6CA3F0u, "8B 11 FF 92 28 01 00 00 8B D0 85 C0 75 79 85 DB 79 2D 8B CB E8 F7 54 FF FF 48 8B D0 48 8D 0D 75 7E FC 00 E8 C8 72 EF FF 48 8B 15 C1 20 F9 00 41"},
    {0x6CA420u, "B1 01 4C 8B C0 B9 03 00 00 00 E8 81 EA EE FF 41 FF C4 49 83 C7 18 48 83 C5 04 49 83 C6 20 41 83 FC 03 0F 82 EA FE FF FF 4C 8D 9C 24 80 00 00 00"},
    {0x6CA450u, "90 49 8B 5B 30 33 C0 49 8B 6B 38 49 8B 73 40 41 C7 85 90 00 00 00 01 00 00 00 49 8B E3 41 5F 41 5E 41 5D 41 5C 5F C3 48 8B 4E 68 48 81 C1 A8 00"},
    {0x6CA480u, "00 00 E8 49 6A 9C FF 48 8B C8 E8 01 57 FF FF CC"},
    /* the feeder: the recording jobs depend on the update jobs; only these are released */
    {0x6CBE0Bu, "48 8B 46 20 4C 8B 74 24 48 44 8B 08 45 85 C9 74 2D 48 8B 4E 30 4D 8B C6 48 8B 40 08 8B 95 A8 08 00 00 48 89 44 24 20 48 8B 89 F8 00 00 00 E8 12"},
    {0x6CBE3Bu, "88 F5 FF 85 FF 74 20 4D 8B C5 8B D7 EB 09 85 DB 74 15 4D 8B C6 8B D3 48 8B 4E 30 48 8B 89 F8 00 00 00 E8 6E 8B F5 FF"},
    /* the dispatch: a fresh context from the pool, stored at C_n+0x3B0 */
    {0x6CC700u, "48 8D 8F F8 06 00 00 E8 D4 44 FF FF 48 8B 8F 08 07 00 00"},
    {0x6CC87Bu, "49 8B 83 08 07 00 00 41 0F 10 83 60 07 00 00 48 8B 14 C8"},
    {0x6CC8BEu, "48 89 96 B0 03 00 00"},
    /* the dispatch: J+0x70 = C_n, the copy into X_n (grown to the table's size first), the done flag cleared */
    {0x6CD171u, "4C 89 34 C8 FF 45 38 4D 89 7E 70 49 8B 9F B0 03 00 00 41 8B B1 C8 13 00 00 3B 73 0C 76 12 8B D6 48 8D 4B 08 E8 46 84 00 00 4C 8B 8D 00 4B 00 00"},
    {0x6CD1A1u, "8B 43 08 45 33 D2 3B C6 73 25 0F 1F 44 00 00 8B 43 08 48 8D 0C 40 48 8B 43 10 44 89 14 C8 4C 89 54 C8 08 FF 43 08 8B 43 08 3B F0 77 E2 3B C6 76"},
    {0x6CD1D1u, "18 B9 FF FF FF FF 66 0F 1F 84 00 00 00 00 00 01 4B 08 8B 43 08 3B C6 77 F6 45 8B C2 85 C0 74 2F 48 8B 4B 10 41 8B C0 41 FF C0 48 8D 14 40 49 8B"},
    {0x6CD201u, "81 D0 13 00 00 0F 10 04 D0 0F 11 04 D1 F2 0F 10 4C D0 10 F2 0F 11 4C D1 10 44 3B 43 08 72 D1 41 8B 81 E0 13 00 00 0F 57 C0 89 43 20 41 8B 81 E4"},
    {0x6CD231u, "13 00 00 89 43 24 41 8B 81 E8 13 00 00 89 43 28 41 8B 81 EC 13 00 00 89 43 2C 48 8D 85 00 04 00 00 8B 55 C8 44 8B BD 10 4B 00 00 49 89 86 A0 00"},
    {0x6CD261u, "00 00 8B 45 D0 41 89 46 0C 48 8D 0C 52 45 89 7E 08 49 8B 81 70 09 00 00 49 89 06 41 0F 11 46 10 C1 E2 0C 41 0F 11 46 20 41 0F 11 46 30 41 0F 11"},
    {0x6CD291u, "46 40 41 0F 11 46 50 41 0F 11 46 60 49 8B 81 D0 05 00 00 45 89 AE 80 00 00 00 48 8D 0C C8 49 89 4E 78 48 8D 8D 20 05 00 00 41 8B 81 C0 13 00 00"},
    {0x6CD2C1u, "25 FF 0F 00 00 49 89 BE 88 00 00 00 0B C2 48 8D 3D 3A D8 98 FF 41 89 86 84 00 00 00 48 8D 95 30 03 00 00 90 45 89 96 90 00 00 00"},
    /* the dispatch: the job function 0x6CA0C0, the update pass's data holding C_n, the update pass 0x6C9FD0 */
    {0x6CD36Fu, "48 8D 05 4A CD FF FF"},
    {0x6CD3E4u, "49 8B 86 88 00 00 00 48 89 01 49 8B 46 70 48 89 41 08"},
    {0x6CD422u, "48 8D 05 A7 CB FF FF"},
    /* the dispatch: the wait on each done flag, then the copy emptied */
    {0x6CD5C5u, "48 8B 5D 40 41 BD 02 00 00 00 8B 45 38 4C 8B A5 00 4B 00 00 4C 8D 34 C3 49 3B DE 74 72 48 8B 33 45 8B CD 49 8B 8C 24 20 05 00 00 48 8D 96 90 00"},
    {0x6CD5F5u, "00 00 E8 64 9B F5 FF 48 8B BE 88 00 00 00 48 85 FF 74 2F 48 8B CF E8 F0 06 9C FF 48 8B 85 20 05 00 00 4C 8B 48 40 48 8D 05 7E D5 98 FF 4C 3B C8"},
    {0x6CD625u, "74 10 45 33 C0 48 8D 8D 20 05 00 00 48 8B D7 41 FF D1 48 8B 46 70 48 8B 88 B0 03 00 00 48 83 C1 08 E8 15 7F 00 00 48 83 C3 08 49 3B DE 75 8E"},
    /* the resize to 0 with count 0: the block freed through the array's allocator, data and capacity 0 */
    {0x6D55E0u, "48 89 5C 24 10 57 48 83 EC 30 48 8B D9 8B FA 3B 51 04 74 68 48 89 74 24 40 3B 39 73 07 8B D7 E8 6C 5B 00 00 33 F6 85 FF 74 35 48 8B 4B 10 4C 8D"},
    {0x6D5610u, "04 7F 49 C1 E0 03 44 8D 4E 08 48 8D 54 24 20 4C 8B 11 41 FF 52 30 48 8B 53 08 48 8B 30 8B 03 48 8B CE 4C 8D 04 40 49 C1 E0 03 E8 11 22 B9 00 48"},
    {0x6D5640u, "8B 4B 10 45 33 C0 48 8B 53 08 48 8B 01 FF 50 40 48 89 73 08 48 8B 74 24 40 89 7B 04 48 8B 5C 24 48 48 83 C4 30 5F C3"},
    /* the empty: with count 0 it reads no entry */
    {0x6D5560u, "48 83 EC 18 44 8B 01 45 33 D2 41 8B C0 44 89 51 18 2B 41 1C 41 8B D2 C7 41 24 FF FF FF FF 89 41 20 45 85 C0 74 4E"},
    /* the reader: a copy with used 0 sends the lookup to the device's own table */
    {0x6E2230u, "83 79 20 00 44 8B CA 4C 8B C1 48 C7 44 24 08 00 00 00 00 75 62"},
};
#define RJC_PINS (sizeof kRjcPins / sizeof kRjcPins[0])

typedef uint64_t (*RjcRecordFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef void (*RjcResizeFn)(uintptr_t array, uint32_t n);

static RjcRecordFn g_rjcRecord;
static RjcResizeFn g_rjcResize;
static int g_rjcState; /* 0 waiting for the exe's code, 1 installed, -1 not */
static ULONGLONG g_rjcLoadedAt;
static volatile LONG g_rjcFreed, g_rjcLargest;
static volatile LONG64 g_rjcBytes;
static LONG g_rjcReportedLargest;

static int RjcPinned(uintptr_t exe)
{
    for (size_t i = 0; i < RJC_PINS; i++)
        if (!ApiCodeMatches(exe + kRjcPins[i].rva, kRjcPins[i].hex)) return 0;
    return 1;
}

/* On the job's worker thread, its recording done. The map at X+8: count +0, capacity +4, data +8, used +0x18. Used 0
 * first, as after the dispatch's own empty: the reader then looks at no copy. Count 0 spares the resize its one-by-one
 * shrink. The copy's proxy allocator keeps atomic counters (0x5C2ED0); the recording allocates through it on workers. */
static void RjcFree(uintptr_t context)
{
    uintptr_t x = *(volatile uintptr_t*)(context + 0x3B0);
    uint32_t capacity = x != 0 ? *(volatile uint32_t*)(x + 0xC) : 0;
    if (capacity < RJC_LONG) return;
    DWORD error = GetLastError(); /* the game's code around the call may read it */
    *(volatile uint32_t*)(x + 0x20) = 0;
    *(volatile uint32_t*)(x + 8) = 0;
    g_rjcResize(x + 8, 0);
    *(volatile uint32_t*)(x + 0x28) = 0;           /* spill and free list as an empty map's, until the next copy */
    *(volatile uint32_t*)(x + 0x2C) = 0xFFFFFFFFu;
    InterlockedIncrement(&g_rjcFreed);
    InterlockedAdd64(&g_rjcBytes, (LONG64)capacity * 24);
    LONG largest;
    while ((largest = g_rjcLargest) < (LONG)capacity && InterlockedCompareExchange(&g_rjcLargest, (LONG)capacity, largest) != largest) {}
    SetLastError(error);
}

/* The patched call lands here with the game's stack (return 0x6CA321, arguments 5-7 above it): the recording runs
 * through 0x6FD1B0, so a mod's hook on it still runs, then the job's copy goes. */
static uint64_t RjcRecord(uint64_t device, uint64_t lists, uint64_t context, uint64_t count, uint64_t e, uint64_t f, uint64_t g)
{
    uint64_t result = g_rjcRecord(device, lists, context, count, e, f, g);
    RjcFree((uintptr_t)context);
    return result;
}

/* A page for the jump to RjcRecord that the call's rel32 reaches, or NULL. */
static uint8_t* RjcCavePage(uintptr_t exe)
{
    uint8_t* cave = AllocateNearGame(exe, 0x1000);
    if (cave == NULL) return NULL;
    int64_t distance = (int64_t)((uintptr_t)cave - (exe + RJC_CALL + 5));
    return distance < 0x7FFF0000 && distance > -0x7FFF0000 ? cave : NULL; /* one unused page kept: it may be the shared arena's */
}

/* After the pins matched: the jump, then the call's rel32 written with no thread in it. 1 on, -1 not. */
static int RenderJobCopiesInstallAt(uintptr_t exe)
{
    if (!PerfFixOn(RJC_NAME)) return -1;
    uint8_t* cave = RjcCavePage(exe);
    if (cave == NULL) { Note("render job copies: not installed; no memory near the game's exe"); return -1; }
    g_rjcRecord = (RjcRecordFn)(exe + RJC_RECORD);
    g_rjcResize = (RjcResizeFn)(exe + RJC_RESIZE);
    static const uint8_t jump[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}; /* jmp [rip+0], the address after it */
    uint64_t to = (uint64_t)(uintptr_t)RjcRecord;
    memcpy(cave, jump, sizeof jump);
    memcpy(cave + sizeof jump, &to, 8);
    DWORD previous;
    VirtualProtect(cave, 0x1000, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), cave, 0x1000);
    uint8_t call[5] = {0xE8};
    int32_t rel = (int32_t)((intptr_t)cave - (intptr_t)(exe + RJC_CALL + 5));
    memcpy(call + 1, &rel, 4);
    if (JournalPut(exe + RJC_CALL, call, sizeof call)) { Note("render job copies: not installed; the job's code could not be rewritten"); return -1; }
    Note("render job copies: a render job's version copy of %u entries or more is given back when its recording returns", RJC_LONG);
    return 1;
}

/* From the watch thread. The exe's code is decoded some time after start; a minute after the game loaded, it never
 * will be the reviewed one. */
static void RenderJobCopiesInstall(void)
{
    if (g_rjcState != 0) return;
    if (strcmp(g_build, "25480438")) { g_rjcState = -1; Note("render job copies: not installed; build %s is not the reviewed one", g_build); return; }
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (!RjcPinned(exe))
    {
        if (!g_fired[HF_EVENT_GAME_LOADED]) return;
        if (g_rjcLoadedAt == 0) g_rjcLoadedAt = GetTickCount64();
        if (GetTickCount64() - g_rjcLoadedAt < 60000) return;
        g_rjcState = -1;
        Note("render job copies: not installed; the render jobs' code is not the reviewed one");
        return;
    }
    g_rjcState = RenderJobCopiesInstallAt(exe);
}

/* A line each time a longer copy is given back: the table's size is what the fix saves per context. */
static void RenderJobCopiesReport(void)
{
    LONG largest = g_rjcLargest;
    if (g_rjcState != 1 || largest <= g_rjcReportedLargest) return;
    g_rjcReportedLargest = largest;
    Note("render job copies: copies of up to %ld entries given back; %ld copies, %.1f MB so far", (long)largest, (long)g_rjcFreed,
        (double)g_rjcBytes / 1048576.0);
}

static void RenderJobCopiesWatch(void)
{
    RenderJobCopiesInstall();
    RenderJobCopiesReport();
}
