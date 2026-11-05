/* A live code mod for the mod loader's fixture (live_mods_test.c, SDK 16): set up while the game runs, replaced by a
 * copy of itself, then taken out. Once the "game" is loaded (an event the loader replays to it) it hooks the fake game's
 * function, adding 7, and writes 42 into the fake game's cell through the API. It counts the load events it hears and
 * HF_EVENT_MODS_CHANGED, and keeps how it was set up and why it was unloaded, for the fixture to read through its
 * exports. */
#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <windows.h>
#include "hellforge_mod.h"

typedef int (*Target)(int);
static const HfApi* hf;
static Target original;
__declspec(dllexport) volatile LONG live_test_phase = -1, live_test_loaded, live_test_changed, live_test_unloaded;

static int Detour(int x) { return original(x) + 7; }

static void Loaded(HfMod* mod, HfEvent event, void* user)
{
    InterlockedIncrement(&live_test_loaded);
    HMODULE game = GetModuleHandleA("fake_game.dll");
    uintptr_t target = (uintptr_t)GetProcAddress(game, "hellforge_fake_target");
    uintptr_t cell = (uintptr_t)GetProcAddress(game, "hellforge_fake_cell");
    uint32_t value = 42;
    if (target != 0 && hf->hook(mod, target, (void*)Detour, (void**)&original) == 0) hf->log(mod, "detour installed");
    if (cell != 0 && hf->write(cell, &value, sizeof value) == 0) hf->log(mod, "cell written");
}

static void Changed(HfMod* mod, HfEvent event, void* user) { InterlockedIncrement(&live_test_changed); }

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hf = api;
    if (hf->size < offsetof(HfApi, load_phase) + sizeof hf->load_phase) return 1; /* SDK 16 */
    live_test_phase = (LONG)hf->load_phase(mod);
    if (hf->on(mod, HF_EVENT_GAME_LOADED, Loaded, NULL)) return 1;
    return hf->on(mod, HF_EVENT_MODS_CHANGED, Changed, NULL);
}

HF_MOD_EXPORT int hellforge_mod_unload(const HfApi* api, HfMod* mod, uint32_t reason)
{
    InterlockedExchange(&live_test_unloaded, (LONG)reason);
    return 0;
}
