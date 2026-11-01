/* Per-player projectile swaps in co-op (docs/multiplayer.md "Shots in co-op"). Every machine simulates every player's
 * shots from its own data, so a swap one player made in the editor (a weapon's ProjectileWeapon +0, its WeaponRounds +64,
 * its ammunition's entity delta or its programmable round, ProjectileWeapon +0x240) showed as the stock round on the
 * others' screens, and the swapping player's own swap showed on the others' guns. A shot's type is chosen in the
 * ProjectileWeapon fire function (game.dll 0x6128B0) for the local player's shots and for this machine's copies of the
 * others' alike: after the native choice ([rbp-0x68], at 0x614081, past both of its branches), before the pellet loop
 * and the pool's constructor, with the shot's source at [rbp-0x80] (+0 the weapon's resource, +8 its entity). Each pellet
 * (0x615940) then takes the weapon's programmable round instead while its function is on (0x615B23: [r13+0x240], r13 the
 * weapon's row, the source at [rsp+0x48]). Research: HD2Mods\research\projectile-per-player (Astra, from a decoded
 * snapshot of this build; every pin is checked here before anything is written).
 *
 * A stub at 0x614081 calls ShotType with the chosen type and the source, writes back what it returns, and replays the
 * two instructions it displaced; one at 0x615B23 calls ShotFunctionType with the programmable round and replays the
 * three it displaced with what that returns. Every type reaches 0x614081: the resolver's (0x7456D0: ProjectileWeapon
 * +0, WeaponRounds +0x40/+0x44, a magazine's chambered or pattern type) and the explicit one a charge or heat level
 * passes. A rule swaps one round of a weapon, recognised by its stock type on this machine: the weapon's ProjectileWeapon
 * +0 (no suffix), WeaponRounds +64 ("/r"), its programmable round for ShotFunctionType ("/f": ProjectileWeapon +0x240), or
 * a stock type the rule names ("/<stock type>": an ammunition's entity delta, a magazine pattern entry, a charge or heat
 * level; this machine's own swap of one is its own rule). A type this machine's own swap made stands for the stock round
 * it replaced; a round no rule names (another mode's, another ammunition's) stays. It finds the shooter (the weapon's
 * wielder, Wieldable manager [game+0x3326730]) among the players (event_polls.c EvpPlayers: peer and Helldiver), then:
 *   - a player with a swap for that weapon (the lobby's rules, from the session's backend: live.c "shots"): theirs;
 *   - this machine's player: what the game chose (its own record, swapped or not);
 *   - another player without one: the weapon's type from before this machine's own swap.
 * Shots by no player (turrets, enemies), and any shot while no rules are set (solo, or a host sharing its stats), are
 * left alone. A rule is used once its projectile's row exists and its packages are resident (refs.c RefCheck). Beams
 * choose no row. A weapon firing an entity (a missile) spawns it only where the machine has authority over the weapon
 * (0x615BE2, entity info +0x14 bit 0: its wielder's), which then reaches the others: from the shooter's own data. */
#define SHOT_FIRE 0x6128B0u
#define SHOT_SITE 0x614081u
#define SHOT_SITE_BACK 0x61408Bu      /* past the two displaced instructions */
#define SHOT_PELLET 0x615940u
#define SHOT_FUNCTION_SITE 0x615B23u
#define SHOT_FUNCTION_BACK 0x615B30u  /* past the three displaced instructions */
#define SHOT_FUNCTION_ROUND 0x240u    /* ProjectileWeapon: the programmable round */
#define SHOT_WIELDERS 0x3326730u
#define SHOT_RULES 256
#define SHOT_LOCAL 128
static const uint8_t kShotFirePin[48] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x8D, 0xA8, 0x18, 0xFB, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xB0, 0x05, 0x00, 0x00, 0x0F, 0x29, 0x70, 0xB8, 0x0F, 0x29,
    0x78, 0xA8, 0x44, 0x0F, 0x29, 0x40, 0x98, 0x44, 0x0F, 0x29};
