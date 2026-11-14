/* The render job copies fix (perf_render_job_copies.c) on a synthetic exe image that holds the pinned bytes, so the
 * engine's own resize (0x6D55E0) runs. A thunk sets up the game's stack and jumps to the patched call; a fake
 * recording checks the copy is still whole while it runs; a marker after the call returns to the test. Included by
 * stats_test.c. */

#define RJC_IMAGE 0x700000u
#define RJC_THUNK 0x6F0000u /* free room in the image */

typedef uint64_t (*RjcJob)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static uint8_t* g_rjcTestX;
static uint64_t g_rjcTestArgs[7];
static int g_rjcTestRecorded, g_rjcTestFrees;
static void* g_rjcTestFreed;

/* The recording, reached through 0x6FD1B0: the job's copy must still be whole here. */
static uint64_t RjcTestRecording(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g)
{
    const uint64_t args[7] = {a, b, c, d, e, f, g};
    memcpy(g_rjcTestArgs, args, sizeof args);
    if (g_rjcTestX != NULL)
    {
        uint32_t count, capacity;
        void* data;
        memcpy(&count, g_rjcTestX + 8, 4);
        memcpy(&capacity, g_rjcTestX + 0xC, 4);
        memcpy(&data, g_rjcTestX + 0x10, 8);
        assert(count != 0 && capacity != 0 && data != NULL);
    }
    g_rjcTestRecorded++;
    return 0x1234;
}

/* The proxy allocator's free (vtable +0x40). */
static void RjcTestFree(void* self, void* data, uint64_t unused)
{
    g_rjcTestFrees++;
    g_rjcTestFreed = data;
    SetLastError(77);
    free(data);
}

/* A job context X with a copy of `capacity` entries, `used` in use, behind the context C at +0x3B0. */
static void RjcTestCopy(uint8_t* x, uint8_t* c, void* allocator, uint32_t capacity, uint32_t used)
{
    memset(x, 0, 0x1A0);
    void* data = malloc((size_t)capacity * 24);
    assert(data != NULL);
    uint32_t fields[] = {capacity, capacity}, table[] = {used, capacity * 3 / 4, 7, 9};
    memcpy(x + 8, fields, sizeof fields);
    memcpy(x + 0x10, &data, 8);
    memcpy(x + 0x18, &allocator, 8);
    memcpy(x + 0x20, table, sizeof table);
    memcpy(c + 0x3B0, &x, 8);
}

static uint32_t RjcU32(const uint8_t* at) { uint32_t v; memcpy(&v, at, 4); return v; }

