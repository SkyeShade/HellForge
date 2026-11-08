/* Extra large maps: the detailed terrain follows the players. The drop tile (difficulty_world_terrain.c) is built while
 * the level generates; this file builds more of them while the mission runs, on a grid of cells 512 coarse samples
 * (2048.4 m) wide anchored at the drop tile's square (the ring of every cell is dipped at load, NearRingGrid), and
 * releases them when no player comes near.
 *
 * Where: after the level update (game 0xF73F10, rcx = level, called once a frame from 0xAB519E), in state 3
 * ([level+0x1E4]), on the game thread: the place the level's own main-thread generation records run from in state 1
 * (0xF73F10 -> 0xF26140 -> the dispatcher 0xC14950). The level's records and its context stay valid all mission
 * ([level+0x2A8], freed only by the level's destructor); the job queue [ctx+0x50] = game+0xAC570 is drained every frame
 * by the render callback (0xAB7680 -> 0xAB0590), and the merge's request ring level+0xB20 by the level update (0xFCE9A0).
 *
 * One tile at a time:
 *  1. the merge package 0x5157EF2020B48DCD: [api+0x10]+0x310 (resident) and, when not, +0x2F0 (acquire), polled until
 *     resident (as op 0x43 does, 0xF61E8B..0xF61EA9; released by the level's 0x67 at 0xC17CF7);
 *  2. a copy of the context (0xE8 bytes) whose settings (+8) point at a copy of the level's settings (0xD0 bytes, level
 *     +0x2D20 up to the context's next field level+0x2DF0): the replay's composite tiles rewrite the world size and
 *     scatter radii there (TerrainBuild), which gameplay reads through the level's own context;
 *  3a. the stamp terrains of the locations within reach of the tile: op 0x2F (0xF44380) on a staging copy of their
 *     records (pass+8 the records, [pass+0x28] the count, +0x48 the resume index; it waits for its own spawns through
 *     [api+8]+0x48 before it moves on), so 0x63 draws them; 0x65 (0xF53D20) takes them away after the replay on the
 *     same staging (mode 0: the records' +0xA8 units; mode 1: the unit list pass+0x10, [pass+0x38], packed: its loop
 *     does not test for NULL), units the content streamer removed meanwhile left out;
 *  3. op 0x37 (0xFCEB80(ring, R): the merge's textures, generators and gui re-created, the flags R+0xE8.. having been
 *     reset by the level's 0x66/0x67), then 0x38 (0xF48DA0: cameras, shading environment slot 5; phase 1 waits for the
 *     ring to drain), each through the dispatcher on a copy of the level's record, the generation arena (ctx+0)
 *     reset after each (0x173BAB0, as 0xF26140 does after a finished op);
 *  4. the drop tile's replay (TerrainNearReplay from NEAR_START, its plan made again for the cell) with craters held
 *     (the deformation renders into whatever is published under the two heightmap names, which hold the tile's
 *     textures while it is replayed, and pushes its readback into the coarse collision): the deformation's update
 *     (0x1384BE0) and render (0x1385C30) are skipped while held; their records wait in their queue;
 *  5. the merge torn down as the level's 0x66 / 0x67 do it (0xC17B34.., 0xC17CC2..): the cameras ([api+8]+0x60 on R+8,
 *     R+0x18), R+0x108 ([api+8]+0x478), 0x94's lookup, 0xFCF2F0(R, world), 0x1009890(ctx+0x98, 5), the four textures
 *     and names of 0x67, the package ([api+0x10]+0x300) when it was acquired here;
 *  6. the spawn (InitSpawnTerrain on the level's kept 0x69 record), the finalize, the coarse collision lowered and the
 *     coarse drawing hidden over the union of the live tiles (NearHideUnion).
 * A tile goes when no player has been within MOVE_KEEP of its square for MOVE_KEEP_MS: removed from the ground
 * queries, the coarse collision raised back from the level's CPU copy (lg+0xD8), the hidden nodes shown again, the
 * terrain destroyed ([api+8]+0x1E0 -> exe 0x1ADEA0) and its textures released a few frames later. The drop tile stays.
 *
 * Players: every Helldiver (as the content streamer reads them), the streamer's research observer, and the test eye
 * (HELLFORGE_XL_EYE) while it is on and within 30 m of the ground. HELLFORGE_XL_MOVE=0 turns this off. */

#define MOVE_LEVEL_UPDATE 0xF73F10u
#define MOVE_CRATER_UPDATE 0x1384BE0u
#define MOVE_CRATER_RENDER 0x1385C30u
static const char kMoveUpdatePin[] = "40 55 53 56 57 41 54 48 8D AC 24 40 FC FF FF 48 81 EC C0 04 00 00";
static const char kMoveCraterUpdatePin[] = "4C 8B DC 55 41 55 49 8D AB 78 F6 FF FF 48 81 EC 78 0A 00 00";
static const char kMoveCraterRenderPin[] = "40 55 56 41 56 41 57 48 8D AC 24 38 FF FF FF 48 81 EC C8 01 00 00";
#define MOVE_PACKAGE 0x5157EF2020B48DCDull
#define MOVE_WANT 600.0f      /* a cell is wanted when a player is this close to its square */
#define MOVE_KEEP 1300.0f     /* a tile is kept while a player is this close to its square ... */
#define MOVE_KEEP_MS 30000u   /* ... or was, this long ago */
#define MOVE_CAP 6            /* tiles at most (the drop tile counted); ~128 MiB GPU and ~150 MiB CPU each */
#define MOVE_HOLD_MS 60000u   /* craters are never held longer than this */
#define MOVE_SKIP_MS 120000u  /* a cell whose build failed waits this long */

enum { MV_IDLE, MV_PACKAGE, MV_STAMPS, MV_KICK, MV_SETUP, MV_LOOKUP, MV_REPLAY, MV_TEARDOWN, MV_UNSTAMP, MV_SPAWN, MV_READY, MV_FAILING };
#define MOVE_STAMPS_MAX 32768u /* the plan's stamps in reach too (difficulty_stream_window.c): a tile at vanilla density */
#define MOVE_STAMP_MARGIN 512.0f /* locations this far past the tile's texels: a stamp's terrain reaches ~256 m from it */
#define MOVE_STAMP_REACH 256.0f /* a stamp's terrain reaches about this far from it: these go in first */

typedef void (*MoveUpdateFn)(uint64_t level);
typedef void (*MoveCraterUpdateFn)(void* system, float dt, uint64_t r8, uint64_t r9);
typedef void (*MoveCraterRenderFn)(void* system, uint64_t rdx, uint64_t r8, uint64_t r9);
static MoveUpdateFn g_moveUpdate;
static MoveCraterUpdateFn g_moveCraterUpdate;
static MoveCraterRenderFn g_moveCraterRender;
static volatile LONG g_moveInstalled;
static volatile LONG64 g_moveHoldSince; /* craters wait while non-zero and younger than MOVE_HOLD_MS */
static volatile LONG64 g_moveHeldUpdates, g_moveHeldRenders;

static struct
{
    LONG level; int on, state, slot, cell[2], acquired, merge;
    uintptr_t levelPtr;
    float origin[2], centre[2];
    uint32_t at2F, at37, at38, at4C, at66, frames, waits, calls;
    int pkgPending; uint32_t pkgFrames;
    uint32_t ringWaits; /* frames the stamp units' destroy waited for the async spawn ring */
    int failState; /* the state a build failed in (MoveFailRest destroys the new tile's terrain from MV_SPAWN on) */ /* the merge package's release, MOVE_PKG_FRAMES after the teardown that gave it up */
    /* the stamp terrains of the tile's locations (0x2F spawns them for 0x63 to draw, 0x65 takes them away): staging
     * copies of their records, the real indices, their units, the counts the two ops read through pointers */
    uint8_t* stLoc; uint32_t* stIndex; uint64_t* stUnit; uint32_t stCount, stUnits, stTemp; int stamped;
    ULONGLONG since, started;
    uint8_t ctx[0xE8];
    uint8_t settings[0xD0];
    uint8_t one[0x19B0];
    uint8_t phase;
    /* the drop tile's maps and handle while a tile builds (g_near / g_matband hold the new tile's) */
    int stashed; float dropCentre[2]; uint64_t dropHandle, dropTex[2], dropBand; uint32_t* dropBandPixels; int dropBandServed;
    int built, failed, released;
    struct { int a, b; ULONGLONG until; } skip[16];
    struct { int used; uint64_t tex[2], band; uint32_t* pixels; uint32_t frames; } freeing[NEAR_TILES_MAX * 2];
    float player[EVT_MAX_PLAYERS + 2][2];
    int players;
} g_move = {.level = -1};

static void MoveCraterUpdate(void* system, float dt, uint64_t r8, uint64_t r9)
{
    LONG64 since = g_moveHoldSince;
    if (since != 0 && GetTickCount64() - (ULONGLONG)since < MOVE_HOLD_MS) { InterlockedIncrement64(&g_moveHeldUpdates); return; }
    g_moveCraterUpdate(system, dt, r8, r9);
}

static void MoveCraterRender(void* system, uint64_t rdx, uint64_t r8, uint64_t r9)
{
    LONG64 since = g_moveHoldSince;
    if (since != 0 && GetTickCount64() - (ULONGLONG)since < MOVE_HOLD_MS) { InterlockedIncrement64(&g_moveHeldRenders); return; }
    g_moveCraterRender(system, rdx, r8, r9);
}

static void MoveHold(int on)
{
    InterlockedExchange64(&g_moveHoldSince, on ? (LONG64)GetTickCount64() : 0);
}

/* The textures of a tile that went, released once the frames that may still draw it are past. */
static void MoveFreeLater(uint64_t tex0, uint64_t tex1, uint64_t band, uint32_t* pixels)
{
    for (int k = 0; k < NEAR_TILES_MAX * 2; k++)
        if (!g_move.freeing[k].used)
        {
            g_move.freeing[k].used = 1; g_move.freeing[k].tex[0] = tex0; g_move.freeing[k].tex[1] = tex1; g_move.freeing[k].band = band;
            g_move.freeing[k].pixels = pixels; g_move.freeing[k].frames = 0;
            return;
        }
    Note("difficulty: moving tiles: no room to release a tile's textures; they stay allocated");
}

static void MoveFreeNow(uint64_t tex0, uint64_t tex1, uint64_t band, uint32_t* pixels)
{
    NearReleaseFn release = (NearReleaseFn)NearApi(0x10, 0x198);
    typedef void (*DestroyFn)(uint64_t tex);
    DestroyFn destroy = (DestroyFn)NearApi(0x10, 0x6B0);
    if (release != NULL) { if (tex0 != 0) release(tex0); if (tex1 != 0) release(tex1); }
    if (band != 0 && destroy != NULL) destroy(band);
    if (pixels != NULL && (band == 0 || destroy != NULL)) VirtualFree(pixels, 0, MEM_RELEASE);
}

