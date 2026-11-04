/* Live mod changes (live_mods.c): stats mods added, removed and replaced in a running session, each keeping its place in
 * load order (a later mod's value and Local Stats' stay on top; removing them shows what was under them); nothing written
 * under a code mod's deferred edit; a live edit's reset value following the stats under it; an edit's expect checked
 * against the field's bottom; retired slots reused; refusals (a friend's lobby, another player, busy, a code mod, own,
 * a table not loaded yet, the order) leaving everything as it was. Included by stats_test.c. */

static uint8_t* g_txnState;
static int g_txnPlayers = 1;
static uint8_t* TxnFixtureState(void) { return g_txnState; }
static int TxnFixturePlayers(void) { return g_txnPlayers; }
static uint32_t g_txnGameState = HF_STATE_SHIP;
static uint32_t TxnFixtureGameState(void) { return g_txnGameState; }

static void TxnSend(const char* text, char* reply, size_t size)
{
    char line[1024];
    snprintf(line, sizeof line, "%s", text);
    LiveRequest(line, reply, size);
}

/* Sends a change's lines, each but the last answered ok; the last one's reply in *reply. */
static void TxnRun(const char* const* lines, int count, char* reply, size_t size)
{
    for (int i = 0; i < count; i++)
    {
        TxnSend(lines[i], reply, size);
        if (i < count - 1 && strstr(reply, "\tok") == NULL) { printf("  live mods: %s -> %s\n", lines[i], reply); assert(0); }
    }
}

/* What the loader queued to send on its own, then forgotten. */
static void TxnSent(char* out, size_t size)
{
    AcquireSRWLockExclusive(&g_liveOutLock);
    size_t n = g_liveOutUsed < size - 1 ? g_liveOutUsed : size - 1;
    memcpy(out, g_liveOut, n);
    out[n] = 0;
    g_liveOutUsed = 0;
    ReleaseSRWLockExclusive(&g_liveOutLock);
}

static int TxnRefused(const char* reply, const char* reason)
{
    char want[64];
    snprintf(want, sizeof want, "\trefused\t%s\t", reason);
    return strstr(reply, want) != NULL;
}

/* The change was refused at its commit for that reason (its modresult), its sent lines then forgotten. */
static int TxnResult(const char* txn, const char* reason)
{
    char sent[2048], want[96];
    TxnSent(sent, sizeof sent);
    snprintf(want, sizeof want, "modresult\t%s\trefused\t%s\t", txn, reason);
    if (strstr(sent, want) == NULL) printf("  live mods: wanted %s, sent: %s\n", want, sent);
    return strstr(sent, want) != NULL && strstr(sent, "modstate") == NULL;
}

