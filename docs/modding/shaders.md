# Shaders

<!-- diataxis: how-to -->

This page is for mod authors who want effects the game's own materials cannot draw. A mod can bring its own pixel shader
in a data spec: HellForge copies one of the game's materials, puts your compiled shader in the copy, and gives you a unit
that draws it. Read [Data mods](data.md#data-specs) first.

- [A shader in a data spec](#a-shader-in-a-data-spec)
- [The hosts](#the-hosts)
- [Writing and compiling a shader](#writing-and-compiling-a-shader)
- [What a shader can use](#what-a-shader-can-use)
- [Getting it on screen](#getting-it-on-screen)
- [Checks and refusals](#checks-and-refusals)
- [Co-op](#co-op)

## A shader in a data spec

A resource with `shader` names a host. HellForge builds the materials and the unit it needs:

```json
{
  "format": 1,
  "patches": [{
    "archives": ["f4dc2361985c3026"],
    "resources": [
      { "shader": "decal", "name": "yourname/mod/rings", "dxbc": "shaders/rings.dxbc" },
      { "shader": "transparent", "name": "yourname/mod/glow", "dxbc": "shaders/glow.dxbc",
        "mesh": { "obj": "models/column.obj" } }
    ]
  }]
}
```

| Field | Meaning |
|---|---|
| `shader` | the host: `decal`, `transparent` or `refract` |
| `name` | the mod's own resource path, and the name of the unit you spawn |
| `dxbc` | the compiled shader, a `.dxbc` file in the mod folder |
| `mesh` | an OBJ, as for a [unit copy](data.md#copy). `transparent` and `refract` need one; `decal` takes none |

`<name>` is the unit and `<name>/material` its material. The `transparent` host also makes `<name>/root`, the root
material your code sits in. Two shaders on one host are two resources with different names: each copy is its own, and
neither touches the other or the game's material.

## The hosts

| Host | Draws | Reads | Room |
|---|---|---|---|
| `decal` | on whatever is already inside a 1 m box, ground and enemies included, so effects follow the terrain | the depth buffer and the normals | 11,688 bytes |
| `transparent` | a mesh that adds light to the scene, both walls | the depth buffer | 63,564 bytes |
| `refract` | a mesh that replaces what is behind it, near wall only | the finished scene and the depth buffer | 61,276 bytes |

Room is for the whole stage, with the game's signatures and reflection in it, so a shader has less than this. A shader
that does not fit is refused with the size it needs.

## Writing and compiling a shader

Each host has a header in `app/sdk/shaders` of an installation (`native/mod-sdk/shaders` in the repository). Include the
host's header, write `Out main(In i)`, and end with the host's output call. A decal that draws one ring closing in:

```hlsl
#include "hellforge_shader_decal.hlsli"

Out main(In i)
{
    HfBox b = hf_box(i);    // the pixel in the box; no pixel outside it
    float ring = smoothstep(0.05, 0.0, abs(b.radius - (1.0 - frac(hf_time() * 0.5))));
    return hf_decal(float3(0, 0, 0), 0.5 * ring, float3(10, 3, 30) * ring);
}
```

HellForge ships no shader compiler. Compile with `fxc`, from the Windows SDK, for shader model 5.0, keep reflection (no
`/Qstrip_reflect`), and ship the `.dxbc` with the mod:

```text
fxc /T ps_5_0 /E main /I <the shaders folder> /Fo shaders\rings.dxbc shaders\rings.hlsl
```

HellForge reads only the `.dxbc`, so the `.hlsl` is yours to ship or not.

## What a shader can use

The headers declare the host's inputs and outputs and name what is useful, and their comments are the reference:

| Header | Gives you |
|---|---|
| `hellforge_shader.hlsli`, in every host | `hf_time`, `hf_delta_time`, `hf_camera`, `hf_camera_forward`, `hf_target_size`, `hf_view_depth` |
| `hellforge_shader_decal.hlsli` | `hf_box`, `hf_box_size`, `hf_progress`, `hf_emissive`, `hf_decal` |
| `hellforge_shader_transparent.hlsli` | `hf_uv`, `hf_around`, `hf_facing`, `hf_scene_depth`, `hf_soft`, `hf_opacity`, `hf_transparent` |
| `hellforge_shader_refract.hlsli` | `hf_uv`, `hf_normal`, `hf_screen`, `hf_scene`, `hf_bend`, `hf_strength`, `hf_refract` |

A shader has only the bindings of the game's stage, at the stage's registers. There are no textures or constants of
your own, and anything else is refused. Time and depth animate most things, and noise you compute yourself replaces a
texture.

A mod cannot pass a shader a number yet, but it can change a unit's scale, and a shader can read that back. On a decal,
`hf_progress(from, to)` turns the box's height into 0 to 1, so growing the box from 80 to 120 m runs an effect once.

Keep flashes under three a second: a large flash faster than that is a risk for players with photosensitive epilepsy.

## Getting it on screen

A shader draws when its unit is in the world. The SDK has no call for spawning a unit yet. For now a code mod does it
on the game thread through [game memory](hooks.md): the world's spawn call, with the resource's name hashed and the
unit's package loaded. A `decal` box is 1 m across, so scale it to the area it covers, and leave its rotation alone so
`uvw.z` stays up.

## Checks and refusals

`HellForge.Cli build-data --mod <folder>` builds the spec and stops at the first problem, as a session does. HellForge
reads your compiled shader and compares it with the game's stage, so these are refused with the reason:

- not `ps_5_0`, or doubles and other optional GPU features
- an input that is not the stage's, in another register, or bigger than the stage's
- an output left out: every output of the stage must be written, and the headers' output calls do
- a binding the stage does not have, at another register or of another kind, a cbuffer bigger than the stage's, or
  global variables outside a cbuffer
- no reflection in the file

The hosts are pinned to the game build HellForge supports. When a game update changes one, the spec is refused until
HellForge is updated.

These checks are for you, not for players' safety: a compiled shader cannot be read for what it does.

## Co-op

A shader is code HellForge cannot check, so a mod with one counts as unchecked for [mod sync](tools.md). Players who
join get it only from a Steam friend, or when they press TRUST for it, which takes a sandboxed game with a stranger.
Its effects are drawn on each machine by the code that spawns its units, so a mod has to arrange for other players to
see them.
