/* render-version-rehash (docs/performance.md). The D3D12 device keeps a hash table of dynamic buffer id to version
 * (device +0x13C8) and one of the ids changed since the last present (+0x13F0). Growth rehashes them and nothing
 * shrinks them, so a session's largest burst, a level load, sets their size for good: 56,114 entries after a 16 km map
 * with about 120 in use. Every recording job copies and empties the whole versions table each frame, empty buckets
 * too. Included by hellforge_modloader.c after render_guards.c: ApiCodeMatches, LoaderHook, PerfFixOn, Note. */

#define RVR_NAME "render-version-rehash"
#define RVR_DISPATCH 0x6CECB0u /* D3D12RenderDevice vtable +0xF0: a batch's commands, then the batch end 0x6F0240 */
#define RVR_REHASH 0x6F60F0u   /* (map, buckets): a new array holding every live entry */
#define RVR_POOL 0x1340u       /* the device's dynamic buffer pool: the versions at +0x88, the changed ids at +0xB0 */
#define RVR_LONG 4096u         /* shorter tables cost too little to touch */
#define RVR_WINDOW_MS 1000u    /* a table is sized for its peak over the last one to two of these */

/* Every byte the shrink relies on or runs, as build 25480438 has it. */
typedef struct RvrPin
{
    uint32_t rva;
    const char* hex;
} RvrPin;

