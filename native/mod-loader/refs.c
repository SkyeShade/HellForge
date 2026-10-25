/* References between settings rows (HD2R Editor's payload swaps; mappings "ref"): the projectile a weapon fires
 * (ProjectileWeapon.projectile, or for six weapons their default ammunition's entity delta), a projectile's impact and
 * expiry explosions, the status effects a damage type applies, the damage an explosion or a projectile deals. Data only:
 * the game reads the row a reference names like any other. Included by hellforge_modloader.c after fields.c.
 *
 * A new value must be a row of its table that HellForge's catalogue knows (reference_data.h, from HD2Runtime's reviewed
 * catalogues): a projectile type another weapon fires (a mod's own type counts as its base), a catalogued explosion
 * type or 0 (none), an attachable status effect or 0 (an empty slot), a damage id. It is written once the packages its
 * effects need are resident (packages.c); until then the edit waits, and it fails if they do not load. A value back at
 * the field's original needs no check. HD2R Editor's guards: an explosion whose shrapnel leads back to the projectile
 * it is set on is refused (a shot that spawns itself); a status in a slot after an empty one is written but logged (the
 * game stops at the first empty slot, so it has no effect). */

#define REF_SHRAPNEL_COUNT 80u      /* ExplosionSettings */
#define REF_SHRAPNEL_PROJECTILE 84u
#define REF_IMPACT 144u             /* ProjectileSettings */
#define REF_EXPIRY 156u
#define REF_STATUS_FIRST 44u        /* DamageInfo: four slots of {type, strength} */
#define REF_STATUS_SLOTS 4u

static const char* const kRefNames[] = {"", "projectile", "explosion", "status effect", "damage", "mounted weapon", "pickup"};

/* A vehicle's mount slot (HD2R Editor's arm swap; MountComponentData slot k +24k: the mounted entity's resource): the
 * value must be a weapon HD2Runtime allows in that slot (its vanilla occupant first), written once its package is
 * resident. A value back at the field's original needs no check. */
static int RefMountCheck(uint64_t vehicle, uint32_t slot, uint64_t value, uint64_t original, char* why, size_t whySize)
{
    if (value == original) return REF_OK;
    const RefMountSlot* s = NULL;
    for (size_t i = 0; i < sizeof kRefMountSlots / sizeof kRefMountSlots[0]; i++)
        if (kRefMountSlots[i].vehicle == vehicle && kRefMountSlots[i].slot == slot) s = &kRefMountSlots[i];
    if (s == NULL) { snprintf(why, whySize, "slot %u of %016llx is not a mount slot HellForge's catalogue knows", slot, (unsigned long long)vehicle); return REF_REFUSED; }
    const RefMount* m = NULL;
    for (uint16_t i = 0; i < s->count; i++) if (kRefMounts[kRefMountAllowed[s->first + i]].resource == value) m = &kRefMounts[kRefMountAllowed[s->first + i]];
    if (m == NULL) { snprintf(why, whySize, "%016llx is not a weapon this slot can hold", (unsigned long long)value); return REF_REFUSED; }
    if (original != kRefMounts[kRefMountAllowed[s->first]].resource && original != value)
    {
        int ours = 0;
        for (uint16_t i = 0; i < s->count; i++) if (kRefMounts[kRefMountAllowed[s->first + i]].resource == original) ours = 1;
        if (!ours) { snprintf(why, whySize, "the slot holds %016llx, not its vanilla weapon", (unsigned long long)original); return REF_REFUSED; }
    }
    if (m->package == 0) return REF_OK;
    const char *name, *failed;
    int state = PackageWant(kRefPackages[m->package - 1].id, &name, &failed);
    if (state == PKG_FAILED) { snprintf(why, whySize, "the weapon needs package %s, which did not load: %s", name, failed); return REF_REFUSED; }
    if (state == PKG_WAITING) { snprintf(why, whySize, "waits for package %s (mounted weapon)", name); return REF_WAIT; }
    return REF_OK;
}

