# The Steam bridge: the game at low integrity

**Status: in development** on `port/lockdown-0.3.0`. Built and tested with fixtures: the census, the broker and its
channel (phase 2), the generated bridge with its first allow-list (phase 3), and the session side of phase 4. In game,
one sandboxed solo session reached the title screen (2026-10-23, phase 4 below); a mission, a controller and co-op are
untested. The allow-list covers the calls one census run saw, and the next census (a controller, a whole mission, the
social tab) will show what it misses. Since the 0.3.0 beta a session is sandboxed by default: the launcher's Settings >
Sandbox, "Run the game sandboxed", is on unless turned off (`launcher-settings.json` `sandboxed`; the earlier betas'
`sandbox` key is not read, so every installation starts sandboxed once). It is offered only by an engine built with the
Steam bridge and only on Windows (`SessionRunner.CanSandbox`; under Wine/Proton the network namespace is the block and
the sandbox is untested). `HellForge.Cli play --sandbox yes|no` or `settings --sandbox yes|no`; `HellForge.Session play
--sandbox`; or `HELLFORGE_SANDBOX=1`. Only an engine built with the Steamworks SDK has the bridge. An installation that
still has the older Windows Firewall rules gets the WFP block placed by its first sandboxed session, with one Windows
prompt (Windows Firewall never filters loopback, so those rules cannot keep the game from the PC's other programs).

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

```
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
    It waits up to 10 s for the broker's pipe. Without a broker it **fails closed**: init returns false,
    `IsSteamRunning` 0, and nothing falls back to Valve's DLL.
  - It answers the flat API itself: init, shutdown, `RunCallbacks`, `IsSteamRunning` (asked of the broker), callback
    registration, the user and pipe handles, and `SteamInternal_ContextInit` (the inline accessors' cache).
  - The **bridge build** of the stand-in (`steam_api64_bridge.dll`, phase 3) has no forwarders to Valve's DLL: an export
    with no bridged implementation logs once and fails, and a game-server interface is NULL.
  - It hands out generated proxy objects for each interface the game requests (phase 3). Each vtable slot marshals its
    call to the broker.
- **The broker** (`hellforge_steam_broker.exe`, beside the stand-ins) is native C and runs at the player's normal
  integrity: one process per session, started by the session runner as `<pipe suffix> <game pid> <Valve's DLL> <mods
  folder>` (the mods folder for Play with strangers: "The game's own Steam lobby" below).
  - It creates the pipe and ring first, then loads Valve's `steam_api64.dll` (the game's pinned copy) as app 553850
    (`SteamAppId` in its environment; it never calls `SteamAPI_RestartAppIfNecessary`, which may start the installed
    game) and calls `SteamAPI_Init` (`SteamAPI_InitFlat` with a newer SDK) and manual dispatch.
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

The proxies and the broker's dispatch are **generated** by `HellForge.SdkShimGen`, as the PlayFab stand-ins are:
- **Methods and parameter kinds** come from Valve's Steamworks SDK (`steam_api.json`), a version whose
  `redistributable_bin/win64/steam_api64.dll` matches the game's pinned copy byte for byte: **SDK 1.55** (1.57's does
  not match). It is pinned by SHA-256 in the gitignored `tools/steamworks-sdk-1.55`; nothing from it is committed or
  shipped. Its 921 flat method names are exactly the pinned DLL's 921 `SteamAPI_ISteam*` exports, and its interface
  versions are the ones the census saw.
- **Vtable slots** come from the game's `steam_api64.dll` itself. Each flat export is a thunk through its interface's
  vtable (`jmp [rax+d]`, `mov r10,[rax+d]; jmp r10`, or `mov r10,[rcx]; jmp [r10+d]`), and the generator reads the
  slot from its code. Generation fails when a thunk does not decode or two methods share a slot. The slots are not the
  declaration order everywhere: `ISteamUtils` has two private virtual methods the flat API leaves out (slots 7 and 14),
  and MSVC orders overloads in reverse (`ISteamUserStats`' six overload pairs).
- **The ABI is MSVC x64.** `this` is in `RCX`. A method returning a class type (`CSteamID`, for example) returns it
  through a hidden pointer in `RDX`. The proxies are C functions written with that pointer explicit.

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

Generation fails on anything it cannot marshal exactly, on an enum whose 0 may mean success without a refusal value
(`EUserHasLicenseForAppResult` is 0 for "has a license"), and on a table entry the SDK does not declare.

**Each method's mode, in the table:**
- **hand:** answered in the game by `bridge_local.c`, without the broker:
  - `ReadP2PPacket` and `IsP2PPacketAvailable` answer from the packets the broker put in the ring ("Steam P2P"
    below); fixed: `GetAppID` is 553850, the overlay calls answer "no overlay" (it lives in the broker);
  - from the **snapshot**, which the broker fills before the game connects and refreshes about once a second:
    `BLoggedOn`, `GetSteamID`, `GetPersonaName`, the language, the IP country, the DLC list, `BIsSubscribedApp` for the
    game and its DLC, the beta name, Steam Deck. A seqlock: the game copies what it needs and retries if the broker was
    writing; no system call.
- **interface:** `ISteamClient`'s `GetISteam*` methods hand out the bridge's own objects for the version asked.
- **call:** one pipe round trip. The broker checks the arguments as the table says (string lengths, printable strings,
  buffer sizes, integer ranges, lobbies the game made this session) before it calls the flat export.
- **refuse:** answers as Steam does on failure (false, 0, `k_uAPICallInvalid`, an empty string, an invalid CSteamID),
  logged once.

**Kinds marshalled**, from the JSON's types (the table names what the JSON leaves out: which `void *` is an output buffer
and which parameter is its size):
- scalars and enums; `CSteamID` by value (64 bits), and returned through the hidden pointer in `RDX`;
- input strings, bounded and NUL-terminated; output buffers with their size, at most what fits one message (larger is
  refused);
- out scalars and out CSteamIDs (an out struct leaves the method refused until the table needs one);
- a returned `const char *`: copied into a buffer of that method's in the stand-in, valid until its next call;
- `SteamAPICall_t`: when Steam's `SteamAPICallCompleted_t` arrives, the broker takes `GetAPICallResult` and pushes the
  result to the ring behind a call-result header (the call, the I/O failure flag). A `LobbyCreated_t` that succeeded
  names a lobby the game may then use.

**Callbacks:** the stand-in keeps the game's registrations itself (`RegisterCallback`, `RegisterCallResult` and their
unregisters), and marks the object registered with its id as Valve's DLL does. `RunCallbacks` takes each ring entry and
calls slot 0 of each registered `CCallbackBase`. MSVC orders the two `Run` overloads in reverse, so slot 0 should be
`Run(pvParam, bIOFailure, hSteamAPICall)`: a `CCallback`'s forwards to `Run(pvParam)` and a `CCallResult`'s checks the
call. If the game's compiler did not reverse them, slot 0 is `Run(pvParam)` and only a call result's failure flag is lost.
To confirm in game. The struct passed is at least `GetCallbackSizeBytes` long (zero-padded). The broker passes on only
the callback ids in the table, and the game's side re-checks each object is still registered before calling it.

**The bridge build's own exports** (`bridge_exports.c`): `SteamInternal_ContextInit` (the game's inline accessors: a
compare and a return on each of its 73,040 calls in the census), `SteamInternal_CreateInterface` (`SteamClient020`: a
proxy), the game-server interfaces (NULL, as Valve's answer without `SteamGameServer_Init`: the game checks for it, and
a null object in their place crashed it on the title screen in the first sandboxed run), any version the table does not
know (a null object whose every slot answers 0, logged), and `SteamAPI_UnregisterCallResult`. The forwarding build refuses to start a session that asks for the bridge,
rather than run on Valve's DLL.

**The first table** is the census's named calls (step 2, below):

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
| `SteamFriends017` | `SetRichPresence`, `ClearRichPresence` | call | reviewed 2026-10-24: strings friends see; key at most 64, value at most 256 printable characters, every key logged; the key `hellforge` (HellForge's own, how friends' backends find this one) refused in any case |
| `SteamUtils010` | `GetAppID` | local | 553850 |
| `SteamUtils010` | `GetIPCountry`, `IsSteamRunningOnSteamDeck` | snapshot | |
| `SteamUtils010` | `SetOverlayNotificationPosition`, `IsOverlayEnabled` | local | the overlay is in the broker: nothing to place, none shown |
| `SteamUtils010` | `InitFilterText`, `FilterText` | call | input at most 4096 bytes, output buffer bounded |
| `SteamMatchMaking009` | `CreateLobby` | call | reviewed 2026-10-24: strangers can read a public lobby's data; at most 4 members; the type is logged, and Public becomes Friends only while Play with strangers is off (below); whether it can be Friends only always is the two-PC stranger test's question |
| `SteamMatchMaking009` | `JoinLobby` | call | reviewed 2026-10-24: any lobby by id, as the game joins the host's own Steam lobby in a co-op join; that lobby learns this player's SteamID, as without HellForge, and its members are no co-op peers for P2P unless the backend lists them |
| `SteamMatchMaking009` | `SetLobbyData`, `GetLobbyData`, `SetLobbyJoinable`, `LeaveLobby`, `GetNumLobbyMembers`, `GetLobbyMemberByIndex`, `GetLobbyOwner` | call | only a lobby the game made or entered this session (`LobbyCreated_t`, or a `LobbyEnter_t` that says it entered); key at most 255, value at most 8192 bytes |
| `STEAMUSERSTATS_INTERFACE_VERSION012` | `RequestCurrentStats`, `GetAchievement` | call | name at most 128 |
| `SteamInput006` | `GetControllerForGamepadIndex` | call | index 0-15; per-frame use unknown until a controller census |
| `SteamClient020` | `GetISteam*`; the pipe and user calls | interface; local | the bridge's own objects; one pipe, one user |

In the table, local and snapshot are both mode `hand`. Callbacks: the 16 ids the census saw, and 504 (`LobbyEnter_t`)
from the co-op census. 28 methods are calls, 47 are answered in the game, and the other 292 the nine interfaces declare
are refused.

**Cost per frame, from the census's rates (unmeasured in game):** `ReadP2PPacket` (1.00 per frame) takes a slim lock and looks at the packet queue (no system call while uncontended); the DLC
answers (0.85) and `BLoggedOn` (0.024) are memory reads. None makes a system call, so an idle frame stays at the ring
read. The lobby queries (about 0.02 per frame each) and everything else are a round trip each, two system calls in the
game; the census saw them on events only.

### The game's own Steam lobby

The game makes a Steam lobby of its own beside its PlayFab one. A Public one is in every Steam search of the app
(probed 2026-10-22), with its data:
- the PlayFab connection string (key `"0"`), so the lobby id;
- the build string vanilla games share (`"1"`);
- the host's SteamID (`"3"`);
- the host's coordinates ×100 (`"22"`, `"23"`).

HellForge's public lobbies go through its own directory instead (docs/multiplayer.md, step 7).

The census saw `CreateLobby`, `SetLobbyData`, `SetLobbyJoinable` and `LeaveLobby`, never `SetLobbyType`, so the
lobby's type is the one asked for at creation. The table marks `eLobbyType` for the broker to adjust (`adjust` in
`bridge-methods.json`, `BrokerApi.adjust`, run once every check of the table passed):
- **Play with strangers off** (the default): a Public lobby (`2`) is made Friends only (`1`). Friends and invitees
  can still join it, and it is in no search. Strangers could reach no one anyway, so nothing is lost.
- **Play with strangers on:** the type is left as the game asked. A stranger joining through the directory may join
  the host's Steam lobby too (a joiner posts it as its `platform_lobby`), which Friends only would refuse. To settle
  in the two-PC test (docs/multiplayer.md step 7, item 7, with `HELLFORGE_STEAM_LOBBY_FRIENDS_ONLY=1` on the host,
  which keeps it Friends only with strangers on): if a stranger's join still works, it can be Friends only always.
- Every other type is left as asked, and every request is logged: `broker: CreateLobby asks lobby type 2; Steam gets
  1 (Play with strangers off)`.
- The broker reads the setting as the backend does (`mods/hellforge-mods.json` `playWithStrangers`, top level only, off
  without the file). It looks at the file's time stamp every 2 s, in the broker's process. When the setting changes,
  each lobby the game asked to be Public gets `SetLobbyType`, Public or Friends only.
- `bridge_test.c` checks the clamp, a nested key that is not the setting, both changes, the test switch and a private
  lobby left alone.

Sessions that are not sandboxed load Valve's DLL in the game, so this does not apply to them; there the game's Public
lobby stays listed.

### The rest of a low-integrity session

- **The game process:** the session runner starts it with a lowered copy of its own token (`CreateProcessAsUser`; no
  admin needed). Its job adds an active-process limit of 1, so no child processes. That loses the crash handler, and
  NVIDIA's DLSS updater that Steam's overlay starts.
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

  Both copies go back once the game is gone (`SandboxFolders`): regular files only, never through a link or junction
  (a low process can make one in its own folder), at most 4 GiB and 100,000 files. The rest of the run folder stays at
  the player's level: the runner's result, the backend's evidence and the `steam_api64.dll` copy the backend loads for
  the peer link must stay out of the game's reach.
- **Pipes the game opens** carry a low label when the backend runs with `--sandbox` (`PipeServer`): the lobby and party
  stand-ins' pipe and the live pipe. Only the player's account may open them, from this PC only.
- **The in-game editor** cannot write into `mods/` (Local Stats, its settings, its presets); a low-writable `mods/`
  would let a compromised game plant files for the next session. Sandboxed, each save writes the session's saved
  changes as a list (`game/sandbox-stats/changes.json`), and its settings and exported presets beside it. Once the game
  is gone the runner applies them (`SandboxStats`): Local Stats changes onto the file as it is then (the launcher may
  have saved meanwhile), under the launcher's lock, each kept only if `StatsData.Parse` accepts it; mod content values,
  settings and presets rebuilt by System.Text.Json within size limits. A save from the live pipe was the first design;
  the loader's message buffer is 1 KiB and the editor has no API to it.
- **The mods hold** (lockdown item 4) still applies: what appeared in `mods/` during the session is reported and the
  mods it appeared in switched off, before the editor's saves are applied.

### What it does not stop

- **Reading the player's files.** Low integrity blocks writes, not reads; data read can only leave through the
  channels below.
- **Traffic to this PC:** the backend's HTTP on loopback, and its pipes. The backend treats everything from the game
  as untrusted, as it does today.
- **Steam's overlay.** It is loaded by `SteamAPI_Init` in the process that calls it, which is now the broker, so the
  game has no overlay: no chat, invite dialog or screenshots. The invite dialog's callbacks are still bridged.

## The census

`native/steam-cloud/steam_census.c`, off unless `HELLFORGE_STEAM_CENSUS_LOG` names a file.

**Step 1** logs each interface version the game requests, each stand-in export it resolves by name, and each callback
id it registers.

Results (2026-10-22, one session from start to the title screen, the game connected to the backend): **8 interfaces**,
each requested once.

| Interface | Version | What the game needs it for, likely |
| --- | --- | --- |
| `ISteamUtils` | `SteamUtils010` | app id, overlay state, the gamepad text box |
| `ISteamRemoteStorage` | `016` | the cloud save; HellForge's stand-in already answers it locally |
| `ISteamUserStats` | `012` | stats and achievements |
| `ISteamApps` | `008` | ownership, DLC, language |
| `ISteamInput` | `SteamInput006` | controllers: called every frame while a controller is used |
| `ISteamUser` | `SteamUser021` | SteamID, auth tickets |
| `ISteamNetworking` | `SteamNetworking006` | the old P2P API |
| `ISteamFriends` | `SteamFriends017` | names, overlay, invites, rich presence |

**17 callback ids,** decoded by Steamworks' numbering; to be confirmed against the headers:

| Area | Callbacks |
| --- | --- |
| user | 143 `ValidateAuthTicketResponse`, 152 `MicroTxnAuthorizationResponse`, 163 `GetAuthSessionTicketResponse` |
| friends | 331 `GameOverlayActivated`, 333 `GameLobbyJoinRequested`, 337 `GameRichPresenceJoinRequested` |
| matchmaking | 505 `LobbyDataUpdate` |
| utils | 714 `GamepadTextInputDismissed` |
| apps | 1005 `DlcInstalled` |
| user stats | 1101 `UserStatsReceived`, 1102 `UserStatsStored`, 1103 `UserAchievementStored` |
| networking | 1202 `P2PSessionRequest`, 1203 `P2PSessionConnectFail`, 1221 `SteamNetConnectionStatusChanged` |

**Flat functions: not seen by name, but the hook could not see them.** Step 1 hooked kernelbase's `GetProcAddress`.
kernel32's `GetProcAddress` jumps to kernelbase's `GetProcAddressForCaller` instead, so lookups through kernel32 never
reached the hook (the census test showed it). The hook is now on `GetProcAddressForCaller`, and step 2 counts every
export the game calls whichever way it found it.

**Step 2** (the census build) needs no definitions:
- **Generic slot proxies:** each interface the game gets is a proxy whose 192 vtable slots count their calls, then jump
  to Valve's object's slot with the real object in `RCX` and every other register untouched.
- **A counting thunk per export:** every function export the stand-in does not write itself counts its calls and jumps
  to Valve's export. Data exports still forward.
- **Callback struct sizes** come from each registered callback's `GetCallbackSizeBytes`, as a check on the generated
  layouts.
- **A report every 15 s** of each count that changed (`HELLFORGE_STEAM_CENSUS_PERIOD_MS` changes the period). The
  count for `SteamAPI_RunCallbacks` is the frame count, so per-frame rates follow.

Build it with `scripts/Build-Engine.ps1 -SteamCensus`: the package is `hellforge-engine-<version>-census.zip`, and its
`engine.json` says `"steamCensus": true`. It is never for release. Every engine build also builds the census stand-in
and runs `census_test.c` against a fake Steam DLL (`fake_steam.c`): register, XMM and stack arguments, a hidden result
pointer, unwind data, and the log's lines and counts.

The census build costs a call and an interlocked increment per Steam call, and one more for proxied methods.
Unmeasured in game; the census session itself is not a cost measurement.

Run it while someone plays: the menus, the social tab, a mission, a controller.

### Step 2 results (2026-10-23, run `e0b8d508`, PC A, 214 s: the ship and the start of a mission)

Slot names below are read from the pinned DLL's thunks and SDK 1.55's `steam_api.json` (phase 3's slot reader).

**Flat exports: 11 called**, besides the stand-in's own 4. Even with the hook on `GetProcAddressForCaller`, the game
looked none of them up by name, so its protection does resolve them itself.
- Once: `SteamAPI_RestartAppIfNecessary`, `SteamAPI_Init`, `SteamInternal_CreateInterface`,
  `SteamGameServer_GetHSteamUser`, `SteamInternal_FindOrCreateGameServerInterface`, `SteamAPI_UnregisterCallback`,
  `SteamAPI_Shutdown`.
- Often: `SteamAPI_RunCallbacks` 82,436 (about 400 a second), `SteamInternal_ContextInit` 73,040 (the inline
  accessors), `SteamAPI_IsSteamRunning` 2,014, `SteamAPI_GetHSteamUser` 8.

**9 interfaces.** Step 1's 8, plus **`SteamMatchMaking009`**, requested on reaching the ship: the game makes a Steam
lobby of its own (call result 513, likely `LobbyCreated_t`). `ISteamMatchmaking` cannot be refused whole.

**Calls per `RunCallbacks`** (the methods called more than a few times):

| Slot | Calls | Per `RunCallbacks` | Method (checked against SDK 1.55) |
| --- | --- | --- | --- |
| `SteamNetworking006` #2 | 82,438 | 1.00 | `ReadP2PPacket` (P2P polling) |
| `STEAMAPPS_INTERFACE_VERSION008` #11 | 46,894 | 0.57 | `BGetDLCDataByIndex` |
| `STEAMAPPS_INTERFACE_VERSION008` #10 | 11,718 | 0.14 | `GetDLCCount` |
| `STEAMAPPS_INTERFACE_VERSION008` #6 | 11,716 | 0.14 | `BIsSubscribedApp` |
| `SteamUser021` #1 | 2,013 | 0.024 | `BLoggedOn`, beside each `SteamAPI_IsSteamRunning` |
| `SteamMatchMaking009` #17, #18 | 1,534 each | 0.019 | `GetNumLobbyMembers`, `GetLobbyMemberByIndex` |
| `SteamFriends017` #4 | 228 | | `GetFriendByIndex` |

Everything else was called 41 times or fewer:
- `SteamFriends017`: #0 `GetPersonaName`, #3 `GetFriendCount`, #5 `GetFriendRelationship`, #42 `GetUserRestrictions`,
  #43 `SetRichPresence`;
- `SteamUtils010`: #4 `GetIPCountry`, #9 `GetAppID`, #10 `SetOverlayNotificationPosition`, #31 `InitFilterText`,
  #32 `FilterText`, #34 `IsSteamRunningOnSteamDeck`;
- `STEAMAPPS_INTERFACE_VERSION008`: #4 `GetCurrentGameLanguage`, #15 `GetCurrentBetaName`;
- `STEAMUSERSTATS_INTERFACE_VERSION012`: #0 `RequestCurrentStats`, #6 `GetAchievement`;
- `SteamUser021`: #2 `GetSteamID`, #13 `GetAuthSessionTicket`;
- `SteamMatchMaking009`: #13 `CreateLobby`, #15 `LeaveLobby`, #20 `SetLobbyData`, #34 `SetLobbyJoinable`,
  #35 `GetLobbyOwner`.

`SteamInput006` #38 `GetControllerForGamepadIndex` was called 4 times at start and never again.

**Callbacks: 16 ids, with their sizes** (bytes): 143 (20), 152 (24), 163 (8), 331 (1), 333 (16), 337 (264), 505 (24),
714 (8), 1005 (4), 1101 (24), 1102 (16), 1103 (152), 1202 (8), 1203 (9), 1221 (712), and the call result 513 (16).

**What it means for the bridge:**
- Forwarding every call would cost about 1.9 round trips per `RunCallbacks`. Almost all of them can be answered in the
  game instead:
  - `SteamInternal_ContextInit` is the stand-in's own business.
  - The DLC answers (#6, #10, #11) change only with callback 1005 (`DlcInstalled`), so the broker can publish them in
    shared memory and refresh them then.
  - P2P polling (#2) can read a "packets waiting" flag the broker sets.

  An idle frame then needs no round trip.
- The allow-list needs part of `ISteamMatchmaking` (the game's own lobby).
- **Not covered by this run:**
  - The objects from `SteamInternal_CreateInterface` and `SteamInternal_FindOrCreateGameServerInterface` are not
    proxied, so calls on them are uncounted.
  - Whether a controller was in use, and so whether Steam Input polls per frame.
  - A whole mission, and the social tab.

### Co-op census (2026-10-24, two games on one PC, unsandboxed, census build)

Two installations on one PC, one Steam account, their backends on a loopback link (docs/multiplayer.md has no step for
it yet; it is a test set-up). One game joined the other from the social tab. What the joiner asked of Steam:
- after the PlayFab join, #15 `LeaveLobby` (its own Steam lobby), #14 `JoinLobby` (the host's, call result 504
  `LobbyEnter_t`), #19 `GetLobbyData` and #20 `SetLobbyData` on it, and #35 `GetLobbyOwner`;
- SteamFriends #36 `GetLargeFriendAvatar` (the other player's avatar; still refused, so no avatar) and #43
  `SetRichPresence`.

Both games poll #17 `GetNumLobbyMembers` and #18 `GetLobbyMemberByIndex` 10 times a second each, in and out of a join:
20 round trips a second in a sandboxed session, about 0.33 per frame at 60 frames a second (unmeasured in game).

The census counted no `SendP2PPacket`. **That was its blind spot, not the game's:** it does not count calls on objects
from `ISteamClient`, and the host sends P2P through one. A sandboxed run of the same join (the bridge build covers
those objects) logged the host's `SendP2PPacket`, then `GetP2PSessionState`, the moment the joiner joined.

The join stalled there: on one Steam account the host can never see the joiner in its Steam lobby. Between two PCs the
same calls would have been refused by the bridge (`JoinLobby` and `GetLobbyData` unlisted, the others `ownLobby` on a
lobby the game had not made, 504 not passed on). Now `JoinLobby` is a call, 504 is passed on, and a lobby the game
entered counts as its own (`broker_bridge.c` `RememberEntered`).

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
  - The broker reads the file when its time stamp moves, and again at once when a SteamID is not in it: a host sends
    within a second of a join, seconds before the joiner enters its Steam lobby.
- **Out:** `SendP2PPacket` (at most 7000 bytes, channels 0-7), `AcceptP2PSessionWithUser` and `GetP2PSessionState` to a
  peer only; closing a session or channel and `AllowP2PPacketRelay` to anyone.
- **In:** the broker reads channels 0-7 every pump, up to 32 packets a channel. A packet from a peer goes into the ring
  under `BRIDGE_P2P_PACKET`, in pieces of up to 996 bytes. Packets from anyone else, or over 8192 bytes, are read and
  dropped, so they never pile up in Steam. The stand-in puts each whole packet in a queue of 32 (a static array, no
  allocation). `ReadP2PPacket` and `IsP2PPacketAvailable` answer from the queue with no system call. A full ring or
  queue loses the packet, and it is logged.
- Tests: `bridge_test.c` covers a send to the entered lobby's host, a refused one to anyone else, a peer the backend
  lists mid-session, more than 7000 bytes, session state, accept, a 3000-byte packet in pieces, a stranger's packet
  dropped, and a second channel. `smoke-lobby-link` checks each backend lists the other after a join.
- Unmeasured: how much P2P co-op sends, and whether 8192 bytes and 60 ring slots are enough for it.

## Cost

- **Idle frame** (`RunCallbacks`, about 400 a second in the census): one read of the ring's write position, compared
  with the stand-in's own, and one of the tick count (for the cost log below). 0 system calls, 0 pipe messages, no
  allocation. Unmeasured in game.
- **The calls the census saw every frame:** `ReadP2PPacket` (1.00 per frame) takes a slim lock and looks at the packet queue (no system call while uncontended); the DLC answers (0.85 per
  frame) and `BLoggedOn` are a seqlocked read of the snapshot, a few dozen bytes copied. No system call. Unmeasured in
  game.
- **The inline accessors** (`SteamInternal_ContextInit`, about 340 a second in the census): a compare and a return.
- **A frame with callbacks:** per callback, one slot copied (at most 1016 bytes) and two memory fences; no system call.
  Unmeasured in game.
- **The broker's pumping** runs in its own process every 16 ms, not on the game's thread.
- **A bridged call:** one pipe round trip (two system calls in the game). `SteamAPI_IsSteamRunning` is one: 2,014 in
  the census's 214 s, about 9 a second; the lobby queries, about 7 a second each. Unmeasured in game; the rest happen once
  or on events.
- **Steam Input with a controller:** read from the snapshot, with no system call per call. The snapshot's per-frame
  cost is to be measured.

Every number here is unmeasured in game until phase 4. The bridge build measures itself: each minute, and at
`SteamAPI_Shutdown`, the stand-in's log (`HELLFORGE_CLOUD_LOG`) gets two lines:
- `bridge cost, the last minute: <frames> frames; <n> round trips, <ms> in all, longest <ms>; <n> callbacks in <n>
  frames, <ms> in all, longest frame <ms>`. A round trip's time runs from the call to its reply, the lock's wait
  included, on whichever game thread made it.
- `bridge cost, the last minute, by function (id, round trips, time): ...`: the five functions whose round trips took
  longest. Id 0 is `SteamAPI_IsSteamRunning`, and 100 on are `BRIDGE_M_*` in the generated `bridge_gen.h`. The first
  16 functions called in a minute are counted apart.

## Phases, each tested before the next

1. **Census step 2:** slot proxies, export thunks and callback sizes. Built and tested against a fake Steam DLL
   (2026-10-22); first played run 2026-10-23 (the ship and a mission's start). Still to cover: a controller, a whole
   mission, the social tab, and the game-server and `CreateInterface` objects.
2. **The broker and its channel:**
   - the pipe and ring with low labels and the client check;
   - `SteamAPI_Init` in the broker;
   - manual dispatch into the ring, and the flat API in the stand-in.

   Fixture tests use a fake Steam DLL: `broker_test.c` (every engine build) drives both ends. It covers the ring's
   bounds against a game that writes its header, the pipe's label and grant, a second process with the wrong pid, a
   lying HELLO, malformed and oversized calls, a late broker, a dead broker, the game exiting, and the stand-in itself
   in bridge mode (Valve's DLL never loads; without a broker, init fails closed).
3. **Generated proxies:** `SdkShimGen steam-bridge`, from the pinned SDK and the game's DLL, with the allow-list as a
   reviewed table (above). `bridge_test.c` (every engine build with the SDK) runs the broker's loop with a fake Valve
   DLL behind the generated dispatch and calls the bridge build's interfaces as the game does: values, strings and the
   printable check, buffers and their bounds, out values, a CSteamID argument and the hidden-pointer result, the
   snapshot, a refused method that never reaches Steam, a lobby the game did not make, callbacks (and an id the table
   does not list), a call result, `ContextInit`, `ISteamClient`, an unknown interface, the game's lobby type and Play
   with strangers, a co-op join (a lobby entered through `JoinLobby` becomes the game's, a refused entry does not), a
   NULL game-server interface, Steam P2P with the backend's co-op peers only, HellForge's own rich presence key
   refused, and that Valve's DLL never loads.
   Built 2026-10-23. The table's review items (`SetRichPresence`, `CreateLobby`, `JoinLobby`, `SendP2PPacket`) were
   reviewed 2026-10-24: the `hellforge` rich presence key is refused, and a member of a lobby the game entered is no
   longer a P2P peer by itself. Still to do: the next census.
4. **A low-integrity session:** the token and job, labelled folders, pipe labels, and the editor's saves through the
   backend. In game, solo then co-op on two PCs: no STEAM NOT DETECTED, login, a mission, a controller, an invite, and
   per-frame cost measured.

   **First solo run (2026-10-23, engine 0.1.29, one PC):** the game ran at low integrity (`0x1000`; the broker, the
   backend and the runner at `0x2000`), with no STEAM NOT DETECTED: `SteamAPI_Init` through the broker, Steam login and
   the backend's login, the war and progression, the title screen, 209 s and a clean stop. Three refusals:
   `ISteamClient::SetWarningMessageHook`, `SteamGameServer_GetHSteamUser` and the `SteamGameServer014` interface (NULL).
   Two fixes it took, both above: the folders are made with their label (relabelling `userdata` needed WRITE_OWNER), and
   game-server interfaces are NULL (a null object crashed the game on the title screen). Still to do: a mission, a
   controller, co-op on two PCs, and the per-frame cost.
