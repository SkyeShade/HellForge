/* Live edits (set_live, get_live; fields.c's header has how they stack and what they hold): the same edits as fields.c's
 * deferred ones, applied at once, kept in rank order per field and undone by a reset. Included by hellforge_modloader.c
 * right after fields.c. */

static void RemoveEdit(int index)
{
    memmove(&g_edits[index], &g_edits[index + 1], (size_t)(g_editCount - index - 1) * sizeof g_edits[0]);
    g_editCount--;
}

/* A live edit's value goes back to what it was before it (when it was applied in this load and nothing changed it since). */
static void RestoreLive(FieldEdit* e)
{
    int waiting = 0;
    uint8_t* record = e->hasOriginal ? EditRecord(e, e->entity, 0, &waiting) : NULL;
    if (record == NULL) return;
    uint8_t* at = record + e->field->offset;
    if (memcmp(at, e->value, e->field->size) == 0) ApiWrite((uintptr_t)at, e->original, e->field->size);
}

/* The field's bytes as set_live finds them (a shared record too), or NULL. */
static uint8_t* LiveAddress(HfMod* mod, uint64_t target, const SdkComponent* c, const SdkField* f)
{
    FieldEdit probe = {mod, EDIT_SET, target, 0, c, f};
    probe.table = c->table;
    probe.shared = 1;
    int waiting = 0;
    uint8_t* record = EditRecord(&probe, target, 0, &waiting);
    return record != NULL ? record + f->offset : NULL;
}

/* The field's value before every live edit and the Local Stats mod's stats edit (now, when there are none). */
static uint32_t BaseBeforeEdits(uint64_t target, const SdkComponent* c, const SdkField* f, uintptr_t at, uint32_t now)
{
    uint32_t base = now;
    for (int i = 0; i < g_editCount; i++) /* the first live edit's original: the value from before every live edit */
    {
        const FieldEdit* e = &g_edits[i];
        if (e->live && e->hasOriginal && e->entity == target && e->component == c && e->field->offset == f->offset && e->field->size == f->size)
        { base = 0; memcpy(&base, e->original, f->size); break; }
    }
    for (int i = 0; i < g_statCount; i++)
        if (g_stats[i].applied && g_stats[i].address == at && IsLocalStats(g_stats[i].mod)) { base = g_stats[i].original; break; }
    return base;
}

/* The other mod whose stats edit or deferred edit (the last) holds the field, or NULL. */
static const char* OtherHolder(uint64_t target, const SdkComponent* c, const SdkField* f, uintptr_t at)
{
    const char* holder = NULL;
    for (int i = g_statCount - 1; i >= 0 && holder == NULL; i--)
        if (g_stats[i].applied && g_stats[i].address == at && g_stats[i].mod != NULL && !IsLocalStats(g_stats[i].mod)) holder = g_stats[i].mod->id;
    for (int i = g_editCount - 1; i >= 0 && holder == NULL; i--)
    {
        const FieldEdit* e = &g_edits[i];
        /* its bytes, or an edit of the whole array it is an element of (a copied fire_rate) */
        if (!e->live && !e->failed && !e->waiting && e->entity == target && e->component == c && e->field->offset <= f->offset &&
            f->offset + f->size <= e->field->offset + e->field->size) holder = e->mod->id;
    }
    return holder;
}

/* A field's base (get_live, HF_LIVE_BASE): without the Local Stats mod's stats edit and without live edits, so another
 * mod's value, else the game's. *holder: the other mod whose value it is, or NULL. */
static uint32_t FieldBase(uint64_t target, const SdkComponent* c, const SdkField* f, uintptr_t at, uint32_t now, const char** holder)
{
    uint32_t base = BaseBeforeEdits(target, c, f, at, now);
    *holder = OtherHolder(target, c, f, at);
    /* a Lua or code mod's live edit is another mod's value as well (the top one; the editors' own are not) */
    for (int i = g_editCount - 1; i >= 0; i--)
    {
        const FieldEdit* e = &g_edits[i];
        if (e->live && !e->failed && !e->waiting && ModRank(e->mod) < RANK_EDITOR && e->entity == target && e->component == c &&
            e->field->offset == f->offset && e->field->size == f->size)
        {
            base = 0;
            memcpy(&base, e->value, f->size);
            *holder = e->mod->id;
            break;
        }
    }
    return base;
}