/* A call-in's hellpod slot (HD2R Editor's pod payload; HellpodRackComponentData slot k +64k: the item's entity
 * resource): a slot 0-3 of a rack HD2Runtime reviewed, holding a reviewed pickup (a support weapon, a backpack, ammo,
 * stims, grenades, supplies) or nothing (0), written once the pickup's package is resident. Only the slot's vanilla item
 * or a pickup HellForge put there is replaced. A value back at the field's original needs no check. */
static int RefPickupCheck(uint64_t rack, uint32_t slot, uint64_t value, uint64_t original, char* why, size_t whySize)
{
    if (value == original) return REF_OK;
    const RefPodSlot* s = NULL;
    for (size_t i = 0; i < sizeof kRefPodSlots / sizeof kRefPodSlots[0]; i++)
        if (kRefPodSlots[i].rack == rack && kRefPodSlots[i].slot == slot) s = &kRefPodSlots[i];
    if (s == NULL) { snprintf(why, whySize, "slot %u of %016llx is not a hellpod slot HellForge's catalogue knows", slot, (unsigned long long)rack); return REF_REFUSED; }
    const RefPickup* p = NULL;
    for (size_t i = 0; i < sizeof kRefPickups / sizeof kRefPickups[0]; i++) if (kRefPickups[i].resource == value) p = &kRefPickups[i];
    if (value != 0 && p == NULL) { snprintf(why, whySize, "%016llx is not a pickup a hellpod can deliver", (unsigned long long)value); return REF_REFUSED; }
    if (original != s->vanilla && original != 0)
    {
        int ours = 0;
        for (size_t i = 0; i < sizeof kRefPickups / sizeof kRefPickups[0]; i++) if (kRefPickups[i].resource == original) ours = 1;
        if (!ours) { snprintf(why, whySize, "the slot holds %016llx, not its vanilla item", (unsigned long long)original); return REF_REFUSED; }
    }
    if (p == NULL || p->package == 0) return REF_OK;
    const char *name, *failed;
    int state = PackageWant(kRefPackages[p->package - 1].id, &name, &failed);
    if (state == PKG_FAILED) { snprintf(why, whySize, "the pickup needs package %s, which did not load: %s", name, failed); return REF_REFUSED; }
    if (state == PKG_WAITING) { snprintf(why, whySize, "waits for package %s (pickup)", name); return REF_WAIT; }
    return REF_OK;
}

static const RefRow* RefRowOf(const RefRow* rows, size_t count, uint32_t type)
{
    for (size_t i = 0; i < count; i++) if (rows[i].type == type) return &rows[i];
    return NULL;
}

/* Whether the explosion (its shrapnel, their explosions...) spawns the projectile again. */
static int RefLoops(uint32_t explosion, uint64_t projectile, int depth)
{
    int ready;
    uintptr_t row = explosion != 0 && depth < 5 ? g_tableRow("ExplosionSettings", explosion, &ready) : 0;
    uint32_t count = 0, shrapnel = 0, impact = 0, expiry = 0;
    if (!row || SafeRead(row + REF_SHRAPNEL_COUNT, &count, 4) || SafeRead(row + REF_SHRAPNEL_PROJECTILE, &shrapnel, 4) || !count || !shrapnel) return 0;
    if (shrapnel == projectile) return 1;
    uintptr_t next = g_tableRow("ProjectileSettings", shrapnel, &ready);
    if (!next || SafeRead(next + REF_IMPACT, &impact, 4) || SafeRead(next + REF_EXPIRY, &expiry, 4)) return 0;
    return RefLoops(impact, projectile, depth + 1) || (expiry != impact && RefLoops(expiry, projectile, depth + 1));
}

/* Whether the value can be written now: REF_OK, REF_WAIT (its table or its packages are not ready yet; *why says
 * which) or REF_REFUSED (*why). self: the projectile type whose row the explosion is set on, else 0. */
