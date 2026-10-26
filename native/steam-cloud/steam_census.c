// HellForge's Steam census in the copied game: what the game asks of Steam, so a Steam bridge (the game at low integrity,
// its Steam calls carried by a broker at the user's normal integrity) knows the surface it must carry
// (docs/steam-bridge.md). Off unless HELLFORGE_STEAM_CENSUS_LOG names a file; then it writes JSON lines:
//   interface     each interface version the game requests (SteamInternal_FindOrCreateUserInterface), with its user;
//   function      each of this stand-in's exports the game resolves by name (a hook on kernelbase's
//                 GetProcAddressForCaller, behind both kernel32's and kernelbase's GetProcAddress);
//   callback      each callback id the game registers, callbacksize its struct's size (the game's own
//                 GetCallbackSizeBytes), callresult each call result's id.
// The census build (Build-Engine.ps1 -SteamCensus: every function export a counting thunk, census-thunks in
// HellForge.SdkShimGen) adds:
//   export        each Valve export the game calls, the first time;
//   slot          each vtable slot the game calls on an interface (its object is a counting proxy), the first time;
//   calls         every 15 s (HELLFORGE_STEAM_CENSUS_PERIOD_MS, 50 to 600000, changes it), each export's and slot's
//                 total so far (SteamAPI_RunCallbacks' is the frame count).
// Nothing is changed or refused.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"
#include "steam_census.h"

#define CENSUS_SEEN 4096
// A key is kind:name:number. Names are at most 127 characters (the generator refuses longer export names; Clean caps the
// rest), so a key never reaches the limit and no two keys are cut to the same text.
#define CENSUS_KEY 192

typedef FARPROC (WINAPI *GetProcAddressFn)(HMODULE, LPCSTR);
typedef FARPROC (WINAPI *GetProcAddressForCallerFn)(HMODULE, LPCSTR, void *);
static GetProcAddressFn g_getProcAddress;
static GetProcAddressForCallerFn g_getProcAddressForCaller;
static HMODULE g_census;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_censusLock;
static char g_seen[CENSUS_SEEN][CENSUS_KEY];
static int g_seenCount;

// True the first time a key is seen (and it is remembered); false after, or once the list is full.
static bool FirstTime(const char *key)
{
    for (int i = 0; i < g_seenCount; i++) if (!strcmp(g_seen[i], key)) return false;
    if (g_seenCount == CENSUS_SEEN) return false;
    snprintf(g_seen[g_seenCount++], sizeof g_seen[0], "%s", key);
    return true;
}

static void Line(const char *kind, const char *name, long long number)
{
    char line[256];
    SYSTEMTIME now;
    GetSystemTime(&now);
    int length = snprintf(line, sizeof line, "{\"utc\":\"%02u:%02u:%02u.%03u\",\"kind\":\"%s\",\"name\":\"%s\",\"number\":%lld}\n",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, kind, name, number);
    DWORD written;
    if (length > 0 && length < (int)sizeof line) WriteFile(g_log, line, (DWORD)length, &written, NULL);
}

static void Write(const char *kind, const char *name, long long number)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    char key[CENSUS_KEY];
    int length = snprintf(key, sizeof key, "%s:%s:%lld", kind, name, number);
    if (length < 0 || length >= (int)sizeof key) return;
    EnterCriticalSection(&g_censusLock);
    if (FirstTime(key)) Line(kind, name, number);
    LeaveCriticalSection(&g_censusLock);
}

// A name only, printable and without quotes, so the JSON line stays well formed.
static void Clean(const char *text, char *out, size_t size)
{
    size_t n = 0;
    for (const char *c = text; c && *c && n < size - 1; c++) out[n++] = (*c >= 0x20 && *c < 0x7F && *c != '"' && *c != '\\') ? *c : '?';
    out[n] = 0;
}

static void LookedUp(HMODULE module, LPCSTR name)
{
    if (module != g_census || name == NULL) return;
    char clean[80];
    if ((ULONG_PTR)name >> 16) Clean(name, clean, sizeof clean);
    else snprintf(clean, sizeof clean, "#%u", (unsigned)(ULONG_PTR)name);
    Write("function", clean, 0);
}

static FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name)
{
    LookedUp(module, name);
    return g_getProcAddress(module, name);
}

