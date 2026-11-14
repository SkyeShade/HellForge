/* Live changes of the session's mods (docs/modding/live.md): the session's backend sends a change of the loader's mods
 * over the live pipe as one transaction, and the loader takes all of it or none. Mod sync uses it to give a joining
 * player the host's mods without a restart; the launcher's Mods page to switch mods while the game runs. A change the
 * session cannot take is refused before anything in the game changes, and the backend then restarts the session as
 * before. Included by hellforge_modloader.c after lua_bridge.c (it adds mods to g_mods under the bridge's lock).
 *
 *   modbegin   <seq> <txn> <solo|sync> <target digest>
 *   modorder   <seq> <txn> <index> <mod id>           the loader's mods after the change, 0 loads first
 *   modadd     <seq> <txn> <mod id> <kinds> <hash16> <folder> <dll or ->
 *   modreplace <seq> <txn> <mod id> <kinds> <hash16> <folder> <dll or ->
 *   modstat    <seq> <txn> <mod id> <kind> <component> <resource> <record> <offset> <storage> <value> <expect> <own>
 *              <shared> <unverified> <label>          a plan's stat line after "stat", for a mod added or replaced
 *   modnet     <seq> <txn> <mod id> <name> <kind> <expect bits> <expect min> <bits>   a networkFieldTypes entry of a mod added
 *              or replaced (nettypes.c)
 *   moddata    <seq> <txn> <mod id or -> <package id hex>                  a hot package the change needs (live_data.c)
 *   modres     <seq> <txn> <mod id or -> <type hex> <name hex> <target hex> an override of a resource's name (live_data.c)
 *   modremove  <seq> <txn> <mod id>
 *   modcommit  <seq> <txn>
 *   modabort   <seq> <txn>
 * kinds: what the mod carries, comma separated: stats (stat lines), code (a DLL: <dll> is its path), live (its manifest's
 * "live": true, SDK 16), net (network field types), data (game data). Each line answers <seq> ok now, or <seq> refused
 * <reason> <why>; a refusal ends the change. modcommit answers "ok queued" at once: the change runs on a thread of its
 * own (it may wait for packages, run mods' set-up and take a step on the game's thread), and the loader then sends on
 * its own "modstate <txn> <mod id> <added|removed|replaced> <applied> <refused>" for each mod and "modresult <txn> ok",
 * "modresult <txn> refused <reason> <why>" (nothing changed), or "modresult <txn> failed <why>" (made, but something of
 * a mod taken out may be left: the backend restarts the session). Reasons: state, coop, busy, capability, capacity,
 * malformed, packages.
 *
 * The gate: no friend in the game (this game hosts alone, or plays solo) at modbegin and again before anything changes;
 * not while the game loads (its prepare states). Anything but stats only on the ship or the title screen, once the
 * boot's events have all fired. One change at a time.
 *
 * Order of a commit: checks; a stat line naming another row waits for its packages; added and replaced code mods are set
 * up (a failure, or a refusal after it, takes them out again: live_code.c), what they register numbered as a boot of the
 * change's mods would (live_content.c); the gate again and every number checked; the content installed (one freeze,
 * one step on the game's thread); code mods taken out; the stats layer (live_stats.c); the set-ups' load events;
 * HF_EVENT_MODS_CHANGED to every mod. */

#include "live_code.c"  /* code mods set up and taken out (SDK 16) */
#include "live_data.c"  /* game data: hot packages and overrides */

#define TXN_MODS 64
enum { TXN_ADD, TXN_REMOVE, TXN_REPLACE };
static const char* const kTxnActions[] = {"added", "removed", "replaced"};
enum { TXN_STATS = 1, TXN_CODE = 2, TXN_LIVE = 4, TXN_NET = 8, TXN_DATA = 16 };
static const char* const kTxnKinds[] = {"stats", "code", "live", "net", "data"};
typedef struct TxnMod
{
    char id[80];
    int action;
    unsigned kinds;
    char hash[17];
    char folder[MAX_PATH * 3];
    char dll[MAX_PATH * 3]; /* UTF-8, "-" none */
    HfMod* old;             /* the session's entry it changes (removed or replaced), or NULL */
    HfMod* mod;             /* its entry after the change (added or replaced) */
    int applied, refused, failed;
} TxnMod;
typedef struct TxnStat { StatEdit edit; int mod; uintptr_t at; } TxnStat;
#define TXN_NETS 128
typedef struct TxnNet { int mod; char name[64]; uint8_t kind, expectBits; int32_t min, bits; } TxnNet;
/* A field the change touched: where, its size, what it held before any stats mod (known 0: a deferred edit holds it,
 * and keeps no original), and the difficulty row it is in (every copy of that row follows it: DifficultyMirror). */
typedef struct TxnField { uintptr_t at; uint32_t size, bottom; int known; uintptr_t difficultyRow; } TxnField;

