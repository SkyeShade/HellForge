/* The forest cells' empty box (perf_speedtree_empty_cells.c) on a synthetic exe image: the pins, the waits and refusals
 * that leave the game's bytes, then the game's block and HellForge's both run on a cell through a thunk, with a ret
 * where the game goes on to the next cell. Included by stats_test.c. */

#define ST_IMAGE 0x16AB000u
#define ST_THUNK 0x300000u

typedef void (*StCall)(uint8_t* cell, uint8_t* frame);

static void StPut(uint8_t* image, uint32_t rva, const uint8_t* bytes, size_t size) { memcpy(image + rva, bytes, size); }

/* 1 when one changed byte at rva makes the pins refuse; the byte is put back. */
static int StRefuses(uint8_t* image, uint32_t rva)
{
    image[rva] ^= 0x10;
    int refused = !StPinned((uintptr_t)image);
    image[rva] ^= 0x10;
    return refused;
}

/* The block on a cell of 0xAA and a frame of 0x55, the ret at ST_NEXT in place for the run only. */
static void StRun(uint8_t* image, StCall call, uint8_t* cell, uint8_t* frame)
{
    memset(cell, 0xaa, 0xe0);
    memset(frame, 0x55, 0x30);
    image[ST_NEXT] = 0xc3;
    FlushInstructionCache(GetCurrentProcess(), image + ST_NEXT, 1);
    call(cell, frame);
    StPut(image, ST_NEXT, kStNextPin, sizeof kStNextPin);
}

