/* render-wait-help (docs/performance.md). The engine's job system puts a job in channel affinity * 3 + priority, and a
 * thread that waits runs queued jobs meanwhile, from its own list of channels: three a priority on the render thread
 * (its own, any thread's, render-affine) but only the first 2 * (priority + 1) of them. So the renderer waiting at
 * priority 0 never takes its render-affine jobs (channel 6), and at priority 1 never channel 7, where the culling
 * chain it waits for runs. HellForge takes 3 * (priority + 1), at most the 6 its idle loop already polls, in the poll
 * and in the counter wait. Included by hellforge_modloader.c after render_guards.c: ApiCodeMatches, JournalPut. */

#define RWH_NAME "render-wait-help"
#define RWH_POLL_SITE 0x626942u /* the poll's render-thread branch: its count and channel list */
#define RWH_POLL_LENGTH 46
#define RWH_WAIT_SITE 0x6271BAu /* the counter wait's render-thread branch, run again on each pass */
#define RWH_WAIT_LENGTH 54

typedef struct RwhPin
{
    uint32_t rva;
    const char* hex;
} RwhPin;

/* The poll (jobs, priority) and the counter wait (jobs, counter, _, priority), whole: the thread index in TLS +0x1D8
 * picks the list, the job pop is 0x624D00 (jobs +0xF8, channel). Then a job's channel and the render thread's index 1. */
