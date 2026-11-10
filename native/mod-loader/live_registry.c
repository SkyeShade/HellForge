/* Weapons and entities in a live change (live_content.c, SDK 16: docs/modding/live.md "Content"). Included by
 * live_mods.c after live_content.c.
 *
 * Numbering. A boot gives registered entities their short ids, and with them their network types, in the order of their
 * resource ids (WeaponOrder), after the last vanilla one. A live change keeps every short id an entity that stays has,
 * and appends the set-ups' entities after the last one given; that must be where a boot of the change's mods would put
 * them (the entities that stay and the new ones, by resource id, numbered from the first after the vanilla ones). An
 * entity of a mod the change takes out keeps its short id (its rows stay: a tombstone), so a new entity numbered after it,
 * or one whose resource id sorts before an entity that stays, refuses the change: unlike the other kinds' numbers, a
 * short id never passes to another mod's entity (it is the engine's entity type, and what the game keeps per type is not
 * known; KB hellforge-live-mod-sync-tombstone-reuse). A new version's entity defined as its
 * old version's keeps the old registration (carried over); defined otherwise it refuses the change.
 *
 * Install, in the game thread's step (KB hellforge-live-mod-sync-re-runtime, "A late RegisterLate"): the registry's hooks
 * (entity loader, armory builder) when the boot had none, taken on the change's thread; each new entity's rows
 * (RegisterWeapon, a row's key last); the loadout table and short-id list copies (made now when the boot made none); the
 * network types appended (counts last); the apply switch's entries, then its bound; the armory built into a fresh object
 * and swapped in (the old one is never freed: a reader may hold it); texts. Network type indices follow the short ids as
 * at boot: a weapon whose base has no network type gets none, as RegisterNetworkTypes does. */

/* For each entry before the change (g_lrCarryCount of them): the set-up's entry a new version carries it over to, or -1.
 * Sized at each check (LiveCarryReset), on the change's thread. */
static int* g_lrCarry;
static int g_lrCarryCount;
static uint32_t g_lrNetBefore;

static int LiveCarried(int weapon) { return weapon >= 0 && weapon < g_lcWeaponsBefore && weapon < g_lrCarryCount && g_lrCarry[weapon] >= 0; }

/* No entry carried over, with room for one per entry before the change: 1, or 0 out of memory. */
static int LiveCarryReset(void)
{
    int count = g_lcWeaponsBefore > 0 ? g_lcWeaponsBefore : 0;
    if (count > g_lrCarryCount)
    {
        int* grown = (int*)realloc(g_lrCarry, sizeof(int) * (size_t)count);
        if (grown == NULL) return 0;
        g_lrCarry = grown;
        g_lrCarryCount = count;
    }
    for (int i = 0; i < g_lrCarryCount; i++) g_lrCarry[i] = -1;
    return 1;
}

static int SameText(const char* a, const char* b) { return !strcmp(a, b); }

/* Whether two registrations define the same entity (a new version's, and its old one's). */
static int WeaponSame(const Weapon* a, const Weapon* b)
{
    if (a->plain != b->plain || a->base != b->base || a->id != b->id || a->baseEntity != b->baseEntity || a->unit != b->unit ||
        a->behaviour != b->behaviour || a->modelFrom != b->modelFrom || a->projectile != b->projectile || a->soundFrom != b->soundFrom ||
        a->ownAll != b->ownAll || a->ownCount != b->ownCount || a->partCount != b->partCount || memcmp(a->text, b->text, sizeof a->text))
        return 0;
    for (int k = 0; k < a->ownCount; k++) if (a->own[k] != b->own[k]) return 0;
    return SameText(a->name, b->name) && SameText(a->title, b->title) && SameText(a->designation, b->designation);
}

/* A set-up's entity whose resource id a registered entry has: carried over when that entry's mod leaves and defines it
 * the same. NULL, or "<reason>\t<why>". */
