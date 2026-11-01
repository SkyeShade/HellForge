# Asset migrator (proof of concept)

Mods that ship the game's own resources (models, textures, sounds, scripts, whether copied whole, kitbashed or edited)
redistribute Arrowhead's content. The asset migrator takes that content out of a mod: what is left is the author's own
bytes plus a recipe that rebuilds the original files, byte for byte, from the game files the player already has.

Code: `src/HellForge.Core/AssetMigration/` (index, matcher, recipe, rebuilder) and `src/HellForge.AssetMigrator/` (the
command line). Tests: `tests/HellForge.Tests/AssetMigrationTests.cs` (synthetic data only).

## How it works

1. **Index** (maintainers, once per game build). Every resource part of each build is fingerprinted: its SHA-256, its
   size and where it lives, and content-defined anchors (an 8-byte hash of a 64-byte window, about one per 2 KB, picked by
   the bytes alone). The index holds no game content. Two builds (25480438 and 26251122, 53 GB read) index in under two
   minutes into 241 MB.
2. **Migrate** (when a mod is uploaded). Each file's anchors are looked up in the index to find which game resources it
   shares content with, including resources the mod does not replace (a kitbash's parts). Those resources are read from
   the game folders and matched against the file at every byte; ranges of 48 bytes or more become references, runs of one
   byte become fills, and the rest is the author's.
3. **Verify, then purge.** The recipe is rebuilt from the game folders alone and every file is compared with the
   original (SHA-256). Only then is the game content taken out. If anything fails, or the mod shares content with a build
   the migrator was not given, the mod is kept exactly as it was and flagged `NEEDS REVIEW`.
4. **Decide** (`MigrationVerdict`). What is left after the purge is either the author's own or game content from a
   build the index does not hold. The build catalog (`data/game-builds.json`) lists every build the migrator knows, with
   its depot manifests and layout, and says whether that list is the whole history since release (`historyComplete`).
   A mod that keeps resources no indexed build has is `PARTLY MIGRATED, NEEDS REVIEW` while the index does not cover
   the whole history; the verdict names the builds still to index. Once every build since release is indexed, content
   found in none of them is the author's own and the mod is `VERIFIED`. There is no size threshold.
5. **Rebuild** (the player's launcher). Each referenced part is found in the player's game folders by its resource and
   accepted only if its SHA-256 matches; each rebuilt file is checked against the original's SHA-256. A player without
   the build a part comes from gets a fetch plan instead: per build, the archive files the recipe needs (an old loose
   build's archives are files of their own) and the `download_depot` commands for that build's manifests, the way
   HellForge's setup already fetches its supported build. Nothing half-built is used.

### Steam chunk index: every build since release, from the depot manifests alone

A depot manifest lists each file of a build as chunks of about 1 MiB, with each chunk's SHA-1 (of its uncompressed
bytes) and its Adler-32 (rsync's rolling checksum, seeded with 0). Steam cuts a new build's files around the chunks it
already shipped, at any byte offset, with that rolling checksum, which is why most chunks of later builds start at odd
offsets. `steam-chunks` turns the manifests of every build in `data/depot-history/553853.txt` into one index (107 builds,
546,459 distinct chunks describing 0.45 TB of game data, 55 MB, built in 4 s from the cached manifests). It holds
hashes only.

`scan` (and `migrate --chunks`) rolls the Adler-32 over each mod file, checks every candidate's SHA-1, and then checks
the neighbouring chunks of the same game file in place, whatever their length. Any run of the game's bytes as Steam
shipped them, of about 2 MiB or more (one whole chunk), is found under any name and at any position, and attributed to
the game file and the builds that have it. Nothing is downloaded and no login is needed, so an uploader's launcher can
run it. It covers the loose-archive builds (February 2024 to February 2026), where cut content comes from. The packed
builds ship `bundles.*.nxa` chunks compressed, which a mod never holds.

Bytes the mod still ships that the scan proves are Steam's hold the mod (`PARTLY MIGRATED, NEEDS REVIEW`), even under a
resource name no build has. Only the exact ranges the mod still ships are counted. A kitbash's own edit next to a chunk
the recipe already takes from the game does not count, and neither does padding (runs of one byte are fills).
`resolve --chunks` then finds each such chunk in a build that has it and reads that archive's table there. It
fingerprints only the game resources at those places, plus the mod's leftover resources of the same name, and migrates
again. Only the chunks those resources sit in are downloaded, not the archive and not the build.

## Run it

```sh
M=src/HellForge.AssetMigrator/bin/Release/net10.0/HellForge.AssetMigrator.dll
PINNED=25480438=/path/to/HellForge/game/data
STEAM="26251122=$HOME/.local/share/Steam/steamapps/common/Helldivers 2/data"

dotnet $M index   --game "$PINNED" --game "$STEAM" --out hd2.hfsi
dotnet $M index   --add-to hd2.hfsi --game "<old build id>=<old build's data folder>" --out hd2.hfsi   # one more build
dotnet $M migrate --index hd2.hfsi --catalog data/game-builds.json --game "$PINNED" --game "$STEAM" --mod <mod folder> --out out/<mod>
dotnet $M rebuild --mod out/<mod>/mod --catalog data/game-builds.json --game "$PINNED" --game "$STEAM" --out rebuilt/<mod>
dotnet $M compare <mod folder> rebuilt/<mod>

# Steam chunk index (no login with the manifests cached), scan, and the targeted fetch (login needed)
dotnet $M steam-chunks --out steam-chunks.hfsc --depot 553853 --history data/depot-history/553853.txt
dotnet $M scan    --chunks steam-chunks.hfsc --mod <mod folder>
dotnet $M migrate --index hd2.hfsi --catalog data/game-builds.json --game "$PINNED" --game "$STEAM" --chunks steam-chunks.hfsc --mod <mod folder> --out out/<mod>
dotnet $M resolve --index hd2.hfsi --catalog data/game-builds.json --game "$PINNED" --game "$STEAM" --chunks steam-chunks.hfsc --mod <mod folder> --out out/<mod>
```

`migrate` writes `out/<mod>/mod` (what would be uploaded), `migration-report.md` and `verdict.txt`.
`kitbash-fixture --game "$PINNED" --out <folder>` makes a test kitbash from real game resources.

## Results (2026-10-31)

| Mod | Original | Game content found | Migrated mod | Verdict |
|---|---|---|---|---|
| R-40 Sightliner backport (premium warbond, from build 26251122) | 8.50 MB | 98.7% | 122 KB (mostly its DLL) | Rebuilt byte-identical; partly migrated while the history is incomplete (one 796-byte resource no indexed build has) |
| Kitbash from real resources: unit with 1.65 MB of another unit spliced in, texture with 64 KB repainted, 20 KB new | 5.04 MB | 98.3% (both units and the texture found) | 91 KB (the repaint and the new resource) | Rebuilt byte-identical; partly migrated while the history is incomplete (the author's 20 KB resource) |
| R-40, migrated with only the pinned build | 8.50 MB | content from 26251122 the migrator cannot read | unchanged | Needs review |
| Bingus Shared Loader (Lua) | 89.5 KB | 11.5% (a game Lua script it embeds) | 80.5 KB | Verified |
| APC & Stalwart Battlement (uploaded mod; 521 resources, cut APC content from older depot builds) | 290.17 MB | 89.5% from 25480438/26251122 | 30.73 MB | Partly migrated, needs review: rebuilt byte-identical, but 69 parts (28.5 MB) whose resources are in neither build stay in the mod until an older build is indexed |

Builds older than `bundles.nxa` (loose archives named by hash, with `.gpu_resources` and `.stream` beside them) are
read too: a `--game` folder without `bundles.nxa` is read that way. Authors do not need to know which build their
content came from: the site indexes the builds in the catalog, and the index says which build (and which archive)
each piece is in. Each extra build is about a minute of indexing and adds to the index only what is new in it.

Archives are matched one resource part at a time, each with its own candidates, so an archive of hundreds of
resources is matched completely (the APC's first run, matched per file, missed 7.4 MB of resources identical to the
game's). The report's last column says, for each part, whether the game has a resource of that name and whether its
bytes are the same. Matching the APC takes about a minute and 9 GB of memory (every candidate is kept in memory); a
production version would evict them.

## Limits

- The catalog lists only the two builds on this PC, with `historyComplete: false`, so until the older builds are added
  and indexed, every mod with a resource of its own (even the R-40's 796-byte sound dependency, or the kitbash's new
  resource) stays in review, not only mods with cut content like the APC. Filling it in is a maintainer task: every build's depot
  manifests (public build trackers list them), one `download_depot` per depot and an `index --add-to` each.
- Steam's `download_depot` fetches a whole depot at a manifest, so a player without an old build downloads that depot
  once (then keeps only the archives recipes use). A file-level fetch would need a depot client of HellForge's own.

- Matching is on bytes. A model re-exported through a 3D tool (re-encoded vertices, new order) or a texture re-encoded
  from scratch does not match and stays in the mod as the author's bytes. Detecting those needs geometry and image
  fingerprints (quantised vertex positions, perceptual tile hashes); that is the next step, for flagging rather than
  rebuilding.
- References to the current Steam build break when Steam updates the game and that content changes or moves. The site
  should re-verify every recipe against each new build (re-indexing takes about a minute), prefer the pinned build's
  copy of anything both builds have (it already does: places are tried in index order), and flag recipes that stop
  rebuilding.
- The encrypted `.dl_bin` gameplay tables are not read; data mods that change them are out of scope.
- Parts over the reader's 256 MB limit (one in build 26251122) are not indexed.
- Premium tagging (which warbond a referenced resource belongs to) is not done yet; the report lists every referenced
  resource and build, which is what that mapping needs.
