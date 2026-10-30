/* The mod channel (SDK 16, docs/modding/concepts.md "Co-op"): what the same mod on every machine of a lobby sends each
 * other, and the host's shared table. Included by hellforge_modloader.c after live.c (its lines go over the live pipe and
 * the session's role is there). The game functions are the reviewed pins abilities.c and behaviours.c call: 0xFD9BA0 for
 * the entity a network id names, and an entity's descriptor (+0x14 bit 0: the authority every component manager reads).
 *
 * The table is per mod on every machine: keys are the mod's own, values at most HF_SHARE_VALUE_MAX bytes, at most
 * HF_SHARE_ROWS rows. The host sets rows (share_set); its backend sends them to every machine, and a late joiner gets the
 * whole table. Joined machines may read it and receive HF_ON_SHARE as it changes. Rows go when the host removes them, when
 * the backend sends sharereset (the lobby changed) or when the pipe goes.
 *
 * Live-pipe lines (live.c's header; 1024-byte messages are hex, so a line is longer than a reply):
 *   loader:   send <mod id> <all|host|peer hex> <data hex>
 *             share <mod id> <key hex> <value hex or ->
 *   backend:  message <seq> <mod id> <from hex> <data hex>
 *             share <seq> <mod id> <key hex> <value hex or ->
 *             sharereset <seq>
 * A line queued while the pipe is down is dropped (send returns 1); the host's backend sends the table again when the
 * game connects. Messages are at most HF_MESSAGE_MAX bytes and 64 a second per mod. */

#define CHANNEL_RATE 64u          /* messages a second a mod may send */
#define CHANNEL_REFILL_MS 16u     /* a message's share of a second, in whole milliseconds */
#define CHANNEL_MISSING_MAX 16    /* mod ids logged as absent at most (once each) */

typedef struct ChannelRow
{
    uint64_t key;
    uint32_t bytes;
    uint8_t value[HF_SHARE_VALUE_MAX];
} ChannelRow;

typedef struct ChannelMod
{
    HfMod* mod;
    uint32_t count;
    uint32_t tokens;              /* messages this mod may send now */
    ULONGLONG refilled;
    ChannelRow rows[HF_SHARE_ROWS];
} ChannelMod;

static SRWLOCK g_channelLock = SRWLOCK_INIT;
static ChannelMod* g_channels[MAX_MODS];
static volatile LONG g_channelPipeDown; /* a dropped line was logged since the game connected */
static uintptr_t g_channelGame;
static int g_channelState;              /* 0 waiting for the pins, 1 ready, -1 refused */
static ULONGLONG g_channelFirstTry;
static char g_channelMissing[CHANNEL_MISSING_MAX][80];
static int g_channelMissingCount;

/* `bytes` bytes as lowercase hex, NUL-terminated. */
static void ChannelHex(const uint8_t* data, uint32_t bytes, char* out)
{
    static const char digits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < bytes; i++)
    {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    out[bytes * 2] = 0;
}

/* The bytes a field of hex digits names (none for an empty field); 1 when it is, all its digits and at most capacity. */
static int ChannelUnhex(const char* text, uint8_t* out, uint32_t capacity, uint32_t* bytes)
{
    size_t length = strlen(text);
    if (length % 2 != 0 || length / 2 > capacity) return 0;
    for (size_t i = 0; i < length; i += 2)
    {
        int high = Hex(text[i]), low = Hex(text[i + 1]);
        if (high < 0 || low < 0) return 0;
        out[i / 2] = (uint8_t)(high << 4 | low);
    }
    *bytes = (uint32_t)(length / 2);
    return 1;
}

/* A number of at most `digits` hex digits; 1 when it is. */
static int ChannelNumber(const char* text, size_t digits, uint64_t* out)
{
    size_t length = strlen(text);
    if (length == 0 || length > digits) return 0;
    uint64_t value = 0;
    for (size_t i = 0; i < length; i++)
    {
        int digit = Hex(text[i]);
        if (digit < 0) return 0;
        value = value << 4 | (uint64_t)digit;
    }
    *out = value;
    return 1;
}

