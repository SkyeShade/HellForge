/* Co-op diagnostics (the diagnostics beta, 2026-10-21): what this machine's game sees of HellForge-added content, in
 * coop-diagnostics.log of the session folder, so two players' logs can be laid side by side. Included by
 * hellforge_modloader.c after live.c (the co-op role).
 * The two-PC test of 2026-10-21 (0.1.8) left these open: a friend's Pelican never ran its behaviours on the others'
 * machines; a silo missile launched on one machine stayed in its silo on the other, or vanished with no flight or blast;
 * a host's Orbital Plasma Barrage was not seen by the friend, and the friend's landed on the host's previous beacon on
 * the host's screen; a host's custom support weapon fired no shots the friend could see.
 *   stratagem  every activated beacon of an added stratagem (HF_ON_STRATAGEM: the thrower's machine): its type and name,
 *              the thrower, the beacon, and whether the stratagem's package is resident here
 *   spawn      every spawn of an added entity (HF_ON_SPAWN: remote spawns too): entity, network id, name, authority
 *   explosion  for 45 s after an added stratagem's beacon, an added entity's spawn or a silo launch: the explosions this
 *              machine's world requests, a line a second per type (count, the first's place, source and owner)
 *   packages   at each mission start and 20 s into it: every added stratagem's package, and the packages mods hold,
 *              resident here or not
 *   silo       (abilities.c) each countdown's start and end here and the laser's point; then for 15 s after a launch,
 *              or after the countdown ended here or was overtaken, every 0.25 s: is it still here, its engine (+0x35)
 *              and launched (+0x36) bytes and guidance target (+0xC) as this machine has them (received, without
 *              authority), and where it is (the SeekingMissile manager's rows of 0xC0 at +0x48, +0x48 the position)
 *   barrage    one frame after an added entity spawns, its bombardment block if it is a barrage ([game + 0x3326CE8]:
 *              map +0x30, blocks of 0x1C at +0x60: +0 shells a salvo, +4 salvos, +8 the target, +0x14 heading,
 *              +0x18 seed): a friend's copy holding 0 shells, or another barrage's target, fires nothing or lands there
 *   host       the game's host as the game has it ([game + 0x347CEF0] +0xB3A8, the host's peer; +0xB398 this machine's),
 *              each time it changes: the host can change mid-mission (host migration), and HellForge's host-only parts
 *              (the host's stats, the joiner's read-only editor) follow the lobby, not the game
 * Behaviour entities (the Pelican) keep their own co-op check in modloader.log (behaviours.c). Every line carries the
 * co-op role ("solo", "host", "joiner") at the time. */

#define DIAG_WINDOW_MS 45000u
#define DIAG_BURST_TYPES 32u
#define DIAG_ENTITIES 256u
#define DIAG_MISSILES 8u
#define DIAG_BARRAGES 16u
#define DIAG_BOMBARDMENTS 0x3326CE8u
#define DIAG_PEERS 0x347CEF0u     /* the game's network context: +0xB398 this machine's peer, +0xB3A8 the host's */
#define DIAG_LOCAL_PEER 0xB398u
#define DIAG_HOST_PEER 0xB3A8u

static HfMod g_diagMod;
static int g_diagOn, g_diagListed;
static ULONGLONG g_diagWindowUntil, g_diagBurstSecond, g_diagPackagesAt;
static struct { uint32_t type, count, source, owner; float first[3]; } g_diagBursts[DIAG_BURST_TYPES];
static uint32_t g_diagBurstCount;
static struct { uint32_t entity; const char* name; } g_diagEntities[DIAG_ENTITIES]; /* added entities seen spawning, a ring */
static uint32_t g_diagEntityNext;
static struct { uint32_t missile; ULONGLONG since, next; const char* how; int lastHere; } g_diagMissiles[DIAG_MISSILES];
static struct { uint32_t entity; const char* name; uint64_t update; int authority; } g_diagBarrages[DIAG_BARRAGES]; /* bombardments spawned, their block read next frame */
static uint64_t g_diagFrame, g_diagHost = ~0ull;
static int g_diagDescriptorChecked; /* the entity descriptor lookup is the reviewed one (checked here, not by another part) */
static uintptr_t g_diagGame;

static uintptr_t DiagGame(void)
{
    if (g_diagGame == 0) g_diagGame = ApiModuleBase("game.dll", NULL);
    return g_diagGame;
}

/* Whether this machine has authority over an entity now: the entity descriptor lookup (behaviours.c F_DESCRIPTOR), checked
 * here against its pin before it is ever called, its +0x14 bit 0. */
