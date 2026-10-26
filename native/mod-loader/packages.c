/* The game thread and packages (SDK 6: on_game_thread, load_package; refs.c's donor packages). Included by
 * hellforge_modloader.c before stats.c.
 *
 * The game thread: the menu sound manager's update (game.dll +0x13270B0) runs every frame on the game thread (the
 * in-game editor's sounds, which the engine takes only from that thread, proved it on 2026-10-17). Once its first 32
 * bytes read as reviewed it is hooked; the package requests and the mods' on_game_thread callbacks run there, before
 * the game's own update.
 *
 * Packages (HD2Runtime 0.40.1 core/assets.lua and domains/package_residency.lua; reference_data.h): HellForge takes a
 * reference of its own on a package through the game's RefcountedPackageSystem, request(system, ids, 1) at game.dll
 * +0x12FE350 (its 384 bytes checked), with the system at [game.dll +0x347CEB0]. The request changes the system's
 * reference map with no lock, so it is made only on the game thread; once per package per session, never released. The
 * map has no overflow path (a full one writes to address 0), so nothing is requested with it 3/4 full, and HellForge
 * holds at most 64 packages. Whether a package is resident is read from the engine's package list (helldivers2.exe;
 * its has_loaded and package lookup functions checked): known with every part in state 4; else loading, queued or
 * absent. A package not resident 90 s after its request has failed. Only the catalogue's packages are requested.
 *
 * Co-op (docs/multiplayer.md "Assets in co-op"): every player keeps their own edits, and what a friend's edit puts in the
 * game (a vehicle's swapped weapon, a pod's pickup) is created on every machine, so each machine needs its package. The
 * packages this machine holds for its own edits, mods and actions are its "own": the live pipe sends them to the
 * session's backend whenever they change (live.c "packages"), the backends gather the lobby's, and each machine asks for
 * its friends' (live.c "preload", PackagesForFriends) as it does for its own. Friends' packages leave 16 of the 64 for
 * this machine's own. */
#include "reference_data.h"

#define TICK_UPDATE 0x13270B0u
#define TICK_MAX 16
#define PKG_MAX_HELD 64
#define PKG_OWN_RESERVE 16
#define PKG_TIMEOUT_MS 90000u
#define PKG_FILL_LIMIT 0.75

static const uint8_t kTickPin[32] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0x83, 0xb9, 0x58, 0x46, 0x00, 0x00, 0x00, 0x48, 0x8b,
    0xd9, 0x0f, 0x29, 0x7c, 0x24, 0x20, 0x0f, 0x28, 0xf9, 0x0f, 0x84, 0x0d, 0x02, 0x00, 0x00, 0x48};

typedef void (*TickFn)(void* manager, float dt);
typedef struct TickCallback { HfMod* mod; HfTickCallback callback; void* user; } TickCallback;
static TickFn g_tickOriginal;
static int g_tickState;                 /* 0 not hooked yet, 1 hooked, -1 refused */
static ULONGLONG g_tickFirstTry;
static TickCallback g_ticks[TICK_MAX];
static volatile LONG g_tickCount;
static volatile LONG64 g_tickLast;      /* when the game thread last ran it (GetTickCount64) */

enum { PKG_WAITING, PKG_RESIDENT, PKG_FAILED };
typedef struct HeldPackage
{
    uint64_t id;
    const char* name;
    ULONGLONG since;    /* asked for; requested: the request */
    int requested;
    int failed;
    int own;            /* for this machine's edits, mods or actions (sent to friends); else only for a friend's */
    char why[160];
} HeldPackage;
static HeldPackage g_held[PKG_MAX_HELD];
static int g_heldCount;
static SRWLOCK g_packagesLock = SRWLOCK_INIT;
static int g_packagesProven;            /* 1 the pins read as reviewed */
static uintptr_t g_packagesGame, g_packagesExe; /* the native fixture's images (else game.dll and the executable) */
static uint64_t (*g_packageState)(uint64_t id); /* the native fixture's engine (PKG_... states), else the list is read */
static void (*g_packageRequest)(uintptr_t system, const uint64_t* ids, uint32_t count); /* the fixture's, else the game's */

static uintptr_t PackagesGame(void) { return g_packagesGame ? g_packagesGame : ApiModuleBase("game.dll", NULL); }
static uintptr_t PackagesExe(void) { return g_packagesExe ? g_packagesExe : (uintptr_t)GetModuleHandleW(NULL); }

static int PinMatches(uintptr_t at, const uint8_t* pin, size_t size)
{
    uint8_t bytes[512];
    return at != 0 && size <= sizeof bytes && !HookedCode(at, bytes, size) && !memcmp(bytes, pin, size); /* a hook in front is no change */
}