static const uint8_t kShotSitePin[48] = {0x80, 0x7C, 0x24, 0x60, 0x00, 0x48, 0x8B, 0x4C, 0x24, 0x78, 0x4C, 0x8B, 0x75, 0xB0, 0xC6, 0x41,
    0x11, 0x00, 0x0F, 0x84, 0x14, 0x03, 0x00, 0x00, 0x49, 0x8B, 0x4E, 0x68, 0x41, 0x8B, 0xC4, 0x48, 0x8B, 0x0C, 0xC1, 0xE8, 0x57, 0x10,
    0xF0, 0xFF, 0x8B, 0x8D, 0x18, 0x03, 0x00, 0x00, 0x4C, 0x8D};
static const uint8_t kShotPelletPin[48] = {0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x49, 0x8D, 0xAB,
    0xA8, 0xF5, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x20, 0x0B, 0x00, 0x00, 0x41, 0x0F, 0x29, 0x73, 0xB8, 0x41, 0x0F, 0x29, 0x7B, 0xA8, 0x48,
    0x8B, 0x05, 0xA4, 0x66, 0x02, 0x02, 0x48, 0x33, 0xC4, 0x48};
static const uint8_t kShotFunctionPin[48] = {0x41, 0x8B, 0x85, 0x40, 0x02, 0x00, 0x00, 0x85, 0xC0, 0x44, 0x0F, 0x45, 0xE0, 0x49, 0x8B, 0x85,
    0x48, 0x02, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x48, 0x0F, 0x45, 0xD8, 0x48, 0x8B, 0x46, 0x68, 0x8B, 0x54, 0x24, 0x40, 0x48, 0x8B, 0x0C,
    0xD0, 0x8B, 0x41, 0x08, 0x3B, 0x05, 0xCD, 0xE0, 0xE6, 0x02};

/* A swap's slot: a weapon's normal round, or its programmable round. native: that round here (its record's, or stock:
 * its ammunition's stock type); rounds: a rounds feed's */
enum { SHOT_NORMAL, SHOT_FUNCTION };
typedef struct ShotRule { uint64_t peer, resource; uint32_t type, native, stock; int slot, rounds, ready; } ShotRule;
typedef struct ShotLocal { uint64_t resource; int slot; uint32_t base, now; } ShotLocal;
typedef struct ShotPlayer { uint64_t peer; uint32_t avatar; int local; } ShotPlayer;
static ShotRule g_shotRules[SHOT_RULES];
static int g_shotRuleCount;
static ShotLocal g_shotLocal[SHOT_LOCAL];
static int g_shotLocalCount;
static ShotPlayer g_shotPlayers[4];
static int g_shotPlayerCount;
static volatile LONG g_shotsActive; /* rules are set: co-op with every player keeping their own */
static SRWLOCK g_shotsLock = SRWLOCK_INIT;
static int g_shotState;              /* 0 not installed yet, 1 installed, -1 refused */
static int g_shotFunctionHooked;     /* the programmable round's site too */
static ULONGLONG g_shotFirstTry;
static volatile LONG g_shotChanged;  /* shots whose type a rule changed, for the log */
/* the last weapon with a lobby swap that fired with no player found as its wielder (an underbarrel the Wieldable manager
 * may not list), and that wielder, for the log */
static volatile LONG64 g_shotUnowned;
static volatile LONG g_shotUnownedWielder;

static volatile uintptr_t g_shotGame; /* game.dll, as the tick last found it */

static uint32_t ShotWielderOf(uint32_t weapon)
{
    uintptr_t game = g_shotGame, wielders, of;
    uint32_t index;
    if (!game || !EvpPtr(game + SHOT_WIELDERS, &wielders) || !EvpHash(wielders + 0x18, weapon, 8, 4, &index) || !EvpPtr(wielders + 0x38, &of)) return 0;
    return EvpU32(of + (uintptr_t)index * 4, 0);
}
/* The weapon's current wielder; the native fixture replaces it. */
static uint32_t (*g_shotWielder)(uint32_t weapon) = ShotWielderOf;

/* This machine's own swap of a weapon's round (a Local Stats edit at boot, or the editor's live one), by slot: its type
 * before the first and now. Called after every write of ProjectileWeaponComponentData +0 or +0x240 or
 * WeaponRoundsComponentData +64 (stats.c, fields.c: ShotsSlotOf). */
