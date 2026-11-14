/* record-hash (perf_record_hash.c) on a synthetic game.dll image: the pins and refusals, then the game's own shared
 * insert (0x172F790) and destruction-record lookup (0x13E1B50), copied byte for byte, run on two maps set up by the
 * setup's store before and after the rewrite. Every lookup answers the same; misses take far fewer probes. Included by
 * stats_test.c. */

#define RH_IMAGE 0x3470000u   /* past [game + 0x346D528], the lookup's global */
#define RH_GLOBAL 0x346D528u
#define RH_INSERT 0x172F790u
#define RH_LOOKUP 0x13E1B50u
#define RH_THUNK 0x1000u
#define RH_OWNER 0x200000u    /* the map's owner: slots inline at +0x5F48, records at +0xC5F78 */
#define RH_KEYS 20000u

static const uint8_t kRhInsertCode[110] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18, 0x44, 0x8b, 0x51, 0x08, 0x33,
    0xc0, 0x44, 0x8b, 0x59, 0x10, 0x41, 0x8b, 0xf0, 0x44, 0x0f, 0xaf, 0xda, 0x44, 0x8b, 0xca, 0x41, 0x8d, 0x7a, 0xff, 0x45,
    0x85, 0xd2, 0x74, 0x27, 0x4c, 0x8b, 0x01, 0x8b, 0x59, 0x0c, 0x8b, 0xcf, 0x42, 0x8d, 0x14, 0x18, 0x48, 0x23, 0xd1, 0x49,
    0x8d, 0x0c, 0xd0, 0x41, 0x8b, 0x14, 0xd0, 0x3b, 0xd3, 0x74, 0x0e, 0x41, 0x3b, 0xd1, 0x74, 0x09, 0xff, 0xc0, 0x41, 0x3b,
    0xc2, 0x72, 0xdf, 0x33, 0xc9, 0x48, 0x8b, 0x5c, 0x24, 0x08, 0x48, 0x8b, 0xc1, 0x48, 0x8b, 0x7c, 0x24, 0x18, 0x89, 0x71,
    0x04, 0x48, 0x8b, 0x74, 0x24, 0x10, 0x44, 0x89, 0x09, 0xc3};
static const uint8_t kRhLookupCode[158] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20,
    0x4c, 0x8b, 0x1d, 0xbd, 0xb9, 0x08, 0x02, 0x33, 0xc0, 0x45, 0x8b, 0x8b, 0x50, 0x5f, 0x0c, 0x00, 0x41, 0x8b, 0x9b, 0x58,
    0x5f, 0x0c, 0x00, 0x0f, 0xaf, 0xda, 0x41, 0x8d, 0x69, 0xff, 0x45, 0x85, 0xc9, 0x74, 0x39, 0x49, 0x8b, 0xbb, 0x48, 0x5f,
    0x0c, 0x00, 0x41, 0x8b, 0xb3, 0x54, 0x5f, 0x0c, 0x00, 0x66, 0x66, 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x8b, 0xcd, 0x44, 0x8d, 0x04, 0x18, 0x4c, 0x23, 0xc1, 0x42, 0x8b, 0x0c, 0xc7, 0x4e, 0x8d, 0x14, 0xc7, 0x3b, 0xce, 0x74,
    0x22, 0x3b, 0xca, 0x74, 0x1e, 0xff, 0xc0, 0x41, 0x3b, 0xc1, 0x72, 0xe0, 0xb0, 0xff, 0x48, 0x8b, 0x5c, 0x24, 0x08, 0x48,
    0x8b, 0x6c, 0x24, 0x10, 0x48, 0x8b, 0x74, 0x24, 0x18, 0x48, 0x8b, 0x7c, 0x24, 0x20, 0xc3, 0x85, 0xc9, 0x74, 0xe5, 0x41,
    0x8b, 0x4a, 0x04, 0x48, 0x8d, 0x04, 0x49, 0x41, 0x0f, 0xb6, 0x84, 0x83, 0x7f, 0x5f, 0x0c, 0x00, 0xeb, 0xd4};

typedef void (*RhSetupFn)(uint8_t* owner);
typedef void (*RhInsertFn)(void* map, uint32_t key, uint32_t value);
typedef uint8_t (*RhLookupFn)(void* unused, uint32_t key);

