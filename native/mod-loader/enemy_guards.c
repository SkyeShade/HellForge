/* What lets an enemy be any entity (docs/modding/enemies.md): the director's unchecked reads of per-enemy data, each
 * given an answer for an enemy without that data. Included by director.c after enemy_rosters.c.
 * - Behavior (component 284): 0x90E150 reads the task mask (+8) of a group's first row. No record: a zeroed one, so
 *   the enemy takes no garrison task.
 * - SquadTransportFormation (122): the reinforcement planner (0x8B6930) and a transport's seating (0x6846C0) read the
 *   entry with the formation's id. No such entry: a copy of a single seat's entry of that formation.
 * - A group's name: 0x94B570 and 0xAD51B0 index a table of the game's rows by the group's first row. Their six reads
 *   go to the tables enemy_rosters.c grew.
 * The lookups (0x510790, 0x4FE170) change rax, rcx, rdx and r8 to r10; their callers keep r11 and xmm across them. */

#define EG_BEHAVIOR 0x510790u
#define EG_FORMATION 0x4FE170u
#define EG_FORMATION_ID 0x3DE4072Eu /* the formation the planner sizes every squad by */
#define EG_FORMATIONS 122u
#define EG_STUB 0x100 /* each stub's room in the cave (the longest is 0x8B bytes) */

/* The calls replaced: site, the lookup called, the stub taking its place (0 Behavior, 1 formation by ebx, 2 the planner's). */
static const struct { uint32_t site, lookup, stub; uint8_t pin[5]; } kEgCalls[] = {
    {0x90E7A8u, EG_BEHAVIOR, 0, {0xe8, 0xe3, 0x1f, 0xc0, 0xff}}, {0x90EC29u, EG_BEHAVIOR, 0, {0xe8, 0x62, 0x1b, 0xc0, 0xff}},
    {0x90F069u, EG_BEHAVIOR, 0, {0xe8, 0x22, 0x17, 0xc0, 0xff}}, {0x684A64u, EG_FORMATION, 1, {0xe8, 0x07, 0x97, 0xe7, 0xff}},
    {0x684B47u, EG_FORMATION, 1, {0xe8, 0x24, 0x96, 0xe7, 0xff}}, {0x8B6A96u, EG_FORMATION, 2, {0xe8, 0xd5, 0x76, 0xc4, 0xff}},
    {0x8B6DA3u, EG_FORMATION, 2, {0xe8, 0xc8, 0x73, 0xc4, 0xff}}};
#define EG_CALLS (sizeof kEgCalls / sizeof kEgCalls[0])
/* The name reads: the place of each one's 32-bit displacement from game.dll's base, by faction as kErFactions. */
static const uint32_t kEgNameReads[ER_FACTIONS][2] = {{0x94B5EDu, 0xAD52ABu}, {0x94B5CDu, 0xAD5297u}, {0x94B5DDu, 0xAD52A1u}};

static uint8_t g_egNoBehavior[12];
static struct { uint32_t id; uint8_t record[48]; } g_egSeats[8];
static volatile LONG g_egSeatCount, g_egSeated;
static LONG g_egSeatedReported;
static ULONGLONG g_egReportedAt;
static int g_egState; /* 1 installed */

/* A formation's entry (24 bytes) in the game's records, a single seat's first: NULL when no record has the formation. */
static const uint8_t* EgSeatInGame(uint32_t id)
{
    uint8_t* table = StatePointer(EntityState(), REG_TABLES + 8 * EG_FORMATIONS);
    uint32_t slots = SlotsOf(EG_FORMATIONS);
    const uint8_t* any = NULL;
    for (uint32_t i = 0; table != NULL && i < slots; i++)
    {
        uint64_t entity = 0;
        uint32_t record = 0, seats[2];
        memcpy(&entity, table + (size_t)i * 16, 8);
        memcpy(&record, table + (size_t)i * 16 + 8, 4);
        const uint8_t* entry = table + (size_t)slots * 16 + (size_t)record * 48;
        for (int n = 0; entity != 0 && n < 2; n++, entry += 24)
        {
            if (memcmp(entry, &id, 4) != 0) continue;
            memcpy(seats, entry + 4, 8);
            if (seats[0] == 1 && seats[1] == 1) return entry;
            any = entry;
        }
    }
    return any;
}
/* The native fixture replaces it. */
static const uint8_t* (*g_egSeat)(uint32_t id) = EgSeatInGame;

