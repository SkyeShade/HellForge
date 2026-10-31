/* Game functions by name (SDK 16; mappings/functions.json, hellforge_functions.h, docs/modding/hooks.md "Hooking by
 * name"): function and hook_function. A mapped function is its module, its rva in the build the mappings were reviewed
 * on (sdk_functions.h, generated) and its first bytes as reviewed; it is used only on that build, and only while its
 * code reads as reviewed through every hook in front of it (HookedCode). The plan's `function` lines add or replace
 * entries (the smoke test's fake game). A hook asked for before the code reads as reviewed waits in a queue the watch
 * loop works through (WatchInstalls), as HellForge's own event hooks do: 60 s after HF_EVENT_GAME_LOADED for the copy
 * protection to decode it, then refused. Included by hellforge_modloader.c after hooks.c. */

typedef struct SdkFunction { const char* name; const char* module; uint32_t rva; const char* code; int hook; } SdkFunction;
#include "sdk_functions.h"

#define FN_MAX 512
#define FN_PLAN_MAX 32
#define FN_HOOKS_MAX 256
#define FN_WAIT_MS 60000

/* A function as this session has it: the build's mapped ones, then the plan's (names first come, first kept). */
typedef struct FnEntry { const SdkFunction* f; int state; int warned; } FnEntry; /* state 1: its code read as reviewed once */
static FnEntry g_fns[FN_MAX];
static int g_fnCount;
static int g_fnMappedHere; /* the session's build is the mappings' */

/* The plan's own entries (function <name> <module> <rva hex> <hook 0|1> <code>), kept as the table's are. */
static SdkFunction g_fnPlan[FN_PLAN_MAX];
static char g_fnPlanText[FN_PLAN_MAX][3][800];
static int g_fnPlanCount;

typedef struct FnHook { HfMod* mod; FnEntry* fn; void* detour; void** original; int32_t priority; int state; } FnHook; /* 1 in, -1 refused */
static FnHook g_fnHooks[FN_HOOKS_MAX];
static int g_fnHookCount;
static SRWLOCK g_fnLock = SRWLOCK_INIT;

/* A plan line `function ...`: 1 when it is one (taken, or skipped when malformed or past FN_PLAN_MAX). */
static int FnPlanLine(char** f, int n)
{
    if (n != 6 || strcmp(f[0], "function")) return 0;
    if (g_fnPlanCount >= FN_PLAN_MAX) return 1;
    char (*text)[800] = g_fnPlanText[g_fnPlanCount];
    snprintf(text[0], sizeof text[0], "%s", f[1]);
    snprintf(text[1], sizeof text[1], "%s", f[2]);
    snprintf(text[2], sizeof text[2], "%s", f[5]);
    g_fnPlan[g_fnPlanCount++] = (SdkFunction){text[0], text[1], (uint32_t)strtoul(f[3], NULL, 16), text[2], f[4][0] == '1'};
    return 1;
}

static void FnAdd(const SdkFunction* f)
{
    for (int i = 0; i < g_fnCount; i++) if (!strcmp(g_fns[i].f->name, f->name)) return;
    if (g_fnCount < FN_MAX) g_fns[g_fnCount++] = (FnEntry){f, 0, 0};
}

/* After the plan is read, before any mod loads: the session's table. */
static void FnStart(void)
{
    for (int i = 0; i < g_fnPlanCount; i++) FnAdd(&g_fnPlan[i]);
    g_fnMappedHere = strcmp(g_build, SDK_FUNCTIONS_BUILD) == 0;
    if (g_fnMappedHere) for (const SdkFunction* f = kSdkFunctions; f->name != NULL; f++) FnAdd(f);
    else Note("functions: the mappings are for game build %s, the session's is %s: only the plan's %d function(s) by name",
        SDK_FUNCTIONS_BUILD, g_build, g_fnPlanCount);
}

static FnEntry* FnFind(const char* name)
{
    for (int i = 0; name != NULL && i < g_fnCount; i++) if (!strcmp(g_fns[i].f->name, name)) return &g_fns[i];
    return NULL;
}

/* Why a name has no entry, for the mod's log. */
static const char* FnMissing(const char* name)
{
    if (name == NULL) return "no name";
    for (const SdkFunction* f = kSdkFunctions; !g_fnMappedHere && f->name != NULL; f++)
        if (!strcmp(f->name, name)) return "mapped for another game build (HellForge has not mapped this one yet)";
    return "not a mapped function (hellforge_functions.h names them)";
}

/* The function's address once its code reads as reviewed (through hooks in front of it); 0 not yet or not at all. */
static uintptr_t FnAddress(FnEntry* e)
{
    uintptr_t base = ApiModuleBase(e->f->module, NULL);
    if (base == 0) return 0;
    if (e->state == 1 || ApiCodeMatches(base + e->f->rva, e->f->code)) { e->state = 1; return base + e->f->rva; }
    return 0;
}

static volatile LONG g_fnUnknownWarned;

