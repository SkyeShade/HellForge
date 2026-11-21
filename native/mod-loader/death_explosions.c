/* The explosions enemies fire from their behaviour code as they die (EnemyDeathExplosion; mappings/death-explosions.json,
 * docs/internals-live.md "Enemies"). A Hulk's behaviour fires 40 at its back as it goes down and 10 from its body 2.2 s
 * later; turrets, tanks and the spore and bile bugs fire one on_dead. Each is an immediate handed to BehaviourExplosion
 * (0x4C89C0: context, type, node, r9), which queues it here and sends it to the peers (rpc_do_explosion), so the type
 * the host's hook gives is what every machine sees. Several enemies share a behaviour, and a type is shared by many
 * things, so the row is keyed by the enemy's resource (context +0: its descriptor) and the type: only that enemy's
 * explosion changes. A row of HellForge's per enemy and explosion holds the type to fire (0: none). The hook goes in
 * once a row differs from the game's. Included by director.c. */

#define DTH_FUNCTION 0x4C89C0u
#define DTH_ROW 4u

#include "death_explosions.h"
#define DTH_SITES (sizeof kDeathSites / sizeof kDeathSites[0])

/* 0x4C89C0: mov [rsp+8], rbx; mov [rsp+0x10], rbp; mov [rsp+0x18], rsi; mov [rsp+0x20], rdi; push r12; push r14; push r15;
 * sub rsp, 0x70; mov rax, [rip+0x217362B] (the stack cookie) */
static const uint8_t kDeathPin[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C,
    0x24, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B, 0x05, 0x2B, 0x36, 0x17, 0x02};

typedef void (*DeathExplosionFn)(void* context, uint32_t type, uint32_t node, uint8_t r9);
static DeathExplosionFn g_deathOriginal;
static uint32_t g_deathRows[DTH_SITES];
static uint8_t g_deathNoted[DTH_SITES];
static int g_deathState; /* 0 rows hold the game's types, not hooked; 1 hooked; -1 not as reviewed */

static void DeathRowsInit(void)
{
    static int done;
    if (done) return;
    for (size_t i = 0; i < DTH_SITES; i++) g_deathRows[i] = kDeathSites[i].type;
    done = 1;
}

/* The behaviour's explosion, for its enemy's row where one lists it: that row's type instead, none for 0. */
static void DeathExplosionDetour(void* context, uint32_t type, uint32_t node, uint8_t r9)
{
    uint64_t resource = 0;
    const uint8_t* descriptor = NULL;
    if (context != NULL && !SafeRead((uintptr_t)context, &descriptor, 8) && descriptor != NULL) SafeRead((uintptr_t)descriptor, &resource, 8);
    for (size_t i = 0; i < DTH_SITES && resource != 0; i++)
    {
        if (kDeathSites[i].resource != resource || kDeathSites[i].type != type) continue;
        uint32_t now = g_deathRows[i];
        if (now != type && !g_deathNoted[i])
        {
            g_deathNoted[i] = 1;
            Note("death explosions: row %u (enemy %016llX) fires %s%u instead of %u", kDeathSites[i].id, (unsigned long long)resource,
                now == 0 ? "no explosion, type " : "explosion ", now, type);
        }
        if (now == 0) return;
        type = now;
        break;
    }
    g_deathOriginal(context, type, node, r9);
}

/* A row by its id, or 0. Always ready: the rows are HellForge's. */
static uintptr_t DeathRowOf(uint64_t id, int* ready)
{
    *ready = 1;
    DeathRowsInit();
    for (size_t i = 0; i < DTH_SITES; i++)
        if (kDeathSites[i].id == id) return (uintptr_t)&g_deathRows[i];
    return 0;
}

/* Why a row's write is refused, or NULL (the reference itself is checked as every explosion field's is: refs.c). */
static const char* DeathRefuses(uint32_t offset)
{
    return offset != 0 ? "only a death explosion's type is edited" : g_deathState == -1 ? "game.dll's BehaviourExplosion is not the reviewed one" : NULL;
}

/* The game-thread update: once a row differs from the game's, the hook (its first bytes as reviewed, a minute for the copy
 * protection to decode them). */
static void DeathExplosionsTick(void)
{
    static ULONGLONG first;
    if (g_deathState != 0) return;
    DeathRowsInit();
    size_t changed = 0;
    for (size_t i = 0; i < DTH_SITES; i++) changed += g_deathRows[i] != kDeathSites[i].type;
    if (changed == 0) return;
    size_t size = 0;
    uintptr_t game = g_directorImage(&size);
    if (game == 0 || size != DIR_IMAGE || strcmp(g_build, "25480438")) return;
    if (!first) first = GetTickCount64();
    uint8_t b[sizeof kDeathPin];
    if (SafeRead(game + DTH_FUNCTION, b, sizeof b) || memcmp(b, kDeathPin, sizeof b))
    {
        if (GetTickCount64() - first < 60000) return;
        g_deathState = -1;
        Note("death explosions: game.dll's BehaviourExplosion is not the reviewed one: enemies keep their own death explosions");
        return;
    }
    g_deathState = LoaderHook("the behaviour explosion", game + DTH_FUNCTION, (void*)DeathExplosionDetour, (void**)&g_deathOriginal) ? -1 : 1;
    Note(g_deathState == 1 ? "death explosions: hooked BehaviourExplosion (%zu of %zu rows changed)" : "death explosions: could not hook BehaviourExplosion (%zu of %zu rows changed)",
        changed, DTH_SITES);
}
