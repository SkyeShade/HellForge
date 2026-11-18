/* The director's fixed enemy limits (EnemyLimits; docs/internals-live.md "Enemy limits"): numbers game.dll keeps in its
 * code, so the difficulty table's caps stop counting past them. A row per level (1 to 11) holds the game's values:
 * +0 the patrol gate (0x9514ED: 100), +4 the roaming gate (0x952F56: 70), +8 the spawn gate (0x952F62: 448), +12 the
 * despawn pass on (0x572C51), +16 its start (0x572C57: 100; any enemy from ten more, 0xA2C098) and +20 its distance in
 * metres (0xA2C0BC reads 22500.0, 150 squared). The stubs read a block near game.dll that the watch thread fills from
 * the mission's level (HiveMind manager +0x650, +0xF9). The code changes only once a row differs from the game's. The
 * director and the despawn pass run on the host (0x572C3C checks), so the host's rows decide. Included by director.c. */

#define EL_ROW 24u
#define EL_LEVELS 11u
#define EL_MOST 1000u
#define EL_PATROL_SITE 0x9514EAu   /* add eax, [rcx+0x1C]; cmp eax, 0x64; jbe spawn (8 bytes) */
#define EL_PATROL_SPAWN 0x9514F9u
#define EL_PATROL_BACK 0x9514F2u
#define EL_ROAMING_SITE 0x952F53u  /* add ecx, [rax+0x1C]; cmp ecx, 0x46; jge refuse (8 bytes) */
#define EL_ROAMING_REFUSE 0x952F0Bu
#define EL_ROAMING_BACK 0x952F5Bu
#define EL_UNITS_IMM 0x952F65u     /* cmp dword [rax+0xC], 0x1C0: its imm32 */
#define EL_PASS_SITE 0x572C51u     /* mov eax, [rdi+0x75E9D4]; add eax, 0x64; cmp [rdi+0x75E9E4], eax; jb skip (17 bytes) */
#define EL_PASS_SKIP 0x572C6Eu
#define EL_PASS_BACK 0x572C62u
#define EL_ANY_SITE 0xA2C090u      /* mulss xmm3, [0.6]; add ebx, 0x6E (11 bytes) */
#define EL_ANY_BACK 0xA2C09Bu
#define EL_POINT_SIX 0x23C6B3Cu
#define EL_FAR_READ 0xA2C0C0u      /* movss xmm5, [rip+disp32]: its disp32, read from 0xA2C0C4 */
#define EL_FAR_SQUARED 0x23C7E5Cu  /* 22500.0, shared with other code: the read moves, the constant stays */
#define EL_HIVEMIND 0x3326D10u
#define EL_AI_ENEMIES 0x33264D8u   /* +0xC: the AI enemy manager's count, which the spawn gate reads */
#define EL_AI_CHARACTERS 0x3326D18u /* +0x1C: the living AI characters, which the patrol gate adds */
#define EL_COUNT_MS 30000u

typedef struct { uint32_t patrolGate, roamingGate, spawnGate, despawn, despawnStart; float despawnDistance; } EnemyLimitsRow;
/* What the stubs read: the values of the mission's level. */
typedef struct { uint32_t patrolGate, roamingGate, despawnOff, despawnStart, despawnAny; float farSquared; uint32_t patrols; } EnemyLimitsLive;
typedef struct { uint32_t rva; uint8_t length; uint8_t bytes[60]; } LimitsPin;