static const char* LiveWeaponCarry(int pending, char* why, size_t size)
{
    const Weapon* p = &g_weapons[pending];
    for (int e = 0; e < g_lcWeaponsBefore; e++)
    {
        const Weapon* w = &g_weapons[e];
        if (w->entity != p->entity) continue;
        if (w->state == 1 && (LiveTomb(LC_WEAPON, (uint32_t)e) || LiveLeaves(w->mod)) && e < g_lrCarryCount && g_lrCarry[e] < 0 && !strcmp(w->mod->id, p->mod->id) && WeaponSame(w, p))
        { g_lrCarry[e] = pending; return NULL; }
        snprintf(why, size, "capability\t%s's %s %s was registered otherwise by its old version; a restart registers it anew", p->mod->id,
            p->plain ? "entity" : "weapon", p->name);
        return why;
    }
    return NULL;
}

/* A carried entry's set-up twin (the one dropped when the change is made). */
static int LiveWeaponIsCarry(int pending)
{
    for (int e = 0; e < g_lcWeaponsBefore && e < g_lrCarryCount; e++) if (g_lrCarry[e] == pending) return 1;
    return 0;
}

/* The last vanilla short id: the game's list's last entry (HellForge's copy holds no added entity when none registered). */
static uint32_t LiveVanillaLastShort(void)
{
    uint8_t* list = StatePointer(EntityState(), REG_LIST);
    uint32_t bytes = 0, last = 0;
    if (list == NULL || SafeRead((uintptr_t)list - 12, &bytes, 4) || bytes < 8) return 0;
    SafeRead((uintptr_t)list + bytes - 8, &last, 4);
    return last;
}

/* Whether an entry is registered after the change: one that stays (or is carried over), or a set-up's not carried. */
static int LiveWeaponAfter(int i)
{
    const Weapon* w = &g_weapons[i];
    if (i >= g_lcWeaponsBefore) return w->state == 2 && !LiveWeaponIsCarry(i);
    return w->state == 1 && (LiveCarried(i) || (!LiveTomb(LC_WEAPON, (uint32_t)i) && !LiveLeaves(w->mod)));
}

/* The short ids registered entities hold, a tombstone's too: the first (right after the vanilla ones) and the last. */
static void LiveShortIds(uint32_t* first, uint32_t* last)
{
    int registered = 0;
    *first = *last = 0;
    for (int i = 0; i < g_lcWeaponsBefore; i++)
    {
        if (g_weapons[i].state != 1) continue;
        if (!registered++ || g_weapons[i].shortId < *first) *first = g_weapons[i].shortId;
        if (g_weapons[i].shortId > *last) *last = g_weapons[i].shortId;
    }
    if (!registered) { *first = LiveVanillaLastShort() + 1; *last = *first - 1; }
}

/* Whether a set-up's entity is one the change leaves out of the rows it adds (its old version's registration is kept). */
static int LiveRowsSkip(int i) { return i < g_lcWeaponsBefore || LiveWeaponIsCarry(i); }

/* The game's entity map rows in use (32-byte rows, an empty one's key 0: FreeRow). */
static uint32_t LiveMapUsed(const uint8_t* map)
{
    uint32_t used = 0;
    for (uint32_t i = 0; i < REG_MAP_SLOTS; i++)
    {
        uint64_t key;
        memcpy(&key, map + (size_t)i * 32, 8);
        used += key != 0;
    }
    return used;
}

/* Whether the game's entity map has a free row for each entity the set-ups register (`needed`): its 4096 rows are
 * compiled into the game, and RegisterWeapon refuses an entity without one while the others go in, which would number
 * them as no boot of these mods does (a boot refuses the ones sorting last). NULL, or "<reason>\t<why>". */
static const char* LiveMapCheck(const uint8_t* map, uint32_t needed, char* why, size_t size)
{
    uint32_t left = REG_MAP_SLOTS - LiveMapUsed(map);
    if (needed <= left) return NULL;
    snprintf(why, size, "capability\tthese mods' weapons and entities need %u row(s) in the game's entity map, which has %u of its %u free; "
        "a restart registers the ones that fit, as a boot of these mods does", needed, left, REG_MAP_SLOTS);
    return why;
}

/* Whether the component tables have a free row for each row the set-ups' entities take. A boot grows a table its
 * entities would fill before adding them (registry_load.c GrowForPending); a running game's tables keep their slot
 * counts, and RegisterWeapon refuses an entity whose table is full while the others go in, which would number them as no
 * boot of these mods does. So the change refuses instead, and the restart grows the table. NULL, or "<reason>\t<why>". */
