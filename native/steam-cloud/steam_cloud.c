// HellForge stand-in for the copied game's steam_api64.dll. Every export it does not write itself (the HELLFORGE_API
// functions: remote storage, the flat API's init, callbacks and shutdown) is a loader forwarder to the real DLL, which the
// session places next to it as steam_api64_hellforge.dll (HellForge.SdkShimGen forward writes the .def): sign-in,
// ownership and everything else stay Valve's own code.
//
// The bridge build (HELLFORGE_BRIDGE_BUILD, steam_api64_bridge: docs/steam-bridge.md) answers everything through the
// session's broker instead: the interfaces are the generated proxies (bridge_proxies_gen.c), the callbacks are dispatched
// from the broker's ring (bridge_local.c), and no export forwards, so Valve's DLL never loads in the game. The forwarding
// build refuses to start when a session asks for the bridge (HELLFORGE_BRIDGE_PIPE), rather than run on Valve's DLL.
//
// The game keeps a cloud save (testament_new.sav: loadout, title, player card) and input_settings.config in Steam
// Cloud, which belongs to the Steam account and is shared with the regular game and uploaded to Valve. The stand-in
// interface keeps those files in HELLFORGE_CLOUD_DIR (the profile's folder inside the installation) instead. The game's
// executable calls only FileWrite, FileRead, FileForget, FileDelete, FileExists, GetFileSize and the two
// IsCloudEnabled checks (docs/steam-cloud.md); every other method fails without side effects. Without HELLFORGE_CLOUD_DIR
// cloud storage reports itself disabled: the real storage is never used. Calls are logged to HELLFORGE_CLOUD_LOG if set.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "net_gate.h"
#include "steam_census.h"
#include "bridge_channel.h"
#include "../common/hf_privacy.h"
#ifdef HELLFORGE_BRIDGE_BUILD
#include "bridge_local.h"
void BridgeContextsChanged(void); // bridge_exports.c
#endif

#define HELLFORGE_API
#define STORAGE_VERSION "STEAMREMOTESTORAGE_INTERFACE_VERSION016"
#define REAL_MODULE L"steam_api64_hellforge.dll"
#define SLOT_COUNT 59
#define MAX_FILE_BYTES (64 * 1024 * 1024)

static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static wchar_t g_dir[MAX_PATH];
static bool g_enabled;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HMODULE g_self;
static GameChannel g_bridge;
static bool g_bridged;

