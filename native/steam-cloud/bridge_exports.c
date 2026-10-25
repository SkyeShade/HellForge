// The exports only the bridge build of the stand-in writes itself (docs/steam-bridge.md, phase 3): in the forwarding build
// they forward to Valve's DLL, here they answer from the bridge, so nothing reaches Valve's code in the game. Every other
// export the stand-in does not write is a generated stub that refuses (bridge_exports_gen.c).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "bridge_gen.h"
#include "bridge_local.h"

#define HELLFORGE_API

void BridgeLog(const char* format, ...);  // steam_cloud.c
void* SteamCloudStorage(void);           // steam_cloud.c: the Steam Cloud storage interface
#define STORAGE_VERSION "STEAMREMOTESTORAGE_INTERFACE_VERSION016"
#define CLIENT_VERSION "SteamClient020"

// An interface the allow-list does not know: an object whose every slot answers 0 (false, NULL, no handle), logged once
// per version. A method returning a class through a hidden pointer gets that pointer back unwritten.
#define NULL_SLOTS 192
static uint64_t NullSlot(void* self, void* second) { (void)self; (void)second; BridgeRefused("a method of an interface the bridge does not know"); return 0; }
static void* const g_nullVtable[NULL_SLOTS] = { [0 ... NULL_SLOTS - 1] = (void*)NullSlot };
static struct { void* const* vtable; } g_nullObject = { g_nullVtable };

void* BridgeInterface(const char* version)
{
    if (!version) return NULL;
    if (strcmp(version, STORAGE_VERSION) == 0) return SteamCloudStorage();
    void* proxy = BridgeProxy(version);
    if (proxy) return proxy;
    char line[96];
    strncpy(line, version, sizeof line - 1);
    line[sizeof line - 1] = 0;
    BridgeLog("bridge: %s is not bridged; its object answers 0", line);
    return &g_nullObject;
}

// The inline accessors (SteamUser() and the rest, steam_api_internal.h) pass { void (*fn)(void *ctx); uintptr_t counter;
// void *ptr }: fn fills ptr whenever counter is not the current one. A compare and a return on every call.
static volatile LONG64 g_contextCounter = 1;

typedef struct ContextInitData { void (*fn)(void* ctx); uintptr_t counter; void* ptr; } ContextInitData;

HELLFORGE_API void* SteamInternal_ContextInit(void* pContextInitData)
{
    ContextInitData* data = (ContextInitData*)pContextInitData;
    uintptr_t now = (uintptr_t)g_contextCounter;
    if (data->counter != now)
    {
        data->fn(&data->ptr);
        data->counter = now;
    }
    return &data->ptr;
}

// SteamAPI_Init and SteamAPI_Shutdown: the accessors fetch their objects again.
void BridgeContextsChanged(void) { InterlockedIncrement64(&g_contextCounter); }

HELLFORGE_API void* SteamInternal_CreateInterface(const char* ver)
{
    if (ver && strcmp(ver, CLIENT_VERSION) == 0) return BridgeProxy(CLIENT_VERSION);
    return BridgeInterface(ver);
}

// The game is never a game server: with no SteamGameServer_Init, Valve's answer is NULL (SteamGameServer_GetHSteamUser
// is 0), and the game takes NULL to mean none. A null object instead looked like a running game server, and the game
// crashed on the title screen calling it (2026-10-23).
HELLFORGE_API void* SteamInternal_FindOrCreateGameServerInterface(int32_t user, const char* version)
{
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) <= 8)
    {
        char line[96];
        strncpy(line, version ? version : "(null)", sizeof line - 1);
        line[sizeof line - 1] = 0;
        BridgeLog("bridge: game-server interface %s (user %d) -> NULL: no game server", line, user);
    }
    return NULL;
}

HELLFORGE_API void SteamAPI_UnregisterCallResult(void* callback, uint64_t call) { BridgeUnregisterCallResult(callback, call); }
