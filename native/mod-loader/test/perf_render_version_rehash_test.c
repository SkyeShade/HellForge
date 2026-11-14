/* render-version-rehash (perf_render_version_rehash.c) on a synthetic exe image that holds the pinned bytes, so the
 * shrink runs the game's own rehash, reallocation, insert and find on tables the game's insert filled. A marker after
 * the dispatch's prologue goes to a stand-in, and a fake allocator stands for the pool's. Included by stats_test.c. */

#define RVR_IMAGE 0x16AA000u   /* past the rehash's float constants */
#define RVR_MEMCPY 0x1267850u  /* the reallocation's memcpy */
#define RVR_INSERT 0x6F4220u
#define RVR_FIND 0x6F51F0u
#define RVR_MARKER 0x6CECC0u   /* past the prologue, whichever way the hook moved it */

typedef uint64_t* (*RvrInsertFn)(uintptr_t map, const uint32_t* key);
typedef uint32_t (*RvrFindFn)(uintptr_t map, const uint32_t* key);

static LONG g_rvrTestFrees, g_rvrTestCalls;
static uint64_t g_rvrTestDevice;
static ULONGLONG g_rvrTestNow;

static void** RvrTestAlloc(void* self, void** out, uint64_t size, uint64_t align)
{
    *out = malloc((size_t)size);
    return out;
}

static void RvrTestFree(void* self, void* block, uint64_t flags)
{
    if (block == NULL) return;
    free(block);
    g_rvrTestFrees++;
}

static ULONGLONG WINAPI RvrTestClock(void) { return g_rvrTestNow; }

/* The dispatch's stand-in: the arguments as the hook passed them on. */
static uint64_t RvrTestDispatch(uint64_t device, uint64_t count, uint64_t commands, uint64_t r9, uint64_t flags)
{
    assert(device == g_rvrTestDevice && count == 7 && commands == 0x55 && r9 == 0x66 && (uint32_t)flags == 0x15);
    g_rvrTestCalls++;
    return 0x1234;
}

static uint32_t RvrTestField(uintptr_t map, uint32_t offset) { return *(uint32_t*)(map + offset); }

/* The size the rehash gives b buckets: its spill is b * 0.37 + 1, truncated, in single precision. */
static uint32_t RvrTestSize(uint32_t buckets) { return buckets + (uint32_t)(int32_t)((float)buckets * 0.37f + 1.0f); }

static uint64_t RvrTestValue(uint32_t key) { return key * 0x9E3779B97F4A7C15ull; }

static void RvrTestFill(uintptr_t exe, uintptr_t map, uint32_t from, uint32_t count)
{
    for (uint32_t key = from; key < from + count; key++) *((RvrInsertFn)(exe + RVR_INSERT))(map, &key) = RvrTestValue(key);
}

/* Every key found by the game's find, its value kept, and nothing else counted in use. */
static int RvrTestHolds(uintptr_t exe, uintptr_t map, uint32_t from, uint32_t count)
{
    for (uint32_t key = from; key < from + count; key++)
    {
        uint32_t at = ((RvrFindFn)(exe + RVR_FIND))(map, &key);
        if (at == 0x7FFFFFFFu || *(uint64_t*)(*(uint8_t**)(map + 8) + at * 24ull + 8) != RvrTestValue(key)) return 0;
    }
    return RvrTestField(map, 0x18) == count;
}

/* What the present's clear (0x6D5560) leaves: every entry empty, the size kept. */
static void RvrTestClear(uintptr_t map)
{
    uint8_t* entries = *(uint8_t**)(map + 8);
    for (uint32_t i = 0; i < RvrTestField(map, 0); i++)
    {
        memset(entries + i * 24ull, 0, 16);
        *(int32_t*)(entries + i * 24ull + 0x10) = -2;
    }
    *(uint32_t*)(map + 0x18) = 0;
    *(uint32_t*)(map + 0x20) = RvrTestField(map, 0) - RvrTestField(map, 0x1C);
    *(int32_t*)(map + 0x24) = -1;
}

static uint8_t* RvrTestImage(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, RVR_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    for (size_t i = 0; i < RVR_PINS; i++)
    {
        uint8_t bytes[256], mask[256];
        size_t count = ParsePattern(kRvrPins[i].hex, bytes, mask);
        assert(count > 0);
        memcpy(image + kRvrPins[i].rva, bytes, count);
    }
    uint64_t copy = (uint64_t)(uintptr_t)memcpy;
    uint8_t* c = image + RVR_MEMCPY;
    *c++ = 0x48; *c++ = 0xb8; memcpy(c, &copy, 8); c += 8; /* mov rax, memcpy */
    *c++ = 0xff; *c = 0xe0;                                 /* jmp rax */
    return image;
}