static const char* LiveTablesCheck(char* why, size_t size)
{
    uint8_t* state = EntityState();
    uint8_t* map = StatePointer(state, REG_MAP);
    uint32_t records;
    uint8_t* loadout = LoadoutTable(state, &records);
    if (map == NULL || loadout == NULL) return NULL; /* not loaded: nothing is registered into them yet */
    uint32_t entities = 0;
    for (int i = g_lcWeaponsBefore; i < g_weaponCount; i++) entities += g_weapons[i].state == 2 && !LiveRowsSkip(i);
    if (LiveMapCheck(map, entities, why, size) != NULL) return why;
    static uint32_t rows[GROW_COMPONENTS];
    memset(rows, 0, sizeof rows);
    PendingRows(map, loadout, records, rows, 2, LiveRowsSkip);
    for (uint32_t c = 0; c < GROW_COMPONENTS; c++)
    {
        uint8_t* table = rows[c] != 0 ? StatePointer(state, REG_TABLES + 8 * c) : NULL;
        uint32_t slots = SlotsOf(c);
        if (table == NULL || slots == 0) continue; /* a component without data, or not understood: RegisterWeapon's own */
        uint32_t used = UsedRows(table, slots);
        if (used + rows[c] <= slots) continue;
        snprintf(why, size, "capability\tthese mods' weapons and entities need %u row(s) in component table %u, which has %u of its %u "
            "free; a restart grows it", rows[c], c, used < slots ? slots - used : 0, slots);
        return why;
    }
    return NULL;
}

/* The network type index's width a boot of the change's mods would have against the session's. A boot sends it in 12
 * bits only when its entities need them (registry_load.c NetworkTypesToAdd), else in 11; a session keeps what its boot
 * set (a width is never narrowed, and widened only before the session has connections). A friend with these mods reads
 * and writes type indices in the boot's width, so a change that would need another refuses. Only a change that adds or
 * takes out entities (an earlier change was checked with the rest); after: the entities registered after it. NULL, or
 * "<reason>\t<why>". */
static const char* LiveNetWidthCheck(uint32_t after, char* why, size_t size)
{
    int changed = 0;
    for (int i = 0; i < g_weaponCount && !changed; i++)
        changed = i >= g_lcWeaponsBefore ? g_weapons[i].state == 2 && !LiveWeaponIsCarry(i)
                                         : g_weapons[i].state == 1 && !LiveTomb(LC_WEAPON, (uint32_t)i) && LiveLeaves(g_weapons[i].mod) && !LiveCarried(i);
    if (!changed) return NULL;
    NetworkConfig n;
    uint8_t width = 0;
    if (!ReadNetworkConfig(&n) || SafeRead(n.config + 0xC, &width, 1)) return NULL; /* not loaded: nothing is numbered in it yet */
    uint32_t vanilla = g_networkVanillaTypes != 0 ? g_networkVanillaTypes : n.count;
    uint32_t needed = vanilla + after > (1u << NET_TYPE_BITS) ? NET_WIDE_BITS : NET_TYPE_BITS;
    if (width == needed) return NULL;
    snprintf(why, size, "capability	these mods' %u weapons and entities take a %u-bit network type index, and this session's is %u bits; a restart "
        "sets it as the host's", after, needed, width);
    return why;
}

/* After the numbering: the tables' free rows, then the network type index's width. */
static const char* LiveRowsAndWidthCheck(uint32_t after, char* why, size_t size)
{
    const char* refused = LiveTablesCheck(why, size);
    return refused != NULL ? refused : LiveNetWidthCheck(after, why, size);
}

/* The short ids of the entities registered after the change, in resource id order (`order`, WeaponOrder): each the one a
 * boot of these mods gives it. *rank: how many there are. NULL, or "<reason>\t<why>". */
static const char* LiveShortIdsCheck(const int* order, uint32_t* rank, char* why, size_t size)
{
    uint32_t first, last;
    LiveShortIds(&first, &last);
    uint32_t next = last + 1;
    *rank = 0;
    for (int k = 0; k < g_weaponCount; k++)
    {
        int i = order[k];
        if (!LiveWeaponAfter(i)) continue;
        const Weapon* w = &g_weapons[i];
        uint32_t expected = first + (*rank)++, has = i < g_lcWeaponsBefore ? w->shortId : next++;
        if (has == expected) continue;
        snprintf(why, size, "capability\t%s's %s %s %s short id %u, which a boot of these mods gives %u; a restart numbers it as the host does",
            w->mod->id, w->plain ? "entity" : "weapon", w->name, i < g_lcWeaponsBefore ? "has" : "would take", has, expected);
        return why;
    }
    return NULL;
}

