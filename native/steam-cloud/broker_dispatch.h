/* The broker's side of the generated bridge (broker_dispatch_gen.c): what a call-mode method needs from the broker. Tests
 * fill it with a fake Steam DLL. */
#pragma once
#include <stdint.h>
#include "bridge_marshal.h"

typedef struct BrokerApi {
    void* context;
    /* Valve's flat export by name (NULL: missing) */
    void* (*resolve)(const struct BrokerApi* api, const char* flat);
    /* an interface's object, from its flat accessor (SteamAPI_SteamUser_v021) */
    void* (*self)(const struct BrokerApi* api, const char* accessor);
    /* a lobby the game made or entered this session */
    int (*ownLobby)(const struct BrokerApi* api, uint64_t lobby);
    /* logs why and returns BRIDGE_STATUS_REFUSED */
    uint32_t (*refuse)(const struct BrokerApi* api, const char* why);
    /* the table's "logged" strings */
    void (*log)(const struct BrokerApi* api, const char* what, const char* value);
    /* the table's "p2pPeer" SteamIDs: one of the game's co-op peers */
    int (*p2pPeer)(const struct BrokerApi* api, uint64_t steamId);
    /* the table's "adjust" integers: the value Valve's DLL gets for that method's argument */
    int64_t (*adjust)(const struct BrokerApi* api, uint32_t method, const char* argument, int64_t value);
    /* BrokerDispatchMethod: the serving loop reaches the generated code through this, so it builds without it */
    uint32_t (*dispatch)(const struct BrokerApi* api, uint32_t method, BridgeReader* r, BridgeWriter* w);
} BrokerApi;

/* A call-mode method's call: reads the arguments from r, checks them, calls Valve's flat export, writes the results to w.
 * Returns a BRIDGE_STATUS_*. */
uint32_t BrokerDispatchMethod(const BrokerApi* api, uint32_t method, BridgeReader* r, BridgeWriter* w);
