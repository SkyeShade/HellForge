/* The game events' sources (SDK 7), for game.dll build 25480438. Included by events.c. From HD2Runtime 0.40.1's
 * research (event-state, event-mission, event-combat, event-actions) and game.dll's code (a memory image of the build;
 * the file on disk is packed, so the pins are checked in memory once the copy protection has decoded the code).
 *   state      polled: u32 [[game + 0x3326340] + 0xAC21C] (the Game's state, set by ChangeState 0xAB2FA0).
 *   players    polled: the player manager [game + 0x3326468]: count +0x84 (at most 4); per slot i, peer id u64 at
 *              +0x2C8 + 0x38i, lifecycle u32 at +0x2E0 + 0x38i (2 waiting for its respawn timer, 3 spawned), entity
 *              [+0xE8 + 8i] + 8. Slots move as players leave, so players are told apart by peer id. Local: the peer id
 *              at [[game + 0x347CEF0] + 0xB398].
 *   damage     ConsumeDamageEvent 0x12A7D40 (DamageSystem*, DamageEvent*): every damage event the frame's drain takes
 *              from this machine's queue, before ApplyDamage's multipliers. DamageEvent: +0 target entity, +4 kind,
 *              +8 element, +0x10 dealer and +0x14 owner network ids, +0x18 creditor peer, +0x2C i32 amount, +0x50 the hit.
 *   stratagem  the beacon activation dispatcher 0x6ABDB0 (manager, element, thrower player entity, vec3* position,
 *              record, StratagemType, ...): once per activated beacon, on the machine of its thrower.
 *   kill       KillDispatch 0xB96310 (context, u64 creditor peer, victim entity, dealer entity, kind, element).
 *   spawn      SpawnEntity 0xFDC140 (entity manager, entity id, u64 entity resource, SpawnInfo*: +0 authority byte,
 *              +4 network id), raised after the spawn.
 *   explosion  RequestExplosion 0x13C0A80 (queue, const float position[3], type, source, owner, u64 creditor, nine more
 *              the game's callers pass as 0, NULL, 1, 0, NULL, NULL, NULL, 0, 0), raised after the request.
 *   damage pre the damage consumer's hook, before its original: listeners may change the event's amount (+0x2C).
 *   heal, stat, weapon, bug hole   polled: event_polls.c.
 * Every hook runs on the game thread (the world update), so its events go out at once. A hook is installed only when
 * a mod listens to its kind, once its first bytes read as reviewed (60 s for the copy protection to decode them). */
#define EVT_STATE_GAME 0x3326340u
#define EVT_STATE_AT 0xAC21Cu
#define EVT_PLAYERS 0x3326468u
#define EVT_PLAYERS_BYTES 0x430u
#define EVT_LOCAL_PEER 0x347CEF0u
#define EVT_LOCAL_PEER_AT 0xB398u
#define EVT_MAX_PLAYERS 4

static const uint8_t kConsumeDamagePin[32] = {0x40, 0x55, 0x53, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x68, 0xff,
    0xff, 0xff, 0x48, 0x81, 0xec, 0xb8, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x05, 0xb0, 0x42, 0x39, 0x01};
static const uint8_t kKillPin[32] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xec, 0x40,
    0x48, 0x8b, 0x2d, 0x41, 0x0b, 0x79, 0x02, 0x33, 0xf6, 0x41, 0x8b, 0xd9, 0x41, 0x8b, 0xf8, 0x4c};
static const uint8_t kActivatePin[32] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8d, 0xac, 0x24, 0xb0, 0xfc, 0xff, 0xff, 0x48, 0x81, 0xec, 0x50, 0x04, 0x00, 0x00, 0x48};
static const uint8_t kExplosionPin[26] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x8b, 0x41, 0x20, 0x4c, 0x8b, 0xda, 0x48, 0x8b, 0xd9, 0x3d,
    0x00, 0x01, 0x00, 0x00, 0x0f, 0x83, 0x69, 0x02, 0x00, 0x00};
static const uint8_t kSpawnPin[48] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48,
    0x89, 0x7c, 0x24, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x30, 0x4c, 0x8b, 0xa1, 0x20, 0xf8, 0xf3, 0x00, 0x48,
    0x8b, 0xf1, 0x48, 0x8b, 0x05, 0x79, 0xa2, 0x34, 0x02, 0x49};

/* Each detour takes and passes on spare register and stack slots, so an argument the review did not show still
 * reaches the original unchanged (the spare stack slots are the caller's own). */