static const EnemyLimitsRow kLimitsGame = {100, 70, 448, 1, 100, 150.0f};
static const LimitsPin kLimitsPins[] = {
    {0x9514D6u, 35, {0x48, 0x8B, 0x0D, 0x33, 0x58, 0x9D, 0x02, 0x03, 0x81, 0xC0, 0x36, 0x00, 0x00, 0x48, 0x8B, 0x0D, 0x2E, 0x58,
        0x9D, 0x02, 0x03, 0x41, 0x1C, 0x83, 0xF8, 0x64, 0x76, 0x07, 0x80, 0x7C, 0x24, 0x51, 0x00, 0x74, 0x70}},
    {0x952F3Au, 49, {0x8B, 0x8B, 0xC0, 0x36, 0x00, 0x00, 0x2B, 0x8B, 0x20, 0x06, 0x00, 0x00, 0x2B, 0x8B, 0x1C, 0x06, 0x00, 0x00,
        0x48, 0x8B, 0x05, 0xC5, 0x3D, 0x9D, 0x02, 0x03, 0x48, 0x1C, 0x83, 0xF9, 0x46, 0x7D, 0xB0, 0x48, 0x8B, 0x05, 0x76, 0x35,
        0x9D, 0x02, 0x81, 0x78, 0x0C, 0xC0, 0x01, 0x00, 0x00, 0x73, 0xA0}},
    {0x572C35u, 57, {0x48, 0x8B, 0x05, 0xB4, 0xA2, 0xF0, 0x02, 0x48, 0x8B, 0x88, 0xA8, 0xB3, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74,
        0x09, 0x48, 0x3B, 0x88, 0x98, 0xB3, 0x00, 0x00, 0x75, 0x1D, 0x8B, 0x87, 0xD4, 0xE9, 0x75, 0x00, 0x83, 0xC0, 0x64, 0x39,
        0x87, 0xE4, 0xE9, 0x75, 0x00, 0x72, 0x0C, 0x48, 0x8D, 0x8F, 0x80, 0xD3, 0x75, 0x00, 0xE8, 0xC2, 0x93, 0x4B, 0x00}},
    {0xA2C08Au, 17, {0x8B, 0x99, 0x54, 0x16, 0x00, 0x00, 0xF3, 0x0F, 0x59, 0x1D, 0xA4, 0xAA, 0x99, 0x01, 0x83, 0xC3, 0x6E}},
    {0xA2C0BCu, 8, {0xF3, 0x0F, 0x10, 0x2D, 0x98, 0xBD, 0x99, 0x01}},
    {EL_POINT_SIX, 4, {0x9A, 0x99, 0x19, 0x3F}},    /* 0.6f */
    {EL_FAR_SQUARED, 4, {0x00, 0xC8, 0xAF, 0x46}}, /* 22500.0f */
};
#define EL_PINS (sizeof kLimitsPins / sizeof kLimitsPins[0])

static EnemyLimitsRow g_limitsRows[EL_LEVELS + 1]; /* by level; [0] unused */
static int g_limitsState;                          /* 0 not read yet, 1 as reviewed, 2 the code reads the block, -1 refused */
static uint8_t* g_limitsCode;                      /* the stubs; +0x1000 the block (EnemyLimitsLive) */
static EnemyLimitsRow g_limitsShown;               /* the values last applied, and their level, for the log */
static int g_limitsShownLevel = -1;
static ULONGLONG g_limitsFirst;                    /* the first read's time */

/* The rows, once the code reads as reviewed: every level the game's values. game.dll's code is decoded some time after
 * it loads; not as reviewed a minute after the first try, none (-1). */
static void LimitsRead(uintptr_t game)
{
    if (g_limitsState != 0) return;
    int ok = 1;
    for (size_t i = 0; i < EL_PINS && ok; i++) ok = PinMatches(game + kLimitsPins[i].rva, kLimitsPins[i].bytes, kLimitsPins[i].length);
    if (!g_limitsFirst) g_limitsFirst = GetTickCount64();
    if (!ok && GetTickCount64() - g_limitsFirst < 60000) return;
    g_limitsState = ok ? 1 : -1;
    for (uint32_t level = 1; ok && level <= EL_LEVELS; level++) g_limitsRows[level] = kLimitsGame;
    if (!ok) Note("enemy limits: game.dll's spawn gates and despawn pass are not the reviewed code: the enemy limits are not edited");
}

/* A level's row by its id (1 to 11), or 0: *ready 0 while the code may still be decoded. */
static uintptr_t LimitsRowOf(uintptr_t game, uint64_t id, int* ready)
{
    LimitsRead(game);
    *ready = g_limitsState != 0;
    return g_limitsState >= 1 && id >= 1 && id <= EL_LEVELS ? (uintptr_t)&g_limitsRows[id] : 0;
}

