/* New projectile types (SDK 3, docs/modding/content.md "Projectiles and damage"): copies of vanilla projectile settings under type
 * numbers of their own, so a weapon can fire a projectile no vanilla weapon shares. Data only: the game reads the new
 * row like any other, a weapon fires it because its own record names it (HfWeapon.projectile, registry.c), and its
 * damage is the DamageInfo its row names (+0x3C; damage.c adds new ones). Included by hellforge_modloader.c.
 *
 * The game's projectile list (build 25480438, game.dll 2E2C3B7C...):
 *   game.dll + 0x37C7670  ProjectileType -> settings row: 8-byte pointers, slot 0 empty (type 0 uses a static default
 *              row, +0x37C7560), slots 1..350 pointing at the 272-byte rows of the loaded settings file. Every reader
 *              indexes it with the type and no bound ([game + 0x37C7670 + type*8], or a lea of the base + index).
 *   loader 0x11EC5A0 / unloader 0x11EC760: clear exactly 0xAF8 bytes (351 slots) and fill slots 1..350.
 *   slots 351..353 (+0x37C8168): padding before the next table (slot 354), so the list has room for 3 more types in
 *              place; HellForge moves it instead, as it does the damage and stratagem lists, for up to 64.
 *   66 instructions address the list (projectile_sites.h): 37
 *              indexed loads [image base + type*8 + 0x37C7670] and 29 rip-relative (slot 0, the loader's slot 1 check,
 *              one read of slot 216), among them the leaf GetProjectileInfo 0x11EC7C0 (no .pdata entry; a raw scan of
 *              the code found it). No data in game.dll points into the list.
 *   Two lookups bound the type, cmp type, 351; jae: 0x75A376 (in 0x75A0D0; above it, the function returns null) and
 *              0x1829374 (in 0x1829110; above it, another branch). HellForge raises both to 351 + the new types.
 * Moving it: a block within 2 GB above game.dll (the indexed loads add their displacement to the image base) gets the
 * 351 vanilla pointers and the new rows' pointers, then every site's displacement is rewritten to the block, then the
 * bounds rise. Each site is checked first; any difference moves nothing. The loader clears 351 slots of whichever list
 * its instructions name, so a reload keeps the new slots.
 * The row's +0 is the type: SpawnProjectile stores it in the pool's per-slot type array and every later lookup of the
 * flying shot indexes the list with it, so the new row names its own type and those lookups read it.
 * game.dll's code is decoded in parts after GAME_LOADED, so the pins are checked once the list has loaded, with a grace
 * period for bytes still being decoded (one session saw the bounds undecoded at GAME_LOADED, the next did not). */

#define PRJ_TABLE 0x37C7670u
#define PRJ_VANILLA 351u
#define PRJ_MAX_NEW 64u
#define PRJ_ROW 272u
#define PRJ_DAMAGE_AT 0x3Cu
#define PRJ_VISUAL_AT 0x48u
#define PRJ_VISUAL_SIZE 0x28u /* +0x48 .. +0x6F: spawn effects and their parameters */
#define PRJ_FLIGHT_AT 0x70u    /* the effect drawing it in flight (the Talon's bolt; the Liberator has none) */
#define PRJ_BOUND_A 0x75A376u  /* 48 3D imm32: cmp rax, 351 */
#define PRJ_BOUND_B 0x1829374u /* 49 81 FE imm32: cmp r14, 351 */
#define PRJ_CLEAR_A 0x11EC61Eu /* 41 B8 F8 0A 00 00: mov r8d, 0xAF8 (the loader's clear) */
#define PRJ_CLEAR_B 0x11EC79Eu /* the unloader's */
#define PRJ_PIN_GRACE_MS 20000u

static const uint8_t kBoundAPin[12] = {0x48, 0x3d, 0x5f, 0x01, 0x00, 0x00, 0x0f, 0x83, 0x1f, 0x01, 0x00, 0x00};
static const uint8_t kBoundBPin[13] = {0x49, 0x81, 0xfe, 0x5f, 0x01, 0x00, 0x00, 0x0f, 0x83, 0xe4, 0x00, 0x00, 0x00};
static const uint8_t kClearPin[6] = {0x41, 0xb8, 0xf8, 0x0a, 0x00, 0x00};

