/* The level generator renders two orthographic cameras over the whole 2048 m world: the water height map (ops
 * 0x0A/0x0B) and the fog volumes (ops 0x43-0x45). The engine's scatter update (exe 0x212F10, build 25480438) treats
 * them as views and grows every scatter layer's node vector to the whole map, a size it keeps all mission, and fills
 * every cell. Neither viewport draws SpeedTree, so HellForge leaves those two views out of the update. Included by
 * hellforge_modloader.c after perf_ai_path_budget.c: PinMatches (packages.c), LoaderHook (hooks.c), PerfFixOn. */

#define SC_UPDATE 0x212F10u   /* exe: the scatter update, once per view and scatter layer */
#define SC_CALLER 0x210DE5u   /* exe: its only call; the next instruction drops rax */
#define SC_CAMERA 0x4D0F57u   /* exe: the render camera's box (kind 1) and near/far/projection (kind 0) writers */
#define SC_DISPATCH 0xC14950u /* game.dll: the generator's op dispatcher, one record per call */
#define SC_DECODE 0xC1499Au   /* game.dll: its index, record, mask and op reads */
#define SC_RECORD 0x19B0u     /* a generator record: the op at +0, the run mask's flags at +0x1998 */
#define SC_WORLD 2048.0f      /* the vanilla world's side, which only the water and fog cameras span */

static const uint8_t kScUpdatePin[] = {0x48,0x89,0x5c,0x24,0x18, 0x55,0x56,0x57,0x41,0x55,0x41,0x56, 0x48,0x8d,0xac,0x24,0xe0,0xcb,0xff};
static const uint8_t kScCallerPin[] = {0xe8,0x26,0x21,0x00,0x00, 0x48,0x81,0xc3,0x38,0x02,0x00,0x00};
static const uint8_t kScCameraPin[] = {0x41,0x0f,0x10,0x40,0x08, 0x0f,0x11,0x41,0x2c, 0x41,0x0f,0x10,0x48,0x18, 0x0f,0x11,0x49,0x3c, 0xc3,
    0xf2,0x41,0x0f,0x10,0x40,0x08, 0xf2,0x0f,0x11,0x41,0x20, 0x41,0x8b,0x40,0x10, 0x89,0x41,0x28, 0xc3};
static const uint8_t kScDispatchPin[] = {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57, 0x48,0x8d,0xac,0x24,0xc0,0xea,0xff,0xff};
static const uint8_t kScDecodePin[] = {0x41,0x8b,0x00, 0x41,0x8b,0xd9, 0x48,0x8b,0xbd,0xa0,0x15,0x00,0x00, 0x4d,0x8b,0xf0, 0x89,0x45,0xc8,
    0x4c,0x8b,0xfa, 0x48,0x8b,0x05,0xd9,0x19,0x71,0x02, 0x4c,0x8b,0xe1, 0x89,0x5d,0xcc, 0x4c,0x89,0x45,0x80, 0x48,0x89,0x55,0xa8,
    0x48,0x89,0x4d,0xd0, 0xff,0x10, 0x41,0x8b,0x16, 0x48,0x69,0xf2,0xb0,0x19,0x00,0x00, 0x48,0x89,0x45,0xd8, 0x49,0x03,0xf4,
    0x48,0x89,0x74,0x24,0x60, 0x3b,0xda, 0x0f,0x86,0x3e,0x54,0x00,0x00, 0x8b,0x8d,0xa8,0x15,0x00,0x00, 0x85,0xc9, 0x74,0x0c,
    0x85,0x8e,0x98,0x19,0x00,0x00, 0x0f,0x85,0x28,0x54,0x00,0x00, 0x48,0x63,0x06, 0x3d,0xbb,0x00,0x00,0x00};

typedef uint64_t (*ScDispatchFn)(uint64_t records, uint64_t context, uint64_t index, uint64_t end, uint64_t phase, uint64_t mask);
typedef void (*ScUpdateFn)(void* system, const uint8_t* view, uint64_t stamp, void* layer, uint64_t flags);
static ScDispatchFn g_scDispatchNext;
static ScUpdateFn g_scUpdateNext;
static volatile LONG g_scOp = -1; /* the op of the record the generator runs now, on any level */
static volatile LONG64 g_scSkipped;
static int g_scState; /* 0 not asked yet, 2 waiting for the code, 1 installed, -1 not */
static ULONGLONG g_scLoadedAt;

/* Only these ops' windows hold the two cameras. Op 0x47's ambient-occlusion bake has the same camera and draws
 * SpeedTree; it runs before op 0x0A's second record, waits for its own readback, and is never in a window. */
static int ScCutOp(LONG op) { return op == 0x0A || op == 0x0B || (op >= 0x43 && op <= 0x45); }

