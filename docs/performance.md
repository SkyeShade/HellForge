# Performance fixes in the engine

<!-- diataxis: explanation -->

For contributors and testers: the changes the mod loader makes so the game, and HellForge itself, do less work in
every session. Each fix comes from measurements on Complex Operations' 16 km maps, where the game's costs are largest.
Each one keeps the game's results and changes only how much work it takes to reach them, except where its section
says what a player sees.

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
| `record-hash` | The destruction-record map spreads its keys over its slots instead of packing them into one run | A miss on a 16 km map: 2,316 probes to 11 |
| `ai-path-budget` | The game's AI path threads get 0.5 ms a step instead of 2 ms, and 2 ms again while path requests wait over 500 ms | The game thread's wait for the AI step on a 16 km map: 1.0-1.5 ms to 0.2-0.3 ms a frame at 117-209 enemies |
| `corpse-freeze` | A corpse at rest in the game's corpse ring leaves the physics world, as the game's disposal does to it later | A fight on a 16 km map: the corpses' terrain pairs 456 k to 49 k a window, median frame 18 to 15.5 ms, a trend in one direction |
| `render-job-copies` | On Direct3D 12, each render job's copy of the resource-version table is freed once the job is recorded | On a 16 km city map, the copies standing at a peak: 561-930 MB to 4-9 MB |
| `render-version-rehash` | The Direct3D 12 renderer's resource-version tables are rehashed short when a level load left them far too large | A 16 km map, standing still: frame 12.5-13.0 ms to 8.5 ms, about 79 to 116 fps |
| `vehicle-box` | Each vehicle's oriented box is computed once per motion and steering pass, then copied for the other enemies | The motion pass at 213-225 enemies on a 16 km map: 6.8 ms to 0.5-0.6 ms a frame |
| `heap-trim` | The game's allocator caches are emptied once more when a load ends on the ship or a mission | 16 km maps: 0.5 to 5.5 ms a trim on the game thread, 10 to 72 MB given back each time; not measured at normal size |
| `scatter-cameras` | The level generator's water and fog cameras are left out of the engine's scatter update, so its node vectors stay the size of real views | 16 km map: 4,205 MB less memory on entering play. Normal size: not measured, an estimated 1/64 of that |
| `knn-grid` | The Hive Lord graph links its points through a grid instead of trying every pair, with the same result | Level generation on a 4 km map: 1,447 ms to 24 ms; on a 16 km map about 400 s saved |
| `spread-sort` | Map generation's location spreading sorts its candidates with the game's own steps, the comparison inlined | The sort alone, outside the game: 55 to 28 ms for 460,000 candidates, 0.61 to 0.32 ms for 7,200 |
| `tree-cell-limits` | After trees are taken out, SpeedTree's forest keeps both axes of the cut that limits each view to cells with trees | On a 16 km map, a view walked about 20,000 cells instead of 120,000 to 325,000 |
| `early-stamp-packages` | A mission's stamp packages are requested during its stamp planning, before the load step that asks for them | The wait for them on a 16 km map was about 13 s; the fix's own saving is not measured |

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

## record-hash

The game keeps a map from each destructible's handle to its destruction record. Several game systems look entities up
in it every frame: objectives, hint triggers and enemy spawners. Most of those lookups are misses.

- The map hashes a handle as handle times 1. A level's destructibles get consecutive handles, so they fill one long run
  of slots, and every miss walks to the end of that run.
- The map's setup now stores 0x9E3779B1 as the multiplier (`native/mod-loader/perf_record_hash.c`). Consecutive handles
  land spread over the table, and a miss stops at the first empty slot.

Why the game's results stay the same:

- The setup runs once per process, before the map holds a key. The loader writes before it runs, so every key is
  placed and found with the same multiplier.
- All 20 probes of the map and the game's shared insert read the multiplier from the map. The map is only filled and
  cleared, never erased from, so each lookup gives the same answer under any multiplier.
- Slot positions stay inside the process. Peers exchange record numbers, which follow spawn order, so a host and a
  client with or without the fix agree.

Measured on a 16 km map with 27,303 keys: a miss took 2,316 probes and a hit 143; with the multiplier, 11 and 2. With
the map full as well, its lookups had cost 11 of the 14.4 ms world update a frame. The effect on a normal-size mission
is not measured: it has fewer keys, so its runs and its saving are shorter.

