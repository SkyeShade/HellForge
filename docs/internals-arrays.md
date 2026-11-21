# Moved arrays and longer lists

<!-- diataxis: explanation -->

This page is for people working on HellForge itself. It explains two places where the mod loader gives the game more
room than it was built with, and how secondaries get paints. The rest of the mod machinery is in [internals.md](internals.md).

## Contents

- [The game state's arrays](#the-game-states-arrays)
- [The customize screen's list](#the-customize-screens-list)
- [Paints on secondaries](#paints-on-secondaries)
- [Where to go next](#where-to-go-next)

## The game state's arrays

The progression arrays sit inside one large state object, sized for what the game ships and reached by displacements
compiled into every access. `native/mod-loader/state_moves.c` allocates the state larger, puts the arrays past its old
end (a fixed distance from P) and rewrites every displacement, listed in `state_move_sites.h`.

- **Package progress slots.** 256 slots of 0x18 bytes at P+0x4D4 become 1,024. One memset of P+0x4B4..P+0x1CE0 resets
  them, and a stub clears the new slots after it.
- **The item catalogue.** Count, definitions, hierarchy, category starts, index and statuses, P+0x1CE0..P+0xD5D50, move
  as one block, each field at its place for 40,960 items.
- **Capacity.** The caps, parse buffers and reset follow in the same writes, all or none. The backend serves more than
  4,096 items only when told the move happened (`capacity` in `registered-items.json`).
- **game.dll still decoding.** WinLicense decodes upward past the pin, so a site above it can briefly read as encoded.
  The move then retries each tick and holds setup_game (+0x4EE160, which allocates the state) until it
  moves or refuses. `HELLFORGE_MOVE_UNDECODED=n` fakes n such tries.
- **The guard.** With `HELLFORGE_MOVE_GUARD=1` the old slots hold a canary and the old catalogue's pages are no-access:
  a vectored handler logs each access once, by its address. Every such line is a site the list missed.

## The customize screen's list

A weapon's customize screen holds 64 cards, with its own fields right after them. The game's list builder appends with
no bound, so more cards would write over those fields. On game build 25480438, `native/mod-loader/customize_list.c`
gives each screen a longer list:

- Each screen gets a frame: a copy of its layout, with room for every card a weapon can have. That is the game's 64
  plus the 1,024 attachments and weapon patterns that mods share.
- The build runs on the frame, and the UI is handed the frame's list. The 75 instructions that reach the screen's
  fields are rewritten to reach the frame's.
- The UI shows 127 cards: its six loop bounds are one byte each. The screen keeps a copy of the first 64 for the game
  code that still reads it.
- Every site is checked against its reviewed bytes first. A write that fails puts back the sites already written and
  takes the hooks off again.
- `registered-items.json` says `"customizeCards":127` once the list is in place. Only then does the backend serve 48
  mods' patterns a weapon instead of 16.
- `HELLFORGE_CUSTOMIZE_LIST=0` keeps the game's 64 cards. `modloader.log` then has a `customize list: off` line, and
  the backend serves 16 mods' patterns a weapon.

## Paints on secondaries

The customize screen makes its cards only from a weapon's mastery package, and no secondary has one. So the loader and
the backend give each vanilla secondary a paint package of its own:

- `native/mod-loader/registry_paints.c` gives each secondary's customization record the paint slot (first in its
  slots, as on every primary), the Default paint as its default and a package id derived from the weapon's id.
- Mods' copies of a secondary inherit that record, and their package id is renamed with their other ids.
- `registered-items.json` lists these weapons in `paintItems`. The backend (`RegisteredItems.Paints.cs`) serves each
  package as a copy of the Liberator's without its rewards: the game's 12 paints at level 1, held with the weapon.
- No mastery track names a paint package, so missions give it no XP.
- A secondary that already has a package, or has no room for another slot or default, is left as it is.

The Armory's weapon panel (game.dll `0x191D040`) shows CUSTOMIZE only for a package with an unlockable item on some
level, so the Default paint is the package's free unlockable. The panel also asks for a package for primaries only
(`cmp r15d, 1` at `0x191D684`). So CUSTOMIZE stays off on secondaries until a mod removes that jump.

## Where to go next

- [internals.md](internals.md): staging, the load chain, the registries and the other parts behind the mod SDK.
- [Patterns](modding/patterns.md): what a mod's weapon paints can do, and how many a weapon takes.
