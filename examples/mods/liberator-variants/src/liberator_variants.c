/* Liberator Variants: five standalone arsenal weapons (SDK 17) on the AR-23 Liberator's model, each showing one part of
 * a weapon of your own. hellforge-data.json builds them from each player's game copy, in the Liberator's archives:
 *   - AR-23G Liberator Gold: the Liberator's unit naming a gold copy of its body material;
 *   - CB-1 Purple Metal Cube: the mesh of models/cube.obj in place of the rifle, its colour lookup purple metal;
 *   - AR-23A Liberator Animated: bones, clips and a state machine from animations/assault_rifle.json;
 *   - AR-23T Liberator Talon: a projectile and damage type of its own, blue effect copies and the LAS-16 Sickle's shot;
 *   - MC-1 Model Carbine: a whole unit from models/carbine.glb and its own textures, firing from its own joints.
 * None copies a weapon: each declares the Liberator's reload, sounds and effects (DeclareLiberator) and its class gives
 * it the rest. Build: open this folder as a CMake project, or scripts/Build-Mod.ps1 examples/mods/liberator-variants. */
#include "hellforge_mod.h"

#define VARIANT "skyeshade/liberator_variants/"
#define SET_AT(mod, e, C, f, i, v) hf_set_field_((mod), (e), HF_LITERAL(HfField) HF_FIELD_AT(C, f, i), HF_LITERAL(HfValue) v)
#define LIBERATOR_FIRE_NODE 0x2C8ECE2Bu   /* "muzzle" in the Liberator's unit */
#define LIBERATOR_CASING_NODE 0xDC0A2993u /* its casing joint */

static uint32_t Thin(const HfApi* api, const char* name) { return (uint32_t)(api->resource_hash(name) >> 32); }

/* What the class leaves to the weapon: the Liberator's sound events, animation events and effects, named by id (its
 * archives, which the data patch adds to, hold them), and the joints it fires and ejects from. */
static void DeclareLiberator(HfMod* mod, uint64_t e, uint32_t fire_node, uint32_t casing_node)
{
    hf_set(mod, e, Wieldable, wield_state_audio_event, HF_U32(0x8B88F052u));
    hf_set(mod, e, WeaponReload, event_type_0, HF_U32(1));
    hf_set(mod, e, WeaponReload, weapon_event_0, HF_U32(0xF19FFE15u));
    hf_set(mod, e, WeaponReload, wielder_event_0, HF_U32(0x282D892Cu));
    hf_set(mod, e, WeaponReload, event_type_1, HF_U32(3));
    hf_set(mod, e, WeaponReload, weapon_event_1, HF_U32(0x67C31A4Bu));
    hf_set(mod, e, WeaponReload, wielder_event_1, HF_U32(0xF511EC9Du));
    hf_set(mod, e, WeaponReload, vo_normal, HF_U32(0x8E929178u));
    hf_set(mod, e, Equipment, holster_animation_event, HF_U32(0x85450F78u));
    hf_set(mod, e, Equipment, switch_wield_audio_event, HF_U32(0x54AB6574u));
    hf_set(mod, e, Equipment, switch_holster_audio_event, HF_U32(0xBED51E4Fu));
    hf_set(mod, e, WeaponData, enter_first_person_aim_audio_event, HF_U32(0x646B3C6Fu));
    hf_set(mod, e, WeaponData, exit_first_person_aim_audio_event, HF_U32(0xEC5B01F7u));
    hf_set(mod, e, WeaponData, enter_third_person_aim_audio_event, HF_U32(0x284DAA89u));
    hf_set(mod, e, WeaponData, exit_third_person_aim_audio_event, HF_U32(0x299B40E5u));
    hf_set(mod, e, WeaponData, fire_mode_changed_audio_event, HF_U32(0x57619C1Au));
    hf_set(mod, e, WeaponData, fire_round_wielder_animation_event, HF_U32(0xAE6CC371u));
    hf_set(mod, e, WeaponData, fire_mode_changed_wielder_animation_event, HF_U32(0xE68B5142u));
    hf_set(mod, e, WeaponCustomization, muzzle_unit, HF_U64(0x756E3FDACA41CFCAULL));
    hf_set(mod, e, ProjectileWeapon, world_shake_effect, HF_U64(0xA080528BF7ED726DULL));
    hf_set(mod, e, ProjectileWeapon, local_shake_effect, HF_U64(0xA080528BF7ED726DULL));
    hf_set(mod, e, ProjectileWeapon, shake_inner_radius, HF_F32(5));
    hf_set(mod, e, ProjectileWeapon, shake_outer_radius, HF_F32(10));
    hf_set(mod, e, ProjectileWeapon, low_ammo_audio_event, HF_U32(0x560C72D2u));
    hf_set(mod, e, ProjectileWeapon, ejection_effect, HF_U64(0x598D2DD2808A6073ULL));
    hf_set(mod, e, ProjectileWeapon, casing_effect, HF_U64(0x5AE9276989F2BD5EULL));
    SET_AT(mod, e, ProjectileWeapon, casing_node, 0, HF_U32(casing_node));
    hf_set(mod, e, ProjectileWeapon, muzzle_flash, HF_U64(0xD033B685F434BF37ULL));
    hf_set(mod, e, ProjectileWeapon, fire_single_audio_event, HF_U32(0xAC6B5FFBu));
    hf_set(mod, e, ProjectileWeapon, haptics_loop_stop_event, HF_U32(0xFF07C3D9u));
    hf_set(mod, e, ProjectileWeapon, fire_node, HF_U32(fire_node));
    hf_set(mod, e, ProjectileWeapon, dry_fire_audio_event, HF_U32(0xF918651Au));
    hf_set(mod, e, ProjectileWeapon, dry_fire_repeat_audio_event, HF_U32(0x40538B1Cu));
}

