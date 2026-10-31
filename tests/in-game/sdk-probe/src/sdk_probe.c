/* SDK probe (in-game test only, never shipped): exercises SDK 16's names in the real game and writes one line per
 * check to its log, "PROBE <check> ok" or "PROBE <check> FAIL <why>", for the development harness's modtest
 * (tests/in-game/sdk16.json): hooks by name (HF_HOOK, asked from init), typed events (hf_on_...), game functions
 * (hf_fn_...) and game globals with their fields (hf_global_...).
 *
 * A check passes the first time it holds and is written then; a phase's checks still open when the phase ends (the
 * ship's when the first mission starts, the mission's when it ends) are written as FAIL. Fields are read with hf->read,
 * so a wrong offset fails a check instead of crashing the game. */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hellforge_mod.h"

static HfMod* g_mod;
static const HfApi* hf;
#define LOG(...) hf->log(g_mod, __VA_ARGS__)

/* ---- checks ------------------------------------------------------------------------------------------------------ */

typedef struct Check { const char* name; int done; char why[96]; } Check;
enum { PHASE_SHIP, PHASE_MISSION, PHASES };
#define MAX_CHECKS 48
static Check g_checks[PHASES][MAX_CHECKS];
static int g_checkCount[PHASES];
static int g_phaseOver[PHASES];

/* Passes the check (once) or keeps why it does not hold yet. */
static void Expect(int phase, const char* name, int holds, const char* why)
{
    Check* c = NULL;
    for (int i = 0; i < g_checkCount[phase] && c == NULL; i++) if (!strcmp(g_checks[phase][i].name, name)) c = &g_checks[phase][i];
    if (c == NULL && g_checkCount[phase] < MAX_CHECKS) { c = &g_checks[phase][g_checkCount[phase]++]; c->name = name; }
    if (c == NULL || c->done) return;
    if (holds) { c->done = 1; LOG("PROBE %s ok", name); }
    else snprintf(c->why, sizeof c->why, "%s", why ? why : "never held");
}

static void EndPhase(int phase)
{
    if (g_phaseOver[phase]) return;
    g_phaseOver[phase] = 1;
    for (int i = 0; i < g_checkCount[phase]; i++)
        if (!g_checks[phase][i].done) LOG("PROBE %s FAIL %s", g_checks[phase][i].name, g_checks[phase][i].why);
    LOG("PROBE phase %s over", phase == PHASE_SHIP ? "ship" : "mission");
}

/* A field of a game object, read safely. */
static int ReadU32(const void* field, uint32_t* out) { return field != NULL && hf->read((uintptr_t)field, out, 4) == 0; }
static int ReadU64(const void* field, uint64_t* out) { return field != NULL && hf->read((uintptr_t)field, out, 8) == 0; }
static int ReadU8(const void* field, uint8_t* out) { return field != NULL && hf->read((uintptr_t)field, out, 1) == 0; }

/* ---- hooks by name ----------------------------------------------------------------------------------------------- */

static volatile LONG g_spawns, g_explosions, g_kills, g_damage, g_beacons, g_heardSpawns;
static HfFn_SpawnEntity g_spawn;
static HfFn_RequestExplosion g_explosion;
static HfFn_KillDispatch g_kill;
static HfFn_ConsumeDamageEvent g_consume;
static HfFn_ActivateStratagemBeacon g_activate;

