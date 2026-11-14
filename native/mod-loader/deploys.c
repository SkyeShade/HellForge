/* Per-player deployable and orbital stats in co-op (docs/multiplayer.md "Deployables in co-op"). Every machine builds its
 * own copy of a deployable (SpawnEntity, game.dll 0xFDC140) and reads its Shield, Health, OrbitalAbility and Bombardment
 * values through a resolver (0x4FB390, 0x507920, 0x512E20, 0x5043C0) from its own type record, every frame for some, so a
 * friend's relay or Orbital Laser showed this machine's edits and not theirs. The machine that throws a deployable has
 * authority over it and reads the thrower's edits already; the work is on the others. Research:
 * HD2Mods\research\per-player-deployables. Included by hellforge_modloader.c after rounds.c (RoundBase).
 *
 * The lobby's players' edits of those records come from the session's backend (live.c "deploys", AssetShare). At the
 * post-create pass (0x581780: the network's first state has landed, nothing has been built from the records yet) an
 * entity this machine has no authority over, of a type anyone in the lobby edited, gets a private copy of each edited
 * record: this machine's, with this machine's own changes undone, then the values of the player who made it (the creator
 * peer, LoadoutPackage +0xC0, which the game replicates). Each resolver returns that copy for that entity. Nothing goes
 * on the game's own wire, so two players' relays differ on every screen at once, and a machine without this sees stock.
 * A copy lasts until its entity's id comes back, the lobby's list ends, or DEPLOY_COPIES of its kind are newer. */

#define DEPLOY_POST 0x581780u
#define DEPLOY_LOADOUT 0x3326C40u  /* the LoadoutPackage manager: a pointer to it */
#define DEPLOY_ENTRIES 512
#define DEPLOY_COPIES 64           /* of each kind */
#define DEPLOY_RESOLVER_PIN 19u    /* the part the four resolvers have in common */
static const uint8_t kDeployPostPin[17] = {0x40, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05};
static const uint8_t kDeployResolverPin[DEPLOY_RESOLVER_PIN] = {0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0xD1, 0x48, 0x85, 0xC9, 0x75, 0x07, 0x33, 0xC0,
    0x48, 0x83, 0xC4, 0x28, 0xC3};

typedef void* (*DeployResolveFn)(void* descriptor);
typedef uint64_t (*DeployPostFn)(void* world, uint8_t* row, uint8_t* info, void* extra);
enum { DEPLOY_SHIELD, DEPLOY_HEALTH, DEPLOY_ORBITAL, DEPLOY_BOMBARDMENT, DEPLOY_KINDS };
typedef struct DeployEntry { uint64_t peer, resource; int kind; uint32_t offset, bits; } DeployEntry;
/* entity 0: free, or being written */
typedef struct DeployCopy { volatile LONG entity; uint64_t resource; uint8_t* record; } DeployCopy;
typedef struct DeployKind
{
    char tag;
    const char* what;
    uint32_t rva, size;
    void* detour;
    DeployResolveFn original;
    DeployCopy copies[DEPLOY_COPIES];
    int next;                      /* the slot the next copy takes */
    volatile LONG live;            /* copies to look through */
} DeployKind;

static void* DeployShield(void* descriptor);
static void* DeployHealth(void* descriptor);
static void* DeployOrbital(void* descriptor);
static void* DeployBombardment(void* descriptor);
static DeployKind g_deployKinds[DEPLOY_KINDS] = {
    {'S', "the Shield records", 0x4FB390u, 344u, (void*)DeployShield, NULL, {{0}}, 0, 0},
    {'H', "the Health records", 0x507920u, ROUND_BASE_BYTES, (void*)DeployHealth, NULL, {{0}}, 0, 0},
    {'O', "the OrbitalAbility records", 0x512E20u, 552u, (void*)DeployOrbital, NULL, {{0}}, 0, 0},
    {'B', "the Bombardment records", 0x5043C0u, 192u, (void*)DeployBombardment, NULL, {{0}}, 0, 0},
};
static DeployEntry g_deployEntries[DEPLOY_ENTRIES];
static int g_deployEntryCount;
static SRWLOCK g_deployLock = SRWLOCK_INIT; /* the entries */
static int g_deployState;                   /* 0 not installed yet, 1 installed, -1 refused */
static ULONGLONG g_deployFirstTry;
static DeployPostFn g_deployPost;
static volatile LONG g_deployMade;          /* copies made, for the log */

