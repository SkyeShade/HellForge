# HellForge documentation

Every HellForge document, by who it is for, and what an installation holds. To install HellForge, start with the
[front page](../README.md).

## Playing

- [Front page](../README.md): what HellForge is, what you need, how to install it and your first session.
- [Troubleshooting](troubleshooting.md): problems players hit (Steam, setup, antivirus, the network block, sessions that
  fail), how to collect logs and where to ask.
- [Using mods](mods.md): adding and removing mods, the Mods page, load order and playing with friends.
- [Lua mods](lua-bridge.md): Bingus Shared Loader, HD2Runtime and their mods on HellForge, and the Lua bridge behind them.
- [Co-op and public lobbies](multiplayer.md): playing with friends and in public lobbies, and which settings decide
  who can join.
- [Linux (Proton)](linux.md): the experimental Linux setup: installing, updating, sessions, co-op, account sync and
  Settings.
- [HellForge on SteamOS](linux-steamos.md): for Steam Deck and Steam Machine players; the read-only system, first setup
  in Desktop Mode, the shader cache and SteamOS updates.
- [Backing up and removing HellForge on Linux](linux-backup-and-removal.md): for Linux players; prefix backup and
  restore, and what uninstall removes and leaves.
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
   [Animations](modding/animations.md): a unit copy's own joints, clips and state machine from a JSON file.
   [The asset kit](modding/assets.md): sharing patch files made from the game's resources without Arrowhead's bytes.
6. [Content](modding/content.md): changing and adding weapons, projectiles, stratagems, entities, behaviours, abilities
   and difficulties.
   [Warbonds](modding/warbonds.md): warbonds of a mod's own, selling game items and its weapons and stratagems.
7. [Logic at run time](modding/runtime.md): events, actions, live edits and the game thread.
8. [Hooks and game memory](modding/hooks.md): hooking game functions and reading and patching memory where the SDK has
   nothing yet.
9. [Debugging](modding/debugging.md): logs, refusals, crashes and testing with friends.
10. [Tools and sharing](modding/tools.md): IDEs, building from a terminal or the repository, prebuilt mods, releasing.

Reference and related pages:

- [SDK reference](sdk-reference.md): every API call, component field and content constant (generated from the SDK).
- [Launchers of your own](external-launchers.md): for authors of mod managers and launchers; `HellForge.Cli.exe` and the
  files it reads and writes.
- [Mods that change while the game runs](modding/live.md): for code mod authors; what HellForge does when a mod is
  switched on or off during a session, and how a mod takes part.
- [Building a lobby browser](lobby-browser.md): for front-end developers drawing their own lobby browser on the ship's
  terminal; the SDK 16 calls, the terminal's visit, what each lobby says and the rules a browser keeps.

## Contributing and the engine

- [CONTRIBUTING.md](../CONTRIBUTING.md): licensing of contributions, building and testing, where to start reading.
- [The engine](engine.md): the session runner, local backend, mod loader and stand-ins, and how the engine package is
  built.
- [How HellForge implements mods](internals.md): staging, source builds, the load chain, registries, named fields, data
  spec builds.
- [How Difficulty 11 is implemented](internals-difficulty.md): how add_difficulty adds level 11 after Super Helldive,
  which game tables move or grow, and what each part of the level changes.
- [Stats, live editing and events](internals-live.md): the stats pass at load, live editing, the in-game editor and live
  pipe, event sources and native edits.
- [Steam Cloud stays in the profile](steam-cloud.md): how the `steam_api64.dll` stand-in keeps the game's cloud files in
  each profile.
- [Code signing and Smart App Control](code-signing.md): for maintainers; which files Smart App Control blocks, what
  signing would fix, the options, and the build scripts' optional signing step.
- [The Steam download of the supported build](steam-download.md): how setup downloads the build HellForge runs from
  Steam's servers for the player's account, what the sign-in keeps (nothing), and why HellForge hosts no game files.
- [Asset migrator](asset-migrator.md): for maintainers; how the asset kit finds the game's bytes in a mod, the recipe,
  the Steam chunk index, measured results and the maintainers' command line.
- [Co-op internals](multiplayer-internals.md): what a co-op session starts, the stand-in DLLs, the backends and their
  Steam link, joins, privacy, invitations and kicks, and the research into how the game does co-op.
- [Co-op mod sync](multiplayer-mod-sync.md): how a joiner takes the host's mods, the protocol and its limits, the rejoin
  after a restart, the download speed, and what another player's mods can reach.