Limits:

- It works only when the loader starts before game.dll's plugin setup, as the physics pools need too. Otherwise
  `modloader.log` says it is not installed, and the session keeps multiplier 1.
- It is pinned to game build 25480438 and to every reader of the multiplier. Another build leaves it off.
- A mod that checks the setup's original bytes at that store sees the new multiplier there.

## ai-path-budget

Each frame the game steps its Havok AI world on a worker thread, and the next frame's game thread waits for that step
to end. While path requests are queued, the step hands the AI's four path threads a time budget and sleeps until they
use it up: 2 ms, the game's own setting. HellForge sets it to 0.5 ms (`native/mod-loader/perf_ai_path_budget.c`).

- The game thread's wait for the step is shorter by about the time cut from the budget.
- Path searches and navmesh updates are the same work, spread over more steps. Enemies get the same paths, a few
  frames later.
- Every thread still passes its next yield point each step, so the work always moves on.
- When requests have waited over 500 ms, the game's 2 ms comes back until the wait is under 250 ms. The game keeps
  that wait itself.

There is one AI world for the whole session: the game makes it when it starts and frees it when it quits. The ship,
missions of every size and the moves between them all use it. HellForge reads the engine's record of it each frame,
on the thread that makes and frees it, in the game's update, where no step is running. The game clears the record
when it frees the world, so a set record is a live world.

Measured on Complex Operations' 16 km maps with 117 to 209 enemies:

- the wait fell from 1.0-1.5 ms to 0.2-0.3 ms a frame, and the mean frame was 1.5-2 ms shorter at 195-209 enemies;
- in another fight the wait fell from 1.2-1.3 ms to 0.5-0.85 ms, but the 0.1% low did not improve: it is not a fix
  for stutter;
- requests waited 6-12 ms on average in three 45 s windows and 157 ms in the fourth; the 500 ms limit was reached
  on 0 to 3 frames a window.

On a normal-size map the effect is not measured. The wait there is about 1 ms with 117 enemies and 0.03 ms with none,
so a mission with 30 to 90 enemies saves less.

Limits:

- It acts only on a world whose budget is the game's 2 ms or its own 0.5 ms; a value another mod set stays.
- It changes nothing on a game build other than 25480438, or when the game thread's update is not hooked.
- It runs on the host and in a friend's game alike. The budget only matters on a machine with path requests queued.

## corpse-freeze

The game keeps the 32 newest corpses of each machine in its corpse ring. Each one keeps its ragdoll bodies in the
physics world until 32 newer deaths push it out. In a fight, those resting bodies' contacts with the terrain are most
of the physics step's pair work.

- The game freezes a corpse only once it leaves the ring for disposal. When the corpse has moved less than 0.1 m² in a
  second, its actors go off and its ragdoll goes out of the physics world.
- The fix runs that same step on corpses still in the ring (`native/mod-loader/perf_corpse_freeze.c`): the same test,
  the same two engine calls and the same skip of the actors that the game's entry asks for.
- It runs on the game thread once a frame, after the game's own corpse update. It asks whether the unit still exists
  before every call.
- It leaves a corpse alone while the ragdoll pose sync still moves it. On a client, that lasts until the dead enemy's
  old entity is gone.
- It sends nothing over the network. Each machine freezes its own copies, as the game's disposal does.

What a player sees: a corpse at rest stays where it lies. Its bodies are out of the physics world, so it no longer
reacts to shots or explosions and no longer blocks anything. The game's own corpses behave this way once they sink.

Measured on a 16 km map, in a fight with 135 to 144 enemies online:

- The corpses' share of the deferred terrain pairs fell from 456 k to 49 k over four 30 s windows.
- The median frame went from 18 to 15.5 ms.
- A frozen corpse cannot be thawed, so this is a trend in one direction, not an on and off comparison.

A fight on a normal-size map costs about as much, and the freeze ran there without a fault. Its saving at normal size
is not measured.

Limits:

- A frozen corpse stays frozen. It stays where it lies even if the ground under it changes.
- Build 25480438 only. Another build leaves it off, with a line in `modloader.log`.

## render-job-copies

