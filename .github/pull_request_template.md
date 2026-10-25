<!-- Pull requests go into dev, not main (CONTRIBUTING.md, "Branches and pull requests"). -->

## What and why


## Tested

- [ ] `scripts/Test.ps1` passes (CI runs it too)
- [ ] `py -3 scripts/lint/lint.py check` passes: no errors, warnings read (CI runs it too; `scripts/lint/README.md`)
- [ ] Engine or editor changes: `scripts/Build-Engine.ps1` / `scripts/Build-Editor.ps1 -Test` pass
- [ ] In game, if it changes what players see or do: what was tried, solo or co-op

## For launchers

- [ ] No `HellForge.Cli` command, option or JSON output changed, or: `docs/external-launchers.md` is updated and the
      change is described here