typedef uint64_t (*ConsumeDamageFn)(void* system, uint8_t* event, uint64_t r8, uint64_t r9, uint64_t s5, uint64_t s6);
typedef uint64_t (*KillFn)(void* context, uint64_t creditor, uint32_t victim, uint32_t dealer, uint32_t kind, uint32_t element,
    uint64_t s7, uint64_t s8);
typedef uint64_t (*ActivateFn)(void* manager, int32_t element, uint32_t thrower, const float* position, void* record, uint32_t type,
    uint64_t s7, uint64_t s8, uint64_t s9, uint64_t s10);
typedef uint64_t (*SpawnFn)(void* manager, uint32_t entity, uint64_t resource, const uint8_t* info, uint64_t s5, uint64_t s6);
/* every argument after the creditor is an integer or a pointer (passed on as the stack slots they are) */
typedef uint64_t (*ExplosionFn)(void* queue, const float* position, uint32_t type, uint32_t source, uint32_t owner, uint64_t creditor,
    uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14, uint64_t a15);
static ExplosionFn g_explosion;
static ConsumeDamageFn g_consumeDamage;
static KillFn g_kill;
static ActivateFn g_activate;
static SpawnFn g_spawn;
static uintptr_t g_eventsGame;

static void DamageEventOf(const uint8_t* event, HfDamageEvent* e)
{
    memcpy(&e->target, event, 4); memcpy(&e->kind, event + 4, 4); memcpy(&e->element, event + 8, 4);
    memcpy(&e->dealer, event + 0x10, 4); memcpy(&e->owner, event + 0x14, 4); memcpy(&e->creditor, event + 0x18, 8);
    memcpy(&e->amount, event + 0x2C, 4); memcpy(e->position, event + 0x50, 12);
}

static uint64_t ConsumeDamageDetour(void* system, uint8_t* event, uint64_t r8, uint64_t r9, uint64_t s5, uint64_t s6)
{
    /* before it applies: listeners may change the amount (on the game thread, in priority order) */
    if (AnyListener(HF_ON_DAMAGE_PRE) && event != NULL)
    {
        HfDamagePreEvent e = {{{sizeof e, HF_ON_DAMAGE_PRE, 0}}, 0};
        DamageEventOf(event, &e.damage);
        int32_t before = e.damage.amount;
        Dispatch(&e.damage.info);
        int32_t after = e.cancel ? 0 : e.damage.amount;
        if (after < -1) after = 0;
        if (after != before) memcpy(event + 0x2C, &after, 4);
    }
    if (AnyListener(HF_ON_DAMAGE) && event != NULL)
    {
        HfDamageEvent e = {{sizeof e, HF_ON_DAMAGE, 0}};
        DamageEventOf(event, &e);
        Raise(&e.info);
    }
    return g_consumeDamage(system, event, r8, r9, s5, s6);
}

static uint64_t KillDetour(void* context, uint64_t creditor, uint32_t victim, uint32_t dealer, uint32_t kind, uint32_t element, // lint-ok: R12 mirrors KillDispatch 0xB96310 (KillFn)
    uint64_t s7, uint64_t s8)
{
    if (AnyListener(HF_ON_KILL))
    {
        HfKillEvent e = {{sizeof e, HF_ON_KILL, 0}, victim, dealer, creditor, kind, element};
        Raise(&e.info);
    }
    return g_kill(context, creditor, victim, dealer, kind, element, s7, s8);
}

static uint64_t ActivateDetour(void* manager, int32_t element, uint32_t thrower, const float* position, void* record, uint32_t type, // lint-ok: R12 mirrors the beacon activation dispatcher 0x6ABDB0 (ActivateFn)
    uint64_t s7, uint64_t s8, uint64_t s9, uint64_t s10)
{
    if (AnyListener(HF_ON_STRATAGEM))
    {
        HfStratagemEvent e = {{sizeof e, HF_ON_STRATAGEM, 0}, type, thrower, {0}};
        if (position != NULL) memcpy(e.position, position, 12);
        Raise(&e.info);
    }
    return g_activate(manager, element, thrower, position, record, type, s7, s8, s9, s10);
}

static uint64_t SpawnDetour(void* manager, uint32_t entity, uint64_t resource, const uint8_t* info, uint64_t s5, uint64_t s6)
{
    uint64_t result = g_spawn(manager, entity, resource, info, s5, s6);
    BehaviourSpawned(entity, resource, info); /* co-op diagnostics (behaviours.c) */
    if (AnyListener(HF_ON_SPAWN))
    {
        HfSpawnEvent e = {{sizeof e, HF_ON_SPAWN, 0}, entity, 0, resource, 0};
        if (info != NULL) { e.authority = info[0] != 0; memcpy(&e.network, info + 4, 4); }
        Raise(&e.info);
    }
    return result;
}