typedef struct Projectile
{
    HfMod* mod;
    char name[128];
    uint32_t type, base, damage, visualFrom;
    uint64_t spawnEffect, flightEffect;
    uint8_t* row; /* HellForge's row, once installed */
} Projectile;

#include "projectile_sites.h"

static Projectile g_projectiles[PRJ_MAX_NEW];
static uint32_t g_projectileCount;
static int g_projectileState; /* 0 waiting, 1 installed, -1 refused (the build is not as reviewed) */
static uint8_t* g_projectileTable; /* HellForge's moved list, once installed */

/* The projectile list the game reads now: HellForge's moved copy, else the game's. */
static uintptr_t ProjectileTable(uintptr_t game) { return g_projectileTable != NULL ? (uintptr_t)g_projectileTable : game + PRJ_TABLE; }

/* The first added type of that name; NULL none. */
static const Projectile* ProjectileNamed(const char* name)
{
    for (uint32_t i = 0; i < g_projectileCount; i++) if (strcmp(g_projectiles[i].name, name) == 0) return &g_projectiles[i];
    return NULL;
}

static uint32_t ApiAddProjectile(HfMod* mod, const HfProjectile* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfProjectile, spawn_effect) + sizeof def->spawn_effect)
    {
        ApiLog(mod, "add_projectile: no projectile, or its size is not set (HfProjectile p = {sizeof p})");
        return 0;
    }
    char identity[256];
    const char* name = def->name != NULL && def->name[0] ? Identity(mod, def->name, identity, sizeof identity) : "(unnamed)";
    if (g_fired[HF_EVENT_PROJECTILES_READY] || g_projectileState != 0)
    {
        ApiLog(mod, "add_projectile(%s): too late; call it from hellforge_mod_init", name);
        return 0;
    }
    if (g_projectileCount >= PRJ_MAX_NEW)
    {
        ApiLog(mod, "add_projectile(%s): mods added %u new projectile types already (HellForge's room)", name, PRJ_MAX_NEW);
        return 0;
    }
    if (def->base == 0 || def->base >= PRJ_VANILLA || def->visual_from >= PRJ_VANILLA)
    {
        ApiLog(mod, "add_projectile(%s): base %u / visual_from %u is not a vanilla projectile type (1 .. 350)", name, def->base, def->visual_from);
        return 0;
    }
    if (def->damage != 0 && def->damage >= DMG_VANILLA + g_damageCount)
    {
        ApiLog(mod, "add_projectile(%s): damage %u is neither a vanilla damage id nor one a mod added", name, def->damage);
        return 0;
    }
    const Projectile* added = ProjectileNamed(name);
    if (added != NULL)
    {
        ApiLog(mod, "add_projectile(%s): already added by %s", name, added->mod->id);
        return 0;
    }
    Projectile* p = &g_projectiles[g_projectileCount];
    memset(p, 0, sizeof *p);
    p->mod = mod;
    snprintf(p->name, sizeof p->name, "%s", name);
    p->type = PRJ_VANILLA + g_projectileCount;
    p->base = def->base;
    p->damage = def->damage;
    p->visualFrom = def->visual_from;
    p->spawnEffect = def->spawn_effect;
    if (def->size >= offsetof(HfProjectile, flight_effect) + sizeof def->flight_effect) p->flightEffect = def->flight_effect;
    g_projectileCount++;
    ApiLog(mod, "adding projectile %s as type %u: a copy of type %u%s%s", p->name, p->type, p->base,
        p->damage ? ", its own damage" : "", p->visualFrom ? ", the look of another type" : "");
    return p->type;
}

static uintptr_t ApiProjectileRow(uint32_t type)
{
    if (g_projectileState != 1) return 0;
    for (uint32_t i = 0; i < g_projectileCount; i++)
        if (g_projectiles[i].type == type) return (uintptr_t)g_projectiles[i].row;
    return 0;
}

/* For the weapon registry: whether a weapon may be pointed at this projectile type now (vanilla, or added and not
 * refused). An added type still pending is allowed; if it is refused later, RevertWeaponProjectiles undoes it. */