static int RefCheck(uint32_t ref, uint32_t value, uint32_t original, uint64_t self, char* why, size_t whySize)
{
    if (ref == REF_NONE || value == original) return REF_OK;
    const char* kind = ref < sizeof kRefNames / sizeof kRefNames[0] ? kRefNames[ref] : "?";
    if (value == 0 && (ref == REF_EXPLOSION || ref == REF_STATUS)) return REF_OK;
    if (ref == REF_STATUS)
    {
        for (size_t i = 0; i < sizeof kRefStatuses / sizeof kRefStatuses[0]; i++) if (kRefStatuses[i] == value) return REF_OK;
        snprintf(why, whySize, "%u is not a status effect a damage type can apply", value);
        return REF_REFUSED;
    }
    const char* table = ref == REF_PROJECTILE ? "ProjectileSettings" : ref == REF_EXPLOSION ? "ExplosionSettings" : "DamageInfo";
    int ready = 1;
    if (!g_tableRow(table, value, &ready))
    {
        if (!ready) { snprintf(why, whySize, "waits for the %s table to load", kind); return REF_WAIT; }
        snprintf(why, whySize, "the game has no %s %u", kind, value);
        return REF_REFUSED;
    }
    if (ref == REF_DAMAGE) return REF_OK;
    const RefRow* row;
    if (ref == REF_PROJECTILE)
    {
        uint32_t type = value;
        for (uint32_t i = 0; i < g_projectileCount; i++) if (g_projectiles[i].type == value) type = g_projectiles[i].base;
        row = RefRowOf(kRefProjectiles, sizeof kRefProjectiles / sizeof kRefProjectiles[0], type);
    }
    else row = RefRowOf(kRefExplosions, sizeof kRefExplosions / sizeof kRefExplosions[0], value);
    if (row == NULL)
    {
        snprintf(why, whySize, "%s %u is not one HellForge's catalogue knows the packages of", kind, value);
        return REF_REFUSED;
    }
    if (ref == REF_EXPLOSION && self != 0 && RefLoops(value, self, 0))
    {
        snprintf(why, whySize, "explosion %u's shrapnel fires projectile %llu again (a shot that spawns itself)", value, (unsigned long long)self);
        return REF_REFUSED;
    }
    int waiting = 0;
    uint16_t packages[2] = {row->package, row->package2};
    for (int k = 0; k < 2; k++)
    {
        if (packages[k] == 0) continue;
        const char *name, *failed;
        int state = PackageWant(kRefPackages[packages[k] - 1].id, &name, &failed);
        if (state == PKG_FAILED) { snprintf(why, whySize, "%s %u needs package %s, which did not load: %s", kind, value, name, failed); return REF_REFUSED; }
        if (state == PKG_WAITING) { snprintf(why, whySize, "waits for package %s (%s %u)", name, kind, value); waiting = 1; }
    }
    return waiting ? REF_WAIT : REF_OK;
}

/* SDK 9, copy_row: a settings row's bytes (offset, size) replaced live by another row's of the same table (HD2R Editor's
 * G-109 Urchin pulse explosion: another explosion's whole settings over the Urchin's own row). Only rows HellForge's
 * catalogue names for it (kRefRowCopies) take it, from the donors listed there (kRefRowDonors: catalogued explosions),
 * once the donor's packages are resident (it waits for them, tried again each tick). from 0, or the row itself, puts
 * the row's own bytes back. HF_APPLIES_NOW, or 0 (the mod's log says why). */
#define ROW_COPY_MAX 16
typedef struct RowCopy { HfMod* mod; const RefRowCopy* spec; uint32_t from; int applied, waiting; uint8_t original[256]; int hasOriginal; } RowCopy;
static RowCopy g_rowCopies[ROW_COPY_MAX];
static int g_rowCopyCount;
static volatile LONG g_rowCopiesWaiting;
static SRWLOCK g_rowCopyLock = SRWLOCK_INIT;

