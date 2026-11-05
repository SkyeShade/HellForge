# Custom stratagem selection categories

<!-- diataxis: explanation -->

This page is for contributors and curious modders. It explains how HellForge adds headings of its own to the game's
stratagem selection screen (SDK 8): the native code involved, how it is tested, and what still needs checking in game.
To use headings in a mod, read [Content: selection headings](modding/content.md#selection-headings), which has the API
example and the limits, and see the `stratagem-categories` example mod.

The implementation is covered by the native synthetic fixture and was checked in game on build 25480438 on 2026-10-18,
with a heading showing its stratagems in `selection_order`. Controller navigation and saved loadouts still need an
in-game check.

## The native path

The game.dll image in the retained `F5FEE03DCFDB-20260929T172647Z-mission-host.hd2snap` was inspected offline. The
picker was identified by existing research in `artifacts/stratagem-registry/BOUNDS.md` and in the sibling HD2Runtime
repository's `research/stratagem-picker-F5FEE03DCFDB.json`. The relevant instructions:

| Address in game.dll | Role |
|---|---|
| `0x18D8710` | Selection-grid builder; filters available types before constructing candidates |
| `0x18D8BFD..0x18D8C15` | Candidate section from StratagemInfo `+0xB8`, type from `+0`, order from `+0x188` |
| `0x18D8C40` | Sorts 20-byte candidates using comparator `0x18D86E0` |
| `0x18D86E0` | Compares section, then order, then type, all unsigned |
| `0x18D43A0` | Appends an item key with section, enabled and blocked bytes; a section change starts a row |
| `0x18D2E85` | The realize pass calls the stratagem heading renderer `0x18CEC00` with the widget and first item key |
| `0x18CEC7F`, `0x18CECC7` | Header label at widget `+0x110`, count text at `+0x3C8` |
| `0x18CECF0..0x18CEDA9` | Counts selectable rows with catalogue entries, and their available subset |

### Grouping and order

A wrapper around the sort call changes only the candidates' section numbers and, for stratagems under a custom heading,
their order. The order becomes `HfStratagem.selection_order` biased by `0x80000000` (because the comparator is
unsigned), in place of the order a copy inherits from its base row's `+0x188`. The original comparator then groups and
orders them, breaking ties by type (call order).

Item keys, types, the vanilla sections' order, disabled flags, selection handling and saved identities follow the
existing native path. The rows themselves are not written: StratagemInfo `+0xB8` keeps a vanilla category, because
other consumers use it for HUD and ping presentation, and `+0x188` keeps the base's value.

### Section numbers and the 28-category limit

Each registered category gets a session handle. UI section numbers start at 5 and are assigned by signed sort order,
then by namespaced identity. Three native section arrays start at list `+0x9274C` (first row), `+0x927D0` (first card)
and `+0x92854` (section id), and the next field begins at `+0x928D8`. Each array holds 33 u32 entries. Five are
reserved for the vanilla sections, which leaves 28 custom categories, so registration refuses the 29th. Empty
categories create no entries.

### Headings and counts

The header wrapper keeps the native renderer and supplies the custom title text, plus counts grouped by selection
category. The counts use the native catalogue lookup `0x1372290` and availability predicate `0x136FC20`, as the original
renderer does. Vanilla counts exclude stratagems assigned to custom groups. Other screens share the renderer, so the
header thunk passes the list in RSI as a third argument, and the wrapper only changes a list whose keys and section
boundaries match the selection builder's latest output.

### Guards

Only two call sites are redirected, through nearby executable jump thunks. Before either is changed, `kCategoryPins`
checks the candidate layout, the sort call, the header call, the section-array writes, the widget offsets and the
called helpers. The existing stratagem installation guards its registry and bounds on its own.

If a category guard, allocation or write fails, HellForge logs a reason and leaves grouping on the vanilla path. If only
the heading call was redirected, that wrapper stays a pass-through. Categories do not raise the existing limits of 48
new stratagems and 150 picker candidates.

## Validation

The engine's native test of the categories runs with every engine build. It covers namespacing, shared definitions,
conflicts, bad and late registrations, older struct sizes, deterministic ordering, and selection order within a heading
(signed range, ties in call order, vanilla sections untouched). It also covers mixed native categories in one group,
unchanged keys, flags and HUD categories, custom and vanilla counts, reopening an empty picker, isolation of the shared
renderer, maximum registration, and rejection of mismatched native instructions.

In-game acceptance is still required:

1. Install a mod with two custom headings and several stratagems, including mixed offensive and defensive bases in one
   heading.
2. Check the names and counts, scroll between headings, and select with mouse and controller.
3. Reopen each loadout slot, save and reload the loadout, and check call-ins and ping and beam colours.
4. Test a loadout with just one nonempty custom category, and a run without any mod that uses categories.

## Hellpod crash investigated, 2026-10-18

The first reported hellpod crash was a native loadout-preview recursion that happened before custom category candidates
were built. In `helldivers2.exe.40780.dmp`, the category cache count and candidate storage were zero. The stack held 182
returns to `game.dll+0x18EA758`: the query at `0x18E9D50` kept retrying alternate preview 1 for slot 6, mode 0, context
0. Those frames held resource `0x5C14F27759DD3BE0` (G-8 Immolation). The access violation that followed, in the loader's
crash logger, was its stack probe running after the native recursion had exhausted the stack.

`loadout_preview.c` guards this query against repeating an active `(alternate, slot, mode, context)` on the same
thread. A cycle returns the zeroed 76-byte result the native query already uses for a missing loadout row. Ordinary
queries and distinct nested queries go through the original function. The entry, empty-result initialisation and
recursive-call bytes are checked before hooking. The regression fixture reproduces the cycle (ordinary, then
alternate, then the same alternate again) and checks termination, output bounds and that distinct queries are
preserved. Live validation is still needed; a missing preview part may stay empty instead of being retried.