static void PerfSpeedTreeFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, ST_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    uintptr_t exe = (uintptr_t)image;
    StPut(image, ST_SITE, kStOld, sizeof kStOld);
    StPut(image, ST_PROBE, kStProbePin, sizeof kStProbePin);
    StPut(image, ST_NEXT, kStNextPin, sizeof kStNextPin);
    StPut(image, ST_FOUND, kStFoundPin, sizeof kStFoundPin);
    StPut(image, ST_CALL, kStCallPin, sizeof kStCallPin);
    StPut(image, ST_SKIP, kStSkipPin, sizeof kStSkipPin);
    StPut(image, ST_EMPTY, kStEmptyPin, sizeof kStEmptyPin);
    StPut(image, ST_FLT_MAX, kStEmptyPin, 4);
    StPut(image, ST_FLT_MIN, kStEmptyPin + 12, 4);
    assert(StPinned(exe));
    const uint32_t changed[] = {ST_SITE, ST_SITE + 30, ST_PROBE + 12, ST_NEXT, ST_FOUND + 18, ST_CALL + 1, ST_SKIP + 6, ST_EMPTY + 15,
        ST_FLT_MAX, ST_FLT_MIN + 3};
    for (size_t i = 0; i < sizeof changed / sizeof changed[0]; i++) assert(StRefuses(image, changed[i])); /* another build's code */

    /* push rbp; push r14; sub rsp, 0x28; movups [rsp], xmm9; r14 = the cell; rbp = the frame; xmm9 = 0, as the game
     * has it there; call the site; then everything back */
    static const uint8_t enter[] = {0x55, 0x41,0x56, 0x48,0x83,0xec,0x28, 0x44,0x0f,0x11,0x0c,0x24, 0x49,0x89,0xce, 0x48,0x89,0xd5,
        0x45,0x0f,0x57,0xc9, 0xe8};
    static const uint8_t leave[] = {0x44,0x0f,0x10,0x0c,0x24, 0x48,0x83,0xc4,0x28, 0x41,0x5e, 0x5d, 0xc3};
    StPut(image, ST_THUNK, enter, sizeof enter);
    int32_t rel = (int32_t)(ST_SITE - (ST_THUNK + sizeof enter + 4));
    memcpy(image + ST_THUNK + sizeof enter, &rel, 4);
    StPut(image, ST_THUNK + sizeof enter + 4, leave, sizeof leave);
    FlushInstructionCache(GetCurrentProcess(), image, ST_IMAGE);
    StCall call = (StCall)(void*)(image + ST_THUNK);
    _Alignas(16) uint8_t cell[0xe0], frame[0x30], zero[24] = {0}, empty[24];
    memcpy(empty, kStEmptyPin, 16);
    memcpy(empty + 16, kStEmptyPin + 12, 4);
    memcpy(empty + 20, kStEmptyPin + 12, 4);

    /* the game's block: a zero box, and its dead stores into the frame */
    StRun(image, call, cell, frame);
    assert(!memcmp(cell + 0x58, zero, 24) && cell[0x57] == 0xaa && cell[0x70] == 0xaa);
    assert(!memcmp(frame + 0x10, zero, 24) && frame[0x0f] == 0x55 && frame[0x28] == 0x55);

    char build[sizeof g_build];
    memcpy(build, g_build, sizeof build);
    strcpy(g_build, "25480438");
    LONG loaded = g_fired[HF_EVENT_GAME_LOADED];

    /* code not decoded yet: it waits until a minute after the game loaded, then gives up; the bytes stay */
    image[ST_PROBE] ^= 0x10;
    g_stState = 0;
    g_fired[HF_EVENT_GAME_LOADED] = 0;
    StWatchAt(exe);
    assert(g_stState == 2 && !memcmp(image + ST_SITE, kStOld, ST_LENGTH));
    g_fired[HF_EVENT_GAME_LOADED] = 1;
    StWatchAt(exe);
    assert(g_stState == 2 && g_stLoadedAt != 0);
    g_stLoadedAt = GetTickCount64() - 60001;
    StWatchAt(exe);
    assert(g_stState == -1 && !memcmp(image + ST_SITE, kStOld, ST_LENGTH));
    image[ST_PROBE] ^= 0x10;
    g_stLoadedAt = 0;
    g_fired[HF_EVENT_GAME_LOADED] = loaded;

    /* turned off, and another build: the game's bytes stay */
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "safe-read, SpeedTree-Empty-Cells");
    g_stState = 0;
    StWatchAt(exe);
    assert(g_stState == -1 && !memcmp(image + ST_SITE, kStOld, ST_LENGTH));
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    strcpy(g_build, "99999999");
    g_stState = 0;
    StWatchAt(exe);
    assert(g_stState == -1 && !memcmp(image + ST_SITE, kStOld, ST_LENGTH));
    strcpy(g_build, "25480438");

    /* installed: the reviewed bytes, the rel32 reaching the game's constant from the site */
    static const uint8_t expect[ST_LENGTH] = {0x0f,0x28,0x05,0x2c,0x83,0x49,0x01, 0x41,0x0f,0x11,0x46,0x58, 0x0f,0xc6,0xc0,0xff,
        0xf2,0x41,0x0f,0x11,0x46,0x68, 0x66,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00};
    g_stState = 0;
    StWatchAt(exe);
    assert(g_stState == 1 && !memcmp(image + ST_SITE, expect, ST_LENGTH));
    assert(!memcmp(image + ST_NEXT, kStNextPin, sizeof kStNextPin) && !memcmp(image + ST_PROBE, kStProbePin, sizeof kStProbePin));
    StWatchAt(exe);
    assert(g_stState == 1); /* once */

    /* HellForge's block: the empty box the cull skips, nothing else in the cell, and the frame left alone */
    StRun(image, call, cell, frame);
    assert(!memcmp(cell + 0x58, empty, 24) && cell[0x57] == 0xaa && cell[0x70] == 0xaa);
    for (int i = 0; i < 0x30; i++) assert(frame[i] == 0x55);

    memcpy(g_build, build, sizeof build);
    VirtualFree(image, 0, MEM_RELEASE);
    puts("PASS speedtree empty cells: pins refuse any changed byte, a wait for decoded code that gives up, off by name and on another build with the game's bytes kept, the game's zero box and HellForge's empty box run on a cell, installed once");
}
