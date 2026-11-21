/* A code mod's spend or grant of the player's currency (SDK 21; hellforge_currency.h, docs/modding/currency.md). The
 * backend holds the saved balance, so a change asks it first (CurrencySpend.cs) on a worker thread, and only then does
 * the game thread change the game's copy the same way with the game's own remove or add (ownership.c): the HUD shows it
 * at once and the shown and saved balances never part. Included by live_content.c after paid_guard_step.c (the backend's port,
 * WinHTTP). */

#define CS_PENDING 64
#define CS_REASON 128
#define CS_BODY 512
#define CS_REPLY 4096

enum { CS_FREE, CS_ASKING, CS_ANSWERED };

typedef struct Spend
{
    HfMod* mod;
    uint32_t request, currency, result;
    int64_t amount, balance, added;
    int grant;                    /* grant_currency: added, not taken */
    char reason[CS_REASON];
    HfSpendCallback callback;
    void* user;
    int state;
} Spend;

static Spend g_spends[CS_PENDING];
static SRWLOCK g_spendLock = SRWLOCK_INIT;
static uint32_t g_spendRequests;
static HANDLE g_spendWake;
static volatile LONG g_spendThread;

static const uint32_t kSpendCurrencies[] = {HF_CURRENCY_MEDALS, HF_CURRENCY_REQUISITION, HF_CURRENCY_COMMON_SAMPLES,
    HF_CURRENCY_RARE_SAMPLES, HF_CURRENCY_SUPER_SAMPLES, HF_CURRENCY_FORGE_CREDITS};

static int SpendCurrencyKnown(uint32_t currency)
{
    for (size_t i = 0; i < sizeof kSpendCurrencies / sizeof kSpendCurrencies[0]; i++)
        if (kSpendCurrencies[i] == currency) return 1;
    return 0;
}

/* text as a JSON string's contents: quotes and backslashes escaped, control bytes as spaces. */
static void SpendJsonText(char* out, size_t size, const char* text)
{
    size_t n = 0;
    for (; *text && n + 3 < size; text++)
    {
        unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else out[n++] = c < 0x20 ? ' ' : (char)c;
    }
    out[n] = 0;
}

/* A text cut to its buffer may end inside a UTF-8 character, which the backend's JSON parser refuses: that end dropped. */
static void SpendTrimUtf8(char* text)
{
    size_t n = strlen(text), start = n;
    while (start > 0 && ((unsigned char)text[start - 1] & 0xC0) == 0x80) start--;
    if (start == 0) return;
    unsigned char lead = (unsigned char)text[start - 1];
    size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (start - 1) < want) text[start - 1] = 0;
}

