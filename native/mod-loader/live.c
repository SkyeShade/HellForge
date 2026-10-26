/* The launcher's live stats editor (docs/modding/runtime.md "Live edits"), over the named pipe \\.\pipe\hellforge-live-<session
 * id> that the launcher serves (current user only) while its editor is live in a solo session; in a co-op session the
 * session's backend serves it, to give a joiner the host's stats (docs/multiplayer.md step 5). Included by hellforge_modloader.c after fields.c. The loader connects once the entity data has loaded, and
 * again whenever the launcher stops and starts serving.
 *
 * Lines, UTF-8 and tab-separated:
 *   loader:   hello <protocol> <game build> <stats schema hash>
 *   launcher: set <seq> <kind> <component> <resource hex> <record> <offset> <storage> <value hex> <shared 0|1>
 *             reset <seq> <kind> <component> <resource hex> <record> <offset> <storage>
 *             base <seq> <kind> <component> <resource hex> <record> <offset> <storage>   (without Local Stats' edits)
 *             get <seq> <kind> <component> <resource hex> <record> <offset> <storage>
 *             session <seq> <solo|host|joiner> [stats=<own|host>] [<host name>]   (a joiner without stats= plays with
 *                     the host's stats, as before SDK 15)
 *             notice <seq> <error|info> <title> [<line>...]   (SDK 12: a message the in-game editor shows the player)
 *             preload <seq> <package id hex>,... or -      (the lobby's packages, for friends' changes: packages.c)
 *             shots <seq> <peer hex>:<weapon resource hex>=<projectile type>,... or -   (each player's projectile
 *                     swaps, which their shots fire on this machine: shots.c)
 *             syncmod <seq> <version> <index> <have|download|manual|blocked|update|off|trust> <content|code|bundled|raw> <bytes>
 *                     <id> <name> <mod version> <https:// url or ->
 *             syncstate <seq> <version> <none|offer|blocked|downloading|ready|failed> <host name> <bytes> <done> <mods>
 *                       <message>                                      (syncmod, syncstate: SDK 14, mod sync)
 *   loader:   <seq> ok <now|respawn|mission> [<value hex>]     or     <seq> refused <reason>
 *             syncanswer <version> <accept|decline|restart|block|trust|open> <mod id or ->   (unasked: the player's answer)
 *             packages <package id hex>,... or -          (unasked: this machine's own packages, sent on connecting and
 *                                                           whenever they change, for friends to load: packages.c)
 * A request names a field as the stats schema does (kind "component" and a component record, or a settings kind and
 * its row id; a stratagem by its stable id), so the editor sends what it would save. Each becomes a set_live edit of the
 * Local Stats mod (zz.local.stats), whose file the editor saves into: same checks, same conflict reports. */

#define LIVE_PROTOCOL 1
#define LIVE_MOD_ID "zz.local.stats"

static HfMod g_liveModStorage;
static HfMod* g_liveMod;
static SRWLOCK g_sessionLock = SRWLOCK_INIT;
static HfSession g_session;
static uint32_t g_sessionStats; /* HF_STATS_...: whose stats the lobby plays with */

/* SDK 6, session: the co-op role the session's backend last gave (solo until it says otherwise). */
static void ApiSession(HfMod* mod, HfSession* out)
{
    (void)mod;
    if (out == NULL) return;
    AcquireSRWLockShared(&g_sessionLock);
    *out = g_session;
    ReleaseSRWLockShared(&g_sessionLock);
}

/* SDK 15, stats_source: HF_STATS_HOST in a lobby whose host shares its stats (joined or hosting), else HF_STATS_OWN. */
static uint32_t ApiStatsSource(HfMod* mod)
{
    (void)mod;
    AcquireSRWLockShared(&g_sessionLock);
    uint32_t stats = g_sessionStats;
    ReleaseSRWLockShared(&g_sessionLock);
    return stats;
}

/* stats: whose stats the lobby plays with, as the session's backend sends it with the role (solo: always this machine's). */
static void SetSession(uint32_t role, uint32_t stats, const char* host)
{
    if (role == HF_SESSION_SOLO) stats = HF_STATS_OWN;
    AcquireSRWLockExclusive(&g_sessionLock);
    if (g_session.role != role || g_sessionStats != stats || strcmp(g_session.host, host))
    {
        g_session.role = role;
        g_sessionStats = stats;
        snprintf(g_session.host, sizeof g_session.host, "%s", host);
        g_session.version++;
        Note("session: %s%s%s%s", role == HF_SESSION_JOINER ? (stats == HF_STATS_HOST ? "joined, playing with the host's stats" :
            "joined, every player keeps their own stats") : role == HF_SESSION_HOST ? (stats == HF_STATS_HOST ?
            "hosting: friends play with this machine's stats" : "hosting: every player keeps their own stats") : "solo",
            host[0] ? " (host: " : "", host, host[0] ? ")" : "");
    }
    ReleaseSRWLockExclusive(&g_sessionLock);
}

/* SDK 12, notice: the backend's latest message for the player (why a friend's lobby refused this game's join, say). */
static HfNotice g_notice;

static void ApiNotice(HfMod* mod, HfNotice* out)
{
    (void)mod;
    if (out == NULL) return;
    AcquireSRWLockShared(&g_sessionLock);
    *out = g_notice;
    ReleaseSRWLockShared(&g_sessionLock);
}

/* f: the request's fields after the sequence number: the kind, the title, the lines. */
static void SetNotice(char** f, int n)
{
    AcquireSRWLockExclusive(&g_sessionLock);
    g_notice.kind = !strcmp(f[0], "error") ? HF_NOTICE_ERROR : !strcmp(f[0], "brief") ? HF_NOTICE_BRIEF : HF_NOTICE_INFO;
    snprintf(g_notice.title, sizeof g_notice.title, "%s", n > 1 ? f[1] : "");
    size_t used = 0;
    g_notice.text[0] = 0;
    for (int i = 2; i < n && used < sizeof g_notice.text - 1; i++)
    {
        int wrote = snprintf(g_notice.text + used, sizeof g_notice.text - used, "%s%s", used ? "\n" : "", f[i]);
        if (wrote < 0) break;
        used += (size_t)wrote;
    }
    if (used >= sizeof g_notice.text) used = sizeof g_notice.text - 1;
    g_notice.version++;
    Note("notice (%s): %s%s%s", g_notice.kind == HF_NOTICE_ERROR ? "error" : g_notice.kind == HF_NOTICE_BRIEF ? "brief" : "info", g_notice.title, used ? ": " : "", g_notice.text);
    ReleaseSRWLockExclusive(&g_sessionLock);
}