static const char* LiveWeaponsCheck(char* why, size_t size)
{
    int* order = LiveCarryReset() ? WeaponOrder() : NULL;
    if (order == NULL) { snprintf(why, size, "state\tHellForge is out of memory; try again in a moment"); return why; }
    const char* refused = NULL;
    for (int p = g_lcWeaponsBefore; p < g_weaponCount && refused == NULL; p++)
        if (g_weapons[p].state == 2) refused = LiveWeaponCarry(p, why, size);
    uint32_t rank = 0;
    if (refused == NULL) refused = LiveShortIdsCheck(order, &rank, why, size);
    free(order);
    return refused != NULL ? refused : LiveRowsAndWidthCheck(rank, why, size);
}

/* Once the change is made: each carried registration is the new version's, its item owned again (a mod taken out gave
 * it back); the set-ups' twins of them go. */
static void LiveRegistryCarry(LivePlan* plan)
{
    for (int e = 0; e < g_lcWeaponsBefore && e < g_lrCarryCount; e++)
        if (g_lrCarry[e] >= 0)
        {
            Weapon* w = &g_weapons[e];
            w->mod = g_weapons[g_lrCarry[e]].mod;
            w->tomb = 0; /* a mod back with what it had: no longer a tombstone */
            if (!w->plain) LiveOwn(plan, (OwnItem){OWN_WEAPON, w->id, 0, 0, w->base, 0});
            ApiLog(w->mod, "%s: its registration is kept", w->name);
        }
    int kept = g_lcWeaponsBefore;
    for (int p = g_lcWeaponsBefore; p < g_weaponCount; p++) if (!LiveWeaponIsCarry(p)) g_weapons[kept++] = g_weapons[p];
    g_weaponCount = kept;
    for (int i = 0; i < g_lrCarryCount; i++) g_lrCarry[i] = -1;
}

static int LiveWeaponsPending(void)
{
    for (int i = g_lcWeaponsBefore; i < g_weaponCount; i++) if (g_weapons[i].state == 2 && !LiveWeaponIsCarry(i)) return 1;
    return 0;
}

/* On the change's thread: the entity data loaded, and the registry's hooks in (the armory builder must be HellForge's
 * before the first late weapon: the game's builds 193 records). NULL, or "<reason>\t<why>". */
static const char* LiveRegistryPrepare(LivePlan* plan, char* why, size_t size)
{
    (void)plan;
    if (!LiveWeaponsPending()) return NULL;
    uint8_t* state = EntityState();
    uint32_t records;
    if (!g_fired[HF_EVENT_ENTITIES_READY] || StatePointer(state, REG_MAP) == NULL || StatePointer(state, REG_LIST) == NULL || LoadoutTable(state, &records) == NULL)
    { snprintf(why, size, "state\tthe game has not loaded its entities yet; try again in a moment"); return why; }
    if (!RegistryHook()) { snprintf(why, size, "capability\tthe weapon registry could not hook the game (modloader.log)"); return why; }
    return NULL;
}

/* An entity's network type at index, a copy of type found's, not yet counted. The copy owns its field lists
 * (CopyNetworkEntry): a byte copy's were freed twice when the game quit. */
static void LiveNetworkEntry(const Weapon* w, uint32_t index, int found)
{
    if (CopyNetworkEntry(g_netEntries + (size_t)index * 80, g_netEntries + (size_t)found * 80))
        ApiLog(w->mod, "%s: its network type keeps its base's field lists (no memory for its own)", w->name);
    memcpy(g_netEntries + (size_t)index * 80, &w->shortId, 4);
    memcpy(g_netOwn + (size_t)index * 4, &index, 4);
    g_netValues[index] = g_netValues[found];
    g_networkTypes[g_networkTypeCount++] = (NetworkType){index, (uint32_t)found, w->entity, w->copies, w->name, w->mod};
    ApiLog(w->mod, "%s: network entity type %u (a copy of type %d)", w->name, index, found);
}