/* The channel table of a mod, made on first use; NULL when it cannot be made or the pointer is not a session mod. */
static ChannelMod* ChannelFor(HfMod* mod)
{
    int index = ModLoadIndex(mod);
    if (index >= MAX_MODS) return NULL;
    AcquireSRWLockExclusive(&g_channelLock);
    ChannelMod* c = g_channels[index];
    if (c == NULL)
    {
        c = calloc(1, sizeof *c);
        if (c != NULL)
        {
            c->mod = mod;
            c->tokens = CHANNEL_RATE;
            c->refilled = GetTickCount64();
            g_channels[index] = c;
        }
    }
    ReleaseSRWLockExclusive(&g_channelLock);
    if (c == NULL) ApiLog(mod, "the mod channel has no memory for its table");
    return c;
}

/* The loaded mod of a line's id, or NULL. */
static HfMod* ChannelModId(const char* id)
{
    for (int i = 0; i < g_modCount; i++) if (g_mods[i].loaded && !strcmp(g_mods[i].id, id)) return &g_mods[i];
    return NULL;
}

/* A line for a mod this game does not load: dropped, and said once for that id. */
static void ChannelMissing(const char* id)
{
    int fresh = 0;
    AcquireSRWLockExclusive(&g_channelLock);
    int known = 0;
    for (int i = 0; i < g_channelMissingCount; i++) if (!strcmp(g_channelMissing[i], id)) known = 1;
    if (!known)
    {
        int at = g_channelMissingCount < CHANNEL_MISSING_MAX ? g_channelMissingCount++ : CHANNEL_MISSING_MAX - 1;
        snprintf(g_channelMissing[at], sizeof g_channelMissing[at], "%s", id);
        fresh = 1;
    }
    ReleaseSRWLockExclusive(&g_channelLock);
    if (fresh) Note("channel: a line names %s, which this game does not load; dropped", id);
}

static ChannelRow* ChannelRowFind(ChannelMod* c, uint64_t key)
{
    for (uint32_t i = 0; i < c->count; i++) if (c->rows[i].key == key) return &c->rows[i];
    return NULL;
}

/* Sets (bytes > 0) or removes (0) a row; *changed when the table changed. 1 when the table is full and the key is new.
 * Called with g_channelLock held. */
static int ChannelRowSet(ChannelMod* c, uint64_t key, const uint8_t* value, uint32_t bytes, int* changed)
{
    ChannelRow* row = ChannelRowFind(c, key);
    *changed = 0;
    if (bytes == 0)
    {
        if (row == NULL) return 0;
        *row = c->rows[--c->count];
        *changed = 1;
        return 0;
    }
    if (row != NULL)
    {
        *changed = row->bytes != bytes || memcmp(row->value, value, bytes) != 0;
        if (*changed) { row->bytes = bytes; memcpy(row->value, value, bytes); }
        return 0;
    }
    if (c->count == HF_SHARE_ROWS) return 1;
    row = &c->rows[c->count++];
    row->key = key;
    row->bytes = bytes;
    memcpy(row->value, value, bytes);
    *changed = 1;
    return 0;
}

/* The row's HF_ON_SHARE, when a mod listens (after the lock: a listener may read the table). */
static void ChannelShareEvent(HfMod* mod, uint64_t key, const ChannelRow* row)
{
    if (!AnyListener(HF_ON_SHARE)) return;
    HfShareEvent e = {{sizeof e, HF_ON_SHARE, 0}, key, row->bytes, {0}};
    if (row->bytes > 0) memcpy(e.value, row->value, row->bytes);
    RaiseTo(&e.info, mod);
}

