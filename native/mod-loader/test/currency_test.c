/* Mods' currency spends (currency.c): refusals, the backend's answers, the game's copy taken only for a spend the
 * backend made, the mod told on the game thread, and the balance read from the game's table. A fake backend answers
 * on the spend worker. Included by stats_test.c after paid_guard_test.c (its fake item table and remove). */

static DWORD g_csStatus;
static const char* g_csReply;
static char g_csBody[CS_BODY];
static DWORD FakeSpendPost(const char* body, char* reply, size_t size)
{
    snprintf(g_csBody, sizeof g_csBody, "%s", body);
    snprintf(reply, size, "%s", g_csReply != NULL ? g_csReply : "");
    return g_csStatus;
}
static uint32_t g_csState = HF_STATE_SHIP;
static uint32_t FakeSpendState(void) { return g_csState; }

static int g_csCalls;
static uint32_t g_csRequest, g_csResult;
static int64_t g_csBalance;
static void SpendAnswered(HfMod* mod, uint32_t request, uint32_t result, int64_t balance, void* user)
{
    assert(user == &g_csCalls);
    g_csCalls++;
    g_csRequest = request;
    g_csResult = result;
    g_csBalance = balance;
}

/* Asks, waits for the worker's answer, then runs the game thread's update. */
static uint32_t SpendOnce(HfMod* mod, uint32_t currency, int64_t amount, const char* reason, DWORD status, const char* reply)
{
    g_csStatus = status;
    g_csReply = reply;
    int before = g_csCalls;
    uint32_t request = ApiSpendCurrency(mod, currency, amount, reason, SpendAnswered, &g_csCalls);
    assert(request != 0);
    for (int i = 0; i < 500 && g_csCalls == before; i++)
    {
        Sleep(10);
        CurrencyTick();
    }
    assert(g_csCalls == before + 1 && g_csRequest == request);
    return g_csResult;
}

static void CurrencyFixture(void)
{
    int mods = g_modCount;
    HfMod* mod = ContentMod("yui.kit");
    g_spendPost = FakeSpendPost;
    g_spendState = FakeSpendState;
    uintptr_t manager = PgTable(0xDDCF8 + 4);
    FakeOwnAdd(0, HF_CURRENCY_REQUISITION, HF_CURRENCY_REQUISITION, 0, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 6, 1);
    int requisition = OwnEntry(manager, HF_CURRENCY_REQUISITION, 0, 0);
    PgSet(manager, requisition, 2, 5000);
    g_ownRemove = PgRemove;
    g_ownManager = FakeManager;
    g_ownApply = FakeOwnApply;
    int removes = g_pgRemoves;

    /* refused before anything is asked */
    int port = g_paidGuardPort;
    g_paidGuardPort = 0;
    assert(ApiSpendCurrency(mod, HF_CURRENCY_REQUISITION, 10, "kit", NULL, NULL) == 0); /* no backend */
    g_paidGuardPort = 41234;
    assert(ApiSpendCurrency(mod, 0x7001, 10, "kit", NULL, NULL) == 0);                   /* no currency */
    assert(ApiSpendCurrency(mod, HF_CURRENCY_REQUISITION, 0, "kit", NULL, NULL) == 0);
    assert(ApiSpendCurrency(mod, HF_CURRENCY_REQUISITION, HF_SPEND_MAX + 1, "kit", NULL, NULL) == 0);
    assert(ApiSpendCurrency(mod, HF_CURRENCY_REQUISITION, 10, "", NULL, NULL) == 0);
    g_csState = HF_STATE_MISSION;
    assert(ApiSpendCurrency(mod, HF_CURRENCY_REQUISITION, 10, "kit", NULL, NULL) == 0);
    g_csState = HF_STATE_SHIP;

    /* spent: the backend took it, then the game's copy, and the mod hears the balance left */
    assert(SpendOnce(mod, HF_CURRENCY_REQUISITION, 1500, "Lenna \"level 2\"", 200, "{\"result\":\"spent\",\"balance\":3500}  ") == HF_SPEND_SPENT);
    assert(g_csBalance == 3500 && g_pgRemoves == removes + 1 && PgAmount(manager, requisition) == 3500);
    assert(strstr(g_csBody, "\"mod\":\"yui.kit\"") && strstr(g_csBody, "\"amount\":1500") && strstr(g_csBody, "Lenna \\\"level 2\\\""));
    int64_t held = 0;
    assert(ApiCurrencyBalance(mod, HF_CURRENCY_REQUISITION, &held) == 0 && held == 3500);
    assert(ApiCurrencyBalance(mod, HF_CURRENCY_MEDALS, &held) == 1 && ApiCurrencyBalance(mod, 0x7001, &held) == 1);

    /* a reason cut inside a character loses that character, not the JSON */
    char cut[] = "ab\xE2\x82";
    SpendTrimUtf8(cut);
    assert(strcmp(cut, "ab") == 0);
    char whole[] = "ab\xE2\x82\xAC";
    SpendTrimUtf8(whole);
    assert(strlen(whole) == 5);

    /* too little, or no answer: the game's copy is left alone */
    assert(SpendOnce(mod, HF_CURRENCY_REQUISITION, 9000, "kit", 200, "{\"result\":\"too_little\",\"balance\":3500}") == HF_SPEND_TOO_LITTLE);
    assert(g_csBalance == 3500 && g_pgRemoves == removes + 1 && PgAmount(manager, requisition) == 3500);
    assert(SpendOnce(mod, HF_CURRENCY_REQUISITION, 10, "kit", 0, "") == HF_SPEND_FAILED && g_csBalance == -1);
    assert(SpendOnce(mod, HF_CURRENCY_REQUISITION, 10, "kit", 400, "{\"errorCode\":\"HELLFORGE_BAD_SPEND\"}") == HF_SPEND_FAILED);
    assert(g_pgRemoves == removes + 1);

    /* a mod taken out before its answer is not called */
    g_csStatus = 200;
    g_csReply = "{\"result\":\"spent\",\"balance\":3490}";
    int calls = g_csCalls;
    assert(ApiSpendCurrency(mod, HF_CURRENCY_REQUISITION, 10, "kit", SpendAnswered, &g_csCalls) != 0);
    mod->removing = 1;
    for (int i = 0; i < 50; i++) { Sleep(10); CurrencyTick(); }
    assert(g_csCalls == calls && PgAmount(manager, requisition) == 3490);
    mod->removing = 0;

    g_paidGuardPort = port;
    g_modCount = mods;
    g_spendPost = SpendPostHttp;
    g_spendState = SpendGameState;
    g_ownAdd = NULL; g_ownSort = NULL; g_ownRemove = NULL; g_ownManager = NULL; g_ownApply = NULL;
    free(g_fakeCatalogue);
    g_fakeCatalogue = NULL;
    printf("  currency: a mod's spend is asked of the backend, then taken from the game's copy only when the backend took it; "
           "refusals, too little and no answer leave it; the mod hears the balance on the game thread\n");
}
