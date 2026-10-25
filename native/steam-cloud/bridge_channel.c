/* The Steam bridge channel (docs/steam-bridge.md).
 *
 * The pipe grants the player's own SID (read and write, nothing more) and carries a low mandatory label, so the game at
 * low integrity can open it; the broker checks the client's process id before it answers anything. The ring is an
 * unnamed section: no other process can open it by name, and the broker duplicates it into the checked client only.
 *
 * The pipe is message-mode and duplex: each WriteFile is one message, each ReadFile returns one whole message. The ring
 * has 60 fixed-size slots (1024 bytes each), single-producer (broker) single-consumer (game). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "bridge_channel.h"

static void PipeName(wchar_t* out, size_t size, const wchar_t* suffix)
{
    swprintf(out, size, L"\\\\.\\pipe\\hellforge-broker-%ls", suffix);
}

/* The current user's SID may read and write the pipe; low-integrity processes may too (no write up past the label). */
static PSECURITY_DESCRIPTOR PipeSecurity(void)
{
    static PSECURITY_DESCRIPTOR sd;
    if (sd) return sd;
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;
    union { TOKEN_USER user; BYTE bytes[256]; } info;
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenUser, &info, sizeof info, &size);
    CloseHandle(token);
    if (!ok) return NULL;
    wchar_t* sid = NULL;
    if (!ConvertSidToStringSidW(info.user.User.Sid, &sid)) return NULL;
    wchar_t sddl[256];
    int n = swprintf(sddl, 256, L"D:P(A;;GRGW;;;%ls)S:(ML;;NW;;;LW)", sid);
    LocalFree(sid);
    if (n <= 0 || !ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL)) { sd = NULL; return NULL; }
    return sd;
}

/* --- Synchronous pipe I/O --- */

uint32_t BridgePipeRecv(HANDLE pipe, void* body, uint32_t bodyCapacity, uint32_t* bodySize)
{
    uint8_t buf[BRIDGE_MSG_BUF];
    DWORD read = 0;
    if (!ReadFile(pipe, buf, sizeof buf, &read, NULL))
    {
        /* larger than any message the protocol has: read it to its end, then report it unusable */
        while (GetLastError() == ERROR_MORE_DATA && !ReadFile(pipe, buf, sizeof buf, &read, NULL)) {}
        return 0;
    }
    if (read < 4) return 0;
    uint32_t type;
    memcpy(&type, buf, 4);
    uint32_t n = read - 4;
    if (n > bodyCapacity) return 0;
    *bodySize = n;
    if (n > 0) memcpy(body, buf + 4, n);
    return type;
}

int BridgePipeSend(HANDLE pipe, uint32_t type, const void* body, uint32_t bodySize)
{
    uint8_t buf[BRIDGE_MSG_BUF];
    if (bodySize > sizeof buf - 4) return 0;
    uint32_t total = 4 + bodySize;
    memcpy(buf, &type, 4);
    if (bodySize > 0 && body) memcpy(buf + 4, body, bodySize);
    DWORD written = 0;
    return WriteFile(pipe, buf, total, &written, NULL) && written == total;
}

/* --- Broker (server) --- */

int BrokerChannelCreate(BrokerChannel* ch, const wchar_t* suffix)
{
    memset(ch, 0, sizeof *ch);
    ch->pipe = INVALID_HANDLE_VALUE;
    PipeName(ch->pipeName, 128, suffix);

    ch->readOv.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ch->writeOv.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ch->readOv.hEvent || !ch->writeOv.hEvent) goto fail;

    /* the ring first: once the pipe exists, a client may connect */
    ch->ringMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)BRIDGE_RING_MAP_SIZE, NULL);
    if (!ch->ringMapping) goto fail;
    ch->ring = (BridgeRingHeader*)MapViewOfFile(ch->ringMapping, FILE_MAP_ALL_ACCESS, 0, 0, BRIDGE_RING_MAP_SIZE);
    if (!ch->ring) goto fail;
    memset(ch->ring, 0, BRIDGE_RING_MAP_SIZE);

    SECURITY_ATTRIBUTES sa = { sizeof sa, PipeSecurity(), FALSE };
    if (!sa.lpSecurityDescriptor) goto fail;
    ch->pipe = CreateNamedPipeW(ch->pipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, BRIDGE_MSG_BUF, BRIDGE_MSG_BUF, 0, &sa);
    if (ch->pipe == INVALID_HANDLE_VALUE) goto fail;
    return 1;
