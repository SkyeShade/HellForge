// The tablet gate: a sandboxed game is refused Wintab32.dll. The engine loads it whenever a tablet driver is installed,
// though the game has no use for a pen. At low integrity the driver is refused its own shared memory, and Gaomon's
// driver then shows a modal "OpenMem failed. Err=5" box from WTEnable and WTOverlap, which the engine calls as its window
// gains or loses focus (alt-tab). Refused, the game runs as on a PC without a tablet driver; the pen still moves the mouse.

// The NVIDIA telemetry: at low integrity it's refused the pipe to NVIDIA's service and retries without pause
// at quit. DLSS' shutdown waits for it with no time limit, so the game never exits and its shader cache is never saved.
// NvTelemetryAPI64.dll refused, nvapi carries on as on a PC with no telemetry, DLSS still functional.
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include "MinHook.h"
#include "net_gate.h"
#include "tablet_gate.h"

#define RETURN_ADDRESS __builtin_extract_return_addr(__builtin_return_address(0))

typedef HMODULE (WINAPI *LoadLibraryAFn)(LPCSTR);
typedef HMODULE (WINAPI *LoadLibraryWFn)(LPCWSTR);
typedef HMODULE (WINAPI *LoadLibraryExAFn)(LPCSTR, HANDLE, DWORD);
typedef HMODULE (WINAPI *LoadLibraryExWFn)(LPCWSTR, HANDLE, DWORD);
static LoadLibraryAFn g_loadLibraryA;
static LoadLibraryWFn g_loadLibraryW;
static LoadLibraryExAFn g_loadLibraryExA;
static LoadLibraryExWFn g_loadLibraryExW;

typedef struct { const wchar_t *wide; const char *narrow; const char *dll; const char *loadedLine; } RefusedDll;
static const RefusedDll g_refused[] = {
    {L"wintab32", "wintab32", "Wintab32.dll", "{\"gate\":\"tablet loaded\",\"module\":\"Wintab32.dll\"}\n"},
    {L"nvtelemetryapi64", "nvtelemetryapi64", "NvTelemetryAPI64.dll", "{\"gate\":\"telemetry loaded\",\"module\":\"NvTelemetryAPI64.dll\"}\n"},
};
#define REFUSED_COUNT (sizeof g_refused / sizeof g_refused[0])

//Whether a file name is name, or name followed by ".dll", in any case.
static bool SameNameW(const wchar_t *file, const wchar_t *name)
{
    size_t length = wcslen(name);
    return !_wcsnicmp(file, name, length) && (file[length] == 0 || !_wcsicmp(file + length, L".dll"));
}

static bool SameNameA(const char *file, const char *name)
{
    size_t length = strlen(name);
    return !_strnicmp(file, name, length) && (file[length] == 0 || !_stricmp(file + length, ".dll"));
}

// The refused DLL a LoadLibrary name is (any folder, any case, with or without ".dll"), or NULL.
static const RefusedDll *RefusedW(const wchar_t *name)
{
    if (!name) return NULL;
    const wchar_t *file = name;
    for (const wchar_t *c = name; *c; c++) if (*c == L'\\' || *c == L'/') file = c + 1;
    for (size_t i = 0; i < REFUSED_COUNT; i++)
        if (SameNameW(file, g_refused[i].wide)) return &g_refused[i];
    return NULL;
}

static const RefusedDll *RefusedA(const char *name)
{
    if (!name) return NULL;
    const char *file = name;
    for (const char *c = name; *c; c++) if (*c == '\\' || *c == '/') file = c + 1;
    for (size_t i = 0; i < REFUSED_COUNT; i++)
        if (SameNameA(file, g_refused[i].narrow)) return &g_refused[i];
    return NULL;
}

static HMODULE Refuse(const char *api, const RefusedDll *refused, void *returnAddress)
{
    NetGateNote(api, refused->dll, "refused", returnAddress);
    SetLastError(ERROR_MOD_NOT_FOUND);
    return NULL;
}

// The answer a PC without the DLL gets (no tablet driver, no NVIDIA telemetry), so its caller takes its own path for that.
static HMODULE WINAPI HookLoadLibraryA(LPCSTR name)
{
    const RefusedDll *refused = RefusedA(name);
    return refused ? Refuse("LoadLibraryA", refused, RETURN_ADDRESS) : g_loadLibraryA(name);
}

static HMODULE WINAPI HookLoadLibraryW(LPCWSTR name)
{
    const RefusedDll *refused = RefusedW(name);
    return refused ? Refuse("LoadLibraryW", refused, RETURN_ADDRESS) : g_loadLibraryW(name);
}

static HMODULE WINAPI HookLoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    const RefusedDll *refused = RefusedA(name);
    return refused ? Refuse("LoadLibraryExA", refused, RETURN_ADDRESS) : g_loadLibraryExA(name, file, flags);
}

static HMODULE WINAPI HookLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    const RefusedDll *refused = RefusedW(name);
    return refused ? Refuse("LoadLibraryExW", refused, RETURN_ADDRESS) : g_loadLibraryExW(name, file, flags);
}

// kernel32's LoadLibrary calls end in kernelbase's, so these four see every load made through either. The game's own
// call is in its encrypted code; a load that reaches ntdll another way is written by OnDllLoaded below.
typedef struct { const char *name; void *hook; void **original; } Site;
static Site g_sites[] = {
    {"LoadLibraryA", (void *)HookLoadLibraryA, (void **)&g_loadLibraryA},
    {"LoadLibraryW", (void *)HookLoadLibraryW, (void **)&g_loadLibraryW},
    {"LoadLibraryExA", (void *)HookLoadLibraryExA, (void **)&g_loadLibraryExA},
    {"LoadLibraryExW", (void *)HookLoadLibraryExW, (void **)&g_loadLibraryExW},
};
#define SITE_COUNT (sizeof g_sites / sizeof g_sites[0])

// Wintab32 loaded although refused: the session's census then says the gate missed the game's way of loading it.
static void OnDllLoaded(const wchar_t *name)
{
    const RefusedDll *refused = RefusedW(name);
    if (refused) NetGateWrite(refused->loadedLine);
}

void TabletGateStart(void)
{
    if (GetEnvironmentVariableW(L"HELLFORGE_BRIDGE_PIPE", NULL, 0) == 0) return;
    // after NetGateStart, which initialises MinHook itself when the census is on
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return;
    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    int hooked = 0;
    for (size_t i = 0; i < SITE_COUNT && kernelBase; i++)
    {
        void *target = (void *)GetProcAddress(kernelBase, g_sites[i].name);
        if (target && MH_CreateHook(target, g_sites[i].hook, g_sites[i].original) == MH_OK && MH_EnableHook(target) == MH_OK) hooked++;
    }
    bool watching = NetGateWatchLoads(OnDllLoaded);
    char line[128];
    snprintf(line, sizeof line, "{\"gate\":\"tablet\",\"hooked\":%d,\"of\":%d,\"watchingLoads\":%s}\n", hooked, (int)SITE_COUNT, watching ? "true" : "false");
    NetGateWrite(line);
}
