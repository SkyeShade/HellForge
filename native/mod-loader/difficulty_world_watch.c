/* A write-watchpoint research tool (XL-4): find which game code writes a memory address every frame, first for the game
 * camera's local pose, so a later task can hold the camera high above the map for full-world screenshots. The address is
 * known only at runtime (the Lua runtime computes it from the game's Lua), so the Lua runtime calls these DLL exports
 * through LuaJIT's ffi:
 *
 *   int hellforge_research_watch_write(uint64_t address, uint32_t length, uint32_t hits, uint32_t thread_id)
 *       Arms a hardware write watchpoint (Dr3, local enable) on `address` for `length` bytes (1, 2, 4 or 8; the address
 *       must be aligned to it). Dr3 is free: crash_notes.c's FieldWatch uses Dr0-Dr2 and never sets Dr3 or its Dr7 local
 *       enable (L3, Dr7's bit 6), and its Dr6 claim mask is bits 0-2, so it passes this watch's traps. `hits` is clamped
 *       to 1..64.
 *       thread_id 0 arms every thread of this process; any other value arms only that thread. The arming runs on a
 *       short-lived helper thread (a thread cannot set its own context while it runs), which skips itself. Returns the
 *       number of threads armed, or:
 *         -1  length is not 1, 2, 4 or 8, or address is not aligned to it
 *         -2  no such thread in this process (or the thread snapshot failed)
 *         -3  the helper thread could not be created
 *         -4  the helper thread did not finish within 2 s (the watch may be partly armed: call the stop export)
 *         -5  no thread could be armed
 *         -6  the vectored exception handler could not be added
 *         -7  a write watch is already active (stop it first)
 *
 *   int hellforge_research_watch_stop(void)
 *       Clears the watchpoint on every thread it armed (again with a short-lived helper thread), writes one Note per
 *       distinct (writer, caller chain) with its count, and returns the number of hits recorded. It returns 0 when no
 *       watch is active. A stop whose helper fails, or whose handler could not finish, leaves the watch active; its
 *       handler still claims its Dr3 traps but records nothing, so the stop can be retried.
 *
 * The exception handler claims only EXCEPTION_SINGLE_STEP with Dr6 bit 3 (Dr3's) set while this file owns Dr3 in the
 * process. It records in fixed static tables only: no allocation and no Note inside the handler. It clears Dr6 and
 * continues; every other exception is passed on. `armed` is set when the handler is installed and kept for the process,
 * so a trap already in flight when a stop runs is still claimed (and then ignored) instead of reaching the game's own
 * handlers; nothing else in the loader uses Dr3.
 *
 * Included by hellforge_modloader.c after crash_notes.c (FieldWatch and its helpers exist by then); it uses Note,
 * SafeRead and ApiModuleBase from earlier in the loader. Nothing here runs until a Lua runtime calls it. */

#ifndef HF_LUA_API
#define HF_LUA_API __declspec(dllexport)
#endif

#define HF_WRITE_WATCH_SLOT 3        /* Dr3: FieldWatch uses Dr0-Dr2 */
#define HF_WRITE_WATCH_ENABLE ((DWORD64)1 << (2 * HF_WRITE_WATCH_SLOT)) /* L3: Dr7's bit 6 (L0/G0, L1/G1, ...) */
#define HF_WRITE_WATCH_GLOBAL ((DWORD64)1 << (2 * HF_WRITE_WATCH_SLOT + 1)) /* G3: bit 7; this watch is local */
#define HF_WRITE_WATCH_RW_SHIFT (16 + 4 * HF_WRITE_WATCH_SLOT)  /* RW3: Dr7's bits 28-29 */
#define HF_WRITE_WATCH_LEN_SHIFT (18 + 4 * HF_WRITE_WATCH_SLOT) /* LEN3: Dr7's bits 30-31 */
#define HF_WRITE_WATCH_CONTROL ((DWORD64)0xF << HF_WRITE_WATCH_RW_SHIFT) /* RW3 and LEN3 */
#define HF_WRITE_WATCH_MAX 64        /* recorded hits */
#define HF_WRITE_WATCH_DEPTH 4       /* return addresses recorded per hit */
#define HF_WRITE_WATCH_THREADS 512   /* threads that may be armed at once */
#define HF_WRITE_WATCH_NONE 0
#define HF_WRITE_WATCH_GAME 1
#define HF_WRITE_WATCH_EXE 2
#define HF_WRITE_WATCH_OTHER 3