/* POSTs body to the backend: the status (0: no answer) and the reply's text. The fixture sets its own. */
static DWORD SpendPostHttp(const wchar_t* path, const char* body, char* reply, size_t size)
{
    const char* why = NULL;
    const PgHttp* http = PaidGuardHttp(&why);
    if (http == NULL || g_paidGuardPort == 0) return 0;
    DWORD status = 0, statusSize = sizeof status, got = 0;
    size_t used = 0;
    HINTERNET session = http->open(L"HellForge-loader", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET link = session ? http->connect(session, L"127.0.0.1", (INTERNET_PORT)g_paidGuardPort, 0) : NULL;
    HINTERNET call = link ? http->request(link, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : NULL;
    if (call && http->timeouts(call, 2000, 2000, 5000, 5000) &&
        http->send(call, L"Content-Type: application/json\r\n", (DWORD)-1, (LPVOID)body, (DWORD)strlen(body), (DWORD)strlen(body), 0) &&
        http->receive(call, NULL) &&
        http->query(call, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
        while (used + 1 < size && http->read(call, reply + used, (DWORD)(size - 1 - used), &got) && got > 0) used += got;
    reply[used] = 0;
    if (call) http->close(call);
    if (link) http->close(link);
    if (session) http->close(session);
    return status;
}
static DWORD (*g_spendPost)(const wchar_t* path, const char* body, char* reply, size_t size) = SpendPostHttp;
static uint32_t SpendGameState(void) { return ApiGameState(NULL); }
static uint32_t (*g_spendState)(void) = SpendGameState; /* the fixture sets its own */

/* The backend's answer {"result": "spent" | "too_little" | "granted", "balance": n, "added": n} into result, balance and
 * added; HF_SPEND_FAILED when it is not one. */
static uint32_t SpendParse(DWORD status, const char* reply, int64_t* balance, int64_t* added)
{
    *balance = -1;
    *added = 0;
    const char* at = status == 200 ? strstr(reply, "\"balance\":") : NULL;
    if (at == NULL) return HF_SPEND_FAILED;
    char* end;
    long long value = strtoll(at + 10, &end, 10);
    if (end == at + 10 || value < 0) return HF_SPEND_FAILED;
    uint32_t result = strstr(reply, "\"result\":\"spent\"") ? HF_SPEND_SPENT : strstr(reply, "\"result\":\"too_little\"") ? HF_SPEND_TOO_LITTLE
                    : strstr(reply, "\"result\":\"granted\"") ? HF_SPEND_GRANTED : HF_SPEND_FAILED;
    const char* sum = strstr(reply, "\"added\":");
    if (result == HF_SPEND_GRANTED && (sum == NULL || (*added = strtoll(sum + 8, &end, 10)) < 0)) result = HF_SPEND_FAILED;
    if (result != HF_SPEND_FAILED) *balance = value;
    return result;
}

/* One asked spend sent and its answer kept for the game thread. */
static void SpendAsk(Spend* spend)
{
    char mod[160], reason[CS_REASON * 2], body[CS_BODY], reply[CS_REPLY];
    SpendJsonText(mod, sizeof mod, spend->mod->id);
    SpendJsonText(reason, sizeof reason, spend->reason);
    snprintf(body, sizeof body, "{\"mod\":\"%s\",\"currency\":%u,\"amount\":%lld,\"reason\":\"%s\"}", mod, spend->currency,
        (long long)spend->amount, reason);
    DWORD status = g_spendPost(spend->grant ? L"/hellforge/grant" : L"/hellforge/spend", body, reply, sizeof reply);
    int64_t balance, added;
    uint32_t result = SpendParse(status, reply, &balance, &added);
    AcquireSRWLockExclusive(&g_spendLock);
    spend->result = result;
    spend->balance = balance;
    spend->added = added;
    spend->state = CS_ANSWERED;
    ReleaseSRWLockExclusive(&g_spendLock);
}

/* The oldest spend waiting for the backend, or NULL. */
static Spend* SpendWaiting(void)
{
    Spend* found = NULL;
    AcquireSRWLockShared(&g_spendLock);
    for (int i = 0; i < CS_PENDING; i++)
        if (g_spends[i].state == CS_ASKING && (found == NULL || g_spends[i].request < found->request)) found = &g_spends[i];
    ReleaseSRWLockShared(&g_spendLock);
    return found;
}

/* The worker: the spends in the order they came, one request at a time, so two never race for one balance. */
static DWORD WINAPI SpendWorker(void* unused)
{
    (void)unused;
    for (;;)
    {
        WaitForSingleObject(g_spendWake, INFINITE);
        for (Spend* spend; (spend = SpendWaiting()) != NULL;) SpendAsk(spend);
    }
}

/* Why a spend cannot be asked now, or NULL. */
static const char* SpendRefusal(uint32_t currency, int64_t amount, const char* reason)
{
    if (!SpendCurrencyKnown(currency)) return "not a currency mods may spend (HF_CURRENCY_...)";
    if (amount < 1 || amount > HF_SPEND_MAX) return "the amount is not 1 to HF_SPEND_MAX";
    if (reason == NULL || reason[0] == 0) return "it gives no reason";
    if (g_paidGuardPort == 0) return "no backend serves this session";
    if (g_spendState() == HF_STATE_MISSION) return "the player is in a mission";
    return NULL;
}

/* The worker, started on the first change asked for. 1 while it runs. */
static int SpendWorkerRunning(void)
{
    if (InterlockedCompareExchange(&g_spendThread, 1, 0) == 0)
    {
        g_spendWake = CreateEventW(NULL, FALSE, FALSE, NULL);
        HANDLE thread = g_spendWake ? CreateThread(NULL, 0, SpendWorker, NULL, 0, NULL) : NULL;
        if (thread == NULL) { g_spendThread = 2; Note("currency: the spend worker could not start; no spends this session"); }
        else CloseHandle(thread);
    }
    return g_spendThread == 1;
}

static uint32_t SpendQueue(HfMod* mod, int grant, uint32_t currency, int64_t amount, const char* reason, HfSpendCallback callback, void* user)
{
    const char* call = grant ? "grant_currency" : "spend_currency";
    if (mod == NULL || ModGone(mod)) return 0;
    const char* why = SpendRefusal(currency, amount, reason);
    if (why != NULL) { ApiLog(mod, "%s refused: %s", call, why); return 0; }
    if (!SpendWorkerRunning()) { ApiLog(mod, "%s refused: HellForge's spend worker is not running", call); return 0; }
    uint32_t request = 0;
    AcquireSRWLockExclusive(&g_spendLock);
    for (int i = 0; i < CS_PENDING && request == 0; i++)
        if (g_spends[i].state == CS_FREE)
        {
            request = ++g_spendRequests;
            g_spends[i] = (Spend){mod, request, currency, 0, amount, -1, 0, grant, "", callback, user, CS_ASKING};
            snprintf(g_spends[i].reason, sizeof g_spends[i].reason, "%s", reason);
            SpendTrimUtf8(g_spends[i].reason);
        }
    ReleaseSRWLockExclusive(&g_spendLock);
    if (request == 0) { ApiLog(mod, "%s refused: %d changes wait already", call, CS_PENDING); return 0; }
    SetEvent(g_spendWake);
    return request;
}
static uint32_t ApiSpendCurrency(HfMod* mod, uint32_t currency, int64_t amount, const char* reason, HfSpendCallback callback, void* user)
{
    return SpendQueue(mod, 0, currency, amount, reason, callback, user);
}
static uint32_t ApiGrantCurrency(HfMod* mod, uint32_t currency, int64_t amount, const char* reason, HfSpendCallback callback, void* user)
{
    return SpendQueue(mod, 1, currency, amount, reason, callback, user);
}

/* The game's item table once loaded (ownership.c), or 0. */
static uintptr_t SpendManager(void)
{
    if (g_ownApply == NULL && !OwnProve()) return 0;
    uintptr_t manager = g_ownManager != NULL ? g_ownManager() : 0;
    if (manager == 0 || *(const uint32_t*)(manager + OWN_CATALOGUE) != 2 || *(const uint32_t*)(manager + OWN_INVENTORY) != 2) return 0;
    return manager;
}

static int ApiCurrencyBalance(HfMod* mod, uint32_t currency, int64_t* amount)
{
    (void)mod;
    uintptr_t manager = amount != NULL && SpendCurrencyKnown(currency) ? SpendManager() : 0;
    int entry = manager != 0 ? OwnEntry(manager, currency, 0, 0) : -1;
    if (entry < 0) return 1;
    int32_t held = *(const int32_t*)(OwnRecordAt(manager, entry) + 0xB0);
    *amount = held > 0 ? held : 0;
    return 0;
}

/* What the backend changed, changed in the game's copy too: its remove for a spend, its add for a grant. */
static void SpendToGame(const Spend* spend)
{
    int changed = spend->result == HF_SPEND_SPENT || (spend->result == HF_SPEND_GRANTED && spend->added > 0);
    uintptr_t manager = changed ? SpendManager() : 0;
    if (manager != 0 && spend->result == HF_SPEND_SPENT) g_ownRemove(manager, spend->currency, (uint32_t)spend->amount);
    else if (manager != 0) g_ownApply(manager, spend->currency, (uint32_t)spend->added, 0, NULL, 0);
    else if (changed)
        Note("currency: %s's change is saved, but the game's item functions are not ready; it shows the old balance until it reloads", spend->mod->id);
}

/* The game thread's update (events.c EventsTick): each answered change made to the game's copy, then its mod told. */
static void CurrencyTick(void)
{
    for (int i = 0; i < CS_PENDING; i++)
    {
        if (g_spends[i].state != CS_ANSWERED) continue;
        AcquireSRWLockExclusive(&g_spendLock);
        Spend spend = g_spends[i];
        g_spends[i].state = CS_FREE;
        ReleaseSRWLockExclusive(&g_spendLock);
        SpendToGame(&spend);
        static const char* const kResults[] = {"", "spent", "too little", "failed: the backend did not answer or refused it", "granted"};
        ApiLog(spend.mod, "%s %u (%s): %lld of 0x%08x %s; balance %lld", spend.grant ? "grant_currency" : "spend_currency", spend.request,
            spend.reason, (long long)spend.amount, spend.currency, kResults[spend.result], (long long)spend.balance);
        if (spend.callback != NULL && !ModGone(spend.mod)) spend.callback(spend.mod, spend.request, spend.result, spend.balance, spend.user);
    }
}
