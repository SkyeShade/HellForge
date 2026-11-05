/* Game data in a live change (live_mods.c, SDK 16: docs/modding/live.md): a mod's data patches and data spec reach the
 * running game as a hot package, a loose archive data/<16 hex> the session's tools wrote for the change, which the
 * engine loads by id like the game's own loose archives (its patch probe included). Its new resources are found by name
 * once it is resident; a resource it replaces gets a name of its own in it, and the original name is overridden to it,
 * so every lookup from then on (a unit spawned, a texture asked for, a Lua require) finds the new one. A mod taken out
 * takes its overrides with it; what was staged at boot comes back through overrides the session owns (owner "-"),
 * pointing the names at the game's own bytes in another hot package. Included by live_mods.c.
 *
 *   moddata <seq> <txn> <mod id or -> <package id hex>                         a hot package the change needs resident
 *   modres  <seq> <txn> <mod id or -> <type hex> <name hex> <target hex>       an override: lookups of name find target
 *
 * Engine (helldivers2.exe build 25480438, KB hellforge-live-mod-sync-re-runtime): the package C API table at 0x27C9E60;
 * load(id) at +0x2F0 (0x320DD0) changes an unlocked queue, so it runs on the game thread; whether a package is resident
 * is read from the engine's package list (packages.c PackageResident); set_resource_override(type, name, target) at
 * +0x378 (0x3217C0), clear(type, name) at +0x380 (0x321900) and can_get(type, name) at +0x388 (0x321930) take the
 * resource manager's own lock. Each slot and each function's bytes are checked before the first call. An override of a
 * resource already online changes later lookups only: objects that hold it keep it until they are made again.
 *
 * Packages are never unloaded in the session: an object made from one may still be alive. */

#define HOT_PACKAGES 64
#define HOT_OVERRIDES 4096
#define HOT_TABLE 0x27C9E60u
#define HOT_LOAD 0x320DD0u
#define HOT_OVERRIDE 0x3217C0u
#define HOT_CLEAR 0x321900u
#define HOT_CAN_GET 0x321930u
#define HOT_TIMEOUT_MS 90000u
static const uint8_t kHotLoadPin[22] = {0x48, 0x8B, 0xD1, 0x48, 0x8B, 0x0D, 0x2E, 0xF4, 0x6E, 0x01, 0x48, 0x8B, 0x89, 0x00, 0x04, 0x00, 0x00, 0xE9, 0x8A,
    0x28, 0x2E, 0x00};
static const uint8_t kHotOverridePin[64] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x05, 0x36, 0xEA, 0x6E, 0x01,
    0x4D, 0x8B, 0xF0, 0x48, 0x8B, 0xE9, 0x4C, 0x89, 0x44, 0x24, 0x60, 0x48, 0x89, 0x54, 0x24, 0x58, 0x4C, 0x8D, 0x44, 0x24, 0x58, 0x48, 0x89, 0x4C,
    0x24, 0x50, 0x48, 0x8D, 0x54, 0x24, 0x50, 0x48, 0x8B, 0xB0, 0xF8, 0x03, 0x00, 0x00, 0x48, 0x8B, 0xCE, 0xE8, 0x10, 0x1A, 0x2D, 0x00};
static const uint8_t kHotClearPin[48] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0x4C, 0x24, 0x38, 0x4C, 0x8D, 0x44, 0x24, 0x30, 0x48, 0x8B, 0x0D, 0xF3, 0xE8,
    0x6E, 0x01, 0x48, 0x89, 0x54, 0x24, 0x30, 0x48, 0x8D, 0x54, 0x24, 0x38, 0x48, 0x8B, 0x89, 0xF8, 0x03, 0x00, 0x00, 0xE8, 0xA5, 0x0E, 0x2D, 0x00, 0x48,
    0x83, 0xC4, 0x28, 0xC3};
static const uint8_t kHotCanGetPin[48] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0x4C, 0x24, 0x38, 0x4C, 0x8D, 0x44, 0x24, 0x30, 0x48, 0x8B, 0x0D, 0xC3, 0xE8,
    0x6E, 0x01, 0x48, 0x89, 0x54, 0x24, 0x30, 0x48, 0x8D, 0x54, 0x24, 0x38, 0x48, 0x8B, 0x89, 0xF8, 0x03, 0x00, 0x00, 0xE8, 0x05, 0x08, 0x2D, 0x00, 0x48,
    0x83, 0xC4, 0x28, 0xC3};

typedef void (*HotLoadFn)(uint64_t package);
typedef void (*HotOverrideFn)(uint64_t type, uint64_t name, uint64_t target);
typedef void (*HotClearFn)(uint64_t type, uint64_t name);
typedef uint8_t (*HotCanGetFn)(uint64_t type, uint64_t name);
/* The engine's functions once proven (the native fixture sets its own). */
static HotLoadFn g_hotLoad;
static HotOverrideFn g_hotOverride;
static HotClearFn g_hotClear;
static HotCanGetFn g_hotCanGet;
/* How long a change waits for its packages (the native fixture shortens it). */
static ULONGLONG g_hotTimeoutMs = HOT_TIMEOUT_MS;

/* The overrides live changes set, each with the mod whose it is ("-": the session's, kept whatever mods change). */
typedef struct HotOverride { char owner[80]; uint64_t type, name, target; } HotOverride;
static HotOverride g_hotOverrides[HOT_OVERRIDES];
static int g_hotOverrideCount;
/* A change's data lines. */
typedef struct TxnData { char owner[80]; uint64_t package; } TxnData;
typedef struct TxnRes { char owner[80]; uint64_t type, name, target; } TxnRes;

