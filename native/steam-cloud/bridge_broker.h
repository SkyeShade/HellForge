/* The broker's serving loop (docs/steam-bridge.md): one thread answers the game's calls and, between them, pumps Steam's
 * manual dispatch into the ring, so every Steam call stays on that thread. Separate from main so tests can drive it. */
#pragma once
#include "bridge_channel.h"

typedef struct BrokerSteam {
    void* context;
    /* manual dispatch: push what Steam has for the game (BrokerChannelPushCallback); may be NULL */
    void (*pump)(void* context, BrokerChannel* ch);
    /* SteamAPI_IsSteamRunning */
    int (*isSteamRunning)(void* context);
    /* the generated dispatch's needs (broker_dispatch.h); NULL: interface methods are unknown */
    const struct BrokerApi* api;
} BrokerSteam;

/* How long the loop waits for a call before it pumps again. */
#define BROKER_PUMP_MS 16

/* Why the loop ended. */
enum { BROKER_END_SHUTDOWN = 1, BROKER_END_CLIENT_GONE = 2, BROKER_END_GAME_GONE = 3 };

/* Serves the accepted client until it says BRIDGE_SHUTDOWN, its pipe breaks, or `game` (optional) exits. */
int BrokerServe(BrokerChannel* ch, const BrokerSteam* steam, HANDLE game);

/* One call's answer: status and result bytes (exposed for tests). */
uint32_t BrokerHandleCall(const BrokerSteam* steam, uint32_t function, const uint8_t* args, uint32_t argsSize,
                          uint8_t* result, uint32_t capacity, uint32_t* resultSize);
