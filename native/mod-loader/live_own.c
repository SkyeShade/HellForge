/* "own" stat lines in a live change (live_stats.c, M6): the entity gets a record of its own in HellForge's copy of the
 * component table, as the boot pass gives it (stats.c StatsComponent, registry.c OwnRecord): its record copied to a spare
 * one past every vanilla record, then its index row pointed at the copy (one aligned 4-byte store). New spawns take the
 * copy; entities already spawned keep the values they copied at their spawn (KB hellforge-live-mod-sync-re-runtime, 4).
 * Included by live_mods.c before live_stats.c.
 *
 * A component's first copy moves the whole table (the game is pointed at HellForge's copy, which holds the same bytes):
 * every address HellForge holds in it follows into the copy (HeldFollow), so taking a mod out later restores where the
 * game reads; a live change's registrations move tables the same way (live_registry.c). An entity's copy no longer
 * follows its shared record, so it is made only where nothing HellForge holds is in that record (no stats entry, field
 * edit or live value): otherwise the change is refused, and a restart gives the entity its record as the entity data
 * loads. A copy is never taken back: a removed mod's own line leaves the field's bottom in it. */

/* HellForge's copy of a component's table, when the game reads it (registry.c OwnRecord made it), or NULL. */
static OwnedTable* TxnOwnedTable(uint32_t component, const uint8_t* table)
{
    for (int i = 0; i < g_ownedCount; i++) if (g_owned[i].component == component && g_owned[i].index == table) return &g_owned[i];
    return NULL;
}

/* Whether HellForge holds an address in [from, to): an applied stats entry, a field edit, a live value. */
static int TxnHeld(uintptr_t from, uintptr_t to)
{
    for (int i = 0; i < g_statCount; i++) if (g_stats[i].applied && g_stats[i].address >= from && g_stats[i].address < to) return 1;
    for (int i = 0; i < g_editCount; i++) if (!g_edits[i].failed && g_edits[i].address >= from && g_edits[i].address < to) return 1;
    for (int i = 0; i < g_liveExtraCount; i++) if (g_liveExtra[i].address >= from && g_liveExtra[i].address < to) return 1;
    return 0;
}

/* A table a copy moved: the stats entries, field edits and live values HellForge holds in it follow into the copy. Under
 * both locks. */
static void HeldFollow(const OwnedTable* owned)
{
    uintptr_t from = (uintptr_t)owned->from, to = from + owned->fromBytes, moved = (uintptr_t)owned->index;
    for (int i = 0; i < g_statCount; i++) if (g_stats[i].address >= from && g_stats[i].address < to) g_stats[i].address = moved + (g_stats[i].address - from);
    for (int i = 0; i < g_editCount; i++) if (g_edits[i].address >= from && g_edits[i].address < to) g_edits[i].address = moved + (g_edits[i].address - from);
    for (int i = 0; i < g_liveExtraCount; i++)
        if (g_liveExtra[i].address >= from && g_liveExtra[i].address < to) g_liveExtra[i].address = moved + (g_liveExtra[i].address - from);
}

/* The tables copies moved from g_owned[first] on (a live change's registrations, on the game thread): what HellForge
 * holds in them follows. */
static void HeldFollowFrom(int first)
{
    if (first >= g_ownedCount) return;
    AcquireSRWLockExclusive(&g_fieldsLock);
    AcquireSRWLockExclusive(&g_statsLock);
    for (int i = first; i < g_ownedCount; i++) HeldFollow(&g_owned[i]);
    ReleaseSRWLockExclusive(&g_statsLock);
    ReleaseSRWLockExclusive(&g_fieldsLock);
}

/* An own line's component, its table in use and the entity's record there (index row +8), or NULL. */
static const Component* TxnOwnRecord(const StatEdit* e, uint8_t** table, uint32_t* record)
{
    const Component* c = NULL;
    for (int i = 0; i < g_componentCount && c == NULL; i++) if (!strcmp(g_components[i].name, e->component)) c = &g_components[i];
    *table = c != NULL ? StatePointer(g_liveEntityState(), REG_TABLES + 8 * c->index) : NULL;
    uint8_t* row = *table != NULL ? FindRow(*table, c->indices, 16, e->resource) : NULL;
    if (row == NULL) return NULL;
    memcpy(record, row + 8, 4);
    return c;
}

/* Whether an earlier own line of this change names the same entity and component. */
static int TxnOwnRepeat(int line)
{
    const StatEdit* e = &g_txn.stats[line].edit;
    for (int k = 0; k < line; k++)
    {
        const StatEdit* o = &g_txn.stats[k].edit;
        if (o->own && g_txn.stats[k].at != 0 && o->resource == e->resource && !strcmp(o->component, e->component)) return 1;
    }
    return 0;
}