static const char* DiagAuthority(uint32_t entity)
{
    uintptr_t game = DiagGame();
    if (game == 0 || entity == 0) return "authority unknown";
    if (!g_diagDescriptorChecked)
    {
        uint8_t code[32];
        if (SafeRead(game + kBhvFunctions[F_DESCRIPTOR].rva, code, 32) || memcmp(code, kBhvFunctions[F_DESCRIPTOR].pin, 32)) return "authority unknown";
        g_diagDescriptorChecked = 1;
    }
    const uint8_t* descriptor = ((const uint8_t* (*)(uint32_t))(game + kBhvFunctions[F_DESCRIPTOR].rva))(entity);
    uint32_t flags = 0;
    if (descriptor == NULL || SafeRead((uintptr_t)descriptor + 0x14, &flags, 4)) return "authority unknown";
    return (flags & 1) ? "authority here" : "NOT authority here";
}

/* An entity's index in a manager's map (table at +at: {key, index} pairs; +8 capacity, +0xC the empty key, +0x10 the
 * multiplier), every read checked: -1 when it is not there or the map does not read. */
static int64_t DiagMapFind(uintptr_t manager, uint32_t at, uint32_t entity)
{
    uintptr_t table = 0;
    uint32_t fields[3];
    if (manager == 0 || SafeRead(manager + at, &table, 8) || table == 0 || SafeRead(manager + at + 8, fields, sizeof fields)) return -1;
    uint32_t capacity = fields[0], empty = fields[1], multiplier = fields[2];
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) return -1;
    for (uint32_t i = 0; i < capacity; i++)
    {
        uint32_t pair[2];
        if (SafeRead(table + (uintptr_t)((i + multiplier * entity) & (capacity - 1)) * 8, pair, 8)) return -1;
        if (pair[0] == entity) return pair[1];
        if (pair[0] == empty) return -1;
    }
    return -1;
}

static void CoopDiag(const char* format, ...)
{
    if (!g_diagOn) return;
    va_list arguments;
    va_start(arguments, format);
    WriteLog(g_diagMod.log, "", format, arguments);
    va_end(arguments);
}

static const char* DiagRole(void)
{
    uint32_t role = g_session.role;
    return role == HF_SESSION_HOST ? "host" : role == HF_SESSION_JOINER ? "joiner" : "solo";
}

static void DiagWindow(void) { g_diagWindowUntil = GetTickCount64() + DIAG_WINDOW_MS; }

static const char* DiagPackageState(uint64_t package)
{
    if (package == 0) return "none";
    return PackageResident(package) == PKG_RESIDENT ? "resident here" : "NOT resident here";
}

static const Stratagem* DiagStratagem(uint32_t type)
{
    for (uint32_t i = 0; i < g_stratagemCount; i++) if (g_stratagems[i].type == type) return &g_stratagems[i];
    return NULL;
}

/* An added stratagem's package as its row holds it (its own, else its base's), once the row is in. */
static uint64_t DiagStratagemPackage(const Stratagem* s)
{
    uint64_t package = 0;
    if (s->row != NULL) memcpy(&package, s->row + 0xA8, 8);
    return package;
}

static const char* DiagEntityName(uint64_t resource)
{
    for (int i = 0; i < g_weaponCount; i++) if (g_weapons[i].entity == resource) return g_weapons[i].name;
    return NULL;
}

static const char* DiagSeenEntity(uint32_t entity)
{
    if (entity == 0) return NULL;
    for (uint32_t i = 0; i < DIAG_ENTITIES; i++) if (g_diagEntities[i].entity == entity) return g_diagEntities[i].name;
    return NULL;
}

static void DiagPackages(const char* when)
{
    CoopDiag("packages (%s, %s):", when, DiagRole());
    for (uint32_t i = 0; i < g_stratagemCount; i++)
    {
        const Stratagem* s = &g_stratagems[i];
        uint64_t package = DiagStratagemPackage(s);
        CoopDiag("  stratagem %u %s: package %016llx %s", s->type, s->name, (unsigned long long)package, DiagPackageState(package));
    }
    AcquireSRWLockShared(&g_packagesLock);
    for (int i = 0; i < g_heldCount; i++)
        CoopDiag("  held by mods: %016llx (%s) %s%s", (unsigned long long)g_held[i].id, g_held[i].name ? g_held[i].name : "?",
            g_held[i].failed ? "FAILED: " : DiagPackageState(g_held[i].id), g_held[i].failed ? g_held[i].why : "");
    ReleaseSRWLockShared(&g_packagesLock);
}