static void RenderJobCopiesFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, RJC_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    uintptr_t exe = (uintptr_t)image;
    for (size_t i = 0; i < RJC_PINS; i++)
    {
        uint8_t bytes[256], mask[256];
        size_t count = ParsePattern(kRjcPins[i].hex, bytes, mask);
        memcpy(image + kRjcPins[i].rva, bytes, count);
    }
    assert(RjcPinned(exe));
    const uint32_t changed[] = {RJC_CALL + 1, 0x6CA45F, RJC_RESIZE + 0x41, 0x6CD646, 0x6E2230};
    for (size_t i = 0; i < sizeof changed / sizeof changed[0]; i++)
    {
        image[changed[i]] ^= 1;
        assert(!RjcPinned(exe)); /* another build's code is left alone */
        image[changed[i]] ^= 1;
    }

    uint8_t before[5];
    memcpy(before, image + RJC_CALL, 5);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "heap-trim, Render-Job-Copies");
    assert(RenderJobCopiesInstallAt(exe) == -1 && memcmp(before, image + RJC_CALL, 5) == 0);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    assert(RenderJobCopiesInstallAt(exe) == 1);
    g_rjcState = 1;
    int32_t rel;
    memcpy(&rel, image + RJC_CALL + 1, 4);
    const uint8_t* cave = image + RJC_CALL + 5 + rel;
    uint64_t to;
    memcpy(&to, cave + 6, 8);
    assert(image[RJC_CALL] == 0xE8 && cave[0] == 0xFF && cave[1] == 0x25 && to == (uint64_t)(uintptr_t)RjcRecord);
    assert(g_rjcRecord == (RjcRecordFn)(exe + RJC_RECORD) && g_rjcResize == (RjcResizeFn)(exe + RJC_RESIZE));

    /* the markers, after the install: the recording jumps to the fake; the job's tail returns to the thunk's caller */
    static const uint8_t tail[] = {0x48,0x83,0xC4,0x48, 0xC3}; /* add rsp, 0x48; ret */
    memcpy(image + RJC_CALL + 5, tail, sizeof tail);
    static const uint8_t jumpFar[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint64_t recording = (uint64_t)(uintptr_t)RjcTestRecording;
    memcpy(image + RJC_RECORD, jumpFar, sizeof jumpFar);
    memcpy(image + RJC_RECORD + 6, &recording, 8);
    /* sub rsp, 0x48 (16-aligned at the call, as the job's frame); arguments 5-7 to [rsp+0x20..0x30]; jmp to the call */
    static const uint8_t thunk[] = {0x48,0x83,0xEC,0x48, 0x48,0x8B,0x44,0x24,0x70, 0x48,0x89,0x44,0x24,0x20, 0x48,0x8B,0x44,0x24,0x78,
        0x48,0x89,0x44,0x24,0x28, 0x48,0x8B,0x84,0x24,0x80,0x00,0x00,0x00, 0x48,0x89,0x44,0x24,0x30};
    memcpy(image + RJC_THUNK, thunk, sizeof thunk);
    RgJump(image + RJC_THUNK + sizeof thunk, 0xE9, exe + RJC_CALL);
    FlushInstructionCache(GetCurrentProcess(), image, RJC_IMAGE);
    RjcJob job = (RjcJob)(void*)(image + RJC_THUNK);

    void* vtable[9] = {0};
    vtable[8] = (void*)RjcTestFree;
    void* allocator = vtable;
    _Alignas(16) uint8_t x[0x1A0], c[0x400] = {0};
    g_rjcTestX = x;

    /* a long copy: whole during the recording, given back after it; every argument forwarded, arg 6's high half too */
    RjcTestCopy(x, c, &allocator, 7014, 120);
    void* data;
    memcpy(&data, x + 0x10, 8);
    LONG freed = g_rjcFreed;
    LONG64 bytes = g_rjcBytes;
    SetLastError(4321);
    assert(job(0x1111, 0x2222, (uint64_t)(uintptr_t)c, 0x44, 0x5555, 0xDEADBEEF00000006ull, 0x7777) == 0x1234);
    assert(GetLastError() == 4321);
    const uint64_t expect[7] = {0x1111, 0x2222, (uint64_t)(uintptr_t)c, 0x44, 0x5555, 0xDEADBEEF00000006ull, 0x7777};
    assert(g_rjcTestRecorded == 1 && memcmp(g_rjcTestArgs, expect, sizeof expect) == 0);
    void* after;
    memcpy(&after, x + 0x10, 8);
    assert(g_rjcTestFrees == 1 && g_rjcTestFreed == data && after == NULL);
    assert(RjcU32(x + 8) == 0 && RjcU32(x + 0xC) == 0 && RjcU32(x + 0x20) == 0 && RjcU32(x + 0x28) == 0 && RjcU32(x + 0x2C) == 0xFFFFFFFFu);
    assert(RjcU32(x + 0x24) == 7014 * 3 / 4); /* buckets: the next copy writes them */
    assert(g_rjcFreed == freed + 1 && g_rjcBytes == bytes + 7014 * 24 && g_rjcLargest >= 7014);
    RenderJobCopiesReport();
    assert(g_rjcReportedLargest == g_rjcLargest);

    /* the same context again: nothing left to give back */
    g_rjcTestX = NULL;
    assert(job(1, 2, (uint64_t)(uintptr_t)c, 3, 4, 5, 6) == 0x1234 && g_rjcTestFrees == 1 && g_rjcFreed == freed + 1);

    /* the shortest copy given back, and one entry shorter kept */
    g_rjcTestX = x;
    RjcTestCopy(x, c, &allocator, RJC_LONG, 3);
    assert(job(1, 2, (uint64_t)(uintptr_t)c, 3, 4, 5, 6) == 0x1234 && g_rjcTestFrees == 2 && RjcU32(x + 0xC) == 0);
    RjcTestCopy(x, c, &allocator, RJC_LONG - 1, 3);
    memcpy(&data, x + 0x10, 8);
    assert(job(1, 2, (uint64_t)(uintptr_t)c, 3, 4, 5, 6) == 0x1234 && g_rjcTestFrees == 2);
    assert(RjcU32(x + 0xC) == RJC_LONG - 1 && RjcU32(x + 8) == RJC_LONG - 1 && RjcU32(x + 0x20) == 3);
    free(data);

    /* no context: nothing read past C */
    g_rjcTestX = NULL;
    memset(c + 0x3B0, 0, 8);
    assert(job(1, 2, (uint64_t)(uintptr_t)c, 3, 4, 5, 6) == 0x1234 && g_rjcTestFrees == 2 && g_rjcTestRecorded == 5);
    g_rjcState = 0;
    puts("PASS render job copies: pins (call, tail, resize, empty, reader), HELLFORGE_PERF_OFF, the call patched, a long copy "
        "whole during the recording and given back after it by the engine's resize, all seven arguments forwarded, the last "
        "error kept, short copies and no context left alone");
}