/* The merge package goes some frames after the teardown, never in the same call (run s84b: exe+0x504F42 on the render
 * thread: a kind-7 render object's destroy, still in the world's command list, freed the constant blocks of a shader
 * program that the package's material bring-out had already freed (0x51B4B0 via 0x282EC0, queued by 0x168470);
 * the level's own 0x66 destroys and 0x67 releases are separate ops; static rendercrash504). A build that starts while
 * the release is pending takes the package back instead of asking for it again. */
#define MOVE_PKG_FRAMES 8u
static void MovePackageTick(int now)
{
    if (!g_move.pkgPending || (!now && ++g_move.pkgFrames < MOVE_PKG_FRAMES)) return;
    typedef void (*PkgFn)(uint64_t hash);
    PkgFn pkgRelease = (PkgFn)NearApi(0x10, 0x300);
    if (pkgRelease != NULL) pkgRelease(MOVE_PACKAGE);
    g_move.pkgPending = 0;
}
static void MovePackageLater(void)
{
    if (!g_move.acquired) return;
    g_move.acquired = 0;
    if (g_move.pkgPending) { MovePackageTick(1); } /* one pending at most: the older goes now (its frames are mostly past) */
    g_move.pkgPending = 1; g_move.pkgFrames = 0;
}

static void MoveFreeTick(int all)
{
    MovePackageTick(all);
    for (int k = 0; k < NEAR_TILES_MAX * 2; k++)
        if (g_move.freeing[k].used && (all || ++g_move.freeing[k].frames >= 4))
        {
            MoveFreeNow(g_move.freeing[k].tex[0], g_move.freeing[k].tex[1], g_move.freeing[k].band, g_move.freeing[k].pixels);
            g_move.freeing[k].used = 0;
        }
}

/* The drop tile's maps back in g_near / g_matband, the build's flags off. */
static void MoveRestoreDrop(void)
{
    if (g_move.stashed)
    {
        g_near.tex[0] = g_move.dropTex[0]; g_near.tex[1] = g_move.dropTex[1]; g_near.handle = g_move.dropHandle;
        g_near.centre[0] = g_move.dropCentre[0]; g_near.centre[1] = g_move.dropCentre[1];
        g_matband.tex = g_move.dropBand; g_matband.pixels = g_move.dropBandPixels; g_matband.served = g_move.dropBandServed;
        g_move.stashed = 0;
    }
    g_near.state = NEAR_IN;
    InterlockedExchange(&g_moveBuilding, 0);
    MoveHold(0);
}

/* A new level starts (TerrainNearReplay's first call): the last level's world is gone with every tile in it. */
static void MoveLevelGone(void)
{
    if (g_move.state != MV_IDLE && g_move.stashed)
        MoveFreeNow(g_move.dropTex[0] == g_near.tex[0] ? 0 : g_move.dropTex[0], g_move.dropTex[1] == g_near.tex[1] ? 0 : g_move.dropTex[1],
            g_move.dropBand == g_matband.tex ? 0 : g_move.dropBand, g_move.dropBand == g_matband.tex ? NULL : g_move.dropBandPixels);
    g_move.stashed = 0;
    MoveFreeTick(1);
    for (int k = 0; k < NEAR_TILES_MAX; k++)
        if (g_tiles[k].tex[0] != 0 || g_tiles[k].tex[1] != 0 || g_tiles[k].band != 0)
        {
            MoveFreeNow(g_tiles[k].tex[0], g_tiles[k].tex[1], g_tiles[k].band, g_tiles[k].bandPixels);
            g_tiles[k].tex[0] = g_tiles[k].tex[1] = g_tiles[k].band = 0; g_tiles[k].bandPixels = NULL;
        }
    g_move.state = MV_IDLE; g_move.level = -1; g_move.on = 0;
    g_move.stTemp = 0; g_move.stamped = 0; /* their levels went with the world */
    InterlockedExchange(&g_moveBuilding, 0);
    MoveHold(0);
}

static int MoveOn(void)
{
    static int on = -1;
    if (on < 0) { char t[4] = ""; on = !(GetEnvironmentVariableA("HELLFORGE_XL_MOVE", t, sizeof t) > 0 && t[0] == '0'); }
    return on;
}

static void MovePlayers(void)
{
    g_move.players = 0;
    EvpPlayer players[EVT_MAX_PLAYERS];
    int count = EvpPlayers(g_world.game, players);
    for (int i = 0; count > 0 && i < count && g_move.players < EVT_MAX_PLAYERS; i++)
    {
        if (players[i].avatar == 0) continue;
        ActHealth h;
        float at[3] = {0.0f, 0.0f, 0.0f};
        if (!ActHealthOf(g_world.game, players[i].avatar, &h) || h.unit == 0 || !EvpUnitPosition(h.unit, at) || !isfinite(at[0]) || !isfinite(at[1])) continue;
        g_move.player[g_move.players][0] = at[0]; g_move.player[g_move.players][1] = at[1]; g_move.players++;
    }
    if (g_streamObserverOn) { g_move.player[g_move.players][0] = g_streamObserverAt[0]; g_move.player[g_move.players][1] = g_streamObserverAt[1]; g_move.players++; }
    if (g_eye.on) /* the test eye walks like a player: not when it looks from high above (elevated views) */
    {
        float ground = NearEyeGround(g_eye.pos[0], g_eye.pos[1]);
        if (ground < -9000.0f || g_eye.pos[2] - ground < 30.0f)
        { g_move.player[g_move.players][0] = g_eye.pos[0]; g_move.player[g_move.players][1] = g_eye.pos[1]; g_move.players++; }
    }
}

static void MoveCellCentre(int a, int b, float* c)
{
    c[0] = g_move.origin[0] + (float)a * NEAR_HIDE; c[1] = g_move.origin[1] + (float)b * NEAR_HIDE;
}

/* Metres from (x, y) to the square of cell (a, b) (0 inside). */
static float MoveCellDistance(int a, int b, float x, float y)
{
    float c[2];
    MoveCellCentre(a, b, c);
    float dx = fmaxf(0.0f, fabsf(x - c[0]) - NEAR_HIDE / 2), dy = fmaxf(0.0f, fabsf(y - c[1]) - NEAR_HIDE / 2);
    return sqrtf(dx * dx + dy * dy);
}

static int MoveCellValid(int a, int b) { return a >= g_ring.a0 && a <= g_ring.a1 && b >= g_ring.b0 && b <= g_ring.b1; }

static int MoveCellLive(int a, int b)
{
    for (int k = 0; k < NEAR_TILES_MAX; k++)
        if (g_tiles[k].live && g_tiles[k].level == g_world.levels && g_tiles[k].cell[0] == a && g_tiles[k].cell[1] == b) return 1;
    return 0;
}

static int MoveCellSkipped(int a, int b, ULONGLONG now)
{
    for (int k = 0; k < 16; k++) if (g_move.skip[k].until > now && g_move.skip[k].a == a && g_move.skip[k].b == b) return 1;
    return 0;
}

static void MoveCellSkip(int a, int b, ULONGLONG now)
{
    int at = 0;
    for (int k = 1; k < 16; k++) if (g_move.skip[k].until < g_move.skip[at].until) at = k;
    g_move.skip[at].a = a; g_move.skip[at].b = b; g_move.skip[at].until = now + MOVE_SKIP_MS;
}

/* The tile's stamp terrains are drawn 20 km north of their places, where no terrain stands: mid-mission the merge's 0x63
 * render (a data-driven request, 0xABA7FC8A, set up by 0x37 at 0xFCF1AE) draws the live terrains into the heights as
 * well (run s72k: its delta over the tile was the level's surface, corr 0.978, slope 0.99). Each staged location gets a
 * stamp unit of its own (SpawnUnit, the world interface's +0x380, as the streamer respawns one) from the resource and
 * pose the streamer saved at capture ([[loc+0x90]+8], unit+0x20), its translation (row 3) + MOVE_STAMP_OFFSET in y; 0x2F
 * spawns its stamp terrains there, 0x63 sees them through the cameras moved by the same offset (an orthographic view: the
 * heights are the same), 0x65 takes them away, and the units are destroyed (+0x388). The real stamp units, their
 * buildings and the streamer's state are not touched. */
#define MOVE_STAMP_OFFSET 20000.0f
static uint64_t MoveWorld(void);

/* The world's async spawn ring is empty: with r9 = [world+0x20], *[r9+8] == *[r9+0x10] (the test 0x338960 makes at
 * 0x338974..0x33898F before it drains). A level destroyed while its units still wait there skips them (0xD9970 at
 * 0xD9B8A / 0xD9D6A: the handle resolves to no unit yet) and the drain makes them later as orphans with bodies no level
 * removes (KB xl-world-streaming, subagent deadbody). 1 empty, 0 not, -1 unreadable. */
static int MoveSpawnRingEmpty(uint64_t world)
{
    uint64_t ring = 0, at8 = 0, at10 = 0, a = 0, b = 0;
    if (world == 0 || SafeRead(world + 0x20, &ring, 8) || ring == 0 || SafeRead(ring + 8, &at8, 8) || SafeRead(ring + 0x10, &at10, 8) ||
        at8 == 0 || at10 == 0 || SafeRead(at8, &a, 8) || SafeRead(at10, &b, 8)) return -1;
    return a == b;
}

/* MV_UNSTAMP / MV_FAILING: the stamp units go once the spawn ring is empty (at most 600 frames' wait). */
static int MoveStampUnitsReady(void)
{
    if (g_move.stTemp == 0 || MoveSpawnRingEmpty(MoveWorld()) != 0) return 1;
    if (++g_move.ringWaits <= 600) return 0;
    Note("difficulty: moving tiles: the async spawn ring did not empty in 600 frames; the tile's stamp units go now");
    return 1;
}

