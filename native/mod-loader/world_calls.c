/* SDK 21 (hellforge_world.h): particle effects on entities, enemies near a point and the armory's customize screen, for
 * game.dll and helldivers2.exe build 25480438. Where each one reads and what it calls: docs/internals-world.md.
 * Included by hellforge_modloader.c after live_mods.c (channel.c's live pipe, actions.c's and event_polls.c's reads). */

#define WC_ENGINE_API 0x3326308u /* game.dll: the engine's C API (hf_global_EngineApi) */
#define WC_GAME 0x3326340u       /* game.dll: the Game; its +0x10E8 is the world gameplay plays effects in */
#define WC_GAME_WORLD 0x10E8u
#define WC_MENU 0x347CE38u       /* game.dll: the menu object (hf_global_MenuStack): +0 its state, +0x158 the customize screen */
#define WC_MENU_SCREEN 0x158u
#define WC_SCREEN_WEAPON 0x10u
#define WC_PARTICLES_TYPE 0xA8193123526FAD64ull /* MurmurHash64A("particles") */
#define WC_EFFECTS 256
#define WC_INBOX 64
#define WC_FOUND_MAX 2048
#define WC_WIRE_VERSION 1u
#define WC_NO_NETWORK 0x7FFFu

/* The engine calls, each a slot of an engine API table checked against its function and the function's first bytes. */
typedef struct WcSlot { uint32_t table, tableRva, at, rva; const char* hex; } WcSlot;
enum { WC_CREATE, WC_DESTROY, WC_STOP, WC_LINK, WC_POSE, WC_HAS_NODE, WC_NODE, WC_ALIVE, WC_CAN_GET, WC_SLOTS };
static const WcSlot kWcSlots[WC_SLOTS] = {
    {0x08, 0x27CAB40u, 0x278, 0x337D50u, "488b8988082300488b0148ff6008"},         /* World: create particles */
    {0x08, 0x27CAB40u, 0x298, 0x3380B0u, "488b8988082300488b0148ff6070"},         /* World: destroy particles */
    {0x08, 0x27CAB40u, 0x2A8, 0x338130u, "488b8988082300488b0148ffa090000000"},   /* World: stop spawning particles */
    {0x08, 0x27CAB40u, 0x338, 0x338330u, "48895c24084889742410574883ec40488bf9418bd9418bc8"}, /* World: link particles */
    {0x18, 0x27C8F10u, 0x090, 0x1FD220u, "40534883ec204863dae89206eaff488b"},     /* Unit: a node's world pose */
    {0x18, 0x27C8F10u, 0x6D0, 0x1FE590u, "40534883ec208bdae823f3e9ff488bc8"},     /* Unit: has a node of a name */
    {0x18, 0x27C8F10u, 0x6D8, 0x1FE5E0u, "40534883ec208bdae8d3f2e9ff488bc8"},     /* Unit: a node by name */
    {0x18, 0x27C8F10u, 0x720, 0x1FEEB0u, "4883ec28e807eae9ff4885c00f95c0"},       /* Unit: alive */
    {0x10, 0x27C9E60u, 0x388, 0x321930u, "4883ec2848894c24384c8d442430488b0dc3e86e01"}, /* Application: can get */
};
typedef uint32_t (*WcCreateFn)(void* world, uint64_t particles, const float pose[16]);
typedef void (*WcIdFn)(void* world, uint32_t id);
typedef void (*WcLinkFn)(void* world, uint32_t id, uint32_t unit, int32_t node, const float pose[16], uint32_t orphaned, uint8_t flag);
typedef const float* (*WcPoseFn)(uint32_t unit, int32_t node);
typedef uint8_t (*WcHasNodeFn)(uint32_t unit, uint32_t name);
typedef uint32_t (*WcNodeFn)(uint32_t unit, uint32_t name);
typedef uint8_t (*WcAliveFn)(uint32_t unit);
typedef uint8_t (*WcCanGetFn)(uint64_t type, uint64_t name);

