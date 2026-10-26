/* New abilities (SDK 7, add_ability): ability ids of their own (2868 and up) that the game's ability system runs like its
 * own. An entity picks one by data, where it names its vanilla one (ExplosiveComponentData.armed_ability: the ability
 * an explosive runs once it is set off). Included by hellforge_modloader.c after behaviours.c (its stub helpers) and
 * packages.c (load the sounds' package).
 *
 * The game's abilities (build 25480438, game.dll 2E2C3B7C...):
 *   the tick dispatcher 0x11509E0 (ctx, id, tick) -> al: 1 when the ability is done, 0 while it runs. It begins
 *   `sub rsp, 0x28; xor r9b, r9b; dec edx; cmp edx, 0xB32; ja 0x115C779` and switches on the id through a 2867-entry
 *   table of rvas (+0x115C784); each case is `mov edx, r8d; call <the ability>; movzx r9d, al; add rsp, 0x28; ret` (2088:
 *   0x11593C3, 41 8B D0 E8 ...), and
 *   the default (0x115C779) returns r9b (0: an unknown id never finishes). ctx +8 is the entity it runs on.
 *   The ability manager ticks it at 60 a second from 0, every tick in turn (0x7C894C: floor(time x 60), 0x23C7740).
 *   2088 (0x110F270), the Solo Silo missile's armed ability, set off by its targeting remote: tick 0 the launch (its
 *   launch blast, explosion 172), tick 2 it leaves the rack, tick 120 it arms and is done.
 * HellForge points that `ja` at a stub in a block near game.dll: HellForge's ids go to its code, every other id to the
 * default, so vanilla abilities run as before. A HellForge ability holds hold_seconds (posting its sounds), then runs its
 * base with the tick less the hold: the base runs exactly as if it had started then.
 * Its sounds play on the world's default source (+0x4148 of the WwiseWorld, [[game + 0x3326340] + 0x10F8]), posted through
 * the Wwise plugin's API table ([game + 0x3326318] +0x338) as the in-game editor's menu sounds are. A sound whose bank is
 * not loaded plays nothing: HfAbility.package loads it with the session.
 * Live (2026-10-18): the Solo Silo missile's armed ability is not what launches it. The missile left the silo at once;
 * 2088 runs while it flies (it arms at tick 120), so holding it left the missile unarmed: it never came down.
 *
 * A silo countdown (HF_ABILITY_HOLDS_LINKED_LAUNCH) holds the launch itself. A silo's targeting remote is linked to its
 * missile when it is picked up (0x76AAB0: the rack's items in the SeekingMissile manager), and its laser launches the
 * missile on the first frame it marks a point: the laser update (0x9BF6D0) calls the remote's link (0x76B580) every
 * frame, which runs only while the remote's linker record says a silo link (+4 0x06212002): it launches the missile
 * when it is not yet launched (its SeekingMissile instance +0x36: 0x645B80 its engine, 0x645940 its explosive set off,
 * which starts 2088), names the explosive's owner (0x8CE620), sets the missile's guidance target to the laser's point
 * (0x919940) and has the rack let go of it (0x9374D0). Fired, the copy of the remote runs this ability (its
 * AbilityWeaponComponentData fire modes): it marks its own linker record (the linker manager's copy, 0x4FD6F0) as no
 * link, so its laser launches nothing, and starts the same ability on the missile (StartAbility 0x7CAF40, as the
 * linker does its cleanup). There it holds hold_seconds, keeping the point the laser marks, then does what the link
 * does, in its order, and gives the remote its link back: a laser still on then guides the missile as the silo's does.
 * The marked record also keeps the remote from its cleanup (2089, which removes it once it is dropped and its rack is
 * gone: 0x76BC70 runs only for a silo link), so a tossed remote stays until the launch. When the hold ends the
 * missile's explosive starts 2088 on it in place of this ability; the ability manager leaves an ability started
 * meanwhile running (0x7C89BE). If the laser launched the missile before the remote's fire ran, there is no
 * countdown: the log says so.
 * A loose remote (co-op, 2026-10-18): the SeekingMissile instance names its remote (+8, its network handle, written at
 * pickup by 0x76AAB0), and when the missile goes off the game takes the remote out of its holder's hands (0x642570, on
 * the machine with authority over the remote): it looks the remote up among the attachables ([game + 0x3326698], map
 * +0x18, records of 0xAC) and reads the record without checking it was found. A remote is always in someone's hands in
 * the game's own play; a remote fired here is tossed and kept until the launch, so it can be alive and attached to
 * nothing when the missile lands, and the game reads record -1 (game.dll+0x64271d, a host whose friend fired one).
 * Engine 0.1.21 guards the read (InstallLooseGuard): not found, the game leaves the remote alone, as it does for a
 * missile naming no remote. (Engine 0.1.2 to 0.1.20 cleared the missile's +8 instead; once another machine's state
 * reaches its copies (netapply.c) the missile's network block is copied in again on every update, so that no longer
 * held where the missile is not this machine's.)
 * Co-op (engine 0.1.21, co-op fix 2): as the game's own silo, only the machine
 * with authority over the missile launches it; the launch reaches the others with the missile's state (its engine
 * byte +0x35 and launched byte +0x36, their flight gated on the received engine byte, 0x6434A3). */

#define ABL_FIRST 2868u
#define ABL_MAX 8u
_Static_assert(ABL_FIRST + ABL_MAX <= 4096u, "ability ids travel in 12 bits (StartAbility's RPC, co-op research 2026-10-21)");
#define ABL_DISPATCH 0x11509E0u
#define ABL_JA 0x11509EFu
#define ABL_DEFAULT 0x115C779u
#define ABL_TABLE 0x115C784u
#define ABL_VANILLA 2867u
#define ABL_TICKS 60u                 /* ticks a second */
#define ABL_SOUND_API 0x3326318u      /* the Wwise plugin's API table */
#define ABL_GAME 0x3326340u           /* the game context: +0x10F8 its WwiseWorld */
#define ABL_WWISE_WORLD 0x10F8u
#define ABL_DEFAULT_SOURCE 0x4148u
#define ABL_API_POST 0x338u
#define ABL_API_SOURCE 0x590u         /* the Wwise plugin's API: a source on a unit's node */
#define ABL_API_OBJECT 0x5A0u         /* ... and a source's game object (-1: gone) */
#define ABL_UNIT_OF 0x748u            /* the scene graph's unit for an index */
#define ABL_ABILITIES 0x3326640u      /* the ability manager (StartAbility's first argument): map +0x18 */
#define ABL_LINKERS 0x33267F8u        /* WeaponLinker manager: map +0x20; +0x38 instance descriptors; its records' copies
                                       * (cache map +0x58, records of 12 at +0x98) */
#define ABL_MISSILES 0x3326DF0u       /* SeekingMissile manager: map +0x20; +0x50 instances of 0x38 (+0x36 launched) */
#define ABL_LASERS 0x3326960u         /* LaserDesignator manager: map +0x30; +0x58 instances of 0x38 (+0x2C the marked
                                       * point); +0x60 a byte each, the laser on */
#define ABL_GUIDANCE 0x3326BD8u       /* guidance targets: map +0x840 */
#define ABL_EXPLOSIVES 0x3326728u     /* Explosive manager: map +0x38 */
#define ABL_OWNERS 0x3326DC0u         /* the remote's owner: map +0x20; +0x40 records of 48 (+4 the owner) */
#define ABL_RACKS 0x3326A58u          /* HellpodRack manager: map +0x28 */
#define ABL_NO_ENTITY 0x3483C34u      /* u32: the entity id the managers' maps leave empty */
#define ABL_SILO_LINK 0x06212002u     /* WeaponLinker record +4: a silo remote's link, which launches its missile */
#define ABL_ATTACHABLES 0x3326698u    /* attachables: map +0x18 (records of 0xAC, +0 the holder's link) */
#define ABL_NO_HANDLE 0x7FFFu         /* the network handle of no entity */
#define ABL_HANDLE_NONE 0x348456Cu    /* u32: the entity a handle of none resolves to (0xFD9BA0) */
#define ABL_MAX_HELD 8u

