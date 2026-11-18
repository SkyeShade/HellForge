/* What summoning enemies call up (EnemySummon; docs/internals-live.md "Spawning"). The Impaler's strike (ability 333:
 * 3 of family 0xAFF5B26B, its tentacles) and the Alpha Commander's call (463: 1 of 0x8F7704F8, the warriors, three
 * times) hand 0x11C8230 a family and a count as immediates in their code; it spawns them on the machine with the
 * director's authority (the host) from the mission faction's roster. (1702, 1922 and 1923 name 0x319EF181, which no
 * roster holds.) A row of HellForge's per ability, written as any table row, goes into the immediates on the game-thread
 * update (JournalPut). Pinned: the ability table entry and case stub, the function's first bytes, both moves, the call
 * and 0x11C8230's prologue. Included by director.c. */

#define SUM_SPAWN 0x11C8230u
#define SUM_ABILITIES 0x115C784u /* AbilityTick's table of case rvas, by id - 1 (abilities.c ABL_TABLE) */
#define SUM_ROW 8u
#define SUM_MAX 16u

typedef struct
{
    uint32_t ability, function, key, count, call; /* rvas: the function, its moves of the family and the count, its call */
    uint8_t opLength;                             /* the moves' bytes before their immediates */
    uint8_t keyOp[4], countOp[4];
    uint8_t prologue[13];
} SummonSite;
static const SummonSite kSummons[] = {
    {333u, 0x10B03B0u, 0x10B059Du, 0x10B05B1u, 0x10B05C0u, 4, {0xC7, 0x44, 0x24, 0x48}, {0xC7, 0x44, 0x24, 0x60},
        {0x40, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x50}}, /* mov [rsp+0x48] / [rsp+0x60], imm32 */
    {463u, 0x10B9190u, 0x10B922Du, 0x10B9245u, 0x10B9267u, 3, {0xC7, 0x45, 0xC8}, {0xC7, 0x45, 0xE0},
        {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x70}}, /* mov [rbp-0x38] / [rbp-0x20], imm32 */
};
#define SUM_SITES (sizeof kSummons / sizeof kSummons[0])
/* 0x11C8230: mov r11, rsp; push rbp; push rsi; lea rbp, [r11-0x9A8]; sub rsp, 0xA98 */
static const uint8_t kSummonSpawnPin[] = {0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x49, 0x8D, 0xAB, 0x58, 0xF6, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x98, 0x0A, 0x00, 0x00};

static int JournalPut(uintptr_t address, const uint8_t* bytes, size_t size); /* live_code.c */

typedef struct { uint32_t family, count; } SummonRow;
static SummonRow g_summonRows[SUM_SITES];
static int g_summonState; /* 0 not yet read, 1 rows hold the code's, -1 not as reviewed */

/* A call (E8 rel32) at `at` to `target`. */
static int SummonCalls(uintptr_t game, uint32_t at, uint32_t target)
{
    uint8_t b[5];
    int32_t rel;
    if (SafeRead(game + at, b, 5) || b[0] != 0xE8) return 0;
    memcpy(&rel, b + 1, 4);
    return at + 5 + (uint32_t)rel == target;
}

/* Whether a summon's code is as reviewed: its ability's case stub (mov edx, r8d; call function), the function's first
 * bytes, the two moves and the call. */
static int SummonPinned(uintptr_t game, const SummonSite* s)
{
    uint32_t stub = 0;
    uint8_t b[16];
    if (SafeRead(game + SUM_ABILITIES + (s->ability - 1) * 4, &stub, 4) || SafeRead(game + stub, b, 3) || b[0] != 0x41 || b[1] != 0x8B ||
        b[2] != 0xD0 || !SummonCalls(game, stub + 3, s->function)) return 0;
    if (SafeRead(game + s->function, b, sizeof s->prologue) || memcmp(b, s->prologue, sizeof s->prologue)) return 0;
    if (SafeRead(game + s->key, b, s->opLength) || memcmp(b, s->keyOp, s->opLength)) return 0;
    if (SafeRead(game + s->count, b, s->opLength) || memcmp(b, s->countOp, s->opLength)) return 0;
    return SummonCalls(game, s->call, SUM_SPAWN);
}

