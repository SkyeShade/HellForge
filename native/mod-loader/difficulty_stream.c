/* Extra large maps' content streaming, v1: the world of difficulty_world.c grows, but the map's content is spawned near
 * the players while they play instead of all at generation.
 *
 * The generator's SpawnLevelLayers (op 0x6C, game.dll + 0xF46940) walks the location records (0xE0 bytes each) from
 * *(uint8_t**)(pass + 0x08), count **(uint32_t**)(pass + 0x48), starting at the resume index *(uint32_t*)(pass + 0xD8).
 * A location whose stamp unit at +0xA8 is non-null has its level layers queued on the engine world's asynchronous spawn
 * queue; a location at +0xA8 == 0 is skipped by the original. SpawnStamps (op 0x31) already stored each location's stamp
 * unit there.
 *
 * This file hooks SpawnLevelLayers. On the generator's first call for an extra large level (g_world.current > 1.0f) it
 * copies each location's stamp unit into its own array and zeroes +0xA8 for every location outside the start area (the
 * squad's drop point on the war board, or the map centre when none reads), so the generator loads only that area and
 * skips the rest. When a generator call returns with the resume index at the count (the op is done) the units are put
 * back. The game thread then streams a deferred location when a player's Helldiver comes within 500 m: it copies up to
 * 16 location records into a staging array, points the captured pass at that array (resume 0, count 16), calls the
 * original, restores the pass fields and copies the records back. Locations before the returned resume index are done;
 * the rest stay due for the next frame (the engine's queue was full).
 *
 * Claude's in-game test of 2026-10-30 made the same call with one location record and the content appeared within a
 * frame or two, physics bodies included. It only works with difficulty_world.c's kept layer heightfield (WorldLayerSnap):
 * the generator frees its heightfield after generation and a layer spawned later would crash without the kept copy.
 *
 * The pin below is SpawnLevelLayers' first 23 bytes, checked against .deepcode/xl2/spawn_level_layers.asm (the task's
 * pin had 48 8D AB there; the reviewed bytes are 49 8D AB: lea rbp, [r11 - 0x1c8] needs REX.WB):
 *   0xF46940: 4C 8B DC 55 53 41 56 41 57 49 8D AB 38 FE FF FF 48 81 EC A8 02 00 00
 * A streamed location whose nearest player is farther than kStreamOutRadius (800 m) for 5 s is removed: DestroyUnit
 * (the world interface's +0x388 entry) destroys its stamp unit, the real record's +0xA8 goes to 0 and the location waits
 * for a new unit. When it becomes due again SpawnUnit (+0x380) makes a new stamp unit from the resource and pose saved at
 * capture, SpawnStamps' set-data call puts the location index on it, and its layers stream as before. Only locations this
 * streamer spawned are removed; the start area's content and the generator's own spawns stay. The layer units go, but the
 * game entities some layers queue stay (logged at the 20th removal; a later task).
 *
 * The mission's own locations (every important location's stamp: objectives, extraction, outposts, points of interest,
 * satellites) are never deferred: the game links their entities by the stamp unit and registers objectives and
 * garrisons at level setup (WorldStreamMissionLocked). Only the rest (stamps, formations) streams. */

#ifndef HF_LUA_API
#define HF_LUA_API __declspec(dllexport)
#endif

#define DIF_STREAM_SPAWN 0xF46940u /* uint8_t SpawnLevelLayers(uint8_t* pass, uint8_t* record, uint8_t* context) */
static const char kStreamSpawnPin[] = "4C 8B DC 55 53 41 56 41 57 49 8D AB 38 FE FF FF 48 81 EC A8 02 00 00";
/* The galactic war board's drop point (Claude's in-game test, 2026-10-30; asked for 800,0, landed at 789,37): the board
 * object at [game + 0x347CEE8], a u8 at +0x17A2B0 = 1 when a drop point is set, and its world-space metres at
 * +0x17A2B4 (x) and +0x17A2B8 (y). */
#define DIF_STREAM_BOARD 0x347CEE8u
#define DIF_STREAM_BOARD_FLAG 0x17A2B0u
#define DIF_STREAM_BOARD_X 0x17A2B4u
#define DIF_STREAM_BOARD_Y 0x17A2B8u
/* The world interface ([game + 0x3326308]): the table at +8 holds SpawnUnit (+0x380) and DestroyUnit (+0x388); the
 * object at +0xB8 has the method at its +0x10 object's +8 (SpawnStamps calls it after spawning to store the location
 * index on the unit). A stamp unit's pose is its 4x4 float matrix at +0x20; a location's resource is [[loc+0x90]+8]. */
#define DIF_STREAM_INTERFACE 0x3326308u
#define DIF_STREAM_TABLE 0x8u
#define DIF_STREAM_SPAWN_ENTRY 0x380u
#define DIF_STREAM_DESTROY_ENTRY 0x388u
/* The table's +0x410 (exe 0x3340A0: mov rcx,[rcx+0x230050]; jmp 0x556040) waits for the world's asynchronous animation
 * update. The animation state machine manager (world+0x230050, built by 0x554870) starts its async jobs (0x555BB0, kicked
 * from game 0x127A38D through the table's +0x400) over a copy of its list (+0xE0) and does not wait; every game destroy
 * waits first (+0x410 before +0x388 at game 0x664A2B, 0x6652AE, 0xAB3A31, 0xAC04DE, 0x1304659), and the frame's end waits
 * at 0xAB61AD. A unit destroyed between the kick and that wait leaves its state machine in the jobs' copy: the update
 * 0x5530C0 reads [[asm]+0x30] of the freed unit (run s75q: exe+0x55310E, reading -1; static, subagent crash55310e).
 * The streamer's tick also runs from the late update (0x13270B0 called at 0x13F8938, after the kick), so every
 * removal waits first; the wait returns at once when no job is in flight (+0xF0 == 0). */
#define DIF_STREAM_ANIM_WAIT_ENTRY 0x410u
static const char kStreamAnimWaitPin[] = "48 8B 89 50 00 23 00 E9";
typedef void (*WorldStreamAnimWaitFn)(uint64_t world);
/* The wait entry from the world interface's table when it is the reviewed one (exe 0x3340A0's bytes), else 0. */
static uintptr_t WorldStreamAnimWait(uintptr_t table)
{
    uintptr_t wait = 0, exe = ApiModuleBase("helldivers2.exe", NULL);
    if (table == 0 || exe == 0 || SafeRead(table + DIF_STREAM_ANIM_WAIT_ENTRY, &wait, 8) || wait != exe + 0x3340A0u ||
        ApiCodeMatches(wait, kStreamAnimWaitPin) != 1) return 0;
    return wait;
}
#define DIF_STREAM_SETDATA_ENTRY 0xB8u
#define DIF_STREAM_SETDATA_OBJECT 0x10u
#define DIF_STREAM_SETDATA_METHOD 0x8u
#define DIF_STREAM_UNIT_POSE 0x20u
#define DIF_STREAM_LOC_RESOURCE 0x90u

enum
{
    kStreamMaxLocations = 65536, /* the location records this streamer holds; above that nothing is deferred */
    kStreamBatch = 16,           /* location records per SpawnLevelLayers call */
    kStreamRecord = 0xE0,        /* one location record */
    kStreamScanChunk = 64,       /* location records read per SafeRead while deciding */
    kStreamOutTimeout = 5000,    /* ms beyond kStreamOutRadius before a streamed location is removed */
    kStreamRemoveBatch = 8       /* removals per removal pass (one frame) at most */
};
static const float kStreamStartRadius = 600.0f; /* kept at generation: inside this of the start point */
static const float kStreamInRadius = 500.0f;    /* streamed: a player closer than this makes a location due */
static const float kStreamOutRadius = 800.0f;   /* removed: every player farther than this for kStreamOutTimeout */
static const float kStreamFar = 3.402823466e38f; /* no player: beyond any distance in a map */

/* The start area's centre, in world space (map position - half). WorldStreamCaptureLocked sets it from the war board's
 * drop point when that reads, else to the map centre (0, 0). */