static struct
{
    int open;
    volatile LONG committing; /* the change runs on its worker; the pipe takes no line of it meanwhile */
    char id[40];
    int sync;
    char digest[80];
    char order[MAX_MODS][80];
    int orderCount;
    TxnMod mods[TXN_MODS];
    int modCount;
    TxnStat* stats;
    int statCount, statCapacity;
    TxnField* fields;
    int fieldCount, fieldCapacity;
    TxnNet nets[TXN_NETS];
    int netCount;
    TxnData data[HOT_PACKAGES + 1];
    int dataCount;
    TxnRes res[HOT_OVERRIDES];
    int resCount;
    int ordersBefore[MAX_MODS]; /* the mods' order before the change, put back when it is refused */
    int orderSaved;             /* ordersBefore holds this change's: a change refused before it set the order keeps it */
} g_txn;
/* A retired stat entry's mod: never loaded, so every pass skips the entry. */
static HfMod g_retiredMod = {.id = "retired"};

static int LivePlayers(void)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL), manager = 0;
    uint32_t count = 0;
    int ok = 1;
    uint32_t at = game ? MfOffset(game, MF_PLAYER_COUNT, &ok) : 0; /* where the game's code addresses the count now */
    if (game != 0 && !ok) return EVP_LAYOUT;
    if (game == 0 || SafeRead(game + EVT_PLAYERS, &manager, 8) || manager == 0 || SafeRead(manager + at, &count, 4)) return -1;
    return (int)count;
}
/* The players in the game now (-1 unknown, EVP_LAYOUT the count's place unknown: taken as co-op), the entity load's state (registry.c) and the game's state: the native
 * fixture replaces them. */
static int (*g_livePlayers)(void) = LivePlayers;
static uint8_t* (*g_liveEntityState)(void) = EntityState;
static uint32_t LiveGameState(void) { return ApiGameState(NULL); }
static uint32_t (*g_liveGameState)(void) = LiveGameState;
/* The fixture runs a commit on its own thread's call (no worker). */
static int g_txnInline;

/* Why no change can run now ("<reason>\t<why>"), or NULL. */
static const char* TxnGate(void)
{
    HfSession session;
    ApiSession(NULL, &session);
    if (session.role == HF_SESSION_JOINER) return "coop\tthis game plays in a friend's lobby";
    int players = g_livePlayers();
    if (players == EVP_LAYOUT) return "coop\t" EVP_LAYOUT_WHY;
    if (players > 1) return "coop\tanother player is in the game";
    uint32_t state = g_liveGameState();
    if (state == HF_STATE_PREPARE_SHIP || state == HF_STATE_PREPARE_MISSION) return "state\tthe game is loading; try again once it has";
    return NULL;
}

/* The gate for a change of more than stats (code, network types, data): on the ship or the title screen, every boot
 * event fired. */
static const char* TxnGateWide(void)
{
    uint32_t state = g_liveGameState();
    if (state != HF_STATE_SHIP && state != HF_STATE_TITLE) return "state\tthis change waits for the ship or the title screen";
    for (int event = HF_EVENT_GAME_LOADED; event <= HF_EVENT_PROJECTILES_READY; event++)
        if (!g_fired[event]) return "state\tthe game has not finished starting yet; try again in a moment";
    return NULL;
}

static void TxnReset(void)
{
    g_txn.open = 0;
    g_txn.id[0] = 0;
    g_txn.orderCount = g_txn.modCount = g_txn.statCount = g_txn.fieldCount = g_txn.netCount = g_txn.dataCount = g_txn.resCount = 0;
    g_txn.orderSaved = 0;
}

static void LiveModsAbort(const char* why)
{
    if (!g_txn.open || g_txn.committing) return; /* a change under way runs to its end; its lines are queued */
    Note("live mod change %s dropped: %s", g_txn.id, why);
    TxnReset();
}

static void TxnRefuse(char* reply, size_t size, const char* seq, const char* reason, const char* why)
{
    snprintf(reply, size, "%s\trefused\t%s\t%s", seq, reason, why);
    if (g_txn.open) Note("live mod change %s refused (%s): %s", g_txn.id, reason, why);
    TxnReset();
}

static TxnMod* TxnFind(const char* id)
{
    for (int i = 0; i < g_txn.modCount; i++) if (!strcmp(g_txn.mods[i].id, id)) return &g_txn.mods[i];
    return NULL;
}

/* The session's running entry of that id the loader started or added (not a Lua mod, not one taken out, not a new
 * version being set up beside it), or NULL. */
static HfMod* TxnSessionMod(const char* id)
{
    for (int i = 0; i < g_modCount; i++)
        if (!g_mods[i].lua && !g_mods[i].removed && !g_mods[i].staging && !strcmp(g_mods[i].id, id)) return &g_mods[i];
    return NULL;
}

static int ModRunsCode(const HfMod* mod) { return mod != NULL && mod->loaded && wcscmp(mod->dll, L"-"); }

/* "stats,code,live" as TXN_ flags; 0 when a kind is unknown or none is given. */
static unsigned TxnKinds(const char* text)
{
    unsigned kinds = 0;
    for (const char* at = text; *at;)
    {
        size_t length = strcspn(at, ",");
        unsigned kind = 0;
        for (size_t k = 0; k < sizeof kTxnKinds / sizeof kTxnKinds[0]; k++)
            if (strlen(kTxnKinds[k]) == length && !strncmp(at, kTxnKinds[k], length)) kind = 1u << k;
        if (kind == 0) return 0;
        kinds |= kind;
        at += length + (at[length] == ',');
    }
    return kinds;
}

