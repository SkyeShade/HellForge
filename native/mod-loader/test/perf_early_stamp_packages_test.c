/* The early stamp packages (perf_early_stamp_packages.c) on synthetic images: the pins, the off switch, both caves run
 * through thunks, and a generation's records stepped as the game steps them. QueueStampPackages and the engine's
 * package table are fakes that keep the game's contracts: a list per location type, the requests in list order, a load
 * ring that adds an id once. Included by stats_test.c. */

#define ESPT_GAME 0x3400000u
#define ESPT_EXE 0x610000u
#define ESPT_THUNKS 0x100000u /* free room in the game image */
#define ESPT_TYPES 100u
#define ESPT_RING 4096u

typedef void (*EsptStepCall)(uintptr_t level, uintptr_t block);
typedef void (*EsptEndCall)(uintptr_t level);

static uintptr_t g_esptGame;
static uint64_t g_esptRing[ESPT_RING], g_esptReleased[ESPT_RING];
static uint32_t g_esptRingCount, g_esptReleasedCount, g_esptDepthBase, g_esptQueueCalls, g_esptMostTypes;
static uint64_t g_esptStepSeen[2], g_esptEndSeen;
static uint32_t g_esptFaction = 0xFAC7;

/* The engine's load request: an id already queued gets no second entry (0x6036D0). */
static void EsptLoad(uint64_t id)
{
    for (uint32_t i = 0; i < g_esptRingCount; i++) if (g_esptRing[i] == id) return;
    assert(g_esptRingCount < ESPT_RING);
    g_esptRing[g_esptRingCount++] = id;
    assert(g_esptDepthBase + g_esptRingCount <= ESP_ROOM); /* the ring is never filled past the room left */
}

static void EsptRelease(const uint64_t* ids, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) { assert(g_esptReleasedCount < ESPT_RING); g_esptReleased[g_esptReleasedCount++] = ids[i]; }
}

static uint32_t EsptLoaded(uint64_t id)
{
    for (uint32_t i = 0; i < g_esptRingCount; i++) if (g_esptRing[i] == id) return 1;
    return 0;
}

static uint32_t EsptDepth(void) { return g_esptDepthBase + g_esptRingCount; }

/* A type's packages: its own id, and the id its second key names (shared by several types; none when 0). */
static uint32_t EsptTypeIds(const uint8_t* type, uint64_t ids[2])
{
    uint32_t n = 0, second = 0;
    memcpy(&ids[n++], type, 8);
    memcpy(&second, type + 8, 4);
    if (second != 0) ids[n++] = 0x5000000000000000ull | second;
    return n;
}

/* QueueStampPackages as reviewed: [pass+0x10]'s count of types from [pass], each new id into [pass+0x30] from
 * [[pass+0x38]], then a request of every listed one through the package table. The context must be the level's with
 * our arena. */
static void EsptQueue(uint8_t* pass, uintptr_t record, uint8_t* context)
{
    uint64_t types, countAt, outAt, outCountAt, arena, api, table, load;
    memcpy(&types, pass, 8); memcpy(&countAt, pass + 0x10, 8); memcpy(&outAt, pass + 0x30, 8); memcpy(&outCountAt, pass + 0x38, 8);
    memcpy(&arena, context, 8);
    assert(arena == (uint64_t)(uintptr_t)&g_espArena && g_espChunk.used == 0 && g_espArena.owner == -1);
    assert(*(uint32_t*)(context + 0xD0) == g_esptFaction);
    uint32_t count = *(uint32_t*)(uintptr_t)countAt, *outCount = (uint32_t*)(uintptr_t)outCountAt;
    uint64_t* out = (uint64_t*)(uintptr_t)outAt;
    uint32_t listed = *outCount;
    g_esptQueueCalls++;
    if (count > g_esptMostTypes) g_esptMostTypes = count;
    for (uint32_t i = 0; i < count; i++)
    {
        uint64_t ids[2];
        uint32_t n = EsptTypeIds((const uint8_t*)(uintptr_t)(types + 16ull * i), ids);
        for (uint32_t j = 0; j < n; j++)
        {
            uint32_t seen = 0;
            for (uint32_t k = 0; k < listed; k++) seen |= out[k] == ids[j];
            if (!seen) out[listed++] = ids[j];
        }
    }
    *outCount = listed;
    memcpy(&api, (void*)(g_esptGame + ESP_API), 8); memcpy(&table, (void*)(uintptr_t)(api + 0x10), 8); memcpy(&load, (void*)(uintptr_t)(table + 0x2F0), 8);
    for (uint32_t i = 0; i < listed; i++) ((void (*)(uint64_t))(uintptr_t)load)(out[i]);
}

