/* Room for 1,024 new stratagems (stratagems.c): the type guards run as machine code, the picker's frame by its formula,
 * the co-op field's width, and a boot install of all 1,024. Included by stats_test.c after live_content_test.c
 * (ContentGame, ContentMod). */

/* The stratagem type guards (GuardStratagemTypes), run as machine code: each guarded instruction sits at its RVA in a
 * fake game.dll, inside a small function that loads its registers from the arguments; the guard sends it through its
 * stub. A vanilla type reads and writes the loadout's choice as before; a new one reads 0 and writes nothing, and the
 * loader's count store keeps the moved list's length (read from the block, so a live raise moves it). */
typedef uint32_t (*GuardCallFn)(uint8_t* base, uint32_t type, uint32_t value);
static uint8_t* GuardHarness(uintptr_t game, uint32_t rva, const uint8_t* prefix, size_t prefixLength, const uint8_t* pin, size_t length,
    const uint8_t* suffix, size_t suffixLength)
{
    uint8_t* at = (uint8_t*)(game + rva - prefixLength);
    assert(VirtualAlloc((void*)((uintptr_t)at & ~(uintptr_t)0xFFF), 0x2000, MEM_COMMIT, PAGE_EXECUTE_READWRITE) != NULL);
    memcpy(at, prefix, prefixLength);
    memcpy(at + prefixLength, pin, length);
    memcpy(at + prefixLength + length, suffix, suffixLength);
    return at;
}