static uint64_t OnSpawn(void* manager, uint32_t entity, uint64_t resource, const uint8_t* info, uint64_t s5, uint64_t s6)
{
    InterlockedIncrement(&g_spawns);
    return g_spawn(manager, entity, resource, info, s5, s6);
}
static uint64_t OnExplosion(void* queue, const float* position, uint32_t type, uint32_t source, uint32_t owner, uint64_t creditor,
    uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14, uint64_t a15)
{
    InterlockedIncrement(&g_explosions);
    return g_explosion(queue, position, type, source, owner, creditor, a7, a8, a9, a10, a11, a12, a13, a14, a15);
}
static uint64_t OnKill(void* context, uint64_t creditor, uint32_t victim, uint32_t dealer, uint32_t kind, uint32_t element,
    uint64_t s7, uint64_t s8)
{
    InterlockedIncrement(&g_kills);
    return g_kill(context, creditor, victim, dealer, kind, element, s7, s8);
}
static uint64_t OnConsume(void* system, uint8_t* event, uint64_t r8, uint64_t r9, uint64_t s5, uint64_t s6)
{
    InterlockedIncrement(&g_damage);
    return g_consume(system, event, r8, r9, s5, s6);
}
static uint64_t OnActivate(void* manager, int32_t element, uint32_t thrower, const float* position, void* record, uint32_t type,
    uint64_t s7, uint64_t s8, uint64_t s9, uint64_t s10)
{
    InterlockedIncrement(&g_beacons);
    return g_activate(manager, element, thrower, position, record, type, s7, s8, s9, s10);
}

static void AskHooks(void)
{
    struct { const char* name; int asked; } hooks[] = {
        {"hook asked SpawnEntity", HF_HOOK(g_mod, SpawnEntity, OnSpawn, &g_spawn)},
        {"hook asked RequestExplosion", HF_HOOK(g_mod, RequestExplosion, OnExplosion, &g_explosion)},
        {"hook asked KillDispatch", HF_HOOK(g_mod, KillDispatch, OnKill, &g_kill)},
        {"hook asked ConsumeDamageEvent", HF_HOOK(g_mod, ConsumeDamageEvent, OnConsume, &g_consume)},
        {"hook asked ActivateStratagemBeacon", HF_HOOK(g_mod, ActivateStratagemBeacon, OnActivate, &g_activate)},
    };
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) LOG(hooks[i].asked == 0 ? "PROBE %s ok" : "PROBE %s FAIL refused", hooks[i].name);
}

/* ---- typed events -------------------------------------------------------------------------------------------------- */

static uint64_t g_localPeer;
static ULONGLONG g_missionSince;

static void Loaded(HfMod* mod, HfEvent event, void* user) { LOG("PROBE event game_loaded ok"); }
static void StateChanged(HfMod* mod, const HfStateEvent* state, void* user)
{
    LOG("PROBE info state %u -> %u", state->from, state->to);
}
static void MissionStarted(HfMod* mod, const HfStateEvent* state, void* user)
{
    LOG("PROBE event mission_start ok");
    EndPhase(PHASE_SHIP);
    g_missionSince = GetTickCount64();
}
static void MissionEnded(HfMod* mod, const HfStateEvent* state, void* user)
{
    LOG("PROBE event mission_end ok");
    LOG("PROBE info detours: SpawnEntity %ld, RequestExplosion %ld, KillDispatch %ld, ConsumeDamageEvent %ld, "
        "ActivateStratagemBeacon %ld; hf_on_spawn heard %ld", g_spawns, g_explosions, g_kills, g_damage, g_beacons, g_heardSpawns);
    EndPhase(PHASE_MISSION);
}
static void Spawned(HfMod* mod, const HfSpawnEvent* spawn, void* user) { InterlockedIncrement(&g_heardSpawns); }
static void PlayerLanded(HfMod* mod, const HfPlayerEvent* player, void* user)
{
    if (player->local && g_localPeer == 0) { g_localPeer = player->peer; LOG("PROBE event player_spawn ok"); }
}

static void Listen(void)
{
    struct { const char* name; int answer; } events[] = {
        {"listen game_loaded", hf_on_game_loaded(g_mod, Loaded, NULL)},
        {"listen state", hf_on_state(g_mod, StateChanged, NULL, 0)},
        {"listen mission_start", hf_on_mission_start(g_mod, MissionStarted, NULL, 0)},
        {"listen mission_end", hf_on_mission_end(g_mod, MissionEnded, NULL, 0)},
        {"listen spawn", hf_on_spawn(g_mod, Spawned, NULL, 0)},
        {"listen player_spawn", hf_on_player_spawn(g_mod, PlayerLanded, NULL, 0)},
    };
    for (size_t i = 0; i < sizeof events / sizeof events[0]; i++) LOG(events[i].answer == 0 ? "PROBE %s ok" : "PROBE %s FAIL refused", events[i].name);
}

