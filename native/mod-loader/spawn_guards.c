/* The game's crash on an enemy family with nothing to spawn, build 25480438 (game.dll+0x9530F5, reading 0x18). The
 * director picks a family's enemy by weight at the mission's level (0x953150); with no weighted row it returns -1, and
 * its callers hand the spawn (0x953580) a null enemy, which reads it unchecked. Roster edits that empty a family are
 * refused (director.c); for writes that skip that check, these guard the spawn, the spawner queue and the three pick
 * sites that read a -1's row. Included by director.c: AllocateNearGame (damage.c), PinMatches (packages.c), JournalPut
 * (live_code.c), LoaderHook and HookRemove (hooks.c). */

/* The spawn: (director, enemy row, position, count, then six on the stack). Every path reads the row (0x952FF0's cap
 * check, at +0x18 or +0xA8); its gates already refuse a spawn by returning null (0x954079). A null row is refused the
 * same way, before the function's frame: a caller that reads the result unchecked (0x6862E2) crashed on it anyway. */
#define SG_SPAWN 0x953580u
#define SG_SPAWN_UNCHECKED 0x95300Eu /* 0x952FF0: the path without a null check */
#define SG_SPAWN_READ 0x9530F5u
#define SG_SPAWN_REFUSED 0x954079u
static const uint8_t kSgSpawnPin[] = {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0xa8,0xfd,
    0xff,0xff,0x48,0x81,0xec,0x58,0x03,0x00,0x00};
static const uint8_t kSgUncheckedPin[] = {0x45,0x84,0xc0,0x0f,0x84,0xde,0x00,0x00,0x00};
static const uint8_t kSgReadPin[] = {0x44,0x8b,0x42,0x18,0x45,0x85,0xc0};
static const uint8_t kSgRefusedPin[] = {0x33,0xc0,0x48,0x8b,0x8d,0xa0,0x01,0x00,0x00};

/* A spawner group (bug hole, fabricator: 0x7F8740) spawns its queued enemies in a loop. A refused spawn ends the loop
 * without taking any off the queue, so a null at its head would be tried again forever while the queue fills. The call
 * site's cave skips a null one as a spawn the game let go: on to the next (0x7F8CDF), and the loop's end drops it. */
#define SG_QUEUE_LOAD 0x7F88B3u /* mov rax, [rdi+rax*8+0x88]; mov [rsp+0x78], rax: the queued row */
#define SG_QUEUE_ARG 0x7F8C57u  /* mov rdx, [rsp+0x78] */
#define SG_QUEUE_CALL 0x7F8CB9u /* call 0x953580, after the flags 0x7E */
#define SG_QUEUE_NEXT 0x7F8CDFu /* dec [rdi+0x214], the spawn's tail: reads no volatile register before writing it */
static const uint8_t kSgLoadPin[] = {0x48,0x8b,0x84,0xc7,0x88,0x00,0x00,0x00,0x48,0x89,0x44,0x24,0x78};
static const uint8_t kSgArgPin[] = {0x48,0x8b,0x54,0x24,0x78};
static const uint8_t kSgCallPin[] = {0xc7,0x44,0x24,0x20,0x7e,0x00,0x00,0x00, 0xe8,0xc2,0xa8,0x15,0x00, 0x48,0x8b,0xc8,0x48,0x85,0xc0,
    0x75,0x05,0x44,0x8b,0xf3};
static const uint8_t kSgNextPin[] = {0xff,0x8f,0x14,0x02,0x00,0x00,0x38,0x9e,0xa7,0x00,0x00,0x00};
#define SG_CALL_AT 8 /* the call's place in kSgCallPin */
#define SG_QUEUE_CAVE 0x40 /* the queue cave's place in the cave page, after the spawn's detour */

/* The pick (director, family key, roster, r9b): with r9b clear, the key's rows that pass the planet's tags with a
 * weight at the level, else -1; with it set, those rows whatever their weight, the first when all are 0 (0x9532C3).
 * These callers pass r9b clear and read a -1's row unchecked (0x949B80's [r8+8], 0x86950C); they ask again with it set,
 * so a family emptied by weight spawns a row of weight 0 there instead of crashing. Callers that handle -1 are left. */
