# Tools and sharing

This page is for mod authors who want more than the launcher's buttons. It covers how HellForge builds mods, working in
an IDE, building from a terminal or in the HellForge repository, prebuilt mods, and releasing a mod.

- [How HellForge builds mods](#how-hellforge-builds-mods), [IDEs](#ides), [from a terminal](#from-a-terminal)
- [In the repository](#in-the-repository), [prebuilt mods](#prebuilt-mods), [releasing a mod](#releasing-a-mod)

## How HellForge builds mods

A code mod ships its sources (C or C++ under `src/`), and every player's HellForge builds it before a session into
`<installation>/temp/mod-builds/<mod id>/`. Every machine uses the SDK's pinned Zig compiler with the same flags,
including `-Wall -Werror`, so a warning fails the build. Nobody installs a toolchain: the first source mod fetches Zig
into `<installation>/tools` (pinned and checksummed), and the Mods page offers **Install mod tools** while a source mod
waits for it.

- The cache. A build is kept under a key made of the manifest, every file under `src/`, the SDK headers, the compiler
  and flags, and the game build. When any of them changes, the mod builds again; otherwise the session starts at once.
- The check. A build is used only when its record still matches its sources and its DLL's hash. A stale or swapped DLL
  is built again, never loaded.
- Errors. A mod that does not compile stays off for that session, and its row on the Mods page shows the compiler's
  message (file and line). **Build now** in its ⋮ menu compiles it at once; with unchanged sources it shows the last
  result instead of compiling again.
- A DLL in the mod's own folder (an IDE's build) is not loaded. The session loads HellForge's build.

## IDEs

Rider, CLion and VS Code (CMake Tools) open a mod as a CMake project. Each mod made from the template has a
`CMakeLists.txt` and a `CMakePresets.json` that bring in the compiler, Ninja and the SDK, so code completion and
navigation into `hellforge_mod.h` work.

1. Fetch the tools for IDEs once: `HellForge.Cli mod-tools --install <installation> --ide yes` puts Zig, CMake and Ninja
   into `<installation>/tools`. The Mods page's **Install mod tools** fetches the same, but it only shows while a source
   mod waits for Zig; once a session has fetched Zig, use the command.
2. In Rider, open **Settings > Build, Execution, Deployment > Toolchains** (or **CMake**) and set CMake to
   `<installation>/tools/cmake/bin/cmake.exe`. That is the only setting.
3. Use **File > Open** on the mod's folder. It loads with the `default` preset. **Build** compiles the DLL so the IDE
   can show errors; the session still builds its own.

## From a terminal

`HellForge.Cli` is in `<installation>/app/cli`.

```
HellForge.Cli new-mod --id yourname.my-mod --name "My Mod" --install <installation>   # a mod from the template, in mods/
HellForge.Cli build-mod --mod <installation>/mods/my-mod                             # compile now
HellForge.Cli package-mod --mod <installation>/mods/my-mod                           # zip it to share
HellForge.Cli check-stats --mod <folder>                                             # check a stats mod
HellForge.Cli build-data --mod <folder> --install <installation>                     # build a data spec
HellForge.Cli mod-hash --mod <folder or zip>                                         # its content hash (mod sync)
```

`new-mod` also takes `--out <folder>` instead of `--install`, to make the mod outside an installation. `build-mod` on a
mod in an installation's mods folder makes the build a session loads (in `temp/mod-builds`); elsewhere it writes the DLL
its manifest names beside the manifest. `package-mod` takes `--out <folder>` for a mod outside an installation.

## In the repository

To work on the examples or the SDK in the HellForge repository, `scripts/Setup-ModTools.ps1` fetches the tools into
`tools/`, and `scripts/Build-Mod.ps1 <mod folder>` compiles `src/*.c` (or `*.cpp`) into the DLL the manifest names.
With `-Install <installation>` it also builds the mod's data spec.

The examples' CMake presets copy the built mod (its manifest, DLL and `data/`, but not a data spec, stats or sources)
into `<Documents>/HellForge/mods/<folder>`, which suits trying a DLL quickly. To install a mod the way players get it,
copy its folder instead. A `CMakeUserPresets.json` (not committed) that inherits `default` can set `HELLFORGE_INSTALL`
to another installation, or to empty to only build:

```
cd examples/mods/golden-liberator
../../../tools/cmake/bin/cmake --preset default
../../../tools/cmake/bin/cmake --build --preset default
```

The SDK headers `hellforge_components.h` and `hellforge_content.h` and the [reference](../sdk-reference.md) are generated
from `mappings/` by `HellForge.Cli sdk-headers --repo .`. Edit the mappings, never the generated files.

## Prebuilt mods

A mod can ship its DLL instead of sources (closed source, or built with other tools or libraries). Without `src/`, the
DLL in the folder is loaded. A mod that ships both and wants its own DLL loaded sets `"prebuilt": true`. Name the game
build the DLL was built for, so that another build refuses it with a message instead of crashing:

```json
"code": { "dll": "my_mod.dll", "sdk": 15, "prebuilt": true, "game": "25480438" }
```

Friends then compare the DLL itself, so everyone needs the identical file.

## Releasing a mod

1. Raise `version` in `hellforge-mod.json`.
2. Choose **Zip to share** in the mod's ⋮ menu (or run `HellForge.Cli package-mod --mod <folder>`). HellForge builds
   the mod first, so a mod that does not compile is not packed. It then writes
   `<installation>/exports/<folder>-<version>.zip` with the manifest, `src/`, images and data, without IDE leftovers.
   The zip has no DLL, since every player's HellForge builds it; a prebuilt mod keeps its DLL.
3. Players drop the zip into their `mods` folder, unpacked or not, but not both: two copies of one id load neither.
   Their HellForge builds it, and they need no tools of their own.

Friends who play together need the same gameplay mods with identical files, so share the zip rather than a folder that
picked up leftovers. The SDK, template and examples are MIT No Attribution, so your mod is yours under any licence you
choose.

Give the manifest a `url` (an `https://` address: the mod's page on GitHub, Nexus Mods or your Discord's site) when the
mod has one ([the manifest](concepts.md#the-manifest)). The Mods page links it, and a player who joins someone running
your mod and has to install it themselves gets a GET IT button on the in-game MOD SYNC prompt that opens it.

### Players who join you

When a player joins a lobby whose host runs your mod and lacks it, mod sync can download it from the host
([mods.md](../mods.md#playing-with-other-mods)). A mod HellForge checks all of (schema stats, network field types, data
specs without `replace`) downloads from any host. A code mod, or one with raw data, downloads only from a host who is
the player's Steam friend, and only when:
- HellForge's verified list names that exact version (below), or
- the player trusts it on the prompt (TRUST, after a warning that it runs its own code on their PC). That trust is for
  those exact files: a new version has to be trusted again.

Otherwise the player installs it themselves, from your `url` when the manifest has one.

### The verified list

`data/verified-mods.json` in HellForge's repository lists the code and raw-data mods a HellForge maintainer has
reviewed. It ships inside each HellForge release, so a change reaches players with the next release. Each entry names
one exact version by its content hash: the SHA-256 of the lines the modlist code takes from the mod (its id, version,
each counted file and that file's SHA-256, and for a data spec the data builder's version).

To add a mod (maintainers):
1. Review the release as players get it: the zip its author shares, not a working folder. Read all of its code.
2. Print its hash from that zip: `HellForge.Cli mod-hash --mod <the zip or folder>`. It hashes a zip by what it ships,
   as a player's HellForge loads it, and prints the mod's class and an `entry` to copy.
3. Add the entry to `mods` in `data/verified-mods.json`: `id`, `name`, `version`, `hash` (64 lower-case hex digits) and
   `reviewed` (the date, who reviewed it, and what: for example "2026-10-23, SkyeShade: stratagem-compendium-1.0.4.zip
   as shipped").
4. Run `scripts/Test.ps1`: the tests parse the list (a malformed entry fails them) and recheck the Stratagem Compendium
   entry against its zip where it is installed.

Rehash every listed mod that has a data spec when `ModDataBuilder.Version` changes: its hash includes the builder's
version, so the old entry no longer matches. Any change to a mod's files (a comment, a line ending) is a new hash too:
the repository's `examples/mods/stratagem-compendium` hashes differently from its shipped 1.0.4 zip because its
comments were edited after the release.

## Where to go next

This is the last page of the guide. Keep the [SDK reference](../sdk-reference.md) at hand, go back to the
[guide's start page](README.md) for the example mods, or read [Lua mods and the Lua bridge](../lua-bridge.md) for how
HellForge runs HD2Runtime mods.
