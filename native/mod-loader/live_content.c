/* Numbered content in a live change (live_mods.c, SDK 16: docs/modding/live.md "Content"): what a mod set up while the
 * game runs registers (stratagems and their selection categories, projectiles, damage, behaviours, abilities; weapons and
 * entities: live_registry.c) is numbered as a boot of the change's mods would number it, installed once the change is
 * checked, and made owned. What a mod taken out registered stays where it is, out of the catalogue, content() and the
 * player's items (a tombstone: rows the game may still hold stay valid). Included by live_mods.c.
 *
 * Numbering. A boot sets the code mods up in the order of their ids and numbers what each registers in its call order
 * (SetupOrder), so every machine with the same mods numbers alike. A live change sets its mods up in that order too, and
 * each registration takes the number a boot of the change's mods would give it (LiveEntry): after everything the
 * change's mods with lower ids registered. That number must be the next one, or one that a mod the change takes out (or
 * one taken out earlier this session: a tombstone) holds. The registration stands in for that entry until the change is
 * made: the entry itself again (a new version's, or its mod back), or another mod's, which takes the number over (KB
 * hellforge-live-mod-sync-tombstone-reuse; LiveTakeoversCheck says when the game lets it). A mod back this session comes
 * back at its old number. Any other number refuses the change: only a restart numbers the content as the host does.
 * After the set-ups, every mod that stays is checked the same way (LiveNumbersCheck): a mod whose numbers a boot would
 * move refuses the change.
 *
 * Install, once the whole change is checked (LiveContentMake):
 *   1. on the change's thread: each list's pins checked, rows built from their bases, blocks allocated, and every write
 *      to game memory queued (LivePut): a list's first move (its instructions' displacements: 339 + 3 stratagem sites,
 *      66 projectile, 489 damage), the new rows' list entries, the counts and bounds that let code reach them;
 *   2. one freeze: every other thread suspended, the stand-ins copied in, the queued writes made (each code page made
 *      writable once), the threads resumed. No allocation, lock or log meanwhile: a suspended thread may hold one;
 *   3. behaviours' and abilities' first stubs (their branches are atomic stores), then one step on the game thread:
 *      selection categories, weapons and entities (live_registry.c), what a removed mod's content leaves in the
 *      loadout (loadout_scrub.c), then the items: the catalogue copies of each new one and each owned, a removed mod's
 *      given back (ownership.c).
 *
 * Cost: nothing on any frame but the change's own (no per-frame code is added). During one: the freeze (about 1,000
 * writes for a first move of all three lists, else a few per entry), one game-thread step with the catalogue's sort;
 * unmeasured in game. Afterwards the lists are read as before; a stub's id check compares with a higher count. */

#include "ownership.c" /* items made owned, and given back */
#include "loadout_scrub.c" /* the loadout without what a removed mod added */

#define LC_MAX 1024 /* entries of any numbered kind HellForge has room for (weapons and entities grow: their tombs are Weapon.tomb) */
/* table: the settings table edits name its rows in by their number (fields.c), or NULL */
typedef struct LiveKind { const char* name; void* entries; size_t stride, nameAt; uint32_t* count; uint32_t max, first; const char* table; } LiveKind;
_Static_assert(offsetof(Stratagem, mod) == 0 && offsetof(Projectile, mod) == 0 && offsetof(Damage, mod) == 0 && offsetof(Behaviour, mod) == 0 &&
    offsetof(Ability, mod) == 0, "every content entry begins with its mod");
_Static_assert(STR_MAX_NEW <= LC_MAX && PRJ_MAX_NEW <= LC_MAX && DMG_MAX_NEW <= LC_MAX && BHV_MAX <= LC_MAX && ABL_MAX <= LC_MAX,
    "a kind's flags fit");
static const LiveKind kLiveKinds[LC_KINDS] = {
    {"stratagem", g_stratagems, sizeof(Stratagem), offsetof(Stratagem, name), &g_stratagemCount, STR_MAX_NEW, STR_VANILLA, "StratagemDefinition"},
    {"projectile", g_projectiles, sizeof(Projectile), offsetof(Projectile, name), &g_projectileCount, PRJ_MAX_NEW, PRJ_VANILLA, "ProjectileSettings"},
    {"damage", g_damages, sizeof(Damage), offsetof(Damage, name), &g_damageCount, DMG_MAX_NEW, DMG_VANILLA, "DamageInfo"},
    {"behaviour", g_behaviours, sizeof(Behaviour), 0, &g_behaviourCount, BHV_MAX, BHV_FIRST, NULL}, /* unnamed: its mod's, by place */
    {"ability", g_abilities, sizeof(Ability), 0, &g_abilityCount, ABL_MAX, ABL_FIRST, NULL},
};

static uint8_t g_lcTomb[LC_KINDS][LC_MAX];      /* entries a mod taken out left (LC_WEAPON's: Weapon.tomb) */
static uint32_t g_lcBefore[LC_KINDS];          /* the entries before the change's set-ups; after them, the set-ups' */
static uint8_t* g_lcShadow[LC_KINDS];          /* stand-ins, by index, for entries of mods taken out */
/* How each stand-in stands in (0: none): the entry's new version, the entry back at its tombstone, or another mod's
 * entry taking over the number (the entry it replaces goes as a mod taken out does). */
enum { LC_STAND_SAME = 1, LC_STAND_BACK, LC_STAND_OVER };
static uint8_t g_lcShadowed[LC_KINDS][LC_MAX];
static int g_lcOpen;                           /* a change's set-ups are registering */
static char g_lcRefused[320];                  /* the first registration the numbering refused: "<reason>\t<why>" */
static int g_lcTextsBefore, g_lcWeaponsBefore;
static uint32_t g_lcCategoriesBefore;
static uintptr_t g_lcFixtureGame; /* the native fixture's synthetic game.dll image, and its size (else the loaded one) */
static size_t g_lcFixtureGameSize;

/* ---- numbering ----------------------------------------------------------------------------------------------------- */

static int LiveLeaves(const HfMod* mod)
{
    for (int i = 0; i < g_txn.modCount; i++) if (g_txn.mods[i].action != TXN_ADD && g_txn.mods[i].old == mod) return 1;
    return 0;
}

static uint8_t* LiveAt(int kind, uint32_t index) { return (uint8_t*)kLiveKinds[kind].entries + index * kLiveKinds[kind].stride; }
static HfMod* LiveModAt(int kind, uint32_t index) { return kind == LC_WEAPON ? g_weapons[index].mod : *(HfMod**)LiveAt(kind, index); }

static int LiveTomb(int kind, uint32_t index)
{
    if (kind == LC_WEAPON) return index < (uint32_t)g_weaponCount && g_weapons[index].tomb;
    return kind >= 0 && kind < LC_KINDS && index < LC_MAX && g_lcTomb[kind][index];
}
static int LiveGone(int kind, uint32_t index) { return LiveTomb(kind, index) || (g_lcOpen && LiveLeaves(LiveModAt(kind, index))); }