/* The Lua or code mod whose live edit is the value at `at` (get_stat's holder), its value in *value; NULL none. */
static const char* LiveHolderAt(uintptr_t at, uint32_t size, uint32_t* value)
{
    const char* holder = NULL;
    AcquireSRWLockShared(&g_fieldsLock);
    for (int i = g_editCount - 1; i >= 0 && holder == NULL; i--)
    {
        const FieldEdit* e = &g_edits[i];
        if (e->live && !e->failed && !e->waiting && e->address == at && e->field->size == size && ModRank(e->mod) < RANK_EDITOR)
        {
            *value = 0;
            memcpy(value, e->value, size);
            holder = e->mod->id;
        }
    }
    ReleaseSRWLockShared(&g_fieldsLock);
    return holder;
}

/* HF_LIVE_BASE: the field back to its base, so a change saved into Local Stats and applied at load can be undone in the
 * running game too (another mod's value stays). */
static void RestoreBase(HfMod* mod, uint64_t target, const SdkComponent* c, const SdkField* f)
{
    uint8_t* at = f->size == 4 || f->size == 1 ? LiveAddress(mod, target, c, f) : NULL;
    uint32_t now = 0;
    if (at == NULL || SafeRead((uintptr_t)at, &now, f->size)) return;
    const char* holder;
    uint32_t base = FieldBase(target, c, f, (uintptr_t)at, now, &holder);
    if (base == now) return;
    ApiWrite((uintptr_t)at, &base, f->size);
    ApiLog(mod, "set_live: %s.%s of %016llx back to its base %08x (%s)", c->name, f->name, (unsigned long long)target, base,
        holder != NULL ? holder : "the game's own");
}

static void ValueOf(uint32_t type, uint32_t bits, HfValue* out)
{
    out->type = type;
    out->u64 = 0;
    if (type == HF_TYPE_F32) { float v; memcpy(&v, &bits, 4); out->number = v; }
    else if (type == HF_TYPE_I32) out->number = (int32_t)bits;
    else out->number = bits;
}

/* SDK 6, get_live: a field's value now and its base. */
static int ApiGetLive(HfMod* mod, HfRef target, const HfField* field, HfLiveValue* out)
{
    if (out == NULL) return 0;
    memset(out, 0, sizeof *out);
    const SdkComponent* c = NULL;
    const SdkField* f = mod != NULL ? ResolveField(mod, "get_live", field, &c) : NULL;
    if (f == NULL || (f->size != 4 && f->size != 1)) return 0; /* a single value: four bytes, or a one-byte flag */
    AcquireSRWLockExclusive(&g_fieldsLock);
    uint8_t* at = LiveAddress(mod, target, c, f);
    uint32_t now = 0;
    int result = 0;
    if (at != NULL && !SafeRead((uintptr_t)at, &now, f->size))
    {
        uint32_t base = FieldBase(target, c, f, (uintptr_t)at, now, &out->mod);
        ValueOf(f->type, now, &out->now);
        ValueOf(f->type, base, &out->base);
        result = (int)f->applies;
    }
    ReleaseSRWLockExclusive(&g_fieldsLock);
    return result;
}

static int SameField(const FieldEdit* a, const FieldEdit* b)
{
    return a->entity == b->entity && a->component == b->component && a->field->offset == b->field->offset && a->field->size == b->field->size;
}

/* The live edit of the same field right above edit `index` (the next one in the list: the list keeps a field's live
 * edits in rank order), or -1 when it is the top one. */
static int LiveAbove(int index)
{
    for (int i = index + 1; i < g_editCount; i++) if (g_edits[i].live && SameField(&g_edits[i], &g_edits[index])) return i;
    return -1;
}

/* Where a new live edit goes: under the first live edit of the field with a higher rank, else last. */
static int LiveInsertAt(const FieldEdit* e)
{
    int rank = ModRank(e->mod);
    for (int i = 0; i < g_editCount; i++) if (g_edits[i].live && SameField(&g_edits[i], e) && ModRank(g_edits[i].mod) > rank) return i;
    return g_editCount;
}

/* Edit `index` (applied, or kept under another) leaves the field: the one above it takes its original, else the record
 * gets it back. */
static void LiveLeave(int index)
{
    int above = LiveAbove(index);
    if (above < 0) { RestoreLive(&g_edits[index]); return; }
    if (g_edits[index].hasOriginal && g_edits[above].hasOriginal) memcpy(g_edits[above].original, g_edits[index].original, sizeof g_edits[above].original);
}