#include "live_own.c"   /* "own" lines: an entity's record of its own */
#include "live_stats.c" /* the stats layer: stat lines added, removed and replaced in load order */
#include "live_content.c" /* numbered content: registered, numbered, installed and made owned; tombstones */
#include "live_registry.c" /* weapons and entities */

/* Why the session cannot take this mod's part of the change ("<reason>\t<why>" in why), or NULL. */
static const char* TxnModCheck(TxnMod* t, char* why, size_t size)
{
    t->old = TxnSessionMod(t->id);
    int present = t->old != NULL && t->old->loaded;
    if (t->action == TXN_ADD && present) { snprintf(why, size, "malformed\t%s is already loaded", t->id); return why; }
    if (t->action != TXN_ADD && !present) { snprintf(why, size, "malformed\t%s is not loaded", t->id); return why; }
    if (t->action != TXN_ADD && ModRunsCode(t->old) && CodeRemovable(t->old, why, size) != NULL) return why;
    if (t->action != TXN_ADD && g_difficultyAdded && g_difficulty.mod == t->old)
    { snprintf(why, size, "capability\t%s added level 11, which the running game keeps; it goes at the next session", t->id); return why; }
    if ((t->kinds & TXN_CODE) && !(t->kinds & TXN_LIVE))
    { snprintf(why, size, "capability\t%s runs code that does not declare \"live\": true, which changes at the next session", t->id); return why; }
    return NULL;
}

/* The order: every loader mod that stays, in the order it had, the added ones anywhere, and none left out. */
static const char* TxnOrderCheck(char* why, size_t size)
{
    int last = -1;
    for (int i = 0; i < g_txn.orderCount; i++)
    {
        const TxnMod* t = TxnFind(g_txn.order[i]);
        HfMod* mod = TxnSessionMod(g_txn.order[i]);
        if (t != NULL && t->action == TXN_REMOVE) { snprintf(why, size, "malformed\tthe order lists %s, which is removed", t->id); return why; }
        if (t != NULL && t->action == TXN_ADD) continue;
        if (mod == NULL) { snprintf(why, size, "malformed\tthe order lists %s, which is not a mod of this session", g_txn.order[i]); return why; }
        if (mod->order <= last) { snprintf(why, size, "capability\tthe mods' order changed (%s); a new order needs a new session", mod->id); return why; }
        last = mod->order;
    }
    for (int i = 0; i < g_modCount; i++)
    {
        const HfMod* mod = &g_mods[i];
        const TxnMod* t = TxnFind(mod->id);
        if (mod->lua || !mod->loaded || mod->removed || (t != NULL && t->action == TXN_REMOVE)) continue;
        int listed = 0;
        for (int k = 0; k < g_txn.orderCount && !listed; k++) listed = !strcmp(g_txn.order[k], mod->id);
        if (!listed) { snprintf(why, size, "malformed\tthe order leaves out %s", mod->id); return why; }
    }
    return NULL;
}

/* The checks before anything changes: the gate, each mod's kinds, the order and room. NULL, or "<reason>\t<why>". */
static const char* TxnCheck(char* why, size_t size)
{
    const char* gate = TxnGate();
    if (gate == NULL && (g_txn.dataCount > 0 || g_txn.resCount > 0)) gate = TxnGateWide(); /* game data, whoever's */
    if (gate != NULL) return gate;
    for (int i = 0; i < g_txn.modCount; i++)
    {
        HfMod* running = TxnSessionMod(g_txn.mods[i].id);
        /* more than stats: what the change brings (its kinds), or what a running version takes with it (code, field types) */
        int wide = (g_txn.mods[i].kinds & ~(unsigned)TXN_STATS) || ModRunsCode(running) || NetAsksOf(running);
        const char* refused = wide ? TxnGateWide() : NULL;
        if (refused == NULL) refused = TxnModCheck(&g_txn.mods[i], why, size);
        if (refused != NULL) return refused;
    }
    const char* order = TxnOrderCheck(why, size);
    if (order != NULL) return order;
    int adds = 0;
    for (int i = 0; i < g_txn.modCount; i++) adds += g_txn.mods[i].action != TXN_REMOVE && (g_txn.mods[i].kinds & TXN_CODE || TxnSessionMod(g_txn.mods[i].id) == NULL);
    if (g_modCount + adds > MAX_MODS) { snprintf(why, size, "capacity\tthe session has %d mods, the most it can hold", MAX_MODS); return why; }
    return NULL;
}

/* A taken-out entry of this id a code mod's set-up takes again, instead of a new one (the session holds MAX_MODS): one
 * taken out whole (nothing of it left, every write it made put back) and not staged. Only under the same id: what still
 * names the entry (the tombstones of its content, its old DLL) then names that mod again, as a mod back this session
 * is numbered anyway (live_content.c); under another id they would name another mod. NULL when there is none. */
