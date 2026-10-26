# Co-op: Steam friends, or the local network

This page explains how HellForge's co-op works, with Steam friends or over the local network: first for players, then
for contributors working on it. The second half is the research and build log from October 2026, kept as history.

Friends, each running HellForge with their own local backend, play together through the game's own social menu and
join flow. There is no port forwarding and no central HellForge server. The copied game never reaches PlayFab,
Arrowhead's services or vanilla players' sessions.

## How co-op works for players

1. In the launcher, open Settings. The MULTIPLAYER card has "Local PlayFab" and "Lobby and session through Steam
   friends", both on by default and both needed for co-op, and "LAN co-op", off by default. A change applies to the next
   session. (The launcher's console still calls the first two experimental.)
2. Compare modlist codes. Friends need the same gameplay mods, in the same versions with the same files; the order is
   up to each player, and cosmetic mods, packs and Lua mods are each player's own. The Mods page shows the code.
   [mods.md](mods.md#playing-with-friends) describes what each player sees when a join is refused.
3. Start a session. In the game's social menu, the friends list shows Steam friends who are playing HELLDIVERS 2 with
   HellForge running, and you join their game from there. Steam sign-in, Steam friends and Steam lobbies are the real
   ones.
   For players on the same network instead, switch "LAN co-op" on in the same card (and, only if the network drops
   broadcasts, type the host's IP address in the field below it). Nobody has to be a Steam friend: every HellForge backend
   on the subnet announces itself, the two find each other, and the game's social menu lists the other player as a friend
   you can join. The first LAN session asks Windows for one administrator consent, for the firewall rule that lets the
   local network reach this PC's backend. The rule names this PC's backend only, and only for the local subnet. Anyone
   who can reach that port can join, so keep LAN co-op off on networks you do not trust.
4. While you are in a friend's lobby, you play with the host's Local Stats (the in-game editor's saved changes). Your
   in-game editor turns grey and read-only, with "READ-ONLY · HOST: <name>" in its header. The host's shows "HOSTING ·
   FRIENDS PLAY WITH YOUR STATS".
5. When the host leaves, the lobby closes for everyone, as HD2's host ends the session. Your own stats come back.

Each session's logs hold `backend/multiplayer.log` (the backend's side of co-op) and `coop-diagnostics.log` (what this
machine's game saw of mod content). Collect both players' logs (launcher Console > "Save logs to Desktop") for a bug
report.

## How co-op works for contributors

HellForge replaces the services and never patches the game's logic. The game's join checks still run as they always
do (see [The join gate](#the-join-gate)).

### What a session starts

With "Local PlayFab" on (`--local-playfab`, settings key `localPlayFab`), the session runner redirects the game's
PlayFab web API to the local backend (`HellForgeLocalRoutingPatch.StartPlayFabRedirect` in
`src/HellForge.Session/LocalRoutingPatch.cs`: heap data only). It waits for the engine to format the PlayFab URL and
replaces it with `http://127.0.0.1:<port>/playfab`; three instruction checks pin the layout. With it on, the session
does not pass `-ClearPlatformMatchmaking`.

With "Lobby and session through Steam friends" on (`--multiplayer`, settings key `sdkShims`), the session runner:

1. swaps the copy's `PartyWin.dll` and `PlayFabMultiplayerWin.dll` for HellForge's stand-ins, after checking their
   pins. The originals are restored after the session, or at the next start if a session was cut short;
2. copies the real `steam_api64.dll` to `<run>/steam-link/` for the backend's Steam link;
3. starts the backend with `--pipe hellforge-<run id> --link steam:<dll> --mod-digest <digest> --mods <mods folder>`
   and, when there is one, `--mod-list <run>/modlist.txt`;
4. sets `HELLFORGE_PIPE` for the game, so the stand-ins can find the backend.

With "LAN co-op" on (`--lan`, settings key `lanCoop`), the session runner does the same (the stand-ins and the session
need them just the same), and starts the backend with `--link lan` instead: no Steam library is copied for a peer link,
and typed addresses go in `--lan-peer <ip>[,<ip>]`.

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
- PlayFab lobby invites are not implemented (`PFLobbySendInvite` completes with `E_NOTIMPL`), and lobby searches find
  nothing.

The stand-ins cannot talk to PlayFab at all, so a vanilla client given a HellForge connection string fails, and a
HellForge client given a vanilla one refuses it.

### The backends and the Steam link

Friends' backends talk to each other; the copied game itself keeps talking to loopback only.

| Piece | Source | What it does |
|---|---|---|
| Steam link | `Multiplayer/SteamPeerTransport.cs` | `ISteamNetworkingMessages` in the backend process (which is not firewalled), channel 0, reliable or unreliable sends with AutoRestartBrokenSession. Accepts sessions from Steam friends only. |
| LAN link | `Multiplayer/LanTransport.cs` | Backends announce themselves on the subnet (UDP 47991: broadcast, and unicast to typed addresses) and pair over TCP on the same port, one connection per peer, frames length-prefixed. A peer is known by the SteamID64 of the Steam account signed in on its PC, so the game sees it as a friend. Unauthenticated: anyone who announces itself on the LAN is a peer. |
| Presence | `SteamPeerTransport.cs`, `FriendPresence.cs` | Sets the Steam rich presence key `hellforge` (to the protocol string) and clears it on exit; every 5 s, lists Steam friends playing HD2 (app 553850) who have the key. |
| Lobbies | `Multiplayer/LobbyService.cs` | The creator's backend owns each lobby, and the lobby stand-in mirrors it from events. |
| Party | `Multiplayer/PartyService.cs` | The host's backend owns each Party network, admits only its lobby members, assigns endpoint ids and routes the game's messages (guaranteed delivery is a reliable Steam message). |
| Peer upkeep | `Multiplayer/MultiplayerHost.cs` | Peers sharing a lobby or network send `peer.ping` every 2 s; one silent for 15 s is treated as gone (members leave, its lobbies and networks end). Logs to `multiplayer.log`. |
| Frames | `Multiplayer/Frame.cs` | A u32 length, a u16 field count, then length-prefixed fields; at most 1 MiB. |

Paths in the table are under `src/HellForge.LocalBackend/`. Endpoint ids requested by this machine's game start at
1000; each friend's endpoints take ids in a block of 64 starting at 4096. The lobby's size limit is the game's own
`maxMembers`. Test transports (in memory, and loopback UDP) stand in for Steam in the tests.

### The local network link

`LanTransport` (`Multiplayer/LanTransport.cs`) is the other side of the same transport interface, so lobbies, Party
traffic, presence, keys and stats ride it unchanged. What differs is who may be a peer and how one is found:

- Each backend announces itself every 2 s (UDP 47991) with its SteamID64 and port: to `255.255.255.255`, to each
  interface's subnet broadcast, and to any address typed in Settings (`--lan-peer`). The announcement carries the
  sender's address with it, so the receiving backend learns the sender's address without a broadcast of its own.
- The backend with the smaller SteamID dials; the larger one never does, so two backends cannot build rival connections.
  When only the larger one hears an announcement, it answers with its own, addressed back to the sender.
- The connection is TCP on the same port (47991), one per peer. Every frame is a 4-byte little-endian length and then
  the frame (`Frame.Encode`), at most 60 000 bytes, so frames arrive ordered and whole as the Steam link's reliable
  messages do; the unreliable flag is ignored.
- The peer's SteamID64 is read from `HKCU\Software\Valve\Steam\ActiveProcess` (`ActiveUser`), the account the game
  logs in as, so PlayFab ids (`PlayFabService.MasterId`) and lobby member ids line up exactly as with the Steam link,
  and the peer shows in the social menu like a Steam friend (named from its id; there is no persona name to read).
- Trust: nothing authenticates a peer beyond "it announced itself on the local network", unlike the Steam link, which
  accepts only the signed-in account's friends. Keep LAN co-op off on networks you do not trust.
- The backend is not in the network block (`scripts/NetworkBlock.Common.ps1` lists the game's executables), so the block
  needs no change. What LAN co-op needs is one inbound Windows Firewall rule for
  `app/backend/HellForge.LocalBackend.exe`, TCP and UDP 47991, local subnet only, installed by
  `scripts/Install-LanCoop.ps1` on the first LAN session (one administrator consent; marker
  `network-block/lan-coop.json`) and removed by the uninstall.

The protocol is `hellforge-mp-6`: mods set up in id order, added entities numbered by resource id, and mod content
values in `stats.set`. A backend with another protocol is refused.

| Frames | Names |
|---|---|
| Pipe (stand-in to backend) | `hello` |
| Lobby | `lobby.create`, `lobby.join`, `lobby.update`, `lobby.leave`, `lobby.result`, `lobby.memberAdded`, `lobby.memberRemoved`, `lobby.updated`, `lobby.disconnected`, `lobby.refused` |
| Party | `party.host`, `party.connect`, `party.endpoint`, `party.endpointGone`, `party.leave`, `party.message`, `party.connected`, `party.endpointCreated`, `party.endpointDestroyed`, `party.deviceJoined`, `party.deviceLeft`, `party.networkGone` |
| Between backends only | `peer.ping`, `account.key`, `presence`, `presence.hello`, `stats.set`, `war.clock` |
| Live-edit pipe (backend to game) | `session`, `notice`, `set`, `reset`, `base`, `get` |

### Joining and the modlist gate

A joiner must join as themselves: the member id must equal the SteamID that Steam networking names as the sender. The
host's backend checks a join in this order:

1. the protocol;
2. the modlist digest, so a mismatch is refused before the game sees a member join;
3. whether the lobby is locked or full.

A refused join is answered with HRESULT `0x89236227` ("lobby not joinable").

The digest is a SHA-256 over the gameplay mods sorted by id (id, version and file hashes), so load order does not
count. It leaves out the Local Stats mod (`zz.local.stats`), cosmetic mods, mods made only of Lua patches, and files a
zipped mod writes into its own folder. The joiner's backend also sends its gameplay mods one by one (`--mod-list`, the
session's `modlist.txt`, HellForge.Core's `ModlistDiff`), so the refusal can name the mods that differ. The host sends
`lobby.refused` [reason, up to 6 lines for the joiner's game]. Both backends turn it into notices ("Couldn't join
<host>" for the joiner, "<friend> couldn't join" for the host) and post them to their game over the live-edit pipe
(`notice`), and the in-game editor draws them over the game. Backends from before the list send only the reason; they
still interoperate.

Once a join is accepted, the host sends `war.clock`, and the joiner's backend adopts the host's war time.

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

Everyone in a session plays with the host's Local Stats. The design is in [sdk-roadmap.md](sdk-roadmap.md), phase 3,
"Runtime edits in multiplayer".

- The Local Stats mod (`zz.local.stats`) is not part of the modlist digest, so friends with different saved changes can
  play together. Every other gameplay mod must still match.
- The host's backend (`StatsShare`, `src/HellForge.LocalBackend/Multiplayer/StatsShare.cs`) checks once a second for
  changes to `mods/zz.local.stats/hellforge-stats.json` (the session passes `--mods`), the editor's `mod-stats.json`
  (mod content values) and the editor settings' `mounts` (arm swaps and hellpod payloads). A friend admitted to a lobby
  hosted here gets them at once, and every friend gets them again on each change (the editor saves on APPLY, RESET ITEM
  and preset import), as `stats.set` [version, then each file gzipped].
- The joiner's backend accepts `stats.set` only from the host of the lobby it joined, writes the files into the editor's
  folder as `host-stats.json`, `host-mod-stats.json` and `host-mounts.json`, and serves the session's live-edit pipe
  (`\\.\pipe\hellforge-live-<run id>`, `native/mod-loader/live.c`). `HellForge.Core.StatsSync` turns the host's file
  into live edits of the Local Stats mod: `set` for the host's values, `base` for the joiner's own saved changes the
  host does not have. Attachments and status multipliers go over the pipe too.
- The game is told its role over the pipe (`session`: solo, host, or joiner with the host's name; `HfApi.session` for
  mods). A joiner's in-game editor allows browsing and export, and refuses typing, APPLY, RESET ITEM, RESET TO DEFAULTS
  and preset import.
- When the host leaves, closes the lobby or stops answering (15 s), the host files are deleted and the joiner's own
  values come back (`set`, or `reset` where it had none).
- Not synced: a stats mod's own records (`"own"`), and armor perks, which are solo only (with more than one player,
  everyone gets the armor's own).
- Trust: joiners take the host's values. That is fine for co-op with friends and on a trusted local network.

### When the host leaves

When the lobby's owner leaves, the lobby closes for everyone (`lobby.disconnected`, reason LobbyDeleted). When the
host's game closes, its Party networks end (`party.deviceLeft`, then `party.networkGone`). A host silent for 15 s has
the same effect. The backend has no lobby owner migration. A rejoining friend's old membership and endpoints are
removed first, and the mod loader (`native/mod-loader/netreap.c`) destroys a crashed peer's dead connection so the
friend can rejoin.

### Mod content in co-op

What mods add (stratagems, entities, behaviours, abilities) is numbered alike on every machine whatever the load
order, and HellForge's own logic runs where the game runs its own. What is done and what is still open is in
[engine-roadmap.md](engine-roadmap.md#co-op-sync-for-mod-content) and [sdk-roadmap.md](sdk-roadmap.md) (phase 7).

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

`tests/HellForge.LocalBackend.Tests` holds the multiplayer and stats sharing checks, including a LAN pair (both typed
addresses, and one side typed alone) on loopback (`MultiplayerTests.cs`,
`StatsShareTests.cs`). `HellForge.SdkShimGen smoke-lobby-link` runs two backend processes over loopback UDP and two
copies of each stand-in through the PlayFab C APIs: create, join, member data, Party connect, endpoints, a 1,200-byte
message and a broadcast, and leaving.

## History: research and build log (2026-10-14 to 2026-10-21)

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

### Co-op diagnostics (engine 0.1.19, the 0.1.11 diagnostics beta)

`coop-diagnostics.log` was added for the two-PC test of 2026-10-21 (0.1.8), which left these open: a friend's Pelican
never ran its behaviours on the others' machines; a silo launch did not carry over (the other machine's missile stayed
or vanished, with no flight or blast), so the silo launches on every machine again; a host's Orbital Plasma Barrage was
unseen by the friend, and the friend's landing showed on the host's previous beacon on the host's screen; and a host's
custom support weapon's shots were unseen by the friend.
