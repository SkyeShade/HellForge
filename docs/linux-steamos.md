# HellForge on SteamOS

<!-- diataxis: how-to -->

For players on SteamOS 3.x (the Steam Deck and Steam Machine): what differs from [HellForge on Linux](linux.md), from
the read-only system image to first setup in Desktop Mode, the shader cache and SteamOS updates.

SteamOS 3.x is Arch-based. `hellforge-linux.sh` detects it by `ID=steamos` in `/etc/os-release`, so a missing program is
reported with SteamOS's own answer instead of a package-manager command. Sessions on SteamOS 3.x, the Steam Deck and
Game Mode are untested so far ([not known yet](linux-testing.md#not-known-yet)).

## Read-only root

Do not install HellForge's programs system-wide on SteamOS. `sudo steamos-readonly disable` makes the image writable
only until the next update, which drops the packages again and can leave an unsupported image.

`/` is a small read-only A/B system image. `/home` is a separate writable file system, and `/var` and `/run` are the
other writable places. A normal native installation, its prefix, game copy, logs, desktop shortcut and Steam's shader
cache all live under `/home` or a Steam library (a microSD card on a Deck). So a normal setup and session do not need to
write the system image.

The only system changes in this tree are the optional `allow-namespace.sh` and the Account Sync helper.
`allow-namespace.sh` is for Ubuntu's user-namespace restriction, which SteamOS is not expected to have. The helper is
needed: the first session needs one **Sync Inventory**, and a sync needs the helper. Its install does not write the
system image.

## The sync helper on SteamOS

The helper install works on the read-only image as it is. Do not run `steamos-readonly disable` for it. It is checked
on a stand-in system with a read-only `/usr/local` and real sudo, not yet on a SteamOS device.

| What | Where on SteamOS | Why |
|---|---|---|
| Helper, relay and their hashes | `/opt/hellforge` | `/usr/local` is on the read-only image. `/opt` is kept on `/home`, so it is writable and an update leaves it. |
| The same, if `/opt` cannot be used | `/var/lib/hellforge` | The installer takes the next folder that only root can change. |
| sudoers rule | `/etc/sudoers.d/zz-hellforge-capture` | SteamOS's own `sudo` and `wheel` files ask for a password; see below. |
| Keep list | `/etc/atomic-update.conf.d/hellforge.conf` | From SteamOS 3.6, an update drops `/etc` changes that no keep list names. The file names the rule, and nothing else. |

SteamOS ships `sudo` and `wheel` in `/etc/sudoers.d`, and both ask for a password. Valve's newer packages name them
`holo-group-sudo` and `holo-group-wheel`. sudo uses the last rule that matches, so HellForge's rule is named
`zz-hellforge-capture` to be read after them.

An installation from an earlier version has the rule as `hellforge-capture`, which those files override: a sync stopped
with "sudo: a password is required". The next **Sync Inventory** offers the helper install again and moves the rule.

If an update drops the rule anyway, the next sync finds that sudo asks for a password and offers the install again.
The helper in `/opt/hellforge` is still there, so only the rule is written again.

Uninstall, or `sudo bash <installation>/app/linux/allow-capture.sh --remove`, removes the helper folder, the rule and
the keep list.

## Programs

| Program | Needed for | Stock SteamOS 3.x |
|---|---|---|
| `ip` (iproute2) | loopback, the veth adapter and the `prohibit` routes | Expected: `iproute2` is in Arch's base set, and the Steam Machine ownership run passed through the namespace. |
| `unshare`, `flock` (util-linux) | the user and network namespace, and the bridge's locks | Expected, for the same reasons. |
| `pgrep` (procps-ng) | waits for the live-war fetch and the mod tools | Expected, for the same reasons. |
| `pasta` (passt), `python3` | co-op over Steam relays (on by default) | Expected missing: neither is in Arch's base set. A solo session does not need them; without them a session plays without co-op. |

The Steam Machine run's `find` and `ownership` steps passed, so at least that image had the required programs and
unprivileged user namespaces. It does not prove that a fresh Deck image is the same, or that the tester had not
installed anything. Treat "Expected" as unverified here.

## First setup in Desktop Mode

Run the installer and the setup wizard in Desktop Mode, not Game Mode. The wizard needs the desktop (and can need a file
picker and Steam's console), and the desktop shortcut it creates is not shown in Game Mode. Once setup says Ready to
play, add the launcher to Steam from Desktop Mode:

1. Steam > Games > **Add a Non-Steam Game to My Library**.
2. Browse to `<installation>/app/launcher-linux/HellForge.Launcher` and add it.
3. Set **Start In** to the installation root (the installer starts the launcher with that working directory). Rename it
   and choose an icon if wanted.

The entry then appears in Game Mode under Library > Non-Steam, and the launcher's PLAY button starts the session as
usual. Steam sets `SteamGameId` and `SteamAppId` for a non-Steam entry. The wrapper keeps an inherited `SteamGameId`
and sets `STEAM_COMPAT_APP_ID=553850` itself.

Not verified or tested yet:

- `steamos-add-to-steam`, if the image has it, may make these steps a single command.
- The Game Mode entry itself.
- The launcher is an Avalonia X11 application. Game Mode's gamescope normally provides XWayland, which this also needs.

A controller alone cannot navigate the desktop UI.

## Shader cache

Install the game through Steam once, even if HellForge copies its own supported build. The library then has
`appmanifest_553850.acf` and Valve's pre-compiled shaders. For a session, `hf-inner.sh` gives the game the same shader
caches Steam gives it: `steamapps/shadercache/553850` in the Steam library that has HELLDIVERS 2. The cache can take
several GB. It is Steam's, and prefix backup and uninstall do not include it.

## After a SteamOS update

The snapshot signer keeps its key in `/var/lib/private/hellforge-signer` and its public key in `/var/lib/hellforge`
([the snapshot signer](linux-account-sync.md#the-snapshot-signer)). If an update removes either, a snapshot is refused
until the next sync. Run **Sync Inventory** again, which makes a new signer. Whether SteamOS keeps both across an A/B
update is not established. Record them before and after one (the E.3 evidence list) and report the result.
