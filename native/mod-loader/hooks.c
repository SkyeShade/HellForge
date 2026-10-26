/* Managed hooks (SDK 13; docs/modding/hooks.md). Every hook the loader puts in the game's process, a mod's
 * (hook, hook_ex) or HellForge's own (event sources, the registry, the game thread...), goes into one chain per hooked
 * function, so any number of them coexist and none silently replaces another. Included by hellforge_modloader.c.
 *
 * A function is patched once: MinHook (or FarHook, where MinHook finds no room) sends it to a head stub, a jump through a
 * pointer to the first link's detour. Each link's original is the next link's detour; the last link's is a tail stub, a
 * jump through the pointer to the trampoline that runs the function's own code. Adding a link only changes pointers,
 * one 8-byte write each, the new link's own original first, so a call in flight always finds a whole chain. Detours of
 * any signature chain: the stubs only jump.
 *
 * The order, first to run to last (each calls the next through its original):
 *   1. mods, highest priority first (hook is priority 0; hook_ex takes one); equal priorities by load order, the mod
 *      higher on the Mods page (loaded later) first, as it wins other conflicts. A mod's own later hook on the same
 *      function goes before its earlier one;
 *   2. HellForge's own hooks last, nearest the game's code (the newest first): what a mod's detour passes on is what
 *      HellForge's events see.
 * Hooks stay for the session. A mod must call the original through the variable it passed: HellForge rewrites it when a
 * hook goes in after it in the chain. */

/* Every other thread of the process suspended (0 none could be), to patch code they may run; FarResume undoes it. */
#define FAR_THREADS 1024
static HANDLE g_farThreads[FAR_THREADS];
static int g_farThreadCount;
static void FarSuspend(void)
{
    g_farThreadCount = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry = {sizeof entry};
    DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more && g_farThreadCount < FAR_THREADS; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
        if (thread == NULL) continue;
        if (SuspendThread(thread) == (DWORD)-1) { CloseHandle(thread); continue; }
        g_farThreads[g_farThreadCount++] = thread;
    }
    CloseHandle(snapshot);
}
static int FarAnyIn(uintptr_t from, size_t size)
{
    for (int i = 0; i < g_farThreadCount; i++)
    {
        CONTEXT context = {0};
        context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_farThreads[i], &context) && context.Rip >= from && context.Rip < from + size) return 1;
    }
    return 0;
}
static void FarResume(void)
{
    for (int i = 0; i < g_farThreadCount; i++) { ResumeThread(g_farThreads[i]); CloseHandle(g_farThreads[i]); }
    g_farThreadCount = 0;
}

/* Appends an absolute jump (jmp [rip+0]; target), 14 bytes. */
static size_t FarJump(uint8_t* at, uint64_t target)
{
    static const uint8_t jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, jump, 6);
    memcpy(at + 6, &target, 8);
    return 14;
}

#define FAR_CODE 32 /* the bytes FarHook reads at its target */

/* A relative jump at the target: its destination hooked with MinHook. */
static int FarHookFollow(uintptr_t target, const uint8_t* code, void* detour, void** original, const char** how)
{
    int32_t rel = code[0] == 0xE9 ? 0 : (int8_t)code[1];
    if (code[0] == 0xE9) memcpy(&rel, code + 1, 4);
    uintptr_t next = target + (code[0] == 0xE9 ? 5 : 2) + (intptr_t)rel;
    MakeExecutable(next);
    MH_STATUS status = MH_CreateHook((void*)next, detour, original);
    if (status == MH_OK) status = MH_EnableHook((void*)next);
    *how = status == MH_OK ? "a jump followed, its destination hooked" : "a jump followed, its destination not hookable either";
    return status == MH_OK ? 0 : 1;
}

/* A target that jumps at once: an overlay's stub's address or the pointer swapped, or a relative jump followed. -1 when
 * it does not start with a jump. */
static int FarHookJump(uintptr_t target, const uint8_t* code, void* detour, void** original, const char** how)
{
    if (code[0] == 0xFF && code[1] == 0x25) /* jmp [rip+disp32] */
    {
        int32_t disp; memcpy(&disp, code + 2, 4);
        *how = disp == 0 ? "an overlay's jump stub, its address swapped" : "a jump through a pointer, the pointer swapped";
        return SwapPointer(target + 6 + (intptr_t)disp, detour, original);
    }
    if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) /* mov rax, imm64; jmp rax */
    {
        *how = "an overlay's jump stub (mov rax), its address swapped";
        return SwapPointer(target + 2, detour, original);
    }
    if (code[0] == 0xE9 || code[0] == 0xEB) return FarHookFollow(target, code, detour, original, how); /* a relative jump: hook where it goes */
    return -1;
}

