/* Network field types (docs/modding/data.md "Native edits in the manifest"): what mods ask of the engine's field types,
 * a mod's networkFieldTypes (the plan's nettype lines) and a code mod's set_network_field_bits (SDK 1), applied once the
 * engine's network configuration is loaded (HF_EVENT_NETWORK_READY) and again whenever a live change adds or takes out
 * a mod that asks (live_mods.c, SDK 16). Included by hellforge_modloader.c after the field type table's readers.
 *
 * Each type is as wide as the widest of the loaded mods' asks whose expectation (kind, width, minimum) is the game's own
 * type, else the game's own width; an ask that expects something else leaves the type alone (logged). Only integer
 * types widen, to at most 31 bits. The width is the network's wire format, so friends must agree on it: the plan's asks
 * apply before any friend can join, a live change only with no friend in the game (its gate). The engine reads a row's
 * width on every encode, decode and clamp; nothing caches it, so a change applies from the next value sent (build
 * 25480438, KB hellforge-live-mod-sync-re-runtime). A narrower width wraps a value already above it when that value is
 * next sent: a live change narrows only on its own ship, before joining or being joined. */

#define NET_ASKS 256
typedef struct NetAsk { HfMod* mod; char name[64]; uint8_t kind, expectBits, bits; int32_t min; } NetAsk;
static NetAsk g_netAsks[NET_ASKS];
static int g_netAskCount;
/* Each type an ask named: its row and the game's own width, read before anything changed it. */
typedef struct NetType { char name[64]; uintptr_t row; uint8_t kind, bits; int32_t min; } NetType;
static NetType g_netTypes[NET_ASKS];
static int g_netTypeCount;
static SRWLOCK g_netLock = SRWLOCK_INIT;

/* The type's row and its game's own width (read once). NULL when the configuration has no such single type. */
static NetType* NetTypeNamed(const char* name)
{
    for (int i = 0; i < g_netTypeCount; i++) if (!strcmp(g_netTypes[i].name, name)) return &g_netTypes[i];
    uintptr_t row = FindFieldType(name);
    uint8_t bytes[0x18];
    if (row == 0 || g_netTypeCount == NET_ASKS || SafeRead(row, bytes, sizeof bytes)) return NULL;
    NetType* t = &g_netTypes[g_netTypeCount++];
    snprintf(t->name, sizeof t->name, "%s", name);
    t->row = row;
    t->kind = bytes[12];
    t->bits = bytes[13];
    memcpy(&t->min, bytes + 16, 4);
    return t;
}

/* The width a type takes from the loaded mods' asks: the widest that expects the game's own type, else that. g_netLock
 * held. */
static uint8_t NetWidth(const NetType* t)
{
    uint8_t width = t->bits;
    for (int i = 0; i < g_netAskCount; i++)
    {
        const NetAsk* a = &g_netAsks[i];
        if (strcmp(a->name, t->name) || !a->mod->loaded || a->mod->removing) continue;
        if (a->kind != t->kind || a->expectBits != t->bits || a->min != t->min) continue;
        if (a->bits > width) width = a->bits;
    }
    return width;
}

/* The type's row written to the width its asks give. 0 when it holds it. g_netLock held. */
static int NetApply(const char* name)
{
    NetType* t = NetTypeNamed(name);
    if (t == NULL) { Note("network field type %s: not in the network configuration", name); return 1; }
    uint8_t want = NetWidth(t), now = 0;
    if (SafeRead(t->row + 13, &now, 1)) return 1;
    if (now == want) return 0;
    if (ApiWrite(t->row + 13, &want, 1)) return 1;
    Note("network field type %s: %u -> %u bits%s", name, now, want, want == t->bits ? " (the game's own)" : "");
    return 0;
}

/* An ask that cannot be honoured whatever the game's type: why, or NULL. */
static const char* NetAskRefused(uint8_t kind, uint8_t expectBits, int32_t bits)
{
    if (kind != 1) return "only an integer type can be widened";
    if (bits <= expectBits || bits > 31) return "a type widens to at most 31 bits, wider than it was";
    return NULL;
}

/* Keeps an ask of mod's (replacing its earlier one of the same type). 0 kept. g_netLock held. */
static int NetKeep(HfMod* mod, const char* name, uint8_t kind, uint8_t expectBits, int32_t min, int32_t bits)
{
    const char* refused = NetAskRefused(kind, expectBits, bits);
    if (refused != NULL || strlen(name) >= sizeof g_netAsks[0].name) { ApiLog(mod, "network field type %s left alone: %s", name, refused ? refused : "its name is too long"); return 1; }
    NetAsk* a = NULL;
    for (int i = 0; i < g_netAskCount && a == NULL; i++) if (g_netAsks[i].mod == mod && !strcmp(g_netAsks[i].name, name)) a = &g_netAsks[i];
    if (a == NULL && g_netAskCount == NET_ASKS) { ApiLog(mod, "network field type %s left alone: mods asked %d changes already", name, NET_ASKS); return 1; }
    if (a == NULL) a = &g_netAsks[g_netAskCount++];
    *a = (NetAsk){mod, "", kind, expectBits, (uint8_t)bits, min};
    snprintf(a->name, sizeof a->name, "%s", name);
    return 0;
}

