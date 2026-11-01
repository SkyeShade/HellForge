/* Per-player round stats in co-op (docs/multiplayer.md "Shots in co-op"). A shot copies its stats when it spawns, from
 * the spawning machine's own rows: SpawnProjectile (game.dll 0x13A9830) reads the ProjectileSettings row through its
 * descriptor ([rdx+0x10]: flight, the damage id +0x3C, explosions) and copies the damage row's four armour-penetration
 * lanes into the shot's hit record (+0x18..+0x1E); the damage itself is read from the DamageInfo row per hit, times the
 * shot's damage multiplier (hit +0x34). So a friend's edited round flew and penetrated on this machine as this machine's
 * rows had it (their AP 4 deflecting at AP 2 here), and this machine's own edits showed on the others' guns. Research:
 * HD2Mods\research\coop-weapon-stats (part B). Included by hellforge_modloader.c after shots.c (its players and wielders).
 *
 * The lobby's players' edits of those two tables come from the session's backend (live.c "rounds", AssetShare). For
 * every other player present, each row anyone in the lobby edited is rebuilt here as that player has it: this machine's
 * row with this machine's own changes undone (the Local Stats mod's and the editors': their originals), then that
 * player's values. A projectile row that comes out unlike this machine's is that player's shadow row, a copy keeping the
 * stock type at +0 (every later lookup of the flying shot goes by that type); a damage row that comes out unlike it gives
 * that player's lanes and a damage scale (their standard damage over this machine's). One call is changed: the pellet
 * spawn in the ProjectileWeapon fire path (0x616612, call SpawnProjectile), which makes this machine's copies of every
 * player's hand-held shots. For a shot whose shooter (its weapon's wielder, as shots.c finds it) is another player, the
 * descriptor carries that player's shadow row for the call, and after it the shot takes their lanes and damage scale.
 * This machine's own player's shots are left alone. Not covered: the pellet count (read by type in the fire function,
 * 0x61444B), explosion values, damage statuses and demolition (read per hit by id), shots spawned elsewhere. */

#define ROUND_CALL 0x616603u      /* mov rcx,[pool]; lea r8,[rbp+0x20]; lea rdx,[rbp-0x20]; call SpawnProjectile */
#define ROUND_CALL_AT 15u         /* the call's place in the pin */
#define ROUND_SPAWN 0x13A9830u
#define ROUND_POOL_ACTIVE 0x28u
#define ROUND_HITS 0x4D040u       /* the pool's hit records, ROUND_HIT bytes a slot */
#define ROUND_HIT 0xC8u
#define ROUND_SLOTS 0x7FFu
#define ROUND_ENTRIES 512
#define ROUND_ROWS 32             /* distinct rows of each table the lobby's edits touch */
#define ROUND_SHADOWS 96
#define ROUND_VIEWS 96
static const uint8_t kRoundCallPin[20] = {0x48, 0x8B, 0x0D, 0x9E, 0x68, 0xE6, 0x02, 0x4C, 0x8D, 0x45, 0x20, 0x48, 0x8D, 0x55, 0xE0,
    0xE8, 0x19, 0x32, 0xD9, 0x00};

typedef struct RoundEntry { uint64_t peer; int damage; uint32_t id, offset, bits; } RoundEntry;
typedef struct RoundShadow { uint64_t peer; uint32_t type; uint8_t row[PRJ_ROW]; } RoundShadow;
typedef struct RoundView { uint64_t peer; uint32_t id; uint16_t lanes[4]; float scale; } RoundView;
static RoundEntry g_roundEntries[ROUND_ENTRIES];
static int g_roundEntryCount;
static RoundShadow g_roundShadows[ROUND_SHADOWS];
static int g_roundShadowCount;
static RoundView g_roundViews[ROUND_VIEWS];
static int g_roundViewCount;
static volatile LONG g_roundsActive; /* shadow rows or views to use */
static SRWLOCK g_roundsLock = SRWLOCK_INIT;
static int g_roundState;             /* 0 not installed yet, 1 installed, -1 refused */
static ULONGLONG g_roundFirstTry;
static uintptr_t g_roundSpawn;       /* SpawnProjectile, as the hook calls it */
static volatile LONG g_roundShots;   /* shots that took another player's values, for the log */
typedef uint32_t (*RoundSpawnFn)(uint8_t* pool, uint8_t* descriptor, uint8_t* extra);

