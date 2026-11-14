# Textures

<!-- diataxis: reference -->

A data spec's `texture` resource builds a texture of your own from PNG files in the mod folder. A material copy's
`textures` edit then puts it in one of the material's slots, and a model or unit drawn with that material samples it.
This page lists the formats, what the weapon material's slots hold, and the limits.

## Contents

- [In the data spec](#in-the-data-spec)
- [Formats](#formats)
- [Array textures](#array-textures)
- [Putting a texture in a material](#putting-a-texture-in-a-material)
- [The weapon material](#the-weapon-material)
- [Limits](#limits)

## In the data spec

```json
{ "texture": "textures/rifle_data.png", "name": "yourname/your_mod/rifle/base_data", "format": "bc3" }
```

| Field | Value |
|---|---|
| `texture` | a `.png` file in the mod folder, or a list of them for an [array texture](#array-textures) (required) |
| `name` | the texture's name, the mod's own resource path (required) |
| `format` | `rgba8`, `bc1`, `bc3`, `bc4` or `bc5` (required) |
| `category` | the texture's category: a name such as `normal` or `albedo`, or `0x<8 hex digits>`; default none |

The PNGs are part of the spec, so changing one rebuilds the data and changes the modlist code. HellForge encodes every
pixel itself, so mod sync treats a texture as content ([data specs](data.md#data-specs)).

## Formats

| Format | Keeps | Size per pixel | Use |
|---|---|---|---|
| `rgba8` | all four channels exactly | 4 bytes | masks and small data textures, such as `id_masks_array` |
| `bc1` | RGB, compressed; alpha dropped | 0.5 byte | colour without alpha |
| `bc3` | RGBA, compressed | 1 byte | four channels of data, such as `base_data` |
| `bc4` | red only, compressed | 0.5 byte | one greyscale channel |
| `bc5` | red and green, compressed | 1 byte | a normal map's X and Y |

Every texture of the game is UNORM: shaders decode colour themselves, so no format is sRGB. Pixels go in as they are,
with no gamma change. HellForge writes the full mip chain, halving each side (rounding down) to 1 x 1. Each channel is
box-filtered on its own, so a transparent pixel keeps its colour and the mips of masks and normals stay data.

The category is the thin hash of `core/texture_categories/<name>`. The game's names include `albedo`, `albedo_opacity`,
`normal`, `nac`, `linear_greyscale`, `roughness_metallic_ao` and `uncompressed`. Textures of every format also come
without one, so leave it out unless you match a game texture.

## Array textures

A list of PNGs makes an array texture: one layer per PNG, in order, all the same size. Materials sample some slots as
arrays, such as the weapon material's `id_masks_array` (two layers).

## Putting a texture in a material

Copy a material and give it your textures by slot ([copy](data.md#copy)):

```json
{ "type": "material", "copy": "0x7F112ACD09A5E523", "name": "yourname/your_mod/rifle/m_body",
  "textures": { "base_data": "yourname/your_mod/rifle/base_data", "id_masks_array": "yourname/your_mod/rifle/id_masks" } }
```

A slot is its name or `0x<8 hex digits>` (its thin hash). A texture is your own texture's name or a game texture's
`0x<hash>`. Every slot you name must be in the material, or the build is refused. Then draw your model with the copy:
`"materials": { "Body": "yourname/your_mod/rifle/m_body" }` ([models](models.md)).

## The weapon material

Weapon materials such as the AR-23 Liberator's, `0x7F112ACD09A5E523`, take their colours from a lookup texture, not
from a colour texture. Community tools (filediver) read their slots so:

| Slot | Game texture | Holds |
|---|---|---|
| `base_data` | BC7, 1024 x 1024 | R, G: the surface normal's X and Y; B: ambient occlusion; A: roughness |
| `id_masks_array` | RGBA8, 256 x 256, 2 layers | which material zone each pixel is: channel c of layer l at 0.5 or more picks zone 4l + c |
| `material_lut` | RGBA16F, 23 x 8 | one row per zone: base colour (column 0), metal, roughness and detail settings |
| `decal_sheet` | BC3, 2048 x 2048 | the weapon's decals and markings |
| `lens_cutout_texture` | BC4, 512 x 512 | the sight lens's shape |

The other slots are shared tiling textures (camouflage, dirt, blood and bug splatter) that do not depend on the model's
UVs. For a model of your own, give it a `base_data` and an `id_masks_array` laid out on its UVs. Recolour the zones
with a copy of the lookup texture and its `lut` edit ([copy](data.md#copy)); a PNG cannot hold its 16-bit floats.

## Limits

- At most 4096 x 4096 pixels, as the game's largest textures.
- Textures are written whole, without streamed mips, as half of the game's textures are. A large texture costs its
  full size in memory whenever its bundle is loaded.
- The block compression is simple and fast, not a quality encoder. For fine detail, use `rgba8` or a larger texture.
- No BC7 or 16-bit float formats yet.

Example: `model-carbine` builds its body material from two textures of its own.
