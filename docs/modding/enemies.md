# New enemies

<!-- diataxis: how-to -->

This page is for code mod authors who want the game to spawn an enemy of their own, or a vanilla one it no longer
spawns. You should know how to add an entity: see [new entities](content.md#new-entities).

Three calls (SDK 17) do it, and none ties your enemy to a vanilla one:

| Call | What it adds |
|---|---|
| `add_enemy` | a roster row: your entity, its spawn group, weights, cost and limits |
| `add_enemy_squad` | a squad: a mix of groups the game may send |
| `add_enemy_swap` | a replacement rule: one enemy spawns where another would |

Call them from `hellforge_mod_init`, after checking `hf->size` for the last one you use: older SDK 17 builds lack them.
Each returns 0 when accepted and 1 when refused. The rows are made when the entity data has loaded, and your mod's log
then says what each entry got, or why it got nothing. `examples/mods/alpha-scavenger` is a whole mod.

## How the game spawns enemies

- Each faction has a roster. A roster row names an enemy, its group, and a spawn weight per difficulty.
- A squad lists up to 8 groups with a count each. The game sends squads, and fills each count from the group's rows
  by weight.
- So an enemy reaches the map in one of two ways. In a vanilla group, every squad that draws the group may spawn it.
  In a group of your own, only your squads do.

## Add the enemy

```c
HfEnemy brute = {sizeof brute};
brute.name = "brute";
brute.unit = brute_entity;            /* add_entity or register_entity's id, or a vanilla entity */
brute.factions = HF_ENEMY_TERMINIDS;
brute.group = "my_name/my_mod/brutes"; /* a name of your own, or a vanilla group such as "Scavenger" */
for (int level = 4; level <= HF_ENEMY_LEVELS; level++) brute.weights[level - 1] = 1.0f;
brute.cost = 40.0f;
hf->add_enemy(mod, &brute);
```

| Field | Meaning |
|---|---|
| `factions` | the rosters it joins: `HF_ENEMY_TERMINIDS`, `HF_ENEMY_AUTOMATONS`, `HF_ENEMY_ILLUMINATE` |
| `group` | its spawn group's name, under 96 bytes |
| `weights` | its share of the group's draw at difficulty 1 to 10; 0 keeps it out of that level. Level 11 uses level 10's |
| `weight` | multiplies every weight (0 means 1) |
| `lowest_level`, `highest_level` | no weight outside them (0: no bound) |
| `cost` | what one costs a squad's budget, in the game's points: a Scavenger 6, a Warrior 15, a Charger 100 |
| `budget` | its weight in the count of living enemies (0 means 1; a Bile Titan has 8) |
| `alive_cap` | at most this many alive at once (0: no cap). In a squad, the group's first row decides the cap |
| `size_class` | 0 small, 1 medium, 2 heavy, 3 the largest, as the game marks its own rows |
| `with_tags`, `without_tags` | up to 4 [director tags](#director-tags) each: it spawns only while one of the first is on, never while one of the second is |
| `flags` | `HF_ENEMY_ALWAYS_LOADED`: a mission loads its packages though no squad names its group |
| `packages` | up to 2 packages the entity needs, as `load_package` takes them (0: none). See [What the entity needs](#what-the-entity-needs) |

A vanilla group keeps its own enemies: yours shares the draw with them. The game's groups are:

| Faction | Groups |
|---|---|
| Terminids | `Scavenger`, `Scav Spitter`, `Scav Predator`, `Warrior`, `Warrior Acid`, `Hunter`, `Brood Commander`, `Charger`, `Boomer`, `Strider`, `Stalker`, `Shrieker`, `Impaler`, `Prowler`, `Dragon`, `Hive Lord`, and 17 more |
| Automatons | `Conscript`, `Conscript Commander`, `Conscript MG`, `Conscript Jump`, `Conscript RPG`, `Berserker`, `Soldier`, `Soldier MG`, `Soldier RPG`, `Scout Walker`, `Lieutenant`, `Tank`, `Gunship`, `Spawner`, `Assault Walker`, and 15 more |
| Illuminate | `corrupted`, `champion`, `champion_staff`, `champion_jet`, `champion_beamer`, `observer`, `tripod`, `engineer`, `exomech`, `attack_ship`, `warmachine`, and 10 more |

### Shorthand: beside a vanilla enemy

Set `like` to a vanilla enemy's resource id and leave `group` and the fields after it out. HellForge then copies every
roster row of that enemy for your unit, with `weight` and the level bounds applied. `factions` may be 0: every roster
`like` is in.

## Add a squad

```c
HfEnemySquad pack = {sizeof pack};
pack.name = "brute_pack";
pack.factions = HF_ENEMY_TERMINIDS;
pack.kind = HF_SQUAD_PATROL;
pack.members[0].group = "my_name/my_mod/brutes"; pack.members[0].count = 2;
pack.members[1].group = "Scavenger";             pack.members[1].count = 5;
pack.weight = 1.0f;
pack.lowest_level = 4;
hf->add_enemy_squad(mod, &pack);
```

| Kind | Where the game uses it |
|---|---|
| `HF_SQUAD_PATROL` | patrols roaming the map |
| `HF_SQUAD_OUTPOST` | the garrison of outposts and objectives; a member's count is a pool (the game uses 999) |
| `HF_SQUAD_ENCOUNTER` | reinforcements: bug breaches and dropships |
| `HF_SQUAD_BORDER` | flyers crossing the map's border |
| `HF_SQUAD_CONVOY` | convoys |
| `HF_SQUAD_AIR_SUPPORT` | air support (the game's Illuminate attack ships) |
| `HF_SQUAD_GUARD` | placed at a mission's guard positions when it starts, where the mission has any. The game ships no guard squad |
| `HF_SQUAD_STRAGGLER` | the small groups a mission sends now and then |

- A mission takes the squads whose levels, player counts and tags fit. Among those of a kind it draws by `weight`;
  the game's squads use 0.01 to 3.
- A squad whose group has no enemy with weight at the mission's level is left out of that mission.
- Each roster in `factions` must have an enemy in every group. If one lacks a group, the squad is not added there
  and your log says so.
- A squad holds 32 enemies at most, except an outpost's pool.
- `cost` multiplies the members' cost in the director's budget (0 means 1).
- `with_tags` holds up to 8 [director tags](#director-tags): the squad is sent only while one is on.
- An outpost squad with no tag, or with a tag that is on, is taken at every level and player count. The game's
  outposts work this way.
- A mission uses one straggler squad throughout. The game takes its first that fits. With mods' squads fitting too,
  HellForge draws one by `weight` among that first squad and the mods'.

## Replacement rules

`add_enemy_swap` adds a rule like the game's own modifier rules: while it is on, `replacement` spawns where `unit`
would. Use it for a variant that only appears under a modifier.

```c
HfEnemySwap predator = {sizeof predator};
predator.name = "brute_predator";
predator.unit = brute_entity;       /* the enemy replaced: any unit of a roster row */
predator.replacement = predator_entity;
predator.tag = "BugPredators";      /* NULL: always on */
predator.priority = 5;
hf->add_enemy_swap(mod, &predator);
```

- `lowest_level` and `highest_level` bound the difficulty (0: any).
- Of the rules that match a spawn, the highest `priority` wins. The game's rules use 0 to 9. Of equal priorities the
  earlier rule wins, and the game's rules come before yours.
- A replacement can be replaced again, 3 times in a row at most, and only by a rule whose priority is not lower.
  HellForge leaves out a rule that would make a longer chain, or a loop. Your mod's log says so.
- A rule applies in a faction's roster only when that roster has a row of `unit`.
- `budget` is the replacement's weight in the enemy budget when it has no roster row. 0 takes the replaced enemy's.
- `cost` multiplies the replaced enemy's cost in a squad's budget (0 means 1).

## Director tags

A planet or a mission turns tags on. Rows, squads and rules name them.

| Faction | Tags |
|---|---|
| Terminids | `BugAcid`, `BugArmored`, `BugPredators`, `BugFodder`, `BugCrawlers`, `BugBalanced`, `GM_BugSuperPredators`, `GM_BugGloom`, `GM_BugBurrowers`, `GM_BugDragon_Traveler`, `GM_BugShrieker_Traveler` |
| Automatons | `BotAssault`, `BotPhalanx`, `BotArtillery`, `BotAir`, `BotPanzer`, `BotBalanced`, `GM_BotAssault`, `GM_BotCyborgs`, `GM_IvoryLegion` |
| Illuminate | `GM_IlluminateWarmachine_Traveler`, `GM_IlluminateEngineers`, `GM_IlluminateInvasion`, `GM_IlluminateHarvest`, `GM_IlluminateBodyHorror` |
| Any | `HordeOnly`, `GM_SEAF` |

## What the entity needs

Any entity the game has can be an enemy's unit, one [built from parts](#an-entity-from-parts) with no base
included. HellForge answers the game's reads that a vanilla enemy's data would:

- **Packages.** The entity's resources must be loaded when it spawns, or the game crashes. A copy of a vanilla enemy
  shares its base's packages, which the mission loads for a roster row. For any other entity, name its packages in
  `packages`: HellForge loads them when the game starts and holds them for the session. An enemy whose package
  cannot be loaded is left out, and your log says why.
- **Outposts.** An enemy with no `Behavior` record takes no garrison place: an outpost squad of its group stays unused.
- **Dropships.** An enemy with no `SquadTransportFormation` record rides as a single-seat passenger.
- **Free spawns.** Some spawns need the enemy's navigation record. An entity without one is skipped by those.

## An entity from parts

`parts` (SDK 17) builds an entity component by component instead of copying one base. Each part names a component and
the entity whose record of it yours takes. Set `.size = sizeof(HfEntityDef)`.

```c
static const HfEntityPart bruteParts[] = {
    {"UnitComponentData", HF_ENTITY_CHARGER},
    {"HealthComponentData", HF_ENTITY_BILE_WARRIOR},
    {"BehaviorComponentData", HF_ENTITY_RUPTURE_WARRIOR},
    /* ... and every other component it needs */
    {0}
};
static const HfEntityDef brute = {.size = sizeof brute, .name = "brute", .parts = bruteParts};
```

- With no `base`, the entity has the parts and no other component. With a `base`, a part replaces the base's record
  of that component, or adds a component the base lacks.
- A part's record is shared with the entity it comes from. The block's changes, or `own_all`, give yours a record
  of its own, which you then edit freely.
- A part may come from an entity your mod registered earlier.
- Some components hold no records, and HellForge has no name for them. Name one by its number, as `"#52"`; its
  `from` is not read.
- The game sends some components' state to other players. Those components of your entity, taken together, must be
  the ones some entity of the game has: yours takes that entity's network type. When no entity matches, yours is not
  added, and your mod's log says so. Components that are not sent, such as `BehaviorComponentData`, are free.
- HellForge refuses a list the game would crash on: a component whose record the entity named lacks, or one of the
  four components that read `UnitComponentData`, `HealthComponentData` or `VehicleMotionComponentData` without them.
- Records still have to fit together. A record that names bones or nodes of another model misbehaves in ways
  HellForge cannot check, so take the records tied to a model from one entity, as [Model](content.md#model) lists them.
- An entity from parts is added when the game starts, not by a mod set up while it runs.

## Limits and co-op

- The entity must exist when the entity data has loaded. An entry whose entity is missing is left out.
- A mission's plan holds 96 kinds of patrol, 64 of encounter, 32 each of outpost, guard and straggler, and 8 each of
  border, convoy and air support. When more fit a mission, HellForge leaves an even spread out, the game's and the mods' alike, and logs it.
- The rosters all factions share take no new rows.
- The game's enemy budget holds 64 places for the kinds of enemy alive at once. An enemy type takes one place for
  each spawn source it is alive from. With the table full, a further kind still spawns but is not counted: its cap on
  living enemies does not apply. The log says when that happens.
- Enemies are added when the game starts. A mod set up while the game runs gets a refusal from all three calls.
- Only the host's rosters decide what spawns. Every player needs the mod, for the entity.
- When a live mod is taken out, its enemies, squads and rules leave the rosters once no mission is running.
