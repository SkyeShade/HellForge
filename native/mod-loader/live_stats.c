/* The stats layer of a live change (live_mods.c): stat lines (a mod's hellforge-stats.json and componentWrites) added,
 * removed and replaced while the game runs, each mod keeping its place in load order. Included by live_mods.c.
 *
 * A field holds, bottom up: what it held before any stats mod, the stats mods' values in load order (the boot's and live
 * ones alike), code mods' deferred edits, then live edits by rank (fields.c). A change writes the new top of the stats
 * layers only where nothing above holds the field: under a deferred edit nothing is written (the code mod's value stays,
 * as at boot); under live edits the lowest one's original (what its reset puts back) becomes the new top. A removed
 * mod's entries are retired in place (no kind, an unloaded stand-in mod), so every pass and reader skips them, and a
 * later change reuses their slots; nothing in g_stats moves, since other threads read it. A line the session cannot
 * place yet (its kind's boot pass has not run) refuses the change: the boot pass would apply it out of load order. A
 * line naming another row (a payload swap, refs.c) waits for that row's packages before the change is made, as the
 * boot pass's does. */

/* A table or settings kind's field: 0 with *later when its boot pass has not run yet, else the field or 0 with *why. */
static uintptr_t TxnTableAddress(StatEdit* e, int* later, const char** why)
{
    int phase = !strcmp(e->kind, "ProjectileSettings") ? 0 : !strcmp(e->kind, "DamageInfo") ? 1 : !strcmp(e->kind, "StratagemDefinition") ? 2 : -1;
    int extra = StatsExtraKind(e->kind);
    uintptr_t row = 0;
    if (phase >= 0) { if (!g_statsTablesDone[phase]) { *later = 1; return 0; } return StatsTableField(phase, ApiModuleBase("game.dll", NULL), e, why); }
    if (!strcmp(e->kind, "BoosterSettings")) { if (!g_boostersDone) { *later = 1; return 0; } return BoosterField(e, BoosterTable(), why); }
    if (!strcmp(e->kind, "MeleeHitVolume")) { if (!g_meleeDone) { *later = 1; return 0; } return MeleeField(e, why); }
    if (DirectorStride(e->kind)) { if (!g_directorDone) { *later = 1; return 0; } return DirectorField(e, &row, why); }
    if (!strncmp(e->kind, "Passive", 7))
    {
        if (!g_perksDone) { *later = 1; return 0; }
        uintptr_t at = PerkField(e);
        if (!at) *why = "the armor perk's entry is not as reviewed";
        return at;
    }
    if (extra >= 0)
    {
        if (!g_statsExtraDone[extra]) { *later = 1; return 0; }
        uintptr_t at = strcmp(g_build, "25480438") ? 0 : StatsExtraRow(e);
        if (!at) *why = "settings allocation or delta ownership/layout is not uniquely as reviewed";
        return at;
    }
    *why = "not a kind of stats HellForge edits";
    return 0;
}

/* Where a stat line applies now: its field, 0 with *why (refused, as the boot pass refuses it), or 0 with *later when
 * its kind's boot pass has not run yet. Reads only; called with g_statsLock held. */
static uintptr_t TxnStatAddress(StatEdit* e, int* later, const char** why)
{
    *later = 0;
    *why = NULL;
    if (strcmp(e->kind, "component")) return TxnTableAddress(e, later, why);
    if (!g_statsComponentsReady) { *later = 1; return 0; }
    int expect = e->hasExpect, own = e->own;
    e->hasExpect = 0; /* checked against the bottom of the field's layers instead (TxnBottom) */
    e->own = 0;       /* the record it has now: an own line's copy is made once the change is checked (live_own.c) */
    uintptr_t at = strcmp(g_build, "25480438") ? 0 : StatsComponent(e, g_liveEntityState());
    e->hasExpect = expect;
    e->own = own;
    if (!at) *why = "component layout or ownership differs from the reviewed record";
    return at;
}

/* The edits above the stats layers at a field: whether a code mod's deferred edit holds any of its bytes (the lowest one
 * of exactly this field in *under, else NULL), and the lowest live edit of exactly this field (NULL none). Called with
 * g_fieldsLock held. */