static void Log(const char *format, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    char line[512];
    SYSTEMTIME now;
    GetSystemTime(&now);
    int length = snprintf(line, sizeof line, "%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_list arguments;
    va_start(arguments, format);
    int more = vsnprintf(line + length, sizeof line - length - 1, format, arguments);
    va_end(arguments);
    length += more < 0 ? 0 : (more > (int)sizeof line - length - 2 ? (int)sizeof line - length - 2 : more);
    length = (int)HfPrivacyScrub(line, (size_t)length, sizeof line - 1);
    line[length++] = '\n';
    DWORD written;
    WriteFile(g_log, line, (DWORD)length, &written, NULL);
}

// For the bridge's own files (bridge_local.c, bridge_exports.c).
void BridgeLog(const char *format, ...)
{
    char line[400];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    Log("%s", line);
}

static BOOL CALLBACK Initialize(PINIT_ONCE once, void *parameter, void **context)
{
    wchar_t log[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_CLOUD_LOG", log, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        g_log = CreateFileW(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    length = GetEnvironmentVariableW(L"HELLFORGE_CLOUD_DIR", g_dir, MAX_PATH - 64);
    DWORD attributes = length > 0 && length < MAX_PATH - 64 ? GetFileAttributesW(g_dir) : INVALID_FILE_ATTRIBUTES;
    g_enabled = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    Log("start: cloud storage %s", g_enabled ? "in HELLFORGE_CLOUD_DIR" : "disabled (HELLFORGE_CLOUD_DIR missing)");
    return TRUE;
}

static void EnsureInitialized(void) { InitOnceExecuteOnce(&g_once, Initialize, NULL, NULL); }

// Cloud file names are plain names ("testament_new.sav"); anything that could leave the folder is refused.
static bool PathFor(const char *name, wchar_t *path)
{
    if (!g_enabled || !name || !name[0] || strlen(name) > 128 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (const char *c = name; *c; c++)
        if (*c < 0x20 || strchr("\\/:*?\"<>|", *c)) return false;
    wchar_t wide[160];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, 160)) return false;
    return swprintf(path, MAX_PATH, L"%ls\\%ls", g_dir, wide) > 0;
}

static int32_t SizeOf(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return data.nFileSizeHigh || data.nFileSizeLow > MAX_FILE_BYTES ? -1 : (int32_t)data.nFileSizeLow;
}

// Slot 0. Written to a temporary file first, so a save is either the old or the new one, never half of each.
static bool StorageFileWrite(void *self, const char *name, const void *data, int32_t size)
{
    wchar_t path[MAX_PATH], temporary[MAX_PATH];
    if (!PathFor(name, path) || size < 0 || size > MAX_FILE_BYTES || (!data && size)) { Log("FileWrite %s refused", name ? name : "(null)"); return false; }
    swprintf(temporary, MAX_PATH, L"%ls.hellforge-tmp", path);
    EnterCriticalSection(&g_lock);
    bool ok = false;
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        ok = WriteFile(file, data, (DWORD)size, &written, NULL) && written == (DWORD)size && FlushFileBuffers(file);
        CloseHandle(file);
        ok = ok && MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok) DeleteFileW(temporary);
    }
    LeaveCriticalSection(&g_lock);
    Log("FileWrite %s %d bytes %s", name, size, ok ? "ok" : "failed");
    return ok;
}

// Slot 1. Returns the number of bytes read (at most the buffer size), 0 when the file is missing.
static int32_t StorageFileRead(void *self, const char *name, void *buffer, int32_t capacity)
{
    wchar_t path[MAX_PATH];
    int32_t result = 0;
    if (PathFor(name, path) && buffer && capacity > 0)
    {
        EnterCriticalSection(&g_lock);
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD read = 0;
            if (ReadFile(file, buffer, (DWORD)capacity, &read, NULL)) result = (int32_t)read;
            CloseHandle(file);
        }
        LeaveCriticalSection(&g_lock);
    }
    Log("FileRead %s capacity %d -> %d", name ? name : "(null)", capacity, result);
    return result;
}

// Slot 5. Steam would stop syncing the file but keep it locally; there is nothing to sync here.
static bool StorageFileForget(void *self, const char *name)
{
    wchar_t path[MAX_PATH];
    bool exists = PathFor(name, path) && SizeOf(path) >= 0;
    Log("FileForget %s -> %d", name ? name : "(null)", exists);
    return exists;
}

// Slot 6.
static bool StorageFileDelete(void *self, const char *name)
{
    wchar_t path[MAX_PATH];
    bool ok = false;
    if (PathFor(name, path))
    {
        EnterCriticalSection(&g_lock);
        ok = DeleteFileW(path) != 0;
        LeaveCriticalSection(&g_lock);
    }
    Log("FileDelete %s -> %d", name ? name : "(null)", ok);
    return ok;
}

// Slots 13 and 14 (FileExists, FilePersisted).
static bool StorageFileExists(void *self, const char *name)
{
    wchar_t path[MAX_PATH];
    bool exists = PathFor(name, path) && SizeOf(path) >= 0;
    Log("FileExists %s -> %d", name ? name : "(null)", exists);
    return exists;
}

// Slot 15. 0 when the file is missing, like Steam.
static int32_t StorageGetFileSize(void *self, const char *name)
{
    wchar_t path[MAX_PATH];
    int32_t size = PathFor(name, path) ? SizeOf(path) : -1;
    Log("GetFileSize %s -> %d", name ? name : "(null)", size);
    return size < 0 ? 0 : size;
}

