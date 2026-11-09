/* The mod channel (channel.c, SDK 16): the calls' answers and the events, then the live pipe's lines: a message and a row
 * in, HF_ON_MESSAGE/HF_ON_SHARE out on the game thread; a line for a mod this game does not load dropped; malformed lines
 * refused; send's words, its 1024-byte cap and its 64 a second; share_set's host, size and row caps; is_authority and
 * entity_of_network on a synthetic game.dll. Included by stats_test.c. */

static int g_channelMessages, g_channelShares, g_channelRemovals, g_channelOther;
static HfMessageEvent g_channelMessage;
static HfShareEvent g_channelShare;

static void ChannelMessageListener(HfMod* mod, const HfEventInfo* event, void* user)
{
    (void)mod; (void)user;
    g_channelMessages++;
    g_channelMessage = *(const HfMessageEvent*)event;
}

static void ChannelShareListener(HfMod* mod, const HfEventInfo* event, void* user)
{
    (void)mod; (void)user;
    g_channelShares++;
    g_channelShare = *(const HfShareEvent*)event;
    if (g_channelShare.bytes == 0) g_channelRemovals++;
}

/* Another mod's listener: a message or row is its own mod's and never reaches it. */
static void ChannelOtherListener(HfMod* mod, const HfEventInfo* event, void* user)
{
    (void)mod; (void)event; (void)user;
    g_channelOther++;
}

static uint8_t g_channelAuthority[0x20];
static uint32_t* ChannelFakeHandle(uint32_t* out, uint32_t handle) { *out = handle == 0x1234 ? 42 : 0xFFFFFFFFu; return out; }
static const uint8_t* ChannelFakeDescriptor(uint32_t entity) { return entity == 42 ? g_channelAuthority : NULL; }
static void ChannelFakeJump(uint8_t* at, void* to)
{
    at[0] = 0x48; at[1] = 0xB8;
    memcpy(at + 2, &to, 8);
    at[10] = 0xFF; at[11] = 0xE0; /* mov rax, to; jmp rax */
}

/* is_authority and entity_of_network against a synthetic game.dll at their pinned rvas. */
static void ChannelGameFixture(void)
{
    size_t size = 0x3500000;
    uint8_t* game = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(game);
    int state = g_channelState;
    uintptr_t base = g_channelGame;
    g_channelState = 1;
    g_channelGame = (uintptr_t)game;
    ChannelFakeJump(game + kLaunchFunctions[L_HANDLE].rva, (void*)ChannelFakeHandle);
    ChannelFakeJump(game + kBhvFunctions[F_DESCRIPTOR].rva, (void*)ChannelFakeDescriptor);
    uint32_t yes = 1, no = 0, none = 0xFFFFFFFFu;
    memcpy(game + ABL_NO_ENTITY, &none, 4);
    memcpy(game + ABL_HANDLE_NONE, &none, 4);
    memcpy(g_channelAuthority + 0x14, &yes, 4);
    assert(ApiIsAuthority(NULL, 42) == 1);
    memcpy(g_channelAuthority + 0x14, &no, 4);
    assert(ApiIsAuthority(NULL, 42) == 0);
    assert(ApiIsAuthority(NULL, 7) == -1); /* no descriptor: no such entity */
    assert(ApiEntityOfNetwork(NULL, 0x1234) == 42);
    assert(ApiEntityOfNetwork(NULL, 0x9999) == 0xFFFFFFFFu); /* the game's handle of none */
    assert(ApiEntityOfNetwork(NULL, ABL_NO_HANDLE) == 0xFFFFFFFFu);
    /* not reviewed yet: no answer */
    g_channelState = 0;
    assert(ApiIsAuthority(NULL, 42) == -1 && ApiEntityOfNetwork(NULL, 0x1234) == 0xFFFFFFFFu);
    g_channelState = state;
    g_channelGame = base;
    VirtualFree(game, 0, MEM_RELEASE);
}

