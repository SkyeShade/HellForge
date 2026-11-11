# HellForge on Linux (Proton)

<!-- diataxis: how-to -->

For players on Linux: installing, updating and setting up HellForge, sessions, co-op, account sync and Settings.
SteamOS has [its own page](linux-steamos.md), and what is untested is in [Testing HellForge on Linux](linux-testing.md).
Linux support is experimental. The native launcher is the normal route; running the Windows launcher through Proton is
the fallback.

The graphical installer, the launcher and the setup wizard run natively. The Windows session runner, backend and game
run through Proton inside the offline network namespace. How that works is in
[How HellForge isolates the game on Linux](linux-isolation.md), [The Linux sandbox](linux-sandbox.md) and
[Account sync on Linux](linux-account-sync.md).

Linux support is the work of DreamyBranch and joshua-software-dev, who worked it out and shared it in
[issue 1](https://github.com/SkyeShade/HellForge/issues/1). Thank you both. The setup steps below, and the dependency
check and Steam folder search in `hellforge-linux.sh`, came from DreamyBranch's guided install script in
[issue 11](https://github.com/SkyeShade/HellForge/issues/11). HellForge's own scripts and the native launcher have taken
over from it. The scripts are in `linux/` (in an installation: `app/linux/`).

## What you need

- Steam for Linux, running and signed in to an account that owns HELLDIVERS 2, with at least one Proton installed.
  HELLDIVERS 2 itself does not need to be installed. Setup gets the supported build from Steam's installed copy, its
  depot download, or a build patch applied to a newer installed copy.
- About 50 GB of free space while setting up. Steam's download of the supported build and HellForge's copy of the game
  are about 24 GB each. Setup deletes the download after copying unless you untick that. A build patch needs about 2 GB
  more while it is applied.
- The `HellForge-Linux-Installer` artifact from a successful **Build Linux package** run. It contains the native
  launcher and `HellForge-Linux.zip`, which holds the app and no game files. The same run's `HellForge-Linux-package`
  artifact also contains `HellForge-Setup.exe` for Windows; Linux players do not run it.
- The programs below.

### Programs

HellForge needs `ip` (from iproute2), `unshare` and `flock` (from util-linux) and `pgrep` (from procps).
`hellforge-linux.sh` checks for them and prints the install command for your package manager:

| Package manager | Common distributions | Command |
|---|---|---|
| `pacman` | Arch, CachyOS, Manjaro | `sudo pacman -S --needed iproute2 util-linux procps-ng` |
| `apt-get` | Ubuntu, Debian, Mint, Pop!_OS | `sudo apt install iproute2 util-linux procps` |
| `dnf` | Fedora, Nobara | `sudo dnf install iproute util-linux procps-ng` |
| `zypper` | openSUSE | `sudo zypper install iproute2 util-linux procps` |

On other systems, install the same packages with your package manager.

HellForge unpacks `.7z` and `.rar` mods itself, so `bsdtar` is not needed. `socat` is not needed for setup, a session or
account sync. The Steam bridge is HellForge's own native program (`app/cli-linux/HellForge.Cli steam-bridge`), and
[account sync](#account-sync) uses HellForge's own native relay.

`passt` (which provides the `pasta` command) and `python3` are needed only for
[co-op over Steam relays](#co-op-on-linux-opening-udp), which is on by default. Without them a session still starts,
without co-op. `hellforge-linux.sh` warns when `HELLFORGE_STEAM_UDP=1` and either is missing. The native launcher says
so in its log with this install command:

| Package manager | Common distributions | Command for co-op only |
|---|---|---|
| `pacman` | Arch, CachyOS, Manjaro | `sudo pacman -S --needed passt python` |
| `apt-get` | Ubuntu, Debian, Mint, Pop!_OS | `sudo apt install passt python3` |
| `dnf` | Fedora, Nobara | `sudo dnf install passt python3` |
| `zypper` | openSUSE | `sudo zypper install passt python3` |

On other systems, install `passt` and Python 3 with your package manager.

## Building the Linux package without a Windows PC

The engine's session runner and backend are Windows NativeAOT programs, so the complete package must be built on a
Windows runner. You can prepare and trigger that build from Linux. The runner uses only the repository and a small
text-only manifest, not your game files.

1. From the repository root, point `steam_api` at the supported game's `bin/steam_api64.dll` in your Steam library and
   generate the manifest. Use the actual path if Steam or the game is installed elsewhere.

   ```fish
   set steam_api "$HOME/.local/share/Steam/steamapps/common/HELLDIVERS 2/bin/steam_api64.dll"
   dotnet run --project src/HellForge.SdkShimGen -- manifest "$steam_api" data/steam-export-manifest.json
   ```

   The command writes only export names, ordinals, data export flags, and the source DLL's SHA-256.
2. **Commit and push `data/steam-export-manifest.json` only; never add or upload `steam_api64.dll`.**
3. On GitHub, open **Actions → Build Linux package → Run workflow** on the branch containing that JSON.
4. Download `HellForge-Linux-Installer` from that workflow run's Artifacts section, then follow
   [the graphical installer](#graphical-installer).

The Windows runner checks the manifest's recorded source hash against the supported-build pin, then builds the engine
and package. It publishes `HellForge-Linux-package` (the app zip, engine package, and Windows setup executable) plus the
separate `HellForge-Linux-Installer` graphical installer artifact. The run stops with a clear error if the manifest is
missing or does not match the supported build.

## Setting it up

### Graphical installer

1. Download the `HellForge-Linux-Installer` artifact from the successful Actions run.
2. Extract its downloaded ZIP with your desktop's archive manager. Extract the resulting
   `HellForge-Linux-Installer.tar.gz` with the archive manager too.
3. Open the extracted folder and launch `HellForge.Launcher`. If your desktop asks, choose **Run** or **Allow executing
   file as program**.
4. Choose a Proton version and a prefix folder. The default prefix is `~/.local/share/hellforge-prefix`.

The installer reads Steam's folders and lists the Proton versions installed there:

- every Steam library's `steamapps/common/Proton*`;
- the Steam folder's and the system `compatibilitytools.d` folders;
- each folder in `STEAM_EXTRA_COMPAT_TOOLS_PATHS`.

If the version you use is not in the list, choose **Other...** and pick its `proton` file. It is accepted only when it
is an executable file named `proton` with a `files` or `dist` folder beside it, and the installer says why when it is
not. If Steam is outside its usual places, the installer asks for the folder that holds `steamapps` instead of failing.
The choice is saved in the installation and passed to every wrapper run.

The installer then:

1. Checks the package's required entries, before it prepares the prefix (the list is below).
2. Runs `proton run wineboot -u` (three minutes at most) when the prefix has no HellForge launcher yet. Only when the
   folder did not exist or was empty, it writes a `.hellforge-created` marker that uninstall later uses for its prefix
   offer.
3. Extracts the app into `<prefix>/pfx/drive_c/users/steamuser/Documents/HellForge`.
4. Installs the engine package from `app/engine-package.zip`.
5. Saves the Proton and Steam folder choices and starts the native launcher. The setup wizard starts automatically while
   the game copy is incomplete.

It refuses a package that is missing any of these entries:

| Entries the package must have |
|---|
| `app/app-build.json`, `app/engine-package.zip`, the native launcher |
| `app/capture-linux/HellForge.CaptureProxy`, `app/backend-linux/HellForge.LocalBackend` |
| `app/linux/hellforge-linux.sh`, `app/linux/hf-steam.sh`, `app/linux/hf-container.sh`, `app/linux/hf-wineserver.sh` |
| `app/linux/hf-sandbox-probe`, `app/linux/hf-sandbox` |
| `app/linux/allow-capture.sh`, `app/linux/capture-helper.sh`, `app/linux/capture-relay` |

On Ubuntu 24.04 and later, and other systems where `/proc/sys/kernel/apparmor_restrict_unprivileged_userns` is 1, only
allowed programs may make the namespace. `hellforge-linux.sh` tells you when it is needed. Run this once, from your own
account:

```fish
sudo bash ~/.local/share/hellforge-prefix/pfx/drive_c/users/steamuser/Documents/HellForge/app/linux/allow-namespace.sh
```

It installs a copy of `unshare` that only root and members of your account's primary group can run. It also installs an
AppArmor profile that lets that copy make namespaces, as Ubuntu does for Steam. For you this lifts Ubuntu's restriction
for any namespace, not only HellForge's; accounts outside that group keep it. `--remove` undoes it. Run it again after a
util-linux update.

### Installing on another drive

The game copy lives inside the Proton prefix, so a prefix on the drive with room puts the game copy there too. In the
installer, choose a prefix folder there, for example `/mnt/games/hellforge-prefix`. The launcher, game copy, logs,
profiles and mods stay in that prefix.

A link above the prefix also works, for example `ln -s /mnt/games/hellforge-prefix ~/.local/share/hellforge-prefix`. A
link above the folder HellForge protects is resolved and the real folder is used. These are refused:

- A link at the installation folder or inside it, with `Linked/reparse paths are not supported: <path>`. A game copy
  written through it could land somewhere else. The same rule covers the game folder and every Steam library HellForge
  reads.
- For the sandbox self-test, `/run`, because the sandbox gives the game its own private `/run`. The exception is a drive
  your desktop mounted under `/run/media/<user>/<drive>` (KDE, GNOME, SteamOS), which works, sandbox included.

The supported build's copy is 23.8 GB (about 24 GB). A first setup that copies from Steam's download needs room for that
download too, about another 24 GB, unless setup can use an installed game or a build patch. Allow at least 50 GB free on
the prefix's drive for a first setup from Steam's download. A prefix backup is about the size of the prefix. Its default
folder is `hellforge-backups` beside the prefix, on the same drive; choose another folder with **Choose folder**.

What stays in the home is small: `~/Desktop/HellForge.desktop`, the shortcut setup writes (a few hundred bytes). The
launcher, game copy, logs, profiles, mods, temporary files and backups are in the prefix. HellForge does not write
`~/.local/share/applications`, and it has no settings folder in the home. A crash report is saved to the Desktop only
when you choose to. A Steam folder you chose outside the usual places is recorded in
`<installation>/steam-selection.json`.

### Setup wizard

The wizard runs natively. In order, it:

1. Asks for the Proton version, with the installer's choice already selected. The list uses the same search as the
   installer, and **Other...** picks a `proton` outside it. Setup saves the choice in `proton-selection.json` and uses
   it for setup and later game sessions. Settings > Installation > Check and repair can change it.
2. Runs the [sandbox self-test](#sandbox). Its result decides whether Settings > Sandbox can be offered. A failure
   leaves the switch off and does not stop the rest of setup.
3. Checks Steam, the chosen Proton and its own files. If Steam is not found, setup asks for the folder that holds
   `steamapps`, checks it and saves it in `steam-selection.json`.
4. Installs the engine package from `app/engine-package.zip`.
5. Looks for the game in this order:
   1. the installation's own copy, if every file matches the supported build;
   2. Steam's installed copy, if it is the supported build;
   3. Steam's completed `download_depot` download;
   4. otherwise, [Steam's download of the supported build](#steams-download-of-the-supported-build).
6. Confirms ownership through the selected Proton ([below](#the-ownership-check)).
7. Prepares the optional mod tools through the wrapper. These are the pinned native Linux Zig (about 55 MB, a `.tar.xz`
   from ziglang.org) that compiles source mods, and the pinned Windows Zig (about 90 MB) for an installation that is
   also used from Windows. A failure is reported in the row but does not stop setup.
8. Copies the game from the source it chose, or keeps the installation's own copy after verifying it. It checks every
   file against Steam's hashes. When the copy came from Steam's depot download and **Delete Steam's download after
   copying** is ticked, the download is deleted afterwards.
9. Installs the namespace marker, only after `hellforge-linux.sh`'s offline self-test passes.
10. Creates `~/Desktop/HellForge.desktop`, syncs the bundled mods and shows Ready to play. Your desktop may ask you to
    allow launching the shortcut.

#### The ownership check

Steam must be running and signed in to an account that owns HELLDIVERS 2 (or has it through Steam Family Sharing). The
launcher starts its Steam probe through `app/linux/hellforge-linux.sh`, so the check runs inside the same namespace as a
session. The probe writes `logs/setup-ownership.json`, and that report decides. A failure stops setup with:

- `The ownership check did not pass: ...`, followed by the probe's reason or exit code;
- `The wrapper log says: "..."`, when the wrapper stopped before writing the report (missing programs, AppArmor, no
  Steam bridge, and so on);
- `Make sure Steam is running and signed in to an account that owns HELLDIVERS 2.`

### Steam's download of the supported build

When setup needs the download, it shows a card with one `download_depot 553850 <depot> <manifest>` command per depot.

1. Press **Open the Steam console**, or run `xdg-open steam://open/console`.
2. Paste each command into the console and press Enter, one at a time.
3. Wait. The wizard checks the folders every two seconds, shows the progress it can read, and continues by itself when
   every depot is complete.

When five or fewer files remain, the wizard names them. A file larger than this build's is another build's file, so
delete it and run that depot's command again.

The Linux Steam client downloads relative to the folder its program runs from. So `download_depot` often writes to
`<Steam>/ubuntu12_32/steamapps/content/app_553850/` (a Steam Deck does), and sometimes to the same path under
`<Steam>/ubuntu12_64/` or `<Steam>/steamrt64/`, or to `<Steam>/steamapps/content/app_553850/`. Setup's card names the
first and the last before the download, and the folder it found once Steam has started. The native setup reads these
locations directly and does not move the depot folders:

```text
<Steam>/ubuntu12_32/steamapps/content/app_553850/depot_553851/
<Steam>/ubuntu12_32/steamapps/content/app_553850/depot_553853/
<Steam>/ubuntu12_32/steamapps/content/app_553850/depot_553854/
```

The download needs about 24 GB on the drive that holds the Steam folder, wherever HellForge is installed.

The download can remain in Steam's folder, or setup can delete it. **Delete Steam's download after copying** is ticked by
default. It removes the build's depot folders from the content locations, and each `app_553850` folder when it becomes
empty.

### Proton-only fallback

The native launcher is the normal route. If it cannot start, the Windows launcher can still run through Proton, but
only inside the network namespace. After the graphical installer has made an installation, run:

```fish
bash "<installation>/app/linux/hellforge-linux.sh"
```

With no program argument the wrapper starts the installation's `HellForge.exe` through Proton inside the namespace.
Setup then runs under Wine, and refuses to run outside the namespace with `On Linux, setup runs inside HellForge's
network namespace. Close HellForge and start it with app/linux/hellforge-linux.sh (docs/linux.md), then start setup
there.` Uninstall under Wine says to use the native launcher. `HellForge-Setup.exe` is not used on Linux.

An installation set up on Windows already has the game copy, so it needs no download:

1. Copy the whole installation folder into a prefix's C: drive, for example to
   `~/.local/share/hellforge-prefix/pfx/drive_c/HellForge`. The prefix directory is created by Proton.
2. Run that installation's `app/linux/allow-namespace.sh` once, if the system needs it.
3. Start its native launcher. If the native launcher will not run there either, use the wrapper command above.

Keep an installation inside the prefix's C: drive (`pfx/drive_c`). Wine shows your Linux folders as its Z: drive, and
HellForge refuses that drive as a linked path (`Linked/reparse paths are not supported: Z:\`).

## Updating

Close HellForge and any session first. Then run a newer `HellForge-Linux-Installer` the same way as in
[the graphical installer](#graphical-installer), with the same prefix. The installer refuses while a process from that
installation is running, and names the processes. It does not compare versions, so pick the package you mean to install.

The installer:

1. Finds the existing launcher at `app/launcher-linux/HellForge.Launcher` and checks the package.
2. Unpacks it into `<installation>/temp/app-staging`.
3. Moves the old `app` to `temp/app-previous`.
4. Puts the new `app` and the top-level `HellForge.exe` in place, and installs the new engine package.
5. Starts the new launcher, then deletes the old app.

If replacing the app or installing the engine fails, or the new launcher cannot start, the previous app and engine are
put back. Because the whole `app` folder is replaced, files a release no longer ships do not stay behind. On an existing
installation the installer does not run `wineboot` again; the Proton you choose is saved for setup and sessions.

A release that pins another HELLDIVERS 2 build normally reopens setup. That can need another large download or a build
patch (see [the setup wizard](#setup-wizard)). Settings > Installation > Check and repair also installs the engine
package again, verifies the game copy and repeats ownership and the namespace check. Back up the prefix before a big
update if you want a way back ([prefix backup and restore](linux-backup-and-removal.md#prefix-backup-and-restore)).

## Sessions

Press PLAY. The native launcher starts `app/linux/hellforge-linux.sh` with the existing
`app/session/HellForge.Session.exe play ...` command. The launcher itself stays outside the namespace and reads and
writes only HellForge's local files. Do not start a session runner directly through Proton or run it without the wrapper.

Before entering the namespace, the wrapper:

- checks the programs in [What you need](#what-you-need), and finds Steam, the prefix and Proton;
- refuses with the exact command when AppArmor restricts user namespaces;
- fetches the live galactic war (`HellForge.LocalBackend.exe fetch-live-war`, with a 60-second timeout);
- fetches the pinned mod tools once per pin: the native Linux Zig, and the Windows Zig for an installation shared with
  Windows;
- compiles the enabled source mods with the native Linux Zig into `temp/mod-builds`. The session finds those builds
  ready, so it never runs `zig.exe` under Wine;
- starts a shared [bridge to Steam](linux-isolation.md#the-steam-bridge) when none is running.

A failure of the war fetch, the mod tools or a build is logged, and the session continues without that data or those
tools. A source mod it could not build stays off with the normal mod problem.

The wrapper then runs `hellforge-offline.sh`, which makes the namespace without root. It waits up to 20 seconds for a
wineserver of the prefix still running outside it, and refuses to start while one runs. Inside, `hf-inner.sh` opens the
bridge ports and runs the requested program through Proton. The session runner's own network probe runs before the game
starts. What else the wrapper sets up (the locale setting, the time limit, the shader caches) is in
[inside the wrapper](linux-isolation.md#inside-the-wrapper).

### Steam inside another sandbox

The bridge reads the `steam` process's open sockets in `/proc/<pid>/fd`. That only works from Steam's own user
namespace or the one above it. From inside another sandbox, such as NixOS's `steam-run` (measured: same PID namespace,
its own user namespace), Steam is visible but its sockets are not, and the bridge says so. There, start
`app/linux/hf-bridge-host.sh` where Steam runs, for example as a Steam shortcut, and leave it running. The wrapper uses
a bridge that answers.

The launcher's leftover-process check before PLAY, and its End button, leave the bridge and the programs that only
start it (Steam's `reaper`) alone. Updating HellForge and backing up or restoring the prefix still ask you to close it
first, as they do for every process of the installation.

### Settings the wrapper reads

`hellforge-linux.sh` reads these settings from the environment. Each one has a default it finds by itself:

| Variable | Default |
|---|---|
| `PROTON` | the proton script saved in the installation, else the last of every Steam library's `steamapps/common/Proton*`, the Steam folder's `compatibilitytools.d`, each folder in `STEAM_EXTRA_COMPAT_TOOLS_PATHS`, and `/usr/share/steam/compatibilitytools.d` and `/usr/local/share/steam/compatibilitytools.d` (version sort, which puts Proton - Experimental after the numbered versions) |
| `INSTALL` | the installation the script is in |
| `STEAM_COMPAT_CLIENT_INSTALL_PATH` | the folder saved in the installation (`steam-selection.json`), else the first of `~/.local/share/Steam`, `~/.steam/debian-installation`, `~/.steam/steam`, `~/.steam/root` and Flatpak's and Snap's Steam folders that has a `steamapps` folder |
| `STEAM_COMPAT_DATA_PATH` | the prefix the installation is in (the part of its path before `/pfx/drive_c/`); for an installation outside a prefix, HELLDIVERS 2's prefix, `steamapps/compatdata/553850` |
| `STEAM_COMPAT_APP_ID` | `553850` (HELLDIVERS 2), unless explicitly set otherwise |
| `HELLFORGE_STEAM_RUNTIME` | unset: Proton runs inside the Steam Linux Runtime its manifest requires, when installed (see [Proton in the Steam Linux Runtime](#proton-in-the-steam-linux-runtime)). `off` runs Proton on the host instead, for diagnosis only; it changes neither the sandbox nor the offline namespace |

### Other launchers and HELLDIVERS 2's prefix

Another launcher (a mod manager, [external-launchers.md](external-launchers.md)) starts sessions through the same
script. It passes the Windows program to run in place of HellForge.exe, for example
`hellforge-linux.sh "$INSTALL/app/cli/HellForge.Cli.exe" play --wait yes`, and keeps the Steam bridge, the war fetch and
the namespace.

Sharing HELLDIVERS 2's prefix (553850) works too, but then nothing else may run in that prefix while HellForge runs. A
Windows program of the prefix running outside would share its network with HellForge's
([one prefix, one network](linux-isolation.md#one-prefix-one-network)). The real game, started while HellForge runs,
would have no network.

### Proton in the Steam Linux Runtime

HellForge starts Proton inside the Steam Linux Runtime that the Proton's manifest names, as Steam does. Every session
prints one line saying which it chose:

```text
HellForge: Steam Linux Runtime: container "steamrt4" 4.0.20261021.265997 at <folder>; Proton runs inside it.
HellForge: Steam Linux Runtime: host mode: <reason>; Proton runs on the host.
```

Host mode is used, and the reason always printed, when:

- the Proton names no runtime;
- the runtime is not installed, or its manifest cannot be reproduced;
- `HELLFORGE_STEAM_RUNTIME=off` is set.

A runtime that is present but fails to start is not retried on the host. How HellForge finds the runtime, and what runs
inside it, is in [the Steam Linux Runtime](linux-isolation.md#the-steam-linux-runtime).

### Started from inside a container

If HellForge itself was started inside one of these, `hellforge-linux.sh` refuses with the exact action:

| Started inside | Detected by | Action |
|---|---|---|
| Flatpak | `/.flatpak-info` | Start HellForge from its own shortcut, or `app/launcher-linux/HellForge.Launcher`, on the host. HellForge already finds Flatpak's and Snap's Steam folders from outside; it does not need to run inside Steam's sandbox. |
| A Snap | `SNAP_NAME` | The same as for Flatpak. |
| Steam's runtime container | `container=pressure-vessel`, or `/run/host/container-manager` naming it | The Steam entry that started HellForge has a compatibility tool. Open its Properties > Compatibility and untick "Force the use of a specific Steam Play compatibility tool". HellForge's launcher is a native program and runs on the host. |

The wrapper asks this before it finds Steam or Proton and before any namespace step, in every mode: a session,
`--sandbox-probe` and `--sandbox-self-test`. The native launcher shows the same reason. The reason is also recorded:

- `hellforge-linux.sh` writes it to `logs/linux-container-refusal.txt`;
- setup's "Check the Linux sandbox" row records it as the reason the sandbox stays off (Settings > Sandbox shows it);
- `--sandbox-probe` writes it as `firstReason` in `logs/sandbox-probe.json`.

Other containers (`container=podman`, `docker`, `oci`, toolbox and distrobox) are not refused, because they can have
the user namespaces a session needs. If a later namespace step fails, the message names the container. Why HellForge must
start on the host is in
[How HellForge isolates the game on Linux](linux-isolation.md#why-hellforge-starts-on-the-host).

## Account sync

The profile page's **Sync Inventory** button opens the normal game through Steam. It copies the signed-in account's
inventory and progression into a sealed snapshot. It is the same capture as on Windows, but on Linux the proxy is a
native program, and the two system changes are made by one root-owned helper. What a sync changes, and how HellForge
checks and undoes it, is in [Account sync on Linux](linux-account-sync.md). Treat Account Sync on Linux as experimental
until it is recorded that the game trusts the CA's root store during a real sync.

HellForge finds the game's Proton in Steam's settings or, when they name none, in the prefix's `config_info`. Starting
HELLDIVERS 2 once through Steam writes `config_info`.

### Before a sync starts

The launcher asks HellForge's Steam probe for the signed-in account.

- "Sign in to Steam first" means the probe's report says no account is signed in.
- A probe that failed says why: its own `failure`, the wrapper's reason from `logs/linux-session-wrapper.log`, or that
  it ended without a report.
- A sync is refused while a HellForge session runs that this launcher did not start (an earlier launcher's, or one left
  behind). The play page's End button ends it.

Every refusal comes before the sync's own `logs/sync-<utc>/` folder exists, so the last one is kept in
`logs/sync-refused.txt` with the time and the HellForge build. The Windows launcher under Wine cannot sync on Linux,
because nothing gives it the account. It says to start HellForge from its desktop shortcut.

### Installing the helper

The first sync asks once to install the helper. Choosing **Install** runs the shipped `app/linux/allow-capture.sh`
through `pkexec /usr/bin/bash ...`, so the desktop's own polkit dialog asks for the password. The confirmation says
what it installs. After the installer exits, the launcher continues the same sync without another click.

- A cancelled or denied password dialog means "not installed; nothing changed".
- If `pkexec` is missing or no polkit authentication agent is running (Game Mode on a Deck, a bare window manager), the
  launcher shows the terminal command instead.
- SteamOS 3.x users without a password are told to run `passwd` first.
- A package update that changes either shipped file makes the next sync offer the same install again.
- `allow-capture.sh --remove` removes the rule, the helper, the relay and both hashes. Uninstall offers the same
  `--remove` through pkexec.

### During and after a sync

The capture usually ends while the game is still open. The launcher waits for HELLDIVERS 2 to close, then removes the
one-day CA from its prefix. Until the game closes, the capture still forwards it:

- the sync card says to close the game to finish;
- PLAY waits with the same message;
- the leftover-process check and its End button leave this launcher's own capture alone. A capture another launcher left
  is still listed. Leftovers are named in full, not as the kernel's 15-character command name (`HellForge.Captu`).

If the launcher is closed first, its next start removes the CA. During the sync, the game may still show "Could not
connect to Easy Anti Cheat network" after the login and close itself. The capture is finished by then, as on Windows.

### Cleaning up after a killed sync

If a sync was killed before it could clean up, start HellForge and press **Clean up**, or run:

```fish
sudo /usr/local/lib/hellforge/capture-helper cleanup
```

The launcher checks `/etc/hosts` for the marker on every start and, when the game is not running, runs that cleanup at
startup. If it fails, the card offers **Clean up**. The CA record is under `logs/sync-*/capture-ca.json`. **Clean up**
removes the recorded registry key through Wine's `reg.exe` too.

## Co-op on Linux: opening UDP

**Experimental.** The game starts and runs with this, and the network check passes. The restricted version joined
Windows players' games and played a full mission. Two testers reported this on issue 22, one of them as the fourth
player in a lobby of Windows players. Please report what you see, good or bad, with `backend/multiplayer.log` from the
session.

Co-op needs UDP to Valve's relays, which HellForge's network namespace does not carry by default. The native launcher's
Settings > Multiplayer > **Co-op over Steam relays (UDP to Valve's relays only)** opens it. It is on by default since the
0.4.0 beta.

- It is saved as `coopRelays` in `launcher-settings.json`. The `steamUdp` key that launchers before wrote, `false` for
  everyone who never chose, is not read, so every installation starts with it on once.
- The launcher and `HellForge.Cli play` set `HELLFORGE_STEAM_UDP=1` for that session's wrapper only, not for the Steam
  ownership check.
- Running the wrapper directly, `HELLFORGE_STEAM_UDP=1 bash linux/hellforge-linux.sh` does the same. Without it the
  wrapper starts without co-op.
- It needs `pasta` (package `passt`) and `python3`. Without `pasta`, `python3` or a relay list, or when pasta does not
  bring the network up, the session starts without co-op. The launcher and the wrapper say what to install, with the
  package-manager commands in [What you need](#what-you-need).

Co-op through Steam's relays should still work. A direct peer-to-peer path between two players is not allowed, and
Steam falls back to a relay.

With the switch off, lobbies are still found, because the lobby directory goes through the Steam client. But no join can
reach a host:

- the multiplayer log says so first;
- the lobby browser lists every lobby with the fix "Co-op relays off";
- Quick Play says why it joins nothing;
- this machine's Public lobby is not listed;
- a join the game starts itself (an invitation, a friend's game) gets a notice.

The in-game lobby browser asks before a join, and **yes turns it on in the running session**. If it cannot open (no
pasta, no relay list, a session started without HellForge's launcher), the browser says why. The switch is saved, so
STOP, then PLAY, and the next session starts with it. How the relays are opened, and what stays refused, is in
[co-op over Steam relays](linux-isolation.md#co-op-over-steam-relays).

## Settings

### Check and repair

Settings > Installation > Check and repair reruns the native setup:

1. Runs the informational [sandbox probe](linux-testing.md#sandbox-probe).
2. Asks for the Proton version.
3. Installs the engine package again.
4. Confirms Steam ownership.
5. Verifies the game copy, copying missing or damaged files from Steam, a complete depot download or a build patch.
6. Checks the namespace marker, installing it with the self-test when it is missing.
7. Renews the desktop shortcut and the bundled mods.

Use it after changing Proton, when the game copy is damaged, or after replacing `app` by hand.

### Sandbox

Settings > Sandbox is on when this PC passes the sandbox self-test for this HellForge build and kernel. An update of
either reruns it. A failure leaves the switch off and shows the reason. With the switch on, a session runs the copied
game in its own domain ([the Linux sandbox](linux-sandbox.md)). Check the sandbox on this PC with:

```fish
bash "<installation>/app/linux/hellforge-linux.sh" --sandbox-self-test
```

Every check prints `PASS` or `FAIL`. The exit code is 0 only when all of them pass. The self-test's own output, with its
Steam Linux Runtime mode line, is kept in `logs/sandbox-self-test.txt`. The launcher keeps the readiness command's output
in `logs/sandbox-readiness.txt`. What the self-test checks is in [the self-test](linux-sandbox.md#the-self-test).

### Backup, restore and uninstall

Settings > Installation > Prefix backup, **Restore prefix...** and Uninstall HellForge... are in
[Backing up and removing HellForge on Linux](linux-backup-and-removal.md).

## Building mods with native Zig

On Linux, a source mod is compiled by the pinned native Linux Zig (Zig 0.17.0, `app/sdk/tools.json`), never the pinned
Windows `zig.exe` under Wine (issue 81). The wrapper installs it into `<installation>/tools/zig-linux/zig` before the
session. It then runs the native build agent, which calls the same `ModBuilds` code a session does:

```fish
app/cli-linux/HellForge.Cli build-source-mods --install <installation>
```

That builds every enabled source mod into `temp/mod-builds`, with the cache key and `record.json` a session checks. The
session then finds the DLLs ready and does not compile.

- For a session after a mod sync, the wrapper adds `--sync yes --sandbox yes|no`. That builds the host's mods that
  session plays too; a host's source mod would otherwise stay off and the host refuse the join again.
- The key names the compiler (`zig 0.17.0 / x86_64-windows-gnu`) rather than its archive. So a Windows session and a
  native Linux build of the same sources share it.
- A mod that does not compile is recorded with its compiler message and stays off for that session, as before.
- A synced source mod that is first downloaded from the host during a session cannot be prebuilt by the wrapper. It
  stays off for that session with the normal mod problem. The next start can build it when the synced copy is still in
  `mods/.synced`.

**Build now** in a mod's ⋮ menu runs the same native agent for that mod on Linux
(`app/cli-linux/HellForge.Cli build-mod --mod <mod folder>`), so it fills the build the next session loads. It is
enabled when the pinned native Zig is installed. `$ZIG`, `~/.local/share/zig-native/zig` and `zig` on `PATH` are
fallbacks for a compiler installed elsewhere.

`app/linux/hf-build-mod.sh` remains the authoring helper for mods outside an installation:

```fish
bash "$INSTALL/app/linux/hf-build-mod.sh" <mod-folder>
```

- It reads `CMakeLists.txt` for the first `hellforge_add_mod(...)` call, for the language and the source list.
- Without one, it uses the `dll` name in `hellforge-mod.json` and `src/*.cpp` or `src/*.c`.
- It compiles an `x86_64-windows-gnu` DLL with the SDK headers from `app/sdk`, the same flags as a session
  (`-O2 -Wall -Werror`, C++17 or C11) and a cache under the installation.
- It copies the manifest, the DLL and `data/` into `<installation>/mods/<mod-folder-name>/`. For a mod already in
  `mods/`, it delegates to the native agent instead.
- It uses `<installation>/tools/zig-linux/zig` first, then `$ZIG`, `~/.local/share/zig-native/zig` and `zig` on `PATH`.

## Problems players have reported

These come from the testers in [issue 1](https://github.com/SkyeShade/HellForge/issues/1). None of them is fixed yet.

- The game closes when you pick a mission on the ship's map. DreamyBranch found it happens after hovering over the
  mission's card, and that clicking a mission quickly, without hovering, gets you into the Hellpod. For other players
  it closes as soon as the ship starts to move. One player saw the unmodified game close a few minutes into missions
  under Proton too. The cause is not known.
- HellForge stops at start with a connection error although Steam is running and signed in. Two testers saw this after
  a Vulkan overlay such as MangoHud, or a Proton build that changes the overlays, had changed the prefix. Starting
  HellForge once with another Proton (`PROTON=...`), then with the usual one again, cleared it.
- Co-op between a Linux player and Windows players did not connect in joshua-software-dev's tests.

## Helping with Linux

What is still untested, the sandbox probe and how to report a result are in
[Testing HellForge on Linux](linux-testing.md).

If a session refuses to start, see [troubleshooting](troubleshooting.md). For every other page, see the
[docs map](README.md).
