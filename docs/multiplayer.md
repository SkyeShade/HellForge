# Co-op and public lobbies

This page explains how HellForge's co-op works: first for players, then for contributors working on it. The second half
is the research and build log from October 2026, kept as history.

Players, each running HellForge with their own local backend, play together through the game's own social menu, join
flow and Galactic Map. Friends find each other through Steam presence; public lobbies are listed on Steam's lobby
service, never on Arrowhead's. There is no port forwarding and no central HellForge server. The copied game never
reaches PlayFab, Arrowhead's services or vanilla players' sessions.

## How co-op works for players

1. In the launcher, open Settings. The MULTIPLAYER card has two switches, "Local PlayFab" and "Lobby and session through
   Steam friends". Both are on by default, and both are needed. A change applies to the next session. (The launcher's
   console still calls them experimental.)
2. Compare modlist codes. Friends need the same gameplay mods, in the same versions with the same files; the order is
   up to each player, and cosmetic mods, packs and Lua mods are each player's own. The Mods page shows the code.
   [mods.md](mods.md#playing-with-friends) describes what each player sees when a join is refused. When the host shares
   their mods, the joiner can download them and restart with them instead
   ([mods.md](mods.md#playing-with-other-mods)). Once the restarted game is on the ship, the host's invitation to their
   lobby waits in game: accept it to join.
3. Start a session. In the game's social menu, the friends list shows Steam friends who are playing HELLDIVERS 2 with
   HellForge running, and you join their game from there. Steam sign-in, Steam friends and Steam lobbies are the real
   ones.
4. In a friend's lobby, every player keeps their own in-game editor changes and can keep making them. HellForge
   loads what each player's changes need on every machine (a vehicle's swapped weapon, a pod's pickup), so friends
   don't see missing assets. The editor's header shows "JOINED · <name> · YOUR OWN STATS", and the host's shows
   "HOSTING · EVERYONE KEEPS THEIR OWN". A host can instead have everyone play with the host's changes: "Everyone plays
   with my editor changes" under "When I host" on the launcher's Mods page (MOD SYNC card). Then a joiner's editor turns
   grey and read-only, with "READ-ONLY · HOST: <name>", and the host's shows "HOSTING · FRIENDS PLAY WITH YOUR STATS".
   [Stats in co-op](#stats-in-co-op) says what each way means for what other players see.
5. When the host leaves, the lobby closes for everyone, as HD2's host ends the session. If you were playing with the
   host's stats, your own come back.
6. Who may reach you at all is "Play with strangers" (Settings > MULTIPLAYER), off by default: off, only your Steam
   friends reach your backend, and the Galactic Map shows your friends' public lobbies only. See
   [Play with strangers](#play-with-strangers) for what turning it on risks.
7. The game's own Privacy setting (Options) decides who may join a lobby you host:
   - **Public:** with Play with strangers on, any HellForge player; with it off, your Steam friends only, and the lobby is
     not listed. The lobby is listed for other HellForge players, who see it on the Galactic Map
     and can join it from there. It shows its players' names and the host's Steam account id. Lobbies a player could
     not join (a blocked mod, mods to install by hand, or a host who shares edited stats unless shown) are left off
     their map. A Steam friend's lobby is shown even when it runs code mods HellForge has not checked (when the friend
     shares their mods, the default): joining it brings the MOD SYNC prompt, where you can trust them.
   - **Friends only:** your Steam friends, and players you invite. It is not listed.
   - **Invite only:** players you invite from the game's social menu. Friends you have not invited are refused too. It
     is not listed.

   Changing the setting during a session never removes anyone: players already in the lobby stay, and a player who was
   in it (whose game crashed, say) may come back. A refused player's game says why ("The host's lobby is Friends only").
8. To keep your Public lobbies off other players' Galactic Map, turn off "List my Public lobbies on the Galactic
   Map" on the launcher's Mods page (MOD SYNC card). It is on by default. Your Public lobby then still
   takes anyone who joins it, but players who are not your friends cannot find it.
9. Invitations from the game's social menu reach Steam friends running HellForge, and the invited player may join even
   an Invite only lobby.
10. When the host kicks a player, that player's game leaves the lobby and says "Removed from <host>'s lobby", their game
   traffic is cut off at once, and they cannot join that lobby again while it lasts.

Each session's logs hold `backend/multiplayer.log` (the backend's side of co-op) and `coop-diagnostics.log` (what this
machine's game saw of mod content). Collect both players' logs (launcher Console > "Save logs to Desktop") for a bug
report.

## How co-op works for contributors

HellForge replaces the services and never patches the game's logic. The game's join checks still run as they always
do (see [The join gate](#the-join-gate)).

### What a session starts

With "Local PlayFab" on (`--local-playfab`, settings key `localPlayFab`), the session runner redirects the game's
PlayFab web API to the local backend (`HellForgeLocalRoutingPatch.StartPlayFabRedirect` in
`src/HellForge.Session/LocalRoutingPatch.cs`: heap data only). It waits for the engine to format the PlayFab URL, for as long as the game
runs (a start-up dialog such as the GPU driver warning can hold the game for minutes), and replaces it with
`http://127.0.0.1:<port>/playfab`; three instruction checks pin the layout. With it on, the session
does not pass `-ClearPlatformMatchmaking`.

With "Lobby and session through Steam friends" on (`--multiplayer`, settings key `sdkShims`), the session runner:

1. swaps the copy's `PartyWin.dll` and `PlayFabMultiplayerWin.dll` for HellForge's stand-ins, after checking their
   pins. The originals are restored after the session, or at the next start if a session was cut short;
2. copies the real `steam_api64.dll` to `<run>/steam-link/` for the backend's Steam link;
3. starts the backend with `--pipe hellforge-<run id> --link steam:<dll> --mod-digest <digest> --mods <mods folder>`
   and, when there is one, `--mod-list <run>/modlist.txt`;
4. sets `HELLFORGE_PIPE` for the game, so the stand-ins can find the backend.

With multiplayer off, no DLLs are swapped and the backend gets no pipe.

### The local PlayFab web API

`src/HellForge.LocalBackend/PlayFabService.cs` serves the PlayFab calls under `/playfab`. It reads the Steam ID from the
session ticket (never stored) and derives the ids from it: the title player account is the SteamID64 in hex, and the
master account is SteamID64 + 0x1000000000000000. Every HellForge backend therefore gives a friend the same ids with no
exchange.

### The stand-in DLLs

`native/sdk-shims` holds the hand-written parts of the two stand-ins (`lobby_shim.cpp`, `party_shim.cpp`, `link.cpp`,
`shim_common.cpp`). `src/HellForge.SdkShimGen` generates a logged stub for every other export from Microsoft's public SDK
headers, and `scripts/Build-Engine.ps1` builds them ([engine.md](engine.md)). Every call is logged to the session folder.

Each stand-in connects to the pipe named in `HELLFORGE_PIPE` and says `hello [lobby|party, pid]`. The backend then owns
the lobby and Party state. Without the pipe the stand-ins work for this machine only.

- Lobby connection strings are `hellforge1-lobby;<lobby id>;<owner SteamID64 hex>`. Lobby ids are GUIDs.
- Party descriptors are `hellforge1;<network id>;<region>;<connection information hex>`. The connection information is
  `hellforge-host:<entity id>` and the region is `HellForgeLocal`.
- `PFLobbySendInvite` goes to the backend as `lobby.invite`, and a friend's invitation reaches the game as
  `InviteReceived` (see [Lobby privacy, invitations and kicks](#lobby-privacy-invitations-and-kicks)). Without the
  backend an invitation completes with `E_NOTIMPL`.
- `PFLobbyForceRemoveMember` (the owner's kick) goes to the backend as `lobby.kick`, and the Party stand-in's
  `PartyNetworkKickDevice` and `PartyNetworkKickUser` as `party.kick`.
- `PFMultiplayerFindLobbies` (the Galactic Map's search) goes to the backend as `lobby.find`; without the backend a
  search finds nothing.

The stand-ins cannot talk to PlayFab at all, so a vanilla client given a HellForge connection string fails, and a
HellForge client given a vanilla one refuses it.

### The backends and the Steam link

Friends' backends talk to each other; the copied game itself keeps talking to loopback only.

| Piece | Source | What it does |
|---|---|---|
| Steam link | `Multiplayer/SteamPeerTransport.cs` | `ISteamNetworkingMessages` in the backend process (which is not firewalled), channel 0, reliable or unreliable sends with AutoRestartBrokenSession. Accepts sessions from Steam friends only. Send rate 8 MiB/s and 8 MiB buffers, set at start ([Mod sync speed](#mod-sync-speed)); a message Steam does not take is logged. |
| Presence | `SteamPeerTransport.cs`, `FriendPresence.cs` | Sets the Steam rich presence key `hellforge` (to the protocol string) and clears it on exit; every 5 s, lists Steam friends playing HD2 (app 553850) who have the key. |
| Lobbies | `Multiplayer/LobbyService.cs` | The creator's backend owns each lobby, and the lobby stand-in mirrors it from events. |
| Party | `Multiplayer/PartyService.cs` | The host's backend owns each Party network, admits only its lobby members, assigns endpoint ids and routes the game's messages (guaranteed delivery is a reliable Steam message). |
| Peer upkeep | `Multiplayer/MultiplayerHost.cs` | Peers sharing a lobby or network send `peer.ping` every 2 s; one silent for 15 s is treated as gone (members leave, its lobbies and networks end). Logs to `multiplayer.log`. |
| Frames | `Multiplayer/Frame.cs` | A u32 length, a u16 field count, then length-prefixed fields; at most 1 MiB. |

Paths in the table are under `src/HellForge.LocalBackend/`. Endpoint ids requested by this machine's game start at
1000; each friend's endpoints take ids in a block of 64 starting at 4096. The lobby's size limit is the game's own
`maxMembers`. Test transports (in memory, and loopback UDP) stand in for Steam in the tests.

The protocol is `hellforge-mp-6`: mods set up in id order, added entities numbered by resource id, and mod content
values in `stats.set`. A backend with another protocol is refused.

| Frames | Names |
|---|---|
| Pipe (stand-in to backend) | `hello` |
| Lobby | `lobby.create`, `lobby.join`, `lobby.update`, `lobby.leave`, `lobby.kick`, `lobby.invite`, `lobby.find`, `lobby.result`, `lobby.memberAdded`, `lobby.memberRemoved`, `lobby.updated`, `lobby.disconnected`, `lobby.found`, `lobby.inviteReceived`, `lobby.refused` |
| Party | `party.host`, `party.connect`, `party.endpoint`, `party.endpointGone`, `party.leave`, `party.message`, `party.kick`, `party.connected`, `party.endpointCreated`, `party.endpointDestroyed`, `party.deviceJoined`, `party.deviceLeft`, `party.networkGone` |
| Between backends only | `peer.ping`, `account.key`, `presence`, `presence.hello`, `stats.set`, `war.clock`, `lobby.invited`, `lobby.rejoin`, `lobby.rejoined` |
| Live-edit pipe (backend to game) | `session`, `notice`, `set`, `reset`, `base`, `get` |

### Joining and the modlist gate

A joiner must join as themselves: the member id must equal the SteamID that Steam networking names as the sender. Its
member properties must keep to PlayFab's limits (below). The host's backend checks a join in this order:

1. whether the joiner may join at all: not kicked from this lobby, and allowed by its privacy
   ([Lobby privacy, invitations and kicks](#lobby-privacy-invitations-and-kicks)). This comes first, so a refused
   stranger is never offered the host's mods;
2. the protocol;
3. the modlist digest, so a mismatch is refused before the game sees a member join;
4. whether the lobby is locked or full.

A refused join is answered with HRESULT `0x89236227` ("lobby not joinable"), after `lobby.refused` with the reason.

The digest is a SHA-256 over the gameplay mods sorted by id (id, version and file hashes), so load order does not
count. It leaves out the Local Stats mod (`zz.local.stats`), cosmetic mods, mods made only of Lua patches, and files a
zipped mod writes into its own folder. The joiner's backend also sends its gameplay mods one by one (`--mod-list`, the
session's `modlist.txt`, HellForge.Core's `ModlistDiff`), so the refusal can name the mods that differ. The host sends
`lobby.refused` [reason, up to 6 lines for the joiner's game]. Both backends turn it into notices ("Couldn't join
<host>" for the joiner, "<friend> couldn't join" for the host) and post them to their game over the live-edit pipe
(`notice`), and the in-game editor draws them over the game: the joiner's as a panel, the host's (kind `brief`,
`HF_NOTICE_BRIEF`) as a small note in the top right corner. Backends from before the list send only the reason; they
still interoperate.

Once a join is accepted, the host sends `war.clock`, and the joiner's backend adopts the host's war time. A time below
0 or above 20 years is ignored, whoever sends it.

Lobby frames from other machines keep to PlayFab's limits (`PFLobby.h`): at most 30 lobby, member and search
properties each, at most 60 changes in one update, and the result within the limits. HellForge also caps a key at 64
characters and a value at 1,024; the game's longest value seen is 123 characters, its Party descriptor. Search keys
must be `string_key1`..`string_key30` or `number_key1`..`number_key30`, and a `number_key`'s value must be a number. A
frame that breaks them fails with `E_INVALIDARG`.

### Lobby privacy, invitations and kicks

**Privacy.** The game advertises its Privacy setting in its own lobby, so the host's backend reads it there
(`LobbyService.PrivacyOf`). Lobby property `"19"` (PrivacyMode) and search property `number_key6` carry the same value:

| Value | Setting | Who the host's backend admits | Listed |
|---|---|---|---|
| 0 | Public | anyone who reaches it (a stranger only through the lobby directory) | yes, unless the listing switch is off |
| 1 | Friends only | the host's Steam friends, and invited players | no |
| 2 | Invite only | invited players only; friends who were not invited are refused | no |
| 3 | Friends and clan | as Friends only (HellForge has no clans) | no |
| missing | | as Friends only | no |

The evidence:
- The values come from HD2Runtime's research on build F5FEE03DCFDB: the settings value `privacy_mode`, name table
  0x21D3F28 (Open, FriendsOnly, InviteOnly, FriendsAndClan), lobby key 19 `PrivacyMode` (search attribute 6), and the
  host's join gate 0x108BC70, which refuses a non-friend under 1 and anyone not invited under 2. An active SOS Beacon
  advertises 0.
- The stand-in logs captured 0, 1 and 2 at lobby creation. `"19"` and `number_key6` were always alike, and the access
  policy was always 0 whatever the setting, so the access policy says nothing about privacy.

The game's own join gate still applies the same rules once a member is in. The backend refuses first, before the
modlist check and before the game sees a member join.

**Who is always let back.** Under every setting, a machine already in the lobby, or one that was admitted to it before
(its game crashed, say), may join again. So changing the privacy never removes anyone. A machine refused only for its
gameplay mods passed the privacy check, so it is expected back for 15 minutes with the host's mods
([Mod sync](#mod-sync)), whatever the privacy then; a kick ends that too. A lobby that turns Friends only
or Invite only stops being listed within a second, and its members stay.

**Invitations.** The game calls `PFLobbySendInvite` from its social menu (seen in the stand-in logs).
- The stand-in sends `lobby.invite [op, lobbyId, sender, invitee]`. A member's backend forwards it to the lobby's owner,
  which records the invitee for the lobby's lifetime (at most 64) and answers.
- The inviting player's backend then sends `lobby.invited [lobbyId, connection string, inviter]` to the invitee's
  backend. That backend takes it only from a Steam friend, at most 10 a minute, and its stand-in hands every listening
  entity `InviteReceived`.
- A stranger can be invited (the lobby admits them), but no invitation reaches a stranger's game: the stranger joins
  from the Galactic Map or a link.
- A stranger the lobby expects (admitted before, or invited) may open a Steam session and send a join even when nothing
  is listed (`LobbyService.Welcome`).

**Kicks.** The owner's `PFLobbyForceRemoveMember` becomes `lobby.kick [op, lobbyId, memberId, preventRejoin]`, which only
the owner's own machine may send. The backend then:
1. removes the machine's members;
2. sends the kicked machine `lobby.memberRemoved` with reason 1 (LocalUserForciblyRemoved), then `lobby.disconnected`
   with reason 0 (NoLocalMembers), as the SDK reports a force removal. The kicked machine's backend shows the game
   "Removed from <host>'s lobby";
3. sends every other member `lobby.memberRemoved` with reason 2;
4. answers its own game with the completion, after that game's own MemberRemoved, as the SDK documents.

What follows:
- The machine is refused for the lobby's lifetime, whatever `preventRejoin` says (decided 2026-10-23), and it cannot be
  invited back. Nothing is stored.
- `PartyService.Expel` takes the machine out of every Party network the host has and tells it the networks are gone.
  It has no endpoints left, so whatever it sends is dropped, and it cannot connect again because it is in no lobby.
- The Party stand-in's `PartyNetworkKickDevice` and `PartyNetworkKickUser` on a hosted network send
  `party.kick [networkId, node]`, which kicks that machine from the host's lobbies the same way.

Which of the two kick calls HD2 makes is not known, so both are covered:
- No session log shows either call, and the exe's list of 22 `PFLobby*`/`PFMultiplayer*` imports was not kept.
- 20 functions have been seen called in session logs, and the Galactic Map's search (`PFMultiplayerFindLobbies`) is a
  21st. That makes `PFLobbyForceRemoveMember` a likely 22nd, but this is an inference.

### Mod sync

A joiner refused for different gameplay mods can take the host's (protocol `hellforge-mp-7`). Before the refusal the
host sends its mod set (`mods.offer`); the joiner's backend plans the difference (`ModSyncPlan` in HellForge.Core) and
the in-game editor shows the MOD SYNC prompt (SDK 14 `sync_state`, `sync_mod`, `sync_answer`). On accept the joiner
downloads each mod's counted files from the host over the Steam link (`mods.files`, `mods.get`; speeds in
[Mod sync speed](#mod-sync-speed)), checks every file and the whole mod (`SyncedStore.Install`), keeps it in `mods/.synced/<id>@<hash>/`, and the
launcher restarts the session with the host's mods (`--mod-sync`). Each mod's class comes from the bytes received,
with one rule for every host, Steam friends included (`ModSets.ClassOf`). Content (data specs HellForge checks all of,
`DataSpecs.Unchecked`; schema stats; network field types) downloads from any host. Code and raw data (binary data
patches, data specs with `"replace"` or unreviewed resource types such as `lua`, raw stats lines, `componentWrites`)
download only from a Steam friend (a host serves them to friends alone), and only when `data/verified-mods.json` names
their exact content hash or the player trusted it. A friend's unverified one is `SyncAction.Trust` ("needs your trust"):
the prompt's TRUST, after a warning, answers `trust`, which adds `{id, hash, name, version, from, since}` to
`"trusted"` in `mods/hellforge-mods.json` (`ModSettings`, beside `"blocked"`; the Mods page lists and untrusts them) and
makes the offer again, downloading once nothing else waits. The joiner's checks then take the verified list or that
list (`ModSync.Allowed`), every byte checked as before. A mod's manifest `url` (an `https://` address,
`ModCatalog.WebAddress`) travels in its mod set entry; GET IT answers `open`, and the joiner's backend opens that
address (checked again) in the default browser. Bundled mods never download. The host side is
`src/HellForge.LocalBackend/Multiplayer/ModSync.cs`; the protocol and limits are in
[Step 6](#step-6-mod-sync-a-joiner-takes-the-hosts-mods-2026-10-22) below.

**Rejoining after the restart** (`Multiplayer/AutoRejoin.cs`; the research is
[Step 9](#step-9-rejoining-after-a-mod-sync-2026-10-23)). The restarted game is brought back into the host's lobby
through the game's own invitation path, the one a friend's invitation takes. HellForge does not make the game join.
- The session runner passes the sync file to the backend (`--rejoin <install>/temp/mod-sync/session.json`), but only
  when mods-stage found all of the host's mods.
- The backend takes the file only while it is less than 15 minutes old, when its connection string names the host's
  lobby (and not this player's own), and when the session's modlist digest is the host's.
- It waits until the game creates its own lobby, which means the game is on the ship (`LobbyService.LocalLobbyCreated`).
  It waits at most 3 minutes, then 5 s more for the ship's screens.
- It then asks the host: `lobby.rejoin [lobby id, modlist digest, protocol]`.
- The host answers for the lobby asked. If that lobby has closed, it answers for the one lobby its own game owns now.
  It checks the answer as it would a join: kicked, privacy, version, mods, lock, room.
- The answer is `lobby.rejoined [lobby asked, "ok", lobby id, connection string]`, or
  `[lobby asked, gone|full|locked|refused|mods|version, line...]`.
- On "ok" the lobby stand-in hands the game `InviteReceived` from the host (`LobbyService.InviteLocal`), and the in-game
  editor shows "Rejoining <host>".
- Any other answer, no answer within 10 s, or no ship within 3 minutes gives up once. The game shows "Couldn't rejoin
  <host>" with the reason and, where it helps, how to join by hand. Nothing is tried again.
- A join the game starts by itself ends the rejoin. Into the host's lobby, the player is rejoining; into another
  lobby, the player chose that one.
- **Admission.** The host keeps a machine refused only for its gameplay mods expected back in that lobby for 15 minutes
  (`LobbyService.ExpectedBack`), and an "ok" keeps it expected for 3 minutes more (`RejoinGrace`). Its join is then
  admitted whatever the lobby's privacy, and a stranger's Steam session and join pass the stranger gate
  (`LobbyService.Welcome`). An invited player stays invited, as a refusal never removes an invitation. A kick removes
  the expectation.
- **Older hosts.** A host from before `lobby.rejoin` does not answer, and the joiner's game says the host did not
  answer. The protocol stays `hellforge-mp-7`.

### Mod sync speed

**What held the first two-PC test back (2026-10-23).** The joiner downloaded 38 MB (two of a Steam friend's mods, 44
files) in 163 s, about 0.24 MB/s. The 39.48 MB mod took 161 s of it, which is 245 KB/s.

That is Steam networking's own send rate:
- A connection sends at a fixed rate between `k_ESteamNetworkingConfig_SendRateMin` and `SendRateMax`. Both are
  256 KiB/s (262,144 B/s) by default.
- It estimates no bandwidth. The header says the two "should always be set to the same value".
- With Min below Max, a connection starts at about four packets per round trip (RFC 3390) and is clamped to Min..Max.
  At a 136 ms round trip that is Min. Raising only Max changes nothing.
- 256 KiB/s less the packet and message framing is the 245 KB/s measured.

Sources: GameNetworkingSockets
[csteamnetworkingsockets.cpp](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/src/steamnetworkingsockets/clientlib/csteamnetworkingsockets.cpp)
(`DEFINE_CONNECTON_DEFAULT_CONFIGVAL( int32, SendRateMin, 256*1024, ...)`, the same for `SendRateMax`; `SendBufferSize`
512 KiB; `RecvBufferSize` 1 MiB),
[steamnetworkingsockets_snp.cpp](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp)
(`SNP_ClampSendRate`; `SNP_SendMessage` refuses a message past `SendBufferSize` with `k_EResultLimitExceeded`) and
[steamnetworkingtypes.h](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/include/steam/steamnetworkingtypes.h)
("The default value is 256K"). The Steamworks page
[steamnetworkingtypes](https://partner.steamgames.com/doc/api/steamnetworkingtypes) still says "Default is 0
(no-limit)", which is out of date.

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
| Steam link turn | a sleep of a timer tick | sends at once (`Send` wakes the thread); a turn with traffic is followed by the next at once | `SteamPeerTransport.Run` |

**Game traffic.** A Steam connection without congestion control sends whatever it is given at that rate, and a peer's
game frames share it with mod sync. `ISteamNetworkingMessages` channels share one connection and its one lane, so a
channel of its own would not let game frames pass the chunks. The joiner's window keeps the queue short instead (TCP
Vegas and LEDBAT do the same):
- It times each whole chunk from asking to arrival.
- A chunk within 100 ms (`TargetDelay`) of the quickest so far adds one chunk: a doubling each round trip at first,
  then one chunk per round trip once it has been cut.
- A later chunk takes a quarter off, at most once per round trip and never below 2.

So whatever the slowest stretch is (the host's upload, a relay, the joiner's link), its queue stays near 100 ms, and
the window fills it.

**Older hosts.** A host from before this change (Steam's 256 KiB/s, a queue of 8) still works with a new joiner:
- Its chunks queue at once, so the window stays near 2.
- A request it refuses as "too many requests at once" halves the window's ceiling, and that chunk is asked again.

The speed comes from the host's end: an updated joiner downloads from a host on an earlier 0.3.0 test build at its old speed.

**Expected.**
- On a good connection, 8 MiB/s: about 5 s for the test's 38 MB.
- In practice the host's upload sets the speed (20 to 50 Mbit/s is 2.5 to 6 MB/s, so 7 to 15 s), along with what
  Valve's relays carry, which is not measured yet.
- A sync from a stranger goes through the same link.

**Tests** (`tests/HellForge.LocalBackend.Tests/ModSyncSpeedTests.cs`, over a simulated path without Steam), for a 6 MB
mod of 63 files:

| Path | Download time | Notes |
|---|---|---|
| Next door, no bottleneck | 0.76 s | `PeerRate` allows 0.72 s and is never exceeded |
| 120 ms round trip | 1.55 s | The old design: about 13 s |
| 2 MiB/s link, 80 ms round trip | 3.65 s | The link alone takes 3.03 s |

On the 2 MiB/s link, a small frame the host sends every 50 ms down the same link waits a median of 160 ms and at
most 360 ms (40 ms of that is the one-way delay). A fixed window of 16 chunks would queue 2 MiB there, a second.

`DownloadWindow`'s growth, cuts, floor and ceiling are checked on their own as well.
### Trust: what another player's mods can reach

Mod sync and public lobbies bring mods from people a player may not know. What limits them:
- **What downloads by itself** is only what HellForge checks itself (data specs, schema stats, network field types),
  read from the bytes received, with the same rule for every host. Code and raw data need the verified list, or,
  from a Steam friend, the player's own trust after a warning ([Mod sync](#mod-sync)).
- **Synced mods load only in the synced session.** They sit in `mods/.synced`, which normal sessions never load, and
  the player's own Mods page never changes.
- **The copy is off the internet.** The network block lets the copied game reach only this PC (loopback).
- **The game cannot start other programs.** The session runner puts the game in a job whose active-process limit is 1
  (`HostBoundedProcess`). That costs the game's crash reporter, already blocked from the network, and the DLSS updater
  Steam's overlay starts. The Crash dump setting lifts the limit, because the loader starts `HellForge.Cli` to write
  the dump; so does `HELLFORGE_ALLOW_CHILD_PROCESSES=1`, for diagnosing. The session's `result.json` says which
  (`childProcesses`).
- **HellForge's own programs stay as they are.** For the session, every file of the installation's `app` folder is held
  read-only, as the game copy's are: nothing the game runs can replace the launcher, the session runner, the backend
  or the stand-ins for the next start (`appFilesHeld` in `result.json`).

- **Only programs on this PC reach the backend.** Its loopback HTTP answers a request only when its `Host` is
  `127.0.0.1`, `localhost` or `[::1]` (with any port; matched exactly, so `LOCALHOST` is refused) and it carries none of
  a browser's marks (`Origin`, `Referer`, any `Sec-Fetch-*`). Anything else gets 403 with
  `{"errorCode": "HELLFORGE_NOT_FROM_THIS_PC"}` and `web_page_rejected` in `requests.jsonl`, before its body is read:
  a web page in the player's browser cannot call it, by a form, a fetch or DNS rebinding. The game, the session runner
  and the launcher send none of these marks.
- **Records are bounded both ways** (`Multiplayer/RecordGuard.cs`). The game's own lobby record, live state and profile
  objects are rebuilt before friends or the directory get them, and friends' and strangers' before the game reads them:
  a lobby record keeps only the fields the game uploads (its players only theirs), every value has a depth, list
  length and text length (lobby records 16 KiB, 256 characters of text; profile objects 64 KiB, 4096), keys are at
  most 64 characters, and control characters are taken out of text. A stranger's listing is not unpacked past 64 KiB.
  One field is longer than other text: `seed`, the selected mission (below), kept only in its exact layout.
  The local PlayFab keeps at most 32 objects of 16 KiB each per entity, 16 entities. These are bounds, not a closed
  channel: code in the game can still send a few KB of text in its record's fields (a player name, the state) to
  friends and the public listing on every change.
- **A host's stats files are rebuilt before the editor reads them.** A joiner's backend writes `host-stats.json`,
  `host-mod-stats.json` and `host-mounts.json` only as JSON objects within RecordGuard's host-file limits (1 MiB, depth
  6, 8192 items, keys of 256 characters for `mod:` categories, text of 256), written out by System.Text.Json, so the
  in-game editor's native JSON reader never parses what the host sent. A file that is not JSON is left out; values
  past a limit are left out and counted in `multiplayer.log`.

What this does not stop: code in the game runs with the player's own Windows permissions. It can read their files,
write those outside HellForge's held folders, and reach other programs of the same user. That is why code from
another player downloads only when HellForge has verified it, or when the player trusts the friend it comes from.

### Public lobbies (the lobby directory)

Each backend lists the public lobby its game hosts as an Invisible Steam lobby of the game's app, held beside the game's
own (`Multiplayer/SteamLobbyDirectory.cs`, `LobbyDirectory.cs`, `DirectoryService.cs`). Its data carries the lobby's
PlayFab search properties, its record with every account id but the host's at 0, its mod set and whether the host plays
with edited stats. The lobby stand-in sends the game's `PFMultiplayerFindLobbies` to the backend (`lobby.find`), which
searches the directory and evaluates the game's filter, drops lobbies the player could not join, and answers
`lobby.found`. The map's records come from the listings (`POST /lobby/accounts`).

All of this needs [Play with strangers](#play-with-strangers): off, the Steam link accepts no one who is not a Steam
friend. On, it accepts someone who is not a Steam friend only while it deals with them (`StrangerGate.cs`):
- while a lobby is listed (and Steam holds the listing);
- a member or host of a lobby here;
- a machine a lobby here expects back;
- a mod download.

A lobby is listed only while it is Public, the player's `"listPublic"` switch is on (the Mods page; on by default) and
Play with strangers is on.

A stranger's listing is checked before anything of it reaches the game (`DirectoryService.Valid`, `LobbyRecords.cs`).
One that breaks any of the following is left out whole:
- a GUID lobby id, and a connection string naming the listing's host;
- 1 to 4 members, with a limit of 2 to 8 (the game makes its lobbies for 8);
- a lock of 0 or 1;
- only PlayFab search keys, at most 30, within the limits above;
- a record rebuilt from the schema of the game's own record builder (game.dll 0x12CB190). Known fields must be of the
  right kind and in range: at most 4 players, names of at most 32 characters, account ids that are numbers, at most one
  host, who must be the listing's. Unknown fields are dropped.
- a `seed` in the exact layout below, if the record has one.

The published record is rebuilt from the same schema, with every account id but the host's at 0.

**The selected mission (`seed`) is what puts a lobby on the map.** The game's `PUT /lobby` carries `"seed"` only once
the host has selected a mission. It is the mission as fixed-width lowercase hex, version `f3fb` (game.dll encoder
`0xFC3E20`; the host's lobby key 10 holds the same string). The Galactic Map's planet scan decodes it (`0xFC39F0`) and
draws a scanned lobby's squad marker ("1/4") and hover card from it: no seed, no marker, even when the search found the
lobby and its record arrived. Until 2026-10-24 neither `RecordGuard`, whose field list came from an upload made before
any mission was selected, nor `LobbyRecords`, which allowed 32 characters, kept it. So no listed lobby showed on
another player's map.

The decoder writes the string's arrays into a 0xE8-byte struct on the stack without checking their counts, so a forged
seed could overflow it. `MissionSeed.Valid` keeps one only in the `f3fb` layout:
- version `f3fb`, then 58 hex digits of fixed fields;
- four arrays, each a count byte then its entries: u32 (at most 32), bytes (8), bytes (3), bytes (2), then a byte;
- an array of u32 (at most 4), two bytes, and a last array of u32 (at most 8);
- nothing after it, and at most 458 digits in all.

A host's seed for a D1 mission is 102 digits. A record with any other seed loses it (between friends and in this
machine's own listing), and a stranger's listing with one is left out.

Which lobbies a search shows is mod sync's rule (`ModSync.Joinable`), and it uses the plan a join would make
(`ModSyncPlan`):
- A Steam friend's lobby running code or raw mods that are not verified is shown, because joining brings the MOD SYNC
  prompt with TRUST.
- A stranger's lobby running such mods is hidden, because they are mods to install by hand.

Each search logs what it found, what it showed, and why it left out the rest: `Directory: search "<the game's filter>": 2
listed, 1 shown (1 left out: it needs mods installed by hand)`. The reasons are:
- `this machine's own lobby`, which Steam's search returns too;
- `malformed (...)`;
- `the game's filter`, which covers clauses Steam's search does not apply, such as difficulty ranges and member counts;
- `its host plays with edited stats`;
- `its mod list is malformed`;
- `it runs a blocked mod`;
- `its host runs another HellForge version`;
- `it needs mods installed by hand`.

Until 2026-10-23 a search left this machine's own lobby out without counting it. On the first two-PC test, "1 listed, 0
shown" was the searching player's own lobby, not the friend's:
- It was listed from the planet's search once the player's own listing had been rewritten for that planet (11:22:25,
  "3 listing value(s) written").
- None of the lines named a reason, and every other path names one.
- The friend's backend first reached this one at 11:26:17, after the last search.

`HELLFORGE_TEST_STRANGERS=1` makes a backend treat friends as strangers, for testing with two friends' PCs. Details and
open questions: [Step 7](#step-7-public-lobbies-on-the-galactic-map-2026-10-22).

### Play with strangers

Off by default, in `mods/hellforge-mods.json` (`playWithStrangers`): the launcher's Settings > MULTIPLAYER switch, or
`HellForge.Cli settings --strangers yes|no` ([external-launchers.md](external-launchers.md#profiles-and-settings)). The
backend reads it again within 2 s of a change, also during a session, and never more than once per 2 s however many
frames strangers send (`Multiplayer/StrangerSetting.cs`).

Off:
- every frame from someone who is not a Steam friend is dropped (`StrangerGate`; logged once a minute per machine),
  and the Steam link accepts no session from them;
- this machine's public lobby is not listed ("Directory: this machine's public lobby is not listed: Play with strangers
  is off" in `multiplayer.log`);
- a Galactic Map search shows friends' listings only, and only the one each friend's backend vouches for (see
  "Listings' hosts" under [step 7](#step-7-public-lobbies-on-the-galactic-map-2026-10-22));
- a join to a stranger's lobby is answered at once as not joinable, and the in-game editor shows why ("Couldn't join").

On, strangers' games send this one data nobody checks for them: HellForge is open source, anti-cheat is off and a
modified build can lie about its mods, so a flaw in the game's network code could let a stranger run code on this PC
(lockdown plan item 10). The launcher's warning says so. Until the game runs sandboxed ([steam-bridge.md](steam-bridge.md)),
play with strangers only when that risk is acceptable.

### The join gate

The host's join gate in the game (game.dll 0x108BC70) still applies its privacy rules and compares the game and
network-config hashes. HellForge never fakes those checks.

### Player keys

Each game logs in with a public key, and the host's game asks the backend for a joiner's key before it admits them (the
research is under [Player keys](#player-keys-research) below). HellForge keeps each game's own key from its login, as
text, verbatim (1 to 256 printable characters). The backends swap keys over the Steam link as `account.key`: on the
first frame heard from a friend, when the local key changes, and in reply to a new key. Each backend can then answer
`/Account/Keys` for every player in its session (`AccountKeys.cs`, `Multiplayer/AccountKeyExchange.cs`); it waits up to
3 s for a key. Platform account ids are taken to be SteamID64s (confirmed in the second two-PC test). A lookup with no
key is logged to `multiplayer.log` with the id.

### The social tab

The friends list and the in-game state of friends come from `FriendPresence.cs`. It lists Steam friends who have the
`hellforge` rich presence key, without sending anything to anyone else. Friends' backends send `presence.hello` to a
newly found friend at once, and `presence` every 10 s or on a change: the player's PlayFab profile objects (which the
engine needs to list a profile) and the game's current lobby with its HellForge connection string. A friend's lobby
stays listed for 45 s after their last frame, and a profile request waits up to 6 s for friends' objects. Accounts are
taken to be SteamID64s; unanswered ids are logged.

### Stats in co-op

By default every player keeps their own Local Stats, and joiners can edit as the host can. A host may share its stats
instead (ModSettings `hostStats`, the Mods page switch "Everyone plays with my editor changes", under "When I host"); then
everyone in its lobby plays with the host's. Protocol `hellforge-mp-8`. How host's stats were first built is under
[step 5](#step-5-stats-in-co-op-the-hosts-values-everywhere-2026-10-17), and the per-player default under
[step 10](#step-10-every-players-own-stats-and-the-lobbys-assets-2026-10-23).

What each way means in game, from HD2Runtime's live tests (`docs/projectile-homing.md`, the Gas EAT test in
`research/docs/runtime-peer-messaging-F5FEE03DCFDB.md`): every machine simulates its own copy of each shot from its
own data, and the shooter's machine decides the hit and the damage. With their own stats, each player's weapons hit as
their own edits say, while friends see those shots as their own data has them (a swapped round flying as the stock
one, say). A vehicle's arm swap is a type-record edit on the machine that calls the vehicle in, so vehicles a player
calls in carry that player's swaps. Whatever a change puts in the game for everyone (that vehicle's weapon, a pod's
pickup) needs its package on every machine: [Assets in co-op](#assets-in-co-op).

- The Local Stats mod (`zz.local.stats`) is not part of the modlist digest, so friends with different saved changes can
  play together. Every other gameplay mod must still match.
- The host's backend (`StatsShare`, `src/HellForge.LocalBackend/Multiplayer/StatsShare.cs`) sends every friend admitted
  to a lobby hosted here `stats.set` [version, then three files gzipped, then `host` or `own`], again whenever its
  switch or a shared file changes. Sharing, the files are `mods/zz.local.stats/hellforge-stats.json` (the session
  passes `--mods`), the editor's `mod-stats.json` (mod content values) and the editor settings' `mounts` (arm swaps and
  hellpod payloads), checked once a second (the editor saves on APPLY, RESET ITEM and preset import). Not sharing, the
  files are sent empty.
- The joiner's backend accepts `stats.set` only from the host of the lobby it joined. With `own` its game keeps its own
  values. With `host` it writes the files into the editor's folder as `host-stats.json`, `host-mod-stats.json` and
  `host-mounts.json`, and serves the session's live-edit pipe (`\\.\pipe\hellforge-live-<run id>`,
  `native/mod-loader/live.c`). `HellForge.Core.StatsSync` turns the host's file into live edits of the Local Stats mod:
  `set` for the host's values, `base` for the joiner's own saved changes the host does not have. Attachments and status
  multipliers go over the pipe too.
- The game is told its role over the pipe (`session`: solo, host or joiner, `stats=own` or `stats=host`, and the host's
  name; `HfApi.session` and, from SDK 15, `HfApi.stats_source` for mods). A joiner playing with the host's stats has an
  in-game editor that allows browsing and export, and refuses typing, APPLY, RESET ITEM, RESET TO DEFAULTS and preset
  import.
- When the host leaves, closes the lobby, stops answering (15 s) or stops sharing, the host files are deleted and the
  joiner's own values come back (`set`, or `reset` where it had none).
- Not synced: a stats mod's own records (`"own"`), and armor perks, which are solo only (with more than one player,
  everyone gets the armor's own).
- Trust: joiners of a sharing host take its values. Between friends that is fine; a public lobby whose host shares
  edited stats is hidden from other players' maps unless they choose to show such lobbies.

### Assets in co-op

What one player's change puts in the game for everyone (a vehicle's swapped weapon, a pod's pickup, a swapped round's
effects) is made on every machine, and a machine without its package shows a missing asset (the purple question mark)
or an invisible effect. HellForge loads a change's packages on the machine that makes it
([internals.md](internals.md#packages)); `AssetShare` (`src/HellForge.LocalBackend/Multiplayer/AssetShare.cs`) has every
other machine in the lobby load them too.

- The mod loader sends its own packages up the live pipe whenever they change, as `packages <ids>` (16 hex digits
  each, comma separated, `-` none): those it holds for this machine's edits, mods and actions
  (`native/mod-loader/packages.c`).
- A joiner's backend sends its game's to the host of the lobby it joined, numbered: `assets.mine` [version, ids]. The
  host keeps each friend's, and sends the lobby's (its own and every friend's) to every friend, numbered, whenever they
  change: `assets.lobby` [version, ids, the version of that friend's own it holds]. A joiner whose latest the host lacks
  sends it again. Lists are at most 64 ids a player and 256 a lobby; malformed ones are dropped and logged.
- Each backend asks its game to load the lobby's packages less its own: `preload <ids>` over the live pipe, again
  whenever they change and when the game connects again. The loader asks for each as for its own edits' (catalogue
  packages only, requested on the game thread, never with the reference map 3/4 full), but a friend's may not take the
  last 16 of HellForge's 64: those stay for this machine's own. Every new one is logged in `modloader.log` ("lobby
  assets: ...").
- Packages are never released, so a change taken back leaves its package loaded for the session.
- Nothing yet makes a change wait until friends have its package. A friend's game starts loading as soon as the change
  is applied (and on joining), which is normally long before anyone calls in what it changed; a vehicle called in at
  once may still show a missing asset on a friend's screen until its package is resident.

### Shots in co-op

Every machine simulates every player's shots from its own data, and nothing about a shot's projectile type travels over
the network (a remote shot is the local fire function run from the shooter's replicated trigger, or an explicit fire
message that carries no type). So without more, a player's projectile swap fired as the stock round on the others'
screens, and on the swapping player's screen the others' guns fired the swap too. Research:
`HD2Mods\research\projectile-per-player` (from a decoded snapshot of build 25480438).

- The mod loader (`native/mod-loader/shots.c`) hooks the ProjectileWeapon fire function (game.dll 0x6128B0) after the
  game has chosen a shot's type (0x614081, before the pellet loop and the projectile pool's constructor), for this
  machine's own shots and its copies of the others' alike, and each pellet (0x615940) where a weapon's programmable
  round replaces it while its function is on (0x615B23, ProjectileWeapon +0x240). Every site's bytes are checked first.
- It finds the shooter: the weapon's wielder (the Wieldable manager), matched to a player's Helldiver (the player list:
  peer and avatar). A player's peer is their SteamID64.
- Each player's swaps come from their Local Stats (the editor's Projectile and Programmable ammo projectile rows) and
  travel with the lobby's assets: `assets.mine` adds `<weapon resource hex>=<type>[/r|/f|/<stock>]`, `assets.lobby`
  adds every player's `<peer hex>:...`, and each backend sends its game `shots <rules>` over the live pipe. Not while the
  host shares its stats (`shots -`): then every machine already plays with the same. The weapon is the one firing
  (`StatsSchema.ProjectileSwaps` maps each Projectile row to it; an ammunition every weapon equipping it shares maps to
  each of them), and the suffix says which of its rounds the swap replaces, recognised on each machine by its stock type:
  - none: ProjectileWeapon +0 (most weapons, and the Maelstrom's and Bastion's machine guns and the M-1000 Maxigun,
    whose round it is after all);
  - `/r`: WeaponRounds +64, a rounds feed (the DBS-2, R-2124, SG-22, SG-451 and SG-8S; the GL-15 Evictor, M90A, P-11
    Stim Pistol, P-4 Senator, P-69 Veto, R-6 Deadeye and SG-97 Sweeper, whose ProjectileWeapon +0 holds the same round;
    and the One-Two's and Arbitrator's underbarrels, weapons of their own). The type resolver (0x7456D0) checks heat
    (flag bit 9), then a magazine (bit 7), then rounds (bit 8, WeaponRounds +0x40); every live WeaponRounds weapon in
    the decoded snapshots (SG-8 Punisher, SG-88 Break-Action Shotgun) has bit 8, and these have no magazine or heat. The
    AC-8, SG-88, GL-52, the Breakthrough's right gun and the GP-31 too; +0x44, a second mode's round, is left alone;
  - `/<stock>`: the round with that stock type. An ammunition's entity delta: the six weapons whose magazines chamber
    it (AR-23 Liberator, JAR-5 Dominator, P-19 Redeemer, P-2 Peacemaker, R-63 Diligence, SG-225 Breaker), the SMG-37
    Defender, and every ammunition option of the SG-8 Punisher and SG-20 Halt (WeaponRounds +64 / +68, written into the
    weapon's own copy when it is built) and the MP-98 Knight. A magazine pattern's entry (MG-43, MGX-42, the A/G-16 and
    A/MG-43 sentries: round n fires entry (n - 1) % count, the tracer included). A charge level's projectile (PLAS-101,
    PLAS-15, PLAS-45: WeaponCharge +4 / +28 / +52, passed at release) or a heat level's (LAS-17: WeaponHeat +4 / +28 /
    +52 at 50, 100 and 190 heat). Where those come from: `mappings/projectile-sources.json`
    (`scripts/ExportProjectileSources.py`, research `HD2Mods\research\blocked-projectile-swaps`). This machine's own
    swap of one is its own rule (the loader does not tie a delta to its weapon). Rounds of one weapon with the same stock
    type (an MG-43's four normal pattern entries, the Loyalist's full charge and overcharge) are one round in co-op: the
    last edit of them is the one the others see;
  - `/f`: the programmable round (ProjectileWeapon +0x240: the GR-8 Recoilless Rifle, RL-77 Airburst, AC-8 Autocannon
    and W.A.S.P.), a swap of its own beside the weapon's normal one.
- A shot fires: the shooter's swap of that round (by its stock type: a type this machine's own swap made stands for the
  stock round it replaced), if they have one and its row and packages are ready here; else, for this machine's player,
  what the game chose; else, for another player, the stock round. A round no swap names stays. Shots by no
  player (sentries, enemies) are left alone; a weapon with a swap in the lobby that fires with no player found as its
  wielder is logged once (`shots: ... no player found as its wielder`), the first thing to check if an underbarrel's
  swap does not show.
- Choosing the type before the pellet loop gives the shot the swapped round's whole row: flight, direct damage, impact
  and expiry explosions, and a shotgun's pellet count. The weapon stays the source's: its ammunition, rate, recoil and
  sounds.
- A weapon firing an entity (the W.A.S.P.'s missiles, the Spear's, the Commando's) spawns it only on the machine with
  authority over the weapon (0x615BE2: entity info +0x14 bit 0, its wielder's), and the entity reaches the others: it
  is the shooter's own, with their swap, already. The editor offers no swap of the entity itself (the Spear, Commando,
  P-33, P-92 and the Patriot's left arm have no Projectile row).
- Not covered: beams (they choose no row). A swap that follows the gun itself (dropped and picked up by another player)
  needs the weapon's network identity through drops, which the research leaves to in-game tests.

#### A player's edited rounds

The same holds for a round's stats: a shot copies them when it spawns, from the spawning machine's own rows
(SpawnProjectile, game.dll 0x13A9830, reads the ProjectileSettings row through its descriptor and copies the DamageInfo
row's four armour-penetration lanes into the shot; damage is read from the DamageInfo row per hit, times the shot's
damage multiplier). So a friend's AP 4 round deflected on a host with AP 2, and this machine's own edits showed on the
others' guns. Research: `HD2Mods\research\coop-weapon-stats` (part B).

- Each player's changes of ProjectileSettings and DamageInfo rows (four-byte fields: flight, the damage and explosion
  ids, armour penetration, damage) travel with the lobby's assets: `assets.mine` adds `P<type>+<offset>=<bits>` and
  `D<id>+<offset>=<bits>` entries (hex offsets and bits, `;` between them, at most 96), `assets.lobby` adds
  `<peer hex>=<entries>` for every player, and each backend sends its game `rounds <list>` (`AssetShare.Rounds`). Not
  while the host shares its stats (`rounds -`).
- The mod loader (`native/mod-loader/rounds.c`) rebuilds every row anyone in the lobby changed, for every other player
  present: this machine's row with this machine's own changes undone (the Local Stats mod's boot edits and the editors'
  live ones, by their originals), then that player's values. It changes one call, the pellet spawn of the ProjectileWeapon
  fire path (0x616612), which makes this machine's copies of every player's hand-held shots. For a shot another player
  fires (the shooter found as in shots.c), SpawnProjectile is handed that player's projectile row (a copy that keeps
  the stock type), and the spawned shot takes their armour-penetration lanes and a damage scale (their standard damage
  over this machine's, else their durable over this machine's). This machine's player's shots, and shots by no player,
  are left alone.
- Not covered: a shotgun's pellet count (read by type in the fire function), explosion values, damage statuses and
  demolition (read by damage id per hit; the damage list is not moved for this), and shots spawned anywhere else
  (sentries, vehicles, stratagems). Which machine decides damage is unchanged: the shooter's.

### When the host leaves

When the lobby's owner leaves, the lobby closes for everyone (`lobby.disconnected`, reason LobbyDeleted). When the
host's game closes, its Party networks end (`party.deviceLeft`, then `party.networkGone`). A host silent for 15 s has
the same effect. The backend has no lobby owner migration. A rejoining friend's old membership and endpoints are
removed first, and the mod loader (`native/mod-loader/netreap.c`) destroys a crashed peer's dead connection so the
friend can rejoin.

### Mod content in co-op

What mods add (stratagems, entities, behaviours, abilities) is numbered alike on every machine whatever the load
order, and HellForge's own logic runs where the game runs its own. What is done and what is still open is in
[engine-roadmap.md](engine-roadmap.md#co-op-sync-for-mod-content) and [sdk-roadmap.md](sdk-roadmap.md) (phase 6).

Every session folder gets `coop-diagnostics.log` (`native/mod-loader/coop_diag.c`): what this machine's game sees of
content mods added, each line with the co-op role, so two players' files can be laid side by side. It records added
stratagems' beacons (on the thrower's machine) with their package resident or not; added entities' spawns with network
id and authority; explosions for 45 s after either (a line a second per type); packages at mission start and 20 s in;
the behaviour co-op checks (the Pelican); the silo's countdown start and end, laser points, launches, and the missile
each second for 15 s after; Orbital Plasma Barrage blocks; changes of the game's host; and mission start and end with
the list of added content. The game's host can change mid-mission, while HellForge's host-only parts follow the lobby.

### GameGuard

GameGuard, the anti-cheat the supported build shipped with, does not run in a session, because the session turns it
off. While the copied game starts, the session runner pauses it at an early point and changes its exe in memory (never
on disk) so that it skips the static constructor that starts GameGuard and the three checks of GameGuard's status that
would otherwise end the game: one at start-up and two later (`src/HellForge.Session/EarlyConstructor.cs`, applied as
`SkipConstructorAndMainCheck` in `PlaySession.cs`). Each place is checked byte for byte before it is changed and read
back after. On a mismatch the change is undone, or the game is ended, and the session stops. HellForge no longer copies
GameGuard's files into an installation. The copy is also blocked from the internet, and HELLDIVERS 2 builds after the
one HellForge runs (25480438) use Easy Anti-Cheat instead.

### Tests

`tests/HellForge.LocalBackend.Tests` holds the multiplayer, stats sharing, asset sharing, mod sync, rejoin and directory
checks (`MultiplayerTests.cs`, `StatsShareTests.cs`, `AssetShareTests.cs`, `ModSyncTests.cs`, `ModSyncSpeedTests.cs`,
`RejoinTests.cs`, `DirectoryTests.cs`);
`tests/HellForge.Tests` has Core's mod sync checks (`ModSyncTests.cs`). `HellForge.SdkShimGen smoke-lobby-link` runs
two backend processes over loopback UDP and two copies of each stand-in through the PlayFab C APIs: create, join,
member data, Party connect, endpoints, a 1,200-byte message and a broadcast, an invitation, a kick, and leaving. Last,
the friend comes back as after a mod sync: a new backend started with `--rejoin` and a new copy of the lobby stand-in,
whose game receives the host's invitation once it has its own lobby, and joins with it.
`HellForge.SdkShimGen smoke-lobby-search` runs two backends sharing a folder directory and two lobby stand-ins, as
friends and again as strangers: the friend's search finds the host's lobby, joins it, and stops finding it after the
host leaves.

## History: research and build log (2026-10-14 to 2026-10-23)

This section is the record of how co-op was researched and built. It describes the game and HellForge as they were at
each date, and some of it is out of date: where it differs from the sections above, the sections above describe the
current code. Notes marked "Now:" point out the main changes.

The research was static analysis of build F5FEE03DCFDB: our own snapshots of the game, HD2Runtime's research notes
(matchmaking safety, peer messaging, PlayFab lobbies; not published) and Microsoft's public SDK headers. Nothing in it
was observed live unless marked.

### How vanilla HD2 does co-op

There are three layers, all in the engine (`helldivers2.exe`, stingray `runtime/playfab/*`). game.dll drives them
through the engine API table T = `[[game.dll+0x3326308]+0xF8]` (exe 0x27CDE70, about 96 slots).

1. The PlayFab web API (`playfab_web_api.cpp`). The base URL is `https://<title>.playfabapi.com`, with title ids from
   `settings.ini` `playfab.title_ids` (LIVE `974A9`). The base URL is formatted into a 0x80-byte heap buffer (web object
   +0x90; init exe 0x8C4127), and every request is `"%s/<call>"` on it. The calls: `Client/LoginWithSteam` (with a
   genuine Steam session ticket) and the other `LoginWith*`/`Link*` variants, `Client/GetPlayerCombinedInfo`,
   `Client/GetPlayFabIDsFromSteamIDs`, `Profile/GetTitlePlayersFromMasterPlayerAccountIds`, `Profile/GetProfile(s)`,
   `Object/SetObjects` (each player publishes `testament_backing_platform`, `testament_platform_user_name` and
   `testament_platform_peer_id`), `Client/GetFriendsList`, `Client/UpdateUserTitleDisplayName` and
   `Client/ReportPlayer`. The login result (system +0x28, T slot 1) is what the backend-enable gate's platform service
   checks, which is why offline sessions need `-ClearPlatformMatchmaking`. The title player account id (system +0xB0)
   is the game's peer id, and the user's vanilla saves are named after it.
2. PlayFab Lobby (`PlayFabMultiplayerWin.dll` 1.8.7) and PlayFab Party (`PartyWin.dll` 1.10.16). Both DLLs are
   byte-identical to Microsoft's public SDK releases. The exe imports 22 `PFLobby*`/`PFMultiplayer*` and 157 `Party*`
   functions (recovered by emulating the protector's import stubs on a snapshot). Lobbies hold the session: members,
   properties keyed `"0".."26"`, search keys, and `invite_id`/`descriptor` for the Party network. Party carries the game
   traffic (`playfab_transport`) and VOIP over Azure relays.
3. Steam (`SteamMatchMaking009`, `SteamFriends017`, engine `steam_lobby`, `+connect_lobby`): the platform lobby for
   friend joins and the Steam overlay. Each member posts its platform lobby as the PlayFab member property
   `platform_lobby` (`"%d:%llu:%s:%s"`).

### Player keys (research)

Each game logs in (`POST /Account/Login`) with a 32-byte `publicKey`, encoded as text by the engine (T+0x38 slot
0x128). When a player joins, the host's game asks Arrowhead's backend for that player's key with
`GET /Account/Keys?id=<platform account id>`, which returns `{"accountKeys":[{"platformAccountId","key"}]}` (builder
0x12be5a0, parser 0x1042120, stored in a table of 8 by the consumer at 0x12c996b). The host admits the player only when
the key matches the one the player's game shows on the game connection (T+0x38 slot 0x130, check at 0x108f92b). With
no answer the joiner is never admitted and sees "lobby timeout". In the first two-PC test the host's backend answered
501 and the joiner left about 100 ms later. A first version of HellForge's key exchange required 64 hex digits and
dropped every real key, which left the joiner on "synchronizing space time"; keys have been kept verbatim since.

### Social tab (research)

The FRIENDS list is PlayFab `Client/GetFriendsList`: the engine asks `{"ExternalPlatformFriends": "None"}` and reads
`FriendPlayFabId` and `TitleDisplayName`. Online and in-game state and joining come from Arrowhead's lobby presence
service. Each game uploads its lobby with `PUT /lobby` (players, operation, region; the reply is ignored), keeps it
alive with `POST /lobby/<id>/member` every 20 s and drops it with `DELETE /lobby/clear`. The tab asks
`POST /lobby/accounts {"ids":["<account>",...]}` and reads `{"accountLobbies":[{"account","lobbyId"}],"lobbies":[...]}`
(game.dll builders 0x12cbc40/0x12cbdc0, parser 0x1055b50, lobby fields 0x1053f60 = the uploaded fields + `lobbyId`,
`connectionString`). HellForge's first version of `FriendPresence` was written on 2026-10-15, before it was tried in
game.

### The plan (2026-10-14)

Replace the services, never patch the game logic:

- A local PlayFab web API in the backend, with ids derived from the SteamID, and a redirect of the game's PlayFab URL
  (both built as described above).
- Replacement SDK DLLs: HellForge's own `PlayFabMultiplayerWin.dll` and `PartyWin.dll` implementing the public C APIs.
  Lobby state and Party messages go to the local backend over loopback, and the backends relay between friends. The game
  loads the stand-ins through ordinary DLL loading. HD2's two DLLs are byte-identical to Microsoft's public releases
  (Party: NuGet `Microsoft.PlayFab.PlayFabParty.Cpp.Windows` 1.10.16, SHA-256 `DE34667C…`; Multiplayer: GitHub
  `PlayFab/PlayFabMultiplayer` v1.8.7 Windows zip, `95BC5F95…`), so those packages' headers give the exact signatures.
  Both export flat C APIs (157 and 78 functions, undecorated `__stdcall` on x64).
- A modlist gate with joins (built as described above).
- Backend-to-backend transport, planned for later: Steam networking relays between Steam friends (no port forwarding),
  or a VPN such as Tailscale as the simple fallback. Now: Steam networking is built; there is no VPN path.
- Steam (Valve) stays genuine: real sign-in, real friends, real Steam lobbies.

### Step 1: local PlayFab login

Implemented as an opt-in launcher setting, Experiments > Local PlayFab (script `-LocalPlayFab`). Now: it is the
"Local PlayFab" switch under Settings > MULTIPLAYER, on by default (`--local-playfab` in the CLI). What was to be
verified live:

- the redirect outcome;
- which `/playfab/...` calls arrive;
- whether `loggedIn` turns 1 (the observations' `playFab` field);
- whether the session still reaches the ship;
- whether `-ClearPlatformMatchmaking` becomes unnecessary.

With only this step, the original SDK DLLs then try PlayFab's real lobby and relay services, and the network block stops
that.

### Step 2: stub DLLs

`scripts/Build-SdkShims.ps1` downloaded portable Zig 0.17.0 and the two SDK packages (all pinned by SHA-256, kept in the
gitignored `tools/`), then `src/HellForge.SdkShimGen` generated a logged stub for every export from the headers,
skipping the hand-written ones in `native/sdk-shims`. Output went to `artifacts/sdk-shims/out`; the generated sources
repeat Microsoft's signatures and are never committed. The build checks the exports and ordinals against the
originals, allows only Windows system imports, and smoke-tests both DLLs in-process. Now: `scripts/Build-Engine.ps1`
does all of this, with Zig from `scripts/Get-BuildTools.ps1`.

Phase-1 behaviour: `PartyInitialize`/`PFMultiplayerInitialize` hand out a handle, the state-change pumps report
nothing, and the error-message functions name the stand-in. Every other export succeeds when it has no output
parameters and otherwise fails without writing (Party `0x10D0`, Multiplayer `E_NOTIMPL`), so the game never reads an
output we did not write. `HELLFORGE_SHIM_POLICY="Name=ok,Other=fail"` overrides single functions without a rebuild. The
17 Multiplayer `*WithEntityHandle` exports appear in no public header (GDK variants; HD2 uses the entity-key forms) and
are exported without reading arguments.

First live boot (2026-10-14, with Local PlayFab): the protected exe loads both stand-ins. At boot the game calls
`PFMultiplayerSetMemoryCallbacks`, `PFMultiplayerInitialize`, `PFMultiplayerSetEntityToken` (our local PlayFab entity
token), `PFMultiplayerStartListeningForLobbyInvites`, `PartySetMemoryCallbacks`, `PartyInitialize`,
`PartyCreateLocalUser` (entity id = the SteamID-derived title player id) and `PartyGetLocalDevice`, then pumps both
state-change queues every frame. It tolerated the stubs failing those last two, but loading the ship calls
`PartyCreateNewNetwork` (to host), and the game exited with code 1 when that failed synchronously, after
`PFLobbyGetOwner`/`PFLobbyGetLobbyProperty(NULL, "8")`. Sessions with the original DLLs and the network blocked reach
the ship, because the real library accepts the request and reports the failure later. Phase 1.5 therefore matches the
offline library: local users and the local device are real objects, and `PartyCreateNewNetwork` is accepted, then
completed through `CREATE_NEW_NETWORK_COMPLETED` with `INTERNET_CONNECTIVITY_ERROR`.

Second boot: local user and device now succeed (the game then tries `PartyDeviceCreateChatControl`, still a tolerated
failure). At the ship it calls `PartyCreateNewNetwork` and, in the same frame, `PartyConnectToNetwork` with the
descriptor just written (the documented hosting pattern). That call was still a synchronous failure, and the game exited
again. The stand-in now models networks as the docs describe: `PartyConnectToNetwork` returns a network at once;
authentication, endpoints, invitations and leave requests queue on it; and at the next delivery it fails in the
documented order (connect completion, the queued operations' completions and endpoint/invitation destroyed notices,
`NETWORK_DESTROYED`, then a requested leave). Network objects live until `PartyCleanup`, so no handle the game still
holds can dangle.

Third boot: every Party call in the hosting frame succeeded (create, connect, authenticate, `ConnectChatControl` with a
null control, create endpoint), and the game then crashed in the same frame. The engine's crash handler left a minidump
(`%APPDATA%/Arrowhead/HellForge/dumps`): an access violation reading 0x8 at game.dll+0x1092908, the `strlen` of a lobby
property (`PFLobbyGetLobbyProperty(NULL, "8")`; the game reads keys "0".."26" from an engine lobby that has no PlayFab
lobby yet). Its engine wrapper (exe 0x8c6f00, slot T+0x110) returns the SDK's output without initializing or checking
it, and the real SDK writes `*value = NULL` before validating. The stub failed without writing, so stack garbage reached
`strlen`. (The `owner()` wrapper, 0x8c6ce0, initializes and null-checks its output.) All stubs now clear their
write-only (`_Out*`) parameters whatever the result, as the real libraries do; in-out parameters are left alone.
game.dll and the exe are WinLicense-packed on disk; the unpacked images used for this research match this build
(verified against the dump's code bytes).

At the time this ran with launcher Experiments > PlayFab SDK stand-ins or script `-SdkShims` (best with Local
PlayFab). Now: it is the "Lobby and session through Steam friends" switch, on by default (`--multiplayer` in the CLI).
For the session only, the copy's two DLLs are swapped for the stand-ins after the copy is verified; the originals are
restored afterwards, or before verification at the next start if a session is cut short. Each call is logged to
`<run>/sdk-shims/<dll>-<pid>.log` (the first 32 calls per function, then every power of two). The live boot was to
answer whether the protected exe accepts the stand-ins, which calls it makes at start-up and in the social menu, and how
it reacts to each failure.

Fourth boot: the ship loads. The game handles the failed host like the offline original (error messages, endpoint and
network queries), cleans up about 20 s later (`PartyNetworkDestroyEndpoint`, `LeaveNetwork`) and hosts again. The ship
UI polls lobby properties "5", "14" and "19" every frame. It never creates a PlayFab lobby: that waits for a working
Party network.

### Step 3: solo lobby and Party (2026-10-14)

The stand-ins now played the services for this machine, still entirely in the DLLs:

- Party: a network created here can be connected to. The game sees `REGIONS_CHANGED` (it passes no regions),
  `CREATE_NEW_NETWORK_COMPLETED`, `CONNECT_TO_NETWORK_COMPLETED`, `NETWORK_CONFIGURATION_MADE_AVAILABLE`,
  `INVITATION_CREATED` (the initial invitation, which the host authenticates with),
  `AUTHENTICATE_LOCAL_USER_COMPLETED`, `ENDPOINT_CREATED` and `CREATE_ENDPOINT_COMPLETED`, all succeeded. Leaving
  destroys endpoints, invitations and the network before `LEAVE_NETWORK_COMPLETED`. Descriptors serialize as
  `hellforge1;<network id>;<region>;<connection information hex>`; the connection information is
  `hellforge-host:<entity id>`, so a descriptor names its host. A network created on another machine still failed to
  connect.
- Lobby: `PFMultiplayerCreateAndJoinLobby` creates a lobby owned by the creator (`MemberAdded`, then the completion).
  Owner, members, lobby/search/member properties and their keys, policies and custom contexts read back;
  `PFLobbyPostUpdate` applies at the next delivery (`PostUpdateCompleted`, then `Updated`); leaving delivers
  `MemberRemoved`, `LeaveLobbyCompleted`, `Disconnecting`, `Disconnected`. Text given to the game stays valid until
  `PFMultiplayerUninitialize`. Connection strings were `hellforge1-lobby;<lobby id>;<owner entity id>`. Joining another
  machine's lobby failed with 0x89236226 ("lobby doesn't exist"); invitations complete with `E_NOTIMPL`; lobby searches
  find nothing.

The lobby lived in the DLL at first so the game's call sequence could be learned quickly. Joins moved it behind the
backend, which a friend's join request reaches.

### Step 4: two machines (2026-10-14)

With the stand-ins on (the default in installations), the session starts the backend with the named pipe
`hellforge-<run id>` (`HELLFORGE_PIPE`) and a Steam peer link:

- `Multiplayer/SteamPeerTransport.cs`: ISteamNetworkingMessages in the backend process (not firewalled), beside the
  running game (checked with a probe program); sessions accepted from Steam friends only.
- `LobbyService.cs`: the creator's backend owns each lobby; the lobby stand-in mirrors it from events. Joins name the
  host through the connection string `hellforge1-lobby;<id>;<host SteamID hex>`; a joiner must join as themselves and
  run the same gameplay mods (the modlist code, [mods.md](mods.md)). A joiner's backend adopts the host's war time.
- `PartyService.cs`: the host's backend owns each Party network, admits only its lobby members, assigns friends'
  endpoint ids (1000+, now blocks of 64 from 4096) and routes the game's messages (guaranteed delivery = reliable Steam
  messages).
- Peers sharing a lobby or network ping every 2 s; one silent for 15 s is treated as gone (members leave, its lobbies
  and networks end).

It was verified on one PC: 248 backend checks, and `HellForge.SdkShimGen smoke-lobby-link`. Not yet seen at the time:
the game's own join path on a second PC (Steam "Join game" / invite -> Steam lobby -> PFMultiplayerJoinLobby), which
the first two-PC session logs were to show.

### Step 5: stats in co-op, the host's values everywhere (2026-10-17)

Built 2026-10-17 and tested on one PC at first. The protocol was then `hellforge-mp-5` (now `hellforge-mp-6`). At the
time, fields that apply only from a session start (attachments' entity deltas, status multipliers: each machine kept
its own, and the backend logged how many), the editor's arm swaps and armor perks (`editor-settings.json`), and the
launcher's stats editor in co-op sessions were not synced. Now: arm swaps, hellpod payloads and the editor's mod content
values are synced, and attachments and status multipliers go over the pipe; see [Stats in co-op](#stats-in-co-op).

The two-PC test written for it (the user and a friend, both on the installer):

1. Both start a session with the multiplayer switch on. The host changes a field in the in-game editor (say the
   Liberator's fire rate) and presses APPLY; the friend has different saved changes of their own.
2. The friend joins the host's lobby. Check: the join is not refused for "different gameplay mods"; the friend's
   editor turns grey with "READ-ONLY · HOST: <host's Steam name>"; the friend's Liberator fires at the host's rate
   after a respawn; a field only the friend had changed shows its default.
3. The host changes another field and presses APPLY: within a couple of seconds the friend's editor shows it (and it
   applies in the friend's game as the field's timing says).
4. The friend tries to type a value, APPLY, RESET ITEM and a preset IMPORT: each is refused with a message.
5. The host leaves (or closes the game): the friend's editor is editable again and their own values are back.
6. Collect both sessions' logs (launcher Console > "Save logs to Desktop"): `multiplayer.log` ("Stats:" lines) and
   `modloader.log` / `mod-zz.local.stats.log` ("session:", "live set") on each side.

### Step 6: mod sync, a joiner takes the host's mods (2026-10-22)

Built 2026-10-22. On two PCs a refusal and its offer are confirmed; the download and restart are not yet. Players see
it in [Playing with other mods](mods.md). Protocol `hellforge-mp-7`.

- Each session writes its mod set (`<run>/modset.json`, from `mods-stage --modset`): every gameplay mod with its
  content hash, class (content, code, raw, bundled), load-order place, folder and counted files. The backend reads
  it with `--modset`.
- **Host** (`Multiplayer/ModSync.cs`):
  - A joiner refused for different gameplay mods is first sent the set without its folders:
    `mods.offer [lobby id, set JSON, sharing 0|1]`. Then comes the refusal, as before: `lobby.refused [reason,
    line...]`, its lines shown by both games. When the host shares, the joiner's last line says the MOD SYNC window
    offers the download. A backend from before ignores the offer.
  - A joiner takes an offer only for a join it asked for in the last minute.
  - The joiner's machine (a stranger too: step 7's gate lets a download through) may then download for 10 minutes from
    its first refused join (a later one extends nothing), at most the set's bytes twice over and 16 chunks:
    - `mods.files [req, id, hash, start]` → `mods.filelist [req, ok, total, start, (path, size, sha256)...]`, pages
      of at most 1000 files (a frame holds 4096 fields), at most 10000 files a mod on both sides;
    - `mods.get [req, id, hash, path, offset, length ≤ 128 KiB]` → `mods.chunk [req, ok, offset, bytes]`;
    - or `[req, refused, reason]`.
  - It serves only a mod in its set (not a bundled one), only a file that mod counts, and only within the file's size;
    a code or raw mod only to a Steam friend.
  - One worker reads the files, at most 24 queued requests per machine (8 before 2026-10-23), paced to 8 MiB/s per
    machine and 16 MiB/s in total (1 and 2 MiB/s before; [Mod sync speed](#mod-sync-speed)). A request that fails in any way is refused or logged; the worker serves the next one.
  - `"share": false` in `hellforge-mods.json` stops it. Sharing is on by default (a maintainer's call still open).
- **Joiner:**
  - The offer becomes the game's (`ModSyncPlan` in HellForge.Core). The loader's live pipe gets
    `syncmod` lines and then `syncstate` (`native/mod-loader/live.c`, SDK 14 `sync_state`/`sync_mod`), and the in-game
    editor shows its MOD SYNC prompt over the game, the editor window closed. The answer comes back as
  `syncanswer <version> <answer> [<mod id>]` (accept, decline, restart, block, trust, open). Each `syncmod` line ends
  with the mod's `url` or `-`; the loader keeps only an `https://` address of printable ASCII.
  - A Steam friend's code or raw mod that is not verified is offered for trust (`trust` in the plan, the offer
    `blocked` until none waits). `trust <mod id>` is taken only for such a mod of the current offer: its id and hash go
    on the player's trusted list with the host's name and SteamID64, and the offer is made again; when nothing else
    waits, its download starts at once. A stranger's are `manual` whatever the player trusted.
  - `open <mod id>` opens the mod's `url` from the host's set in the default browser (at most once a second), after
    `ModCatalog.WebAddress` checks it again; nothing else is ever started.
  - The offer is also written to `<run>/backend/mod-sync.json`.
  - On accept it checks the drive has room (the download and 256 MB), then downloads, the chunks asked for on across files, 2 to 16 in flight
    ([Mod sync speed](#mod-sync-speed); before 2026-10-23 four, one file at a time). The
    file list, the manifest (`hellforge-mod.json` before `bts-mod.json`), the stats file and the data spec come
    first: a mod they show to be code or raw data (`ModSets.Declared`) that is not on the verified list stops there.
  - Each file is checked against its listed size and SHA-256, and the whole mod by `SyncedStore.Install`, before it
    goes to `mods/.synced/`. Each attempt writes into its own `.synced/.partial/<id>@<hash>-<attempt>/`, deleted
    whatever goes wrong; anything that goes wrong ends in `failed` with its message. A stored mod is found again only
    while its files rehash to its content hash.
  - The editor's prompt takes answers (buttons, Enter, B) one second after it appears or its state turns.
  - The loader holds offers of up to 512 mods (`live.c`); the backend lists the ones that need something first.
  - On restart it writes `<install>/temp/mod-sync/session.json` (`ModSyncSession`) and `mod-sync` into
    `stop-observation.signal`. The session runner reports `restart: mod-sync`, and the launcher starts the next
    session with `--mod-sync <file>` (`mods-stage --sync`).
  - That session plays the host's mods, each found by id and content hash, in the host's order. If they do not make
    the host's modlist digest, it plays the player's own mods and reports why.
  - With the in-game editor switched off, the launcher shows the offer from `mod-sync.json` on its Play page
    (`ModSyncPrompt.cs`, `ModSyncOffer`), TRUST and GET IT included, and writes the answer to
    `<run>/backend/mod-sync-answer.json`, which the backend takes within a second, as the game's `syncanswer`.
- Rejoining by itself after the restart is built ([Step 9](#step-9-rejoining-after-a-mod-sync-2026-10-23)).

**To test on two PCs:**
1. The host enables a content mod (a data patch or stats mod) and a code mod that is not on the verified list, with a
   `"url": "https://..."` in its manifest; the friend has neither.
2. The friend joins. Check that the MOD SYNC prompt shows (no editor window behind it), lists the content mod as
   downloading and the code mod as "needs your trust" with TRUST and GET IT buttons, and refuses to start (OK only).
3. GET IT (or G): the mod's page opens in the friend's browser; the prompt stays.
4. TRUST (or T): the warning shows in the prompt's place; Enter does nothing, the button is grey for a second, CANCEL
   and Esc go back to the list. Then TRUST AND DOWNLOAD:
   - check the download starts at once, the progress bar, and that the files land in `mods/.synced`;
   - check that the session restarts with "Playing with <host>'s mods" in the launcher;
   - check that the restarted game is invited back into the host's lobby by itself (step 9's test);
   - check that the launcher's Mods page (MOD SYNC card) lists the code mod as trusted from the host, and that
     Untrust removes it and its `mods/.synced` copy.
5. Time how long a large download takes (`multiplayer.log`: "downloading N MB" to "every mod is here"; both backends
   log "Steam link: send rate 8 MiB/s" at start), and whether the host notices it in game.
6. BLOCK a mod from the dialog: the next join to that lobby shows it blocked; unblock it on the Mods page.
7. With `HELLFORGE_TEST_STRANGERS=1` on the friend's PC (the host then counts as a stranger, in a Public lobby): the
   code mod is "install it yourself", with GET IT and no TRUST, even after it was trusted.
8. Collect both sessions' `multiplayer.log` ("Mod sync:" lines), `backend/mod-sync.json` and `modloader.log`
   ("mod sync" lines).

### Step 7: public lobbies on the Galactic Map (2026-10-22)

Built 2026-10-22 and tested on one PC only.

- **The directory** (`Multiplayer/LobbyDirectory.cs`):
  - Every backend lists the public lobby its game hosts on Steam matchmaking (app 553850), as an Invisible Steam lobby
    the backend holds beside the game's own (`SteamLobbyDirectory`, on the peer link's Steam thread).
  - Listed means: privacy `number_key6` 0, not locked, not full. A listing appears and goes at once; changes are
    written at most every 5 s (`DirectoryService`). Since 2026-10-23 the player's `"listPublic"` switch must be on as
    well, and `Listing` holds only while Steam holds the listing lobby: a failed `CreateLobby` is retried after 5 s,
    doubling up to a minute, and a lobby Steam no longer has this machine own is made again (its owner is read every
    10 s).
  - A listing's data:
    - `hellforge` (the protocol);
    - `hf.connection`, `hf.lobbyId`, `hf.host` (SteamID64 in decimal);
    - `hf.members`, `hf.maxMembers`, `hf.slots`, `hf.lock`;
    - every PlayFab search property under its own name (`string_keyN`, `number_keyN`);
    - `hf.lobby` (the game's `PUT /lobby` record, gzip and base64; long values split into parts) and `hf.live`;
    - `hf.mods` (the mod set), `hf.digest`, `hf.share`;
    - `hf.stats` (the host plays with edited stats).
  - **Published record (decided 2026-10-22):** player names are kept, and every account id except the host's becomes
    0.
- **The search:**
  - The lobby stand-in sends `PFMultiplayerFindLobbies` to the backend: `lobby.find [op, entity, filter, sort, count]`.
    It logs the game's filter string when it changes.
  - The backend asks the directory with coarse filters: the protocol, the filter's `eq` clauses, a slot free,
    worldwide, at most 50. It then evaluates the game's whole filter (`LobbyFilter`: `and` of `eq ne gt lt ge le`).
    Two clauses are left out on purpose: `number_key7`'s random window and the continent exclusions
    (`string_key6 ne`).
  - It then drops lobbies the player could not join (`ModSync.Joinable`): a blocked mod, mods to install by hand, or a
    host with edited stats unless `"showEditedStats"`. It also drops its own lobby.
  - It answers `lobby.found [op, hresult, count, (lobby id, connection string, owner, max, members, locked, search
    properties)...]`, which the stand-in turns into `FindLobbiesCompleted` with `PFLobbySearchResult`s. The last four
    searches' results stay valid after their batch.
  - The game's two searches per scan (about 30 ms apart) share one directory search; nothing older than 500 ms is
    reused.
- **Listings' hosts** (since 2026-10-23). Steam does not say who made a lobby to someone who
  is not in it (`GetLobbyOwner` gave 0 for all 50 of the app's public lobbies in a search, 2026-10-23), so a listing's
  host is its own `hf.host`: anyone can list under another player's id, with that
  player's lobby id too (the game's own Steam lobby shows it). What holds anyway:
  - a join goes to the host a listing names, over Steam's authenticated link, and that host refuses a lobby it does not
    hold (`LobbyNotFound`); whoever made the listing hears nothing;
  - listings that claim one host or one lobby with different data are all left out, except one a friend's backend
    vouches for against ones it does not; copies of the same data count once (`DirectoryService.Unambiguous`). The
    cost: anyone can hide a listed lobby from strangers by copying it with other data;
  - a backend vouches for its listing by sending friends its digest (`DirectoryData.Digest`, SHA-256 of the data) as
    the fifth field of `presence` (`FriendPresence.Vouches`; the last two digests count). With Play with strangers off,
    a friend's listing is shown only while that friend vouches for it.
- **The map's records:** `POST /lobby/accounts` answers, after friends, for the hosts the last searches found (for 2
    minutes), with the listing's record, its lobby id and connection string. `GET /lobby/<id>` and
    `GET /lobby/<id>/live` answer from the local, a friend's or a found lobby. `PUT /lobby/<id>/live` is stored and
    listed. The game has not been seen calling the GET routes; an unknown lobby is a 404 (`lobby_unknown`).
- **Strangers** (`StrangerGate`), only with [Play with strangers](#play-with-strangers) on:
  - The Steam link accepts a session from someone who is not a Steam friend only while this machine lists a lobby, or
    when it deals with them: a member of a lobby hosted here, the host of a lobby joined or being joined from here, or
    a mod download.
  - From a stranger, a backend takes:
    - `lobby.join`: only with a lobby listed, from a machine it deals with, or from one a lobby here expects back. The
      limits are at most 6 a minute from one machine and 60 in all, and since 2026-10-23 they hold for a stranger it
      already deals with too;
    - pings;
    - then what follows once it deals with them.
  - Presence (the social tab) and invitations stay between friends. A stranger host's name comes from the record its
    listing carried.
  - The gate's tables forget a machine after a minute of silence.
- **Multiplayer needs Local PlayFab.** The session runner and the launcher refuse `--multiplayer` without it: the
  platform matchmaking selector that sessions clear without Local PlayFab sends the game's own search to Steam's
  lobbies, where vanilla games list theirs under the same build string.
- **Tests on one PC:**
  - `HellForge.LocalBackend.Tests` (directory: filter, values, publishing, search, records, edited stats, blocked and
    unjoinable mods, strangers);
  - `HellForge.SdkShimGen smoke-lobby-search <stand-ins> <HellForge.LocalBackend.dll>`: two backends over loopback UDP
    sharing a folder directory (`--directory`), and two lobby stand-ins through the PlayFab C API. The friend's
    search finds the host's lobby about 0.35 s after it is listed, joins with the connection string it found, and
    stops finding it about 0.35 s after the host leaves. Both backends get a `--mods` folder with Play with strangers
    on (off, nothing would be listed). It runs twice: as friends, then as strangers
    (`HELLFORGE_TEST_STRANGERS`, below). As strangers the join passes the gate, no presence is swapped, and the map's
    record for the host is the listing's (names kept, other account ids 0).
- **Test mode for two friends' PCs: `HELLFORGE_TEST_STRANGERS=1`.** A backend started with it treats every Steam
  friend as a stranger:
  - no presence, so the social tab lists none;
  - the stranger gate applies;
  - Steam sessions are accepted only by the directory's rule.

  `multiplayer.log` says "Test mode (HELLFORGE_TEST_STRANGERS)". Play with strangers must be on as well on both PCs
  (it is off by default, and test mode does not turn it on). Set the variable on both PCs before starting the launcher
  (the session runner and the backend inherit it), for example in Command Prompt:
  `set HELLFORGE_TEST_STRANGERS=1` then `start "" "<installation>\HellForge.exe"`. Steam itself still sees two friends;
  nothing in the directory depends on that.
- **The map, on one PC (2026-10-24, two installations, [multi-instance.md](multi-instance.md)):**
  - **The marker.** A lobby whose host is in a mission shows as a squad marker with the hover card: host name, level,
    mission and objective. A lobby with no mission selected shows none; the marker needs the record's `seed` (above).
    A host on its ship with a mission selected uploads a seed too, but that was not tried. A host with no mission selected also lists difficulty 0 (`number_key5`), which the map's search (the
    map's own difficulty window) leaves out as "the game's filter".
  - **Friends.** A planet scan leaves out lobbies of the player's Steam friends: they show under Social instead. So
    two friends test the map with `HELLFORGE_TEST_STRANGERS=1`.
  - **Joining.** Selecting the marker joins: the loadout screen of the host's mission comes up within seconds. After
    Ready Up the joiner drops into the mission in progress (hellpod, "Allied Destroyer joining squadron"), and the host
    sees it. Ready Up does nothing while all four stratagem slots are empty (the loadout screen's own rule).
- **Not yet known:**
  - whether the hover card works with the other members' account ids at 0 (only solo hosts so far);
  - the card's elapsed time on the surface: it reads 0m 0s, because no host has uploaded `PUT /lobby/<id>/live`.
- **Steam's search rate (soak, 2026-10-23, one PC, live Steam):** a search shaped like the directory's (the protocol
  key, a slot free, worldwide, at most 50) for one Invisible lobby of the soak's own. Steam failed none and always
  found that lobby:
  - 60 searches at the map's rate (every 10 s, 10 minutes), half of them in 175 ms or less, the slowest 231 ms;
  - 60 at one a second;
  - 359 back to back in a minute (358 a minute, 60 times the map's rate), the slowest 192 ms.

  No limit was reached. A new lobby was found 0.84 s after it was made.

**To test on two PCs that are not Steam friends** (or two friends' PCs with `HELLFORGE_TEST_STRANGERS=1` on both):
1. Both turn on multiplayer, Local PlayFab and Play with strangers (Settings > MULTIPLAYER). The host opens a Public
   lobby, selects a mission on a planet and deploys. An idle host on the ground can be killed (one was, about 11
   minutes in), and with no one alive the mission fails. `multiplayer.log` says "Play with strangers is on".
2. The other player opens the Galactic Map on that planet, at the host's difficulty. Check:
   - a squad marker ("1/4") and a hover card with the host's name and mission;
   - `multiplayer.log` "Directory: search ... shown", with the reasons for any lobby left out;
   - the stand-in log's `PFMultiplayerFindLobbies: filter "..."`.
3. Select the marker, pick stratagems, Ready Up. Check:
   - the join is admitted (the host's log has no "Dropped ... not a Steam friend");
   - the joiner drops into the mission, and the mission plays.
   Then with different mods: the MOD SYNC prompt shows.
4. The host turns Invite Only, or the lobby fills: within a scan or two the marker goes. The other player, already in
   the lobby, stays; a new one is refused ("The host's lobby is Invite only").
5. The host edits a stat (F8, APPLY): the lobby disappears from the other player's map until "Show public lobbies
   whose host plays with edited stats" is on.
6. The host turns Play with strangers off: within a few seconds the listing goes ("not listed: Play with strangers is
   off"). The other player turns it off: the map shows no stranger's lobby, and a join already on screen is refused
   with "Couldn't join".
7. The game's own Steam lobby ([steam-bridge.md](steam-bridge.md#the-games-own-steam-lobby)). Both PCs sandboxed
   (`--sandbox`), Play with strangers on, and on the host `HELLFORGE_STEAM_LOBBY_FRIENDS_ONLY=1`, which makes the
   broker keep the game's Public Steam lobby Friends only anyway (`steam-broker.log`: "Steam gets 1"). The other
   player joins from the map. If the join and the mission work, the broker can keep the lobby Friends only with
   strangers on too.

### Step 8: privacy, invitations and kicks (2026-10-23)

Built 2026-10-23 and tested on one PC only (`HellForge.LocalBackend.Tests`: privacy against friends, strangers and
invited players, kicks, invitations, hostile listings, the listing switch; `smoke-lobby-link` with an invitation and a
kick through both stand-ins). What it does is in [Lobby privacy, invitations and kicks](#lobby-privacy-invitations-and-kicks).

**To test on two PCs** (A hosts, B joins; for the stranger cases set `HELLFORGE_TEST_STRANGERS=1` on both):
1. A sets Privacy to Friends only. B (a Steam friend) joins from the social tab: admitted. With the test variable set on
   both, B is refused with "The host's lobby is Friends only".
2. A sets Invite only. B joins from the social tab: refused, and A's game shows "<B> couldn't join". A invites B from the
   social menu: B's game shows the invitation, and B joins.
3. A sets Public with B in the lobby, then Friends only: B stays, and the map marker goes on a third player's map.
4. A kicks B from the social menu. Check that:
   - B's game leaves the lobby and shows "Removed from <A>'s lobby";
   - A's `multiplayer.log` has "Kicked ..." and "Removed a kicked machine from Party network ...";
   - B's join back is refused with "The host removed you from this lobby".
   Note which call the game made: `PFLobbyForceRemoveMember` in A's `PlayFabMultiplayerWin.dll` log, or
   `PartyNetworkKick*` in A's `PartyWin.dll` log.
5. A turns off "List my Public lobbies ..." and hosts a Public lobby: it is not on B's map (as a stranger), and A's log
   says "Directory: this player's public lobbies are not listed".
6. Collect both sessions' logs (launcher Console > "Save logs to Desktop").

### Step 9: rejoining after a mod sync (2026-10-23)

Built 2026-10-23 and tested on one PC only (`RejoinTests.cs`; `smoke-lobby-link` through the real lobby stand-in). What
it does is in [Mod sync](#mod-sync). The research was static, on the supported build's unpacked `helldivers2.exe` and
`game.dll` images (the research repo's snapshots), and on the stand-in and backend logs of earlier sessions.

**How the game joins a lobby, as far as known:**
- **Invitations (PlayFab).** Every session calls `PFMultiplayerStartListeningForLobbyInvites` at boot, about 9 s in.
  The engine logs a received invitation: `Lobby: InviteReceived (sender=%s, receiver=%s, connection_string=%s)` (exe
  strings beside the other lobby state changes). game.dll has the input actions `InvitePopupAccept`, `InvitePopupDecline`
  and `InvitePopupModifier`, so an invitation is most likely a popup the player accepts. No session log has an
  `InviteReceived` yet, because HellForge delivers invitations only since step 8.
- **Steam.** The engine's Steam friends module (`stingray/runtime/steam/steam_friends.cpp`) reads `+connect_lobby <id>`
  and `+connect <address>` from the command line ("Invalid command line connect lobby `%s`"). Its script API has
  `boot_invite` and `next_invite`, which return `INVITE_LOBBY`, `INVITE_SERVER` or `NO_INVITE`. game.dll's title screen
  (`state_title_screen.cpp`) names `friends_handle_boot_invites`, and there is a deep-link state (`StateDeepLink`,
  `deeplink_temporary_hosting`, `ship_teleporter_deeplink_join`, `TimedOut_CantJoinLobby`). A game started with a
  Steam lobby id therefore joins through a path of its own, by way of the title screen and the ship. That path was not
  decoded. The title screen's update is game.dll 0xAD7CB0 and its deep-link step 0xAD8760.
- **What the logs show.** A joining game posts the host's Steam lobby as its own `platform_lobby` after the PlayFab join.
  The order seen is PlayFab first, then Steam; whether a Steam lobby leads the game to a PlayFab lobby is not known. The
  social tab's join is `/lobby/accounts` and then `PFMultiplayerJoinLobby` with the connection string. A game creates its
  own lobby on reaching the ship, 28 to 34 s after the stand-in loads in sessions that reached the ship. One host log
  shows `PFMultiplayerJoinLobby` repeated on a closed lobby's connection string every 20 s. Whether the game or the
  player did that is not known.

**The path chosen: an invitation from the host, handed to the game by its own backend.**
- It is the game's own path, and the stand-in already delivers it for friends' invitations. No game code or data
  changes.
- It reaches strangers too: the rejoining machine's own backend hands the invitation to its own game, after the host
  answered.
- It waits for a state the backend can see: the game's own lobby, which it creates on the ship.
- The Steam boot invite (`+connect_lobby`) was set aside. It would need the host's Steam lobby id, the game would join a
  real Steam lobby of possibly another privacy (a stranger may not get in), and how the game goes from a Steam lobby to
  the PlayFab lobby is not known. It remains a candidate if the invitation proves to need more than one press.

**Not yet known (the two-PC test answers it):**
- whether the game shows a popup for the invitation or joins at once, and whether it names the host (a stranger host
  too);
- whether an invitation 5 s after the game's lobby is made comes too early for the ship's screens.

**To test on two PCs** (A hosts, B joins; for the stranger case set `HELLFORGE_TEST_STRANGERS=1` on both and use a
Public lobby; B turns on the launcher's engine log for the last step):
1. A enables a content mod that B lacks, hosts a Friends only lobby, and keeps sharing on. B joins from the social tab:
   the MOD SYNC prompt shows. B chooses DOWNLOAD AND RESTART, then RESTART TO JOIN.
2. Before B is back, A sets Privacy to Invite only.
3. B's launcher says "Once the game is on the ship, the host's invitation to their lobby waits in game." Check that
   B's `result.json` has `"rejoin": true`.
4. On B's ship, check that:
   - the in-game editor shows "Rejoining <A>";
   - the game shows A's invitation. Note whether it is a popup, whether it names A, and whether it joins without a
     press;
   - accepting it puts B in A's lobby, although the lobby is now Invite only;
   - B's `multiplayer.log` reads "Rejoin: ... on the ship", "asking", "lets this player back", then "back in <A>'s
     lobby", and A's reads "may rejoin lobby ... after a mod sync".
5. Repeat, each time with a different change before B is back. Each should give one "Couldn't rejoin <A>" notice with
   the reason, and no retries:
   - A leaves the lobby without hosting another;
   - A fills the lobby;
   - A closes the game ("did not answer", after about 10 s).
6. Repeat with A leaving and hosting a new lobby. Check that B is invited into the new one ("their lobby now").
7. B leaves the invitation alone for 2 minutes. Check that nothing more happens. Then check that a session B starts
   with PLAY has no "Rejoin:" lines.
8. Collect both sessions' logs:
   - `multiplayer.log` ("Rejoin:" lines on B, "rejoin" lines on A);
   - B's `PlayFabMultiplayerWin.dll` log ("invited to lobby", `InviteReceived`, `PFMultiplayerJoinLobby`);
   - B's engine log (`Lobby: InviteReceived`).

### Step 10: every player's own stats, and the lobby's assets (2026-10-23)

Built 2026-10-23 and tested on one PC only: `HellForge.LocalBackend.Tests` (`StatsShareTests`: shared, not shared,
shared again; `AssetShareTests`: a host and two joiners), the loader's fixture (`stats_test.c`: `preload`, the 16 kept
for this machine's own, the `packages` line, `session ... stats=`) and the editor's overlay test (read-only only while
playing with the host's stats). What it does is in [Stats in co-op](#stats-in-co-op) and
[Assets in co-op](#assets-in-co-op). HD2Runtime's own synced asset loading (its `hd2as` lobby key,
`docs/asset-loading.md`) was never proven live; this is HellForge's over its backends' channel.

**To test on two PCs** (A hosts, B joins; both with the "Everyone plays with my editor changes" switch off, the default):
1. B swaps a vehicle's weapon in the in-game editor (Mounted Weapons) to one A's loadout never brings, and APPLY. A
   swaps a hellpod's payload the same way. Check that each editor stays editable, with "JOINED · <A> · YOUR OWN STATS"
   on B's and "HOSTING · EVERYONE KEEPS THEIR OWN" on A's.
2. Check `modloader.log` on each: "lobby assets: ... package(s) for friends' changes" with the other's packages, and
   "package ... requested" for them. `multiplayer.log` has "Assets:" lines on both.
3. Start a mission. B calls in the swapped vehicle, and A the swapped pod. Check that each sees the other's swapped
   weapon and pickup, not a purple question mark. Note what each player sees of the other's weapon firing and what it
   hits.
4. B changes a hand-held weapon's projectile (say the AR/GL-21 One-Two's to the PLAS-1 Scorcher's) and fires it. With
   the per-player shots (shots.c) A sees B's swapped round too, and A's own One-Two still fires its own. Note whether
   enemies A sees die as B sees them die. Then the same with a Liberator (its round comes from its ammunition), the
   One-Two's grenade launcher (its own weapon, fed by rounds) and a Recoilless Rifle's programmable round.
5. A turns the switch on: B's editor turns read-only ("READ-ONLY · HOST: <A>") and B plays with A's stats. A turns it
   off: B's own come back.
6. Collect both sessions' logs (launcher Console > "Save logs to Desktop").

### Co-op diagnostics (engine 0.1.19, the 0.1.11 diagnostics beta)

`coop-diagnostics.log` was added for the two-PC test of 2026-10-21 (0.1.8), which left these open: a friend's Pelican
never ran its behaviours on the others' machines; a silo launch did not carry over (the other machine's missile stayed
or vanished, with no flight or blast), so the silo launches on every machine again; a host's Orbital Plasma Barrage was
unseen by the friend, and the friend's landing showed on the host's previous beacon on the host's screen; and a host's
custom support weapon's shots were unseen by the friend.
