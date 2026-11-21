/* Explosion knockback behind cover (docs/gameplay-fixes.md). ProduceDamage (game.dll
 * 0x129DD30) moves a hit's damage event into the explosion system's line-of-sight queue where the hit zone asks for it;
 * a ray decides next frame (0x13C6A50, from ExplosionGather 0x13C6180) whether it lands. The knockback the same call
 * queues ([0x347CF38] + 0x155120) is checked by nothing, so cover stopped the damage and the Helldiver was thrown anyway.
 * Such a knockback is held here and goes into its queue only if the game's check lets its damage through. Hits the game
 * does not check keep its ways. Each machine does it for the explosions it simulates, as the game runs the check there.
 * All of it is pinned. Included by hellforge_modloader.c after shield_block.c. HELLFORGE_EXPLOSION_COVER=off turns it off. */

#define COVER_PRODUCE 0x129DD30u      /* ProduceDamage */
#define COVER_GATHER 0x13C6180u       /* ExplosionGather */
#define COVER_CHECK 0x13C6A50u        /* the line-of-sight check of one queued entry and ray */
#define COVER_EXPLOSIONS 0x346D558u   /* the explosion system (ExplosionQueue) */
#define COVER_COMBAT 0x347CF38u       /* CombatQueues */
#define COVER_LOS_COUNT 0x9828u
#define COVER_LOS_KICKED 0x982Cu      /* the entries this frame's gather decides, from the front */
#define COVER_LOS_ENTRIES 0x9830u
#define COVER_LOS_STRIDE 0xB0u
#define COVER_LOS_MAX 0x100u
#define COVER_LOS_DAMAGE 0x70u        /* 1: the entry is a damage event (+0 its target) */
#define COVER_LOS_RAY_A 0xA4u         /* the two ray ids the kick writes; the bytes before them are the entry's own */
#define COVER_LOS_RAY_B 0xA8u
#define COVER_KNOCK_ENTRIES 0x155120u
#define COVER_KNOCK_COUNT 0x201130u
#define COVER_KNOCK_STRIDE 0x40u
#define COVER_KNOCK_MAX 0x1000u
#define COVER_KNOCK_TARGET 4u
#define COVER_HIT_TARGET 0x20u        /* the hit event's target */
#define COVER_HELD 256u
#define COVER_STALE 600u              /* gathers without its entry: the queue was cleared (a mission ended) */

