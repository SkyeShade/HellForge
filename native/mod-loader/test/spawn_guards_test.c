/* The spawn guards (spawn_guards.c) run on a synthetic game.dll image: the spawn is hooked for real, and small markers
 * stand where the game goes on, so the hook and the spawner loop's cave are executed. A null enemy is refused (null
 * back) and skipped in the spawner loop; a real one reaches the spawn with every argument. Included by stats_test.c. */

#define SG_IMAGE 0x960000u
#define SG_THUNK 0x900000u /* free room in the image */
#define SG_PICK_THUNK 0x910000u

typedef uint64_t (*SgLoopCall)(const void* row, uint64_t unused, uint64_t tenth);
typedef int32_t (*SgPickCall)(void* director, uint32_t key, void* roster, uint64_t all);
typedef void* (*SgSpawnFn)(void* director, const void* row, void* position, uint64_t count, uint64_t flags, uint64_t a6,
    uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10); /* the spawn's ten arguments */

static void SgPut(uint8_t* image, uint32_t rva, const uint8_t* bytes, size_t size) { memcpy(image + rva, bytes, size); }

static void SpawnGuardsFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, SG_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    uintptr_t game = (uintptr_t)image;
    SgPut(image, SG_SPAWN, kSgSpawnPin, sizeof kSgSpawnPin);
    SgPut(image, SG_SPAWN_UNCHECKED, kSgUncheckedPin, sizeof kSgUncheckedPin);
    SgPut(image, SG_SPAWN_READ, kSgReadPin, sizeof kSgReadPin);
    SgPut(image, SG_SPAWN_REFUSED, kSgRefusedPin, sizeof kSgRefusedPin);
    SgPut(image, SG_QUEUE_LOAD, kSgLoadPin, sizeof kSgLoadPin);
    SgPut(image, SG_QUEUE_ARG, kSgArgPin, sizeof kSgArgPin);
    SgPut(image, SG_QUEUE_CALL - SG_CALL_AT, kSgCallPin, sizeof kSgCallPin);
    SgPut(image, SG_QUEUE_NEXT, kSgNextPin, sizeof kSgNextPin);
    /* the spawn's body after its prologue: the frame undone, then row + its tenth argument (the last on the stack) */
    static const uint8_t body[] = {0x48,0x81,0xc4,0x58,0x03,0x00,0x00, 0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5b,0x5d,
        0x48,0x8b,0x44,0x24,0x50, 0x48,0x01,0xd0, 0xc3};
    SgPut(image, SG_SPAWN + sizeof kSgSpawnPin, body, sizeof body);
    /* the pick: a family 0xF00D emptied by weight (-1, its first row 7 when asked for every row), any other key its own */
    SgPut(image, SG_PICK, kSgPickPin, sizeof kSgPickPin);
    SgPut(image, SG_PICK_ALL, kSgAllPin, sizeof kSgAllPin);
    SgPut(image, SG_PICK_WEIGHT, kSgWeightPin, sizeof kSgWeightPin);
    SgPut(image, SG_PICK_FIRST, kSgFirstPin, sizeof kSgFirstPin);
    static const uint8_t pick[] = {0x48,0x83,0xc4,0x40, 0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d, /* the frame undone */
        0x81,0xfa,0x0d,0xf0,0x00,0x00, 0x74,0x03, 0x89,0xd0, 0xc3,                   /* cmp edx, 0xF00D; je; mov eax, edx; ret */
        0x41,0x0f,0xb6,0xc1, 0x8d,0x04,0xc5,0xff,0xff,0xff,0xff, 0xc3};             /* movzx eax, r9b; lea eax, [rax*8-1]; ret */
    SgPut(image, SG_PICK + sizeof kSgPickPin, pick, sizeof pick);
    for (int s = 0; s < (int)SG_PICK_SITES; s++)
    {
        SgJump(image + kSgPickSites[s][0], 0xe8, game + SG_PICK);
        SgPut(image, kSgPickSites[s][0] + 5, kSgUncheckedUse, sizeof kSgUncheckedUse);
        SgPut(image, kSgPickSites[s][1], kSgClearPin, sizeof kSgClearPin);
    }
    assert(SgPinned(game) && SgQueuePinned(game) && SgPickPinned(game) && SgPickSitePinned(game, 1));
    image[kSgPickSites[1][1]] ^= 1;
    assert(!SgPickSitePinned(game, 1));
    image[kSgPickSites[1][1]] ^= 1;
    image[SG_SPAWN_READ + 3] ^= 1;
    assert(!SgPinned(game)); /* another build's code is left alone */
    image[SG_SPAWN_READ + 3] ^= 1;
    image[SG_QUEUE_CALL + 1] ^= 1;
    assert(!SgQueuePinned(game));
    image[SG_QUEUE_CALL + 1] ^= 1;

    assert(SpawnGuardsInstallAt(game));
    assert(image[SG_QUEUE_CALL] == 0xe8 && memcmp(image + SG_QUEUE_CALL + 1, kSgCallPin + SG_CALL_AT + 1, 4) != 0);

    /* the spawn, through its hook: a null enemy refused, a real one on to the spawn with the stack arguments intact */
    SgSpawnFn spawn = (SgSpawnFn)(void*)(image + SG_SPAWN);
    uint8_t row[0x100] = {0};
    LONG refused = g_sgRefused, skipped = g_sgSkipped;
    assert((uintptr_t)spawn(NULL, row, NULL, 1, 0x7E, 0, 0, 0, 0, 5) == (uintptr_t)row + 5 && g_sgRefused == refused);
    assert(spawn(NULL, NULL, NULL, 1, 0x7E, 0, 0, 0, 0, 5) == NULL && g_sgRefused == refused + 1);

    /* the spawner loop's call, entered as the game does (the row in rdx, a 0x58 frame holding the tenth argument): a real
     * row comes back after the call (0x7F8CBE), a null one skips to the loop's tail (0x7F8CDF) with the call's frame gone */
    static const uint8_t back[] = {0x48,0x83,0xc4,0x58, 0xc3};                         /* add rsp, 0x58; ret */
    static const uint8_t tail[] = {0xb8,0x51,0x51,0x00,0x00, 0x48,0x83,0xc4,0x58, 0xc3}; /* mov eax, 0x5151; same */
    SgPut(image, SG_QUEUE_CALL + 5, back, sizeof back);
    SgPut(image, SG_QUEUE_NEXT, tail, sizeof tail);
    static const uint8_t enter[] = {0x48,0x83,0xec,0x58, 0x4c,0x89,0x44,0x24,0x48, 0x48,0x89,0xca}; /* sub rsp; mov [rsp+0x48], r8; mov rdx, rcx */
    SgPut(image, SG_THUNK, enter, sizeof enter);
    SgJump(image + SG_THUNK + sizeof enter, 0xe9, game + SG_QUEUE_CALL);
    FlushInstructionCache(GetCurrentProcess(), image, SG_IMAGE);
    SgLoopCall loop = (SgLoopCall)(void*)(image + SG_THUNK);
    assert(loop(row, 0, 9) == (uintptr_t)row + 9 && g_sgSkipped == skipped && g_sgRefused == refused + 1);
    assert(loop(NULL, 0, 9) == 0x5151 && g_sgSkipped == skipped + 1 && g_sgRefused == refused + 1);

    /* each pick site, called as the game does (r9 clear, aligned): a family with no weight gets its row of weight 0 */
    SgPickCall direct = (SgPickCall)(void*)(image + SG_PICK);
    for (int s = 0; s < (int)SG_PICK_SITES; s++)
    {
        assert(image[kSgPickSites[s][0]] == 0xe8);
        image[kSgPickSites[s][0] + 5] = 0xc3; /* back to the thunk after the call */
        uint8_t* thunk = image + SG_PICK_THUNK + 0x20 * s;
        static const uint8_t frame[] = {0x48,0x83,0xec,0x20}, unframe[] = {0x48,0x83,0xc4,0x20, 0xc3};
        memcpy(thunk, frame, sizeof frame);
        SgJump(thunk + sizeof frame, 0xe8, game + kSgPickSites[s][0]);
        memcpy(thunk + sizeof frame + 5, unframe, sizeof unframe);
        FlushInstructionCache(GetCurrentProcess(), image, SG_IMAGE);
        SgPickCall site = (SgPickCall)(void*)thunk;
        LONG picked = g_sgPicked;
        assert(site(NULL, 5, NULL, 0) == 5 && g_sgPicked == picked);
        assert(site(NULL, 0xF00D, NULL, 0) == 7 && g_sgPicked == picked + 1);
    }
    assert(direct(NULL, 0xF00D, NULL, 0) == -1); /* other callers still see -1 */

    assert(HookRemove(NULL, game + SG_SPAWN, g_sgDetour) == 0);
    assert((uintptr_t)spawn(NULL, NULL, NULL, 1, 0x7E, 0, 0, 0, 0, 5) == 5 && g_sgRefused == refused + 1); /* unhooked: the game's own */
    puts("PASS spawn guards: pins, a null enemy refused at the hooked spawn and a real one passed with its stack arguments, "
        "the spawner loop skipping a null one to its tail and calling the spawn for a real one, each pick site asking again "
        "for a row of weight 0");
}
