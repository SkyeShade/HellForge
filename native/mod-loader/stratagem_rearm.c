/* Uses before cooldown (StratagemUses; docs/modding/stats-mods.md "Uses before cooldown"): how many times a non-Eagle
 * stratagem can be called before its cooldown starts, 1 (the game's own) unless a stats edit raises it. A call runs the
 * game's cooldown start (game.dll 0x135C2C0, from the use at 0xB9AAB0, on every machine through rpc_stratagem_used):
 * it sets the slot's cooldown end (+0x1D8) to its arrival (+0x1E0) plus the cooldown. HellForge's detour counts the calls
 * of each player's slot and, until the last of a stratagem's uses, passes a cooldown of 1 ms, so the slot is ready again
 * once the stratagem has arrived; the last call starts the full cooldown and the count starts over. Eagles (delivery 0)
 * and Eagle Rearm keep their own rearm. The cooldown start is pinned; another build refuses every uses edit. Included by
 * stats.c. */

#define SUSES_MAX 99u
#define SUSES_EAGLE_REARM 0x31u
#define SUSES_COOLDOWN_START 0x135C2C0u
#define SUSES_BURST_COOLDOWN 0.001f /* seconds: ready again once it has arrived */
#define SUSES_COUNTERS 256u

static const CodePin kUsesPins[] = {
    {SUSES_COOLDOWN_START, 24, {0x40, 0x53, 0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xD9, 0x45, 0x8B, 0xE0, 0x33, 0xC9, 0x4C, 0x8B, 0xFA, 0x44, 0x8B, 0x93}},
    {0x135C2D8u, 13, {0x00, 0xD2, 0x02, 0x00, 0x45, 0x85, 0xD2, 0x0F, 0x84, 0xD4, 0x02, 0x00, 0x00}}, /* the players: [+0x2D200], 0x1690 each */
    {0x135C32Eu, 7, {0x8B, 0x94, 0x1F, 0xC0, 0x01, 0x00, 0x00}},                                       /* the slot's type at +0x1C0 */
    {0x135C413u, 16, {0x48, 0x03, 0x84, 0x1F, 0xE0, 0x01, 0x00, 0x00, 0x48, 0x89, 0x84, 0x1F, 0xD8, 0x01, 0x00, 0x00}}, /* end = arrival + cooldown */
    {0xB9AC82u, 10, {0xF3, 0x0F, 0x11, 0x7C, 0x24, 0x20, 0x49, 0x8B, 0xCE, 0xE8}},                       /* a use starts it */
};

typedef void (*UsesCooldownFn)(uint8_t* players, int64_t player, uint32_t slot, char stamp, float cooldown);
static UsesCooldownFn g_usesOriginal;
static uint32_t g_stratagemUses[STR_VANILLA]; /* what the stats layers write (StratagemUses +0): 0 read as 1 */
static int g_usesState;                       /* 0 not yet, 1 hooked, -1 refused */
static ULONGLONG g_usesFirstTry;
static struct { uintptr_t players; int64_t player; uint32_t slot, type, calls; } g_usesCounters[SUSES_COUNTERS];
static uint32_t g_usesNext;

static uintptr_t StatsTableOf(int phase, uintptr_t game); /* stats.c */

/* A vanilla stratagem's row by type, or 0. */
static uintptr_t UsesGameRow(uint32_t type)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL), row = 0;
    uint32_t id = 0;
    if (game == 0 || type == 0 || type >= STR_VANILLA || SafeRead(StatsTableOf(2, game) + type * 8u, &row, 8) || row == 0 ||
        SafeRead(row, &id, 4) || id != type) return 0;
    return row;
}

static uintptr_t (*UsesRow)(uint32_t type) = UsesGameRow; /* the native fixture replaces it */

/* Whether a type keeps the game's one use: an Eagle (delivery 0), Eagle Rearm, or no row. */
static int UsesEagle(uint32_t type)
{
    uintptr_t row = UsesRow(type);
    uint32_t delivery = 0;
    return row == 0 || type == SUSES_EAGLE_REARM || SafeRead(row + 60, &delivery, 4) || delivery == 0;
}

/* The uses a type has before its cooldown (1 unless edited). */
static uint32_t UsesOf(uint32_t type) { return type < STR_VANILLA && g_stratagemUses[type] > 1 ? g_stratagemUses[type] : 1; }

/* A StratagemUses field (record: the stable id; offset 0): its u32, or 0 with *why. Unset fields read 1. */
static uintptr_t UsesField(const StatEdit* e, const char** why)
{
    if (g_usesState < 0) { *why = "uses before cooldown is refused on this build"; return 0; }
    for (uint32_t type = 1; e->offset == 0 && type < STR_VANILLA; type++)
    {
        uintptr_t row = UsesRow(type);
        uint32_t stable = 0;
        if (row == 0 || SafeRead(row + 4, &stable, 4) || stable != (uint64_t)e->record) continue;
        if (UsesEagle(type)) break;
        if (g_stratagemUses[type] == 0) g_stratagemUses[type] = 1;
        return (uintptr_t)&g_stratagemUses[type];
    }
    *why = "no vanilla non-Eagle stratagem with that id";
    return 0;
}