/* Why a write to a limits row is refused, or NULL. */
static const char* LimitsRefuses(uint32_t offset, uint32_t bits)
{
    float value;
    memcpy(&value, &bits, 4);
    if (offset == 12) return bits > 1 ? "the despawn pass is 0 (off) or 1 (on)" : NULL;
    if (offset == 20) return !isfinite(value) || value < 10 || value > 2000 ? "the despawn distance is 10 to 2000 m" : NULL;
    if (offset > 16 || offset % 4) return "not a field of the enemy limits";
    return bits > EL_MOST ? "an enemy limit is 0 to 1000" : NULL;
}

/* The row of the mission's level, or the game's values outside a mission; *level its level (0 none). */
static const EnemyLimitsRow* LimitsNow(uintptr_t game, int* level)
{
    uint64_t manager = 0, mission = 0;
    uint8_t at = 0;
    *level = 0;
    if (SafeRead(game + EL_HIVEMIND, &manager, 8) || manager == 0 || SafeRead((uintptr_t)manager + 0x650, &mission, 8) || mission == 0 ||
        SafeRead((uintptr_t)mission + 0xF9, &at, 1) || at < 1 || at > EL_LEVELS) return &kLimitsGame;
    *level = at;
    return &g_limitsRows[at];
}

static int LimitsChanged(void)
{
    for (uint32_t level = 1; level <= EL_LEVELS; level++)
        if (memcmp(&g_limitsRows[level], &kLimitsGame, sizeof kLimitsGame)) return 1;
    return 0;
}

/* A rel32 at `at` (the instruction ending at `next`) to `to`. */
static uint8_t* LimitsRel(uint8_t* at, uintptr_t next, uintptr_t to)
{
    int32_t rel = (int32_t)((intptr_t)to - (intptr_t)next);
    memcpy(at, &rel, 4);
    return at + 4;
}

static uint8_t* LimitsBytes(uint8_t* c, const uint8_t* bytes, size_t size) { memcpy(c, bytes, size); return c + size; }

/* op (its bytes before a rel32 operand), the rel32 to `to`, then `tail` bytes of the instruction after the rel32. */
static uint8_t* LimitsOp(uint8_t* c, const uint8_t* op, size_t size, uintptr_t to, size_t tail)
{
    c = LimitsBytes(c, op, size);
    return LimitsRel(c, (uintptr_t)c + 4 + tail, to);
}

