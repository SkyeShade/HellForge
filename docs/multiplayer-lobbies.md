# Public lobbies and strangers

<!-- diataxis: explanation -->

This page is for contributors working on public lobbies: the lobby directory on Steam, the Galactic Map's search, the
lobby browser, HellForge 0.3 players, and the Play with strangers setting that gates them all.

Players see public lobbies in [multiplayer.md](multiplayer.md). The rest of co-op is in
[Co-op internals](multiplayer-internals.md), and what a stranger's mods can reach in
[Co-op mod sync](multiplayer-mod-sync.md#trust-what-another-players-mods-can-reach).

## Public lobbies (the lobby directory)

Each backend lists the public lobby its game hosts as an Invisible Steam lobby of the game's app (553850), held beside
the game's own (`Multiplayer/SteamLobbyDirectory.cs`, `LobbyDirectory.cs`, `DirectoryService.cs`). The listing works on
the peer link's Steam thread.

All of this needs [Play with strangers](#play-with-strangers) effective for the session. Off, the Steam link accepts no
one who is not a Steam friend ([The stranger gate](#the-stranger-gate)).

### Listing

A lobby is listed only while:

- it is Public (privacy `number_key6` 0), not locked and not full;
- the player's `"listPublic"` switch is on (the Mods page, on by default);
- Play with strangers is effective on;
- Steam holds the listing lobby.

With the switch off, `multiplayer.log` says "Directory: this player's public lobbies are not listed". With Play with
strangers on, it says "Play with strangers is on".

A listing appears and goes at once. Changes are written at most every 5 s (`DirectoryService`). A failed `CreateLobby`
is retried after 5 s, doubling up to a minute. A lobby Steam no longer has this machine own is made again; its owner is
read every 10 s.

| Listing data | Holds |
|---|---|
| `hellforge` | the feature protocol |
| `hf.wire` | the wire level |
| `hf.connection`, `hf.lobbyId`, `hf.host` | the connection string, the lobby id and the host's SteamID64 in decimal |
| `hf.members`, `hf.maxMembers`, `hf.slots`, `hf.lock` | the lobby's members, size and lock |
| `string_keyN`, `number_keyN` | every PlayFab search property, under its own name |
| `hf.lobby`, `hf.live` | the game's `PUT /lobby` record (gzip and base64; long values split into parts), and its live state |
| `hf.mods`, `hf.digest`, `hf.share` | the mod set, the host's modlist digest, and whether the host shares |
| `hf.open` | `1` when the host shares and every shared mod downloads for anyone without trust ([Searching past Steam's cut](#searching-past-steams-cut)) |
| `hf.stats` | whether the host plays with edited stats |

**The published record (decided 2026-10-22).** Player names are kept, and every account id except the host's becomes 0.
The record is rebuilt from the schema of the game's own record builder (below).

### The search

The lobby stand-in sends the game's `PFMultiplayerFindLobbies` to the backend:
`lobby.find [op, entity, filter, sort, count]`. It logs the game's filter string when it changes.

1. The backend asks the directory with coarse filters: `hf.wire` equal to this build's `LobbyService.WireLevel`, the
   filter's `eq` clauses, a slot free, worldwide, at most 50.
2. `DirectoryService.Valid` checks each listing's `hellforge` with `LobbyService.Compatible`.
3. It evaluates the game's whole filter (`LobbyFilter`: `and` of `eq ne gt lt ge le`). Two clauses are left out on
   purpose: `number_key7`'s random window and the continent exclusions (`string_key6 ne`).
4. It drops lobbies the player could not join (`ModSync.Joinable`), and its own lobby.
5. It answers `lobby.found [op, hresult, count, (lobby id, connection string, owner, max, members, locked, search
   properties)...]`. The stand-in turns that into `FindLobbiesCompleted` with `PFLobbySearchResult`s.

- The last four searches' results stay valid after their batch.
- The game's two searches per scan (about 30 ms apart) share one directory search. Nothing older than 500 ms is
  reused.
- **Multiplayer needs Local PlayFab.** The session runner and the launcher refuse `--multiplayer` without it. The
  platform matchmaking selector that sessions clear without Local PlayFab sends the game's own search to Steam's
  lobbies, where vanilla games list theirs under the same build string.

**The map's records** come from the listings.

- `POST /lobby/accounts` answers, after friends, for the hosts the last searches found (for 2 minutes), with the
  listing's record, its lobby id and connection string.
- `GET /lobby/<id>` and `GET /lobby/<id>/live` answer from the local, a friend's or a found lobby. The game has not been
  seen calling the GET routes. An unknown lobby is a 404 (`lobby_unknown`).
- `PUT /lobby/<id>/live` is stored and listed.

### A stranger's listing

A stranger's listing is checked before anything of it reaches the game (`DirectoryService.Valid`, `LobbyRecords.cs`).
One that breaks any of the following is left out whole:

- a feature protocol `LobbyService.Compatible` accepts (`hf.protocol`, else `hellforge`: now `hellforge-mp-9`, or
  0.3's `hellforge-mp-8`). `hf.wire` must be the directory's wire level, or `hellforge` 0.3's. The searches filter
  those;
- a GUID lobby id, and a connection string naming the listing's host;
- 1 to 4 members, with a limit of 2 to 8 (the game makes its lobbies for 8);
- a lock of 0 or 1;
- only PlayFab search keys, at most 30, within PlayFab's limits
  ([Joining and the modlist gate](multiplayer-internals.md#joining-and-the-modlist-gate));
- a record rebuilt from the schema of the game's own record builder (game.dll 0x12CB190). Known fields must be of the
  right kind and in range: at most 4 players, names of at most 32 characters, account ids that are numbers, at most one
  host, who must be the listing's. Unknown fields are dropped;
- a `seed` in the exact layout below, if the record has one.

The published record is rebuilt from the same schema, with every account id but the host's at 0.

### Listings' hosts

Steam does not say who made a lobby to someone who is not in it. `GetLobbyOwner` gave 0 for all 50 of the app's public
lobbies in a search (2026-10-23). So a listing's host is its own `hf.host`: anyone can list under another player's id,
with that player's lobby id too (the game's own Steam lobby shows it). What holds anyway:

- A join goes to the host a listing names, over Steam's authenticated link. That host refuses a lobby it does not hold
  (`LobbyNotFound`), and whoever made the listing hears nothing.
- Listings that claim one host or one lobby with different data are all left out, except one a friend's backend vouches
  for against ones it does not. Copies of the same data count once (`DirectoryService.Unambiguous`). The cost: anyone
  can hide a listed lobby from strangers by copying it with other data.
- A backend vouches for its listing by sending friends its digest (`DirectoryData.Digest`, SHA-256 of the data) as the
  fifth field of `presence` (`FriendPresence.Vouches`; the last two digests count).
- With Play with strangers off, a friend's listing is shown only while that friend vouches for it.

### The selected mission (`seed`)

**The selected mission is what puts a lobby on the map.** The game's `PUT /lobby` carries `"seed"` only once the host
has selected a mission.

- It is the mission as fixed-width lowercase hex, version `f3fb` (game.dll encoder `0xFC3E20`). The host's lobby key 10
  holds the same string.
- The Galactic Map's planet scan decodes it (`0xFC39F0`) and draws a scanned lobby's squad marker ("1/4") and hover card
  from it.
- No seed, no marker, even when the search found the lobby and its record arrived.
- Until 2026-10-24 neither `RecordGuard` nor `LobbyRecords` kept it, so no listed lobby showed on another player's map.
  `RecordGuard`'s field list came from an upload made before any mission was selected, and `LobbyRecords` allowed 32
  characters.

The decoder writes the string's arrays into a 0xE8-byte struct on the stack without checking their counts, so a forged
seed could overflow it. `MissionSeed.Valid` keeps one only in the `f3fb` layout:

- version `f3fb`, then 58 hex digits of fixed fields;
- four arrays, each a count byte then its entries: u32 (at most 32), bytes (8), bytes (3), bytes (2), then a byte;
- an array of u32 (at most 4), two bytes, and a last array of u32 (at most 8);
- nothing after it, and at most 458 digits in all.

A host's seed for a D1 mission is 102 digits. A record with any other seed loses it, between friends and in this
machine's own listing. A stranger's listing with one is left out.

### Which lobbies a search shows

Which lobbies a search shows is mod sync's rule (`ModSync.Joinable`), and it uses the plan a join would make
(`ModSyncPlan`):

- A lobby running code or raw mods that are not verified is shown when joining brings the MOD SYNC prompt with TRUST: a
  Steam friend's, or anyone's in a sandboxed session.
- A stranger's lobby running such mods is hidden from an unsandboxed session, because they are mods to install by hand.
- A lobby whose host plays with edited stats is dropped unless the player chose `"showEditedStats"`.
- A lobby running a blocked mod is dropped.

Each search logs what it found, what it showed, and why it left out the rest:
`Directory: search "<the game's filter>": 2 listed, 1 shown (1 left out: its host's unchecked mods need a sandboxed
game)`. The reasons are:

- `this machine's own lobby`, which Steam's search returns too;
- `malformed (...)`;
- `the game's filter`, which covers clauses Steam's search does not apply, such as difficulty ranges and member counts;
- `its host enforces stat changes`;
- `its mod list is malformed`;
- `it runs a blocked mod`;
- `its host runs an older HellForge (<bundled mod> <its version>, yours <yours>): the host needs to update`, or
  `its host runs a newer HellForge (...): update HellForge to join`;
- `its host runs another HellForge version`, when the versions do not compare;
- `its host's unchecked mods need a sandboxed game`: a stranger's unverified code or raw data, in an unsandboxed
  session;
- `its host's unchecked mods wait for your trust, and other lobbies can be joined without it`: a stranger's lobby whose
  plan is `SyncVerdict.Trust`, in a sandboxed session. The game's Quick Play and SOS score the answer without knowing
  its mods, so such a lobby is answered only when no other is. A Steam friend's stays, and the browser lists every one;
- `it needs mods installed by hand`.

After that line, each lobby left out (except this machine's own) gets a line of its own (`ModSync.Check`,
`DirectoryService.LeftOutLine`). It names the lobby's host, the reason and, for a mod plan, the mods in the way with
why: `Directory: left out 1100001029d1b02: its host's unchecked mods need a sandboxed game: Code 1 (test.code):
stranger code, not verified.`

| A mod's why | Means |
|---|---|
| `share off` | the host does not share its mods |
| `stranger code, not verified`, `stranger raw data, not verified` | unverified code or raw data from a host who is not a Steam friend |
| `bundled differs` | another HellForge |
| `blocked` | the player blocked the mod |

- A line names at most 8 mods, then how many more.
- A search writes at most 10 lines, then how many more.
- The lines are written only when they differ from the last search's, since the map and the debug browser search every
  few seconds.

Until 2026-10-23 a search left this machine's own lobby out without counting it. On the first two-PC test, "1 listed,
0 shown" was the searching player's own lobby, not the friend's:

- It was listed from the planet's search once the player's own listing had been rewritten for that planet (11:22:25,
  "3 listing value(s) written").
- None of the lines named a reason, and every other path names one.
- The friend's backend first reached this one at 11:26:17, after the last search.

### Searching past Steam's cut

Steam returns at most 50 lobbies a search (`DirectoryQuery.MaxResults`), and HellForge's own checks run after that. So
once more than 50 are listed, the joinable ones could be cut.

- Each listing also carries `hf.digest` (the host's modlist digest) and `hf.open`. `hf.open` is `1` when the host shares
  and every shared mod downloads for anyone without trust: content or verified (`ModSync.Open`).
- `DirectoryService.Gather` runs the plain search first. When it comes back full, it also searches for `hf.digest` equal
  to this session's digest and for `hf.open` equal to `1`, and puts those first.
- Then it searches for 0.3's listings ([HellForge 0.3 players](#hellforge-03-players)).
- Each listing counts once. Below the cut a search costs one Steam search more than before (0.3's).

### HellForge 0.3 players

0.3's backends speak `hellforge-mp-8`. Their frames are this protocol's. They differ in what must match: 0.3 counts
every gameplay mod as shared (stats mods too), and bundled mods by their files, which differ between releases. This
backend meets them both ways.

- **Finding each other.** 0.3 searches for `hellforge` equal to its protocol. A listing here that 0.3 can join (no
  shared bundled mod on) carries:
  - `hellforge: hellforge-mp-8`, with this HellForge's protocol in `hf.protocol`;
  - 0.3's form of the set in `hf.mods` (`ModSet.Legacy`: the shared mods that are not bundled, with
    `ModSet.LegacyDigest`, the digest 0.3 takes from them);
  - the whole set in `hf.modset`.

  This backend searches for 0.3's listings too (`DirectoryService.Legacy`), and the browse API marks them
  `"older": true`.
- **A 0.3 joiner.** It sends its own digest, which counts its bundled in-game editor's files. So the host compares its
  mods one by one (`ModSync.LegacyAdmits`): each shared mod of the host's with the same content (a bundled one by id),
  and nothing else but mods the host runs per player.
  - Refused, it is offered `ModSet.Legacy`, which its mod sync reads.
  - It turns off what the host lacks (its editor, its stats mods), downloads what it may, restarts and rejoins with the
    legacy digest, which the host admits.
- **A 0.3 host.** A host known as 0.3's (its listing, its Steam presence's protocol) is joined in its protocol
  (`ModSync.JoinAs`).
  - The plan reads its bundled mods as this machine's own at `ModSets.LegacyBundledLevel` (1: their co-op behaviour has
    not changed since).
  - When this session plays exactly the host's shared mods, the join carries the host's own digest.
  - Otherwise the host refuses and offers its set, and the sync session after it is marked `legacy`.
    `ModSyncSession.Resolve` and `AutoRejoin` check it mod by mod, since the host's digest counts its own editor's
    files.
    The rejoin and the join after it speak 0.3's protocol with the host's digest (`ModSync.Rejoining`).
- A host with a shared bundled mod on (Hell Forge's difficulty, Vanilla Plus) is not listed for 0.3, whose mod sync
  would call it another HellForge version. 0.3 friends who run the same bundled mod still join it.

**Tested** (2026-10-30) against the 0.3.0 release's backend (built from `v0.3.0-beta`) and this branch's, as two
processes over loopback UDP and a folder directory. Each was driven over its pipe as its game would be, with a bundled
editor of each release's own and a content mod.

- A 0.3 joiner with the host's mods joined a 0.4 host, admitted mod by mod ("joins with HellForge 0.3").
- A 0.3 joiner with a stats mod more was refused, took the offer ("Ready, test.ammo have, hellforge.editor off,
  test.stats off"), restarted and rejoined.
- A 0.4 joiner joined a 0.3 host as `hellforge-mp-8`.
- A 0.4 joiner of a 0.3 host with a stats mod more downloaded it, restarted, was invited back and rejoined.
- Not covered: two games in a mission together.

## The lobby browser

Besides the map's per-planet searches, the backend lists every lobby the directory has, with no planet or difficulty
filter, and friends' lobbies heard through presence (`Multiplayer/BrowseApi.cs`, `DirectoryService.BrowseAsync`).

- Each comes with the join check a search makes (`ModSync.Check`). Lobbies the map would leave out for their mods stay
  in the list, with why, so the player sees what keeps them out.
- It runs at most one directory search every 2 s.
- Joining a listed lobby hands the game an invitation (`LobbyService.InviteLocal`, as a rejoin after a mod sync does),
  so the game's own join runs only when the player accepts it.
- The API is `GET /hellforge/v1/lobbies` and `POST /hellforge/v1/lobbies/<id>/join` on the loopback HTTP, for programs
  on this PC only ([external-launchers.md](external-launchers.md#the-lobby-browser-api)).
- The game's own debug lobby browser (menu presenter 41, DebugServerBrowser) searches with an empty filter every ~4 s,
  so the map's search answers it as well. Seen live on 2026-10-30: a bare list of one row per lobby, or "NO SERVERS".

**The list's order** puts what a player can join at the top, so it survives the loader's 128 places (`BrowseApi.Rank`).
Lobbies that can be joined come first (a friend's first), then ones a setting would let in, then full or locked ones,
then the rest.

- `needed` names the mods that keep the most lobbies out (`BrowseApi.Needed`: each lobby's mods to install by hand,
  counted once per lobby).
- A lobby's `mod_list` for the game lists the mods in the way first, and only those for a lobby that can't be joined. It
  is fitted whole into 250 bytes, with `+N more` for the rest (`LiveBrowse.Fit`).
- When lobbies are listed but none can be joined and no setting would let one in, the browser's message says so. It
  names the most needed mods and suggests hosting (`LiveBrowse.Message`).
- The in-game browser hides lobbies out of reach while any other shows (CAN'T JOIN or H shows them), and names each row
  by what keeps it out.

**Squad size.** A squad is at most 4, although the game's lobby has 8 PlayFab places. A listing's `hf.slots` counts to
4, and the browser shows a lobby of 4 as full (`DirectoryService.SquadSize`). That holds for older HellForge versions'
listings too, which count to 8.

**Logs.** Each browse logs its counts when they change (`Directory: browse: N listed, M joinable (...)`). Like a
search, it logs one line per lobby it cannot join (`Directory: browse: cannot join <host>: <why>: <mods>`), so a
tester's log says what keeps lobbies out.

### The ship's information terminal

The ship's information terminal opens the lobby browser (`native/mod-loader/terminal.c`, build 25480438).

- The terminal across from the Armory is menu presenter 6 (InformationTerminal). It needs Arrowhead's news, Major Order
  and statistics services.
- Offline the game asks for `NewsFeed/801`, `v2/Assignment/War/801`, the war and profile statistics and `Episode/801`.
  It gets 501 for each, and closes the terminal into an error popup.
- The loader hooks the presenter manager's open (game.dll+0x14C0550, which the game calls directly) and opens presenter
  41 when 6 is asked for.
- The table entry at +0x14C0350 that Ship Station Hotkeys calls checks the open stack and jumps to the open. Ship
  Station Hotkeys checks that entry's first bytes through `code_matches`, which reads through hooks, so it still
  matches.
- Seen live on 2026-10-30: walking up to the terminal and Ship Station Hotkeys' Control Center key both open the
  browser, and a click closes it.
- Each presenter asked for is logged once with its caller (`terminal: the game opens menu presenter N`).
- `HELLFORGE_TERMINAL=news` keeps the terminal as the game has it.

### The in-game browser

The in-game browser is SDK 16 (`native/mod-loader/live_browse.c`, `LiveBrowse` in `Multiplayer/BrowseApi.cs`,
`native/editor/app_lobbies.cpp`).

- An editor asks over the live-edit pipe (`browseask refresh|join <lobby id>`). The backend answers with the API's own
  list: one `browselobby` line per lobby (at most 128), then `browsestate`, which publishes them, as mod sync's
  `syncmod` and `syncstate` do.
- A search under way (`searching`) and a join's outcome (`invited`, `failed`) are a `browsestate` of the same version,
  which keeps the lobbies.
- The terminal hook counts each redirect, and `browse_state`'s `terminal` carries the count. The in-game editor opens
  its LOBBY BROWSER panel when it goes up, and on Ctrl+L. The panel shows alone over the game and holds its input like
  the MOD SYNC prompt.
- The panel asks for a new list every 10 s while it shows. A join closes it once the invitation is in the game, where
  the player accepts it.
- It is a placeholder for Echelon's browser, which can use the same calls or the HTTP API
  ([lobby-browser.md](lobby-browser.md)).
- A mod that draws its own asks `HF_BROWSE_CLAIM`. The first claim holds until released, and `browse_state`'s `owner`
  names it. The editor then opens no browser, from the terminal or Ctrl+L, and closes one that shows. Only the owner's
  `HF_BROWSE_SHOWN` and `HF_BROWSE_HIDDEN` move the terminal's camera.

### Quick Play and the hidden lobbies

`browseask quick -` (Space or QUICK PLAY) lists again and joins `BrowseApi.Best`'s lobby, the same as
`POST /hellforge/v1/lobbies/quick`. `browsestate` says `invited` with the host's name, or `failed` when nothing can be
joined.

The browse API lists what a map search leaves out, each with a fix (`JoinCheck.Fix`, the `browselobby` line's last word,
`HfBrowseLobby.fix`):

| Lobby | Listed as | Fix |
|---|---|---|
| A stranger's, with Play with strangers off | unjoinable; not contacted, its record not remembered for the map | `strangers` |
| One whose host shares edited stats | unjoinable | `editedStats` |
| A stranger's running unverified code or raw mods, in an unsandboxed session | unjoinable | none: the sandbox cannot change once a session runs |

- Selecting one, A (or the button) offers the setting. Play with strangers is offered only after the launcher's
  warning, on the screen, and TURN ON (Y).
- `browseask allow strangers|editedstats` then turns it on for this session only (`MultiplayerHost.AllowForSession`:
  the stranger setting's `AllowForSession`, `ModSync.ShowEditedStatsThisSession`), and lists again.
- Nothing is saved: any mod in the game can send the ask, so the launcher's switch and the Mods page stay the player's.
  The next session uses them again.
- Only the game's pipe can ask that, not the HTTP API.
- Every ask ends in a state that is not `searching`. A search that fails or times out, or a list the game refuses, is
  answered `failed` with the old list kept. The editor gives up on a search or a join with no answer after 60 s.

**Co-op relays off.** On Linux with co-op over Steam relays off (`Multiplayer/CoopRelays.cs`;
[linux.md](linux.md#co-op-on-linux-opening-udp)), no join could reach a host.

- Every lobby that could otherwise be joined, or that a setting lets in, has the fix `relays` (`HF_LOBBY_FIX_RELAYS`,
  "Co-op relays off").
- The list's `coopRelays` says `{"on": false, "why": ...}`.
- Quick Play answers 409 `HELLFORGE_COOP_RELAYS_OFF`.
- This machine's Public lobby is not listed.
- The editor asks before any JOIN or QUICK PLAY while it is off ("TURN ON CO-OP OVER STEAM RELAYS", Y or N).
- `browseask allow relays` (`HF_BROWSE_TURN_ON_RELAYS`) has the launcher open it in the running session and save it
  for later ones (`CoopRelaysSwitch`). It then lists again and carries on with the join.
- Unlike the other fixes it is the player's saved launcher setting: the question is that choice.

### The terminal's camera

The terminal has no camera move of its own, so the loader makes one (`native/mod-loader/terminal_camera.c`, build
25480438).

- When an editor read `browse_state` in the last 3 s, the terminal opens no presenter at all.
- When the player pressed E at it, the camera glides for 1.1 s from the player's view to the wall screen. It ends square
  to the screen and close enough that the screen fills the view. `browse_state`'s `camera` goes `moving`, then `held`.
- The E press calls the open from game.dll. Ship Station Hotkeys' key calls the table entry from its own code and gets
  the browser without a glide.
- The editor draws nothing (and holds input) until the camera is held. It then powers its full-screen browser on and
  says `HF_BROWSE_SHOWN`.
- Closing it says `HF_BROWSE_HIDDEN`, and the camera glides back in 0.8 s.
- Held with no `HF_BROWSE_SHOWN` for 4 s, it glides back by itself.
- Without an editor the terminal still opens presenter 41.

How the glide works:

- It hooks the engine's script render step (helldivers2.exe + 0x689BC0).
- After that step, the camera unit's pose is blended toward the terminal's. The camera unit is the main world's unit
  with a camera at the world's last render pose.
- The pose is set with the engine's own `set_local_pose` and `update_unit`, so the frame renders with it.
- Lua's `render()` setting the pose does the same, which is how it was found. game.dll's camera code sets it before
  that step.
- The pins cover the five engine functions it calls. Any mismatch turns it off (`camera` stays `none`), and the browser
  shows at once.

## Play with strangers

The player's choice lives in `mods/hellforge-mods.json` as `"strangers": "on"` or `"off"`, written only when chosen.
With no key the default is **on when the game is sandboxed, off otherwise** (`StrangersEffective`). Before the
matchmaking overhaul of 2026-10-30, an unsandboxed session treated strangers the same as a sandboxed one when nobody had
chosen.

- A legacy `"playWithStrangers"` keeps its value: true is on, and false stays off. Older settings saves wrote false for
  every player, so it cannot tell "never chosen" from "turned off". Off is the side that cannot let a stranger in
  unasked, and the launcher's "Use default" link clears it.
- The launcher's Settings > MULTIPLAYER switch shows the value the next session would use. Under it is "On by default
  while your game is sandboxed" or "Off by default: \<the sandbox's reason>".
- Moving the switch stores the explicit choice. "Use default" removes the key.
- `HellForge.Cli settings --strangers yes|no|default`
  ([external-launchers.md](external-launchers.md#profiles-and-settings)) does the same. Its JSON output has
  `playWithStrangers` (the effective bool) and `playWithStrangersChosen` (`true`, `false` or `null`).
- The backend reads the effective value again within 2 s of a change, also during a session
  (`Multiplayer/StrangerSetting.cs`). It never reads more than once per 2 s, however many frames strangers send. Its
  first read logs once which rule applies and why.
- The first session that runs with the sandbox default on shows the stranger warning in the launcher's Play page once.
  `strangersWarned` in the same file remembers it.

Off:

- every frame from someone who is not a Steam friend is dropped (`StrangerGate`), and the Steam link accepts no
  session from them. The drop is logged once a minute per machine ("Dropped ... not a Steam friend");
- this machine's public lobby is not listed ("Directory: this machine's public lobby is not listed: Play with strangers
  is off" in `multiplayer.log`);
- a Galactic Map search shows friends' listings only, and only the one each friend's backend vouches for
  ([Listings' hosts](#listings-hosts));
- a join to a stranger's lobby is answered at once as not joinable, and the in-game editor shows why ("Couldn't join").

On, strangers' games send this one data nobody checks for them. HellForge is open source, anti-cheat is off and a
modified build can lie about its mods. So a flaw in the game's network code could let a stranger run code on this PC
(lockdown plan item 10). The launcher's warning says so. The sandbox, on by default since the 0.3.0 beta
([steam-bridge.md](steam-bridge.md)), limits what such code could reach. Without it, play with strangers only when that
risk is acceptable.

### The stranger gate

With Play with strangers on, the Steam link accepts someone who is not a Steam friend only while it deals with them
(`StrangerGate.cs`):

- while a lobby is listed here (and Steam holds the listing);
- a member of a lobby hosted here, or the host of a lobby joined or being joined from here;
- a machine a lobby here expects back;
- a mod download.

From a stranger, a backend takes:

- `lobby.join`: only with a lobby listed, from a machine it deals with, or from one a lobby here expects back. The
  limits are at most 6 a minute from one machine and 60 in all, also for a stranger it already deals with;
- pings;
- then what follows once it deals with them.

Presence (the social tab) and invitations stay between friends. A stranger host's name comes from the record its listing
carried. The gate's tables forget a machine after a minute of silence.

## Testing public lobbies

**`HELLFORGE_TEST_STRANGERS=1`** is a test mode for two friends' PCs. A backend started with it treats every Steam
friend as a stranger:

- no presence, so the social tab lists none;
- the stranger gate applies;
- Steam sessions are accepted only by the directory's rule.

`multiplayer.log` says "Test mode (HELLFORGE_TEST_STRANGERS)". Play with strangers must be on as well on both PCs; test
mode does not turn it on. Set the variable on both PCs before starting the launcher (the session runner and the backend
inherit it). In Command Prompt, for example: `set HELLFORGE_TEST_STRANGERS=1`, then
`start "" "<installation>\HellForge.exe"`. Steam itself still sees two friends, and nothing in the directory depends on
that.

**Tests on one PC.**

- `HellForge.LocalBackend.Tests` covers the directory: filter, values, publishing, search, records, edited stats,
  blocked and unjoinable mods, strangers.
- `HellForge.SdkShimGen smoke-lobby-search <stand-ins> <HellForge.LocalBackend.dll>` runs two backends over loopback UDP
  sharing a folder directory (`--directory`), and two lobby stand-ins through the PlayFab C API.
  - The friend's search finds the host's lobby about 0.35 s after it is listed, and joins with the connection string it
    found. It stops finding it about 0.35 s after the host leaves.
  - Both backends get a `--mods` folder with Play with strangers on. Off, nothing would be listed.
  - It runs twice: as friends, then as strangers (`HELLFORGE_TEST_STRANGERS`). As strangers the join passes the gate,
    no presence is swapped, and the map's record for the host is the listing's (names kept, other account ids 0).

**The map, seen on one PC** (2026-10-24, two installations, [multi-instance.md](multi-instance.md)):

- **The marker.** A lobby whose host is in a mission shows as a squad marker with the hover card: host name, level,
  mission and objective. A lobby with no mission selected shows none, as the marker needs the record's `seed`.
- A host on its ship with a mission selected uploads a seed too, but that was not tried.
- A host with no mission selected also lists difficulty 0 (`number_key5`). The map's search (the map's own difficulty
  window) leaves that out as "the game's filter".
- **Friends.** A planet scan leaves out lobbies of the player's Steam friends: they show under Social instead. So two
  friends test the map with `HELLFORGE_TEST_STRANGERS=1`.
- **Joining.** Selecting the marker joins: the loadout screen of the host's mission comes up within seconds. After
  Ready Up the joiner drops into the mission in progress (hellpod, "Allied Destroyer joining squadron"), and the host
  sees it.
- Ready Up does nothing while all four stratagem slots are empty (the loadout screen's own rule).
- An idle host on the ground can be killed (one was, about 11 minutes in), and with no one alive the mission fails.

Not yet known:

- whether the hover card works with the other members' account ids at 0 (only solo hosts so far);
- the card's elapsed time on the surface. It reads 0m 0s, because no host has uploaded `PUT /lobby/<id>/live`.

**Steam's search rate** (soak, 2026-10-23, one PC, live Steam). The search was shaped like the directory's (the
protocol key, a slot free, worldwide, at most 50), for one Invisible lobby of the soak's own. Steam failed none and
always found that lobby. No limit was reached, and a new lobby was found 0.84 s after it was made.

| Searches | Rate | Time |
|---|---|---|
| 60 | the map's rate (every 10 s, 10 minutes) | half in 175 ms or less, the slowest 231 ms |
| 60 | one a second | |
| 359 | back to back in a minute (358 a minute, 60 times the map's rate) | the slowest 192 ms |

**The game's own Steam lobby** ([steam-bridge.md](steam-bridge.md#the-games-own-steam-lobby)).
`HELLFORGE_STEAM_LOBBY_FRIENDS_ONLY=1` on the host makes the broker keep the game's Public Steam lobby Friends only
anyway (`steam-broker.log`: "Steam gets 1"). A two-PC test shows whether the broker can keep the lobby Friends only with
strangers on too. Both PCs are sandboxed (`--sandbox`), with Play with strangers on. The other player joins from the
map, and the join and the mission must work.

The two-PC test plans for public lobbies and for privacy, invitations and kicks are in the build log's
[step 7](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multiplayer.md#step-7-public-lobbies-on-the-galactic-map-2026-10-22)
and
[step 8](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multiplayer.md#step-8-privacy-invitations-and-kicks-2026-10-23).
The matchmaking overhaul and HellForge 0.3 support are
[steps 11 and 12](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multiplayer.md#step-11-matchmaking-overhaul-2026-10-30).
