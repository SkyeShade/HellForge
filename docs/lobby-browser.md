# Building a lobby browser

This page is for front-end developers who build their own lobby browser on the ship's information terminal (the one
across from the Armory). HellForge does the matchmaking: it finds the lobbies, works out what joining each one takes,
and joins. Your browser shows the list and sends the player's choices. Layout, look, input and sound are up to you.

HellForge's in-game editor has a placeholder browser (`native/editor/app_lobbies.cpp`). It is the reference
implementation for everything on this page, and its behaviour is the baseline yours should match.

There are two ways in:

| Where your browser runs | API | Reference |
|---|---|---|
| In the game, on the terminal (this page) | SDK 16 calls from a code mod: `browse_state`, `browse_lobby`, `browse_ask` | [sdk-reference.md](sdk-reference.md#sdk-16-the-lobby-browser-hfbrowsestate-check-hf-size-before-calling), `native/mod-sdk/hellforge_mod.h` |
| Outside the game (a launcher) | HTTP on loopback: `GET /hellforge/v1/lobbies`, `POST .../join`, `POST .../quick` | [external-launchers.md](external-launchers.md#the-lobby-browser-api) |

Both read the same list from the same backend. Only the in-game API can change the player's settings (Play with
strangers, edited stats), and only it knows about the terminal and its camera.

## How it fits together

```
 player presses E at the terminal
        │
        ▼
 mod loader (terminal.c)  ── counts the open: HfBrowseState.terminal += 1
        │                    glides the camera to the screen: HfBrowseState.camera
        ▼
 your mod  ── sees the count go up, draws the browser once the camera is HELD
        │      browse_ask(REFRESH / JOIN / QUICK / ALLOW_...)
        ▼
 mod loader ── live-edit pipe ──► session backend (BrowseApi.cs, LiveBrowse)
                                      │  searches the lobby directory and Steam friends,
                                      │  checks each lobby's mods against this machine's (ModSync.Check)
                                      ▼
 your mod  ◄── browse_state / browse_lobby: a new list version, or the outcome of a join
```

Every call is non-blocking. `browse_ask` queues a request and returns at once. The answer shows up later as a change
in `browse_state`, so you poll.

## Setting up

A code mod made from HellForge's template (`HellForge.Cli new-mod`, or the Mods page; see
[first-mod.md](modding/first-mod.md)). The manifest needs SDK 16:

```json
"code": { "dll": "my_browser.dll", "sdk": 16 }
```

Add `"coop": "per-player"` if it only draws the browser, as HellForge's editor does. It changes nothing other players'
games need, so it stays on the player's PC and never has to match a lobby's mods
([concepts.md](modding/concepts.md)).

At init, check that the loader has the calls, then claim the browser:

```c
#include <stddef.h>
#include "hellforge_mod.h"

static const HfApi* g_hf;
static HfMod* g_mod;

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    g_hf = hf;
    g_mod = mod;
    if (hf->size < offsetof(HfApi, browse_ask) + sizeof hf->browse_ask) return 0; /* an older HellForge: no browser */
    if (!hf->browse_ask(mod, HF_BROWSE_CLAIM, NULL)) hf->log(mod, "another mod draws the lobby browser");
    /* ...hook the swap chain's Present and the window procedure here (see "Drawing and input") */
    return 0;
}
```

**The claim.** `HF_BROWSE_CLAIM` tells HellForge that your mod draws the browser. While you hold it, HellForge's
editor never opens its placeholder (from the terminal or from Ctrl+L), and only your mod can move the terminal's camera
(`HF_BROWSE_SHOWN`, `HF_BROWSE_HIDDEN`). It returns 1 when your mod holds the browser and 0 when another mod claimed it
first. Code mods set up in order of their ids, and the first claim stays until that mod sends `HF_BROWSE_RELEASE`.
`HfBrowseState.owner` names the mod that holds it. The claim needs no backend and lasts for the whole game.

`modloader.log` records it: `lobby browser: <your id> draws it; HellForge's editor shows none of its own`.

## The life of one visit

1. **The player uses the terminal.** `HfBrowseState.terminal` goes up by one. Remember the count you first read and
   open your browser each time it changes. Don't open it on the first read, which is just the count so far.
2. **The camera glides in.** If `camera` is `HF_BROWSE_CAMERA_MOVING` when you see the new count, draw nothing yet,
   but already hold the game's input. The glide takes 1.1 s. It doesn't happen when Ship Station Hotkeys' Control Center
   key opens the terminal, or when the camera is off (`HF_BROWSE_CAMERA_NONE`): in those cases show the browser at once.
