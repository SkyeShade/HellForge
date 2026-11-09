# Crash guards in the renderer

<!-- diataxis: explanation -->

The game's renderer crashes in two places on build 25480438. Both happen most on a slow PC under Direct3D 11
(`--use-d3d11`). The loader guards both from the start of the session (`native/mod-loader/render_guards.c`). Each
guard sends the crashing case to the path the game already has, so it adds no new drawing or skipping.

| Crash | What the game does | The guard |
|---|---|---|
| `helldivers2.exe+0x732040` | A material still binds a texture that has been freed. Its slot reads `0xFFFFFFFF` and the binding loop reads entry -1 | A freed slot takes the null-texture path: the slot is bound empty. Four lookups are guarded: three in the binding loop, one in the view lookup |
| `helldivers2.exe+0x626296` | The job pool (8192 jobs) is full for a moment. Its allocator gives up after 33 spins without writing the new job's handle, and the caller reads the handle anyway | The thread waits 1 ms at a time until a worker frees a job, for up to 10 s. After that the game fails as it did before |

- The guards check the game's bytes at every site first. On another build they stay off.
- Freed slot indices are never reused, so a stale handle always reads `0xFFFFFFFF`.
- `modloader.log` says when a guard caught something: `render guards: ... drawn as empty`, or `the job pool ran full`
  with the longest wait.
- Not guarded: copy and command lookups (no null path), and why the textures are freed while still bound.

The analysis behind each guard (the code at every site, and why the fallback is safe) is in the comments of
`render_guards.c`. Its test, `native/mod-loader/test/render_guards_test.c`, runs every guard on a synthetic image.
