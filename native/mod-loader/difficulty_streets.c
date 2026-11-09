/* City streets on level 11's larger maps (research d11-city-blocks §4.1, §4.3). Included by hellforge_modloader.c after
 * difficulty_towns.c.
 *
 * FlattenCityRoads (0xFAF130) dresses each city street: the road ribbons and junction discs everywhere, but the pieces
 * at both ends of an edge (+0x240/+0x250) and the spaced street props (+0x270) only up to their reach (+0x258, +0x280:
 * 30 m on 18 of 22 city types) past the playable radius R; the road-prop placer (0xFB1DB0) tests every position the same
 * way against its rule's +0x54 (0 or 30 m). At level 10 that band is scenery the cull strips. On a larger map the
 * buildings stand up to the border (difficulty_towns.c's S1-S5, and InstallCull keeps their units), so from R + 30 m to
 * the border the streets were bare ribbons and lone discs. These four tests measure from the border too, with the same
 * stub as S1-S5 (lg+0x2D74, base +0x54 instead of +0x50), and with the same switch (HELLFORGE_D11_TOWN_BORDER=0).
 *
 * The road props per edge are a fixed count, rand % type+0x230 + 1 (N = 4, 6 or 8; 0xFB2925), so on a larger map's 1.5 x
 * longer edges they were 2/3 as dense. There N becomes N x 3/2 (at most 12 of the buffer's 64; off with
 * HELLFORGE_D11_STREET_PROPS=0, for comparing). */

/* {site, length, the instruction (R at +0x50; its last byte 0x50 becomes 0x54, the border), cmp dword [base+0x5c],
 * back}: the base is lg+0x2D20 at all four, the flags are dead (the next flag writer is a ucomiss with no reader
 * before it) and nothing branches into them. */
static const struct { uint32_t site; uint8_t length; uint8_t pin[6]; uint8_t cmp[3]; uint32_t back; } kStreetBorderSites[4] = {
    {0xFB20E1u, 6, {0xF3, 0x44, 0x0F, 0x59, 0x41, 0x50}, {0x81, 0x79, 0x5C}, 0xFB20E7u}, /* road props per position: mulss xmm8, [rcx+0x50] */
    {0xFB0B8Eu, 5, {0xF3, 0x0F, 0x59, 0x79, 0x50}, {0x81, 0x79, 0x5C}, 0xFB0B93u},       /* spaced street props +0x270: mulss xmm7, [rcx+0x50] */
    {0xFB1474u, 5, {0xF3, 0x0F, 0x59, 0x71, 0x50}, {0x81, 0x79, 0x5C}, 0xFB1479u},       /* edge-end pieces: mulss xmm6, [rcx+0x50] */
    {0xFB16E7u, 5, {0xF3, 0x0F, 0x59, 0x71, 0x50}, {0x81, 0x79, 0x5C}, 0xFB16ECu}};      /* the same, other end */
#define DIF_STREET_PROPS_SITE 0xFB2925u /* div dword [rbp+0x230] */
#define DIF_STREET_PROPS_BACK 0xFB292Bu
static const uint8_t kStreetPropsPin[6] = {0xF7, 0xB5, 0x30, 0x02, 0x00, 0x00};
/* r9 and r11 are free (the memset after it writes them); [rsp+0xb0] is the pass context (0xFB25B6), +8 = lg+0x2D20. */
static const uint8_t kStreetPropsStub[44] = {
    0x44, 0x8B, 0x8D, 0x30, 0x02, 0x00, 0x00,                         /*  0 mov r9d, [rbp+0x230] (N) */
    0x4C, 0x8B, 0x9C, 0x24, 0xB0, 0x00, 0x00, 0x00,                   /*  7 mov r11, [rsp+0xb0] (the context) */
    0x4D, 0x8B, 0x5B, 0x08,                                           /* 15 mov r11, [r11+8] (lg+0x2D20) */
    0x41, 0x81, 0x7B, 0x5C, 0, 0, 0, 0,                               /* 19 cmp dword [r11+0x5c], the larger disk (at 23) */
    0x75, 0x07,                                                       /* 27 jne 36 */
    0x47, 0x8D, 0x0C, 0x49,                                           /* 29 lea r9d, [r9+r9*2] */
    0x41, 0xD1, 0xE9,                                                 /* 33 shr r9d, 1 (N x 3/2) */
    0x41, 0xF7, 0xF1,                                                 /* 36 div r9d */
    0xE9, 0, 0, 0, 0};                                                /* 39 jmp 0xFB292B (rel32 at 40) */

