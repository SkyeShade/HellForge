# Co-op mod sync

<!-- diataxis: explanation -->

This page is for contributors working on mod sync: how a joiner refused for different mods takes the host's. It
covers the protocol and its limits, the rejoin after a restart, the download speed, and what another player's mods can
reach.

Players see mod sync in [mods.md](mods.md#playing-with-other-mods). The rest of co-op is in
[Co-op internals](multiplayer-internals.md), and public lobbies in
[Public lobbies and strangers](multiplayer-lobbies.md).

## Mod sync

A joiner refused for different gameplay mods can take the host's (protocol `hellforge-mp-9`). The host side is
`src/HellForge.LocalBackend/Multiplayer/ModSync.cs`.

1. Before the refusal the host sends its mod set (`mods.offer`).
2. The joiner's backend plans the difference (`ModSyncPlan` in HellForge.Core). The plan ignores per-player entries:
   they never download and are never an off.
3. The in-game editor shows the MOD SYNC prompt (SDK 14 `sync_state`, `sync_mod`, `sync_answer`).
4. On accept the joiner downloads each mod's counted files from the host over the Steam link (`mods.files`,
   `mods.get`; speeds in [Mod sync speed](#mod-sync-speed)). What the joiner's own game has is written from it instead
   ([Mod sync from your own game](#mod-sync-from-your-own-game)).
5. It checks every file and the whole mod (`SyncedStore.Install`), and keeps it in `mods/.synced/<id>@<hash>/`.
6. Then one of two things happens:
   - the running game takes the host's set (`LiveMods.ApplySyncAsync`). That is one live change of the session's mods
     (docs/mods.md "Changing mods while you play"), and a rejoin as soon as the game is on the ship
     (`MultiplayerHost.RejoinAfterLiveSync`);
   - or the launcher restarts the session with the host's mods (`--mod-sync`).

**What downloads.** Each mod's class comes from the bytes received, with one rule for every host, Steam friends
included (`ModSets.ClassOf`). Bundled mods never download.

| Class | What it holds | Downloads |
|---|---|---|
| Content | data specs HellForge checks all of (`DataSpecs.Unchecked`), schema stats, network field types | from any host |
| Code and raw data | binary data patches, data specs with `"replace"` or unreviewed resource types such as `lua`, raw stats lines, `componentWrites` | only when `data/verified-mods.json` names their exact content hash (from any host), or the player trusted it |

An unverified code or raw mod is:

- `SyncAction.Trust` ("needs your trust") when it is a Steam friend's;
- `SyncAction.Trust` too when it is a stranger's in a sandboxed session;
- `SyncAction.Manual` when it is a stranger's in an unsandboxed session.

**Trusting a mod.** The prompt's TRUST, after a warning, answers `trust`.

- That adds `{id, hash, name, version, from, since}` to `"trusted"` in `mods/hellforge-mods.json` (`ModSettings`,
  beside `"blocked"`). The Mods page lists and untrusts them.
- The entry gets `"stranger": true` for a stranger's mod, and `"sandboxed": true` for a friend's trusted on a sandboxed
  game's prompt.
- The offer is made again, and the mod downloads once nothing else waits.
- The joiner's checks then take the verified list or that list (`ModSync.Allowed`), every byte checked as before.
- A stranger's trust, and any trust answered by a sandboxed game, counts only in a sandboxed session
  (`TrustedMod.SandboxOnly`).

Why a sandboxed game's trust is sandbox-only:

- The game's `sync_answer` is any mod's to call, and a sandboxed mod must not open another mod to unsandboxed play.
- So `ModSync.Answer` stores the game's trust in a sandboxed session as sandbox-only, a Steam friend's too. It tells the
  game so (`HF_SYNC_FLAG_SANDBOX_ONLY`, the syncmod line's `sandboxonly`).
- The launcher's prompt answers through `<run>/backend/mod-sync-answer.json`, which a sandboxed game cannot write.
  `<run>/game` is the only folder it can write, low integrity on Windows and in the Linux sandbox. So the launcher's
  trust of a friend's mod counts everywhere.
- `ModSettings.SetTrusted` never narrows a trust that counts everywhere.

**Synced mods later.**

- A synced code or raw mod counts as `have` in the plan (`ModSyncPlan.Make`, its class read from the stored files).
- It loads in the session after the sync (`ModSyncSession.Resolve`, `mods-stage --sandbox`) only while it is verified
  now or trusted for that session. Otherwise the plan treats it as missing, and Resolve plays the player's own mods and
  says why.
- Once trusted, a mod downloads with no prompt from any host that runs that exact content. That is what lets Quick Play
  join a lobby of mods the player has trusted before.
- A host serves its shared mods to whoever its grant covers: what a joiner may run is the joiner's to check.
- A mod's manifest `url` (an `https://` address, `ModCatalog.WebAddress`) travels in its mod set entry. GET IT answers
  `open`, and the joiner's backend opens that address (checked again) in the default browser.

## Protocol and limits

Each session writes its mod set (`<run>/modset.json`, from `mods-stage --modset`): every gameplay mod with its content
hash, class (content, code, raw, bundled), load-order place, folder and counted files. The backend reads it with
`--modset`.

### The host

- A joiner refused for different gameplay mods is first sent the set without its folders:
  `mods.offer [lobby id, set JSON, sharing 0|1]`.
- Then comes the refusal, as before: `lobby.refused [reason, line...]`, its lines shown by both games. When the host
  shares, the joiner's last line says the MOD SYNC window offers the download. A backend from before ignores the offer.
- A joiner takes an offer only for a join it asked for in the last minute.
- The joiner's machine may then download for 10 minutes from its first refused join. A later join extends nothing. A
  stranger may download too: the stranger gate lets a download through.
- Each request it makes for one of the set's mods keeps the grant on for 2 minutes more, up to 6 hours from that join,
  so a large mod over a slow link finishes. A grant left idle ends (`GrantTime`, `GrantIdle`, `GrantLongest`). Before
  2026-11-05 it ended at 10 minutes, downloading or not.
- The grant's budget is the set's bytes twice over and 16 chunks.
- It serves only a mod in its set (not a bundled one), only a file that mod counts, and only within the file's size.
- One worker reads the files, with at most 24 queued requests per machine. It is paced to 8 MiB/s per machine and
  16 MiB/s in total ([Mod sync speed](#mod-sync-speed)).
- A request that fails in any way is refused or logged, and the worker serves the next one.
- `"share": false` in `hellforge-mods.json` stops it. Sharing is on by default (a maintainer's call still open).

| Request | Answer |
|---|---|
| `mods.files [req, id, hash, start]` | `mods.filelist [req, ok, total, start, (path, size, sha256)...]`: pages of at most 1000 files (a frame holds 4096 fields), at most 10000 files a mod on both sides |
| `mods.get [req, id, hash, path, offset, length ≤ 128 KiB]` | `mods.chunk [req, ok, offset, bytes]` |
| `mods.parts [req, id, hash, start]` | `mods.partlist [req, ok, total, start, (path, offset, length, type, name, part, sha256)...]`: 500 parts a page ([Mod sync from your own game](#mod-sync-from-your-own-game)) |
| either | `[req, refused, reason]` |

### The joiner

- The offer becomes the game's (`ModSyncPlan` in HellForge.Core). The loader's live pipe gets `syncmod` lines and then
  `syncstate` (`native/mod-loader/live.c`, SDK 14 `sync_state`/`sync_mod`).
- The in-game editor shows its MOD SYNC prompt over the game, with the editor window closed. The prompt takes answers
  (buttons, Enter, B) one second after it appears or its state turns.
- The answer comes back as `syncanswer <version> <answer> [<mod id>]` (accept, decline, restart, block, trust, open).
- Each `syncmod` line ends with the mod's `url` or `-`. The loader keeps only an `https://` address of printable ASCII.
- The loader holds offers of up to 512 mods (`live.c`). The backend lists the ones that need something first.
- The offer is also written to `<run>/backend/mod-sync.json`.

**Trust and open.**

- An unverified code or raw mod, a Steam friend's or in a sandboxed session anyone's, is offered for trust: `trust` in
  the plan, with the offer `blocked` until none waits.
- `trust <mod id>` is taken only for such a mod of the current offer. Its id and hash go on the player's trusted list
  with the host's name and SteamID64, and the offer is made again. When nothing else waits, its download starts at
  once.
- Unsandboxed, a stranger's are `manual` unless the player trusted them. The prompt lists such a mod as "install it
  yourself", with GET IT and no TRUST.
- `open <mod id>` opens the mod's `url` from the host's set in the default browser, at most once a second, after
  `ModCatalog.WebAddress` checks it again. Nothing else is ever started.

**The download.**

- On accept it checks the drive has room: the download and 256 MB.
- It downloads with the chunks asked for on across files, 2 to 16 in flight ([Mod sync speed](#mod-sync-speed)).
- The file list, the manifest (`hellforge-mod.json` before `bts-mod.json`), the stats file and the data spec come
  first. A mod they show to be code or raw data (`ModSets.Declared`) that is not on the verified list stops there.
- Each file is checked against its listed size and SHA-256, and the whole mod by `SyncedStore.Install`, before it goes
  to `mods/.synced/`.
- Each attempt writes into its own `.synced/.partial/<id>@<hash>-<attempt>/`, deleted whatever goes wrong. Anything
  that goes wrong ends in `failed` with its message.
- A stored mod is found again only while its files rehash to its content hash.

**The restart.**

1. On restart the backend writes `<install>/temp/mod-sync/session.json` (`ModSyncSession`), and `mod-sync` into
   `stop-observation.signal`.
2. The session runner reports `restart: mod-sync`, and the launcher starts the next session with `--mod-sync <file>`
   (`mods-stage --sync`).
3. The launcher says "Playing with \<host>'s mods". When a rejoin follows, it says "Once the game is on the ship, the
   host's invitation to their lobby waits in game.", and the session's `result.json` has `"rejoin": true`.
4. That session plays the host's mods, each found by id and content hash, in the host's order. If they do not make the
   host's modlist digest, it plays the player's own mods and reports why.

**Without the in-game editor.** With the in-game editor switched off, the launcher shows the offer from
`mod-sync.json` on its Play page (`ModSyncPrompt.cs`, `ModSyncOffer`), TRUST and GET IT included. It writes the answer
to `<run>/backend/mod-sync-answer.json`, which the backend takes within a second, as the game's `syncanswer`.

## Rejoining after the restart

The restarted game is brought back into the host's lobby through the game's own invitation path, the one a friend's
invitation takes (`Multiplayer/AutoRejoin.cs`). HellForge does not make the game join.

- The session runner passes the sync file to the backend (`--rejoin <install>/temp/mod-sync/session.json`), but only
  when mods-stage found all of the host's mods.
- The backend takes the file only while it is less than 15 minutes old, and only when its connection string names
  the host's lobby (and not this player's own). The session's modlist digest must be the host's too.
- It waits until the game creates its own lobby, which means the game is on the ship (`LobbyService.LocalLobbyCreated`).
  It waits at most 3 minutes, then 5 s more for the ship's screens.
- It then asks the host: `lobby.rejoin [lobby id, modlist digest, protocol]`.
- The host answers for the lobby asked. If that lobby has closed, it answers for the one lobby its own game owns now.
  It checks the answer as it would a join: kicked, privacy, version, mods, lock, room.
- The answer is `lobby.rejoined [lobby asked, "ok", lobby id, connection string]`, or
  `[lobby asked, gone|full|locked|refused|mods|version, line...]`.
- On "ok" the lobby stand-in hands the game `InviteReceived` from the host (`LobbyService.InviteLocal`), and the in-game
  editor shows "Rejoining \<host>".
- Any other answer, no answer within 10 s, or no ship within 3 minutes gives up once. The game shows "Couldn't rejoin
  \<host>" with the reason and, where it helps, how to join by hand. Nothing is tried again.
- A join the game starts by itself ends the rejoin. Into the host's lobby, the player is rejoining. Into another lobby,
  the player chose that one.
- **Admission.** The host keeps a machine refused only for its gameplay mods expected back in that lobby for 15 minutes
  (`LobbyService.ExpectedBack`), and an "ok" keeps it expected for 3 minutes more (`RejoinGrace`).
  - While it downloads the host's mods, each minute with requests keeps it expected for 15 minutes more
    (`ModSync.Downloading` → `LobbyService.StillExpected`, since 2026-11-05). That never brings back an expectation
    that ran out.
  - Its join is then admitted whatever the lobby's privacy.
  - A stranger's Steam session and join pass the stranger gate (`LobbyService.Welcome`).
  - An invited player stays invited, as a refusal never removes an invitation.
  - A kick removes the expectation.
- **Other protocols and older hosts.** A host whose feature protocol `Compatible` refuses (now anything but
  `hellforge-mp-9`) counts as another HellForge. Later feature versions are added there while wire level 1 stays. A
  host from before `lobby.rejoin` does not answer, and the joiner's game says the host did not answer.

### How the game joins a lobby

The research behind the rejoin was static, on the supported build's unpacked `helldivers2.exe` and `game.dll` images
(the research repo's snapshots), and on the stand-in and backend logs of earlier sessions.

- **Invitations (PlayFab).** Every session calls `PFMultiplayerStartListeningForLobbyInvites` at boot, about 9 s in.
  - The engine logs a received invitation: `Lobby: InviteReceived (sender=%s, receiver=%s, connection_string=%s)` (exe
    strings beside the other lobby state changes).
  - game.dll has the input actions `InvitePopupAccept`, `InvitePopupDecline` and `InvitePopupModifier`, so an
    invitation is most likely a popup the player accepts.
- **Steam.** The engine's Steam friends module (`stingray/runtime/steam/steam_friends.cpp`) reads `+connect_lobby <id>`
  and `+connect <address>` from the command line ("Invalid command line connect lobby `%s`").
  - Its script API has `boot_invite` and `next_invite`, which return `INVITE_LOBBY`, `INVITE_SERVER` or `NO_INVITE`.
  - game.dll's title screen (`state_title_screen.cpp`) names `friends_handle_boot_invites`.
  - There is a deep-link state (`StateDeepLink`, `deeplink_temporary_hosting`, `ship_teleporter_deeplink_join`,
    `TimedOut_CantJoinLobby`).
  - A game started with a Steam lobby id therefore joins through a path of its own, by way of the title screen and the
    ship. That path was not decoded. The title screen's update is game.dll 0xAD7CB0 and its deep-link step 0xAD8760.
- **What the logs show.**
  - A joining game posts the host's Steam lobby as its own `platform_lobby` after the PlayFab join. The order seen is
    PlayFab first, then Steam. Whether a Steam lobby leads the game to a PlayFab lobby is not known.
  - The social tab's join is `/lobby/accounts` and then `PFMultiplayerJoinLobby` with the connection string.
  - A game creates its own lobby on reaching the ship, 28 to 34 s after the stand-in loads in sessions that reached the
    ship.
  - One host log shows `PFMultiplayerJoinLobby` repeated on a closed lobby's connection string every 20 s. Whether the
    game or the player did that is not known.

**The path chosen: an invitation from the host, handed to the game by its own backend.**

- It is the game's own path, and the stand-in already delivers it for friends' invitations. No game code or data
  changes.
- It reaches strangers too: the rejoining machine's own backend hands the invitation to its own game, after the host
  answered.
- It waits for a state the backend can see: the game's own lobby, which it creates on the ship.

The Steam boot invite (`+connect_lobby`) was set aside:

- it would need the host's Steam lobby id;
- the game would join a real Steam lobby of possibly another privacy, which a stranger may not get in;
- how the game goes from a Steam lobby to the PlayFab lobby is not known.

It remains a candidate if the invitation proves to need more than one press. Not yet known, for a two-PC test to
answer:

- whether the game shows a popup for the invitation or joins at once, and whether it names the host (a stranger host
  too);
- whether an invitation 5 s after the game's lobby is made comes too early for the ship's screens.

## Mod sync speed

**What held the first two-PC test back (2026-10-23).** The joiner downloaded 38 MB (two of a Steam friend's mods, 44
files) in 163 s, about 0.24 MB/s. The 39.48 MB mod took 161 s of it, which is 245 KB/s.

That is Steam networking's own send rate:

- A connection sends at a fixed rate between `k_ESteamNetworkingConfig_SendRateMin` and `SendRateMax`. Both are
  256 KiB/s (262,144 B/s) by default.
- It estimates no bandwidth. The header says the two "should always be set to the same value".
- With Min below Max, a connection starts at about four packets per round trip (RFC 3390) and is clamped to Min..Max.
  At a 136 ms round trip that is Min. Raising only Max changes nothing.
- 256 KiB/s less the packet and message framing is the 245 KB/s measured.

Sources, from GameNetworkingSockets:

- [csteamnetworkingsockets.cpp](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/src/steamnetworkingsockets/clientlib/csteamnetworkingsockets.cpp):
  `DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, SendRateMin, 256*1024, ...)`, the same for `SendRateMax`.
  `SendBufferSize` is 512 KiB and `RecvBufferSize` 1 MiB.
- [steamnetworkingsockets_snp.cpp](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp):
  `SNP_ClampSendRate`, and `SNP_SendMessage`, which refuses a message past `SendBufferSize` with
  `k_EResultLimitExceeded`.
- [steamnetworkingtypes.h](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/include/steam/steamnetworkingtypes.h):
  "The default value is 256K".

The Steamworks page [steamnetworkingtypes](https://partner.steamgames.com/doc/api/steamnetworkingtypes) still says
"Default is 0 (no-limit)", which is out of date.

The other limits were looser:

- **Round trip.** The rejoin's ask and answer took 136 ms (11:35:12.783 to .919, which includes both links' polling).
- **Window.** 4 × 128 KiB per round trip comes to about 3.8 MB/s.
- **Host pacing.** 1 MiB/s.
- **Files.** Each file waited at least one round trip, about 6 s for 44 files. This is why the 65 KB mod of 13 files
  took 2.3 s.
- **Polling.** The Steam link's thread slept a timer tick (15.6 ms) between turns, which added up to 4 ticks to each
  round trip.

**Now.**

| What | Before | Now | Where |
|---|---|---|---|
| Steam send rate (Min = Max) | 256 KiB/s | 8 MiB/s | `SteamPeerTransport.SendRate` |
| Steam send buffer, receive buffer | 512 KiB, 1 MiB | 8 MiB each (a download's whole window and room to spare) | `SendBuffer`, `ReceiveBuffer` |
| Joiner's requests in flight | 4 chunks, one file at a time | 2 to 16 chunks' bytes (adaptive), on across files, at most 16 requests | `ModSync.MinWindow`/`MaxWindow`, `DownloadWindow` |
| Host's queue per machine | 8 | 24 | `ModSync.QueuedPerPeer` |
| Host's pacing | 1 MiB/s a machine, 2 MiB/s in all | 8 MiB/s a machine, 16 MiB/s in all (caps against abuse; a 40 ms burst makes up a timer's overshoot) | `PeerRate`, `TotalRate`, `PaceBurst` |
| Grant budget | the set twice and 4 chunks | the set twice and 16 chunks | `GrantBudget` |
| Grant time | 10 minutes from the join | 10 minutes, kept on 2 minutes past each request, at most 6 hours (2026-11-05) | `GrantTime`, `GrantIdle`, `GrantLongest` |
| Steam link turn | a sleep of a timer tick | sends at once (`Send` wakes the thread); a turn with traffic is followed by the next at once | `SteamPeerTransport.Run` |

**Game traffic.** A Steam connection without congestion control sends whatever it is given at that rate, and a peer's
game frames share it with mod sync. `ISteamNetworkingMessages` channels share one connection and its one lane, so a
channel of its own would not let game frames pass the chunks. The joiner's window keeps the queue short instead, as TCP
Vegas and LEDBAT do:

- It times each whole chunk from asking to arrival.
- A chunk within 100 ms (`TargetDelay`) of the quickest so far adds one chunk: a doubling each round trip at first,
  then one chunk per round trip once it has been cut.
- A later chunk takes a quarter off, at most once per round trip and never below 2.

So whatever the slowest stretch is (the host's upload, a relay, the joiner's link), its queue stays near 100 ms, and
the window fills it.

**Older hosts.** A host from before this change (Steam's 256 KiB/s, a queue of 8) still works with a new joiner:

- Its chunks queue at once, so the window stays near 2.
- A request it refuses as "too many requests at once" halves the window's ceiling, and that chunk is asked again.

The speed comes from the host's end: an updated joiner downloads from a host on an earlier 0.3.0 test build at its old
speed.

**Expected.**

- On a good connection, 8 MiB/s: about 5 s for the test's 38 MB.
- In practice the host's upload sets the speed (20 to 50 Mbit/s is 2.5 to 6 MB/s, so 7 to 15 s), along with what
  Valve's relays carry, which is not measured yet.
- A sync from a stranger goes through the same link.

### Mod sync from your own game

A gameplay mod's game patch files are mostly the game's own bytes: a model, texture or sound copied, overridden or
backported. The joiner has those already. So the large part of a big mod does not have to cross the link
(`ModSync.Parts.cs`, `SyncParts` and `RecipeRebuilder.WriteWhatCan` in HellForge.Core):

1. **The host lists parts.** Its offer carries a fourth field, `parts`, and it answers `mods.parts` with
   `mods.partlist`, 500 parts a page. The list is every resource part of the mod's archives, read from each archive's
   table (no game index). Each part is hashed. The list is made once per mod and content, and started in the background
   when the offer goes out.
2. **The joiner writes what its game has.** While the mod's other files download, it looks each part up in its own
   builds (HellForge's game copy and Steam's install). It tries the same key in any build, then any key the game index
   knows those bytes under. A part is used only when the bytes found have its SHA-256. Each distinct part is read once.
3. **The rest downloads.** Only the ranges no part covered (the archives' tables, the author's own resources) come
   over `mods.get`. Every file is then checked against the host's file list. One that does not match (a host whose part
   list does not match its files) downloads again whole.

The game index is the asset kit's (`AssetKit.Index`: fingerprints of each build, hashes only, in
`<installation>/temp/asset-kit`). Mod sync never makes one, since it reads the whole game and takes gigabytes of memory
while the game runs. Without it, only resources under their own names are found, and a rest of 64 MB or more says so
on the prompt. The Mods page's MOD SYNC card makes it on request (**Index my game**, under "When I join"), with no
session running.

**Recipe mods.** A mod that ships a recipe instead of the game's bytes ([the asset kit](modding/assets.md)) already
syncs as its recipe. The joiner rebuilds its game patch files from its own game. Where its game lacks a part the recipe
takes (a build it does not have, as after a game update), that range comes from the host's own rebuilt files. The
host serves those for its ready recipe mods, and the grant's budget counts them. From an older host the joiner is told
which build it lacks.

**Measured (synthetic, `ModSyncPartsTests`).**

| Case | Bytes over the link |
|---|---|
| A 2.46 MB mod: an overridden unit (same key), a renamed texture, the author's material; no game index | 1.55 MB |
| The same, with the game index | 51 KB |
| The same, from an older host | 2.46 MB (whole) |
| A recipe mod (54 KB counted), the joiner has the whole game | 54 KB |
| The same, the joiner's game lacks the texture | 1.55 MB (the texture's 1.5 MB, from the host's rebuilt files) |

**Tests** (`tests/HellForge.LocalBackend.Tests/ModSyncSpeedTests.cs`, over a simulated path without Steam), for a 6 MB
mod of 63 files:

| Path | Download time | Notes |
|---|---|---|
| Next door, no bottleneck | 0.76 s | `PeerRate` allows 0.72 s and is never exceeded |
| 120 ms round trip | 1.55 s | The old design: about 13 s |
| 2 MiB/s link, 80 ms round trip | 3.65 s | The link alone takes 3.03 s |

On the 2 MiB/s link, a small frame the host sends every 50 ms down the same link waits a median of 160 ms. It waits at
most 360 ms, 40 ms of that the one-way delay. A fixed window of 16 chunks would queue 2 MiB there, a second.

`DownloadWindow`'s growth, cuts, floor and ceiling are checked on their own as well.

To time a large download, read `multiplayer.log` from "downloading N MB" to "every mod is here". Both backends log
"Steam link: send rate 8 MiB/s" at start.

## Trust: what another player's mods can reach

Mod sync and public lobbies bring mods from people a player may not know. What limits them:

- **What downloads by itself** is only what HellForge checks itself (data specs, schema stats, network field types),
  read from the bytes received, with the same rule for every host. Code and raw data need the verified list
  (HellForge's own mods), or the player's own trust after a warning. That trust can be given to a Steam friend's mod,
  or in a sandboxed session to anyone's ([Mod sync](#mod-sync)).
- **Synced mods load only in the synced session.** They sit in `mods/.synced`, which normal sessions never load, and
  the player's own Mods page never changes.
- **The copy is off the internet.** The network block lets the copied game reach only this PC (loopback).
- **The game cannot start other programs.** The session runner puts the game in a job whose active-process limit is 1
  (`HostBoundedProcess`).
  - That costs the game's crash reporter, already blocked from the network, and the DLSS updater Steam's overlay
    starts.
  - The loader writes the small crash dump from inside the game.
  - The Crash dump setting (a full dump) lifts the limit, because the loader starts `HellForge.Cli` to write that one.
    So does `HELLFORGE_ALLOW_CHILD_PROCESSES=1`, for diagnosing.
  - The session's `result.json` says which (`childProcesses`).
- **HellForge's own programs stay as they are.** For the session, every file of the installation's `app` folder is held
  read-only, as the game copy's are. Nothing the game runs can replace the launcher, the session runner, the backend or
  the stand-ins for the next start (`appFilesHeld` in `result.json`).
- **Only programs on this PC reach the backend.** Its loopback HTTP answers a request only when:
  - its `Host` is `127.0.0.1`, `localhost` or `[::1]`, with any port, matched exactly (so `LOCALHOST` is refused);
  - and it carries none of a browser's marks (`Origin`, `Referer`, any `Sec-Fetch-*`).

  Anything else gets 403 with `{"errorCode": "HELLFORGE_NOT_FROM_THIS_PC"}` and `web_page_rejected` in
  `requests.jsonl`, before its body is read. A web page in the player's browser cannot call it, by a form, a fetch or
  DNS rebinding. The game, the session runner and the launcher send none of these marks.
- **Records are bounded both ways** (`Multiplayer/RecordGuard.cs`). The game's own lobby record, live state and profile
  objects are rebuilt before friends or the directory get them, and friends' and strangers' before the game reads them.
  - A lobby record keeps only the fields the game uploads, and its players only theirs.
  - Every value has a depth, list length and text length: lobby records 16 KiB and 256 characters of text, profile
    objects 64 KiB and 4096.
  - Keys are at most 64 characters, and control characters are taken out of text.
  - A stranger's listing is not unpacked past 64 KiB.
  - One field is longer than other text: `seed`, the selected mission
    ([The selected mission](multiplayer-lobbies.md#the-selected-mission-seed)), kept only in its exact layout.
  - The local PlayFab keeps at most 32 objects of 16 KiB each per entity, 16 entities.
  - These are bounds, not a closed channel. Code in the game can still send a few KB of text in its record's fields (a
    player name, the state) to friends and the public listing on every change.
- **A host's stats files are rebuilt before the editor reads them.** A joiner's backend writes `host-stats.json`,
  `host-mod-stats.json` and `host-mounts.json` only as JSON objects within RecordGuard's host-file limits.
  - The limits are 1 MiB, depth 6, 8192 items, keys of 256 characters for `mod:` categories, and text of 256.
  - The files are written out by System.Text.Json, so the in-game editor's native JSON reader never parses what the
    host sent.
  - A file that is not JSON is left out. Values past a limit are left out and counted in `multiplayer.log`.

What this does not stop:

- Unsandboxed, code in the game runs with the player's own Windows permissions. It can read their files, write those
  outside HellForge's held folders, and reach other programs of the same user.
- Sandboxed (at low integrity, [steam-bridge.md](steam-bridge.md)), it can still read their files. It can send what it
  reads through the game's traffic, to the lobby's other players among others.

That is why code from another player downloads only when HellForge has verified it or the player trusts it. It is why
a stranger's can be trusted only in a sandboxed session, and why that trust counts only in one. The risk is the
player's to take: the prompt says what the sandbox does not stop, and HellForge does not know what is in the mod. A
host's listing cannot vouch for it; a name is whatever the host wrote.

## The build log

Mod sync was built on 2026-10-22 (protocol `hellforge-mp-7` then) and the rejoin on 2026-10-23. The build log, with
their two-PC test plans, is in the page's earlier version:
[step 6](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multiplayer.md#step-6-mod-sync-a-joiner-takes-the-hosts-mods-2026-10-22)
and
[step 9](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multiplayer.md#step-9-rejoining-after-a-mod-sync-2026-10-23).
Findings that still hold are in the sections above.

- Logs to collect from a mod sync test: both sessions' `multiplayer.log` ("Mod sync:" lines), `backend/mod-sync.json`
  and `modloader.log` ("mod sync" lines).
- For the rejoin, the joiner's `multiplayer.log` reads "Rejoin: ... on the ship", "asking", "lets this player back",
  then "back in \<host>'s lobby". The host's reads "may rejoin lobby ... after a mod sync". When the host has moved to a
  new lobby, the joiner is invited into it ("their lobby now").
- Also collect the joiner's `PlayFabMultiplayerWin.dll` log ("invited to lobby", `InviteReceived`,
  `PFMultiplayerJoinLobby`) and engine log (`Lobby: InviteReceived`).
