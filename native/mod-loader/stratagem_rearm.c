/* Rearms after uses (StratagemRearm; docs/modding/stats-mods.md "Rearms after uses"): a non-Eagle stratagem whose uses
 * per mission come back. Ticked, its uses go with no cooldown between them (the game still holds it while a strike
 * runs), the cooldown starts once they are spent, and when it ends they refill. Every call runs the game's use (game.dll
 * 0xB9AAB0: the slot's uses left, +0x1C4, counted down unless unlimited) and its cooldown start (0x135C2C0), on every
 * machine through rpc_stratagem_used. The detour on the cooldown start passes 1 ms while uses are left; spent, it passes
 * the game's cooldown and notes the slot, which the game-thread update refills after the cooldown with the game's own
 * count (0x879550, as at mission start). The HUD reads the uses left, and every machine refills every player's slot
 * alike. Eagles, Eagle Rearm and team cooldowns keep the game's ways. All of it is pinned. Included by stats.c. */

#define SREARM_EAGLE_REARM 0x31u
#define SREARM_COOLDOWN_START 0x135C2C0u
#define SREARM_USES_FOR 0x879550u     /* uses(customization, player, type, 0): a slot's count at mission start */
#define SREARM_CUSTOMIZATION 0x33264F8u
#define SREARM_CLOCK 0x3326348u        /* +0x18: now, microseconds */
#define SREARM_BURST_COOLDOWN 0.001f   /* seconds: ready again once it has arrived */
#define SREARM_PENDING 128u

static const CodePin kRearmPins[] = {
    {SREARM_COOLDOWN_START, 24, {0x40, 0x53, 0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xD9, 0x45, 0x8B, 0xE0, 0x33, 0xC9, 0x4C, 0x8B, 0xFA, 0x44, 0x8B, 0x93}},
    {0x135C2D8u, 13, {0x00, 0xD2, 0x02, 0x00, 0x45, 0x85, 0xD2, 0x0F, 0x84, 0xD4, 0x02, 0x00, 0x00}}, /* the players: [+0x2D200], 0x1690 each */
    {0x135C32Eu, 7, {0x8B, 0x94, 0x1F, 0xC0, 0x01, 0x00, 0x00}},                                       /* the slot's type at +0x1C0 */
    {0x135C351u, 11, {0x48, 0x8B, 0x05, 0xF0, 0x9F, 0xFC, 0x01, 0x48, 0x8B, 0x48, 0x18}},              /* the clock, now at +0x18 */
    {0x135C413u, 16, {0x48, 0x03, 0x84, 0x1F, 0xE0, 0x01, 0x00, 0x00, 0x48, 0x89, 0x84, 0x1F, 0xD8, 0x01, 0x00, 0x00}}, /* end = arrival + cooldown */
    {0xB9AC82u, 10, {0xF3, 0x0F, 0x11, 0x7C, 0x24, 0x20, 0x49, 0x8B, 0xCE, 0xE8}},                       /* a use starts it */
    {SREARM_USES_FOR, 14, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x40, 0x0A, 0x00, 0x00}},
    {0x66F102u, 7, {0x48, 0x8B, 0x0D, 0xEF, 0x73, 0xCB, 0x02}},                                       /* the loadout's call of it: the customization */
};

typedef void (*RearmCooldownFn)(uint8_t* players, int64_t player, uint32_t slot, char stamp, float cooldown);
typedef uint32_t (*RearmUsesFn)(uintptr_t customization, int64_t player, uint32_t type, char flag);
static RearmCooldownFn g_rearmOriginal;
static RearmUsesFn g_rearmUsesFor;
static uint32_t g_stratagemRearm[STR_VANILLA]; /* what the stats layers write (StratagemRearm +0): 1 ticked */
static int g_rearmState;                       /* 0 not yet, 1 hooked, -1 refused */
static ULONGLONG g_rearmFirstTry;
static struct { uintptr_t players; int64_t player; uint32_t slot, type; } g_rearmPending[SREARM_PENDING];

static uintptr_t StatsTableOf(int phase, uintptr_t game); /* stats.c */

