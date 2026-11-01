# Launchers of your own

This page is for people writing a mod manager or launcher on top of HellForge (Echelon, or your own). It lists the
commands, the JSON they print and the files in an installation that such a program may rely on.

A launcher of your own can do everything HellForge.exe does for a player while HellForge.exe stays closed: find the
installation, manage its mods and profiles, start and stop sessions, and show the logs. It does all of this through
`HellForge.Cli.exe` and the files described here.

Whoever starts a session, the engine's session runner (`app\session\HellForge.Session.exe`) still does every check and
every cleanup it needs: the game copy, Steam ownership, the network block, the local backend, and staging and building
the mods. A launcher never touches the backend, the network block, `app\` or `game\` itself.

The interface is the commands and the JSON they print. Other files in an installation can change between versions;
`status` tells you which HellForge, engine and mod SDK an installation has.

## A launcher of your own, step by step

1. Find the installation (next section) and run `status`. Its versions say which HellForge you are talking to.
2. If `ready.gameCopy` is false, the installation is not set up yet. Start its `HellForge.exe` once: the setup wizard
   copies (or patches) the game from the player's Steam library, confirms ownership with Steam and places the network
   block, which takes one Windows administrator prompt. Repairs also go through HellForge.exe (Settings > Check and
   repair). Everything after that works with HellForge.exe closed.
3. Manage mods with `mods`, `mod-add`, `mod-remove`, `mod-switch`, `mod-order` and `mod-pack`. They all work on the
   installation's `mods\` folder, which HellForge.exe's Mods page shows too, so both see the same list. Lua mods go in
   the same way (see [Mods](#mods)).
4. Manage profiles and settings with `profiles` and `settings`.
5. Start a session with `play --wait yes`, show its status lines, and stop it with `stop`.
6. Show what happened with `logs`, and let the player send `logs --export <file.zip>` when they need help, or
   `logs --crash-report <file.zip>` after a session that `crashed`.

To update HellForge, the player runs a newer HellForge-Setup.exe on the same folder. It replaces `app\` and keeps the
game copy, profiles, mods and settings. Compare `status` before and after if your launcher depends on a version.

## Finding an installation

An installation is a folder with `hellforge-install.json` in it (`{"schemaVersion": 1, "buildId": "25480438", ...}`).
Installations from before the rename have `bts-install.json` instead. HellForge-Setup suggests
`%USERPROFILE%\Documents\HellForge` (or `%USERPROFILE%\HellForge` when Documents is not usable, for example when OneDrive
holds it), but players can pick any folder outside Steam's `steamapps`. HellForge writes nothing to the registry. Its
optional shortcuts, `HellForge.lnk` on the Desktop and in the Start menu, point at `<installation>\HellForge.exe`.

| Path | What it is |
|---|---|
| `HellForge.exe` | the launcher |
| `app\cli\HellForge.Cli.exe` | the commands below |
| `game\` | the copy of HELLDIVERS 2 that sessions run (`bin\helldivers2.exe`, `data\`); the player's Steam install is never used |
| `mods\` | every mod, and `hellforge-mods.json` (on/off, order, pack options) |
| `profiles\<name>\` | HellForge profiles: each one's progress and inventory |
| `launcher-settings.json` | the launcher's choices (selected profile, war, multiplayer...) |
| `logs\` | session logs (see [Logs](#logs)) |
| `userdata\` | the game's own settings, saves, logs and crash dumps |

## The commands

Run `<installation>\app\cli\HellForge.Cli.exe <command> [options]`. `--install <folder>` is optional and defaults to the
installation the CLI is in. Every command prints JSON on standard output. A failure prints `HellForge: <why>` on
standard error and exits with 1.

| Command | What it does |
|---|---|
| `status` | Prints the versions (HellForge, engine, mod SDK, game build), readiness (`ready.gameCopy`, `ready.sessionRunner`, `ready.linuxNamespace`), how the copy is kept off the internet (`networkBlock`: `wfp`, `firewall` or `linux-netns`), the launcher's settings, the last session, whether one is running, and the language packs in the copy (`languages`; English is always there). |
| `profiles` | Lists the profiles (name, template, folder, used, selected) and the templates. `--create <name> --template <template>` makes a profile; the name is 1 to 32 lowercase letters, digits or hyphens. |
| `mods` | Prints the Mods page's list, top first. Each mod has its id, name, version, description, enabled, group, tags, folder, archive (its zip, if it came as one), removable, and a pack's options. Also printed: the folder's problems, the modlist code friends compare (null when no gameplay mod is on) and the path of `hellforge-mods.json`. |
| `mod-add --file <path>` | Adds a mod the way the Mods page's "Add mods..." does: a `.zip`, `.rar` or `.7z`, a folder mod's `hellforge-mod.json`, or a cosmetic pack's `manifest.json`. A mod with the same id is replaced (an update). |
| `mod-remove --id <id>` | Deletes the mod's zip or folder. Mods HellForge ships (tag `bundled`) come back by themselves: switch these off instead. Local Stats (tag `local-stats`) cannot be removed. |
| `mod-switch --id <id> --on yes\|no` | Switches a mod on or off. Local Stats (tag `local-stats`) is always on and refuses `--on no`. |
| `mod-order --ids <id,id,...>` | Puts the listed mods first, top first, in that order. The others keep their order after them. Gameplay mods stay above cosmetic ones, so the order changes within each group. |
| `mod-pack --id <pack id> --option <name> [--on yes\|no] [--variant <name>]` | Switches one of a cosmetic pack's options, or picks one of its variants, as the pack's picker on the Mods page does. `--all yes\|no` switches every option at once. Prints the pack's options. |
| `settings [options]` | Prints the choices `play` and HellForge.exe start from, after saving any option given (see [Profiles and settings](#profiles-and-settings)). |
| `logs [--run <id>] [--export <file.zip> \| --crash-report <file.zip> [--dump yes \| no]]` | Lists the sessions, newest first, with how each ended; `--run` adds one session's files; `--export` zips a session's logs; `--crash-report` makes its crash report (see [Logs](#logs)). |
| `play` | Starts a session (see [Sessions](#sessions)). |
| `stop [--run <run folder>] [--wait yes]` | Asks the running session to close the game and clean up, as STOP does. Without `--run` it stops the newest session. |
| `fetch-war` | Fetches the live galactic war into `live-war.json`. The Linux scripts run this before a session. |
| `languages [--steam-root <Steam folder>]` | Lists the language packs pinned for the supported build: `name`, `dlcAppId`, `bytes`, `inCopy`, `installedInSteam` (the player owns and installed that language in Steam) and the `command` to paste into Steam's console to download it at the pinned manifest. |
| `language-add --name <language> [--steam-root <Steam folder>] [--keep yes\|no]` | Copies one pack from Steam's download into the game copy, every file checked against the pinned list, then deletes the download unless `--keep yes`. Refused while a session runs, and until Steam has finished the download. |

The tags in `mods` are `source`, `code`, `stats`, `data`, `pack`, `lua`, `bundled` and `local-stats`. The Mods page does
not show Local Stats (the in-game editor's saved changes, always on and never compared between friends); `mods` still
lists it, so a launcher can hide it the same way.

Mod changes are refused while a session runs, as on the Mods page, because a session keeps the mods it started with.
HellForge.exe can stay open meanwhile: its Mods page watches the folder.

## Mods

Put mods in `mods\`, with `mod-add` or by copying a zip or folder there, and never into `game\data`. Each session stages
every enabled mod's patches into the copy itself, in load order, and removes the ones it staged the time before
([modding/data.md](modding/data.md#patch-files)). Patches put into `game\data` by hand are left where they are, but no
list shows them and nothing orders them.

Everything in `mods\` appears on HellForge's Mods page, whoever put it there. A mod's id is the `id` in its
`hellforge-mod.json`. A cosmetic pack's id is `pack.<its manifest's Guid>`, or `pack.<its name, as a slug>` when the
manifest has no Guid. `mods` prints them.