// Slots 21 and 22 (IsCloudEnabledForAccount, IsCloudEnabledForApp).
static bool StorageIsCloudEnabled(void *self)
{
    Log("IsCloudEnabled -> %d", g_enabled);
    return g_enabled;
}

// Every other slot: a zero result is failure for each of them (false, 0 bytes or files, k_uAPICallInvalid, an
// invalid handle). Out parameters are left alone; the game calls none of these.
#define UNUSED_SLOT(n) static uint64_t Unused##n(void *self) { Log("unused slot %d", n); return 0; }
UNUSED_SLOT(2) UNUSED_SLOT(3) UNUSED_SLOT(4) UNUSED_SLOT(7) UNUSED_SLOT(8) UNUSED_SLOT(9) UNUSED_SLOT(10) UNUSED_SLOT(11)
UNUSED_SLOT(12) UNUSED_SLOT(16) UNUSED_SLOT(17) UNUSED_SLOT(18) UNUSED_SLOT(19) UNUSED_SLOT(20) UNUSED_SLOT(23) UNUSED_SLOT(24)
UNUSED_SLOT(25) UNUSED_SLOT(26) UNUSED_SLOT(27) UNUSED_SLOT(28) UNUSED_SLOT(29) UNUSED_SLOT(30) UNUSED_SLOT(31) UNUSED_SLOT(32)
UNUSED_SLOT(33) UNUSED_SLOT(34) UNUSED_SLOT(35) UNUSED_SLOT(36) UNUSED_SLOT(37) UNUSED_SLOT(38) UNUSED_SLOT(39) UNUSED_SLOT(40)
UNUSED_SLOT(41) UNUSED_SLOT(42) UNUSED_SLOT(43) UNUSED_SLOT(44) UNUSED_SLOT(45) UNUSED_SLOT(46) UNUSED_SLOT(47) UNUSED_SLOT(48)
UNUSED_SLOT(49) UNUSED_SLOT(50) UNUSED_SLOT(51) UNUSED_SLOT(52) UNUSED_SLOT(53) UNUSED_SLOT(54) UNUSED_SLOT(55) UNUSED_SLOT(56)
UNUSED_SLOT(57) UNUSED_SLOT(58)

// ISteamRemoteStorage v016 in declaration order (isteamremotestorage.h); a C table, so no compiler vtable ABI is involved.
static const void *const g_vtable[SLOT_COUNT] = {
    (void *)StorageFileWrite, (void *)StorageFileRead, (void *)Unused2, (void *)Unused3, (void *)Unused4, (void *)StorageFileForget,
    (void *)StorageFileDelete, (void *)Unused7, (void *)Unused8, (void *)Unused9, (void *)Unused10, (void *)Unused11,
    (void *)Unused12, (void *)StorageFileExists, (void *)StorageFileExists, (void *)StorageGetFileSize, (void *)Unused16, (void *)Unused17,
    (void *)Unused18, (void *)Unused19, (void *)Unused20, (void *)StorageIsCloudEnabled, (void *)StorageIsCloudEnabled, (void *)Unused23,
    (void *)Unused24, (void *)Unused25, (void *)Unused26, (void *)Unused27, (void *)Unused28, (void *)Unused29,
    (void *)Unused30, (void *)Unused31, (void *)Unused32, (void *)Unused33, (void *)Unused34, (void *)Unused35,
    (void *)Unused36, (void *)Unused37, (void *)Unused38, (void *)Unused39, (void *)Unused40, (void *)Unused41,
    (void *)Unused42, (void *)Unused43, (void *)Unused44, (void *)Unused45, (void *)Unused46, (void *)Unused47,
    (void *)Unused48, (void *)Unused49, (void *)Unused50, (void *)Unused51, (void *)Unused52, (void *)Unused53,
    (void *)Unused54, (void *)Unused55, (void *)Unused56, (void *)Unused57, (void *)Unused58,
};
static struct { const void *const *vtable; } g_storage = { g_vtable };