/* Under both locks, before anything changes: each own line's copy can be made. NULL, or "<reason>\t<why>". */
/* Why one own line's copy cannot be made: copies already wanted of each table (need) counted in. */
static const char* TxnOwnRefused(const Component* c, uint8_t* table, uint32_t record, uint32_t need[])
{
    OwnedTable* owned = TxnOwnedTable(c->index, table);
    if (owned != NULL && record >= owned->first) return NULL; /* its own already */
    uintptr_t records = (uintptr_t)table + c->recordOffset, at = records + (uintptr_t)record * c->stride;
    uint32_t wanted = need[c - g_components]++;
    if (owned != NULL) return owned->next + wanted >= owned->end ? "HellForge's copy of its table has no room left" : TxnHeld(at, at + c->stride) ?
        "an edit or stat already holds a field of the record it would copy" : NULL;
    if (wanted >= REG_MAX_WEAPONS) return "HellForge's copy of its table has no room left";
    if (wanted == 0 && g_ownedCount >= REG_MAX_OWNED) return "HellForge copies no more tables";
    return TxnHeld(at, at + c->stride) ? "an edit or stat already holds a field of the record it would copy" : NULL; /* the table's move takes the rest */
}

static const char* TxnOwnCheck(char* why, size_t size)
{
    uint32_t need[MAX_COMPONENTS]; /* copies wanted, by the component's place in the profile */
    memset(need, 0, sizeof need);
    for (int i = 0; i < g_txn.statCount; i++)
    {
        const TxnStat* s = &g_txn.stats[i];
        uint8_t* table;
        uint32_t record;
        const Component* c = s->edit.own && s->at != 0 && !TxnOwnRepeat(i) ? TxnOwnRecord(&s->edit, &table, &record) : NULL;
        const char* refused = c != NULL ? TxnOwnRefused(c, table, record, need) : NULL;
        if (refused == NULL) continue;
        snprintf(why, size, "capability\t%s: \"own\" %s: %s; a restart gives the entity its record", g_txn.mods[s->mod].id, s->edit.label, refused);
        return why;
    }
    return NULL;
}

/* A table a copy moved: the change's own addresses into it follow (what HellForge held there: HeldFollow). */
static void TxnOwnFollow(uintptr_t from, uintptr_t to, uintptr_t moved)
{
    for (int i = 0; i < g_txn.statCount; i++) if (g_txn.stats[i].at >= from && g_txn.stats[i].at < to) g_txn.stats[i].at = moved + (g_txn.stats[i].at - from);
}

/* A component's table the first copy moved: what HellForge held in it, and this change's lines at it, follow. */
static void TxnOwnMoved(const Component* c, const uint8_t* table, const uint8_t* moved)
{
    HeldFollow(TxnOwnedTable(c->index, moved));
    TxnOwnFollow((uintptr_t)table, (uintptr_t)table + c->recordOffset + (uintptr_t)c->records * c->stride, (uintptr_t)moved);
}

/* Under both locks, first in the stats part of the change: each own line's entity gets its copy, and every line of this
 * change at its record (own or not, as at boot) lands in the copy. */
static void TxnOwnApply(void)
{
    uint8_t* state = g_liveEntityState();
    for (int i = 0; i < g_txn.statCount; i++)
    {
        TxnStat* s = &g_txn.stats[i];
        uint8_t* table;
        uint32_t record;
        const Component* c = s->edit.own && s->at != 0 ? TxnOwnRecord(&s->edit, &table, &record) : NULL;
        if (c == NULL) continue;
        OwnedTable* owned = TxnOwnedTable(c->index, table);
        if (owned != NULL && record >= owned->first) continue; /* made by an earlier line: its address followed */
        uintptr_t from = (uintptr_t)table + c->recordOffset + (uintptr_t)record * c->stride;
        uint8_t* copy = OwnRecord(state, c->index, c->stride, s->edit.resource, s->edit.resource);
        uint8_t* moved = StatePointer(state, REG_TABLES + 8 * c->index);
        if (copy == NULL) { s->at = 0; continue; } /* (checked: never) refused as the boot refuses it */
        if (moved != table) TxnOwnMoved(c, table, moved);
        from = moved != table ? (uintptr_t)moved + (from - (uintptr_t)table) : from;
        TxnOwnFollow(from, from + c->stride, (uintptr_t)copy);
        Note("live mod change %s: %016llx has a record of its own in component %u (%s)", g_txn.id, (unsigned long long)s->edit.resource, c->index,
            moved != table ? "its table copied first" : "HellForge's copy");
    }
}
