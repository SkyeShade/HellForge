# Contributing to HellForge

Pull requests and issues are welcome. This page covers the terms for contributions, how to build and test HellForge,
and where to start reading.

## Terms

Your contribution is licensed under the licence of the files it changes, the same as everyone else's:

- HellForge itself is under the GNU General Public License, version 3 ([LICENSE](LICENSE)), with the additional
  permission for mods in [LICENSE-EXCEPTION](LICENSE-EXCEPTION).
- The mod SDK (`native/mod-sdk/`), the mod template (`templates/`) and the example mods (`examples/mods/`) are under the
  MIT No Attribution License ([LICENSE-SDK](LICENSE-SDK)).

By opening a pull request you confirm that you wrote the change, or have the right to submit it under those terms. Do
not submit the game's files or code taken from the game, or code from other projects whose licences do not allow it.

## Branches and pull requests

- **`dev`** is where work comes together: open pull requests against `dev`. Development builds are made from it.
- **`main`** holds releases. It changes only by a release pull request from `dev`, once that release has been checked
  (the checklist in `.github/PULL_REQUEST_TEMPLATE/release.md`); a pull request into `main` from any other branch fails
  its checks.
- **Checks:** every pull request runs CI (`.github/workflows/ci.yml`): the structure lint
  (`py -3 scripts/lint/lint.py check`, [scripts/lint/README.md](scripts/lint/README.md)), the build, where warnings
  are errors, and `scripts/Test.ps1`'s checks. A pull request is merged once they pass. The engine and the in-game
  editor are built and tested on a maintainer's PC, since they need the game's files. CI also checks the engine's
  version (below).
- **The engine's version:** the engine has its own version, `EngineVersion` in `Directory.Build.props`, bumped with
  HellForge's `Version` in the version-bump pull request before each test build or release, whenever the engine
  changed since the last bump. An ordinary pull request does not bump it: say "engine bump with the next build" in its
  description. CI's "Engine version" job (`py -3 scripts/engine-version/engine_version.py`) finds the commit that set
  the current `EngineVersion` and lists the engine sources changed since: a warning on a pull request that changes the
  engine, a notice when earlier merges did, and a failure on a version-bump pull request (one that changes `Version`)
  or a release into `main` when the engine changed and `EngineVersion` did not. Engine sources are what
  `scripts/Build-Engine.ps1` builds into the package: `native/` except the editor and the launcher stub,
  `src/HellForge.Core`, `src/HellForge.Session`, `src/HellForge.LocalBackend`, `src/HellForge.SdkShimGen`,
  `mappings/`, the data files they embed and the scripts the package ships, but no `test/` or `tests/` folder and no
  `.md` file (the list is `ENGINE_SOURCES` in the script). Run it locally to see where the engine stands; add
  `--base origin/dev` to see what a pull request would get.
- **Launchers:** if you change a `HellForge.Cli` command, an option or its JSON output, update
  [docs/external-launchers.md](docs/external-launchers.md) in the same pull request and say so in it. Launchers built
  on the CLI read that page.

## Changelog

[CHANGELOG.md](CHANGELOG.md) lists what changed in each release, as a player updating from the release before sees it.

- **Each pull request** that changes something a player, a modder or a launcher would notice adds its line under
  Unreleased, in the same pull request: Added, Changed or Fixed, with the issue and pull request numbers ("(#18, #50)").
  CLI and settings changes go under "For launcher developers", SDK changes under "For mod makers".
- **Fixed** is only for a bug the latest release has. Something new since that release that a later pull request fixes
  goes into the line that adds it.
- **Write it in players' words:** what they see or do, not the code. Leave out test status and what is still to do.
- **The release pull request** renames Unreleased to the version and date (`## 0.2.0-beta (2026-10-22)`), puts a new
  empty Unreleased above it, and the GitHub release notes are written from that section. Issues its Fixed lines name
  are closed when the release lands.

## Building and testing

You need Windows 10 or 11, the .NET 10 SDK, Visual Studio 2022 Build Tools with the C++ workload (the NativeAOT linker
for the session runner and the local backend), and HELLDIVERS 2 installed through Steam. The `steam_api64.dll` stand-in
is generated from the game's own DLL, which is checked against its pin and never committed or shipped.

```powershell
./scripts/Test.ps1                # build and integration checks (no game needed)
./scripts/Build-Engine.ps1        # the engine: stand-ins, mod loader, native and smoke tests, NativeAOT programs
./scripts/Package-HellForge.ps1   # artifacts/package/HellForge-Setup.exe (builds the engine first)
```

- `scripts/Test.ps1` builds everything and runs the checks that need no game (`tests/HellForge.Tests`,
  `tests/HellForge.LocalBackend.Tests`).
- `scripts/Build-Engine.ps1` builds the engine package, `artifacts/engine-package/hellforge-engine-<version>.zip` (the
  version is `EngineVersion` in `Directory.Build.props`), and runs its native and smoke tests. It needs HELLDIVERS 2
  installed through Steam.
- `scripts/Publish-HellForge.ps1 -InstallRoot <folder>` updates an installation's app directly during development. It
  builds the engine, checks every file against its `engine.json` and puts it into the app; `-EnginePackage` takes a
  package built already.
- `Launch.cmd` builds and opens the launcher from the repository.

Downloads are pinned by SHA-256 and kept in the ignored `tools/` folder (`scripts/Get-BuildTools.ps1`: Zig and MinHook).
Build outputs and test fixtures go to the ignored `artifacts/` folder. The supported build's file list
(`src/HellForge.Core/supported-build.json`, names and hashes only) comes from Steam's depot manifests via
`HellForge.Cli supported-build`.

## Where things are

This repository is all of HellForge:

- the launcher, setup and command-line tool: `src/HellForge.Launcher`, `src/HellForge.Setup`, `src/HellForge.Cli`,
  `src/HellForge.Core`;
- the engine: the session runner `src/HellForge.Session`, the local backend `src/HellForge.LocalBackend`, the mod loader
  `native/mod-loader`, the SDK stand-ins `native/sdk-shims` and `native/steam-cloud`, the network block
  `native/network-block`, and `scripts/`;
- the mod SDK, its docs and examples, the in-game editor (`native/editor`) and the mod tooling.

Mods are built against the SDK headers alone.

## Where to start reading

- [docs/engine.md](docs/engine.md): the engine's parts and how its package is built.
- [docs/internals.md](docs/internals.md): how mods are implemented, what is checked and why.
- [docs/modding](docs/modding/README.md): the SDK as modders see it.
- [docs/README.md](docs/README.md): every other document, including the roadmaps and research notes.