void *SteamCloudStorage(void)
{
    EnsureInitialized();
    return &g_storage;
}

typedef void *(*FindOrCreateUserInterfaceFn)(int32_t user, const char *version);

// Bridge mode (docs/steam-bridge.md): HELLFORGE_BRIDGE_PIPE names the session's broker. Valve's DLL is then never loaded by
// this stand-in, even when the broker cannot be reached: the flat functions answer as they do without Steam.
#define BRIDGE_CONNECT_MS 10000
static wchar_t g_bridgeSuffix[65];
static bool g_bridgeRequested;

// The real DLL, which sits next to this one; loaded by full path, so no search order is involved. Tried once; never in
// bridge mode.
static HMODULE Real(void)
{
#ifdef HELLFORGE_BRIDGE_BUILD
    return NULL;
#endif
    static HMODULE real;
    static bool tried;
    if (!tried && !g_bridgeRequested)
    {
        wchar_t path[MAX_PATH];
        DWORD length = GetModuleFileNameW(g_self, path, MAX_PATH);
        wchar_t *slash = length > 0 && length < MAX_PATH ? wcsrchr(path, L'\\') : NULL;
        if (slash && (size_t)(slash + 1 - path) + wcslen(REAL_MODULE) < MAX_PATH)
        {
            wcscpy(slash + 1, REAL_MODULE);
            real = LoadLibraryW(path);
        }
        tried = true;
    }
    return real;
}

HMODULE SteamRealModule(void) { return Real(); }

// One of the real DLL's exports, looked up on the first call only (RunCallbacks runs every frame).
static FARPROC RealProc(FARPROC *cache, bool *looked, const char *name)
{
    if (!*looked)
    {
        HMODULE real = Real();
        *cache = real ? GetProcAddress(real, name) : NULL;
        *looked = true;
    }
    return *cache;
}
#define REAL(type, name) ({ static FARPROC cache_; static bool looked_; (type)(void *)RealProc(&cache_, &looked_, name); })

// The accessor behind the game's SteamRemoteStorage(); every other interface comes from the real DLL (in the census build,
// behind a counting proxy). In bridge mode only the storage interface is answered until phase 3's proxies.
HELLFORGE_API void *SteamInternal_FindOrCreateUserInterface(int32_t user, const char *version)
{
    if (version) SteamCensusInterface(version, user);
#ifdef HELLFORGE_BRIDGE_BUILD
    return BridgeInterface(version);
#endif
    if (version && strcmp(version, STORAGE_VERSION) == 0)
    {
        EnsureInitialized();
        Log("FindOrCreateUserInterface %s -> HellForge storage", version);
        return &g_storage;
    }
    FindOrCreateUserInterfaceFn forward = REAL(FindOrCreateUserInterfaceFn, "SteamInternal_FindOrCreateUserInterface");
    return forward ? SteamCensusWrap(forward(user, version), version) : NULL;
}

// Callback registrations, passed on to the real DLL; the census notes each callback id and its struct's size once (a
// CCallbackBase: its vtable, a flags byte, then the id at +12; Steamworks' steam_api_common.h).
typedef void (*RegisterCallbackFn)(void *callback, int id);
typedef void (*RegisterCallResultFn)(void *callback, uint64_t call);

HELLFORGE_API void SteamAPI_RegisterCallback(void *callback, int id)
{
    SteamCensusCallback("callback", id, callback);
#ifdef HELLFORGE_BRIDGE_BUILD
    BridgeRegisterCallback(callback, id);
    return;
#endif
    if (g_bridgeRequested) return;
    RegisterCallbackFn forward = REAL(RegisterCallbackFn, "SteamAPI_RegisterCallback");
    if (forward) forward(callback, id);
}

