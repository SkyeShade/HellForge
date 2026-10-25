/* HellForge mod SDK: the interface between HellForge and a code mod (HF_MOD_SDK_VERSION below; docs/modding/README.md).
 *
 * A code mod is a Windows x64 DLL in its mod folder, named by "code": {"dll": "...", "sdk": <the SDK it needs>} in
 * hellforge-mod.json. HellForge loads it into the copied game at the start of a session and calls its exported
 *
 *     HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod);
 *
 * once, on the loader's thread, while the game is still starting; mods are set up in the order of their ids (the same on
 * every machine), and their edits apply in the Mods page's load order (its bottom mod first). Return 0 to stay loaded;
 * anything else marks the mod not loaded (modloader.log says so; what it queued before still applies). Keep the api
 * pointer: it is valid for the whole session. Register for events to act once the game's code and data exist; load
 * events are delivered on the loader's watch thread, in the order mods registered, never two at once.
 *
 * Everything runs inside HellForge's own local copy only; the normal game is never touched. The loader does not catch a mod's crashes; the session log names the mod that was running. */
#ifndef HELLFORGE_MOD_H
#define HELLFORGE_MOD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HF_MOD_SDK_VERSION 15
#define HF_MOD_EXPORT __declspec(dllexport)

typedef struct HfMod HfMod; /* Identifies the calling mod in logs and status. Opaque, but for SDK 6's HF_API(mod). */

typedef enum HfEvent
{
    /* game.dll is loaded and its code is ready: functions can be found and hooked (not before). */
    HF_EVENT_GAME_LOADED = 1,
    /* The engine's network configuration is loaded: network field types can be read and widened. */
    HF_EVENT_NETWORK_READY = 2,
    /* The game's entity component data is loaded, with the weapons mods added: component records can be found and
     * edited. Fires again if the game reloads that data, so edits can be renewed. */
    HF_EVENT_ENTITIES_READY = 3,
    /* SDK 3: the game's projectile and damage lists are loaded, with every projectile and damage type mods added
     * (add_projectile, add_damage): projectile_row and damage_row return their rows from now on. */
    HF_EVENT_PROJECTILES_READY = 4,
} HfEvent;

typedef void (*HfEventCallback)(HfMod* mod, HfEvent event, void* user);
/* SDK 6, on_game_thread: called every game update on the game thread, dt in seconds. */
typedef void (*HfTickCallback)(HfMod* mod, float dt, void* user);

/* SDK 6, load_package's states. */
#define HF_PACKAGE_LOADING 1
#define HF_PACKAGE_RESIDENT 2

/* A new arsenal weapon (SDK 2, add_weapon): a copy of an existing one with a model, a name and, optionally, stats of
 * its own. Zero-initialize it and set `size`, so fields added later read as unset:
 *
 *     HfWeapon gold = {sizeof gold};
 *     gold.base = HF_ITEM_AR23_LIBERATOR;
 *     gold.unit = "my_name/my_mod/assault_rifle_gold";
 *     gold.designation = "AR-23G";
 *     gold.name = "Liberator Gold";
 *     uint32_t id = api->add_weapon(mod, &gold); */
typedef struct HfWeapon
{
    uint32_t size;           /* sizeof(HfWeapon) */
    uint32_t base;           /* the weapon it copies: a vanilla loadout id (HF_ITEM_..., or the session's arsenal.tsv) */
    /* Its model: a unit resource the mod's data provides (a copy of the base's unit with other materials, say). The name
     * also identifies the weapon: its entity is the name's hash and its loadout id the top 32 bits of that, so every
     * player's game agrees. Keep it unique, prefixed with your name and mod. */
    const char* unit;
    /* Its text, shown wherever the game names it (the armory shows "<designation> <NAME>"). UTF-8, one language for
     * every game language. NULL keeps the base's text: the name is required to give it text of its own. */
    const char* designation; /* "AR-23G" */
    const char* name;        /* "Liberator Gold" (an upper-case copy is made for the places that use one) */
    const char* description; /* may be NULL */
    /* Components the weapon gets records of its own in, NULL-terminated ({"WeaponMagazineComponentData", NULL}), each a
     * copy of the base's: after HF_EVENT_ENTITIES_READY, component_record(name, weapon_entity(id), ...) finds them, and
     * changing them changes this weapon only. Any other component's record stays shared with the base (component_record
     * refuses shared records, so the base can't be changed by mistake). NULL: none. */
    const char* const* own_components;
    /* SDK 3: the projectile it fires (a vanilla type, or one add_projectile returned; 0: its base's). Its own
     * ProjectileWeapon record names it and its default ammunition is removed, so it fires it like any vanilla weapon
     * without ammunition attachments (no ammunition can set its projectile back). */
    uint32_t projectile;
    /* SDK 3: a vanilla weapon (its loadout id, e.g. HF_ITEM_LAS16_SICKLE) whose firing sounds it makes, or 0 for its
     * base's: its own ProjectileWeapon record takes that weapon's sound events. Their sound banks must load with it: the
     * mod's data puts them in the weapon's archive patch (examples/mods/talon-liberator). */
    uint32_t sound_from;
} HfWeapon;

/* A new entity of any kind (SDK 4, add_entity): a copy of a vanilla entity (a Pelican, a turret...) under a resource
 * name of its own, sharing every component record of its base except the ones it owns. No loadout, armory or catalogue
 * entry (add_weapon is for arsenal weapons). Zero-initialize it and set `size`:
 *
 *     static const char* const own[] = {"MountComponentData", "BehaviorComponentData", NULL};
 *     HfEntity pelican = {sizeof pelican};
 *     pelican.base = HF_ENTITY_PELICAN_TRANSPORT;
 *     pelican.name = "my_name/my_mod/pelican";
 *     pelican.own_components = own;
 *     uint64_t entity = api->add_entity(mod, &pelican);
 *
 * After HF_EVENT_ENTITIES_READY, component_record(name, entity, ...) finds its own records (and the base's for the
 * rest), to set once: which turret its mount carries, which behaviour it runs, its weapon's round... */
typedef struct HfEntity
{
    uint32_t size;                      /* sizeof(HfEntity) */
    uint64_t base;                      /* the vanilla entity resource it copies (HF_ENTITY_...) */
    const char* name;                   /* unique: "my_name/my_mod/entity"; the entity's resource id is this name's hash */
    const char* unit;                   /* a unit resource for its model (the mod's data provides it); NULL: the base's */
    const char* const* own_components;  /* NULL-terminated component names it gets records of its own in, or NULL */
    uint32_t behaviour;                 /* the behaviour it runs (a vanilla id, or add_behaviour's), or 0 for the base's */
    /* 0, or a vanilla entity whose model it takes, with every record tied to that model (the parts of the model they
     * name: thrusters, effects, collision, seats; the packages that load it): Unit, LoadoutPackage, ThrusterGroup,
     * Boids, OverlapDamage, EffectReference, Vehicle, HitReact, VehicleMotion. A model under another model's records
     * hangs the game when it spawns. HF_ENTITY_PELICAN_GUNSHIP gives a Pelican the extraction Pelican's model. */
    uint64_t model_from;
    /* SDK 7: 1 gives it a record of its own in every component of its base (a copy of the base's), so it shares nothing
     * with it: an edit of the base (a stats mod, the editor) never changes it, and its own edits never change the base.
     * Its log names any component HellForge could not copy (record size unknown), which it still shares. */
    uint32_t own_all;
    /* SDK 7: its text (its own encyclopedia record), NULL for the base's. The game names an entity "<designation>
     * <title>": a sentry's stratagem shows its payload's designation before the stratagem's title (A/MG-43 ...). */
    const char* title;        /* "Heavy MG Sentry" */
    const char* designation;  /* "A/MG-101" */
    const char* description;
} HfEntity;

/* A new behaviour (SDK 4, add_behaviour): a vanilla behaviour extended by HellForge, under an id of its own (694 and
 * up) that the game's behaviour system runs like its own. An entity runs it through HfEntity.behaviour. Bases:
 *   HF_BEHAVIOUR_PELICAN_TRANSPORT  flies to a point on its orbit around the beacon, holds there for hold_seconds
 *                                   instead of 6, circling the beacon and turning its body toward its turret's target,
 *                                   then departs;
 *   HF_BEHAVIOUR_PELICAN_TURRET     the chin turret, choosing its targets as HD2Runtime's Pelican Gatling did (only
 *                                   what it sees, letting go of what it cannot see or hurt) and firing for
 *                                   burst_seconds (vanilla: 0.5) or, with 0, as long as it keeps a target.
 *
 *     HfBehaviour support = {sizeof support};
 *     support.base = HF_BEHAVIOUR_PELICAN_TRANSPORT;
 *     support.hold_seconds = 90.0f;
 *     support.orbit_radius = 40.0f;
 *     support.orbit_height = 30.0f;
 *     support.face_target = 1;
 *     uint32_t id = api->add_behaviour(mod, &support);
 */
typedef struct HfBehaviour
{
    uint32_t size;          /* sizeof(HfBehaviour) */
    uint32_t base;          /* the vanilla behaviour it extends */
    float hold_seconds;     /* how long it holds at the beacon (0: the base's) */
    float orbit_radius;     /* metres around the beacon while it holds (0: it hovers in place) */
    float orbit_height;     /* metres above the beacon while it orbits */
    uint32_t face_target;   /* 1: it turns its body toward its mounted turret's target while it holds */
    float burst_seconds;    /* a turret's bursts (0: it fires as long as it keeps a target) */
    uint32_t reserve_rounds; /* a turret's rounds beyond its magazine, refilled into it before it runs dry (it never
                              * reloads): its magazine's capacity (at most 2047, the game's network limit) + this */
    float aim_height;        /* a turret aims this high above its target's feet (0: at the game's aim point for that
                              * enemy, about 1.5 m up; from above, rounds aimed there overshoot) */
} HfBehaviour;

/* A new damage type (SDK 3, add_damage): a copy of a vanilla DamageInfo row (standard and durable damage, armour
 * penetration, status effects) under an id of its own. Zero-initialize it and set `size`:
 *
 *     HfDamage heavy = {sizeof heavy};
 *     heavy.base = HF_DAMAGE_LAS58_TALON;
 *     heavy.name = "my_name/my_mod/heavy_bolt";
 *     heavy.scale = 5.0f;
 *     uint32_t damage = api->add_damage(mod, &heavy);
 *
 * Projectiles deal it through HfProjectile.damage. The game's damage list has no spare room, so HellForge moves it to
 * a larger copy (docs/modding/content.md "Projectiles and damage"); ids are numbered in the mods' set-up order (by mod
 * id), the same in every game. */