static float g_streamStart[2] = {0.0f, 0.0f};

/* Research: the Lua export's extra player position. WorldStreamPlayersLocked counts it as one more player, and it is
 * set and cleared under g_streamLock. */
static int g_streamObserverOn;
static float g_streamObserverAt[2] = {0.0f, 0.0f};

typedef uint8_t (*WorldStreamSpawnFn)(uint8_t* pass, uint8_t* record, uint8_t* context);
static WorldStreamSpawnFn g_streamSpawn;
/* The engine calls SpawnStamps makes for a stamp unit: SpawnUnit (the world interface's +0x380) and the set-data call
 * after it (the +0xB8 object's +0x10 object, method at +8), whose 24-byte argument is {&location index, 2, 4, 0}.
 * DestroyUnit is the table's +0x388 entry. */
typedef uint64_t (*WorldStreamSpawnUnitFn)(uint64_t world, uint64_t resource, const float* pose, uint64_t zero, int zero2);
typedef void (*WorldStreamDestroyUnitFn)(uint64_t world, uint64_t unit, uint32_t flags, uint8_t extra);
typedef void (*WorldStreamSetDataFn)(uint64_t unit, void* data, uint64_t one, uint64_t hash);
typedef struct WorldStreamSetData
{
    uint32_t* index;
    uint32_t two, four;
    uint64_t zero;
} WorldStreamSetData;

/* The captured generator call and what was decided for its locations. All of it is read and written on one side of
 * g_streamLock: the generator thread captures and restores, the game thread streams. */
typedef struct WorldStreamState
{
    uintptr_t game;          /* game.dll */
    uintptr_t level;         /* g_worldLevel the state belongs to */
    uint8_t* pass;           /* the generator's SpawnLevelLayers arguments (valid for the mission) */
    uint8_t* record;
    uint8_t* context;
    uint8_t* array;          /* *(pass + 0x08): the location records */
    uintptr_t countAt;       /* *(pass + 0x48): the count */
    float half;              /* g_world.values->half when captured (map space -> world space) */
    uint32_t count, kept, deferred, bad;  /* locations, kept at the start, deferred, unreadable at capture */
    uint32_t bodiesUsed, bodySlots;       /* the mission world's physics bodies at the last scan, and its store */
    uint32_t spawned, broken, removed, respawned; /* streamed now, records skipped, removals and respawns (cumulative) */
    uint32_t orderCount, orderCursor;     /* the due locations, nearest first */
    int captured, failed, generatorDone;  /* state, the capture was refused, the generator's op is at its count */
    int playerCount;
    int resumeBad, passBad, recordBad, heightBad, destroyBad, respawnBad; /* one note per failure */
    uint32_t bodyBefore;                  /* the physics bodies before the first removal (bodyBeforeSeen) */
    int bodyBeforeSeen, bodyPending, bodyNoted; /* before count read; after count due; the 20-removal line is done */
    ULONGLONG nextScan, nextNote;
    double spentMs;          /* time in the streamer's own SpawnLevelLayers calls */
} WorldStreamState;
static WorldStreamState g_stream;

static uint64_t g_streamUnit[kStreamMaxLocations];       /* each location's real stamp unit */
static float g_streamAt[kStreamMaxLocations][2];         /* its world-space x, y */
static uint8_t g_streamDeferred[kStreamMaxLocations];    /* 1: its +0xA8 was zeroed for the generator */
static uint8_t g_streamDue[kStreamMaxLocations];         /* 1: a player has been within kStreamInRadius (sticky) */
static uint8_t g_streamSpawned[kStreamMaxLocations];     /* 1: queued, or skipped because its record did not read */
static float g_streamDistance[kStreamMaxLocations];      /* its squared distance to the nearest player, for the order */
static uint32_t g_streamOrder[kStreamMaxLocations];      /* the due locations, nearest first */
static uint64_t g_streamChunk[(kStreamScanChunk * kStreamRecord) / 8];   /* records read while deciding */
static uint64_t g_streamStage[(kStreamBatch * kStreamRecord) / 8];       /* records of one streamed call */
static float g_streamPlayerAt[EVT_MAX_PLAYERS + 1][2];   /* the players' Helldivers' x, y; the last slot can be the observer */
static uint64_t g_streamResource[kStreamMaxLocations];   /* [[loc+0x90]+8] at capture */
static float g_streamPose[kStreamMaxLocations][16];      /* its stamp unit's pose (unit+0x20) at capture */
static uint8_t g_streamPoseGood[kStreamMaxLocations];    /* 1: resource and pose saved, so it may be removed */
static uint8_t g_streamLive[kStreamMaxLocations];        /* 1: the streamer queued its layers and they are live */
static uint8_t g_streamDetached[kStreamMaxLocations];    /* 1: the real record's +0xA8 stays 0 (a respawned unit) */
static uint8_t g_streamNeedsUnit[kStreamMaxLocations];   /* 1: removed; a new stamp unit is needed before layers */
static uint8_t g_streamFormation[kStreamMaxLocations];   /* 1: a large formation of the outer stamp rings: streamed in
                                                            under the physics budget wherever the players are, kept */
static ULONGLONG g_streamOutSince[kStreamMaxLocations];  /* tick it first went beyond kStreamOutRadius (0: not out) */
static uint8_t g_streamMission[kStreamMaxLocations];     /* 1: an important location's stamp (WorldStreamMissionLocked) */
static float g_streamOut2[kStreamMaxLocations];          /* > 0: this slot's own removal radius, squared (a far window stamp) */

static SRWLOCK g_streamLock = SRWLOCK_INIT;
static volatile LONG g_streamOwn;     /* the streamer's own call: the thread id inside it (0: none) */
static volatile LONG g_streamInside;  /* a generator call is inside (capture, the original, restore): no streaming then */
static int g_streamInstallState;      /* 0 not tried, 1 installed, -1 refused */
static LONGLONG g_streamFrequency;    /* QueryPerformanceFrequency */

/* Back to no captured level. The arrays are cleared too: a capture fills only the indices below its count. */
static void WorldStreamClearLocked(void)
{
    g_stream.level = 0;
    g_stream.pass = NULL;
    g_stream.record = NULL;
    g_stream.context = NULL;
    g_stream.array = NULL;
    g_stream.countAt = 0;
    g_stream.half = 0.0f;
    g_stream.count = 0;
    g_stream.kept = 0;
    g_stream.deferred = 0;
    g_stream.bad = 0;
    g_stream.spawned = 0;
    g_stream.broken = 0;
    g_stream.removed = 0;
    g_stream.respawned = 0;
    g_stream.orderCount = 0;
    g_stream.orderCursor = 0;
    g_stream.captured = 0;
    g_stream.failed = 0;
    g_stream.generatorDone = 0;
    g_stream.playerCount = 0;
    g_stream.resumeBad = 0;
    g_stream.passBad = 0;
    g_stream.recordBad = 0;
    g_stream.heightBad = 0;
    g_stream.destroyBad = 0;
    g_stream.respawnBad = 0;
    g_stream.bodyBefore = 0;
    g_stream.bodyBeforeSeen = 0;
    g_stream.bodyPending = 0;
    g_stream.bodyNoted = 0;
    g_stream.nextScan = 0;
    g_stream.nextNote = 0;
    g_stream.spentMs = 0.0;
    memset(g_streamDeferred, 0, sizeof g_streamDeferred);
    memset(g_streamFormation, 0, sizeof g_streamFormation);
    memset(g_streamOut2, 0, sizeof g_streamOut2);
    memset(g_streamDue, 0, sizeof g_streamDue);
    memset(g_streamSpawned, 0, sizeof g_streamSpawned);
    memset(g_streamPoseGood, 0, sizeof g_streamPoseGood);
    memset(g_streamLive, 0, sizeof g_streamLive);
    memset(g_streamDetached, 0, sizeof g_streamDetached);
    memset(g_streamNeedsUnit, 0, sizeof g_streamNeedsUnit);
    memset(g_streamOutSince, 0, sizeof g_streamOutSince);
}

