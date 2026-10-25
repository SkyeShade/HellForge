/* Tests for the Steam bridge (bridge_channel.c, bridge_broker.c): the ring and its bounds against a game that writes
 * the header, the pipe's security, the client check (a second process with the wrong pid, a lying HELLO), the serving
 * loop with a malformed and an oversized message, a late broker, a dead broker and a game that exits. No Valve DLL: a
 * fake Steam pumps one callback.
 *
 * Usage: broker_test.exe [stand-in steam_api64.dll]   (the tests; with the stand-in, its bridge mode too: a fake
 *                                                      steam_api64_hellforge.dll beside it must never load)
 *        broker_test.exe client <suffix>   (a second process the broker must refuse; exits 3 when refused)
 * Exit 0 on success, 1 on failure. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../bridge_broker.h"

static int g_failures;
#define CHECK(ok, what) do { int ok_ = (ok); printf("%s %s\n", ok_ ? "  ok  " : "  FAIL", what); if (!ok_) g_failures++; } while (0)

/* ---- the ring ---- */

static void TestRing(void)
{
    BrokerChannel* ch = (BrokerChannel*)calloc(1, sizeof *ch);
    uint8_t* mem = (uint8_t*)VirtualAlloc(NULL, BRIDGE_RING_MAP_SIZE + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!ch || !mem) { CHECK(0, "ring: memory"); return; }
    /* a guard page after the ring: a write past it faults */
    DWORD old;
    size_t ringPages = (BRIDGE_RING_MAP_SIZE + 4095) & ~(size_t)4095;
    VirtualProtect(mem + ringPages, 4096, PAGE_NOACCESS, &old);
    ch->ring = (BridgeRingHeader*)(mem + ringPages - BRIDGE_RING_MAP_SIZE);

    GameChannel game;
    memset(&game, 0, sizeof game);
    game.ring = ch->ring;

    CHECK(!GameChannelPending(&game), "ring: empty at start");
    uint8_t payload[4] = { 0x11, 0x22, 0x33, 0x44 };
    CHECK(BrokerChannelPushCallback(ch, 1101, payload, sizeof payload), "ring: one callback pushed");
    CHECK(GameChannelPending(&game), "ring: the game sees it with one read");
    uint32_t id = 0, size = 0;
    uint8_t data[BRIDGE_RING_DATA_SIZE];
    CHECK(GameChannelReadCallback(&game, &id, data, sizeof data, &size) && id == 1101 && size == 4 && !memcmp(data, payload, 4),
        "ring: the game reads it back");
    CHECK(!GameChannelPending(&game) && ch->ring->readSeq == 1, "ring: empty again, read position published");

    int pushed = 0;
    for (unsigned i = 0; i < BRIDGE_RING_SLOT_COUNT + 5; i++) pushed += BrokerChannelPushCallback(ch, 2000 + i, NULL, 0);
    CHECK(pushed == (int)BRIDGE_RING_SLOT_COUNT && ch->dropped == 5, "ring: a full ring refuses and counts what it drops");
    CHECK(!BrokerChannelPushCallback(ch, 1, data, BRIDGE_RING_DATA_SIZE + 1), "ring: a callback larger than a slot is refused");

    /* the game writes nonsense into its read position: the broker clamps it and never writes outside the ring */
    uint64_t written = ch->writeSeq;
    CHECK(BridgeRingClampRead(UINT64_MAX, written) == written, "ring: a read position past the writes is clamped to them");
    CHECK(BridgeRingClampRead(0, written + 1000) == written + 1000 - BRIDGE_RING_SLOT_COUNT, "ring: one far behind is clamped to a ring");
    ch->ring->readSeq = UINT64_MAX; /* "everything read, and more" */
    int wrote = 0;
    for (int i = 0; i < 200; i++) wrote += BrokerChannelPushCallback(ch, 3000 + i, data, BRIDGE_RING_DATA_SIZE);
    CHECK(wrote > 0 && ch->writeSeq == ch->ring->writeSeq, "ring: pushes with a forged read position stay in bounds");
    ch->ring->readSeq = (uint64_t)-5; /* a "negative" one */
    CHECK(BrokerChannelPushCallback(ch, 4000, data, 8) || ch->dropped > 0, "ring: a negative read position stays in bounds");

    /* a forged write position: the game reader resyncs to the last ring of entries instead of reading past it */
    GameChannel reader;
    memset(&reader, 0, sizeof reader);
    reader.ring = ch->ring;
    reader.readSeq = 0;
    ch->ring->writeSeq = 1000000;
    CHECK(GameChannelReadCallback(&reader, &id, data, sizeof data, &size) && reader.readSeq == 1000000 - BRIDGE_RING_SLOT_COUNT + 1 &&
        size <= BRIDGE_RING_DATA_SIZE, "ring: the game resyncs when the write position jumps");

    VirtualFree(mem, 0, MEM_RELEASE);
    free(ch);
}