/* The four stubs at code (+0, +0x40, +0x80, +0xC0), each ending in a jump back; live: the block they read. */
static void LimitsStubs(uint8_t* code, uintptr_t game, uintptr_t live)
{
    static const uint8_t addEax[] = {0x03, 0x41, 0x1C}, cmpEax[] = {0x3B, 0x05}, ja[] = {0x77, 0x0C}, count[] = {0xF0, 0xFF, 0x05}, jmp[] = {0xE9};
    static const uint8_t addEcx[] = {0x03, 0x48, 0x1C}, cmpEcx[] = {0x3B, 0x0D}, jge[] = {0x0F, 0x8D};
    static const uint8_t cmpByte[] = {0x80, 0x3D}, jne[] = {0x0F, 0x85}, jb[] = {0x0F, 0x82}, addFromRip[] = {0x03, 0x05};
    static const uint8_t movEax[] = {0x8B, 0x87, 0xD4, 0xE9, 0x75, 0x00}, cmpCount[] = {0x39, 0x87, 0xE4, 0xE9, 0x75, 0x00};
    static const uint8_t mulss[] = {0xF3, 0x0F, 0x59, 0x1D}, addEbx[] = {0x03, 0x1D};
    uint8_t* c = code; /* patrols: the gate from the block; each one let through counted (lock inc) for the log */
    c = LimitsBytes(c, addEax, sizeof addEax);
    c = LimitsOp(c, cmpEax, sizeof cmpEax, live + offsetof(EnemyLimitsLive, patrolGate), 0);
    c = LimitsBytes(c, ja, sizeof ja);
    c = LimitsOp(c, count, sizeof count, live + offsetof(EnemyLimitsLive, patrols), 0);
    c = LimitsOp(c, jmp, sizeof jmp, game + EL_PATROL_SPAWN, 0);
    LimitsOp(c, jmp, sizeof jmp, game + EL_PATROL_BACK, 0);
    c = code + 0x40; /* the roaming gate */
    c = LimitsBytes(c, addEcx, sizeof addEcx);
    c = LimitsOp(c, cmpEcx, sizeof cmpEcx, live + offsetof(EnemyLimitsLive, roamingGate), 0);
    c = LimitsOp(c, jge, sizeof jge, game + EL_ROAMING_REFUSE, 0);
    LimitsOp(c, jmp, sizeof jmp, game + EL_ROAMING_BACK, 0);
    c = code + 0x80; /* the despawn pass: off, or from the start */
    c = LimitsOp(c, cmpByte, sizeof cmpByte, live + offsetof(EnemyLimitsLive, despawnOff), 1);
    *c++ = 0x00;
    c = LimitsOp(c, jne, sizeof jne, game + EL_PASS_SKIP, 0);
    c = LimitsBytes(c, movEax, sizeof movEax);
    c = LimitsOp(c, addFromRip, sizeof addFromRip, live + offsetof(EnemyLimitsLive, despawnStart), 0);
    c = LimitsBytes(c, cmpCount, sizeof cmpCount);
    c = LimitsOp(c, jb, sizeof jb, game + EL_PASS_SKIP, 0);
    LimitsOp(c, jmp, sizeof jmp, game + EL_PASS_BACK, 0);
    c = code + 0xC0; /* the pass's "any enemy" count */
    c = LimitsOp(c, mulss, sizeof mulss, game + EL_POINT_SIX, 0);
    c = LimitsOp(c, addEbx, sizeof addEbx, live + offsetof(EnemyLimitsLive, despawnAny), 0);
    LimitsOp(c, jmp, sizeof jmp, game + EL_ANY_BACK, 0);
}

static void LimitsLiveOf(const EnemyLimitsRow* row, EnemyLimitsLive* live)
{
    live->patrolGate = row->patrolGate;
    live->roamingGate = row->roamingGate;
    live->despawnOff = row->despawn ? 0u : 1u;
    live->despawnStart = row->despawnStart;
    live->despawnAny = row->despawnStart + 10u;
    live->farSquared = row->despawnDistance * row->despawnDistance;
    live->patrols = 0;
}

/* A jump at site to stub, the rest of the `size` replaced bytes int3. */
static int LimitsJump(uintptr_t site, size_t size, const uint8_t* stub)
{
    uint8_t jump[17];
    memset(jump, 0xCC, sizeof jump);
    jump[0] = 0xE9;
    LimitsRel(jump + 1, site + 5, (uintptr_t)stub);
    return JournalPut(site, jump, size);
}

/* The stubs and the block near game.dll, then each site to them. NULL, or why not. A site already pointed at its stub
 * reads the block, which holds the game's values until LimitsApply writes the level's. */
static const char* LimitsInstall(uintptr_t game)
{
    g_limitsCode = AllocateNearGame(game, 0x2000);
    if (g_limitsCode == NULL) return "no memory within 2 GB of game.dll for the enemy limits";
    EnemyLimitsLive* live = (EnemyLimitsLive*)(g_limitsCode + 0x1000);
    LimitsLiveOf(&kLimitsGame, live);
    LimitsStubs(g_limitsCode, game, (uintptr_t)live);
    DWORD previous;
    VirtualProtect(g_limitsCode, 0x1000, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), g_limitsCode, 0x1000);
    uint8_t moved[4];
    LimitsRel(moved, game + EL_FAR_READ + 4, (uintptr_t)&live->farSquared);
    if (JournalPut(game + EL_FAR_READ, moved, 4)) return "the despawn distance's read could not be moved";
    if (LimitsJump(game + EL_PATROL_SITE, 8, g_limitsCode) || LimitsJump(game + EL_ROAMING_SITE, 8, g_limitsCode + 0x40) ||
        LimitsJump(game + EL_PASS_SITE, 17, g_limitsCode + 0x80) || LimitsJump(game + EL_ANY_SITE, 11, g_limitsCode + 0xC0))
        return "a spawn gate or the despawn pass could not be pointed at its stub";
    return NULL;
}

