# The Steam bridge: the game at low integrity

<!-- diataxis: explanation -->

**Status: in development** on `port/lockdown-0.3.0`.

- Built and tested with fixtures: the census, the broker and its channel (phase 2), the generated bridge with its first
  allow-list (phase 3), and the session side of phase 4.
- In game, one sandboxed solo session reached the title screen (2026-10-23,
  [phase 4](#phases-each-tested-before-the-next)). A mission, a controller and co-op are untested.
- The allow-list covers the calls one census run saw. The next census (a controller, a whole mission, the social tab)
  will show what it misses.
- Players found that Steam Input's controllers (a Steam Controller, for one) do not reach a sandboxed game.
  [Controllers and Steam Input](#controllers-and-steam-input) says why and what a session does.

Since the 0.3.0 beta a session is sandboxed by default. The launcher's Settings > Sandbox, "Run the game sandboxed", is
on unless turned off (`launcher-settings.json` `sandboxed`). The earlier betas' `sandbox` key is not read, so every
installation starts sandboxed once.

- The sandbox is offered only by an engine built with the Steam bridge, and only on Windows
  (`SessionRunner.CanSandbox`). Only an engine built with the Steamworks SDK has the bridge.
- Under Wine/Proton the network namespace is the block, and the sandbox is untested.
- Other switches: `HellForge.Cli play --sandbox yes|no` or `settings --sandbox yes|no`, `HellForge.Session play
  --sandbox`, or `HELLFORGE_SANDBOX=1`.
- An installation that still has the older Windows Firewall rules gets the WFP block placed by its first sandboxed
  session, with one Windows prompt. Windows Firewall never filters loopback, so those rules cannot keep the game from
  the PC's other programs.

## Why

Mods run inside the game's process, and so does anything a crafted file manages to execute through a bug in the
engine's parsers. HellForge already keeps the copied game off the internet with firewall rules. Code inside the game can
still:

- write anywhere the player can (documents, the Startup folder, `HKCU` Run keys, HellForge's own programs);
- write into other programs the player runs (the launcher, the backend, Steam);
- start programs the firewall rules do not list.

A process at **low integrity** cannot do any of that. Windows refuses its writes to the player's files, registry and
processes ("no write up"). That holds even against native code calling the kernel directly.

**Without a bridge the game stops.** Tried on one PC (branch `experiment/low-integrity-game`): the game ran at low
integrity (`0x1000`) and its graphics worked, but it stopped at **CONNECTION ERROR - STEAM NOT DETECTED**. The Steam
client's IPC objects are at the player's normal integrity, so a low-integrity process cannot use them.

## Design

```text
 game (low integrity, job: no child processes)          broker (normal integrity, one per session)
 ┌────────────────────────────────────────────┐          ┌──────────────────────────────────────────┐
 │ steam_api64.dll stand-in, bridged mode:    │  pipe    │ hellforge_steam_broker.exe               │
 │  flat API (Init, RunCallbacks, Register..) │ ───────► │  Valve's steam_api64.dll, SteamAPI_Init  │
 │  interface proxies (generated vtables)     │ ◄─────── │  the allow-list, then the flat API       │
 │  callbacks: read from the shared ring      │  ring    │  manual dispatch: callbacks, call results│
 └────────────────────────────────────────────┘          └──────────────────────────────────────────┘
```

- **The game's side** is the `steam_api64.dll` stand-in, in bridged mode (when the session sets
  `HELLFORGE_BRIDGE_PIPE`).
  - Its own functions never load Valve's DLL in that mode. Its `SteamAPI_Init` and `SteamAPI_InitSafe` (the pinned
    build's init exports) connect to the broker on first use, never in `DllMain` (no wait under the loader lock).
    It waits up to 10 s for the broker's pipe, which the session runner has already made sure of (below). Without a
    broker it **fails closed**: init returns false, `IsSteamRunning` 0, and nothing falls back to Valve's DLL.
  - It answers the flat API itself: init, shutdown, `RunCallbacks`, `IsSteamRunning` (asked of the broker), callback
    registration, the user and pipe handles, and `SteamInternal_ContextInit` (the inline accessors' cache).
  - The **bridge build** of the stand-in (`steam_api64_bridge.dll`, phase 3) has no forwarders to Valve's DLL: an export
    with no bridged implementation logs once and fails, and a game-server interface is NULL.
  - It hands out generated proxy objects for each interface the game requests (phase 3). Each vtable slot marshals its
    call to the broker.
- **The broker** (`hellforge_steam_broker.exe`, beside the stand-ins) is native C and runs at the player's normal
  integrity, one process per session. The session runner starts it as
  `<pipe suffix> <game pid> <Valve's DLL> <mods folder>`. The mods folder is for Play with strangers
  ("The game's own Steam lobby" below).
  - It creates the pipe and ring first, then loads Valve's `steam_api64.dll` (the game's pinned copy) as app 553850
    (`SteamAppId` in its environment). It never calls `SteamAPI_RestartAppIfNecessary`, which may start the installed
    game. It then calls `SteamAPI_Init` (`SteamAPI_InitFlat` with a newer SDK) and manual dispatch.
  - It serves one client: the game process it was started for. A client whose pipe process id differs from its own
    HELLO or from the game's, or that says nothing for 5 s, is refused and disconnected, and the broker waits on.
  - It watches the game's process, and exits when the game does, even while it waits for the connection.
  - One thread makes every Steam call: it answers calls, and when none comes for 16 ms it runs manual dispatch.
  - It makes allowed calls through Valve's flat C API, so the broker itself needs no C++ ABI.
- **Calls** go over a duplex pipe, one request then its reply, serialized by a lock in the stand-in; each carries a
  sequence number its reply must echo. A call too short or too large is answered as malformed; a broken pipe marks
  the channel dead, and every later call fails at once.
  - The pipe's security descriptor grants the player's own SID read and write only, and carries a **low** mandatory
    label (no write up), so the game can open it. It is the first and only instance of its name and refuses remote
    clients, so nothing that made the name first can stand in for the broker.
  - The pipe name is the session's id. Any low-integrity process of the player could open it; the process id check
    is what keeps them out.
- **Callbacks** are pushed by the broker.
  - The broker writes every callback and call result (`SteamAPICallCompleted_t`, then `GetAPICallResult`) into a
    shared-memory ring: an unnamed section the broker duplicates into the checked game process only.
  - The game can write the ring, so each side keeps its own position and only publishes it there. The broker clamps
    the game's read position before it uses it, and a slot index is always an unsigned position modulo 60. Callbacks
    that find the ring full are counted and logged.
  - The stand-in's `RunCallbacks` reads the ring and calls the game's registered `CCallbackBase` objects (phase 3:
    until then they are read and dropped).
  - An idle frame is one read of the ring's sequence number: no system call.
- **Steam Input,** which the game polls every frame, is answered from a shared snapshot the broker refreshes. This is
  done once the census shows which calls happen per frame; see "Cost".

### The allow-list (the security boundary)

The bridge must not become the way out of the sandbox. Every method of the 7 bridged interfaces is classed, by review,
as one of:

- **allowed;**
- **allowed with checks** (sizes, ranges, the strings a key may carry);
- **refused** (the broker answers as Steam would on failure).

Interfaces the game never requests are refused whole: `ISteamHTTP`, `ISteamUGC` and so on. `ISteamMatchmaking` is
not: the game makes a Steam lobby of its own (census step 2).

Methods that carry data to other people or out of the PC, which are refused unless the census shows the game needs them,
and then checked:

- `ISteamFriends`: `SetRichPresence` (strings friends see), `ReplyToFriendMessage`, `InviteUserToGame`,
  `ActivateGameOverlayToWebPage` (it may open the player's browser), `SetPersonaName`;
- `ISteamNetworking`: `SendP2PPacket` to any SteamID. Co-op uses it (the host sends to a joiner when it joins), so it
  passes only to the game's co-op peers ("Steam P2P" below).
- `ISteamUserStats`: leaderboard uploads.

The broker logs every refusal.

### Where the definitions come from

The proxies and the broker's dispatch are generated by `HellForge.SdkShimGen` from Valve's Steamworks SDK 1.55 (the one
whose DLL is the game's pinned copy) and the game's own `steam_api64.dll`, for the MSVC x64 ABI. The details are in
[the Steam bridge reference](steam-bridge-reference.md#where-the-definitions-come-from).

### Phase 3: the generated bridge

**Inputs:** the pinned `steam_api.json` and `steam_api64.dll`, and `native/steam-cloud/bridge-methods.json`, the
reviewed table. A method that is not in the table is refused. `HellForge.SdkShimGen steam-bridge` writes, into the
build's `gen` folder:

- `bridge_gen.h`: the call-mode methods' numbers, every method's slot, and the prototypes of the hand-written methods
  (so `bridge_local.c` fails to compile if one is wrong);
- `bridge_proxies_gen.c`: the game's side, a vtable per bridged interface;
- `broker_dispatch_gen.c`: the broker's side, each call-mode method's arguments read with bounds, checked, and passed to
  Valve's flat export;
- `bridge_exports_gen.c` and `steam_api64_bridge.def`: the **bridge build** of the stand-in (`steam_api64_bridge.dll`).
  Every export it does not write itself is a stub that refuses: no export forwards, so Valve's DLL never loads in the
  game. (A forwarder loads it as soon as anything resolves that export, whatever the session's mode, and the game's
  protection resolves the flat exports itself.)

Generation fails on:

- anything it cannot marshal exactly;
- an enum whose 0 may mean success without a refusal value (`EUserHasLicenseForAppResult` is 0 for "has a license");
- a table entry the SDK does not declare.

**Each method's mode, in the table:**

| Mode | How the method is answered |
| --- | --- |
| hand | In the game by `bridge_local.c`, without the broker. `ReadP2PPacket` and `IsP2PPacketAvailable` answer from the packets the broker put in the ring ("Steam P2P" below). Fixed answers: `GetAppID` is 553850, and the overlay calls answer "no overlay" (it lives in the broker). The rest come from the snapshot (below) |
| interface | `ISteamClient`'s `GetISteam*` methods hand out the bridge's own objects for the version asked |
| call | One pipe round trip. The broker checks the arguments as the table says (string lengths, printable strings, buffer sizes, integer ranges, lobbies the game made this session) before it calls the flat export |
| refuse | As Steam does on failure (false, 0, `k_uAPICallInvalid`, an empty string, an invalid CSteamID), logged once |

The **snapshot** is filled by the broker before the game connects and refreshed about once a second. It holds
`BLoggedOn`, `GetSteamID`, `GetPersonaName`, the language, the IP country, the DLC list, `BIsSubscribedApp` for the game
and its DLC, the beta name and Steam Deck. It is a seqlock: the game copies what it needs and retries if the broker was
writing, with no system call.

**Kinds marshalled**, from the JSON's types. The table names what the JSON leaves out: which `void *` is an output
buffer and which parameter is its size.

| Kind | How |
| --- | --- |
| Scalars and enums | Supported |
| `CSteamID` | By value (64 bits), and returned through the hidden pointer in `RDX` |
| Input strings | Bounded and NUL-terminated |
| Output buffers | With their size, at most what fits one message (larger is refused) |
| Out scalars and out CSteamIDs | Supported; an out struct leaves the method refused until the table needs one |
| A returned `const char *` | Copied into a buffer of that method's in the stand-in, valid until its next call |
| `SteamAPICall_t` | When Steam's `SteamAPICallCompleted_t` arrives, the broker takes `GetAPICallResult` and pushes the result to the ring behind a call-result header (the call, the I/O failure flag). A `LobbyCreated_t` that succeeded names a lobby the game may then use |

**Callbacks:** the stand-in keeps the game's registrations itself (`RegisterCallback`, `RegisterCallResult` and their
unregisters), and marks the object registered with its id as Valve's DLL does. `RunCallbacks` takes each ring entry and
calls slot 0 of each registered `CCallbackBase`. The struct passed is at least `GetCallbackSizeBytes` long
(zero-padded). The broker passes on only the callback ids in the table, and the game's side re-checks each object is
still registered before calling it.

MSVC orders the two `Run` overloads in reverse, so slot 0 should be `Run(pvParam, bIOFailure, hSteamAPICall)`. A
`CCallback`'s forwards to `Run(pvParam)`, and a `CCallResult`'s checks the call. If the game's compiler did not reverse
them, slot 0 is `Run(pvParam)` and only a call result's failure flag is lost. To confirm in game.

**The bridge build's own exports** (`bridge_exports.c`):

- `SteamInternal_ContextInit`: the game's inline accessors, a compare and a return on each of its 73,040 calls in the
  census;
- `SteamInternal_CreateInterface` (`SteamClient020`): a proxy;
- the game-server interfaces: NULL, as Valve's answer without `SteamGameServer_Init`. The game checks for it, and a
  null object in their place crashed it on the title screen in the first sandboxed run;
- any version the table does not know: a null object whose every slot answers 0, logged;
- `SteamAPI_UnregisterCallResult`.

The forwarding build refuses to start a session that asks for the bridge, rather than run on Valve's DLL.

**The first table** is the census's named calls ([reference](steam-bridge-reference.md#step-2-results-2026-10-23)):

| Interface | Method | Mode | Checks, and why |
| --- | --- | --- | --- |
| `SteamNetworking006` | `ReadP2PPacket`, `IsP2PPacketAvailable` | local | the packets the broker read from the game's co-op peers, through the ring |
| `SteamNetworking006` | `SendP2PPacket`, `AcceptP2PSessionWithUser`, `GetP2PSessionState` | call | reviewed 2026-10-24: only a co-op peer the backend lists ("Steam P2P"); a packet at most 7000 bytes, channels 0-7 |
| `SteamNetworking006` | `CloseP2PSessionWithUser`, `CloseP2PChannelWithUser`, `AllowP2PPacketRelay` | call | |
| `STEAMAPPS_INTERFACE_VERSION008` | `GetDLCCount`, `BGetDLCDataByIndex`, `GetCurrentGameLanguage`, `GetCurrentBetaName` | snapshot | name buffers bounded |
| `STEAMAPPS_INTERFACE_VERSION008` | `BIsSubscribedApp` | snapshot, else call | the DLC ids and the game from the snapshot |
| `SteamUser021` | `BLoggedOn`, `GetSteamID` | snapshot | |
| `SteamUser021` | `GetAuthSessionTicket`, `CancelAuthTicket` | call | ticket buffer at most 1024 bytes |
| `SteamFriends017` | `GetPersonaName` | snapshot | |
| `SteamFriends017` | `GetFriendCount`, `GetFriendByIndex`, `GetFriendRelationship`, `GetUserRestrictions`, `GetFriendPersonaState`, `GetFriendPersonaName`, `GetFriendGamePlayed` (its `FriendGameInfo_t`, 24 bytes out) | call | read only; the last three since 0.3.0-dev.10: the social menu showed every friend offline without them |
| `SteamFriends017` | `SetRichPresence`, `ClearRichPresence` | call | reviewed 2026-10-24: strings friends see. Key at most 64, value at most 256 printable characters, every key logged. The key `hellforge` (HellForge's own, how friends' backends find this one) refused in any case |
| `SteamUtils010` | `GetAppID` | local | 553850 |
| `SteamUtils010` | `GetIPCountry`, `IsSteamRunningOnSteamDeck` | snapshot | |
| `SteamUtils010` | `SetOverlayNotificationPosition`, `IsOverlayEnabled` | local | the overlay is in the broker: nothing to place, none shown |
| `SteamUtils010` | `InitFilterText`, `FilterText` | call | input at most 4096 bytes, output buffer bounded |
| `SteamMatchMaking009` | `CreateLobby` | call | reviewed 2026-10-24: strangers can read a public lobby's data. At most 8 members, for mods that take eight players. The type is logged, and Public becomes Friends only while Play with strangers is off (below). Whether it can be Friends only always is the two-PC stranger test's question |
| `SteamMatchMaking009` | `JoinLobby` | call | reviewed 2026-10-24: any lobby by id, as the game joins the host's own Steam lobby in a co-op join; that lobby learns this player's SteamID, as without HellForge, and its members are no co-op peers for P2P unless the backend lists them |
| `SteamMatchMaking009` | `SetLobbyData`, `GetLobbyData`, `SetLobbyJoinable`, `LeaveLobby`, `GetNumLobbyMembers`, `GetLobbyMemberByIndex`, `GetLobbyOwner` | call | only a lobby the game made or entered this session (`LobbyCreated_t`, or a `LobbyEnter_t` that says it entered); key at most 255, value at most 8192 bytes |
| `STEAMUSERSTATS_INTERFACE_VERSION012` | `RequestCurrentStats`, `GetAchievement` | call | name at most 128 |
| `SteamInput006` | `GetControllerForGamepadIndex` | local | 0, "not one of Steam Input's pads": Steam's emulated pads are the broker's ("Controllers and Steam Input"); a call in 0.3.0-beta |
| `SteamClient020` | `GetISteam*`; the pipe and user calls | interface; local | the bridge's own objects; one pipe, one user |

In the table, local and snapshot are both mode `hand`. Callbacks: the 16 ids the census saw, and 504 (`LobbyEnter_t`)
from the co-op census. 30 methods are calls, 48 are answered in the game, and the other 289 the nine interfaces declare
are refused (367 in all, as the first table's 20, 47 and 300). What each mode costs a frame is under [Cost](#cost).

### The game's own Steam lobby

The game makes a Steam lobby of its own beside its PlayFab one. A Public one is in every Steam search of the app
(probed 2026-10-22), with its data:

- the PlayFab connection string (key `"0"`), so the lobby id;
- the build string vanilla games share (`"1"`);
- the host's SteamID (`"3"`);
- the host's coordinates ×100 (`"22"`, `"23"`).

HellForge's public lobbies go through its own directory instead ([multiplayer.md](multiplayer.md), step 7).

The census saw `CreateLobby`, `SetLobbyData`, `SetLobbyJoinable` and `LeaveLobby`, never `SetLobbyType`, so the
lobby's type is the one asked for at creation. The table marks `eLobbyType` for the broker to adjust (`adjust` in
`bridge-methods.json`, `BrokerApi.adjust`, run once every check of the table passed):

- **Play with strangers off:** a Public lobby (`2`) is made Friends only (`1`). Friends and invitees
  can still join it, and it is in no search. Strangers could reach no one anyway, so nothing is lost.
- **Play with strangers on:** the type is left as the game asked. A stranger joining through the directory may join
  the host's Steam lobby too (a joiner posts it as its `platform_lobby`), which Friends only would refuse. The two-PC
  test settles it: if a stranger's join still works, it can be Friends only always. That test is
  [multiplayer.md](multiplayer.md) step 7, item 7, with `HELLFORGE_STEAM_LOBBY_FRIENDS_ONLY=1` on the host, which
  keeps it Friends only with strangers on.
- Every other type is left as asked, and every request is logged: `broker: CreateLobby asks lobby type 2; Steam gets
  1 (Play with strangers off)`.
- The broker reads the setting as the backend does: `mods/hellforge-mods.json` `"strangers": "on"`/`"off"`, top level
  only. A legacy `playWithStrangers` true or false still counts. Without a choice it is on, because the broker runs
  only in a sandboxed session. It looks at the file's time stamp every 2 s, in the broker's process. When the setting
  changes, each lobby the game asked to be Public gets `SetLobbyType`, Public or Friends only.
- `bridge_test.c` checks it ([tests](steam-bridge-reference.md#tests)).

Sessions that are not sandboxed load Valve's DLL in the game, so this does not apply to them; there the game's Public
lobby stays listed.

### Steam P2P

Co-op uses the old `ISteamNetworking`: the host sends to a joiner when it joins (2026-10-24). Through the bridge it
reaches only the game's **co-op peers**, so the game has no way to send to any other Steam user:

- **A co-op peer** is a SteamID the backend lists (`P2PPeer`). A member of a lobby the game made or entered is not
  enough (reviewed 2026-10-24): `JoinLobby` takes any lobby id, so a compromised game could enter any lobby it knows of
  and reach its members.
  - The backend writes its lobby and Party peers to `run/backend/p2p-peers.txt` (`MultiplayerHost.PeersFileName`), at
    once when a lobby changes or a join starts, and on each heartbeat. The low-integrity game cannot write that folder.
  - A friend admitted to a lobby hosted here, and the host of a lobby this game joins, are written in **before** the
    game hears of them (`LobbyService.PeerAdmitted`, `MultiplayerHost.ListPeer`; 2026-10-24). The host's game sends the
    moment it hears of a joiner, so the file must already list it; before, the file was written a few milliseconds
    after the announcement. A peer listed that way stays in the file until the lobby or Party lists it too (at most
    30 s), and a kick takes it out at once.
  - A joiner also lists the lobby's **other members** (`LobbyService.LobbyMates`). These are the members of the host's
    join answer, then each one the host reports added, written in before the game hears of them (`MateAdded`). Each is
    taken out at once when the host reports them gone or this game leaves the lobby (`MateRemoved`).
  - The list comes from the host of a lobby this game is in, at most 8 a lobby, never this machine or the host itself.
    The joiners' backends still never talk: these peers are not pinged. Before (0.3.0-beta), a joiner listed only the
    host, so in a squad of three or four the joiners' games could not reach each other.
  - The broker reads the file when its time stamp moves, and again at once when a SteamID is not in it. A host sends
    within a second of a join, seconds before the joiner enters its Steam lobby.
- **Out:** `SendP2PPacket` (at most 7000 bytes, channels 0-7), `AcceptP2PSessionWithUser` and `GetP2PSessionState` to a
  peer only; closing a session or channel and `AllowP2PPacketRelay` to anyone.
- **In:** the broker reads channels 0-7 every pump, up to 32 packets a channel. A packet from a peer goes into the ring
  under `BRIDGE_P2P_PACKET`, in pieces of up to 996 bytes. Packets from anyone else, or over 8192 bytes, are read and
  dropped, so they never pile up in Steam. The stand-in puts each whole packet in a queue of 32 (a static array, no
  allocation). `ReadP2PPacket` and `IsP2PPacketAvailable` answer from the queue with no system call. A full ring or
  queue loses the packet, and it is logged.
- **A squadmate the game cannot reach** (static reading of the game's 2026-10-30 memory dumps, not confirmed live):
  - Co-op is a full mesh. Each game opens its own Steam P2P session to every other player, not only to the host.
  - A session that never becomes active (`GetP2PSessionState`, which the bridge refuses for a SteamID not listed) gets
    no PlayFab fallback. After at least 45 s without a pong, the connection fails with `PongTimeout` ("Pong timeout for
    %llx", exe `0x2A5D0A`). Only a session that was active once falls back to PlayFab Party on a pong timeout
    ("Platform pong timeout ... Falling back to playfab").
  - The other way to the fallback is Steam's `P2PSessionConnectFail_t` (1203) for that player (exe `0x751B80`). The
    next send to them fails, and the game falls back to PlayFab Party (exe `0x2A9A9B`). It tells the other game, which
    falls back too (a packet whose first byte is 5, exe `0x2A6BA2`). HellForge's Party relays through the host's
    backend, so the pair keeps playing.
  - A refused `SendP2PPacket` never reaches Steam, so a game whose own bridge refused the send never gets that
    callback. A game outside the sandbox should get it (Steam's timeout error, "target isn't responding, perhaps not
    calling AcceptP2PSessionWithUser()"), since the sandboxed game's bridge refuses to accept its session. That fits
    #202's report that unsandboxing one player let everyone join, if that player was one of the two joiners.
  - The failure is reported to the host's game session. 20 s after the first report, it votes one of the pair out (exe
    `0x296AB0`): a player accused twice, else the one of the two that joined the session first. The host then removes
    that player (game.dll `0x108A9CB`, "Peer %llx was voted out").
  - With the joiners not listed (0.3.0-beta), this is issue #202. The third player waits. About a minute later the
    second player is pushed out, and then the third gets in. When the second rejoins, the third is pushed out.
- Tests: `bridge_test.c`, `smoke-lobby-link` and `SquadTests` ([what they check](steam-bridge-reference.md#tests)).
- Unmeasured: how much P2P co-op sends, and whether 8192 bytes and 60 ring slots are enough for it.

### The rest of a low-integrity session

- **The game process:** the session runner starts it with a lowered copy of its own token (`CreateProcessAsUser`; no
  admin needed). Its job adds an active-process limit of 1, so no child processes. That loses the crash handler, and
  NVIDIA's DLSS updater that Steam's overlay starts.
- **The broker's start:** the game is created suspended. The runner resumes it only once the broker's log says
  `broker: waiting for the game`: Steam started in the broker, the snapshot filled and the pipe open
  (`PlaySession.WaitForBroker`, `SteamBrokerLog`).
  - A broker that ends first is the session's failure, with its reason. The reason is Steam (not running or signed in,
    or run as administrator), Valve's DLL or the channel. A broker not ready in 60 s is a failure too.
  - The session log says it is waiting after 3 s.
  - A broker that ends while the game runs, other than with the game (its `SteamAPI_Shutdown` or exit), is said in the
    session log once and recorded (`brokerEndedDuringPlay`).
  - `brokerReadyMilliseconds` and `brokerGameConnected` are recorded for every sandboxed session.
  - Why it waits: before, the broker started after the game and the game resumed at once. Two brokers then left the
    game at STEAM NOT DETECTED while the session said it was running. One was held up more than about 10 s (an
    antivirus checking a new program, say); the other's `SteamAPI_Init` failed.
- **What the sandbox refuses the game** is recorded. In a sandboxed session the network gate's census (`net_gate.c`)
  adds a `denied` group.
  - It hooks kernelbase's `OpenFileMappingW`, `CreateFileMappingW`, `OpenEventW`, `CreateEventW`, `CreateEventExW`,
    `OpenMutexW`, `CreateMutexW`, `CreateMutexExW`, `OpenSemaphoreW`, `CreateFileW` (files and pipes) and
    `WaitNamedPipeW`. The A variants and direct `Nt*` calls are not hooked.
  - A call that fails with `ERROR_ACCESS_DENIED` is written with the object's name and the module that asked
    (`"scope":"denied"`), and the caller's last error is kept.
  - A folder opened as a file and a read-only file opened for writing are left out: Windows refuses those at any level.
  - Overlays reach their own program through such objects, which other programs made at the player's level. So these
    lines say exactly what each one is refused. `result.json` has `networkCensus.denied` and `deniedTargets`.
    `session.log` names each module, by the overlay it is when it is a known one (`SandboxRefusals`).
  - `HELLFORGE_NETGATE_HOOKS` without `denied` leaves it out.
  - The census's hooks also keep the caller's last error now: `TlsGetValue` in their bookkeeping reset it to 0, so a
    failed `connect` read back as no error.
- **The tablet driver is refused** (`tablet_gate.c`): kernelbase's four `LoadLibrary` calls answer `Wintab32.dll` with
  `ERROR_MOD_NOT_FOUND`. At low integrity the driver is refused its shared memory, and Gaomon's then shows a modal
  `OpenMem failed. Err=5` on every alt-tab. The census writes each refusal; `result.json` has `networkCensus.tablet*`.
- **Nahimic crashes a sandboxed game** as it starts (#369). The failure message names it from the census
  (`SandboxRefusals.CrashHint`).
- **Steam's overlay** loads with `SteamAPI_Init`, so only into the broker. `HELLFORGE_SANDBOX_OVERLAY_PROBE=1` is a
  diagnostic: the stand-in loads Steam's `GameOverlayRenderer64.dll` into the sandboxed game at `SteamAPI_Init`
  (`OverlayProbe`). The runner passes its path, from Steam's `SteamPath`, as `HELLFORGE_OVERLAY_PROBE`. The census then
  records what Steam's overlay is refused there; `result.json` has `overlayProbe`.
- **Folders the game writes** are all inside the session's `game/` folder in its run folder, which the runner **makes
  new with a low label** (`LowIntegrityHelper.CreateLowFolder`); everything made in it later inherits the label.
  Labelling at creation, at or below one's own integrity, needs no admin. Relabelling an existing folder needs
  WRITE_OWNER, which a standard account seldom has on its own folders: the first in-game try stopped there
  (2026-10-23). The folder holds:
  - the mod loader's, the stand-ins' and the census's logs, the stats snapshot, the editor's saves, and the items mods
    registered (`registered-items.json`, which the backend reads from here when it runs with `--sandbox`);
  - `environment/`, a copy of `userdata/environment` (the game's AppData and temp);
  - `userdata/`, the user-data root `data/settings.ini` points at for the session (`prepare-session --user-data`): a
    copy of the profile's folder (settings, saves, the cloud folder), the shader cache (110 MB on the test PC) and the
    ImGui settings. The game's logs, dumps and crash data land there too and stay with the session.

  Both copies go back once the game is gone (`SandboxFolders`), within limits:
  - regular files only, never through a link or junction (a low process can make one in its own folder);
  - at most 4 GiB and 100,000 files.

  The game writes its shader cache only on a clean exit. So a stop the player asked for first posts WM_CLOSE to the
  game's windows and waits up to 30 s (`GameClose`). An installation without a shader cache starts from the normal
  game's (`ShaderCache.Seed`).

  The rest of the run folder stays at the player's level. The runner's result, the backend's evidence and the
  `steam_api64.dll` copy the backend loads for the peer link must stay out of the game's reach.
- **Pipes the game opens** carry a low label when the backend runs with `--sandbox` (`PipeServer`): the lobby and party
  stand-ins' pipe and the live pipe. Only the player's account may open them, from this PC only.
- **The in-game editor** cannot write into `mods/` (Local Stats, its settings, its presets). A low-writable `mods/`
  would let a compromised game plant files for the next session.
  - Sandboxed, each save writes the session's saved changes as a list (`game/sandbox-stats/changes.json`), and its
    settings and exported presets beside it.
  - Once the game is gone the runner applies them (`SandboxStats`). Local Stats changes go onto the file as it is then
    (the launcher may have saved meanwhile), under the launcher's lock. Each is kept only if `StatsData.Parse` accepts
    it.
  - Mod content values, settings and presets are rebuilt by System.Text.Json within size limits.
  - A save from the live pipe was the first design. The loader's message buffer is 1 KiB and the editor has no API to
    it.
- **The mods hold** (lockdown item 4) still applies: what appeared in `mods/` during the session is reported and the
  mods it appeared in switched off, before the editor's saves are applied.

### What it does not stop

- **Reading the player's files.** Low integrity blocks writes, not reads; data read can only leave through the
  channels below.
- **Traffic to this PC:** the backend's HTTP on loopback, and its pipes. The backend treats everything from the game
  as untrusted, as it does today.
- **Steam's overlay.** It is loaded by `SteamAPI_Init` in the process that calls it, which is now the broker, so the
  game has no overlay: no chat, invite dialog or screenshots. The invite dialog's callbacks are still bridged. Steam
  Input's controllers come with the overlay, so the game has none of them either (below).

### Controllers and Steam Input

**What players saw (#94, #204, 0.3.0-beta):** with the sandbox on, a Steam Controller did nothing in the game while
keyboard and mouse worked, and with the sandbox off it worked again. Through a non-Steam shortcut with Steam Input on,
its A button opened the chat and the game's controller page showed no bindings. An Xbox controller worked.

**Why (from the code and the reports; not reproduced on a PC with these controllers):**

- The game reads controllers itself, through XInput (and its own PlayStation support, as far as is known). Of Steam
  Input it calls only `GetControllerForGamepadIndex`, four times at start (census step 2), and the stand-in's log of
  #94's sandboxed session refuses no Steam Input call. There is no Steam Input traffic to forward.
- A controller that Steam Input handles (a Steam Controller, the Steam Deck's controls, a PlayStation or Switch
  controller with Steam Input on) reaches such a game as an XInput pad Steam emulates. That pad exists only in a
  process Steam's overlay is in, which hooks XInput there. Sandboxed, the overlay is in the broker (above), so Steam's
  pad is the broker's and the game gets none.
- Steam still counts the game as running (the broker is app 553850), so Steam Input keeps the controller. The game's
  window is no window of the process Steam knows, so Steam can treat it as the desktop and send its desktop layout: A
  presses Enter, which opens the chat.
- A controller the game reads itself reaches it as before: an Xbox controller through XInput. Whether a low-integrity
  process may open a PlayStation controller's HID device for the game's own support (which writes to it) is untested.

**What would not do:**

- The overlay in the game: it needs Steam's IPC, which a low-integrity process cannot use, the reason the game alone
  stops at STEAM NOT DETECTED ("Why"). It would take raising the game's integrity or loosening Steam's objects.
- Forwarding `ISteamInput`: the game makes no such calls.
- Starting the game unsandboxed by itself when Steam Input has a controller: the sandbox lost without the player's
  choice. The player can turn it off (Settings > Sandbox).

**What a sandboxed session does:**

- The broker asks Steam which controllers Steam Input handles (`broker_input.c`; `Init` with the broker calling
  `RunFrame`, then every 2 s `GetConnectedControllers`, `GetInputTypeForHandle` and `GetGamepadIndexForController`) and
  logs the list when it changes: `broker: Steam Input controllers: Steam Controller [1] as XInput pad 0`, or `none`.
- The session reads that line (`SteamInputNote` in HellForge.Core) into result.json's `steamInput` (the controllers'
  names and types, and the note), and the note into its log. The launcher shows it under the status while the game
  runs and in its console; the CLI writes a `steam-input` event. Settings > Sandbox says it beforehand. Xbox and
  PlayStation controllers, which the game may still read itself, get a milder note than the others.
- `GetControllerForGamepadIndex` is answered 0 in the game: with `Init` done in the broker, Steam could name a pad it
  emulates there, which is none of the game's.
- For a relay (below), the broker's log also says:
  - whether the overlay is in the broker;
  - which XInput pad Steam emulates for each controller;
  - once a minute, how often that pad changed in the broker's own XInput (`broker: XInput pad 0 in the broker changed
    37 times in the last minute: ...`), or that XInput there does not show it.
- Tests: `test/input_test.c` and `SteamInputNoteTests` ([what they check](steam-bridge-reference.md#tests)).

**A relay, not built:**

- The broker reads Steam's emulated pad in its own XInput and publishes its state (16 bytes a pad) beside the snapshot.
- The stand-in answers the game's `XInputGetState` and `XInputGetCapabilities` for a free pad from it (a MinHook
  detour, as the network gate's), and `GetControllerForGamepadIndex` names it.
- Data goes only from the broker to the game (rumble back would be two numbers a pad), so nothing new leaves the
  sandbox.
- It works only if the log above shows the pad changing while the player plays the game, and only once Steam's desktop
  layout stays off the game's window.

## The census

The census (`native/steam-cloud/steam_census.c`) logs what the game asks of Steam. Step 1 logs the interfaces, exports
and callbacks it looks up; step 2, the census build, counts every call through slot proxies and export thunks. How it
works, how to build it and what it found are in
[the Steam bridge reference](steam-bridge-reference.md#the-census).

## Cost

Every number here is unmeasured in game until phase 4. Rates are from the census
([reference](steam-bridge-reference.md#step-2-results-2026-10-23)).

| What | Its cost to the game |
| --- | --- |
| An idle frame (`RunCallbacks`, about 400 a second) | One read of the ring's write position, compared with the stand-in's own, and one of the tick count (for the cost log below). 0 system calls, 0 pipe messages, no allocation |
| `ReadP2PPacket` (1.00 per frame) | A slim lock and a look at the packet queue; no system call while uncontended |
| The DLC answers (0.85 per frame) and `BLoggedOn` (0.024 per frame) | A seqlocked read of the snapshot, a few dozen bytes copied; no system call |
| The inline accessors (`SteamInternal_ContextInit`, about 340 a second) | A compare and a return |
| A frame with callbacks | Per callback, one slot copied (at most 1016 bytes) and two memory fences; no system call |
| The broker's pumping | Runs in its own process every 16 ms, not on the game's thread |
| A bridged call | One pipe round trip: two system calls in the game |
| Steam Input with a controller | Read from the snapshot, with no system call per call; the snapshot's per-frame cost is to be measured |

None of the calls the census saw every frame makes a system call, so an idle frame stays at the ring read. The bridged
calls the census saw:

- `SteamAPI_IsSteamRunning`: 2,014 in the census's 214 s, about 9 a second;
- the lobby queries: about 7 a second each (about 0.02 per frame);
- the rest: once or on events.

The bridge build measures itself. Each minute, and at `SteamAPI_Shutdown`, the stand-in's log (`HELLFORGE_CLOUD_LOG`)
gets two lines:

- `bridge cost, the last minute: <frames> frames; <n> round trips, <ms> in all, longest <ms>; <n> callbacks in <n>
  frames, <ms> in all, longest frame <ms>`. A round trip's time runs from the call to its reply, the lock's wait
  included, on whichever game thread made it.
- `bridge cost, the last minute, by function (id, round trips, time): ...`: the five functions whose round trips took
  longest. Id 0 is `SteamAPI_IsSteamRunning`, and 100 on are `BRIDGE_M_*` in the generated `bridge_gen.h`. The first
  16 functions called in a minute are counted apart.

## Phases, each tested before the next

| Phase | What it builds | Status | Still to do |
| --- | --- | --- | --- |
| 1. Census step 2 | Slot proxies, export thunks and callback sizes | Built and tested against a fake Steam DLL (2026-10-22); first played run 2026-10-23 (the ship and a mission's start) | A controller, a whole mission, the social tab, and the game-server and `CreateInterface` objects |
| 2. The broker and its channel | The pipe and ring with low labels and the client check. `SteamAPI_Init` in the broker. Manual dispatch into the ring, and the flat API in the stand-in | Built, with fixture tests | |
| 3. Generated proxies | `SdkShimGen steam-bridge`, from the pinned SDK and the game's DLL, with the allow-list as a reviewed table ([above](#phase-3-the-generated-bridge)) | Built 2026-10-23; the table's review items reviewed 2026-10-24 | The next census |
| 4. A low-integrity session | The token and job, labelled folders, pipe labels, and the editor's saves through the backend | Session side built, with fixture tests; one solo run in game (2026-10-23) | In game, solo then co-op on two PCs: a mission, a controller, an invite, and the per-frame cost measured |

The build log of each phase is in [the page before this
rewrite](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/steam-bridge.md#phases-each-tested-before-the-next).

The phases' tests are `broker_test.c` (phase 2) and `bridge_test.c` (phase 3)
([what they check](steam-bridge-reference.md#tests)).

**Phase 4's first solo run** (2026-10-23, engine 0.1.29, one PC):

- The game ran at low integrity (`0x1000`); the broker, the backend and the runner ran at `0x2000`.
- There was no STEAM NOT DETECTED. `SteamAPI_Init` went through the broker, then came Steam login and the backend's
  login, the war and progression, and the title screen: 209 s and a clean stop.
- Three refusals: `ISteamClient::SetWarningMessageHook`, `SteamGameServer_GetHSteamUser` and the `SteamGameServer014`
  interface (NULL).
- It took the two fixes described above: folders made with their label, and NULL game-server interfaces.
