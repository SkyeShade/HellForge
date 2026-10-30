# HellForge on Linux (Proton)

For players on Linux: the graphical installer, the launcher and the setup wizard run natively, while the Windows session
runner, backend, and game run through Proton inside the offline network namespace. This page covers installing,
updating, setup, sessions, Settings, isolation and what is untested. Linux support is experimental. The native launcher
is the normal route; running the Windows launcher through Proton is the fallback.

Linux support is the work of DreamyBranch and joshua-software-dev, who worked it out and shared it in
[issue 1](https://github.com/SkyeShade/HellForge/issues/1). Thank you both. The setup steps below, and the dependency
check and Steam folder search in `hellforge-linux.sh`, came from DreamyBranch's guided install script in
[issue 11](https://github.com/SkyeShade/HellForge/issues/11); HellForge's own scripts and the native launcher have taken
over from it. The scripts are in `linux/` (in an installation: `app/linux/`).

## What you need

- Steam for Linux, running and signed in to an account that owns HELLDIVERS 2, with at least one Proton installed.
  HELLDIVERS 2 itself does not need to be installed: setup gets the supported build from Steam's installed copy, its
  depot download, or a build patch applied to a newer installed copy.
- About 50 GB of free space while setting up: Steam's download of the supported build and HellForge's copy of the game
  are about 24 GB each. Setup deletes the download after copying unless you untick that. A build patch needs about 2 GB
  more while it is applied.
- The `HellForge-Linux-Installer` artifact from a successful **Build Linux package** run. It contains the native
  launcher and `HellForge-Linux.zip`, which holds the app and no game files. The same run's `HellForge-Linux-package`
  artifact also contains `HellForge-Setup.exe` for Windows; Linux players do not run it.
- These programs: `ip` (from iproute2), `unshare` and `flock` (from util-linux) and `pgrep` (from procps). HellForge
  unpacks `.7z` and `.rar` mods itself, so `bsdtar` is not needed. The Steam bridge is HellForge's own native program
  (`app/cli-linux/HellForge.Cli steam-bridge`), and [account sync](#account-sync) uses HellForge's own native relay, so
  `socat` is not needed for setup, a session or account sync.
  `hellforge-linux.sh` checks for them and prints the install command for your package manager:

  | Package manager | Common distributions | Command |
  |---|---|---|
  | `pacman` | Arch, CachyOS, Manjaro | `sudo pacman -S --needed iproute2 util-linux procps-ng` |
  | `apt-get` | Ubuntu, Debian, Mint, Pop!_OS | `sudo apt install iproute2 util-linux procps` |
  | `dnf` | Fedora, Nobara | `sudo dnf install iproute util-linux procps-ng` |
  | `zypper` | openSUSE | `sudo zypper install iproute2 util-linux procps` |

  On other systems, install the same packages with your package manager.

  `passt` (which provides the `pasta` command) and `python3` are needed only for co-op over Steam relays, the optional
  Settings switch below. `hellforge-linux.sh` warns when `HELLFORGE_STEAM_UDP=1` and either is missing, and the native
  launcher refuses a co-op session before it starts and prints this install command:

  | Package manager | Common distributions | Command for co-op only |
  |---|---|---|
  | `pacman` | Arch, CachyOS, Manjaro | `sudo pacman -S --needed passt python` |
  | `apt-get` | Ubuntu, Debian, Mint, Pop!_OS | `sudo apt install passt python3` |
  | `dnf` | Fedora, Nobara | `sudo dnf install passt python3` |
  | `zypper` | openSUSE | `sudo zypper install passt python3` |

  On other systems, install `passt` and Python 3 with your package manager.

## Building the Linux package without a Windows PC

The engine's session runner and backend are Windows NativeAOT programs, so the complete package must be built on a
Windows runner. You can prepare and trigger that build from Linux; the runner uses only the repository and a small
text-only manifest, not your game files.

From the repository root, point `steam_api` at the supported game's `bin/steam_api64.dll` in your Steam library and
generate the manifest:

```fish
set steam_api "$HOME/.local/share/Steam/steamapps/common/HELLDIVERS 2/bin/steam_api64.dll"
dotnet run --project src/HellForge.SdkShimGen -- manifest "$steam_api" data/steam-export-manifest.json
```

Use the actual path if Steam or the game is installed elsewhere. The command writes only export names, ordinals, data
export flags, and the source DLL's SHA-256. **Commit and push `data/steam-export-manifest.json` only; never add or upload
`steam_api64.dll`.**

On GitHub, open **Actions → Build Linux package → Run workflow** on the branch containing that JSON. The Windows runner
checks the manifest's recorded source hash against the supported-build pin, builds the engine and package, and publishes
`HellForge-Linux-package` (the app zip, engine package, and Windows setup executable) plus the separate
`HellForge-Linux-Installer` graphical installer artifact. Download the latter from that workflow run's Artifacts section,
then follow [the graphical installer](#graphical-installer). The run will stop with a clear error if the manifest is
missing or does not match the supported build.

## Setting it up

### Graphical installer

Download the `HellForge-Linux-Installer` artifact from the successful Actions run and extract its downloaded ZIP with
your desktop's archive manager. Extract the resulting `HellForge-Linux-Installer.tar.gz` with the archive manager too,
then open the extracted folder and launch `HellForge.Launcher`. If your desktop asks, choose **Run** or **Allow
executing file as program**.

The installer reads Steam's folders and lists the Proton versions installed there: every Steam library's
`steamapps/common/Proton*`, the Steam folder's and the system `compatibilitytools.d` folders, and each folder in
`STEAM_EXTRA_COMPAT_TOOLS_PATHS`. Choose one and a prefix folder; the default is
`~/.local/share/hellforge-prefix`. If the version you use is not in the list, choose **Other...** and pick its `proton`
file: it is accepted only when it is an executable file named `proton` with a `files` or `dist` folder beside it, and the
installer says why when it is not. If Steam is outside its usual places, the installer asks for the folder that holds
`steamapps` instead of failing; the choice is saved in the installation and passed to every wrapper run. When the prefix
has no HellForge launcher yet, it runs `proton run wineboot -u` (three minutes at most) and, only when the folder did not
exist or was empty, writes a `.hellforge-created` marker that uninstall later uses for its prefix offer. It extracts the
app into `<prefix>/pfx/drive_c/users/steamuser/Documents/HellForge`, installs the engine package from
`app/engine-package.zip`, saves the Proton and Steam folder choices and starts the native launcher. The setup wizard
starts automatically while the game copy is incomplete. The installer checks the package's required entries before it
prepares the prefix,
and refuses a package that is missing `app/app-build.json`, `app/engine-package.zip`, the native launcher,
`app/capture-linux/HellForge.CaptureProxy`, `app/backend-linux/HellForge.LocalBackend`, `app/linux/hellforge-linux.sh`,
`app/linux/hf-steam.sh`, `app/linux/hf-container.sh`, `app/linux/hf-sandbox-probe`, `app/linux/hf-sandbox`,
`app/linux/allow-capture.sh`, `app/linux/capture-helper.sh` or `app/linux/capture-relay`.

On Ubuntu 24.04 and later, and other systems where `/proc/sys/kernel/apparmor_restrict_unprivileged_userns` is 1, only
allowed programs may make the namespace. Run this once, from your own account:

```fish
sudo bash ~/.local/share/hellforge-prefix/pfx/drive_c/users/steamuser/Documents/HellForge/app/linux/allow-namespace.sh
```

It installs a copy of `unshare` that only root and members of your account's primary group can run, and an AppArmor
profile that lets that copy make namespaces, as Ubuntu does for Steam. For you this lifts Ubuntu's restriction, for any
namespace, not only HellForge's; accounts outside that group keep it. `--remove` undoes it. Run it again after a
util-linux update. `hellforge-linux.sh` tells you when it is needed.

### Installing on another drive

The game copy lives inside the Proton prefix, so a prefix on the drive with room puts the game copy there too. In the
installer, choose a prefix folder there, for example `/mnt/games/hellforge-prefix`; the launcher, game copy, logs,
profiles and mods stay in that prefix. A link above the prefix also works, for example
`ln -s /mnt/games/hellforge-prefix ~/.local/share/hellforge-prefix`: a link above the folder HellForge protects is
resolved and the real folder is used. A link at the installation folder or inside it is refused with
`Linked/reparse paths are not supported: <path>`, because a game copy written through it could land somewhere else. The
same rule covers the game folder and every Steam library HellForge reads.

The supported build's copy is 23.8 GB (about 24 GB). A first setup that copies from Steam's download needs room for that
download too, about another 24 GB, unless setup can use an installed game or a build patch; a patch needs about 2 GB more
while it is applied. Allow at least 50 GB free on the prefix's drive for a first setup from Steam's download. A prefix
backup is about the size of the prefix, and its default folder is `hellforge-backups` beside the prefix, on the same
drive; choose another folder with **Choose folder**.

What stays in the home is small: `~/Desktop/HellForge.desktop`, the shortcut setup writes (a few hundred bytes). The
launcher, game copy, logs, profiles, mods, temporary files and backups are in the prefix. HellForge does not write
`~/.local/share/applications`, and it has no settings folder in the home. A crash report is saved to the Desktop only
when you choose to. A Steam folder you chose outside the usual places is recorded in
`<installation>/steam-selection.json`.

### Setup wizard

The wizard runs natively. At its start, choose the Proton version; the installer's choice is already selected. The list
uses the same search as the installer, and **Other...** picks a `proton` outside it. Setup
saves the choice in `proton-selection.json` and uses it for setup and later game sessions. Settings > Installation >
Check and repair can change it. If Steam is not found, setup asks for the folder that holds `steamapps`, checks it and
saves it in `steam-selection.json`.

Before those checks, setup runs the [sandbox self-test](#phase-2-the-filesystem-sandbox). Its result decides whether
Settings > Sandbox can be offered; a failure leaves the switch off and does not stop the rest of setup. Setup then checks
Steam, the chosen Proton and its own files, and installs the engine package from `app/engine-package.zip`. It then looks
for the game in this order:

1. the installation's own copy, if every file matches the supported build;
2. Steam's installed copy, if it is the supported build;
3. Steam's completed `download_depot` download;
4. a published build patch for the build Steam has installed;
5. otherwise, Steam's download of the supported build (see below).

A build patch is about 1.7 GB. Setup applies it to a copy of Steam's installed game before the ownership check, because
the check needs the supported build's `steam_api64.dll`. The pinned native Linux `hpatchz` runs directly, with the same
arguments as the Windows tool and exit code 24 for a full drive; Steam's installation is only read. If the native tool
cannot be fetched or checked, setup falls back to the pinned Windows `hpatchz.exe` through the selected Proton, with
Wine's `start /wait /b` so no console window opens per file. Every patched file is checked against the supported build's
hash. If the patch fails or the drive is full, setup keeps the files it already patched, remembers the failed patch for
this launcher run and offers Steam's download instead; starting the launcher again tries the patch again. A complete depot
download is used before a patch, as on Windows.

Setup then confirms ownership through the selected Proton. Steam must be running and signed in to an account that owns
HELLDIVERS 2 (or has it through Steam Family Sharing). The launcher starts its Steam probe through
`app/linux/hellforge-linux.sh`, so the check runs inside the same namespace as a session. The probe writes
`logs/setup-ownership.json`, and that report decides. A failure stops setup with `The ownership check did not pass: ...`
followed by the probe's reason or exit code, `The wrapper log says: "..."` when the wrapper stopped before writing the
report (missing programs, AppArmor, no Steam bridge, and so on), and `Make sure Steam is running and signed in to an
account that owns HELLDIVERS 2.`

After the ownership check, setup prepares the optional mod tools through the wrapper: the pinned native Linux Zig (about
55 MB, a `.tar.xz` from ziglang.org) that compiles source mods, and the pinned Windows Zig (about 90 MB) for an
installation that is also used from Windows. A failure is reported in the row but does not stop setup. Setup then copies
the game from the source it chose, or keeps the installation's own copy after verifying it, and checks every file
against Steam's hashes. When the copy came from Steam's depot download and **Delete Steam's download after copying** is
ticked, the download is deleted afterwards. Setup installs the namespace marker only after `hellforge-linux.sh`'s
offline self-test passes. It then creates `~/Desktop/HellForge.desktop`, syncs the bundled mods and shows Ready to play;
your desktop may ask you to allow launching the shortcut.

### Steam's download of the supported build

The Linux Steam client puts what `download_depot` downloads in `<Steam>/steamapps/content/app_553850/`, or in the same
path under `<Steam>/ubuntu12_32/`, `<Steam>/ubuntu12_64/` or `<Steam>/steamrt64/`, because it downloads relative to the
folder its program runs from. The native setup reads these locations directly and does not move the depot folders:

```text
<Steam>/steamapps/content/app_553850/depot_553851/
<Steam>/steamapps/content/app_553850/depot_553853/
<Steam>/steamapps/content/app_553850/depot_553854/
```

When setup needs the download, it shows a card with one `download_depot 553850 <depot> <manifest>` command per depot.
Press **Open the Steam console** (or run `xdg-open steam://open/console`), paste each command into the console and press
Enter, one at a time. The wizard checks the folders every two seconds, shows the progress it can read, and continues by
itself when every depot is complete. When five or fewer files remain, it names them; a file larger than this build's is
another build's file, so delete it and run that depot's command again.

The download can remain in Steam's folder, or the setup option can delete it: **Delete Steam's download after copying**
is ticked by default and removes the build's depot folders from the content locations, and each `app_553850` folder
when it becomes empty.

### Proton-only fallback

The native launcher is the normal route. If it cannot start, the Windows launcher can still run through Proton, but
only inside the network namespace. After the graphical installer has made an installation, run:

```fish
bash "<installation>/app/linux/hellforge-linux.sh"
```

With no program argument the wrapper starts the installation's `HellForge.exe` through Proton inside the namespace.
Setup then runs under Wine and refuses to run outside the namespace with `On Linux, setup runs inside HellForge's
network namespace. Close HellForge and start it with app/linux/hellforge-linux.sh (docs/linux.md), then start setup
there.` Uninstall under Wine says to use the native launcher. `HellForge-Setup.exe` is not used on Linux.

An installation set up on Windows already has the game copy, so it needs no download. Copy the whole installation folder
into a prefix's C: drive, for example to `~/.local/share/hellforge-prefix/pfx/drive_c/HellForge` (the prefix directory
is created by Proton), run that installation's `app/linux/allow-namespace.sh` once if the system needs it, then start
its native launcher. If the native launcher will not run there either, use the wrapper command above. Keep an
installation inside the prefix's C: drive (`pfx/drive_c`): Wine shows your Linux folders as its Z: drive, and HellForge
refuses that drive as a linked path (`Linked/reparse paths are not supported: Z:\`).

## Updating

Run a newer `HellForge-Linux-Installer` the same way as in [the graphical installer](#graphical-installer), with the
same prefix. Close HellForge and any session first: the installer refuses while a process from that installation is
running and names the processes. It finds the existing launcher at `app/launcher-linux/HellForge.Launcher`, checks the
package, unpacks it into `<installation>/temp/app-staging`, moves the old `app` to `temp/app-previous`, puts the new
`app` and the top-level `HellForge.exe` in place, and installs the new engine package. The old app stays until the new
launcher starts; if replacing the app or installing the engine fails, or the new launcher cannot start, the previous
app and engine are put back. After the new launcher starts, the old app is deleted. Because the whole `app` folder is
replaced, files a release no longer ships do not stay behind.

The installer does not compare versions, so pick the package you mean to install. On an existing installation it does
not run `wineboot` again; the Proton you choose is saved for setup and sessions.

A release that pins another HELLDIVERS 2 build normally reopens setup, and that can need another large download or a
build patch (see [the setup wizard](#setup-wizard)). Settings > Installation > Check and repair also installs the engine
package again, verifies the game copy and repeats ownership and the namespace check. Back up the prefix before a big
update if you want a way back ([prefix backup and restore](#prefix-backup-and-restore)).

## Sessions

Press PLAY: the native launcher starts `app/linux/hellforge-linux.sh` with the existing
`app/session/HellForge.Session.exe play ...` command. The launcher itself stays outside the namespace and reads and
writes only HellForge's local files.

The wrapper checks the programs listed above, finds Steam, the prefix and Proton, and refuses with the exact command
when AppArmor restricts user namespaces. Before entering the namespace it fetches the live galactic war
(`HellForge.LocalBackend.exe fetch-live-war`, with a 60-second timeout), the pinned mod tools once per pin (the native
Linux Zig, and the Windows Zig for an installation shared with Windows), and then compiles the enabled source mods with
the native Linux Zig into `temp/mod-builds`. The session finds those builds ready, so it never runs `zig.exe` under
Wine. A failure is logged, and the session continues without that data or those tools (a source mod it could not build
stays off with the normal mod problem).

The wrapper starts a shared bridge to Steam when none is running: `hf-bridge-host.sh` execs
`app/cli-linux/HellForge.Cli steam-bridge host`, which finds the loopback ports that the process named `steam` listens
on and exposes each as a 0600 `hf-steam-<port>.sock` file in `${XDG_RUNTIME_DIR:-/run/user/<uid>}`. `steam-bridge check`
tests whether a bridge still answers. `hf-inner.sh` starts `steam-bridge inner`, which recreates the same loopback ports
inside the namespace and relays each connection to its socket. The relay is HellForge's own native program, so `socat`
is not needed; only loopback (127.0.0.1) and the socket files are used, and no route is added. Sessions that run at the
same time share the bridge: each wrapper holds the users lock (`hf-bridge-users.lock`) shared, and the last one to close
takes it exclusively and stops a bridge the wrapper started. A bridge started by hand with `hf-bridge-host.sh` has no
PID file, is used as it is, and HellForge never stops it. The wrapper records the host's PID in `hf-bridge.pid` and
checks its command line before stopping it: the hand-started `hf-bridge-host.sh`, or the native
`app/cli-linux/HellForge.Cli steam-bridge host` it execs. The wrapper serializes checking, starting and stopping the
bridge with `flock` on `hf-bridge.lock` and `hf-bridge-users.lock`.

The bridge reads the `steam` process's open sockets in `/proc/<pid>/fd`, which only works from Steam's own user
namespace or the one above it. From inside another sandbox, such as NixOS's `steam-run` (measured: same PID namespace,
its own user namespace), Steam is visible but its sockets are not, and the bridge says so. There, start
`app/linux/hf-bridge-host.sh` where Steam runs, for example as a Steam shortcut, and leave it running: the wrapper uses a
bridge that answers. The launcher's leftover-process check before PLAY, and its End button, leave the bridge and the
programs that only start it (Steam's `reaper`) alone. Updating HellForge and backing up or restoring the prefix still
ask you to close it first, as they do for every process of the installation.

The wrapper then runs `hellforge-offline.sh`, which makes the namespace without root, waits up to 20 seconds for a
wineserver of the prefix still running outside it, and refuses to start while one runs. Inside, `hf-inner.sh` opens
the bridge ports and runs the requested program through Proton. The session runner's own network probe runs before the
game starts. Do not start a session runner directly through Proton or run it without the wrapper.

The wrapper runs every Windows program with `DOTNET_SYSTEM_GLOBALIZATION_USENLS=1`, so `HellForge.Cli.exe` (which the
session runner starts for each step, the mod loader starts for a crash dump, and another launcher may start in place of
HellForge.exe) uses Windows' NLS locale functions, which Wine implements itself, rather than ICU. Proton's `icu.dll`
forwards every call to `icuuc68.dll`, which Proton links into the prefix from its own folder. When that DLL does not
load, .NET ends the program at its first culture-aware call (`Cannot get symbol u_charsToUChars from libicuuc`, error
127 in `cli-output.txt`), and the session stops with "Mods could not be loaded". The session runner is built with
invariant globalization and uses neither.

Proton is bounded by `HELLFORGE_PROTON_TIMEOUT` (default `HELLFORGE_WAIT_TIMEOUT`, 12 hours). When that expires,
`hf-inner.sh` first asks a sandboxed session's native supervisor to stop its domain through the same
`stop-observation.signal` STOP uses, and waits up to 30 seconds for it. The domain runs in the overlay prefix's own
wineserver, so the supervisor's domain stop file and process-group TERM/KILL are the only way to end it. Then
`hf-inner.sh` runs the session Proton's `wineserver -k` for the real prefix and ends any game or backend process of the
installation it finds, with a bounded TERM-then-KILL scan, so a game that outlived Proton's script does not keep running
in the namespace.

For a session, `hf-inner.sh` also gives the game the shader caches Steam gives it: the folder
`steamapps/shadercache/553850` in the Steam library that has HELLDIVERS 2, with the same NVIDIA, Mesa, AMD and DXVK
cache variables Steam sets. The game then reuses the shaders Steam has already compiled for it (and, on a Steam Deck, the
ones Valve ships) instead of compiling them again into an empty cache, which shows as short freezes the first time a
planet, the hellpod or a mission loads. Like Steam, it also records the session's new pipelines with Steam's Fossilize
layer, which Steam then compiles in the background; whether Steam shares them is Steam's own shader pre-caching
setting. A variable the player has already set is kept, `DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1` turns the recording
off, and when Steam has no shader cache for the game the wrapper says so and leaves the driver's defaults.

`hellforge-linux.sh` reads these settings from the environment. Each one has a default it finds by itself:

| Variable | Default |
|---|---|
| `PROTON` | the proton script saved in the installation, else the last of every Steam library's `steamapps/common/Proton*`, the Steam folder's `compatibilitytools.d`, each folder in `STEAM_EXTRA_COMPAT_TOOLS_PATHS`, and `/usr/share/steam/compatibilitytools.d` and `/usr/local/share/steam/compatibilitytools.d` (version sort, which puts Proton - Experimental after the numbered versions) |
| `INSTALL` | the installation the script is in |
| `STEAM_COMPAT_CLIENT_INSTALL_PATH` | the folder saved in the installation (`steam-selection.json`), else the first of `~/.local/share/Steam`, `~/.steam/debian-installation`, `~/.steam/steam`, `~/.steam/root` and Flatpak's and Snap's Steam folders that has a `steamapps` folder |
| `STEAM_COMPAT_DATA_PATH` | the prefix the installation is in (the part of its path before `/pfx/drive_c/`); for an installation outside a prefix, HELLDIVERS 2's prefix, `steamapps/compatdata/553850` |
| `STEAM_COMPAT_APP_ID` | `553850` (HELLDIVERS 2), unless explicitly set otherwise |
| `HELLFORGE_STEAM_RUNTIME` | unset: Proton runs inside the Steam Linux Runtime its manifest requires, when installed (see [Proton in the Steam Linux Runtime](#proton-in-the-steam-linux-runtime)). `off` runs Proton on the host instead, for diagnosis only; it changes neither the sandbox nor the offline namespace |

Another launcher (a mod manager, [external-launchers.md](external-launchers.md)) starts sessions through the same
script. It passes the Windows program to run in place of HellForge.exe, for example
`hellforge-linux.sh "$INSTALL/app/cli/HellForge.Cli.exe" play --wait yes`, and keeps the Steam bridge, the war fetch and
the namespace.

Sharing HELLDIVERS 2's prefix (553850) works too, but then nothing else may run in that prefix while HellForge runs. A
Windows program of the prefix running outside would share its network with HellForge's
([one prefix, one network](#one-prefix-one-network)), and the real game, started while HellForge runs, would have no
network.

### Proton in the Steam Linux Runtime

Steam never runs a current Proton directly: Proton's `toolmanifest.vdf` names a Steam Linux Runtime
(`require_tool_appid`; Proton - Experimental and GE-Proton name SteamLinuxRuntime_4 or sniper), and Steam starts Proton
inside that container (pressure-vessel). HellForge does the same. `SteamRuntimeLocator` (and `hf_runtime_*` in
`hf-steam.sh`, the same rules) reads the manifest beside the chosen Proton, finds the runtime app in any Steam library
(`libraryfolders.vdf`, `appmanifest_<appid>.acf`), and builds the command Steam would:
`<runtime>/_v2-entry-point --verb=<verb> -- <proton> <verb> ...`. Every session prints one line saying which it chose:

```
HellForge: Steam Linux Runtime: container "steamrt4" 4.0.20261021.265997 at <folder>; Proton runs inside it.
HellForge: Steam Linux Runtime: host mode: <reason>; Proton runs on the host.
```

Host mode is used when the Proton names no runtime, when the runtime is not installed or its manifest cannot be
reproduced, or with `HELLFORGE_STEAM_RUNTIME=off`; the reason is always printed. A runtime that is present but fails to
start is not retried on the host. The session, the sandboxed domain, the live-war fetch, the Windows mod tools, setup's
`wineboot` and the build-patch fallback all use the chosen mode. HellForge's own steps (`ip`, `unshare`, the Steam
bridge, the native CLI) stay outside the container, which does not have those programs.

The order is fixed: the offline network namespace, then the runtime container (it keeps that namespace: only loopback,
measured), then `hf-sandbox`, then Proton. The reverse is impossible by design, because `hf-sandbox`'s seccomp refuses
new namespaces and pressure-vessel needs them. A helper Proton call (a `reg` command, `wineboot`) never carries both
`SteamAppId` and `SteamGameId`: that pair makes Steam count the call as a HELLDIVERS 2 session, with its post-session
shader processing.

### Started from inside a container

`hellforge-linux.sh` asks one question before it finds Steam or Proton and before any namespace step, in every mode
(a session, `--sandbox-probe` and `--sandbox-self-test`). If HellForge itself was started inside Flatpak
(`/.flatpak-info`), a Snap (`SNAP_NAME`) or Steam's runtime container (`container=pressure-vessel`, or
`/run/host/container-manager` naming it), it refuses with the exact action:

- Flatpak or Snap: start HellForge from its own shortcut, or `app/launcher-linux/HellForge.Launcher`, on the host.
  HellForge already finds Flatpak's and Snap's Steam folders from outside; it does not need to run inside Steam's
  sandbox.
- Steam's runtime container: the Steam entry that started HellForge has a compatibility tool. Open its
  Properties > Compatibility and untick "Force the use of a specific Steam Play compatibility tool". HellForge's
  launcher is a native program and runs on the host.

HellForge must start on the host because it makes the network namespace that keeps the game offline with the host's
programs. Flatpak's seccomp filter blocks the user namespace it needs, a Snap's confinement can block it and lacks the
host's programs, and Steam's runtime container lacks `ip` and `pgrep`. HellForge never offers to bypass a container
(no `flatpak-spawn --host`), and every session keeps the namespace. The native launcher shows the same reason;
`hellforge-linux.sh` writes it to `logs/linux-container-refusal.txt`, setup's "Check the Linux sandbox" row records it
as the reason the sandbox stays off (Settings > Sandbox shows it), and `--sandbox-probe` also writes it as
`firstReason` in `logs/sandbox-probe.json`. Other containers (`container=podman`, `docker`, `oci`, toolbox and distrobox) are not
refused, because they can have the user namespaces a session needs; if a later namespace step fails, the message names
the container.

## SteamOS 3.x

SteamOS 3.x (on the Steam Deck and Steam Machine) is Arch-based. `hellforge-linux.sh` detects it by `ID=steamos` in
`/etc/os-release`, so a missing program is reported with SteamOS's own answer instead of a package-manager command.

### Read-only root

`/` is a small read-only A/B system image. `/home` is a separate writable file system, and `/var` and `/run` are the
other writable places. A normal native installation, its prefix, game copy, logs, desktop shortcut and Steam's shader
cache all live under `/home` or a Steam library (a microSD card on a Deck), so a normal setup and session do not need
to write the system image. Do not install HellForge's programs system-wide on SteamOS: `sudo steamos-readonly disable`
makes the image writable only until the next update, which drops the packages again and can leave an unsupported image.
The only system changes in this tree are the optional `allow-namespace.sh` (for Ubuntu's user-namespace restriction,
which SteamOS is not expected to have) and the account-sync helper; neither is needed for a normal session.

### Programs

| Program | Needed for | Stock SteamOS 3.x |
|---|---|---|
| `ip` (iproute2) | loopback, the veth adapter and the `prohibit` routes | Expected: `iproute2` is in Arch's base set, and the Steam Machine ownership run passed through the namespace. |
| `unshare`, `flock` (util-linux) | the user and network namespace, and the bridge's locks | Expected, for the same reasons. |
| `pgrep` (procps-ng) | waits for the live-war fetch and the mod tools | Expected, for the same reasons. |
| `pasta` (passt), `python3` | co-op over Steam relays, only with the Settings switch on | Expected missing: neither is in Arch's base set. A solo session does not need them. |

The Steam Machine run's `find` and `ownership` steps passed, so at least that image had the required programs and
unprivileged user namespaces. It does not prove that a fresh Deck image is the same, or that the tester had not
installed anything. Treat "Expected" as unverified here.

### First setup in Desktop Mode

Run the installer and the setup wizard in Desktop Mode, not Game Mode. The wizard needs the desktop (and can need a file
picker and Steam's console), and the desktop shortcut it creates is not shown in Game Mode. Once setup says Ready to
play, add the launcher to Steam from Desktop Mode:

1. Steam > Games > **Add a Non-Steam Game to My Library**.
2. Browse to `<installation>/app/launcher-linux/HellForge.Launcher` and add it.
3. Set **Start In** to the installation root (the installer starts the launcher with that working directory). Rename it
   and choose an icon if wanted.

The entry then appears in Game Mode under Library > Non-Steam, and the launcher's PLAY button starts the session as
usual. `steamos-add-to-steam`, if the image has it, may make these steps a single command; that is not verified. Steam
sets `SteamGameId` and `SteamAppId` for a non-Steam entry; the wrapper keeps an inherited `SteamGameId` and sets
`STEAM_COMPAT_APP_ID=553850` itself, but the Game Mode entry has not been tested. The launcher is an Avalonia X11
application: Game Mode's gamescope normally provides XWayland, which this also needs testing. A controller alone cannot
navigate the desktop UI.

### Shader cache

For a session, `hf-inner.sh` gives the game the same shader caches Steam gives it: `steamapps/shadercache/553850` in the
Steam library that has HELLDIVERS 2. Install the game through Steam once (even if HellForge copies its own supported
build) so the library has `appmanifest_553850.acf` and Valve's pre-compiled shaders. The cache can take several GB; it
is Steam's, and prefix backup and uninstall do not include it.

### After a SteamOS update

The inventory seal (C.5a) is bound to `/etc/machine-id`. If a SteamOS update changes it, a snapshot is refused with
"This snapshot was sealed on another system: sync again": run **Sync Inventory** again, which writes a seal for the new
id. Whether SteamOS keeps `/etc/machine-id` across an A/B update is not established; record it before and after one
(the E.3 evidence list) and report the result.

## Sandbox probe

Phase 1 of the planned Linux sandbox is a probe, not a sandbox. It changes nothing: it does not enable the sandbox,
does not refuse a session, and does not change how the game starts. It answers two questions:
does this kernel provide the unprivileged mechanisms the design needs, and do the four canaries hold in a throwaway
child that uses them? The package ships the static x86_64 program as `app/linux/hf-sandbox-probe`; it needs no game,
Steam, Proton, compiler, root or setuid helper.

Run it as your normal user:

```fish
bash "<installation>/app/linux/hellforge-linux.sh" --sandbox-probe
```

The wrapper prints one `PASS`, `FAIL` or `SKIP` line per mechanism and canary, with the evidence it saw, then a final
one-line result. It writes `logs/sandbox-probe.json`; the wrapper prints that path. Exit code 0 means every mechanism is
present and every canary held. Any other result is non-zero, and the last line names the first reason. A `SKIP` is not a
pass.

The mechanisms are Landlock ABI 3 or later, seccomp filter mode and user notification, unprivileged user, mount, PID,
IPC and network namespaces, overlayfs in a user namespace with a read-only lower layer, a read-only bind of `/` with a
writable session bind, private `/tmp` and `/dev/shm`, `openat2`, `close_range`, `PR_SET_NO_NEW_PRIVS`, and the AppArmor
user-namespace sysctl. The canaries then try to write outside the session folders, use `ptrace`/`process_vm_writev`/signals
on a process outside, fork/exec/clone a new process, and reach a loopback TCP port opened outside. Each attempt must fail;
writes in the session folder, thread creation and a signal to the process itself must still work. The probe makes its
temporary folders under `TMPDIR` (or `/tmp`), bounds every child and removes the folders when it exits.

On Ubuntu 24.04 and later, run the existing `allow-namespace.sh` step first when the AppArmor sysctl is 1. After that the
wrapper runs the probe under the same allowed `unshare` copy and profile as a session, so the probe can still make its
nested user namespace; the report records that profile instead of calling the restriction a failure. Without the helper
the probe reports the restriction as `SKIP` and exits non-zero, never as a pass.

### Tester procedure

Run the command above on each target, from Desktop Mode on SteamOS and Bazzite, and attach the report plus the exact
command, `uname -a`, `/etc/os-release`, and
`cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns`:

- Fedora 44;
- Arch or CachyOS;
- Ubuntu 24.04 or later, with `allow-namespace.sh` applied if the sysctl is 1;
- SteamOS 3.x (the root filesystem is read-only and `/home` is separate);
- Bazzite.

Do not run it with `sudo`. If the report says the probe is missing, the package is incomplete; update HellForge. If a
probe child says `SKIP`, include the report unchanged: it is the evidence that a mechanism was unavailable or not
testable, not a substitute for a pass.

The design claims that need the game rather than this probe are items 8-18. They are phase 2-4 work:

- **Phase 2:** 8 (a per-session overlay prefix gets its own `wineserver`), 9 (trusted Windows clients reach a
  `wineserver` across the chosen namespaces), 16 (Wine's job and token calls are not a Linux boundary, and a forced
  `--sandbox` must be refused), 17 (`.NET` file-share locks do not stop Wine writes), and 18 (the copy-back refuses links,
  FIFOs, devices and special files without hanging).
- **Phase 3:** 14 (the complete Wine helper set starts before the game is locked down) and 15 (Wine exceptions, signals
  and threads still work with the seccomp rules).
- **Phase 4:** 10-13 (the broker's named pipe flags, client pid and ring, Valve's DLL through `lsteamclient`, and
  Proton's DLL overrides in the game).

### Phase 2: the filesystem sandbox

Phase 2 ships `app/linux/hf-sandbox`, the program that starts the game in its own domain. With Settings > Sandbox on, a
session runs the copied game through it. The trusted session runner still prepares the copy, the mods, the backend and
the stand-in DLLs in the real prefix. It then writes a validated start request; `hf-inner.sh` starts the native supervisor
in `app/cli-linux/HellForge.Cli`, and the supervisor starts `hf-sandbox`. A second `HellForge.Session.exe` in the overlay
prefix, the domain host, starts the game and applies the memory-only launch edits. The trusted runner never opens the game
process.

In container mode the supervisor resolves the runtime itself (from `PROTON` and Steam's folder, never from the session
request) and starts `setsid <entry point> --verb=waitforexitandrun -- hf-sandbox ... --steam-runtime <runtime folder>
... -- <proton> waitforexitandrun <domain host>`. Inside the container `hf-sandbox` builds the same view as on the host,
plus what pressure-vessel moved: the runtime sockets are resolved before binding and accepted only under the runtime
folder, `/run/pressure-vessel` or `/tmp/.X11-unix`; Xauthority and the PulseAudio cookie, which pressure-vessel
bind-mounts from deleted files, are copied (mode 0400); and the container's `/run/host` system folders (the host's loader
and GPU drivers) and `/var/pressure-vessel/ldso` are bound read-only. `/run/host/user-share`, `user-fonts-cache` and
`etc` are never bound. The supervisor starts the chain from the runtime's folder and without `TEMP`/`TMP`, because
pressure-vessel shares the working and temporary folders as binds, and one inside the prefix stops the overlay
("failed to clone lowerpath", measured). For the same reason a path inside another shared path is not passed to the
container again. A stop signals the domain's process group and then every process below the supervisor, which is a
child subreaper: pressure-vessel's processes start new sessions, so the group alone does not reach them. The readiness
record (schema 2) states the mode and the runtime (its folder and `VERSIONS.txt` SHA-256); a self-test from the other
mode, or from an older runtime, does not count.

`hf-sandbox --session <run folder> --prefix <prefix> -- <command>` makes a new user and mount namespace and runs the
command in a view built for it. Each step fails closed: if one fails, it prints the reason and the command does not run.
The supervisor passes the exact active mod folders, source-built DLLs, the mod loader and `app/session` as read-only
`--bind-ro` inputs. The supervisor drops a bind equal to or inside an earlier bind; hf-sandbox skips one again if a
caller bypasses that check, so a mod folder covers the DLLs inside it. The native side repeats the allow list, refuses
any link in a bind path, and refuses the installation, real prefix, session folder, real home or any folder above them.
There is no command string and no shell: the request names fixed paths and an environment allowlist.

- **Prefix:** the command gets a per-session overlay of the Wine prefix. The real prefix is its read-only lower layer,
  and everything written to `C:` lands in the run folder's `sandbox/upper`, outside the view. An installation inside
  the prefix is hidden in the overlay too: through `C:` only its `game` folder is visible, read-only.
- **Read-only:** the system folders, Proton, the parts of Steam that Proton needs (`ubuntu12_32`, `ubuntu12_64`,
  `linux32`, `linux64`, the Steam runtimes), the game copy, the exact active mod inputs and `app/session`. Steam's own
  folder is not in the view, so its `config` folder, which holds the login data, is absent.
- **Writable:** the session's `game` folder (`gameOut`, reached through `Z:`), a private `/tmp` and `/dev/shm`, Steam's
  HELLDIVERS 2 shader cache, and Proton's `dist.lock` file only.
- **Session services:** the display and audio sockets (Wayland, X11, PipeWire, PulseAudio) are passed in. D-Bus is not.
- **Absent:** the home folder, the rest of the installation (profiles, user data, the account snapshot and the real
  mods folder except the exact active inputs), the real prefix's path, and every device except null, zero, full, random,
  urandom, tty, the GPU nodes and `/dev/shm`.
- **Folder listings:** every folder of the view can be listed (Landlock `READ_DIR` on its root, nothing else). Wine
  finds a path given in another letter case by listing each parent folder, and HELLDIVERS 2 opens its
  `user_settings.config` lowercased; without this the bare folders between the view's mounts refused the listing, the
  settings did not load, and the game showed its first-time setup in every sandboxed session. The view's root is
  `hf-sandbox`'s own tmpfs, so a listing shows only what the sandbox mounted and the folders leading to it; the
  self-test checks that a host file planted in the installation folder is not listed.
- **Kernel locks:** Landlock enforces the same read and write sets again. A seccomp filter denies mounting, namespace
  changes (including `clone` with new-namespace flags), `clone3`, the x32 system calls, eBPF, io_uring and the kernel
  keyring. All capabilities are dropped and `no_new_privs` is set.

The game's user data and environment are copied into `gameOut` before the domain is built. When it exits, the native
`HellForge.Cli sandbox-copy-back` command stages only regular files with one link: links, FIFOs, devices and hard links are
skipped, and a kernel without `openat2` is an error rather than a fallback. The trusted runner merges that staging folder
and applies the editor's saves. The session home holds only `.steam/sdk32` and `.steam/sdk64` links to Steam's runtimes;
there are no `.steam/root` or `.steam/steam` aliases. The supervisor removes `.steam` in its after-domain cleanup. The
session config folder starts empty except for the player's `lsfg-vk/conf.toml` (from `$XDG_CONFIG_HOME` or
`~/.config`, links resolved, at most 64 KiB), copied mode 0600. lsfg-vk is a frame-generation layer among the player's
implicit Vulkan layers, which the view binds; without that copy it wrote its default config and ignored the player's
own, so a game the player had told it to leave alone crashed at start (#112). The game's edits stay in the session copy. The
overlay's `<run>/sandbox/` (including mode-000 `work/work`) is removed last, whether or not the copy-back ran, and an empty
`gameOut/prefix/merged` is removed too.

Every recursive walk over a run folder (`SessionLogs.ExportFiles`, the retention size count, `CrashReport`, `SandboxStats`
and the engine-log scan) skips reparse points, so a link is never followed into Steam's `config` or library. A sandboxed
`result.json` records `gameExitCode`, `gameCrashed` and the loader's first crash line as `loaderCrash`.

Named pipes exist only inside their own Wine server, so in a multiplayer session the domain host serves the backend's
`hellforge-<run>` pipe and the live editor's `hellforge-live-<run>` pipe in the overlay. It relays each connection to the
trusted session runner in the real prefix over loopback TCP; the runner connects it to the real pipe. Both ends use a
per-run token, length-prefixed frames of at most 64 KiB, bounded connection counts and a bounded byte rate. TCP is used
because the backend's HTTP loopback already works under Wine; a Unix socket from a PE process is still an untested Wine
contract.
Co-op, mod sync and the live editor therefore work in a sandboxed session; a solo session has no live-editor pipe, as
before. Steam itself still uses the wrapper's loopback bridge and the forwarding stand-in; this phase does not claim the
broker guarantee G4. G2 and G3 are phase 3.

The self-test is also the default switch. Settings > Sandbox is on when this PC passes it for this HellForge build and
kernel; an update of either reruns it. A failure leaves the switch off and shows the reason. The graphical launcher
records the result, and before every session-starting command the wrapper asks
`HellForge.Cli sandbox-readiness --install <installation>` to refresh a missing or stale record using the installed
app's `app-build.json` and the kernel. That command runs the same self-test; a failure is recorded with its reason, so
an external launcher, CLI `play` or harness session runs unsandboxed and reports it instead of staying silently off.
A sandboxed session still reruns the self-test before Proton, and one that cannot build its domain stops without an
unsandboxed retry.

Check the sandbox on this PC with:

```fish
bash "<installation>/app/linux/hellforge-linux.sh" --sandbox-self-test
```

It builds the same view as a real run from a throwaway tree in the installation's `temp/` folder, with the default
layout (the installation inside the prefix). It checks every boundary listed above, including that a linked bind, the
installation root and `profiles/` are refused and that an allowed `mods/` input is readable but not writable, then removes
the tree. Every check prints `PASS` or `FAIL`; the exit code is 0 only when all of them pass. The launcher and the
native readiness command store the result with the app build and kernel and show the first reason on failure.

## Account sync

The profile page's **Sync Inventory** button opens the normal game through Steam and copies the signed-in account's
inventory and progression into a sealed snapshot. It is the same capture as on Windows, but on Linux the proxy is a
native program and the two system changes are made by one root-owned helper. **Review import**, the import and a backup
restore run in `app/backend-linux/HellForge.LocalBackend`, the local backend's profile commands built for Linux. It
finds the signed-in Steam account with its own probe through the wrapper, because a native Linux program trusts only a
probe it ran itself (`HELLFORGE_STEAM_ACCOUNT` is read only by the Windows backend under Wine).

The capture usually ends while the game is still open. The prefix is not changed while the game runs, so the launcher
waits for HELLDIVERS 2 to close and then removes the one-day CA from its prefix. If the launcher is closed first, its
next start removes it. The CA's private key exists only in the capture proxy's memory, so a leftover CA cannot be used
to intercept the game. Until the game closes, the capture still forwards it: the sync card says to close the game to
finish, PLAY waits with the same message, and the leftover-process check and its End button leave this launcher's own
capture alone (a capture another launcher left is still listed). Leftovers are named in full, not as the kernel's
15-character command name (`HellForge.Captu`).

The first sync asks once to install the helper. Choosing **Install** runs the shipped
`app/linux/allow-capture.sh` through `pkexec /usr/bin/bash ...`, so the desktop's own polkit dialog asks for the
password. The launcher never keeps root or a password, writes no polkit rule that skips the password, and only that
absolute, link-checked shipped script is elevated. The confirmation says what it does: the installer copies
`capture-helper.sh` to `/usr/local/lib/hellforge/capture-helper` and `capture-relay` to
`/usr/local/lib/hellforge/capture-relay`, records each installed copy's SHA-256 in `capture-helper.sha256` and
`capture-relay.sha256`, and adds `/etc/sudoers.d/hellforge-capture`, a rule for that one root-owned helper's exact
`check`, `start`, `stop` and `cleanup` commands. There is no wildcard and no port argument; `start` reads the native
proxy's pid from stdin and the relay's ports are fixed. The account comes from `SUDO_USER` or, under pkexec, from
`PKEXEC_UID`. After the installer exits, the launcher checks both installed hashes again and continues the same sync
without another click; a package update that changes either shipped file makes the next sync offer the same install
again. A cancelled or denied password dialog means "not installed; nothing changed". If `pkexec` is missing or no polkit
authentication agent is running (Game Mode on a Deck, a bare window manager), the launcher shows the terminal command
instead. SteamOS 3.x users without a password are told to run `passwd` first. Running `allow-capture.sh --remove`
removes the rule, the helper, the relay and both hashes, and uninstall offers the same `--remove` through pkexec. The
helper checks its arguments; it cannot run Wine, a game, or a program chosen by the caller. `capture-relay` is a tiny
static C program built from `native/linux-capture`; the helper runs only the installed root-owned copy, never one from
the installation, which any Wine program can write.

HellForge asks Steam's `CompatToolMapping` for the Proton that opens HELLDIVERS 2's own prefix first. Current Steam
often writes no mapping at all, and a mapping can name a tool that is no longer installed; then the prefix's own
`compatdata/553850/config_info` decides. Its first line is the
prefix version and its paths point inside the Proton that wrote it, so HellForge strips the trailing `files/...` or
`dist/...` part, requires the `proton` script beside that root, and requires its `CURRENT_PREFIX_VERSION` to equal the
prefix's `version`. Proton's install `version` file is a build id and is not compared with it. If neither source gives a
Proton, the message says both were looked for and that HELLDIVERS 2 must have been started once through Steam, which is
what writes `config_info`.

For the length of a sync, until the normal game exits, these change:

- `/etc/hosts` gets two marked lines: `# HellForge-CaptureProxy` and
  `127.0.0.1 api.live.prod.thehelldiversgame.com`. The helper rewrites the file next to itself and moves the new file into
  place, keeping its mode, owner and SELinux label, so a kill during the write cannot leave it empty. It removes only its
  own exact marker and the mapping directly below it; a player's line that merely contains the marker text stays.
- The installed relay binds `127.0.0.1:443` as root, drops to `nobody` with `setgroups`/`setgid`/`setuid` (each checked),
  and only then accepts connections. It forwards only to `127.0.0.1:18443`, where the native proxy listens; no argument
  can change either address or port. Nothing is bound beyond loopback, the table of connections is bounded, and the relay
  stops with the helper. The relay is a static musl program, whose account lookup reads only `/etc/passwd`. Fedora Atomic
  systems (Silverblue, Kinoite, Bazzite) keep `nobody` in `/usr/lib/passwd` instead, so the relay reads that root-owned
  file when `/etc/passwd` has no `nobody`. It refuses an entry with uid or gid 0.
- The proxy makes a one-day CA and writes only its public certificate to the sync folder. HellForge starts the Proton
  that Steam's `CompatToolMapping` selects for HELLDIVERS 2, not HellForge's own Proton choice; when the mapping names
  no tool, the prefix's `config_info` decides as described above. It refuses if neither gives a matching Proton. The
  supervisor imports the certificate into HELLDIVERS 2's own Proton prefix,
  `steamapps/compatdata/553850/pfx`, as the Windows Root store's serialized `Blob`
  (`HKEY_LOCAL_MACHINE\Software\Microsoft\SystemCertificates\Root\Certificates\<SHA-1>`), using
  `proton run reg import` with a UTF-16 LE `.reg` file. Import and export stdout and stderr are kept apart and written
  in full to `logs/sync-<utc>/launcher.log`; Proton's OpenXR-Loader, xrizer, fsync and "Proton: Upgrading prefix" lines
  stay but are marked `[known Proton noise]`. The supervisor then waits (bounded) for that prefix's own `wineserver -w`,
  exports the key with `proton run reg export <key> <Z: path in a fresh private temp folder> /y`, parses that UTF-16 LE
  file's `"Blob"=hex:` value, and reads `pfx/system.reg` as a second, independent check. Both must hold the byte-exact
  Blob before the game opens; the failure message says which check disagreed. `proton run reg query` is not used for
  the verdict: the measured Proton gives it 0 bytes of stdout even for a known key. Import/export stdout and stderr stay
  as log evidence only. Removal imports
  a delete `.reg`, then requires `reg export` to write no file **and** `system.reg` to no longer have the key after
  the flush; the export's exit code is only logged, since Proton 11 exits 0 for a missing key. Empty output is never
  read as "gone". A failed check is a failure, never "removed". A running
  wineserver for that prefix (Steam's game or a tool) is refused
  with "close Steam's game/tools in that prefix first"; a record whose prefix differs from the game library's
  `steamapps/compatdata/553850` (for example a second Steam library) is refused before the import. The prefix's
  `version` file is read before and after the import, so a version change is recorded as evidence. Wine runs only while
  the game is closed. The private key stays in the proxy process.
- `logs/sync-<utc>/` is mode `0700`; the inventory, progression, state, certificate and seal files are mode `0600`. The
  snapshot is sealed natively with the machine-id transform; the account is the one the Steam probe confirmed. Captured
  JSON is never put in a log.

The normal host CA bundle, the prefix's `hosts` file, nftables/iptables and Steam launch options are not changed, and Wine
is never started as root. During the sync, the game may still show "Could not connect to Easy Anti Cheat network" after
the login and close itself; the capture is finished by then, as on Windows.

The first real Sync Inventory on Linux (2026-10-29; CachyOS, KDE Wayland, RX 7600, issue #112) stopped at this CA check
before the game opened. The native proxy had written and served its CA ("CA written for the game prefix", "listening on
port 18443"), and Steam mapped HELLDIVERS 2 to `proton-cachyos-11.0-20260506-slr`. The launcher then reported that
Wine's registry did not hold the Blob, although `reg import` had exited 0; the report kept only Proton's
OpenXR-Loader/xrizer stderr.

The reviewer's offline reproduction (`.deepcode/c6/repro.out`, 2026-10-29) found the cause. In all three combinations
(Proton - Experimental on the host, `proton-cachyos-11.0-20260506-slr` on the host, and the same Proton inside
SteamLinuxRuntime_4), `reg import` exited 0 and `system.reg` held the key with a byte-exact Blob, but
`proton run reg query <key> /v Blob` exited 0 with **0 bytes of stdout**. A known key behaved the same; Proton's own
`files/bin/wine reg query` prints it, and `proton run reg export <key> Z:\...\out.reg /y` writes the value. So the CA
import reaches the root store in every combination, and the old check failed on every machine because it parsed console
output that `proton run` does not deliver. The fix above uses the export file and `system.reg`. That answers the
registry-import question; the remaining open check is whether the game trusts that root store during a real sync. Treat
Account Sync on Linux as experimental until that is recorded.

The maintainer's own sync on 2026-10-29 hit the other open case: `config.vdf` had no `CompatToolMapping` at all, neither
for 553850 nor a `0` default, and no `userdata` file had one either. The prefix did record its Proton:
`compatdata/553850/version` and the first line of `compatdata/553850/config_info` were both `11.0-100`, and
`config_info` pointed into `steamapps/common/Proton - Experimental/files/`; that Proton script has
`CURRENT_PREFIX_VERSION="11.0-100"`. The `config_info` fallback above is for that case. Steam's install `version` file
inside the Proton is a build id such as `1791989105 experimental-11.0-20261014-x86_64`, not the prefix version.

If a sync was killed before it could clean up, start HellForge and press **Clean up**, or run:

```fish
sudo /usr/local/lib/hellforge/capture-helper cleanup
```

The launcher checks `/etc/hosts` for the marker on every start and, when the game is not running, runs that cleanup at
startup. If it fails, the card offers **Clean up**. The CA record is under `logs/sync-*/capture-ca.json`; **Clean up**
removes the recorded registry key through Wine's `reg.exe` too.

If HellForge itself is killed mid-sync, the helper watches the native proxy's pid and the proxy's pid is checked against
`/proc/<pid>/cmdline`, so a reused pid does not keep the redirect alive. The helper removes the hosts lines and the relay
when the proxy exits, and has a hard lifetime cap of the proxy's 10-minute timeout plus two minutes (never more than an
hour). The prefix CA is not removed by that helper; if HellForge was killed, the next launcher start finds
`capture-ca.json` and removes the certificate, with **Clean up** as the manual fallback. If the game is still running,
the launcher refuses to touch the prefix with Wine and leaves the record until the next start after the game closes.
Closing the launcher window while a sync runs stops the helper and attempts the CA removal before the window closes.

## Settings

### Check and repair

Settings > Installation > Check and repair reruns the native setup: run the informational
[sandbox probe](#sandbox-probe), choose the Proton version, install the engine package again, confirm Steam ownership,
verify the game copy (copying missing or damaged files from Steam, a complete depot download or a build patch), and check
the namespace marker (installing it, with the self-test, when it is missing). It also renews the desktop shortcut and the
bundled mods. Use it after changing Proton, when the game copy is damaged, or after replacing `app` by hand.

### Prefix backup and restore

Settings > Installation > Prefix backup saves the whole Proton prefix, including the game copy, profiles, mods and
Steam's depot download, to one archive. The default folder is `hellforge-backups` beside the prefix, on the same drive;
choose another with **Choose folder**. The archive is named `hellforge-prefix-<date>-<time>.tar.zst` when `zstd` is on
`PATH`, otherwise `.tar.gz`. Both are ordinary tar streams that standard tools can read. The launcher writes to a `.part`
file and moves it into place when tar exits; cancel deletes the partial file.

Backup refuses while a session or setup runs, while another launcher or any process uses the prefix, when the archive
folder is inside the prefix, and when the backup folder lacks room for the estimated prefix size plus a margin. It also
refuses a prefix that is the home folder, the filesystem root, Steam's folder or under `steamapps/compatdata`, or that
has a link on its path.

**Restore prefix...** lists the archive with tar first and refuses an archive that is not one top-level folder, has an
absolute path or `..`, contains a device or special entry, or has a link that would leave the prefix. Wine's own
`pfx/dosdevices` links and Proton's links to its own files are allowed (Proton links its own files into every prefix):
the target must be below `files` or `dist` in `steamapps/common/Proton*` or `compatibilitytools.d/<name>`. Nothing in
the archive may be written through a link. Restore refuses the same prefix locations and needs the archive's decompressor
(`zstd` or `gzip`) on `PATH`. It unpacks the archive beside the prefix and refuses when there is not room for the
unpacked prefix while the current one is kept. The launcher then closes; a helper waits for the prefix to be free,
moves the current prefix aside, moves the restored one into place, removes the old one, and starts the launcher again.
If the move fails, the old prefix is put back. The current prefix is not touched until the restored copy is complete.

### Removing HellForge

Settings > Installation > Uninstall HellForge... (the confirmation button says **Uninstall this installation**) shows
exactly what will be removed. Steam's depot download is a separate checkbox, off by default. After you confirm, it first
removes what HellForge put outside the installation:

- Steam's depot download of the supported build, only when its checkbox is ticked: the `depot_<id>` folders under
  `<Steam>/steamapps/content/app_553850`, also looked for under `ubuntu12_32`, `ubuntu12_64` and `steamrt64`, plus the
  `state_553850_*.patch` download-state files in or next to that folder. The confirmation lists those paths under the
  checkbox and warns that a depot download made for another HellForge installation in the same Steam library is removed
  too. While the box is unticked, they are left where they are.
- The removed shell installer's app-menu entry, `~/.local/bin/hellforge-linux` command and
  `~/.config/hellforge-linux/config` saved settings, when the saved settings name this prefix.
- The Account Sync helper, relay, their hashes and `/etc/sudoers.d/hellforge-capture`, through the same pkexec
  confirmation and password dialog. If no polkit agent is available, the uninstall text shows the terminal `--remove`
  command and the helper files are left for it.

Then the launcher closes and a helper deletes the installation folder (`game`, `profiles`, `mods`, `userdata`, `logs`,
`temp`, `app` and the namespace marker) and `~/Desktop/HellForge.desktop` when it still has HellForge's exact `Exec=`
line. It leaves Steam, HELLDIVERS 2, your Proton versions, anything that does not prove it belongs to this installation,
backups in the backup folder (`hellforge-backups` beside the prefix unless you chose another), Steam's own update and
download state elsewhere under `steamapps`, and the bridge's lock files in `${XDG_RUNTIME_DIR:-/run/user/<uid>}` (empty
and harmless).

If `/etc/apparmor.d/hellforge-unshare` or `/usr/local/lib/hellforge/unshare` exists, the confirmation lists them and
plain commands to remove them. The launcher cannot run sudo; **Copy sudo commands** copies them. When both exist they
are:

```fish
sudo apparmor_parser -R /etc/apparmor.d/hellforge-unshare 2>/dev/null || true
sudo rm -f -- /etc/apparmor.d/hellforge-unshare /usr/local/lib/hellforge/unshare
sudo rmdir /usr/local/lib/hellforge 2>/dev/null || true
```

They work after the installation is gone. `sudo bash <installation>/app/linux/allow-namespace.sh --remove` does the same
before uninstall.

The Proton prefix is left unless the native installer made it. The installer writes `.hellforge-created` into a prefix
that did not exist or was empty before, and the marker names the prefix. Uninstall offers the prefix only when that
marker matches, the prefix still contains this installation, it is not the home folder, the filesystem root, Steam's
folder or under Steam's `compatdata`, and no process is using it. Links inside the prefix do not refuse it: Proton links
its own files into every prefix, and the helper deletes with `rm -rf --one-file-system`, which removes a link without
following it. A link above the prefix, such as `/home -> /var/home`, is resolved first, and the real folder is offered
and deleted. Only a prefix or marker that is itself a link is refused. The checkbox is off by default; ticking it
deletes the whole prefix, including the game copy, every profile and every mod. A prefix made by HELLDIVERS 2, one you
chose as an existing non-empty folder, or one from an earlier HellForge version has no marker and is never offered;
delete it by hand if you want it gone.

## Building mods with native Zig

On Linux, a source mod is compiled by the pinned native Linux Zig (Zig 0.17.0, `app/sdk/tools.json`), never the
pinned Windows `zig.exe` under Wine (issue 81). The wrapper installs it into `<installation>/tools/zig-linux/zig`
before the session and runs the native build agent, which calls the same `ModBuilds` code a session does:

```fish
app/cli-linux/HellForge.Cli build-source-mods --install <installation>
```

That builds every enabled source mod into `temp/mod-builds` with the cache key and `record.json` a session checks. The
session then finds the DLLs ready and does not compile. The key names the compiler (`zig 0.17.0 /
x86_64-windows-gnu`) rather than its archive, so a Windows session and a native Linux build of the same sources share
it. A mod that does not compile is recorded with its compiler message and stays off for that session, as before.

**Build now** in a mod's ⋮ menu runs the same native agent for that mod on Linux
(`app/cli-linux/HellForge.Cli build-mod --mod <mod folder>`), so it fills the build the next session loads. It is
enabled when the pinned native Zig is installed; `$ZIG`, `~/.local/share/zig-native/zig` and `zig` on `PATH` are
fallbacks for a compiler installed elsewhere.

`app/linux/hf-build-mod.sh` remains the authoring helper for mods outside an installation:

- it reads `CMakeLists.txt` for the first `hellforge_add_mod(...)` call, for the language and the source list;
- without one, it uses the `dll` name in `hellforge-mod.json` and `src/*.cpp` or `src/*.c`;
- it compiles an `x86_64-windows-gnu` DLL with the SDK headers from `app/sdk`, the same flags as a session (`-O2
  -Wall -Werror`, C++17 or C11) and a cache under the installation, and copies the manifest, the DLL and `data/` into
  `<installation>/mods/<mod-folder-name>/`. For a mod already in `mods/`, it delegates to the native agent instead.

It uses `<installation>/tools/zig-linux/zig` first, then `$ZIG`, `~/.local/share/zig-native/zig` and `zig` on `PATH`.
For a mod outside an installation:

```fish
bash "$INSTALL/app/linux/hf-build-mod.sh" <mod-folder>
```

A synced source mod that is first downloaded from the host during a session cannot be prebuilt by the wrapper: it stays
off for that session with the normal mod problem, and the next start can build it when the synced copy is still in
`mods/.synced`.

## How it keeps the game off the internet

On Windows the copied game is kept off the internet by a network block in the Windows Filtering Platform. Wine has
none, so on Linux the session runner, backend and game run inside a network namespace: a private network with no route
out. `linux/hellforge-offline.sh` makes it without root, with:

- loopback, for the local backend and the bridge to Steam;
- a virtual network adapter (`veth`), so Wine and the game see an ordinary network;
- "prohibit" default routes for IPv4 and IPv6, so every outside connection is refused at once. That is the same refusal
  the Windows block gives, which the session's network probe checks before every session;
- a self-test that refuses to start if a connection to 1.1.1.1 gets through.

The native launcher stays outside the namespace. The session runner, local backend and game run inside it. The launcher
starts each session through the wrapper and checks that the wrapper and network-block marker exist. The wrapper enforces
the namespace and its self-test, and the session runner checks the isolation before starting the game
(`src/HellForge.Core/LinuxNetworkBlock.cs`), including for installations copied from Windows.

The game still needs the Steam client, which runs outside with its normal network. `linux/hf-bridge-host.sh` runs
`app/cli-linux/HellForge.Cli steam-bridge host`, which listens on the loopback ports the Steam client listens on and
exposes each as a 0600 unix socket (files, which cross into the namespace). `linux/hf-inner.sh` runs `steam-bridge
inner`, which opens the same loopback ports inside and relays each connection to its socket. Both sides use only
127.0.0.1 and the socket directory; no route is added, and `socat` is not used. The TCP relays use `nodelay`: without
it the game's many small calls to Steam were held back, and the frame rate dropped badly.

The namespace is a network block, not a sandbox. It holds back the game's own connections, but programs the game asks to
act for it outside still can: the Steam client through the bridge, the desktop session's services through their
sockets, and, on systems that resolve names through systemd-resolved, name lookups. The Windows block is the same kind of
block.

The galactic war is fetched just before the namespace is entered (`HellForge.LocalBackend.exe fetch-live-war`), and the
session serves that copy. With "Follow the live galactic war", the war is the one from launch time and moves on at the
next launch.

The mod tools are fetched there too, once (`HellForge.Cli.exe mod-tools` for the Windows Zig and the native
`app/cli-linux/HellForge.Cli mod-tools` for `tools/zig-linux`), and again only when a release pins another Zig. The
native agent then compiles the enabled source mods before the namespace, where a download would be refused ("Access
denied. (ziglang.org:443)"). The Mods page's **Install mod tools** installs the same native Zig directly; the wrapper
fetches it before a session too.

### Co-op on Linux: opening UDP (optional)

**Experimental.** The game starts and runs with this, and the network check below passes. The restricted version below joined Windows players' games and played a full mission (reported on issue 22 by two testers, one of them as the fourth player in a lobby of Windows players). Please report what you see, good or bad, with `backend/multiplayer.log` from the session.

Steam's networking, which co-op uses, runs inside the backend's own process and reaches Valve's relays over UDP, so the
default namespace (no route at all) cannot carry it. In the native launcher, turn on Settings > Multiplayer >
**Co-op over Steam relays (UDP to Valve's relays only)** and press PLAY. It is off by default. The launcher sets
`HELLFORGE_STEAM_UDP=1` for that session's wrapper only, not for the Steam ownership check. For the wrapper directly or
an external launcher, `HELLFORGE_STEAM_UDP=1 bash linux/hellforge-linux.sh` does the same.

With it on, `hellforge-offline.sh` attaches `pasta` (package `passt`) to the same namespace from outside (running the
game under `pasta -- command` gave it its own pid, mount, ipc and hostname namespaces and it quit during start), which
gives it a real route. It runs with `--no-netns-quit`: by default pasta watches the namespace holder's `/proc/<pid>/ns`
folder so it can quit with it, and Fedora's `passt-selinux` policy (Fedora, Bazzite, Nobara and other SELinux systems)
denies that with "netns dir open: Permission denied", leaving the namespace without a route. The wrapper ends pasta
itself when the session ends. Two policy rules then close it again: `ip rule add ipproto tcp prohibit` refuses every TCP
connection with the same WSAEACCES, so the session's network probe passes as before, and UDP is allowed only to Valve's
relays, address and port range each, from the list Valve publishes for the game (`ISteamApps/GetSDRConfig/v1`, app
553850), fetched at launch while this PC still has its network (the last one is kept in
`~/.cache/hellforge/sdr-relays.txt` for a launch without a network; with neither, it does not start). Every other UDP
destination, name lookups included, is refused, and the self-test refuses to start if one works. It needs `python3` for
the list; without `pasta` or `python3`, the launcher refuses before the session starts and the wrapper warns, with the
package-manager commands in [What you need](#what-you-need). Co-op through Steam's relays should still work; a direct
peer-to-peer path between two players is not allowed and Steam falls back to a relay. Checked in the namespace: UDP to a
relay address and port goes out, the same address on another port, 1.1.1.1 and 8.8.8.8 are refused, TCP refused,
loopback fine. A solo session's packet capture (about 5 minutes) showed only backend UDP to Valve relays and nothing
from the game process.

## One prefix, one network

Wine opens a Windows program's sockets in the prefix's `wineserver`, so a program gets the wineserver's network, not its
own. A wineserver of HellForge's prefix still running outside the namespace would give the programs started inside the
internet. `hellforge-offline.sh` therefore waits up to 20 seconds for one to end (it does, a few seconds after its last
program) and refuses to start while one runs. Should one start in the moment between that check and the namespace, the
session's network probe (in the session runner, before the game starts) refuses the session. A prefix of HellForge's own
avoids all of this.

## Problems players have reported

These come from the testers in [issue 1](https://github.com/SkyeShade/HellForge/issues/1). None of them is fixed yet.

- The game closes when you pick a mission on the ship's map. DreamyBranch found it happens after hovering over the
  mission's card, and that clicking a mission quickly, without hovering, gets you into the Hellpod. For other players
  it closes as soon as the ship starts to move, and one player saw the unmodified game close a few minutes into missions
  under Proton too. The cause is not known.
- HellForge stops at start with a connection error although Steam is running and signed in. Two testers saw this after
  a Vulkan overlay such as MangoHud, or a Proton build that changes the overlays, had changed the prefix. Starting
  HellForge once with another Proton (`PROTON=...`), then with the usual one again, cleared it.
- Co-op between a Linux player and Windows players did not connect in joshua-software-dev's tests.

## Tested so far

In an Ubuntu 26.04 VM with plain Wine 10 (no Steam, no GPU; 2026-10-21):

- the namespace as a normal user, with `allow-namespace.sh` where AppArmor restricts it;
- Windows programs inside refused with WSAEACCES;
- the war fetched outside and served inside without a request to the mirror;
- the session runner's network probe;
- the refusal while a wineserver of the prefix runs outside;
- the Steam bridge (against a stand-in for Steam), started and stopped by `hellforge-linux.sh`.

The native launcher UI was started successfully on CachyOS. The game itself, Proton and the Steam client were tested by
the testers in issue 1. This environment has no game files or live Steam client, so native setup, the Proton ownership
probe, and a real game session have not been tested here.

## Not known yet

- Native setup's direct depot reads, engine-package extraction, and the Proton ownership probe still need a full test on
  a Linux machine with Steam and the game depot download.
- Co-op on Linux. Friends' games are reached through Steam's networking, which runs in the Steam client outside the
  namespace, but the one test so far did not connect (see [above](#problems-players-have-reported)).
- Steam as a Flatpak or Snap (its own `/tmp` and paths), and SteamOS 3.x (see [SteamOS 3.x](#steamos-3x)), including the
  Steam Deck and Game Mode, are untested. Starting HellForge itself inside Flatpak, Snap or Steam's runtime container is
  refused with the action in [Started from inside a container](#started-from-inside-a-container); that path has not
  been tried on a real Flatpak or Snap either.
- The Steam ownership check under Proton. Setup and sessions run it, as on Windows (a session reuses a check that passed
  within 7 days for the same installation, account and build, where it can see Steam's signed-in account): under Wine, HellForge starts
  its Steam probe (`app\steamprobe\HellForge.SteamProbe.exe`) directly, because Wine's `powershell.exe` runs nothing.
  The probe must confirm the game on the signed-in Steam account, or setup and the session stop. It has not yet been
  run on a Linux machine, so if setup or a session stops at "The ownership check did not pass", please report it with
  the logs.
- The graphical installer's install and update path (the staged app swap, rollback and new engine install), build
  patches applied through Proton, prefix backup and restore, and uninstall's outside cleanup (including the AppArmor
  commands) and prefix deletion offer have not been tried with a real game and Steam on a Linux machine.
- Account Sync on Linux worked end to end on one PC (2026-10-29: Fedora 44, Proton - Experimental): the launcher found
  the game's Proton from the prefix, installed the helper through pkexec, the CA reached the prefix's `Root` store, the
  game logged in through the capture (732 items and progression), and Review import and the import ran in the native
  backend. A tester's earlier run (issue #112) stopped at the old CA check, which is fixed. It has not been tried on
  other distributions or on SteamOS yet.
- Proton inside the Steam Linux Runtime has been tested on one PC (Fedora 44, SteamLinuxRuntime_4): a sandboxed session
  reached the ship with co-op, the live editor and STOP working, and no domain process outlived STOP. Ubuntu's AppArmor
  user-namespace restriction with the runtime container is untested: if a sandboxed session there cannot start the
  container, it fails closed. Please report the result with the logs.

## Helping with Linux

Linux support grows from what Linux players try. If you get further than this page, find a fix, or test on a
distribution or Steam setup listed above as untested, open an issue with your logs or send a pull request: the scripts
are in [`linux/`](../linux) and the Wine checks in `src/HellForge.Core/LinuxNetworkBlock.cs`. See
[CONTRIBUTING.md](../CONTRIBUTING.md) for how to build and test.

If a session refuses to start, see [troubleshooting](troubleshooting.md). For every other page, see the
[docs map](README.md).
