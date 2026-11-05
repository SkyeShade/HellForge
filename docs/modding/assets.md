# Game content in your mod: the asset kit

<!-- diataxis: how-to -->

This page is for authors whose game patch files hold the game's own bytes. That can be a model, texture or sound copied
from the game, a kitbash of several, a backported weapon, or cut content found in the game's files. Those bytes are Arrowhead's,
and sharing them shares Arrowhead's files. The asset kit takes them out of your mod. What you share is your own work
plus a recipe, and each player's HellForge rebuilds the exact files from their own copy of the game before the mod
loads.

You keep working the way you do now, with your usual tools and your patch files. The kit runs when you package the mod.

- [Quick start](#quick-start)
- [What the check tells you](#what-the-check-tells-you)
- [Stripping a mod by hand](#stripping-a-mod-by-hand)
- [What players get](#what-players-get)
- [Limits](#limits)
- [Data specs or the asset kit?](#data-specs-or-the-asset-kit)

## Quick start

```text
HellForge.Cli assets-check --mod <installation>/mods/my-mod     # which bytes are the game's, and from which build
HellForge.Cli package-mod --mod <installation>/mods/my-mod      # the zip to share, with the game's bytes taken out
```

`HellForge.Cli` is in `<installation>/app/cli`. `package-mod` strips every mod that has game patch files before it
zips it, so the zip never holds Arrowhead's bytes. If the recipe would not rebuild your files exactly, `package-mod`
stops and makes no zip.

The first run reads the game builds on your PC once: HellForge's game copy and HELLDIVERS 2 in Steam, about a minute
each. It keeps their fingerprints in `<installation>/temp/asset-kit`. Fingerprints are hashes, never game content. Later
runs take seconds.

## What the check tells you

`assets-check` reads every game patch file in the mod (`data/`, and a cosmetic pack's option folders) and compares it
with the game, byte for byte. It finds whole resources, resources you edited in places, and pieces of one resource
inside another, wherever they sit in your file. It changes nothing, and it writes a report:

```text
STRIPPED: 259.17 MB of Arrowhead's content (89.4% of the mod's patch files) is out; each player's HellForge rebuilds it
from their own game (HELLDIVERS 2 in Steam (build 26251122)), checked by SHA-256. 73 resource parts (28.44 MB) are in
none of the builds checked: ...
Report: <installation>/temp/asset-kit/my-mod.asset-report.md
```

The report lists each patch file with how much of it is the game's. It lists every game resource the mod uses and the
build it is in, and every resource that is in none of the builds checked. The exit code is 0 when nothing in the mod is the game's,
3 when it found game content, and 1 when it could not prove a recipe would rebuild the mod.

A resource in none of the builds checked is one of two things:

- your own work (a new material, your own texture or mesh): it is yours to ship, and it stays in the mod;
- content copied from an older build of the game that is not on your PC, such as cut content from launch: it is still
  Arrowhead's. The kit cannot match it without that build, so it stays in the mod as it is. Check where it came from
  before you share the mod.

## Stripping a mod by hand

```text
HellForge.Cli assets-strip --mod <folder> [--out <folder>]
```

This writes the mod as it is to be shared into `--out`, or into the installation's `exports` folder under the mod's
folder name, with the report beside it (`<out>.asset-report.md`). Your own folder is never changed. The stripped mod is
your folder with three differences:

- the patch files the recipe rebuilds are gone;
- `hellforge-recipe.json` lists each of those files with its size, its SHA-256 and how to put it back together. The
  pieces are the game's resources (named by resource, part and the SHA-256 of the piece), runs of one byte, and your
  own bytes;
- `hellforge-recipe.literals` holds your own bytes.

A patch file with nothing from the game in it ships as it is. Everything that is not a game patch file (the manifest,
code, pictures, your README) ships as it is too.

Before it writes anything, `assets-strip` rebuilds every file from the game alone and checks each one against its
original's SHA-256. A mod that does not rebuild exactly is not stripped.

Strip the folder you work in, not a stripped copy: a mod that already ships a recipe is refused.

## What players get

Players add your mod as they add any other. Before HellForge loads it, it rebuilds the game patch files into the mod's
folder from the player's own game and checks each file's SHA-256. The Mods page starts this as soon as it lists the
mod, and says so while it works. Each session does it too, before it stages mods.

- **Once per version.** A record beside the recipe (`.hellforge-assets.json`) says what was rebuilt. While the recipe
  and the rebuilt files are unchanged, nothing is rebuilt again. A new version of the mod, or a rebuilt file that
  changed, means a rebuild.
- **Nothing half-built loads.** A mod whose files cannot be rebuilt stays listed with the reason and does not load. The
  usual reason is that the player lacks the build the recipe takes something from.
- **Co-op and mod sync.** The modlist code friends compare counts the recipe, not the rebuilt files. The recipe holds the
  SHA-256 of every rebuilt file, so two players with the same recipe run the same bytes. Mod sync sends the recipe, never
  the rebuilt files, and the joiner's HellForge rebuilds them before it checks the download. A stripped mod is still
  data HellForge cannot check, as any game patch is, so it downloads only from the verified list or when trusted
  ([mods.md](../mods.md), "Playing with other mods").
- **Packages.** `package-mod` never puts rebuilt files in a zip, even from an installed copy.

A recipe from someone else is checked before it is followed. It may only make game patch files
(`<archive>.patch_<n>`, `.gpu_resources`, `.stream`) inside the mod's folder, never code, a manifest or a script. It may
never overwrite a file the mod ships. Every operation is range-checked.

## Limits

- **Byte matching.** A model re-exported through a 3D tool (re-encoded vertices, a new order) or a texture re-encoded
  from scratch no longer has the game's bytes. It is not found, and it stays in the mod.
- **Builds on your PC only.** The kit matches against HellForge's game copy and HELLDIVERS 2 in Steam. Content that is
  in neither, such as cut content removed from the game's files since, is not matched yet.
- **Steam updates.** Content the recipe takes from Steam's build comes from the player's Steam install. When Steam
  updates the game and that content changes or moves, players who have only the new build cannot rebuild the mod until
  you strip it again. HellForge's game copy is the pinned build and never changes, so the kit takes anything both builds
  have from the copy.
- **Game patch files only.** Files of other kinds (a Lua script beside the patches, a loose texture) are shipped as they
  are and are not checked.

## Data specs or the asset kit?

Both keep the game's bytes out of your mod, in different ways:

- A [data spec](data.md#data-specs) describes the change: copy this unit, recolour that lookup texture, use this OBJ.
  HellForge builds it on each PC, and it follows game updates by name. Use it when what you want is something a data
  spec can say.
- The asset kit takes patch files you already made with other tools, whatever is in them, and ships them as your bytes
  plus a recipe. Use it for work a data spec cannot describe, or for a mod you have already made.

Next: [Content](content.md), for changing and adding weapons, stratagems and entities in code.
