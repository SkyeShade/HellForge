/* Alpha Scavenger: a new enemy beside the vanilla ones (docs/modding/enemies.md). The entity is a copy of the tier 2
 * Scavenger with more health; add_enemy copies that Scavenger's roster rows for it, so it spawns in the same groups on
 * the same planets, a third as often, from difficulty 4. No vanilla enemy is replaced. On planets of the BugPredators
 * constellation, a replacement rule (add_enemy_swap) turns each Alpha into an Alpha Predator, a second copy.
 * Build: HellForge builds the sources before each session, or scripts/Build-Mod.ps1 examples/mods/alpha-scavenger. */
#include "hellforge_mod.h"

#define SCAVENGER_TIER_2 0x9A8A3AAE287B230Cull /* mappings/entities.json: "scavenger_tier_2", "resource" */

static const HfSet alphaSet[] = {{HF_FIELD(Health, health), HF_I32(400)}, {0}};
static const HfEntityDef alphaDef = {
    .size = sizeof(HfEntityDef),
    .name = "skyeshade/alpha_scavenger/entity",
    .base = SCAVENGER_TIER_2,
    .set = alphaSet,
};
static const HfSet predatorSet[] = {{HF_FIELD(Health, health), HF_I32(600)}, {0}};
static const HfEntityDef predatorDef = {
    .size = sizeof(HfEntityDef),
    .name = "skyeshade/alpha_scavenger/predator_entity",
    .base = SCAVENGER_TIER_2,
    .set = predatorSet,
};

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    if (hf->version < 18) return 1;
    HfRef alpha = hf->register_entity(mod, &alphaDef);
    if (!alpha) return 1;
    HfEnemy enemy = {sizeof enemy};
    enemy.name = "skyeshade/alpha_scavenger/enemy";
    enemy.unit = alpha;
    enemy.like = SCAVENGER_TIER_2;
    enemy.weight = 0.33f;
    enemy.lowest_level = 4;
    if (hf->add_enemy(mod, &enemy)) return 1;
    HfRef predator = hf->register_entity(mod, &predatorDef);
    if (!predator) return 1;
    HfEnemySwap strain = {sizeof strain};
    strain.name = "skyeshade/alpha_scavenger/predator_rule";
    strain.unit = alpha;
    strain.replacement = predator;
    strain.tag = "BugPredators";
    strain.priority = 5;
    return hf->add_enemy_swap(mod, &strain);
}