/* The engine's package and override functions, checked once: each table slot points at the reviewed function, and
 * its bytes are the reviewed ones. 1 when they are. */
static int HotProve(char* why, size_t size)
{
    if (g_hotLoad != NULL) return 1;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL), slot[4] = {0};
    static const uint32_t slots[4] = {0x2F0, 0x378, 0x380, 0x388}, functions[4] = {HOT_LOAD, HOT_OVERRIDE, HOT_CLEAR, HOT_CAN_GET};
    for (int i = 0; i < 4; i++)
        if (SafeRead(exe + HOT_TABLE + slots[i], &slot[i], 8) || slot[i] != exe + functions[i])
        { snprintf(why, size, "packages\tthe engine's package table is not the reviewed one (slot +0x%X)", slots[i]); return 0; }
    if (!PinMatches(exe + HOT_LOAD, kHotLoadPin, sizeof kHotLoadPin) || !PinMatches(exe + HOT_OVERRIDE, kHotOverridePin, sizeof kHotOverridePin) ||
        !PinMatches(exe + HOT_CLEAR, kHotClearPin, sizeof kHotClearPin) || !PinMatches(exe + HOT_CAN_GET, kHotCanGetPin, sizeof kHotCanGetPin))
    { snprintf(why, size, "packages\tthe engine's package and override functions are not the reviewed ones"); return 0; }
    g_hotOverride = (HotOverrideFn)(exe + HOT_OVERRIDE);
    g_hotClear = (HotClearFn)(exe + HOT_CLEAR);
    g_hotCanGet = (HotCanGetFn)(exe + HOT_CAN_GET);
    MemoryBarrier();
    g_hotLoad = (HotLoadFn)(exe + HOT_LOAD);
    return 1;
}

/* On the game thread: a load asked of every package the change needs that is not resident yet. */
static void HotLoadStep(void* arg)
{
    const TxnData* data = (const TxnData*)arg;
    for (int i = 0; data[i].package != 0; i++)
        if (PackageResident(data[i].package) != PKG_RESIDENT) g_hotLoad(data[i].package);
}

/* Every package of the change resident (asked for on the game thread, then waited for, at most 90 s), and every
 * override's target there. NULL, or "<reason>\t<why>". */
static const char* HotPrepare(TxnData* data, int dataCount, const TxnRes* res, int resCount, char* why, size_t size)
{
    if (dataCount == 0 && resCount == 0) return NULL;
    if (!HotProve(why, size)) return why;
    data[dataCount].package = 0; /* the step's end */
    if (dataCount > 0 && !g_liveOnGameThread(HotLoadStep, data)) { snprintf(why, size, "packages\tthe game thread did not take the packages' load"); return why; }
    ULONGLONG deadline = GetTickCount64() + g_hotTimeoutMs;
    for (int i = 0; i < dataCount; i++)
        while (PackageResident(data[i].package) != PKG_RESIDENT)
        {
            if (GetTickCount64() > deadline) { snprintf(why, size, "packages\tthe change's package %016llx did not load within 90 s", (unsigned long long)data[i].package); return why; }
            Sleep(100);
        }
    for (int i = 0; i < resCount; i++)
        if (!g_hotCanGet(res[i].type, res[i].target))
        {
            snprintf(why, size, "packages\tresource %016llx.%016llx is not in the change's packages", (unsigned long long)res[i].target, (unsigned long long)res[i].type);
            return why;
        }
    Note("live data: %d package(s) resident, %d override target(s) there", dataCount, resCount);
    return NULL;
}

static HotOverride* HotFind(uint64_t type, uint64_t name)
{
    for (int i = 0; i < g_hotOverrideCount; i++) if (g_hotOverrides[i].type == type && g_hotOverrides[i].name == name) return &g_hotOverrides[i];
    return NULL;
}

/* The overrides after a change: those of the mods it takes out or replaces go (cleared, unless the change sets the same
 * name again), then the change's own are set. gone: the owners whose overrides go. */
static void HotApply(const char* const* gone, int goneCount, const TxnRes* res, int resCount)
{
    if (g_hotOverride == NULL) return;
    for (int i = g_hotOverrideCount - 1; i >= 0; i--)
    {
        HotOverride* o = &g_hotOverrides[i];
        int leaves = 0, again = 0;
        for (int k = 0; k < goneCount && !leaves; k++) leaves = !strcmp(o->owner, gone[k]);
        for (int k = 0; k < resCount && !again; k++) again = res[k].type == o->type && res[k].name == o->name;
        if (!leaves) continue;
        if (!again) g_hotClear(o->type, o->name);
        g_hotOverrides[i] = g_hotOverrides[--g_hotOverrideCount];
    }
    for (int k = 0; k < resCount; k++)
    {
        HotOverride* o = HotFind(res[k].type, res[k].name);
        if (o == NULL && g_hotOverrideCount < HOT_OVERRIDES) o = &g_hotOverrides[g_hotOverrideCount++];
        if (o == NULL) { Note("live data: %d overrides already; %016llx is not redirected", HOT_OVERRIDES, (unsigned long long)res[k].name); continue; }
        *o = (HotOverride){"", res[k].type, res[k].name, res[k].target};
        snprintf(o->owner, sizeof o->owner, "%s", res[k].owner);
        g_hotOverride(res[k].type, res[k].name, res[k].target);
    }
    Note("live data: %d override(s) set, %d held", resCount, g_hotOverrideCount);
}

/* moddata / modres fields: an owner ("-" or a mod of the change that carries data) and hex numbers. 0 when malformed. */
static int HotHex(const char* text, uint64_t* out)
{
    char* end;
    if (strlen(text) != 16) return 0;
    *out = strtoull(text, &end, 16);
    return *end == 0 && *out != 0;
}
