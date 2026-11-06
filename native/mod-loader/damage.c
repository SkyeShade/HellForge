/* New damage types (SDK 3, docs/modding/content.md "Projectiles and damage"): copies of vanilla DamageInfo rows under ids of their own, so a
 * projectile can deal damage no vanilla one shares. Included by hellforge_modloader.c, before projectiles.c.
 *
 * The game's damage list (build 25480438, game.dll 2E2C3B7C...):
 *   game.dll + 0x37C60C0  DamageInfo id -> 76-byte row {+0 id, +4 standard damage, +8 durable damage, +0x0C AP x4,
 *              +0x1C.. statuses} (HD2Runtime direct-hit-damage), 8-byte pointers for ids 1..649. Slot 650 would be the
 *              static default row at +0x37C7510 (id 0's), so there is no spare slot: HellForge moves the list.
 *   loader 0x11F8E50.. clears exactly 0x1450 bytes (650 slots, kDamageClearPin) and fills ids 1..649; a reload therefore
 *              never clears slots above 649 of the moved list.
 *   489 instructions address the list (damage_sites.h): 477 indexed loads [image base + id*8 + 0x37C60C0] and 12
 *              rip-relative (slots 0, 1 and 633), among them the leaf GetDamageInfo 0x11F8EA0, which has no .pdata entry
 *              and was found by scanning .text. A raw scan for the absolute displacement finds the same 477.
 * Moving it: a block within 2 GB above game.dll (the indexed loads add their displacement to the image base) gets the
 * 650 vanilla pointers, then every site's displacement is rewritten to the block. Each site is checked first; any
 * difference moves nothing. Both lists hold the same rows while the sites change, so a read through either is right;
 * the new rows are written last. */

typedef struct DamageSite { uint32_t rva; uint8_t dispAt, size, rip; uint16_t slot; } DamageSite;
#include "damage_sites.h"

#define DMG_TABLE 0x37C60C0u
#define DMG_VANILLA 650u
#define DMG_MAX_NEW 512u /* a damage type for each of REG_MAX_WEAPONS weapons: the list is HellForge's own once moved */
#define DMG_ROW 76u
#define DMG_CLEAR 0x11F8E72u /* 41 B8 50 14 00 00: mov r8d, 0x1450 (650 slots) */

static const uint8_t kDamageClearPin[6] = {0x41, 0xb8, 0x50, 0x14, 0x00, 0x00};

typedef struct Damage
{
    HfMod* mod;
    char name[128];
    uint32_t id, base;
    float scale;
    uint8_t* row;
} Damage;

static Damage g_damages[DMG_MAX_NEW];
static uint32_t g_damageCount;
static int g_damageState; /* 0 waiting, 1 installed, -1 refused */
static uint8_t* g_damageTable;

static uint32_t ApiAddDamage(HfMod* mod, const HfDamage* def)
{
    if (mod == NULL || def == NULL || def->size < offsetof(HfDamage, scale) + sizeof def->scale)
    {
        ApiLog(mod, "add_damage: no damage, or its size is not set (HfDamage d = {sizeof d})");
        return 0;
    }
    char identity[256];
    const char* name = def->name != NULL && def->name[0] ? Identity(mod, def->name, identity, sizeof identity) : "(unnamed)";
    int live = g_liveSetup != NULL && g_liveSetup == mod; /* a mod set up while the game runs (live_content.c) */
    if (!live && (g_damageState != 0 || g_fired[HF_EVENT_PROJECTILES_READY])) { ApiLog(mod, "add_damage(%s): too late; call it from hellforge_mod_init", name); return 0; }
    if (!live && g_damageCount >= DMG_MAX_NEW) { ApiLog(mod, "add_damage(%s): mods added %u damage types already", name, DMG_MAX_NEW); return 0; }
    if (def->base == 0 || def->base >= DMG_VANILLA) { ApiLog(mod, "add_damage(%s): base %u is not a vanilla damage id (1 .. 649)", name, def->base); return 0; }
    float scale = def->scale == 0.0f ? 1.0f : def->scale;
    if (!(scale > 0.0f && scale <= 1000.0f)) { ApiLog(mod, "add_damage(%s): scale must be above 0 and at most 1000", name); return 0; }
    for (uint32_t i = 0; i < g_damageCount; i++)
        if (!LiveGone(LC_DAMAGE, i) && strcmp(g_damages[i].name, name) == 0) { ApiLog(mod, "add_damage(%s): already added by %s", name, g_damages[i].mod->id); return 0; }
    uint32_t index = g_damageCount;
    Damage* d = live ? (Damage*)LiveEntry(LC_DAMAGE, mod, name, &index) : &g_damages[index];
    if (d == NULL) return 0;
    memset(d, 0, sizeof *d);
    d->mod = mod;
    snprintf(d->name, sizeof d->name, "%s", name);
    d->id = DMG_VANILLA + index;
    d->base = def->base;
    d->scale = scale;
    MemoryBarrier(); /* the entry is whole before the count shows it */
    if (index == g_damageCount) g_damageCount++;
    ApiLog(mod, "adding damage %s as id %u: a copy of id %u, x%g", d->name, d->id, d->base, (double)scale);
    return d->id;
}