/* The mod whose entry this is after the change: a stand-in's, else the entry's when its mod stays; NULL when it leaves. */
static HfMod* LiveAfter(int kind, uint32_t index)
{
    if (g_lcShadowed[kind][index]) return *(HfMod**)(g_lcShadow[kind] + index * kLiveKinds[kind].stride);
    HfMod* mod = LiveModAt(kind, index);
    return LiveTomb(kind, index) || mod == NULL || mod->removed || LiveLeaves(mod) ? NULL : mod;
}

/* What a boot of the change's mods registers of this kind before this mod's next registration: everything of the mods
 * with lower ids, then its own so far. */
static uint32_t LiveNumbered(int kind, const HfMod* mod)
{
    uint32_t numbered = 0, count = *kLiveKinds[kind].count;
    for (uint32_t i = 0; i < count; i++)
    {
        const HfMod* after = LiveAfter(kind, i);
        numbered += after != NULL && (after == mod || strcmp(after->id, mod->id) < 0);
    }
    return numbered;
}

/* The tombstone a named registration had before its mod was taken out this session (its number), or -1. One another
 * registration of the change takes over no longer names it. */
static int LiveTombNamed(int kind, const char* name)
{
    const LiveKind* k = &kLiveKinds[kind];
    for (uint32_t i = 0; i < g_lcBefore[kind] && k->nameAt != 0 && name != NULL; i++)
        if (LiveTomb(kind, i) && !g_lcShadowed[kind][i] && !strcmp((const char*)LiveAt(kind, i) + k->nameAt, name)) return (int)i;
    return -1;
}

/* How a registration may stand in for the entry at index (LC_STAND_..., 0: it may not). The entry must be one a mod taken
 * out holds: a tombstone, or one the change takes out. Under the same mod id and, for a named kind, the same name, it is
 * that entry again; else the registration takes the number over, unless it is itself back from a tombstone elsewhere
 * (a stratagem's stable id must name one type). */
static int LiveStandsIn(int kind, uint32_t index, const HfMod* mod, const char* name)
{
    const LiveKind* k = &kLiveKinds[kind];
    const uint8_t* entry = LiveAt(kind, index);
    const HfMod* old = *(HfMod* const*)entry;
    int tomb = LiveTomb(kind, index);
    if (g_lcShadowed[kind][index] || old == NULL || (!tomb && !LiveLeaves(old))) return 0;
    if (!strcmp(old->id, mod->id) && (k->nameAt == 0 || (name != NULL && !strcmp((const char*)entry + k->nameAt, name)))) return tomb ? LC_STAND_BACK : LC_STAND_SAME;
    return LiveTombNamed(kind, name) >= 0 ? 0 : LC_STAND_OVER;
}

static void LiveRefuse(HfMod* mod, const char* kind, const char* name, const char* why)
{
    ApiLog(mod, "%s %s not added: %s", kind, name != NULL ? name : "", why);
    if (!g_lcRefused[0]) snprintf(g_lcRefused, sizeof g_lcRefused, "capability\t%s's %s %s: %s; a restart numbers it as the host does", mod->id, kind,
        name != NULL ? name : "", why);
}

/* A registration refused for want of room, not for its number (registry.c WeaponsFull): logged, and the change refused
 * with it, so it is never made with part of a mod's content. */
static void LiveRefuseRoom(HfMod* mod, const char* what, const char* name, const char* why)
{
    ApiLog(mod, "%s(%s): %s", what, name != NULL ? name : "", why);
    if (!g_lcRefused[0]) snprintf(g_lcRefused, sizeof g_lcRefused, "capacity\t%s's %s: %s", mod->id, name != NULL ? name : "", why);
}

/* Why a registration cannot take number index (what holds it), in why. */
static void LiveHeldWhy(int kind, uint32_t index, const char* name, char* why, size_t size)
{
    const LiveKind* k = &kLiveKinds[kind];
    int back = LiveTombNamed(kind, name);
    if (back >= 0 && (uint32_t)back != index)
        snprintf(why, size, "a boot of these mods numbers it %u, but it was %u before its mod was taken out this session", k->first + index, k->first + (uint32_t)back);
    else
        snprintf(why, size, "a boot of these mods numbers it %u, which %s holds", k->first + index, LiveModAt(kind, index) ? LiveModAt(kind, index)->id : "another mod");
}

static void* LiveEntry(int kind, HfMod* mod, const char* name, uint32_t* index)
{
    const LiveKind* k = &kLiveKinds[kind];
    uint32_t at = g_lcOpen ? LiveNumbered(kind, mod) : 0, count = *k->count;
    int stand = 0;
    char why[160];
    why[0] = 0;
    if (!g_lcOpen) snprintf(why, sizeof why, "no live change is setting mods up");
    else if (at >= k->max) snprintf(why, sizeof why, "HellForge has room for %u", k->max);
    else if (at < g_lcBefore[kind] && (stand = LiveStandsIn(kind, at, mod, name)) == 0) LiveHeldWhy(kind, at, name, why, sizeof why);
    else if (at >= g_lcBefore[kind] && at != count) snprintf(why, sizeof why, "a boot of these mods numbers it %u, not the next (%u)", k->first + at, k->first + count);
    else if (at >= g_lcBefore[kind] && LiveTombNamed(kind, name) >= 0)
        snprintf(why, sizeof why, "a boot of these mods numbers it %u, but it was %u before its mod was taken out this session", k->first + at,
            k->first + (uint32_t)LiveTombNamed(kind, name));
    if (why[0]) { LiveRefuse(mod, k->name, name, why); return NULL; }
    *index = at;
    if (at >= g_lcBefore[kind]) return LiveAt(kind, at);
    g_lcShadowed[kind][at] = (uint8_t)stand;
    return g_lcShadow[kind] + at * k->stride;
}

/* After the set-ups: every entry that stays, in the order a boot would number it (by mod id, then call order), must be
 * where it is. NULL, or "<reason>\t<why>". */
static const char* LiveKindCheck(int kind, char* why, size_t size)
{
    const LiveKind* k = &kLiveKinds[kind];
    uint32_t order[LC_MAX], n = 0, count = *k->count;
    for (uint32_t i = 0; i < count && n < LC_MAX; i++) if (LiveAfter(kind, i) != NULL) order[n++] = i;
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j > 0 && strcmp(LiveAfter(kind, order[j - 1])->id, LiveAfter(kind, order[j])->id) > 0; j--)
        {
            uint32_t t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (uint32_t r = 0; r < n; r++)
        if (order[r] != r)
        {
            snprintf(why, size, "capability\t%s's %s %u would be %u at a boot of these mods; a restart numbers it as the host does",
                LiveAfter(kind, order[r])->id, k->name, k->first + order[r], k->first + r);
            return why;
        }
    return NULL;
}

/* Whether a mod is one the change sets up (added, or a new version). */
static int LiveIncoming(const HfMod* mod)
{
    for (int i = 0; i < g_txn.modCount; i++) if (g_txn.mods[i].action != TXN_REMOVE && g_txn.mods[i].mod == mod) return 1;
    return 0;
}