/* game.dll's reads: the world (Game +0x10E8) and the customize screen's weapon (menu +0x158, its +0x10, in state 33). */
static const ActPin kWcWorldPins[] = {{0, 0x6541AFu, "488b0d8a21cd02488b89e8100000"}, {0, 0x65417Au, "488b058721cd02"}};
static const ActPin kWcMenuPins[] = {{0, 0x1495282u, "488b15af7bfe01"}, {0, 0x14964F5u, "4c8ba758010000"}, {0, 0x149382Fu, "41895710"},
    {0, 0x14966C4u, "f5644901"}, {0, 0x14954D1u, "ffcb83fb21"}};
/* The Armory's selection (state 2, menu +0x60): its item list at +0x7FDE8, which CUSTOMIZE turns into a loadout id at
 * 0x1458192 through 0x18D2270: the selected category and index (+0x928E8, +0x928EC), the categories' counts (+0x92318,
 * how many at +0x91F14) and the ids (+0x92990), as the game reads them. */
static const ActPin kWcArmoryPins[] = {{0, 0x14ACA13u, "488b4960"}, {0, 0x1458192u, "448b8fd4261100"}, {0, 0x145819Eu, "448b87d0261100"},
    {0, 0x14581A5u, "488d8fe8fd0700"}, {0, 0x18D240Cu, "448b89ec280900"}, {0, 0x18D2416u, "448b81e8280900"},
    {0, 0x18D227Cu, "443b81141f09"}, {0, 0x18D22F0u, "8b848718230900"}, {0, 0x18D2301u, "8b8c8790290900"}};
static const ActPin kWcFactionPins[] = {{0, 0xBB2A2u, "488b3d0fba2603"}};

static int g_wcState;              /* 0 not checked, 1 every slot as reviewed, -1 not */
static uintptr_t g_wcFn[WC_SLOTS];
static SRWLOCK g_wcLock = SRWLOCK_INIT;

typedef struct WcEffect { HfMod* mod; uint64_t peer; uint32_t handle, id, flags; } WcEffect; /* peer 0: this machine's */
static WcEffect g_wcEffects[WC_EFFECTS];
static uint32_t g_wcNextHandle = 1, g_wcNextSlot;
static uint32_t g_wcStateSeen = HF_STATE_NONE;

/* An effect on the wire: what a machine that plays a shared one tells the others. */
#pragma pack(push, 1)
typedef struct WcWire { uint8_t version, op; uint16_t reserved; uint32_t handle, network, node, how; uint64_t particles; float position[3]; } WcWire;
#pragma pack(pop)
enum { WC_OP_PLAY = 1, WC_OP_STOP = 2 };
typedef struct WcInbox { uint64_t from; WcWire wire; } WcInbox;
static WcInbox g_wcInbox[WC_INBOX];
static uint32_t g_wcInboxCount, g_wcTokens = CHANNEL_RATE;
static ULONGLONG g_wcRefilled;
static volatile LONG g_wcTicking, g_wcInboxDropped;
static HfMod g_wcMod; /* the loader's own tick, for effects other machines send */

/* Every engine slot, once: 1 when each points at its reviewed function and that function reads as reviewed. */
static int WcReady(void)
{
    if (g_wcState != 0) return g_wcState > 0;
    uintptr_t game = ApiModuleBase("game.dll", NULL), exe = (uintptr_t)GetModuleHandleW(NULL), api, table, fn;
    if (!game || !EvpPtr(game + WC_ENGINE_API, &api)) return 0; /* not loaded yet: asked again later */
    int ok = ActPins(game, kWcWorldPins, sizeof kWcWorldPins / sizeof kWcWorldPins[0]);
    for (int i = 0; i < WC_SLOTS && ok; i++)
    {
        const WcSlot* s = &kWcSlots[i];
        ok = EvpPtr(api + s->table, &table) && table == exe + s->tableRva && EvpPtr(table + s->at, &fn) && fn == exe + s->rva &&
            ActBytes(fn, s->hex, 0);
        if (ok) g_wcFn[i] = fn;
    }
    g_wcState = ok ? 1 : -1;
    Note(ok ? "effects: the engine's particle calls read as reviewed" : "effects: the engine's particle calls are not the reviewed ones; play_effect is off");
    return ok;
}

