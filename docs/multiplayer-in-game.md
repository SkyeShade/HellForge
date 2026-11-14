# Co-op in game

<!-- diataxis: explanation -->

This page is for contributors working on what co-op players share in game: stats, the assets a change needs, pickups,
each player's projectile swaps and edited rounds, the mod channel, and mod content.

Players see the stats choice in [multiplayer.md](multiplayer.md). The backends, the Steam link and joins are in
[Co-op internals](multiplayer-internals.md).

## Stats in co-op

By default every player keeps their own Local Stats, and joiners can edit as the host can. A host may share its stats
instead (ModSettings `hostStats`, the Mods page switch "Everyone plays with my editor changes", under "When I host").
Then everyone in its lobby plays with the host's. This arrangement was first carried by protocol `hellforge-mp-8`. The
current feature protocol is `hellforge-mp-9`.

**What each way means in game.** HD2Runtime's live tests (`docs/projectile-homing.md`, the Gas EAT test in
`research/docs/runtime-peer-messaging-F5FEE03DCFDB.md`) found:

- Every machine simulates its own copy of each shot from its own data, and the shooter's machine decides the hit and
  the damage.
- With their own stats, each player's weapons hit as their own edits say. Friends see those shots as their own data has
  them: a swapped round flying as the stock one, say.
- A vehicle's arm swap is a type-record edit on the machine that calls the vehicle in, so vehicles a player calls in
  carry that player's swaps.
- Whatever a change puts in the game for everyone (that vehicle's weapon, a pod's pickup) needs its package on every
  machine: [Assets in co-op](#assets-in-co-op).

**How it works.**

- The Local Stats mod (`zz.local.stats`) is not part of the modlist digest, so friends with different saved changes can
  play together. Every other gameplay mod must still match.
- The host's backend (`StatsShare`, `src/HellForge.LocalBackend/Multiplayer/StatsShare.cs`) sends every friend admitted
  to a lobby hosted here `stats.set` [version, then three files gzipped, then `host` or `own`]. It sends it again
  whenever its switch or a shared file changes.
- Sharing, the files are:
  - `mods/zz.local.stats/hellforge-stats.json` (the session passes `--mods`);
  - the editor's `mod-stats.json` (mod content values);
  - the editor settings' `mounts` (arm swaps and hellpod payloads, in `editor-settings.json`).

  They are checked once a second. The editor saves on APPLY, RESET ITEM and preset import. Not sharing, the files are
  sent empty.
- The joiner's backend accepts `stats.set` only from the host of the lobby it joined. With `own` its game keeps its own
  values.
- With `host` it writes the files into the editor's folder as `host-stats.json`, `host-mod-stats.json` and
  `host-mounts.json`. It serves the session's live-edit pipe (`\\.\pipe\hellforge-live-<run id>`,
  `native/mod-loader/live.c`).
- `HellForge.Core.StatsSync` turns the host's file into live edits of the Local Stats mod: `set` for the host's values,
  `base` for the joiner's own saved changes the host does not have. Attachments and status multipliers go over the pipe
  too.
- The game is told its role over the pipe: `session` with solo, host or joiner, `stats=own` or `stats=host`, and the
  host's name. Mods read it through `HfApi.session` and, from SDK 15, `HfApi.stats_source`.
- A joiner playing with the host's stats has an in-game editor that allows browsing and export. It refuses typing,
  APPLY, RESET ITEM, RESET TO DEFAULTS and preset import.
- When the host leaves, closes the lobby, stops answering (15 s) or stops sharing, the host files are deleted. The
  joiner's own values come back (`set`, or `reset` where it had none).
- Not synced: a stats mod's own records (`"own"`), and armor perks, which are solo only. With more than one player,
  everyone gets the armor's own.
- Trust: joiners of a sharing host take its values. Between friends that is fine. A public lobby whose host shares
  edited stats is hidden from other players' maps unless they choose to show such lobbies.