/* Whether an edit names a settings row of the kind by its number (fields.c). */
static int LiveRowEdit(const FieldEdit* e, int kind, uint32_t number)
{
    return kLiveKinds[kind].table != NULL && e->table && e->entity == number && !strcmp(e->component->component, kLiveKinds[kind].table);
}

/* An edit of a row that would stay with the row a takeover replaces, where a boot of these mods applies it to the new
 * one: a deferred edit of a mod that stays, or a live edit of a mod not taken out. A mod taken out takes its edits with
 * it, and the set-ups' deferred edits apply again to the new row (LiveEditsFollow). The edit's mod, or NULL.
 * g_fieldsLock held. */
static const HfMod* LiveRowEdited(int kind, uint32_t number)
{
    for (int i = 0; i < g_editCount; i++)
    {
        const FieldEdit* e = &g_edits[i];
        if (!LiveRowEdit(e, kind, number) || e->failed || LiveLeaves(e->mod)) continue;
        if (e->live || !LiveIncoming(e->mod)) return e->mod;
    }
    return NULL;
}

/* A takeover of entry index, right before the change is made: no edit of its row stays behind, and for a stratagem the
 * game holds none of the old one's packages (loadout_scrub.c ScrubTakeover). NULL, or "<reason>\t<why>". */
static const char* LiveTakeoverRefused(int kind, uint32_t index, char* why, size_t size)
{
    const LiveKind* k = &kLiveKinds[kind];
    const HfMod* editor = LiveRowEdited(kind, k->first + index);
    if (editor != NULL)
    {
        snprintf(why, size, "capability\t%s %u passes to %s, but %s's edit of it would stay with the old one (a boot of these mods gives it the new "
            "one); a restart numbers it as the host does", k->name, k->first + index, (*(HfMod**)(g_lcShadow[kind] + index * k->stride))->id, editor->id);
        return why;
    }
    if (kind != LC_STRATAGEM || g_stratagemState != 1) return NULL; /* not in the game: numbers only */
    uintptr_t game = g_lcFixtureGame ? g_lcFixtureGame : ApiModuleBase("game.dll", NULL);
    return game != 0 ? ScrubTakeover(game, k->first + index, why, size) : NULL;
}

/* Before the change is checked, off every lock: a change giving a stratagem's type to another mod waits up to 20 s (the
 * loader's grace for later pins; the backend waits 30 s for a change) for the code its check reads to be decoded
 * (game.dll decodes in parts after it loads; a mod sync on the title screen comes seconds after the boot). The check
 * then refuses only a pin that still differs. */
static void LiveTakeoversWait(void)
{
    int stratagem = 0;
    for (uint32_t i = 0; i < g_lcBefore[LC_STRATAGEM] && !stratagem; i++) stratagem = g_lcShadowed[LC_STRATAGEM][i] == LC_STAND_OVER;
    uintptr_t game = stratagem && g_stratagemState == 1 ? (g_lcFixtureGame ? g_lcFixtureGame : ApiModuleBase("game.dll", NULL)) : 0;
    uint32_t pin = game != 0 ? ScrubTakeoverPin(game) : 0;
    for (int waited = 0; pin != 0 && waited < 20000; waited += 500, pin = ScrubTakeoverPin(game)) Sleep(500);
}

/* Every takeover of the change (LiveTakeoverRefused). g_fieldsLock held. */
static const char* LiveTakeoversCheck(char* why, size_t size)
{
    for (int kind = 0; kind < LC_KINDS; kind++)
        for (uint32_t i = 0; i < g_lcBefore[kind]; i++)
            if (g_lcShadowed[kind][i] == LC_STAND_OVER && LiveTakeoverRefused(kind, i, why, size) != NULL) return why;
    return NULL;
}

static const char* LiveWeaponsCheck(char* why, size_t size); /* live_registry.c */

static const char* LiveNumbersCheck(char* why, size_t size)
{
    if (g_lcRefused[0]) { snprintf(why, size, "%s", g_lcRefused); return why; }
    for (int kind = 0; kind < LC_KINDS; kind++)
    {
        const char* refused = LiveKindCheck(kind, why, size);
        if (refused != NULL) return refused;
    }
    const char* refused = LiveWeaponsCheck(why, size);
    return refused != NULL ? refused : LiveTakeoversCheck(why, size);
}

/* Before the change's set-ups. */
static const char* LiveContentBegin(char* why, size_t size)
{
    for (int kind = 0; kind < LC_KINDS; kind++)
    {
        g_lcBefore[kind] = *kLiveKinds[kind].count;
        memset(g_lcShadowed[kind], 0, sizeof g_lcShadowed[kind]);
        if (g_lcShadow[kind] == NULL) g_lcShadow[kind] = (uint8_t*)calloc(kLiveKinds[kind].max, kLiveKinds[kind].stride);
        if (g_lcShadow[kind] == NULL) { snprintf(why, size, "capacity\tno memory for the change's content"); return why; }
    }
    g_lcTextsBefore = g_textCount;
    g_lcWeaponsBefore = g_weaponCount;
    g_lcCategoriesBefore = g_stratagemCategoryCount;
    g_lcRefused[0] = 0;
    g_lcOpen = 1;
    g_textsHeld = 1; /* the set-ups' texts wait for the change to be made (registry.c TextDetour) */
    return NULL;
}

/* A refused change: what its set-ups registered goes (their texts too), the stand-ins with it. */
static void LiveContentUnstage(void)
{
    if (!g_lcOpen) return;
    for (int kind = 0; kind < LC_KINDS; kind++)
    {
        *kLiveKinds[kind].count = g_lcBefore[kind];
        memset(g_lcShadowed[kind], 0, sizeof g_lcShadowed[kind]);
    }
    g_weaponCount = g_lcWeaponsBefore;
    g_stratagemCategoryCount = g_lcCategoriesBefore;
    if (g_difficultyLive) { g_difficultyLive = g_difficultyAdded = 0; memset(&g_difficulty, 0, sizeof g_difficulty); }
    /* the set-ups' texts go unserved; never freed (ApiContent may have handed one out), their slots taken again */
    g_textCount = g_lcTextsBefore;
    g_textsHeld = 0;
    g_lcOpen = 0;
}

/* ---- the queued writes and the freeze ------------------------------------------------------------------------------ */

typedef struct ContentWrite { uintptr_t at; uint32_t size, bytes; int code; } ContentWrite; /* bytes: where in the pool */
static ContentWrite* g_lcWrites;
static int g_lcWriteCount, g_lcWriteCapacity, g_lcWriteFailed;
static uint8_t* g_lcPool;
static size_t g_lcPoolUsed, g_lcPoolCapacity;
static uintptr_t g_lcImageFrom, g_lcImageTo; /* game.dll's image: a write there makes its page writable first */
#define LC_PAGES 4096
static uintptr_t g_lcPages[LC_PAGES];
static DWORD g_lcPageProtect[LC_PAGES];
static int g_lcPageCount;

