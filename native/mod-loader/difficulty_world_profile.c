/* A sampling profiler for the extra large world's generation ops (difficulty_world.c: WorldDispatch wraps game.dll
 * 0xC14950). The generating thread never waits: WorldProfileBegin and WorldProfileEnd only publish the op, a sequence
 * number, the start tick, the running flag and (at End) the op's ms through a seqlock of interlocked stores, then return.
 * The sampler thread, created by the first Begin, polls every 10 ms; once the same op has run for 500 ms it samples it
 * (SuspendThread, GetThreadContext, ResumeThread at once, then the unwind and the tables) with Sleep(1) between samples.
 * When the sequence changes or the op ends it writes the final report (only for an op of 1000 ms or more) and clears
 * its own tables; while the op runs it writes an interim report after 20 s and every 60 s after that. Only the sampler
 * thread touches the tables.
 *
 * The generating thread never waits on the sampler: the sampler reads the published state with a seqlock, so a look
 * taken in the middle of Begin or End is simply discarded. If SuspendThread fails or GetThreadContext fails 50 times in
 * a row, sampling stops for that op and a single Note says so.
 *
 * The sleep between samples is Sleep(1). Whether that is about 1 ms or about 15 ms is not measured here, and
 * timeBeginPeriod is not available to the loader, so the sampling interval may be either.
 *
 * Included by hellforge_modloader.c before difficulty_world.c, whose WorldDispatch calls it: this file needs only Note,
 * SafeRead and ApiModuleBase, all defined earlier in the loader. */

#define WORLD_PROFILE_IPS 4096       /* distinct (module, offset) instruction pointers */
#define WORLD_PROFILE_CALLERS 4096   /* distinct game.dll caller offsets, each once per sample */
#define WORLD_PROFILE_DEPTH 6        /* return addresses walked per sample */
#define WORLD_PROFILE_TOP 15         /* entries in each report list */
#define WORLD_PROFILE_GAME 1         /* module in the IP table */
#define WORLD_PROFILE_EXE 2
#define WORLD_PROFILE_OTHER 3
#define WORLD_PROFILE_NO_CALLER 0xFFFFFFFFu /* an empty caller slot (game.dll is 64 MB, so never an offset) */
#define WORLD_PROFILE_POLL_MS 10             /* the idle poll; 1 ms between samples while sampling */
#define WORLD_PROFILE_SAMPLE_AFTER_MS 500ULL
#define WORLD_PROFILE_REPORT_AFTER_MS 1000.0
#define WORLD_PROFILE_INTERIM_MS 20000ULL
#define WORLD_PROFILE_INTERIM_EVERY_MS 60000ULL
#define WORLD_PROFILE_FAILURES 50            /* SuspendThread/GetThreadContext failures in a row */

typedef struct WorldProfileIp
{
    uint32_t module; /* WORLD_PROFILE_GAME, _EXE or _OTHER; 0 is an empty slot */
    uint32_t count;  /* samples that stopped at this instruction */
    uint64_t offset; /* the module offset, or the address itself for _OTHER */
} WorldProfileIp;

typedef struct WorldProfileCaller
{
    uint32_t offset; /* game.dll offset; WORLD_PROFILE_NO_CALLER when empty */
    uint32_t count;  /* samples that had it at any depth, once each */
} WorldProfileCaller;

/* What the generating thread publishes. Begin and End write these under the version seqlock with interlocked stores
 * only; nothing here is a table, and there is no event or handle for the generating thread to wait on. */
static struct
{
    volatile LONG version;      /* odd while Begin or End writes the fields below */
    volatile LONG sequence;     /* one more at each Begin: the op's identity until it is replaced */
    volatile LONG op;
    volatile LONG running;
    volatile LONG64 startTick;  /* GetTickCount64 at Begin */
    volatile LONG endSequence;  /* sequence of the last End whose ms was >= 1000 */
    volatile LONG64 endMsBits;  /* that End's ms as double bits */
    volatile LONG started;      /* the first Begin made the sampler and opened the target */
    HANDLE target;              /* the generating thread, opened once and kept */
    DWORD targetId;
    HANDLE thread;
} g_worldProfile;