static void StratagemGuardsFixture(void)
{
    uintptr_t game = (uintptr_t)VirtualAlloc(NULL, 0x3500000, MEM_RESERVE, PAGE_NOACCESS);
    assert(game != 0);
    /* rbx / rdi = the loadout, eax = the type; the read's result returned */
    static const uint8_t rbxIn[] = {0x53, 0x48, 0x89, 0xCB, 0x89, 0xD0}, rdiIn[] = {0x57, 0x48, 0x89, 0xCF, 0x89, 0xD0};
    static const uint8_t r9Out[] = {0x44, 0x89, 0xC8, 0x5B, 0xC3}, eaxOut[] = {0x5B, 0xC3}, r9OutRdi[] = {0x44, 0x89, 0xC8, 0x5F, 0xC3};
    /* decoder: r13 = the choices, r14 = the type, ebp = the value; writer: rax = the loadout, ecx = the type, r9d = the value */
    static const uint8_t decodeIn[] = {0x41, 0x55, 0x41, 0x56, 0x55, 0x49, 0x89, 0xCD, 0x41, 0x89, 0xD6, 0x44, 0x89, 0xC5};
    static const uint8_t decodeOut[] = {0x5D, 0x41, 0x5E, 0x41, 0x5D, 0xC3};
    static const uint8_t writeIn[] = {0x48, 0x89, 0xC8, 0x89, 0xD1, 0x45, 0x89, 0xC1}, ret[] = {0xC3};
    /* the count: r10d = the rows loaded (edx), and r10d returned, as the stub must keep it */
    static const uint8_t countIn[] = {0x41, 0x89, 0xD2}, countOut[] = {0x44, 0x89, 0xD0, 0xC3};
    GuardCallFn call[9];
    for (int i = 0; i < 8; i++)
    {
        const ChoiceGuard* g = &kChoiceGuards[i];
        int rdi = g->pin[3] == 0x87, r9 = g->pin[0] == 0x44;
        if (i == 6) call[i] = (GuardCallFn)GuardHarness(game, g->rva, decodeIn, sizeof decodeIn, g->pin, g->length, decodeOut, sizeof decodeOut);
        else if (i == 7) call[i] = (GuardCallFn)GuardHarness(game, g->rva, writeIn, sizeof writeIn, g->pin, g->length, ret, sizeof ret);
        else call[i] = (GuardCallFn)GuardHarness(game, g->rva, rdi ? rdiIn : rbxIn, sizeof rbxIn, g->pin, g->length,
            rdi ? r9OutRdi : r9 ? r9Out : eaxOut, rdi ? sizeof r9OutRdi : r9 ? sizeof r9Out : sizeof eaxOut);
    }
    call[8] = (GuardCallFn)GuardHarness(game, STR_COUNT_STORE, countIn, sizeof countIn, kCountStorePin, sizeof kCountStorePin, countOut, sizeof countOut);
    uint32_t* count = (uint32_t*)(game + STR_ROWS_LOADED);
    assert(VirtualAlloc((void*)((uintptr_t)count & ~(uintptr_t)0xFFF), 0x1000, MEM_COMMIT, PAGE_READWRITE) != NULL);
    uint32_t* length = (uint32_t*)(game + 0x3400000); /* the block's length: near game.dll, as StratagemBlock's */
    assert(VirtualAlloc(length, 0x1000, MEM_COMMIT, PAGE_READWRITE) != NULL);
    assert(ChoiceGuardsFailed(game) == NULL);

    static uint8_t loadout[0x2000];
    uint32_t* choices = (uint32_t*)(loadout + 0x78C);
    for (uint32_t t = 0; t < (sizeof loadout - 0x78C) / 4; t++) choices[t] = t < 150 ? 1000 + t : 0xDEADBEEFu;
    for (int i = 0; i < 6; i++) assert(call[i](loadout, 149, 0) == 1149 && call[i](loadout, 150, 0) == 0xDEADBEEFu); /* unguarded: reads past */
    *length = STR_VANILLA + 48;
    assert(GuardStratagemTypes(game, length, ApiWrite) == NULL);
    assert(ChoiceGuardsFailed(game) != NULL); /* the sites now jump */
    for (int i = 0; i < 6; i++)
    {
        assert(call[i](loadout, 0, 0) == 1000 && call[i](loadout, 7, 0) == 1007 && call[i](loadout, 149, 0) == 1149);
        assert(call[i](loadout, 150, 0) == 0 && call[i](loadout, 197, 0) == 0 && call[i](loadout, 255, 0) == 0 && call[i](loadout, 1173, 0) == 0 &&
            call[i](loadout, 0xFFFFFFFFu, 0) == 0);
    }
    call[6](loadout + 0x78C, 5, 77);
    call[6](loadout + 0x78C, 150, 99);
    call[6](loadout + 0x78C, 1000, 99);
    call[7](loadout, 9, 88);
    call[7](loadout, 151, 99);
    call[7](loadout, 1173, 99);
    assert(choices[5] == 77 && choices[9] == 88 && choices[150] == 0xDEADBEEFu && choices[151] == 0xDEADBEEFu && choices[1000] == 0xDEADBEEFu);
    for (uint32_t t = 0; t < 150; t++) assert(t == 5 || t == 9 || choices[t] == 1000 + t);
    /* the loader's 149 rows, kept at the list's length; more than it, as loaded; a live raise's length, kept too */
    *count = 0;
    assert(call[8](NULL, 149, 0) == 149 && *count == 198);
    assert(call[8](NULL, 300, 0) == 300 && *count == 300);
    *length = STR_TYPES;
    assert(call[8](NULL, 149, 0) == 149 && *count == STR_TYPES);
    VirtualFree((void*)game, 0, MEM_RELEASE);
    printf("  stratagem cap: the loadout's choices guarded at eight sites (new types read 0, write nothing), the loader's count kept at the list's length\n");
}

/* The picker's frame for C candidates (WidenPicker): at 150 the game's own bytes again; at 256 the prior research's
 * figures (P 0x1450, K 0x1C50, F 0x1C60; unwind slots 0x395 / 0x394 / 0x393 / 0x38C); at 1,174 and 4,096 the formula;
 * past STR_PICKER_MAX refused. */
static uint32_t PickerValue(uintptr_t game, int site) { uint32_t v; memcpy(&v, (void*)(game + kPickerFrame[site].rva + kPickerFrame[site].at), 4); return v; }
static uint16_t PickerSlot(uintptr_t game, int code) { uint16_t v; memcpy(&v, (void*)(game + STR_PICKER_UNWIND + 6 + 4 * code), 2); return v; }

