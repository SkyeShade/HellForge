/* The paid-item guard (paid_guard.c, paid_guard_step.c): the protected functions, the backend's reply, and the take-back
 * on a fake item table, on the vanilla layout and a moved one. Included by stats_test.c after live_content_test.c (its
 * fake catalogue). No backend runs: port 0 keeps the worker from starting, and each test sets the list itself. */

/* The game's remove as it is (0x136F5E0): the amount down to 0 and state 1 for an amount; nothing at amount 0. */
static int g_pgRemoves;
static void PgRemove(uintptr_t manager, uint32_t mixId, uint32_t amount)
{
    g_pgRemoves++;
    int entry = OwnEntry(manager, mixId, 0, 0);
    uint8_t* record = (uint8_t*)OwnRecordAt(manager, entry);
    int32_t* have = (int32_t*)(record + 0xB0);
    if (*have <= 0) return;
    *have = *have > (int32_t)amount ? *have - (int32_t)amount : 0;
    *(uint32_t*)(record + 0x14) = *have > 0 ? 2 : 1;
}
/* A mod's hook on remove that does nothing. */
static void PgRemoveHooked(uintptr_t manager, uint32_t mixId, uint32_t amount) { g_pgRemoves++; }

static void PgList(const uint32_t* ids, int count)
{
    free(g_paidGuardRefused);
    g_paidGuardRefused = (uint32_t*)malloc(sizeof *ids * (size_t)(count ? count : 1));
    memcpy(g_paidGuardRefused, ids, sizeof *ids * (size_t)count);
    g_paidGuardRefusedCount = count;
    g_paidGuardAnswers++;
}

static void PgSet(uintptr_t manager, int entry, uint32_t state, int32_t amount)
{
    *(uint32_t*)(OwnRecordAt(manager, entry) + 0x14) = state;
    *(int32_t*)(OwnRecordAt(manager, entry) + 0xB0) = amount;
}
static uint32_t PgState(uintptr_t manager, int entry) { return *(const uint32_t*)(OwnRecordAt(manager, entry) + 0x14); }
static int32_t PgAmount(uintptr_t manager, int entry) { return *(const int32_t*)(OwnRecordAt(manager, entry) + 0xB0); }

/* The table: a free item held, a paid weapon with a child, a paid stratagem's item and its category-10 child. */
static uintptr_t PgTable(size_t bytes)
{
    g_fakeCatalogue = (uint8_t*)calloc(1, bytes);
    assert(g_fakeCatalogue != NULL);
    *(uint32_t*)(g_fakeCatalogue + OWN_CATALOGUE) = 2;
    *(uint32_t*)(g_fakeCatalogue + OWN_INVENTORY) = 2;
    FakeOwnAdd(0, 0x7001, 0x7001, 0, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 1, 1);
    FakeOwnAdd(0, 0x9000, 0x9000, 0, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 1, 1);
    FakeOwnAdd(0, 0x9001, 0x9001, 0x9000, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 2, 1);
    FakeOwnAdd(0, 0x9100, 0x9100, 0, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 11, 1);
    FakeOwnAdd(0, 0x9101, 0x3333, 0x9100, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 10, 1);
    uintptr_t manager = (uintptr_t)g_fakeCatalogue;
    for (int i = 0; i < 5; i++) PgSet(manager, i, 1, 0);
    PgSet(manager, 0, 2, 1);
    return manager;
}

static void PgReset(void)
{
    g_pgWatchTableCount = 0; g_pgWatchAnswers = 0; g_pgWatchRecords = 0; g_pgSuspect = -1; g_pgTaken = 0; g_pgRemoves = 0;
    g_paidGuardAnswers = 0;
}

