# How HellForge implements mods

This page is for people working on HellForge itself. It explains how the parts behind the [mod SDK](modding/README.md)
work, what they check before they change the game, and where the research behind them is. Mod authors do not need it:
the [modding guide](modding/README.md) covers everything a mod can do.

The mod loader is in `native/mod-loader/`, and the rest of the engine is described in [engine.md](engine.md). The
research notes and scripts named under `artifacts/` are not published; what they found is written here and in the
source.

## Contents

- [The mods folder and staging](#the-mods-folder-and-staging)
- [Load order](#load-order)
- [Source mod builds](#source-mod-builds)
- [The load chain](#the-load-chain)
- [Registries](#registries): [weapons](#weapons), [projectiles and damage](#projectiles-and-damage),
  [stratagems](#stratagems), [entities and behaviours](#entities-and-behaviours), [abilities](#abilities)
- [Difficulty 11](#difficulty-11)
- [Named fields](#named-fields)
- [Data spec builds](#data-spec-builds)
- [The stats pass](#the-stats-pass)
- [Live editing](#live-editing)
- [Event sources](#event-sources)
- [Native edits](#native-edits)
- [Research behind the examples](#research-behind-the-examples)
- [Where to go next](#where-to-go-next)

## The mods folder and staging

This section explains where an installation finds its mods, how zipped mods are unpacked, and what a session does with
them.

An installation loads every mod in `<installation>/mods/<folder>/` that has a valid `hellforge-mod.json`, and every
`<installation>/mods/<name>.zip` that holds one (at the zip's root or in its single top-level folder).

Zips are unpacked into `mods/.unpacked/<name>-<content hash>` whenever mods are loaded (by the launcher's Mods page or a
session start), and the mod is loaded from there. Deleting or replacing a zip removes its unpacked copy at the next
load, so a zipped mod is managed through its zip alone. Zips with absolute or `..` paths (files that would land outside
the mod's folder) are refused. There is no limit on size or file count.

A zip and a folder with the same files give the same modlist code. A zipped mod counts by what its zip holds: files it
writes into its unpacked folder as it runs (key bindings, a cache) are each player's own and never split friends. A mod
kept as a loose folder counts by every file in it, so share mods as zips.

Each session stages the data mods into the copied game, loads the code mods into it and applies the declarative native
edits while the game runs. Nothing is changed in the normal game. Staging copies only what changed since the last
session: a patch already staged from the same file (same size and time) stays, so an unchanged modlist starts without
copying anything. For the same reason, `mods/.unpacked` keeps the zips' hashes and the files they ship
(`.zip-hashes.json`, `.zip-files.json`).

The session's `result.json` lists what loaded (`mods`) and the status of network edits (`nativeMods`).
`native-mods.log` records network edits, and `mod-<id>.log` records stats and legacy component edits.

## Load order

This section explains how the Mods page's order becomes the order sessions load mods in, and why friends may order
their mods differently.

On the Mods page, rows are dragged from anywhere on them (or moved with ⋮ Move up / Move down) within their group. The
page follows the mods folder: a mod copied in or deleted there shows at once. Mods new to the page go to the top of
their group. The editor's Local Stats stays at the top, so what you edit in game applies last.

The bottom mod loads first and the top one last, as in the community's mod managers. Where two mods change the same
thing, the higher one wins: its data patches get the higher numbers, and its stats and its code's edits apply later.
Sessions load and stage mods in this order (`ModCatalog.LoadOrder`).

The modlist code compares the gameplay mods, not their order, and leaves out the cosmetic group. Friends who order the
same mods differently can still play together, but that is theirs to sort out: a conflict goes to a different mod on
each machine. What mods add (stratagems, weapons, entities, projectiles, damage types, abilities, behaviours) is
numbered alike whatever the order. Code mods are set up in the order of their ids, the same on every machine, and added
entities are numbered by their resource ids.

## Source mod builds

This section explains how HellForge compiles code mods that ship their sources, and when it trusts a cached build.

A code mod that ships its sources (C or C++ under `src/`) is built by HellForge, not by its author. Before every
session that has it enabled, `HellForge.Cli mods-stage` compiles it with the SDK's pinned Zig and the same flags on
every machine, into `<installation>/temp/mod-builds/<mod id>/`, and the loader loads that build. A DLL in the mod's own
folder (an IDE's build, say) is not used. The code is in `src/HellForge.Core/ModBuilds.cs`.

### The cache

A build is kept under its key: a hash of the mod's manifest and every file under `src/`, the SDK's headers, the
compiler (its pinned archive) and flags, and the game build. A change to any of them builds the mod again, so a mod
follows a game update by itself as long as the names it uses still exist.

### The check

`record.json` beside the build holds its key, the sources' hash and the DLL's SHA-256. A session uses the build only
when all three still match; a stale, corrupted or swapped DLL is built again and never loaded. This guards against
accidents and casual swaps, not against someone with access to the installation (who could edit the sources or the
record too).

### Errors

A mod that does not compile stays off for that session, and the session still runs. The record keeps the compiler's
message (file and line), which the launcher's Mods page shows on the mod's card, and the same sources are not compiled
again. Build now in the mod's ⋮ menu (or `HellForge.Cli build-mod --mod <mods>/<mod>`) makes the same build at once.

### Tools and friends

The first source mod fetches the pinned Zig into `<installation>/tools` (checksummed) if it is not there. The modlist
digest takes a source mod's sources, not a DLL, so friends who play together build identical mods.

### Prebuilt DLLs

A mod can ship its DLL instead (closed source, or built with other tools and libraries). If it ships sources as well,
it sets `"prebuilt": true` in `code` so that HellForge loads the DLL rather than building it. A prebuilt mod names the
game build it was built for, `"game": "25480438"`, and one built for another build is refused with a message saying so.
Without `"game"`, a prebuilt DLL still loads (mods from before this rule).

## The load chain

This section explains how the mod loader gets into the game's process and starts the code mods.

Every session runs `HellForge.Cli mods-stage`, which writes the plan `modloader-plan.txt` into the session folder (build
pins, entity profile, mods in load order). The session then sets `HELLFORGE_MODLOADER` and `HELLFORGE_MODLOADER_PLAN`
for the game. This happens with or without code mods, because the loader also collects the read-only stats snapshot
(see [The stats pass](#the-stats-pass)).

The `steam_api64.dll` stand-in, the first HellForge code in the game's process, starts `hellforge_modloader.dll` on its
own thread. The loader loads each mod and calls its exported `hellforge_mod_init(const HfApi*, HfMod*)`, setting mods
up in the order of their ids (the same on every machine); their edits apply in the Mods page's load order. It then
watches the game start and delivers the load events on its watch thread, in the order mods registered for them, never
two at once (`native/mod-sdk/hellforge_mod.h`). [modding/concepts.md](modding/concepts.md) says when each load event
comes, and [modding/runtime.md](modding/runtime.md) covers the events mods can subscribe to.

## Registries

This section explains what HellForge does in the game's data when a mod adds a weapon, projectile, damage type,
stratagem, entity, behaviour or ability, what it checks first, and how each one behaves in co-op. The mod's side of all
this is in [modding/content.md](modding/content.md).

### Weapons

HellForge adds the weapon to the game's entity data when that loads (the file is never changed), to the engine's
network types (so it can be equipped and synced), to the armory, and to the backend's item catalogue and the player's
inventory (`registered-items.json` in the session folder, or its `game` folder when sandboxed). The loader's
`modloader.log` and the mod's log record each step, and `native/mod-loader/registry.c` explains how.

In multiplayer, the weapon is part of the mod's code, which the modlist gate already requires friends to share.

### Projectiles and damage

A weapon gets its own projectile through `HfWeapon.projectile`. This gives the weapon its own
`ProjectileWeaponComponentData` record naming the type (+0), and removes the default ammunition from its own
customization record. Ammunition attachments carry an entity delta that sets a weapon's projectile when it is assembled
(the Liberator's default Full Metal Jacket sets 276). Without one, the weapon fires what its record names, as vanilla
weapons without ammunition do.

A new projectile is a copy of the base's 272-byte settings row under a type after the game's list (types 0-350; 64 new
types in all mods). The list has room for only 3 more where it is, so the loader moves it to a larger copy within 2 GB
of game.dll and rewrites the 66 instructions that address it (`native/mod-loader/projectile_sites.h`, generated by
`artifacts/projectile-registry/projectile_sites.py`), checking each one first. Then it raises the game's two type
bounds. The projectile's damage is the DamageInfo id at row +0x3C. `projectile_row(type)` returns the row after
`HF_EVENT_PROJECTILES_READY` (+0 is the type and must stay), and `set_field` / `hf_set` change a mod's own row as they
change an entity's record. For example, `hf_set(mod, type, ProjectileSettings, impact_explosion, HF_U32(82))` makes its
hits burst into the Orbital Gas Strike's cloud; HellForge loads an explosion's packages itself.

The game's DamageInfo list (ids 0-649) has no spare room, so the loader moves it to a larger copy within 2 GB of game.dll
and rewrites the 489 instructions that address it (`native/mod-loader/damage_sites.h`, generated by
`artifacts/projectile-registry/damage_sites.py`), checking each one first. New rows are copies of their base with the
damage scaled. `damage_row(id)` returns one after `HF_EVENT_PROJECTILES_READY` for further changes (+4 standard, +8
durable, +0x0C four armour penetration values).

Every assumption (the lists, their loaders, the bounds, every rewritten instruction) is checked against the reviewed
build once the lists have loaded. Any difference adds nothing and logs why, and weapons then fire their base's
projectile. The research is in `artifacts/projectile-registry/FINDINGS.md`, and the code in
`native/mod-loader/projectiles.c` and `damage.c`.

In multiplayer, every machine fires its own copy of a hand-held weapon's shots from its own data, so friends with the
same mods (the modlist gate) fire the same projectile. Types and ids are numbered in the mods' set-up order (by mod id),
the same on every machine whatever its load order.

`HfWeapon.sound_from` (a vanilla weapon's loadout id, such as `HF_ITEM_LAS16_SICKLE`) gives the weapon that weapon's
firing sounds: its own `ProjectileWeaponComponentData` record takes the donor's sound events (shot, loops, fire modes,
secondary, silenced). The donor's sound banks must load with the weapon, so the mod's data puts them in the weapon's
archive patch, as `examples/mods/talon-liberator` does with the Sickle's two banks.

No row member is a colour. A projectile's look is its particle effects: `spawn_effect` (row +0x48) draws the LAS-58
Talon's bolt, and `flight_effect` (row +0x70) its light and trail. An effect's colours are curves over the particle's
life (10 key times, then 10 RGB keys on a 0..255 scale), not its materials or textures. A recoloured effect therefore
has to come as data, in an archive the weapon loads (as the gold Liberator's model does). `examples/mods/talon-liberator`
turns the Talon's yellow curves blue.

### Stratagems

The game's stratagem list (`game.dll + 0x37CB600`) holds exactly the 150 StratagemTypes its enum names. HellForge moves
it to a larger copy once the game has loaded its stratagems, as it does the damage list: 339 instructions address it,
and 69 bounds on the type grow by the number of new stratagems. The names the mission report reads by type move with
it. Every instruction and bound is checked against the reviewed game build first, and anything else adds no stratagem.
The review is in `artifacts/stratagem-registry/BOUNDS.md` (not committed).

There can be 48 new stratagems in all mods: the loadout picker lists at most 150 available stratagems, and up to 96
vanilla ones can be. A new stratagem does not load its base's ship-upgrade (level) packages (the loadout keeps one per
vanilla type), never counts in the mission statistics, and is never a mission's random pick.

To make it owned, the local backend lists each new stratagem for the session as a copy of its base's catalogue item
(level, price, traits) and its child under the new stable id, and adds it to the inventory. Nothing is saved to the
profile; a saved loadout that names a removed stratagem drops it.

A stratagem's code must be neither another stratagem's code nor a prefix of one, and must not have one as a prefix.
`artifacts/stratagem-registry/codes.py <code>` lists the vanilla ones it collides with.

Its icon is a data spec `icon` resource in the stratagem icons archive `007e093ca718ca1a` (see
[Data specs](modding/data.md#data-specs)), as in `examples/mods/stratagem-compendium`. A missing icon material crashes
the game, which is why the spec always builds both. The image follows the stratagem pack's format: red where the icon
takes its category's colour, green where it is white (the in-game editor draws it that way too).

In multiplayer, peers exchange stratagem types as numbers, so every player must run the same mods. The modlist gate
ensures it, and types are numbered in the mods' set-up order (by mod id), the same on every machine.

`payload` / `payload_count` name what a call spawns, and `package` what it loads for the mission. A Pelican drop (a base
of delivery kind 3, such as `HF_STRATAGEM_EXO45_PATRIOT`) spawns `payload[1]` carrying `payload[0]`; `{0, pelican}`
sends an empty Pelican.

The game names a stratagem "<designation> <title>" on the permit, the designation being its payload entity's (a
stratagem row has none). So a copy of the A/MG-43 sentry whose sentry is a copy too reads "A/MG-43 <title>" until the
sentry entity is given text of its own (`HfEntity` / `HfEntityDef` `designation` and `title`, SDK 7). The compendium's
HMG sentry names its sentry "A/MG-101" and its stratagem "Heavy MG Sentry".

In the in-game editor (SDK 7), every custom stratagem gets a page under its category, with its own icon (the PNG its
data spec builds the icon from). The page is a vanilla stratagem's (`editor_like`; 0: its base's) pointed at the
stratagem's own data. That means its own settings row, and for each vanilla entity on that page the mod's entity
standing for it, with that entity's projectile and damage where the mod copied them from the page's (one copy of each
base; two of the same base are a guess, so their fields are left out). `editor_entities` (`{entity, like}` pairs ending
in `{0}`) names the entities; by default, each payload entity a mod added stands for the vanilla entity it copies.
Fields with nothing of the mod's to point at are left out, so the page never edits the base, and a shared record is
refused as for any live edit (give the entity `own_all` to edit everything).

The page's edits are saved in the editor's own `mod-stats.json`, not in Local Stats (the stats pass knows no mod
content), and the editor applies them as live edits at every session's start. In co-op the host's go with its Local
Stats: a joiner's editor takes its own off and applies the host's (`host-mod-stats.json`, which its backend writes), and
puts its own back when the host goes. Tools read the same through `content` (`HfContent`: every stratagem, entity,
projectile and damage type mods added, with what it copies).

For selection headings, [stratagem-categories.md](stratagem-categories.md) has the implementation notes and acceptance
checks (controller selection and saved loadouts still need in-game validation).

### Entities and behaviours

`own_all = 1` (SDK 7, on `HfEntity`, or on `HfEntityDef` with `size = sizeof(HfEntityDef)`) makes a copy share nothing:
it gets a record of its own in every component of its base, each a copy of the base's. An edit of the base (a stats mod,
the in-game editor) never changes the copy, and the copy's edits never change the base. HellForge sizes each record
from the profile's component, or from the table's shape among every table of the build's entity data
(`native/mod-loader/component_sizes.h`, from HD2Runtime's component catalogue: its index slots and its size). A table it
cannot size for certain stays shared, and the mod's log names it. Every component of the A/MG-43 sentry sizes, and
`examples/mods/stratagem-compendium` builds every stratagem this way.

`add_behaviour` adds a behaviour: a vanilla behaviour that HellForge extends, under an id of its own (694 and up), which
an entity runs through `HfEntity.behaviour` (its own `BehaviorComponentData` record names it). The game's behaviour
system dispatches it like its own. Each of its six per-behaviour switches (update, stage change, event, stage count, and
the two network event mappings) sends ids above 693 to HellForge, which runs its behaviours and sends every other id
where the game always did.

The one base so far is the transport Pelican's (`HF_BEHAVIOUR_PELICAN_TRANSPORT`, 667). It flies to the beacon and
hovers. Its HellForge version holds there for `hold_seconds` instead of 6, circling the beacon (`orbit_radius`,
`orbit_height`) and turning toward its mounted turret's target (`face_target`), then departs. Everything else is 667's
own code. If HellForge cannot install its behaviours (on another game build), entities given one run their base's. The
review is in `artifacts/pelican-registry/behaviour/DISPATCH.md` (not committed).

`examples/mods/stratagem-compendium` (`src/pelicans.c`) uses all of it, as definition blocks
([modding/content.md](modding/content.md)): two Pelicans with turrets of their own (the Gatling's fires the MG-206's
armour-piercing rounds at the Gatling Sentry's 1600 RPM) on that behaviour, called in by two new stratagems.

### Abilities

A silo countdown (`flags = HF_ABILITY_HOLDS_LINKED_LAUNCH`, no base) is for a copy of a silo's targeting remote, named
in its fire modes. Firing the remote holds the launch of the missile it is linked to for `hold_seconds`. The countdown
runs on the missile, so it goes on once the remote is tossed. Then the missile launches at the point the laser marked
last, and while the laser stays on, it follows it, as the silo's does. The compendium's Shredder Silo
(`src/shredder_silo.c`) is the example.

HellForge does what the remote's link does at a launch (engine, explosive set off, owner, guidance target, the rack
letting go) through the game's own functions, each checked against the reviewed build. If one differs, the remote
launches at once, as the silo's does. This is experimental (2026-10-18): if the laser launches the missile before the
remote's fire runs, there is no countdown, and `modloader.log` says so.

The game's tick dispatcher (game.dll 0x11509E0) runs an ability 60 times a second from tick 0 until it reports it is
done. Its ids end at 2867. HellForge points the dispatcher's default branch at a stub that hands its own ids to the
loader (`native/mod-loader/abilities.c`) and every other id on, as it does for behaviours. A HellForge ability reports
"running" through its hold, then calls its base with the tick less the hold.

Sounds play in the world at the entity, on its unit's sound source, as the game's weapon sounds do: a silo countdown's
at the silo, and with no unit, on the world's default source, heard everywhere. On machines where the countdown's start
is replicated it plays its sounds too (not yet seen in a two-PC session). A sound whose bank is not loaded plays
nothing; `package` loads it (a package of HellForge's catalogue). `modloader.log` says when an ability is set off and
when its base runs.

In multiplayer, the silo, its missile and its remote belong to the machine that called the silo in, and the countdown
runs on every machine its start reaches. Since engine 0.1.21, as with the game's own silo, only the machine with
authority over the missile at the last tick launches it, and the launch reaches the others with the missile's state;
they keep the link marked until it arrives. (Engine 0.1.19 launched on every machine, which set the explosive off once
per machine.) The remote's own fire runs on the machine with authority over the remote. The comments in
`native/mod-loader/abilities.c` have the details, including the guard for a remote left attached to nothing when the
missile lands.

## Difficulty 11

This section explains how `add_difficulty` adds a level after Super Helldive: which game tables move or grow, what each
part of the level changes, and what it checks first. The mod's side is in [modding/content.md](modding/content.md).

`add_difficulty` adds level 11 after Super Helldive: a copy of level 10's settings (enemy mix, packages, mission rules)
with its own title, description, unlock item and enemy budgets (`encounter_points`, `guardforce_points`,
`spawner_cooldown`; level 10 has 600, 600 and 1.5 s). One mod can add it.

The game keeps its 10 difficulties in a static table of 0x330-byte rows, in 19 identical copies. HellForge moves it once
`game.dll` has filled it: one table with 11 rows replaces every copy for the 57 instructions that read them
(`native/mod-loader/difficulty_sites.h`, generated by `artifacts/difficulty11/difficulty_sites.py`, which is not
committed; the research is in `FINDINGS.md` there). The difficulty API's limits of 10 become 11: the highest level, row
and level lookups by unlock item, and the unlocked mask and highest unlocked level. Every pinned instruction, limit and
copy is checked first. Anything else refuses the change, and the game keeps its 10 levels.

Level 11 is published in the lobby like any other level, so friends need the same mod (the modlist gate checks it).
`examples/mods/hell-forge` adds Hell Forge: 1500 encounter and guardforce points, a 1 s spawner cooldown, 8 optional
and 2 secondary objectives, three times the outposts, a 72-minute countdown, twice the map radius, patrols twice as
often and up to 1.5 times as many, the M-102 Gunner FRV granted in every mission, every flag below, and its own icon.

### Choosing it

On the war table, level 11 is the carousel's eleventh place, after Super Helldive. The bar's object holds exactly 10
widgets (one per level; another member follows them), so level 11 gets an eleventh of its own. The bar's build, update,
destroy and choice loops run to 11, and the five instructions that turn an index into a widget address go through stubs
that give index 10 the bar's own extra widget (a pool of 32, one per bar). The band sprite behind each icon comes from a
10-entry array read by level, which moves too (level 11 takes level 10's).

### Icon and name

`HfDifficulty.icon` names an icon the mod's data spec builds (`"icon"` with `"prepare": "image"`, archive
`007e093ca718ca1a`). The bar's own image setter takes atlas sprites only, so for level 11 its icon call goes through a
stub: Super Helldive's sprite first (it gives the image element its resource), then the mod's texture with the whole
texture as its UV rect. Other places that draw a level's icon (the war table's mission markers) take atlas sprites only,
so level 11's row keeps Super Helldive's sprite for them. The bar draws the icon in 200 x 138, so make the art fill the
square texture. The title is shown in capitals, as the game's names are.

### Enemies

Level 11 has its own enemy tech level, 11. Each roster row (0x80) holds a spawn weight per tech level at +0x30, with 8
unused bytes before them, so the weights move 4 bytes down and level 11's (level 10's to begin with) goes after them; the
six readers' displacements follow. That is every row of all six rosters: the sixth (0x21CB470, one row) is read by the
same readers, and left in place it read level 1 as 0 and had no composition at 11. Compositions allowing up to tech 10
allow 11 (319 rows), and the director's clamp of
the tech level becomes 11. The level gets its own budgets. A single enemy group still holds at most 32 enemies (more
crashes the game), so bigger budgets mean more groups more often.

### Unlocking

Each level's row names an inventory item that unlocks it. Level 11's item is a copy of Super Helldive's in the
backend's catalogue (`registered-items.json`, `src/HellForge.LocalBackend/RegisteredItems.cs`). A player who holds
Super Helldive's item holds it too. Nothing is saved to the profile.

### The live cap

The game client configuration carries Arrowhead's cap on the level (online override 48726272, "10"), and the game uses
the lower of it and its own maximum. With a difficulty registered, the backend serves the cap as 11.

### Rewards

Level 11 pays 1800 XP and 2200 requisition per mission.

### Patrols

`patrol_frequency` divides the row's per-faction patrol interval (+0x2A0..; level 10: 65 / 78 / 75 s for Terminids /
Automatons / Illuminate), and `patrol_cap` scales the most living patrol enemies (+0x2AC..; 41 / 39 / 48; the HiveMind
scales it further by mission progress and players). The director reads both from the row every frame.

### Flags

`flags` takes these:

- `HF_DIFFICULTY_SECOND_PRIMARY` makes every mission at the level two missions in one. At the end of objective
  selection (0x1756730, every caller: war table, mission, preview), the loader picks a partner mission: same faction, a
  standard mission an operation lists with this one, placed on this map's layout, with main objectives this mission
  does not have. The pick is deterministic from the mission's seed, row and faction, so it is the same on every
  HellForge client. The loader runs the game's own selection for it and appends its main objectives and their steps
  (class 0 and 1). The steps are registered in the base missions' rows as class 1 with weight 0 (never drawn, so levels
  1 to 10 are unchanged), so the game gates every main objective on all steps of both missions.
- `HF_DIFFICULTY_CITY_STRONGHOLDS` gives the level's city maps the strongholds (Automaton fortresses, Terminid mega
  nests) its outpost counts ask for. The outposts set's two stronghold groups get copies of their items up to 100 m
  across, appended at level init, with class list {3} (the city layout's only class) and minimum difficulty 11, so only
  cities at the level take them. Open maps keep theirs.
- `HF_DIFFICULTY_CITY_ICBM` makes every city operation at the level start with a Launch ICBM mission. Three of the
  game's unused mission rows (one per faction) become city rows for levels 11..11: the faction's city row with the ICBM
  row's name, gen_icbm x 3 and the codes, power and refuelling steps, plus the city's side objectives. They are listed
  first in the city operations, with a weight that always wins the operation's first (kind 1) mission. The ICBM
  objectives' stamps get city copies the same way as the strongholds. These rows are not mixed with partner missions.
- `HF_DIFFICULTY_NO_ERADICATE` keeps Eradicate missions (their game mode's delta) at level 10 and below. Their kill
  objectives remain second main objectives.
- `HF_DIFFICULTY_NO_RAPID_ACQUISITION` and `HF_DIFFICULTY_NO_BLITZ` keep Rapid Acquisition and Blitz missions (mission
  kind 6) from being offered at the level.
- `HF_DIFFICULTY_RADAR` gives every mission at the level the radar station (an optional objective that reveals the
  map), marked on the map from the start.

The mission-end telemetry keeps 16 objectives (capped).

`mission_stratagem` names a stratagem type every mission at the level grants every player, in the mission group with
Reinforce and Resupply (no loadout slot; the type's own uses and cooldown). The header's descriptions of these flags and
fields are in `native/mod-sdk/hellforge_mod.h` (`HfDifficulty`).

### Objectives, outposts and map

Objective definitions allowing up to level 10 allow 11 (148 of them; without this a level-11 mission had no objectives
at all).

`optional_objectives` and `major_objectives` set the row's counts (+0x28, +0x2C; a mission's own list limits them).
Together they are at most 18. With more objectives, the map generator's objective list moves from 16 to 30 places and
its mission stamp list from 128 to 1024, both to the end of the enlarged generation block. Up to 12 of the 30 are the
mission's own, its live event's and a partner mission's.

`outpost_scale` sets the row's outposts per size and faction (+0x34..).

`mission_minutes` sets every mission's countdown. It is an entity delta of its own, registered in the game's entity
delta table (HiveMind +0x74 = the level's seconds), and added to a level-11 mission's delta list at launch (0xAD0E4E),
after the mission's own deltas, as High Value Asset missions get 1200 s. The shared definition is never written, so
other levels keep their countdown. The countdown's network definition (207, 0..2500 s, clamped by the host every frame)
is widened to fit.

`map_scale` sets the row's playable radius factors (+0xF0.., up to 2). The map generator's route cells (a Poisson disc
of 40 m spacing within 680 m of the world's centre) fill fixed arrays without checking them, so a larger playable area
alone overflows them. Above 1, each mission at the level gets a disk that covers its border (at most 1022 m: the world
is 2048 m) and a spacing that keeps level 10's number of cells, in quarter metres. Map type 1 lays them as a grid, and an
inexact grid splits its corners into extra vertices; at 2 x this gives 1022 m and 65.75 m. Level init writes the level's
radii (game.dll 0xF71CD8, after the map type is known), and the two spacing reads (0xF3614D, 0xF3732C) take the level's.

GenerateVoronoi (0xF3F8E0) lays a 16 m grid over the same square, keeps the points in the disk and builds their Voronoi
diagram (jc_voronoi). Level 10 nearly fills its unchecked arrays (5672 of 5680 sites, 5839 of 5849 vertices), so its
spacing grows with the disk too (0xF3FB04), rounded up to a quarter metre so the grid points stay exact in float. An
inexact spacing makes near-degenerate circles and extra vertices, overruns the vertex array, and the pass retries
forever (the war table's "Receiving Geodetic Data"). At 2 x the spacing is 24.5 m.

GenerateRouteCellVoronoi (op 3, 0xF93800) fills each route cell with points at its type's size / 2 or / 4. Level 10
makes 11954 areas of 13224, in proportion to the generated area, so that spacing grows with the disk too (0xF9420F,
x 1.04, in quarter metres).

On map type 1, FinalizeVoronoi (0xF68050) rebuilds the route cells from points over the whole square at each cell
type's size. It is scaled the same way (0xF68273); otherwise 2.25 x the points overrun the 1024 route cells.

The generator's stamp index (0xC04B80, for overlap tests of stamps, locations and roads) has 10757 slots. Level 10 uses
4892, and its 17 inserters don't check for a full index. Above 1 it gets 16383 (the same 14 index bits).

The level's locations (objectives, points of interest, outposts, ...) are two arrays of 2740 in the generation block
(G+0x758E20, 0xE0 each, and G+0x7EEBA0, 0x40 each). Level 10 makes about 824; 2 x maps with Hell Forge's objectives and
outposts made 3900 to 5500. Above 1 the block grows (0x11A7268 -> 0x13E7280 bytes) and both arrays move to its end with
8192 places: 223 instructions, 236 builder capacities and the block's size at 6 places (`location_sites.h`). The level's
location types (G+0x756C20, 96 x 0x10; level 10 uses about 44, 2 x maps 73..81) move after them with 512 places (99
instructions, 92 capacities). The minimap's arrays (proxies G+0x8198A0, 4096 x 0x30; scatter G+0x8498A0, 2048 x 0x58;
proxy units G+0x118BEFC and G+0x118FEFC, 4096 and 16384 x 4) follow with 4 x their places, because a 2 x map's proxies
ran over the scatter into the terrain merge slots (G+0x8758A8). The block is then 0x15A9280 bytes.

Blocks are made with a world, after the loader installs this; if one exists already, the move is skipped. Structured
maps (map type 2, another generator) keep level 10's size.

The mission distance factors (10 levels x 3, read unclamped at 7 sites) move to a table with level 11's.

### Missions

The war table offers a level only the operations whose difficulty range includes it (operation settings, 25 rows, and
missions, 162 rows, each with a lowest and highest level). Every range ending at 10 ends at 11. The game's random pick
of a difficulty within a mission's range keeps 10, so nothing picks 11 by chance.

### Mission board

The war table generates three missions per level into a 110-slot board per planet: 30 for the planet's own (levels 1 to
10), then 10 for each of up to 8 regions. Level 11 takes region 7's ten: the planet's three and one for each of regions
0 to 6. A planet with a region 7 (only Cyberstan) offers no level 11 of its own (it offers none at any level) and puts
each region's level 11 in that region's level 1 slot, which its regions (difficulty 4 and up) never use.

## Named fields

This section explains where the SDK's field and content names come from and which files are generated from them.

SDK 6 names the fields of component records, so a mod never writes an offset (how they grow is in
[sdk-roadmap.md](sdk-roadmap.md#mappings)). One source names them: `mappings/components.json` (per component: index, record
size, the game build whose layout it is, and fields with offset, type, count, meaning, source and whether the game
confirmed it) and `mappings/catalog.json` (vanilla content by name). `HellForge.Cli sdk-headers --repo .` generates
these files from them:

| File | What it holds |
|---|---|
| `native/mod-sdk/hellforge_components.h` | a packed struct per component record (`HfProjectileWeapon`, `HfWeaponData`, `HfDeposit`...): named fields at their offsets, the rest as `_unknown_XX` padding, size and offsets asserted; field groups (`HF_GROUP_...`); and structs for the settings tables' rows (`HfDifficultySettings`, `HfStratagemSettings`, `HfProjectileSettings`, `HfDamageInfo`) |
| `native/mod-sdk/hellforge_content.h` | vanilla content constants, each commented with its catalog name: every armory item and its entity (`HF_ITEM_...`, `HF_ENTITY_...`), every stratagem (`HF_STRATAGEM_...`), projectiles, behaviours, damage, packages, tags, stratagem traits (`HF_TRAIT_...`), categories and beams; and the kinds `find` takes (`HF_KIND_...`) |
| `native/mod-loader/sdk_fields.h`, `sdk_content.h` | the loader's tables of the same fields (which a mod's fields must match) and content (which `find` resolves) |
| `docs/sdk-reference.md` | the SDK reference: the API from `hellforge_mod.h`'s comments, and every component, table and constant |

`hellforge_mod.h` includes both SDK headers. Edit the mappings, never the generated files; the integration checks fail
when they differ. Naming a researched range is one entry in the mappings: the padding around it splits, nothing else
moves, and mods built before keep working.

## Data spec builds

This section explains how a mod's data spec is checked and built from each player's game copy, and how to build one by
hand.

Everything is checked before anything is written: every resource the spec names must be in the game data (all missing
ones are listed), every edit must find what it expects, and new names must be new. When a session stages mods, the data
is built from the installation's game copy into `<installation>/temp/mod-data/<mod id>/<key>/` and reused while the key
holds. The key covers the data builder's version, the supported game build, the game's `bundles.nxa`, the spec and its
input files. A mod whose data cannot be built is not loaded, and the session's result lists why. The modlist code
counts the spec and the builder version, not the generated files: with the same game build they make the same bytes on
every machine.

To build or check one by hand, run `HellForge.Cli build-data --mod <folder>` (which checks the spec), with
`--install <folder>` (builds into that installation's cache) or `--game-data <game/data> --out <folder>`.
`scripts/Build-Mod.ps1` runs it for mods that have a spec (`-Install` or `HELLFORGE_INSTALL` to build). The code is in
`src/HellForge.Core/GameData/`.

## The stats pass

This section explains how stats files (`hellforge-stats.json`) are validated and written at load: the schema, the
guards, ownership, references, conflicts, raw edits, and what has and has not been tested.

### The schema

The embedded `src/HellForge.Core/stats-schema.json` lists the supported fields, their storage, ranges and sharing, and
blocked fields with refusal reasons. For the editor it also lists each target's armory slot, weapon class, host item and
stratagem family, and the catalogs' field order. The exporter reads literal catalogs without executing Lua:

```powershell
py scripts/ExportEntityProfile.py <HD2Runtime>/schemas/current.lua src/HellForge.Core/entity-profile.json
py scripts/ExportStatsSchema.py <HD2Runtime> src/HellForge.Core/stats-schema.json native/mod-loader/stats-layouts.h
```

The generated stats schema contains metadata, not game bytes or extracted default values. Named edits validate the
reviewed component header, stride, resource ownership and record index, or the settings row identity and layout, before
writing. The schema fingerprint must match the supported game.dll profile (build 25480438). Floats must be finite,
integer fields reject fractions and overflow, and catalog ranges are enforced. Invalid names, blocked fields, duplicate
properties and duplicate physical fields within a stats file reject that mod.

### When edits run

Component edits run on the entity-loading thread after registry additions, before `HF_EVENT_ENTITIES_READY`, once for
each entity-data load. Projectile, damage and stratagem edits run once after their tables become available and registry
installation has finished, using the moved table where one exists.

Explosion, arc, beam and status settings use uniquely identified retained allocations, validated against their
structural headers, row counts and relocated pointers. Default weapon magazine overrides use the attachment catalog's
reviewed entity-delta chain (attachment resource, settings index, component, member and data offset). Readiness is
polled for up to 20 seconds. Each validated allocation is edited once, and missing or ambiguous allocations refuse the
edit.

There are no per-frame, per-shot or periodic field rewrites. A settings reload later in the session does not reapply
stats. Every applied edit and runtime refusal goes to `mod-<id>.log`, and missing load-time machinery refuses the edit.

### Shared records and ownership

Shared records affect all consumers and produce a warning in that log. Add `"own": true` to an entity's entry to clone
its component record through the registry before editing; all fields in that entry must then be component-backed.
`"own": true` uses `OwnRecord` for the item's component fields only; projectile, damage and explosion definitions stay
shared.

Settings and attachment-delta ownership are rejected, because isolating these shared definitions requires reviewed
relinking of their consumers. A magazine edit affects weapons using that default attachment, and selecting a different
attachment can select different stats. Unverified gameplay effects are logged even when the structural checks pass.

### References in stats files

References take type ids (the editor's pickers name them, and `mappings/references.json` lists them):

- `attack.primary.projectile`: a weapon's projectile. For the Liberator, Dominator, Redeemer, Peacemaker, Diligence and
  Breaker it is `ammunition.projectile`, their default ammunition's entity delta, which every weapon equipping it
  shares and which applies from the next boot.
- `terminal.<role>.impact.explosion` and `terminal.<role>.expiry.explosion` (0 for none).
- `<damage>.status_<k>_type` with `<damage>.status_<k>_strength`, for k 1 to 4, on every damage type a target shows
  (`damage.`, `explosion.primary.impact.damage.`...; 0 is empty, and slots fill from slot 1).

Projectile pages take `terminal.impact.explosion` / `terminal.expiry.explosion`, and damage pages `status_<k>_type`. The
loader writes a reference once the packages it needs are loaded (see [References](#references) under Live editing).

### Conflicts

Mods apply in load order (the Mods page's bottom mod first). Conflicting stats and legacy component writes are reported
in `ModCatalog` problems, and both mods stay enabled; the later write wins if its guard passes. An expected value is
compared with the value before earlier writes in that pass, so an intentional conflict does not defeat the guard.
Network-type conflict handling keeps its existing behaviour.

### Raw edits

For a reviewed component not exposed by a named field, `raw` requires an explicit expected value:

```json
{ "format": 1, "raw": [
  { "component": "DepositComponentData", "resource": "0x056DE1C5E21E723E",
    "offset": "0x0", "type": "u32", "expect": 1000, "value": 1500 }
] }
```

Raw storage types are `u32`, `i32` and `f32`. Offsets must be aligned and within a component in the pinned entity
profile. A mismatched expected value refuses the write. Raw writes also support `own` and log shared-record warnings.

### Coverage

Coverage is limited to reviewed numeric fields and the projectile-to-damage reference. Unsupported structured fields,
unreviewed references and ambiguous attack branches have explicit refusal reasons. The Liberator's default magazine is
supported through the newer attachment catalog, which supersedes the older weapon-catalog refusal.

### Observed values

Each local session writes `logs/sessions/<session>/stats-values.tsv` (format 2), even without active mods. The loader
observes each field once per load, after its stats pass, and reports a field that a stats file changed with its value
from before that pass (its fingerprint covers addresses only). The schema contains no observed values.

### Tests

The fixture checks are `dotnet run --project tests/HellForge.Tests`,
`py -m unittest discover -s scripts -p test_export_stats_schema.py`, and the engine's native stats test (every engine
build runs it). They use synthetic component and settings tables, including shared and owned records, guard refusals,
conflict ordering, moved damage tables, retained allocations and attachment-delta ownership chains.

Gameplay acceptance is a separate check in the isolated installation: primary rate of fire and magazine, secondary,
grenade fuse or damage, support charge, backpack, stratagem cooldown and uses, vehicle ammunition, projectile speed and
damage. Fixture results establish resolution, guards and writes; they do not establish that every unverified field
affects gameplay. The current implementation has not yet passed this in-game category checklist.

## Live editing

This section explains how fields change while the game runs: the timing of each field, the live-edit calls, the
settings tables and passives they reach, the in-game editor built on them, and the pipe that carries co-op stats.

Code mods (`set_live`), the in-game editor and co-op stats sync change fields in the running game through the same path
as deferred edits, with the same checks and conflict reports.

### When a change applies

Every mapped field has an `applies` timing in `mappings/components.json` (a component default, overridden per field),
shown as a `[tag]` in `hellforge_components.h`:

- `now`: the next time the game reads it (projectile, damage, explosion, arc, beam and status settings, the difficulty
  director's budgets and patrols);
- `respawn`: when the entity is next made (component records: weapons, backpacks, vehicles, sentries; the next spawn,
  re-equip or call-in);
- `mission`: when the next mission loads (stratagem settings, packages, level generation).

`appliesVerified` marks timings confirmed from game code. The rest are inferred from where the game reads the field,
and the header marks them with a "?" (`[respawn?]`). Every stats-schema field is a mapped field (the schema's fields were imported into
the mappings, and a test keeps them in step), so the live editor and the SDK check the same layout.

### set_live

`set_live(mod, target, field, value, flags)` returns `HF_APPLIES_NOW`, `_RESPAWN`, `_MISSION` or 0 (refused, with the
reason in the mod's log). The target is found by identity every time, never by address: an entity by its resource id,
or a settings table row by its id (a projectile type, a damage id, a stratagem type, a level, or an explosion, arc, beam
or status row id). One element of an array field is `HF_FIELD_AT(Component, field, index)`.

A mod's next live edit of the same field replaces its earlier one, and `value` NULL undoes it, putting back the value
from before it. A record other entities share is changed only with `HF_LIVE_SHARED`, which changes all of them, as a
stats file does. Live edits stay for the session and are renewed if the game reloads its data; nothing is saved.
`hf_live(mod, target, Component, field, HF_U32(60), 0)` is the macro form. Deferred `set_field` edits reach table rows
the same way and wait until the table has loaded.

### get_live

`get_live(mod, target, field, &out)` reads a field, found as `set_live` finds it (a shared record too), into an
`HfLiveValue`. `now` is what the record holds. `base` is the field without the Local Stats mod's change and without live
edits: another mod's value (`out.mod` names it), else the game's own. It returns the field's timing, or 0 when it cannot
be read yet. `set_live(..., NULL, HF_LIVE_BASE)` puts the base back, past a value Local Stats applied at load: this is
how an editor removes a change it saved.

### Boosters

Boosters are a settings table, `BoosterSettings` (game.dll's static booster definitions, a row per `HF_BOOSTER_...`
value). `tuning` (+0x08 f32) is the value its effect reads, which `mappings/boosters.json` explains per booster (damage
taken for Vitality Enhancement, stamina for Stamina Enhancement, ...), with its range. Boosters are also a stats category
(`"boosters"` in hellforge-stats.json, keyed `booster.<value>`), so the in-game editor edits and saves them. Rows are
used once game.dll's image and all 21 row ids read as reviewed.

### Armor passives

`set_passives(mod, armor, second)` and `get_passives(mod, &state)` set the local player's armor passive and a second
passive on top (the helmet's slot), by passive id (`mappings/passives.json`), or `HF_PASSIVE_KIT` for what the kits
give. This works solo only: with other players the kits' values come back and the override waits. The loader keeps it
through armor changes (it looks every 250 ms) and only replaces the kits' value or its own. It acts after proving the 59
code pins of HD2Runtime's research. Passives whose effect needs a package (INTEGRATED EXPLOSIVES, ADRENO-DEFIBRILLATOR)
are refused unless the armor gives them.

### References

References are HD2R Editor's payload swaps. Fields marked `"ref"` in the mappings name another table's row:
`ProjectileWeapon.projectile` (the projectile a weapon fires), `ProjectileSettings.impact_explosion` and
`expiry_explosion` (+0x90, +0x9C; 0 for none), `DamageInfo.status_1_type` .. `status_4_type` (+0x2C, 8 apart, each
followed by its strength; slots fill from 1), and the damage of an explosion and of a projectile.

A new value must be a row the reference catalogue knows (`mappings/references.json`, from HD2Runtime's reviewed
catalogues with `scripts/ExportReferences.py`): a projectile type another weapon fires, a catalogued explosion, or an
attachable status effect. It is written once the packages its effects need are resident. Until then the edit waits
(logged), and it fails if they do not load within 90 seconds. An explosion whose shrapnel leads back to the projectile
it is set on is refused. A status after an empty slot is written but logged, since the game stops at the first empty
one. Stats files take the same references, with the same waits.

### Packages

`load_package(mod, package)` loads a catalogue package through the game's own RefcountedPackageSystem (its request
checked byte for byte), on the game thread, once per session, and holds it for the session: at most 64, and never when
the game's reference map is 3/4 full. Residency is read from the engine's package list. A mod's `load_package` is per
client, and nothing it loads is advertised to other players. In co-op the loader also tells the session's backend which
packages it holds for this machine's own edits, mods and actions (the live pipe's `packages` line), and loads the
lobby's other packages when the backend asks (`preload`); a friend's may not take the last 16 of the 64
([multiplayer.md](multiplayer.md#assets-in-co-op)).

### The game thread

`on_game_thread(mod, callback, user)` calls back every game update, on the thread the game's own engine calls come from
(sounds, package requests). The callback comes from the menu sound manager's update, which the loader hooks once its
bytes are checked.

### The in-game editor

The in-game editor (`native/editor`, F8; built by `scripts/Build-Editor.ps1`) is a code mod on these calls, with the HD2R
Editor's layout and look.

Its catalogue (`editor-catalog.json`, from `HellForge.Cli editor-data`) holds the primaries, secondaries, throwables,
stratagems and boosters with their stats fields, and the armor perks (two choice rows with a picker, kept in the
editor's settings and set again at every boot). Its weapon and grenade silhouettes are the game's own HUD sprites, and
its text is in the game's own FS Sinclair, both extracted from the installation's game copy into the editor's folder
(`game-icons.bin`, `fs-sinclair*.ttf`; never published). Its buttons play the game's menu sounds through the game's own
UI sound path (checked byte for byte; silent on another build).

The ENEMIES group is the HD2R Editor's Enemies pages: Terminids, Automatons, Illuminate, Structures and Other Entities.
These are the 1,170-odd entities HD2Runtime's enemy catalogue lists, from `mappings/entities.json`
(`scripts/ExportEntities.py`, which reads HD2Runtime's `domains/enemy_authoring.lua` and `enemy_controls.lua` as
literals). Each entity's page has Health & Armor, a group per damage zone, a group per attack, and its other sections
(Spawning, Director Multipliers, Shield, Size & Geometry...), with only the fields whose exact bytes the engine maps
(`mappings/components.json`): about 38,800 rows, every row the HD2R Editor lets you edit there but one (its authored
value lies outside its own write range). `scripts/ImportSchemaMappings.py <HD2Runtime>` names the fields of components
the mappings have (each damage zone's, the HiveMind's director multipliers), and adds the components the entity profile
locates (`src/HellForge.Core/entity-profile.json`: Unit, AiSpawner, SquadTransportFormation, SquadTransport, Gore,
StratagemBlocker, Bughole); `HellForge.Cli sdk-headers` then regenerates the engine's tables and `ExportEntities.py`
offers them. The profile's entries for those come from the entity file's own frames (`scripts/ExportEntityProfile.py`
EXTRA: each frame found by the component index the u32 before it holds, as the game's loader 0xFDB440 reads it; the
method reproduces every other entry). With a third path, the exporter writes the rest by entity and reason: rows the
HD2R Editor only shows. Rows are keyed as the HD2R Editor's are
(`en|<entity>|<path>:<zone or attack>:<field>`), so its presets import and export them, and a stats file saves them under
`"enemies"`. The Helldiver's own entity adds its main fields (health, armor, constitution, regeneration) to the Helldiver
page, and the MS-11 Solo Silo's rack and missile are parts of its stratagem's page (`StatsSchema.EntityParts`), each
leaving out bytes the page shows already. Only an enemy class's main health and zone armor are proven in game
(HD2Runtime's evidence); the rest are marked unverified.

The SPAWNING group is the HD2R Editor's Mission Difficulty, Spawn Rosters and Spawn Compositions, from
`mappings/director.json` (`scripts/ExportDirector.py`, from HD2Runtime's `domains/enemy_director.lua`), written by
`native/mod-loader/director.c`:

- **Mission Difficulty:** a level's `DifficultySettings` row (1 to 10, and 11 with a mod that adds it). It holds the HD2R
  Editor's three budgets, plus the objectives and per-faction patrol interval and cap. Without level 11 the game's own
  table is written: the copy HD2Runtime edits, and every other of its 19 copies that held the same bytes.
- **Spawn Rosters:** an enemy's spawn weight at each difficulty (`EnemyRoster`, a row by `(roster << 16) | row` in
  `difficulty.c`'s roster order). The weights sit at +0x30, or at +0x2C once level 11 gave every row an 11th, which is
  then `EnemyRosterLevel11`.
- **Spawn Compositions:** a squad (`EnemyComposition`, `(roster << 16) | (family - 1 << 12) | row`). It holds its groups'
  counts and emission times, its weight, cost multiplier and post-schedule duration, and the tech levels and player
  counts it is drawn at.
- **Demolition Templates:** the force a destruction stage requires (`DemolitionTemplate`, a row by its index, 1 to 9;
  "none" and "Count" are not offered), 1 to 255, shared by every stage that names it. The templates are 16-byte rows at
  `[[game.dll + 0x346D928] + 0x30]` (HD2Runtime's destructionGlobal); all eleven must hold their index and the bytes
  HD2Runtime read before anything is written, and edits wait until the destruction system exists. Untested in game.

Every write, live or from Local Stats at boot (`StatsDirectorReady`), is checked against the row as it is: a squad's
groups total at most 32 (the AI squad queue's capacity; a 33rd entry crashed the game in HD2Runtime's research), a group
with no roster key takes no count, the lowest tech level and player count may not exceed the highest, tech levels go to
11 only with level 11, and weights are finite. The director reads these tables when a mission is set up, so a change
applies from the next mission. Only the host's tables decide what spawns. None of it is proven in game yet (HD2Runtime's
director research is offline). Demolition templates and destruction models are not here yet.

Every change and reset is staged until APPLY (F9), so DISCARD takes any of them back. Once applied, a change goes
through `set_live` and into Local Stats, so it comes back at every boot.

Reference rows (HD2R Editor's payload swaps) open a picker instead, filtered by typing:

- a weapon's projectile: its own first, then every donor by what fires it;
- a projectile's impact and expiry payloads: its own, none, then the 241 catalogued explosions in the HD2R Editor's
  groups;
- status slots: empty, then the 35 statuses a damage type can apply.

While a weapon fires a donor, its own round's rows are hidden (shown LOCKED when they hold a change). While a slot sets
off another explosion, its own explosion's rows are hidden, and the chosen explosion's rows show under the slot where
the editor has them (edits land on their own item). Status slots fill in order (IN ORDER until the slot before holds a
status; a slot with later ones filled is not emptied) and are applied in that order.

Presets live in the PRESETS tab (F6 or Shift+Tab): HD2R Editor presets (`.hd2rpreset.json`, its format 1) in the
editor's `presets` folder. Every catalogue row carries HD2R Editor's row keys on its bytes (`mappings/hd2r-keys.json`,
rebuilt from HD2Runtime's tables by `scripts/ExportHd2rKeys.py`). IMPORT stages each value HellForge has a row for
(swaps first; a weapon's own round is left out while the preset swaps it; values are checked as typed ones are), and
skips the rest with a count per family. EXPORT writes the changes saved in Local Stats (and the armor perks) in the same
format, with references as HD2R Editor stores them (status ids, `{ref:"explosion"}`, `{ref:"output"}`, `"unlimited"`).

The Armor perks category edits each perk's values (`native/mod-loader/perks.c`), the Boosters category every
booster's fields, and two Advanced categories (Projectiles, Damage Types) reach rows by type id.

### The live pipe

The live pipe is `native/mod-loader/live.c` on the game's side and `src/HellForge.Core/LiveEdit.cs` on the backend's.
The local backend serves `\\.\pipe\hellforge-live-<run id>` (current user only) for co-op (its `StatsShare`, with
`src/HellForge.Core/StatsSync.cs`: a joiner of a host who shares its stats plays with them; and its `AssetShare`: the
lobby's packages, `packages` up and `preload` down), and the loader connects once the entity data has loaded.

Requests name a field as the stats schema does (kind, component, resource, record, offset, storage), with `set`,
`reset` and `get`. Each becomes a `set_live` edit of the Local Stats mod (`zz.local.stats`), so conflicts with other
mods are reported as for any edit, and the editor's own saved file is not a conflict. The game's hello carries its build
and stats schema; nothing is sent to a game running another build or schema. Entity deltas and status multipliers go
over the pipe too (written where the stats pass writes them).

### Attachments and status multipliers

`set_stat(mod, &stat, value, flags)` (SDK 9) changes, in the running game, the fields `set_live` cannot reach and
`get_stat` reads: an attachment's or a default magazine's entity delta, and a booster's status multiplier. It writes
them in the place a stats file writes them at boot. `value` NULL puts back the value from before this session's first
live change, or with `HF_LIVE_BASE` the field's base (another mod's value, else the game's). It returns
`HF_APPLIES_RESPAWN`: the game reads an attachment's delta when it builds the weapon, so a change is expected from the
next respawn or re-equip (not yet confirmed in game). The in-game editor applies its attachment and magazine changes
this way, and saves them, so they hold from every boot.

### Hellpod payloads

HD2R Editor's pod rows are here too. A support weapon's, backpack's, expendable's or Resupply's call-in page has a
Hellpod Payload category. It sets what the pod opens with in slots 1 to 4 (`HellpodRack.slot0_item` .. `slot3_item`, a
`"pickup"` reference: the slot's vanilla item, nothing, or a pickup HD2Runtime reviewed, by kind: support weapons,
backpacks, ammo, stims, grenades, supplies; `mappings/references.json` `pickups`, `podRacks`) and how many of them spawn
(`spawn_count`, 1 to 4).

The loader writes a pickup once its package is resident, and only over the slot's vanilla item or a pickup it put there;
a mod's own racks are not checked. Pods called in after the change carry it. A rack several call-ins share (the EAT-17's
and the Surplus EAT booster's) changes for all of them. The editor keeps the choices with the arm swaps, so they come
back at every boot and follow the host in co-op. This is untested in game except for the pairs HD2Runtime proved (a
Grenade Box in the Resupply and MG-43 pods, an EAT-700 in the Stalwart's slot 2), which the picker marks.

### Whole-row copies

`copy_row(mod, table, row, from, offset, size)` (SDK 9) replaces a settings row's bytes live with another row's of the
same table. It works for the rows `mappings/references.json` `rowCopies` names (HD2R Editor's G-109 Urchin pulse
explosion: one of three explosions' settings over its own), from the donors listed there, once the donor's packages are
resident. `from` 0 puts the row's own bytes back. The in-game editor keeps the choice with the arm swaps.

### Displayed armor penetration

The armory's displayed armor penetration is a choice row after a weapon's trait slots (as in HD2R Editor): None or an
armor penetration trait, written into the slot that holds one, else into the first empty slot. It changes only what the
armory shows; the weapon's damage decides its real armor penetration.

### Co-op

Every player keeps their own Local Stats, and the editor stays editable when joined; every machine loads the packages
the others' changes need, and fires each player's projectile swaps as theirs (`native/mod-loader/shots.c`; see
[multiplayer.md](multiplayer.md#shots-in-co-op)). A host may share its stats instead: then, over the live pipe, a joiner plays with the host's
Local Stats, including attachments, the in-game editor's mod content values, its arm swaps (Mounted Weapons) and hellpod
payloads, and its editor shows the host's changes read-only (`HfApi.stats_source`, SDK 15, says which). Armor perks
stay solo only. [multiplayer.md](multiplayer.md#stats-in-co-op) has the backend's side.

## Event sources

This section explains where the game events that mods subscribe to come from, and which machine raises each one in
co-op.

The sources are in `native/mod-loader/event_sources.c`, built from HD2Runtime 0.40.1's event research and game.dll's
code. The state and the players are read every game update. Damage, kills, stratagems and spawns come from hooks on the
game functions that do them (the damage consumer, the kill dispatch, the beacon activation dispatcher, the entity
spawner). A hook is installed only when some mod listens to its kind, and only if its first bytes read as reviewed for
the game build; otherwise `modloader.log` says so and that kind stays silent.

The SDK 11 sources are in `native/mod-loader/event_polls.c`. Heal, stats, weapon and bug hole events are read from the
game's own records (health, mission stats, wielders, destructibles) every update or a few times a second. The explosion
event hooks the game's explosion request, so every explosion is reported.

Each machine raises its own events. Damage events are the hits this machine's game applies (a target another machine
owns takes its damage there), and a stratagem's activation is raised on its thrower's machine. How the other kinds
behave on a joined client is still to be confirmed in a two-PC session.

Entity ids in events are the session's entity instances (the low 22 bits index them), not resource ids; the spawn event
pairs the two. The compendium's HMG sentry (`src/hmg_sentry.c`) logs its beacon, each of its sentries landing, and the
deployments per mission.

## Native edits

This section explains the manifest's native edits: widening network field types, and legacy component writes.

Native manifests support network field types and legacy component writes. Each edit is checked against this exact build
before it is written; anything that is not as reviewed is logged and left alone. The session runner
(`src/HellForge.Session/NativeModApplier.cs`) applies the network field types from beside the running game until it
exits, logging to `native-mods.log` and `native-mods.json` in the session folder. Component writes now use the stats loader pass above and are no
longer periodically restored.

The engine's loaded network configuration (`[[helldivers2.exe+0x1A10268]]`) lists its field types at `config+0x18`
(`config+0x10` entries of 0x18 bytes: name hash at +0, kind at +12, bits at +13, min at +16). The type is found by its
name hash (IdString32: upper half of MurmurHash64A, seed 0) and must be unique and exactly as expected. Only integer
types (kind 1) can be widened, to at most 31 bits. The engine validates every integer field written to the network
against `min .. min + 2^bits - 1` and clamps it in place (exe 0x34BAB0), so a wider type raises that limit for every
field of that type. All players must run the same edit, and the modlist digest makes sure of that.

The game keeps entity component records in one private allocation of a fixed size whose first 28 bytes are the entity
map header. HellForge finds it, and each component's index, exactly as HD2Runtime's `core/entity_catalog.lua` does,
using the profile `src/HellForge.Core/entity-profile.json` (exported from HD2Runtime's reviewed `schemas/current.lua` by
`scripts/ExportEntityProfile.py`, and pinned to the exe and game.dll SHA-256). The resource must own exactly one record
of the component. Shared records now produce a warning, as with stats data mods. Fields are u32 at a 4-byte aligned
offset inside the record, and their original value must equal `expect`. Resource ids and offsets come from HD2Runtime's
catalogs (`domains/entity_authoring.lua`, the `backing` of each field).

## Research behind the examples

This section explains what research two of the example mods rest on.

`examples/mods/machine-gun-10k` gives the M-1000 Maxigun's backpack 10,000 rounds. HD2Runtime's research
(`local_research/events/maxigun/deposit-limit-notes.md`) found why no Lua mod can: a backpack's live ammo is the network
field `remaining` of type `deposit_value` {int, 10 bits, min 0}, so the engine clamps it to 1,023 at the first shot after
any value above that. The mod widens `deposit_value` to 14 bits (up to 16,383) and sets the backpack's
`DepositComponentData` (record 23 of resource `0x056DE1C5E21E723E`) capacity and start amount to 10,000 and refill to
5,000. The HUD maximum is the start amount, so it shows 10,000.

`examples/mods/golden-liberator` is a gold-plated copy of the AR-23 Liberator as its own arsenal weapon. Its
`hellforge-data.json` describes the model: the Liberator's unit under the mod's own name, pointing to a copy of the body
material whose colour lookup texture is gold metal in every zone (every zone takes the bare-metal zone's values).
HellForge builds it from each player's game copy. `examples/mods/talon-liberator` does the same in blue and adds the
blue copies of the Talon's bolt and flight effects, everything they use, and the Sickle's sound banks. Both build byte
for byte what the research scripts `artifacts/registry/build_gold_liberator.py` and `build_talon_liberator.py` built.

## Where to go next

- [engine.md](engine.md): the engine's parts, building and tests.
- [sdk-roadmap.md](sdk-roadmap.md): what is planned for the SDK.
- [engine-roadmap.md](engine-roadmap.md): network isolation and co-op sync plans.
- [multiplayer.md](multiplayer.md): how co-op works.
- [sdk-reference.md](sdk-reference.md): every SDK call, component, table and constant.
