/* The edge-batch fix (enemy_pools.c) on a synthetic game image: the pass is hooked for real, and a stand-in for its body
 * appends the queries as the game's does (count + 1, the entry at count * 0x65C + 8, index + 1 into the agent's block).
 * Included by stats_test.c. */

#define EBT_GAME 0x3480000u
#define EBT_AGENTS 700u
#define EBT_PAGE 4u /* the agents' blocks: handle (4 << 16) | k, past the component's fields */
#define EBT_COMPONENT (0x8000u + EBT_AGENTS * 8u)
#define EBT_WORLD (8u + EB_GAME_ROOM * EB_STRIDE + 0x2000u) /* the batch, its job table and the HUD's state behind it */

static uint8_t* g_ebtGame;
static uint8_t* g_ebtComponent;
static int g_ebtCalls;

static uint32_t* EbtBlock(uint32_t k) { return (uint32_t*)(g_ebtComponent + (((EBT_PAGE - 1u) * 0x486u + 3u + k) * 8u)); }
static uint8_t* EbtEntry(uintptr_t batch, uint32_t i) { return (uint8_t*)batch + 8 + (uintptr_t)i * EB_STRIDE; }

/* The pass's body after the hook's moved instructions (mov r11, rsp; push rbp; push rsi), which the jump's pops undo. */
static void EbtPassBody(uintptr_t component)
{
    g_ebtCalls++;
    uint32_t* batch = *(uint32_t**)(component + EB_COMPONENT_BATCH);
    uint8_t* records = *(uint8_t**)(component + EB_COMPONENT_RECORDS);
    for (uint32_t k = 0; k < *(uint32_t*)(component + EB_COMPONENT_COUNT); k++)
    {
        uint32_t handle = *(uint32_t*)(records + k * 0xA4u + 0x54u);
        if (handle == 0xFFFFFFFFu) continue;
        uint32_t i = batch[0]++;
        uint8_t* e = (uint8_t*)batch + 8 + (uintptr_t)i * EB_STRIDE;
        memset(e, 0, 0x60);
        memcpy(e, &k, 4);
        ((uint32_t*)(component + (((uintptr_t)(handle >> 16) - 1u) * 0x486u + 3u + (handle & 0xFFFFu)) * 8u))[1] = batch[0];
    }
}

static void EbtAgents(uint32_t count, uint32_t without)
{
    uint8_t* records = *(uint8_t**)(g_ebtComponent + EB_COMPONENT_RECORDS);
    for (uint32_t k = 0; k < EBT_AGENTS; k++)
    {
        *(uint32_t*)(records + k * 0xA4u + 0x54u) = k % without == without - 1 ? 0xFFFFFFFFu : (EBT_PAGE << 16) | k;
        EbtBlock(k)[1] = 0xDEAD;
    }
    *(uint32_t*)(g_ebtComponent + EB_COMPONENT_COUNT) = count;
}

static void EbtPass(void) { ((EbPassFn)(void*)(g_ebtGame + EB_PASS))((uintptr_t)g_ebtComponent); }

/* Every agent with a block has its own query in `batch`, from `first` on, in order; the rest kept their marker. */
static void EbtCheck(uintptr_t batch, uint32_t count, uint32_t without, uint32_t first, uint32_t room)
{
    uint32_t i = first;
    for (uint32_t k = 0; k < count; k++)
    {
        if (k % without == without - 1) { assert(EbtBlock(k)[1] == 0xDEAD); continue; }
        if (i >= room) { assert(EbtBlock(k)[1] == 0); continue; }
        uint32_t agent;
        memcpy(&agent, EbtEntry(batch, i), 4);
        assert(agent == k && EbtBlock(k)[1] == i + 1);
        i++;
    }
    assert(((uint32_t*)batch)[0] == (i < room ? i : room));
}