/* SDK 14, mod sync: the backend's offer to download the host's mods and restart with them (docs/mods.md "Playing with
 * other mods"), for an editor to show (sync_state, sync_mod) and answer (sync_answer). A version's syncmod lines come
 * first and wait in g_syncNext (the pipe's thread alone touches it); its syncstate line publishes them with the state. A
 * syncstate of the version already published is progress (state, done, message): its mods stay. Answers are queued
 * for LiveWatch, the pipe's only writer, so an answer never waits on the pipe's pending read. */
/* As many as the backend lists (LiveSyncDisplay.MaxMods): a host's set (at most 256) and this machine's own mods. */
#define SYNC_MAX_MODS 512
static const char* const kSyncStates[] = {"none", "offer", "blocked", "downloading", "ready", "failed"};
static const char* const kSyncActions[] = {"have", "download", "manual", "blocked", "update", "off", "trust"};
static const char* const kSyncClasses[] = {"content", "code", "bundled", "raw"};
static SRWLOCK g_syncLock = SRWLOCK_INIT;
static HfSyncState g_sync;
static HfSyncMod g_syncMods[SYNC_MAX_MODS];
static HfSyncMod g_syncNext[SYNC_MAX_MODS];
static uint32_t g_syncNextVersion;
static uint64_t g_syncNextSeen[SYNC_MAX_MODS / 64]; /* the indices of g_syncNext that arrived, a bit each */
/* Lines for LiveWatch to write (the answers), and whether a backend is connected to take them. */
static SRWLOCK g_liveOutLock = SRWLOCK_INIT;
static char g_liveOut[1024];
static size_t g_liveOutUsed;
static int g_liveConnected;
static HANDLE g_liveOutEvent;
static volatile LONG g_packagesDirty; /* this machine's own packages changed since the last "packages" line */

/* packages.c: this machine's own packages changed; LiveWatch sends them (called with no lock held, from any thread). */
static void LivePackagesChanged(void)
{
    InterlockedExchange(&g_packagesDirty, 1);
    if (g_liveOutEvent != NULL) SetEvent(g_liveOutEvent);
}

/* Whether the `length` characters at `at` are all hex digits. */
static int HexDigits(const char* at, size_t length)
{
    int hex = 1;
    for (size_t i = 0; hex && i < length; i++) hex = (at[i] >= '0' && at[i] <= '9') || ((at[i] | 0x20) >= 'a' && (at[i] | 0x20) <= 'f');
    return hex;
}

/* preload: the lobby's packages (list: 16 hex digits each, comma separated, or "-"), each asked for as a friend's. At
 * most 256: four players' 64. */
static void PreloadLine(const char* seq, const char* list, char* reply, size_t size)
{
    enum { PRELOAD_MAX = 256 };
    uint64_t ids[PRELOAD_MAX];
    int count = 0;
    for (const char* at = list; strcmp(list, "-") && *at;)
    {
        size_t length = strcspn(at, ",");
        int hex = length == 16 && count < PRELOAD_MAX && HexDigits(at, length);
        if (!hex) { snprintf(reply, size, "%s\trefused\tmalformed preload", seq); return; }
        ids[count++] = strtoull(at, NULL, 16);
        at += length + (at[length] == ',');
    }
    int resident, loading, refused, fresh;
    const char* why;
    PackagesForFriends(ids, count, &resident, &loading, &refused, &fresh, &why);
    if (fresh > 0 || refused > 0)
        Note("lobby assets: %d package(s) for friends' changes, %d new: %d resident, %d loading, %d refused%s%s", count, fresh, resident, loading,
            refused, why != NULL ? ": " : "", why != NULL ? why : "");
    snprintf(reply, size, "%s\tok\tnow", seq);
}

static int SyncName(const char* text, const char* const* names, int count)
{
    for (int i = 0; i < count; i++) if (!strcmp(text, names[i])) return i;
    return -1;
}

static int SyncNumber(const char* text, uint64_t* out)
{
    char* end;
    if (*text < '0' || *text > '9') return 0;
    *out = strtoull(text, &end, 10);
    return *end == 0;
}

/* Copies text into a field of `size` bytes; cut short, it ends on a whole UTF-8 character. */
static void SyncCopy(char* out, size_t size, const char* text)
{
    size_t length = strlen(text);
    if (length >= size)
    {
        length = size - 1;
        while (length > 0 && ((unsigned char)text[length] & 0xC0) == 0x80) length--;
    }
    memcpy(out, text, length);
    out[length] = 0;
}

/* Splits at every tab and keeps empty fields (Split skips them): a host name or message may be empty. The last field
 * takes the rest of the line. */
static int SplitAll(char* line, char** fields, int capacity)
{
    int count = 0;
    for (char* at = line; count < capacity;)
    {
        fields[count++] = at;
        char* tab = count < capacity ? strchr(at, '\t') : NULL;
        if (tab == NULL) break;
        *tab = 0;
        at = tab + 1;
    }
    return count;
}

/* A mod's web address as the backend checked it (ModCatalog.WebAddress): https://, at most 200 bytes, nothing but printable
 * ASCII without spaces, quotes or backslashes. Anything else (and "-") is no address: an editor only shows it, and the
 * backend opens it (HF_SYNC_OPEN) from its own offer. */
static int SyncUrl(const char* text)
{
    size_t length = strlen(text);
    if (length < 12 || length > 200 || strncmp(text, "https://", 8)) return 0;
    for (const char* c = text; *c; c++)
        if ((unsigned char)*c <= ' ' || (unsigned char)*c >= 0x7F || *c == '"' || *c == '\'' || *c == '\\' || *c == '<' || *c == '>' || *c == '`') return 0;
    return 1;
}