/* The record a transport reads for an enemy that has none for its formation: one seat, placed as a vanilla passenger
 * of a single seat is. Kept per formation; with no room left the first is made again. */
static const uint8_t* EgSeatFor(uint32_t id)
{
    InterlockedIncrement(&g_egSeated);
    LONG count = g_egSeatCount;
    for (LONG i = 0; i < count; i++) if (g_egSeats[i].id == id) return g_egSeats[i].record;
    LONG at = count < (LONG)(sizeof g_egSeats / sizeof g_egSeats[0]) ? count : 0;
    const uint8_t* seat = g_egSeat(id);
    uint32_t one[2] = {1, 1};
    memset(g_egSeats[at].record, 0, sizeof g_egSeats[at].record);
    if (seat != NULL) memcpy(g_egSeats[at].record, seat, 24);
    memcpy(g_egSeats[at].record, &id, 4);
    memcpy(g_egSeats[at].record + 4, one, 8);
    g_egSeats[at].id = id;
    if (at == count) InterlockedCompareExchange(&g_egSeatCount, count + 1, count);
    return g_egSeats[at].record;
}

static uint8_t* EgBytes(uint8_t* c, const uint8_t* bytes, size_t size) { memcpy(c, bytes, size); return c + size; }
static uint8_t* EgCall(uint8_t* c, uintptr_t to)
{
    static const uint8_t call[] = {0xff, 0xd0}; /* call rax */
    return EgBytes(SgImm64(c, (uint64_t)to), call, sizeof call);
}

/* Behavior: the lookup's record, or the zeroed one. */
static void EgBehaviorStub(uint8_t* c, uintptr_t lookup)
{
    static const uint8_t enter[] = {0x48,0x83,0xec,0x28};                             /* sub rsp, 0x28 */
    static const uint8_t leave[] = {0x48,0x83,0xc4,0x28, 0x48,0x85,0xc0, 0x75,0x0a};  /* add rsp, 0x28; test rax, rax; jnz ret */
    c = EgBytes(EgCall(EgBytes(c, enter, sizeof enter), lookup), leave, sizeof leave);
    c = SgImm64(c, (uint64_t)(uintptr_t)g_egNoBehavior);
    *c = 0xc3;
}

/* Formations: the lookup's record when one of its two entries has the id (ebx, or the planner's constant), else
 * EgSeatFor's, called with r11 and xmm0 to xmm5 kept. */
static void EgFormationStub(uint8_t* c, uintptr_t lookup, int planner)
{
    static const uint8_t enter[] = {0x48,0x83,0xec,0x28};
    static const uint8_t leave[] = {0x48,0x83,0xc4,0x28, 0x48,0x85,0xc0, 0x74,0x00}; /* ...; jz miss (patched below) */
    static const uint8_t byRegister[] = {0x39,0x18, 0x74,0x00, 0x39,0x58,0x18, 0x74,0x00};  /* cmp [rax], ebx; je done; cmp [rax+0x18], ebx; je done */
    static const uint8_t save[] = {0x41,0x53, 0x48,0x81,0xec,0x80,0x00,0x00,0x00,     /* push r11; sub rsp, 0x80 */
        0xf3,0x0f,0x7f,0x44,0x24,0x20, 0xf3,0x0f,0x7f,0x4c,0x24,0x30, 0xf3,0x0f,0x7f,0x54,0x24,0x40,
        0xf3,0x0f,0x7f,0x5c,0x24,0x50, 0xf3,0x0f,0x7f,0x64,0x24,0x60, 0xf3,0x0f,0x7f,0x6c,0x24,0x70};
    static const uint8_t restore[] = {0xf3,0x0f,0x6f,0x44,0x24,0x20, 0xf3,0x0f,0x6f,0x4c,0x24,0x30, 0xf3,0x0f,0x6f,0x54,0x24,0x40,
        0xf3,0x0f,0x6f,0x5c,0x24,0x50, 0xf3,0x0f,0x6f,0x64,0x24,0x60, 0xf3,0x0f,0x6f,0x6c,0x24,0x70,
        0x48,0x81,0xc4,0x80,0x00,0x00,0x00, 0x41,0x5b, 0xc3};                         /* add rsp, 0x80; pop r11; ret */
    uint32_t id = EG_FORMATION_ID;
    c = EgBytes(EgCall(EgBytes(c, enter, sizeof enter), lookup), leave, sizeof leave);
    uint8_t* toMiss = c - 1;
    uint8_t* done[2];
    if (planner)
    {
        *c++ = 0x81; *c++ = 0x38; memcpy(c, &id, 4); c += 4;               /* cmp dword [rax], id */
        *c++ = 0x74; done[0] = c++;                                         /* je done */
        *c++ = 0x81; *c++ = 0x78; *c++ = 0x18; memcpy(c, &id, 4); c += 4;  /* cmp dword [rax+0x18], id */
        *c++ = 0x74; done[1] = c++;
    }
    else
    {
        c = EgBytes(c, byRegister, sizeof byRegister);
        done[0] = c - 6; done[1] = c - 1;
    }
    *toMiss = (uint8_t)(c - (toMiss + 1));
    c = EgBytes(c, save, sizeof save);
    if (planner) { *c++ = 0xb9; memcpy(c, &id, 4); c += 4; }               /* mov ecx, id */
    else { *c++ = 0x89; *c++ = 0xd9; }                                      /* mov ecx, ebx */
    c = EgBytes(EgCall(c, (uintptr_t)EgSeatFor), restore, sizeof restore);
    uint8_t* ret = c - 1;
    for (int i = 0; i < 2; i++) *done[i] = (uint8_t)(ret - (done[i] + 1));
}