typedef struct HfDamage
{
    uint32_t size;  /* sizeof(HfDamage) */
    uint32_t base;  /* the vanilla damage id it copies (1 .. 649; HF_DAMAGE_...) */
    const char* name;
    float scale;    /* x its standard and durable damage (0 means 1); the rest stays the base's */
} HfDamage;

/* A new stratagem (SDK 4, add_stratagem): a copy of a vanilla stratagem's settings under a StratagemType of its own,
 * with its own name, description, input code and icon. What it does when called (its payload, beacon and the packages
 * it loads) stays the base's, except its ship-upgrade (level) packages. Zero-initialize it and set `size`:
 *
 *     HfStratagem gatling = {sizeof gatling};
 *     gatling.base = HF_STRATAGEM_ORBITAL_PRECISION_STRIKE;
 *     gatling.name = "my_name/my_mod/pelican_gatling";
 *     gatling.title = "Pelican Gatling Support";
 *     gatling.description = "Calls down a Pelican for close air support.";
 *     gatling.code = "LDLLUU";
 *     gatling.icon = "my_name/my_mod/icons/pelican_gatling";
 *     uint32_t type = api->add_stratagem(mod, &gatling);
 *
 * The game's stratagem list has no spare room, so HellForge moves it to a larger copy (docs/internals.md
 * "Stratagems"); types are numbered in the mods' set-up order (by mod id), so every game numbers them alike. Players
 * own it through the local backend (it joins the catalogue and inventory for the session) and pick it in the loadout like
 * any other. */
/* An entity a stratagem's editor page shows, and the vanilla entity whose fields it shows for it (HfStratagem). */
typedef struct HfEditorEntity
{
    uint64_t entity;  /* an entity the mod added (add_entity's, register_entity's) */
    uint64_t like;    /* the vanilla entity on the page it takes (HF_ENTITY_...) */
} HfEditorEntity;

/* SDK 8: a heading in the game's stratagem selection screen. Register during mod init, before its stratagems.
 * At most 28 custom headings across all mods; empty headings are hidden. Names use the usual modid:name identity.
 * Headings follow the vanilla sections, ordered by sort_order then identity. This does not change gameplay colours. */
typedef struct HfStratagemCategory
{
    uint32_t size;            /* sizeof(HfStratagemCategory) */
    const char* name;         /* short name, or a fully qualified identity shared by cooperating mods */
    const char* title;        /* heading players see; required, UTF-8, under 192 bytes */
    int32_t sort_order;       /* lower comes first; 0 is the default */
} HfStratagemCategory;

typedef struct HfStratagem
{
    uint32_t size;            /* sizeof(HfStratagem) */
    uint32_t base;            /* the vanilla StratagemType it copies (HF_STRATAGEM_...; a selectable one) */
    const char* name;         /* unique: "my_name/my_mod/stratagem"; its identity (its stable id is the name's 32-bit id) */
    const char* title;        /* the name players see ("Pelican Gatling Support"); NULL: the base's */
    const char* description;  /* NULL: the base's */
    const char* code;         /* its input: 1 to 9 of U, D, L, R ("LDLLUU"); NULL: the base's. Avoid other codes' prefixes */
    const char* icon;         /* the resource name of its icon (a texture and a GUI material of that name in the mod's
                               * data, in the stratagem icons archive 007e093ca718ca1a); NULL: the base's */
    float cooldown;           /* seconds; 0: the base's */
    int32_t uses;             /* 0: the base's; -1: unlimited */
    /* What a call spawns, or 0 for the base's: entity resources (add_entity's, or vanilla). For a Pelican drop (a base of
     * delivery kind 3, like HF_STRATAGEM_EXO45_PATRIOT): {cargo, vehicle}; {0, pelican} sends an empty Pelican. */
    const uint64_t* payload;
    uint32_t payload_count;   /* 1 .. 8 */
    /* A package it loads for the mission (row +0xA8; HF_PACKAGE_...), ~0ull for none, or 0: with a payload (SDK 6), the
     * package its payload entity loads (its LoadoutPackage record, or its model's), else the base's. */
    uint64_t package;
    uint32_t loads_item;      /* a loadout item (HF_ITEM_...) whose package it also loads (row +0xF8), as support weapons do */
    uint32_t category;        /* its loadout section and ping colour: HF_CATEGORY_..., or 0 for the base's */
    uint32_t beam;            /* its beacon's beam: HF_BEAM_..., or 0 for the base's */
    uint32_t flags;           /* its traits (row +0x104): HF_TRAIT_... ORed together, or 0 for the base's. HF_TRAIT_VEHICLE
                               * makes it a vehicle (one per loadout); orbital strikes have ORBITAL | EXPLOSIVE | ANTI_TANK */
    /* SDK 7: its page in the in-game editor. It takes a vanilla stratagem's page (editor_like, 0: its base's) and points
     * every field at the stratagem's own: its own settings row, and for each vanilla entity on that page the entity
     * of the mod standing for it, with that entity's own projectile and damage where they are mod copies of the page's.
     * Fields with nothing of the mod's to point at are left out (the base's are never edited through it). The entities
     * standing in: editor_entities, ending in {0}; NULL: each payload entity for the vanilla entity it copies. */
    uint32_t editor_like;
    const HfEditorEntity* editor_entities;
    /* SDK 8: add_stratagem_category's handle, or 0 for the vanilla section. UI grouping only: category above still
     * controls the native HUD/ping colours. Handles are session-local; never save them. */
    uint32_t selection_category;
    /* SDK 8: the stratagem's place under its selection_category heading: lower comes first; equal orders (0 by
     * default) follow the stratagem types: the mods' set-up order (by mod id), then add_stratagem call order. Ignored in
     * vanilla sections. */
    int32_t selection_order;
} HfStratagem;

/* HfStratagem.category, beam and flags: HF_CATEGORY_..., HF_BEAM_... and HF_TRAIT_... (hellforge_content.h). */

/* A new projectile type (SDK 3, add_projectile): a copy of a vanilla projectile's settings under a type number of its
 * own, so changing it changes no vanilla projectile. Zero-initialize it and set `size`:
 *
 *     HfProjectile bolt = {sizeof bolt};
 *     bolt.base = HF_PROJECTILE_LAS58_TALON;
 *     bolt.name = "my_name/my_mod/bolt";
 *     bolt.damage = api->add_damage(mod, &heavy);
 *     uint32_t type = api->add_projectile(mod, &bolt);
 *
 * A weapon fires it through HfWeapon.projectile. Every player's game must run the same mods: the modlist gate ensures
 * it, and types are numbered in the mods' set-up order (by mod id), so every game numbers them alike. */
typedef struct HfProjectile
{
    uint32_t size;          /* sizeof(HfProjectile) */
    uint32_t base;          /* the vanilla ProjectileType it copies (1 .. 350; HF_PROJECTILE_...) */
    const char* name;       /* names it in the logs; unique, prefixed with your name and mod */
    uint32_t damage;        /* the damage id its hits deal (vanilla, or one add_damage returned; 0: the base's) */
    /* Its look: 0 keeps the base's spawn effects; a vanilla type takes that type's (row +0x48 .. +0x6F: the effects and
     * their parameters), for example a projectile whose effects are already loaded with the weapon. */
    uint32_t visual_from;
    /* 0, or a particles resource for its main effect (row +0x48: the LAS-58 Talon's bolt itself), applied after
     * visual_from: resource_hash of its name. The mod's data must load it, and everything it uses, with the weapon that
     * fires it (a patch of that weapon's archive; examples/mods/talon-liberator). */
    uint64_t spawn_effect;
    /* 0, or a particles resource for its second effect (row +0x70: the LAS-58 Talon's light and trail; the Liberator
     * has none): resource_hash of its name, loaded with the weapon like spawn_effect. */
    uint64_t flight_effect;
} HfProjectile;

/* A network field type: kind 1 is an integer of `bits` bits starting at `min` (range min .. min + 2^bits - 1). */
typedef struct HfFieldType
{
    int32_t kind;
    int32_t bits;
    int32_t min;
} HfFieldType;

/* A new difficulty level (SDK 5, add_difficulty): level 11, chosen on the war table past Super Helldive. A copy of level
 * 10's settings (enemy mix, packages, mission rules) with its own name, unlock item, enemy budgets, map size and
 * objectives. Players who have unlocked level 10 have it unlocked too. It has its own place in the difficulty bar, and
 * its own enemy tech level (11): every enemy's level-11 spawn weight starts as its level-10 weight.
 *
 *     HfDifficulty hell = {sizeof hell};
 *     hell.name = "my_name/my_mod/hell_forge";
 *     hell.title = "Hell Forge";
 *     hell.encounter_points = 1500.0f;
 *     uint32_t level = api->add_difficulty(mod, &hell);   (11, or 0) */
typedef struct HfDifficulty
{
    uint32_t size;               /* sizeof(HfDifficulty) */
    const char* name;            /* unique, resource-style: its unlock item and text ids are hashed from it */
    const char* title;           /* the name players see */
    const char* description;     /* unused: the game shows no difficulty description (the title shows in capitals
                                    on the war table; the mission's top bar shows the level, "11") */
    float encounter_points;      /* spawn budget of patrols, drops and breaches (level 10: 600); 0 keeps level 10's */
    float guardforce_points;     /* budget of the map's guards (level 10: 600); 0 keeps level 10's */
    float spawner_cooldown;      /* seconds between the shared spawners' waves (level 10: 1.5); 0 keeps level 10's */
    const char* icon;            /* optional: an icon resource the mod's data spec builds ("icon" entries, as for
                                    stratagems); shown in the difficulty bar. NULL keeps Super Helldive's */
    /* Missions at this level (0 keeps level 10's). */
    float map_scale;             /* playable radius, relative to level 10's: up to 2. Above 1 the route cells are
                                    farther apart (the generator has room for level 10's number); the structured
                                    maps (map type 2) keep level 10's size */
    uint32_t optional_objectives;/* optional objectives a mission draws from its list (level 10: 4) */
    uint32_t major_objectives;   /* secondary objectives (level 10: 1); optional + major at most 18 (the map
                                    generator places 30 objectives at this level, the missions' own among them) */
    float outpost_scale;         /* enemy outposts of each size, relative to level 10's (rounded) */
    float mission_minutes;       /* every mission's countdown at this level, 1 to 75 (level 10: the mission's own,
                                    mostly 40); 0 keeps the mission's own */
    uint32_t flags;              /* HF_DIFFICULTY_*: */
    float patrol_frequency;      /* how often roaming patrols come, relative to level 10's (their interval, row +0x2A0..
                                    per faction, is divided by it); 0 keeps level 10's; up to 4 */
    float patrol_cap;            /* the most living patrol enemies, relative to level 10's (row +0x2AC.., per faction;
                                    the HiveMind scales it further by progress and players); 0 keeps level 10's; up to 4 */
    uint32_t mission_stratagem;  /* a stratagem type every mission at the level grants every player, in the mission group
                                    with Reinforce and Resupply (no loadout slot; the type's own uses and cooldown), e.g.
                                    105, the M-102 Fast Recon Vehicle; 0 none */
} HfDifficulty;
/* Every mission at the level is two missions in one: its own plus a partner mission of the same faction that the game
 * offers alongside it, with the partner's main objectives and their steps as the game selects them (chosen with the
 * mission's seed; never main objectives the mission has already). Every main objective of both must be done. */
