/* The network apply switch (co-op research 2026-10-21, fix 1). Every machine
 * applies the state it receives for another machine's network object through one function, game.dll 0xBC2D30: at the
 * object's creation (0xFDC29B), for received data every frame (0xFDDE6A, 0xFDDEB3) and at a migration to this machine
 * (0xFDBE10, 0x581DA9: the full state). It jumps by the object's network type index through a table of the 1,855 vanilla
 * types: `0xBC2DA1 cmp eax,0x73E; ja 0xBC6859` (the exit) and `0xBC2DB3 mov ecx,[rdx+rax*4+0xBD9EB8]; add rcx,rdx;
 * jmp rcx`, rdx the image base (0xBC2DAC). The network types RegisterNetworkTypes adds (1855 and up) took the exit: another
 * machine's copy of every added entity got none of its state (a friend's barrage fired nothing or this machine's last
 * barrage's block, an added weapon never pulled its trigger, an added Pelican never moved, an added missile never flew).
 * The table is the only structure on the path sized to the vanilla types (exe and game.dll, code and memory).
 * The table moves into HellForge's space near game.dll (an image-relative u32 per type) with an entry for each added type:
 * its base type's case, as a copy keeps its base's component list and field layout (registry.c copies the base's map
 * row). A type whose component list is not its base's keeps the exit: a migration applies the whole state, and a case
 * would write through a missing component's index -1. Then the displacement, then the bound. Installed from the watch loop
 * once the code is decoded, before any co-op session: the switch runs only for other machines' objects, and the
 * displacement (0xBC2DB6..B9) straddles two qwords, so it is not written while a friend's objects arrive. */

#define NET_SWITCH_BOUND 0xBC2DA1u   /* 3D imm32 0F 87 rel32: cmp eax, count - 1; ja exit */
#define NET_SWITCH_BASE 0xBC2DACu    /* 48 8D 15 rel32: lea rdx, [image base] */
#define NET_SWITCH_LOAD 0xBC2DB3u    /* 8B 8C 82 disp32 48 03 CA FF E1: mov ecx, [rdx+rax*4+table]; add rcx, rdx; jmp rcx */
#define NET_SWITCH_TABLE 0xBD9EB8u
#define NET_SWITCH_EXIT 0xBC6859u
#define NET_VANILLA_TYPES 0x73Fu
#define NET_CASES_FIRST 0xBC2DBFu    /* every case lies in the switch's own code (the exit too), right after the jmp */
#define NET_CASES_LAST 0xBD9E0Eu
#define NET_TABLE_SLOTS (NET_VANILLA_TYPES + REG_MAX_WEAPONS)

static const uint8_t kNetBoundPin[11] = {0x3D, 0x3E, 0x07, 0x00, 0x00, 0x0F, 0x87, 0xAD, 0x3A, 0x00, 0x00};
static const uint8_t kNetBasePin[7] = {0x48, 0x8D, 0x15, 0x4D, 0xD2, 0x43, 0xFF};
static const uint8_t kNetLoadPin[12] = {0x8B, 0x8C, 0x82, 0xB8, 0x9E, 0xBD, 0x00, 0x48, 0x03, 0xCA, 0xFF, 0xE1};
static const uint8_t kNetExitPin[5] = {0x48, 0x8B, 0x5C, 0x24, 0x40};

static int g_netSwitchState; /* (declared in hellforge_modloader.c too) 0 waiting, 1 installed (or nothing to add), -1 refused */
static uint32_t* g_netSwitchTable; /* HellForge's table, once installed */

static int RefuseNetSwitch(const char* why)
{
    g_netSwitchState = -1;
    Note("network: the apply switch is not extended (%s); another machine's copies of the %u added network type(s) get none "
        "of their state (barrages fire nothing, added weapons never fire, added Pelicans and missiles do not move)", why, g_networkTypeCount);
    return 1;
}

/* An entity's map row components {pointer, count}, or 0 when it has no row. */
static int NetComponents(uint8_t* map, uint64_t entity, uint64_t out[2])
{
    uint8_t* row = map != NULL && entity != 0 ? FindRow(map, REG_MAP_SLOTS, 32, entity) : NULL;
    if (row == NULL) return 0;
    memcpy(&out[0], row + 8, 8);
    memcpy(&out[1], row + 16, 8);
    return 1;
}

/* The switch's bound, base, table load and exit as reviewed. */
static int NetSwitchPinned(uintptr_t game)
{
    uint8_t bound[sizeof kNetBoundPin], base[sizeof kNetBasePin], load[sizeof kNetLoadPin], exit[sizeof kNetExitPin];
    return !(SafeRead(game + NET_SWITCH_BOUND, bound, sizeof bound) || memcmp(bound, kNetBoundPin, sizeof bound) != 0 ||
        SafeRead(game + NET_SWITCH_BASE, base, sizeof base) || memcmp(base, kNetBasePin, sizeof base) != 0 ||
        SafeRead(game + NET_SWITCH_LOAD, load, sizeof load) || memcmp(load, kNetLoadPin, sizeof load) != 0 ||
        SafeRead(game + NET_SWITCH_EXIT, exit, sizeof exit) || memcmp(exit, kNetExitPin, sizeof exit) != 0);
}

/* Whether every entry of the vanilla table is one of the switch's cases. */
static int NetVanillaDecoded(const uint32_t* vanilla)
{
    for (uint32_t i = 0; i < NET_VANILLA_TYPES; i++)
        if (vanilla[i] < NET_CASES_FIRST || vanilla[i] > NET_CASES_LAST) return 0;
    return 1;
}

