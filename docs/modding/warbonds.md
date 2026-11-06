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
- [The showcase](#the-showcase)
- [What players see](#what-players-see)
- [Limits](#limits)
- [Where to go next](#where-to-go-next)

## What a warbond is

A warbond is a set of pages. Each page holds up to 10 items on a 5 by 3 grid. A player buys an item once with the
warbond's currency. What they spend on a warbond counts towards opening its later pages, as medals do on the game's own
warbonds.

HellForge serves a mod's warbonds beside the game's, after them in the list. A player's profile saves only what they bought,
what they spent on each warbond and the warbonds they paid to unlock. Taking a mod out leaves those behind, and putting it
back restores them, as long as its warbonds and items keep their ids (see `key` below).

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
| `variant` | its look: `free` (the default), `premium` or `legendary`: the card's style and the tag under its name |
| `season` | the season line the game keeps with a warbond's name ("Season I" on Helldivers Mobilize), 1 to 32 characters; your mod's name by default. The Acquisition Center of this game build shows it on no warbond, the game's included |
| `art` | its picture: `{"image": "card.png"}`, a 2048 by 1024 PNG in your mod's folder; or `{"from": <id>}`, the art of one of the game's warbonds, by its id (the keys of `mappings/warbond-art.json`, or Helldivers Mobilize's `1929468580`); without it, a plain dark card HellForge draws |
| `unlock` | `{"currency": "forge-credits", "cost": <n>}`: players buy the warbond once, for 1 to 1,000,000 Forge Credits, before they can buy its items; without it, the warbond is open to everyone |
| `showcase` | the Helldiver and background beside its card: see [The showcase](#the-showcase); without it, the game's default |

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
| `cost` | its price, 1 to 1,000,000 |
| `currency` | what it costs, when not the warbond's `currency`: one of the same names |
| `position` | its top-left cell `[x, y]` on the 5 by 3 grid, from `[0, 0]`; without one it takes the first free cells, row by row |
| `size` | the cells it covers `[width, height]`; `[1, 1]` by default |
| `repeatable` | `true` to sell it again after each purchase: for a game item (a consumable) only, never a `weapon` or `stratagem`; `false` by default |
| `key` | needed only when two items of a warbond sell the same thing: a name for each, 1 to 64 characters. An item's id comes from its `key`, or without one from what it sells, so adding a `key` later or renaming what it sells starts it unbought |

A weapon or stratagem of yours sold on a warbond is not in a player's armory until they buy it, unless their profile has
everything unlocked. A weapon or stratagem no warbond sells stays as it was: in every player's armory.

An item is left out for the session, with a line in the backend's log, when the game could not hold it:

- a `mixId` the game's catalogue does not have;
- Super Credits, or premium content (a premium warbond's item or token, a Superstore item, Steam DLC gear), owned or not:
  only its own warbond sells it;
- a `weapon` or `stratagem` that is not registered this session, because its mod is not loaded, its premium content is
  not owned or the game's catalogue is full.

## Pages and prices

A page's `requirement` is what players must have spent on this warbond before the page opens. Every price counts
towards it as its number, whatever its currency, so keep a warbond's prices in one currency when its pages build on them.

The first page has none.
Each later page needs at least as much as the page before it, and no more than the earlier pages cost in all, so it can
always be opened. A player who buys everything on the earlier pages has spent their whole cost. When items are left out for
a session (below), a page needs no more than the pages before it sell that session.

A price is paid from the player's balance, so it cannot be above what a player can hold. Today that is 250 medals,
50,000 requisition, and 500 common, 250 rare and 100 super samples. HellForge leaves such an item out of the session and
says why.

## The showcase

Beside the warbond's card, the Acquisition Center shows a Helldiver in front of a background, as on the game's warbonds.
Each field is optional:

```json
"showcase": {
  "from": 610512218,
  "pose": 1929468580,
  "armor": 3605514351,
  "helmet": 1004735217,
  "cape": 752209840,
  "weapon": "27EE1ED8F6FB6356"
}
```

| Field | Meaning |
|---|---|
| `from` | a game warbond by its id (as for `art`): its background and its Helldiver, which the fields below change; without it, the game's default |
| `pose` | a game warbond by its id: the Helldiver stands in that warbond's pose |
| `armor`, `helmet`, `cape` | an item of the game's catalogue, by its `mixId`: the Helldiver wears it. Without one, it wears what `from` gives it. That is the player's own cape on most of the game's warbonds |
| `weapon` | an entity id, in hex: the weapon the Helldiver holds. The `entity` column of `arsenal.tsv` (see [Debugging](debugging.md)) lists every armory weapon's, and `HF_ENTITY_` in `hellforge_content.h` names the game's |

Custom armor shows here when your mod changes how one of the game's armors looks: name that armor's `mixId`. An item that is
not that kind of piece is skipped, with a line in the game's log, and the Helldiver keeps what `from` gives it. The
Helldiver stands where the game's do, and the camera is the game's.

## What players see

A mod's warbond is listed in the Acquisition Center after the game's warbonds. Its pages open and its items buy and
unlock as the game's do, and players with everything unlocked have bought it all.

Its card and title show your `name`, `variant` look and `art`, and the Helldiver beside it is your `showcase`. Prices show the icon of the warbond's
currency: medals, requisition, each kind of sample in its colour, or Forge Credits. The line with what was spent and a
locked page's line with what is left to spend name that currency too, in English.

A warbond with an `unlock` price shows the game's warbond button with that price, as the game's premium warbonds do. Players
buy it with Forge Credits, once, and then buy its items as on any warbond. Players with everything unlocked own it.

Your picture is the warbond's card as it is: the game writes no name over a card, so draw in the title or logo you want.
The name shows beside the card. The PNG may use any colour type; its transparency is dropped.

## Limits

- HellForge makes room for 128 warbonds in the game's list, where the game has 32. The game has 25 of its own, so mods
  share 103 places. Later warbonds, in load order, wait until there is room, with a line in the backend's log.
  The Acquisition Center's row of dots under the cards has 32; past 32 warbonds it shows the first 32, and its arrows
  still reach every warbond.
- Mods' warbonds sell 4,280 items in all, which is what a player's profile has room for besides the game's warbonds.
  A warbond that would go past that waits too.
- The first 99 mods' warbonds have their own name, look and art; any after them show the game's placeholder.
- 15 pages a warbond, 10 items a page, and a 5 by 3 grid. HellForge refuses a warbond over these limits, and the
  launcher says why.
- An item has one price, in one currency. The page's balance at the top shows the warbond's `currency`; an item in
  another currency shows its own icon and is checked against what the player holds of it.
- Warbonds are read when a session starts. A mod added while the game runs brings its warbonds at the next start.

Each player's warbonds are their own profile's, so warbonds take nothing from co-op: friends need the same mods only for
the weapons and stratagems themselves.

## Where to go next

- `examples/mods/warbond-example` (in `app/sdk/examples/mods` once installed): two warbonds with no code, one in
  requisition with its own card and an item in samples, one with a premium look bought for Forge Credits and its own Helldiver.

- [Content](content.md): register the weapons and stratagems a warbond sells.
- [Debugging](debugging.md): where the launcher's and the backend's messages are.
