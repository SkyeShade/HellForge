/* Extra large maps: the engine's destroy queue takes only units that exist. The world's destroy list (world+0x30, 8192
 * u64, count world+0x10030) is filled by 0x12D2D0(world, u32* handles, n) (helldivers2.exe; world interface +0x58 and
 * +0x60 jump to it, 196 game.dll sites). For each unit handle (index = h & 0x3FFFFF, generation = h >> 22, in the
 * engine's "unit_manager" table [exe+0x1A100F0]: generation bytes +0xA0 with their count +0x98, pointers +0x88, lock
 * +0xD0 taken through the imports at exe+0x13F6760 / +0x13F6758) it stores the unit and frees its slot when the
 * handle resolves, and otherwise `handle | 0xF000000000000000`. The world update's drain (0x12D600) resolves a tagged
 * entry again after it has made this frame's async spawns (at most 512 a frame, 0x12D889) and, when it still does not
 * resolve, reads [0 + 8] (0x12E701..0x12E71B). So a destroy asked for a unit still waiting past the 512 cut, a unit
 * whose creation returned none (0x1A3930), or one already gone ends the game (run s75z: exe+0x12E71B in the mission's
 * first frames; static, subagent crash12e71b). Here every handle is looked at first, as the enqueue itself does: one
 * that resolves goes to the original; a pending one (generation equal, no unit yet) waits in a list and is passed on
 * once it resolves (at most DESTROY_WAIT_TICKS frames); a stale one (generation moved on, or past the count) is
 * dropped: its unit is gone already. */

#define DESTROY_QUEUE 0x12D2D0u
#define DESTROY_TABLE 0x1A100F0u
#define DESTROY_LOCK_SHARED 0x13F6760u
#define DESTROY_UNLOCK_SHARED 0x13F6758u
#define DESTROY_WAITING 65536 /* a 0x65 cleanup asks for up to 4096 a call (one per stamp level), MoveFail's for more */
#define DESTROY_LIST 0x10030u   /* world+0x10030: the list's count; its 8192 slots at world+0x30 end there */
#define DESTROY_ROOM (8192 - 512) /* 0x12D2D0 reserves with lock xadd and never checks for room: slot 8192 IS the count */
#define DESTROY_WAIT_TICKS 600u
static const char kDestroyQueuePin[] = "40 56 41 54 41 55 41 56 41 57 48 83 EC 20 45 8B E0 4C 8B F2 4C 8B E9 45 8B F8 F0 44 0F C1 B9 30 00 01 00";

typedef void (*DestroyQueueFn)(uint64_t world, uint32_t* handles, uint32_t n);
typedef void (*DestroyLockFn)(void* lock);
static DestroyQueueFn g_destroyQueue;
static volatile LONG* g_poseSkipped; /* InstallPoseGuard's count of poses skipped for units that are gone */
static volatile LONG* g_bodyDead;    /* InstallBodySyncGuard's count of moved bodies dropped for units that are gone */
static const uint32_t* g_bodyDeadSeen; /* the first BODY_DEAD_KEPT of them: {unit id, body, node, actor} each */
#define BODY_DEAD_KEPT 16
static DestroyLockFn g_destroyLock, g_destroyUnlock;
static uintptr_t g_destroyTableAt;
static SRWLOCK g_destroyWaitLock = SRWLOCK_INIT;
static struct { uint64_t world; uint32_t handle, ticks; uintptr_t level; } g_destroyWait[DESTROY_WAITING];
static volatile LONG g_destroyRoomHeld; /* destroys of live units held until the list had room */
static volatile LONG g_destroyWaiting, g_destroyHeld, g_destroyPassed, g_destroyStale, g_destroyTimedOut, g_destroyFull, g_destroyNotes;

/* 0: resolves (a unit), 1: pending (its slot is live, no unit yet), 2: stale. */
static int DestroyHandleState(uint32_t h)
{
    uint64_t table = *(volatile uint64_t*)g_destroyTableAt;
    if (table == 0) return 2;
    void* lock = (void*)(uintptr_t)(table + 0xD0);
    g_destroyLock(lock);
    uint32_t i = h & 0x3FFFFFu, count = *(volatile uint32_t*)(uintptr_t)(table + 0x98);
    int state = 2;
    if (i < count)
    {
        uint8_t generation = *(volatile uint8_t*)(uintptr_t)(*(volatile uint64_t*)(uintptr_t)(table + 0xA0) + i);
        if (generation == (uint8_t)(h >> 22))
            state = *(volatile uint64_t*)(uintptr_t)(*(volatile uint64_t*)(uintptr_t)(table + 0x88) + 8ull * i) != 0 ? 0 : 1;
    }
    g_destroyUnlock(lock);
    return state;
}

