# Models

<!-- diataxis: reference -->

A data spec's `model` resource builds a unit of your own from a glTF 2.0 file, the format Blender exports. Every node
becomes a joint and every mesh draws, rigid on its node. Node names give the levels of detail (LODs) and shadow casters.
No game model is copied. This page lists what HellForge reads from the file and what the spec says about it.

## Contents

- [In the data spec](#in-the-data-spec)
- [The file](#the-file)
- [Joints](#joints)
- [Meshes, LODs and shadows](#meshes-lods-and-shadows)
- [Materials](#materials)
- [Physics and animations](#physics-and-animations)
- [Limits](#limits)

## In the data spec

```json
{ "model": "models/carbine.glb", "name": "yourname/your_mod/carbine",
  "materials": { "Body": "0x7F112ACD09A5E523" }, "physics": "0x968211C0033DCE64" }
```

| Field | Value |
|---|---|
| `model` | a `.glb` file in the mod folder, or a `.gltf` with its buffer files beside it (required) |
| `name` | the unit's name, the mod's own resource path (required) |
| `materials` | the material resource for each of the model's material names, `0x<hash>` or a name (required) |
| `lods` | the share of the screen at which each LOD starts, such as `[1, 0.5, 0.25]`; default the weapons' own |
| `physics` | a game unit whose physics (collision) the unit takes under its own name |
| `animationsFrom` | a game unit whose animations the unit plays |

The file is part of the spec, so changing it rebuilds the data and changes the modlist code. HellForge writes the
whole unit itself, so mod sync treats it as content ([data specs](data.md#data-specs)). A weapon names the unit in its
`unit` field ([content](content.md)).

## The file

HellForge reads the default scene's nodes: each node's name, parent and translation, rotation and scale (a matrix
without shear also works), and its mesh. A mesh's primitives are triangles with positions, normals and one to three UV
sets. Every primitive has a material. Sparse accessors, morph targets and other primitive modes are refused.

Coordinates are metres. A weapon points down +Y with +Z up and +X to its right. These are Blender's own axes: model in
Blender as the gun sits in the game and export with the glTF exporter's default "+Y Up" setting. HellForge turns
glTF's Y-up back to the game's Z-up, (x, y, z) to (x, -z, y).

## Joints

Every node is a joint with the node's name, and HellForge adds the root joint `StingrayEntityRoot` above the scene's
root nodes. Names must be unique. The game finds a weapon's hands, sight and attachment points by joint name, so a
weapon model has these joints. On the AR-23 Liberator they are at:

| Joint | Position (x, y, z) |
|---|---|
| `IK_right` (the trigger hand) | 0.038, -0.094, 0.001 |
| `IK_left` (the support hand) | -0.074, 0.132, 0.051 |
| `sight` | 0, -0.1, 0.21 |
| `attach_muzzle` | 0, 0.385, 0.127 |
| `attach_mag` | 0, -0.187, 0.109 |
| `attach_optic` | 0, 0.068, 0.193 |
| `attach_underbarrel` | 0, 0.245, 0.035 |
| `attach_ammo` | 0, -0.13, -0.023 |
| `attach_paintjob` | 0, -0.035, 0.132 |
| `ejector` | 0.007, -0.139, 0.123 |

Put them where your gun's grip, sights and muzzle are. The `model-carbine` example has them at the Liberator's places.

## Meshes, LODs and shadows

A mesh is rigid on its own node, so parent a moving part's mesh to the joint that moves it. A skinned mesh puts each
vertex on the joint that weights it most.

A node's name says where its mesh draws:

| Name | Draws as |
|---|---|
| `body` | LOD 0 |
| `body_lod1` to `body_lod7` | that LOD |
| `body_shadow`, `body_lod1_shadow` | the shadow caster of that LOD |

The meshes of one LOD are drawn together, so a gun can be several nodes. Every LOD from 0 up needs a mesh. A LOD without
a shadow caster casts its own geometry, except the last of several, which casts none.

## Materials

`materials` names the material resource each of the model's material names draws with: a game material, or your own
copy of one in the same spec, such as a recoloured one ([copy](data.md#copy)). The model's UVs sample that material's
textures.

## Physics and animations

`physics` gives the unit a game unit's collision under its own name. Without it the engine gives the unit a default
body. For a weapon, name its base weapon's unit.

`animationsFrom` makes the unit play a game unit's animations, its bones and state machine. Their clips hold each joint's
whole local transform, not an offset from rest. So every joint those animations move must be a joint of your model,
with the same name and the same rest transform, or the build is refused with the joint named. Joints they do not move
can be anything.

## Limits

- Each vertex is rigid on one joint. Vertices are written with three UV sets (the first repeated when the file has
  fewer), as most of the game's weapons have them.
- The unit has no lights.
- A weapon's attachments (magazine, muzzle, optic) are still its base's units, drawn at your `attach_` joints.

Example: `model-carbine`, an original carbine of boxes with its charging handle on the `bolt` joint.
