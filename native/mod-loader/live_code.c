/* Code mods in a live change (live_mods.c, SDK 16: docs/modding/live.md): a mod whose manifest says "live": true is set
 * up while the game runs, or taken out again. Included by live_mods.c.
 *
 * Set-up: its DLL is loaded (from its own path: a new version is a new file), its hellforge_mod_init runs on the
 * change's thread with g_liveSetup set (on() calls back after it, not inside; its deferred edits go in load order), and
 * once the whole change is checked every load event that already fired is delivered to it, in the order the game fired
 * them, then HF_EVENT_MODS_CHANGED to every mod. A set-up that fails, or a change refused after it, takes the mod out
 * again (below): the game is as it was, but for its DLL staying loaded.
 *
 * Taking out: the mod is marked removing (the API refuses what it would add), its game-thread callbacks, listeners,
 * queued actions and shot rules go in one step on the game's thread (none of them is running then), its HF event
 * callbacks and hooks go (a call in flight finishes: hooks.c), its hellforge_mod_unload stops what it started outside
 * the API, its writes are put back (newest first; code under a freeze, none of whose threads may be in the bytes; bytes
 * someone changed since are left alone), its edits and live edits go (each field's other edits apply again over what
 * was under it), its live stat values go. Its stat lines are retired by the stats layer (live_stats.c). The DLL is never
 * freed: a thread may still be returning into it. A mod that did not declare "live" is never taken out (its init may
 * have relied on the game still starting), nor one whose writes were not all journaled, nor one without an unload
 * export whose own threads still run. */

/* ---- the write journal: HfApi write and write_u32_if --------------------------------------------------------------- */

/* A mod's write through the API: where, how many bytes, the bytes before its first write there, then the last it wrote
 * (one allocation of 2 x size). A mod writing the same bytes again updates its entry. */
typedef struct WriteEntry { HfMod* mod; uintptr_t address; uint32_t size; uint8_t* bytes; } WriteEntry;
#define WRITE_JOURNAL_MAX 65536
#define WRITE_JOURNAL_SIZE 4096 /* the largest write journaled; a mod writing more cannot be taken out */
static WriteEntry* g_writes;
static int g_writeCount, g_writeCapacity;
static SRWLOCK g_writesLock = SRWLOCK_INIT;
/* A mod whose writes were not all journaled (too large, too many, unreadable): it cannot be taken out. */
static HfMod* g_writesLost[MAX_MODS];
static int g_writesLostCount;

/* The code mod whose DLL holds this address (a write's return address), or NULL. */
static HfMod* ModAt(uintptr_t address)
{
    for (int i = 0; i < g_modCount; i++)
        if (!g_mods[i].lua && g_mods[i].imageBase && address >= g_mods[i].imageBase && address < g_mods[i].imageEnd) return &g_mods[i];
    return NULL;
}

/* After a mod's DLL is loaded: its image's range and its unload export. */
static void ModImage(HfMod* mod)
{
    if (mod->module == NULL) return;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)mod->module;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)((const uint8_t*)mod->module + dos->e_lfanew);
    mod->imageBase = (uintptr_t)mod->module;
    mod->imageEnd = mod->imageBase + nt->OptionalHeader.SizeOfImage;
    mod->unload = (int (*)(const HfApi*, HfMod*, uint32_t))(void*)GetProcAddress(mod->module, "hellforge_mod_unload");
}

static int WritesLost(const HfMod* mod)
{
    for (int i = 0; i < g_writesLostCount; i++) if (g_writesLost[i] == mod) return 1;
    return 0;
}

static void WritesLose(HfMod* mod, const char* why)
{
    AcquireSRWLockExclusive(&g_writesLock);
    if (!WritesLost(mod) && g_writesLostCount < MAX_MODS) g_writesLost[g_writesLostCount++] = mod;
    ReleaseSRWLockExclusive(&g_writesLock);
    ApiLog(mod, "write: %s; the mod cannot be taken out while the game runs (a new session can)", why);
}

