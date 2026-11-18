/* SafeRead (safe_read.c) on memory the test makes: a readable buffer, a page that cannot be read, a guard page and a read
 * that runs off a readable page. Each fails as ReadProcessMemory does; a guard page keeps its guard. Also the names
 * HELLFORGE_PERF_OFF turns off. Included by stats_test.c. */

static void SafeReadFixture(void)
{
    assert(PerfFixListed("safe-read", "safe-read") && PerfFixListed(" corpse-freeze , SAFE-READ ", "safe-read"));
    assert(PerfFixListed("All", "anything") && !PerfFixListed("safe-reads,safe", "safe-read") && !PerfFixListed("", "safe-read"));
    assert(!PerfFixListed(",, ,", "safe-read"));

    uint8_t source[300], target[300];
    for (int i = 0; i < 300; i++) source[i] = (uint8_t)i;
    memset(target, 0, sizeof target);
    assert(SafeRead((uintptr_t)source, target, 256) == 0 && memcmp(source, target, 256) == 0);
    assert(g_safeCopyOn == 1); /* the first read installed the in-place copy */
    memset(target, 0, sizeof target);
    assert(SafeRead((uintptr_t)source, target, 300) == 0 && memcmp(source, target, 300) == 0); /* over 256: the system call */
    assert(SafeRead((uintptr_t)source, target, 0) == 0);
    assert(SafeRead(0x1000, target, 8) == 1 && SafeRead(0x7FFFFFFFFFF0ull, target, 32) == 1);

    /* two pages: the first readable, the second not */
    uint8_t* pages = (uint8_t*)VirtualAlloc(NULL, 0x3000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(pages != NULL);
    DWORD old;
    VirtualProtect(pages + 0x1000, 0x1000, PAGE_NOACCESS, &old);
    LONG faults = g_safeCopyFaults;
    assert(SafeRead((uintptr_t)(pages + 0x1000), target, 8) == 1);
    assert(SafeRead((uintptr_t)(pages + 0xFFC), target, 8) == 1); /* runs off the readable page */
    assert(SafeRead((uintptr_t)(pages + 0xFF8), target, 8) == 0);
    assert(g_safeCopyFaults == faults + 2);

    /* a guard page: the read fails, and the guard is back for the next touch */
    VirtualProtect(pages + 0x2000, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old);
    assert(SafeRead((uintptr_t)(pages + 0x2000), target, 4) == 1);
    MEMORY_BASIC_INFORMATION m;
    assert(VirtualQuery(pages + 0x2000, &m, sizeof m) && (m.Protect & PAGE_GUARD) != 0);
    assert(SafeRead((uintptr_t)(pages + 0x2000), target, 4) == 1);
    VirtualFree(pages, 0, MEM_RELEASE);
    printf("safe read: in-place copies and their failures pass\n");
}