static int RowCopyApply(RowCopy* c)
{
    int ready = 1;
    uintptr_t row = g_tableRow(c->spec->table, c->spec->row, &ready), donor = g_tableRow(c->spec->table, c->from, &ready);
    if (!row || !donor) { c->waiting = 1; return REF_WAIT; }
    char why[256];
    int state = RefCheck(REF_EXPLOSION, c->from, 0xFFFFFFFFu, 0, why, sizeof why);
    if (state == REF_WAIT) { if (!c->waiting) ApiLog(c->mod, "copy_row %s %u: %s", c->spec->table, c->spec->row, why); c->waiting = 1; return state; }
    c->waiting = 0;
    if (state == REF_REFUSED) { ApiLog(c->mod, "copy_row %s %u from %u refused: %s", c->spec->table, c->spec->row, c->from, why); return state; }
    uint8_t bytes[256];
    if (c->spec->size > sizeof bytes || SafeRead(donor + c->spec->offset, bytes, c->spec->size)) return REF_REFUSED;
    if (!c->hasOriginal) { if (SafeRead(row + c->spec->offset, c->original, c->spec->size)) return REF_REFUSED; c->hasOriginal = 1; }
    if (ApiWrite(row + c->spec->offset, bytes, c->spec->size)) return REF_REFUSED;
    c->applied = 1;
    ApiLog(c->mod, "copy_row %s %u: +%u..+%u from %s %u", c->spec->table, c->spec->row, c->spec->offset, c->spec->offset + c->spec->size, c->spec->table, c->from);
    return REF_OK;
}

static void RowCopyRestore(RowCopy* c)
{
    int ready = 1;
    uintptr_t row = c->hasOriginal ? g_tableRow(c->spec->table, c->spec->row, &ready) : 0;
    if (row && c->applied) ApiWrite(row + c->spec->offset, c->original, c->spec->size);
}

/* One catalogued row's copy from a donor (0: its own bytes back). REF_OK, REF_WAIT or REF_REFUSED. g_rowCopyLock held. */
static int RowCopyInto(HfMod* mod, const RefRowCopy* spec, uint32_t from)
{
    int index = -1;
    for (int i = 0; i < g_rowCopyCount; i++) if (g_rowCopies[i].spec == spec) index = i;
    if (from == 0)
    {
        if (index >= 0) { RowCopyRestore(&g_rowCopies[index]); g_rowCopies[index] = g_rowCopies[--g_rowCopyCount]; }
        return REF_OK;
    }
    if (index < 0)
    {
        if (g_rowCopyCount == ROW_COPY_MAX) { ApiLog(mod, "copy_row: too many copies"); return REF_REFUSED; }
        index = g_rowCopyCount++;
        memset(&g_rowCopies[index], 0, sizeof g_rowCopies[index]);
        g_rowCopies[index].spec = spec;
    }
    RowCopy* c = &g_rowCopies[index];
    c->mod = mod;
    c->from = from;
    int state = RowCopyApply(c);
    if (state == REF_WAIT) InterlockedExchange(&g_rowCopiesWaiting, 1);
    if (state == REF_REFUSED) { if (c->applied) RowCopyRestore(c); g_rowCopies[index] = g_rowCopies[--g_rowCopyCount]; }
    return state;
}

static int ApiCopyRow(HfMod* mod, const char* table, uint64_t row, uint64_t from, uint32_t offset, uint32_t size)
{
    if (mod == NULL || table == NULL) return 0;
    const RefRowCopy* spec = NULL;
    for (size_t i = 0; i < sizeof kRefRowCopies / sizeof kRefRowCopies[0]; i++)
        if (kRefRowCopies[i].table && !strcmp(kRefRowCopies[i].table, table) && kRefRowCopies[i].row == row &&
            kRefRowCopies[i].offset == offset && kRefRowCopies[i].size == size) spec = &kRefRowCopies[i];
    if (spec == NULL) { ApiLog(mod, "copy_row: %s %llu +%u (%u bytes) is not a row HellForge copies into", table, (unsigned long long)row, offset, size); return 0; }
    if (spec->with != 0) { ApiLog(mod, "copy_row: %s %llu follows row %u's copy (copy into that row)", table, (unsigned long long)row, spec->with); return 0; }
    int donor = from == 0 || from == row;
    for (uint16_t i = 0; i < spec->count && !donor; i++) if (kRefRowDonors[spec->first + i] == from) donor = 1;
    if (!donor) { ApiLog(mod, "copy_row: %s %llu does not take %llu", table, (unsigned long long)row, (unsigned long long)from); return 0; }
    uint32_t donorRow = from == row ? 0 : (uint32_t)from;
    AcquireSRWLockExclusive(&g_rowCopyLock);
    int state = RowCopyInto(mod, spec, donorRow);
    /* the rows that follow it (the Urchin's final discharge with its repeated pulse) take the same donor, or their own back */
    for (size_t i = 0; i < sizeof kRefRowCopies / sizeof kRefRowCopies[0]; i++)
    {
        const RefRowCopy* follower = &kRefRowCopies[i];
        if (follower->table == NULL || follower->with != spec->row || strcmp(follower->table, spec->table)) continue;
        int followed = RowCopyInto(mod, follower, state == REF_REFUSED ? 0 : donorRow);
        if (followed == REF_REFUSED && state != REF_REFUSED) { RowCopyInto(mod, spec, 0); state = REF_REFUSED; }
    }
    ReleaseSRWLockExclusive(&g_rowCopyLock);
    return state == REF_REFUSED ? 0 : HF_APPLIES_NOW;
}