/* A write the mod made: kept so taking the mod out can put the bytes back. */
static void JournalNote(HfMod* mod, uintptr_t address, const uint8_t* before, const void* after, size_t size)
{
    if (size > WRITE_JOURNAL_SIZE) { WritesLose(mod, "a write larger than 4 KiB is not kept"); return; }
    AcquireSRWLockExclusive(&g_writesLock);
    for (int i = g_writeCount - 1; i >= 0; i--)
        if (g_writes[i].mod == mod && g_writes[i].address == address && g_writes[i].size == size)
        {
            memcpy(g_writes[i].bytes + size, after, size);
            ReleaseSRWLockExclusive(&g_writesLock);
            return;
        }
    int room = g_writeCount < g_writeCapacity;
    if (!room && g_writeCapacity < WRITE_JOURNAL_MAX)
    {
        int capacity = g_writeCapacity ? g_writeCapacity * 2 : 256;
        WriteEntry* grown = (WriteEntry*)realloc(g_writes, (size_t)capacity * sizeof *grown);
        if (grown != NULL) { g_writes = grown; g_writeCapacity = capacity; room = 1; }
    }
    uint8_t* bytes = room ? (uint8_t*)malloc(2 * size) : NULL;
    if (bytes != NULL)
    {
        memcpy(bytes, before, size);
        memcpy(bytes + size, after, size);
        g_writes[g_writeCount++] = (WriteEntry){mod, address, (uint32_t)size, bytes};
    }
    ReleaseSRWLockExclusive(&g_writesLock);
    if (bytes == NULL) WritesLose(mod, "the journal of writes is full");
}

/* HfApi.write: the caller is the mod whose DLL the call returns into. Only a live mod's writes are kept: no other mod
 * is ever taken out while the game runs (CodeRemovable), so theirs are written as before SDK 16. */
__attribute__((noinline)) static int ApiModWrite(uintptr_t address, const void* data, size_t size)
{
    HfMod* mod = ModAt((uintptr_t)__builtin_return_address(0));
    if (mod == NULL || !mod->live) return ApiWrite(address, data, size);
    if (ModGone(mod)) { ApiLog(mod, "write refused: the mod is being taken out"); return 1; }
    uint8_t small[256];
    uint8_t* before = size <= sizeof small ? small : (uint8_t*)malloc(size);
    int known = before != NULL && size > 0 && !SafeRead(address, before, size);
    int result = ApiWrite(address, data, size);
    if (!result)
    {
        if (known) JournalNote(mod, address, before, data, size);
        else WritesLose(mod, "a write whose bytes could not be read first is not kept");
    }
    if (before != small) free(before);
    return result;
}

/* HfApi.write_u32_if; kept, as write's, for a live mod only. */
static int ApiModWriteU32If(HfMod* mod, uintptr_t address, uint32_t expect, uint32_t value)
{
    if (mod == NULL || !mod->live) return ApiWriteU32If(mod, address, expect, value);
    if (ModGone(mod)) { ApiLog(mod, "write_u32_if refused: the mod is being taken out"); return 1; }
    uint32_t before = 0;
    int known = address != 0 && !SafeRead(address, &before, 4);
    int result = ApiWriteU32If(mod, address, expect, value);
    if (!result && known && before != value) JournalNote(mod, address, (const uint8_t*)&before, &value, 4);
    return result;
}

/* Bytes into a module's image (code, maybe running): under a freeze, with no thread in them; elsewhere as they are. 0 when
 * written. */
static int JournalPut(uintptr_t address, const uint8_t* bytes, size_t size)
{
    HMODULE module = NULL;
    int image = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module);
    if (!image) return ApiWrite(address, bytes, size);
    for (int tries = 0; tries < 50; tries++)
    {
        FarSuspend();
        if (!FarAnyIn(address, size))
        {
            /* no lock while the others are suspended (one of them may hold it): the write itself, then the note */
            DWORD protect;
            int result = !VirtualProtect((void*)address, size, PAGE_EXECUTE_READWRITE, &protect);
            if (!result)
            {
                memcpy((void*)address, bytes, size);
                VirtualProtect((void*)address, size, protect, &protect);
                FlushInstructionCache(GetCurrentProcess(), (void*)address, size);
            }
            FarResume();
            if (!result) NoteRewrite(address, size);
            return result;
        }
        FarResume();
        Sleep(1);
    }
    return 1;
}