static const uint8_t kAbilityDispatchPin[21] = {0x48, 0x83, 0xec, 0x28, 0x45, 0x32, 0xc9, 0xff, 0xca, 0x81, 0xfa, 0x32, 0x0b, 0x00, 0x00,
    0x0f, 0x87, 0x84, 0xbd, 0x00, 0x00};
static const uint8_t kAbilityDefaultPin[8] = {0x41, 0x0f, 0xb6, 0xc1, 0x48, 0x83, 0xc4, 0x28};

typedef uint8_t (*AbilityFn)(void* ctx, uint32_t tick);
typedef uint32_t* (*LinkFn)(void* linkers, uint32_t* out, uint32_t remote);
typedef uint8_t* (*LinkerRecordFn)(void* descriptor);
typedef void (*EngineFn)(void* missiles, uint32_t missile, uint8_t on);
typedef void (*MissileFn)(void* missiles, uint32_t missile);
typedef void (*OwnerFn)(void* explosives, uint32_t missile, uint32_t owner);
typedef void (*GuideFn)(void* unused, uint32_t entity, const float point[3]);
typedef void (*ReleaseFn)(void* racks, uint32_t rack, uint32_t item);
typedef void (*StartAbilityFn)(void* abilities, uint32_t entity, uint32_t ability, uint32_t mode, float speed);
typedef uint32_t* (*HandleEntityFn)(uint32_t* out, uint32_t handle);

/* What a silo countdown calls (the remote's link's own calls, 0x76B580), checked before it is used. */
enum { L_LINKED, L_RECORD, L_RACK, L_ENGINE, L_LAUNCH, L_OWNER, L_GUIDE, L_RELEASE, L_START, L_HANDLE, L_COUNT };
static const GameFunction kLaunchFunctions[L_COUNT] = {
    {0x76B410U, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x05, 0x04, 0x88, 0xd1, 0x02}}, /* the missile a remote is linked to */
    {0x4FD6F0U, {0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0xd1, 0x48, 0x85, 0xc9, 0x75, 0x07, 0x33, 0xc0, 0x48, 0x83, 0xc4, 0x28, 0xc3, 0x8b, 0x41, 0x08, 0x3b, 0x05, 0x18, 0x65, 0xf8, 0x02, 0x0f, 0x84, 0x94, 0x00}}, /* a linker instance record */
    {0x76B4C0U, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x05, 0x54, 0x87, 0xd1, 0x02}}, /* the rack a remote is linked to */
    {0x645B80U, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x50, 0x3b, 0x15}}, /* a missile engine on or off */
    {0x645940U, {0x40, 0x56, 0x41, 0x56, 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x05, 0xc1, 0x66, 0xff, 0x01, 0x48, 0x33, 0xc4, 0x48, 0x89, 0x44, 0x24, 0x30, 0x8b, 0x05, 0xc3, 0xe2, 0xe3, 0x02, 0x45, 0x33, 0xd2}}, /* a missile launched */
    {0x8CE620U, {0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x20, 0x3b, 0x15, 0xf2, 0x55, 0xbb, 0x02, 0x41, 0x8b, 0xf8, 0x8b, 0xda, 0x48, 0x8b, 0xf1, 0x0f, 0x84, 0xaa, 0x01, 0x00, 0x00, 0x44, 0x8b, 0x41, 0x40}}, /* an explosive owner */
    {0x919940U, {0x40, 0x55, 0x3b, 0x15, 0xec, 0xa2, 0xb6, 0x02, 0x49, 0x8b, 0xe8, 0x4c, 0x8b, 0x15, 0x86, 0xd2, 0xa0, 0x02, 0x75, 0x07, 0xb8, 0xff, 0xff, 0xff, 0xff, 0xeb, 0x75, 0x45, 0x8b, 0x8a, 0x48, 0x08}}, /* an entity guidance target */
    {0x9374D0U, {0x40, 0x53, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x40, 0x3b, 0x15, 0x55, 0xc7, 0xb4, 0x02, 0x41, 0x8b, 0xf8, 0x8b, 0xda, 0x4c, 0x8b, 0xf1, 0x0f, 0x84, 0x27, 0x01, 0x00, 0x00, 0x44, 0x8b, 0x41}}, /* a rack lets go of an item */
    {0x7CAF40U, {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x18, 0x55, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xec, 0xb0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x05, 0xb4, 0x10, 0xe7, 0x01, 0x48, 0x33, 0xc4, 0x48}}, /* StartAbility */
    {0xFD9BA0U, {0x48, 0x83, 0xec, 0x08, 0x4c, 0x8b, 0x1d, 0xed, 0x23, 0x49, 0x02, 0x44, 0x8b, 0xc2, 0x4c, 0x8b, 0xc9, 0x81, 0xfa, 0xff, 0x7f, 0x00, 0x00, 0x75, 0x10, 0x8b, 0x05, 0xad, 0xa9, 0x4a, 0x02, 0x89}}, /* the entity of a network handle */
};
typedef uint64_t (*SoundPostFn)(void* world, uint32_t event, void* object, uint64_t flags);
typedef uint64_t (*UnitOfFn)(uint32_t index);
typedef uint32_t (*SourceFn)(void* world, uint64_t unit, uint32_t node);
typedef void* (*SourceObjectFn)(void* world, uint32_t source);

typedef struct Ability Ability;
struct Ability
{
    HfMod* mod;
    uint32_t id, base;
    AbilityFn baseFn;          /* the base's function, once installed */
    uint32_t holdTicks;
    uint32_t startEvent, secondEvent, finalEvent;
    uint64_t package;
    int holdsLaunch;           /* HF_ABILITY_HOLDS_LINKED_LAUNCH */
};

/* A silo countdown under way on this machine: its remote fired here, or its start reached this machine with the missile. */
typedef struct HeldLaunch
{
    uint32_t remote, missile, rack, owner;
    int linkMarked;            /* this countdown marked the remote's link "no link" (MarkLink) */
    uint8_t* sharedRecord;     /* that mark's record when it is the table's (no copy for this remote here); a copy is
                                * looked up again each time (LinkRecord) */
    int started, marked;       /* the missile's countdown ran; the laser marked a point */
    int shadow;                /* the remote was not fired here (ShadowFor) */
    int ended;                 /* its last tick ran here without authority: waiting for the launch to arrive (SiloFrame) */
    const struct Ability* ability;
    ULONGLONG since;           /* when the countdown started here */
    uint32_t holdMs;           /* its hold */
    float point[3];
} HeldLaunch;

static HeldLaunch g_heldLaunches[ABL_MAX_HELD];
/* The table's linker records countdowns here marked "no link", and how many: the last one gives the link back. */
static struct { uint8_t* record; uint32_t count; } g_sharedMarks[ABL_MAX_HELD];
static int g_launchReady;      /* the countdown's game functions are the reviewed ones */
static int g_looseGuard;       /* the missile pre-destroy's guard (0x64270B) is installed */

static Ability g_abilities[ABL_MAX];
static uint32_t g_abilityCount;
static int g_abilityState; /* 0 waiting, 1 installed, -1 refused */
static uintptr_t g_ablGame;
static ULONGLONG g_abilityFirstTry;

