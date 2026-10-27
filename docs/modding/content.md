# Content

This chapter is for code mod authors who want to change the game's weapons, stratagems and other entities, or add new
ones: entities, weapons, projectiles and damage types, stratagems and their selection headings, behaviours, abilities
and a difficulty level.

After reading it you will be able to:

- change any field of an existing entity by name, with checks that refuse a mistake instead of crashing;
- add a new entity, weapon, projectile, damage type, stratagem, behaviour or ability as a copy of a vanilla one;
- add difficulty level 11;
- find vanilla content and other mods' content by name.

Before you start, you should have built [your first mod](first-mod.md) and read [how mods run](concepts.md), in
particular [when your code runs](concepts.md#when-your-code-runs), [names and identities](concepts.md#names-and-identities)
and [the three ways to change the game's data](concepts.md#three-ways-to-change-the-games-data). New models, icons and
sounds are game data, built by a data spec: see [data mods](data.md) (this chapter tells you when you need one).

## Contents

- [How content works](#how-content-works)
- [Changing existing content](#changing-existing-content)
- [New entities](#new-entities)
- [Weapons](#weapons)
- [Projectiles and damage](#projectiles-and-damage)
- [Stratagems](#stratagems)
- [Selection headings](#selection-headings)
- [Behaviours](#behaviours)
- [Abilities](#abilities)
- [A difficulty](#a-difficulty)
- [Settings rows](#settings-rows)
- [Finding content](#finding-content)
- [Where to go next](#where-to-go-next)

## How content works

Everything on this page is game data. The game reads it the way it reads its own, and no code of yours runs per shot or
per frame. You describe the change once, in `hellforge_mod_init`, and the loader applies it whenever the game loads its
data (and again if the game reloads it), before `HF_EVENT_ENTITIES_READY` reaches any mod. You never time an edit with
an event ([how mods run](concepts.md#when-your-code-runs)).

New content is always a copy of something vanilla: a new stratagem copies a vanilla stratagem's settings, a new weapon
copies a vanilla weapon. You give the copy a name and change what you want; everything else stays the base's. The name
is the copy's identity. Its ids come from the name (an entity's resource id is the name's hash) or are numbered in the
order of mod ids (stratagem, projectile and damage types), so every player's game agrees on them without talking.

A copy of premium content (a premium warbond's or a Steam DLC's weapon, a Superstore item) is in a player's armory only
when they own the original. Content built on premium content in other ways is declared in the manifest's `premium`
list ([premium content](concepts.md#premium-content)).

Every struct is documented field by field in `native/mod-sdk/hellforge_mod.h`, and every component, field and vanilla
constant is in the [SDK reference](../sdk-reference.md). The calls on this page, with the SDK version that added each:

| Call | SDK | Adds or changes | Returns | Room across all mods |
|---|---|---|---|---|
| `edit_entity`, `hf_set`, `hf_copy`, `hf_tag`, `hf_mount` | 6 | fields of an existing entity | 0 queued, 1 refused | |
| `register_entity` | 6 | an entity (from a definition block) | its resource id, or 0 | 64, shared with weapons |
| `add_entity` | 4 | an entity (the older form) | its resource id, or 0 | the same 64 |
| `add_weapon` | 2 | an arsenal weapon | its loadout id, or 0 | the same 64 |
| `add_damage` | 3 | a damage type | its id (650 and up), or 0 | 64 |
| `add_projectile` | 3 | a projectile type | its type (351 and up), or 0 | 64 |
| `add_stratagem` | 4 | a stratagem | its type (150 and up), or 0 | 48 |
| `add_stratagem_category` | 8 | a heading in the selection screen | a handle, or 0 | 28 |
| `add_behaviour` | 4 | a behaviour | its id (694 and up), or 0 | 8 |
| `add_ability` | 7 | an ability | its id (2868 and up), or 0 | 8 |
| `add_difficulty` | 5 | difficulty level 11 | 11, or 0 | one mod |

When a call returns 0 (or 1 for the edit calls), your mod's log says why; `hf->last_log(mod)` gives you that line in
code. Check `hf->version` for the SDK a call needs, and `hf->size` for a call added after the version you require
([compatibility](concepts.md#compatibility)).

## Changing existing content

An entity's data is split into components (`WeaponMagazine`, `WeaponData`, `ProjectileWeapon`, `Deposit`...), and each
component record has named fields. To change an existing entity, you write a definition block: a list of fields and the
values they take, ending in `{0}`, and the entity it applies to.

This is the mod template's own change (`templates/c-mod`), which makes the AR-23 Liberator fire at 1200 rounds per
minute with no recoil:

```c
static const HfSet liberatorSet[] = {
    {HF_FIELD_AT(ProjectileWeapon, fire_rate, 1), HF_F32(1200)}, /* the rate slot the weapon fires at */
    HF_SET_GROUP(WeaponData, recoil, HF_F32(0)),                  /* both recoil blocks, sideways and up */
    {0}
};
static const HfEditDef liberator = {.entity = HF_ENTITY_AR23_LIBERATOR, .set = liberatorSet};

/* in hellforge_mod_init */
return hf->edit_entity(mod, &liberator);   /* 0 queued; 1 refused, the log says why */
```

The pieces:

- `HF_FIELD(Component, field)` names a field, such as `HF_FIELD(WeaponMagazine, capacity)`. A misspelt name does not
  compile. `HF_FIELD_AT(Component, field, index)` names one element of an array field.
- `HF_U32(2000)`, `HF_F32(0)`, `HF_I32`, `HF_U8`, `HF_U16` and `HF_U64` are values tagged with their type. The tag must
  be the field's type, and the value must fit it (`HF_U32(-1)` is refused, not wrapped).
- `HF_SET_GROUP` and `HF_COPY_GROUP` expand a group of fields the mappings name: the casings, the firing sounds, both
  recoil blocks, the spread.
- `HfEditDef` takes the same lists as a new entity's block: `copy` (fields taken from another entity), `set`,
  `add_tags`, `remove_tags` and `mount`. Every list may be NULL.

The [reference](../sdk-reference.md#components) lists every component with its fields, each with its type, its meaning
and when a change applies.

`examples/mods/machine-gun-10k-code` is a complete mod built around one block: the Maxigun backpack's
`DepositComponentData` record, with three fields set.

### What the loader checks

When the block is queued, the loader checks every field and value: a component without a mapping, a field of another
layout, a value of another type or out of range, a set on a whole array. If anything is wrong it refuses the whole
block, logs each problem with your source line, and changes nothing.

### Shared records

Many entities share records (several weapons can share one magazine record). A vanilla entity's record is changed only
if no other entity shares it; a shared one is refused when the data loads. To change one entity alone, register a copy
of it ([new entities](#new-entities)) with records of its own. Stats mods can change shared records, with their own
guards ([stats mods](stats-mods.md)).

### The same edits as calls

For code, `hf_set(mod, entity, Component, field, HF_U32(2000))`, `hf_copy(mod, entity, from, Component, field)`,
`hf_tag(mod, entity, HF_TAG_..., 0 or 1)` and `hf_mount(mod, entity, slot, carried)` queue the same edits as a block.
An entity a mod added gets a record of its own in the field's component when the call comes from `hellforge_mod_init`;
called later, the entity must own one already.

### Reading a record

`hf_record(mod, entity, HfWeaponData)` returns a typed pointer to an entity's own record after
`HF_EVENT_ENTITIES_READY`, or NULL. A vanilla entity's records are the game's read-only data: read them, never write
through the pointer (that crashes the game). An entity your mod added is yours to write.

### Conflicts

Two code mods changing the same field are reported in both mods' logs and `modloader.log`, and the later one in load
order wins. A code mod's edit always applies after the stats files (stats mods, Local Stats), so it stays over theirs
whatever the order ([load order](concepts.md#load-order-and-conflicts)).

## New entities

An entity is anything the game spawns: a sentry, a Pelican, a missile, a turret, a hellpod rack. `register_entity`
(SDK 6) adds one as a copy of a vanilla entity under a name of its own, from a definition block (`HfEntityDef`). The
block has the lists an edit has, plus a `name` and a `base`.

`examples/mods/blue-sentry` adds the simplest kind: the A/MG-43 Machine Gun Sentry with a blue model and a name of its
own.

```c
static const HfEntityDef sentryDef = {
    .size = sizeof(HfEntityDef),
    .name = "skyeshade/blue_sentry/sentry_entity",
    .base = HF_ENTITY_AMG43_MACHINE_GUN_SENTRY,
    .unit = "skyeshade/blue_sentry/sentry", /* the blue model hellforge-data.json builds */
    .title = "Blue Metal Sentry",
    .designation = "A/MG-43B",
};

/* in hellforge_mod_init */
HfRef sentry = hf->register_entity(mod, &sentryDef);   /* its resource id, or 0 if refused */
```

Keep definitions static: the loader reads them once, at registration, and they must stay in memory.

### What a copy shares

The copy shares its base's records except in the components the block changes, where it gets records of its own. Set
`own_all = 1` (with `.size = sizeof(HfEntityDef)`) to give it its own record in every component. Then an edit of the
base (a stats mod, the in-game editor) never reaches the copy, and the copy's edits never reach the base. Prefer this
for anything you will tune. A component HellForge cannot size for certain stays shared, and the log names it.

The compendium's A/MG-101 Heavy MG Sentry (`examples/mods/stratagem-compendium/src/hmg_sentry.c`) is an `own_all` copy
that takes the MG-206 Heavy Machine Gun's casings and sound, and sets its own rate, spread and magazine:

```c
static const HfCopy hmgCopies[] = {
    HF_COPY_GROUP(HF_ENTITY_MG206_HEAVY_MACHINE_GUN, ProjectileWeapon, casings),
    HF_COPY_GROUP(HF_ENTITY_MG206_HEAVY_MACHINE_GUN, ProjectileWeapon, firing_sounds),
    {0}
};
static HfSet hmgSet[48]; /* filled once its round's type is known */

static HfEntityDef hmgSentry = {
    .size = sizeof(HfEntityDef),
    .name = "skyeshade/hmg_sentry/hmg_sentry",
    .base = HF_ENTITY_AMG43_MACHINE_GUN_SENTRY,
    .copy = hmgCopies,
    .set = hmgSet,
    .own_all = 1,
    .title = "Heavy MG Sentry",
    .designation = "A/MG-101",
    .description = kDescription,
};
```

### Model

`model_from` takes another entity's model together with every record that names the model's parts (thrusters,
effects, collision, seats) and its packages. A model under another model's records hangs the game when it spawns, which
is why they come together. `unit` names a model your mod's data provides ([data mods](data.md)).

### Text

`title`, `designation` and `description` (SDK 7) give the entity text of its own. Like `own_all`, they are read only
when `.size = sizeof(HfEntityDef)`; otherwise they are silently ignored. The game names a stratagem after its payload
entity (`<designation> <title>`), so give a payload entity its own designation.

### Mounts and behaviours

A `mount` entry carries another entity in one of the entity's mount slots (slot 0 is a Pelican's chin). An entry that
points at another block registers that block first. `behaviour` points at an `HfBehaviour` ([behaviours](#behaviours)),
added once however many entities name it; `behaviour_id` names an existing one instead.

This is the compendium's Pelican Gatling Support (`src/pelicans.c`), condensed: a Pelican carrying a turret of its own
that fires the MG-206's casings and sound at the Gatling Sentry's rate, credits its kills to the players, and holds
2000 rounds.

```c
static const HfCopy turretCopies[] = {
    {HF_ENTITY_MG206_HEAVY_MACHINE_GUN, HF_FIELD(ProjectileWeapon, projectile)},
    HF_COPY_GROUP(HF_ENTITY_MG206_HEAVY_MACHINE_GUN, ProjectileWeapon, firing_sounds),
    {HF_ENTITY_GATLING_SENTRY, HF_FIELD(ProjectileWeapon, fire_rate)},
    {0}
};
static const HfSet turretSet[] = {HF_SET_GROUP(WeaponData, recoil, HF_F32(0)), {HF_FIELD(WeaponMagazine, capacity), HF_U32(2000)}, {0}};
static const uint64_t chinTags[] = {HF_TAG_NO_KILL_CREDIT, 0};
static const HfEntityDef turret = {.name = "turret", .base = HF_ENTITY_PELICAN_TURRET,
    .copy = turretCopies, .set = turretSet, .remove_tags = chinTags};
static const HfMountDef mounts[] = {{.slot = 0, .entity = &turret}, {0}};
static const HfEntityDef pelican = {.name = "pelican", .base = HF_ENTITY_PELICAN_TRANSPORT, .mount = mounts};
/* in hellforge_mod_init */
HfRef ref = hf->register_entity(mod, &pelican);   /* registers the turret too; 0 if refused */
```

### Other rules

- Registering the same block again returns the same entity.
- `add_entity` (SDK 4, `HfEntity`) is the older call; new mods use blocks.
- A block changes or owns at most 8 components and has at most 8 mounts; there is room for 64 blocks.
- Entities and weapons share 64 places across all mods.

`examples/mods/stratagem-compendium` builds every stratagem this way, one source file per family.

## Weapons

`add_weapon` (SDK 2) puts a new weapon in the arsenal: a copy of an existing one with its own loadout id, model, name
and, if you want, stats. `examples/mods/golden-liberator` is the whole mod:

```c
HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    if (api->version < 2) return 1;
    HfWeapon gold = {sizeof gold};
    gold.base = HF_ITEM_AR23_LIBERATOR;
    gold.unit = "skyeshade/golden_liberator/assault_rifle_gold";
    gold.designation = "AR-23G";
    gold.name = "Liberator Gold";
    gold.description = "The SEAF standard assault rifle, plated in gold for those who serve Liberty with distinction. Performs "
        "exactly like the Liberator; looks considerably more democratic.";
    uint32_t id = api->add_weapon(mod, &gold);
    api->log(mod, id ? "AR-23G Liberator Gold is loadout item %08x" : "the weapon was not added", id);
    return id ? 0 : 1;
}
```

`HfWeapon gold = {sizeof gold};` zeroes the struct and sets its size, so fields added in later SDKs read as unset.
`base` is the vanilla weapon it copies, by loadout id (`HF_ITEM_...`). `unit` is its model, which the mod's data
provides; here a data spec builds a gold copy of the Liberator's model ([data mods](data.md#data-specs)).

### Identity

The weapon's entity is the hash of `unit`, and its loadout id is the top 32 bits of that, so every player's game agrees
on it. Prefix `unit` with your name and mod.

### What it shares

The weapon has everything its base has (handling, magazine, ammunition, customisation, sounds) except its loadout entry,
its model, its text and the components it owns. `own_components` lists up to 8 components to own
(`{"WeaponMagazineComponentData", NULL}`); after `HF_EVENT_ENTITIES_READY`,
`hf_record(mod, hf->weapon_entity(id), HfWeaponMagazine)` is the weapon's own record.

### Text

Text is one language for every game language. The armory shows `<designation> <NAME>`. Without a `name`, the weapon
keeps its base's text.

### Sounds

`sound_from` (a vanilla weapon's loadout id, SDK 3) gives the weapon that weapon's firing sounds. That weapon's sound
banks must load with yours, so your data includes them (`examples/mods/talon-liberator`).

### Where it shows

The weapon appears in the armory and in the player's inventory (the local backend adds it for the session). A session
in which a mod adds a weapon (or changes component stats) writes `arsenal.tsv` into its log folder: every armory item's loadout id, type, entity,
designation and name, for choosing a base.

`cube-gun` gives a weapon a model of your own, built from an OBJ file.

## Projectiles and damage

A weapon fires projectiles, and each projectile deals a damage type. Both are rows in tables the game keeps: a
projectile row holds speed, gravity, lifetime, effects and the damage id it deals; a damage row holds standard and
durable damage, armour penetration and status effects. `add_damage` and `add_projectile` (SDK 3) copy a vanilla row
under an id of its own, so changing yours changes no vanilla weapon.

`examples/mods/talon-liberator` adds all three: a damage type with five times the LAS-58 Talon's damage, a projectile
that deals it with a blue look, and a Liberator that fires it with the LAS-16 Sickle's sound.

```c
HfDamage heavy = {sizeof heavy};
heavy.base = HF_DAMAGE_LAS58_TALON;
heavy.name = "bolt_damage"; /* SDK 6: short names are the mod's ("skyeshade.talon-liberator:bolt_damage") */
heavy.scale = 5.0f;
uint32_t damage = api->add_damage(mod, &heavy);

HfProjectile bolt = {sizeof bolt};
bolt.base = HF_PROJECTILE_LAS58_TALON;
bolt.name = "bolt";
bolt.damage = damage;
bolt.spawn_effect = api->resource_hash("skyeshade/talon_liberator/assault_rifle_talon/bolt_blue");
bolt.flight_effect = api->resource_hash("skyeshade/talon_liberator/assault_rifle_talon/bolt_flight_blue");
uint32_t projectile = api->add_projectile(mod, &bolt);

HfWeapon rifle = {sizeof rifle};
rifle.base = HF_ITEM_AR23_LIBERATOR;
rifle.unit = "skyeshade/talon_liberator/assault_rifle_talon";
rifle.designation = "AR-23T";
rifle.name = "Liberator Talon";
rifle.description = "...";
rifle.projectile = projectile;
rifle.sound_from = HF_ITEM_LAS16_SICKLE; /* its banks are in data/ with the rifle's model */
uint32_t weapon = api->add_weapon(mod, &rifle);
```

- `scale` multiplies the base's standard and durable damage (0 means 1); the rest stays the base's. New damage ids are
  650 and up.
- A projectile's `damage` is any damage id (0 keeps the base's). `visual_from` takes another projectile type's look,
  such as `HF_PROJECTILE_AR23_LIBERATOR`. New projectile types are 351 and up.
- A weapon's `projectile` is any projectile type, vanilla or added.
- Add damage before the projectiles that deal it, and projectiles before the weapons that fire them.

### Changing your rows further

After `HF_EVENT_PROJECTILES_READY`, `projectile_row(type)` and `damage_row(id)` return your rows to change further.
They return addresses: cast them to `HfProjectileSettings*` and `HfDamageInfo*`, and keep each row's id as it is.
`hf_set` reaches the rows too. The compendium's Pelican EMS and Gas turrets give their rounds an impact explosion this
way (`src/pelicans.c`), and `hf_set(mod, type, ProjectileSettings, impact_explosion, HF_U32(82))` makes a projectile's
hits burst into the Orbital Gas Strike's cloud. HellForge loads the explosion's packages.

### Ammunition

Attachments can set a weapon's projectile when it is assembled (the Liberator's default ammunition does). A weapon given
`projectile` drops the default ammunition from its own customisation, so it fires what you named.

### Looks

A projectile looks the way its particle effects (`spawn_effect`, `flight_effect`) draw it. The colours are curves inside
the effect, so a recoloured effect comes as data ([data specs](data.md#data-specs)). `examples/mods/talon-liberator`
turns the Talon's bolt blue.

### Room and co-op

There is room for 64 new projectile types and 64 new damage types across all mods. In co-op, every machine fires its
own copy of a hand-held weapon's shots from its own data; with the same mods, that is the same projectile.

## Stratagems

`add_stratagem` (SDK 4) adds a stratagem: a copy of a vanilla one's settings under a type of its own (150 and up), with
its own name, description, input code, icon, cooldown and uses. Players own it and pick it in the loadout. What it does
when called is its base's, or what its `payload` names.

`examples/mods/blue-sentry` calls in the blue sentry from [new entities](#new-entities):

```c
#define SUPPORT_HELLPOD 0x73F8498BFFDCF415ull /* the hellpod support weapons and sentries come in (payload[1]) */

/* in hellforge_mod_init, after register_entity */
static uint64_t payload[2];
payload[0] = sentry;
payload[1] = SUPPORT_HELLPOD;
HfStratagem stratagem = {sizeof stratagem};
stratagem.base = HF_STRATAGEM_AMG43_MACHINE_GUN_SENTRY;
stratagem.name = "skyeshade/blue_sentry/stratagem";
stratagem.title = "Blue Metal Sentry"; /* shown after its sentry's designation */
stratagem.code = "DUDURR";
stratagem.payload = payload;
stratagem.payload_count = 2;
uint32_t type = hf->add_stratagem(mod, &stratagem);
```

`name` is the stratagem's identity: its stable id, which saved loadouts keep, is the name's 32-bit id.

### Payload

`payload` and `payload_count` (1 to 8) name what a call spawns: entities your mod added, or vanilla ones. `package` is
what it loads for the mission; by default, its payload entity's package. `loads_item` names a loadout item whose package
it also loads, as support weapons do (the compendium's Heavy MG Sentry and Pelicans load the MG-206's for their
round).

A Pelican drop (a base of delivery kind 3, such as `HF_STRATAGEM_EXO45_PATRIOT`) spawns `payload[1]` carrying
`payload[0]`; `{0, pelican}` sends an empty Pelican.

### Traits, category and beam

`flags` takes traits (`HF_TRAIT_CROWD_CONTROL | HF_TRAIT_ARMOR_PIERCING`); `HF_TRAIT_VEHICLE` makes it a vehicle (one
per loadout). `category` and `beam` take their constants (`HF_CATEGORY_...`, `HF_BEAM_...`), which set the HUD colour
and the beacon. 0 keeps the base's.

### Code

Pick a code that is neither another stratagem's code nor a prefix of one, and does not have one as its prefix.
HellForge does not check this for you. A code is 1 to 9 of U, D, L and R.

### Values

`cooldown` is 0 to 3600 seconds and `uses` is -1 (unlimited) and up. 0 keeps the base's for both.

### Icon

The icon is optional (without one, the stratagem shows its base's). It is a data spec `icon` entry
([data specs](data.md#data-specs)). An icon named but not built crashes the game, which is why the spec always builds
both the texture and its material. The icon is red where it takes its category's colour and green where it is white;
`"prepare": "auto"` converts a red-and-white picture.

### Owning it

The local backend gives the stratagem to the player for the session, as a copy of its base's catalogue entry. Nothing is
saved to the profile; a saved loadout naming a removed stratagem drops it.

### The in-game editor

The in-game editor gives every mod stratagem a page under its category. `editor_like` chooses which vanilla
stratagem's page it takes, and `editor_entities` which of your entities stand for the vanilla ones on that page.

### Limits

There is room for 48 new stratagems across all mods. A new stratagem never counts in mission statistics, is never a
mission's random pick, and does not load its base's ship-upgrade packages.

### Co-op

Peers exchange stratagem types as numbers, so every player must run the same mods; the modlist code makes sure of it.

## Selection headings

Stratagems can sit under headings of their own in the selection screen (SDK 8). A heading changes only where a
stratagem is listed: `category` still sets its HUD and ping colours. `examples/mods/stratagem-categories` registers two
headings and puts three stratagems under them:

```c
HfStratagemCategory category = {sizeof category, .name = "experimental", .title = "Experimental", .sort_order = 10};
uint32_t experimental = hf->add_stratagem_category(mod, &category);
category.name = "fire_support"; category.title = "Fire Support"; category.sort_order = 20;
uint32_t fireSupport = hf->add_stratagem_category(mod, &category);
if (!experimental || !fireSupport) return 1;

/* A blue support weapon and a green defensive sentry share one menu heading without changing their colours.
 * selection_order puts the sentry first although it is added second (equal orders keep call order). */
HfStratagem mg = {sizeof mg, .base = HF_STRATAGEM_MG43_MACHINE_GUN, .name = "experimental_mg",
    .title = "Experimental Machine Gun", .selection_category = experimental, .selection_order = 2};
HfStratagem sentry = {sizeof sentry, .base = HF_STRATAGEM_AG16_GATLING_SENTRY, .name = "experimental_sentry",
    .title = "Experimental Gatling Sentry", .selection_category = experimental, .selection_order = 1};
```

- Register a heading before the stratagems that use it.
- Headings follow the vanilla sections, lowest `sort_order` first (equal ones by name). Inside one heading,
  `selection_order` decides, lowest first; equal ones follow the order of their stratagem types, which follow the
  mods' set-up order (by mod id) and each mod's `add_stratagem` calls. Empty headings are hidden.
- A short name becomes `<mod id>:<name>`, so two mods' `"air_support"` are two headings. Mods share one heading by
  registering the same fully qualified name with an identical definition; a conflicting title or order is refused.
- A heading's `title` is required, UTF-8, under 192 bytes.
- There is room for 28 headings across all mods. `selection_category = 0` keeps the vanilla section.
- Handles are for the session only; never save them.

More on headings: [stratagem-categories.md](../stratagem-categories.md).

## Behaviours

A behaviour is the logic an entity runs, such as a Pelican's flight or a turret's choice of target. An entity's
`BehaviorComponentData` names it. `add_behaviour` (SDK 4) adds a vanilla behaviour that HellForge extends, under an id of
its own (694 and up). There are two bases so far:

- `HF_BEHAVIOUR_PELICAN_TRANSPORT` flies to the beacon and hovers. HellForge's version holds there for
  `hold_seconds` (0 to 600), circling the beacon (`orbit_radius` 0 to 200 m, `orbit_height` 0 to 200 m) and turning
  toward its turret's target (`face_target`).
- `HF_BEHAVIOUR_PELICAN_TURRET` is the chin turret, choosing what it sees and can hurt. `burst_seconds` (0 to 60; 0
  fires as long as it keeps a target), `reserve_rounds` (0 to 100000, refilled into its magazine; it never reloads) and
  `aim_height` (0 to 5 m above the target's feet).

The compendium's Pelicans use both (`src/pelicans.c`):

```c
static const HfBehaviour pelicanSupport = {
    .size = sizeof(HfBehaviour),
    .base = HF_BEHAVIOUR_PELICAN_TRANSPORT,
    .hold_seconds = 90.0f,
    .orbit_radius = 40.0f,
    .orbit_height = 60.0f,
    .face_target = 1,
};
/* Full auto, with HellForge's target choice; 4000 rounds in all. */
static const HfBehaviour fullAuto = {
    .size = sizeof(HfBehaviour),
    .base = HF_BEHAVIOUR_PELICAN_TURRET,
    .reserve_rounds = 2000,
    .aim_height = 0.15f, /* aims at the feet + 0.15 m: from 60 m up the game's aim point overshoots (0.7 and 0.3 still did) */
};
```

An entity block names one with `.behaviour = &pelicanSupport`. There is room for 8 new behaviours across all mods. If
HellForge cannot install its behaviours, entities given one run their base's.

## Abilities

An ability is what an entity does when its data sets it off: an explosive once it is armed, an ability weapon when it
fires. `add_ability` (SDK 7) adds a vanilla ability that HellForge holds for `hold_seconds` (0 to 600) before it runs,
playing sounds of its own at the entity: `start_event`, `second_event` every whole second, and `final_event`. The
sounds are Wwise event ids, and `package` names the package holding their bank. New abilities are 2868 and up; there is
room for 8 across all mods.

An entity runs the ability where its data names its vanilla one: an explosive's armed ability
(`HF_FIELD(Explosive, armed_ability)`), or an ability weapon's fire modes (`HF_FIELD(AbilityWeapon, mode0_ability)`).

`flags = HF_ABILITY_HOLDS_LINKED_LAUNCH` (with no base) makes a silo countdown: firing a copy of a silo's targeting
remote holds the launch of its missile for `hold_seconds`, then the missile follows the laser as the silo's does. This
is experimental. The compendium's Shredder Silo (`src/shredder_silo.c`) gives its remote a 10-second countdown with the
Launch ICBM objective's sound:

```c
HfAbility countdown = {sizeof countdown};
countdown.hold_seconds = COUNTDOWN_SECONDS;
countdown.flags = HF_ABILITY_HOLDS_LINKED_LAUNCH;
countdown.start_event = ICBM_COUNTDOWN;
countdown.package = PACKAGE_ICBM;
uint32_t countdownId = hf->size >= offsetof(HfApi, add_ability) + sizeof hf->add_ability ? hf->add_ability(mod, &countdown) : 0;
/* without it (an older HellForge), the silo's remote: it launches at once */
remoteSet[0] = (HfSet){HF_FIELD(AbilityWeapon, mode0_ability), HF_U32(countdownId)};
remoteSet[1] = (HfSet){HF_FIELD(AbilityWeapon, mode1_ability), HF_U32(countdownId)};
```

## A difficulty

`add_difficulty` (SDK 5) adds level 11 after Super Helldive: a copy of level 10 with what you change. One mod can add
it. `examples/mods/hell-forge` is the whole mod:

```c
HfDifficulty hell = {sizeof hell};
hell.name = "skyeshade/hell_forge/hell_forge";
hell.title = "Hell Forge";
hell.encounter_points = 1500.0f;   /* Super Helldive: 600 */
hell.guardforce_points = 1500.0f;  /* Super Helldive: 600 */
hell.spawner_cooldown = 1.0f;      /* Super Helldive: 1.5 s */
hell.icon = "skyeshade/hell_forge/icons/hell_forge"; /* images/hell_forge.png, built by hellforge-data.json */
hell.optional_objectives = 8;      /* Super Helldive: 4 */
hell.major_objectives = 2;         /* Super Helldive: 1 */
hell.outpost_scale = 3.0f;         /* three times Super Helldive's enemy outposts (the director handles 128 spawners) */
hell.mission_minutes = 72.0f;      /* every mission's countdown (Super Helldive: mostly 40 minutes) */
hell.map_scale = 2.0f;             /* twice Super Helldive's map radius, mega cities included */
hell.patrol_frequency = 2.0f;      /* roaming patrols twice as often */
hell.patrol_cap = 1.5f;            /* and up to 1.5 times as many living patrol enemies */
hell.mission_stratagem = HF_STRATAGEM_M102_GUNNER_FRV; /* every mission grants the M-102 Gunner FRV (no loadout slot) */
hell.flags = HF_DIFFICULTY_SECOND_PRIMARY | HF_DIFFICULTY_NO_ERADICATE | HF_DIFFICULTY_NO_RAPID_ACQUISITION | HF_DIFFICULTY_RADAR |
    HF_DIFFICULTY_NO_BLITZ | HF_DIFFICULTY_CITY_STRONGHOLDS | HF_DIFFICULTY_CITY_ICBM;
return api->add_difficulty(mod, &hell) == 11 ? 0 : 1;
```

### Enemies

`encounter_points` is the spawn budget of patrols, drops and breaches, `guardforce_points` the budget of the map's
guards, and `spawner_cooldown` the time between the shared spawners' waves. An enemy group holds at most 32 enemies
(more crashes the game), so bigger budgets mean more groups.

### Missions

- `optional_objectives` (0 to 32) and `major_objectives` (0 to 8) are together at most 18.
- `outpost_scale` is 0 to 8, relative to level 10's outposts.
- `mission_minutes` is 1 to 75.
- `map_scale` is up to 2.
- `patrol_frequency` and `patrol_cap` are up to 4 each.
- `mission_stratagem` is a stratagem every player gets in missions at the level.

0 keeps level 10's value, except for `mission_minutes` (0 keeps the mission's own countdown) and `mission_stratagem`
(0 grants none).

### Flags

`HF_DIFFICULTY_SECOND_PRIMARY` (every mission is two in one), `_NO_ERADICATE`, `_NO_RAPID_ACQUISITION`, `_NO_BLITZ`,
`_RADAR`, `_CITY_STRONGHOLDS` and `_CITY_ICBM`. `hellforge_mod.h` says what each does.

### Icon

`icon` is optional, built by your data spec with `"prepare": "image"` (`examples/mods/hell-forge/hellforge-data.json`).
Without one, the level shows Super Helldive's.

### Players

Players choose the level on the war table past Super Helldive; holding Super Helldive's unlock unlocks it. A successful
mission pays 1800 XP (and 50 per objective) and 2200 requisition. Friends need the same mod to join.

After the level exists, `difficulty_row(11)` is its settings row ([settings rows](#settings-rows)). `hell-swarm` is a
stress test of its rows.

## Settings rows

`difficulty_row`, `stratagem_row`, `projectile_row` and `damage_row` return rows the generated structs describe
(`HfDifficultySettings`, `HfStratagemSettings`, `HfProjectileSettings`, `HfDamageInfo`). `examples/mods/hell-swarm`
rewrites level 11's row once the entity data is ready:

```c
static void Swarm(HfMod* mod, HfEvent event, void* user)
{
    HfDifficultySettings* level = (HfDifficultySettings*)HF_API(mod)->difficulty_row(11);
    if (level == NULL)
    {
        HF_API(mod)->log(mod, "no difficulty 11 (is Hell Forge installed and enabled?); nothing changed");
        return;
    }
    level->encounter_points = 6000.0f;  /* Super Helldive 600 */
    level->guardforce_points = 6000.0f; /* 600 */
    level->spawner_cooldown = 0.25f;    /* 1.5 s */
    for (int faction = 0; faction < 3; faction++)
    {
        level->patrol_interval[faction] = 8.0f; /* 65 / 78 / 75 s */
        level->patrol_cap[faction] = 400;       /* 41 / 39 / 48 */
    }
}
```

These are direct writes: not checked and not reported as conflicts
([concepts](concepts.md#three-ways-to-change-the-games-data)). Keep each row's ids as they are (the header says which
members must stay). For a vanilla row, prefer `hf_set` or a [live edit](runtime.md#live-edits), which find the row by id
and check the change.

## Finding content

- Vanilla content: the constants in `hellforge_content.h` (`HF_ENTITY_...`, `HF_ITEM_...`, `HF_STRATAGEM_...`; every
  name is in the [reference](../sdk-reference.md#content)).
- At run time: `hf->find(HF_KIND_ENTITY, "Pelican Transport")`, `find(HF_KIND_TRAIT, "Anti-Tank")`, or another mod's
  content by its identity (`"yourname.pelican-support:pelican_gatling"`). Use it for names known only at run time (a
  config file, another mod's content); code that knows the content uses the constants.
- Everything mods added: `HfContent out = {sizeof out}; hf->content(mod, index, &out)` (SDK 7; it returns 0 without
  `size`), complete from `HF_EVENT_GAME_LOADED`.
- `hf->resource_hash("yourname/your_mod/bolt")` is the 64-bit id the game's data stores for a resource name.

## Where to go next

Next in the guide: [logic at run time](runtime.md), where your mod reacts to what happens in the game, makes the game do
things, and changes values while it runs.

In the [SDK reference](../sdk-reference.md): the calls on this page under [API](../sdk-reference.md#api) (SDK 2 to 8),
every field under [components](../sdk-reference.md#components) and
[settings tables](../sdk-reference.md#settings-tables), and every vanilla constant under
[content](../sdk-reference.md#content).
