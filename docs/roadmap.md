# HellForge roadmap

What HellForge can do today, what is being tested, and what comes after. Updated 2026-10-26 for HellForge 0.3.0-beta.
Nothing here has a date, and stability comes first. What each release changed is in the
[changelog](../CHANGELOG.md).

## At a glance

| Area | Status | Where it stands |
|---|---|---|
| Local game copy | Done | Shipped in 0.2.0 |
| Co-op with friends | Done | Shipped in 0.2.0 |
| [Mod SDK](sdk-roadmap.md) | Done | SDK 15; some events and registries left |
| Build patches for newer Steam builds | Done | Each new Steam build needs a new patch |
| Open source | Done | GPL-3 |
| In-game editor | Mostly done | Player's kit mostly ported; enemies and structures built; missions left |
| Difficulty 11 (Hell Forge) | Mostly done | Needs its own enemy mix |
| Linux | Experimental | Runs under Proton; co-op and Steam Deck untested |
| Public lobbies, privacy, invites, kicking | Testing | Built for 0.3.0; needs two-PC tests |
| Faster mod sync | Testing | Built for 0.3.0 |
| Lockdown: strangers off by default, narrowed network block, mods held read-only | Testing | Built for 0.3.0-dev.5 (PR 24); needs in-game tests |
| Sandbox (the game at low integrity, Steam through a broker) | Testing | On by default since the 0.3.0 beta (Settings > Sandbox), Windows only; old installations get the WFP block on their first sandboxed session |
| Host-leaves fix | Next | Planned |
| Co-op tools for mods | Next | Planned |
| Cyberstan, Meridia and 5 more planets | Testing | Built for 0.3.0-dev.4 (All planets open); the six planets without missions and difficulty 11 on Cyberstan added, untested |
| Super Earth missions | Later | Research done (research/cyberstan sketches it) |
| Subfactions | Testing | Built for 0.3.0-dev.4, with a seed in the launcher; not fully working yet |
| Custom missions | Later | Planned in steps |
| Progression system | Later | Design not decided |
| Custom models | Later | Mesh importer works; physics, AI and textures left |
| 1.0 release | Later | No date yet |

Not planned: a way onto the live servers, play with people outside HellForge, support for the EAC builds, or getting
around an active anti-cheat. See also [known issues](#known-issues), and the [README](../README.md) for what HellForge
is.

Much of this is mod SDK work underneath: the in-game editor, co-op tools for mods, difficulty 11's enemy mix,
subfactions, custom missions, and mod content in progression. The [SDK roadmap](sdk-roadmap.md) tracks that side.

## The details

The rest of this page goes through each item. Contributors will find more detail in [engine-roadmap.md](engine-roadmap.md)
(session runner, local backend, mod loader, stand-ins) and [sdk-roadmap.md](sdk-roadmap.md) (the mod SDK).

Versions: HellForge 0.3.0-beta, engine 0.1.40, mod SDK 15.

## Done in 0.2.0-beta

- The local game copy, the local backend, and the WFP network block, which setup installs for you.
- The first step of the network census: the game logs every connection it tries to make.
- Mod content syncs in co-op. Added stratagems, entities, behaviours and abilities get the same numbers on every PC,
  and new behaviours and abilities sync the way the game's own versions do.
- The mod SDK's foundations: fields by name instead of raw offsets, looking content up by name, and the mod kit (a
  template, the CLI, and mods built from source).
- Live editing of game values, with the host's values used for everyone in co-op.
- The in-game editor, a port of the HD2R Editor that is mostly finished.
- Hook chains, so several mods can hook the same game function, and most of the game events (SDK 13).
- Mod groups, modlist codes, cosmetic packs, and a CLI for mod managers.
- Difficulty 11, "Hell Forge", as a mod: bigger maps, more objectives and outposts, a 72-minute timer, more patrols,
  two missions in one, and fortresses and mega nests in the mega cities.
- Build patches: setup turns a newer Steam build into the build HellForge runs ([downpatch.md](downpatch.md)).
- Linux under Proton, experimental, from the community's work in issue 1 ([linux.md](linux.md)).
- The source code, released under GPL-3.

## 0.3.0-beta (built, now in testing)

This is on the `dev` branch.

