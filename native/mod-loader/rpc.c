/* The game's RPCs for mods (SDK 17, rpc_listen and rpc_send; docs/modding/networking.md, "How it works"). Included
 * at the top of channel.c (after live.c: the session's role; before live_mods.c: a mod taken out forgets its listeners
 * here). Build 25480438: every RPC this machine gets goes through RpcDispatch (game.dll 0xBC2D10, (u64 sender, u32 hash,
 * u32 index, RpcArg* args, u32 entries), the game thread); HellForge hooks it once a mod listens and runs the listeners
 * before it. RpcSend (game.dll 0xBDE430, (u32 hash, u64 target, RpcArg* args, u32 entries); target a peer, or -2 every
 * other member of the session) trusts its caller: an unknown hash crashes it, a short list is read past, a message over
 * 0x4B0 bytes goes out cut short. rpc_send checks all three against sdk_rpcs.h first. */
#include "sdk_rpcs.h"

#define RPC_DISPATCH 0xBC2D10u
#define RPC_SEND 0xBDE430u
#define RPC_LOCAL_USER 0x347CEF0u  /* the game's network context: +0xB398 this machine's peer, +0xB3A8 the host's */
#define RPC_LOCAL_PEER 0xB398u
#define RPC_HOST_PEER 0xB3A8u
#define RPC_TO_OTHERS 0xFFFFFFFFFFFFFFFEull  /* RpcSend's -2 */
#define RPC_MESSAGE_BITS (0x4B0u * 8u)       /* the game's message buffer */
#define RPC_STRING_MAX 2047u                 /* a string's length goes in 11 bits */
#define RPC_COPY 16                          /* listeners the dispatch copies on its stack */

static const uint8_t kRpcDispatchPin[17] = {0x41, 0x8b, 0xc0, 0x49, 0x8b, 0xd1, 0x4c, 0x8d, 0x05, 0xc3, 0x95, 0x58, 0x01, 0x49, 0xff, 0x24, 0xc0};
static const uint8_t kRpcSendPin[37] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x98, 0x00, 0x00, 0x00, 0x48, 0x8b,
    0x05, 0xc9, 0xdb, 0xa5, 0x01, 0x48, 0x33, 0xc4, 0x48, 0x89, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x45, 0x8b, 0xf9};

typedef void (*RpcDispatchFn)(uint64_t sender, uint32_t hash, uint32_t index, const HfRpcArg* args, uint32_t entries);
typedef void (*RpcSendFn)(uint32_t hash, uint64_t target, const HfRpcArg* args, uint32_t entries);
static RpcDispatchFn g_rpcDispatch;
static int g_rpcState;           /* the dispatch hook: 0 not yet, 1 in, -1 refused */
static int g_rpcSendState;       /* the send pin: 0 not checked, 1 reviewed, -1 not */
static ULONGLONG g_rpcFirstTry;

typedef struct RpcListener { HfMod* mod; uint32_t rpc; HfRpcListener listener; void* user; int32_t priority; uint32_t order; } RpcListener;
static RpcListener* g_rpcListeners;   /* highest priority first, then in the order they listened */
static int g_rpcListenerCount, g_rpcListenerCapacity;
static uint32_t g_rpcListenerOrder;
static SRWLOCK g_rpcLock = SRWLOCK_INIT;
static volatile LONG g_rpcListening;  /* any listener (read on the dispatch's hot path) */
static volatile LONG g_rpcDropsNoted;