static FieldEdit* TxnAboveUnder(uintptr_t at, uint32_t size, int* deferred, FieldEdit** under)
{
    *deferred = 0;
    *under = NULL;
    FieldEdit* lowest = NULL;
    for (int i = 0; i < g_editCount; i++)
    {
        FieldEdit* e = &g_edits[i];
        if (e->failed || e->waiting || e->address == 0 || e->kind == EDIT_TAG) continue;
        if (e->address >= at + size || e->address + e->field->size <= at) continue;
        int exact = e->address == at && e->field->size == size;
        if (!e->live) { *deferred = 1; if (*under == NULL && exact) *under = e; }
        else if (lowest == NULL && exact) lowest = e;
    }
    return lowest;
}

static FieldEdit* TxnAbove(uintptr_t at, uint32_t size, int* deferred)
{
    FieldEdit* under;
    return TxnAboveUnder(at, size, deferred, &under);
}

static TxnField* TxnFieldAt(uintptr_t at, uint32_t size)
{
    for (int i = 0; i < g_txn.fieldCount; i++) if (g_txn.fields[i].at == at && g_txn.fields[i].size == size) return &g_txn.fields[i];
    return NULL;
}

/* What the field held before any stats mod: an applied stat entry's original (each keeps the bottom: StatsWrite chains
 * them), else what is under the edits above the stats (the lowest live edit's original, a live attachment change's
 * before), else the field now. 0 when a deferred edit holds it with no stats entry under it. Both locks held. */
static int TxnBottom(uintptr_t at, uint32_t size, uint32_t* bottom)
{
    *bottom = 0;
    const TxnField* touched = TxnFieldAt(at, size);
    if (touched != NULL) { *bottom = touched->bottom; return touched->known; }
    for (int i = 0; i < g_statCount; i++)
        if (g_stats[i].applied && g_stats[i].address == at && StatSize(&g_stats[i]) == size) { *bottom = g_stats[i].original; return 1; }
    int deferred = 0;
    const FieldEdit* live = TxnAbove(at, size, &deferred);
    if (deferred) return 0;
    if (live != NULL && live->hasOriginal) { memcpy(bottom, live->original, size); return 1; }
    if (LiveExtraBefore(at, bottom)) return 1;
    return !SafeRead(at, bottom, size);
}

/* Notes a field the change touches (before anything changes at it). 0 when the list is full. */
static int TxnTouch(uintptr_t at, uint32_t size, const StatEdit* e)
{
    if (TxnFieldAt(at, size) != NULL) return 1;
    uint32_t bottom = 0;
    int known = TxnBottom(at, size, &bottom);
    if (g_txn.fieldCount == g_txn.fieldCapacity)
    {
        int capacity = g_txn.fieldCapacity ? g_txn.fieldCapacity * 2 : 256;
        TxnField* grown = capacity <= MAX_STATS * 2 ? (TxnField*)realloc(g_txn.fields, (size_t)capacity * sizeof *grown) : NULL;
        if (grown == NULL) return 0;
        g_txn.fields = grown;
        g_txn.fieldCapacity = capacity;
    }
    uintptr_t row = !strcmp(e->kind, "DifficultySettings") ? at - e->offset : 0;
    g_txn.fields[g_txn.fieldCount++] = (TxnField){at, size, bottom, known, row};
    return 1;
}

/* The top of a field's stats layers: the last in load order (a later line of a mod over an earlier one), else -1. */
static int TxnTopStat(const TxnField* f)
{
    int best = -1, bestOrder = -1;
    for (int i = 0; i < g_statCount; i++)
    {
        const StatEdit* s = &g_stats[i];
        if (!s->applied || s->address != f->at || StatSize(s) != f->size) continue;
        int order = ModLoadIndex(s->mod);
        if (order >= bestOrder) { best = i; bestOrder = order; }
    }
    return best;
}

/* A live attachment change at the field (live.c LiveExtraWrite) above the stats: its "before" takes the new top, and
 * it keeps the field. 1 when one holds it. */
static int TxnUnderExtra(const TxnField* f, uint32_t top)
{
    for (int i = 0; i < g_liveExtraCount; i++)
    {
        LiveExtra* x = &g_liveExtra[i];
        if (x->address != f->at) continue;
        x->before = top;
        for (int r = RANK_CODE; r <= RANK_EDITOR; r++) if (x->layer[r].held) return 1;
    }
    return 0;
}

/* The field's stats layers changed: their top goes where nothing above holds the field. Both locks held; 0 when the
 * write did not hold. */