- [Public lobbies and strangers](multiplayer-lobbies.md): the lobby directory, the Galactic Map's search, the lobby
  browser, HellForge 0.3 players and Play with strangers.
- [Co-op in game](multiplayer-in-game.md): what co-op players share in game: stats, assets, pickups, per-player shots
  and edited rounds, the mod channel and mod content.
- [Linux internals: isolation](linux-isolation.md): the network namespace, the Steam bridge, co-op over Valve's relays,
  the Steam Linux Runtime and what the wrapper sets up.
- [The Linux sandbox](linux-sandbox.md): the sandbox probe, how `hf-sandbox` runs the game in its own domain, the
  self-test and the design claims still open.
- [Account sync on Linux](linux-account-sync.md): what Sync Inventory changes on a Linux PC (hosts file, root relay, the
  CA in the game's prefix) and how each change is checked and undone.
- [Testing HellForge on Linux](linux-testing.md): for Linux testers; what is still untested, the sandbox probe and what
  to report.
- [Several installations on one PC](multi-instance.md): for testing co-op without a second PC; a test mode that runs
  two games under one Steam account.
- [Stratagem selection categories](stratagem-categories.md): the research behind SDK 8's selection headings.
- [The Steam bridge](steam-bridge.md): how the sandboxed game reaches Steam through a broker; the design, its cost and
  the phases.
- [Steam bridge reference](steam-bridge-reference.md): the Steam census and its results, where the bridge's generated
  code gets its definitions, and what each bridge test checks.
- [Changelog](../CHANGELOG.md): what changed in each release, and what `dev` has that is not released yet.
- [Roadmap](roadmap.md): what HellForge has, what is being built, and what comes next.
- [Engine roadmap](engine-roadmap.md): planned engine work, starting with network isolation.
- [SDK roadmap](sdk-roadmap.md): what is left in the modding SDK.

## Your installation

### What setup does

On Linux, the graphical installer and native setup wizard do this instead; see
[linux.md](linux.md#setting-it-up).

`HellForge-Setup.exe` unpacks HellForge into the folder you pick and opens the launcher, whose setup wizard:

1. Finds HELLDIVERS 2 in your Steam libraries and checks that it is the supported build (Steam build 25480438).
   - When Steam has that build, setup copies it as it is.
   - When Steam has another build, or no HELLDIVERS 2, the setup screen offers three ways to get the supported build
     for your account. With a QR code approved in the Steam app on your phone (the default), setup signs in and
     downloads the build from Steam's servers; what your Steam install already has is copied, not downloaded. With no
     phone, Valve's SteamCMD downloads it, and you type your password into its window, not into HellForge. Nothing of a
     sign-in is kept, and every file is checked against Steam's hashes. [steam-download.md](steam-download.md) explains
     how.
   - When you choose Use the Steam app, when Steam does not allow the download (a game borrowed through Steam Family
     Sharing, for example), when SteamCMD cannot be set up, or on Linux, the Steam app downloads the supported build on
     your account. The wizard shows one
     `download_depot 553850 <depot> <manifest>` command per depot (the manifests pinned in
     `supported-build.json`) to paste into the Steam console (`steam://open/console`), watches
     `<Steam>/steamapps/content/app_553850/`, continues when every depot is complete, copies from there and can delete
     the download afterwards. [Troubleshooting](troubleshooting.md#setup-cannot-find-the-supported-build) walks through
     it.

   The normal game is not touched either way.
2. Confirms with Steam that the signed-in account owns the game or has it through Steam Family Sharing. When the build
   is downloaded from Steam's servers, this runs after the download, with the downloaded copy's `steam_api64.dll`.
3. Copies (or downloads) the game's 135 depot files (about 24 GB), each checked against Steam's hashes. Mods and other
   files in the Steam folder are not copied, and an interrupted copy or download continues where it stopped.
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
  replaces the app and keeps the game copy, profiles and settings. Close HellForge first. On Linux, run a newer
  `HellForge-Linux-Installer` with the same prefix instead; see [linux.md](linux.md#updating).
- Repair: Settings > Installation > Check and repair compares the game files with the supported build, copies any that
  are missing or damaged from Steam, and renews the network block and shortcuts.
- Uninstall: Settings > Installation > Uninstall HellForge... removes the network block (Windows asks for permission)
  and the shortcuts, then deletes the installation folder once the launcher has closed: the game copy, profiles, every
  mod in its mods folder, saves and logs. Copy anything you want to keep first. Your normal HELLDIVERS 2 and Steam are not
  touched.