static void MoveStampUnitsDestroy(void)
{
    uint64_t world = MoveWorld();
    uintptr_t wi = 0, table = 0, destroy = 0;
    if (world == 0 || SafeRead(g_world.game + DIF_STREAM_INTERFACE, &wi, 8) || wi == 0 || SafeRead(wi + DIF_STREAM_TABLE, &table, 8) || table == 0 ||
        SafeRead(table + DIF_STREAM_DESTROY_ENTRY, &destroy, 8) || destroy == 0) { g_move.stTemp = 0; return; }
    /* the animation wait first, as every game destroy (difficulty_stream.c, DIF_STREAM_ANIM_WAIT_ENTRY); this runs before
     * the async kick today (the level-update hook, game 0xAB519E), where the wait returns at once */
    uintptr_t wait = WorldStreamAnimWait(table);
    if (wait == 0) { Note("difficulty: near terrain: the tile's stamp units kept: the animation wait is not the reviewed one"); g_move.stTemp = 0; return; }
    ((WorldStreamAnimWaitFn)wait)(world);
    for (uint32_t k = 0; k < g_move.stTemp; k++) if (g_move.stUnit[k] != 0) ((WorldStreamDestroyUnitFn)destroy)(world, g_move.stUnit[k], 0, 0);
    g_move.stTemp = 0;
    g_nearStampOffset[0] = g_nearStampOffset[1] = 0.0f;
}

/* The locations whose stamps reach the cell's tile (their record xy +0 in generator metres, world + half), each with a
 * stamp unit of its own at the offset, copied for 0x2F. */
static uint32_t MoveStampsSelect(uint64_t locs, uint32_t count, float half)
{
    g_move.stCount = 0; g_move.stUnits = 0; g_move.stTemp = 0;
    if (g_move.stLoc == NULL)
    {
        g_move.stLoc = (uint8_t*)VirtualAlloc(NULL, (size_t)MOVE_STAMPS_MAX * 0xE0, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_move.stIndex = (uint32_t*)VirtualAlloc(NULL, (size_t)MOVE_STAMPS_MAX * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_move.stUnit = (uint64_t*)VirtualAlloc(NULL, (size_t)MOVE_STAMPS_MAX * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_move.stLoc == NULL || g_move.stIndex == NULL || g_move.stUnit == NULL) return 0;
    }
    if (!g_stream.captured || g_stream.level != g_worldLevel)
    { Note("difficulty: moving tiles: the streamer has no stamp poses for this level; the tile gets no stamp terrains"); return 0; }
    uint64_t world = MoveWorld();
    uintptr_t wi = 0, table = 0, spawn = 0;
    if (world == 0 || SafeRead(g_world.game + DIF_STREAM_INTERFACE, &wi, 8) || wi == 0 || SafeRead(wi + DIF_STREAM_TABLE, &table, 8) || table == 0 ||
        SafeRead(table + DIF_STREAM_SPAWN_ENTRY, &spawn, 8) || spawn == 0) return 0;
    const float reach = NEAR_SIDE / 2 + MOVE_STAMP_MARGIN;
    uint8_t rec[0xE0];
    uint32_t failed = 0;
    const uint32_t tableEnd = g_win.on && g_win.level == g_stream.level ? g_win.base : count; /* window slots come from the plan below */
    for (uint32_t i = 0; i < count && i < tableEnd && i < kStreamMaxLocations && g_move.stCount < MOVE_STAMPS_MAX; i++)
    {
        if (!g_streamPoseGood[i] || g_streamResource[i] == 0 || SafeRead(locs + (uint64_t)i * 0xE0, rec, sizeof rec)) continue;
        float at[2];
        memcpy(at, rec, 8);
        if (!isfinite(at[0]) || !isfinite(at[1])) continue;
        if (fabsf(at[0] - half - g_move.centre[0]) > reach || fabsf(at[1] - half - g_move.centre[1]) > reach) continue;
        float pose[16];
        memcpy(pose, g_streamPose[i], sizeof pose);
        pose[13] += MOVE_STAMP_OFFSET;
        uint64_t unit = ((WorldStreamSpawnUnitFn)spawn)(world, g_streamResource[i], pose, 0, 0);
        if (unit == 0) { failed++; continue; }
        memcpy(rec + 0xA8, &unit, 8);
        memcpy(g_move.stLoc + (size_t)g_move.stCount * 0xE0, rec, sizeof rec);
        g_move.stIndex[g_move.stCount] = i; g_move.stUnit[g_move.stUnits++] = unit;
        g_move.stCount++; g_move.stTemp = g_move.stUnits;
    }
    /* the plan's stamps in reach (g_plan: buckets of the map in generator metres), with op 0x31's pose (WindowPose) */
    uint32_t planned = 0, past = 0, pastInner = 0;
    if (g_plan.level == g_world.levels && g_plan.count != 0)
    {
        const float cell = g_plan.cell, gx = g_move.centre[0] + half, gy = g_move.centre[1] + half;
        int i0 = (int)floorf((gx - reach) / cell), i1 = (int)floorf((gx + reach) / cell), j0 = (int)floorf((gy - reach) / cell), j1 = (int)floorf((gy + reach) / cell);
        /* pass 0: the stamps whose terrain reaches the tile (MOVE_STAMP_REACH past its texels); pass 1: the rest of the
         * margin. A full room then leaves out only the outer margin, never a stamp that shapes the tile. */
        const float inner = NEAR_SIDE / 2 + MOVE_STAMP_REACH;
        for (int pass = 0; pass < 2; pass++)
        for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < STAMP_PLAN_GRID; j++)
            for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < STAMP_PLAN_GRID; i++)
            {
                int b = j * STAMP_PLAN_GRID + i;
                for (uint32_t e = g_plan.start[b]; e < g_plan.start[b + 1]; e++)
                {
                    const uint8_t* loc = g_stamps.cand[e].loc;
                    float at[2]; memcpy(at, loc, 8);
                    const float dx = fabsf(at[0] - half - g_move.centre[0]), dy = fabsf(at[1] - half - g_move.centre[1]);
                    if (dx > reach || dy > reach || (pass == 0) != (dx <= inner && dy <= inner)) continue;
                    if (g_move.stCount >= MOVE_STAMPS_MAX) { past++; pastInner += pass == 0; continue; }
                    uint64_t holder = 0, resource = 0;
                    memcpy(&holder, loc + DIF_STREAM_LOC_RESOURCE, 8);
                    if (holder == 0 || SafeRead((uintptr_t)holder + 8, &resource, 8) || resource == 0) continue;
                    float pose[16];
                    WindowPose(loc, 2.0f * half, pose);
                    pose[13] += MOVE_STAMP_OFFSET;
                    uint64_t unit = ((WorldStreamSpawnUnitFn)spawn)(world, resource, pose, 0, 0);
                    if (unit == 0) { failed++; continue; }
                    memcpy(rec, loc, sizeof rec);
                    memcpy(rec + 0xA8, &unit, 8);
                    memcpy(g_move.stLoc + (size_t)g_move.stCount * 0xE0, rec, sizeof rec);
                    g_move.stIndex[g_move.stCount] = 0xFFFFFFFFu; g_move.stUnit[g_move.stUnits++] = unit;
                    g_move.stCount++; g_move.stTemp = g_move.stUnits; planned++;
                }
            }
    }
    Note("difficulty: moving tiles: %u stamp terrains for the tile (%u of them the plan's%s)", g_move.stCount, planned, past ? "; more in reach than the tile's room" : "");
    if (past) Note("difficulty: moving tiles: %u of the plan's stamps in reach left out (room %u), %u of them within %.0f m of the tile", past, MOVE_STAMPS_MAX, pastInner, MOVE_STAMP_REACH);
    if (failed) Note("difficulty: moving tiles: %u stamp units could not be made at the offset", failed);
    if (g_move.stCount) { g_nearStampOffset[0] = 0.0f; g_nearStampOffset[1] = MOVE_STAMP_OFFSET; }
    return g_move.stCount;
}

/* The level's 0x2F (op = 0x2F) or 0x65 record on the staging lists: locations pass+8 / count [pass+0x28] for both, units
 * pass+0x10 / count [pass+0x38] for 0x65's unit mode, the resume index (0x2F +0x48, 0x65 +0x4C) from 0. */
static int MoveStampsRecord(uint32_t at, int op)
{
    if (SafeRead(g_near.records + (uint64_t)at * 0x19B0u, g_move.one, sizeof g_move.one)) return 0;
    uint8_t* pass = g_move.one + 8;
    uint64_t locs = (uint64_t)(uintptr_t)g_move.stLoc, cnt = (uint64_t)(uintptr_t)&g_move.stCount;
    memcpy(pass + 8, &locs, 8); memcpy(pass + 0x28, &cnt, 8);
    uint32_t zero = 0;
    if (op == 0x2F) memcpy(pass + 0x48, &zero, 4);
    else
    {
        uint64_t units = (uint64_t)(uintptr_t)g_move.stUnit, ucnt = (uint64_t)(uintptr_t)&g_move.stUnits;
        memcpy(pass + 0x10, &units, 8); memcpy(pass + 0x38, &ucnt, 8); memcpy(pass + 0x4C, &zero, 4);
    }
    g_move.phase = 0;
    return 1;
}

/* One record of the level's through the dispatcher on the context copy, a call; 1 when it moved on, 0 not yet, -1 on a
 * fault. The generation arena is reset after a finished op (0xF26140 at 0xF261FE). */
static int MoveRunRecord(void)
{
    volatile uint32_t index = 0;
    volatile int calls = 0;
    LONG fault = WorldTileGuarded(g_move.one, (uint64_t)(uintptr_t)g_move.ctx, &index, &g_move.phase, 0, 0, 1, &calls);
    if (fault)
    {
        Note("difficulty: moving tiles: op 0x%02X faulted (%08lX at game.dll+0x%llX)", g_move.one[0], (unsigned long)fault, (unsigned long long)(TileGuardSelf()->at - g_world.game));
        return -1;
    }
    if (index == 0) return 0;
    uint64_t arena = 0;
    memcpy(&arena, g_move.ctx, 8);
    typedef void (*ArenaFn)(uint64_t arena);
    if (arena != 0) ((ArenaFn)(g_world.game + 0x173BAB0u))(arena);
    return 1;
}

static uint64_t MoveWorld(void)
{
    uint64_t world = 0;
    memcpy(&world, g_move.ctx + 0x40, 8);
    return world;
}