static int LiveWriteQueue(uintptr_t at, const void* bytes, size_t size, int code)
{
    if (g_lcPoolUsed + size > g_lcPoolCapacity)
    {
        size_t capacity = g_lcPoolCapacity ? g_lcPoolCapacity * 2 : 65536;
        while (capacity < g_lcPoolUsed + size) capacity *= 2;
        uint8_t* grown = (uint8_t*)realloc(g_lcPool, capacity);
        if (grown == NULL) { g_lcWriteFailed = 1; return 1; }
        g_lcPool = grown;
        g_lcPoolCapacity = capacity;
    }
    if (g_lcWriteCount == g_lcWriteCapacity)
    {
        int capacity = g_lcWriteCapacity ? g_lcWriteCapacity * 2 : 2048;
        ContentWrite* grown = (ContentWrite*)realloc(g_lcWrites, (size_t)capacity * sizeof *grown);
        if (grown == NULL) { g_lcWriteFailed = 1; return 1; }
        g_lcWrites = grown;
        g_lcWriteCapacity = capacity;
    }
    ContentWrite* w = &g_lcWrites[g_lcWriteCount++];
    *w = (ContentWrite){at, (uint32_t)size, (uint32_t)g_lcPoolUsed, code || (at >= g_lcImageFrom && at < g_lcImageTo)};
    memcpy(g_lcPool + g_lcPoolUsed, bytes, size);
    g_lcPoolUsed += size;
    return 0;
}
/* PutFn for the installs: game memory (its image's pages made writable), or HellForge's own. */
static int LivePut(uintptr_t at, const void* bytes, size_t size) { return LiveWriteQueue(at, bytes, size, 0); }
/* Into HellForge's executable stubs (read-only pages). */
static int LivePutCode(uintptr_t at, const void* bytes, size_t size) { return LiveWriteQueue(at, bytes, size, 1); }

/* The pages the code writes touch, each once (before the freeze: nothing is allocated under it). 0 when too many. */
static int LivePagesPlan(void)
{
    g_lcPageCount = 0;
    for (int i = 0; i < g_lcWriteCount; i++)
    {
        const ContentWrite* w = &g_lcWrites[i];
        for (uintptr_t page = w->at & ~(uintptr_t)0xFFF; w->code && page <= ((w->at + w->size - 1) & ~(uintptr_t)0xFFF); page += 0x1000)
        {
            int known = 0;
            for (int k = g_lcPageCount - 1; k >= 0 && !known; k--) known = g_lcPages[k] == page;
            if (known) continue;
            if (g_lcPageCount == LC_PAGES) return 0;
            g_lcPages[g_lcPageCount++] = page;
        }
    }
    return 1;
}

/* Under the freeze: the code pages writable, every write in its order, the pages as they were. */
static void LiveWritesMake(void)
{
    for (int k = 0; k < g_lcPageCount; k++)
        if (!VirtualProtect((void*)g_lcPages[k], 0x1000, PAGE_EXECUTE_READWRITE, &g_lcPageProtect[k])) g_lcPageProtect[k] = 0;
    for (int i = 0; i < g_lcWriteCount; i++) memcpy((void*)g_lcWrites[i].at, g_lcPool + g_lcWrites[i].bytes, g_lcWrites[i].size);
    for (int k = 0; k < g_lcPageCount; k++)
    {
        DWORD old;
        if (g_lcPageProtect[k] != 0) VirtualProtect((void*)g_lcPages[k], 0x1000, g_lcPageProtect[k], &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)g_lcPages[k], 0x1000);
    }
}

/* Every other thread suspended: the stand-ins copied into their entries, the queued writes made. Then the rewrites noted
 * for the Lua runtimes (hellforge_lua_rewrites), outside the freeze. */
static void LiveFreeze(void)
{
    FarSuspend();
    for (int kind = 0; kind < LC_KINDS; kind++)
        for (uint32_t i = 0; i < g_lcBefore[kind]; i++)
            if (g_lcShadowed[kind][i])
            {
                memcpy(LiveAt(kind, i), g_lcShadow[kind] + i * kLiveKinds[kind].stride, kLiveKinds[kind].stride);
                g_lcTomb[kind][i] = 0;
            }
    LiveWritesMake();
    FarResume();
    for (int i = 0; i < g_lcWriteCount; i++)
        if (g_lcWrites[i].at >= g_lcImageFrom && g_lcWrites[i].at < g_lcImageTo) NoteRewrite(g_lcWrites[i].at, g_lcWrites[i].size);
}

/* ---- each kind's install ------------------------------------------------------------------------------------------- */

/* Whether the change registers anything of this kind: an entry after the set-ups began, or a stand-in. */
static int LiveChanged(int kind)
{
    if (*kLiveKinds[kind].count > g_lcBefore[kind]) return 1;
    for (uint32_t i = 0; i < g_lcBefore[kind]; i++) if (g_lcShadowed[kind][i]) return 1;
    return 0;
}

/* The change's entries of a kind, one at a time: index the entry's place, the entry to install (a stand-in, or the entry
 * itself) and home, where it lives once made. 0 when there are no more. */
static int LiveNext(int kind, uint32_t* index, uint8_t** entry, uint8_t** home)
{
    for (uint32_t i = *index; i < *kLiveKinds[kind].count; i++)
    {
        if (i < g_lcBefore[kind] && !g_lcShadowed[kind][i]) continue;
        *index = i;
        *home = LiveAt(kind, i);
        *entry = i < g_lcBefore[kind] ? g_lcShadow[kind] + i * kLiveKinds[kind].stride : *home;
        return 1;
    }
    return 0;
}

/* A kind whose install refused at boot takes the change's entries as the boot does: numbered, not installed. NULL when
 * the kind installs; "<reason>\t<why>" when the game has not finished installing it yet. */
static const char* LiveKindState(int kind, int state, char* why, size_t size)
{
    if (state == 0) { snprintf(why, size, "state\tthe game has not installed its %ss yet; try again in a moment", kLiveKinds[kind].name); return why; }
    if (state < 0)
    {
        uint8_t *entry, *home;
        for (uint32_t i = 0; LiveNext(kind, &i, &entry, &home); i++)
            ApiLog(*(HfMod**)entry, "%s %u not installed: %ss were refused when the game started (modloader.log)", kLiveKinds[kind].name, kLiveKinds[kind].first + i,
                kLiveKinds[kind].name);
    }
    return NULL;
}

