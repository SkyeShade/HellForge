# How the world calls work

<!-- diataxis: explanation -->

This page is for people working on HellForge itself. It explains what the SDK 21 calls read and call in the engine.
The mod's side is in [modding/world.md](modding/world.md), and the rest of the mod machinery in
[internals.md](internals.md).

## What each call uses

The SDK 21 calls (`world_calls.c`) call the engine's own C API, the table at game.dll+0x3326308, as the game's Lua
bindings do. Each slot and the first bytes of its function are checked before the first call.

| Call | Reads or calls |
| --- | --- |
| `play_effect` | the World table (+0x8): create particles (+0x278), link them to a unit (+0x338); the Unit table (+0x18): node, pose, alive |
| `stop_effect` | the World table: stop spawning (+0x2A8) or destroy (+0x298). An id that already ended is ignored |
| `find_enemies` | the health manager's records (game.dll+0x3326688) and the faction manager's masks (game.dll+0x3326CB8) |
| `armory_weapon` | the menu object (game.dll+0x347CE38): its state at +0; the customize screen at +0x158, its weapon at +0x10; the Armory at +0x60 |

- Effects play in the gameplay world, the Game's +0x10E8. A particles resource must pass the Application table's
  can-get check (+0x388) first, as an unloaded one crashes the engine.
- An effect is created at the node's world pose and linked with the "destroy" orphan policy. The unit's death then
  ends it.
- The Armory's selected id is read as its CUSTOMIZE finds it (0x1458192, 0x18D2270). The item list at the
  screen's +0x7FDE8 holds the selected category and index, each category's count and the ids in one array.
- The link call does not check its unit, so the unit's alive check comes first.
- A shared effect travels on the mod channel under the id `@hellforge.effects`, which no mod id can take. It names
  the entity by network id; the receiver finds it with `entity_of_network` and plays the effect on the game thread.

## Where to go next

- [internals.md](internals.md): the rest of how HellForge implements mods.
- [multiplayer.md](multiplayer.md): how co-op works, including the mod channel.
- [sdk-reference.md](sdk-reference.md): every SDK call, component, table and constant.
