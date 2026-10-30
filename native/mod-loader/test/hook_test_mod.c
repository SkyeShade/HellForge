/* A code mod for the mod loader smoke test: once the "game" is loaded, it finds the decode pin by pattern, checks
 * hellforge_fake_target the way a mod checks a function before hooking it (SDK 13 code_matches: its bytes from before
 * any hook, so a second copy of this mod hooking after the first still passes) and detours it so it returns 1000 more
 * than the next in its chain. Its status (SDK 16) says so, and it logs the session's end when it hears it. */
#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <stdio.h>
#include <windows.h>
#include "hellforge_mod.h"

typedef int (*Target)(int);
static const HfApi* hellforge;
static Target original;

static int Detour(int x) { return original(x) + 1000; }

static void Loaded(HfMod* mod, HfEvent event, void* user)
{
    HMODULE game = GetModuleHandleA("fake_game.dll");
    uintptr_t pin = (uintptr_t)GetProcAddress(game, "hellforge_fake_pin");
    uintptr_t found = hellforge->find_pattern("fake_game.dll", "75 75 48 8B 15 ?? ?? ?? ?? 48 81 C2");
    hellforge->log(mod, found == pin && pin != 0 ? "pattern found the pin" : "pattern missed the pin");
    uintptr_t target = (uintptr_t)GetProcAddress(game, "hellforge_fake_target");
    if (hellforge->size >= offsetof(HfApi, read_code) + sizeof hellforge->read_code)
    {
        unsigned char before[8], live[8];
        char pattern[32] = "";
        if (!hellforge->read_code(target, before, sizeof before) && !hellforge->read(target, live, sizeof live))
        {
            for (int i = 0; i < 8; i++) snprintf(pattern + 3 * i, sizeof pattern - 3 * i, i < 7 ? "%02X " : "%02X", before[i]);
            /* a hook's jump (E9) is never what read_code returns */
            if (before[0] != 0xE9 && hellforge->code_matches(target, pattern))
                hellforge->log(mod, "code check passed (live code hooked: %s)", memcmp(before, live, sizeof live) ? "yes" : "no");
            else hellforge->log(mod, "code check failed");
        }
    }
    if (hellforge->hook(mod, target, (void*)Detour, (void**)&original) == 0) hellforge->log(mod, "detour installed");
    if (hellforge->size >= offsetof(HfApi, set_status) + sizeof hellforge->set_status) hellforge->set_status(mod, "detour\tinstalled");
}

static void Ended(HfMod* mod, HfEvent event, void* user)
{
    hellforge->log(mod, "heard the session end");
    hellforge->set_status(mod, "session over");
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    hellforge = api;
    hellforge->log(mod, "hook test for %s (%s)", hellforge->mod_id(mod), hellforge->build_id());
    if (hellforge->version >= 16 && hellforge->on(mod, HF_EVENT_SESSION_END, Ended, 0)) return 1;
    return hellforge->on(mod, HF_EVENT_GAME_LOADED, Loaded, 0);
}
