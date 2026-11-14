# Performance fixes in level generation

<!-- diataxis: explanation -->

For contributors and testers: the fixes from [Performance fixes in the engine](performance.md) that run while the game
generates a level. Each one leaves the level the same and only takes less time or memory to make it. The table there
lists every fix, and [Turning a fix off](performance.md#turning-a-fix-off) works for these too.

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

## overlap-grid

While a mission is generated, op 0x2B places stamps. It tests each candidate stamp against every record its item has
placed so far, so the work grows with the square of an item's records. A grid of the records' boxes now gives the test
only the records near the candidate (`native/mod-loader/perf_overlap_grid.c`).

- **What changes.** Each of the operation's eight jobs gets a grid when it zeroes its record buffer. The test's loop
  then visits the grid's candidates in index order instead of every record.
- **Why the results stay the same.**
  - The loop's own box test, and everything after it, still runs on each candidate in the game's order.
  - A job writes a record once, at the end of its list, and never changes it. The grid adds records as they appear and
    starts again for a new item or job.
  - The grid widens every box by 4 m. The loop's float rounding passes boxes at most 0.75 m apart, for coordinates up
    to 2^20 m.
  - A record with a larger or non-finite coordinate, or a very large box, is a candidate for every test.
  - So every record the loop would pass is a candidate. The records it no longer visits are ones it rejects.
- **Co-op.** Each machine generates the same records with or without the fix, so a host and a client agree.
- **Measured.** On Complex Operations' 16 km maps the pass took about 80 s with the game's loop. With the grid, a test
  visited 8.2 records on average instead of 136.7, over 87.5 million tests.
- **Measured outside the game.** The game's loop and the grid ran on synthetic records. With 1,024 records over 600 m,
  a test took 0.6 µs instead of 2.4 µs.
- **Normal-size maps.** Not measured in game. Most items there hold a few records, under the 64 the grid starts at, so
  most tests keep the game's loop. Items with many records gain, during generation only.

Limits:

- It is pinned to game build 25480438: the whole test function, the job's start and every record writer. Another
  build keeps the game's code, with a line in `modloader.log`.
- The game's loop still takes an item under 64 records and a candidate box over 1,024 cells. It also takes candidates
  that are half the records or more, and that item's next 8 tests.
- Records crowded in one spot pay for the grid and still use the game's loop: about 7% slower in the synthetic test.
- When an item's records run past its job's buffer, every grid stays off until the next job starts.
- Complex Operations' own overlap patch uses the same bytes, so it no longer installs; its maps use this fix.