static void SyncModLine(char* line, char* reply, size_t size)
{
    char* f[11];
    int n = SplitAll(line, f, 11);
    uint64_t version = 0, index = 0, bytes = 0;
    int action = n == 11 ? SyncName(f[4], kSyncActions, 7) : -1, kind = n == 11 ? SyncName(f[5], kSyncClasses, 4) : -1;
    if (n != 11 || !SyncNumber(f[2], &version) || version > UINT32_MAX || !SyncNumber(f[3], &index) || !SyncNumber(f[6], &bytes) ||
        action < 0 || kind < 0 || !f[7][0])
    { snprintf(reply, size, "%s\trefused\tmalformed syncmod", n >= 2 ? f[1] : "-"); return; }
    if (index >= SYNC_MAX_MODS) { snprintf(reply, size, "%s\trefused\tan offer lists at most %d mods", f[1], SYNC_MAX_MODS); return; }
    if ((uint32_t)version != g_syncNextVersion)
    {
        memset(g_syncNext, 0, sizeof g_syncNext);
        g_syncNextVersion = (uint32_t)version;
        memset(g_syncNextSeen, 0, sizeof g_syncNextSeen);
    }
    HfSyncMod* m = &g_syncNext[index];
    memset(m, 0, sizeof *m);
    m->size = sizeof *m;
    m->action = (uint32_t)action;
    m->kind = (uint32_t)kind;
    m->bytes = bytes;
    SyncCopy(m->id, sizeof m->id, f[7]);
    SyncCopy(m->name, sizeof m->name, f[8]);
    SyncCopy(m->version, sizeof m->version, f[9]);
    if (SyncUrl(f[10])) SyncCopy(m->url, sizeof m->url, f[10]);
    g_syncNextSeen[index / 64] |= 1ull << (index % 64);
    snprintf(reply, size, "%s\tok\tnow", f[1]);
}

/* Whether every one of the first `mods` indices of g_syncNext arrived. */
static int SyncNextSeen(uint64_t mods)
{
    int seen = 1;
    for (uint64_t i = 0; i < mods && seen; i++) seen = (g_syncNextSeen[i / 64] >> (i % 64)) & 1;
    return seen;
}

/* Why a syncstate line cannot stand (NULL: it can): a published offer's mod count changed, or a new offer's syncmod lines
 * did not all arrive. Called with g_syncLock held. */
static const char* SyncStateRefused(int fresh, uint64_t version, uint64_t mods, int seen)
{
    return !fresh && (uint32_t)mods != g_sync.mods ? "its mod count changed without its mods" :
        fresh && mods > 0 && (g_syncNextVersion != (uint32_t)version || !seen) ? "its syncmod lines are missing" : NULL;
}

static void SyncStateLine(char* line, char* reply, size_t size)
{
    char* f[9];
    int n = SplitAll(line, f, 9);
    uint64_t version = 0, bytes = 0, done = 0, mods = 0;
    int state = n == 9 ? SyncName(f[3], kSyncStates, 6) : -1;
    if (n != 9 || !SyncNumber(f[2], &version) || version > UINT32_MAX || state < 0 || !SyncNumber(f[5], &bytes) || !SyncNumber(f[6], &done) ||
        !SyncNumber(f[7], &mods))
    { snprintf(reply, size, "%s\trefused\tmalformed syncstate", n >= 2 ? f[1] : "-"); return; }
    if (mods > SYNC_MAX_MODS) { snprintf(reply, size, "%s\trefused\tan offer lists at most %d mods", f[1], SYNC_MAX_MODS); return; }
    int seen = SyncNextSeen(mods);
    AcquireSRWLockExclusive(&g_syncLock);
    int fresh = (uint32_t)version != g_sync.version;
    const char* why = SyncStateRefused(fresh, version, mods, seen);
    if (why != NULL)
    {
        ReleaseSRWLockExclusive(&g_syncLock);
        snprintf(reply, size, "%s\trefused\t%s", f[1], why);
        return;
    }
    int changed = fresh || g_sync.state != (uint32_t)state;
    if (fresh) memcpy(g_syncMods, g_syncNext, (size_t)mods * sizeof g_syncMods[0]);
    g_sync.size = sizeof g_sync;
    g_sync.version = (uint32_t)version;
    g_sync.state = (uint32_t)state;
    g_sync.mods = (uint32_t)mods;
    g_sync.bytes = bytes;
    g_sync.done = done;
    SyncCopy(g_sync.host, sizeof g_sync.host, f[4]);
    SyncCopy(g_sync.message, sizeof g_sync.message, f[8]);
    HfSyncState noted = g_sync;
    ReleaseSRWLockExclusive(&g_syncLock);
    /* logged after the lock: the editor's render thread reads the state under it */
    if (changed)
        Note("mod sync %u: %s%s%s, %u mod(s), %llu bytes to download%s%s", noted.version, kSyncStates[state], noted.host[0] ? " from " : "",
            noted.host, noted.mods, (unsigned long long)bytes, noted.message[0] ? ": " : "", noted.message);
    snprintf(reply, size, "%s\tok\tnow", f[1]);
}

/* The backend is gone: no offer stands. */
static void SyncClear(void)
{
    AcquireSRWLockExclusive(&g_syncLock);
    if (g_sync.state != HF_SYNC_NONE) Note("mod sync %u: withdrawn (the backend disconnected)", g_sync.version);
    memset(&g_sync, 0, sizeof g_sync);
    ReleaseSRWLockExclusive(&g_syncLock);
}

/* SDK 14, sync_state: the current offer (state HF_SYNC_NONE, and 0, without one). At most out->size bytes are written. */
static int ApiSyncState(HfMod* mod, HfSyncState* out)
{
    (void)mod;
    if (out == NULL || out->size < sizeof(uint32_t)) return 0;
    uint32_t size = out->size;
    AcquireSRWLockShared(&g_syncLock);
    HfSyncState now = g_sync;
    ReleaseSRWLockShared(&g_syncLock);
    memcpy(out, &now, size < sizeof now ? size : sizeof now);
    out->size = size;
    return now.state != HF_SYNC_NONE;
}

/* SDK 14, sync_mod: the index-th mod of the current offer. At most out->size bytes are written. */
static int ApiSyncMod(HfMod* mod, uint32_t index, HfSyncMod* out)
{
    (void)mod;
    if (out == NULL || out->size < sizeof(uint32_t)) return 0;
    uint32_t size = out->size;
    HfSyncMod now;
    AcquireSRWLockShared(&g_syncLock);
    int ok = g_sync.state != HF_SYNC_NONE && index < g_sync.mods;
    if (ok) now = g_syncMods[index];
    ReleaseSRWLockShared(&g_syncLock);
    if (!ok) return 0;
    memcpy(out, &now, size < sizeof now ? size : sizeof now);
    out->size = size;
    return 1;
}

static const char* SyncAnswerWord(uint32_t answer)
{
    return answer == HF_SYNC_ACCEPT ? "accept" : answer == HF_SYNC_DECLINE ? "decline" : answer == HF_SYNC_RESTART ? "restart" :
        answer == HF_SYNC_BLOCK ? "block" : answer == HF_SYNC_TRUST ? "trust" : answer == HF_SYNC_OPEN ? "open" : NULL;
}