static void PickerFrameFixture(void)
{
    uintptr_t game = (uintptr_t)VirtualAlloc(NULL, 0x2600000, MEM_RESERVE, PAGE_NOACCESS);
    assert(game != 0);
    assert(VirtualAlloc((void*)(game + 0x18D8000), 0x1000, MEM_COMMIT, PAGE_READWRITE) != NULL);
    assert(VirtualAlloc((void*)(game + (STR_PICKER_UNWIND & ~0xFFFu)), 0x1000, MEM_COMMIT, PAGE_READWRITE) != NULL);
    for (int i = 0; i < 6; i++) memcpy((void*)(game + kPickerFrame[i].rva), kPickerFrame[i].pin, kPickerFrame[i].length);
    memcpy((void*)(game + STR_PICKER_UNWIND), kPickerUnwindPin, sizeof kPickerUnwindPin);
    assert(PickerFrameFailed(game) == NULL);

    assert(WidenPicker(game, 150, ApiWrite) == NULL && PickerFrameFailed(game) == NULL); /* the vanilla frame, byte for byte */
    assert(WidenPicker(game, 256, ApiWrite) == NULL && PickerFrameFailed(game) != NULL);
    assert(PickerValue(game, 0) == 0x1C60 && PickerValue(game, 5) == 0x1C60 && PickerValue(game, 1) == 0x1C50 && PickerValue(game, 4) == 0x1C50 &&
        PickerValue(game, 2) == 0x1450 && PickerValue(game, 3) == 0x1450);
    assert(PickerSlot(game, 0) == 0x395 && PickerSlot(game, 1) == 0x394 && PickerSlot(game, 2) == 0x393 && PickerSlot(game, 3) == 0x38C);
    uint32_t cookie;
    memcpy(&cookie, (void*)(game + STR_PICKER_UNWIND + 36), 4);
    assert(cookie == 0x1C50);
    static const uint32_t sizes[2] = {STR_TYPES, STR_PICKER_MAX};
    for (int k = 0; k < 2; k++)
    {
        uint32_t c = sizes[k];
        assert(WidenPicker(game, c, ApiWrite) == NULL);
        PickerFrame f = PickerFrameFor(c);
        assert(f.pointers >= 0x50 + 20 * c && (f.pointers & 15) == 0 && f.cookie == f.pointers + 8 * c && f.frame == f.cookie + 0x10);
        assert(PickerValue(game, 0) == f.frame && PickerValue(game, 1) == f.cookie && PickerValue(game, 2) == f.pointers);
        assert(PickerSlot(game, 3) * 8u == f.frame && PickerSlot(game, 2) * 8u == f.frame + 0x38 && PickerSlot(game, 0) * 8u == f.frame + 0x48);
        memcpy(&cookie, (void*)(game + STR_PICKER_UNWIND + 36), 4);
        assert(cookie == f.cookie);
    }
    assert(PickerFrameFor(1174).frame == 0x80D0); /* research: C 1,174 gives F 0x80D0 (32,976 bytes) */
    assert(WidenPicker(game, STR_PICKER_MAX + 1, ApiWrite) != NULL && PickerValue(game, 0) == PickerFrameFor(STR_PICKER_MAX).frame);
    /* the rest of the unwind data is the game's */
    assert(memcmp((void*)(game + STR_PICKER_UNWIND), kPickerUnwindPin, 6) == 0 && memcmp((void*)(game + STR_PICKER_UNWIND + 20), kPickerUnwindPin + 20, 16) == 0);
    VirtualFree((void*)game, 0, MEM_RELEASE);
    printf("  stratagem cap: the picker's frame and unwind data by formula (150 the game's own, 256 the research's, 1,174, 4,096), past it refused\n");
}

/* A fake network configuration whose field types are rows[count] (FieldTypes: [[module+rva]] -> +0x10 count, +0x18 rows);
 * the saved globals are put back by FakeNetworkEnd. */
static uint8_t* g_fakeNetConfig;
static uint8_t** g_fakeNetHolder = &g_fakeNetConfig;
static char g_fakeNetSavedModule[sizeof g_networkModule];
static uint32_t g_fakeNetSavedRva;

