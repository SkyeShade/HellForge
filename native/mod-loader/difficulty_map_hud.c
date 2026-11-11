/* The in-mission map (HUD object [game+0x346D538]+0x24E340+0x1A0E28) on an extra large map. Its zoom levels are 2
 * (3 px/m), 3 (1.5 px/m) and 4, a fit to the whole world (0x18AC410): 16 km in a 448 px element, where the 8 m map
 * texels are under a pixel. On a world above 2048 m the map (shown while Tab is held) zooms between level 3 (0.45 px/m:
 * the map texture holds 8 m a texel, so 1.5 and 3 px/m only magnify blocks) and 4 (2048 m around the player), and one
 * step further out to level 5: the whole world in a full-screen element. Letting go of Tab drops level 5 to 4. Level
 * 10's 2048 m worlds keep every vanilla value. Included by hellforge_modloader.c after difficulty_cities.c (the stub
 * macros). */

#define DIF_HUD_ZOOM_SITE 0x18AC461u     /* lea eax, [r9-1]; mov ecx, 2 (9 bytes): the next level and its scale */
#define DIF_HUD_ZOOM_DONE 0x18AC540u     /* the level is unchanged */
#define DIF_HUD_ZOOM_SET 0x18AC4F5u      /* r8d the new level, xmm2 its scale */
#define DIF_HUD_COLLAPSE_SITE 0x18ACDC3u /* xorps xmm1, xmm1; mov dword [rdi+0x1B8], 0 (13 bytes) */
#define DIF_HUD_COLLAPSE_NEXT 0x18ACDD0u
#define DIF_HUD_MODE_SITE 0x18A7DCAu     /* mov ecx, [rax+0xAC518] (6 bytes): the map placement setting */
#define DIF_HUD_MODE_NEXT 0x18A7DD0u
#define DIF_HUD_SIZE_SITE 0x18A7FE6u     /* mov rdx, [rip+648 x 648] (7 bytes): the centred element's size */
#define DIF_HUD_SIZE_NEXT 0x18A7FEDu
#define DIF_HUD_CENTRE_SIZE 0x21E68F8u
#define DIF_HUD_CORNER_SIZE 0x21E7498u
#define DIF_HUD_FULL 960.0f /* UI px on the 1920 x 1080 reference canvas; with the 64 px frame it fits 1080 */
#define DIF_HUD_NEAR 2048.0f
#define DIF_HUD_CLOSE 0.45f /* px/m: 3.6 px a map texel; about 1 km across the corner map */
#define DIF_HUD_CLAMP_SITE 0x18AB691u /* minss xmm13, [rip+3.0] (9 bytes): the scale's upper bound, on every draw */
#define DIF_HUD_CLAMP_NEXT 0x18AB69Au
#define DIF_HUD_THREE 0x23C7280u


static uintptr_t g_hudGame;

static int HudLarge(uintptr_t map) { return *(const float*)(map + 0x140) > DIF_HUD_NEAR + 0.5f; }
static int HudFull(uintptr_t map) { return HudLarge(map) && *(const uint8_t*)(map + 0x195) && *(const uint32_t*)(map + 0x1A4) == 5; }

/* The element's size outside level 5: the corner's 448, or the centred 648 when the placement setting is 1. */
static float HudNormalSize(uint32_t setting)
{
    return *(const float*)(g_hudGame + (setting == 1 ? DIF_HUD_CENTRE_SIZE : DIF_HUD_CORNER_SIZE));
}

static uint32_t HudSetting(void)
{
    uintptr_t settings = *(const uintptr_t*)(g_hudGame + 0x3326340u);
    return settings ? *(const uint32_t*)(settings + 0xAC518u) : 0;
}

/* The fit scale of 0x18AC4B7..0x18AC4D7: the larger of W and H over (playable fraction + 0.01) x span. */
static float HudFit(uintptr_t map, float w, float h, float span)
{
    uintptr_t lg = *(const uintptr_t*)(map + 0x118);
    float d = (*(const float*)(lg + 0x2D70) + 0.01f) * span, a = w / d, b = h / d;
    return a > b ? a : b;
}

static float HudNearFit(uintptr_t map)
{
    float size = HudNormalSize(HudSetting());
    return HudFit(map, size, size, DIF_HUD_NEAR);
}