/* One of a mod's 64 messages this second; 0 when it has none left. */
static int ChannelTakeToken(ChannelMod* c)
{
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_channelLock);
    ULONGLONG elapsed = now - c->refilled;
    if (elapsed >= CHANNEL_REFILL_MS)
    {
        uint64_t add = elapsed * CHANNEL_RATE / 1000;
        if (add > 0)
        {
            c->tokens = (uint32_t)(c->tokens + (add > CHANNEL_RATE ? CHANNEL_RATE : add));
            if (c->tokens > CHANNEL_RATE) c->tokens = CHANNEL_RATE;
            c->refilled = now;
        }
    }
    int taken = c->tokens > 0;
    if (taken) c->tokens--;
    ReleaseSRWLockExclusive(&g_channelLock);
    return taken;
}

/* The backend did not take a line: said once per connection. */
static void ChannelLineDropped(HfMod* mod)
{
    if (!InterlockedExchange(&g_channelPipeDown, 1))
        ApiLog(mod, "the backend is not connected (or its queue is full): the line is dropped; the host's backend sends the shared table again when the game reconnects");
}

/* A row set or removed here, for the backend (unasked; nothing in solo, with no backend, is an error). */
static void ChannelQueueShare(HfMod* mod, uint64_t key, const uint8_t* value, uint32_t bytes)
{
    char hex[HF_SHARE_VALUE_MAX * 2 + 1];
    if (bytes > 0) ChannelHex(value, bytes, hex); else hex[0] = 0;
    char line[128 + sizeof hex];
    int length = snprintf(line, sizeof line, "share\t%s\t%016llx\t%s\n", mod->id, (unsigned long long)key, bytes > 0 ? hex : "-");
    if (length <= 0 || (size_t)length >= sizeof line || !LiveQueue(line, length)) ChannelLineDropped(mod);
}

/* A message this mod sends, for the backend. 0 queued, 1 refused (the backend is gone). */
static int ChannelQueueSend(HfMod* mod, uint64_t to, const uint8_t* data, uint32_t bytes)
{
    char hex[HF_MESSAGE_MAX * 2 + 1];
    ChannelHex(data, bytes, hex);
    char line[160 + sizeof hex];
    int length;
    if (to == HF_TO_ALL) length = snprintf(line, sizeof line, "send\t%s\tall\t%s\n", mod->id, hex);
    else if (to == HF_TO_HOST) length = snprintf(line, sizeof line, "send\t%s\thost\t%s\n", mod->id, hex);
    else length = snprintf(line, sizeof line, "send\t%s\t%016llx\t%s\n", mod->id, (unsigned long long)to, hex);
    if (length <= 0 || (size_t)length >= sizeof line || !LiveQueue(line, length)) { ChannelLineDropped(mod); return 1; }
    return 0;
}

/* SDK 16, is_authority: 1 when this machine simulates the entity, 0 when it does not, -1 when there is no such entity
 * (or the game functions are not the reviewed ones). */
static int ApiIsAuthority(HfMod* mod, uint32_t entity)
{
    (void)mod;
    if (g_channelState != 1) return -1;
    int exists = 0;
    int authority = AblAuthorityAt(g_channelGame, entity, &exists);
    return exists ? authority : -1;
}

/* SDK 16, entity_of_network: the entity a network id names here now (0xFD9BA0), or 0xFFFFFFFF. */
static uint32_t ApiEntityOfNetwork(HfMod* mod, uint32_t network)
{
    (void)mod;
    if (g_channelState != 1 || network == ABL_NO_HANDLE) return 0xFFFFFFFFu;
    /* the game's handle of none resolves to its no-entity id, which the SDK reports as 0xFFFFFFFF */
    uint32_t none = 0xFFFFFFFFu, noneOfHandle = 0;
    if (SafeRead(g_channelGame + ABL_NO_ENTITY, &none, 4) || SafeRead(g_channelGame + ABL_HANDLE_NONE, &noneOfHandle, 4) || none != noneOfHandle)
        none = 0xFFFFFFFFu;
    uint32_t entity = none; /* as abilities.c's EntityOf: the game writes it, and none when it does not */
    ((HandleEntityFn)(g_channelGame + kLaunchFunctions[L_HANDLE].rva))(&entity, network);
    return entity == none ? 0xFFFFFFFFu : entity;
}

