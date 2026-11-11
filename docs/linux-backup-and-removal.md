# Backing up and removing HellForge on Linux

<!-- diataxis: how-to -->

For players on Linux: backing up and restoring the Proton prefix that holds HellForge, and uninstalling HellForge,
with what it removes and what it leaves.

## Prefix backup and restore

Settings > Installation > Prefix backup saves the whole Proton prefix, including the game copy, profiles, mods and
Steam's depot download, to one archive.

- The default folder is `hellforge-backups` beside the prefix, on the same drive. Choose another with **Choose folder**.
- The archive is named `hellforge-prefix-<date>-<time>.tar.zst` when `zstd` is on `PATH`, otherwise `.tar.gz`. Both are
  ordinary tar streams that standard tools can read.
- The launcher writes to a `.part` file and moves it into place when tar exits. Cancel deletes the partial file.

Backup refuses:

- while a session or setup runs, or while another launcher or any process uses the prefix;
- when the archive folder is inside the prefix;
- when the backup folder lacks room for the estimated prefix size plus a margin;
- a prefix that is the home folder, the filesystem root, Steam's folder or under `steamapps/compatdata`, or that has a
  link on its path.

**Restore prefix...** lists the archive with tar first. It refuses an archive that:

- is not one top-level folder;
- has an absolute path or `..`;
- contains a device or special entry;
- has a link that would leave the prefix. Wine's own `pfx/dosdevices` links and Proton's links to its own files are
  allowed, because Proton links its own files into every prefix. The target must be below `files` or `dist` in
  `steamapps/common/Proton*` or `compatibilitytools.d/<name>`.

Nothing in the archive may be written through a link. Restore refuses the same prefix locations as backup and needs the
archive's decompressor (`zstd` or `gzip`) on `PATH`. It unpacks the archive beside the prefix, and refuses when there is
not room for the unpacked prefix while the current one is kept. The current prefix is not touched until the restored
copy is complete. Then:

1. The launcher closes.
2. A helper waits for the prefix to be free.
3. It moves the current prefix aside and moves the restored one into place. If the move fails, the old prefix is put
   back.
4. It removes the old prefix and starts the launcher again.

## Removing HellForge

Settings > Installation > Uninstall HellForge... shows exactly what will be removed. Its confirmation button says
**Uninstall this installation**. After you confirm, it first removes what HellForge put outside the installation:

- Steam's depot download of the supported build, only when its checkbox is ticked. The checkbox is off by default, and
  while it is unticked the download is left where it is. It removes the `depot_<id>` folders under
  `<Steam>/steamapps/content/app_553850`, also looked for under `ubuntu12_32`, `ubuntu12_64` and `steamrt64`. It also
  removes the `state_553850_*.patch` download-state files in or next to that folder. The confirmation lists those paths
  under the checkbox. It warns that a depot download made for another HellForge installation in the same Steam library
  is removed too.
- The removed shell installer's app-menu entry, `~/.local/bin/hellforge-linux` command and
  `~/.config/hellforge-linux/config` saved settings, when the saved settings name this prefix.
- The Account Sync helper, relay, their hashes and the sudoers rule (`/etc/sudoers.d/zz-hellforge-capture`, and
  `hellforge-capture` from an earlier version), through the same pkexec confirmation and password dialog. If no polkit
  agent is available, the uninstall text shows the terminal `--remove` command and the helper files are left for it.

Then the launcher closes and a helper deletes:

- the installation folder (`game`, `profiles`, `mods`, `userdata`, `logs`, `temp`, `app` and the namespace marker);
- `~/Desktop/HellForge.desktop`, when it still has HellForge's exact `Exec=` line.

It leaves:

- Steam, HELLDIVERS 2 and your Proton versions;
- anything that does not prove it belongs to this installation;
- backups in the backup folder (`hellforge-backups` beside the prefix unless you chose another);
- Steam's own update and download state elsewhere under `steamapps`;
- the bridge's lock files in `${XDG_RUNTIME_DIR:-/run/user/<uid>}` (empty and harmless).

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

### The Proton prefix

The Proton prefix is left unless the native installer made it. The installer writes `.hellforge-created` into a prefix
that did not exist or was empty before, and the marker names the prefix. Uninstall offers the prefix only when:

- that marker matches;
- the prefix still contains this installation;
- it is not the home folder, the filesystem root, Steam's folder or under Steam's `compatdata`;
- no process is using it.

The checkbox is off by default. Ticking it deletes the whole prefix, including the game copy, every profile and every
mod. A prefix made by HELLDIVERS 2, one you chose as an existing non-empty folder, or one from an earlier HellForge
version has no marker and is never offered. Delete it by hand if you want it gone.

Links inside the prefix do not refuse it. Proton links its own files into every prefix, and the helper deletes with
`rm -rf --one-file-system`, which removes a link without following it. A link above the prefix, such as
`/home -> /var/home`, is resolved first, and the real folder is offered and deleted. Only a prefix or marker that is
itself a link is refused.