typedef struct WriteWatchCaller
{
    uint32_t module; /* HF_WRITE_WATCH_GAME, _EXE or _OTHER */
    uint64_t offset; /* the module offset, or the address itself for _OTHER */
} WriteWatchCaller;

typedef struct WriteWatchHitRecord
{
    uint32_t module;
    DWORD thread;
    uint64_t offset;
    WriteWatchCaller callers[HF_WRITE_WATCH_DEPTH];
} WriteWatchHitRecord;

/* The job a helper thread runs. Only one export runs at a time (the lock), and the main thread waits for the helper
 * before reading its result. The helper writes the arm count or the stop counts, and the main thread resets them. */
typedef struct WriteWatchJob
{
    DWORD only;          /* 0: every thread; else just this one */
    uint64_t address;
    uint32_t length;
    int result;          /* threads armed, or a negative error code */
    int cleared, failed; /* stop: threads whose Dr3 was cleared, and those that could not be */
} WriteWatchJob;

static struct
{
    volatile LONG armed;     /* the handler is installed: this file owns Dr3 in this process */
    volatile LONG active;    /* a watch is installed (stop clears it) */
    volatile LONG recording; /* the handler may record hits */
    volatile LONG count;     /* hit slots reserved, at most cap */
    volatile LONG cap;       /* hits to record, 1..HF_WRITE_WATCH_MAX */
    volatile LONG inHandler; /* handlers between their claim and their return */
    uint64_t address;
    uint32_t length;
    uint64_t game; /* game.dll's base (a 0x4000000 window), for module resolution */
    uint64_t exe;  /* helldivers2.exe's base and PE SizeOfImage */
    uint32_t exeSize;
    int threadCount;
    DWORD threads[HF_WRITE_WATCH_THREADS];
    WriteWatchHitRecord hits[HF_WRITE_WATCH_MAX];
    volatile LONG ready[HF_WRITE_WATCH_MAX]; /* slot filled; the handler sets it last */
} g_writeWatch;

static WriteWatchJob g_writeWatchJob;
static PVOID g_writeWatchHandler;
static SRWLOCK g_writeWatchLock = SRWLOCK_INIT;

/* Dr7's LEN bits for a hardware watchpoint length (00 1 byte, 01 2, 10 8, 11 4). The export checks the length; this is
 * only called with 1, 2, 4 or 8. */
static DWORD64 WriteWatchLengthBits(uint32_t length)
{
    switch (length)
    {
        case 1: return 0;
        case 2: return 1;
        case 8: return 2;
        default: return 3; /* 4 bytes */
    }
}

/* Dr3 on one thread, set while it is briefly suspended: 1 when it was set. The other debug registers and Dr7's other
 * fields stay as they were (FieldWatch's Dr0-Dr2 included). */
static int WriteWatchSetThread(DWORD id, uint64_t address, uint32_t length)
{
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, id);
    if (thread == NULL) return 0;
    int set = 0;
    if (SuspendThread(thread) != (DWORD)-1)
    {
        CONTEXT context;
        memset(&context, 0, sizeof context);
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context))
        {
            context.Dr3 = (DWORD64)address;
            context.Dr7 &= ~HF_WRITE_WATCH_GLOBAL;
            context.Dr7 = (context.Dr7 & ~HF_WRITE_WATCH_CONTROL) | ((DWORD64)1 << HF_WRITE_WATCH_RW_SHIFT) | /* RW3 = 01: write */
                (WriteWatchLengthBits(length) << HF_WRITE_WATCH_LEN_SHIFT) | HF_WRITE_WATCH_ENABLE; /* LEN3, L3 local enable */
            if (SetThreadContext(thread, &context)) set = 1;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return set;
}

/* Dr3 off one thread, set while it is briefly suspended: 1 when it was cleared. Only Dr3, its Dr7 enable bits and its
 * RW/LEN fields go; every other debug register stays (FieldWatch's included). */