#define SG_PICK 0x953150u
#define SG_PICK_ALL 0x9531DCu    /* movzx r14d, byte [rsp+0x98]: r9b */
#define SG_PICK_WEIGHT 0x953221u /* test r14b, r14b; jnz take; comiss xmm0, xmm2; jbe skip */
#define SG_PICK_FIRST 0x9532C3u  /* comiss xmm2, xmm0; jae found */
static const uint8_t kSgPickPin[] = {0x48,0x89,0x5c,0x24,0x10,0x44,0x88,0x4c,0x24,0x20,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,
    0x41,0x57,0x48,0x83,0xec,0x40};
static const uint8_t kSgAllPin[] = {0x44,0x0f,0xb6,0xb4,0x24,0x98,0x00,0x00,0x00};
static const uint8_t kSgWeightPin[] = {0x45,0x84,0xf6,0x75,0x05,0x0f,0x2f,0xc2,0x76,0x09};
static const uint8_t kSgFirstPin[] = {0x0f,0x2f,0xd0,0x0f,0x83,0xcf,0x00,0x00,0x00};
static const uint8_t kSgClearPin[] = {0x45,0x33,0xc9};          /* xor r9d, r9d (nothing writes r9 after it) */
static const uint8_t kSgUncheckedUse[] = {0x83,0xf8,0xff,0x74}; /* cmp eax, -1; je: the branch that nulls the row */
static const uint32_t kSgPickSites[][2] = {{0x802000u, 0x801FFAu}, {0x8694EAu, 0x8694D4u}, {0x90F315u, 0x90F30Cu}}; /* call, xor */
#define SG_PICK_SITES (sizeof kSgPickSites / sizeof kSgPickSites[0])
#define SG_PICK_CAVE 0x80

static int JournalPut(uintptr_t address, const uint8_t* bytes, size_t size); /* live_code.c */

static void* g_sgSpawnOriginal; /* the next in the spawn's hook chain */
static uint8_t* g_sgDetour;
static int g_sgState; /* 0 waiting for game.dll's code, 1 installed, -1 not */
static ULONGLONG g_sgLoadedAt, g_sgReportedAt;
static volatile LONG g_sgRefused, g_sgSkipped, g_sgPicked;
static LONG g_sgRefusedReported, g_sgSkippedReported, g_sgPickedReported;

static int SgPinned(uintptr_t game)
{
    return PinMatches(game + SG_SPAWN, kSgSpawnPin, sizeof kSgSpawnPin) &&
        PinMatches(game + SG_SPAWN_UNCHECKED, kSgUncheckedPin, sizeof kSgUncheckedPin) &&
        PinMatches(game + SG_SPAWN_READ, kSgReadPin, sizeof kSgReadPin) &&
        PinMatches(game + SG_SPAWN_REFUSED, kSgRefusedPin, sizeof kSgRefusedPin);
}

static int SgQueuePinned(uintptr_t game)
{
    return PinMatches(game + SG_QUEUE_LOAD, kSgLoadPin, sizeof kSgLoadPin) && PinMatches(game + SG_QUEUE_ARG, kSgArgPin, sizeof kSgArgPin) &&
        PinMatches(game + SG_QUEUE_CALL - SG_CALL_AT, kSgCallPin, sizeof kSgCallPin) &&
        PinMatches(game + SG_QUEUE_NEXT, kSgNextPin, sizeof kSgNextPin);
}

static int SgPickPinned(uintptr_t game)
{
    return PinMatches(game + SG_PICK, kSgPickPin, sizeof kSgPickPin) && PinMatches(game + SG_PICK_ALL, kSgAllPin, sizeof kSgAllPin) &&
        PinMatches(game + SG_PICK_WEIGHT, kSgWeightPin, sizeof kSgWeightPin) &&
        PinMatches(game + SG_PICK_FIRST, kSgFirstPin, sizeof kSgFirstPin);
}

/* A pick site as reviewed: r9d cleared, the call to the pick, then the -1 test. */
static int SgPickSitePinned(uintptr_t game, int site)
{
    uintptr_t call = game + kSgPickSites[site][0];
    uint8_t op = 0;
    int32_t rel = 0;
    if (SafeRead(call, &op, 1) || SafeRead(call + 1, &rel, 4)) return 0;
    return op == 0xe8 && call + 5 + rel == game + SG_PICK && PinMatches(call + 5, kSgUncheckedUse, sizeof kSgUncheckedUse) &&
        PinMatches(game + kSgPickSites[site][1], kSgClearPin, sizeof kSgClearPin);
}