/* The rows, once the code reads as reviewed: the code's own values. game.dll's code is decoded some time after it
 * loads; not as reviewed a minute after the first try, none (-1). */
static void SummonsRead(uintptr_t game)
{
    static ULONGLONG first;
    if (g_summonState != 0) return;
    uint8_t b[sizeof kSummonSpawnPin];
    int ok = !SafeRead(game + SUM_SPAWN, b, sizeof b) && !memcmp(b, kSummonSpawnPin, sizeof b);
    for (size_t i = 0; i < SUM_SITES && ok; i++)
        ok = SummonPinned(game, &kSummons[i]) && !SafeRead(game + kSummons[i].key + kSummons[i].opLength, &g_summonRows[i].family, 4) &&
            !SafeRead(game + kSummons[i].count + kSummons[i].opLength, &g_summonRows[i].count, 4);
    if (!first) first = GetTickCount64();
    if (!ok && GetTickCount64() - first < 60000) return;
    g_summonState = ok ? 1 : -1;
    if (!ok) Note("summons: game.dll's summoning abilities are not the reviewed ones: what enemies summon is not edited");
}

/* A summon's row by its ability id, or 0: *ready 0 while the code may still be decoded. */
static uintptr_t SummonRowOf(uintptr_t game, uint64_t id, int* ready)
{
    SummonsRead(game);
    *ready = g_summonState != 0;
    if (g_summonState != 1) return 0;
    for (size_t i = 0; i < SUM_SITES; i++)
        if (kSummons[i].ability == id) return (uintptr_t)&g_summonRows[i];
    return 0;
}

static const char* DirectorFamilyUnknown(uint32_t family); /* director.c */

/* Why a summon row's write is refused, or NULL. */
static const char* SummonRefuses(uint32_t offset, uint32_t bits)
{
    if (offset == 4) return bits > SUM_MAX ? "a summon calls up 0 to 16 enemies" : NULL;
    if (offset != 0) return "only a summon's family and count are edited";
    return DirectorFamilyUnknown(bits);
}

/* A summon's row into its immediates where they differ: 0 when they hold it. */
static int SummonWrite(uintptr_t game, size_t i)
{
    const SummonSite* s = &kSummons[i];
    uint32_t family = 0, count = 0;
    if (SafeRead(game + s->key + s->opLength, &family, 4) || SafeRead(game + s->count + s->opLength, &count, 4)) return 0;
    if (family == g_summonRows[i].family && count == g_summonRows[i].count) return 0;
    int failed = 0;
    if (family != g_summonRows[i].family) failed |= JournalPut(game + s->key + s->opLength, (const uint8_t*)&g_summonRows[i].family, 4);
    if (count != g_summonRows[i].count) failed |= JournalPut(game + s->count + s->opLength, (const uint8_t*)&g_summonRows[i].count, 4);
    if (!failed) Note("summons: ability %u calls up %u of family 0x%08X a call", s->ability, g_summonRows[i].count, g_summonRows[i].family);
    return failed;
}

/* The game-thread update: the rows into the code; one that cannot be written ends the edits. */
static void SummonsTick(void)
{
    size_t size = 0;
    uintptr_t game = g_directorImage(&size);
    if (game == 0 || size != DIR_IMAGE || strcmp(g_build, "25480438")) return;
    SummonsRead(game);
    for (size_t i = 0; i < SUM_SITES && g_summonState == 1; i++)
        if (SummonWrite(game, i))
        {
            Note("summons: ability %u's code could not be written: summons are no longer edited", kSummons[i].ability);
            g_summonState = -1;
        }
}
