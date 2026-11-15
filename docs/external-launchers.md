# Launchers of your own

<!-- diataxis: reference -->

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
game copy, profiles, mods and settings. Compare `status` before and after if your launcher depends on a version. On
Linux, the player runs a newer `HellForge-Linux-Installer` with the same prefix instead; see
[linux.md](linux.md#updating).

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
| `logs [--run <id>] [--export <file.zip> \| --crash-report <file.zip> [--dump yes \| no]]` | Lists the sessions, newest first, with how each ended. `--run` adds one session's files. `--export` zips a session's logs, and `--crash-report` makes its crash report (see [Logs](#logs)). |
| `play` | Starts a session (see [Sessions](#sessions)). |
| `stop [--run <run folder>] [--wait yes] [--now yes]` | Asks the running session to close the game and clean up, as STOP does. Without `--run` it stops the newest session. `--now yes` ends the game at once instead of asking it to close first. |
| `fetch-war` | Fetches the live galactic war into `live-war.json`. The Linux scripts run this before a session. |
| `languages [--steam-root <Steam folder>]` | Lists the language packs pinned for the supported build: `name`, `dlcAppId`, `bytes`, `inCopy`, `installedInSteam` (the player owns and installed that language in Steam) and the `command` to paste into Steam's console to download it at the pinned manifest. |
| `language-add --name <language> [--steam-root <Steam folder>] [--keep yes\|no]` | Copies one pack from Steam's download into the game copy, every file checked against the pinned list, then deletes the download unless `--keep yes`. Refused while a session runs, and until Steam has finished the download. |
| `steam-download --out <folder> [--file <a file of the build>] [--steam-root <Steam folder>] [--sign-in qr\|steamcmd] [--account <name>] [--steamcmd <folder>] [--dry-run yes]` | Downloads the supported build from Steam's servers into `--out` for the account that signs in, as setup does ([steam-download.md](steam-download.md)), copying what Steam's install already has. It is interactive: it prints a QR code on standard error to approve in the Steam app. With `--sign-in steamcmd`, Valve's SteamCMD downloads it instead into `--steamcmd` (default `<out>-steamcmd`; asks for the account name unless `--account` gives it), and SteamCMD asks for the password and any Steam Guard code in the terminal itself. The files are then checked and moved into `--out`, SteamCMD's sign-in is forgotten and SteamCMD removed, and it prints `files`, `bytes`, `by`, `destination` and `seconds`. Nothing of the sign-in is kept or printed. `--file` takes one file of the build; `--dry-run yes` prints the plan (`files`, `bytes`, `knownLocalBytes`, `downloadAtMostBytes`, `notes` and a `plan` per file) and writes nothing. Without it, prints `files`, `bytes`, `downloadedBytes`, `copiedBytes`, `destination`, `seconds` and `notes`. `--out` and `--steamcmd` must be outside any `steamapps` folder. It needs no installation. |
| `steam-signout` | Deletes HellForge's saved Steam sign-in, the one the asset kit's fetches keep (`<ApplicationData>/HellForge/steam-login.json`; [signing in to Steam](modding/assets.md#signing-in-to-steam)). It prints `signedOut` (whether there was one) and `file`. The asset migrator's saved sign-in is its own and stays. |

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

The page has three groups:

- Gameplay mods: friends need the same ones in co-op, and they make up the modlist code.
- Per-player gameplay mods: stats-only mods, texture/sound/text-only patch mods, and manifest `"coop": "per-player"`
  mods. Each player keeps their own; they are not in the modlist code and never download in co-op.
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

| Field | Meaning |
|---|---|
| `disabled` | Mods are on unless `disabled` lists them. |
| `order` | Top first. Mods missing from it go to the top of their group. |
| `packs` | A pack's `off` lists the options switched off, and `sub` the variant picked for an option that has variants (otherwise the first one is used). |
| `blocked` | The mods mod sync never offers (see [mods.md](mods.md#playing-with-other-mods)). |
| `share` | Default true. Lets players who join you download your gameplay mods. |
| `showEditedStats` | Default false. Shows public lobbies with host-enforced stat changes. |
| `listPublic` | Default true. Lists the player's own Public lobbies on other HellForge players' Galactic Map. |

Write the whole file at once, to a temporary file and then a rename, as HellForge does.

## Profiles and settings

`play` uses the launcher's settings unless an option replaces one, so a player gets the same session from either
launcher. It never changes `launcher-settings.json`.

To change them for good, use `settings`. It takes the same options as `play` (`--profile`, `--war`, `--local-playfab`,
`--multiplayer`, `--engine-log`, `--crash-dump`, `--sandbox`, `--steam-udp`, `--game-options`), and also
`--strangers yes|no|default`, `--live-changes` and `--forge-credits`. It saves the ones given, checks them as `play`
would, and prints every setting:

```json
{"profile": "default", "war": "live", "localPlayFab": true, "multiplayer": true, "engineLog": true, "crashDump": false,
 "gameOptions": "", "sandbox": true, "steamUdp": true, "steamBridge": true, "playWithStrangers": true,
 "playWithStrangersChosen": null, "liveChanges": false, "forgeCreditChance": 30, "file": "<installation>\\launcher-settings.json"}
```

`settings --live-changes yes|no` sets the experimental Change mods without restarting (off by default, read when a
session starts; the settings JSON says `liveChanges`).

`settings --steam-udp yes|no` saves co-op over Steam relays (on by default since the 0.4.0 beta; the settings JSON
says `steamUdp`, the file `coopRelays`).

- On Linux, `play` sets `HELLFORGE_STEAM_UDP=1` for the session when it is on. UDP goes to Valve's Steam relay servers
  only, and TCP and every other destination stay refused.
- Without `passt`'s `pasta` command, `python3` or Valve's relay list the session starts without it, and its lobby
  browser says why ([linux.md](linux.md#co-op-on-linux-opening-udp)).
- Turning it on from the in-game lobby browser needs HellForge's own launcher running (it carries the change out).
  Other launchers save `--steam-udp yes` and start again.
- Other systems ignore it for sessions.

`settings --strangers yes|no|default` also sets Play with strangers:

- `yes`/`no` store the explicit choice as `"strangers": "on"`/`"off"`. A legacy `playWithStrangers` true or false
  still counts.
- `default` removes it and lets the sandbox decide: on in a sandboxed session, off otherwise (see
  [Play with strangers](multiplayer-lobbies.md#play-with-strangers)).

It is saved in `mods\hellforge-mods.json`, not in `launcher-settings.json`, and `play` has no option for it. The printed
`playWithStrangers` is the effective bool for the next session's sandbox state; `playWithStrangersChosen` is `true`,
`false` or `null`. A running session's backend picks the effective value up within a few seconds.

`settings --forge-credits <0-100>|default` sets the share of the random loot caches at points of interest that hold Forge
Credits (HellForge's default is 30 %; the game's was 20 % for Super Credits). It is saved in `mods\hellforge-mods.json` as
`forgeCreditChance` and applies from the next session. Everyone in a co-op mission should use the same value: each player
rolls the loot from the mission's seed.

HellForge.exe reads the same file, so a choice made in your launcher is the one its Settings page shows. `profiles`
lists and creates profiles; `settings --profile <name>` selects one.

## Sessions

```text
HellForge.Cli.exe play [--profile <name>] [--war live | frozen | open] [--local-playfab yes | no] [--multiplayer yes | no]
                       [--engine-log yes | no] [--crash-dump yes | no] [--sandbox yes | no] [--steam-udp yes | no]
                       [--game-options "<launch options>"] [--full-ownership-check yes | no] [--wait yes] [--dry-run yes]
```

| Option | Default (the launcher's setting) | Meaning |
|---|---|---|
| `--profile` | the selected profile | which HellForge profile plays |
| `--war` | `live` | `live`: the current galactic war, read-only through the community mirror. `frozen`: the war HellForge ships. `open`: every planet open |
| `--local-playfab` | `yes` | the game's PlayFab calls go to the local backend |
| `--multiplayer` | `yes` | co-op with Steam friends |
| `--engine-log` | `yes` | the game's own log, copied into the session's logs |
| `--crash-dump` | `no` | a full memory dump if the game crashes (large), instead of the small dump every crash leaves |
| `--sandbox` | `yes` where the sandbox can run | the game at low integrity, with Steam through the Steam broker ([steam-bridge.md](steam-bridge.md)); on unless turned off, but only with an engine built with the Steam bridge (`settings` prints `steamBridge`) and only on Windows, so `play` runs unsandboxed elsewhere even when `settings` prints `"sandbox": true`. An installation with the older Windows Firewall rules gets the WFP block placed first (one Windows prompt) |
| `--steam-udp` | the launcher's setting, on by default | on Linux, opens UDP to Valve's Steam relay servers only so Steam networking can carry co-op, with TCP and every other destination refused. Needs `passt`'s `pasta` command and `python3`; without them the session starts without co-op. Defaults to the saved setting and applies to this session only; `settings --steam-udp` saves it ([linux.md](linux.md#co-op-on-linux-opening-udp)). Other systems ignore it for sessions |
| `--game-options` | the launcher's launch options | options for the game's command line, as Steam takes them; options a session sets itself are refused |
| `--full-ownership-check` | `no` | `yes` asks Steam to confirm ownership even when a check of this installation passed within the last 7 days for the same Steam account and game build (which a session otherwise reuses); for this session only, not a setting |
| `--dry-run yes` | | prints the session runner's command line and starts nothing |

Once the session runner has made its run folder, `play` prints
`{"event": "started", "runId": ..., "run": <run folder>, "pid": ..., ...}` and returns. The session goes on without it.

With `--wait yes`, `play` stays and prints one JSON line each time the session's status changes, then
`{"event": "ended", "status": ..., "result": ..., "logs": ...}`. It exits with 0 when the session ended normally. Two
other lines can come:

- `{"event": "mod-problems", "problems": [...]}` lists mods the session left out (a mod that did not compile, data that
  was not built...).
- `{"event": "steam-input", "note": ...}` comes from a sandboxed session when the controllers Steam Input handles
  change. `note` is what to tell the player (`SteamInputNote.Of` reads it from the status).

Closing `play` does not stop the session; `stop` does:

- `stop` asks the game to close by itself first, as its window's close button does. It ends the game after 30 seconds
  if it has not closed. The game saves its shader cache only when it closes by itself.
- A game without a window, or with one that does not respond, is ended at once, and so is one stopped with `--now yes`.
- The stop signal, `stop-observation.signal` in the run folder, holds `stop` or, for `--now yes`, `stop-now`.

| `status` | Meaning |
|---|---|
| `preparing` | building mods that ship sources, then checking the copied game (progress in `<run>\mod-builds.json`) |
| `awaiting_network_block_install` | Windows asks the player, once, to install the network block |
| `awaiting_windows_firewall_consent` | Windows asks for this session's firewall rules |
| `awaiting_steam_broker` | sandboxed: the game waits for HellForge's Steam broker to reach Steam (at most 60 s) |
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

### The lobby browser API

While a session runs with multiplayer on, its local backend lists HellForge lobbies and can hand the game an
invitation to one. The in-game lobby browser uses it, and a launcher of your own can too
(`src/HellForge.LocalBackend/Multiplayer/BrowseApi.cs`).

**Where.** The backend's address is in the session's `<run>\backend\ready.json`, under `"urls"`
(`http://127.0.0.1:<port>`). It listens on loopback only.

**Who may call it.** Programs on this PC only. Send no `Origin`, `Referer` or `Sec-Fetch-*` header, and keep `Host` as
`127.0.0.1`, `localhost` or `[::1]` with the port. Anything else gets 403 with `{"errorCode": "HELLFORGE_NOT_FROM_THIS_PC"}`,
so a web page in the player's browser cannot use it.

**`GET /hellforge/v1/lobbies`.** Every HellForge lobby the lobby directory lists, with no planet or difficulty filter,
plus Steam friends' lobbies that are not listed (Friends only). With Play with strangers off, strangers' lobbies are
still listed (never contacted), as unjoinable with `"fix": "strangers"`. A new search runs at most once every 2 s;
asking sooner returns the last list. 200 with:

```json
{"version": 1, "available": true, "searchedUtc": "2026-10-30T20:01:02.0000000Z",
 "lobbies": [{"lobbyId": "<GUID>", "host": "<SteamID64>", "hostName": "Helldiver", "friend": false,
              "source": "directory", "members": 1, "maxMembers": 4, "locked": false, "privacy": 0,
              "difficulty": 7, "planetId": 168, "race": 2, "inMission": true, "older": false, "players": ["Helldiver"],
              "joinable": false, "verdict": "unjoinable", "reason": "It needs mods installed by hand.",
              "downloadBytes": 0,
              "mods": [{"id": "author.mod", "name": "Mod", "version": "1.0", "tier": "shared", "class": "code",
                        "action": "manual", "why": "stranger code, not verified", "url": "https://..."}]}],
 "needed": [{"id": "author.mod", "name": "Mod", "lobbies": 1, "versions": ["1.0"], "url": "https://..."}]}
```

| Field | Meaning |
|---|---|
| `available` | False, with a `reason` and an empty list, when the session has no multiplayer. |
| `lobbies` | In this order: those that can be joined now (a friend's first), then ones a `fix` would let in, then full or locked ones, then the ones that can't be joined. Within each, the directory's order. |
| `source` | `directory` for a listed lobby, or `friend` for one heard only from a Steam friend. A friend's lobby has no mod list until a join; its `verdict` is `unknown`. |
| `verdict` | One of: `ready` (the same shared mods), `download` (the join downloads `downloadBytes`), `trust` (a mod waits for the player's trust in the MOD SYNC prompt), `unjoinable`, `blocked`, `full`, `locked`, `unknown`. |
| `joinable`, `reason` | `joinable` is whether a join can be tried now; `reason` says why not. |
| `fix` | When present, the player's setting that would let the lobby in. `strangers`: Play with strangers is off (the launcher's Settings > MULTIPLAYER, after its warning). `editedStats`: lobbies with host-enforced stat changes are hidden (the Mods page). A stranger's unchecked mods have no fix: trusting them takes a sandboxed session. This API does not change settings: point the player at them. |
| `privacy` | The game's setting (0 Public, 1 Friends only, 2 Invite only). |
| `difficulty`, `planetId`, `race`, `players` | From the lobby record the host's game uploads; null or empty until it has uploaded one. |
| `inMission` | Whether the host has a mission selected. |
| `older` | Whether the host runs HellForge 0.3, which this HellForge joins in that version's protocol. Its lobbies are joinable as any other's. |
| `mods` | The host's gameplay mods (at most 64). |
| `tier` (in `mods`) | `shared` (must match) or `per-player` (each player's own). |
| `action` (in `mods`) | For a shared mod, the action a join would take: `have`, `download`, `trust`, `manual`, `blocked`, `update`. |
| `why` (in `mods`) | When the mod keeps the lobby out: `share off`, `stranger code, not verified`, `stranger raw data, not verified`, `bundled differs`, `blocked`. |
| `url` (in `mods`) | Where to get it, when the author gave one. |
| `needed` | The mods that keep the most lobbies out, at most 5, most first. Each is one a listed lobby needs installed by hand (`"action": "manual"`: a stranger's unchecked mod, or a host who doesn't share). Each comes with how many lobbies need it, the versions they run, and the author's address when one gave it. It tells a player what to install to get into the most lobbies. |

**`POST /hellforge/v1/lobbies/<lobbyId>/join`.** Hands this session's game an invitation to a lobby from a list of the
last 2 minutes. The game shows it as a friend's invitation would, and joins only if the player accepts. A lobby with
other shared mods then brings HellForge's MOD SYNC prompt, as any join does. Nothing downloads or is trusted without
that prompt.

- 202 `{"version": 1, "lobbyId": ..., "state": "invited", "message": ...}`.
- 404 `HELLFORGE_LOBBY_UNKNOWN`: the lobby is not in a recent list. List again.
- 409 `HELLFORGE_LOBBY_NOT_JOINABLE`, with `message` saying why (full, locked, or its mods).
- 503 `HELLFORGE_MULTIPLAYER_OFF`: the session has no multiplayer.

**`POST /hellforge/v1/lobbies/quick`.** Quick Play: the same invitation, to the best joinable lobby of a list at most
2 s old. The best is one with a slot free, a Steam friend's first, then the least a join needs, then the one with the
most players. The least a join needs ranks the same mods first, then the smallest download, then a friend's lobby
whose mods are known only on joining, then mods to trust.

- 202 as a join's, with `hostName`.
- 404 `HELLFORGE_NO_JOINABLE_LOBBY`: nothing can be joined now.
- 503 `HELLFORGE_MULTIPLAYER_OFF`.

The routes take only these methods: anything else is 405 with `Allow`. Version 1 only grows: fields may be added,
none removed or changed in meaning. A change that breaks this goes to `/hellforge/v2`.

A browser drawn inside the game (a code mod's, as HellForge's in-game editor draws its own) gets the same list. It
joins through the SDK 16 calls `browse_state`, `browse_lobby` and `browse_ask` ([sdk-reference.md](sdk-reference.md)),
over the session's live-edit pipe, with no HTTP of its own. `browse_state`'s `terminal` count goes up each time the
ship's information terminal opens the browser. [lobby-browser.md](lobby-browser.md) is the guide to building one, and
`HF_BROWSE_CLAIM` makes HellForge's own step aside for it.

### Logs

Each session has its own folder, `logs\sessions\<runId>\`, and `logs\play-session.json` names the newest one (`"run"`).
Only the newest ten are kept, and of the older ones the five newest that ended in a crash. These are the files to show
a player:

| File | What it holds |
|---|---|
| `session.log` | the session runner's steps; it opens with HellForge's, the engine's, the mod SDK's and the game's versions, Windows' version (and Wine's), the GPUs and their drivers, the session's settings, and the mods with their versions and content hashes |
| `result.json` | the session's status and outcome; its fields are in the next table |
| `modloader.log` | the mod loader: which mods loaded, with each code mod's version and its DLL's SHA-256, and why one did not |
| `mod-status.tsv` | each code mod's status for the player (SDK 16 `set_status`), a line `<id>\t<text>` per mod; the launcher shows it on the Mods page |
| `modlist.txt` | the mods the session ran, in load order |
| `mod-<id>.log` | a code mod's own log; `mod-lua.<name>.log` is a Lua mod's |
| `cli-output.txt` | mod builds and staging |
| `engine-logs\` | the game's console log |

`result.json` holds:

| Field | What it holds |
|---|---|
| `report` | the opening facts of `session.log` as JSON |
| `ownershipCheck` | `full` when it asked Steam, `recent` when it reused a check that passed at `ownershipConfirmedUtc` |
| `shaderCache` | the game's shader cache at the start and end: `atStart` and `atEnd` (`files`, `bytes`, `newestUtc`), `written`, and `seededFromGame` when a new installation took the normal game's. While `atStart.files` is 0 and `firstBackendRequestSeconds` is not there yet, the game is most likely building it behind a black screen |
| `gameClose` | how a `stop` went: `outcome` (`ClosedByItself`, `TimedOut`, `NoWindow`, `Hung`, `AlreadyExited` or `EndedAtOnce`), `windows`, `closedByItself`, `seconds` |
| `prepare`, `connect` | how the two launch edits ended: `outcome`, `waitedMilliseconds`, and for `connect` `gameDllSeenMilliseconds` (-1 when `game.dll` never appeared) |
| `windowsCrash` | Windows' record of a game that crashed (`module`, `offset`, `code`, `modulePath`, `owner`: `windows`, `installation`, `other` or `unknown`), or `none` when Windows had none |
| `sessionEndHeard` | whether the mods answered the session's end before a stop ended the game |
| `stuckQuitting` | the game closed its window and was ended 30 s later |
| `gameHung` | its window stopped responding for 15 s |
| `gameAtStop` | `stop` on a game without a window or with a hung one |
| `jobActiveAtStop` | counts what was still in the game's job when the session ended it |
| `steamInput` | in a sandboxed session once the Steam broker has asked Steam: `controllers` (names), `types` (Steam's `ESteamInputType`) and `note` (what to tell the player, or null) |
| `gameExitCode`, `gameCrashed`, `loaderCrash` | recorded by a sandboxed session; `loaderCrash` is the loader's first crash line |

`stuckQuitting`, `gameHung` and `gameAtStop` each hold the game's threads by start address and state, and the Windows
Error Reporting or crash reporter processes holding it.

Anything that stops a session before its run folder exists goes to `logs\session-runner.log`. The game's crash reports
are in `userdata\`.

`logs` reads all of this for you. It lists the sessions newest first, each with:

- `id`, `folder`, `startedUtc`, `status`, `failure`, `gameExitCode` and `completedUtc` (a session still starting has no
  status yet)
- `crashed`: the game ended by itself with an error, the mod loader noted a crash, or it left a crash dump.

It also prints the paths of `session-runner.log` (when there is one) and the game's own log folder. `--run <id>` adds
that session's files with their sizes.

`logs --export <file.zip>` makes the zip the launcher's Save logs to Desktop makes, of the newest session or the one
`--run` names. The zip must not exist yet.

- Its `start` folder holds `session-runner.log` and, on Linux, the wrapper's logs. With no session yet, the zip holds
  only that folder and `run` is null.
- It also has `<run>/processes-now.txt`, the programs of the session.
- Text files and file names in it say `%USERPROFILE%` and `PC` instead of the player's user folder and computer name,
  whoever wrote them.
- Programs are left out, links in the run folder are skipped and never followed, and a crash dump goes in as it is.
- While a session runs, its logs are read with shared access, and a file it has locked is left out (`skipped` counts
  them).

`logs --crash-report <file.zip>` makes the crash report the launcher's Save crash report makes, of the newest session or
the one `--run` names. It is one zip for a player to attach to a GitHub issue. Nothing is uploaded; offer
`https://github.com/SkyeShade/HellForge/issues/new` (`issuePage`, no parameters) for them to attach it. It holds:

| Path | What it holds |
|---|---|
| `summary.txt` | the versions, system and settings, the game's exit code, the mod loader's crash note (its code, module and offset and thread), Windows' record of the crash when the session looked for one, the mods, the loader's events before the crash and the last 200 lines of its log |
| `logs\` | the export, without crash dumps |
| `dump\crash-<pid>.dmp` | only with `--dump yes` |

Its text is scrubbed further than the export, for a public issue. Steam ids, IP addresses other than loopback and the
other players HellForge's logs name are replaced with `steam-id-1`, `ip-1`, `player-1` and so on. A dump is the game's
memory and cannot be scrubbed: ask the player before passing `--dump yes`. The JSON says `files`, `skipped`, `dump`
(whether it went in), `dumpAvailable` and `issuePage`.

## Linux

On Linux, HellForge's game runs only inside its network namespace, which `app/linux/hellforge-linux.sh` sets up
([linux.md](linux.md)). Outside it the session runner stops before the game starts, as it does for HellForge.exe. The
script also bridges Steam and fetches the live war while the PC still has its network. Give it the program to run in
place of HellForge.exe, with its arguments:

```sh
PROTON=<proton> STEAM_COMPAT_DATA_PATH=<prefix> app/linux/hellforge-linux.sh "$INSTALL/app/cli/HellForge.Cli.exe" play --wait yes
```

When `PROTON` or `STEAM_COMPAT_CLIENT_INSTALL_PATH` is not set, the wrapper first uses the Proton and Steam folder saved
in the installation (`proton-selection.json`, `steam-selection.json`). After that it uses its own search
([linux.md](linux.md#sessions)). So a player's choice outside the usual places carries over.

When the script returns:

- For `play` that starts a session, the script returns when the session's programs have ended, even when `play` itself
  returned as soon as the session started.
- Every other command, including `play --dry-run yes`, returns as soon as Proton returns, with the command's exit code.

Where its output goes:

- The command's standard output is passed through unchanged, so `status` prints its JSON with nothing from the wrapper
  on that stream.
- The wrapper's own messages go to standard error, as do the command's own error messages.
- Only the native launcher sets `HELLFORGE_WRAPPER_LOG=1`, which sends the wrapper's output to
  `logs/linux-session-wrapper.log` instead. Do not set it in your own launcher.

`stop` and the other commands also run through Proton outside the namespace
(`"$PROTON" run "$INSTALL/app/cli/HellForge.Cli.exe" stop`). None of them goes online except `fetch-war`, which is
meant to.

## Related pages

- [mods.md](mods.md): the Mods page and playing with friends, as players see them.
- [engine.md](engine.md): what the session runner, the local backend and the mod loader are, and where their source is.
- [multiplayer.md](multiplayer.md): how co-op works.
- [The docs map](README.md): every document, by reader.
