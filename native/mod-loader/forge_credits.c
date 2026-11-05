/* Build 25480438: Super Credits are drawn in the game's cyan (A, R, G, B floats: 1, 113, 245, 255 / 255); Forge Credits in
 * HellForge red (research/forge-credits-tint, "UI colours"):
 * - the code UI's currency colour table (0x21CAE60, one 16-byte colour per currency type: 2 the Super Credits price,
 *   3 the Premium wallet, 0x21CAE80 and 0x21CAE90; only types 2 to 6 reach them) is written red in place; its readers
 *   include the price widgets (0x1797A60, 0x19C1DF0) and the icon getter (0x12682A0); so is its premium warbond token
 *   entry (0x21CAEF0, light blue), which the Acquisition Center's wallet draws the currency in: it shows Forge
 *   Credits;
 * - two direct loads of 0x21CAE90 (the mission loot counters' switch 0x198BACE, beside the sprite 0x9414AB4F02745820,
 *   and the in-mission counter row 0x1977612) and the premium (warbond) button's colours, set up in 0x1968AC0 and
 *   refreshed in 0x1969F30 only for its premium variant: border, highlight, its two dark-teal fills, and its star's red
 *   and green loads and blue immediate. Their constants are shared with other UI, so each load is pointed at HellForge's
 *   own values instead.
 * The XAML pages (the pause menu's wallet, popups) are data: ForgeCredits.cs. Included after damage.c (AllocateNearGame)
 * and packages.c (PinMatches). */
enum { FC_RED = 0x00, FC_FILL = 0x10, FC_FILL_DARK = 0x20, FC_RED_R = 0x30, FC_RED_G = 0x34, FC_RED_B = 0x38 };
/* HellForge red (229, 57, 43) and the button's fills in red instead of teal (0, 44, 65 and 18, 27, 33) */
static const float kForgeCreditsColours[] = {
    1.0f, 229.0f / 255.0f, 57.0f / 255.0f, 43.0f / 255.0f,
    1.0f, 65.0f / 255.0f, 10.0f / 255.0f, 8.0f / 255.0f,
    1.0f, 33.0f / 255.0f, 10.0f / 255.0f, 9.0f / 255.0f,
    229.0f / 255.0f, 57.0f / 255.0f, 43.0f / 255.0f};
/* A load whose disp32 (at `at` in the instruction) is pointed at one of the colours above (slot), or, with slot < 0, bytes
 * written at `at` (size bytes of with). */
