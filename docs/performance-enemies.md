# Performance fixes for many enemies

<!-- diataxis: explanation -->

For contributors and testers: the fixes from [Performance fixes in the engine](performance.md) for missions with
hundreds of enemies, as mods and the editor's spawn settings make them. Each one keeps the game's results and only
takes less time to reach them. The table there lists every fix, and [Turning a fix off](performance.md#turning-a-fix-off)
works for these too.

## motion-grid

The motion component's agent grids get 4,096 slots and 4,096 entries of HellForge's
(`native/mod-loader/perf_motion_grid.c`).

Each moving enemy is kept in one of two hash grids, by kind: 2 m cells and 10 m cells. Its boundary clip looks up the
cells around it to steer clear of the others, about three times a frame.

A grid has room for 256 enemies and probes its slots one after another until it finds an empty one. Past 256 moving
enemies no slot is empty. Every lookup then reads all 256 slots, and a new enemy takes over another's entry. At about
1,000 enemies the clip took 11% of the game thread, most of it in that probe.

### Why the answers stay the same

- The grid's code is the game's own: its insert, move, remove and the clip's lookups. Only the header that names the
  slots, the entries and their count changes, when the game sets a grid up for a new world.
- Each entry keeps 8 bytes inside the grid: its radius and its edge query (see edge-batch). The grid's 9 KB holds them
  for 1,152 entries, zeroed as the game starts them. Entries past 1,152 are marked taken, so the game never hands them
  out.
- Up to 256 moving enemies every lookup finds the same enemies as before, in fewer probes.

### Measured effect

Measured together with ai-query-chunks on a D11 Automaton mission with only Devastators, its seed pinned, Reflex on:
3 sessions with both fixes against 3 without, by enemies alive.

| Enemies alive | Without | With | Median frame | 1% low |
|---|---|---|---|---|
| 950-1,100 | 26.7 fps | 32.9 fps | 38.1 to 30.2 ms | 24.6 to 30.2 fps |
| 800-950 | 30.9 fps | 37.8 fps | 32.8 to 26.3 ms | 27.9 to 34.2 fps |
| 600-800 | 39.6 fps | 44.6 fps | 25.5 to 23.0 ms | 36.1 to 40.4 fps |
| 400-600 | 50.5 fps | 54.4 fps | 20.1 to 18.1 ms | 45.9 to 48.6 fps |

- At 950-1,100 the sessions do not overlap: 24.7-30.6 fps without, 32.0-35.8 with.
- Below 400 enemies the frame waits on Reflex and the GPU, so nothing changes.
- Stack samples at about 1,000 enemies put nearly all of the gain here: the clip fell from 11-13% of the game thread
  to 2-3%.
- The enemies still found, aimed at and hit the player: the test's immortal Helldiver took about as many hits as
  without the fixes.

### Limits

- Build 25480438 only. The places it relies on are pinned to that build's bytes; any other code leaves it off with a
  note in `modloader.log`.
- Grids set up before the fix installs keep the game's size. A world is set up when a level loads, so the ship and
  missions get the larger grids.
- Past 1,152 moving enemies of one kind, a new one takes over another's entry, as the game does past 256.

## ai-query-chunks

The AI's query jobs share their work out in small chunks (`native/mod-loader/perf_ai_query_chunks.c`).

Each frame the game sends the AI's raycasts, navmesh raycasts, overlaps and navmesh edge gathers to the worker
threads, 8 to 24 jobs each, each job a fixed share. The world update then waits for all of them before its first
pass. A share is a run of neighbouring requests, often one squad's, so a few shares take much longer than the rest.
The game thread waited for them, the overlaps without helping. At about 1,000 enemies that wait was 13.5% of the game
thread while most workers were idle.

- Each job now takes chunks of an eighth of a share from a shared count until none is left.
- When the game thread reaches the wait, it takes chunks too, then waits for the jobs still running.

### Why the answers stay the same

- Every request runs once, through the game's own job function, and writes only its own entry, as before. Only which
  thread runs which request changes.
- A batch is taken in chunks only when its shares run from the first request to the last without a gap. Any other
  batch runs as the game's.
- The jobs run at the same point of the frame as before, on the requests the game handed them. The wait still ends
  only when every chunk and every job is done.

### Measured effect

- At about 1,000 enemies the game thread spends about 2 ms a frame running query chunks itself and waits about 0.2 ms
  more.
- Without the fix its wait was 2.6 ms in the same sessions. In an earlier session it was 7 to 12 ms, when a few shares
  ran long.
- So it saves little when the shares are even, and guards against the frames where they are not.

### Limits

- Build 25480438 only, as above. Its edge gathers rely on edge-batch's pins too.
- The status-effect volume queries and the navmesh free-area queries stay the game's: they are small.
- `modloader.log` says once a minute how long the wait took and how much of it the game thread spent on chunks.