/* The pins, then the install refused when switched off or on changed bytes, then the hook. */
static void RvrTestInstall(uint8_t* image)
{
    uintptr_t exe = (uintptr_t)image;
    assert(RvrPinned(exe));
    for (size_t i = 0; i < RVR_PINS; i++)
    {
        uint8_t bytes[256], mask[256];
        uint8_t* at = image + kRvrPins[i].rva + ParsePattern(kRvrPins[i].hex, bytes, mask) / 2;
        *at ^= 0x10;
        assert(!RvrPinned(exe)); /* another build's code is left alone */
        *at ^= 0x10;
    }
    g_rvrState = 0;
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "safe-read, Render-Version-Rehash");
    RvrInstallPolled(exe, 0);
    assert(g_rvrState == -1 && image[RVR_DISPATCH] == 0x48);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);

    g_rvrState = 0;
    image[RVR_REHASH + 9] ^= 1;
    RvrInstallPolled(exe, 0);
    assert(g_rvrState == 2); /* the code may still be decoded */
    RvrInstallPolled(exe, 1);
    assert(g_rvrState == -1 && image[RVR_DISPATCH] == 0x48);
    image[RVR_REHASH + 9] ^= 1;

    g_rvrState = 0;
    RvrInstallPolled(exe, 0);
    assert(g_rvrState == 1 && image[RVR_DISPATCH] != 0x48);
    /* undo the prologue's push and sub, then the stand-in: add rsp, 0x30; pop r15; mov rax, imm64; jmp rax */
    uint64_t to = (uint64_t)(uintptr_t)RvrTestDispatch;
    uint8_t marker[] = {0x48,0x83,0xc4,0x30, 0x41,0x5f, 0x48,0xb8, 0,0,0,0,0,0,0,0, 0xff,0xe0};
    memcpy(marker + 8, &to, 8);
    memcpy(image + RVR_MARKER, marker, sizeof marker);
    FlushInstructionCache(GetCurrentProcess(), image, RVR_IMAGE);
}

static void RenderVersionRehashFixture(void)
{
    uint8_t* image = RvrTestImage();
    uintptr_t exe = (uintptr_t)image;
    RvrTestInstall(image);
    g_rvrClock = RvrTestClock;
    g_rvrTestNow = 5000;
    g_rvrWindowAt = 0;
    memset(g_rvrPeak, 0, sizeof g_rvrPeak);

    uint8_t* device = (uint8_t*)calloc(1, 0x1500);
    assert(device != NULL);
    void* allocator[9] = {0};
    allocator[6] = (void*)RvrTestAlloc;
    allocator[8] = (void*)RvrTestFree;
    uintptr_t pool = (uintptr_t)device + RVR_POOL, versions = pool + 0x88, changed = pool + 0xB0;
    *(void***)pool = allocator;
    for (uintptr_t map = versions; map <= changed; map += 0x28)
    {
        *(uintptr_t*)(map + 0x10) = pool; /* the tables allocate through the pool, as the device's constructor sets */
        *(int32_t*)(map + 0x24) = -1;
    }
    g_rvrTestDevice = (uint64_t)(uintptr_t)device;
    RvrDispatchFn dispatch = (RvrDispatchFn)(void*)(image + RVR_DISPATCH);

    /* a load of 6,000 ids grows both tables; after the present's clear a frame updates 100 */
    RvrTestFill(exe, versions, 1, 6000);
    RvrTestFill(exe, changed, 1, 6000);
    assert(RvrTestField(versions, 0) >= 7014 && RvrTestHolds(exe, versions, 1, 6000));
    RvrTestClear(versions);
    RvrTestClear(changed);
    RvrTestFill(exe, versions, 1, 100);
    RvrTestFill(exe, changed, 1, 50);
    LONG shrinks = g_rvrShrinks, frees = g_rvrTestFrees;
    assert(dispatch(g_rvrTestDevice, 7, 0x55, 0x66, 0x15) == 0x1234 && g_rvrTestCalls == 1);
    assert(RvrTestField(versions, 0x1C) == 419 && RvrTestField(versions, 0) == RvrTestSize(419) && RvrTestHolds(exe, versions, 1, 100));
    assert(RvrTestField(changed, 0x1C) == 219 && RvrTestField(changed, 0) == RvrTestSize(219) && RvrTestHolds(exe, changed, 1, 50));
    assert(g_rvrShrinks == shrinks + 2 && g_rvrTestFrees == frees + 2); /* the long arrays given back */
    assert(g_rvrFrom >= 7014 && g_rvrTo == (LONG)RvrTestSize(219));

    /* short tables stay as they are; the game's insert grows one again, every entry kept */
    assert(dispatch(g_rvrTestDevice, 7, 0x55, 0x66, 0x15) == 0x1234 && g_rvrShrinks == shrinks + 2);
    RvrTestFill(exe, versions, 101, 3000);
    assert(RvrTestField(versions, 0x1C) > 419 && RvrTestHolds(exe, versions, 1, 3100));

    /* a frame of 3,000 keeps a long table long until it is two windows old */
    RvrTestClear(versions);
    RvrTestFill(exe, versions, 1, 9000);
    RvrTestClear(versions);
    RvrTestFill(exe, versions, 1, 3000);
    uint32_t longSize = RvrTestField(versions, 0);
    assert(longSize >= RVR_LONG && 3000 > RvrTestField(versions, 0x1C) / 8);
    dispatch(g_rvrTestDevice, 7, 0x55, 0x66, 0x15);
    RvrTestClear(versions);
    RvrTestFill(exe, versions, 1, 100);
    dispatch(g_rvrTestDevice, 7, 0x55, 0x66, 0x15);
    g_rvrTestNow += RVR_WINDOW_MS;
    dispatch(g_rvrTestDevice, 7, 0x55, 0x66, 0x15);
    assert(RvrTestField(versions, 0) == longSize && g_rvrShrinks == shrinks + 2);
    g_rvrTestNow += RVR_WINDOW_MS;
    dispatch(g_rvrTestDevice, 7, 0x55, 0x66, 0x15);
    assert(RvrTestField(versions, 0x1C) == 419 && RvrTestHolds(exe, versions, 1, 100) && g_rvrShrinks == shrinks + 3);
    assert(RvrTestField(changed, 0x1C) == 219 && RvrTestHolds(exe, changed, 1, 50) && g_rvrTestCalls == 6);

    g_rvrClock = GetTickCount64; /* the image and its hook stay: the hook's chain points into them */
    puts("PASS render version rehash: pins, off by HELLFORGE_PERF_OFF, refused on changed bytes, the game's rehash keeping every entry, short tables kept, regrowth, the peak window");
}