/* ---- a broker on a thread ---- */

typedef struct Fake { int pumps; int pushed; } Fake;

static void FakePump(void* context, BrokerChannel* ch)
{
    Fake* fake = (Fake*)context;
    fake->pumps++;
    uint8_t data[3] = { 1, 2, 3 };
    if (!fake->pushed && BrokerChannelPushCallback(ch, 1221, data, sizeof data)) fake->pushed = 1;
}
static int FakeRunning(void* context) { (void)context; return 1; }

typedef struct Broker {
    const wchar_t* suffix;
    DWORD expectedPid;
    HANDLE game;        /* optional: a waitable handle standing for the game's process */
    DWORD delayMs;      /* before the channel exists: a broker that starts late */
    int serve;          /* 0: close right after accepting (a broker that dies) */
    HANDLE ready;
    int created, accepted, ended;
    uint32_t refused;
    Fake fake;
} Broker;

static DWORD WINAPI BrokerThread(void* param)
{
    Broker* b = (Broker*)param;
    if (b->delayMs) Sleep(b->delayMs);
    BrokerChannel* ch = (BrokerChannel*)calloc(1, sizeof *ch);
    b->created = ch && BrokerChannelCreate(ch, b->suffix);
    SetEvent(b->ready);
    if (!b->created) { free(ch); return 0; }
    b->accepted = BrokerChannelAccept(ch, b->expectedPid, b->game, 2000);
    b->refused = ch->refused;
    if (b->accepted == BROKER_ACCEPTED && b->serve)
    {
        BrokerSteam steam = { &b->fake, FakePump, FakeRunning };
        b->ended = BrokerServe(ch, &steam, b->game);
    }
    BrokerChannelClose(ch);
    free(ch);
    return 0;
}

static HANDLE StartBroker(Broker* b)
{
    b->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE thread = CreateThread(NULL, 0, BrokerThread, b, 0, NULL);
    if (!b->delayMs) WaitForSingleObject(b->ready, 5000);
    return thread;
}

static void Join(HANDLE thread, Broker* b)
{
    CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "the broker thread ended");
    CloseHandle(thread);
    CloseHandle(b->ready);
}

/* ---- the channel and the serving loop ---- */

