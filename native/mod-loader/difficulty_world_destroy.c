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

static void DestroyQueueDetour(uint64_t world, uint32_t* handles, uint32_t n)
{
    if (handles == NULL || n == 0) { g_destroyQueue(world, handles, n); return; }
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