/* A vanilla stratagem's row by type, or 0. */
static uintptr_t RearmGameRow(uint32_t type)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL), row = 0;
    uint32_t id = 0;
    if (game == 0 || type == 0 || type >= STR_VANILLA || SafeRead(StatsTableOf(2, game) + type * 8u, &row, 8) || row == 0 ||
        SafeRead(row, &id, 4) || id != type) return 0;
    return row;
}
static uintptr_t (*RearmRow)(uint32_t type) = RearmGameRow; /* the native fixture replaces it */

/* Why a type keeps the game's own ways, or NULL: an Eagle (delivery 0), Eagle Rearm, a team cooldown (+0x94), no row. */
static const char* RearmExcluded(uint32_t type)
{
    uintptr_t row = RearmRow(type);
    uint32_t delivery = 0, cooldownType = 0;
    if (row == 0 || SafeRead(row + 60, &delivery, 4) || SafeRead(row + 148, &cooldownType, 4)) return "no vanilla stratagem row";
    if (delivery == 0 || type == SREARM_EAGLE_REARM) return "Eagles keep their own rearm";
    return cooldownType != 0 ? "a team cooldown's uses are the squad's" : NULL;
}

static int RearmOn(uint32_t type) { return type < STR_VANILLA && g_stratagemRearm[type] == 1 && RearmExcluded(type) == NULL; }

/* A StratagemRearm field (record: the stable id; offset 0): its u32, or 0 with *why. */
static uintptr_t RearmField(const StatEdit* e, const char** why)
{
    if (g_rearmState < 0) { *why = "rearms after uses is refused on this build"; return 0; }
    *why = "no vanilla stratagem with that id";
    for (uint32_t type = 1; e->offset == 0 && type < STR_VANILLA; type++)
    {
        uintptr_t row = RearmRow(type);
        uint32_t stable = 0;
        if (row == 0 || SafeRead(row + 4, &stable, 4) || stable != (uint64_t)e->record) continue;
        const char* excluded = RearmExcluded(type);
        if (excluded != NULL) { *why = excluded; return 0; }
        return (uintptr_t)&g_stratagemRearm[type];
    }
    return 0;
}

/* fields.c's row of a StratagemRearm table (by type, as live requests name stratagems). */
static uintptr_t RearmRowOf(uint64_t type, int* ready)
{
    *ready = g_rearmState != 0;
    return g_rearmState == 1 && type >= 1 && type < STR_VANILLA && RearmExcluded((uint32_t)type) == NULL ? (uintptr_t)&g_stratagemRearm[type] : 0;
}

static const char* RearmRefusal(const StatEdit* e)
{
    return !strcmp(e->kind, "StratagemRearm") && e->value > 1 ? "rearms after uses is 0 (no) or 1 (yes)" : NULL;
}

/* A player's slot entry in the players' records (+0 type, +4 uses left, +0x18 cooldown end), or 0. */
static uintptr_t RearmSlot(uintptr_t players, int64_t player, uint32_t slot)
{
    uint32_t count = 0;
    if (slot >= 32 || SafeRead(players + 0x2D200u, &count, 4)) return 0;
    for (uint32_t i = 0; i < count && i < 64; i++)
    {
        int64_t who = 0;
        if (SafeRead(players + i * 0x1690u, &who, 8)) return 0;
        if (who == player) return players + i * 0x1690u + slot * 0x30u + 0x1C0u;
    }
    return 0;
}

static void RearmNote(uintptr_t players, int64_t player, uint32_t slot, uint32_t type)
{
    uint32_t spare = SREARM_PENDING;
    for (uint32_t i = 0; i < SREARM_PENDING; i++)
    {
        if (g_rearmPending[i].players == players && g_rearmPending[i].player == player && g_rearmPending[i].slot == slot) { g_rearmPending[i].type = type; return; }
        if (g_rearmPending[i].players == 0 && spare == SREARM_PENDING) spare = i;
    }
    if (spare == SREARM_PENDING) { Note("stratagem rearm: more than %u slots rearming at once; type %u will not refill", SREARM_PENDING, type); return; }
    g_rearmPending[spare].players = players; g_rearmPending[spare].player = player; g_rearmPending[spare].slot = slot; g_rearmPending[spare].type = type;
}

/* The cooldown one call passes on: the game's, or SREARM_BURST_COOLDOWN while a rearming stratagem has uses left (or
 * unlimited ones, 0xFFFFFFFF). Spent, the slot is noted for its refill. */