/* The mod id an answer names, without control characters, into id (`size` bytes); its length. */
static size_t SyncAnswerId(const char* modId, char* id, size_t size)
{
    size_t n = 0;
    for (const char* c = modId != NULL ? modId : ""; *c && n < size - 1; c++) if ((unsigned char)*c >= 0x20 && *c != 0x7F) id[n++] = *c;
    id[n] = 0;
    return n;
}

/* A line queued for LiveWatch to write, when a backend is connected and it fits; whether it was. */
static int LiveQueue(const char* line, int length)
{
    AcquireSRWLockExclusive(&g_liveOutLock);
    int queued = g_liveConnected && length > 0 && g_liveOutUsed + (size_t)length <= sizeof g_liveOut;
    if (queued) { memcpy(g_liveOut + g_liveOutUsed, line, (size_t)length); g_liveOutUsed += (size_t)length; }
    ReleaseSRWLockExclusive(&g_liveOutLock);
    if (queued && g_liveOutEvent != NULL) SetEvent(g_liveOutEvent);
    return queued;
}

/* SDK 14, sync_answer: the player's answer to the offer of that version, queued for the backend. 0 when another offer
 * came since, none stands, or no backend is connected. */
static int ApiSyncAnswer(HfMod* mod, uint32_t version, uint32_t answer, const char* modId)
{
    (void)mod;
    const char* word = SyncAnswerWord(answer);
    if (word == NULL) return 0;
    char id[65] = "-";
    int named = answer == HF_SYNC_BLOCK || answer == HF_SYNC_TRUST || answer == HF_SYNC_OPEN; /* these name their mod */
    if (named && SyncAnswerId(modId, id, sizeof id) == 0) return 0;
    AcquireSRWLockShared(&g_syncLock);
    int current = g_sync.state != HF_SYNC_NONE && g_sync.version == version;
    ReleaseSRWLockShared(&g_syncLock);
    if (!current) { Note("mod sync: answer %s to offer %u dropped: it is not the current offer", word, version); return 0; }
    char line[128];
    int length = snprintf(line, sizeof line, "syncanswer\t%u\t%s\t%s\n", version, word, id);
    int queued = LiveQueue(line, length);
    Note("mod sync %u: the player answered %s%s%s%s", version, word, named ? " " : "", named ? id : "",
        queued ? "" : " (not sent: no backend connected)");
    return queued;
}

/* The Local Stats mod when the session loads it, else a stand-in under its id with a log of its own. */
static HfMod* LiveMod(void)
{
    if (g_liveMod != NULL) return g_liveMod;
    for (int i = 0; i < g_modCount; i++) if (!strcmp(g_mods[i].id, LIVE_MOD_ID)) g_liveMod = &g_mods[i];
    if (g_liveMod == NULL)
    {
        g_liveModStorage.api = &g_api;
        snprintf(g_liveModStorage.id, sizeof g_liveModStorage.id, "%s", LIVE_MOD_ID);
        g_liveModStorage.log = OpenLog(L"mod-" LIVE_MOD_ID L".log");
        g_liveModStorage.loaded = 1;
        g_liveMod = &g_liveModStorage;
    }
    return g_liveMod;
}

static const char* AppliesName(int applies)
{
    return applies == HF_APPLIES_NOW ? "now" : applies == HF_APPLIES_RESPAWN ? "respawn" : applies == HF_APPLIES_MISSION ? "mission" : "?";
}

/* A stratagem's type from its stable id (the stats schema names stratagem rows by it). */
static uint64_t StratagemTypeOf(uint32_t stable)
{
    for (uint32_t type = 1; type < STR_VANILLA + STR_MAX_NEW; type++)
    {
        int ready = 1;
        uintptr_t row = g_tableRow("StratagemDefinition", type, &ready);
        uint32_t id = 0;
        if (row != 0 && !SafeRead(row + 4, &id, 4) && id == stable) return type;
    }
    return 0;
}

/* Entity deltas (an attachment's stats, a default magazine's) and status multipliers are bytes of the tables the boot
 * stats pass edits (stats-tables.c StatsExtraRow), not mapped component fields: a live request writes them in the same
 * places, so a co-op joiner gets the host's attachment values too. The game reads an attachment's delta when it builds
 * a weapon, so a change is expected from the next respawn (not proven: HD2Runtime never tested a rebuilt weapon). Each
 * address keeps what it held before its first live write (reset) and its base: what another mod wrote there, else what
 * the boot pass found before the Local Stats mod's edit, else what it held before. Live values stack as live edits do
 * (fields.c): the editors' over a Lua mod's over a code mod's, one value per rank (a later mod of the same rank replaces
 * the earlier's); the address holds the highest, and taking one away shows the one under it. */
#define LIVE_EXTRA_MAX 2048
typedef struct LiveExtraLayer { int held; uint32_t bits; HfMod* mod; } LiveExtraLayer;
typedef struct LiveExtra { uintptr_t address; uint32_t before; LiveExtraLayer layer[RANK_EDITOR + 1]; } LiveExtra;
static LiveExtra g_liveExtra[LIVE_EXTRA_MAX];
static int g_liveExtraCount;

static int LiveExtraKind(const char* kind) { return !strcmp(kind, "EntityDelta") || !strcmp(kind, STATUS_MULTIPLIER); }

/* What a live write found at the address before its first one (get_stat's base where no boot edit is there); 0 none.
 * Called with g_statsLock held. */
static int LiveExtraBefore(uintptr_t address, uint32_t* before)
{
    for (int i = 0; i < g_liveExtraCount; i++) if (g_liveExtra[i].address == address) { *before = g_liveExtra[i].before; return 1; }
    return 0;
}

/* The Lua or code mod whose live value is at the address under the editors' (get_stat's holder), and that value; NULL
 * none. Called with g_statsLock held. */
static const char* LiveExtraHolder(uintptr_t address, uint32_t* value)
{
    for (int i = 0; i < g_liveExtraCount; i++)
    {
        if (g_liveExtra[i].address != address) continue;
        for (int rank = RANK_LUA; rank >= RANK_CODE; rank--)
        {
            const LiveExtraLayer* l = &g_liveExtra[i].layer[rank];
            if (l->held && l->mod != NULL) { *value = l->bits; return l->mod->id; }
        }
    }
    return NULL;
}

/* A live write of an entity delta or status multiplier: verb "set" (bits), "reset" (the rank's value taken away: the one
 * under it shows, else what the address held before this session's first live write), "base" (the editors' value taken
 * away, and without the Local Stats mod's edits) or "get", by a mod of `rank` (mod: the Lua or code mod; NULL for the
 * editors), to the field of `bytes` bytes (floating: an f32) at `offset` of the record. */
