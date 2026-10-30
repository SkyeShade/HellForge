# Logic at run time

This chapter is for code mod authors whose mod needs to do something while the game runs: listen to what happens, make
the game do things, and change values live. Content itself stays game data ([content](content.md)); this page is the
logic around it.

After reading it you will be able to:

- run code when the game reaches a point of its start, and on every game update;
- listen to game events (a spawn, a hit, a kill, a stratagem called in) and change a hit's damage before it applies;
- make the game do what it already does (heal, apply a status, spawn an explosion) through actions;
- change a field in the running game with a live edit, and load a package;
- read the session: the co-op role, the game's state, the other mods.

Before you start, read [content](content.md), and from [how mods run](concepts.md) the sections on
[when your code runs](concepts.md#when-your-code-runs) and [threads](concepts.md#threads). Where the SDK has no event or
action for what you need yet, [hooks](hooks.md) reach the game's functions directly.

## Contents

- [Three ways your code gets called](#three-ways-your-code-gets-called)
- [Load events](#load-events)
- [Game events](#game-events)
- [The game thread](#the-game-thread)
- [Actions](#actions)
- [Live edits](#live-edits)
- [Packages](#packages)
- [The session, the player and other mods](#the-session-the-player-and-other-mods)
- [Where to go next](#where-to-go-next)

## Three ways your code gets called

After `hellforge_mod_init` returns, the game calls your mod in three ways:

| Way | Register with | Called | On |
|---|---|---|---|
| Load events | `hf->on` (SDK 1) | once, when the game reaches a point of its start | the loader's watch thread |
| Game events | `hf->subscribe` (SDK 7) | each time something happens in the game | the game thread |
| Game updates | `hf->on_game_thread` (SDK 6) | every game update (every frame) | the game thread |

Register all three from `hellforge_mod_init`. The game thread matters because the game's own engine calls (sounds,
spawns, package requests) come from it: [actions](#actions) run at once there, and wait for the next update anywhere
else.

Check `hf->version` for the SDK your mod requires. For some calls, `hellforge_mod.h` also asks you to check `hf->size`
before calling, so a mod still runs on a HellForge whose table ends before them: `on_game_thread`, `last_log`,
`set_stat`, `copy_row`, `mods`, every action, `notice`, `hook_ex` and `code_matches`. The check looks like this:

```c
if (hf->size >= offsetof(HfApi, heal) + sizeof hf->heal) { /* heal is there */ }
```

## Load events

`hf->on(mod, event, callback, user)` calls back once when the game reaches a point of its start, on the loader's watch
thread, in the order mods registered (mod-id order, as they are set up). Registered after the event has happened, the
callback runs at once, on the caller's thread. `HF_EVENT_GAME_LOADED` comes first; the others come in no promised
order:

| Event | When | Typical use |
|---|---|---|
| `HF_EVENT_GAME_LOADED` | game.dll's code is ready | find functions, hook, patch code |
| `HF_EVENT_NETWORK_READY` | the engine's network configuration exists | read or widen network field types |
| `HF_EVENT_ENTITIES_READY` | the entity data has loaded, with every entity mods added; again after a reload | read records, log what loaded |
| `HF_EVENT_PROJECTILES_READY` | the projectile and damage lists have loaded, with mods' | change your projectile and damage rows |
| `HF_EVENT_SESSION_END` (SDK 16) | the session is ending: the launcher stops it, or the game quits normally | save or flush your files |

`HF_EVENT_SESSION_END` comes once, last. A stop from the launcher delivers it on the watch thread, and the session
runner waits up to 2 seconds for your callback before it ends the game. A normal quit delivers it on the quitting thread
while Windows ends the process: the game's other threads are gone and Windows' loader lock is held, so only write and
close files there, load no DLL and wait for nothing. A crash delivers nothing, and on build 25480438 the game crashes as
it quits ([debugging](debugging.md#crashes)), so do not keep work that matters for the last moment. An older loader
refuses the event: check `hf->version >= 16` first.

`examples/mods/machine-gun-10k-code` uses two of them: it widens a network field type at `HF_EVENT_NETWORK_READY`, and
reads its backpack record back at `HF_EVENT_ENTITIES_READY`:

```c
static void BackpackReady(HfMod* mod, HfEvent event, void* user)
{
    const HfDeposit* deposit = hf_record(mod, HF_ENTITY_MAXIGUN_BACKPACK, HfDeposit);
    if (deposit != NULL)
        HF_API(mod)->log(mod, "Maxigun backpack: capacity %u, start %d, refill %u",
            deposit->capacity, deposit->start_amount, deposit->refill_amount);
}

/* in hellforge_mod_init */
hf->on(mod, HF_EVENT_NETWORK_READY, WidenAmmoField, NULL);
hf->on(mod, HF_EVENT_ENTITIES_READY, BackpackReady, NULL);
```

`HF_API(mod)` (SDK 6) gives you the API table from the mod handle, so a callback needs no global.

## Game events

`hf->subscribe(mod, kind, listener, user, priority)` (SDK 7) hands you what happens in the game. You subscribe one
listener per kind of event. Listeners are called on the game thread, highest priority first, equal priorities in
subscription order. A listener gets an `HfEventInfo` and casts it to its kind's struct; the data is valid during the
call only.

The compendium's Heavy MG Sentry (`examples/mods/stratagem-compendium/src/hmg_sentry.c`) logs when its beacon
activates, when one of its sentries lands, and how many were deployed each mission:

```c
static void Called(HfMod* mod, const HfEventInfo* event, void* user)
{
    const HfStratagemEvent* called = (const HfStratagemEvent*)event;
    if (called->type == hmgType)
        HF_API(mod)->log(mod, "HMG sentry called in at (%.1f, %.1f, %.1f)", (double)called->position[0], (double)called->position[1],
            (double)called->position[2]);
}

static void Spawned(HfMod* mod, const HfEventInfo* event, void* user)
{
    const HfSpawnEvent* spawn = (const HfSpawnEvent*)event;
    if (spawn->resource == hmgRef) HF_API(mod)->log(mod, "HMG sentry %u is down (%d this mission)", spawn->entity, ++deployed);
}

static void Mission(HfMod* mod, const HfEventInfo* event, void* user)
{
    if (event->kind == HF_ON_MISSION_END) HF_API(mod)->log(mod, "mission over: %d HMG sentr%s deployed", deployed, deployed == 1 ? "y" : "ies");
    deployed = 0;
}

/* in hellforge_mod_init, after add_stratagem */
hf->subscribe(mod, HF_ON_STRATAGEM, Called, NULL, 0);
hf->subscribe(mod, HF_ON_SPAWN, Spawned, NULL, 0);
hf->subscribe(mod, HF_ON_MISSION_START, Mission, NULL, 0);
hf->subscribe(mod, HF_ON_MISSION_END, Mission, NULL, 0);
```

The kinds:

| Kind | Struct | When |
|---|---|---|
| `HF_ON_STATE` | `HfStateEvent` | the game's state changes (`HF_STATE_TITLE`, `_SHIP`, `_MISSION`, `_PREPARE_SHIP`...); `hf->game_state(mod)` reads it any time |
| `HF_ON_MISSION_START`, `HF_ON_MISSION_END` | `HfStateEvent` | a mission starts or ends |
| `HF_ON_PLAYER_SPAWN`, `HF_ON_PLAYER_DOWN` | `HfPlayerEvent` | a player lands (dropped in, reinforced) or dies; players by peer id, `local` marks this machine's |
| `HF_ON_DAMAGE` | `HfDamageEvent` | damage this machine applies: target, kind, element, dealer, owner, creditor, amount (before armour and multipliers), where it hit |
| `HF_ON_DAMAGE_PRE` (SDK 11) | `HfDamagePreEvent` | the same, before it applies: a listener may change `damage.amount` or set `cancel` |
| `HF_ON_KILL` | `HfKillEvent` | something is killed: victim, dealer, creditor, kind, element |
| `HF_ON_STRATAGEM` | `HfStratagemEvent` | a stratagem's beacon activates: its type, the thrower, the beacon's position |
| `HF_ON_SPAWN` | `HfSpawnEvent` | an entity spawns: its entity id, network id, resource, whether this machine simulates it |
| `HF_ON_HEAL` (SDK 11) | `HfHealEvent` | a Helldiver's health went up: amount, health now, maximum, peer |
| `HF_ON_STAT` (SDK 11) | `HfStatEvent` | the local player's mission stats grew (`HF_STAT_SHOTS`, `_HITS`, `_KILLS`, `_DAMAGE`), with up to five sources |
| `HF_ON_WEAPON` (SDK 11) | `HfWeaponEvent` | what the local Helldiver holds changed |
| `HF_ON_EXPLOSION` (SDK 11) | `HfExplosionEvent` | an explosion was requested: its type, where, its source, the creditor |
| `HF_ON_BUGHOLE` (SDK 11) | `HfBugholeEvent` | a bug hole was closed: entity, resource, stage, where |

`hellforge_mod.h` documents every field of every struct.

### Rules for events

- Events observe. Changing an event's data changes nothing, except `HF_ON_DAMAGE_PRE`'s `amount` and `cancel`.
- Each machine raises its own. A hit is raised on the machine that applies it (the target's owner); a stratagem's
  activation on its thrower's machine.
- Entity ids are the session's entity instances, not resource ids. `HF_ON_SPAWN` pairs the two.
- There is room for 128 listeners across all mods. Events wait in a queue of 256 for the game thread; when it is full,
  new ones are dropped.
- A kind's source is installed when a mod first listens to it, and only if the game code it reads looks as reviewed.
  Otherwise `modloader.log` says so and that kind stays silent.

Missing an event? A [hook](hooks.md) on the game function that does it covers the gap, and saying which one you needed
helps it become an event.

## The game thread

`hf->on_game_thread(mod, callback, user)` (SDK 6) calls you every game update on the game thread, before the game's
update, with the frame time in seconds. The callback starts once the loader has hooked the game's update, which can
take up to a minute after `HF_EVENT_GAME_LOADED`. There is room for 16 callbacks across all mods. It runs every frame,
so keep it short.

This callback heals the local Helldiver by 10 every 5 seconds:

```c
static void Tick(HfMod* mod, float dt, void* user)
{
    static float clock;
    if ((clock += dt) < 5.0f) return;
    clock = 0;
    HF_API(mod)->heal(mod, 10.0f); /* 10 health every 5 seconds */
}
/* in hellforge_mod_init: heal is SDK 11 (on_game_thread SDK 6) */
if (hf->size >= offsetof(HfApi, heal) + sizeof hf->heal) hf->on_game_thread(mod, Tick, NULL);
```

`on_game_thread` returns 1, or 0 when the game's update could not be hooked (`modloader.log` says why).

## Actions

Actions (SDK 11) are calls that make the game do what it already does, through its own functions. New weapons,
projectiles and stratagems stay [content](content.md).

Call actions on the game thread: from a game event listener or an `on_game_thread` callback. From another thread, an
action waits for the next update and answers `HF_ACT_QUEUED`. Otherwise each answers one of:

- `HF_ACT_DONE`: done.
- `HF_ACT_REQUESTED`: the game does it this update or the next.
- `HF_ACT_WAITING`: its packages are loading; call again.
- 0: refused, with the reason in `hf->last_log(mod)`.

The exceptions are `limb_state`, `modify_shots`, `homing` and `stop_shots`, which answer at once on any thread (a rule
id for `modify_shots` and `homing`, found or not for `stop_shots`). At most 64 actions wait for the game thread at once.

In the table, "Where" is who may call it: any player, or only the host. The local Helldiver is this machine's player's,
alive.

| Call | Does | Where |
|---|---|---|
| `heal(mod, amount)` | heals the local Helldiver, up to its maximum | any |
| `heal_limb`, `injure`, `limb_state` | a limb (`HF_LIMB_HEAD` .. `_RIGHT_LEG`) healed, damaged as a hit would, or read | any; `injure` in a mission |
| `apply_status(mod, entity, status, buildup)` | a status (`HF_STATUS_FIRE`, `_STUN_LARGE`, `_GAS`...) | host |
| `add_velocity(mod, delta)` | pushes the local Helldiver (upward pushes hold; walking resets sideways speed) | any |
| `resupply_from_pack(mod)` | the local Helldiver uses its B-1 Supply Pack on itself | any, in a mission |
| `spawn_explosion(mod, type, position)` | an explosion, credited to this machine's player | host |
| `spawn_projectile(mod, type, position, direction)` | a projectile, as the local Helldiver's | host |
| `credit_to_player(mod, entity)` | an entity's kills credited to its owner | solo host |
| `freeze_mission_timer(mod, frozen)` | pauses or resumes the mission timer | host |
| `spawn_pelican(mod, &call, &entity)` | calls in a transport Pelican to hover over a point; `entity` is filled only when it runs at once, not queued | host; the hover solo |
| `modify_shots`, `homing`, `stop_shots` | rules for the local player's shots of a projectile type: damage, penetration, speed, gravity, drag, or steering | solo, or `HF_SHOT_MULTIPLAYER` |

The host-only actions also need a mission in progress. The limits on each call's values (an injury's damage, a push's
speed, a shot rule's multipliers) are in `hellforge_mod.h`.

An action called from a game event listener runs at once. Here a big heal also mends a leg:

```c
static void Healed(HfMod* mod, const HfEventInfo* event, void* user)
{
    const HfHealEvent* heal = (const HfHealEvent*)event;
    if (heal->local && heal->amount >= 50) HF_API(mod)->heal_limb(mod, HF_LIMB_LEFT_LEG); /* a big heal mends a leg too */
}
/* in hellforge_mod_init */
hf->subscribe(mod, HF_ON_HEAL, Healed, NULL, 0);
```

## Live edits

A live edit (SDK 6) changes a field in the running game, with the same checks and conflict reports as the deferred
edits on the [content](content.md#changing-existing-content) page. Use it for changes during play (a mode switch, an
editor); use a deferred edit for your mod's content itself.

```c
int when = hf_live(mod, HF_ENTITY_MAXIGUN_BACKPACK, Deposit, capacity, HF_U32(5000), 0);
/* HF_APPLIES_NOW, _RESPAWN, _MISSION, or 0 refused (the log says why) */
```

`hf_live` is the macro form of `set_live(mod, target, &field, &value, flags)`; `get_live` reads a field the same way.

### When a change applies

Every field has a timing, shown as a `[tag]` in the reference:

- `now`: the next time the game reads it (projectile, damage, explosion and status settings, the director's budgets).
- `respawn`: when the entity is next made (the next spawn, re-equip or call-in).
- `mission`: when the next mission loads (stratagem settings, packages, level generation).

A `?` marks a timing not yet confirmed from game code.

### What a live edit finds

The target is found by identity every time: an entity by its resource id, or a settings row by its id (a projectile
type, a damage id, a stratagem type, a level, an explosion, arc, beam or status row).

`examples/mods/vanilla-plus` live-edits every hellpod stratagem's beacon flags at `HF_EVENT_ENTITIES_READY`, so blue
beacons stick where they land. It reads each stratagem type's row with `get_live` and writes with `set_live`:

```c
static void StratagemBounce(HfMod* mod, HfEvent event, void* user)
{
    HfField delivery = HF_FIELD(StratagemSettings, delivery), flags = HF_FIELD(StratagemSettings, beacon_flags);
    unsigned changed = 0, kept = 0;
    for (uint32_t type = 1; type <= LAST_TYPE; type++)
    {
        HfLiveValue kind, beacon;
        if (!hellforge->get_live(mod, type, &delivery, &kind) || !hellforge->get_live(mod, type, &flags, &beacon)) continue;
        if ((uint32_t)kind.now.number != DELIVERY_HELLPOD) continue;
        uint32_t now = (uint32_t)beacon.now.number;
        if (!(now & NAVMESH_ONLY)) { kept++; continue; }
        HfValue value = HF_U8(now & ~NAVMESH_ONLY);
        if (hellforge->set_live(mod, type, &flags, &value, 0)) changed++;
        else hellforge->log(mod, "stratagem type %u: its beacon was left as it is (%s)", type, hellforge->last_log(mod));
    }
    hellforge->log(mod, "stratagem bounce: %u hellpod stratagems' beacons now stick like orbitals' (%u already did)", changed, kept);
}
```

### Undoing, sharing and saving

- Your next live edit of the same field replaces your last; a NULL value undoes it.
- A record other entities share changes only with `HF_LIVE_SHARED`, and then for all of them.
- Live edits last for the session and are renewed if the game reloads its data. Nothing is saved: an editor saves into
  a stats file.

### Reading values

`get_live` returns `now` (what the record holds) and `base`: the value without the Local Stats mod and live edits,
which is another mod's (named in `out.mod`), else the game's.

### Fields `set_live` cannot reach

- `set_stat` (SDK 9) and `get_stat` (SDK 6) reach attachments' and default magazines' entity deltas, and boosters'
  status multipliers.
- `copy_row` (SDK 9) replaces a settings row with a listed donor's (`mappings/references.json`, `rowCopies`).
- References (a weapon's projectile, a projectile's impact explosion, a damage type's statuses) take rows the reference
  catalogue knows (`mappings/references.json`), and wait until the packages they need are loaded.
- Armor passives (`set_passives`, `get_passives`, SDK 6): the local player's armor passive and a second one, solo only.

## Packages

A package is a set of game resources the engine loads together (a weapon's model and sounds, an explosion's effects).
What your mod uses must be loaded, so a stratagem or effect that needs another one's resources needs that package held.

`hf->load_package(mod, package)` (SDK 6) loads a package from HellForge's catalogue (`mappings/references.json`) through
the game's own package system and holds it for the session. It answers `HF_PACKAGE_LOADING` until the engine has all of
it (call again to see), then `HF_PACKAGE_RESIDENT`, or 0 when refused. At most 64 packages are held, counting the ones
HellForge loads for references and abilities; a package not resident within 90 seconds fails. Loading is per machine:
what your mod loads is not told to other players. (In co-op HellForge does share the packages it holds for each player's
edits, mods and actions, so friends load them too: [multiplayer.md](../multiplayer.md#assets-in-co-op).)

A stratagem's payload packages, a reference's packages and a spawned explosion's are loaded for you. The compendium holds
the extra packages its stratagems need (a shell of another stratagem, say) once the entity data is in
(`examples/mods/stratagem-compendium/src/compendium.c`):

```c
static void Hold(HfMod* mod, HfEvent event, void* user)
{
    for (int i = 0; i < g_packageCount; i++)
        if (!HF_API(mod)->load_package(mod, g_packages[i]))
            HF_API(mod)->log(mod, "package %016llx could not be loaded: %s", (unsigned long long)g_packages[i], HF_API(mod)->last_log(mod));
}
```

## The session, the player and other mods

| Call | SDK | Gives |
|---|---|---|
| `session(mod, &out)` | 6 | the co-op role: `HF_SESSION_SOLO`, `_HOST`, `_JOINER`, and the host's name |
| `game_state(mod)` | 7 | the game's state (`HF_STATE_...`) |
| `mods(mod, index, &out)` | 10 | every mod of the session, code and stats mods in load order, then Lua mods (and the in-game editor, `HF_MOD_EDITOR`) |
| `content(mod, index, &out)` | 7 | every piece of content mods added |
| `notice(mod, &out)` | 12 | HellForge's latest message for the player (a refused join, say) |
| `last_log(mod)`, `logs_folder(mod)` | 6 | your last log line (why a call was refused), the session's logs folder |
| `build_id()`, `mod_id(mod)`, `mod_folder(mod)` | 1 | the game build, your id, your folder (for files your mod ships) |

`set_status(mod, text)` (SDK 16) tells the player what your mod is doing, or why it stopped, in one line: "waiting for
a mission", "off: this game build is not supported". The launcher's Mods page shows it on your mod's row, during the
session and after it, and your log records each change. Set it when it changes; setting the same text again only
compares it. `NULL` or `""` clears it. Check `hf->size` before calling it.

## Where to go next

Next in the guide: [hooks and game memory](hooks.md), for what the SDK has no event, field or action for yet.

In the [SDK reference](../sdk-reference.md): load events, live edits, packages and the session under
[SDK 6](../sdk-reference.md#sdk-6), game events under [SDK 7](../sdk-reference.md#sdk-7), the actions under
[SDK 11](../sdk-reference.md#sdk-11-actions-hf_act_-check-hf-size-before-calling), and each field's timing tag under
[components](../sdk-reference.md#components) and [settings tables](../sdk-reference.md#settings-tables).
