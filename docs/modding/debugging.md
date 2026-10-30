# Debugging

This page is for mod authors whose mod does not do what they meant. It covers the logs, the reasons HellForge gives
for refusing something, crashes, and testing with friends.

## The logs

Every session has a folder, `<installation>/logs/sessions/<session>/`. `logs/play-session.json` names the newest one.
The newest ten are kept, and of the older ones the five newest that ended in a crash. The launcher's CONSOLE page has an **Open session logs** button for the current one.

| File | Has |
|---|---|
| `session.log` | the session's steps: building and staging mods, checks, starting the game. Look here first when a session fails early |
| `mod-<your id>.log` | everything your mod logs (`hf->log`), and what the SDK did for it or refused, with your source line |
| `modloader.log` | the loader: which mods loaded and in what order (each code mod's version and its DLL's SHA-256, and why a DLL did not load), events, every registry step, conflicts between mods, crash notes |
| `modlist.txt` | multiplayer sessions: the gameplay mods friends compare (id, name, version, hash), by id |
| `cli-output.txt` | building source mods and data specs, staging |
| `result.json` | the session's outcome: `mods.loaded` lists the mods in load order, `mods.problems` the mods left out and why |
| `arsenal.tsv` | every armory item: loadout id, type, entity, name (for choosing a weapon's base); written when a mod adds a weapon or changes component stats |
| `stats-values.tsv` | every stats field's raw value from before the stats pass, by its place (kind, component, resource, record, offset) |
| `hooks.tsv` | every hooked function, with its hooks in the order they run (owner, priority) |
| `mod-status.tsv` | each code mod's status (`set_status`, SDK 16) as the Mods page shows it: id, tab, text |
| `engine-logs/` | the game's own logs, when the launcher's **Engine log** setting is on (`play --engine-log yes`) |

Each log's first line gives the local date and time it was opened and the offset from UTC; the lines after it give a
time of day only, in UTC (`session.log`: local time).

In code, `hf->last_log(mod)` returns your mod's last log line, so it can show why a call that returned 0 was refused.

## Before the game starts

- If the mod does not compile, it stays off for that session (the session still runs), and the Mods page shows the
  compiler's message with the file and line on its row. **Build now** in its ⋮ menu, or `HellForge.Cli build-mod --mod
  <folder>`, compiles it at once.
- If the mod is not listed, or is listed under the mod notices, the Mods page says why: a manifest error, an id another
  mod has (two mods with one id load neither), an SDK newer than this HellForge's, or a data spec naming a resource the
  game does not have.

## Refusals

When the SDK cannot check something, it refuses it and says why in your log. The common reasons:

| The reason | Means |
|---|---|
| a field of another layout | the mod was built against another SDK's headers: rebuild it with this HellForge's |
| a value of another type, or out of range | `HF_U32(-1)`, or a float in an integer field: use the field's own type |
| a shared record | that record belongs to several entities: give yours its own (a registered copy, `own_all`), or use `HF_LIVE_SHARED` or a stats mod knowingly |
| not loaded yet | the data or table the call needs has not loaded: call it from the right event ([concepts](concepts.md#when-your-code-runs)) |
| no room | a limit is reached ([limits](concepts.md#limits)) |
| not as reviewed | the game code a feature needs does not look as HellForge knows it, so the feature stays off |

Two mods changing the same field is not a refusal. SDK edits that collide are reported in both mods' logs and
`modloader.log`, and stats files in the later mod's log and on the Mods page ([which one
stays](concepts.md#load-order-and-conflicts)).

## Crashes

The loader does not catch a mod's crash, so the game ends. Then:

1. Read the crash note in `modloader.log`. It says where the crash happened (`crash: c0000005 on thread ... at
   game.dll+0x...`, or `?+0x...` outside the game), the address it read, the registers and the game addresses on the
   stack. The next line, `crash: by module: at my_mod.dll+0x...`, names the module, your DLL among them. A fault in your
   DLL is your code, and a fault in game.dll right after your mod changed something often is too. Notes cover access
   violations (`c0000005`), illegal instructions (`c000001d`), heap corruption (`c0000374`), stack overflows
   (`c00000fd`) and integer divide by zero (`c0000094`), the first four of each.
2. Open the crash dump in WinDbg or Visual Studio. The first noted crash of a session leaves one beside the session's
   logs (`crash-<pid>.dmp`): a small dump, with every thread's stack and registers and the memory they point to. The
   launcher's **Crash dump** setting (or `HellForge.Cli play --crash-dump yes`) makes it a full memory dump instead.
3. Narrow it down: run your mod alone, then with the others, and turn off one change at a time.

HELLDIVERS 2 also crashes as it quits (`helldivers2.exe+0x626296` at the end of a session). That crash is the game's
own and has nothing to do with your mod.

## Testing with friends

- Friends need the same gameplay mods, with the same versions and files: a different source file is a different mod.
  The Mods page shows the modlist code to compare, and a refused join says which mods differ, on both sides.
- Every machine writes its own logs, so ask your friend for theirs. `backend/multiplayer.log` (multiplayer sessions)
  follows the lobby and the co-op connection, and `coop-diagnostics.log` records what this machine knows of mod-added
  content.
- Test as host and as joiner, because some actions and game decisions are the host's alone
  ([co-op](concepts.md#co-op)).

Next: [Tools and sharing](tools.md), for IDEs, terminal builds and releasing a mod.