static const RvrPin kRvrPins[] = {
    /* the dispatch, all of it: rcx the device, five arguments, the batch end its last call */
    {0x6CECB0u,
        "48 89 7C 24 18 4C 89 74 24 20 41 57 48 83 EC 30 4D 8B C8 48 8B F9 44 8B C2 48 8D 91 C8 05 00 00 E8 6B 82 E5 "
        "FF 44 8B BF C8 05 00 00 45 85 FF 0F 84 CD 00 00 00 48 89 5C 24 40 8B 5C 24 60 D1 EB 48 89 74 24 48 80 E3 01 "
        "8B 74 24 60 83 E6 04 75 18 48 8B 97 58 14 00 00 4C 8D 87 C8 05 00 00 48 8D 4C 24 20 E8 A7 77 21 00 F6 44 24 "
        "60 01 8B 8F 30 14 00 00 48 8B 87 68 14 00 00 44 8B 04 C8 74 1D 81 BF C8 05 00 00 00 01 00 00 72 11 44 0F B6 "
        "CB 41 8B D7 48 8B CF E8 E4 D3 FF FF EB 0F 44 0F B6 CB 41 8B D7 48 8B CF E8 93 B8 FF FF FF 87 C0 13 00 00 85 "
        "F6 C6 87 84 14 00 00 01 48 8B 74 24 48 75 2E 4C 8B 87 58 14 00 00 49 8B 48 08 49 8D 98 E0 00 00 00 4D 8B 80 "
        "E8 00 00 00 8B 13 48 8B 01 FF 90 E0 00 00 00 33 D2 48 8B CB E8 5F E3 A4 FF 48 8D 8F 40 13 00 00 E8 93 14 02 "
        "00 48 8B 5C"},
    {0x6CEDB0u, "24 40 48 8B 7C 24 50 4C 8B 74 24 58 48 83 C4 30 41 5F C3"},
    /* the batch end: the changed ids at pool +0xB0, the versions at pool +0x88 */
    {0x6F0240u,
        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 30 44 8B B1 B0 00 00 00 33 DB 48 8B F1 45 85 F6 74 1E 41 "
        "3B DE 73 12 48 8B 86 B8 00 00 00 48 8D 0C 5B 83 7C C8 10 00 7D 07 FF C3 41 3B DE 72 E7 4C 89 7C 24 50 41 3B "
        "DE 72 06 0F 86 B0 00 00 00 48 8B 86 B8 00 00 00 48 8D 0C 5B 48 8D 14 C8 8B 04 C8 48 8D 8E 88 00 00 00 89 44 "
        "24 20 48 8B 42 08 48 8D 54 24 20 48 89 44 24 28 E8 5F 3F 00 00"},
    /* the rehash */
    {0x6F60F0u,
        "40 55 48 8B EC 48 83 EC 50 8B C2 0F 57 C0 48 89 74 24 68 33 F6 89 55 EC 48 89 7C 24 70 48 8B F9 F3 48 0F 2A "
        "C0 48 8B 41 10 48 8D 4D D0 48 89 75 D0 48 89 75 D8 48 89 45 E0 89 75 E8 C7 45 F4 FF FF FF FF F3 0F 59 05 55 "
        "38 FB 00 F3 0F 58 05 C5 38 FB 00 F3 44 0F 2C C0 41 03 D0 44 89 45 F0 E8 1C 50 FE FF 8B D6 39 75 D0 76 1E 0F "
        "1F 44 00 00 8B C2 FF C2 48 8D 0C 40 48 8B 45 D8 C7 44 C8 10 FE FF FF FF 3B 55 D0 72 E7 83 7F 24 FF 74 2B 90 "
        "44 8B 47 24 48 8B 57 08 41 0F BA F0 1F 4B 8D 04 40 8B 4C C2 10 4B 8D 04 40 89 4F 24 C7 44 C2 10 FE FF FF FF "
        "83 7F 24 FF 75 D6 44 8B 07 45 85 C0 74 42 48 89 5C 24 60 66 0F 1F 84 00 00 00 00 00 8B C6 48 8D 0C 40 48 8B "
        "47 08 83 7C C8 10 FE 48 8D 14 C8 74 10 48 8B 5A 08 48 8D 4D D0 E8 3E E0 FF FF 48 89 18 44 8B 07 FF C6 41 3B "
        "F0 72 D1 48"},
    {0x6F61F0u,
        "8B 5C 24 60 8B 45 D0 8B 57 04 8B 4F 18 4C 8B 57 08 4C 8B 4F 10 48 8B 74 24 68 89 07 8B 45 D4 89 47 04 48 8B "
        "45 D8 48 89 47 08 48 8B 45 E0 48 89 47 10 8B 45 E8 89 47 18 8B 45 EC 89 4D E8 8B 4F 1C 89 47 1C 8B 45 F0 89 "
        "4D EC 8B 4F 20 89 47 20 8B 45 F4 89 4D F0 8B 4F 24 89 47 24 48 8B 7C 24 70 44 89 45 D0 89 55 D4 4C 89 55 D8 "
        "4C 89 4D E0 89 4D F4 85 D2 74 22 45 85 C0 74 0E 0F 1F 40 00 41 83 C0 FF 75 FA 44 89 45 D0 49 8B 01 45 33 C0 "
        "49 8B D2 49 8B C9 FF 50 40 48 83 C4 50 5D C3"},
    /* the insert, which grows a full table through the rehash */
    {0x6F4220u,
        "48 89 5C 24 10 57 48 83 EC 20 83 79 20 00 48 8B FA 48 8B D9 75 52 83 79 24 FF 75 4C E8 AF 0F 00 00 3D FF FF "
        "FF 7F 74 0B 8B C0 48 8D 0C 40 E9 2F 01 00 00 48 89 74 24 30 BE 13 00 00 00 0F 1F 00 8B 43 1C 48 8B CB 8D 14 "
        "45 01 00 00 00 3B D6 0F 42 D6 E8 79 1E 00 00 48 8B CB E8 81 0B 00 00 84 C0 75 DD 48 8B 74 24 30 44 8B 1F 33 "
        "D2 4C 8B 53 08 41 69 C3 95 E9 D1 5B 8B C8 C1 E9 18 33 C8 69 C1 95 E9 D1 5B F7 73 1C 44 8B C2 44 8B CA 48 8D "
        "04 52 48 8D 14 C5 00 00 00 00 42 83 7C 12 10 FE 4A 8D 04 12 75 18 C7 40 10 FF FF FF 7F 8B 07 48 8B 4B 08 89 "
        "04 0A FF 43 18 E9 A0 00 00 00 44 3B 18 0F 84 97 00 00 00 66 0F 1F 84 00 00 00 00 00 46 8B 44 12 10 41 81 F8 "
        "FF FF FF 7F 74 17 4B 8D 04 40 45 8B C8 48 8D 14 C5 00 00 00 00 46 3B 1C 12 75 DD EB 69 FF 43 18 44 8B 43 24 "
        "41 83 F8 FF"},
    {0x6F4320u,
        "74 13 41 0F BA F0 1F 4B 8D 0C 40 41 8B 44 CA 10 89 43 24 EB 1F 8B 43 20 44 8B 03 44 2B C0 FF C8 89 43 20 4B "
        "8D 0C 40 41 C7 44 CA 10 FE FF FF FF 4C 8B 53 08 4B 8D 04 49 45 89 44 C2 10 4B 8D 0C 40 8B 07 48 8D 14 CD 00 "
        "00 00 00 48 8B 4B 08 89 04 0A 48 8B 43 08 C7 44 02 10 FF FF FF 7F 4B 8D 0C 40 48 8B 43 08 48 8B 5C 24 38 48 "
        "8D 04 C8 48 83 C0 08 48 83 C4 20 5F C3"},
    /* the rehash's resize */
    {0x6DB170u,
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 41 04 48 8B D9 8B FA BE FF FF FF FF 3B D0 76 23 48 8D 0C 45 "
        "0A 00 00 00 8B C7 48 3B CF 72 0A 48 3B CE 48 8B C1 48 0F 47 C6 8B D0 48 8B CB E8 2D A4 FF FF 8B 03 3B C7 73 "
        "23 33 D2 0F 1F 44 00 00 8B 03 48 8D 0C 40 48 8B 43 08 89 14 C8 48 89 54 C8 08 FF 03 8B 03 3B F8 77 E6 3B C7 "
        "76 08 66 90 01 33 39 3B 77 FA 48 8B 5C 24 30 48 8B 74 24 38 48 83 C4 20 5F C3"},
    /* its reallocation */
    {0x6D55E0u,
        "48 89 5C 24 10 57 48 83 EC 30 48 8B D9 8B FA 3B 51 04 74 68 48 89 74 24 40 3B 39 73 07 8B D7 E8 6C 5B 00 00 "
        "33 F6 85 FF 74 35 48 8B 4B 10 4C 8D 04 7F 49 C1 E0 03 44 8D 4E 08 48 8D 54 24 20 4C 8B 11 41 FF 52 30 48 8B "
        "53 08 48 8B 30 8B 03 48 8B CE 4C 8D 04 40 49 C1 E0 03 E8 11 22 B9 00 48 8B 4B 10 45 33 C0 48 8B 53 08 48 8B "
        "01 FF 50 40 48 89 73 08 48 8B 74 24 40 89 7B 04 48 8B 5C 24 48 48 83 C4 30 5F C3"},
    /* the insert's find */
    {0x6F51F0u,
        "83 79 18 00 74 56 44 8B 02 4C 8B 49 08 41 69 C0 95 E9 D1 5B 8B D0 C1 EA 18 33 D0 69 C2 95 E9 D1 5B 33 D2 F7 "
        "71 1C 48 8D 0C 52 41 83 7C C9 10 FE 74 2A 81 FA FF FF FF 7F 74 1F 66 0F 1F 44 00 00 8B C2 48 8D 0C 40 45 3B "
        "04 C9 74 0D 41 8B 54 C9 10 81 FA FF FF FF 7F 75 E7 8B C2 C3 B8 FF FF FF 7F C3"},
    {0x6F4E00u, "83 79 20 00 75 09 83 79 24 FF 75 03 B0 01 C3 32 C0 C3"}, /* its full test */
    {0x16A9990u, "A4 70 BD 3E"}, /* the rehash's 0.37 and 1.0 */
    {0x16A9A08u, "00 00 80 3F"},
};
#define RVR_PINS (sizeof kRvrPins / sizeof kRvrPins[0])