static uint32_t ApiAddAbility(HfMod* mod, const HfAbility* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfAbility, package) + sizeof def->package)
    {
        ApiLog(mod, "add_ability: no ability, or its size is not set (HfAbility a = {sizeof a})");
        return 0;
    }
    if (g_fired[HF_EVENT_GAME_LOADED] || g_abilityState != 0) { ApiLog(mod, "add_ability: too late; call it from hellforge_mod_init"); return 0; }
    if (g_abilityCount >= ABL_MAX) { ApiLog(mod, "add_ability: mods added %u abilities already", ABL_MAX); return 0; }
    if (def->base > ABL_VANILLA) { ApiLog(mod, "add_ability: base %u is not a vanilla ability (1 .. %u, or 0 for none)", def->base, ABL_VANILLA); return 0; }
    if (!(def->hold_seconds >= 0.0f && def->hold_seconds <= 600.0f)) { ApiLog(mod, "add_ability: hold_seconds must be 0 to 600"); return 0; }
    uint32_t flags = def->size >= offsetof(HfAbility, flags) + sizeof def->flags ? def->flags : 0;
    if (flags & ~HF_ABILITY_HOLDS_LINKED_LAUNCH) { ApiLog(mod, "add_ability: unknown flags %x", flags & ~HF_ABILITY_HOLDS_LINKED_LAUNCH); return 0; }
    if ((flags & HF_ABILITY_HOLDS_LINKED_LAUNCH) && def->base != 0) { ApiLog(mod, "add_ability: a silo countdown has no base"); return 0; }
    Ability* a = &g_abilities[g_abilityCount];
    *a = (Ability){mod, ABL_FIRST + g_abilityCount, def->base, NULL, (uint32_t)(def->hold_seconds * ABL_TICKS + 0.5f),
        def->start_event, def->second_event, def->final_event, def->package, (flags & HF_ABILITY_HOLDS_LINKED_LAUNCH) != 0};
    g_abilityCount++;
    if (a->holdsLaunch)
        ApiLog(mod, "adding ability %u: a silo countdown, %.1f s before the missile launches (sounds %08x, %08x each second, %08x)",
            a->id, (double)def->hold_seconds, a->startEvent, a->secondEvent, a->finalEvent);
    else
        ApiLog(mod, "adding ability %u: ability %u after a %.1f s hold (sounds %08x, %08x each second, %08x)", a->id, a->base,
            (double)def->hold_seconds, a->startEvent, a->secondEvent, a->finalEvent);
    return a->id;
}

/* The game object of the source on the entity's unit's root node; NULL when it has no unit or its source is gone. */
static void* AbilityUnitObject(uintptr_t game, uintptr_t api, uintptr_t world, uint32_t entity)
{
    uintptr_t sourceFn = 0, objectFn = 0, engine = 0, graph = 0, unitOf = 0;
    void* object = NULL;
    const uint8_t* descriptor = entity != 0 ? ((const uint8_t* (*)(uint32_t))BhvFn(F_DESCRIPTOR))(entity) : NULL;
    uint32_t index = 0, flags = 0;
    if (descriptor != NULL && !SafeRead((uintptr_t)descriptor + 0xC, &index, 4) && !SafeRead((uintptr_t)descriptor + 0x14, &flags, 4) &&
        (flags & 1) && !SafeRead(api + ABL_API_SOURCE, &sourceFn, 8) && sourceFn != 0 && !SafeRead(api + ABL_API_OBJECT, &objectFn, 8) &&
        objectFn != 0 && !SafeRead(game + BHV_ENGINE_API, &engine, 8) && engine != 0 && !SafeRead(engine + 0x18, &graph, 8) && graph != 0 &&
        !SafeRead(graph + ABL_UNIT_OF, &unitOf, 8) && unitOf != 0)
    {
        uint64_t unit = ((UnitOfFn)unitOf)(index);
        uint32_t source = unit != 0 ? ((SourceFn)sourceFn)((void*)world, unit, 0) : 0xFFFFFFFFu;
        if (source != 0xFFFFFFFFu) object = ((SourceObjectFn)objectFn)((void*)world, source);
        if (object == (void*)(intptr_t)-1) object = NULL;
    }
    return object;
}

/* A sound in the world at the entity, as the game posts a weapon's (0x614A16): the source on the entity's unit's root
 * node (the Wwise plugin's API +0x590: (world, unit, node)) and its game object (+0x5A0: (world, source), -1 when the
 * source is gone), posted with +0x338. Asked for at every post, as the game asks before it posts: nothing is kept
 * across posts, so a source never outlives its unit here. With no unit, on the world's default source (+0x4148: heard
 * everywhere, as the in-game editor's menu sounds are). The unit: the scene graph's ([[game + 0x3326308] + 0x18] +0x748)
 * for the entity descriptor's +0xC. (The helldivers2.exe+0x626296 fault once laid at a unit source's door is the
 * game's own, on quitting.) */
static void AbilitySound(uint32_t entity, uint32_t event)
{
    if (event == 0) return;
    uintptr_t game = g_ablGame, api = 0, context = 0, world = 0, postFn = 0;
    if (SafeRead(game + ABL_SOUND_API, &api, 8) || api == 0 || SafeRead(game + ABL_GAME, &context, 8) || context == 0 ||
        SafeRead(context + ABL_WWISE_WORLD, &world, 8) || world == 0 || SafeRead(api + ABL_API_POST, &postFn, 8) || postFn == 0)
        return;
    void* object = AbilityUnitObject(game, api, world, entity);
    if (object == NULL)
    {
        uint32_t source = 0;
        if (SafeRead(world + ABL_DEFAULT_SOURCE, &source, 4) || source == 0) return;
        object = (void*)(uintptr_t)source;
    }
    ((SoundPostFn)postFn)((void*)world, event, object, 0);
}

static void* LaunchFn(int f) { return (void*)(g_ablGame + kLaunchFunctions[f].rva); }
static uintptr_t AblManager(uint32_t rva) { uintptr_t m = 0; return SafeRead(g_ablGame + rva, &m, 8) ? 0 : m; }
static int64_t AblFind(uintptr_t manager, uint32_t at, uint32_t entity) { return manager != 0 ? BhvMapFind(manager, at, entity) : -1; }
/* A missile on its way: its engine on (+0x35) or launched (+0x36), here or as another machine's state reached it. */
static int MissileFlies(uintptr_t missiles, int64_t seeking)
{
    const uint8_t* instance = *(uint8_t**)(missiles + 0x50) + (size_t)seeking * 0x38;
    return instance[0x35] != 0 || instance[0x36] != 0;
}

static HeldLaunch* HeldFor(uint32_t missile)
{
    for (uint32_t i = 0; i < ABL_MAX_HELD; i++) if (g_heldLaunches[i].missile == missile && missile != 0) return &g_heldLaunches[i];
    return NULL;
}

/* The point the remote's laser marks while it is on (and, until it has marked one, wherever it points). */
static void KeepMarkedPoint(HeldLaunch* h)
{
    uintptr_t lasers = AblManager(ABL_LASERS);
    int64_t laser = AblFind(lasers, 0x30, h->remote);
    if (laser < 0) return;
    uint8_t on = (*(uint8_t**)(lasers + 0x60))[laser];
    if (!on && h->marked) return;
    memcpy(h->point, *(uint8_t**)(lasers + 0x58) + (size_t)laser * 0x38 + 0x2C, 12);
    h->marked |= on != 0;
}

/* The remote's linker record now, looked up again each time: the linker manager keeps a copy per remote this machine has
 * authority over and compacts them when one goes (0x76D280: the last copy moves into the freed slot), so a pointer kept
 * across frames can name another remote's. *shared: the table's record (no copy for this remote here: one this machine
 * has no authority over), which every such remote of its resource shares. */
static uint8_t* LinkRecord(uint32_t remote, int* shared)
{
    uintptr_t linkers = AblManager(ABL_LINKERS);
    int64_t link = AblFind(linkers, 0x20, remote);
    *shared = 0;
    if (link < 0) return NULL;
    uint8_t* descriptor = *(uint8_t**)(*(uint8_t**)(linkers + 0x38) + (size_t)link * 8);
    uint8_t* record = ((LinkerRecordFn)LaunchFn(L_RECORD))(descriptor);
    if (record == NULL) return NULL;
    uint8_t* copies = *(uint8_t**)(linkers + 0x98);
    uint32_t copyCount = *(uint32_t*)(linkers + 0x90); /* the copies (0x76D35A); +0x60 is their map's capacity */
    *shared = !(copies != NULL && record >= copies && record < copies + (size_t)copyCount * 12);
    return record;
}

/* The table's record counted among the shared marks for this countdown: one more on its mark, else a spare mark when it
 * is a silo's link. 0 when it is neither, or no mark is spare. */