static void ShotsNoteLocal(uint64_t resource, int slot, uint32_t before, uint32_t after)
{
    AcquireSRWLockExclusive(&g_shotsLock);
    int i = 0; /* the same member changed again keeps its first type (the stock round it swaps) */
    while (i < g_shotLocalCount && (g_shotLocal[i].resource != resource || g_shotLocal[i].slot != slot || g_shotLocal[i].now != before)) i++;
    if (i == g_shotLocalCount && g_shotLocalCount < SHOT_LOCAL) g_shotLocal[g_shotLocalCount++] = (ShotLocal){resource, slot, before, after};
    else if (i < g_shotLocalCount) g_shotLocal[i].now = after;
    ReleaseSRWLockExclusive(&g_shotsLock);
}

/* The slot a write of a component's field swaps (ShotsNoteLocal), or -1 none. */
static int ShotsSlotOf(const char* component, uint32_t offset)
{
    if (!strcmp(component, "ProjectileWeaponComponentData")) return offset == 0 ? SHOT_NORMAL : offset == SHOT_FUNCTION_ROUND ? SHOT_FUNCTION : -1;
    return !strcmp(component, "WeaponRoundsComponentData") && offset == 64 ? SHOT_NORMAL : -1;
}

/* The shooter among the players (the weapon's wielder), and *me this machine's player's peer. g_shotsLock held. */
static const ShotPlayer* ShotShooter(uint32_t wielder, uint64_t* me)
{
    const ShotPlayer* shooter = NULL;
    for (int i = 0; i < g_shotPlayerCount; i++)
    {
        if (wielder != 0 && g_shotPlayers[i].avatar == wielder) shooter = &g_shotPlayers[i];
        if (g_shotPlayers[i].local) *me = g_shotPlayers[i].peer;
    }
    return shooter;
}

/* the round's stock type: what this machine fires is its own, swapped or not; a type this machine's swap made
 * (noted where it wrote it, or for a stock member, its own rule) stands for the stock round it replaced (*swapped).
 * g_shotsLock held. */
static uint32_t ShotStock(uint64_t resource, int slot, uint32_t selected, uint64_t me, int* swapped)
{
    uint32_t stock = selected;
    for (int i = 0; i < g_shotLocalCount; i++)
    {
        const ShotLocal* l = &g_shotLocal[i];
        if (l->resource == resource && l->slot == slot && l->now == selected && l->base != l->now) { stock = l->base; *swapped = 1; }
    }
    for (int i = 0; me != 0 && i < g_shotRuleCount; i++)
    {
        const ShotRule* r = &g_shotRules[i];
        if (r->peer == me && r->resource == resource && r->slot == slot && r->stock && r->type == selected && r->stock != r->type)
        { stock = r->stock; *swapped = 1; }
    }
    return stock;
}

/* the shooter's swap of that round: a weapon may carry one per round (an ammunition option, a charge or heat level,
 * a magazine's tracer). g_shotsLock held. */
static const ShotRule* ShotRuleOf(uint64_t peer, uint64_t resource, int slot, uint32_t stock)
{
    const ShotRule* rule = NULL;
    for (int i = 0; i < g_shotRuleCount; i++)
    {
        const ShotRule* r = &g_shotRules[i];
        if (r->peer == peer && r->resource == resource && r->slot == slot && r->ready && r->native == stock) rule = r;
    }
    return rule;
}

/* A weapon with a lobby swap fired with no player found as its wielder: noted for the log (ShotsTick). g_shotsLock held. */
static void ShotNoteUnowned(uint64_t resource, uint32_t wielder)
{
    for (int i = 0; i < g_shotRuleCount; i++)
        if (g_shotRules[i].resource == resource && g_shotUnowned != (LONG64)resource)
        {
            InterlockedExchange(&g_shotUnownedWielder, (LONG)wielder);
            InterlockedExchange64(&g_shotUnowned, (LONG64)resource);
            break;
        }
}

/* The round a player's shot fires: their swap of it, else (another player's gun) the stock round this machine's swap
 * replaced, else what the game chose. g_shotsLock held. */