/* The sampler's own state and tables. Only the sampler thread reads or writes any of this. */
static struct
{
    LONG sequence;              /* the op being tracked; 0 when none */
    uint32_t op;
    ULONGLONG start;
    int sampling;
    int failed;                 /* sampling stopped for this op after 50 failures */
    int failures;               /* consecutive SuspendThread/GetThreadContext failures */
    int failureNoted;
    ULONGLONG nextInterim;
    uintptr_t game;
    uintptr_t exe;
    uint32_t exeSize;
    uint32_t samples;           /* GetThreadContext successes */
    uint32_t ipUsed, callerUsed;
    uint32_t droppedIps, droppedCallers; /* a full table's entries */
    WorldProfileIp ips[WORLD_PROFILE_IPS];
    WorldProfileCaller callers[WORLD_PROFILE_CALLERS];
} g_worldSamples;

static void WorldProfileReport(uint32_t op, uint32_t samples); /* the two top lists */

static uint32_t WorldProfileHash(uint32_t module, uint64_t offset)
{
    uint64_t x = offset + ((uint64_t)module << 56);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return (uint32_t)x & (WORLD_PROFILE_IPS - 1);
}

static void WorldProfileIpAdd(uint32_t module, uint64_t offset)
{
    uint32_t slot = WorldProfileHash(module, offset);
    for (uint32_t step = 0; step < WORLD_PROFILE_IPS; step++, slot = (slot + 1) & (WORLD_PROFILE_IPS - 1))
    {
        WorldProfileIp* e = &g_worldSamples.ips[slot];
        if (e->module == 0)
        {
            e->module = module;
            e->offset = offset;
            e->count = 1;
            g_worldSamples.ipUsed++;
            return;
        }
        if (e->module == module && e->offset == offset)
        {
            e->count++;
            return;
        }
    }
    g_worldSamples.droppedIps++;
}

static void WorldProfileCallerAdd(uint32_t offset)
{
    uint32_t slot = WorldProfileHash(WORLD_PROFILE_GAME, offset);
    for (uint32_t step = 0; step < WORLD_PROFILE_CALLERS; step++, slot = (slot + 1) & (WORLD_PROFILE_CALLERS - 1))
    {
        WorldProfileCaller* e = &g_worldSamples.callers[slot];
        if (e->offset == WORLD_PROFILE_NO_CALLER)
        {
            e->offset = offset;
            e->count = 1;
            g_worldSamples.callerUsed++;
            return;
        }
        if (e->offset == offset)
        {
            e->count++;
            return;
        }
    }
    g_worldSamples.droppedCallers++;
}

/* Which module the address belongs to, with its offset in it. game.dll is the loader's fixed 64 MB window around its
 * base (ApiModuleBase's, the same as g_world.game); the exe is its PE image (SizeOfImage was read safely at the op's
 * start). Anything else is "other" and keeps its address. */
static uint32_t WorldProfileModule(uint64_t ip, uint64_t* offset)
{
    uint64_t game = (uint64_t)g_worldSamples.game;
    uint64_t exe = (uint64_t)g_worldSamples.exe;
    if (game != 0 && ip >= game && ip - game < 0x4000000ULL)
    {
        *offset = ip - game;
        return WORLD_PROFILE_GAME;
    }
    if (exe != 0 && ip >= exe && ip - exe < (uint64_t)g_worldSamples.exeSize)
    {
        *offset = ip - exe;
        return WORLD_PROFILE_EXE;
    }
    *offset = ip;
    return WORLD_PROFILE_OTHER;
}