/* SDK 16, send: a message to the same mod on other machines. 0 queued, 1 refused (solo, no backend, too long or too
 * many in a second; the mod's log says why). */
static int ApiSend(HfMod* mod, uint64_t to, const void* data, uint32_t bytes)
{
    if (mod == NULL) return 1;
    if (bytes > HF_MESSAGE_MAX || (bytes > 0 && data == NULL))
    { ApiLog(mod, "send refused: %u bytes (at most %u)", bytes, HF_MESSAGE_MAX); return 1; }
    uint32_t role;
    AcquireSRWLockShared(&g_sessionLock);
    role = g_session.role;
    ReleaseSRWLockShared(&g_sessionLock);
    if (role == HF_SESSION_SOLO) { ApiLog(mod, "send refused: solo (there is no other machine)"); return 1; }
    ChannelMod* c = ChannelFor(mod);
    if (c == NULL) return 1;
    if (!ChannelTakeToken(c)) { ApiLog(mod, "send refused: more than %u messages a second", CHANNEL_RATE); return 1; }
    return ChannelQueueSend(mod, to, data, bytes);
}

/* SDK 16, share_set: sets (bytes > 0) or removes (0) one of the host's rows. 0 done, 1 refused (a joined machine, too
 * big, the table is full: the log says why). */
static int ApiShareSet(HfMod* mod, uint64_t key, const void* value, uint32_t bytes)
{
    if (mod == NULL) return 1;
    if (bytes > HF_SHARE_VALUE_MAX || (bytes > 0 && value == NULL))
    { ApiLog(mod, "share_set refused: %u bytes (at most %u)", bytes, HF_SHARE_VALUE_MAX); return 1; }
    uint32_t role;
    AcquireSRWLockShared(&g_sessionLock);
    role = g_session.role;
    ReleaseSRWLockShared(&g_sessionLock);
    if (role == HF_SESSION_JOINER) { ApiLog(mod, "share_set refused: only the host sets the shared table"); return 1; }
    ChannelMod* c = ChannelFor(mod);
    if (c == NULL) return 1;
    int changed = 0, full;
    ChannelRow row;
    memset(&row, 0, sizeof row);
    AcquireSRWLockExclusive(&g_channelLock);
    full = ChannelRowSet(c, key, value, bytes, &changed);
    if (!full && changed && bytes > 0) { ChannelRow* found = ChannelRowFind(c, key); if (found != NULL) row = *found; }
    ReleaseSRWLockExclusive(&g_channelLock);
    if (full) { ApiLog(mod, "share_set refused: the table holds %u rows", HF_SHARE_ROWS); return 1; }
    if (!changed) return 0;
    ChannelShareEvent(mod, key, &row);
    ChannelQueueShare(mod, key, bytes > 0 ? (const uint8_t*)value : NULL, bytes);
    return 0;
}

/* SDK 16, share_get: the row's size (0 none), up to capacity bytes copied. */
static uint32_t ApiShareGet(HfMod* mod, uint64_t key, void* value, uint32_t capacity)
{
    if (mod == NULL) return 0;
    ChannelMod* c = ChannelFor(mod);
    if (c == NULL) return 0;
    uint32_t bytes = 0;
    AcquireSRWLockShared(&g_channelLock);
    ChannelRow* row = ChannelRowFind(c, key);
    if (row != NULL)
    {
        bytes = row->bytes;
        if (value != NULL && capacity > 0) memcpy(value, row->value, bytes < capacity ? bytes : capacity);
    }
    ReleaseSRWLockShared(&g_channelLock);
    return bytes;
}