/* Why the switch cannot be extended (NULL: it can), its vanilla table read into `vanilla`. *wait: refused only once the
 * grace for code still being decoded is over. */
static const char* NetSwitchRefused(uintptr_t game, uint32_t* vanilla, int* wait)
{
    *wait = 1;
    if (!NetSwitchPinned(game)) return "its code is not the reviewed one";
    *wait = 0;
    if (g_networkVanillaTypes != NET_VANILLA_TYPES) return "the network configuration does not hold the 1,855 vanilla types";
    AcquireSRWLockShared(&g_sessionLock);
    uint32_t role = g_session.role;
    ReleaseSRWLockShared(&g_sessionLock);
    if (role != HF_SESSION_SOLO) return "a co-op session had already started";

    /* The table is in the code section, ~92 KB past the pinned instructions, and the code is decoded in parts after the
     * game loads: every entry must be one of the switch's cases before it is copied (a still-encoded table copied in
     * would send every received object to garbage). */
    if (SafeRead(game + NET_SWITCH_TABLE, vanilla, NET_VANILLA_TYPES * sizeof vanilla[0])) return "the vanilla table cannot be read";
    *wait = 1;
    if (!NetVanillaDecoded(vanilla)) return "its table is not the reviewed one (or not decoded yet)";
    *wait = 0;
    return NULL;
}

/* Each added type's entry: its base's case when it has its base's component list, else the exit (logged). *highest:
 * the highest type in the table, *mapped how many were mapped; how many keep the exit. */
static uint32_t NetMapTypes(uint32_t* table, uint32_t* highest, uint32_t* mapped)
{
    uint8_t* map = StatePointer(EntityState(), REG_MAP);
    uint32_t kept = 0;
    for (uint32_t k = 0; k < g_networkTypeCount; k++)
    {
        const NetworkType* t = &g_networkTypes[k];
        if (t->index < NET_VANILLA_TYPES || t->index >= NET_TABLE_SLOTS || t->base >= NET_VANILLA_TYPES)
        {
            Note("network: type %u (%s) is outside the table; it keeps the exit", t->index, t->name);
            kept++;
            continue;
        }
        if (t->index > *highest) *highest = t->index;
        uint64_t own[2], copied[2];
        if (!NetComponents(map, t->entity, own) || !NetComponents(map, t->baseEntity, copied) || own[0] != copied[0] || own[1] != copied[1])
        {
            Note("network: type %u (%s) has not its base's component list; it keeps the exit (its copies get no state)", t->index, t->name);
            kept++;
            continue;
        }
        table[t->index] = table[t->base];
        (*mapped)++;
        Note("network: type %u (%s) -> case game.dll+0x%x (base type %u)", t->index, t->name, table[t->index], t->base);
    }
    return kept;
}

/* The table, then the displacement, then the bound: an index above the old bound is never read before its entry is.
 * NULL when written, else why not (the vanilla table back: nothing half done). */
static const char* NetSwitchWrite(uintptr_t game, const uint32_t* table, uint32_t newBound)
{
    int32_t displacement = (int32_t)(int64_t)((uintptr_t)table - game);
    if ((intptr_t)((uintptr_t)table - game) != displacement) return "the table is not within 2 GB of game.dll";
    if (ApiWrite(game + NET_SWITCH_LOAD + 3, &displacement, 4)) return "the table load could not be written";
    if (ApiWrite(game + NET_SWITCH_BOUND + 1, &newBound, 4))
    {
        uint32_t vanilla = NET_SWITCH_TABLE;
        ApiWrite(game + NET_SWITCH_LOAD + 3, &vanilla, 4); /* the vanilla table back: nothing half done */
        return "the bound could not be written";
    }
    return NULL;
}

/* Polled from the watch loop until it returns 1. pinsReady: the grace period for code still being decoded is over. */
static int InstallNetSwitch(uintptr_t game, int pinsReady)
{
    if (g_netSwitchState != 0) return 1;
    if (g_networkTypesAdded != 2) return 0;                         /* RegisterNetworkTypes has not finished */
    if (g_networkTypeCount == 0) { g_netSwitchState = 1; return 1; } /* nothing added */
    static uint32_t vanilla[NET_VANILLA_TYPES];
    int wait = 0;
    const char* why = NetSwitchRefused(game, vanilla, &wait);
    if (why != NULL) return wait && !pinsReady ? 0 : RefuseNetSwitch(why);
    uint32_t* table = (uint32_t*)AllocateNearGame(game, NET_TABLE_SLOTS * 4u);
    if (table == NULL) return RefuseNetSwitch("no memory within 2 GB of game.dll for the table");
    memcpy(table, vanilla, sizeof vanilla);
    for (uint32_t i = NET_VANILLA_TYPES; i < NET_TABLE_SLOTS; i++) table[i] = NET_SWITCH_EXIT;
    uint32_t highest = NET_VANILLA_TYPES - 1, mapped = 0, kept = NetMapTypes(table, &highest, &mapped);
    uint32_t newBound = highest;
    why = NetSwitchWrite(game, table, newBound);
    if (why != NULL) return RefuseNetSwitch(why);
    g_netSwitchTable = table;
    g_netSwitchState = 1;
    Note("network: the apply switch takes %u added type(s) (bound 0x%x -> 0x%x, table at 0x%llx, %s game.dll)%s", mapped,
        NET_VANILLA_TYPES - 1, newBound, (unsigned long long)(uintptr_t)table, (uintptr_t)table > game ? "above" : "below",
        kept ? "; some keep the exit, see above" : "");
    return 1;
}
