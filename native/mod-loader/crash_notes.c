/* Crash and hang diagnostics that are not a behaviour's (from behaviours.c): a freeze of the game's clock, any access
 * violation or illegal instruction, a fail-fast exit, and the war table's map preview watchpoints. Each logs and leaves
 * the game's own handling as it was. Included by hellforge_modloader.c after behaviours.c. */

/* The game's two code modules: game.dll and helldivers2.exe, by their address ranges. */
typedef struct GameModules { uintptr_t game; size_t gameSize; uintptr_t exe; size_t exeSize; } GameModules;

/* Diagnostics for a hang anywhere: when the game's clock stops for 3 s after it has run, every thread executing in
 * game code is briefly suspended and logged (its position and the game return addresses on its stack), once per stop. */
static int InGame(uintptr_t at, uintptr_t game, size_t gameSize, uintptr_t exe, size_t exeSize, const char** module, uintptr_t* offset)
{
    if (at >= game && at < game + gameSize) { *module = "game.dll"; *offset = at - game; return 1; }
    if (at >= exe && at < exe + exeSize) { *module = "helldivers2.exe"; *offset = at - exe; return 1; }
    return 0;
}

static size_t ExeSize(uintptr_t exe)
{
    return ((IMAGE_NT_HEADERS64*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew))->OptionalHeader.SizeOfImage;
}

static GameModules CurrentModules(void)
{
    GameModules g = {0};
    g.game = ApiModuleBase("game.dll", &g.gameSize);
    g.exe = (uintptr_t)GetModuleHandleA(NULL);
    g.exeSize = ExeSize(g.exe);
    return g;
}

/* The game addresses among a stack's first `count` values (at most `most`), " g+0x..." or " x+0x..." each, into trail
 * (`size` bytes, ending in 0): its length. */
static int GameTrail(const uintptr_t* stack, int count, int most, const GameModules* g, char* trail, int size)
{
    int n = 0;
    for (int k = 0, found = 0; k < count && found < most && n < size - 40; k++)
    {
        const char* m;
        uintptr_t o;
        if (InGame(stack[k], g->game, g->gameSize, g->exe, g->exeSize, &m, &o)) { n += snprintf(trail + n, size - n, " %s+0x%llx", m[0] == 'g' ? "g" : "x", (unsigned long long)o); found++; }
    }
    trail[n] = 0;
    return n;
}

/* The loaded module an address lies in, as "name+0xoffset" (0 when it lies in none): the crash note names the code
 * outside the game a fault came from (2026-10-21: a host's render thread called address 0 from a module at a fixed
 * address in two sessions; the note could not say which). */
static int ModuleAt(uintptr_t at, char* out, size_t size)
{
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    if (at < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at, &module) ||
        module == NULL || !GetModuleFileNameW(module, path, MAX_PATH)) return 0;
    const wchar_t* name = wcsrchr(path, L'\\');
    snprintf(out, size, "%ls+0x%llx", name != NULL ? name + 1 : path, (unsigned long long)(at - (uintptr_t)module));
    return 1;
}

/* A thread at a freeze, briefly suspended: logged (1) when it is executing in game code, with the game addresses on its
 * stack. */
static int FreezeNote(DWORD id, const GameModules* g)
{
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id);
    if (thread == NULL) return 0;
    CONTEXT context = {0};
    context.ContextFlags = CONTEXT_CONTROL;
    uint64_t stack[96] = {0};
    int got = 0;
    if (SuspendThread(thread) != (DWORD)-1)
    {
        if (GetThreadContext(thread, &context)) { got = 1; SafeRead(context.Rsp, stack, sizeof stack); }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    const char* module;
    uintptr_t offset;
    if (!got || !InGame(context.Rip, g->game, g->gameSize, g->exe, g->exeSize, &module, &offset)) return 0;
    char trail[512];
    GameTrail(stack, 96, 10, g, trail, (int)sizeof trail);
    Note("freeze: thread %lu at %s+0x%llx; stack:%s", id, module, (unsigned long long)offset, trail);
    return 1;
}