#define HF_DIFFICULTY_SECOND_PRIMARY 0x1u
/* Eradicate missions are not offered at the level (their kill objectives can still be second main objectives). */
#define HF_DIFFICULTY_NO_ERADICATE 0x2u
/* Rapid Acquisition missions are not offered at the level. */
#define HF_DIFFICULTY_NO_RAPID_ACQUISITION 0x4u
/* Every mission at the level has the radar station (an optional objective that reveals the map), marked on the map from
 * the start. */
#define HF_DIFFICULTY_RADAR 0x8u
/* Blitz missions (mission kind 6) are not offered at the level. */
#define HF_DIFFICULTY_NO_BLITZ 0x10u
/* City maps (mega cities) at the level get the strongholds (Automaton fortresses, Terminid mega nests) the level's
 * outpost counts ask for: city-only copies of the game's stronghold outposts (up to 100 m across), offered only at the
 * level. Open maps keep theirs; Illuminate has none. */
#define HF_DIFFICULTY_CITY_STRONGHOLDS 0x20u
/* Every city operation at the level starts with a Launch ICBM mission on the city map: three silos to launch, launch
 * codes to retrieve, power to restore and a refuelling station, beside the city's side objectives (three unused mission
 * rows of the game, one per faction, offered only at the level; city copies of the ICBM objectives' stamps). */
#define HF_DIFFICULTY_CITY_ICBM 0x40u

/* ---- SDK 6: named fields, definition blocks and deferred edits (docs/modding/content.md) ------------------------- */

/* Field types, as the mappings name them. */
#define HF_TYPE_U8 1u
#define HF_TYPE_U16 2u
#define HF_TYPE_U32 3u
#define HF_TYPE_I32 4u
#define HF_TYPE_F32 5u
#define HF_TYPE_U64 6u
#define HF_TYPE_BYTES 7u /* raw bytes the research has not split into values: copied, never set */

/* When a change to a field takes effect in a running game (each field's [tag] in hellforge_components.h; set_live's
 * result). */
#define HF_APPLIES_NOW 1u     /* the next time the game reads it: the next shot, hit or explosion */
#define HF_APPLIES_RESPAWN 2u /* when the entity is next made: a spawn, a re-equip, the next call-in */
#define HF_APPLIES_MISSION 3u /* when the next mission loads */

#ifdef __cplusplus
#define HF_ASSERT_SIZE(T, n) static_assert(sizeof(T) == (n), #T " is not its record's size")
#define HF_ASSERT_OFFSET(T, f, n) static_assert(offsetof(T, f) == (n), #T "." #f " is not at its offset")
#define HF_LITERAL(T) T
#else
#define HF_ASSERT_SIZE(T, n) _Static_assert(sizeof(T) == (n), #T " is not its record's size")
#define HF_ASSERT_OFFSET(T, f, n) _Static_assert(offsetof(T, f) == (n), #T "." #f " is not at its offset")
#define HF_LITERAL(T) (T)
#endif

/* Component records with named fields (HfProjectileWeapon, HfWeaponData...), generated from mappings/components.json. */
#include "hellforge_components.h"

/* An entity: its 64-bit resource id. A vanilla one (HF_ENTITY_...), or one register_entity or add_entity returned. */
typedef uint64_t HfRef;

/* A named field of a component record: HF_FIELD(WeaponData, recoil_up). The component and field are checked when the
 * mod compiles (a typo does not build); the loader checks the field against the layout it has for this game build and
 * reports one it does not know with the mod's source line, instead of writing anything. */