On Direct3D 12, a render job's copy of the renderer's resource-version table is freed as soon as the job has recorded
its commands. Before, every pooled job context kept its largest copy for the whole session.

### What the game does

- The renderer keeps a table of the current version of each dynamic buffer. It grows to the most buffers updated in one
  frame and never shrinks. Each entry is 24 bytes.
- Each frame's commands are recorded by parallel jobs, at most 128 commands a job. Each job takes a context from a pool
  and copies the whole table into it, empty buckets included.
- The pool grows to the most jobs in flight, often hundreds, and keeps every context and its copy until the game quits.

### What changes

- The job's call of the recording (`helldivers2.exe+0x6CA31C`) is sent through the loader. After the recording returns,
  a copy of 1,024 entries or more is freed with the game's own resize, on the job's thread.
- The next job that takes the context copies the table again, as it already did each frame, so only its allocation is
  new. The dispatch's emptying of the copy then has nothing to walk.

### Why the results stay the same

- Only the job's update pass and its recording read the copy. The job starts after its update pass, and after the
  recording it touches only its barrier lists and its done flag.
- The dispatch waits for every done flag before it empties the copies, and a context goes back to the pool only after
  that wait.
- The freed copy reads as empty, as after the dispatch's own emptying: a lookup in an empty copy already goes to the
  renderer's table.
- The copy's allocator is thread-safe, and the recording already allocates through it on the job threads.
- Direct3D 11 never runs this code, so nothing changes there. The patch is pinned to build 25480438: any other code leaves
  it off with a line in `modloader.log`.

### Effect and limits

- On a 16 km city map, the copies standing at a memory peak fell from 561-930 MB to 4-9 MB. Only jobs under way still
  held a copy: 3-14 against 219-809 jobs.
- On a normal map the table is smaller, so the saving is smaller. It is not measured.
- `modloader.log` names the longest copy given back so far, which is the table's size.
- A copy of over about 41,000 entries is a large allocation. On 16 km maps each job then maps fresh memory for its copy.

## render-version-rehash

The Direct3D 12 renderer keeps a hash table of each dynamic buffer's current version, and one of the buffers changed
since the last frame. Tables grow when a frame needs more room and never shrink. A session's largest burst, usually a
level load, sets their size for the rest of the session, the ship included.

Every recording job copies the whole versions table, empty buckets too, and empties its copy again. On a 16 km map
that is 56,114 entries with about 120 in use, copied about 50 times a frame: 45 % of the render thread.

What changes (`native/mod-loader/perf_render_version_rehash.c`):

- A hook on the device's command dispatch runs after it returns.
- A table of 4,096 entries or more is rehashed by the game's own rehash when its peak use over the last one to two
  seconds fills under an eighth of its buckets.
- It gets 4 buckets for each entry of that peak, plus 19.

Why the game's results stay the same:

- The rehash is the routine the game grows the tables with. It inserts every entry in use again, with its value.
- The dispatch already has the tables to itself, and the rehash runs on its thread before the call returns.
- Nothing keeps a pointer into a table between calls. Each job works on its own copy, which is emptied first.
- A table that needs more room grows again the game's way. The peak window keeps one from being rehashed back and
  forth.

Measured on Complex Operations' 16 km map, standing still, in alternating runs of one session: frame time 12.5-13.0 ms
to 8.5 ms, about 79 to 116 fps. A normal map is not measured. There the fix acts only once a load grew a table to
7,014 entries or more, the first size over the limit, and saves less in proportion.

Limits:

- Direct3D 12 only. Under Direct3D 11 the hooked dispatch never runs.
- It is pinned to build 25480438's bytes. Another build leaves it off, with a line in `modloader.log`.

## vehicle-box

Each vehicle's box is computed once per pass and copied for every other enemy that asks for it in that pass
(`native/mod-loader/perf_vehicle_box.c`).

Enemies walk around vehicles. Twice a frame, in the motion pass and in the AI steering pass, the game clips each moving
enemy's move against every vehicle. It asks the engine for each vehicle's oriented box before it tests the distance.
On a 16 km map with 39 scout walkers, the game asked for over 7,000 boxes a frame, of 39 vehicles.

### Why the answers stay the same

- The engine's box function writes only its 76-byte answer. The answer depends only on the vehicle's scene-graph world
  matrices and mesh records.