/* The world's destroy list count (world+0x10030), or DESTROY_ROOM when it does not read. Runs s85a/s85b: MoveFail's
 * in-frame 0x65 loop queued 15218 / 23529 layer-unit destroys (0xE1200 -> 0x12D2D0 at 0xE149C) into the 8192 slots: the
 * 8193rd overwrote the count, later ones the records after it, and the drain walked past the end (exe+0x12E71B; static
 * + dumps, subagent drain85). */
static LONG DestroyListCount(uint64_t world)
{
    int32_t c = 0;
    if (world == 0 || SafeRead(world + DESTROY_LIST, &c, 4) || c < 0 || c > 8192) return DESTROY_ROOM;
    return c;
}

static void DestroyHold(uint64_t world, uint32_t handle)
{
    AcquireSRWLockExclusive(&g_destroyWaitLock);
    if (g_destroyWaiting < DESTROY_WAITING)
    {
        g_destroyWait[g_destroyWaiting].world = world; g_destroyWait[g_destroyWaiting].handle = handle;
        g_destroyWait[g_destroyWaiting].ticks = 0; g_destroyWait[g_destroyWaiting].level = g_worldLevel;
        g_destroyWaiting++;
    }
    else InterlockedIncrement(&g_destroyFull);
    ReleaseSRWLockExclusive(&g_destroyWaitLock);
}

/* Research (s90a: 16 objectives COMPLETED on the first frame in play through the entity machines' API 0x4D7EA0, after
 * 0x4D8440 -> 0x5D6E30 counted every listed unit as done because [api+0x18]+0x720 = exe 0x1FEEB0 (0x9D8C0(id) != 0)
 * found none; the units existed when listed, the adders read them through +0x750 = 0x1FC120): the last DESTROY_SEEN
 * destroys asked for, each with its call chain, so a completion can name what took its units. */
#define DESTROY_SEEN 262144
static struct { uint32_t handle, ms; uintptr_t chain[4]; } g_destroySeen[DESTROY_SEEN];
static volatile LONG g_destroySeenAt;
static void DestroySeen(uint32_t* handles, uint32_t n)
{
    void* chain[4] = {0};
    RtlCaptureStackBackTrace(2, 4, chain, NULL);
    uint32_t ms = (uint32_t)GetTickCount64();
    for (uint32_t i = 0; i < n; i++)
    {
        LONG at = (InterlockedIncrement(&g_destroySeenAt) - 1) & (DESTROY_SEEN - 1);
        g_destroySeen[at].handle = handles[i]; g_destroySeen[at].ms = ms;
        for (int f = 0; f < 4; f++) g_destroySeen[at].chain[f] = (uintptr_t)chain[f];
    }
}
/* Research (s91a: those units were destroyed by the game's own entity removal, g+0xFDCCFC in 0xFDC820(state, record,
 * 0) <- the frame's removal list g+0xAB7060 (state+0xF3EF20, count +0xF3EF18; records state+0xF32F18 + 24 i {+0 type,
 * +8 entity id, +0xC unit, +0x10 cell or 0x7FFF, +0x14 flags}), one frame after 0xFDC310(state, entity id) queued them
 * (via 0xFDC4E0 when this peer owns the cell); 0xFDC310 has 422 callers): each queued removal's unit and requester. */
#define REMOVE_REQUEST 0xFDC310u
static const char kRemoveRequestPin[] = "48 89 5C 24 08 57 48 83 EC 30 3B 15";
#define REMOVE_SEEN 65536
typedef void (*RemoveRequestFn)(uint64_t state, uint32_t id);
static RemoveRequestFn g_removeRequest;
static struct { uint32_t unit, id, ms; uintptr_t chain[6]; } g_removeSeen[REMOVE_SEEN];
static volatile LONG g_removeSeenAt;
static void RemoveRequestDetour(uint64_t state, uint32_t id)
{
    uint32_t before = 0, after = 0, index = 0, unit = 0;
    uint64_t list = 0;
    SafeRead(state + 0xF3EF18, &before, 4);
    g_removeRequest(state, id);
    if (SafeRead(state + 0xF3EF18, &after, 4) || after != before + 1 || SafeRead(state + 0xF3EF20, &list, 8) || list == 0 ||
        SafeRead(list + 4ull * before, &index, 4) || SafeRead(state + 0xF32F18 + 24ull * index + 0xC, &unit, 4)) return;
    void* chain[6] = {0};
    RtlCaptureStackBackTrace(1, 6, chain, NULL);
    LONG at = (InterlockedIncrement(&g_removeSeenAt) - 1) & (REMOVE_SEEN - 1);
    g_removeSeen[at].unit = unit; g_removeSeen[at].id = id; g_removeSeen[at].ms = (uint32_t)GetTickCount64();
    for (int f = 0; f < 6; f++) g_removeSeen[at].chain[f] = (uintptr_t)chain[f];
}
/* Research (s94b crash, c0000005 at g+0x54A4B2 during the level's spawns: SpawnEntity 0xFDC140 -> 0x581320 -> the
 * ability component's post-create 0x54A3E0 found no instance of the entity in the ability manager [g+0x3326640]'s
 * hash (+0x18 slots {id, index}, +0x20 places, +0x24 empty, +0x28 multiplier) and wrote its index -1. The create loop
 * (state+0xF0C110[290] = add 0x54A2B0) runs before the post-create loop (state+0xF0CB30[290]) over the same component
 * list, so the instance was taken out in between (remove 0x54A380 = state+0xF0F2A0[290]) or by another thread): every
 * add and remove with its thread and chain, and a post-create that finds none logs the entity's history and is left
 * out (research only). */
