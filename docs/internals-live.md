# Stats, live editing and events

<!-- diataxis: explanation -->

This page is for people working on HellForge itself. It explains how stats files are written at load, how fields change
while the game runs, where the events mods subscribe to come from, and how the manifest's native edits work. The rest of
the mod machinery is in [internals.md](internals.md).

## Contents

- [The stats pass](#the-stats-pass)
- [Live editing](#live-editing), including [the in-game editor](#the-in-game-editor) and [the live pipe](#the-live-pipe)
- [Event sources](#event-sources)
- [Native edits](#native-edits)
- [Where to go next](#where-to-go-next)

## The stats pass

Stats files (`hellforge-stats.json`) are validated and written at load. This section covers the schema, the guards,
ownership, references, conflicts, raw edits, and what has and has not been tested.

### The schema

The embedded `src/HellForge.Core/stats-schema.json` lists the supported fields, their storage, ranges and sharing, and
blocked fields with refusal reasons. For the editor it also lists each target's armory slot, weapon class, host item and
stratagem family, and the catalogs' field order. The exporter reads literal catalogs without executing Lua:

```powershell
py scripts/ExportEntityProfile.py <HD2Runtime>/schemas/current.lua src/HellForge.Core/entity-profile.json
py scripts/ExportStatsSchema.py <HD2Runtime> src/HellForge.Core/stats-schema.json native/mod-loader/stats-layouts.h
```

The generated stats schema contains metadata, not game bytes or extracted default values. Before writing, a named edit
validates whichever of these applies to its target:

- the reviewed component header, stride, resource ownership and record index;
- the settings row identity and layout.

The schema fingerprint must match the supported game.dll profile (build 25480438). Floats must be finite, integer fields
reject fractions and overflow, and catalog ranges are enforced. Invalid names, blocked fields, duplicate properties and
duplicate physical fields within a stats file reject that mod.

### When edits run

Component edits run on the entity-loading thread after registry additions, before `HF_EVENT_ENTITIES_READY`, once for
each entity-data load. Projectile, damage and stratagem edits run once after their tables become available and registry
installation has finished. They use the moved table where one exists.

Explosion, arc, beam and status settings use uniquely identified retained allocations. These are validated against their
structural headers, row counts and relocated pointers. Default weapon magazine overrides use the attachment catalog's
reviewed entity-delta chain: attachment resource, settings index, component, member and data offset. Readiness is
polled for up to 20 seconds. Each validated allocation is edited once, and missing or ambiguous allocations refuse the
edit.

There are no per-frame, per-shot or periodic field rewrites. A settings reload later in the session does not reapply
stats. Every applied edit and runtime refusal goes to `mod-<id>.log`, and missing load-time machinery refuses the edit.

### Shared records and ownership

Shared records affect all consumers and produce a warning in that log. Add `"own": true` to an entity's entry to clone
its component record through the registry before editing. All fields in that entry must then be component-backed.
`"own": true` uses `OwnRecord` for the item's component fields only. Projectile, damage and explosion definitions stay
shared.

Settings and attachment-delta ownership are rejected, because isolating these shared definitions requires reviewed
relinking of their consumers. A magazine edit affects weapons using that default attachment, and selecting a different
attachment can select different stats. Unverified gameplay effects are logged even when the structural checks pass.

### References in stats files

References take type ids (the editor's pickers name them, and `mappings/references.json` lists them):

- `attack.primary.projectile`: a weapon's projectile. For the Liberator, Dominator, Redeemer, Peacemaker, Diligence and
  Breaker it is `ammunition.projectile`. That is their default ammunition's entity delta, which every weapon equipping
  it shares and which applies from the next boot.
- `terminal.<role>.impact.explosion` and `terminal.<role>.expiry.explosion` (0 for none).
- `<damage>.status_<k>_type` with `<damage>.status_<k>_strength`, for k 1 to 4, on every damage type a target shows
  (`damage.`, `explosion.primary.impact.damage.`...). 0 is empty, and slots fill from slot 1.

Projectile pages take `terminal.impact.explosion` / `terminal.expiry.explosion`, and damage pages `status_<k>_type`.
The loader writes a reference once the packages it needs are loaded (see [References](#references) under Live editing).

### Conflicts

Mods apply in load order (the Mods page's bottom mod first). Conflicting stats and legacy component writes are reported
in `ModCatalog` problems, and both mods stay enabled. The later write wins if its guard passes. An expected value is
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
observes each field once per load, after its stats pass. It reports a field that a stats file changed with its value
from before that pass (its fingerprint covers addresses only). The schema contains no observed values.

### Tests

The fixture checks are:

- `dotnet run --project tests/HellForge.Tests`;
- `py -m unittest discover -s scripts -p test_export_stats_schema.py`;
- the engine's native stats test, which every engine build runs.

They use synthetic component and settings tables, including shared and owned records, guard refusals, conflict
ordering, moved damage tables, retained allocations and attachment-delta ownership chains.

Gameplay acceptance is a separate check in the isolated installation. It covers primary rate of fire and magazine,
secondary, grenade fuse or damage, support charge, backpack, stratagem cooldown and uses, vehicle ammunition, projectile
speed and damage. Fixture results establish resolution, guards and writes. They do not establish that every unverified
field affects gameplay. The current implementation has not yet passed this in-game category checklist.

## Live editing

Code mods (`set_live`), the in-game editor and co-op stats sync change fields in the running game. They use the same
path as deferred edits, with the same checks and conflict reports. This section covers the timing of each field, the
live-edit calls, the settings tables and passives they reach, the in-game editor built on them, and the pipe that
carries co-op stats.

### When a change applies

Every mapped field has an `applies` timing in `mappings/components.json`: a component default, overridden per field. It
shows as a `[tag]` in `hellforge_components.h`.

| Timing | Applies | Fields |
| --- | --- | --- |
| `now` | The next time the game reads it | Projectile, damage, explosion, arc, beam and status settings, the difficulty director's budgets and patrols, a stratagem's cooldown and call-in time (at its next call), an orbital's tracking |
| `respawn` | When the entity is next made: the next spawn, re-equip or call-in | Component records: weapons, backpacks, vehicles, sentries |
| `mission` | When the next mission loads | Packages, level generation, a stratagem's uses (the game copies them into each player's stratagem record at the hellpod launch) |

`appliesVerified` marks timings confirmed from game code. The rest are inferred from where the game reads the field,
and the header marks them with a "?" (`[respawn?]`). Every stats-schema field is a mapped field: the schema's fields
were imported into the mappings, and a test keeps them in step. So the live editor and the SDK check the same layout.

### set_live

`set_live(mod, target, field, value, flags)` returns `HF_APPLIES_NOW`, `_RESPAWN`, `_MISSION` or 0. A 0 means refused,
with the reason in the mod's log. The target is found by identity every time, never by address:

- an entity by its resource id;
- a settings table row by its id: a projectile type, a damage id, a stratagem type, a level, or an explosion, arc, beam
  or status row id.

One element of an array field is `HF_FIELD_AT(Component, field, index)`.

A mod's next live edit of the same field replaces its earlier one. `value` NULL undoes it, putting back the value from
before it. A record other entities share is changed only with `HF_LIVE_SHARED`, which changes all of them, as a stats
file does. Live edits stay for the session and are renewed if the game reloads its data. Nothing is saved.

`hf_live(mod, target, Component, field, HF_U32(60), 0)` is the macro form. Deferred `set_field` edits reach table rows
the same way and wait until the table has loaded.

### get_live

`get_live(mod, target, field, &out)` reads a field into an `HfLiveValue`, finding it as `set_live` does (a shared record
too). `now` is what the record holds. `base` is the field without the Local Stats mod's change and without live edits:
another mod's value (`out.mod` names it), else the game's own. It returns the field's timing, or 0 when it cannot be
read yet.

`set_live(..., NULL, HF_LIVE_BASE)` puts the base back, past a value Local Stats applied at load. This is how an editor
removes a change it saved.

### Boosters

Boosters are a settings table, `BoosterSettings`: game.dll's static booster definitions, a row per `HF_BOOSTER_...`
value. `tuning` (+0x08 f32) is the value its effect reads. `mappings/boosters.json` explains it per booster, with its
range: damage taken for Vitality Enhancement, stamina for Stamina Enhancement, and so on.

Boosters are also a stats category (`"boosters"` in hellforge-stats.json, keyed `booster.<value>`), so the in-game
editor edits and saves them. Rows are used once game.dll's image and all 21 row ids read as reviewed.

### Armor passives

`set_passives(mod, armor, second)` and `get_passives(mod, &state)` set the local player's armor passive and a second
passive on top (the helmet's slot). They take a passive id (`mappings/passives.json`), or `HF_PASSIVE_KIT` for what the
kits give. This works solo only: with other players the kits' values come back and the override waits.

The loader keeps it through armor changes (it looks every 250 ms) and only replaces the kits' value or its own. It acts
after proving the 59 code pins of HD2Runtime's research. Passives whose effect needs a package (INTEGRATED EXPLOSIVES,
ADRENO-DEFIBRILLATOR) are refused unless the armor gives them.

### References

References are HD2R Editor's payload swaps. Fields marked `"ref"` in the mappings name another table's row:

- `ProjectileWeapon.projectile`: the projectile a weapon fires;
- `ProjectileSettings.impact_explosion` and `expiry_explosion` (+0x90, +0x9C), 0 for none;
- `DamageInfo.status_1_type` .. `status_4_type` (+0x2C, 8 apart), each followed by its strength, with slots filling
  from 1;
- the damage of an explosion and of a projectile.

A new value must be a row the reference catalogue knows: a projectile type another weapon fires, a catalogued
explosion, or an attachable status effect. The catalogue is `mappings/references.json`, built from HD2Runtime's
reviewed catalogues with `scripts/ExportReferences.py`.

A value is written once the packages its effects need are resident. Until then the edit waits (logged), and it fails if
they do not load within 90 seconds. An explosion whose shrapnel leads back to the projectile it is set on is refused. A
status after an empty slot is written but logged, since the game stops at the first empty one. Stats files take the
same references, with the same waits.

### Packages

`load_package(mod, package)` loads a catalogue package through the game's own RefcountedPackageSystem, its request
checked byte for byte. It loads on the game thread, once per session, and holds the package for the session. At most 64
are held, and none is loaded when the game's reference map is 3/4 full. Residency is read from the engine's package list.

A mod's `load_package` is per client, and nothing it loads is advertised to other players. In co-op the loader also
tells the session's backend which packages it holds for this machine's own edits, mods and actions (the live pipe's
`packages` line). It loads the lobby's other packages when the backend asks (`preload`). A friend's packages may not
take the last 16 of the 64 ([Co-op in game](multiplayer-in-game.md#assets-in-co-op)).

### The game thread

`on_game_thread(mod, callback, user)` calls back every game update, on the thread the game's own engine calls come from
(sounds, package requests). The callback comes from the menu sound manager's update, which the loader hooks once its
bytes are checked.

### The in-game editor

The in-game editor (`native/editor`, F8; built by `scripts/Build-Editor.ps1`) is a code mod on these calls, with the
HD2R Editor's layout and look.

#### Its catalogue

The catalogue comes from `HellForge.Cli editor-data` (`EditorCatalog.cs`). It holds the primaries, secondaries,
throwables, stratagems and boosters with their stats fields. It also holds the armor perks: two choice rows with a
picker, kept in the editor's settings and set again at every boot.

The catalogue is an index, `editor-catalog.json` (the categories, the references and HD2R Editor's ids), and a file per
category, `editor-catalog-<category>.json`. The editor reads no file of 64 MB or more, and the whole catalogue in one
file was 48 MB. A test keeps every file under a quarter of the limit. The editor reads every file at start, because rows
on the same bytes are linked across categories (a damage type on a weapon's page and on the Damage Types page).

Its weapon and grenade silhouettes are the game's own HUD sprites, and its text is in the game's own FS Sinclair. Both
are extracted from the installation's game copy into the editor's folder (`game-icons.bin`, `fs-sinclair*.ttf`) and
never published. Its buttons play the game's menu sounds through the game's own UI sound path. That path is checked
byte for byte, and silent on another build.

#### HDR

With the game's HDR on, the editor draws in HDR too. The game then renders into a 10-bit back buffer in HDR10 (8-bit
without), and the editor's colours written as they are came out washed out.

When the back buffer is 10-bit and the game's display is in HDR (its DXGI colour space ST.2084), the editor's pixel
shader:

1. makes its sRGB colours linear;
2. sets them at the brightness Windows gives SDR content on that display ("SDR content brightness", or 200 nits when
   Windows cannot say);
3. moves them to Rec.2020 and PQ-encodes them.

With a 16-bit float back buffer it writes scRGB. A 10-bit back buffer on an SDR display stays SDR. ImGui's DX11 and DX12
renderers take that shader through a one-line change `Build-Editor.ps1` makes in a copy of each. The editor's log
(`mod-hellforge.editor.log`) says which it chose. `HELLFORGE_EDITOR_HDR=off|pq|scrgb[:nits]` decides instead (the tests
use it).

#### Enemies

The ENEMIES group is the HD2R Editor's Enemies pages: Terminids, Automatons, Illuminate, Structures and Other Entities.
These are the 1,170-odd entities HD2Runtime's enemy catalogue lists, from `mappings/entities.json`. That file comes from
`scripts/ExportEntities.py`, which reads HD2Runtime's `domains/enemy_authoring.lua` and `enemy_controls.lua` as
literals.

Each entity's page has Health & Armor, a group per damage zone, a group per attack, and its other sections (Spawning,
Director Multipliers, Shield, Size & Geometry...). It shows only the fields whose exact bytes the engine maps
(`mappings/components.json`). That is about 38,800 rows: every row the HD2R Editor lets you edit there but one, whose
authored value lies outside its own write range.

`scripts/ImportSchemaMappings.py <HD2Runtime>` names the fields of components the mappings have (each damage zone's,
the HiveMind's director multipliers). It also adds the components the entity profile locates
(`src/HellForge.Core/entity-profile.json`): Unit, AiSpawner, SquadTransportFormation, SquadTransport, Gore,
StratagemBlocker, Bughole. `HellForge.Cli sdk-headers` then regenerates the engine's tables, and `ExportEntities.py`
offers them.

The profile's entries for those come from the entity file's own frames (`scripts/ExportEntityProfile.py` EXTRA). Each
frame is found by the component index the u32 before it holds, as the game's loader 0xFDB440 reads it. The method
reproduces every other entry. With a third path, the exporter writes the rest by entity and reason: rows the HD2R Editor
only shows.

Rows are keyed as the HD2R Editor's are (`en|<entity>|<path>:<zone or attack>:<field>`), so its presets import and
export them. A stats file saves them under `"enemies"`. The Helldiver's own entity adds its main fields (health, armor,
constitution, regeneration) to the Helldiver page. The MS-11 Solo Silo's rack and missile are parts of its stratagem's
page (`StatsSchema.EntityParts`). Each leaves out bytes the page shows already.

Only an enemy class's main health and zone armor are proven in game (HD2Runtime's evidence). The rest are marked
unverified.

#### Spawning

The SPAWNING group is the HD2R Editor's Mission Difficulty, Spawn Rosters and Spawn Compositions. It comes from
`mappings/director.json` (`scripts/ExportDirector.py`, from HD2Runtime's `domains/enemy_director.lua`), and
`native/mod-loader/director.c` writes it:

- **Mission Difficulty:** a level's `DifficultySettings` row (1 to 10, and 11 with a mod that adds it). It holds the
  HD2R Editor's three budgets, plus the objectives and per-faction patrol interval and cap. Without level 11 the game's
  own table is written: the copy HD2Runtime edits, and every other of its 19 copies that held the same bytes.
- **Spawn Rosters:** an enemy's spawn weight at each difficulty (`EnemyRoster`, a row by `(roster << 16) | row` in
  `difficulty.c`'s roster order). The weights sit at +0x30, or at +0x2C once level 11 gave every row an 11th weight.
  That 11th weight is then `EnemyRosterLevel11`.
- **Spawn Compositions:** a squad (`EnemyComposition`, `(roster << 16) | (family - 1 << 12) | row`). It holds its
  groups' counts and emission times, its weight, cost multiplier and post-schedule duration, and the tech levels and
  player counts it is drawn at.
- **Demolition Templates:** the force a destruction stage requires (`DemolitionTemplate`, a row by its index, 1 to 9;
  "none" and "Count" are not offered), 1 to 255, shared by every stage that names it. The templates are 16-byte rows at
  `[[game.dll + 0x346D928] + 0x30]` (HD2Runtime's destructionGlobal). All eleven must hold their index and the bytes
  HD2Runtime read before anything is written, and edits wait until the destruction system exists. Untested in game.

Every write, live or from Local Stats at boot (`StatsDirectorReady`), is checked against the row as it is:

- a squad's groups total at most 32, the AI squad queue's capacity (a 33rd entry crashed the game in HD2Runtime's
  research);
- a group with no roster key takes no count;
- the lowest tech level and player count may not exceed the highest;
- tech levels go to 11 only with level 11;
- weights are finite.

The director reads these tables when a mission is set up, so a change applies from the next mission. Only the host's
tables decide what spawns. None of it is proven in game yet (HD2Runtime's director research is offline). Demolition
templates and destruction models are not here yet.

#### Applying, pickers and presets

Every change and reset is staged until APPLY (F9), so DISCARD takes any of them back. Once applied, a change goes
through `set_live` and into Local Stats, so it comes back at every boot.

Reference rows (HD2R Editor's payload swaps) open a picker instead, filtered by typing:

- a weapon's projectile: its own first, then every donor by what fires it;
- a projectile's impact and expiry payloads: its own, none, then the 241 catalogued explosions in the HD2R Editor's
  groups;
- status slots: empty, then the 35 statuses a damage type can apply.

While a weapon fires a donor, its own round's rows are hidden (shown LOCKED when they hold a change). While a slot sets
off another explosion, its own explosion's rows are hidden. The chosen explosion's rows show under the slot where the
editor has them, and edits land on their own item. Status slots fill in order and are applied in that order. A slot
shows IN ORDER until the slot before holds a status, and a slot with later ones filled is not emptied.

Presets live in the PRESETS tab (F6 or Shift+Tab): HD2R Editor presets (`.hd2rpreset.json`, its format 1) in the
editor's `presets` folder. Every catalogue row carries HD2R Editor's row keys on its bytes (`mappings/hd2r-keys.json`,
rebuilt from HD2Runtime's tables by `scripts/ExportHd2rKeys.py`).

- IMPORT stages each value HellForge has a row for, and skips the rest with a count per family. Swaps come first, and a
  weapon's own round is left out while the preset swaps it. Values are checked as typed ones are.
- EXPORT writes the changes saved in Local Stats, and the armor perks, in the same format. References are written as
  HD2R Editor stores them (status ids, `{ref:"explosion"}`, `{ref:"output"}`, `"unlimited"`).

The Armor perks category edits each perk's values (`native/mod-loader/perks.c`), and the Boosters category every
booster's fields. Two Advanced categories (Projectiles, Damage Types) reach rows by type id.

### The live pipe

The live pipe is `native/mod-loader/live.c` on the game's side and `src/HellForge.Core/LiveEdit.cs` on the backend's.
The local backend serves `\\.\pipe\hellforge-live-<run id>` (current user only) for co-op, and the loader connects once
the entity data has loaded. Two parts of the backend use it:

- `StatsShare`, with `src/HellForge.Core/StatsSync.cs`: a joiner of a host who shares its stats plays with them;
- `AssetShare`: the lobby's packages, `packages` up and `preload` down.

Requests name a field as the stats schema does (kind, component, resource, record, offset, storage), with `set`,
`reset` and `get`. Each becomes a `set_live` edit of the Local Stats mod (`zz.local.stats`). So conflicts with other
mods are reported as for any edit, and the editor's own saved file is not a conflict. The game's hello carries its build
and stats schema, and nothing is sent to a game running another build or schema. Entity deltas and status multipliers
go over the pipe too, written where the stats pass writes them.

### Attachments and status multipliers

`set_stat(mod, &stat, value, flags)` (SDK 9) changes, in the running game, the fields `set_live` cannot reach and
`get_stat` reads. These are an attachment's or a default magazine's entity delta, and a booster's status multiplier. It
writes them in the place a stats file writes them at boot.

`value` NULL puts back the value from before this session's first live change. With `HF_LIVE_BASE` it puts back the
field's base: another mod's value, else the game's. It returns `HF_APPLIES_RESPAWN`. The game reads an attachment's
delta when it builds the weapon, so a change is expected from the next respawn or re-equip (not yet confirmed in game).
The in-game editor applies its attachment and magazine changes this way, and saves them, so they hold from every boot.

### Hellpod payloads

HD2R Editor's pod rows are here too. A support weapon's, backpack's, expendable's or Resupply's call-in page has a
Hellpod Payload category. It sets what the pod opens with in slots 1 to 4, and how many of them spawn (`spawn_count`,
1 to 4).

Each slot is `HellpodRack.slot0_item` .. `slot3_item`, a `"pickup"` reference. It takes the slot's vanilla item,
nothing, or a pickup HD2Runtime reviewed. The pickups are grouped by kind: support weapons, backpacks, ammo, stims,
grenades, supplies (`mappings/references.json` `pickups`, `podRacks`).

The loader writes a pickup once its package is resident, and only over the slot's vanilla item or a pickup it put
there. A mod's own racks are not checked. Pods called in after the change carry it. A rack several call-ins share (the
EAT-17's and the Surplus EAT booster's) changes for all of them. The editor keeps the choices with the arm swaps, so
they come back at every boot and follow the host in co-op.

This is untested in game except for the pairs HD2Runtime proved, which the picker marks: a Grenade Box in the Resupply
and MG-43 pods, and an EAT-700 in the Stalwart's slot 2.

### Whole-row copies

`copy_row(mod, table, row, from, offset, size)` (SDK 9) replaces a settings row's bytes live with another row's of the
same table. It works for the rows `mappings/references.json` `rowCopies` names, from the donors listed there, once the
donor's packages are resident. One such row is HD2R Editor's G-109 Urchin pulse explosion: one of three explosions'
settings over its own. `from` 0 puts the row's own bytes back. The in-game editor keeps the choice with the arm swaps.

### Displayed armor penetration

The armory's displayed armor penetration is a choice row after a weapon's trait slots (as in HD2R Editor). It takes
None or an armor penetration trait, written into the slot that holds one, else into the first empty slot. It changes
only what the armory shows: the weapon's damage decides its real armor penetration.

### Co-op

Every player keeps their own Local Stats, and the editor stays editable when joined. Every machine loads the packages
the others' changes need, and fires each player's projectile swaps as theirs (`native/mod-loader/shots.c`, see
[Co-op in game](multiplayer-in-game.md#shots-in-co-op)).

A host may share its stats instead. Then, over the live pipe, a joiner plays with the host's Local Stats. That includes
attachments, the in-game editor's mod content values, its arm swaps (Mounted Weapons) and hellpod payloads. The
joiner's editor shows the host's changes read-only (`HfApi.stats_source`, SDK 15, says which). Armor perks stay solo
only. [Co-op in game](multiplayer-in-game.md#stats-in-co-op) has the backend's side.

## Event sources

Each machine raises its own events. This section explains where the events mods subscribe to come from, and which
machine raises each one in co-op.

The sources are in `native/mod-loader/event_sources.c`, built from HD2Runtime 0.40.1's event research and game.dll's
code. The state and the players are read every game update. Damage, kills, stratagems and spawns come from hooks on the
game functions that do them: the damage consumer, the kill dispatch, the beacon activation dispatcher, the entity
spawner. A hook is installed only when some mod listens to its kind, and only if its first bytes read as reviewed for
the game build. Otherwise `modloader.log` says so and that kind stays silent.

The SDK 11 sources are in `native/mod-loader/event_polls.c`. Heal, stats, weapon and bug hole events are read from the
game's own records (health, mission stats, wielders, destructibles) every update or a few times a second. The explosion
event hooks the game's explosion request, so every explosion is reported.

Damage events are the hits this machine's game applies: a target another machine owns takes its damage there. A
stratagem's activation is raised on its thrower's machine. How the other kinds behave on a joined client is still to be
confirmed in a two-PC session.

Entity ids in events are the session's entity instances (the low 22 bits index them), not resource ids. The spawn event
pairs the two. The compendium's HMG sentry (`src/hmg_sentry.c`) logs its beacon, each of its sentries landing, and the
deployments per mission.

## Native edits

Native manifests support two edits: network field types and legacy component writes. Each edit is checked against this
exact build before it is written, and anything that is not as reviewed is logged and left alone.

The session runner (`src/HellForge.Session/NativeModApplier.cs`) applies the network field types from beside the
running game until it exits. It logs to `native-mods.log` and `native-mods.json` in the session folder. Component writes
now use [the stats pass](#the-stats-pass) and are no longer periodically restored.

The engine's loaded network configuration (`[[helldivers2.exe+0x1A10268]]`) lists its field types at `config+0x18`.
There are `config+0x10` entries of 0x18 bytes: name hash at +0, kind at +12, bits at +13, min at +16. The type is found
by its name hash (IdString32: upper half of MurmurHash64A, seed 0) and must be unique and exactly as expected. Only
integer types (kind 1) can be widened, to at most 31 bits.

The engine validates every integer field written to the network against `min .. min + 2^bits - 1` and clamps it in
place (exe 0x34BAB0). So a wider type raises that limit for every field of that type. All players must run the same
edit, and the modlist digest makes sure of that.

The game keeps entity component records in one private allocation of a fixed size whose first 28 bytes are the entity
map header. HellForge finds it, and each component's index, exactly as HD2Runtime's `core/entity_catalog.lua` does. It
uses the profile `src/HellForge.Core/entity-profile.json`, exported from HD2Runtime's reviewed `schemas/current.lua` by
`scripts/ExportEntityProfile.py` and pinned to the exe and game.dll SHA-256.

The resource must own exactly one record of the component. Shared records now produce a warning, as with stats data
mods. Fields are u32 at a 4-byte aligned offset inside the record, and their original value must equal `expect`.
Resource ids and offsets come from HD2Runtime's catalogs (`domains/entity_authoring.lua`, the `backing` of each field).

## Where to go next

- [internals.md](internals.md): the rest of how HellForge implements mods.
- [internals-difficulty.md](internals-difficulty.md): how the Difficulty 11 level is added.
- [multiplayer.md](multiplayer.md): how co-op works, including stats and assets in co-op.
- [sdk-reference.md](sdk-reference.md): every SDK call, component, table and constant.