/* The merge as the level's 0x66 and 0x67 leave it (the parts this build re-created). */
static void MoveTeardown(void)
{
    if (!g_move.merge) return;
    g_move.merge = 0;
    uintptr_t gen = 0, R = 0, api = 0, t8 = 0, t10 = 0;
    uint64_t world = MoveWorld(), env = 0;
    memcpy(&env, g_move.ctx + 0x98, 8);
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(gen, &R, 8) || R == 0 || SafeRead(g_world.game + 0x3326308u, &api, 8) || api == 0 ||
        SafeRead(api + 8, &t8, 8) || t8 == 0 || SafeRead(api + 0x10, &t10, 8) || t10 == 0 || world == 0)
    { Note("difficulty: moving tiles: the merge could not be torn down (no merge object or engine tables)"); return; }
    typedef void (*NodeFn)(uint64_t world, uint32_t node);
    typedef void (*UnitFn)(uint64_t world, uint64_t unit);
    typedef void (*PtrFn)(uint64_t p);
    typedef void (*NameFn)(uint32_t name);
    typedef void (*PkgFn)(uint64_t hash);
    NodeFn destroyNode = NULL; UnitFn destroyUnit = NULL; PtrFn releaseRead = NULL, release = NULL; NameFn unpublish = NULL; PkgFn pkgRelease = NULL;
    SafeRead(t8 + 0x60, &destroyNode, 8); SafeRead(t8 + 0x478, &destroyUnit, 8); SafeRead(t10 + 0x1E0, &releaseRead, 8); SafeRead(t10 + 0x190, &release, 8);
    SafeRead(t10 + 0x218, &unpublish, 8); SafeRead(t10 + 0x300, &pkgRelease, 8);
    if (destroyNode == NULL || destroyUnit == NULL || releaseRead == NULL || release == NULL || unpublish == NULL || pkgRelease == NULL) return;
    /* 0x66: the cameras, R+0x108 */
    uint32_t n0 = 0, n1 = 0; uint64_t r108 = 0;
    SafeRead(R + 8, &n0, 4); SafeRead(R + 0x18, &n1, 4); SafeRead(R + 0x108, &r108, 8);
    if (n0 != 0) destroyNode(world, n0);
    if (n1 != 0) destroyNode(world, n1);
    if (r108 != 0) destroyUnit(world, r108);
    *(volatile uint32_t*)(R + 8) = 0; *(volatile uint64_t*)(R + 0x108) = 0;
    /* 0x66: 0x94's lookup ([record+0x18]: its readback +0x10, its texture +0x18 under 0xDBCD240D) */
    uint64_t lookup = 0, rd = 0, tex = 0;
    if (!SafeRead(g_near.records + (uint64_t)g_move.at66 * 0x19B0u + 0x18, &lookup, 8) && lookup != 0)
    {
        if (!SafeRead(lookup + 0x10, &rd, 8) && rd != 0) releaseRead(lookup);
        if (!SafeRead(lookup + 0x18, &tex, 8) && tex != 0) { unpublish(0xDBCD240Du); release(tex); *(volatile uint64_t*)(lookup + 0x18) = 0; }
    }
    typedef void (*MergeFn)(uint64_t R, uint64_t world);
    typedef void (*EnvFn)(uint64_t manager, uint32_t slot);
    ((MergeFn)(g_world.game + 0xFCF2F0u))(R, world);
    if (env != 0) ((EnvFn)(g_world.game + 0x1009890u))(env, 5);
    /* 0x67: the four textures and their names */
    static const struct { uint32_t name, at, flag; } kFree[4] = {{0xA2003184u, 0x98, 0xE8}, {0x1BB41B90u, 0xA0, 0xE9}, {0x2FFE957Du, 0xB8, 0xEC}, {0xEBD2B190u, 0xD8, 0xF0}};
    for (int k = 0; k < 4; k++)
    {
        uint64_t t = 0;
        SafeRead(R + kFree[k].at, &t, 8);
        if (t != 0) { unpublish(kFree[k].name); release(t); }
        *(volatile uint64_t*)(R + kFree[k].at) = 0; *(volatile uint8_t*)(R + kFree[k].flag) = 0;
    }
    MovePackageLater(); /* not now: the destroys above are still in the world's command list */
    uint64_t arena = 0;
    memcpy(&arena, g_move.ctx, 8);
    typedef void (*ArenaFn)(uint64_t arena);
    if (arena != 0) ((ArenaFn)(g_world.game + 0x173BAB0u))(arena);
}

/* 0x65's cleanup queues one destroy per live layer unit (exe 0xE1200 -> 0x12D2D0) and up to 0x1000 stamp levels a call;
 * the world's list holds 8192 and the frame's drain empties it. A call runs only while the list is below the engine's
 * own "many destroys" mark (0x800, world interface +0x78), one a frame. */
#define MOVE_DESTROY_ROOM 0x800

/* The rest of a failed build, once its stamp terrains are away. */
static void MoveFailRest(void)
{
    MoveStampUnitsDestroy();
    MoveTeardown();
    if (g_move.failState >= MV_SPAWN && g_near.handle != 0 && g_near.handle != g_move.dropHandle)
    {
        typedef void (*DestroyFn)(uint64_t world, uint64_t handle);
        DestroyFn destroy = (DestroyFn)NearApi(0x08, 0x1E0);
        if (destroy != NULL) destroy(MoveWorld(), g_near.handle);
    }
    if (g_move.stashed)
    {
        MoveFreeLater(g_near.tex[0] != g_move.dropTex[0] ? g_near.tex[0] : 0, g_near.tex[1] != g_move.dropTex[1] ? g_near.tex[1] : 0,
            g_matband.tex != g_move.dropBand ? g_matband.tex : 0, g_matband.pixels != g_move.dropBandPixels ? g_matband.pixels : NULL);
    }
    MovePackageLater();
    MoveCellSkip(g_move.cell[0], g_move.cell[1], GetTickCount64());
    MoveRestoreDrop();
    g_move.failed++;
    g_move.state = MV_IDLE;
}

/* A build that did not finish: the merge torn down, the new tile's maps and terrain gone, the drop tile's back. With
 * stamp terrains spawned, 0x65 takes them away first, one call a frame (MV_FAILING), as MV_UNSTAMP does. */
static void MoveFail(const char* why)
{
    Note("difficulty: moving tiles: the tile of cell (%d, %d) is not built: %s", g_move.cell[0], g_move.cell[1], why);
    if (g_nearActive) NearReplayEnd(0);
    if (g_move.state != MV_FAILING) g_move.failState = g_move.state;
    if (g_move.stamped && g_move.state != MV_FAILING && MoveStampsRecord(g_near.at65, 0x65))
    {
        g_move.waits = 0; g_move.ringWaits = 0; g_move.state = MV_FAILING;
        return;
    }
    if (g_move.stamped) Note("difficulty: moving tiles: the stamp terrains were not all taken away");
    g_move.stamped = 0;
    if (g_move.stTemp != 0 && g_move.state != MV_FAILING) { g_move.ringWaits = 0; g_move.state = MV_FAILING; return; } /* the stamp units wait for the ring */
    MoveFailRest();
}

static void MoveRelease(int slot)
{
    NearTile t = g_tiles[slot];
    uint64_t world = MoveWorld();
    NearTileRemove(slot);
    g_tiles[slot].tex[0] = g_tiles[slot].tex[1] = g_tiles[slot].band = 0; g_tiles[slot].bandPixels = NULL;
    int raised = NearRaiseCoarse(world, &g_tileHidden[slot]);
    g_tileHidden[slot].lowered = 0;
    NearHideUnion(world, "a tile released");
    typedef void (*DestroyFn)(uint64_t world, uint64_t handle);
    DestroyFn destroy = (DestroyFn)NearApi(0x08, 0x1E0);
    if (destroy != NULL && t.handle != 0) destroy(world, t.handle);
    MoveFreeLater(t.tex[0], t.tex[1], t.band, t.bandPixels);
    g_move.released++;
    Note("difficulty: moving tiles: the tile of cell (%d, %d) released (no player within %.0f m for %u s); the coarse collision %s; %d tiles live",
        t.cell[0], t.cell[1], (double)MOVE_KEEP, MOVE_KEEP_MS / 1000, raised ? "raised back" : "NOT raised back (the level's copy did not read)", NearTilesLive());
}

/* The context's arena ([ctx+0]): the generation thread's arena is destroyed when the level is done (0x173BB80: its
 * chunks freed, its head zeroed; the allocator 0x173B8D0 then reads [0] at 0x173B936, run s72d's 0x2F fault). The
 * build gets its own, laid out as 0x173B700 makes one: arena {head chunk +0, lock +8 (0), owner thread index +0xC (-1:
 * the allocator skips the index call), growth size +0x10 (0x100000)}; chunk {used +0, data +8, capacity +0x10, next
 * +0x18}. The reset (0x173BAB0) frees the chunks the allocator grew (engine memory, put in front of ours) and sets the
 * last one's used to 0: ours is the last and is never freed. */
#define MOVE_ARENA_BYTES (64u << 20)
static struct { uint64_t head; LONG lock; int32_t owner; uint64_t growth; } g_moveArena;
static struct { uint64_t used, data, capacity, next; } g_moveChunk;