static FARPROC WINAPI HookGetProcAddressForCaller(HMODULE module, LPCSTR name, void *caller)
{
    LookedUp(module, name);
    return g_getProcAddressForCaller(module, name, caller);
}

int SteamCensusActive(void) { return g_log != INVALID_HANDLE_VALUE; }

#ifdef HELLFORGE_STEAM_CENSUS_THUNKS
// ---- the census build: counting thunks for every export and every interface slot (census-thunks) ----
#define CENSUS_EXPORTS 2048
#define CENSUS_INTERFACES 16
#define CENSUS_SLOTS 192
extern const int g_censusExportCount;
extern const char *const g_censusExportNames[];
extern void *const g_censusSlots[CENSUS_SLOTS];
static void *volatile g_exportTargets[CENSUS_EXPORTS];
static volatile LONG g_exportCalls[CENSUS_EXPORTS], g_exportReported[CENSUS_EXPORTS];

typedef struct CensusProxy { void *const *vtable; void *real; int index; } CensusProxy;
static char g_interfaceNames[CENSUS_INTERFACES][64];
static CensusProxy *g_proxies[CENSUS_INTERFACES];
static int g_interfaceCount;
static volatile LONG g_slotCalls[CENSUS_INTERFACES][CENSUS_SLOTS], g_slotReported[CENSUS_INTERFACES][CENSUS_SLOTS];

// An export Valve's DLL does not have (or that DLL is missing): the call returns 0.
static uintptr_t CensusMissing(void) { return 0; }

// Called by an export's thunk with the argument registers saved: where to jump.
void *CensusExport(int index)
{
    if (index < 0 || index >= g_censusExportCount || index >= CENSUS_EXPORTS)
    {
        // the generator refuses more than CENSUS_EXPORTS, so this is a build mismatch: said once
        static volatile LONG said;
        if (InterlockedExchange(&said, 1) == 0) Write("overflow", "export index past the census table", index);
        return (void *)CensusMissing;
    }
    void *target = g_exportTargets[index];
    if (target == NULL)
    {
        HMODULE real = SteamRealModule();
        target = real ? (void *)GetProcAddress(real, g_censusExportNames[index]) : NULL;
        if (target == NULL)
        {
            target = (void *)CensusMissing;
            Write("missing", g_censusExportNames[index], 0);
        }
        g_exportTargets[index] = target;
    }
    if (InterlockedIncrement(&g_exportCalls[index]) == 1) Write("export", g_censusExportNames[index], 0);
    return target;
}

// Called by slot k's thunk with the argument registers saved: counts the call and returns the real object's slot k,
// where the thunk jumps with the real object in RCX.
void *CensusSlot(CensusProxy *proxy, int slot)
{
    if (proxy->index >= 0 && proxy->index < CENSUS_INTERFACES && slot >= 0 && slot < CENSUS_SLOTS &&
        InterlockedIncrement(&g_slotCalls[proxy->index][slot]) == 1)
        Write("slot", g_interfaceNames[proxy->index], slot);
    return (*(void *const *const *)proxy->real)[slot];
}

void *SteamCensusWrap(void *real, const char *version)
{
    if (real == NULL || version == NULL || !SteamCensusActive()) return real;
    char clean[64];
    Clean(version, clean, sizeof clean);
    EnterCriticalSection(&g_censusLock);
    CensusProxy *proxy = NULL;
    for (int i = 0; i < g_interfaceCount && proxy == NULL; i++)
        if (!strcmp(g_interfaceNames[i], clean) && g_proxies[i]->real == real) proxy = g_proxies[i];
    if (proxy == NULL && g_interfaceCount < CENSUS_INTERFACES)
    {
        proxy = (CensusProxy *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *proxy);
        if (proxy != NULL)
        {
            proxy->vtable = g_censusSlots;
            proxy->real = real;
            proxy->index = g_interfaceCount;
            snprintf(g_interfaceNames[g_interfaceCount], sizeof g_interfaceNames[0], "%s", clean);
            g_proxies[g_interfaceCount++] = proxy;
        }
    }
    LeaveCriticalSection(&g_censusLock);
    return proxy != NULL ? (void *)proxy : real;
}

static DWORD g_period = 15000;