/* The mod's writes put back, newest first: bytes that still hold what it wrote get what was there before. The number
 * put back; *kept counts those someone changed since (left alone). */
static int JournalRevert(HfMod* mod, int* kept)
{
    int reverted = 0;
    *kept = 0;
    AcquireSRWLockExclusive(&g_writesLock);
    for (int i = g_writeCount - 1; i >= 0; i--)
    {
        WriteEntry* w = &g_writes[i];
        if (w->mod != mod) continue;
        uint8_t now[WRITE_JOURNAL_SIZE];
        if (!SafeRead(w->address, now, w->size) && !memcmp(now, w->bytes + w->size, w->size) && !JournalPut(w->address, w->bytes, w->size)) reverted++;
        else (*kept)++;
        free(w->bytes);
        g_writes[i] = g_writes[--g_writeCount];
    }
    ReleaseSRWLockExclusive(&g_writesLock);
    return reverted;
}

/* ---- one step on the game's thread --------------------------------------------------------------------------------- */

/* A HellForge link on the game thread's update (packages.c TickDetour's function) for one call of fn, then out of the
 * chain again: no cost on any other frame. */
static void (*volatile g_liveStepFn)(void*);
static void* g_liveStepArg;
static HANDLE g_liveStepDone;
static TickFn g_liveStepOriginal;

static void LiveStepDetour(void* manager, float dt)
{
    void (*fn)(void*) = (void (*)(void*))InterlockedExchangePointer((void* volatile*)&g_liveStepFn, NULL);
    if (fn != NULL) { fn(g_liveStepArg); SetEvent(g_liveStepDone); }
    g_liveStepOriginal(manager, dt);
}

/* One step at a time: a second caller's would take the first's function out of the slot. */
static SRWLOCK g_liveStepLock = SRWLOCK_INIT;

static int LiveOnGameThreadLocked(void (*fn)(void*), void* arg);

/* Runs fn(arg) on the game thread and waits for it (at most 10 s), after any step under way. 1 when it ran. Without the
 * update hooked nothing runs there (game-thread callbacks, listeners and actions are refused then), so fn runs here. */
static int LiveOnGameThreadNow(void (*fn)(void*), void* arg)
{
    AcquireSRWLockExclusive(&g_liveStepLock);
    int ran = LiveOnGameThreadLocked(fn, arg);
    ReleaseSRWLockExclusive(&g_liveStepLock);
    return ran;
}

/* The same unless a step is under way: then 0 at once, for a caller that tries again later (DifficultyCapTick). */
static int LiveOnGameThreadTry(void (*fn)(void*), void* arg)
{
    if (!TryAcquireSRWLockExclusive(&g_liveStepLock)) return 0;
    int ran = LiveOnGameThreadLocked(fn, arg);
    ReleaseSRWLockExclusive(&g_liveStepLock);
    return ran;
}

static int LiveOnGameThreadLocked(void (*fn)(void*), void* arg)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (g_tickState != 1 || game == 0) { fn(arg); return 1; }
    if (g_liveStepDone == NULL && (g_liveStepDone = CreateEventW(NULL, FALSE, FALSE, NULL)) == NULL) return 0;
    ResetEvent(g_liveStepDone);
    g_liveStepArg = arg;
    InterlockedExchangePointer((void* volatile*)&g_liveStepFn, (void*)fn);
    if (LoaderHook("live change step", game + TICK_UPDATE, (void*)LiveStepDetour, (void**)&g_liveStepOriginal))
    {
        InterlockedExchangePointer((void* volatile*)&g_liveStepFn, NULL);
        return 0;
    }
    DWORD waited = WaitForSingleObject(g_liveStepDone, 10000);
    /* taken back if it has not run: then it never will */
    int ran = waited == WAIT_OBJECT_0 || InterlockedExchangePointer((void* volatile*)&g_liveStepFn, NULL) == NULL;
    if (waited != WAIT_OBJECT_0 && ran) WaitForSingleObject(g_liveStepDone, INFINITE); /* it was running: let it end */
    HookRemove(NULL, game + TICK_UPDATE, (void*)LiveStepDetour);
    return ran;
}
/* The native fixture runs it in place. */
static int (*g_liveOnGameThread)(void (*fn)(void*), void* arg) = LiveOnGameThreadNow;