static HfMod* TxnModAgain(const char* id)
{
    for (int i = 0; i < g_modCount; i++)
    {
        HfMod* m = &g_mods[i];
        if (!m->lua && m->removed && !m->staging && !m->leftover && !WritesLost(m) && !strcmp(m->id, id)) return m;
    }
    return NULL;
}

/* A taken-out entry set up anew: what its last set-up left in it cleared, field by field (a reader on another thread
 * may hold it: its id and log stay). Under g_luaLock. */
static void TxnModRenew(HfMod* m)
{
    m->module = NULL;
    m->imageBase = m->imageEnd = 0;
    m->unload = NULL;
    wcscpy(m->dll, L"-");
    m->last[0] = 0;
    m->status[0] = m->statusGiven[0] = 0; /* set_status: its last set-up's line goes (mod_status.c skips it till now) */
    m->phase = 0;
    m->live = 0;
    m->staging = 1;
    m->leftover = 0;
    InterlockedExchange(&m->removing, 0);
}

/* The entry a change's mod takes without a new one: a stats mod's (fresh 0) running entry of its id or a taken-out one,
 * a code mod's set-up's (fresh 1) a taken-out one it may take again (TxnModAgain). NULL: it needs a new one. */
static HfMod* TxnModExisting(const TxnMod* t, int fresh)
{
    if (fresh) return TxnModAgain(t->id);
    HfMod* mod = TxnSessionMod(t->id);
    for (int i = 0; i < g_modCount && mod == NULL; i++)
        if (!g_mods[i].lua && g_mods[i].removed && !wcscmp(g_mods[i].dll, L"-") && !strcmp(g_mods[i].id, t->id)) mod = &g_mods[i];
    return mod;
}

/* A new entry at the end of the session's mods, taking *log; NULL when they are full. Under g_luaLock. */
static HfMod* TxnModNew(const TxnMod* t, int fresh, HANDLE* log)
{
    if (g_modCount >= MAX_MODS) return NULL;
    HfMod* m = &g_mods[g_modCount];
    memset(m, 0, sizeof *m);
    m->api = &g_api;
    snprintf(m->id, sizeof m->id, "%s", t->id);
    wcscpy(m->dll, L"-");
    m->log = *log;
    *log = INVALID_HANDLE_VALUE;
    m->order = g_modCount;
    m->staging = fresh;
    MemoryBarrier(); /* the entry is whole before anything counting the mods sees it */
    g_modCount++;
    return m;
}

/* The entry in g_mods for a mod added or replaced: a code mod's set-up (fresh) is staged beside the running version, in
 * a taken-out entry of its id (TxnModAgain) or a new one; a stats mod takes the one it had (a mod removed earlier in the
 * session, its slot kept). NULL when the mods are full. */
static HfMod* TxnModEntry(const TxnMod* t, int fresh)
{
    HfMod* mod = TxnModExisting(t, fresh);
    HANDLE log = INVALID_HANDLE_VALUE;
    if (mod == NULL)
    {
        /* opened before the lock: the game's thread queues Lua events under it */
        wchar_t logName[128];
        swprintf(logName, 128, L"mod-%hs.log", t->id);
        log = OpenLog(logName);
    }
    AcquireSRWLockExclusive(&g_luaLock);
    if (mod != NULL && fresh) TxnModRenew(mod);
    if (mod == NULL) mod = TxnModNew(t, fresh, &log);
    if (mod != NULL)
    {
        snprintf(mod->folder, sizeof mod->folder, "%s", t->folder);
        mod->removed = 0;
        mod->loaded = 1;
    }
    ReleaseSRWLockExclusive(&g_luaLock);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log); /* the mods were full */
    return mod;
}

/* The change's load order on the mods it keeps, adds and sets up (their hooks and edits sort by it); the order before
 * is kept, to put back if the change is refused. */
static void TxnAssignOrder(void)
{
    for (int i = 0; i < g_modCount; i++) g_txn.ordersBefore[i] = g_mods[i].order;
    g_txn.orderSaved = 1;
    for (int i = 0; i < g_txn.orderCount; i++)
    {
        const TxnMod* t = TxnFind(g_txn.order[i]);
        HfMod* mod = t != NULL && t->mod != NULL ? t->mod : TxnSessionMod(g_txn.order[i]);
        if (mod != NULL) mod->order = i;
        if (t != NULL && t->old != NULL) t->old->order = i; /* the running version keeps its place until it goes */
    }
}

static void TxnRestoreOrder(void)
{
    if (!g_txn.orderSaved) return;
    for (int i = 0; i < g_modCount; i++) if (!g_mods[i].staging) g_mods[i].order = g_txn.ordersBefore[i];
}

/* The set-up order of the change's code mods: by id, as at boot (live_code.c, co-op numbering). */
static void TxnCodeOrder(int order[TXN_MODS], int* count)
{
    *count = 0;
    for (int i = 0; i < g_txn.modCount; i++) if (g_txn.mods[i].action != TXN_REMOVE && (g_txn.mods[i].kinds & TXN_CODE)) order[(*count)++] = i;
    for (int i = 1; i < *count; i++)
        for (int j = i; j > 0 && strcmp(g_txn.mods[order[j - 1]].id, g_txn.mods[order[j]].id) > 0; j--)
        {
            int k = order[j];
            order[j] = order[j - 1];
            order[j - 1] = k;
        }
}