static void* WcWorld(void)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL), g, world;
    return game && EvpPtr(game + WC_GAME, &g) && EvpPtr(g + WC_GAME_WORLD, &world) ? (void*)world : NULL;
}

static uint32_t WcIdString32(const char* name) { return (uint32_t)(Murmur64A(name, strlen(name)) >> 32); }

/* The node of a unit a name gives: 0 (the root) for none, -1 when the unit has no such node. */
static int32_t WcNode(uint32_t unit, uint32_t name)
{
    if (name == 0) return 0;
    if (!(((WcHasNodeFn)g_wcFn[WC_HAS_NODE])(unit, name) & 1)) return -1;
    return (int32_t)((WcNodeFn)g_wcFn[WC_NODE])(unit, name);
}

/* What to play: the entity's unit (0: in the world), its node, the particles and the offset or world position. */
typedef struct WcPlay { uint32_t entity, unit, nodeName; uint64_t particles; float position[3]; } WcPlay;

/* The particle id the engine made, or 0 with why. On the game thread. */
static uint32_t WcPlayHere(const WcPlay* p, const char** why)
{
    void* world = WcWorld();
    float pose[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, p->position[0], p->position[1], p->position[2], 1};
    if (!WcReady()) { *why = "the engine's particle calls are not the reviewed ones"; return 0; }
    if (world == NULL) { *why = "there is no game world now"; return 0; }
    if (!(((WcCanGetFn)g_wcFn[WC_CAN_GET])(WC_PARTICLES_TYPE, p->particles) & 1)) { *why = "the particles are not loaded"; return 0; }
    if (!p->unit) return ((WcCreateFn)g_wcFn[WC_CREATE])(world, p->particles, pose);
    if (!(((WcAliveFn)g_wcFn[WC_ALIVE])(p->unit) & 1)) { *why = "the entity's unit is gone"; return 0; }
    int32_t node = WcNode(p->unit, p->nodeName);
    if (node < 0) { *why = "the entity's unit has no such node"; return 0; }
    const float* unitPose = ((WcPoseFn)g_wcFn[WC_POSE])(p->unit, node);
    if (unitPose == NULL) { *why = "the entity's unit has no pose"; return 0; }
    float at[16];
    memcpy(at, unitPose, sizeof at);
    uint32_t id = ((WcCreateFn)g_wcFn[WC_CREATE])(world, p->particles, at);
    /* orphaned 0 ("destroy"): the effect goes with the unit, so a dead entity never leaves it hanging */
    ((WcLinkFn)g_wcFn[WC_LINK])(world, id, p->unit, node, pose, 0, 0);
    return id;
}

/* A slot for a new effect: a free one, else the oldest. */
static WcEffect* WcSlotFree(void)
{
    for (int i = 0; i < WC_EFFECTS; i++) if (g_wcEffects[i].handle == 0) return &g_wcEffects[i];
    WcEffect* e = &g_wcEffects[g_wcNextSlot++ % WC_EFFECTS];
    memset(e, 0, sizeof *e);
    return e;
}
static WcEffect* WcFind(HfMod* mod, uint64_t peer, uint32_t handle)
{
    for (int i = 0; i < WC_EFFECTS; i++)
        if (g_wcEffects[i].handle == handle && g_wcEffects[i].peer == peer && (mod == NULL || g_wcEffects[i].mod == mod)) return &g_wcEffects[i];
    return NULL;
}

/* A line for the other machines, under the effects' own budget (CHANNEL_RATE a second): 0 queued, 1 not. */
static int WcSend(const WcWire* wire)
{
    char hex[sizeof *wire * 2 + 1], line[64 + sizeof hex];
    ULONGLONG now = GetTickCount64();
    uint32_t role;
    AcquireSRWLockShared(&g_sessionLock);
    role = g_session.role;
    ReleaseSRWLockShared(&g_sessionLock);
    if (role == HF_SESSION_SOLO) return 0; /* nobody else plays it */
    if (now - g_wcRefilled >= 1000) { g_wcTokens = CHANNEL_RATE; g_wcRefilled = now; }
    if (g_wcTokens == 0) return 1;
    g_wcTokens--;
    ChannelHex((const uint8_t*)wire, sizeof *wire, hex);
    snprintf(line, sizeof line, "send\t%s\tall\t%s", WC_CHANNEL, hex);
    return LiveQueue(line) ? 0 : 1;
}

