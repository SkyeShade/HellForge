# The Steam download of the supported build

<!-- diataxis: explanation -->

How setup gets the HELLDIVERS 2 build HellForge runs when Steam has another one, and why HellForge hosts no game files.

HellForge runs one HELLDIVERS 2 build, the supported build in `src/HellForge.Core/supported-build.json`. That file pins
the build's three depots (553851, 553853, 553854) at exact manifests, and every file's size and SHA-1, names and hashes
only. When Steam's own copy is that build, setup copies it. When Steam has another build (Arrowhead updated the game) or
no HELLDIVERS 2, setup gets the supported build from Steam for the player's account, in one of three ways
(`src/HellForge.SteamDownload`). The player's Steam install is only read, never changed.

HellForge distributes no game bytes. Every byte of the copy comes from the player's own Steam install or from Steam's
content servers, under the player's own licence. Earlier versions downloaded build patches from files.hf-mod.com
instead. A patch from a newer build to an older one carries the older build's content, so those patches were removed.

## Three ways to get it

Setup shows one card, Get the supported build from Steam, that says why Steam is needed: Steam sends the build to the
player's own account. It opens on the QR code. Each way has **Choose another way**, back to the three:

1. **Scan a QR code with your phone (the default):** the player scans it with the Steam mobile app (Steam Guard, then
   the QR scanner) and approves the sign-in there. HellForge signs in with it and downloads from Steam's content
   servers, fully automatically ([what is downloaded](#what-is-downloaded)). HellForge never sees the password.
2. **No phone: sign in with SteamCMD:** Valve's own command-line Steam tool downloads the build, and the player types
   the password, and any Steam Guard code, into SteamCMD's window ([below](#with-steamcmd)). No password is ever typed
   into HellForge.
3. **Use the Steam app:** no sign-in in HellForge at all. The Steam app on the PC downloads the build through its
   console, and HellForge copies it from there ([below](#with-the-steam-app)). It is also where setup goes when another
   way cannot work: a game borrowed through Family Sharing, SteamCMD that cannot be set up, and Linux.

The progress of every way shows in one place, the Game files card under it.

Nothing is kept. HellForge's QR sign-in is not persistent: no refresh token and no Steam Guard data is written
anywhere, and the next download asks again. The sign-in is a separate Steam session, so the Steam client stays signed
in. Steam may email the account about a new sign-in.

Every message the sign-in shows passes through a scrubber that hides the tokens. A test checks that no line of the
sign-in code that shows, logs, writes or throws text names one. It also checks that no password sign-in, password box or
hidden password prompt is left anywhere in HellForge.

The download runs on [SteamKit2](https://github.com/SteamRE/SteamKit) (LGPL-2.1, as its own unchanged DLL beside
HellForge's, which a player may replace). The code on top of it is HellForge's own. SteamKit2's licence, those of the
libraries it uses and QRCoder's, and where each one's source is, ship in `licences/` beside the launcher and the CLI
(`src/HellForge.SteamDownload/licences/THIRD-PARTY-NOTICES.txt`).

## What is downloaded

For each pinned depot, setup asks Steam for:

1. the depot's decryption key, which Steam gives only to an account whose licence covers the depot;
2. a manifest request code for the pinned manifest;
3. the pinned manifest itself, from a content server.

Each file of the supported build must be in the manifest with the size and SHA-1 `supported-build.json` lists, and its
chunks must cover it exactly once. Then setup plans every file chunk by chunk. A Steam chunk's id is the SHA-1 of its
content, so a chunk can be copied from anywhere on the PC that holds the same bytes:

- **Chunks the installed build has.** Steam's app manifest (`appmanifest_553850.acf`) names the manifests of the
  installed build. Setup reads them from Steam's own cache (`<Steam>/depotcache`), or fetches them like the pinned ones,
  and knows from them which file of Steam's install holds each chunk, at which offset.
- **The same place in the same file.** Without the installed build's manifest, setup still tries the bytes at the same
  offset in Steam's file. Most of an updated file is unchanged.
- **Everything else** downloads from Steam's content servers.

A file already in the copy with the build's content is kept. Every other file is written into `<file>.partial` in
order:

1. Each chunk is used only when its SHA-1 matches, whether it was copied or downloaded. A downloaded chunk that does not
   match is tried again, three times at most.
2. The whole file must then have the build's size and SHA-1 before it is renamed into place. A file that does not is
   deleted and setup stops.

A stopped download (Pause, a closed launcher, a lost connection) leaves the checked beginning of the file it was
writing. The next run copies that part from `<file>.resume` instead of downloading it again, and keeps every file
already in place.

Setup checks for room before it writes anything. On a full drive it asks for space ("Not enough free space on D:\: ...
more is needed"); the files already written are kept. The copy is about 24 GB; the download writes it straight into the
installation, so no second copy needs room.

On this PC's Steam (build 26251122 installed, October 2026), an older manifest of depot 553853 (23.5 GB) planned against
the installed build needed at most 0.44 GB from Steam's servers. The rest was found in the installed files. How much the
supported build needs depends on how far Steam's build has moved from it.

## With SteamCMD

HellForge gets SteamCMD from Valve's own download (`https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip`, or
`steamcmd_linux.tar.gz` on Linux) into its own tools folder, `<installation>/tools/steamcmd`, never into a Steam
library. On Windows, `steamcmd.exe` must carry a valid Authenticode signature by Valve, or it is deleted and never
started. The check is Windows' own, WinVerifyTrust, and the signer's organisation must be Valve or Valve Corp.
(Valve's `steamcmd.exe` is signed "CN=Valve, O=Valve".) Valve updates SteamCMD itself, so no hash of it can be pinned.
SteamCMD updates itself once, without a window, and is checked again.

Then it starts in a window of its own with every command filled in except the password: `+@ShutdownOnFailedCommand 1
+login <account name> +download_depot 553850 <depot> <manifest>` for each pinned depot, and `+quit`. The card asks for
the account name only: letters, digits and underscores, so it can never carry another command. It says: "Type your
password in the SteamCMD window; it goes to Valve's own tool, not HellForge."

SteamCMD asks for the password, and for a Steam Guard code when Steam wants one, in its window. Its `download_depot`
writes each depot into `<SteamCMD>/steamapps/content/app_553850/depot_<depot>/` (`linux32/steamapps` on Linux). Setup
reads it the way it reads the Steam app's download, showing the progress under Game files.

When SteamCMD ends, setup removes what it remembers of the sign-in. Setup waits for each of its processes first, since
SteamCMD may restart itself after an update. It removes:

- its `config` folder (`config.vdf` keeps the sign-in token, `loginusers.vdf` the account)
- `userdata`
- `logs` (they name the account)
- the Steam Guard files (`ssfn*`)

On Linux SteamCMD runs with its HOME inside its own folder, so it never reaches the player's own Steam folders, and that
is removed too. This happens however SteamCMD ended: done, closed, a failed sign-in, Stop SteamCMD or Choose another
way.

When the download is complete, each file is checked against its SHA-1 in `supported-build.json` and moved into the
installation. That is the same drive, so the 24 GB need room once. A wrong file is never moved. Then the whole SteamCMD
folder is deleted.

When it ended early, the card says why in plain words and offers Start SteamCMD again, which continues the download it
kept. A borrowed game cannot be downloaded this way either; the card then points to the Steam app.

## With the Steam app

The third way, and where setup goes when another way cannot work. **Open the Steam console** opens it
(`steam://open/console`), and each depot has its `download_depot 553850 <depot> <manifest>` command with a Copy button.

The Steam app downloads each depot into `<Steam>/steamapps/content/app_553850/depot_<depot>/`. The Linux client may use
`ubuntu12_32/steamapps`, `ubuntu12_64/steamapps` or `steamrt64/steamapps` under the Steam folder instead. Setup looks in
all of them and takes, for each depot, the folder that holds the most of the build. So an empty folder left by an
earlier download does not hide the real one.

Setup watches the download by itself and says which file it still waits for. Once every file has the build's size, it
copies them into the installation, each checked against the SHA-1 in `supported-build.json`. A file the Steam app wrote
wrong is never copied. Choosing another way while setup waits starts that way instead.

## When Steam does not allow the download

Steam refuses the depot key or the manifest request code when the account that signed in does not own the game. The
usual case is a game borrowed through Steam Family Sharing. Setup then says so and shows the Steam app's console
download ([above](#with-the-steam-app), and
[troubleshooting](troubleshooting.md#setup-cannot-find-the-supported-build)). It also shows it when the download from
Steam's servers stops for another reason (a lost connection, Steam's servers refusing), and when SteamCMD cannot be set
up. Choose another way tries the QR code or SteamCMD again; the files already downloaded are kept.

On Linux, setup runs inside HellForge's network namespace, which has no route out, so it cannot reach Steam's servers
and offers only the Steam app ([linux.md](linux.md#steams-download-of-the-supported-build)).

## After a game update

Nothing needs to be published. Setup downloads the supported build for any Steam build, and copies whatever the new
build still shares with it. A new HellForge release that targets a new build changes `supported-build.json`
(`HellForge.Cli supported-build`, from a Steam-validated install and its cached depot manifests).

## Trying it from a terminal

`HellForge.Cli steam-download` runs the same download outside the launcher ([external-launchers.md](external-launchers.md)):

```sh
HellForge.Cli steam-download --out <folder> [--file <a file of the build>] [--steam-root <Steam folder>]
                             [--sign-in qr | steamcmd] [--account <name>] [--steamcmd <folder>] [--dry-run yes]
```

It prints the QR code in the terminal. Its sign-in is forgotten after it runs.

- `--file bin/SONYGame.ini` fetches one small file of the build. With `--file` only that file's depot is downloaded.
- `--dry-run yes` prints the plan: per file, the bytes known to be on this PC and the most that would download.
- `--sign-in steamcmd` lets SteamCMD download it instead, in `--steamcmd` (default: beside `--out`, as
  `<out>-steamcmd`). It asks for the account name (or takes `--account`), and SteamCMD asks for the password itself, in
  the same terminal; HellForge never reads it. SteamCMD is removed once the files are moved into `--out`.

`--out` and `--steamcmd` must be outside any `steamapps` folder.

For the rest of setup, see [the docs map](README.md#what-setup-does).