Other HellForge players' public lobbies now show up on the Galactic Map. HellForge lists them on Steam's lobby service,
and only HellForge players can see them (CowboyBingus, PR #17). If you'd rather not appear there, a switch in the
launcher keeps your Public lobbies off the map.

When you join someone, mod sync downloads the host's mods for that session. You can trust a Steam friend's code mods
after reading a warning, but a stranger's code mods have to be installed by hand. Downloads run at several MB/s: 38 MB
takes about 10 seconds now, where it took about 3 minutes before commit 5fb79fe. Mods can also list a web address, which MOD
SYNC shows as a GET IT button.

Your lobby's privacy setting decides who can join: Public lets in any HellForge player, Friends only lets in your Steam
friends, and Invite only lets in the people you invite. In-game invites and kicking both work. Multiplayer now requires
Local PlayFab.

In co-op, every player keeps their own in-game editor changes, and joiners can edit too. HellForge loads what each
player's changes need on every machine, so a friend's swapped vehicle weapon or hellpod payload shows up instead of a
purple question mark. A host can still have everyone play with their changes, with a switch on the Mods page. A
player's projectile swaps fire as theirs on every screen, and everyone else's guns keep their own: hand-held weapons,
the ones whose round comes from their ammunition or rounds feed (the Liberator, the shotguns), underbarrels (the
One-Two's grenade) and programmable rounds ([multiplayer.md, shots in co-op](multiplayer.md#shots-in-co-op)). Every
hand-held weapon's round can be swapped in the editor now: ammunition options, charge and heat levels and magazine
patterns included.

Fixed: issue #5 (the F8 editor didn't show with RivaTuner or Afterburner running), plus #6, #13, #18 and #20. Resets in
Local Stats are no longer lost.

Before release we still need two-PC tests: privacy with a friend, a stranger and an invited player; invites; kicking;
mod sync with its restart and rejoin; a public lobby on the map; and each player's own editor changes in co-op
([multiplayer.md, step 10](multiplayer.md#step-10-every-players-own-stats-and-the-lobbys-assets-2026-10-23)). The editor's overlay test also needs one run with
RivaTuner closed.

## Next: stability and co-op

Vehicle weapons' projectile swaps are next: in co-op they should fire as their shooter's on every screen, as hand-held
weapons' do since 0.3.0-beta.

When the host goes back to the ship in the middle of a mission, everyone else gets dropped to the main menu. The fix
will keep the lobby and network alive and hand ownership to another player. If the host quits HellForge entirely, the
session will still end, because fixing that means handing the session over to another player's backend.

Mods need a way to tell which machine is in charge of an entity (`is_authority`) and a channel for sending messages
between players (`send`). That covers state the game itself has nowhere to keep, such as the point a laser has marked or
the target a custom turret has picked. The Pelican's facing and aim height will move onto it
([SDK roadmap, phase 6](sdk-roadmap.md#co-op-sync-phase-6)).

More events for mods: a player joining or leaving, an objective being completed, and a level being generated
([SDK roadmap, phase 5b](sdk-roadmap.md#events-hooks-and-registries-phase-5b)).

The network census has two more steps. Step 2 names every destination the game copy tries to reach. Step 3 answers or
refuses each one and makes a test fail whenever a new destination shows up. We re-run it for every game build HellForge
pins.

CowboyBingus is hardening security with the Steamworks SDK. Public lobbies are being reviewed with hostile players in
mind: checking what they send, kicking them, and rate limits.

## War and missions

The local backend can serve the war two ways. It can follow the live war read-only through the community API mirror, or
it can run "All planets open" (`OpenWar.cs`), where every planet except Super Earth gets a liberation campaign. Some of
the game's content only appears when the war sets it up, and "All planets open" doesn't set any of it up yet.

### Super Earth missions

Status: planned. Research comes first.

Super Earth (planet 0) has seven regions in the war data, which are its mega cities. "All planets open" never gives
Super Earth a campaign and clears every planet event, so its missions can't show up. With the live war they show up
only while the live war has them. The plan:

1. Find out, without changing anything, what the game needs before it offers a Super Earth mission: the campaign or
   defense event, which race it's against, the regions, and anything else the mission generator checks.
2. Have "All planets open" give Super Earth and its mega cities missions against the enemies the game allows there.
3. Add a launcher setting to turn them on or off.

### Cyberstan missions

Status: built for 0.3.0-dev.4, needs testing in game.

Cyberstan (planet 260, the Automatons' home world) has missions of its own, but not from its campaign: the game strips
its map's drop points (game.dll 0x12D88FB) and builds its operations from its 8 Megafactory regions instead ("LIBERATE
CYBERSTAN", difficulties 4 to 10). The war never served those regions, so it offered none. "All planets open" now serves
each of an enemy home world's regions as the enemy's, at full health and available (HD2Mods research/cyberstan). To test:
each region at difficulty 4 and 10, the map and biome loading, the campaign panel, and a friend joining.

Difficulty 11 (Hell Forge): the war table keeps level 11 in a part of the mission board that belongs to region 7, so a
planet with a region 7 got no level 11, and Cyberstan is the only one. Each Cyberstan region's level 11 now takes that
region's level 1 slot, which stays empty because its regions start at difficulty 4 (`difficulty.c`, the mission board).
To test: a region at difficulty 11.

### Meridia and the other planets without missions

Status: built, needs testing in game.

Six planets had a campaign in "All planets open" but no missions: the frozen war keeps a mission modifier on each with
which the campaign builder offers none (HD2Mods research/cyberstan). They are Meridia (1229, the Supercolony biome), Ivis
and Moradesh (1252), Calypso (1418), Penta (1352) and Angel's Venture (1241). "All planets open" now leaves those effects
out, so each has missions at every level, 11 included, against its sector's enemy (Meridia the Terminids'). To test, on
Meridia first: the map loads, a mission at 1 and at 11, extraction.

### Subfactions

Status: built (steps 1 and 2); the war table's flags show since 0.3.0-dev.8. Co-op still needs the host's war on every
machine.

A mission's subfactions come from its planet's galactic war effects of type 40: each adds an enemy tag (Predator Strain,
Spore Burst, Rupture, Dragonroach, Hive Lord; Jet Brigade, Incineration Corps, Cyborgs; the Illuminate's four), and the
spawn director only picks roster rows and squads whose tags are there (0x953150). The game builds its effect table from
/WarSeason/GalacticWarEffects, which HellForge served empty, so no planet effect worked. The backend now serves the 13
subfaction effects and their war-table flags (type 71), so the frozen and live wars get their own subfactions. "All
planets open" deals them planet by planet from the launcher's seed: half of each enemy's planets get none, the others one
of that enemy's, and every Illuminate planet one (HD2Mods research/subfactions); SEAF allies go to about one planet in
eight. Key planets keep their own and are never dealt others: Cyberstan its Cyborgs, and Oshaune, the Terminid hive
world, its Rupture Strain, Dragonroach and Hive Lord. Each deal has a switch in the launcher; off, the frozen war's stay.
In co-op the host's war decides; whether a joiner with another seed loads the host's subfaction enemies is to be tested.

1. Find out what decides a mission's subfaction. Done.
2. Have "All planets open" give each planet subfactions that fit its enemy. Done.
3. Let players choose subfactions per session or per planet, in the launcher or the editor, and let mods set them
   through the SDK ([SDK roadmap, phase 5b](sdk-roadmap.md#events-hooks-and-registries-phase-5b)). A subfaction is
   set up with the mission and never swapped in per spawn.

### Difficulty 11 and custom missions

Difficulty 11 ("Hell Forge") is done apart from its enemy mix. Every enemy's chance of spawning at level 11 starts as
a copy of its chance at level 10, so the mix is Super Helldive's with bigger budgets. Since 0.3.0-beta the
in-game editor's Spawn Rosters can set each enemy's weight at level 11, and Spawn Compositions which squads are drawn
up to it. Next: a default mix shipped with the mod, and an SDK registry for it. Adding new enemies or new squad types to
the mix still needs research.

Custom missions would come in steps. First, a Missions category in the editor for timers and per-difficulty settings.
Second, remixed missions through a mission registry like the stratagem one, once we've studied the mission definition
record. New objective logic would be the third step, and it's probably out of reach.

We also want objectives, mission rows, operations, outposts and stamps as SDK registries. Those registries and the enemy
weights are in the [SDK roadmap, phase 5b](sdk-roadmap.md#events-hooks-and-registries-phase-5b).

## Progression

Status: planned. The design isn't decided yet.

A new profile starts from a template: `starter` (a fresh player just out of basic training) or `unlock-all`
(everything, at the top level). For now the local backend pays out with simple placeholder rules (`MissionService.cs`):
XP based on difficulty, success and objectives, requisition based on difficulty, and whatever samples the game reports
you looted. We want playing up from `starter` to be fun, without turning into a grind or giving everything away. The
plan:

1. Make the whole loop work. Everything a profile earns should be spendable on what the game sells for it (stratagems,
   ship modules, warbond pages, the store), and every way of earning should pay out (medals, super credits, operation
   rewards). The first job is listing which of the game's progression requests the backend answers today.
2. Tune the rewards. That means how fast a profile levels up and unlocks things, per difficulty and across a whole
   operation. The numbers live in one data file, so players and mods can change the pace: faster, slower, or hardcore.
3. Bring HellForge's own content in. Mod stratagems and weapons get a level requirement, a price or a warbond page
   through an SDK call ([SDK roadmap, phase 5b](sdk-roadmap.md#events-hooks-and-registries-phase-5b)), and
   difficulty 11 unlocks through progress.
4. Add more start templates between `starter` and `unlock-all`.

In co-op, every player keeps their own profile and earns rewards on their own machine.

## Modding

This section covers what players will notice. The [SDK roadmap](sdk-roadmap.md) lists everything left in the mod SDK,
phase by phase.

HD2Runtime runs on HellForge through the Lua bridge, and its edits rank between the editors' and code mods'
([lua-bridge.md](lua-bridge.md)). Some of the bridge's events and actions still have no engine call behind them.

The in-game editor should eventually edit every field the HD2R Editor does
([SDK roadmap, phase 5a](sdk-roadmap.md#the-in-game-editor-phase-5a)). That happens in three steps:

1. Weapons and the rest of the player's kit. Weapons, stratagems, backpacks, vehicles, boosters, the Helldiver and
   armor are mostly ported. The GL-15 Evictor, M90A, P-11, P-4, P-69, R-6 and SG-97, blocked before, now have their
   Projectile row, and so do the One-Two's and Arbitrator's underbarrels. Still to do: beam conversion, the HD2R rows that have no HellForge field yet (a coverage
   count will show which), and checking in game when each field takes effect.
2. Enemies: their stats, damage zones and attacks, spawn rosters and compositions. Built in 0.3.0-beta: the in-game
   editor has the HD2R Editor's Enemies pages (Terminids, Automatons, Illuminate, Structures,
   Other Entities) with health, armor, damage zones, attacks, the bug hives' director multipliers, unit size,
   spawners, drop-ship transports, stratagem jammers, bug holes and gore for about 1,170 entities: about 38,800 fields,
   every one the HD2R Editor lets you edit there. Spawn rosters and compositions are built too. The Helldiver's main
   health and size and the Solo Silo's health come with it.
3. Demolition templates, destruction models and mission settings (the other Spawning & Destruction pages).

For custom models, the OBJ mesh importer works in game (the cube gun proved it). Copied units still need working
physics and AI (the blue sentry has neither yet), along with animation events and PNG textures.

The launcher should show which mods hook what and where mods conflict
([SDK roadmap, phase 5b](sdk-roadmap.md#events-hooks-and-registries-phase-5b)).

## Platform and install

Linux support shipped in 0.2.0 as experimental ([linux.md](linux.md)). Testers in issue 1 have run the game under
Proton, but co-op on Linux, the Steam ownership check under Proton, and Steam as a Flatpak or on the Steam Deck are
still untested.

Build patches work: setup picks a patch from the list online and keeps Steam's depot download as the fallback. Each
new Steam build of HD2 needs a new patch.

Setup still needs a full run through its GUI on a fresh PC.

## Known issues

| Issue | Workaround |
|---|---|
| Smart App Control blocks the unsigned session exe (exit code `0xC0000428`) | Turn Smart App Control off. Windows can't turn it back on without a reset ([troubleshooting.md](troubleshooting.md)) |
| HellForge's editor and the HD2R Editor clash | Use one at a time |
| Antivirus programs can quarantine HellForge's files | Add an exclusion for the install folder ([troubleshooting.md](troubleshooting.md#antivirus)) |
| A crash at `helldivers2.exe+0x626296` when the game closes | Nothing to fix: it's the game's own crash on quitting |
