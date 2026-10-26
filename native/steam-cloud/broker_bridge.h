/* The broker's Steam side of the bridge (docs/steam-bridge.md, phase 3): Valve's DLL behind the generated dispatch
 * (BrokerApi), manual dispatch into the ring with only the allow-list's callbacks, call results, the lobbies the game
 * made, and the snapshot. Separate from main so tests drive it with a fake Steam DLL. */
#pragma once
#include "bridge_broker.h"
#include "broker_dispatch.h"
#include "broker_lobby.h"

#define BROKER_SNAPSHOT_MS 1000
#define BROKER_MAX_LOBBIES 16
/* how often the settings file's time stamp is looked at, as the backend does (StrangerSetting.Recheck) */
#define BROKER_STRANGERS_MS 2000
#define BROKER_PENDING_LOBBIES 4
#define BROKER_MAX_PEERS 64
/* Steam P2P: the channels the broker reads for the game, and how many packets a channel gives up in one pump */
#define BROKER_P2P_CHANNELS 8
#define BROKER_P2P_PER_PUMP 32

typedef struct BrokerBridge {
    HMODULE valve;
    int hPipe;
    BrokerApi api;
    BrokerSteam steam;
    uint64_t lobbies[BROKER_MAX_LOBBIES];     /* the lobbies the game made or entered (RememberEntered) */
    int32_t lobbyAsked[BROKER_MAX_LOBBIES];   /* the lobby type the game asked for each; -1 for one it entered */
    int lobbyCount;
    /* CreateLobby's types asked for, oldest first, until their LobbyCreated_t (the calls run in order) */
    int32_t pendingAsked[BROKER_PENDING_LOBBIES];
    int pendingCount;
    /* Play with strangers (mods/hellforge-mods.json "playWithStrangers", off without the file): the mods folder, the
     * settings file's time stamp when read, when it was last looked at */
    wchar_t modsFolder[520];
    /* Steam P2P's co-op peers (BrokerBridgeWatchPeers): the backend's file, its time stamp when read, when (system time)
     * it was read, when last looked at */
    wchar_t peersFile[520];
    uint64_t peers[BROKER_MAX_PEERS];
    int peerCount;
    FILETIME peersStamp, peersRead;
    DWORD peersChecked;
    uint32_t p2pDropped;            /* packets not passed on: from someone else, too large, or the ring full */

    /* several installations on one PC (BrokerBridgeMultiInstance): the game's own Steam lobby kept locally, and the
     * SteamID and name the snapshot gives the game instead of the account's */
    BrokerLobbies* localLobbies;
    uint64_t identity;
    char persona[128];
    int strangers;
    FILETIME strangersStamp;
    DWORD strangersChecked;
    DWORD lastSnapshot;
    int snapshotDone;
    uint32_t refusals;
    struct { const char* name; void* object; } selves[16];  /* the accessors' objects, each looked up once */
    void (*runFrame)(int hPipe);
    void* next;                 /* SteamAPI_ManualDispatch_GetNextCallback */
    void (*freeLast)(int hPipe);
} BrokerBridge;

/* Fills b for a loaded and initialized Valve DLL (or a fake) and its Steam pipe. Returns 0 when manual dispatch's
 * exports are missing. */
int BrokerBridgeOpen(BrokerBridge* b, HMODULE valve, int hPipe);

/* Reads Play with strangers from that mods folder now, and again whenever its settings file changes (the pump looks
 * every BROKER_STRANGERS_MS). Without it, strangers are off. Call before serving. */
void BrokerBridgeWatchStrangers(BrokerBridge* b, const wchar_t* modsFolder);

/* Steam P2P passes only to and from the game's co-op peers: the SteamIDs in this file (the backend's lobby and Party
 * peers, decimal, one a line; written as they change). Without the file, to no one. Call before serving. */
void BrokerBridgeWatchPeers(BrokerBridge* b, const wchar_t* peersFile);

/* Several installations on one PC (HELLFORGE_MULTI_INSTANCE=1): the game is this SteamID and name, and its own Steam
 * lobby is kept with the other instances' in that folder (broker_lobby.h). Call before serving; 0 when the id is not a
 * SteamID64 or the folder cannot be used. */
int BrokerBridgeMultiInstance(BrokerBridge* b, const wchar_t* lobbyFolder, uint64_t identity, const char* persona);

/* Play with strangers in a settings file's text: a top-level "playWithStrangers": true. */
int BrokerStrangersIn(const char* json, size_t length);

/* Fills the snapshot in ch's mapping now (the pump also does it every BROKER_SNAPSHOT_MS). */
void BrokerBridgeSnapshot(BrokerBridge* b, BrokerChannel* ch);