/* Rows that cannot go in a list's block (a stand-in's: its entry's row is in use), never freed. */
static uint8_t* LiveSpareRow(uint8_t** pool, uint32_t* used, size_t row, uint32_t count)
{
    if (*pool == NULL) *pool = (uint8_t*)VirtualAlloc(NULL, row * count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return *pool != NULL && *used < count ? *pool + row * (*used)++ : NULL;
}

/* The change's install plan for after the freeze: lists whose block the freeze put in, stubs to build, items. */
typedef struct LivePlan
{
    uintptr_t game;
    uint8_t *stratagems, *projectiles, *damage; /* a list moved by this change: its block */
    int behaviourStubs, abilityStubs, categories, launch, difficulty;
    OwnBatch items;
    Scrub scrub;          /* what the change takes out that the loadout may hold */
    int scrubbing, scrubFailed;
} LivePlan;

static void LiveOwn(LivePlan* plan, OwnItem item)
{
    OwnBatch* b = &plan->items;
    if (b->count == b->capacity)
    {
        int capacity = b->capacity ? b->capacity * 2 : 64;
        OwnItem* grown = (OwnItem*)realloc(b->items, sizeof *grown * (size_t)capacity);
        if (grown == NULL) { b->why = "out of memory: some of the change's items are owned or given back from the next session"; return; }
        b->items = grown;
        b->capacity = capacity;
    }
    b->items[b->count++] = item;
}

/* What an earlier change's plan holds (its item and scrub lists), freed before the next one is made. */
static void LivePlanFree(LivePlan* plan)
{
    free(plan->items.items);
    free(plan->scrub.weapons);
    memset(plan, 0, sizeof *plan);
}

/* A kind's gate: 1 when the change's entries of it install now; 0 when there are none, the kind was refused at boot (its
 * entries numbered, not installed), or (*refused) the game has not installed it yet. */
static int LiveKindInstalls(int kind, int state, char* why, size_t size, const char** refused)
{
    *refused = NULL;
    if (!LiveChanged(kind)) return 0;
    if (LiveKindState(kind, state, why, size) != NULL) { *refused = why; return 0; }
    return state > 0;
}

static const char* LiveFailed(char* why, size_t size, const char* what, const char* failed)
{
    snprintf(why, size, "capability\t%s: %s", what, failed != NULL ? failed : "no memory for its row");
    return why;
}

/* One of the change's stratagems: its row (in the block for a new type, else a spare one: the stood-in entry's row is in
 * use), its list entry and name, its item to own (a new type's, one back at its tombstone, or one taking a type over: a
 * new version keeps its item). NULL, or why not. */
static const char* LiveStratagem(LivePlan* plan, uint8_t* table, uint32_t index, Stratagem* s, Stratagem* home)
{
    static uint8_t* spare;
    static uint32_t spareUsed;
    int added = index >= g_lcBefore[LC_STRATAGEM], owned = added || g_lcShadowed[LC_STRATAGEM][index] != LC_STAND_SAME;
    s->row = added ? StratagemNewRow(index) : LiveSpareRow(&spare, &spareUsed, STR_ROW, STR_MAX_NEW);
    const char* failed = s->row != NULL ? StratagemRow(s, home, table) : NULL;
    if (s->row == NULL || failed != NULL) return failed != NULL ? failed : "no memory for its row";
    const char* name = home->name;
    LivePut((uintptr_t)table + s->type * 8, &s->row, 8);
    LivePut((uintptr_t)table + STR_NAMES_AT + s->type * 8, &name, 8);
    plan->categories |= s->selectionCategory != 0;
    uint64_t base = 0;
    uint32_t baseStable = 0;
    memcpy(&base, table + s->base * 8, 8);
    if (owned && base != 0 && !SafeRead(base + 4, &baseStable, 4)) LiveOwn(plan, (OwnItem){OWN_STRATAGEM, s->item, s->stable, baseStable, 0, 0});
    return NULL;
}

static const char* LiveStratagems(LivePlan* plan, char* why, size_t size)
{
    const char* refused;
    if (!LiveKindInstalls(LC_STRATAGEM, g_stratagemState, why, size, &refused)) return refused;
    uint8_t* table = g_stratagemTable;
    const char* failed = NULL;
    if (table == NULL && ((failed = StratagemSitesFailed(plan->game)) != NULL || (table = StratagemBlock(plan->game, &failed)) == NULL))
        return LiveFailed(why, size, "stratagems", failed);
    uint8_t *entry, *home;
    for (uint32_t i = 0; LiveNext(LC_STRATAGEM, &i, &entry, &home); i++)
        if ((failed = LiveStratagem(plan, table, i, (Stratagem*)entry, (Stratagem*)home)) != NULL) return LiveFailed(why, size, ((Stratagem*)entry)->name, failed);
    uint32_t count = STR_VANILLA + g_stratagemCount;
    if (g_stratagemTable == NULL && (failed = StratagemMove(plan->game, table, LivePut)) != NULL) return LiveFailed(why, size, "stratagems", failed);
    LivePut((uintptr_t)table + STR_NAMES_AT + count * 8, &g_stratagemCountName, 8); /* "Count" after the last type */
    if (g_stratagemCount > g_lcBefore[LC_STRATAGEM] && (failed = StratagemRaise(plan->game, table, count, g_stratagemTable == NULL, LivePut)) != NULL)
        return LiveFailed(why, size, "stratagems", failed);
    if (g_stratagemTable == NULL) plan->stratagems = table;
    return NULL;
}

/* The damage list a first move needs: the vanilla pointers in a block near game.dll. NULL, and why. */
static uint8_t* LiveDamageList(uintptr_t game, const char** failed)
{
    if ((*failed = DamageSitesFailed(game)) != NULL) return NULL;
    uint8_t* table = AllocateNearGame(game, (size_t)(DMG_VANILLA + DMG_MAX_NEW) * 8);
    *failed = "no memory near game.dll for the moved list";
    return table != NULL && !SafeRead(game + DMG_TABLE, table, DMG_VANILLA * 8) ? table : NULL;
}

static const char* LiveDamage(LivePlan* plan, char* why, size_t size)
{
    const char* refused;
    if (!LiveKindInstalls(LC_DAMAGE, g_damageState, why, size, &refused)) return refused;
    const char* failed = NULL;
    uint8_t* table = g_damageTable != NULL ? g_damageTable : LiveDamageList(plan->game, &failed);
    if (table == NULL) return LiveFailed(why, size, "damage", failed);
    static uint8_t* rows;
    static uint32_t rowsUsed;
    uint8_t *entry, *home;
    for (uint32_t i = 0; LiveNext(LC_DAMAGE, &i, &entry, &home); i++)
    {
        Damage* d = (Damage*)entry;
        uint8_t* row = LiveSpareRow(&rows, &rowsUsed, DMG_ROW, DMG_MAX_NEW * 2);
        if (row == NULL || (failed = DamageRow(d, row, plan->game)) != NULL) return LiveFailed(why, size, d->name, failed);
        LivePut((uintptr_t)table + d->id * 8, &d->row, 8);
    }
    if (g_damageTable == NULL && (failed = DamageMove(plan->game, table, LivePut)) != NULL) return LiveFailed(why, size, "damage", failed);
    if (g_damageTable == NULL) plan->damage = table;
    return NULL;
}

/* The projectile list a first move needs: its block (rows for every new type after the list), the vanilla pointers
 * copied in. NULL, and why. */
static uint8_t* LiveProjectileList(uintptr_t game, const char** failed)
{
    if ((*failed = ProjectilePinsFailed(game)) != NULL) return NULL;
    if (g_projectileBlock == NULL) g_projectileBlock = AllocateNearGame(game, PRJ_ROWS_AT + (size_t)PRJ_MAX_NEW * PRJ_ROW);
    *failed = "no memory near game.dll for the moved list";
    return g_projectileBlock != NULL && !SafeRead(game + PRJ_TABLE, g_projectileBlock, PRJ_VANILLA * 8) ? g_projectileBlock : NULL;
}

static const char* LiveProjectiles(LivePlan* plan, char* why, size_t size)
{
    const char* refused;
    if (!LiveKindInstalls(LC_PROJECTILE, g_projectileState, why, size, &refused)) return refused;
    const char* failed = NULL;
    uint8_t* table = g_projectileTable != NULL ? g_projectileTable : LiveProjectileList(plan->game, &failed);
    if (table == NULL) return LiveFailed(why, size, "projectiles", failed);
    static uint8_t* spare;
    static uint32_t spareUsed;
    uint8_t *entry, *home;
    for (uint32_t i = 0; LiveNext(LC_PROJECTILE, &i, &entry, &home); i++)
    {
        Projectile* p = (Projectile*)entry;
        p->row = i >= g_lcBefore[LC_PROJECTILE] ? g_projectileBlock + PRJ_ROWS_AT + (size_t)i * PRJ_ROW : LiveSpareRow(&spare, &spareUsed, PRJ_ROW, PRJ_MAX_NEW);
        if (p->row == NULL || ProjectileRow(p, plan->game, &failed) <= 0) return LiveFailed(why, size, p->name, p->row ? failed : NULL);
        LivePut((uintptr_t)table + p->type * 8, &p->row, 8);
    }
    if (g_projectileTable == NULL && (failed = ProjectileMove(plan->game, table, LivePut)) != NULL) return LiveFailed(why, size, "projectiles", failed);
    if (g_projectileCount > g_lcBefore[LC_PROJECTILE] && (failed = ProjectileRaise(plan->game, PRJ_VANILLA + g_projectileCount, LivePut)) != NULL)
        return LiveFailed(why, size, "projectiles", failed);
    if (g_projectileTable == NULL) plan->projectiles = table;
    return NULL;
}

/* Behaviours: each new id's answers in the stubs' tables, then the stubs' COUNT (below 256: its low byte). Without stubs
 * yet, they are built after the freeze (InstallBehaviours: its branches are atomic stores). */
static const char* LiveBehaviours(LivePlan* plan, char* why, size_t size)
{
    const char* refused;
    if (!LiveKindInstalls(LC_BEHAVIOUR, g_behaviourState, why, size, &refused)) return refused;
    if (g_bhvBlock == NULL) { plan->behaviourStubs = 1; return NULL; }
    uint8_t *entry, *home;
    for (uint32_t i = 0; LiveNext(LC_BEHAVIOUR, &i, &entry, &home); i++)
    {
        uint32_t stages, resume;
        uintptr_t to, from;
        BehaviourAnswers((const Behaviour*)entry, plan->game, &stages, &to, &from, &resume);
        LivePutCode((uintptr_t)g_bhvBlock + 0x700 + 4 * i, &stages, 4);
        LivePutCode((uintptr_t)g_bhvBlock + 0x740 + 8 * i, &to, 8);
        LivePutCode((uintptr_t)g_bhvBlock + 0x780 + 8 * i, &from, 8);
        LivePutCode((uintptr_t)g_bhvBlock + 0x7C0 + 4 * i, &resume, 4);
    }
    for (int k = 0; k < g_bhvCountSites && g_behaviourCount > g_lcBefore[LC_BEHAVIOUR]; k++) LivePutCode((uintptr_t)g_bhvCountAt[k], &g_behaviourCount, 4);
    return NULL;
}

/* Abilities: each new one's base function, then the stub's COUNT. A silo countdown needs the launch functions proven;
 * without a stub yet, it is built after the freeze (InstallAbilities: its branch is an atomic store). */
static const char* LiveAbilities(LivePlan* plan, char* why, size_t size)
{
    const char* refused;
    if (!LiveKindInstalls(LC_ABILITY, g_abilityState, why, size, &refused)) return refused;
    if (g_ablCountAt == NULL) { plan->abilityStubs = 1; return NULL; }
    uint8_t *entry, *home;
    for (uint32_t i = 0; LiveNext(LC_ABILITY, &i, &entry, &home); i++)
    {
        Ability* a = (Ability*)entry;
        if (!AbilityBase(a, plan->game)) { snprintf(why, size, "capability\tability %u: its base's case is not the reviewed shape", a->id); return why; }
        plan->launch |= a->holdsLaunch && !g_launchReady;
    }
    if (g_abilityCount > g_lcBefore[LC_ABILITY]) LivePutCode((uintptr_t)g_ablCountAt, &g_abilityCount, 4);
    return NULL;
}

/* A live level 11 (difficulty.c): its install as a boot runs it (InstallDifficulty), every write it makes to game.dll
 * queued for the freeze, then its unlock item; the highest level is raised later, once no menu is open
 * (DifficultyCapTick). Only a level without larger maps or extra objectives gets here (add_difficulty refuses them). */
static const char* LiveDifficulty(LivePlan* plan, char* why, size_t size)
{
    if (!g_difficultyLive) return NULL;
    if (g_difficultyState < 0) { ApiLog(g_difficulty.mod, "difficulty %s not installed: level 11 was refused when the game started", g_difficulty.name); return NULL; }
    if (g_difficultyState == 0) { snprintf(why, size, "state\tthe game has not checked its difficulties yet; try again in a moment"); return why; }
    if (!DifficultyCapPins(plan->game)) { snprintf(why, size, "capability\tdifficulty: the game's highest-level lookup is not the reviewed one"); return why; }
    g_writeQueue = LivePut;
    g_writeQueueThread = GetCurrentThreadId();
    int done = InstallDifficulty(plan->game, 1); /* not through state 0, which the Watch thread installs on */
    g_writeQueueThread = 0;
    if (!done || g_difficultyState != 1)
    {
        int waited = !done;
        g_difficultyState = waited ? 1 : g_difficultyState;
        snprintf(why, size, waited ? "state\tdifficulty: the game's code is not decoded yet; try again in a moment" : "capability\tdifficulty: its install was refused (modloader.log)");
        return why;
    }
    plan->difficulty = 1;
    LiveOwn(plan, (OwnItem){OWN_DIFFICULTY, g_difficulty.item, 0, 0, kDifficultyItems[DIF_VANILLA - 1], 0});
    return NULL;
}

/* A silo countdown in a session whose boot had none: the launch functions proven, the loose guard in a page of its own,
 * the per-frame pass and the mission's end. */
static void LiveLaunch(uintptr_t game)
{
    uint8_t code[32];
    int f = 0;
    for (; f < L_COUNT; f++) if (SafeRead(game + kLaunchFunctions[f].rva, code, 32) || memcmp(code, kLaunchFunctions[f].pin, 32)) break;
    g_launchReady = f == L_COUNT;
    const Ability* holder = NULL;
    for (uint32_t i = 0; i < g_abilityCount && holder == NULL; i++)
        if ((i >= g_lcBefore[LC_ABILITY] || g_lcShadowed[LC_ABILITY][i]) && g_abilities[i].holdsLaunch) holder = &g_abilities[i];
    if (!g_launchReady || holder == NULL) { if (holder) ApiLog(holder->mod, "ability %u: game.dll 0x%x is not the reviewed function; its remote launches at once", holder->id, kLaunchFunctions[f].rva); return; }
    uint8_t* stub = !g_looseGuard && GuardPins(game) ? AllocateNearGame(game, 0x1000) : NULL;
    DWORD old;
    if (stub != NULL)
    {
        EmitLooseGuard(game, stub);
        if (VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old)) { FlushInstructionCache(GetCurrentProcess(), stub, 0x1000); InstallLooseGuard(game, stub); }
    }
    ApiOnGameThread(holder->mod, SiloFrame, NULL);
    ApiSubscribe(holder->mod, HF_ON_MISSION_END, SiloMissionEnd, NULL, 0);
}