static float RearmCooldownFor(uintptr_t entry, uintptr_t players, int64_t player, uint32_t slot, float cooldown)
{
    uint32_t type = 0, left = 0;
    if (entry == 0 || SafeRead(entry, &type, 4) || SafeRead(entry + 4, &left, 4) || !RearmOn(type)) return cooldown;
    if (left != 0) return SREARM_BURST_COOLDOWN;
    RearmNote(players, player, slot, type);
    return cooldown;
}

static void RearmCooldownDetour(uint8_t* players, int64_t player, uint32_t slot, char stamp, float cooldown)
{
    uintptr_t entry = RearmSlot((uintptr_t)players, player, slot);
    g_rearmOriginal(players, player, slot, stamp, RearmCooldownFor(entry, (uintptr_t)players, player, slot, cooldown));
}

/* One noted slot at `now`: 1 once done with (refilled, or no longer that stratagem's), 0 while its cooldown runs. */
static int RearmRefill(uintptr_t customization, uint32_t i, uint64_t now)
{
    uintptr_t entry = RearmSlot(g_rearmPending[i].players, g_rearmPending[i].player, g_rearmPending[i].slot);
    uint32_t type = 0, left = 0;
    uint64_t end = 0;
    if (entry == 0 || SafeRead(entry, &type, 4) || type != g_rearmPending[i].type || SafeRead(entry + 4, &left, 4) || left != 0) return 1;
    if (SafeRead(entry + 0x18, &end, 8) || now < end) return 0;
    uint32_t uses = g_rearmUsesFor(customization, g_rearmPending[i].player, type, 0);
    if (uses == 0 || ApiWrite(entry + 4, &uses, 4)) return 1;
    Note("stratagem rearm: type %u slot %u of player %llx rearmed: %u uses", type, g_rearmPending[i].slot, (unsigned long long)g_rearmPending[i].player, uses);
    return 1;
}

/* The game-thread update: every noted slot whose cooldown has ended gets its uses back. */
static void RearmTick(void)
{
    uintptr_t game = g_rearmState == 1 ? ApiModuleBase("game.dll", NULL) : 0, clock = 0, customization = 0;
    uint64_t now = 0;
    if (game == 0 || SafeRead(game + SREARM_CLOCK, &clock, 8) || clock == 0 || SafeRead(clock + 0x18, &now, 8) ||
        SafeRead(game + SREARM_CUSTOMIZATION, &customization, 8) || customization == 0) return;
    for (uint32_t i = 0; i < SREARM_PENDING; i++)
        if (g_rearmPending[i].players != 0 && RearmRefill(customization, i, now)) memset(&g_rearmPending[i], 0, sizeof g_rearmPending[i]);
}

/* From game load: the cooldown start hooked once its code is decoded (60 s grace), else refused. */
static void RearmInstall(void)
{
    if (g_rearmState != 0) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    ULONGLONG now = GetTickCount64();
    if (!g_rearmFirstTry) g_rearmFirstTry = now;
    if (game == 0) return;
    if (strcmp(g_build, "25480438")) { g_rearmState = -1; return; }
    for (size_t i = 0; i < sizeof kRearmPins / sizeof kRearmPins[0]; i++)
        if (!PinMatches(game + kRearmPins[i].rva, kRearmPins[i].bytes, kRearmPins[i].length))
        {
            if (now - g_rearmFirstTry < 60000) return;
            g_rearmState = -1;
            Note("stratagem rearm: game.dll+%X is not the reviewed code; rearms after uses is refused", kRearmPins[i].rva);
            return;
        }
    g_rearmUsesFor = (RearmUsesFn)(game + SREARM_USES_FOR);
    g_rearmState = LoaderHook("stratagem rearm", game + SREARM_COOLDOWN_START, (void*)RearmCooldownDetour, (void**)&g_rearmOriginal) ? -1 : 1;
}

/* A field of HellForge's own stratagem tables (StratagemOwnKind): a code's or a rearm flag's, or 0 with *why. */
static uintptr_t StratagemOwnField(const StatEdit* e, const char** why)
{
    return !strcmp(e->kind, "StratagemCode") ? CodesField(e, why) : RearmField(e, why);
}