static int FarIsBranch(const hde64s* hs)
{
    return (hs->opcode >= 0x70 && hs->opcode <= 0x7F) || (hs->opcode >= 0xE0 && hs->opcode <= 0xE3) ||
        (hs->opcode == 0x0F && hs->opcode2 >= 0x80 && hs->opcode2 <= 0x8F);
}

/* One of the target's first instructions (`length` bytes, at `bytes`; `next` the address after it) into the trampoline
 * at *out: a call rel32 made absolute, anything else copied, a jump, a conditional branch or a RIP-relative operand
 * refused (0, *how). */
static int FarMoveInstruction(const hde64s* hs, const uint8_t* bytes, unsigned length, uintptr_t next, uint8_t* trampoline, size_t* out,
    const char** how)
{
    if (hs->opcode == 0xE8) /* call rel32: call [rip+2]; jmp +8; address */
    {
        static const uint8_t call[8] = {0xFF, 0x15, 0x02, 0, 0, 0, 0xEB, 0x08};
        uint64_t dest = next + (int64_t)(int32_t)hs->imm.imm32;
        memcpy(trampoline + *out, call, 8); memcpy(trampoline + *out + 8, &dest, 8); *out += 16;
    }
    else if (hs->opcode == 0xE9 || hs->opcode == 0xEB) { *how = "code that jumps away in its first bytes"; return 0; }
    else if (FarIsBranch(hs)) { *how = "a conditional branch in its first bytes"; return 0; }
    else if ((hs->flags & F_MODRM) && hs->modrm_mod == 0 && hs->modrm_rm == 5) { *how = "a RIP-relative operand in its first bytes"; return 0; }
    else { memcpy(trampoline + *out, bytes, length); *out += length; }
    return 1;
}

/* The target's first instructions, 14 bytes or more, moved into the trampoline (*out: its length): the bytes they take,
 * or 0 refused (*how). */
static size_t FarMove(uintptr_t target, const uint8_t code[FAR_CODE], uint8_t* trampoline, size_t* out, const char** how)
{
    size_t moved = 0;
    while (moved < 14)
    {
        hde64s hs;
        unsigned length = hde64_disasm(code + moved, &hs);
        if ((hs.flags & F_ERROR) || length == 0 || moved + length > FAR_CODE - 8) { *how = "code it cannot read"; return 0; }
        uintptr_t at = target + moved, next = at + length;
        if (!FarMoveInstruction(&hs, code + moved, length, next, trampoline, out, how)) return 0;
        moved += length;
    }
    return moved;
}

/* A hook where MinHook finds no memory within 2 GB of the target for its trampoline: code in allocated memory amid
 * the GPU driver's reservations, which is where overlays (RivaTuner, Discord, Overwolf...) put the stubs they send a
 * swap chain's methods through. A stub is taken over in place, as overlays chain: an absolute jump through a pointer
 * (jmp [rip+disp]; the common "FF 25 00000000 <address>") or mov rax, imm64; jmp rax gets the detour as its address,
 * the old address becoming the original (the call runs through the detour, then the overlay). A relative jump first
 * is followed and its destination hooked. Other code gets a 14-byte absolute jump and a trampoline anywhere, when the
 * instructions it moves can be moved (a relative jump or call is rewritten absolute; a conditional branch or another
 * RIP-relative operand refuses it); every other thread is suspended meanwhile and none may be in those bytes.
 * 0 when hooked, *how saying which. */
