# Stats mods

<!-- diataxis: how-to -->

This page is for anyone who wants to change numbers (fire rate, magazine size, cooldowns, damage) without writing code.
It covers the `hellforge-stats.json` file, how its changes apply, and the in-game editor that writes the same format.

A stats mod is a `hellforge-stats.json` beside the mod's manifest. It changes fields by name, with the same checks the
SDK makes. The in-game editor (F8) writes the same format.

## The file

A stats mod needs two files in its folder. The manifest names the mod:

```json
{ "format": 1, "id": "yourname.liberator-tune", "name": "Liberator Tune", "version": "1.0.0" }
```

and `hellforge-stats.json` holds the changes:

```json
{
  "format": 1,
  "weapons": { "AR-23 Liberator": { "rpm": 900, "magazine": 60, "spread": 5 } },
  "throwables": { "G-12 High Explosive": { "fuse": 2.0 } },
  "projectiles": { "276": { "speed": 1200, "damage": 54 } },
  "damage": { "54": { "standard": 150, "durable": 40, "armorPenetration": [4, 4, 4, 4] } },
  "stratagems": { "Orbital Precision Strike": { "cooldown": 60, "uses": -1 } }
}
```

The file is organised by category, then by target, then by field:

- Categories: `weapons` (primary, secondary, support and mounted), `throwables`, `backpacks`, `vehicles`,
  `stratagems`, `helldiver`, `boosters`, `perks`, `projectiles`, `damage`, `explosions`, `statuses`, `enemies`,
  `difficulty`, `rosters`, `compositions`.
- Targets: entities by their exact armory names, and mounted weapons with their host and mount (`A/AC-8 Autocannon
  Sentry / weapon`). Projectiles and damage types go by their decimal type id, and boosters by name (`"Armed Resupply
  Pods"`). Enemies, structures and other entities go by the HD2R Editor's names (`"Bile Titan"`), with their fields
  named as its rows are: `"entity::entity.health"`, `"damage_zone:zone_3:zone.armor"`, `"attack:mapped_4:damage.stagger"`.
  Difficulties go by their level (`"7"`), roster rows and compositions by the HD2R Editor's keys
  (`"terminids/row_0"`: `"spawn.weight_3"`, `"spawn.weight_11"`; `"automatons/family_1/row_0"`: `"composition.group_0_count"`).
  A composition's group counts total at most 32, and these apply from the next mission, on the host. A roster weight
  of 0 that leaves an enemy with nothing to spawn at that level on some planet is refused (at boot, after a file's
  other lines).
