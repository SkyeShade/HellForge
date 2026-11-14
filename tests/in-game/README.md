# In-game tests

Checks that need the real game. The development harness runs them unattended: CowboyBingus's
BingusDevelopmentHarness, with a `modtest` command that is not in its published repository yet. It:

1. copies the test mods into a HellForge installation with every other mod off
2. plays a session to the ship and into a solo mission and back
3. checks the logs the session wrote against an expectations file
4. puts the installation back as it was

Without the harness, the same test runs by hand: add `sdk-probe` to a test installation, and play to the ship and into
a mission and back. Then compare the probe's log (`mod-hellforge.sdk-probe.log` in the session's folder) with
`sdk16.json`.

| File | What |
| --- | --- |
| `sdk16.json` | SDK 16's names in the game: hooks by name (`HF_HOOK`), typed events (`hf_on_...`), game functions (`hf_fn_...`) and game globals with their fields (`hf_global_...`) |
| `sdk-probe/` | the test mod `sdk16.json` runs (`hellforge.sdk-probe`), never shipped: one `PROBE <check> ok` or `PROBE <check> FAIL <why>` line per check in its log |
| `rpc17.json` | SDK 17's RPCs in the game: `rpc_listen` on every RPC on the ship and in a mission, their arguments, the readers, and `rpc_send`'s refusals in solo |
| `rpc-probe/` | the test mod `rpc17.json` runs (`hellforge.rpc-probe`), never shipped; it sends nothing outside solo |
| `stratagem-cap.json` | room for 1,024 new stratagems: 300 added (types 150 to 449), the co-op type field at 16 bits, type 449 in the loadout before the mission and called in by its code (needs the harness's `call` step) |
| `stratagem-cap-probe/` | the test mod `stratagem-cap.json` runs (`hellforge.stratagem-cap-probe`), never shipped |
| `patterns18.json` | SDK 18's patterns: two weapon patterns on every weapon taking paints, the Pelican's, hellpod's, exosuits' and FRVs' skins in every collection, their deltas and package map, the Armory's Pattern tab captured |
| `pattern-probe/` | the test mod `patterns18.json` runs (`hellforge.pattern-probe`), never shipped: magenta copies of Forest Camo (material LUT swaps on weapons, a pattern_lut on units) and an Arctic Camo copy |
| `stratagem-codes.json` | stratagem input codes: a stats edit gives Resupply Up Up Left Right Down, the vanilla codes move into HellForge's arrays, and the new code calls Resupply in (needs the harness's `call` step) |
| `stratagem-codes-probe/` | the test mod `stratagem-codes.json` runs (`hellforge.stratagem-codes-probe`), never shipped: the stats edit, the Resupply row's code in the mission, every beacon's type |

## Running one

Use a test installation, not your own: publish the branch into it first (`scripts/Publish-HellForge.ps1
-InstallRoot <folder>`). Steam must be running and the game closed. From the harness's folder:

```powershell
py harness.py --target hellforge --hellforge-root <test installation> modtest <HellForge>\tests\in-game\sdk16.json
```

It prints a JSON verdict (`passed`, each check with the log lines it matched) and writes it to the harness run's
`modtest.json`, beside the session's logs (`hellforge-session/`). The mission makes the same requests a solo deploy
does, to the session's local backend.

## Running one in co-op

`modtest` plays solo. `rpc-probe` also checks co-op: the joiner sends the host one `rpc_request_hostability`, a query
the host's game answers. Two test installations on one PC play together with the harness's `pair.py`. From the
harness's folder:

```powershell
py livemods.py setup --hellforge-root <host installation> --copy <HellForge>\tests\in-game\rpc-probe --on hellforge.rpc-probe
py livemods.py setup --hellforge-root <joiner installation> --copy <HellForge>\tests\in-game\rpc-probe --on hellforge.rpc-probe
py pair.py pickup --host <host installation> --joiner <joiner installation>
```

`pickup` joins them and deploys the squad. Read `mod-hellforge.rpc-probe.log` in each installation's newest
`logs\sessions\<run>\` folder once the mission's tally is there, 30 s into the mission. The joiner's log must say
`PROBE rpc_send arrived ok`, the host's `PROBE rpc_send to itself refused ok`, and neither may have a `FAIL`. Then
end both with `py pair.py stop` and run `py livemods.py teardown --hellforge-root <installation>` for each.

## Adding a check

A probe writes what it checks to its log; the expectations file says which lines must be there (`contains`, `regex`
with `min`) and which must not (`absent`, `absent_regex`), in which log (`mod-<id>.log`, `modloader.log`,
`session.log`...). Retry a probe check that can only hold at some moment (in a mission, after a spawn) until it holds,
and write it as FAIL when its phase ends. `sdk-probe` does this, so the test does not depend on timing.
