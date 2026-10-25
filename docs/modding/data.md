# Data mods

This page is for mod authors who want to change or add game data: models, textures, effects, sounds and icons. It
covers the three ways a HellForge mod brings data, and the two engine edits a manifest can make without code.

Game data is the archives the game loads. A mod brings data as patch files it ships (`data/`), as a cosmetic pack in the
community's format, or as a data spec that HellForge builds on each player's machine from their own game copy, so the
mod ships no game bytes.

- [Patch files](#patch-files) and [cosmetic packs](#cosmetic-packs)
- [Data specs](#data-specs): copies and recolours, your own models, icons, sounds
- [Native edits in the manifest](#native-edits-in-the-manifest)

## Patch files

`data/` holds the game's own mod format, the files mod managers install: `<bundle>.patch_<n>`, with
`.patch_<n>.gpu_resources` and `.patch_<n>.stream`, at the top of `data/`. Anything else there refuses the mod.

Each session copies every enabled mod's patches into the game copy's `game/data` in load order, renumbered per bundle
after any already there. The top mod's patches get the highest numbers, so they win. The game's own files are never
changed.

Only ship data you made. Data copied or changed from the game's own resources is the game's bytes: describe it in a
[data spec](#data-specs) instead, and HellForge builds it for each player.

## Cosmetic packs

Packs in the community's format load as they are, without a `hellforge-mod.json`. A pack is a folder or zip with a
`manifest.json` and folders of patches, or just patches at its root. The launcher's **Add mods...** also takes `.rar`
and `.7z`.

```json
{
  "Version": 1, "Guid": "0967dcdf-...", "Name": "Kinetic Replacer AIO", "Description": "...", "IconPath": ".jpgs/icon.png",
  "Options": [
    { "Name": "AC-11 Autocannon", "Image": ".jpgs/ac11.jpg", "Include": [],
      "SubOptions": [ { "Name": "AC-11/A", "Include": ["AC-11 Autocannon (Bipod)"] } ] },
    { "Name": "JAR-40 Conqueror", "Include": ["JAR-40 Conqueror"] }
  ]
}
```

- A pack's id is `pack.<Guid>` (or `pack.<name>` when it has no Guid). It is cosmetic, so it is not in the modlist code
  friends compare.
- Players switch each option on or off on the Mods page, and pick a variant for an option with sub-options (the first
  by default). A session stages the root patches first, then the chosen options in the manifest's order.
- Share a pack as you made it; HellForge never repacks one.

Lua mods (HD2Runtime, Bingus Shared Loader and the mods made for them) come as packs too: see
[lua-bridge.md](../lua-bridge.md).

## Data specs

A data spec, `hellforge-data.json` beside the manifest, describes data made from the game's own resources: a recoloured
model, an effect copy, a weapon's sound banks, your own geometry, a stratagem icon. HellForge builds it from each
player's game copy when a session starts, caches it, and stages it like `data/` (after the mod's own `data/`, if it has
both).

```json
{
  "format": 1,
  "patches": [
    {
      "archives": ["dbd9381f9ea9788f", "047c26f2f11d253e"],
      "resources": [
        { "type": "unit", "copy": "0x968211C0033DCE64", "name": "yourname/your_mod/rifle",
          "replace": [{ "from": "0x7F112ACD09A5E523", "to": "yourname/your_mod/rifle/m_weapon", "at": ["0x2E44"] }] },
        { "type": "material", "copy": "0x7F112ACD09A5E523", "name": "yourname/your_mod/rifle/m_weapon",
          "replace": [{ "from": "0xBABD338977FAA2FF", "to": "yourname/your_mod/rifle/lut" }] },
        { "type": "texture", "copy": "0xBABD338977FAA2FF", "name": "yourname/your_mod/rifle/lut",
          "lut": { "zonesFrom": 6, "columns": { "0": [0.83, 0.62, 0.18], "2": "#8C6114" } } },
        { "type": "wwise_bank", "include": ["0xE9A5E5E12FD10F52"] },
        { "type": "wwise_dep", "include": ["0xE9A5E5E12FD10F52"] }
      ]
    },
    { "archives": ["007e093ca718ca1a"], "resources": [{ "icon": "icons/my_stratagem.png", "name": "yourname/your_mod/icons/my_stratagem" }] }
  ]
}
```

Each patch becomes one archive per bundle in `archives`. Bundles are 16 lowercase hex digits: the bundles that load with
what your mod adds, such as the base weapon's, or `007e093ca718ca1a` for UI icons. Resources and references are
`0x<hash>` or a name (hashed as the game hashes them). Types are names (`unit`, `material`, `texture`, `particles`,
`wwise_bank`, ...) or `0x<hash>`. Colours are `[r, g, b]` as the game stores them, or `"#RRGGBB"`.

Each resource is one of the following.

### copy

`copy` puts a game resource, with all its parts, under your own `name`. The name is a path prefixed with your name and
mod, never a game resource's name. The copy can then be changed, in this order:

- `expect`: `{"0x<offset>": "0x<hex>"}`, what the resource must hold first. A value that is not `0x` hex is taken as a
  name and hashed.
- `replace`: references to another resource (`from`) become `to`, usually another of your copies. Without `at` every
  occurrence changes; with it, exactly the `at` offsets (`"0x..."` strings, each of which must hold `from`). HellForge
  cannot tell a reference from any other 8 bytes, so a spec with `replace` counts as raw data for mod sync: players
  who join you download it only from a Steam friend, and only when HellForge's verified list names it or they trust it
  on the prompt (see [mods.md](../mods.md), "Playing with other mods"). So does a copy or include of a type other than
  `unit`, `material`, `texture` and `particles` (and includes of `wwise_bank` and `wwise_dep`), or an edit on another
  type than its own.
- `curves`: a particle effect's colour curves (10 key times, then 10 RGB keys on 0..255). Each warm key (red >= green
  >= blue, red above blue, brightest channel at least 2) is recoloured, either with `"preset": "warm-to-blue"` or with
  `"colour": "#3070FF"` (that hue and saturation at the key's brightness). `"keys": n` requires exactly n keys to
  change; without it, at least one must.
- `colours`: a material's RGB parameters by name (`0x<IdString32>` or the name): `{"0xB4138578": [0, 0.35, 1]}`.
- `lut`: a material's colour lookup texture, with a row per material zone and column 0 the base colour. `zonesFrom`
  makes every row first take this row's values, `columns` (`{"<column>": colour}`) sets columns in every row, and
  `rows` (`{"<row>": {"<column>": colour}}`) sets single cells. Alpha is kept and the mips are rebuilt.
- `mesh` (on a `unit`): your own geometry from a Wavefront OBJ in the mod folder, `{"obj": "models/cube.obj", "scale":
  1, "yUp": true}`. Every render and shadow mesh in every LOD gets it, rigid on the unit's main bone and centred on its
  mesh joint (a weapon's grip area). Materials, physics, hitboxes and attachment points (the muzzle) stay the base's, so
  recolour the materials in the same patch. Models HellForge cannot rebuild yet are refused with the reason.

A `unit` copy also gets its base's physics under its own name, because the engine finds a unit's physics by its name.

### include

`include` adds game resources of `type` under their own names, unchanged: what a copy needs loaded with it, such as the
materials and textures an effect uses, or a donor weapon's sound banks (both its `wwise_bank` and `wwise_dep` entries).

### source: steam

`"source": "steam"` on a `copy` or `include` reads the resource from the game as Steam has it installed (a newer
build's resource, from the player's own files) instead of from the game copy. An `include` from Steam must be new to the
copy.

### icon

`icon` turns a 256 x 256 PNG in the mod folder into an icon named `name`, with the material that draws it (the game
crashes on an icon without one). `"prepare": "auto"` turns a red-and-white picture into the stratagem icon format: red
takes the category's colour and green becomes white. The default, `"raw"`, uses the pixels as they are, and `"image"`
keeps the picture's colours and transparency for widgets that draw it as it is (a difficulty icon).

### Checks

Everything is checked before anything is written. Every resource the spec names must exist (all missing ones are
listed), every edit must find what it expects, and new names must be new. A mod whose data cannot be built does not
load that session, and the session says why. The modlist code counts the spec, not the built files, because with the
same game build they are the same bytes on every machine.

### Building by hand

`HellForge.Cli build-data --mod <folder>` checks a spec. With `--install <installation>` it builds into that
installation's cache, and with `--game-data <game/data> --out <folder>` anywhere. In the repository, use
`scripts/Build-Mod.ps1 <folder> -Install <installation>`.

Examples: `golden-liberator` (a model recoloured gold through its lookup texture), `talon-liberator` (blue effect
copies and another weapon's sound banks), `cube-gun` (an OBJ model), `blue-sentry` (a sentry) and
`stratagem-compendium` (icons).

## Native edits in the manifest

Two engine edits need no code; they go in the manifest's `native` section. Each is checked against the game build
before it is written, and anything that is not as expected is logged and left alone.

```json
"native": {
  "networkFieldTypes": [ { "name": "deposit_value", "expect": { "kind": 1, "bits": 10, "min": 0 }, "bits": 14 } ],
  "componentWrites": [ { "component": "DepositComponentData", "resource": "0x056DE1C5E21E723E",
                         "fields": [ { "name": "capacity", "offset": 0, "expect": 1000, "value": 10000 } ] } ]
}
```

- `networkFieldTypes` widens an integer network field type (kind 1) to at most 31 bits. The engine clamps every integer
  it sends to its type's range, so a wider type raises that limit for every field of the type. Every player must run
  the same edit, and the modlist code makes sure they do. In code, the same edit is `set_network_field_bits`
  (`machine-gun-10k-code`).
- `componentWrites` sets u32 fields of an entity's record, only over the expected value. It predates named fields; new
  mods use a [definition block](content.md#changing-existing-content) or a [stats mod](stats-mods.md). A mod with
  `componentWrites`, like one with binary patch files, downloads through mod sync only when it is on the verified list
  ([tools.md](tools.md#the-verified-list)); `networkFieldTypes` and the data specs HellForge checks whole download by
  themselves.

`examples/mods/machine-gun-10k` is the Maxigun mod made this way, with no code.

Next: [Content](content.md), for changing and adding weapons, stratagems and entities in code.
