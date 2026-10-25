/* HellForge's Steam broker (docs/steam-bridge.md, phase 2): a process at normal integrity that loads Valve's
 * steam_api64.dll as app 553850, accepts the session's game over a named pipe with a low mandatory label, answers the
 * calls the allow-list permits, and pushes callbacks into a shared-memory ring.
 *
 * Usage: hellforge_steam_broker.exe <pipe-suffix> <game-pid> [valve-dll-path] [mods-folder] [p2p-peers-file]
 *
 * The session runner starts one broker per session; the game's stand-in finds the pipe by HELLFORGE_BRIDGE_PIPE. The
 * broker serves only the process with <game-pid>, and exits when that process exits, when it says BRIDGE_SHUTDOWN, or
 * when its pipe breaks. Its log is its standard error. The mods folder holds Play with strangers (hellforge-mods.json),
 * which decides whether the game's Public Steam lobby stays Public (broker_bridge.c). The peers file is the backend's
 * list of the game's co-op peers, the only Steam users P2P passes to and from.
 *
 * The channel and the client check are phase 2; the interface methods, the allow-list's callbacks, call results and the
 * snapshot (broker_bridge.c, with the dispatch SdkShimGen steam-bridge generates) are phase 3. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "broker_bridge.h"

#define GAME_APP_ID "553850"

/* --- Valve's DLL (loaded at runtime) --- */

/* The pinned build's SDK exports SteamAPI_Init (bool). Newer SDKs have SteamAPI_InitFlat instead, which returns an
 * ESteamAPIInitResult (0: OK) with a reason in a 1024-character SteamErrMsg: used only when SteamAPI_Init is missing. */
typedef bool (__cdecl *PFN_SteamAPI_Init)(void);
typedef int  (__cdecl *PFN_SteamAPI_InitFlat)(char* error);
typedef void (__cdecl *PFN_SteamAPI_Shutdown)(void);
typedef void (__cdecl *PFN_SteamAPI_ManualDispatch_Init)(void);
typedef void (__cdecl *PFN_SteamAPI_ManualDispatch_RunFrame)(int hPipe);
typedef int  (__cdecl *PFN_SteamAPI_ManualDispatch_GetNextCallback)(int hPipe, void* msg);
typedef void (__cdecl *PFN_SteamAPI_ManualDispatch_FreeLastCallback)(int hPipe);
typedef int  (__cdecl *PFN_SteamAPI_GetHSteamPipe)(void);
typedef int  (__cdecl *PFN_SteamAPI_IsSteamRunning)(void);

static struct {
    HMODULE module;
    PFN_SteamAPI_Init                            Init;
    PFN_SteamAPI_InitFlat                        InitFlat;
    PFN_SteamAPI_Shutdown                        Shutdown;
    PFN_SteamAPI_ManualDispatch_Init             ManualDispatch_Init;
    PFN_SteamAPI_ManualDispatch_RunFrame         ManualDispatch_RunFrame;
    PFN_SteamAPI_ManualDispatch_GetNextCallback  ManualDispatch_GetNextCallback;
    PFN_SteamAPI_ManualDispatch_FreeLastCallback ManualDispatch_FreeLastCallback;
    PFN_SteamAPI_GetHSteamPipe                   GetHSteamPipe;
    PFN_SteamAPI_IsSteamRunning                  IsSteamRunning;
    int hPipe;
} g_steam;

static int LoadValveDll(const wchar_t* path)
{
    g_steam.module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_steam.module) return 0;
#define LOAD(name) g_steam.name = (PFN_SteamAPI_##name)(void*)GetProcAddress(g_steam.module, "SteamAPI_" #name); if (!g_steam.name) return 0
    g_steam.Init = (PFN_SteamAPI_Init)(void*)GetProcAddress(g_steam.module, "SteamAPI_Init");
    g_steam.InitFlat = (PFN_SteamAPI_InitFlat)(void*)GetProcAddress(g_steam.module, "SteamAPI_InitFlat");
    if (!g_steam.Init && !g_steam.InitFlat) return 0;
    LOAD(Shutdown);
    LOAD(ManualDispatch_Init);
    LOAD(ManualDispatch_RunFrame);
    LOAD(ManualDispatch_GetNextCallback);
    LOAD(ManualDispatch_FreeLastCallback);
    LOAD(GetHSteamPipe);
    LOAD(IsSteamRunning);
#undef LOAD
    return 1;
}

/* SteamAPI_RestartAppIfNecessary is never called: it may start the installed game through Steam. The app id comes from
 * the environment instead, which Steam's init reads when there is no steam_appid.txt. */
static int InitSteam(void)
{
    if (!SetEnvironmentVariableA("SteamAppId", GAME_APP_ID) || !SetEnvironmentVariableA("SteamGameId", GAME_APP_ID)) return 0;
    if (g_steam.Init)
    {
        if (!g_steam.Init()) { fprintf(stderr, "broker: SteamAPI_Init -> false\n"); return 0; }
    }
    else
    {
        char error[1024] = "";
        int result = g_steam.InitFlat(error);
        error[sizeof error - 1] = 0;
        if (result != 0) { fprintf(stderr, "broker: SteamAPI_InitFlat -> %d (%s)\n", result, error); return 0; }
    }
    g_steam.ManualDispatch_Init();
    g_steam.hPipe = g_steam.GetHSteamPipe();
    return g_steam.hPipe != 0;
}