- Nothing writes those during the two passes. The game's unit calls there read, or queue a world command, such as a
  position or a flow event, that the engine applies after the game's update.
- The engine computes world matrices later in the frame, in its own world update. The animation jobs that run during
  the passes write local transforms and private copies only.
- Copies live inside one pass on its own thread and are dropped when the pass starts and ends. Nothing is kept between
  frames, levels or worlds, so a world freed at level end is never read.
- Only the clip's own call, with its flag, gets a copy. Other callers and other threads get the engine's answer every
  time.

### Measured effect

On a 16 km map with 213-225 enemies, 39 of them scout walkers, the motion pass went from 6.8-6.9 ms to 0.5-0.6 ms a
frame. The median frame went from 25 ms to 18 ms. A compare mode checked about 8 million answers, and none differed.

That measurement kept each box for a whole frame. This fix keeps it for one pass, so it computes each box twice a
frame instead of once: 78 boxes instead of 39, against more than 7,000 without it.

The effect on a normal-size mission is not measured. The cost is moving enemies times vehicles, so a mission with
scout walkers or an exosuit gains in proportion, and one without vehicles gains nothing.

### Limits

- Build 25480438 only. The four places it relies on are pinned to that build's bytes; any other code leaves it off with
  a note in `modloader.log`.
- Scout walkers and the exosuit carry the vehicle component. Whether war striders and factory striders do was not
  checked.
- A pass copies at most 128 boxes, and the game holds at most 64 vehicles. Past that, boxes are computed every time.
- A mod that hooks the box function itself turns the copies off: the call then no longer comes from the clip.

## heap-trim

When a load ends, the loader calls the game's own allocator trim once more (`native/mod-loader/perf_heap_trim.c`). The
game trims only as a load starts, so the memory that load frees stays cached until the next one.

- **What it changes.** The game's allocator, rpmalloc, keeps freed memory in caches: about 0.5 GB shared by all
  threads, and up to 62 MB for each thread. The game empties them as it enters the ship's or a mission's loading state.
- **When.** On the game thread's first update in the ship or a mission after another state. The first state the
  loader reads only sets the baseline, so a fix that installs during play waits for the next load.
- **What the trim does.** It is the function the game calls, through the same engine slot. It releases the game
  thread's caches and the shared ones; the pool workers and the render thread release their own on their next turn.
- **Why the game's results stay the same.** The trim runs on the thread and in the frame update the game calls it
  from, holding no allocator or queue lock. It releases only memory nothing uses. Another thread that needs a shared
  cache meanwhile waits on that cache's spin lock, as during the game's own trims.
- **Measured.** On Complex Operations' 16 km maps, nine trims after tile events took 0.5 to 5.5 ms on the game thread
  and gave back 10 to 72 MB each. The effect at normal size is not measured. Each trim writes a `heap trim` line to
  `modloader.log` with its time and the process's commit before and after.
- **Limits.** Only whole cached spans go back: partly used spans and the engine's own pools (Havok, the renderer's
  tagged heap) keep their memory. The first frame after a load pays the trim's time once. Memory the game asks for
  again after the trim is mapped again.
- **Pins.** The trim (`0x3245F0` in the executable), the calls it makes and both of `game.dll`'s calls of it must read
  as on build 25480438, and the engine slot must hold the trim. Otherwise the fix stays off with a `heap trim: off`
  line.

## scatter-cameras

While a level generates, two orthographic cameras look down on the whole 2048 m world: the water height map's (ops
0x0A and 0x0B) and the fog volumes' (ops 0x43 to 0x45). The engine's scatter update treated each as a view. HellForge
now leaves both out of it (`native/mod-loader/perf_scatter_cameras.c`).

- **What changes.** For such a view the update grew every scatter layer's node vector to the whole map and filled
  every cell with grass and small rocks. The engine never shrinks that vector, so the memory stayed all mission.
- **Which views.** A view is left out only while the generator runs one of those five ops, and only when it is
  orthographic with near 1, far 1001 and a box at least 2048 m wide. That is the water and fog cameras alone. The
  far-scatter bake's 256 m tiles and the in-game map's views keep their scatter. So does the ambient-occlusion bake,
  which runs before these ops.
