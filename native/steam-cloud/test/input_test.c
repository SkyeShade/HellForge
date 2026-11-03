// What the broker sees of Steam Input (broker_input.c), with fake Steam Input, XInput and overlay: when it asks, the
// controllers line the session reads (SteamInputNote.Parse in HellForge.Core, whose test parses these same lines), the
// emulated pads' activity, and what it does when Valve's DLL lacks Steam Input or it does not start. Needs nothing
// generated from the Steamworks SDK, so Build-Engine.ps1 runs it in every build.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "broker_input.h"

static int g_failures;
#define EXPECT(cond, what) do { if (cond) printf("  ok    %s\n", what); else { printf("  FAIL  %s\n", what); g_failures++; } } while (0)

// ---- the lines the module says ----
static char g_lines[32][BROKER_INPUT_LINE];
static int g_lineCount;
static void Capture(const char* line)
{
    if (g_lineCount < 32) snprintf(g_lines[g_lineCount], BROKER_INPUT_LINE, "%s", line);
    g_lineCount++;
}
static const char* Last(void) { return g_lineCount > 0 && g_lineCount <= 32 ? g_lines[g_lineCount - 1] : ""; }
static int Said(int from, const char* line)
{
    for (int i = from; i < g_lineCount && i < 32; i++) if (strcmp(g_lines[i], line) == 0) return 1;
    return 0;
}

// ---- fake Steam Input: the controllers Steam would list, by handle ----
static int g_object, g_inits, g_explicit, g_frames, g_initResult = 1, g_overlay;
static int32_t g_count;
static int32_t g_types[24], g_pads[24];
static uint32_t g_packets[BROKER_INPUT_PADS];
static int g_shown[BROKER_INPUT_PADS], g_xinputReads;

static void* FakeAccessor(void) { return &g_object; }
static bool FakeInit(void* self, bool explicitly) { g_inits++; g_explicit = explicitly; return self == &g_object && g_initResult; }
static void FakeRunFrame(void* self, bool reserved) { g_frames++; }
static int32_t FakeConnected(void* self, uint64_t* handles)
{
    for (int32_t i = 0; i < g_count && i < BROKER_INPUT_MAX; i++) handles[i] = 100 + (uint64_t)i; // Steam writes at most 16
    return g_count;
}
static int32_t Index(uint64_t handle) { return handle >= 100 && handle < 124 ? (int32_t)(handle - 100) : 0; }
static int32_t FakeType(void* self, uint64_t handle) { return g_types[Index(handle)]; }
static int32_t FakePad(void* self, uint64_t handle) { return g_pads[Index(handle)]; }
static DWORD WINAPI FakeXInput(DWORD pad, void* state)
{
    g_xinputReads++;
    if (pad >= BROKER_INPUT_PADS || !g_shown[pad]) return ERROR_DEVICE_NOT_CONNECTED;
    memset(state, 0, 16);
    memcpy(state, &g_packets[pad], 4);
    return ERROR_SUCCESS;
}
static bool FakeOverlay(void) { return g_overlay != 0; }

static void Fake(BrokerInput* in)
{
    BrokerInputOpen(in, NULL, Capture);
    in->api = (BrokerInputApi){ FakeAccessor, FakeInit, FakeRunFrame, FakeConnected, FakeType, FakePad, FakeXInput, FakeOverlay };
}

static void Controller(int i, int32_t type, int32_t pad) { g_types[i] = type; g_pads[i] = pad; }

static void Unavailable(void)
{
    static BrokerInput in;
    BrokerInputOpen(&in, GetModuleHandleW(L"kernel32.dll"), Capture);
    EXPECT(!in.api.accessor && !in.api.init && in.api.overlayLoaded, "a DLL without Steam Input's exports: none found; the overlay check is this process's");
    int before = g_lineCount;
    BrokerInputPoll(&in, 1000);
    BrokerInputPoll(&in, 9000);
    EXPECT(g_lineCount == before + 1 && strcmp(Last(), "broker: Steam Input is not asked: Valve's DLL lacks its functions") == 0,
        "without them Steam Input is not asked, said once");

    Fake(&in);
    g_initResult = 0;
    before = g_lineCount;
    int frames = g_frames;
    BrokerInputPoll(&in, 1000);
    BrokerInputPoll(&in, 9000);
    EXPECT(g_inits == 1 && g_frames == frames && g_lineCount == before + 1 &&
        strcmp(Last(), "broker: Steam Input did not start, so which controllers it handles is not known") == 0,
        "an Init that fails is said once, and Steam Input is not asked again");
    g_initResult = 1;
    g_inits = 0;
}

