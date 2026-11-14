/* Pattern probe (in-game test only, never shipped; tests/in-game/patterns18.json): SDK 18's add_pattern in the real game.
 * Two weapon patterns, offered on every weapon that takes paints: a copy of Arctic Camo as it is, and a copy of Forest
 * Camo in magenta (a weapon paint sets material_luts only: its 8 are swapped for copies whose base colour is magenta).
 * Four unit patterns in magenta (a pattern_lut): the Pelican's, the hellpod's, the exosuits' and the FRVs' Forest Camo.
 * The magenta textures are recoloured copies of the game's in the probe's session package (hellforge-data.json). One
 * PROBE line per check in its log. */
#include <stdio.h>
#include "hellforge_mod.h"

/* Forest Camo's material_luts on weapons (the probe's log lists them: "its base sets material_lut ..."). */
static const uint64_t kForestLuts[] = {0x1BD978D3960F3B97ull, 0x29B6EE9DF3B4C1F4ull, 0x6C93BF50B44EFBEEull, 0x72BA1E791ED0A166ull,
    0x9886CE3F53F25AA0ull, 0xB2B5B31CBB19075Eull, 0xCE305B12E0F89AC7ull, 0xF86502CFC46AC046ull};

static const struct { uint32_t base; const char* name; const char* text; int magenta; } kPatterns[] = {
    {0x5D125795u, "hellforge/pattern_probe/arctic_copy", "Probe Arctic", 0},
    {0x8F739A74u, "hellforge/pattern_probe/magenta", "Probe Magenta", 1},
    {0xD1D0318Du, "hellforge/pattern_probe/pelican", "Probe Magenta", 1},
    {0xB9F05F72u, "hellforge/pattern_probe/hellpod", "Probe Magenta", 1},
    {0xAC94B70Cu, "hellforge/pattern_probe/exosuit", "Probe Magenta", 1},
    {0xAAD7AF74u, "hellforge/pattern_probe/frv", "Probe Magenta", 1},
};

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    if (hf->version < 18 || hf->size < offsetof(HfApi, add_pattern) + sizeof(void*)) { hf->log(mod, "PROBE sdk FAIL %u", hf->version); return 1; }
    hf->log(mod, "PROBE sdk ok");
    static const float magenta[][3] = {{1, 0, 1}, {0.2f, 0, 0.2f}};
    uint64_t lut = hf->resource_hash("hellforge/pattern_probe/magenta_lut");
    HfTextureSwap swaps[sizeof kForestLuts / sizeof kForestLuts[0]];
    for (size_t i = 0; i < sizeof kForestLuts / sizeof kForestLuts[0]; i++)
    {
        char name[96];
        snprintf(name, sizeof name, "hellforge/pattern_probe/forest_%016llx", (unsigned long long)kForestLuts[i]);
        swaps[i] = (HfTextureSwap){kForestLuts[i], hf->resource_hash(name)};
    }
    int added = 0;
    for (size_t i = 0; i < sizeof kPatterns / sizeof kPatterns[0]; i++)
    {
        HfPattern p = {sizeof p};
        p.base = kPatterns[i].base;
        p.name = kPatterns[i].name;
        p.name_text = kPatterns[i].text;
        p.description = "An in-game test of add_pattern.";
        p.pattern_lut = kPatterns[i].magenta ? lut : 0;
        if (kPatterns[i].magenta && kPatterns[i].base == 0x8F739A74u) { p.swaps = swaps; p.swap_count = sizeof swaps / sizeof swaps[0]; }
        p.colours = magenta;
        p.colour_count = 2;
        uint32_t id = hf->add_pattern(mod, &p);
        hf->log(mod, "PROBE pattern %s %s %08x", p.name, id ? "ok" : "FAIL", id);
        added += id != 0;
    }
    /* refusals: a default skin (id 0 is no base), and the same name twice */
    HfPattern none = {sizeof none};
    none.name = "hellforge/pattern_probe/none";
    HfPattern twice = {sizeof twice};
    twice.base = kPatterns[0].base;
    twice.name = kPatterns[0].name;
    hf->log(mod, "PROBE refusals %s", hf->add_pattern(mod, &none) == 0 && hf->add_pattern(mod, &twice) == 0 ? "ok" : "FAIL");
    hf->log(mod, "PROBE patterns added %d", added);
    return 0;
}
