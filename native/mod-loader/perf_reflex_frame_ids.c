/* reflex-frame-ids (docs/performance.md). The engine tells NVIDIA Reflex where each frame's time goes with latency
 * markers, each with a frame id: the game thread sends simulation start and end, the render thread render-submit
 * start and end and present start and end. The render thread's submit start (its begin-frame command, exe 0x10A1E0)
 * reads the frame id from the renderer's frame counter (+0x1A50), which the game thread raises right after it posts
 * the frame (0x10A5C0, 0x10A80A). So the begin-frame command usually runs after the raise and marks frame N's submit
 * start with N + 1: 95% of frames in a D11 run. The driver then sees each frame's render submission begin one frame
 * early, before that frame's own simulation start.
 *
 * HellForge keeps the id the render thread's submit end used last (0x10ABDC: the frame's own id, carried in its
 * command) and gives submit start that id + 1 when the counter has already moved on to + 2. Every other marker, the
 * counter and the device's frame id (+0x3A8, stored from the counter just after) stay as they are. Two small stubs
 * near the exe, both run on the render thread only. Included by hellforge_modloader.c after render_guards.c:
 * ApiCodeMatches, JournalPut, AllocateNearGame (damage.c), PerfFixOn, Note. */

#define RFI_NAME "reflex-frame-ids"
#define RFI_START_SITE 0x10A211u /* mov r8d, [rdi+0x1A50]: the submit start's frame id */
#define RFI_START_LENGTH 7
#define RFI_END_SITE 0x10ABD7u /* mov edx, 3; mov r8d, [rbp+4]: the submit end's type and frame id */
#define RFI_END_LENGTH 9
#define RFI_LAST 0x800u /* the stub page's offset of the last submit end's id */

typedef struct RfiPin
{
    uint32_t rva;
    const char* hex;
} RfiPin;

static const RfiPin kRfiPins[] = {
    /* the begin-frame command, up to its store of the counter into the device (+0x3A8) */
    {0x10A1E0u,
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B F9 48 8B 89 50 1C 00 00 "
        "48 85 C9 74 12 48 8B 01 BA 02 00 00 00 44 8B 87 50 1A 00 00 FF 50 10 8B 87 50 1A 00 00 45 33 F6 48 8B 8F 68 "
        "11 00 00 41 8B DE 89 81 A8 03 00 00"},
    /* the submit end's marker, its frame id from the command (rbp) */
    {0x10ABC8u, "48 8B 8F 50 1C 00 00 48 85 C9 74 0F 48 8B 01 BA 03 00 00 00 44 8B 45 04 FF 50 10 49 8B CF"},
    /* the game thread's raise of the counter after posting the frame */
    {0x10A80Au, "41 8B BF 50 1A 00 00 49 8B 8F 48 12 00 00 FF C7 F7 E7 41 89 BF 50 1A 00 00"},
    /* both are commands of the render thread's dispatch (0x113010) */
    {0x113153u, "49 8B CE E8 85 70 FF FF E9 EE 07 00 00 49 8B D7 49 8B CE E8 E5 77 FF FF"},
    /* the latency object's marker function: (object, type, frame id) */
    {0xADEB0u, "48 81 EC D8 00 00 00 44 8B 49 10 41 83 E9 01"},
};
#define RFI_PINS (sizeof kRfiPins / sizeof kRfiPins[0])

static int g_rfiState; /* 0 not asked, 2 waiting for the exe's code, 1 installed, -1 not */
static ULONGLONG g_rfiLoadedAt;
static uint8_t* g_rfiPage;

static int JournalPut(uintptr_t address, const uint8_t* bytes, size_t size); /* live_code.c */

static int RfiPinned(uintptr_t exe)
{
    for (size_t i = 0; i < RFI_PINS; i++)
        if (!ApiCodeMatches(exe + kRfiPins[i].rva, kRfiPins[i].hex)) return 0;
    return 1;
}

static void RfiRel32(uint8_t* at, const uint8_t* next, const uint8_t* target)
{
    int32_t rel = (int32_t)((intptr_t)target - (intptr_t)next);
    memcpy(at, &rel, 4);
}