/* Whether an entity has a network type already (one of g_networkTypes). */
static int LiveNetworkTypeKnown(const Weapon* w)
{
    for (uint32_t t = 0; t < g_networkTypeCount; t++) if (g_networkTypes[t].entity == w->entity) return 1;
    return 0;
}

/* An entity's network type at `index` (after lastKey, within the arrays and the width): 1 when it was given, else 0 (logged). */
static int LiveNetworkTypeAt(const Weapon* w, uint32_t index, uint32_t lastKey, uint8_t width)
{
    int found = NetworkEntryOf(g_netEntries, g_networkVanillaTypes, w->baseShortId);
    if (found < 0 || w->shortId <= lastKey)
    { ApiLog(w->mod, "%s: no network type to copy (base short id %u); joining games with it may fail", w->name, w->baseShortId); return 0; }
    if (index >= g_netCapacity || index >= (1u << width))
    { ApiLog(w->mod, "%s: no network type (the type index is full); joining games with it may fail", w->name); return 0; }
    if (!NetworkTypeRoom()) { ApiLog(w->mod, "%s: no network type (out of memory); joining games with it may fail", w->name); return 0; }
    LiveNetworkEntry(w, index, found);
    return 1;
}

/* The network types of the entities registered since the step began, appended to the arrays RegisterNetworkTypes made
 * (g_netCapacity entries each) within the type index's width (registry_load.c: 11 bits, or 12 when the boot's set
 * needed them); the first ever made by it. Entries first, the counts last. */
static void LiveNetworkTypes(void)
{
    if (g_networkTypesAdded == 0) { RegisterNetworkTypes(); return; }
    uintptr_t base = ApiModuleBase(g_networkModule, NULL), holder = 0, config = 0;
    uint32_t count = 0;
    uint8_t width = 0;
    if (g_networkTypesAdded != 2 || g_netEntries == NULL || base == 0 || SafeRead(base + g_networkRva, &holder, 8) || holder == 0 ||
        SafeRead(holder, &config, 8) || config == 0 || SafeRead(config + 0x70, &count, 4) || count < g_networkVanillaTypes ||
        SafeRead(config + 0xC, &width, 1) || width < NET_TYPE_BITS || width > NET_WIDE_BITS)
    { Note("registry: the network types made at boot are not there; the live weapons have none (equipping them crashes)"); return; }
    uint32_t added = 0, lastKey = 0;
    memcpy(&lastKey, g_netEntries + (size_t)(count - 1) * 80, 4);
    int* order = WeaponOrder();
    for (int k = 0; order != NULL && k < g_weaponCount; k++)
    {
        const Weapon* w = &g_weapons[order[k]];
        if (w->state != 1 || order[k] < g_lcWeaponsBefore || LiveNetworkTypeKnown(w)) continue;
        if (LiveNetworkTypeAt(w, count + added, lastKey, width)) { lastKey = w->shortId; added++; }
    }
    free(order);
    if (added == 0) return;
    uint32_t pair[2] = {count + added, count + added};
    MemoryBarrier();
    ApiWrite(config + 0x28, pair, 8);
    ApiWrite(config + 0x40, pair, 8);
    ApiWrite(config + 0x70, pair, 8);
    Note("registry: %u network entity type(s) added live (%u in all)", added, count + added);
}

/* The apply switch takes the new network types: their base's case, then the bound (0x73E + n stays below 0x77F: its low
 * byte changes only). Without a table yet (nothing was added at boot), it is installed now: solo, no friend's object
 * arrives. */
static void LiveNetSwitch(uintptr_t game)
{
    if (g_networkTypeCount == g_lrNetBefore) return;
    if (g_netSwitchTable == NULL)
    {
        if (g_netSwitchState == 1) { g_netSwitchState = 0; InstallNetSwitch(game, 1); }
        return;
    }
    uint8_t* map = StatePointer(EntityState(), REG_MAP);
    uint32_t highest = 0, bound = 0;
    SafeRead(game + NET_SWITCH_BOUND + 1, &bound, 4);
    for (uint32_t k = g_lrNetBefore; k < g_networkTypeCount; k++)
    {
        const NetworkType* t = &g_networkTypes[k];
        uint64_t own[2], copied[2];
        if (t->index >= NET_TABLE_SLOTS || t->base >= NET_VANILLA_TYPES || !NetComponents(map, t->entity, own) || !NetComponents(map, t->baseEntity, copied) ||
            !SameNetworked(EntityState(), own, copied))
        { Note("network: type %u (%s) keeps the exit (its copies get no state)", t->index, t->name); continue; }
        g_netSwitchTable[t->index] = g_netSwitchTable[t->base];
        if (t->index > highest) highest = t->index;
    }
    if (highest > bound && !ApiWrite(game + NET_SWITCH_BOUND + 1, &highest, 4)) Note("network: the apply switch's bound 0x%x -> 0x%x", bound, highest);
}