static int ProjectileUsable(uint32_t type)
{
    if (type > 0 && type < PRJ_VANILLA) return 1;
    return type >= PRJ_VANILLA && type < PRJ_VANILLA + g_projectileCount && g_projectileState >= 0;
}

static int PinIs(uintptr_t address, const uint8_t* bytes, size_t size)
{
    uint8_t current[32];
    return size <= sizeof current && !SafeRead(address, current, size) && memcmp(current, bytes, size) == 0;
}

/* Every assumption above, checked on this game before anything is written; any difference refuses all new types. */
static const char* ProjectilePinsFailed(uintptr_t game)
{
    if (!PinIs(game + PRJ_CLEAR_A, kClearPin, sizeof kClearPin) || !PinIs(game + PRJ_CLEAR_B, kClearPin, sizeof kClearPin))
        return "the projectile list's loader does not clear the reviewed 351 slots";
    if (!PinIs(game + PRJ_BOUND_A, kBoundAPin, sizeof kBoundAPin) || !PinIs(game + PRJ_BOUND_B, kBoundBPin, sizeof kBoundBPin))
        return "the projectile type bounds are not the reviewed code";
    for (size_t i = 0; i < sizeof kProjectileSites / sizeof kProjectileSites[0]; i++)
    {
        int32_t disp;
        if (SafeRead(game + kProjectileSites[i].rva + kProjectileSites[i].dispAt, &disp, 4) ||
            disp != SiteDisplacement(game + PRJ_TABLE, game, &kProjectileSites[i]))
            return "an instruction addressing the projectile list is not the reviewed one";
    }
    return NULL;
}

static void StartProjectiles(void) {}

static int RefuseProjectiles(const char* why)
{
    g_projectileState = -1;
    Note("projectiles: %u new type(s) refused: %s; weapons fire their base's projectiles", g_projectileCount, why);
    for (uint32_t i = 0; i < g_projectileCount; i++)
        ApiLog(g_projectiles[i].mod, "projectile %s (type %u) not added: %s", g_projectiles[i].name, g_projectiles[i].type, why);
    RevertWeaponProjectiles();
    return 1;
}

/* The type's own spawn and flight effects and damage over its base's (a damage that was not added is logged). */
static void ProjectileRowOwn(Projectile* p)
{
    if (p->spawnEffect != 0) memcpy(p->row + PRJ_VISUAL_AT, &p->spawnEffect, 8);
    if (p->flightEffect != 0) memcpy(p->row + PRJ_FLIGHT_AT, &p->flightEffect, 8);
    if (p->damage != 0 && DamageUsable(p->damage)) memcpy(p->row + PRJ_DAMAGE_AT, &p->damage, 4);
    else if (p->damage != 0) ApiLog(p->mod, "projectile %s keeps its base's damage: damage %u was not added", p->name, p->damage);
}

/* Each new type's row in `rows`: its base's, with its look, effects and damage. 1 built; 0 a row the game has not
 * loaded yet (tried again); -1 a base row that does not name its type. */
static int ProjectileRowsBuild(uintptr_t game, uint8_t* rows)
{
    for (uint32_t i = 0; i < g_projectileCount; i++)
    {
        Projectile* p = &g_projectiles[i];
        uint64_t base = 0, look = 0;
        if (SafeRead(game + PRJ_TABLE + (uintptr_t)p->base * 8, &base, 8) || base == 0) return 0;
        if (p->visualFrom && (SafeRead(game + PRJ_TABLE + (uintptr_t)p->visualFrom * 8, &look, 8) || look == 0)) return 0;
        p->row = rows + i * PRJ_ROW;
        if (SafeRead(base, p->row, PRJ_ROW) || *(uint32_t*)p->row != p->base) return -1;
        if (look != 0 && SafeRead(look + PRJ_VISUAL_AT, p->row + PRJ_VISUAL_AT, PRJ_VISUAL_SIZE)) return 0;
        ProjectileRowOwn(p);
        memcpy(p->row, &p->type, 4);
    }
    return 1;
}

/* The whole list first (the vanilla pointers, then the new rows'), then the instructions, then the bounds (*bound): no
 * code reads the new list before it is complete, nor a new type before its instructions read the new list. NULL when
 * done, else why not. */
