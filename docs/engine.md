# The engine

This page is the starting point for contributors to HellForge's engine: what it is made of, where each part lives in
this repository, how to build it and how to run its tests. Mod authors start at [modding/README.md](modding/README.md)
instead.

The engine is what runs the copied game locally, against the local backend, and loads mods into it. Its full source is in this repository, next to
the launcher, setup program, command-line tool and mod SDK. `scripts/Build-Engine.ps1` builds it from here into the
engine package, `artifacts/engine-package/hellforge-engine-<version>.zip`, and `scripts/Publish-HellForge.ps1` puts
that package into every installation's `app` folder.

## The parts

| Part | What it does | Source | In the package |
|---|---|---|---|
| Session runner | Runs every step of a session: the memory-only initialization and routing edits, the stand-in swaps, starting the local backend | `src/HellForge.Session` (NativeAOT) | `session/HellForge.Session.exe` |
| Local backend | Serves the game's online services on this PC, and co-op lobbies and relays between friends | `src/HellForge.LocalBackend` (NativeAOT) | `backend/HellForge.LocalBackend.exe` |
| Mod loader | Loads code mods into the game and applies registrations, stats and live edits | `native/mod-loader` (C, MinHook) | `sdk-shims/hellforge_modloader.dll` |
| PlayFab SDK stand-ins | Replace the game's PlayFab Party and Lobby DLLs, so co-op goes through the local backend | `native/sdk-shims`, generated with `src/HellForge.SdkShimGen` | `sdk-shims/PartyWin.dll`, `sdk-shims/PlayFabMultiplayerWin.dll` |
| Steam Cloud stand-in | `steam_api64.dll`: keeps the game's cloud files in the profile, starts the mod loader, and carries the network census | `native/steam-cloud` (`steam_cloud.c`, `net_gate.c`), export table generated with `src/HellForge.SdkShimGen` | `sdk-shims/steam_api64.dll` |
| Network block | Keeps the copy off the internet: the WFP tool, and the Windows Firewall rules and egress probe | `native/network-block` (the WFP tool) and `scripts/` | `scripts/` |
| Steam ownership check | Confirms the player owns the game | `scripts/Test-SteamOwnership.ps1` | `scripts/` |

The PowerShell scripts ship readable, because they are what a careful player audits. In the package, `scripts/` holds
`NetworkBlock.Common.ps1`, `Install-NetworkBlock.ps1`, `Install-WfpBlock.ps1`, `Test-SteamOwnership.ps1`,
`OfflineControlNetworkProbe.cs` and `hellforge_wfp.exe`.

A few things the engine uses live elsewhere in the repository:

- `src/HellForge.Core`: the shared library the session runner, launcher and CLI are built on (mods, staging, stats,
  game data).
- `native/mod-sdk`: the mod SDK headers. The loader is compiled against them, and its field and content tables are
  generated from `mappings/` first.
- `native/common/hf_privacy.h`: keeps native logs from naming the PC they ran on.
- `native/mod-loader/hellforge_modloader.c` includes the loader's other `.c` files, so the loader builds as one unit.

Some parts are not in the engine package. The in-game editor (`native/editor`) is a bundled code mod built by
`scripts/Build-Editor.ps1`. `native/launcher-stub` is the top-level `HellForge.exe`, which starts
`app\launcher\HellForge.Launcher.exe`. `src/HellForge.SteamProbe` is the Steam ownership probe, and
`src/HellForge.CaptureProxy` (`app\capture`) is the launcher's Sync Inventory capture. All four are built by
`scripts/Publish-HellForge.ps1`.

The session runner's `src/HellForge.Session/EarlyConstructor.cs` is generated from `scripts/EarlyConstructorExperiment.cs`
(pinned by its SHA-256) by `scripts/HostEarlyConstructor.Common.ps1`, with `HostLaterStatusExperiment.ps1` alongside.
[multiplayer.md](multiplayer.md) says what it does.

## The package and its version

The engine's version is `EngineVersion` in `Directory.Build.props` (currently 0.1.41). HellForge's own version,
`Version` in the same file, is separate (currently 0.3.0-beta).

