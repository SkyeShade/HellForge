# Several installations on one PC (test mode)

<!-- diataxis: how-to -->

For testing co-op without a second PC: two or more HellForge installations run a game each, at the same time, under
one Steam account, and join each other. It is a test set-up, switched on by `HELLFORGE_MULTI_INSTANCE=1` in the
session runner's environment. Without the switch nothing below changes what a session does.

## What the switch changes

- **The process check** (`HellForge.Session/GameProcesses.cs`): a game another HellForge installation runs
  (`<root>\game\bin\helldivers2.exe` with that installation's marker) no longer keeps a session from starting. The
  normal game, GameGuard, and a game of this installation still do.
- **The installed game's hold** (`PlaySession`): the first session holds the installed game's files closed. A later
  one takes the hold over when that one ends, and reads the installed executable's hash only when it can.
- **Identity** (`PlayFabService.IdentityOverride`): the backend logs the game in as `HELLFORGE_IDENTITY` (a SteamID64 of
  an individual account), so each installation is its own player.
- **The link** (`PlaySession`): with `HELLFORGE_PEER_LINK` the backends reach each other over loopback UDP
  (`udp:<own id hex>:<port>:<peer id hex>=<port>`) instead of Steam. `HELLFORGE_DIRECTORY_FOLDER` makes a folder the
  lobby directory (`--directory`).
- **The broker** (`native/steam-cloud/broker_lobby.c`, sandboxed sessions only): the game sees `HELLFORGE_IDENTITY` as
  its SteamID, named `HELLFORGE_PERSONA` (else "Diver \<last four digits>"). The game's own Steam lobby is kept between
  the installations in `<directory folder>\steam-lobbies`, one text file per lobby, changed under a lock file. It
  covers create, join, leave, members, owner and owner-only lobby data, and the call results and data updates the game
  waits for. Steam P2P between them goes over loopback UDP, port 48000 + (SteamID mod 1000).

## Setting it up

1. Make each extra installation from an existing one:
   `scripts\New-TestInstallation.ps1 -Main C:\HellForge -Instance C:\HellForge-2`. The game data is hard-linked (about
   600 MB per installation instead of a full copy), and the rest is copied.
2. Start one session from each copy once, to place its network block (a Windows administrator prompt).
3. For each installation, set these in the session runner's environment, then start
   `HellForge.Session play --multiplayer --sandbox`:

   | Variable | Example (first / second) |
   | --- | --- |
   | `HELLFORGE_MULTI_INSTANCE` | `1` |
   | `HELLFORGE_IDENTITY` | `76561199000000002` / `76561199000000003` |
   | `HELLFORGE_PEER_LINK` | `udp:11000013DF91602:47002:11000013DF91603=47003`, and the reverse |
   | `HELLFORGE_DIRECTORY_FOLDER` | the same folder for both |
   | `HELLFORGE_TEST_STRANGERS` | `1`, for the Galactic Map ([public lobbies](multiplayer-lobbies.md#testing-public-lobbies)) |

   Both installations need Play with strangers on for the map. `--toaster_mode` in the game options lowers the
   memory each game needs: with it, a game used 3 to 4.5 GB of RAM, and two games 12 GB of graphics memory. It also
   turns texture streaming off, so Armory icons stay blank.
4. Join from the second game with `+connect_lobby <the first game's Steam lobby id>` in its options (the lobby file's
   name, in decimal), or from the Galactic Map.

## What has been tested

Two installations on one Windows PC (2026-10-24):

- `+connect_lobby` puts the joiner in the host's lobby and Party network, with about 30 Party messages a second each
  way. That was seen in the logs only, not in game.
- **The Galactic Map** works with both as strangers. The joiner sees the host's squad marker ("1/4") and hover card
  (host name, level, mission) on the host's planet. Selecting the marker brings the loadout screen of the host's
  mission, and after Ready Up the joiner drops into the mission in progress. The marker needs the record's `seed`
  ([public lobbies](multiplayer-lobbies.md#the-selected-mission-seed)).
- `BeginAuthSession` (each game checks the other's Steam ticket at a join) is refused, and the join goes on.
- **Not covered:**
  - more than two installations
  - two installations without the sandbox (the game would then talk to Valve's DLL, which cannot give two games
    different SteamIDs)
  - a whole mission together

The full test notes are in
[the earlier version of this page](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multi-instance.md#tested-on-one-pc-2026-10-24).

## On Linux

Two installations can run side by side on Linux (Proton), each a session of its own, for the lobby browser. Tested on
Fedora, 2026-10-30:

- **A Wine prefix each.** Two sessions cannot share a prefix: the second wrapper sees the first one's wineserver, which
  runs outside its network namespace, and refuses (`hellforge-offline.sh`). The tested second installation went into a
  copy of the prefix (`~/.local/share/hellforge-prefix-b/pfx/drive_c/...`, about 380 MB without the installations),
  with its game data hard-linked as on Windows.
- **A namespace each.** Each session has its own loopback, so `HELLFORGE_PEER_LINK`'s ports do not reach the other
  session. A relay run in both namespaces (`nsenter`) and joined over a Unix datagram socket can carry them. The
  directory folder needs nothing, since files cross namespaces.
- **What works:** each backend lists its lobby in the shared folder, under `HELLFORGE_IDENTITY`. The other session's
  browse API and in-game browser list it as a stranger's ("Ready to join"), and Quick Play hands that game the
  invitation.
- **What needs the test broker: the game's join.** Without it both games are the same Steam account, and a second
  game signing in sends the first back to its title screen.

The full test notes are in
[the earlier version of this page](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multi-instance.md#on-linux-2026-10-30-the-lobby-browser).

## On Linux: the test broker

On Linux the sandbox is a separate Wine prefix and wineserver, which the broker's pipe cannot reach, so the steps above
need `HELLFORGE_TEST_BROKER=1` instead of `--sandbox`. Each installation needs its own Wine prefix too: a second
session's wrapper refuses a prefix whose wineserver runs outside its network namespace (`hellforge-offline.sh`).

1. Make the second installation in a copy of the prefix (for example `~/.local/share/hellforge-prefix-b`), with its game
   data hard-linked as on Windows.
2. Set the variables of step 3 and `HELLFORGE_TEST_BROKER=1`, and start `HellForge.Session play --multiplayer` without
   `--sandbox`. The session uses the Steam bridge and starts the broker at the player's integrity (no low token;
   `PlaySession.LinuxTestBroker`, the session's `"testBroker": true`). With `--sandbox` it refuses to start. The build
   needs the bridge and broker (`scripts/Build-Engine.ps1` with the Steamworks SDK, not `-NoSteamApi`).
3. Each session has its own network namespace, so loopback does not reach the other one. Once both games run, start the
   relay in each session's namespace, giving the other installation's backend link port and broker P2P port
   (48000 + its SteamID mod 1000):

   ```sh
   nsenter -t <first game pid> -U -n --preserve-credentials python3 -I app/linux/hf-mi-relay.py a b 47003 48003
   nsenter -t <second game pid> -U -n --preserve-credentials python3 -I app/linux/hf-mi-relay.py b a 47002 48002
   ```

The lobby folder is shared through files, which cross namespaces.

- Its lock is an exclusive create (`CREATE_NEW`, in `broker_lobby.c`): two prefixes' wineservers do not see each
  other's share modes. The second prefix is refused until the first lets go, and the file goes with the handle.
- A stale lock (older than 5 s, left by a wineserver that was killed) is taken over.
- The broker's pipe security (a low label, the owner's SID) works under Wine as it is: a client at the player's
  integrity connects, and the broker reads its pid.

**Not yet run with two games.** The bridge and broker are not in the Linux builds made so far, which had no Steamworks
SDK. Whether two brokers signed in to one Steam account keep each other's game at its title screen (as two games with
Valve's DLL did) is still to be seen.