To check sharing in a test, read `multiplayer.log` ("Stats:" lines) and `modloader.log` / `mod-zz.local.stats.log`
("session:", "live set") on each side.

## Assets in co-op

What one player's change puts in the game for everyone (a vehicle's swapped weapon, a pod's pickup, a swapped round's
effects) is made on every machine. A machine without its package shows a missing asset (the purple question mark) or an
invisible effect. HellForge loads a change's packages on the machine that makes it
([internals-live.md](internals-live.md#packages)). `AssetShare` (`src/HellForge.LocalBackend/Multiplayer/AssetShare.cs`)
has every other machine in the lobby load them too.

- The mod loader sends its own packages up the live pipe whenever they change, as `packages <ids>`: 16 hex digits each,
  comma separated, `-` for none. They are those it holds for this machine's edits, mods and actions
  (`native/mod-loader/packages.c`).
- A joiner's backend sends its game's to the host of the lobby it joined, numbered: `assets.mine` [version, ids].
- The host keeps each friend's, and sends the lobby's (its own and every friend's) to every friend, numbered, whenever
  they change: `assets.lobby` [version, ids, the version of that friend's own it holds]. A joiner whose latest the host
  lacks sends it again.
- Lists are at most 64 ids a player and 256 a lobby. Malformed ones are dropped and logged.
- Each backend asks its game to load the lobby's packages less its own: `preload <ids>` over the live pipe. It asks
  again whenever they change and when the game connects again.
- The loader asks for each as for its own edits': catalogue packages only, requested on the game thread, never with the
  reference map 3/4 full. A friend's may not take the last 16 of HellForge's 64: those stay for this machine's own.
- Every new one is logged in `modloader.log` ("lobby assets: ... package(s) for friends' changes", then "package ...
  requested" for each). `multiplayer.log` has "Assets:" lines on both sides.
- Packages are never released, so a change taken back leaves its package loaded for the session.
- Nothing yet makes a change wait until friends have its package. A friend's game starts loading as soon as the change
  is applied (and on joining), which is normally long before anyone calls in what it changed. A vehicle called in at
  once may still show a missing asset on a friend's screen until its package is resident.

Tests:

- `HellForge.LocalBackend.Tests`: `StatsShareTests` (shared, not shared, shared again) and `AssetShareTests` (a host
  and two joiners);
- the loader's fixture `stats_test.c`: `preload`, the 16 kept for this machine's own, the `packages` line,
  `session ... stats=`;
- the editor's overlay test: read-only only while playing with the host's stats.

HD2Runtime's own synced asset loading (its `hd2as` lobby key, `docs/asset-loading.md`) was never proven live. This is
HellForge's own, over its backends' channel.

## Pickups in co-op

A pile picked up on the map (Forge Credits, medals, requisition slips) is sent by the picking player's game alone. Once
every player confirmed it, the game sends `POST /Operation/Mission/Activity` to its own backend, with the lobby's
account ids (`MissionService`). The reply pays each of them the same amount. The game adds its own share and forwards
the others' to their games.

`PickupShare` (`src/HellForge.LocalBackend/Multiplayer/PickupShare.cs`) tells their backends, so the share is in their
profiles too and survives a reload:

- The picking backend sends `pickup.paid` [mission id, pile, activityId32, participant count, account ids]. A joiner
  sends it to the host of the lobby it joined, and a host to every participant in its lobby.
- The host credits itself when listed, and passes a joiner's on to the other participants in its lobby.
- A backend takes it only from a machine in a lobby with it (the host it joined, or a member of its own) that names
  itself among the participants.
- It works out the amount from the pile itself (`MissionService.Rewards`) and pays a pile once.
  `ProfileStore.ApplyPickup` saves the receipt with the item (`progression.paidPiles`, "mission/pile", the last 256).
  So a relayed copy, a retried request or a restart adds nothing.

## Shots in co-op

Every machine simulates every player's shots from its own data, and nothing about a shot's projectile type travels over
the network. A remote shot is the local fire function run from the shooter's replicated trigger, or an explicit fire
message that carries no type. So without more, a player's projectile swap fired as the stock round on the others'
screens. On the swapping player's screen the others' guns fired the swap too. Research:
`HD2Mods\research\projectile-per-player` (from a decoded snapshot of build 25480438).

**The hooks** (`native/mod-loader/shots.c`). Every site's bytes are checked first.

- The ProjectileWeapon fire function (game.dll 0x6128B0), after the game has chosen a shot's type (0x614081, before the
  pellet loop and the projectile pool's constructor). This covers this machine's own shots and its copies of the
  others' alike.
- Each pellet (0x615940) where a weapon's programmable round replaces it while its function is on (0x615B23,
  ProjectileWeapon +0x240).

**The shooter.** It is the weapon's wielder (the Wieldable manager), matched to a player's Helldiver (the player list:
peer and avatar). A player's peer is their SteamID64.

**The swaps.** Each player's swaps come from their Local Stats: the editor's Projectile and Programmable ammo projectile
rows. They travel with the lobby's assets:

- `assets.mine` adds `<weapon resource hex>=<type>[/r|/f|/<stock>]`;
- `assets.lobby` adds every player's `<peer hex>:...`;
- each backend sends its game `shots <rules>` over the live pipe.

Not while the host shares its stats (`shots -`): then every machine already plays with the same. The weapon is the one
firing. `StatsSchema.ProjectileSwaps` maps each Projectile row to it, and an ammunition every weapon equipping it shares
maps to each of them. The suffix says which of its rounds the swap replaces, recognised on each machine by its stock
type:

| Suffix | Round replaced | Weapons |
|---|---|---|
| none | ProjectileWeapon +0 | most weapons, and the Maelstrom's and Bastion's machine guns and the M-1000 Maxigun, whose round it is after all |
| `/r` | WeaponRounds +64, a rounds feed | the DBS-2, R-2124, SG-22, SG-451 and SG-8S. The GL-15 Evictor, M90A, P-11 Stim Pistol, P-4 Senator, P-69 Veto, R-6 Deadeye and SG-97 Sweeper, whose ProjectileWeapon +0 holds the same round. The One-Two's and Arbitrator's underbarrels, weapons of their own. The AC-8, SG-88, GL-52, the Breakthrough's right gun and the GP-31 too |
| `/<stock>` | the round with that stock type | see below |
| `/f` | the programmable round (ProjectileWeapon +0x240) | the GR-8 Recoilless Rifle, RL-77 Airburst, AC-8 Autocannon and W.A.S.P.: a swap of its own beside the weapon's normal one |

About `/r`:

- The type resolver (0x7456D0) checks heat (flag bit 9), then a magazine (bit 7), then rounds (bit 8, WeaponRounds
  +0x40).
- Every live WeaponRounds weapon in the decoded snapshots (SG-8 Punisher, SG-88 Break-Action Shotgun) has bit 8, and
  these weapons have no magazine or heat.
- +0x44, a second mode's round, is left alone.

`/<stock>` covers:

- an ammunition's entity delta. That is the six weapons whose magazines chamber it (AR-23 Liberator, JAR-5 Dominator,
  P-19 Redeemer, P-2 Peacemaker, R-63 Diligence, SG-225 Breaker), and the SMG-37 Defender. It is also every ammunition
  option of the SG-8 Punisher and SG-20 Halt (WeaponRounds +64 / +68, written into the weapon's own copy when it is
  built), and the MP-98 Knight;
- a magazine pattern's entry (MG-43, MGX-42, the A/G-16 and A/MG-43 sentries): round n fires entry (n - 1) % count, the
  tracer included;
- a charge level's projectile (PLAS-101, PLAS-15, PLAS-45: WeaponCharge +4 / +28 / +52, passed at release);
- a heat level's (LAS-17: WeaponHeat +4 / +28 / +52 at 50, 100 and 190 heat).

Where those come from: `mappings/projectile-sources.json` (`scripts/ExportProjectileSources.py`, research
`HD2Mods\research\blocked-projectile-swaps`). This machine's own swap of one is its own rule: the loader does not tie a
delta to its weapon. Rounds of one weapon with the same stock type are one round in co-op, such as an MG-43's four
normal pattern entries, or the Loyalist's full charge and overcharge. The last edit of them is the one the others see.

**Which round a shot fires**, by its stock type. A type this machine's own swap made stands for the stock round it
replaced.

1. The shooter's swap of that round, if they have one and its row and packages are ready here.
2. Else, for this machine's player, what the game chose.
3. Else, for another player, the stock round.

- A round no swap names stays.
- Shots by no player (sentries, enemies) are left alone.
- A weapon with a swap in the lobby that fires with no player found as its wielder is logged once
  (`shots: ... no player found as its wielder`). That is the first thing to check if an underbarrel's swap does not
  show.
- Choosing the type before the pellet loop gives the shot the swapped round's whole row: flight, direct damage, impact
  and expiry explosions, and a shotgun's pellet count. The weapon stays the source's: its ammunition, rate, recoil and
  sounds.
- A weapon firing an entity (the W.A.S.P.'s missiles, the Spear's, the Commando's) spawns it only on the machine with
  authority over the weapon (0x615BE2: entity info +0x14 bit 0, its wielder's). The entity reaches the others: it is
  the shooter's own, with their swap, already.
- The editor offers no swap of the entity itself (the Spear, Commando, P-33, P-92 and the Patriot's left arm have no
  Projectile row).
- Not covered: beams (they choose no row). A swap that follows the gun itself (dropped and picked up by another player)
  needs the weapon's network identity through drops, which the research leaves to in-game tests.

### A player's edited rounds

The same holds for a round's stats: a shot copies them when it spawns, from the spawning machine's own rows.
SpawnProjectile (game.dll 0x13A9830) reads the ProjectileSettings row through its descriptor. It copies the DamageInfo
row's four armour-penetration lanes into the shot. Damage is read from the DamageInfo row per hit, times the shot's
damage multiplier. So a friend's AP 4 round deflected on a host with AP 2, and this machine's own edits showed on the
others' guns. Research: `HD2Mods\research\coop-weapon-stats` (part B).

- Each player's changes of ProjectileSettings and DamageInfo rows (four-byte fields: flight, the damage and explosion
  ids, armour penetration, damage) travel with the lobby's assets.
  - `assets.mine` adds `P<type>+<offset>=<bits>` and `D<id>+<offset>=<bits>` entries: hex offsets and bits, `;` between
    them, at most 96.
  - `assets.lobby` adds `<peer hex>=<entries>` for every player.
  - Each backend sends its game `rounds <list>` (`AssetShare.Rounds`). Not while the host shares its stats
    (`rounds -`).
- The mod loader (`native/mod-loader/rounds.c`) rebuilds every row anyone in the lobby changed, for every other player
  present. It takes this machine's row with this machine's own changes undone (the Local Stats mod's boot edits and the
  editors' live ones, by their originals), then that player's values.
- It changes one call, the pellet spawn of the ProjectileWeapon fire path (0x616612), which makes this machine's copies
  of every player's hand-held shots.
- For a shot another player fires (the shooter found as in shots.c), SpawnProjectile is handed that player's projectile
  row: a copy that keeps the stock type. The spawned shot takes their armour-penetration lanes and a damage scale: their
  standard damage over this machine's, else their durable over this machine's.
- This machine's player's shots, and shots by no player, are left alone.
- Not covered: a shotgun's pellet count (read by type in the fire function), explosion values, damage statuses and
  demolition (read by damage id per hit; the damage list is not moved for this). Shots spawned anywhere else (sentries,
  vehicles, stratagems) are not covered either. Which machine decides damage is unchanged: the shooter's.

## The mod channel in co-op

A code mod can talk to the same mod on the other players' machines, and keep a small table its host owns (SDK 16;
[modding/concepts.md](modding/concepts.md#co-op)). `ModChannel` (`src/HellForge.LocalBackend/Multiplayer/ModChannel.cs`)
carries it, shaped like `AssetShare`.

- The game's live pipe carries its mods' lines: `send <mod id> <all|host|peer hex> <data hex>` for a message, and
  `share <mod id> <key hex> <value hex or ->` for a row the host set.
- A joiner's backend sends its game's messages to the host as `mods.msg` [mod id, from hex, target, data]. The host
  delivers those that name it to its own game, and relays the rest to the machines they name (or to every other member
  for `all`).
- Every receiving backend sends `message <seq> <mod id> <from hex> <data hex>` to its game, which raises
  `HF_ON_MESSAGE` for that mod alone.
- The host's rows are its table. `mods.share` [version, mod id, key hex, value] is one change (no value: removed).
  `mods.sharesnap` [version, count, (mod id, key hex, value) * count] is the whole table to a member whose version is
  behind, which is how a late joiner gets it.
- A joining or reconnecting game is sent `sharereset`, then a `share` line per row it holds. The same lines carry the
  host's changes. The game raises `HF_ON_SHARE` for its own mod.
- The host enforces the caps: a message at most 1024 bytes and 64 a second from each machine, a value at most 64 bytes,
  512 rows a mod. A joiner's share frame is refused, so only the host's rows can grow the table.
- Rows go when the host removes them, or the lobby ends, or this machine leaves it. The game is sent `sharereset` then.
- A game whose live protocol is 1 gets none of these lines. An older backend ignores the frames.

## Mod content in co-op

What mods add (stratagems, entities, behaviours, abilities) is numbered alike on every machine whatever the load order.
HellForge's own logic runs where the game runs its own. What is done and what is still open is in
[engine-roadmap.md](engine-roadmap.md#co-op-sync-for-mod-content) and [sdk-roadmap.md](sdk-roadmap.md) (phase 6). The
game's host can change mid-mission, while HellForge's host-only parts follow the lobby.

Every session folder gets `coop-diagnostics.log` (`native/mod-loader/coop_diag.c`, engine 0.1.19). It holds what this
machine's game sees of content mods added, each line with the co-op role, so two players' files can be laid side by
side. It records:

- added stratagems' beacons (on the thrower's machine), with their package resident or not;
- added entities' spawns, with network id and authority;
- explosions for 45 s after either (a line a second per type);
- packages at mission start and 20 s in;
- the behaviour co-op checks (the Pelican);
- the silo's countdown start and end, laser points, launches, and the missile each second for 15 s after;
- Orbital Plasma Barrage blocks;
- changes of the game's host;
- mission start and end, with the list of added content.

The log was added for what the two-PC test of 2026-10-21 (0.1.8) left open:

- a friend's Pelican never ran its behaviours on the others' machines;
- a silo launch did not carry over: the other machine's missile stayed or vanished, with no flight or blast. So the silo
  launches on every machine again;
- a host's Orbital Plasma Barrage was unseen by the friend, and the friend's landing showed on the host's previous
  beacon on the host's screen;
- a host's custom support weapon's shots were unseen by the friend.

The build logs, with their two-PC test plans, are in the page's earlier version:
[host-shared stats (step 5, 2026-10-17)](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multiplayer.md#step-5-stats-in-co-op-the-hosts-values-everywhere-2026-10-17) and
[per-player stats and lobby assets (step 10, 2026-10-23)](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multiplayer.md#step-10-every-players-own-stats-and-the-lobbys-assets-2026-10-23).