/* The target's stack is read by RtlVirtualUnwind, after the target has been resumed. Checking that the return address
 * at RSP is readable stops a stale or wild frame before the unwinder faults. */
static int WorldProfileStackReadable(uint64_t rsp)
{
    uint64_t word = 0;
    if (rsp < 0x10000) return 0; /* not a user stack address; ReadProcessMemory would fail anyway */
    return SafeRead((uintptr_t)rsp, &word, sizeof word) == 0;
}

/* One sample: the IP and up to six return addresses, all from the copied context. The context is unwound with the
 * target's own stack while the target runs, so this is after ResumeThread. */
static void WorldProfileRecord(const CONTEXT* context)
{
    uint64_t offset = 0;
    uint32_t module = WorldProfileModule((uint64_t)context->Rip, &offset);
    g_worldSamples.samples++;
    WorldProfileIpAdd(module, offset);
    CONTEXT walk = *context;
    uint64_t control = (uint64_t)walk.Rip;
    uint32_t seen[WORLD_PROFILE_DEPTH];
    uint32_t seenCount = 0;
    for (uint32_t depth = 0; depth < WORLD_PROFILE_DEPTH; depth++)
    {
        uint64_t callerOffset = 0;
        if (WorldProfileModule(control, &callerOffset) == WORLD_PROFILE_OTHER) break;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry((DWORD64)control, &imageBase, NULL);
        if (function == NULL) break;
        if (!WorldProfileStackReadable((uint64_t)walk.Rsp)) break;
        PVOID handlerData = NULL;
        DWORD64 establisherFrame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, (DWORD64)control, function, &walk, &handlerData,
            &establisherFrame, NULL);
        uint64_t caller = (uint64_t)walk.Rip;
        if (caller == 0) break;
        uint32_t callerModule = WorldProfileModule(caller, &callerOffset);
        if (callerModule == WORLD_PROFILE_OTHER) break;
        if (callerModule == WORLD_PROFILE_GAME)
        {
            int duplicate = 0;
            for (uint32_t i = 0; i < seenCount; i++) if (seen[i] == (uint32_t)callerOffset) { duplicate = 1; break; }
            if (!duplicate)
            {
                if (seenCount < WORLD_PROFILE_DEPTH) seen[seenCount++] = (uint32_t)callerOffset;
                WorldProfileCallerAdd((uint32_t)callerOffset);
            }
        }
        control = caller;
    }
}

/* One attempt; 0 on a recorded sample, 1 when the target could not be suspended or its context was not read. */
static int WorldProfileSampleOnce(void)
{
    HANDLE target = g_worldProfile.target;
    if (target == NULL) return 1;
    CONTEXT context;
    memset(&context, 0, sizeof context);
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(target) == (DWORD)-1) return 1;
    BOOL got = GetThreadContext(target, &context); /* the only work done while the target is suspended */
    ResumeThread(target);
    if (!got) return 1;
    WorldProfileRecord(&context);
    return 0;
}