/* A large world's level scale: 3 is the closest the map texture allows, 4 the near fit, 5 the whole world. */
static float HudLargeScale(uintptr_t map, uint32_t level)
{
    if (level == 5) return HudFit(map, DIF_HUD_FULL, DIF_HUD_FULL, *(const float*)(map + 0x140));
    if (level == 4) return HudNearFit(map);
    return DIF_HUD_CLOSE;
}

/* 0x18AC461..0x18AC4F5 as the game computes it; on a large world levels 3..4, and 5 while expanded. */
static uint64_t HudZoomTarget(uintptr_t map, uint32_t level, float input, float* scale)
{
    int large = HudLarge(map);
    uint32_t top = large && *(const uint8_t*)(map + 0x195) ? 5u : 4u, closest = large ? 3u : 2u;
    uint32_t in = level - 1 > closest ? level - 1 : closest, out = level + 1 < top ? level + 1 : top;
    uint32_t next = input > 0.0f ? in : out;
    float s = 1.5f;
    if (large) s = HudLargeScale(map, next);
    else if (next == 2) s = 3.0f;
    else if (next == 4) s = HudFit(map, *(const float*)(map + 0x180), *(const float*)(map + 0x184), *(const float*)(map + 0x140));
    *scale = s;
    return next;
}

/* A level and its scale at once: no zoom animation between the corner map and the whole world. */
static void HudSetLevel(uintptr_t map, uint32_t level, float s)
{
    *(uint32_t*)(map + 0x1A0) = level;
    *(uint32_t*)(map + 0x1A4) = level;
    *(float*)(map + 0x148) = s;
    *(float*)(map + 0x1A8) = s;
    *(float*)(map + 0x1AC) = s;
    *(float*)(map + 0x1B0) = *(const float*)(map + 0x1B4);
}

/* Letting go of Tab at level 5: level 4, at once (no zoom animation from the whole world). */
static void HudCollapse(uintptr_t map)
{
    if (HudLarge(map) && *(const uint32_t*)(map + 0x1A4) == 5) HudSetLevel(map, 4, HudLargeScale(map, 4));
}

/* The layout runs only when the element's pivot (+0x290C) differs from its placement's, so a size change at the
 * same placement (648 to 960 and back) clears the pivot to make it run. Level 5 is laid out as the centred map. */
static uint64_t HudLayoutMode(uintptr_t map, uint32_t setting)
{
    if (!HudLarge(map) || setting > 1) return setting;
    int full = HudFull(map);
    float want = full ? DIF_HUD_FULL : HudNormalSize(setting);
    if (*(const float*)(map + 0x180) != want) *(float*)(map + 0x290C) = -1.0f;
    return full ? 1u : setting;
}

static uint64_t HudCentreSize(uintptr_t map)
{
    if (!HudFull(map)) return *(const uint64_t*)(g_hudGame + DIF_HUD_CENTRE_SIZE);
    float size[2] = {DIF_HUD_FULL, DIF_HUD_FULL};
    uint64_t packed;
    memcpy(&packed, size, 8);
    return packed;
}

/* Each stub returns its results through STUB_ENTER's register slots. */
static const uint8_t kHudZoomStub[] = {
    STUB_ENTER,                                          /*   0 */
    0x48, 0x89, 0xD9,                                   /*  54 mov rcx, rbx (the map) */
    0x44, 0x89, 0xCA,                                   /*  57 mov edx, r9d (the level) */
    0x0F, 0x28, 0xD1,                                   /*  60 movaps xmm2, xmm1 (the input) */
    0x4C, 0x8D, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00,     /*  63 lea r9, [rsp+0x80] (the scale) */
    STUB_CALL_IMM64,                                    /*  71 HudZoomTarget (imm64 at 73) */
    0x48, 0x89, 0x84, 0x24, 0xA0, 0x00, 0x00, 0x00,     /*  83 mov [rsp+0xA0], rax (r8: the new level) */
    0x8B, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00,           /*  91 mov ecx, [rsp+0x80] */
    0x89, 0x4C, 0x24, 0x40,                             /*  98 mov [rsp+0x40], ecx (xmm2: its scale) */
    STUB_LEAVE,                                          /* 102 */
    0x89, 0xD0,                                         /* 156 mov eax, edx (as 0x18AC480) */
    0x45, 0x39, 0xC1,                                   /* 158 cmp r9d, r8d */
    0x0F, 0x84, 0, 0, 0, 0,                             /* 161 je 0x18AC540 (rel32 at 163) */
    0xE9, 0, 0, 0, 0};                                  /* 167 jmp 0x18AC4F5 (rel32 at 168) */