static void FreezeWatch(void)
{
    static uint64_t lastClock;
    static ULONGLONG lastChange;
    static int ran, reported;
    GameModules g = {0};
    uintptr_t clock = 0;
    g.game = ApiModuleBase("game.dll", &g.gameSize);
    uint64_t value = 0;
    if (g.game == 0 || SafeRead(g.game + BHV_CLOCK, &clock, 8) || clock == 0 || SafeRead(clock + 0x18, &value, 8)) return;
    ULONGLONG now = GetTickCount64();
    if (value != lastClock) { if (lastClock != 0) ran = 1; lastClock = value; lastChange = now; reported = 0; return; }
    if (!ran || reported || now - lastChange < 3000) return;
    reported = 1;
    g.exe = (uintptr_t)GetModuleHandleA(NULL);
    g.exeSize = ExeSize(g.exe);
    Note("freeze: the game's clock has not moved for %llu ms; threads in game code:", (unsigned long long)(now - lastChange));
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry = {sizeof entry};
    DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    int logged = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more && logged < 12; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        logged += FreezeNote(entry.th32ThreadID, &g);
    }
    CloseHandle(snapshot);
}

/* Any access violation or illegal instruction: where it happened (game.dll / exe offset or raw), what it read, the main
 * registers and the game addresses on the stack, then left to the game's own handling. The game's crash reporter does
 * not always get to write its dump; this is the record the loader keeps. The first 4 only. */
/* Diagnostics (the launcher's Crash dump setting: HELLFORGE_CRASH_DUMP names HellForge.Cli.exe): a full memory dump of the
 * game at its first crash, beside the session's logs. It is written from outside (HellForge.Cli crash-dump), while this
 * thread waits: a process dumping itself from its exception handler can deadlock on a lock a suspended thread holds. */
static void WriteCrashDump(EXCEPTION_POINTERS* info)
{
    wchar_t cli[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_CRASH_DUMP", cli, MAX_PATH);
    if (length == 0 || length >= MAX_PATH || g_logs[0] == 0) return;
    wchar_t command[3 * MAX_PATH];
    swprintf(command, sizeof command / sizeof command[0], L"\"%ls\" crash-dump --pid %lu --thread %lu --pointers %llx --out \"%ls\\crash-%lu.dmp\"",
        cli, GetCurrentProcessId(), GetCurrentThreadId(), (unsigned long long)(uintptr_t)info, g_logs, GetCurrentProcessId());
    STARTUPINFOW startup = {sizeof startup};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(cli, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process))
    {
        Note("crash dump: could not start %ls (%lu)", cli, GetLastError());
        return;
    }
    DWORD waited = WaitForSingleObject(process.hProcess, 120000), exit = 1;
    GetExitCodeProcess(process.hProcess, &exit);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Note(waited == WAIT_OBJECT_0 && exit == 0 ? "crash dump: written (thread %lu)" : "crash dump: not written (thread %lu, exit %lu)", GetCurrentThreadId(), exit);
}

/* The registers that point into a module, appended to names at m: its length. */
static int RegisterModules(const CONTEXT* c, char* where, size_t whereSize, char* names, int m, int size)
{
    const struct { const char* name; DWORD64 value; } regs[] = {{"rax", c->Rax}, {"rbx", c->Rbx}, {"rcx", c->Rcx}, {"rdx", c->Rdx},
        {"rsi", c->Rsi}, {"rdi", c->Rdi}, {"r8", c->R8}, {"r9", c->R9}, {"r12", c->R12}, {"r13", c->R13}, {"r14", c->R14}, {"r15", c->R15}};
    for (size_t k = 0; k < sizeof regs / sizeof regs[0] && m < size - 200; k++)
        if (ModuleAt((uintptr_t)regs[k].value, where, whereSize)) m += snprintf(names + m, size - m, "; %s %s", regs[k].name, where);
    return m;
}