static uint32_t ShotChoice(const ShotPlayer* shooter, uint64_t me, uint64_t resource, int slot, uint32_t selected)
{
    int swapped = 0;
    uint32_t stock = ShotStock(resource, slot, selected, me, &swapped);
    const ShotRule* rule = ShotRuleOf(shooter->peer, resource, slot, stock);
    /* a round no rule names (a mode's, another ammunition's) stays */
    if (rule != NULL) return rule->type;
    if (!shooter->local && swapped) return stock; /* another player's gun: not this machine's swap */
    return selected;
}

/* The round a shot fires in a slot (see the top): from a stub, on the thread that fires. */
static uint32_t ShotPick(int slot, uint32_t selected, const uint8_t* source)
{
    if (!g_shotsActive || selected == 0 || source == NULL) return selected;
    uint64_t resource = 0;
    uint32_t weapon = 0;
    if (SafeRead((uintptr_t)source, &resource, 8) || SafeRead((uintptr_t)source + 8, &weapon, 4) || resource == 0) return selected;
    uint32_t wielder = weapon ? g_shotWielder(weapon) : 0;
    uint32_t chosen = selected;
    AcquireSRWLockShared(&g_shotsLock);
    uint64_t me = 0;
    const ShotPlayer* shooter = ShotShooter(wielder, &me);
    if (shooter == NULL) ShotNoteUnowned(resource, wielder);
    else chosen = ShotChoice(shooter, me, resource, slot, selected);
    ReleaseSRWLockShared(&g_shotsLock);
    if (chosen != selected) InterlockedIncrement(&g_shotChanged);
    return chosen;
}

/* The normal round (the stub at 0x614081). */
static uint32_t ShotType(uint32_t selected, const uint8_t* source) { return ShotPick(SHOT_NORMAL, selected, source); }

/* The programmable round while the weapon's function is on (the stub at 0x615B23): 0 keeps the shot's type. */
static uint32_t ShotFunctionType(uint32_t round, const uint8_t* source) { return round == 0 ? 0 : ShotPick(SHOT_FUNCTION, round, source); }

/* A rule's suffix at end: "/r" (a rounds feed's), "/f" (the programmable round's), "/<stock type>", or none. Past it,
 * or NULL when malformed. */
static const char* ShotSuffix(const char* end, int* rounds, int* slot, unsigned long* stock)
{
    if (*end == '/' && end[1] == 'r') { *rounds = 1; return end + 2; }
    if (*end == '/' && end[1] == 'f') { *slot = SHOT_FUNCTION; return end + 2; }
    if (*end != '/') return end;
    const char* s = end + 1;
    char* after;
    *stock = strtoul(s, &after, 10);
    if (after == s || *stock == 0 || *stock > 0xFFFF) return NULL;
    return after;
}

/* One rule of the list, at `at`, into *rule: where the next one starts (past its comma), or NULL when malformed. */
static const char* ShotParseRule(const char* at, ShotRule* rule)
{
    char* end;
    uint64_t peer = strtoull(at, &end, 16);
    if (end == at || *end != ':') return NULL;
    const char* r = end + 1;
    uint64_t resource = strtoull(r, &end, 16);
    if (end == r || *end != '=') return NULL;
    const char* t = end + 1;
    unsigned long type = strtoul(t, &end, 10), stock = 0;
    int rounds = 0, slot = SHOT_NORMAL;
    if (end == t || type == 0 || type > 0xFFFF) return NULL;
    const char* next = ShotSuffix(end, &rounds, &slot, &stock);
    if (next == NULL || (*next != ',' && *next != 0)) return NULL;
    *rule = (ShotRule){peer, resource, (uint32_t)type, (uint32_t)stock, (uint32_t)stock, slot, rounds, 0};
    return *next == ',' ? next + 1 : next;
}

/* a rule that was ready stays so (its round here kept). g_shotsLock held. */
static void ShotKeepReady(ShotRule* rules, int count)
{
    for (int i = 0; i < count; i++)
        for (int j = 0; j < g_shotRuleCount; j++)
            if (g_shotRules[j].peer == rules[i].peer && g_shotRules[j].resource == rules[i].resource && g_shotRules[j].type == rules[i].type &&
                g_shotRules[j].stock == rules[i].stock && g_shotRules[j].slot == rules[i].slot && g_shotRules[j].rounds == rules[i].rounds)
                { rules[i].ready = g_shotRules[j].ready; rules[i].native = g_shotRules[j].native; }
}

