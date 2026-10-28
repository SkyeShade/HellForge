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
- These programs: `socat`, `ip` and `ss` (from iproute2), `unshare` and `flock` (from util-linux), `pgrep` (from
  procps) and `bsdtar`. `bsdtar` is needed only to add `.7z` or `.rar` mods.
  `hellforge-linux.sh` checks for them and prints the install command for your package manager:

  | Package manager | Common distributions | Command |
  |---|---|---|
  | `pacman` | Arch, CachyOS, Manjaro | `sudo pacman -S --needed iproute2 socat util-linux procps-ng libarchive` |
  | `apt-get` | Ubuntu, Debian, Mint, Pop!_OS | `sudo apt install iproute2 socat util-linux procps libarchive-tools` |
  | `dnf` | Fedora, Nobara | `sudo dnf install iproute socat util-linux procps-ng bsdtar` |
  | `zypper` | openSUSE | `sudo zypper install iproute2 socat util-linux procps bsdtar` |

  On other systems, install the same packages with your package manager.

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

The installer reads Steam's folders and lists the Proton versions installed there. Choose one and a prefix folder; the
default is `~/.local/share/hellforge-prefix`. When the prefix has no HellForge launcher yet, it runs
`proton run wineboot -u` (three minutes at most) and, only when the folder did not exist or was empty, writes a
`.hellforge-created` marker that uninstall later uses for its prefix offer. It extracts the app into
`<prefix>/pfx/drive_c/users/steamuser/Documents/HellForge`, installs the engine package from
`app/engine-package.zip`, saves the Proton choice and starts the native launcher. The setup wizard starts automatically
while the game copy is incomplete. The installer checks the package's required entries before it prepares the prefix,
and refuses a package that is missing `app/app-build.json`, `app/engine-package.zip`, the native launcher or
`app/linux/hellforge-linux.sh`.

On Ubuntu 24.04 and later, and other systems where `/proc/sys/kernel/apparmor_restrict_unprivileged_userns` is 1, only
allowed programs may make the namespace. Run this once, from your own account:

```fish
sudo bash ~/.local/share/hellforge-prefix/pfx/drive_c/users/steamuser/Documents/HellForge/app/linux/allow-namespace.sh
```

It installs a copy of `unshare` that only root and members of your account's primary group can run, and an AppArmor
profile that lets that copy make namespaces, as Ubuntu does for Steam. For you this lifts Ubuntu's restriction, for any
namespace, not only HellForge's; accounts outside that group keep it. `--remove` undoes it. Run it again after a
util-linux update. `hellforge-linux.sh` tells you when it is needed.

### Setup wizard

The wizard runs natively. At its start, choose the Proton version; the installer's choice is already selected. Setup
saves the choice in `proton-selection.json` and uses it for setup and later game sessions. Settings > Installation >
Check and repair can change it.

Setup first checks Steam, the chosen Proton and its own files, and installs the engine package from
`app/engine-package.zip`. It then looks for the game in this order:

1. the installation's own copy, if every file matches the supported build;
2. Steam's installed copy, if it is the supported build;
3. Steam's completed `download_depot` download;
4. a published build patch for the build Steam has installed;
5. otherwise, Steam's download of the supported build (see below).

A build patch is about 1.7 GB. Setup applies it to a copy of Steam's installed game before the ownership check, because
the check needs the supported build's `steam_api64.dll`. The patch tool is a Windows program, so the selected Proton
runs `hpatchz.exe` with `waitforexitandrun`; Steam's installation is only read. Every patched file is checked against
the supported build's hash. If the patch fails or the drive is full, setup keeps the files it already patched, remembers
the failed patch for this launcher run and offers Steam's download instead; starting the launcher again tries the patch
again. A complete depot download is used before a patch, as on Windows.

Setup then confirms ownership through the selected Proton. Steam must be running and signed in to an account that owns
HELLDIVERS 2 (or has it through Steam Family Sharing). The launcher starts its Steam probe through
`app/linux/hellforge-linux.sh`, so the check runs inside the same namespace as a session. The probe writes
`logs/setup-ownership.json`, and that report decides. A failure stops setup with `The ownership check did not pass: ...`
followed by the probe's reason or exit code, `The wrapper log says: "..."` when the wrapper stopped before writing the
report (missing programs, AppArmor, no Steam bridge, and so on), and `Make sure Steam is running and signed in to an
account that owns HELLDIVERS 2.`

After the ownership check, setup prepares the optional mod tools through the wrapper: HellForge's pinned Zig, about
90 MB from ziglang.org. A failure is reported in the row but does not stop setup. Setup then copies
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
(`HellForge.LocalBackend.exe fetch-live-war`, with a 60-second timeout) and, once per Zig pin, the pinned mod tools
(about 90 MB). A failure is logged and the session continues without that data or those tools.