/* ---- functions and globals --------------------------------------------------------------------------------------- */

static void Functions(int phase)
{
    Expect(phase, "fn ConsumeDamageEvent", hf_fn_ConsumeDamageEvent(g_mod) != NULL, "NULL");
    Expect(phase, "fn KillDispatch", hf_fn_KillDispatch(g_mod) != NULL, "NULL");
    Expect(phase, "fn ActivateStratagemBeacon", hf_fn_ActivateStratagemBeacon(g_mod) != NULL, "NULL");
    Expect(phase, "fn SpawnEntity", hf_fn_SpawnEntity(g_mod) != NULL, "NULL");
    Expect(phase, "fn RequestExplosion", hf_fn_RequestExplosion(g_mod) != NULL, "NULL");
    Expect(phase, "fn AddHealthFraction", hf_fn_AddHealthFraction(g_mod) != NULL, "NULL");
    Expect(phase, "fn RestoreZone", hf_fn_RestoreZone(g_mod) != NULL, "NULL");
    Expect(phase, "fn QueueStatusRequest", hf_fn_QueueStatusRequest(g_mod) != NULL, "NULL");
    Expect(phase, "fn SetVelocity", hf_fn_SetVelocity(g_mod) != NULL, "NULL");
    Expect(phase, "fn FireProjectile", hf_fn_FireProjectile(g_mod) != NULL, "NULL");
}

/* The checks that hold on the ship and in a mission alike. */
static void Common(int phase, uint32_t wantState)
{
    char why[96];
    uint32_t state = 0, count = 0;
    uint64_t peer = 0;
    HfGame* game = hf_global_Game(g_mod);
    int read = game && ReadU32(&game->state, &state);
    snprintf(why, sizeof why, "%s, state %u, game_state %u", game ? "read" : "NULL", state, hf->game_state(g_mod));
    Expect(phase, "global Game state", read && state == wantState && state == hf->game_state(g_mod), why);
    HfLocalUser* user = hf_global_LocalUser(g_mod);
    read = user && ReadU64(&user->peer, &peer);
    snprintf(why, sizeof why, "%s, peer %llx", user ? "read" : "NULL", (unsigned long long)peer);
    Expect(phase, "global LocalUser peer", read && peer != 0, why);
    HfPlayerManager* players = hf_global_PlayerManager(g_mod);
    read = players && ReadU32(&players->count, &count);
    snprintf(why, sizeof why, "%s, count %u", players ? "read" : "NULL", count);
    Expect(phase, "global PlayerManager count", read && count >= 1 && count <= 4, why);
}