static int TxnLayerApply(const TxnField* f)
{
    uint32_t top = f->bottom;
    int best = TxnTopStat(f);
    if (best >= 0) top = g_stats[best].value;
    else if (!f->known) return 1; /* under a deferred edit with no stats left: nothing to write, nothing known to restore */
    int deferred = 0;
    FieldEdit* under = NULL;
    FieldEdit* live = TxnAboveUnder(f->at, f->size, &deferred, &under);
    /* under a code mod's deferred edit: its value stays, and the new top is what taking it out puts back */
    if (deferred) { if (under != NULL && under->hasBefore) memcpy(under->before, &top, f->size); return 1; }
    if (live != NULL) { if (live->hasOriginal) memcpy(live->original, &top, f->size); return 1; }
    if (TxnUnderExtra(f, top)) return 1;
    uint32_t now = 0, check = 0;
    if (SafeRead(f->at, &now, f->size)) return 0;
    if (!memcmp(&now, &top, f->size)) return 1;
    if (ApiWrite(f->at, &top, f->size) || SafeRead(f->at, &check, f->size) || memcmp(&check, &top, f->size)) return 0;
    /* a difficulty row of the game's own table: every copy that held the same bytes follows (stats-tables.c) */
    if (f->difficultyRow) DifficultyMirror(f->difficultyRow, (uint32_t)(f->at - f->difficultyRow), &now, &top, f->size);
    return 1;
}

/* A free slot for a stat entry: a retired one, else the next; -1 when g_stats is full. */
static int TxnStatSlot(int* from)
{
    for (; *from < g_statCount; (*from)++)
        if (g_stats[*from].mod == &g_retiredMod) return (*from)++;
    return g_statCount < MAX_STATS ? g_statCount : -1;
}

static int TxnFreeSlots(void)
{
    int free = MAX_STATS - g_statCount;
    for (int i = 0; i < g_statCount; i++) free += g_stats[i].mod == &g_retiredMod;
    return free;
}

static void TxnRetire(StatEdit* e)
{
    e->applied = 0;
    e->refWaiting = 0;
    e->kind[0] = 0;
    MemoryBarrier();
    e->mod = &g_retiredMod;
}

/* The change's lines that name another row (payload swaps): each waits until the row's packages are resident (they are
 * asked for as the boot pass's are), at most 30 s. NULL, or "<reason>\t<why>". */
static const char* TxnStatsPrepare(char* why, size_t size)
{
    ULONGLONG deadline = GetTickCount64() + 30000;
    for (int i = 0; i < g_txn.statCount; i++)
    {
        TxnStat* s = &g_txn.stats[i];
        for (;;)
        {
            AcquireSRWLockExclusive(&g_statsLock);
            int later = 0, state = REF_OK;
            const char* refused = NULL;
            StatEdit e = s->edit;
            uintptr_t at = TxnStatAddress(&e, &later, &refused);
            uint32_t bottom = 0;
            if (at && !SafeRead(at, &bottom, StatSize(&e)))
            {
                if (e.ref < 0) e.ref = StatRefKind(&e);
                char reason[256];
                if (e.ref != REF_NONE) state = RefCheck((uint32_t)e.ref, e.value, bottom, !strcmp(e.kind, "ProjectileSettings") ? (uint64_t)e.record : 0, reason, sizeof reason);
            }
            ReleaseSRWLockExclusive(&g_statsLock);
            if (state != REF_WAIT) break;
            if (GetTickCount64() > deadline)
            {
                snprintf(why, size, "packages\t%s: the packages a value of it names did not load within 30 s", g_txn.mods[s->mod].id);
                return why;
            }
            Sleep(100);
        }
    }
    return NULL;
}

/* Under both locks, before anything changes: room for every line, every line placeable now, every own line's copy. */
static const char* TxnStatsCheck(char* why, size_t size)
{
    if (g_txn.statCount > TxnFreeSlots()) { snprintf(why, size, "capacity\tthe session holds %d stats edits at most", MAX_STATS); return why; }
    for (int i = 0; i < g_txn.statCount; i++)
    {
        TxnStat* s = &g_txn.stats[i];
        int later = 0;
        const char* refused = NULL;
        s->at = TxnStatAddress(&s->edit, &later, &refused);
        if (later)
        {
            snprintf(why, size, "state\t%s: the game has not loaded its %s values yet; try again in a moment", g_txn.mods[s->mod].id, s->edit.kind);
            return why;
        }
    }
    return TxnOwnCheck(why, size);
}