/* Added and replaced code mods: their new entries, DLLs and hellforge_mod_init, in set-up order. NULL, or why not
 * ("<reason>\t<why>"; the set-ups are taken out again by TxnUnstage). */
static const char* TxnCodeStage(char* why, size_t size)
{
    int order[TXN_MODS], count = 0;
    TxnCodeOrder(order, &count);
    for (int k = 0; k < count; k++)
    {
        TxnMod* t = &g_txn.mods[order[k]];
        t->mod = TxnModEntry(t, 1);
        if (t->mod == NULL) { snprintf(why, size, "capacity\tthe session has %d mods, the most it can hold", MAX_MODS); return why; }
        t->mod->live = 1;
    }
    TxnAssignOrder();
    for (int k = 0; k < count; k++)
    {
        TxnMod* t = &g_txn.mods[order[k]];
        if (CodeLoad(t->mod, t->dll, why, size) != NULL || CodeInit(t->mod, why, size) != NULL) return why;
    }
    return NULL;
}

/* A refused change: the code mods it set up are taken out again, what they registered with them, the order put back. */
static void TxnUnstage(void)
{
    LiveContentUnstage();
    for (int i = 0; i < g_txn.modCount; i++)
    {
        TxnMod* t = &g_txn.mods[i];
        if (t->mod == NULL || !t->mod->staging) continue;
        if (t->mod->module != NULL) CodeRemove(t->mod, HF_UNLOAD_REMOVE);
        t->mod->loaded = 0;
        t->mod->removed = 1;
        t->mod->staging = 0;
        t->mod = NULL;
    }
    TxnRestoreOrder();
}

/* The running code mods the change takes out (removed, or replaced by a new version); then the new versions run. */
static void TxnCodeRemove(void)
{
    for (int i = 0; i < g_txn.modCount; i++)
    {
        TxnMod* t = &g_txn.mods[i];
        if (t->action == TXN_ADD || !ModRunsCode(t->old)) continue;
        t->failed = CodeRemove(t->old, t->action == TXN_REPLACE ? HF_UNLOAD_REPLACE : HF_UNLOAD_REMOVE);
    }
    for (int i = 0; i < g_txn.modCount; i++) if (g_txn.mods[i].mod != NULL) g_txn.mods[i].mod->staging = 0;
}

/* The mods' entries after the change: stats mods added (a new entry, or the one they had) and replaced (the same
 * entry), removed ones unloaded. Under both locks. */
static void TxnApplyMods(void)
{
    for (int i = 0; i < g_txn.modCount; i++)
    {
        TxnMod* t = &g_txn.mods[i];
        if (t->action == TXN_REMOVE) { if (t->old != NULL) t->old->loaded = 0; continue; }
        if (t->mod == NULL) t->mod = t->action == TXN_ADD ? TxnModEntry(t, 0) : t->old;
        if (t->mod == NULL) continue;
        snprintf(t->mod->folder, sizeof t->mod->folder, "%s", t->folder);
        if (t->old != NULL && t->old != t->mod) t->old->loaded = 0; /* replaced by a new version */
    }
    for (int i = 0; i < g_txn.orderCount; i++)
    {
        HfMod* mod = TxnSessionMod(g_txn.order[i]);
        if (mod != NULL) mod->order = i;
    }
}

/* Network field types: the asks of the mods taken out or replaced go, the change's modnet asks come (their mods' new
 * entries), and every type either names takes its new width (nettypes.c). */
static void TxnNetApply(void)
{
    for (int i = 0; i < g_txn.modCount; i++) if (g_txn.mods[i].action != TXN_ADD && g_txn.mods[i].old != NULL) NetForget(g_txn.mods[i].old);
    AcquireSRWLockExclusive(&g_netLock);
    for (int i = 0; i < g_txn.netCount; i++)
    {
        const TxnNet* n = &g_txn.nets[i];
        if (g_txn.mods[n->mod].mod != NULL) NetKeep(g_txn.mods[n->mod].mod, n->name, n->kind, n->expectBits, n->min, n->bits);
    }
    for (int i = 0; i < g_txn.netCount; i++) NetApply(g_txn.nets[i].name);
    ReleaseSRWLockExclusive(&g_netLock);
}

/* Game data: the overrides of the mods taken out or replaced go, the change's are set (live_data.c). */
static void TxnDataApply(void)
{
    const char* gone[TXN_MODS];
    int goneCount = 0;
    for (int i = 0; i < g_txn.modCount; i++) if (g_txn.mods[i].action != TXN_ADD) gone[goneCount++] = g_txn.mods[i].id;
    if (goneCount > 0 || g_txn.resCount > 0) HotApply(gone, goneCount, g_txn.res, g_txn.resCount);
}