/* The class, package, sound bank and reload the variants share: the Liberator's archive and bank, which the data patch
 * puts their units in. */
static uint32_t AddVariant(const HfApi* api, HfMod* mod, HfWeapon* w, uint32_t fire_node, uint32_t casing_node)
{
    w->weapon_class = HF_WEAPON_CLASS_PRIMARY;
    w->package = "0xDBD9381F9EA9788F";
    w->audio_package = "0xC553C951A7BE2327";
    w->reload_ability = 2739; /* the Liberator's reload, whose events the animated variant's state machine answers */
    if (w->projectile == 0) w->projectile = HF_PROJECTILE_AR23_LIBERATOR;
    uint32_t id = api->add_weapon(mod, w);
    if (id) DeclareLiberator(mod, api->weapon_entity(id), fire_node, casing_node);
    api->log(mod, id ? "%s %s is loadout item %08x" : "%s %s was not added", w->designation, w->name, id);
    return id;
}

static uint32_t Simple(const HfApi* api, HfMod* mod, const char* unit, const char* designation, const char* name,
                       const char* description)
{
    HfWeapon w = {sizeof w};
    w.unit = unit;
    w.designation = designation;
    w.name = name;
    w.description = description;
    return AddVariant(api, mod, &w, LIBERATOR_FIRE_NODE, LIBERATOR_CASING_NODE);
}

/* A damage type with five times the LAS-58 Talon bolt's damage, a copy of the bolt dealing it with blue effects, and a
 * variant firing it with the Sickle's shot (its banks are in the data patch). */
static uint32_t Talon(const HfApi* api, HfMod* mod)
{
    HfDamage heavy = {sizeof heavy};
    heavy.base = HF_DAMAGE_LAS58_TALON;
    heavy.name = "bolt_damage"; /* SDK 6: short names are the mod's ("skyeshade.liberator-variants:bolt_damage") */
    heavy.scale = 5.0f;
    uint32_t damage = api->add_damage(mod, &heavy);

    HfProjectile bolt = {sizeof bolt};
    bolt.base = HF_PROJECTILE_LAS58_TALON;
    bolt.name = "bolt";
    bolt.damage = damage;
    bolt.spawn_effect = api->resource_hash(VARIANT "talon/bolt_blue");
    bolt.flight_effect = api->resource_hash(VARIANT "talon/bolt_flight_blue");
    uint32_t projectile = api->add_projectile(mod, &bolt);
    if (damage == 0 || projectile == 0) return 0;

    HfWeapon rifle = {sizeof rifle};
    rifle.unit = VARIANT "talon";
    rifle.designation = "AR-23T";
    rifle.name = "Liberator Talon";
    rifle.description = "A Liberator rebuilt around the LAS-58 Talon's bolt, tuned well past its rated output. Five times the "
        "damage per hit; the warranty did not survive the modification.";
    rifle.projectile = projectile;
    uint32_t id = AddVariant(api, mod, &rifle, LIBERATOR_FIRE_NODE, LIBERATOR_CASING_NODE);
    if (id == 0) return 0;
    uint64_t e = api->weapon_entity(id);
    hf_set(mod, e, ProjectileWeapon, fire_single_audio_event, HF_U32(0xD8B6D690u)); /* the Sickle's shot */
    hf_set(mod, e, ProjectileWeapon, haptics_loop_stop_event, HF_U32(0xC43DF0AEu));
    hf_set(mod, e, ProjectileWeapon, use_midi_event_system, HF_U8(1));
    return id;
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    if (api->version < 17) return 1;
    int added = 0;
    added += Simple(api, mod, VARIANT "gold", "AR-23G", "Liberator Gold", "The SEAF standard assault rifle, plated in gold "
        "for those who serve Liberty with distinction. Performs like the Liberator; looks considerably more democratic.") != 0;
    added += Simple(api, mod, VARIANT "cube", "CB-1", "Purple Metal Cube", "A cube. Of purple metal. It fires like a "
        "Liberator, somehow.") != 0;
    added += Simple(api, mod, VARIANT "animated", "AR-23A", "Liberator Animated", "The SEAF standard assault rifle with a "
        "bolt you can see working and a magazine change you can follow.") != 0;
    added += Talon(api, mod) != 0;
    HfWeapon carbine = {sizeof carbine};
    carbine.unit = VARIANT "carbine";
    carbine.designation = "MC-1";
    carbine.name = "Model Carbine";
    carbine.description = "A carbine built from a model file of its own. Handles like the Liberator.";
    added += AddVariant(api, mod, &carbine, Thin(api, "attach_muzzle"), Thin(api, "ejector")) != 0;
    return added == 5 ? 0 : 1;
}