typedef struct { uint32_t rva; uint8_t pin[32]; uint8_t length, at; int8_t slot; uint8_t size; uint8_t with[32]; } ForgeCreditsPatch;
#define FC_ARGB_RED 0x00,0x00,0x80,0x3f, 0xe6,0xe5,0x65,0x3f, 0xe5,0xe4,0x64,0x3e, 0xad,0xac,0x2c,0x3e
#define FC_CYAN 0x00,0x00,0x80,0x3f, 0xe3,0xe2,0xe2,0x3e, 0xf6,0xf5,0x75,0x3f, 0x00,0x00,0x80,0x3f
static const ForgeCreditsPatch kForgeCreditsPatches[] = {
    {0x21CAE80u, {FC_CYAN, FC_CYAN}, 32, 0, -1, 32, {FC_ARGB_RED, FC_ARGB_RED}},            /* currency types 2 and 3 */
    {0x21CAEF0u, {0x00,0x00,0x80,0x3f, 0x84,0x83,0x03,0x3f, 0xe0,0xdf,0x5f,0x3f, 0xfb,0xfa,0x7a,0x3f}, 16, 0, -1, 16, {FC_ARGB_RED}}, /* the premium warbond token's (131, 223, 251), the Acquisitions wallet */
    {0x198BACEu, {0x0f,0x28,0x05,0xbb,0xf3,0x83,0x00}, 7, 3, FC_RED, 0, {0}},               /* loot counter's switch */
    {0x1977612u, {0x0f,0x28,0x35,0x77,0x38,0x85,0x00}, 7, 3, FC_RED, 0, {0}},               /* in-mission counter row */
    {0x19690F3u, {0x0f,0x28,0x05,0x86,0x13,0xa6,0x00}, 7, 3, FC_RED, 0, {0}},               /* premium button: border */
    {0x196912Du, {0x0f,0x28,0x05,0x4c,0x13,0xa6,0x00}, 7, 3, FC_RED, 0, {0}},
    {0x196931Fu, {0xf3,0x0f,0x10,0x15,0x39,0xd7,0xa5,0x00}, 8, 4, FC_RED_R, 0, {0}},        /* its star */
    {0x196932Bu, {0xf3,0x0f,0x10,0x0d,0xe5,0xd9,0xa5,0x00}, 8, 4, FC_RED_G, 0, {0}},
    {0x1969340u, {0xc7,0x45,0xb7,0x00,0x00,0x80,0x3f}, 7, 3, -1, 4, {0xad,0xac,0x2c,0x3e}},
    {0x196A11Cu, {0xf3,0x0f,0x10,0x15,0x3c,0xc9,0xa5,0x00}, 8, 4, FC_RED_R, 0, {0}},        /* refresh: star */
    {0x196A129u, {0xf3,0x0f,0x10,0x0d,0xe7,0xcb,0xa5,0x00}, 8, 4, FC_RED_G, 0, {0}},
    {0x196A144u, {0xc7,0x44,0x24,0x28,0x00,0x00,0x80,0x3f}, 8, 4, -1, 4, {0xad,0xac,0x2c,0x3e}},
    {0x196A151u, {0x0f,0x28,0x05,0x28,0x03,0xa6,0x00}, 7, 3, FC_RED, 0, {0}},               /* border, two states */
    {0x196A174u, {0x0f,0x28,0x05,0x05,0x03,0xa6,0x00}, 7, 3, FC_RED, 0, {0}},
    {0x196A197u, {0x0f,0x28,0x05,0x52,0xf7,0xa5,0x00}, 7, 3, FC_FILL, 0, {0}},              /* fills */
    {0x196A19Eu, {0x0f,0x28,0x0d,0xdb,0xf4,0xa5,0x00}, 7, 3, FC_FILL_DARK, 0, {0}},
    {0x196A1B6u, {0x0f,0x28,0x05,0xc3,0x02,0xa6,0x00}, 7, 3, FC_RED, 0, {0}},               /* highlight */
    /* the currency icon switch 0x1948933: its Super Credits case (sprite 0x9414AB4F02745820, the Acquisition Center's
     * wallet) and the case after it (sprite 0x83F2040E9718C895) build light blue (131, 223, 250) from scalars */
    {0x1948A6Du, {0xf3,0x0f,0x10,0x05,0xbb,0xe2,0xa7,0x00}, 8, 4, FC_RED_B, 0, {0}},
    {0x1948A75u, {0xf3,0x0f,0x10,0x15,0x5b,0xe0,0xa7,0x00}, 8, 4, FC_RED_R, 0, {0}},
    {0x1948BADu, {0xf3,0x0f,0x10,0x15,0x23,0xdf,0xa7,0x00}, 8, 4, FC_RED_R, 0, {0}},
    {0x1948BBAu, {0xf3,0x0f,0x10,0x15,0x6e,0xe1,0xa7,0x00}, 8, 4, FC_RED_B, 0, {0}},
    {0x1948BC7u, {0xf3,0x0f,0x10,0x0d,0xd5,0xe0,0xa7,0x00}, 8, 4, FC_RED_G, 0, {0}}};       /* their shared green */
static int g_forgeCreditsState;

static void InstallForgeCreditsColour(void)
{
    if (g_forgeCreditsState) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    g_forgeCreditsState = -1;
    if (strcmp(g_build, "25480438") || !game) return;
    size_t count = sizeof kForgeCreditsPatches / sizeof *kForgeCreditsPatches;
    for (size_t i = 0; i < count; i++)
        if (!PinMatches(game + kForgeCreditsPatches[i].rva, kForgeCreditsPatches[i].pin, kForgeCreditsPatches[i].length))
        {
            Note("forge credits: UI colours not changed; the code at game.dll+0x%x does not match the reviewed build", kForgeCreditsPatches[i].rva);
            return;
        }
    uint8_t* colours = AllocateNearGame(game, sizeof kForgeCreditsColours); /* a page: 16-byte aligned for movaps */
    if (colours == NULL) { Note("forge credits: UI colours not changed; no memory near game.dll"); return; }
    memcpy(colours, kForgeCreditsColours, sizeof kForgeCreditsColours);
    DWORD previous;
    VirtualProtect(colours, 0x1000, PAGE_READONLY, &previous);
    for (size_t i = 0; i < count; i++)
    {
        const ForgeCreditsPatch* p = &kForgeCreditsPatches[i];
        uintptr_t at = game + p->rva;
        int failed;
        if (p->slot >= 0)
        {
            int32_t disp = (int32_t)((intptr_t)(colours + p->slot) - (intptr_t)(at + p->length));
            failed = ApiWrite(at + p->at, &disp, sizeof disp);
        }
        else failed = ApiWrite(at + p->at, p->with, p->size);
        if (failed) { Note("forge credits: UI colours: game.dll+0x%x could not be rewritten", p->rva); return; }
    }
    g_forgeCreditsState = 1;
    Note("forge credits: the UI's Super Credits colours are HellForge red (%u places)", (unsigned)count);
}

