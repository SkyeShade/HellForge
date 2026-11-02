/* Build 25480438: the mission HUD's loot counters (requisition, Super Credits...) pick each type's sprite and colour in a
 * switch in 0x198B960. The Super Credits case at 0x198BACE loads its colour (A, R, G, B floats: 1, 113, 245, 255 / 255, the
 * game's Super Credits cyan) from 0x21CAE90, beside its sprite 0x9414AB4F02745820, which ForgeCredits.cs redraws as the
 * Forge Credit. That one load is pointed at HellForge red instead; the cyan constant stays as it is for its other reader
 * (0x1977612). Included after damage.c (AllocateNearGame) and packages.c (PinMatches). */
#define FORGE_CREDITS_COLOUR_LOAD 0x198BACEu
static const uint8_t kForgeCreditsCase[] = {
    0x0f,0x28,0x05,0xbb,0xf3,0x83,0x00,                     /* movaps xmm0, [rip + 0x83F3BB] (0x21CAE90) */
    0x48,0xbb,0x20,0x58,0x74,0x02,0x4f,0xab,0x14,0x94};     /* movabs rbx, 0x9414AB4F02745820 (the sprite) */
/* HellForge red, (229, 57, 43): A, R, G, B as the HUD reads them */
static const float kForgeCreditsRed[4] = {1.0f, 229.0f / 255.0f, 57.0f / 255.0f, 43.0f / 255.0f};
static int g_forgeCreditsState;

static void InstallForgeCreditsColour(void)
{
    if (g_forgeCreditsState) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (strcmp(g_build, "25480438") || !game) { g_forgeCreditsState = -1; return; }
    g_forgeCreditsState = -1;
    if (!PinMatches(game + FORGE_CREDITS_COLOUR_LOAD, kForgeCreditsCase, sizeof kForgeCreditsCase))
    {
        Note("forge credits: HUD colour not changed; the loot counter's code does not match the reviewed build");
        return;
    }
    uint8_t* colour = AllocateNearGame(game, 16); /* a page, 16-byte aligned for movaps */
    if (colour == NULL) { Note("forge credits: HUD colour not changed; no memory near game.dll"); return; }
    memcpy(colour, kForgeCreditsRed, sizeof kForgeCreditsRed);
    DWORD previous;
    VirtualProtect(colour, 0x1000, PAGE_READONLY, &previous);
    int32_t disp = (int32_t)((intptr_t)colour - (intptr_t)(game + FORGE_CREDITS_COLOUR_LOAD + 7));
    if (ApiWrite(game + FORGE_CREDITS_COLOUR_LOAD + 3, &disp, sizeof disp)) { Note("forge credits: HUD colour not changed; the load could not be rewritten"); return; }
    g_forgeCreditsState = 1;
    Note("forge credits: the HUD's Super Credits counter is drawn in HellForge red");
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