static void ChannelFixture(void)
{
    HfMod* mod = &g_mods[0];
    int mods = g_modCount, loaded0 = g_mods[0].loaded, loaded1 = g_mods[1].loaded, listeners = g_listenerCount, missingBefore = g_channelMissingCount;
    char id0[sizeof g_mods[0].id], id1[sizeof g_mods[1].id];
    memcpy(id0, g_mods[0].id, sizeof id0);
    memcpy(id1, g_mods[1].id, sizeof id1);
    LONG listening = g_listening;
    DWORD thread = g_gameThread;
    int connected = g_liveConnected;
    size_t used = g_liveOutUsed;

    g_modCount = 2;
    strcpy(g_mods[0].id, "test.champ");
    strcpy(g_mods[1].id, "test.other");
    g_mods[0].loaded = g_mods[1].loaded = 1;
    g_channelMessages = g_channelShares = g_channelRemovals = g_channelOther = 0;
    assert(ApiSubscribe(mod, HF_ON_MESSAGE, ChannelMessageListener, NULL, 0));
    assert(ApiSubscribe(mod, HF_ON_SHARE, ChannelShareListener, NULL, 0));
    assert(ApiSubscribe(&g_mods[1], HF_ON_MESSAGE, ChannelOtherListener, NULL, 0));
    assert(ApiSubscribe(&g_mods[1], HF_ON_SHARE, ChannelOtherListener, NULL, 0));
    EventsTick(); /* this thread is the game thread: events go out at once */
    SetSession(HF_SESSION_HOST, HF_STATS_OWN, "");

    char line[4096], reply[512];
    uint8_t value[8] = {1, 2, 3, 4}, out[8] = {0};
    uint64_t key = 0;
    uint32_t bytes = 0;

    /* share_set, share_get, share_row: one row, a smaller capacity, a value that did not change */
    assert(ApiShareSet(mod, 0x1122, value, 4) == 0);
    assert(g_channelShares == 1 && g_channelShare.key == 0x1122 && g_channelShare.bytes == 4 && !memcmp(g_channelShare.value, value, 4) && g_channelOther == 0);
    assert(ApiShareGet(mod, 0x1122, out, sizeof out) == 4 && !memcmp(out, value, 4));
    memset(out, 0, sizeof out);
    assert(ApiShareGet(mod, 0x1122, out, 2) == 4 && out[0] == 1 && out[1] == 2 && out[2] == 0);
    assert(ApiShareRow(mod, 0, &key, out, sizeof out, &bytes) && key == 0x1122 && bytes == 4 && !memcmp(out, value, 4));
    assert(!ApiShareRow(mod, 1, &key, out, sizeof out, &bytes));
    assert(ApiShareGet(mod, 0x3344, out, sizeof out) == 0);
    int shares = g_channelShares;
    assert(ApiShareSet(mod, 0x1122, value, 4) == 0 && g_channelShares == shares);
    /* a removal, and no row left */
    assert(ApiShareSet(mod, 0x1122, NULL, 0) == 0 && g_channelShares == shares + 1 && g_channelShare.bytes == 0 && g_channelRemovals == 1);
    assert(ApiShareGet(mod, 0x1122, out, sizeof out) == 0 && !ApiShareRow(mod, 0, &key, out, sizeof out, &bytes));

    /* the caps: a value over 64 bytes, and a 513th row */
    uint8_t big[HF_SHARE_VALUE_MAX + 1] = {0};
    assert(ApiShareSet(mod, 1, big, HF_SHARE_VALUE_MAX + 1) == 1);
    assert(ApiShareSet(mod, 2, big, HF_SHARE_VALUE_MAX) == 0);
    assert(ApiShareSet(mod, 2, NULL, 0) == 0);
    for (uint32_t i = 0; i < HF_SHARE_ROWS; i++) assert(ApiShareSet(mod, 0x10000 + i, value, 4) == 0);
    assert(ApiShareSet(mod, 0x99999, value, 4) == 1);
    ChannelReset();
    assert(ApiShareGet(mod, 2, out, sizeof out) == 0 && !ApiShareRow(mod, 0, &key, out, sizeof out, &bytes));

    /* only the host sets rows; solo sends nothing */
    SetSession(HF_SESSION_JOINER, HF_STATS_OWN, "Host");
    assert(ApiShareSet(mod, 1, value, 4) == 1);
    SetSession(HF_SESSION_SOLO, HF_STATS_OWN, "");
    assert(ApiSend(mod, HF_TO_ALL, value, 4) == 1);
    SetSession(HF_SESSION_HOST, HF_STATS_OWN, "");

    /* send while the pipe is up: 64 a second, all, host and a peer's id */
    AcquireSRWLockExclusive(&g_liveOutLock);
    g_liveConnected = 1;
    g_liveOutUsed = 0;
    ReleaseSRWLockExclusive(&g_liveOutLock);
    ChannelMod* c = ChannelFor(mod);
    assert(c != NULL);
    c->tokens = CHANNEL_RATE;
    c->refilled = GetTickCount64();
    for (int i = 0; i < (int)CHANNEL_RATE; i++) assert(ApiSend(mod, HF_TO_ALL, value, 4) == 0);
    assert(ApiSend(mod, HF_TO_ALL, value, 4) == 1);
    g_liveOut[g_liveOutUsed] = 0;
    assert(strstr(g_liveOut, "send\ttest.champ\tall\t01020304\n") != NULL);
    AcquireSRWLockExclusive(&g_liveOutLock);
    g_liveOutUsed = 0;
    ReleaseSRWLockExclusive(&g_liveOutLock);
    c->tokens = CHANNEL_RATE;
    c->refilled = GetTickCount64();
    assert(ApiSend(mod, HF_TO_HOST, value, 1) == 0);
    assert(ApiSend(mod, 0x110000100000022ull, value, 1) == 0);
    g_liveOut[g_liveOutUsed] = 0;
    assert(strstr(g_liveOut, "send\ttest.champ\thost\t01\n") != NULL);
    assert(strstr(g_liveOut, "send\ttest.champ\t0110000100000022\t01\n") != NULL);
    /* the whole 1024 bytes, and one over refused */
    AcquireSRWLockExclusive(&g_liveOutLock);
    g_liveOutUsed = 0;
    ReleaseSRWLockExclusive(&g_liveOutLock);
    c->tokens = CHANNEL_RATE;
    c->refilled = GetTickCount64();
    uint8_t message[HF_MESSAGE_MAX];
    memset(message, 0xAB, sizeof message);
    assert(ApiSend(mod, HF_TO_ALL, message, HF_MESSAGE_MAX) == 0);
    assert(ApiSend(mod, HF_TO_ALL, message, HF_MESSAGE_MAX + 1) == 1);
    assert(strstr(g_liveOut, "send\ttest.champ\tall\tab") != NULL && g_liveOutUsed > HF_MESSAGE_MAX * 2);
    /* the pipe goes: send is refused */
    AcquireSRWLockExclusive(&g_liveOutLock);
    g_liveConnected = 0;
    g_liveOutUsed = 0;
    ReleaseSRWLockExclusive(&g_liveOutLock);
    c->tokens = CHANNEL_RATE;
    c->refilled = GetTickCount64();
    assert(ApiSend(mod, HF_TO_ALL, value, 4) == 1);

    /* the pipe's lines: queued for the game thread, then delivered by its update */
    g_channelMessages = 0;
    g_gameThread = GetCurrentThreadId() + 1;
    snprintf(line, sizeof line, "message\t40\ttest.champ\t110000100000022\tdeadbeef");
    LiveRequest(line, reply, sizeof reply);
    assert(!strcmp(reply, "40\tok\tnow") && g_channelMessages == 0 && g_channelOther == 0);
    EventsTick();
    assert(g_channelMessages == 1 && g_channelMessage.info.kind == HF_ON_MESSAGE && g_channelMessage.from == 0x110000100000022ull &&
        g_channelMessage.bytes == 4 && g_channelMessage.data[0] == 0xde && g_channelMessage.data[3] == 0xef && g_channelMessage.info.update == g_updates);

    /* a message for the other mod reaches only it */
    g_channelMessages = 0;
    snprintf(line, sizeof line, "message\t41\ttest.other\t5\t01");
    LiveRequest(line, reply, sizeof reply);
    assert(!strcmp(reply, "41\tok\tnow") && g_channelMessages == 0 && g_channelOther == 1);

    /* a line for a mod this game does not load is dropped, and said once for its id */
    g_channelOther = 0;
    int missing = g_channelMissingCount;
    snprintf(line, sizeof line, "message\t42\tno.such.mod\t1\t01");
    LiveRequest(line, reply, sizeof reply);
    assert(!strcmp(reply, "42\tok\tnow") && g_channelOther == 0 && g_channelMissingCount == missing + 1);
    snprintf(line, sizeof line, "message\t43\tno.such.mod\t1\t01");
    LiveRequest(line, reply, sizeof reply);
    assert(!strcmp(reply, "43\tok\tnow") && g_channelMissingCount == missing + 1);

    /* malformed lines are refused; a whole 1024-byte message is not */
    const char* const bad[] = {
        "message\t44", "message\t45\ttest.champ\tzz\t01", "message\t46\ttest.champ\t1\t0g",
        "share\t47\ttest.champ\tzz\t01", "share\t48\ttest.champ\t1\tzz",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    {
        snprintf(line, sizeof line, "%s", bad[i]);
        LiveRequest(line, reply, sizeof reply);
        assert(strstr(reply, "\trefused\t") != NULL);
    }
    char* at = line + snprintf(line, sizeof line, "message\t49\ttest.champ\t1\t");
    for (int i = 0; i < HF_MESSAGE_MAX * 2 + 2; i++) *at++ = 'a';
    *at = 0;
    LiveRequest(line, reply, sizeof reply);
    assert(strstr(reply, "\trefused\t") != NULL);
    at = line + snprintf(line, sizeof line, "message\t50\ttest.champ\t1\t");
    for (int i = 0; i < HF_MESSAGE_MAX * 2; i++) *at++ = 'a';
    *at = 0;
    g_channelMessages = 0;
    LiveRequest(line, reply, sizeof reply);
    assert(!strcmp(reply, "50\tok\tnow") && g_channelMessages == 1 && g_channelMessage.bytes == HF_MESSAGE_MAX);

    /* share and sharereset down: the row arrives, changes and goes */
    g_channelShares = g_channelRemovals = 0;
    snprintf(line, sizeof line, "share\t51\ttest.champ\t0000000000000001\t0102");
    LiveRequest(line, reply, sizeof reply);
    assert(!strcmp(reply, "51\tok\tnow") && g_channelShares == 1 && g_channelShare.key == 1 && g_channelShare.bytes == 2 && g_channelShare.value[1] == 2);
    assert(ApiShareGet(mod, 1, out, sizeof out) == 2 && out[0] == 1 && out[1] == 2);
    snprintf(line, sizeof line, "share\t52\ttest.champ\t0000000000000001\t-");
    LiveRequest(line, reply, sizeof reply);
    assert(!strcmp(reply, "52\tok\tnow") && g_channelShares == 2 && g_channelShare.bytes == 0 && g_channelRemovals == 1 &&
        ApiShareGet(mod, 1, out, sizeof out) == 0);
    assert(ApiShareSet(mod, 5, value, 4) == 0 && ApiShareSet(mod, 6, value, 4) == 0);
    shares = g_channelShares;
    int removals = g_channelRemovals;
    strcpy(line, "sharereset\t53");
    LiveRequest(line, reply, sizeof reply);
    assert(!strcmp(reply, "53\tok\tnow") && g_channelShares == shares + 2 && g_channelRemovals == removals + 2 &&
        ApiShareGet(mod, 5, out, sizeof out) == 0 && ApiShareGet(mod, 6, out, sizeof out) == 0);

    /* a row of the other mod reaches only it */
    snprintf(line, sizeof line, "share\t54\ttest.other\t0000000000000002\t09");
    LiveRequest(line, reply, sizeof reply);
    assert(!strcmp(reply, "54\tok\tnow") && g_channelShares == shares + 2 && g_channelOther == 1);

    ChannelReset();
    SetSession(HF_SESSION_SOLO, HF_STATS_OWN, "");
    g_listenerCount = listeners;
    g_listening = listening;
    g_gameThread = thread;
    AcquireSRWLockExclusive(&g_liveOutLock);
    g_liveConnected = connected;
    g_liveOutUsed = used;
    ReleaseSRWLockExclusive(&g_liveOutLock);
    g_channelMissingCount = missingBefore;
    g_modCount = mods;
    memcpy(g_mods[0].id, id0, sizeof id0);
    memcpy(g_mods[1].id, id1, sizeof id1);
    g_mods[0].loaded = loaded0;
    g_mods[1].loaded = loaded1;
    g_channelMessages = g_channelShares = g_channelRemovals = g_channelOther = 0;
    if (g_liveOutUsed == 0) g_liveOut[0] = 0;
}