static int FarHook(uintptr_t target, void* detour, void** original, const char** how)
{
    uint8_t code[FAR_CODE];
    if (SafeRead(target, code, sizeof code)) { *how = "unreadable"; return 1; }
    int jumped = FarHookJump(target, code, detour, original, how);
    if (jumped >= 0) return jumped;
    uint8_t* trampoline = (uint8_t*)VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (trampoline == NULL) { *how = "no memory for a trampoline"; return 1; }
    size_t out = 0, moved = FarMove(target, code, trampoline, &out, how);
    if (moved == 0) goto refuse;
    out += FarJump(trampoline + out, target + moved);
    uint8_t patch[32];
    size_t used = FarJump(patch, (uint64_t)(uintptr_t)detour);
    memset(patch + used, 0x90, moved - used); /* the rest of the moved instructions: nops */
    *original = trampoline; /* before the jump is written: a call may come through it at once */
    FarSuspend();
    if (FarAnyIn(target, moved)) { FarResume(); *how = "a thread is running the code it would move"; goto refuse; }
    DWORD protect;
    if (!VirtualProtect((void*)target, moved, PAGE_EXECUTE_READWRITE, &protect)) { FarResume(); *how = "the code cannot be made writable"; goto refuse; }
    memcpy((void*)target, patch, moved);
    VirtualProtect((void*)target, moved, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, moved);
    NoteRewrite(target, moved);
    FarResume();
    *how = "a far jump and a trampoline";
    return 0;
refuse:
    VirtualFree(trampoline, 0, MEM_RELEASE);
    return 1;
}

#define HOOK_TARGETS 1024
#define HOOK_LINKS 16
#define HOOK_SAVED 32

typedef struct HookLink
{
    HfMod* mod;       /* NULL: HellForge's own */
    char owner[96];   /* the mod's id, or "HellForge (<what>)" */
    int32_t priority;
    int loadIndex;    /* the mod's place in load order */
    void* detour;
    void** original;
} HookLink;

typedef struct HookTarget
{
    uintptr_t target;
    uint8_t* stubs;               /* head stub (+0, its pointer at +8) and tail stub (+16, its pointer at +24) */
    uint8_t saved[HOOK_SAVED];    /* the function's first bytes before it was hooked (HookedCode) */
    size_t savedSize;             /* 0 when the hook changed none of them (an overlay's stub, a jump followed) */
    char how[96];
    int count;
    HookLink links[HOOK_LINKS];   /* first to run (outermost) to last */
} HookTarget;

static HookTarget g_hookTargets[HOOK_TARGETS];
static volatile LONG g_hookTargetCount;
static SRWLOCK g_hookLock = SRWLOCK_INIT;
static uint8_t* g_hookStubPage;
static size_t g_hookStubUsed;
#define HOOK_STUB_PAGE 0x10000

/* game.dll functions hooked (the Lua bridge's hellforge_lua_rewrites): their rvas. */
#define HOOKED_MAX 256
static uint32_t g_hookedRvas[HOOKED_MAX];
static volatile LONG g_hookedCount;

static void** StubPointer(uint8_t* stub) { return (void**)(stub + 8); }

/* jmp [rip+2]; int3; int3; <pointer>: the pointer 8-byte aligned, so it is swapped in one write. */
static void WriteStub(uint8_t* stub, void* to)
{
    static const uint8_t jump[8] = {0xFF, 0x25, 0x02, 0x00, 0x00, 0x00, 0xCC, 0xCC};
    memcpy(stub, jump, 8);
    *StubPointer(stub) = to;
}

static void SetPointer(void** at, void* value)
{
    if (((uintptr_t)at & 7) == 0) InterlockedExchangePointer((void* volatile*)at, value);
    else memcpy(at, &value, sizeof value);
}

/* "game.dll+0x13c0a80", or the bare address outside a module. */
static void HookWhere(uintptr_t at, char* out, size_t size)
{
    HMODULE module = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &module) &&
        GetModuleFileNameA(module, path, sizeof path))
    {
        const char* name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
        snprintf(out, size, "%s+0x%llx", name, (unsigned long long)(at - (uintptr_t)module));
    }
    else snprintf(out, size, "0x%llx", (unsigned long long)at);
}

/* True when `a` runs before `b` (is outside it) in a chain. */
static int HookRunsBefore(const HookLink* a, const HookLink* b)
{
    if ((a->mod == NULL) != (b->mod == NULL)) return a->mod != NULL; /* mods before HellForge's own */
    if (a->mod == NULL) return 1;                                     /* HellForge's: the newest first */
    if (a->priority != b->priority) return a->priority > b->priority;
    return a->loadIndex >= b->loadIndex;                              /* later in load order first; the same mod's newest first */
}

/* One chain's lines of hooks.tsv, first to last. */
static void HookReportChain(HANDLE file, const HookTarget* h)
{
    char line[512];
    DWORD written;
    char where[128];
    HookWhere(h->target, where, sizeof where);
    for (int i = 0; i < h->count; i++)
    {
        int length = snprintf(line, sizeof line, "%s\t%d of %d\t%s\t%d\t%s\n", where, i + 1, h->count, h->links[i].owner,
            (int)h->links[i].priority, h->how[0] ? h->how : "MinHook");
        if (length > 0) WriteFile(file, line, (DWORD)(length < (int)sizeof line ? length : (int)sizeof line - 1), &written, NULL);
    }
}