/* The report's two lists: the top entries by count, each line one entry. Only the sampler thread calls this. */
static void WorldProfileReport(uint32_t op, uint32_t samples)
{
    if (samples == 0) return;
    if (g_worldSamples.droppedIps != 0 || g_worldSamples.droppedCallers != 0)
        Note("difficulty: extra large maps: profile op 0x%X: %u ip and %u caller entries dropped (tables full)", op,
            (unsigned)g_worldSamples.droppedIps, (unsigned)g_worldSamples.droppedCallers);
    uint32_t shown[WORLD_PROFILE_TOP];
    uint32_t shownCount = 0;
    for (uint32_t rank = 0; rank < WORLD_PROFILE_TOP; rank++)
    {
        uint32_t best = 0, bestCount = 0;
        for (uint32_t i = 0; i < WORLD_PROFILE_IPS; i++)
        {
            const WorldProfileIp* e = &g_worldSamples.ips[i];
            if (e->module == 0) continue;
            int taken = 0;
            for (uint32_t k = 0; k < shownCount; k++) if (shown[k] == i) { taken = 1; break; }
            if (taken) continue;
            if (bestCount == 0 || e->count > bestCount) { best = i; bestCount = e->count; }
        }
        if (bestCount == 0) break;
        shown[shownCount++] = best;
        const WorldProfileIp* e = &g_worldSamples.ips[best];
        const char* name = e->module == WORLD_PROFILE_GAME ? "game" : e->module == WORLD_PROFILE_EXE ? "exe" : "other";
        Note("difficulty: extra large maps: profile op 0x%X: ip %u: %s+0x%llX %u samples (%.1f%%)", op,
            (unsigned)(rank + 1), name, (unsigned long long)e->offset, (unsigned)e->count,
            (double)e->count * 100.0 / (double)samples);
    }
    shownCount = 0;
    for (uint32_t rank = 0; rank < WORLD_PROFILE_TOP; rank++)
    {
        uint32_t best = 0, bestCount = 0;
        for (uint32_t i = 0; i < WORLD_PROFILE_CALLERS; i++)
        {
            const WorldProfileCaller* e = &g_worldSamples.callers[i];
            if (e->offset == WORLD_PROFILE_NO_CALLER) continue;
            int taken = 0;
            for (uint32_t k = 0; k < shownCount; k++) if (shown[k] == i) { taken = 1; break; }
            if (taken) continue;
            if (bestCount == 0 || e->count > bestCount) { best = i; bestCount = e->count; }
        }
        if (bestCount == 0) break;
        shown[shownCount++] = best;
        const WorldProfileCaller* e = &g_worldSamples.callers[best];
        Note("difficulty: extra large maps: profile op 0x%X: caller %u: game+0x%llX %u samples (%.1f%%)", op,
            (unsigned)(rank + 1), (unsigned long long)e->offset, (unsigned)e->count,
            (double)e->count * 100.0 / (double)samples);
    }
}

/* Only the sampler thread calls this, between ops or before a new one. */
static void WorldProfileClear(void)
{
    memset(g_worldSamples.ips, 0, sizeof g_worldSamples.ips);
    memset(g_worldSamples.callers, 0xFF, sizeof g_worldSamples.callers); /* every slot empty */
    g_worldSamples.samples = 0;
    g_worldSamples.ipUsed = 0;
    g_worldSamples.callerUsed = 0;
    g_worldSamples.droppedIps = 0;
    g_worldSamples.droppedCallers = 0;
}

/* The exe's PE header (its SizeOfImage), read through SafeRead: the loader does not link psapi. */
static void WorldProfileReadImages(void)
{
    g_worldSamples.game = ApiModuleBase("game.dll", NULL); /* the same base as g_world.game */
    uintptr_t exe = ApiModuleBase("helldivers2.exe", NULL);
    uint32_t size = 0;
    if (exe != 0)
    {
        IMAGE_DOS_HEADER dos;
        if (SafeRead(exe, &dos, sizeof dos) == 0 && dos.e_magic == IMAGE_DOS_SIGNATURE &&
            dos.e_lfanew > 0 && dos.e_lfanew < 0x1000)
        {
            uintptr_t nt = exe + (uint32_t)dos.e_lfanew;
            uint32_t signature = 0;
            uint32_t image = 0;
            size_t at = offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage);
            if (SafeRead(nt, &signature, sizeof signature) == 0 && signature == IMAGE_NT_SIGNATURE &&
                SafeRead(nt + at, &image, sizeof image) == 0) size = image;
        }
    }
    g_worldSamples.exe = exe;
    g_worldSamples.exeSize = size;
}

/* A consistent copy of what the generating thread published; 0 while Begin or End is in the middle of a write. */
typedef struct WorldProfileSnapshot
{
    LONG sequence;
    uint32_t op;
    ULONGLONG start;
    LONG running;
    LONG endSequence;
    double endMs;
} WorldProfileSnapshot;

