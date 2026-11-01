/* The broker's serving loop (bridge_broker.h). Phase 2 answers the flat functions the stand-in bridges; phase 3's
 * generated dispatch and allow-list add the interface methods. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "bridge_broker.h"
#include "broker_dispatch.h"

/* An interface method, through the generated dispatch. */
static uint32_t HandleMethod(const BrokerSteam* steam, uint32_t function, const uint8_t* args, uint32_t argsSize,
                             uint8_t* result, uint32_t capacity, uint32_t* resultSize)
{
    if (!steam || !steam->api) return BRIDGE_STATUS_UNKNOWN;
    BridgeReader r = { argsSize, 0, args };
    static BridgeWriter w; /* the one serving thread */
    w = BridgeWriterStart();
    uint32_t status = steam->api->dispatch ? steam->api->dispatch(steam->api, function, &r, &w) : BRIDGE_STATUS_UNKNOWN;
    if (status != BRIDGE_STATUS_OK) return status;
    if (w.size > capacity) return BRIDGE_STATUS_MALFORMED;
    memcpy(result, w.data, w.size);
    *resultSize = w.size;
    return BRIDGE_STATUS_OK;
}

uint32_t BrokerHandleCall(const BrokerSteam* steam, uint32_t function, const uint8_t* args, uint32_t argsSize,
                          uint8_t* result, uint32_t capacity, uint32_t* resultSize)
{
    *resultSize = 0;
    if (function >= BRIDGE_FN_METHODS) return HandleMethod(steam, function, args, argsSize, result, capacity, resultSize);
    switch (function)
    {
    case BRIDGE_FN_IS_STEAM_RUNNING:
        if (argsSize != 0 || capacity < 1) return BRIDGE_STATUS_MALFORMED;
        result[0] = (uint8_t)(steam && steam->isSteamRunning && steam->isSteamRunning(steam->context) ? 1 : 0);
        *resultSize = 1;
        return BRIDGE_STATUS_OK;
    default:
        return BRIDGE_STATUS_UNKNOWN;
    }
}

static int Reply(BrokerChannel* ch, uint32_t seq, uint32_t status, const uint8_t* result, uint32_t resultSize)
{
    uint8_t reply[BRIDGE_MSG_BUF - 4];
    if (resultSize > sizeof reply - 8) { resultSize = 0; status = BRIDGE_STATUS_MALFORMED; }
    memcpy(reply, &seq, 4);
    memcpy(reply + 4, &status, 4);
    if (resultSize > 0) memcpy(reply + 8, result, resultSize);
    return BrokerChannelSend(ch, BRIDGE_RESULT, reply, 8 + resultSize);
}

/* Steam's dispatch, when BROKER_PUMP_MS have passed since the last: how long ago the last one ran (0: just now). */
static DWORD PumpWhenDue(BrokerChannel* ch, const BrokerSteam* steam, DWORD* lastPump)
{
    DWORD now = GetTickCount(), since = now - *lastPump;
    if (since < BROKER_PUMP_MS) return since;
    if (steam && steam->pump) steam->pump(steam->context, ch);
    *lastPump = now;
    return 0;
}

/* Answers one message that is not a shutdown: a call with its result, anything else as malformed (oversized, too short,
 * or not a call), for whichever call it was meant to be. Returns 0 when the reply could not be sent. */
static int Answer(BrokerChannel* ch, const BrokerSteam* steam, uint32_t type, const uint8_t* body, uint32_t bodySize,
                  uint8_t* result, uint32_t capacity)
{
    if (type != BRIDGE_CALL || bodySize < 8)
    {
        uint32_t seq = 0;
        if (type == BRIDGE_CALL && bodySize >= 4) memcpy(&seq, body, 4);
        return Reply(ch, seq, BRIDGE_STATUS_MALFORMED, NULL, 0);
    }
    uint32_t seq, function, resultSize = 0;
    memcpy(&seq, body, 4);
    memcpy(&function, body + 4, 4);
    uint32_t status = BrokerHandleCall(steam, function, body + 8, bodySize - 8, result, capacity, &resultSize);
    return Reply(ch, seq, status, result, resultSize);
}

int BrokerServe(BrokerChannel* ch, const BrokerSteam* steam, HANDLE game)
{
    uint8_t body[BRIDGE_MSG_BUF];
    uint8_t result[BRIDGE_MSG_BUF - 12];
    DWORD lastPump = GetTickCount() - BROKER_PUMP_MS;
    for (;;)
    {
        /* Steam's dispatch runs at least every BROKER_PUMP_MS, however busy the pipe is */
        DWORD since = PumpWhenDue(ch, steam, &lastPump);
        uint32_t bodySize = 0;
        int gone = 0;
        uint32_t type = BrokerChannelRecv(ch, game, BROKER_PUMP_MS - since, body, sizeof body, &bodySize, &gone);
        if (gone) return game && WaitForSingleObject(game, 0) == WAIT_OBJECT_0 ? BROKER_END_GAME_GONE : BROKER_END_CLIENT_GONE;
        if (type == 0 && bodySize == 0) continue; /* the wait ran out: pump */
        if (type == BRIDGE_SHUTDOWN) return BROKER_END_SHUTDOWN;
        if (!Answer(ch, steam, type, body, bodySize, result, sizeof result)) return BROKER_END_CLIENT_GONE;
    }
}