/* Several installations on one PC (HELLFORGE_MULTI_INSTANCE=1, a test set-up; the session runner's environment): the
 * game is HELLFORGE_IDENTITY (named HELLFORGE_PERSONA, else "Diver <last digits>"), and its own Steam lobby is kept with
 * the other instances' in <HELLFORGE_DIRECTORY_FOLDER>\steam-lobbies. */
static void MultiInstance(BrokerBridge* bridge)
{
    wchar_t multi[8], identity[32], folder[520], persona[64];
    if (GetEnvironmentVariableW(L"HELLFORGE_MULTI_INSTANCE", multi, 8) != 1 || multi[0] != L'1') return;
    DWORD folderLength = GetEnvironmentVariableW(L"HELLFORGE_DIRECTORY_FOLDER", folder, 480);
    if (!GetEnvironmentVariableW(L"HELLFORGE_IDENTITY", identity, 32) || folderLength == 0 || folderLength >= 480)
    {
        fprintf(stderr, "broker: several installations need HELLFORGE_IDENTITY and HELLFORGE_DIRECTORY_FOLDER; the account's own Steam lobby is used\n");
        return;
    }
    wcscat(folder, L"\\steam-lobbies");
    uint64_t id = wcstoull(identity, NULL, 10);
    char name[128];
    if (GetEnvironmentVariableW(L"HELLFORGE_PERSONA", persona, 64) > 0 && WideCharToMultiByte(CP_UTF8, 0, persona, -1, name, sizeof name, NULL, NULL) > 0) { }
    else snprintf(name, sizeof name, "Diver %04llu", (unsigned long long)(id % 10000));
    if (!BrokerBridgeMultiInstance(bridge, folder, id, name)) fprintf(stderr, "broker: several installations: not set up (identity or folder)\n");
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 3 || wcslen(argv[1]) == 0 || wcslen(argv[1]) > 64)
    {
        fprintf(stderr, "Usage: hellforge_steam_broker <pipe-suffix> <game-pid> [valve-dll-path] [mods-folder] [p2p-peers-file]\n");
        return 2;
    }
    wchar_t* end = NULL;
    DWORD gamePid = (DWORD)wcstoul(argv[2], &end, 10);
    if (gamePid == 0 || end == argv[2] || *end != L'\0') { fprintf(stderr, "broker: bad game pid\n"); return 2; }

    /* the broker lives only as long as the game: watched while it waits and while it serves */
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, gamePid);
    if (!game) { fprintf(stderr, "broker: game process %lu not found (%lu)\n", gamePid, GetLastError()); return 1; }

    /* the channel before Steam, so the game's wait starts as soon as possible */
    BrokerChannel* ch = (BrokerChannel*)calloc(1, sizeof *ch);
    if (!ch || !BrokerChannelCreate(ch, argv[1]))
    {
        fprintf(stderr, "broker: cannot create the channel (%lu)\n", GetLastError());
        CloseHandle(game);
        return 1;
    }

    if (argc >= 4 && argv[3][0] != L'\0')
    {
        if (!LoadValveDll(argv[3])) { fprintf(stderr, "broker: cannot load Valve's steam_api64.dll (%lu)\n", GetLastError()); goto fail; }
        if (!InitSteam()) { fprintf(stderr, "broker: Steam did not start\n"); goto fail; }
        fprintf(stderr, "broker: Steam ready (pipe %d)\n", g_steam.hPipe);
    }
    static BrokerBridge bridge;
    BrokerSteam steam = { 0 };
    if (g_steam.module)
    {
        if (!BrokerBridgeOpen(&bridge, g_steam.module, g_steam.hPipe)) { fprintf(stderr, "broker: Valve's DLL lacks manual dispatch\n"); goto fail; }
        BrokerBridgeSnapshot(&bridge, ch); /* ready before the game's first frame */
        BrokerBridgeWatchStrangers(&bridge, argc >= 5 && argv[4][0] != L'\0' ? argv[4] : NULL);
        BrokerBridgeWatchPeers(&bridge, argc >= 6 && argv[5][0] != L'\0' ? argv[5] : NULL);
        MultiInstance(&bridge);
        BrokerBridgeSnapshot(&bridge, ch); /* with this instance's identity, when it has one */
        steam = bridge.steam;
    }

    fprintf(stderr, "broker: waiting for the game (%lu)\n", gamePid);
    int accepted = BrokerChannelAccept(ch, gamePid, game, 5000);
    if (ch->refused) fprintf(stderr, "broker: refused %u other client(s)\n", ch->refused);
    if (accepted != BROKER_ACCEPTED)
    {
        fprintf(stderr, accepted == BROKER_GAME_GONE ? "broker: the game exited before connecting\n" : "broker: accept failed (%lu)\n", GetLastError());
        goto fail;
    }
    fprintf(stderr, "broker: game connected\n");

    int why = BrokerServe(ch, &steam, game);
    fprintf(stderr, "broker: %s; %u callback(s) dropped, %u call(s) refused\n",
        why == BROKER_END_SHUTDOWN ? "shutdown" : why == BROKER_END_GAME_GONE ? "the game exited" : "the pipe closed", ch->dropped, bridge.refusals);
    BrokerLobbiesClose(bridge.localLobbies); /* the game is gone: it leaves the local lobbies it was in */

    BrokerChannelClose(ch);
    free(ch);
    CloseHandle(game);
    if (g_steam.module) g_steam.Shutdown();
    return 0;
fail:
    BrokerChannelClose(ch);
    free(ch);
    CloseHandle(game);
    if (g_steam.hPipe) g_steam.Shutdown();
    return 1;
}
