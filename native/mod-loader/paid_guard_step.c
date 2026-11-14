/* The paid-item guard, second half: the game's item table checked against the backend's list (paid_guard.c) on the game
 * thread, and a paid item the player may not hold taken back. The backend never serves one, so the game holding it
 * means something in the game added it. Included after ownership.c (OwnEntry's table, the remove it proved).
 *
 * game.dll 25480438 (KB paid-item-guard-static-re): an item is held at state 2 or 4 (record +0x14) or with an amount
 * (+0xB0); the catalogue append never sets either, only the inventory apply, add, set and the uncalled 0x136F790 do.
 * Remove (0x136F5E0) takes the amount and sets state 1, then recounts package rewards; it does nothing at amount 0, so a
 * record left at 2 or 4 is set to 1 as the game's own undo (0x136F850) sets it. The check starts once the inventory is
 * applied ([P + inventory status] == 2), before the login checks the equipped loadout (state 12, 0x1750870), which then
 * swaps a weapon or armour the player no longer holds for the default. */

#define PG_EVERY 8u             /* game updates between passes */
#define PG_REFRESH_MS 60000ull  /* a fresh list at least this often */
#define PG_NOTES 32             /* take-backs logged one by one at most */

static int* g_pgWatch;            /* table entries whose id the list refuses */
static int g_pgWatchCount, g_pgWatchCapacity;
static uint32_t g_pgWatchTableCount;
static LONG g_pgWatchAnswers;
static uint32_t g_pgWatchRecords; /* OWN_RECORDS when built: the catalogue can move (state_moves.c) */
static LONG g_pgSuspect = -1;     /* the answers seen when a held refused item was first found */
static uint32_t g_pgUpdates;
static ULONGLONG g_pgAskedAt;
static int g_pgOff, g_pgTaken;

static int PaidGuardHeld(uintptr_t manager, int entry)
{
    const uint8_t* record = OwnRecordAt(manager, entry);
    uint32_t state = *(const uint32_t*)(record + 0x14);
    return state == 2 || state == 4 || *(const int32_t*)(record + 0xB0) > 0;
}

/* The entries whose id the list refuses (under the list's lock). 0 when there was no memory. */
static int PaidGuardWatchBuild(uintptr_t manager)
{
    uint32_t count = *(const uint32_t*)(manager + OWN_COUNT);
    if (count > OWN_MAX) count = OWN_MAX;
    g_pgWatchCount = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        if (!PaidGuardRefusedHas(((const uint32_t*)(manager + OWN_INDEX + (uintptr_t)i * 24))[1])) continue;
        if (g_pgWatchCount == g_pgWatchCapacity)
        {
            int capacity = g_pgWatchCapacity ? g_pgWatchCapacity * 2 : 256;
            int* grown = (int*)realloc(g_pgWatch, sizeof *grown * (size_t)capacity);
            if (grown == NULL) return 0;
            g_pgWatch = grown;
            g_pgWatchCapacity = capacity;
        }
        g_pgWatch[g_pgWatchCount++] = (int)i;
    }
    return 1;
}

/* One entry taken back: the game's remove for an amount, then state 1 where it stays held (a hooked remove too). */
static void PaidGuardTake(uintptr_t manager, int entry)
{
    uint32_t mixId = ((const uint32_t*)(manager + OWN_INDEX + (uintptr_t)entry * 24))[1];
    uint8_t* record = (uint8_t*)OwnRecordAt(manager, entry);
    int32_t amount = *(const int32_t*)(record + 0xB0);
    uint32_t state = *(const uint32_t*)(record + 0x14);
    if (amount > 0) g_ownRemove(manager, mixId, (uint32_t)amount);
    if (*(int32_t*)(record + 0xB0) > 0) *(int32_t*)(record + 0xB0) = 0;
    if (*(uint32_t*)(record + 0x14) == 2 || *(uint32_t*)(record + 0x14) == 4) *(uint32_t*)(record + 0x14) = 1;
    if (g_pgTaken++ < PG_NOTES)
        Note("paid guard: took back item 0x%08x (amount %d, state %u): the backend does not give it to this player, so a mod added it in the game",
            mixId, amount, state);
}

/* One pass over the table (manager: the progression manager, its catalogue and inventory loaded). How many entries it
 * took back. A held refused item is taken only after a list asked for since it was found says so: a token just bought
 * with Forge Credits is in the table before an older list knows it is allowed. */
static int PaidGuardPass(uintptr_t manager)
{
    LONG answers = g_paidGuardAnswers;
    if (answers == 0) return 0;
    AcquireSRWLockShared(&g_paidGuardLock);
    uint32_t count = *(const uint32_t*)(manager + OWN_COUNT);
    int built = 1;
    if (count != g_pgWatchTableCount || answers != g_pgWatchAnswers || g_pgWatchRecords != OWN_RECORDS)
    {
        built = PaidGuardWatchBuild(manager);
        g_pgWatchTableCount = count;
        g_pgWatchAnswers = answers;
        g_pgWatchRecords = OWN_RECORDS;
    }
    ReleaseSRWLockShared(&g_paidGuardLock);
    if (!built) { g_pgWatchTableCount = 0; return 0; }

    int held = 0;
    for (int i = 0; i < g_pgWatchCount && !held; i++) held = PaidGuardHeld(manager, g_pgWatch[i]);
    if (!held) { g_pgSuspect = -1; return 0; }
    if (g_pgSuspect < 0) { g_pgSuspect = answers; PaidGuardAsk(); return 0; }
    if (answers <= g_pgSuspect) return 0; /* the fresh list is on its way */
    int taken = 0;
    for (int i = 0; i < g_pgWatchCount; i++)
        if (PaidGuardHeld(manager, g_pgWatch[i])) { PaidGuardTake(manager, g_pgWatch[i]); taken++; }
    g_pgSuspect = -1;
    return taken;
}

/* The game thread's update (events.c EventsTick). */
static void PaidGuardTick(void)
{
    if (g_paidGuardPort == 0 || g_pgOff || ++g_pgUpdates % PG_EVERY) return;
    if (g_ownApply == NULL && !OwnProve())
    {
        g_pgOff = 1;
        Note("paid guard: the game's item functions are not the reviewed ones; off this session");
        return;
    }
    uintptr_t manager = g_ownManager != NULL ? g_ownManager() : 0;
    if (manager == 0 || *(const uint32_t*)(manager + OWN_CATALOGUE) != 2 || *(const uint32_t*)(manager + OWN_INVENTORY) != 2) return;
    ULONGLONG now = GetTickCount64();
    if (g_pgAskedAt == 0 || now - g_pgAskedAt >= PG_REFRESH_MS)
    {
        g_pgAskedAt = now;
        if (!PaidGuardAsk()) return;
    }
    PaidGuardPass(manager);
}
