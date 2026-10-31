/* Armory thumbnail diagnostics (armory_diag.c): a stalled card is reported once with the requests it waits on, a change
 * after it once, finished or moving cards never, and nothing off the Armory and briefing screens. */
static void ArmoryPut32(uint8_t* m, size_t at, uint32_t v) { memcpy(m + at, &v, 4); }
static void ArmoryPut64(uint8_t* m, size_t at, uint64_t v) { memcpy(m + at, &v, 8); }

static void ArmoryCard(uint8_t* m, uint32_t c, uint32_t state, uint32_t count, uint32_t kind, uint32_t stage)
{
    ArmoryPut32(m, c * ARMORY_CARD_STRIDE + ARMORY_CARD_STATE, state);
    ArmoryPut32(m, c * ARMORY_CARD_STRIDE + ARMORY_CARD_COUNT, count);
    for (uint32_t i = 0; i < count; i++)
    {
        size_t at = c * ARMORY_CARD_STRIDE + ARMORY_REQUESTS + i * ARMORY_REQUEST_STRIDE;
        ArmoryPut64(m, at + 24, 0x1000u + c * 16 + i);
        ArmoryPut32(m, at + 100, kind);
        ArmoryPut32(m, at + 104, stage);
    }
}

static void ArmoryDiagFixture(void)
{
    static uint8_t m[ARMORY_BYTES];
    ArmoryDiag d;
    char line[1024];
    memset(&d, 0, sizeof d);
    memset(m, 0, sizeof m);
    ArmoryPut32(m, ARMORY_ACTIVE, 1);
    ArmoryPut32(m, ARMORY_PHASE, 5);
    ArmoryCard(m, 0, 8, 15, 2, 4);              /* the first page: done */
    ArmoryCard(m, 1, 5, 15, 2, 2);              /* scrolled to: every request submitted, none done */
    ArmoryPut32(m, 1 * ARMORY_CARD_STRIDE + ARMORY_REQUESTS + 3 * ARMORY_REQUEST_STRIDE + 104, 4);

    /* Off the thumbnail screens: nothing. On one with no manager: one line a session. */
    assert(!ArmoryDiagStep(&d, 3, m, 1000, line, sizeof line));
    assert(ArmoryDiagStep(&d, 5, NULL, 1000, line, sizeof line) && strstr(line, "the Armory is open but its thumbnail manager could not be read"));
    assert(!ArmoryDiagStep(&d, 5, NULL, 1500, line, sizeof line));
    /* The first screen watched: one line with its cards; standing still below the threshold: nothing; at it: one line
     * naming the waiting requests (not the done one). */
    assert(ArmoryDiagStep(&d, 5, m, 2000, line, sizeof line) && strstr(line, "watching the Armory (cards state:count 8:15 5:15 0:0 0:0 0:0 0:0)"));
    assert(!ArmoryDiagStep(&d, 5, m, 2500, line, sizeof line));
    assert(!ArmoryDiagStep(&d, 5, m, 2000 + ARMORY_STALL_MS - 1, line, sizeof line));
    assert(ArmoryDiagStep(&d, 5, m, 2000 + ARMORY_STALL_MS, line, sizeof line));
    assert(strstr(line, "no progress for 10 s on the Armory") && strstr(line, "active card 1, phase 5"));
    assert(strstr(line, "cards (state:count) 8:15 5:15 0:0 0:0 0:0 0:0"));
    assert(strstr(line, "helmet 0x1010 (card 1, stage 2)") && !strstr(line, "0x1013") && !strstr(line, "0x1000 "));
    assert(strstr(line, ", and 2 more") && !strstr(line, "toaster"));
    assert(!ArmoryDiagStep(&d, 5, m, 2000 + 3 * ARMORY_STALL_MS, line, sizeof line)); /* once a stall */
    /* The generator moves: one line with how long it stood, then quiet while it moves. */
    ArmoryPut32(m, 1 * ARMORY_CARD_STRIDE + ARMORY_REQUESTS + 104, 3);
    assert(ArmoryDiagStep(&d, 5, m, 40000, line, sizeof line) && strstr(line, "generating again on the Armory after 38 s"));
    for (ULONGLONG t = 41000; t < 41000 + 3 * ARMORY_STALL_MS; t += 1000)
    {
        ArmoryPut32(m, ARMORY_PHASE, (uint32_t)(t / 1000) % 2 ? 4 : 5);
        assert(!ArmoryDiagStep(&d, 5, m, t, line, sizeof line));
    }
    /* Every card done: never a stall, however long it stands. */
    ArmoryCard(m, 1, 8, 15, 3, 4);
    ArmoryPut32(m, ARMORY_ACTIVE, 0xFFFFFFFFu);
    assert(!ArmoryDiagStep(&d, 5, m, 100000, line, sizeof line));
    assert(!ArmoryDiagStep(&d, 5, m, 100000 + 5 * ARMORY_STALL_MS, line, sizeof line));
    /* A stall on the briefing, left before it moves (with --toaster_mode on): the stall and the leaving. */
    ArmoryCard(m, 2, 3, 2, 4, 1);
    d.toaster = 1;
    assert(!ArmoryDiagStep(&d, 14, m, 200000, line, sizeof line));
    assert(ArmoryDiagStep(&d, 14, m, 200000 + ARMORY_STALL_MS, line, sizeof line));
    assert(strstr(line, "briefing loadout") && strstr(line, "--toaster_mode is on") && strstr(line, "active card none"));
    assert(strstr(line, "cape 0x1020 (card 2, stage 1), cape 0x1021 (card 2, stage 1)") && !strstr(line, "more"));
    assert(ArmoryDiagStep(&d, 0, m, 230000, line, sizeof line) && strstr(line, "left the briefing loadout after 30 s"));
    assert(!ArmoryDiagStep(&d, 0, m, 231000, line, sizeof line));
    /* Bounds not as reviewed: read as no manager. */
    ArmoryPut32(m, 3 * ARMORY_CARD_STRIDE + ARMORY_CARD_STATE, 9);
    assert(!ArmoryDiagStep(&d, 5, m, 300000, line, sizeof line) && !ArmoryDiagStep(&d, 5, m, 400000, line, sizeof line));
    ArmoryPut32(m, 3 * ARMORY_CARD_STRIDE + ARMORY_CARD_STATE, 0);
    /* At most ARMORY_REPORTS stalls a session. */
    for (ULONGLONG t = 500000; d.reports < ARMORY_REPORTS; t += 2 * ARMORY_STALL_MS)
    {
        assert(!ArmoryDiagStep(&d, 5, m, t, line, sizeof line));
        assert(ArmoryDiagStep(&d, 5, m, t + ARMORY_STALL_MS, line, sizeof line));
        assert(ArmoryDiagStep(&d, 0, m, t + ARMORY_STALL_MS + 1, line, sizeof line));
    }
    assert(!ArmoryDiagStep(&d, 5, m, 900000, line, sizeof line) && !ArmoryDiagStep(&d, 5, m, 900000 + ARMORY_STALL_MS, line, sizeof line));
    puts("PASS armory diagnostics: a stalled card reported once with its waiting requests and toaster mode, recovery and leaving once, done or moving cards and other screens never, bounds checked, capped a session");
}