static int PackagesProve(char* why, size_t whySize)
{
    if (g_packagesProven) return 1;
    uintptr_t game = PackagesGame(), exe = PackagesExe();
    if (!PinMatches(game + PKG_REQUEST, kPackageRequestPin, sizeof kPackageRequestPin))
    { snprintf(why, whySize, "game.dll's package request is not the reviewed one"); return 0; }
    if (!PinMatches(exe + PKG_EXE_HAS_LOADED, kPackageHasLoadedPin, sizeof kPackageHasLoadedPin) ||
        !PinMatches(exe + PKG_EXE_FIND, kPackageFindPin, sizeof kPackageFindPin))
    { snprintf(why, whySize, "the engine's package list is not the reviewed one"); return 0; }
    g_packagesProven = 1;
    return 1;
}

/* The package system and how full its reference map is (0..1), or 0 if it is not as reviewed. */
static uintptr_t PackageSystem(double* fill)
{
    uintptr_t system = 0, entries = 0;
    uint32_t capacity = 0;
    if (SafeRead(PackagesGame() + PKG_SYSTEM, &system, 8) || !system || SafeRead(system + PKG_MAP_ENTRIES_AT, &entries, 8) ||
        SafeRead(system + PKG_MAP_CAPACITY_AT, &capacity, 4) || !entries || capacity < 16 || capacity > 65536 || capacity % 2) return 0;
    uint8_t* map = (uint8_t*)malloc((size_t)capacity * PKG_MAP_STRIDE);
    if (map == NULL) return 0;
    uint32_t used = 0;
    int unreadable = SafeRead(entries, map, (size_t)capacity * PKG_MAP_STRIDE);
    for (uint32_t i = 0; !unreadable && i < capacity; i++)
    {
        uint64_t key;
        memcpy(&key, map + (size_t)i * PKG_MAP_STRIDE, 8);
        if (key) used++;
    }
    free(map);
    if (unreadable) return 0;
    *fill = (double)used / capacity;
    return system;
}

/* A package's parts: PKG_RESIDENT when every one is loaded, else PKG_WAITING. */
static int PackagePartsState(uintptr_t package)
{
    uintptr_t parts = 0;
    uint32_t partCount = 0;
    if (SafeRead(package + PKG_PART_COUNT, &partCount, 4) || partCount == 0 || partCount > 64 ||
        SafeRead(package + PKG_PARTS, &parts, 8) || !parts) return PKG_WAITING;
    for (uint32_t p = 0; p < partCount; p++)
    {
        uintptr_t part = 0;
        uint32_t state = 0;
        if (SafeRead(parts + p * 8u, &part, 8) || !part || SafeRead(part + PKG_PART_STATE, &state, 4) || state != PKG_LOADED)
            return PKG_WAITING;
    }
    return PKG_RESIDENT;
}

/* Whether the engine has a package: PKG_RESIDENT (every part loaded), else PKG_WAITING (loading, queued, absent or
 * unreadable). */
static int PackageResident(uint64_t id)
{
    if (g_packageState) return (int)g_packageState(id);
    uintptr_t exe = PackagesExe(), engine = 0, manager = 0, resources = 0, list = 0;
    uint32_t count = 0;
    if (SafeRead(exe + PKG_EXE_ENGINE, &engine, 8) || !engine || SafeRead(engine + PKG_MANAGER, &manager, 8) || !manager ||
        SafeRead(manager + PKG_RESOURCES, &resources, 8) || !resources || SafeRead(resources + PKG_LIST_COUNT, &count, 4) ||
        count > 8192 || (count && (SafeRead(resources + PKG_LIST, &list, 8) || !list))) return PKG_WAITING;
    for (uint32_t i = 0; i < count; i++)
    {
        uintptr_t package = 0;
        uint64_t pid = 0;
        if (SafeRead(list + i * 8u, &package, 8) || !package || SafeRead(package + PKG_ID, &pid, 8) || pid != id) continue;
        return PackagePartsState(package);
    }
    return PKG_WAITING;
}

static const RefPackage* CataloguePackage(uint64_t id)
{
    for (size_t i = 0; i < sizeof kRefPackages / sizeof kRefPackages[0]; i++) if (kRefPackages[i].id == id) return &kRefPackages[i];
    return NULL;
}

static void LivePackagesChanged(void); /* live.c: this machine's own packages changed, for the session's backend */

static HeldPackage* PackageHeld(uint64_t id)
{
    HeldPackage* held = NULL;
    for (int i = 0; i < g_heldCount; i++) if (g_held[i].id == id) held = &g_held[i];
    return held;
}