/* ---- what a mod made through the API, taken away ------------------------------------------------------------------- */

/* On the game thread: its game-thread callbacks, listeners, queued actions and shot rules. */
static void ForgetOnGameThread(void* arg)
{
    const HfMod* mod = (const HfMod*)arg;
    AcquireSRWLockExclusive(&g_lock);
    LONG kept = 0;
    for (LONG i = 0; i < g_tickCount; i++) if (g_ticks[i].mod != mod) g_ticks[kept++] = g_ticks[i];
    InterlockedExchange(&g_tickCount, kept);
    ReleaseSRWLockExclusive(&g_lock);
    AcquireSRWLockExclusive(&g_eventsLock);
    int listeners = 0;
    LONG listening = 0;
    for (int i = 0; i < g_listenerCount; i++)
        if (g_listeners[i].mod != mod) { g_listeners[listeners++] = g_listeners[i]; listening |= 1L << g_listeners[i].kind; }
    g_listenerCount = listeners;
    InterlockedExchange(&g_listening, listening);
    ReleaseSRWLockExclusive(&g_eventsLock);
    AcquireSRWLockExclusive(&g_actQueueLock);
    int queued = 0;
    for (int i = 0; i < g_actQueued; i++) if (g_actQueue[i].mod != mod) g_actQueue[queued++] = g_actQueue[i];
    g_actQueued = queued;
    ReleaseSRWLockExclusive(&g_actQueueLock);
    AcquireSRWLockExclusive(&g_actRulesLock);
    int rules = 0;
    for (int i = 0; i < g_actRuleCount; i++) if (g_actRules[i].mod != mod) g_actRules[rules++] = g_actRules[i];
    g_actRuleCount = rules;
    ReleaseSRWLockExclusive(&g_actRulesLock);
}

static void CallbacksForget(const HfMod* mod)
{
    AcquireSRWLockExclusive(&g_callbacksLock);
    int kept = 0;
    for (int i = 0; i < g_callbackCount; i++) if (g_callbacks[i].mod != mod) g_callbacks[kept++] = g_callbacks[i];
    g_callbackCount = kept;
    ReleaseSRWLockExclusive(&g_callbacksLock);
}

/* Its live attachment values and status multipliers (live.c set_stat): its layer goes, the address takes the top left
 * (another rank's, else what it held before). */
static void ExtrasForget(const HfMod* mod)
{
    AcquireSRWLockExclusive(&g_statsLock);
    for (int i = 0; i < g_liveExtraCount; i++)
    {
        LiveExtra* x = &g_liveExtra[i];
        int touched = 0;
        for (int r = RANK_CODE; r <= RANK_EDITOR; r++)
            if (x->layer[r].held && x->layer[r].mod == mod) { x->layer[r].held = 0; x->layer[r].mod = NULL; touched = 1; }
        if (!touched) continue;
        uint32_t want = x->before;
        for (int r = RANK_CODE; r <= RANK_EDITOR; r++) if (x->layer[r].held) want = x->layer[r].bits;
        ApiWrite(x->address, &want, 4);
    }
    ReleaseSRWLockExclusive(&g_statsLock);
}

/* The bytes under a field's deferred edits: the first applied one's before. 0 when none is applied. g_fieldsLock held. */
static int FieldBottom(const FieldEdit* field, uint8_t* out)
{
    for (int i = 0; i < g_editCount; i++)
    {
        const FieldEdit* e = &g_edits[i];
        if (e->live || e->failed || e->waiting || !e->hasBefore || !SameField(e, field)) continue;
        memcpy(out, e->before, e->field->size);
        return 1;
    }
    return 0;
}