static void PgTakeBackOn(size_t bytes)
{
    PgReset();
    uintptr_t manager = PgTable(bytes);
    static const uint32_t refused[] = {0x9000, 0x9001, 0x9100, 0x9101};
    g_ownRemove = PgRemove;
    assert(PaidGuardPass(manager) == 0); /* no list yet */
    PgList(refused, 4);
    assert(PaidGuardPass(manager) == 0 && g_pgSuspect < 0); /* nothing refused is held */

    /* a mod adds the paid weapon: found, then taken back only after a newer list still refuses it */
    PgSet(manager, 1, 2, 1);
    assert(PaidGuardPass(manager) == 0 && g_pgSuspect == g_paidGuardAnswers);
    assert(PaidGuardPass(manager) == 0 && PgState(manager, 1) == 2); /* waits for the fresh list */
    PgList(refused, 4);
    assert(PaidGuardPass(manager) == 1 && g_pgRemoves == 1 && PgState(manager, 1) == 1 && PgAmount(manager, 1) == 0);
    assert(PgState(manager, 0) == 2 && PgAmount(manager, 0) == 1); /* the free item stays */

    /* a stratagem held through its child at state 4 with no amount (0x136F790): no remove, state 1 as the game's undo */
    PgSet(manager, 4, 4, 0);
    assert(PaidGuardPass(manager) == 0);
    PgList(refused, 4);
    assert(PaidGuardPass(manager) == 1 && g_pgRemoves == 1 && PgState(manager, 4) == 1);

    /* a hooked remove that does nothing: the amount and state written as remove leaves them */
    g_ownRemove = PgRemoveHooked;
    PgSet(manager, 2, 2, 3);
    assert(PaidGuardPass(manager) == 0);
    PgList(refused, 4);
    assert(PaidGuardPass(manager) == 1 && g_pgRemoves == 2 && PgState(manager, 2) == 1 && PgAmount(manager, 2) == 0);

    /* a token just bought: held before the old list knows, the fresh list allows it, so it stays */
    g_ownRemove = PgRemove;
    PgSet(manager, 3, 2, 1);
    assert(PaidGuardPass(manager) == 0 && g_pgSuspect >= 0);
    static const uint32_t allowed[] = {0x9000, 0x9001, 0x9101};
    PgList(allowed, 3);
    assert(PaidGuardPass(manager) == 0 && g_pgSuspect < 0 && PgState(manager, 3) == 2);

    /* an item added to the table later is watched too */
    FakeOwnAdd(0, 0x9200, 0x9200, 0, 0, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0, 1, 1);
    static const uint32_t more[] = {0x9000, 0x9001, 0x9101, 0x9200};
    PgList(more, 4);
    PgSet(manager, 5, 2, 1);
    assert(PaidGuardPass(manager) == 0);
    PgList(more, 4);
    assert(PaidGuardPass(manager) == 1 && PgState(manager, 5) == 1);

    g_ownRemove = NULL;
    free(g_fakeCatalogue);
    g_fakeCatalogue = NULL;
}

static void PaidGuardFixture(void)
{
    /* the protected functions: an ownership check refused at its first byte and its last, not the byte past it; the
     * item table's add stays open; another build has none */
    uintptr_t game = 0x10000000u;
    assert(PaidGuardProtected(game, game + 0x136FB40u, 1) != NULL && PaidGuardProtected(game, game + 0x136FBC2u, 1) != NULL);
    assert(PaidGuardProtected(game, game + 0x136FBC3u, 1) == NULL && PaidGuardProtected(game, game + 0x136FB3Fu, 1) == NULL);
    assert(PaidGuardProtected(game, game + 0x136FB3Fu, 2) != NULL); /* a write that runs into it */
    assert(PaidGuardProtected(game, game + 0x136EF80u, 16) == NULL && PaidGuardProtected(game, game + 0x1750870u, 5) != NULL);
    assert(PaidGuardProtected(0, 0x136FB40u, 1) == NULL);
    strcpy(g_build, "99999999");
    assert(PaidGuardProtected(game, game + 0x136FB40u, 1) == NULL);
    strcpy(g_build, "25480438");

    /* the backend's reply */
    uint32_t ids[8];
    static const char good[] = "{\"refused\":[1,5,4294967295]}";
    assert(PaidGuardParse(good, sizeof good - 1, ids, 8) == 3 && ids[0] == 1 && ids[2] == 4294967295u);
    static const char empty[] = "{\"refused\":[]}";
    assert(PaidGuardParse(empty, sizeof empty - 1, ids, 8) == 0);
    static const char* const bad[] = {"{\"refused\":[5,1]}", "{\"refused\":[4294967296]}", "{\"refused\":[1,2", "{\"other\":[1]}",
        "{\"refused\":[1,x]}", "{\"refused\":[1,1]}", "{\"refused\":[-1]}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) assert(PaidGuardParse(bad[i], strlen(bad[i]), ids, 8) == -1);
    static const char many[] = "{\"refused\":[1,2,3]}";
    assert(PaidGuardParse(many, sizeof many - 1, ids, 2) == -1);

    /* the plan's port */
    char portKey[] = "backend_port", portValue[] = "41234", badPort[] = "70000";
    char* line[2] = {portKey, portValue};
    assert(PaidGuardPlanLine(line, 2) && g_paidGuardPort == 41234);
    line[1] = badPort;
    assert(PaidGuardPlanLine(line, 2) && g_paidGuardPort == 0);

    /* the take-back, on the vanilla layout and on a moved catalogue (state_moves.c) */
    CatalogueLayout vanilla = g_catalogue;
    PgTakeBackOn(0xDDCF8 + 4);
    g_catalogue = (CatalogueLayout)MOVE_LAYOUT(MOVE_CATALOGUE_NEW, MOVE_ITEMS);
    PgTakeBackOn(MOVE_CATALOGUE_NEW + MOVE_CATALOGUE_BYTES);
    g_catalogue = vanilla;
    free(g_paidGuardRefused);
    g_paidGuardRefused = NULL;
    g_paidGuardRefusedCount = 0;
    PgReset();
    printf("  paid guard: mods may not hook or write the ownership checks or paid purchases; a paid item the backend refuses is "
           "taken back once a fresh list still refuses it (a remove, or the state the game's undo sets), a just-bought one stays\n");
}