Besides the folders in the table, the package holds `engine.json` and the licences (`LICENSE`, `LICENSE-EXCEPTION`).
`engine.json` lists the engine version, the game build, the commit and every file's SHA-256. `sdk-shims/build.json`
records the pins of the stand-ins and the loader, and `sdk-shims/MinHook-LICENSE.txt` ships with MinHook. Before it
writes the zip, the build fails if any file names the PC it was built on (the user folder or the git author's email).

## Building

You need:

- Windows 10 or 11
- the .NET 10 SDK
- Visual Studio 2022 Build Tools with the C++ workload (NativeAOT's linker)
- HELLDIVERS 2 installed through Steam

The game is needed because the `steam_api64.dll` stand-in's export table is generated from the game's own DLL. That DLL
is checked against its pin and is never committed or shipped.

```powershell
./scripts/Build-Engine.ps1
```

| Parameter | What it does |
|---|---|
| `-SteamApi <path>` | the game's `bin/steam_api64.dll`. By default the script finds it in the Steam libraries listed under `HKCU:\Software\Valve\Steam`. |
| `-SkipSmoke` | builds everything but skips running the native tests, the smoke tests and the NativeAOT backend comparison |

In order, the script:

1. fetches the pinned build tools into the ignored `tools/` folder (`scripts/Get-BuildTools.ps1`: Zig 0.17.0 and
   MinHook 1.3.4) and Microsoft's public PlayFab Party 1.10.16 and PlayFab Multiplayer 1.8.7 packages, each checked by
   SHA-256 (their DLLs are byte-identical to the ones HD2 ships, so their headers give the exact signatures; nothing
   from them is committed or shipped);
2. regenerates the loader's field and content tables from `mappings/` (`HellForge.Cli sdk-headers`);
3. generates and compiles the two PlayFab stand-ins, the mod loader, the `steam_api64.dll` stand-in with the network
   gate, and the WFP tool, all with Zig, then checks each stand-in's exports against the original;
4. builds and runs the native tests and smoke tests (see [Tests](#tests));
5. compiles the session runner and the local backend with NativeAOT;
6. stages the package, checks it for the PC's name, writes `engine.json` and zips it.

Intermediate output goes to `artifacts/sdk-shims/out` (the native DLLs and `build.json`) and
`artifacts/engine-package/staging`.

## Running it

To try the engine in a real installation, publish into it:

```powershell
./scripts/Publish-HellForge.ps1 -InstallRoot <installation>
```

Publishing builds the engine first (passing `-SteamApi` on), or takes a package built already with
`-EnginePackage <zip>`. It checks every file against `engine.json` and the game build against
`src/HellForge.Core/supported-build.json`, then swaps the new `app` folder in. Close the installation's launcher and
sessions first. `./scripts/Package-HellForge.ps1` makes `artifacts/package/HellForge-Setup.exe` the same way.

`Launch.cmd` builds the solution and opens the launcher from the repository. A launcher run from the repository uses
the engine that `scripts/Build-Engine.ps1` last staged in `artifacts/engine-package/staging`.

## Tests

| Command | What it runs | Needs the game |
|---|---|---|
| `./scripts/Test.ps1` | builds the solution, runs `tests/HellForge.Tests` and `tests/HellForge.LocalBackend.Tests`, then the CLI's path checks on synthetic fixtures | no |
| `dotnet run --project tests/HellForge.Tests` | the integration checks (mods, stats, SDK, data builder, editor, live edits, privacy and more) | no |
| `dotnet run --project tests/HellForge.LocalBackend.Tests` | the local backend checks (multiplayer, stats sharing, live and open war, registered items) | no |
| `py -m unittest discover -s scripts -p test_export_stats_schema.py` | the stats schema exporter | no |
| `./scripts/Build-Engine.ps1` | the native and smoke tests below | yes |
| `./scripts/Build-Engine.ps1 -NoSteamApi` | the same, but the `steam_api64.dll` stand-in is built against a synthetic export table instead of Valve's DLL: no Steam bridge, broker or bridge test, no package (CI runs this) | no |
| `./scripts/Build-Editor.ps1 -Test` | the in-game editor against real D3D12 and D3D11 swap chains (`overlay_test`), and its saving into Local Stats (`local_stats_test`) | no |
| `./scripts/Build-Editor.ps1 -Test -SkipOverlayTest` | the editor build and `local_stats_test` only, without a GPU (CI runs this) | no |

The engine build runs:

- `native/mod-loader/test/stats_test.c`: the loader's stats pass and more, on synthetic component and settings tables
  (it includes `stratagem_categories_test.c` and `loadout_preview_test.c`);
- `native/common/test/privacy_test.c`: native logs do not name the PC;
- `native/steam-cloud/test/net_gate_test.c`: the network gate in the built `steam_api64.dll`, all groups and the `dns`
  group alone;
- a start of `hellforge_wfp.exe` (installing and removing filters needs administrator rights, so that round trip is
  tested by hand);
- `HellForge.SdkShimGen smoke`, `smoke-cloud` and `smoke-modloader`: the stand-ins, the cloud stand-in against the
  real `steam_api64.dll`, and the loader end to end with a fake game (`fake_game.c`), a hook test mod
  (`hook_test_mod.c`) and the `machine-gun-10k-code` and `stratagem-compendium` example mods;
- `scripts/Test-BackendAot.ps1`: the NativeAOT backend must answer the same requests byte for byte as the JIT build.

`HellForge.SdkShimGen smoke-lobby-link` runs two backends and two copies of each stand-in over loopback; run it by hand
(see its usage).

## Where to go next

- [internals.md](internals.md): how the mod loader and the rest of the mod machinery work.
- [multiplayer.md](multiplayer.md): how co-op works, and the research behind it.
- [steam-cloud.md](steam-cloud.md): the Steam Cloud stand-in's design.
- [engine-roadmap.md](engine-roadmap.md) and [sdk-roadmap.md](sdk-roadmap.md): what is done and what is planned.
- [external-launchers.md](external-launchers.md): driving sessions from another program.
- [CONTRIBUTING.md](../CONTRIBUTING.md): licence terms for contributions.
- [The docs map](README.md): every document, by reader.
