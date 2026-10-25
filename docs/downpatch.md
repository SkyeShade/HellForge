# Build patches

For maintainers: how setup turns a newer Steam build into the build HellForge runs, and how to publish a patch after a
game update.

HellForge runs one HELLDIVERS 2 build, the supported build in `src/HellForge.Core/supported-build.json`. When Steam has a
newer build, setup turns a copy of it into the supported build with a build patch (`Downpatcher.cs`). The player's Steam
install is only read, never changed.

## How setup finds a patch

Setup reads the list of patches at `https://files.hf-mod.com/downpatch/index.json`. When the list can't be reached, setup
uses the copy it saved last time, and failing that only the patches built into the release. It picks the patch whose
source build is the one Steam has, matching by build id or by the hashes of `helldivers2.exe` and `game.dll`.

```json
{
  "schemaVersion": 1,
  "packs": [
    {
      "sourceBuild": "26088801",
      "targetBuild": "25480438",
      "url": "https://files.hf-mod.com/downpatch/downpatch-26088801-to-25480438.zip",
      "sha256": "…",
      "size": 1234,
      "sourceExeSha256": "…",
      "sourceGameDllSha256": "…"
    }
  ]
}
```

The list only tells setup which pack to try; the pack itself is checked:

- Setup only takes pack URLs under `https://files.hf-mod.com/downpatch/`.
- The pack must match its SHA-256.
- Its manifest must name exactly the supported build's files, and every file it writes must have that file's size and
  Steam SHA-1 from `supported-build.json`.
- The patch tool (`hpatchz` from HDiffPatch 5.1.3) is pinned by hash.

A file that does not come out with its hash is never kept. If the patch fails for any reason (its download, a changed
Steam file, the pack itself), setup keeps the files it already wrote correctly and switches to the Steam console download
of the supported build, which copies the rest. A full drive is the exception (`DiskSpace`: checked before the download
and before patching; hpatchz exit code 24 is HDiffPatch's out-of-space error): setup asks for room and tries the patch
again, since Steam's download would need the same space. It does the same when no patch matches the build Steam has
([troubleshooting](troubleshooting.md#setup-cannot-find-the-supported-build)).

## What a pack holds

A pack is a zip with `downpatch.json` (its manifest) and one entry per file of the supported build:

- `copy`: Steam's file is already the supported build's, so setup copies it from the player's install.
- `patch`: a binary diff (HDiffPatch) from Steam's file to the supported build's, under `patches/`.
- `add`: the file itself, under `add/`, only when Steam's build has no such file, or for a file players change
  themselves, such as `data/settings.ini`.

Everything else comes from the player's own game, so a pack carries as little of the game as possible. The pack for
build 26088801 adds three small files whole: `bin/SONYGame.ini` and `bin/lua51.dll`, which the newer build no longer
has, and `data/settings.ini`.

## After a game update

1. Let Steam finish updating HELLDIVERS 2, and close the game.
2. Run `scripts/New-DownpatchPack.ps1 -Verify`. It reads the new build from Steam and the supported build from
   `Documents\HellForge\game` (any folder with the right files works: pass `-Target`). It checks every target file,
   diffs the changed ones and checks that each patch reproduces its file. Then it writes
   `artifacts/downpatch/downpatch-<new>-to-<supported>.zip` and an `index.json` with the new pack merged into the
   published one.
3. Upload the zip first, then `index.json`, to `downpatch/` on files.hf-mod.com.

To rebuild a pack for builds that already have one, pass `-Revision 2` (then 3, and so on). The new pack gets its own
file name (`...-r2.zip`), so a setup that is still downloading the old one is not cut off when `index.json` switches
over. Remove the old pack once the new list is live.

Setups that read the list (0.2.0-beta and later) pick up the new pack on their next run, and Check again on the setup
screen re-reads it, so no HellForge release is needed. Packs hold bytes derived from the game, so they never go into this
repository.

For the rest of setup, see [the docs map](README.md#what-setup-does).