static void FakeNetwork(uint8_t (*rows)[0x18], uint32_t count)
{
    static uint8_t config[0x20];
    uint64_t rowsAt = (uint64_t)(uintptr_t)rows;
    memcpy(config + 0x10, &count, 4);
    memcpy(config + 0x18, &rowsAt, 8);
    g_fakeNetConfig = config;
    char exe[MAX_PATH];
    assert(GetModuleFileNameA(NULL, exe, sizeof exe) != 0);
    memcpy(g_fakeNetSavedModule, g_networkModule, sizeof g_fakeNetSavedModule);
    g_fakeNetSavedRva = g_networkRva;
    snprintf(g_networkModule, sizeof g_networkModule, "%s", strrchr(exe, '\\') != NULL ? strrchr(exe, '\\') + 1 : exe);
    g_networkRva = (uint32_t)((uintptr_t)&g_fakeNetHolder - (uintptr_t)GetModuleHandleA(NULL));
}

static void FakeNetworkEnd(void)
{
    memcpy(g_networkModule, g_fakeNetSavedModule, sizeof g_fakeNetSavedModule);
    g_networkRva = g_fakeNetSavedRva;
}

/* The co-op stratagem field (WidenStratagemField): left at 8 bits up to 256 types, 16 past them, and refused when the
 * row is not the reviewed {kind 1, 8 bits, min 0} or its hash names two rows. */
static void StratagemFieldFixture(void)
{
    static uint8_t rows[4][0x18];
    uint32_t hashes[4] = {0x11111111u, STR_FIELD_HASH, 0x22222222u, 0x33333333u};
    FakeNetwork(rows, 4);
    uint32_t savedCount = g_stratagemCount;
    struct { uint32_t stratagems; uint8_t bits; int duplicate; int state; uint8_t after; } cases[] = {
        {106, 8, 0, 1, 8},     /* 256 types: type 255 still fits */
        {107, 8, 0, 1, 16},    /* 257 types */
        {1024, 8, 0, 1, 16},
        {1024, 16, 0, 1, 16},  /* already 16 (a second network load) */
        {1024, 10, 0, -1, 10}, /* not the reviewed width */
        {1024, 8, 1, -1, 8}};  /* the hash names two rows */
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++)
    {
        memset(rows, 0, sizeof rows);
        for (int i = 0; i < 4; i++) { memcpy(rows[i], &hashes[i], 4); rows[i][12] = 1; rows[i][13] = 8; }
        if (cases[k].duplicate) memcpy(rows[3], &hashes[1], 4);
        rows[1][13] = cases[k].bits;
        g_stratagemCount = cases[k].stratagems;
        g_stratagemField = 0;
        WidenStratagemField();
        assert(g_stratagemField == cases[k].state && rows[1][13] == cases[k].after);
        assert(rows[0][13] == 8 && rows[2][13] == 8 && rows[1][12] == 1); /* nothing else changes */
    }
    /* a live change past 256 types widens it too (its gate: no friend in the game) */
    memset(rows, 0, sizeof rows);
    for (int i = 0; i < 4; i++) { memcpy(rows[i], &hashes[i], 4); rows[i][12] = 1; rows[i][13] = 8; }
    assert(StratagemFieldHolds(256, ApiWrite) && rows[1][13] == 8 && StratagemFieldHolds(300, ApiWrite) && rows[1][13] == 16);
    g_stratagemCount = savedCount;
    g_stratagemField = 0;
    FakeNetworkEnd();
    printf("  stratagem cap: the co-op type field 8 bits up to 256 types, 16 past them, refused when not the reviewed row\n");
}

/* All 1,024 at boot, on the live content fixture's fake game.dll: each its own type and row, the list's length in the
 * rows loaded, the block and every bound, the picker sized for every type, the guards in, the field at 16 bits. One
 * more is refused. */
