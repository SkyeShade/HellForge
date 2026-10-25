<!-- A release: dev into main. Open it with ?template=release.md on the compare page, or
     gh pr create --base main --head dev --template release.md -->

## Release HellForge X.Y.Z

Changelog:

## Checklist

- [ ] 1. CI is green (the linter, the build and the tests)
- [ ] 2. `scripts/Test.ps1` passes locally
- [ ] 3. `scripts/Build-Engine.ps1` with the Steamworks SDK passes: the Steam bridge, broker and mod loader tests
- [ ] 4. `scripts/Build-Editor.ps1 -Test` passes on Direct3D 12 and Direct3D 11
- [ ] 5. A staff build of this commit was tested in game by the maintainer and the admins, with no open crash or
  blocker tickets
- [ ] 6. The version is bumped (`Directory.Build.props`), the roadmap and the README are updated, and the changelog is
  written
- [ ] 6b. If any `HellForge.Cli` command or its JSON changed, `docs/external-launchers.md` is updated and the change is
  in the changelog, so launchers that use the CLI know before the release lands

After merging: tag `vX.Y.Z-beta` on main and make the GitHub release from it.
