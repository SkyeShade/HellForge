# New enemies

<!-- diataxis: how-to -->

This page is for code mod authors who want the game to spawn an enemy of their own, or a vanilla one it no longer
spawns. You should know how to add an entity: see [new entities](content.md#new-entities).

`add_enemy` (SDK 18) makes the game spawn an entity as an enemy, beside the vanilla ones. No vanilla enemy is replaced.
You name a vanilla enemy it is `like`, and it spawns wherever that one does.

```c
HfEnemy alpha = {sizeof alpha};
alpha.name = "alpha_scavenger";
alpha.unit = scavenger_copy;       /* add_entity or register_entity's id, or a vanilla entity */
alpha.like = 0x9A8A3AAE287B230Cull; /* scavenger_tier_2: "resource" in mappings/entities.json */
alpha.weight = 0.5f;               /* half as likely as that one */
alpha.lowest_level = 6;            /* from difficulty 6 */
if (hf->size >= offsetof(HfApi, add_enemy) + sizeof hf->add_enemy) hf->add_enemy(mod, &alpha);
```

Call `add_enemy` from `hellforge_mod_init`. It returns 0 when the enemy is accepted and 1 when it is refused. The rows
are made when the entity data has loaded, and your mod's log then counts them or says why there are none.
`examples/mods/alpha-scavenger` is a whole mod.

## What spawns it

The game picks enemies from rosters, one per faction. A roster row names an enemy, the group it belongs to, the planets
it appears on and a spawn weight per difficulty. Squads draw from groups, by weight.

- HellForge copies every roster row of `like` for your unit. Your enemy joins the same groups, on the same planets,
  with the same cap and budget weight.
- `weight` multiplies the copied spawn weights (0 means 1). The vanilla enemy keeps its own, so the group spawns both.
- `lowest_level` and `highest_level` (1 to 10, 0 for no bound) set its weight to 0 outside them. Level 11 counts as
  level 10.
- `factions` picks the rosters: `HF_ENEMY_TERMINIDS`, `HF_ENEMY_AUTOMATONS`, `HF_ENEMY_ILLUMINATE`, or 0 for every
  faction roster that has a row of `like`.
- There is no set number of enemies. The game's rows keep their places, and yours follow them.

## Replacement rules

`add_enemy_swap` (SDK 18) adds a rule like the game's own modifier rules: while it is on, `replacement` spawns where
`unit` would. Use it for a variant that only appears under a modifier.

```c
HfEnemySwap predator = {sizeof predator};
predator.name = "alpha_predator";
predator.unit = scavenger_copy;     /* the enemy replaced: vanilla, or an add_enemy unit */
predator.replacement = predator_copy;
predator.tag = "GM_BugSuperPredators"; /* NULL: always on */
predator.priority = 5;
hf->add_enemy_swap(mod, &assault);
```

- `tag` is a director tag, a planet's or a mission's. The table below lists the game's.
- `lowest_level` and `highest_level` bound the difficulty (0: any).
- Of the rules that match a spawn, the highest `priority` wins. The game's rules use 0 to 9. A replacement can be
  replaced again, up to 5 times.
- `budget` is the replacement's weight in the enemy budget when it has no roster row. 0 takes the replaced enemy's.

| Faction | Tags |
|---|---|
| Terminids | `BugAcid`, `BugArmored`, `BugPredators`, `BugFlyers`, `BugFodder`, `BugCrawlers`, `BugBalanced`, `GM_BugSuperPredators`, `GM_BugGloom`, `GM_BugBurrowers`, `GM_BugDragon_Traveler`, `GM_BugShrieker_Traveler`, `GM_BugHiveLord` |
| Automatons | `BotAssault`, `BotPhalanx`, `BotArtillery`, `BotAir`, `BotPanzer`, `BotBalanced`, `GM_BotAssault`, `GM_BotCyborgs`, `GM_BotGunships_Traveler`, `GM_IvoryLegion` |
| Illuminate | `IlluminateStraggler`, `GM_IlluminateWarmachine_Traveler`, `GM_IlluminateEngineers`, `GM_IlluminateInvasion`, `GM_IlluminateHarvest`, `GM_IlluminateBodyHorror` |
| Any | `HordeOnly`, `GM_SEAF` |

## Limits and co-op

- The entity must exist when the entity data has loaded: a vanilla one, or one your mod or another added. An entry
  whose entity is missing is left out.
- A mission loads the packages of your entity's base. An entity that needs a package of its own must load it itself
  (see [data mods](data.md)).
- `like` must be an enemy of a faction roster. The rosters all factions share take no new rows.
- New squad mixes (compositions) are not part of this: your enemy joins existing groups.
- The game tracks at most 64 pairs of enemy type and spawn source alive at once. Each new enemy type alive adds
  pairs, so add few types to one faction.
- Only the host's rosters decide what spawns. Every player needs the mod, for the entity.
- When a live mod is taken out, its rows stop spawning and its rules stop matching.
