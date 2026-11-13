# War scripts: your own major orders, dispatches, rewards and planet modifiers

<!-- diataxis: reference -->

A war script is a JSON file that adds to the galactic war HellForge serves. It can hold:

- major orders and personal orders, with your own title and briefing;
- dispatches (the news feed);
- reward mail that every player can claim once;
- galactic war effects (planet modifiers such as an extra stratagem, cooldowns, subfactions) and additions to the war
  status (where those effects go, defense events, campaigns).

The war every HellForge player shares comes from a war server ([hellforge-war.md](hellforge-war.md)), and a war script
adds to it as to any other war. The file lives on each player's PC, so everyone in a lobby should use the same file.

Nothing here has been tried in game yet. Every reply was run through the game's own parsers in an emulator, but which
task lines and modifiers the game shows still needs testing (HD2Mods `research/galactic-war`, "Unknowns / in-game
tests").

## Where it goes

Put `war-script.json` in your HellForge folder, beside `live-war.json`. The next session serves it with whichever war you
picked (HellForge war, live, frozen or all planets open). With the HellForge war, the server's major orders and
dispatches come first and the script's follow. HellForge reads the file again whenever it changes, and the game polls every
30 to 60 seconds, so an edit shows up on the ship within a minute. A file that doesn't parse is ignored and the last good
one stays; the session's `backend/war-script.log` says which. Comments (`//`) and trailing commas are allowed.

For the backend alone, pass `--war-script <file>`.

## Times

Times are war seconds, the clock the war runs on (`GET /WarSeason/801/timeSinceStart`, and `status.time` in
`live-war.json`). A major order ends at `endsAt` (war seconds) or after `expiresIn` seconds; one that has ended is no
longer served. A dispatch shows once the war reaches its `published` time, and mail once it reaches `sentAt`.

## The file

```jsonc
{
  "majorOrders": [
    { "id": 9001, "title": "HOLD THE LINE", "brief": "Automatons are massing on the border. Thin their ranks.",
      "endsAt": 84900000,
      "tasks": [ { "type": 3, "values": [3, 0, 100000], "valueTypes": [1, 2, 3] } ],
      "progress": [42000],
      "rewards": [ { "mixId": 897894480, "amount": 25 } ] }
  ],
  "personalOrders": [
    { "id": 7001, "title": "", "brief": "", "expiresIn": 86400,
      "tasks": [ { "type": 3, "values": [2, 0, 250], "valueTypes": [1, 2, 3] } ],
      "rewards": [ { "mixId": 897894480, "amount": 5 } ] }
  ],
  "dispatches": [
    { "id": 1, "published": 84500000, "message": "<i=3>VICTORY ON MALEVELON CREEK</i>
The <i=1>Automatons</i> are in retreat." }
  ],
  "mail": [
    { "id": 501, "sentAt": 84500000, "message": "The major order is won. Thank you, Helldiver.",
      "items": [ { "mixId": 897894480, "amount": 25 } ] }
  ],
  "effects": [
    { "id": 90001, "gameplayEffectId32": 0, "effectType": 32, "flags": 0, "nameHash": 1181673243,
      "descriptionFluffHash": 0, "descriptionGamePlayLongHash": 1569800370, "descriptionGamePlayShortHash": 2151089975,
      "valueTypes": [4, 1], "values": [1982351727, 0] }
  ],
  "status": {
    "planetActiveEffects": [ { "index": 126, "galacticEffectId": 90001 } ]
  }
}
```

### Major orders and personal orders

| Field | What it is |
|---|---|
| `id` | A number of your own. Keep it while the order runs: the game never lowers the progress it showed for an id, so to start over, use a new id. |
| `title`, `brief` | Free text, up to 255 and 1023 bytes. |
| `endsAt` or `expiresIn` | When it ends (see [Times](#times)). |
| `tasks` | Up to 8: `type`, and `values` with their `valueTypes`, pair by pair. The game writes each task's line itself from these. |
| `progress` | One number per task (0 when left out). |
| `rewards` | Up to 3 `{mixId, amount}`, shown on the order. |
| `flags` | 0 unless you know better. |

Up to 16 of each are served. Task value types: 1 faction (2 Terminids, 3 Automatons, 4 Illuminate, 0 any), 3 the goal,
4 an enemy, 5 an item, 9 a difficulty (likely), 12 a planet (likely).

| Task type | What it is |
|---|---|
| 2 | Collect an item (value type 5). Seen in the live war. |
| 3 | Kill enemies (value types 1, 4). Seen in the live war. |
| 9 | Operations at a difficulty. Seen in the live war. |
| 4, 7, 11, 12, 13, 15 | The community's names, not tied to the game's code yet: complete objectives, complete missions, liberate a planet, defend planets, hold a planet when the order ends, liberate more planets than are lost. |

Progress is what the file says; HellForge doesn't count it from missions yet. A **major order's reward isn't paid by the
order itself** (the game has no claim for one): send it as mail. A personal order pays its rewards when the game reports
every task done, once per profile.

### Dispatches

`id`, `published` and `message`. The message is free text; `
` breaks a line, `<i=3>...</i>` is a white heading and
`<i=1>...</i>` yellow. The game shows up to 16, newest first.

### Mail: rewards for everyone

Each mail is claimed once per profile, from the in-game mailbox, whether or not that player played for it. `items` are
`{mixId, amount}`. `897894480` is medals (the reward item major orders name; it lands as the medals you hold).
Requisition is `3608481516`. Super Credits and anything sold for real money are never granted, whatever the file says.
A profile remembers what it claimed (`hellforgeClaimedMail` in its progression).

### Effects and the war status

`effects` are galactic war effects, served with the game's own (one with the same `id` replaces it). `status` holds lists
added to the war status: `planetActiveEffects` (`{"index": planet, "galacticEffectId": id}`), `planetEvents`,
`campaigns`, `jointOperations`, `globalEvents` and the rest. They are added as written, so they must have the game's exact
shape: a mistyped field makes the game drop the whole war update and keep the old one. Use new effect ids (90000 and up).

Effects that work from the file alone (HD2Mods `research/galactic-war/effects`, "Modifier cookbook"):

- **An extra stratagem on a planet** for everyone in the mission, beside their four: type 32, `valueTypes [4, 1]`,
  `values [<stratagem id>, <uses, 0 for unlimited>]`, with the text of the live war's WEAPONS EXPERIMENTATION (as in the
  example). Keep fewer than about 28 on one planet. The stratagem ids are listed below.
- **Shorter or longer cooldowns:** type 36, `[4, 2]` `[<stratagem id>, -50]` (50% shorter), or a whole category with
  `[1, 1]` `[16, 40]` (orbitals +40 s; 128 eagles, 256 sentries, 1048576 exosuits).
- **A booster for the whole mission:** type 83, `[4]` `[<booster item>]`, at most two (the live war used `4066886572`,
  Increased Reinforcement Budget).
- **Subfactions:** type 40 ([research/subfactions](roadmap.md#war-and-missions)); the backend already serves the 13
  vanilla ones.

Banning a stratagem on a planet, changing a stratagem's stats there and weather hazards need engine support; they are
not effects the game has.

<details>
<summary>Stratagem ids for type 32 and 36</summary>

| Stratagem | Id |
|---|---|
| 40-K Meltagun | 4080785205 |
| A/AC-8 Autocannon Sentry | 2616066963 |
| A/ARC-3 Tesla Tower | 1671728820 |
| A/FLAM-40 Flame Sentry | 669794144 |
| A/G-16 Gatling Sentry | 2446402932 |
| A/GM-17 Gas Mortar Sentry | 3428286935 |
| A/LAS-98 Laser Sentry | 914471076 |
| A/M-12 Mortar Sentry | 461790327 |
| A/M-23 EMS Mortar Sentry | 3157053145 |
| A/MG-43 Machine Gun Sentry | 1228689284 |
| A/MLS-4X Rocket Sentry | 3467463065 |
| AC-8 Autocannon | 841182351 |
| APW-1 Anti-Materiel Rifle | 376960160 |
| ARC-3 Arc Thrower | 2138935687 |
| AX/AR-23 Guard Dog | 3791047893 |
| AX/ARC-3 K-9 | 2686392625 |
| AX/FLAM-75 Hot Dog | 4252990341 |
| AX/LAS-5 Rover | 4277455125 |
| AX/TX-13 Dog Breath | 3484474549 |
| B-1 Supply Pack | 336620886 |
| B/FLAM-80 Cremator | 2427670661 |
| B/MD C4 Pack | 3413503378 |
| CQC-1 One True Flag | 2265180087 |
| CQC-20 Breaching Hammer | 2771075958 |
| CQC-9 Defoliation Tool | 1214427163 |
| E/AT-12 Anti-Tank Emplacement | 3106925116 |
| E/GL-21 Grenadier Battlement | 1736727415 |
| E/MG-101 HMG Emplacement | 3827587060 |
| EAT-17 Expendable Anti-Tank | 1207425221 |
| EAT-411 Leveller | 4095488231 |
| EAT-700 Expendable Napalm | 4189624954 |
| EXO-49 Emancipator Exosuit | 754365924 |
| EXO-51 Lumberer Exosuit | 1299305542 |
| EXO-55 Breakthrough Exosuit | 841332828 |
| Eagle 110mm Rocket Pods | 3723465233 |
| Eagle 500kg Bomb | 1982351727 |
| Eagle Airstrike | 700547364 |
| Eagle Cluster Bomb | 1220665708 |
| Eagle Gas Airstrike | 2961949068 |
| Eagle Missile | 2773910920 |
| Eagle Napalm Airstrike | 1427614189 |
| Eagle Rearm | 3064457538 |
| Eagle Smoke Strike | 1062482104 |
| Eagle Strafing Run | 2025422424 |
| FAF-14 Spear | 202236804 |
| FLAM-40 Flamethrower | 1944161163 |
| FX-12 Shield Generator Relay | 70017975 |
| GL-21 Grenade Launcher | 1159284196 |
| GL-28 Belt-Fed Grenade Launcher | 316296057 |
| GL-52 De-Escalator | 3560739565 |
| GR-8 Recoilless Rifle | 1594211884 |
| Health Pack Rack | 894488513 |
| Hellbomb Backpack | 66059712 |
| LAS-98 Laser Cannon | 961518079 |
| LAS-99 Quasar Cannon | 1597673685 |
| LIFT-182 Warp Pack | 3123380863 |
| LIFT-850 Jump Pack | 3863540692 |
| LIFT-860 Hover Pack | 230501979 |
| M-1000 Maxigun | 3659583399 |
| M-102 Gunner FRV | 2074801524 |
| M-103 Supply FRV | 2996892424 |
| M-104 Incinerator FRV | 3657826519 |
| M-105 Stalwart | 1978117092 |
| MD-17 Anti-Tank Mines | 783152568 |
| MD-6 Anti-Personnel Minefield | 3039399791 |
| MD-8 Gas Mines | 1326547218 |
| MD-I4 Incendiary Mines | 3111134131 |
| MG-206 Heavy Machine Gun | 4038802832 |
| MG-43 Machine Gun | 934703916 |
| MGX-42 Bullet Storm | 4001788629 |
| MLS-4X Commando | 3212037062 |
| MS-11 Solo Silo | 906184838 |
| Orbital EMS Strike | 3551336597 |
| Orbital Gas Strike | 1134323464 |
| Orbital Illumination Flare | 1695682779 |
| Orbital Laser | 1520012896 |
| Orbital Napalm Barrage | 691091357 |
| Orbital Railcannon Strike | 2197477188 |
| Orbital Smoke Strike | 1363304012 |
| Orbital Walking Barrage | 808823003 |
| PLAS-45 Epoch | 2961443701 |
| RL-77 Airburst Rocket Launcher | 3201417018 |
| RS-422 Railgun | 295440526 |
| Reinforcement Beacon | 2266266587 |
| Resupply | 548295611 |
| S-11 Speargun | 4030799106 |
| SH-20 Ballistic Shield Backpack | 2369837022 |
| SH-32 Shield Generator Pack | 485637029 |
| SH-51 Directional Shield | 1382612374 |
| StA-X3 W.A.S.P. Launcher | 3564779466 |
| TD-110 Maelstrom | 2237630340 |
| TD-220 Bastion MK XVI | 2437239365 |
| TX-41 Sterilizer | 1725541340 |

</details>

## The services the game polls

The backend answers these for any war, with or without a war script (`WarServices.cs`). Every reply was run through the
client's own parser in an emulator; none has been tried in game yet. The addresses are the client's parsers.

| Request | What it is |
|---|---|
| `GET /v2/Assignment/War/801` | Major orders, polled about every 30 seconds: an array, at most 16 (parser `0x1077770`). |
| `GET /v2/Assignment/Player` | Personal orders, in the same shape. |
| `POST /v2/Assignment/War`, `POST /v2/Assignment/Player` | The client's progress deltas. No reply is read. |
| `POST /v2/Assignment/Player/Complete` `{id32}` | Sent once a personal order's tasks are done (`0x1043d90`). The client adds the reply's rewards itself, so the backend saves them first. |
| `GET /NewsFeed/801?maxEntries=16&fromTimeStamp=<t>` | Dispatches, polled about every 30 seconds: an array with lowercase keys (`0x107d480`). |
| `GET /Episode/801`, `GET /Episode/801/status` | `{episodes: []}` (`0x104be20`, `0x104bab0`). |
| `GET /Stats/war/801/summary` | `{}` (`0x106d040`). |
| `POST /Mail/inbox` | The mailbox, polled about every 60 seconds (`0x10567a0`). |
| `POST /Mail/claim/mail` `{mailIds}`, `POST /Mail/claim/mail/<id>` | A claim. The client adds the attachments' items itself (`0x1373cf0`), so the backend saves them first. |
| `POST /Mission/Session`, `POST /Pes/GetConfiguration` | Mission telemetry (no reply read) and feature messages. |