/* live.c "rounds": the lobby's players' edits of projectile and damage rows, "<peer hex>=<entry>;<entry>..." comma
 * separated, an entry "P<type>+<offset hex>=<bits hex>" (ProjectileSettings) or "D<id>+<offset hex>=<bits hex>"
 * (DamageInfo), four bytes each and never a row's +0 (its own id); "-" none (solo, or a host sharing its stats).
 * Replaces the last. 0 with *why when malformed. */
/* The entry at *at, which moves past it: 0 with *why when it is malformed or names no field of its row. */
static int RoundEntryAt(const char** at, uint64_t peer, RoundEntry* out, const char** why)
{
    char* end;
    const char* p = *at;
    if (*p != 'P' && *p != 'D') { *why = "malformed entry"; return 0; }
    int damage = *p == 'D';
    unsigned long id = strtoul(p + 1, &end, 10);
    if (end == p + 1 || *end != '+') { *why = "malformed entry"; return 0; }
    const char* o = end + 1;
    unsigned long offset = strtoul(o, &end, 16);
    if (end == o || *end != '=') { *why = "malformed entry"; return 0; }
    const char* b = end + 1;
    unsigned long long bits = strtoull(b, &end, 16);
    if (end == b || bits > 0xFFFFFFFFull) { *why = "malformed entry"; return 0; }
    uint32_t size = damage ? DMG_ROW : PRJ_ROW, rows = damage ? DMG_VANILLA + DMG_MAX_NEW : PRJ_VANILLA + PRJ_MAX_NEW;
    if (id == 0 || id >= rows || offset < 4 || offset % 4 || offset + 4 > size) { *why = "an entry names no field of its row"; return 0; }
    *out = (RoundEntry){peer, damage, (uint32_t)id, (uint32_t)offset, (uint32_t)bits};
    *at = end;
    return 1;
}

/* One player's entries at *at, "<peer hex>=<entry>;<entry>...", into entries from *count; *at moves past them. 0 with
 * *why when malformed, or past ROUND_ENTRIES. */
static int RoundGroupAt(const char** at, RoundEntry* entries, int* count, const char** why)
{
    char* end;
    uint64_t peer = strtoull(*at, &end, 16);
    if (end == *at || *end != '=' || peer == 0) { *why = "malformed list"; return 0; }
    *at = end; /* each entry follows an '=' or a ';' */
    do
    {
        (*at)++;
        if (*count == ROUND_ENTRIES) { *why = "more changes than HellForge keeps"; return 0; }
        if (!RoundEntryAt(at, peer, &entries[(*count)++], why)) return 0;
    } while (**at == ';');
    return 1;
}

static int RoundsSetLobby(const char* list, const char** why)
{
    static RoundEntry entries[ROUND_ENTRIES]; /* the pipe's thread only */
    int count = 0;
    for (const char* at = list; strcmp(list, "-") && *at;)
    {
        if (!RoundGroupAt(&at, entries, &count, why)) return 0;
        if (*at != ',' && *at != 0) { *why = "malformed list"; return 0; }
        if (*at == ',') at++;
    }
    AcquireSRWLockExclusive(&g_roundsLock);
    memcpy(g_roundEntries, entries, sizeof entries[0] * (size_t)count);
    g_roundEntryCount = count;
    if (count == 0) { g_roundShadowCount = g_roundViewCount = 0; InterlockedExchange(&g_roundsActive, 0); }
    ReleaseSRWLockExclusive(&g_roundsLock);
    Note("rounds: %d projectile and damage change(s) of the lobby's players%s", count,
        count > 0 && g_roundState != 1 ? " (not applied: the projectile spawn is not hooked)" : "");
    return 1;
}

/* A copy of a row as it was before this machine's own changes: the editors' live edits' originals (the first of each
 * field's, from before every live edit) and the Local Stats mod's boot edits' (from before it), written over it. */