/* The creator peer on an entity (0x9C4B00's lookup: open addressing on (i + mul * key) & (capacity - 1), u32 key, u32 row,
 * then the rows' u64 at +0xC0), by its id or its network id; 0 none. */
static uint64_t DeployCreatorReal(uint32_t key)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL), manager = 0, keys = 0, rows = 0;
    uint32_t capacity = 0, empty = 0, mul = 0;
    if (game == 0 || SafeRead(game + DEPLOY_LOADOUT, &manager, 8) || manager == 0) return 0;
    if (SafeRead(manager + 0xA0, &keys, 8) || SafeRead(manager + 0xA8, &capacity, 4) || SafeRead(manager + 0xAC, &empty, 4) ||
        SafeRead(manager + 0xB0, &mul, 4) || SafeRead(manager + 0xC0, &rows, 8) || capacity == 0) return 0;
    uint32_t mixed = mul * key;
    for (uint32_t i = 0; i < capacity; i++)
    {
        uint64_t slot = (uint64_t)((i + mixed) & (capacity - 1));
        uint32_t found = 0, row = 0;
        uint64_t peer = 0;
        if (SafeRead(keys + slot * 8, &found, 4) || found == empty) return 0;
        if (found != key) continue;
        if (SafeRead(keys + slot * 8 + 4, &row, 4) || SafeRead(rows + (uint64_t)row * 8, &peer, 8)) return 0;
        return peer;
    }
    return 0;
}

static uint64_t DeployLocalReal(void)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL), user = 0;
    uint64_t peer = 0;
    if (game != 0 && !SafeRead(game + EVT_LOCAL_PEER, &user, 8) && user != 0) SafeRead(user + EVT_LOCAL_PEER_AT, &peer, 8);
    return peer;
}

static uint64_t (*g_deployCreator)(uint32_t key) = DeployCreatorReal;
static uint64_t (*g_deployLocal)(void) = DeployLocalReal;

/* live.c "deploys": the lobby's players' edits of these records, "<peer hex>=<entry>;<entry>..." comma separated, an entry
 * "E<resource hex>/<S|H|O|B>+<offset hex>=<bits hex>", four bytes each; "-" none (solo, or a host sharing its stats).
 * Replaces the last. 0 with *why when malformed. */
static int DeployEntryAt(const char** at, uint64_t peer, DeployEntry* out, const char** why)
{
    char* end;
    const char* p = *at;
    if (*p != 'E') { *why = "malformed entry"; return 0; }
    unsigned long long resource = strtoull(p + 1, &end, 16);
    if (end == p + 1 || *end != '/' || resource == 0) { *why = "malformed entry"; return 0; }
    int kind = -1;
    for (int k = 0; k < DEPLOY_KINDS; k++) if (end[1] == g_deployKinds[k].tag) kind = k;
    if (kind < 0 || end[2] != '+') { *why = "malformed entry"; return 0; }
    const char* o = end + 3;
    unsigned long offset = strtoul(o, &end, 16);
    if (end == o || *end != '=') { *why = "malformed entry"; return 0; }
    const char* b = end + 1;
    unsigned long long bits = strtoull(b, &end, 16);
    if (end == b || bits > 0xFFFFFFFFull) { *why = "malformed entry"; return 0; }
    if (offset % 4 || offset + 4 > g_deployKinds[kind].size) { *why = "an entry names no field of its record"; return 0; }
    *out = (DeployEntry){peer, resource, kind, (uint32_t)offset, (uint32_t)bits};
    *at = end;
    return 1;
}