/* A catalogue package held from now on, wanted first for this machine (own) or a friend's change. g_packagesLock held. */
static HeldPackage* PackageHold(uint64_t id, const RefPackage* known, int own, int* added)
{
    HeldPackage* held = &g_held[g_heldCount++];
    memset(held, 0, sizeof *held);
    held->id = id;
    held->name = known->name;
    held->since = GetTickCount64();
    held->own = own;
    if (added != NULL) *added = 1;
    if (g_tickState < 0) { held->failed = 1; snprintf(held->why, sizeof held->why, "the game thread's update could not be hooked"); }
    Note("package %016llx (%s) wanted%s", (unsigned long long)id, known->name, own ? "" : " for a friend's change");
    return held;
}

/* A held package's state: failed, resident once requested, else waiting until 90 s are up (then failed, logged; *changed
 * when it was this machine's own). g_packagesLock held. */
static int PackageHeldState(HeldPackage* held, uint64_t id, int* changed)
{
    if (held->failed) return PKG_FAILED;
    if (held->requested && PackageResident(id) == PKG_RESIDENT) return PKG_RESIDENT;
    if (GetTickCount64() - held->since > PKG_TIMEOUT_MS)
    {
        held->failed = 1;
        *changed |= held->own;
        snprintf(held->why, sizeof held->why, "it was %s for 90 seconds", held->requested ? "not resident" : "not requested");
        Note("package %016llx (%s) failed: %s", (unsigned long long)id, held->name, held->why);
        return PKG_FAILED;
    }
    return PKG_WAITING;
}

/* A catalogue package HellForge wants resident: asked for on the first call (the game thread requests it), then
 * PKG_WAITING until the engine has every part, PKG_RESIDENT, or PKG_FAILED (*why says why). own: for this machine's
 * edits, mods or actions; else for a friend's (PackagesForFriends), which may not take the last PKG_OWN_RESERVE places.
 * *added (may be NULL): 1 when this call asked for it first. */
static int PackageWantAs(uint64_t id, int own, const char** name, const char** why, int* added)
{
    static const char* const notInCatalogue = "it is not a package HellForge's catalogue names";
    const RefPackage* known = CataloguePackage(id);
    *name = known != NULL ? known->name : "?";
    if (added != NULL) *added = 0;
    if (known == NULL) { *why = notInCatalogue; return PKG_FAILED; }
    AcquireSRWLockExclusive(&g_packagesLock);
    HeldPackage* held = PackageHeld(id);
    int changed = 0;
    if (held == NULL && g_heldCount >= (own ? PKG_MAX_HELD : PKG_MAX_HELD - PKG_OWN_RESERVE))
    {
        *why = own ? "HellForge holds 64 packages already (its budget)" :
            "friends' packages leave 16 of HellForge's 64 for this machine's own, and the rest are taken";
        ReleaseSRWLockExclusive(&g_packagesLock);
        return PKG_FAILED;
    }
    if (held == NULL) { held = PackageHold(id, known, own, added); changed = own; }
    else if (own && !held->own) { held->own = 1; changed = 1; } /* a friend's first, now this machine's too */
    int result = PackageHeldState(held, id, &changed);
    *why = held->why;
    ReleaseSRWLockExclusive(&g_packagesLock);
    if (changed) LivePackagesChanged();
    return result;
}

static int PackageWant(uint64_t id, const char** name, const char** why) { return PackageWantAs(id, 1, name, why, NULL); }

/* This machine's own packages, for the live pipe's "packages" line: ids in hex, comma separated, "-" for none. Failed
 * ones are left out (no friend needs them for an edit that never applied here). */
static void PackagesOwnList(char* out, size_t size)
{
    size_t used = 0;
    out[0] = 0;
    AcquireSRWLockShared(&g_packagesLock);
    for (int i = 0; i < g_heldCount && used + 18 < size; i++)
        if (g_held[i].own && !g_held[i].failed)
            used += (size_t)snprintf(out + used, size - used, "%s%016llx", used ? "," : "", (unsigned long long)g_held[i].id);
    ReleaseSRWLockShared(&g_packagesLock);
    if (used == 0) snprintf(out, size, "-");
}

/* The lobby's packages from the session's backend: each asked for as this machine's own are, for a friend's change.
 * Counts what is resident, loading and refused, and how many were asked for the first time; *why, the first refusal. */
static void PackagesForFriends(const uint64_t* ids, int count, int* resident, int* loading, int* refused, int* fresh, const char** why)
{
    *resident = *loading = *refused = *fresh = 0;
    *why = NULL;
    for (int i = 0; i < count; i++)
    {
        const char *name, *reason;
        int added = 0, state = PackageWantAs(ids[i], 0, &name, &reason, &added);
        *fresh += added;
        if (state == PKG_RESIDENT) (*resident)++;
        else if (state == PKG_WAITING) (*loading)++;
        else { (*refused)++; if (*why == NULL) *why = reason; }
    }
}