/* An entity's unit and network id (WC_NO_NETWORK: none); 0 when it is not an entity of this world. */
static int WcEntity(uint32_t entity, uint32_t* unit, uint32_t* network)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    const uint8_t* d = game && entity ? ((const uint8_t* (*)(uint32_t))(game + kBhvFunctions[F_DESCRIPTOR].rva))(entity) : NULL;
    if (d == NULL || SafeRead((uintptr_t)d + 0xC, unit, 4) || SafeRead((uintptr_t)d + 0x10, network, 4)) return 0;
    *network &= 0xFFFFu;
    /* the descriptor's network id must name this entity back, or the others would play it on something else */
    if (*network != WC_NO_NETWORK && ApiEntityOfNetwork(NULL, *network) != entity) *network = WC_NO_NETWORK;
    return *unit != 0;
}

static int WcGameThread(HfMod* mod, const char* what)
{
    if (g_gameThread != 0 && GetCurrentThreadId() == g_gameThread) return 1;
    ApiLog(mod, "%s: refused: call it on the game thread (a hook of the game's update, or on_game_thread)", what);
    return 0;
}

static void WcTick(HfMod* mod, float dt, void* user);

/* The other machines' copy of a shared effect, by the entity's network id. */
static void WcShare(HfMod* mod, uint32_t handle, uint32_t network, const WcPlay* p)
{
    WcWire wire = {WC_WIRE_VERSION, WC_OP_PLAY, 0, handle, network, p->nodeName, 0, p->particles, {0}};
    memcpy(wire.position, p->position, sizeof wire.position);
    if (WcSend(&wire)) ApiLog(mod, "play_effect: effect %u plays here only: more than %u shared effects a second", handle, CHANNEL_RATE);
}

/* SDK 21, play_effect: an effect here and, shared, on the other machines. Its handle, or 0 (the mod's log says why). */
static uint32_t ApiPlayEffect(HfMod* mod, const HfEffect* effect)
{
    if (effect == NULL || effect->size < offsetof(HfEffect, position) + sizeof effect->position)
    { ApiLog(mod, "play_effect: refused: no HfEffect, or its size is not set"); return 0; }
    if (!WcGameThread(mod, "play_effect")) return 0;
    WcPlay p = {effect->entity, 0, effect->node && effect->node[0] ? WcIdString32(effect->node) : 0, effect->particles, {0}};
    memcpy(p.position, effect->position, sizeof p.position);
    uint32_t network = WC_NO_NETWORK;
    if (p.entity && !WcEntity(p.entity, &p.unit, &network)) { ApiLog(mod, "play_effect: refused: entity %u is not in this world", p.entity); return 0; }
    if ((effect->flags & HF_EFFECT_SHARED) && p.entity && network == WC_NO_NETWORK)
    { ApiLog(mod, "play_effect: refused: entity %u has no network id, so the other machines cannot find it", p.entity); return 0; }
    const char* why = NULL;
    uint32_t id = WcPlayHere(&p, &why);
    if (!id) { ApiLog(mod, "play_effect: refused: %s", why ? why : "the engine made no effect"); return 0; }
    AcquireSRWLockExclusive(&g_wcLock);
    WcEffect* e = WcSlotFree();
    *e = (WcEffect){mod, 0, g_wcNextHandle++, id, effect->flags};
    if (g_wcNextHandle == 0) g_wcNextHandle = 1;
    uint32_t handle = e->handle;
    ReleaseSRWLockExclusive(&g_wcLock);
    if (InterlockedExchange(&g_wcTicking, 1) == 0) ApiOnGameThread(&g_wcMod, WcTick, NULL);
    if (effect->flags & HF_EFFECT_SHARED) WcShare(mod, handle, network, &p);
    return handle;
}

static void WcEnd(const WcEffect* e, uint32_t how)
{
    void* world = WcWorld();
    if (world == NULL || g_wcState <= 0) return;
    ((WcIdFn)g_wcFn[how == HF_EFFECT_DESTROY ? WC_DESTROY : WC_STOP])(world, e->id); /* an id that already ended is ignored */
}