static void EsptCommit(uint8_t* image, uint32_t from, uint32_t to)
{
    assert(VirtualAlloc(image + (from & ~0xFFFu), ((to + 0xFFFu) & ~0xFFFu) - (from & ~0xFFFu), MEM_COMMIT, PAGE_EXECUTE_READWRITE) != NULL);
}

/* mov rax, fn; jmp rax */
static void EsptJumpTo(uint8_t* at, void* fn)
{
    uint8_t* c = EspImm64(at, 0xb8, (uintptr_t)fn);
    c[0] = 0xff; c[1] = 0xe0;
}

/* A record: its op and flags, and up to two pass fields (record offsets) set. */
static void EsptRecord(uint8_t* block, uint32_t k, uint32_t op, uint64_t flags, uint32_t at1, uint64_t v1, uint32_t at2, uint64_t v2)
{
    uint8_t* r = block + (size_t)k * ESP_RECORD;
    memset(r, 0, ESP_RECORD);
    memcpy(r, &op, 4);
    memcpy(r + 0x1998, &flags, 8);
    if (at1) memcpy(r + at1, &v1, 8);
    if (at2) memcpy(r + at2, &v2, 8);
}

static uint32_t EsptReleased(uint64_t id)
{
    for (uint32_t i = 0; i < g_esptReleasedCount; i++) if (g_esptReleased[i] == id) return 1;
    return 0;
}

/* One step at `index`: the cave runs, then the game's step (the stub) gets rcx and rdx as the call site had them. */
static void EsptStep(EsptStepCall step, uintptr_t level, uint8_t* block, uint32_t index)
{
    memcpy(block + ESP_INDEX, &index, 4);
    g_esptStepSeen[0] = g_esptStepSeen[1] = 0;
    step(level, (uintptr_t)block);
    assert(g_esptStepSeen[0] == (uintptr_t)block && g_esptStepSeen[1] == 0x1234);
}

/* A generation of ten records: types written by records 0 and 1, the window 2..5 (op 0x2B at 4 holds both pointers),
 * op 0x1F at 6, its wait at 8. */
static void EsptPipeline(uint8_t* block, uint8_t* types, uint32_t* typeCount, uint64_t* out, uint32_t* outCount)
{
    uint32_t count = 10;
    memset(block, 0, ESP_COUNT);
    EsptRecord(block, 0, 0x16, 0, 0x48, (uintptr_t)types, 0, 0);
    EsptRecord(block, 1, 0x06, 0, 0x100, (uintptr_t)typeCount, 0, 0);
    EsptRecord(block, 2, 0x38, 0xB, 0, 0, 0, 0);
    EsptRecord(block, 3, 0x40, 2, 0, 0, 0, 0);
    EsptRecord(block, 4, 0x2B, 0, 0x258, (uintptr_t)types, 0x288, (uintptr_t)typeCount);
    EsptRecord(block, 5, 0x8F, 3, 0, 0, 0, 0);
    EsptRecord(block, 6, 0x1F, 2, 0x8, (uintptr_t)types, 0x18, (uintptr_t)typeCount);
    memcpy(block + 6 * ESP_RECORD + 0x38, &out, 8);
    memcpy(block + 6 * ESP_RECORD + 0x40, &outCount, 8);
    EsptRecord(block, 7, 0x1D, 0x1B, 0, 0, 0, 0);
    EsptRecord(block, 8, 0x20, 3, 0x8, (uintptr_t)out, 0x10, (uintptr_t)outCount);
    EsptRecord(block, 9, 0x31, 2, 0, 0, 0, 0);
    memcpy(block + ESP_COUNT, &count, 4);
    memcpy(block + ESP_CONTEXT + 0xD0, &g_esptFaction, 4);
    *typeCount = 0;
    *outCount = 0;
}

/* The game's order of the whole list in one call: every type's ids, each the first time it comes. */
static uint32_t EsptExpected(const uint8_t* types, uint64_t* list)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < ESPT_TYPES; i++)
    {
        uint64_t ids[2];
        uint32_t m = EsptTypeIds(types + 16 * i, ids);
        for (uint32_t j = 0; j < m; j++)
        {
            uint32_t seen = 0;
            for (uint32_t k = 0; k < n; k++) seen |= list[k] == ids[j];
            if (!seen) list[n++] = ids[j];
        }
    }
    return n;
}