static const RwhPin kRwhPins[] = {
    {0x626900u,
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 55 41 56 41 57 48 8B EC 48 83 EC 30 65 48 8B 04 25 58 00 00 00 "
        "48 8B F1 B9 D8 01 00 00 48 8B 00 44 8B 04 01 8B 05 AB 9F D3 01 83 C0 02 41 83 F8 01 75 6A 44 8D 34 55 02 00 "
        "00 00 C7 45 F0 12 00 06 13 33 DB C7 45 F4 01 07 14 02 C6 45 F8 08 45 85 F6 74 2E 48 8D 7D F0 66 0F 1F 84 00 "
        "00 00 00 00 0F B6 17 48 8B 8E F8 00 00 00 E8 81 E3 FF FF 85 C0 0F 85 9B 01 00 00 FF C3 48 FF C7 41 3B DE 72 "
        "DF 32 C0 48 8B 5C 24 50 48 8B 74 24 58 48 8B 7C 24 68 48 83 C4 30 41 5F 41 5E 5D C3 45 85 C0 75 52 44 8D 34 "
        "55 02 00 00 00 C7 45 F0 0F 00 03 10 33 DB C7 45 F4 01 04 11 02 C6 45 F8 05 45 85 F6 74 BF 48 8D 7D F0 66 66 "
        "0F 1F 84 00 00 00 00 00 0F B6 17 48 8B 8E F8 00 00 00 E8 11 E3 FF FF 85 C0 0F 85 2B 01 00 00 FF C3 48 FF C7 "
        "41 3B DE 72"},
    {0x626A00u,
        "DF EB 8E 44 3B C0 0F 83 D6 00 00 00 0F B6 05 D6 CB 4E 01 44 8D 34 95 04 00 00 00 84 C0 74 77 8B 0D BF 9E D3 "
        "01 B8 01 00 00 00 FF C1 D1 E9 3B C8 0F 42 C8 41 8D 40 FE 3B C1 72 5B 41 BF 08 00 00 00 C7 45 30 09 00 03 06 "
        "45 3B F7 C7 45 34 0A 01 04 07 45 0F 42 FE 33 DB 45 85 FF 0F 84 30 FF FF FF 48 8D 7D 30 66 66 66 0F 1F 84 00 "
        "00 00 00 00 0F B6 17 48 8B 8E F8 00 00 00 E8 81 E2 FF FF 85 C0 0F 85 9B 00 00 00 FF C3 48 FF C7 41 3B DF 72 "
        "DF E9 FB FE FF FF 33 DB C7 45 F0 09 00 03 06 C7 45 F4 0A 01 04 07 C7 45 F8 0B 02 05 08 45 85 F6 0F 84 DB FE "
        "FF FF 48 8D 7D F0 66 0F 1F 44 00 00 0F B6 17 48 8B 8E F8 00 00 00 E8 31 E2 FF FF 85 C0 75 4F FF C3 48 FF C7 "
        "41 3B DE 72 E3 E9 AF FE FF FF 44 8D 72 01 66 C7 45 30 0C 0D 33 DB C6 45 32 0E 45 85 F6 0F 84 96 FE FF FF 48 "
        "8D 7D 30 90"},
    {0x626B00u,
        "0F B6 17 48 8B 8E F8 00 00 00 E8 F1 E1 FF FF 85 C0 75 0F FF C3 48 FF C7 41 3B DE 72 E3 E9 6F FE FF FF B0 01 "
        "E9 6A FE FF FF"},
    /* the counter wait */
    {0x627160u,
        "48 89 5C 24 08 44 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 50 45 8B E9 4C 8B E2 4C 8B "
        "F9 41 B8 01 00 00 00 B9 D8 01 00 00 41 8B 04 24 90 85 C0 0F 85 25 02 00 00 65 48 8B 04 25 58 00 00 00 48 8B "
        "00 8B 14 01 8B 05 32 97 D3 01 83 C0 02 83 FA 01 75 64 42 8D 34 6D 02 00 00 00 C7 45 D0 12 00 06 13 33 DB C7 "
        "45 D4 01 07 14 02 C6 45 D8 08 85 F6 0F 84 DD 01 00 00 48 8D 7D D0 0F 1F 40 00 66 66 0F 1F 84 00 00 00 00 00 "
        "0F B6 17 49 8B 8F F8 00 00 00 E8 01 DB FF FF B9 D8 01 00 00 41 B8 01 00 00 00 85 C0 75 82 FF C3 48 FF C7 3B "
        "DE 72 D9 F3 90 E9 6D FF FF FF 85 D2 75 60 42 8D 34 6D 02 00 00 00 C7 45 E0 0F 00 03 10 33 DB C7 45 E4 01 04 "
        "11 02 C6 45 E8 05 85 F6 0F 84 75 01 00 00 48 8D 7D E0 66 0F 1F 44 00 00 0F B6 17 49 8B 8F F8 00 00 00 E8 A1 "
        "DA FF FF B9"},
    {0x627260u,
        "D8 01 00 00 41 B8 01 00 00 00 85 C0 0F 85 1E FF FF FF FF C3 48 FF C7 3B DE 72 D5 F3 90 E9 09 FF FF FF 3B D0 "
        "0F 83 E8 00 00 00 0F B6 05 58 C3 4E 01 42 8D 34 AD 04 00 00 00 84 C0 74 76 8B 0D 41 96 D3 01 8D 42 FE FF C1 "
        "D1 E9 83 F9 01 41 0F 42 C8 3B C1 72 5E 41 BE 08 00 00 00 C7 45 48 09 00 03 06 41 3B F6 C7 45 4C 0A 01 04 07 "
        "44 0F 42 F6 33 DB 45 85 F6 0F 84 E0 00 00 00 48 8D 7D 48 90 0F B6 17 49 8B 8F F8 00 00 00 E8 11 DA FF FF B9 "
        "D8 01 00 00 41 B8 01 00 00 00 85 C0 0F 85 8E FE FF FF FF C3 48 FF C7 41 3B DE 72 D4 F3 90 E9 78 FE FF FF 33 "
        "DB C7 45 F0 09 00 03 06 C7 45 F4 0A 01 04 07 C7 45 F8 0B 02 05 08 85 F6 0F 84 89 00 00 00 48 8D 7D F0 66 66 "
        "0F 1F 84 00 00 00 00 00 0F B6 17 49 8B 8F F8 00 00 00 E8 B1 D9 FF FF B9 D8 01 00 00 41 B8 01 00 00 00 85 C0 "
        "0F 85 2E FE"},
    {0x627360u,
        "FF FF FF C3 48 FF C7 3B DE 72 D5 F3 90 E9 19 FE FF FF 41 8D 75 01 66 C7 45 50 0C 0D 33 DB C6 45 52 0E 85 F6 "
        "74 35 48 8D 7D 50 66 0F 1F 44 00 00 0F B6 17 49 8B 8F F8 00 00 00 E8 61 D9 FF FF B9 D8 01 00 00 41 B8 01 00 "
        "00 00 85 C0 0F 85 DE FD FF FF FF C3 48 FF C7 3B DE 72 D5 F3 90 E9 C9 FD FF FF 48 8B 9C 24 90 00 00 00 48 83 "
        "C4 50 41 5F 41 5E 41 5D 41 5C 5F 5E 5D C3"},
    {0x6261C9u,
        "0F B6 95 20 20 00 00 8B F0 41 8D 0C 54 03 D1"},
    {0x113E1Au,
        "65 48 8B 04 25 58 00 00 00 48 8B D9 B9 D8 01 00 00 48 8B 00 C7 04 01 01 00 00 00"},
};
#define RWH_PINS (sizeof kRwhPins / sizeof kRwhPins[0])