/* The level's values into the block and the spawn gate's immediate; logged when they change. 0, or 1 when the
 * immediate could not be written. */
static int LimitsApply(uintptr_t game, const EnemyLimitsRow* row, int level)
{
    EnemyLimitsLive want, *live = (EnemyLimitsLive*)(g_limitsCode + 0x1000);
    LimitsLiveOf(row, &want);
    want.patrols = live->patrols;
    if (memcmp(live, &want, offsetof(EnemyLimitsLive, patrols))) memcpy(live, &want, offsetof(EnemyLimitsLive, patrols));
    uint32_t units = 0;
    if (SafeRead(game + EL_UNITS_IMM, &units, 4) || (units != row->spawnGate && JournalPut(game + EL_UNITS_IMM, (const uint8_t*)&row->spawnGate, 4)))
        return 1;
    if (level == g_limitsShownLevel && !memcmp(row, &g_limitsShown, sizeof g_limitsShown)) return 0;
    g_limitsShown = *row;
    g_limitsShownLevel = level;
    if (level == 0) Note("enemy limits: no mission: the game's");
    else
        Note("enemy limits: difficulty %d: patrols up to %u enemies, roaming spawns under %u, other spawns under %u; despawn pass %s from %u (any enemy from %u, far from %.0f m)",
            level, row->patrolGate, row->roamingGate, row->spawnGate, row->despawn ? "on" : "off", row->despawnStart, row->despawnStart + 10u,
            (double)row->despawnDistance);
    return 0;
}

/* In a mission, every 30 s: the counts the gates compare against, so a log shows how near the game's walls a mod's
 * limits take it, and the patrols let through so far. */
static void LimitsCount(uintptr_t game, int level)
{
    static ULONGLONG last;
    uint64_t enemies = 0, characters = 0;
    uint32_t units = 0, living = 0;
    ULONGLONG now = GetTickCount64();
    if (level == 0 || now - last < EL_COUNT_MS || SafeRead(game + EL_AI_ENEMIES, &enemies, 8) || enemies == 0 ||
        SafeRead((uintptr_t)enemies + 0xC, &units, 4) || SafeRead(game + EL_AI_CHARACTERS, &characters, 8) || characters == 0 ||
        SafeRead((uintptr_t)characters + 0x1C, &living, 4)) return;
    last = now;
    Note("enemy limits: %u AI enemies, %u AI characters alive; %u patrols let through the gate", units, living,
        ((const EnemyLimitsLive*)(g_limitsCode + 0x1000))->patrols);
}

/* The watch thread's update: once a row differs from the game's the code reads the block, which then follows the
 * mission's level. A failed write ends the edits, the block back at the game's values. */
static void EnemyLimitsTick(void)
{
    size_t size = 0;
    uintptr_t game = g_directorImage(&size);
    if (game == 0 || size != DIR_IMAGE || strcmp(g_build, "25480438")) return;
    LimitsRead(game);
    if (g_limitsState == 1 && LimitsChanged())
    {
        const char* why = LimitsInstall(game);
        g_limitsState = why == NULL ? 2 : -1;
        if (why != NULL)
        {
            Note("enemy limits: refused: %s; the game's limits stay", why);
            if (g_limitsCode != NULL) LimitsLiveOf(&kLimitsGame, (EnemyLimitsLive*)(g_limitsCode + 0x1000));
            return;
        }
        Note("enemy limits: the spawn gates and the despawn pass read HellForge's rows");
    }
    if (g_limitsState != 2) return;
    int level = 0;
    const EnemyLimitsRow* row = LimitsNow(game, &level);
    if (LimitsApply(game, row, level))
    {
        Note("enemy limits: the spawn gate could not be written: the game's limits are back");
        LimitsLiveOf(&kLimitsGame, (EnemyLimitsLive*)(g_limitsCode + 0x1000));
        g_limitsState = -1;
        return;
    }
    LimitsCount(game, level);
}
