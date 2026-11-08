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
| attachment, in `attachments` | `mixId`, `"standalone": true`, `weapons`, `level`, `price` |

- `class` is `primary`. Another class, or a missing or zero `package`, makes the entry malformed, and it is skipped.
- `package` is the weapon's own mastery package id, chosen by its mod.
- `level` 0 or 1 makes the attachment a default. A higher level sells it at that level.
- `price` is its requisition price. 0, or none in an older file, takes a price in line with vanilla's at that level
  (`RegisteredItems.PriceAt`).
- A standalone attachment is a child only of the standalone weapons in its `weapons`.

## What the game is served

| Path | What is added |
| --- | --- |
| `/Progression/items` | the weapon's item, then one child per standalone attachment |
| `/Progression/ProgressionPackages` | the weapon's mastery package, type 2 |
| `/Progression/inventory` | the weapon, its default children, and the children its progress holds |
| `/Progression` | its level, from its saved progress, at least 1 |

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

The levels take vanilla's shapes:

- **Defaults** are type 1 rewards of the first level (level 1, `requirementType` 3), as every vanilla weapon's are.
- **An attachment at level N >= 2** is an `unlockableItems` entry of level N: `mixId` its child, `amount` 1,
  `sortOrder` 0 and `cost` `[{"mixId": 3608481516, "amount": <price>}]`, requisition. `Purchases` sells it.
- An attachment past the last level is never offered, and the log says so.

The Armory needs both. Its item panel shows CUSTOMIZE only when some level has an unlockable (game.dll 0x191d6b0), and
the customize screen lists only rewards and unlockables (0x1493770). The mod loader logs a standalone weapon with
nothing for sale.

## Its level

A standalone weapon starts at level 1, so the Armory shows LVL 1 and the defaults are owned. On the first
`/Progression` request the backend runs `WeaponMastery.Earn` with no XP, which passes level 1 and holds its rewards, and
saves that in `modPackageProgress`. Without a profile it still serves level 1. Defaults stay held whatever the saved
progress says, so a default a mod adds later is held too.

## When a weapon is left out

The backend logs each one once:

- the vanilla weapon packages do not share one set of levels;
- its `package` is an id already in use, by the game or by another weapon, which would share one progress;
- the catalogue or the packages the game keeps are full, as for a copy.

## Premium content

A standalone weapon is not premium content of the game. Only its mod's own premium list
([modding/concepts.md](modding/concepts.md) "Premium content") keeps it out of the inventory.