static uint8_t* SgJump(uint8_t* c, uint8_t op, uintptr_t to)
{
    int32_t rel = (int32_t)((intptr_t)to - (intptr_t)(c + 5));
    *c = op;
    memcpy(c + 1, &rel, 4);
    return c + 5;
}

static uint8_t* SgImm64(uint8_t* c, uint64_t value)
{
    *c++ = 0x48; *c++ = 0xb8; /* mov rax, imm64: rax carries no argument on Win64 */
    memcpy(c, &value, 8);
    return c + 8;
}

/* The spawn's detour, in code so every argument and register passes untouched: a row goes on down the chain (its next
 * link read at each call, as an unhook changes it), a null one is refused. */
static void SgSpawnDetour(uint8_t* c)
{
    static const uint8_t test[] = {0x48,0x85,0xd2, 0x74,0x0c};       /* test rdx, rdx; jz null (past the 12 below) */
    memcpy(c, test, sizeof test); c += sizeof test;
    c = SgImm64(c, (uint64_t)(uintptr_t)&g_sgSpawnOriginal);
    *c++ = 0xff; *c++ = 0x20;                                         /* jmp [rax] */
    c = SgImm64(c, (uint64_t)(uintptr_t)&g_sgRefused);                /* null: */
    static const uint8_t refuse[] = {0xf0,0xff,0x00, 0x31,0xc0, 0xc3}; /* lock inc dword [rax]; xor eax, eax; ret */
    memcpy(c, refuse, sizeof refuse);
}

/* Entered by the loop's call (its return address on the stack): a row goes on to the spawn, a null one leaves the
 * call's frame and takes the tail. rax is dead there. */
static void SgQueueCave(uint8_t* c, uintptr_t game)
{
    static const uint8_t test[] = {0x48,0x85,0xd2, 0x74,0x05};          /* test rdx, rdx; jz null */
    memcpy(c, test, sizeof test); c += sizeof test;
    c = SgJump(c, 0xe9, game + SG_SPAWN);
    c = SgImm64(c, (uint64_t)(uintptr_t)&g_sgSkipped);                  /* null: */
    static const uint8_t skip[] = {0xf0,0xff,0x00, 0x48,0x83,0xc4,0x08}; /* lock inc dword [rax]; add rsp, 8 */
    memcpy(c, skip, sizeof skip); c += sizeof skip;
    SgJump(c, 0xe9, game + SG_QUEUE_NEXT);
}

/* Entered by a pick site's call, its arguments kept in its own frame: the pick, and on -1 the pick again with r9b set.
 * The frame keeps the game's 16-byte alignment at both calls. */
static void SgPickCave(uint8_t* c, uintptr_t game)
{
    static const uint8_t save[] = {0x48,0x83,0xec,0x38, 0x48,0x89,0x4c,0x24,0x20, 0x48,0x89,0x54,0x24,0x28, 0x4c,0x89,0x44,0x24,0x30};
    static const uint8_t again[] = {0x83,0xf8,0xff, 0x75,0x2c, /* cmp eax, -1; jne done */
        0x48,0x8b,0x4c,0x24,0x20, 0x48,0x8b,0x54,0x24,0x28, 0x4c,0x8b,0x44,0x24,0x30, 0x41,0xb9,0x01,0x00,0x00,0x00};
    static const uint8_t found[] = {0x83,0xf8,0xff, 0x74,0x0d}; /* cmp eax, -1; je done (past the 13 below) */
    static const uint8_t done[] = {0xf0,0xff,0x01, 0x48,0x83,0xc4,0x38, 0xc3}; /* lock inc dword [rcx]; done: add rsp, 0x38; ret */
    memcpy(c, save, sizeof save); c += sizeof save;
    c = SgJump(c, 0xe8, game + SG_PICK);
    memcpy(c, again, sizeof again); c += sizeof again;
    c = SgJump(c, 0xe8, game + SG_PICK);
    memcpy(c, found, sizeof found); c += sizeof found;
    *c++ = 0x48; *c++ = 0xb9; /* mov rcx, imm64: eax holds the result */
    uint64_t counter = (uint64_t)(uintptr_t)&g_sgPicked;
    memcpy(c, &counter, 8); c += 8;
    memcpy(c, done, sizeof done);
}

