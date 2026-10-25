// A stand-in for Valve's steam_api64.dll in the Steam census test (census_test.c): built as steam_api64_hellforge.dll next
// to the census build of the stand-in, it exports a few of Valve's names with signatures that show whether the census's
// thunks pass every argument (registers, XMM registers, stack) and the result through untouched. FakeSteam_State reports
// what it saw.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define FAKE_API __declspec(dllexport)
#define FAKE_SLOTS 192

typedef struct FakeSteamState
{
    void *self;                  // the last object a slot or flat function was called on
    uint64_t vibration[5];       // TriggerVibrationExtended's handle and four speeds
    void *callback;              // the last registered callback object and its id
    int callbackId;
    int interfaceRequests;
} FakeSteamState;

static FakeSteamState g_state;

typedef struct FakeInterface { void *const *vtable; int marker; } FakeInterface;

static uint32_t SlotAppId(FakeInterface *self) { g_state.self = self; return 553850; }
// An MSVC member function returning a class: this in RCX, the hidden result pointer in RDX, returned in RAX.
static uint64_t *SlotSteamId(FakeInterface *self, uint64_t *result) { g_state.self = self; *result = 76561197960287930ull; return result; }
// Two arguments on the stack.
static uint64_t SlotSix(FakeInterface *self, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    g_state.self = self;
    return a + 2 * b + 3 * c + 4 * d + 5 * e;
}
// XMM1-XMM3 in, XMM0 out.
static float SlotFloats(FakeInterface *self, float x, double y, float z) { g_state.self = self; return (float)(x + y + z); }
static int SlotLast(FakeInterface *self) { g_state.self = self; return FAKE_SLOTS - 1; }
static int SlotOther(FakeInterface *self) { g_state.self = self; return -1; }

static void *g_vtable[FAKE_SLOTS];
static FakeInterface g_utils = { g_vtable, 0x5EA7 };

FAKE_API FakeSteamState *FakeSteam_State(void) { return &g_state; }
FAKE_API void *FakeSteam_Utils(void) { return &g_utils; }

FAKE_API void *SteamInternal_FindOrCreateUserInterface(int32_t user, const char *version)
{
    g_state.interfaceRequests++;
    return version && !strcmp(version, "SteamUtils010") ? &g_utils : NULL;
}

FAKE_API void SteamAPI_RegisterCallback(void *callback, int id) { g_state.callback = callback; g_state.callbackId = id; }
FAKE_API void SteamAPI_RegisterCallResult(void *callback, uint64_t call) { g_state.callback = callback; g_state.callbackId = (int)call; }
// the stand-in writes SteamAPI_GetHSteamUser itself (bridge mode), so the thunk tests use the game server one
FAKE_API int32_t SteamGameServer_GetHSteamUser(void) { return 7; }

FAKE_API bool SteamAPI_ISteamUserStats_UpdateAvgRateStat(void *self, const char *name, float count, double session)
{
    g_state.self = self;
    return name && !strcmp(name, "rate") && count == 3.5f && session == 9.25;
}

FAKE_API void SteamAPI_ISteamInput_TriggerVibrationExtended(void *self, uint64_t handle, unsigned short left, unsigned short right,
    unsigned short leftTrigger, unsigned short rightTrigger)
{
    g_state.self = self;
    g_state.vibration[0] = handle;
    g_state.vibration[1] = left;
    g_state.vibration[2] = right;
    g_state.vibration[3] = leftTrigger;
    g_state.vibration[4] = rightTrigger;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        for (int i = 0; i < FAKE_SLOTS; i++) g_vtable[i] = (void *)SlotOther;
        g_vtable[0] = (void *)SlotAppId;
        g_vtable[1] = (void *)SlotSteamId;
        g_vtable[2] = (void *)SlotSix;
        g_vtable[3] = (void *)SlotFloats;
        g_vtable[FAKE_SLOTS - 1] = (void *)SlotLast;
    }
    return TRUE;
}