/* Whether the whole range is committed and writable: the generator's records have their +0xA8 zeroed in place. */
static int WorldStreamWritable(uint8_t* at, size_t bytes)
{
    uintptr_t p = (uintptr_t)at, end = p + bytes;
    while (p < end)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0) return 0;
        DWORD protect = mbi.Protect & 0xFF;
        if (protect != PAGE_READWRITE && protect != PAGE_WRITECOPY && protect != PAGE_EXECUTE_READWRITE && protect != PAGE_EXECUTE_WRITECOPY)
            return 0;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= p) return 0;
        p = next;
    }
    return 1;
}

static void WorldStreamSift(uint32_t root, uint32_t count)
{
    for (;;)
    {
        uint32_t child = root * 2 + 1;
        if (child >= count) return;
        if (child + 1 < count && g_streamDistance[g_streamOrder[child + 1]] > g_streamDistance[g_streamOrder[child]]) child++;
        if (g_streamDistance[g_streamOrder[root]] >= g_streamDistance[g_streamOrder[child]]) return;
        uint32_t swap = g_streamOrder[root];
        g_streamOrder[root] = g_streamOrder[child];
        g_streamOrder[child] = swap;
        root = child;
    }
}

/* The due locations by squared distance, ascending (a max-heap, then its largest moved to the end). */
static void WorldStreamSort(uint32_t count)
{
    for (uint32_t start = count / 2; start > 0; start--) WorldStreamSift(start - 1, count);
    for (uint32_t end = count; end > 1; end--)
    {
        uint32_t swap = g_streamOrder[0];
        g_streamOrder[0] = g_streamOrder[end - 1];
        g_streamOrder[end - 1] = swap;
        WorldStreamSift(0, end - 1);
    }
}

/* Every player's Helldiver's world position: EvpPlayers' avatars, their health records' units, the units' scene graph
 * roots. Players without a Helldiver or a readable position are skipped. The research observer, when on, is one more
 * position and is counted like a player. */
static void NearReportPlayer(const float* at);
static void WindowLeaveLocked(uint32_t s);   /* difficulty_stream_window.c */
static void WindowScanLocked(ULONGLONG now);
static void WindowNoteLocked(void);
static void DestroyTick(void);                /* difficulty_world_destroy.c */
static void DestroyNote(void);
static const char* InstallDestroyQueue(void);
static void WorldStreamPlayersLocked(void)
{
    g_stream.playerCount = 0;
    if (g_stream.game != 0)
    {
        EvpPlayer players[EVT_MAX_PLAYERS];
        int count = EvpPlayers(g_stream.game, players);
        for (int i = 0; count > 0 && i < count && g_stream.playerCount < EVT_MAX_PLAYERS; i++)
        {
            if (players[i].avatar == 0) continue;
            ActHealth h;
            float at[3] = {0.0f, 0.0f, 0.0f};
            if (!ActHealthOf(g_stream.game, players[i].avatar, &h) || h.unit == 0 || !EvpUnitPosition(h.unit, at)) continue;
            g_streamPlayerAt[g_stream.playerCount][0] = at[0];
            g_streamPlayerAt[g_stream.playerCount][1] = at[1];
            if (g_stream.playerCount == 0) NearReportPlayer(at);
            g_stream.playerCount++;
        }
    }
    if (g_streamObserverOn)
    {
        g_streamPlayerAt[g_stream.playerCount][0] = g_streamObserverAt[0];
        g_streamPlayerAt[g_stream.playerCount][1] = g_streamObserverAt[1];
        g_stream.playerCount++;
    }
}

/* The first player's x, y (for the terrain diagnostics); 0 without one. */
static int WorldStreamFirstPlayer(float* xy)
{
    if (g_stream.playerCount <= 0) return 0;
    xy[0] = g_streamPlayerAt[0][0];
    xy[1] = g_streamPlayerAt[0][1];
    return 1;
}

/* The mission's physics bodies in use, as difficulty_diag.c's NoteMissionMap reads them. */
static int WorldStreamBodies(uint32_t* bodies)
{
    uintptr_t exe = ApiModuleBase("helldivers2.exe", NULL);
    uint64_t physics = 0;
    if (exe == 0 || SafeRead(exe + 0x27BA890u + 0xB0u * 2u, &physics, 8) || physics == 0 ||
        SafeRead((uintptr_t)physics + 0x4C, bodies, 4)) return 0;
    return 1;
}

/* The mission world's physics bodies in use and the body store's size (Havok's body manager at world + 0x38: bodies
 * [+0x38], slot count [+0x40] compared by the allocator 0xD456B0, free list [+0x48]; in use [+0x4C], 0x814F00's lock
 * xadd). The far formations stream in only while the bodies in use stay under kStreamFormationShare of the store: a
 * formation's trees each take a lightweight body (0x814F00) and its rocks their actors, and a full store makes the
 * allocator's "none" (0xFFFFFF) a body that the navmesh cutter reads (exe 0xCB304C; run s70i spawned 3069 at load). */
static const float kStreamFormationShare = 0.40f;
static const uint32_t kStreamFormationsPerScan = 16;
static int WorldStreamBodyStore(uint32_t* used, uint32_t* slots)
{
    uintptr_t exe = ApiModuleBase("helldivers2.exe", NULL);
    uint64_t physics = 0;
    if (exe == 0 || SafeRead(exe + 0x27BA890u + 0xB0u * 2u, &physics, 8) || physics == 0 ||
        SafeRead((uintptr_t)physics + 0x4C, used, 4) || SafeRead((uintptr_t)physics + 0x40, slots, 4)) return 0;
    *slots &= 0x3FFFFFFFu;
    return *slots >= 1024 && *slots <= (1u << 24) && *used <= *slots;
}

/* Remove one streamed location: DestroyUnit (the table's +0x388) takes its stamp unit and the layer units go; the real
 * record's +0xA8 is zeroed and the location waits for a new stamp unit. Only a location this streamer queued
 * (g_streamLive) is ever passed here. */
static int WorldStreamRemoveLocked(uint32_t i)
{
    if (g_stream.destroyBad || g_streamInside) return 0;
    uint64_t world = 0;
    uintptr_t wi = 0, table = 0, destroy = 0;
    if (SafeRead((uintptr_t)g_stream.context + 0x40, &world, 8) || world == 0 ||
        SafeRead(g_stream.game + DIF_STREAM_INTERFACE, &wi, 8) || wi == 0 ||
        SafeRead(wi + DIF_STREAM_TABLE, &table, 8) || table == 0 ||
        SafeRead(table + DIF_STREAM_DESTROY_ENTRY, &destroy, 8) || destroy == 0)
    {
        g_stream.destroyBad = 1;
        Note("difficulty: extra large maps: content streaming: removal off: the world interface's +0x388 entry does not read");
        return 0;
    }
    uintptr_t wait = WorldStreamAnimWait(table);
    if (wait == 0)
    {
        g_stream.destroyBad = 1;
        Note("difficulty: extra large maps: content streaming: removal off: the world interface's +0x410 entry (the animation wait) is not the reviewed one");
        return 0;
    }
    if (g_stream.removed == 0) g_stream.bodyBeforeSeen = WorldStreamBodies(&g_stream.bodyBefore);
    uint64_t unit = g_streamUnit[i];
    ((WorldStreamAnimWaitFn)wait)(world);
    ((WorldStreamDestroyUnitFn)destroy)(world, unit, 0, 0);
    uint64_t zero = 0;
    memcpy((void*)((uintptr_t)g_stream.array + (size_t)i * kStreamRecord + 0xA8), &zero, 8);
    g_streamUnit[i] = 0;
    g_streamDeferred[i] = 1;
    g_streamSpawned[i] = 0;
    g_streamLive[i] = 0;
    g_streamDetached[i] = 1;
    g_streamDue[i] = 0;
    g_streamOutSince[i] = 0;
    g_streamNeedsUnit[i] = 1;
    if (g_stream.spawned > 0) g_stream.spawned--;
    g_stream.removed++;
    if (!g_stream.bodyNoted && g_stream.removed >= 20) g_stream.bodyPending = 1; /* read after the next scan (a few frames on) */
    WindowLeaveLocked(i); /* a window slot: out of the index and free (difficulty_stream_window.c) */
    return 1;
}

