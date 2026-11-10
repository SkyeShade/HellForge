# New vehicles

<!-- diataxis: how-to -->

This page is for code mod authors who want a vehicle of their own: an FRV, a tank or an exosuit that a Pelican drops.
You should know how to add an entity and a stratagem: see [new entities](content.md#new-entities).

A new vehicle is a copy of a vanilla one with records of its own, the way the game makes its own variants. It can
change any record, and it can have a model of its own as long as the model keeps its base's joints. A vehicle built
from nothing (its own skeleton, seats, drivetrain and camera) is not possible yet.

## Contents

- [A copy with stats of its own](#a-copy-with-stats-of-its-own)
- [A model of its own](#a-model-of-its-own)
- [Calling it in](#calling-it-in)
- [What stays the base's](#what-stays-the-bases)

## A copy with stats of its own

Register an entity with the vanilla vehicle as `base` and `own_all = 1`, so every record is the copy's own and your
changes leave the vanilla vehicle alone. Change records with `set`:

```c
static const HfSet rallySet[] = {{HF_FIELD(VehicleMotion, steering_response_speed), HF_F32(6.2f)}, {0}};
static const HfEntityDef rally = {.size = sizeof(HfEntityDef), .name = "yourname/your_mod/rally_frv",
    .base = 0xCC21C7FFD3EBEFB9ull, .own_all = 1, .set = rallySet,
    .designation = "M-102R", .title = "Rally FRV", .description = "An M-102 with a quicker steering rack."};
```

An exosuit's arms are mounts: give it arms of your own with `mount`, as the `vehicle-variants` example does. A ground
vehicle ignores `model_from`, because its seats, wheels, tracks and damage zones name its own model's parts.

## A model of its own

Give the copy a unit of your own: a copy of the vehicle's unit with your mesh in parts (`"parts": true` on a unit
copy's `mesh`, [data mods](data.md#copy)), named in the entity's `unit`:

```json
{ "package": "yourname/your_mod/buggy", "load": "session", "resources": [
  { "type": "unit", "copy": "0xCC21C7FFD3EBEFB9", "name": "yourname/your_mod/buggy",
    "mesh": { "obj": "models/buggy.obj", "parts": true } } ] }
```

```c
static const HfEntityDef buggy = {.size = sizeof(HfEntityDef), .name = "yourname/your_mod/buggy",
    .base = 0xCC21C7FFD3EBEFB9ull, .own_all = 1, .unit = "yourname/your_mod/buggy"};
```

The unit keeps every joint of the vehicle's, so its seats, wheels, gun mount and damage zones still fit. Your OBJ is in
the unit's own space at rest: metres, +Y forward, +Z up, +X to the right (with `yUp`, the default, Blender's export of a
model sitting that way). Each object in the file rides the joint it is named after, and moves with it:

| Object name | What it does |
|---|---|
| none (faces before the first `o`) | rides the main bone, the vehicle's body |
| a joint's name, such as `l_front_rotation` | turns and moves with that joint |
| a joint's name with Blender's number, `l_front_rotation.001` | the same joint |

Put a part where its joint is. `HellForge.Cli unit-joints --unit <unit>` lists a unit's joints with their positions.
The M-102's are:

| Joint | Part | Position (x, y, z) |
|---|---|---|
| `boss` | the body (the main bone) | 0, 0.241, 0.760 |
| `l_front_rotation` | front left wheel | -0.903, 1.704, 0.096 |
| `r_front_rotation` | front right wheel | 0.848, 1.704, 0.096 |
| `l_rear_rotation` | rear left wheel | -0.907, -1.647, 0.094 |
| `r_rear_rotation` | rear right wheel | 0.904, -1.647, 0.094 |
| `steering_wheel` | the steering wheel | -0.361, 0.797, 1.038 |

A vehicle's meshes are split into groups by material. A part goes in the first group whose bones reach its joint and
draws with that group's material: the M-102's wheels draw with its tyre material and the body with its body material.
Where a level of detail or a shadow does not reach a joint, that part rides the nearest joint above it there.

The `vehicle-model` example is an M-102 under a body of boxes, with its wheels on the four wheel joints.

## Calling it in

A Pelican drop stratagem carries the vehicle. Add a stratagem whose base is the vanilla vehicle's, with your vehicle
and the transport Pelican as its payload:

```c
static uint64_t payload[2];
payload[0] = hf->register_entity(mod, &buggy);
payload[1] = HF_ENTITY_PELICAN_TRANSPORT;
HfStratagem s = {sizeof s, .base = HF_STRATAGEM_M102_GUNNER_FRV, .name = "yourname/your_mod/buggy",
    .code = "RRDLLUDD", .payload = payload, .payload_count = 2};
hf->add_stratagem(mod, &s);
```

The drop loads the package of its first payload entity, your vehicle's.

## What stays the base's

- Physics and collision, and so how it drives: the drivetrain is in the physics resource.
- Animations, seats and where players sit, the camera, and the HUD.
- Damage zones and where they are.
- Room: the game's vehicle tables have space for about 3 new tracked vehicles and 13 ground vehicles in all.

Models HellForge cannot rewrite yet are refused with the reason, such as the tanks' bone weights.
