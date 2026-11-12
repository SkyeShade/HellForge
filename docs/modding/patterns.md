# Patterns

<!-- diataxis: how-to -->

A pattern is a look a player picks for a weapon in the armory, or for the Pelican, the hellpod or a vehicle in the ship's
hangar. `hf->add_pattern` (SDK 18) adds one of your own: a copy of a vanilla pattern under an id of its own, with the
textures you swap in. The player holds it from the start.

## Contents

- [What a pattern is](#what-a-pattern-is)
- [Adding one](#adding-one)
- [Bases](#bases)
- [Textures](#textures)
- [Where it shows](#where-it-shows)
- [Limits](#limits)

## What a pattern is

A vanilla pattern changes a model's materials. For each material it sets up to four textures:

| Texture | What it holds |
|---|---|
| `pattern_lut` | the pattern's colour: a 3 x 1 RGBA16F lookup texture, the colour in its first texel |
| `pattern_masks_array` | where the colour goes: a greyscale mask painted on that model's UV map, white for the pattern colour |
| `material_lut` | the material's own colour table |
| `decal_sheet` | the decals |

A pattern is not a tiling camouflage: a new shape means a mask of your own for each model, painted on its UV map. A new
colour on the base's shape needs only a `pattern_lut`.

## Adding one

Call `add_pattern` from `hellforge_mod_init`. It returns the pattern's id, or 0 with the reason in your mod's log.

```c
#include "hellforge_mod.h"

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    if (hf->version < 18) return 1;
    /* a mask the base sets (your log names them), and yours; add_pattern copies these */
    HfTextureSwap masks[] = {{0x27E4F7F8D01DFEA4ull, hf->resource_hash("yourname/your_mod/patterns/tiger_liberator_mask")}};
    static const float swatch[][3] = {{0.6f, 0.35f, 0.1f}, {0.1f, 0.1f, 0.1f}};
    HfPattern tiger = {sizeof tiger};
    tiger.base = 0x5D125795;                         /* Arctic Camo, a weapon paint */
    tiger.name = "yourname/your_mod/tiger";          /* its id: the thin hash of this */
    tiger.name_text = "Tiger Stripe";
    tiger.description = "Orange and black.";
    tiger.pattern_lut = hf->resource_hash("yourname/your_mod/patterns/tiger_lut");
    tiger.swaps = masks;
    tiger.swap_count = 1;
    tiger.colours = swatch;
    tiger.colour_count = 2;
    return hf->add_pattern(mod, &tiger) ? 0 : 1;     /* weapons NULL: every weapon offering Arctic Camo */
}
```

| Field | Value |
|---|---|
| `base` | the vanilla pattern it copies, by id ([bases](#bases)); a default skin changes nothing and is refused |
| `name` | unique, prefixed with your name and mod; the pattern's id is its thin hash |
| `name_text` | its name on the card; NULL keeps the base's. Hangar names are shown in upper case, as the game's are |
| `description` | a weapon pattern's description; NULL keeps the base's |
| `pattern_lut` | your pattern colour, put in place of every `pattern_lut` the base sets; 0 keeps the base's |
| `swaps`, `swap_count` | up to 16 other textures the base names, each replaced by yours (masks, material LUTs, decals) |
| `colours`, `colour_count` | the swatch on its card, 1 to 8 linear RGB colours; NULL keeps the base's |
| `weapons`, `weapon_count` | a weapon pattern's weapons, up to 16 loadout ids, vanilla or `add_weapon`'s; NULL: every weapon offering the base |

Once the game has its tables, your mod's log lists each texture the base sets, by material override, so you know what
to swap:

```text
add_pattern(yourname/your_mod/tiger): its base sets pattern_lut 0c80315ea9d101bf (material override at +0xc8)
add_pattern(yourname/your_mod/tiger): its base sets pattern_masks_array 27e4f7f8d01dfea4 (material override at +0xc8)
```

## Bases

The base decides what the pattern is for.

- **A weapon paint** makes a weapon pattern, offered in the armory's pattern slot. The game has 12 paints, each offered
  on every weapon that takes paints:

  | Id | Paint |
  |---|---|
  | `0x80FBE63D` | Default |
  | `0xBD04CA8C` | Helldiver Classic Solid |
  | `0x94685290`, `0x5D125795` | Arctic Solid, Arctic Camo |
  | `0x82C32409`, `0x46A140E2` | Desert Solid, Desert Camo |
  | `0x680145DE`, `0x8F739A74` | Forest Solid, Forest Camo |
  | `0xC5F23640`, `0x96F3BBDE` | Urban Solid, Urban Camo |
  | `0xB89373ED`, `0x5B8B77CB` | Night Solid, Venus Solid |

- **A unit skin** makes a skin of every collection that has the base. A skin's id is shared by a family of models, so
  one call covers the whole family. Forest Camo, for instance:

  | Id | Family |
  |---|---|
  | `0xD1D0318D` | the Pelican |
  | `0xB9F05F72` | the hellpod and its weapon rack |
  | `0xAC94B70C` | the exosuits: EXO-45 Patriot, EXO-49 Emancipator, EXO-51 Lumberer, EXO-55 Breakthrough |
  | `0xAAD7AF74` | the FRVs and the TD-220 Bastion |

  Each model of a family has its own deltas and masks: the log lists the base's textures for each collection, so a
  mask swap names each model's mask.

## Textures

Your textures must be loaded whenever a pattern may be drawn: put them in a session package of your mod's data
([packages of your own](data.md#packages-of-your-own)). The base's own textures load with the base's package, which
HellForge loads for your pattern too.

A colour of your own is a copy of the base's `pattern_lut` with its colour changed (the `lut` edit,
[textures](textures.md)). A shape of your own is a greyscale PNG painted on the model's UV map, at the base mask's size
(most are 256 x 256), in the base mask's category:

```json
{ "format": 1, "patches": [{ "package": "yourname/your_mod/patterns", "load": "session", "resources": [
  { "type": "texture", "copy": "0x0C80315EA9D101BF", "name": "yourname/your_mod/patterns/tiger_lut",
    "lut": { "columns": { "0": [0.6, 0.35, 0.1] } } },
  { "texture": "patterns/tiger_liberator.png", "name": "yourname/your_mod/patterns/tiger_liberator_mask",
    "format": "bc4", "category": "0x8C3BB092" } ] }] }
```

## Where it shows

- **Weapons.** The pattern's card is in the pattern slot of each weapon it is on, a reward of the weapon's first
  mastery level as the game's first paints are. Mods' copies of a weapon offering the base take it too.
- **The hangar.** The ship's hangar has tabs for the Pelican, the hellpod, the EXO-45, the EXO-49 and the FRV. A pick
  saves in the game's own save, as the game's patterns do.
- **Co-op.** A mod with patterns is shared, so friends have it too. The Pelican always wears the host's pick.

## Limits

| Limit | Value |
|---|---|
| Weapon patterns and attachments, all mods | 128 |
| Unit patterns, all mods | 128 |
| Mods' patterns on one weapon | 16 (its customize screen holds 64 entries over five slots) |
| Skins in one collection | 24 in all, 16 of them the game's: the hangar list shows no more |
| Texture swaps, swatch colours | 16, 8 |

A pattern past a limit is left out, and the mod's log (or the backend's) says which and why.