static void RoundBase(uintptr_t row, uint8_t* copy, uint32_t size)
{
    uint8_t done[PRJ_ROW] = {0};
    AcquireSRWLockShared(&g_fieldsLock);
    for (int i = 0; i < g_editCount; i++)
    {
        const FieldEdit* e = &g_edits[i];
        uint32_t bytes = e->field != NULL ? e->field->size : 0;
        if (!e->live || !e->hasOriginal || e->failed || ModRank(e->mod) != RANK_EDITOR || bytes == 0 || bytes > sizeof e->original ||
            e->address < row || e->address + bytes > row + size || done[e->address - row]) continue;
        memcpy(copy + (e->address - row), e->original, bytes);
        done[e->address - row] = 1;
    }
    ReleaseSRWLockShared(&g_fieldsLock);
    AcquireSRWLockShared(&g_statsLock);
    for (int i = 0; i < g_statCount; i++)
    {
        const StatEdit* s = &g_stats[i];
        uint32_t bytes = StatSize(s);
        if (s->applied && IsLocalStats(s->mod) && s->address >= row && s->address + bytes <= row + size)
            memcpy(copy + (s->address - row), &s->original, bytes);
    }
    ReleaseSRWLockShared(&g_statsLock);
}

/* The rows of each table anyone in the lobby changed ([0] projectiles, [1] damage). */
static void RoundsTouched(const RoundEntry* entries, int count, uint32_t rows[2][ROUND_ROWS], int rowCount[2])
{
    rowCount[0] = rowCount[1] = 0;
    for (int i = 0; i < count; i++)
    {
        int t = entries[i].damage, seen = 0;
        for (int j = 0; j < rowCount[t]; j++) seen |= rows[t][j] == entries[i].id;
        if (!seen && rowCount[t] < ROUND_ROWS) rows[t][rowCount[t]++] = entries[i].id;
    }
}

/* A row as a player has it: this machine's (into now) without this machine's own changes, then the player's. 0 when
 * there is no such row, or it comes out as this machine has it (nothing to change). */
static int RoundTheirs(int t, uint32_t id, uint64_t peer, const RoundEntry* entries, int count, uint8_t* now, uint8_t* theirs)
{
    int ready = 1;
    uint32_t size = t ? DMG_ROW : PRJ_ROW;
    uintptr_t row = g_tableRow(t ? "DamageInfo" : "ProjectileSettings", id, &ready);
    if (row == 0 || SafeRead(row, now, size)) return 0;
    memcpy(theirs, now, size);
    RoundBase(row, theirs, size);
    for (int i = 0; i < count; i++)
        if (entries[i].peer == peer && entries[i].damage == t && entries[i].id == id) memcpy(theirs + entries[i].offset, &entries[i].bits, 4);
    return memcmp(theirs, now, size) != 0;
}

/* A player's damage row on their shots: its lanes, and one damage scale for both kinds of damage (by standard damage,
 * else, when only durable damage changed, by durable). */
static void RoundViewOf(const uint8_t* now, const uint8_t* theirs, RoundView* v)
{
    for (int k = 0; k < 4; k++) { uint32_t lane; memcpy(&lane, theirs + 0x0C + 4 * k, 4); v->lanes[k] = (uint16_t)lane; }
    uint32_t mine, standard, mineDurable, durable;
    memcpy(&mine, now + 4, 4); memcpy(&standard, theirs + 4, 4);
    memcpy(&mineDurable, now + 8, 4); memcpy(&durable, theirs + 8, 4);
    v->scale = standard != mine && mine != 0 ? (float)standard / (float)mine :
        durable != mineDurable && mineDurable != 0 ? (float)durable / (float)mineDurable : 1.0f;
}

/* The lobby's entries as RoundsRebuild copied them, and the rows of each table they touch (RoundsTouched). */
typedef struct RoundsLobby
{
    const RoundEntry* entries;
    int entryCount;
    uint32_t rows[2][ROUND_ROWS];
    int rowCount[2];
} RoundsLobby;

/* A player's projectile rows that differ from this machine's, into shadows from *count (at most ROUND_SHADOWS). */
static void RoundShadowsOf(const RoundsLobby* lobby, uint64_t peer, RoundShadow* shadows, int* count)
{
    uint8_t now[PRJ_ROW], theirs[PRJ_ROW];
    for (int r = 0; r < lobby->rowCount[0] && *count < ROUND_SHADOWS; r++)
        if (RoundTheirs(0, lobby->rows[0][r], peer, lobby->entries, lobby->entryCount, now, theirs))
        {
            shadows[*count].peer = peer;
            shadows[*count].type = lobby->rows[0][r];
            memcpy(shadows[(*count)++].row, theirs, PRJ_ROW);
        }
}