static void Controllers(void)
{
    static BrokerInput in;
    Fake(&in);
    g_overlay = 1;
    int start = g_lineCount;
    BrokerInputPoll(&in, 1000);
    EXPECT(g_inits == 1 && g_explicit && g_frames == 1, "the first poll calls Init once, with RunFrame called by the broker, then RunFrame");
    EXPECT(Said(start, "broker: Steam's overlay is in the broker, and with it Steam Input's controller emulation: the sandboxed game has neither"),
        "where Steam's overlay is, is said");
    EXPECT(strcmp(Last(), "broker: Steam Input controllers: none") == 0, "no controller: \"none\", said at the first poll");
    BrokerInputPoll(&in, 2999);
    EXPECT(g_frames == 1, "Steam is asked at most every 2 s");

    g_count = 1;
    Controller(0, 1, 0);
    g_shown[0] = 1;
    g_packets[0] = 10;
    BrokerInputPoll(&in, 3000);
    EXPECT(g_frames == 2 && strcmp(Last(), "broker: Steam Input controllers: Steam Controller [1] as XInput pad 0") == 0,
        "a Steam Controller Steam emulates as XInput pad 0");
    int lines = g_lineCount;
    g_packets[0] = 25;
    BrokerInputPoll(&in, 5000);
    EXPECT(g_lineCount == lines, "the same list is not said again");
    BrokerInputPoll(&in, 61000);
    EXPECT(strcmp(Last(), "broker: XInput pad 0 in the broker changed 15 times in the last minute: Steam Input's emulated pad reaches the broker, not the game") == 0,
        "the emulated pad's changes are counted from its packet numbers and said once a minute");
    lines = g_lineCount;
    BrokerInputPoll(&in, 121000);
    EXPECT(g_lineCount == lines, "a minute without changes says nothing");

    g_count = 3;
    Controller(1, 13, -1);
    Controller(2, 14, 1);
    lines = g_lineCount;
    BrokerInputPoll(&in, 123000);
    EXPECT(Said(lines, "broker: Steam Input controllers: Steam Controller [1] as XInput pad 0, PS5 controller [13], Steam Deck controls [14] as XInput pad 1"),
        "several controllers, one without an emulated pad, on one line");
    EXPECT(Said(lines, "broker: Steam Input emulates XInput pad 1, which XInput in the broker does not show"), "an emulated pad XInput does not show is said");
    lines = g_lineCount;
    BrokerInputPoll(&in, 125000);
    EXPECT(g_lineCount == lines, "and said once");

    g_count = 0;
    BrokerInputPoll(&in, 127000);
    EXPECT(strcmp(Last(), "broker: Steam Input controllers: none") == 0, "the controllers gone: \"none\" again");
    int reads = g_xinputReads;
    BrokerInputPoll(&in, 129000);
    EXPECT(g_xinputReads == reads, "without an emulated pad XInput is not read");
}

static void Bounds(void)
{
    static BrokerInput in;
    Fake(&in);
    g_overlay = 0;
    g_count = 24;
    for (int i = 0; i < 24; i++) Controller(i, 99, -1);
    int start = g_lineCount;
    BrokerInputPoll(&in, 1000);
    EXPECT(Said(start, "broker: Steam's overlay is not in the broker"), "an overlay that is not in the broker is said so");
    int listed = 0;
    for (const char* at = Last(); (at = strstr(at, "controller [99]")) != NULL; at++) listed++;
    EXPECT(listed == BROKER_INPUT_MAX, "more than Steam's 16 controllers: 16 listed; an unknown type is \"controller [n]\"");
    g_count = -1;
    BrokerInputPoll(&in, 3000);
    EXPECT(strcmp(Last(), "broker: Steam Input controllers: none") == 0, "a negative count is none");
    EXPECT(strcmp(BrokerInputTypeName(-1), "controller") == 0 && strcmp(BrokerInputTypeName(0), "unknown controller") == 0 &&
        strcmp(BrokerInputTypeName(14), "Steam Deck controls") == 0 && strcmp(BrokerInputTypeName(15), "controller") == 0, "type names, in and out of range");
}

int main(void)
{
    printf("Steam Input in the broker\n");
    Unavailable();
    Controllers();
    Bounds();
    if (g_failures) printf("%d check(s) failed\n", g_failures);
    else printf("all checks passed\n");
    return g_failures ? 1 : 0;
}
