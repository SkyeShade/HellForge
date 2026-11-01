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

/* A cell a live test mod writes through the API, and the loader puts back when it takes the mod out (live_mods_test.c). */
__declspec(dllexport) volatile uint32_t hellforge_fake_cell = 5;