typedef struct LiveExtraEdit
{
    const char* verb;
    int rank;
    HfMod* mod;
    const char* kind;
    const char* component;
    uint64_t resource;
    int64_t record;
    uint32_t offset, bytes;
    int floating;
    uint32_t bits;
} LiveExtraEdit;
enum { LIVE_EXTRA_REFUSED, LIVE_EXTRA_KEPT, LIVE_EXTRA_WRITTEN }; /* LiveExtraApply */

static int LiveExtraFind(uintptr_t address)
{
    int slot = -1;
    for (int i = 0; i < g_liveExtraCount; i++) if (g_liveExtra[i].address == address) slot = i;
    return slot;
}

/* The edit's rank's value set, or taken away when it is this mod's (or the editors'); 0 when it is another mod's: none
 * of this one's is here. */
static int LiveExtraLayerSet(LiveExtraLayer* own, const LiveExtraEdit* c, int set)
{
    if (set) { own->held = 1; own->bits = c->bits; own->mod = c->mod; return 1; }
    if (c->rank == RANK_EDITOR || own->mod == c->mod) { own->held = 0; own->mod = NULL; return 1; }
    return 0;
}

/* The highest rank holding a value at x (-1 none); *held whether any does. */
static int LiveExtraTop(const LiveExtra* x, int* held)
{
    int top = -1;
    *held = 0;
    for (int r = RANK_EDITOR; r >= RANK_CODE; r--) if (x->layer[r].held) { *held = 1; if (top < 0) top = r; }
    return top;
}

/* base: another mod's value, else what the Local Stats mod's boot edit replaced, else what it held before */
static uint32_t LiveExtraBase(uintptr_t at, const LiveExtra* x)
{
    int other = 0, local = 0;
    uint32_t otherValue = 0, localOriginal = 0;
    for (int i = 0; i < g_statCount; i++)
    {
        const StatEdit* s = &g_stats[i];
        if (!s->applied || s->address != at) continue;
        if (strcmp(s->mod->id, LIVE_MOD_ID)) { other = 1; otherValue = s->value; }
        else if (!local) { local = 1; localOriginal = s->original; }
    }
    return other ? otherValue : local ? localOriginal : x->before;
}

/* What the address holds after the edit: the top rank's value, else (reset) what it held before, else its base. */
static uint32_t LiveExtraWant(uintptr_t at, const LiveExtra* x, int top, int reset)
{
    if (top >= 0) return x->layer[top].bits;
    if (reset) return x->before;
    return LiveExtraBase(at, x);
}

/* `want` written at `at`, and read back as written. */
static int LiveExtraStore(uintptr_t at, uint32_t want, uint32_t bytes)
{
    uint32_t check = 0;
    return !ApiWrite(at, &want, bytes) && !SafeRead(at, &check, bytes) && (bytes == 1 ? (uint8_t)check == (uint8_t)want : check == want);
}

/* The edit at `at`, which holds `now`: *want, what it wrote there. LIVE_EXTRA_KEPT when there was nothing to write
 * (get, or nothing of this mod's to take away), LIVE_EXTRA_REFUSED with *why. Called with g_statsLock held. */
static int LiveExtraApply(const LiveExtraEdit* c, uintptr_t at, uint32_t now, uint32_t* want, const char** why)
{
    if (!strcmp(c->verb, "get")) return LIVE_EXTRA_KEPT;
    int slot = LiveExtraFind(at);
    int reset = !strcmp(c->verb, "reset"), set = !strcmp(c->verb, "set");
    if (reset && slot < 0) return LIVE_EXTRA_KEPT;
    if (slot < 0)
    {
        if (g_liveExtraCount == LIVE_EXTRA_MAX) { *why = "too many live attachment changes"; return LIVE_EXTRA_REFUSED; }
        slot = g_liveExtraCount++;
        memset(&g_liveExtra[slot], 0, sizeof g_liveExtra[slot]);
        g_liveExtra[slot].address = at;
        g_liveExtra[slot].before = now;
    }
    LiveExtra* x = &g_liveExtra[slot];
    if (!LiveExtraLayerSet(&x->layer[c->rank], c, set)) return LIVE_EXTRA_KEPT;
    int held, top = LiveExtraTop(x, &held);
    *want = LiveExtraWant(at, x, top, reset);
    if (c->floating) { float v; memcpy(&v, want, 4); if (!isfinite(v)) { *why = "not a finite number"; return LIVE_EXTRA_REFUSED; } }
    int written = LiveExtraStore(at, *want, c->bytes);
    if (written && reset && !held) g_liveExtra[slot] = g_liveExtra[--g_liveExtraCount];
    if (!written) { *why = "the write did not hold"; return LIVE_EXTRA_REFUSED; }
    return LIVE_EXTRA_WRITTEN;
}

/* The edit (get: *now only). HF_APPLIES_RESPAWN, or 0 with *why. */
static int LiveExtraWrite(const LiveExtraEdit* c, uint32_t* now, const char** why)
{
    StatEdit e;
    memset(&e, 0, sizeof e);
    snprintf(e.kind, sizeof e.kind, "%s", c->kind);
    snprintf(e.component, sizeof e.component, "%s", c->component);
    e.resource = c->resource;
    e.record = c->record;
    e.offset = c->offset;
    *now = 0;
    int table = StatsExtraKind(e.kind);
    if (table < 0 || !LiveExtraKind(c->kind)) { *why = "not an entity delta or status multiplier"; return 0; }
    if (!g_statsExtraDone[table]) { *why = "its table is not loaded yet"; return 0; }
    AcquireSRWLockExclusive(&g_statsLock);
    uintptr_t at = !strcmp(g_build, "25480438") ? StatsExtraRow(&e) : 0;
    if (!at || SafeRead(at, now, c->bytes))
    {
        ReleaseSRWLockExclusive(&g_statsLock);
        *why = "the delta's table or ownership is not as reviewed";
        return 0;
    }
    uint32_t want = 0;
    int applied = LiveExtraApply(c, at, *now, &want, why);
    ReleaseSRWLockExclusive(&g_statsLock);
    if (applied == LIVE_EXTRA_REFUSED) return 0;
    if (applied == LIVE_EXTRA_KEPT) return HF_APPLIES_RESPAWN;
    ApiLog(c->mod != NULL ? c->mod : LiveMod(), "live %s %s %s +%u (record %lld) of %016llx: %08x -> %08x (expected from the next respawn)", c->verb,
        c->kind, c->component, c->offset, (long long)c->record, (unsigned long long)c->resource, *now, want);
    return HF_APPLIES_RESPAWN;
}