static int WorldProfileReadSnapshot(WorldProfileSnapshot* snapshot)
{
    LONG before = InterlockedCompareExchange(&g_worldProfile.version, 0, 0);
    if ((before & 1) != 0) return 0;
    snapshot->sequence = InterlockedCompareExchange(&g_worldProfile.sequence, 0, 0);
    snapshot->op = (uint32_t)InterlockedCompareExchange(&g_worldProfile.op, 0, 0);
    snapshot->running = InterlockedCompareExchange(&g_worldProfile.running, 0, 0);
    snapshot->start = (ULONGLONG)InterlockedCompareExchange64(&g_worldProfile.startTick, 0, 0);
    snapshot->endSequence = InterlockedCompareExchange(&g_worldProfile.endSequence, 0, 0);
    LONG64 bits = InterlockedCompareExchange64(&g_worldProfile.endMsBits, 0, 0);
    memcpy(&snapshot->endMs, &bits, sizeof snapshot->endMs);
    LONG after = InterlockedCompareExchange(&g_worldProfile.version, 0, 0);
    return before == after && (after & 1) == 0;
}

/* Start tracking the op in the snapshot. Only the sampler thread runs this, so its tables are clear. */
static void WorldProfileTrack(const WorldProfileSnapshot* snapshot)
{
    g_worldSamples.sequence = snapshot->sequence;
    g_worldSamples.op = snapshot->op;
    g_worldSamples.start = snapshot->start;
    g_worldSamples.sampling = 0;
    g_worldSamples.failed = 0;
    g_worldSamples.failures = 0;
    g_worldSamples.failureNoted = 0;
    g_worldSamples.nextInterim = snapshot->start + WORLD_PROFILE_INTERIM_MS;
    WorldProfileClear();
    WorldProfileReadImages();
}

/* The op being tracked has ended (or a new one has replaced it): the final report, when End saw at least 1000 ms,
 * then the tables clear for the next op. Only the sampler thread runs this. */
static void WorldProfileFinalize(const WorldProfileSnapshot* snapshot)
{
    uint32_t samples = g_worldSamples.samples;
    if (snapshot->endSequence == g_worldSamples.sequence && snapshot->endMs >= WORLD_PROFILE_REPORT_AFTER_MS)
    {
        Note("difficulty: extra large maps: profile op 0x%X: %u samples, %.1f ms",
            g_worldSamples.op, (unsigned)samples, snapshot->endMs);
        if (samples != 0) WorldProfileReport(g_worldSamples.op, samples);
    }
    WorldProfileClear();
    g_worldSamples.sampling = 0;
    g_worldSamples.failed = 0;
    g_worldSamples.failures = 0;
    g_worldSamples.failureNoted = 0;
    g_worldSamples.sequence = 0;
}

/* The sampler's interim report for an op that runs long (or never returns): after 20 s, then every 60 s. The tables
 * keep accumulating, so the final report still has every sample. */
static void WorldProfileInterim(ULONGLONG now)
{
    ULONGLONG elapsed = now - g_worldSamples.start;
    uint32_t samples = g_worldSamples.samples;
    Note("difficulty: extra large maps: profile op 0x%X: still running after %llu s: %u samples, %.1f ms",
        g_worldSamples.op, (unsigned long long)(elapsed / 1000), (unsigned)samples, (double)elapsed);
    WorldProfileReport(g_worldSamples.op, samples);
    do
    {
        g_worldSamples.nextInterim += WORLD_PROFILE_INTERIM_EVERY_MS;
    } while (g_worldSamples.nextInterim <= now);
}

