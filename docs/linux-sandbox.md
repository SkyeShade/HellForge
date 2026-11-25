# The Linux sandbox

<!-- diataxis: explanation -->

For contributors and testers: how the Linux sandbox runs the game in its own domain, what the sandbox probe and the
self-test check, and which design claims still need the game. Turning the sandbox on is in
[HellForge on Linux](linux.md#sandbox), and running the probe in [Testing HellForge on Linux](linux-testing.md).

The sandbox is separate from the network namespace that keeps the game offline
([How HellForge isolates the game on Linux](linux-isolation.md)). It ships in phases:

| Phase | What it is |
|---|---|
| 1 | The sandbox probe: checks the kernel's mechanisms and changes nothing |
| 2 | The filesystem sandbox: `hf-sandbox` runs the game in its own domain when Settings > Sandbox is on |
| 3, 4 | Later phases: the Wine helper set and seccomp rules (phase 3), the broker (phase 4) |

## The sandbox probe

Phase 1 is a probe, not a sandbox. It answers two questions: does this kernel provide the unprivileged mechanisms the
design needs, and do the four canaries hold in a throwaway child that uses them? The package ships the static x86_64
program as `app/linux/hf-sandbox-probe`. It needs no game, Steam, Proton, compiler, root or setuid helper.

The mechanisms are:

- Landlock ABI 3 or later;
- seccomp filter mode and user notification;
- unprivileged user, mount, PID, IPC and network namespaces;
- overlayfs in a user namespace with a read-only lower layer;
- a read-only bind of `/` with a writable session bind;
- private `/tmp` and `/dev/shm`;
- `openat2`, `close_range` and `PR_SET_NO_NEW_PRIVS`;
- the AppArmor user-namespace sysctl.

The canaries then try to:

- write outside the session folders;
- use `ptrace`/`process_vm_writev`/signals on a process outside;
- fork/exec/clone a new process;
- reach a loopback TCP port opened outside.

Each attempt must fail. Writes in the session folder, thread creation and a signal to the process itself must still
work. The probe makes its temporary folders under `TMPDIR` (or `/tmp`), bounds every child and removes the folders when
it exits.

Where `allow-namespace.sh` has been applied for Ubuntu's AppArmor restriction, the wrapper runs the probe under the same
allowed `unshare` copy and profile as a session. So the probe can still make its nested user namespace. The report
records that profile instead of calling the restriction a failure.

### Design claims that need the game

The design claims that need the game rather than this probe are items 8-18. They are phase 2-4 work:

- **Phase 2:**
  - 8: a per-session overlay prefix gets its own `wineserver`.
  - 9: trusted Windows clients reach a `wineserver` across the chosen namespaces.
  - 16: Wine's job and token calls are not a Linux boundary, and a forced `--sandbox` must be refused.
  - 17: `.NET` file-share locks do not stop Wine writes.
  - 18: the copy-back refuses links, FIFOs, devices and special files without hanging.
- **Phase 3:**
  - 14: the complete Wine helper set starts before the game is locked down.
  - 15: Wine exceptions, signals and threads still work with the seccomp rules.
- **Phase 4:** 10-13, the broker's named pipe flags, client pid and ring, Valve's DLL through `lsteamclient`, and
  Proton's DLL overrides in the game.

## How a sandboxed session starts

Phase 2 ships `app/linux/hf-sandbox`, the program that starts the game in its own domain. With Settings > Sandbox on, a
session runs the copied game through it:

1. The trusted session runner prepares the copy, the mods, the backend and the stand-in DLLs in the real prefix.
2. It writes a validated start request.
3. `hf-inner.sh` starts the native supervisor in `app/cli-linux/HellForge.Cli`, and the supervisor starts `hf-sandbox`.
4. A second `HellForge.Session.exe` in the overlay prefix, the domain host, starts the game and applies the memory-only
   launch edits.

The trusted runner never opens the game process.

`hf-sandbox --session <run folder> --prefix <prefix> -- <command>` makes a new user and mount namespace and runs the
command in a view built for it. Each step fails closed: if one fails, it prints the reason and the command does not run.
There is no command string and no shell: the request names fixed paths and an environment allowlist.

The supervisor passes the exact active mod folders, source-built DLLs, the mod loader and `app/session` as read-only
`--bind-ro` inputs.

- The supervisor drops a bind equal to or inside an earlier bind. hf-sandbox skips one again if a caller bypasses that
  check, so a mod folder covers the DLLs inside it.
- The native side repeats the allow list and refuses any link in a bind path.
- It refuses the installation, real prefix, session folder, real home or any folder above them.

### In the Steam Linux Runtime

In container mode the supervisor resolves the runtime itself, from `PROTON` and Steam's folder, never from the session
request. It starts `setsid <entry point> --verb=waitforexitandrun -- hf-sandbox ... --steam-runtime <runtime folder>
... -- <proton> waitforexitandrun <domain host>`.

Inside the container `hf-sandbox` builds the same view as on the host, plus what pressure-vessel moved:

- The runtime sockets are resolved before binding, and accepted only under the runtime folder, `/run/pressure-vessel`
  or `/tmp/.X11-unix`.
- Xauthority and the PulseAudio cookie, which pressure-vessel bind-mounts from deleted files, are copied (mode 0400).
- The container's `/run/host` system folders (the host's loader and GPU drivers) and `/var/pressure-vessel/ldso` are
  bound read-only.