_Static_assert(sizeof kHudZoomStub == 172, "the zoom stub's offsets");

static const uint8_t kHudCollapseStub[] = {
    STUB_ENTER,                                          /*   0 */
    0x48, 0x89, 0xF9,                                   /*  54 mov rcx, rdi (the map) */
    STUB_CALL_IMM64,                                    /*  57 HudCollapse (imm64 at 59) */
    STUB_LEAVE,                                          /*  69 */
    0x0F, 0x57, 0xC9,                                   /* 123 xorps xmm1, xmm1 (the game's) */
    0xC7, 0x87, 0xB8, 0x01, 0x00, 0x00, 0, 0, 0, 0,     /* 126 mov dword [rdi+0x1B8], 0 (the game's) */
    0xE9, 0, 0, 0, 0};                                  /* 136 jmp 0x18ACDD0 (rel32 at 137) */
_Static_assert(sizeof kHudCollapseStub == 141, "the collapse stub's offsets");

static const uint8_t kHudModeStub[] = {
    STUB_ENTER,                                          /*   0 */
    0x8B, 0x90, 0x18, 0xC5, 0x0A, 0x00,                 /*  54 mov edx, [rax+0xAC518] (the setting) */
    0x48, 0x89, 0xE9,                                   /*  60 mov rcx, rbp (the map) */
    STUB_CALL_IMM64,                                    /*  63 HudLayoutMode (imm64 at 65) */
    0x48, 0x89, 0x84, 0x24, 0xB0, 0x00, 0x00, 0x00,     /*  75 mov [rsp+0xB0], rax (rcx: the placement) */
    STUB_LEAVE,                                          /*  83 */
    0xE9, 0, 0, 0, 0};                                  /* 137 jmp 0x18A7DD0 (rel32 at 138) */
_Static_assert(sizeof kHudModeStub == 142, "the layout stub's offsets");

static const uint8_t kHudSizeStub[] = {
    STUB_ENTER,                                          /*   0 */
    0x48, 0x89, 0xE9,                                   /*  54 mov rcx, rbp (the map) */
    STUB_CALL_IMM64,                                    /*  57 HudCentreSize (imm64 at 59) */
    0x48, 0x89, 0x84, 0x24, 0xA8, 0x00, 0x00, 0x00,     /*  69 mov [rsp+0xA8], rax (rdx: the size) */
    STUB_LEAVE,                                          /*  77 */
    0xE9, 0, 0, 0, 0};                                  /* 131 jmp 0x18A7FED (rel32 at 132) */
_Static_assert(sizeof kHudSizeStub == 136, "the size stub's offsets");

/* The clamp 0x18AB5C0 (rcx the map; xmm0 and the flags are free until 0x18AB6A3): the game's bound of 3 px/m, and on
 * a world above 2048 m also DIF_HUD_CLOSE. It runs on every draw, so the first open's 1.5 px/m is held there too. */
static const uint8_t kHudClampStub[48] = {
    0xF3, 0x44, 0x0F, 0x5D, 0x2D, 0, 0, 0, 0,           /*  0 minss xmm13, [rip+3.0] (the game's; rel32 at 5) */
    0xF3, 0x0F, 0x10, 0x81, 0x40, 0x01, 0x00, 0x00,     /*  9 movss xmm0, [rcx+0x140] (S) */
    0x0F, 0x2E, 0x05, 0x10, 0x00, 0x00, 0x00,           /* 17 ucomiss xmm0, [rip+16] (2048.5 at 40) */
    0x76, 0x09,                                         /* 24 jbe 35 (not larger, or NaN) */
    0xF3, 0x44, 0x0F, 0x5D, 0x2D, 0x09, 0x00, 0x00, 0x00, /* 26 minss xmm13, [rip+9] (DIF_HUD_CLOSE at 44) */
    0xE9, 0, 0, 0, 0,                                   /* 35 jmp 0x18AB69A (rel32 at 36) */
    0x00, 0x08, 0x00, 0x45,                             /* 40 2048.5f */
    0x66, 0x66, 0xE6, 0x3E};                            /* 44 0.45f */