HELLFORGE_API void SteamAPI_RegisterCallResult(void *callback, uint64_t call)
{
    int id = 0;
    if (callback) memcpy(&id, (const uint8_t *)callback + 12, 4);
    SteamCensusCallback("callresult", id, callback);
#ifdef HELLFORGE_BRIDGE_BUILD
    BridgeRegisterCallResult(callback, call);
    return;
#endif
    if (g_bridgeRequested) return;
    RegisterCallResultFn forward = REAL(RegisterCallResultFn, "SteamAPI_RegisterCallResult");
    if (forward) forward(callback, call);
}

// The flat-API accessor for the same interface.
HELLFORGE_API void *SteamAPI_SteamRemoteStorage_v016(void)
{
    EnsureInitialized();
    return &g_storage;
}

// --- Flat API: answered by the broker in bridge mode, forwarded to the real DLL otherwise. ---
// The .def generator (SdkShimGen forward) sees HELLFORGE_API and emits these as code exports, not forwarders.

typedef int (__cdecl *IntFn)(void);
typedef void (__cdecl *VoidFn)(void);

HELLFORGE_API int __cdecl SteamAPI_RestartAppIfNecessary(uint32_t appId)
{
    EnsureInitialized();
    Log("RestartAppIfNecessary %u -> 0 (stand-in)", appId);
    return 0;
}

// The pinned build's SDK exports SteamAPI_Init and SteamAPI_InitSafe (both bool); newer SDKs replace them with
// SteamAPI_InitFlat and SteamInternal_SteamAPI_Init, which the .def generator would refuse here.
// Bridge mode: connects here rather than in DllMain, since the wait for the broker must not run under the loader lock.
static bool BridgeInit(const char *how)
{
#ifdef HELLFORGE_BRIDGE_BUILD
    if (!g_bridged)
    {
        g_bridged = g_bridgeSuffix[0] && GameChannelConnect(&g_bridge, g_bridgeSuffix, BRIDGE_CONNECT_MS);
        Log(g_bridged ? "%s -> 1 (broker)" : "%s -> 0: the broker did not answer", how);
        if (g_bridged) { BridgeLocalAttach(&g_bridge); BridgeContextsChanged(); }
    }
    return g_bridged && !g_bridge.dead;
#else
    Log("%s -> 0: the session asks for the bridge, and this is not the bridge build", how);
    return false;
#endif
}

HELLFORGE_API bool __cdecl SteamAPI_Init(void)
{
    EnsureInitialized();
    if (g_bridgeRequested) return BridgeInit("SteamAPI_Init");
    typedef bool (__cdecl *Fn)(void);
    Fn forward = REAL(Fn, "SteamAPI_Init");
    bool ok = forward ? forward() : false;
    Log("SteamAPI_Init -> %d (real)", ok);
    return ok;
}

HELLFORGE_API bool __cdecl SteamAPI_InitSafe(void)
{
    EnsureInitialized();
    if (g_bridgeRequested) return BridgeInit("SteamAPI_InitSafe");
    typedef bool (__cdecl *Fn)(void);
    Fn forward = REAL(Fn, "SteamAPI_InitSafe");
    bool ok = forward ? forward() : false;
    Log("SteamAPI_InitSafe -> %d (real)", ok);
    return ok;
}

// What the bridge costs the game (docs/steam-bridge.md, Cost), logged each minute and at shutdown: the frames
// (RunCallbacks), the round trips with the functions that took longest, and the callbacks read from the ring.
static struct { uint64_t frames, callbackFrames, callbacks, ticks, maxTicks; ULONGLONG since; } g_bridgeCost;

static double Milliseconds(uint64_t ticks)
{
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart ? ticks * 1000.0 / (double)frequency.QuadPart : 0.0;
}