/* A silo missile to follow for 15 s (abilities.c): launched here, or another machine's launch reached it first. */
static void CoopDiagMissile(uint32_t missile, const char* how)
{
    if (!g_diagOn || missile == 0) return;
    DiagWindow();
    uint32_t slot = DIAG_MISSILES, oldest = 0;
    for (uint32_t i = 0; i < DIAG_MISSILES; i++)
    {
        if (g_diagMissiles[i].missile == missile) return;
        if (g_diagMissiles[i].missile == 0 && slot == DIAG_MISSILES) slot = i;
        if (g_diagMissiles[i].since < g_diagMissiles[oldest].since) oldest = i;
    }
    if (slot == DIAG_MISSILES) slot = oldest; /* all followed: the oldest goes */
    ULONGLONG now = GetTickCount64();
    g_diagMissiles[slot].missile = missile;
    g_diagMissiles[slot].since = now;
    g_diagMissiles[slot].next = now;
    g_diagMissiles[slot].how = how;
    g_diagMissiles[slot].lastHere = -1;
}

/* The missile as this machine has it, one line. 0 when it is not here. */
static int DiagMissileSample(uint32_t missile, double seconds, const char* how)
{
    uintptr_t missiles = AblManager(ABL_MISSILES), rows = 0;
    int64_t seeking = AblFind(missiles, 0x20, missile);
    if (seeking < 0) return 0;
    const uint8_t* instance = *(uint8_t**)(missiles + 0x50) + (size_t)seeking * 0x38;
    float target[3] = {0, 0, 0}, at[3] = {0, 0, 0};
    memcpy(target, instance + 0xC, 12);
    int placed = !SafeRead(missiles + 0x48, &rows, 8) && rows != 0 && !SafeRead(rows + (uintptr_t)seeking * 0xC0 + 0x48, at, 12);
    CoopDiag("silo: missile %u (%s) %.2f s: engine %u, launched %u, target (%.1f, %.1f, %.1f), at %s(%.1f, %.1f, %.1f) (%s, %s)", missile, how,
        seconds, instance[0x35], instance[0x36], (double)target[0], (double)target[1], (double)target[2], placed ? "" : "? ",
        (double)at[0], (double)at[1], (double)at[2], DiagAuthority(missile), DiagRole());
    return 1;
}

/* An entity's index among the bombardments (the map is keyed by entity id, filled within the spawn), or -1. */
static int64_t DiagBombardment(uint32_t entity, uintptr_t* manager)
{
    *manager = 0;
    if (DiagGame() == 0 || SafeRead(DiagGame() + DIAG_BOMBARDMENTS, manager, 8) || *manager == 0) return -1;
    return DiagMapFind(*manager, 0x30, entity);
}

/* A bombardment's block as this machine has it (barrage.c's remote copies: the block is the creator's once its state
 * arrives; a stale slot holds this machine's previous barrage's). 0 when the entity is no bombardment. authority: as the
 * spawn found it. */
static int DiagBarrageBlock(uint32_t entity, const char* name, int authority)
{
    uintptr_t manager = 0, blocks = 0;
    int64_t index = DiagBombardment(entity, &manager);
    if (index < 0 || SafeRead(manager + 0x60, &blocks, 8) || blocks == 0) return 0;
    uint8_t block[0x1C];
    if (SafeRead(blocks + (uintptr_t)index * 0x1C, block, sizeof block)) return 0;
    uint32_t shells, salvos, seed;
    float target[3], heading;
    memcpy(&shells, block, 4); memcpy(&salvos, block + 4, 4); memcpy(target, block + 8, 12); memcpy(&heading, block + 0x14, 4);
    memcpy(&seed, block + 0x18, 4);
    CoopDiag("barrage %s: entity %u (%s): %u shell(s) a salvo, %u salvo(s), target (%.1f, %.1f, %.1f), heading %.2f, seed %08x%s (%s)", name,
        entity, authority ? "authority at spawn" : "NOT authority at spawn", shells, salvos, (double)target[0], (double)target[1],
        (double)target[2], (double)heading, seed, shells == 0 ? ": NO SHELLS, it fires nothing here" : "", DiagRole());
    return 1;
}

static void DiagOnStratagem(const HfEventInfo* event)
{
    const HfStratagemEvent* e = (const HfStratagemEvent*)event;
    const Stratagem* s = DiagStratagem(e->type);
    if (s == NULL) return;
    uint64_t package = DiagStratagemPackage(s);
    CoopDiag("stratagem %u %s: beacon active at (%.1f, %.1f, %.1f), thrower %u (%s, update %llu); its package %016llx %s",
        e->type, s->name, (double)e->position[0], (double)e->position[1], (double)e->position[2], e->thrower, DiagRole(),
        (unsigned long long)event->update, (unsigned long long)package, DiagPackageState(package));
    DiagWindow();
}

