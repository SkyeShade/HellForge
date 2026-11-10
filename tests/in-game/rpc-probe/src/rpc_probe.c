/* RPC probe (in-game test only, never shipped): SDK 17's rpc_listen and rpc_send in the real game, one line per check
 * in its log for the development harness's modtest (tests/in-game/rpc17.json). It listens to every RPC (HF_RPC_ALL),
 * checks each one's arguments are well formed, decodes the ones it has readers for, and lists the RPCs it saw with
 * their counts once a phase ends. In solo it checks rpc_send is refused; joined, it sends the host one
 * rpc_request_hostability, a query the host answers with rpc_hostability (its score, kept in a table the game refreshes
 * on its own), and checks the answer arrives. Nothing it does changes the game. */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hellforge_mod.h"
#include "hellforge_rpcs.h"

static HfMod* g_mod;
static const HfApi* hf;
#define LOG(...) hf->log(g_mod, __VA_ARGS__)

#define SEEN_MAX 600
static uint32_t g_seenHash[SEEN_MAX], g_seenCount[SEEN_MAX];
static int g_seen;
static long g_received, g_badArgs, g_decoded, g_decodeFailed;
static ULONGLONG g_lastLog, g_missionSince;
static uint32_t g_lastState;
static int g_sendChecked, g_listedShip, g_listedMission;
static ULONGLONG g_askedAt;    /* when the joiner asked the host, 0 before and once answered */
static long g_requestsFromPeers;

/* The first entry of an event that is not a kind the game uses, an array whose count runs past what follows, or a value
 * without a pointer; -1 when every entry is well formed (and -2 for a count without a list). */
static int FirstBadArg(const HfRpcEvent* e)
{
    if (e->count > 0 && e->args == NULL) return -2;
    for (uint32_t i = 0; i < e->count; i++)
    {
        uint32_t k = e->args[i].kind;
        if (k > HF_RPC_KIND_VECTOR2 || k == 8) return (int)i;
        if (k == HF_RPC_KIND_ARRAY && (uint32_t)(uintptr_t)e->args[i].value > e->count - i - 1) return (int)i;
        if (k != HF_RPC_KIND_ARRAY && e->args[i].value == NULL) return (int)i;
    }
    return -1;
}

/* The first few malformed events in full, so a FAIL says which RPC and which entry. */
static void LogBad(const HfRpcEvent* e, int at)
{
    static int logged;
    if (logged++ >= 8) return;
    LOG("PROBE bad rpc 0x%08X from %llx: %u entries, entry %d", e->rpc, (unsigned long long)e->sender, e->count, at);
    for (uint32_t i = 0; e->args != NULL && i < e->count && i < 12; i++)
        LOG("PROBE bad rpc   [%u] kind %u size %u value %p", i, e->args[i].kind, e->args[i].size, e->args[i].value);
}

/* A few readers on the RPCs a mission sends often: they must decode what the game itself decoded. */
static void TryReaders(const HfRpcEvent* e)
{
    int r = -1;
    if (e->rpc == HF_RPC_PICKUP_ENTITY) { HfRpc_pickup_entity p; r = hf_rpc_pickup_entity_read(e, &p); }
    else if (e->rpc == HF_RPC_ADD_PROJECTILE_TO_MANAGER) { HfRpc_add_projectile_to_manager p; r = hf_rpc_add_projectile_to_manager_read(e, &p); }
    else if (e->rpc == HF_RPC_FIRE_SINGLE_ROUND) { HfRpc_fire_single_round p; r = hf_rpc_fire_single_round_read(e, &p); }
    /* these two carry an id_string64 (kind 7), which only co-op sends */
    else if (e->rpc == HF_RPC_SET_INVENTORY) { HfRpc_set_inventory p; r = hf_rpc_set_inventory_read(e, &p); }
    else if (e->rpc == HF_RPC_SET_THROWABLE_PATH) { HfRpc_set_throwable_path p; r = hf_rpc_set_throwable_path_read(e, &p); }
    if (r == 0) g_decoded++;
    else if (r == 1) g_decodeFailed++;
}

static int OnRpc(HfMod* mod, const HfRpcEvent* e, void* user)
{
    (void)mod; (void)user;
    g_received++;
    int bad = FirstBadArg(e);
    if (bad != -1) { g_badArgs++; LogBad(e, bad); }
    TryReaders(e);
    if (e->rpc == HF_RPC_REQUEST_HOSTABILITY) g_requestsFromPeers++;
    if (e->rpc == HF_RPC_HOSTABILITY && g_askedAt != 0)
    {
        HfRpc_hostability answer;
        int read = hf_rpc_hostability_read(e, &answer);
        LOG(read == 0 ? "PROBE rpc_send arrived ok (peer %llx answered with %.1f after %llu ms)" : "PROBE rpc_send arrived FAIL unreadable answer",
            (unsigned long long)e->sender, read == 0 ? answer.float_value : 0.0, GetTickCount64() - g_askedAt);
        g_askedAt = 0;
    }
    int i = 0;
    while (i < g_seen && g_seenHash[i] != e->rpc) i++;
    if (i == g_seen && g_seen < SEEN_MAX) { g_seenHash[g_seen] = e->rpc; g_seenCount[g_seen] = 0; g_seen++; }
    if (i < g_seen) g_seenCount[i]++;
    return HF_RPC_KEEP;
}

