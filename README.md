# HellForge

HellForge runs your own copy of HELLDIVERS 2 offline, against a local backend on your PC, and loads mods into it: new
stratagems, weapons, stats and difficulties. You can play solo or with Steam friends who run HellForge too. It comes with
an in-game editor (F8) and a C mod SDK with guides and examples.

Status: beta (0.2.0-beta), made for modders first, so expect rough edges. The launcher shows its version and the game
build it targets in its bottom-left corner.

## What HellForge is and is not

HellForge plays your own game. Setup copies HELLDIVERS 2 from your Steam library into HellForge's folder and checks every
file against Steam's hashes. This repository and HellForge-Setup.exe contain no game files. When Steam has a newer build
than the one HellForge runs, setup downloads a patch made from the differences between the two builds and applies it to
your own Steam files. Your normal Steam game is never changed.

The Steam ownership check stays. A genuine check runs at setup and before every launch, so Steam must be running and
signed in to an account that owns the game or has it through Steam Family Sharing.

The copy is offline. It talks to a local backend on your PC in place of Arrowhead's online services, and a network block
in the Windows Filtering Platform stops it reaching anything but this PC. It never connects to official servers, so it
cannot be used to play online with a modified game.

Co-op runs with Steam friends, or over the local network with LAN co-op on (Settings > Multiplayer) and nobody else
needed: other HellForge players on the same network find each other, and a typed address covers networks that drop
broadcasts. Players need the same gameplay mods, and HellForge checks them before a join. There is no matchmaking, there
are no public lobbies, and you cannot play with the normal game's players.

Profiles are local. A new profile starts with every item unlocked, and other templates can be picked. The galactic war can
follow the live war read-only from the community mirror [api.helldivers2.dev](https://api.helldivers2.dev), stay
HellForge's frozen war, or open every planet to liberation (Settings > Galactic war).

## Requirements

- Windows 10 or 11 (64-bit). Linux under Proton is experimental: see [docs/linux.md](docs/linux.md).
- Steam running and signed in to an account that owns HELLDIVERS 2 or has it through Steam Family Sharing.
- About 25 GB free on the drive you install to. Add about 2 GB while a patch pack is applied, or plan on about 50 GB in
  total if Steam has to download the supported build.
- Permission to approve one Windows administrator prompt, for the network block. Windows Firewall does not need to be on:
  the block lives in the Windows Filtering Platform and holds even when a security suite has switched the firewall off.

## Install

1. Download `HellForge-Setup.exe` from the [releases page](https://github.com/SkyeShade/HellForge/releases). Windows
   SmartScreen may warn that it is unrecognised because it is not code-signed: choose More info > Run anyway.
2. Run it and pick a folder. The default is `Documents\HellForge`, or your user folder when Documents is in OneDrive.
   Setup refuses OneDrive and linked folders, Steam's `steamapps` folder and folders that are not empty. It unpacks
   HellForge there and opens the launcher.
3. In the launcher, press Start setup. The wizard confirms with Steam that you own the game, copies the game (about
   24 GB) from your own Steam library with every file checked against Steam's hashes, places the network block (the one
   administrator prompt) and creates shortcuts.

HellForge runs one game build (Steam build 25480438). When Steam has a newer build that HellForge has a patch pack for,
setup downloads the pack (about 1.7 GB) and makes the copy by patching your own Steam files. Otherwise the wizard shows
commands to paste into the Steam console, and Steam itself downloads the supported build for your account.

Each step in detail, the installation's folders, updating and uninstalling are in
[docs/README.md](docs/README.md#your-installation). If setup stops, see [troubleshooting](docs/troubleshooting.md).

## First steps

1. Start HellForge with `HellForge.exe` at the top of the installation folder, or its shortcut, with Steam running and
   signed in.
2. Press PLAY. The launcher builds any mods that ship their sources, verifies the game files, checks the network block
   and starts the game.
3. The first launch of a new installation can stop in the cryo pod, and the launcher tells you when it does. Press STOP,
   then PLAY again.
4. In game, press F8 for the in-game editor.
5. Three mods come installed: the in-game editor, the Hell Forge difficulty (level 11) and Vanilla Plus (small fixes).
   Add more on the Mods page: [docs/mods.md](docs/mods.md). A code mod runs inside the game with your permissions, so
   only enable code mods from people you trust.
6. To play with friends, compare the modlist code on the Mods page (friends need the same gameplay mods), then host or
   join from your Steam friends list. For players on your own network instead, switch LAN co-op on in Settings >
   Multiplayer; the first LAN session asks Windows once for the firewall rule that lets the local network reach it.

## Where to go next

- Players: [the docs map](docs/README.md) lists every page, [troubleshooting](docs/troubleshooting.md) covers the
  problems people hit, and [using mods](docs/mods.md) covers the Mods page and co-op. For help and news, join the
  [HellForge Discord](https://discord.gg/3Yye92xCzh).
- Modders: [docs/modding](docs/modding/README.md), starting with [your first mod](docs/modding/first-mod.md). In an
  installation the guide is in `app/sdk/docs/modding`, the template mod in `app/sdk/template-mod` and the examples in
  `app/sdk/examples/mods`. Mods ship their C sources and every player's HellForge builds them with the same pinned
  compiler, so a mod is the same on every machine and follows game updates while its field names still exist.
- Mod managers and launchers: `app/cli/HellForge.Cli.exe` manages an installation's mods and profiles and starts and
  stops its sessions without HellForge.exe open. See [docs/external-launchers.md](docs/external-launchers.md).
- Contributors: [CONTRIBUTING.md](CONTRIBUTING.md) (building and testing), [docs/engine.md](docs/engine.md) (the engine's
  parts) and [docs/internals.md](docs/internals.md) (how mods are implemented).

## Credits

- [Dear ImGui](https://github.com/ocornut/imgui) (MIT) draws the in-game editor,
  [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause) hooks for the mod loader and the editor, and
  [Zig](https://ziglang.org) compiles native code and mods. All three are downloaded at build time, pinned, and their
  licences ship with what uses them. Microsoft's public PlayFab Party and PlayFab Multiplayer SDK packages are downloaded
  at build time for their headers (the stand-ins' signatures); nothing from them is committed or shipped.
- The live war comes from the community [Helldivers 2 API](https://github.com/helldivers-2/api).
- Linux support was worked out by DreamyBranch and joshua-software-dev
  ([issue 1](https://github.com/SkyeShade/HellForge/issues/1)).

HellForge is an independent fan project, not affiliated with or endorsed by Arrowhead Game Studios or Sony Interactive
Entertainment. HELLDIVERS is a trademark of Sony Interactive Entertainment.

## Licence

HellForge is free software under the GNU General Public License, version 3 ([LICENSE](LICENSE)): you may use, study,
share and change it, and versions you share must stay under the GPL-3.0, with their source.

The mod SDK, the mod template and the example mods (`native/mod-sdk/`, `templates/`, `examples/mods/`) are under the MIT
No Attribution License instead ([LICENSE-SDK](LICENSE-SDK)), so you can copy from them freely. An additional permission
under the GPL's section 7 ([LICENSE-EXCEPTION](LICENSE-EXCEPTION)) keeps mods out of the GPL: a mod made for HellForge
belongs to its author, under any licence they choose, with no notice required. Contributions:
[CONTRIBUTING.md](CONTRIBUTING.md).

Copyright (c) 2026 SkyeShade and HellForge's contributors.