/* From the watch loop: copies waiting for their table or the donor's packages, tried again. */
static void RowCopiesTick(void)
{
    AcquireSRWLockExclusive(&g_rowCopyLock);
    int waiting = 0;
    for (int i = 0; i < g_rowCopyCount; i++)
        if (g_rowCopies[i].waiting && RowCopyApply(&g_rowCopies[i]) == REF_WAIT) waiting = 1;
    InterlockedExchange(&g_rowCopiesWaiting, waiting);
    ReleaseSRWLockExclusive(&g_rowCopyLock);
}

/* A status slot set after an empty one (the game stops at the first empty slot). row: the DamageInfo row. */
static int RefStatusGap(uintptr_t row, uint32_t offset, uint32_t value)
{
    if (offset < REF_STATUS_FIRST || (offset - REF_STATUS_FIRST) % 8 || (offset - REF_STATUS_FIRST) / 8 >= REF_STATUS_SLOTS) return 0;
    uint32_t slot = (offset - REF_STATUS_FIRST) / 8, type = 0;
    for (uint32_t k = 0; k < REF_STATUS_SLOTS; k++)
    {
        uint32_t current = k == slot ? value : 0;
        if (k != slot && SafeRead(row + REF_STATUS_FIRST + k * 8, &current, 4)) return 0;
        if (current && k > 0 && !type) return 1;
        type = current;
    }
    return 0;
}

/* A field edit's reference (ApplyEdit): REF_OK to write, else waiting or refused (done here). */
static int RefEdit(FieldEdit* e, const uint8_t* at)
{
    uint32_t value, original;
    memcpy(&value, e->value, 4);
    memcpy(&original, e->hasOriginal ? e->original : at, 4);
    char why[256];
    uint64_t self = e->table && !strcmp(e->component->component, "ProjectileSettings") ? e->entity : 0;
    int state;
    if (e->field->ref == REF_MOUNT)
    {
        uint64_t wide, was;
        memcpy(&wide, e->value, 8);
        memcpy(&was, e->hasOriginal ? e->original : at, 8);
        state = RefMountCheck(e->entity, e->field->offset / 24, wide, was, why, sizeof why);
    }
    else if (e->field->ref == REF_PICKUP)
    {
        uint64_t wide, was;
        memcpy(&wide, e->value, 8);
        memcpy(&was, e->hasOriginal ? e->original : at, 8);
        state = RefPickupCheck(e->entity, e->field->offset / 64, wide, was, why, sizeof why);
    }
    else state = RefCheck(e->field->ref, value, original, self, why, sizeof why);
    if (state == REF_WAIT)
    {
        if (!e->refNoted) ApiLog(e->mod, "%s.%s of %016llx: %s", e->component->name, e->field->name, (unsigned long long)e->entity, why);
        e->refNoted = 1;
        e->waiting = 1;
        g_editsWaiting = 1;
        return state;
    }
    e->refNoted = 0;
    if (state == REF_REFUSED) { EditFailed(e, why); return state; }
    if (e->field->ref == REF_STATUS && RefStatusGap((uintptr_t)(at - e->field->offset), e->field->offset, value))
        ApiLog(e->mod, "%s.%s of %016llx: set after an empty slot, so the game does not apply it", e->component->name, e->field->name,
            (unsigned long long)e->entity);
    return REF_OK;
}