/* The armory built into a fresh object from the loadout table in use, then swapped in. */
static void LiveArmory(void)
{
    uint32_t count;
    uint8_t* table = LoadoutTable(EntityState(), &count);
    uint8_t* armory = table != NULL ? (uint8_t*)VirtualAlloc(NULL, ArmoryBytes(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : NULL;
    if (armory == NULL) { Note("registry: no memory for the armory; the live weapons are not in it"); return; }
    ArmoryFill(armory, table, count);
    MemoryBarrier();
    if (!ApiWrite(g_game + REG_ARMORY_GLOBAL, &armory, 8)) g_armory = armory;
}

/* The deferred edits of entities a set-up just registered: queued from hellforge_mod_init, when their records did not
 * exist yet (FieldRestack skips those), so nothing has written them. At a boot ApplyFieldEdits runs after the entities
 * load; here this is its turn. */
static void LiveNewEntityEdits(void)
{
    AcquireSRWLockExclusive(&g_fieldsLock);
    for (int i = 0; i < g_editCount; i++)
    {
        FieldEdit* e = &g_edits[i];
        if (!e->table && !e->live && e->address == 0 && ModEntity(e->entity) != NULL) ApplyEdit(e);
    }
    ReleaseSRWLockExclusive(&g_fieldsLock);
}

/* On the game thread: the set-ups' entities registered (in resource id order), then their deferred edits, their network
 * types, the apply switch, the armory, texts, and their items to own. */
static void LiveRegistryStep(LivePlan* plan)
{
    if (!LiveWeaponsPending()) return;
    uint8_t* state = EntityState();
    uint8_t* map = StatePointer(state, REG_MAP);
    uint8_t* list = StatePointer(state, REG_LIST);
    uint32_t records;
    uint8_t* loadout = LoadoutTable(state, &records);
    int first = g_loadoutTable == NULL;
    WeaponLoad L = {state, map};
    if (first && !CopyLoadoutAndList(&L, loadout, list)) { Note("registry: the live weapons are not registered"); return; }
    if (!first)
    {
        L.table = g_loadoutTable - 32;
        L.index = g_loadoutTable;
        L.records = g_loadoutTable + (size_t)SlotsOf(REG_LOADOUT) * 16; /* past the index as copied: grown, when table 29 was */
        L.recordCount = g_loadoutRecords;
        L.recordCapacity = g_loadoutCapacity;
        L.newList = g_regList;
        L.listCount = g_regListCount;
        L.listCapacity = g_regListCapacity;
        memcpy(&L.lastShort, L.newList + 32 + (size_t)(L.listCount - 1) * 8, 4);
    }
    L.late = 1;
    g_lrNetBefore = g_networkTypeCount;
    int ownedBefore = g_ownedCount;
    int* order = WeaponOrder();
    for (int k = 0; order != NULL && k < g_weaponCount; k++)
    {
        Weapon* w = &g_weapons[order[k]];
        if (w->state != 2) continue;
        if (RegisterWeapon(&L, w) && !w->plain) LiveOwn(plan, (OwnItem){OWN_WEAPON, w->id, 0, 0, w->base, 0});
    }
    free(order);
    HeldFollowFrom(ownedBefore); /* a table a registration moved takes the stats and edits held in it along */
    if (L.appended == 0 && first) { VirtualFree(L.table, 0, MEM_RELEASE); VirtualFree(L.newList, 0, MEM_RELEASE); return; }
    PublishWeaponLoad(&L, first);
    LiveNewEntityEdits();
    LiveNetworkTypes();
    LiveNetSwitch(g_game);
    LiveArmory();
    HookText();
    SelfCheck("live");
    Note("registry: %u weapon(s) and entities added live (loadout records %u, entities %u)", L.appended, L.recordCount, L.listCount + L.appended);
}