static int MoveArena(void)
{
    if (g_moveChunk.data == 0)
    {
        void* p = VirtualAlloc(NULL, MOVE_ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (p == NULL) { Note("difficulty: moving tiles: no memory for the build's arena"); return 0; }
        g_moveChunk.data = (uint64_t)(uintptr_t)p; g_moveChunk.capacity = MOVE_ARENA_BYTES;
    }
    g_moveChunk.used = 0; g_moveChunk.next = 0;
    g_moveArena.head = (uint64_t)(uintptr_t)&g_moveChunk; g_moveArena.lock = 0; g_moveArena.owner = -1; g_moveArena.growth = 0x100000;
    return 1;
}

static int MoveStart(void)
{
    uint64_t block = 0;
    if (SafeRead(g_worldLevel + 0x2A8, &block, 8) || block == 0 || block != g_near.records) return 0;
    memcpy(g_move.ctx, (void*)(uintptr_t)(block + 0x19B008u), sizeof g_move.ctx);
    uint64_t settings = 0;
    memcpy(&settings, g_move.ctx + 8, 8);
    if (settings == 0 || SafeRead(settings, g_move.settings, sizeof g_move.settings)) return 0;
    uint64_t mine = (uint64_t)(uintptr_t)g_move.settings;
    memcpy(g_move.ctx + 8, &mine, 8);
    g_move.ctx[0xE1] = 0; g_move.ctx[0xE2] = 0; /* abort, restart */
    if (!MoveArena()) return 0;
    { uint64_t arena = (uint64_t)(uintptr_t)&g_moveArena; memcpy(g_move.ctx, &arena, 8); }
    /* the drop tile's maps aside: the replay creates the new tile's in g_near / g_matband */
    g_move.dropTex[0] = g_near.tex[0]; g_move.dropTex[1] = g_near.tex[1]; g_move.dropHandle = g_near.handle;
    g_move.dropCentre[0] = g_near.centre[0]; g_move.dropCentre[1] = g_near.centre[1];
    g_move.dropBand = g_matband.tex; g_move.dropBandPixels = g_matband.pixels; g_move.dropBandServed = g_matband.served;
    g_move.stashed = 1;
    g_near.tex[0] = g_near.tex[1] = 0; g_matband.tex = 0; g_matband.pixels = NULL;
    InterlockedExchange(&g_moveBuilding, 1);
    g_move.acquired = 0; g_move.merge = 0; g_move.waits = 0; g_move.frames = 0; g_move.started = GetTickCount64();
    g_move.state = MV_PACKAGE;
    Note("difficulty: moving tiles: building cell (%d, %d) at (%.0f, %.0f) for a player %.0f m from it; %d tiles live", g_move.cell[0], g_move.cell[1],
        (double)g_move.centre[0], (double)g_move.centre[1], (double)MoveCellDistance(g_move.cell[0], g_move.cell[1], g_move.player[0][0], g_move.player[0][1]),
        NearTilesLive());
    return 1;
}

/* The replay's merge state for the cell (as TerrainNearReplay's first call sets it up at load). */
static int MoveReplayBegin(void)
{
    uintptr_t gen = 0, lg = 0;
    uint64_t settings = 0;
    uint32_t size = 0, tiles = 0;
    memcpy(&settings, g_move.ctx + 8, 8);
    if (SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0) return 0;
    lg = gen + 0x8758A8u;
    g_near.lg = lg; g_near.settings = settings;
    if (SafeRead(gen, &g_near.R, 8) || g_near.R == 0 || SafeRead(g_near.R + 0x110, &g_near.gui, 8) || SafeRead(g_near.R, &g_near.cam[0], 8) ||
        SafeRead(g_near.R + 8, &g_near.node[0], 4) || SafeRead(g_near.R + 0x10, &g_near.cam[1], 8) || SafeRead(g_near.R + 0x18, &g_near.node[1], 4) ||
        SafeRead(settings + 0x2C, &size, 4) || SafeRead(lg + 0x120, &g_near.samples, 4) || SafeRead(lg + 0x124, &tiles, 4) || g_near.cam[0] == 0 ||
        g_near.cam[1] == 0 || g_near.gui == 0)
    { Note("difficulty: moving tiles: the merge was not re-created (R 0x%llX)", (unsigned long long)g_near.R); return 0; }
    g_near.world = MoveWorld();
    if (size != 16384u || tiles != 1 || !(g_near.samples >= 1024.0f && g_near.samples <= 8192.0f) || g_near.world == 0)
    { Note("difficulty: moving tiles: the merge is not the reviewed one (size %u, %u tiles, %.0f samples)", size, tiles, (double)g_near.samples); return 0; }
    g_near.centre[0] = g_move.centre[0]; g_near.centre[1] = g_move.centre[1];
    g_near.world_size = (float)size;
    g_near.scale = g_near.world_size / NEAR_SIDE;
    for (int k = 0; k < 2; k++) g_near.offset[k] = -(g_near.centre[k] + g_near.world_size / 2 - NEAR_SIDE / 2) * g_near.samples / NEAR_SIDE;
    if (!NearPlan(g_near.records, g_near.at65)) { Note("difficulty: moving tiles: the level's merge records are not all there"); return 0; }
    g_near.step = 0; g_near.calls = 0; g_near.building = 0; g_near.tilesDrawn = 0; g_near.tilesFailed = 0; g_near.waits = 0; g_near.frames = 0;
    g_near.matLeave = 0; g_near.handle = 0;
    g_near.started = GetTickCount64();
    g_stepDump.phase = 0;
    g_near.state = NEAR_START; g_near.loggedStep = 0; g_near.stampLeave = 0;
    MoveHold(1);
    return 1;
}

/* After the finalize: the coarse terrain under the tile, its ground, the tile in the list. */
static void MoveTileIn(uint64_t world)
{
    int slot = g_move.slot, x0 = 0, y0 = 0;
    NearCellSquare(g_move.cell[0], g_move.cell[1], g_move.centre[0], g_move.centre[1], &x0, &y0);
    NearHideCoarse(world, x0, y0, &g_tileHidden[slot]);
    NearGround ground;
    NearGroundOf(world, g_near.handle, NEAR_SPAN, g_move.centre[0], g_move.centre[1], &ground, &g_groundBuf[slot]);
    NearTileAdd(slot, g_move.centre[0], g_move.centre[1], g_near.handle, &ground);
    g_tiles[slot].cell[0] = g_move.cell[0]; g_tiles[slot].cell[1] = g_move.cell[1];
    g_tiles[slot].tex[0] = g_near.tex[0]; g_tiles[slot].tex[1] = g_near.tex[1];
    g_tiles[slot].band = g_matband.tex; g_tiles[slot].bandPixels = g_matband.pixels;
    NearHideUnion(world, "a tile built in mission");
    g_move.built++;
    Note("difficulty: moving tiles: the tile of cell (%d, %d) is in after %.1f s (%u frames): ground %s, centre %.2f m (coarse %.2f m); %d tiles live; craters held %lld updates",
        g_move.cell[0], g_move.cell[1], (double)(GetTickCount64() - g_move.started) / 1000.0, g_move.frames, ground.v != NULL ? "read" : "NOT read",
        (double)NearGroundAt(&ground, g_move.centre[0], g_move.centre[1]), (double)NearGroundAt(&g_nearCoarseGround, g_move.centre[0], g_move.centre[1]),
        NearTilesLive(), (long long)g_moveHeldUpdates);
    MoveRestoreDrop(); /* g_near.tex / g_matband: the drop tile's again (the new tile's are in its entry) */
}

/* ---- Units left under the ground (living world audit 1f criterion 7) ----------------------------------------------
 * KB enemies/living-world-tile-switch-falls (subagent; checked by me in game.dll): the motion loop 0x59F440 gives the
 * records below [mgr+0x4890] (mgr [game+0x3326558]) the step 0x59FED0; the mode is +0x1C of the record's 0x2C entry at
 * [mgr+0x48D8] (0x5A03B2; jump table 0x5A1884: modes 4 and 7 -> 0x5A0B70), its active byte +0x20 (0x59FF3D). Those
 * modes look the entity up in the ground system [game+0x3326440] (hash header +0x28, records [+0x48] of 0x3C bytes;
 * 0x5A0B70..0x5A0BF8, 0x5A0D22) and read its ray's hit flag +0x38: hit -> z := the hit's z, miss -> gravity. The ray
 * starts `up` (+0x2C) over the unit, straight down. The step's output position is the 0xA4 record +0..+8 at
 * [mgr+0x48D0] (0x5A16D5 / 0x5A16DE); the authority bit is [descriptor+0x14] bit 0 (0x5A2D2C). A heightfield has one
 * surface, so a unit left under it never meets it again and falls to the motion bound: the coarse collision lowered
 * under a new tile's square while it stood on the coarse ground below the tile (lw89a: 7 of 7 falls began 0.2-0.35 s
 * after NearHideCoarse), the coarse raised back over the tile at release, or a step on a tile into a rise taller than
 * `up`. Each frame, before the ray is cast (MoveTick runs in the level update at 0xAB519E, before the cast at 0xAB5597
 * and the world update at 0xAB5EC9): a unit in mode 4 or 7, active, ours to move, whose last ray missed and that is
 * more than MOVE_KEEP_UNDER under the ground there (the live tile's heights inside its square, else the level's coarse
 * ones) is put back on it by the game's teleport 0x5A2BB0, as the test teleport does. A missed ray means gravity, so
 * these are exactly the units vanilla would let fall; nothing standing is touched. Vanilla units too: garrisons and
 * objective units standing on the coarse ground when a tile comes are hit the same way. */
#define MOVE_KEEP_UNDER 0.3f
#define MOVE_KEEP_RECORDS 8192
static uint8_t g_keepModes[MOVE_KEEP_RECORDS * 0x2C], g_keepPos[MOVE_KEEP_RECORDS * 0xA4];
static struct { int pins; LONG level; uint32_t lifted, fromTile, fromCoarse, held, refused; uint64_t frame; } g_keep;
/* Each unit put back, by entity: its lift count and whether the collision under it was found (for the notes).
 * KB living-world-tile-switch-falls section 6 (subagent; checked by me in game.dll): the game's teleport 0x5A2BB0 writes
 * the position but never the velocity, the 0x84 record at [mgr+0x48C8] (+8 vz: the no-hit branch adds gravity to it at
 * 0x5A101F, only the grounded branch writes it, at 0x5A10AA). lw92a's squad 1913 kept its free-fall speed through 2850
 * lifts: each one was undone by the next frame's fall at that speed (the drop grew g dt per frame, vz = g (t - 11.8 s)).
 * So a lift also sets vz = 0, and is made onto the collision as it is: the ground query (0x13CA380, as the teleport's own
 * check 0x909B20 uses it) cast down from 1.5 m over the expected ground; when it hits within MOVE_KEEP_FIND of the
 * expected ground the unit goes 5 cm over the hit, else (no collision there: lw92a's released square, under study) it is
 * held 10 cm over the expected ground, vz 0, every frame it would fall. No caps: a unit under the ground with a missed
 * ray is always falling (P). The 3843ca65 rules (only while falling, three tries) are gone. */
#define MOVE_KEEP_FIND 3.0f
#define MOVE_KEEP_TRACKS 1024
static struct { uint32_t entity; uint32_t lifts; uint8_t noCollision; } g_keepTrack[MOVE_KEEP_TRACKS];
static int MoveKeepTrack(uint32_t e)
{
    uint32_t h = (e * 2654435761u) % MOVE_KEEP_TRACKS;
    for (uint32_t k = 0; k < MOVE_KEEP_TRACKS; k++, h = (h + 1) % MOVE_KEEP_TRACKS)
    {
        if (g_keepTrack[h].entity == e) return (int)h;
        if (g_keepTrack[h].entity == 0) { memset(&g_keepTrack[h], 0, sizeof g_keepTrack[h]); g_keepTrack[h].entity = e; return (int)h; }
    }
    return -1;
}

/* The level's coarse heights (lg+0xD8, the rows the coarse collision was made from and NearRaiseCoarse writes back) at a
 * point, on the triangles NearCoarseTri uses; -9999 outside or unreadable. */
static float MoveCoarseHere(uintptr_t data, float wx, float wy)
{
    const float step = 16383.5f / 4095.0f;
    float fx = (wx + 8191.75f) / step, fy = (8191.75f - wy) / step;
    if (!(fx >= 0 && fy >= 0 && fx < 4095.0f && fy < 4095.0f)) return -9999.0f;
    uint32_t c = (uint32_t)fx, r = (uint32_t)fy;
    float ax = fx - c, ay = fy - r;
    uint16_t no[2], so[2];
    if (SafeRead(data + ((uint64_t)r * 4096 + c) * 2, no, 4) || SafeRead(data + ((uint64_t)(r + 1) * 4096 + c) * 2, so, 4)) return -9999.0f;
    float nw = no[0], ne = no[1], sw = so[0], se = so[1];
    float z = ax <= ay ? sw + ax * (se - sw) + (1 - ay) * (nw - sw) : ne + (1 - ax) * (nw - ne) + ay * (se - ne);
    return z / 65535.0f * 1000.0f - 500.0f;
}

/* The ground as drawn at a point on an extra large level with moving tiles: the live tile's heights inside its hidden
 * square, else the level's coarse rows (lg+0xD8); *tile the slot or -1; -9999 when it does not read. */
static float MoveDrawnGround(float x, float y, int* tile)
{
    *tile = -1;
    for (int k = 0; k < NEAR_TILES_MAX; k++)
        if (g_tiles[k].live && g_tiles[k].level == g_world.levels && g_tiles[k].ground.v != NULL &&
            fabsf(x - g_tiles[k].centre[0]) < NEAR_HIDE / 2 && fabsf(y - g_tiles[k].centre[1]) < NEAR_HIDE / 2)
        { *tile = k; return NearGroundAt(&g_tiles[k].ground, x, y); }
    uintptr_t gen = 0, coarse = 0; uint32_t dims[3] = {0};
    if (g_worldLevel == 0 || SafeRead(g_worldLevel + 0x2C8, &gen, 8) || gen == 0 || SafeRead(gen + 0x8758A8u + 0xD8, dims, 12) || dims[0] != 4096 ||
        dims[1] != 4096 || NearTexelBytes(dims[2]) != 2 || SafeRead(gen + 0x8758A8u + 0xD8 + 0x10, &coarse, 8) || coarse == 0) return -9999.0f;
    return MoveCoarseHere(coarse, x, y);
}
/* Moving tiles on for this level (an extra large level whose detailed tiles follow the players). */
static int MoveTilesOn(void) { return g_moveInstalled && g_move.on && g_move.level == g_world.levels; }
/* A point inside the square of the cell being built now (its tile comes in at the end, its coarse ground lowered then). */
static int MoveCellBuilding(float x, float y)
{
    return MoveTilesOn() && g_move.state != MV_IDLE && fabsf(x - g_move.centre[0]) < NEAR_HIDE / 2 && fabsf(y - g_move.centre[1]) < NEAR_HIDE / 2;
}

static void MoveGroundKeep(void)
{
    static uintptr_t game;
    if (game == 0) game = (uintptr_t)GetModuleHandleW(L"game.dll");
    if (g_keep.pins == 0)
    {
        g_keep.pins = game && ApiCodeMatches(game + 0x59F478u, "3B 9F 90 48 00 00") && ApiCodeMatches(game + 0x573F5Cu, "44 8B 80 8C 48 00 00") &&
            ApiCodeMatches(game + 0x5A03B2u, "8B 44 10 1C FF C8 83 F8 06") && ApiCodeMatches(game + 0x5A1884u, "D7 03 5A 00 0F 04 5A 00 58 04 5A 00 70 0B 5A 00 04 05 5A 00 B7 10 5A 00 70 0B 5A 00") &&
            ApiCodeMatches(game + 0x59FF3Du, "80 7C 08 20 00") && ApiCodeMatches(game + 0x5A0B79u, "4C 8B 35 C0 58 D8 02") &&
            ApiCodeMatches(game + 0x5A0BE6u, "48 6B F1 3C 49 03 76 48 0F B6 46 38") && ApiCodeMatches(game + 0x5A16D5u, "F2 45 0F 11 6D 00 8B 45 98 41 89 45 08") &&
            ApiCodeMatches(game + 0x5A2C62u, "49 69 F4 A4 00 00 00") && ApiCodeMatches(game + 0x5A2BB0u, "48 89 5C 24 08 48 89 6C 24 10 56 57 41 54 41 56 41 57 48 83 EC 40") ? 1 : -1;
        Note(g_keep.pins > 0 ? "difficulty: moving tiles: units left under the ground are put back on it (motion modes 4 and 7)"
                             : "difficulty: moving tiles: the motion step is not the reviewed code; units left under the ground are not put back");
    }
    if (g_keep.pins < 0) return;
    if (g_keep.level != g_world.levels)
    { g_keep.level = g_world.levels; g_keep.lifted = g_keep.fromTile = g_keep.fromCoarse = g_keep.held = g_keep.refused = 0; memset(g_keepTrack, 0, sizeof g_keepTrack); }
    g_keep.frame++;
    uintptr_t mgr = 0, modes = 0, pos = 0, descs = 0, gsys = 0, grecs = 0, gen = 0, coarse = 0;
    uint32_t dims[3] = {0};
    if (!EvpPtr(game + 0x3326558u, &mgr) || !EvpPtr(mgr + 0x48D8, &modes) || !EvpPtr(mgr + 0x48D0, &pos) || !EvpPtr(mgr + 0x48B8, &descs) ||
        !EvpPtr(game + 0x3326440u, &gsys) || !EvpPtr(gsys + 0x48, &grecs)) return;
    if (!SafeRead(g_worldLevel + 0x2C8, &gen, 8) && gen != 0 && !SafeRead(gen + 0x8758A8u + 0xD8, dims, 12) && dims[0] == 4096 && dims[1] == 4096 &&
        NearTexelBytes(dims[2]) == 2) SafeRead(gen + 0x8758A8u + 0xD8 + 0x10, &coarse, 8);
    uint32_t n = EvpU32(mgr + 0x4890, 0), all = EvpU32(mgr + 0x488C, 0);
    if (n > all) n = all;
    if (n > MOVE_KEEP_RECORDS) n = MOVE_KEEP_RECORDS;
    if (n == 0 || SafeRead(modes, g_keepModes, (size_t)n * 0x2C) || SafeRead(pos, g_keepPos, (size_t)n * 0xA4)) return;
    for (uint32_t i = 0; i < n; i++)
    {
        const uint8_t* m = g_keepModes + (size_t)i * 0x2C;
        uint32_t mode; memcpy(&mode, m + 0x1C, 4);
        if ((mode != 4 && mode != 7) || m[0x20] == 0) continue;
        float p[3]; memcpy(p, g_keepPos + (size_t)i * 0xA4, 12);
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) continue;
        int tile = -1; float ground = -9999.0f;
        for (int k = 0; k < NEAR_TILES_MAX; k++)
            if (g_tiles[k].live && g_tiles[k].level == g_world.levels && g_tiles[k].ground.v != NULL &&
                fabsf(p[0] - g_tiles[k].centre[0]) < NEAR_HIDE / 2 && fabsf(p[1] - g_tiles[k].centre[1]) < NEAR_HIDE / 2)
            { tile = k; ground = NearGroundAt(&g_tiles[k].ground, p[0], p[1]); break; }
        if (tile < 0 && coarse) ground = MoveCoarseHere(coarse, p[0], p[1]);
        if (ground < -9000.0f || !(p[2] < ground - MOVE_KEEP_UNDER)) continue;
        uintptr_t d = 0; uint32_t e = 0, gi = 0; uint8_t flags = 0, hit = 1; float ray[2] = {0, 0};
        if (!EvpPtr(descs + 8u * i, &d) || d == 0 || SafeRead(d + 0x14, &flags, 1) || !(flags & 1) || (e = EvpU32(d + 8, 0)) == 0) continue;
        if (!EvpHash(gsys + 0x28, e, 8, 4, &gi) || gi >= 0x100000 || SafeRead(grecs + 0x3Cu * gi + 0x38, &hit, 1) || hit) continue;
        SafeRead(grecs + 0x3Cu * gi + 0x2C, ray, 8);
        if (((uint8_t (*)(void*, uint32_t))(game + 0x4A52B0u))(NULL, e)) continue; /* attached: the teleport stops half-way */
        int tk = MoveKeepTrack(e);
        float to[3] = {p[0], p[1], ground + 0.1f}, floor_ = NAN;
        uintptr_t world = 0;
        if (EvpPtr(game + 0x346D508u, &world))
        {
            typedef uint8_t (*GroundFn)(uintptr_t world, const float* in, float* outZ, uint8_t reject, float* normal, uint8_t mode);
            float in[3] = {p[0], p[1], ground + 1.5f - 500.0f}; /* the query casts from in z + 500 m down 1000 m */
            ((GroundFn)(game + 0x13CA380u))(world, in, &floor_, 0, NULL, 0);
        }
        int found = isfinite(floor_) && fabsf(floor_ - ground) <= MOVE_KEEP_FIND;
        if (found) to[2] = floor_ + 0.05f;
        ((void (*)(void*, uint32_t, const float*))(game + 0x5A2BB0u))(NULL, e, to);
        uintptr_t vel = 0; float vz = NAN, zero = 0.0f;
        if (EvpPtr(mgr + 0x48C8, &vel) && !SafeRead(vel + 0x84u * i + 8, &vz, 4)) *(volatile float*)(vel + 0x84u * i + 8) = zero;
        g_keep.lifted++; if (tile >= 0) g_keep.fromTile++; else g_keep.fromCoarse++;
        if (!found) g_keep.held++;
        uint32_t lifts = 0;
        if (tk >= 0) { lifts = ++g_keepTrack[tk].lifts; if (!found && !g_keepTrack[tk].noCollision) { g_keepTrack[tk].noCollision = 1; g_keep.refused++; } }
        if (g_keep.lifted <= 200 || g_keep.lifted % 50 == 0 || lifts == 1 || (!found && lifts % 100 == 1))
            Note("difficulty: moving tiles: unit %08X (motion mode %u) under the %s at (%.1f, %.1f): z %.2f, ground %.2f (%.2f under), vz %.2f, its ray missed (up %.2f, below %.2f); the collision %s: put back at %.2f, vz 0 (its lift %u; %u this level, %u on tiles, %u on the coarse ground, %u without collision under them)",
                e, mode, tile >= 0 ? "tile's ground" : "coarse ground", (double)p[0], (double)p[1], (double)p[2], (double)ground, (double)(ground - p[2]), (double)vz,
                (double)ray[0], (double)ray[1], found ? "found" : "NOT found within 3 m", (double)to[2], lifts, g_keep.lifted, g_keep.fromTile, g_keep.fromCoarse, g_keep.refused);
    }
}