static const struct
{
    uint32_t site, length;
    const char* pin;
    const uint8_t* stub;
    uint32_t size, fnAt;
    void* fn;
    struct { uint32_t at, target; } jumps[2];
} kHudSites[5] = {
    {DIF_HUD_ZOOM_SITE, 9, "41 8D 41 FF B9 02 00 00 00", kHudZoomStub, sizeof kHudZoomStub, 73, (void*)HudZoomTarget,
     {{163, DIF_HUD_ZOOM_DONE}, {168, DIF_HUD_ZOOM_SET}}},
    {DIF_HUD_COLLAPSE_SITE, 13, "0F 57 C9 C7 87 B8 01 00 00 00 00 00 00", kHudCollapseStub, sizeof kHudCollapseStub, 59, (void*)HudCollapse,
     {{137, DIF_HUD_COLLAPSE_NEXT}, {0, 0}}},
    {DIF_HUD_MODE_SITE, 6, "8B 88 18 C5 0A 00", kHudModeStub, sizeof kHudModeStub, 65, (void*)HudLayoutMode,
     {{138, DIF_HUD_MODE_NEXT}, {0, 0}}},
    {DIF_HUD_SIZE_SITE, 7, "48 8B 15 0B E9 93 00", kHudSizeStub, sizeof kHudSizeStub, 59, (void*)HudCentreSize,
     {{132, DIF_HUD_SIZE_NEXT}, {0, 0}}},
    {DIF_HUD_CLAMP_SITE, 9, "F3 44 0F 5D 2D E6 BB B1 00", kHudClampStub, sizeof kHudClampStub, 0, NULL,
     {{5, DIF_HUD_THREE}, {36, DIF_HUD_CLAMP_NEXT}}}};
#define DIF_HUD_SITES (sizeof kHudSites / sizeof kHudSites[0])

/* Larger maps only. Every site is checked before any is written: a game whose code differs keeps the vanilla map. */
static const char* InstallMapHud(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    for (size_t i = 0; i < DIF_HUD_SITES; i++)
    {
        if (ApiCodeMatches(d->game + kHudSites[i].site, kHudSites[i].pin) != 1)
        {
            Note("difficulty: map HUD: the code at game+0x%X is not the reviewed one; the vanilla map zoom stays", kHudSites[i].site);
            return NULL;
        }
    }
    uint8_t* page = AllocateNearGame(d->game, 0x1000);
    if (page == NULL) { Note("difficulty: map HUD: no page near the game; the vanilla map zoom stays"); return NULL; }
    g_hudGame = d->game;
    for (size_t i = 0; i < DIF_HUD_SITES; i++)
    {
        uint8_t* stub = page + 0x100 * i;
        memcpy(stub, kHudSites[i].stub, kHudSites[i].size);
        uintptr_t fn = (uintptr_t)kHudSites[i].fn;
        if (fn) memcpy(stub + kHudSites[i].fnAt, &fn, 8);
        for (size_t j = 0; j < 2 && kHudSites[i].jumps[j].at; j++)
        {
            int32_t rel = (int32_t)(d->game + kHudSites[i].jumps[j].target - (uintptr_t)(stub + kHudSites[i].jumps[j].at + 4));
            memcpy(stub + kHudSites[i].jumps[j].at, &rel, 4);
        }
    }
    DWORD protect;
    if (!VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &protect)) return "the map HUD's stubs could not be made executable";
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    for (size_t i = 0; i < DIF_HUD_SITES; i++)
    {
        uint8_t jump[16];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        int32_t rel = DifficultyJumpRel(d->game + kHudSites[i].site, (uintptr_t)(page + 0x100 * i));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(d->game + kHudSites[i].site, jump, kHudSites[i].length)) return "the map HUD's zoom could not be installed";
    }
    Note("difficulty: map HUD: near fit %.0f m, closest %.2f px/m, full-screen level 5 on the expanded map", (double)DIF_HUD_NEAR, (double)DIF_HUD_CLOSE);
    return NULL;
}

