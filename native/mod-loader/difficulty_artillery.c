/* The SEAF Artillery at level 11 (research d11-artillery): its reach on larger maps, and its shells
 * (HF_DIFFICULTY_HEAVY_ARTILLERY). Included by hellforge_modloader.c after difficulty_towns.c.
 *
 * The gun fires the projectile of the shell at the head of its queue, and its six shells' rows fly alike: 300 m/s,
 * gravity x 10 (x -9.82), drag 0.5, diameter 360, 70 kg. With drag that reaches about 710 m, enough for a level-10 map
 * (778 m across) but not for a larger one, where the far half is out of reach and the shell lands short. The aim
 * (0x617E50: the solver 0x13B1BD0) and the landing prediction (0x13B18B0) read speed and gravity from the same row the
 * shell flies with (SpawnProjectile 0x13A9830), and nothing clamps the distance.
 *
 * In a mission at the added level, each shell row's speed is multiplied by sqrt(map_scale) and its drag by
 * 1 / map_scale: range is v^2 / g x f(k v^2 / g), so k v^2 / g stays and the whole arc scales by map_scale (about
 * 1420 m at 2 x), its flight time by sqrt(map_scale). The aim follows, as it reads the same rows. Only the SEAF guns
 * fire these types. A row is set again when it no longer holds what was written (the game reloaded its list, or a
 * stats mod changed it: its value is then the one scaled), and put back in a mission at another level only while it
 * still holds what was written. Every player runs it on their own machine, as each simulates the shell. The level is
 * the world's level generator's (lg+0xF9), as the mission stubs read it. */

#define DIF_ARTILLERY_SHELLS 6
#define DIF_ARTILLERY_POLL_MS 500u
#define DIF_SHELL_SPEED 0x20u
#define DIF_SHELL_DRAG 0x28u
/* Explosive, High-Yield Explosive, Mini Nuke, Napalm, Smoke, Static Field (ObjectiveShell +0; 71 is also the gun's). */
static const uint32_t kArtilleryShells[DIF_ARTILLERY_SHELLS] = {71u, 339u, 99u, 145u, 10u, 92u};
typedef struct ArtilleryRow { float base[2], written[2]; int scaled; } ArtilleryRow;
static ArtilleryRow g_artilleryRows[DIF_ARTILLERY_SHELLS];
static int g_artilleryScaled; /* 1 while the rows are the added level's */

/* A shell's projectile row, or 0. */
static uintptr_t ArtilleryRowOf(uintptr_t game, uint32_t type)
{
    uintptr_t row = 0;
    uint32_t stored = 0;
    if (SafeRead(ProjectileTable(game) + (uintptr_t)type * 8, &row, 8) || row == 0 || SafeRead(row, &stored, 4) || stored != type) return 0;
    return row;
}

/* 1 in a mission at the added level (the world's level generator, set before its objectives spawn). */
static int ArtilleryLevel(uintptr_t game)
{
    uint64_t world = 0, level = 0;
    uint8_t difficulty = 0;
    if (SafeRead(game + DIF_WORLD, &world, 8) || world == 0 || SafeRead(world + 0xAE288, &level, 8) || level == 0 ||
        SafeRead(level + 0xF9, &difficulty, 1)) return 0;
    return difficulty == DIF_VANILLA + 1;
}

/* Shell s's speed and drag scaled (again, when the row no longer holds what was written). */
static void ArtilleryScale(uintptr_t row, int s)
{
    static const uint32_t at[2] = {DIF_SHELL_SPEED, DIF_SHELL_DRAG};
    float factor[2] = {sqrtf(g_difficulty.mapScale), 1.0f / g_difficulty.mapScale};
    ArtilleryRow* r = &g_artilleryRows[s];
    for (int k = 0; k < 2; k++)
    {
        float now;
        if (SafeRead(row + at[k], &now, 4) || (r->scaled && now == r->written[k])) continue;
        float value = now * factor[k];
        if (ApiWrite(row + at[k], &value, 4)) continue;
        r->base[k] = now;
        r->written[k] = value;
        r->scaled = 1;
    }
}

