/* Read-only, per-load observations for the launcher. Values stay in the local session directory.
 * A field a stats file changed reports its value from before the stats pass: the editor's game value. */
/* Whether one of the mods' own records of the component, in the table, has that index. */
static int StatsOwnedIndex(const Component* c, const uint8_t* table, uint32_t index)
{
    for (int j = 0; j < g_ownedCount; j++)
        if (g_owned[j].component == c->index && g_owned[j].index == table && g_owned[j].stride == c->stride &&
            index >= g_owned[j].first && index < g_owned[j].next) return 1;
    return 0;
}

/* The field of the record an index row names for the probe (flags: the row's): a vanilla record when it is the probe's
 * record, or a mod's own; 0 otherwise. */
static uintptr_t StatsObservedRecord(const StatEdit* e, const Component* c, uint8_t* table, uint32_t index, uint32_t flags)
{
    if (flags) return 0;
    int valid = (index < c->records && index == e->record) || StatsOwnedIndex(c, table, index);
    return valid ? (uintptr_t)table + c->recordOffset + (uintptr_t)index * c->stride + e->offset : 0;
}

static uintptr_t StatsObservedComponent(StatEdit* e)
{
    const Component* c = LastComponentNamed(e->component);
    if (!c || c->stride < 4 || e->offset > c->stride - StatSize(e) || c->recordOffset != c->indices * 16u || !c->indices) return 0;
    uint8_t* table = StatePointer(EntityState(), REG_TABLES + 8 * c->index);
    uint8_t head[32];
    if (!table || SafeRead((uintptr_t)table - 32, head, 32) || memcmp(head, &c->index, 4) || memcmp(head + 4, c->header, 28)) return 0;
    uint32_t at = (uint32_t)(e->resource % c->indices);
    for (uint32_t i = 0; i < c->indices; i++)
    {
        uint8_t row[16]; uint64_t resource; uint32_t index, flags;
        if (SafeRead((uintptr_t)table + at * 16u, row, sizeof row)) return 0;
        memcpy(&resource, row, 8); memcpy(&index, row + 8, 4); memcpy(&flags, row + 12, 4);
        if (!resource) return 0;
        if (resource == e->resource) return StatsObservedRecord(e, c, table, index, flags);
        at = (at + 1) % c->indices;
    }
    return 0;
}

/* A table kind's phase (StatsTableOf): projectile, damage, else stratagem. */
static int StatsTablePhase(const char* kind) { return !strcmp(kind, "ProjectileSettings") ? 0 : !strcmp(kind, "DamageInfo") ? 1 : 2; }

static uintptr_t StatsObservedTable(StatEdit* e, uintptr_t game)
{
    if (StatsExtraKind(e->kind) >= 0) return StatsExtraRow(e);
    int phase = StatsTablePhase(e->kind);
    uintptr_t table = StatsTableOf(phase, game);
    uint32_t count = phase == 0 ? PRJ_VANILLA : phase == 1 ? DMG_VANILLA : STR_VANILLA;
    uint32_t stride = phase == 0 ? PRJ_ROW : phase == 1 ? DMG_ROW : STR_ROW;
    if (!game || e->offset < 4 || e->offset > stride - 4) return 0;
    uint32_t first = phase == 2 ? 1 : (uint32_t)e->record;
    uint32_t last = phase == 2 ? count : first + 1;
    if (first == 0 || first >= count) return 0;
    uintptr_t found = StatsTableRow(phase, table, first, last, e->record);
    return found ? found + e->offset : 0;
}