/* Spawn a new stamp unit for a removed location: SpawnUnit (table +0x380) with the saved resource and pose, then
 * SpawnStamps' set-data call (the +0xB8 object's +0x10 object, method +8) with {&location index, 2, 4, 0}. The new unit
 * stays in g_streamUnit; the real record's +0xA8 stays 0. 0 when a call cannot be made (the location stays due). */
static int WorldStreamRespawnLocked(uint32_t i)
{
    if (g_streamInside || !g_streamPoseGood[i] || g_streamResource[i] == 0) return 0;
    uint64_t world = 0;
    uintptr_t wi = 0, table = 0, spawn = 0, holder = 0, object = 0, method = 0;
    if (SafeRead((uintptr_t)g_stream.context + 0x40, &world, 8) || world == 0 ||
        SafeRead(g_stream.game + DIF_STREAM_INTERFACE, &wi, 8) || wi == 0 ||
        SafeRead(wi + DIF_STREAM_TABLE, &table, 8) || table == 0 ||
        SafeRead(table + DIF_STREAM_SPAWN_ENTRY, &spawn, 8) || spawn == 0 ||
        SafeRead(wi + DIF_STREAM_SETDATA_ENTRY, &holder, 8) || holder == 0 ||
        SafeRead(holder + DIF_STREAM_SETDATA_OBJECT, &object, 8) || object == 0 ||
        SafeRead(object + DIF_STREAM_SETDATA_METHOD, &method, 8) || method == 0)
    {
        if (!g_stream.respawnBad)
        {
            g_stream.respawnBad = 1;
            Note("difficulty: extra large maps: content streaming: a removed location's new stamp unit cannot be made (the world interface's +0x380 entry or the set-data call does not read)");
        }
        return 0;
    }
    uint64_t unit = ((WorldStreamSpawnUnitFn)spawn)(world, g_streamResource[i], g_streamPose[i], 0, 0);
    if (unit == 0)
    {
        if (!g_stream.respawnBad)
        {
            g_stream.respawnBad = 1;
            Note("difficulty: extra large maps: content streaming: SpawnUnit returned no stamp unit for a removed location");
        }
        return 0;
    }
    g_streamUnit[i] = unit;
    g_streamDetached[i] = 1;
    uint32_t index = i;
    WorldStreamSetData data = {&index, 2, 4, 0};
    ((WorldStreamSetDataFn)method)(unit, &data, 1, 0x39D268A5u);
    g_streamNeedsUnit[i] = 0;
    g_stream.respawned++;
    return 1;
}

/* The mission's own stamps: every stamp record an important location points to (G+0x8996B0, 0x108 each, count
 * G+0x11A715C; loc+0x98 = &stamp[i], op 0x16 at 0xF90275..0xF90280): objectives, extraction, outposts, points of
 * interest, satellites. The game ties their entities to the location by the stamp unit alone ([loc+0x98]+0xA8 against
 * the entity's spawning unit: Objective init 0x5D1067/0x5D122B, ExtractionZone 0x8D1216, ShowOnMap 0x677D85,
 * GuardForce 0x90C459), builds GuardForce once at level setup (0xAD0010 -> 0xAD1FD5) from what exists then, and unlocks
 * extraction for good once no live main objective is pending (0x5D91B0, Mission +0xAD8 at 0x8F53FF, never cleared).
 * So these spawn with the level like vanilla's and are never removed: a deferred main objective meant extraction at
 * landing, and a removed one came back as a fresh instance with no location. Returns how many were marked. */
static uint32_t WorldStreamMissionLocked(const uint8_t* array, uint32_t count)
{
    memset(g_streamMission, 0, count);
    uintptr_t block = 0;
    uint32_t locations = 0, marked = 0, outside = 0;
    if (g_worldLevel == 0 || SafeRead(g_worldLevel + 0x2C8, &block, 8) || block == 0 || SafeRead(block + 0x11A715Cu, &locations, 4))
    {
        Note("difficulty: extra large maps: content streaming: the important locations do not read; every far location streams");
        return 0;
    }
    if (locations > 256) Note("difficulty: extra large maps: content streaming: %u important locations, more than the game's 256 places", locations);
    if (locations > 1024) locations = 1024;
    const uintptr_t first = (uintptr_t)array, end = first + (uintptr_t)count * kStreamRecord;
    for (uint32_t l = 0; l < locations; l++)
    {
        uint64_t P = 0;
        if (SafeRead(block + g_importantList + 0x108ull * l + 0x98, &P, 8) || P == 0) continue;
        if (P < first || P >= end || (P - first) % kStreamRecord != 0) { outside++; continue; }
        uint32_t i = (uint32_t)((P - first) / kStreamRecord);
        if (!g_streamMission[i]) { g_streamMission[i] = 1; marked++; }
    }
    if (outside) Note("difficulty: extra large maps: content streaming: %u important location(s) point outside the location records", outside);
    return marked;
}

/* The generator's first call for this level: read its records and zero the stamp units outside the start area. The
 * original then spawns the start area and skips the deferred locations; WorldStreamEnd puts the units back when the op
 * reaches its count. `resume` is normally 0; locations before a non-zero one were already done before this call. */