/* SDK 16, share_row: the index-th row (any order). 1 with *key, the value and *bytes filled; 0 past the last. */
static int ApiShareRow(HfMod* mod, uint32_t index, uint64_t* key, void* value, uint32_t capacity, uint32_t* bytes)
{
    if (mod == NULL || key == NULL || bytes == NULL) return 0;
    ChannelMod* c = ChannelFor(mod);
    if (c == NULL) return 0;
    int found = 0;
    AcquireSRWLockShared(&g_channelLock);
    if (index < c->count)
    {
        const ChannelRow* row = &c->rows[index];
        *key = row->key;
        *bytes = row->bytes;
        if (value != NULL && capacity > 0) memcpy(value, row->value, row->bytes < capacity ? row->bytes : capacity);
        found = 1;
    }
    ReleaseSRWLockShared(&g_channelLock);
    return found;
}

/* Drops every row of every mod, raising HF_ON_SHARE for each (bytes 0), as sharereset does. */
static void ChannelReset(void)
{
    ChannelMod* channels[MAX_MODS];
    AcquireSRWLockShared(&g_channelLock);
    memcpy(channels, g_channels, sizeof channels);
    ReleaseSRWLockShared(&g_channelLock);
    for (int m = 0; m < MAX_MODS; m++)
    {
        ChannelMod* c = channels[m];
        if (c == NULL) continue;
        for (;;)
        {
            ChannelRow row;
            int have;
            AcquireSRWLockExclusive(&g_channelLock);
            have = c->count > 0;
            if (have) row = c->rows[--c->count];
            ReleaseSRWLockExclusive(&g_channelLock);
            if (!have) break;
            row.bytes = 0;
            ChannelShareEvent(c->mod, row.key, &row);
        }
    }
}

/* The game connected: the drop notice may be said again, and its mods may send their 64 a second from now. */
static void ChannelConnected(void)
{
    InterlockedExchange(&g_channelPipeDown, 0);
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_channelLock);
    for (int i = 0; i < MAX_MODS; i++)
        if (g_channels[i] != NULL) { g_channels[i]->tokens = CHANNEL_RATE; g_channels[i]->refilled = now; }
    ReleaseSRWLockExclusive(&g_channelLock);
}

/* The game's pipe went: its rows are the lobby's and the lobby is over for this game. */
static void ChannelDisconnected(void)
{
    ChannelReset();
    ChannelConnected();
}

/* Once the game's code has decoded: the reviewed functions the channel calls (the pins abilities.c and behaviours.c
 * use). Nothing is hooked. */
static void ChannelInstall(uintptr_t game)
{
    if (g_channelState != 0 || game == 0) return;
    if (strcmp(g_build, "25480438")) { g_channelState = -1; Note("channel: game build %s is not reviewed; no mod channel", g_build); return; }
    ULONGLONG now = GetTickCount64();
    if (g_channelFirstTry == 0) g_channelFirstTry = now;
    const GameFunction* handle = &kLaunchFunctions[L_HANDLE];
    const GameFunction* descriptor = &kBhvFunctions[F_DESCRIPTOR];
    if (!PinMatches(game + handle->rva, handle->pin, sizeof handle->pin) || !PinMatches(game + descriptor->rva, descriptor->pin, sizeof descriptor->pin))
    {
        if (now - g_channelFirstTry < 60000) return;
        g_channelState = -1;
        Note("channel: the network handle and descriptor functions are not the reviewed ones; no mod channel");
        return;
    }
    g_channelGame = game;
    g_channelState = 1;
    Note("channel: the network handle and descriptor functions are reviewed (0x%x, 0x%x)", (unsigned)handle->rva, (unsigned)descriptor->rva);
}