/* On the game thread: requests the packages wanted since the last update. */
static void PackagesRequest(void)
{
    if (g_heldCount == 0) return;
    int changed = 0;
    AcquireSRWLockExclusive(&g_packagesLock);
    for (int i = 0; i < g_heldCount; i++)
    {
        HeldPackage* held = &g_held[i];
        if (held->requested || held->failed) continue;
        double fill = 1;
        uintptr_t system = 0;
        if (!PackagesProve(held->why, sizeof held->why)) held->failed = 1;
        else if ((system = PackageSystem(&fill)) == 0) continue; /* not set up yet: the timeout ends the wait */
        else if (fill >= PKG_FILL_LIMIT)
        { held->failed = 1; snprintf(held->why, sizeof held->why, "the game's package reference map is %.0f%% full", fill * 100); }
        if (held->failed)
        {
            changed |= held->own;
            Note("package %016llx (%s) not requested: %s", (unsigned long long)held->id, held->name, held->why);
            continue;
        }
        typedef void (*RequestFn)(uintptr_t system, const uint64_t* ids, uint32_t count);
        (g_packageRequest ? g_packageRequest : (RequestFn)(PackagesGame() + PKG_REQUEST))(system, &held->id, 1);
        held->requested = 1;
        held->since = GetTickCount64();
        Note("package %016llx (%s) requested (reference map %.0f%% full, %d held)", (unsigned long long)held->id, held->name, fill * 100, g_heldCount);
    }
    ReleaseSRWLockExclusive(&g_packagesLock);
    if (changed) LivePackagesChanged();
}

static void EventsTick(void); /* events.c */

static void TickDetour(void* manager, float dt)
{
    InterlockedExchange64(&g_tickLast, (LONG64)GetTickCount64());
    EventsTick();
    PackagesRequest();
    LONG count = g_tickCount;
    for (LONG i = 0; i < count; i++) g_ticks[i].callback(g_ticks[i].mod, dt, g_ticks[i].user);
    g_tickOriginal(manager, dt);
}

/* The watch loop, after HF_EVENT_GAME_LOADED: hooks the update once its bytes are decoded and as reviewed (60 s). */
static void TickInstall(void)
{
    if (g_tickState != 0) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    ULONGLONG now = GetTickCount64();
    if (!g_tickFirstTry) g_tickFirstTry = now;
    if (!game || strcmp(g_build, "25480438")) { g_tickState = -1; Note("game thread: not hooked (unsupported build)"); return; }
    if (!PinMatches(game + TICK_UPDATE, kTickPin, sizeof kTickPin))
    {
        if (now - g_tickFirstTry < 60000) return;
        g_tickState = -1;
        Note("game thread: the menu sound manager's update is not the reviewed one; on_game_thread and package loading are off");
        return;
    }
    g_tickState = LoaderHook("game thread", game + TICK_UPDATE, (void*)TickDetour, (void**)&g_tickOriginal) ? -1 : 1;
    Note(g_tickState == 1 ? "game thread: hooked the menu sound manager's update" : "game thread: could not hook the update");
}

/* SDK 6: a callback every game update, on the game thread. */
static int ApiOnGameThread(HfMod* mod, HfTickCallback callback, void* user)
{
    if (mod == NULL || callback == NULL) return 0;
    if (g_tickState < 0) { ApiLog(mod, "on_game_thread: the game thread's update is not hooked (modloader.log says why)"); return 0; }
    AcquireSRWLockExclusive(&g_lock);
    int ok = g_tickCount < TICK_MAX;
    if (ok) { g_ticks[g_tickCount] = (TickCallback){mod, callback, user}; InterlockedIncrement(&g_tickCount); }
    ReleaseSRWLockExclusive(&g_lock);
    if (!ok) ApiLog(mod, "on_game_thread: mods registered %d callbacks already", TICK_MAX);
    return ok;
}

/* SDK 6: a catalogue package loaded and held for the session. */
static int ApiLoadPackage(HfMod* mod, uint64_t package)
{
    const char *name, *why;
    int state = PackageWant(package, &name, &why);
    if (state == PKG_FAILED) ApiLog(mod, "load_package(%016llx, %s): %s", (unsigned long long)package, name, why);
    return state == PKG_RESIDENT ? HF_PACKAGE_RESIDENT : state == PKG_WAITING ? HF_PACKAGE_LOADING : 0;
}