The page has two groups:

- Gameplay mods: friends need the same ones in co-op, and they make up the modlist code.
- Cosmetic mods, which include Lua mods (tag `lua`: Bingus Shared Loader, HD2Runtime and the mods made for them). These
  are client-only and never part of the modlist code; see [lua-bridge.md](lua-bridge.md).

The page reads from the bottom up: the bottom mod loads first and the top one wins.

If you write `mods\hellforge-mods.json` yourself, it looks like this:

```json
{
  "format": 1,
  "disabled": ["owner.mod-id"],
  "order": ["top.mod", "next.mod"],
  "packs": { "pack.<guid>": { "off": ["Option name"], "sub": { "Option name": "Variant name" } } },
  "blocked": ["owner.mod-id"],
  "share": true,
  "showEditedStats": false,
  "listPublic": true
}
```

Mods are on unless `disabled` lists them. `order` is top first, and mods missing from it go to the top of their group.
A pack's `off` lists the options switched off, and `sub` the variant picked for an option that has variants (otherwise
the first one is used). `blocked` lists the mods mod sync never offers (see [mods.md](mods.md#playing-with-other-mods)),
`share` (default true) lets players who join you download your gameplay mods, and
`showEditedStats` (default false) shows public lobbies whose host plays with edited stats, and `listPublic` (default
true) lists the player's own Public lobbies on other HellForge players' Galactic Map. Write the whole file at once, to a
temporary file and then a rename, as HellForge does.

## Profiles and settings

`play` uses the launcher's settings unless an option replaces one, so a player gets the same session from either
launcher. It never changes `launcher-settings.json`.

To change them for good, use `settings`. It takes the same options as `play` (`--profile`, `--war`, `--local-playfab`,
`--multiplayer`, `--engine-log`, `--crash-dump`, `--sandbox`, `--game-options`), saves the ones given, checks them as `play` would,
and prints every setting:

```json
{"profile": "default", "war": "live", "localPlayFab": true, "multiplayer": true, "engineLog": true, "crashDump": false,
 "gameOptions": "", "sandbox": true, "steamBridge": true, "playWithStrangers": false,
 "file": "<installation>\\launcher-settings.json"}
```

`settings --strangers yes|no` also turns Play with strangers on or off (off by default; see
[multiplayer.md](multiplayer.md#play-with-strangers)). It is saved in `mods\hellforge-mods.json`, not in
`launcher-settings.json`, and `play` has no option for it. A running session's backend picks it up within a few seconds.

HellForge.exe reads the same file, so a choice made in your launcher is the one its Settings page shows. `profiles`
lists and creates profiles; `settings --profile <name>` selects one.

## Sessions

```
HellForge.Cli.exe play [--profile <name>] [--war live | frozen | open] [--local-playfab yes | no] [--multiplayer yes | no]
                       [--engine-log yes | no] [--crash-dump yes | no] [--sandbox yes | no] [--game-options "<launch options>"]
                       [--full-ownership-check yes | no] [--wait yes] [--dry-run yes]
```

| Option | Default (the launcher's setting) | Meaning |
|---|---|---|
| `--profile` | the selected profile | which HellForge profile plays |
| `--war` | `live` | `live`: the current galactic war, read-only through the community mirror; `frozen`: the war HellForge ships; `open`: every planet open |
| `--local-playfab` | `yes` | the game's PlayFab calls go to the local backend |
| `--multiplayer` | `yes` | co-op with Steam friends |
| `--engine-log` | `yes` | the game's own log, copied into the session's logs |
| `--crash-dump` | `no` | a full memory dump if the game crashes (large) |
| `--sandbox` | `yes` where the sandbox can run | the game at low integrity, with Steam through the Steam broker ([steam-bridge.md](steam-bridge.md)); on unless turned off, but only with an engine built with the Steam bridge (`settings` prints `steamBridge`) and only on Windows, so `play` runs unsandboxed elsewhere even when `settings` prints `"sandbox": true`. An installation with the older Windows Firewall rules gets the WFP block placed first (one Windows prompt) |
| `--game-options` | the launcher's launch options | options for the game's command line, as Steam takes them; options a session sets itself are refused |
| `--full-ownership-check` | `no` | `yes` asks Steam to confirm ownership even when a check of this installation passed within the last 7 days for the same Steam account and game build (which a session otherwise reuses); for this session only, not a setting |
| `--dry-run yes` | | prints the session runner's command line and starts nothing |

Once the session runner has made its run folder, `play` prints
`{"event": "started", "runId": ..., "run": <run folder>, "pid": ..., ...}` and returns. The session goes on without it.

With `--wait yes`, `play` stays and prints one JSON line each time the session's status changes, then
`{"event": "ended", "status": ..., "result": ..., "logs": ...}`. It exits with 0 when the session ended normally. A line
`{"event": "mod-problems", "problems": [...]}` lists mods the session left out (a mod that did not compile, data that
was not built...). Closing `play` does not stop the session; `stop` does.

| `status` | Meaning |
|---|---|
| `preparing` | building mods that ship sources, then checking the copied game (progress in `<run>\mod-builds.json`) |
| `awaiting_network_block_install` | Windows asks the player, once, to install the network block |
| `awaiting_windows_firewall_consent` | Windows asks for this session's firewall rules |
| `patching` | launching the game |
| `observing` | the game is running |
| `observed` | the session ended normally |
| `failed` | the session stopped; `failure` says why |
| `cleanup_failed` | the cleanup failed: check the logs before playing again |

`awaiting_windows_firewall_consent` and `cleanup_failed` come from older session runners. The current one
(`src/HellForge.Session/PlaySession.cs`) does not write them, but HellForge.exe and `play --wait` still recognise them,
so handle them too.

One session runs at a time on a PC, because the game runs only once. `play` refuses while one is running, in any
installation.

### Logs

Each session has its own folder, `logs\sessions\<runId>\`, and `logs\play-session.json` names the newest one (`"run"`).
Only the newest ten are kept. These are the files to show a player:

| File | What it holds |
|---|---|
| `session.log` | the session runner's steps; it opens with HellForge's, the engine's, the mod SDK's and the game's versions, Windows' version (and Wine's), the GPUs and their drivers, the session's settings, and the mods with their versions and content hashes |
| `result.json` | the session's status and outcome; `report` holds the opening facts of `session.log` as JSON; `ownershipCheck` is `full` when it asked Steam, `recent` when it reused a check that passed at `ownershipConfirmedUtc` |
| `modloader.log` | the mod loader: which mods loaded, and why one did not |
| `modlist.txt` | the mods the session ran, in load order |
| `mod-<id>.log` | a code mod's own log; `mod-lua.<name>.log` is a Lua mod's |
| `cli-output.txt` | mod builds and staging |
| `engine-logs\` | the game's console log |

Anything that stops a session before its run folder exists goes to `logs\session-runner.log`. The game's crash reports
are in `userdata\`.

`logs` reads all of this for you. It lists the sessions newest first, each with `id`, `folder`, `startedUtc`, `status`,
`failure`, `gameExitCode`, `completedUtc` (a session still starting has no status yet) and `crashed` (the game ended by
itself with an error, the mod loader noted a crash, or it left a crash dump), and prints the paths of
`session-runner.log` (when there is one) and the game's own log folder. `--run <id>` adds that session's files with their
sizes.

`logs --export <file.zip>` makes the zip the launcher's Save logs to Desktop makes, of the newest session or the one
`--run` names. Text files and file names in it say `%USERPROFILE%` and `PC` instead of the player's user folder and
computer name, whoever wrote them. Programs are left out, and a crash dump goes in as it is. While a session runs, its
logs are read with shared access, and a file it has locked is left out (`skipped` counts them). The zip must not exist
yet.

`logs --crash-report <file.zip>` makes the crash report the launcher's Save crash report makes, of the newest session or
the one `--run` names: one zip for a player to attach to a GitHub issue. Nothing is uploaded; offer
`https://github.com/SkyeShade/HellForge/issues/new` (`issuePage`, no parameters) for them to attach it. It holds
`summary.txt` (the versions, system and settings, the game's exit code, the mod loader's crash note: its code, module and
offset and thread, the mods, the loader's events before the crash and the last 200 lines of its log), `logs\` (the
export, without crash dumps) and, only with `--dump yes`, `dump\crash-<pid>.dmp`. Its text is scrubbed further than the
export, for a public issue: Steam ids, IP addresses other than loopback and the other players HellForge's logs name are
replaced with `steam-id-1`, `ip-1`, `player-1` and so on. A dump is the game's memory and cannot be scrubbed: ask the
player before passing `--dump yes`. The JSON says `files`, `skipped`, `dump` (whether it went in), `dumpAvailable` and
`issuePage`.

## Linux

On Linux, HellForge's game runs only inside its network namespace, which `app/linux/hellforge-linux.sh` sets up
([linux.md](linux.md)). Outside it the session runner stops before the game starts, as it does for HellForge.exe. The
script also bridges Steam and fetches the live war while the PC still has its network. Give it the program to run in
place of HellForge.exe, with its arguments:

```
PROTON=<proton> STEAM_COMPAT_DATA_PATH=<prefix> app/linux/hellforge-linux.sh "$INSTALL/app/cli/HellForge.Cli.exe" play --wait yes
```

The script returns when the session's programs have ended. `stop` and the other commands also run through Proton
outside the namespace (`"$PROTON" run "$INSTALL/app/cli/HellForge.Cli.exe" stop`). None of them goes online except
`fetch-war`, which is meant to.

## Related pages

- [mods.md](mods.md): the Mods page and playing with friends, as players see them.
- [engine.md](engine.md): what the session runner, the local backend and the mod loader are, and where their source is.
- [multiplayer.md](multiplayer.md): how co-op works.
- [The docs map](README.md): every document, by reader.