static int MarkShared(HeldLaunch* h, uint8_t* record, uint32_t magic)
{
    uint32_t at = ABL_MAX_HELD, spare = ABL_MAX_HELD;
    for (uint32_t i = 0; i < ABL_MAX_HELD; i++)
        if (g_sharedMarks[i].count > 0 && g_sharedMarks[i].record == record) at = i;
        else if (g_sharedMarks[i].count == 0 && spare == ABL_MAX_HELD) spare = i;
    if (at == ABL_MAX_HELD)
    {
        if (magic != ABL_SILO_LINK || spare == ABL_MAX_HELD) return 0;
        at = spare;
        g_sharedMarks[at].record = record;
    }
    g_sharedMarks[at].count++;
    h->sharedRecord = record;
    return 1;
}

/* The remote's link marked "no link" for the countdown (its laser launches nothing meanwhile, here: once the laser-on byte
 * replicates, a machine without the mark would launch its copy at once, setting the explosive off a second time). 1 when
 * marked: a silo's link, or the table's record already marked by another countdown here. */
static int MarkLink(HeldLaunch* h)
{
    int shared;
    uint8_t* record = LinkRecord(h->remote, &shared);
    uint32_t magic = 0;
    if (record == NULL || SafeRead((uintptr_t)record + 4, &magic, 4)) return 0;
    if (shared)
    {
        if (!MarkShared(h, record, magic)) return 0;
    }
    else if (magic != ABL_SILO_LINK) return 0;
    *(uint32_t*)(record + 4) = 0;
    h->linkMarked = 1;
    return 1;
}

/* The remote's link back: its own copy as the manager holds it now; the table's when the last countdown marking it ends. */
static void GiveLinkBack(HeldLaunch* h)
{
    if (!h->linkMarked) return;
    h->linkMarked = 0;
    uint32_t magic = 0;
    if (h->sharedRecord != NULL)
    {
        for (uint32_t i = 0; i < ABL_MAX_HELD; i++)
            if (g_sharedMarks[i].count > 0 && g_sharedMarks[i].record == h->sharedRecord && --g_sharedMarks[i].count == 0)
            {
                if (!SafeRead((uintptr_t)h->sharedRecord + 4, &magic, 4) && magic == 0) *(uint32_t*)(h->sharedRecord + 4) = ABL_SILO_LINK;
                g_sharedMarks[i].record = NULL;
            }
        h->sharedRecord = NULL;
        return;
    }
    int shared;
    uint8_t* record = LinkRecord(h->remote, &shared);
    if (record != NULL && !shared && !SafeRead((uintptr_t)record + 4, &magic, 4) && magic == 0) *(uint32_t*)(record + 4) = ABL_SILO_LINK;
}

/* Whether this machine has authority over an entity now (its entity info +0x14 bit 0, as every component manager reads it). */
static int AblAuthority(uint32_t entity)
{
    const uint8_t* descriptor = entity != 0 ? ((const uint8_t* (*)(uint32_t))BhvFn(F_DESCRIPTOR))(entity) : NULL;
    uint32_t flags = 0;
    return descriptor != NULL && !SafeRead((uintptr_t)descriptor + 0x14, &flags, 4) && (flags & 1) != 0;
}

/* The remote a missile names (its SeekingMissile instance +8, a network handle), or none; *instance its instance. */
static uint32_t MissileRemote(uint32_t missile, uint8_t** instance)
{
    uint32_t handle = 0, none = 0, noneOfHandle = 0;
    *instance = NULL;
    if (SafeRead(g_ablGame + ABL_NO_ENTITY, &none, 4)) return 0;
    uintptr_t missiles = AblManager(ABL_MISSILES);
    int64_t seeking = AblFind(missiles, 0x20, missile);
    if (seeking < 0) return none;
    *instance = *(uint8_t**)(missiles + 0x50) + (size_t)seeking * 0x38;
    if (SafeRead((uintptr_t)*instance + 8, &handle, 4) || handle == ABL_NO_HANDLE) return none;
    /* the handle of none must resolve to what the game's checks take for none */
    if (SafeRead(g_ablGame + ABL_HANDLE_NONE, &noneOfHandle, 4) || none != noneOfHandle) return none;
    uint32_t remote = none;
    ((HandleEntityFn)LaunchFn(L_HANDLE))(&remote, handle);
    return remote;
}

/* A free countdown slot: an unused one, else one whose missile is gone (destroyed or its mission over before the
 * countdown ended, so its end never came), its remote given its link back. */
static HeldLaunch* FreeHeld(void)
{
    for (uint32_t i = 0; i < ABL_MAX_HELD; i++) if (g_heldLaunches[i].missile == 0) return &g_heldLaunches[i];
    uintptr_t missiles = AblManager(ABL_MISSILES);
    for (uint32_t i = 0; i < ABL_MAX_HELD; i++)
        if (AblFind(missiles, 0x20, g_heldLaunches[i].missile) < 0)
        {
            GiveLinkBack(&g_heldLaunches[i]);
            g_heldLaunches[i].missile = 0;
            return &g_heldLaunches[i];
        }
    return NULL;
}

/* Why a fired remote has no missile to hold. */
static const char* NoMissileWhy(int64_t link, uint32_t missile, uint32_t none, int64_t seeking)
{
    return link < 0 ? "no link of its own" : missile == none ? "its link names no missile" : seeking < 0 ? "the missile is not a seeking missile" : "the missile runs no abilities";
}

/* The copy of a silo's remote fired: hold its missile's launch, counting down on the missile. */
static uint8_t RemoteFired(const Ability* a, uint32_t remote)
{
    uintptr_t linkers = AblManager(ABL_LINKERS), missiles = AblManager(ABL_MISSILES), abilities = AblManager(ABL_ABILITIES);
    uint32_t none = *(uint32_t*)(g_ablGame + ABL_NO_ENTITY), missile = none;
    int64_t link = AblFind(linkers, 0x20, remote);
    ((LinkFn)LaunchFn(L_LINKED))((void*)linkers, &missile, remote);
    int64_t seeking = missile != none ? AblFind(missiles, 0x20, missile) : -1;
    if (link < 0 || seeking < 0 || AblFind(abilities, 0x18, missile) < 0)
    {
        const char* why = NoMissileWhy(link, missile, none, seeking);
        Note("abilities: %u: remote %u fired with no missile linked to it (%s)", a->id, remote, why);
        CoopDiag("silo: remote %u fired here with no missile linked to it (%s, link names %u): another machine's silo; its missile counts down from its own start", remote, why, missile);
        return 1;
    }
    if (HeldFor(missile) != NULL) return 1; /* counting down already */
    if (MissileFlies(missiles, seeking))
    {
        Note("abilities: %u: remote %u fired, but its laser launched missile %u first: no countdown", a->id, remote, missile);
        return 1;
    }
    HeldLaunch* h = FreeHeld();
    if (h == NULL) { Note("abilities: %u: remote %u fired, but %u countdowns run already: no countdown", a->id, remote, ABL_MAX_HELD); return 1; }
    *h = (HeldLaunch){remote, missile, none, missile};
    h->ability = a;
    h->since = GetTickCount64();
    h->holdMs = a->holdTicks * 1000u / ABL_TICKS;
    if (!MarkLink(h))
    {
        h->missile = 0;
        Note("abilities: %u: remote %u fired, but its link is not a silo's: no countdown", a->id, remote);
        return 1;
    }
    ((LinkFn)LaunchFn(L_RACK))((void*)linkers, &h->rack, remote);
    uintptr_t owners = AblManager(ABL_OWNERS);
    int64_t owner = AblFind(owners, 0x20, remote);
    if (owner >= 0) h->owner = *(uint32_t*)(*(uint8_t**)(owners + 0x40) + (size_t)owner * 48 + 4);
    KeepMarkedPoint(h);
    /* Fired on a machine without authority over the missile (a friend firing the caller's silo): mode 2, whose start is
     * always sent (0x7CAFF1), so the countdown reaches the machine with authority, which launches at its end. */
    int authority = AblAuthority(missile);
    ((StartAbilityFn)LaunchFn(L_START))((void*)abilities, missile, a->id, authority ? 1u : 2u, 1.0f);
    if (!h->started)
    {
        GiveLinkBack(h);
        h->missile = 0;
        Note("abilities: %u: remote %u fired, but missile %u did not take the countdown", a->id, remote, missile);
        return 1;
    }
    Note("abilities: %u: remote %u holds missile %u's launch for %u ticks (rack %u, owner %u, its link record %s; %s)", a->id, remote,
        missile, a->holdTicks, h->rack, h->owner, h->sharedRecord ? "the table's" : "the manager's copy",
        authority ? "this machine launches it" : "the start was sent to the machine with authority over the missile, which launches it");
    CoopDiag("silo: remote %u fired here: missile %u counts down %u ticks (rack %u, owner %u, %s); the laser %s (%.1f, %.1f, %.1f)", remote, missile,
        a->holdTicks, h->rack, h->owner, authority ? "authority here" : "NOT authority here: start sent with mode 2",
        h->marked ? "marks" : "marks nothing yet, pointing at", (double)h->point[0], (double)h->point[1], (double)h->point[2]);
    return 1;
}