typedef uint64_t (*RvrDispatchFn)(uint64_t device, uint64_t count, uint64_t commands, uint64_t r9, uint64_t flags);
typedef void (*RvrRehashFn)(uintptr_t map, uint32_t buckets);

static int g_rvrState; /* 0 not asked, 2 waiting for the exe's code, 1 installed, -1 not */
static ULONGLONG g_rvrLoadedAt, g_rvrReportedAt, g_rvrWindowAt;
static RvrDispatchFn g_rvrOriginal;
static uintptr_t g_rvrRehash;
static uint32_t g_rvrPeak[2][2]; /* per table: the most entries in use this window and the last */
static volatile LONG g_rvrShrinks, g_rvrFrom, g_rvrTo;
static LONG g_rvrReported;
static ULONGLONG (WINAPI *g_rvrClock)(void) = GetTickCount64; /* a fixture steps it */

static int RvrPinned(uintptr_t exe)
{
    for (size_t i = 0; i < RVR_PINS; i++)
        if (!ApiCodeMatches(exe + kRvrPins[i].rva, kRvrPins[i].hex)) return 0;
    return 1;
}

/* A table: size +0, used +0x18, buckets +0x1C. With 4 buckets for each entry of its recent peak it regrows only for
 * a burst about 4 times larger, and the 1/8 rule leaves the regrown table alone until that burst is out of the window. */
