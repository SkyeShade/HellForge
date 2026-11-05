# Steam bridge reference

<!-- diataxis: reference -->

For people working on the [Steam bridge](steam-bridge.md): the census of what the game asks of Steam and its results,
where the generated code's definitions come from, and what the bridge's tests check. Why the bridge works as it does is
in [steam-bridge.md](steam-bridge.md).

## The census

`native/steam-cloud/steam_census.c`, off unless `HELLFORGE_STEAM_CENSUS_LOG` names a file. The bridge's allow-list is
built from what it found.

**Step 1** logs each interface version the game requests, each stand-in export it resolves by name, and each callback
id it registers. It hooks kernelbase's `GetProcAddressForCaller`, where kernel32's `GetProcAddress` jumps, so it sees
lookups through kernel32 too.

**Step 2** (the census build) needs no definitions, and counts every export the game calls whichever way it found it:

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

## Step 1 results (2026-10-22)

One session from start to the title screen, the game connected to the backend.

**8 interfaces**, each requested once:

| Interface | Version | What the game needs it for, likely |
| --- | --- | --- |
| `ISteamUtils` | `SteamUtils010` | app id, overlay state, the gamepad text box |
| `ISteamRemoteStorage` | `016` | the cloud save; HellForge's stand-in already answers it locally |
| `ISteamUserStats` | `012` | stats and achievements |
| `ISteamApps` | `008` | ownership, DLC, language |
| `ISteamInput` | `SteamInput006` | controllers: which XInput pad is one of Steam Input's (`GetControllerForGamepadIndex` only, [Controllers and Steam Input](steam-bridge.md#controllers-and-steam-input)) |
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
reached the hook (the census test showed it). The hook is now on `GetProcAddressForCaller`.

## Step 2 results (2026-10-23)

Run `e0b8d508`, PC A, 214 s: the ship and the start of a mission. Slot names below are read from the pinned DLL's thunks
and SDK 1.55's `steam_api.json` (phase 3's slot reader).

**Flat exports: 11 called**, besides the stand-in's own 4. Even with the hook on `GetProcAddressForCaller`, the game
looked none of them up by name, so its protection does resolve them itself.

- Once: `SteamAPI_RestartAppIfNecessary`, `SteamAPI_Init`, `SteamInternal_CreateInterface`,
  `SteamGameServer_GetHSteamUser`, `SteamInternal_FindOrCreateGameServerInterface`, `SteamAPI_UnregisterCallback`,
  `SteamAPI_Shutdown`.
- Often: `SteamAPI_RunCallbacks` 82,436 (about 400 a second), `SteamInternal_ContextInit` 73,040 (the inline
  accessors), `SteamAPI_IsSteamRunning` 2,014, `SteamAPI_GetHSteamUser` 8.

**9 interfaces.** Step 1's 8, plus **`SteamMatchMaking009`**, requested on reaching the ship. The game makes a Steam
lobby of its own (call result 513, likely `LobbyCreated_t`), so `ISteamMatchmaking` cannot be refused whole.

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

**Callbacks: 16 ids, with their sizes:**

| Id | Size (bytes) |
| --- | --- |
| 143 | 20 |
| 152 | 24 |
| 163 | 8 |
| 331 | 1 |
| 333 | 16 |
| 337 | 264 |
| 505 | 24 |
| 714 | 8 |
| 1005 | 4 |
| 1101 | 24 |
| 1102 | 16 |
| 1103 | 152 |
| 1202 | 8 |
| 1203 | 9 |
| 1221 | 712 |
| 513 (the call result) | 16 |

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

## Co-op census results (2026-10-24)

Two games on one PC, unsandboxed, census build. Two installations, one Steam account, their backends on a loopback link
([multiplayer.md](multiplayer.md) has no step for it yet; it is a test set-up). One game joined the other from the
social tab. What the joiner asked of Steam:

- after the PlayFab join, #15 `LeaveLobby` (its own Steam lobby), #14 `JoinLobby` (the host's, call result 504
  `LobbyEnter_t`), #19 `GetLobbyData` and #20 `SetLobbyData` on it, and #35 `GetLobbyOwner`;
- SteamFriends #36 `GetLargeFriendAvatar` (the other player's avatar; still refused, so no avatar) and #43
  `SetRichPresence`.

Both games poll #17 `GetNumLobbyMembers` and #18 `GetLobbyMemberByIndex` 10 times a second each, in and out of a join.
In a sandboxed session that is 20 round trips a second, about 0.33 per frame at 60 frames a second (unmeasured in
game).

The census counted no `SendP2PPacket`. **That was its blind spot, not the game's:** it does not count calls on objects
from `ISteamClient`, and the host sends P2P through one. A sandboxed run of the same join (the bridge build covers those
objects) logged the host's `SendP2PPacket`, then `GetP2PSessionState`, the moment the joiner joined.

The join stalled there: on one Steam account the host can never see the joiner in its Steam lobby. Between two PCs the
bridge would have refused the same calls:

- `JoinLobby` and `GetLobbyData` were unlisted;
- the others were `ownLobby` on a lobby the game had not made;
- 504 was not passed on.

Now `JoinLobby` is a call, 504 is passed on, and a lobby the game entered counts as its own (`broker_bridge.c`
`RememberEntered`).

## Where the definitions come from

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

## Tests

| Test | Runs | Checks |
| --- | --- | --- |
| `census_test.c` | Every engine build, against a fake Steam DLL (`fake_steam.c`) | The census stand-in: register, XMM and stack arguments, a hidden result pointer, unwind data, and the log's lines and counts |
| `broker_test.c` | Every engine build, with a fake Steam DLL, driving both ends | The ring's bounds against a game that writes its header. The pipe's label and grant. A second process with the wrong pid, and a lying HELLO. Malformed and oversized calls. A late broker, a dead broker, and the game exiting. The stand-in itself in bridge mode (Valve's DLL never loads, and without a broker, init fails closed) |
| `bridge_test.c` | Every engine build with the SDK: the broker's loop with a fake Valve DLL behind the generated dispatch, calling the bridge build's interfaces as the game does | Values, strings and the printable check, buffers and their bounds, and out values. A CSteamID argument and the hidden-pointer result. The snapshot. A refused method that never reaches Steam, and a lobby the game did not make. Callbacks (and an id the table does not list), and a call result. `ContextInit`, `ISteamClient`, an unknown interface, and a NULL game-server interface. HellForge's own rich presence key refused. Valve's DLL never loading |
| `bridge_test.c`, the game's lobby | As above | The lobby type clamp with Play with strangers, a nested key that is not the setting, both changes, the test switch and a private lobby left alone |
| `bridge_test.c`, co-op | As above | A co-op join (a lobby entered through `JoinLobby` becomes the game's, and a refused entry does not). Steam P2P with the backend's co-op peers only: a send to the entered lobby's host, a refused one to anyone else, a peer the backend lists mid-session, more than 7000 bytes, session state, accept, a 3000-byte packet in pieces, a stranger's packet dropped, and a second channel |
| `smoke-lobby-link` | | Each backend lists the other after a join |
| `SquadTests` (HellForge.LocalBackend.Tests) | Three backends over loopback UDP | Each one's `p2p-peers.txt` as a third player joins, the second leaves and rejoins, and the host kicks the third |
| `test/input_test.c` | Every build, with fakes for Steam Input, XInput and the overlay | When Steam is asked, the lines, the 16-controller bound, the pads' changes, and a DLL without Steam Input or an `Init` that fails |
| `SteamInputNoteTests` | | Reads the lines `input_test.c` checks, compares the two type-name tables, and checks the note and result.json's `steamInput` |
