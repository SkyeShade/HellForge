/* Build 25480438: the CONTROL CENTER terminal's camera (docs/multiplayer.md "The ship's information terminal"). The
 * terminal has no camera move of its own; when it opens HellForge's lobby browser, the camera glides from the player's
 * view to the terminal's wall screen, holds there while the browser shows, and glides back when it closes.
 *
 * The camera is a unit's: the main world's unit with a camera whose pose is the world's last render pose (world +
 * 0x230920, a row-major 4x4: right, forward, up, position). The game's camera code sets that unit's pose every frame
 * before the engine's script render step (exe+0x689BC0, the one that calls the boot script's render()), and the game's
 * render() renders the world (the binding at exe+0x3F7E90), which copies the camera unit's world pose to the world's
 * render pose then (exe+0x3181D8). A pose set before it is the one the frame renders with (seen live from Lua's
 * render(), 2026-10-30; set after the step it moves the unit but not the frame). So the step is hooked: before it, the
 * camera unit's local pose (its scene graph's node 0, at +0x20: rotation rows right, forward, up, then
 * position and scale) is read, blended towards the terminal's, and set with the engine's own set_local_pose
 * (exe+0x1FCAD0) and update_unit (exe+0x1B2DC0). Measured live: the wall screen faces -X, its centre at about
 * (3.9, -1.29, 22.15), 3.75 m wide; at the default vertical fov (0.96) and 16:9 the camera fills the view with it at
 * x = 1.85. Included before terminal.c, which starts the glide; the browser's editor says when it shows and goes (live_browse.c). */
#define TCAM_RENDER_STEP 0x689BC0u
#define TCAM_SET_LOCAL_POSE 0x1FCAD0u
#define TCAM_UPDATE_UNIT 0x1B2DC0u
#define TCAM_UNIT 0x9D8C0u
#define TCAM_MAIN_WORLD 0x317DE0u
#define TCAM_WORLD_UNITS 0x2305B8u   /* uint32 count, then (at +8) the Unit* array */
#define TCAM_WORLD_RENDER_POSE 0x230920u
#define TCAM_IN_MS 1100
#define TCAM_OUT_MS 800
#define TCAM_GIVE_UP_MS 4000         /* held with no browser to show: the editor never said it opened */
enum { TCAM_IDLE, TCAM_IN, TCAM_HELD, TCAM_OUT };
typedef void (*TcamStepFn)(void* app);
typedef void (*TcamSetPoseFn)(uint32_t unit, int32_t node, const float* pose);
typedef void (*TcamUpdateFn)(uintptr_t world, uintptr_t unit);
typedef uintptr_t (*TcamUnitFn)(uint32_t unit);
typedef uintptr_t (*TcamWorldFn)(void);
typedef int (*TcamCamerasFn)(uintptr_t unit);
typedef uintptr_t (*TcamGraphFn)(uintptr_t unit);
static TcamStepFn g_tcamStep;
static TcamSetPoseFn g_tcamSetPose;
static TcamUpdateFn g_tcamUpdate;
static TcamUnitFn g_tcamUnit;
static TcamWorldFn g_tcamWorld;
static volatile LONG g_tcamPhase;       /* TCAM_... */
static volatile LONG g_tcamShown;       /* the editor's browser shows (live_browse.c) */
static ULONGLONG g_tcamSince;           /* when the phase began */
static float g_tcamFrom;                /* the blend the phase started from (0 the game's view, 1 the terminal's) */
static float g_tcamBlend;               /* the blend set last frame */
static int g_tcamMisses;                /* frames the camera unit was looked for and not found */
static uint32_t g_tcamRef;              /* the camera unit's reference, once found */
static int g_tcamState;
static const uint8_t kTcamStep[] = {0x48,0x83,0xec,0x58,0x33,0xc0,0x4c,0x8d,0x05,0xeb,0x71,0x00,0x01,0x48,0x89,0x44,
    0x24,0x30,0x48,0x8d,0x54,0x24,0x40,0x89,0x44,0x24,0x38,0x48,0x81,0xc1,0x90,0x01};
static const uint8_t kTcamSetPose[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x49,
    0x8b,0xd8,0x48,0x63,0xfa,0xe8,0xd6,0x0d,0xea,0xff,0x48,0x8b,0xc8,0x48,0x8b,0xf0};
static const uint8_t kTcamUpdate[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x55,
    0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0x30,0xf8,0xff,0xff};
static const uint8_t kTcamUnit[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,
    0x8b,0x35,0x1a,0x28,0x97,0x01,0x8b,0xd9,0x48,0x8d,0x8e,0xd0,0x00,0x00,0x00,0xff};
static const uint8_t kTcamWorld[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x48,
    0x8b,0x05,0x12,0x84,0x6f,0x01,0x33,0xc9,0x0f,0x57,0xc0,0x8b,0xd1,0x8b,0xd9,0x44};