static void StatsSaveSnapshot(void)
{
    wchar_t temporary[MAX_PATH], path[MAX_PATH];
    if (!g_logs[0] || !g_statsSchema[0] || swprintf(path, MAX_PATH, L"%ls\\stats-values.tsv", g_logs) < 0 ||
        swprintf(temporary, MAX_PATH, L"%ls\\stats-values.tsv.partial", g_logs) < 0) return;
    FILE* file = _wfopen(temporary, L"wb");
    if (!file) return;
    fprintf(file, "stats_snapshot\t2\t%s\t%s\n", g_build, g_statsSchema);
    for (int i = 0; i < g_statsProbeCount; i++)
    {
        const StatEdit* e = &g_statsProbes[i];
        fprintf(file, "%s\t%s\t%016llX\t%lld\t%u\t%s\t%s\t%08X\n", e->kind, e->component,
            (unsigned long long)e->resource, (long long)e->record, e->offset, e->floating ? "f32" : e->signedValue ? "i32" : e->size == 1 ? "u8" : "u32",
            e->applied ? "ok" : "unavailable", e->value);
    }
    int failed = ferror(file); if (fclose(file)) failed = 1;
    if (!failed) MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

/* What the last snapshot was taken after: the component pass's generation, and which table passes were done. */
static struct { unsigned componentGeneration; int phases[3], extra[STATS_EXTRA_COUNT], boosters, perks, melee; } g_statsSeen = {UINT32_MAX};

/* Whether a pass was done since the last snapshot (components: the component pass ran again). */
static int StatsSnapshotChanged(int components)
{
    int changed = components;
    for (int i = 0; i < 3; i++) if (g_statsSeen.phases[i] != g_statsTablesDone[i]) changed = 1;
    for (int i = 0; i < (int)STATS_EXTRA_COUNT; i++) if (g_statsSeen.extra[i] != g_statsExtraDone[i]) changed = 1;
    if (g_statsSeen.boosters != g_boostersDone || g_statsSeen.perks != g_perksDone || g_statsSeen.melee != g_meleeDone) changed = 1;
    return changed;
}

/* Whether the pass a probe's field belongs to was done since the last snapshot: it is read again. */
static int StatsProbeDue(const StatEdit* e, int components)
{
    if (!strcmp(e->kind, "component")) return components;
    if (!strncmp(e->kind, "Passive", 7)) return g_statsSeen.perks != g_perksDone;
    if (!strcmp(e->kind, "MeleeHitVolume")) return g_statsSeen.melee != g_meleeDone;
    if (!strcmp(e->kind, "BoosterSettings")) return g_statsSeen.boosters != g_boostersDone;
    int kind = StatsExtraKind(e->kind);
    if (kind >= 0) return g_statsSeen.extra[kind] != g_statsExtraDone[kind];
    int phase = StatsTablePhase(e->kind);
    return g_statsSeen.phases[phase] != g_statsTablesDone[phase];
}

/* Where a probe's field is now; 0 none. */
static uintptr_t StatsProbeAddress(StatEdit* e, uintptr_t game)
{
    if (!strcmp(e->kind, "component")) return StatsObservedComponent(e);
    if (!strncmp(e->kind, "Passive", 7))
    {
        int ready;
        uintptr_t at = PerkRow(e->kind, (uint64_t)e->record, &ready);
        return at && e->offset >= 4 && e->offset <= 12 ? at + e->offset : 0;
    }
    if (!strcmp(e->kind, "MeleeHitVolume"))
    {
        uintptr_t row = MeleeVolume((uint64_t)e->record);
        return row && e->offset >= 4 && e->offset <= MELEE_VOLUME_SIZE - 4 ? row + e->offset : 0;
    }
    if (!strcmp(e->kind, "BoosterSettings"))
    {
        uintptr_t row = BoosterRow((uint64_t)e->record);
        return row && e->offset >= 4 && e->offset <= BOOSTER_ROW - 4 ? row + e->offset : 0;
    }
    return StatsObservedTable(e, game);
}

/* A probe's value at its address: the game's, from before a stats edit there. */
static void StatsProbeRead(StatEdit* e, uintptr_t address)
{
    e->value = 0;
    e->applied = address && !SafeRead(address, &e->value, StatSize(e));
    e->address = e->applied ? address : 0; /* get_stat reads it again */
    for (int j = 0; e->applied && j < g_statCount; j++)
        if (g_stats[j].applied && g_stats[j].address == address) { e->value = g_stats[j].original; break; }
    if (e->applied && e->floating && !StatsFiniteBits(e->value)) e->applied = 0;
}

static void StatsSnapshotReady(uintptr_t game)
{
    if (!g_statsProbeCount || strcmp(g_build, "25480438")) return;
    int components = g_statsComponentsReady && g_statsSeen.componentGeneration != g_statsGeneration;
    if (!StatsSnapshotChanged(components)) return;
    AcquireSRWLockExclusive(&g_statsLock);
    for (int i = 0; i < g_statsProbeCount; i++)
    {
        StatEdit* e = &g_statsProbes[i];
        if (StatsProbeDue(e, components)) StatsProbeRead(e, StatsProbeAddress(e, game));
    }
    if (components) g_statsSeen.componentGeneration = g_statsGeneration;
    memcpy(g_statsSeen.phases, g_statsTablesDone, sizeof g_statsSeen.phases); memcpy(g_statsSeen.extra, g_statsExtraDone, sizeof g_statsSeen.extra);
    g_statsSeen.boosters = g_boostersDone; g_statsSeen.perks = g_perksDone; g_statsSeen.melee = g_meleeDone;
    ReleaseSRWLockExclusive(&g_statsLock);
    StatsSaveSnapshot();
}

static int StatsParseProbe(char** f, int n)
{
    if (n != 7) return 1;
    StatEdit e = {0}; char* end;
    if (strlen(f[1]) >= sizeof e.kind || strlen(f[2]) >= sizeof e.component ||
        (strcmp(f[1], "component") && !StatsTableKind(f[1]) && StatsExtraKind(f[1]) < 0)) return 1;
    snprintf(e.kind, sizeof e.kind, "%s", f[1]); snprintf(e.component, sizeof e.component, "%s", f[2]);
    e.resource = strtoull(f[3], &end, 16); if (*end) return 1;
    e.record = strtoll(f[4], &end, 10); if (*end || e.record < 0 || e.record > UINT32_MAX) return 1;
    uint64_t offset = strtoull(f[5], &end, 10); if (*end || offset > UINT32_MAX) return 1; e.offset = (uint32_t)offset;
    if (strcmp(f[6], "f32") && strcmp(f[6], "i32") && strcmp(f[6], "u32") && (strcmp(f[6], "u8") || strcmp(e.kind, "component"))) return 1;
    e.floating = !strcmp(f[6], "f32"); e.signedValue = !strcmp(f[6], "i32"); e.size = !strcmp(f[6], "u8") ? 1 : 4;
    StatEdit* slot = StatsProbeSlot();
    if (slot == NULL) return 1;
    *slot = e;
    return 0;
}