static void EsptReset(void)
{
    g_esptRingCount = g_esptReleasedCount = g_esptDepthBase = g_esptQueueCalls = g_esptMostTypes = 0;
}

/* Pins, the off switch and the install on the images; the stubs and markers after it. */
static void EsptImages(uint8_t* game, uint8_t* exe)
{
    for (size_t i = 0; i < ESP_PINS; i++) memcpy((kEspPins[i].game ? game : exe) + kEspPins[i].rva, kEspPins[i].bytes, kEspPins[i].size);
    static uint8_t api[0x20], table[0x400];
    uintptr_t at = (uintptr_t)api, tableAt = (uintptr_t)table;
    memcpy(game + ESP_API, &at, 8);
    memcpy(api + 0x10, &tableAt, 8);
    for (int s = 0; s < ESP_SLOTS; s++) { uintptr_t fn = (uintptr_t)exe + kEspSlots[s].rva; memcpy(table + kEspSlots[s].slot, &fn, 8); }
    g_espGame = (uintptr_t)game; g_espExe = (uintptr_t)exe;
    assert(EspNotReviewed((uintptr_t)exe, (uintptr_t)game) == NULL);
    game[0xF40DE3 + 0x1A] ^= 1;
    assert(EspNotReviewed((uintptr_t)exe, (uintptr_t)game) != NULL); /* another build's code is left alone */
    game[0xF40DE3 + 0x1A] ^= 1;
    exe[0x603746 + 0x40] ^= 1;
    assert(EarlyStampPackagesInstallAt((uintptr_t)exe, (uintptr_t)game) == 0);
    exe[0x603746 + 0x40] ^= 1;
    uintptr_t slot = (uintptr_t)exe + 0x321330u;
    memcpy(table + 0x310, &(uintptr_t){1}, 8);
    assert(EarlyStampPackagesInstallAt((uintptr_t)exe, (uintptr_t)game) == 0); /* a swapped slot: not the reviewed table */
    memcpy(table + 0x310, &slot, 8);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "safe-read, early-stamp-packages");
    assert(EarlyStampPackagesInstallAt((uintptr_t)exe, (uintptr_t)game) == -1);
    assert(memcmp(game + 0xF74090, kEspStepPin, sizeof kEspStepPin) == 0 && memcmp(game + 0xF74430, kEspEndPin, sizeof kEspEndPin) == 0);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    assert(EarlyStampPackagesInstallAt((uintptr_t)exe, (uintptr_t)game) == 1);
    assert(game[ESP_STEP_CALL] == 0xe8 && memcmp(game + ESP_STEP_CALL + 1, kEspStepPin + 26, 4) != 0);
    assert(game[ESP_END_CALL] == 0xe8 && memcmp(game + ESP_END_CALL + 1, kEspEndPin + 27, 4) != 0);
    EsptJumpTo(exe + 0x320DD0u, (void*)EsptLoad);
    EsptJumpTo(exe + 0x321050u, (void*)EsptRelease);
    EsptJumpTo(exe + 0x321330u, (void*)EsptLoaded);
    EsptJumpTo(exe + 0x321770u, (void*)EsptDepth);
    EsptJumpTo(game + ESP_QUEUE, (void*)EsptQueue);
    /* the step and the release record what they were given; the markers end the thunks' frames */
    static const uint8_t keep2[] = {0x48,0x89,0x08, 0x48,0x89,0x50,0x08, 0xc3}, keep1[] = {0x48,0x89,0x08, 0xc3};
    static const uint8_t leave[] = {0x48,0x83,0xc4,0x20, 0x5b, 0xc3};
    memcpy(EspImm64(game + ESP_STEP, 0xb8, (uintptr_t)g_esptStepSeen), keep2, sizeof keep2);
    memcpy(EspImm64(game + ESP_END, 0xb8, (uintptr_t)&g_esptEndSeen), keep1, sizeof keep1);
    memcpy(game + ESP_STEP_CALL + 5, leave, sizeof leave);
    memcpy(game + ESP_END_CALL + 5, leave, sizeof leave);
    /* push rbx; sub rsp, 0x20; rbx = level; then rcx = records, edx = 0x1234 for the step, rcx = level for the end */
    static const uint8_t step[] = {0x53, 0x48,0x83,0xec,0x20, 0x48,0x89,0xcb, 0x48,0x89,0xd1, 0xba,0x34,0x12,0x00,0x00};
    static const uint8_t end[] = {0x53, 0x48,0x83,0xec,0x20, 0x48,0x89,0xcb};
    memcpy(game + ESPT_THUNKS, step, sizeof step);
    EspRel32(game + ESPT_THUNKS + sizeof step, 0xe9, (uintptr_t)game + ESP_STEP_CALL);
    memcpy(game + ESPT_THUNKS + 0x40, end, sizeof end);
    EspRel32(game + ESPT_THUNKS + 0x40 + sizeof end, 0xe9, (uintptr_t)game + ESP_END_CALL);
    FlushInstructionCache(GetCurrentProcess(), game, ESPT_GAME);
}