static void TestServe(void)
{
    Broker b = { .suffix = L"test-serve", .expectedPid = GetCurrentProcessId(), .serve = 1 };
    HANDLE thread = StartBroker(&b);
    CHECK(b.created, "serve: channel created");

    BrokerChannel second;
    CHECK(!BrokerChannelCreate(&second, L"test-serve"), "serve: a second pipe of the same name is refused (first instance)");

    GameChannel game;
    CHECK(GameChannelConnect(&game, L"test-serve", 2000), "serve: the game connects and gets the ring");

    uint8_t running = 0;
    uint32_t size = 0, status = 99;
    CHECK(GameChannelCall(&game, BRIDGE_FN_IS_STEAM_RUNNING, NULL, 0, &running, 1, &size, &status) &&
        status == BRIDGE_STATUS_OK && size == 1 && running == 1, "serve: IsSteamRunning answered by the broker");
    CHECK(GameChannelCall(&game, 777, NULL, 0, NULL, 0, &size, &status) && status == BRIDGE_STATUS_UNKNOWN,
        "serve: an unknown function is refused, the channel stays up");

    /* malformed: a call too short to name its function */
    uint8_t reply[64];
    uint32_t replySize = 0, seq = 0, replyStatus = 0;
    uint8_t shortBody[2] = { 9, 9 };
    CHECK(BridgePipeSend(game.pipe, BRIDGE_CALL, shortBody, sizeof shortBody) &&
        BridgePipeRecv(game.pipe, reply, sizeof reply, &replySize) == BRIDGE_RESULT && replySize == 8 &&
        (memcpy(&replyStatus, reply + 4, 4), replyStatus == BRIDGE_STATUS_MALFORMED), "serve: a malformed call is answered as malformed");

    /* oversized: larger than any message, written raw */
    static uint8_t big[BRIDGE_MSG_BUF + 1000];
    uint32_t type = BRIDGE_CALL;
    memcpy(big, &type, 4);
    DWORD written = 0;
    CHECK(WriteFile(game.pipe, big, sizeof big, &written, NULL) && written == sizeof big &&
        BridgePipeRecv(game.pipe, reply, sizeof reply, &replySize) == BRIDGE_RESULT && replySize == 8 &&
        (memcpy(&seq, reply, 4), memcpy(&replyStatus, reply + 4, 4), seq == 0 && replyStatus == BRIDGE_STATUS_MALFORMED),
        "serve: an oversized message is read to its end and answered as malformed");

    running = 0;
    CHECK(GameChannelCall(&game, BRIDGE_FN_IS_STEAM_RUNNING, NULL, 0, &running, 1, &size, &status) && running == 1,
        "serve: calls still work after both");

    /* the round trips' cost: the three calls made through GameChannelCall, by function, then a fresh count */
    GameChannelCost cost;
    GameChannelTakeCost(&game, &cost);
    CHECK(cost.calls == 3 && cost.ticks > 0 && cost.maxTicks > 0 && cost.maxTicks <= cost.ticks &&
        cost.byFunction[0].function == BRIDGE_FN_IS_STEAM_RUNNING && cost.byFunction[0].calls == 2 &&
        cost.byFunction[1].function == 777 && cost.byFunction[1].calls == 1 && cost.byFunction[2].calls == 0,
        "serve: the round trips are counted with their time, by function");
    GameChannelTakeCost(&game, &cost);
    CHECK(cost.calls == 0 && cost.ticks == 0 && cost.byFunction[0].calls == 0, "serve: a take starts a fresh count");

    /* the pump runs between calls: its callback reaches the ring */
    DWORD start = GetTickCount();
    while (!GameChannelPending(&game) && GetTickCount() - start < 2000) Sleep(5);
    uint32_t id = 0;
    uint8_t data[BRIDGE_RING_DATA_SIZE];
    CHECK(GameChannelReadCallback(&game, &id, data, sizeof data, &size) && id == 1221 && size == 3 && data[2] == 3,
        "serve: the pump's callback reaches the game through the ring");
    CHECK(b.fake.pumps > 0, "serve: the broker pumps Steam on its own thread");

    GameChannelClose(&game);
    Join(thread, &b);
    CHECK(b.ended == BROKER_END_SHUTDOWN, "serve: closing says shutdown and the loop ends");
}

static void TestSecurity(void)
{
    BrokerChannel* ch = (BrokerChannel*)calloc(1, sizeof *ch);
    if (!ch || !BrokerChannelCreate(ch, L"test-security")) { CHECK(0, "security: channel created"); free(ch); return; }
    PSECURITY_DESCRIPTOR sd = NULL;
    wchar_t* text = NULL;
    DWORD got = GetSecurityInfo(ch->pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL, &sd);
    if (got == ERROR_SUCCESS) ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, &text, NULL);
    CHECK(text && wcsstr(text, L"ML;;NW;;;LW"), "security: the pipe carries a low no-write-up label");
    CHECK(text && !wcsstr(text, L";;;WD)"), "security: Everyone is not granted");
    CHECK(text && wcsstr(text, L"S-1-5-21") , "security: the player's own SID is granted");
    if (text) LocalFree(text);
    if (sd) LocalFree(sd);
    BrokerChannelClose(ch);
    free(ch);
}