/* The stack's values in modules other than the game (at most 16), appended to names at m: how many. */
static int OutsideStack(const uintptr_t* stack, const GameModules* g, char* where, size_t whereSize, char* names, int m, int size)
{
    int listed = 0;
    for (int k = 0; k < 256 && listed < 16 && m < size - 200; k++)
    {
        const char* inGame;
        uintptr_t o;
        if (InGame(stack[k], g->game, g->gameSize, g->exe, g->exeSize, &inGame, &o) || !ModuleAt(stack[k], where, whereSize)) continue;
        m += snprintf(names + m, size - m, " %s", where);
        listed++;
    }
    return listed;
}

/* The crash again, named by module: where the fault is, the registers that point into a module, and the stack's values
 * in modules other than the game (a caller outside it: a driver, an overlay, a script loader). stack: 256 values' room. */
static void CrashModules(uintptr_t at, const CONTEXT* c, const GameModules* g, uintptr_t* stack, size_t stackBytes)
{
    char where[160] = "", names[1000];
    int m = snprintf(names, sizeof names, "crash: by module: at %s", ModuleAt(at, where, sizeof where) ? where : "no module");
    m = RegisterModules(c, where, sizeof where, names, m, (int)sizeof names);
    int listed = 0;
    if (!SafeRead(c->Rsp, stack, stackBytes))
    {
        m += snprintf(names + m, sizeof names - m, "; stack outside the game:");
        listed = OutsideStack(stack, g, where, sizeof where, names, m, (int)sizeof names);
    }
    Note("%s%s", names, listed == 0 ? " none" : "");
}

/* GenerateObjectiveLocations (game.dll 0xF8F670) reads a mission row from its objective list (r12: +0 row, +4 ids,
 * +0x54 starts, +0x94 counts, +0xD4 count): the list and what surrounds it, 0x40 bytes a line. */
static void CrashObjectiveList(const CONTEXT* c)
{
    for (int k = -4; k < 8; k++)
    {
        uint8_t line[0x40];
        char hex[0x40 * 2 + 1];
        if (SafeRead(c->R12 + (intptr_t)k * 0x40, line, sizeof line)) continue;
        for (int i = 0; i < (int)sizeof line; i++) snprintf(hex + 2 * i, 3, "%02x", line[i]);
        Note("crash: objective list %+04x: %s", k * 0x40, hex);
    }
}

typedef struct LocationKind { uint32_t d8, cc, count, past; } LocationKind;
typedef struct LocationType { uint64_t type; uint32_t d8, count; } LocationType;

/* Counts location i of kind (d8, cc) in kinds (64 at most). */
static void CountLocationKind(LocationKind* kinds, int* nk, uint32_t d8, uint32_t cc, uint32_t i)
{
    int k = 0;
    while (k < *nk && !(kinds[k].d8 == d8 && kinds[k].cc == cc)) k++;
    if (k == *nk && *nk < 64) { kinds[*nk].d8 = d8; kinds[*nk].cc = cc; kinds[*nk].count = kinds[*nk].past = 0; (*nk)++; }
    if (k < *nk) { kinds[k].count++; if (i >= 2740) kinds[k].past++; }
}

/* Counts a location of this type hash in types (512 at most). */
static void CountLocationType(LocationType* types, int* nt, uint64_t type, uint32_t d8)
{
    int t = 0;
    while (t < *nt && types[t].type != type) t++;
    if (t == *nt && *nt < 512) { types[*nt].type = type; types[*nt].d8 = d8; types[*nt].count = 0; (*nt)++; }
    if (t < *nt) types[t].count++;
}

/* The 16 commonest location types (type(kind)=count) into text: its length. Their counts are used up. */
static int CommonestTypes(LocationType* types, int nt, char* text, int size)
{
    int len = 0;
    for (int shown = 0; shown < 16; shown++)
    {
        int best = -1;
        for (int t = 0; t < nt; t++) if (types[t].count && (best < 0 || types[t].count > types[best].count)) best = t;
        if (best < 0 || len >= size - 48) break;
        len += snprintf(text + len, size - len, " %016llx(%u)=%u", (unsigned long long)types[best].type, types[best].d8, types[best].count);
        types[best].count = 0;
    }
    return len;
}

