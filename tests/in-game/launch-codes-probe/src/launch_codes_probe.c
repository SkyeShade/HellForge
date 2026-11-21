/* Launch codes probe (in-game test only, never shipped; tests/in-game/launch-codes.json): level 11's launch code count
 * (native/mod-loader/difficulty_codes.c). The game shows a keypad code's digits by 0x9B9820(key): revealed / total over
 * every live KeypadCodeReveal instance whose record has the code's key 0xE8153A02, which the SSSD hard drives share.
 * With second primaries the count's lookup call (0x9B9864) goes to a stub that passes on the Launch Codes only.
 *   - "stub": the call no longer reaches the lookup 0x4F6A80 directly.
 *   - "direct": the stub and the lookup asked for each of the seven carriers: the Launch Codes keep their key, every
 *     other carrier gets 0 from the stub and 0xE8153A02 from the lookup.
 *   - "carriers" (every 5 s in a mission, when it changes): the live carriers, and the game's own count against the
 *     Launch Codes' share and every carrier's share. With carriers other than the Launch Codes in the level, the count
 *     must be the Launch Codes' share ("filter ok").
 * Reads only: nothing is revealed or written. "PROBE <check> ok" or "PROBE <check> FAIL <why>" per check. */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hellforge_mod.h"

static HfMod* g_mod;
static const HfApi* hf;
#define LOG(...) hf->log(g_mod, __VA_ARGS__)

/* game.dll 25480438 */
#define CODE_CALL 0x9B9864u      /* call 0x4F6A80 in 0x9B9820 */
#define CODE_FRACTION 0x9B9820u  /* float (unused, key): revealed / total, 1 with none */
#define CODE_LOOKUP 0x4F6A80u    /* KeypadCodeRevealComponentData's record by resource hash, or NULL */
#define COMPONENT_DATA 0x346BF98u /* the global the lookup reads; its table at +0xF125D8 */
#define CARRIERS 0x3326470u      /* the KeypadCodeReveal manager: +0xC count, +0x30 instances, +0x38 revealed bytes */
#define CODE_KEY 0xE8153A02u

typedef const uint32_t* (*LookupFn)(uint64_t resource);
typedef float (*FractionFn)(void* unused, uint32_t key);

static const struct { uint64_t resource; const char* name; int code; } kCarriers[] = {
    {0xF48E9DEC9CC20D8Aull, "Launch Codes (intel)", 1},
    {0x5684D928C9AB00D1ull, "Launch Codes (carried)", 1},
    {0xF41432892465C5FDull, "SSSD Hard Drive", 0},
    {0x33F2BCDAE3A12592ull, "SSSD Hard Drive (stratagem)", 0},
    {0x9867D9EC6AC7E680ull, "Access Codes", 0},
    {0x3E099DDF97ACF85Full, "Specimen Case", 0},
    {0xA1E90D748B3D8AAAull, "Terminal", 0},
};
#define CARRIER_KINDS (sizeof kCarriers / sizeof kCarriers[0])

static uintptr_t g_game, g_stub;
static int g_stubChecked, g_directDone, g_filterOk, g_filterFail;
static uint32_t g_last[5] = {0xFFFFFFFFu};

static uintptr_t Pointer(uintptr_t at) { uintptr_t v = 0; return at != 0 && hf->read(at, &v, 8) == 0 ? v : 0; }
static uint32_t U32(uintptr_t at) { uint32_t v = 0; return at != 0 && hf->read(at, &v, 4) == 0 ? v : 0xFFFFFFFFu; }
static uint8_t U8(uintptr_t at) { uint8_t v = 0; return at != 0 && hf->read(at, &v, 1) == 0 ? v : 0; }
static uint32_t Key(const uint32_t* record) { return record ? U32((uintptr_t)record) : 0xFFFFFFFFu; }

static int IsCode(uint64_t resource)
{
    for (size_t i = 0; i < CARRIER_KINDS; i++) if (kCarriers[i].resource == resource) return kCarriers[i].code;
    return 0;
}

static void CheckStub(void)
{
    uint8_t call[5];
    g_stubChecked = 1;
    if (hf->read(g_game + CODE_CALL, call, sizeof call) != 0 || call[0] != 0xE8) { LOG("PROBE stub FAIL no call at game.dll+0x%X", CODE_CALL); return; }
    int32_t rel;
    memcpy(&rel, call + 1, 4);
    uintptr_t target = g_game + CODE_CALL + 5 + (intptr_t)rel;
    if (target == g_game + CODE_LOOKUP) { LOG("PROBE stub FAIL the count still calls the lookup (no second primaries?)"); return; }
    g_stub = target;
    LOG("PROBE stub ok: the count calls %p, not the lookup", (void*)target);
}