typedef struct HfField
{
    const char* component; /* the component record ("WeaponDataComponentData") */
    uint32_t layout;       /* the game build whose layout the offset comes from */
    uint16_t offset;
    uint16_t size;
    uint32_t type;         /* HF_TYPE_... */
    uint32_t line;         /* the mod's source line, for messages */
} HfField;
#define HF_FIELD(C, f) {Hf##C##_INFO, (uint16_t)offsetof(Hf##C, f), (uint16_t)sizeof(((Hf##C*)0)->f), HF_TYPE_##C##_##f, __LINE__}
/* One element of an array field, to set on its own: HF_FIELD_AT(ProjectileWeapon, fire_rate, 1). */
#define HF_FIELD_AT(C, f, i) {Hf##C##_INFO, (uint16_t)(offsetof(Hf##C, f) + (i) * sizeof(((Hf##C*)0)->f[0])), \
    (uint16_t)sizeof(((Hf##C*)0)->f[0]), HF_TYPE_##C##_##f, __LINE__}

/* A value for a field, tagged with its type: HF_F32(0), HF_U32(2000). The tag must be the field's type, and the value
 * must fit it (HF_U32(-1) is refused, not wrapped). */
typedef struct HfValue
{
    uint32_t type;   /* HF_TYPE_... */
    double number;   /* every type but u64 */
    uint64_t u64;
} HfValue;
#define HF_U8(x) {HF_TYPE_U8, (double)(x), 0}
#define HF_U16(x) {HF_TYPE_U16, (double)(x), 0}
#define HF_U32(x) {HF_TYPE_U32, (double)(x), 0}
#define HF_I32(x) {HF_TYPE_I32, (double)(x), 0}
#define HF_F32(x) {HF_TYPE_F32, (double)(x), 0}
#define HF_U64(x) {HF_TYPE_U64, 0.0, (uint64_t)(x)}

/* The local player's armor passives (set_passives, get_passives): passive ids as the game numbers them (mappings/passives.json;
 * 0 STANDARD ISSUE ... 41 TRUE GRIT), or HF_PASSIVE_KIT for what the armor and helmet worn give. */
#define HF_PASSIVE_KIT 0xFFFFFFFFu
typedef struct HfPassives
{
    uint32_t armor, second;           /* what the local player's record holds now */
    uint32_t kit_armor, kit_second;   /* what the kits worn give */
    uint32_t want_armor, want_second; /* the override (HF_PASSIVE_KIT: none) */
    uint32_t players;                 /* players in the game: an override holds only alone */
    int held;                         /* an override is held */
    char status[160];                 /* how it goes: "held", "solo only: ...", "waiting: ...", a refusal */
} HfPassives;

/* A field as get_live reads it. The base is the field without the Local Stats mod's change (mods/zz.local.stats, the
 * editors' save file) and without live edits: another mod's value, else the game's own. */
typedef struct HfLiveValue
{
    HfValue now;     /* what the record holds */
    HfValue base;
    const char* mod; /* the mod whose value the base is (its id), or NULL for the game's own */
} HfLiveValue;

/* A stats field as the stats files and the launcher's schema name it (get_stat): its kind ("component", a settings table
 * such as "DamageInfo", "EntityDelta", "BoosterSettings"), the component ("" for tables; "<component>:<member>" for an
 * entity delta), the entity resource, the record (a row id, or a delta's settings index), the offset and HF_TYPE_... */
typedef struct HfStat
{
    const char* kind;
    const char* component;
    uint64_t resource;
    int64_t record;
    uint32_t offset;
    uint32_t type;
} HfStat;

/* The session as co-op sees it (session): solo, hosting friends who play with this machine's stats, or joined to a
 * friend's lobby and playing with the host's (an editor shows them read-only). version changes with every change. */
#define HF_SESSION_SOLO 0u
#define HF_SESSION_HOST 1u
#define HF_SESSION_JOINER 2u
typedef struct HfSession
{
    uint32_t role;    /* HF_SESSION_... */
    uint32_t version;
    char host[64];    /* the host's name (UTF-8), when joined */
} HfSession;

/* SDK 15, stats_source: whose stats the lobby plays with. HF_STATS_HOST: its host shares its stats with friends (joined,
 * this game plays with the host's, and the in-game editor shows them read-only; hosting, friends play with this
 * machine's). HF_STATS_OWN: solo, or every player in the lobby keeps their own (the default: each machine's edits are its
 * own, and HellForge loads the packages friends' changes need). The session's version changes with it. */
#define HF_STATS_OWN 0u
#define HF_STATS_HOST 1u

/* SDK 12: HellForge's latest message for the player (notice), as the session's backend posts it: why a friend's lobby
 * refused this game's join, or why a friend could not join this one. A title and up to 8 lines (UTF-8, '\n' between
 * them); version changes with every new one, 0 before the first. The in-game editor shows each over the game: a brief
 * one (that a friend couldn't join this game's lobby) as a small note in the corner, its title and first line. */
#define HF_NOTICE_INFO 0u
#define HF_NOTICE_ERROR 1u
#define HF_NOTICE_BRIEF 2u
typedef struct HfNotice
{
    uint32_t version;
    uint32_t kind;    /* HF_NOTICE_... */
    char title[128];
    char text[1792];
} HfNotice;

/* ---- game events (SDK 7, subscribe) ------------------------------------------------------------------------------
 * What happens in the game, delivered on the game thread (engine calls are safe in a listener) to every listener of the
 * kind, highest priority first. A listener gets the event as an HfEventInfo and casts it to the kind's struct; the data
 * is valid during the call only. Events observe: changing their data changes nothing in the game. Every machine
 * raises its own: an event says what this machine's game did (see each kind for host and client).
 *
 *     static void Spawned(HfMod* mod, const HfEventInfo* event, void* user)
 *     {
 *         const HfSpawnEvent* spawn = (const HfSpawnEvent*)event;
 *         if (spawn->resource == mySentry) HF_API(mod)->log(mod, "sentry %u is down", spawn->entity);
 *     }
 *     hf->subscribe(mod, HF_ON_SPAWN, Spawned, NULL, 0); */
#define HF_ON_STATE 1          /* HfStateEvent: the game's state changed (HF_STATE_...) */
#define HF_ON_MISSION_START 2  /* HfStateEvent: the game entered HF_STATE_MISSION */
#define HF_ON_MISSION_END 3    /* HfStateEvent: the game left HF_STATE_MISSION (to: where it went) */
#define HF_ON_PLAYER_SPAWN 4   /* HfPlayerEvent: a player is on the ground (dropped in, or reinforced) */
#define HF_ON_PLAYER_DOWN 5    /* HfPlayerEvent: a player died and waits to be reinforced */
#define HF_ON_DAMAGE 6         /* HfDamageEvent: damage this machine's game applies (its own hits; another machine's
                                * targets get theirs from that machine), before armour and multipliers */
#define HF_ON_STRATAGEM 7      /* HfStratagemEvent: a stratagem's beacon activates (on the thrower's machine) */
#define HF_ON_KILL 8           /* HfKillEvent: something was killed (on the victim's owner; the kill also reaches the others) */
#define HF_ON_SPAWN 9          /* HfSpawnEvent: an entity was spawned in this machine's world (remote spawns too) */
/* SDK 11 */
#define HF_ON_HEAL 10          /* HfHealEvent: a Helldiver's health went up (any player's; a stim, a Supply Pack's, a booster's) */
#define HF_ON_STAT 11          /* HfStatEvent: the local player's mission stats grew (shots fired, hits, kills, damage dealt) */
#define HF_ON_WEAPON 12        /* HfWeaponEvent: what the local player's Helldiver holds changed */
#define HF_ON_EXPLOSION 13     /* HfExplosionEvent: an explosion was requested in this machine's world (its own simulation) */
#define HF_ON_BUGHOLE 14       /* HfBugholeEvent: a bug hole (any bug spawner) was closed, on every machine */
#define HF_ON_DAMAGE_PRE 15    /* HfDamagePreEvent: damage about to apply, as HF_ON_DAMAGE's; a listener may change it */

/* The game's states (HfStateEvent, game_state). */
#define HF_STATE_NONE 0u
#define HF_STATE_SPLASH 1u
#define HF_STATE_TITLE 2u
#define HF_STATE_SHIP 3u
#define HF_STATE_MISSION 4u
#define HF_STATE_PREPARE_SHIP 5u
#define HF_STATE_PREPARE_MISSION 6u

/* A new ability (SDK 7, add_ability): a vanilla ability that HellForge holds before it runs, with sounds of its own, under
 * an id of its own (2868 and up). An entity runs it where its data names an ability: an explosive's armed ability
 * (HF_FIELD(Explosive, armed_ability): what it runs once it is set off), an ability weapon's fire modes
 * (HF_FIELD(AbilityWeapon, mode0_ability): what it runs when it fires). The hold is a countdown: the base runs as it
 * would have, hold_seconds later. Sounds play in the world at the entity (a silo countdown's at the silo), heard by
 * whoever is near it (Wwise event ids; 0 for none), and their bank must be loaded: package names the one holding it.
 *
 * HF_ABILITY_HOLDS_LINKED_LAUNCH makes it a silo countdown, for a copy of a silo's targeting remote: firing the remote
 * holds the launch of the missile it is linked to for hold_seconds (the countdown runs on the missile, so it goes on
 * once the remote is tossed), then launches it at the point the remote's laser marked last. While the laser stays on,
 * the launched missile follows it, as the silo's does. No base.
 *
 *     HfAbility countdown = {sizeof countdown};
 *     countdown.hold_seconds = 10.0f;
 *     countdown.flags = HF_ABILITY_HOLDS_LINKED_LAUNCH;
 *     uint32_t id = api->add_ability(mod, &countdown);
 *     // the remote copy: {HF_FIELD(AbilityWeapon, mode0_ability), HF_U32(id)}, likewise mode1_ability */
#define HF_ABILITY_SILO_LAUNCH 2088u /* the MS-11 Solo Silo missile's armed ability: its flight, once launched */
#define HF_ABILITY_HOLDS_LINKED_LAUNCH 0x1u /* HfAbility.flags: a targeting remote's countdown before its missile launches */
typedef struct HfAbility
{
    uint32_t size;          /* sizeof(HfAbility) */
    uint32_t base;          /* the vanilla ability it runs after its hold (HF_ABILITY_...), or 0: done when the hold ends */
    float hold_seconds;     /* 0 .. 600 */
    uint32_t start_event;   /* posted when it is set off */
    uint32_t second_event;  /* posted every whole second of the hold after the first */
    uint32_t final_event;   /* posted when the hold ends and the base starts */
    uint64_t package;       /* the package holding the sounds' bank, loaded for the session (a catalogue package), or 0 */
    uint32_t flags;         /* HF_ABILITY_... flags (HellForge 2026-10-18; older mods leave it out) */
} HfAbility;

/* Content mods added (SDK 7, content), for tools: the in-game editor gives each stratagem a page. Strings live as long
 * as the session. */
typedef struct HfContent
{
    uint32_t size;          /* sizeof(HfContent): set by the caller */
    uint32_t kind;          /* HF_KIND_STRATAGEM, HF_KIND_ENTITY, HF_KIND_PROJECTILE or HF_KIND_DAMAGE */
    uint64_t id;            /* its stratagem type, entity resource, projectile type or damage id */
    uint64_t base;          /* the vanilla one of its kind it copies */
    uint64_t link;          /* a projectile: the damage it deals (0: its base's) */
    const char* mod;        /* the id of the mod that added it, and that mod's folder (UTF-8) */
    const char* folder;
    const char* name;       /* its identity */
    const char* title;      /* a stratagem's or entity's name for players, or NULL for its base's */
    const char* icon;       /* a stratagem's icon resource name, or NULL for its base's */
    uint64_t editor_like;   /* a stratagem: the stratagem whose editor page it takes */
    const HfEditorEntity* editor_entities; /* a stratagem: the entities its page shows, ending in {0} */
    const char* designation; /* an entity's designation (HfEntity.designation), or NULL for its base's */
    uint32_t category;       /* a stratagem's HF_CATEGORY_... (HfStratagem.category), or 0 for its base's */
} HfContent;

typedef struct HfEventInfo
{
    uint32_t size;   /* the kind's struct size */
    uint32_t kind;   /* HF_ON_... */
    uint64_t update; /* the game update it was delivered in (counted from the session's start) */
} HfEventInfo;
typedef void (*HfListener)(HfMod* mod, const HfEventInfo* event, void* user);

typedef struct HfStateEvent { HfEventInfo info; uint32_t from, to; } HfStateEvent;
typedef struct HfPlayerEvent
{
    HfEventInfo info;
    uint64_t peer;    /* the player's peer id: who it is (the player list's order changes as players leave) */
    uint32_t entity;  /* the player's entity id (this session's), 0 when not known */
    uint32_t local;   /* 1: a player of this machine */
} HfPlayerEvent;
/* Entity ids in events are the session's entity instances (index: the low 22 bits), not resource ids. */
typedef struct HfDamageEvent
{
    HfEventInfo info;
    uint32_t target;      /* the entity hit */
    uint32_t kind;        /* 0 projectile, 1 ricochet, 2 damage over time, 3 single-shot beam, 4 explosion, 5 impact,
                           * 6 ability, 7 flat, 8 drowning, 9 bleeding out */
    uint32_t element;
    uint32_t dealer;      /* the network ids of what dealt it and its owner (0x7FFF: none) */
    uint32_t owner;
    int32_t amount;       /* before the target's armour and multipliers; -1 kills */
    uint64_t creditor;    /* the peer credited with it (0: none) */
    float position[3];    /* where it hit */
} HfDamageEvent;
typedef struct HfStratagemEvent
{
    HfEventInfo info;
    uint32_t type;        /* its StratagemType (HF_STRATAGEM_..., or add_stratagem's) */
    uint32_t thrower;     /* the thrower's player entity (0xFFFFFFFF: none) */
    float position[3];    /* the beacon */
} HfStratagemEvent;
typedef struct HfKillEvent
{
    HfEventInfo info;
    uint32_t victim, dealer; /* entities (dealer 0xFFFFFFFF: none, a death over time) */
    uint64_t creditor;       /* the peer credited with it (0: none) */
    uint32_t kind, element;  /* as HfDamageEvent's */
} HfKillEvent;
typedef struct HfSpawnEvent
{
    HfEventInfo info;
    uint32_t entity;      /* its entity id */
    uint32_t network;     /* its network id */
    uint64_t resource;    /* what it is: its entity resource (HF_ENTITY_..., add_entity's or register_entity's) */
    uint32_t authority;   /* 1: this machine simulates it */
} HfSpawnEvent;

/* SDK 11 events. A heal: the avatar's health record grew between two game updates (alive or downed; a heal and a hit in
 * the same update show as their sum). Every player's Helldiver, as this machine's game holds it. */
typedef struct HfHealEvent
{
    HfEventInfo info;
    uint32_t entity;      /* the Helldiver (avatar) entity */
    int32_t amount;       /* health gained */
    int32_t health;       /* health now */
    int32_t max_health;   /* its maximum, or -1 when not known */
    uint64_t peer;        /* the player it belongs to (0: none found) */
    uint32_t local;       /* 1: this machine's player */
} HfHealEvent;
/* The local player's mission stats, as the game keeps them: a total and a part per source (the weapon, throwable or
 * stratagem payload it recorded the stat under, by entity resource). Checked 10 times a second; one event per stat that
 * grew. A shotgun counts each pellet; a hit is a projectile that hit (a knife's is damage only). */
#define HF_STAT_SHOTS 0x565790DAu  /* projectiles fired */
#define HF_STAT_HITS 0x897A5551u   /* projectiles that hit */
#define HF_STAT_KILLS 0xD494C5CCu  /* kills credited */
#define HF_STAT_DAMAGE 0x5C7A2930u /* damage dealt (after armour; team damage not counted) */
#define HF_STAT_SOURCES 5
typedef struct HfStatSource { uint64_t resource; int32_t added; uint32_t reserved; } HfStatSource;
typedef struct HfStatEvent
{
    HfEventInfo info;
    uint32_t stat;        /* HF_STAT_... */
    int32_t added;        /* growth since the last check */
    int32_t total;        /* the stat now */
    int32_t unattributed; /* growth of the part with no source */
    uint32_t player;      /* the local player's player entity (not its Helldiver) */
    uint32_t count;       /* sources[] used: the largest growths first, at most HF_STAT_SOURCES */
    HfStatSource sources[HF_STAT_SOURCES];
} HfStatEvent;
/* What the local Helldiver holds (its hand: a weapon, a support weapon, a held item), checked 10 times a second. 0 for
 * nothing: before the first item after a respawn, and when the Helldiver is gone. The first check sets the start. */
typedef struct HfWeaponEvent
{
    HfEventInfo info;
    uint32_t avatar, previous_avatar;     /* the Helldiver now and at the last check (0: none) */
    uint32_t entity, previous;            /* the held entity now and before (0: nothing) */
    uint64_t resource, previous_resource; /* their entity resources */
    uint32_t selection;                   /* the inventory selection: 1 primary, 2 secondary, 3 support, 4/5 held item, 0 none */
    uint32_t previous_selection;
} HfWeaponEvent;
/* An explosion request (every one: projectile impacts, explosives, stratagems, chained blasts, mods'). */
typedef struct HfExplosionEvent
{
    HfEventInfo info;
    uint32_t type;        /* its ExplosionSettings row (HF_EXPLOSION_... or a mod's) */
    uint32_t source;      /* the entity it comes from (an explosive, a projectile's weapon; 0: none) */
    uint32_t owner;       /* that entity's owner (0: none) */
    uint32_t reserved;
    uint64_t creditor;    /* the peer credited with it (0: none) */
    float position[3];
} HfExplosionEvent;
/* A bug hole closed: its destruction stage left 0. Holes already closed when HellForge first sees them are not told. */
typedef struct HfBugholeEvent
{
    HfEventInfo info;
    uint32_t entity;      /* the hole's entity (it stays after closing) */
    uint32_t unit;
    uint64_t resource;    /* its bug_spawner_* entity resource */
    uint32_t stage;       /* the stage it reached (1 to 3) */
    uint32_t owned;       /* 1: this machine owns it (the host) */
    float position[3];
    uint32_t has_position;
} HfBugholeEvent;
/* Damage before it applies, on the machine applying it (HF_ON_DAMAGE's moment, the same fields). The one event a listener
 * may write to: set damage.amount (before armour and multipliers; -1 kills), or cancel (1: none of its amount applies;
 * the hit's other effects, a status or a push, may still). Listeners see the value earlier ones left. */
typedef struct HfDamagePreEvent
{
    HfDamageEvent damage; /* damage.info.kind is HF_ON_DAMAGE_PRE */
    uint32_t cancel;
} HfDamagePreEvent;

/* SDK 11 actions: what a mod makes the game do, through the game's own functions (HD2Runtime 0.40.1's research), on the
 * game thread (an event listener's or on_game_thread's call; from another thread an action waits for the next game
 * update and answers HF_ACT_QUEUED). Each answers HF_ACT_..., or 0 refused (last_log says why). The local Helldiver:
 * this machine's player's, alive (not downed). Host: the session's host in a mission. */
#define HF_ACT_DONE 1u      /* done */
#define HF_ACT_REQUESTED 2u /* the game does it within this update or the next (damage, statuses, explosions) */
#define HF_ACT_WAITING 3u   /* its packages are loading (the call took them): call again */
#define HF_ACT_QUEUED 4u    /* not on the game thread: done at the next game update (its log says how it went) */
/* The Helldiver's limbs (heal_limb, injure, limb_state) */
#define HF_LIMB_HEAD 0u
#define HF_LIMB_CHEST 1u
#define HF_LIMB_LEFT_ARM 2u
#define HF_LIMB_RIGHT_ARM 3u
#define HF_LIMB_LEFT_LEG 4u
#define HF_LIMB_RIGHT_LEG 5u
typedef struct HfLimbState { int32_t health, max_health; uint32_t injured; } HfLimbState;
/* Statuses apply_status takes (StatusEffectSettings rows) */
#define HF_STATUS_FIRE 5u
#define HF_STATUS_FIRE_PANIC 6u
#define HF_STATUS_BURNING_HEAVY 32u
#define HF_STATUS_STUN_SMALL 37u
#define HF_STATUS_STUN_MEDIUM 38u
#define HF_STATUS_STUN_LARGE 39u
#define HF_STATUS_GAS 42u
#define HF_STATUS_GAS_2 43u
#define HF_STATUS_GAS_CONFUSION 44u
#define HF_STATUS_GAS_CONFUSION_2 45u
#define HF_STATUS_FLAMER_SLOWED 67u
/* Shot rules (modify_shots, homing): what they take is this machine's player's own shots of one projectile type, fired
 * by the listed weapons (entity resources, ending in 0; NULL or none: any of the player's). Each shot is changed once, as
 * it leaves; other weapons firing the same projectile stay as they are. Without HF_SHOT_MULTIPLAYER only in a solo game
 * (another machine's copy of the shot flies as the game made it). */
#define HF_SHOT_MULTIPLAYER 0x1u
#define HF_HOMING_NO_RETARGET 0x2u /* homing: keep the first target */
#define HF_HOMING_FRIENDLY 0x4u    /* homing: home on the other players' Helldivers, not on enemies */
typedef struct HfShotRule
{
    uint32_t size;          /* sizeof(HfShotRule) */
    uint32_t projectile;    /* its ProjectileSettings type */
    const uint64_t* sources;
    float damage;           /* multipliers, 1 for unchanged: damage 0.01 .. 100, armor_penetration 0.1 .. 10, */
    float armor_penetration; /* speed 0.1 .. 10, gravity 0 .. 20, drag 0 .. 20 */
    float speed, gravity, drag;
    uint32_t flags;
} HfShotRule;
typedef struct HfHoming
{
    uint32_t size;          /* sizeof(HfHoming) */
    uint32_t projectile;
    const uint64_t* sources;
    float turn_rate;        /* degrees a second, 1 .. 1440 */
    float cone;             /* the half-angle it looks for targets in, 1 .. 180 degrees */
    float range;            /* metres, 1 .. 500 */
    float arm_distance;     /* metres flown before it steers, 0 .. 100 */
    float aim_height;       /* metres above the target's root, -5 .. 10 */
    uint32_t flags;         /* HF_SHOT_MULTIPLAYER, HF_HOMING_... */
} HfHoming;
/* A transport Pelican called in (spawn_pelican): it flies in from the approach point, hovers over the position and
 * leaves after hover_seconds once released (0: at once). Host; solo for the hover. */
typedef struct HfPelicanCall
{
    uint32_t size;          /* sizeof(HfPelicanCall) */
    float position[3];      /* where it hovers */
    float hover_seconds;    /* 0 .. 120 */
    float approach_distance, approach_height; /* where it starts: this far back and this high (0: 250 m, 80 m) */
    float facing[2];        /* its heading (x, y); 0, 0: from the local Helldiver towards the position */
} HfPelicanCall;

/* A mod of the session (SDK 10, mods): what get_live names as a holder is its id. Set size before the call. */
#define HF_MOD_CODE 1u   /* a code mod (a DLL) */
#define HF_MOD_STATS 2u  /* a stats mod (stats files, no DLL) */
#define HF_MOD_LUA 3u    /* a Lua mod, through the Lua bridge (an HD2Runtime mod) */
#define HF_MOD_EDITOR 4u /* the editors: the F8 editor and Local Stats, where the editors save */
typedef struct HfModInfo
{
    uint32_t size;
    uint32_t kind;      /* HF_MOD_... */
    const char* id;     /* "skyeshade.vanilla-plus", "lua.mods-azura-railgun_repenetrated" */
    const char* folder; /* a code or stats mod's folder (UTF-8), "" for a Lua mod */
    const char* name;   /* a Lua mod's name as its runtime gave it ("mods/azura/railgun_repenetrated"), else "" */
    uint32_t loaded;    /* 1: it loaded (a mod whose hellforge_mod_init declined, or whose DLL failed, is 0) */
} HfModInfo;

/* Mod sync (SDK 14, sync_state): a join refused because the host runs other gameplay mods. The session's backend plans
 * the difference and offers to download the host's mods and restart with them (docs/mods.md "Playing with other mods");
 * an editor shows the offer and sends the player's answer (sync_answer). version changes with every new offer; a
 * download's progress (done) changes without it. */
#define HF_SYNC_NONE 0u        /* no offer */
#define HF_SYNC_OFFER 1u       /* the lobby needs mods: HF_SYNC_ACCEPT downloads them (or, with bytes 0, restarts) */
#define HF_SYNC_BLOCKED 2u     /* the lobby cannot be joined as it is: a blocked mod, one to install by hand, or mods the
                                * player must trust first (HF_SYNC_MOD_TRUST rows); the message says */
#define HF_SYNC_DOWNLOADING 3u /* downloading from the host: done of bytes */
#define HF_SYNC_READY 4u       /* everything is here: HF_SYNC_RESTART restarts the session with the host's mods */
#define HF_SYNC_FAILED 5u      /* the download or its checks failed (message says why) */
typedef struct HfSyncState
{
    uint32_t size;      /* sizeof(HfSyncState): set by the caller */
    uint32_t state;     /* HF_SYNC_... */
    uint32_t version;   /* the offer's: sync_answer names it */
    uint32_t mods;      /* the mods sync_mod lists */
    uint64_t bytes;     /* the bytes to download */
    uint64_t done;      /* downloaded so far */
    char host[64];      /* the host's name (UTF-8) */
    char message[192];  /* what the backend says about it (UTF-8), or "" */
} HfSyncState;

/* One mod of an offer (sync_mod): what happens to it. */
#define HF_SYNC_MOD_HAVE 0u     /* this machine has it (installed or synced before) */
#define HF_SYNC_MOD_DOWNLOAD 1u /* downloads from the host */
#define HF_SYNC_MOD_MANUAL 2u   /* the player installs it by hand (code or unchecked data from a host who is not a Steam
                                 * friend, or the host does not share); url, when set, says where to get it */
#define HF_SYNC_MOD_BLOCKED 3u  /* on the player's blocklist */
#define HF_SYNC_MOD_UPDATE 4u   /* part of HellForge, another version: both players need the same HellForge */
#define HF_SYNC_MOD_OFF 5u      /* one of this machine's own, off for the synced session (the host does not run it) */
#define HF_SYNC_MOD_TRUST 6u    /* code or unchecked data HellForge has not verified, from a Steam friend who shares:
                                 * downloads once the player trusts it (HF_SYNC_TRUST), or the player installs it by hand */
#define HF_SYNC_CLASS_CONTENT 0u /* what HellForge checks: data specs, schema stats, network field types */
#define HF_SYNC_CLASS_CODE 1u    /* a DLL, sources HellForge builds, or Lua */
#define HF_SYNC_CLASS_BUNDLED 2u /* shipped with HellForge (the in-game editor) */
#define HF_SYNC_CLASS_RAW 3u     /* data HellForge cannot check: data patches, raw stats or component writes */
typedef struct HfSyncMod
{
    uint32_t size;      /* sizeof(HfSyncMod): set by the caller */
    uint32_t action;    /* HF_SYNC_MOD_... */
    uint32_t kind;      /* HF_SYNC_CLASS_... */
    uint32_t reserved;
    uint64_t bytes;     /* its size */
    char id[65];
    char name[81];      /* UTF-8 */
    char version[33];
    char url[201];      /* where players get it: an https:// address from its manifest ("url"), or "" */
} HfSyncMod;

/* The player's answer to an offer (sync_answer). */
#define HF_SYNC_ACCEPT 1u  /* download (HF_SYNC_OFFER) */
#define HF_SYNC_DECLINE 2u /* no: the offer goes (a download stops) */
#define HF_SYNC_RESTART 3u /* restart the session with the host's mods (HF_SYNC_READY, or an offer with nothing to download) */
#define HF_SYNC_BLOCK 4u   /* put mod_id on the blocklist; the offer goes */
#define HF_SYNC_TRUST 5u   /* trust mod_id (an HF_SYNC_MOD_TRUST row: that exact content) and download it once nothing else
                            * waits; a new offer follows. Ask the player first: the mod runs its own code on their PC */
#define HF_SYNC_OPEN 6u    /* open mod_id's url in the player's browser (the backend opens only an https:// address) */

typedef struct HfEntityDef HfEntityDef;

/* Definition-block lists are arrays ending in {0}. */
typedef struct HfCopy
{
    HfRef from;     /* the entity whose value the field takes (when its data loads) */
    HfField field;
} HfCopy;
typedef struct HfSet
{
    HfField field;  /* a single value: arrays and raw bytes are copied, not set */
    HfValue value;
} HfSet;
/* A group of fields the mappings name, as one entry per field in a copy or set list:
 *     HF_COPY_GROUP(HF_ENTITY_MG206_HEAVY_MACHINE_GUN, ProjectileWeapon, firing_sounds),
 *     HF_SET_GROUP(WeaponData, recoil, HF_F32(0)), */
#define HF_COPY_GROUP(from, C, g) HF_GROUP_##C##_##g(HF_COPY_ONE_, from)
#define HF_SET_GROUP(C, g, ...) HF_GROUP_##C##_##g(HF_SET_ONE_, __VA_ARGS__)
#define HF_COPY_ONE_(C, f, from) {(from), HF_FIELD(C, f)}
#define HF_SET_ONE_(C, f, ...) {HF_FIELD(C, f), __VA_ARGS__}
typedef struct HfMountDef
{
    uint32_t slot;              /* the mount's slot (0: a Pelican's chin) */
    const HfEntityDef* entity;  /* what it carries: an entity defined by this mod (registered with this one) */
    HfRef resource;             /* or any entity by its id */
} HfMountDef;

/* A new entity as a definition block (register_entity): a copy of a vanilla entity under a name of its own, with the
 * changes its data gets. Every list may be NULL; unset fields mean the base's. The entity gets records of its own in
 * every component the block changes, and the loader applies the changes whenever the game loads its entity data (and
 * again if it reloads it), before HF_EVENT_ENTITIES_READY reaches any mod. Keep definitions static: the loader reads them
 * once, at registration.
 *
 *     static const HfSet turretSet[] = {{HF_FIELD(WeaponMagazine, capacity), HF_U32(2000)}, {0}};
 *     static const HfEntityDef turret = {.name = "my_name/my_mod/turret", .base = HF_ENTITY_PELICAN_TURRET, .set = turretSet};
 *     HfRef ref = api->register_entity(mod, &turret); */
struct HfEntityDef
{
    uint32_t size;                 /* sizeof(HfEntityDef), or 0 for SDK 6's */
    const char* name;              /* unique: "my_name/my_mod/entity"; the entity's resource id is this name's hash */
    HfRef base;                    /* the vanilla entity it copies */
    HfRef model_from;              /* 0, or a vanilla entity whose model it takes (as HfEntity.model_from) */
    const char* unit;              /* a unit resource for its model (the mod's data provides it); NULL: the base's */
    const HfBehaviour* behaviour;  /* a new behaviour it runs, added once however many entities name it */
    uint32_t behaviour_id;         /* or an existing one (HF_BEHAVIOUR_..., add_behaviour's); neither: the base's */
    const HfCopy* copy;            /* fields it takes from other entities */
    const HfSet* set;              /* fields it sets */
    const uint64_t* add_tags;      /* tags it gets (HF_TAG_...), ending in 0 */
    const uint64_t* remove_tags;   /* tags of its base it loses, ending in 0 */
    const HfMountDef* mount;          /* what its mount carries; entities defined by the mod are registered with it */
    uint32_t own_all;              /* SDK 7 (set size = sizeof(HfEntityDef)): 1, records of its own in every component (HfEntity.own_all) */
    const char* title;             /* SDK 7: its text, as HfEntity's (NULL: the base's) */
    const char* designation;
    const char* description;
};

/* Changes to an existing entity as a definition block (edit_entity): the same lists as HfEntityDef's. A vanilla entity's
 * record is changed only if no other entity shares it (checked when the data loads; stats mods change shared records);
 * an entity a mod added gets records of its own as with register_entity. Applied whenever the game loads its entity data.
 *
 *     static const HfSet backpackSet[] = {{HF_FIELD(Deposit, capacity), HF_U32(10000)}, {0}};
 *     static const HfEditDef backpack = {.entity = HF_ENTITY_MAXIGUN_BACKPACK, .set = backpackSet};
 *     api->edit_entity(mod, &backpack); */
typedef struct HfEditDef
{
    uint32_t size;                 /* sizeof(HfEditDef), or 0 for SDK 6's */
    HfRef entity;                  /* the entity it changes */
    const HfCopy* copy;
    const HfSet* set;
    const uint64_t* add_tags;
    const uint64_t* remove_tags;
    const HfMountDef* mount;
} HfEditDef;

typedef struct HfApi
{
    uint32_t version; /* HF_MOD_SDK_VERSION of the loader; functions are only ever added at the end. */
    uint32_t size;    /* sizeof(HfApi) of the loader. */

    /* Logging to the mod's own log in the session folder (printf format). */
    void (*log)(HfMod* mod, const char* format, ...);

    /* The supported game build ("25480438") and the mod's own id and folder (UTF-8). */
    const char* (*build_id)(void);
    const char* (*mod_id)(HfMod* mod);
    const char* (*mod_folder)(HfMod* mod);

    /* Calls back once when the event happens (at once if it already did). Returns 0 on success. */
    int (*on)(HfMod* mod, HfEvent event, HfEventCallback callback, void* user);

    /* A loaded module's base address and image size, or 0 ("game.dll", "helldivers2.exe"). */
    uintptr_t (*module_base)(const char* module, size_t* size);

    /* The first match of an IDA-style byte pattern ("48 8B ?? 24 ?? E8") in the module's image, or 0. It reads the code as
     * it is now: a function another mod or HellForge hooked starts with a jump. To check a function before hooking
     * it, use code_matches (SDK 13), which reads its bytes from before any hook. */
    uintptr_t (*find_pattern)(const char* module, const char* pattern);

    /* Memory: 0 on success. write makes the bytes writable for the write, restores their protection and flushes the
     * instruction cache, so it works for code and data. */
    int (*read)(uintptr_t address, void* buffer, size_t size);
    int (*write)(uintptr_t address, const void* data, size_t size);
    /* Writes a u32 only if it holds `expect` (or already `value`): 0 written or already set, 1 left alone. */
    int (*write_u32_if)(HfMod* mod, uintptr_t address, uint32_t expect, uint32_t value);

    /* Detours the function at `target` to `detour`; *original receives a pointer that calls the next hook in the
     * function's chain, or its own code. 0 on success. Hooks stay for the session. Since SDK 13 every hook, a mod's or
     * HellForge's own, joins one chain per function, so several mods can hook the same function: hook_ex (SDK 13) says
     * the order, and this is hook_ex with priority 0. Call the original through `*original` every time, never a copy of
     * it: HellForge rewrites it when another hook joins the chain after this one. Code another overlay already
     * redirected (its jump stub in allocated memory, where MinHook finds no room) is chained: the stub's address
     * becomes the detour, the overlay's code the original. */
    int (*hook)(HfMod* mod, uintptr_t target, void* detour, void** original);

    /* Network field types by name (after HF_EVENT_NETWORK_READY). get: 0 and *type filled, or 1. set_bits widens an
     * integer type, only if it is exactly `expect` (or already widened): 0 on success, 1 left alone. */
    int (*network_field_type)(const char* name, HfFieldType* type);
    int (*set_network_field_bits)(HfMod* mod, const char* name, HfFieldType expect, int32_t bits);

    /* The address of an entity's record of a component ("DepositComponentData") after HF_EVENT_ENTITIES_READY, or 0
     * when the entity owns no such record, owns several, or shares it with another entity. *stride receives the
     * record size. Resource ids are the 64-bit entity resource hashes HD2Runtime's catalogs list. */
    uintptr_t (*component_record)(const char* component, uint64_t resource, uint32_t* stride);

    /* ---- SDK 2 ---------------------------------------------------------------------------------------------------- */
    /* Adds a new arsenal weapon (see HfWeapon). HellForge adds it to the game's entity data when that loads, to the armory,
     * and to the backend's item catalogue and the player's inventory; it behaves exactly like its base except for what
     * the mod changes in its own records. Call it from hellforge_mod_init. Returns the new weapon's loadout id, or 0 (the
     * mod's log says why). */
    uint32_t (*add_weapon)(HfMod* mod, const HfWeapon* weapon);
    /* The entity resource of a weapon any mod added (its loadout id), for component_record; 0 if there is none. */
    uint64_t (*weapon_entity)(uint32_t weapon);

    /* ---- SDK 3 ---------------------------------------------------------------------------------------------------- */
    /* Adds a new projectile type (see HfProjectile). Call it from hellforge_mod_init. Returns its ProjectileType (351 and
     * up), or 0 (the mod's log says why). 64 new types in all mods together (HellForge moves the game's list). */
    uint32_t (*add_projectile)(HfMod* mod, const HfProjectile* projectile);
    /* The 272-byte settings row of a projectile a mod added, after HF_EVENT_PROJECTILES_READY, or 0. Its +0 is the type
     * and must stay so; the rest may be changed (HD2Runtime's research names the members). */
    uintptr_t (*projectile_row)(uint32_t type);
    /* Adds a new damage type (see HfDamage). Call it from hellforge_mod_init, before the projectiles that deal it.
     * Returns its id (650 and up), or 0 (the mod's log says why). */
    uint32_t (*add_damage)(HfMod* mod, const HfDamage* damage);
    /* The 76-byte DamageInfo row of a damage type a mod added, after HF_EVENT_PROJECTILES_READY, or 0: +0 id (must
     * stay), +4 standard damage, +8 durable damage, +0x0C four armour penetration values, +0x1C on status effects. */
    uintptr_t (*damage_row)(uint32_t id);
    /* The game's 64-bit id of a resource name ("my_name/my_mod/bolt"): what the game's data and rows store. */
    uint64_t (*resource_hash)(const char* name);

    /* ---- SDK 4 ---------------------------------------------------------------------------------------------------- */
    /* Adds a new stratagem (see HfStratagem). Call it from hellforge_mod_init. Returns its StratagemType (150 and up),
     * or 0 (the mod's log says why). The game has room for 48 new stratagems in all mods together. */
    uint32_t (*add_stratagem)(HfMod* mod, const HfStratagem* stratagem);
    /* The 400-byte settings row of a stratagem a mod added, once the game has loaded its stratagems, or 0. +0 type and
     * +4 stable id must stay; HD2Runtime's research names the rest (docs/internals.md "Stratagems"). */
    uintptr_t (*stratagem_row)(uint32_t type);
    /* Adds a new entity (see HfEntity). Call it from hellforge_mod_init. Returns its resource id, or 0 (the mod's log
     * says why). Entities and weapons share room for 64. */
    uint64_t (*add_entity)(HfMod* mod, const HfEntity* entity);
    /* Adds a new behaviour (see HfBehaviour). Call it from hellforge_mod_init. Returns its id (694 and up), or 0 (the
     * mod's log says why). An entity given it runs its base instead if HellForge could not install it. */
    uint32_t (*add_behaviour)(HfMod* mod, const HfBehaviour* behaviour);

    /* ---- SDK 5 ---------------------------------------------------------------------------------------------------- */
    /* Adds difficulty level 11 (see HfDifficulty). Call it from hellforge_mod_init. Returns 11, or 0 (the mod's log
     * says why). One mod can add it. */
    uint32_t (*add_difficulty)(HfMod* mod, const HfDifficulty* difficulty);
    /* The 0x330-byte settings row of a level (1 .. 11) once HellForge has moved the table, or 0. +0 unlock item and
     * +4 level must stay (artifacts/difficulty11/FINDINGS.md names the members known). */
    uintptr_t (*difficulty_row)(uint32_t level);

    /* ---- SDK 6 ---------------------------------------------------------------------------------------------------- */
    /* Registers a new entity from its definition block (see HfEntityDef), with the entities its mount carries and the
     * behaviour it runs. Call it from hellforge_mod_init. Registering the same block again returns the same entity.
     * Returns the entity, or 0 (the mod's log says why, with the source line of a field the loader refused). */
    HfRef (*register_entity)(HfMod* mod, const HfEntityDef* entity);
    /* The calls behind definition blocks, for code: each queues a deferred edit, applied whenever the game loads its
     * entity data (at once if it has). An entity a mod added gets a record of its own in the field's component when the
     * call comes from hellforge_mod_init (later, it must own one already); a vanilla entity's record is changed only if
     * no other entity shares it. Another mod (or a stats mod) changing the same field is reported in both mods' logs and
     * modloader.log; the later edit's value stays. 0 queued, 1 refused (the mod's log says why).
     * Prefer the hf_copy, hf_set, hf_tag and hf_mount macros below. */
    int (*copy_field)(HfMod* mod, HfRef entity, HfRef from, const HfField* field);
    int (*set_field)(HfMod* mod, HfRef entity, const HfField* field, const HfValue* value);
    int (*set_tag)(HfMod* mod, HfRef entity, uint64_t tag, int present);
    int (*mount)(HfMod* mod, HfRef entity, uint32_t slot, HfRef carried);
    /* An entity's own record of a component after HF_EVENT_ENTITIES_READY, checked against the layout the mod was built
     * with, or NULL (the mod's log says why). Use hf_record. */
    void* (*record)(HfMod* mod, HfRef entity, const char* component, uint32_t layout, uint32_t size);
    /* Content by name, or 0: vanilla content by its catalog name (find(HF_KIND_ENTITY, "Pelican Transport"); the
     * hellforge_content.h constants have the same ids), or content a mod added by its identity
     * ("skyeshade.pelican-support:pelican_gatling", or the full name it was added under). For names known only at run
     * time (a config file, another mod's content); code that knows the content uses the constants. */
    uint64_t (*find)(uint32_t kind, const char* name);
    /* Changes an existing entity from a definition block (see HfEditDef). 0 queued, 1 refused (the mod's log says why,
     * with the source line of a field the loader refused). */
    int (*edit_entity)(HfMod* mod, const HfEditDef* edit);
    /* Changes a field in the running game (live editing): the same deferred edit as set_field, applied at once and kept
     * for the rest of the session (renewed if the game reloads its data), with the same checks and conflict reports.
     * The target is found by identity every time, never by address: an entity by its resource id (a vanilla one or one
     * a mod added), or a settings table row by its id (HfProjectileSettings: a projectile type; HfDamageInfo: a damage
     * id; HfStratagemSettings: a stratagem type; HfDifficultySettings: a level; the explosion, arc, beam and status
     * tables: their row ids). The mod's next live edit of the same field replaces this one; value NULL undoes it,
     * putting back the value from before it. A record other entities share is changed only with HF_LIVE_SHARED (it
     * changes all of them, as a stats file does). Returns when the change applies (HF_APPLIES_...), or 0 refused (the
     * mod's log says why). Nothing is saved: a live editor saves into a stats file (hellforge-stats.json). */
    int (*set_live)(HfMod* mod, HfRef target, const HfField* field, const HfValue* value, uint32_t flags);
    /* Reads a field of the running game, found as set_live finds it (a shared record too): its value now and its base.
     * Returns when a change to it applies (HF_APPLIES_...), or 0 when it cannot be read (no such record, or its table
     * is not loaded yet). */
    int (*get_live)(HfMod* mod, HfRef target, const HfField* field, HfLiveValue* out);
    /* Overrides the local player's armor passive and second passive (the helmet's slot), solo only: the game's attribute
     * reader takes the armor passive's bonuses first, then the second's that the armor lacks. The override is kept
     * (written again after a kit change) until both are HF_PASSIVE_KIT; with other players in, the kits' values return
     * and it waits. Refused per slot, and said in get_passives' status: a passive whose effect needs a package
     * (INTEGRATED EXPLOSIVES, ADRENO-DEFIBRILLATOR) unless the armor gives it, INTEGRATED EXPLOSIVES as the second.
     * Returns 1 held (or released), 0 for an id the game has no passive for. */
    int (*set_passives)(HfMod* mod, uint32_t armor, uint32_t second);
    /* The local player's passives now; returns 1 when the record is readable (the status says why not). */
    int (*get_passives)(HfMod* mod, HfPassives* out);
    /* Calls back every game update on the game thread (the thread the game's own engine calls come from: sounds,
     * spawns, package requests), before the game's update. Keep it short; it runs every frame. Returns 1, or 0 when
     * the game thread's update could not be hooked (modloader.log says why). Check hf->size before calling. */
    int (*on_game_thread)(HfMod* mod, HfTickCallback callback, void* user);
    /* Loads a package (by its resource name hash, as mappings/references.json lists them) and holds it for the session,
     * through the game's own package system on the game thread: HF_PACKAGE_RESIDENT once the engine has all of it,
     * HF_PACKAGE_LOADING until then (call it again to see), 0 refused or failed (not a catalogue package, HellForge's
     * 64 held already, or not loaded within 90 seconds; the mod's log says why). Loading is per client: other players
     * load their own. A reference set_live or a stats file writes (a donor projectile, an explosion) loads its packages
     * itself. */
    int (*load_package)(HfMod* mod, uint64_t package);
    /* The mod's last log line (say, why the call that just returned 0 was refused), and the session's logs folder
     * (UTF-8: modloader.log and every mod-<id>.log), or "". Check hf->size before calling. */
    const char* (*last_log)(HfMod* mod);
    const char* (*logs_folder)(HfMod* mod);
    /* A stats field's value now and its base (HfLiveValue's), for any field the session's stats snapshot reads, the
     * fields get_live cannot (entity deltas, say). 1 when read, 0 not (not in the snapshot, or not read yet). */
    int (*get_stat)(HfMod* mod, const HfStat* stat, HfLiveValue* out);
    /* The session's co-op role (HfSession), as the session's backend tells the loader. */
    void (*session)(HfMod* mod, HfSession* out);

    /* ---- SDK 7 ---------------------------------------------------------------------------------------------------- */
    /* Listens to one kind of game event (HF_ON_..., see HfEventInfo), on the game thread, highest priority first (equal
     * priorities in subscription order). Call it from hellforge_mod_init. Returns 1, or 0 (the mod's log says why: an
     * unknown kind, or the game thread not hooked). A kind's source is hooked once a mod listens to it; one that does
     * not read as reviewed on this game build delivers nothing, and modloader.log says so. */
    int (*subscribe)(HfMod* mod, uint32_t kind, HfListener listener, void* user, int32_t priority);
    /* The game's state now (HF_STATE_...), or HF_STATE_NONE before the game has one. */
    uint32_t (*game_state)(HfMod* mod);
    /* The index-th piece of content mods added (stratagems, entities, projectiles, damage; see HfContent): 1 with *out
     * filled, 0 past the last. Complete once every mod's hellforge_mod_init has run (from HF_EVENT_GAME_LOADED). */
    int (*content)(HfMod* mod, uint32_t index, HfContent* out);
    /* Adds a new ability (see HfAbility). Call it from hellforge_mod_init. Returns its id (2868 and up), or 0 (the mod's
     * log says why). An entity naming it never sets it off if HellForge could not install it (modloader.log). */
    uint32_t (*add_ability)(HfMod* mod, const HfAbility* ability);
    /* SDK 8: returns a selection-category handle, or 0 with a logged reason. Re-registering an identical fully
     * qualified definition returns the same handle; a conflicting title/order is refused. */
    uint32_t (*add_stratagem_category)(HfMod* mod, const HfStratagemCategory* category);

    /* ---- SDK 9 ---------------------------------------------------------------------------------------------------- */
    /* Changes a stats field set_live cannot reach (an entity delta: an attachment's or a default magazine's stats; a
     * status multiplier), the fields get_stat reads, live, in the place a stats file writes it at boot. value NULL: back
     * to its value from before this session's live change, or with HF_LIVE_BASE to its base (get_stat's). Returns
     * HF_APPLIES_RESPAWN (a weapon takes an attachment's value when it is next built, expected) or 0 (the mod's log says
     * why). Check hf->size before calling. */
    int (*set_stat)(HfMod* mod, const HfStat* stat, const HfValue* value, uint32_t flags);
    /* A settings row's bytes (offset, size) replaced live by another row's of the same table, for the rows HellForge's
     * catalogue names (mappings/references.json "rowCopies": the G-109 Urchin's pulse explosion), from the donors listed
     * there, once the donor's packages are resident (it waits for them). from 0 puts the row's own bytes back. Returns
     * HF_APPLIES_NOW or 0 (the mod's log says why). Check hf->size before calling. */
    int (*copy_row)(HfMod* mod, const char* table, uint64_t row, uint64_t from, uint32_t offset, uint32_t size);

    /* ---- SDK 10 --------------------------------------------------------------------------------------------------- */
    /* The index-th mod of the session (HfModInfo): the code and stats mods in load order, then the Lua mods as they
     * register through the Lua bridge (docs/lua-bridge.md). 1 with *out filled, 0 past the last. Check hf->size. */
    int (*mods)(HfMod* mod, uint32_t index, HfModInfo* out);

    /* ---- SDK 11: actions (HF_ACT_...; check hf->size before calling) ----------------------------------------------- */
    /* Heals the local Helldiver by `amount` health (each limb by the same share), up to its maximum. */
    int (*heal)(HfMod* mod, float amount);
    /* Heals one of the local Helldiver's limbs (HF_LIMB_...) in full, and mends it. */
    int (*heal_limb)(HfMod* mod, uint32_t limb);
    /* Damages one of the local Helldiver's limbs (1 .. its maximum: head 85, chest 60, arms 35, legs 45); it may down
     * or kill the Helldiver, as the game's own hit would. */
    int (*injure)(HfMod* mod, uint32_t limb, int32_t damage);
    /* A Helldiver's limb now: its health, maximum and whether it is injured (any Helldiver this machine has). */
    int (*limb_state)(HfMod* mod, uint32_t entity, uint32_t limb, HfLimbState* out);
    /* Applies a status (HF_STATUS_...) to an entity: `buildup` 0 .. 1000 (100: as a full hit); host. */
    int (*apply_status)(HfMod* mod, uint32_t entity, uint32_t status, float buildup);
    /* Adds to the local Helldiver's velocity (m/s, world space, Z up; each part at most 25, the result at most 50).
     * Walking resets sideways speed every update: an upward push is the dependable one. */
    int (*add_velocity)(HfMod* mod, const float delta[3]);
    /* The local Helldiver uses its B-1 Supply Pack on itself, as the game's own input does (one supply). */
    int (*resupply_from_pack)(HfMod* mod);
    /* An explosion (its ExplosionSettings row) at a position, credited to this machine's player; host. Loads the
     * explosion's packages first (HF_ACT_WAITING until they are in). */
    int (*spawn_explosion)(HfMod* mod, uint32_t type, const float position[3]);
    /* A projectile (its ProjectileSettings type) fired from a position along a direction, as the local Helldiver's;
     * host. Loads its packages first. */
    int (*spawn_projectile)(HfMod* mod, uint32_t type, const float position[3], const float direction[3]);
    /* The kills of an entity (a turret, a vehicle a mod spawned) credited to its owner, this machine's player; solo host. */
    int (*credit_to_player)(HfMod* mod, uint32_t entity);
    /* Pauses (1) or resumes (0) the mission timer; host. A new mission starts it again. */
    int (*freeze_mission_timer)(HfMod* mod, int frozen);
    /* Calls in a transport Pelican (HfPelicanCall); *entity its entity. Host. */
    int (*spawn_pelican)(HfMod* mod, const HfPelicanCall* call, uint32_t* entity);
    /* Shot rules: an id (1 and up), or 0 refused. stop_shots ends one (later shots fly as the game makes them). One
     * rule of each kind per projectile type. */
    uint32_t (*modify_shots)(HfMod* mod, const HfShotRule* rule);
    uint32_t (*homing)(HfMod* mod, const HfHoming* rule);
    int (*stop_shots)(HfMod* mod, uint32_t id);

    /* ---- SDK 12 --------------------------------------------------------------------------------------------------- */
    /* HellForge's latest message for the player (HfNotice), as the session's backend posts it. Check hf->size. */
    void (*notice)(HfMod* mod, HfNotice* out);

    /* ---- SDK 13 --------------------------------------------------------------------------------------------------- */
    /* hook with an order: a function's hooks run first to last, each calling the next through its original. Mods'
     * hooks run first, the highest `priority` first; equal priorities by load order, the mod higher on the Mods page
     * first (a mod's own later hook on the same function before its earlier one). HellForge's own hooks (its events,
     * the registry, the game thread) run last, nearest the game's code. The mod's log and modloader.log name each hook
     * with its place in the chain, a function hooked by more than one mod or by HellForge too is logged as a shared
     * hook with every owner in order, and the session folder's hooks.tsv lists every chain. Refused (1, the mod's log
     * says why): the same detour on the same function twice, more than 16 hooks on one function or 1024 hooked
     * functions in all. Check hf->size before calling. */
    int (*hook_ex)(HfMod* mod, uintptr_t target, void* detour, void** original, int32_t priority);
    /* 1 when the bytes at `address`, as they were before any hook HellForge placed (its own or a mod's), match the
     * IDA-style pattern ("48 8B ?? 24"); 0 otherwise or when they cannot be read. The check to make before hooking a
     * function: another mod that hooked it first does not fail it, where find_pattern and read would (they see the
     * hook's jump). Only hooks are seen through: bytes a mod changed with write, or that HellForge rewrote, read as
     * they are now. Check hf->size before calling. */
    int (*code_matches)(uintptr_t address, const char* pattern);
    /* Like read, with the bytes as they were before any hook HellForge placed (as code_matches reads them). 0 on
     * success. */
    int (*read_code)(uintptr_t address, void* buffer, size_t size);

    /* ---- SDK 14: mod sync (HfSyncState; check hf->size before calling) ------------------------------------------- */
    /* The current offer: 1 with *out filled (at most out->size bytes), 0 none (solo, or no backend connected). */
    int (*sync_state)(HfMod* mod, HfSyncState* out);
    /* The index-th mod of the current offer: 1 with *out filled (at most out->size bytes), 0 past the last. */
    int (*sync_mod)(HfMod* mod, uint32_t index, HfSyncMod* out);
    /* Sends the player's answer (HF_SYNC_ACCEPT ...) to the offer of that version; mod_id names the mod for
     * HF_SYNC_BLOCK, HF_SYNC_TRUST and HF_SYNC_OPEN (NULL otherwise). 1 sent; 0 not (another offer came since, or no
     * backend connected). */
    int (*sync_answer)(HfMod* mod, uint32_t version, uint32_t answer, const char* mod_id);

    /* ---- SDK 15 --------------------------------------------------------------------------------------------------- */
    /* Whose stats the lobby plays with: HF_STATS_HOST or HF_STATS_OWN (see HF_STATS_OWN). Check hf->size. */
    uint32_t (*stats_source)(HfMod* mod);
} HfApi;

/* SDK 6: an HfMod starts with the API, so these need only the mod. */
#define HF_API(mod) (*(const HfApi* const*)(const void*)(mod))

/* A typed pointer to an entity's own record, or NULL: HfWeaponData* data = hf_record(mod, turret, HfWeaponData);
 * The records of entities a mod added are writable. A vanilla entity's are the game's read-only data: read them, and
 * change them with hf_set or edit_entity (writing through the pointer crashes the game). */
#define hf_record(mod, entity, T) ((T*)HF_API(mod)->record((mod), (entity), T##_INFO, (uint32_t)sizeof(T)))

/* Deferred edits in code, the calls behind definition blocks:
 *     hf_copy(mod, turret, HF_ENTITY_GATLING_SENTRY, ProjectileWeapon, fire_rate);
 *     hf_set(mod, turret, WeaponMagazine, capacity, HF_U32(2000));
 *     hf_tag(mod, turret, HF_TAG_NO_KILL_CREDIT, 0);
 *     hf_mount(mod, pelican, 0, turret); */
static inline int hf_copy_field_(HfMod* mod, HfRef entity, HfRef from, HfField field) { return HF_API(mod)->copy_field(mod, entity, from, &field); }
static inline int hf_set_field_(HfMod* mod, HfRef entity, HfField field, HfValue value) { return HF_API(mod)->set_field(mod, entity, &field, &value); }
#define hf_copy(mod, entity, from, C, f) hf_copy_field_((mod), (entity), (from), HF_LITERAL(HfField) HF_FIELD(C, f))
#define hf_set(mod, entity, C, f, value) hf_set_field_((mod), (entity), HF_LITERAL(HfField) HF_FIELD(C, f), HF_LITERAL(HfValue) value)
#define hf_tag(mod, entity, tag, present) HF_API(mod)->set_tag((mod), (entity), (tag), (present))
#define hf_mount(mod, entity, slot, carried) HF_API(mod)->mount((mod), (entity), (slot), (carried))

/* set_live's flags. */
#define HF_LIVE_SHARED 0x1u /* the target's record may be one other entities share */
#define HF_LIVE_BASE 0x2u   /* with value NULL: back to the field's base (get_live), not just to the value from before
                             * this mod's live edit: an editor removing a change it saved into Local Stats */


/* A live change: hf_live(mod, HF_ENTITY_..., WeaponMagazine, capacity, HF_U32(60), 0) returns HF_APPLIES_... or 0. */
static inline int hf_live_field_(HfMod* mod, HfRef target, HfField field, HfValue value, uint32_t flags) { return HF_API(mod)->set_live(mod, target, &field, &value, flags); }
#define hf_live(mod, target, C, f, value, flags) hf_live_field_((mod), (target), HF_LITERAL(HfField) HF_FIELD(C, f), HF_LITERAL(HfValue) value, (flags))

/* Vanilla content by name (HF_ITEM_..., HF_ENTITY_..., HF_STRATAGEM_...), generated from mappings/catalog.json. */
#include "hellforge_content.h"

/* Implemented by the mod. */
#ifndef HF_MOD_LOADER
HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod);
#endif

#ifdef __cplusplus
}
#endif

#endif