/* Forge Credits at points of interest (the plan's forge_credits_chance, ModSettings.ForgeCreditChance; -1: the game's
 * odds). Level generation's QueueRandomCacheLootRewardsPackages pass (op 0x21) calls 0xF3A800, which rolls each random
 * loot cache against the generation settings' cache loot tables ([[[lg+0x2D0]+0x68]+0x90], count +0x98, 0x28 bytes:
 * u32 id, i32 weight (a percentage; negative: a share of what the others leave of 100), i32 min and max difficulty (-1:
 * none), entries at +0x18 (0x30 bytes, the entity first), count +0x20). Three tables, one per difficulty band, hold only
 * the Super Credits pickup at weight 20 (research/forge-credits-spawn). Before each roll the Super Credits tables get the
 * chance and the other positive weights shrink to fit what is left; at 100 the others are made ineligible (minimum
 * difficulty out of reach), since a roll of 0 also lands on a table of weight 0. "Guaranteed" (0x3E1512C4) is left
 * alone. Without a chance in the plan nothing is hooked and the tables stay the game's. */
#define FORGE_CREDITS_ROLL 0xF3A800u
#define FORGE_CREDITS_ENTITY 0x818603ED54533506ull
#define FORGE_CREDITS_GUARANTEED 0x3E1512C4u
#define FORGE_CREDITS_TABLES 32
static const uint8_t kForgeCreditsRoll[] = {0x4c,0x8b,0xdc,0x53,0x56,0x57,0x41,0x54,0x41,0x57,0x48,0x81,0xec,0x30,0x01,0x00,0x00};
typedef uintptr_t (*ForgeCreditsRollFn)(uintptr_t params, uintptr_t unused, uintptr_t context, uintptr_t r9);
static ForgeCreditsRollFn g_forgeCreditsRollOriginal;
static int g_forgeCreditChance = -1;
static int g_forgeCreditsRollState;
/* Each table changed: where it is, its own weight and minimum difficulty, and what was written over them. A table whose
 * memory no longer holds what was written is new data (another mission's settings) and is read again. */
static struct { uintptr_t table; int32_t weight, minimum, wroteWeight, wroteMinimum; } g_forgeCreditsSaved[FORGE_CREDITS_TABLES];
static int g_forgeCreditsSavedCount;
static LONG g_forgeCreditsRolls;

static int ForgeCreditsHolds(uintptr_t table)
{
    uintptr_t entries = 0;
    uint32_t count = 0;
    if (SafeRead(table + 0x18, &entries, 8) || SafeRead(table + 0x20, &count, 4) || !entries || count > 256) return 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint64_t entity = 0;
        if (!SafeRead(entries + 0x30 * (uintptr_t)i, &entity, 8) && entity == FORGE_CREDITS_ENTITY) return 1;
    }
    return 0;
}

