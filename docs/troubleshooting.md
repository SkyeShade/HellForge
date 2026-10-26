# Troubleshooting

For players: problems people hit when setting up or playing HellForge, the message HellForge shows for each, and what
to do. The launcher's messages are quoted as it shows them. If your problem is not here, [collect the logs](#collecting-logs)
and [ask](#where-to-ask).

- [Steam](#steam)
- [Setup](#setup): the folder, the supported build, the build patch, the network block
- [Antivirus](#antivirus)
- [Sessions that fail](#sessions-that-fail)
- [Playing with friends](#playing-with-friends)
- [Collecting logs](#collecting-logs) · [Where to ask](#where-to-ask)

## Steam

### Steam must be running and signed in

HellForge keeps the game's Steam ownership check. It runs at setup, and before a launch unless a check passed within the
last 7 days for the same installation, the same Steam account (the one signed in now) and the same game build; another
account, Steam signed out, a new game build or an older check runs it again. Either way Steam must be running and signed
in to an account that owns HELLDIVERS 2 or has it through Steam Family Sharing. When it fails, setup says "The
ownership check did not pass: ... Make sure Steam is running and signed in, then start setup again", and a session fails
with "Steam ownership was not confirmed: ..." followed by the reason. A failed check is never reused.

The last passed check is kept in the installation's `temp\steam-ownership.json`, sealed for your Windows account.
Deleting it, `HellForge.Cli play --full-ownership-check yes` or Settings > Installation's check and repair makes
HellForge ask Steam again.

Start Steam, sign in to the account that owns the game, and try again. The launcher's idle line is a reminder: "Ready.
Steam must be running and signed in."

### "Steam is not installed on this PC"

Setup found no Steam installation. Install Steam and HELLDIVERS 2, then start setup again.

## Setup

### Setup will not install into the folder

`HellForge-Setup.exe` checks the folder before it installs:

| Message | What to do |
|---|---|
| "Pick a folder outside Steam's steamapps folder." | Choose a folder that is not inside a Steam library. |
| "This folder is inside OneDrive or a linked folder, which HellForge cannot use (and OneDrive would upload about 24 GB of game files)." | Choose a folder outside OneDrive, such as `%USERPROFILE%\HellForge`. |
| "This folder is not empty. Pick a new or empty folder." | Choose a new or empty folder. A folder that already holds a HellForge installation is fine: setup updates it. |
| "Drive ... has ... GB free; the game copy needs about 25 GB." | A warning, not a refusal. Free up space or pick another drive. |
| "Close HellForge (launcher and any session) first." | Close the launcher and any running session, then press Install or Update again. |

Windows SmartScreen may warn that `HellForge-Setup.exe` is unrecognised. It is not code-signed; choose More info > Run
anyway.

### Setup cannot find the supported build

Each HellForge release runs one HELLDIVERS 2 build (Steam build 25480438). When Steam has another build, setup finds the
supported build one of two ways.

When HellForge has a build patch for the build Steam has, setup downloads it (about 1.7 GB) and makes the copy by
patching your Steam files. Your Steam install is only read. If HellForge has no patch for that build yet, the wizard
says so: "HellForge has no patch for build ... yet; once one is published, Check again uses it."

Otherwise, Steam downloads the supported build for your account. The wizard shows a card headed Download the supported
build through Steam, with one `download_depot 553850 <depot> <manifest>` command per depot. To use it:

1. Press Open the Steam console, or open `steam://open/console` yourself.
2. Copy the first command (each has a Copy button), paste it into the console and press Enter. Do the same for each
   command, one at a time.
3. Watch the wizard's progress bar: Steam shows no progress in its library for this download. Steam puts it in
   `<Steam>/steamapps/content/app_553850/` (about 24 GB). On Linux the bar stays empty, and setup reads the download
   only once `hellforge-linux.sh` has moved it into the prefix: see
   [Steam's download of the supported build](linux.md#steams-download-of-the-supported-build).
4. Setup continues by itself when every depot is complete and copies the game from the download. "Delete Steam's
   download after copying" is ticked unless you untick it, and frees the space afterwards.

Near the end the wizard names the files it still waits for. A file listed as larger than this build's is another
build's file: delete it and run that depot's `download_depot` command again.

If the wizard says "HELLDIVERS 2 is in Steam, but setup cannot use it right now", Steam is updating the game or the game
is running. Let Steam finish, close the game, and press Try again.

### The build patch did not download or did not work

A failed download says "Could not download the build patch: ... Something on this PC may be blocking or inspecting HTTPS
(an antivirus web shield, a VPN or a proxy); allow it, then try again."

When a patch fails for any reason, setup shows Steam's own download of the build instead (the steps above). Files it
already patched are kept, so the copy only needs the rest. Restarting the launcher tries the failed patch again.

Except when the drive is full: setup checks for room before it downloads the patch and again before it patches, and
says "Not enough free space on D:\: ... more is needed" (or "D:\ ran out of space while patching ...", which older
versions showed as "hpatchz exit code 24" with advice to validate the game). Free that much on the drive HellForge is
installed on and press Check again; the files already patched are kept. Steam's download would need the same room.

### Pausing and interrupted copies

Pause copying stops the copy with "Paused. Start setup again to continue where it stopped." An interrupted copy also
continues where it stopped the next time you run setup.

### Microsoft Visual C++ runtimes

The game needs two Microsoft runtimes that do not come with it: the Visual C++ 2012 Redistributable (x64) and the
Visual C++ 2015-2022 Redistributable (x64). Steam's own install of the game no longer adds the 2012 one, so a fresh PC
often lacks it. Setup checks for both before it copies the game, and every session checks again before it starts the
game. When one is missing, the message names it and gives Microsoft's download link:

- 2012: <https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x64.exe>
- 2015-2022: <https://aka.ms/vc14/vc_redist.x64.exe>

Install the one named, then start setup (or press PLAY) again. Both install side by side with any other versions you
have.

### The network block

Setup places a network block so the copied game can only reach this PC. It lives in the Windows Filtering Platform;
Windows Firewall does not need to be on.

| Message | What to do |
|---|---|
| "Windows permission was declined. The network block is required; start setup again and allow it." | Start setup again and answer Yes to the Windows administrator prompt. Setup does not finish without the block. |
| "The Windows Base Filtering Engine (BFE) service is off. HellForge needs it to keep its copy of the game off the internet." | Turn it back on (Services > Base Filtering Engine, or run `net start BFE` as administrator), then start setup again. |
| "The network block was not installed: ..." | The reason follows the colon. Setup also writes `logs\setup-network-block.json`; include it when you ask for help. |

## Antivirus

Antivirus programs sometimes flag or quarantine HellForge's files. That is a false positive. Add an exclusion for the
HellForge installation folder in your antivirus, then restore anything it quarantined. Running `HellForge-Setup.exe`
again over the installation puts the app back and keeps your game copy, profiles and settings.

Cases HellForge already recognises:

- The network block's check program, `network-block\NetworkProbe.exe`, is missing. A session then fails with "The network
  block's check program is missing (...). Antivirus programs sometimes remove it: restore it from quarantine and exclude
  the HellForge folder, or run Settings > Installation again to put the block back."
- The build patch download fails because an antivirus web shield inspects HTTPS (see
  [the build patch](#the-build-patch-did-not-download-or-did-not-work)).
- When the first source mod fetches the mod compiler, the antivirus scans the unpacked files and HellForge waits: "waiting
  for the antivirus to finish scanning the unpacked files...".
- The game exits with `0xC0000022` ("Windows or security software denied the game access") or `0xC0000428` ("Windows
  blocked an unsigned DLL (Smart App Control or a code-integrity policy)"). See the exit codes under
  [the game could not be prepared](#the-game-could-not-be-prepared-or-connected).

## Sessions that fail

When a session fails, the launcher shows the step that failed in red and the reason under it. Open session logs (on the
Console page and in Settings) shows the session's folder; `session.log` there lists its steps.

### The game seems stuck in the cryo pod

The first launch of a new installation can stop in the cryo pod. The launcher notices and says "The game seems stuck in
the cryo pod." Press STOP, then PLAY again: the next launch goes through.

### Messages before the game starts

| Message | What to do |
|---|---|
| "Existing game or GameGuard process." | HELLDIVERS 2 (or its GameGuard) is already running. Close it, then press PLAY. |
| "Another HellForge session is still running for this installation..." | Wait for it to end, or close `HellForge.Session.exe` in Task Manager (or restart the PC), then press PLAY again. |
| "The game copy is not the supported build (...); repair it from the launcher." | Run Settings > Installation > Check and repair. |
| "The game needs a Microsoft runtime this PC does not have..." | Install the runtime it names from the link it gives, then press PLAY again ([Visual C++ runtimes](#microsoft-visual-c-runtimes)). |
| A message ending in "reinstall HellForge" (stand-ins, mod loader or local backend missing or changed) | Run `HellForge-Setup.exe` and pick the installation; Update keeps the game copy, profiles and settings. If it comes back, check your antivirus ([above](#antivirus)). |
| "Windows administrator consent was not given for the network block." | Press PLAY again and answer Yes to the Windows prompt. |
| "The launch options cannot be used." | Fix or clear Settings > Launch options; the reason shows under the box. |
| "The session runner exited before starting." | The launcher points to `logs\session-runner.log`, which says why. |
| "Mod left out of this session: ..." (in the Console) | The session still runs without that mod. The Mods page shows why on the mod's row ([using mods](mods.md)). |
| "HellForge's file ... could not be held read-only for the session ..." | Another program has that file of the installation's `app` folder open for writing. Close it (or restart the PC), then press PLAY. |
| "Mod file ... could not be held read-only for the session ..." | Another program (an editor, an archive tool) has that file in `mods` open for writing. Close it, then press PLAY. |
| "WARNING: ... file(s) appeared in the mods folder during the session ..." (in the Console, after a session) | Something wrote into `mods` while the game ran. If you did not add them, delete the files it lists (in that session's `result.json`, `plantedMods`) and leave any switched-off mod off ([using mods](mods.md#the-mods-page)). |
| "Port ..., HellForge's backend port, is used by another program on this PC." | Close the program using it (or restart the PC). The network block lets the game reach only that port. |
| "The network block still lets the game reach every program on this PC: placing it again ..." | An installation from before 0.3.0: the session narrows the block to HellForge's backend, after one Windows prompt. Accept it. |
| "The network block could not be narrowed ..." | The prompt was declined or the install failed (the reason is in brackets). Run Settings > Installation once: setup narrows the block. |
| "A sandboxed session needs the network block narrowed ..." or "... needs the WFP network block ..." | A sandboxed game must not reach other programs on this PC. Run Settings > Installation once, or turn off Settings > Sandbox. |
| "Cleanup failed." | Check the session logs before playing again. |

### The game could not be prepared or connected

After the game starts, the session runner makes two memory-only edits to the running game: the first prepares it for
the local backend, the second connects it. These messages mean one of them did not happen:

- "The game could not be prepared for the local backend."
- "The game could not be connected to the local backend."

For the second, the session runner waits up to 45 seconds for the game to load and unpack its code. The edit fails when
the game closes or crashes first, takes longer than that, or the code is not what HellForge expects. Press PLAY once
more. If it fails the same way again, [save the logs](#collecting-logs) and report it.

When the game exited before it was prepared, the first message adds the game's exit code, and for these codes what it
means:

| Exit code | Meaning |
|---|---|
| `0xC0000135` | a DLL the game needs could not be found |
| `0xC0000142` | a DLL the game loads failed to start |
| `0xC000007B` | a DLL the game loads is damaged or for the wrong architecture |
| `0xC0000005` | the game crashed (access violation) |
| `0xC0000409` | the game stopped itself after detecting a problem |
| `0xC0000022` | Windows or security software denied the game access |
| `0xC0000428` | Windows blocked an unsigned DLL (Smart App Control or a code-integrity policy) |
| `0xC0000361` | a Windows policy blocked a file the game loads |

For a damaged or missing DLL, check the [Visual C++ runtimes](#microsoft-visual-c-runtimes) first, then run Settings >
Installation > Check and repair. For the security codes, see
[antivirus](#antivirus).

### Other network block messages

"The network block is not holding" means the session's check of the network block did not pass: the local backend must
answer on this PC, and a connection to the outside must be refused by the block. (Before 0.3.0 the message was
"Required loopback/egress controls failed.") The message says which part failed:

- **"not stopping the game's connections to the internet"**: the block's filters no longer apply to the game. They name
  each program by its drive's internal device path, which for a drive other than Windows' own can change after a
  restart (for example with a USB drive or another disk plugged in), and they stay behind when the HellForge folder is
  moved. The session places the block again by itself, with one Windows prompt. If it still does not hold, a VPN, a
  proxy or a security program is letting the game's traffic past it: turn it off or exclude the HellForge folder.
- **"stops the game reaching HellForge's own backend"**: usually an older HellForge started on an installation a newer
  one set up. Use the newer HellForge, or run Settings > Installation.
- **"the game cannot reach HellForge's own backend on this PC"**: another firewall or security program is blocking
  connections inside this PC (loopback). Allow HellForge's folder in it.

The session's `network-probe.json` (and `network-probe-again.json` after the block was placed again) records what the
check saw; include the logs when you ask.

"Network block was not installed: Linked target path: <path> is a link or a special file" means a folder or file on
the way to the game copy is a link: a junction or symbolic link (a Documents folder moved to another drive is often
one), a OneDrive or other cloud placeholder, or a file compressed with CompactGUI or `compact /exe`. The block refuses
it because a link could point its files or rules elsewhere. Move HellForge to a plain folder (for example
`C:\HellForge`), or undo the link or compression at the path the message names, then run Settings > Installation.

## Playing with friends

Friends need the same gameplay mods, versions and files, and the same HellForge version. When they differ, the join is
refused and the in-game editor shows both players which mods differ. [Using mods](mods.md#playing-with-friends) explains
the modlist code and the notices. With the in-game editor switched off, only the logs say why
(`backend\multiplayer.log` in each session's folder).

Settings > Multiplayer has two switches, Local PlayFab and Lobby and session through Steam friends. Co-op needs both;
they are on unless you switch them off.

## Collecting logs

Every session keeps its logs in `logs\sessions\<session>\` in the installation; the newest ten sessions are kept.

To send them to someone, open the Console page and press Save logs to Desktop. It zips the latest session's folder to
your Desktop as `HellForge-logs-<date>-<time>.zip`, with the game's console logs. If a session is still running, files in
use may be left out, and the launcher says how many.

In the text files in the zip, your Windows user folder is written as `%USERPROFILE%` (a Linux home folder as `~`) and the
PC's name as `PC`. Paths stay readable. The zip still names Steam accounts by their id. Files that are not text, such
as a crash dump, go in as they are.

Settings > Troubleshooting has two switches for harder problems. Both apply to the next session.

- Engine log starts the game with its own log switched on (`--log-to-file`) and copies that log into the session's logs,
  so Save logs to Desktop includes it. It helps find why a join or connection fails.
- Crash dump writes a full memory dump of the game (about 1 GB) into the session's logs if it crashes. It holds what the
  game had in memory, your Steam account name for one, so only send it to people you trust. Leave it off unless you need
  it.

## Where to ask

Open an issue on GitHub: [github.com/SkyeShade/HellForge/issues](https://github.com/SkyeShade/HellForge/issues). Say
what you did and what the launcher showed, and attach the logs zip. Text the launcher shows does not name your PC, so a
screenshot of it is safe to post. You can also ask on the [HellForge Discord](https://discord.gg/3Yye92xCzh), which the
launcher's Discord button opens too.

For Linux, see [linux.md](linux.md) first; for problems with a mod you are making, [debugging](modding/debugging.md).