fail:
    BrokerChannelClose(ch);
    return 0;
}

/* Ends the read in flight, if any, before rbuf or readOv is used for anything else. */
static void CancelRead(BrokerChannel* ch)
{
    if (!ch->readPending) return;
    DWORD bytes;
    CancelIoEx(ch->pipe, &ch->readOv);
    GetOverlappedResult(ch->pipe, &ch->readOv, &bytes, TRUE);
    ch->readPending = 0;
}

/* Waits for an event or the game's exit: 1 the event, 0 timeout, -1 the game is gone or the wait failed. */
static int Wait(HANDLE event, HANDLE game, DWORD timeoutMs)
{
    HANDLE waits[2] = { event, game };
    DWORD w = WaitForMultipleObjects(game ? 2 : 1, waits, FALSE, timeoutMs);
    if (w == WAIT_OBJECT_0) return 1;
    if (w == WAIT_TIMEOUT) return 0;
    return -1;
}

uint32_t BrokerChannelRecv(BrokerChannel* ch, HANDLE game, DWORD timeoutMs, void* body, uint32_t capacity, uint32_t* bodySize, int* gone)
{
    *gone = 0;
    *bodySize = 0;
    DWORD read = 0;
    BOOL more = FALSE;
    if (!ch->readPending)
    {
        if (!ResetEvent(ch->readOv.hEvent)) { *gone = 1; return 0; }
        if (ReadFile(ch->pipe, ch->rbuf, sizeof ch->rbuf, &read, &ch->readOv)) goto done;
        DWORD err = GetLastError();
        if (err == ERROR_MORE_DATA) { more = TRUE; goto done; }
        if (err != ERROR_IO_PENDING) { *gone = 1; return 0; }
        ch->readPending = 1;
    }
    int w = Wait(ch->readOv.hEvent, game, timeoutMs);
    if (w == 0) return 0;
    if (w < 0) { CancelRead(ch); *gone = 1; return 0; }
    ch->readPending = 0;
    if (!GetOverlappedResult(ch->pipe, &ch->readOv, &read, FALSE))
    {
        if (GetLastError() != ERROR_MORE_DATA) { *gone = 1; return 0; }
        more = TRUE;
    }
done:
    if (more)
    {
        /* larger than any message the protocol has: read it to its end, then report it unusable */
        for (;;)
        {
            if (!ResetEvent(ch->readOv.hEvent)) { *gone = 1; return 0; }
            if (ReadFile(ch->pipe, ch->rbuf, sizeof ch->rbuf, &read, &ch->readOv)) break;
            DWORD err = GetLastError();
            if (err == ERROR_MORE_DATA) continue;
            if (err != ERROR_IO_PENDING) { *gone = 1; return 0; }
            ch->readPending = 1;
            if (Wait(ch->readOv.hEvent, game, INFINITE) != 1) { CancelRead(ch); *gone = 1; return 0; }
            ch->readPending = 0;
            if (GetOverlappedResult(ch->pipe, &ch->readOv, &read, FALSE)) break;
            if (GetLastError() != ERROR_MORE_DATA) { *gone = 1; return 0; }
        }
        *bodySize = UINT32_MAX;
        return 0;
    }
    if (read < 4 || read - 4 > capacity) { *bodySize = UINT32_MAX; return 0; }
    uint32_t type;
    memcpy(&type, ch->rbuf, 4);
    *bodySize = read - 4;
    if (*bodySize > 0) memcpy(body, ch->rbuf + 4, *bodySize);
    return type;
}