static int ForgeCreditsWritable(uintptr_t at, size_t size)
{
    MEMORY_BASIC_INFORMATION region;
    return VirtualQuery((void*)at, &region, sizeof region) && region.State == MEM_COMMIT &&
        (region.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && at + size <= (uintptr_t)region.BaseAddress + region.RegionSize;
}

static void ForgeCreditsTables(uintptr_t context)
{
    uintptr_t lg = 0, settings = 0, gen = 0, tables = 0;
    uint32_t count = 0;
    if (SafeRead(context + 0x18, &lg, 8) || !lg || SafeRead(lg + 0x2D0, &settings, 8) || !settings ||
        SafeRead(settings + 0x68, &gen, 8) || !gen || SafeRead(gen + 0x90, &tables, 8) || !tables ||
        SafeRead(gen + 0x98, &count, 4) || count == 0 || count > FORGE_CREDITS_TABLES || !ForgeCreditsWritable(tables, 0x28 * (size_t)count)) return;
    int changed = 0, holders = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uintptr_t table = tables + 0x28 * (uintptr_t)i;
        int32_t* weight = (int32_t*)(table + 4);
        int32_t* minimum = (int32_t*)(table + 8);
        int s = 0;
        while (s < g_forgeCreditsSavedCount && g_forgeCreditsSaved[s].table != table) s++;
        if (s == FORGE_CREDITS_TABLES) continue;
        if (s == g_forgeCreditsSavedCount || *weight != g_forgeCreditsSaved[s].wroteWeight || *minimum != g_forgeCreditsSaved[s].wroteMinimum)
        {
            if (s == g_forgeCreditsSavedCount) g_forgeCreditsSavedCount++;
            g_forgeCreditsSaved[s].table = table;
            g_forgeCreditsSaved[s].weight = *weight;
            g_forgeCreditsSaved[s].minimum = *minimum;
        }
        int32_t w = g_forgeCreditsSaved[s].weight, m = g_forgeCreditsSaved[s].minimum;
        if (*(uint32_t*)table != FORGE_CREDITS_GUARANTEED)
        {
            if (ForgeCreditsHolds(table)) { w = g_forgeCreditChance; holders++; }
            else if (g_forgeCreditChance == 100) m = 0x7FFFFFFF;
            else if (w > 0) w = w * (100 - g_forgeCreditChance) / 100;
        }
        if (*weight != w || *minimum != m) changed++;
        *weight = g_forgeCreditsSaved[s].wroteWeight = w;
        *minimum = g_forgeCreditsSaved[s].wroteMinimum = m;
    }
    if (changed && InterlockedIncrement(&g_forgeCreditsRolls) <= 3)
        Note("forge credits: loot caches roll Forge Credits at %d %% (%d of %u tables hold them)", g_forgeCreditChance, holders, count);
}

static uintptr_t ForgeCreditsRollDetour(uintptr_t params, uintptr_t unused, uintptr_t context, uintptr_t r9)
{
    if (context) ForgeCreditsTables(context);
    return g_forgeCreditsRollOriginal(params, unused, context, r9);
}

/* Hooked only when the plan sets a chance: without one the game's tables are never touched. */
static void InstallForgeCreditsRoll(void)
{
    if (g_forgeCreditsRollState || g_forgeCreditChance < 0) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    g_forgeCreditsRollState = -1;
    if (strcmp(g_build, "25480438") || !game) return;
    if (!PinMatches(game + FORGE_CREDITS_ROLL, kForgeCreditsRoll, sizeof kForgeCreditsRoll))
    {
        Note("forge credits: loot chance not applied; the loot roll does not match the reviewed build");
        return;
    }
    if (LoaderHook("forge credits loot roll", game + FORGE_CREDITS_ROLL, (void*)ForgeCreditsRollDetour, (void**)&g_forgeCreditsRollOriginal))
    {
        Note("forge credits: loot chance not applied; could not hook the loot roll");
        return;
    }
    g_forgeCreditsRollState = 1;
    Note("forge credits: loot caches will hold Forge Credits at %d %%", g_forgeCreditChance);
}

/* Locked warbonds (research/forge-credits-tint, "Locked warbonds"). The backend serves a premium warbond's token without a
 * price when the live account does not own it (OwnershipFilter.FilterCatalogueResponse); the client takes a warbond with
 * no Super Credits offer for a free one (Helldivers Mobilize) and opens it. These patches make such a warbond, when its
 * variant is premium or legendary (the built-in table 0x32E0C20: Mobilize is 0), count as not owned: its pages stay
 * locked as on the live game, and its button reads HellForge's "WARBOND LOCKED" (strings id 0xF8AD8A16, added by
 * ForgeCredits.cs) instead of "BUY PREMIUM WARBOND":
 * - the warbond screen's data (0x196CE10): "owned = 1" for no offer becomes "owned = variant is free" (sete), and the
 *   owned path's jump into those bytes moves past them;
 * - the warbond list (0x1963860): its count of priced tokens starts at the variant, so a premium one without any goes to
 *   the ownership check instead of "owned";
 * - the button's label refresh (0x1969F30's tail, 0x196A229): a jump to code that writes the label when the button is
 *   the warbond's own (+0x2E78 != 1), its warbond not free, not owned and without an offer, into both its label slots;
 * - the button's state (0x1969E00, 0x1969E1D): such a button takes state 4, an owned warbond's, which takes no hover or
 *   press and shows no price. */
