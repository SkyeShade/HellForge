/* The Acquisition Center's pager past 32 cards (static analysis of build 25480438). The pager (AC panel W + 0x110) and its
 * 32 dots read {count, current} through pager+0x7AE8 / dot+0x228 (last frame +0x7AF0 / +0x230): it spaces count dots over
 * its height ((564 - 10 count) / (count + 1) each, negative past ~54 cards), lights the dot at current (none past 32) and
 * takes a clicked dot i as the card to select (pager+0x7B14). The AC keeps the real pair ([W+0x87E8], state+0x320).
 * Past 32 cards the pager shows the page of at most 32 the current card is in (ceil(n / 32) equal pages): its pair and
 * the dots' point at the loader's, recomputed before its update and input, and a clicked dot becomes the real card. A
 * screen whose pager keeps its own pair runs as before. Included after warbond_list.c. */
#define WB_PAGER_INIT 0x1968015u   /* mov dword [rbx+0x87FC],0x1346E4EA, right after 0x19430B0 built the pager; rbx = W */
#define WB_PAGER_UPDATE 0x194345Au /* call 0x1947950 (rcx = pager, xmm1 = the frame's time) */
#define WB_PAGER_INPUT 0x1943384u  /* call 0x1947730 (rcx = pager, rdx, r8, r9; eax nonzero when +0x7B14 changed) */
static const uint8_t kWbPagerInitPin[10] = {0xc7,0x83,0xfc,0x87,0x00,0x00,0xea,0xe4,0x46,0x13};
static const uint8_t kWbPagerUpdatePin[5] = {0xe8,0xf1,0x44,0x00,0x00};
static const uint8_t kWbPagerInputPin[5] = {0xe8,0xa7,0x43,0x00,0x00};
static uint32_t g_wbPagerView[2], g_wbPagerLast[2], g_wbPagerBase;
static void (*g_wbPagerUpdate)(uint8_t* pager, float dt);
static int (*g_wbPagerInput)(uint8_t* pager, void* a, void* b, void* c);

static int WarbondPagerOurs(const uint8_t* pager)
{
    return *(uint32_t* const*)(pager + 0x7AE8) == g_wbPagerView;
}

static const uint32_t* WarbondPagerReal(const uint8_t* pager)
{
    return *(const uint32_t* const*)(pager - 0x110 + 0x87E8);
}

/* The pager's pair now: the page of the current card. */
static void WarbondPagerView(const uint8_t* pager)
{
    const uint32_t* real = WarbondPagerReal(pager);
    uint32_t n = real[0];
    int32_t current = (int32_t)real[1];
    uint32_t c = current < 0 ? 0u : (uint32_t)current;
    if (n <= 32)
    {
        g_wbPagerBase = 0;
        memcpy(g_wbPagerView, real, 8);
        return;
    }
    if (c >= n) c = n - 1;
    uint32_t pages = (n + 31) / 32, per = (n + pages - 1) / pages;
    g_wbPagerBase = c / per * per;
    g_wbPagerView[0] = n - g_wbPagerBase < per ? n - g_wbPagerBase : per;
    g_wbPagerView[1] = c - g_wbPagerBase;
}

static void WarbondPagerUpdate(uint8_t* pager, float dt)
{
    if (!WarbondPagerOurs(pager))
    {
        g_wbPagerUpdate(pager, dt);
        return;
    }
    WarbondPagerView(pager);
    g_wbPagerUpdate(pager, dt);
    memcpy(g_wbPagerLast, g_wbPagerView, 8);
}

static int WarbondPagerInput(uint8_t* pager, void* a, void* b, void* c)
{
    if (!WarbondPagerOurs(pager)) return g_wbPagerInput(pager, a, b, c);
    WarbondPagerView(pager);
    int changed = g_wbPagerInput(pager, a, b, c);
    if (changed)
    {
        uint32_t n = WarbondPagerReal(pager)[0], dot;
        memcpy(&dot, pager + 0x7B14, 4);
        uint32_t card = g_wbPagerBase + dot;
        if (n > 0 && card > n - 1) card = n - 1;
        memcpy(pager + 0x7B14, &card, 4);
    }
    return changed;
}