static void DiagOnSpawn(const HfEventInfo* event)
{
    const HfSpawnEvent* e = (const HfSpawnEvent*)event;
    const char* name = DiagEntityName(e->resource);
    if (name == NULL) return;
    g_diagEntities[g_diagEntityNext].entity = e->entity;
    g_diagEntities[g_diagEntityNext].name = name;
    g_diagEntityNext = (g_diagEntityNext + 1) % DIAG_ENTITIES;
    CoopDiag("spawn %s: entity %u, network %u, %s (%s, update %llu)", name, e->entity, e->network,
        e->authority ? "authority here" : "NOT authority here", DiagRole(), (unsigned long long)event->update);
    uintptr_t bombardments;
    uint32_t queued = DIAG_BARRAGES;
    if (DiagBombardment(e->entity, &bombardments) >= 0) /* a barrage: its block a frame later, once its state applied */
        for (queued = 0; queued < DIAG_BARRAGES; queued++)
            if (g_diagBarrages[queued].entity == 0)
            {
                g_diagBarrages[queued].entity = e->entity;
                g_diagBarrages[queued].name = name;
                g_diagBarrages[queued].update = g_diagFrame;
                g_diagBarrages[queued].authority = e->authority != 0;
                break;
            }
    if (queued == DIAG_BARRAGES && bombardments != 0 && DiagMapFind(bombardments, 0x30, e->entity) >= 0)
        CoopDiag("barrage %s: entity %u: %u barrages wait already, its block is not logged", name, e->entity, DIAG_BARRAGES);
    DiagWindow();
}

static void DiagOnExplosion(const HfEventInfo* event)
{
    const HfExplosionEvent* e = (const HfExplosionEvent*)event;
    if (GetTickCount64() > g_diagWindowUntil) return;
    uint32_t i = 0;
    while (i < g_diagBurstCount && g_diagBursts[i].type != e->type) i++;
    if (i == g_diagBurstCount)
    {
        if (g_diagBurstCount == DIAG_BURST_TYPES) return;
        g_diagBursts[i].type = e->type;
        g_diagBursts[i].count = 0;
        g_diagBursts[i].source = e->source;
        g_diagBursts[i].owner = e->owner;
        memcpy(g_diagBursts[i].first, e->position, 12);
        g_diagBurstCount++;
    }
    g_diagBursts[i].count++;
}

static void DiagOnMissionStart(void)
{
    CoopDiag("mission start (%s)", DiagRole());
    if (!g_diagListed)
    {
        g_diagListed = 1;
        CoopDiag("added: %u stratagem(s), %d entities and weapons, %u projectile(s)", g_stratagemCount, g_weaponCount, g_projectileCount);
        for (uint32_t i = 0; i < g_stratagemCount; i++) CoopDiag("  stratagem %u: %s", g_stratagems[i].type, g_stratagems[i].name);
        for (int i = 0; i < g_weaponCount; i++) CoopDiag("  entity %016llx: %s", (unsigned long long)g_weapons[i].entity, g_weapons[i].name);
        for (uint32_t i = 0; i < g_projectileCount; i++) CoopDiag("  projectile %u: %s", g_projectiles[i].type, g_projectiles[i].name);
    }
    DiagPackages("mission start");
    g_diagPackagesAt = GetTickCount64() + 20000;
}

static void DiagListener(HfMod* mod, const HfEventInfo* event, void* user)
{
    (void)mod;
    (void)user;
    switch (event->kind)
    {
    case HF_ON_STRATAGEM: DiagOnStratagem(event); return;
    case HF_ON_SPAWN: DiagOnSpawn(event); return;
    case HF_ON_EXPLOSION: DiagOnExplosion(event); return;
    case HF_ON_MISSION_START: DiagOnMissionStart(); return;
    case HF_ON_MISSION_END:
        CoopDiag("mission end (%s)", DiagRole());
        return;
    }
}

/* The last second's explosions, one line a type. */
static void DiagBursts(void)
{
    for (uint32_t i = 0; i < g_diagBurstCount; i++)
    {
        const char* source = DiagSeenEntity(g_diagBursts[i].source);
        CoopDiag("explosions: type %u x%u, the first at (%.1f, %.1f, %.1f), source %u%s%s%s, owner %u (%s)", g_diagBursts[i].type,
            g_diagBursts[i].count, (double)g_diagBursts[i].first[0], (double)g_diagBursts[i].first[1], (double)g_diagBursts[i].first[2],
            g_diagBursts[i].source, source ? " (" : "", source ? source : "", source ? ")" : "", g_diagBursts[i].owner, DiagRole());
    }
    g_diagBurstCount = 0;
}