/* The session's hooks, every chain first to last: logs/sessions/<session>/hooks.tsv. Under g_hookLock. */
static void HookReport(void)
{
    HANDLE file;
    wchar_t path[MAX_PATH];
    if (!g_logs[0] || swprintf(path, MAX_PATH, L"%ls\\hooks.tsv", g_logs) < 0) return;
    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[512];
    DWORD written;
    int length = snprintf(line, sizeof line, "function\tplace\towner\tpriority\thow\n");
    WriteFile(file, line, (DWORD)length, &written, NULL);
    for (LONG t = 0; t < g_hookTargetCount; t++) HookReportChain(file, &g_hookTargets[t]);
    CloseHandle(file);
}

/* Whether the i-th link is its owner's first in the chain. */
static int HookFirstOfOwner(const HookTarget* h, int i)
{
    int seen = 0;
    for (int j = 0; j < i; j++) seen |= h->links[j].mod == h->links[i].mod;
    return !seen;
}

/* A chain with two owners or more: one line naming them all, first to last, in modloader.log and each mod's log. */
static void HookShared(HookTarget* h)
{
    int owners = 0;
    for (int i = 0; i < h->count; i++) owners += HookFirstOfOwner(h, i);
    if (owners < 2) return;
    char where[128], list[1024];
    size_t used = 0;
    HookWhere(h->target, where, sizeof where);
    list[0] = 0;
    for (int i = 0; i < h->count && used < sizeof list; i++)
    {
        int n = snprintf(list + used, sizeof list - used, "%s%s (priority %d)", i ? ", then " : "", h->links[i].owner, (int)h->links[i].priority);
        if (n < 0) break;
        used += (size_t)n;
    }
    Note("hooks: shared hook %s, first to last: %s", where, list);
    for (int i = 0; i < h->count; i++)
        if (h->links[i].mod != NULL && HookFirstOfOwner(h, i)) ApiLog(h->links[i].mod, "shared hook %s, first to last: %s", where, list);
}

/* The function's first patch: MinHook, else FarHook, sending it to the head stub; the trampoline goes into the tail
 * stub's pointer. 0 when hooked. Under g_hookLock. */
static int HookInstall(HookTarget* h, char* why, size_t whySize)
{
    if (!g_hooksReady) g_hooksReady = MH_Initialize() == MH_OK ? 1 : -1;
    if (g_hooksReady != 1) { snprintf(why, whySize, "MinHook did not start"); return 1; }
    MakeExecutable(h->target);
    MH_STATUS status = MH_CreateHook((void*)h->target, h->stubs, StubPointer(h->stubs + 16));
    if (status == MH_OK) status = MH_EnableHook((void*)h->target);
    if (status == MH_OK) { h->savedSize = HOOK_SAVED; return 0; }
    if (status == MH_ERROR_MEMORY_ALLOC)
    {
        const char* how = NULL;
        if (FarHook(h->target, h->stubs, StubPointer(h->stubs + 16), &how) == 0)
        {
            snprintf(h->how, sizeof h->how, "no room near it for MinHook: %s", how);
            h->savedSize = strstr(how, "trampoline") ? HOOK_SAVED : 0;
            return 0;
        }
        snprintf(why, whySize, "MinHook %d, then: %s", (int)status, how ? how : "?");
        return 1;
    }
    snprintf(why, whySize, status == MH_ERROR_ALREADY_CREATED ? "MinHook %d: hooked outside HellForge's chain" : "MinHook %d", (int)status);
    return 1;
}

/* A hook refused before it is looked at: no function, detour or original. */
static void HookRefused(HfMod* mod, const char* what, uintptr_t target, void* detour)
{
    if (mod) ApiLog(mod, "hook refused: %s", target == 0 ? "no function (0)" : detour == NULL ? "no detour" : "no variable for the original");
    else Note("hooks: %s refused (no function, detour or original)", what ? what : "?");
}