static void MoveTick(uint64_t level)
{
    if (!g_moveInstalled || level == 0 || level != g_worldLevel || g_world.current != 8.0f || !TerrainOn() || !NearOn() || !MoveOn()) return;
    uint32_t state = 0;
    if (SafeRead(level + 0x1E4, &state, 4) || state != 3) return;
    if (g_near.level != g_world.levels || (g_move.state == MV_IDLE && g_near.state != NEAR_IN)) return;
    ULONGLONG now = GetTickCount64();
    if (g_move.level != g_world.levels)
    {
        g_move.level = g_world.levels; g_move.levelPtr = level; g_move.state = MV_IDLE; g_move.on = 0;
        g_move.built = g_move.failed = g_move.released = 0;
        memset(g_move.skip, 0, sizeof g_move.skip);
        if (!(g_ring.level == g_world.levels && g_ring.applied)) { Note("difficulty: moving tiles: off for this level (no grid ring at load)"); return; }
        if (!g_tiles[0].live || g_tiles[0].level != g_world.levels) { Note("difficulty: moving tiles: off for this level (no drop tile)"); return; }
        g_move.at2F = g_move.at37 = g_move.at38 = g_move.at66 = 0;
        for (uint32_t i = g_near.at65; i > 0; i--)
        {
            uint32_t op = 0;
            if (SafeRead(g_near.records + (uint64_t)(i - 1) * 0x19B0u, &op, 4)) break;
            if (op == 0x38 && g_move.at38 == 0) g_move.at38 = i - 1;
            if (op == 0x37 && g_move.at37 == 0) g_move.at37 = i - 1;
            if (op == 0x2F && g_move.at2F == 0) g_move.at2F = i - 1; /* after 0x38 in the list (s72c: 0x37 67, 0x38 78, 0x2F 102) */
            if (g_move.at2F != 0 && g_move.at37 != 0 && g_move.at38 != 0) break;
        }
        /* 0x4C between 0x38 and 0x65: it makes R+0xB8 (0x2FFE957D), which 0x67 releases and the replay's 0x64 binds */
        g_move.at4C = 0;
        for (uint32_t i = g_move.at38 + 1; i < g_near.at65; i++)
        {
            uint32_t op = 0;
            if (!SafeRead(g_near.records + (uint64_t)i * 0x19B0u, &op, 4) && op == 0x4C) { g_move.at4C = i; break; }
        }
        for (uint32_t i = g_near.at65 + 1; i < g_near.end; i++)
        {
            uint32_t op = 0;
            if (!SafeRead(g_near.records + (uint64_t)i * 0x19B0u, &op, 4) && op == 0x66) { g_move.at66 = i; break; }
        }
        if (g_move.at2F == 0 || g_move.at37 == 0 || g_move.at38 == 0 || g_move.at4C == 0 || g_move.at66 == 0)
        { Note("difficulty: moving tiles: off for this level (records 0x2F %u, 0x37 %u, 0x38 %u, 0x4C %u, 0x66 %u)", g_move.at2F, g_move.at37, g_move.at38, g_move.at4C, g_move.at66); return; }
        g_move.origin[0] = g_tiles[0].centre[0]; g_move.origin[1] = g_tiles[0].centre[1];
        g_move.on = 1;
        Note("difficulty: moving tiles: on: cells %d..%d east, %d..%d north of the drop tile's at (%.0f, %.0f), %.1f m each; wanted within %.0f m, kept within %.0f m "
            "for %u s, %d at most; records 0x2F %u, 0x37 %u, 0x38 %u, 0x65 %u, 0x66 %u", g_ring.a0, g_ring.a1, g_ring.b0, g_ring.b1, (double)g_move.origin[0],
            (double)g_move.origin[1], (double)NEAR_HIDE, (double)MOVE_WANT, (double)MOVE_KEEP, MOVE_KEEP_MS / 1000, MOVE_CAP, g_move.at2F, g_move.at37, g_move.at38,
            g_near.at65, g_move.at66);
    }
    if (!g_move.on) return;
    MoveGroundKeep(); /* before the frame's ground rays are cast */
    MoveFreeTick(0);
    MovePlayers();
    if (g_move.state == MV_IDLE)
    {
        if (g_move.players == 0) return;
        for (int k = 0; k < NEAR_TILES_MAX; k++)
        {
            if (!g_tiles[k].live || g_tiles[k].level != g_world.levels) continue;
            for (int p = 0; p < g_move.players; p++)
                if (MoveCellDistance(g_tiles[k].cell[0], g_tiles[k].cell[1], g_move.player[p][0], g_move.player[p][1]) <= MOVE_KEEP) { g_tiles[k].seen = now; break; }
        }
        for (int k = 1; k < NEAR_TILES_MAX; k++) /* the drop tile (slot 0) stays */
            if (g_tiles[k].live && g_tiles[k].level == g_world.levels && now - g_tiles[k].seen > MOVE_KEEP_MS)
            {
                LARGE_INTEGER t0, t1, hz; QueryPerformanceCounter(&t0);
                MoveRelease(k);
                QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&hz);
                Note("difficulty: moving tiles: the release took %.1f ms on the game thread", (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart);
                return;
            }
        if (NearTilesLive() >= MOVE_CAP) return;
        float best = 1e9f; int ba = 0, bb = 0, bp = -1;
        for (int p = 0; p < g_move.players; p++)
        {
            int pa = (int)floorf((g_move.player[p][0] - g_move.origin[0]) / NEAR_HIDE + 0.5f), pb = (int)floorf((g_move.player[p][1] - g_move.origin[1]) / NEAR_HIDE + 0.5f);
            for (int a = pa - 1; a <= pa + 1; a++)
                for (int b = pb - 1; b <= pb + 1; b++)
                {
                    if (!MoveCellValid(a, b) || MoveCellLive(a, b) || MoveCellSkipped(a, b, now)) continue;
                    float d = MoveCellDistance(a, b, g_move.player[p][0], g_move.player[p][1]);
                    if (d <= MOVE_WANT && d < best) { best = d; ba = a; bb = b; bp = p; }
                }
        }
        if (bp < 0) return;
        g_move.slot = NearTileFree();
        if (g_move.slot < 0) return;
        g_move.cell[0] = ba; g_move.cell[1] = bb;
        MoveCellCentre(ba, bb, g_move.centre);
        if (bp != 0) { float t0 = g_move.player[0][0], t1 = g_move.player[0][1]; g_move.player[0][0] = g_move.player[bp][0]; g_move.player[0][1] = g_move.player[bp][1]; g_move.player[bp][0] = t0; g_move.player[bp][1] = t1; }
        if (!MoveStart()) { Note("difficulty: moving tiles: the level's context did not read; no tile"); MoveCellSkip(ba, bb, now); return; }
    }
    g_move.frames++;
    if (g_move.state == MV_PACKAGE)
    {
        typedef int (*HasFn)(uint64_t hash);
        typedef void (*PkgFn)(uint64_t hash);
        HasFn has = (HasFn)NearApi(0x10, 0x310);
        PkgFn acquire = (PkgFn)NearApi(0x10, 0x2F0);
        if (has == NULL || acquire == NULL) { MoveFail("the package interface is not there"); return; }
        /* The package calls queue requests (exe 0x603670 / 0x603A80, ring mgr+0x218, 512 x {hash, context, load flag}); no
         * count. An acquire turns this context's queued unload back into a load, is a no-op when the package is listed
         * and nothing is queued (0x60E800), else queues a load. The unload is done when the consumer (0x603290) reaches
         * it (0x60E410), so has() (0x321330) still answers 1 while a release waits in the queue: run s85a/s85b built on
         * that, the package went, and 0x38's gui material lookup (exe 0x264620 via [api+0xD0]+0x238) found no material
         * (set_scalar on 0, exe 0x4F33DA). So the build asks whenever it does not hold the package, whatever has() says. */
        if (g_move.pkgPending && !g_move.acquired) { g_move.pkgPending = 0; g_move.acquired = 1; } /* not released yet: taken back */
        if (!g_move.acquired) { acquire(MOVE_PACKAGE); g_move.acquired = 1; Note("difficulty: moving tiles: the merge package asked for"); }
        if (has(MOVE_PACKAGE) == 0)
        {
            if (++g_move.waits > 3600) MoveFail("the merge package did not load");
            return;
        }
        g_move.waits = 0; g_move.stamped = 0; g_move.stCount = g_move.stUnits = 0;
        uint64_t locs = 0, countAt = 0; uint32_t count = 0;
        uint64_t rec = g_near.records + (uint64_t)g_move.at2F * 0x19B0u;
        if (SafeRead(rec + 8 + 8, &locs, 8) || SafeRead(rec + 8 + 0x28, &countAt, 8) || locs == 0 || countAt == 0 || SafeRead(countAt, &count, 4))
        { MoveFail("the 0x2F record's locations did not read"); return; }
        static int stamps = -1; /* HELLFORGE_XL_MOVE_STAMPS=0: no stamp terrains for the tile (0x63 draws none) */
        if (stamps < 0) { char t[4] = ""; stamps = !(GetEnvironmentVariableA("HELLFORGE_XL_MOVE_STAMPS", t, sizeof t) > 0 && t[0] == '0'); }
        if (stamps) MoveStampsSelect(locs, count, g_near.world_size > 0 ? g_near.world_size / 2 : 8192.0f);
        if (g_move.stCount != 0 && !MoveStampsRecord(g_move.at2F, 0x2F)) { MoveFail("record 0x2F did not read"); return; }
        g_move.state = g_move.stCount != 0 ? MV_STAMPS : MV_KICK;
        if (g_move.state == MV_KICK && SafeRead(g_near.records + (uint64_t)g_move.at37 * 0x19B0u, g_move.one, sizeof g_move.one)) { MoveFail("record 0x37 did not read"); return; }
        g_move.phase = g_move.state == MV_KICK ? 0 : g_move.phase;
        return;
    }
    if (g_move.state == MV_STAMPS)
    {
        g_move.stamped = 1; /* from the first call some of the terrains may be spawned */
        int r = MoveRunRecord();
        if (r < 0) { MoveFail("0x2F faulted"); return; }
        if (r == 0) { if (++g_move.waits > 1200) MoveFail("0x2F did not finish (the spawns did not come)"); return; }
        Note("difficulty: moving tiles: the stamp terrains of %u locations spawned for the tile (%.1f s)", g_move.stCount, (double)(GetTickCount64() - g_move.started) / 1000.0);
        if (SafeRead(g_near.records + (uint64_t)g_move.at37 * 0x19B0u, g_move.one, sizeof g_move.one)) { MoveFail("record 0x37 did not read"); return; }
        g_move.phase = 0; g_move.waits = 0; g_move.state = MV_KICK;
        return;
    }
    if (g_move.state == MV_KICK)
    {
        g_move.merge = 1; /* from here the merge's resources may exist */
        int r = MoveRunRecord();
        if (r < 0) { MoveFail("0x37 faulted"); return; }
        if (r == 0) { if (++g_move.waits > 60) MoveFail("0x37 did not finish"); return; }
        if (SafeRead(g_near.records + (uint64_t)g_move.at38 * 0x19B0u, g_move.one, sizeof g_move.one)) { MoveFail("record 0x38 did not read"); return; }
        /* the merge in one tile, as the level's own 0x38 ran it (WorldDispatch sets the record's T, pass+0xA8, to 1 for
         * the step: the records of an extra large level give 2); this copy goes to the dispatcher directly */
        { uint32_t t = 0; memcpy(&t, g_move.one + 8 + 0xA8, 4); if (t > 1 && t <= 16) { t = 1; memcpy(g_move.one + 8 + 0xA8, &t, 4); } }
        g_move.phase = 0; g_move.waits = 0; g_move.state = MV_SETUP;
        return;
    }
    if (g_move.state == MV_SETUP)
    {
        int r = MoveRunRecord();
        if (r < 0) { MoveFail("0x38 faulted"); return; }
        if (r == 0) { if (++g_move.waits > 1200) MoveFail("0x38 did not finish (the ring did not drain)"); return; }
        /* 0x4C (case 0xC16B85): phase 0 creates R+0xB8 (4096 x 1, format 0x2A, [api+0x10]+0x170) and publishes it as
         * 0x2FFE957D, phase 1 fills it (0xF4F4F0). 0x67 released the level's; 0x37 re-creates only the other three
         * (0xA2003184, 0x1BB41B90, 0xEBD2B190, 0xFCEB80), and the replay's 0x64 binds it (run s72h: without it 0x64's job
         * crashed a worker at exe+0x396CCD, an id with no resource) */
        if (SafeRead(g_near.records + (uint64_t)g_move.at4C * 0x19B0u, g_move.one, sizeof g_move.one)) { MoveFail("record 0x4C did not read"); return; }
        g_move.phase = 0; g_move.waits = 0; g_move.state = MV_LOOKUP;
        return;
    }
    if (g_move.state == MV_LOOKUP)
    {
        int r = MoveRunRecord();
        if (r < 0) { MoveFail("0x4C faulted"); return; }
        if (r == 0) { if (++g_move.waits > 1200) MoveFail("0x4C did not finish"); return; }
        if (!MoveReplayBegin()) { MoveFail("the replay could not start"); return; }
        Note("difficulty: moving tiles: the merge is re-created (%.1f s); replaying %d steps for the tile", (double)(GetTickCount64() - g_move.started) / 1000.0, g_near.steps);
        g_move.state = MV_REPLAY;
        return;
    }
    if (g_move.state == MV_REPLAY)
    {
        TerrainNearReplay(g_near.records, g_near.end, g_near.at65, (uint64_t)(uintptr_t)g_move.ctx, 0, 0);
        if (g_near.state == NEAR_FAILED) { MoveFail("the replay failed"); return; }
        if (g_near.state != NEAR_SPAWN) return;
        MoveHold(0); /* NearReplayEnd unpublished the tile's textures */
        g_move.waits = 0; g_move.state = MV_TEARDOWN;
        return;
    }
    if (g_move.state == MV_TEARDOWN)
    {
        if (NearQueued((uint64_t)(uintptr_t)g_move.ctx) != 0 || ++g_move.waits < 3) { if (g_move.waits > 600) MoveFail("the job queue did not empty"); return; }
        if (g_move.stamped)
        {
            if (!MoveStampsRecord(g_near.at65, 0x65)) { MoveFail("record 0x65 did not read"); return; }
            g_move.waits = 0; g_move.state = MV_UNSTAMP;
            return;
        }
        MoveTeardown();
        memcpy(g_near.one69, g_near.keep69, sizeof g_near.one69); memcpy(g_near.one6B, g_near.keep6B, sizeof g_near.one6B);
        g_move.state = MV_SPAWN;
        return;
    }
    if (g_move.state == MV_FAILING)
    {
        if (g_move.stamped && DestroyListCount(MoveWorld()) >= MOVE_DESTROY_ROOM) { if (++g_move.waits > 600) { g_move.stamped = 0; g_move.ringWaits = 0; Note("difficulty: moving tiles: the destroy list did not empty; the stamp terrains were not all taken away"); } return; }
        if (g_move.stamped)
        {
            int r = MoveRunRecord();
            if (r == 0 && ++g_move.waits <= 600) return;
            if (r != 1) Note("difficulty: moving tiles: the stamp terrains were not all taken away (0x65: %d)", r);
            g_move.stamped = 0; g_move.ringWaits = 0;
        }
        if (!MoveStampUnitsReady()) return;
        MoveFailRest();
        return;
    }
    if (g_move.state == MV_UNSTAMP)
    {
        if (g_move.stamped)
        {
            if (DestroyListCount(MoveWorld()) >= MOVE_DESTROY_ROOM) { if (++g_move.waits > 600) { g_move.stamped = 0; MoveFail("the destroy list did not empty"); } return; }
            int r = MoveRunRecord();
            if (r < 0) { g_move.stamped = 0; MoveFail("0x65 faulted"); return; }
            if (r == 0) { if (++g_move.waits > 600) { g_move.stamped = 0; MoveFail("0x65 did not finish"); } return; }
            g_move.stamped = 0; g_move.ringWaits = 0;
        }
        if (!MoveStampUnitsReady()) return;
        MoveStampUnitsDestroy();
        MoveTeardown();
        memcpy(g_near.one69, g_near.keep69, sizeof g_near.one69); memcpy(g_near.one6B, g_near.keep6B, sizeof g_near.one6B);
        g_move.state = MV_SPAWN;
        return;
    }
    if (g_move.state == MV_SPAWN)
    {
        if (g_matband.tex != 0 && (g_matband.holds < NEAR_MAT_HOLD || GetTickCount64() - g_matband.made < 100)) { g_matband.holds++; return; }
        if (!NearSpawnTile((uint64_t)(uintptr_t)g_move.ctx)) { MoveFail("the spawn failed"); return; }
        g_move.state = MV_READY;
        return;
    }
    if (g_move.state == MV_READY)
    {
        uint64_t world = 0;
        if (NearReadyTile((uint64_t)(uintptr_t)g_move.ctx, &world)) return;
        if (g_near.state == NEAR_FAILED) { MoveFail("the tile was not ready"); return; }
        MoveTileIn(world);
        g_move.state = MV_IDLE;
    }
}