static uintptr_t ApiDamageRow(uint32_t id)
{
    if (g_damageState != 1) return 0;
    for (uint32_t i = 0; i < g_damageCount; i++) if (g_damages[i].id == id && !LiveTomb(LC_DAMAGE, i)) return (uintptr_t)g_damages[i].row;
    return 0;
}

/* Whether a damage id resolves in this game: vanilla, or added and installed. */
static int DamageUsable(uint32_t id)
{
    if (id < DMG_VANILLA) return 1;
    return g_damageState == 1 && id < DMG_VANILLA + g_damageCount;
}

static int32_t SiteDisplacement(uintptr_t table, uintptr_t game, const DamageSite* s)
{
    return s->rip ? (int32_t)(int64_t)(table + s->slot * 8u - (game + s->rva + s->size)) : (int32_t)(int64_t)(table + s->slot * 8u - game);
}

static const char* DamageSitesFailed(uintptr_t game)
{
    uint8_t clear[sizeof kDamageClearPin];
    if (SafeRead(game + DMG_CLEAR, clear, sizeof clear) || memcmp(clear, kDamageClearPin, sizeof clear) != 0)
        return "the damage list's loader does not clear the reviewed 650 slots";
    for (size_t i = 0; i < sizeof kDamageSites / sizeof kDamageSites[0]; i++)
    {
        int32_t disp;
        if (SafeRead(game + kDamageSites[i].rva + kDamageSites[i].dispAt, &disp, 4) ||
            disp != SiteDisplacement(game + DMG_TABLE, game, &kDamageSites[i]))
            return "an instruction addressing the damage list is not the reviewed one";
    }
    return NULL;
}

/* Memory within 2 GB of game.dll, for the moved lists and tables and the stubs: the indexed loads take a signed 32-bit
 * offset from the image base ([base + index*scale + disp32]) and the stubs' rel32 jumps and RIP-relative operands are
 * signed too. Above the image first; below it when there is no room above (game.dll mapped near the top of the address
 * space, as on a tester's PC where level 11 was refused for it).
 * NEAR_ARENA_BYTES are reserved there as soon as game.dll is decoded (ReserveNearGame), before the game's own large
 * allocations (the enlarged physics pools among them) can fill that window; AllocateNearGame hands out whole pages of it
 * (stubs are made executable page by page) and searches only when it is missing or full. Everything the loader moves
 * comes to well under 1 MB. */
#define NEAR_ARENA_BYTES 0x400000u
static SRWLOCK g_nearLock = SRWLOCK_INIT;
static uintptr_t g_nearGame;
static uint8_t* g_nearArena;
static size_t g_nearUsed;

/* Above the image (`image` bytes at game): the first free region within 2 GB that takes the block. */
static uint8_t* FindNearAbove(uintptr_t game, size_t image, size_t bytes, DWORD type)
{
    uintptr_t at = (game + image + 0xFFFF) & ~(uintptr_t)0xFFFF;
    for (; at + bytes < game + 0x7FFF0000u; at += 0x10000)
    {
        MEMORY_BASIC_INFORMATION region;
        if (!VirtualQuery((void*)at, &region, sizeof region)) break;
        if (region.State != MEM_FREE) { at = (((uintptr_t)region.BaseAddress + region.RegionSize + 0xFFFF) & ~(uintptr_t)0xFFFF) - 0x10000; continue; }
        void* block = VirtualAlloc((void*)at, bytes, type, PAGE_READWRITE);
        if (block != NULL) return (uint8_t*)block;
    }
    return NULL;
}