3. **The camera arrives.** Once `camera` is no longer `MOVING` (normally `HF_BROWSE_CAMERA_HELD`), draw the browser
   and send `browse_ask(mod, HF_BROWSE_SHOWN, NULL)`. The camera stays on the screen while you show. If nobody sends
   `SHOWN` within 4 s of arriving, the camera glides back by itself.
4. **Ask for a list** with `HF_BROWSE_REFRESH` as soon as the browser opens.
5. **The player browses, filters and joins** (see below).
6. **The browser closes** (Esc, or a join's invitation arrived). Stop drawing, give the input back, and send
   `HF_BROWSE_HIDDEN`. The camera glides back to the player in 0.8 s (`HF_BROWSE_CAMERA_RETURNING`, then `IDLE`).

**Poll `browse_state` at least every 3 seconds the whole game, not only while the browser shows.** While a mod has
read it in the last 3 s, the terminal opens no game screen of its own. Otherwise the game's bare debug lobby list
(menu presenter 41) opens underneath. The placeholder reads it every 15 frames, and every frame while the camera moves.

**Ignore the key that opened the browser.** The E press (or click) that used the terminal reaches your browser too.
The placeholder ignores keys and clicks for its first 0.5 s (`LOBBY_ARM`), so that press can't join a lobby.

## Reading the list

`browse_state` fills an `HfBrowseState`:

| Field | Meaning |
|---|---|
| `state` | `HF_BROWSE_...`, below |
| `version` | the list's version; a new one comes with every search. Re-read the lobbies when it changes |
| `lobbies` | how many lobbies `browse_lobby` has (at most 128) |
| `message` | what the backend says (UTF-8), or "" |
| `terminal` | how many times the terminal opened the browser this game |
| `camera` | `HF_BROWSE_CAMERA_NONE`, `IDLE`, `MOVING`, `HELD`, `RETURNING` |
| `owner` | the id of the mod that claimed the browser, or "" |

It returns 0 when no backend is connected. The struct is still filled: state `NONE`, plus the terminal count.

| `state` | What it means | What to show |
|---|---|---|
| `HF_BROWSE_NONE` | nothing asked yet, or the backend isn't connected yet | a connecting state ("Connecting to the ship's network...") |
| `HF_BROWSE_SEARCHING` | a search is running; the lobbies are still the last list's | the last list, with a searching indicator |
| `HF_BROWSE_READY` | the list is in | the list. `message` may say "(N more lobbies are not listed.)" |
| `HF_BROWSE_UNAVAILABLE` | this session has no multiplayer | `message`, with no list |
| `HF_BROWSE_INVITED` | your join (or Quick Play) worked: the game has the lobby's invitation | close the browser; the player accepts the invitation in game. After Quick Play, `message` names the host |
| `HF_BROWSE_FAILED` | the search or the join failed | `message` (why), and keep the list |

`INVITED` and `FAILED` keep the version and the lobbies they arrived with. They also stay in `state` until the next
search, so react when the state *changes* while you have a join pending. Don't react to the level: a browser opened
later must not close because an old `INVITED` is still there. The placeholder keeps the id it asked to join in
`joining` and checks the answer only while that is set.

`browse_lobby(mod, i, &lobby)` fills the i-th `HfBrowseLobby`, for i from 0 to `lobbies - 1`. Set `lobby.size =
sizeof lobby` first. Copy the whole list whenever `version` changes; the backend never changes a version's lobbies.

| Field | Meaning |
|---|---|
| `lobby_id` | what `HF_BROWSE_JOIN` takes (at most 64 printable ASCII characters) |
| `host` | the host's name (UTF-8), or "" |
| `players` | the players' names, comma separated (UTF-8), or "" until the host's game uploads its record |
| `members`, `max_members` | how many are in it, of at most 4 (a squad) |
| `difficulty` | 1 and up, or -1 not known |
| `planet` | the planet's index, or -1 (on the ship, or not said). The SDK has no planet names yet |
| `flags` | `HF_LOBBY_FLAG_FRIEND` (a Steam friend hosts), `HF_LOBBY_FLAG_MISSION` (on a mission now), `HF_LOBBY_FLAG_JOINABLE` (a join can be asked for now) |
| `verdict` | what joining takes, below |
| `reason` | why it can't be joined (UTF-8), or "" |
| `download_bytes` | what a join downloads first (`HF_LOBBY_DOWNLOAD`) |
| `mods` | how many shared mods the host runs |
| `mod_list` | the host's shared mods that a join would change, each with what happens to it: `"Tanks 2.0: download; Guns 1.1: trust"` (UTF-8), or "" |
| `fix` | the player's setting that would let it in, below |

Text fields are cut to fit on a whole UTF-8 character. Use `HF_LOBBY_FLAG_JOINABLE` to enable a join button, and
don't work joinability out from `verdict` yourself.

| `verdict` | Joinable | Meaning | The placeholder says |
|---|---|---|---|
| `HF_LOBBY_READY` | yes | the same shared mods; joins as it is | Ready to join |
| `HF_LOBBY_DOWNLOAD` | yes | joins after downloading the host's shared mods (`download_bytes`), through the MOD SYNC prompt | Downloads 12.4 MB |
| `HF_LOBBY_TRUST` | yes | a mod needs the player's trust first, in the MOD SYNC prompt | Needs your trust |
| `HF_LOBBY_UNKNOWN` | yes | a friend's lobby heard through Steam presence; its mods are known only when joining | A friend's lobby: mods known on joining |
| `HF_LOBBY_UNJOINABLE` | no | this machine's mods can't join it; `reason` says why | Can't join: `reason` |
| `HF_LOBBY_BLOCKED` | no | the host runs a mod on the player's blocklist | `reason`, or "Runs a mod you blocked" |
| `HF_LOBBY_FULL` | no | 4 of 4 | Full |
| `HF_LOBBY_LOCKED` | no | the host locked it | Locked |

The placeholder colours these green (ready), amber (download, trust), red (can't join, blocked) and dim (full, locked,
unknown).

| `fix` | The setting | What your browser does |
|---|---|---|
| `HF_LOBBY_FIX_NONE` | none | nothing |
| `HF_LOBBY_FIX_STRANGERS` | Play with strangers is off | offer to turn it on with `HF_BROWSE_ALLOW_STRANGERS`, **only after showing the warning below and getting a clear yes** |
| `HF_LOBBY_FIX_EDITED_STATS` | lobbies whose host shares edited stats are hidden | offer `HF_BROWSE_SHOW_EDITED_STATS`. Tell the player they play with the host's stats there |
| `HF_LOBBY_FIX_SANDBOX` | a stranger's unchecked mods, which only a sandboxed session may download | no ask can change it. Tell the player to turn on Settings > Sandbox in HellForge's launcher and start a new session |

A lobby with a `fix` still has its `verdict` and `reason`. The fix is a way in, offered next to them.

## Actions

`browse_ask(mod, ask, lobby_id)` returns 1 when the ask was sent and 0 when it wasn't (no backend connected, or a
malformed ask). On 0, tell the player ("The session's backend is not connected") rather than waiting.

