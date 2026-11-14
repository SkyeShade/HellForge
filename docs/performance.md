# Performance fixes in the engine

<!-- diataxis: explanation -->

For contributors and testers: the changes the mod loader makes so the game, and HellForge itself, do less work in
every session. Each fix comes from measurements on Complex Operations' 16 km maps, where the game's costs are largest.
Each one keeps the game's results and changes only how much work it takes to reach them.

## Turning a fix off

Every fix is on by default. The environment variable `HELLFORGE_PERF_OFF` turns fixes off for one session:

- by name, separated by commas: `HELLFORGE_PERF_OFF=safe-read`;
- all of them: `HELLFORGE_PERF_OFF=all`.

`modloader.log` then has `performance fix <name>: off (HELLFORGE_PERF_OFF)`. A tester who suspects a fix can rule it
out this way without another build.

## The fixes

| Name | What changes | Measured |
|---|---|---|
| `safe-read` | HellForge's own reads of game memory, and every mod's `HfApi.read`, copy up to 256 bytes in place instead of a system call | The living world's tick on a 16 km map: 1.8 ms to 0.5 ms a frame, together with its own slicing |
| `speedtree-empty-cells` | Forest cells with no trees stay out of every view, also those that hold the world origin | 16 km map: render-thread waits over 3 ms fell from 343 to 0; a fight's 0.1% low rose from 43.4 to 51.7 fps. Normal size: about 0.04 ms a frame to gain |

## safe-read

The loader reads game memory thousands of times a frame: its event polls, its readers and every mod's `read`. Each
read used to be a `ReadProcessMemory` call, a system call even within the game's own process.

- A read of up to 256 bytes is now a plain copy (`native/mod-loader/safe_read.c`). A larger read keeps the system call.
- A copy that touches memory it cannot read faults. A vectored exception handler turns the fault into the failed read
  that `ReadProcessMemory` returned, so callers see the same answers.
- A guard page that the copy touches gets its guard back, so a thread's stack still grows.
- The loader's other fault handlers pass such a fault on: the behaviour fault notes, the moved-state guard and the
  field watch.

## speedtree-empty-cells

The forest renderer keeps a grid of cells and culls them for each view: the camera's, and each shadow tile's. A cell
with no trees now gets an empty box that every view skips (`native/mod-loader/perf_speedtree_empty_cells.c`).

- **What changes.** The game gave a cell missing from its forest table an all-zero box: a point at the world origin.
  Any view that holds the origin passed every such cell. Each one became an empty entry that the view's later passes
  walked.
- **Why the results stay the same.** The new box is the one each cell is built with, and the one a cell with no trees
  gets. The cull already skips it. Views away from the origin rejected these cells already, so every view now treats
  them alike. Nothing is drawn from a cell with no trees.
- **Measured.** On a 16 km map the sun's static shadow tiles at the origin walked about 20,000 empty cells. The render
  thread then waited 5-9 ms on 4 of every 64 frames.

| 16 km map | Without | With |
|---|---|---|
| Render-thread waits over 3 ms, normal play | 343 | 0 |
| Fight 0.1% low | 43.4 fps | 51.7 fps |
| Normal-play 1% low | 69.7 fps | 79.7 fps |

- **Normal-size maps.** There the same work cost about 0.04 ms a frame, so the gain is small. The fix itself was not
  measured at that size.
- **Limits.** It patches 31 bytes of the exe, build 25480438 only; any other build keeps the game's code, with a line
  in `modloader.log`. A view that holds the origin used to keep a cell's old entry after its trees left the forest
  table. It now drops that entry, as the other views do.
