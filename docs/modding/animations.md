# Animations

<!-- diataxis: reference -->

A data spec's `animations` resource gives a unit copy animations of its own. One JSON file says which joints move,
the clips that move them, and the state machine that plays the clips when the game sends the weapon its events.
HellForge builds the file into the engine's bones, animation and state machine resources. This page describes that file.

## Contents

- [In the data spec](#in-the-data-spec)
- [The file](#the-file)
- [bones and rest](#bones-and-rest)
- [clips](#clips)
- [layers and states](#layers-and-states)
- [Events the game sends a weapon](#events-the-game-sends-a-weapon)
- [Finding a unit's joints](#finding-a-units-joints)
- [Timing a reload](#timing-a-reload)
- [Limits](#limits)

## In the data spec

```json
{ "type": "unit", "copy": "0x968211C0033DCE64", "name": "yourname/your_mod/rifle" },
{ "animations": "animations/rifle.json", "unit": "yourname/your_mod/rifle" }
```

`animations` is a `.json` file in the mod folder. `unit` names a unit copy in the same patch. The copy then points at
its own bones and state machine, both under the unit's name. Each clip is named `<unit>/animation/<clip>`, such as
`yourname/your_mod/rifle/animation/fire`.

The file is part of the spec, so changing it rebuilds the data and changes the modlist code. HellForge checks everything
it builds from the file, so mod sync treats it as content ([data specs](data.md#data-specs)).

## The file

```json
// comments and trailing commas are allowed
{
  "bones": ["bolt", "attach_mag"],
  "clips": {
    "idle": { "length": 1 },
    "fire": { "length": 0.15, "keys": { "bolt": { "position": [[0.03, 0, 0.255, 0.1639], [0.15, 0, 0.3369, 0.1639]] } } },
    "reload": {
      "length": 3.333,
      "keys": { "attach_mag": { "position": [[0.3, 0, -0.1874, 0.109], [0.5, 0, -0.1874, -0.6], [1.4, 0, -0.1874, 0.109]] } },
      "events": [[0.5, "mag_out"]]
    }
  },
  "layers": [
    { "default": "idle", "states": {
        "idle": { "clip": "idle", "loop": true, "on": { "fire": "fire" } },
        "fire": { "clip": "fire", "end": "fire_done", "on": { "fire": "fire", "fire_done": "idle" } } } },
    { "default": "Empty_State", "states": {
        "Empty_State": { "empty": true, "on": { "reload": ["reload", 0.2] } },
        "reload": { "clip": "reload", "end": "reload_done", "on": { "reload_done": "Empty_State", "reset": ["Empty_State", 0.05] } } } }
  ]
}
```

| Field | Value |
|---|---|
| `bones` | the joints the clips move, each once (required) |
| `rest` | rest poses by joint name; optional, see below |
| `clips` | clips by name: one word, such as `fire` (at least one) |
| `layers` | the state machine's layers (at least one) |

Any field this page does not list is refused, and every refusal names the field, such as
`clips.reload.keys.attach_mag.position: keys are in time order`.

## bones and rest

`bones` lists the unit's joints that move. Each must be a joint of the copied unit, named as the game names it. HellForge
adds the two root entries every weapon has (`StingrayEntityRoot`, `FbxAxisSystem_ConvertNode`) in front.

A joint's rest pose is its pose at time 0 in every clip. By default it is the unit's own joint transform, so most files
leave `rest` out. `rest` sets it by name: `{"bolt": {"position": [x, y, z], "rotation": [x, y, z, w], "scale": [x, y,
z]}}`, each part optional (0, no rotation, 1).

## clips

| Field | Value |
|---|---|
| `length` | seconds, more than 0 |
| `keys` | per joint in `bones`: `position`, `rotation` and `scale`, each a list of keys |
| `events` | `[[seconds, "event"]]`, events the clip sends while it plays, from 0 to `length` |

A position or scale key is `[seconds, x, y, z]`, and a rotation key is `[seconds, x, y, z, w]` (a quaternion). Values are
the joint's local transform, in its parent's space and in metres, not offsets from the rest pose. They stay within
+-10.

Keys come after 0 (time 0 is the rest pose), in time order, at least 1 ms apart, and within `length`. A channel's last key
holds to the end of the clip. A joint or channel without keys stays at rest. `idle` above has no keys at all: the gun
holds its rest pose.

## layers and states

Each layer plays one state at a time, and all layers play together. A layer is
`{"default": "<state>", "states": {...}}`, plus an optional `bones` list. With `bones`, the layer moves only those
joints, and another layer can animate the rest.

| State field | Value |
|---|---|
| `clip` | the clip it plays, or `"empty": true` for a state that plays nothing |
| `loop` | `true` to repeat the clip; default `false` |
| `speed` | playback speed from 0 to 100; default 1 |
| `end` | the event the state sends when its clip ends, such as `fire_done` |
| `on` | `{"<event>": "<state>"}`, or `{"<event>": ["<state>", <blend seconds>]}`; the event switches the layer to that state |
| `events` | `[[seconds, "event"]]`, events sent that long after the state starts |

An `on` target must be a state of the same layer, and blends are 0 to 10 seconds (default 0). A state may name itself:
the game's own `fire` states have `"fire": "fire"` for the next shot. Events are names, hashed as the game hashes them, or `0x` and 8 hex
digits for an event whose name is not known.

## Events the game sends a weapon

| Event | When |
|---|---|
| `fire` | every shot |
| `reload` | a reload starts |
| `reload_fast` | a shorter reload; half the game's weapons handle it, so handle it as well as `reload` |
| `cycle` | only from fire abilities that send it, such as a bolt-action's |

The shape in [the file](#the-file) is the game's own for a rifle. Layer 0 plays `idle` and `fire`, and the second layer
plays reloads over it from an empty state. Your own `end` events, such as `fire_done` and `reload_done`, return a layer to
where it started.

## Finding a unit's joints

```text
HellForge.Cli unit-joints --install <installation> --unit 0x968211C0033DCE64
```

It lists the unit's joints: the name hash, the name where the game's data has it, and the local rest transform. Use
`--game-data <game/data>` instead of `--install` for a game data folder. The unit's named joints are the ones its own
clips move, such as the AR-23 Liberator's `bolt`, `bolt_carrier`, `attach_mag`, `ejection_cover` and `fire_selector`.
Only named joints can go in `bones`.

The rest transforms show each joint's axes. On the Liberator, the bolt runs back along -y and the magazine drops along
-z.

## Timing a reload

The weapon's reload ability starts the gun's `reload` state together with the Helldiver's own reload, and plays both at
one speed. Make your reload clips as long as the vanilla weapon's whose `reload_ability` you use, so the magazine stays
in the Helldiver's hand. A standalone weapon also sets `WeaponReload.duration` to that length (the class leaves it 0),
and its reload states need a `reset` exit, or an interrupted reload leaves the gun in its reload pose:

| Reload of | `reload` | `reload_fast` |
|---|---|---|
| AR-23 Liberator | 3.333 s | 2.167 s |
| BR-14 Adjudicator | 3.0 s | 1.833 s |

## Limits

- Only joints with geometry show movement. A unit copy with a `mesh` of your own is rigid on the unit's first joint, so
  its animations move nothing yet.
- The file replaces all of the copy's animations. Joints not in `bones` stay at rest, such as the Liberator's fire
  selector and ejection cover in the example.
- Clips carry no sounds. A weapon's sounds come from its records and abilities.

Example: the animated variant of `liberator-variants`, on the AR-23's model. Its bolt and carrier kick back on each shot,
and its magazine drops and comes back on a reload.