/* The three stubs in one cave near game.dll, then each call to its stub. NULL, or why not (nothing changed then). */
static const char* EnemyGuardsInstall(uintptr_t game)
{
    if (g_egState) return NULL;
    for (size_t i = 0; i < EG_CALLS; i++)
        if (!PinMatches(game + kEgCalls[i].site, kEgCalls[i].pin, 5)) return "an enemy's Behavior or transport read is not the reviewed one";
    uint8_t* cave = AllocateNearGame(game, 0x1000);
    if (cave == NULL) return "no memory near game.dll for the enemy guards";
    EgBehaviorStub(cave, game + EG_BEHAVIOR);
    EgFormationStub(cave + EG_STUB, game + EG_FORMATION, 0);
    EgFormationStub(cave + 2 * EG_STUB, game + EG_FORMATION, 1);
    DWORD previous;
    VirtualProtect(cave, 0x1000, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), cave, 0x1000);
    for (size_t i = 0; i < EG_CALLS; i++)
    {
        uint8_t call[5] = {0xe8};
        int32_t rel = (int32_t)((intptr_t)(cave + kEgCalls[i].stub * EG_STUB) - (intptr_t)(game + kEgCalls[i].site + 5));
        memcpy(call + 1, &rel, 4);
        if (JournalPut(game + kEgCalls[i].site, call, sizeof call)) return "an enemy guard could not be installed";
    }
    g_egState = 1;
    Note("enemy guards: an enemy needs no Behavior record and no transport formation of its own");
    return NULL;
}

/* The name reads go to the grown tables (their displacement from game.dll's base fits 32 bits: the tables are near it).
 * A faction that did not grow keeps the game's table. NULL, or why not. */
static const char* EnemyNamesInstall(uintptr_t game)
{
    for (uint32_t f = 0; f < ER_FACTIONS; f++)
    {
        if (g_erGrown[f].names == NULL || g_erGrown[f].rowCount == 0) continue;
        int32_t now = 0, to = (int32_t)((intptr_t)g_erGrown[f].names - (intptr_t)game);
        for (int k = 0; k < 2; k++)
        {
            if (SafeRead(game + kEgNameReads[f][k], &now, 4) || (uint32_t)now != kErNames[f]) return "an enemy group's name read is not the reviewed one";
            if (JournalPut(game + kEgNameReads[f][k], (const uint8_t*)&to, 4)) return "an enemy group's name read could not be moved";
        }
    }
    return NULL;
}

/* What the transport guard did, at most every 10 s. */
static void EnemyGuardsWatch(void)
{
    LONG seated = g_egSeated;
    if (seated == g_egSeatedReported || GetTickCount64() - g_egReportedAt < 10000) return;
    g_egReportedAt = GetTickCount64();
    g_egSeatedReported = seated;
    Note("enemy guards: %ld transport seat(s) given to enemies without a formation of their own so far", (long)seated);
}
