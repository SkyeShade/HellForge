# Mods that change while the game runs

HellForge can change a running session's mods without a restart: when a player switches a mod on or off on the Mods
page during a session, and when mod sync gives a joining player the host's mods (**APPLY AND JOIN**,
[docs/mods.md](../mods.md#changing-mods-while-you-play)). This page is for mod authors: what HellForge does with your
mod then, and what a code mod must do to take part.

A change is one transaction. The session's backend plans the whole difference, the in-game loader checks all of it, and
then makes it, or refuses all of it and leaves the game as it was. A refused change waits for the next session, so a
mod that does not take part still works: it just needs a restart to change.

## Which mods change live

| What the mod has | Live? | How |
|---|---|---|
| stats (`hellforge-stats.json`, `componentWrites`) | yes | the stats layer: each line keeps its place in load order |
| `"own": true` stat lines | yes | the entity gets its record of its own (new spawns take it); refused when HellForge already holds a field of that record |
| network field types | yes | widened (or narrowed back) at once |
| game patches and data specs | yes | one package for the change; replaced resources are overridden by name |
| code with `"live": true` (SDK 16) | yes | set up and taken out while the game runs, as below |
| code without `"live"` | no | next session |
| Lua mods, cosmetic packs | no | next session (they are not part of mod sync either) |
| a `premium` list the player's synced account does not own all of | no | next session, which loads it with its weapons and stratagems locked ([premium content](concepts.md#premium-content)); a mod already running keeps the lock it started with |

Everything happens only while no friend is in the game, on the ship or the title screen (stats alone also in a mission).

## A code mod that opts in

Declare it in the manifest, with SDK 16:

```json
"code": { "dll": "my_mod.dll", "sdk": 16, "live": true }
```

Then your mod may be set up after the game has started, and taken out before it ends:
- **`hellforge_mod_init` may run while the game runs.** `api->load_phase(mod)` says which: `HF_LOAD_BOOT`, or
  `HF_LOAD_LIVE`. Register content, events and hooks exactly as at boot. `on()` calls you back after your init returns,
  not inside it: every load event that already fired (`HF_EVENT_GAME_LOADED`, `ENTITIES_READY`, `NETWORK_READY`,
  `PROJECTILES_READY`) is delivered to you in the order the game fired it, once the whole change is checked.
- **`HF_EVENT_MODS_CHANGED`** reaches every mod after a change. Read `mods()` and `content()` again if you show them.
- **Taking out.** HellForge removes everything you made through the API: game-thread callbacks, event listeners, queued
  actions, shot rules, hooks (`unhook` takes one out yourself), `api->write` changes (put back, newest first; bytes
  someone changed since are left alone), stats and field edits. What you started outside the API, stop it in the optional
  export:

  ```c
  HF_MOD_EXPORT int hellforge_mod_unload(const HfApi* api, HfMod* mod, uint32_t reason); /* HF_UNLOAD_REMOVE or _REPLACE */
  ```

  Return 0 once your threads and timers are stopped. Without the export, HellForge takes the mod out only when none of
  its own threads still runs. Your DLL is never unloaded (a thread may still be returning into it), so a new version is
  a new file (HellForge loads each build from its own path).
- **Writes outside `api->write`** (a raw `memcpy` into game code) cannot be put back: a mod that made one is never taken
  out live.
- **Use `api->write`'s result** in a game-thread callback or a hook's detour. HellForge knows whose write it is from
  where the call returns; a call the compiler turns into a tail call (a function that ends with it) returns elsewhere.
  From your init, an event callback or a helper of your own HellForge still finds you; from a game-thread callback or a
  detour it cannot, and that write is not put back when the mod is taken out.

## Content added live

What a live mod registers is numbered as a fresh start of the session's mods would number it: code mods are set up in
the order of their ids, and each one's stratagems, projectiles, damage types, behaviours and abilities are numbered in
its call order; weapons and entities by their resource ids. Friends with the same mods therefore number them alike, as
co-op needs. A number that a mod taken out this session held goes to whatever a fresh start numbers there: another
mod's stratagem, projectile, damage type, behaviour or ability takes it over, so a player whose content mods differ from
a host's takes the host's live. A stratagem's number passes only while the game holds none of the old stratagem's
packages, which on the ship it does not; the loadout screen must be closed. A weapon's or entity's number stays the
removed mod's until the next session. The change is refused (a restart) when the number is not free: another mod that
stays holds it, a weapon or entity taken out held it, a mod that stays edits that row by its number (`set_field`,
`set_live`), or a mod back this session would get a number other than its own old one. A new version of your mod that
registers the same names again keeps its old numbers; entries under other names take them over as another mod's would.

New items are owned at once (the catalogue's copies of the base's items, as the backend makes them at login). What a mod
taken out registered stays in the game's memory, so nothing that still holds it breaks, but it leaves `content()`,
`find()`, the items you own and the loadout: a removed stratagem leaves every loadout, since a host that lacks it would
crash on receiving it.

A level 11 difficulty without larger maps or extra objectives is added live; the game offers it once no menu is open.
Larger maps (Hell Forge) need the buffers the game makes once as it starts, so they need a new session, and so does
taking a difficulty out.

## Data

The change's game data (your `data/` patches and your data spec's output) is written as one package, which the game
loads by id. A resource the game has not loaded keeps its name; one it has (the game's own, or another mod's) gets a
name of its own in the package, and the game's lookups of the old name are pointed at it. Objects already in the world
keep what they loaded until they are made again: a respawn, or the next mission. Taking a data mod out points the names
back at the bytes of the mods that stay, or the game's own.

## Testing it

Switch your mod on and off on the Mods page while a solo session is on the ship. The console says what changed; the
session's `backend/live-mods.log` and `modloader.log` say what the loader did, and why it refused a change.