/* A world-sized orthographic view with near 1 and far 1001, read from the render camera (+0x20 near, +0x24 far, +0x28
 * projection 0, +0x30..+0x3C left, right, bottom, top). The far-scatter bake's tiles have the same near and far but
 * are 256 m wide, and they can still render early in op 0x0A: the width keeps them. */
static int ScCutView(LONG op, const uint8_t* view)
{
    if (!ScCutOp(op)) return 0;
    struct { float nearZ, farZ; uint32_t projection; float fov, left, right, bottom, top; } v;
    if (view == NULL || SafeRead((uintptr_t)view + 0x20, &v, sizeof v)) return 0;
    return v.projection == 0 && v.nearZ == 1.0f && v.farZ == 1001.0f && v.right - v.left >= SC_WORLD && v.top - v.bottom >= SC_WORLD;
}

/* rcx the records, r8 the current index, r9d the end, the sixth argument a mask the record's flags must miss: the
 * dispatcher's own test (SC_DECODE) for whether the record runs. Only a record that runs moves the op. */
static uint64_t ScDispatch(uint64_t records, uint64_t context, uint64_t index, uint64_t end, uint64_t phase, uint64_t mask)
{
    uint32_t i = 0, op = 0, flags = 0;
    if (!SafeRead(index, &i, 4) && i < (uint32_t)end && !SafeRead(records + (uint64_t)i * SC_RECORD, &op, 4) &&
        ((uint32_t)mask == 0 || (!SafeRead(records + (uint64_t)i * SC_RECORD + 0x1998, &flags, 4) && !(flags & (uint32_t)mask))))
        InterlockedExchange(&g_scOp, (LONG)op);
    return g_scDispatchNext(records, context, index, end, phase, mask);
}

/* A view left out leaves the layer as the last update left it: the next view's update stamps, fills and drops its own
 * cells (0x9531E0, called only here), and a cell's scatter depends on its cell and the layer, not on the view. */
static void ScUpdate(void* system, const uint8_t* view, uint64_t stamp, void* layer, uint64_t flags)
{
    if (ScCutView(g_scOp, view)) { InterlockedIncrement64(&g_scSkipped); return; }
    g_scUpdateNext(system, view, stamp, layer, flags);
}

static int ScPinned(uintptr_t exe, uintptr_t game)
{
    return PinMatches(exe + SC_UPDATE, kScUpdatePin, sizeof kScUpdatePin) && PinMatches(exe + SC_CALLER, kScCallerPin, sizeof kScCallerPin) &&
        PinMatches(exe + SC_CAMERA, kScCameraPin, sizeof kScCameraPin) && PinMatches(game + SC_DISPATCH, kScDispatchPin, sizeof kScDispatchPin) &&
        PinMatches(game + SC_DECODE, kScDecodePin, sizeof kScDecodePin);
}

/* The dispatcher first: until it runs the op stays -1 and nothing is cut. If the update's hook then fails, the
 * dispatcher's only keeps a number that nothing reads. */
static int ScInstallAt(uintptr_t exe, uintptr_t game)
{
    if (LoaderHook("scatter-cameras: the generator's op", game + SC_DISPATCH, (void*)ScDispatch, (void**)&g_scDispatchNext)) return 0;
    return LoaderHook("scatter-cameras: the water and fog views", exe + SC_UPDATE, (void*)ScUpdate, (void**)&g_scUpdateNext) == 0;
}

static void ScWatchAt(uintptr_t exe, uintptr_t game)
{
    if (g_scState == 0)
    {
        if (strcmp(g_build, "25480438")) { g_scState = -1; Note("performance fix scatter-cameras: not installed; not build 25480438"); return; }
        if (!PerfFixOn("scatter-cameras")) { g_scState = -1; return; }
        g_scState = 2;
    }
    if (g_scState != 2 || game == 0) return;
    if (!ScPinned(exe, game))
    {
        /* the exe's code is decoded some time after start; a minute after the game loaded, it never will be the reviewed one */
        if (g_scLoadedAt == 0) g_scLoadedAt = GetTickCount64();
        if (GetTickCount64() - g_scLoadedAt < 60000) return;
        g_scState = -1;
        Note("performance fix scatter-cameras: not installed; the scatter update or the generator's dispatcher is not the reviewed code");
        return;
    }
    if (!ScInstallAt(exe, game))
    {
        g_scState = -1;
        Note("performance fix scatter-cameras: not installed; its hooks could not be added");
        return;
    }
    g_scState = 1;
    Note("performance fix scatter-cameras: installed; the generator's water and fog cameras are left out of the scatter update");
}

/* From the watch thread, once game.dll is loaded: the generator runs only after that. */
static void PerfScatterCamerasWatch(void)
{
    if ((g_scState == 0 || g_scState == 2) && g_fired[HF_EVENT_GAME_LOADED])
        ScWatchAt((uintptr_t)GetModuleHandleW(NULL), ApiModuleBase("game.dll", NULL));
}