/* Co-op: a countdown another machine set off (its start reaches this one with the missile). The remote's link is marked
 * here too: once the laser-on byte replicates, an unmarked link would launch this machine's copy at once. The launch at
 * the end is this machine's when it has authority over the missile (a friend fired the caller's silo, or the missile
 * migrated here). What it knows: the remote, from the missile (+8); the silo, through the remote's link where it has
 * one; the point the remote's laser marks; the remote's owner. */
static HeldLaunch* ShadowFor(const Ability* a, uint32_t missile)
{
    HeldLaunch* h = FreeHeld();
    if (h == NULL) return NULL;
    uint32_t none = *(uint32_t*)(g_ablGame + ABL_NO_ENTITY);
    uint8_t* instance = NULL;
    uint32_t remote = MissileRemote(missile, &instance);
    *h = (HeldLaunch){remote, missile, none, missile};
    h->ability = a;
    h->shadow = h->started = 1;
    h->since = GetTickCount64();
    h->holdMs = a->holdTicks * 1000u / ABL_TICKS;
    if (remote != none)
    {
        uintptr_t linkers = AblManager(ABL_LINKERS), owners = AblManager(ABL_OWNERS);
        if (AblFind(linkers, 0x20, remote) >= 0) ((LinkFn)LaunchFn(L_RACK))((void*)linkers, &h->rack, remote);
        int64_t owner = AblFind(owners, 0x20, remote);
        if (owner >= 0) h->owner = *(uint32_t*)(*(uint8_t**)(owners + 0x40) + (size_t)owner * 48 + 4);
        KeepMarkedPoint(h);
        MarkLink(h);
    }
    Note("abilities: %u: missile %u's countdown was set off on another machine (remote %u, rack %u, its link %s)", a->id, missile, remote,
        h->rack, h->linkMarked ? "marked here" : "not marked here");
    CoopDiag("silo: missile %u counts down here from another machine's start (remote %u, rack %u, owner %u, link %s); the laser %s (%.1f, %.1f, %.1f)",
        missile, remote, h->rack, h->owner, h->linkMarked ? "marked" : "NOT marked", h->marked ? "marks" : "marks nothing here",
        (double)h->point[0], (double)h->point[1], (double)h->point[2]);
    return h;
}

/* A launch point a laser can mark: a sane place on a mission map (a joiner's first co-op launch of 2026-10-21 went to
 * (9550, -3148, -1273), the host's to (384, -450, 16)). */
static int PlausiblePoint(const float p[3])
{
    for (int k = 0; k < 3; k++) if (!isfinite(p[k])) return 0;
    return fabsf(p[0]) < 20000.0f && fabsf(p[1]) < 20000.0f && p[2] > -500.0f && p[2] < 3000.0f;
}

/* A countdown's end that launches nothing here: its missile gone (seeking < 0) or flying already. */
static void NoteNotLaunched(const Ability* a, uint32_t missile, int64_t seeking)
{
    Note("abilities: %u: missile %u %s: not launched here", a->id, missile, seeking < 0 ? "is gone" : "flies already");
    CoopDiag("silo: missile %u's countdown is over here, but it %s: not launched here", missile, seeking < 0 ? "is GONE" : "flies already");
    CoopDiagMissile(missile, "countdown over, not launched here");
}
/* A countdown's launch here, where it went (plausible: to the laser's point). */
static void NoteLaunched(const Ability* a, const HeldLaunch* h, uint32_t missile, int plausible)
{
    Note("abilities: %u: missile %u launched%s at (%.0f, %.0f, %.0f)%s", a->id, missile, h->shadow ? " (its countdown set off elsewhere)" : "",
        (double)h->point[0], (double)h->point[1], (double)h->point[2],
        !plausible ? ", which is no place on a map: it keeps its own guidance" :
        h->marked ? "" : h->shadow ? ", with its own guidance (no laser point reached this machine)" : ", where the remote pointed (its laser marked no point)");
    CoopDiag("silo: missile %u launched here%s toward (%.1f, %.1f, %.1f)%s (laser %s, owner %u)", missile, h->shadow ? " (its countdown set off elsewhere)" : "",
        (double)h->point[0], (double)h->point[1], (double)h->point[2], plausible ? "" : ": NO PLACE ON A MAP, its own guidance kept",
        h->marked ? "marked it" : "marked nothing", h->owner);
    CoopDiagMissile(missile, h->shadow ? "launched here, set off elsewhere" : "launched here");
}

/* The countdown's end: what the remote's link does at a launch, in its order (0x76B580). A missile flying already (its
 * launch reached this machine some other way) is left as it is. */
static void LaunchHeld(const Ability* a, HeldLaunch* h)
{
    uintptr_t missiles = AblManager(ABL_MISSILES), explosives = AblManager(ABL_EXPLOSIVES), racks = AblManager(ABL_RACKS);
    uint32_t none = *(uint32_t*)(g_ablGame + ABL_NO_ENTITY), missile = h->missile;
    h->missile = 0; /* first: launching starts 2088 on the missile, in this ability's place */
    GiveLinkBack(h);
    int64_t seeking = AblFind(missiles, 0x20, missile);
    if (seeking < 0 || MissileFlies(missiles, seeking)) { NoteNotLaunched(a, missile, seeking); return; }
    ((EngineFn)LaunchFn(L_ENGINE))((void*)missiles, missile, 1);
    ((MissileFn)LaunchFn(L_LAUNCH))((void*)missiles, missile);
    if (AblFind(explosives, 0x38, missile) >= 0) ((OwnerFn)LaunchFn(L_OWNER))((void*)explosives, missile, h->owner);
    /* a copy whose remote's laser marked nothing here keeps the guidance it has, rather than flying to (0, 0, 0); so does
     * one whose laser point is no place on a map */
    int plausible = PlausiblePoint(h->point);
    if ((h->marked || !h->shadow) && plausible && AblFind(AblManager(ABL_GUIDANCE), 0x840, missile) >= 0) ((GuideFn)LaunchFn(L_GUIDE))(NULL, missile, h->point);
    if (h->rack != none && AblFind(racks, 0x28, h->rack) >= 0) ((ReleaseFn)LaunchFn(L_RELEASE))((void*)racks, h->rack, missile);
    NoteLaunched(a, h, missile, plausible);
}