/* What a build costs the game thread: MoveTick's time in each frame of a build, the six longest with the build's state,
 * the replay's step and the conform's phase at the frame's start; noted once the build is over (the frame after the tile
 * is in, or after a failure). */
static struct { double ms[6]; int mv[6], ns[6], step[6], conf[6], bld[6]; double sum; uint32_t frames; int pending; } g_moveCost;

static void MoveCostAdd(double ms, int mv, int ns, int step, int conf, int bld)
{
    g_moveCost.sum += ms; g_moveCost.frames++;
    int at = 6;
    while (at > 0 && ms > g_moveCost.ms[at - 1]) at--;
    if (at >= 6) return;
    for (int k = 5; k > at; k--)
    { g_moveCost.ms[k] = g_moveCost.ms[k - 1]; g_moveCost.mv[k] = g_moveCost.mv[k - 1]; g_moveCost.ns[k] = g_moveCost.ns[k - 1]; g_moveCost.step[k] = g_moveCost.step[k - 1]; g_moveCost.conf[k] = g_moveCost.conf[k - 1]; g_moveCost.bld[k] = g_moveCost.bld[k - 1]; }
    g_moveCost.ms[at] = ms; g_moveCost.mv[at] = mv; g_moveCost.ns[at] = ns; g_moveCost.step[at] = step; g_moveCost.conf[at] = conf; g_moveCost.bld[at] = bld;
}