static void EdgeBatchFixture(void)
{
    g_ebtGame = (uint8_t*)VirtualAlloc(NULL, EBT_GAME, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    g_ebtComponent = (uint8_t*)VirtualAlloc(NULL, EBT_COMPONENT, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t* world = (uint8_t*)VirtualAlloc(NULL, 2 * EBT_WORLD, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t* records = (uint8_t*)VirtualAlloc(NULL, EBT_AGENTS * 0xA4u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(g_ebtGame != NULL && g_ebtComponent != NULL && world != NULL && records != NULL);
    uintptr_t game = (uintptr_t)g_ebtGame;
    for (size_t i = 0; i < EB_PINS; i++) memcpy(g_ebtGame + kEbPins[i].rva, kEbPins[i].bytes, kEbPins[i].size);
    assert(EdgeBatchMismatch(game) == NULL);
    for (size_t i = 0; i < EB_PINS; i++)
    {
        g_ebtGame[kEbPins[i].rva + kEbPins[i].size - 1] ^= 1;
        assert(EdgeBatchMismatch(game) == kEbPins[i].what);
        g_ebtGame[kEbPins[i].rva + kEbPins[i].size - 1] ^= 1;
    }

    /* refused: switched off, then code not yet decoded and never decoded; nothing is hooked */
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "vehicle-box,edge-batch");
    assert(EdgeBatchTry(game, 0) == -1 && g_ebBatch == NULL);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    g_ebtGame[0xAB58DBu + 9] ^= 1;
    assert(EdgeBatchTry(game, 0) == 0 && EdgeBatchTry(game, 1) == -1 && g_ebBatch == NULL);
    g_ebtGame[0xAB58DBu + 9] ^= 1;
    assert(g_ebtGame[EB_PASS] == 0x4c && g_ebNext == NULL);

    assert(EdgeBatchTry(game, 0) == 1 && g_ebBatch != NULL && g_ebCapacity == EB_CAPACITY);
    assert(EdgeBatchMismatch(game) == NULL); /* the pins read through the hook */
    static const uint8_t pops[] = {0x5e, 0x5d}; /* pop rsi; pop rbp */
    memcpy(g_ebtGame + EB_PASS + 5, pops, sizeof pops);
    VbtFar(g_ebtGame + EB_PASS + 7, (void*)EbtPassBody);
    FlushInstructionCache(GetCurrentProcess(), g_ebtGame, EBT_GAME);

    /* the world's set-up: its batch in the component and the pointer, its navmesh at +4 */
    uintptr_t w = (uintptr_t)world, ours = (uintptr_t)g_ebBatch;
    *(uintptr_t*)(g_ebtComponent + EB_COMPONENT_RECORDS) = (uintptr_t)records;
    *(uintptr_t*)(g_ebtComponent + EB_COMPONENT_BATCH) = w;
    *(uintptr_t*)(g_ebtGame + EB_POINTER) = w;
    ((uint32_t*)world)[1] = 77;
    memset(world + 8, 0xA5, EBT_WORLD - 8);
    uint8_t* behind = world + 8 + EB_GAME_ROOM * EB_STRIDE; /* where the game's 257th query went */

    /* a few agents: ours holds the queries, the world batch only the count, both pointers ours */
    EbtAgents(100, 7);
    EbtPass();
    EbtCheck(ours, 100, 7, 0, EB_CAPACITY);
    assert(((uint32_t*)world)[0] == ((uint32_t*)g_ebBatch)[0] && ((uint32_t*)g_ebBatch)[1] == 77);
    assert(*(uintptr_t*)(g_ebtComponent + EB_COMPONENT_BATCH) == ours && *(uintptr_t*)(g_ebtGame + EB_POINTER) == ours);
    assert(world[8] == 0xA5 && world[EBT_WORLD - 1] == 0xA5);

    /* after the game's reset, 700 agents: past 256, and nothing behind the world batch is written */
    ((uint32_t*)world)[0] = 0;
    EbtAgents(EBT_AGENTS, 50);
    EbtPass();
    EbtCheck(ours, EBT_AGENTS, 50, 0, EB_CAPACITY);
    assert(((uint32_t*)world)[0] == 686 && behind[0] == 0xA5 && behind[0x2000 - 1] == 0xA5);

    /* without a reset the queries follow on, as in the game */
    ((uint32_t*)world)[0] = 5;
    EbtAgents(10, 1000);
    EbtPass();
    EbtCheck(ours, 10, 1000, 5, EB_CAPACITY);
    assert(((uint32_t*)world)[0] == 15);

    /* more agents than our room: the rest get no query (index 0), and the count stops at the room */
    g_ebCapacity = 60;
    ((uint32_t*)world)[0] = 0;
    EbtAgents(80, 1000);
    LONG shortBefore = g_ebShort;
    EbtPass();
    EbtCheck(ours, 80, 1000, 0, 60);
    assert(((uint32_t*)world)[0] == 60 && g_ebShort == shortBefore + 1 && *(uint32_t*)(g_ebtComponent + EB_COMPONENT_COUNT) == 80);
    g_ebCapacity = EB_CAPACITY;

    /* the world torn down (the pointer 0): the queries go to ours, nothing reads them, the pointer stays 0 */
    *(uintptr_t*)(g_ebtGame + EB_POINTER) = 0;
    ((uint32_t*)world)[0] = 3;
    EbtAgents(20, 1000);
    EbtPass();
    EbtCheck(ours, 20, 1000, 0, EB_CAPACITY);
    assert(*(uintptr_t*)(g_ebtGame + EB_POINTER) == 0 && ((uint32_t*)world)[0] == 3);

    /* a new world sets up its own batch: taken over the same way */
    uint8_t* world2 = world + EBT_WORLD;
    *(uintptr_t*)(g_ebtComponent + EB_COMPONENT_BATCH) = (uintptr_t)world2;
    *(uintptr_t*)(g_ebtGame + EB_POINTER) = (uintptr_t)world2;
    ((uint32_t*)world2)[0] = 0;
    ((uint32_t*)world2)[1] = 78;
    EbtAgents(300, 1000);
    EbtPass();
    EbtCheck(ours, 300, 1000, 0, EB_CAPACITY);
    assert(((uint32_t*)world2)[0] == 300 && ((uint32_t*)g_ebBatch)[1] == 78 && g_ebWorld == (uintptr_t)world2);

    /* a batch that is not the pointer's (a mod's): left to the game */
    static uint8_t foreign[8 + 20 * EB_STRIDE];
    memset(foreign, 0, sizeof foreign);
    *(uintptr_t*)(g_ebtComponent + EB_COMPONENT_BATCH) = (uintptr_t)foreign;
    LONG other = g_ebOther;
    EbtAgents(20, 1000);
    EbtPass();
    EbtCheck((uintptr_t)foreign, 20, 1000, 0, EB_CAPACITY);
    assert(g_ebOther == other + 1 && *(uintptr_t*)(g_ebtGame + EB_POINTER) == ours);
    assert(g_ebtCalls == 7);
}
