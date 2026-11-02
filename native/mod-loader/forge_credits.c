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