static void LiveExtraRequest(const char* verb, char** f, const char* seq, char* reply, size_t size)
{
    char* end;
    uint64_t resource = strtoull(f[4], &end, 16);
    const char* storage = f[7];
    uint32_t bytes = !strcmp(storage, "u8") ? 1 : 4;
    if (*end || (bytes == 4 && strcmp(storage, "f32") && strcmp(storage, "u32") && strcmp(storage, "i32")))
    { snprintf(reply, size, "%s	refused	malformed request", seq); return; }
    uint32_t now = 0;
    const char* why = "refused";
    LiveExtraEdit edit = {verb, RANK_EDITOR, NULL, f[2], f[3], resource, strtoll(f[5], NULL, 10), (uint32_t)strtoul(f[6], NULL, 10), bytes,
        !strcmp(storage, "f32"), !strcmp(verb, "set") ? (uint32_t)strtoul(f[8], NULL, 16) : 0};
    int applies = LiveExtraWrite(&edit, &now, &why);
    if (!applies) snprintf(reply, size, "%s	refused	%s", seq, why);
    else if (!strcmp(verb, "get")) snprintf(reply, size, "%s	ok	respawn	%08x", seq, now);
    else snprintf(reply, size, "%s	ok	respawn", seq);
}

/* SDK 9, set_stat: an entity delta or status multiplier (what set_live cannot reach, get_stat reads) changed live in the
 * place the stats pass writes it. value NULL: back to before this session's live change, or with HF_LIVE_BASE to the
 * field's base. HF_APPLIES_RESPAWN (a weapon takes an attachment's value when it is next built), or 0 (logged why). */
static int ApiSetStat(HfMod* mod, const HfStat* stat, const HfValue* value, uint32_t flags)
{
    if (mod == NULL || stat == NULL || stat->kind == NULL || stat->component == NULL) return 0;
    uint32_t type = stat->type, bytes = type == HF_TYPE_U8 ? 1 : 4, bits = 0;
    if (value != NULL)
    {
        if (type == HF_TYPE_F32) { float v = (float)value->number; memcpy(&bits, &v, 4); }
        else if (type == HF_TYPE_I32) { int32_t v = (int32_t)value->number; memcpy(&bits, &v, 4); }
        else bits = (uint32_t)value->number;
    }
    uint32_t now = 0;
    const char* why = "refused";
    int rank = ModRank(mod);
    LiveExtraEdit edit = {value != NULL ? "set" : (flags & HF_LIVE_BASE) && rank == RANK_EDITOR ? "base" : "reset", rank,
        rank == RANK_EDITOR ? NULL : mod, stat->kind, stat->component, stat->resource, stat->record, stat->offset, bytes, type == HF_TYPE_F32,
        bits};
    int applies = LiveExtraWrite(&edit, &now, &why);
    if (!applies) ApiLog(mod, "set_stat %s %s +%u refused: %s", stat->kind, stat->component, stat->offset, why);
    return applies;
}

/* session <seq> <solo|host|joiner> [stats=<own|host>] [<host name>] */
static void SessionLine(char** f, int n, char* reply, size_t size)
{
    uint32_t role = !strcmp(f[2], "joiner") ? HF_SESSION_JOINER : !strcmp(f[2], "host") ? HF_SESSION_HOST : HF_SESSION_SOLO;
    int marked = n >= 4 && !strncmp(f[3], "stats=", 6);
    uint32_t stats = marked && !strcmp(f[3] + 6, "own") ? HF_STATS_OWN : HF_STATS_HOST;
    SetSession(role, stats, n >= 4 + marked ? f[3 + marked] : "");
    snprintf(reply, size, "%s\tok\tnow", f[1]);
}

/* A set, reset, base or get request for a mapped field, as LiveFieldRequest parsed it (f: its fields). */
typedef struct LiveField
{
    char** f;
    const char* seq;
    int set, base;
    const SdkComponent* c;
    uint32_t type;
    long long record;
    unsigned long offset;
    uint64_t target;
    HfField field;
    HfMod* mod;
} LiveField;

static uint32_t LiveStorageType(const char* storage)
{
    return !strcmp(storage, "f32") ? HF_TYPE_F32 : !strcmp(storage, "u32") ? HF_TYPE_U32 : !strcmp(storage, "i32") ? HF_TYPE_I32 :
        !strcmp(storage, "u8") ? HF_TYPE_U8 : 0;
}

/* The component, storage, record and offset the request names, and its target (a component's resource, a table's row):
 * 0 with the refusal in reply. */
static int LiveFieldNamed(LiveField* r, char* reply, size_t size)
{
    char** f = r->f;
    const char *kind = f[2], *storage = f[7];
    r->c = SdkComponentNamed(!strcmp(kind, "component") ? f[3] : kind);
    r->type = LiveStorageType(storage);
    char* end;
    uint64_t resource = strtoull(f[4], &end, 16);
    r->record = strtoll(f[5], NULL, 10);
    r->offset = strtoul(f[6], NULL, 10);
    if (r->c == NULL || r->type == 0 || *end || r->offset > 0xFFFF || (r->c->table != (strcmp(kind, "component") != 0)))
    { snprintf(reply, size, "%s\trefused\tHellForge has no mappings for %s %s", r->seq, kind, f[3]); return 0; }
    r->target = !r->c->table ? resource : !strcmp(kind, "StratagemDefinition") ? StratagemTypeOf((uint32_t)r->record) : (uint64_t)r->record;
    if (r->target == 0) { snprintf(reply, size, "%s\trefused\tno such %s row", r->seq, kind); return 0; }
    return 1;
}

/* The record the field is in, as an edit of it would find it (sf NULL: none). */
static uint8_t* LiveFieldRecord(const LiveField* r, const SdkComponent* resolved, const SdkField* sf, int* waiting)
{
    FieldEdit probe = {r->mod, EDIT_SET, r->target, 0, resolved, sf};
    probe.table = r->c->table;
    probe.shared = 1;
    return sf != NULL ? EditRecord(&probe, r->target, 0, waiting) : NULL;
}

