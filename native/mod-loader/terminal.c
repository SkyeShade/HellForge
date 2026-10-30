/* Build 25480438: the ship's information terminal (across from the Armory) becomes the lobby browser. The terminal is
 * menu presenter 6 (InformationTerminal, name table rva 0x2065380), which needs Arrowhead's news, Major Order and
 * statistics services and, offline, closes at once into an error popup. The game also keeps a debug lobby browser,
 * presenter 41 (DebugServerBrowser, the table's second part at 0x2065448, index + 25): a list it fills from its own
 * lobby search (PFMultiplayerFindLobbies, empty filter, every ~4 s), which HellForge's lobby directory answers
 * (docs/multiplayer.md "The lobby browser"). The presenter manager's open entry (0x14C0350, (manager, presenter, arg),
 * the one Ship Station Hotkeys calls) is asked for 6, and this opens 41 instead. HELLFORGE_TERMINAL=news keeps the
 * terminal as it is. Each presenter asked for is noted once, with its caller, for the record.
 * Included after loadout_preview.c for PinMatches and LoaderHook. Nothing is written into the game. */
#define TERMINAL_OPEN_PRESENTER 0x14C0550u
#define TERMINAL_INFORMATION 6
#define TERMINAL_SERVER_BROWSER 41
#define TERMINAL_NOTED 64
typedef uintptr_t (*TerminalOpenFn)(uintptr_t manager, int32_t presenter, void* arg);
static TerminalOpenFn g_terminalOpenOriginal;
static int g_terminalState;
static int g_terminalRedirect = 1;
static ULONGLONG g_terminalFirstTry;
static volatile LONG g_terminalNoted[TERMINAL_NOTED];
static const uint8_t kTerminalOpenEntry[] = {0x4c,0x89,0x44,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x60,0x8b,0x41,0x28};

static uintptr_t TerminalOpenDetour(uintptr_t manager, int32_t presenter, void* arg)
{
    if (presenter >= 0 && presenter < TERMINAL_NOTED && InterlockedExchange(&g_terminalNoted[presenter], 1) == 0)
    {
        uintptr_t game = ApiModuleBase("game.dll", NULL), caller = (uintptr_t)__builtin_return_address(0);
        Note("terminal: the game opens menu presenter %d (called from game.dll+0x%llX)", presenter,
            (unsigned long long)(game && caller > game ? caller - game : caller));
    }
    if (presenter == TERMINAL_INFORMATION && g_terminalRedirect)
    {
        static volatile LONG redirected;
        if (InterlockedIncrement(&redirected) <= 4) Note("terminal: the information terminal opens the lobby browser (presenter %d)", TERMINAL_SERVER_BROWSER);
        presenter = TERMINAL_SERVER_BROWSER;
    }
    return g_terminalOpenOriginal(manager, presenter, arg);
}

static void InstallTerminal(void)
{
    if (g_terminalState) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    ULONGLONG now = GetTickCount64();
    if (!g_terminalFirstTry) g_terminalFirstTry = now;
    if (strcmp(g_build, "25480438")) { g_terminalState = -1; return; }
    if (!game || !PinMatches(game + TERMINAL_OPEN_PRESENTER, kTerminalOpenEntry, sizeof kTerminalOpenEntry))
    {
        if (now - g_terminalFirstTry < 60000) return;
        g_terminalState = -1;
        Note("terminal: not changed; the presenter manager does not match the reviewed build");
        return;
    }
    char mode[16] = {0};
    if (GetEnvironmentVariableA("HELLFORGE_TERMINAL", mode, sizeof mode) && !_stricmp(mode, "news")) g_terminalRedirect = 0;
    g_terminalState = LoaderHook("information terminal", game + TERMINAL_OPEN_PRESENTER, (void*)TerminalOpenDetour,
        (void**)&g_terminalOpenOriginal) ? -1 : 1;
    Note(g_terminalState != 1 ? "terminal: could not hook the presenter manager"
        : g_terminalRedirect ? "terminal: the information terminal opens the lobby browser"
        : "terminal: kept as the game has it (HELLFORGE_TERMINAL=news)");
}
