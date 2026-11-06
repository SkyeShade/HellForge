/* Shields that stop grenades (the stats field shield.blocks_grenades, a byte in the Shield record's trailing padding).
 * A sphere shield is a real Havok body and only the collision matrix keeps grenades out of it, so the pair goes in while
 * such a shield stands. The matrix is shared by every shield and the sphere is solid, so a detour on the filter's pair
 * check drops the pair for every other shield, and for a grenade already inside: it leaves as a bullet does. */

#define SB_MANAGER 0x3326630u        /* game.dll: the Shield manager */
#define SB_RECORD_OF 0x4FB390u       /* game.dll: a shield's record, the way the game's own readers get it */
#define SB_FILTER_PAIRS 0xCE5A10u    /* exe: slot +0x18 of both collision filters, called for new pairs of nearby bodies */
#define SB_HANDLE_BODY 0x77D6F0u     /* exe: a physics handle's world and body id */
#define SB_PROPERTIES 0x27C5E48u     /* exe: +0xF8 filter count, +0x100 their name hashes */
#define SB_FILTER_A 0x27C5E40u
#define SB_FILTER_B 0x27C5B90u
#define SB_MATRIX 0x28u              /* 16 bytes for each filter, a bit for each other filter */
#define SB_FLAG 0x151u
#define SB_MAX 32
#define SB_BODY_STRIDE 0xA0u
#define SB_HASH_SHIELD 0xC61AD6E5u
#define SB_HASH_GRENADE 0xB03B6BFFu
#define SB_NO_BODY 0xFFFFFFu
#define SB_BALL 0x16F397CAu          /* a stratagem ball: its resource's top half, as a body holds it at +0x98 */

static const uint8_t kShieldBlockPin[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x41, 0x56, 0x49, 0x8B, 0xE8};

typedef struct SbShield { uint32_t body; float radius; } SbShield;
typedef struct SbList { int count; SbShield shields[SB_MAX]; } SbList;
typedef int (*SbPairsFn)(void* filter, uintptr_t world, uint32_t* pairs, int count);

static SbPairsFn g_sbPairsOriginal;
static SbList g_sbLists[2];
static SbList* volatile g_sbActive;   /* read on the physics threads; NULL leaves every pair as the game decided */
static int g_sbNext, g_sbState, g_sbShieldRow = -1, g_sbGrenadeRow = -1, g_sbPaired;
static uint32_t g_sbTicks;
static ULONGLONG g_sbFirstTry;

/* Whether a pair the matrix allows stays: not for a shield off the list, a body inside the sphere or a stratagem ball,
 * which shares the grenades' filter but must reach the ground. */
static int ShieldBlockKeeps(const SbList* list, uint32_t shield, const uint8_t* shieldBody, const uint8_t* body)
{
    if (*(const uint32_t*)(body + 0x98) == SB_BALL) return 0;
    const float* shieldAt = (const float*)(shieldBody + 0x30), *grenadeAt = (const float*)(body + 0x30);
    for (int i = 0; i < list->count; i++)
    {
        if (list->shields[i].body != shield) continue;
        float x = grenadeAt[0] - shieldAt[0], y = grenadeAt[1] - shieldAt[1], z = grenadeAt[2] - shieldAt[2];
        return x * x + y * y + z * z >= list->shields[i].radius * list->shields[i].radius;
    }
    return 0;
}

static int ShieldBlockPairsDetour(void* filter, uintptr_t world, uint32_t* pairs, int count)
{
    int kept = g_sbPairsOriginal(filter, world, pairs, count);
    const SbList* list = g_sbActive;
    if (list == NULL || kept <= 0) return kept;
    const uint8_t* bodies = *(const uint8_t**)(world + 0x38);
    int out = 0;
    for (int i = 0; i < kept; i++)
    {
        uint32_t a = pairs[i * 2] & SB_NO_BODY, b = pairs[i * 2 + 1] & SB_NO_BODY;
        const uint8_t* bodyA = bodies + (uintptr_t)a * SB_BODY_STRIDE;
        const uint8_t* bodyB = bodies + (uintptr_t)b * SB_BODY_STRIDE;
        int rowA = *(const uint32_t*)(bodyA + 0x6C) & 0x7F, rowB = *(const uint32_t*)(bodyB + 0x6C) & 0x7F, keep = 1;
        if (rowA == g_sbShieldRow && rowB == g_sbGrenadeRow)
            keep = ShieldBlockKeeps(list, a, bodyA, bodyB);
        else if (rowB == g_sbShieldRow && rowA == g_sbGrenadeRow)
            keep = ShieldBlockKeeps(list, b, bodyB, bodyA);
        if (keep)
        {
            pairs[out * 2] = pairs[i * 2];
            pairs[out * 2 + 1] = pairs[i * 2 + 1];
            out++;
        }
    }
    return out;
}

/* A filter's row is its place in the physics properties, so rows are found by name. */
static int ShieldBlockRows(uintptr_t exe)
{
    uintptr_t properties, keys;
    uint32_t count, key;
    if (!EvpPtr(exe + SB_PROPERTIES, &properties) || SafeRead(properties + 0xF8, &count, 4) || !EvpPtr(properties + 0x100, &keys)) return 0;
    for (uint32_t i = 0; i < count && i < 128; i++)
    {
        if (SafeRead(keys + i * 4, &key, 4)) return 0;
        if (key == SB_HASH_SHIELD) g_sbShieldRow = (int)i;
        if (key == SB_HASH_GRENADE) g_sbGrenadeRow = (int)i;
    }
    return g_sbShieldRow >= 0 && g_sbGrenadeRow >= 0;
}

