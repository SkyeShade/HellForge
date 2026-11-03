/* Steam Input and a sandboxed game (docs/steam-bridge.md, "Controllers and Steam Input"). On Windows, Steam Input
 * hands a game the controllers it handles (a Steam Controller, the Steam Deck's controls, a PlayStation controller with
 * Steam Input on) through Steam's overlay, which SteamAPI_Init loads into the process that calls it: the broker, not
 * the low-integrity game. So those controllers never reach a sandboxed game.
 *
 * The broker asks Steam which controllers Steam Input is handling for the game, and logs them on one line the session
 * reads (SteamInputNote in HellForge.Core) to tell the player. It also logs what a relay to the game would depend on:
 * whether Steam's overlay is in the broker, which XInput pad Steam emulates for each controller, and whether that pad
 * changes in the broker's own XInput while the player plays. Nothing here reaches the game. Separate from
 * broker_bridge.c, so tests drive it with fakes and without the Steamworks SDK. */
#pragma once
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#define BROKER_INPUT_MS 2000            /* how often Steam is asked */
#define BROKER_INPUT_REPORT_MS 60000    /* how often the emulated pads' activity is logged */
#define BROKER_INPUT_MAX 16             /* STEAM_INPUT_MAX_COUNT */
#define BROKER_INPUT_PADS 4             /* XInput's pads */
#define BROKER_INPUT_LINE 1024

/* Valve's flat functions (SDK 1.55; the game's pinned steam_api64.dll exports them all), and this process's XInput and
 * overlay, as BrokerInputOpen finds them; tests put fakes here. */
typedef struct BrokerInputApi {
    void* (*accessor)(void);                              /* SteamAPI_SteamInput_v006 */
    bool (*init)(void* self, bool explicitlyCallRunFrame); /* SteamAPI_ISteamInput_Init */
    void (*runFrame)(void* self, bool reserved);          /* SteamAPI_ISteamInput_RunFrame */
    int32_t (*connected)(void* self, uint64_t* handles);  /* GetConnectedControllers: BROKER_INPUT_MAX handles */
    int32_t (*type)(void* self, uint64_t handle);         /* GetInputTypeForHandle: an ESteamInputType */
    int32_t (*pad)(void* self, uint64_t handle);          /* GetGamepadIndexForController: the XInput pad, or -1 */
    DWORD (WINAPI *xinputState)(DWORD pad, void* state);  /* XInputGetState (an XINPUT_STATE: 16 bytes) */
    bool (*overlayLoaded)(void);                          /* Steam's overlay is in this process */
} BrokerInputApi;

typedef struct BrokerInput {
    BrokerInputApi api;
    void (*say)(const char* line);     /* where the lines go (the broker: its log, standard error) */
    void* input;                       /* ISteamInput, once its Init succeeded */
    int state;                         /* 0 not asked yet, 1 asking, -1 unavailable */
    int listed;                        /* the list was logged */
    int count;
    int32_t types[BROKER_INPUT_MAX];   /* the list logged last: each controller's type and pad */
    int32_t pads[BROKER_INPUT_MAX];
    DWORD lastPoll, lastReport;
    int xinputTried;
    uint32_t packets[BROKER_INPUT_PADS];   /* each emulated pad's XInput packet number when read last */
    uint32_t changes[BROKER_INPUT_PADS];   /* its state changes since the last report */
    int read[BROKER_INPUT_PADS];           /* 1 once read; -1 once logged as missing */
} BrokerInput;

/* Finds Valve's flat Steam Input functions in valve (with any missing, Steam Input is not asked) and this process's
 * overlay; XInput is loaded only once Steam emulates a pad. Each line goes to say (NULL: standard error). */
void BrokerInputOpen(BrokerInput* in, HMODULE valve, void (*say)(const char* line));

/* Asks Steam at most every BROKER_INPUT_MS (now: GetTickCount). The first time: Init (RunFrame is called here) and
 * whether the overlay is in this process. Then each time: the controllers, logged when they change, and the emulated
 * pads' XInput, whose changes are logged every BROKER_INPUT_REPORT_MS. On the broker's Steam thread. */
void BrokerInputPoll(BrokerInput* in, DWORD now);

/* An ESteamInputType's name, as the session shows it too (SteamInputNote.Name). */
const char* BrokerInputTypeName(int32_t type);
