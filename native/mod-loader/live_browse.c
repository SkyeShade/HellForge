/* SDK 16, the lobby browser (docs/multiplayer.md "The lobby browser"): the backend's list of every HellForge lobby it
 * finds, for an editor to show (browse_state, browse_lobby), and the editor's asks back (browse_ask). Over the live pipe:
 *   backend: browselobby <seq> <version> <index> <ready|download|trust|unjoinable|blocked|full|locked|unknown>
 *                        <friend,mission,joinable or -> <members> <max members> <difficulty or -> <planet or -> <mods>
 *                        <download bytes> <lobby id> <host name> <reason> <players> <mod list> <strangers|editedstats|->
 *            browsestate <seq> <version> <none|searching|ready|unavailable|invited|failed> <lobbies> <message>
 *   loader:  browseask <refresh|join|quick|allow> <lobby id, strangers|editedstats, or ->  (unasked: the editor's ask)
 * As mod sync's lines do, a version's browselobby lines come first and wait in g_browseNext (the pipe's thread alone
 * touches it); its browsestate line publishes them. A browsestate of the version already published (a search under way,
 * a join's outcome) keeps its lobbies. The terminal count is terminal.c's: how many times the ship's information
 * terminal opened the browser. Included by live.c after LiveQueue. */
#define BROWSE_MAX_LOBBIES 128
static const char* const kBrowseStates[] = {"none", "searching", "ready", "unavailable", "invited", "failed"};
static const char* const kBrowseVerdicts[] = {"ready", "download", "trust", "unjoinable", "blocked", "full", "locked", "unknown"};
static const char* const kBrowseFixes[] = {"-", "strangers", "editedstats", "sandbox"};
static SRWLOCK g_browseLock = SRWLOCK_INIT;
static HfBrowseState g_browse;
static HfBrowseLobby g_browseLobbies[BROWSE_MAX_LOBBIES];
static HfBrowseLobby g_browseNext[BROWSE_MAX_LOBBIES];
static uint32_t g_browseNextVersion;
static uint64_t g_browseNextSeen[BROWSE_MAX_LOBBIES / 64]; /* the indices of g_browseNext that arrived, a bit each */

/* A lobby id as the backend lists it: 1 to 64 printable ASCII characters, no spaces. */
static int BrowseLobbyId(const char* text)
{
    size_t length = strlen(text);
    if (length == 0 || length > 64) return 0;
    for (const char* c = text; *c; c++) if ((unsigned char)*c <= ' ' || (unsigned char)*c >= 0x7F) return 0;
    return 1;
}

/* "friend,mission,joinable" (any of them) or "-": HF_LOBBY_FLAG_..., or -1 for another word. */
static int BrowseFlags(const char* text)
{
    if (!strcmp(text, "-")) return 0;
    int flags = 0;
    for (const char* at = text; *at;)
    {
        size_t length = strcspn(at, ",");
        if (length == 6 && !strncmp(at, "friend", 6)) flags |= HF_LOBBY_FLAG_FRIEND;
        else if (length == 7 && !strncmp(at, "mission", 7)) flags |= HF_LOBBY_FLAG_MISSION;
        else if (length == 8 && !strncmp(at, "joinable", 8)) flags |= HF_LOBBY_FLAG_JOINABLE;
        else return -1;
        at += length + (at[length] == ',');
    }
    return flags;
}

/* A whole number up to max, or "-" (-1); 0 for anything else. */
static int BrowseOptional(const char* text, uint64_t max, int32_t* out)
{
    uint64_t n;
    if (!strcmp(text, "-")) { *out = -1; return 1; }
    if (!SyncNumber(text, &n) || n > max) return 0;
    *out = (int32_t)n;
    return 1;
}

