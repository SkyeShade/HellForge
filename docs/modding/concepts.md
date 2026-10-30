# How mods run

This page is for mod authors who have made a first mod and are planning the next one. It covers the kinds of mods, the
manifest, when your code runs and on which thread, the three ways to change game data, how content is named, co-op,
load order, compatibility and limits.

## Kinds of mods

A mod is a folder in `<installation>/mods` (or a zip of one) with a `hellforge-mod.json`. What the folder holds decides
the mod's kind, and one mod can be several kinds at once (code with a data spec, say).

| Kind | What it holds | Guide |
|---|---|---|
| Code mod | C or C++ sources under `src/`, built by HellForge against the mod SDK (`hellforge_mod.h`) | this page, [content](content.md), [run time](runtime.md), [hooks](hooks.md) |
| Prebuilt code mod | a DLL instead of sources (closed source, or built with other tools) | [tools](tools.md#prebuilt-mods) |
| Stats mod | `hellforge-stats.json`: number changes by field name, no code | [stats mods](stats-mods.md) |
| Data mod | game patch files in `data/`, or a data spec (`hellforge-data.json`) HellForge builds from each player's own game | [data mods](data.md) |
| Native edits | the manifest's `native` section: a network field type widened, a component record's u32 set | [data mods](data.md#native-edits-in-the-manifest) |
| Cosmetic pack | the community's format (`manifest.json` and folders of patches), no `hellforge-mod.json` | [data mods](data.md#cosmetic-packs) |
| Lua mod | an HD2Runtime or Bingus Shared Loader mod; client only | [lua-bridge.md](../lua-bridge.md) |

## The manifest

```json
{
  "format": 1,
  "id": "yourname.liberator-tweaks",
  "name": "Liberator Tweaks",
  "version": "1.0.0",
  "description": "What it changes, in a sentence or two.",
  "icon": "images/icon.png",
  "url": "https://github.com/yourname/liberator-tweaks",
  "gameplay": true,
  "code": { "dll": "liberator_tweaks.dll", "sdk": 16 }
}
```

| Key | Meaning |
|---|---|
| `format` | always 1 |
| `id` | `owner.mod-name`: 1-64 lowercase letters, digits, `.`, `_`, `-`, starting with a letter or digit. It names the mod everywhere (its log, its content's identities, the modlist friends compare) and must be unique: two mods with one id load neither |
| `name`, `description` | what players see on the Mods page |
| `version` | raise it for every release; friends compare it |
| `icon` | optional: a PNG or JPEG in the mod folder; square looks best |
| `url` | optional: where players get the mod, an `https://` address of at most 200 characters (no `http://`, user name, port or spaces). The Mods page links it, and the in-game MOD SYNC prompt shows it as GET IT to a joiner who has to install the mod themselves. Any other address is left out; a bundled mod's is never shown |
| `gameplay` | default `true`: friends must run the same gameplay mods. Set `false` for cosmetic mods, which are not compared. A mod with stats or `componentWrites` is always gameplay; one holding only Lua patches is always cosmetic |
| `code.dll` | the DLL's file name; HellForge builds it from `src/` |
| `code.sdk` | the SDK version the mod needs (see [compatibility](#compatibility)) |
| `code.prebuilt`, `code.game` | for a mod that ships its DLL ([tools](tools.md#prebuilt-mods)) |
| `native` | declarative engine edits ([data mods](data.md#native-edits-in-the-manifest)) |
| `premium` | optional: Arrowhead's premium content the mod recreates or builds on, such as `["dlc:super-citizen-edition"]`. Players who do not own it get none of the mod's weapons and stratagems ([premium content](#premium-content)) |

A mod must change something: code, data, a data spec, stats or native edits. A manifest with none of them is refused.

## When your code runs

```
session start   HellForge builds the source mods, stages data mods into the game copy
game starts     the loader calls each code mod's hellforge_mod_init, in the order of mod ids    loader thread
                HF_EVENT_GAME_LOADED first: game.dll's code is ready (find functions, hook)     loader's watch thread
                then, in no promised order:
                  HF_EVENT_NETWORK_READY      network field types exist
                  HF_EVENT_PROJECTILES_READY  projectile and damage rows (yours too) exist
                  HF_EVENT_ENTITIES_READY     entity data loaded (again after a reload)
game running    game events (subscribe), on_game_thread callbacks, every update                 game thread
                hook detours: whenever the game calls the function                             the caller's thread
session end     HF_EVENT_SESSION_END (SDK 16): a stop from the launcher, or a normal quit       watch or quitting thread
```

`hellforge_mod_init(const HfApi* hf, HfMod* mod)` runs once, while the game is still starting. Register everything
there: content (`add_*`, `register_entity`), edits of existing content (`edit_entity`, `hf_set`) and event listeners
(`subscribe`, `on`). Keep the `hf` pointer; it is valid for the whole session. Mods are set up in the order of their
ids, the same on every machine, so what they add is numbered alike. Their edits then apply in load order.

Return 0 from `hellforge_mod_init` to stay loaded. Any other value marks the mod as not loaded, and `modloader.log`
says so. The DLL still stays in the game, though, and whatever it queued before returning still applies, so decide
whether to decline before you register anything.

Load events (`hf->on(mod, HF_EVENT_..., callback, user)`) come once each, in the order mods registered for them. A
callback registered after its event has happened is called at once. `HF_EVENT_ENTITIES_READY` comes again when the game
reloads its entity data. Game events and the game thread are covered in [run time](runtime.md).

Content is data, so it needs no timing. The loader applies definition blocks and `hf_set` whenever the game loads its
data, before `HF_EVENT_ENTITIES_READY` reaches any mod, and again after a reload. You never time an edit with an event;
events are for logic.

## Threads

Your code runs on three kinds of threads:

- The loader's threads. `hellforge_mod_init` runs on the loader's thread while the game starts, and the load events on
  its watch thread, which runs beside the game. Once `HF_EVENT_GAME_LOADED` has come, game-thread callbacks can run at
  the same time as a later load event (`HF_EVENT_ENTITIES_READY` after a reload, say).
- The game thread, which runs game events and `on_game_thread` callbacks. The game's own engine calls (sounds, spawns,
  package requests) come from it, so HellForge's actions are safe to call there. Called from any other thread, an action
  waits for the next game update and answers `HF_ACT_QUEUED`. Keep these callbacks short: they run every frame.
- Whatever thread the game uses. A hook's detour runs on the thread that called the function, often a worker thread.
  See [hooks](hooks.md#threads).

State shared between them needs the usual care: atomics, or a lock that is never held while calling into the game.

## Three ways to change the game's data

| | How | When it applies | Use for |
|---|---|---|---|
| Deferred edits | definition blocks (`HfEntityDef`, `HfEditDef`), `hf_set`, `hf_copy`, `hf_tag`, `hf_mount`, stats mods | whenever the game loads its data | content: almost everything |
| Live edits | `set_live` / `hf_live`, `set_stat`, `copy_row` | at once, for the session (`now`, on `respawn`, next `mission`) | changes during play: an editor, a mode switch |
| Direct writes | a mod's own rows (`projectile_row`, `damage_row`, `stratagem_row`, `difficulty_row`), its own entities' records (`hf_record`), `write` | when written | what the first two do not cover |

Deferred and live edits are checked against the game build's layout, refused with your source line when something does
not match, and reported when two mods change the same field. Direct writes are not checked, so getting them right is up
to you. A vanilla entity's record is the game's read-only data, and writing through `hf_record`'s pointer crashes the
game: use `hf_set` or `edit_entity` instead.

## Names and identities

- Content a mod adds is named, and its ids come from the name, so every player's game agrees on them without talking.
  `"pelican_gatling"` added by `yourname.pelican-support` is `yourname.pelican-support:pelican_gatling`. A full name
  with `/` or `:` (`"yourname/pelican_support/pelican_gatling"`) stays as it is, and a weapon's `unit` is a resource
  name, never changed. Prefix full names with your name and mod.
- Vanilla content has constants in `hellforge_content.h` (`HF_ENTITY_...`, `HF_ITEM_...`, `HF_STRATAGEM_...`), and the
  reference lists them all. `hf->find(kind, name)` resolves a name at run time (from a config file, or another mod's
  content).
- Fields have names: `HF_FIELD(Component, field)`, and `HF_FIELD_AT(Component, field, index)` for one element of an
  array. A misspelt name does not compile.
- Values carry their type: `HF_F32`, `HF_U32`, `HF_I32`, `HF_U8`, `HF_U16`, `HF_U64`. A value of the wrong type or out
  of range is refused.
- Content is numbered in the order of mod ids, not load order, so friends with the same mods in another order still
  agree.

## Co-op

- The modlist code. Before a friend joins, HellForge compares both players' gameplay mods: ids, versions and files (a
  source mod's sources, not a DLL). Any difference refuses the join, and both games say which mods differ; the
  joiner may then sync to the host's mods ([mods.md](../mods.md#playing-with-other-mods)). Cosmetic
  mods, packs and Lua mods are not compared. Nor is the order: friends who order the same mods differently can play
  together, but where two mods conflict, a different one may win on each machine.
- Every machine runs its own mods. Each player's game runs the mods' code and data. A hit is applied by the machine
  that owns the target, and its damage events are raised there; a stratagem's activation is applied on its thrower's
  machine.
- The host decides some things. Actions marked "host" in the reference (statuses, explosions, projectiles, the mission
  timer, Pelicans) only work on the host. `hf->session(mod, &out)` gives the role: `HF_SESSION_SOLO`, `_HOST` or
  `_JOINER`. In co-op every player keeps their own Local Stats (the in-game editor's saved changes) unless the host
  shares theirs; `hf->stats_source(mod)` (SDK 15) says which.
- What travels between machines. New content travels as ids (stratagem types, projectile types), so a mod's content
  must exist on every machine, which the modlist code makes sure of. A network field type a mod widens is widened on
  every machine for the same reason.

## Premium content

Arrowhead's premium content (premium warbonds, Superstore items bought with Super Credits, and the Steam DLCs' gear)
is a player's only when they bought it in the normal game. A player brings it into HellForge with Profile > Account
sync, which reads their own account and seals it to their PC and Steam account. A mod that ports premium content, or
builds on it, must follow that sync: it may not give premium content to players who have not bought it.

HellForge does most of this for you.

- **Copies of premium content are locked automatically.** A weapon or stratagem your mod makes as a copy of a premium
  one (a premium warbond's weapon, a Superstore weapon, the Super Citizen Edition's MP-98 Knight) is in a player's
  armory only when that player owns the original. It still exists on every machine, so friends with and without it
  play together; a player without it cannot equip it.
- **Declare what your mod recreates or builds on** in the manifest's `premium` list, when it is not a plain copy: a
  free weapon made to play like a premium one, a premium armour's look on a free armour, a larger variant of a DLC
  weapon. Each entry is one of:

  | Entry | Owned when the player holds |
  |---|---|
  | `warbond:<id>` | any of that warbond's tokens (Helldivers Mobilize, `warbond:1929468580`, is everyone's) |
  | `dlc:super-citizen-edition`, `dlc:tr-117-alpha-commander`, `dlc:tr-40-gold-eagle` | any item that Steam DLC grants |
  | `item:<mixId>` | that item |

  A warbond's id is its key under `seasonPassDetails` in HellForge's progression data (`app/progression-data.json` in an
  installation, `data/progression-data.json` in the repository). A premium warbond sold on Steam, such as Steeled
  Veterans, is a `warbond:` entry. A malformed entry or an unknown DLC refuses the mod, and a warbond or item no game
  data names is never owned, so a typo never unlocks anything.

  ```json
  {
    "format": 1,
    "id": "yourname.knight-carbine",
    "name": "Knight Carbine",
    "version": "1.0.0",
    "premium": ["dlc:super-citizen-edition"],
    "code": { "dll": "knight_carbine.dll", "sdk": 15 }
  }
  ```

- **What a player who does not own all of the list gets:** a gameplay mod still loads (friends keep running the same
  mods), but none of the weapons and stratagems it adds are in their armory. A cosmetic mod (`"gameplay": false`) is not
  loaded for them. The Mods page tags the mod PREMIUM, or PREMIUM: LOCKED for the selected profile, and its details say
  what is missing. A player who has not synced owns nothing premium.
- **Your code's own effects are yours to gate.** HellForge locks items, not logic: a code mod that recreates a premium
  booster's or armour passive's effect must not apply it for players without the content. Until the SDK has an
  ownership call, give such effects only to the items HellForge locks, or leave them out.

Cosmetic packs in the community's format have no `hellforge-mod.json` and cannot declare premium content: ship a
premium look as a data mod with a manifest instead.

## Load order and conflicts

The Mods page lists mods top to bottom. The bottom mod loads first and the top one wins: its data patches get the
higher numbers, and its stats apply later. The in-game editor's Local Stats sits on top. Which change stays depends on
its kind:

- Stats files (stats mods, and Local Stats at boot) apply first, in load order: the later mod's value stays.
- Code mods' deferred edits apply after every stats file, in load order, so a code mod's edit stays over a stats mod's
  whatever their places.
- Live edits of one field stack: the editors' over Lua mods' over code mods' ([lua-bridge.md](../lua-bridge.md)).
- Network field types: two mods widening the same type do not both load. The later one by id is left out.
- Tags compose (each edit adds or removes one). Hooks chain ([hooks](hooks.md#several-mods-one-function)).

When two mods' SDK edits change the same field, both mods' logs and `modloader.log` report it with the other mod's
source line, and both mods stay enabled. A stats file's conflict is reported in the later mod's log and on the Mods
page.

## Compatibility

- SDK versions. The manifest's `code.sdk` is the SDK the mod needs. A HellForge with an older SDK refuses the mod
  ("needs HellForge mod SDK 17; this HellForge provides SDK 16"), and newer ones load it. Functions are only ever added
  at the end of `HfApi`, so a mod built for SDK 6 runs on SDK 13. In code, check what you use:

  ```c
  if (hf->version < 6) return 1;   /* hf_record, edit_entity, HF_API */
  /* a call added later: is it in this loader's table? */
  if (hf->size < offsetof(HfApi, add_stratagem_category) + sizeof hf->add_stratagem_category) return 1;
  ```

- Structs grow too. Zero-initialise them and set `size` (`HfWeapon gold = {sizeof gold};`), and fields added later read
  as unset.
- The game build. HellForge runs one game build (25480438), so the field layouts and addresses in the SDK hold. The
  generated headers carry the layout, and a field from another layout is refused. Every HellForge that changes the SDK
  rebuilds source mods, so a mod keeps working as long as the names it uses exist.

## Limits

These limits are for all mods together:

| What | Room |
|---|---|
| New entities and weapons | 64 |
| New projectile types | 64 (types 351 and up) |
| New damage types | 64 (ids 650 and up) |
| New stratagems | 48 (types 150 and up) |
| New behaviours | 8 |
| New abilities | 8 |
| Selection headings | 28 |
| Difficulty 11 | one mod |
| Packages held | 64, with the ones HellForge loads for references and abilities |
| Hooks | 16 per function and 1,024 functions, HellForge's own included |
| Event listeners, `on_game_thread` callbacks | 128, 16 |

A definition block can own or change 8 components and have 8 mounts, and all mods together can have 64 blocks. An `add_*` call that
would pass a limit returns 0 (`hook` returns non-zero), and the mod's log says why.

## Safety

A code mod runs inside the game with the player's permissions, and the loader does not catch its crashes: a crash in
your code ends the game, and `modloader.log` names where it happened ([debugging](debugging.md#crashes)). Players should
only enable code mods from people they trust. Everything runs in HellForge's own local copy; the normal game is
never touched.

Next: [Stats mods](stats-mods.md), number changes without code.