int BrokerChannelSend(BrokerChannel* ch, uint32_t type, const void* body, uint32_t bodySize)
{
    uint8_t buf[BRIDGE_MSG_BUF];
    if (bodySize > sizeof buf - 4) return 0;
    uint32_t total = 4 + bodySize;
    memcpy(buf, &type, 4);
    if (bodySize > 0 && body) memcpy(buf + 4, body, bodySize);
    DWORD written = 0;
    if (!ResetEvent(ch->writeOv.hEvent)) return 0;
    if (!WriteFile(ch->pipe, buf, total, &written, &ch->writeOv))
    {
        if (GetLastError() != ERROR_IO_PENDING) return 0;
        /* a client that stops reading cannot hold the broker: the write is given up after 5 s */
        if (Wait(ch->writeOv.hEvent, NULL, 5000) != 1)
        {
            CancelIoEx(ch->pipe, &ch->writeOv);
            GetOverlappedResult(ch->pipe, &ch->writeOv, &written, TRUE);
            return 0;
        }
        if (!GetOverlappedResult(ch->pipe, &ch->writeOv, &written, FALSE)) return 0;
    }
    return written == total;
}

/* Gives the checked client its own handle to the ring. Returns 0 when it cannot. */
static int GiveRing(BrokerChannel* ch, DWORD pid)
{
    HANDLE client = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    if (!client) return 0;
    HANDLE theirs = NULL;
    int ok = DuplicateHandle(GetCurrentProcess(), ch->ringMapping, client, &theirs, FILE_MAP_READ | FILE_MAP_WRITE, FALSE, 0);
    if (ok)
    {
        uint64_t value = (uint64_t)(uintptr_t)theirs;
        ok = BrokerChannelSend(ch, BRIDGE_HELLO_OK, &value, sizeof value);
        /* not delivered: the handle must not stay behind in that process */
        if (!ok) DuplicateHandle(client, theirs, NULL, NULL, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
    }
    CloseHandle(client);
    return ok;
}

int BrokerChannelAccept(BrokerChannel* ch, DWORD expectedPid, HANDLE game, DWORD helloTimeoutMs)
{
    for (;;)
    {
        CancelRead(ch);
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.hEvent = ch->readOv.hEvent;
        if (!ResetEvent(ov.hEvent)) return BROKER_ERROR;
        if (!ConnectNamedPipe(ch->pipe, &ov))
        {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING)
            {
                int w = Wait(ov.hEvent, game, INFINITE);
                DWORD bytes;
                if (w != 1)
                {
                    CancelIoEx(ch->pipe, &ov);
                    GetOverlappedResult(ch->pipe, &ov, &bytes, TRUE);
                    return game && WaitForSingleObject(game, 0) == WAIT_OBJECT_0 ? BROKER_GAME_GONE : BROKER_ERROR;
                }
                if (!GetOverlappedResult(ch->pipe, &ov, &bytes, FALSE)) { DisconnectNamedPipe(ch->pipe); continue; }
            }
            else if (err == ERROR_NO_DATA) { DisconnectNamedPipe(ch->pipe); continue; } /* came and went */
            else if (err != ERROR_PIPE_CONNECTED) return BROKER_ERROR;
        }

        uint8_t body[16];
        uint32_t bodySize = 0;
        int gone = 0;
        uint32_t type = BrokerChannelRecv(ch, game, helloTimeoutMs, body, sizeof body, &bodySize, &gone);
        if (gone && game && WaitForSingleObject(game, 0) == WAIT_OBJECT_0) return BROKER_GAME_GONE;

        ULONG pipePid = 0;
        uint32_t declared = 0;
        int good = type == BRIDGE_HELLO && bodySize == sizeof declared && GetNamedPipeClientProcessId(ch->pipe, &pipePid);
        if (good) memcpy(&declared, body, sizeof declared);
        good = good && declared == pipePid && (expectedPid == 0 || pipePid == expectedPid) && GiveRing(ch, pipePid);
        if (good)
        {
            ch->clientPid = pipePid;
            return BROKER_ACCEPTED;
        }
        /* not the game (or silent): refused, and the broker waits for the next client */
        if (type != 0) BrokerChannelSend(ch, BRIDGE_HELLO_DENIED, NULL, 0);
        ch->refused++;
        CancelRead(ch);
        DisconnectNamedPipe(ch->pipe);
    }
}

