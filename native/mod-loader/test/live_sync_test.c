/* Mod sync over the live pipe (live.c, SDK 14): the backend's syncmod and syncstate lines, held until their syncstate
 * publishes them; progress keeping the mods; malformed and incomplete offers refused; long text cut on a whole
 * character; answers queued only for the current offer. Then the real pipe: LiveWatch connected to a server here, an
 * offer sent, and an answer written while LiveWatch waits on its read (a synchronous pipe would hold it until the next
 * request), and the offer withdrawn when the server goes. Then the lobby browser's lines (SDK 16, live_browse.c), held and
 * published the same way, and the editor's asks. Included by stats_test.c. */

static void SyncLine(const char* text, char* reply, size_t size)
{
    char line[1024];
    snprintf(line, sizeof line, "%s", text);
    LiveRequest(line, reply, size);
}

/* Reads one line the loader wrote, waiting at most two seconds. */
static int SyncServerLine(HANDLE server, char* line, size_t size)
{
    size_t used = 0;
    ULONGLONG deadline = GetTickCount64() + 2000;
    while (GetTickCount64() < deadline && used < size - 1)
    {
        DWORD available = 0, read = 0;
        if (!PeekNamedPipe(server, NULL, 0, NULL, &available, NULL)) return 0;
        if (available == 0) { Sleep(5); continue; }
        if (!ReadFile(server, line + used, 1, &read, NULL) || read != 1) return 0;
        if (line[used] == '\n') { line[used] = 0; return 1; }
        used++;
    }
    return 0;
}

static void SyncServerSend(HANDLE server, const char* text)
{
    DWORD written;
    assert(WriteFile(server, text, (DWORD)strlen(text), &written, NULL) && written == strlen(text));
}

static void LiveSyncPipeFixture(void)
{
    wchar_t logs[MAX_PATH], pipeName[MAX_PATH];
    memcpy(logs, g_logs, sizeof logs);
    LONG fired = g_fired[HF_EVENT_ENTITIES_READY];
    swprintf(g_logs, MAX_PATH, L"C:\\hellforge-test\\synctest-%lu", GetCurrentProcessId());
    swprintf(pipeName, MAX_PATH, L"\\\\.\\pipe\\hellforge-live-synctest-%lu", GetCurrentProcessId());
    HANDLE server = CreateNamedPipeW(pipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    assert(server != INVALID_HANDLE_VALUE);
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 1);
    HANDLE watch = CreateThread(NULL, 0, LiveWatch, NULL, 0, NULL);
    assert(watch != NULL);
    assert(ConnectNamedPipe(server, NULL) || GetLastError() == ERROR_PIPE_CONNECTED);
    char line[512];
    assert(SyncServerLine(server, line, sizeof line) && !strncmp(line, "hello\t", 6));
    /* this machine's own packages follow the hello (none held here), and again whenever they change */
    assert(SyncServerLine(server, line, sizeof line) && !strcmp(line, "packages\t-"));
    LivePackagesChanged();
    assert(SyncServerLine(server, line, sizeof line) && !strcmp(line, "packages\t-"));
    SyncServerSend(server, "syncmod\t1\t21\t0\tdownload\tcontent\t5000\tfriend.tanks\tTanks\t2.0\t-\n"
        "syncstate\t2\t21\toffer\tFriend Two\t5000\t0\t1\tthe host runs other mods\n");
    assert(SyncServerLine(server, line, sizeof line) && !strcmp(line, "1\tok\tnow"));
    assert(SyncServerLine(server, line, sizeof line) && !strcmp(line, "2\tok\tnow"));
    HfSyncState state = {sizeof state};
    assert(ApiSyncState(NULL, &state) && state.state == HF_SYNC_OFFER && state.version == 21 && !strcmp(state.host, "Friend Two"));
    /* LiveWatch now waits on its read: the answer goes out at once all the same */
    assert(ApiSyncAnswer(NULL, 21, HF_SYNC_ACCEPT, NULL));
    assert(SyncServerLine(server, line, sizeof line) && !strcmp(line, "syncanswer\t21\taccept\t-"));
    /* and requests are still answered after it */
    SyncServerSend(server, "syncstate\t3\t21\tdownloading\tFriend Two\t5000\t2500\t1\t\n");
    assert(SyncServerLine(server, line, sizeof line) && !strcmp(line, "3\tok\tnow"));
    assert(ApiSyncState(NULL, &state) && state.state == HF_SYNC_DOWNLOADING && state.done == 2500);
    /* the backend goes: the offer with it, and answers are no longer queued */
    DisconnectNamedPipe(server);
    CloseHandle(server);
    ULONGLONG deadline = GetTickCount64() + 3000;
    while (ApiSyncState(NULL, &state) && GetTickCount64() < deadline) Sleep(10);
    assert(state.state == HF_SYNC_NONE && !ApiSyncAnswer(NULL, 21, HF_SYNC_DECLINE, NULL));
    /* LiveWatch keeps looking for a pipe of that name (none comes); the fixture leaves it */
    CloseHandle(watch);
    memcpy(g_logs, logs, sizeof logs);
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], fired);
}

