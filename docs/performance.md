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

## safe-read

The loader reads game memory thousands of times a frame: its event polls, its readers and every mod's `read`. Each
read used to be a `ReadProcessMemory` call, a system call even within the game's own process.

- A read of up to 256 bytes is now a plain copy (`native/mod-loader/safe_read.c`). A larger read keeps the system call.
- A copy that touches memory it cannot read faults. A vectored exception handler turns the fault into the failed read
  that `ReadProcessMemory` returned, so callers see the same answers.
- A guard page that the copy touches gets its guard back, so a thread's stack still grows.
- The loader's other fault handlers pass such a fault on: the behaviour fault notes, the moved-state guard and the
  field watch.