// Each export's count that changed since the last report (g_censusLock held).
static void ReportExports(void)
{
    for (int i = 0; i < g_censusExportCount && i < CENSUS_EXPORTS; i++)
    {
        LONG calls = g_exportCalls[i];
        if (calls != g_exportReported[i]) { g_exportReported[i] = calls; Line("calls", g_censusExportNames[i], calls); }
    }
}

// Each interface slot's count that changed since the last report (g_censusLock held).
static void ReportSlots(void)
{
    for (int i = 0; i < g_interfaceCount; i++)
        for (int k = 0; k < CENSUS_SLOTS; k++)
        {
            LONG calls = g_slotCalls[i][k];
            if (calls == g_slotReported[i][k]) continue;
            g_slotReported[i][k] = calls;
            char name[96];
            snprintf(name, sizeof name, "%s#%d", g_interfaceNames[i], k);
            Line("calls", name, calls);
        }
}

// Every period: each count that changed since the last report.
static DWORD WINAPI Rates(void *parameter)
{
    for (;;)
    {
        Sleep(g_period);
        EnterCriticalSection(&g_censusLock);
        ReportExports();
        ReportSlots();
        LeaveCriticalSection(&g_censusLock);
    }
    return 0;
}

// HELLFORGE_STEAM_CENSUS_PERIOD_MS (50 to 600000) sets the period; the reporting thread starts.
static void StartRates(void)
{
    wchar_t period[16];
    DWORD periodLength = GetEnvironmentVariableW(L"HELLFORGE_STEAM_CENSUS_PERIOD_MS", period, 16);
    long milliseconds = periodLength > 0 && periodLength < 16 ? wcstol(period, NULL, 10) : 0;
    if (milliseconds >= 50 && milliseconds <= 600000) g_period = (DWORD)milliseconds;
    HANDLE rates = CreateThread(NULL, 0, Rates, NULL, 0, NULL);
    if (rates) CloseHandle(rates);
}
#else
void *SteamCensusWrap(void *real, const char *version) { (void)version; return real; }
#endif

// kernel32's GetProcAddress jumps to kernelbase's GetProcAddressForCaller, not to kernelbase's GetProcAddress, so the
// hook goes there (and on GetProcAddress only where Windows has no GetProcAddressForCaller). ready: MH_Initialize's.
static bool HookLookups(MH_STATUS ready)
{
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    void *forCaller = kernelbase ? (void *)GetProcAddress(kernelbase, "GetProcAddressForCaller") : NULL;
    void *target = forCaller ? forCaller : kernelbase ? (void *)GetProcAddress(kernelbase, "GetProcAddress") : NULL;
    void *hook = forCaller ? (void *)HookGetProcAddressForCaller : (void *)HookGetProcAddress;
    void **original = forCaller ? (void **)&g_getProcAddressForCaller : (void **)&g_getProcAddress;
    return (ready == MH_OK || ready == MH_ERROR_ALREADY_INITIALIZED) && target &&
        MH_CreateHook(target, hook, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

void SteamCensusStart(HMODULE self)
{
    wchar_t path[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_STEAM_CENSUS_LOG", path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    InitializeCriticalSection(&g_censusLock);
    g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE) return;
    g_census = self;
    MH_STATUS ready = MH_Initialize(); /* the network gate may have done it already */
    bool hooked = HookLookups(ready);
#ifdef HELLFORGE_STEAM_CENSUS_THUNKS
    Write("census", hooked ? "started (census build)" : "started (census build) without function names", (long long)GetCurrentProcessId());
    StartRates();
#else
    Write("census", hooked ? "started" : "started-without-function-names", (long long)GetCurrentProcessId());
#endif
}

void SteamCensusInterface(const char *version, int user)
{
    char clean[80];
    Clean(version, clean, sizeof clean);
    Write("interface", clean, user);
}

// A CCallbackBase's vtable is Run, Run (the call-result overload), GetCallbackSizeBytes (steam_api_common.h); MSVC
// may swap the two Runs, never the third slot.
typedef int (*CallbackSizeFn)(void *self);

void SteamCensusCallback(const char *how, int id, void *callback)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    Write(how, "callback", id);
    if (callback == NULL) return;
    CallbackSizeFn size = (CallbackSizeFn)(*(void *const *const *)callback)[2];
    char name[32];
    snprintf(name, sizeof name, "%d", id);
    Write("callbacksize", name, size(callback));
}
