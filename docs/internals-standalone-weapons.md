# Standalone weapons in the backend

<!-- diataxis: explanation -->

This page is for people working on HellForge itself. It explains how the local backend serves a mod weapon that names
no base weapon. A copy of a vanilla weapon is in [internals.md](internals.md) ("Weapons").

A standalone weapon references no other weapon. The backend authors its catalogue item, its children and its mastery
package from its own entry in `registered-items.json` (`RegisteredItems.cs`). Nothing is read from a single weapon, and
no other item's premium rule applies.

## The entries

The mod loader writes them beside the copy entries. An entry without `"standalone": true` is a copy, as before.

| Entry | Fields |
| --- | --- |
| weapon, in `items` | `mixId`, `"standalone": true`, `class`, `mod`, `unit`, `package` |
| attachment, in `attachments` | `mixId`, `"standalone": true`, `weapons`, `level` |

- `class` is `primary`. Another class, or a missing or zero `package`, makes the entry malformed, and it is skipped.
- `package` is the weapon's own mastery package id, chosen by its mod.
- `level` is the mastery level that gives the attachment. Level 0 means held from the start.
- A standalone attachment is a child only of the standalone weapons in its `weapons`.

## What the game is served

| Path | What is added |
| --- | --- |
| `/Progression/items` | the weapon's item, then one child per standalone attachment |
| `/Progression/ProgressionPackages` | the weapon's mastery package, type 2 |
| `/Progression/inventory` | the weapon, its level 0 children, and the children its progress gives |
| `/Progression` | its level, from its saved progress, as for a copy |

**The item.** `itemId` and `mixId` are the weapon's id, in category 1. It has no price, no level requirement and is not
consumable, like most vanilla weapons. Each field every vanilla weapon item has is written, since an unseen layout risks
the client's parse.

**Tags.** The class's tags are those every vanilla weapon's item shares. A weapon here is a category 1 item that a type 2
package names. In the game's data they differ (warbond tags, or none). The weapon then gets none, and the log says so.

**Children.** A child's `itemId` is the attachment, its `mixId` is `WeaponChild(attachment, weapon)`, and its
`parentId` is the weapon. The run of children follows the weapon's item, because the client links a child to the
parent before it.

**The package.** It lists the weapon in `requiredItems`, as every weapon package does. Its levels are the 25 that every
vanilla weapon package shares: `level`, `requirementType` and `requirement`. Each level also has `rewards`, `cost` and
`unlockableItems`, as every vanilla level does.

- An attachment at level N is a type 1 reward of level N. The client grants the rewards of the levels a mission reaches,
  and `WeaponMastery.Earn` does the same.
- Vanilla `unlockableItems` always carry a price, and these attachments have none, so they are not offered there.
- An attachment past the last level is never held, and the log says so.

## When a weapon is left out

The backend logs each one once:

- the vanilla weapon packages do not share one set of levels;
- its `package` is an id already in use, by the game or by another weapon, which would share one progress;
- the catalogue or the packages the game keeps are full, as for a copy.

## Premium content

A standalone weapon is not premium content of the game. Only its mod's own premium list
([modding/concepts.md](modding/concepts.md) "Premium content") keeps it out of the inventory.
