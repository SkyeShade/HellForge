# Changelog

What changed in each HellForge release, newest first, as a player updating from the release before sees it. Numbers
are GitHub issues and pull requests (#24); short hashes are commits on `dev`. Each pull request adds its lines under
Unreleased, and a release renames that section ([CONTRIBUTING.md](CONTRIBUTING.md#changelog)).

<!-- docs-lint-ok: D5 a release log grows with every release; each section stays short -->

## Unreleased

On `dev`, since 0.4.0-beta.

### Added

- **Mods can add enemies of their own (SDK 17).** `add_enemy` gives an entity a roster row with its own spawn group,
  weights, cost and limits, tied to no vanilla enemy. `add_enemy_squad` adds squads that send those groups as patrols,
  guards, stragglers, outpost garrisons, reinforcements, border flyers, convoys or air support. `add_enemy_swap` adds
  a replacement rule. An enemy names the packages its entity needs, and HellForge loads them. No vanilla enemy is
  replaced, and any entity can be an enemy ([new enemies](docs/modding/enemies.md)).
- **Entities built from parts (SDK 17).** `parts` in `register_entity` and `add_entity` takes each component's record
  from an entity of your choice, with or without a base
  ([an entity from parts](docs/modding/enemies.md#an-entity-from-parts)).
- **Mods can animate their weapons.** A data spec's `animations` names a JSON file with the joints that move, their clips
  and the state machine that plays them on the game's events. HellForge builds the unit copy's own bones, clips and
  state machine from it ([animations](docs/modding/animations.md)). `HellForge.Cli unit-joints` lists a unit's joints,
  and the `animated-liberator` example kicks its bolt back on each shot and drops its magazine on a reload.
- **Mods can build weapons from their own models.** A data spec's `model` turns a glTF 2.0 file (a `.glb` from Blender)
  into a unit of the mod's own. Every node becomes a joint, every mesh draws rigid on its node, and node names give LODs
  and shadow casters ([models](docs/modding/models.md)). The `model-carbine` example builds a whole weapon this way.
- **Mods can bring their own textures.** A data spec's `texture` builds a texture from PNG files in the mod folder,
  in RGBA8 or BC1, BC3, BC4 or BC5, with its mips and as an array of layers. A material copy's `textures` puts it in a
  slot by name ([textures](docs/modding/textures.md)).
- **Mods can have packages of their own.** A data spec patch with `package` becomes a whole archive under the package's
  id. It loads when the game starts, or with `"load": "request"` only through `load_package` (SDK 17), which now takes
  a mod's own packages ([packages of your own](docs/modding/data.md#packages-of-your-own)).
- **Mods can add attachments to their weapons (SDK 17).** `add_attachment` copies a vanilla magazine, muzzle, optic or
  other item under an id of its own, optionally with the mod's own model, and offers it on weapons mods added. The
  player holds it from the start ([attachments](docs/modding/content.md#attachments-of-your-own)).
- **Sound banks of a mod's own.** A data spec's `sounds` builds a Wwise bank from WAV files, one event each, routed like
  a game bank's mixer. `hf_wwise_id` gives an event's id for the records that play it ([sounds](docs/modding/data.md#sounds)).
- **A weapon's handling records have named fields.** Equipment, Wieldable, Attachable and WeaponCustomization, all of
  WeaponReload and Unit's `unit` can be set by name, so a weapon can name only assets of its own
  ([a weapon of your own](docs/modding/content.md#a-weapon-of-your-own)).
- **New explosion, beam and arc types for mods (SDK 17).** `add_explosion`, `add_beam` and `add_arc` copy a vanilla type
  with its own damage, radii, length or chained arc, up to 512 of each across all mods. A projectile names its blasts in
  `impact_explosion` and `expiry_explosion`, and a weapon its `beam` or `arc`
  ([projectiles and damage](docs/modding/content.md#projectiles-and-damage)) (#298).
- **295 explosions and 342 damage ids by name.** The catalogue lists them as `HF_EXPLOSION_` and `HF_DAMAGE_` constants
  for mods (#298).
- **A mod weapon's own explosion, beam and arc in the editor.** Its page shows them with the damage they deal, and mods'
  explosions are offered wherever the editor picks an explosion (#298).
- **Mods can add warbonds.** A mod's `hellforge-warbonds.json` lists warbonds with up to 15 pages of up to 10 items each,
  priced in medals, requisition, samples or Forge Credits, with later pages opened by what was spent
  ([warbonds](docs/modding/warbonds.md)).
  - Pages can sell game items and the mod's own weapons and stratagems, which stay out of the armory until bought.
  - Up to 99 mod warbonds are listed after the game's, where the game lists 32 warbonds in all. Each has its own name,
    look and art (a plain card without any), and its prices and texts show its currency. Past 32 warbonds the column of
    dots beside the cards shows the group of up to 32 the current one is in.
  - Each has its own Helldiver beside its card: a game warbond's background and pose, and the armor, helmet, cape and
    armory weapon the mod names (its own weapons included).
  - A warbond can cost Forge Credits, paid once with the game's warbond button, and an item can have a currency of its
    own, which the claim button names when the player cannot afford it. Only prices in the warbond's own currency count
    as spent on it.
  - An item the player has already, from the game's warbonds or another mod's, shows claimed and is not sold again.
- **Shields that stop grenades.** The FX-12 Shield Generator Relay, G/SH-39 Shield and SH-32 Shield Generator Pack have a
  **Stops grenades** setting in the editor, off by default. Grenades, throwing knives and self-destruct drones bounce off
  the bubble from outside, and ones thrown from inside still leave. Helldivers, enemies, vehicles and
  stratagem balls pass as before.
- **Shields that stop enemies.** The FX-12 Shield Generator Relay has a **Stops enemies** setting in the editor, off by
  default. Enemies stop at the bubble and break it with their own attacks, charges end there with their hit reaction,
  and pounces stop on it. Enemies already inside stay inside, and Helldivers pass.
- **Shields past 10,000 health.** A shield set stronger than that keeps its value: the network type that capped it now
  widens to match.
- **Backpack ammo past 1,023.** The editor now takes backpack ammo up to 1,048,575 on any backpack. The HellForge Editor
  widens `deposit_value` to 20 bits for everyone, so it now counts as a shared mod: friends need the same HellForge version.
- **Mod sync from your own game.** A host's mods' game patch files are written from the joiner's own game wherever it
  has the same bytes, and only the rest downloads. The host lists each archive part with its SHA-256 (`mods.parts`),
  and every file is still checked against the host's. **Index my game** on the Mods page finds renamed game files too.
  A recipe mod whose sources the joiner's game lacks (after a game update) takes just those ranges from the host's
  rebuilt files ([mod sync from your own game](docs/multiplayer-mod-sync.md#mod-sync-from-your-own-game)).
- **The asset kit: share mods without Arrowhead's files.** `assets-check` shows which bytes of a mod's game patch files
  are the game's and from which build. `assets-strip`, and `package-mod` by itself, ship the author's own bytes plus a
  recipe. Each player's HellForge rebuilds the files from that recipe, out of their own game, checked by SHA-256. The
  Mods page and every session rebuild such mods before they load; friends and mod sync compare and send the recipe,
  never the game's bytes ([the asset kit](docs/modding/assets.md)).
- **The asset kit: cut content from older builds.** `--history yes` on `assets-check`, `assets-strip` and `package-mod`
  checks what a mod still ships against every build since release. It reads Steam's depot manifests with the author's
  own sign-in, and fingerprints just the resources it proves. A player whose recipe needs such a build gets **GET THEM
  FROM STEAM** on the Mods page (or `assets-rebuild --steam yes`), which fetches only the chunks those pieces sit in.
- **Prepare for sharing.** The Mods page's ⋮ menu item (formerly Zip to share) takes Arrowhead's bytes out of a mod's
  game patch files before it zips it, as `package-mod` does. A card says what is out, with the zip, the asset report
  and **CHECK OLDER BUILDS** for bytes that matched no build on the PC.
- **The asset kit's Steam sign-in is kept.** GET THEM FROM STEAM, CHECK OLDER BUILDS, `--history yes` and
  `assets-rebuild --steam yes` ask for the Steam app's QR code once. The sign-in is then kept for your user only and
  renewed, and the asset migrator's saved sign-in is used too. **Sign out of Steam**, or `steam-signout`, forgets it.
  Setup's download still keeps nothing ([signing in to Steam](docs/modding/assets.md#signing-in-to-steam)).
- **Data specs: resources from past builds.** `"source": {"build": "2024-03-01"}` (a day, or a data manifest) on a
  `copy` or `include` takes the resource from that build. Each player fetches just those resources from Steam once, with
  the same prompt, and the mod ships only its spec ([data specs](docs/modding/data.md#source-a-past-build)).
- **Room for 1,024 new stratagems across all mods (was 48).** The loadout picker's frame is sized for every type, and
  past 106 new stratagems the type travels in co-op in 16 bits instead of 8, so every player needs the same mods, as
  before. The picker still shows 256 cards, about 160 of them new ones
  ([limits](docs/modding/content.md#limits)).

### Changed

- **New profiles start live-like.** The new `live-like` template is a starter profile until you run Sync Inventory.
  The sync then brings in everything it can from your live account: level, unlocks, ship modules, medals and samples,
  the ship's name and the tutorial progress. The premium warbonds you own open without a purchase in Forge Credits. It is the first profile of a new installation and the launcher's first choice. Existing profiles keep
  their template, and `starter` stays for earning everything again (#282, #262).
- **No fixed limit on mods' entities and weapons.** Mods together could register 64 entity blocks and 512 entities and
  weapons. HellForge now takes as many as the game's own tables hold (its entity map has room for about 2,187 more),
  at boot and in live changes. A strat pack and a compendium together no longer run out (#307).
- **No idle kick.** Alone on the ship, 15 minutes without touching the controls no longer sends you back to the title
  screen, and the countdown warning before it is gone (#304).
- **A mod download from a host no longer stops at 10 minutes.** The grant now stays on while the download goes on
  (2 minutes past each request, at most 6 hours), so a large mod over a slow link finishes; one left idle still ends.
  The host keeps the player expected back while they download, so the rejoin after a long download is still let in
  ([mod sync](docs/multiplayer-mod-sync.md#mod-sync-speed)).
- **Sync Inventory only listens.** It keeps what the normal game itself receives after logging in (your progression and
  inventory) and no longer asks Arrowhead's API for them a second time, so a sync adds no requests of its own.
- **The game's catalogues come from your own sync.** HellForge no longer ships the item catalogue, item and progression
  packages, level table or warbond list. Sync Inventory copies them from what the normal game receives when it logs in,
  cut to what the supported build knows, so a new installation needs one sync before its first PLAY. The warbond
  pages and the requisition store, which the normal game doesn't read at login, still ship (#296).
- **Mods take far less disk space.** PLAY no longer puts a second copy of every mod's patches into the game's data
  folder. On Windows they are links to the mod's own files; on Linux the files HellForge keeps itself are linked and
  mod folders still copied ([disk space](docs/mods.md#disk-space)). While the game runs on Windows, a mod's patch files
  cannot be saved over, as game files cannot. A staged patch written over in the game's data folder is found at the
  next session start (#306).
- **Zipped cosmetic packs are kept as folders.** A zip next to its unpacked copy held the pack twice. Add mods puts a
  zipped pack in as a folder, and one already in the mods folder becomes one, with its options and place kept. A
  `.rar`, `.7z` or zipped pack dropped into the mods folder whose files the installed mod already has is removed as a
  second copy (#306).

### Fixed

- **Mod stratagems no longer read past the loadout's level choices.** Six places in the game read a per-type choice
  that has only 150 entries for every stratagem, mod ones included, and two more could write there; they now see no
  choice for a mod stratagem. When the game loaded its stratagem settings again, the count of stratagems went back to
  the vanilla 149 while mod stratagems stayed, and it now stays at the full list.
- **Direct3D 11 crashes in the Armory and at mission start.** Browsing armour crashed the game at
  `helldivers2.exe+0x732040`, and a slow PC could crash at `+0x626296` as a mission loaded. A freed texture is now bound
  empty instead of read, and a full job pool is waited out while jobs finish
  ([crash guards](docs/crash-guards.md)).
- **A mod's own weapon mesh in the right place on every LOD and its shadow.** A data spec's `mesh` put the shadow 20 cm
  off the gun on an Adjudicator copy (4.5 cm on a Liberator), and hung a Liberator copy's LOD2 on its fire selector. Each
  render and shadow mesh now holds the geometry where the first render mesh does, on the weapon's main bone. With the new
  `"unitSpace": true` the OBJ keeps its own coordinates, the frame the weapon's attachment points are in
  ([data specs](docs/modding/data.md)).
- **A crash in missions with edited spawn weights (`game.dll+0x9530F5`).** A roster weight of 0 that left an enemy
  with no weight at a level, such as Scavengers at difficulty 10, made bug holes spawn a null enemy. Such an edit is now
  refused, also when only planets with a given tag lose it. For writes that bypass the check, the spawn and bug-hole
  queues skip a null enemy, and the game's other unchecked picks take a row of weight 0
  ([live editing](docs/internals-live.md#spawning)).
- **Silent explosions on copied rounds.** A round that explodes, copied from a weapon not in the loadout, blew up without
  a sound, because the sound comes with that weapon's package. HellForge now loads the package (#298).
- **Playing with no internet connection on Windows.** With Wi-Fi off or the cable out, Play stopped at the network
  block with "not stopping the game's connections to the internet (error 10065)", asked for administrator consent and
  failed again. It still did on a PC that keeps a network route while offline. Offline, the block is now proved by
  the PC having no route out, or by its refusing a UDP datagram to the internet. While the game runs it is checked
  again every 10 seconds, and HellForge stops the game if it does not hold (#300, #317).
- **Code mods stuck on "did not compile" after the mod tools were damaged.** An antivirus that removed files from
  HellForge's Zig made every source mod fail in Zig's own libraries ("sub-compilation of libzigc failed"). That failure
  was kept as the mod's, so restoring Zig or running Check and repair changed nothing. HellForge now notices a damaged
  Zig, unpacks it again from its download and builds the mod. Only a mod's own errors are kept, and Build now always
  compiles again (#301).

### For contributors

- **Nightlies have a Linux installer:** `HellForge-Nightly-Linux-Installer.tar.gz`, once this workflow is on `main`
  (#309).
- `tests/HellForge.LocalBackend.Tests` needs the game's catalogues: `HELLFORGE_TEST_CATALOGUE` names a folder with
  `progression-data.json` and `item-catalog.json` (an installation's `catalogue` folder after a sync). Without one the
  suite is skipped with a warning. CI takes them from a private test-data repository
  ([CONTRIBUTING.md](CONTRIBUTING.md#building-and-testing)) (#296).
- A "Docs lint" CI job keeps docs and code comments readable. It checks page types, paragraph, sentence and page
  sizes, Markdown structure, links and filler words, plus code comment blocks of over 10 lines. Existing text is in a
  baseline, and a pull request sees what it adds ([CONTRIBUTING.md](CONTRIBUTING.md#writing-docs-and-comments))
  (#277).

## 0.4.0-beta (2026-11-05)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.4.0-beta). Engine 0.1.47, mod SDK 16.

### Added

- **Experimental: change mods without restarting** (Settings > Experimental, off by default). Stats mods, data mods
  and code mods that declare `"live": true` switch while the game runs, on the Mods page and when you join a friend
  with other mods (APPLY AND JOIN). Whatever cannot change live waits for a restart, as before. Mod SDK 16:
  `hellforge_mod_unload`, `load_phase`, `unhook` and `HF_EVENT_MODS_CHANGED` ([live mods](docs/modding/live.md)) (#65).
- **Experimental native Linux launcher and setup.** The launcher runs natively while the game stays under Proton in
  HellForge's offline network namespace. Setup confirms Steam ownership before copying the game and fetching the
  mod tools; Settings can remove the HellForge installation and its desktop shortcut without deleting the
  Proton prefix (#90).
- The Mods page shows what a code mod says it is doing ("In game: ..." during a session, "Last session: ..." after it),
  for mods that tell it (SDK 16) (#84).
- **Weapons level up.** Your primary gains the mission's XP, won or lost, and mod weapons do too. A friend who joins
  your game doesn't earn weapon XP yet (#297).
- **Many more mod weapons.** Mods can go past the old limit of about 20 weapon copies. HellForge moves the game's item
  catalogue at start-up, so it keeps 40,960 items and 1,024 progression packages (#297).
- 512 new damage types and 512 new projectiles across all mods (#297).

### Changed

- **Linux: co-op over Steam relays is on by default.** Without it a Linux session finds lobbies but every join times
  out. It now starts on for everyone (an earlier "off" that was only the old default is not kept), and turning it off
  in Settings > Multiplayer is still your choice. While it is off, or could not open (no `pasta` or `python3`: the
  session now starts without co-op instead of refusing), the lobby browser says so on each lobby and asks before a
  join; yes turns it on in the running game, through the launcher, and keeps it on for later sessions. Your own Public
  lobby is not listed while nobody could join it.
- **Public lobbies open up: you decide which mods you trust.** With your game sandboxed, a stranger's lobby running mods
  that run code or carry unchecked data is no longer out of reach: MOD SYNC asks you to TRUST each one, after a warning
  of what the sandbox doesn't stop. A mod you trust is stamped with its content hash and downloads without asking from
  any lobby after that, so Quick Play can take you straight in. Your trusted list lives with your mods, so updating
  HellForge leaves it alone, and the Mods page takes a trust back. Without the sandbox, a stranger's such mods are still
  yours to install. HellForge's verified list is now just its own mods, built into each release; nothing is fetched
  for it any more. Hosting, the mods you share reach anyone who joins and trusts them, so turn sharing off before
  hosting with something you haven't released.
- **A trust given in a sandboxed game holds only in the sandbox, a Steam friend's mod too.** Any mod in the game can
  answer the MOD SYNC prompt as you would, so a TRUST from inside a sandboxed game no longer lets a mod run unsandboxed
  later; the warning and the Mods page say so. Trust it again outside the sandbox (unsandboxed, or on the launcher's
  prompt) to play it there. Trusting a mod again never narrows a trust you already gave it.
- **Quick Play and SOS go to a lobby you can join first.** The game's own search leaves out a stranger's lobby whose
  mods wait for your trust while other lobbies need none, so Quick Play doesn't stop at a TRUST prompt; when it is the
  only one, it is still found. The lobby browser lists them all, as before.
- **A new look for the lobby browser.** The terminal's lobby browser is now laid out like the game's own Social
  menu, in HellForge red: lobby cards two by two in pages, each with a coloured edge for what joining it takes, the
  filters on the left, QUICK PLAY on the right and the game's hint strip along the bottom. Clicking a card (or Enter)
  opens the lobby's pop-up with its squad, what joining changes and JOIN; Space is Quick Play (#245).
- **The lobby browser shows what you can join first.** Lobbies you can join are at the top, and lobbies no setting can
  let you into are hidden while there are others; Show can't join (or H) shows them. Each row says in a word or two what
  keeps you out ("Needs sandbox", "Host must update", "Needs mods") instead of a sentence cut short. A lobby's details
  list every mod in the way that fits, and say how many more there are instead of cutting names off. When no lobby
  can be joined, the browser says so, names the mods most of them need, and suggests hosting a Public lobby.
- The lobby browser API lists the mods that keep the most
  lobbies out (`needed`) and gives lobbies you can join first.
- The armory holds 256 cards of each type, vanilla weapons counted. That leaves room for 199 new primaries, 229
  secondaries and 81 throwables, and about 100 copies each of lasers, shotguns, plasma and melee weapons. A weapon past
  that is refused by name in the mod's log, so a vanilla weapon never falls off the list (#297).
- Every mod weapon has its own level, XP and attachments, starting at the lowest level, instead of copying its base's.
  Attachments can be bought on a mod weapon's page, and the Weapon levels card lists mod weapons. A removed mod leaves
  your vanilla progress as it was, and adding it back brings its progress back (#297).
- After a crash the Play page says "The game crashed." with the game's exit code and what it means, instead of
  "Session ended.", and offers Save logs to Desktop beside Save crash report (#84).
- Sessions that ended in a crash keep their logs past the newest ten, the five newest of them, so a crash report can
  still be made a few sessions later (#84).
- A crash leaves a small dump in the session's logs (the game's threads and the memory they point to, not all of
  it), also without the Crash dump setting, which now only makes it a full memory dump. Save logs to Desktop includes
  it (#84).

### For mod makers

- SDK 16: `set_status`, a mod's line for the player, which the Mods page shows on its row (and `mod-status.tsv` in the
  session's logs); and `HF_EVENT_SESSION_END`, the session's end, when the launcher stops it (the session runner waits
  up to 2 seconds for the mods) or the game quits normally (#84).
- `modloader.log` gives each code mod's version and its DLL's SHA-256, and, for a DLL that does not load, Windows'
  error ("error 126: The specified module could not be found.") (#84).
- `modloader.log`, each mod's log, `steam-cloud.log`, `session.log` and `backend/multiplayer.log` start with the local
  date and time they were opened and the offset from UTC: their lines give only a time of day (#84).
- The crash note in `modloader.log` also covers heap corruption, stack overflows and integer divide by zero, and notes
  the first four faults of each kind instead of the first four in all (#84).

<!-- markdownlint-disable-next-line MD024 -->
### Added

- **Medals and spending what you earn.** A won mission now pays medals (2 at Trivial up to 10 at Super Helldive), and
  the operation's card on the war table shows what its missions pay. Medals unlock warbond pages and items, requisition
  buys stratagems and store items, samples buy ship module upgrades, and requisition buys weapon attachments. Each
  purchase is saved to your profile. Before, claiming a warbond item failed with an error and medals could only come
  from the map. Premium warbonds still need to be owned in the normal game (#232).
- Mod weapons get a page in the in-game editor, in their base weapon's section. Setting `own_all = 1` on the weapon
  (`HfWeapon`) gives it its own copy of every record its base has, so the page shows everything the editor can change
  while the game runs. Without it, the page shows only what the weapon owns, and a weapon that owns nothing gets no
  page. Editing a mod weapon's page never changes the weapon it copies. (#177)
- With the sandbox on, the session log now names what the sandbox refused the game, by the overlay or module that
  asked (Steam's, NVIDIA's, Discord's, RivaTuner's overlays and others), and `result.json` lists each object. This is
  what tells which overlays can be given what they need.
- The ship's information terminal, across from the Armory, now opens a lobby browser instead of an error about
  Arrowhead's news service. It lists every HellForge lobby on any planet and difficulty, with what joining each takes
  (ready, a download, your trust, or why you can't), filters for joinable, friends' and difficulty, and joins with the
  lobby's invitation. Using the terminal, the camera glides up to its screen and the browser fills it. Ctrl+L in the
  in-game editor opens it too. QUICK PLAY joins the best lobby you can join, a friend's first. Lobbies a setting
  hides (strangers' with Play with strangers off, or ones with host-enforced stat changes) are listed with the setting
  that lets them in, which you can turn on from the browser for that session.
- The Play page says when your premium content (warbonds, Superstore items, DLC gear you bought) isn't in the profile
  because it was never synced, or its sync was for another Steam account, and syncs it from there. A synced profile
  gets an "Update profile sync" button beside PLAY for after you buy or unlock something new (#206).
- Premium warbonds you don't own look locked in Acquisitions: their art is darkened, with a padlock in the middle and
  lines in from the corners, and their pages show nothing bought (an unlock-all profile showed every item there as
  bought). Ownership follows your profile's sync, the same as what the game lets you use, so a warbond unlocks there
  once you buy it in the live game and sync again. HellForge draws this from your own game copy when a session starts
  (#206).
- Super Credits become Forge Credits, HellForge's own currency: the pickup on the map shows the Forge Credit card and
  the currency icon is the Forge Credit's. HellForge draws them from your own game copy when a session starts (#224).
- Forge Credits are earned: a pile picked up on a mission pays everyone in the squad the same amount, as Super Credits
  do in the live game, and the Super Credits on warbond pages pay Forge Credits. Your balance shows where Super Credits
  did. On every profile but unlock-all, a premium warbond you own in the live game now opens once you buy it with Forge
  Credits (its Super Credits price); a warbond you don't own still can't be bought. Unlock-all profiles open your owned
  warbonds as before (#225).
- Medals and requisition slips picked up on a mission now pay out the same way, with the card on the right, for everyone
  in the squad. Every player keeps their share, not only the one who picked it up, and a pile pays once, also after a
  restart. Premium warbonds your profile's sync shows you don't own show WARBOND LOCKED instead of a buy button
  (#224, #225).

- Difficulty 11's SEAF Artillery side objective now has only Explosive, High-Yield Explosive and Mini Nuke shells,
  and a quarter of them are nukes. Napalm, Smoke and Static Field shells don't spawn there
  (`HF_DIFFICULTY_HEAVY_ARTILLERY`, on in the Hell Forge difficulty).

- **Nightly builds** for testers: each night that the `dev` branch changed, an untested build of it is published on
  GitHub as the Nightly pre-release (`HellForge-Nightly-Setup.exe`). Its version, as the launcher shows it, reads
  `-nightly.<date>` (`0.4.0-nightly.20261102`), so it is never taken for a release. Releases stay the builds to play
  on ([CONTRIBUTING.md](CONTRIBUTING.md#nightly-builds)) (#211).

<!-- markdownlint-disable-next-line MD024 -->
### Changed

- The in-game editor says when a stratagem change takes effect, as the game reads it: cooldown and call-in time from
  the next call, the Orbital Laser's tracking speed, search radius and re-search interval at once (also on a laser
  already firing), its duration from the next call-in, and uses from the next mission. It said "next mission" for all of
  them (#234).
- A lobby that refuses you for another HellForge version now says whose HellForge is older: the host needs to update,
  or you do.
- Difficulty 11 pays 50% more XP and medals than Super Helldive: 2250 XP (was 1800) and 15 medals per won mission (#232).
- The Mods page's second group is called CLIENT ONLY instead of COSMETIC: besides cosmetic packs it holds Lua mods,
  which aren't cosmetic. Nothing about these mods changed; friends still don't need the same ones (#180).
- **Setup gets the supported game build from Steam itself.** When Steam has another HELLDIVERS 2 build, setup offers
  three ways to get the build HellForge runs for your own account. Scan a QR code with the Steam app on your phone
  (the default): setup downloads the build from Steam's servers and copies whatever your Steam install already has.
  With no phone, sign in with SteamCMD: setup gets Valve's own SteamCMD and opens it with everything filled in, and you
  type your password into SteamCMD's window, never into HellForge. Or use the Steam app on your PC, which downloads
  it through its console. Every file is checked against Steam's hashes, and nothing of a sign-in is kept: SteamCMD's
  remembered sign-in is removed after it runs, and SteamCMD itself once the build is in place. Setup also finds the
  Steam app's download in the Linux client's `steamrt64` folder. HellForge no longer downloads build patches from
  files.hf-mod.com, so it hosts no game files. When Steam won't allow the download (a game borrowed through Family
  Sharing), when SteamCMD can't be set up, or on Linux, setup shows the Steam console commands (#205).
- **Modded co-op is much easier to get into.** Only mods that change what every player's game needs must still match:
  - Stats-only mods, and mods whose patches change only textures, materials, sounds, fonts or text, are now each
    player's own ("PER PLAYER" on the Mods page). Joining a host who runs different ones works, and a short note
    names them. A mod's author can set this with the manifest's new `"coop"` field.
  - HellForge's bundled mods count by their co-op level, not their files, so most HellForge updates no longer split
    players.
  - **Play with strangers is now on by default while your game is sandboxed**, and off otherwise. A choice you make in
    Settings > MULTIPLAYER still wins, and "Use default" goes back to it. A setting an older HellForge saved is kept
    (they saved off unless you turned it on): "Use default" switches to the new default. The first time the default
    applies, the launcher shows the stranger warning once.
  - **Players on HellForge 0.3 still play with you.** Their lobbies show on the Galactic Map and in the lobby browser,
    yours show in theirs, and joins work both ways, MOD SYNC included. A lobby with Hell Forge's difficulty or Vanilla
    Plus on is not listed for 0.3 players, though 0.3 friends who run the same mod still join it. Versions before 0.3
    can't join you, and you can't join them.
  - A mod's author can give it a `"coopLevel"` in its manifest: players whose copies have the same level play together,
    whatever version of the mod each has, so a fix release no longer splits them.
  - With more than 50 HellForge lobbies listed (the most Steam returns in one search), lobbies you can join right away,
    with your mods or after a download, are searched for and shown first.
- A code or raw-data mod that mod sync downloaded now plays only while HellForge's verified list names it or you trust
  it: one taken off the list, or one you untrusted, waits for your trust again.
- Mods on HellForge's verified list download from any host, not just Steam friends.
- `.rar` and `.7z` mods work everywhere: HellForge unpacks them itself, where it used Windows' `tar`, which could only
  on Windows 11. A `.rar` or `.7z` dropped straight into the mods folder is unpacked into a folder too, instead of being
  ignored.
- Sessions start a few seconds sooner: the in-game editor's catalogue is built once instead of on every check (#147,
  #149).
- The in-game editor's catalogue is split into a file per category. As one file it had reached 48 MB of the 64 MB the
  editor reads, and it grows with every category the editor gains. If a catalogue file can't be read, the editor now
  names the file and says why (missing, too large, or from another HellForge), instead of always saying it's missing. (#176)
- Mods together can add 512 weapons and entities, up from 192. Past 2,048 network types, HellForge widens the game's
  network type index from 11 to 12 bits for that session, and only then, so smaller mod sets play exactly as before.
  Weapons still run out sooner (the game keeps 4,096 catalogue items), and an entity whose base uses a small game table
  can still be refused; each refusal is in the mod's log.
- The game tables weapons and stratagems use grow when mods would fill them: HellForge builds a full table again with
  twice the slots its rows need and rewrites the game's code that reads it, so large weapon packs are no longer refused
  for want of rows (the loadout index alone stopped at 193 more weapons; 300 copies of a weapon grow 13 tables). Other
  tables still refuse what does not fit.

### Fixed

- **Linux: controllers work in a sandboxed game.** The sandbox's device folder held no input device at all, so no
  controller of any kind reached the game, only keyboard and mouse. It now holds the game controllers connected when the
  session starts (wired or Bluetooth), and still no keyboard, mouse or virtual input device: those stay out, so code in
  the game can't read what you type elsewhere. A controller connected while the game runs needs the next session
  (#254).
- **Linux: after MOD SYNC's Restart, a sandboxed game starts again.** The game closed and the launcher never started it
  with the host's mods: the sandbox stopped the game before the session noted why it stopped, so the launcher did not
  know to restart it. The session now notes it either way. The game ending because HellForge stopped it (STOP, or a mod
  sync) is no longer reported as a crash.
- **Linux: joining a host whose mods include a source mod no longer loops.** After MOD SYNC downloaded the host's mods
  and restarted the session, a source mod among them was never compiled (Linux builds source mods before the session,
  and only the player's own were built), so the session left it off, the host refused the join again, and MOD SYNC
  asked for another restart. The build before the session now builds the host's mods too.
- If your game was launched straight into a lobby, weapons other players' mods added could stay dead on your screen for the
  whole session. They now work however you joined.
- With the sandbox on, the game could freeze for 30 seconds as it started, and sometimes crash (0xC0000005 in dbgcore.dll), on
  a PC with a tablet driver: its Wintab32.dll faults as it loads, and the session's crash dump waited for it (#235).
- On Linux, Sync Inventory said "Sign in to Steam first" whenever its Steam check failed for any reason, and left no
  log. It now gives the check's own reason, keeps the last refusal in `logs/sync-refused.txt`, and won't start while a
  HellForge session that the launcher didn't start itself is still running (#112).
- On Linux, a session's sandbox check could test a different Steam Linux Runtime layering from the one the session
  runs, and then said "the self-test ran in host mode" even when only the runtime differed. The check now tests the
  session's own choice, names both runtimes when they differ, and keeps the self-test's full output in
  `logs/sandbox-self-test.txt` (the launcher's own output goes to `logs/sandbox-readiness.txt`) (#112).
- On Linux, a game that crashes as its window opens because Proton's Wine lacks a text-input function the game needs
  (seen with a Wine 10 Proton) now says so after the crash, and that Proton 11 or Proton Experimental fixes it (#112).
- Linux: on a new Proton prefix the sandbox self-test failed during setup and was never run again, so sessions ran
  unsandboxed with "the self-test did not report its runtime mode". It now runs after the prefix is made
  case-sensitive, a failure says its real reason, and Check and repair tests it again (#112).
- On Linux, with HellForge Difficulty on, the game crashed when a mission was selected, at the hellpod, or when the
  ship started moving to another system (#21, #166). A diagnostic of HellForge's own, which watches each new map with
  the processor's debug registers, did not recognise its own traps under Proton and handed them to the game, which
  ended. The diagnostic is now off unless it is turned on (`HELLFORGE_DIFFICULTY_FIELDWATCH=1`), which also ends the
  short freezes at Esc and mission select. When on, it recognises its own traps under Proton, turns itself off once it
  has what it needs, and stays off on a PC where the self-test it runs first fails (#124, #127, #197).
- Sync Inventory's "The capture could not start: see proxy.log" now says why and what to do: a security program
  blocking or stopping the capture (named, with the exact file to add to its exclusions), another program on port 443
  (named), the certificate refused, no internet, or the administrator prompt declined; a hosts file problem (below)
  also names the security program to allow. A capture slow to start is reported after a minute. An
  earlier sync's capture still running is stopped by the next sync, so restarting the PC is no longer needed for it
  (#160).
- In a sandboxed session the in-game editor dropped the EDITED mark from your saved changes once you closed and opened
  it again, as if they were gone. They were kept, and they now show as saved (#234).
- The lobby browser and Quick Play no longer offer a full squad of 4 as joinable ("4 / 8"): the game's lobby has 8
  places, a squad 4. A full squad's listing now has no free slot.
- Armory icons that go blank (the FAQ's known issue): with `--toaster_mode` in Settings > Launch options they never
  appear, because it turns the game's texture streaming off and the game draws its Armory icons only once their
  textures have loaded. The launcher now says so under the box and when the session starts. For every other case,
  `modloader.log` now names the icons the game stopped drawing, so a Save logs zip shows what they wait for.
- A mission whose report listed heavy loot got a reply the game refuses, so that mission's rewards screen could come
  up empty. Heavy loot is still added to your profile (#232).
- A mission is paid once. If the game sent the end of a mission again (its reply lost), it paid the XP, requisition,
  medals and samples a second time. Thanks to diemitze, whose PR #27 found it (#232).
- Acquisitions lists the warbonds newest first, as the live game does. It listed them oldest first, with Helldivers
  Mobilize at the top (#206).
- On Windows 10 21H1 and older or unpatched Windows 10 PCs, HellForge Setup closed without a word, leaving ".NET
  Runtime" and "Application Error" entries (0x80131506) in the event log. On a newer processor HellForge's programs need
  Windows updates from October 2024 or later. Setup now starts on such a PC and says which Windows it has and what to
  update to, and can open Windows Update (#14).
- HD2R Editor (HD2Runtime's own in-game editor) doesn't work on HellForge, but HellForge loaded it anyway, on the same
  F8 key as its own editor, with nothing to say why. The Mods page now marks it UNSUPPORTED and keeps it switched off,
  so it can't take F8 or slow the game down, and says to use HellForge's own editor (F8 in game) instead. HD2Runtime
  and the Lua mods made for it still run.
- Big frame drops with a Difficulty 11 mod on: HellForge left a developer's diagnostic for the war table's map
  previews running, which interrupted the game each time it read the map's size. It is off now, so Difficulty 11
  mods don't need to be switched off any more.
- A mod stratagem's page in the in-game editor could edit the vanilla stratagem's entity or projectile: it showed the
  fields of records the mod's entity still shared with its base, and of a vanilla projectile the entity fired. The page
  now shows only what the mod owns. (#177)
- A mod entity with `own_all = 1` (`HfEntity`) refused live edits (`set_live`, the in-game editor's saved values) of
  any record it hadn't also named in `own_components`, saying it had no record of its own when it had one. (#177)
- New profiles (Starter) had difficulty 1 locked: no level-1 missions showed on the war table, and the difficulty
  bar jumped to the highest level when you left a planet. Every profile now has it unlocked (#230).
- New profiles (Starter) had an empty Ship Module page ("under construction"). Every profile now has the six ship
  module tracks (#233).
- On some PCs every session stopped at Launching with "The game could not be prepared for the local backend", while
  the normal game started fine: the game got past the point where HellForge edits it before HellForge froze it. HellForge
  now checks for that point more often and freezes the whole game in one step, rather than one thread at a time after
  listing them.
- With the sandbox on, the game could stop at STEAM NOT DETECTED while HellForge said the session was running. The
  game reaches Steam through HellForge's Steam broker, which was started after the game and could be late, for example
  while an antivirus checked it, or could fail to reach Steam. The game now starts only once the broker has Steam
  (the launcher shows "Waiting for Steam..." meanwhile, and Stop ends the wait). If
  the broker can't reach Steam, PLAY stops and says why (Steam not running or signed in, or running as administrator),
  and the session log says if the broker stops during play.
- The network census in every session wiped the game's error code after each call it watches (connections, name
  lookups, programs started): a failed connection read back as no error.
- A game that crashed while starting showed "The game could not be connected to the local backend.", as if HellForge's
  backend were the problem (#203, #212). It now says the game closed while starting, with its exit code and, when
  Windows recorded the crash, the file the game crashed in. When that file belongs to another program that puts itself
  into games (an overlay, a recorder, RGB or security software), the message names it as the one to close. The crash
  report carries Windows' record too, also for crashes later in a session.
- When Windows 11's Smart App Control blocked HellForge, the launcher showed only Windows' "An Application Control
  policy has blocked this file". It now says which file Windows blocked, that it is because HellForge's files aren't
  signed yet, and what you can do, including that Windows may not let you turn Smart App Control back on once it is
  off. Setup and the launcher warn before you install or play while Smart App Control is on, a mod whose DLL Windows
  blocked says so in its log, and the session log says whether Smart App Control was on.
- With the sandbox on (Settings > Sandbox), a Steam Controller and other controllers that work only through Steam
  Input did nothing in the game, and nothing said why (#94, #204): Steam Input can't reach a sandboxed game yet. While
  the game runs, the launcher now names the controller Steam Input is handling and says it won't work, and that Steam
  may send its desktop keys instead (A presses Enter, which opens the chat). Xbox controllers work. Settings > Sandbox
  says so too.
- The game sometimes closed its window but kept running, and had to be ended in Task Manager. If the game is still
  running 30 seconds after its window closes, HellForge now ends it and finishes the session as STOP does, so your
  settings and saves are still kept. Ending the session runner from Task Manager no longer leaves HellForge's backend or
  Steam helper behind, and Save logs to Desktop lists which of the session's programs were still running.
- "The game needs a Microsoft runtime this PC does not have" (the game's MSVCR110.dll, from the Visual C++ 2012
  Redistributable): setup and PLAY now download Microsoft's installer and install it themselves, after Windows asks for
  permission. You only get the download link when that fails, with the reason. This also covers installations set up
  with 0.2, a runtime Windows lists as installed whose DLL is gone (it's repaired), a 32-bit copy of the DLL put in
  System32 by hand, and the newer runtime's VCRUNTIME140_1.dll, which an older 2015-2019 install doesn't have.
- Every start sat on a black screen for minutes if you ended your sessions with STOP or by closing the launcher: the
  game saves its shader cache only when it closes by itself, and STOP ended it without asking. STOP now asks the game
  to close and waits up to 30 seconds for it before ending it (#194).
- The first start of a new installation built the shader cache from nothing even when the normal game already had
  one. A new installation now starts from the normal game's shader cache. While the game builds one, the launcher says
  "Building the shader cache (first start only, this can take a few minutes)..." instead of "Running" (#194).
- Premium warbond stratagems could be used without owning their warbond, for example from an unlock-all profile. A
  stratagem's loadout entry was served on its own, without the warbond's token, and the game took it as the stratagem.
  It now follows the stratagem, and the same goes for a premium weapon's attachments.
- Sync Inventory failed with "progression.location.warId must be an integer from 0 to 4294967295" when the ship
  wasn't at a planet in the normal game. The import now takes the ship's name, and the profile keeps its own location.
- On a standard (non-administrator) Windows account, Sync Inventory never opened the game, or said "The capture
  finished without an inventory" after a capture that worked, until the launcher was run as administrator. The capture
  runs as the administrator who answers the prompt and kept its folder to that account; it now shares it with yours.
  A sync that ends without an inventory now also says whether the game never started, closed before logging in, or
  ran without reaching the title screen, and says so while the sync still waits.
- Sync Inventory failed at once when Windows' hosts file was read-only, missing, or held open for a moment by another
  program such as an antivirus scan (#160). The sync now writes a read-only hosts file and sets it read-only again
  straight after, so it stays read-only even if the sync crashes; creates a missing one and deletes it afterwards; and
  waits a few seconds for a file another program holds. When the change still can't be made, the sync says why: which
  program holds the file, or that security software (Microsoft Defender's SettingsModifier:Win32/HostsFileHijack)
  blocked or undid it. Clean up says the same instead of asking you to edit the hosts file.
- During Sync Inventory the normal game showed "Could not connect to Easy Anti Cheat network" after the login and
  closed itself, and you had to close it when it stayed open. HellForge now closes the game itself as soon as your
  inventory is in, says so, and removes the sync's hosts line and certificate right away. If it can't close the game,
  the launcher tells you to close it; your sync is already saved.
- A modpack with many new weapons lost every mod stratagem, and could lose difficulty 11 or leave a weapon with only
  some of its attachments: the game keeps only 4,096 items, and each weapon brings all of its base's attachments (57 for
  a Liberator copy). Difficulty 11 and stratagems now go in first, then each weapon whole, and what does not fit is left
  out with a line in the log.
- With more than about 150 mod weapons, the rest and every mod stratagem never reached your inventory: the list of
  them HellForge hands its backend was cut off at a fixed size. A quote in a mod weapon's name also broke that list.
- Past 64 mod weapons and entities with models of their own, the rest were not set up for destruction.
- A large weapon pack used up the room for mod text, so mod stratagems showed their base's names.
- A copy of an entity with a decoration, encounter or ship prop visibility component could be written past its table,
  into the game's records: HellForge had those tables' sizes wrong.
- On difficulty 11's larger maps every base, outpost and objective sat in a ring near the edge around an empty middle.
  The game puts all its location clusters on one circle at 60-80 % of the map's radius, and the locations stay as
  many and as far apart as on a level-10 map. Every other cluster now goes on an inner circle at level 10's distance,
  so the middle has bases again. Every player still generates the same map from the same seed.
- City buildings on difficulty 11's larger maps may now stand up to the border, not only inside the level's playable
  radius, as the enemies there already spawn (`HELLFORGE_D11_TOWN_BORDER=0` keeps the old radius, for comparing).
- Small towns on difficulty 11's larger maps came out as scattered flat pieces at different heights instead of one
  flat town. Two causes, both from a larger map's cells being further apart. The game picks a map's town cells from a
  noise pattern measured in metres, so each town broke into single cells; on larger maps the pattern is now read at
  level 10's spacing, so a town keeps level 10's cells, each larger. And the ring of cells the game puts around each
  town cell still used level 10's size, so two other cells sat between every pair of town cells and each was
  flattened on its own; the ring now uses the larger cells' size, so a town's cells touch and form one flat area
  (`HELLFORGE_D11_TOWN_NOISE=0` and `HELLFORGE_D11_TOWN_RING=0` keep the old behaviour, for comparing).
- On the war table, a difficulty 11 map's low ground sank into the table at the drop zone view and showed black until
  zoomed in: the hologram lifts the terrain by its scale, a formula made for level 10's scales only. It now reads the
  scale a level-10 map would have, so every map sits above the table as at level 10.
- Small towns on difficulty 11's larger maps lost most of their outer walls: a wall piece is placed only where a town
  edge lies within 25 m, sized for level 10's smaller cells. On larger maps the reach grows with the cells
  (`HELLFORGE_D11_TOWN_WALLS=0` keeps 25 m); the session log counts the wall pieces tried and refused.
- Mega cities on difficulty 11's larger maps had streets with bare road and lone junction pieces, especially toward the
  edge. The game dresses a street (the pieces that finish its ends, and its props) only up to 30 m past the playable
  radius, where level 10 stops building; on difficulty 11 buildings stand all the way to the border, so their streets
  are now dressed that far too (`HELLFORGE_D11_TOWN_BORDER=0` keeps the old radius). Streets are also 1.5x longer
  there but kept level 10's number of props per street; they now get 1.5x as many (`HELLFORGE_D11_STREET_PROPS=0`
  keeps level 10's).
- Mega cities on difficulty 11 needed more destructible records than the world's map of them holds (about 33,300 of
  32,768), and the ones registered last were left without one. On larger maps the map now has 65,536; the session log
  shows how many a mission used.
- On difficulty 11's larger maps the ICBM silo's lower platform, where you raise the manual locks, could sit under the
  map's water, and other sunken objectives could too. The game shapes each patch of ground by its distance from the
  nearest road, reaching its lowest point 64 m out. Larger maps keep level 10's number of patches spread wider, so
  far more ground reached the bottom, including under objectives. That 64 m now grows with the patches
  (`HELLFORGE_D11_TERRAIN_RAMP=0` keeps 64 m, for comparing), which also takes away the flat terraces between roads.
  The ground within an objective's footprint also never goes below 0 m, above every planet's water
  (`HELLFORGE_D11_DRY_OBJECTIVES=0` turns that off); the session log counts the heights raised.
- A difficulty 11 mission could crash while loading when its map had more than 512 mines: the game copies every mine
  into a list of 512 without checking, and the 513th overwrote the list's count. Mines past the 512th are now left out
  (no mine there), and the session log counts them, as it already did for the enemy director's 128 spawners.
- The SEAF Artillery side objective's shells fell short of far targets on difficulty 11's larger maps: they reach
  about 710 m, enough for a level-10 map but not for one twice as wide. In difficulty 11 missions the shells now fly
  faster with less drag, so their whole arc grows with the map (about 1,420 m on a 2x map), and the gun aims for it as
  it already did. Other levels keep the game's shells.
- A difficulty 11 city map could still crash when the game's 32,768 destruction records were full: a second,
  separate registration wrote through a null slot. It now leaves that destructible without a record, as the first fix
  does, and the session log counts them.
- With the sandbox on (Settings > Sandbox), a player who joined a squad could reach only the host's game, never the
  other players who joined: the sandbox let the game's Steam connection through to the host alone. In a squad of three
  or four, each player's game now reaches every other player in the lobby too. This is a likely cause of the known
  issue where a third player joining a lobby of two pushed the second player out (#202). Every sandboxed player in the
  squad needs this version: on 0.3.0-beta, the sandbox still lets that player's game reach only the host.

<!-- markdownlint-disable-next-line MD024 -->
### For mod makers

- SDK 16: game functions by name. `HF_HOOK(mod, SpawnEntity, detour, &original)` hooks a game function HellForge has
  mapped (`mappings/functions.json`, generated into `hellforge_functions.h`), and `hf_fn_<Name>(mod)` gives one to
  call. The compiler checks the detour against the function's signature, IDEs complete the names, and HellForge keeps
  the addresses and checks, so a game update doesn't mean rebuilding the mod. A hook can be asked for from
  `hellforge_mod_init`; HellForge puts it in once the game's code is ready. The first ten names: the damage consumer,
  kill dispatch, beacon activation, entity spawner and explosion request to hook or call, and five functions behind
  the SDK's actions to call ([docs/modding/hooks.md](docs/modding/hooks.md#hooking-by-name)). `hook`, `hook_ex` and
  `find_pattern` work as before (#179).
- Game globals by name: `hf_global_ExplosionQueue(mod)` gives one of the game's objects (its managers and queues,
  what the functions above take) as a struct with its named fields, so `queue->count` instead of an offset. 15 to
  start (the players, the local user, the game mode, entities, health, motion, explosion, projectile and combat queues,
  scores, wielders, inventories, destructibles), each checked against an instruction of the game's code that loads it
  ([docs/modding/hooks.md](docs/modding/hooks.md#game-globals), #179).
- Typed events: `hf_on_spawn(mod, listener, user, priority)` and one call per event, whose listener takes the event's
  own struct (`const HfSpawnEvent*`), so there is nothing to cast and a listener for the wrong event doesn't compile.
  Only `hf_on_damage_pre`'s struct is writable. The load events are under the same prefix (`hf_on_game_loaded` and the
  rest), and so is the game thread: `hf_on_game_thread`, which answers 0 when it runs, as every `hf_on_` call does.
  They come with SDK 16's header and run on any HellForge from SDK 7 on; `subscribe` and `on` work as before
  ([docs/modding/runtime.md](docs/modding/runtime.md#game-events), #179).
- A renamed game function or global keeps its old name as an alias, so mods written or built with it keep working (#179).
- `HfSyncMod.flags` (the field that was `reserved`): `HF_SYNC_FLAG_STRANGER` marks a mod whose host is not the player's
  Steam friend, so an editor can warn harder before `HF_SYNC_TRUST`; `HF_SYNC_FLAG_SANDBOX_ONLY` marks every mod of an
  offer to a sandboxed game, where a trust answered with `sync_answer` counts only in sandboxed sessions.
- SDK 16: the lobby browser. `browse_state`, `browse_lobby` and `browse_ask` give a mod the list of every HellForge
  lobby with what joining each takes, and join one; `HfBrowseState.terminal` counts the times the ship's information
  terminal opened the browser, and `HfBrowseState.camera` where the terminal's camera is (`HF_BROWSE_SHOWN` and
  `HF_BROWSE_HIDDEN` keep it on the screen while a browser shows). `HF_BROWSE_QUICK` joins the best lobby, and
  `HfBrowseLobby.fix` names the setting that would let a lobby in (`HF_BROWSE_ALLOW_STRANGERS`,
  `HF_BROWSE_SHOW_EDITED_STATS`, which hold for the session and are never saved; show the player the launcher's warning
  before turning strangers on). A mod that draws a browser of its own asks `HF_BROWSE_CLAIM`, and HellForge's in-game
  editor then shows none (`HfBrowseState.owner`). How to build one: [docs/lobby-browser.md](docs/lobby-browser.md).

### For launcher developers

- `HellForge.Cli steam-download --out <folder>` downloads the supported build from Steam's servers for the account
  that signs in with a QR code (on standard error), as setup does; `--file` fetches one file of the build and
  `--dry-run yes` prints the plan only. `--sign-in steamcmd` lets Valve's SteamCMD download it instead (into
  `--steamcmd`, default `<out>-steamcmd`), which asks for the password itself (#205).
- A session's `result.json` can have `stuckQuitting` (the game closed its window and was ended 30 s later), `gameHung`
  (its window stopped responding for 15 s) and `gameAtStop` (STOP on a game without a window or with a hung one): each
  the game's threads by start address and state, and the Windows Error Reporting or crash reporter processes holding it.
  `jobActiveAtStop` counts what was still in the game's job when the session ended it. The logs zip has
  `<run>/processes-now.txt`.
- A session's `result.json` has `shaderCache` (`atStart` and `atEnd`: files, bytes and the newest write; `written`;
  `seededFromGame` when a new installation took the normal game's), `firstBackendRequestSeconds` (from the game's start
  to its first backend request) and, after a stop asked for, `gameClose` (`outcome`, `windows`, `closedByItself`,
  `seconds`). A stop asked for (`HellForge.Cli stop`, the stop signal) now gives the game up to 30 seconds to close by
  itself, unless it has no window or a hung one; `stop --now yes` (signal text `stop-now`) ends it at once.
- `HellForge.Cli mods-stage --sync` loads a synced code or raw-data mod only while it is verified or trusted;
  `--sandbox yes|no` says whether the session is sandboxed, which a trust marked `"stranger": true` needs.
- The backend's `mod-sync.json` offer has `"stranger"`, and a trusted entry in `hellforge-mods.json` can have
  `"stranger": true`.
- `HellForge.Cli mods` and each session's `mods-stage` unpack a `.rar` or `.7z` dropped into the mods folder into a
  folder first. One they can't add is listed in `problems` and stays where it is. `mod-add` no longer needs Windows'
  `tar` for them.
- A session's `result.json` has `prepare` and `connect`, how the two launch edits ended (`outcome`, the wait, and when
  `game.dll` appeared), and `windowsCrash`: Windows' record of a game crash (`module`, `offset`, `code`, `modulePath`,
  `owner`), or `none` (#203, #212).
- A sandboxed session's `result.json` has `steamInput` once the Steam broker has asked Steam: `controllers` (names),
  `types` (Steam's `ESteamInputType`) and `note` (what to tell the player, or null). `HellForge.Cli play` writes a
  `steam-input` event with the note when it changes; `SteamInputNote.Of` reads it.

## 0.3.0-beta (2026-10-26)

[Release](https://github.com/SkyeShade/HellForge/releases/tag/v0.3.0-beta). Engine 0.1.41, mod SDK 15.

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
- **Sync Inventory** (Profile > Account sync) brings your own HELLDIVERS 2 account's inventory and progression into a
  profile, after one Windows prompt. A review then shows each category (level and XP, medals, requisition and samples,
  warbond and armory items, ship modules, weapon levels, ship name, tutorial) side by side, and nothing changes until
  Apply. Warbond unlocks come as on your account, every item of the warbonds you own, or none; which warbonds you own
  always follows the sync. Your choices are kept per profile. The game stays connected until you close it, and a sync
  that cannot read your progression fails and keeps the last complete snapshot (#45). Experimental: during a sync the
  normal game shows "Could not connect to Easy Anti Cheat network" after the login and closes itself; the sync is
  complete by then.
- Profile backups: every import or restore first saves the profile, the newest 10 are kept, and Profile lists them with
  Restore backup (#45).
- Crash reports: after a session ends in a crash, the Play page offers Save crash report, a zip for a GitHub issue with
  the versions, Windows, GPU, settings and mods, the crash note and the logs, scrubbed of Steam ids, names and paths
  (a crash dump only if you say yes). Every `session.log` now starts with those facts (#70).

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
- Premium content (premium warbonds, Superstore items bought with Super Credits, and the Steam DLCs' gear) is in a
  profile only when your last Sync Inventory, for the Steam account signed in now, shows you own it. The unlock-all
  template no longer includes it (#45).
- Super Credits can't be earned, found on a map, imported or edited into a profile (#45).
- A new installation's first profile starts from the starter template instead of unlock-all (#45).
- Mods that recreate premium content follow your Sync Inventory: their weapons and stratagems are locked, and a
  cosmetic one isn't loaded, unless you own the original. The Mods page tags them PREMIUM or PREMIUM: LOCKED and says
  what is missing (#45).

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
- Mods together could add only 64 weapons and entities: in a large modpack the last ones were left out, such as a
  stratagem missing from the stratagem select. Now 192, the most the game's network allows (#79).
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
- Premium content: a weapon or stratagem a mod makes as a copy of a premium one is locked for players who don't own the
  original, and a manifest's `"premium"` list (`warbond:<id>`, `dlc:<name>`, `item:<mixId>`) declares other content a mod
  recreates or builds on ([docs/modding/concepts.md](docs/modding/concepts.md#premium-content), #45).

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
- `logs --crash-report <file.zip> [--dump yes|no]` makes the crash report of a session that crashed, and `result.json`
  gains `report`, the opening facts of `session.log` (#70).

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