- `/run/host/user-share`, `user-fonts-cache` and `etc` are never bound.

The supervisor starts the chain from the runtime's folder and without `TEMP`/`TMP`. pressure-vessel shares the working
and temporary folders as binds, and one inside the prefix stops the overlay ("failed to clone lowerpath", measured). For
the same reason a path inside another shared path is not passed to the container again.

A stop signals the domain's process group, then every process below the supervisor, which is a child subreaper.
pressure-vessel's processes start new sessions, so the group alone does not reach them. The readiness record (schema 2)
states the mode and the runtime (its folder and `VERSIONS.txt` SHA-256). A self-test from the other mode, or from an
older runtime, does not count.

## The domain's view

- **Prefix:** the command gets a per-session overlay of the Wine prefix. The real prefix is its read-only lower layer,
  and everything written to `C:` lands in the run folder's `sandbox/upper`, outside the view. An installation inside
  the prefix is hidden in the overlay too: through `C:` only its `game` folder is visible, read-only.
- **Read-only:** the system folders, Proton, the parts of Steam that Proton needs (`ubuntu12_32`, `ubuntu12_64`,
  `linux32`, `linux64`, the Steam runtimes), the game copy, the exact active mod inputs and `app/session`. Steam's own
  folder is not in the view, so its `config` folder, which holds the login data, is absent.
- **Writable:** the session's `game` folder (`gameOut`, reached through `Z:`), a private `/tmp` and `/dev/shm`, Steam's
  HELLDIVERS 2 shader cache, and Proton's `dist.lock` file only.
- **Session services:** the display and audio sockets (Wayland, X11, PipeWire, PulseAudio) are passed in. D-Bus is not.
- **Absent:** the home folder, and the rest of the installation: profiles, user data, the account snapshot and the real
  mods folder except the exact active inputs. The real prefix's path is absent too.
- **Devices:** every device is absent except null, zero, full, random, urandom, tty, the GPU nodes, `/dev/shm` and the
  game controllers (below). Keyboards, mice and `/dev/uinput` never enter the view. An evdev node of a keyboard would
  let the game read every key typed on the PC, and `/dev/uinput` would let it type into other programs.
- **Folder listings:** every folder of the view can be listed (Landlock `READ_DIR` on its root, nothing else). The
  view's root is `hf-sandbox`'s own tmpfs, so a listing shows only what the sandbox mounted and the folders leading to
  it.
- **Kernel locks:** Landlock enforces the same read and write sets again. A seccomp filter denies mounting, namespace
  changes (including `clone` with new-namespace flags), `clone3`, the x32 system calls, eBPF, io_uring and the kernel
  keyring. All capabilities are dropped and `no_new_privs` is set.

### Folder listings

Wine finds a path given in another letter case by listing each parent folder, and HELLDIVERS 2 opens its
`user_settings.config` lowercased. Without listings, the bare folders between the view's mounts refused the listing and
the settings did not load. The game then showed its first-time setup in every sandboxed session. The self-test checks
that a host file planted in the installation folder is not listed.

### Game controllers

The evdev nodes (`/dev/input/eventN`) and hidraw nodes (`/dev/hidrawN`) that are game controllers are bound at their
real paths, the ones Wine and SDL read from sysfs. So the game finds them as it does unsandboxed. Until 0.4.0-beta no
controller at all reached a sandboxed game.

- An evdev node counts when sysfs shows joystick or gamepad buttons and absolute axes, no keyboard key (`KEY_ESC` to
  `KEY_F12`) and no relative X or Y.
- A hidraw node counts when its report descriptor has a joystick, gamepad or multi-axis application collection, and no
  mouse, keyboard, keypad or digitizer one.
- Only a node the player can open is bound.

Without the udev database in the view, libudev still lists the devices (`udev_enumerate` allows uninitialized ones) and
SDL classifies them from sysfs. Before the game starts, the domain checks that every input or hidraw node in `/dev` and
`/dev/input` is such a controller at its own kernel name, and refuses the session otherwise. The self-test also opens
each one under the domain's rules. The view's `/dev` is fixed at the start: a controller connected later needs the next
session.

### A case-insensitive `drive_c` (SteamOS)

Proton 11 sets ext4's casefold flag on a new prefix's `drive_c` where the filesystem supports it, as SteamOS's does. The
kernel's overlayfs cannot use that:

- With the session's layers below `drive_c` (case-insensitive) and the prefix root above it (case-sensitive), the mount
  is refused ("case-insensitive directory ... is inconsistent", darcien on #112).
- Even with matching layers, every file below `drive_c` fails with "Object is remote" (measured on 7.2.12 with a
  casefold tmpfs).

So `sandbox-readiness` makes HellForge's own prefix, the one holding the installation, case-sensitive before its
self-test, as a prefix is on every other filesystem. Wine finds names case-insensitively by itself there. Each
case-insensitive folder is rebuilt top-down:

1. A case-sensitive sibling `.hf-cs-<name>` is made.
2. Every entry is moved into it with `renameat2(RENAME_NOREPLACE)`. Nothing is copied or replaced, and links are moved as
   links.
3. The old folder is removed, and the new one renamed into its place.

A journal file in the prefix (`.hellforge-case-sensitive.journal`) marks a conversion in progress. The next run
finishes a folder a stopped one left half-moved. The conversion is refused while a Windows program runs in the prefix
(its wineserver socket answers). The sandbox is then off for that session with the reason, and readiness tries again
next time. A prefix other than HellForge's own is never changed, only reported. `HellForge.Cli sandbox-prefix-case
--install <folder>` runs the conversion alone.

## Before and after the game

The game's user data and environment are copied into `gameOut` before the domain is built. When it exits, the native
`HellForge.Cli sandbox-copy-back` command stages only regular files with one link. Links, FIFOs, devices and hard links
are skipped, and a kernel without `openat2` is an error rather than a fallback. The trusted runner merges that staging
folder and applies the editor's saves.

The session home holds only `.steam/sdk32` and `.steam/sdk64` links to Steam's runtimes. There are no `.steam/root` or
`.steam/steam` aliases. The supervisor removes `.steam` in its after-domain cleanup.

The session config folder starts empty except for the player's `lsfg-vk/conf.toml`. It comes from `$XDG_CONFIG_HOME` or
`~/.config`, links resolved, at most 64 KiB, and is copied mode 0600. lsfg-vk is a frame-generation layer among the
player's implicit Vulkan layers, which the view binds. Without that copy it wrote its default config and ignored the
player's own, so a game the player had told it to leave alone crashed at start (#112). The game's edits to that copy
stay in the session copy.

The overlay's `<run>/sandbox/` (including mode-000 `work/work`) is removed last, whether or not the copy-back ran. An
empty `gameOut/prefix/merged` is removed too.

Every recursive walk over a run folder skips reparse points, so a link is never followed into Steam's `config` or
library. These walks are `SessionLogs.ExportFiles`, the retention size count, `CrashReport`, `SandboxStats` and the
engine-log scan. A sandboxed `result.json` records `gameExitCode`, `gameCrashed` and the loader's first crash line as
`loaderCrash`.

## Named pipes

Named pipes exist only inside their own Wine server. So in a multiplayer session the domain host serves the backend's
`hellforge-<run>` pipe and the live editor's `hellforge-live-<run>` pipe in the overlay. It relays each connection to the
trusted session runner in the real prefix over loopback TCP, and the runner connects it to the real pipe. Both ends use:

- a per-run token;
- length-prefixed frames of at most 64 KiB;
- bounded connection counts and a bounded byte rate.

TCP is used because the backend's HTTP loopback already works under Wine. A Unix socket from a PE process is still an
untested Wine contract.

Co-op, mod sync and the live editor therefore work in a sandboxed session. A solo session has no live-editor pipe, as
before. Steam itself still uses the wrapper's loopback bridge and the forwarding stand-in. This phase does not claim the
broker guarantee G4. G2 and G3 are phase 3.

## The self-test

The self-test is also the default switch. Settings > Sandbox is on when this PC passes it for this HellForge build and
kernel, and an update of either reruns it. A failure leaves the switch off and shows the reason.

The graphical launcher records the result. Before every session-starting command, the wrapper asks
`HellForge.Cli sandbox-readiness --install <installation>` to refresh a missing or stale record. It uses the installed
app's `app-build.json` and the kernel. That command runs the same self-test, and a failure is recorded with its reason.
So an external launcher, CLI `play` or harness session runs unsandboxed and reports it, instead of staying silently off.
A sandboxed session still reruns the self-test before Proton. One that cannot build its domain stops without an
unsandboxed retry.

`--sandbox-self-test` builds the same view as a real run from a throwaway tree in the installation's `temp/` folder,
with the default layout (the installation inside the prefix). It checks every boundary listed above, then removes the
tree. These checks include:

- a linked bind, the installation root and `profiles/` are refused;
- an allowed `mods/` input is readable but not writable.

The launcher and the native readiness command store the result with the app build and kernel, and show the first reason
on failure. When a session's readiness check runs the self-test, it takes the runtime choice that session's wrapper made
(`HELLFORGE_STEAM_RUNTIME_*`) instead of resolving it again. So the test runs the layering the session will run.