The wrapper starts a shared bridge to Steam when none is running. The bridge exposes Steam's loopback ports as
`hf-steam-*.sock` files in `${XDG_RUNTIME_DIR:-/run/user/<uid>}`, and `hf-inner.sh` recreates the same loopback ports
inside the namespace. Sessions that run at the same time share the bridge: each wrapper holds the users lock
(`hf-bridge-users.lock`) shared, and the last one to close takes it exclusively and stops a bridge the wrapper started.
A bridge started by hand with `hf-bridge-host.sh` has no PID file, is used as it is, and HellForge never stops it. The
wrapper serializes checking, starting and stopping the bridge with `flock` on `hf-bridge.lock` and `hf-bridge-users.lock`.

The wrapper then runs `hellforge-offline.sh`, which makes the namespace without root, waits up to 20 seconds for a
wineserver of the prefix still running outside it, and refuses to start while one runs. Inside, `hf-inner.sh` forwards
the bridge ports and runs the requested program through Proton. The session runner's own network probe runs before the
game starts. Do not start a session runner directly through Proton or run it without the wrapper.

`hellforge-linux.sh` reads these settings from the environment. Each one has a default it finds by itself:

| Variable | Default |
|---|---|
| `PROTON` | a Proton the Steam client has (`steamapps/common/Proton*` or `compatibilitytools.d/*`, the last by version sort, which puts Proton - Experimental after the numbered versions) |
| `INSTALL` | the installation the script is in |
| `STEAM_COMPAT_CLIENT_INSTALL_PATH` | the first of `~/.local/share/Steam`, `~/.steam/debian-installation`, `~/.steam/steam` and Flatpak's and Snap's Steam folders that has a `steamapps` folder |
| `STEAM_COMPAT_DATA_PATH` | the prefix the installation is in (the part of its path before `/pfx/drive_c/`); for an installation outside a prefix, HELLDIVERS 2's prefix, `steamapps/compatdata/553850` |
| `STEAM_COMPAT_APP_ID` | `553850` (HELLDIVERS 2), unless explicitly set otherwise |

Another launcher (a mod manager, [external-launchers.md](external-launchers.md)) starts sessions through the same
script. It passes the Windows program to run in place of HellForge.exe, for example
`hellforge-linux.sh "$INSTALL/app/cli/HellForge.Cli.exe" play --wait yes`, and keeps the Steam bridge, the war fetch and
the namespace.

