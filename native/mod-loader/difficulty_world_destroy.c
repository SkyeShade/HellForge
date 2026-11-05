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
#define DESTROY_WAITING 4096
#define DESTROY_WAIT_TICKS 600u
static const char kDestroyQueuePin[] = "40 56 41 54 41 55 41 56 41 57 48 83 EC 20 45 8B E0 4C 8B F2 4C 8B E9 45 8B F8 F0 44 0F C1 B9 30 00 01 00";

typedef void (*DestroyQueueFn)(uint64_t world, uint32_t* handles, uint32_t n);
typedef void (*DestroyLockFn)(void* lock);
static DestroyQueueFn g_destroyQueue;
static DestroyLockFn g_destroyLock, g_destroyUnlock;
static uintptr_t g_destroyTableAt;
static SRWLOCK g_destroyWaitLock = SRWLOCK_INIT;
static struct { uint64_t world; uint32_t handle, ticks; uintptr_t level; } g_destroyWait[DESTROY_WAITING];
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

static void DestroyQueueDetour(uint64_t world, uint32_t* handles, uint32_t n)
{
    if (handles == NULL || n == 0) { g_destroyQueue(world, handles, n); return; }
    uint32_t i = 0;
    while (i < n && DestroyHandleState(handles[i]) == 0) i++;
    if (i == n) { g_destroyQueue(world, handles, n); return; } /* the usual case: all of them exist */
    uint32_t pass[256], k = 0;
    void* caller = __builtin_return_address(0);
    for (i = 0; i < n; i++)
    {
        int state = DestroyHandleState(handles[i]);
        if (state == 0) { pass[k++] = handles[i]; if (k == 256) { g_destroyQueue(world, pass, k); k = 0; } continue; }
        if (InterlockedIncrement(&g_destroyNotes) <= 8)
            Note("difficulty: extra large maps: a destroy asked for unit 0x%08X, which %s (caller %p); %s", handles[i], state == 1 ? "is not made yet" : "is gone",
                caller, state == 1 ? "it waits until it is" : "dropped");
        if (state == 2) { InterlockedIncrement(&g_destroyStale); continue; }
        AcquireSRWLockExclusive(&g_destroyWaitLock);
        if (g_destroyWaiting < DESTROY_WAITING)
        {
            g_destroyWait[g_destroyWaiting].world = world; g_destroyWait[g_destroyWaiting].handle = handles[i];
            g_destroyWait[g_destroyWaiting].ticks = 0; g_destroyWait[g_destroyWaiting].level = g_worldLevel;
            g_destroyWaiting++; InterlockedIncrement(&g_destroyHeld);
        }
        else InterlockedIncrement(&g_destroyFull);
        ReleaseSRWLockExclusive(&g_destroyWaitLock);
    }
    if (k) g_destroyQueue(world, pass, k);
}

/* Each frame on the game thread (WorldStreamTick): the waiting handles that resolve now go to the original. */
static void DestroyTick(void)
{
    if (g_destroyQueue == NULL || g_destroyWaiting == 0) return;
    AcquireSRWLockExclusive(&g_destroyWaitLock);
    for (LONG j = 0; j < g_destroyWaiting;)
    {
        int state = g_destroyWait[j].level != g_worldLevel ? 2 : DestroyHandleState(g_destroyWait[j].handle);
        if (state == 1 && ++g_destroyWait[j].ticks <= DESTROY_WAIT_TICKS) { j++; continue; }
        if (state == 0) { uint32_t h = g_destroyWait[j].handle; g_destroyQueue(g_destroyWait[j].world, &h, 1); g_destroyPassed++; }
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
    if (memcmp(now, last, sizeof now) == 0 || (now[0] == 0 && now[2] == 0 && now[3] == 0)) return;
    memcpy(last, now, sizeof now);
    Note("difficulty: extra large maps: destroys of units not made yet: %ld held (%ld passed on once made, %ld waited too long, %ld waiting), %ld of units gone dropped, %ld not held (list full)",
            (long)g_destroyHeld, (long)g_destroyPassed, (long)g_destroyTimedOut, (long)g_destroyWaiting, (long)g_destroyStale, (long)g_destroyFull);
}

static const char* InstallDestroyQueue(void)
{
    if (g_destroyQueue != NULL) return NULL;
    uintptr_t exe = ApiModuleBase("helldivers2.exe", NULL);
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