| `ask` | `lobby_id` | What happens |
|---|---|---|
| `HF_BROWSE_REFRESH` | NULL | a new search. The backend searches at most once every 2 s, whoever asks; an earlier ask gets the last list again. `SEARCHING` comes first when a list is already showing |
| `HF_BROWSE_JOIN` | the lobby's `lobby_id` | the game gets that lobby's invitation. The lobby must be from a list of the last 2 minutes; if not, the answer is `FAILED`, and you should search again |
| `HF_BROWSE_QUICK` | NULL | Quick Play: a fresh search, then a join of the best joinable lobby. A friend's goes first, then the fewest changes (same mods, then the smallest download, then a friend's unknown, then trust), then the fullest. `INVITED` with the host's name, or `FAILED` when nothing can be joined |
| `HF_BROWSE_ALLOW_STRANGERS` | NULL | turns Play with strangers on and saves it, then searches again (about 2 s later, when the setting has taken) |
| `HF_BROWSE_SHOW_EDITED_STATS` | NULL | shows lobbies whose host shares edited stats, and searches again |
| `HF_BROWSE_SHOWN` | NULL | your browser shows: the terminal's camera stays on the screen. This machine only |
| `HF_BROWSE_HIDDEN` | NULL | your browser went: the camera glides back. This machine only |
| `HF_BROWSE_CLAIM` | NULL | your mod draws the browser (see [Setting up](#setting-up)) |
| `HF_BROWSE_RELEASE` | NULL | gives it back to HellForge's editor |

**A join is an invitation, not a join.** The game shows the invitation the way it shows a friend's, and joins only
when the player accepts it. If the host runs other shared mods, HellForge's MOD SYNC prompt comes next, and nothing
downloads or is trusted without it. So close your browser on `INVITED` and tell the player to accept the invitation.
Don't promise they're in.

**Refreshing.** The placeholder asks for a new list when it opens, on REFRESH (R), and every 10 s while it shows. It
skips the 10 s refresh while a search runs or a join is pending, and shows a searching indicator for at least 0.8 s
after an ask, because a list from the last 2 s comes back at once and a REFRESH that changes nothing looks broken.

## Rules your browser must keep

These are part of the contract. HellForge's safety model depends on them.

1. **Warn before Play with strangers.** Before `HF_BROWSE_ALLOW_STRANGERS`, show this warning (the launcher's) and wait
   for an explicit yes. Esc and any other key must cancel, and the warning must hold other keys meanwhile:

   > Strangers' games send yours data you cannot check: a modified game can lie about its mods, and a flaw in the
   > game's network code could let it run code on your PC. Turn this on only if you accept that risk. You can turn it
   > off again in the launcher's Settings > MULTIPLAYER.

   The backend logs that the player turned it on after the warning.
2. **Show why.** Show `reason` for a lobby that can't be joined, and `mod_list` for one that downloads or needs trust.
   Players need to see what keeps them out.
3. **Trust the flags.** Enable joining from `HF_LOBBY_FLAG_JOINABLE` only.
4. **Close on `INVITED`**, so the player can accept the invitation in game.
5. **Send `HF_BROWSE_HIDDEN` whenever your browser closes**, or the camera stays on the screen until its 4 s timeout.
6. **Hold the game's input while you show** (including during the glide), and give it back when you close.
7. **Poll `browse_state` at least every 3 s** for the whole game.

## Drawing and input

The SDK has no drawing calls. Your mod draws over the game itself, the way HellForge's editor does
(`native/editor/editor.cpp`, a working example to copy from):

- **Drawing.** The game renders with D3D12. The editor finds `IDXGISwapChain::Present`, `Present1` and `ResizeBuffers`
  through a throwaway device and swap chain, hooks them with `hf->hook`, and records Dear ImGui into the back buffer
  on the swap chain's own command queue just before the game presents. Launched with `--use-d3d11`, the game renders
  with D3D11, and the editor draws with ImGui's DX11 backend instead. Hooks made through `hf->hook` chain with
  HellForge's and other mods' hooks on the same function, so your Present hook and the editor's both run.
- **Input.** The editor subclasses the game window's procedure (`SetWindowLongPtrW(GWLP_WNDPROC)`, calling the previous
  one with `CallWindowProcW`). While it shows, it gives keyboard, mouse and raw input to its UI instead of the game, and
  ignores the game's `ClipCursor` and `SetCursorPos`. Chain the same way, so the editor's F8 window keeps working
  alongside your browser.
- **Threads.** Present runs on the game's render thread and the window procedure on the window thread. The editor
  runs its UI under one lock that both take. The SDK browse calls are safe from any thread.
- **The look.** The placeholder fills the screen in the terminal's own CONTROL CENTER style: a blue blueprint field
  with a grid and scanlines, filters on the left, lobbies in the middle, the selected lobby's details on the right, and
  a power-on and power-off animation (0.55 s and 0.3 s). Its colours are the `TERM_` constants in `app_lobbies.cpp`.
  None of this is required.

## A skeleton

The state handling of a browser, without drawing. Call `BrowserTick` once a frame from your Present hook, and the
action functions from your UI.

```c
#include <string.h>
#include <windows.h>
#include "hellforge_mod.h"

#define REFRESH_MS 10000

static struct
{
    int open;                   /* drawn now */
    int waitingForCamera;       /* the camera glides to the screen: hold input, draw nothing */
    int terminalRead;
    uint32_t terminal;          /* browse_state's terminal count last read */
    HfBrowseState state;        /* as last read */
    HfBrowseLobby lobbies[128];
    uint32_t count;
    char joining[65];           /* the lobby a join was asked for, until INVITED or FAILED ("quick" for Quick Play) */
    ULONGLONG askedAt;
} b;

static void Refresh(void)
{
    b.askedAt = GetTickCount64();
    if (!g_hf->browse_ask(g_mod, HF_BROWSE_REFRESH, NULL)) { /* show: the session's backend is not connected */ }
}

static void Open(void)
{
    b.open = 1;
    b.joining[0] = 0;
    /* hold the game's input here; ignore keys and clicks for the next 0.5 s */
    Refresh();
}

static void Close(void)
{
    b.open = 0;
    b.waitingForCamera = 0;
    b.joining[0] = 0;
    /* give the game its input back */
    g_hf->browse_ask(g_mod, HF_BROWSE_HIDDEN, NULL);
}

void BrowserTick(void)
{
    HfBrowseState now = {sizeof now};
    g_hf->browse_state(g_mod, &now); /* every frame is fine: it only copies */

    if (!b.terminalRead) { b.terminalRead = 1; b.terminal = now.terminal; }
    else if (now.terminal != b.terminal)
    {
        b.terminal = now.terminal;
        Open();
        b.waitingForCamera = now.camera == HF_BROWSE_CAMERA_MOVING;
    }
    if (b.waitingForCamera && now.camera != HF_BROWSE_CAMERA_MOVING)
    {
        b.waitingForCamera = 0; /* on the screen (or the glide gave up): power the browser on */
        g_hf->browse_ask(g_mod, HF_BROWSE_SHOWN, NULL);
    }

    int turned = now.version != b.state.version || now.state != b.state.state;
    if (now.version != b.state.version)
    {
        b.count = 0;
        for (uint32_t i = 0; i < now.lobbies && i < 128; i++)
        {
            memset(&b.lobbies[i], 0, sizeof b.lobbies[i]);
            b.lobbies[i].size = sizeof b.lobbies[i];
            if (!g_hf->browse_lobby(g_mod, i, &b.lobbies[i])) break;
            b.count++;
        }
    }
    if (turned && b.joining[0])
    {
        if (now.state == HF_BROWSE_INVITED) Close(); /* show now.message: accept the invitation in game */
        else if (now.state == HF_BROWSE_FAILED) b.joining[0] = 0; /* show now.message */
    }
    b.state = now;

    if (b.open && !b.joining[0] && now.state != HF_BROWSE_SEARCHING && GetTickCount64() - b.askedAt >= REFRESH_MS) Refresh();
}

void Join(const HfBrowseLobby* lobby)
{
    if (!(lobby->flags & HF_LOBBY_FLAG_JOINABLE) || b.joining[0]) return;
    if (g_hf->browse_ask(g_mod, HF_BROWSE_JOIN, lobby->lobby_id)) strcpy(b.joining, lobby->lobby_id);
}

void QuickPlay(void)
{
    if (!b.joining[0] && g_hf->browse_ask(g_mod, HF_BROWSE_QUICK, NULL)) strcpy(b.joining, "quick");
}

/* After the player said yes to the warning in "Rules your browser must keep". */
void AllowStrangers(void) { g_hf->browse_ask(g_mod, HF_BROWSE_ALLOW_STRANGERS, NULL); b.askedAt = GetTickCount64(); }
```

Filters are yours. The placeholder has joinable only (J), friends' only (F) and a lowest difficulty (D, cycling any,
5 to 10), applied on this machine to the list it has. The backend always lists every lobby.

## Testing

- **Without the game.** Point your UI at a fake `HfApi` whose `browse_state`, `browse_lobby` and `browse_ask` return
  lobbies you made up, as the editor's test does (`native/editor/test/overlay_test.cpp`, `BrowseState`, `BrowseLobby`,
  `BrowseAsk`). That covers every verdict, fix and state, plus the terminal count and the camera, with no other
  players.
- **In game.** The list has only other players' lobbies, so you need a second PC hosting (a friend's works). For a
  stranger's lobby between two friends' PCs, start the session with `HELLFORGE_TEST_STRANGERS=1`, which treats
  friends as strangers. `HELLFORGE_TERMINAL=news` gives the terminal back to the game.
- **Logs** (in the session's log folder, [external-launchers.md](external-launchers.md#logs)):
  - `modloader.log`: the claim, each list (`lobby browser 7: ready, 3 lobbies`) and each ask
  - `backend\multiplayer.log`: each search's counts and one line for every lobby it can't join, with why
    (`Directory: browse: cannot join <host>: <why>: <mods>`)
  - `mod-<your id>.log`: your mod's own `hf->log` lines

## What the in-game API doesn't have yet

The HTTP API has a few things `HfBrowseLobby` doesn't. Structs only grow, so any of these can be added without
breaking a browser built today. Ask if you need one.

- the faction (`race`) and privacy (`privacy`)
- whether the host runs HellForge 0.3 (`older`)
- the host's mods as a list with each one's tier, action, why and download address (`HfBrowseLobby.mod_list` is one
  string)
- planet names (`planet` is the game's index)
