# Making mods for HellForge

This guide is for anyone who wants to make a mod for HellForge, from a first number change to new stratagems and code
that reacts to the game. You do not need to know HellForge's SDK to start, and the first walkthrough explains each line
of its C code, so it suits people new to C too.

HellForge mods change HELLDIVERS 2 the way its own data and code do. New weapons, stratagems, entities and difficulties
are real game content, existing content is changed by named field, and logic runs on the game's own events and
functions. Everything runs in HellForge's own local copy of the game; the normal game is never touched.

## What you can build

| Kind | What it does | Code? |
|---|---|---|
| Stats mod | changes numbers (fire rate, magazine size, cooldowns, damage) by field name in a `hellforge-stats.json` | no |
| Data mod | models, textures, effects, sounds and icons: game patches, cosmetic packs, or a data spec HellForge builds from each player's own game | no |
| Content mod | edits existing weapons, stratagems and entities, and adds new ones: weapons, projectiles, damage types, stratagems and their selection headings, entities, behaviours, abilities, a difficulty | C |
| Code mod with logic | reacts to game events and makes the game act (heal, statuses, explosions, shot rules), edits values live | C |
| Hooks | reaches what the SDK has no event, field or action for yet: hooking game functions, reading and patching memory | C |

Content mods, logic and hooks are all code mods: one C (or C++) project under `src/` can do any mix of them. One mod can
also be several kinds at once, such as a code mod with a data spec.

HellForge also loads Lua mods made for HD2Runtime and Bingus Shared Loader. You write those with HD2Runtime's own
tools, and HellForge runs them through its [Lua bridge](../lua-bridge.md). They are client only and not part of what
friends compare in co-op.

## What you need

- HellForge, installed with HellForge-Setup. The installation is a folder (by default `Documents\HellForge`, or
  `%USERPROFILE%\HellForge` when Documents is in OneDrive) with a `mods` folder where every mod lives.
- For stats and data mods, a text editor. Nothing else.
- For code mods, the mod tools as well: HellForge's pinned Zig compiler, plus CMake and Ninja if you want an IDE. The
  launcher installs them for you into the installation's `tools` folder (the Mods page's **Install mod tools**), and a
  session fetches Zig by itself the first time it has a mod to build. Nothing is installed system-wide.

## Where to start

| You want to | Start with |
|---|---|
| change a number (fire rate, magazine, cooldown) | [Stats mods](stats-mods.md), or the in-game editor (F8) |
| learn how code mods work, from nothing | [Your first mod](first-mod.md) |
| change an existing weapon or stratagem in code | [Content: changing existing content](content.md#changing-existing-content) |
| add a new stratagem, weapon or entity | [Content](content.md), after [your first mod](first-mod.md) |
| put your stratagems under a heading of their own | [Content: selection headings](content.md#selection-headings) |
| recolour a model or ship your own model, sound or icon | [Data mods](data.md) |
| react to kills, damage or missions | [Logic at run time](runtime.md) |
| change something the SDK does not cover yet | [Hooks and game memory](hooks.md) |
| find out why a mod does not load or crashes | [Debugging](debugging.md) |

## The learning path

Read the pages in this order the first time. Each one ends with a link to the next.

1. [Your first mod](first-mod.md): a code mod from the template to in game, then changed, logged and shared. About ten
   minutes, most of it the one-time tools download.
2. [How mods run](concepts.md): the kinds of mods, the manifest, when your code runs and on which thread, how content is
   named, co-op, load order, conflicts, compatibility and limits. Read it before your second mod.
3. [Stats mods](stats-mods.md): number changes without code, `hellforge-stats.json`, and the in-game editor's Local
   Stats.
4. [Data mods](data.md): models, textures, effects, sounds and icons, as game patches, cosmetic packs or data specs.
5. [Content](content.md): changing existing weapons, stratagems and entities by field name, and adding new content of
   every kind.
6. [Logic at run time](runtime.md): game events, actions, live edits, packages and the game thread.
7. [Hooks and game memory](hooks.md): hooking game functions, reading and patching memory.
8. [Debugging](debugging.md): logs, refusals, crashes and testing with friends.
9. [Tools and sharing](tools.md): IDEs (Rider, CLion, VS Code), building from a terminal or the repository, prebuilt
   mods, releasing a mod.

## The SDK reference, template and examples

The [SDK reference](../sdk-reference.md) lists every call, component field and content constant. It is generated from
the SDK, so keep it open beside the guide pages.

| What | In an installation | In the repository |
|---|---|---|
| these guide pages | `app/sdk/docs/modding` | `docs/modding` |
| the SDK headers (`hellforge_mod.h` documents every call) | `app/sdk` | `native/mod-sdk` |
| the template, filled in as "My Mod", ready to copy | `app/sdk/template-mod` | none (`scripts/Publish-HellForge.ps1` makes it) |
| the template with placeholders, which `new-mod` fills in | `app/sdk/templates/c-mod` | `templates/c-mod` |
| the example mods | `app/sdk/examples/mods` | `examples/mods` |

The example mods:

| Example | Shows |
|---|---|
| `machine-gun-10k-code` | the reference example: a definition block, a load event and the mod's log, in about 60 lines |
| `golden-liberator` | a new arsenal weapon with a recoloured model, in about 20 lines |
| `talon-liberator` | a weapon with a projectile of its own, recoloured effects and another weapon's sounds |
| `cube-gun` | a weapon with a model of your own, from an OBJ file |
| `blue-sentry` | a new sentry stratagem with a recoloured model |
| `stratagem-compendium` | eleven stratagems, one file per family: Pelicans with turrets and a behaviour, a sentry, barrages, expendables, a silo with a countdown ability |
| `stratagem-categories` | stratagem selection headings and their order ([how headings work](../stratagem-categories.md)) |
| `hell-forge` | difficulty 11 |
| `hell-swarm` | a difficulty's settings row, changed directly |
| `vanilla-plus` | live edits of every stratagem's settings, and code patches: game code checked against the build, then changed (a branch, a jump to a stub of its own) |
| `more-voices` | calling a function the game's Wwise plugin exports (in the repository only) |
| `machine-gun-10k` | the same as `machine-gun-10k-code` without code: the manifest's `native` edits |
| `stats-example` | a stats mod without code |

To try an example, copy its folder into the installation's `mods` folder. `hell-forge` and `vanilla-plus` come
installed already, built and without their sources. To work on one of them, replace its folder in `mods` with the
example's folder instead of adding a second copy, since two mods with one id load neither.

## Good to know

- Use what the SDK offers before you hook. Fields, definition blocks, events and actions are checked against the game
  build, reported when they conflict, and kept working across HellForge updates. [Hooks](hooks.md) reach everything
  else.
- Mods ship their sources. Every player's HellForge builds a mod's C or C++ with the same pinned compiler, so friends run
  identical mods and nobody needs a toolchain of their own.
- Code mods run inside the game with the player's permissions. Players should only enable code mods from people they
  trust; the Mods page marks them with a CODE tag.
- The SDK, the template and the examples are MIT No Attribution, so you can copy from them freely. Your mod is yours,
  under any licence you choose.
- For players and mod managers, [mods.md](../mods.md) covers the Mods page, load order and joining friends, and
  [external-launchers.md](../external-launchers.md) covers the commands a mod manager uses.

Next: [Your first mod](first-mod.md).