static int DeployGroupAt(const char** at, DeployEntry* entries, int* count, const char** why)
{
    char* end;
    uint64_t peer = strtoull(*at, &end, 16);
    if (end == *at || *end != '=' || peer == 0) { *why = "malformed list"; return 0; }
    *at = end; /* each entry follows an '=' or a ';' */
    do
    {
        (*at)++;
        if (*count == DEPLOY_ENTRIES) { *why = "more changes than HellForge keeps"; return 0; }
        if (!DeployEntryAt(at, peer, &entries[(*count)++], why)) return 0;
    } while (**at == ';');
    return 1;
}

/* Every copy gone: the lobby's values no longer hold. */
static void DeployForget(void)
{
    for (int k = 0; k < DEPLOY_KINDS; k++)
    {
        DeployKind* kind = &g_deployKinds[k];
        InterlockedExchange(&kind->live, 0);
        for (int i = 0; i < DEPLOY_COPIES; i++) InterlockedExchange(&kind->copies[i].entity, 0);
    }
}

static int DeploysSetLobby(const char* list, const char** why)
{
    static DeployEntry entries[DEPLOY_ENTRIES]; /* the pipe's thread only */
    int count = 0;
    for (const char* at = list; strcmp(list, "-") && *at;)
    {
        if (!DeployGroupAt(&at, entries, &count, why)) return 0;
        if (*at != ',' && *at != 0) { *why = "malformed list"; return 0; }
        if (*at == ',') at++;
    }
    AcquireSRWLockExclusive(&g_deployLock);
    memcpy(g_deployEntries, entries, sizeof entries[0] * (size_t)count);
    g_deployEntryCount = count;
    ReleaseSRWLockExclusive(&g_deployLock);
    if (count == 0) DeployForget();
    Note("deploys: %d deployable change(s) of the lobby's players%s", count,
        count > 0 && g_deployState != 1 ? " (not applied yet: the records are not hooked)" : "");
    return 1;
}

/* A kind's copy for this entity and resource, or NULL. The game's threads, every read of a record: no lock. */
static void* DeployCopyOf(const DeployKind* kind, const uint8_t* descriptor)
{
    uint32_t entity;
    uint64_t resource;
    memcpy(&resource, descriptor, 8);
    memcpy(&entity, descriptor + 8, 4);
    for (int i = 0; i < DEPLOY_COPIES; i++)
        if ((uint32_t)kind->copies[i].entity == entity && kind->copies[i].resource == resource) return kind->copies[i].record;
    return NULL;
}

static void* DeployResolve(int k, void* descriptor)
{
    DeployKind* kind = &g_deployKinds[k];
    if (kind->live && descriptor != NULL)
    {
        void* copy = DeployCopyOf(kind, descriptor);
        if (copy != NULL) return copy;
    }
    return kind->original(descriptor);
}
static void* DeployShield(void* descriptor) { return DeployResolve(DEPLOY_SHIELD, descriptor); }
static void* DeployHealth(void* descriptor) { return DeployResolve(DEPLOY_HEALTH, descriptor); }
static void* DeployOrbital(void* descriptor) { return DeployResolve(DEPLOY_ORBITAL, descriptor); }
static void* DeployBombardment(void* descriptor) { return DeployResolve(DEPLOY_BOMBARDMENT, descriptor); }

/* The copy of this kind for an entity the owner made: this machine's record with its own changes undone, then the owner's
 * values. The game's thread that spawns. */
static void DeployMake(DeployKind* kind, int k, uint64_t resource, uint32_t entity, uint64_t owner)
{
    uint8_t descriptor[16] = {0};
    memcpy(descriptor, &resource, 8);
    memcpy(descriptor + 8, &entity, 4);
    uint8_t* mine = kind->original(descriptor);
    if (mine == NULL) return;
    DeployCopy* slot = &kind->copies[kind->next];
    kind->next = (kind->next + 1) % DEPLOY_COPIES;
    if (slot->record == NULL) slot->record = malloc(kind->size);
    if (slot->record == NULL) return;
    InterlockedExchange(&slot->entity, 0);
    memcpy(slot->record, mine, kind->size);
    RoundBase((uintptr_t)mine, slot->record, kind->size);
    AcquireSRWLockShared(&g_deployLock);
    for (int i = 0; i < g_deployEntryCount; i++)
    {
        const DeployEntry* e = &g_deployEntries[i];
        if (e->peer == owner && e->kind == k && e->resource == resource) memcpy(slot->record + e->offset, &e->bits, 4);
    }
    ReleaseSRWLockShared(&g_deployLock);
    slot->resource = resource;
    InterlockedExchange(&slot->entity, (LONG)entity);
    InterlockedExchange(&kind->live, 1);
    InterlockedIncrement(&g_deployMade);
}