/* A player's damage rows that differ from this machine's, as views into views from *count (at most ROUND_VIEWS). */
static void RoundViewsOf(const RoundsLobby* lobby, uint64_t peer, RoundView* views, int* count)
{
    uint8_t now[PRJ_ROW], theirs[PRJ_ROW];
    for (int r = 0; r < lobby->rowCount[1] && *count < ROUND_VIEWS; r++)
        if (RoundTheirs(1, lobby->rows[1][r], peer, lobby->entries, lobby->entryCount, now, theirs))
        {
            views[*count] = (RoundView){peer, lobby->rows[1][r], {0}, 1.0f};
            RoundViewOf(now, theirs, &views[(*count)++]);
        }
}

/* The watch loop, every second while the lobby has changes: every other player's shadow rows and damage views, from
 * the rows as they are now (this machine's edits can change under them). */
static void RoundsRebuild(void)
{
    static RoundEntry entries[ROUND_ENTRIES];
    static RoundShadow shadows[ROUND_SHADOWS];
    static RoundView views[ROUND_VIEWS];
    ShotPlayer players[4];
    int playerCount;
    AcquireSRWLockShared(&g_shotsLock);
    playerCount = g_shotPlayerCount;
    memcpy(players, g_shotPlayers, sizeof players[0] * (size_t)playerCount);
    ReleaseSRWLockShared(&g_shotsLock);
    AcquireSRWLockShared(&g_roundsLock);
    int entryCount = g_roundEntryCount;
    memcpy(entries, g_roundEntries, sizeof entries[0] * (size_t)entryCount);
    ReleaseSRWLockShared(&g_roundsLock);
    RoundsLobby lobby = {entries, entryCount};
    RoundsTouched(entries, entryCount, lobby.rows, lobby.rowCount);
    int shadowCount = 0, viewCount = 0;
    for (int p = 0; p < playerCount; p++)
    {
        if (players[p].local || players[p].peer == 0) continue;
        RoundShadowsOf(&lobby, players[p].peer, shadows, &shadowCount);
        RoundViewsOf(&lobby, players[p].peer, views, &viewCount);
    }
    AcquireSRWLockExclusive(&g_roundsLock);
    int changed = shadowCount != g_roundShadowCount || viewCount != g_roundViewCount ||
        memcmp(shadows, g_roundShadows, sizeof shadows[0] * (size_t)shadowCount) || memcmp(views, g_roundViews, sizeof views[0] * (size_t)viewCount);
    if (entryCount == g_roundEntryCount) /* not replaced meanwhile */
    {
        memcpy(g_roundShadows, shadows, sizeof shadows[0] * (size_t)shadowCount);
        memcpy(g_roundViews, views, sizeof views[0] * (size_t)viewCount);
        g_roundShadowCount = shadowCount;
        g_roundViewCount = viewCount;
        InterlockedExchange(&g_roundsActive, shadowCount + viewCount > 0);
    }
    ReleaseSRWLockExclusive(&g_roundsLock);
    if (changed) Note("rounds: %d projectile row(s) and %d damage row(s) of other players differ from this machine's", shadowCount, viewCount);
}

/* Another player who wields the weapon (its peer), or 0: this machine's player, or no player. */
static uint64_t RoundShooter(uint32_t weapon)
{
    uint32_t wielder = weapon ? g_shotWielder(weapon) : 0;
    uint64_t peer = 0;
    AcquireSRWLockShared(&g_shotsLock);
    for (int i = 0; wielder != 0 && i < g_shotPlayerCount; i++)
        if (g_shotPlayers[i].avatar == wielder && !g_shotPlayers[i].local) peer = g_shotPlayers[i].peer;
    ReleaseSRWLockShared(&g_shotsLock);
    return peer;
}

/* The shooter's damage row on the shot in a slot: its lanes and damage scale. 1 when it took them. */
static int RoundHit(uint8_t* pool, uint32_t slot, uint64_t peer)
{
    uint8_t* hit = pool + ROUND_HITS + (size_t)(slot & ROUND_SLOTS) * ROUND_HIT;
    uint32_t damage;
    memcpy(&damage, hit + 0x0C, 4);
    int took = 0;
    AcquireSRWLockShared(&g_roundsLock);
    for (int i = 0; i < g_roundViewCount && !took; i++)
    {
        if (g_roundViews[i].peer != peer || g_roundViews[i].id != damage) continue;
        memcpy(hit + 0x18, g_roundViews[i].lanes, 8);
        float scale;
        memcpy(&scale, hit + 0x34, 4);
        scale *= g_roundViews[i].scale;
        memcpy(hit + 0x34, &scale, 4);
        took = 1;
    }
    ReleaseSRWLockShared(&g_roundsLock);
    return took;
}