/* below: within 2 GB of the image's end too, since RIP-relative operands and jumps anywhere in it reach the block */
static uint8_t* FindNearBelow(uintptr_t game, size_t image, size_t bytes, DWORD type)
{
    uintptr_t at;
    uintptr_t lowest = game + image > 0x7FFF0000u + 0x10000u ? game + image - 0x7FFF0000u : 0x10000;
    for (at = (game - bytes) & ~(uintptr_t)0xFFFF; at >= lowest && at < game;)
    {
        MEMORY_BASIC_INFORMATION region;
        if (!VirtualQuery((void*)at, &region, sizeof region)) break;
        if (region.State == MEM_FREE)
        {
            void* block = VirtualAlloc((void*)at, bytes, type, PAGE_READWRITE);
            if (block != NULL) return (uint8_t*)block;
            at -= 0x10000; /* free here, but not for all of it: lower */
            continue;
        }
        /* in use: below its allocation, with room for the block under it */
        uintptr_t base = (uintptr_t)region.AllocationBase != 0 ? (uintptr_t)region.AllocationBase : (uintptr_t)region.BaseAddress;
        if (base < lowest + bytes) break;
        at = (base - bytes) & ~(uintptr_t)0xFFFF;
    }
    return NULL;
}

/* A region of `bytes` within 2 GB of game.dll (type: MEM_RESERVE alone, or with MEM_COMMIT), or NULL. */
static uint8_t* FindNearGame(uintptr_t game, size_t bytes, DWORD type)
{
    size_t image = 0;
    ApiModuleBase("game.dll", &image);
    uint8_t* block = FindNearAbove(game, image, bytes, type);
    return block != NULL ? block : FindNearBelow(game, image, bytes, type);
}

/* Once game.dll is decoded: the loader's own space near it, and where both are in modloader.log. */
static void ReserveNearGame(uintptr_t game)
{
    if (game == 0 || g_nearArena != NULL) return;
    size_t image = 0;
    ApiModuleBase("game.dll", &image);
    uint8_t* arena = FindNearGame(game, NEAR_ARENA_BYTES, MEM_RESERVE);
    AcquireSRWLockExclusive(&g_nearLock);
    if (g_nearArena == NULL) { g_nearGame = game; g_nearArena = arena; g_nearUsed = 0; }
    ReleaseSRWLockExclusive(&g_nearLock);
    if (arena != NULL)
        Note("near game.dll: the image at 0x%llx (%llu MB), %u MB reserved at 0x%llx, %s it", (unsigned long long)game,
            (unsigned long long)(image >> 20), NEAR_ARENA_BYTES >> 20, (unsigned long long)(uintptr_t)arena, (uintptr_t)arena > game ? "above" : "below");
    else
        Note("near game.dll: the image at 0x%llx (%llu MB); no %u MB within 2 GB of it to reserve, moved tables search as they install",
            (unsigned long long)game, (unsigned long long)(image >> 20), NEAR_ARENA_BYTES >> 20);
}

/* Whole pages within 2 GB of game.dll, read-write and zeroed: from the reserved space, else found as they are asked for. */
static uint8_t* AllocateNearGame(uintptr_t game, size_t bytes)
{
    size_t pages = (bytes + 0xFFF) & ~(size_t)0xFFF;
    uint8_t* block = NULL;
    AcquireSRWLockExclusive(&g_nearLock);
    if (g_nearArena != NULL && game == g_nearGame && pages <= NEAR_ARENA_BYTES - g_nearUsed)
    {
        block = (uint8_t*)VirtualAlloc(g_nearArena + g_nearUsed, pages, MEM_COMMIT, PAGE_READWRITE);
        if (block != NULL) g_nearUsed += pages;
    }
    ReleaseSRWLockExclusive(&g_nearLock);
    if (block == NULL) block = FindNearGame(game, bytes, MEM_COMMIT | MEM_RESERVE);
    return block;
}

static int RefuseDamage(const char* why)
{
    g_damageState = -1;
    Note("damage: %u new type(s) refused: %s; projectiles keep their own damage", g_damageCount, why);
    for (uint32_t i = 0; i < g_damageCount; i++) ApiLog(g_damages[i].mod, "damage %s (id %u) not added: %s", g_damages[i].name, g_damages[i].id, why);
    return 1;
}