int BrokerChannelPushCallback(BrokerChannel* ch, uint32_t callbackId, const void* data, uint32_t dataSize)
{
    if (dataSize > BRIDGE_RING_DATA_SIZE) return 0;
    uint64_t read = BridgeRingClampRead(ch->ring->readSeq, ch->writeSeq);
    if (ch->writeSeq - read >= BRIDGE_RING_SLOT_COUNT) { ch->dropped++; return 0; }

    BridgeRingSlot* slot = BridgeRingSlotAt(ch->ring, ch->writeSeq);
    slot->callbackId = callbackId;
    slot->dataSize = dataSize;
    if (dataSize > 0) memcpy(slot->data, data, dataSize);

    MemoryBarrier(); /* the slot before the sequence that publishes it */
    ch->writeSeq++;
    ch->ring->writeSeq = ch->writeSeq;
    return 1;
}

void BrokerChannelClose(BrokerChannel* ch)
{
    if (ch->pipe != INVALID_HANDLE_VALUE && ch->pipe != NULL)
    {
        CancelRead(ch);
        DisconnectNamedPipe(ch->pipe);
        CloseHandle(ch->pipe);
    }
    ch->pipe = INVALID_HANDLE_VALUE;
    if (ch->ring) { UnmapViewOfFile(ch->ring); ch->ring = NULL; }
    if (ch->ringMapping) { CloseHandle(ch->ringMapping); ch->ringMapping = NULL; }
    if (ch->readOv.hEvent) { CloseHandle(ch->readOv.hEvent); ch->readOv.hEvent = NULL; }
    if (ch->writeOv.hEvent) { CloseHandle(ch->writeOv.hEvent); ch->writeOv.hEvent = NULL; }
}

/* --- Game (client) --- */

static int GameHello(GameChannel* ch)
{
    uint32_t pid = GetCurrentProcessId();
    if (!BridgePipeSend(ch->pipe, BRIDGE_HELLO, &pid, sizeof pid)) return 0;
    uint8_t body[16];
    uint32_t bodySize = 0;
    if (BridgePipeRecv(ch->pipe, body, sizeof body, &bodySize) != BRIDGE_HELLO_OK || bodySize != sizeof(uint64_t)) return 0;
    uint64_t value;
    memcpy(&value, body, sizeof value);
    HANDLE section = (HANDLE)(uintptr_t)value;
    ch->ring = (BridgeRingHeader*)MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, BRIDGE_RING_MAP_SIZE);
    CloseHandle(section); /* the view keeps the section */
    if (!ch->ring) return 0;
    ch->readSeq = 0;
    return 1;
}

int GameChannelConnect(GameChannel* ch, const wchar_t* suffix, DWORD timeoutMs)
{
    memset(ch, 0, sizeof *ch);
    ch->pipe = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&ch->lock);
    ch->lockReady = 1;
    if (!suffix || !suffix[0] || wcslen(suffix) > 64) { ch->dead = 1; return 0; }
    wchar_t name[128];
    PipeName(name, 128, suffix);

    /* the broker may still be starting: wait for its pipe */
    DWORD start = GetTickCount();
    for (;;)
    {
        ch->pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (ch->pipe != INVALID_HANDLE_VALUE) break;
        DWORD err = GetLastError(), elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs) { ch->dead = 1; return 0; }
        if (err == ERROR_PIPE_BUSY) WaitNamedPipeW(name, timeoutMs - elapsed);
        else if (err == ERROR_FILE_NOT_FOUND) Sleep(20);
        else { ch->dead = 1; return 0; }
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(ch->pipe, &mode, NULL, NULL) || !GameHello(ch))
    {
        CloseHandle(ch->pipe);
        ch->pipe = INVALID_HANDLE_VALUE;
        ch->dead = 1;
        return 0;
    }
    return 1;
}