/* live.c "shots": the lobby's rules, "<peer hex>:<resource hex>=<type>[/r|/f|/<stock type>]" comma separated (the
 * weapon's resource; "/r" a rounds feed's swap, "/f" a programmable round's, "/<stock type>" the swap of the round with
 * that stock type: an ammunition's, a magazine pattern entry's, a charge or heat level's), "-" none (solo, or a host
 * sharing its stats).
 * Replaces the last. 0 with *why when malformed. */
static int ShotsSetRules(const char* list, const char** why)
{
    ShotRule rules[SHOT_RULES];
    int count = 0;
    for (const char* at = list; strcmp(list, "-") && *at;)
    {
        if (count == SHOT_RULES) { *why = "more rules than HellForge keeps"; return 0; }
        at = ShotParseRule(at, &rules[count]);
        if (at == NULL) { *why = "malformed rule"; return 0; }
        count++;
    }
    AcquireSRWLockExclusive(&g_shotsLock);
    ShotKeepReady(rules, count);
    memcpy(g_shotRules, rules, sizeof rules[0] * (size_t)count);
    g_shotRuleCount = count;
    InterlockedExchange(&g_shotsActive, count > 0);
    ReleaseSRWLockExclusive(&g_shotsLock);
    Note("shots: %d player projectile swap(s) from the lobby%s", count, count > 0 && g_shotState != 1 ? " (not applied: the fire site is not hooked)" : "");
    return 1;
}

/* The rule's readiness: its projectile's row and packages (refs.c RefCheck). *native: the slot's round here, its
 * ammunition's stock type, else its WeaponRounds +64 or ProjectileWeapon +0 / +0x240 record's (a shared record: read
 * only); 0 ready when that does not read. */
static int ShotRuleReady(const ShotRule* r, uint32_t* native)
{
    char why[200];
    int ready = r->ready || RefCheck(REF_PROJECTILE, r->type, 0xFFFFFFFFu, 0, why, sizeof why) == REF_OK;
    *native = r->stock;
    if (*native != 0) return ready;
    uint32_t stride = 0;
    uintptr_t record = g_fieldRecord(r->rounds ? "WeaponRoundsComponentData" : "ProjectileWeaponComponentData", r->resource, &stride, 1);
    uint32_t at = r->rounds ? 64 : r->slot == SHOT_FUNCTION ? SHOT_FUNCTION_ROUND : 0;
    if (record == 0 || SafeRead(record + at, native, 4)) ready = 0;
    return ready;
}

/* a record this machine swapped holds its swap: the round it stands for is the stock one. g_shotsLock held. */
static uint32_t ShotLocalBase(const ShotRule* r, uint32_t native)
{
    for (int j = 0; j < g_shotLocalCount; j++)
        if (g_shotLocal[j].resource == r->resource && g_shotLocal[j].slot == r->slot && g_shotLocal[j].now == native)
            native = g_shotLocal[j].base;
    return native;
}

/* The lobby's rule i, still the one copied: its round here, and ready once it is (logged). g_shotsLock held. */
static void ShotRuleUpdate(int i, const ShotRule* copy, uint32_t native, int ready)
{
    ShotRule* r = &g_shotRules[i];
    if (r->peer != copy->peer || r->resource != copy->resource || r->slot != copy->slot) return;
    r->native = native;
    if (!ready || r->ready) return;
    r->ready = 1;
    Note("shots: player %016llx's %016llx fires projectile %u%s", (unsigned long long)copy->peer, (unsigned long long)copy->resource, copy->type,
        copy->slot == SHOT_FUNCTION ? " (its programmable round)" : "");
}