static void BrowseLobbyLine(char* line, char* reply, size_t size)
{
    char* f[18];
    int n = SplitAll(line, f, 18);
    uint64_t version = 0, index = 0, members = 0, max = 0, mods = 0, bytes = 0;
    int32_t difficulty = -1, planet = -1;
    int verdict = n == 18 ? SyncName(f[4], kBrowseVerdicts, 8) : -1, flags = n == 18 ? BrowseFlags(f[5]) : -1;
    int fix = n == 18 ? SyncName(f[17], kBrowseFixes, 4) : -1;
    if (n != 18 || fix < 0 || !SyncNumber(f[2], &version) || version > UINT32_MAX || !SyncNumber(f[3], &index) || verdict < 0 || flags < 0 ||
        !SyncNumber(f[6], &members) || members > 64 || !SyncNumber(f[7], &max) || max > 64 || !BrowseOptional(f[8], 100, &difficulty) ||
        !BrowseOptional(f[9], 100000, &planet) || !SyncNumber(f[10], &mods) || mods > 4096 || !SyncNumber(f[11], &bytes) || !BrowseLobbyId(f[12]))
    { snprintf(reply, size, "%s\trefused\tmalformed browselobby", n >= 2 ? f[1] : "-"); return; }
    if (index >= BROWSE_MAX_LOBBIES) { snprintf(reply, size, "%s\trefused\ta list holds at most %d lobbies", f[1], BROWSE_MAX_LOBBIES); return; }
    if ((uint32_t)version != g_browseNextVersion)
    {
        memset(g_browseNext, 0, sizeof g_browseNext);
        g_browseNextVersion = (uint32_t)version;
        memset(g_browseNextSeen, 0, sizeof g_browseNextSeen);
    }
    HfBrowseLobby* l = &g_browseNext[index];
    memset(l, 0, sizeof *l);
    l->size = sizeof *l;
    l->verdict = (uint32_t)verdict;
    l->flags = (uint32_t)flags;
    l->members = (uint32_t)members;
    l->max_members = (uint32_t)max;
    l->difficulty = difficulty;
    l->planet = planet;
    l->mods = (uint32_t)mods;
    l->download_bytes = bytes;
    SyncCopy(l->lobby_id, sizeof l->lobby_id, f[12]);
    SyncCopy(l->host, sizeof l->host, f[13]);
    SyncCopy(l->reason, sizeof l->reason, f[14]);
    SyncCopy(l->players, sizeof l->players, f[15]);
    SyncCopy(l->mod_list, sizeof l->mod_list, f[16]);
    l->fix = (uint32_t)fix;
    g_browseNextSeen[index / 64] |= 1ull << (index % 64);
    snprintf(reply, size, "%s\tok\tnow", f[1]);
}

static void BrowseStateLine(char* line, char* reply, size_t size)
{
    char* f[6];
    int n = SplitAll(line, f, 6);
    uint64_t version = 0, lobbies = 0;
    int state = n == 6 ? SyncName(f[3], kBrowseStates, 6) : -1;
    if (n != 6 || !SyncNumber(f[2], &version) || version > UINT32_MAX || state < 0 || !SyncNumber(f[4], &lobbies))
    { snprintf(reply, size, "%s\trefused\tmalformed browsestate", n >= 2 ? f[1] : "-"); return; }
    if (lobbies > BROWSE_MAX_LOBBIES) { snprintf(reply, size, "%s\trefused\ta list holds at most %d lobbies", f[1], BROWSE_MAX_LOBBIES); return; }
    int seen = 1;
    for (uint64_t i = 0; i < lobbies && seen; i++) seen = (g_browseNextSeen[i / 64] >> (i % 64)) & 1;
    AcquireSRWLockExclusive(&g_browseLock);
    int fresh = (uint32_t)version != g_browse.version;
    const char* why = !fresh && (uint32_t)lobbies != g_browse.lobbies ? "its lobby count changed without its lobbies" :
        fresh && lobbies > 0 && (g_browseNextVersion != (uint32_t)version || !seen) ? "its browselobby lines are missing" : NULL;
    if (why != NULL)
    {
        ReleaseSRWLockExclusive(&g_browseLock);
        snprintf(reply, size, "%s\trefused\t%s", f[1], why);
        return;
    }
    int changed = fresh || g_browse.state != (uint32_t)state;
    if (fresh) memcpy(g_browseLobbies, g_browseNext, (size_t)lobbies * sizeof g_browseLobbies[0]);
    g_browse.size = sizeof g_browse;
    g_browse.version = (uint32_t)version;
    g_browse.state = (uint32_t)state;
    g_browse.lobbies = (uint32_t)lobbies;
    SyncCopy(g_browse.message, sizeof g_browse.message, f[5]);
    HfBrowseState noted = g_browse;
    ReleaseSRWLockExclusive(&g_browseLock);
    if (changed)
        Note("lobby browser %u: %s, %u lobb%s%s%s", noted.version, kBrowseStates[state], noted.lobbies, noted.lobbies == 1 ? "y" : "ies",
            noted.message[0] ? ": " : "", noted.message);
    snprintf(reply, size, "%s\tok\tnow", f[1]);
}

