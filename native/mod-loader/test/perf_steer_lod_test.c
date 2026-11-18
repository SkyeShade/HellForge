/* The steer-lod fix (perf_steer_lod.c) on a synthetic game image holding the reviewed bytes: the steering pass is hooked
 * for real and a stand-in behind its moved instructions counts each instance's updates. Positions and Helldivers come
 * from the fixture. Included by stats_test.c after perf_vehicle_box_test.c (VbtFar). */

#define SLT_GAME (SL_MANAGER + 0x1000u)
#define SLT_AGENTS 30u

static uint8_t* g_sltGame;
static int g_sltUpdates[SLT_AGENTS];
static int g_sltNoPlayers;

static void SltPassBody(void* unused, uint32_t start, uint32_t end, float dt)
{
    (void)unused; (void)dt;
    for (uint32_t i = start; i < end && i < SLT_AGENTS; i++) g_sltUpdates[i]++;
}

/* Unit k + 1: the first 10 within 10 m of the Helldiver, the rest 200 m away. */
static int SltPosition(uint32_t unit, float* p)
{
    if (unit == 0 || unit > SLT_AGENTS) return 0;
    p[0] = unit <= 10 ? 10.0f : 200.0f; p[1] = 0.0f; p[2] = 0.0f;
    return 1;
}

static int SltPlayers(float (*out)[3], int room)
{
    if (g_sltNoPlayers || room < 1) return 0;
    out[0][0] = out[0][1] = out[0][2] = 0.0f;
    return 1;
}

static void SteerLodFixture(void)
{
    g_sltGame = (uint8_t*)VirtualAlloc(NULL, SLT_GAME, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(g_sltGame != NULL);
    uintptr_t game = (uintptr_t)g_sltGame;
    memcpy(g_sltGame + VB_STEER, kVbSteerPin, sizeof kVbSteerPin);
    for (size_t i = 0; i < SL_PINS; i++) memcpy(g_sltGame + kSlPins[i].rva, kSlPins[i].bytes, kSlPins[i].size);
    assert(SteerLodMismatch(game) == NULL);
    g_sltGame[VB_STEER + 4] ^= 1;
    assert(SteerLodMismatch(game) != NULL);
    g_sltGame[VB_STEER + 4] ^= 1;
    for (size_t i = 0; i < SL_PINS; i++)
    {
        g_sltGame[kSlPins[i].rva + 1] ^= 1;
        assert(SteerLodMismatch(game) == kSlPins[i].what);
        g_sltGame[kSlPins[i].rva + 1] ^= 1;
    }
    g_slPosition = SltPosition;
    g_slPlayers = SltPlayers;
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "steer-lod");
    assert(SteerLodTry(game, 0) == -1 && !g_slOn);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    assert(SteerLodTry(game, 0) == 1 && g_slOn);
    assert(SteerLodMismatch(game) == NULL);
    VbtFar(g_sltGame + VB_STEER + 7, (void*)SltPassBody); /* mov r11, rsp; mov [r11+8], rbx moved */
    FlushInstructionCache(GetCurrentProcess(), g_sltGame, SLT_GAME);

    /* the steering component: 30 instances, descriptor k with unit k + 1 at +0xC */
    static uint8_t manager[0x60], descriptors[SLT_AGENTS][0x10];
    static uintptr_t table[SLT_AGENTS];
    for (uint32_t k = 0; k < SLT_AGENTS; k++) { *(uint32_t*)(descriptors[k] + 0xC) = k + 1; table[k] = (uintptr_t)descriptors[k]; }
    *(uint32_t*)(manager + SL_COUNT_AT) = SLT_AGENTS;
    *(uintptr_t*)(manager + SL_DESCRIPTORS_AT) = (uintptr_t)table;
    *(uintptr_t*)(g_sltGame + SL_MANAGER) = (uintptr_t)manager;
    SlPassFn pass = (SlPassFn)(void*)(g_sltGame + SL_STEER);

    /* 4 frames to read every distance, then 9 frames: near every frame, far one frame in three */
    for (int f = 0; f < 4; f++) pass(NULL, 0, SLT_AGENTS, 0.016f);
    memset(g_sltUpdates, 0, sizeof g_sltUpdates);
    for (int f = 0; f < 9; f++) pass(NULL, 0, SLT_AGENTS, 0.016f);
    for (uint32_t k = 0; k < SLT_AGENTS; k++) assert(g_sltUpdates[k] == (k < 10 ? 9 : 3));

    /* a range past the count still reaches the game's pass; no Helldiver read: everyone every frame */
    memset(g_sltUpdates, 0, sizeof g_sltUpdates);
    g_sltNoPlayers = 1;
    for (int f = 0; f < 3; f++) pass(NULL, 0, SLT_AGENTS + 5, 0.016f);
    for (uint32_t k = 0; k < SLT_AGENTS; k++) assert(g_sltUpdates[k] == 3);
    g_sltNoPlayers = 0;
    g_slPosition = EvpUnitPosition;
    InterlockedExchange(&g_slOn, 0);
    puts("PASS steer lod: pins (with vehicle-box's of the pass's start), the switch, enemies within 100 m steered every frame and "
        "those beyond every third, staggered, after a quarter of the distances is read each frame; without a Helldiver every frame");
}
