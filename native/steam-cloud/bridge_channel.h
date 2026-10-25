/* The Steam bridge channel (docs/steam-bridge.md): a named pipe (duplex, message mode) with a low mandatory label, so
 * the game at low integrity can open it, and a callback ring (an unnamed section the broker duplicates into the game once
 * it has checked the client). The broker creates both; the game connects. */
#pragma once
#include <windows.h>
#include "bridge_protocol.h"

#define BRIDGE_MSG_BUF 8192

/* --- Broker (server) side: an overlapped pipe, so waits can also watch the game's process --- */

typedef struct BrokerChannel {
    HANDLE pipe;
    HANDLE ringMapping;
    BridgeRingHeader* ring;
    OVERLAPPED readOv, writeOv;
    int readPending;        /* a ReadFile into rbuf is in flight (kept across Recv timeouts, so no message is lost) */
    uint64_t writeSeq;      /* the broker's own; ring->writeSeq is only its published copy */
    uint32_t dropped;       /* callbacks lost to a full ring */
    uint32_t refused;       /* clients refused by Accept */
    DWORD clientPid;
    wchar_t pipeName[128];
    uint8_t rbuf[BRIDGE_MSG_BUF];
} BrokerChannel;

/* The ring, then the pipe (FILE_FLAG_FIRST_PIPE_INSTANCE: fails when anything else made the name first). */
int  BrokerChannelCreate(BrokerChannel* ch, const wchar_t* suffix);
void BrokerChannelClose(BrokerChannel* ch);

/* Accept results. */
enum { BROKER_ACCEPTED = 1, BROKER_GAME_GONE = 0, BROKER_ERROR = -1 };

/* Waits for the expected client (expectedPid 0: any). A client whose pipe pid differs from its HELLO, or from
 * expectedPid, or that says nothing for helloTimeoutMs, is refused and disconnected, and the wait goes on. Returns
 * BROKER_GAME_GONE as soon as `game` (optional) is signalled. */
int BrokerChannelAccept(BrokerChannel* ch, DWORD expectedPid, HANDLE game, DWORD helloTimeoutMs);

/* One message from the client within timeoutMs. Returns its type; 0 on timeout (*gone stays 0), on a broken pipe or the
 * game's exit (*gone = 1). An oversized message is read to its end and reported as type 0 with *bodySize = UINT32_MAX. */
uint32_t BrokerChannelRecv(BrokerChannel* ch, HANDLE game, DWORD timeoutMs, void* body, uint32_t capacity, uint32_t* bodySize, int* gone);
int BrokerChannelSend(BrokerChannel* ch, uint32_t type, const void* body, uint32_t bodySize);

/* Push a callback into the ring. Returns 0 when the ring is full (counted in ch->dropped) or the data exceeds a slot. */
int BrokerChannelPushCallback(BrokerChannel* ch, uint32_t callbackId, const void* data, uint32_t dataSize);

/* --- Game (client) side: synchronous, calls serialized by a lock --- */

/* What the round trips cost the game's threads (docs/steam-bridge.md, Cost), in QueryPerformanceCounter ticks from the
 * call to its reply, the lock's wait included. The first BRIDGE_COST_FUNCTIONS functions called are counted apart. */
#define BRIDGE_COST_FUNCTIONS 16
typedef struct GameChannelCost {
    uint64_t calls, ticks, maxTicks;
    struct { uint32_t function, calls; uint64_t ticks; } byFunction[BRIDGE_COST_FUNCTIONS];
} GameChannelCost;

typedef struct GameChannel {
    HANDLE pipe;
    BridgeRingHeader* ring;
    uint64_t readSeq;       /* the game's own; ring->readSeq is only its published copy */
    uint32_t nextSeq;
    int dead;               /* the broker went away: every call fails from then on */
    CRITICAL_SECTION lock;
    int lockReady;
    GameChannelCost cost;   /* under the lock */
} GameChannel;

/* Opens the pipe, waiting up to timeoutMs for the broker to make it, then says HELLO and maps the ring it is given. */
int  GameChannelConnect(GameChannel* ch, const wchar_t* suffix, DWORD timeoutMs);
void GameChannelClose(GameChannel* ch);

/* One call and its reply. Returns 1 with *status set when the broker answered; 0 when it did not (the channel is then
 * dead). result receives min(resultSize, capacity) bytes; *resultSize is the reply's full size. */
int GameChannelCall(GameChannel* ch, uint32_t function, const void* args, uint32_t argsSize,
                    void* result, uint32_t capacity, uint32_t* resultSize, uint32_t* status);

/* The round trips' cost since the last take, and a fresh count from now. */
void GameChannelTakeCost(GameChannel* ch, GameChannelCost* out);

/* The idle check: nonzero when the broker has written a callback the game has not read (one shared-memory read). */
static inline int GameChannelPending(const GameChannel* ch) { return ch->ring && ch->ring->writeSeq != ch->readSeq; }

/* Read the next callback from the ring. Returns 0 when the ring is empty. */
int GameChannelReadCallback(GameChannel* ch, uint32_t* callbackId, void* data, uint32_t capacity, uint32_t* dataSize);

/* --- Synchronous pipe I/O (the game's side, and tests) --- */

/* Receive one pipe message. Returns the type (> 0), or 0 on error or a message larger than the buffer. */
uint32_t BridgePipeRecv(HANDLE pipe, void* body, uint32_t bodyCapacity, uint32_t* bodySize);
/* Send one pipe message: [uint32 type][body]. */
int BridgePipeSend(HANDLE pipe, uint32_t type, const void* body, uint32_t bodySize);