static void StratagemCapFixture(void)
{
    ContentEventsSave();
    int mods = g_modCount;
    size_t size = 0x3800000;
    uint8_t* game = ContentGame(size);
    uintptr_t g = (uintptr_t)game;
    uint32_t savedCount = g_stratagemCount;
    int savedState = g_stratagemState;
    uint8_t* savedTable = g_stratagemTable;
    static uint8_t rows[2][0x18];
    uint32_t hash = STR_FIELD_HASH, other = 0x44444444u;
    memset(rows, 0, sizeof rows);
    memcpy(rows[0], &other, 4); memcpy(rows[1], &hash, 4);
    rows[0][12] = rows[1][12] = 1; rows[0][13] = rows[1][13] = 8;
    FakeNetwork(rows, 2);
    g_stratagemCount = 0;
    g_stratagemState = 0;
    g_stratagemTable = NULL;
    g_stratagemField = 0;
    InterlockedExchange(&g_fired[HF_EVENT_GAME_LOADED], 0);

    HfMod* m = ContentMod("cap");
    char name[32];
    for (uint32_t i = 0; i < STR_MAX_NEW; i++)
    {
        snprintf(name, sizeof name, "s%u", i);
        HfStratagem s = {sizeof s, .base = 1, .name = name, .description = "d"};
        assert(ApiAddStratagem(m, &s) == STR_VANILLA + i);
    }
    HfStratagem more = {sizeof more, .base = 1, .name = "more", .description = "d"};
    assert(ApiAddStratagem(m, &more) == 0 && g_stratagemCount == STR_MAX_NEW);
    InterlockedExchange(&g_fired[HF_EVENT_GAME_LOADED], 1);

    assert(InstallStratagems(g, 1) == 0 && g_stratagemState == 0); /* past 256 types it waits for the network */
    WidenStratagemField();
    assert(g_stratagemField == 1 && rows[1][13] == 16 && rows[0][13] == 8);
    assert(InstallStratagems(g, 1) == 1 && g_stratagemState == 1 && g_stratagemTable != NULL);
    uint8_t* table = g_stratagemTable;
    for (uint32_t i = 0; i < STR_MAX_NEW; i++)
    {
        uint8_t* row = NULL;
        memcpy(&row, table + (STR_VANILLA + i) * 8, 8);
        assert(row == g_stratagems[i].row && *(uint32_t*)row == STR_VANILLA + i && *(uint32_t*)(row + 4) == g_stratagems[i].stable);
    }
    assert(*(uint32_t*)(game + STR_ROWS_LOADED) == STR_TYPES && *(uint32_t*)(table + STR_LENGTH_AT) == STR_TYPES);
    assert(*(uint32_t*)(game + STR_PICKER_BOUND + 1) == STR_TYPES && *(uint32_t*)(game + STR_PICKER_PAD + 1) == STR_TYPES);
    assert(PickerValue(g, 0) == PickerFrameFor(STR_TYPES).frame);
    for (size_t i = 0; i < sizeof kStratagemBounds / sizeof kStratagemBounds[0]; i++)
    {
        uint32_t value = 0;
        memcpy(&value, game + kStratagemBounds[i].rva + kStratagemBounds[i].immAt, kStratagemBounds[i].width);
        assert(value == kStratagemBounds[i].value + STR_MAX_NEW);
    }
    for (size_t i = 0; i < sizeof kChoiceGuards / sizeof kChoiceGuards[0]; i++) assert(game[kChoiceGuards[i].rva] == 0xE9);
    assert(game[STR_COUNT_STORE] == 0xE9);
    const char* last = NULL;
    memcpy(&last, table + STR_NAMES_AT + (STR_TYPES - 1) * 8, 8);
    assert(last == g_stratagems[STR_MAX_NEW - 1].name);
    memcpy(&last, table + STR_NAMES_AT + STR_TYPES * 8, 8);
    assert(!strcmp(last, "Count"));

    g_stratagemCount = savedCount;
    g_stratagemState = savedState;
    g_stratagemTable = savedTable;
    g_stratagemField = 0;
    FakeNetworkEnd();
    g_modCount = mods;
    ContentEventsRestore();
    VirtualFree(game, 0, MEM_RELEASE);
    printf("  stratagem cap: 1,024 new stratagems at boot (types 150..1173), the 1,025th refused, waiting for the 16-bit field\n");
}
