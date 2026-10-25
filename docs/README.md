# HellForge documentation

Every HellForge document, by who it is for, and what an installation holds. To install HellForge, start with the
[front page](../README.md).

## Playing

- [Front page](../README.md): what HellForge is, what you need, how to install it and your first session.
- [Troubleshooting](troubleshooting.md): problems players hit (Steam, setup, antivirus, the network block, sessions that
  fail), how to collect logs and where to ask.
- [Using mods](mods.md): adding and removing mods, the Mods page, load order and playing with friends.
- [Lua mods](lua-bridge.md): Bingus Shared Loader, HD2Runtime and their mods on HellForge, and the Lua bridge behind them.
- [Linux (Proton)](linux.md): the experimental Linux setup, its network namespace and what is still untested.
- [Your installation](#your-installation) (below): what setup does, the installation's folders, updating, repairing and
  uninstalling.

## Making mods

The modding guide, in reading order:

1. [Making mods](modding/README.md): where to start, the guides, the examples.
2. [Your first mod](modding/first-mod.md): a code mod from the template to in game.
3. [How mods run](modding/concepts.md): kinds of mods, the manifest, when and on which thread code runs, naming, co-op,
   load order, compatibility.
4. [Stats mods](modding/stats-mods.md): number changes without code, and the in-game editor's Local Stats.
5. [Data mods](modding/data.md): models, textures, effects, sounds and icons; patch files, cosmetic packs and data specs.
6. [Content](modding/content.md): changing and adding weapons, projectiles, stratagems, entities, behaviours, abilities
   and difficulties.
7. [Logic at run time](modding/runtime.md): events, actions, live edits and the game thread.
8. [Hooks and game memory](modding/hooks.md): hooking game functions and reading and patching memory where the SDK has
   nothing yet.
9. [Debugging](modding/debugging.md): logs, refusals, crashes and testing with friends.
10. [Tools and sharing](modding/tools.md): IDEs, building from a terminal or the repository, prebuilt mods, releasing.

Reference and related pages:

- [SDK reference](sdk-reference.md): every API call, component field and content constant (generated from the SDK).
- [Launchers of your own](external-launchers.md): for authors of mod managers and launchers; `HellForge.Cli.exe` and the
  files it reads and writes.

## Contributing and the engine

- [CONTRIBUTING.md](../CONTRIBUTING.md): licensing of contributions, building and testing, where to start reading.
- [The engine](engine.md): the session runner, local backend, mod loader and stand-ins, and how the engine package is
  built.
- [How HellForge implements mods](internals.md): staging, source builds, the load chain, registries, named fields, live
  editing.
- [Steam Cloud stays in the profile](steam-cloud.md): how the `steam_api64.dll` stand-in keeps the game's cloud files in
  each profile.
- [Build patches](downpatch.md): for maintainers; how setup patches a newer Steam build and how to publish a patch after
  a game update.
- [Co-op and public lobbies](multiplayer.md): how HellForge's co-op, mod sync and public lobbies work, and the research
  log from building them.
- [Several installations on one PC](multi-instance.md): for testing co-op without a second PC; a test mode that runs
  two games under one Steam account.
- [Stratagem selection categories](stratagem-categories.md): the research behind SDK 8's selection headings.
- [Roadmap](roadmap.md): what HellForge has, what is being built, and what comes next.
- [Engine roadmap](engine-roadmap.md): planned engine work, starting with network isolation.
- [SDK roadmap](sdk-roadmap.md): what is left in the modding SDK.

## Your installation

### What setup does

`HellForge-Setup.exe` unpacks HellForge into the folder you pick and opens the launcher, whose setup wizard:

1. Finds HELLDIVERS 2 in your Steam libraries and checks that it is the supported build (Steam build 25480438).
   - When Steam has that build, setup copies it as it is.
   - When Steam has a newer build that HellForge has a patch pack for, setup downloads the pack (about 1.7 GB, checked
     against its SHA-256) and makes the copy by patching your own Steam files. A pack holds the binary differences
     between the two builds and a few small files the newer build no longer has. [downpatch.md](downpatch.md) explains
     how setup picks and checks a pack.
   - Otherwise (another build, or no HELLDIVERS 2), Steam itself downloads the supported build on your account. The
     wizard shows one `download_depot 553850 <depot> <manifest>` command per depot (the manifests pinned in
     `supported-build.json`) to paste into the Steam console (`steam://open/console`), watches
     `<Steam>/steamapps/content/app_553850/`, continues when every depot is complete, copies from there and can delete
     the download afterwards. [Troubleshooting](troubleshooting.md#setup-cannot-find-the-supported-build) walks through
     it.

   The normal game is not touched either way.
2. Confirms with Steam that the signed-in account owns the game or has it through Steam Family Sharing. When a patch
   pack is used, this runs after the copy, with the copy's Steam library.
3. Copies or patches the game's 135 depot files (about 24 GB) from your own Steam library, each checked against Steam's
   hashes. Mods and other files in the Steam folder are not copied, and an interrupted copy continues where it stopped.
4. Places the network block for this folder's copy (the only Windows administrator prompt) and checks that it blocks.
   Setup does not finish until it is in place.
5. Optionally imports game settings and saves from an earlier HellForge setup on this PC (the originals stay where they
   were) and creates Desktop and Start menu shortcuts.

An installation keeps working when Steam later updates the normal game, because sessions check the copy, not the Steam
installation. Each HellForge release targets one HELLDIVERS 2 build.

### The installation's folders

Everything HellForge and the copied game write stays inside the installation folder:

| Folder | Holds |
|---|---|
| `HellForge.exe` | the launcher; start HellForge with it |
| `app\` | the program |
| `game\` | the copy of the game |
| `userdata\` | game settings, saves, shader cache and crash reports (`settings.ini` is pointed here each session) |
| `profiles\` | HellForge progress |
| `mods\` | your mods ([mods.md](mods.md)) |
| `logs\` | setup and session logs ([troubleshooting](troubleshooting.md#collecting-logs)) |
| `temp\` | working files |

The game's Steam Cloud files (loadout, title, player card, controls) stay in each profile's
`userdata\profiles\<name>\cloud\` and not in the Steam account's cloud, which the normal game shares
([steam-cloud.md](steam-cloud.md)). Outside the folder there are only the network block and the optional shortcuts.

### Bundled mods

Three mods ship with HellForge, built: the in-game editor (F8), the Hell Forge difficulty (level 11) and Vanilla Plus
(small fixes: beacons stick, hellpods steer over any ground, reinforcements drop at their beacon). They are copied into
`mods\hellforge-editor`, `mods\hell-forge` and `mods\vanilla-plus` when the launcher starts, when setup has copied the
game, and before each session. The editor's icons and fonts and the difficulty's icon are made from your game copy.
Switch any of them off on the Mods page and it stays off.

An update brings their new versions. An older copy you downloaded separately (in another folder, or as a zip) is moved to
`mods-retired`, and the editor's settings and presets come along to the bundled copy.

### Updating, repairing and uninstalling

- Update: run a newer `HellForge-Setup.exe` and pick the existing installation (the button then says Update). It
  replaces the app and keeps the game copy, profiles and settings. Close HellForge first.
- Repair: Settings > Installation > Check and repair compares the game files with the supported build, copies any that
  are missing or damaged from Steam, and renews the network block and shortcuts.
- Uninstall: Settings > Installation > Uninstall HellForge... removes the network block (Windows asks for permission)
  and the shortcuts, then deletes the installation folder once the launcher has closed: the game copy, profiles, every
  mod in its mods folder, saves and logs. Copy anything you want to keep first. Your normal HELLDIVERS 2 and Steam are not
  touched.
