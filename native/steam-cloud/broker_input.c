/* Steam Input as the broker sees it (broker_input.h). Every Steam call here runs on the broker's one Steam thread, from
 * its pump. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "broker_input.h"

/* ESteamInputType (isteaminput.h, SDK 1.55), in order */
static const char* const g_typeNames[] = {
    "unknown controller", "Steam Controller", "Xbox 360 controller", "Xbox One controller", "generic gamepad",
    "PS4 controller", "Apple MFi controller", "Android controller", "Joy-Con pair", "Joy-Con", "Switch Pro controller",
    "Steam Link touch controller", "PS3 controller", "PS5 controller", "Steam Deck controls",
};

const char* BrokerInputTypeName(int32_t type)
{
    return type >= 0 && type < (int32_t)(sizeof g_typeNames / sizeof g_typeNames[0]) ? g_typeNames[type] : "controller";
}

static void ToStandardError(const char* line) { fprintf(stderr, "%s\n", line); }

static void Say(BrokerInput* in, const char* format, ...)
{
    char line[BROKER_INPUT_LINE];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    in->say(line);
}

/* Steam's overlay, which carries Steam Input's controller emulation into the process it is loaded in */
static bool OverlayLoaded(void) { return GetModuleHandleW(L"GameOverlayRenderer64.dll") != NULL; }

void BrokerInputOpen(BrokerInput* in, HMODULE valve, void (*say)(const char* line))
{
    memset(in, 0, sizeof *in);
    in->say = say ? say : ToStandardError;
    in->api.overlayLoaded = OverlayLoaded;
    if (!valve) return;
#define FIND(field, name) in->api.field = (__typeof__(in->api.field))(void*)GetProcAddress(valve, name)
    FIND(accessor, "SteamAPI_SteamInput_v006");
    FIND(init, "SteamAPI_ISteamInput_Init");
    FIND(runFrame, "SteamAPI_ISteamInput_RunFrame");
    FIND(connected, "SteamAPI_ISteamInput_GetConnectedControllers");
    FIND(type, "SteamAPI_ISteamInput_GetInputTypeForHandle");
    FIND(pad, "SteamAPI_ISteamInput_GetGamepadIndexForController");
#undef FIND
}

/* Init with RunFrame called here (the broker runs manual dispatch, never SteamAPI_RunCallbacks), and the overlay's
 * place: 1 when Steam Input answers, else -1, logged. */
static int Start(BrokerInput* in)
{
    BrokerInputApi* a = &in->api;
    if (!a->accessor || !a->init || !a->runFrame || !a->connected || !a->type)
    {
        Say(in, "broker: Steam Input is not asked: Valve's DLL lacks its functions");
        return -1;
    }
    in->input = a->accessor();
    if (!in->input || !a->init(in->input, true))
    {
        in->input = NULL;
        Say(in, "broker: Steam Input did not start, so which controllers it handles is not known");
        return -1;
    }
    Say(in, a->overlayLoaded && a->overlayLoaded()
        ? "broker: Steam's overlay is in the broker, and with it Steam Input's controller emulation: the sandboxed game has neither"
        : "broker: Steam's overlay is not in the broker");
    return 1;
}

/* The controllers Steam Input handles now: their types and emulated XInput pads (-1: none), at most BROKER_INPUT_MAX. */
static int ReadList(BrokerInput* in, int32_t* types, int32_t* pads)
{
    uint64_t handles[BROKER_INPUT_MAX];
    memset(handles, 0, sizeof handles);
    in->api.runFrame(in->input, true);
    int32_t n = in->api.connected(in->input, handles);
    if (n < 0) n = 0;
    if (n > BROKER_INPUT_MAX) n = BROKER_INPUT_MAX;
    for (int32_t i = 0; i < n; i++)
    {
        types[i] = in->api.type(in->input, handles[i]);
        pads[i] = in->api.pad ? in->api.pad(in->input, handles[i]) : -1;
    }
    return (int)n;
}

/* The line the session reads (SteamInputNote.Parse): "none", or each controller as "<name> [<type>]", with " as XInput
 * pad <n>" when Steam emulates one for it, comma separated. */
