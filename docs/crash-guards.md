# Crash guards in the renderer

<!-- diataxis: explanation -->

The game's renderer crashes in two places on build 25480438. Both happen most on a slow PC under Direct3D 11
(`--use-d3d11`). The loader guards both from the start of the session (`native/mod-loader/render_guards.c`). Each
guard sends the crashing case to the path the game already has, so it adds no new drawing or skipping.

| Crash | What the game does | The guard |
|---|---|---|
| `helldivers2.exe+0x732040` | A material still binds a resource that has been freed. Its slot reads `0xFFFFFFFF`, and the binding reads entry -1 | A freed slot takes the path of the null handle: the slot is bound empty, or nothing is bound |
| `helldivers2.exe+0x626296` | The job pool (8192 jobs) is full for a moment. Its allocator gives up after 33 spins without writing the new job's handle, and the caller reads the handle anyway | The thread waits 1 ms at a time while workers finish jobs. It gives up as the game did when no job finishes for 0.5 s (the game quitting, or stuck) or after 10 s |

## What is guarded

Every lookup of a handle read from a material's data that has a null-handle path: eight in all.

- the material binding loop: textures, other views, buffers, and buffers and UAVs;
- the view lookup;
- the compute binding: buffers, views and structured buffers.

The guard acts only on `0xFFFFFFFF`. A slot that was freed and then reused would bind another resource; it would not
crash.

## What is not guarded

- **Constant buffers of a material** (`0x73197D`, `0x730701`). Their null path reads data inside the material, which
  is not a safe stand-in for a resource that was freed.
- **Copy, update and command lookups.** They take handles from the command the game is issuing, not from a material,
  and have no null path.
- **The cause.** Why a resource is freed while a material still binds it is unknown.

## In the log

`modloader.log` says what the guards caught, at most every 10 s:

- `render guards: ... bound as empty in the ...`: a freed resource was caught.
- `the job pool ran full`: how often, and the longest wait.
- `saw no job finish`: the wait stopped because nothing finished. At quit this is the game's own crash on quitting.
- `still full after 10 s`: the wait ran out during play. The game crashes next: please attach the crash report.

The analysis behind each guard is in the comments of `render_guards.c`. Its test,
`native/mod-loader/test/render_guards_test.c`, runs every guard on a synthetic image.