static void TestWrongClient(const wchar_t* self)
{
    Broker b = { .suffix = L"test-wrong", .expectedPid = GetCurrentProcessId(), .serve = 1 };
    HANDLE thread = StartBroker(&b);

    /* another process: same user, wrong pid */
    wchar_t command[MAX_PATH + 64];
    swprintf(command, MAX_PATH + 64, L"\"%ls\" client test-wrong", self);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    if (CreateProcessW(self, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 10000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CHECK(code == 3, "wrong client: another process is refused");

    /* this process, lying about its pid */
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 100 && pipe == INVALID_HANDLE_VALUE; i++)
    {
        pipe = CreateFileW(L"\\\\.\\pipe\\hellforge-broker-test-wrong", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) Sleep(20);
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    uint32_t lie = GetCurrentProcessId() + 4;
    uint8_t body[16];
    uint32_t bodySize = 0;
    CHECK(pipe != INVALID_HANDLE_VALUE && SetNamedPipeHandleState(pipe, &mode, NULL, NULL) &&
        BridgePipeSend(pipe, BRIDGE_HELLO, &lie, sizeof lie) && BridgePipeRecv(pipe, body, sizeof body, &bodySize) == BRIDGE_HELLO_DENIED,
        "wrong client: a HELLO naming another pid is refused");
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);

    /* the game itself still gets in */
    GameChannel game;
    CHECK(GameChannelConnect(&game, L"test-wrong", 3000), "wrong client: the right one connects afterwards");
    GameChannelClose(&game);
    Join(thread, &b);
    CHECK(b.accepted == BROKER_ACCEPTED && b.refused == 2, "wrong client: the broker refused two and kept waiting");
}

static void TestLateBroker(void)
{
    Broker b = { .suffix = L"test-late", .expectedPid = GetCurrentProcessId(), .serve = 1, .delayMs = 400 };
    HANDLE thread = StartBroker(&b);
    DWORD start = GetTickCount();
    GameChannel game;
    int ok = GameChannelConnect(&game, L"test-late", 5000);
    CHECK(ok && GetTickCount() - start >= 300, "late broker: the game waits for its pipe, then connects");
    GameChannelClose(&game);
    Join(thread, &b);

    start = GetTickCount();
    CHECK(!GameChannelConnect(&game, L"test-nobody", 300) && game.dead && GetTickCount() - start < 2000,
        "no broker: the connect gives up after its timeout");
    GameChannelClose(&game);
}

static void TestDeadBroker(void)
{
    Broker b = { .suffix = L"test-dead", .expectedPid = GetCurrentProcessId(), .serve = 0 };
    HANDLE thread = StartBroker(&b);
    GameChannel game;
    CHECK(GameChannelConnect(&game, L"test-dead", 2000), "dead broker: connected");
    Join(thread, &b);
    uint8_t running = 1;
    uint32_t size = 0, status = 0;
    CHECK(!GameChannelCall(&game, BRIDGE_FN_IS_STEAM_RUNNING, NULL, 0, &running, 1, &size, &status) && game.dead,
        "dead broker: the call fails and the channel is marked dead");
    CHECK(!GameChannelCall(&game, BRIDGE_FN_IS_STEAM_RUNNING, NULL, 0, &running, 1, &size, &status), "dead broker: later calls fail at once");
    GameChannelClose(&game);
}

static void TestGameGone(void)
{
    HANDLE game = CreateEventW(NULL, TRUE, FALSE, NULL);
    Broker b = { .suffix = L"test-gone", .expectedPid = GetCurrentProcessId(), .serve = 1, .game = game };
    HANDLE thread = StartBroker(&b);
    Sleep(100);
    SetEvent(game);
    Join(thread, &b);
    CHECK(b.accepted == BROKER_GAME_GONE, "game gone: the broker stops waiting when the game exits before connecting");

    ResetEvent(game);
    Broker s = { .suffix = L"test-gone2", .expectedPid = GetCurrentProcessId(), .serve = 1, .game = game };
    thread = StartBroker(&s);
    GameChannel client;
    CHECK(GameChannelConnect(&client, L"test-gone2", 2000), "game gone: connected");
    SetEvent(game);
    Join(thread, &s);
    CHECK(s.ended == BROKER_END_GAME_GONE, "game gone: the serving loop ends when the game exits");
    GameChannelClose(&client);
    CloseHandle(game);
}

/* ---- the stand-in itself in bridge mode (with a fake Valve DLL next to it, which must never load) ---- */

typedef bool (__cdecl *InitSafeFn)(void);
typedef int (__cdecl *IntFn)(void);
typedef void (__cdecl *VoidFn)(void);

static void TestStandIn(const wchar_t* standIn)
{
    Broker b = { .suffix = L"test-standin", .expectedPid = GetCurrentProcessId(), .serve = 1 };
    HANDLE thread = StartBroker(&b);
    SetEnvironmentVariableW(L"HELLFORGE_BRIDGE_PIPE", L"test-standin");
    HMODULE dll = LoadLibraryW(standIn);
    CHECK(dll != NULL, "stand-in: loads with HELLFORGE_BRIDGE_PIPE set (no connect under the loader lock)");
    if (!dll) { Join(thread, &b); return; }
    InitSafeFn initSafe = (InitSafeFn)(void*)GetProcAddress(dll, "SteamAPI_InitSafe");
    IntFn running = (IntFn)(void*)GetProcAddress(dll, "SteamAPI_IsSteamRunning");
    IntFn pipe = (IntFn)(void*)GetProcAddress(dll, "SteamAPI_GetHSteamPipe");
    VoidFn run = (VoidFn)(void*)GetProcAddress(dll, "SteamAPI_RunCallbacks");
    VoidFn shutdown = (VoidFn)(void*)GetProcAddress(dll, "SteamAPI_Shutdown");
    if (!initSafe || !running || !pipe || !run || !shutdown) { CHECK(0, "stand-in: the bridged exports are there"); Join(thread, &b); return; }

    CHECK(initSafe() && pipe() == 1, "stand-in: Init connects to the broker");
    CHECK(running() == 1, "stand-in: IsSteamRunning is the broker's answer");
    DWORD start = GetTickCount();
    while (!b.fake.pushed && GetTickCount() - start < 2000) Sleep(5);
    run();
    run();
    CHECK(GetModuleHandleW(L"steam_api64_hellforge.dll") == NULL, "stand-in: Valve's DLL is never loaded in bridge mode");
    shutdown();
    Join(thread, &b);
    CHECK(b.ended == BROKER_END_SHUTDOWN, "stand-in: Shutdown ends the broker's loop");

    /* no broker now: Init fails closed, and nothing falls back to Valve's DLL */
    start = GetTickCount();
    CHECK(!initSafe() && running() == 0 && pipe() == 0, "stand-in: without a broker, Init fails and Steam reads as not running");
    CHECK(GetTickCount() - start < 15000, "stand-in: the wait for a broker is bounded");
    CHECK(GetModuleHandleW(L"steam_api64_hellforge.dll") == NULL, "stand-in: still no Valve DLL after a failed Init");
    SetEnvironmentVariableW(L"HELLFORGE_BRIDGE_PIPE", NULL);
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc == 3 && !wcscmp(argv[1], L"client"))
    {
        GameChannel game;
        int ok = GameChannelConnect(&game, argv[2], 5000);
        GameChannelClose(&game);
        return ok ? 0 : 3;
    }
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    TestRing();
    TestSecurity();
    TestServe();
    TestWrongClient(self);
    TestLateBroker();
    TestDeadBroker();
    TestGameGone();
    if (argc == 2) TestStandIn(argv[1]);
    else puts("  (no stand-in given: its bridge-mode test skipped)");
    printf("%s broker: %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