/* The post-create pass begins for an entity: its id may be one a dead entity had, and it may need copies. */
static void DeployEntity(const uint8_t* row, const uint8_t* info)
{
    uint64_t resource;
    uint32_t entity, network;
    memcpy(&resource, row, 8);
    memcpy(&entity, row + 8, 4);
    memcpy(&network, row + 0x10, 4);
    if (entity == 0) return;
    int touched[DEPLOY_KINDS] = {0}, any = 0;
    AcquireSRWLockShared(&g_deployLock);
    for (int i = 0; i < g_deployEntryCount; i++)
        if (g_deployEntries[i].resource == resource) touched[g_deployEntries[i].kind] = any = 1;
    ReleaseSRWLockShared(&g_deployLock);
    for (int k = 0; k < DEPLOY_KINDS; k++)
        if (g_deployKinds[k].live)
            for (int i = 0; i < DEPLOY_COPIES; i++)
                if ((uint32_t)g_deployKinds[k].copies[i].entity == entity) InterlockedExchange(&g_deployKinds[k].copies[i].entity, 0);
    if (!any || info[0]) return; /* this machine has authority: it reads its own records */
    uint64_t owner = g_deployCreator(entity), local = g_deployLocal();
    if (owner == 0) owner = g_deployCreator(network);
    if (owner == 0 || owner == local) return;
    for (int k = 0; k < DEPLOY_KINDS; k++) if (touched[k]) DeployMake(&g_deployKinds[k], k, resource, entity, owner);
}

static uint64_t DeployPostDetour(void* world, uint8_t* row, uint8_t* info, void* extra)
{
    if (g_deployEntryCount > 0 && row != NULL && info != NULL) DeployEntity(row, info);
    return g_deployPost(world, row, info, extra);
}

/* The watch loop, after the game loads: the hooks once the lobby has changes and their bytes read as reviewed (60 s for
 * code still being decoded). A session with no such change never pays for them. */
static void DeploysTick(void)
{
    static ULONGLONG next;
    static LONG madeNoted;
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 1000;
    LONG made = g_deployMade;
    if (made != madeNoted) { Note("deploys: %ld record(s) of other players' deployables made here so far", (long)made); madeNoted = made; }
    if (g_deployState != 0 || g_deployEntryCount == 0) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0) return;
    if (!g_deployFirstTry) g_deployFirstTry = now;
    if (strcmp(g_build, "25480438")) { g_deployState = -1; Note("deploys: not hooked (unsupported build)"); return; }
    int reviewed = PinMatches(game + DEPLOY_POST, kDeployPostPin, sizeof kDeployPostPin);
    for (int k = 0; k < DEPLOY_KINDS; k++) reviewed = reviewed && PinMatches(game + g_deployKinds[k].rva, kDeployResolverPin, DEPLOY_RESOLVER_PIN);
    if (!reviewed)
    {
        if (now - g_deployFirstTry < 60000) return;
        g_deployState = -1;
        Note("deploys: the records' code is not the reviewed one; friends' deployables keep this machine's values");
        return;
    }
    for (int k = 0; k < DEPLOY_KINDS; k++)
        if (LoaderHook(g_deployKinds[k].what, game + g_deployKinds[k].rva, g_deployKinds[k].detour, (void**)&g_deployKinds[k].original))
        {
            g_deployState = -1;
            Note("deploys: could not hook %s", g_deployKinds[k].what);
            return;
        }
    if (LoaderHook("the post-create pass", game + DEPLOY_POST, (void*)DeployPostDetour, (void**)&g_deployPost))
    {
        g_deployState = -1;
        Note("deploys: could not hook the post-create pass");
        return;
    }
    g_deployState = 1;
    Note("deploys: hooked: in co-op each player's deployables and orbitals carry their own values on every machine");
}