/* Shell s's speed and drag put back, where the row still holds what was written. */
static void ArtilleryRestore(uintptr_t row, int s)
{
    static const uint32_t at[2] = {DIF_SHELL_SPEED, DIF_SHELL_DRAG};
    ArtilleryRow* r = &g_artilleryRows[s];
    for (int k = 0; k < 2 && r->scaled; k++)
    {
        float now;
        if (!SafeRead(row + at[k], &now, 4) && now == r->written[k]) ApiWrite(row + at[k], &r->base[k], 4);
    }
    r->scaled = 0;
}

/* Polled by the watch: the shell rows follow the mission's level. */
static void DifficultyArtillery(void)
{
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (!g_difficultyAdded || !LargerMaps() || !g_fired[HF_EVENT_PROJECTILES_READY] || now - last < DIF_ARTILLERY_POLL_MS) return;
    last = now;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0) return;
    int want = ArtilleryLevel(game);
    if (!want && !g_artilleryScaled) return;
    for (int s = 0; s < DIF_ARTILLERY_SHELLS; s++)
    {
        uintptr_t row = ArtilleryRowOf(game, kArtilleryShells[s]);
        if (row == 0) continue;
        if (want) ArtilleryScale(row, s);
        else ArtilleryRestore(row, s);
    }
    if (want != g_artilleryScaled)
        Note("difficulty: the SEAF Artillery's shells %s (speed x %.2f, drag x %.2f)", want ? "reach across the larger map" : "are level 10's again",
            want ? (double)sqrtf(g_difficulty.mapScale) : 1.0, want ? 1.0 / (double)g_difficulty.mapScale : 1.0);
    g_artilleryScaled = want;
}

/* The side objective's shells (HF_DIFFICULTY_HEAVY_ARTILLERY). Its stamps place 12 to 28 spawn points (unit
 * C5A8CB32B972E580), and each rolls one shell (0x9F3250): the unit's script data table 0x3268FC21 lists {shell kind,
 * weight} (Explosive 20, Napalm, Smoke and Static Field 10, High-Yield Explosive 5, Mini Nuke 1), each kind is matched
 * to its ObjectiveShell record (+0x10; a weight above 0 and a projectile only) and one is picked by weight with the
 * objective's seeded random numbers. At 0x9F342F the weight is loaded ([rbx-0x10] its value, [rbx] the kind); in a
 * mission at the added level the stub gives Explosive 10, High-Yield Explosive 5 and Mini Nuke 5 (a quarter of the
 * shells nukes) and every other kind 0, which the roll skips. Every machine rolls alike: they all run HellForge. */
