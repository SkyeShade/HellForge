# Warbonds

<!-- diataxis: how-to -->

This chapter is for mod authors who want to add warbonds: pages of items players unlock in the Acquisition Center, each
item at a price in a currency you choose.

After reading it you will be able to:

- add a warbond to your mod with a `hellforge-warbonds.json` file, no code needed;
- put vanilla items and your mod's own weapons and stratagems on its pages;
- price its items in medals, requisition, samples or Forge Credits, and lock later pages behind what was spent.

You should know [how mods run](concepts.md) first. To sell your own weapons and stratagems, you also need them registered
by a code mod ([content](content.md)).

## Contents

- [What a warbond is](#what-a-warbond-is)
- [The file](#the-file)
- [Items](#items)
- [Pages and prices](#pages-and-prices)
- [What players see](#what-players-see)
- [Limits](#limits)
- [Where to go next](#where-to-go-next)

## What a warbond is

A warbond is a set of pages. Each page holds up to 10 items on a 5 by 3 grid. A player buys an item once with the
warbond's currency. What they spend on a warbond counts towards opening its later pages, as medals do on the game's own
warbonds.

HellForge serves a mod's warbonds beside the game's, after them in the list. Nothing about them is saved but what a player
bought, so taking a mod out leaves nothing behind but those purchases. Putting it back restores them, as long as its
warbonds keep their keys.

## The file

Put `hellforge-warbonds.json` beside your mod's manifest:

```json
{
  "format": 1,
  "warbonds": [
    {
      "key": "gold-arsenal",
      "name": "Gold Arsenal",
      "currency": "requisition",
      "pages": [
        {
          "items": [
            { "weapon": "yourname/your_mod/gold_liberator", "cost": 5000, "position": [0, 0], "size": [1, 2] },
            { "stratagem": "yourname/your_mod/gold_strike", "cost": 8000 },
            { "mixId": 1861407750, "cost": 1500 }
          ]
        },
        {
          "requirement": 10000,
          "items": [{ "weapon": "yourname/your_mod/gold_breaker", "cost": 12000 }]
        }
      ]
    }
  ]
}
```

| Field | Meaning |
|---|---|
| `format` | always `1` |
| `key` | the warbond's identity within your mod: 1 to 64 of `a`-`z`, `0`-`9`, `-`, `_` and `.`. Every id the game sees comes from your mod's id and this key, so never change it once players have bought from the warbond |
| `name` | its name, 1 to 64 characters |
| `currency` | what its items cost: `medals` (the default), `requisition`, `common-samples`, `rare-samples`, `super-samples` or `forge-credits` |
| `pages` | 1 to 15 pages, in order |

## Items

Each item names exactly one thing it sells:

| Field | Sells |
|---|---|
| `mixId` | an item of the game's catalogue, by its id |
| `weapon` | a weapon a code mod registers: by its `unit` for `add_weapon`, by its `name` for `register_entity` and `add_entity` |
| `stratagem` | a stratagem a code mod adds (`add_stratagem`), by its `name` |
| `mod` | with `weapon` or `stratagem`: the id of the mod that registers it, when it is not your own |

An item also has:

| Field | Meaning |
|---|---|
| `cost` | its price in the warbond's currency, 1 to 1,000,000 |
| `position` | its top-left cell `[x, y]` on the 5 by 3 grid, from `[0, 0]`; without one it takes the first free cells, row by row |
| `size` | the cells it covers `[width, height]`; `[1, 1]` by default |
| `repeatable` | `true` to sell it again after each purchase (for consumables); `false` by default |
| `key` | needed only when two items of a warbond sell the same thing: a name for each, 1 to 64 characters |

A weapon or stratagem of yours sold on a warbond is not in a player's armory until they buy it, unless their profile has
everything unlocked. A weapon or stratagem no warbond sells stays as it was: in every player's armory.

An item is left out for the session, with a line in the backend's log, when the game could not hold it:

- a `mixId` the game's catalogue does not have;
- Super Credits, or premium content the player does not own (a premium warbond's item, a Superstore item, Steam DLC gear);
- a `weapon` or `stratagem` that is not registered this session, because its mod is not loaded, its premium content is
  not owned or the game's catalogue is full.

## Pages and prices

A page's `requirement` is what players must have spent on this warbond before the page opens. The first page has none.
Each later page needs at least as much as the page before it, and no more than the earlier pages cost in all, so it can
always be opened. A player who buys everything on the earlier pages has spent their whole cost.

A price is paid from the player's balance, so it cannot be above what a player can hold. Today that is 250 medals,
50,000 requisition, and 500 common, 250 rare and 100 super samples. HellForge leaves such an item out of the session and
says why.

## What players see

A mod's warbond is listed in the Acquisition Center after the game's warbonds. Its pages open and its items buy and
unlock as the game's do, and players with everything unlocked have bought it all.

Not done yet: the game draws a warbond's name, picture and price icon from its own data. Until HellForge provides
them, a mod's warbond shows a placeholder card and title, and every price shows the medal icon whatever its currency.
The amounts and the currency taken are right. Your `name` is kept for when HellForge shows it.

## Limits

- The game lists 32 warbonds. It has 25 of its own, so mods share the 7 places left. Later warbonds, in load order,
  wait until there is room, with a line in the backend's log.
- 15 pages a warbond, 10 items a page, and a 5 by 3 grid. HellForge refuses a warbond over these limits, and the
  launcher says why.
- One currency a warbond. An item has one price.
- Warbonds are read when a session starts. A mod added while the game runs brings its warbonds at the next start.

Each player's warbonds are their own profile's, so warbonds take nothing from co-op: friends need the same mods only for
the weapons and stratagems themselves.

## Where to go next

- [Content](content.md): register the weapons and stratagems a warbond sells.
- [Debugging](debugging.md): where the launcher's and the backend's messages are.
