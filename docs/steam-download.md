# The Steam download of the supported build

How setup gets the HELLDIVERS 2 build HellForge runs when Steam has another one, and why HellForge hosts no game files.

HellForge runs one HELLDIVERS 2 build, the supported build in `src/HellForge.Core/supported-build.json`. That file pins
the build's three depots (553851, 553853, 553854) at exact manifests, and every file's size and SHA-1, names and hashes
only. When Steam's own copy is that build, setup copies it. When Steam has another build (Arrowhead updated the game) or
no HELLDIVERS 2, setup downloads the supported build from Steam's own servers, for the player's account
(`src/HellForge.SteamDownload`). The player's Steam install is only read, never changed.

HellForge distributes no game bytes. Every byte of the copy comes from the player's own Steam install or from Steam's
content servers, under the player's own licence. Earlier versions downloaded build patches from files.hf-mod.com
instead. A patch from a newer build to an older one carries the older build's content, so those patches were removed.

## Signing in

Setup shows a sign-in card that says why it is needed: Steam sends the build to the player's own account. There are
two ways to sign in:

- **QR code (the default):** the player scans it with the Steam mobile app (Steam Guard, then the QR scanner) and
  approves the sign-in there. HellForge never sees the password.
- **Account name and password:** with a Steam Guard code when Steam asks for one, from the email Steam sends or from the
  Steam app.

Nothing is kept. The session is not persistent: no password, no refresh token and no Steam Guard data is written
anywhere, and the next download asks again. The password box is emptied as soon as it is read. Every message the
sign-in shows passes through a scrubber that hides the password and the tokens, and a test checks that no line of the
sign-in code that shows, logs, writes or throws text names one. The sign-in is a separate Steam session, so the Steam
client stays signed in. Steam may email the account about a new sign-in.

The download runs on [SteamKit2](https://github.com/SteamRE/SteamKit) (LGPL-2.1, as its own DLL beside HellForge's).
The code on top of it is HellForge's own.

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
order. Each chunk is used only when its SHA-1 matches, whether it was copied or downloaded; a downloaded chunk that
does not match is tried again, three times at most. The whole file must then have the build's size and SHA-1 before it
is renamed into place. A file that does not is deleted and setup stops. A stopped download (Pause, a closed launcher,
a lost connection) leaves the checked beginning of the file it was writing. The next run copies that part from
`<file>.resume` instead of downloading it again, and keeps every file already in place.

Setup checks for room before it writes anything. On a full drive it asks for space ("Not enough free space on D:\: ...
more is needed"); the files already written are kept. The copy is about 24 GB; the download writes it straight into the
installation, so no second copy needs room.

On this PC's Steam (build 26251122 installed, October 2026), an older manifest of depot 553853 (23.5 GB) planned against
the installed build needed at most 0.44 GB from Steam's servers: the rest was found in the installed files. How much the
supported build needs depends on how far Steam's build has moved from it.

## When Steam does not allow the download

Steam refuses the depot key or the manifest request code when the account that signed in does not own the game. The
usual case is a game borrowed through Steam Family Sharing. Setup then says so and shows Steam's console download
instead: one `download_depot 553850 <depot> <manifest>` command per depot, pasted into the Steam client's console, which
downloads the build into `<Steam>/steamapps/content/app_553850/`. Setup copies from there once every depot is complete
([troubleshooting](troubleshooting.md#setup-cannot-find-the-supported-build)). The console card also shows when the
download from Steam's servers stops for another reason (a lost connection, Steam's servers refusing). Check again
signs in again and continues it instead.

On Linux, setup runs inside HellForge's network namespace, which has no route out, so it cannot reach Steam's servers.
There the console download stays the way ([linux.md](linux.md#steams-download-of-the-supported-build)).

## After a game update

Nothing needs to be published. Setup downloads the supported build for any Steam build, and copies whatever the new
build still shares with it. A new HellForge release that targets a new build changes `supported-build.json`
(`HellForge.Cli supported-build`, from a Steam-validated install and its cached depot manifests).

## Trying it from a terminal

`HellForge.Cli steam-download` runs the same download outside the launcher ([external-launchers.md](external-launchers.md)):

```sh
HellForge.Cli steam-download --out <folder> [--file <a file of the build>] [--steam-root <Steam folder>]
                             [--sign-in qr | password] [--account <name>] [--dry-run yes]
```

It prints the QR code in the terminal, or asks for the password without showing it. `--file bin/SONYGame.ini` fetches
one small file of the build, and `--dry-run yes` prints the plan: per file, the bytes known to be on this PC and the most
that would download. `--out` must be outside Steam's `steamapps` folders.

For the rest of setup, see [the docs map](README.md#what-setup-does).