/* The change's results for the backend, and the change done. refused: "<reason>\t<why>", or NULL when it was made. */
static void TxnFinish(const char* refused)
{
    char line[640];
    int failed = 0;
    if (refused != NULL)
    {
        snprintf(line, sizeof line, "modresult\t%s\trefused\t%s", g_txn.id, refused);
        LiveQueue(line);
        Note("live mod change %s refused: %s", g_txn.id, refused);
    }
    else
    {
        for (int i = 0; i < g_txn.modCount; i++)
        {
            const TxnMod* t = &g_txn.mods[i];
            failed |= t->failed;
            snprintf(line, sizeof line, "modstate\t%s\t%s\t%s\t%d\t%d", g_txn.id, t->id, kTxnActions[t->action], t->applied, t->refused);
            LiveQueue(line);
            Note("live mod change %s: %s %s, %d stats edit(s) applied, %d refused%s", g_txn.id, t->id, kTxnActions[t->action], t->applied, t->refused,
                t->failed ? "; something of its old version may be left" : "");
        }
        snprintf(line, sizeof line, failed ? "modresult\t%s\tfailed\tsomething of a mod taken out may be left; restart the session to be sure" :
            "modresult\t%s\tok", g_txn.id);
        LiveQueue(line);
    }
    TxnReset();
    InterlockedExchange(&g_txn.committing, 0);
}

/* Stat lines whose field was not found: those naming an entity the change registered find it now. */
static void TxnStatsResolve(void)
{
    for (int i = 0; i < g_txn.statCount; i++)
    {
        TxnStat* s = &g_txn.stats[i];
        int later = 0;
        const char* refused = NULL;
        if (s->at == 0) s->at = TxnStatAddress(&s->edit, &later, &refused);
    }
}

/* The gate again, the stats layer's checks and the content's numbers, under both locks, then (when they pass) the parts
 * that change the game: the content, code mods taken out, the stats layer, the set-ups' load events,
 * HF_EVENT_MODS_CHANGED. */
static const char* TxnMake(char* why, size_t size)
{
    LiveTakeoversWait(); /* off the locks: game.dll's code may still be decoding */
    AcquireSRWLockExclusive(&g_fieldsLock);
    AcquireSRWLockExclusive(&g_statsLock);
    const char* refused = TxnGate(); /* again: a friend may have joined meanwhile */
    if (refused == NULL) refused = TxnStatsCheck(why, size);
    if (refused == NULL) refused = LiveNumbersCheck(why, size);
    ReleaseSRWLockExclusive(&g_statsLock);
    ReleaseSRWLockExclusive(&g_fieldsLock);
    if (refused != NULL) return refused;
    int contentFailed = 0;
    if ((refused = LiveContentMake(why, size, &contentFailed)) != NULL) return refused;
    for (int i = 0; i < g_txn.modCount && contentFailed; i++) g_txn.mods[i].failed = 1;
    TxnStatsResolve();
    TxnCodeRemove();
    AcquireSRWLockExclusive(&g_fieldsLock);
    AcquireSRWLockExclusive(&g_statsLock);
    TxnApplyMods();
    TxnStatsApply();
    ReleaseSRWLockExclusive(&g_statsLock);
    ReleaseSRWLockExclusive(&g_fieldsLock);
    StatusRefresh(); /* mod_status.c: a mod taken out leaves the launcher's status list */
    TxnNetApply();
    TxnDataApply();
    int order[TXN_MODS], count = 0;
    TxnCodeOrder(order, &count);
    for (int k = 0; k < count; k++) CodeReplay(g_txn.mods[order[k]].mod);
    Deliver(HF_EVENT_MODS_CHANGED);
    return NULL;
}

/* The change itself, on its own thread. */
static DWORD WINAPI TxnWorker(void* parameter)
{
    (void)parameter;
    char why[512];
    const char* refused = TxnCheck(why, sizeof why);
    if (refused == NULL) refused = LiveContentBegin(why, sizeof why);
    if (refused == NULL) refused = TxnStatsPrepare(why, sizeof why);
    if (refused == NULL) refused = HotPrepare(g_txn.data, g_txn.dataCount, g_txn.res, g_txn.resCount, why, sizeof why);
    if (refused == NULL) refused = TxnCodeStage(why, sizeof why);
    if (refused == NULL) refused = TxnMake(why, sizeof why);
    if (refused != NULL) TxnUnstage();
    TxnFinish(refused);
    return 0;
}

static void TxnCommit(const char* seq, char* reply, size_t size)
{
    InterlockedExchange(&g_txn.committing, 1);
    snprintf(reply, size, "%s\tok\tqueued", seq);
    Note("live mod change %s: committed; it is made on its own thread", g_txn.id);
    if (g_txnInline) { TxnWorker(NULL); return; }
    HANDLE thread = CreateThread(NULL, 0, TxnWorker, NULL, 0, NULL);
    if (thread != NULL) { CloseHandle(thread); return; }
    TxnFinish("capacity\tthe loader could not start the change's thread");
}