/* A silo countdown's tick on the remote: its fire (tick 0) starts the countdown where this machine has authority over it. */
static uint8_t RemoteTick(const Ability* a, uint32_t remote, uint32_t tick, int authority)
{
    if (tick != 0) return 1;
    if (authority) return RemoteFired(a, remote);
    CoopDiag("silo: remote %u fired on another machine (NOT authority here over the remote): that machine starts the countdown", remote);
    return 1;
}
static void DiagCountdownStart(uint32_t missile, int authority, const HeldLaunch* h)
{
    CoopDiag("silo: missile %u's countdown starts here (%s, %s)", missile, authority ? "authority here" : "NOT authority here",
        h == NULL ? "no countdown slot" : h->shadow ? "set off elsewhere" : "fired here");
}
/* Where a countdown's sounds play: the silo, else the missile. */
static uint32_t CountdownSoundAt(const HeldLaunch* h, uint32_t missile)
{
    uint32_t none = *(uint32_t*)(g_ablGame + ABL_NO_ENTITY);
    return h != NULL && h->rack != none && h->rack != 0 ? h->rack : missile;
}
/* A hold's sound at its tick: the start's at tick 0, then one each second. */
static void HoldSound(uint32_t at, const Ability* a, uint32_t tick)
{
    if (tick == 0) AbilitySound(at, a->startEvent);
    else if (tick % ABL_TICKS == 0) AbilitySound(at, a->secondEvent);
}
/* The countdown's last tick here without authority over the missile: the machine with it launches. */
static void CountdownOverElsewhere(const Ability* a, HeldLaunch* h, uint32_t missile)
{
    if (h != NULL) h->ended = 1; /* the link stays marked until the launch arrives (SiloFrame) */
    Note("abilities: %u: missile %u's countdown is over here; the machine with authority over it launches it", a->id, missile);
    CoopDiag("silo: missile %u's countdown is over here, NOT authority here: the launch comes with the missile's state (%s)", missile,
        h != NULL ? "link kept marked until then" : "no countdown slot here");
    CoopDiagMissile(missile, "countdown over without authority");
}

/* A silo countdown's tick: on the remote, it fired; on its missile, the hold, then the launch.
 * Co-op: the silo, its missile and its remote belong to the machine that called the silo in (the host's ids on a
 * friend's machine), and the countdown runs on every machine its start reaches. Only the machine with authority over the
 * missile at the last tick launches it (a migration can move it mid-countdown: the slot is made then if there is none);
 * the others keep the link marked until the launch arrives with the missile's state (their laser's link runs on every
 * machine, 0x9C0769, and launches a missile not yet launched here), and SiloFrame frees the slot then. Engine 0.1.19's
 * launch on every machine set the explosive off once per machine (0x645940 starts 2088 wherever it runs). The remote's
 * own fire runs on the machine with authority over the remote (whose holder fires it); the others' copies of its ability
 * start nothing (a second start would restart the missile's countdown on its owner, 0x7CB01F). */
static uint8_t LinkedLaunchTick(const Ability* a, uint32_t entity, uint32_t tick, int authority)
{
    if (!g_launchReady) return 1;
    if (AblFind(AblManager(ABL_LINKERS), 0x20, entity) >= 0) return RemoteTick(a, entity, tick, authority);
    HeldLaunch* h = HeldFor(entity);
    if (h == NULL && tick == 0) h = ShadowFor(a, entity);
    if (tick == 0) DiagCountdownStart(entity, authority, h);
    uint32_t at = CountdownSoundAt(h, entity);
    if (h != NULL)
    {
        if (tick == 0) { h->started = 1; h->ended = 0; h->since = GetTickCount64(); } /* a start again: the countdown again */
        KeepMarkedPoint(h);
    }
    if (tick < a->holdTicks) { HoldSound(at, a, tick); return 0; }
    AbilitySound(at, a->finalEvent);
    if (!authority) { CountdownOverElsewhere(a, h, entity); return 1; }
    if (h == NULL) h = ShadowFor(a, entity); /* the countdown migrated here, or this machine joined during it */
    if (h != NULL) LaunchHeld(a, h);
    else CoopDiag("silo: missile %u's countdown is over here with no countdown slot: not launched", entity);
    return 1;
}

/* A countdown slot freed (why: what became of its missile), its remote given its link back. */
static void FreeSiloSlot(HeldLaunch* h, const char* why, int64_t seeking, int authority)
{
    Note("abilities: missile %u %s: its countdown slot is freed (remote %u)", h->missile, why, h->remote);
    CoopDiag("silo: missile %u %s %s (%s); its slot is freed", h->missile, why,
        h->ended ? "after this machine's countdown ended" : "before this machine's countdown ended", authority ? "authority here" : "NOT authority here");
    if (seeking >= 0) CoopDiagMissile(h->missile, h->ended ? "launch arrived" : "launched elsewhere or overdue");
    GiveLinkBack(h);
    h->missile = 0;
}
/* One countdown slot's frame (SiloFrame). */
static void SiloSlotFrame(HeldLaunch* h, uintptr_t missiles, ULONGLONG now)
{
    if (h->missile == 0 || !h->started) return;
    int64_t seeking = AblFind(missiles, 0x20, h->missile);
    int authority = AblAuthority(h->missile);
    int launched = seeking >= 0 && (*(uint8_t**)(missiles + 0x50))[(size_t)seeking * 0x38 + 0x36] != 0;
    if (h->ended && authority && seeking >= 0 && !launched && h->ability != NULL)
    {
        CoopDiag("silo: missile %u's countdown ended here without authority, and the missile has since moved here unlaunched: launched here", h->missile);
        LaunchHeld(h->ability, h);
        return;
    }
    ULONGLONG late = (ULONGLONG)h->holdMs + (authority ? 2ull * h->holdMs + 10000 : 5000);
    const char* why = seeking < 0 ? "is gone" : launched ? "was launched" : now - h->since > late ? "is long past its countdown's end" : NULL;
    if (why == NULL) return;
    FreeSiloSlot(h, why, seeking, authority);
}

/* Every frame on the game thread: frees the countdown slots whose last tick will not come here, or came without authority
 * (the game drops a removed missile's abilities with no last tick, 0x54A380), giving the remote its link back: the
 * missile gone; launched (its launched byte +0x36, which is what the laser's link checks, set here or as another
 * machine's launch reached it); or long past its hold (with authority, three holds and 10 s, in case the game slows;
 * without, 5 s: the owner's launch is on its way). A countdown over without authority whose missile then migrated here
 * unlaunched is launched here. */
static void SiloFrame(HfMod* mod, float dt, void* user)
{
    (void)mod;
    (void)dt;
    (void)user;
    if (!g_launchReady) return;
    ULONGLONG now = GetTickCount64();
    uintptr_t missiles = AblManager(ABL_MISSILES);
    for (uint32_t i = 0; i < ABL_MAX_HELD; i++) SiloSlotFrame(&g_heldLaunches[i], missiles, now);
}

/* A mission's end: every countdown is over (its missile goes with the mission); the remotes' links back, now, while the
 * records they marked are still the mission's. */
static void SiloMissionEnd(HfMod* mod, const HfEventInfo* event, void* user)
{
    (void)mod;
    (void)event;
    (void)user;
    for (uint32_t i = 0; i < ABL_MAX_HELD; i++)
    {
        if (g_heldLaunches[i].missile == 0) continue;
        GiveLinkBack(&g_heldLaunches[i]);
        g_heldLaunches[i].missile = 0;
    }
    memset(g_sharedMarks, 0, sizeof g_sharedMarks);
}

/* The missile pre-destroy's read of its remote's attachable record (0x642570; see the loose remote above). The game
 * looks the remote up (ebx: its record, -1 when not found) and then reads the record unchecked:
 *   0x64270B mov rdx, [r10+0x38]; mov eax, ebx; imul r8, rax, 0xAC   (13 bytes, entered from 0x6426A6, 0x6426BB,
 *   0x6426F0, 0x642705 and by falling through 0x642707)
 *   0x642718 mov eax, [r12+0xC]; cmp [r8+rdx], eax; jne 0x642731 ...
 * Those 13 bytes become a jump to a stub near game.dll: not found, on to the function's end (0x642731, as for a holder
 * that is not the missile's); found, the 13 bytes, then back to 0x642718. The stub is written before the jump. */
