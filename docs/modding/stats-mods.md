# Stats mods

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
  A composition's group counts total at most 32, and these apply from the next mission, on the host.
- Fields go by their full names (`weapon.fire_rate`, `deposit.capacity`), or by short aliases where the target has the
  field: `rpm`, `magazine`, `fuse`, `cooldown`, `uses`, projectile `speed`, and damage `standard` and `durable`. Not
  every weapon has a magazine field, and not every throwable a fuse. Recoil is edited through its four input fields.
  `spread` sets both spreads, `armorPenetration` takes four values (direct, slight, large, extreme angle), and a
  stratagem's `uses: -1` is unlimited.
- References (a weapon's projectile, a projectile's impact or expiry explosion, a damage type's statuses) take type
  ids. `mappings/references.json` lists what each can be.
- Every field and its range is shown on its item's page in the in-game editor, and `check-stats` names any field it
  refuses and why. In the repository, the fields are in `src/HellForge.Core/stats-schema.json` and `mappings/*.json`.

To check a file without starting the game, run `HellForge.Cli check-stats --mod <folder>`. `examples/mods/stats-example`
is a working example.

## How it applies

- Changes apply when the game loads its data, every session. A stats mod is always a gameplay mod, so friends compare
  it.
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
and the joiner's editor shows them read-only ([multiplayer.md](../multiplayer.md#stats-in-co-op)).

Next: [Data mods](data.md), for models, textures, effects, sounds and icons.
