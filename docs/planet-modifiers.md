# Custom planets: pick each planet's enemy, subfactions and surges

<!-- diataxis: how-to -->

**Custom planets** is a war you pick on the launcher's **04 GALACTIC WAR** page. It starts as All planets open, and
the same page then sets, planet by planet, who holds it and what fights there. For example: Malevelon Creek held
by the Automatons, with the Jet Brigade and a Hulk surge. Every HellForge player in a lobby you host plays with your
planets.

## Change a planet

1. Open **04 GALACTIC WAR** and pick **Custom planets** in the WAR tab (or press **Play Custom planets** on the page's
   banner).
2. Click a planet on the map, or type its name or sector in the search box and press Enter. The PLANET tab opens.
3. Under **ENEMY**, pick Terminids, Automatons or Illuminate. **As the war has it** keeps the war's owner.
4. Under **SUBFACTIONS**, untick **As the war has it**, then tick the ones you want. Ticking none leaves the planet
   without subfactions.
5. Under **ENEMIES**, set a group to **Surge (x10)** or **None**. **As usual** leaves it as the war has it.

Each change is saved at once. A gold ring marks a changed planet. **Back to the war's own** undoes one planet, and
**Reset all** undoes every planet.

With another war picked, the page shows that war as it is and keeps your planets for later. A surge is a planet
modifier, not a faction: give the planet its enemy first, then that enemy's groups appear under ENEMIES.

While you play, the game picks a change up within a minute (it asks for the war's status about once a minute). Between
sessions, the next session starts with it.

## What each choice does

| Choice | In the game |
|---|---|
| An enemy | The planet is that enemy's, as a liberation that starts at 0%. A defense, attack or joint operation there goes. So do the modifiers that stop a planet from offering missions (Calypso's, Meridia's and the others). |
| Subfactions | The planet's missions spawn that subfaction's enemies, and the planet's card shows it. Only the planet's enemy's subfactions apply; Heavy SEAF Presence works with any enemy. |
| Surge (x10) | Squads with that group's enemies are ten times as likely, as the live war's HULK SURGE. Hulks, Devastators and Tanks show the live war's card on the planet; the others don't show on the card. |
| None | Squads with that group's enemies are left out. |

Some rules the game sets:

- **Home worlds keep their owners.** Super Earth stays Super Earth's, and Cyberstan stays the Automatons'. You can
  still change Cyberstan's subfactions and enemies.
- **An Illuminate planet always has one Illuminate subfaction.** Every Illuminate squad needs one, so a planet with none
  gets Invasion Fleet.
- **A planet holds at most 32 effects.** The war's own count too; past 32 the rest are left out.
- **A group joins only its own enemy.** Chargers on an Automaton planet do nothing, and the page lists only the planet's
  enemy's groups.

## In co-op

The host's planets count. When you host with Custom planets, every HellForge player who joins your lobby is served
your planets from the moment they join, whatever war they picked, and gets every change you make while they're there.
When they leave, their own war comes back. A host playing another war sends no planets.

A host on a HellForge version without planet modifiers sends none, so its joiners keep their own. Each player's game
builds its missions' enemies from its own backend's war, which is why the host's planets are sent to everyone.

## Where they're kept

The installation's `planet-modifiers.json`, beside `launcher-settings.json`:

```json
{
  "planets": [
    { "index": 196, "name": "Malevelon Creek", "faction": "automatons", "subfactions": ["jet-brigade"],
      "enemies": { "hulks": "surge", "tanks": "none" } }
  ]
}
```

| Field | What it is |
|---|---|
| `index` | The planet's index in the war. `name` is only for people reading the file. |
| `faction` | `terminids`, `automatons` or `illuminate`. Left out: the war's owner. |
| `subfactions` | Subfaction keys. Left out: the war's own. An empty list: none. |
| `enemies` | A group's key and `surge` or `none`. |

`HellForge.Cli planets --catalogue yes` lists every key. The same command changes planets for other launchers and
scripts ([external-launchers.md](external-launchers.md)):

```text
HellForge.Cli planets --planet "Malevelon Creek" --faction automatons --subfactions jet-brigade --enemies hulks=surge,tanks=none
```

A file that doesn't parse is ignored and the last good one stays. The session's `backend/planet-modifiers.log` says
what was read, what a host sent, and any choice that couldn't apply.

## How it works

The session runner starts the backend with the all-planets war and the installation's `planet-modifiers.json`. The
backend changes the war it serves after a [war script](war-script.md)'s additions:

- **Subfactions** are galactic war effects of type 40 (enemy tags), with their war table flags (type 71).
- **Surges and None** are type 72 effects: the spawn weight of one enemy family in squad compositions, x10 (1000) or
  left out (-100). The families come from `mappings/director.json`.

The map's faction emblems are the game's own, read from your installation's game data the first time the page opens and
kept in its `temp\war-emblems.json`; HellForge ships none. Without a game copy the map shows coloured dots.

`scripts/ExportPlanetModifiers.py` builds the catalogue (`data/planet-modifiers.json`), and
`scripts/ExportPlanets.py` builds the planets' names, positions and supply lines (`data/planets.json`). Planet names
come from the community data at [helldivers-2/json](https://github.com/helldivers-2/json) (MIT).
