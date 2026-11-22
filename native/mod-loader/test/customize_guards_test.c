/* The customize guards (customize_guards.c) on a synthetic game.dll image: the scene pass's loop and the sort order are
 * the game's bytes, with markers where the game goes on, so the cave and the hooked order are executed. */

#define CG_IMAGE 0x1A06000u
#define CG_THUNK 0x1A05000u

typedef uint64_t (*CgEntryCall)(void* entry);

static void CustomizeGuardsFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, CG_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    uintptr_t game = (uintptr_t)image;
    memcpy(image + CG_LOOP, kCgLoopPin, sizeof kCgLoopPin);
    assert(PinMatches(game + CG_LOOP, kCgLoopPin, sizeof kCgLoopPin));
    image[CG_READ + 3] ^= 1;
    assert(!PinMatches(game + CG_LOOP, kCgLoopPin, sizeof kCgLoopPin)); /* another build's code is left alone */
    image[CG_READ + 3] ^= 1;
    static const uint8_t test[] = {0x89,0xd0, 0xc3};                /* the type test: mov eax, edx; ret */
    static const uint8_t next[] = {0xb8,0x51,0x51,0x00,0x00, 0xc3}; /* the next entry: mov eax, 0x5151; ret */
    memcpy(image + CG_BACK, test, sizeof test);
    memcpy(image + CG_NEXT, next, sizeof next);
    static const uint8_t enter[] = {0x48,0x89,0xc8}; /* mov rax, rcx: the item, as the loop holds it */
    memcpy(image + CG_THUNK, enter, sizeof enter);
    CgJump(image + CG_THUNK + sizeof enter, game + CG_READ);
    /* the order: the game's first instructions, then the types compared (-1, 0, 1) */
    memcpy(image + CG_ORDER, kCgOrderPin, sizeof kCgOrderPin);
    static const uint8_t order[] = {0x72,0x07, 0x77,0x0b, 0x31,0xc0, 0xc3, 0x90,0x90, /* jb less; ja more; xor eax, eax; ret */
        0xb8,0xff,0xff,0xff,0xff, 0xc3, 0xb8,0x01,0x00,0x00,0x00, 0xc3};
    memcpy(image + CG_ORDER + sizeof kCgOrderPin, order, sizeof order);

    assert(CustomizeGuardsInstallAt(game));
    assert(image[CG_READ] == 0xe9 && image[CG_READ + 5] == 0x90);
    FlushInstructionCache(GetCurrentProcess(), image, CG_IMAGE);

    /* the scene pass: a typed item reaches the type test, one with no type goes on to the next entry */
    CgEntryCall entry = (CgEntryCall)(void*)(image + CG_THUNK);
    uint32_t type3 = 3, type5 = 5;
    void* typed[8] = {0};
    typed[6] = &type3; /* +0x30 */
    void* other[8] = {0};
    other[6] = &type5;
    void* untyped[8] = {0};
    LONG skipped = g_cgSkipped;
    assert(entry(typed) == 3 && g_cgSkipped == skipped);
    assert(entry(untyped) == 0x5151 && g_cgSkipped == skipped + 1);

    /* the sort order, through its hook: typed cards by the game's order, a card with no item or type last */
    CgOrderFn compare = (CgOrderFn)(void*)(image + CG_ORDER);
    void* cardTyped[2] = {typed, 0}, *cardOther[2] = {other, 0}, *cardUntyped[2] = {untyped, 0}, *cardEmpty[2] = {0, 0};
    LONG sorted = g_cgSorted;
    assert(compare(cardTyped, cardOther) == -1 && compare(cardOther, cardTyped) == 1 && g_cgSorted == sorted);
    assert(compare(cardTyped, cardEmpty) == -1 && compare(cardEmpty, cardTyped) == 1);
    assert(compare(cardUntyped, cardOther) == 1 && compare(cardEmpty, cardUntyped) == 0 && g_cgSorted == sorted + 4);
    assert(HookRemove(NULL, game + CG_ORDER, (void*)CgOrderDetour) == 0);
    puts("PASS customize guards: pinned, a card with no type skipped in the scene pass, a card with no item or type "
        "sorted last and typed cards in the game's order");
}
