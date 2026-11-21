/* Alpha Scavenger: a new enemy tied to no vanilla one (docs/modding/enemies.md). The entity is a Scavenger copy with
 * more health. add_enemy gives it a roster row of its own in a group of its own, from difficulty 4; add_enemy_squad
 * sends that group as a patrol with Scavengers and as reinforcements. On planets of the BugPredators constellation, a
 * replacement rule (add_enemy_swap) turns each Alpha into an Alpha Predator, a second copy.
 * Build: HellForge builds the sources before each session, or scripts/Build-Mod.ps1 examples/mods/alpha-scavenger. */
#include <stddef.h>
#include "hellforge_mod.h"

#define ALPHAS "skyeshade/alpha_scavenger/alphas" /* the spawn group: a name of the mod's own */
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
    if (hf->version < 17 || hf->size < offsetof(HfApi, add_enemy_squad) + sizeof hf->add_enemy_squad) return 1;
    HfRef alpha = hf->register_entity(mod, &alphaDef);
    if (!alpha) return 1;
    HfEnemy enemy = {sizeof enemy};
    enemy.name = "skyeshade/alpha_scavenger/enemy";
    enemy.unit = alpha;
    enemy.factions = HF_ENEMY_TERMINIDS;
    enemy.group = ALPHAS;
    for (int level = 4; level <= HF_ENEMY_LEVELS; level++) enemy.weights[level - 1] = 1.0f;
    enemy.cost = 12.0f; /* twice a Scavenger's */
    if (hf->add_enemy(mod, &enemy)) return 1;
    HfEnemySquad patrol = {sizeof patrol};
    patrol.name = "skyeshade/alpha_scavenger/patrol";
    patrol.factions = HF_ENEMY_TERMINIDS;
    patrol.kind = HF_SQUAD_PATROL;
    patrol.members[0].group = ALPHAS;      patrol.members[0].count = 2;
    patrol.members[1].group = "Scavenger"; patrol.members[1].count = 5;
    patrol.weight = 1.0f;
    patrol.lowest_level = 4;
    HfEnemySquad breach = patrol;
    breach.name = "skyeshade/alpha_scavenger/breach";
    breach.kind = HF_SQUAD_ENCOUNTER;
    if (hf->add_enemy_squad(mod, &patrol) || hf->add_enemy_squad(mod, &breach)) return 1;
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