/* A field's deferred edits applied again, in order, over `bottom` (NULL: what is under them now, the first one's
 * before): after one went in among them (a live set-up, fields.c QueueEdit) or came out. A live edit above keeps the
 * field: its original (what a reset puts back) takes the result. g_fieldsLock held. */
static void FieldRestack(const FieldEdit* field, const uint8_t* bottom)
{
    uint32_t size = field->field->size;
    uint8_t under[FIELD_BEFORE], top[FIELD_BEFORE], result[FIELD_BEFORE];
    if (size > FIELD_BEFORE) return;
    if (bottom == NULL && FieldBottom(field, under)) bottom = under;
    int waiting = 0;
    uint8_t* record = EditRecord(field, field->entity, 0, &waiting);
    if (record == NULL) return;
    uintptr_t at = (uintptr_t)record + field->field->offset;
    FieldEdit* live = NULL;
    for (int i = 0; i < g_editCount && live == NULL; i++)
        if (g_edits[i].live && !g_edits[i].failed && g_edits[i].hasOriginal && SameField(&g_edits[i], field)) live = &g_edits[i];
    if (live != NULL && SafeRead(at, top, size)) return;
    if (bottom != NULL) ApiWrite(at, bottom, size);
    for (int i = 0; i < g_editCount; i++)
    {
        FieldEdit* e = &g_edits[i];
        if (e->live || !SameField(e, field)) continue;
        e->hasBefore = 0;
        ApplyEdit(e);
    }
    if (live == NULL || SafeRead(at, result, size)) return;
    if (size <= sizeof live->original) memcpy(live->original, result, size);
    ApiWrite(at, top, size);
}

/* Its deferred edits (each field's others applied again over what was under it) and its live edits (reset). */
static void EditsForget(HfMod* mod)
{
    AcquireSRWLockExclusive(&g_fieldsLock);
    for (int i = g_editCount - 1; i >= 0; i--)
    {
        FieldEdit* e = &g_edits[i];
        if (e->mod != mod) continue;
        if (e->live) { LiveLeave(i); RemoveEdit(i); continue; }
        FieldEdit field = *e;
        uint8_t bottom[FIELD_BEFORE];
        int known = field.field->size <= FIELD_BEFORE && FieldBottom(&field, bottom);
        RemoveEdit(i);
        if (known) FieldRestack(&field, bottom);
    }
    ReleaseSRWLockExclusive(&g_fieldsLock);
}

/* Live threads whose start address is in the mod's DLL. */
typedef LONG (WINAPI* NtQueryThreadFn)(HANDLE thread, int what, void* out, ULONG size, ULONG* returned);
static int ThreadsIn(const HfMod* mod)
{
    static NtQueryThreadFn query;
    if (query == NULL) query = (NtQueryThreadFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    if (query == NULL || mod->imageBase == 0) return 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry = {sizeof entry};
    DWORD process = GetCurrentProcessId();
    int found = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != process) continue;
        HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == NULL) continue;
        uintptr_t start = 0;
        if (query(thread, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, NULL) == 0 && start >= mod->imageBase && start < mod->imageEnd)
            found++;
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return found;
}

/* Why a running code mod cannot be taken out now ("<reason>\t<why>" in why), or NULL. Before anything changes. */
static const char* CodeRemovable(const HfMod* mod, char* why, size_t size)
{
    if (!mod->live) { snprintf(why, size, "capability\t%s runs code that does not declare \"live\": true, which changes at the next session", mod->id); return why; }
    if (WritesLost(mod)) { snprintf(why, size, "capability\t%s wrote more than HellForge keeps to put back; it changes at the next session", mod->id); return why; }
    if (mod->unload == NULL && ThreadsIn(mod) > 0)
    { snprintf(why, size, "capability\t%s runs threads of its own and exports no hellforge_mod_unload to stop them", mod->id); return why; }
    return NULL;
}

/* Takes a running code mod out (the top of this file). reason: HF_UNLOAD_REMOVE or HF_UNLOAD_REPLACE. 0 when the game is
 * as if it had never set up; 1 when something of it may be left (logged: the session should restart). */