static uint64_t ExplosionDetour(void* queue, const float* position, uint32_t type, uint32_t source, uint32_t owner, uint64_t creditor, // lint-ok: R12 mirrors RequestExplosion 0x13C0A80 (ExplosionFn)
    uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14, uint64_t a15)
{
    uint64_t result = g_explosion(queue, position, type, source, owner, creditor, a7, a8, a9, a10, a11, a12, a13, a14, a15);
    if (AnyListener(HF_ON_EXPLOSION))
    {
        HfExplosionEvent e = {{sizeof e, HF_ON_EXPLOSION, 0}, type, source, owner, 0, creditor, {0}};
        if (position != NULL) memcpy(e.position, position, 12);
        Raise(&e.info);
    }
    return result;
}

typedef struct EventHook { uint32_t kind, rva; const uint8_t* pin; size_t pinSize; void* detour; void** original; int state; const char* what; } EventHook;
static EventHook g_eventHooks[] = {
    {HF_ON_DAMAGE, 0x12A7D40u, kConsumeDamagePin, sizeof kConsumeDamagePin, (void*)ConsumeDamageDetour, (void**)&g_consumeDamage, 0, "the damage consumer"},
    {HF_ON_KILL, 0xB96310u, kKillPin, sizeof kKillPin, (void*)KillDetour, (void**)&g_kill, 0, "the kill dispatch"},
    {HF_ON_STRATAGEM, 0x6ABDB0u, kActivatePin, sizeof kActivatePin, (void*)ActivateDetour, (void**)&g_activate, 0, "the beacon activation dispatcher"},
    {HF_ON_SPAWN, 0xFDC140u, kSpawnPin, sizeof kSpawnPin, (void*)SpawnDetour, (void**)&g_spawn, 0, "the entity spawner"},
    {HF_ON_EXPLOSION, 0x13C0A80u, kExplosionPin, sizeof kExplosionPin, (void*)ExplosionDetour, (void**)&g_explosion, 0, "the explosion request"},
};
static void EventPollsInstall(uintptr_t game, ULONGLONG waited); /* event_polls.c */
static ULONGLONG g_eventHooksFirstTry;

/* Whether a source's hook is wanted: a mod listens to its kind; the spawn hook also when an entity runs a HellForge
 * behaviour (the co-op diagnostics, behaviours.c); the damage consumer also for damage before it applies. */
static int EventHookWanted(const EventHook* h)
{
    return AnyListener(h->kind) || (h->kind == HF_ON_SPAWN && BehaviourSpawnsWatched()) || (h->kind == HF_ON_DAMAGE && AnyListener(HF_ON_DAMAGE_PRE));
}

/* A source's hook, when it is wanted and its bytes read as reviewed (60 s for them to decode). */
static void EventHookInstall(EventHook* h, uintptr_t game, ULONGLONG now)
{
    if (h->state != 0 || !EventHookWanted(h)) return;
    if (strcmp(g_build, "25480438")) { h->state = -1; Note("events: %s not hooked (unsupported build); no %s events", h->what, EventName(h->kind)); return; }
    if (!PinMatches(game + h->rva, h->pin, h->pinSize))
    {
        if (now - g_eventHooksFirstTry < 60000) return;
        h->state = -1;
        Note("events: %s is not the reviewed one; no %s events", h->what, EventName(h->kind));
        return;
    }
    h->state = LoaderHook(h->what, game + h->rva, h->detour, h->original) ? -1 : 1;
    Note(h->state == 1 ? "events: hooked %s (%s events)" : "events: could not hook %s; no %s events", h->what, EventName(h->kind));
}

/* The watch loop, after HF_EVENT_GAME_LOADED: hooks each source a mod listens to, once its bytes read as reviewed. */
static void EventsInstall(void)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (!game) return;
    g_eventsGame = game;
    ULONGLONG now = GetTickCount64();
    if (!g_eventHooksFirstTry) g_eventHooksFirstTry = now;
    for (size_t i = 0; i < sizeof g_eventHooks / sizeof g_eventHooks[0]; i++) EventHookInstall(&g_eventHooks[i], game, now);
    EventPollsInstall(game, now - g_eventHooksFirstTry);
}