/* plan: nettype <mod id> <name> <kind> <expect bits> <expect min> <bits> */
static int NetParse(char** f, int n)
{
    if (n != 7) return 1;
    HfMod* mod = NULL;
    for (int i = 0; i < g_modCount; i++) if (!strcmp(g_mods[i].id, f[1])) mod = &g_mods[i];
    char* end;
    long kind = strtol(f[3], &end, 10), expect = *end ? -1 : strtol(f[4], &end, 10), min = *end ? 0 : strtol(f[5], &end, 10),
        bits = *end ? -1 : strtol(f[6], &end, 10);
    if (mod == NULL || *end || kind < 0 || kind > 255 || expect < 0 || expect > 32) return 1;
    AcquireSRWLockExclusive(&g_netLock);
    NetKeep(mod, f[2], (uint8_t)kind, (uint8_t)expect, (int32_t)min, (int32_t)bits); /* a refused ask is logged, the plan stays */
    ReleaseSRWLockExclusive(&g_netLock);
    return 0;
}

/* HF_EVENT_NETWORK_READY: every type an ask names, at its width. */
static void NetApplyAll(void)
{
    AcquireSRWLockExclusive(&g_netLock);
    for (int i = 0; i < g_netAskCount; i++)
    {
        int seen = 0;
        for (int k = 0; k < i && !seen; k++) seen = !strcmp(g_netAsks[k].name, g_netAsks[i].name);
        if (!seen) NetApply(g_netAsks[i].name);
    }
    for (int i = 0; i < g_netAskCount; i++)
    {
        const NetType* t = NetTypeNamed(g_netAsks[i].name);
        if (t != NULL && (g_netAsks[i].kind != t->kind || g_netAsks[i].expectBits != t->bits || g_netAsks[i].min != t->min))
            ApiLog(g_netAsks[i].mod, "network field type %s left alone: the game's is kind %u, %u bits from %d", t->name, t->kind, t->bits, t->min);
    }
    ReleaseSRWLockExclusive(&g_netLock);
}

/* Whether a mod asks for any field type's width (a live change taking it out changes the network schema). */
static int NetAsksOf(const HfMod* mod)
{
    int asks = 0;
    AcquireSRWLockShared(&g_netLock);
    for (int i = 0; i < g_netAskCount && !asks; i++) asks = g_netAsks[i].mod == mod;
    ReleaseSRWLockShared(&g_netLock);
    return mod != NULL && asks;
}

/* A live change: the asks of the mods it takes out go, those of the mods it adds come (net: its modnet asks), and every
 * type either named is written to its new width. */
static void NetForget(const HfMod* mod)
{
    char touched[NET_ASKS][64];
    int count = 0, kept = 0;
    AcquireSRWLockExclusive(&g_netLock);
    for (int i = 0; i < g_netAskCount; i++)
    {
        if (g_netAsks[i].mod != mod) { g_netAsks[kept++] = g_netAsks[i]; continue; }
        snprintf(touched[count++], sizeof touched[0], "%s", g_netAsks[i].name);
    }
    g_netAskCount = kept;
    if (g_fired[HF_EVENT_NETWORK_READY]) for (int i = 0; i < count; i++) NetApply(touched[i]);
    ReleaseSRWLockExclusive(&g_netLock);
}

/* SDK 1, set_network_field_bits: a code mod's ask, kept (so taking the mod out undoes it) and applied. 0 when the type
 * is at least as wide as asked. */
static int ApiSetNetworkFieldBits(HfMod* mod, const char* name, HfFieldType expect, int32_t bits)
{
    if (mod == NULL || name == NULL || ModGone(mod)) return 1;
    AcquireSRWLockExclusive(&g_netLock);
    NetType* t = NetTypeNamed(name);
    int result = 1;
    if (t == NULL) ApiLog(mod, "network field type %s: not found", name);
    else if (expect.kind != t->kind || expect.bits != t->bits || expect.min != t->min)
        ApiLog(mod, "network field type %s left alone: %d bits, expected %d", name, t->bits, expect.bits);
    else if (!NetKeep(mod, name, (uint8_t)expect.kind, (uint8_t)expect.bits, expect.min, bits) && !NetApply(name))
    {
        result = NetWidth(t) >= bits ? 0 : 1;
        ApiLog(mod, "network field type %s: %d -> %d bits", name, expect.bits, NetWidth(t));
    }
    ReleaseSRWLockExclusive(&g_netLock);
    return result;
}