static void BridgeCostReport(const char *span)
{
    GameChannelCost calls;
    GameChannelTakeCost(&g_bridge, &calls);
    Log("bridge cost, %s: %llu frames; %llu round trips, %.1f ms in all, longest %.2f ms; %llu callbacks in %llu frames, "
        "%.1f ms in all, longest frame %.2f ms", span, (unsigned long long)g_bridgeCost.frames, (unsigned long long)calls.calls,
        Milliseconds(calls.ticks), Milliseconds(calls.maxTicks), (unsigned long long)g_bridgeCost.callbacks,
        (unsigned long long)g_bridgeCost.callbackFrames, Milliseconds(g_bridgeCost.ticks), Milliseconds(g_bridgeCost.maxTicks));
    // the five functions whose round trips took longest (ids: bridge_protocol.h, and BRIDGE_M_* in bridge_gen.h)
    char line[300];
    int at = 0;
    for (int n = 0; n < 5; n++)
    {
        int best = -1;
        for (int i = 0; i < BRIDGE_COST_FUNCTIONS; i++)
            if (calls.byFunction[i].calls > 0 && (best < 0 || calls.byFunction[i].ticks > calls.byFunction[best].ticks)) best = i;
        if (best < 0) break;
        at += snprintf(line + at, sizeof line - at, " %u x%u %.1f ms", calls.byFunction[best].function, calls.byFunction[best].calls,
                       Milliseconds(calls.byFunction[best].ticks));
        calls.byFunction[best].calls = 0;
    }
    if (at > 0) Log("bridge cost, %s, by function (id, round trips, time):%s", span, line);
    memset(&g_bridgeCost, 0, sizeof g_bridgeCost);
    g_bridgeCost.since = GetTickCount64();
}

// A bridged game's frame (SteamAPI_RunCallbacks): the minute's cost report when it is due, then the callbacks the
// broker queued, timed.
static void BridgeRunCallbacks(void)
{
    // the idle frame: one read of the ring's sequence and of the tick count, no system call
    g_bridgeCost.frames++;
    if (g_bridged && GetTickCount64() - g_bridgeCost.since >= 60000)
    {
        if (g_bridgeCost.since != 0) BridgeCostReport("the last minute");
        else g_bridgeCost.since = GetTickCount64();
    }
    if (!g_bridged || !GameChannelPending(&g_bridge)) return;
    uint32_t id, size;
    uint8_t data[BRIDGE_RING_DATA_SIZE];
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    while (GameChannelReadCallback(&g_bridge, &id, data, sizeof data, &size))
    {
        g_bridgeCost.callbacks++;
#ifdef HELLFORGE_BRIDGE_BUILD
        BridgeDispatch(id, data, size);
#endif
    }
    QueryPerformanceCounter(&end);
    uint64_t ticks = (uint64_t)(end.QuadPart - start.QuadPart);
    g_bridgeCost.callbackFrames++;
    g_bridgeCost.ticks += ticks;
    if (ticks > g_bridgeCost.maxTicks) g_bridgeCost.maxTicks = ticks;
}

HELLFORGE_API void __cdecl SteamAPI_RunCallbacks(void)
{
    if (g_bridgeRequested) { BridgeRunCallbacks(); return; }
    VoidFn forward = REAL(VoidFn, "SteamAPI_RunCallbacks");
    if (forward) forward();
}

HELLFORGE_API int __cdecl SteamAPI_IsSteamRunning(void)
{
    if (g_bridgeRequested)
    {
        uint8_t running = 0;
        GameChannelReply reply = { &running, 1, 0, BRIDGE_STATUS_UNKNOWN };
        if (!g_bridged || !GameChannelCall(&g_bridge, BRIDGE_FN_IS_STEAM_RUNNING, NULL, 0, &reply) ||
            reply.status != BRIDGE_STATUS_OK || reply.size != 1)
            return 0;
        return running;
    }
    IntFn forward = REAL(IntFn, "SteamAPI_IsSteamRunning");
    return forward ? forward() : 0;
}

HELLFORGE_API void __cdecl SteamAPI_Shutdown(void)
{
    EnsureInitialized();
    if (g_bridgeRequested)
    {
        if (g_bridged) BridgeCostReport("since the last report");
        if (g_bridged) GameChannelClose(&g_bridge); // says BRIDGE_SHUTDOWN first
        g_bridged = false;
#ifdef HELLFORGE_BRIDGE_BUILD
        BridgeLocalAttach(NULL);
        BridgeContextsChanged();
#endif
        Log("SteamAPI_Shutdown (broker)");
        return;
    }
    VoidFn forward = REAL(VoidFn, "SteamAPI_Shutdown");
    if (forward) forward();
    Log("SteamAPI_Shutdown (real)");
}

