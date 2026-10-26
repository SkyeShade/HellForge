# Changelog

What changed in each HellForge release, newest first, as a player updating from the release before sees it. Numbers
are GitHub issues and pull requests (#24); short hashes are commits on `dev`. Each pull request adds its lines under
Unreleased, and a release renames that section ([CONTRIBUTING.md](CONTRIBUTING.md#changelog)).

## Unreleased (0.3.0-beta)

On `dev`, since 0.2.0-beta. Engine 0.1.40, mod SDK 15.

### Added

- **Public lobbies on the Galactic Map.** Other HellForge players' Public lobbies show on the map, and you can join one
  from its squad marker. They are listed on Steam's lobby service, and only HellForge players see them. "List my
  Public lobbies on the Galactic Map" on the Mods page keeps yours off it (#3, #17, #24).
- **Play with strangers** (Settings > MULTIPLAYER), off by default. Off, only your Steam friends can join you, you can
  only join friends, and your Public lobby is not listed (#24).
- **Lobby privacy decides who joins:** Public lets in any HellForge player (with Play with strangers on), Friends only
  your Steam friends, and Invite only the people you invite. In-game invites and kicking work (#17).
- **Mod sync.** A join refused because your mods differ from the host's now shows a MOD SYNC prompt with each of the
  host's mods. DOWNLOAD AND RESTART fetches what it can, restarts with the host's mods, and the host invites you back.
  Your own Mods page is not changed. Mods HellForge can check itself download from any host. Mods that run code or
  carry unchecked data download when they are on HellForge's verified list, or from a Steam friend once you TRUST them
  past a warning. A stranger's you install yourself, and GET IT opens the mod's web page (#17, 15e54f1, 30bae0e).
- Mod sync downloads at several MB/s: 38 MB takes about 10 seconds instead of about 3 minutes (5fb79fe).
- A MOD SYNC card on the Mods page: share your gameplay mods with players who join, the mods you blocked, and the mods
  you downloaded (#17, #31).
- **Editor changes in co-op.** Every player keeps their own in-game editor changes, and joiners can edit too. Each
  machine loads what the others' changes need, so a friend's swapped vehicle weapon or hellpod payload shows instead of
  a purple question mark. "Everyone plays with my editor changes" on the Mods page (off by default) makes everyone play
  with the host's (0df8add).
- **Projectile swaps for every hand-held weapon's round:** ammunition options, charge and heat levels, magazine
  patterns and rounds feeds. In co-op a player's swapped or edited rounds fly and hit as theirs on every screen, armour
  penetration and damage included (#37, 6171237, 60dfddc, 63b66cc).
- **The game runs sandboxed** (Settings > Sandbox, on by default on Windows): at low integrity, with Steam reached
  through a broker. An installation with the old Windows Firewall rules gets the newer network block on its first
  sandboxed session, after one Windows prompt (#24, #31, #48).
- **In-game editor:** the HD2R Editor's Enemies pages (Terminids, Automatons, Illuminate, Structures, Other Entities:
  health, armor, damage zones, attacks and more for about 1,170 entities), its Spawning pages (difficulty, spawn
  rosters and compositions, each enemy's weight at level 11) and Demolition Templates, the Helldiver's main health and
  the Solo Silo's health (06a44ec, 73baf5b, c40cc6c, af1ad77).
- Editor: faction emblems on enemies, structures and other entities, difficulty icons ("D10 · Super Helldive"), and
  HD2R-style roster rows (#31, #32).
- Editor: the GL-15 Evictor, M90A, P-11, P-4, P-69, R-6 and SG-97 get their Projectile row, and so do the One-Two's and
  Arbitrator's underbarrels (2732b17).
- Editor: giving a weapon a second or third rate of fire binds a free input to Rate of fire, so the added rates can be
  picked in game. The Left and Right weapon function rows are gone (#37, #39).
- **All planets open: subfactions.** Planets get the war's subfactions (Predator Strain, Jet Brigade, the Illuminate's
  and the rest), with their flags on the war table. They are dealt planet by planet from a seed you type or roll in the
  launcher's GALACTIC WAR card: half of each enemy's planets get none, and SEAF allies fight on about one planet in
  eight. Each deal has a switch. Cyberstan and Oshaune keep their own (ea63b33, 1440557, #35, #38, #46).
- All planets open: Cyberstan's Megafactory missions, difficulties 4 to 11 (9adf5b1, #46).
- All planets open: missions on Meridia, Ivis, Moradesh, Calypso, Penta and Angel's Venture (#46).
- Setup and every session check for the Visual C++ 2012 and 2015-2022 x64 runtimes, and name Microsoft's download for
  the one missing (8b4985e).
- Editor: with HDR on in the game, the in-game editor draws in HDR too (HDR10 and scRGB), at the brightness Windows
  gives SDR content, instead of looking washed out (#53).
- Language packs: Japanese can be added to the game copy, so the game runs in Japanese instead of crashing when Steam
  is set to it. For now through the command line, `HellForge.Cli language-add --name japanese` (#73).

### Changed

- Multiplayer needs Local PlayFab (#17).
- Local Stats (your in-game editor changes) is no longer listed on the Mods page and is always on. Friends never had to
  match it (#36).
- During a session the mods folder and HellForge's app folder are read-only, and the game cannot start other programs.
  A file that appears in a mod during play is listed at the end and that mod switched off (#24).
- The network block lets the game reach only HellForge's local backend, on one port. An older block is placed again on
  the next session, with one Windows prompt (#24, #31).
- A host's "X couldn't join" is a short note in the top right corner. The player who couldn't join still gets the full
  panel with the mod list (#34).
- The in-game editor shows the values on screen first, before it works out which mod holds each one (#34).
- Save logs to Desktop from a sandboxed session leaves out the game's AppData copy, saves and shader cache (#40).
- The launcher says HellForge runs the game locally with co-op over Steam, not "offline", and its tagline is "Local
  HELLDIVERS 2" (e224491, #31).
- Play asks Steam whether you own the game only when no check passed in the last 7 days on the same installation,
  Steam account and game build, so HELLDIVERS 2 no longer flashes as running in Steam at every launch. Setup and Check
  and repair always ask (#43, #69).

### Fixed

- The F8 editor didn't show with RivaTuner, MSI Afterburner or another overlay running (#5, #44; c15be21, 32c1047).
- `IO_SharingViolation_File` on `steam_appid.txt` after a crash or hard reset blocked every later session (#6; c15be21).
- ReShade couldn't save presets or find its shaders and textures, and DLSS, XeSS or FSR libraries of another version
  stopped the game. Sessions now hold only the game's own files (#13; c15be21).
- A file in a mod's folder open in another program (an Excel sheet) blanked the Mods page and crashed the launcher
  (#18; c15be21).
- Linux: mods that build from source failed with "Access denied. (ziglang.org:443)". `hellforge-linux.sh` now fetches
  Zig before the network namespace starts (#20; c15be21).
- Resets in Local Stats were lost when another program held the file (32c1047).
- A "Network Error" after a start-up dialog stayed open for more than 3 minutes (#34).
- Difficulty 11: big city maps crashed the game as the mission launched (#51).
- The Steam ownership check failed in an installation folder with `[` or `]` in its name (#50).
- "Network block was not installed: Linked target path." didn't say which path. It names the link now (#50).
- Setup on a full drive blamed Steam's files instead of asking for space (e9e1d63).
- Rejoining could fail when the host answered before the join started (821aa23).
- The game crashed as it quit with a mod's added weapon, such as the R-40 Sightliner (#64).
- "Required loopback/egress controls failed." stopped every session on some installations outside drive C: (or moved to
  another folder), where the network block had stopped holding after a restart. The session now places the block again
  by itself, with one Windows prompt, and a failed check says which part failed (#75).

### For mod makers

- SDK 14: `sync_state`, `sync_mod` and `sync_answer`, the MOD SYNC prompt's calls (#17).
- SDK 15: `stats_source`, whether the lobby plays with the host's stats or each player's own (0df8add).
- `HF_NOTICE_BRIEF` shows a notice as a short corner note (#34).
- A mod's manifest can give its web page as `"url"`, which MOD SYNC shows as GET IT (15e54f1).
- `HellForge.Cli mod-hash --mod <folder or zip>` prints a mod's content hash, as mod sync and the verified list use it
  (9f09153).

### For launcher developers

See [docs/external-launchers.md](docs/external-launchers.md).

- `play` and `settings` take `--sandbox yes|no`. It defaults to yes where the sandbox can run (Windows, with an engine
  built with the Steam bridge); elsewhere `play` runs unsandboxed even when `settings` prints `"sandbox": true` (#31,
  #48).
- `settings --strangers yes|no` turns Play with strangers on or off. It is saved in `mods\hellforge-mods.json`, and
  `play` has no option for it (#24, #31).
- `settings` prints `sandbox`, `steamBridge` and `playWithStrangers` (#31).
- `mods\hellforge-mods.json` gains `blocked`, `share`, `showEditedStats` and `listPublic` (#17, #24).
- `mods` still lists Local Stats (tag `local-stats`), and `mod-switch --id zz.local.stats --on no` is refused (#36).
- `launcher-settings.json` gains `subfactionSeed`, `dealSubfactions` and `dealSeaf`, which `play --war open` uses
  (1440557, #38).
- `play --full-ownership-check yes` asks Steam for this session even when a recent check holds. `result.json` gains
  `ownershipCheck` (`full` or `recent`) and, for a reused check, `ownershipConfirmedUtc` (#69).
- `languages` lists the language packs pinned for the supported build, and `language-add --name <language>` copies one
  from Steam's download into the game copy. `status` prints `languages`, the packs in the copy (#73).

### For contributors

- CI runs `scripts/Test.ps1` on every pull request into `dev` or `main`, and `main` changes only by a release from
  `dev` (#29).
- A structure lint in CI, `py -3 scripts/lint/lint.py check` (#30).
- `HELLFORGE_MULTI_INSTANCE=1` runs two HellForge installations on one PC as two players, for co-op tests
  ([docs/multi-instance.md](docs/multi-instance.md), #24).
- The structure lint's known violations are fixed everywhere, so its baseline is empty: the mod loader, editor, Core
  and CLI, backend, session runner, launcher, generator, Steam bridge and stand-ins, with no change in behaviour (#54,
  #55, #56, #57, #58, #60).
- CI builds and tests the native code (`Build-Engine.ps1 -NoSteamApi`, `Build-Editor.ps1 -Test -SkipOverlayTest`), and
  an "Engine version" job warns when a pull request changes the engine and fails a release that ships it without an
  `EngineVersion` bump (#66, #68).
- Tests no longer use fixed ports, pipe names or sleeps, so two runs at once don't collide (#59, #71).
- This changelog, and a Changelog checkbox in the pull request template (#67).

## 0.2.0-beta (2026-10-22)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.2.0-beta)

### Added

- HellForge is open source under the GPL-3.0, with the engine's source in this repository. Mods made for HellForge may
  use any licence; the mod SDK, template and example mods are MIT-0.
- Setup works with the current Steam build: it downloads a patch (about 1.7 GB) that turns your own Steam copy into the
  build HellForge runs, and checks every file against Steam's hashes. If the patch fails or there is none yet, it falls
  back to Steam's own download of the supported build.
- Linux (experimental): setup runs under Proton. `hellforge-linux.sh` checks the programs it needs, finds Steam and the
  Proton prefix, and moves Steam's download of the game into the prefix ([docs/linux.md](docs/linux.md), #11).

### Changed

- Logs and the launcher's messages say `%USERPROFILE%` and `PC` instead of your Windows user folder and computer name.
- Save logs to Desktop scrubs every text file it packs, mods' and the game's logs included, and no longer packs Steam's
  `steam_api64.dll`.
- The launcher calls HellForge's block a network block, and no longer ends every session with "Firewall rules removed:
  unconfirmed".
- The Steam ownership check needs Steam to confirm the game. A check that failed to run used to pass. It runs on Linux
  too.

### Fixed

- New profiles didn't own the Orbital Precision Strike and the MG-43 Machine Gun, so buying them failed. Existing
  profiles get them too.
- Weapon levels on the profile page reset within a second of typing.
- The Discord button opened the old server.
- An installation folder with non-English characters in its name got no network block on some Windows languages.
- Linux: Steam's downloads under `ubuntu12_32`, `ubuntu12_64` or `steamrt64` weren't found.

### For mod makers

- Mod SDK 13: several mods (and HellForge) can hook the same game function, in order (`hook_ex`, `code_matches`,
  `read_code`).
- A mod guide, from a first mod to hooks ([docs/modding](docs/modding/README.md)).

### For launcher developers

- `HellForge.Cli` covers what a launcher needs with HellForge.exe closed: sessions, mods and cosmetic packs, profiles,
  settings and session logs ([docs/external-launchers.md](docs/external-launchers.md)).

## 0.1.13-beta (2026-10-21)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.1.13-beta)

### Added

- Mod-added content syncs between players: custom stratagems, barrages, support weapons, Pelicans and missiles get
  their state on every machine like the game's own.
- A refused join says why in game: a different HellForge version, or which mods differ.
- A friend whose game crashed mid-mission can join it again.
- Linux (experimental): HellForge runs under Proton, with the game kept off the internet by a network namespace. Setup
  still runs on Windows ([docs/linux.md](docs/linux.md), #1).
- `coop-diagnostics.log` in each session's logs records what this machine saw of mod-added content. Save logs to
  Desktop also packs the game's console log, and a crash note names the module the crash came from.

### Changed

- Silo missiles launch once, from the machine that owns the silo, and everyone sees the flight and the blast.
- Session start no longer rewrites unchanged mods or re-reads mod zips.
- The Mods page names HD2Runtime's mod template as a template instead of "no hellforge-mod.json".

### Fixed

- Mods that write files into their own folder made friends' modlists differ.
- A crash when a missile hit with its remote lying on the ground.
- Difficulty 11 was missing on some PCs (when Windows placed the game high in memory).
- The Integrated Extinguishers booster was missing from unlock-all profiles.

### For mod makers

- SDK 12: `notice`, a message the in-game editor shows the player.

## 0.1.8-beta (2026-10-21)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.1.8-beta)

### Added

- Lua mods through HD2Runtime: install Bingus Shared Loader and HD2Runtime 0.50.0 like any other mod, then the Lua mods
  made for them. HellForge makes every change, so the F8 editor shows what each Lua mod changed. HD2Runtime's custom
  stratagems and enemy controls don't run on HellForge. Lua mods are client-only.
- The Mods page warns when a Lua mod needs Bingus Shared Loader or HD2Runtime and it is missing, or HD2Runtime is too
  old.
- A MODS page in the F8 editor: HellForge mods and Lua mods, each with the values it changed. Values a mod set are
  marked blue (HellForge mods) or purple (Lua mods), and Reset goes back to the mod's value.
- Galactic War "All planets open": the frozen war with every planet open for a liberation, one faction per sector.

### Changed

- Setup finds the patch more reliably: it reads every Steam library, accepts a game with a Steam update queued or
  paused, and says so when Steam is busy with the game or the game is running.

### For mod makers

- SDK 11: events for heals, mission stats, weapon switches, explosions, bug holes and damage before it applies, and
  actions (heal, limbs, statuses, velocity, the Supply Pack, explosions, projectiles, the Pelican, kill credit, the
  mission timer, shot rules). SDK 10: `mods` lists the session's mods.

## 0.1.7-beta (2026-10-20)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.1.7-beta)

### Added

- Setup works with the current Steam version (build 25988413): it downloads a patch pack (about 1.7 GB) and patches
  your own Steam files into HellForge's game copy, instead of a 24 GB depot download. Your Steam installation is only
  read. Other game versions still use Steam's console download.

### Changed

- The game copy leaves out 19 depot files HellForge never uses (135 files instead of 154).

### Fixed

- Editor: weapons that fire an entity (the Breacher's charge, the Maelstrom's rockets) edited rows the weapon doesn't
  use. They now edit that entity's explosive, explosion and damage rows.

## 0.1.6-beta (2026-10-19)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.1.6-beta)

### Added

- Editor: many more fields, close to the HD2R Editor's set: underbarrel weapons, the K-9's rate of fire, plasma
  charge, sway and recoil for exosuit arms, emplacements and vehicle guns, explosion damage on damage zones, the
  armory's armor penetration, the Epoch's and Railgun's overcharge explosion and the Urchin's pulse.
- Editor: hellpod payloads, what a call-in's pod drops and how many.
- In co-op a joiner's editor shows the host's changes, read-only. The joiner's own changes come back after.
- Import settings from HELLDIVERS 2 (Profile page): your options and, if you like, your key bindings. Undo import takes
  it back.
- Vanilla Plus is bundled, on by default: beacons stick where they land, hellpods steer over any ground,
  reinforcements land at their beacon.

### Changed

- Attachment changes reach the running game (respawn or re-equip to see them).
- Presets carry pod slots, arm swaps and armory labels.
- The editor is easier to read: fire modes and rates sit together, choice popups' scrollbars show, and notes explain
  special values.
- Updating replaces older copies of the bundled mods. Old copies move to `mods-retired`, with their settings.

### Fixed

- F8 did nothing for some players with an overlay (Discord, RivaTuner, Overwolf, NVIDIA).
- Difficulty 11 could pick an Eradicate mission as its second mission.

### For mod makers

- SDK 9: `set_stat` and `copy_row`.

## 0.1.5-beta (2026-10-19)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.1.5-beta)

### Changed

- Windows Firewall is no longer needed. The network block lives in the Windows Filtering Platform and holds whether the
  firewall is on or off, so security suites and VPN kill switches no longer break it.
- Setup places the block and proves the game copy can't reach the internet before it finishes.
- Uninstall HellForge (Settings > Installation) removes the block, its shortcuts and the folder.
- Existing installations keep their firewall rules until Settings > Installation > Check and repair.

## 0.1.4-beta (2026-10-19)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.1.4-beta)

### Changed

- Editor: weapon reticles are picked by style, named with the weapons that have it, instead of Yes/No.

### Fixed

- Difficulty 11: opening the map crashed the game on missions with many objectives and a route.
- Editor: typing a number into a list of choices crashed the game.

## 0.1.3-beta (2026-10-18)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.1.3-beta)

### Changed

- Friends on different HellForge versions are told so and can't join each other.
- The launcher keeps the newest 10 sessions' logs.

### Fixed

- Co-op: a host could crash when a friend launched the Shredder Silo.
- Co-op: custom Pelican stratagems sat on the ground for the other players.
- Co-op: friends who ordered the same mods differently could get each other's stratagems, weapons and Pelicans.

## 0.1.2-beta (2026-10-18)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.1.2-beta)

### Added

- Steam Family Sharing: setup and sessions accept a game shared through Steam Family Sharing.
- The in-game editor works with DirectX 11 (`--use-d3d11`).

### Fixed

- Co-op: a friend's Shredder Silo missile and blast didn't show on the other screens.
- Co-op: a player whose game crashed couldn't join the lobby again until the host restarted.
- The Mods page counted Local Stats among the gameplay mods friends need.

## 0.1.1-beta (2026-10-18)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.1.1-beta)

### Added

- Launch options (Settings > Launch options), as in Steam: `--use-d3d11` starts the game with DirectX 11.
- Add mods... takes `.7z` as well as `.zip` and `.rar`.

### Fixed

- Co-op: a game crashed when a friend's Shredder Silo missile landed.
- Co-op: joiners kept their own values for mod stratagems instead of the host's.
- Co-op: friends with the same gameplay mods in a different order were refused.
- Cosmetic packs with a "VANILLA" or "Default" choice were refused.
- The in-game editor's category list didn't scroll when it didn't fit.

## 0.1.0-beta (2026-10-18)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.1.0-beta)

The first public beta: a local copy of HELLDIVERS 2 that runs against a local backend on your PC, co-op with Steam
friends, the in-game editor (F8), the Hell Forge difficulty (level 11), a Mods page, and a C mod SDK with docs and
examples.
