// The tablet gate: a sandboxed game is refused Wintab32.dll. The engine loads it whenever a tablet driver is installed,
// though the game has no use for a pen. At low integrity the driver is refused its own shared memory, and Gaomon's
// driver then shows a modal "OpenMem failed. Err=5" box from WTEnable and WTOverlap, which the engine calls as its window
// gains or loses focus (alt-tab). Refused, the game runs as on a PC without a tablet driver; the pen still moves the mouse.
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

// A name as LoadLibrary takes it: any folder, any case, with or without ".dll".
static bool IsWintabW(const wchar_t *name)
{
    if (!name) return false;
    const wchar_t *file = name;
    for (const wchar_t *c = name; *c; c++) if (*c == L'\\' || *c == L'/') file = c + 1;
    return !_wcsicmp(file, L"wintab32.dll") || !_wcsicmp(file, L"wintab32");
}

static bool IsWintabA(const char *name)
{
    if (!name) return false;
    const char *file = name;
    for (const char *c = name; *c; c++) if (*c == '\\' || *c == '/') file = c + 1;
    return !_stricmp(file, "wintab32.dll") || !_stricmp(file, "wintab32");
}

// The answer a PC without a tablet driver gets, so the engine takes its own no-tablet path.
static HMODULE Refuse(const char *api, void *returnAddress)
{
    NetGateNote(api, "Wintab32.dll", "refused", returnAddress);
    SetLastError(ERROR_MOD_NOT_FOUND);
    return NULL;
}

static HMODULE WINAPI HookLoadLibraryA(LPCSTR name)
{
    return IsWintabA(name) ? Refuse("LoadLibraryA", RETURN_ADDRESS) : g_loadLibraryA(name);
}

static HMODULE WINAPI HookLoadLibraryW(LPCWSTR name)
{
    return IsWintabW(name) ? Refuse("LoadLibraryW", RETURN_ADDRESS) : g_loadLibraryW(name);
}

static HMODULE WINAPI HookLoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    return IsWintabA(name) ? Refuse("LoadLibraryExA", RETURN_ADDRESS) : g_loadLibraryExA(name, file, flags);
}

static HMODULE WINAPI HookLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    return IsWintabW(name) ? Refuse("LoadLibraryExW", RETURN_ADDRESS) : g_loadLibraryExW(name, file, flags);
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
    if (IsWintabW(name)) NetGateWrite("{\"gate\":\"tablet loaded\",\"module\":\"Wintab32.dll\"}\n");
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