Sharing HELLDIVERS 2's prefix (553850) works too, but then nothing else may run in that prefix while HellForge runs. A
Windows program of the prefix running outside would share its network with HellForge's
([one prefix, one network](#one-prefix-one-network)), and the real game, started while HellForge runs, would have no
network.

## Settings

### Check and repair

Settings > Installation > Check and repair reruns the native setup: choose the Proton version, install the engine
package again, confirm Steam ownership, verify the game copy (copying missing or damaged files from Steam, a complete
depot download or a build patch), and check the namespace marker (installing it, with the self-test, when it is
missing). It also renews the desktop shortcut and the bundled mods. Use it after changing Proton, when the game copy is
damaged, or after replacing `app` by hand.

### Prefix backup and restore

Settings > Installation > Prefix backup saves the whole Proton prefix, including the game copy, profiles, mods and
Steam's depot download, to one archive. The default folder is `~/hellforge-backups`; choose another with **Choose
folder**. The archive is named `hellforge-prefix-<date>-<time>.tar.zst` when `zstd` is on `PATH`, otherwise `.tar.gz`.
Both are ordinary tar streams that standard tools can read. The launcher writes to a `.part` file and moves it into
place when tar exits; cancel deletes the partial file.

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

Then the launcher closes and a helper deletes the installation folder (`game`, `profiles`, `mods`, `userdata`, `logs`,
`temp`, `app` and the namespace marker) and `~/Desktop/HellForge.desktop` when it still has HellForge's exact `Exec=`
line. It leaves Steam, HELLDIVERS 2, your Proton versions, anything that does not prove it belongs to this installation,
backups in `~/hellforge-backups`, Steam's own update and download state elsewhere under `steamapps`, and the bridge's
lock files in `${XDG_RUNTIME_DIR:-/run/user/<uid>}` (empty and harmless).

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

On Linux, a source mod's **Build now** compiles with a native Linux Zig, not the pinned Windows Zig sessions use.
HellForge looks for Zig at `$ZIG`, `~/.local/share/zig-native/zig`, then `zig` on `PATH`; without one, Build now is
disabled and its tooltip says how to install it. Build now runs `app/linux/hf-build-mod.sh <mod folder>`:

- it reads `CMakeLists.txt` for the first `hellforge_add_mod(...)` call, for the language and the source list;
- without one, it uses the `dll` name in `hellforge-mod.json` and `src/*.cpp` or `src/*.c`;
- it compiles an `x86_64-windows-gnu` DLL with the SDK headers from `app/sdk`, `-Wall -Werror`, C++17 or C11, and copies
  `hellforge-mod.json`, the DLL and `data/` into `<installation>/mods/<mod-folder-name>/`.

The script can be run by hand with `INSTALL` set to the installation and `ZIG` to the native zig:

```fish
bash "$INSTALL/app/linux/hf-build-mod.sh" <mod-folder>
```

It needs a native Linux Zig: `$ZIG`, else `~/.local/share/zig-native/zig`, else `zig` on `PATH`. Sessions still build
source mods with the pinned Windows Zig inside the namespace.

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

The game still needs the Steam client, which runs outside with its normal network. `linux/hf-bridge-host.sh` exposes
Steam's local ports as files (unix sockets, which cross into the namespace), and `linux/hf-inner.sh` turns them back into
the same local ports inside. The bridges use `nodelay`: without it the game's many small calls to Steam were held back,
and the frame rate dropped badly.

The namespace is a network block, not a sandbox. It holds back the game's own connections, but programs the game asks to
act for it outside still can: the Steam client through the bridge, the desktop session's services through their
sockets, and, on systems that resolve names through systemd-resolved, name lookups. The Windows block is the same kind of
block.

The galactic war is fetched just before the namespace is entered (`HellForge.LocalBackend.exe fetch-live-war`), and the
session serves that copy. With "Follow the live galactic war", the war is the one from launch time and moves on at the
next launch.

The mod tools are fetched there too, once (`HellForge.Cli.exe mod-tools`: HellForge's pinned Zig, about 90 MB from
ziglang.org), and again only when a release pins another Zig. Source mods are compiled when a session starts, inside the
namespace, where the download would be refused ("Access denied. (ziglang.org:443)"). The Mods page's "Install mod tools"
button cannot fetch them on Linux for the same reason; start HellForge again with `hellforge-linux.sh` instead.

### Co-op on Linux: opening UDP (optional)

**Experimental.** The game starts and runs with this, and the network check below passes. The restricted version below joined Windows players' games and played a full mission (reported on issue 22 by two testers, one of them as the fourth player in a lobby of Windows players). Please report what you see, good or bad, with `backend/multiplayer.log` from the session.

Steam's networking, which co-op uses, runs inside the backend's own process and reaches Valve's relays over UDP, so the
default namespace (no route at all) cannot carry it. Start with `HELLFORGE_STEAM_UDP=1 bash linux/hellforge-linux.sh` and
`hellforge-offline.sh` attaches `pasta` (package `passt`) to the same namespace from outside (running the game under `pasta -- command` gave it its own pid, mount, ipc and hostname namespaces and it quit during start), which gives it a real route. Two policy rules then
close it again: `ip rule add ipproto tcp prohibit` refuses every TCP connection with the same WSAEACCES, so the session's
network probe passes as before, and UDP is allowed only to Valve's relays, address and port range each, from the list Valve
publishes for the game (`ISteamApps/GetSDRConfig/v1`, app 553850), fetched at launch while this PC still has its network
(the last one is kept in `~/.cache/hellforge/sdr-relays.txt` for a launch without a network; with neither, it does not
start). Every other UDP destination, name lookups included, is refused, and the self-test refuses to start if one
works. It needs `python3` for the list. Co-op through Steam's relays should still work; a direct peer-to-peer path between
two players is not allowed and Steam falls back to a relay. Checked in the namespace: UDP to a relay address and port
goes out, the same address on another port, 1.1.1.1 and 8.8.8.8 are refused, TCP refused, loopback fine. A solo session's packet capture
(about 5 minutes) showed only backend UDP to Valve relays and nothing from the game process.

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
- Steam as a Flatpak or Snap (its own `/tmp` and paths), and the Steam Deck (no `socat` by default), are untested.
- The Steam ownership check under Proton. Setup and sessions run it, as on Windows (a session reuses a check that passed
  within 7 days for the same installation, account and build, where it can see Steam's signed-in account): under Wine, HellForge starts
  its Steam probe (`app\steamprobe\HellForge.SteamProbe.exe`) directly, because Wine's `powershell.exe` runs nothing.
  The probe must confirm the game on the signed-in Steam account, or setup and the session stop. It has not yet been
  run on a Linux machine, so if setup or a session stops at "The ownership check did not pass", please report it with
  the logs.
- The graphical installer's install and update path (the staged app swap, rollback and new engine install), build
  patches applied through Proton, prefix backup and restore, and uninstall's outside cleanup (including the AppArmor
  commands) and prefix deletion offer have not been tried with a real game and Steam on a Linux machine.

## Helping with Linux

Linux support grows from what Linux players try. If you get further than this page, find a fix, or test on a
distribution or Steam setup listed above as untested, open an issue with your logs or send a pull request: the scripts
are in [`linux/`](../linux) and the Wine checks in `src/HellForge.Core/LinuxNetworkBlock.cs`. See
[CONTRIBUTING.md](../CONTRIBUTING.md) for how to build and test.

If a session refuses to start, see [troubleshooting](troubleshooting.md). For every other page, see the
[docs map](README.md).