static uint32_t ReadState(uintptr_t game)
{
    uintptr_t g = 0;
    uint32_t state = 0;
    if (!game || SafeRead(game + EVT_STATE_GAME, &g, 8) || g == 0 || SafeRead(g + EVT_STATE_AT, &state, 4) || state > 8) return HF_STATE_NONE;
    return state;
}

#include "event_polls.c"

typedef struct KnownPlayer { uint64_t peer; uint32_t lifecycle; int seen; } KnownPlayer;
static KnownPlayer g_players[EVT_MAX_PLAYERS * 2];
static int g_playerCount;
static uint32_t g_lastState;

/* The player known by a peer id, made known when there is room; NULL none. */
static KnownPlayer* KnownPlayerOf(uint64_t peer)
{
    KnownPlayer* known = NULL;
    for (int k = 0; k < g_playerCount; k++) if (g_players[k].peer == peer) known = &g_players[k];
    if (known == NULL && g_playerCount < EVT_MAX_PLAYERS * 2) { known = &g_players[g_playerCount++]; *known = (KnownPlayer){peer, 0, 0}; }
    return known;
}

/* Slot i of the player list (m: the manager's bytes) against the last update. */
static void PollPlayer(const uint8_t* m, uint32_t i, uint64_t local)
{
    uint64_t peer;
    uint32_t lifecycle, entity = 0;
    uintptr_t player = 0;
    memcpy(&peer, m + 0x2C8 + 0x38 * i, 8);
    memcpy(&lifecycle, m + 0x2E0 + 0x38 * i, 4);
    memcpy(&player, m + 0xE8 + 8 * i, 8);
    if (player) SafeRead(player + 8, &entity, 4);
    if (peer == 0) return;
    KnownPlayer* known = KnownPlayerOf(peer);
    if (known == NULL) return;
    known->seen = 1;
    uint32_t from = known->lifecycle;
    known->lifecycle = lifecycle;
    if (from == lifecycle) return;
    uint32_t kind = lifecycle == 3 ? HF_ON_PLAYER_SPAWN : from == 3 && lifecycle == 2 ? HF_ON_PLAYER_DOWN : 0;
    if (kind == 0 || !AnyListener(kind)) return;
    HfPlayerEvent e = {{sizeof e, kind, 0}, peer, entity, peer == local};
    Dispatch(&e.info);
}

/* Each player's lifecycle against the last update's: spawned (to 3), down (3 to 2). */
static void PollPlayers(uintptr_t game)
{
    uintptr_t manager = 0, peers = 0;
    uint8_t m[EVT_PLAYERS_BYTES];
    if (SafeRead(game + EVT_PLAYERS, &manager, 8) || manager == 0 || SafeRead(manager, m, sizeof m)) return;
    uint64_t local = 0;
    if (!SafeRead(game + EVT_LOCAL_PEER, &peers, 8) && peers != 0) SafeRead(peers + EVT_LOCAL_PEER_AT, &local, 8);
    uint32_t count;
    memcpy(&count, m + 0x84, 4);
    if (count > EVT_MAX_PLAYERS) return; /* not a player list this build has */
    for (int k = 0; k < g_playerCount; k++) g_players[k].seen = 0;
    for (uint32_t i = 0; i < count; i++) PollPlayer(m, i, local);
    for (int k = 0; k < g_playerCount;) /* players who left */
        if (!g_players[k].seen) g_players[k] = g_players[--g_playerCount];
        else k++;
}

static void EventSourcesTick(void)
{
    uintptr_t game = g_eventsGame;
    if (!game) return;
    if (AnyListener(HF_ON_STATE) || AnyListener(HF_ON_MISSION_START) || AnyListener(HF_ON_MISSION_END))
    {
        uint32_t state = ReadState(game), from = g_lastState;
        if (state != HF_STATE_NONE && state != from)
        {
            g_lastState = state;
            HfStateEvent e = {{sizeof e, HF_ON_STATE, 0}, from, state};
            Dispatch(&e.info);
            if (state == HF_STATE_MISSION) { e.info.kind = HF_ON_MISSION_START; Dispatch(&e.info); }
            if (from == HF_STATE_MISSION) { e.info.kind = HF_ON_MISSION_END; Dispatch(&e.info); }
        }
    }
    if (AnyListener(HF_ON_PLAYER_SPAWN) || AnyListener(HF_ON_PLAYER_DOWN)) PollPlayers(game);
    EventPollsTick(game);
}

/* SDK 7: the game's state now. */
static uint32_t ApiGameState(HfMod* mod) { return ReadState(ApiModuleBase("game.dll", NULL)); }