/* The mod's live edit of the field, or -1. */
static int LiveEditOf(const HfMod* mod, uint64_t target, const SdkComponent* c, const SdkField* f)
{
    int index = -1;
    for (int i = 0; i < g_editCount; i++)
        if (g_edits[i].live && g_edits[i].mod == mod && g_edits[i].entity == target && g_edits[i].component == c &&
            g_edits[i].field->offset == f->offset && g_edits[i].field->size == f->size) index = i;
    return index;
}

/* Edit `index` under a higher-ranked one (`above`): kept, its value what the one above goes back to. */
static void KeepUnder(int index, int above)
{
    FieldEdit* q = &g_edits[index];
    FieldEdit* top = &g_edits[above];
    if (top->hasOriginal)
    {
        if (!q->hasOriginal) { memcpy(q->original, top->original, sizeof q->original); q->hasOriginal = 1; }
        memcpy(top->original, q->value, sizeof top->original);
    }
    q->failed = 0;
}

/* Edit `index`, the top one of its field, applied (its table row, or once the entity data is in): 1 when it applies.
 * Refused: the mod's earlier live edit of the field stays (its value is still the game's, its original still what a
 * reset puts back). */
static int ApplyLive(int index, int replaced, const FieldEdit* previous)
{
    FieldEdit* q = &g_edits[index];
    if (q->table || g_fired[HF_EVENT_ENTITIES_READY]) ApplyEdit(q);
    if (q->failed && replaced) *q = *previous;
    else if (q->failed) RemoveEdit(index);
    else return 1;
    return 0;
}

/* Live edit e into the list: in place of the mod's earlier one of the field (`index`), else under the first higher-ranked
 * one; then kept under the one above it or applied. 1 when the change applies. */
static int PlaceLiveEdit(HfMod* mod, FieldEdit* e, int index)
{
    int replaced = index >= 0;
    FieldEdit previous = replaced ? g_edits[index] : *e;
    if (replaced)
    {
        e->hasOriginal = g_edits[index].hasOriginal;
        memcpy(e->original, g_edits[index].original, sizeof e->original);
        g_edits[index] = *e;
    }
    else if (EditRoom())
    {
        ReportConflicts(e, g_editCount);
        index = LiveInsertAt(e);
        memmove(&g_edits[index + 1], &g_edits[index], (size_t)(g_editCount - index) * sizeof g_edits[0]);
        g_edits[index] = *e;
        g_editCount++;
    }
    else ApiLog(mod, "set_live: mods queued %d edits already", FIELD_MAX_EDITS);
    int above = index >= 0 ? LiveAbove(index) : -1;
    if (above >= 0)
    {
        KeepUnder(index, above);
        return 1;
    }
    return index >= 0 && ApplyLive(index, replaced, &previous);
}

/* Changes a field now (SDK 6, set_live): the same deferred edit as set_field, applied at once and kept for later loads.
 * value NULL undoes the mod's live edit of the field. Returns when the change applies (HF_APPLIES_...), or 0 refused.
 * An edit under a higher-ranked one (an editor's over a Lua mod's) is kept, not written: it becomes the value the one
 * above it goes back to. */
static int ApiSetLive(HfMod* mod, HfRef target, const HfField* field, const HfValue* value, uint32_t flags)
{
    const SdkComponent* c = NULL;
    const SdkField* f = mod != NULL ? ResolveField(mod, "set_live", field, &c) : NULL;
    if (f == NULL) return 0;
    AcquireSRWLockExclusive(&g_fieldsLock);
    int index = LiveEditOf(mod, target, c, f), result = 0;
    if (value == NULL)
    {
        if (index >= 0) { LiveLeave(index); RemoveEdit(index); }
        if (flags & HF_LIVE_BASE) RestoreBase(mod, target, c, f);
        result = (int)f->applies;
    }
    else
    {
        FieldEdit e = {mod, EDIT_SET, target, 0, c, f};
        int kind = -1;
        if (!ValueBytes(mod, "set_live", field->line, c, f, value, e.value) && (kind = EditTarget(mod, "set_live", target, c)) >= 0)
        {
            e.line = field->line;
            e.vanilla = kind == 1;
            e.table = kind == 2;
            e.live = 1;
            e.shared = (flags & HF_LIVE_SHARED) != 0;
            if (PlaceLiveEdit(mod, &e, index)) result = (int)f->applies;
        }
    }
    ReleaseSRWLockExclusive(&g_fieldsLock);
    return result;
}