static void MissionGlobals(void)
{
    char why[96];
    uint32_t value = 0, second = 0;
    uint8_t running = 0;
    uint64_t peer = 0;
    HfLocalUser* user = hf_global_LocalUser(g_mod);
    int read = user && ReadU64(&user->peer, &peer);
    snprintf(why, sizeof why, "peer %llx, HF_ON_PLAYER_SPAWN's %llx", (unsigned long long)peer, (unsigned long long)g_localPeer);
    Expect(PHASE_MISSION, "global LocalUser peer is the local player's", read && g_localPeer != 0 && peer == g_localPeer, why);
    HfGameMode* mode = hf_global_GameMode(g_mod);
    void* mission = NULL;
    read = mode && ReadU32(&mode->mode, &value) && ReadU64(&mode->mission, (uint64_t*)&mission);
    snprintf(why, sizeof why, "%s, mode %u, mission %p", mode ? "read" : "NULL", value, mission);
    Expect(PHASE_MISSION, "global GameMode mode", read && value == 1 && mission != NULL, why);
    HfProjectileSystem* projectiles = hf_global_ProjectileSystem(g_mod);
    read = projectiles && ReadU8(&projectiles->running, &running);
    snprintf(why, sizeof why, "%s, running %u", projectiles ? "read" : "NULL", running);
    Expect(PHASE_MISSION, "global ProjectileSystem running", read && running == 1, why);
    HfExplosionQueue* explosions = hf_global_ExplosionQueue(g_mod);
    value = 999;
    Expect(PHASE_MISSION, "global ExplosionQueue count", explosions && ReadU32(&explosions->count, &value) && value <= 256, "NULL or over 256");
    HfCombatQueues* combat = hf_global_CombatQueues(g_mod);
    value = second = 99999;
    Expect(PHASE_MISSION, "global CombatQueues counts", combat && ReadU32(&combat->damage_count, &value) && ReadU32(&combat->status_count, &second) &&
        value <= 4096 && second <= 4096, "NULL or over 4096");
    HfHealthManager* health = hf_global_HealthManager(g_mod);
    value = 0;
    void* records = NULL;
    read = health && ReadU32(&health->count, &value) && ReadU64(&health->records, (uint64_t*)&records);
    snprintf(why, sizeof why, "%s, count %u, records %p", health ? "read" : "NULL", value, records);
    Expect(PHASE_MISSION, "global HealthManager count and records", read && value > 0 && value < 100000 && records != NULL, why);
    HfMotionManager* motion = hf_global_MotionManager(g_mod);
    value = 0;
    read = motion && ReadU32(&motion->count, &value) && ReadU64(&motion->records, (uint64_t*)&records);
    snprintf(why, sizeof why, "%s, count %u, records %p", motion ? "read" : "NULL", value, records);
    Expect(PHASE_MISSION, "global MotionManager count and records", read && value > 0 && value < 100000 && records != NULL, why);
    HfDestructibles* destructibles = hf_global_Destructibles(g_mod);
    value = 999999;
    Expect(PHASE_MISSION, "global Destructibles count", destructibles && ReadU32(&destructibles->count, &value) && value < 100000, "NULL or implausible");
    Expect(PHASE_MISSION, "global EntityManager", hf_global_EntityManager(g_mod) != NULL, "NULL");
    Expect(PHASE_MISSION, "global ScoreManager", hf_global_ScoreManager(g_mod) != NULL, "NULL");
    Expect(PHASE_MISSION, "global WielderManager", hf_global_WielderManager(g_mod) != NULL, "NULL");
    Expect(PHASE_MISSION, "global InventoryManager", hf_global_InventoryManager(g_mod) != NULL, "NULL");
    Expect(PHASE_MISSION, "global DestructionRecords", hf_global_DestructionRecords(g_mod) != NULL, "NULL");
    snprintf(why, sizeof why, "%ld detour calls", g_spawns);
    Expect(PHASE_MISSION, "detour SpawnEntity ran", g_spawns > 0, why);
    snprintf(why, sizeof why, "%ld heard", g_heardSpawns);
    Expect(PHASE_MISSION, "event spawn heard", g_heardSpawns > 0, why);
}

static ULONGLONG g_lastCheck;

static void Tick(HfMod* mod, float dt, void* user)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lastCheck < 1000) return;
    g_lastCheck = now;
    uint32_t state = hf->game_state(mod);
    if (state == HF_STATE_SHIP && !g_phaseOver[PHASE_SHIP])
    {
        Functions(PHASE_SHIP);
        Common(PHASE_SHIP, HF_STATE_SHIP);
    }
    else if (state == HF_STATE_MISSION && g_missionSince != 0 && now - g_missionSince > 5000 && !g_phaseOver[PHASE_MISSION])
    {
        Common(PHASE_MISSION, HF_STATE_MISSION);
        MissionGlobals();
    }
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hf = api;
    g_mod = mod;
    if (api->version < 16) { api->log(mod, "PROBE sdk FAIL the loader is SDK %u", api->version); return 1; }
    LOG("PROBE sdk ok (SDK %u, game build %s)", api->version, api->build_id());
    AskHooks();
    Listen();
    LOG(hf_on_game_thread(mod, Tick, NULL) == 0 ? "PROBE game_thread ok" : "PROBE game_thread FAIL refused");
    return 0;
}
