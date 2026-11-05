<!-- Pull requests go into dev, not main (CONTRIBUTING.md, "Branches and pull requests"). -->

## What and why

## Tested

- [ ] `scripts/Test.ps1` passes (CI builds `dev` after the merge, not this pull request)
- [ ] `py -3 scripts/lint/lint.py check --warnings-as-errors` passes: no errors or warnings (CI runs it too;
  `scripts/lint/README.md`)
- [ ] Docs or comments changed: `py -3 scripts/docs-lint/docs_lint.py check --warnings-as-errors` passes (CI runs the
  docs lint with markdownlint, lychee and Vale; `scripts/docs-lint/README.md`)
- [ ] Engine or editor changes: `scripts/Build-Engine.ps1` / `scripts/Build-Editor.ps1 -Test` pass
- [ ] In game, if it changes what players see or do: what was tried, solo or co-op

## Changelog

- [ ] `CHANGELOG.md` has this change's line under Unreleased, with this pull request's number (Added, Changed or Fixed;
      CLI and settings changes under "For launcher developers"), or: nothing a player, modder or launcher would notice

## For launchers

- [ ] No `HellForge.Cli` command, option or JSON output changed, or: `docs/external-launchers.md` is updated and the
      change is described here