static void List(const char* phase)
{
    LOG("PROBE rpc %s: %ld received, %d kinds, %ld with bad arguments, %ld decoded by a reader, %ld a reader refused, %ld hostability requests",
        phase, g_received, g_seen, g_badArgs, g_decoded, g_decodeFailed, g_requestsFromPeers);
    for (int i = 0; i < g_seen; i++) LOG("PROBE rpc seen 0x%08X x%u", g_seenHash[i], g_seenCount[i]);
    LOG(g_received > 0 ? "PROBE rpc %s received ok" : "PROBE rpc %s received FAIL none", phase);
    LOG(g_badArgs == 0 ? "PROBE rpc %s arguments ok" : "PROBE rpc %s arguments FAIL %ld bad", phase, g_badArgs);
    LOG(g_decodeFailed == 0 ? "PROBE rpc %s readers ok" : "PROBE rpc %s readers FAIL %ld refused", phase, g_decodeFailed);
}

static void Reset(void)
{
    g_seen = 0;
    g_received = g_badArgs = g_decoded = g_decodeFailed = g_requestsFromPeers = 0;
}

static void Tick(HfMod* mod, float dt, void* user)
{
    (void)dt; (void)user;
    ULONGLONG now = GetTickCount64();
    if (now - g_lastLog < 1000) return;
    g_lastLog = now;
    uint32_t state = hf->game_state(mod);
    if (state == HF_STATE_MISSION && g_lastState != HF_STATE_MISSION) g_missionSince = now;
    /* the mission's tally 30 s in (a solo run's mission lasts about 40 s, a co-op one is stopped in it) or as it ends */
    if (!g_listedMission && g_lastState == HF_STATE_MISSION && (state != HF_STATE_MISSION || now - g_missionSince > 30000))
    { List("mission"); g_listedMission = 1; }
    /* the ship ends on its first change of state (to loading, usually), and the mission's counts start afresh */
    if (state != HF_STATE_SHIP && g_lastState == HF_STATE_SHIP && !g_listedShip) { List("ship"); g_listedShip = 1; Reset(); }
    g_lastState = state;
    if (g_askedAt != 0 && now - g_askedAt > 15000) { LOG("PROBE rpc_send arrived FAIL no answer in 15 s"); g_askedAt = 0; }
    if (state == HF_STATE_MISSION && !g_sendChecked && now - g_missionSince > 5000)
    {
        HfSession session;
        memset(&session, 0, sizeof session);
        hf->session(mod, &session);
        HfRpc_pickup_entity p = {0, 0};
        g_sendChecked = 1;
        if (session.role == HF_SESSION_HOST)
        {
            LOG(hf_rpc_request_hostability_send(mod, HF_TO_HOST) == 1 ? "PROBE rpc_send to itself refused ok" : "PROBE rpc_send to itself refused FAIL sent");
            return;
        }
        if (session.role == HF_SESSION_JOINER)
        {
            g_askedAt = now;
            if (hf_rpc_request_hostability_send(mod, HF_TO_HOST) != 0) { LOG("PROBE rpc_send arrived FAIL refused"); g_askedAt = 0; }
            return;
        }
        /* pickup_entity only ever in solo, where HellForge refuses it: in co-op it would reach the other machines */
        LOG(hf_rpc_pickup_entity_send(mod, HF_TO_ALL, &p) == 1 ? "PROBE rpc_send solo refused ok" : "PROBE rpc_send solo refused FAIL sent");
        HfRpcArg wrong = {HF_RPC_KIND_FLOAT, 4, &p};
        LOG(hf->rpc_send(mod, HF_RPC_PICKUP_ENTITY, HF_TO_ALL, &wrong, 1) == 1 ? "PROBE rpc_send wrong arguments refused ok"
            : "PROBE rpc_send wrong arguments refused FAIL sent");
        LOG(hf->rpc_send(mod, 0x12345678u, HF_TO_ALL, NULL, 0) == 1 ? "PROBE rpc_send unknown refused ok" : "PROBE rpc_send unknown refused FAIL sent");
    }
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hf = api;
    g_mod = mod;
    if (api->version < 17 || api->size < offsetof(HfApi, rpc_send) + sizeof(void*))
    { api->log(mod, "PROBE sdk FAIL the loader is SDK %u without rpc_send", api->version); return 1; }
    LOG("PROBE sdk ok (SDK %u, game build %s)", api->version, api->build_id());
    LOG(hf->rpc_listen(mod, HF_RPC_ALL, OnRpc, NULL, 0) == 0 ? "PROBE rpc_listen all ok" : "PROBE rpc_listen all FAIL refused");
    LOG(hf->rpc_listen(mod, 0x12345678u, OnRpc, NULL, 0) == 1 ? "PROBE rpc_listen unknown refused ok" : "PROBE rpc_listen unknown refused FAIL taken");
    LOG(hf_on_game_thread(mod, Tick, NULL) == 0 ? "PROBE game_thread ok" : "PROBE game_thread FAIL refused");
    return 0;
}
