# How HellForge isolates the game on Linux

<!-- diataxis: explanation -->

For contributors and curious players: how HellForge keeps the game off the internet on Linux, how the game still
reaches Steam and Valve's relays, and how the wrapper runs Proton. Installing and playing are in
[HellForge on Linux](linux.md).

## The network namespace

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
the namespace and its self-test. The session runner checks the isolation before starting the game
(`src/HellForge.Core/LinuxNetworkBlock.cs`), including for installations copied from Windows.

The namespace is a network block, not a sandbox. It holds back the game's own connections, but programs the game asks to
act for it outside still can:

- the Steam client, through the bridge;
- the desktop session's services, through their sockets;
- on systems that resolve names through systemd-resolved, name lookups.

The Windows block is the same kind of block. The filesystem sandbox is a separate layer
([The Linux sandbox](linux-sandbox.md)).

## The Steam bridge

The game still needs the Steam client, which runs outside with its normal network. HellForge bridges the two with its
own native program, so `socat` is not used:

- On the host, `linux/hf-bridge-host.sh` execs `app/cli-linux/HellForge.Cli steam-bridge host`. It finds the loopback
  ports that the process named `steam` listens on. It exposes each as a 0600 unix socket file, `hf-steam-<port>.sock`
  in `${XDG_RUNTIME_DIR:-/run/user/<uid>}`. Files cross into the namespace.
- Inside, `linux/hf-inner.sh` runs `steam-bridge inner`. It recreates the same loopback ports inside the namespace and
  relays each connection to its socket.
- `steam-bridge check` tests whether a bridge still answers.

Both sides use only 127.0.0.1 and the socket directory, and no route is added. The TCP relays use `nodelay`: without it
the game's many small calls to Steam were held back, and the frame rate dropped badly.

The wrapper starts the bridge when none is running, and sessions that run at the same time share it:

- Each wrapper holds the users lock (`hf-bridge-users.lock`) shared. The last one to close takes it exclusively and stops
  a bridge the wrapper started.
- The wrapper serializes checking, starting and stopping the bridge with `flock` on `hf-bridge.lock` and
  `hf-bridge-users.lock`.
- The wrapper records the host's PID in `hf-bridge.pid`. Before stopping it, it checks its command line: the
  hand-started `hf-bridge-host.sh`, or the native `app/cli-linux/HellForge.Cli steam-bridge host` it execs.
- A bridge started by hand with `hf-bridge-host.sh` has no PID file, is used as it is, and HellForge never stops it.