/* The players (peer and Helldiver), and each rule's readiness and round here. */
static void ShotsUpdate(uintptr_t game)
{
    EvpPlayer players[EVT_MAX_PLAYERS];
    int count = EvpPlayers(game, players);
    int ready[SHOT_RULES], rules;
    AcquireSRWLockShared(&g_shotsLock);
    rules = g_shotRuleCount;
    ShotRule copy[SHOT_RULES];
    memcpy(copy, g_shotRules, sizeof copy[0] * (size_t)rules);
    ReleaseSRWLockShared(&g_shotsLock);
    uint32_t native[SHOT_RULES];
    for (int i = 0; i < rules; i++) ready[i] = ShotRuleReady(&copy[i], &native[i]);
    AcquireSRWLockExclusive(&g_shotsLock);
    for (int i = 0; i < rules; i++) if (!copy[i].stock) native[i] = ShotLocalBase(&copy[i], native[i]);
    g_shotPlayerCount = 0;
    for (int i = 0; i < count && i < 4; i++)
        if (players[i].peer != 0) g_shotPlayers[g_shotPlayerCount++] = (ShotPlayer){players[i].peer, players[i].avatar, players[i].local};
    for (int i = 0; i < rules && i < g_shotRuleCount; i++) ShotRuleUpdate(i, &copy[i], native[i], ready[i]);
    ReleaseSRWLockExclusive(&g_shotsLock);
}

static void ShotsNoteUnowned(void)
{
    static LONG64 unownedNoted;
    LONG64 unowned = g_shotUnowned;
    if (unowned != 0 && unowned != unownedNoted)
    {
        unownedNoted = unowned;
        Note("shots: %016llx fired with a swap in the lobby but no player found as its wielder (%u): left as the game chose",
            (unsigned long long)unowned, (unsigned)g_shotUnownedWielder);
    }
}

/* The stubs, in a block near game.dll: ShotType's at the block, ShotFunctionType's at +0x200. */
static void ShotsEmit(uint8_t* stub, uintptr_t game)
{
    /* the stubs: save what the game holds in volatile registers, ShotType(type, source), the type written back, the two
     * displaced instructions, and back; ShotFunctionType(round, source) at +0x200, its three displaced instructions run on
     * what it returns. Both are written before either site jumps to them. */
    static const uint8_t head[] = {
        0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x9C, /* push rax rcx rdx r8 r9 r10 r11, pushfq */
        0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,                               /* sub rsp, 0x80 (shadow space, xmm0-5) */
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30, 0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,
        0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50, 0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,
        0x8B, 0x4D, 0x98,                                                       /* mov ecx, [rbp-0x68] (the chosen type) */
        0x48, 0x8B, 0x55, 0x80,                                                 /* mov rdx, [rbp-0x80] (the shot's source) */
        0x48, 0xB8};                                                            /* mov rax, imm64 (ShotType) */
    static const uint8_t tail[] = {
        0xFF, 0xD0,                                                             /* call rax */
        0x89, 0x45, 0x98,                                                       /* mov [rbp-0x68], eax */
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30, 0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,
        0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50, 0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,
        0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00,                               /* add rsp, 0x80 */
        0x9D, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, /* popfq, pop r11 r10 r9 r8 rdx rcx rax */
        0x80, 0x7C, 0x24, 0x60, 0x00,                                           /* cmp byte [rsp+0x60], 0 (displaced) */
        0x48, 0x8B, 0x4C, 0x24, 0x78,                                           /* mov rcx, [rsp+0x78] (displaced) */
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};                                    /* jmp [rip] -> back */
    static const uint8_t fhead[] = {
        0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x9C,       /* push rcx rdx r8 r9 r10 r11, pushfq */
        0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,                               /* sub rsp, 0x88 (shadow space, xmm0-5) */
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30, 0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,
        0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50, 0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,
        0x41, 0x8B, 0x8D, 0x40, 0x02, 0x00, 0x00,                               /* mov ecx, [r13+0x240] (the programmable round) */
        0x48, 0x8B, 0x94, 0x24, 0x08, 0x01, 0x00, 0x00,                         /* mov rdx, [rsp+0x108] (the source: the function's [rsp+0x48]) */
        0x48, 0xB8};                                                            /* mov rax, imm64 (ShotFunctionType) */
    static const uint8_t ftail[] = {
        0xFF, 0xD0,                                                             /* call rax */
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30, 0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,
        0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50, 0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,
        0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,                               /* add rsp, 0x88 */
        0x9D, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,       /* popfq, pop r11 r10 r9 r8 rdx rcx */
        0x85, 0xC0,                                                             /* test eax, eax (displaced, on the returned round) */
        0x44, 0x0F, 0x45, 0xE0,                                                 /* cmovne r12d, eax (displaced) */
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};                                    /* jmp [rip] -> back */
    uintptr_t fn = (uintptr_t)ShotType, back = game + SHOT_SITE_BACK, ffn = (uintptr_t)ShotFunctionType, fback = game + SHOT_FUNCTION_BACK;
    uint8_t* second = stub + 0x200;
    size_t n = 0;
    memcpy(stub, head, sizeof head); n += sizeof head;
    memcpy(stub + n, &fn, 8); n += 8;
    memcpy(stub + n, tail, sizeof tail); n += sizeof tail;
    memcpy(stub + n, &back, 8);
    n = 0;
    memcpy(second, fhead, sizeof fhead); n += sizeof fhead;
    memcpy(second + n, &ffn, 8); n += 8;
    memcpy(second + n, ftail, sizeof ftail); n += sizeof ftail;
    memcpy(second + n, &fback, 8);
}

