/* The warbond texts that name medals (static analysis of build 25480438): "#COUNT Medal(s) spent" (0xDAD34D57), on the
 * Acquisition Center's cards and a warbond's page header, and "Spend #COUNT more medals to unlock" (0xE631A5A8), on a
 * locked page. A mod warbond in another currency shows its own: the spent widget's update (0x194A700; its data block is
 * a card's or the page view, with the price block warbonds.c fills at +0x40) gives its text element (+0x378) the text of
 * the block's currency; the page's lock text, which its update (0x1975060) sets on every call, takes its id from a stub
 * at both `mov edx,0xE631A5A8` (the page's currency, which the header's price stub keeps). Medals keep the game's. A text
 * element resolves its id through the engine's text function (HookText) and fills #COUNT after it. Included after
 * warbond_list.c. */
#define WB_SPENT_TEXT 0xDAD34D57u
#define WB_LOCK_TEXT 0xE631A5A8u
#define WB_SPENT_UPDATE 0x194A700u
#define WB_TEXT_SET 0x1441720u      /* a text element of kind 0x200000 */
#define WB_TEXT_SET_ALT 0x143BF90u  /* of kind 0x1C0000 */
static const uint32_t kWbLockSites[2] = {0x19751AAu, 0x19751B9u};
static const uint8_t kWbLockPin[5] = {0xba, 0xa8, 0xa5, 0x31, 0xe6};                           /* mov edx,0xE631A5A8 */
static const uint8_t kWbSpentUpdatePin[15] = {0x48,0x89,0x5c,0x24,0x10,0x55,0x56,0x57,0x48,0x81,0xec,0x90,0x00,0x00,0x00};
/* requisition, Forge Credits, then the samples in kWbSampleIds' order (common, rare, super) */
static const char* const kWbSpentTexts[5] = {"#COUNT Requisition spent", "#COUNT Forge Credits spent",
    "#COUNT Common Sample(s) spent", "#COUNT Rare Sample(s) spent", "#COUNT Super Sample(s) spent"};
static const char* const kWbLockTexts[5] = {"Spend #COUNT more requisition to unlock", "Spend #COUNT more Forge Credits to unlock",
    "Spend #COUNT more common samples to unlock", "Spend #COUNT more rare samples to unlock", "Spend #COUNT more super samples to unlock"};
static uint32_t g_wbSpentText[5], g_wbLockText[5];
static void (*g_wbSpentUpdate)(void* widget, float a, float b);

/* A currency's place in the texts: from its mixId, or from a price block {kind, icon override} as WarbondPriceKind
 * writes it; -1 for medals and any other. */
static int WarbondTextOf(uint32_t currency)
{
    if (currency == 0xD7150EECu) return 0;
    if (currency == 0x6C3C83C1u) return 1;
    for (int i = 0; i < 3; i++) if (currency == kWbSampleIds[i]) return 2 + i;
    return -1;
}
static int WarbondTextOfBlock(const uint8_t* block)
{
    uint32_t kind;
    uint64_t icon;
    memcpy(&kind, block, 4);
    memcpy(&icon, block + 8, 8);
    if (kind == 1) return 0;
    if (kind == 2) return 1;
    for (int i = 0; i < 3 && kind == 3 && icon != 0; i++) if (icon == g_wbSampleIcon[i]) return 2 + i;
    return -1;
}

/* Sets a text element's id the way the game does (by the element's kind); only over the game's spent text or ours. */
static void WarbondSetSpentText(uint8_t* element, uint32_t id)
{
    uint32_t now, kind;
    memcpy(&now, element + 0x110, 4);
    memcpy(&kind, element, 4);
    if (now == id) return;
    int ours = now == WB_SPENT_TEXT;
    for (int i = 0; i < 5; i++) ours |= now == g_wbSpentText[i];
    if (!ours) return;
    kind &= 0x3C0000u;
    if (kind == 0x200000u) ((void (*)(void*, uint32_t))(g_wbGame + WB_TEXT_SET))(element, id);
    else if (kind == 0x1C0000u) ((void (*)(void*, uint32_t))(g_wbGame + WB_TEXT_SET_ALT))(element, id);
}

/* The update redraws the page bar only when the pages open (+0x5C) differ from the last frame's block (+0x2138), but
 * fills it only for a warbond the player holds (+0x64): the next card with as many pages open and the other flag kept
 * the last one's bar. A last-frame value that differs makes it redraw; the game copies the block anew each frame. */
static void WarbondBarRedraw(const uint8_t* block, uint8_t* last)
{
    if (last == NULL || block[0x64] == last[0x64] || memcmp(block + 0x5C, last + 0x5C, 4) != 0) return;
    uint32_t other = ~*(const uint32_t*)(block + 0x5C);
    memcpy(last + 0x5C, &other, 4);
}

static void WarbondSpentUpdate(void* widget, float a, float b)
{
    const uint8_t* block = *(const uint8_t* const*)((const uint8_t*)widget + 0x2130);
    if (block != NULL)
    {
        WarbondBarRedraw(block, *(uint8_t* const*)((const uint8_t*)widget + 0x2138));
        int text = WarbondTextOfBlock(block + 0x40);
        uint32_t id = text >= 0 && g_wbSpentText[text] != 0 ? g_wbSpentText[text] : WB_SPENT_TEXT;
        WarbondSetSpentText((uint8_t*)widget + 0x378, id);
    }
    g_wbSpentUpdate(widget, a, b);
}

/* The lock text's id for the page on screen (the stub's eax). */
static uint32_t WarbondLockText(void)
{
    int text = WarbondTextOf(g_wbPageCurrency);
    return text >= 0 && g_wbLockText[text] != 0 ? g_wbLockText[text] : WB_LOCK_TEXT;
}

/* After the price stubs. NULL, or why not. */
static const char* InstallWarbondTexts(uintptr_t game)
{
    if (!PinMatches(game + WB_SPENT_UPDATE, kWbSpentUpdatePin, sizeof kWbSpentUpdatePin) ||
        !PinMatches(game + kWbLockSites[0], kWbLockPin, 5) || !PinMatches(game + kWbLockSites[1], kWbLockPin, 5))
        return "the text code does not match the reviewed build";
    for (int i = 0; i < 5; i++)
    {
        g_wbSpentText[i] = AddText(kWbSpentTexts[i], kWbSpentTexts[i], 0x45u);
        g_wbLockText[i] = AddText(kWbLockTexts[i], kWbLockTexts[i], 0x46u);
    }
    uint8_t* code = AllocateNearGame(game, 0x1000);
    if (code == NULL) return "no memory near game.dll";
    Emit e = {code}; /* sub rsp,0x28; WarbondLockText(); add rsp,0x28; mov edx,eax; ret (rsp is 8 off 16 at the stub) */
    Put(&e, "\x48\x83\xec\x28", 4);
    PutAbs(&e, (const void*)WarbondLockText);
    Put(&e, "\xff\xd0\x48\x83\xc4\x28\x89\xc2\xc3", 9);
    DWORD protect;
    if (!VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &protect)) return "its stub could not be made executable";
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    if (LoaderHook("warbond spent text", game + WB_SPENT_UPDATE, (void*)WarbondSpentUpdate, (void**)&g_wbSpentUpdate)) return "the spent text's update could not be hooked";
    for (size_t i = 0; i < 2; i++)
        if (WarbondStubJump(game + kWbLockSites[i], 5, 0xE8, code)) return "game.dll could not be rewritten";
    return NULL;
}
