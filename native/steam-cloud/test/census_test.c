// The Steam census build of the steam_api64.dll stand-in (steam_census.c with HELLFORGE_STEAM_CENSUS_THUNKS, census-thunks
// in HellForge.SdkShimGen): loaded next to the fake Steam DLL (fake_steam.c, as steam_api64_hellforge.dll), each kind of
// call goes through the census's thunks and must arrive untouched (registers, XMM registers, stack arguments, a hidden
// result pointer, the result), with the real object in place of the proxy; then the log must name it, and the periodic
// report must count it.
// Usage: census_test.exe <census steam_api64.dll, next to the fake> <log>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FakeSteamState
{
    void *self;
    uint64_t vibration[5];
    void *callback;
    int callbackId;
    int interfaceRequests;
} FakeSteamState;

typedef void *(*FindInterfaceFn)(int32_t user, const char *version);
typedef void (*RegisterCallbackFn)(void *callback, int id);
typedef int32_t (*GetUserFn)(void);
typedef bool (*AvgRateFn)(void *self, const char *name, float count, double session);
typedef void (*VibrationFn)(void *self, uint64_t handle, unsigned short left, unsigned short right, unsigned short leftTrigger,
    unsigned short rightTrigger);
typedef int32_t (*RunningFn)(void); /* SteamGameServer_GetHSteamPipe, which the fake lacks */
typedef uint32_t (*SlotAppIdFn)(void *self);
typedef uint64_t *(*SlotSteamIdFn)(void *self, uint64_t *result);
typedef uint64_t (*SlotSixFn)(void *self, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);
typedef float (*SlotFloatsFn)(void *self, float x, double y, float z);
typedef int (*SlotIntFn)(void *self);

static int g_failed;

static void Check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failed++;
}

static char *ReadAll(const wchar_t *path)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(file, NULL), read = 0;
    char *text = (char *)calloc(size + 1, 1);
    if (text != NULL) ReadFile(file, text, size, &read, NULL);
    CloseHandle(file);
    return text;
}

// A line holding every one of the given pieces.
static int Has(const char *log, const char *a, const char *b, const char *c)
{
    for (const char *line = log; line && *line;)
    {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        char buffer[512];
        if (n >= sizeof buffer) n = sizeof buffer - 1;
        memcpy(buffer, line, n);
        buffer[n] = 0;
        if (strstr(buffer, a) && (!b || strstr(buffer, b)) && (!c || strstr(buffer, c))) return 1;
        line = end ? end + 1 : NULL;
    }
    return 0;
}

// A CCallbackBase as Steamworks lays it out: the vtable (Run, Run, GetCallbackSizeBytes), a flags byte, the id at +12.
typedef struct FakeCallback { void *const *vtable; uint8_t flags; uint8_t pad[3]; int id; } FakeCallback;
static void CallbackRun(void *self, void *parameter) { (void)self; (void)parameter; }
static int CallbackSize(void *self) { (void)self; return 24; }
static void *const g_callbackVtable[3] = { (void *)CallbackRun, (void *)CallbackRun, (void *)CallbackSize };