/* The fire site's hook (and the programmable round's), once their bytes read as reviewed (60 s for code still being
 * decoded). */
static void ShotsInstall(uintptr_t game, ULONGLONG now)
{
    if (!g_shotFirstTry) g_shotFirstTry = now;
    if (strcmp(g_build, "25480438")) { g_shotState = -1; Note("shots: not hooked (unsupported build)"); return; }
    if (!PinMatches(game + SHOT_FIRE, kShotFirePin, sizeof kShotFirePin) || !PinMatches(game + SHOT_SITE, kShotSitePin, sizeof kShotSitePin))
    {
        if (now - g_shotFirstTry < 60000) return;
        g_shotState = -1;
        Note("shots: the projectile fire site is not the reviewed one; per-player projectile swaps are off");
        return;
    }
    int function = PinMatches(game + SHOT_PELLET, kShotPelletPin, sizeof kShotPelletPin) &&
        PinMatches(game + SHOT_FUNCTION_SITE, kShotFunctionPin, sizeof kShotFunctionPin);
    uint8_t* stub = AllocateNearGame(game, 0x1000);
    if (stub == NULL) { g_shotState = -1; Note("shots: no room near game.dll for the stub"); return; }
    ShotsEmit(stub, game);
    uint8_t* second = stub + 0x200;
    DWORD protect;
    if (!VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &protect)) { g_shotState = -1; Note("shots: the stub could not be made executable"); return; }
    uint8_t jump[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    int32_t rel = (int32_t)((intptr_t)stub - (intptr_t)(game + SHOT_SITE + 5));
    memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + SHOT_SITE, jump, sizeof jump)) { g_shotState = -1; Note("shots: the fire site could not be changed"); return; }
    g_shotState = 1;
    Note("shots: hooked the projectile fire site: in co-op each player's projectile swaps fire as theirs on every machine");
    if (!function) { Note("shots: the programmable round's site is not the reviewed one; its swaps fire as each machine's own"); return; }
    uint8_t fjump[13] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    rel = (int32_t)((intptr_t)second - (intptr_t)(game + SHOT_FUNCTION_SITE + 5));
    memcpy(fjump + 1, &rel, 4);
    if (ApiWrite(game + SHOT_FUNCTION_SITE, fjump, sizeof fjump)) { Note("shots: the programmable round's site could not be changed"); return; }
    g_shotFunctionHooked = 1;
    Note("shots: hooked the programmable round's site too");
}

/* The watch loop, after the game loads: the players (peer and Helldiver), each rule's readiness (its row and
 * packages), and the hook once its bytes read as reviewed (60 s for code still being decoded). */
static void ShotsTick(void)
{
    static ULONGLONG next;
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 250;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0) return;
    g_shotGame = game;
    ShotsUpdate(game);
    ShotsNoteUnowned();
    if (g_shotState != 0) return;
    ShotsInstall(game, now);
}
