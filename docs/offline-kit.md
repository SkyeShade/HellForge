# Installing HellForge on another PC from your installation

<!-- diataxis: how-to -->

How to hand HellForge to a friend, or put it on a laptop, from a USB stick or a shared folder instead of the releases
page. Your installation keeps its own installer, and a downpatch file can carry the game build. Together they are the
offline kit.

## What your installation keeps

Every install and update keeps a copy of the installer that ran, in `<installation>\installer`:

| Platform | Kept file |
|---|---|
| Windows | `HellForge-Setup-<version>.exe`, the setup program you ran |
| Linux | `HellForge-Linux-<version>.zip`, the Linux installer's package |

A `<file>.sha256` file beside it holds its SHA-256, in `sha256sum`'s format. The next update replaces both, so only the
installer of the version you run is kept. If the drive has no room for it, setup keeps none and Settings says why. Setup
copies only the file you started, never one unpacked to a temporary folder.

Going back to an earlier version ([docs/README.md](README.md#going-back-to-an-earlier-version)) leaves `installer`
as it is. It then holds the newer installer, so Copy the installer hands out that version, and running it updates
this installation to it again.

An installation made before this feature has no kept installer. Settings says so, and one appears after the next install
or update.

## Copy the installer

1. Open Settings, Installation, and press **Copy the installer...**.
2. Choose the folder to copy to, such as a USB stick or a shared folder.
3. Wait while HellForge checks the kept installer against its SHA-256, copies it and reads the copy back. **Stop**
   ends it and deletes the unfinished copy.

The folder then has the installer and its `.sha256` file. On Windows that is `HellForge-Setup-<version>.exe`. On
Linux it is a `HellForge-Linux-Installer-<version>` folder with `HellForge.Launcher` beside `HellForge-Linux.zip`, as the
release's installer has it.

HellForge refuses to copy and says why when:

| Message starts with | What to do |
|---|---|
| No installer is kept here | Run `HellForge-Setup.exe` (or the Linux installer) once more on this installation. |
| The kept installer is damaged | Run setup again to keep a good copy. Nothing was copied. |
| The kept installer has no SHA-256 file beside it | Run setup again, so a damaged copy can be told from a good one. |
| Not enough free space on | Free the space it names, or choose another drive. |
| The copy in ... reads back with SHA-256 | The drive may be failing. Try another folder or drive. |

To check a copy again on the other PC, compare the hash in the `.sha256` file with `Get-FileHash <file>` in PowerShell,
or run `sha256sum -c <file>.sha256` on Linux.

## What your friend needs

The installer holds HellForge only, never game files. Your friend still needs:

- their own HELLDIVERS 2 in Steam, since setup checks that the account owns it or has it through Steam Family Sharing;
- Steam running and signed in for that check, so the PC needs internet once during setup;
- the game build HellForge runs: Steam's download of it ([steam-download.md](steam-download.md)), or a downpatch file
  made from the build their Steam has ([local-downpatch.md](local-downpatch.md)).

Make the downpatch file in the same Settings card, with **Make a downpatch file...**, and put it beside the installer.

## From a terminal

External launchers and scripts use the CLI ([external-launchers.md](external-launchers.md#the-commands)):

```sh
HellForge.Cli installer [--install <installation>]
HellForge.Cli installer-copy [--install <installation>] --out <folder>
```

`installer` prints what is kept and `installer-copy` copies it with the same checks.