static void TxnBeginLine(char** f, int n, char* reply, size_t size)
{
    const char *seq = f[1], *txn = f[2];
    if (g_txn.open) { snprintf(reply, size, "%s\trefused\tbusy\tanother change (%s) is under way", seq, g_txn.id); return; }
    if (n != 5 || strlen(txn) >= sizeof g_txn.id || strlen(f[4]) >= sizeof g_txn.digest || (strcmp(f[3], "solo") && strcmp(f[3], "sync")))
    { TxnRefuse(reply, size, seq, "malformed", "modbegin"); return; }
    const char* gate = TxnGate();
    if (gate != NULL)
    {
        char reason[16];
        snprintf(reason, sizeof reason, "%.*s", (int)(strchr(gate, '\t') - gate), gate);
        TxnRefuse(reply, size, seq, reason, strchr(gate, '\t') + 1);
        return;
    }
    TxnReset();
    g_txn.open = 1;
    g_txn.sync = !strcmp(f[3], "sync");
    snprintf(g_txn.id, sizeof g_txn.id, "%s", txn);
    snprintf(g_txn.digest, sizeof g_txn.digest, "%s", f[4]);
    Note("live mod change %s begins (%s): the mods' digest becomes %s", txn, f[3], f[4]);
    snprintf(reply, size, "%s\tok\tnow", seq);
}

static void TxnOrderLine(char** f, int n, char* reply, size_t size)
{
    char* end;
    unsigned long index = n == 5 ? strtoul(f[3], &end, 10) : 0;
    if (n != 5 || *end || index != (unsigned long)g_txn.orderCount || index >= MAX_MODS || strlen(f[4]) >= sizeof g_txn.order[0])
    { TxnRefuse(reply, size, f[1], "malformed", "modorder"); return; }
    snprintf(g_txn.order[g_txn.orderCount++], sizeof g_txn.order[0], "%s", f[4]);
    snprintf(reply, size, "%s\tok\tnow", f[1]);
}

static void TxnModLine(const char* verb, char** f, int n, char* reply, size_t size)
{
    const char* seq = f[1];
    int remove = !strcmp(verb, "modremove");
    if (remove ? n != 4 : n != 8) { TxnRefuse(reply, size, seq, "malformed", verb); return; }
    if (strlen(f[3]) >= sizeof g_txn.mods[0].id || TxnFind(f[3]) != NULL) { TxnRefuse(reply, size, seq, "malformed", "a mod named twice"); return; }
    if (g_txn.modCount == TXN_MODS) { TxnRefuse(reply, size, seq, "capacity", "a change holds 64 mods at most"); return; }
    unsigned kinds = remove ? 0 : TxnKinds(f[4]);
    if (!remove && (kinds == 0 || strlen(f[5]) != 16 || strlen(f[6]) >= sizeof g_txn.mods[0].folder || strlen(f[7]) >= sizeof g_txn.mods[0].dll ||
        ((kinds & TXN_CODE) != 0) != (strcmp(f[7], "-") != 0)))
    { TxnRefuse(reply, size, seq, "malformed", verb); return; }
    TxnMod* t = &g_txn.mods[g_txn.modCount++];
    memset(t, 0, sizeof *t);
    snprintf(t->id, sizeof t->id, "%s", f[3]);
    t->action = !strcmp(verb, "modadd") ? TXN_ADD : remove ? TXN_REMOVE : TXN_REPLACE;
    t->kinds = kinds;
    if (!remove)
    {
        snprintf(t->hash, sizeof t->hash, "%s", f[5]);
        snprintf(t->folder, sizeof t->folder, "%s", f[6]);
        snprintf(t->dll, sizeof t->dll, "%s", f[7]);
    }
    snprintf(reply, size, "%s\tok\tnow", seq);
}

/* modstat: f[3] the mod, then the plan's stat fields. */
static void TxnStatLine(char** f, int n, char* reply, size_t size)
{
    TxnMod* t = n == 16 ? TxnFind(f[3]) : NULL;
    if (t == NULL || t->action == TXN_REMOVE || !(t->kinds & TXN_STATS)) { TxnRefuse(reply, size, f[1], "malformed", "modstat for no mod added or replaced with stats"); return; }
    StatEdit e;
    if (StatsParseInto(f + 2, 14, &e)) { TxnRefuse(reply, size, f[1], "malformed", "modstat"); return; }
    if (g_txn.statCount == g_txn.statCapacity)
    {
        int capacity = g_txn.statCapacity ? g_txn.statCapacity * 2 : 256;
        TxnStat* grown = capacity <= MAX_STATS * 2 ? (TxnStat*)realloc(g_txn.stats, (size_t)capacity * sizeof *grown) : NULL;
        if (grown == NULL) { TxnRefuse(reply, size, f[1], "capacity", "too many stats edits"); return; }
        g_txn.stats = grown;
        g_txn.statCapacity = capacity;
    }
    g_txn.stats[g_txn.statCount++] = (TxnStat){e, (int)(t - g_txn.mods), 0};
    snprintf(reply, size, "%s\tok\tnow", f[1]);
}