/* SDK 21, stop_effect: 1 stopped, 0 refused (logged). */
static int ApiStopEffect(HfMod* mod, uint32_t handle, uint32_t how)
{
    if (how > HF_EFFECT_DESTROY) { ApiLog(mod, "stop_effect: refused: how is HF_EFFECT_STOP or HF_EFFECT_DESTROY"); return 0; }
    if (!WcGameThread(mod, "stop_effect")) return 0;
    AcquireSRWLockExclusive(&g_wcLock);
    WcEffect* found = WcFind(mod, 0, handle);
    WcEffect e = found ? *found : (WcEffect){0};
    if (found) memset(found, 0, sizeof *found);
    ReleaseSRWLockExclusive(&g_wcLock);
    if (!e.handle) { ApiLog(mod, "stop_effect: refused: %u is not one of the mod's effects now", handle); return 0; }
    WcEnd(&e, how);
    if (e.flags & HF_EFFECT_SHARED)
    {
        WcWire wire = {WC_WIRE_VERSION, WC_OP_STOP, 0, handle, WC_NO_NETWORK, 0, how, 0, {0}};
        if (WcSend(&wire)) ApiLog(mod, "stop_effect: effect %u stops here only: more than %u shared effects a second", handle, CHANNEL_RATE);
    }
    return 1;
}

/* Another machine's effect, as the live pipe delivers it (its thread): queued for the game thread. */
static void WcReceive(uint64_t from, const uint8_t* data, uint32_t bytes)
{
    WcWire wire;
    if (bytes != sizeof wire) return;
    memcpy(&wire, data, sizeof wire);
    if (wire.version != WC_WIRE_VERSION) return;
    AcquireSRWLockExclusive(&g_wcLock);
    if (g_wcInboxCount < WC_INBOX) g_wcInbox[g_wcInboxCount++] = (WcInbox){from, wire};
    else InterlockedIncrement(&g_wcInboxDropped);
    ReleaseSRWLockExclusive(&g_wcLock);
    if (InterlockedExchange(&g_wcTicking, 1) == 0) ApiOnGameThread(&g_wcMod, WcTick, NULL);
}

static void WcPlayRemote(const WcInbox* in)
{
    const WcWire* w = &in->wire;
    uint32_t network = WC_NO_NETWORK;
    WcPlay p = {0, 0, w->node, w->particles, {w->position[0], w->position[1], w->position[2]}};
    if (w->network != WC_NO_NETWORK)
    {
        p.entity = ApiEntityOfNetwork(NULL, w->network);
        if (p.entity == 0xFFFFFFFFu || !WcEntity(p.entity, &p.unit, &network)) return; /* not here (yet): nothing to show it on */
    }
    const char* why = NULL;
    uint32_t id = WcPlayHere(&p, &why);
    if (!id) { Note("effects: another player's effect %u does not play here: %s", w->handle, why ? why : "the engine made none"); return; }
    AcquireSRWLockExclusive(&g_wcLock);
    *WcSlotFree() = (WcEffect){NULL, in->from, w->handle, id, 0};
    ReleaseSRWLockExclusive(&g_wcLock);
}

static void WcStopRemote(const WcInbox* in)
{
    AcquireSRWLockExclusive(&g_wcLock);
    WcEffect* found = WcFind(NULL, in->from, in->wire.handle);
    WcEffect e = found ? *found : (WcEffect){0};
    if (found) memset(found, 0, sizeof *found);
    ReleaseSRWLockExclusive(&g_wcLock);
    if (e.handle) WcEnd(&e, in->wire.how);
}

/* The game thread: other machines' effects played or stopped, and every effect forgotten when the game's state changes
 * (a world's particle ids end with it). */