/* The RPC of a hash in this build's table, or NULL. */
static const SdkRpc* RpcFind(uint32_t hash)
{
    size_t lo = 0, hi = SDK_RPC_COUNT;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (kSdkRpcs[mid].hash == hash) return &kSdkRpcs[mid];
        if (kSdkRpcs[mid].hash < hash) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

/* The most bits one value of a kind takes on the wire (byte-aligned values with their padding). */
static uint32_t RpcValueBits(uint32_t kind, uint32_t bits, uint32_t size)
{
    switch (kind)
    {
    case HF_RPC_KIND_BOOL: return 1;
    case HF_RPC_KIND_INT: case HF_RPC_KIND_FLOAT: case HF_RPC_KIND_QUATERNION: return bits;
    case HF_RPC_KIND_VECTOR3: return 3 * bits;
    case HF_RPC_KIND_VECTOR2: return 2 * bits;
    case HF_RPC_KIND_DIRECTION: return 1 + 2 * bits;
    case HF_RPC_KIND_STRING: return 11 + 7 + 8 * size;
    case HF_RPC_KIND_U32: return 7 + 32;
    case HF_RPC_KIND_U64: return 7 + 64;
    default: return 0;
    }
}

/* The bits an array's count takes: the bit length of its most elements. */
static uint32_t RpcCountBits(uint32_t max)
{
    uint32_t n = 0;
    while (max) { n++; max >>= 1; }
    return n;
}

/* rpc_send's walk through a list: the entries, the next one, the bits so far, why it stopped. */
typedef struct RpcCheck { const HfRpcArg* args; uint32_t count, at, bits; char* why; size_t whySize; } RpcCheck;

/* One value entry of argument i: of kind, with a value, a string within its 2047 bytes. Adds its bits. */
static int RpcValueMatches(RpcCheck* c, uint32_t kind, uint32_t width, uint32_t i)
{
    const HfRpcArg* arg = &c->args[c->at++];
    if (arg->kind != kind) { snprintf(c->why, c->whySize, "argument %u is kind %u, not %u", i, arg->kind, kind); return 0; }
    if (arg->value == NULL) { snprintf(c->why, c->whySize, "argument %u: no value", i); return 0; }
    uint32_t size = 0;
    if (kind == HF_RPC_KIND_STRING)
    {
        const char* s = (const char*)arg->value;
        while (size <= RPC_STRING_MAX && s[size] != 0) size++;
        if (size > RPC_STRING_MAX) { snprintf(c->why, c->whySize, "argument %u: a string over %u bytes", i, RPC_STRING_MAX); return 0; }
    }
    c->bits += RpcValueBits(kind, width, size);
    return 1;
}

/* Declared argument i at the walk's next entry: one value, or an array's count and that many elements. */
static int RpcArgMatches(RpcCheck* c, const SdkRpcArg* d, uint32_t i)
{
    if (c->at >= c->count) { snprintf(c->why, c->whySize, "argument %u missing (%u given)", i, c->count); return 0; }
    if (d->kind != HF_RPC_KIND_ARRAY) return RpcValueMatches(c, d->kind, d->bits, i);
    if (c->args[c->at].kind != HF_RPC_KIND_ARRAY) { snprintf(c->why, c->whySize, "argument %u is kind %u, not an array", i, c->args[c->at].kind); return 0; }
    uint32_t n = (uint32_t)(uintptr_t)c->args[c->at++].value;
    if (n > d->max) { snprintf(c->why, c->whySize, "argument %u: %u elements, at most %u", i, n, d->max); return 0; }
    c->bits += RpcCountBits(d->max);
    for (uint32_t k = 0; k < n; k++)
    {
        if (c->at >= c->count) { snprintf(c->why, c->whySize, "argument %u: element %u missing", i, k); return 0; }
        if (!RpcValueMatches(c, d->element, d->elementBits, i)) return 0;
    }
    return 1;
}

/* Whether args are rpc's declared arguments, in order, and the message fits the game's buffer; why not in why. */
static int RpcArgsMatch(const SdkRpc* rpc, const HfRpcArg* args, uint32_t count, char* why, size_t whySize)
{
    if (count > 0 && args == NULL) { snprintf(why, whySize, "%u arguments but no list", count); return 0; }
    RpcCheck c = {args, count, 0, 10u + (rpc->sessionBound ? 16u : 0u), why, whySize};
    for (uint32_t i = 0; i < rpc->count; i++)
        if (!RpcArgMatches(&c, &kSdkRpcArgs[rpc->first + i], i)) return 0;
    if (c.at != count) { snprintf(why, whySize, "%u entries, %u expected", count, c.at); return 0; }
    if (c.bits > RPC_MESSAGE_BITS) { snprintf(why, whySize, "%u bytes encoded, over the game's %u", (c.bits + 7) / 8, RPC_MESSAGE_BITS / 8); return 0; }
    return 1;
}

/* The listeners of a hash (its own and HF_RPC_ALL's, of loaded mods) copied into out, in order: their count, or -1. */
static int RpcListenersOf(uint32_t hash, RpcListener* stack, RpcListener** out)
{
    int n = 0;
    AcquireSRWLockShared(&g_rpcLock);
    *out = g_rpcListenerCount > RPC_COPY ? malloc(sizeof **out * g_rpcListenerCount) : stack;
    if (*out != NULL)
        for (int i = 0; i < g_rpcListenerCount; i++)
            if ((g_rpcListeners[i].rpc == hash || g_rpcListeners[i].rpc == HF_RPC_ALL) && g_rpcListeners[i].mod->loaded) (*out)[n++] = g_rpcListeners[i];
    ReleaseSRWLockShared(&g_rpcLock);
    return *out != NULL ? n : -1;
}

/* Runs the listeners in order until one drops the message: 1 dropped (logged, the first 20 times), 0 kept. */
static int RpcRunListeners(const RpcListener* listeners, int n, const HfRpcEvent* event)
{
    for (int i = 0; i < n; i++)
    {
        if (listeners[i].listener(listeners[i].mod, event, listeners[i].user) != HF_RPC_DROP) continue;
        const SdkRpc* rpc = RpcFind(event->rpc);
        if (InterlockedIncrement(&g_rpcDropsNoted) <= 20)
            Note("rpc: %s dropped %s from peer %llx", listeners[i].mod->id, rpc ? rpc->name : "an RPC", (unsigned long long)event->sender);
        return 1;
    }
    return 0;
}

/* RpcDispatch's detour: the mods' listeners first, then the game's receiver unless one dropped the message. */
static void RpcDispatchDetour(uint64_t sender, uint32_t hash, uint32_t index, const HfRpcArg* args, uint32_t entries)
{
    int dropped = 0;
    if (g_rpcListening)
    {
        RpcListener stack[RPC_COPY], *copy;
        int n = RpcListenersOf(hash, stack, &copy);
        HfRpcEvent event = {sender, hash, entries, args};
        if (n > 0) dropped = RpcRunListeners(copy, n, &event);
        if (copy != NULL && copy != stack) free(copy);
    }
    if (!dropped) g_rpcDispatch(sender, hash, index, args, entries);
}

/* The watch loop (ChannelInstall), after HF_EVENT_GAME_LOADED: hooks the dispatch once a mod listens and its bytes
 * read as reviewed. */
static void RpcInstall(uintptr_t game)
{
    if (g_rpcState != 0 || !g_rpcListening || game == 0) return;
    ULONGLONG now = GetTickCount64();
    if (!g_rpcFirstTry) g_rpcFirstTry = now;
    if (strcmp(g_build, SDK_RPCS_BUILD)) { g_rpcState = -1; Note("rpc: not hooked (unsupported build); no RPC listeners"); return; }
    if (!PinMatches(game + RPC_DISPATCH, kRpcDispatchPin, sizeof kRpcDispatchPin))
    {
        if (now - g_rpcFirstTry < 60000) return;
        g_rpcState = -1;
        Note("rpc: the RPC dispatch is not the reviewed one; no RPC listeners");
        return;
    }
    g_rpcState = LoaderHook("the RPC dispatch", game + RPC_DISPATCH, (void*)RpcDispatchDetour, (void**)&g_rpcDispatch) ? -1 : 1;
    Note(g_rpcState == 1 ? "rpc: hooked the RPC dispatch" : "rpc: could not hook the RPC dispatch; no RPC listeners");
}

/* Puts a listener in its place (highest priority first, equal ones in order). 0 when out of memory. */
static int RpcListenerAdd(RpcListener listener)
{
    AcquireSRWLockExclusive(&g_rpcLock);
    int ok = g_rpcListenerCount < g_rpcListenerCapacity;
    if (!ok)
    {
        int capacity = g_rpcListenerCapacity ? g_rpcListenerCapacity * 2 : 32;
        RpcListener* grown = realloc(g_rpcListeners, sizeof *grown * capacity);
        if (grown) { g_rpcListeners = grown; g_rpcListenerCapacity = capacity; ok = 1; }
    }
    if (ok)
    {
        int at = g_rpcListenerCount;
        while (at > 0 && g_rpcListeners[at - 1].priority < listener.priority) { g_rpcListeners[at] = g_rpcListeners[at - 1]; at--; }
        listener.order = g_rpcListenerOrder++;
        g_rpcListeners[at] = listener;
        g_rpcListenerCount++;
        InterlockedExchange(&g_rpcListening, 1);
    }
    ReleaseSRWLockExclusive(&g_rpcLock);
    return ok;
}

/* SDK 17, rpc_listen. */
static int ApiRpcListen(HfMod* mod, uint32_t rpc, HfRpcListener listener, void* user, int32_t priority)
{
    if (mod == NULL || listener == NULL) return 1;
    const SdkRpc* known = rpc == HF_RPC_ALL ? NULL : RpcFind(rpc);
    if (rpc != HF_RPC_ALL && known == NULL) { ApiLog(mod, "rpc_listen refused: 0x%08X is no RPC of this build", rpc); return 1; }
    if (!RpcListenerAdd((RpcListener){mod, rpc, listener, user, priority, 0})) { ApiLog(mod, "rpc_listen: out of memory"); return 1; }
    Note("rpc: %s listens to %s (priority %d)", mod->id, known ? known->name : "every RPC", priority);
    return 0;
}

/* Whether a send may go now: the game thread, this build, the send's code as reviewed, in co-op; why not in why. */
static uintptr_t RpcSendReady(char* why, size_t whySize)
{
    if (g_gameThread == 0 || GetCurrentThreadId() != g_gameThread) { snprintf(why, whySize, "not on the game thread"); return 0; }
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0 || strcmp(g_build, SDK_RPCS_BUILD)) { snprintf(why, whySize, "not build %s", SDK_RPCS_BUILD); return 0; }
    if (g_rpcSendState == 0) g_rpcSendState = PinMatches(game + RPC_SEND, kRpcSendPin, sizeof kRpcSendPin) ? 1 : -1;
    if (g_rpcSendState != 1) { snprintf(why, whySize, "the game's RPC send is not the reviewed one"); return 0; }
    uint32_t role;
    AcquireSRWLockShared(&g_sessionLock);
    role = g_session.role;
    ReleaseSRWLockShared(&g_sessionLock);
    if (role == HF_SESSION_SOLO) { snprintf(why, whySize, "solo (there is no other machine)"); return 0; }
    return game;
}