/* modnet: f[3] the mod, then the type's name, kind, expected width and minimum, and the width it asks. */
static void TxnNetLine(char** f, int n, char* reply, size_t size)
{
    TxnMod* t = n == 9 ? TxnFind(f[3]) : NULL;
    char* end = "";
    long kind = t ? strtol(f[5], &end, 10) : -1, expect = *end ? -1 : strtol(f[6], &end, 10), min = *end ? 0 : strtol(f[7], &end, 10),
        bits = *end ? -1 : strtol(f[8], &end, 10);
    if (t == NULL || t->action == TXN_REMOVE || !(t->kinds & TXN_NET) || *end || kind < 0 || kind > 255 || expect < 0 || expect > 32 ||
        strlen(f[4]) >= sizeof g_txn.nets[0].name || NetAskRefused((uint8_t)kind, (uint8_t)expect, (int32_t)bits) != NULL)
    { TxnRefuse(reply, size, f[1], "malformed", "modnet"); return; }
    if (g_txn.netCount == TXN_NETS) { TxnRefuse(reply, size, f[1], "capacity", "a change holds 128 network field types at most"); return; }
    TxnNet* net = &g_txn.nets[g_txn.netCount++];
    *net = (TxnNet){(int)(t - g_txn.mods), "", (uint8_t)kind, (uint8_t)expect, (int32_t)min, (int32_t)bits};
    snprintf(net->name, sizeof net->name, "%s", f[4]);
    snprintf(reply, size, "%s\tok\tnow", f[1]);
}

/* A data line's owner: "-" (the session's) or a mod the change adds or replaces with data. */
static int TxnDataOwner(const char* owner)
{
    const TxnMod* t = strcmp(owner, "-") ? TxnFind(owner) : NULL;
    return !strcmp(owner, "-") || (t != NULL && t->action != TXN_REMOVE && (t->kinds & TXN_DATA));
}

/* moddata <owner> <package>, modres <owner> <type> <name> <target>. */
static void TxnDataLine(int res, char** f, int n, char* reply, size_t size)
{
    uint64_t a = 0, b = 0, c = 0;
    int ok = n == (res ? 7 : 5) && strlen(f[3]) < sizeof g_txn.data[0].owner && TxnDataOwner(f[3]) && HotHex(f[4], &a) &&
        (!res || (HotHex(f[5], &b) && HotHex(f[6], &c)));
    if (!ok) { TxnRefuse(reply, size, f[1], "malformed", res ? "modres" : "moddata"); return; }
    if (res ? g_txn.resCount == HOT_OVERRIDES : g_txn.dataCount == HOT_PACKAGES)
    { TxnRefuse(reply, size, f[1], "capacity", res ? "a change holds 4096 overrides at most" : "a change holds 64 packages at most"); return; }
    if (res)
    {
        TxnRes* r = &g_txn.res[g_txn.resCount++];
        *r = (TxnRes){"", a, b, c};
        snprintf(r->owner, sizeof r->owner, "%s", f[3]);
    }
    else
    {
        TxnData* d = &g_txn.data[g_txn.dataCount++];
        *d = (TxnData){"", a};
        snprintf(d->owner, sizeof d->owner, "%s", f[3]);
    }
    snprintf(reply, size, "%s\tok\tnow", f[1]);
}

/* One of the change's lines (its verb starts with "mod"): 1 when it was one, with the reply written. */
static int LiveModsRequest(char* line, char* reply, size_t size)
{
    char* f[20];
    char copy[8192];
    snprintf(copy, sizeof copy, "%s", line);
    int n = Split(copy, f, 20);
    if (n < 3) return 0;
    const char *verb = f[0], *seq = f[1], *txn = f[2];
    int begin = !strcmp(verb, "modbegin"), order = !strcmp(verb, "modorder"), stat = !strcmp(verb, "modstat"), commit = !strcmp(verb, "modcommit"),
        abort = !strcmp(verb, "modabort"), mod = !strcmp(verb, "modadd") || !strcmp(verb, "modreplace") || !strcmp(verb, "modremove"),
        net = !strcmp(verb, "modnet"), data = !strcmp(verb, "moddata"), res = !strcmp(verb, "modres");
    if (!(begin || order || stat || commit || abort || mod || net || data || res)) return 0;
    if (g_txn.committing) { snprintf(reply, size, "%s\trefused\tbusy\tchange %s is being made", seq, g_txn.id); return 1; }
    if (begin) { TxnBeginLine(f, n, reply, size); return 1; }
    if (!g_txn.open || strcmp(txn, g_txn.id)) { snprintf(reply, size, "%s\trefused\tmalformed\tno change %s is under way", seq, txn); return 1; }
    if (abort) { LiveModsAbort("the backend withdrew it"); snprintf(reply, size, "%s\tok\tnow", seq); return 1; }
    if (commit) TxnCommit(seq, reply, size);
    else if (order) TxnOrderLine(f, n, reply, size);
    else if (mod) TxnModLine(verb, f, n, reply, size);
    else if (net) TxnNetLine(f, n, reply, size);
    else if (data || res) TxnDataLine(res, f, n, reply, size);
    else TxnStatLine(f, n, reply, size);
    return 1;
}