#define FC_LOCKED_LABEL 0xF8AD8A16u
static const ForgeCreditsPatch kLockedWarbondPatches[] = {
    {0x196CE81u, {0x49,0x8b,0x44,0x24,0x08,0xc6,0x40,0x64,0x01,0x49,0x8b,0x44,0x24,0x08}, 14, 0, -1, 14,
        {0x49,0x8b,0x44,0x24,0x08, 0x83,0x78,0x60,0x00, 0x0f,0x94,0x40,0x64, 0x90}},      /* cmp [rax+0x60],0; sete [rax+0x64] */
    {0x196CFDFu, {0xe9,0xa6,0xfe,0xff,0xff}, 5, 0, -1, 5, {0xe9,0xab,0xfe,0xff,0xff}},  /* to 0x196CE8F */
    {0x19639E7u, {0x49,0x8b,0x07,0x44,0x8b,0xff,0x48,0x89,0x43,0x30,0x8b,0x02,0x89,0x43,0x60}, 15, 0, -1, 15,
        {0x49,0x8b,0x07,0x48,0x89,0x43,0x30,0x8b,0x02,0x89,0x43,0x60,0x44,0x8b,0xf8}}};  /* r15d = variant */
#define FC_LABEL_HOOK 0x196A229u
#define FC_STATE_HOOK 0x1969E1Du
#define FC_SET_VISIBLE 0x14508E0u /* (widget, visible) */
static const uint8_t kStateHookPin[] = {0x80,0x78,0x64,0x00,0x74,0x08,0x41,0xb8,0x04,0x00,0x00,0x00}; /* cmp byte [rax+0x64],0; je; mov r8d,4 */
static const uint8_t kLabelHookPin[] = {0xc6,0x87,0x84,0x2e,0x00,0x00,0x01};             /* mov byte [rdi+0x2E84], 1 */
static int g_lockedWarbondsState;

