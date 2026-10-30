# Your first mod

This walkthrough takes you from nothing to a code mod running in game, then through changing it, reading its log and
sharing it. It is for newcomers to HellForge modding, including people new to C. It takes about ten minutes, most of it
the one-time download of the mod tools.

The mod you make starts as HellForge's template: the AR-23 Liberator fires at 1200 rounds per minute with no recoil. In
an installation these guide pages are in `app/sdk/docs/modding`, and the [SDK reference](../sdk-reference.md) lists every
field and constant you will meet.

## Before you start

- HellForge is installed with HellForge-Setup and the launcher opens. The installation is a folder, by default
  `Documents\HellForge` (or `%USERPROFILE%\HellForge` when Documents is in OneDrive). This page writes it as
  `<installation>`.
- You have a text editor. An IDE is optional; [Tools and sharing](tools.md#ides) sets one up later.
- No session is running. The launcher builds mods only between sessions, because the game holds the mods' DLLs while it
  runs.

## 1. Create the mod from the template

1. Open the launcher's MODS page and press **Open mods folder**.
2. In a second Explorer window, open `<installation>\app\sdk` and copy its `template-mod` folder into the mods folder.
3. Rename the copy after your mod, for example `liberator-tweaks`. The folder name is up to you; the mod is known by
   the id in its manifest.
4. Open `hellforge-mod.json` in the new folder and change `id`, `name` and `description`:

   ```json
   {
     "format": 1,
     "id": "yourname.liberator-tweaks",
     "name": "Liberator Tweaks",
     "version": "0.1.0",
     "description": "Liberator Tweaks: a HellForge code mod. Source in src/.",
     "gameplay": true,
     "code": { "dll": "my_mod.dll", "sdk": 16 }
   }
   ```

   The `id` is `owner.mod-name` in lowercase (letters, digits and `-`). It names the mod everywhere: its log, the
   identities of the content it adds, and the modlist code friends compare. Always change it from the template's
   `yourname.my-mod`, because two mods with one id load neither.

The Mods page follows the mods folder, so the mod appears in the GAMEPLAY group as soon as the folder is there (press
**Refresh** if it does not). It is switched on, and it has a CODE tag because it runs code inside the game.

From a terminal, `new-mod` makes the same folder with your names filled in, including the DLL and source file names:

```
<installation>\app\cli\HellForge.Cli.exe new-mod --id yourname.liberator-tweaks --name "Liberator Tweaks" --install <installation>
```

It creates `mods\liberator-tweaks` (the part of the id after the dot), with `"dll": "liberator_tweaks.dll"` and
`src\liberator_tweaks.c`, and prints the folder and whether the mod tools are ready. A mod id for `new-mod` must be
`owner.mod-name` with each part at most 31 characters.

## 2. What is in the folder

```
mods/liberator-tweaks/
  hellforge-mod.json   the manifest: id, name, version, description, "code": {"dll": "my_mod.dll", "sdk": 16}
  src/my_mod.c         the mod's C source
  CMakeLists.txt       for IDEs (Rider, CLion, VS Code)
  CMakePresets.json    the IDE build settings: HellForge's pinned Zig, CMake and Ninja
  .gitignore           keeps built DLLs and the build/ folder out of git
```

The manifest's keys:

| Key | Meaning |
|---|---|
| `format` | always 1 |
| `id` | the mod's unique id, `owner.mod-name` |
| `name`, `description` | what players see on the Mods page |
| `version` | raise it for every release; friends compare it |
| `gameplay` | `true`: friends must run the same gameplay mods to play together |
| `code.dll` | the DLL's file name. HellForge builds it from every `.c` and `.cpp` file under `src/` |
| `code.sdk` | the mod SDK version the mod needs; this HellForge provides SDK 13 |

The full list, with `icon` and the rest, is in [How mods run](concepts.md#the-manifest). You can rename `src/my_mod.c`
and `"dll"` to anything; if you rename the source file, change it in `CMakeLists.txt` too, so an IDE still finds it.

## 3. Install the mod tools

Code mods are compiled by Zig. HellForge fetches it once into `<installation>\tools`: about 90 MB from ziglang.org,
pinned to one version and checked against its checksum. Nothing is installed system-wide.

1. On the Mods page, a MOD TOOLS card shows above the mod list while a mod with sources waits for the compiler.
2. Press **Install mod tools**. The card shows each step, and the CONSOLE page says `Mod tools installed in ...` when it
   is done. CMake and Ninja come too, for IDEs.

From a terminal: `HellForge.Cli mod-tools --install <installation> --ide yes`. Without `--ide yes` it fetches Zig
alone. In the HellForge repository, `scripts/Setup-ModTools.ps1` does the same into the repository's `tools` folder.

If you skip this step, the first session that has a mod to build fetches Zig by itself, without CMake and Ninja.

## 4. Build it

You do not have to build by hand: HellForge builds every mod with sources when a session starts, and keeps that build
until the sources change. Building now shows any compiler errors at once.

1. Open the mod's **⋮** menu on the Mods page (or right-click its row).
2. Choose **Build now**. It is available once the mod tools are installed and no session is running.

The CONSOLE page shows `Building yourname.liberator-tweaks...` and then `yourname.liberator-tweaks is built (... KB);
the next session loads it.` The build goes to `<installation>\temp\mod-builds\<mod id>\`, which is where sessions load
it from.

If the code does not compile, the mod's row opens with `Did not compile; it stays off until fixed:` and the compiler's
message with the file and line, and the row gets a DID NOT COMPILE tag. Every mod is built with `-Wall -Werror`, so a
warning fails the build too. A mod that does not compile stays off for that session, and the session still runs.

From a terminal: `HellForge.Cli build-mod --mod <installation>\mods\liberator-tweaks`. For a mod in an installation's
mods folder it makes the same build a session would.

## 5. See it in game

1. Press **PLAY**. The launcher builds and stages the mods, then starts the game.
2. In game, take the AR-23 Liberator. It fires at 1200 rounds per minute with no recoil.

## 6. Read its log

Every session has a folder of logs, `<installation>\logs\sessions\<session>\`. The CONSOLE page's **Open session logs**
opens the folder of the session you just played (or `logs\sessions` when there is none yet). Your mod's log is
`mod-<id>.log`, here `mod-yourname.liberator-tweaks.log`, and it has the line the template writes once the game's
entity data has loaded:

```
AR-23 Liberator: 1200 rounds per minute
```

The same log is where the SDK says what it refused and why, with the line in your source. If the mod did not load at
all, `modloader.log` in the same folder says why. [Debugging](debugging.md) lists every log file.

## 7. Change it

Open `src/my_mod.c` (or `src/liberator_tweaks.c` if you used `new-mod`). Without its opening comment, this is the whole
template as `app/sdk/template-mod` has it (`new-mod` puts your own id in the last comment):

```c
#include "hellforge_mod.h"

static const HfSet liberatorSet[] = {
    {HF_FIELD_AT(ProjectileWeapon, fire_rate, 1), HF_F32(1200)}, /* the rate slot the weapon fires at */
    HF_SET_GROUP(WeaponData, recoil, HF_F32(0)),                  /* both recoil blocks, sideways and up */
    {0}
};
static const HfEditDef liberator = {.entity = HF_ENTITY_AR23_LIBERATOR, .set = liberatorSet};

/* Events are for logic, not for timing edits: here, a line in the log once the game's data has loaded. */
static void Ready(HfMod* mod, HfEvent event, void* user)
{
    const HfProjectileWeapon* weapon = hf_record(mod, HF_ENTITY_AR23_LIBERATOR, HfProjectileWeapon);
    if (weapon != NULL) HF_API(mod)->log(mod, "AR-23 Liberator: %.0f rounds per minute", (double)weapon->fire_rate[1]);
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    if (hf->version < 6) return 1; /* needs HellForge's mod SDK 6 */
    hf->on(mod, HF_EVENT_ENTITIES_READY, Ready, NULL);
    /* 0: loaded. Otherwise the session's log folder has mod-yourname.my-mod.log saying why. */
    return hf->edit_entity(mod, &liberator);
}
```

Read it from the top:

- `liberatorSet` is a definition block's list of changes, one per line, ending with `{0}`. Each change names a field
  and gives its new value.
- Fields have names. `HF_FIELD(Component, field)` names one field of a component, and `HF_FIELD_AT(Component, field,
  index)` one element of an array field: `fire_rate` holds three rate slots, and slot 1 is the one the weapon fires at.
  `HF_SET_GROUP(WeaponData, recoil, ...)` sets every field of the `recoil` group (four fields: two recoil blocks,
  sideways and up). The components and their fields are in the [reference](../sdk-reference.md#components), such as
  `HfWeaponMagazine`'s `capacity` or `HfWeaponData`'s `horizontal_spread`. A misspelt name does not compile, and an IDE
  completes them.
- Values carry their type: `HF_F32(1200)` for a float field, `HF_U32(60)` for an unsigned integer. The loader refuses a
  value of the wrong type or out of range, with your source line, in the mod's log.
- Vanilla content has constants: `HF_ENTITY_...`, `HF_ITEM_...`, `HF_STRATAGEM_...`, `HF_TRAIT_...`, all in
  `hellforge_content.h` and the [reference](../sdk-reference.md#content). `liberator` ties the list of changes to the
  AR-23 Liberator.
- `hellforge_mod_init` is the function the loader calls once, while the game starts. It checks the SDK version,
  registers `Ready` for the `HF_EVENT_ENTITIES_READY` event, and hands the definition block to `edit_entity`. Returning
  0 keeps the mod loaded.
- The loader applies the block whenever the game loads its data, so you never time an edit with an event. Events are for
  logic: here `Ready` reads the Liberator's record back with `hf_record` and logs its fire rate.

Now give the Liberator a bigger magazine. Add one line to `liberatorSet`, before `{0}`:

```c
    {HF_FIELD(WeaponMagazine, capacity), HF_U32(60)},
```

Save, choose **Build now** again (or leave it to the next session), and press **PLAY**. A change to the sources loads
from the next session; while a session runs, builds wait.

### The reference example

`machine-gun-10k-code` (in `app/sdk/examples/mods`, or `examples/mods` in the repository) is the SDK's reference
example, and the next code to read. It shows the three things most mods do: a definition block that changes an
existing entity by field name, an event that runs code once the game is ready for it, and log lines that say what
happened. Its definition block gives the M-1000 Maxigun's backpack 10,000 rounds:

```c
static const HfSet backpackSet[] = {
    {HF_FIELD(Deposit, capacity), HF_U32(10000)},     /* vanilla 1000: the most it holds */
    {HF_FIELD(Deposit, start_amount), HF_I32(10000)}, /* vanilla 1000: what it holds at call-in; the HUD's maximum */
    {HF_FIELD(Deposit, refill_amount), HF_U32(5000)}, /* vanilla 500: what a resupply adds */
    {0}
};
static const HfEditDef backpack = {.entity = HF_ENTITY_MAXIGUN_BACKPACK, .set = backpackSet};
```

and its init function registers two events before it queues the edit:

```c
HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    if (hf->version < 6) return 1; /* edit_entity, hf_record and HF_API(mod) need SDK 6 */
    hf->log(mod, "Machine Gun 10k for game build %s", hf->build_id());
    hf->on(mod, HF_EVENT_NETWORK_READY, WidenAmmoField, NULL);
    hf->on(mod, HF_EVENT_ENTITIES_READY, BackpackReady, NULL);
    return hf->edit_entity(mod, &backpack); /* 0: queued for whenever the entity data loads */
}
```

`WidenAmmoField` widens the `deposit_value` network field from 10 to 14 bits with `set_network_field_bits`, because
the engine would otherwise clamp the backpack's live ammo to 1,023. The source file explains each part. To try it, copy
its folder into the mods folder.

## 8. Share it

1. Raise `version` in `hellforge-mod.json` for each release.
2. Choose **Zip to share** in the mod's **⋮** menu. HellForge builds the mod first, so a mod that does not compile is
   not packed.
3. HellForge writes `<installation>\exports\<folder>-<version>.zip` (here `liberator-tweaks-0.1.0.zip`) and shows it
   in Explorer.

The zip holds the mod's folder: the manifest, `src/` and every other file, without the DLL and without workspace
leftovers (dot files and folders, `build/`, `*.user` files). From a terminal, `HellForge.Cli package-mod --mod <folder>`
does the same, and `--out <folder>` writes the zip elsewhere, but never into an installation's mods folder.

Players drop the zip into their `mods` folder as it is, or unpacked, but not both, since two copies of one id load
neither. They need no DLL and no tools setup: their HellForge downloads Zig by itself the first time and builds the
sources. Friends who play together need the same gameplay mods; the Mods page shows the modlist code to compare.

Once the mod has a page (GitHub, Nexus Mods), add its address to the manifest as `"url": "https://..."`. The Mods page
links it, and players who join a friend running your mod get a GET IT button for it in game
([tools and sharing](tools.md#players-who-join-you)).

A code mod runs inside the game with the player's permissions, so players should only enable code mods from people they
trust.

## Where to go next

Next, read [How mods run](concepts.md): when your code runs, threads, co-op, load order and compatibility. Then pick
the page for what you want to make:

- new content: `register_entity` with an `HfEntityDef` (a copy of a vanilla entity with its own records and changes),
  `add_stratagem`, `add_weapon`, `add_behaviour`, `add_stratagem_category`, in [Content](content.md). The examples
  `stratagem-compendium` (eleven stratagems, one source file per family, under a selection heading of their own) and
  `stratagem-categories` (headings and their order) show them.
- number changes without code: the in-game editor (F8) edits values live and saves them into the Local Stats mod, and
  EXPORT on its PRESETS page shares them ([Stats mods](stats-mods.md)).
- calls alongside definition blocks: `hf_set`, `hf_copy` and `hf_live` for logic, and `hf_record` for a typed pointer
  to a record ([Logic at run time](runtime.md)).
- events, actions and hooks: react to the game and make it do things ([Logic at run time](runtime.md)), and where the
  SDK has no event or action yet, hook the game's functions ([Hooks and game memory](hooks.md)).
- the difficulty 11 mod: `hell-forge` comes installed and built. To work on its source, replace `mods/hell-forge` with
  the example's folder instead of adding a second folder; a copy with `src/` is left as it is.