/* get: what the field holds now, and when a change to it applies. */
static void LiveFieldGet(const LiveField* r, char* reply, size_t size)
{
    const SdkComponent* resolved = NULL;
    const SdkField* sf = ResolveField(r->mod, "live get", &r->field, &resolved);
    int waiting = 0;
    uint8_t* record = LiveFieldRecord(r, resolved, sf, &waiting);
    uint32_t bits = 0;
    g_reasonMod = NULL;
    if (record == NULL || SafeRead((uintptr_t)(record + sf->offset), &bits, sf->size == 1 ? 1 : 4))
    { snprintf(reply, size, "%s\trefused\t%s", r->seq, sf == NULL ? g_liveReason : waiting ? "its table is not loaded yet" : "no such record"); return; }
    snprintf(reply, size, "%s\tok\t%s\t%08x", r->seq, AppliesName((int)sf->applies), bits);
}

/* set (the value sent), reset or base, as a set_live edit of the Local Stats mod's. When it applies (HF_APPLIES_...), or 0. */
static int LiveFieldChange(const LiveField* r)
{
    if (!r->set) return ApiSetLive(r->mod, r->target, &r->field, NULL, r->base ? HF_LIVE_BASE : 0);
    uint32_t bits = (uint32_t)strtoul(r->f[8], NULL, 16);
    HfValue value = {r->type, 0.0, 0};
    if (r->type == HF_TYPE_F32) { float v; memcpy(&v, &bits, 4); value.number = v; }
    else if (r->type == HF_TYPE_I32) value.number = (int32_t)bits;
    else value.number = bits;
    return ApiSetLive(r->mod, r->target, &r->field, &value, r->f[9][0] == '1' ? HF_LIVE_SHARED : 0);
}

/* Every live change, with what the record holds afterwards, so a session log shows what the game got. */
static void LiveFieldLog(const LiveField* r, int applies)
{
    const SdkComponent* resolved = NULL;
    const SdkField* sf = applies ? ResolveField(r->mod, "live", &r->field, &resolved) : NULL;
    uint32_t now = 0;
    int waiting = 0;
    uint8_t* at = LiveFieldRecord(r, resolved, sf, &waiting);
    int readable = at != NULL && !SafeRead((uintptr_t)(at + sf->offset), &now, sf->size == 1 ? 1 : 4);
    ApiLog(r->mod, "live %s %s +%lu (%s %s, record %lld) of %016llx: %s%s%s", r->set ? "set" : r->base ? "base" : "reset", r->c->name, r->offset,
        r->f[2], r->f[3], r->record, (unsigned long long)r->target, applies ? "applies " : "refused: ",
        applies ? AppliesName(applies) : (g_liveReason[0] ? g_liveReason : "?"), "");
    if (readable) ApiLog(r->mod, "  %s at 0x%llx now holds %08x%s", sf->name, (unsigned long long)(uintptr_t)(at + sf->offset), now,
        r->set ? (strtoul(r->f[8], NULL, 16) == now ? " (as sent)" : " (NOT as sent)") : "");
}

/* set, reset, base and get: a field as the stats schema names it. */
static void LiveFieldRequest(char** f, int n, char* reply, size_t size)
{
    const char* seq = f[1];
    int set = !strcmp(f[0], "set"), reset = !strcmp(f[0], "reset"), get = !strcmp(f[0], "get"), base = !strcmp(f[0], "base");
    if (!(set && n == 10) && !((reset || get || base) && n == 8)) { snprintf(reply, size, "%s\trefused\tmalformed request", seq); return; }
    if (LiveExtraKind(f[2])) { LiveExtraRequest(f[0], f, seq, reply, size); return; }
    LiveField r = {f, seq, set, base};
    if (!LiveFieldNamed(&r, reply, size)) return;
    r.field = (HfField){r.c->component, r.c->layout, (uint16_t)r.offset, (uint16_t)(r.type == HF_TYPE_U8 ? 1 : 4), r.type, 0};
    r.mod = LiveMod();
    g_liveReason[0] = 0;
    g_reasonMod = r.mod;
    if (get) { LiveFieldGet(&r, reply, size); return; }
    int applies = LiveFieldChange(&r);
    g_reasonMod = NULL;
    if (applies == 0) snprintf(reply, size, "%s\trefused\t%s", seq, g_liveReason[0] ? g_liveReason : "refused");
    else snprintf(reply, size, "%s\tok\t%s", seq, AppliesName(applies));
    LiveFieldLog(&r, applies);
}

/* A request that sets a list of the lobby's (ShotsSetRules, RoundsSetLobby): ok, or refused with why. */
static void ListLine(const char* seq, const char* list, int (*set)(const char* list, const char** why), char* reply, size_t size)
{
    const char* why = NULL;
    if (set(list, &why)) snprintf(reply, size, "%s\tok\tnow", seq);
    else snprintf(reply, size, "%s\trefused\t%s", seq, why);
}

/* One request; the reply goes to `reply`. Also the native fixture's entry point. */
static void LiveRequest(char* line, char* reply, size_t size)
{
    if (!strncmp(line, "syncmod\t", 8)) { SyncModLine(line, reply, size); return; }
    if (!strncmp(line, "syncstate\t", 10)) { SyncStateLine(line, reply, size); return; }
    char* f[12];
    int n = Split(line, f, 12);
    if (n < 2) { snprintf(reply, size, "-\trefused\tmalformed request"); return; }
    const char* seq = f[1];
    if (!strcmp(f[0], "session") && n >= 3) { SessionLine(f, n, reply, size); return; }
    if (!strcmp(f[0], "preload") && n == 3) { PreloadLine(seq, f[2], reply, size); return; }
    if (!strcmp(f[0], "shots") && n == 3) { ListLine(seq, f[2], ShotsSetRules, reply, size); return; }
    if (!strcmp(f[0], "rounds") && n == 3) { ListLine(seq, f[2], RoundsSetLobby, reply, size); return; }
    if (!strcmp(f[0], "notice") && n >= 4)
    {
        SetNotice(f + 2, n - 2);
        snprintf(reply, size, "%s\tok\tnow", seq);
        return;
    }
    LiveFieldRequest(f, n, reply, size);
}

/* One write on the overlapped pipe, waited for. */
static int LiveWrite(HANDLE pipe, OVERLAPPED* op, const char* data, size_t length)
{
    DWORD written = 0;
    ResetEvent(op->hEvent);
    if (!WriteFile(pipe, data, (DWORD)length, NULL, op) && GetLastError() != ERROR_IO_PENDING) return 0;
    return GetOverlappedResult(pipe, op, &written, TRUE) && written == length;
}

static int LiveSend(HANDLE pipe, OVERLAPPED* op, const char* text)
{
    char line[600];
    int length = snprintf(line, sizeof line, "%s\n", text);
    return length > 0 && (size_t)length < sizeof line && LiveWrite(pipe, op, line, (size_t)length);
}