/* The pick sites' calls into the cave: how many were rewritten. */
static int SgPickInstall(uintptr_t game, uint8_t* cave)
{
    int sites = 0;
    if (!SgPickPinned(game)) return 0;
    for (int s = 0; s < (int)SG_PICK_SITES; s++)
    {
        if (!SgPickSitePinned(game, s)) continue;
        uint8_t call[5] = {0xe8};
        int32_t rel = (int32_t)((intptr_t)(cave + SG_PICK_CAVE) - (intptr_t)(game + kSgPickSites[s][0] + 5));
        memcpy(call + 1, &rel, 4);
        sites += !JournalPut(game + kSgPickSites[s][0], call, sizeof call);
    }
    return sites;
}

/* The spawn's hook, then the spawner loop's call and the pick sites. 1 when the spawn is guarded (the rest maybe not), 0
 * when not. */
static int SpawnGuardsInstallAt(uintptr_t game)
{
    uint8_t* cave = AllocateNearGame(game, 0x1000);
    if (cave == NULL) { Note("spawn guards: not installed; no memory near game.dll"); return 0; }
    SgSpawnDetour(cave);
    SgQueueCave(cave + SG_QUEUE_CAVE, game);
    SgPickCave(cave + SG_PICK_CAVE, game);
    DWORD previous;
    VirtualProtect(cave, 0x1000, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), cave, 0x1000);
    if (LoaderHook("spawn guard: a null enemy", game + SG_SPAWN, cave, &g_sgSpawnOriginal))
    {
        Note("spawn guards: not installed; the spawn could not be hooked");
        return 0;
    }
    g_sgDetour = cave;
    int queue = 0;
    if (SgQueuePinned(game))
    {
        uint8_t call[5] = {0xe8};
        int32_t rel = (int32_t)((intptr_t)(cave + SG_QUEUE_CAVE) - (intptr_t)(game + SG_QUEUE_CALL + 5));
        memcpy(call + 1, &rel, 4);
        queue = !JournalPut(game + SG_QUEUE_CALL, call, sizeof call);
    }
    int picks = SgPickInstall(game, cave);
    Note("spawn guards: a spawn of no enemy is refused%s; %d of %d pick sites ask again for a row of weight 0", queue ? "; a spawner skips a queued one" : "; the spawner queue is not guarded", picks, (int)SG_PICK_SITES);
    return 1;
}

/* After the game loaded (WatchStats). game.dll's code is decoded some time after; a minute later it never will be the
 * reviewed one. */
static void SpawnGuardsInstall(void)
{
    if (g_sgState != 0) return;
    if (strcmp(g_build, "25480438")) { g_sgState = -1; return; }
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0 || !SgPinned(game))
    {
        if (g_sgLoadedAt == 0) g_sgLoadedAt = GetTickCount64();
        if (GetTickCount64() - g_sgLoadedAt < 60000) return;
        g_sgState = -1;
        Note("spawn guards: not installed; the spawn's code is not the reviewed one");
        return;
    }
    g_sgState = SpawnGuardsInstallAt(game) ? 1 : -1;
}

/* What the guards caught, at most every 10 s. */
static void SpawnGuardsWatch(void)
{
    SpawnGuardsInstall();
    if (g_sgState != 1 || GetTickCount64() - g_sgReportedAt < 10000) return;
    LONG refused = g_sgRefused, skipped = g_sgSkipped, picked = g_sgPicked;
    if (refused == g_sgRefusedReported && skipped == g_sgSkippedReported && picked == g_sgPickedReported) return;
    g_sgReportedAt = GetTickCount64();
    g_sgRefusedReported = refused;
    g_sgSkippedReported = skipped;
    g_sgPickedReported = picked;
    Note("spawn guards: %ld spawn(s) of no enemy refused, %ld queued one(s) skipped and %ld row(s) of weight 0 picked so far: "
        "an enemy family has no spawn weight at this level (a roster edit or a mod's write)", (long)refused, (long)skipped, (long)picked);
}