int GameChannelCall(GameChannel* ch, uint32_t function, const void* args, uint32_t argsSize,
                    void* result, uint32_t capacity, uint32_t* resultSize, uint32_t* status)
{
    if (!ch->lockReady) return 0;
    uint8_t buf[BRIDGE_MSG_BUF];
    if (argsSize > sizeof buf - 12) return 0;
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    EnterCriticalSection(&ch->lock);
    int ok = 0;
    if (!ch->dead && ch->pipe != INVALID_HANDLE_VALUE)
    {
        uint32_t seq = ++ch->nextSeq;
        memcpy(buf, &seq, 4);
        memcpy(buf + 4, &function, 4);
        if (argsSize > 0 && args) memcpy(buf + 8, args, argsSize);
        uint32_t replySize = 0, replySeq = 0;
        if (BridgePipeSend(ch->pipe, BRIDGE_CALL, buf, 8 + argsSize) &&
            BridgePipeRecv(ch->pipe, buf, sizeof buf, &replySize) == BRIDGE_RESULT && replySize >= 8)
        {
            memcpy(&replySeq, buf, 4);
            if (replySeq == seq)
            {
                memcpy(status, buf + 4, 4);
                uint32_t n = replySize - 8;
                if (resultSize) *resultSize = n;
                if (result && n > 0) memcpy(result, buf + 8, n < capacity ? n : capacity);
                ok = 1;
            }
        }
        if (!ok) ch->dead = 1;
        QueryPerformanceCounter(&end);
        uint64_t ticks = (uint64_t)(end.QuadPart - start.QuadPart);
        GameChannelCost* cost = &ch->cost;
        cost->calls++;
        cost->ticks += ticks;
        if (ticks > cost->maxTicks) cost->maxTicks = ticks;
        for (int i = 0; i < BRIDGE_COST_FUNCTIONS; i++)
            if (cost->byFunction[i].calls == 0 || cost->byFunction[i].function == function)
            {
                cost->byFunction[i].function = function;
                cost->byFunction[i].calls++;
                cost->byFunction[i].ticks += ticks;
                break;
            }
    }
    LeaveCriticalSection(&ch->lock);
    return ok;
}

void GameChannelTakeCost(GameChannel* ch, GameChannelCost* out)
{
    memset(out, 0, sizeof *out);
    if (!ch->lockReady) return;
    EnterCriticalSection(&ch->lock);
    *out = ch->cost;
    memset(&ch->cost, 0, sizeof ch->cost);
    LeaveCriticalSection(&ch->lock);
}

int GameChannelReadCallback(GameChannel* ch, uint32_t* callbackId, void* data, uint32_t capacity, uint32_t* dataSize)
{
    if (!ch->ring) return 0;
    uint64_t written = ch->ring->writeSeq;
    if (written == ch->readSeq) return 0;
    if (written < ch->readSeq || written - ch->readSeq > BRIDGE_RING_SLOT_COUNT) ch->readSeq = written > BRIDGE_RING_SLOT_COUNT ? written - BRIDGE_RING_SLOT_COUNT : 0;
    if (written == ch->readSeq) return 0;
    MemoryBarrier(); /* the sequence before the slot it published */
    BridgeRingSlot* slot = BridgeRingSlotAt(ch->ring, ch->readSeq);
    uint32_t size = slot->dataSize;
    if (size > BRIDGE_RING_DATA_SIZE) size = BRIDGE_RING_DATA_SIZE;
    *callbackId = slot->callbackId;
    *dataSize = size;
    if (data && size > 0) memcpy(data, slot->data, size < capacity ? size : capacity);
    MemoryBarrier(); /* the slot read before the broker may reuse it */
    ch->readSeq++;
    ch->ring->readSeq = ch->readSeq;
    return 1;
}

void GameChannelClose(GameChannel* ch)
{
    if (ch->lockReady) EnterCriticalSection(&ch->lock);
    if (!ch->dead && ch->pipe != INVALID_HANDLE_VALUE && ch->pipe != NULL) BridgePipeSend(ch->pipe, BRIDGE_SHUTDOWN, NULL, 0);
    if (ch->ring) { UnmapViewOfFile(ch->ring); ch->ring = NULL; }
    if (ch->pipe != INVALID_HANDLE_VALUE && ch->pipe != NULL) { CloseHandle(ch->pipe); ch->pipe = INVALID_HANDLE_VALUE; }
    ch->dead = 1;
    if (ch->lockReady) { LeaveCriticalSection(&ch->lock); DeleteCriticalSection(&ch->lock); ch->lockReady = 0; }
}