static void LiveSyncFixture(void)
{
    char reply[512];
    HfSyncState state = {sizeof state};
    HfSyncMod mod = {sizeof mod};
    assert(!ApiSyncState(NULL, &state) && state.state == HF_SYNC_NONE && state.size == sizeof state);
    assert(!ApiSyncMod(NULL, 0, &mod));

    /* a version's mods wait for its syncstate */
    SyncLine("syncmod\t1\t5\t0\tdownload\tcontent\t1048576\tfriend.tanks\tTanks\t2.0\t-", reply, sizeof reply);
    assert(!strcmp(reply, "1\tok\tnow"));
    SyncLine("syncmod\t2\t5\t1\tmanual\tcode\t200\tfriend.guns\tGuns\t1.1\thttps://example.com/mods/guns?v=1.1", reply, sizeof reply);
    assert(!strcmp(reply, "2\tok\tnow"));
    assert(!ApiSyncState(NULL, &state) && !ApiSyncMod(NULL, 0, &mod));
    SyncLine("syncstate\t3\t5\toffer\tFriend One\t1048576\t0\t2\t", reply, sizeof reply); /* an empty message */
    assert(!strcmp(reply, "3\tok\tnow"));
    assert(ApiSyncState(NULL, &state) && state.state == HF_SYNC_OFFER && state.version == 5 && state.mods == 2 && state.bytes == 1048576 &&
        state.done == 0 && !strcmp(state.host, "Friend One") && state.message[0] == 0 && state.size == sizeof state);
    assert(ApiSyncMod(NULL, 0, &mod) && mod.action == HF_SYNC_MOD_DOWNLOAD && mod.kind == HF_SYNC_CLASS_CONTENT && mod.bytes == 1048576 &&
        !strcmp(mod.id, "friend.tanks") && !strcmp(mod.name, "Tanks") && !strcmp(mod.version, "2.0") && mod.url[0] == 0);
    assert(ApiSyncMod(NULL, 1, &mod) && mod.action == HF_SYNC_MOD_MANUAL && mod.kind == HF_SYNC_CLASS_CODE && !strcmp(mod.id, "friend.guns") &&
        !strcmp(mod.url, "https://example.com/mods/guns?v=1.1"));
    assert(!ApiSyncMod(NULL, 2, &mod));

    /* progress: the same version, its mods kept */
    SyncLine("syncstate\t4\t5\tdownloading\tFriend One\t1048576\t524288\t2\tdownloading Tanks", reply, sizeof reply);
    assert(!strcmp(reply, "4\tok\tnow"));
    assert(ApiSyncState(NULL, &state) && state.state == HF_SYNC_DOWNLOADING && state.done == 524288 && !strcmp(state.message, "downloading Tanks"));
    assert(ApiSyncMod(NULL, 1, &mod) && !strcmp(mod.id, "friend.guns"));
    SyncLine("syncstate\t5\t5\tdownloading\tFriend One\t1048576\t600000\t3\t", reply, sizeof reply);
    assert(!strncmp(reply, "5\trefused\t", 10)); /* a mod count changed without its mods */

    /* a new version: its syncstate without all of its syncmod lines is refused, the old offer stays */
    /* raw: data HellForge cannot check; an address that is not https:// is no address */
    SyncLine("syncmod\t6\t6\t1\toff\traw\t0\tmine.extra\tExtra\t1.0\thttp://example.com/extra", reply, sizeof reply);
    SyncLine("syncstate\t7\t6\tready\tFriend One\t0\t0\t2\t", reply, sizeof reply);
    assert(!strncmp(reply, "7\trefused\t", 10) && strstr(reply, "missing"));
    assert(ApiSyncState(NULL, &state) && state.version == 5);
    /* a mod to trust; an address with a space (or a quote, a backslash) is no address either */
    SyncLine("syncmod\t8\t6\t0\ttrust\tcode\t300\tfriend.trusty\tTrusty\t0.4.1\thttps://example.com/a b", reply, sizeof reply);
    SyncLine("syncstate\t9\t6\tready\tFriend One\t0\t0\t2\t", reply, sizeof reply);
    assert(!strcmp(reply, "9\tok\tnow"));
    assert(ApiSyncState(NULL, &state) && state.state == HF_SYNC_READY && state.version == 6 && state.mods == 2);
    assert(ApiSyncMod(NULL, 0, &mod) && mod.action == HF_SYNC_MOD_TRUST && mod.kind == HF_SYNC_CLASS_CODE && mod.url[0] == 0 && mod.flags == 0);
    assert(ApiSyncMod(NULL, 1, &mod) && mod.action == HF_SYNC_MOD_OFF && mod.kind == HF_SYNC_CLASS_RAW && !strcmp(mod.id, "mine.extra") && mod.url[0] == 0);

    /* malformed lines: refused, nothing changes */
    const char* bad[] = {
        "syncmod\t10\t7\t0\tsteal\tcontent\t1\tx\tX\t1\t-",        /* an unknown action */
        "syncmod\t11\t7\t0\tdownload\tscript\t1\tx\tX\t1\t-",      /* an unknown class */
        "syncmod\t12\t7\t512\tdownload\tcontent\t1\tx\tX\t1\t-",   /* past the 512th mod */
        "syncmod\t13\t7\t0\tdownload\tcontent\t-1\tx\tX\t1\t-",    /* not a number */
        "syncmod\t14\t7\t0\tdownload\tcontent\t1\t\tX\t1\t-",      /* no id */
        "syncmod\t15\t7\t0\tdownload\tcontent\t1\tx\tX\t1",        /* a field short (the url's) */
        "syncstate\t16\t7\twaiting\tA\t0\t0\t0\t",                 /* an unknown state */
        "syncstate\t17\t7\toffer\tA\t0\t0\t513\t",                 /* too many mods */
        "syncstate\t18\t99999999999\toffer\tA\t0\t0\t0\t",         /* a version past 32 bits */
        "syncstate\t19\t7\toffer\tA\t0\t0",                        /* fields short */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    {
        SyncLine(bad[i], reply, sizeof reply);
        assert(strstr(reply, "\trefused\t") != NULL);
    }
    assert(ApiSyncState(NULL, &state) && state.version == 6 && state.state == HF_SYNC_READY);

    /* a host's full set and more (the backend lists up to 512): every mod kept; one missing among them refuses the offer */
    char many[128];
    for (int i = 0; i < 300; i++)
    {
        if (i == 257) continue;
        snprintf(many, sizeof many, "syncmod\t%d\t30\t%d\tdownload\tcontent\t%d\tfriend.mod%d\tMod %d\t1\t-", 100 + i, i, i, i, i);
        SyncLine(many, reply, sizeof reply);
        assert(strstr(reply, "\tok\t") != NULL);
    }
    SyncLine("syncstate\t99\t30\toffer\tFriend\t0\t0\t300\t", reply, sizeof reply);
    assert(strstr(reply, "missing") != NULL && ApiSyncState(NULL, &state) && state.version == 6);
    SyncLine("syncmod\t98\t30\t257\tdownload\tcontent\t257\tfriend.mod257\tMod 257\t1\t-", reply, sizeof reply);
    SyncLine("syncstate\t99\t30\toffer\tFriend\t0\t0\t300\t", reply, sizeof reply);
    assert(!strcmp(reply, "99\tok\tnow") && ApiSyncState(NULL, &state) && state.version == 30 && state.mods == 300);
    assert(ApiSyncMod(NULL, 299, &mod) && !strcmp(mod.id, "friend.mod299") && mod.bytes == 299 && ApiSyncMod(NULL, 257, &mod) && mod.bytes == 257);
    assert(!ApiSyncMod(NULL, 300, &mod));

    /* long text is cut to its field, on a whole UTF-8 character; the message takes the rest of its line, tabs and all */
    char line[1024], name[200] = "x";
    for (int i = 0; i < 41; i++) strcat(name, "\xC3\xA9"); /* 83 bytes: the 80-byte cut falls inside the 40th e-acute */
    snprintf(line, sizeof line, "syncmod\t20\t8\t0\tdownload\tcontent\t9\t%s\t%s\t1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0\t-", "a.very.long.id", name);
    SyncLine(line, reply, sizeof reply);
    char host[100];
    memset(host, 'h', 90); host[90] = 0;
    snprintf(line, sizeof line, "syncstate\t21\t8\tblocked\t%s\t9\t0\t1\tyou blocked\tfriend.tanks", host);
    SyncLine(line, reply, sizeof reply);
    assert(!strcmp(reply, "21\tok\tnow"));
    assert(ApiSyncState(NULL, &state) && state.state == HF_SYNC_BLOCKED && strlen(state.host) == sizeof state.host - 1 &&
        !strcmp(state.message, "you blocked\tfriend.tanks"));
    assert(ApiSyncMod(NULL, 0, &mod) && strlen(mod.name) == 79 && (unsigned char)mod.name[78] == 0xA9 && strlen(mod.version) == sizeof mod.version - 1);

    /* a caller's smaller struct gets only its own bytes */
    struct { uint32_t size, state, version; } small = {sizeof small, 0, 0};
    uint8_t guard[sizeof(HfSyncState)];
    memset(guard, 0xAB, sizeof guard);
    memcpy(guard, &small, sizeof small);
    assert(ApiSyncState(NULL, (HfSyncState*)guard));
    memcpy(&small, guard, sizeof small);
    assert(small.size == 12 && small.state == HF_SYNC_BLOCKED && small.version == 8 && guard[12] == 0xAB);

    /* answers: only to the current offer, only with a backend connected, a blocked mod's id cleaned */
    AcquireSRWLockExclusive(&g_liveOutLock); g_liveConnected = 1; g_liveOutUsed = 0; ReleaseSRWLockExclusive(&g_liveOutLock);
    assert(!ApiSyncAnswer(NULL, 7, HF_SYNC_ACCEPT, NULL));  /* not the current offer */
    assert(!ApiSyncAnswer(NULL, 8, 99, NULL));              /* not an answer */
    assert(!ApiSyncAnswer(NULL, 8, HF_SYNC_BLOCK, NULL));   /* a block names its mod */
    assert(!ApiSyncAnswer(NULL, 8, HF_SYNC_BLOCK, "\t\n")); /* nothing left of it */
    assert(!ApiSyncAnswer(NULL, 8, HF_SYNC_TRUST, NULL));   /* a trust names its mod */
    assert(!ApiSyncAnswer(NULL, 8, HF_SYNC_OPEN, ""));      /* so does an open */
    assert(ApiSyncAnswer(NULL, 8, HF_SYNC_DECLINE, NULL));
    assert(ApiSyncAnswer(NULL, 8, HF_SYNC_BLOCK, "friend\t.tanks\n"));
    assert(ApiSyncAnswer(NULL, 8, HF_SYNC_TRUST, "friend.guns"));
    assert(ApiSyncAnswer(NULL, 8, HF_SYNC_OPEN, "friend.guns"));
    g_liveOut[g_liveOutUsed] = 0;
    assert(!strcmp(g_liveOut, "syncanswer\t8\tdecline\t-\nsyncanswer\t8\tblock\tfriend.tanks\nsyncanswer\t8\ttrust\tfriend.guns\n"
        "syncanswer\t8\topen\tfriend.guns\n"));
    AcquireSRWLockExclusive(&g_liveOutLock); g_liveConnected = 0; g_liveOutUsed = 0; ReleaseSRWLockExclusive(&g_liveOutLock);
    assert(!ApiSyncAnswer(NULL, 8, HF_SYNC_RESTART, NULL)); /* no backend */

    /* withdrawn: state none of a new version, then the pipe going clears it */
    SyncLine("syncstate\t22\t9\tnone\t\t0\t0\t0\t", reply, sizeof reply);
    assert(!strcmp(reply, "22\tok\tnow") && !ApiSyncState(NULL, &state) && state.state == HF_SYNC_NONE && state.version == 9);

    /* a stranger's mod to trust (a sandboxed game's offer): flagged, its address kept; a 12th field that is not "stranger"
     * is refused; a later offer without it is not flagged */
    SyncLine("syncmod\t23\t10\t0\ttrust\tcode\t300\tfar.trusty\tTrusty\t0.4.1\thttps://example.com/trusty\tfriend", reply, sizeof reply);
    assert(strstr(reply, "\trefused\t") != NULL);
    SyncLine("syncmod\t24\t10\t0\ttrust\tcode\t300\tfar.trusty\tTrusty\t0.4.1\thttps://example.com/trusty\tstranger", reply, sizeof reply);
    SyncLine("syncstate\t25\t10\tblocked\tFar One\t0\t0\t1\t", reply, sizeof reply);
    assert(!strcmp(reply, "25\tok\tnow"));
    assert(ApiSyncMod(NULL, 0, &mod) && mod.action == HF_SYNC_MOD_TRUST && (mod.flags & HF_SYNC_FLAG_STRANGER) && !strcmp(mod.url, "https://example.com/trusty"));
    SyncLine("syncmod\t26\t11\t0\ttrust\tcode\t300\tfar.trusty\tTrusty\t0.4.1\t-", reply, sizeof reply);
    SyncLine("syncstate\t27\t11\tblocked\tFriend One\t0\t0\t1\t", reply, sizeof reply);
    assert(!strcmp(reply, "27\tok\tnow") && ApiSyncMod(NULL, 0, &mod) && mod.flags == 0);
    SyncClear();
    assert(!ApiSyncState(NULL, &state) && state.version == 0);

    LiveSyncPipeFixture();
}

/* SDK 16, the lobby browser: a list's browselobby lines wait for its browsestate; a search under way and a join's outcome
 * keep the lobbies; malformed lines refused; asks queued only with a backend, a join only for a lobby id; the terminal's
 * count carried with the state; the list gone with the backend. */
static void LiveBrowseFixture(void)
{
    char reply[512];
    HfBrowseState state = {sizeof state};
    HfBrowseLobby lobby = {sizeof lobby};
    assert(!ApiBrowseState(NULL, &state) && state.state == HF_BROWSE_NONE && state.size == sizeof state && !ApiBrowseLobby(NULL, 0, &lobby));

    SyncLine("browselobby\t1\t3\t0\tready\tfriend,joinable\t2\t4\t7\t118\t0\t0\tsteam-1\tFriend One\t", reply, sizeof reply);
    assert(!strcmp(reply, "1\tok\tnow"));
    SyncLine("browselobby\t2\t3\t1\tunjoinable\t-\t1\t4\t-\t-\t3\t0\tsteam-2\tStranger\tit needs mods installed by hand", reply, sizeof reply);
    assert(!strcmp(reply, "2\tok\tnow"));
    SyncLine("browselobby\t3\t3\t2\tdownload\tmission,joinable\t3\t4\t10\t5\t2\t1048576\tsteam-3\t\t", reply, sizeof reply);
    assert(!strcmp(reply, "3\tok\tnow"));
    assert(!ApiBrowseLobby(NULL, 0, &lobby)); /* not published yet */
    SyncLine("browsestate\t4\t3\tready\t3\t", reply, sizeof reply);
    assert(!strcmp(reply, "4\tok\tnow"));
    assert(ApiBrowseState(NULL, &state) == 0 && state.state == HF_BROWSE_READY && state.version == 3 && state.lobbies == 3); /* 0: no backend */
    assert(ApiBrowseLobby(NULL, 0, &lobby) && lobby.verdict == HF_LOBBY_READY && lobby.flags == (HF_LOBBY_FLAG_FRIEND | HF_LOBBY_FLAG_JOINABLE) &&
        lobby.members == 2 && lobby.max_members == 4 && lobby.difficulty == 7 && lobby.planet == 118 && !strcmp(lobby.lobby_id, "steam-1") &&
        !strcmp(lobby.host, "Friend One") && lobby.reason[0] == 0);
    assert(ApiBrowseLobby(NULL, 1, &lobby) && lobby.verdict == HF_LOBBY_UNJOINABLE && lobby.flags == 0 && lobby.difficulty == -1 && lobby.planet == -1 &&
        lobby.mods == 3 && !strcmp(lobby.reason, "it needs mods installed by hand"));
    assert(ApiBrowseLobby(NULL, 2, &lobby) && lobby.verdict == HF_LOBBY_DOWNLOAD && (lobby.flags & HF_LOBBY_FLAG_MISSION) && lobby.download_bytes == 1048576 &&
        lobby.host[0] == 0);
    assert(!ApiBrowseLobby(NULL, 3, &lobby));

    /* a search under way, then a join's outcome: the same version keeps its lobbies; another count is refused */
    SyncLine("browsestate\t5\t3\tsearching\t3\t", reply, sizeof reply);
    assert(!strcmp(reply, "5\tok\tnow") && (ApiBrowseState(NULL, &state), state.state == HF_BROWSE_SEARCHING) && ApiBrowseLobby(NULL, 2, &lobby));
    SyncLine("browsestate\t6\t3\tinvited\t3\tAccept the invitation in game to join.", reply, sizeof reply);
    ApiBrowseState(NULL, &state);
    assert(!strcmp(reply, "6\tok\tnow") && state.state == HF_BROWSE_INVITED && !strcmp(state.message, "Accept the invitation in game to join."));
    SyncLine("browsestate\t7\t3\tready\t2\t", reply, sizeof reply);
    assert(strstr(reply, "\trefused\t") != NULL);
    /* a new version without all of its lines is refused, the old list stays; an empty list needs none */
    SyncLine("browselobby\t8\t4\t1\tready\t-\t1\t4\t-\t-\t0\t0\tsteam-9\tA\t", reply, sizeof reply);
    SyncLine("browsestate\t9\t4\tready\t2\t", reply, sizeof reply);
    assert(strstr(reply, "missing") != NULL && (ApiBrowseState(NULL, &state), state.version == 3));
    SyncLine("browsestate\t10\t5\tunavailable\t0\tMultiplayer is off for this session.", reply, sizeof reply);
    ApiBrowseState(NULL, &state);
    assert(!strcmp(reply, "10\tok\tnow") && state.state == HF_BROWSE_UNAVAILABLE && state.lobbies == 0 && !ApiBrowseLobby(NULL, 0, &lobby));

    const char* bad[] = {
        "browselobby\t11\t6\t0\tgreat\t-\t1\t4\t-\t-\t0\t0\tsteam-1\tA\t",       /* an unknown verdict */
        "browselobby\t12\t6\t0\tready\tvip\t1\t4\t-\t-\t0\t0\tsteam-1\tA\t",      /* an unknown flag */
        "browselobby\t13\t6\t128\tready\t-\t1\t4\t-\t-\t0\t0\tsteam-1\tA\t",      /* past the 128th lobby */
        "browselobby\t14\t6\t0\tready\t-\t1\t4\t-\t-\t0\t0\tsteam 1\tA\t",       /* a space in the lobby id */
        "browselobby\t15\t6\t0\tready\t-\t1\t4\t-\t-\t0\t0\t\tA\t",              /* no lobby id */
        "browselobby\t16\t6\t0\tready\t-\t1\t4\t-1\t-\t0\t0\tsteam-1\tA\t",      /* a difficulty that is not a number */
        "browselobby\t17\t6\t0\tready\t-\t1\t4\t-\t-\t0\t0\tsteam-1\tA",           /* a field short */
        "browsestate\t18\t6\tjoining\t0\t",                                          /* an unknown state */
        "browsestate\t19\t6\tready\t129\t",                                          /* too many lobbies */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    {
        SyncLine(bad[i], reply, sizeof reply);
        assert(strstr(reply, "\trefused\t") != NULL);
    }

    /* asks: only with a backend; a join names a lobby id */
    assert(!ApiBrowseAsk(NULL, HF_BROWSE_REFRESH, NULL));
    AcquireSRWLockExclusive(&g_liveOutLock); g_liveConnected = 1; g_liveOutUsed = 0; ReleaseSRWLockExclusive(&g_liveOutLock);
    assert(ApiBrowseState(NULL, &state) == 1);
    assert(!ApiBrowseAsk(NULL, 9, NULL) && !ApiBrowseAsk(NULL, HF_BROWSE_JOIN, NULL) && !ApiBrowseAsk(NULL, HF_BROWSE_JOIN, "steam\t1"));
    assert(ApiBrowseAsk(NULL, HF_BROWSE_REFRESH, "ignored") && ApiBrowseAsk(NULL, HF_BROWSE_JOIN, "steam-1"));
    g_liveOut[g_liveOutUsed] = 0;
    assert(!strcmp(g_liveOut, "browseask\trefresh\t-\nbrowseask\tjoin\tsteam-1\n"));
    AcquireSRWLockExclusive(&g_liveOutLock); g_liveConnected = 0; g_liveOutUsed = 0; ReleaseSRWLockExclusive(&g_liveOutLock);

    /* the terminal's count goes with the state, and stays when the backend goes */
    InterlockedExchange(&g_terminalOpens, 2);
    BrowseClear();
    assert(!ApiBrowseState(NULL, &state) && state.state == HF_BROWSE_NONE && state.version == 0 && state.terminal == 2);
    InterlockedExchange(&g_terminalOpens, 0);
}