static void ShieldBlockBit(uintptr_t filter, int row, int bit, int on)
{
    volatile LONG* word = (volatile LONG*)(filter + SB_MATRIX + (uintptr_t)row * 16 + (uintptr_t)(bit >> 5) * 4);
    if (on) InterlockedOr(word, (LONG)(1u << (bit & 31)));
    else InterlockedAnd(word, (LONG)~(1u << (bit & 31)));
}

static void ShieldBlockPair(uintptr_t exe, int on)
{
    const uint32_t globals[2] = {SB_FILTER_A, SB_FILTER_B};
    for (int i = 0; i < 2; i++)
    {
        uintptr_t filter;
        if (!EvpPtr(exe + globals[i], &filter)) continue;
        ShieldBlockBit(filter, g_sbGrenadeRow, g_sbShieldRow, on);
        ShieldBlockBit(filter, g_sbShieldRow, g_sbGrenadeRow, on);
    }
    g_sbPaired = on;
}

/* Adds the shield at a manager index, if its record has the field on and its body exists. */
static void ShieldBlockAdd(uintptr_t game, uintptr_t exe, uintptr_t bindings, uintptr_t rows, uint32_t index, SbList* list)
{
    void* (*recordOf)(void*) = (void* (*)(void*))(game + SB_RECORD_OF);
    uint32_t (*bodyOf)(uint32_t, uint32_t*) = (uint32_t (*)(uint32_t, uint32_t*))(exe + SB_HANDLE_BODY);
    uint32_t handle, body = SB_NO_BODY;
    uintptr_t binding;
    if (!EvpPtr(bindings + (uintptr_t)index * 8, &binding)) return;
    const uint8_t* record = (const uint8_t*)recordOf((void*)binding);
    if (record == NULL || record[SB_FLAG] == 0) return;
    float radius = *(const float*)record;
    if (radius <= 0 || SafeRead(rows + (uintptr_t)index * 0x88, &handle, 4) || handle == 0 || handle == 0xFFFFFFFFu) return;
    bodyOf(handle, &body);
    body &= SB_NO_BODY; /* an actor's id has a generation in its top byte; the filter's pairs compare the low 24 bits */
    if (body == SB_NO_BODY) return;
    list->shields[list->count].body = body;
    list->shields[list->count].radius = radius;
    list->count++;
}

/* The shields standing now that have the field on. On the game thread, which adds and removes them. */
static void ShieldBlockScan(uintptr_t game, uintptr_t exe, SbList* list)
{
    list->count = 0;
    uintptr_t manager, bindings, rows, table;
    uint32_t capacity, empty;
    if (!EvpPtr(game + SB_MANAGER, &manager) || !EvpPtr(manager + 0x30, &table)) return;
    if (SafeRead(manager + 0x38, &capacity, 4) || SafeRead(manager + 0x3C, &empty, 4) || capacity > 65536) return;
    if (!EvpPtr(manager + 0x48, &bindings) || !EvpPtr(manager + 0x50, &rows)) return;
    const uint32_t* entries = (const uint32_t*)table;
    for (uint32_t i = 0; i < capacity && list->count < SB_MAX; i++)
        if (entries[i * 2] != empty && entries[i * 2 + 1] != 0xFFFFFFFFu) ShieldBlockAdd(game, exe, bindings, rows, entries[i * 2 + 1], list);
}

static void ShieldBlockTick(void)
{
    if (g_sbState != 1 || (++g_sbTicks & 3)) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL), exe = (uintptr_t)GetModuleHandleW(NULL);
    SbList* next = &g_sbLists[g_sbNext];
    ShieldBlockScan(game, exe, next);
    if (next->count && g_sbShieldRow < 0 && !ShieldBlockRows(exe))
    {
        g_sbState = -1;
        g_sbActive = NULL;
        Note("shield block: the physics filters are not the reviewed ones; shields stop no grenades");
        return;
    }
    /* The list goes live before the pair is set, so no shield passes the matrix unchecked. */
    g_sbActive = next->count ? next : NULL;
    g_sbNext ^= 1;
    if ((next->count != 0) != g_sbPaired)
    {
        ShieldBlockPair(exe, next->count != 0);
        Note("shield block: grenades %s", next->count ? "stop at the marked shields" : "pass every shield again");
    }
}

/* From game load, like the game thread: the exe's code is decoded by then, or soon after. */
static void ShieldBlockInstall(void)
{
    if (g_sbState != 0) return;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    ULONGLONG now = GetTickCount64();
    if (!g_sbFirstTry) g_sbFirstTry = now;
    if (strcmp(g_build, "25480438")) { g_sbState = -1; return; }
    if (!PinMatches(exe + SB_FILTER_PAIRS, kShieldBlockPin, sizeof kShieldBlockPin))
    {
        if (now - g_sbFirstTry < 60000) return;
        g_sbState = -1;
        Note("shield block: the collision filter is not the reviewed one; shields stop no grenades");
        return;
    }
    g_sbState = LoaderHook("shield block", exe + SB_FILTER_PAIRS, (void*)ShieldBlockPairsDetour, (void**)&g_sbPairsOriginal) ? -1 : 1;
}
