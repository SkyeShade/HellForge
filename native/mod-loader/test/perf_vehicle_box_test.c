/* The vehicle box fix (perf_vehicle_box.c) on synthetic images. The box function's own bytes run, with a fake unit
 * lookup, unit box and OBB conversion behind its calls; the clip's call site runs from a thunk and returns through
 * its own return address; both passes are hooked for real and run a test body. Included by stats_test.c. */

#define VBT_EXE 0x210000u
#define VBT_GAME 0x1350000u
#define VBT_THUNK 0x1000u      /* free room in the game image: the site's thunk, flag 1 */
#define VBT_THUNK_FLAG0 0x1040u /* the same entered after the flag is set, with flag 0 */
#define VBT_UNITS 160
#define VBT_HANDLE(k) ((uint32_t)(k) + VBT_UNITS * 3u)

typedef struct VbtUnit { void** vtable; uint32_t index; } VbtUnit;
typedef void* (*VbtSiteFn)(const uint32_t* instance, uint8_t* out);

static uint8_t* g_vbtExe;
static uint8_t* g_vbtGame;
static VbtUnit g_vbtUnits[VBT_UNITS];
static void* g_vbtVtable[0x240 / 8];
static uint64_t g_vbtTable[0x780 / 8], g_vbtRoot[4];
static uint32_t g_vbtInstances[VBT_UNITS][4];
static uint8_t g_vbtWant[VBT_UNITS][VB_BYTES];
static volatile LONG g_vbtComputed; /* the fake unit box's calls */
static float g_vbtFrame;            /* moves every box */
static int g_vbtVehicles, g_vbtExtras, g_vbtFailed;
static DWORD g_vbtMain;

static void* VbtLookup(uint32_t handle) { return &g_vbtUnits[handle % VBT_UNITS]; }

/* Unit vtable +0x238: a pose, then min at +0x40 and max at +0x4C. Unit 7's box is empty (min above max). */
static void VbtUnitBox(VbtUnit* u, float* box, uint32_t flag)
{
    InterlockedIncrement(&g_vbtComputed);
    for (int i = 0; i < 16; i++) box[i] = (float)(u->index * 100 + i) + g_vbtFrame + (float)flag * 1000.0f;
    for (int i = 0; i < 3; i++) { box[16 + i] = -1.0f - (float)i; box[19 + i] = 2.0f + (float)(u->index + i); }
    if (u->index == 7) box[16] = 50.0f;
}

/* 0x1FB3A0: the box's 0x58 bytes, its centre into the pose and its half extents at +0x4C. */
static void* VbtObb(float* out, const float* in)
{
    memcpy(out, in, 0x58);
    for (int i = 0; i < 3; i++) { out[12 + i] += (in[19 + i] + in[16 + i]) * 0.5f; out[19 + i] = (in[19 + i] - in[16 + i]) * 0.5f; }
    return out;
}

static void VbtFar(uint8_t* at, void* to)
{
    at[0] = 0x48; at[1] = 0xb8; /* mov rax, imm64; jmp rax */
    uint64_t v = (uint64_t)(uintptr_t)to;
    memcpy(at + 2, &v, 8);
    at[10] = 0xff; at[11] = 0xe0;
}

static void* VbtSite(int k) { uint8_t out[VB_BYTES]; return ((VbtSiteFn)(void*)(g_vbtGame + VBT_THUNK))(g_vbtInstances[k], out); }

static void VbtWant(int vehicles)
{
    for (int k = 0; k < vehicles; k++) assert(((VbBoxFn)(void*)(g_vbtExe + VB_BOX))(g_vbtWant[k], VBT_HANDLE(k), 1) == g_vbtWant[k]);
}

static DWORD WINAPI VbtOtherSite(void* parameter)
{
    uint8_t out[VB_BYTES];
    if (((VbtSiteFn)(void*)(g_vbtGame + VBT_THUNK))(g_vbtInstances[0], out) != out || memcmp(out, g_vbtWant[0], VB_BYTES)) g_vbtFailed = 1;
    return 0;
}