/* RpcSend's target for rpc_send's to (HF_TO_ALL, HF_TO_HOST or a peer): 1 with *target set, 0 refused (why). */
static int RpcSendTarget(uintptr_t game, uint64_t to, uint64_t* target, char* why, size_t whySize)
{
    if (to == HF_TO_ALL) { *target = RPC_TO_OTHERS; return 1; }
    if (to >= 0xFFFFFFFFFFFFFFFCull && to != HF_TO_HOST) { snprintf(why, whySize, "0x%llx is no peer", (unsigned long long)to); return 0; }
    if (to != HF_TO_HOST) { *target = to; return 1; }
    uintptr_t user = 0;
    uint64_t host = 0, self = 0;
    if (SafeRead(game + RPC_LOCAL_USER, &user, 8) || user == 0 || SafeRead(user + RPC_HOST_PEER, &host, 8) ||
        SafeRead(user + RPC_LOCAL_PEER, &self, 8) || host == 0)
    { snprintf(why, whySize, "no host yet"); return 0; }
    if (host == self) { snprintf(why, whySize, "this machine is the host"); return 0; }
    *target = host;
    return 1;
}

/* SDK 17, rpc_send. */
static int ApiRpcSend(HfMod* mod, uint32_t rpc, uint64_t to, const HfRpcArg* args, uint32_t count)
{
    if (mod == NULL) return 1;
    const SdkRpc* known = RpcFind(rpc);
    if (known == NULL) { ApiLog(mod, "rpc_send refused: 0x%08X is no RPC of this build", rpc); return 1; }
    char why[160];
    uint64_t target = 0;
    uintptr_t game = RpcSendReady(why, sizeof why);
    if (game == 0 || !RpcArgsMatch(known, args, count, why, sizeof why) || !RpcSendTarget(game, to, &target, why, sizeof why))
    { ApiLog(mod, "rpc_send(%s) refused: %s", known->name, why); return 1; }
    ((RpcSendFn)(game + RPC_SEND))(rpc, target, args, count);
    return 0;
}

/* A mod taken out (live_code.c, on the game thread): its listeners go. */
static void RpcForget(const HfMod* mod)
{
    AcquireSRWLockExclusive(&g_rpcLock);
    int kept = 0;
    for (int i = 0; i < g_rpcListenerCount; i++) if (g_rpcListeners[i].mod != mod) g_rpcListeners[kept++] = g_rpcListeners[i];
    g_rpcListenerCount = kept;
    InterlockedExchange(&g_rpcListening, kept > 0);
    ReleaseSRWLockExclusive(&g_rpcLock);
}