#define ABL_GUARD_LOOKUP 0x64260Fu
#define ABL_GUARD_FOUND 0x642703u
#define ABL_GUARD_SITE 0x64270Bu
#define ABL_GUARD_BACK 0x642718u
#define ABL_GUARD_NEXT 0x642721u
#define ABL_GUARD_END 0x642731u
static const uint8_t kGuardLookupPin[5] = {0xBB, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t kGuardFoundPin[8] = {0x3B, 0xC1, 0x75, 0x04, 0x41, 0x8B, 0x58, 0x04};
static const uint8_t kGuardSitePin[13] = {0x49, 0x8B, 0x52, 0x38, 0x8B, 0xC3, 0x4C, 0x69, 0xC0, 0xAC, 0x00, 0x00, 0x00};
static const uint8_t kGuardBackPin[9] = {0x41, 0x8B, 0x44, 0x24, 0x0C, 0x41, 0x39, 0x04, 0x10};
static const uint8_t kGuardNextPin[2] = {0x75, 0x0E};
static const uint8_t kGuardEndPin[5] = {0x4C, 0x8B, 0x74, 0x24, 0x28};

static int GuardPins(uintptr_t game)
{
    static const struct { uint32_t rva; const uint8_t* pin; size_t size; } pins[] = {
        {ABL_GUARD_LOOKUP, kGuardLookupPin, sizeof kGuardLookupPin}, {ABL_GUARD_FOUND, kGuardFoundPin, sizeof kGuardFoundPin},
        {ABL_GUARD_SITE, kGuardSitePin, sizeof kGuardSitePin}, {ABL_GUARD_BACK, kGuardBackPin, sizeof kGuardBackPin},
        {ABL_GUARD_NEXT, kGuardNextPin, sizeof kGuardNextPin}, {ABL_GUARD_END, kGuardEndPin, sizeof kGuardEndPin}};
    uint8_t code[16];
    for (size_t i = 0; i < sizeof pins / sizeof pins[0]; i++)
        if (SafeRead(game + pins[i].rva, code, pins[i].size) || memcmp(code, pins[i].pin, pins[i].size)) return 0;
    return 1;
}

/* The guard's stub at `stub` (in the abilities' block, writable still). */
static void EmitLooseGuard(uintptr_t game, uint8_t* stub)
{
    Emit e = {stub};
    Put(&e, "\x83\xFB\xFF", 3);                                       /* cmp ebx, -1 */
    Put(&e, "\x0F\x84", 2);                                           /* je end */
    Put32(&e, (uint32_t)(int32_t)(int64_t)(game + ABL_GUARD_END - ((uintptr_t)e.at + 4)));
    Put(&e, kGuardSitePin, sizeof kGuardSitePin);                     /* the 13 bytes */
    Put8(&e, 0xE9);                                                   /* jmp back */
    Put32(&e, (uint32_t)(int32_t)(int64_t)(game + ABL_GUARD_BACK - ((uintptr_t)e.at + 4)));
}

/* The jump to the stub, once the block is executable: 5 bytes (the stub returns past the rest), inside one aligned qword
 * (0x642708), written at once. */
static void InstallLooseGuard(uintptr_t game, uint8_t* stub)
{
    uint8_t jump[5] = {0xE9};
    int32_t rel = (int32_t)(int64_t)((uintptr_t)stub - (game + ABL_GUARD_SITE + 5));
    memcpy(jump + 1, &rel, 4);
    uintptr_t at = game + ABL_GUARD_SITE, qword = at & ~(uintptr_t)7;
    DWORD protect;
    g_looseGuard = 0;
    if (at + 5 <= qword + 8 && VirtualProtect((void*)qword, 8, PAGE_EXECUTE_READWRITE, &protect))
    {
        LONG64 before = *(volatile LONG64*)qword, after = before;
        memcpy((uint8_t*)&after + (at - qword), jump, 5);
        g_looseGuard = InterlockedCompareExchange64((volatile LONG64*)qword, after, before) == before;
        VirtualProtect((void*)qword, 8, protect, &protect);
        FlushInstructionCache(GetCurrentProcess(), (void*)qword, 8);
        NoteRewrite(qword, 8);
    }
    Note(g_looseGuard ? "abilities: the missile pre-destroy's remote lookup is guarded (game.dll+0x64271d)"
                      : "abilities: the missile pre-destroy's guard could not be written");
}

/* A HellForge ability's tick (the dispatcher's stub calls it on the game thread): the hold, then its base. */
static uint8_t AbilityTick(void* ctx, uint32_t id, uint32_t tick)
{
    if (id < ABL_FIRST || id >= ABL_FIRST + g_abilityCount) return 0;
    const Ability* a = &g_abilities[id - ABL_FIRST];
    uint32_t entity = 0;
    SafeRead((uintptr_t)ctx + 8, &entity, 4);
    uint32_t flags = 0; /* the ability's entity info (as every component manager's): bit 0, authority on this machine */
    SafeRead((uintptr_t)ctx + 0x14, &flags, 4);
    if (a->holdsLaunch) return LinkedLaunchTick(a, entity, tick, (flags & 1) != 0);
    if (tick == 0) Note("abilities: %u set off on entity %u (its base %u runs in %u ticks)", id, entity, a->base, a->holdTicks);
    if (tick < a->holdTicks) { HoldSound(entity, a, tick); return 0; }
    if (tick == a->holdTicks) { AbilitySound(entity, a->finalEvent); Note("abilities: %u on entity %u: its base %u runs", id, entity, a->base); }
    return a->baseFn != NULL ? a->baseFn(ctx, tick - a->holdTicks) : 1;
}

static int RefuseAbilities(const char* why, uint32_t at)
{
    Note("abilities: %s (0x%x); %u ability(s) not added: entities given one never set it off", why, at, g_abilityCount);
    g_abilityState = -1;
    for (uint32_t i = 0; i < g_abilityCount; i++) ApiLog(g_abilities[i].mod, "ability %u not added: %s", g_abilities[i].id, why);
    return 1;
}

/* Code that is not the reviewed one yet: waited for until the grace is over, then refused (1). */
static int AbilityNotReviewed(int graceOver, const char* why, uint32_t at) { return graceOver ? RefuseAbilities(why, at) : 0; }

/* The ability dispatcher (its start and its default) and the entity descriptor lookup as reviewed: NULL, else why not
 * (*at: where). */
static const char* AbilityCodeRefused(uintptr_t game, uint32_t* at)
{
    uint8_t pin[sizeof kAbilityDispatchPin], tail[sizeof kAbilityDefaultPin];
    if (SafeRead(game + ABL_DISPATCH, pin, sizeof pin) || memcmp(pin, kAbilityDispatchPin, sizeof pin) ||
        SafeRead(game + ABL_DEFAULT, tail, sizeof tail) || memcmp(tail, kAbilityDefaultPin, sizeof tail))
    { *at = ABL_DISPATCH; return "the ability dispatcher is not the reviewed one"; }
    uint8_t descriptor[32];
    if (SafeRead(game + kBhvFunctions[F_DESCRIPTOR].rva, descriptor, 32) || memcmp(descriptor, kBhvFunctions[F_DESCRIPTOR].pin, 32))
    { *at = kBhvFunctions[F_DESCRIPTOR].rva; return "the entity descriptor lookup is not the reviewed one"; }
    return NULL;
}

/* The silo countdowns' game functions, when an ability holds a launch: g_launchReady when they are the reviewed ones.
 * 0 while one is not and the grace is not over. */
static int CheckLaunchFunctions(uintptr_t game, int graceOver)
{
    int launches = 0;
    for (uint32_t i = 0; i < g_abilityCount; i++) launches |= g_abilities[i].holdsLaunch;
    g_launchReady = 0;
    if (!launches) return 1;
    int f = 0;
    uint8_t code[32];
    for (; f < L_COUNT; f++)
        if (SafeRead(game + kLaunchFunctions[f].rva, code, 32) || memcmp(code, kLaunchFunctions[f].pin, 32)) break;
    if (f < L_COUNT && !graceOver) return 0;
    g_launchReady = f == L_COUNT;
    if (!g_launchReady)
        for (uint32_t i = 0; i < g_abilityCount; i++)
            if (g_abilities[i].holdsLaunch)
                ApiLog(g_abilities[i].mod, "ability %u: game.dll 0x%x is not the reviewed function; its remote launches at once, as the silo's",
                    g_abilities[i].id, kLaunchFunctions[f].rva);
    return 1;
}

/* Each base's function, from its case: `mov edx, r8d; call <ability>; movzx r9d, al`. 0 with *base the first whose case
 * is not that shape. */
static int FindBaseFunctions(uintptr_t game, uint32_t* base)
{
    for (uint32_t i = 0; i < g_abilityCount; i++)
    {
        Ability* a = &g_abilities[i];
        if (a->base == 0) continue; /* none: it is done when its hold ends */
        int32_t rva = 0;
        uint8_t code[12];
        if (SafeRead(game + ABL_TABLE + (uintptr_t)(a->base - 1) * 4, &rva, 4) || SafeRead(game + (uintptr_t)rva, code, sizeof code) ||
            (memcmp(code, "\x41\x8B\xD0\xE8", 4) != 0 && memcmp(code, "\x44\x89\xC2\xE8", 4) != 0) || memcmp(code + 8, "\x44\x0F\xB6\xC8", 4) != 0)
        { *base = a->base; return 0; }
        int32_t rel;
        memcpy(&rel, code + 4, 4);
        a->baseFn = (AbilityFn)(game + (uintptr_t)rva + 8 + rel);
    }
    return 1;
}

/* The dispatcher's stub in a block near game.dll (*block; the guard's stub too, with `guard`), executable, with its
 * unwind data: the stub, or NULL with *why. */
static uint8_t* BuildAbilityStub(uintptr_t game, int guard, uint8_t** blockOut, const char** why)
{
    uint8_t* block = AllocateNearGame(game, 0x1000);
    *blockOut = block;
    if (block == NULL) { *why = "no memory near game.dll"; return NULL; }
    Emit e = {block};
    uint8_t* slots = block + 0x600;
    /* edx = id - 1, rcx = ctx, r8d = tick, r9b = 0 (the default returns it) */
    uint8_t* stub = e.at;
    Put8(&e, 0x8D); Put8(&e, 0x82); Put32(&e, (uint32_t)(1 - (int32_t)ABL_FIRST)); /* lea eax, [rdx + 1 - FIRST] */
    Put8(&e, 0x3D); Put32(&e, g_abilityCount);                                       /* cmp eax, COUNT */
    Put8(&e, 0x73); Put8(&e, 0);                                                      /* jae default */
    uint8_t* jae = e.at - 1;
    Put8(&e, 0x8D); Put8(&e, 0x90); Put32(&e, ABL_FIRST);                            /* lea edx, [rax + FIRST] */
    PutIndirect(&e, 0x15, &slots, (uintptr_t)AbilityTick);                           /* call AbilityTick(ctx, id, tick) */
    Put(&e, "\x41\x88\xC1", 3);                                                       /* mov r9b, al */
    Land(jae, &e);
    PutIndirect(&e, 0x25, &slots, game + ABL_DEFAULT);                               /* jmp default: movzx eax, r9b; ... ret */
    uint8_t* end = e.at;
    if (guard) EmitLooseGuard(game, block + 0x400);
    /* unwind data: the dispatcher's frame (sub rsp, 0x28: UWOP_ALLOC_SMALL) */
    BhvUnwind* unwind = (BhvUnwind*)(block + 0x800);
    *unwind = (BhvUnwind){1, 0, 1, 0, {0x4200}};
    RUNTIME_FUNCTION* function = (RUNTIME_FUNCTION*)(block + 0x900);
    function->BeginAddress = (DWORD)(stub - block);
    function->EndAddress = (DWORD)(end - block);
    function->UnwindData = (DWORD)((uint8_t*)unwind - block);
    DWORD old;
    if (!VirtualProtect(block, 0x1000, PAGE_EXECUTE_READ, &old)) { *why = "the stub could not be made executable"; return NULL; }
    FlushInstructionCache(GetCurrentProcess(), block, 0x1000);
    if (!RtlAddFunctionTable(function, 1, (DWORD64)(uintptr_t)block)) Note("abilities: the stub's unwind data was not registered");
    return stub;
}

/* The dispatcher's `ja` pointed at the stub, last, atomic (its rel32 sits inside one aligned qword): NULL, else why not. */
static const char* WriteAbilityBranch(uintptr_t game, uint8_t* stub)
{
    uintptr_t at = game + ABL_JA + 2;
    int32_t rel = (int32_t)(int64_t)((uintptr_t)stub - (game + ABL_JA + 6));
    uintptr_t qword = at & ~(uintptr_t)7;
    if (at + 4 > qword + 8) return "the branch straddles a qword";
    DWORD protect;
    if (!VirtualProtect((void*)qword, 8, PAGE_EXECUTE_READWRITE, &protect)) return "the branch could not be written";
    LONG64 before = *(volatile LONG64*)qword, after = before;
    memcpy((uint8_t*)&after + (at - qword), &rel, 4);
    InterlockedCompareExchange64((volatile LONG64*)qword, after, before);
    VirtualProtect((void*)qword, 8, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), (void*)qword, 8);
    return NULL;
}