static const char* ProjectileListInstall(uintptr_t game, uint8_t* table, uint32_t* bound)
{
    if (SafeRead(game + PRJ_TABLE, table, PRJ_VANILLA * 8)) return "the projectile list is unreadable";
    for (uint32_t i = 0; i < g_projectileCount; i++) memcpy(table + (size_t)g_projectiles[i].type * 8, &g_projectiles[i].row, 8);
    for (size_t i = 0; i < sizeof kProjectileSites / sizeof kProjectileSites[0]; i++)
    {
        int32_t disp = SiteDisplacement((uintptr_t)table, game, &kProjectileSites[i]);
        if (ApiWrite(game + kProjectileSites[i].rva + kProjectileSites[i].dispAt, &disp, 4))
        {
            /* Unreachable in practice (the pages are writable); the list stays usable through both copies. */
            Note("projectiles: could not rewrite the instruction at 0x%x; %zu of %zu moved", kProjectileSites[i].rva, i,
                sizeof kProjectileSites / sizeof kProjectileSites[0]);
            return "an instruction could not be rewritten";
        }
    }
    g_projectileTable = table;
    *bound = PRJ_VANILLA + g_projectileCount;
    if (ApiWrite(game + PRJ_BOUND_A + 2, bound, 4) || ApiWrite(game + PRJ_BOUND_B + 3, bound, 4)) return "could not raise the type bounds";
    return NULL;
}

static void ProjectileLogs(void)
{
    for (uint32_t i = 0; i < g_projectileCount; i++)
    {
        uint32_t damage;
        uint64_t spawn, flight;
        memcpy(&damage, g_projectiles[i].row + PRJ_DAMAGE_AT, 4);
        memcpy(&spawn, g_projectiles[i].row + PRJ_VISUAL_AT, 8);
        memcpy(&flight, g_projectiles[i].row + PRJ_FLIGHT_AT, 8);
        ApiLog(g_projectiles[i].mod, "projectile %s is type %u (damage id %u, spawn effect %016llx, flight effect %016llx)",
            g_projectiles[i].name, g_projectiles[i].type, damage, (unsigned long long)spawn, (unsigned long long)flight);
    }
}

/* Called from the watch loop until it returns 1: moves the damage list if mods added damage, then installs the rows,
 * once the game has loaded its settings. */
static int InstallProjectiles(void)
{
    if (g_projectileState != 0) return 1;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    uint64_t first = 0;
    if (game == 0 || SafeRead(game + PRJ_TABLE + 8, &first, 8) || first == 0) return 0; /* not loaded yet */
    static ULONGLONG listSeen;
    if (listSeen == 0) listSeen = GetTickCount64();
    int graceOver = GetTickCount64() - listSeen >= PRJ_PIN_GRACE_MS;
    if (!InstallDamage(game, graceOver)) return 0;
    if (g_projectileCount == 0) { g_projectileState = 1; return 1; }
    const char* why = ProjectilePinsFailed(game);
    if (why != NULL) return graceOver ? RefuseProjectiles(why) : 0;
    /* the moved list and every new row in one block, kept for the session (the game reads it until it exits) */
    static uint8_t* table;
    if (table == NULL) table = AllocateNearGame(game, (size_t)(PRJ_VANILLA + PRJ_MAX_NEW) * 8 + (size_t)PRJ_MAX_NEW * PRJ_ROW);
    if (table == NULL) return RefuseProjectiles("no memory within 2 GB of game.dll for the moved list");
    uint8_t* rows = table + (PRJ_VANILLA + PRJ_MAX_NEW) * 8;
    int built = ProjectileRowsBuild(game, rows);
    if (built < 0) return RefuseProjectiles("a base row does not name its type");
    if (built == 0) return 0;
    uint32_t bound = 0;
    why = ProjectileListInstall(game, table, &bound);
    if (why != NULL) return RefuseProjectiles(why);
    g_projectileState = 1;
    Note("projectiles: the list moved to 0x%llx (%zu instructions) with %u new type(s) (bounds raised to %u)",
        (unsigned long long)(uintptr_t)table, sizeof kProjectileSites / sizeof kProjectileSites[0], g_projectileCount, bound);
    ProjectileLogs();
    return 1;
}