static void MoveCostNote(void)
{
    static const char* const mv[] = {"idle", "package", "stamps", "kick", "setup", "lookup", "replay", "teardown", "unstamp", "spawn", "ready", "failing"};
    char line[640]; int n = 0;
    for (int k = 0; k < 6 && g_moveCost.ms[k] > 0 && n < 600; k++)
        n += snprintf(line + n, sizeof line - n, " %.0f ms (%s, near state %d, step %d, building %d, conform phase %d);", g_moveCost.ms[k],
            g_moveCost.mv[k] >= 0 && g_moveCost.mv[k] <= MV_FAILING ? mv[g_moveCost.mv[k]] : "?", g_moveCost.ns[k], g_moveCost.step[k], g_moveCost.bld[k], g_moveCost.conf[k]);
    Note("difficulty: moving tiles: the build's cost on the game thread: %.0f ms over %u frames; the longest:%s", g_moveCost.sum, g_moveCost.frames, line);
    memset(&g_moveCost, 0, sizeof g_moveCost);
}

static void MoveLevelUpdate(uint64_t level)
{
    g_moveUpdate(level);
    int mv = g_move.state, ns = g_near.state, step = g_near.step, conf = g_conform.phase, bld = g_near.building;
    LARGE_INTEGER t0, t1, hz;
    QueryPerformanceCounter(&t0);
    MoveTick(level);
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&hz);
    if (mv != MV_IDLE || g_move.state != MV_IDLE) { MoveCostAdd((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart, mv, ns, step, conf, bld); g_moveCost.pending = 1; }
    else if (g_moveCost.pending) MoveCostNote();
}

static void MoveInstall(uintptr_t game)
{
    if (g_moveInstalled || !MoveOn()) return;
    if (!ApiCodeMatches(game + MOVE_LEVEL_UPDATE, kMoveUpdatePin) || !ApiCodeMatches(game + MOVE_CRATER_UPDATE, kMoveCraterUpdatePin) ||
        !ApiCodeMatches(game + MOVE_CRATER_RENDER, kMoveCraterRenderPin))
    { Note("difficulty: moving tiles: the level update or the terrain deformation is not the reviewed code; the detailed terrain stays at the drop point"); return; }
    if (LoaderHook("extra large maps' crater hold (update)", game + MOVE_CRATER_UPDATE, (void*)MoveCraterUpdate, (void**)&g_moveCraterUpdate) ||
        LoaderHook("extra large maps' crater hold (render)", game + MOVE_CRATER_RENDER, (void*)MoveCraterRender, (void**)&g_moveCraterRender) ||
        LoaderHook("extra large maps' moving tiles", game + MOVE_LEVEL_UPDATE, (void*)MoveLevelUpdate, (void**)&g_moveUpdate))
    { Note("difficulty: moving tiles: a hook did not go in; the detailed terrain stays at the drop point"); return; }
    g_moveInstalled = 1;
    Note("difficulty: moving tiles: installed (level update 0x%X, crater hold 0x%X / 0x%X)", MOVE_LEVEL_UPDATE, MOVE_CRATER_UPDATE, MOVE_CRATER_RENDER);
}