HELLFORGE_API int __cdecl SteamAPI_GetHSteamPipe(void)
{
    if (g_bridgeRequested) return g_bridged && !g_bridge.dead ? 1 : 0;
    IntFn forward = REAL(IntFn, "SteamAPI_GetHSteamPipe");
    return forward ? forward() : 0;
}

HELLFORGE_API int __cdecl SteamAPI_GetHSteamUser(void)
{
    if (g_bridgeRequested) return g_bridged && !g_bridge.dead ? 1 : 0;
    IntFn forward = REAL(IntFn, "SteamAPI_GetHSteamUser");
    return forward ? forward() : 0;
}

HELLFORGE_API void __cdecl SteamAPI_UnregisterCallback(void *callback)
{
#ifdef HELLFORGE_BRIDGE_BUILD
    BridgeUnregisterCallback(callback);
    return;
#endif
    if (g_bridgeRequested) return;
    typedef void (__cdecl *Fn)(void *);
    Fn forward = REAL(Fn, "SteamAPI_UnregisterCallback");
    if (forward) forward(callback);
}

HELLFORGE_API void __cdecl SteamAPI_ManualDispatch_Init(void)
{
    if (g_bridgeRequested) return;
    VoidFn forward = REAL(VoidFn, "SteamAPI_ManualDispatch_Init");
    if (forward) forward();
}

typedef int (*LoaderStart)(const wchar_t *plan);

// Sessions with code mods name the HellForge mod loader and its plan (docs/internals.md, "The load chain"). This DLL is the first HellForge code in the
// game's process, so it starts the loader, on its own thread once the loader lock is released.
static DWORD WINAPI StartModLoader(void *parameter)
{
    wchar_t loader[MAX_PATH], plan[MAX_PATH];
    DWORD loaderLength = GetEnvironmentVariableW(L"HELLFORGE_MODLOADER", loader, MAX_PATH);
    DWORD planLength = GetEnvironmentVariableW(L"HELLFORGE_MODLOADER_PLAN", plan, MAX_PATH);
    if (loaderLength == 0 || loaderLength >= MAX_PATH || planLength == 0 || planLength >= MAX_PATH) return 0;
    HMODULE module = LoadLibraryExW(loader, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    LoaderStart start = module ? (LoaderStart)(void *)GetProcAddress(module, "hellforge_loader_start") : NULL;
    if (start) start(plan);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        InitializeCriticalSection(&g_lock);
        // the network gate first: hooked before the game's own code can reach the network (net_gate.c)
        NetGateStart();
        // the Steam census, when asked for (steam_census.c): what the game asks of Steam
        SteamCensusStart(instance);
        // bridge mode (docs/steam-bridge.md): the broker instead of Valve's DLL; the Init functions connect. Any value set
        // asks for it, so a name too long to use fails closed rather than falling back to Valve's DLL.
        DWORD suffixLength = GetEnvironmentVariableW(L"HELLFORGE_BRIDGE_PIPE", g_bridgeSuffix, 65);
        g_bridgeRequested = suffixLength > 0;
#ifdef HELLFORGE_BRIDGE_BUILD
        g_bridgeRequested = true; // without a broker it fails closed: there is nothing else to run on
#endif
        if (suffixLength == 0 || suffixLength > 64) g_bridgeSuffix[0] = 0;
        if (GetEnvironmentVariableW(L"HELLFORGE_MODLOADER", NULL, 0) > 0)
        {
            HANDLE thread = CreateThread(NULL, 0, StartModLoader, NULL, 0, NULL);
            if (thread) CloseHandle(thread);
        }
    }
    return TRUE;
}