- **Why the results stay the same.** Neither viewport has a SpeedTree pass, so they never drew this scatter. The next
  view's update stamps, fills and drops its own cells. A cell's scatter depends only on the cell and the layer's maps,
  which none of the five ops writes. The water height map and the fog volumes come out as before.
- **Measured.** Complex Operations made the same cut on its 16 km maps: 4,205 MB less memory on entering play.
- **Normal-size maps.** The vectors follow the map's area, so the saving should be about 1/64 of that: tens of MB,
  plus the time spent filling every cell once. It was not measured at that size.
- **Limits.** It hooks two functions of build 25480438, the scatter update in the exe and the generator's dispatcher
  in `game.dll`; another build keeps the game's code, with a line in `modloader.log`. A view rendered after its op
  has ended is not left out: no saving, no harm.

## knn-grid

During level generation the game builds the Hive Lord's movement graph: points about the map, each linked to up to 4
others within 18 m. Its linking pass (game.dll `0x177C9E0`) tries every pair of points, so its time grows with the
square of their number. HellForge replaces it with a grid search that writes the same bytes
(`native/mod-loader/perf_knn_grid.c`).

What the game's pass does, for each point i in order and each later point j in order:

- It takes the float distance `(xi - xj)^2 + (yi - yj)^2`, x first, one rounding a step. Height is not read.
- It skips the pair when that is over the radius squared, or when j's last link slot is taken.
- Otherwise it fills i's first free slot, or replaces i's largest distance, by fixed rules. A distance of 0 marks a free
  slot, so a duplicate point leaves its slot free.
- After i's pairs it turns the new distances into lengths and writes each link back into the neighbour's first free
  slot.

Why the result stays the same:

- A skipped pair changes nothing. The grid visits every pair the radius test can pass, in the game's order, through a
  copy of the game's per-pair code.
- Cells are the radius plus 1/1024 wide. When a float distance passes the test, its x and y differences are under
  the radius plus a few float steps, so both points lie in neighbouring cells. This holds in every rounding mode,
  with flush-to-zero too.
- A point with a coordinate that is not finite can give a NaN distance, which passes the test. Such points are paired
  with every point.
- The grid runs only on nodes as the graph's maker leaves them, all zero, and on a radius whose square is a normal
  float. Anything else, and graphs under 1,024 points, go to the game's own pass.

The native test runs the game's own 734 bytes against the grid on 900 random point sets. They include clustered,
duplicate, collinear, non-finite and far-off points, and pairs a float step either side of the radius. Every node byte
matches.

Measured in Complex Operations: a 4 km map's 80,746 points took 1,447 ms in the game's pass and 24 ms on the grid,
with the same nodes. A 16 km map's million points had taken about 400 s.

On a normal-size map the effect is not measured. The game builds the graph only on missions that need it. By area, a
2 km map has about a quarter of a 4 km map's points. In the native test the game's pass took 98-146 ms on 20,000
points and the grid 9-11 ms. Level 11's larger maps have more points and gain more.

Limits:

- It is pinned to the whole function on game build 25480438. Another build keeps the game's pass, with a line in
  `modloader.log`.
- Below 1,024 points the grid is no faster: both take about 0.4 ms. The game's pass runs there.

## spread-sort

While a map is generated, the game places each location at the nearest free candidate point. For every location it
sorts the whole candidate list by squared distance with its own quicksort. That sort calls the comparison through a
pointer for every pair. HellForge runs the same quicksort with the comparison inlined (`native/mod-loader/perf_spread_sort.c`).

- **What changes.** The game's sort is hooked. A call with the spreading's comparison and 12-byte records runs the
  copy; every other sort goes on to the game's code.
- **Why the results stay the same.** The copy takes the same steps as the game's sort: the same pivot, comparisons
  and swaps. Two candidates can be at exactly the same distance, so a different sort could order them differently.
  This one leaves them in the game's order, so every byte of the list is the same.
- **Tested.** The native tests run the game's sort, copied from build 25480438, beside the copy on random, repeated,
  sorted, reversed and NaN keys at every size up to 300 and larger. The outputs match byte for byte.
- **Co-op.** The map is the same with or without the fix, so a host and a friend need not both have it.

Measured outside the game, the sort alone takes half the time:

