/* The Reflex frame ids fix (perf_reflex_frame_ids.c) on a synthetic exe image: the pins and refusals, the two call
 * sites as written, and both stubs run the way their sites call them (rdi the renderer, rbp the submit end's command,
 * rax the marker's vtable kept). Included by stats_test.c. */

#define RFI_IMAGE 0x200000u
#define RFI_THUNKS 0x1F0000u /* test code, outside every pin */

typedef uint64_t (*RfiThunkFn)(void* frame);

/* A stand-in for a site: rax = 0x5A5A (the marker's vtable there), rdi = rbp = the frame, call the stub; edx goes to
 * frame +0x10 and the result is (rax << 32) | r8d, so a stub that touched rax shows. */
static void RfiThunk(uint8_t* at, const uint8_t* stub)
{
    uint8_t code[] = {0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0, /* mov r11, imm64 */
        0x48, 0xC7, 0xC0, 0x5A, 0x5A, 0x00, 0x00,          /* mov rax, 0x5A5A */
        0x57, 0x55,                                        /* push rdi; push rbp */
        0x48, 0x89, 0xCF, 0x48, 0x89, 0xCD,                /* mov rdi, rcx; mov rbp, rcx */
        0x48, 0x83, 0xEC, 0x28,                            /* sub rsp, 0x28 */
        0x41, 0xFF, 0xD3,                                  /* call r11 */
        0x48, 0x83, 0xC4, 0x28,                            /* add rsp, 0x28 */
        0x89, 0x55, 0x10,                                  /* mov [rbp+0x10], edx */
        0x48, 0xC1, 0xE0, 0x20,                            /* shl rax, 32 */
        0x45, 0x89, 0xC1,                                  /* mov r9d, r8d */
        0x4C, 0x09, 0xC8,                                  /* or rax, r9 */
        0x5D, 0x5F, 0xC3};                                 /* pop rbp; pop rdi; ret */
    uint64_t target = (uint64_t)(uintptr_t)stub;
    memcpy(code + 2, &target, 8);
    memcpy(at, code, sizeof code);
}

static uint8_t* RfiImage(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, RFI_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    for (size_t i = 0; i < RFI_PINS; i++)
    {
        uint8_t bytes[256], mask[256];
        size_t count = ParsePattern(kRfiPins[i].hex, bytes, mask);
        assert(count > 0);
        memcpy(image + kRfiPins[i].rva, bytes, count);
    }
    return image;
}

/* The frame the thunk hands over: +0x1A50 the counter (as rdi), +4 the command's id (as rbp), +0x10 edx back. */
typedef struct RfiFrame
{
    uint8_t bytes[0x1A60];
} RfiFrame;

static uint32_t RfiStart(uintptr_t exe, uint32_t counter)
{
    static RfiFrame f;
    memcpy(f.bytes + 0x1A50, &counter, 4);
    uint64_t r = ((RfiThunkFn)(void*)(exe + RFI_THUNKS))(&f);
    assert((r >> 32) == 0x5A5A); /* rax, the marker's vtable at the site, kept */
    return (uint32_t)r;
}

static uint32_t RfiEnd(uintptr_t exe, uint32_t id)
{
    static RfiFrame f;
    memcpy(f.bytes + 4, &id, 4);
    uint64_t r = ((RfiThunkFn)(void*)(exe + RFI_THUNKS + 0x80))(&f);
    uint32_t edx;
    memcpy(&edx, f.bytes + 0x10, 4);
    assert((r >> 32) == 0x5A5A && edx == 3); /* rax kept, the marker type as the game had it */
    return (uint32_t)r;
}