static void LogList(BrokerInput* in)
{
    char list[BROKER_INPUT_LINE - 64];
    size_t at = (size_t)snprintf(list, sizeof list, "%s", in->count ? "" : "none");
    for (int i = 0; i < in->count && at < sizeof list; i++)
    {
        at += (size_t)snprintf(list + at, sizeof list - at, "%s%s [%d]", i ? ", " : "", BrokerInputTypeName(in->types[i]), (int)in->types[i]);
        if (in->pads[i] >= 0 && at < sizeof list) at += (size_t)snprintf(list + at, sizeof list - at, " as XInput pad %d", (int)in->pads[i]);
    }
    Say(in, "broker: Steam Input controllers: %s", list);
}

/* Keeps and logs the list when it is not the one logged last. */
static void Remember(BrokerInput* in, int n, const int32_t* types, const int32_t* pads)
{
    size_t size = sizeof types[0] * (size_t)n;
    if (in->listed && n == in->count && memcmp(types, in->types, size) == 0 && memcmp(pads, in->pads, size) == 0) return;
    in->count = n;
    memcpy(in->types, types, size);
    memcpy(in->pads, pads, size);
    in->listed = 1;
    LogList(in);
}

/* XInput in this process, loaded once Steam emulates a pad (the system's xinput1_4.dll); 0 when it cannot be. */
static int HaveXInput(BrokerInput* in)
{
    if (in->api.xinputState) return 1;
    if (in->xinputTried) return 0;
    in->xinputTried = 1;
    HMODULE xinput = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (xinput) in->api.xinputState = (__typeof__(in->api.xinputState))(void*)GetProcAddress(xinput, "XInputGetState");
    if (!in->api.xinputState) Say(in, "broker: XInput cannot be loaded in the broker, so Steam Input's emulated pads are not read");
    return in->api.xinputState != NULL;
}

/* One emulated pad: its packet number counts the state changes since it was read last. */
static void ReadPad(BrokerInput* in, int pad)
{
    uint8_t state[16];
    memset(state, 0, sizeof state);
    if (in->api.xinputState((DWORD)pad, state) != ERROR_SUCCESS)
    {
        if (in->read[pad] != 0) return;
        Say(in, "broker: Steam Input emulates XInput pad %d, which XInput in the broker does not show", pad);
        in->read[pad] = -1;
        return;
    }
    uint32_t packet;
    memcpy(&packet, state, sizeof packet);
    if (in->read[pad] == 1) in->changes[pad] += packet - in->packets[pad];
    in->packets[pad] = packet;
    in->read[pad] = 1;
}

static void ReadPads(BrokerInput* in)
{
    for (int i = 0; i < in->count; i++)
    {
        int pad = (int)in->pads[i];
        if (pad < 0 || pad >= BROKER_INPUT_PADS) continue;
        if (!HaveXInput(in)) return;
        ReadPad(in, pad);
    }
}

/* Every BROKER_INPUT_REPORT_MS: each emulated pad that changed. A pad that changes while the player plays the game is
 * Steam Input's controller reaching the broker, where nothing reads it. */
static void Report(BrokerInput* in, DWORD now)
{
    if (now - in->lastReport < BROKER_INPUT_REPORT_MS) return;
    in->lastReport = now;
    for (int pad = 0; pad < BROKER_INPUT_PADS; pad++)
    {
        if (in->changes[pad])
            Say(in, "broker: XInput pad %d in the broker changed %u times in the last minute: Steam Input's emulated pad reaches the broker, not the game",
                pad, (unsigned)in->changes[pad]);
        in->changes[pad] = 0;
    }
}

void BrokerInputPoll(BrokerInput* in, DWORD now)
{
    if (in->state < 0 || (in->state > 0 && now - in->lastPoll < BROKER_INPUT_MS)) return;
    if (in->state == 0)
    {
        in->state = Start(in);
        in->lastReport = now;
        if (in->state < 0) return;
    }
    in->lastPoll = now;
    int32_t types[BROKER_INPUT_MAX], pads[BROKER_INPUT_MAX];
    int n = ReadList(in, types, pads);
    Remember(in, n, types, pads);
    ReadPads(in);
    Report(in, now);
}