/* The link a hook adds: its owner (`mod`, or HellForge's own, `what` naming it), its priority and place in load order. */
static HookLink HookLinkOf(HfMod* mod, const char* what, void* detour, void** original, int32_t priority)
{
    HookLink link = {0};
    link.mod = mod;
    link.priority = mod ? priority : 0;
    link.loadIndex = mod ? ModLoadIndex(mod) : -1;
    link.detour = detour;
    link.original = original;
    if (mod) snprintf(link.owner, sizeof link.owner, "%s", mod->id);
    else snprintf(link.owner, sizeof link.owner, "HellForge (%s)", what ? what : "loader");
    return link;
}

/* The chain of `target`, NULL none yet. Under g_hookLock. */
static HookTarget* HookTargetOf(uintptr_t target)
{
    HookTarget* h = NULL;
    for (LONG t = 0; t < g_hookTargetCount && h == NULL; t++) if (g_hookTargets[t].target == target) h = &g_hookTargets[t];
    return h;
}

/* A hooked function of game.dll's: its rva noted (hellforge_lua_rewrites). */
static void HookNoteRva(uintptr_t target)
{
    size_t gameSize = 0;
    uintptr_t game = ApiModuleBase("game.dll", &gameSize);
    if (game != 0 && target >= game && target < game + gameSize && g_hookedCount < HOOKED_MAX)
    {
        g_hookedRvas[g_hookedCount] = (uint32_t)(target - game);
        InterlockedIncrement(&g_hookedCount);
    }
}

/* A function's first hook: its chain made (its stubs, the link) and the function patched. NULL with *why. Under
 * g_hookLock. */
