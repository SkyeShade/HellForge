# Steam Cloud stays in the profile

<!-- diataxis: explanation -->

For people working on HellForge: which files the game keeps in Steam Cloud and how it reaches them. It also covers how
HellForge's `steam_api64.dll` stand-in keeps them in each HellForge profile so they never mix with the normal game's.

## What the game keeps in Steam Cloud

The game's user data folder from `settings.ini` is redirected into the installation each session. Apart from that, the
game keeps two files in Steam Cloud:

- `testament_new.sav`: about 6 KB, binary. "testament" is the engine project's name. It holds what the players saw as
  equipped armor, title and player card.
- `input_settings.config`: control bindings, as text.

Steam stores them in `Steam/userdata/<account id>/553850/remote/` and uploads them to Valve. That folder belongs to the
Steam account, not to an installation. So until 2026-10-14 every HellForge session read and wrote the same save as the
normal game, and Steam synced the save HellForge wrote to the account's cloud. This was seen: `testament_new.sav` was
written at 22:07 during a HellForge session, and its `remotetime` in `remotecache.vdf` equalled its local time.

## How the game reaches it

The unpacked executable (`exe.img`, build 25480438) gets the interface through Steam's inline accessor:
`SteamInternal_ContextInit` on the context at `0x190d908`, whose init function (`0x758c00`) calls
`SteamInternal_FindOrCreateUserInterface(SteamAPI_GetHSteamUser(), "STEAMREMOTESTORAGE_INTERFACE_VERSION016")`.

All 22 references to that context call one of eight methods. The slot is the method's index in `isteamremotestorage.h`
(v016, 59 methods; the order was checked against Steamworks.NET's generated `isteamremotestorage.cs`):

| Slot | Method | Call sites |
|---|---|---|
| 0 | FileWrite | 3 |
| 1 | FileRead | 3 |
| 5 | FileForget | 2 |
| 6 | FileDelete | 2 |
| 13 | FileExists | 3 |
| 15 | GetFileSize | 2 |
| 21 | IsCloudEnabledForAccount | 4 |
| 22 | IsCloudEnabledForApp | 2 |

The game makes no asynchronous calls and uses no streams or callbacks. The file name `testament_new.sav` is in
`game.dll`. With both `IsCloudEnabled` checks true, the engine builds its Steam cloud save backend (`0x750bdb`); with
either false it skips it. That fallback has not been studied, so the stand-in reports cloud enabled and stores the files.

## The stand-in

The stand-in is `native/steam-cloud/steam_cloud.c`. `scripts/Build-Engine.ps1` builds it into
`artifacts/sdk-shims/out/steam_api64.dll`, which ships in `app/sdk-shims`.

Its export table repeats all 1,065 names and ordinals of the game's `steam_api64.dll` (pinned SHA-256 `1DB3FD41...`).
1,063 of them are loader forwarders to `steam_api64_hellforge.<name>`, so sign-in, ownership and every other interface
run Valve's own code. Only two exports are its own: `SteamInternal_FindOrCreateUserInterface` (for the remote storage
version; other versions are passed to the original) and `SteamAPI_SteamRemoteStorage_v016`. They return a storage object
whose methods keep plain-named files in `HELLFORGE_CLOUD_DIR`. Writes go to a temporary file first, and names with path
characters are refused. Unused methods return zero.

Without `HELLFORGE_CLOUD_DIR` the stand-in reports cloud disabled and stores nothing. It never calls the real storage.
Calls are logged to `HELLFORGE_CLOUD_LOG`.

The same DLL also carries the network gate's census (`native/steam-cloud/net_gate.c`), which logs every connection, name
lookup and program the copy attempts to `network-census.jsonl` in the session's folder.

`HellForge.SdkShimGen forward` writes the forwarding `.def`, and `verify-forward` checks the built DLL. `smoke-cloud`
loads the stand-in next to a copy of the original and exercises the storage methods (Steam is not started).

## Sessions

The session runner (`src/HellForge.Session/PlaySession.cs`) swaps the stand-in in for every session, right after the
ownership check:

1. The verified original is saved to the stand-in originals folder (`temp/sdk-shim-originals`) and placed next to the
   stand-in as `bin/steam_api64_hellforge.dll`.
2. The stand-in becomes `bin/steam_api64.dll`.
3. The game gets `HELLFORGE_CLOUD_DIR=<profile user data>/cloud` and `HELLFORGE_CLOUD_LOG=<run>/steam-cloud.log`.

The second ownership check uses the original. After the game exits, the original goes back and
`steam_api64_hellforge.dll` is removed. A session that was cut short is repaired at the start of the next one. A session
refuses to start without the stand-in.

A new profile's cloud folder gets a copy of the signed-in account's `input_settings.config`, read from the Steam folder
(the account comes from `HKCU\Software\Valve\Steam\ActiveProcess\ActiveUser`), so controls carry over. The save starts
empty and stays the profile's own.

For the rest of the engine, see [engine.md](engine.md); for every other page, the [docs map](README.md).
