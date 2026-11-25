/* The game's RPCs for mods (rpc.c, SDK 17): the table's lookup, rpc_send's argument checks (kinds, missing and extra
 * entries, arrays over their maximum, strings over 2047 bytes, a message over the game's buffer), rpc_listen's
 * refusals, the dispatch's listeners in priority order with HF_RPC_DROP keeping the game's receiver from running, an
 * HF_RPC_ALL listener, a listener of a mod no longer loaded skipped, and a mod's listeners forgotten. Included by
 * stats_test.c. */

static int g_rpcOriginal, g_rpcFirst, g_rpcSecond, g_rpcAll;
static uint64_t g_rpcSender;

static void RpcFakeDispatch(uint64_t sender, uint32_t hash, uint32_t index, const HfRpcArg* args, uint32_t entries)
{
    (void)hash; (void)index; (void)args; (void)entries;
    g_rpcOriginal++;
    g_rpcSender = sender;
}
static int RpcFirstListener(HfMod* mod, const HfRpcEvent* e, void* user) { (void)mod; (void)e; g_rpcFirst = ++*(int*)user; return HF_RPC_KEEP; }
static int RpcSecondListener(HfMod* mod, const HfRpcEvent* e, void* user) { (void)mod; (void)e; g_rpcSecond = ++*(int*)user; return HF_RPC_KEEP; }
static int RpcDropListener(HfMod* mod, const HfRpcEvent* e, void* user) { (void)mod; (void)e; (void)user; return HF_RPC_DROP; }
static int RpcAllListener(HfMod* mod, const HfRpcEvent* e, void* user) { (void)mod; (void)user; g_rpcAll += (int)e->count + 1; return HF_RPC_KEEP; }

/* The first RPC of the table whose first argument is an array of ints, or NULL. */
static const SdkRpc* RpcWithIntArray(void)
{
    for (size_t i = 0; i < SDK_RPC_COUNT; i++)
        if (kSdkRpcs[i].count == 1 && kSdkRpcArgs[kSdkRpcs[i].first].kind == HF_RPC_KIND_ARRAY &&
            kSdkRpcArgs[kSdkRpcs[i].first].element == HF_RPC_KIND_INT) return &kSdkRpcs[i];
    return NULL;
}

/* The first RPC of the table that takes one string, or NULL. */
static const SdkRpc* RpcWithString(void)
{
    for (size_t i = 0; i < SDK_RPC_COUNT; i++)
        if (kSdkRpcs[i].count == 1 && kSdkRpcArgs[kSdkRpcs[i].first].kind == HF_RPC_KIND_STRING) return &kSdkRpcs[i];
    return NULL;
}