static HookTarget* HookFirst(uintptr_t target, const HookLink* link, char* why, size_t whySize)
{
    if (g_hookTargetCount >= HOOK_TARGETS) { snprintf(why, whySize, "%d hooked functions already (the most)", HOOK_TARGETS); return NULL; }
    if (g_hookStubPage == NULL && (g_hookStubPage = (uint8_t*)VirtualAlloc(NULL, HOOK_STUB_PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) == NULL)
    { snprintf(why, whySize, "no memory for its stubs"); return NULL; }
    if (g_hookStubUsed + 32 > HOOK_STUB_PAGE) { snprintf(why, whySize, "no room for its stubs"); return NULL; }
    HookTarget* h = &g_hookTargets[g_hookTargetCount];
    memset(h, 0, sizeof *h);
    h->target = target;
    h->stubs = g_hookStubPage + g_hookStubUsed;
    if (SafeRead(target, h->saved, HOOK_SAVED)) { snprintf(why, whySize, "its code cannot be read"); return NULL; }
    /* the whole chain before the function is patched: the link, its original the tail stub */
    void* before = *link->original;
    *link->original = h->stubs + 16;
    WriteStub(h->stubs, link->detour);
    WriteStub(h->stubs + 16, NULL);
    FlushInstructionCache(GetCurrentProcess(), h->stubs, 32);
    if (HookInstall(h, why, whySize)) { *link->original = before; return NULL; }
    h->links[0] = *link;
    h->count = 1;
    g_hookStubUsed += 32;
    InterlockedIncrement(&g_hookTargetCount);
    HookNoteRva(target);
    return h;
}

/* A hook added to a function's chain in its place: the new link's original first, then the pointer that leads to it.
 * NULL with *why. Under g_hookLock. */
static HookTarget* HookChainAdd(HookTarget* h, const HookLink* link, char* why, size_t whySize)
{
    for (int i = 0; i < h->count && !why[0]; i++)
        if (h->links[i].detour == link->detour) snprintf(why, whySize, "%s hooks it with this detour already", h->links[i].owner);
    if (!why[0] && h->count >= HOOK_LINKS) snprintf(why, whySize, "%d hooks on it already (the most)", HOOK_LINKS);
    if (why[0]) return NULL;
    int at = 0;
    while (at < h->count && !HookRunsBefore(link, &h->links[at])) at++;
    SetPointer(link->original, at < h->count ? h->links[at].detour : (void*)(h->stubs + 16));
    for (int i = h->count; i > at; i--) h->links[i] = h->links[i - 1];
    h->links[at] = *link;
    h->count++;
    SetPointer(at == 0 ? StubPointer(h->stubs) : h->links[at - 1].original, link->detour);
    return h;
}

/* The detour's place in its chain, from 1. */
static int HookPlace(const HookTarget* h, void* detour)
{
    int place = 0;
    for (int i = 0; i < h->count; i++) if (h->links[i].detour == detour) place = i + 1;
    return place;
}

static void HookNotHooked(HfMod* mod, uintptr_t target, const char* where, const char* why, const char* owner)
{
    if (mod)
    {
        uint8_t bytes[16] = {0};
        SafeRead(target, bytes, sizeof bytes);
        ApiLog(mod, "could not hook %s (%s); its first bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            where, why, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
            bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
    Note("hooks: %s could not hook %s (%s)", owner, where, why);
}

/* how: a first hook's way in ("" when MinHook's, or for a hook added to a chain). */
static void HookNoteHooked(HfMod* mod, const HookLink* link, const char* where, int place, int count, const char* how)
{
    if (mod) ApiLog(mod, "hooked %s (priority %d; %d of %d in its chain%s%s)", where, (int)link->priority, place, count, how[0] ? "; " : "", how);
    Note("hooks: %s hooked %s (priority %d; %d of %d in its chain%s%s)", link->owner, where, (int)link->priority, place, count,
        how[0] ? "; " : "", how);
}

/* Adds `detour` to the chain of `target` for `mod` (NULL: HellForge's own, `what` naming it). 0 when it is in. */
static int HookAdd(HfMod* mod, const char* what, uintptr_t target, void* detour, void** original, int32_t priority)
{
    char where[128];
    HookWhere(target, where, sizeof where);
    if (target == 0 || detour == NULL || original == NULL) { HookRefused(mod, what, target, detour); return 1; }
    HookLink link = HookLinkOf(mod, what, detour, original, priority);

    AcquireSRWLockExclusive(&g_hookLock);
    HookTarget* h = HookTargetOf(target);
    char why[192] = "";
    int fresh = h == NULL;
    if (fresh) h = HookFirst(target, &link, why, sizeof why);
    else h = HookChainAdd(h, &link, why, sizeof why);
    int place = 0, count = 0;
    if (h != NULL)
    {
        place = HookPlace(h, detour);
        count = h->count;
        HookShared(h);
        HookReport();
    }
    ReleaseSRWLockExclusive(&g_hookLock);

    if (h == NULL) { HookNotHooked(mod, target, where, why, link.owner); return 1; }
    HookNoteHooked(mod, &link, where, place, count, fresh ? h->how : "");
    return 0;
}

/* Code as the game has it: `size` bytes at `at`, with a hooked function's first bytes as they were before its hook (a
 * function behind a hook is still the reviewed one). 0 when read. */
static int HookedCode(uintptr_t at, uint8_t* bytes, size_t size)
{
    if (SafeRead(at, bytes, size)) return 1;
    AcquireSRWLockShared(&g_hookLock);
    for (LONG t = 0; t < g_hookTargetCount; t++)
    {
        const HookTarget* h = &g_hookTargets[t];
        for (size_t i = 0; i < h->savedSize; i++)
            if (h->target + i >= at && h->target + i < at + size) bytes[h->target + i - at] = h->saved[i];
    }
    ReleaseSRWLockShared(&g_hookLock);
    return 0;
}

/* HfApi.read_code and code_matches (SDK 13): a mod's check of a function before it hooks it, which another hook in
 * front (a mod's that hooked first, or HellForge's) does not fail. Only hooks are read through: other writes, a mod's
 * hf->write or HellForge's rewritten instructions, read as they are. */
static int ApiReadCode(uintptr_t address, void* buffer, size_t size)
{
    return address == 0 || buffer == NULL ? 1 : HookedCode(address, (uint8_t*)buffer, size);
}

static int ApiCodeMatches(uintptr_t address, const char* pattern)
{
    uint8_t bytes[256], mask[256], code[256];
    size_t count = ParsePattern(pattern, bytes, mask);
    if (address == 0 || count == 0 || HookedCode(address, code, count)) return 0;
    for (size_t i = 0; i < count; i++) if (mask[i] && code[i] != bytes[i]) return 0;
    return 1;
}

/* HfApi.hook (SDK 1) and hook_ex (SDK 13). */
static int ApiHook(HfMod* mod, uintptr_t target, void* detour, void** original) { return HookAdd(mod, NULL, target, detour, original, 0); }
static int ApiHookEx(HfMod* mod, uintptr_t target, void* detour, void** original, int32_t priority)
{
    return HookAdd(mod, NULL, target, detour, original, priority);
}

/* HellForge's own hooks: the last in every chain. */
static int LoaderHook(const char* what, uintptr_t target, void* detour, void** original) { return HookAdd(NULL, what, target, detour, original, 0); }
