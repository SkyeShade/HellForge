# Downpatch files

<!-- diataxis: how-to -->

How to install HellForge's game build from a file a friend made, and how to make one for a friend. The Steam download
stays the default and recommended way ([steam-download.md](steam-download.md)). A downpatch file is for the day it
stops working.

**HellForge doesn't host these; you make one and share it yourself.** HellForge has no server, list or link for
downpatch files and never downloads one. A file is made on a player's PC and passed on by players, by whatever means
they choose.

## What a downpatch file is

HellForge runs one HELLDIVERS 2 build (`supported-build.json`). A downpatch file turns the build Steam installs today
into that build, on the PC of someone who owns the game:

- It is one `.zip` file, named `HellForge-downpatch-<Steam build>-to-<HellForge's build>.zip`.
- It works only on the Steam build it was made from. Made from build 26568081, it needs build 26568081 in Steam.
- It holds only what differs between the two builds. Files that are the same are named by their hash, and each changed
  file is a binary delta from Steam's file of the same path.
- Setup still checks that you own the game. Every file it writes must have the size and SHA-1 that Steam lists for
  HellForge's build, as with the Steam download.

## Install from a downpatch file

You need HELLDIVERS 2 installed in Steam, at the build the file was made from, and the downpatch file.

1. Start HellForge setup. When Steam has another build, setup shows the Get the supported build card.
2. Choose **Use a downpatch file**, then **Choose the downpatch file...**, and pick the file.
3. Setup checks the file, then writes the build into the installation's `game` folder. Progress shows under Game files,
   and Pause stops it. The next run keeps the files already written.

Setup refuses a file and says why when:

| Message starts with | What to do |
|---|---|
| This downpatch file was made from HELLDIVERS 2 build A, but your Steam has build B | Ask for a file made from build B. Steam's build is in the message. |
| This downpatch file makes HELLDIVERS 2 build A, but this HellForge runs build B | The file is for another HellForge version. |
| Your Steam's HELLDIVERS 2 says it is build A, but its game files are not that build's | Verify the game files in Steam, then try again. |
| The downpatch file is damaged | Copy the file again, or ask for a new one. |
| Not enough free space on | Free the space it names. The game copy needs about 24 GB. |

Your Steam install is only read, never changed.

## Make a downpatch file for a friend

You need HellForge installed with a complete game copy, and HELLDIVERS 2 in Steam at the same build as your friend's.
Usually that is the latest build, since Steam updates both.

1. Open Settings, Installation, and press **Make a downpatch file...**.
2. Choose where to save it. The drive needs room for the file, about 1 to 2 GB after a game update.
3. Wait while HellForge checks its game copy, reads Steam's game files and makes the file. **Stop** ends it and saves
   nothing.
4. Send the file to your friend yourself.

HellForge checks each part of the file by applying it once before it saves it. It refuses to start when its game copy
has a changed file (run Check and repair first), and when Steam already has HellForge's build.

## From a terminal

External launchers and scripts use the CLI ([external-launchers.md](external-launchers.md#commands)):

```sh
HellForge.Cli downpatch-create [--install <installation>] [--out <file or folder>] [--steam-root <Steam folder>] [--workers <n>]
HellForge.Cli downpatch-apply --file <downpatch file> --out <game folder> [--steam-root <Steam folder>]
```

Both find HELLDIVERS 2 in Steam as setup does. Both print JSON with `result` (the builds, file counts by operation,
bytes and seconds) and the file or folder written.

## What is in the file

The zip holds `README.txt`, `hellforge-downpatch.json` and one stored entry per delta under `files/`. The JSON names:

- the source build: Steam build id, SHA-256 of `bin/helldivers2.exe` and `data/game/game.dll`, and depot manifests;
- the target build, the same way;
- every file of the target build, once, with its size and SHA-1, and one operation:

| Operation | Meaning |
|---|---|
| `same` | Steam's file at `from` is already this file. |
| `patch` | The delta in `entry` makes it from Steam's file at `from` (whose size and SHA-1 are listed). |
| `new` | The delta in `entry` carries it whole: Steam has no such file, it is small, or it is `data/settings.ini`, which players change. |

Each entry also lists its size and SHA-256. Applying trusts none of it:

- the list must name exactly the files of HellForge's build, with their own sizes and hashes;
- every path must be a plain relative path inside the game folder;
- each written file is checked before it is moved into place.

The deltas are HellForge's own format (`src/HellForge.Core/BinaryDelta.cs`), in managed code, so Windows and Linux run
the same thing and no diff tool is downloaded or shipped. Matching works like rsync: Steam's file is indexed in
512-byte blocks by a rolling hash, and each match grows to its exact bytes. The result is Brotli-compressed.
