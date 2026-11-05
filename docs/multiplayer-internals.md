# Co-op internals

<!-- diataxis: explanation -->

This page is for contributors working on HellForge's co-op. It covers what a session starts, the stand-in DLLs, the
backends and their Steam link, joins, privacy, invitations and kicks, and the research into how vanilla HD2 does co-op.

HellForge replaces the services and never patches the game's logic. The game's join checks still run as they always do
(see [The join gate](#the-join-gate)). How players use co-op is in [multiplayer.md](multiplayer.md). The other
contributors' pages:

- [Co-op mod sync](multiplayer-mod-sync.md): a joiner takes the host's mods, the rejoin after a restart, and trust.
- [Public lobbies and strangers](multiplayer-lobbies.md): the lobby directory, the lobby browser and Play with
  strangers.
- [Co-op in game](multiplayer-in-game.md): stats, assets, pickups and shots in co-op, and the mod channel.

## What a session starts

**Local PlayFab** (`--local-playfab`, settings key `localPlayFab`). With it on, the session runner redirects the game's
PlayFab web API to the local backend (`HellForgeLocalRoutingPatch.StartPlayFabRedirect` in
`src/HellForge.Session/LocalRoutingPatch.cs`: heap data only).

- It waits for the engine to format the PlayFab URL, for as long as the game runs. A start-up dialog such as the GPU
  driver warning can hold the game for minutes.
- It replaces the URL with `http://127.0.0.1:<port>/playfab`. Three instruction checks pin the layout.
- With it on, the session does not pass `-ClearPlatformMatchmaking`.

**Lobby and session through Steam friends** (`--multiplayer`, settings key `sdkShims`). With it on, the session runner:

1. swaps the copy's `PartyWin.dll` and `PlayFabMultiplayerWin.dll` for HellForge's stand-ins, after checking their
   pins. The originals are restored after the session, or at the next start if a session was cut short;
2. copies the real `steam_api64.dll` to `<run>/steam-link/` for the backend's Steam link;
3. starts the backend with `--pipe hellforge-<run id> --link steam:<dll> --mod-digest <digest> --mods <mods folder>`
   and, when there is one, `--mod-list <run>/modlist.txt`;
4. sets `HELLFORGE_PIPE` for the game, so the stand-ins can find the backend.

With multiplayer off, no DLLs are swapped and the backend gets no pipe.

## The local PlayFab web API

`src/HellForge.LocalBackend/PlayFabService.cs` serves the PlayFab calls under `/playfab`. It reads the Steam ID from the
session ticket (never stored) and derives the ids from it:

- the title player account is the SteamID64 in hex;
- the master account is SteamID64 + 0x1000000000000000.

Every HellForge backend therefore gives a friend the same ids with no exchange.

## The stand-in DLLs

`native/sdk-shims` holds the hand-written parts of the two stand-ins (`lobby_shim.cpp`, `party_shim.cpp`, `link.cpp`,
`shim_common.cpp`).

- The hand-written exports stay in `lobby_shim.cpp` and `party_shim.cpp`, which the generator reads.
- Their state and backend code are in headers only they include (`lobby_state.h`, `lobby_remote.h`, `party_state.h`,
  `party_network.h`).
- `src/HellForge.SdkShimGen` generates a logged stub for every other export from Microsoft's public SDK headers.
- `scripts/Build-Engine.ps1` builds them ([engine.md](engine.md)).
- Every call is logged to the session folder.

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
- `PFMultiplayerFindLobbies` (the Galactic Map's search) goes to the backend as `lobby.find`. Without the backend a
  search finds nothing.

The stand-ins cannot talk to PlayFab at all, so a vanilla client given a HellForge connection string fails, and a
HellForge client given a vanilla one refuses it.

## The backends and the Steam link

Friends' backends talk to each other. The copied game itself keeps talking to loopback only.

| Piece | Source | What it does |
|---|---|---|
| Steam link | `Multiplayer/SteamPeerTransport.cs` | `ISteamNetworkingMessages` in the backend process (which is not firewalled), channel 0, reliable or unreliable sends with AutoRestartBrokenSession. Accepts sessions from Steam friends only. Send rate 8 MiB/s and 8 MiB buffers, set at start ([Mod sync speed](multiplayer-mod-sync.md#mod-sync-speed)); a message Steam does not take is logged. |
| Presence | `SteamPeerTransport.cs`, `FriendPresence.cs` | Sets the Steam rich presence key `hellforge` (to the protocol string) and clears it on exit; every 5 s, lists Steam friends playing HD2 (app 553850) who have the key. |
| Lobbies | `Multiplayer/LobbyService.cs` | The creator's backend owns each lobby, and the lobby stand-in mirrors it from events. |
| Party | `Multiplayer/PartyService.cs` | The host's backend owns each Party network, admits only its lobby members, assigns endpoint ids and routes the game's messages (guaranteed delivery is a reliable Steam message). |
| Peer upkeep | `Multiplayer/MultiplayerHost.cs` | Peers sharing a lobby or network send `peer.ping` every 2 s; one silent for 15 s is treated as gone (members leave, its lobbies and networks end). Logs to `multiplayer.log`. |
| Frames | `Multiplayer/Frame.cs` | A u32 length, a u16 field count, then length-prefixed fields; at most 1 MiB. |

- Paths in the table are under `src/HellForge.LocalBackend/`.
- Endpoint ids requested by this machine's game start at 1000. Each friend's endpoints take ids in a block of 64
  starting at 4096.
- The lobby's size limit is the game's own `maxMembers`.
- Test transports (in memory, and loopback UDP) stand in for Steam in the tests.

**The protocol.** The feature protocol is `hellforge-mp-9`, at wire level `1` (`LobbyService.Protocol`,
`LobbyService.WireLevel`).

- `LobbyService.Compatible` accepts it. Later feature versions can be added there while they keep wire level 1 and
  older features still answer.
- The directory lists `hf.wire` beside `hellforge` and searches by `hf.wire` equal, so a directory does not have to
  know every feature version. `DirectoryService.Valid` still checks that the listing's feature protocol is compatible.
- Joins and rejoins use `Compatible` too.
- Protocol 9: per-player mods leave the modlist digest, the mod set carries both tiers, bundled mods count by
  `coopLevel`, and the directory searches by the wire level.

**HellForge 0.3 (`hellforge-mp-8`, `LobbyService.LegacyProtocol`).** Protocol 9 changed what must match, not the frames,
so `Compatible` accepts 0.3's too, and this backend speaks down to it
([HellForge 0.3 players](multiplayer-lobbies.md#hellforge-03-players)). Older protocols are refused as another
HellForge.

| Frames | Names |
|---|---|
| Pipe (stand-in to backend) | `hello` |
| Lobby | `lobby.create`, `lobby.join`, `lobby.update`, `lobby.leave`, `lobby.kick`, `lobby.invite`, `lobby.find`, `lobby.result`, `lobby.memberAdded`, `lobby.memberRemoved`, `lobby.updated`, `lobby.disconnected`, `lobby.found`, `lobby.inviteReceived`, `lobby.refused` |
| Party | `party.host`, `party.connect`, `party.endpoint`, `party.endpointGone`, `party.leave`, `party.message`, `party.kick`, `party.connected`, `party.endpointCreated`, `party.endpointDestroyed`, `party.deviceJoined`, `party.deviceLeft`, `party.networkGone` |
| Between backends only | `peer.ping`, `account.key`, `presence`, `presence.hello`, `stats.set`, `war.clock`, `lobby.invited`, `lobby.rejoin`, `lobby.rejoined`, `mods.info`, `mods.offer`, `mods.files`, `mods.get`, `mods.filelist`, `mods.chunk` |
| Mod channel (between backends) | `mods.msg`, `mods.share`, `mods.sharesnap` |
| Live-edit pipe (backend to game) | `session`, `notice`, `set`, `reset`, `base`, `get` |
| Live-edit pipe, SDK 16 (both ways) | `message`, `share`, `sharereset` (backend to game); `send`, `share` (game to backend) |

## Joining and the modlist gate

A joiner must join as themselves: the member id must equal the SteamID that Steam networking names as the sender. Its
member properties must keep to PlayFab's limits (below). The host's backend checks a join in this order:

1. whether the joiner may join at all: not kicked from this lobby, and allowed by its privacy
   ([Lobby privacy, invitations and kicks](#lobby-privacy-invitations-and-kicks)). This comes first, so a refused
   stranger is never offered the host's mods;
2. the protocol (`LobbyService.Compatible`: `hellforge-mp-9` at wire level 1, or 0.3's `hellforge-mp-8`);
3. the modlist digest, so a mismatch is refused before the game sees a member join. A 0.3 joiner's mods are compared
   one by one instead ([HellForge 0.3 players](multiplayer-lobbies.md#hellforge-03-players));
4. whether the lobby is locked or full.

A refused join is answered with HRESULT `0x89236227` ("lobby not joinable"), after `lobby.refused` with the reason.

**The digest** is a SHA-256 over the shared gameplay mods sorted by id (id, version and file hashes), so load order does
not count. It leaves out:

- the Local Stats mod (`zz.local.stats`);
- cosmetic mods, and mods made only of Lua patches;
- files a zipped mod writes into its own folder;
- **per-player gameplay mods**: stats-only mods, texture/sound/text-only patch mods, and mods whose manifest says
  `"coop": "per-player"`.

Per-player mods, co-op levels and one protocol for a family of releases came with the matchmaking overhaul of
2026-10-30 (steps 11 and 12: co-op levels for bundled mods first, then `coopLevel` for any mod). Before it:

- friends were refused for mods that only change what one PC sees;
- bundled mods built at different times could split a lobby;
- every protocol change shut every older HellForge out.

**Per-player mods** are listed in their own PER PLAYER group on the Mods page, and in `mods` as `per-player`.

- A manifest can force `"shared"` or `"per-player"`.
- A per-player declaration that would change the wire (network field types) or numbered resources (a data spec) is
  refused.
- The mod set still carries every gameplay mod, with an optional `"tier"` per entry, so a joiner can hear which
  per-player mods the host has.
- The in-game editor is `"coop": "per-player"`.

**Co-op levels** (`ModSets.MatchLines`). A level is its author's promise that copies at one level play together, so a
fix release does not split players.

- A bundled mod with `"coopLevel"` contributes `id|bundled|<coopLevel>` instead of file hashes. Bundled mods without
  the field keep the file comparison.
- Any other mod with one contributes `id|coop|<coopLevel>`. A raised level splits players.
- Bump it for changes other machines see or need, not for one-machine fixes.
- Mod sync still checks every downloaded byte against the content hash. A joiner whose copy is at the host's level
  plays its own copy (`SyncItem.LocalHash`).

**Naming the mods that differ.** The joiner's backend also sends its shared gameplay mods one by one (`--mod-list`, the
session's `modlist.txt`, HellForge.Core's `ModlistDiff`), so the refusal can name the mods that differ.

- The host sends `lobby.refused` [reason, up to 6 lines for the joiner's game].
- Both backends turn it into notices: "Couldn't join \<host>" for the joiner, "\<friend> couldn't join" for the host.
  They post them to their game over the live-edit pipe (`notice`).
- The in-game editor draws them over the game: the joiner's as a panel, the host's (kind `brief`, `HF_NOTICE_BRIEF`)
  as a small note in the top right corner.
- Backends from before the list send only the reason. They still interoperate.

**After the join.** Once a join is accepted, the host sends `war.clock`, and the joiner's backend adopts the host's war
time. A time below 0 or above 20 years is ignored, whoever sends it.

Then the host sends `mods.info` [lobby id, its set in ModSet's wire form, with a `tier` of `shared` or `per-player` per
entry]. The joiner's backend takes one `mods.info` per join, only from the host whose lobby it joined. If the host's
per-player mods differ, it shows a brief notice: "\<host> plays with 2 mods you don't: \<names>. Each player keeps their
own." Nothing downloads for those, and `ModSyncPlan` ignores per-player entries entirely.

**PlayFab's limits.** Lobby frames from other machines keep to PlayFab's limits (`PFLobby.h`). A frame that breaks them
fails with `E_INVALIDARG`.

- At most 30 lobby, member and search properties each, at most 60 changes in one update, and the result within the
  limits.
- HellForge also caps a key at 64 characters and a value at 1,024. The game's longest value seen is 123 characters, its
  Party descriptor.
- Search keys must be `string_key1`..`string_key30` or `number_key1`..`number_key30`, and a `number_key`'s value must
  be a number.

## Lobby privacy, invitations and kicks

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
  host's join gate 0x108BC70. The gate refuses a non-friend under 1 and anyone not invited under 2. An active SOS Beacon
  advertises 0.
- The stand-in logs captured 0, 1 and 2 at lobby creation. `"19"` and `number_key6` were always alike.
- The access policy was always 0 whatever the setting, so the access policy says nothing about privacy.

The game's own join gate still applies the same rules once a member is in. The backend refuses first, before the
modlist check and before the game sees a member join.

**Who is always let back.** Under every setting, a machine already in the lobby, or one that was admitted to it before
(its game crashed, say), may join again. So changing the privacy never removes anyone.

- A machine refused only for its gameplay mods passed the privacy check. It is expected back for 15 minutes with the
  host's mods ([Mod sync](multiplayer-mod-sync.md#mod-sync)), whatever the privacy then. A kick ends that too.
- A lobby that turns Friends only or Invite only stops being listed within a second, and its members stay.

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

**Kicks.** The owner's `PFLobbyForceRemoveMember` becomes `lobby.kick [op, lobbyId, memberId, preventRejoin]`, which
only the owner's own machine may send. The backend then:

1. removes the machine's members;
2. sends the kicked machine `lobby.memberRemoved` with reason 1 (LocalUserForciblyRemoved), then `lobby.disconnected`
   with reason 0 (NoLocalMembers), as the SDK reports a force removal. The kicked machine's backend shows the game
   "Removed from \<host>'s lobby";
3. sends every other member `lobby.memberRemoved` with reason 2;
4. answers its own game with the completion, after that game's own MemberRemoved, as the SDK documents.

What follows:

- The machine is refused for the lobby's lifetime, whatever `preventRejoin` says (decided 2026-10-23), and it cannot be
  invited back. Nothing is stored. Its join back is refused with "The host removed you from this lobby".
- The host's `multiplayer.log` has "Kicked ..." and "Removed a kicked machine from Party network ...".
- `PartyService.Expel` takes the machine out of every Party network the host has and tells it the networks are gone.
  It has no endpoints left, so whatever it sends is dropped, and it cannot connect again because it is in no lobby.
- The Party stand-in's `PartyNetworkKickDevice` and `PartyNetworkKickUser` on a hosted network send
  `party.kick [networkId, node]`, which kicks that machine from the host's lobbies the same way.

Which of the two kick calls HD2 makes is not known, so both are covered:

- No session log shows either call, and the exe's list of 22 `PFLobby*`/`PFMultiplayer*` imports was not kept.
- 20 functions have been seen called in session logs, and the Galactic Map's search (`PFMultiplayerFindLobbies`) is a
  21st. That makes `PFLobbyForceRemoveMember` a likely 22nd, but this is an inference.

## The join gate

The host's join gate in the game (game.dll 0x108BC70) still applies its privacy rules and compares the game and
network-config hashes. HellForge never fakes those checks.

## Player keys

Each game logs in with a public key, and the host's game asks the backend for a joiner's key before it admits them
([Player keys in the game](#player-keys-in-the-game)). HellForge keeps each game's own key from its login, as text,
verbatim (1 to 256 printable characters).

- The backends swap keys over the Steam link as `account.key`: on the first frame heard from a friend, when the local
  key changes, and in reply to a new key.
- Each backend can then answer `/Account/Keys` for every player in its session (`AccountKeys.cs`,
  `Multiplayer/AccountKeyExchange.cs`). It waits up to 3 s for a key.
- Platform account ids are taken to be SteamID64s (confirmed in the second two-PC test).
- A lookup with no key is logged to `multiplayer.log` with the id.

## The social tab

The friends list and the in-game state of friends come from `FriendPresence.cs`. It lists Steam friends who have the
`hellforge` rich presence key, without sending anything to anyone else.

- Friends' backends send `presence.hello` to a newly found friend at once.
- They send `presence` every 10 s or on a change: the player's PlayFab profile objects (which the engine needs to list
  a profile) and the game's current lobby with its HellForge connection string.
- A friend's lobby stays listed for 45 s after their last frame.
- A profile request waits up to 6 s for friends' objects.
- Accounts are taken to be SteamID64s. Unanswered ids are logged.

## When the host leaves

- When the lobby's owner leaves, the lobby closes for everyone (`lobby.disconnected`, reason LobbyDeleted).
- When the host's game closes, its Party networks end (`party.deviceLeft`, then `party.networkGone`).
- A host silent for 15 s has the same effect.
- The backend has no lobby owner migration.
- A rejoining friend's old membership and endpoints are removed first. The mod loader (`native/mod-loader/netreap.c`)
  destroys a crashed peer's dead connection so the friend can rejoin.

## GameGuard

GameGuard, the anti-cheat the supported build shipped with, does not run in a HellForge session, which is offline and
blocked from the internet. HellForge does not copy GameGuard's files into an installation.

## Tests

- `tests/HellForge.LocalBackend.Tests` holds the multiplayer, stats sharing, asset sharing, mod sync, rejoin and
  directory checks (`MultiplayerTests.cs`, `StatsShareTests.cs`, `AssetShareTests.cs`, `ModSyncTests.cs`,
  `ModSyncSpeedTests.cs`, `RejoinTests.cs`, `DirectoryTests.cs`).
- `tests/HellForge.Tests` has Core's mod sync checks (`ModSyncTests.cs`).
- `HellForge.SdkShimGen smoke-lobby-link` runs two backend processes over loopback UDP and two copies of each stand-in
  through the PlayFab C APIs. It covers create, join, member data, Party connect, endpoints, a 1,200-byte message and a
  broadcast, an invitation, a kick, and leaving. Last, the friend comes back as after a mod sync. A new backend started
  with `--rejoin` and a new copy of the lobby stand-in run, and its game receives the host's invitation once it has its
  own lobby, and joins with it.
- `HellForge.SdkShimGen smoke-lobby-search` runs two backends sharing a folder directory and two lobby stand-ins, as
  friends and again as strangers. The friend's search finds the host's lobby, joins it, and stops finding it after the
  host leaves.

## Research findings

The findings of the October 2026 research and build log that still hold. The log itself, with each step's two-PC test
plan, is in
[the page's earlier version](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multiplayer.md#history-research-and-build-log-2026-10-14-to-2026-10-23).
Findings on mod sync, public lobbies and stats are on those topics' pages.

The research was static analysis of build F5FEE03DCFDB, unless marked as seen live. Its sources were our own snapshots
of the game, HD2Runtime's research notes (matchmaking safety, peer messaging, PlayFab lobbies; not published) and
Microsoft's public SDK headers.

### How vanilla HD2 does co-op

There are three layers, all in the engine (`helldivers2.exe`, stingray `runtime/playfab/*`). game.dll drives them
through the engine API table T = `[[game.dll+0x3326308]+0xF8]` (exe 0x27CDE70, about 96 slots).

1. **The PlayFab web API** (`playfab_web_api.cpp`).
   - The base URL is `https://<title>.playfabapi.com`, with title ids from `settings.ini` `playfab.title_ids` (LIVE
     `974A9`).
   - The base URL is formatted into a 0x80-byte heap buffer (web object +0x90; init exe 0x8C4127), and every request is
     `"%s/<call>"` on it.
   - The calls: `Client/LoginWithSteam` (with a genuine Steam session ticket) and the other `LoginWith*`/`Link*`
     variants, `Client/GetPlayerCombinedInfo`, `Client/GetPlayFabIDsFromSteamIDs`,
     `Profile/GetTitlePlayersFromMasterPlayerAccountIds`, `Profile/GetProfile(s)`, `Object/SetObjects`,
     `Client/GetFriendsList`, `Client/UpdateUserTitleDisplayName` and `Client/ReportPlayer`.
   - With `Object/SetObjects` each player publishes `testament_backing_platform`, `testament_platform_user_name` and
     `testament_platform_peer_id`.
   - The login result (system +0x28, T slot 1) is what the backend-enable gate's platform service checks. That is why
     offline sessions need `-ClearPlatformMatchmaking`.
   - The title player account id (system +0xB0) is the game's peer id, and the user's vanilla saves are named after it.
2. **PlayFab Lobby** (`PlayFabMultiplayerWin.dll` 1.8.7) **and PlayFab Party** (`PartyWin.dll` 1.10.16).
   - Both DLLs are byte-identical to Microsoft's public SDK releases.
   - The exe imports 22 `PFLobby*`/`PFMultiplayer*` and 157 `Party*` functions (recovered by emulating the protector's
     import stubs on a snapshot).
   - Lobbies hold the session: members, properties keyed `"0".."26"`, search keys, and `invite_id`/`descriptor` for the
     Party network.
   - Party carries the game traffic (`playfab_transport`) and VOIP over Azure relays.
3. **Steam** (`SteamMatchMaking009`, `SteamFriends017`, engine `steam_lobby`, `+connect_lobby`): the platform lobby for
   friend joins and the Steam overlay. Each member posts its platform lobby as the PlayFab member property
   `platform_lobby` (`"%d:%llu:%s:%s"`).

### Player keys in the game

- Each game logs in (`POST /Account/Login`) with a 32-byte `publicKey`, encoded as text by the engine (T+0x38 slot
  0x128).
- When a player joins, the host's game asks Arrowhead's backend for that player's key with
  `GET /Account/Keys?id=<platform account id>`. It returns `{"accountKeys":[{"platformAccountId","key"}]}` (builder
  0x12be5a0, parser 0x1042120, stored in a table of 8 by the consumer at 0x12c996b).
- The host admits the player only when the key matches the one the player's game shows on the game connection (T+0x38
  slot 0x130, check at 0x108f92b).
- With no answer the joiner is never admitted and sees "lobby timeout". In the first two-PC test the host's backend
  answered 501, and the joiner left about 100 ms later.
- A key exchange that required 64 hex digits dropped every real key, which left the joiner on "synchronizing space
  time". Keys are kept verbatim for that reason.

### The social tab in the game

- The FRIENDS list is PlayFab `Client/GetFriendsList`: the engine asks `{"ExternalPlatformFriends": "None"}` and reads
  `FriendPlayFabId` and `TitleDisplayName`.
- Online and in-game state and joining come from Arrowhead's lobby presence service.
- Each game uploads its lobby with `PUT /lobby` (players, operation, region; the reply is ignored), keeps it alive with
  `POST /lobby/<id>/member` every 20 s and drops it with `DELETE /lobby/clear`.
- The tab asks `POST /lobby/accounts {"ids":["<account>",...]}` and reads
  `{"accountLobbies":[{"account","lobbyId"}],"lobbies":[...]}`. The game.dll builders are 0x12cbc40/0x12cbdc0, the
  parser 0x1055b50, and the lobby fields 0x1053f60 (the uploaded fields + `lobbyId`, `connectionString`).

### The design

- Replace the services, never patch the game logic.
- A local PlayFab web API in the backend, with ids derived from the SteamID, and a redirect of the game's PlayFab URL.
- Replacement SDK DLLs: HellForge's own `PlayFabMultiplayerWin.dll` and `PartyWin.dll` implementing the public C APIs.
  Lobby state and Party messages go to the local backend over loopback, and the backends relay between friends. The
  game loads the stand-ins through ordinary DLL loading.
- HD2's two DLLs are byte-identical to Microsoft's public releases, so those packages' headers give the exact
  signatures:
  - Party: NuGet `Microsoft.PlayFab.PlayFabParty.Cpp.Windows` 1.10.16, SHA-256 `DE34667C…`;
  - Multiplayer: GitHub `PlayFab/PlayFabMultiplayer` v1.8.7 Windows zip, `95BC5F95…`.
- Both export flat C APIs (157 and 78 functions, undecorated `__stdcall` on x64).
- Backends talk over Steam networking relays between Steam friends, with no port forwarding. A VPN such as Tailscale
  was the planned fallback; there is no VPN path.
- Steam (Valve) stays genuine: real sign-in, real friends, real Steam lobbies.
- With Local PlayFab alone, the original SDK DLLs try PlayFab's real lobby and relay services, and the network block
  stops that.

### The stand-ins and the game

Seen live in the first boots with the stand-ins (2026-10-14, with Local PlayFab):

- The protected exe loads both stand-ins. game.dll and the exe are WinLicense-packed on disk. The unpacked images used
  for this research match this build (verified against a crash dump's code bytes).
- At boot the game calls `PFMultiplayerSetMemoryCallbacks`, `PFMultiplayerInitialize`, `PFMultiplayerSetEntityToken`
  (our local PlayFab entity token), `PFMultiplayerStartListeningForLobbyInvites`, `PartySetMemoryCallbacks`,
  `PartyInitialize`, `PartyCreateLocalUser` (entity id = the SteamID-derived title player id) and
  `PartyGetLocalDevice`. It then pumps both state-change queues every frame.
- Loading the ship calls `PartyCreateNewNetwork` (to host). The game exits with code 1 when that fails synchronously,
  after `PFLobbyGetOwner`/`PFLobbyGetLobbyProperty(NULL, "8")`. The real library accepts the request and reports the
  failure later, so the stand-in does the same.
- After `PartyCreateLocalUser` and `PartyGetLocalDevice` succeed, the game tries `PartyDeviceCreateChatControl`. A
  failure there is tolerated.
- In the hosting frame it calls create, connect, authenticate, `ConnectChatControl` with a null control, and create
  endpoint.
- In the same frame as `PartyCreateNewNetwork` it calls `PartyConnectToNetwork` with the descriptor just written (the
  documented hosting pattern). A synchronous failure there exits the game too.
- So the stand-in models networks as the docs describe. `PartyConnectToNetwork` returns a network at once, and
  authentication, endpoints, invitations and leave requests queue on it.
- A failure is delivered in the documented order: the connect completion, the queued operations' completions and
  endpoint/invitation destroyed notices, `NETWORK_DESTROYED`, then a requested leave.
- Network objects live until `PartyCleanup`, so no handle the game still holds can dangle.
- The game reads lobby keys "0".."26" from an engine lobby that has no PlayFab lobby yet. Its engine wrapper (exe
  0x8c6f00, slot T+0x110) returns the SDK's output without initializing or checking it. The real SDK writes
  `*value = NULL` before validating.
- A stub that failed without writing let stack garbage reach `strlen`: an access violation reading 0x8 at
  game.dll+0x1092908. The `owner()` wrapper, 0x8c6ce0, initializes and null-checks its output.
- So all stubs clear their write-only (`_Out*`) parameters whatever the result, as the real libraries do. In-out
  parameters are left alone.
- The engine's crash handler writes minidumps to `%APPDATA%/Arrowhead/HellForge/dumps`.
- With a failed host the game behaves like the offline original: error messages, endpoint and network queries, a
  clean-up about 20 s later (`PartyNetworkDestroyEndpoint`, `LeaveNetwork`), then it hosts again.
- The ship UI polls lobby properties "5", "14" and "19" every frame. The game creates a PlayFab lobby only once it has
  a working Party network.

**Generated stubs.** Every export the stand-ins do not write by hand behaves so:

- `PartyInitialize`/`PFMultiplayerInitialize` hand out a handle, the state-change pumps report nothing, and the
  error-message functions name the stand-in.
- Every other export succeeds when it has no output parameters. Otherwise it fails without writing (Party `0x10D0`,
  Multiplayer `E_NOTIMPL`), so the game never reads an output we did not write.
- `HELLFORGE_SHIM_POLICY="Name=ok,Other=fail"` overrides single functions without a rebuild.
- The 17 Multiplayer `*WithEntityHandle` exports appear in no public header (GDK variants; HD2 uses the entity-key
  forms). They are exported without reading arguments.

**The build.** `scripts/Build-Engine.ps1` builds the stand-ins, with Zig from `scripts/Get-BuildTools.ps1`.

- The SDK packages are pinned by SHA-256 and kept in the gitignored `tools/`.
- `src/HellForge.SdkShimGen` generates a logged stub for every export from the headers, skipping the hand-written ones
  in `native/sdk-shims`. The generated sources repeat Microsoft's signatures and are never committed.
- The build checks the exports and ordinals against the originals, allows only Windows system imports, and smoke-tests
  both DLLs in-process.
- Each call is logged to `<run>/sdk-shims/<dll>-<pid>.log`: the first 32 calls per function, then every power of two.

**The stand-ins' solo behaviour** (built 2026-10-14, then moved behind the backend):

- Party: a network created here can be connected to. The game sees `REGIONS_CHANGED` (it passes no regions),
  `CREATE_NEW_NETWORK_COMPLETED`, `CONNECT_TO_NETWORK_COMPLETED`, `NETWORK_CONFIGURATION_MADE_AVAILABLE`,
  `INVITATION_CREATED` (the initial invitation, which the host authenticates with),
  `AUTHENTICATE_LOCAL_USER_COMPLETED`, `ENDPOINT_CREATED` and `CREATE_ENDPOINT_COMPLETED`, all succeeded.
- Leaving a network destroys endpoints, invitations and the network before `LEAVE_NETWORK_COMPLETED`.
- Lobby: `PFMultiplayerCreateAndJoinLobby` creates a lobby owned by the creator (`MemberAdded`, then the completion).
  Owner, members, lobby/search/member properties and their keys, policies and custom contexts read back.
- `PFLobbyPostUpdate` applies at the next delivery (`PostUpdateCompleted`, then `Updated`).
- Leaving a lobby delivers `MemberRemoved`, `LeaveLobbyCompleted`, `Disconnecting`, `Disconnected`.
- Text given to the game stays valid until `PFMultiplayerUninitialize`.
- While the lobby lived in the DLL, joining another machine's lobby failed with 0x89236226 ("lobby doesn't exist").
  Joins moved it behind the backend, which a friend's join request reaches.

**The Steam link** runs `ISteamNetworkingMessages` in the backend process, which is not firewalled, beside the running
game. That was checked with a probe program.