static int WriteWatchClearThread(DWORD id)
{
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, id);
    if (thread == NULL) return 0;
    int cleared = 0;
    if (SuspendThread(thread) != (DWORD)-1)
    {
        CONTEXT context;
        memset(&context, 0, sizeof context);
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context))
        {
            context.Dr3 = 0;
            context.Dr7 &= ~HF_WRITE_WATCH_CONTROL;
            context.Dr7 &= ~(HF_WRITE_WATCH_ENABLE | HF_WRITE_WATCH_GLOBAL);
            if (SetThreadContext(thread, &context)) cleared = 1;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return cleared;
}

/* The arm helper (started by hellforge_research_watch_write): arm Dr3 on the requested threads, skipping itself. The
 * snapshot includes the helper, so it is removed by id before any target is opened. An armed id is appended as soon as
 * its SetThreadContext succeeds; no stop can run until this helper has ended, so the list is complete before a stop
 * reads it. */
static DWORD WINAPI WriteWatchArmHelper(void* unused)
{
    (void)unused;
    int armed = 0, found = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { g_writeWatchJob.result = -2; return 0; }
    THREADENTRY32 entry = {sizeof entry};
    DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId(), only = g_writeWatchJob.only;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        if (only != 0 && entry.th32ThreadID != only) continue;
        found = 1;
        if (g_writeWatch.threadCount >= HF_WRITE_WATCH_THREADS) break;
        if (WriteWatchSetThread(entry.th32ThreadID, g_writeWatchJob.address, g_writeWatchJob.length))
        {
            g_writeWatch.threads[g_writeWatch.threadCount++] = entry.th32ThreadID;
            armed++;
        }
    }
    CloseHandle(snapshot);
    if (only != 0 && !found) g_writeWatchJob.result = -2;
    else if (armed == 0) g_writeWatchJob.result = -5;
    else g_writeWatchJob.result = armed;
    return 0;
}

/* The stop helper: clear Dr3 on every thread the arm helper recorded. A thread that has ended cannot be opened and
 * counts as failed (its id can be reused, but a new thread's Dr3 is not ours to keep). */
static DWORD WINAPI WriteWatchStopHelper(void* unused)
{
    (void)unused;
    int cleared = 0, failed = 0;
    DWORD self = GetCurrentThreadId();
    for (int i = 0; i < g_writeWatch.threadCount; i++)
    {
        /* An armed thread may have ended and its id been reused by this helper: that is not our Dr3 to clear. */
        if (g_writeWatch.threads[i] == self) continue;
        if (WriteWatchClearThread(g_writeWatch.threads[i])) cleared++;
        else failed++;
    }
    g_writeWatchJob.cleared = cleared;
    g_writeWatchJob.failed = failed;
    return 0;
}

/* Which module an address belongs to, with its offset in it. game.dll is the loader's fixed 64 MB window around its
 * base (ApiModuleBase's); the exe is its PE image size, read at arm time. Anything else is "other" and keeps the
 * address. */
static uint32_t WriteWatchModule(uint64_t ip, uint64_t* offset)
{
    uint64_t game = g_writeWatch.game;
    uint64_t exe = g_writeWatch.exe;
    if (game != 0 && ip >= game && ip - game < 0x4000000ULL)
    {
        *offset = ip - game;
        return HF_WRITE_WATCH_GAME;
    }
    if (exe != 0 && ip >= exe && ip - exe < (uint64_t)g_writeWatch.exeSize)
    {
        *offset = ip - exe;
        return HF_WRITE_WATCH_EXE;
    }
    *offset = ip;
    return HF_WRITE_WATCH_OTHER;
}

/* The target's stack is read by RtlVirtualUnwind. Checking that the return address at RSP is readable stops a stale or
 * wild frame before the unwinder faults (the profiler's check). */
static int WriteWatchStackReadable(uint64_t rsp)
{
    uint64_t word = 0;
    if (rsp < 0x10000) return 0; /* not a user stack address; ReadProcessMemory would fail anyway */
    return SafeRead((uintptr_t)rsp, &word, sizeof word) == 0;
}

/* One hit's record, filled on the handler's stack and copied into its reserved slot. The writer's module and offset are
 * the instruction after the write (the CPU reports that). Up to four return addresses are walked from a copy of the
 * exception's context with the same RtlVirtualUnwind loop as the profiler; a frame outside the game stops the walk. */