- Explosions go by their editor label or their type (1..422). A projectile, damage type or explosion a mod added goes
  by the name it was added under, `"<mod id>:<name>"` (see [Mods' content by name](#mods-content-by-name)).
- Fields go by their full names (`weapon.fire_rate`, `deposit.capacity`), or by short aliases where the target has the
  field: `rpm`, `magazine`, `fuse`, `cooldown`, `uses`, projectile `speed`, and damage `standard` and `durable`. Not
  every weapon has a magazine field, and not every throwable a fuse. Recoil is edited through its four input fields.
  `spread` sets both spreads, `armorPenetration` takes four values (direct, slight, large, extreme angle), and a
  stratagem's `uses: -1` is unlimited.
- References take type ids: a projectile's `damage`, `terminal.impact.explosion` and `terminal.expiry.explosion`; a
  weapon's `attack.primary.projectile` and `terminal.<role>.impact.explosion`; a damage type's `status_<n>_type`. A
  mod's projectile, damage type or explosion is named instead (`"damage": "you.mod:plasma_burn"`).
  `mappings/references.json` lists what each can be.
- Every field and its range is shown on its item's page in the in-game editor, and `check-stats` names any field it
  refuses and why. In the repository, the fields are in `src/HellForge.Core/stats-schema.json` and `mappings/*.json`.

To check a file without starting the game, run `HellForge.Cli check-stats --mod <folder>`. `examples/mods/stats-example`
is a working example.

### Mods' content by name

A code mod's projectiles, damage types and explosions are numbered when the game starts, in the order of the mods' ids,
so their numbers change with the mods a player has. A stats file names them as the mod added them: the mod's id, a colon
and the name it gave (`HfProjectile.name` and so on). HellForge numbers them once every mod has set up, and refuses an
edit whose name no loaded mod added, with the reason in the mod's log.

```json
{
  "format": 1,
  "projectiles": { "you.mod:plasma_round": { "speed": 900, "damage": "you.mod:plasma_burn" } },
  "damage": { "you.mod:plasma_burn": { "standard": 150 } },
  "explosions": { "you.mod:plasma_blast": { "explosion.inner_radius": 4 } }
}
```

A mod's row has the same fields as the game's rows of its kind, but `own`. A mod's explosion has its own row's fields; its
damage type is a damage target of its own.

### Stratagem input codes

A stratagem's `stratagem.input_code` is its arrows: a string of `U`, `R`, `D` and `L` (or the arrows themselves), 1 to
10 of them, such as `"stratagems": { "Resupply": { "stratagem.input_code": "UULRD" } }`. Local Stats saves it as a
number, one base-5 digit per direction with the first lowest (Up, Right is 1 + 2 x 5 = 11), and both forms load.

- Ten is the most the game takes: the ship's arcade minigame has 10 arrows, its input matcher copies a code into a
  12-direction buffer, and the HUD's arrows go into a 16-direction one. HellForge checks those readers in game.dll and
  refuses every code edit on a build where they differ.
- The game reads the code each time it is asked for it, so a change applies at once, the HUD's arrows too. HellForge
  keeps every vanilla code in an array of its own and points the stratagem at it; taking the mod out puts the vanilla
  code back.
- The code is matched on each player's own machine. The beacon tells the others which stratagem was thrown, never its
  code, so in co-op a code edit only changes what its player presses.

### Rearms after uses

Every non-Eagle stratagem has its mission uses (`uses`, or `stratagem.max_uses`; -1 is unlimited) and a
`stratagem.rearms_after_uses` tick (0 or 1). Unticked, the game's own: a cooldown after every use, and spent uses are
gone for the rest of the mission. Ticked, the uses go one after another with no cooldown between them, though the game
still holds a stratagem while its strike runs. The cooldown starts once they are spent, and when it ends they come back.
Unlimited uses with the tick have no cooldown at all.

```json
{ "format": 1, "stratagems": { "Orbital Railcannon Strike": { "uses": 3, "stratagem.rearms_after_uses": 1 } } }
```

- The HUD's stratagem list shows the uses left, counting down and coming back, as it shows a limited stratagem's.
- Every machine runs every player's calls and refills every player's uses alike, so teammates' views agree when they
  play with the same tick. A mod with the tick is therefore shared: friends need the same copy. In the editor's Local
  Stats it is each player's own unless the host shares theirs.
- Eagles keep their rearm, and stratagems with a team cooldown keep the game's ways (the tick is refused on them).
  HellForge hooks the game's cooldown start (game.dll 0x135C2C0) and refuses the tick on a build where it differs.

### Spawning

The `difficulty`, `rosters` and `compositions` categories are the spawn director's tables (the editor's Spawning
pages, `mappings/director.json`). The host's game spawns the enemies, so only the host's values decide what spawns: a
joiner's edits change nothing in a host's mission.

- A composition's name is its family's role. Family 1 is patrols, 2 guard posts (empty in this build), 3 map guards
  and 4 reinforcements (breaches and drops). Family 5 is travellers (Shriekers, Gunships, War Striders), 6 stragglers
  (they top up the map's guards), 7 convoys (Factory Striders) and 8 air support (the Illuminate's attack ships).
- `composition.group_<n>_key` is the enemy family a group draws from, a number (the editor lists them by name). It
  takes a family of the composition's own faction's roster, or 0 for a group with a count of 0. A family with no spawn
  weight at the mission's difficulty spawns nothing. Give an empty group a family and a count to add enemies.
- What summoning enemies call up is on their pages in `enemies`: `"Impaler"` and `"warrior_big_tier2"` (the Alpha
  Commander) have `summon.family` and `summon.count` (0 to 16 a call; the Alpha Commander calls three times a summon).
  The game keeps them in its abilities' code, so HellForge writes them there. It checks that code in game.dll and
  refuses these edits on a build where it differs.
- A difficulty has `difficulty.patrol_cap_<faction>` (0 to 1000), `difficulty.traveller_interval_<faction>` and
  `difficulty.traveller_cap_<faction>`, and the stragglers' `difficulty.straggler_interval_<faction>`,
  `difficulty.straggler_batch_<faction>` (up to 32, one squad) and `difficulty.straggler_cap_<faction>` (map guards and
  stragglers alive together). The faction is `terminids`, `automatons` or `illuminate`.

```json
{ "format": 1, "enemies": { "Impaler": { "summon.count": 5 } }, "difficulty": { "7": { "difficulty.patrol_cap_terminids": 200 } } }
```

## How it applies

- Changes apply when the game loads its data, every session. A stats-only mod (no code, Lua, data patches, data spec,
  network field types or componentWrites) is a per-player gameplay mod. Friends do not compare it, and each player
  plays with their own. Adding any of those makes it shared, and then friends compare it.
- Some records are shared by several items. Changing a shared record changes it for every item that uses it, and the
  mod's log warns about it. `"own": true` on an entity's entry gives that entity its own copy of the record first
  (component fields only).
- Every write first checks the layout, the record's owner, and the value's type and range. A change that does not match
  is refused and logged in `mod-<id>.log`. Floats must be finite, and integers reject fractions and overflow. An unknown
  name, a blocked field or a duplicate refuses the whole mod.
- Mods apply in load order, the bottom one first. Where two change the same field, the later one wins, and its log and
  the Mods page report the conflict. The in-game editor's Local Stats is the last stats file. A code mod's deferred edit
  of the same field applies after every stats file, and live edits (the editor's among them) stack over it.
- `raw` covers a reviewed component field that has no name yet, with an explicit expected value:

  ```json
  { "format": 1, "raw": [ { "component": "DepositComponentData", "resource": "0x056DE1C5E21E723E",
      "offset": "0x0", "type": "u32", "expect": 1000, "value": 1500 } ] }
  ```

  A raw line has no reviewed range, so a mod with one does not download by itself through mod sync: it needs the
  verified list ([tools.md](tools.md#the-verified-list)). Named fields do.

## The in-game editor and Local Stats

The in-game editor (F8) edits every field live, showing its default and where its value comes from. APPLY (F9) puts the
changes into the game and saves them into the Local Stats mod (`mods/zz.local.stats/hellforge-stats.json`), so they come
back every session.

The PRESETS view imports and exports HD2R Editor presets (`.hd2rpreset.json`); F6 or Shift+Tab move between views.
EXPORT is the quickest way to share a set of changes with other players. To ship changes as a mod, copy Local Stats'
`hellforge-stats.json` into a mod of your own.

In co-op every player keeps their own Local Stats. A host may share theirs instead; then a joiner plays with the host's,
and the joiner's editor shows them read-only ([Co-op in game](../multiplayer-in-game.md#stats-in-co-op)).

Next: [Data mods](data.md), for models, textures, effects, sounds and icons.