static void WorldStreamCaptureLocked(uint8_t* pass, uint8_t* record, uint8_t* context)
{
    g_stream.level = g_worldLevel;
    uint8_t* array = NULL;
    uint64_t countAt = 0;
    uint32_t count = 0, resume = 0;
    if (SafeRead((uintptr_t)pass + 8, &array, 8) || array == NULL || SafeRead((uintptr_t)pass + 0x48, &countAt, 8) || countAt == 0 ||
        SafeRead((uintptr_t)countAt, &count, 4))
    {
        g_stream.failed = 1;
        Note("difficulty: extra large maps: content streaming off for this level: the generator's pass (0x%llX) does not read",
            (unsigned long long)(uintptr_t)pass);
        return;
    }
    SafeRead((uintptr_t)pass + 0xD8, &resume, 4);
    if (g_world.values == NULL || !(g_world.values->half > 0.0f))
    {
        g_stream.failed = 1;
        Note("difficulty: extra large maps: content streaming off for this level: the world's values are not set");
        return;
    }
    g_stream.pass = pass;
    g_stream.record = record;
    g_stream.context = context;
    g_stream.array = array;
    g_stream.countAt = (uintptr_t)countAt;
    g_stream.half = g_world.values->half;
    g_stream.count = count;
    /* The start area's centre: the squad's drop point on the war board when it reads, else the map centre. */
    {
        uint64_t board = 0;
        uint8_t flag = 0;
        float at[2] = {0.0f, 0.0f};
        int fromBoard = g_stream.game != 0 && !SafeRead(g_stream.game + DIF_STREAM_BOARD, &board, 8) && board != 0 &&
            !SafeRead((uintptr_t)board + DIF_STREAM_BOARD_FLAG, &flag, 1) && flag == 1 &&
            !SafeRead((uintptr_t)board + DIF_STREAM_BOARD_X, &at[0], 4) &&
            !SafeRead((uintptr_t)board + DIF_STREAM_BOARD_Y, &at[1], 4) &&
            isfinite(at[0]) && isfinite(at[1]) && fabsf(at[0]) <= g_stream.half && fabsf(at[1]) <= g_stream.half;
        if (fromBoard) { g_streamStart[0] = at[0]; g_streamStart[1] = at[1]; }
        else { g_streamStart[0] = 0.0f; g_streamStart[1] = 0.0f; }
        Note("difficulty: extra large maps: content streaming: the start area is %s (%.1f, %.1f)",
            fromBoard ? "the squad's drop point from the war board" : "the map centre; no usable drop point on the war board",
            (double)g_streamStart[0], (double)g_streamStart[1]);
    }
    if (count == 0)
    {
        g_stream.captured = 1;
        g_stream.generatorDone = 1;
        Note("difficulty: extra large maps: content streaming: level 0x%llX has no locations", (unsigned long long)g_stream.level);
        return;
    }
    if (count > kStreamMaxLocations)
    {
        g_stream.count = 0;
        g_stream.captured = 1;
        g_stream.generatorDone = 1;
        Note("difficulty: extra large maps: content streaming: level 0x%llX has %u locations (more than %d); nothing is deferred",
            (unsigned long long)g_stream.level, count, (int)kStreamMaxLocations);
        return;
    }
    if (resume >= count)
    {
        g_stream.captured = 1;
        g_stream.generatorDone = 1;
        Note("difficulty: extra large maps: content streaming: level 0x%llX is already past its locations (%u of %u); nothing is deferred",
            (unsigned long long)g_stream.level, resume, count);
        return;
    }
    if (!WorldStreamWritable(array, (size_t)count * kStreamRecord))
    {
        g_stream.count = 0;
        g_stream.captured = 1;
        g_stream.generatorDone = 1;
        Note("difficulty: extra large maps: content streaming: level 0x%llX's %u location records are not writable; nothing is deferred",
            (unsigned long long)g_stream.level, count);
        return;
    }
    uint32_t mission = WorldStreamMissionLocked(array, count);
    uint32_t kept = 0, deferred = 0, bad = 0, formations = 0, missionFar = 0;
    float keep = kStreamStartRadius * kStreamStartRadius;
    float full = 2.0f * g_stream.half;
    for (uint32_t i = 0; i < count;)
    {
        uint32_t n = count - i;
        if (n > kStreamScanChunk) n = kStreamScanChunk;
        if (SafeRead((uintptr_t)array + (size_t)i * kStreamRecord, g_streamChunk, (size_t)n * kStreamRecord))
        {
            bad += n; kept += n; /* the chunk did not read: leave those locations to the generator */
            i += n;
            continue;
        }
        for (uint32_t j = 0; j < n; j++, i++)
        {
            if (i < resume) { kept++; continue; } /* done before this call: no unit to hold back */
            const uint8_t* loc = (const uint8_t*)g_streamChunk + (size_t)j * kStreamRecord;
            float at[2] = {0.0f, 0.0f};
            uint64_t unit = 0;
            memcpy(at, loc, 8);
            memcpy(&unit, loc + 0xA8, 8);
            g_streamUnit[i] = unit;
            uint64_t holder = 0, resource = 0;
            memcpy(&holder, loc + DIF_STREAM_LOC_RESOURCE, 8);
            if (holder != 0) SafeRead((uintptr_t)holder + 8, &resource, 8);
            g_streamResource[i] = resource;
            if (unit != 0 && !SafeRead((uintptr_t)unit + DIF_STREAM_UNIT_POSE, g_streamPose[i], 64))
                g_streamPoseGood[i] = resource != 0; /* no resource: it could not be spawned again */
            else
                g_streamPoseGood[i] = 0;
            if (!isfinite(at[0]) || !isfinite(at[1]) || at[0] < -1.0f || at[1] < -1.0f || at[0] > full + 1.0f || at[1] > full + 1.0f)
            {
                kept++; bad++; /* no distance: spawn it with the generator */
                continue;
            }
            g_streamAt[i][0] = at[0] - g_stream.half;
            g_streamAt[i][1] = at[1] - g_stream.half;
            float dx = g_streamAt[i][0] - g_streamStart[0], dy = g_streamAt[i][1] - g_streamStart[1];
            if (unit == 0 || dx * dx + dy * dy <= keep) { kept++; continue; }
            if (g_streamMission[i]) { kept++; missionFar++; continue; } /* the mission's own: spawned with the level, never removed */
            uint64_t zero = 0;
            memcpy((void*)((uintptr_t)array + (size_t)i * kStreamRecord + 0xA8), &zero, 8);
            g_streamDeferred[i] = 1;
            if (StampsKeepFar(i)) { g_streamFormation[i] = 1; formations++; }
            deferred++;
        }
    }
    g_stream.kept = kept;
    g_stream.deferred = deferred;
    g_stream.bad = bad;
    g_stream.captured = 1;
    if (deferred == 0) g_stream.generatorDone = 1; /* nothing held back: the original's op needs no restore */
    Note("difficulty: extra large maps: content streaming: level 0x%llX: %u locations, %u kept at the start (%u of them the mission's own beyond the start area, of %u), %u deferred (%u of them far formations, streamed under the physics budget)",
        (unsigned long long)g_stream.level, count, kept, missionFar, mission, deferred, formations);
    if (bad) Note("difficulty: extra large maps: content streaming: %u location record(s) did not read; they stay with the generator", bad);
}

/* 1 when this call is the generator's own call of the deferring op (the caller must run WorldStreamEnd after it). */
static int WorldStreamBegin(uint8_t* pass, uint8_t* record, uint8_t* context)
{
    if (pass == NULL || !(g_world.current > 1.0f)) return 0;
    AcquireSRWLockExclusive(&g_streamLock);
    if (g_stream.level != g_worldLevel) WorldStreamClearLocked();
    if (!g_stream.captured && !g_stream.failed) WorldStreamCaptureLocked(pass, record, context);
    int watched = g_stream.captured && !g_stream.generatorDone && g_stream.deferred > 0 && g_stream.pass == pass;
    ReleaseSRWLockExclusive(&g_streamLock);
    return watched;
}

/* The generator's record and context live in the generation block [level+0x2B0] (records = blk, ctx = blk + 0x19B008,
 * 0xC14950 at game 0xF6E49C..0xF6E4C7), which the game frees when the level goes live (0xFBF1CB..0xFBF22C, state 3).
 * Their fields point into G (records-2: record 137's +0x10..+0xD0 are all G+ offsets), which stays, so the streamer
 * keeps its own copies of both from the moment the op is done and calls SpawnLevelLayers with those. A context field
 * that points back into the freed block is logged. */
static uint8_t g_streamRecordCopy[0x19B0];
static uint8_t g_streamContextCopy[0x100];
/* The context's arena ([ctx+0], the generation thread's) is destroyed when the level is done (0xF7412D -> 0x173BB30:
 * its chunks freed, its head zeroed). SpawnLevelLayers allocates from it for every layer whose scale ([r13+0xC]) is not
 * 1 (0xF47041..0xF47053 -> 0xF470C6, 16 bytes; the allocator 0x173B8D0 reads the head at 0x173B936): the cities' scaled
 * buildings. So a streamed city location ended the game (s75e/s75f, c0000005 at game+0x173B936 as the first city
 * stamps came within 500 m of a player), while the open land's unscaled stamps never asked. The copy gets its own arena,
 * laid out as 0x173B700 makes one (as difficulty_world_move.c's MoveArena: arena {head chunk +0, lock +8, owner thread
 * index +0xC (-1: no index call), growth +0x10}; chunk {used +0, data +8, capacity +0x10, next +0x18}); after each of
 * the streamer's calls it is reset as the game resets its own after an op (0x173BAB0, 0xF261FE): chunks the allocator
 * grew are freed, ours is the last and stays. */