static int StreetPropsWanted(void)
{
    wchar_t value[8];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_D11_STREET_PROPS", value, 8);
    return !(length == 1 && value[0] == L'0');
}

static const char* StreetPinsFailed(uintptr_t game)
{
    if (!LargerMaps()) return NULL;
    uint8_t bytes[6];
    for (int i = 0; i < 4; i++)
        if (SafeRead(game + kStreetBorderSites[i].site, bytes, kStreetBorderSites[i].length) ||
            memcmp(bytes, kStreetBorderSites[i].pin, kStreetBorderSites[i].length) != 0)
            return "the city streets' dressing is not the reviewed one (a map_scale above 1 needs it)";
    if (SafeRead(game + DIF_STREET_PROPS_SITE, bytes, 6) || memcmp(bytes, kStreetPropsPin, 6) != 0)
        return "the city streets' road props are not the reviewed ones (a map_scale above 1 needs them)";
    return NULL;
}

/* The stubs on a page of their own, then each wanted site sent to its stub. */
static const char* InstallStreets(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    int border = TownBorderWanted(), props = StreetPropsWanted();
    if (!border && !props) return NULL;
    uintptr_t game = d->game;
    uint8_t* code = AllocateNearGame(game, 0x1000);
    if (code == NULL) return "no memory within 2 GB of game.dll for the city streets";
    float diskBits = d->disk / 1024.0f;
    uintptr_t stubs[5];
    uint8_t* p = code;
    for (int i = 0; i < 4; i++) /* cmp [base+0x5c], disk; jne vanilla; the border's mulss; jmp back; R's mulss; jmp back */
    {
        uint8_t length = kStreetBorderSites[i].length;
        stubs[i] = (uintptr_t)p;
        memcpy(p, kStreetBorderSites[i].cmp, 3); p += 3;
        memcpy(p, &diskBits, 4); p += 4;
        *p++ = 0x75; *p++ = (uint8_t)(length + 5);
        memcpy(p, kStreetBorderSites[i].pin, length); p[length - 1] = 0x54; p += length;
        p = TownJump(p, game + kStreetBorderSites[i].back);
        memcpy(p, kStreetBorderSites[i].pin, length); p += length;
        p = TownJump(p, game + kStreetBorderSites[i].back);
    }
    p = (uint8_t*)(((uintptr_t)p + 15) & ~(uintptr_t)15);
    stubs[4] = (uintptr_t)p;
    memcpy(p, kStreetPropsStub, sizeof kStreetPropsStub);
    memcpy(p + 23, &diskBits, 4);
    TownRel32(p + 40, game + DIF_STREET_PROPS_BACK);
    DWORD protect;
    if (!VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &protect)) return "the city streets' stubs could not be made executable";
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    for (int i = border ? 0 : 4; i < 5; i++)
    {
        if (i == 4 && !props) break;
        uint32_t site = i < 4 ? kStreetBorderSites[i].site : DIF_STREET_PROPS_SITE;
        uint8_t length = i < 4 ? kStreetBorderSites[i].length : 6;
        uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
        int32_t rel = (int32_t)(int64_t)(stubs[i] - (game + site + 5));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(game + site, jump, length)) return "the city streets could not be patched";
    }
    Note("difficulty: city streets on larger maps %s; their road props %s (HELLFORGE_D11_STREET_PROPS=0 keeps level 10's)",
        border ? "are dressed up to the border" : "keep level 10's dressing radius", props ? "x 1.5 per edge" : "as level 10's");
    return NULL;
}