/* fields.c's row of a StratagemUses table (by type, as live requests name stratagems). */
static uintptr_t UsesRowOf(uint64_t type, int* ready)
{
    *ready = g_usesState != 0;
    if (g_usesState != 1 || type == 0 || type >= STR_VANILLA || UsesEagle((uint32_t)type)) return 0;
    if (g_stratagemUses[type] == 0) g_stratagemUses[type] = 1;
    return (uintptr_t)&g_stratagemUses[type];
}

static const char* UsesRefusal(const StatEdit* e)
{
    return !strcmp(e->kind, "StratagemUses") && (e->value < 1 || e->value > SUSES_MAX) ? "uses before cooldown must be 1 to 99" : NULL;
}

/* The calls a player's slot has made towards its cooldown (a slot that changed stratagem starts over). */
static uint32_t* UsesCalls(uintptr_t players, int64_t player, uint32_t slot, uint32_t type)
{
    for (uint32_t i = 0; i < SUSES_COUNTERS; i++)
        if (g_usesCounters[i].players == players && g_usesCounters[i].player == player && g_usesCounters[i].slot == slot)
        {
            if (g_usesCounters[i].type != type) { g_usesCounters[i].type = type; g_usesCounters[i].calls = 0; }
            return &g_usesCounters[i].calls;
        }
    uint32_t i = g_usesNext++ % SUSES_COUNTERS;
    g_usesCounters[i].players = players; g_usesCounters[i].player = player; g_usesCounters[i].slot = slot;
    g_usesCounters[i].type = type; g_usesCounters[i].calls = 0;
    return &g_usesCounters[i].calls;
}

/* The slot's type in the players' records (as the original finds it), or 0. */
static uint32_t UsesSlotType(const uint8_t* players, int64_t player, uint32_t slot)
{
    uint32_t count = 0, type = 0;
    if (SafeRead((uintptr_t)players + 0x2D200u, &count, 4)) return 0;
    for (uint32_t i = 0; i < count && slot < 40; i++)
    {
        int64_t who = 0;
        if (SafeRead((uintptr_t)players + i * 0x1690u, &who, 8)) return 0;
        if (who == player) return SafeRead((uintptr_t)players + i * 0x1690u + slot * 0x30u + 0x1C0u, &type, 4) ? 0 : type;
    }
    return 0;
}

/* The decision for one call: the cooldown to pass on (the game's own, or SUSES_BURST_COOLDOWN while uses remain). */
static float UsesCooldownFor(uintptr_t players, int64_t player, uint32_t slot, uint32_t type, float cooldown)
{
    uint32_t uses = UsesOf(type);
    if (uses <= 1 || UsesEagle(type)) return cooldown;
    uint32_t* calls = UsesCalls(players, player, slot, type);
    if (++*calls < uses) return SUSES_BURST_COOLDOWN;
    *calls = 0;
    return cooldown;
}

static void UsesCooldownDetour(uint8_t* players, int64_t player, uint32_t slot, char stamp, float cooldown)
{
    uint32_t type = UsesSlotType(players, player, slot);
    g_usesOriginal(players, player, slot, stamp, type ? UsesCooldownFor((uintptr_t)players, player, slot, type, cooldown) : cooldown);
}

/* From game load: the cooldown start hooked once its code is decoded (60 s grace), else refused. */
static void UsesInstall(void)
{
    if (g_usesState != 0) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    ULONGLONG now = GetTickCount64();
    if (!g_usesFirstTry) g_usesFirstTry = now;
    if (strcmp(g_build, "25480438") || game == 0) { if (game != 0) g_usesState = -1; return; }
    for (size_t i = 0; i < sizeof kUsesPins / sizeof kUsesPins[0]; i++)
        if (!PinMatches(game + kUsesPins[i].rva, kUsesPins[i].bytes, kUsesPins[i].length))
        {
            if (now - g_usesFirstTry < 60000) return;
            g_usesState = -1;
            Note("stratagem uses: game.dll+%X is not the reviewed cooldown start; uses before cooldown is refused", kUsesPins[i].rva);
            return;
        }
    g_usesState = LoaderHook("stratagem uses", game + SUSES_COOLDOWN_START, (void*)UsesCooldownDetour, (void**)&g_usesOriginal) ? -1 : 1;
}

/* A field of HellForge's own stratagem tables (StratagemOwnKind): a code's or a uses count's, or 0 with *why. */
static uintptr_t StratagemOwnField(const StatEdit* e, const char** why)
{
    return !strcmp(e->kind, "StratagemCode") ? CodesField(e, why) : UsesField(e, why);
}