/* The two stubs, at page +0 and +0x40; the id at +RFI_LAST starts at 0xFFFFFFFF (no submit end yet). */
static void RfiStubs(uint8_t* page)
{
    /* submit start: r8d = counter; if counter == last + 2, r8d = last + 1. r10 only; rax (the marker's vtable) kept */
    static const uint8_t start[] = {0x44, 0x8B, 0x87, 0x50, 0x1A, 0x00, 0x00, /* mov r8d, [rdi+0x1A50] */
        0x44, 0x8B, 0x15, 0, 0, 0, 0,                                      /* mov r10d, [rip+last] */
        0x41, 0x83, 0xC2, 0x02,                                            /* add r10d, 2 */
        0x45, 0x39, 0xD0,                                                  /* cmp r8d, r10d */
        0x75, 0x03,                                                        /* jne ret */
        0x41, 0xFF, 0xC8,                                                  /* dec r8d */
        0xC3};
    /* submit end: the moved instructions, then last = r8d */
    static const uint8_t end[] = {0xBA, 0x03, 0x00, 0x00, 0x00, /* mov edx, 3 */
        0x44, 0x8B, 0x45, 0x04,                                  /* mov r8d, [rbp+4] */
        0x44, 0x89, 0x05, 0, 0, 0, 0,                            /* mov [rip+last], r8d */
        0xC3};
    memcpy(page, start, sizeof start);
    RfiRel32(page + 10, page + 14, page + RFI_LAST);
    memcpy(page + 0x40, end, sizeof end);
    RfiRel32(page + 0x40 + 12, page + 0x40 + 16, page + RFI_LAST);
    uint32_t none = 0xFFFFFFFFu;
    memcpy(page + RFI_LAST, &none, 4);
}

/* The call bytes for a site: call rel32 to the stub, then a nop of the rest. */
static void RfiCall(uint8_t* out, size_t length, uintptr_t site, const uint8_t* stub)
{
    out[0] = 0xE8;
    RfiRel32(out + 1, (const uint8_t*)(site + 5), stub);
    if (length == 7) { out[5] = 0x66; out[6] = 0x90; }
    else { out[5] = 0x0F; out[6] = 0x1F; out[7] = 0x40; out[8] = 0x00; }
}

/* 1 when the pins match. Otherwise 0, and a minute after the game loaded the code never will be the reviewed one. */
static int RfiReady(uintptr_t exe)
{
    if (RfiPinned(exe)) return 1;
    if (!g_fired[HF_EVENT_GAME_LOADED]) return 0;
    if (g_rfiLoadedAt == 0) g_rfiLoadedAt = GetTickCount64();
    if (GetTickCount64() - g_rfiLoadedAt < 60000) return 0;
    g_rfiState = -1;
    Note("performance fix %s: not installed; the render thread's latency markers are not the reviewed code", RFI_NAME);
    return 0;
}

/* The submit end's stub goes in first: until the start site is written too, it only records. */
static void RfiWrite(uintptr_t exe)
{
    uint8_t* page = g_rfiPage != NULL ? g_rfiPage : AllocateNearGame(exe, 0x1000);
    DWORD protect;
    if (page == NULL) { g_rfiState = -1; Note("performance fix %s: not installed; no memory within 2 GB of the exe", RFI_NAME); return; }
    g_rfiPage = page;
    RfiStubs(page);
    if (!VirtualProtect(page, 0x1000, PAGE_EXECUTE_READWRITE, &protect))
    {
        g_rfiState = -1;
        Note("performance fix %s: not installed; its stubs could not be made executable", RFI_NAME);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    uint8_t endCall[RFI_END_LENGTH], startCall[RFI_START_LENGTH];
    RfiCall(endCall, RFI_END_LENGTH, exe + RFI_END_SITE, page + 0x40);
    RfiCall(startCall, RFI_START_LENGTH, exe + RFI_START_SITE, page);
    if (JournalPut(exe + RFI_END_SITE, endCall, RFI_END_LENGTH) != 0)
    {
        g_rfiState = -1;
        Note("performance fix %s: not installed; its code could not be rewritten", RFI_NAME);
        return;
    }
    if (JournalPut(exe + RFI_START_SITE, startCall, RFI_START_LENGTH) != 0)
    {
        g_rfiState = -1; /* the end stub only records an id: harmless left in */
        Note("performance fix %s: not installed; the submit start could not be rewritten", RFI_NAME);
        return;
    }
    g_rfiState = 1;
    Note("performance fix %s: installed; the render thread's submit start marks its own frame", RFI_NAME);
}

static void RfiWatchAt(uintptr_t exe)
{
    if (g_rfiState == 0 && strcmp(g_build, "25480438"))
    {
        g_rfiState = -1;
        Note("performance fix %s: not installed; not build 25480438", RFI_NAME);
    }
    if (g_rfiState == 0) g_rfiState = PerfFixOn(RFI_NAME) ? 2 : -1;
    if (g_rfiState == 2 && RfiReady(exe)) RfiWrite(exe);
}

/* From the start: the render thread sends markers from its first frame. */
static void PerfReflexFrameIdsWatch(void)
{
    if (g_rfiState == 0 || g_rfiState == 2) RfiWatchAt((uintptr_t)GetModuleHandleW(NULL));
}
