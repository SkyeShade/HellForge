/* Game globals by name (SDK 16; mappings/globals.json, hellforge_globals.h, docs/modding/hooks.md "Game globals"):
 * global. A mapped global is a cell in its module's data that holds a pointer to one of the game's objects, at its rva
 * in the build the mappings were reviewed on (sdk_globals.h, generated), with a witness: an instruction of the game's
 * code that loads the cell, with the bytes after it (the generator checked that each points at the cell). The cell is
 * handed out only on that build, from HF_EVENT_GAME_LOADED on, once one witness reads as reviewed through any hook in
 * front of it (code_matches). The plan's `global` lines add or replace entries (the smoke test's fake game). Included
 * by hellforge_modloader.c after functions.c. */

/* Up to 3 witnesses (code NULL past the last); any one of them reading as reviewed is enough, so a later patch over one
 * (HellForge's or a mod's) does not lose the global. A former name is a row of its own. */
typedef struct SdkGlobal { const char* name; const char* module; uint32_t rva; uint32_t witness[3]; const char* code[3]; } SdkGlobal;
#include "sdk_globals.h"

#define GL_MAX 256
#define GL_PLAN_MAX 16

/* A global as this session has it: the plan's, then the build's mapped ones (names first come, first kept). */
typedef struct GlEntry { const SdkGlobal* g; int state; int warned; } GlEntry; /* state 1: its witness read as reviewed */
static GlEntry g_globals[GL_MAX];
static int g_globalCount;
static int g_globalsMappedHere;
static SdkGlobal g_glPlan[GL_PLAN_MAX];
static char g_glPlanText[GL_PLAN_MAX][3][800];
static int g_glPlanCount;
static SRWLOCK g_glLock = SRWLOCK_INIT;
static volatile LONG g_glUnknownWarned;

/* A plan line `global <name> <module> <rva hex> <witness rva hex> <witness code>`: 1 when it is one (taken, or
 * skipped past GL_PLAN_MAX). */
static int GlPlanLine(char** f, int n)
{
    if (n != 6 || strcmp(f[0], "global")) return 0;
    if (g_glPlanCount >= GL_PLAN_MAX) return 1;
    char (*text)[800] = g_glPlanText[g_glPlanCount];
    snprintf(text[0], sizeof text[0], "%s", f[1]);
    snprintf(text[1], sizeof text[1], "%s", f[2]);
    snprintf(text[2], sizeof text[2], "%s", f[5]);
    g_glPlan[g_glPlanCount++] = (SdkGlobal){text[0], text[1], (uint32_t)strtoul(f[3], NULL, 16), {(uint32_t)strtoul(f[4], NULL, 16)}, {text[2]}};
    return 1;
}

static void GlAdd(const SdkGlobal* g)
{
    for (int i = 0; i < g_globalCount; i++) if (!strcmp(g_globals[i].g->name, g->name)) return;
    if (g_globalCount < GL_MAX) g_globals[g_globalCount++] = (GlEntry){g, 0, 0};
}

/* With FnStart, before any mod loads: the session's globals. */
static void GlStart(void)
{
    for (int i = 0; i < g_glPlanCount; i++) GlAdd(&g_glPlan[i]);
    g_globalsMappedHere = strcmp(g_build, SDK_GLOBALS_BUILD) == 0;
    if (g_globalsMappedHere) for (const SdkGlobal* g = kSdkGlobals; g->name != NULL; g++) GlAdd(g);
    else Note("globals: the mappings are for game build %s, the session's is %s: only the plan's %d global(s) by name",
        SDK_GLOBALS_BUILD, g_build, g_glPlanCount);
}

static GlEntry* GlFind(const char* name)
{
    for (int i = 0; name != NULL && i < g_globalCount; i++) if (!strcmp(g_globals[i].g->name, name)) return &g_globals[i];
    return NULL;
}

/* Why a name has no entry, for the mod's log. */
static const char* GlMissing(const char* name)
{
    if (name == NULL) return "no name";
    for (const SdkGlobal* g = kSdkGlobals; !g_globalsMappedHere && g->name != NULL; g++)
        if (!strcmp(g->name, name)) return "mapped for another game build (HellForge has not mapped this one yet)";
    return "not a mapped global (hellforge_globals.h names them)";
}

/* The cell's address once its witness reads as reviewed; 0 not yet or not at all. Under g_glLock. */
static uintptr_t GlAddress(GlEntry* e)
{
    uintptr_t base = ApiModuleBase(e->g->module, NULL);
    if (base == 0) return 0;
    for (int i = 0; i < 3 && e->state == 0 && e->g->code[i] != NULL; i++) if (ApiCodeMatches(base + e->g->witness[i], e->g->code[i])) e->state = 1;
    return e->state == 1 ? base + e->g->rva : 0;
}

/* HfApi.global (SDK 16). A mod may ask every frame: each name's failure is logged once (unknown names 32 times in all). */
static uintptr_t ApiGlobal(HfMod* mod, const char* name)
{
    AcquireSRWLockExclusive(&g_glLock);
    GlEntry* e = GlFind(name);
    int loaded = g_fired[HF_EVENT_GAME_LOADED] != 0;
    uintptr_t at = e != NULL && loaded ? GlAddress(e) : 0;
    int warn = e != NULL && at == 0 && (!loaded || !e->warned);
    if (warn && loaded) e->warned = 1;
    ReleaseSRWLockExclusive(&g_glLock);
    if (e == NULL && InterlockedIncrement(&g_glUnknownWarned) <= 32) ApiLog(mod, "global %s: %s", name ? name : "(null)", GlMissing(name));
    else if (warn && !loaded) ApiLog(mod, "global %s: the game is not loaded yet (ask from HF_EVENT_GAME_LOADED on)", name);
    else if (warn) ApiLog(mod, "global %s: none of the code that loads it (%s+0x%x first) is the reviewed one (or it is not decoded yet)",
        name, e->g->module, (unsigned)e->g->witness[0]);
    return at;
}