/* The backend is gone: no list stands (the terminal count stays). */
static void BrowseClear(void)
{
    AcquireSRWLockExclusive(&g_browseLock);
    memset(&g_browse, 0, sizeof g_browse);
    ReleaseSRWLockExclusive(&g_browseLock);
}

/* SDK 16, browse_state: the list as the backend last sent it, with the terminal count. At most out->size bytes are
 * written. 1 while a backend is connected. */
static int ApiBrowseState(HfMod* mod, HfBrowseState* out)
{
    (void)mod;
    if (out == NULL || out->size < sizeof(uint32_t)) return 0;
    uint32_t size = out->size;
    AcquireSRWLockShared(&g_browseLock);
    HfBrowseState now = g_browse;
    ReleaseSRWLockShared(&g_browseLock);
    now.size = sizeof now;
    now.terminal = (uint32_t)g_terminalOpens;
    now.camera = TcamState();
    g_browseReadAt = GetTickCount64(); /* an editor shows the browser: the terminal opens no screen of the game's under it */
    AcquireSRWLockShared(&g_liveOutLock);
    int connected = g_liveConnected;
    ReleaseSRWLockShared(&g_liveOutLock);
    memcpy(out, &now, size < sizeof now ? size : sizeof now);
    out->size = size;
    return connected;
}

/* SDK 16, browse_lobby: the index-th lobby of the list. At most out->size bytes are written. */
static int ApiBrowseLobby(HfMod* mod, uint32_t index, HfBrowseLobby* out)
{
    (void)mod;
    if (out == NULL || out->size < sizeof(uint32_t)) return 0;
    uint32_t size = out->size;
    HfBrowseLobby now;
    AcquireSRWLockShared(&g_browseLock);
    int ok = index < g_browse.lobbies;
    if (ok) now = g_browseLobbies[index];
    ReleaseSRWLockShared(&g_browseLock);
    if (!ok) return 0;
    memcpy(out, &now, size < sizeof now ? size : sizeof now);
    out->size = size;
    return 1;
}

/* SDK 16, browse_ask: a new list, a join of a listed lobby, Quick Play or a setting the browser offers, queued for the
 * backend. */
static int ApiBrowseAsk(HfMod* mod, uint32_t ask, const char* lobbyId)
{
    (void)mod;
    if (ask == HF_BROWSE_SHOWN || ask == HF_BROWSE_HIDDEN) { TcamBrowser(ask == HF_BROWSE_SHOWN); return 1; }
    const char* verb = ask == HF_BROWSE_REFRESH ? "refresh" : ask == HF_BROWSE_JOIN ? "join" : ask == HF_BROWSE_QUICK ? "quick" :
        ask == HF_BROWSE_ALLOW_STRANGERS || ask == HF_BROWSE_SHOW_EDITED_STATS ? "allow" : NULL;
    if (verb == NULL) return 0;
    if (ask == HF_BROWSE_JOIN && (lobbyId == NULL || !BrowseLobbyId(lobbyId))) return 0;
    const char* what = ask == HF_BROWSE_JOIN ? lobbyId : ask == HF_BROWSE_ALLOW_STRANGERS ? "strangers" : ask == HF_BROWSE_SHOW_EDITED_STATS ? "editedstats" : "-";
    char line[128];
    int length = snprintf(line, sizeof line, "browseask\t%s\t%s\n", verb, what);
    int queued = LiveQueue(line, length);
    if (ask != HF_BROWSE_REFRESH || !queued) Note("lobby browser: %s%s%s%s", ask == HF_BROWSE_REFRESH ? "a new list asked for" : verb,
        ask == HF_BROWSE_REFRESH || ask == HF_BROWSE_QUICK ? "" : " ", ask == HF_BROWSE_REFRESH || ask == HF_BROWSE_QUICK ? "" : what,
        queued ? "" : " (not sent: no backend connected)");
    return queued;
}