/* Once the component data is there: each carrier asked of the stub and of the lookup. */
static void CheckDirect(void)
{
    LookupFn lookup = (LookupFn)(g_game + CODE_LOOKUP), stub = (LookupFn)g_stub;
    if (Key(lookup(kCarriers[0].resource)) != CODE_KEY) return; /* not loaded yet */
    int failed = 0;
    for (size_t i = 0; i < CARRIER_KINDS; i++)
    {
        uint32_t viaLookup = Key(lookup(kCarriers[i].resource)), viaStub = Key(stub(kCarriers[i].resource));
        uint32_t want = kCarriers[i].code ? CODE_KEY : 0;
        int ok = viaLookup == CODE_KEY && viaStub == want;
        failed |= !ok;
        LOG("PROBE direct %s %016llx: lookup %08X, stub %08X (want %08X) %s", kCarriers[i].name, (unsigned long long)kCarriers[i].resource,
            viaLookup, viaStub, want, ok ? "ok" : "FAIL");
    }
    LOG(failed ? "PROBE direct FAIL" : "PROBE direct ok: the Launch Codes keep the code's key, the other six carriers get 0");
    g_directDone = 1;
}

static void CheckCarriers(void)
{
    uintptr_t manager = Pointer(g_game + CARRIERS);
    if (manager == 0) return;
    uint32_t count = U32(manager + 0xC);
    if (count == 0 || count > 4096) return;
    uintptr_t instances = Pointer(manager + 0x30), revealed = Pointer(manager + 0x38);
    if (instances == 0 || revealed == 0) return;
    LookupFn lookup = (LookupFn)(g_game + CODE_LOOKUP);
    uint32_t all = 0, allRevealed = 0, codes = 0, codesRevealed = 0;
    char kinds[512] = "";
    size_t used = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uintptr_t instance = Pointer(instances + (uintptr_t)i * 8);
        uint64_t resource = (uint64_t)Pointer(instance);
        if (instance == 0 || resource == 0) continue;
        uint32_t key = Key(lookup(resource));
        uint8_t shown = U8(revealed + i) != 0;
        if (used + 40 < sizeof kinds) used += (size_t)snprintf(kinds + used, sizeof kinds - used, " %016llx/%08X/r%u", (unsigned long long)resource, key, shown);
        if (key != CODE_KEY) continue;
        all++, allRevealed += shown;
        if (IsCode(resource)) codes++, codesRevealed += shown;
    }
    uint32_t now[5] = {count, all, allRevealed, codes, codesRevealed};
    if (memcmp(now, g_last, sizeof now) == 0) return;
    memcpy(g_last, now, sizeof now);
    float fraction = ((FractionFn)(g_game + CODE_FRACTION))(NULL, CODE_KEY);
    float codesShare = codes ? (float)codesRevealed / (float)codes : 1.0f, allShare = all ? (float)allRevealed / (float)all : 1.0f;
    LOG("PROBE carriers: %u live, %u with the code's key (%u revealed), %u Launch Codes (%u revealed); the count says %.3f, the Launch Codes' share %.3f, every carrier's %.3f;%s",
        count, all, allRevealed, codes, codesRevealed, fraction, codesShare, allShare, kinds);
    if (all == codes) { LOG("PROBE filter n/a: only Launch Codes carry the code here"); return; }
    if (fraction - codesShare < 1e-4f && codesShare - fraction < 1e-4f && (codesShare != allShare))
    {
        g_filterOk = 1;
        LOG("PROBE filter ok: with %u other carrier(s) in the level the count is the Launch Codes' share", all - codes);
    }
    else if (fraction - codesShare < 1e-4f && codesShare - fraction < 1e-4f)
        LOG("PROBE filter same: both shares are %.3f here", fraction);
    else
    {
        g_filterFail = 1;
        LOG("PROBE filter FAIL: the count is %.3f, the Launch Codes' share %.3f", fraction, codesShare);
    }
}

static void Tick(HfMod* mod, float dt, void* user)
{
    static float clock;
    (void)mod, (void)user;
    if ((clock += dt) < 5.0f) return;
    clock = 0;
    if (g_game == 0 && (g_game = hf->module_base("game.dll", NULL)) == 0) return;
    if (!g_stubChecked) CheckStub();
    if (g_stub == 0) return;
    uintptr_t data = Pointer(g_game + COMPONENT_DATA);
    if (data == 0 || Pointer(data + 0xF125D8) == 0) return;
    if (!g_directDone) CheckDirect();
    CheckCarriers();
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hf = api;
    g_mod = mod;
    if (api->version < 6 || api->size < offsetof(HfApi, on_game_thread) + sizeof(void*)) { api->log(mod, "PROBE sdk FAIL the loader is SDK %u", api->version); return 1; }
    LOG("PROBE sdk ok (SDK %u)", api->version);
    LOG(api->on_game_thread(mod, Tick, NULL) ? "PROBE game_thread ok" : "PROBE game_thread FAIL refused");
    return 0;
}