static void LiveModsFixture(void)
{
    /* the session: a stats mod a, a code mod b, Local Stats on top; one component record of entity 1 */
    HfMod saved[8];
    memcpy(saved, g_mods, sizeof saved);
    int savedMods = g_modCount, savedComponents = g_componentCount, savedStats = g_statCount, savedEdits = g_editCount;
    int savedTable = g_statsTablesDone[1];
    Component savedComponent = g_components[0];
    memset(g_mods, 0, sizeof saved);
    const char* ids[] = {"a", "b", "zz.local.stats"};
    for (int i = 0; i < 3; i++)
    {
        strcpy(g_mods[i].id, ids[i]);
        g_mods[i].loaded = 1;
        g_mods[i].log = INVALID_HANDLE_VALUE;
        g_mods[i].order = i;
        wcscpy(g_mods[i].dll, i == 1 ? L"b.dll" : L"-");
    }
    for (int i = 3; i < 8; i++) g_mods[i].order = i;
    g_modCount = 3;
    Component* c = &g_components[0];
    g_componentCount = 1;
    memset(c, 0, sizeof *c);
    strcpy(c->name, "Fixture"); c->index = 999; c->indices = 4; c->records = 1; c->recordOffset = 64; c->stride = 16;
    uint32_t extent = 80;
    memcpy(c->header + 16, &extent, 4);
    g_txnState = calloc(1, REG_TABLES + 8 * 1000);
    uint8_t* block = calloc(1, 112);
    uint8_t* table = block + 32;
    memcpy(block, &c->index, 4);
    memcpy(block + 4, c->header, 28);
    *(uint64_t*)(table + 16) = 1; /* entity 1 owns record 0 */
    assert(!SetStatePointer(g_txnState, REG_TABLES + 8 * c->index, table));
    uint32_t* record = (uint32_t*)(table + 64);
    record[0] = 10; record[1] = 20; record[2] = 30;
    /* the boot pass: a sets +0 to 30, Local Stats +4 to 77 */
    g_stats[0] = Edit(0, 30, 0, 0); g_stats[0].hasExpect = 1; g_stats[0].expect = 10;
    g_stats[1] = Edit(2, 77, 4, 0);
    g_statCount = 2;
    g_editCount = 0;
    FixtureLoad(g_txnState);
    assert(record[0] == 30 && record[1] == 77 && g_statsComponentsReady);
    g_liveEntityState = TxnFixtureState;
    g_livePlayers = TxnFixturePlayers;
    g_liveGameState = TxnFixtureGameState;
    g_txnInline = 1; /* each commit made before modcommit's reply */
    AcquireSRWLockExclusive(&g_liveOutLock);
    g_liveConnected = 1;
    g_liveOutUsed = 0;
    ReleaseSRWLockExclusive(&g_liveOutLock);
    char reply[512], sent[2048];

    /* Added between a and b: over a's value, under Local Stats'; a line whose expect is not the field's bottom refused. */
    const char* add[] = {"modbegin\t1\tt1\tsolo\tdigest1", "modorder\t2\tt1\t0\ta", "modorder\t3\tt1\t1\tc", "modorder\t4\tt1\t2\tb",
        "modorder\t5\tt1\t3\tzz.local.stats", "modadd\t6\tt1\tc\tstats\t0123456789abcdef\tC:\\mods\\c\t-",
        "modstat\t7\tt1\tc\tcomponent\tFixture\t0000000000000001\t0\t0\tu32\t00000032\t0000000A\t0\t1\t0\tc +0",
        "modstat\t8\tt1\tc\tcomponent\tFixture\t0000000000000001\t0\t4\tu32\t0000003C\t-\t0\t1\t0\tc +4",
        "modstat\t9\tt1\tc\tcomponent\tFixture\t0000000000000001\t0\t8\tu32\t00000063\t000003E7\t0\t1\t0\tc +8 expects 999",
        "modcommit\t10\tt1"};
    TxnRun(add, 10, reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    assert(!strcmp(reply, "10\tok\tqueued") && record[0] == 50 && record[1] == 77 && record[2] == 30);
    assert(strstr(sent, "modstate\tt1\tc\tadded\t2\t1\n") && strstr(sent, "modresult\tt1\tok\n") && g_modCount == 4 && g_mods[3].order == 1);

    /* Local Stats removed: c's value under it shows. */
    const char* removeLocal[] = {"modbegin\t11\tt2\tsolo\tdigest2", "modorder\t12\tt2\t0\ta", "modorder\t13\tt2\t1\tc",
        "modorder\t14\tt2\t2\tb", "modremove\t15\tt2\tzz.local.stats", "modcommit\t16\tt2"};
    TxnRun(removeLocal, 6, reply, sizeof reply);
    assert(!strcmp(reply, "16\tok\tqueued") && record[1] == 60 && !g_mods[2].loaded);

    /* c replaced by a version that sets +0 only: +0 takes the new value, +4 goes back to the game's own. */
    const char* replace[] = {"modbegin\t17\tt3\tsolo\tdigest3", "modorder\t18\tt3\t0\ta", "modorder\t19\tt3\t1\tc", "modorder\t20\tt3\t2\tb",
        "modreplace\t21\tt3\tc\tstats\tfedcba9876543210\tC:\\mods\\c\t-",
        "modstat\t22\tt3\tc\tcomponent\tFixture\t0000000000000001\t0\t0\tu32\t00000037\t-\t0\t1\t0\tc +0",
        "modcommit\t23\tt3"};
    TxnRun(replace, 7, reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    assert(!strcmp(reply, "23\tok\tqueued") && record[0] == 55 && record[1] == 20 && strstr(sent, "modstate\tt3\tc\treplaced\t1\t0\n"));

    /* Under a code mod's deferred edit nothing is written; the change reuses a retired slot. */
    static const SdkField field = {"fixture", 0, 4, HF_TYPE_U32, 0, 0};
    assert(EditRoom());
    FieldEdit deferred = {&g_mods[1], EDIT_SET, 1, 0, NULL, &field};
    deferred.address = (uintptr_t)&record[0];
    g_edits[g_editCount++] = deferred;
    record[0] = 1234;
    int stats = g_statCount;
    const char* under[] = {"modbegin\t24\tt4\tsolo\tdigest4", "modorder\t25\tt4\t0\ta", "modorder\t26\tt4\t1\tc", "modorder\t27\tt4\t2\td",
        "modorder\t28\tt4\t3\tb", "modadd\t29\tt4\td\tstats\t1111111111111111\tC:\\mods\\d\t-",
        "modstat\t30\tt4\td\tcomponent\tFixture\t0000000000000001\t0\t0\tu32\t00000063\t-\t0\t1\t0\td +0", "modcommit\t31\tt4"};
    TxnRun(under, 8, reply, sizeof reply);
    assert(!strcmp(reply, "31\tok\tqueued") && record[0] == 1234 && g_statCount == stats);

    /* Under a live edit: its reset value follows the stats (d removed: c's 55), the field keeps the live value. */
    FieldEdit* live = &g_edits[0];
    live->live = 1;
    live->hasOriginal = 1;
    uint32_t top = 99, liveValue = 500;
    memcpy(live->original, &top, 4);
    memcpy(live->value, &liveValue, 4);
    record[0] = 500;
    const char* removeD[] = {"modbegin\t32\tt5\tsolo\tdigest5", "modorder\t33\tt5\t0\ta", "modorder\t34\tt5\t1\tc", "modorder\t35\tt5\t2\tb",
        "modremove\t36\tt5\td", "modcommit\t37\tt5"};
    TxnRun(removeD, 6, reply, sizeof reply);
    uint32_t restored;
    memcpy(&restored, live->original, 4);
    assert(!strcmp(reply, "37\tok\tqueued") && record[0] == 500 && restored == 55);
    g_editCount = 0;
    record[0] = 55;
    TxnSent(sent, sizeof sent);

    /* Refusals: nothing changes. (The boot's events have fired: a code mod is weighed past the ship's gate.) */
    LONG firedBefore[5];
    for (int event = 0; event < 5; event++) { firedBefore[event] = g_fired[event]; InterlockedExchange(&g_fired[event], 1); }
    int mods = g_modCount;
    SetSession(HF_SESSION_JOINER, HF_STATS_HOST, "Host");
    TxnSend("modbegin\t40\tr1\tsolo\tx", reply, sizeof reply);
    assert(TxnRefused(reply, "coop"));
    SetSession(HF_SESSION_SOLO, HF_STATS_OWN, "");
    g_txnPlayers = 2;
    TxnSend("modbegin\t41\tr2\tsolo\tx", reply, sizeof reply);
    assert(TxnRefused(reply, "coop"));
    g_txnPlayers = 1;
    TxnSend("modbegin\t42\tr3\tsolo\tx", reply, sizeof reply);
    assert(!strcmp(reply, "42\tok\tnow"));
    TxnSend("modbegin\t43\tr4\tsolo\tx", reply, sizeof reply);
    assert(TxnRefused(reply, "busy"));
    TxnSend("modadd\t44\tr3\tx\tcode\t2222222222222222\tC:\\mods\\x\tC:\\mods\\x\\x.dll", reply, sizeof reply);
    assert(!strcmp(reply, "44\tok\tnow")); /* a code mod is weighed at the commit */
    TxnSend("modstat\t45\tr3\tnobody\tcomponent\tFixture\t0000000000000001\t0\t0\tu32\t00000001\t-\t0\t1\t0\tx", reply, sizeof reply);
    assert(TxnRefused(reply, "malformed") && !g_txn.open);

    /* refused at the commit: queued, then a modresult saying why, and nothing changed (the order neither: refused before
     * it set the order, it does not put back an earlier change's) */
    int orders[8];
    assert(g_modCount <= 8);
    for (int i = 0; i < g_modCount; i++) orders[i] = g_mods[i].order;
    const char* removeCode[] = {"modbegin\t46\tr5\tsolo\tx", "modorder\t47\tr5\t0\ta", "modorder\t48\tr5\t1\tc", "modremove\t49\tr5\tb",
        "modcommit\t50\tr5"};
    TxnRun(removeCode, 5, reply, sizeof reply);
    assert(!strcmp(reply, "50\tok\tqueued") && TxnResult("r5", "capability") && g_mods[1].loaded);
    for (int i = 0; i < g_modCount; i++) assert(g_mods[i].order == orders[i]);
    const char* own[] = {"modbegin\t51\tr6\tsolo\tx", "modorder\t52\tr6\t0\ta", "modorder\t53\tr6\t1\tc", "modorder\t54\tr6\t2\tb",
        "modorder\t55\tr6\t3\te", "modadd\t56\tr6\te\tstats\t3333333333333333\tC:\\mods\\e\t-",
        "modstat\t57\tr6\te\tcomponent\tFixture\t0000000000000001\t0\t0\tu32\t00000001\t-\t1\t0\t0\te own", "modcommit\t58\tr6"};
    TxnRun(own, 8, reply, sizeof reply);
    assert(TxnResult("r6", "capability")); /* its record holds a's and c's values: a copy would leave them behind */
    g_statsTablesDone[1] = 0;
    const char* later[] = {"modbegin\t59\tr7\tsolo\tx", "modorder\t60\tr7\t0\ta", "modorder\t61\tr7\t1\tc", "modorder\t62\tr7\t2\tb",
        "modorder\t63\tr7\t3\tf", "modadd\t64\tr7\tf\tstats\t4444444444444444\tC:\\mods\\f\t-",
        "modstat\t65\tr7\tf\tDamageInfo\t-\t0000000000000000\t54\t4\tu32\t00000096\t-\t0\t0\t0\tf damage", "modcommit\t66\tr7"};
    TxnRun(later, 8, reply, sizeof reply);
    assert(TxnResult("r7", "state"));
    g_statsTablesDone[1] = savedTable;
    const char* leftOut[] = {"modbegin\t67\tr8\tsolo\tx", "modorder\t68\tr8\t0\ta", "modorder\t69\tr8\t1\tg",
        "modadd\t70\tr8\tg\tstats\t5555555555555555\tC:\\mods\\g\t-", "modcommit\t71\tr8"};
    TxnRun(leftOut, 5, reply, sizeof reply);
    assert(TxnResult("r8", "malformed"));
    const char* reordered[] = {"modbegin\t72\tr9\tsolo\tx", "modorder\t73\tr9\t0\tc", "modorder\t74\tr9\t1\ta", "modorder\t75\tr9\t2\tb",
        "modcommit\t76\tr9"};
    TxnRun(reordered, 5, reply, sizeof reply);
    assert(TxnResult("r9", "capability"));
    /* the game loading: refused at the begin */
    g_txnGameState = HF_STATE_PREPARE_MISSION;
    TxnSend("modbegin\t80\tr12\tsolo\tx", reply, sizeof reply);
    assert(TxnRefused(reply, "state"));
    g_txnGameState = HF_STATE_SHIP;
    assert(g_modCount == mods && record[0] == 55 && record[1] == 20 && record[2] == 30);
    for (int event = 0; event < 5; event++) InterlockedExchange(&g_fired[event], firedBefore[event]);
    /* a change the backend withdraws, and one dropped when the backend goes */
    TxnSend("modbegin\t77\tr10\tsolo\tx", reply, sizeof reply);
    TxnSend("modabort\t78\tr10", reply, sizeof reply);
    assert(!strcmp(reply, "78\tok\tnow") && !g_txn.open);
    TxnSend("modbegin\t79\tr11\tsolo\tx", reply, sizeof reply);
    LiveModsAbort("the backend disconnected");
    assert(!g_txn.open);
    printf("  live mods: added over a and under Local Stats, Local Stats removed, replaced, kept under a code mod's edit and a live edit, 10 refusals\n");

    AcquireSRWLockExclusive(&g_liveOutLock);
    g_liveConnected = 0;
    g_liveOutUsed = 0;
    ReleaseSRWLockExclusive(&g_liveOutLock);
    g_liveEntityState = EntityState;
    g_livePlayers = LivePlayers;
    g_liveGameState = LiveGameState;
    g_txnInline = 0;
    free(g_txnState);
    free(block);
    memcpy(g_mods, saved, sizeof saved);
    g_modCount = savedMods;
    g_components[0] = savedComponent;
    g_componentCount = savedComponents;
    g_statCount = savedStats;
    g_editCount = savedEdits;
    InterlockedExchange(&g_statsComponentsReady, 0);
}

/* "own" lines live (live_own.c, M6): an entity whose record nothing holds gets a record of its own, the table copied at
 * the first (a line of the change at that record follows into the copy); a second entity's copy goes into HellForge's
 * copy; the vanilla records stay as they were; removing the mod leaves the copy with the field's bottom. */
static void OwnLiveFixture(void)
{
    HfMod saved[4];
    memcpy(saved, g_mods, sizeof saved);
    int savedMods = g_modCount, savedComponents = g_componentCount, savedStats = g_statCount, savedEdits = g_editCount, savedOwned = g_ownedCount;
    Component savedComponent = g_components[0];
    memset(g_mods, 0, sizeof saved);
    strcpy(g_mods[0].id, "a"); g_mods[0].loaded = 1; g_mods[0].log = INVALID_HANDLE_VALUE; wcscpy(g_mods[0].dll, L"-");
    g_modCount = 1;
    g_statCount = g_editCount = 0;
    Component* c = &g_components[0];
    g_componentCount = 1;
    memset(c, 0, sizeof *c);
    strcpy(c->name, "Fixture"); c->index = 998; c->indices = 4; c->records = 3; c->recordOffset = 64; c->stride = 16;
    uint32_t extent = 112;
    memcpy(c->header + 16, &extent, 4);
    g_txnState = calloc(1, REG_TABLES + 8 * 1000);
    uint8_t* block = calloc(1, 32 + 112);
    uint8_t* table = block + 32;
    memcpy(block, &c->index, 4);
    memcpy(block + 4, c->header, 28);
    *(uint64_t*)(table + 16) = 1;                                            /* entity 1: record 0 */
    *(uint64_t*)(table + 32) = 2; *(uint32_t*)(table + 40) = 1;              /* entity 2: record 1 */
    *(uint64_t*)(table + 48) = 3; *(uint32_t*)(table + 56) = 2;              /* entity 3: record 2 */
    uint32_t* records = (uint32_t*)(table + 64);
    records[0] = 10; records[4] = 20; records[5] = 21; records[9] = 9;
    assert(!SetStatePointer(g_txnState, REG_TABLES + 8 * c->index, table));
    /* the boot pass: a sets entity 3's +4 to 0x55 (the table's first copy takes it along) */
    g_stats[0] = Edit(0, 0x55, 4, 0);
    g_stats[0].resource = 3; g_stats[0].record = 2;
    g_statCount = 1;
    FixtureLoad(g_txnState);
    assert(records[9] == 0x55 && g_stats[0].applied && g_stats[0].address == (uintptr_t)&records[9]);
    g_liveEntityState = TxnFixtureState;
    g_livePlayers = TxnFixturePlayers;
    g_liveGameState = TxnFixtureGameState;
    g_txnInline = 1;
    AcquireSRWLockExclusive(&g_liveOutLock); g_liveConnected = 1; g_liveOutUsed = 0; ReleaseSRWLockExclusive(&g_liveOutLock);
    char reply[512], sent[2048];

    /* entity 2's own +0 and a plain +4 of it: the table copied, entity 2 pointed at its copy, both values there */
    const char* first[] = {"modbegin\t1\to1\tsolo\tx", "modorder\t2\to1\t0\ta", "modorder\t3\to1\t1\te",
        "modadd\t4\to1\te\tstats\t3333333333333333\tC:\\mods\\e\t-",
        "modstat\t5\to1\te\tcomponent\tFixture\t0000000000000002\t1\t0\tu32\t00000063\t-\t1\t0\t0\te own",
        "modstat\t6\to1\te\tcomponent\tFixture\t0000000000000002\t1\t4\tu32\t00000064\t-\t0\t0\t0\te plain", "modcommit\t7\to1"};
    TxnRun(first, 7, reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    uint8_t* copy = StatePointer(g_txnState, REG_TABLES + 8 * c->index);
    uint32_t mine;
    memcpy(&mine, FindRow(copy, 4, 16, 2) + 8, 4);
    const uint32_t* own = (const uint32_t*)(copy + 64 + (size_t)mine * 16);
    if (strstr(sent, "modresult\to1\tok\n") == NULL) fprintf(stderr, "  own live: sent %s\n", sent);
    assert(strstr(sent, "modresult\to1\tok\n") && copy != table && g_ownedCount == savedOwned + 1 && mine >= 3);
    assert(own[0] == 0x63 && own[1] == 0x64 && records[4] == 20 && records[5] == 21);
    uint32_t* held = (uint32_t*)(copy + 64 + 2 * 16 + 4);
    assert(*held == 0x55 && g_stats[0].address == (uintptr_t)held); /* a's entry followed into the copy */
    /* entity 1's own +0: into HellForge's copy, no move */
    const char* second[] = {"modbegin\t8\to2\tsolo\tx", "modorder\t9\to2\t0\ta", "modorder\t10\to2\t1\te", "modorder\t11\to2\t2\tf",
        "modadd\t12\to2\tf\tstats\t4444444444444444\tC:\\mods\\f\t-",
        "modstat\t13\to2\tf\tcomponent\tFixture\t0000000000000001\t0\t0\tu32\t00000077\t-\t1\t0\t0\tf own", "modcommit\t14\to2"};
    TxnRun(second, 7, reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    uint32_t one;
    memcpy(&one, FindRow(copy, 4, 16, 1) + 8, 4);
    assert(strstr(sent, "modresult\to2\tok\n") && StatePointer(g_txnState, REG_TABLES + 8 * c->index) == copy && one > mine &&
        *(const uint32_t*)(copy + 64 + (size_t)one * 16) == 0x77 && *(const uint32_t*)(copy + 64) == 10);
    /* e out: entity 2 keeps its copy, its fields back to their bottoms */
    const char* removeE[] = {"modbegin\t15\to3\tsolo\tx", "modorder\t16\to3\t0\ta", "modorder\t17\to3\t1\tf", "modremove\t18\to3\te",
        "modcommit\t19\to3"};
    TxnRun(removeE, 5, reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    memcpy(&mine, FindRow(copy, 4, 16, 2) + 8, 4);
    own = (const uint32_t*)(copy + 64 + (size_t)mine * 16);
    assert(strstr(sent, "modresult\to3\tok\n") && mine >= 3 && own[0] == 20 && own[1] == 21);
    /* a out: its field goes back where the game reads it (the copy); the old table is left as it was */
    const char* removeA[] = {"modbegin\t20\to4\tsolo\tx", "modorder\t21\to4\t0\tf", "modremove\t22\to4\ta", "modcommit\t23\to4"};
    TxnRun(removeA, 4, reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    assert(strstr(sent, "modresult\to4\tok\n") && *held == 9 && records[9] == 0x55);
    printf("  live own records: the table copied at the first (a held field follows it), a second copy in it, a removed mod's copy keeps the bottoms\n");

    AcquireSRWLockExclusive(&g_liveOutLock); g_liveConnected = 0; g_liveOutUsed = 0; ReleaseSRWLockExclusive(&g_liveOutLock);
    g_liveEntityState = EntityState;
    g_livePlayers = LivePlayers;
    g_liveGameState = LiveGameState;
    g_txnInline = 0;
    free(g_txnState);
    memcpy(g_mods, saved, sizeof saved);
    g_modCount = savedMods;
    g_components[0] = savedComponent;
    g_componentCount = savedComponents;
    g_statCount = savedStats;
    g_editCount = savedEdits;
    g_ownedCount = savedOwned;
    InterlockedExchange(&g_statsComponentsReady, 0);
}

/* A live code mod (live_code.c, SDK 16): the real live_test_mod.dll set up while the game runs (its load events replayed,
 * its hook and its write in), replaced by a copy of itself (the old one's hook out and its write put back, its unload
 * told why), taken out (the game as before), and refused without "live" or when its DLL is missing. */
static LONG LiveTestValue(HMODULE module, const char* name) { return *(volatile LONG*)(void*)GetProcAddress(module, name); }

static void LiveCodeFixture(void)
{
    ApiInit(); /* the table a mod gets, as the loader builds it at its start */
    wchar_t self[MAX_PATH], v1[MAX_PATH], v2[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    *wcsrchr(self, L'\\') = 0;
    swprintf(v1, MAX_PATH, L"%ls\\live_test_mod.dll", self);
    swprintf(v2, MAX_PATH, L"%ls\\live_test_mod_v2.dll", self);
    assert(CopyFileW(v1, v2, FALSE));
    HMODULE game = LoadLibraryW(L"fake_game.dll");
    assert(game != NULL);
    typedef int (*Target)(int);
    Target target = (Target)(void*)GetProcAddress(game, "hellforge_fake_target");
    volatile uint32_t* cell = (volatile uint32_t*)(void*)GetProcAddress(game, "hellforge_fake_cell");
    assert(target(5) == 16 && *cell == 5);
    char path1[MAX_PATH * 3], path2[MAX_PATH * 3], folder[MAX_PATH * 3], line[2048], reply[512], sent[4096];
    WideCharToMultiByte(CP_UTF8, 0, v1, -1, path1, sizeof path1, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, v2, -1, path2, sizeof path2, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, self, -1, folder, sizeof folder, NULL, NULL);

    /* the game on the ship, every load event fired in the boot's order */
    LONG fired[HF_EVENT_SESSION_END + 1];
    for (int event = 0; event <= HF_EVENT_SESSION_END; event++) fired[event] = g_fired[event];
    HfEvent order[4] = {HF_EVENT_GAME_LOADED, HF_EVENT_PROJECTILES_READY, HF_EVENT_NETWORK_READY, HF_EVENT_ENTITIES_READY}, orderBefore[4];
    memcpy(orderBefore, g_fireOrder, sizeof orderBefore);
    LONG countBefore = g_fireCount;
    for (int event = HF_EVENT_GAME_LOADED; event <= HF_EVENT_PROJECTILES_READY; event++) InterlockedExchange(&g_fired[event], 1);
    memcpy(g_fireOrder, order, sizeof order);
    g_fireCount = 4;
    int mods = g_modCount;
    g_liveGameState = TxnFixtureGameState;
    g_livePlayers = TxnFixturePlayers;
    g_txnInline = 1;
    AcquireSRWLockExclusive(&g_liveOutLock); g_liveConnected = 1; g_liveOutUsed = 0; ReleaseSRWLockExclusive(&g_liveOutLock);

    /* added: set up, then the four load events, then MODS_CHANGED */
    TxnSend("modbegin\t1\tc1\tsync\tdigest", reply, sizeof reply);
    TxnSend("modorder\t2\tc1\t0\ta", reply, sizeof reply);
    TxnSend("modorder\t3\tc1\t1\tb", reply, sizeof reply);
    TxnSend("modorder\t4\tc1\t2\tlive.test", reply, sizeof reply);
    snprintf(line, sizeof line, "modadd\t5\tc1\tlive.test\tcode,live\t0123456789abcdef\t%s\t%s", folder, path1);
    TxnSend(line, reply, sizeof reply);
    assert(!strcmp(reply, "5\tok\tnow"));
    TxnSend("modcommit\t6\tc1", reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    HMODULE first = GetModuleHandleW(v1);
    if (strstr(sent, "modresult\tc1\tok\n") == NULL) fprintf(stderr, "  live code mods: sent %s\n", sent);
    assert(strstr(sent, "modresult\tc1\tok\n") && first != NULL);
    assert(LiveTestValue(first, "live_test_phase") == HF_LOAD_LIVE && LiveTestValue(first, "live_test_loaded") == 1 &&
        LiveTestValue(first, "live_test_changed") == 1);
    assert(target(5) == 23 && *cell == 42 && g_modCount == mods + 1 && g_mods[mods].loaded && g_mods[mods].order == 2);

    /* replaced by its copy: the old one unloaded (REPLACE), out of the chain, its write put back; the new one in */
    TxnSend("modbegin\t7\tc2\tsync\tdigest", reply, sizeof reply);
    TxnSend("modorder\t8\tc2\t0\ta", reply, sizeof reply);
    TxnSend("modorder\t9\tc2\t1\tb", reply, sizeof reply);
    TxnSend("modorder\t10\tc2\t2\tlive.test", reply, sizeof reply);
    snprintf(line, sizeof line, "modreplace\t11\tc2\tlive.test\tcode,live\tfedcba9876543210\t%s\t%s", folder, path2);
    TxnSend(line, reply, sizeof reply);
    TxnSend("modcommit\t12\tc2", reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    HMODULE second = GetModuleHandleW(v2);
    assert(strstr(sent, "modresult\tc2\tok\n") && second != NULL && second != first);
    assert(LiveTestValue(first, "live_test_unloaded") == HF_UNLOAD_REPLACE && LiveTestValue(first, "live_test_changed") == 1);
    assert(LiveTestValue(second, "live_test_loaded") == 1 && LiveTestValue(second, "live_test_changed") == 1);
    assert(target(5) == 23 && *cell == 42 && g_mods[mods].removed && !g_mods[mods].loaded && g_mods[mods + 1].loaded);

    /* taken out: the game as before it came */
    TxnSend("modbegin\t13\tc3\tsync\tdigest", reply, sizeof reply);
    TxnSend("modorder\t14\tc3\t0\ta", reply, sizeof reply);
    TxnSend("modorder\t15\tc3\t1\tb", reply, sizeof reply);
    TxnSend("modremove\t16\tc3\tlive.test", reply, sizeof reply);
    TxnSend("modcommit\t17\tc3", reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    assert(strstr(sent, "modresult\tc3\tok\n") && LiveTestValue(second, "live_test_unloaded") == HF_UNLOAD_REMOVE);
    assert(target(5) == 16 && *cell == 5 && !g_mods[mods + 1].loaded && LiveTestValue(second, "live_test_changed") == 1);

    /* refused: code without "live", and a DLL that is not there (its set-up taken out again, nothing changed) */
    TxnSend("modbegin\t18\tc4\tsync\tdigest", reply, sizeof reply);
    TxnSend("modorder\t19\tc4\t0\ta", reply, sizeof reply);
    TxnSend("modorder\t20\tc4\t1\tb", reply, sizeof reply);
    TxnSend("modorder\t21\tc4\t2\tlive.test", reply, sizeof reply);
    snprintf(line, sizeof line, "modadd\t22\tc4\tlive.test\tcode\t0123456789abcdef\t%s\t%s", folder, path1);
    TxnSend(line, reply, sizeof reply);
    TxnSend("modcommit\t23\tc4", reply, sizeof reply);
    assert(TxnResult("c4", "capability"));
    TxnSend("modbegin\t24\tc5\tsync\tdigest", reply, sizeof reply);
    TxnSend("modorder\t25\tc5\t0\ta", reply, sizeof reply);
    TxnSend("modorder\t26\tc5\t1\tb", reply, sizeof reply);
    TxnSend("modorder\t27\tc5\t2\tlive.test", reply, sizeof reply);
    snprintf(line, sizeof line, "modadd\t28\tc5\tlive.test\tcode,live\t0123456789abcdef\t%s\t%s\\no_such_mod.dll", folder, folder);
    TxnSend(line, reply, sizeof reply);
    TxnSend("modcommit\t29\tc5", reply, sizeof reply);
    assert(TxnResult("c5", "capability") && !g_mods[g_modCount - 1].loaded && g_mods[g_modCount - 1].removed && target(5) == 16 && *cell == 5);
    assert(g_modCount == mods + 2); /* its set-up took a taken-out entry of its id again, not a new one */

    /* set up again: a taken-out entry of its id again (the session's mods do not grow), then taken out again */
    TxnSend("modbegin\t30\tc6\tsync\tdigest", reply, sizeof reply);
    TxnSend("modorder\t31\tc6\t0\ta", reply, sizeof reply);
    TxnSend("modorder\t32\tc6\t1\tb", reply, sizeof reply);
    TxnSend("modorder\t33\tc6\t2\tlive.test", reply, sizeof reply);
    snprintf(line, sizeof line, "modadd\t34\tc6\tlive.test\tcode,live\tfedcba9876543210\t%s\t%s", folder, path2);
    TxnSend(line, reply, sizeof reply);
    TxnSend("modcommit\t35\tc6", reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    assert(strstr(sent, "modresult\tc6\tok\n") && g_modCount == mods + 2 && g_mods[mods].loaded && !g_mods[mods].removed && !g_mods[mods].staging &&
        g_mods[mods].module == second && target(5) == 23 && *cell == 42);
    TxnSend("modbegin\t36\tc7\tsync\tdigest", reply, sizeof reply);
    TxnSend("modorder\t37\tc7\t0\ta", reply, sizeof reply);
    TxnSend("modorder\t38\tc7\t1\tb", reply, sizeof reply);
    TxnSend("modremove\t39\tc7\tlive.test", reply, sizeof reply);
    TxnSend("modcommit\t40\tc7", reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    assert(strstr(sent, "modresult\tc7\tok\n") && g_mods[mods].removed && target(5) == 16 && *cell == 5);
    printf("  live code mods: set up while the game runs (events replayed, hook and write in), replaced, taken out, 2 refusals, set up again in "
        "its old entry\n");

    AcquireSRWLockExclusive(&g_liveOutLock); g_liveConnected = 0; g_liveOutUsed = 0; ReleaseSRWLockExclusive(&g_liveOutLock);
    g_liveGameState = LiveGameState;
    g_livePlayers = LivePlayers;
    g_txnInline = 0;
    for (int event = 0; event <= HF_EVENT_SESSION_END; event++) InterlockedExchange(&g_fired[event], fired[event]);
    memcpy(g_fireOrder, orderBefore, sizeof orderBefore);
    g_fireCount = countBefore;
    g_modCount = mods;
}

/* A mod's deferred edits taken out (live_code.c EditsForget, FieldRestack): of two mods' edits of one field, the earlier
 * one out keeps the later's value; the later one out shows what was under both; an edit queued in a live set-up goes
 * under a later mod's, in load order; under a live edit the field keeps the live value and the live edit's reset value
 * follows the edits under it. */
static void EditsForgetFixture(void)
{
    int savedComponents = g_componentCount, savedFake = g_fakeRecordCount, savedEdits = g_editCount, mods = g_modCount;
    Component savedComponent = g_components[0];
    LONG entities = g_fired[HF_EVENT_ENTITIES_READY];
    Component* c = &g_components[0];
    memset(c, 0, sizeof *c);
    strcpy(c->name, "WeaponMagazineComponentData"); c->index = 5; c->stride = 160;
    g_componentCount = 1;
    g_fieldRecord = FakeFieldRecord;
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], 1);
    uint8_t* record = FakeRecordOf("WeaponMagazineComponentData", 0x77, 160, 0);
    HfMod* m[3];
    for (int i = 0; i < 3; i++)
    {
        m[i] = &g_mods[mods + i];
        memset(m[i], 0, sizeof *m[i]);
        snprintf(m[i]->id, sizeof m[i]->id, "edits.%d", i);
        m[i]->loaded = 1; m[i]->log = INVALID_HANDLE_VALUE; m[i]->order = 100 + i; wcscpy(m[i]->dll, L"x.dll");
    }
    g_modCount = mods + 3;
    HfField capacity = HF_FIELD(WeaponMagazine, capacity);
    HfValue v200 = HF_U32(200), v300 = HF_U32(300), v999 = HF_U32(999);
    const SdkComponent* resolved = NULL;
    uint32_t at = ResolveField(m[0], "fixture", &capacity, &resolved)->offset, value;
    #define CAPACITY() (memcpy(&value, record + at, 4), value)
    memcpy(record + at, &(uint32_t){100}, 4);

    assert(!ApiSetField(m[0], 0x77, &capacity, &v200) && !ApiSetField(m[1], 0x77, &capacity, &v300) && CAPACITY() == 300);
    EditsForget(m[0]);
    assert(CAPACITY() == 300 && g_editCount == savedEdits + 1 && g_edits[savedEdits].hasBefore && *(uint32_t*)g_edits[savedEdits].before == 100);
    EditsForget(m[1]);
    assert(CAPACITY() == 100 && g_editCount == savedEdits);

    /* a live set-up's edit goes under the later mod's: the field keeps the later value */
    assert(!ApiSetField(m[1], 0x77, &capacity, &v300) && CAPACITY() == 300);
    g_liveSetup = m[0];
    assert(!ApiSetField(m[0], 0x77, &capacity, &v200));
    g_liveSetup = NULL;
    assert(CAPACITY() == 300 && g_editCount == savedEdits + 2 && g_edits[savedEdits].mod == m[0] && *(uint32_t*)g_edits[savedEdits + 1].before == 200);

    /* under a live edit: the field keeps it; its reset value is what the deferred edits now make */
    assert(ApiSetLive(m[2], 0x77, &capacity, &v999, 0) && CAPACITY() == 999);
    EditsForget(m[1]);
    FieldEdit* live = NULL;
    for (int i = 0; i < g_editCount; i++) if (g_edits[i].live) live = &g_edits[i];
    assert(CAPACITY() == 999 && live != NULL && *(uint32_t*)live->original == 200);
    EditsForget(m[2]);
    assert(CAPACITY() == 200);
    EditsForget(m[0]);
    assert(CAPACITY() == 100 && g_editCount == savedEdits);
    #undef CAPACITY
    printf("  live mods: a mod's deferred edits taken out (the later's value stays, the field's own comes back), a live set-up's edit in load order, under a live edit\n");

    free(record);
    g_fakeRecordCount = savedFake;
    g_fieldRecord = ComponentRecordOf;
    g_components[0] = savedComponent;
    g_componentCount = savedComponents;
    g_modCount = mods;
    InterlockedExchange(&g_fired[HF_EVENT_ENTITIES_READY], entities);
}

/* Network field types in the loader (nettypes.c): a plan's ask applied once the configuration loads, the widest of the
 * loaded mods' asks winning; a live change adding a wider ask, taking it out (back to the plan's), replacing a mod's
 * ask; an ask whose expectation is not the game's type leaves it alone. */
static void NetTypesFixture(void)
{
    HMODULE game = LoadLibraryW(L"fake_game.dll");
    assert(game != NULL);
    void* volatile* holder = (void* volatile*)(void*)GetProcAddress(game, "hellforge_fake_network_holder");
    uint8_t rows[2 * 0x18] = {0}, config[0x20] = {0};
    uint32_t hashes[2] = {(uint32_t)(Murmur64A("deposit_value", 13) >> 32), (uint32_t)(Murmur64A("other_value", 11) >> 32)};
    for (int i = 0; i < 2; i++) { memcpy(rows + i * 0x18, &hashes[i], 4); rows[i * 0x18 + 12] = 1; rows[i * 0x18 + 13] = i ? 8 : 10; }
    *(uint32_t*)(config + 0x10) = 2;
    *(uint8_t**)(config + 0x18) = rows;
    void* cell = config;
    void* holderBefore = *holder;
    *holder = &cell;
    char module[64];
    uint32_t rva = g_networkRva;
    memcpy(module, g_networkModule, sizeof module);
    snprintf(g_networkModule, sizeof g_networkModule, "fake_game.dll");
    g_networkRva = (uint32_t)((uintptr_t)holder - (uintptr_t)game);
    int asks = g_netAskCount, types = g_netTypeCount, mods = g_modCount;
    LONG fired[HF_EVENT_SESSION_END + 1];
    for (int event = 0; event <= HF_EVENT_SESSION_END; event++) { fired[event] = g_fired[event]; if (event >= 1 && event <= 4) InterlockedExchange(&g_fired[event], 1); }

    /* the plan's ask (mod a), and one that expects another type (left alone) */
    char l1[] = "nettype\ta\tdeposit_value\t1\t10\t0\t14", l2[] = "nettype\ta\tother_value\t1\t9\t0\t12", *f[8];
    assert(!NetParse(f, Split(l1, f, 8)) && !NetParse(f, Split(l2, f, 8)));
    NetApplyAll();
    assert(rows[13] == 14 && rows[0x18 + 13] == 8);

    g_liveGameState = TxnFixtureGameState;
    g_livePlayers = TxnFixturePlayers;
    g_txnInline = 1;
    AcquireSRWLockExclusive(&g_liveOutLock); g_liveConnected = 1; g_liveOutUsed = 0; ReleaseSRWLockExclusive(&g_liveOutLock);
    char reply[512], sent[2048];
    const char* add[] = {"modbegin\t1\tn1\tsolo\tx", "modorder\t2\tn1\t0\ta", "modorder\t3\tn1\t1\tb", "modorder\t4\tn1\t2\tnet.wide",
        "modadd\t5\tn1\tnet.wide\tnet\t0123456789abcdef\tC:\\mods\\wide\t-", "modnet\t6\tn1\tnet.wide\tdeposit_value\t1\t10\t0\t16", "modcommit\t7\tn1"};
    TxnRun(add, 7, reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    assert(strstr(sent, "modresult\tn1\tok\n") && rows[13] == 16);
    const char* replace[] = {"modbegin\t8\tn2\tsolo\tx", "modorder\t9\tn2\t0\ta", "modorder\t10\tn2\t1\tb", "modorder\t11\tn2\t2\tnet.wide",
        "modreplace\t12\tn2\tnet.wide\tnet\tfedcba9876543210\tC:\\mods\\wide\t-", "modnet\t13\tn2\tnet.wide\tdeposit_value\t1\t10\t0\t15", "modcommit\t14\tn2"};
    TxnRun(replace, 7, reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    assert(strstr(sent, "modresult\tn2\tok\n") && rows[13] == 15);
    /* taken out in a mission: refused, as an addition is (its asks change the network schema) */
    const char* inMission[] = {"modbegin\t23\tn5\tsolo\tx", "modorder\t24\tn5\t0\ta", "modorder\t25\tn5\t1\tb", "modremove\t26\tn5\tnet.wide", "modcommit\t27\tn5"};
    g_txnGameState = HF_STATE_MISSION;
    TxnRun(inMission, 5, reply, sizeof reply);
    g_txnGameState = HF_STATE_SHIP;
    assert(TxnResult("n5", "state") && rows[13] == 15);
    const char* remove[] = {"modbegin\t15\tn3\tsolo\tx", "modorder\t16\tn3\t0\ta", "modorder\t17\tn3\t1\tb", "modremove\t18\tn3\tnet.wide", "modcommit\t19\tn3"};
    TxnRun(remove, 5, reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    assert(strstr(sent, "modresult\tn3\tok\n") && rows[13] == 14); /* back to the plan's ask */
    TxnSend("modbegin\t20\tn4\tsolo\tx", reply, sizeof reply);
    TxnSend("modadd\t21\tn4\tnet.bad\tnet\t0123456789abcdef\tC:\\mods\\bad\t-", reply, sizeof reply);
    TxnSend("modnet\t22\tn4\tnet.bad\tdeposit_value\t1\t10\t0\t40", reply, sizeof reply); /* past 31 bits */
    assert(TxnRefused(reply, "malformed") && rows[13] == 14);
    /* a chain: an ask that expects the width another mod's ask gave (the API's "or already widened") */
    char l3[] = "nettype	b	deposit_value	1	14	0	18";
    assert(!NetParse(f, Split(l3, f, 8)));
    NetApply("deposit_value");
    assert(rows[13] == 18);
    printf("  network field types: the plan's ask at network ready, the widest wins, live wider, replaced and taken out, 3 refusals (one in a mission)\n");

    AcquireSRWLockExclusive(&g_liveOutLock); g_liveConnected = 0; g_liveOutUsed = 0; ReleaseSRWLockExclusive(&g_liveOutLock);
    g_liveGameState = LiveGameState;
    g_livePlayers = LivePlayers;
    g_txnInline = 0;
    for (int event = 0; event <= HF_EVENT_SESSION_END; event++) InterlockedExchange(&g_fired[event], fired[event]);
    g_netAskCount = asks;
    g_netTypeCount = types;
    g_modCount = mods;
    memcpy(g_networkModule, module, sizeof module);
    g_networkRva = rva;
    *holder = holderBefore;
}

/* Game data in a live change (live_data.c), against a fake engine: a data mod's hot package loaded on the game thread
 * and its overrides set once its targets are there; replaced (its old overrides go unless set again); taken out with a
 * restore the session owns (kept by later changes); refused when a target is missing or the package never loads. */
static uint64_t g_fakeLoaded[8], g_fakeOverrides[16][3], g_fakeCleared[16][2], g_fakePresent[8];
static int g_fakeLoadedCount, g_fakeOverrideCount, g_fakeClearedCount, g_fakePresentCount, g_fakeNeverLoads;
static void FakeHotLoad(uint64_t package) { g_fakeLoaded[g_fakeLoadedCount++] = package; }
static uint64_t FakeHotState(uint64_t package)
{
    for (int i = 0; i < g_fakeLoadedCount; i++) if (g_fakeLoaded[i] == package && !g_fakeNeverLoads) return PKG_RESIDENT;
    return PKG_WAITING;
}
static void FakeHotOverride(uint64_t type, uint64_t name, uint64_t target)
{
    g_fakeOverrides[g_fakeOverrideCount][0] = type; g_fakeOverrides[g_fakeOverrideCount][1] = name; g_fakeOverrides[g_fakeOverrideCount++][2] = target;
}
static void FakeHotClear(uint64_t type, uint64_t name) { g_fakeCleared[g_fakeClearedCount][0] = type; g_fakeCleared[g_fakeClearedCount++][1] = name; }
static uint8_t FakeHotCanGet(uint64_t type, uint64_t name)
{
    for (int i = 0; i < g_fakePresentCount; i++) if (g_fakePresent[i] == name) return 1;
    return 0;
}

static void HotDataFixture(void)
{
    int mods = g_modCount, overrides = g_hotOverrideCount;
    LONG fired[HF_EVENT_SESSION_END + 1];
    for (int event = 0; event <= HF_EVENT_SESSION_END; event++) { fired[event] = g_fired[event]; if (event >= 1 && event <= 4) InterlockedExchange(&g_fired[event], 1); }
    g_hotLoad = FakeHotLoad; g_hotOverride = FakeHotOverride; g_hotClear = FakeHotClear; g_hotCanGet = FakeHotCanGet;
    g_packageState = FakeHotState;
    g_hotTimeoutMs = 300;
    g_fakePresent[g_fakePresentCount++] = 0x1111111111111111ull; /* generation names in the packages */
    g_fakePresent[g_fakePresentCount++] = 0x2222222222222222ull;
    g_fakePresent[g_fakePresentCount++] = 0x3333333333333333ull;
    g_liveGameState = TxnFixtureGameState;
    g_livePlayers = TxnFixturePlayers;
    g_txnInline = 1;
    AcquireSRWLockExclusive(&g_liveOutLock); g_liveConnected = 1; g_liveOutUsed = 0; ReleaseSRWLockExclusive(&g_liveOutLock);
    char reply[512], sent[2048];
    const char* add[] = {"modbegin\t1\td1\tsync\tx", "modorder\t2\td1\t0\ta", "modorder\t3\td1\t1\tb", "modorder\t4\td1\t2\tdata.mod",
        "modadd\t5\td1\tdata.mod\tdata\t0123456789abcdef\tC:\\mods\\data\t-", "moddata\t6\td1\tdata.mod\t00000000000000a1",
        "modres\t7\td1\tdata.mod\tcccccccccccccccc\taaaaaaaaaaaaaaaa\t1111111111111111", "modcommit\t8\td1"};
    TxnRun(add, 8, reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    assert(strstr(sent, "modresult\td1\tok\n") && g_fakeLoadedCount == 1 && g_fakeLoaded[0] == 0xA1 && g_fakeOverrideCount == 1 &&
        g_fakeOverrides[0][1] == 0xAAAAAAAAAAAAAAAAull && g_fakeOverrides[0][2] == 0x1111111111111111ull);
    /* replaced: a new package, the same name set again (not cleared) */
    const char* replace[] = {"modbegin\t9\td2\tsync\tx", "modorder\t10\td2\t0\ta", "modorder\t11\td2\t1\tb", "modorder\t12\td2\t2\tdata.mod",
        "modreplace\t13\td2\tdata.mod\tdata\tfedcba9876543210\tC:\\mods\\data\t-", "moddata\t14\td2\tdata.mod\t00000000000000a2",
        "modres\t15\td2\tdata.mod\tcccccccccccccccc\taaaaaaaaaaaaaaaa\t2222222222222222", "modcommit\t16\td2"};
    TxnRun(replace, 8, reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    assert(strstr(sent, "modresult\td2\tok\n") && g_fakeClearedCount == 0 && g_fakeOverrides[1][2] == 0x2222222222222222ull);
    /* taken out, the game's own bytes back through an override the session owns */
    const char* remove[] = {"modbegin\t17\td3\tsync\tx", "modorder\t18\td3\t0\ta", "modorder\t19\td3\t1\tb", "modremove\t20\td3\tdata.mod",
        "moddata\t21\td3\t-\t00000000000000a3", "modres\t22\td3\t-\tcccccccccccccccc\taaaaaaaaaaaaaaaa\t3333333333333333", "modcommit\t23\td3"};
    TxnRun(remove, 7, reply, sizeof reply);
    TxnSent(sent, sizeof sent);
    assert(strstr(sent, "modresult\td3\tok\n") && g_fakeOverrides[2][2] == 0x3333333333333333ull && g_hotOverrideCount == overrides + 1 &&
        !strcmp(g_hotOverrides[overrides].owner, "-"));
    /* refused: a target not in the packages, and a package that never loads; nothing set */
    int set = g_fakeOverrideCount;
    const char* missing[] = {"modbegin\t24\td4\tsync\tx", "modorder\t25\td4\t0\ta", "modorder\t26\td4\t1\tb", "modorder\t27\td4\t2\tdata.mod",
        "modadd\t28\td4\tdata.mod\tdata\t0123456789abcdef\tC:\\mods\\data\t-", "moddata\t29\td4\tdata.mod\t00000000000000a4",
        "modres\t30\td4\tdata.mod\tcccccccccccccccc\tbbbbbbbbbbbbbbbb\t4444444444444444", "modcommit\t31\td4"};
    TxnRun(missing, 8, reply, sizeof reply);
    assert(TxnResult("d4", "packages") && g_fakeOverrideCount == set);
    g_fakeNeverLoads = 1;
    const char* never[] = {"modbegin\t32\td5\tsync\tx", "modorder\t33\td5\t0\ta", "modorder\t34\td5\t1\tb", "modorder\t35\td5\t2\tdata.mod",
        "modadd\t36\td5\tdata.mod\tdata\t0123456789abcdef\tC:\\mods\\data\t-", "moddata\t37\td5\tdata.mod\t00000000000000a5", "modcommit\t38\td5"};
    TxnRun(never, 7, reply, sizeof reply);
    assert(TxnResult("d5", "packages") && g_fakeOverrideCount == set);
    printf("  live data: a hot package loaded and its overrides set, replaced, taken out with the session's restore, 2 refusals\n");

    AcquireSRWLockExclusive(&g_liveOutLock); g_liveConnected = 0; g_liveOutUsed = 0; ReleaseSRWLockExclusive(&g_liveOutLock);
    g_liveGameState = LiveGameState;
    g_livePlayers = LivePlayers;
    g_txnInline = 0;
    g_hotLoad = NULL; g_hotOverride = NULL; g_hotClear = NULL; g_hotCanGet = NULL;
    g_packageState = NULL;
    g_hotTimeoutMs = HOT_TIMEOUT_MS;
    g_hotOverrideCount = overrides;
    for (int event = 0; event <= HF_EVENT_SESSION_END; event++) InterlockedExchange(&g_fired[event], fired[event]);
    g_modCount = mods;
}