static int CodeRemove(HfMod* mod, uint32_t reason)
{
    InterlockedExchange(&mod->removing, 1);
    int failed = !g_liveOnGameThread(ForgetOnGameThread, mod);
    CallbacksForget(mod);
    int hooks = HookRemoveMod(mod);
    if (mod->unload != NULL)
    {
        Note("%s: hellforge_mod_unload (%s)", mod->id, reason == HF_UNLOAD_REPLACE ? "replaced" : "removed");
        int declined = mod->unload(&g_api, mod, reason);
        if (declined) { Note("%s: hellforge_mod_unload returned %d: what it started may still run", mod->id, declined); failed = 1; }
    }
    for (int waited = 0; mod->imageBase && ThreadsIn(mod) > 0 && waited < 2000; waited += 50) Sleep(50);
    if (mod->imageBase && ThreadsIn(mod) > 0) { Note("%s: a thread it started still runs", mod->id); failed = 1; }
    int kept = 0, reverted = JournalRevert(mod, &kept);
    EditsForget(mod);
    ExtrasForget(mod);
    NetForget(mod);
    AcquireSRWLockExclusive(&g_passivesLock);
    int passives = g_passivesMod == mod;
    ReleaseSRWLockExclusive(&g_passivesLock);
    if (passives) ApiSetPassives(mod, HF_PASSIVE_KIT, HF_PASSIVE_KIT);
    mod->loaded = 0;
    mod->removed = 1;
    Note("%s: taken out (%d hook(s), %d write(s) put back%s%s)", mod->id, hooks, reverted, kept ? ", some bytes changed since and left alone" : "",
        failed ? "; something of it may be left: restart the session to be sure" : "");
    return failed;
}

/* ---- set-up while the game runs ------------------------------------------------------------------------------------ */

typedef int (*LiveInitFunction)(const HfApi* api, HfMod* mod);

/* A live mod's DLL loaded into its entry. NULL, or "<reason>\t<why>" in why. */
static const char* CodeLoad(HfMod* mod, const char* dll, char* why, size_t size)
{
    wchar_t path[MAX_PATH];
    Utf8ToWide(dll, path, MAX_PATH);
    if (!path[0]) { snprintf(why, size, "malformed\t%s: its DLL's path", mod->id); return why; }
    wcscpy(mod->dll, path);
    mod->module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (mod->module == NULL) { snprintf(why, size, "capability\t%s: its DLL could not be loaded (error %lu)", mod->id, GetLastError()); return why; }
    ModImage(mod);
    if (GetProcAddress(mod->module, "hellforge_mod_init") == NULL) { snprintf(why, size, "capability\t%s exports no hellforge_mod_init", mod->id); return why; }
    return NULL;
}

/* Its hellforge_mod_init, with on() calling back after it and its edits in load order. NULL, or "<reason>\t<why>". */
static const char* CodeInit(HfMod* mod, char* why, size_t size)
{
    LiveInitFunction init = (LiveInitFunction)(void*)GetProcAddress(mod->module, "hellforge_mod_init");
    mod->phase = HF_LOAD_LIVE;
    mod->loaded = 1;
    Note("%s: initializing while the game runs", mod->id);
    g_liveSetup = mod;
    int result = init(&g_api, mod);
    g_liveSetup = NULL;
    if (result == 0) return NULL;
    snprintf(why, size, "capability\t%s declined to set up while the game runs (%d): %s", mod->id, result, mod->last[0] ? mod->last : "its log says why");
    return why;
}

/* After the change is checked: the load events that fired, in their order, to the mod set up live (its edits still go
 * in load order meanwhile). */
static void CodeReplay(HfMod* mod)
{
    g_liveSetup = mod;
    for (LONG i = 0; i < g_fireCount; i++) DeliverTo(g_fireOrder[i], mod);
    g_liveSetup = NULL;
    Note("%s: loaded while the game runs (%ld load event(s) delivered)", mod->id, (long)g_fireCount);
}

/* SDK 16, load_phase. */
static uint32_t ApiLoadPhase(HfMod* mod) { return mod != NULL ? mod->phase : HF_LOAD_BOOT; }