/* Every field the change touches, with its bottom, before anything at it changes. */
static void TxnTouchFields(void)
{
    for (int i = 0; i < g_statCount; i++)
    {
        StatEdit* e = &g_stats[i];
        const TxnMod* t = e->mod != NULL ? TxnFind(e->mod->id) : NULL;
        if (t != NULL && t->action != TXN_ADD && e->mod == t->old && e->applied) TxnTouch(e->address, StatSize(e), e);
    }
    for (int i = 0; i < g_txn.statCount; i++) if (g_txn.stats[i].at) TxnTouch(g_txn.stats[i].at, StatSize(&g_txn.stats[i].edit), &g_txn.stats[i].edit);
}

/* Why a line cannot take its field (as the boot pass would refuse it), or NULL; *logged when RefStat said why itself. */
static const char* TxnStatRefused(StatEdit* e, uintptr_t at, uint32_t* bottom, int* logged)
{
    *logged = 0;
    int known = at ? TxnBottom(at, StatSize(e), bottom) : 0;
    if (!at) return "its field was not found";
    if (e->hasExpect && !known) return "a code mod's edit holds the field, so its original value is not known";
    if (e->hasExpect && *bottom != e->expect) return "original value differs from expect";
    if (e->floating) { float v; memcpy(&v, bottom, 4); if (!isfinite(v)) return "current float is not finite"; }
    int ref = RefStat(e, at, *bottom);
    e->refWaiting = 0; /* a waiting reference would be written later out of load order (StatsRefsTick) */
    if (ref == REF_WAIT) return "it names a row whose packages are not loaded; change it again once they are";
    if (ref == REF_REFUSED) { *logged = 1; return "its reference was refused"; }
    return NULL;
}

/* The added and replaced lines take slots (retired ones first): applied, or kept refused as the boot keeps them. */
static void TxnPlaceStats(void)
{
    int from = 0;
    for (int i = 0; i < g_txn.statCount; i++)
    {
        TxnStat* s = &g_txn.stats[i];
        TxnMod* t = &g_txn.mods[s->mod];
        StatEdit e = s->edit;
        e.mod = t->mod;
        int slot = TxnStatSlot(&from);
        if (slot < 0 || e.mod == NULL) { t->refused++; continue; } /* (checked: never) */
        uint32_t bottom = 0;
        int logged = 0;
        const char* why = TxnStatRefused(&e, s->at, &bottom, &logged);
        if (why == NULL) { e.address = s->at; e.original = bottom; e.hasBaseline = 1; }
        e.applied = 0;
        g_stats[slot] = e;
        MemoryBarrier();
        if (slot == g_statCount) g_statCount++;
        if (why != NULL) { if (!logged) StatsRefuse(&g_stats[slot], why); t->refused++; continue; }
        g_stats[slot].applied = 1;
        if (e.shared) ApiLog(e.mod, "stats %s: shared record; all consumers are affected", e.label);
        if (e.unverified) ApiLog(e.mod, "stats %s: gameplay effect is unverified", e.label);
        t->applied++;
    }
}

/* The stats part of the change, once checked: removals and replacements retire their entries (the old version's), the
 * added lines take slots (the new version's), then each touched field gets its new top. Both locks held; after
 * TxnApplyMods. */
static void TxnStatsApply(void)
{
    TxnOwnApply();
    TxnTouchFields();
    for (int i = 0; i < g_txn.modCount; i++)
    {
        const TxnMod* t = &g_txn.mods[i];
        if (t->action == TXN_ADD || t->old == NULL) continue;
        for (int k = 0; k < g_statCount; k++) if (g_stats[k].mod == t->old) TxnRetire(&g_stats[k]);
    }
    TxnPlaceStats();
    for (int i = 0; i < g_txn.fieldCount; i++)
        if (!TxnLayerApply(&g_txn.fields[i])) Note("live mod change %s: the write at 0x%llx did not hold", g_txn.id, (unsigned long long)g_txn.fields[i].at);
    for (int i = 0; i < g_statCount; i++)
    {
        const StatEdit* e = &g_stats[i];
        const TxnMod* t = e->applied && e->mod != NULL ? TxnFind(e->mod->id) : NULL;
        if (t == NULL || t->action == TXN_REMOVE) continue;
        uint32_t now = 0;
        SafeRead(e->address, &now, StatSize(e));
        ApiLog(e->mod, "stats %s: applied live (%08x -> %08x)%s", e->label, e->original, e->value, now == e->value ? "" : " under another mod's value");
    }
}
