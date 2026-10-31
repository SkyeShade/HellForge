# In-game tests

Checks that need the real game, run unattended by the development harness (CowboyBingus's BingusDevelopmentHarness,
its `modtest` command): it copies the test mods into a HellForge installation with every other mod off, plays a
session to the ship and into a solo mission and back, checks the logs the session wrote against an expectations file,
and puts the installation back as it was.

| File | What |
| --- | --- |
| `sdk16.json` | SDK 16's names in the game: hooks by name (`HF_HOOK`), typed events (`hf_on_...`), game functions (`hf_fn_...`) and game globals with their fields (`hf_global_...`) |
| `sdk-probe/` | the test mod `sdk16.json` runs (`hellforge.sdk-probe`), never shipped: one `PROBE <check> ok` or `PROBE <check> FAIL <why>` line per check in its log |

## Running one

Use a test installation, not your own: publish the branch into it first (`scripts/Publish-HellForge.ps1
-InstallRoot <folder>`). Steam must be running and the game closed. From the harness's folder:

```powershell
py harness.py --target hellforge --hellforge-root <test installation> modtest <HellForge>\tests\in-game\sdk16.json
```

It prints a JSON verdict (`passed`, each check with the log lines it matched) and writes it to the harness run's
`modtest.json`, beside the session's logs (`hellforge-session/`). The mission makes the same requests a solo deploy
does, to the session's local backend.

## Adding a check

A probe writes what it checks to its log; the expectations file says which lines must be there (`contains`, `regex`
with `min`) and which must not (`absent`, `absent_regex`), in which log (`mod-<id>.log`, `modloader.log`,
`session.log`...). A probe check that can only hold at some moment (in a mission, after a spawn) should be retried
until it holds and written as FAIL when its phase ends, as `sdk-probe` does, so the test does not depend on timing.