/* The reference kind of a stats edit's field (its mapping's "ref"), or REF_NONE. */
static int StatRefKind(const StatEdit* e)
{
    const SdkComponent* c = NULL;
    uint32_t offset = e->offset;
    if (!strcmp(e->kind, "component")) c = SdkComponentNamed(e->component);
    else if (!strcmp(e->kind, "EntityDelta"))
    {
        unsigned index, member;
        char trailing;
        if (sscanf(e->component, "%u:%u%c", &index, &member, &trailing) != 2) return REF_NONE;
        for (size_t i = 0; i < sizeof kSdkComponents / sizeof kSdkComponents[0]; i++)
            if (kSdkComponents[i].index == (int)index) c = &kSdkComponents[i];
        offset = member;
    }
    else c = SdkComponentNamed(e->kind);
    for (int i = 0; c != NULL && i < c->count; i++) /* (a mount slot is never a stats edit: they write 4 bytes) */
    {
        const SdkField* f = &c->fields[i]; /* a single u32, or an element of an array of them (a magazine's pattern) */
        if (f->ref != REF_NONE && f->ref != REF_MOUNT && f->ref != REF_PICKUP && f->type == HF_TYPE_U32 && offset >= f->offset &&
            offset + 4 <= (uint32_t)f->offset + f->size && (offset - f->offset) % 4 == 0) return (int)f->ref;
    }
    return REF_NONE;
}

/* A stats edit's reference (StatsWrite): REF_OK to write, else waiting (StatsRefsTick tries it again) or refused. */
static int RefStat(StatEdit* e, uintptr_t address, uint32_t original)
{
    if (e->ref < 0) e->ref = StatRefKind(e);
    if (e->ref == REF_NONE) return REF_OK;
    char why[256];
    uint64_t self = !strcmp(e->kind, "ProjectileSettings") ? (uint64_t)e->record : 0;
    int state = RefCheck((uint32_t)e->ref, e->value, original, self, why, sizeof why);
    if (state == REF_WAIT)
    {
        if (!e->refWaiting) ApiLog(e->mod, "stats %s: %s", e->label, why);
        e->refWaiting = 1;
        return state;
    }
    e->refWaiting = 0;
    if (state == REF_REFUSED) { StatsRefuse(e, why); return state; }
    if (e->ref == REF_STATUS && !strcmp(e->kind, "DamageInfo") && RefStatusGap(address - e->offset, e->offset, e->value))
        ApiLog(e->mod, "stats %s: set after an empty status slot, so the game does not apply it", e->label);
    return REF_OK;
}

/* Where a waiting stats edit writes now (its record found again), or 0. */
static uintptr_t StatRefAddress(StatEdit* e)
{
    if (!strcmp(e->kind, "component"))
    {
        uintptr_t own = StatsOwnRecordOf(e->component, e->resource);
        uint32_t stride = 0;
        uintptr_t record = own ? own : g_fieldRecord(e->component, e->resource, &stride, 1);
        return record ? record + e->offset : 0;
    }
    if (!strcmp(e->kind, "ProjectileSettings") || !strcmp(e->kind, "DamageInfo"))
    {
        int ready;
        uintptr_t row = g_tableRow(e->kind, (uint64_t)e->record, &ready);
        return row ? row + e->offset : 0;
    }
    return StatsExtraKind(e->kind) >= 0 ? StatsExtraRow(e) : 0;
}

/* The watch loop: stats edits whose references waited for their packages. */
static void StatsRefsTick(void)
{
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (now - last < 250) return;
    last = now;
    AcquireSRWLockExclusive(&g_statsLock);
    for (int i = 0; i < g_statCount; i++)
    {
        StatEdit* e = &g_stats[i];
        if (!e->refWaiting || !e->mod->loaded) continue;
        uintptr_t address = StatRefAddress(e);
        if (!address) { e->refWaiting = 0; StatsRefuse(e, "its record is no longer as reviewed"); continue; }
        StatsWrite(e, address);
    }
    ReleaseSRWLockExclusive(&g_statsLock);
}

static int StatsRefsWaiting(void)
{
    for (int i = 0; i < g_statCount; i++) if (g_stats[i].refWaiting) return 1;
    return 0;
}