/* The queued answers (sync_answer) and, when they changed, this machine's own packages, written here: this thread is the
 * pipe's only writer. */
static int LiveFlush(HANDLE pipe, OVERLAPPED* op)
{
    char out[sizeof g_liveOut];
    AcquireSRWLockExclusive(&g_liveOutLock);
    size_t used = g_liveOutUsed;
    memcpy(out, g_liveOut, used);
    g_liveOutUsed = 0;
    ReleaseSRWLockExclusive(&g_liveOutLock);
    if (used != 0 && !LiveWrite(pipe, op, out, used)) return 0;
    if (!InterlockedExchange(&g_packagesDirty, 0)) return 1;
    char list[PKG_MAX_HELD * 17 + 2], line[sizeof list + 16];
    PackagesOwnList(list, sizeof list);
    int length = snprintf(line, sizeof line, "packages\t%s\n", list);
    return length > 0 && (size_t)length < sizeof line && LiveWrite(pipe, op, line, (size_t)length);
}

/* A connection's start or end: whether answers can be queued, none queued yet. */
static void LiveOutConnected(int connected)
{
    AcquireSRWLockExclusive(&g_liveOutLock);
    g_liveConnected = connected;
    g_liveOutUsed = 0;
    ReleaseSRWLockExclusive(&g_liveOutLock);
}

/* The complete lines in buffer[0, used) answered in order, until a reply cannot be written (*open 0); the rest of the
 * buffer, an unfinished line, moved to its start. Returns that rest's length. */
static size_t LiveLines(HANDLE pipe, OVERLAPPED* writeOp, char* buffer, size_t used, size_t capacity, int* open)
{
    char reply[512];
    char* start = buffer;
    for (char* newline; *open && (newline = memchr(start, '\n', used - (size_t)(start - buffer))) != NULL; start = newline + 1)
    {
        *newline = 0;
        if (newline > start && newline[-1] == '\r') newline[-1] = 0;
        if (*start == 0) continue;
        LiveRequest(start, reply, sizeof reply);
        *open = LiveSend(pipe, writeOp, reply);
    }
    used -= (size_t)(start - buffer);
    memmove(buffer, start, used);
    if (used >= capacity - 1) used = 0; /* a line too long to be a request */
    return used;
}

/* A connection's requests answered and its queued lines written, until it breaks. A read still pending then is
 * cancelled and waited for here, while its buffer is in scope: a late completion would write into reused stack. */
static void LiveServe(HANDLE pipe, OVERLAPPED* readOp, OVERLAPPED* writeOp)
{
    char buffer[8192];
    size_t used = 0;
    int open = 1, reading = 0;
    while (open)
    {
        if (!reading)
        {
            ResetEvent(readOp->hEvent);
            if (!ReadFile(pipe, buffer + used, (DWORD)(sizeof buffer - 1 - used), NULL, readOp) && GetLastError() != ERROR_IO_PENDING) break;
            reading = 1;
        }
        HANDLE waits[2] = {readOp->hEvent, g_liveOutEvent};
        DWORD woke = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (woke == WAIT_OBJECT_0 + 1) { open = LiveFlush(pipe, writeOp); continue; }
        if (woke != WAIT_OBJECT_0) break;
        DWORD read = 0;
        reading = 0;
        if (!GetOverlappedResult(pipe, readOp, &read, FALSE) || read == 0) break;
        used = LiveLines(pipe, writeOp, buffer, used + read, sizeof buffer, &open);
    }
    if (reading)
    {
        DWORD read;
        CancelIoEx(pipe, readOp);
        GetOverlappedResult(pipe, readOp, &read, TRUE);
    }
}

/* One connection to the launcher's pipe, from its hello to its end. */
static void LiveConnected(HANDLE pipe, OVERLAPPED* readOp, OVERLAPPED* writeOp)
{
    Note("live editor connected");
    LiveOutConnected(1);
    char hello[160];
    snprintf(hello, sizeof hello, "hello\t%d\t%s\t%s", LIVE_PROTOCOL, g_build, g_statsSchema[0] ? g_statsSchema : "-");
    InterlockedExchange(&g_packagesDirty, 1); /* the backend gets this machine's own packages as it connects */
    if (LiveSend(pipe, writeOp, hello) && LiveFlush(pipe, writeOp)) LiveServe(pipe, readOp, writeOp);
    LiveOutConnected(0);
    CloseHandle(pipe);
    Note("live editor disconnected");
    SetSession(HF_SESSION_SOLO, HF_STATS_OWN, ""); /* the backend is gone: nothing tells this game it is joined */
    const char* none;
    ShotsSetRules("-", &none); /* nor whose swaps friends' shots fire */
    RoundsSetLobby("-", &none); /* nor their edited rounds */
    SyncClear();
}

/* Connects to the launcher's pipe while the session runs; one connection at a time. The pipe is overlapped: a
 * synchronous handle serializes its I/O, so an answer written from another thread would wait for the next request. This
 * thread waits for either a request or a queued answer, and does all the writing. */
/* The live pipe's path for a log folder: named after the session's run folder, which is the log folder, or, sandboxed,
 * holds it as its game folder (the folder the game may write, docs/steam-bridge.md). 0 when it has none. */
static int LivePipePath(const wchar_t* logs, wchar_t* path, size_t size)
{
    wchar_t run[MAX_PATH];
    if (swprintf(run, MAX_PATH, L"%ls", logs) < 0) return 0;
    wchar_t* session = wcsrchr(run, L'\\');
    if (session != NULL && _wcsicmp(session + 1, L"game") == 0) { *session = 0; session = wcsrchr(run, L'\\'); }
    return session != NULL && session[1] != 0 && swprintf(path, size, L"\\\\.\\pipe\\hellforge-live-%ls", session + 1) >= 0;
}

static DWORD WINAPI LiveWatch(void* parameter)
{
    wchar_t path[MAX_PATH];
    if (!LivePipePath(g_logs, path, MAX_PATH)) return 0;
    g_liveOutEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    OVERLAPPED readOp = {0}, writeOp = {0};
    readOp.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    writeOp.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_liveOutEvent == NULL || readOp.hEvent == NULL || writeOp.hEvent == NULL) return 0;
    for (;;)
    {
        if (!g_fired[HF_EVENT_ENTITIES_READY]) { Sleep(500); continue; }
        HANDLE pipe = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(2000); continue; }
        LiveConnected(pipe, &readOp, &writeOp);
    }
}