static void InstallLockedWarbonds(void)
{
    if (g_lockedWarbondsState) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    g_lockedWarbondsState = -1;
    if (strcmp(g_build, "25480438") || !game) return;
    size_t count = sizeof kLockedWarbondPatches / sizeof *kLockedWarbondPatches;
    for (size_t i = 0; i < count; i++)
        if (!PinMatches(game + kLockedWarbondPatches[i].rva, kLockedWarbondPatches[i].pin, kLockedWarbondPatches[i].length))
        {
            Note("forge credits: locked warbonds not applied; the code at game.dll+0x%x does not match the reviewed build", kLockedWarbondPatches[i].rva);
            return;
        }
    if (!PinMatches(game + FC_LABEL_HOOK, kLabelHookPin, sizeof kLabelHookPin) || !PinMatches(game + FC_STATE_HOOK, kStateHookPin, sizeof kStateHookPin))
    {
        Note("forge credits: locked warbonds not applied; the button label code does not match the reviewed build");
        return;
    }
    uint8_t* cave = AllocateNearGame(game, 128);
    if (cave == NULL) { Note("forge credits: locked warbonds not applied; no memory near game.dll"); return; }
    /* The moved instruction, then, for the warbond's own button (+0x2E78 != 1): its price (the widget at +0x110, which the
     * refresh shows only when "owned" changes from the warbond shown before, 0x1969FA5; the button is shared, so after a
     * locked warbond it would stay hidden) shown exactly when the warbond can be bought (not owned, a Super Credits offer),
     * and a locked one (not owned, no offer, not free) labelled in both slots; back. */
    uint8_t* c = cave;
    memcpy(c, kLabelHookPin, sizeof kLabelHookPin); c += sizeof kLabelHookPin;
    static const uint8_t checks[] = {
        0x83,0xbf,0x78,0x2e,0x00,0x00,0x01,     /* cmp dword [rdi+0x2E78], 1 */
        0x74,0x3f,                              /* je back */
        0x48,0x8b,0x87,0x58,0x2e,0x00,0x00,     /* mov rax, [rdi+0x2E58] (the warbond's data) */
        0x31,0xd2,                              /* xor edx, edx (price hidden) */
        0x80,0x78,0x64,0x00,                    /* cmp byte [rax+0x64], 0 (owned) */
        0x75,0x24,                              /* jne price */
        0x83,0x78,0x68,0x00,                    /* cmp dword [rax+0x68], 0 (its Super Credits offer) */
        0x74,0x04,                              /* je locked */
        0xb2,0x01,                              /* mov dl, 1 (price shown) */
        0xeb,0x1a,                              /* jmp price */
        0x83,0x78,0x60,0x00,                    /* locked: cmp dword [rax+0x60], 0 (variant: free) */
        0x74,0x14,                              /* je price */
        0xc7,0x87,0x88,0x2e,0x00,0x00};         /* mov dword [rdi+0x2E88], imm32 (the buy label) */
    memcpy(c, checks, sizeof checks); c += sizeof checks;
    uint32_t label = FC_LOCKED_LABEL;
    memcpy(c, &label, 4); c += 4;
    static const uint8_t unlockedSlot[] = {0xc7,0x87,0x8c,0x2e,0x00,0x00}; /* mov dword [rdi+0x2E8C], imm32 (state 4's) */
    memcpy(c, unlockedSlot, sizeof unlockedSlot); c += sizeof unlockedSlot;
    memcpy(c, &label, 4); c += 4;
    static const uint8_t price[] = {0x48,0x8d,0x8f,0x10,0x01,0x00,0x00}; /* price: lea rcx, [rdi+0x110] */
    memcpy(c, price, sizeof price); c += sizeof price;
    int32_t show = (int32_t)((intptr_t)(game + FC_SET_VISIBLE) - (intptr_t)(c + 5));
    *c = 0xe8; memcpy(c + 1, &show, 4); c += 5;                                       /* call game+0x14508E0 (rcx, dl) */
    *c++ = 0xe9;                                /* back: jmp game+0x196A230 */
    int32_t back = (int32_t)((intptr_t)(game + FC_LABEL_HOOK + 7) - (intptr_t)(c + 4));
    memcpy(c, &back, 4); c += 4;
    /* The button's state (0x1969E00): an owned warbond's is 4 (no hover, no press, no price); a locked one's too:
     * if (owned || (+0x2E78 != 1 && variant && !offer)) state 4 */
    uint8_t* state = cave + 96; /* after the label code (84 bytes) */
    static const uint8_t stateChecks[] = {
        0x80,0x78,0x64,0x00,                    /* cmp byte [rax+0x64], 0 (owned) */
        0x75,0x15,                              /* jne four */
        0x83,0xbb,0x78,0x2e,0x00,0x00,0x01,     /* cmp dword [rbx+0x2E78], 1 */
        0x74,0x11,                              /* je normal */
        0x83,0x78,0x60,0x00,                    /* cmp dword [rax+0x60], 0 (variant: free) */
        0x74,0x0b,                              /* je normal */
        0x83,0x78,0x68,0x00,                    /* cmp dword [rax+0x68], 0 (its Super Credits offer) */
        0x75,0x05};                             /* jne normal */
    memcpy(state, stateChecks, sizeof stateChecks);
    uint8_t* s = state + sizeof stateChecks;
    int32_t four = (int32_t)((intptr_t)(game + FC_STATE_HOOK + 6) - (intptr_t)(s + 5));   /* four: jmp game+0x1969E23 */
    *s = 0xe9; memcpy(s + 1, &four, 4); s += 5;
    int32_t normal = (int32_t)((intptr_t)(game + FC_STATE_HOOK + 14) - (intptr_t)(s + 5)); /* normal: jmp game+0x1969E2B */
    *s = 0xe9; memcpy(s + 1, &normal, 4); s += 5;
    DWORD previous;
    VirtualProtect(cave, 0x1000, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), cave, 0x1000);
    for (size_t i = 0; i < count; i++)
    {
        const ForgeCreditsPatch* p = &kLockedWarbondPatches[i];
        if (ApiWrite(game + p->rva, p->with, p->size)) { Note("forge credits: locked warbonds: game.dll+0x%x could not be rewritten", p->rva); return; }
    }
    uint8_t jump[7] = {0xe9, 0, 0, 0, 0, 0x90, 0x90};
    int32_t to = (int32_t)((intptr_t)cave - (intptr_t)(game + FC_LABEL_HOOK + 5));
    memcpy(jump + 1, &to, 4);
    if (ApiWrite(game + FC_LABEL_HOOK, jump, sizeof jump)) { Note("forge credits: locked warbonds: the label hook could not be written"); return; }
    uint8_t stateJump[6] = {0xe9, 0, 0, 0, 0, 0x90};
    int32_t toState = (int32_t)((intptr_t)state - (intptr_t)(game + FC_STATE_HOOK + 5));
    memcpy(stateJump + 1, &toState, 4);
    if (ApiWrite(game + FC_STATE_HOOK, stateJump, sizeof stateJump)) { Note("forge credits: locked warbonds: the button state hook could not be written"); return; }
    g_lockedWarbondsState = 1;
    Note("forge credits: premium warbonds the live account does not own stay locked (WARBOND LOCKED, not pressable)");
}
