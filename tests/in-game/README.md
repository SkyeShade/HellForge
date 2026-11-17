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
| `projectile-cap.json` | room for 1,024 new projectile types: 400 added (types 351 to 750), past the 161 the projectile type's 9-bit co-op field carries, the field widened to 11 bits, rows on both sides of the old reach |
| `projectile-cap-probe/` | the test mod `projectile-cap.json` runs (`hellforge.projectile-cap-probe`), never shipped |
| `attachment-cap.json` | room for 1,024 attachments: 640 drum copies (past the old 128) on three Liberator copies, the game's own item and delta lookups finding them, the 640th put in the primary's magazine slot on the ship and the copy held in the mission without a crash (whether the game equips the 640th from a hand-written loadout is a check by hand) |
| `attachment-cap-probe/` | the test mod `attachment-cap.json` runs (`hellforge.attachment-cap-probe`), never shipped |
| `stratagem-cap.json` | room for 1,024 new stratagems: 300 added (types 150 to 449), the co-op type field at 16 bits, type 449 in the loadout before the mission and called in by its code (needs the harness's `call` step) |
| `stratagem-cap-probe/` | the test mod `stratagem-cap.json` runs (`hellforge.stratagem-cap-probe`), never shipped |
| `patterns18.json` | SDK 18's patterns: two weapon patterns on every weapon taking paints, the Pelican's, hellpod's, exosuits' and FRVs' skins in every collection, their deltas and package map, the Armory's Pattern tab captured |
| `pattern-probe/` | the test mod `patterns18.json` runs (`hellforge.pattern-probe`), never shipped: magenta copies of Forest Camo (material LUT swaps on weapons, a pattern_lut on units) and an Arctic Camo copy |
| `editor-engine.json` | the editor's engine-backed rows: the Eruptor's and Variable's firing ability swapped, spare heatsinks added to the Sai's Improved Cooldown heatsink; the logs prove the edits land, the mission shows what they do |
| `editor-engine-probe/` | the stats mod `editor-engine.json` runs (`hellforge.editor-engine-probe`), never shipped |
| `editor-spawning.json` | the editor's spawning rows: what the Impaler's strike and the Alpha Commander's call summon (their code takes the family and count), a patrol group's enemy family, difficulty 7's Terminid patrol cap past 128 and traveller interval; the logs prove the edits land, a Terminid mission shows what they do |
| `editor-spawning-probe/` | the stats mod `editor-spawning.json` runs (`hellforge.editor-spawning-probe`), never shipped |
| `enemy-combat.json` | the editor's enemy combat rows: the Charger's charge speed, sight and armour 50, the Hunter's melee damage, turn speed and acceleration, a stun added to the Bile Titan and its fire damage multiplier; the logs prove the edits land, the mission shows what they do |
| `enemy-combat-probe/` | the stats mod `enemy-combat.json` runs (`hellforge.enemy-combat-probe`), never shipped |
| `missile-spread.json` | the editor's missile spread rows: the W.A.S.P. missile's spread off, the MLS-4X Commando's on with a 45° veer to one side; the logs prove the edits land, firing each weapon in the mission shows what they do |
| `missile-spread-probe/` | the stats mod `missile-spread.json` runs (`hellforge.missile-spread-probe`), never shipped |
| `missile-spread-look.json` | which way the spread turns: the primary fires the Commando's missile with a 45° veer at direction 0 and a pitch-only curve; the probe measures the heading against the launch frame (direction 0 goes up, the curve's first axis pitches) |
| `missile-spread-look-probe/` | the test mod `missile-spread-look.json` runs (`hellforge.missile-spread-look-probe`), never shipped; its raw stats arm the AR-23 Liberator, the primary of the install it was written for |
| `spray20.json` | SDK 20's sprays: two FLAM-40 copies with damage of their own (a scaled one, the Sterilizer's gas), each in its own spray record while the FLAM-40 keeps its own; the hot copy's stratagem in the loadout, called in, its hellpod bringing the copy. Spraying enemies with it is by hand |
| `spray-probe/` | the test mod `spray20.json` runs (`hellforge.spray-probe`), never shipped; it also logs the first hits the called-in copy deals |
| `stratagem-codes.json` | stratagem input codes: a stats edit gives Resupply Up Up Left Right Down, the vanilla codes move into HellForge's arrays, and the new code calls Resupply in (needs the harness's `call` step) |
| `stratagem-codes-probe/` | the test mod `stratagem-codes.json` runs (`hellforge.stratagem-codes-probe`), never shipped: the stats edit and every beacon's type |
| `stratagem-uses.json` | rearms after uses: a stats edit gives the Orbital Railcannon Strike 3 mission uses that rearm after a 90 s cooldown, the probe puts it in the loadout on the ship (filled as a pick fills a slot), three calls land, the uses left go to 0, a fourth (inside a 90 s cooldown) waits, the uses come back to 3 and a call lands again (needs the harness's `call` step) |
| `pod-break.json` | a mod's support weapon from a broken hellpod: an EAT-17 copy (`register_entity`) on a copy of the EAT-17's rack and the vanilla rack, each called in and broken by the game's own rack death; the copy must drop with the EAT-17's pickup on (needs the harness's `call` step). Picking one up is by hand |
| `pod-break-probe/` | the test mod `pod-break.json` runs (`hellforge.pod-break-probe`), never shipped: both stratagems in the loadout on the ship, each rack broken 20 s after it spawns, the launchers' interactions and which are on, before and after |
| `stratagem-uses-probe/` | the test mod `stratagem-uses.json` runs (`hellforge.stratagem-uses-probe`), never shipped: the stats edit, the loadout, the uses left the HUD shows after each change, the Railcannon's beacons counted |

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
