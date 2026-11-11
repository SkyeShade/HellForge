# Testing HellForge on Linux

<!-- diataxis: how-to -->

For Linux testers and contributors: what is still untested, how to run the sandbox probe and what to report. Setup and
play are in [HellForge on Linux](linux.md).

Linux support grows from what Linux players try. If you get further than the Linux pages, find a fix, or test on a
distribution or Steam setup listed below as untested, open an issue with your logs or send a pull request. The scripts
are in [`linux/`](../linux) and the Wine checks in `src/HellForge.Core/LinuxNetworkBlock.cs`. See
[CONTRIBUTING.md](../CONTRIBUTING.md) for how to build and test.

## Not known yet

- **Native setup:** its direct depot reads, engine-package extraction, and the Proton ownership probe still need a full
  test on a Linux machine with Steam and the game depot download.
- **Co-op on Linux:** friends' games are reached through Steam's networking, which runs in the Steam client outside the
  namespace. The one test so far did not connect
  ([problems players have reported](linux.md#problems-players-have-reported)).
- **Containers and SteamOS:** Steam as a Flatpak or Snap (its own `/tmp` and paths) is untested. So are sessions on
  SteamOS 3.x ([HellForge on SteamOS](linux-steamos.md)), including the Steam Deck and Game Mode. The Account Sync
  helper's install there (`/opt/hellforge`) has only been run on a stand-in system. Starting HellForge itself inside
  Flatpak, Snap or Steam's runtime container is refused
  ([started from inside a container](linux.md#started-from-inside-a-container)). That path has not been tried on a real
  Flatpak or Snap either.
- **The Steam ownership check under Proton:** setup and sessions run it, as on Windows. A session reuses a check that
  passed within 7 days for the same installation, account and build, where it can see Steam's signed-in account. Under
  Wine, HellForge starts its Steam probe (`app\steamprobe\HellForge.SteamProbe.exe`) directly, because Wine's
  `powershell.exe` runs nothing. The probe must confirm the game on the signed-in Steam account, or setup and the session
  stop. It has not yet been run on a Linux machine. If setup or a session stops at "The ownership check did not pass",
  please report it with the logs.
- **Install, update and removal:** the graphical installer's install and update path (the staged app swap, rollback and
  new engine install) is untested with a real game and Steam on a Linux machine. So are build patches applied through
  Proton, prefix backup and restore, and uninstall's outside cleanup (including the AppArmor commands) and prefix
  deletion offer.
- **Account Sync:** it worked end to end on one PC (2026-10-29: Fedora 44, Proton - Experimental). It has not been tried on other
  distributions or on SteamOS yet ([what the first syncs found](linux-account-sync.md#what-the-first-syncs-found)).
- **The Steam Linux Runtime:** tested on one PC (Fedora 44, SteamLinuxRuntime_4). A sandboxed session reached the ship
  with co-op, the live editor and STOP working, and no domain process outlived STOP. Ubuntu's AppArmor user-namespace
  restriction with the runtime container is untested. If a sandboxed session there cannot start the container, it fails
  closed. Please report the result with the logs.

## Tested so far

- An Ubuntu 26.04 VM with plain Wine 10 (no Steam, no GPU; 2026-10-21) passed the namespace checks. These are the namespace as a
  normal user (with `allow-namespace.sh` where AppArmor restricts it) and Windows programs inside refused with WSAEACCES.
  They also cover the war fetched outside and served inside without a request to the mirror, the session runner's network
  probe, and the refusal while a wineserver of the prefix runs outside.
- In the same VM, `hellforge-linux.sh` started and stopped the Steam bridge, against a stand-in for Steam.
- That VM has no game files or live Steam client, so native setup, the Proton ownership probe and a real game session
  were not tested there.
- The native launcher UI starts on CachyOS. The game, Proton and the Steam client were tested by the testers in
  issue 1.

The full test log is in
[the earlier version of the Linux page](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/linux.md#tested-so-far).

## Sandbox probe

The probe is phase 1 of the planned Linux sandbox. It changes nothing: it does not enable the sandbox, does not refuse a
session, and does not change how the game starts. It checks whether this kernel provides the unprivileged mechanisms the
sandbox needs, and whether the four canaries hold. What it tests is in
[the sandbox probe](linux-sandbox.md#the-sandbox-probe).

Run it as your normal user, never with `sudo`:

```fish
bash "<installation>/app/linux/hellforge-linux.sh" --sandbox-probe
```

The wrapper prints one `PASS`, `FAIL` or `SKIP` line per mechanism and canary, with the evidence it saw, then a final
one-line result. It writes `logs/sandbox-probe.json` and prints that path.

- Exit code 0 means every mechanism is present and every canary held.
- Any other result is non-zero, and the last line names the first reason.
- A `SKIP` is not a pass.

On Ubuntu 24.04 and later, run the existing `allow-namespace.sh` step first when the AppArmor sysctl is 1. Without it
the probe reports the restriction as `SKIP` and exits non-zero, never as a pass.

### Tester procedure

Run the command above on each target. On SteamOS and Bazzite, run it from Desktop Mode.

- Fedora 44;
- Arch or CachyOS;
- Ubuntu 24.04 or later, with `allow-namespace.sh` applied if the sysctl is 1;
- SteamOS 3.x (the root filesystem is read-only and `/home` is separate);
- Bazzite.

Attach the report plus the exact command, `uname -a`, `/etc/os-release`, and
`cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns`. If the report says the probe is missing, the package is
incomplete; update HellForge. If a probe child says `SKIP`, include the report unchanged. It is the evidence that a
mechanism was unavailable or not testable, not a substitute for a pass.