static const CodePin kCoverPins[] = {
    {COVER_PRODUCE, 24, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {0x129DD77u, 7, {0x4C, 0x8B, 0x35, 0xBA, 0xF1, 0x1D, 0x02}},                   /* r14 = CombatQueues */
    {0x129E71Cu, 7, {0x4C, 0x8B, 0x05, 0x35, 0xEE, 0x1C, 0x02}},                   /* r8 = the explosion system */
    {0x129E733u, 7, {0x45, 0x8B, 0x88, 0x28, 0x98, 0x00, 0x00}},                   /* its line-of-sight count */
    {0x129E781u, 7, {0x41, 0x81, 0xF9, 0x00, 0x01, 0x00, 0x00}},                   /* 256 at most */
    {0x129E78Eu, 7, {0x49, 0x69, 0xD1, 0xB0, 0x00, 0x00, 0x00}},                   /* 0xB0 each */
    {0x129E7A9u, 9, {0x42, 0xC6, 0x84, 0x02, 0xA0, 0x98, 0x00, 0x00, 0x01}},       /* +0x70: a damage event */
    {0x129EBE9u, 7, {0x41, 0x8B, 0x8E, 0x30, 0x11, 0x20, 0x00}},                   /* the knockback count */
    {0x129EC00u, 6, {0x81, 0xF9, 0x00, 0x10, 0x00, 0x00}},                         /* 4096 at most */
    {0x129EC1Bu, 4, {0x48, 0xC1, 0xE7, 0x06}},                                     /* 0x40 each */
    {0x129EC2Eu, 12, {0x41, 0x8B, 0x47, 0x20, 0x42, 0x89, 0x84, 0x37, 0x24, 0x51, 0x15, 0x00}}, /* +4: the hit's target */
    {COVER_GATHER, 15, {0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x56, 0x41, 0x54, 0x49, 0x8D, 0xAB, 0xF8, 0xF6, 0xFF, 0xFF}},
    {0x13C6651u, 7, {0x44, 0x39, 0xA6, 0x2C, 0x98, 0x00, 0x00}},                   /* the gather decides the kicked ones */
    {0x13C6686u, 12, {0x44, 0x8B, 0x83, 0xA4, 0x00, 0x00, 0x00, 0xE8, 0xBE, 0x03, 0x00, 0x00}}, /* check(entry, +0xA4) */
    {0x13C6696u, 7, {0x44, 0x8B, 0x83, 0xA8, 0x00, 0x00, 0x00}},                   /* else check(entry, +0xA8) */
    {COVER_CHECK, 24, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {0x12A74CCu, 7, {0x49, 0x8D, 0xB5, 0x50, 0x51, 0x15, 0x00}},                   /* the drain reads the knockbacks there */
    {0x12A781Du, 7, {0x49, 0x8D, 0x95, 0x20, 0x51, 0x15, 0x00}},
};

typedef void (*CoverProduceFn)(uint64_t unused, float* values, uint8_t* hit, uint32_t key);
typedef void (*CoverGatherFn)(uint8_t* explosions);
typedef uint8_t (*CoverCheckFn)(uint64_t unused, uint8_t* entry, uint32_t ray);

typedef struct CoverHeld
{
    uint8_t los[COVER_LOS_RAY_A];       /* the line-of-sight entry, up to its ray ids */
    uint8_t knock[COVER_KNOCK_STRIDE];  /* the knockback it decides */
    uint32_t age;                       /* gathers since it was held */
    int used;
} CoverHeld;

static CoverHeld g_coverHeld[COVER_HELD];
static SRWLOCK g_coverLock = SRWLOCK_INIT;
static CoverProduceFn g_coverProduce;
static CoverGatherFn g_coverGather;
static CoverCheckFn g_coverCheck;
static uintptr_t g_coverGame;
static int g_coverState; /* 0 not yet, 1 hooked, -1 not */
static ULONGLONG g_coverFirstTry;
static volatile LONG g_coverCounts[4]; /* held, let through, stopped, stale */

static uint32_t CoverU32(const uint8_t* at) { uint32_t v; memcpy(&v, at, 4); return v; }

/* After a ProduceDamage call (los / knock: the queues' counts before it, target: the hit's): the knockback it queued is
 * held when the same call queued the hit's damage for a line of sight. 1 when held. */
static int CoverHold(uint8_t* explosions, uint8_t* combat, uint32_t los, uint32_t knock, uint32_t target)
{
    uint32_t losNow = CoverU32(explosions + COVER_LOS_COUNT), knockNow = CoverU32(combat + COVER_KNOCK_COUNT);
    if (los >= COVER_LOS_MAX || losNow <= los || losNow > COVER_LOS_MAX || knock >= COVER_KNOCK_MAX || knockNow != knock + 1) return 0;
    uint8_t* entry = explosions + COVER_LOS_ENTRIES + (uintptr_t)los * COVER_LOS_STRIDE;
    uint8_t* order = combat + COVER_KNOCK_ENTRIES + (uintptr_t)knock * COVER_KNOCK_STRIDE;
    if (entry[COVER_LOS_DAMAGE] != 1 || CoverU32(entry) != target || CoverU32(order + COVER_KNOCK_TARGET) != target) return 0;
    int held = 0;
    AcquireSRWLockExclusive(&g_coverLock);
    for (uint32_t i = 0; i < COVER_HELD && !held; i++)
    {
        if (g_coverHeld[i].used) continue;
        memcpy(g_coverHeld[i].los, entry, sizeof g_coverHeld[i].los);
        memcpy(g_coverHeld[i].knock, order, COVER_KNOCK_STRIDE);
        g_coverHeld[i].age = 0;
        g_coverHeld[i].used = 1;
        memcpy(combat + COVER_KNOCK_COUNT, &knock, 4); /* the entry stays as unread bytes past the count */
        held = 1;
    }
    ReleaseSRWLockExclusive(&g_coverLock);
    if (held) InterlockedIncrement(&g_coverCounts[0]);
    return held;
}

/* A held knockback into the knockback queue, as ProduceDamage would have put it. */
static void CoverRelease(uint8_t* combat, const CoverHeld* h)
{
    uint32_t count = CoverU32(combat + COVER_KNOCK_COUNT);
    if (count >= COVER_KNOCK_MAX) return;
    memcpy(combat + COVER_KNOCK_ENTRIES + (uintptr_t)count * COVER_KNOCK_STRIDE, h->knock, COVER_KNOCK_STRIDE);
    count++;
    memcpy(combat + COVER_KNOCK_COUNT, &count, 4);
}

/* One held knockback's entry among the ones this gather decides, or -1. */
static int CoverEntryOf(uint8_t* explosions, uint32_t kicked, const CoverHeld* h)
{
    for (uint32_t i = 0; i < kicked; i++)
        if (!memcmp(explosions + COVER_LOS_ENTRIES + (uintptr_t)i * COVER_LOS_STRIDE, h->los, sizeof h->los)) return (int)i;
    return -1;
}

/* One held knockback before a gather (kicked: the entries it decides): let through or dropped when its entry is among
 * them (either ray clear, as the gather asks), aged otherwise. 1 when it is done with. */
static int CoverDecideOne(uint8_t* explosions, uint32_t kicked, uint8_t* combat, CoverCheckFn check, CoverHeld* h)
{
    int at = CoverEntryOf(explosions, kicked, h);
    if (at < 0)
    {
        if (++h->age < COVER_STALE) return 0;
        InterlockedIncrement(&g_coverCounts[3]);
        return 1;
    }
    uint8_t* entry = explosions + COVER_LOS_ENTRIES + (uintptr_t)at * COVER_LOS_STRIDE;
    int clear = check(0, entry, CoverU32(entry + COVER_LOS_RAY_A)) || check(0, entry, CoverU32(entry + COVER_LOS_RAY_B));
    if (clear) CoverRelease(combat, h);
    InterlockedIncrement(&g_coverCounts[clear ? 1 : 2]);
    return 1;
}

/* Before a gather (explosions: the system it gathers; system: the one ProduceDamage queues into). Held knockbacks whose
 * entry never comes go after COVER_STALE gathers; held for another system, they go in as the game would have put them. */
static void CoverDecide(uint8_t* explosions, uint8_t* system, uint8_t* combat, CoverCheckFn check)
{
    uint32_t kicked = CoverU32(explosions + COVER_LOS_KICKED);
    if (kicked > COVER_LOS_MAX) kicked = 0;
    AcquireSRWLockExclusive(&g_coverLock);
    for (uint32_t i = 0; i < COVER_HELD; i++)
    {
        CoverHeld* h = &g_coverHeld[i];
        if (!h->used) continue;
        if (explosions != system) CoverRelease(combat, h);
        if (explosions != system || CoverDecideOne(explosions, kicked, combat, check, h)) h->used = 0;
    }
    ReleaseSRWLockExclusive(&g_coverLock);
}

/* A pinned global's object (the pins prove both are read this way), or NULL before a world has made it. */
static uint8_t* CoverGlobal(uint32_t rva) { return *(uint8_t* volatile*)(g_coverGame + rva); }

static void CoverProduceDetour(uint64_t unused, float* values, uint8_t* hit, uint32_t key)
{
    uint8_t* explosions = CoverGlobal(COVER_EXPLOSIONS), *combat = CoverGlobal(COVER_COMBAT);
    if (explosions == NULL || combat == NULL || hit == NULL) { g_coverProduce(unused, values, hit, key); return; }
    uint32_t los = CoverU32(explosions + COVER_LOS_COUNT), knock = CoverU32(combat + COVER_KNOCK_COUNT);
    g_coverProduce(unused, values, hit, key);
    CoverHold(explosions, combat, los, knock, CoverU32(hit + COVER_HIT_TARGET));
}

static void CoverGatherDetour(uint8_t* explosions)
{
    uint8_t* combat = CoverGlobal(COVER_COMBAT);
    if (explosions != NULL && combat != NULL) CoverDecide(explosions, CoverGlobal(COVER_EXPLOSIONS), combat, g_coverCheck);
    g_coverGather(explosions);
}

/* Knockbacks held, let through, stopped and gone stale so far: a line at most once a minute while they change. */
static void ExplosionCoverReport(void)
{
    static LONG reported;
    static ULONGLONG last;
    LONG held = g_coverCounts[0];
    ULONGLONG now = GetTickCount64();
    if (g_coverState != 1 || held == reported || now - last < 60000) return;
    reported = held;
    last = now;
    Note("explosion cover: %ld explosion knockback(s) waited for the damage's line of sight: %ld let through, %ld stopped by cover, %ld stale",
        held, g_coverCounts[1], g_coverCounts[2], g_coverCounts[3]);
}

/* From game load: both hooks once every pin reads as reviewed (60 s for the code to decode), else the game's knockback. */
static void ExplosionCoverInstall(void)
{
    if (g_coverState != 0) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    ULONGLONG now = GetTickCount64();
    char value[16];
    if (!g_coverFirstTry) g_coverFirstTry = now;
    if (game == 0) return;
    DWORD size = GetEnvironmentVariableA("HELLFORGE_EXPLOSION_COVER", value, sizeof value);
    if (size > 0 && size < sizeof value && (!_stricmp(value, "off") || !strcmp(value, "0")))
    {
        g_coverState = -1;
        Note("explosion cover: off (HELLFORGE_EXPLOSION_COVER); explosions knock Helldivers down behind cover as the game does");
        return;
    }
    if (strcmp(g_build, "25480438")) { g_coverState = -1; return; }
    for (size_t i = 0; i < sizeof kCoverPins / sizeof kCoverPins[0]; i++)
        if (!PinMatches(game + kCoverPins[i].rva, kCoverPins[i].bytes, kCoverPins[i].length))
        {
            if (now - g_coverFirstTry < 60000) return;
            g_coverState = -1;
            Note("explosion cover: game.dll+%X is not the reviewed code; explosions keep the game's knockback", kCoverPins[i].rva);
            return;
        }
    g_coverGame = game;
    g_coverCheck = (CoverCheckFn)(game + COVER_CHECK);
    if (LoaderHook("explosion cover (damage)", game + COVER_PRODUCE, (void*)CoverProduceDetour, (void**)&g_coverProduce)) { g_coverState = -1; return; }
    if (LoaderHook("explosion cover (line of sight)", game + COVER_GATHER, (void*)CoverGatherDetour, (void**)&g_coverGather))
    {
        HookRemove(NULL, game + COVER_PRODUCE, (void*)CoverProduceDetour);
        g_coverState = -1;
        return;
    }
    g_coverState = 1;
    Note("explosion cover: on; an explosion's knockback waits for its damage's line of sight");
}