#define STREAM_ARENA_BYTES (4u << 20)
static struct { uint64_t head; LONG lock; int32_t owner; uint64_t growth; } g_streamArena;
static struct { uint64_t used, data, capacity, next; } g_streamArenaChunk;
static int g_streamArenaReset = -1; /* 1: 0x173BAB0 is the reviewed reset */
static int WorldStreamArena(void)
{
    if (g_streamArenaChunk.data == 0)
    {
        void* p = VirtualAlloc(NULL, STREAM_ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (p == NULL) return 0;
        g_streamArenaChunk.data = (uint64_t)(uintptr_t)p; g_streamArenaChunk.capacity = STREAM_ARENA_BYTES;
    }
    if (g_streamArenaReset < 0) g_streamArenaReset = ApiCodeMatches(g_stream.game + 0x173BAB0u, "40 53 48 83 EC 20 48 8B 01 48 8B D9 48 85 C0 74") ? 1 : 0;
    if (g_streamArenaReset != 1) return 0;
    g_streamArenaChunk.used = 0; g_streamArenaChunk.next = 0;
    g_streamArena.head = (uint64_t)(uintptr_t)&g_streamArenaChunk; g_streamArena.lock = 0; g_streamArena.owner = -1; g_streamArena.growth = 0x100000;
    return 1;
}
static void WorldStreamArenaReset(void)
{
    typedef void (*ArenaFn)(uint64_t arena);
    if (g_streamArena.head != 0 && g_streamArenaReset == 1) ((ArenaFn)(g_stream.game + 0x173BAB0u))((uint64_t)(uintptr_t)&g_streamArena);
}
static void WorldStreamDetachLocked(void)
{
    uintptr_t ctx = (uintptr_t)g_stream.context, blk = ctx - 0x19B008u, end = ctx + 0xE8;
    if (SafeRead((uintptr_t)g_stream.record, g_streamRecordCopy, sizeof g_streamRecordCopy) || SafeRead(ctx, g_streamContextCopy, 0xE8))
    {
        Note("difficulty: extra large maps: content streaming: the generator's record or context did not read; streaming keeps the originals");
        return;
    }
    char inside[160]; int len = 0;
    for (uint32_t k = 0; k < 0xE8; k += 8)
    {
        uint64_t v; memcpy(&v, g_streamContextCopy + k, 8);
        if (v >= blk && v < end && len < (int)sizeof inside - 8) len += snprintf(inside + len, sizeof inside - len, " +0x%X", k);
    }
    g_stream.pass = g_streamRecordCopy + ((uint8_t*)g_stream.pass - (uint8_t*)g_stream.record);
    g_stream.record = g_streamRecordCopy;
    g_stream.context = g_streamContextCopy;
    int ownArena = WorldStreamArena();
    if (ownArena) { uint64_t a = (uint64_t)(uintptr_t)&g_streamArena; memcpy(g_streamContextCopy, &a, 8); }
    else Note("difficulty: extra large maps: content streaming: no arena of its own (no memory, or the reset is not the reviewed one); scaled layers (city buildings) would read the level's freed one");
    Note("difficulty: extra large maps: content streaming: kept its own copy of the generator's record and context (the generation block is freed when the level goes live)%s%s",
        len ? "; context fields into that block:" : "", len ? inside : "");
}

/* After a generator call: at the count the op is done, so every zeroed stamp unit goes back. */
static void WorldStreamEnd(uint8_t* pass)
{
    AcquireSRWLockExclusive(&g_streamLock);
    if (g_stream.captured && !g_stream.generatorDone && g_stream.deferred > 0 && pass == g_stream.pass && g_stream.level == g_worldLevel)
    {
        uint32_t resume = 0;
        if (SafeRead((uintptr_t)pass + 0xD8, &resume, 4))
        {
            if (!g_stream.resumeBad)
            {
                g_stream.resumeBad = 1;
                Note("difficulty: extra large maps: content streaming: the generator's resume index does not read; its locations stay held");
            }
        }
        else if (resume >= g_stream.count)
        {
            for (uint32_t i = 0; i < g_stream.count; i++)
                if (g_streamDeferred[i])
                    memcpy((void*)((uintptr_t)g_stream.array + (size_t)i * kStreamRecord + 0xA8), &g_streamUnit[i], 8);
            g_stream.generatorDone = 1;
            Note("difficulty: extra large maps: content streaming: the generator finished; %u deferred locations are streamed near the players",
                g_stream.deferred);
            WorldStreamDetachLocked();
        }
    }
    ReleaseSRWLockExclusive(&g_streamLock);
}

/* SpawnLevelLayers: the generator's calls capture, defer and restore; the streamer's own call has g_streamOwn set. While
 * a generator call is inside (Begin, the original, End) g_streamInside keeps the game thread from calling the original. */
static uint8_t WorldStreamSpawn(uint8_t* pass, uint8_t* record, uint8_t* context)
{
    if (g_streamSpawn == NULL) return 0;
    if (g_streamOwn == (LONG)GetCurrentThreadId()) return g_streamSpawn(pass, record, context);
    InterlockedIncrement(&g_streamInside);
    int watched = WorldStreamBegin(pass, record, context);
    uint8_t result = g_streamSpawn(pass, record, context);
    if (watched) WorldStreamEnd(pass);
    InterlockedDecrement(&g_streamInside);
    return result;
}

/* Every 0.25 s: the players' positions and every deferred location's nearest one. A location below kStreamInRadius
 * becomes due (and stays due); the due ones are ordered nearest first. A live location beyond kStreamOutRadius starts a
 * timer and is removed after kStreamOutTimeout, at most kStreamRemoveBatch in this pass. */
static void WorldStreamScanLocked(ULONGLONG now)
{
    if (now < g_stream.nextScan) return;
    g_stream.nextScan = now + 250;
    WorldStreamPlayersLocked();
    if (g_stream.bodyPending && !g_stream.bodyNoted) /* the after count, a scan after the 20th removal */
    {
        uint32_t after = 0;
        if (WorldStreamBodies(&after))
        {
            g_stream.bodyPending = 0;
            g_stream.bodyNoted = 1;
            if (g_stream.bodyBeforeSeen)
            {
                int leaked = (int32_t)after - (int32_t)g_stream.bodyBefore;
                Note("difficulty: extra large maps: streaming: physics bodies %u -> %u over the first %u removals (%d, %d per removal)",
                    g_stream.bodyBefore, after, g_stream.removed, leaked, leaked / (int)g_stream.removed);
            }
            else Note("difficulty: extra large maps: streaming: physics bodies %u after %u removals (the count before the first did not read)",
                after, g_stream.removed);
        }
    }
    float keep = kStreamInRadius * kStreamInRadius;
    float out = kStreamOutRadius * kStreamOutRadius;
    uint32_t order = 0, removes = 0, formationsDue = 0, bodies = 0, slots = 0;
    int budget = WorldStreamBodyStore(&bodies, &slots) && (float)bodies < kStreamFormationShare * (float)slots;
    g_stream.bodiesUsed = bodies; g_stream.bodySlots = slots;
    for (uint32_t i = 0; i < g_stream.count; i++)
    {
        if (!g_streamDeferred[i]) continue;
        float best = kStreamFar;
        for (int p = 0; p < g_stream.playerCount; p++)
        {
            float dx = g_streamAt[i][0] - g_streamPlayerAt[p][0];
            float dy = g_streamAt[i][1] - g_streamPlayerAt[p][1];
            float distance = dx * dx + dy * dy;
            if (distance < best) best = distance;
        }
        if (g_streamLive[i] && g_streamFormation[i]) continue; /* a far formation stays */
        if (g_streamLive[i])
        {
            if (g_stream.playerCount > 0 && best > (g_streamOut2[i] > out ? g_streamOut2[i] : out))
            {
                if (g_streamOutSince[i] == 0) g_streamOutSince[i] = now;
            }
            else g_streamOutSince[i] = 0;
            if (g_streamOutSince[i] != 0 && now - g_streamOutSince[i] >= kStreamOutTimeout && removes < kStreamRemoveBatch &&
                g_streamPoseGood[i] && !g_streamNeedsUnit[i] && g_streamUnit[i] != 0)
            {
                if (WorldStreamRemoveLocked(i)) removes++;
            }
            continue;
        }
        if (g_streamSpawned[i]) continue; /* a record that did not read: never streamed */
        if (best < keep) g_streamDue[i] = 1;
        else if (g_streamFormation[i] && !g_streamDue[i] && budget && g_stream.playerCount > 0 && formationsDue < kStreamFormationsPerScan)
        {
            g_streamDue[i] = 1; /* in the stamps' order (largest apparent size first); the due list then goes nearest first */
            formationsDue++;
        }
        if (g_streamDue[i])
        {
            g_streamDistance[i] = best;
            g_streamOrder[order++] = i;
        }
    }
    g_stream.orderCount = order;
    g_stream.orderCursor = 0;
    WorldStreamSort(order);
}

static uint32_t WorldStreamDueCountLocked(void)
{
    uint32_t due = 0;
    for (uint32_t i = 0; i < g_stream.count; i++)
        if (g_streamDeferred[i] && g_streamDue[i] && !g_streamSpawned[i]) due++;
    return due;
}

/* Up to kStreamBatch due records per call, nearest first. The original queues their layers on the engine world's
 * asynchronous spawn queue; a full queue leaves the rest for the next frame (the returned resume index). Its records
 * are copied back (the call may write them), their +0xA8 kept as the real unit. At most ~2 ms of calls per frame. */
static void WorldStreamSpawnDueLocked(void)
{
    if (!g_stream.captured || !g_stream.generatorDone || g_stream.deferred == 0 || g_stream.orderCount == 0) return;
    if (g_streamSpawn == NULL) return;
    if (g_heightCopy == NULL || g_heightSource == 0) /* a layer spawned after generation crashes without the kept copy */
    {
        if (!g_stream.heightBad)
        {
            g_stream.heightBad = 1;
            Note("difficulty: extra large maps: content streaming waits: the generator's layer heightfield was not kept");
        }
        return;
    }
    uint8_t* stage = (uint8_t*)g_streamStage;
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    uint32_t cursor = g_stream.orderCursor;
    while (cursor < g_stream.orderCount)
    {
        uint32_t picked[kStreamBatch], pickedAt[kStreamBatch], count = 0;
        while (count < kStreamBatch && cursor < g_stream.orderCount)
        {
            uint32_t i = g_streamOrder[cursor];
            if (g_streamSpawned[i]) { cursor++; continue; }
            if (SafeRead((uintptr_t)g_stream.array + (size_t)i * kStreamRecord, stage + (size_t)count * kStreamRecord, kStreamRecord))
            {
                g_streamSpawned[i] = 1;
                g_stream.broken++;
                if (!g_stream.recordBad)
                {
                    g_stream.recordBad = 1;
                    Note("difficulty: extra large maps: content streaming: a location record did not read; it is skipped");
                }
                cursor++;
                continue;
            }
            if (g_streamNeedsUnit[i] && !WorldStreamRespawnLocked(i))
            {
                cursor++;
                continue;
            }
            memcpy(stage + (size_t)count * kStreamRecord + 0xA8, &g_streamUnit[i], 8);
            picked[count] = i;
            pickedAt[count] = cursor;
            count++;
            cursor++;
        }
        if (count == 0) { g_stream.orderCursor = cursor; break; }
        uint8_t* oldArray = NULL;
        uint64_t oldCountAt = 0;
        uint32_t oldResume = 0, resume = 0, countAt = count;
        if (SafeRead((uintptr_t)g_stream.pass + 8, &oldArray, 8) || SafeRead((uintptr_t)g_stream.pass + 0x48, &oldCountAt, 8) ||
            SafeRead((uintptr_t)g_stream.pass + 0xD8, &oldResume, 4))
        {
            if (!g_stream.passBad)
            {
                g_stream.passBad = 1;
                Note("difficulty: extra large maps: content streaming: the generator's pass no longer reads; streaming stops for this level");
            }
            break;
        }
        if (oldArray != g_stream.array)
        {
            if (!g_stream.passBad)
            {
                g_stream.passBad = 1;
                Note("difficulty: extra large maps: content streaming: the generator's pass no longer holds the captured location array; streaming stops for this level");
            }
            break;
        }
        uint64_t countHere = (uint64_t)(uintptr_t)&countAt;
        uint32_t zero = 0;
        memcpy((void*)((uintptr_t)g_stream.pass + 8), &stage, 8);
        memcpy((void*)((uintptr_t)g_stream.pass + 0x48), &countHere, 8);
        memcpy((void*)((uintptr_t)g_stream.pass + 0xD8), &zero, 4);
        g_streamOwn = (LONG)GetCurrentThreadId();
        g_streamSpawn(g_stream.pass, g_stream.record, g_stream.context);
        g_streamOwn = 0;
        if (g_stream.context == g_streamContextCopy) WorldStreamArenaReset();
        if (SafeRead((uintptr_t)g_stream.pass + 0xD8, &resume, 4))
        {
            resume = 0;
            if (!g_stream.resumeBad)
            {
                g_stream.resumeBad = 1;
                Note("difficulty: extra large maps: content streaming: the streamed call's resume index does not read; its locations stay due");
            }
        }
        memcpy((void*)((uintptr_t)g_stream.pass + 8), &oldArray, 8);
        memcpy((void*)((uintptr_t)g_stream.pass + 0x48), &oldCountAt, 8);
        memcpy((void*)((uintptr_t)g_stream.pass + 0xD8), &oldResume, 4);
        if (resume > count) resume = count;
        for (uint32_t k = 0; k < count; k++)
        {
            uintptr_t at = (uintptr_t)g_stream.array + (size_t)picked[k] * kStreamRecord;
            memcpy((void*)at, stage + (size_t)k * kStreamRecord, kStreamRecord);
            if (g_streamDetached[picked[k]])
            {
                uint64_t zero = 0;
                memcpy((void*)(at + 0xA8), &zero, 8); /* a respawned unit stays only in g_streamUnit */
            }
            else memcpy((void*)(at + 0xA8), &g_streamUnit[picked[k]], 8);
            if (k < resume)
            {
                g_streamSpawned[picked[k]] = 1;
                g_streamLive[picked[k]] = 1;
                g_stream.spawned++;
            }
        }
        if (resume < count)
        {
            g_stream.orderCursor = pickedAt[resume]; /* the queue was full: the rest stay due */
            break;
        }
        g_stream.orderCursor = cursor;
        QueryPerformanceCounter(&now);
        double elapsed = g_streamFrequency > 0 ? (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)g_streamFrequency : 0.0;
        if (elapsed >= 2.0) break;
    }
    QueryPerformanceCounter(&now);
    if (g_streamFrequency > 0)
        g_stream.spentMs += (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)g_streamFrequency;
}

/* The game thread's frames since the last note (WorldStreamTick's dt): the cost of what is spawned, as frame rate. */
static uint32_t g_streamFrames;
static float g_streamFrameSum, g_streamFrameMax;

/* The mission's objective instances as the extraction test reads them (0x5D91B0): the manager [game+0x3326DA0], count
 * +0x24, instances [+0x60] + i x 0x1078 with the state at +0x1018 and the class at +0x1038 (classes 2..4 are not
 * waited for; a waited-for one in state 3, or all of them in state 2, opens extraction). Each "class/state". */
static void WorldStreamObjectivesNote(void)
{
    uint64_t manager = 0, data = 0;
    uint32_t count = 0;
    if (g_stream.game == 0 || SafeRead(g_stream.game + 0x3326DA0u, &manager, 8) || manager == 0 || SafeRead(manager + 0x24, &count, 4) ||
        SafeRead(manager + 0x60, &data, 8))
    {
        Note("difficulty: extra large maps: objectives: the objective manager does not read");
        return;
    }
    char list[512], full[512]; int n = 0;
    uint32_t waited = 0, open = 0;
    MissionListsFull(full, sizeof full);
    uint32_t shown[2] = {0, 0}; /* the map's slots and trackers in use when it last updated (map object +0xF948 / +0xF94C) */
    if (g_markerMap != 0) { SafeRead(g_markerMap + 0xF948, &shown[0], 4); SafeRead(g_markerMap + 0xF94C, &shown[1], 4); }
    for (uint32_t i = 0; i < count && i < 64 && data != 0; i++)
    {
        uint32_t state = 0, cls = 0;
        if (SafeRead(data + 0x1078ull * i + 0x1018, &state, 4) || SafeRead(data + 0x1078ull * i + 0x1038, &cls, 4)) break;
        if (!(cls >= 2 && cls <= 4)) { waited++; if (state != 2 && state != 3) open++; }
        if (n < (int)sizeof list - 16) n += snprintf(list + n, sizeof list - n, " %u/%u", cls, state);
    }
    Note("difficulty: extra large maps: objectives: %u instance(s), %u waited for (%u not done); class/state:%s; physics stream swaps under the world's lock so far: %lld; convoy escort spawns stopped at 8 so far: %ld; units past their mission list:%s; patrol spawners without a garrison place: %ld; alarms without one: %ld; spawner positions past a group's 16: %ld; sample owners past 256: %ld, samples past an owner's 256: %ld; AI tasks %u of 2048 (guard points left out at load: %ld); map markers last shown %u, trackers %u",
        count, waited, open, n ? list : " none", (long long)g_sceneSwapsLocked, g_convoyFull != NULL ? (long)*g_convoyFull : -1L, full[0] ? full : " none", g_garrisonSkipped != NULL ? (long)*g_garrisonSkipped : -1L,
        g_alarmSkipped != NULL ? (long)*g_alarmSkipped : -1L, g_spawnerPlacesFull != NULL ? (long)*g_spawnerPlacesFull : -1L,
        g_sampleGroupsFull != NULL ? (long)g_sampleGroupsFull[0] : -1L, g_sampleGroupsFull != NULL ? (long)g_sampleGroupsFull[1] : -1L,
        TasksUsed(), g_tasksLeftOut != NULL ? (long)*g_tasksLeftOut : -1L, shown[0], shown[1]);
}

/* Every 10 s while streaming: what is spawned now, what is due, removals and respawns so far, the players and time,
 * and the frame rate over those 10 s (mean and the slowest frame). */
static void WorldStreamNoteLocked(ULONGLONG now)
{
    if (g_stream.deferred == 0) return;
    if (g_stream.nextNote == 0) { g_stream.nextNote = now + 10000; return; }
    if (now < g_stream.nextNote) return;
    g_stream.nextNote = now + 10000;
    uint32_t due = WorldStreamDueCountLocked();
    uint32_t formations = 0, formationsLive = 0;
    for (uint32_t i = 0; i < g_stream.count; i++) if (g_streamFormation[i]) { formations++; if (g_streamLive[i]) formationsLive++; }
    Note("difficulty: extra large maps: streaming: %u of %u spawned (%u due now, %u skipped; far formations %u of %u), %u removed so far, %u respawned so far, %d player(s), %.1f ms spent; physics bodies %u of %u; %.1f fps, slowest frame %.1f ms",
        g_stream.spawned, g_stream.deferred, due, g_stream.broken, formationsLive, formations, g_stream.removed, g_stream.respawned, g_stream.playerCount, g_stream.spentMs,
        g_stream.bodiesUsed, g_stream.bodySlots,
        g_streamFrameSum > 0 ? (double)g_streamFrames / (double)g_streamFrameSum : 0.0, (double)g_streamFrameMax * 1000.0);
    g_streamFrames = 0; g_streamFrameSum = g_streamFrameMax = 0;
    WindowNoteLocked();
    DestroyNote();
    WorldStreamObjectivesNote();
}

/* Called from TickDetour (packages.c) every frame: nothing until a level is captured and the generator's op is done,
 * nothing once g_worldLevel is another level (all state is dropped), nothing while the world is back to vanilla. */
static void WorldStreamTick(float dt)
{
    if (g_streamInstallState != 1) return;
    DestroyTick(); /* destroys that waited for their unit (difficulty_world_destroy.c) */
    if (dt > 0 && dt < 10.0f) { g_streamFrames++; g_streamFrameSum += dt; if (dt > g_streamFrameMax) g_streamFrameMax = dt; }
    if (g_streamInside) return; /* a generator call is inside: it owns the pass */
    AcquireSRWLockExclusive(&g_streamLock);
    if (!g_stream.captured)
    {
        ReleaseSRWLockExclusive(&g_streamLock);
        return;
    }
    if (g_worldLevel != g_stream.level)
    {
        WorldStreamClearLocked();
        ReleaseSRWLockExclusive(&g_streamLock);
        return;
    }
    if (g_stream.deferred == 0 || g_streamInside || !g_stream.generatorDone || !(g_world.current > 1.0f))
    {
        ReleaseSRWLockExclusive(&g_streamLock);
        return;
    }
    ULONGLONG now = GetTickCount64();
    WorldStreamScanLocked(now);
    WindowScanLocked(now); /* the planned stamps near the players into the window (difficulty_stream_window.c) */
    WorldStreamSpawnDueLocked();
    WorldStreamNoteLocked(now);
    ReleaseSRWLockExclusive(&g_streamLock);
    /* Removal runs in WorldStreamScanLocked and respawn in WorldStreamSpawnDueLocked, both called above on this thread
     * under this lock and only when no generator call is inside; only a location this streamer queued is removed. */
}

/* NULL when the hook is in (or the level is not extra large, so there is nothing to stream), else why it is not. */
static const char* InstallWorldStream(uintptr_t game)
{
    if (g_streamInstallState == 1) return NULL;
    if (g_streamInstallState < 0) return "the extra large maps' content streamer is not installed";
    if (WorldScaleFor(g_difficulty.mapScale) <= 1u) return NULL; /* a vanilla world: no location to defer */
    if (game == 0) return "game.dll is not loaded (content streaming needs it)";
    if (!ApiCodeMatches(game + DIF_STREAM_SPAWN, kStreamSpawnPin))
    {
        g_streamInstallState = -1;
        Note("difficulty: extra large maps: content streaming off: SpawnLevelLayers (0x%X) is not the reviewed one", (unsigned)DIF_STREAM_SPAWN);
        return "SpawnLevelLayers is not the reviewed one (content streaming needs it)";
    }
    if (LoaderHook("extra large maps' content streamer", game + DIF_STREAM_SPAWN, (void*)WorldStreamSpawn, (void**)&g_streamSpawn))
    {
        g_streamInstallState = -1;
        Note("difficulty: extra large maps: content streaming off: SpawnLevelLayers could not be hooked");
        return "SpawnLevelLayers could not be hooked for content streaming";
    }
    LARGE_INTEGER frequency;
    frequency.QuadPart = 0;
    QueryPerformanceFrequency(&frequency);
    g_streamFrequency = frequency.QuadPart;
    g_stream.game = game;
    g_streamInstallState = 1;
    InstallDestroyQueue();
    Note("difficulty: extra large maps: content streams: %d locations at most, %d per call, kept within %.0f m of the start, streamed within %.0f m of a player, removed beyond %.0f m after %d s",
        (int)kStreamMaxLocations, (int)kStreamBatch, (double)kStreamStartRadius, (double)kStreamInRadius, (double)kStreamOutRadius,
        (int)(kStreamOutTimeout / 1000));
    return NULL;
}

/* The install step (difficulty_install.c's kInstallSteps): InstallWorldStream takes the game module. It is declared
 * there before the steps and runs after InstallWorld; NULL when installed or nothing streams, else why. */
static const char* InstallWorldStreamStep(DifficultyInstall* d)
{
    return InstallWorldStream(d->game);
}

/* Research export (Lua): make the streamer treat world-space (x, y) as one more player position, or take it away.
 * The state is set and cleared under g_streamLock; it survives level changes until turned off. 0 when the streamer is
 * not installed. */
HF_LUA_API int hellforge_research_stream_observer(int on, float x, float y)
{
    AcquireSRWLockExclusive(&g_streamLock);
    if (g_streamInstallState != 1)
    {
        ReleaseSRWLockExclusive(&g_streamLock);
        return 0;
    }
    if (on != 0)
    {
        int changed = !g_streamObserverOn || x != g_streamObserverAt[0] || y != g_streamObserverAt[1];
        g_streamObserverOn = 1;
        g_streamObserverAt[0] = x;
        g_streamObserverAt[1] = y;
        if (changed) Note("research: stream observer at %.1f, %.1f", (double)x, (double)y);
    }
    else
    {
        int changed = g_streamObserverOn;
        g_streamObserverOn = 0;
        g_streamObserverAt[0] = 0.0f;
        g_streamObserverAt[1] = 0.0f;
        if (changed) Note("research: stream observer off");
    }
    ReleaseSRWLockExclusive(&g_streamLock);
    return 1;
}
