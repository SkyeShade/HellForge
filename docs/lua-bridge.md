# Lua mods and the Lua bridge

This page is for players who want to run Lua mods on HellForge, and for authors of Lua mods and runtimes who want to
know how their changes reach the game. It covers adding Lua mods, what HD2Runtime does on HellForge, which edit wins a
field, and the C interface the bridge exports.

HellForge loads Lua mods: Bingus Shared Loader, HD2Runtime and the mods made for them. They are the player's own mods.
They run on this player's game only and are not part of the modlist code, so friends do not need the same ones to play
together. HellForge's code mods stay the layer that is the same for everyone in co-op (see the
[modding guide](modding/README.md)).

On HellForge, HD2Runtime does not write to the game itself. It finds HellForge's Lua bridge in the game and asks
HellForge to make each change. A Lua mod's change is then a HellForge live edit, like the F8 editor's: the editor shows
it with the mod's name, and HellForge keeps it when the game reloads its data. HD2Runtime's API stays the same, so a mod
made for HD2Runtime runs unchanged.

## For players

1. Add Bingus Shared Loader and HD2Runtime (a release with the HellForge bridge) on the Mods page, like any other mod.
   HD2Runtime needs Bingus Shared Loader, and HellForge ships neither. HD2Runtime's file is
   `HD2Runtime-<version>-runtime.zip`. `HD2Runtime-ModTemplate-<version>.zip` is a starter for writing HD2Runtime mods,
   not a mod, and the Mods page says so if you add it.
2. Add the Lua mods you want.