/* ---- removal: tombstones -------------------------------------------------------------------------------------------- */

static int LiveCarried(int weapon); /* live_registry.c: a registration a new version keeps */

/* The entries of the mods the change takes out become tombstones, their items given back. One a new version stands in
 * for is replaced instead; one another mod takes over is replaced too, its item given back. Before the freeze: the
 * entries are still theirs. */
static void LiveRetire(LivePlan* plan)
{
    for (int kind = 0; kind < LC_KINDS; kind++)
        for (uint32_t i = 0; i < g_lcBefore[kind]; i++)
        {
            int over = g_lcShadowed[kind][i] == LC_STAND_OVER;
            if ((g_lcShadowed[kind][i] && !over) || LiveTomb(kind, i) || !LiveLeaves(LiveModAt(kind, i))) continue;
            if (!over) g_lcTomb[kind][i] = 1;
            if (kind == LC_STRATAGEM) LiveOwn(plan, (OwnItem){OWN_STRATAGEM, g_stratagems[i].item, 0, 0, 0, 1});
        }
    for (int i = 0; i < g_lcWeaponsBefore; i++)
    {
        Weapon* w = &g_weapons[i];
        if (LiveTomb(LC_WEAPON, (uint32_t)i) || !LiveLeaves(w->mod) || LiveCarried(i)) continue;
        g_weapons[i].tomb = 1;
        if (!w->plain && w->state == 1) LiveOwn(plan, (OwnItem){OWN_WEAPON, w->id, 0, 0, 0, 1});
    }
}

