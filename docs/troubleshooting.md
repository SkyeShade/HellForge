# Troubleshooting

For players: problems people hit when setting up or playing HellForge, the message HellForge shows for each, and what
to do. The launcher's messages are quoted as it shows them. If your problem is not here, [collect the logs](#collecting-logs)
and [ask](#where-to-ask).

- [Steam](#steam)
- [Setup](#setup): the folder, the supported build, the Steam download, languages other than English, the network block
- [Antivirus](#antivirus)
- [Sessions that fail](#sessions-that-fail)
- [In the game](#in-the-game): blank Armory icons
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

### "This PC runs Windows 10 version ..." (Setup or the launcher closes as it starts)

HellForge's programs are built on .NET 10, which uses the processor's hardware stack protection (Intel 11th generation
and AMD Ryzen 5000 and later). Windows 10 needs Microsoft's fixes for it from October 2024 or later; without them every
HellForge program closes as soon as it starts, and the Application event log shows ".NET Runtime" and "Application
Error" entries with 0x80131506. Windows 10 2004, 20H2 and 21H1 never got those fixes. Setup checks this first and names
your Windows: update to Windows 10 version 22H2 or Windows 11 in Settings > Update & Security > Windows Update (on 22H2,
install the latest updates), then run Setup again. Run `winver` to see your version and build.

### Setup cannot find the supported build

Each HellForge release runs one HELLDIVERS 2 build (Steam build 25480438). When Steam has another build, or no
HELLDIVERS 2, setup gets the supported build from Steam for your account.

The wizard shows a card headed Get the supported build from Steam. It opens on the QR code; Choose another way shows
all three ways, and every way has it:

- **Scan a QR code with your phone** (the default): scan the code with the Steam app on your phone (the Steam Guard
  shield, then the QR code scanner) and approve the sign-in in the app. Setup then downloads what your Steam install
  does not have and copies the rest from it. HellForge never sees your password.
- **No phone: sign in with SteamCMD:** type your Steam account name (the name you sign in with) and press Start SteamCMD.
  Setup gets SteamCMD, Valve's own command-line Steam tool, from Valve and opens it in a window of its own. Type your
  password in the SteamCMD window; it goes to Valve's own tool, not HellForge. If Steam asks for a Steam Guard code
  (by email, or from the Steam app), type it there too. SteamCMD then downloads the build; the progress shows under Game
  files. Setup checks every file and moves it into the installation, then removes SteamCMD and what it remembered of the
  sign-in. It needs about 24 GB free on the installation's drive.
- **Use the Steam app:** no sign-in in HellForge. The Steam app on your PC downloads the build through its console, and
  setup copies it (the steps below).

HellForge keeps nothing of a sign-in, so the next download asks again. Every file is checked against Steam's hashes,
and your Steam install is only read. Pause download stops the QR code's download; setup continues where it stopped the
next time, after you sign in again.

When you choose the Steam app, when Steam does not allow the download after a sign-in, when SteamCMD cannot be set up
(below), and always on Linux, the card shows the Steam app's steps, with one `download_depot 553850 <depot> <manifest>`
command per depot. To use them:

1. Press Open the Steam console, or open `steam://open/console` yourself.
2. Copy the first command (each has a Copy button), paste it into the console and press Enter. Do the same for each
   command, one at a time.
3. Watch the progress under Game files: Steam shows no progress in its library for this download. Steam puts it in
   `<Steam>/steamapps/content/app_553850/` (about 24 GB); the Linux Steam client may use
   `steamrt64/steamapps/content/app_553850/` or `ubuntu12_64/steamapps/content/app_553850/` instead, and setup looks in
   each. On Linux the bar stays empty, and setup reads the download only once `hellforge-linux.sh` has moved it into
   the prefix: see [Steam's download of the supported build](linux.md#steams-download-of-the-supported-build).
4. Setup continues by itself when every depot is complete and copies the game from the download. "Delete Steam's
   download after copying" is ticked unless you untick it, and frees the space afterwards.

Near the end the wizard names the files it still waits for. A file listed as larger than this build's is another
build's file: delete it and run that depot's `download_depot` command again.

If the wizard says "HELLDIVERS 2 is in Steam, but setup cannot use it right now", Steam is updating the game or the game
is running. Let Steam finish, close the game, and press Try again.

### The Steam download did not work

| Message | What to do |
|---|---|
| "Steam sign-in did not work: the sign-in expired before it was approved; try again." | The QR code was not approved in time. The card shows a new code: scan it and approve the sign-in in the Steam app. |
| "Steam sign-in did not work: Steam has had too many sign-in attempts; wait a while and try again." | Wait a while, then sign in again on the card. |
| "Steam did not let this account download depot ... This happens when HELLDIVERS 2 is borrowed through Steam Family Sharing ..." | The account that signed in does not own the game. Sign in with the account that owns it (Choose another way), or use the Steam app's steps the card shows. |
| "The download from Steam stopped: ..." | A lost connection or Steam's servers refused. Use the Steam app's steps the card shows, or Choose another way to try again (the files already downloaded are kept). |
| "SteamCMD closed without downloading the build (exit code ...)." | The sign-in in SteamCMD's window did not work (a wrong password or Steam Guard code), or the window was closed. Press Start SteamCMD again and type them in its window. A game borrowed through Family Sharing cannot be downloaded this way: use the Steam app. |
| "SteamCMD closed before the download was complete (... of ... GB)." | Press Start SteamCMD again: it continues, and what it downloaded is kept. |
| "SteamCMD cannot be used: ..." | SteamCMD could not be fetched from Valve, was not signed by Valve, or did not start. The card shows the Steam app's steps instead. |
| "... did not come out as the supported build's file; it was not kept. Start setup again." | Start setup again. It is downloaded again, and the files already in place are kept. |

On a full drive, setup asks for space before it writes anything: "Not enough free space on D:\: ... more is needed".
Free that much on the drive HellForge is installed on and press Check again; the files already downloaded are kept.
SteamCMD needs the same room once (its download is moved, not copied); the Steam console's download would need the
same room and more.

### Pausing and interrupted copies

Pause copying stops the copy with "Paused. Start setup again to continue where it stopped." An interrupted copy also
continues where it stopped the next time you run setup.

### Microsoft Visual C++ runtimes

The game needs two Microsoft runtimes that do not come with it: the Visual C++ 2012 Redistributable (x64), for the
game's `lua51.dll` (MSVCR110.dll), and the Visual C++ 2015-2022 (v14) Redistributable (x64), for PartyWin, PlayFab,
libcurl, XeSS, EOS and the upscalers (VCRUNTIME140.dll, VCRUNTIME140_1.dll, MSVCP140.dll). Steam's own install of the
game no longer adds the 2012 one, so a fresh PC often lacks it.

Setup, and PLAY in an installation that lacks one (one set up with 0.2, or a runtime removed since), download
Microsoft's installer, check it against the SHA-256 HellForge pins, and run it silently after Windows asks for
permission. PLAY shows INSTALLING meanwhile. A runtime counts as missing when its DLL is not in System32 as a 64-bit
DLL, so one Windows lists as installed whose DLL is gone is repaired, and a 32-bit copy put there by hand does not
count. The installer's log is in the installation's `temp` folder.

Only when that fails (the permission prompt declined, no connection to Microsoft, another installation running) does
the message say why and give Microsoft's download link:

- 2012: <https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x64.exe>
- 2015-2022 (v14): <https://aka.ms/vc14/vc_redist.x64.exe>

Install the one named, then start setup (or press PLAY) again. Both install side by side with any other versions you
have. On Linux, Proton's own builtin runtimes serve, and nothing is checked or installed.

### Playing in a language other than English

The copy setup makes holds English only: every other language of HELLDIVERS 2 is a free DLC of its own, which Steam
installs beside the game but setup does not copy yet. A copy without your language crashes when you switch the game to
it.

Until setup offers it, add the language with the command-line tool, `<installation>\app\cli\HellForge.Cli.exe`. Japanese is
the only language HellForge has pinned so far; more follow.

1. Run `HellForge.Cli.exe languages`. It lists the pinned languages and, for each, a `download_depot` command.
2. Open Steam's console (`steam://open/console` in a browser or the Run dialog), paste that command and wait for
   "Depot download complete".
3. Close HellForge, then run `HellForge.Cli.exe language-add --name japanese`. It copies the language into the game copy,
   checks every file, and deletes Steam's download.

`languages` then shows `"inCopy": true` for it. Your live game is never touched.

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
- When the first source mod fetches the mod compiler, the antivirus scans the unpacked files and HellForge waits: "waiting
  for the antivirus to finish scanning the unpacked files...".
- The game exits with `0xC0000022` ("Windows or security software denied the game access") or `0xC0000428` ("Windows
  blocked an unsigned DLL the game loads ..."). See the exit codes under
  [the game could not be prepared](#the-game-could-not-be-prepared-or-connected), and
  [Smart App Control](#smart-app-control) for the second.

## Smart App Control

"An Application Control policy has blocked this file" is Windows 11's Smart App Control blocking one of HellForge's
files. Smart App Control lets a program or DLL run only when Microsoft's cloud service knows it, or a publisher Windows
trusts has signed it. HellForge's files are not signed yet, so while Smart App Control is on, HellForge cannot run, and
HellForge cannot add an exception for itself: Smart App Control has none.

Setup and the launcher warn when it is on. When Windows blocks a file, the launcher says which one ("Windows blocked
HellForge.Session.exe ...") and what you can do:

- Play HELLDIVERS 2 without HellForge for now.
- Or turn Smart App Control off: Windows Security > App & browser control > Smart App Control settings. **Windows may
  not let you turn it back on afterwards** without resetting or reinstalling Windows. Decide with that in mind.

Smart App Control in evaluation mode blocks nothing; Windows may turn it on by itself later. When a block still happens
with Smart App Control off, an App Control policy set by your organisation (a work or school PC) is blocking the file,
and only its administrator can change that.

Mods with code are blocked as well, even once HellForge is signed: the mod compiler builds them on your PC, unsigned.
The session's `modloader.log` then says the mod was "not loaded (... Windows error 4551, an Application Control policy
such as Smart App Control blocked it)".

When reporting it, add the CodeIntegrity entries that name the blocked file: Event Viewer > Applications and Services
Logs > Microsoft > Windows > CodeIntegrity > Operational, events 3077, 3033 and 3089 (right-click > Save Selected
Events, or copy their General tab), along with [the session's logs](#collecting-logs).

## Sessions that fail

When a session fails, the launcher shows the step that failed in red and the reason under it. Open session logs (on the
Console page and in Settings) shows the session's folder; `session.log` there lists its steps.

### The game seems stuck in the cryo pod

The first launch of a new installation can stop in the cryo pod. The launcher notices and says "The game seems stuck in
the cryo pod." Press STOP, then PLAY again: the next launch goes through.

### The game does not close

If the game closes its window but keeps running, HellForge ends it 30 seconds later and finishes the session as STOP
does; the Play page says so when the session ends. If the window stays open but stops responding, press STOP in the
launcher: it ends the game and everything the session started. Either way, the session's `result.json` records what the
game was still doing (`stuckQuitting`, `gameHung` or `gameAtStop`), and Save logs to Desktop adds
`processes-now.txt`, the session's programs that were still running when you saved the logs. When you report it, send
that zip; if you can, save it while the game is still stuck.

### A black screen for minutes when the game starts

Without a shader cache the game builds one before it shows anything, which can take several minutes with the window
black. The launcher says "Building the shader cache (first start only, this can take a few minutes)..." meanwhile. A new
installation starts from the normal game's shader cache when there is one, and a sandboxed session takes a copy of the
installation's and brings it back at the end, so this should happen once.

The game saves its shader cache only when it closes by itself. STOP and closing the launcher ask it to close and wait
up to 30 seconds; a game that crashes, or that does not close in time, keeps the cache it started with. If every start
is slow, play one session and quit from the game's own menu, then send Save logs to Desktop from the next one: its
`result.json` says how big the shader cache was at the start and the end, whether the game wrote it, and how long the
game took to its first request.

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
the local backend, the second connects it.

When the game closes before both are made, usually because it crashed while starting, the message says so:

- "The game closed while starting, before HellForge could prepare it for the local backend (...)."
- "The game closed while starting, before HellForge could connect it to the local backend (...)."

In brackets are the game's exit code, what it means (below) and, when Windows recorded the crash, the file the game
crashed in: "Windows recorded the crash in RTSSHooks64.dll+0x2f3c1". When that file is not part of the game, HellForge
or Windows, another program put it into the game: an overlay, a recorder, RGB lighting or security software. The
message names it: close that program, turn off its in-game overlay or add the HellForge folder to its exclusions, then
press PLAY. Otherwise press PLAY again, and if the game closes again, save the crash report the Play page offers and
attach it to an issue: its `summary.txt` has Windows' record of the crash. (0.3.0-beta said "The game could not be
connected to the local backend." for these too.)

When the game is still running and an edit was not made:

- "The game could not be prepared for the local backend."
- "The game could not be connected to the local backend: ..." and why: the game had not loaded its code (`game.dll`)
  after 45 seconds, its code in memory was not what HellForge expects, or HellForge's change to its memory did not go
  through.

Press PLAY once more. If it fails the same way again, [save the logs](#collecting-logs) and report it. The session's
`result.json` keeps how each edit ended (`prepare`, `connect`) and Windows' record of a crash (`windowsCrash`).

The game's exit codes in the messages, and what they mean:

| Exit code | Meaning |
|---|---|
| `0xC0000135` | a DLL the game needs could not be found |
| `0xC0000142` | a DLL the game loads failed to start |
| `0xC000007B` | a DLL the game loads is damaged or for the wrong architecture |
| `0xC0000005` | the game crashed (access violation) |
| `0xC0000409` | the game stopped itself after detecting a problem |
| `0xC0000022` | Windows or security software denied the game access |
| `0xC0000428` | Windows blocked an unsigned DLL the game loads, such as HellForge's mod loader or SDK stand-ins (Smart App Control or a code-integrity policy): see [Smart App Control](#smart-app-control) |
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

## In the game

### Armory icons are blank

The game draws every Armory icon itself (weapons, helmets, armour, capes), a page at a time, and scrolling hands it the
next page. An icon stays blank until the game has drawn its page, and the game waits for the item's textures to load
first. `--toaster_mode` in Settings > Launch options turns texture loading off, so the icons never appear: remove it
(the launcher warns about it under the box). If icons stay blank without it, wait ten seconds on the Armory, then
Save logs to Desktop and send the zip: `modloader.log` then has an "armory thumbnails: no progress" line naming the
items the game is waiting on.

## Playing with friends

Friends need the same gameplay mods, versions and files, and the same HellForge version. When they differ, the join is
refused and the in-game editor shows both players which mods differ. [Using mods](mods.md#playing-with-friends) explains
the modlist code and the notices. With the in-game editor switched off, only the logs say why
(`backend\multiplayer.log` in each session's folder).

Settings > Multiplayer has two switches, Local PlayFab and Lobby and session through Steam friends. Co-op needs both;
they are on unless you switch them off.

## Controllers

With Settings > Sandbox on (the default), Steam Input can't reach the game yet: a Steam Controller, the Steam Deck's
controls and other controllers that work only through Steam Input do nothing in the game, and Steam may send its
desktop keys to it instead (A presses Enter, which opens the chat). While the game runs, the launcher says "Steam Input
can't reach the sandboxed game" and names the controller. Xbox controllers work. With the sandbox off Steam Input works,
but the game runs without the sandbox's protection. [The Steam bridge](steam-bridge.md#controllers-and-steam-input)
explains why.

## Collecting logs

Every session keeps its logs in `logs\sessions\<session>\` in the installation; the newest ten sessions are kept.

To send them to someone, open the Console page and press Save logs to Desktop. It zips the latest session's folder to
your Desktop as `HellForge-logs-<date>-<time>.zip`, with the game's console logs. If a session is still running, files in
use may be left out, and the launcher says how many.

In the text files in the zip, your Windows user folder is written as `%USERPROFILE%` (a Linux home folder as `~`) and the
PC's name as `PC`. Paths stay readable. The zip still names Steam accounts by their id. Files that are not text, such
as a crash dump, go in as they are.

When a session ends in a crash, the Play page offers Save crash report. It saves one zip to your Desktop
(`HellForge-crash-report-<date>-<time>.zip`) made to be attached to a public GitHub issue: a `summary.txt` with
HellForge's, the engine's and the game's versions, your Windows version and graphics driver, the session's settings,
where the game crashed and which mods ran, and the session's logs. On top of the user folder and the PC's name, it
leaves out Steam ids, IP addresses and the names of the other players HellForge's logs give. The game's own logs can
name players in ways HellForge does not know, so read `summary.txt` before you post it. If Crash dump was on, the
launcher asks whether to add the dump, which cannot be scrubbed. Open a GitHub issue then opens the new-issue page;
nothing is sent until you attach the zip yourself.

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