static void RfiStates(uint8_t* image, uintptr_t exe)
{
    static const uint32_t changed[] = {0x10A1E0u, 0x10A211u, 0x10A22Eu, 0x10ABD7u, 0x10ABDCu, 0x10A81Cu, 0x113156u, 0xADEB7u};
    for (size_t i = 0; i < sizeof changed / sizeof changed[0]; i++)
    {
        image[changed[i]] ^= 0x10;
        assert(!RfiPinned(exe));
        image[changed[i]] ^= 0x10;
    }
    assert(RfiPinned(exe));
    LONG loaded = g_fired[HF_EVENT_GAME_LOADED];
    image[0x10ABDCu] ^= 0x10; /* code not decoded yet: a wait, then a minute after the load it gives up */
    g_rfiState = 0;
    g_fired[HF_EVENT_GAME_LOADED] = 0;
    RfiWatchAt(exe);
    assert(g_rfiState == 2);
    g_fired[HF_EVENT_GAME_LOADED] = 1;
    RfiWatchAt(exe);
    g_rfiLoadedAt = GetTickCount64() - 60001;
    RfiWatchAt(exe);
    assert(g_rfiState == -1);
    image[0x10ABDCu] ^= 0x10;
    g_rfiLoadedAt = 0;
    g_fired[HF_EVENT_GAME_LOADED] = loaded;
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "heap-trim, Reflex-Frame-Ids");
    g_rfiState = 0;
    RfiWatchAt(exe);
    assert(g_rfiState == -1 && RfiPinned(exe));
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    strcpy(g_build, "99999999");
    g_rfiState = 0;
    RfiWatchAt(exe);
    assert(g_rfiState == -1 && RfiPinned(exe));
    strcpy(g_build, "25480438");
}

static void ReflexFrameIdsFixture(void)
{
    uint8_t* image = RfiImage();
    uintptr_t exe = (uintptr_t)image;
    RfiStates(image, exe);

    g_rfiState = 0;
    g_rfiPage = NULL;
    RfiWatchAt(exe);
    assert(g_rfiState == 1 && g_rfiPage != NULL);
    /* the sites: a call to each stub, then a nop to the next instruction (the marker call, untouched) */
    int32_t rel;
    assert(image[RFI_START_SITE] == 0xE8 && image[RFI_START_SITE + 5] == 0x66 && image[RFI_START_SITE + 6] == 0x90);
    memcpy(&rel, image + RFI_START_SITE + 1, 4);
    assert(exe + RFI_START_SITE + 5 + rel == (uintptr_t)g_rfiPage);
    assert(image[RFI_END_SITE] == 0xE8 && !memcmp(image + RFI_END_SITE + 5, "\x0F\x1F\x40\x00", 4));
    memcpy(&rel, image + RFI_END_SITE + 1, 4);
    assert(exe + RFI_END_SITE + 5 + rel == (uintptr_t)g_rfiPage + 0x40);
    assert(!memcmp(image + 0x10A218u, "\xFF\x50\x10", 3) && !memcmp(image + 0x10ABE0u, "\xFF\x50\x10", 3));
    assert(!RfiPinned(exe)); /* the sites are HellForge's now */

    RfiThunk(image + RFI_THUNKS, g_rfiPage);
    RfiThunk(image + RFI_THUNKS + 0x80, g_rfiPage + 0x40);
    FlushInstructionCache(GetCurrentProcess(), image, RFI_IMAGE);
    assert(RfiStart(exe, 0) == 0 && RfiStart(exe, 7) == 7); /* no submit end yet: the counter as it is */
    assert(RfiEnd(exe, 41) == 41);
    assert(RfiStart(exe, 43) == 42); /* the game thread already raised it: the frame after the last end */
    assert(RfiStart(exe, 42) == 42); /* the render thread first: as it was */
    assert(RfiStart(exe, 50) == 50); /* a gap (no frames presented meanwhile): left alone */
    assert(RfiEnd(exe, 0xFFFFFFFEu) == 0xFFFFFFFEu && RfiStart(exe, 0) == 0xFFFFFFFFu); /* wraps like the counter */
    VirtualFree(image, 0, MEM_RELEASE);
    g_rfiState = 0;
    g_rfiPage = NULL;
    printf("PASS reflex-frame-ids: pins, refusals (HELLFORGE_PERF_OFF, build, code not decoded), both sites, the submit start one frame back only after a submit end, rax kept\n");
}