static void WcTick(HfMod* mod, float dt, void* user)
{
    (void)mod; (void)dt; (void)user;
    uint32_t state = ReadState(ApiModuleBase("game.dll", NULL));
    WcInbox inbox[WC_INBOX];
    AcquireSRWLockExclusive(&g_wcLock);
    if (state != g_wcStateSeen) { memset(g_wcEffects, 0, sizeof g_wcEffects); g_wcStateSeen = state; }
    uint32_t count = g_wcInboxCount;
    memcpy(inbox, g_wcInbox, sizeof inbox[0] * count);
    g_wcInboxCount = 0;
    ReleaseSRWLockExclusive(&g_wcLock);
    for (uint32_t i = 0; i < count; i++) (inbox[i].wire.op == WC_OP_PLAY ? WcPlayRemote : WcStopRemote)(&inbox[i]);
    LONG dropped = InterlockedExchange(&g_wcInboxDropped, 0);
    if (dropped) Note("effects: %ld effect message(s) from other players came faster than a frame takes %d; they were dropped", dropped, WC_INBOX);
}

/* ---- find_enemies ------------------------------------------------------------------------------------------------- */

typedef struct WcScan { uintptr_t descriptors, records, ext, factions, masks; uint32_t live; int factionsOk; } WcScan;
static HfFoundEnemy g_wcFound[WC_FOUND_MAX];
static SRWLOCK g_wcFoundLock = SRWLOCK_INIT;

static int WcScanOf(uintptr_t game, WcScan* s)
{
    uintptr_t manager;
    memset(s, 0, sizeof *s);
    if (!EvpPtr(game + EVP_HEALTH, &manager) || !EvpPtr(manager + 0x1048, &s->descriptors) || !EvpPtr(manager + 0x1058, &s->records)) return 0;
    EvpPtr(manager + 0x1060, &s->ext);
    s->live = EvpU32(manager + 0x1020, 0);
    s->factionsOk = ActPins(game, kWcFactionPins, 1) && EvpPtr(game + BHV_FACTIONS, &s->factions) && EvpPtr(s->factions + 0x12078, &s->masks);
    return s->live <= 0x100000;
}

/* The faction mask of an entity, 0 when it has none (or the factions do not read). */
static uint32_t WcFaction(const WcScan* s, uint32_t entity)
{
    uint32_t index;
    return s->factionsOk && EvpHash(s->factions + 0x12050, entity, 8, 4, &index) ? EvpU32(s->masks + (uintptr_t)index * 4, 0) : 0;
}

/* Health index i as an enemy within reach: 1 with *f filled. */
static int WcEnemyAt(const WcScan* s, uint32_t i, const float* centre, float radius, HfFoundEnemy* f)
{
    uintptr_t descriptor;
    uint8_t d[24];
    uint32_t life = EvpU32(s->records + (uintptr_t)i * 0x1B8 + 0x19C, 2);
    if (life >= 2 || !EvpPtr(s->descriptors + (uintptr_t)i * 8, &descriptor) || SafeRead(descriptor, d, sizeof d)) return 0;
    memset(f, 0, sizeof *f);
    memcpy(&f->resource, d, 8); memcpy(&f->entity, d + 8, 4); memcpy(&f->unit, d + 12, 4);
    f->faction = WcFaction(s, f->entity);
    if (f->faction ? (f->faction & 1) != 0 : !ActEnemy(f->resource)) return 0;
    if (!EvpAlive(f->entity) || !EvpUnitPosition(f->unit, f->position)) return 0;
    float dx = f->position[0] - centre[0], dy = f->position[1] - centre[1], dz = f->position[2] - centre[2];
    f->distance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(f->distance <= radius)) return 0;
    f->health = (int32_t)EvpU32(s->records + (uintptr_t)i * 0x1B8 + 0x14, 0);
    f->max_health = s->ext ? (int32_t)EvpU32(s->ext + (uintptr_t)i * 0x1C + 0x14, 0xFFFFFFFFu) : -1;
    f->flags = (life == 1 ? HF_FOUND_DOWNED : 0) | (d[20] & 1 ? HF_FOUND_AUTHORITY : 0);
    return 1;
}

static int WcNearer(const void* a, const void* b)
{
    float x = ((const HfFoundEnemy*)a)->distance, y = ((const HfFoundEnemy*)b)->distance;
    return x < y ? -1 : x > y;
}