static void EarlyStampPackagesFixture(void)
{
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, ESPT_GAME, MEM_RESERVE, PAGE_NOACCESS);
    uint8_t* exe = (uint8_t*)VirtualAlloc(NULL, ESPT_EXE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    uint8_t* block = (uint8_t*)VirtualAlloc(NULL, ESP_CONTEXT + 0x100, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(game != NULL && exe != NULL && block != NULL);
    EsptCommit(game, ESPT_THUNKS, ESPT_THUNKS + 0x100);
    EsptCommit(game, 0xC14000u, 0xF75000u);
    EsptCommit(game, 0x173B000u, 0x173C000u);
    EsptCommit(game, ESP_API, ESP_API + 8);
    g_esptGame = (uintptr_t)game;
    EsptImages(game, exe);
    EsptStepCall step = (EsptStepCall)(void*)(game + ESPT_THUNKS);
    EsptEndCall end = (EsptEndCall)(void*)(game + ESPT_THUNKS + 0x40);

    static uint8_t types[ESPT_TYPES * 16];
    static uint64_t out[256], expected[256];
    static uint32_t typeCount, outCount;
    for (uint32_t i = 0; i < ESPT_TYPES; i++)
    {
        uint64_t own = 0x1000 + i;
        uint32_t shared = i % 3 == 0 ? 0 : 0x200 + i % 50;
        memcpy(types + 16 * i, &own, 8);
        memcpy(types + 16 * i + 8, &shared, 4);
    }
    uint32_t expectedCount = EsptExpected(types, expected);
    _Alignas(16) uint8_t levelData[0x300] = {0};
    uintptr_t level = (uintptr_t)levelData, blockAt = (uintptr_t)block;
    memcpy(levelData + ESP_LEVEL_RECORDS, &blockAt, 8);

    /* the whole list, before op 0x1F, in the game's order; its own call then finds every one listed */
    EsptReset();
    EsptPipeline(block, types, &typeCount, out, &outCount);
    EsptStep(step, level, block, 0);
    assert(g_esp.state == ESP_WAIT && g_esp.from == 2 && g_esp.at1F == 6);
    typeCount = ESPT_TYPES; /* records 0 and 1 have run */
    EsptStep(step, level, block, 1);
    assert(g_esptQueueCalls == 0);
    EsptStep(step, level, block, 2);
    assert(g_esp.state == ESP_HELD && g_esp.done == ESPT_TYPES && g_esp.count == expectedCount);
    assert(g_esptQueueCalls == 2 && g_esptMostTypes == (ESP_ROOM / ESP_PER_TYPE)); /* two chunks: the ring's room */
    assert(g_esptRingCount == expectedCount && memcmp(g_esptRing, expected, expectedCount * 8) == 0);
    EsptStep(step, level, block, 3);
    assert(g_esptQueueCalls == 2);
    memcpy(out, expected, expectedCount * 8);
    outCount = expectedCount; /* record 6, the game's op 0x1F */
    EsptStep(step, level, block, 7);
    assert(g_esp.state == ESP_DONE && g_esptReleasedCount == 0);
    g_esptEndSeen = 0;
    end(level);
    assert(g_esptEndSeen == level && g_esptReleasedCount == 0 && g_esp.state == ESP_NEW);

    /* op 0x1F's list lacks three of ours: those three go */
    EsptReset();
    EsptPipeline(block, types, &typeCount, out, &outCount);
    EsptStep(step, level, block, 0);
    typeCount = ESPT_TYPES;
    EsptStep(step, level, block, 2);
    memcpy(out, expected + 3, (expectedCount - 3) * 8);
    outCount = expectedCount - 3;
    EsptStep(step, level, block, 9);
    assert(g_esptReleasedCount == 3 && EsptReleased(expected[0]) && EsptReleased(expected[1]) && EsptReleased(expected[2]));
    end(level);

    /* the level ends before op 0x1F: everything requested goes, once */
    EsptReset();
    EsptPipeline(block, types, &typeCount, out, &outCount);
    EsptStep(step, level, block, 0);
    typeCount = ESPT_TYPES;
    EsptStep(step, level, block, 3);
    g_esptEndSeen = 0;
    end(level);
    assert(g_esptEndSeen == level && g_esptReleasedCount == expectedCount && g_esp.state == ESP_NEW);
    end(level);
    assert(g_esptReleasedCount == expectedCount);

    /* a full ring waits; a later step has room. A generation that starts over without an end releases what it held */
    EsptReset();
    EsptPipeline(block, types, &typeCount, out, &outCount);
    EsptStep(step, level, block, 0);
    typeCount = ESPT_TYPES;
    g_esptDepthBase = ESP_ROOM - ESP_PER_TYPE + 1;
    EsptStep(step, level, block, 2);
    assert(g_esptQueueCalls == 0 && g_esp.state == ESP_WAIT);
    g_esptDepthBase = ESP_ROOM - 4 * ESP_PER_TYPE;
    EsptStep(step, level, block, 3);
    assert(g_esptQueueCalls >= 1 && g_esp.state == ESP_HELD && g_esp.done >= 4 && g_esp.done < ESPT_TYPES);
    assert(memcmp(g_esptRing, expected, g_esptRingCount * 8) == 0);
    g_esptDepthBase = 0;
    EsptStep(step, level, block, 5);
    assert(g_esp.done == ESPT_TYPES && g_esp.count == expectedCount);
    EsptStep(step, level, block, 0);
    assert(g_esptReleasedCount == expectedCount && g_esp.state == ESP_WAIT);
    end(level);

    /* another op holding the count after op 0x2B: no step is left before op 0x1F, nothing is requested */
    EsptReset();
    EsptPipeline(block, types, &typeCount, out, &outCount);
    memcpy(block + 5 * ESP_RECORD + 0x30, &(uintptr_t){(uintptr_t)&typeCount}, 8);
    typeCount = ESPT_TYPES;
    for (uint32_t i = 0; i < 10; i++) EsptStep(step, level, block, i);
    assert(g_esptQueueCalls == 0 && g_esp.state == ESP_DONE);
    end(level);

    /* no op 0x2B before op 0x1F (the briefing's drop map has neither): nothing */
    EsptReset();
    EsptPipeline(block, types, &typeCount, out, &outCount);
    EsptRecord(block, 4, 0x2A, 0, 0, 0, 0, 0);
    typeCount = ESPT_TYPES;
    for (uint32_t i = 0; i < 10; i++) EsptStep(step, level, block, i);
    assert(g_esptQueueCalls == 0 && g_esp.state == ESP_DONE);
    end(level);

    /* a slot of the package table swapped by someone else: the step and the end leave everything alone */
    EsptReset();
    EsptPipeline(block, types, &typeCount, out, &outCount);
    uintptr_t api = 0, table = 0, depth = 0;
    memcpy(&api, game + ESP_API, 8); memcpy(&table, (void*)(api + 0x10), 8); memcpy(&depth, (void*)(table + 0x358), 8);
    memcpy((void*)(table + 0x358), &(uintptr_t){(uintptr_t)EsptDepth}, 8);
    typeCount = ESPT_TYPES;
    for (uint32_t i = 0; i < 10; i++) EsptStep(step, level, block, i);
    end(level);
    assert(g_esptQueueCalls == 0 && g_esptReleasedCount == 0);
    memcpy((void*)(table + 0x358), &depth, 8);
    puts("PASS early stamp packages: pins, the off switch, both caves keep the game's registers, the list requested after the last op holding the location types in the game's order and in chunks the load ring has room for, handed over to op 0x1F's own list (extras released), released when the level ends first or another generation begins, refused when another op holds the types after op 0x2B, nothing without op 0x2B or with a swapped table slot");
}