/* The game's host, when it changed. */
static void DiagHost(void)
{
    uintptr_t peers = 0;
    uint64_t host = 0, local = 0;
    if (DiagGame() != 0 && !SafeRead(DiagGame() + DIAG_PEERS, &peers, 8) && peers != 0 && !SafeRead(peers + DIAG_HOST_PEER, &host, 8) &&
        !SafeRead(peers + DIAG_LOCAL_PEER, &local, 8) && host != g_diagHost)
    {
        /* 0 is this machine, as the game reads it (0x108D1CD) */
        CoopDiag("host: the game's host is now %016llx%s (this machine %016llx, %s)", (unsigned long long)host,
            host == 0 || host == local ? ": THIS machine" : "", (unsigned long long)local, DiagRole());
        g_diagHost = host;
    }
}

/* The barrages spawned, a frame later (once their state applied). */
static void DiagBarrages(void)
{
    for (uint32_t i = 0; i < DIAG_BARRAGES; i++)
        if (g_diagBarrages[i].entity != 0 && g_diagFrame > g_diagBarrages[i].update + 1)
        {
            DiagBarrageBlock(g_diagBarrages[i].entity, g_diagBarrages[i].name, g_diagBarrages[i].authority);
            g_diagBarrages[i].entity = 0;
        }
}

/* The followed missiles, each 250 ms for 15 s. */
static void DiagMissiles(ULONGLONG now)
{
    for (uint32_t i = 0; i < DIAG_MISSILES; i++)
    {
        if (g_diagMissiles[i].missile == 0 || now < g_diagMissiles[i].next) continue;
        if (now - g_diagMissiles[i].since > 15000) { CoopDiag("silo: missile %u followed for 15 s", g_diagMissiles[i].missile); g_diagMissiles[i].missile = 0; continue; }
        g_diagMissiles[i].next = now + 250;
        double seconds = (double)(now - g_diagMissiles[i].since) / 1000.0;
        int here = DiagMissileSample(g_diagMissiles[i].missile, seconds, g_diagMissiles[i].how);
        if (!here && g_diagMissiles[i].lastHere != 0)
            CoopDiag("silo: missile %u (%s) %.2f s: GONE from this machine (%s)", g_diagMissiles[i].missile, g_diagMissiles[i].how, seconds, DiagRole());
        g_diagMissiles[i].lastHere = here;
    }
}

/* Every frame on the game thread: the second's explosions, the packages 20 s into a mission, the followed missiles. */
static void DiagTick(HfMod* mod, float dt, void* user)
{
    (void)mod;
    (void)dt;
    (void)user;
    ULONGLONG now = GetTickCount64();
    if (now / 1000 != g_diagBurstSecond)
    {
        g_diagBurstSecond = now / 1000;
        DiagBursts();
        DiagHost();
        ReaperSample(CoopDiag);
    }
    if (g_diagPackagesAt != 0 && now >= g_diagPackagesAt) { g_diagPackagesAt = 0; DiagPackages("20 s into the mission"); }
    g_diagFrame++;
    DiagBarrages();
    if (!g_launchReady) return;
    DiagMissiles(now);
}

/* Once the mods are set up: the log, and the listeners (the event sources hook only kinds someone listens to). */
static void CoopDiagStart(void)
{
    if (!g_logs[0]) return;
    g_diagMod.api = &g_api;
    snprintf(g_diagMod.id, sizeof g_diagMod.id, "hellforge.coop-diagnostics");
    g_diagMod.log = OpenLog(L"coop-diagnostics.log");
    g_diagMod.loaded = 1;
    if (g_diagMod.log == INVALID_HANDLE_VALUE) return;
    g_diagOn = 1;
    CoopDiag("HellForge co-op diagnostics: compare this file with the other players' (same mission, same minute); what mods "
        "added is listed at the first mission start");
    static const uint32_t kinds[] = {HF_ON_STRATAGEM, HF_ON_SPAWN, HF_ON_EXPLOSION, HF_ON_MISSION_START, HF_ON_MISSION_END};
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) ApiSubscribe(&g_diagMod, kinds[i], DiagListener, NULL, -1000);
    ApiOnGameThread(&g_diagMod, DiagTick, NULL);
}