static void WriteWatchRecord(const CONTEXT* context, LONG slot)
{
    WriteWatchHitRecord hit;
    memset(&hit, 0, sizeof hit);
    hit.module = WriteWatchModule((uint64_t)context->Rip, &hit.offset);
    hit.thread = GetCurrentThreadId();
    CONTEXT walk = *context;
    uint64_t control = (uint64_t)walk.Rip;
    int depth = 0;
    for (int i = 0; i < HF_WRITE_WATCH_DEPTH; i++)
    {
        uint64_t unused = 0;
        if (WriteWatchModule(control, &unused) == HF_WRITE_WATCH_OTHER) break;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry((DWORD64)control, &imageBase, NULL);
        if (function == NULL) break;
        if (!WriteWatchStackReadable((uint64_t)walk.Rsp)) break;
        PVOID handlerData = NULL;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, (DWORD64)control, function, &walk, &handlerData, &frame, NULL);
        uint64_t caller = (uint64_t)walk.Rip;
        if (caller == 0) break;
        uint64_t callerOffset = 0;
        uint32_t callerModule = WriteWatchModule(caller, &callerOffset);
        if (callerModule == HF_WRITE_WATCH_OTHER) break;
        hit.callers[depth].module = callerModule;
        hit.callers[depth].offset = callerOffset;
        depth++;
        control = caller;
    }
    g_writeWatch.hits[slot] = hit;
    InterlockedExchange(&g_writeWatch.ready[slot], 1);
}

/* The write watch's vectored handler: only Dr3's single-step (Dr6 bit 3) while this file owns Dr3 is claimed. The slot
 * is reserved with a compare-exchange so concurrent hits get distinct records; the record is published by `ready`.
 * Clear Dr6 and continue, recording or not. No allocation and no Note run here. */
static LONG CALLBACK WriteWatchHit(EXCEPTION_POINTERS* info)
{
    CONTEXT* context = info->ContextRecord;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if ((context->Dr6 & ((DWORD64)1 << HF_WRITE_WATCH_SLOT)) == 0) return EXCEPTION_CONTINUE_SEARCH;
    if (!g_writeWatch.armed) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&g_writeWatch.inHandler);
    if (InterlockedCompareExchange(&g_writeWatch.recording, 0, 0) != 0)
    {
        LONG slot = -1;
        for (;;)
        {
            LONG count = g_writeWatch.count;
            if (count >= g_writeWatch.cap) break;
            if (InterlockedCompareExchange(&g_writeWatch.count, count + 1, count) == count) { slot = count; break; }
        }
        if (slot >= 0)
        {
            WriteWatchRecord(context, slot);
            if (slot + 1 >= g_writeWatch.cap) InterlockedExchange(&g_writeWatch.recording, 0); /* the log is full */
        }
    }
    context->Dr6 = 0;
    InterlockedDecrement(&g_writeWatch.inHandler);
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* A module and offset as "game.dll+0x...", "helldivers2.exe+0x..." or the raw "0x..." (for an address outside both).
 * Called from the stop's report only. */
static void WriteWatchAddressText(uint32_t module, uint64_t offset, char* out, size_t size)
{
    if (module == HF_WRITE_WATCH_GAME) snprintf(out, size, "game.dll+0x%llx", (unsigned long long)offset);
    else if (module == HF_WRITE_WATCH_EXE) snprintf(out, size, "helldivers2.exe+0x%llx", (unsigned long long)offset);
    else snprintf(out, size, "0x%llx", (unsigned long long)offset);
}

/* Whether two hits have the same writer offset and caller chain (the stop's grouping key). */
static int WriteWatchSameChain(const WriteWatchHitRecord* a, const WriteWatchHitRecord* b)
{
    if (a->module != b->module || a->offset != b->offset) return 0;
    for (int i = 0; i < HF_WRITE_WATCH_DEPTH; i++)
        if (a->callers[i].module != b->callers[i].module || a->callers[i].offset != b->callers[i].offset) return 0;
    return 1;
}

/* " <- game.dll+0x..." per caller, into out (size bytes): a hit's caller chain. */
static void WriteWatchChainText(const WriteWatchHitRecord* hit, char* out, size_t size)
{
    int length = 0;
    out[0] = 0;
    for (int i = 0; i < HF_WRITE_WATCH_DEPTH; i++)
    {
        char name[64];
        if (hit->callers[i].module == HF_WRITE_WATCH_NONE) break;
        WriteWatchAddressText(hit->callers[i].module, hit->callers[i].offset, name, sizeof name);
        if ((size_t)length >= size - 1) break;
        int more = snprintf(out + length, size - (size_t)length, " <- %s", name);
        if (more < 0) break;
        length += more;
        if ((size_t)length >= size) { out[size - 1] = 0; break; }
    }
}