/* A map as the setup leaves it: 0x8000 empty slots inline, then the setup's own store of the multiplier. */
static uint8_t* RhMap(RhSetupFn setup)
{
    uint8_t* owner = (uint8_t*)VirtualAlloc(NULL, RH_OWNER, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(owner != NULL);
    *(uint8_t**)(owner + 0xC5F48) = owner + 0x5F48;
    *(uint64_t*)(owner + 0xC5F50) = 0x8000u; /* the count, and the empty key 0 */
    setup(owner);
    return owner;
}

/* Probes until the key or an empty slot, as every probe of the map does. */
static uint32_t RhProbes(const uint8_t* owner, uint32_t key)
{
    const uint32_t* slots = *(uint32_t* const*)(owner + 0xC5F48);
    uint32_t count = *(const uint32_t*)(owner + 0xC5F50), start = key * *(const uint32_t*)(owner + 0xC5F58), i = 0;
    while (i < count && slots[2 * ((start + i) & (count - 1))] != key && slots[2 * ((start + i) & (count - 1))] != 0) i++;
    return i + 1;
}

static void RecordHashFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, RH_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    uintptr_t game = (uintptr_t)image;
    memcpy(image + RH_SITE, kRhSitePin, sizeof kRhSitePin);
    image[RH_SITE + sizeof kRhSitePin] = 0xc3; /* ret: the thunk below runs the store alone */
    for (size_t i = 0; i < RH_READERS; i++) memcpy(image + kRhReaders[i].rva, kRhReaders[i].pin, kRhReaders[i].length);
    memcpy(image + RH_INSERT, kRhInsertCode, sizeof kRhInsertCode);
    memcpy(image + RH_LOOKUP, kRhLookupCode, sizeof kRhLookupCode);
    static const uint8_t thunk[] = {0x53, 0x48, 0x89, 0xcb, 0xe8, 0, 0, 0, 0, 0x5b, 0xc3}; /* push rbx; mov rbx, rcx; call; pop rbx */
    memcpy(image + RH_THUNK, thunk, sizeof thunk);
    int32_t rel = (int32_t)(RH_SITE - (RH_THUNK + 9));
    memcpy(image + RH_THUNK + 5, &rel, 4);
    RhSetupFn setup = (RhSetupFn)(void*)(image + RH_THUNK);
    RhInsertFn insert = (RhInsertFn)(void*)(image + RH_INSERT);
    RhLookupFn lookup = (RhLookupFn)(void*)(image + RH_LOOKUP);

    assert(RecordHashPinned(game));
    image[kRhReaders[9].rva + 1] ^= 1;
    assert(!RecordHashPinned(game)); /* a reader changed: another build */
    image[kRhReaders[9].rva + 1] ^= 1;
    uint8_t* game1 = RhMap(setup); /* the game's multiplier, before any rewrite */
    assert(*(uint32_t*)(game1 + 0xC5F58) == 1u);

    g_rhState = 0;
    RecordHashInstallAt(0);
    assert(g_rhState == 0); /* no game.dll yet: asked again later */
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "record-hash");
    RecordHashInstallAt(game);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    assert(g_rhState == -1 && memcmp(image + RH_SITE, kRhSitePin, sizeof kRhSitePin) == 0);
    g_rhState = 0;
    *(uint64_t*)(image + RH_SETUP_STARTED) = 1;
    RecordHashInstallAt(game);
    assert(g_rhState == -1 && memcmp(image + RH_SITE, kRhSitePin, sizeof kRhSitePin) == 0); /* the map may hold keys */
    *(uint64_t*)(image + RH_SETUP_STARTED) = 0;
    g_rhState = 0;
    image[RH_SITE + 6] = 2;
    RecordHashInstallAt(game);
    assert(g_rhState == -1 && image[RH_SITE + 6] == 2); /* another value there: left alone */
    image[RH_SITE + 6] = 1;
    g_rhState = 0;
    image[0x172F7A5u + 3] = 0x18;
    RecordHashInstallAt(game);
    assert(g_rhState == -1 && image[RH_SITE + 6] == 1);
    image[0x172F7A5u + 3] = 0x10;

    g_rhState = 0;
    RecordHashInstallAt(game);
    assert(g_rhState == 1 && memcmp(image + RH_SITE, kRhSitePin, 6) == 0 && *(uint32_t*)(image + RH_SITE_IMM) == RH_SCATTER);
    RecordHashInstallAt(game); /* once */
    uint8_t* fixed = RhMap(setup);
    assert(*(uint32_t*)(fixed + 0xC5F58) == RH_SCATTER);

    /* consecutive handles, as a level's destructibles get them; each record's stage byte (+7) says which it is */
    uint8_t* maps[2] = {game1, fixed};
    for (int m = 0; m < 2; m++)
        for (uint32_t i = 0; i < RH_KEYS; i++)
        {
            insert(maps[m] + 0xC5F48, 0x10002u + i, 0x10000u + i);
            maps[m][0xC5F78 + 12 * (0x10000u + i) + 7] = (uint8_t)(1 + i % 200);
        }
    uint64_t probes[2] = {0, 0};
    for (uint32_t key = 0x10002u; key < 0x10002u + RH_KEYS + 6000u; key += 3)
    {
        uint8_t found[2];
        for (int m = 0; m < 2; m++)
        {
            *(uint8_t**)(image + RH_GLOBAL) = maps[m];
            found[m] = lookup(NULL, key);
        }
        assert(found[0] == found[1]);
        assert(found[0] == (key < 0x10002u + RH_KEYS ? 1 + (key - 0x10002u) % 200 : 0xff));
    }
    for (uint32_t key = 0x38002u; key < 0x38002u + 5 * 4000u; key += 5) /* other entities' handles: misses */
        for (int m = 0; m < 2; m++)
        {
            *(uint8_t**)(image + RH_GLOBAL) = maps[m];
            assert(lookup(NULL, key) == 0xff);
            probes[m] += RhProbes(maps[m], key);
        }
    assert(probes[1] * 100 < probes[0]); /* the run of 20000 slots is spread */
    printf("record hash: pins, refusals, the setup's store, lookups alike, misses %llu -> %llu probes on average\n",
        (unsigned long long)(probes[0] / 4000), (unsigned long long)(probes[1] / 4000));
    VirtualFree(game1, 0, MEM_RELEASE);
    VirtualFree(fixed, 0, MEM_RELEASE);
    VirtualFree(image, 0, MEM_RELEASE);
    g_rhState = 0;
}
