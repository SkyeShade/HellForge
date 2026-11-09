# The HellForge war

The HellForge war is one galactic war shared by every HellForge player. A war server runs it: planets, liberation,
major orders and dispatches. Each player's backend follows that server, and each mission played counts towards it. It
is the default war from this version on (Settings > GALACTIC WAR > HellForge war).

The war server is a service of its own, not part of the website. HellForge's is `https://war.hf-mod.com`.

## What the backend does

Before the game starts, and every minute while it runs, the backend fetches the server's snapshot. After a failed
fetch it tries again 30 seconds later. A snapshot is used only when it is schema 1 for war 801, has a war time above 0
and planets, and has every list in it. Anything else keeps the war served before. The snapshot is saved in the
installation's `hellforge-war` folder, one file per server, with the time it was fetched, so switching servers never
serves another server's war. Without the server the saved copy is served, its war time moved on by the time since it
was fetched. With neither, HellForge's frozen war is served.

From the snapshot the game gets the war info, the war status, the galactic war effects, the major orders
(`/v2/Assignment/War/801`) and the dispatches (`/NewsFeed/801`, from the game's `fromTimeStamp`, 16 at most). Each is
served exactly as the server wrote it, because the game's parsers want every key and want integers to stay integers.
The war clock follows the snapshot's `status.time`.

A war script ([war-script.md](war-script.md)) still works on top: its major orders and dispatches come after the
server's, and its effects and status additions are merged in.

The session's `backend/hellforge-war.log` says what was fetched and what was reported.

## Mission reports

The backend tells the server about each mission from the game's own requests:

| The game sends | The backend sends |
|---|---|
| `Operation/Create` | nothing yet: it notes each operation's planet, difficulty (11 for HellForge's level 11) and enemy |
| `Operation/Mission/Start` | `POST /v1/mission/start` `{installId, planetIndex, difficulty, race}`, which answers a ticket |
| while the mission runs | `POST /v1/mission/heartbeat` `{ticket}` every 2 minutes, which the server uses to count players per planet |
| `Operation/Mission/end` | `POST /v1/mission/end` `{ticket, success}`, which answers `{impact, capped}` |
| `Operation/Abandon` | nothing; the heartbeats stop and the server lets the ticket lapse |

Each report goes out in the background with a 5 second timeout. The game's request is answered as before, whether the
server answers, refuses or is down; a report that fails is logged and dropped. The impact the server answers is logged
but not shown in game yet: the game's Mission/end reply wants an integer there, and what its end screen does with it
has not been tested.

`installId` is a random id made the first time it is needed and kept in `install-id.txt` in the HellForge folder. It is
not derived from the Steam account or the PC; it lets the server count one installation's missions.

## Choosing the server

In order, the first that is set wins:

1. `--war-server <url>` on `HellForge.Cli play` or the backend, for one session;
2. the environment variable `HELLFORGE_WAR_SERVER`;
3. the launcher's War server field (Settings > GALACTIC WAR, shown with the HellForge war), saved as `warServer` in
   `launcher-settings.json`, or `HellForge.Cli settings --war-server <url>` (`default` goes back to HellForge's);
4. `https://war.hf-mod.com`.

An address must be https, or http to this PC (`localhost`, `127.0.0.1`), and has no query. Trailing slashes are dropped.

To try a war server running on this PC: Settings > GALACTIC WAR > HellForge war, War server `http://localhost:5093`,
then PLAY. Or from a terminal: `HellForge.Cli.exe play --war hellforge --war-server http://localhost:5093 --wait yes`.

## Linux

A Linux session runs in a network namespace with no way out, so the wrapper fetches the HellForge war before entering it
(`fetch-hellforge-war`, as it fetches the live war), and the session serves that copy without refreshing it. Missions
are not reported from inside the namespace. They could be later: the backend would write each report to a file in the
installation, and the launcher, which runs outside the namespace, would send them on and pass the tickets back.

## The server's contract

| Request | Answer |
|---|---|
| `GET /v1/snapshot` | `{schema: 1, warId: 801, sequence, generatedUtc, warInfo, status, galacticWarEffects, assignments, news}`: the game's own bodies for war info, status, effects, v2 assignments and dispatches (`{id, published, type, tagIds, message}`) |
| `POST /v1/mission/start` `{installId, planetIndex, difficulty 1..11, race}` | `{ticket}` |
| `POST /v1/mission/heartbeat` `{ticket}` | 204 |
| `POST /v1/mission/end` `{ticket, success}` | `{impact, capped}` |

For the backend alone: `--war hellforge --war-cache <file> [--war-server <url>]`, and
`HellForge.LocalBackend fetch-hellforge-war <installation> [<server>]` to fetch it once.