/* The sampler's loop body: one consistent look at the published op, then at most one sample. */
static void WorldProfilePoll(void)
{
    WorldProfileSnapshot snapshot = {0};
    if (!WorldProfileReadSnapshot(&snapshot)) return;
    if (snapshot.sequence == 0) return; /* no Begin yet */

    if (g_worldSamples.sequence != snapshot.sequence)
    {
        if (g_worldSamples.sequence != 0) WorldProfileFinalize(&snapshot);
        if (snapshot.running == 0) return;
        /* Finalize may have taken a little time: track the op that is running now, not the snapshot's stale one. */
        if (!WorldProfileReadSnapshot(&snapshot) || snapshot.running == 0) return;
        WorldProfileTrack(&snapshot);
    }
    else if (snapshot.running == 0)
    {
        WorldProfileFinalize(&snapshot);
        return;
    }
    if (g_worldSamples.sequence == 0 || snapshot.running == 0) return;

    ULONGLONG now = GetTickCount64();
    if (!g_worldSamples.sampling && !g_worldSamples.failed &&
        now - g_worldSamples.start >= WORLD_PROFILE_SAMPLE_AFTER_MS) g_worldSamples.sampling = 1;
    if (now >= g_worldSamples.nextInterim) WorldProfileInterim(now);
    if (!g_worldSamples.sampling) return;
    if (WorldProfileSampleOnce() == 0)
    {
        g_worldSamples.failures = 0;
        return;
    }
    g_worldSamples.failures++;
    if (g_worldSamples.failures < WORLD_PROFILE_FAILURES) return;
    g_worldSamples.sampling = 0;
    g_worldSamples.failed = 1;
    if (g_worldSamples.failureNoted) return;
    g_worldSamples.failureNoted = 1;
    Note("difficulty: extra large maps: profile op 0x%X: sampling stopped after %u suspend/context failures in a row",
        g_worldSamples.op, (unsigned)WORLD_PROFILE_FAILURES);
}

/* The one sampler thread: 10 ms polls while no op is being sampled, 1 ms between samples after 500 ms. */
static DWORD WINAPI WorldProfileSampler(void* unused)
{
    (void)unused;
    for (;;)
    {
        WorldProfilePoll();
        Sleep(g_worldSamples.sampling ? 1 : WORLD_PROFILE_POLL_MS);
    }
    return 0;
}

/* Called on the generating thread right before the op. The first call makes the sampler and opens the generating
 * thread's handle; every call then only publishes the op with interlocked stores and returns. */
static void WorldProfileBegin(uint32_t op)
{
    if (InterlockedCompareExchange(&g_worldProfile.started, 1, 0) == 0)
    {
        g_worldProfile.targetId = GetCurrentThreadId();
        g_worldProfile.target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE, g_worldProfile.targetId);
        g_worldProfile.thread = CreateThread(NULL, 0, WorldProfileSampler, NULL, 0, NULL);
    }
    InterlockedIncrement(&g_worldProfile.version);
    InterlockedIncrement(&g_worldProfile.sequence);
    InterlockedExchange(&g_worldProfile.op, (LONG)op);
    InterlockedExchange64(&g_worldProfile.startTick, (LONG64)GetTickCount64());
    InterlockedExchange(&g_worldProfile.running, 1);
    InterlockedIncrement(&g_worldProfile.version);
}

/* Called right after the op. It never waits: it publishes the op's end (and, for an op of a second or more, the ms the
 * sampler will use for the final report) and returns. */
static void WorldProfileEnd(uint32_t op, double ms)
{
    LONG64 bits;
    (void)op;
    memcpy(&bits, &ms, sizeof bits);
    InterlockedIncrement(&g_worldProfile.version);
    if (ms >= WORLD_PROFILE_REPORT_AFTER_MS)
    {
        InterlockedExchange64(&g_worldProfile.endMsBits, bits);
        InterlockedExchange(&g_worldProfile.endSequence,
            (LONG)InterlockedCompareExchange(&g_worldProfile.sequence, 0, 0));
    }
    InterlockedExchange(&g_worldProfile.running, 0);
    InterlockedIncrement(&g_worldProfile.version);
}