/* What the change takes out that the loadout may hold: the stratagems and weapons LiveRetire makes tombstones or gives to
 * another mod. */
static void LiveScrubList(LivePlan* plan)
{
    Scrub* s = &plan->scrub;
    for (uint32_t i = 0; i < g_lcBefore[LC_STRATAGEM] && s->typeCount < STR_MAX_NEW; i++)
        if ((!g_lcShadowed[LC_STRATAGEM][i] || g_lcShadowed[LC_STRATAGEM][i] == LC_STAND_OVER) && !LiveTomb(LC_STRATAGEM, i) && LiveLeaves(g_stratagems[i].mod))
        {
            s->types[s->typeCount] = g_stratagems[i].type;
            s->stables[s->typeCount++] = g_stratagems[i].stable;
        }
    free(s->weapons);
    s->weapons = g_lcWeaponsBefore > 0 ? (uint32_t*)malloc(sizeof(uint32_t) * (size_t)g_lcWeaponsBefore) : NULL;
    s->weaponCount = 0;
    if (s->weapons == NULL && g_lcWeaponsBefore > 0) Note("loadout: out of memory listing the weapons the change takes out; a loadout may keep them");
    for (int i = 0; s->weapons != NULL && i < g_lcWeaponsBefore; i++)
    {
        const Weapon* w = &g_weapons[i];
        if (!w->plain && w->state == 1 && !LiveTomb(LC_WEAPON, (uint32_t)i) && LiveLeaves(w->mod) && !LiveCarried(i)) s->weapons[s->weaponCount++] = w->id;
    }
}

/* Whether the loadout must be scrubbed, and can be now. NULL, or "<reason>\t<why>" (the change refused: nothing changed). */
static const char* LiveScrubCheck(LivePlan* plan, char* why, size_t size)
{
    LiveScrubList(plan);
    if ((plan->scrub.typeCount == 0 && plan->scrub.weaponCount == 0) || plan->game == 0 || !ScrubNeeded(plan->game, &plan->scrub)) return NULL;
    const char* refused = ScrubReady(plan->game);
    if (refused != NULL) { snprintf(why, size, "%s", refused); return why; }
    plan->scrubbing = 1;
    return NULL;
}

static void LiveScrubVerify(void* arg)
{
    LivePlan* plan = (LivePlan*)arg;
    ScrubVerify(plan->game, &plan->scrub);
}

/* ---- the change ---------------------------------------------------------------------------------------------------- */

static const char* LiveRegistryPrepare(LivePlan* plan, char* why, size_t size); /* live_registry.c */
static void LiveRegistryStep(LivePlan* plan);
static void LiveRegistryCarry(LivePlan* plan);

/* On the game thread: selection categories, weapons and entities, then the items. */
static void LiveContentStep(void* arg)
{
    LivePlan* plan = (LivePlan*)arg;
    if (plan->categories && !g_stratagemCategoriesActive) InstallStratagemCategories(plan->game);
    else if (g_stratagemCategoriesActive && g_stratagemCategoryCount > g_lcCategoriesBefore) OrderStratagemCategories();
    LiveRegistryStep(plan);
    if (plan->scrubbing && g_livePlayers() <= 1 && g_livePlayers() != EVP_LAYOUT && ScrubReady(plan->game) == NULL) ScrubStep(plan->game, &plan->scrub);
    else if (plan->scrubbing) plan->scrubFailed = 1;
    if (plan->items.count > 0) OwnBatchStep(&plan->items);
}

/* Whether the change adds, replaces or takes out any content. */
static int LiveContentAny(void)
{
    if (g_difficultyLive) return 1; /* a level 11 a set-up added (LiveDifficulty installs it) */
    if (g_weaponCount > g_lcWeaponsBefore) return 1;
    for (int i = 0; i < g_lcWeaponsBefore; i++) if (LiveLeaves(g_weapons[i].mod)) return 1;
    for (int kind = 0; kind < LC_KINDS; kind++)
    {
        if (LiveChanged(kind)) return 1;
        for (uint32_t i = 0; i < g_lcBefore[kind]; i++) if (LiveLeaves(LiveModAt(kind, i))) return 1;
    }
    return 0;
}

/* Whether the freeze has anything to do: a queued write or a stand-in. */
static int LiveFreezeNeeded(void)
{
    if (g_lcWriteCount > 0) return 1;
    for (int kind = 0; kind < LC_KINDS; kind++)
        for (uint32_t i = 0; i < g_lcBefore[kind]; i++) if (g_lcShadowed[kind][i]) return 1;
    return 0;
}