/* message <seq> <mod id> <from hex> <data hex> */
static void ChannelMessageRequest(char* line, char* reply, size_t size)
{
    char* f[8];
    int n = SplitAll(line, f, 8);
    if (n != 5) { snprintf(reply, size, "%s\trefused\tmalformed message", n >= 2 ? f[1] : "-"); return; }
    uint64_t from = 0;
    uint32_t bytes = 0;
    uint8_t data[HF_MESSAGE_MAX];
    if (!ChannelNumber(f[3], 16, &from) || !ChannelUnhex(f[4], data, HF_MESSAGE_MAX, &bytes))
    { snprintf(reply, size, "%s\trefused\tmalformed message", f[1]); return; }
    HfMod* mod = ChannelModId(f[2]);
    if (mod == NULL) { ChannelMissing(f[2]); snprintf(reply, size, "%s\tok\tnow", f[1]); return; }
    if (AnyListener(HF_ON_MESSAGE))
    {
        HfMessageEvent e = {{sizeof e, HF_ON_MESSAGE, 0}, from, bytes, {0}};
        if (bytes > 0) memcpy(e.data, data, bytes);
        RaiseTo(&e.info, mod);
    }
    snprintf(reply, size, "%s\tok\tnow", f[1]);
}

/* share <seq> <mod id> <key hex> <value hex or -> */
static void ChannelShareRequest(char* line, char* reply, size_t size)
{
    char* f[8];
    int n = SplitAll(line, f, 8);
    if (n != 5) { snprintf(reply, size, "%s\trefused\tmalformed share", n >= 2 ? f[1] : "-"); return; }
    uint64_t key = 0;
    uint32_t bytes = 0;
    uint8_t value[HF_SHARE_VALUE_MAX] = {0};
    int removal = !strcmp(f[4], "-");
    if (!ChannelNumber(f[3], 16, &key) || (!removal && !ChannelUnhex(f[4], value, HF_SHARE_VALUE_MAX, &bytes)))
    { snprintf(reply, size, "%s\trefused\tmalformed share", f[1]); return; }
    HfMod* mod = ChannelModId(f[2]);
    if (mod == NULL) { ChannelMissing(f[2]); snprintf(reply, size, "%s\tok\tnow", f[1]); return; }
    ChannelMod* c = ChannelFor(mod);
    if (c == NULL) { snprintf(reply, size, "%s\trefused\tthe mod channel has no memory for its table", f[1]); return; }
    int changed = 0, full;
    ChannelRow row;
    memset(&row, 0, sizeof row);
    AcquireSRWLockExclusive(&g_channelLock);
    full = ChannelRowSet(c, key, value, removal ? 0 : bytes, &changed);
    if (!full && changed && !removal) { ChannelRow* found = ChannelRowFind(c, key); if (found != NULL) row = *found; }
    ReleaseSRWLockExclusive(&g_channelLock);
    if (full) { snprintf(reply, size, "%s\trefused\tthe table holds %u rows", f[1], HF_SHARE_ROWS); return; }
    if (changed) ChannelShareEvent(mod, key, &row);
    snprintf(reply, size, "%s\tok\tnow", f[1]);
}

/* sharereset <seq> */
static void ChannelResetRequest(char* line, char* reply, size_t size)
{
    char* f[4];
    int n = SplitAll(line, f, 4);
    if (n < 2) { snprintf(reply, size, "-\trefused\tmalformed sharereset"); return; }
    ChannelReset();
    snprintf(reply, size, "%s\tok\tnow", f[1]);
}

/* A line of the channel's over the live pipe (message, share, sharereset): 1 when it was one, answered in reply. */
static int ChannelRequest(char* line, char* reply, size_t size)
{
    if (!strncmp(line, "message\t", 8)) { ChannelMessageRequest(line, reply, size); return 1; }
    if (!strncmp(line, "sharereset\t", 11)) { ChannelResetRequest(line, reply, size); return 1; }
    if (!strncmp(line, "share\t", 6)) { ChannelShareRequest(line, reply, size); return 1; }
    return 0;
}
