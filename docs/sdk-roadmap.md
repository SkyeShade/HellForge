# HellForge modding SDK: roadmap

For contributors: what is left in the mod SDK. [modding/README.md](modding/README.md) and
[sdk-reference.md](sdk-reference.md) document what exists, [engine-roadmap.md](engine-roadmap.md) is the engine's side,
and [roadmap.md](roadmap.md) is the overview.

Mod SDK 15, engine 0.1.37, HellForge 0.3.0-dev.9 (2026-10-24).

| Phase | Status |
|---|---|
| 1. Named fields and deferred edits | done (SDK 6) |
| 2. Identity and lookup | done (SDK 6) |
| 3. Live editing | done (SDK 6); in co-op each player keeps their own (SDK 15) or plays with a sharing host's; field timings to check in game |
| 4. MDK | done; a New mod button and a helper kit left |
| 5a. The in-game editor | mostly built; full HD2R Editor parity left |
| 5b. Events, hooks and registries | built in SDK 7 to 15; some events, registries and launcher views left |
| 6. Co-op sync for mod content | numbering and sync built; `is_authority` and the message channel left |

## Goal

Modding Helldivers 2 should feel like making a Minecraft Forge mod: create a mod from a template, register content by
name, change game data through named fields, build with one command, and see it in the launcher. The C SDK is the main
path. Stats files (`hellforge-stats.json`) hold value edits and are the editors' save format.

## Principles

1. Names over offsets. A mod never writes an offset; fields come from the mappings.
2. Deferred registration. Mods declare at init; the loader applies each edit when its data exists, and again when the
   game reloads it.
3. Stable identities. Mod content is `modid:name` and its ids derive from that, so every player's game agrees.
4. Compatible. Functions are only added at the end of `HfApi` and structs keep their `size` field, so older mods keep
   working.
5. Data-level. New content is real game data and registry entries, never per-shot or per-spawn substitutions.
6. Refuse loudly. A name that does not resolve, a field outside the reviewed layout or a failed guard is reported with
   the mod and line, never skipped.
7. Co-op by default. What a mod adds works in co-op with nothing done by the mod. The message channel (phase 6) is only
   for state the game has no place for.

## Mappings

`mappings/components.json` names every field the SDK can touch (offset, type, count, meaning, and the game build it was
reviewed on), and `mappings/catalog.json` names vanilla content. `HellForge.Cli sdk-headers` generates
`hellforge_components.h`, `hellforge_content.h`, the loader's `sdk_fields.h` and `sdk_content.h`, and
`sdk-reference.md` from them ([internals.md](internals.md#named-fields)).

Naming grows one entry at a time: an `_unknown_XX` range becomes a named field by adding its entry (offset, type,
meaning, build, evidence) and regenerating. Adding a name never breaks a mod, and a renamed field keeps its old name as a
deprecated alias for a while. A new game build revalidates every entry.

## What is left

### Live editing (phase 3)

- Check each field's `applies` timing (now, respawn or mission) in game. Most are inferred from code.

### MDK (phase 4)

- A New mod button on the Mods page. Today a mod starts from `HellForge.Cli new-mod` or a copy of
  `app/sdk/template-mod`.
- `hellforge_kit.h`: header-only helpers for common recipes (air support Pelicans, sentries, orbital strikes).

### The in-game editor (phase 5a)

Full parity with the HD2R Editor, in three steps:

1. The player's kit, mostly ported. Left: beam conversion, the HD2R rows with no HellForge field yet (a coverage count
   says which), and armor perks in co-op (solo only today).
2. Enemies and structures: stats, damage zones, attacks, spawn rosters and compositions. Every entity's page with the
   fields the engine maps (`mappings/entities.json`, `scripts/ExportEntities.py`; about 38,800 rows, every editable row of
   the HD2R Editor's Enemies pages but one, named by `scripts/ImportSchemaMappings.py <HD2Runtime>`, the Unit, AiSpawner,
   SquadTransportFormation, SquadTransport, Gore, StratagemBlocker and Bughole components included). Spawn rosters,
   compositions and the difficulty rows are done (`native/mod-loader/director.c`, `mappings/director.json`; level 11's
   weights too). Left: adding roster rows or compositions (their counts are in the game's code). Enemies are the host's
   in co-op, so their edits need no per-player handling.
3. Demolition templates, destruction models and mission settings.

### Events, hooks and registries (phase 5b)

- Events: player joined or left, objective completed, level generated.
- Registries: objectives, mission rows, operations, outposts and stamps for custom missions, and difficulty 11's own
  enemy weights ([roadmap.md](roadmap.md#difficulty-11-and-custom-missions)).
- Subfactions set by a mod per mission or planet, once the research finds what makes a subfaction active
  ([roadmap.md](roadmap.md#subfactions)). A subfaction is set up with the mission, never swapped in per spawn.
- Mod content in progression: a level requirement, a price or a warbond page for a mod's stratagems and weapons
  ([roadmap.md](roadmap.md#progression)).
- Game functions as SDK calls (spawn, give, damage, Wwise events) instead of raw addresses, which move with every game
  build.
- The launcher shows which mods hook what, and conflicts between mods. The logs and `hooks.tsv` already report both.
- Conflict rules beyond load order: a priority set by the mod or the player, and merge rules for list data.
- The Lua bridge's events and actions that have no engine call yet ([lua-bridge.md](lua-bridge.md)).

### Co-op sync (phase 6)

- `is_authority(entity)`: whether this machine is in charge of an entity. The spawn event's `authority` field already
  says so at spawn.
- A message channel between the same mod on every player's machine (`send`, and an event for what arrives), carried
  between the players' backends over the Steam link. HellForge's countdown laser and turret targeting use it first.
- The Pelican turning to face its target, and its custom aim height. Only the machine in charge sees them today.
- A two-backend test: a stratagem of each kind (entity, behaviour, ability) syncs.
- The channel in the co-op section of [modding/concepts.md](modding/concepts.md).

## Research gaps

Areas with no mappings yet: UI and text, audio (Wwise), progression and unlocks, armory customization, enemies and
spawns, missions and objectives, and missing-asset checks for new content.