/* The locations (G+0x758E20, 0xE0 each, 2740 places) by kind (+0xD8/+0xCC) and the 16 commonest type hashes
 * (+0xA0); records past the 2740th sit over the next arrays and may be overwritten. text: 2048 bytes for the lines. */
static void CrashLocations(uintptr_t block, uint32_t locations, uint32_t total, char* text)
{
    LocationKind kinds[64];
    static LocationType types[512];
    int nk = 0, nt = 0;
    for (uint32_t i = 0; i < total && i < 8192; i++)
    {
        uint8_t record[0xE0];
        if (SafeRead(block + locations + (uintptr_t)i * 0xE0, record, sizeof record)) break;
        uint32_t d8, cc; uint64_t type;
        memcpy(&d8, record + 0xD8, 4); memcpy(&cc, record + 0xCC, 4); memcpy(&type, record + 0xA0, 8);
        CountLocationKind(kinds, &nk, d8, cc, i);
        CountLocationType(types, &nt, type, d8);
    }
    int len = 0;
    for (int k = 0; k < nk && len < 2048 - 48; k++)
        len += snprintf(text + len, 2048 - len, " %u/%u=%u(%u past)", kinds[k].d8, kinds[k].cc, kinds[k].count, kinds[k].past);
    text[len] = 0;
    Note("crash: %u locations by kind (+0xD8/+0xCC):%s", total, text);
    len = CommonestTypes(types, nt, text, 2048);
    text[len] = 0;
    Note("crash: %d location types, the commonest (type(kind)=count):%s", nt, text);
}

/* The generation block (G = [r15 + 0x28]) and every count of its arrays (G+0x11A7070..+0x11A7270), then its locations. */
static void CrashGenerationBlock(const CONTEXT* c, uintptr_t game)
{
    uintptr_t block = 0;
    uint32_t counts[0x80];
    if (SafeRead(c->R15 + 0x28, &block, 8) || SafeRead(block + 0x11A7070, counts, sizeof counts)) return;
    char text[2048];
    int len = 0;
    for (int i = 0; i < 0x80 && len < (int)sizeof text - 32; i++)
        if (counts[i] != 0) len += snprintf(text + len, sizeof text - len, " %x=%u", 0x11A7070 + 4 * i, counts[i]);
    text[len] = 0;
    Note("crash: generation block 0x%llx, counts:%s", (unsigned long long)block, text);
    /* Where the locations are: GenerateResourceHash's add rbx, A + 0x90 (0xC176A7; moved for larger maps). */
    uint32_t locations = 0x758E20;
    if (!SafeRead(game + 0xC176A7 + 3, &locations, 4)) locations -= 0x90;
    CrashLocations(block, locations, counts[(0x11A7108 - 0x11A7070) / 4], text);
}

/* GenerateResourceHash (game.dll 0xC176B8) reads each location record's (0xE0 bytes, +0x90 at rbx) resource: the
 * record without one, its neighbours, and how many of the rest (rdi) have none. */
static void CrashLocationRecords(const CONTEXT* c, uintptr_t game)
{
    for (int k = -1; k <= 1; k++)
    {
        uint8_t record[0xE0];
        char hex[0xE0 * 2 + 1];
        if (SafeRead(c->Rbx - 0x90 + (intptr_t)k * 0xE0, record, sizeof record)) continue;
        for (int i = 0; i < (int)sizeof record; i++) snprintf(hex + 2 * i, 3, "%02x", record[i]);
        Note("crash: location record %+d: %s", k, hex);
    }
    int missing = 0;
    for (uint64_t i = 0; i < c->Rdi && i < 4096; i++)
    {
        uint64_t resource = 1;
        if (!SafeRead(c->Rbx + i * 0xE0, &resource, 8) && resource == 0) missing++;
    }
    Note("crash: %d of the %llu remaining location records have no resource", missing, (unsigned long long)c->Rdi);
    CrashGenerationBlock(c, game);
}