/* The silo countdowns' per-frame pass and mission end, under the first mod with a silo countdown. */
static void StartSiloPasses(void)
{
    for (uint32_t i = 0; i < g_abilityCount; i++)
        if (g_abilities[i].holdsLaunch)
        {
            if (!ApiOnGameThread(g_abilities[i].mod, SiloFrame, NULL))
                Note("abilities: the silo countdowns' per-frame pass is not running: a countdown whose missile goes early keeps its remote's link marked until the mission ends");
            ApiSubscribe(g_abilities[i].mod, HF_ON_MISSION_END, SiloMissionEnd, NULL, 0);
            break;
        }
}

static void WantAbilityPackages(void)
{
    for (uint32_t i = 0; i < g_abilityCount; i++)
    {
        const char* name;
        const char* why;
        if (g_abilities[i].package != 0 && PackageWant(g_abilities[i].package, &name, &why) == PKG_FAILED)
            ApiLog(g_abilities[i].mod, "ability %u: its sounds' package %016llx is not loaded: %s", g_abilities[i].id,
                (unsigned long long)g_abilities[i].package, why);
    }
}

/* The watch loop, after HF_EVENT_GAME_LOADED until it returns 1 (60 s for the copy protection to decode the code). */
static int InstallAbilities(uintptr_t game)
{
    if (g_abilityState != 0) return 1;
    if (g_abilityCount == 0) { g_abilityState = 1; return 1; }
    if (!g_abilityFirstTry) g_abilityFirstTry = GetTickCount64();
    int graceOver = GetTickCount64() - g_abilityFirstTry >= 60000;
    g_ablGame = game;
    uint32_t at = 0;
    const char* why = AbilityCodeRefused(game, &at);
    if (why != NULL) return AbilityNotReviewed(graceOver, why, at);
    g_bhvGame = game; /* BhvFn: the descriptor lookup */
    if (!CheckLaunchFunctions(game, graceOver)) return 0;
    if (!FindBaseFunctions(game, &at)) return AbilityNotReviewed(graceOver, "a base ability's case is not the reviewed shape", at);
    int guard = g_launchReady && GuardPins(game);
    if (g_launchReady && !guard) Note("abilities: the missile pre-destroy is not the reviewed code: its remote lookup is not guarded");
    uint8_t* block = NULL;
    uint8_t* stub = BuildAbilityStub(game, guard, &block, &why);
    if (stub == NULL) return RefuseAbilities(why, 0);
    why = WriteAbilityBranch(game, stub);
    if (why != NULL) return RefuseAbilities(why, ABL_JA);
    g_abilityState = 1;
    if (guard) InstallLooseGuard(game, block + 0x400);
    if (g_launchReady) StartSiloPasses();
    WantAbilityPackages();
    Note("abilities: %u ability(s) added (ids %u..%u), stub at 0x%llx", g_abilityCount, ABL_FIRST, ABL_FIRST + g_abilityCount - 1,
        (unsigned long long)(uintptr_t)block);
    return 1;
}
