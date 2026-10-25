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
  editor are built and tested on a maintainer's PC, since they need the game's files.
- **Launchers:** if you change a `HellForge.Cli` command, an option or its JSON output, update
  [docs/external-launchers.md](docs/external-launchers.md) in the same pull request and say so in it. Launchers built
  on the CLI read that page.

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
