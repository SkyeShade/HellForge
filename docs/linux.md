# HellForge on Linux (Proton)

For players on Linux: how to set up and start HellForge under Proton, how it keeps the game off the internet there, and
what is still untested. Linux support is experimental.

Linux support is the work of DreamyBranch and joshua-software-dev, who worked it out and shared it in
[issue 1](https://github.com/SkyeShade/HellForge/issues/1). Thank you both. The setup steps below, and the dependency
check, Steam folder search and depot move in `hellforge-linux.sh`, follow DreamyBranch's guided install script in
[issue 11](https://github.com/SkyeShade/HellForge/issues/11). The scripts are in `linux/` (in an installation:
`app/linux/`).

## What you need

- Steam for Linux, running and signed in to an account that owns HELLDIVERS 2, with at least one Proton installed.
  HELLDIVERS 2 itself does not need to be installed: setup on Linux gets the supported build through Steam's console.
- About 50 GB of free space while setting up: Steam's download of the supported build and HellForge's copy of the game
  are about 24 GB each. Setup deletes the download after copying unless you untick that.
- `HellForge-Setup.exe`.
- These programs: `socat`, `ip` and `ss` (from iproute2), `unshare` (from util-linux) and `pgrep` (from procps).
  `hellforge-linux.sh` checks for them and prints the install command for your package manager:

  | Package manager | Common distributions | Command |
  |---|---|---|
  | `pacman` | Arch, CachyOS, Manjaro | `sudo pacman -S --needed iproute2 socat util-linux procps-ng` |
  | `apt-get` | Ubuntu, Debian, Mint, Pop!_OS | `sudo apt install iproute2 socat util-linux procps` |
  | `dnf` | Fedora, Nobara | `sudo dnf install iproute socat util-linux procps-ng` |
  | `zypper` | openSUSE | `sudo zypper install iproute2 socat util-linux procps` |

  On other systems, install the same packages with your package manager.

## Setting it up

The commands use `~/.local/share/hellforge-prefix` as HellForge's own Wine prefix and `~/.local/share/Steam` as the
Steam folder. Any empty folder works as the prefix. If your Steam is elsewhere (`~/.steam/debian-installation` on some
Debian and Ubuntu installs), use that folder.

1. Install the programs from [What you need](#what-you-need).
2. Run `HellForge-Setup.exe` with Proton in a new prefix. Put the name of a Proton folder from
   `~/.local/share/Steam/steamapps/common/` (or `compatibilitytools.d/`) in place of `<Proton>`:

   ```sh
   mkdir -p ~/.local/share/hellforge-prefix
   STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam" \
   STEAM_COMPAT_DATA_PATH="$HOME/.local/share/hellforge-prefix" \
   "$HOME/.local/share/Steam/steamapps/common/<Proton>/proton" run ~/Downloads/HellForge-Setup.exe
   ```

   Click Install and keep the folder it suggests, normally `Documents\HellForge`. On Linux that is
   `~/.local/share/hellforge-prefix/pfx/drive_c/users/steamuser/Documents/HellForge`. Keep the installation inside the
   prefix's C: drive (`pfx/drive_c`). Wine shows your Linux folders as its Z: drive, and HellForge refuses that drive as
   a linked path ("Linked/reparse paths are not supported: Z:\").

   Setup then opens HellForge. Close it: started this way, outside its network namespace, its setup stops with "On
   Linux, setup runs inside HellForge's network namespace".
3. On Ubuntu 24.04 and later, and other systems where `/proc/sys/kernel/apparmor_restrict_unprivileged_userns` is 1,
   only allowed programs may make the namespace. Run this once, from your own account:

   ```sh
   sudo bash ~/.local/share/hellforge-prefix/pfx/drive_c/users/steamuser/Documents/HellForge/app/linux/allow-namespace.sh
   ```

   It installs a copy of `unshare` that only you (and root) can run, and an AppArmor profile that lets that copy make
   namespaces, as Ubuntu does for Steam. For you this lifts Ubuntu's restriction, for any namespace, not only
   HellForge's; other accounts keep it. `--remove` undoes it. Run it again after a util-linux update.
   `hellforge-linux.sh` tells you when it is needed.
4. Start Steam and sign in. Then start HellForge with `hellforge-linux.sh`:

   ```sh
   bash ~/.local/share/hellforge-prefix/pfx/drive_c/users/steamuser/Documents/HellForge/app/linux/hellforge-linux.sh
   ```

   It finds the prefix from where the installation is, finds Steam, and picks a Proton. When there are several, it
   prints which one it uses and the others. To choose one, put `PROTON=<path to its proton script>` before the command.
   Started with `bash`, the scripts do not need to be made executable.
5. In HellForge, press Start setup. Setup cannot read Steam's own HELLDIVERS 2 from inside Wine, so it shows the card
   for downloading the supported build through Steam, with three `download_depot` commands
   ([troubleshooting](troubleshooting.md#setup-cannot-find-the-supported-build) describes it). Open Steam's console with
   `xdg-open steam://open/console` in a terminal (or start Steam with `steam -console`), paste each command and press
   Enter, one at a time. The wizard's progress bar stays empty on Linux. Wait until the console says "Depot download
   complete" for all three.
6. Close HellForge and start it again with the command from step 4, in a terminal. It finds the download in Steam's
   folder and asks to move it into the prefix, where setup reads it. Answer `y`. Then press Start setup again: setup
   copies the game from the download, verifies every file, and places the network block, which on Linux is the
   namespace.

From then on, start HellForge with the command from step 4. Started any other way, it runs outside the network namespace
and refuses to start a session.

`hellforge-linux.sh` reads these settings from the environment. Each one has a default it finds by itself:

| Variable | Default |
|---|---|
| `PROTON` | a Proton the Steam client has (`steamapps/common/Proton*` or `compatibilitytools.d/*`, the last by version sort, which puts Proton - Experimental after the numbered versions) |
| `INSTALL` | the installation the script is in |
| `STEAM_COMPAT_CLIENT_INSTALL_PATH` | the first of `~/.local/share/Steam`, `~/.steam/debian-installation`, `~/.steam/steam` and Flatpak's and Snap's Steam folders that has a `steamapps` folder |
| `STEAM_COMPAT_DATA_PATH` | the prefix the installation is in (the part of its path before `/pfx/drive_c/`); for an installation outside a prefix, HELLDIVERS 2's prefix, `steamapps/compatdata/553850` |

It starts the bridge to Steam itself and stops it when HellForge closes. A bridge already running (`hf-bridge-host.sh`)
is used as it is.

Another launcher (a mod manager, [external-launchers.md](external-launchers.md)) starts sessions through the same
script. It passes the Windows program to run in place of HellForge.exe, for example
`hellforge-linux.sh "$INSTALL/app/cli/HellForge.Cli.exe" play --wait yes`, and keeps the Steam bridge, the war fetch and
the namespace.

### From an installation made on Windows

An installation set up on Windows already has the game copy, so it needs no download. Copy the whole installation folder
into a prefix's C: drive, for example to `~/.local/share/hellforge-prefix/pfx/drive_c/HellForge` (a prefix exists once
Proton has run a program in it, such as `HellForge-Setup.exe` in step 2). Then follow steps 3 and 4 with that
installation's `app/linux` folder.

Sharing HELLDIVERS 2's prefix (553850) works too, but then nothing else may run in that prefix while HellForge runs. A
Windows program of the prefix running outside would share its network with HellForge's
([one prefix, one network](#one-prefix-one-network)), and the real game, started while HellForge runs, would have no
network.

### Steam's download of the supported build

The Linux Steam client puts what `download_depot` downloads in `<Steam>/steamapps/content/app_553850/`, or in the same
path under `<Steam>/ubuntu12_32/`, `<Steam>/ubuntu12_64/` or `<Steam>/steamrt64/`, because it downloads relative to the
folder its program runs from. Setup runs under Wine and cannot read any of these (they are on the Z: drive). It reads
the download from Steam's folder inside the prefix:

```text
<prefix>/pfx/drive_c/Program Files (x86)/Steam/steamapps/content/app_553850/
```

Until the game is copied, `hellforge-linux.sh` looks for the download in all four places and offers to move it there.
Moving takes a moment and no extra space when the prefix and Steam are on the same filesystem; on another filesystem it
copies about 24 GB. It leaves a download alone while its size still changes, since Steam is then still writing it. It
also sets the prefix's Steam folder in the registry (`SteamPath`) to `C:\Program Files (x86)\Steam`, where setup looks
for Steam.

To move it yourself, move the three `depot_*` folders from `app_553850` in Steam's folder to that `app_553850` folder in
the prefix. Links do not work: HellForge refuses linked paths.

## Updating

Run the newer `HellForge-Setup.exe` with the command from step 2, with the same prefix. It finds the installation, its
button says Update, and it keeps the game copy, profiles and settings. Close the HellForge it opens, then start
HellForge with `hellforge-linux.sh` as before.

## Removing HellForge

HellForge's Uninstall button does not work on Linux; remove it by hand:

1. If you ran `allow-namespace.sh`, undo it first, while the script is still there:
   `sudo bash <installation>/app/linux/allow-namespace.sh --remove`.
2. Delete the prefix, if it is HellForge's own: `rm -rf ~/.local/share/hellforge-prefix`. That deletes the
   installation with the game copy, your HellForge profiles and progress, and every mod in its mods folder, your own
   too. Copy anything you want to keep first. In a shared prefix, delete only the installation folder.
3. If Steam's download of the supported build is still in Steam's folder (it was never moved), delete its `app_553850`
   folder there.

## How it keeps the game off the internet

On Windows the copied game is kept off the internet by a network block in the Windows Filtering Platform. Wine has
none, so on Linux HellForge runs inside a network namespace: a private network with no route out.
`linux/hellforge-offline.sh` makes it without root, with:

- loopback, for the local backend and the bridge to Steam;
- a virtual network adapter (`veth`), so Wine and the game see an ordinary network;
- "prohibit" default routes for IPv4 and IPv6, so every outside connection is refused at once. That is the same refusal
  the Windows block gives, which the session's network probe checks before every session;
- a self-test that refuses to start if a connection to 1.1.1.1 gets through.

The launcher, the session runner, the local backend and the game all run inside it. Under Wine, the launcher checks the
isolation before each session (`src/HellForge.Core/LinuxNetworkBlock.cs`), and both it and the session runner refuse to
start a session outside the namespace, an installation copied from Windows included.

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

The game itself, Proton and the Steam client were tested by the testers in issue 1. Running `HellForge-Setup.exe` under
Proton in a prefix of HellForge's own, moving the depot download into it and the launcher's setup there were tested with
DreamyBranch's guided script (on CachyOS).

## Not known yet

- The depot move and the other steps `hellforge-linux.sh` took over from the guided script have not been tried on Linux
  in HellForge's own scripts yet.
- Co-op on Linux. Friends' games are reached through Steam's networking, which runs in the Steam client outside the
  namespace, but the one test so far did not connect (see [above](#problems-players-have-reported)).
- Steam as a Flatpak or Snap (its own `/tmp` and paths), and the Steam Deck (no `socat` by default), are untested.
- The Steam ownership check under Proton. Setup and every session run it, as on Windows: under Wine, HellForge starts
  its Steam probe (`app\steamprobe\HellForge.SteamProbe.exe`) directly, because Wine's `powershell.exe` runs nothing.
  The probe must confirm the game on the signed-in Steam account, or setup and the session stop. It has not yet been
  run on a Linux machine, so if setup or a session stops at "The ownership check did not pass", please report it with
  the logs.

## Helping with Linux

Linux support grows from what Linux players try. If you get further than this page, find a fix, or test on a
distribution or Steam setup listed above as untested, open an issue with your logs or send a pull request: the scripts
are in [`linux/`](../linux) and the Wine checks in `src/HellForge.Core/LinuxNetworkBlock.cs`. See
[CONTRIBUTING.md](../CONTRIBUTING.md) for how to build and test.

If a session refuses to start, see [troubleshooting](troubleshooting.md). For every other page, see the
[docs map](README.md).