/* The AC's init, its pager just built: the pager and its dots read the loader's pair. */
static void WarbondPagerInit(uint8_t* panel)
{
    uint8_t* pager = panel + 0x110;
    uint32_t* view = g_wbPagerView;
    uint32_t* last = g_wbPagerLast;
    memset(g_wbPagerLast, 0, sizeof g_wbPagerLast); /* differs from the view, so the first update lays the dots out */
    memcpy(pager + 0x7AE8, &view, 8);
    memcpy(pager + 0x7AF0, &last, 8);
    for (int i = 0; i < 32; i++)
    {
        uint8_t* dot = pager + 0x1C50 + (size_t)i * 0x268;
        memcpy(dot + 0x228, &view, 8);
        memcpy(dot + 0x230, &last, 8);
    }
    WarbondPagerView(pager);
}

/* After the list's room. NULL, or why not. */
static const char* InstallWarbondPager(uintptr_t game)
{
    if (!PinMatches(game + WB_PAGER_INIT, kWbPagerInitPin, sizeof kWbPagerInitPin) ||
        !PinMatches(game + WB_PAGER_UPDATE, kWbPagerUpdatePin, sizeof kWbPagerUpdatePin) ||
        !PinMatches(game + WB_PAGER_INPUT, kWbPagerInputPin, sizeof kWbPagerInputPin))
        return "the pager code does not match the reviewed build";
    uint8_t* code = AllocateNearGame(game, 0x1000);
    if (code == NULL) return "no memory near game.dll";
    g_wbPagerUpdate = (void (*)(uint8_t*, float))(game + WB_PAGER_UPDATE + 5 + 0x44F1);
    g_wbPagerInput = (int (*)(uint8_t*, void*, void*, void*))(game + WB_PAGER_INPUT + 5 + 0x43A7);
    Emit e = {code};
    uint8_t* init = e.at; /* save rax rcx rdx r8-r11; WarbondPagerInit(rbx); the replaced mov; ret (rsp 16-aligned at the site) */
    Put(&e, "\x50\x51\x52\x41\x50\x41\x51\x41\x52\x41\x53\x48\x83\xec\x20\x48\x89\xd9", 18);
    PutAbs(&e, (const void*)WarbondPagerInit);
    Put(&e, "\xff\xd0\x48\x83\xc4\x20\x41\x5b\x41\x5a\x41\x59\x41\x58\x5a\x59\x58", 17);
    Put(&e, kWbPagerInitPin, sizeof kWbPagerInitPin);
    Put8(&e, 0xC3);
    uint8_t* update = e.at; /* jmp [rip+0]; the C function's address */
    Put(&e, "\xff\x25\x00\x00\x00\x00", 6);
    uint64_t to = (uint64_t)(uintptr_t)WarbondPagerUpdate;
    Put(&e, &to, 8);
    uint8_t* input = e.at;
    Put(&e, "\xff\x25\x00\x00\x00\x00", 6);
    to = (uint64_t)(uintptr_t)WarbondPagerInput;
    Put(&e, &to, 8);
    DWORD protect;
    if (!VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &protect)) return "its stubs could not be made executable";
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    uint8_t call[10] = {0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90}; /* call init; nops over the rest of the mov */
    int32_t rel = (int32_t)(int64_t)((intptr_t)init - (intptr_t)(game + WB_PAGER_INIT + 5));
    memcpy(call + 1, &rel, 4);
    if (WarbondStubJump(game + WB_PAGER_UPDATE, 5, 0xE8, update) || WarbondStubJump(game + WB_PAGER_INPUT, 5, 0xE8, input) ||
        ApiWrite(game + WB_PAGER_INIT, call, sizeof call))
        return "game.dll could not be rewritten";
    return NULL;
}