/* SDK 21, find_enemies: how many enemies were written to out, nearest first. */
static uint32_t ApiFindEnemies(HfMod* mod, const float centre[3], float radius, HfFoundEnemy* out, uint32_t capacity)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    WcScan s;
    if (centre == NULL || (capacity > 0 && out == NULL) || !(radius > 0.0f)) { ApiLog(mod, "find_enemies: refused: no centre, no room or no radius"); return 0; }
    if (!game || ReadState(game) != HF_STATE_MISSION || !WcScanOf(game, &s)) return 0;
    AcquireSRWLockExclusive(&g_wcFoundLock);
    uint32_t found = 0;
    for (uint32_t i = 0; i < s.live && found < WC_FOUND_MAX; i++) found += (uint32_t)WcEnemyAt(&s, i, centre, radius, &g_wcFound[found]);
    qsort(g_wcFound, found, sizeof g_wcFound[0], WcNearer);
    uint32_t written = found < capacity ? found : capacity;
    if (written) memcpy(out, g_wcFound, sizeof out[0] * written);
    ReleaseSRWLockExclusive(&g_wcFoundLock);
    return written;
}

/* ---- armory_weapon ------------------------------------------------------------------------------------------------ */

#define WC_ARMORY_SCREEN 0x60u
#define WC_ARMORY_LIST 0x7FDE8u
#define WC_LIST_CATEGORIES 0x91F14u
#define WC_LIST_COUNTS 0x92318u
#define WC_LIST_IDS 0x92990u
#define WC_LIST_SELECTED 0x928E8u /* the category, then the index in it (-1: none) */
#define WC_LIST_MAX 4096u

/* The loadout id the Armory's CUSTOMIZE would open: its selected item, found as the game finds it; 0 for none. */
static uint32_t WcArmorySelected(uintptr_t object)
{
    uintptr_t screen;
    if (!EvpPtr(object + WC_ARMORY_SCREEN, &screen)) return 0;
    uintptr_t list = screen + WC_ARMORY_LIST;
    uint32_t category = EvpU32(list + WC_LIST_SELECTED, ~0u), index = EvpU32(list + WC_LIST_SELECTED + 4, ~0u);
    if (index >= WC_LIST_MAX || category >= EvpU32(list + WC_LIST_CATEGORIES, 0) || category >= 64) return 0;
    for (uint32_t k = 0; k < category; k++) index += EvpU32(list + WC_LIST_COUNTS + 4 * k, WC_LIST_MAX);
    return index < WC_LIST_MAX ? EvpU32(list + WC_LIST_IDS + 4 * index, 0) : 0;
}

/* Checks the pins once: *state 1 as reviewed, -1 not. */
static void WcProve(uintptr_t game, int* state, const ActPin* pins, size_t count)
{
    if (*state == 0) *state = ActPins(game, pins, count) ? 1 : -1;
}

/* SDK 21, armory_weapon: the weapon of the customize screen or the item selected in the Armory; 0 on any other menu.
 * Each screen's reads are proven apart, so one that moved in an update leaves the other working. */
static uint32_t ApiArmoryWeapon(HfMod* mod, uint32_t* menu)
{
    (void)mod;
    static int customize, armory;
    uintptr_t game = ApiModuleBase("game.dll", NULL), object, screen;
    if (menu) *menu = 0;
    if (!game) return 0;
    WcProve(game, &customize, kWcMenuPins, sizeof kWcMenuPins / sizeof kWcMenuPins[0]);
    WcProve(game, &armory, kWcArmoryPins, sizeof kWcArmoryPins / sizeof kWcArmoryPins[0]);
    if (customize < 0 || !EvpPtr(game + WC_MENU, &object)) return 0;
    uint32_t state = EvpU32(object, 0);
    if (menu) *menu = state;
    if (state == HF_MENU_ARMORY) return armory > 0 ? WcArmorySelected(object) : 0;
    if (state != HF_MENU_CUSTOMIZE || !EvpPtr(object + WC_MENU_SCREEN, &screen)) return 0;
    return EvpU32(screen + WC_SCREEN_WEAPON, 0);
}

static void WorldCallsStart(void)
{
    g_wcMod.api = &g_api;
    snprintf(g_wcMod.id, sizeof g_wcMod.id, "hellforge.effects");
    g_wcMod.log = g_log;
    g_wcMod.loaded = 1;
}