/* HfApi.function (SDK 16). A mod may ask every frame: each name's failure is logged once (unknown names 32 times in
 * all). */
static uintptr_t ApiFunction(HfMod* mod, const char* name)
{
    AcquireSRWLockExclusive(&g_fnLock);
    FnEntry* e = FnFind(name);
    uintptr_t at = e != NULL && g_fired[HF_EVENT_GAME_LOADED] ? FnAddress(e) : 0;
    int loaded = g_fired[HF_EVENT_GAME_LOADED] != 0;
    int warn = e != NULL && at == 0 && (!loaded || !e->warned);
    if (warn && loaded) e->warned = 1;
    ReleaseSRWLockExclusive(&g_fnLock);
    if (e == NULL && InterlockedIncrement(&g_fnUnknownWarned) <= 32) ApiLog(mod, "function %s: %s", name ? name : "(null)", FnMissing(name));
    else if (warn && !loaded) ApiLog(mod, "function %s: the game is not loaded yet (ask from HF_EVENT_GAME_LOADED on)", name);
    else if (warn) ApiLog(mod, "function %s: its code at %s+0x%x is not the reviewed one (or not decoded yet)", name, e->f->module, (unsigned)e->f->rva);
    return at;
}

/* A waiting hook: in once its code reads as reviewed, refused 60 s after the game loaded. Under g_fnLock. */
static void FnHookTry(FnHook* h, ULONGLONG waited)
{
    uintptr_t at = FnAddress(h->fn);
    if (at == 0)
    {
        if (waited < FN_WAIT_MS) return;
        h->state = -1;
        ApiLog(h->mod, "hook %s refused: its code at %s+0x%x is not the reviewed one", h->fn->f->name, h->fn->f->module, (unsigned)h->fn->f->rva);
        return;
    }
    h->state = HookAdd(h->mod, NULL, at, h->detour, h->original, h->priority) ? -1 : 1;
    if (h->state == 1) ApiLog(h->mod, "hooked %s by name", h->fn->f->name);
}

static ULONGLONG g_fnLoadedAt;

/* The watch loop, after HF_EVENT_GAME_LOADED: the hooks waiting for their code. */
static void FnHooksTick(void)
{
    if (!g_fired[HF_EVENT_GAME_LOADED]) return;
    ULONGLONG now = GetTickCount64();
    if (!g_fnLoadedAt) g_fnLoadedAt = now;
    AcquireSRWLockExclusive(&g_fnLock);
    for (int i = 0; i < g_fnHookCount; i++) if (g_fnHooks[i].state == 0) FnHookTry(&g_fnHooks[i], now - g_fnLoadedAt);
    ReleaseSRWLockExclusive(&g_fnLock);
}

/* Why a hook by name is refused before it is queued, or NULL. */
static const char* FnHookRefusal(const FnEntry* e, const char* name, void* detour, void** original)
{
    if (e == NULL) return FnMissing(name);
    if (!e->f->hook) return "only for calling (its whole signature is not known)";
    if (detour == NULL) return "no detour";
    return original == NULL ? "no variable for the original" : NULL;
}

/* Why the queue refuses it (the same detour asked for twice, the queue full), or NULL. Under g_fnLock. */
static const char* FnHookQueueRefusal(const FnEntry* e, void* detour)
{
    for (int i = 0; i < g_fnHookCount; i++)
        if (g_fnHooks[i].fn == e && g_fnHooks[i].detour == detour && g_fnHooks[i].state >= 0) return "asked for with this detour already";
    return g_fnHookCount >= FN_HOOKS_MAX ? "too many hooks by name (256)" : NULL;
}

/* The hook queued, and tried at once when the game is loaded: its state (0 waiting, 1 in, -1 refused). Under
 * g_fnLock. */
static int FnHookQueue(HfMod* mod, FnEntry* e, void* detour, void** original, int32_t priority)
{
    FnHook* h = &g_fnHooks[g_fnHookCount++];
    *h = (FnHook){mod, e, detour, original, priority, 0};
    if (g_fired[HF_EVENT_GAME_LOADED]) FnHookTry(h, g_fnLoadedAt ? GetTickCount64() - g_fnLoadedAt : 0);
    return h->state;
}

/* HfApi.hook_function (SDK 16). */
static int ApiHookFunction(HfMod* mod, const char* name, void* detour, void** original, int32_t priority)
{
    if (mod == NULL) return 1;
    FnEntry* e = FnFind(name);
    const char* why = FnHookRefusal(e, name, detour, original);
    int state = -1;
    if (why == NULL)
    {
        AcquireSRWLockExclusive(&g_fnLock);
        why = FnHookQueueRefusal(e, detour);
        if (why == NULL) state = FnHookQueue(mod, e, detour, original, priority);
        ReleaseSRWLockExclusive(&g_fnLock);
    }
    if (why != NULL) ApiLog(mod, "hook %s refused: %s", name ? name : "(null)", why);
    else if (state == 0) ApiLog(mod, "hook %s: waiting for the game's code", name);
    return state < 0;
}