| Candidates | The game's sort | The copy |
|---|---|---|
| 7,200 (a normal-size map) | 0.61 ms | 0.32 ms |
| 460,000 (a 16 km map) | 55 ms | 28 ms |

In the game the effect is not measured. On a 16 km map these sorts took 3.7 s of a load, so about 1.8 s should go. A
normal-size map has about 7,200 candidates and a few dozen locations: some 10 ms a generation, a small gain.

Limits:

- It is pinned to game build 25480438: the whole sort and the comparison. Another build keeps the game's sort.
- A mod that hooks the game's sort still sees every call. The copy runs last in the chain, in place of the game's code.

## tree-cell-limits

SpeedTree files a level's trees in 64 m cells. Each view walks the cells of a far rectangle around the camera, cut to
the forest's extents on both axes. The game skips the cut when either axis has no range.

- When trees are taken out (a destroyed tree's record, any removal) or a tree leaves the last cell it shared, the game
  rebuilds the extents from its cell map. The rebuild never reads a cell's row, so the second axis collapses to row 0.
- From then on, every view walks its whole far rectangle for the rest of the level, on any map and for host and
  clients alike.
- Right after either rebuild, the fix sets the second axis from the same cell map, the way the game sets the first: the
  centres of the lowest and highest rows with trees (`native/mod-loader/perf_tree_cell_limits.c`).

### Why it keeps the game's results

- The new range holds every cell with trees, so no tree is cut that was drawn. Cells outside it are empty, so no tree
  appears that was not there.
- After a move, the game widens a collapsed axis only to the moved tree's row. Its trees in other rows are then cut and
  vanish; the fix keeps them.
- It is stateless. It runs where the game writes the same fields, on the render thread, so a level end, a new world
  and other threads see nothing new.
- It acts only on a call straight from the two rebuilds. A mod's hook in front of the forest's push leaves the game's
  extents.

### Measured effect and limits

- Measured with Complex Operations' version on a 16 km map: a view walked about 20,000 cells instead of 120,000 to
  325,000, which had cost a slow frame a second.
- Normal maps take the same rebuild after a destroyed tree. The far rectangle follows the camera, not the map, but the
  effect at normal size is not measured.
- It works with `speedtree-empty-cells`: that fix makes each empty cell a view walks cheap, this one stops the walk
  over cells outside the trees' rows. Their combined effect is not measured.
- The fix is pinned to build 25480438. Any other build leaves it off with a `modloader.log` line.

## early-stamp-packages

A mission load asks for its stamp packages (what the map's placed objects need) in one step, then waits
until all are loaded. HellForge asks for the same packages a few steps earlier, during the stamp planning
(`native/mod-loader/perf_early_stamp_packages.c`).

- **Why it helps.** The engine loads one package a frame. Asked early, the packages load while the game plans its
  stamps, so the later wait is shorter.
- **When.** The game's own request step builds its list only from the map's location types. The loader runs that same
  function right after the last load step that writes those types. From then on the list cannot change.

Why the game's results stay the same:

- The function is the game's own, with the same types and settings. Only its output list and its scratch memory are
  HellForge's, so the load's own data is untouched.
- A load request is not counted: asking for a package twice loads it once, and one release frees it. The game's step
  then finds the same packages already asked for.
- When the game's step has run, any package HellForge asked for that its list lacks is released. When the level ends
  or reloads first, all of them are.
- At most 480 requests are queued at once, below the engine's 512-entry queue. The rest follow on later steps.
- The briefing's drop map and the ship load no stamps, so nothing is asked for there. A friend's game loads its own
  copy the same way.

Measured on Complex Operations' 16 km maps:

- the wait for the stamp packages was about 13 s;
- asked early, 96 to 107 packages were loaded 24 to 28 s later, and the game's own list matched them.

The saving of the fix alone is not measured. On two normal-size missions the same wait was 1.5 s and 1.7 s, so the
gain there is at most that. The effect at that size is not measured.

Limits:

- It is pinned to game build 25480438: the load's steps, the request function, the engine's package functions and
  the two calls it redirects. Another build leaves it off, with a line in `modloader.log`.
- A mod that swaps one of the engine's package functions turns it off for that load.
- A load step added between the last type writer and the request, if it holds the types, turns it off for that load.