// Each thunk has unwind data, so a stack walk or an exception during its counting call unwinds through it.
static int Unwinds(void *code)
{
    DWORD64 base = 0;
    return code != NULL && RtlLookupFunctionEntry((DWORD64)(uintptr_t)code, &base, NULL) != NULL;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: census_test <census steam_api64.dll> <log>\n"); return 2; }
    DeleteFileW(argv[2]);
    SetEnvironmentVariableW(L"HELLFORGE_STEAM_CENSUS_LOG", argv[2]);
    SetEnvironmentVariableW(L"HELLFORGE_STEAM_CENSUS_PERIOD_MS", L"100");
    HMODULE census = LoadLibraryW(argv[1]);
    Check(census != NULL, "the census build loads");
    if (census == NULL) return 1;

    FindInterfaceFn find = (FindInterfaceFn)(void *)GetProcAddress(census, "SteamInternal_FindOrCreateUserInterface");
    void *proxy = find ? find(1, "SteamUtils010") : NULL;
    HMODULE fake = GetModuleHandleW(L"steam_api64_hellforge.dll");
    FakeSteamState *state = fake ? ((FakeSteamState *(*)(void))(void *)GetProcAddress(fake, "FakeSteam_State"))() : NULL;
    void *utils = fake ? ((void *(*)(void))(void *)GetProcAddress(fake, "FakeSteam_Utils"))() : NULL;
    Check(state != NULL && utils != NULL, "the census build loads the real DLL next to it");
    if (state == NULL || utils == NULL || proxy == NULL) return 1;
    Check(proxy != utils, "the game gets a counting proxy, not the real interface object");
    Check(find(1, "SteamUtils010") == proxy, "the same interface object gets the same proxy");
    Check(find(1, "SteamUnknown001") == NULL, "an interface the real DLL does not have stays NULL");

    void *const *slots = *(void *const *const *)proxy;
    state->self = NULL;
    Check(((SlotAppIdFn)slots[0])(proxy) == 553850 && state->self == utils, "slot 0: the result, and the real object in RCX");
    uint64_t steamId = 0;
    uint64_t *returned = ((SlotSteamIdFn)slots[1])(proxy, &steamId);
    Check(returned == &steamId && steamId == 76561197960287930ull, "slot 1: a class result through the hidden pointer in RDX");
    uint64_t six = 0;
    for (int i = 0; i < 3; i++) six = ((SlotSixFn)slots[2])(proxy, 1, 10, 100, 1000, 10000);
    Check(six == 1 + 20 + 300 + 4000 + 50000, "slot 2: register and stack arguments");
    float floats = ((SlotFloatsFn)slots[3])(proxy, 1.5f, 2.25, 4.0f);
    Check(floats == 7.75f, "slot 3: XMM arguments and an XMM result");
    Check(((SlotIntFn)slots[191])(proxy) == 191, "slot 191, the last proxied slot");
    Check(Unwinds(slots[0]) && Unwinds(slots[191]), "slot thunks have unwind data");

    GetUserFn user = (GetUserFn)(void *)GetProcAddress(census, "SteamGameServer_GetHSteamUser");
    Check(user != NULL && user() == 7 && user() == 7, "an export thunk: the real function's result");
    Check(Unwinds((void *)user), "export thunks have unwind data");
    AvgRateFn rate = (AvgRateFn)(void *)GetProcAddress(census, "SteamAPI_ISteamUserStats_UpdateAvgRateStat");
    state->self = NULL;
    Check(rate != NULL && rate(utils, "rate", 3.5f, 9.25) && state->self == utils, "an export thunk: a pointer, a string and XMM2-XMM3");
    VibrationFn vibrate = (VibrationFn)(void *)GetProcAddress(census, "SteamAPI_ISteamInput_TriggerVibrationExtended");
    if (vibrate) vibrate(utils, 0x1122334455667788ull, 11, 22, 33, 44);
    Check(vibrate != NULL && state->vibration[0] == 0x1122334455667788ull && state->vibration[1] == 11 && state->vibration[2] == 22 &&
        state->vibration[3] == 33 && state->vibration[4] == 44, "an export thunk: six arguments, two of them on the stack");
    RunningFn running = (RunningFn)(void *)GetProcAddress(census, "SteamGameServer_GetHSteamPipe");
    Check(running != NULL && !running(), "an export the real DLL lacks returns 0");

    RegisterCallbackFn registerCallback = (RegisterCallbackFn)(void *)GetProcAddress(census, "SteamAPI_RegisterCallback");
    FakeCallback callback = { g_callbackVtable, 0, { 0 }, 1005 };
    if (registerCallback) registerCallback(&callback, 1005);
    Check(state->callback == &callback && state->callbackId == 1005, "a callback registration reaches the real DLL");

    Sleep(400);
    char *log = ReadAll(argv[2]);
    Check(log != NULL && Has(log, "\"kind\":\"census\"", "census build", NULL), "the census log says it is the census build");
    Check(log && Has(log, "\"kind\":\"interface\"", "\"name\":\"SteamUtils010\"", "\"number\":1}"), "the interface request is logged");
    Check(log && Has(log, "\"kind\":\"slot\"", "\"name\":\"SteamUtils010\"", "\"number\":2}"), "a slot's first call is logged");
    Check(log && Has(log, "\"kind\":\"export\"", "\"name\":\"SteamGameServer_GetHSteamUser\"", NULL), "an export's first call is logged");
    Check(log && Has(log, "\"kind\":\"function\"", "\"name\":\"SteamGameServer_GetHSteamUser\"", NULL), "a lookup by name is logged");
    Check(log && Has(log, "\"kind\":\"missing\"", "\"name\":\"SteamGameServer_GetHSteamPipe\"", NULL), "an export the real DLL lacks is logged");
    Check(log && Has(log, "\"kind\":\"callbacksize\"", "\"name\":\"1005\"", "\"number\":24}"), "a callback's struct size is logged");
    Check(log && Has(log, "\"kind\":\"calls\"", "\"name\":\"SteamUtils010#2\"", "\"number\":3}"), "the report counts a slot's calls");
    Check(log && Has(log, "\"kind\":\"calls\"", "\"name\":\"SteamGameServer_GetHSteamUser\"", "\"number\":2}"), "the report counts an export's calls");
    printf("%s\n", g_failed ? "Steam census build: FAILED" : "PASS Steam census build: export and slot thunks, proxies, sizes, counts");
    return g_failed ? 1 : 0;
}