/* The stop's report: a summary, then one Note per distinct (writer, caller chain) with its count and first thread.
 * Returns the number of published hit records. Runs on the Lua thread after the slots are off. */
static int WriteWatchReport(void)
{
    int records = 0;
    for (int i = 0; i < HF_WRITE_WATCH_MAX; i++) if (g_writeWatch.ready[i] != 0) records++;
    Note("research: write watch 0x%llx (%u bytes): %d hit(s) recorded", (unsigned long long)g_writeWatch.address,
        g_writeWatch.length, records);
    int groups[HF_WRITE_WATCH_MAX] = {0};
    int counts[HF_WRITE_WATCH_MAX] = {0};
    int groupCount = 0;
    for (int i = 0; i < HF_WRITE_WATCH_MAX; i++)
    {
        if (g_writeWatch.ready[i] == 0) continue;
        const WriteWatchHitRecord* hit = &g_writeWatch.hits[i];
        int group = 0;
        while (group < groupCount && !WriteWatchSameChain(&g_writeWatch.hits[groups[group]], hit)) group++;
        if (group == groupCount)
        {
            if (groupCount >= HF_WRITE_WATCH_MAX) break;
            groups[groupCount] = i;
            counts[groupCount] = 0;
            groupCount++;
        }
        counts[group]++;
    }
    for (int group = 0; group < groupCount; group++)
    {
        const WriteWatchHitRecord* hit = &g_writeWatch.hits[groups[group]];
        char writer[96], chain[512];
        WriteWatchAddressText(hit->module, hit->offset, writer, sizeof writer);
        WriteWatchChainText(hit, chain, sizeof chain);
        Note("research: write watch: %s%s: %d hit(s) (first on thread %lu)", writer, chain, counts[group],
            (unsigned long)hit->thread);
    }
    return records;
}

/* helldivers2.exe's PE SizeOfImage, read through SafeRead like the profiler (the loader does not link psapi). */
static uint32_t WriteWatchExeSize(uintptr_t exe)
{
    uint32_t size = 0;
    if (exe == 0) return 0;
    IMAGE_DOS_HEADER dos;
    if (SafeRead(exe, &dos, sizeof dos) == 0 && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 0x1000)
    {
        uintptr_t nt = exe + (uint32_t)dos.e_lfanew;
        uint32_t signature = 0, image = 0;
        size_t at = offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage);
        if (SafeRead(nt, &signature, sizeof signature) == 0 && signature == IMAGE_NT_SIGNATURE &&
            SafeRead(nt + at, &image, sizeof image) == 0) size = image;
    }
    return size;
}

