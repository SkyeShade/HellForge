# Effects, enemies and the armory

<!-- diataxis: how-to -->

Three SDK 21 calls let a code mod act on the world around the player. `play_effect` hangs particles on an entity, on
this machine or on every machine of the lobby. `find_enemies` lists the enemies near a point. `armory_weapon` names the
weapon the armory's customize screen shows. The types are in `hellforge_world.h`, which `hellforge_mod.h` includes.

## Contents

- [Before you call](#before-you-call)
- [An effect every player sees](#an-effect-every-player-sees)
- [Enemies near a point](#enemies-near-a-point)
- [The weapon in the armory](#the-weapon-in-the-armory)
- [Limits](#limits)

## Before you call

- The manifest needs `"sdk": 21`, and the code checks `hf->size` before each call, as with every newer call.
- `play_effect` and `stop_effect` run on the game thread only: in a game event, or in an `on_game_thread` callback
  ([the game thread](runtime.md#the-game-thread)). Elsewhere they are refused, and your log says so.
- `find_enemies` and `armory_weapon` may be called from any thread.

```c
#define HAS(hf, call) ((hf)->size >= offsetof(HfApi, call) + sizeof (hf)->call)
if (!HAS(hf, armory_weapon)) { hf->log(mod, "needs HellForge with SDK 21"); return 0; }
```

## An effect every player sees

A backpack that vents steam needs three things:

1. Particles that are loaded. Load their package with `load_package`, or put them in a session package of your mod's
   data ([packages](runtime.md#packages)). Unloaded particles are refused.
2. The entity to hang them on, such as the player's entity from `HfPlayerEvent`.
3. Which node of its unit they hang from. `""` is the unit's root. A node the unit lacks is refused.

```c
static uint32_t g_steam;

static void OnSpawn(HfMod* mod, const HfPlayerEvent* e, void* user)
{
    const HfApi* hf = HF_API(mod);
    if (!e->local || !e->entity) return;            /* only the machine of the player who wears it */
    HfEffect fx = {0};
    fx.size = sizeof fx;
    fx.flags = HF_EFFECT_SHARED;                     /* every machine plays it on its own copy of the player */
    fx.particles = hf->resource_hash("<particles>"); /* the name of your particles resource */
    fx.entity = e->entity;
    fx.node = "<node>";                              /* a node of the Helldiver's unit, "" for its root */
    fx.position[1] = 0.1f;                           /* 10 cm up from the node, in the node's own axes */
    g_steam = hf->play_effect(mod, &fx);             /* 0: refused, your log says why */
}
```

Stop it when the vent closes. `HF_EFFECT_STOP` lets the particles already out fade, and `HF_EFFECT_DESTROY` removes
them at once:

```c
if (g_steam) hf->stop_effect(mod, g_steam, HF_EFFECT_STOP);
```

A shared effect is played by one machine for everyone. So only the machine that owns the event should call it with
`HF_EFFECT_SHARED`, as above. If every machine called it, each player would see the effect twice. Without the flag
the effect is this machine's only.

An effect linked to an entity goes when its unit goes, and every effect ends when the game's state changes. Its
handle then no longer stops anything. With `entity` 0 the effect plays at `position` in the world.

## Enemies near a point

`find_enemies` fills an array with the living enemies inside a radius, nearest first, and answers how many it wrote:

```c
HfFoundEnemy near[16];
float centre[3] = {x, y, z};
uint32_t n = hf->find_enemies(mod, centre, 25.0f, near, 16);
for (uint32_t i = 0; i < n; i++)
    if (!(near[i].flags & HF_FOUND_DOWNED)) hf->log(mod, "%u at %.1f m, %d health", near[i].entity, near[i].distance, near[i].health);
```

| Field | Meaning |
| --- | --- |
| `entity`, `unit` | the enemy's entity id and its unit's handle |
| `resource` | its type, as `HF_ENTITY_...` or an entity a mod added |
| `position`, `distance` | its unit's root in the world, and how far it is from `centre` |
| `health`, `max_health` | its health pool now, and its full pool (-1 when that does not read) |
| `faction` | its faction mask. Bit 0 is the Helldivers' side |
| `flags` | `HF_FOUND_DOWNED`, and `HF_FOUND_AUTHORITY` when this machine simulates it |

An enemy is any entity whose faction is not the Helldivers' side. An entity with no faction counts when its type is
an enemy type. Only a mission has enemies; elsewhere the call answers 0. It reads every health record, so call it a
few times a second at most, not every frame.

## The weapon in the armory

`armory_weapon` gives the loadout id (`HF_ITEM_...`) of the weapon the armory works on. It reads two screens:

| `menu` | Screen | The id |
| --- | --- | --- |
| `HF_MENU_ARMORY` | the Armory's grid, with CUSTOMIZE and EQUIP | the item selected in it, the one CUSTOMIZE would open |
| `HF_MENU_CUSTOMIZE` | the customize screen of one weapon | the weapon it shows |

On any other menu, or with nothing selected, it answers 0. On the Armory's armour tabs the id is the armour piece's.
Its `menu` argument, which may be `NULL`, gets the menu state on top:

```c
uint32_t menu = 0, weapon = hf->armory_weapon(mod, &menu);
if (menu == HF_MENU_ARMORY && weapon == HF_ITEM_AR23_LIBERATOR) hf->set_status(mod, "Liberator selected");
```

Call it again whenever you need it: the screens change weapon without telling your mod. The Armory's answer follows a
click or the gamepad's selection, not the mouse hovering over an item.

## Limits

- 256 effects at once across all mods and players. A new one past that takes the oldest one's place in the table, and
  the oldest can then no longer be stopped.
- 64 shared effect messages a second from one machine. Past that, the effect plays on this machine only, and your log
  says so.
- A shared effect needs an entity the other machines know: a networked one, such as a player. An entity with no
  network id is refused.
- The other machines need the same particles loaded. Mod sync makes every player run the same mods.
- Only the reviewed game build (25480438). On another, these calls are refused and `modloader.log` says why.
