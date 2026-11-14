/* The performance fixes (docs/performance.md), one file each, in their own list so the loader stays within its size
 * limit. Included by hellforge_modloader.c after render_guards.c: the fixes use its hook and write paths. */

#include "perf_speedtree_empty_cells.c" /* forest cells missing from the forest hash: the empty box */
#include "perf_record_hash.c" /* record-hash: the destruction-record map's hash multiplier */
#include "perf_ai_path_budget.c" /* the AI path threads' time a step; the game thread calls AiPathBudgetTick */
#include "perf_corpse_freeze.c" /* settled ring corpses frozen; the game thread calls PerfCorpseFreezeTick */
#include "perf_render_job_copies.c" /* Direct3D 12 render jobs give their version copies back */
#include "perf_render_version_rehash.c" /* the D3D12 version tables kept short */
#include "perf_vehicle_box.c" /* vehicles' boxes once per motion and steering pass */
#include "perf_heap_trim.c" /* the allocator's caches emptied once more when a load ends */
#include "perf_scatter_cameras.c" /* the generator's water and fog cameras left out of the scatter update */
#include "perf_knn_grid.c" /* knn-grid: the Hive Lord graph's neighbour pass on a grid */
#include "perf_spread_sort.c" /* SpreadLocations' candidate sort */
#include "perf_tree_cell_limits.c" /* SpeedTree's view cut kept after its forest is rebuilt */
#include "perf_early_stamp_packages.c" /* after perf_scatter_cameras.c: it pins part of the same dispatcher bytes */