The host side reads the `steam` process's open sockets in `/proc/<pid>/fd`. That only works from Steam's own user
namespace or the one above it, so a Steam inside another sandbox needs the bridge started where Steam runs
([Steam inside another sandbox](linux.md#steam-inside-another-sandbox)).

## The live war and mod tools

The galactic war is fetched just before the namespace is entered (`HellForge.LocalBackend.exe fetch-live-war`), and the
session serves that copy. With "Follow the live galactic war", the war is the one from launch time and moves on at the
next launch.

The mod tools are fetched there too, once, and again only when a release pins another Zig:

- `HellForge.Cli.exe mod-tools` for the Windows Zig;
- the native `app/cli-linux/HellForge.Cli mod-tools` for `tools/zig-linux`.

The native agent then compiles the enabled source mods before the namespace, where a download would be refused ("Access
denied. (ziglang.org:443)"). The Mods page's **Install mod tools** installs the same native Zig directly; the wrapper
fetches it before a session too.

## One prefix, one network

Wine opens a Windows program's sockets in the prefix's `wineserver`, so a program gets the wineserver's network, not its
own. A wineserver of HellForge's prefix still running outside the namespace would give the programs started inside the
internet.

So `hellforge-offline.sh` waits up to 20 seconds for one to end (it does, a few seconds after its last program), and
refuses to start while one runs. Should one start in the moment between that check and the namespace, the session's
network probe (in the session runner, before the game starts) refuses the session. A prefix of HellForge's own avoids
all of this.

## Co-op over Steam relays

Steam's networking, which co-op uses, runs inside the backend's own process and reaches Valve's relays over UDP. So the
default namespace, with no route at all, cannot carry it. Turning it on is in
[co-op on Linux](linux.md#co-op-on-linux-opening-udp).

### Opening the route

With the switch on, `hellforge-offline.sh` attaches `pasta` (package `passt`) to the same namespace from outside, which
gives it a real route. Running the game under `pasta -- command` instead gave it its own pid, mount, ipc and hostname
namespaces, and it quit during start.

pasta runs with `--no-netns-quit`. By default pasta watches the namespace holder's `/proc/<pid>/ns` folder so it can
quit with it. Fedora's `passt-selinux` policy (Fedora, Bazzite, Nobara and other SELinux systems) denies that with "netns
dir open: Permission denied", leaving the namespace without a route. The wrapper ends pasta itself when the session
ends.

### What stays refused

Two policy rules then close it again:

- `ip rule add ipproto tcp prohibit` refuses every TCP connection with the same WSAEACCES, so the session's network
  probe passes as before.
- UDP is allowed only to Valve's relays, address and port range each. They come from the list Valve publishes for the
  game (`ISteamApps/GetSDRConfig/v1`, app 553850), fetched at launch while this PC still has its network. The last one
  is kept in `~/.cache/hellforge/sdr-relays.txt` for a launch without a network. Reading the list needs `python3`.

Every other UDP destination, name lookups included, is refused, and the self-test refuses to start if one works. A
direct peer-to-peer path between two players is not allowed, and Steam falls back to a relay.

Without `pasta`, `python3` or a relay list, or when pasta does not bring the network up, the session starts without
co-op. The wrapper sets `HELLFORGE_STEAM_UDP=0` and the reason in `HELLFORGE_STEAM_UDP_PROBLEM`, which the backend and
the browser repeat. The backend knows the state from `HELLFORGE_LINUX_NETNS=1` and `HELLFORGE_STEAM_UDP` in its
environment.

Checked in the namespace:

- UDP to a relay address and port goes out.
- The same address on another port, 1.1.1.1 and 8.8.8.8 are refused.
- TCP is refused, and loopback is fine.
- A solo session's packet capture (about 5 minutes) showed only backend UDP to Valve relays and nothing from the game
  process.

### Opening it in a running session

When the player says yes in the in-game lobby browser:

1. The backend writes `coop-relays-request.json` in its run folder (`<run>/backend`).
2. The launcher, outside the namespace, sees it, saves the switch on for later sessions, and runs
   `app/linux/hf-coop-relays.sh`.
3. `hf-coop-relays.sh` enters the session's network namespace as this PC's user (`nsenter -U -n
   --preserve-credentials`). It finds the namespace through a keeper process that `hellforge-offline.sh` leaves in the
   user namespace. The keeper owns the network namespace and writes its pid and start time to `temp/linux-netns-keeper`
   (`HELLFORGE_NETNS_KEEPER`).
4. It sets the relay rules first, removes the dead end (the veth adapter and the prohibit routes), attaches pasta, and
   runs the same self-test as a session's start. Anything that fails puts the dead end back.
5. Its answer goes to `coop-relays-answer.json`, and the browser lists again.

The kernel checks pasta's new route with a lookup that the policy's last rule refuses. So while pasta sets it, rules that
refuse UDP and ICMP stand in for that rule. pasta ends when the keeper does, a few seconds after the session.

## The Steam Linux Runtime

Steam never runs a current Proton directly. Proton's `toolmanifest.vdf` names a Steam Linux Runtime
(`require_tool_appid`; Proton - Experimental and GE-Proton name SteamLinuxRuntime_4 or sniper). Steam starts Proton
inside that container (pressure-vessel), and HellForge does the same.

`SteamRuntimeLocator` (and `hf_runtime_*` in `hf-steam.sh`, the same rules) reads the manifest beside the chosen Proton.
It finds the runtime app in any Steam library (`libraryfolders.vdf`, `appmanifest_<appid>.acf`), and builds the command
Steam would: `<runtime>/_v2-entry-point --verb=<verb> -- <proton> <verb> ...`. The line each session prints, and when
host mode is used, are in [Proton in the Steam Linux Runtime](linux.md#proton-in-the-steam-linux-runtime).

The session, the sandboxed domain, the live-war fetch, the Windows mod tools, setup's `wineboot` and the build-patch
fallback all use the chosen mode. HellForge's own steps (`ip`, `unshare`, the Steam bridge, the native CLI) stay
outside the container, which does not have those programs.

The order is fixed:

1. the offline network namespace;
2. the runtime container, which keeps that namespace (only loopback, measured);
3. `hf-sandbox`;
4. Proton.

The reverse is impossible by design, because `hf-sandbox`'s seccomp refuses new namespaces and pressure-vessel needs
them. A helper Proton call (a `reg` command, `wineboot`) never carries both `SteamAppId` and `SteamGameId`. That pair
makes Steam count the call as a HELLDIVERS 2 session, with its post-session shader processing.

## Why HellForge starts on the host

HellForge must start on the host because it makes the network namespace that keeps the game offline with the host's
programs. So `hellforge-linux.sh` refuses a start inside these
([started from inside a container](linux.md#started-from-inside-a-container)):

- Flatpak's seccomp filter blocks the user namespace HellForge needs.
- A Snap's confinement can block it, and lacks the host's programs.
- Steam's runtime container lacks `ip` and `pgrep`.

HellForge never offers to bypass a container (no `flatpak-spawn --host`), and every session keeps the namespace.

## Inside the wrapper

### Locale functions

The wrapper runs every Windows program with `DOTNET_SYSTEM_GLOBALIZATION_USENLS=1`. So `HellForge.Cli.exe` uses
Windows' NLS locale functions, which Wine implements itself, rather than ICU. The session runner starts that program for
each step, the mod loader starts it for a crash dump, and another launcher may start it in place of HellForge.exe.

Proton's `icu.dll` forwards every call to `icuuc68.dll`, which Proton links into the prefix from its own folder. When
that DLL does not load, .NET ends the program at its first culture-aware call (`Cannot get symbol u_charsToUChars from
libicuuc`, error 127 in `cli-output.txt`). The session then stops with "Mods could not be loaded". The session runner is
built with invariant globalization and uses neither.

### The time limit

Proton is bounded by `HELLFORGE_PROTON_TIMEOUT` (default `HELLFORGE_WAIT_TIMEOUT`, 12 hours). When that expires:

1. `hf-inner.sh` asks a sandboxed session's native supervisor to stop its domain, through the same
   `stop-observation.signal` STOP uses, and waits up to 30 seconds for it. The domain runs in the overlay prefix's own
   wineserver, so the supervisor's domain stop file and process-group TERM/KILL are the only way to end it.
2. `hf-inner.sh` runs the session Proton's `wineserver -k` for the real prefix.
3. It ends any game or backend process of the installation it finds, with a bounded TERM-then-KILL scan. So a game that
   outlived Proton's script does not keep running in the namespace.

### Shader caches

For a session, `hf-inner.sh` gives the game the shader caches Steam gives it. That is the folder
`steamapps/shadercache/553850` in the Steam library that has HELLDIVERS 2, with the same NVIDIA, Mesa, AMD and DXVK cache
variables Steam sets. The game then reuses the shaders Steam has already compiled for it, and on a Steam Deck the ones
Valve ships. Without them it compiles them again into an empty cache, which shows as short freezes the first time a
planet, the hellpod or a mission loads.

Like Steam, it also records the session's new pipelines with Steam's Fossilize layer, which Steam then compiles in the
background. Whether Steam shares them is Steam's own shader pre-caching setting.

- A variable the player has already set is kept.
- `DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1` turns the recording off.
- When Steam has no shader cache for the game, the wrapper says so and leaves the driver's defaults.
