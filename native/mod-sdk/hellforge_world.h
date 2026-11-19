/* HellForge mod SDK 21: effects on entities (play_effect, stop_effect), enemies near a point (find_enemies) and the
 * weapon the armory shows (armory_weapon); docs/modding/world.md. It also holds the spawn director's
 * enemy constants (SDK 17, add_enemy). Included by hellforge_mod.h. */
#pragma once
#include <stdint.h>

/* The faction rosters of the enemy spawn director, for HfEnemy and HfEnemySwap (SDK 17). */
#define HF_ENEMY_TERMINIDS 1u
#define HF_ENEMY_AUTOMATONS 2u
#define HF_ENEMY_ILLUMINATE 4u

#define HF_ENEMY_LEVELS 10
/* HfEnemy flags */
#define HF_ENEMY_ALWAYS_LOADED 1u /* a mission loads its packages though no squad names its group (it spawns by other means) */

/* HfEffect.flags */
#define HF_EFFECT_SHARED 0x1u /* every machine of the lobby plays it too, on its own copy of the entity */

/* stop_effect's how */
#define HF_EFFECT_STOP 0u    /* it stops spawning particles; those already out live their life */
#define HF_EFFECT_DESTROY 1u /* it is gone at once */

/* A particle effect (play_effect): attached to an entity's unit, or placed in the world. Zero-initialize it and set
 * `size`. The particles must be loaded: from a package the mod loads (load_package, its data's session package) or one
 * the entity's own packages hold. */
typedef struct HfEffect
{
    uint32_t size;      /* sizeof(HfEffect) */
    uint32_t flags;     /* HF_EFFECT_... */
    uint64_t particles; /* the particles resource: resource_hash of its name ("content/fx/.../steam_vent") */
    /* The entity it follows (its unit carries it, and it goes when the unit goes), or 0: it stays at `position`. */
    uint32_t entity;
    uint32_t reserved;
    const char* node;   /* the unit's node it hangs from ("" or NULL: the root) */
    /* With an entity: its offset from the node, in the node's own axes (metres). Without: where it plays in the world. */
    float position[3];
} HfEffect;

/* find_enemies' answers, nearest first. */
typedef struct HfFoundEnemy
{
    uint32_t entity;
    uint32_t unit;       /* its unit's handle */
    uint64_t resource;   /* its type (HF_ENTITY_..., or the entity a mod added) */
    float position[3];   /* its unit's root in the world */
    float distance;      /* from the centre asked about (metres) */
    int32_t health;      /* its health pool now */
    int32_t max_health;  /* -1 when it does not read */
    uint32_t faction;    /* its faction mask (bit 0 is the Helldivers' side); 0 when it has none and its type says enemy */
    uint32_t flags;      /* HF_FOUND_... */
} HfFoundEnemy;

/* HfFoundEnemy.flags */
#define HF_FOUND_DOWNED 0x1u    /* downed, not dead */
#define HF_FOUND_AUTHORITY 0x2u /* this machine simulates it (is_authority) */

/* armory_weapon's menu: the menu state the game shows. */
#define HF_MENU_ARMORY 2u     /* the Armory: its weapon tabs' grids, with CUSTOMIZE and EQUIP */
#define HF_MENU_CUSTOMIZE 33u /* the armory's customize screen of one weapon */