/* Whether the writes queued so far fit the freeze (its pool and its pages). */
static const char* LiveWritesFit(LivePlan* plan, char* why, size_t size)
{
    (void)plan;
    if (!g_lcWriteFailed && LivePagesPlan()) return NULL;
    snprintf(why, size, "capacity\tthe change's writes do not fit");
    return why;
}

/* Every kind prepared: rows built, writes queued, the registry hooked, the loadout checked. NULL, or "<reason>\t<why>".
 * The difficulty last, the other kinds' writes checked before it: its install changes loader state, so after it only
 * its own writes not fitting can refuse the change. */
static const char* LivePrepare(LivePlan* plan, char* why, size_t size)
{
    static const char* (*const kinds[])(LivePlan*, char*, size_t) = {LiveDamage, LiveProjectiles, LiveStratagems, LiveBehaviours, LiveAbilities,
        LiveRegistryPrepare, LiveScrubCheck, LiveWritesFit, LiveDifficulty, LiveWritesFit};
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++)
    {
        const char* refused = kinds[k](plan, why, size);
        if (refused != NULL) return refused;
    }
    return NULL;
}

/* After the freeze: the lists it moved in use, the first stubs built (their branches are atomic stores), a first silo
 * countdown's launch. */
static void LiveInstalled(const LivePlan* plan)
{
    if (plan->stratagems) StratagemsInstalled(plan->stratagems, plan->game, STR_VANILLA + g_stratagemCount, 0);
    HookText();
    if (plan->damage) { g_damageTable = plan->damage; DamageInstalledLog(); }
    if (plan->projectiles) { g_projectileTable = plan->projectiles; ProjectilesInstalledLog(); }
    if (plan->behaviourStubs) { g_behaviourState = 0; InstallBehaviours(plan->game); }
    if (plan->abilityStubs) { g_abilityState = 0; g_abilityFirstTry = GetTickCount64() - 60000; InstallAbilities(plan->game); }
    if (plan->launch) LiveLaunch(plan->game);
    if (plan->difficulty)
    {
        g_difficultyLive = 0;
        /* the Watch thread's tick tries while no other step is under way (the fixture's runs in place) */
        g_difficultyOnGameThread = g_liveOnGameThread == LiveOnGameThreadNow ? LiveOnGameThreadTry : g_liveOnGameThread;
        InterlockedExchange(&g_difficultyCapPending, 1);
    }
}

/* A taken-over row's edits once the change is made: those of a mod taken out go with the old row (not written: the row
 * they applied to is no longer the game's), and the set-ups' deferred edits, which applied to the old row while they
 * ran, apply again over the new row's own bytes. Edits of mods that stay refused the takeover (LiveTakeoversCheck). */
static void LiveRowEditsFollow(int kind, uint32_t number)
{
    for (int e = g_editCount - 1; e >= 0; e--) if (LiveRowEdit(&g_edits[e], kind, number) && LiveLeaves(g_edits[e].mod)) RemoveEdit(e);
    for (int e = 0; e < g_editCount; e++)
    {
        const FieldEdit* edit = &g_edits[e];
        int first = LiveRowEdit(edit, kind, number) && !edit->live && edit->field->size <= FIELD_BEFORE;
        for (int o = 0; o < e && first; o++) first = !SameField(&g_edits[o], edit); /* each field once */
        int waiting = 0;
        uint8_t* row = first ? EditRecord(edit, number, 0, &waiting) : NULL;
        uint8_t own[FIELD_BEFORE];
        if (row == NULL) continue;
        memcpy(own, row + edit->field->offset, edit->field->size);
        FieldRestack(edit, own);
    }
}

static void LiveEditsFollow(void)
{
    AcquireSRWLockExclusive(&g_fieldsLock);
    for (int kind = 0; kind < LC_KINDS; kind++)
        for (uint32_t i = 0; i < g_lcBefore[kind] && kLiveKinds[kind].table != NULL; i++)
            if (g_lcShadowed[kind][i] == LC_STAND_OVER) LiveRowEditsFollow(kind, kLiveKinds[kind].first + i);
    ReleaseSRWLockExclusive(&g_fieldsLock);
}

/* The game thread's step, the loadout's push checked a few frames on; 0 when something did not happen. */
static int LiveSteps(LivePlan* plan)
{
    int ok = 1;
    if (!g_liveOnGameThread(LiveContentStep, plan)) { Note("live content: the game thread did not take the change's step"); ok = 0; }
    if (plan->scrub.pushed) { Sleep(250); g_liveOnGameThread(LiveScrubVerify, plan); } /* the push's request handled meanwhile */
    if (plan->scrubFailed) { Note("loadout: the loadout screen opened before the change's step; what it took out may stay in the loadout"); ok = 0; }
    else if (plan->scrubbing) Note("loadout: %d slot(s), pair(s) or weapon(s) of what the change took out left the loadout", plan->scrub.scrubbed);
    if (plan->items.why != NULL) Note("items: %s", plan->items.why);
    return ok;
}

/* The change's content, once every check passed: prepared, one freeze, the stubs, one game-thread step. NULL, or
 * "<reason>\t<why>" when it could not be prepared (nothing changed: the set-ups' content stays staged, and the refusal
 * takes it out again, LiveContentUnstage); *failed when something went wrong after the game changed. */
static const char* LiveContentMake(char* why, size_t size, int* failed)
{
    *failed = 0;
    if (!LiveContentAny()) { g_lcOpen = 0; TextsPublish(); return NULL; }
    static LivePlan plan;
    LivePlanFree(&plan);
    size_t image = g_lcFixtureGameSize;
    plan.game = g_lcFixtureGame ? g_lcFixtureGame : ApiModuleBase("game.dll", &image);
    g_lcImageFrom = plan.game;
    g_lcImageTo = plan.game + image;
    if (g_game == 0) g_game = plan.game;
    g_lcWriteCount = g_lcWriteFailed = 0;
    g_lcPoolUsed = 0;
    const char* refused = LivePrepare(&plan, why, size);
    if (refused != NULL) return refused;
    g_lcOpen = 0; /* made from here on: the set-ups' content stays */
    TextsPublish();
    if (plan.items.count > 0 && !OwnProve()) Note("items: the game's catalogue functions are not the reviewed ones; new items are not owned until the next session");
    LiveRegistryCarry(&plan);
    LiveRetire(&plan);
    if (LiveFreezeNeeded()) LiveFreeze();
    LiveInstalled(&plan);
    LiveEditsFollow();
    *failed = !LiveSteps(&plan);
    WriteRegisteredItems();
    Note("live content: %d write(s) under one freeze (%d page(s)); %d catalogue record(s) added, %d item(s) owned, %d given back, %d not owned "
        "(the player does not hold what it copies, or it is not in the catalogue)", g_lcWriteCount, g_lcPageCount, plan.items.added, plan.items.owned,
        plan.items.taken, plan.items.unowned);
    return NULL;
}