#define DIF_SHELL_WEIGHT_SITE 0x9F342Fu  /* mov rax, [rbx-0x10]; movss xmm0, [rax] (8 bytes) */
#define DIF_SHELL_WEIGHT_BACK 0x9F3437u  /* comiss xmm7, xmm0; jae (the kind skipped) */
#define DIF_SHELL_KIND_READ 0x9F3449u    /* mov r11d, [rbx]: the kind, matched to the records */
static const uint8_t kShellWeightPin[17] = {0x48, 0x8B, 0x43, 0xF0, 0xF3, 0x0F, 0x10, 0x00, 0x0F, 0x2F, 0xF8, 0x0F, 0x83, 0xC9, 0x00, 0x00, 0x00};
static const uint8_t kShellKindPin[3] = {0x44, 0x8B, 0x1B};
static const uint8_t kShellWeightStub[95] = {
    0x48, 0x8B, 0x43, 0xF0,                                           /*  0 mov rax, [rbx-0x10] */
    0xF3, 0x0F, 0x10, 0x00,                                           /*  4 movss xmm0, [rax] (the weight) */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  8 mov rax, game+0x3326340 (imm64 at 10) */
    0x48, 0x8B, 0x00,                                                 /* 18 mov rax, [rax] (the world) */
    0x48, 0x85, 0xC0,                                                 /* 21 test rax, rax */
    0x74, 0x40,                                                       /* 24 je 90 */
    0x48, 0x8B, 0x80, 0x88, 0xE2, 0x0A, 0x00,                         /* 26 mov rax, [rax+0xae288] (the level) */
    0x48, 0x85, 0xC0,                                                 /* 33 test rax, rax */
    0x74, 0x34,                                                       /* 36 je 90 */
    0x80, 0xB8, 0xF9, 0x00, 0x00, 0x00, 0x0B,                         /* 38 cmp byte [rax+0xf9], 11 */
    0x75, 0x2B,                                                       /* 45 jne 90 */
    0x8B, 0x03,                                                       /* 47 mov eax, [rbx] (the kind) */
    0x3D, 0x03, 0x3D, 0x55, 0x03,                                     /* 49 cmp eax, Explosive */
    0x74, 0x12,                                                       /* 54 je 74 */
    0x3D, 0x80, 0x48, 0x37, 0x1A,                                     /* 56 cmp eax, High-Yield Explosive */
    0x74, 0x12,                                                       /* 61 je 81 */
    0x3D, 0xCE, 0x10, 0x95, 0xAE,                                     /* 63 cmp eax, Mini Nuke */
    0x74, 0x0B,                                                       /* 68 je 81 */
    0x31, 0xC0,                                                       /* 70 xor eax, eax (0.0: skipped) */
    0xEB, 0x0C,                                                       /* 72 jmp 86 */
    0xB8, 0x00, 0x00, 0x20, 0x41,                                     /* 74 mov eax, 10.0 */
    0xEB, 0x05,                                                       /* 79 jmp 86 */
    0xB8, 0x00, 0x00, 0xA0, 0x40,                                     /* 81 mov eax, 5.0 */
    0x66, 0x0F, 0x6E, 0xC0,                                           /* 86 movd xmm0, eax */
    0xE9, 0, 0, 0, 0};                                                /* 90 jmp 0x9F3437 (rel32 at 91) */

static int HeavyArtillery(void) { return (g_difficulty.flags & HF_DIFFICULTY_HEAVY_ARTILLERY) != 0; }

static const char* ArtilleryPinsFailed(uintptr_t game)
{
    if (!HeavyArtillery()) return NULL;
    uint8_t bytes[17];
    if (SafeRead(game + DIF_SHELL_WEIGHT_SITE, bytes, 17) || memcmp(bytes, kShellWeightPin, 17) != 0 ||
        SafeRead(game + DIF_SHELL_KIND_READ, bytes, 3) || memcmp(bytes, kShellKindPin, 3) != 0)
        return "the SEAF Artillery's shell roll is not the reviewed one (HF_DIFFICULTY_HEAVY_ARTILLERY needs it)";
    return NULL;
}

/* The stub on a page of its own, then the weight's load sent to it. */
static const char* InstallArtilleryShells(DifficultyInstall* d)
{
    if (!HeavyArtillery()) return NULL;
    uintptr_t game = d->game;
    uint8_t* code = AllocateNearGame(game, 0x1000);
    if (code == NULL) return "no memory within 2 GB of game.dll for the SEAF Artillery's shells";
    uint64_t world = game + DIF_WORLD;
    memcpy(code, kShellWeightStub, sizeof kShellWeightStub);
    memcpy(code + 10, &world, 8);
    int32_t rel = (int32_t)(int64_t)(game + DIF_SHELL_WEIGHT_BACK - (uintptr_t)(code + 95));
    memcpy(code + 91, &rel, 4);
    DWORD protect;
    if (!VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &protect)) return "the SEAF Artillery's shell stub could not be made executable";
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    rel = (int32_t)(int64_t)((uintptr_t)code - (game + DIF_SHELL_WEIGHT_SITE + 5));
    memcpy(jump + 1, &rel, 4);
    if (ApiWrite(game + DIF_SHELL_WEIGHT_SITE, jump, 8)) return "the SEAF Artillery's shell roll could not be patched";
    return NULL;
}