static void RvrShrink(uintptr_t map, uint32_t* peak)
{
    uint32_t size = *(volatile uint32_t*)map, used = *(volatile uint32_t*)(map + 0x18), buckets = *(volatile uint32_t*)(map + 0x1C);
    if (used > peak[0]) peak[0] = used;
    uint32_t most = peak[0] > peak[1] ? peak[0] : peak[1];
    if (size < RVR_LONG || most > buckets / 8) return;
    ((RvrRehashFn)g_rvrRehash)(map, most * 4 + 19);
    InterlockedExchange(&g_rvrFrom, (LONG)size);
    InterlockedExchange(&g_rvrTo, (LONG)*(volatile uint32_t*)map);
    InterlockedIncrement(&g_rvrShrinks);
}

/* The dispatch already has the tables to itself: it inserts into them (and grows them) and its batch end frees their
 * version nodes, with no lock. Its callers give it that: an ordered device job at the list's head, or the renderer
 * after it drained that list. So the rehash runs on its thread, after the batch end, before the call returns. */
static uint64_t RvrDispatch(uint64_t device, uint64_t count, uint64_t commands, uint64_t r9, uint64_t flags)
{
    uint64_t result = g_rvrOriginal(device, count, commands, r9, flags);
    ULONGLONG now = g_rvrClock();
    if (now - g_rvrWindowAt >= RVR_WINDOW_MS)
    {
        g_rvrWindowAt = now;
        for (int t = 0; t < 2; t++) { g_rvrPeak[t][1] = g_rvrPeak[t][0]; g_rvrPeak[t][0] = 0; }
    }
    RvrShrink((uintptr_t)device + RVR_POOL + 0x88, g_rvrPeak[0]);
    RvrShrink((uintptr_t)device + RVR_POOL + 0xB0, g_rvrPeak[1]);
    return result;
}

/* giveUp: the code will not change any more, so unpinned bytes are another build's or another tool's. */
static void RvrInstallPolled(uintptr_t exe, int giveUp)
{
    if (g_rvrState == 0) g_rvrState = PerfFixOn(RVR_NAME) ? 2 : -1;
    if (g_rvrState != 2) return;
    if (!RvrPinned(exe))
    {
        if (!giveUp) return;
        g_rvrState = -1;
        Note("render version rehash: not installed; the renderer's code is not the reviewed one");
        return;
    }
    g_rvrRehash = exe + RVR_REHASH;
    if (LoaderHook("render version rehash", exe + RVR_DISPATCH, (void*)RvrDispatch, (void**)&g_rvrOriginal) || g_rvrOriginal == NULL)
    {
        g_rvrState = -1;
        Note("render version rehash: not installed; the D3D12 dispatch could not be hooked");
        return;
    }
    g_rvrState = 1;
    Note("render version rehash: on; D3D12 version tables of %u entries or more, under an eighth used, are rehashed short", RVR_LONG);
}

/* From the watch thread, like the render guards: the exe's code is decoded some time after start, and a minute after
 * the game loaded it never will be the reviewed one. Then what was rehashed, at most every 10 s. */
static void RenderVersionRehashWatch(void)
{
    if (g_rvrState == 0 && strcmp(g_build, "25480438"))
    {
        g_rvrState = -1;
        Note("render version rehash: not installed; it is reviewed for build 25480438 only");
    }
    if (g_rvrState == 0 || g_rvrState == 2)
    {
        if (g_fired[HF_EVENT_GAME_LOADED] && g_rvrLoadedAt == 0) g_rvrLoadedAt = GetTickCount64();
        RvrInstallPolled((uintptr_t)GetModuleHandleW(NULL), g_rvrLoadedAt != 0 && GetTickCount64() - g_rvrLoadedAt >= 60000);
    }
    LONG shrinks = g_rvrShrinks;
    if (g_rvrState != 1 || shrinks == g_rvrReported || GetTickCount64() - g_rvrReportedAt < 10000) return;
    g_rvrReported = shrinks;
    g_rvrReportedAt = GetTickCount64();
    Note("render version rehash: %ld rehash(es) so far; the last took a table from %ld to %ld entries", (long)shrinks, (long)g_rvrFrom, (long)g_rvrTo);
}