static void RpcFixture(void)
{
    char why[160];
    /* the table: sorted, found by hash */
    for (size_t i = 1; i < SDK_RPC_COUNT; i++) assert(kSdkRpcs[i - 1].hash < kSdkRpcs[i].hash);
    const SdkRpc* pickup = RpcFind(0x04365DCFu);
    assert(pickup != NULL && !strcmp(pickup->name, "rpc_pickup_entity") && pickup->count == 2);
    assert(RpcFind(0x12345678u) == NULL);

    /* arguments: two ints */
    int32_t a = 5, b = 7;
    HfRpcArg two[3] = {{HF_RPC_KIND_INT, 4, &a}, {HF_RPC_KIND_INT, 4, &b}, {HF_RPC_KIND_INT, 4, &b}};
    assert(RpcArgsMatch(pickup, two, 2, why, sizeof why));
    assert(!RpcArgsMatch(pickup, two, 1, why, sizeof why) && strstr(why, "missing") != NULL);
    assert(!RpcArgsMatch(pickup, two, 3, why, sizeof why) && strstr(why, "expected") != NULL);
    HfRpcArg wrong[2] = {{HF_RPC_KIND_INT, 4, &a}, {HF_RPC_KIND_FLOAT, 4, &b}};
    assert(!RpcArgsMatch(pickup, wrong, 2, why, sizeof why) && strstr(why, "kind") != NULL);
    HfRpcArg none[2] = {{HF_RPC_KIND_INT, 4, &a}, {HF_RPC_KIND_INT, 4, NULL}};
    assert(!RpcArgsMatch(pickup, none, 2, why, sizeof why) && strstr(why, "no value") != NULL);
    assert(!RpcArgsMatch(pickup, NULL, 2, why, sizeof why));

    /* an array: its count within its maximum, then that many elements */
    const SdkRpc* arrayed = RpcWithIntArray();
    if (arrayed != NULL)
    {
        const SdkRpcArg* d = &kSdkRpcArgs[arrayed->first];
        HfRpcArg list[260];
        int32_t values[259] = {0};
        uint32_t n = d->max;
        list[0] = (HfRpcArg){HF_RPC_KIND_ARRAY, 0, (const void*)(uintptr_t)n};
        for (uint32_t k = 0; k < n && k < 259; k++) list[1 + k] = (HfRpcArg){HF_RPC_KIND_INT, 4, &values[k]};
        assert(RpcArgsMatch(arrayed, list, 1 + n, why, sizeof why));
        assert(!RpcArgsMatch(arrayed, list, n, why, sizeof why) && strstr(why, "missing") != NULL);
        list[0].value = (const void*)(uintptr_t)(n + 1);
        assert(!RpcArgsMatch(arrayed, list, 2 + n, why, sizeof why) && strstr(why, "at most") != NULL);
    }

    /* a string: at most 2047 bytes, and the message within the game's buffer */
    const SdkRpc* texted = RpcWithString();
    if (texted != NULL)
    {
        static char text[2100];
        memset(text, 'x', sizeof text - 1);
        text[1000] = 0;
        HfRpcArg one = {HF_RPC_KIND_STRING, 1000, text};
        assert(RpcArgsMatch(texted, &one, 1, why, sizeof why));
        text[1000] = 'x';
        text[2048] = 0;
        assert(!RpcArgsMatch(texted, &one, 1, why, sizeof why) && strstr(why, "2047") != NULL);
        text[2048] = 'x';
        text[1300] = 0;   /* within a string's 2047 bytes, but over the 1200-byte message */
        assert(!RpcArgsMatch(texted, &one, 1, why, sizeof why) && strstr(why, "over the game's") != NULL);
    }

    /* listening */
    HfMod* m = &g_mods[0];
    HfMod* other = &g_mods[1];
    int counter = 0;
    assert(ApiRpcListen(m, 0x12345678u, RpcFirstListener, &counter, 0) == 1);
    assert(ApiRpcListen(m, pickup->hash, NULL, NULL, 0) == 1);
    assert(ApiRpcListen(m, pickup->hash, RpcFirstListener, &counter, 0) == 0);
    assert(ApiRpcListen(other, pickup->hash, RpcSecondListener, &counter, 5) == 0);   /* higher priority: runs first */
    assert(ApiRpcListen(m, HF_RPC_ALL, RpcAllListener, NULL, 0) == 0);
    g_rpcDispatch = RpcFakeDispatch;
    RpcDispatchDetour(0xABCD, pickup->hash, 7, two, 2);
    assert(g_rpcOriginal == 1 && g_rpcSender == 0xABCD);
    assert(g_rpcSecond == 1 && g_rpcFirst == 2 && g_rpcAll == 3);
    /* another RPC: only the HF_RPC_ALL listener */
    RpcDispatchDetour(0xABCD, 0x00019F67u, 0, two, 2);
    assert(g_rpcOriginal == 2 && g_rpcFirst == 2 && g_rpcAll == 6);

    /* a drop stops the later listeners and the game's receiver */
    assert(ApiRpcListen(other, pickup->hash, RpcDropListener, NULL, 9) == 0);
    RpcDispatchDetour(0xABCD, pickup->hash, 7, two, 2);
    assert(g_rpcOriginal == 2 && g_rpcSecond == 1 && g_rpcFirst == 2);

    /* a mod no longer loaded: its listeners are skipped, then forgotten */
    other->loaded = 0;
    RpcDispatchDetour(0xABCD, pickup->hash, 7, two, 2);
    assert(g_rpcOriginal == 3 && g_rpcSecond == 1 && g_rpcFirst == 3);
    other->loaded = 1;
    RpcForget(other);
    RpcDispatchDetour(0xABCD, pickup->hash, 7, two, 2);
    assert(g_rpcOriginal == 4 && g_rpcSecond == 1 && g_rpcFirst == 4);
    RpcForget(m);
    assert(g_rpcListenerCount == 0 && !g_rpcListening);
    RpcDispatchDetour(0xABCD, pickup->hash, 7, two, 2);
    assert(g_rpcOriginal == 5 && g_rpcFirst == 4);

    /* sending: an unknown RPC, and any RPC off the game thread */
    assert(ApiRpcSend(m, 0x12345678u, HF_TO_ALL, two, 2) == 1);
    DWORD thread = g_gameThread;
    g_gameThread = GetCurrentThreadId() + 1;
    assert(ApiRpcSend(m, pickup->hash, HF_TO_ALL, two, 2) == 1);
    g_gameThread = thread;
    g_rpcDispatch = NULL;
    printf("PASS rpc: the table, rpc_send's argument checks, rpc_listen, listeners in order, a drop, every-RPC listeners, unloaded and forgotten mods\n");
}