/* Arm the write watch. See the file header for the return values. */
HF_LUA_API int hellforge_research_watch_write(uint64_t address, uint32_t length, uint32_t hits, uint32_t thread_id)
{
    if (length != 1 && length != 2 && length != 4 && length != 8)
    {
        Note("research: write watch: length %u is not 1, 2, 4 or 8", length);
        return -1;
    }
    if ((address & (uint64_t)(length - 1)) != 0)
    {
        Note("research: write watch: 0x%llx is not aligned to %u bytes", (unsigned long long)address, length);
        return -1;
    }
    AcquireSRWLockExclusive(&g_writeWatchLock);
    if (g_writeWatch.active)
    {
        ReleaseSRWLockExclusive(&g_writeWatchLock);
        Note("research: write watch: one is already active; stop it first");
        return -7;
    }
    if (g_writeWatchHandler == NULL)
    {
        g_writeWatchHandler = AddVectoredExceptionHandler(1, WriteWatchHit);
        if (g_writeWatchHandler == NULL)
        {
            ReleaseSRWLockExclusive(&g_writeWatchLock);
            Note("research: write watch: the exception handler could not be added");
            return -6;
        }
        g_writeWatch.armed = 1;
    }
    if (hits < 1) hits = 1;
    if (hits > HF_WRITE_WATCH_MAX) hits = HF_WRITE_WATCH_MAX;
    g_writeWatch.address = address;
    g_writeWatch.length = length;
    g_writeWatch.cap = (LONG)hits;
    g_writeWatch.count = 0;
    g_writeWatch.threadCount = 0;
    InterlockedExchange(&g_writeWatch.inHandler, 0);
    for (int i = 0; i < HF_WRITE_WATCH_MAX; i++)
    {
        g_writeWatch.ready[i] = 0;
        memset(&g_writeWatch.hits[i], 0, sizeof g_writeWatch.hits[i]);
    }
    g_writeWatch.game = (uint64_t)ApiModuleBase("game.dll", NULL);
    g_writeWatch.exe = (uint64_t)ApiModuleBase("helldivers2.exe", NULL);
    g_writeWatch.exeSize = WriteWatchExeSize((uintptr_t)g_writeWatch.exe);
    g_writeWatchJob.only = (DWORD)thread_id;
    g_writeWatchJob.address = address;
    g_writeWatchJob.length = length;
    g_writeWatchJob.result = -5;
    g_writeWatchJob.cleared = g_writeWatchJob.failed = 0;
    g_writeWatch.recording = 1;
    g_writeWatch.active = 1;
    MemoryBarrier(); /* the state above is whole before any target thread can trap */
    HANDLE helper = CreateThread(NULL, 0, WriteWatchArmHelper, NULL, 0, NULL);
    if (helper == NULL)
    {
        g_writeWatch.recording = 0;
        g_writeWatch.active = 0;
        ReleaseSRWLockExclusive(&g_writeWatchLock);
        Note("research: write watch: the helper thread could not be created (%lu)", (unsigned long)GetLastError());
        return -3;
    }
    DWORD waited = WaitForSingleObject(helper, 2000);
    CloseHandle(helper);
    int result = waited == WAIT_OBJECT_0 ? g_writeWatchJob.result : -4;
    if (result > 0)
    {
        Note("research: write watch 0x%llx (%u bytes, up to %u hit(s)): %d thread(s) armed",
            (unsigned long long)address, length, hits, result);
    }
    else
    {
        g_writeWatch.recording = 0;
        if (result != -4)
        {
            g_writeWatch.active = 0; /* the helper ended; nothing was armed */
        }
        Note("research: write watch 0x%llx: not armed (%d)%s", (unsigned long long)address, result,
            result == -4 ? "; the helper may still be arming threads: call the stop export" : "");
    }
    ReleaseSRWLockExclusive(&g_writeWatchLock);
    return result;
}

/* Disarm the write watch and report its hits. The return is always the number of recorded hits (0 when nothing is or
 * was active); failure to disarm is noted and leaves the watch active. See the file header. */
HF_LUA_API int hellforge_research_watch_stop(void)
{
    AcquireSRWLockExclusive(&g_writeWatchLock);
    if (!g_writeWatch.active)
    {
        ReleaseSRWLockExclusive(&g_writeWatchLock);
        return 0;
    }
    InterlockedExchange(&g_writeWatch.recording, 0);
    for (int i = 0; i < 500 && g_writeWatch.inHandler != 0; i++) Sleep(1); /* let a record in hand finish */
    int busy = g_writeWatch.inHandler != 0;
    if (busy) Note("research: write watch: a hit was still being recorded; the watch stays active");
    g_writeWatchJob.cleared = g_writeWatchJob.failed = 0;
    HANDLE helper = CreateThread(NULL, 0, WriteWatchStopHelper, NULL, 0, NULL);
    if (helper == NULL)
    {
        Note("research: write watch: the disarm helper could not be created (%lu); the watch stays armed", (unsigned long)GetLastError());
    }
    else
    {
        DWORD waited = WaitForSingleObject(helper, 2000);
        CloseHandle(helper);
        if (waited != WAIT_OBJECT_0)
        {
            Note("research: write watch: the disarm helper did not finish within 2 s; the watch stays armed");
        }
        else
        {
            if (!busy) g_writeWatch.active = 0;
            Note("research: write watch: disarmed %d thread(s), %d could not be cleared", g_writeWatchJob.cleared, g_writeWatchJob.failed);
        }
    }
    int hits = WriteWatchReport();
    ReleaseSRWLockExclusive(&g_writeWatchLock);
    return hits;
}