/* The terminal's camera: square to the wall screen (looking +X, up +Z), far enough back that it fills the view. */
static const float kTcamTarget[3] = {1.85f, -1.29f, 22.15f};
static const float kTcamForward[3] = {1, 0, 0}, kTcamUp[3] = {0, 0, 1};

static void TcamCross(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static int TcamNormalize(float v[3])
{
    float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length < 1e-6f) return 0;
    for (int i = 0; i < 3; i++) v[i] /= length;
    return 1;
}

/* The pose `blend` of the way from the game's (rotation rows right, forward, up; position; scale) to the terminal's:
 * position and the forward and up rows eased linearly, then made square again (right = forward x up). */
static void TcamBlend(const float* game, float blend, float* out)
{
    float forward[3], up[3], right[3];
    for (int i = 0; i < 3; i++)
    {
        forward[i] = game[3 + i] + (kTcamForward[i] - game[3 + i]) * blend;
        up[i] = game[6 + i] + (kTcamUp[i] - game[6 + i]) * blend;
        out[9 + i] = game[9 + i] + (kTcamTarget[i] - game[9 + i]) * blend;
    }
    if (!TcamNormalize(forward) || !TcamNormalize(up)) { memcpy(out, game, 16 * sizeof(float)); return; }
    TcamCross(forward, up, right);
    TcamNormalize(right);
    TcamCross(right, forward, up);
    memcpy(out, right, sizeof right);
    memcpy(out + 3, forward, sizeof forward);
    memcpy(out + 6, up, sizeof up);
    memcpy(out + 12, game + 12, 4 * sizeof(float)); /* scale, and the last float, as the game has them */
}

/* The camera unit: the main world's unit with a camera nearest the world's last render pose, within a metre (the
 * pose is a frame old, and the interaction moves the camera). 0 when none is; *nearest is the closest distance seen. */
static uint32_t TcamFindCamera(uintptr_t world, float* nearest)
{
    *nearest = -1;
    uint32_t count = 0;
    uintptr_t units = 0;
    float render[16];
    if (SafeRead(world + TCAM_WORLD_UNITS, &count, sizeof count) || SafeRead(world + TCAM_WORLD_UNITS + 8, &units, sizeof units) ||
        SafeRead(world + TCAM_WORLD_RENDER_POSE, render, sizeof render) || count > 100000 || units == 0) return 0;
    uint32_t best = 0;
    float bestSq = 1.0f;
    for (uint32_t i = 0; i < count; i++)
    {
        uintptr_t unit = 0, table = 0, cameras = 0, graph = 0, poses = 0;
        if (SafeRead(units + 8 * (uintptr_t)i, &unit, sizeof unit) || unit == 0 || SafeRead(unit, &table, sizeof table) ||
            SafeRead(table + 0x160, &cameras, sizeof cameras) || SafeRead(table + 0xE8, &graph, sizeof graph)) continue;
        if (((TcamCamerasFn)cameras)(unit) <= 0) continue;
        uintptr_t sg = ((TcamGraphFn)graph)(unit);
        float pose[12];
        if (sg == 0 || SafeRead(sg + 0x20, &poses, sizeof poses) || SafeRead(poses, pose, sizeof pose)) continue;
        float dx = pose[9] - render[12], dy = pose[10] - render[13], dz = pose[11] - render[14];
        float sq = dx * dx + dy * dy + dz * dz;
        uint32_t ref = 0;
        if (*nearest < 0 || sq < *nearest * *nearest) *nearest = sqrtf(sq);
        if (sq < bestSq && !SafeRead(unit + 8, &ref, sizeof ref) && ref != 0) { best = ref; bestSq = sq; }
    }
    return best;
}

/* The camera unit's scene graph pose (16 floats); 0 when it cannot be read. */
static uintptr_t TcamPose(uintptr_t unit)
{
    uintptr_t table = 0, graph = 0, poses = 0;
    if (unit == 0 || SafeRead(unit, &table, sizeof table) || SafeRead(table + 0xE8, &graph, sizeof graph)) return 0;
    uintptr_t sg = ((TcamGraphFn)graph)(unit);
    return sg != 0 && !SafeRead(sg + 0x20, &poses, sizeof poses) ? poses : 0;
}

static float TcamEase(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }

/* Where the blend should be this frame, moving the phase on when its time is up. */
static float TcamNow(void)
{
    ULONGLONG elapsed = GetTickCount64() - g_tcamSince;
    switch (g_tcamPhase)
    {
    case TCAM_IN:
        if (elapsed >= TCAM_IN_MS) { InterlockedExchange(&g_tcamPhase, TCAM_HELD); g_tcamSince = GetTickCount64(); return 1; }
        return g_tcamFrom + (1 - g_tcamFrom) * TcamEase((float)elapsed / TCAM_IN_MS);
    case TCAM_HELD:
        if (!g_tcamShown && elapsed >= TCAM_GIVE_UP_MS) { g_tcamFrom = 1; g_tcamSince = GetTickCount64(); InterlockedExchange(&g_tcamPhase, TCAM_OUT); }
        return 1;
    case TCAM_OUT:
        if (elapsed >= TCAM_OUT_MS) { InterlockedExchange(&g_tcamPhase, TCAM_IDLE); return 0; }
        return g_tcamFrom * (1 - TcamEase((float)elapsed / TCAM_OUT_MS));
    default:
        return 0;
    }
}

