# Data mods

<!-- diataxis: how-to -->

This page is for mod authors who want to change or add game data: models, textures, effects, sounds and icons. It
covers the three ways a HellForge mod brings data, and the two engine edits a manifest can make without code.

Game data is the archives the game loads. A mod brings data as patch files it ships (`data/`), as a cosmetic pack in the
community's format, or as a data spec that HellForge builds on each player's machine from their own game copy, so the
mod ships no game bytes.

- [Patch files](#patch-files) and [cosmetic packs](#cosmetic-packs)
- [Data specs](#data-specs): copies and recolours, your own models, icons, sounds, edits to game shaders
- [Shaders](shaders.md): your own pixel shaders in a data spec
- [Native edits in the manifest](#native-edits-in-the-manifest)

## Patch files

`data/` holds the game's own mod format, the files mod managers install: `<bundle>.patch_<n>`, with
`.patch_<n>.gpu_resources` and `.patch_<n>.stream`, at the top of `data/`. Anything else there refuses the mod.

Each session copies every enabled mod's patches into the game copy's `game/data` in load order, renumbered per bundle
after any already there. The top mod's patches get the highest numbers, so they win. The game's own files are never
changed.

Only ship data you made. Data copied or changed from the game's own resources is the game's bytes: describe it in a
[data spec](#data-specs), and HellForge builds it for each player. For patch files you already made with other tools,
[the asset kit](assets.md) takes the game's bytes out and ships a recipe that rebuilds them from each player's game;
`package-mod` does this by itself.

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
model, an effect copy, a weapon's sound banks, your own geometry and textures, a stratagem icon. HellForge builds it from each
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

A patch can instead be a package of your own: `"package": "yourname/your_mod/rifle"` in place of `archives`
([packages of your own](#packages-of-your-own)).

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
- `textures`: a material's textures by slot (the slot's name or `0x<IdString32>`): `{"base_data":
  "yourname/your_mod/rifle/base_data"}`, your own texture or a game texture ([textures](textures.md)).
- `lut`: a material's colour lookup texture, with a row per material zone and column 0 the base colour. `zonesFrom`
  makes every row first take this row's values, `columns` (`{"<column>": colour}`) sets columns in every row, and
  `rows` (`{"<row>": {"<column>": colour}}`) sets single cells. Alpha is kept and the mips are rebuilt.
- `mesh` (on a `unit`): your own geometry from a Wavefront OBJ in the mod folder, `{"obj": "models/cube.obj", "scale":
  1, "yUp": true, "unitSpace": false}`. Every render and shadow mesh in every LOD gets it, rigid on the unit's main bone.
  Models HellForge cannot rebuild yet are refused with the reason.
  - By default the mesh is centred on the render mesh's joint (a weapon's grip area). With `"unitSpace": true` it sits
    at the OBJ's own coordinates in the unit, the frame of the unit's joints and attachment points. For a weapon that is
    +Y to the muzzle, +Z up and +X to its right: Blender's coordinates when Blender writes the OBJ.
  - Materials, physics, hitboxes and attachment points (the muzzle, magazine, optic) stay the base's. Recolour the
    materials in the same patch and model around the attachment points. Its UVs sample the base material's own textures.

A `unit` copy also gets its base's physics under its own name, because the engine finds a unit's physics by its name.

### include

`include` adds game resources of `type` under their own names, unchanged: what a copy needs loaded with it, such as the
materials and textures an effect uses, or a donor weapon's sound banks (both its `wwise_bank` and `wwise_dep` entries).

### source: steam

`"source": "steam"` on a `copy` or `include` reads the resource from the game as Steam has it installed (a newer
build's resource, from the player's own files) instead of from the game copy. An `include` from Steam must be new to the
copy.

### source: a past build

`"source": {"build": "2024-03-01"}` on a `copy` or `include` reads the resource from a past build of the game: cut
content, or a resource as it was before an update changed it. Name the build by day (the build Steam published last on
or before that day, UTC, from the depot history HellForge ships) or by its data manifest, `"553853:<manifest>"`, as an
[asset report](assets.md#cut-content-from-older-builds) names it.

```json
{ "type": "unit", "copy": "0x1234567890abcdef", "name": "yourname/your_mod/cut_apc", "source": { "build": "2024-02-08" } }
```

Your mod ships only the spec. Each player's HellForge fetches the resources it names from that build on Steam once, with
the player's own sign-in. The Mods page shows **GET THEM FROM STEAM** for such a mod, and nothing is fetched until it is
clicked (`HellForge.Cli assets-rebuild --mod <folder> --steam yes` does the same). Only the chunks those resources
sit in are downloaded, each checked by SHA-1, and they are kept in the mod's `.hellforge-build-resources` folder. Until
then the mod is pending and does not load. Friends, mod sync and packages never count or send what was fetched.

A unit copy brings the unit's physics when that build has it. An `include` from a past build must be new to the game
copy. Materials are rebuilt in the game copy's shader layout, as from Steam.

### icon

`icon` turns a 256 x 256 PNG in the mod folder into an icon named `name`, with the material that draws it (the game
crashes on an icon without one). `"prepare": "auto"` turns a red-and-white picture into the stratagem icon format: red
takes the category's colour and green becomes white. The default, `"raw"`, uses the pixels as they are, and `"image"`
keeps the picture's colours and transparency for widgets that draw it as it is (a difficulty icon).

### texture

`texture` builds a texture of your own from a PNG in the mod folder, or an array texture from a list of them:
`{"texture": "textures/rifle_data.png", "name": "yourname/your_mod/rifle/base_data", "format": "bc3"}`. A material
copy's `textures` puts it in a slot ([textures](textures.md)).

### animations

`animations` gives a unit copy in the same patch animations of its own from a JSON file in the mod folder:
`{"animations": "animations/rifle.json", "unit": "yourname/your_mod/rifle"}`. The file lists the joints that move, the
clips and the state machine that plays them on the weapon's events ([animations](animations.md)).

### model

`model` builds a unit of your own from a glTF 2.0 file (a `.glb` from Blender) in the mod folder:
`{"model": "models/carbine.glb", "name": "yourname/your_mod/carbine", "materials": {"Body": "0x7F112ACD09A5E523"}}`. Every
node becomes a joint, every mesh draws rigid on its node, and node names give LODs and shadow casters
([models](models.md)).

### sounds

`sounds` builds a sound bank of your own from WAV files in the mod folder, with one event per file. Name the bank, list
its sounds, and name a mixer of a game bank whose routing, 3D placement and distance curve your sounds use.

```json
{ "sounds": [{ "event": "yourname_rifle_fire", "wav": "sounds/fire.wav", "gain": -3 }],
  "name": "yourname/your_mod/rifle_sounds",
  "mixer_bank": "content/audio/wep_cr1_adjudicator", "mixer": "0x2D200575" }
```

| Field | Value |
|---|---|
| `event` | the event's name: letters, digits and `_`, prefixed with yours; its id is `hf_wwise_id(name)` |
| `wav` | a 16-bit PCM WAV, mono or stereo, 8 to 96 kHz |
| `gain` | dB added to the sound, -96 to 24 (default 0) |
| `mixer_bank`, `mixer` | a game bank and one of its root actor-mixers: no parent, its own output bus |

- The bank and its `wwise_dep` take `name`. Put them in the package or bundle that loads with what plays them, such as
  the weapon's package.
- A record field that names a sound event takes the id: `hf_set(mod, rifle, ProjectileWeapon, fire_single_audio_event,
  HF_U32(hf_wwise_id("yourname_rifle_fire")))`.
- Sounds are stored uncompressed, about 94 KB a second for mono at 48 kHz. Keep them short.

### edit

`edit` changes one of the game's own resources in place: the patch holds it under the game's id, so it takes the
game's place. It edits the GPU part, for a fix the game's shader gets wrong: bytes in its shader stages (`stages`),
bytes outside them (`bytes`), or both.

```json
{ "type": "shader_library", "edit": "0xC4DE51720B9CD9DB",
  "stages": [{ "at": "0x106C", "digest": "<32 hex digits>", "set": { "0x1A4": "0B000032..." } }],
  "bytes": [{ "at": "0x614", "expect": "32329E2A", "set": "<8 hex digits>" }] }
```

| Field | Value |
|---|---|
| `stages[].at` | where the stage's DXBC container starts in the resource's GPU part |
| `stages[].digest` | the game container's digest (its bytes 4 to 19, as hex) |
| `stages[].set` | offsets inside the container, past its chunk table, and the bytes written there, as hex in file order |
| `bytes[].at` | an offset in the GPU part outside every DXBC container, such as a binding table's entry |
| `bytes[].expect` | the game's bytes at that offset, as hex |
| `bytes[].set` | as many bytes to write there instead, as hex |

- HellForge signs each edited container again and keeps its stage id, so the game uses it instead of its own.
- HellForge writes bytes outside the stages as they are, with no signing. Bytes inside a container are refused there.
- The digest and `expect` tie the edit to one game build: on another build, or at a wrong offset, the mod's data is
  refused.
- A HellForge older than this field refuses the whole mod, as it does any field it does not know.
- Write only bytes of your own, such as whole instructions. An edit counts as raw data for mod sync.
- Every mod that edits the same resource replaces it whole: the top mod's edit wins.

### Packages of your own

A patch with `package` becomes a whole archive of its own, `data/<id>`, where the id is the hash of the package's name.
The game loads it by that id like its own loose archives, so your resources load without a game bundle.

```json
{ "package": "yourname/your_mod/rifle", "load": "session", "resources": [ ... ] }
```

| `load` | The package loads |
|---|---|
| `"session"` (the default) | when the game starts, and stays for the session |
| `"request"` | only when asked: `load_package` from code ([packages](runtime.md#packages)), or a record that names it |

- Every player in a session runs the same mods, so each machine has the same package. In co-op HellForge asks friends'
  machines to load the packages this machine holds.
- A package holds only what its patch lists. A resource it references must be in it too, or be loaded by another
  package: include a game material's textures, or keep the patch on the base weapon's bundles.
- Its id must not be an archive of the game's, and two mods may not use one package.
- A mod with a package cannot be added or changed while the game runs; it waits for the next session.

### Checks

Everything is checked before anything is written. Every resource the spec names must exist (all missing ones are
listed, a past build's once they are fetched), every edit must find what it expects, and new names must be new. A mod
whose data cannot be built does not load that session, and the session says why. The modlist code counts the spec, not
the built files, because with the same game build they are the same bytes on every machine.

### Building by hand

`HellForge.Cli build-data --mod <folder>` checks a spec. With `--install <installation>` it builds into that
installation's cache, and with `--game-data <game/data> --out <folder>` anywhere. In the repository, use
`scripts/Build-Mod.ps1 <folder> -Install <installation>`.

Examples: `golden-liberator` (a model recoloured gold), `talon-liberator` (blue effect copies
and another weapon's sound banks), `cube-gun` (an OBJ model), `model-carbine` (a glTF model, textures), `animated-liberator`
(animations), `blue-sentry` (a sentry) and `stratagem-compendium` (icons).

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