/* lea r14d, [rdx+rdx*2+3]; mov eax, 6; cmp r14d, eax; cmova r14d, eax: then the game's list, xor and test as they
 * were, je to the same return, lea rdi of the list. eax is dead here: the branch only returns after. */
static const uint8_t kRwhPollCode[RWH_POLL_LENGTH] = {0x44,0x8d,0x74,0x52,0x03, 0xb8,0x06,0x00,0x00,0x00, 0x41,0x39,0xc6,
    0x44,0x0f,0x47,0xf0, 0xc7,0x45,0xf0,0x12,0x00,0x06,0x13, 0x33,0xdb, 0xc7,0x45,0xf4,0x01,0x07,0x14,0x02,
    0xc6,0x45,0xf8,0x08, 0x45,0x85,0xf6, 0x74,0x25, 0x48,0x8d,0x7d,0xf0};

/* The same with esi and r13d, je rel32 to the wait's return, a 7-byte nop to the loop. eax is reloaded at the top. */
static const uint8_t kRwhWaitCode[RWH_WAIT_LENGTH] = {0x43,0x8d,0x74,0x6d,0x03, 0xb8,0x06,0x00,0x00,0x00, 0x39,0xc6,
    0x0f,0x47,0xf0, 0xc7,0x45,0xd0,0x12,0x00,0x06,0x13, 0x33,0xdb, 0xc7,0x45,0xd4,0x01,0x07,0x14,0x02,
    0xc6,0x45,0xd8,0x08, 0x85,0xf6, 0x0f,0x84,0xd6,0x01,0x00,0x00, 0x48,0x8d,0x7d,0xd0, 0x0f,0x1f,0x80,0x00,0x00,0x00,0x00};

static int g_rwhState; /* 0 not asked, 2 waiting for the exe's code, 1 installed, -1 not */
static ULONGLONG g_rwhLoadedAt;

static int JournalPut(uintptr_t address, const uint8_t* bytes, size_t size); /* live_code.c */

static int RwhPinned(uintptr_t exe)
{
    for (size_t i = 0; i < RWH_PINS; i++)
        if (!ApiCodeMatches(exe + kRwhPins[i].rva, kRwhPins[i].hex)) return 0;
    return 1;
}

/* 1 when the pins match. Otherwise 0, and a minute after the game loaded the code never will be the reviewed one. */
static int RwhReady(uintptr_t exe)
{
    if (RwhPinned(exe)) return 1;
    if (!g_fired[HF_EVENT_GAME_LOADED]) return 0;
    if (g_rwhLoadedAt == 0) g_rwhLoadedAt = GetTickCount64();
    if (GetTickCount64() - g_rwhLoadedAt < 60000) return 0;
    g_rwhState = -1;
    Note("performance fix %s: not installed; the job system's waits are not the reviewed code", RWH_NAME);
    return 0;
}

/* Each branch is straight code, written with no thread in it, and whole on its own: a thread past it holds the old
 * count for one pass, the next pass reads the new one. */
static void RwhWrite(uintptr_t exe)
{
    int poll = !JournalPut(exe + RWH_POLL_SITE, kRwhPollCode, RWH_POLL_LENGTH);
    int wait = !JournalPut(exe + RWH_WAIT_SITE, kRwhWaitCode, RWH_WAIT_LENGTH);
    g_rwhState = poll || wait ? 1 : -1;
    if (poll && wait) Note("performance fix %s: installed; the render thread runs its own jobs of the priority it waits at", RWH_NAME);
    else if (poll || wait) Note("performance fix %s: installed in part; the %s could not be rewritten", RWH_NAME, poll ? "counter wait" : "poll");
    else Note("performance fix %s: not installed; its code could not be rewritten", RWH_NAME);
}

static void RwhWatchAt(uintptr_t exe)
{
    if (g_rwhState == 0 && strcmp(g_build, "25480438"))
    {
        g_rwhState = -1;
        Note("performance fix %s: not installed; not build 25480438", RWH_NAME);
    }
    if (g_rwhState == 0) g_rwhState = PerfFixOn(RWH_NAME) ? 2 : -1;
    if (g_rwhState == 2 && RwhReady(exe)) RwhWrite(exe);
}

/* From the start: the render thread waits from its first frame. */
static void PerfRenderWaitHelpWatch(void)
{
    if (g_rwhState == 0 || g_rwhState == 2) RwhWatchAt((uintptr_t)GetModuleHandleW(NULL));
}