/* This frame's camera pose: the game's, blended toward the terminal's. */
static void TcamApply(void)
{
    if (g_tcamPhase == TCAM_IDLE) return;
    float blend = TcamNow();
    g_tcamBlend = blend;
    if (g_tcamPhase == TCAM_IDLE) return;
    uintptr_t world = g_tcamWorld();
    if (world == 0) return;
    if (g_tcamRef == 0)
    {
        float nearest;
        if ((g_tcamRef = TcamFindCamera(world, &nearest)) == 0)
        {
            if (++g_tcamMisses < 30) return; /* the next frames may find it */
            InterlockedExchange(&g_tcamPhase, TCAM_IDLE);
            Note("terminal camera: no camera unit at the render pose (the nearest %.2f m away)", nearest);
            return;
        }
    }
    uintptr_t unit = g_tcamUnit(g_tcamRef), poses = TcamPose(unit);
    float game[16], pose[16];
    if (poses == 0 || SafeRead(poses, game, sizeof game)) { g_tcamRef = 0; return; } /* found again next frame */
    TcamBlend(game, blend, pose);
    g_tcamSetPose(g_tcamRef, 0, pose);
    g_tcamUpdate(world, unit);
}

/* Before the step: the boot script's render() renders the world, which takes the camera unit's pose then. */
static void TcamStepDetour(void* app)
{
    TcamApply();
    g_tcamStep(app);
}

/* terminal.c: the terminal opened the browser. The camera glides in (from wherever a glide out left it). */
static void TcamOpen(void)
{
    if (g_tcamState != 1) return;
    g_tcamRef = 0; /* found again: the world may have changed since */
    g_tcamMisses = 0;
    g_tcamFrom = g_tcamPhase == TCAM_OUT ? g_tcamBlend : 0;
    g_tcamSince = GetTickCount64();
    InterlockedExchange(&g_tcamShown, 0);
    InterlockedExchange(&g_tcamPhase, TCAM_IN);
}

/* live_browse.c: the editor's browser shows (1) or goes (0). Going, the camera glides back to the player. */
static void TcamBrowser(int shown)
{
    InterlockedExchange(&g_tcamShown, shown);
    if (shown || (g_tcamPhase != TCAM_IN && g_tcamPhase != TCAM_HELD)) return;
    g_tcamFrom = g_tcamBlend;
    g_tcamSince = GetTickCount64();
    InterlockedExchange(&g_tcamPhase, TCAM_OUT);
}

/* HfBrowseState.camera: HF_BROWSE_CAMERA_... */
static uint32_t TcamState(void)
{
    if (g_tcamState != 1) return HF_BROWSE_CAMERA_NONE;
    LONG phase = g_tcamPhase;
    return phase == TCAM_IN ? HF_BROWSE_CAMERA_MOVING : phase == TCAM_HELD ? HF_BROWSE_CAMERA_HELD : phase == TCAM_OUT ? HF_BROWSE_CAMERA_RETURNING :
        HF_BROWSE_CAMERA_IDLE;
}

static void InstallTerminalCamera(void)
{
    if (g_tcamState) return;
    g_tcamState = -1;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    if (strcmp(g_build, "25480438") || exe == 0) return;
    if (!PinMatches(exe + TCAM_RENDER_STEP, kTcamStep, sizeof kTcamStep) || !PinMatches(exe + TCAM_SET_LOCAL_POSE, kTcamSetPose, sizeof kTcamSetPose) ||
        !PinMatches(exe + TCAM_UPDATE_UNIT, kTcamUpdate, sizeof kTcamUpdate) || !PinMatches(exe + TCAM_UNIT, kTcamUnit, sizeof kTcamUnit) ||
        !PinMatches(exe + TCAM_MAIN_WORLD, kTcamWorld, sizeof kTcamWorld))
    { Note("terminal camera: off; the engine does not match the reviewed build"); return; }
    g_tcamSetPose = (TcamSetPoseFn)(exe + TCAM_SET_LOCAL_POSE);
    g_tcamUpdate = (TcamUpdateFn)(exe + TCAM_UPDATE_UNIT);
    g_tcamUnit = (TcamUnitFn)(exe + TCAM_UNIT);
    g_tcamWorld = (TcamWorldFn)(exe + TCAM_MAIN_WORLD);
    if (LoaderHook("terminal camera", exe + TCAM_RENDER_STEP, (void*)TcamStepDetour, (void**)&g_tcamStep)) { Note("terminal camera: could not hook the render step"); return; }
    g_tcamState = 1;
    Note("terminal camera: the terminal's browser zooms the camera into its screen");
}
