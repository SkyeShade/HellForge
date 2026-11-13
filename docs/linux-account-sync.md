# Account sync on Linux

<!-- diataxis: explanation -->

For contributors and curious players: what Sync Inventory does on a Linux PC. It covers the native programs, the
root-owned helper, the certificate in the game's prefix, and how each change is checked and undone. Using it is in
[account sync](linux.md#account-sync).

It is the same capture as on Windows, but on Linux the proxy is a native program, and the two system changes are made by
one root-owned helper. Account Sync on Linux is experimental until it is recorded that the game trusts the CA's root
store during a real sync ([what the first syncs found](#what-the-first-syncs-found)).

## The native programs

**Review import**, the import and a backup restore run in `app/backend-linux/HellForge.LocalBackend`, the local
backend's profile commands built for Linux. It finds the signed-in Steam account with its own probe through the wrapper,
because a native Linux program trusts only a probe it ran itself. `HELLFORGE_STEAM_ACCOUNT` is read only by the Windows
backend under Wine. Before a sync starts, the launcher asks the same probe for the signed-in account.

## The root-owned helper

The first sync installs the helper through `pkexec`. The launcher never keeps root or a password, and writes no polkit
rule that skips the password. Only the absolute, link-checked shipped script `app/linux/allow-capture.sh` is elevated.
It:

- copies `capture-helper.sh` to `capture-helper` and `capture-relay` to `capture-relay` in the helper folder
  (below);
- records each installed copy's SHA-256 in `capture-helper.sha256` and `capture-relay.sha256` beside them;
- adds `/etc/sudoers.d/zz-hellforge-capture`, a rule for that one root-owned helper's exact `check`, `start`, `stop`
  and `cleanup` commands, and removes the rule's earlier name, `hellforge-capture`;
- removes a helper an earlier install left in another of the folders.

The helper folder is the first of these that can be made and that only root can change:

| System | Folders, first choice first |
|---|---|
| SteamOS | `/opt/hellforge`, `/var/lib/hellforge` ([why](linux-steamos.md#the-sync-helper-on-steamos)) |
| Any other | `/usr/local/lib/hellforge`, `/opt/hellforge`, `/var/lib/hellforge` |

A folder is passed over when it, or a folder above it, is not owned by root, is writable by every account, or is
writable by a group the account is in. The rule lets the account run the helper as root, so the account must not be
able to replace it. With no folder left, the installer installs nothing and names the reason for each.

sudo reads `/etc/sudoers.d` in lexical order and its last matching rule wins. The `zz-` name sorts after a
distribution's own files, such as SteamOS's `sudo` and `wheel`, which ask for a password.

There is no wildcard and no port argument. `start` reads the native proxy's pid from stdin, and the relay's ports are
fixed. The account comes from `SUDO_USER` or, under pkexec, from `PKEXEC_UID`. After the installer exits, the launcher
checks both installed hashes again.

Before each sync the launcher also runs `sudo -n` with the helper's `check` command. If sudo asks for a password, the
sync offers the install again, which writes the rule under the name that sorts last.

The helper checks its arguments. It cannot run Wine, a game, or a program chosen by the caller. `capture-relay` is a
tiny static C program built from `native/linux-capture`. The helper runs only the installed root-owned copy beside
itself, never one from the installation, which any Wine program can write.

## Which Proton opens the game's prefix

HellForge starts the Proton that Steam selects for HELLDIVERS 2, not HellForge's own Proton choice. It asks Steam's
`CompatToolMapping` for the Proton that opens HELLDIVERS 2's own prefix first. Current Steam often writes no mapping at
all, and a mapping can name a tool that is no longer installed. Then the prefix's own `compatdata/553850/config_info`
decides:

- Its first line is the prefix version, and its paths point inside the Proton that wrote it.
- HellForge strips the trailing `files/...` or `dist/...` part, and requires the `proton` script beside that root.
- It requires that Proton's `CURRENT_PREFIX_VERSION` to equal the prefix's `version`. Proton's install `version` file
  is a build id and is not compared with it.

If neither source gives a Proton, the message says both were looked for, and that HELLDIVERS 2 must have been started
once through Steam, which is what writes `config_info`.

## What changes during a sync

For the length of a sync, until the normal game exits, these change:

- **`/etc/hosts`** gets two marked lines: `# HellForge-CaptureProxy` and
  `127.0.0.1 api.live.prod.thehelldiversgame.com`. The helper rewrites the file next to itself and moves the new file
  into place, keeping its mode, owner and SELinux label, so a kill during the write cannot leave it empty. It removes
  only its own exact marker and the mapping directly below it. A player's line that merely contains the marker text
  stays.
- **The relay:** the installed relay binds `127.0.0.1:443` as root and drops to `nobody` with
  `setgroups`/`setgid`/`setuid` (each checked). Only then does it accept connections.
- **The game's prefix:** the proxy's one-day CA is imported into HELLDIVERS 2's own Proton prefix
  ([below](#the-certificate-in-the-games-prefix)).
- **The sync folder:** `logs/sync-<utc>/` is mode `0700`. The inventory, progression, state, certificate and seal files
  are mode `0600`. The snapshot is sealed natively with the machine-id transform. The account is the one the Steam probe
  confirmed. Captured JSON is never put in a log.

The relay forwards only to `127.0.0.1:18443`, where the native proxy listens. No argument can change either address or
port. Nothing is bound beyond loopback, the table of connections is bounded, and the relay stops with the helper.

The relay is a static musl program, whose account lookup reads only `/etc/passwd`. Fedora Atomic systems (Silverblue,
Kinoite, Bazzite) keep `nobody` in `/usr/lib/passwd` instead. So the relay reads that root-owned file when `/etc/passwd`
has no `nobody`. It refuses an entry with uid or gid 0.

These are not changed: the normal host CA bundle, the prefix's `hosts` file, nftables/iptables and Steam launch options.
Wine is never started as root.

## The certificate in the game's prefix

The proxy makes a one-day CA and writes only its public certificate to the sync folder. The private key exists only in
the capture proxy's memory, so a leftover CA cannot be used to intercept the game.

The supervisor imports the certificate into `steamapps/compatdata/553850/pfx`. It is stored as the Windows Root store's
serialized `Blob`, under `HKEY_LOCAL_MACHINE\Software\Microsoft\SystemCertificates\Root\Certificates\<SHA-1>`. The import
uses `proton run reg import` with a UTF-16 LE `.reg` file.

Before the game opens, two independent checks must both hold the byte-exact Blob:

1. The supervisor waits (bounded) for that prefix's own `wineserver -w`.
2. It exports the key with `proton run reg export <key> <Z: path in a fresh private temp folder> /y`, and parses that
   UTF-16 LE file's `"Blob"=hex:` value.
3. It reads `pfx/system.reg` as the second check.

The failure message says which check disagreed. `proton run reg query` is not used for the verdict: the measured Proton
gives it 0 bytes of stdout even for a known key.

Removal imports a delete `.reg`. It then requires `reg export` to write no file **and** `system.reg` to no longer have
the key after the flush. The export's exit code is only logged, since Proton 11 exits 0 for a missing key. Empty output
is never read as "gone", and a failed check is a failure, never "removed".

- Import and export stdout and stderr are kept apart, and written in full to `logs/sync-<utc>/launcher.log`. They stay
  as log evidence only.
- Proton's OpenXR-Loader, xrizer, fsync and "Proton: Upgrading prefix" lines stay, but are marked
  `[known Proton noise]`.
- A running wineserver for that prefix (Steam's game or a tool) is refused with "close Steam's game/tools in that
  prefix first".
- A record whose prefix differs from the game library's `steamapps/compatdata/553850` (for example a second Steam
  library) is refused before the import.
- The prefix's `version` file is read before and after the import, so a version change is recorded as evidence.

The prefix is not changed while the game runs, and Wine runs only while the game is closed. So the launcher waits for
HELLDIVERS 2 to close, then removes the CA.

## When a sync is interrupted

If HellForge itself is killed mid-sync, the helper watches the native proxy's pid. The proxy's pid is checked against
`/proc/<pid>/cmdline`, so a reused pid does not keep the redirect alive.

- The helper removes the hosts lines and the relay when the proxy exits.
- It has a hard lifetime cap of the proxy's 10-minute timeout plus two minutes, never more than an hour.
- Closing the launcher window while a sync runs stops the helper, and attempts the CA removal before the window closes.

The prefix CA is not removed by that helper. If HellForge was killed, the next launcher start finds `capture-ca.json`
and removes the certificate, with **Clean up** as the manual fallback. If the game is still running, the launcher
refuses to touch the prefix with Wine. It leaves the record until the next start after the game closes.

## What the first syncs found

- The first real Sync Inventory on Linux (2026-10-29; CachyOS, KDE Wayland, RX 7600, issue #112) stopped at an older CA
  check, before the game opened. The native proxy had written and served its CA ("CA written for the game prefix",
  "listening on port 18443"), and Steam mapped HELLDIVERS 2 to `proton-cachyos-11.0-20260506-slr`. The launcher
  reported that Wine's registry did not hold the Blob, although `reg import` had exited 0. The report kept only
  Proton's OpenXR-Loader/xrizer stderr.
- The reviewer's offline reproduction (`.deepcode/c6/repro.out`, 2026-10-29) found the cause.
  `proton run reg query <key> /v Blob` exits 0 with **0 bytes of stdout**, even for a known key. That held in all three
  combinations tried: Proton - Experimental on the host, `proton-cachyos-11.0-20260506-slr` on the host, and the same
  Proton inside SteamLinuxRuntime_4.
- In each, `reg import` exited 0 and `system.reg` held the key with a byte-exact Blob. Proton's own
  `files/bin/wine reg query` prints it, and `proton run reg export <key> Z:\...\out.reg /y` writes the value. So the
  CA import reaches the root store in every combination. The old check failed on every machine because it parsed console
  output that `proton run` does not deliver; the check now uses the export file and `system.reg`. The
  remaining open check is whether the game trusts that root store during a real sync.
- On the maintainer's own sync (2026-10-29), Steam wrote no `CompatToolMapping` at all: neither for 553850 nor a `0` default, in `config.vdf` or any
  `userdata` file. The prefix still records its Proton. `compatdata/553850/version` and the first line of `config_info`
  were both `11.0-100`. `config_info` pointed into `steamapps/common/Proton - Experimental/files/`, and that Proton
  script has `CURRENT_PREFIX_VERSION="11.0-100"`. The `config_info` fallback is
  for that case.
- Steam's install `version` file inside a Proton is a build id such as `1791989105 experimental-11.0-20261014-x86_64`,
  not the prefix version.
- On one PC (2026-10-29: Fedora 44, Proton - Experimental) a sync worked end to end. The launcher found the game's Proton from the
  prefix, and installed the helper through pkexec. The CA reached the prefix's `Root` store, and the game logged in
  through the capture (732 items and progression). Review import and the import ran in the native backend.

The full account of these runs is in
[the earlier version of the Linux page](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/linux.md#account-sync).