/* A new type's row (row: where it goes): its base's (the game's own list keeps every vanilla row), its damage scaled.
 * NULL, or why not. */
static const char* DamageRow(Damage* d, uint8_t* row, uintptr_t game)
{
    uint64_t base = 0;
    if (SafeRead(game + DMG_TABLE + (uintptr_t)d->base * 8, &base, 8) || base == 0 || SafeRead(base, row, DMG_ROW) || *(uint32_t*)row != d->base)
        return "a base damage row does not name its id";
    uint32_t standard, durable;
    memcpy(&standard, row + 4, 4);
    memcpy(&durable, row + 8, 4);
    standard = (uint32_t)(standard * d->scale + 0.5f);
    durable = (uint32_t)(durable * d->scale + 0.5f);
    memcpy(row, &d->id, 4);
    memcpy(row + 4, &standard, 4);
    memcpy(row + 8, &durable, 4);
    d->row = row;
    return NULL;
}

/* The instructions that address the list, pointed at HellForge's. NULL, or why not. */
static const char* DamageMove(uintptr_t game, const uint8_t* table, PutFn put)
{
    for (size_t i = 0; i < sizeof kDamageSites / sizeof kDamageSites[0]; i++)
    {
        int32_t disp = SiteDisplacement((uintptr_t)table, game, &kDamageSites[i]);
        if (put(game + kDamageSites[i].rva + kDamageSites[i].dispAt, &disp, 4))
        {
            /* Unreachable in practice (the pages are writable); the list stays usable through both copies. */
            Note("damage: could not rewrite the instruction at 0x%x; %zu of %zu moved", kDamageSites[i].rva, i, sizeof kDamageSites / sizeof kDamageSites[0]);
            return "an instruction could not be rewritten";
        }
    }
    return NULL;
}

static void DamageInstalledLog(void)
{
    for (uint32_t i = 0; i < g_damageCount; i++)
    {
        uint32_t standard, durable;
        if (g_damages[i].row == NULL || LiveTomb(LC_DAMAGE, i)) continue;
        memcpy(&standard, g_damages[i].row + 4, 4);
        memcpy(&durable, g_damages[i].row + 8, 4);
        ApiLog(g_damages[i].mod, "damage %s is id %u: %u standard / %u durable", g_damages[i].name, g_damages[i].id, standard, durable);
    }
}

/* Polled with the projectile list (InstallProjectiles) until it returns 1: moves the list and adds the rows once the
 * game has loaded its damage settings. pinsReady: the caller's grace period for code still being decoded is over. */
static int InstallDamage(uintptr_t game, int pinsReady)
{
    if (g_damageState != 0) return 1;
    if (g_damageCount == 0) { g_damageState = 1; return 1; }
    uint64_t first = 0;
    if (SafeRead(game + DMG_TABLE + 8, &first, 8) || first == 0) return 0; /* not loaded yet */
    const char* why = DamageSitesFailed(game);
    if (why != NULL) return pinsReady ? RefuseDamage(why) : 0;
    size_t bytes = (size_t)(DMG_VANILLA + DMG_MAX_NEW) * 8 + (size_t)g_damageCount * DMG_ROW;
    uint8_t* table = AllocateNearGame(game, bytes);
    if (table == NULL) return RefuseDamage("no memory within 2 GB of game.dll for the moved list");
    if (SafeRead(game + DMG_TABLE, table, DMG_VANILLA * 8)) return RefuseDamage("the damage list is unreadable");
    uint8_t* rows = table + (DMG_VANILLA + DMG_MAX_NEW) * 8;
    for (uint32_t i = 0; i < g_damageCount; i++)
        if ((why = DamageRow(&g_damages[i], rows + (size_t)i * DMG_ROW, game)) != NULL) return RefuseDamage(why);
    if ((why = DamageMove(game, table, ApiWrite)) != NULL) return RefuseDamage(why);
    for (uint32_t i = 0; i < g_damageCount; i++) memcpy(table + g_damages[i].id * 8, &g_damages[i].row, 8);
    g_damageTable = table;
    g_damageState = 1;
    Note("damage: the list moved to 0x%llx (%zu instructions) with %u new type(s)", (unsigned long long)(uintptr_t)table,
        sizeof kDamageSites / sizeof kDamageSites[0], g_damageCount);
    DamageInstalledLog();
    return 1;
}