#define ABILITY_ADD 0x54A2B0u
#define ABILITY_REMOVE 0x54A380u
#define ABILITY_POST 0x54A3E0u
#define ABILITY_MANAGER 0x3326640u
static const char kAbilityAddPin[] = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57";
static const char kAbilityRemovePin[] = "40 53 48 83 EC 20 8B 51 08 48 8B 1D";
static const char kAbilityPostPin[] = "40 53 57 41 54 41 56 41 57 48 83 EC 20 8B 41 08";
typedef uint64_t (*AbilityHookFn)(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f); /* the post-create takes 5 (0x58151E) */
static AbilityHookFn g_abilityAdd, g_abilityRemove, g_abilityPost;
#define ABILITY_SEEN 8192
static struct { uint32_t id, op, thread, ms; uintptr_t chain[6]; } g_abilitySeen[ABILITY_SEEN];
static volatile LONG g_abilitySeenAt, g_abilityMissed;
static void AbilitySeen(uint64_t entity, uint32_t op)
{
    uint32_t id = 0;
    if (entity == 0 || SafeRead(entity + 8, &id, 4)) return;
    void* chain[6] = {0};
    RtlCaptureStackBackTrace(2, 6, chain, NULL);
    LONG at = (InterlockedIncrement(&g_abilitySeenAt) - 1) & (ABILITY_SEEN - 1);
    g_abilitySeen[at].id = id; g_abilitySeen[at].op = op; g_abilitySeen[at].thread = GetCurrentThreadId(); g_abilitySeen[at].ms = (uint32_t)GetTickCount64();
    for (int f = 0; f < 6; f++) g_abilitySeen[at].chain[f] = (uintptr_t)chain[f];
}
static uint64_t AbilityAddDetour(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { AbilitySeen(a, 1); return g_abilityAdd(a, b, c, d, e, f); }
static uint64_t AbilityRemoveDetour(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { AbilitySeen(a, 2); return g_abilityRemove(a, b, c, d, e, f); }
static int ChainText(const uintptr_t* chain, int n, char* out, size_t size);
static uint64_t AbilityPostDetour(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    uint64_t mgr = 0, table = 0;
    uint32_t id = 0, places = 0, empty = 0, mult = 0, found = 0;
    if (a == 0 || SafeRead(a + 8, &id, 4) || SafeRead(g_world.game + ABILITY_MANAGER, &mgr, 8) || mgr == 0 || SafeRead(mgr + 0x18, &table, 8) ||
        SafeRead(mgr + 0x20, &places, 4) || SafeRead(mgr + 0x24, &empty, 4) || SafeRead(mgr + 0x28, &mult, 4) || places == 0 || (places & (places - 1)))
        return g_abilityPost(a, b, c, d, e, f);
    for (uint32_t k = 0; k < places; k++)
    {
        uint32_t slot[2] = {0, 0};
        if (SafeRead(table + 8ull * ((k + mult * id) & (places - 1)), slot, 8)) return g_abilityPost(a, b, c, d, e, f);
        if (slot[0] == id) { found = slot[1] != 0xFFFFFFFFu; break; }
        if (slot[0] == empty) break;
    }
    if (found) return g_abilityPost(a, b, c, d, e, f);
    if (InterlockedIncrement(&g_abilityMissed) <= 4)
    {
        void* chain[6] = {0};
        uintptr_t here[6];
        RtlCaptureStackBackTrace(1, 6, chain, NULL);
        for (int f = 0; f < 6; f++) here[f] = (uintptr_t)chain[f];
        char text[400];
        ChainText(here, 6, text, sizeof text);
        uint32_t count = 0;
        SafeRead(mgr + 8, &count, 4);
        Note("difficulty: abilities: a post-create found no instance of entity 0x%X (thread %lu, manager %u instances, %u places); left out (research); from%s",
            id, GetCurrentThreadId(), count, places, text);
        LONG last = g_abilitySeenAt, from = last > ABILITY_SEEN ? last - ABILITY_SEEN : 0, shown = 0;
        for (LONG k = last - 1; k >= from && shown < 6; k--)
        {
            LONG i = k & (ABILITY_SEEN - 1);
            if (g_abilitySeen[i].id != id) continue;
            ChainText(g_abilitySeen[i].chain, 6, text, sizeof text);
            Note("difficulty: abilities: entity 0x%X %s %u ms ago on thread %lu from%s", id, g_abilitySeen[i].op == 1 ? "added" : "removed",
                (uint32_t)GetTickCount64() - g_abilitySeen[i].ms, (unsigned long)g_abilitySeen[i].thread, text);
            shown++;
        }
        if (!shown) Note("difficulty: abilities: entity 0x%X: no add or remove in the last %d", id, ABILITY_SEEN);
    }
    return 0;
}
static void AbilityWatch(uintptr_t game)
{
    if (g_abilityPost != NULL) return;
    if (ApiCodeMatches(game + ABILITY_ADD, kAbilityAddPin) != 1 || ApiCodeMatches(game + ABILITY_REMOVE, kAbilityRemovePin) != 1 ||
        ApiCodeMatches(game + ABILITY_POST, kAbilityPostPin) != 1) { Note("difficulty: abilities: the ability component is not the reviewed code; not watched"); return; }
    if (LoaderHook("ability instance adds (research)", game + ABILITY_ADD, (void*)AbilityAddDetour, (void**)&g_abilityAdd) != 0 ||
        LoaderHook("ability instance removes (research)", game + ABILITY_REMOVE, (void*)AbilityRemoveDetour, (void**)&g_abilityRemove) != 0 ||
        LoaderHook("ability post-creates (research)", game + ABILITY_POST, (void*)AbilityPostDetour, (void**)&g_abilityPost) != 0) g_abilityPost = NULL;
}
static void RemoveWatch(uintptr_t game)
{
    AbilityWatch(game);
    if (g_removeRequest != NULL) return;
    if (ApiCodeMatches(game + REMOVE_REQUEST, kRemoveRequestPin) != 1) { Note("difficulty: objectives: the entity removal request is not the reviewed one; removals not logged"); return; }
    if (LoaderHook("entity removal requests (research)", game + REMOVE_REQUEST, (void*)RemoveRequestDetour, (void**)&g_removeRequest) != 0) g_removeRequest = NULL;
}
static int ChainText(const uintptr_t* chain, int n, char* out, size_t size)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    int at = 0;
    for (int f = 0; f < n && at < (int)size - 24; f++)
    {
        uintptr_t a = chain[f];
        at += a >= exe && a < exe + 0x4000000u ? snprintf(out + at, size - at, " e+0x%llx", (unsigned long long)(a - exe))
            : a >= g_world.game && a < g_world.game + 0x8000000u ? snprintf(out + at, size - at, " g+0x%llx", (unsigned long long)(a - g_world.game))
            : snprintf(out + at, size - at, " %p", (void*)a);
    }
    return at;
}

/* "exists", "pending" or "gone", and when it was asked to be destroyed, by what (or "no destroy seen"). */
static void DestroyDescribe(uint32_t handle, char* out, size_t size)
{
    int state = g_destroyTableAt ? DestroyHandleState(handle) : 2;
    int at = snprintf(out, size, "%s", state == 0 ? "exists" : state == 1 ? "pending" : "gone");
    LONG last = g_destroySeenAt, from = last > DESTROY_SEEN ? last - DESTROY_SEEN : 0;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    for (LONG k = last - 1; k >= from; k--)
    {
        LONG i = k & (DESTROY_SEEN - 1);
        if (g_destroySeen[i].handle != handle) continue;
        at += snprintf(out + at, size - at, ", destroy asked %u ms ago from", (uint32_t)GetTickCount64() - g_destroySeen[i].ms);
        for (int f = 0; f < 4 && at < (int)size - 24; f++)
        {
            uintptr_t a = g_destroySeen[i].chain[f];
            at += a >= exe && a < exe + 0x4000000u ? snprintf(out + at, size - at, " e+0x%llx", (unsigned long long)(a - exe))
                : a >= g_world.game && a < g_world.game + 0x8000000u ? snprintf(out + at, size - at, " g+0x%llx", (unsigned long long)(a - g_world.game))
                : snprintf(out + at, size - at, " %p", (void*)a);
        }
        break;
    }
    if (at < (int)size - 40 && g_removeSeenAt == 0) { snprintf(out + at, size - at, "; removal requests not watched"); return; }
    LONG lastR = g_removeSeenAt, fromR = lastR > REMOVE_SEEN ? lastR - REMOVE_SEEN : 0;
    for (LONG k = lastR - 1; k >= fromR; k--)
    {
        LONG i = k & (REMOVE_SEEN - 1);
        if (g_removeSeen[i].unit != handle) continue;
        at += snprintf(out + at, size - at, "; entity 0x%X removal asked %u ms ago from", g_removeSeen[i].id, (uint32_t)GetTickCount64() - g_removeSeen[i].ms);
        ChainText(g_removeSeen[i].chain, 6, out + at, size - at);
        return;
    }
    snprintf(out + at, size - at, "; no removal request seen");
}

static void DestroyQueueDetour(uint64_t world, uint32_t* handles, uint32_t n)
{
    if (handles == NULL || n == 0) { g_destroyQueue(world, handles, n); return; }
    DestroySeen(handles, n);
    uint32_t i = 0;
    while (i < n && DestroyHandleState(handles[i]) == 0) i++;
    LONG room = DESTROY_ROOM - DestroyListCount(world);
    if (i == n && (LONG)n <= room) { g_destroyQueue(world, handles, n); return; } /* the usual case: all exist, room for them */
    uint32_t pass[256], k = 0;
    void* caller = __builtin_return_address(0);
    for (i = 0; i < n; i++)
    {
        int state = DestroyHandleState(handles[i]);
        if (state == 0)
        {
            if (room - (LONG)k <= 0) { DestroyHold(world, handles[i]); InterlockedIncrement(&g_destroyRoomHeld); continue; }
            pass[k++] = handles[i];
            if (k == 256) { g_destroyQueue(world, pass, k); room -= (LONG)k; k = 0; }
            continue;
        }
        if (InterlockedIncrement(&g_destroyNotes) <= 8)
            Note("difficulty: extra large maps: a destroy asked for unit 0x%08X, which %s (caller %p); %s", handles[i], state == 1 ? "is not made yet" : "is gone",
                caller, state == 1 ? "it waits until it is" : "dropped");
        if (state == 2) { InterlockedIncrement(&g_destroyStale); continue; }
        DestroyHold(world, handles[i]); InterlockedIncrement(&g_destroyHeld);
    }
    if (k) g_destroyQueue(world, pass, k);
}

/* Each frame on the game thread (WorldStreamTick): the waiting handles that resolve now go to the original. */
static void DestroyTick(void)
{
    if (g_destroyQueue == NULL || g_destroyWaiting == 0) return;
    AcquireSRWLockExclusive(&g_destroyWaitLock);
    uint64_t roomWorld = 0;
    LONG room = 0;
    for (LONG j = 0; j < g_destroyWaiting;)
    {
        int state = g_destroyWait[j].level != g_worldLevel ? 2 : DestroyHandleState(g_destroyWait[j].handle);
        if (state == 1 && ++g_destroyWait[j].ticks <= DESTROY_WAIT_TICKS) { j++; continue; }
        if (state == 0)
        {
            if (g_destroyWait[j].world != roomWorld) { roomWorld = g_destroyWait[j].world; room = DESTROY_ROOM - DestroyListCount(roomWorld); }
            if (room <= 0) { j++; continue; } /* the frame's drain empties the list: next frame */
            uint32_t h = g_destroyWait[j].handle; g_destroyQueue(g_destroyWait[j].world, &h, 1); g_destroyPassed++; room--;
        }
        else if (state == 1) g_destroyTimedOut++;
        else g_destroyStale++;
        g_destroyWait[j] = g_destroyWait[--g_destroyWaiting];
    }
    ReleaseSRWLockExclusive(&g_destroyWaitLock);
}

static void DestroyNote(void)
{
    static LONG last[4] = {-1, -1, -1, -1};
    LONG now[4] = {g_destroyHeld, g_destroyPassed + g_destroyTimedOut, g_destroyStale, g_destroyFull};
    static LONG poseNoted;
    LONG skipped = g_poseSkipped != NULL ? *g_poseSkipped : 0;
    if (skipped != poseNoted) { poseNoted = skipped; Note("difficulty: extra large maps: poses for units that are gone skipped: %ld", (long)skipped); }
    static LONG bodyNoted;
    LONG dead = g_bodyDead != NULL ? *g_bodyDead : 0;
    if (dead != bodyNoted)
    {
        uint64_t table = 0, gens = 0;
        uint32_t count = 0;
        if (g_destroyTableAt == 0 || SafeRead(g_destroyTableAt, &table, 8) || table == 0 || SafeRead(table + 0xA0, &gens, 8) || SafeRead(table + 0x98, &count, 4)) gens = 0;
        for (LONG i = bodyNoted; i < dead && i < BODY_DEAD_KEPT; i++)
        {
            const uint32_t* e = g_bodyDeadSeen + i * 4;
            uint32_t index = e[0] & 0x3FFFFFu;
            uint8_t gen = 0;
            int genRead = gens != 0 && index < count && !SafeRead(gens + index, &gen, 1);
            Note("difficulty: extra large maps: moved body %ld of a unit that is gone: unit 0x%08X (index 0x%X, generation %u; the table's now %s%u of %u units), body 0x%X, node %u, actor 0x%08X",
                    (long)i + 1, e[0], index, e[0] >> 22, genRead ? "" : "unread ", (unsigned)gen, count, e[1], e[2], e[3]);
        }
        bodyNoted = dead;
        Note("difficulty: extra large maps: moved bodies of units that are gone left out of the pose sync: %ld", (long)dead);
    }
    static LONG roomNoted;
    if (g_destroyRoomHeld != roomNoted) { roomNoted = g_destroyRoomHeld; Note("difficulty: extra large maps: destroys held until the world's destroy list had room: %ld", (long)roomNoted); }
    if (memcmp(now, last, sizeof now) == 0 || (now[0] == 0 && now[2] == 0 && now[3] == 0)) return;
    memcpy(last, now, sizeof now);
    Note("difficulty: extra large maps: destroys of units not made yet: %ld held (%ld passed on once made, %ld waited too long, %ld waiting), %ld of units gone dropped, %ld not held (list full)",
            (long)g_destroyHeld, (long)g_destroyPassed, (long)g_destroyTimedOut, (long)g_destroyWaiting, (long)g_destroyStale, (long)g_destroyFull);
}

/* The physics pose sync (exe 0x79C360, from 0x79BC82: per moved body {unit id, actor}) writes each pose through the unit
 * API's +0x78 = 0x1FCD00, which looks the unit up (0x9D8C0: null when the handle's generation moved on or its index is
 * past the count) and calls [unit]+0xE8 with no check. Run s83a (city, a moving tile's 26026 stamp units in flight):
 * exe+0x1FCD17 reading 0x0 for unit 0x00C3CBDC: a body outlived its unit. Here a unit that no longer exists gets no pose
 * (0x1FCD00 is reached only through the API table; its result is its tail call's, which a missing unit never reaches).
 * Why the body outlived the unit is open (KB xl-world-streaming: the window's ~180-body leak). */
#define POSE_SITE 0x1FCD14u /* mov rcx,rax; mov r9,[rax] (6 bytes), then call [r9+0xE8] at 0x1FCD1A */
#define POSE_BACK 0x1FCD1Au
static const char kPosePin[] = "48 8B C8 4C 8B 08 41 FF 91 E8 00 00 00 4C 8B C3 8B D7 48 8B C8 48 8B 5C 24 30 48 83 C4 20 5F E9";
static void InstallPoseGuard(uintptr_t exe)
{
    if (g_poseSkipped != NULL) return;
    if (ApiCodeMatches(exe + POSE_SITE - 5, "E8 AC 0B EA FF") != 1 || ApiCodeMatches(exe + POSE_SITE, kPosePin) != 1)
    { Note("difficulty: extra large maps: the unit pose setter (exe 0x1FCD00) is not the reviewed one; a pose for a gone unit is not checked"); return; }
    uint8_t* page = AllocateNearGame(exe, 0x1000);
    if (page == NULL) { Note("difficulty: extra large maps: no room near the exe for the pose guard"); return; }
    /* test rax,rax; jz null; mov rcx,rax; mov r9,[rax]; jmp 0x1FCD1A;
     * null: lock inc dword [count]; mov rbx,[rsp+0x30]; add rsp,0x20; pop rdi; ret (0x1FCD29..0x1FCD32's epilogue) */
    static const uint8_t code[] = {
        0x48, 0x85, 0xC0,                       /* +0  test rax,rax */
        0x74, 0x0B,                             /* +3  jz +16 */
        0x48, 0x8B, 0xC8,                       /* +5  mov rcx,rax */
        0x4C, 0x8B, 0x08,                       /* +8  mov r9,[rax] */
        0xE9, 0, 0, 0, 0,                       /* +11 jmp 0x1FCD1A (ends +16) */
        0xF0, 0xFF, 0x05, 0, 0, 0, 0,           /* +16 lock inc dword [rip+rel] (ends +23) */
        0x48, 0x8B, 0x5C, 0x24, 0x30,           /* +23 mov rbx,[rsp+0x30] */
        0x48, 0x83, 0xC4, 0x20,                 /* +28 add rsp,0x20 */
        0x5F,                                   /* +32 pop rdi */
        0xC3};                                  /* +33 ret */
    _Static_assert(sizeof code == 34, "pose guard size");
    memcpy(page, code, sizeof code);
    g_poseSkipped = (volatile LONG*)(page + 0x40);
    *g_poseSkipped = 0;
    intptr_t back = (intptr_t)(exe + POSE_BACK) - (intptr_t)(page + 16), reach = (intptr_t)page - (intptr_t)(exe + POSE_SITE + 5);
    if (back != (intptr_t)(int32_t)back || reach != (intptr_t)(int32_t)reach) { g_poseSkipped = NULL; Note("difficulty: extra large maps: the pose guard is out of reach"); return; }
    int32_t rel = (int32_t)back; memcpy(page + 12, &rel, 4);
    rel = (int32_t)((intptr_t)(page + 0x40) - (intptr_t)(page + 23)); memcpy(page + 19, &rel, 4);
    DWORD old = 0;
    if (!VirtualProtect(page, 0x1000, PAGE_EXECUTE_READWRITE, &old)) { g_poseSkipped = NULL; Note("difficulty: extra large maps: the pose guard is not executable"); return; }
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
    rel = (int32_t)reach; memcpy(jump + 1, &rel, 4);
    if (ApiWrite(exe + POSE_SITE, jump, sizeof jump)) { g_poseSkipped = NULL; Note("difficulty: extra large maps: the pose setter could not be patched"); return; }
    Note("difficulty: extra large maps: a pose for a unit that is gone is skipped (exe 0x1FCD00)");
}

/* The pose sync's batch (exe 0x79BCD0, main thread, before its jobs run 0x79C360) copies the scene's moved bodies
 * (S+0x230, 16 B {unit id, body, node, actor}) and compacts them at 0x79BD80..0x79BDB2: an entry stays when the scene's
 * [+0xC0](body) is 0 and its unit id is not 0. Every pose call (unit API +0x78 / +0x80 / +0x530) then looks the unit up
 * without a check (s83a exe+0x1FCD17, s84a exe+0x1FCDD0: the lookup 0x9D8C0 null). Here the compaction also drops an
 * entry whose unit no longer exists (0x9D8C0 null), so no job meets it; the first BODY_DEAD_KEPT are kept for the note.
 * The main thread waits inside the sync (0x79C1A0), so no unit goes between this check and the jobs. rsp is 16-aligned
 * at the site (entry 8 + 5 pushes + 0x250); rax, rcx, rdx, r8-r11 and xmm0 are free there (KB xl-world-streaming,
 * subagent deadbody; scratch ~/xl-work/static/deadbody/). */
#define BODY_SITE 0x79BD99u   /* cmp [rsi+rdi*8],eax; je 0x79BDAD (5 bytes; eax = 0 here) */
#define BODY_KEEP 0x79BD9Eu   /* movups xmm0,[rsi+rdi*8]: the entry is kept */
#define BODY_DROP 0x79BDADu   /* inc ebx: the next entry */
#define BODY_LOOKUP 0x9D8C0u
static const char kBodyPin[] = "85 C0 75 14 39 04 FE 74 0F 0F 10 04 FE 8B C5 48 03 C0 FF C5 0F 11 04 C6 FF C3 41 3B DE 75 CC";
static void InstallBodySyncGuard(uintptr_t exe)
{
    if (g_bodyDead != NULL) return;
    if (ApiCodeMatches(exe + BODY_SITE - 4, kBodyPin) != 1 || ApiCodeMatches(exe + BODY_LOOKUP, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20") != 1)
    { Note("difficulty: extra large maps: the pose sync's batch (exe 0x79BCD0) is not the reviewed one; bodies of units that are gone are not left out"); return; }
    uint8_t* page = AllocateNearGame(exe, 0x1000);
    if (page == NULL) { Note("difficulty: extra large maps: no room near the exe for the body sync guard"); return; }
    static const uint8_t code[] = {
        0x8B, 0x0C, 0xFE,                       /* +0  mov ecx,[rsi+rdi*8] (unit id) */
        0x85, 0xC9,                             /* +3  test ecx,ecx */
        0x0F, 0x84, 0, 0, 0, 0,                 /* +5  jz 0x79BDAD (ends +11) */
        0x48, 0x83, 0xEC, 0x20,                 /* +11 sub rsp,0x20 */
        0xE8, 0, 0, 0, 0,                       /* +15 call 0x9D8C0 (ends +20) */
        0x48, 0x83, 0xC4, 0x20,                 /* +20 add rsp,0x20 */
        0x48, 0x85, 0xC0,                       /* +24 test rax,rax */
        0x74, 0x05,                             /* +27 jz +34 */
        0xE9, 0, 0, 0, 0,                       /* +29 jmp 0x79BD9E (ends +34) */
        0xB8, 0x01, 0x00, 0x00, 0x00,           /* +34 mov eax,1 */
        0xF0, 0x0F, 0xC1, 0x05, 0, 0, 0, 0,     /* +39 lock xadd [rip+count],eax (ends +47) */
        0x83, 0xF8, BODY_DEAD_KEPT,             /* +47 cmp eax,16 */
        0x73, 0x12,                             /* +50 jae +70 */
        0xC1, 0xE0, 0x04,                       /* +52 shl eax,4 */
        0x48, 0x8D, 0x0D, 0, 0, 0, 0,           /* +55 lea rcx,[rip+kept] (ends +62) */
        0x0F, 0x10, 0x04, 0xFE,                 /* +62 movups xmm0,[rsi+rdi*8] */
        0x0F, 0x11, 0x04, 0x01,                 /* +66 movups [rcx+rax],xmm0 */
        0xE9, 0, 0, 0, 0};                      /* +70 jmp 0x79BDAD (ends +75) */
    _Static_assert(sizeof code == 75, "body sync guard size");
    memcpy(page, code, sizeof code);
    uint8_t* count = page + 0x100;
    uint8_t* kept = page + 0x200;
    memset(count, 0, 0x300);
    intptr_t rels[][2] = {
        {7, (intptr_t)(exe + BODY_DROP) - (intptr_t)(page + 11)},
        {16, (intptr_t)(exe + BODY_LOOKUP) - (intptr_t)(page + 20)},
        {30, (intptr_t)(exe + BODY_KEEP) - (intptr_t)(page + 34)},
        {43, (intptr_t)count - (intptr_t)(page + 47)},
        {58, (intptr_t)kept - (intptr_t)(page + 62)},
        {71, (intptr_t)(exe + BODY_DROP) - (intptr_t)(page + 75)},
        {-1, (intptr_t)page - (intptr_t)(exe + BODY_SITE + 5)}};
    for (size_t i = 0; i < sizeof rels / sizeof rels[0]; i++)
    {
        if (rels[i][1] != (intptr_t)(int32_t)rels[i][1]) { Note("difficulty: extra large maps: the body sync guard is out of reach"); return; }
        int32_t rel = (int32_t)rels[i][1];
        if (rels[i][0] >= 0) memcpy(page + rels[i][0], &rel, 4);
    }
    DWORD old = 0;
    if (!VirtualProtect(page, 0x1000, PAGE_EXECUTE_READWRITE, &old)) { Note("difficulty: extra large maps: the body sync guard is not executable"); return; }
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    uint8_t jump[5] = {0xE9, 0, 0, 0, 0};
    int32_t rel = (int32_t)rels[6][1]; memcpy(jump + 1, &rel, 4);
    g_bodyDeadSeen = (const uint32_t*)kept;
    g_bodyDead = (volatile LONG*)count;
    if (ApiWrite(exe + BODY_SITE, jump, sizeof jump)) { g_bodyDead = NULL; Note("difficulty: extra large maps: the pose sync's batch could not be patched"); return; }
    Note("difficulty: extra large maps: a moved body of a unit that is gone is left out of the pose sync (exe 0x79BD99)");
}

static const char* InstallDestroyQueue(void)
{
    if (g_destroyQueue != NULL) return NULL;
    uintptr_t exe = ApiModuleBase("helldivers2.exe", NULL);
    if (exe != 0) { InstallPoseGuard(exe); InstallBodySyncGuard(exe); }
    if (exe == 0 || ApiCodeMatches(exe + DESTROY_QUEUE, kDestroyQueuePin) != 1)
    { Note("difficulty: extra large maps: the destroy queue (exe 0x%X) is not the reviewed one; destroys are not checked", DESTROY_QUEUE); return NULL; }
    uint64_t lock = 0, unlock = 0;
    if (SafeRead(exe + DESTROY_LOCK_SHARED, &lock, 8) || SafeRead(exe + DESTROY_UNLOCK_SHARED, &unlock, 8) || lock == 0 || unlock == 0)
    { Note("difficulty: extra large maps: the unit table's lock imports do not read; destroys are not checked"); return NULL; }
    g_destroyLock = (DestroyLockFn)(uintptr_t)lock; g_destroyUnlock = (DestroyLockFn)(uintptr_t)unlock; g_destroyTableAt = exe + DESTROY_TABLE;
    if (LoaderHook("the engine's destroy queue (units not made yet wait)", exe + DESTROY_QUEUE, (void*)DestroyQueueDetour, (void**)&g_destroyQueue))
    { g_destroyQueue = NULL; Note("difficulty: extra large maps: the destroy queue could not be hooked"); return NULL; }
    Note("difficulty: extra large maps: destroys are checked first: a unit not made yet waits (up to %u frames), one gone is dropped", DESTROY_WAIT_TICKS);
    return NULL;
}