/* The pellet spawn's call (from the stub at 0x616612, on the thread that fires): SpawnProjectile with the shooter's own
 * projectile row and, after it, their damage row's lanes and scale on the shot. */
static uint32_t RoundSpawn(uint8_t* pool, uint8_t* descriptor, uint8_t* extra)
{
    RoundSpawnFn spawn = (RoundSpawnFn)g_roundSpawn;
    if (!g_roundsActive || descriptor == NULL || pool == NULL) return spawn(pool, descriptor, extra);
    uint8_t* row;
    uint32_t type, weapon;
    memcpy(&row, descriptor + 0x10, 8);
    memcpy(&weapon, descriptor + 0x18, 4);
    uint64_t peer = row != NULL ? RoundShooter(weapon) : 0;
    if (peer == 0) return spawn(pool, descriptor, extra);
    memcpy(&type, row, 4);
    /* the shooter's row, copied: no lock is held while the game spawns */
    uint8_t shadow[PRJ_ROW];
    int swapped = 0;
    AcquireSRWLockShared(&g_roundsLock);
    for (int i = 0; i < g_roundShadowCount && !swapped; i++)
        if (g_roundShadows[i].peer == peer && g_roundShadows[i].type == type) { memcpy(shadow, g_roundShadows[i].row, PRJ_ROW); swapped = 1; }
    ReleaseSRWLockShared(&g_roundsLock);
    uint8_t* mine = shadow;
    if (swapped) memcpy(descriptor + 0x10, &mine, 8);
    uint32_t slot = spawn(pool, descriptor, extra);
    if (swapped) memcpy(descriptor + 0x10, &row, 8);
    int hit = pool[ROUND_POOL_ACTIVE] && RoundHit(pool, slot, peer);
    if (swapped || hit) InterlockedIncrement(&g_roundShots);
    return slot;
}

/* The watch loop, after the game loads: the shadow rows again each second while the lobby has changes, and the hook
 * once its bytes read as reviewed (60 s for code still being decoded). */
static void RoundsTick(void)
{
    static ULONGLONG next;
    static LONG shotsNoted;
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 1000;
    if (g_roundEntryCount > 0) RoundsRebuild();
    LONG shots = g_roundShots;
    if (shots != shotsNoted && (shotsNoted == 0 || shots - shotsNoted >= 1000))
    {
        Note("rounds: %ld shot(s) of other players flew with their own values here", (long)shots);
        shotsNoted = shots;
    }
    if (g_roundState != 0) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0) return;
    if (!g_roundFirstTry) g_roundFirstTry = now;
    if (strcmp(g_build, "25480438")) { g_roundState = -1; Note("rounds: not hooked (unsupported build)"); return; }
    if (!PinMatches(game + ROUND_CALL, kRoundCallPin, sizeof kRoundCallPin))
    {
        if (now - g_roundFirstTry < 60000) return;
        g_roundState = -1;
        Note("rounds: the projectile spawn is not the reviewed one; friends' edited rounds fly as this machine's");
        return;
    }
    /* the call goes to a jump near game.dll (a rel32 reaches it), which goes on to RoundSpawn */
    uint8_t* stub = AllocateNearGame(game, 0x1000);
    if (stub == NULL) { g_roundState = -1; Note("rounds: no room near game.dll for the stub"); return; }
    static const uint8_t jump[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}; /* jmp [rip] */
    uintptr_t fn = (uintptr_t)RoundSpawn;
    memcpy(stub, jump, sizeof jump);
    memcpy(stub + sizeof jump, &fn, 8);
    DWORD protect;
    if (!VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &protect)) { g_roundState = -1; Note("rounds: the stub could not be made executable"); return; }
    g_roundSpawn = game + ROUND_SPAWN;
    uint8_t call[5] = {0xE8};
    int32_t rel = (int32_t)((intptr_t)stub - (intptr_t)(game + ROUND_CALL + ROUND_CALL_AT + 5));
    memcpy(call + 1, &rel, 4);
    if (ApiWrite(game + ROUND_CALL + ROUND_CALL_AT, call, sizeof call)) { g_roundState = -1; Note("rounds: the projectile spawn could not be changed"); return; }
    g_roundState = 1;
    Note("rounds: hooked the projectile spawn: in co-op each player's edited rounds fly as theirs on every machine");
}