static DWORD WINAPI VbtOtherPass(void* parameter)
{
    ((VbPassFn)(void*)(g_vbtGame + VB_MOTION))(NULL, 0, 2, 0.25f);
    return 0;
}

static void VbtJoin(LPTHREAD_START_ROUTINE fn)
{
    HANDLE t = CreateThread(NULL, 0, fn, NULL, 0, NULL);
    assert(t != NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

/* Both passes' body: every mover clips against every vehicle through the site, as the game's clip does. On the main
 * thread, extras: a flag 0 call, a call from elsewhere, the site on another thread and another thread's pass. */
static void VbtPassBody(void* unused, uint32_t start, uint32_t end, float dt)
{
    if (dt != 0.25f) g_vbtFailed = 1;
    for (uint32_t i = start; i < end; i++)
        for (int k = 0; k < g_vbtVehicles; k++)
        {
            uint8_t out[VB_BYTES];
            memset(out, 0xcd, sizeof out);
            if (((VbtSiteFn)(void*)(g_vbtGame + VBT_THUNK))(g_vbtInstances[k], out) != out || memcmp(out, g_vbtWant[k], VB_BYTES)) g_vbtFailed = 1;
        }
    if (!g_vbtExtras || GetCurrentThreadId() != g_vbtMain) return;
    uint8_t flag0[VB_BYTES], want0[VB_BYTES], direct[VB_BYTES];
    LONG before = g_vbtComputed;
    ((VbtSiteFn)(void*)(g_vbtGame + VBT_THUNK_FLAG0))(g_vbtInstances[0], flag0);
    ((VbtSiteFn)(void*)(g_vbtGame + VBT_THUNK_FLAG0))(g_vbtInstances[0], flag0);
    ((VbBoxFn)(void*)(g_vbtExe + VB_BOX))(want0, VBT_HANDLE(0), 0);
    ((VbBoxFn)(void*)(g_vbtExe + VB_BOX))(direct, VBT_HANDLE(0), 1);
    if (g_vbtComputed != before + 4 || memcmp(flag0, want0, VB_BYTES) || !memcmp(flag0, g_vbtWant[0], VB_BYTES) ||
        memcmp(direct, g_vbtWant[0], VB_BYTES)) g_vbtFailed = 1; /* flag 0 and a call from elsewhere: always computed */
    VbtJoin(VbtOtherSite);
    VbtJoin(VbtOtherPass);
    if (g_vbtComputed != before + 5 + 2 * g_vbtVehicles) g_vbtFailed = 1; /* another thread never sees the copies */
}

static void VbtFlip(uint32_t rva, uint8_t* image)
{
    image[rva] ^= 1;
    assert(!VbPinned((uintptr_t)g_vbtExe, (uintptr_t)g_vbtGame));
    image[rva] ^= 1;
}

static void VehicleBoxFixture(void)
{
    g_vbtExe = (uint8_t*)VirtualAlloc(NULL, VBT_EXE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    g_vbtGame = (uint8_t*)VirtualAlloc(NULL, VBT_GAME, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(g_vbtExe != NULL && g_vbtGame != NULL);
    uintptr_t exe = (uintptr_t)g_vbtExe, game = (uintptr_t)g_vbtGame;
    memcpy(g_vbtExe + VB_BOX, kVbBoxPin, sizeof kVbBoxPin);
    memcpy(g_vbtGame + VB_SITE, kVbSitePin, sizeof kVbSitePin);
    memcpy(g_vbtGame + VB_MOTION, kVbMotionPin, sizeof kVbMotionPin);
    memcpy(g_vbtGame + VB_STEER, kVbSteerPin, sizeof kVbSteerPin);
    assert(VbPinned(exe, game) && !VbPinned(exe, 0));
    VbtFlip(VB_BOX + 0x8B, g_vbtExe); /* the write of +0x48 */
    VbtFlip(VB_SITE + 9, g_vbtGame);  /* the flag */
    VbtFlip(VB_MOTION + 0x40, g_vbtGame);
    VbtFlip(VB_STEER + 0x53, g_vbtGame);

    /* the box function's callees: the unit lookup (0x9D8C0) and the OBB conversion (0x1FB3A0) */
    VbtFar(g_vbtExe + 0x9D8C0, (void*)VbtLookup);
    VbtFar(g_vbtExe + 0x1FB3A0, (void*)VbtObb);
    g_vbtVtable[0x238 / 8] = (void*)VbtUnitBox;
    for (int k = 0; k < VBT_UNITS; k++)
    {
        g_vbtUnits[k].vtable = g_vbtVtable;
        g_vbtUnits[k].index = (uint32_t)k;
        g_vbtInstances[k][3] = VBT_HANDLE(k); /* the vehicle instance: its unit at +0xC */
    }
    g_vbtTable[0x778 / 8] = exe + VB_BOX; /* the unit API's +0x778 */
    g_vbtRoot[3] = (uint64_t)(uintptr_t)g_vbtTable;
    /* the site's thunk: the clip's frame (rsi the instance, rbp + 0x120 the answer) and rax the engine root, as its
     * first instruction loads it; then the site's own bytes, back here through its return address */
    static const uint8_t enter[] = {0x55, 0x56, 0x48,0x83,0xec,0x28, 0x48,0x89,0xce, 0x48,0x8d,0xaa,0xe0,0xfe,0xff,0xff, 0x48,0xb8};
    static const uint8_t leave[] = {0x48,0x83,0xc4,0x28, 0x5e, 0x5d, 0xc3};
    static const uint8_t flag0[] = {0x45,0x31,0xc0}; /* xor r8d, r8d */
    uint64_t root = (uint64_t)(uintptr_t)g_vbtRoot;
    uint8_t* c = g_vbtGame + VBT_THUNK;
    memcpy(c, enter, sizeof enter); memcpy(c + sizeof enter, &root, 8); c += sizeof enter + 8;
    SgJump(c, 0xe9, game + VB_SITE + 7);
    c = g_vbtGame + VBT_THUNK_FLAG0;
    memcpy(c, enter, sizeof enter); memcpy(c + sizeof enter, &root, 8); c += sizeof enter + 8;
    memcpy(c, flag0, sizeof flag0); c += sizeof flag0;
    SgJump(c, 0xe9, game + VB_SITE + 13);
    FlushInstructionCache(GetCurrentProcess(), g_vbtExe, VBT_EXE);
    g_vbtMain = GetCurrentThreadId();

    /* the game's own answers: the empty box (unit 7) has the pose it was given and no extent */
    VbtWant(8);
    float half[3];
    memcpy(half, g_vbtWant[7] + 0x40, 12);
    assert(half[0] == 0 && half[1] == 0 && half[2] == 0 && ((float*)g_vbtWant[7])[12] == 1712.0f);
    memcpy(half, g_vbtWant[3] + 0x40, 12);
    assert(half[0] == 3.0f && half[2] == 5.0f);
    /* the site unhooked: back through its return address, which holds the thunk's end until the pins are read again */
    uint8_t out[VB_BYTES], site[sizeof leave];
    memcpy(site, g_vbtGame + VB_SITE_RETURN, sizeof site);
    memcpy(g_vbtGame + VB_SITE_RETURN, leave, sizeof leave);
    FlushInstructionCache(GetCurrentProcess(), g_vbtGame, VBT_GAME);
    LONG computed = g_vbtComputed;
    assert(((VbtSiteFn)(void*)(g_vbtGame + VBT_THUNK))(g_vbtInstances[3], out) == out && !memcmp(out, g_vbtWant[3], VB_BYTES));
    assert(g_vbtComputed == computed + 1);
    memcpy(g_vbtGame + VB_SITE_RETURN, site, sizeof site);

    /* refused: switched off, then code not yet decoded and never decoded; nothing is hooked */
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "safe-read, vehicle-box");
    assert(VehicleBoxTry(exe, game, 0) == -1);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    g_vbtGame[VB_STEER + 0x20] ^= 1;
    assert(VehicleBoxTry(exe, game, 0) == 0 && VehicleBoxTry(exe, game, 1) == -1);
    g_vbtGame[VB_STEER + 0x20] ^= 1;
    assert(g_vbtExe[VB_BOX] == 0x48 && g_vbtGame[VB_MOTION] == 0x41 && g_vbtGame[VB_STEER] == 0x4c && g_vbMotionNext == NULL);

    assert(VehicleBoxTry(exe, game, 0) == 1);
    assert(VbPinned(exe, game)); /* the pins read through the hooks */
    /* the passes' bodies after the instructions the hooks moved: cmp and jae (the ret stays), mov r11 and the rbx save */
    VbtFar(g_vbtGame + VB_MOTION + 5, (void*)VbtPassBody);
    VbtFar(g_vbtGame + VB_STEER + 7, (void*)VbtPassBody);
    memcpy(g_vbtGame + VB_SITE_RETURN, leave, sizeof leave);
    FlushInstructionCache(GetCurrentProcess(), g_vbtGame, VBT_GAME);
    VbPassFn motion = (VbPassFn)(void*)(g_vbtGame + VB_MOTION), steer = (VbPassFn)(void*)(g_vbtGame + VB_STEER);

    /* outside a pass every call computes */
    computed = g_vbtComputed;
    VbtSite(2); VbtSite(2);
    assert(g_vbtComputed == computed + 2);

    /* a motion pass: 5 movers x 40 vehicles, each box computed once; with the extras on the main thread */
    g_vbtVehicles = 40;
    VbtWant(40);
    computed = g_vbtComputed;
    LONG64 copied = g_vbCopied;
    g_vbtExtras = 1;
    motion(NULL, 0, 5, 0.25f);
    g_vbtExtras = 0;
    assert(!g_vbtFailed && g_vbtComputed == computed + 40 + 5 + 80 && g_vbCopied == copied + 160);
    assert(g_vb.owner == 0 && g_vb.depth == 0 && g_vb.count == 0);
    computed = g_vbtComputed;
    motion(NULL, 3, 3, 0.25f); /* an empty range returns at the moved jae */
    assert(g_vbtComputed == computed && g_vb.owner == 0);

    /* the poses move (a new frame, or a new level with the same handles): the next pass computes every box again */
    g_vbtFrame = 3.5f;
    VbtWant(40);
    computed = g_vbtComputed;
    steer(NULL, 0, 4, 0.25f);
    assert(!g_vbtFailed && g_vbtComputed == computed + 40 && g_vb.owner == 0);

    /* more vehicles than the table holds: the ones past it are computed every time, still right */
    g_vbtVehicles = 150;
    VbtWant(150);
    computed = g_vbtComputed;
    motion(NULL, 0, 2, 0.25f);
    assert(!g_vbtFailed && g_vbtComputed == computed + 150 + (150 - VB_MAX));

    /* unhooked: the passes and the site are the game's own again */
    assert(HookRemove(NULL, exe + VB_BOX, (void*)VbBox) == 0 && HookRemove(NULL, game + VB_MOTION, (void*)VbMotion) == 0 &&
        HookRemove(NULL, game + VB_STEER, (void*)VbSteer) == 0);
    g_vbtVehicles = 10;
    computed = g_vbtComputed;
    motion(NULL, 0, 3, 0.25f);
    assert(!g_vbtFailed && g_vbtComputed == computed + 30);
    puts("PASS vehicle boxes: pins, the switch, each box computed once per motion and steering pass and copied after, "
        "answers equal to the game's (an empty box too), nothing kept between passes, flag 0, other callers and other "
        "threads always computed, more vehicles than the table, unhooked");
}
