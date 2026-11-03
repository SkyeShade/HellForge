/* A stand-in for the game in the mod loader smoke test (HellForge.SdkShimGen smoke-modloader): a module whose plan entries
 * (decode pin, network configuration holder) the test points at, and a function for a mod to hook. */
#include <stdint.h>

/* The real routing site's original bytes (game.dll+0x12b663d), as the decode pin. */
__declspec(dllexport) const unsigned char hellforge_fake_pin[16] = {0x75, 0x75, 0x48, 0x8b, 0x15, 0xfa, 0xfc, 0x06, 0x02, 0x48, 0x81, 0xc2, 0xe4, 0x03, 0x00, 0x00};

/* [[module+rva]] is the network configuration: this holds a pointer to the cell holding it, null until "loaded". */
__declspec(dllexport) void* volatile hellforge_fake_network_holder;

/* Long enough for a detour (more than 5 bytes of instructions). */
__declspec(dllexport) __attribute__((noinline)) int hellforge_fake_target(int x)
{
    volatile int y = x;
    y = y * 3 + 1;
    return y;
}

/* Functions by name (SDK 16): the smoke plan's `function` lines map these two, the first to hook, the second only to
 * call. */
__declspec(dllexport) __attribute__((noinline)) int hellforge_fake_named(int x)
{
    volatile int y = x;
    y = y * 2;
    return y;
}
__declspec(dllexport) __attribute__((noinline)) int hellforge_fake_call_only(int x)
{
    volatile int y = x;
    y = y + 7;
    return y;
}

/* A global by name (SDK 16): a cell holding a pointer to an object, and a function whose load of it the smoke plan's
 * `global` line names as its witness. */
static int g_fakeObject = 1234;
__declspec(dllexport) void* volatile hellforge_fake_global = &g_fakeObject;
__declspec(dllexport) __attribute__((noinline)) void* hellforge_fake_global_user(void) { return hellforge_fake_global; }