static LONG CALLBACK CrashNote(EXCEPTION_POINTERS* info)
{
    static volatile LONG reported;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if ((code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION) || InterlockedIncrement(&reported) > 4)
        return EXCEPTION_CONTINUE_SEARCH;
    if (reported == 1) WriteCrashDump(info);
    GameModules g = CurrentModules();
    uintptr_t at = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
    const char* module = "?";
    uintptr_t offset = at;
    InGame(at, g.game, g.gameSize, g.exe, g.exeSize, &module, &offset);
    CONTEXT* c = info->ContextRecord;
    char trail[1024];
    uintptr_t stack[256];
    if (SafeRead(c->Rsp, stack, sizeof stack)) trail[0] = 0;
    else GameTrail(stack, 256, 24, &g, trail, (int)sizeof trail);
    Note("crash: %08lx on thread %lu at %s+0x%llx (0x%llx), %s 0x%llx; rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx "
        "r9 %llx r12 %llx r13 %llx r14 %llx r15 %llx; stack:%s", code, GetCurrentThreadId(), module, (unsigned long long)offset,
        (unsigned long long)at, info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
        (unsigned long long)(info->ExceptionRecord->NumberParameters > 1 ? info->ExceptionRecord->ExceptionInformation[1] : 0),
        (unsigned long long)c->Rax, (unsigned long long)c->Rbx, (unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
        (unsigned long long)c->Rsi, (unsigned long long)c->Rdi, (unsigned long long)c->R8, (unsigned long long)c->R9,
        (unsigned long long)c->R12, (unsigned long long)c->R13, (unsigned long long)c->R14, (unsigned long long)c->R15, trail);
    CrashModules(at, c, &g, stack, sizeof stack);
    if (module[0] == 'g' && offset == 0xF8F670) CrashObjectiveList(c);
    if (module[0] == 'g' && offset == 0xC176B8) CrashLocationRecords(c, g.game);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* A fail-fast exit (0xC0000409: a stack cookie check, abort, an invalid parameter) skips the exception handlers; the
 * runtime asks IsProcessorFeaturePresent(PF_FASTFAIL_AVAILABLE) just before it. That question logs the game addresses
 * on the asking thread's stack, then is answered as usual. */
static BOOL (WINAPI* g_isProcessorFeaturePresent)(DWORD);

/* The fail-fast note: the game addresses on the stack above the detour's frame, and who asked (`at`, the detour's
 * return address): a game address, else the module and offset (ucrtbase's abort, kernelbase, ...). */
static void FailFastNote(uintptr_t frame, uintptr_t at)
{
    GameModules g = CurrentModules();
    static uintptr_t stack[512]; /* the callers' frames: above this one */
    char trail[1536];
    if (SafeRead(frame, stack, sizeof stack)) trail[0] = 0;
    else GameTrail(stack, 512, 40, &g, trail, (int)sizeof trail);
    uintptr_t o = at;
    const char* m = NULL;
    char owner[MAX_PATH] = "?";
    HMODULE module = NULL;
    if (!InGame(at, g.game, g.gameSize, g.exe, g.exeSize, &m, &o) &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &module))
    {
        GetModuleFileNameA(module, owner, sizeof owner);
        o = at - (uintptr_t)module;
    }
    const char* name = m != NULL ? m : (strrchr(owner, '\\') ? strrchr(owner, '\\') + 1 : owner);
    Note("crash: fail-fast about to end the game (thread %lu, asked from %s+0x%llx); stack:%s", GetCurrentThreadId(), name, (unsigned long long)o, trail);
}

static BOOL WINAPI FeatureDetour(DWORD feature)
{
    static volatile LONG reported;
    if (feature == 23 /* PF_FASTFAIL_AVAILABLE */ && InterlockedIncrement(&reported) <= 4)
        FailFastNote((uintptr_t)__builtin_frame_address(0), (uintptr_t)__builtin_return_address(0));
    return g_isProcessorFeaturePresent(feature);
}

static void InstallFailFastNote(void)
{
    void* target = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsProcessorFeaturePresent");
    if (target == NULL) return;
    if (LoaderHook("fail-fast notes", (uintptr_t)target, (void*)FeatureDetour, (void**)&g_isProcessorFeaturePresent))
        Note("crash notes: fail-fast exits will not be noted");
}

/* Diagnostic (the war table's map previews): hardware watchpoints on a level's playable radius (+0x2D70, DR0) and row
 * factor (+0x2D80, DR1) in every thread; each new game address that touches them is logged once. */
static volatile uintptr_t g_fieldWatched;
static volatile uintptr_t g_previewRecord; /* the record 0x1003EE6 copies the radius into (+0x240); watched with DR2 */
static uintptr_t g_fieldHits[48];
static volatile LONG g_fieldHitCount;

/* A new game address that touched a watched field (`which`: 0 the radius, 1 the row factor, 2 the preview record), as
 * hit n: kept and logged. */
static void NoteFieldHit(EXCEPTION_POINTERS* info, uintptr_t at, int which, LONG n)
{
    g_fieldHits[n] = at;
    InterlockedIncrement(&g_fieldHitCount);
    GameModules g = CurrentModules();
    const char* m = "?";
    uintptr_t o = at;
    InGame(at, g.game, g.gameSize, g.exe, g.exeSize, &m, &o);
    float value = 0;
    SafeRead(which == 2 ? info->ContextRecord->Dr2 : g_fieldWatched + (which ? 0x10 : 0), &value, 4);
    if (which == 2)
        Note("difficulty: preview record +0x240 (now %.3f) touched before %s+0x%llx (thread %lu)", (double)value, m, (unsigned long long)o, GetCurrentThreadId());
    else
        Note("difficulty: level +0x%x (now %.3f) touched before %s+0x%llx (thread %lu)", which ? 0x2D80 : 0x2D70, (double)value, m,
            (unsigned long long)o, GetCurrentThreadId());
}

static LONG CALLBACK FieldWatchHit(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 7)) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t at = info->ContextRecord->Rip;
    int which = (info->ContextRecord->Dr6 & 1) ? 0 : (info->ContextRecord->Dr6 & 2) ? 1 : 2;
    if (which == 0 && at == ApiModuleBase("game.dll", NULL) + 0x1003EE6) g_previewRecord = info->ContextRecord->Rsi;
    info->ContextRecord->Dr6 = 0;
    LONG n = g_fieldHitCount;
    for (LONG i = 0; i < n && i < 48; i++) if (g_fieldHits[i] == at) return EXCEPTION_CONTINUE_EXECUTION;
    if (n < 48) NoteFieldHit(info, at, which, n);
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* One thread's watchpoints, set while it is briefly suspended: 1 when they are. */
static int WatchThread(DWORD id, uintptr_t field, uintptr_t record)
{
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, id);
    if (thread == NULL) return 0;
    int set = 0;
    if (SuspendThread(thread) != (DWORD)-1)
    {
        CONTEXT context = {0};
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context))
        {
            context.Dr0 = field;
            context.Dr1 = field + 0x10;
            context.Dr7 = (context.Dr7 & ~0xFF000Full) | 1 | 4 | (0xFull << 16) | (0xFull << 20); /* L0, L1: read/write, 4 bytes */
            if (record != 0) { context.Dr2 = record + 0x240; context.Dr7 = (context.Dr7 & ~0xF000030ull) | 0x10 | (0xFull << 24); } /* L2 */
            if (SetThreadContext(thread, &context)) set++;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
    return set;
}

static void FieldWatch(uintptr_t field)
{
    static int handler;
    static uintptr_t record;
    if (field == g_fieldWatched && g_previewRecord == record) return;
    record = g_previewRecord;
    if (!handler) { AddVectoredExceptionHandler(1, FieldWatchHit); handler = 1; }
    g_fieldWatched = field;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry = {sizeof entry};
    DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    int set = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        set += WatchThread(entry.th32ThreadID, field, record);
    }
    CloseHandle(snapshot);
    Note("difficulty: watching level fields at 0x%llx%s in %d threads", (unsigned long long)field, record ? " and the preview record" : "", set);
}