HD2R Editor, HD2Runtime's own in-game editor, is not supported on HellForge: use HellForge's F8 editor, which edits the
same fields and shows what each Lua mod changed. If you add HD2R Editor, the Mods page marks it UNSUPPORTED and keeps it
switched off (it would take F8 too, build HD2Runtime's whole catalogue while the game loads, and replay its own saved
changes over HellForge's). HD2Runtime and the other Lua mods made for it are not affected.

Lua mods are listed in the cosmetic group with a LUA tag, and they are staged like any game patch. The F8 editor shows
a value a Lua mod changed with the mod's name, `lua.<mod>`. Your own F8 changes win over a Lua mod's, and a Lua mod's
win over a code mod's.

## What HD2Runtime does on HellForge

| HD2Runtime | On HellForge |
|---|---|
| `hd2.patch`, `transaction`, `plan`, `ensure` | HellForge live edits of the same fields (any single value of a component record or a settings row), its `set_stat` (attachments, mission timers, status multipliers), `copy_row` (the G-109 Urchin's pulse) and stratagem codes |
| `hd2.read`, `observe`, `inspect`, the catalogues, `hd2.store`, options, timers, input | HD2Runtime's own, as before (they change nothing) |
| UI overlays, the F10 panel, sounds, game text | HD2Runtime's own, as before |
| Every event (mission, player, spawn, kill, damage, heal, stats, weapons, explosions, bug holes) | From HellForge's event hooks and readers |
| `hd2.player_passives.set` | HellForge's armor passive override |
| Gameplay actions, explosions and projectiles, homing, per-shot changes, the Pelican, kill credit, the timer freeze | HellForge's engine actions (SDK 11) |
| Custom stratagems | Unavailable for good: custom stratagems are HellForge stratagem mods |
| Enemy spawn weights and the director, enemy fields | Unavailable: HellForge has not mapped enemies yet |

An unavailable call answers `HELLFORGE_UNAVAILABLE` (a rejected handle, or nil, a code and a reason) and logs it once,
so a mod that uses one keeps running. `hd2.hellforge` says whether HD2Runtime runs on HellForge and lists what is
unavailable. HD2Runtime's own lobby sync (synced assets, beam conversions, the mission timer) and its public matchmaking
check stay off, because the session is HellForge's.

Every field HD2Runtime's catalogues let mods write has a route on HellForge (see
[how HD2Runtime's fields map](#how-hd2runtimes-fields-map)). A mod's own icon image for a stratagem answers
`HELLFORGE_UNSUPPORTED`, because HellForge does not load a Lua mod's images yet.

In a HellForge session whose bridge HD2Runtime cannot use (an older HellForge), its operations are refused with
`HELLFORGE_UNAVAILABLE` and the reason. HD2Runtime never falls back to changing the game itself over HellForge's
changes. The Mods page says when an HD2Runtime has no bridge or an older one, and when Bingus Shared Loader or
HD2Runtime is missing for an enabled Lua mod.

## Who wins a field

A field can carry live edits from several mods. They stack, and the record holds the top one:

1. the editors: the F8 editor, and Local Stats (where the launcher's editor and the F8 editor save)
2. Lua mods (`lua.<name>`), the later one on top
3. code mods

An edit under another keeps its value and takes the field when the one above it goes. Entity deltas and status
multipliers (`set_stat`) stack the same way. A row copy (`copy_row`) and a stratagem code are one value each, so the
later Lua mod's replaces the earlier one's. A Lua or code mod's edit also stays under a value that Local Stats saved and
the boot pass wrote, for as long as the record holds it.

`get_live` and `get_stat` name a Lua mod's edit as the field's holder, with its value as the base, so the F8 editor
shows "Mod: lua.<name>" and its reset goes back to the Lua mod's value. Code mods' live edits are not named, as before.

## The C interface

`native/mod-sdk/hellforge_lua.h` declares what `hellforge_modloader.dll` exports for a Lua runtime (LuaJIT's ffi):

| Function | Does |
|---|---|
| `hellforge_lua_info` | The bridge and SDK versions, the game build, the logs folder, what has loaded |
| `hellforge_lua_mod(name)` | The HellForge mod `lua.<name>` (made once; its log is `mod-lua.<name>.log`) |
| `hellforge_lua_set`, `clear`, `get` | A live edit of one value: component or settings table, target, offset, type |
| `hellforge_lua_owner` | The entity a vanilla component record belongs to, by its record index |
| `hellforge_lua_listen`, `poll` | Game events (`HF_ON_...`) for the Lua runtime, queued until its update takes them |
| `hellforge_lua_rewrites` | Whether HellForge may rewrite game code in a range (its moved tables' instructions, its hooks) |
| `hellforge_lua_stratagem_type` | A stratagem's type (its `StratagemDefinition` row) from its stable id |
| `hellforge_lua_set_stat`, `clear_stat` | (bridge 2) `set_stat` at the Lua rank: an entity delta or a status multiplier, as `HfStat` names it |
| `hellforge_lua_copy_row` | (bridge 2) `copy_row`: a catalogued row from a listed donor (the rows that go with it follow) |
| `hellforge_lua_set_code` | (bridge 2) A stratagem's input code by its stable id (1 to 9 directions), or its own back |
| `hellforge_lua_heal` .. `hellforge_lua_stop_shots` | (bridge 3) The SDK 11 actions, with plain values for the ffi |
| `game_state`, `find`, `load_package`, `set_passives`, `get_passives`, `log`, `last` | As the HfApi calls of those names |

The game thread never calls into Lua. Events wait in a queue (512 events) that the runtime empties from its own update.
A Lua runtime finds the bridge from the `HELLFORGE_MODLOADER` environment variable and the loaded DLL; outside a
HellForge session neither is there.

## Events and actions (SDK 11, bridge 3)

The SDK 11 events and actions (`hellforge_mod.h`) give HD2Runtime every event and action it had, and give code mods
the same ones ([Logic at run time](modding/runtime.md)). HD2Runtime builds its payloads from the raw events as its own
sources did, and keeps its own checks and answers for the actions (its `runtime/hellforge_events.lua` and
`runtime/hellforge_actions.lua`).

| HD2Runtime | HellForge |
|---|---|
| `player_healed` | `HF_ON_HEAL` (polled: each player's Helldiver's health record) |
| `player_fired`, `player_hit`, `player_kill_credited`, `player_damage_dealt` | `HF_ON_STAT` (polled: the local player's mission stats, 10 a second, by source) |
| `weapon_equipped`, `weapon_unequipped`, `weapon_changed` | `HF_ON_WEAPON` (polled: the local Helldiver's hand) |
| `explosion` | `HF_ON_EXPLOSION` (the explosion request hooked: every request, none missed) |
| `bughole_destroyed` | `HF_ON_BUGHOLE` (polled: the bug holes' destruction stage) |
| (code mods) | `HF_ON_DAMAGE_PRE`: damage before it applies, its amount changed or cancelled by a listener |
| `hd2.actions.heal`, `heal_limb`, `heal_limbs`, `injure`, `limbs` | `heal`, `heal_limb`, `injure`, `limb_state` |
| `hd2.status.apply` | `apply_status` |
| `hd2.actions.add_velocity`, `resupply_from_pack` | `add_velocity`, `resupply_from_pack` |
| `hd2.explosions.spawn`, `hd2.projectiles.spawn` | `spawn_explosion`, `spawn_projectile` (their packages loaded first) |
| `hd2.projectiles.homing`, `modify_shots` | `homing`, `modify_shots`, `stop_shots` (shot rules) |
| `hd2.pelican.spawn` | `spawn_pelican` (the hover held) |
| `hd2.ownership.credit_to_player` | `credit_to_player` |
| `hd2.freeze_mission_timer` | `freeze_mission_timer` |

An action called off the game thread waits for the next game update (`HF_ACT_QUEUED`). These are still not in
HellForge:

| HD2Runtime call | Why |
|---|---|
| `hd2.custom_stratagem.register` | For good: custom stratagems are HellForge stratagem mods (its stratagem registry) |
| `hd2.enemies.spawn_weight`, the director | HellForge has not mapped enemies yet |
| `modify_shots`'s `on_shot` callback | HellForge changes the shots itself, with no call into Lua |
| The mission timer freeze followed by clients | HD2Runtime's lobby sync is off on HellForge; the host's timer is the mission's |

`entity_damaged` from HellForge's damage hook carries the hit as the game queues it, before armour and multipliers.
HD2Runtime's own source measured the health lost.

## How HD2Runtime's fields map

| HD2Runtime backing | HellForge |
|---|---|
| A component record (`component`, `recordIndex`) | `set_live` on the entity that owns it (`hellforge_lua_owner`), shared records with `HF_LIVE_SHARED` |
| A settings row (projectile, damage, explosion, status, arc, beam) | Its table's row by id (`recordType` or `nativeIdentity`; `row` is its position, never used) |
| A stratagem definition (`StratagemDefinition`, by stable id) | Its row by type (`hellforge_lua_stratagem_type`); its name and icon members too |
| A booster's own row (`booster_table`) | `BoosterSettings`, by the row the catalogue names |
| A booster's explosion, damage, status, granted stratagem | The rows its target names by id (`explosionType`, `damageType`, `statusType`; the granted stratagem's stable id) |
| A stratagem's payload component (the Orbital Laser's `OrbitalAbilityComponentData`) | `set_live` on the payload entity its `rootLink` names |
| An attachment's entity delta | `set_stat` `EntityDelta`: `"<component>:<member>"`, the attachment's resource and settings index, the data offset |
| A mission timer | A director's `HiveMindComponentData` +116 (`set_live`), or an entity delta (`set_stat`) |
| A booster status's multiplier | `set_stat` `StatusMultiplier`: the stat, the status row's id, the element |
| An armor perk's value | `PassiveModifier` or `PassiveStat` row `passive * 16 + index`, the offset within the entry |
| The G-109 Urchin's pulse explosion | `copy_row` into `ExplosionSettings` 369 (412, the final discharge, follows it) |
| A stratagem's calldown code | `hellforge_lua_set_code` |

A stratagem code changed in a mission shows in the HUD's stratagem list from the next mission, because the game draws
the list from the rows when it builds it. HD2Runtime's own HUD redraw follows only its own code arrays.
