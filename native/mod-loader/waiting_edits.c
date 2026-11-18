/* Deferred edits that wait for their tables (fields.c ApplyEdit marks them): the settings tables some edits name load
 * after the entity data, at a time that differs between machines, so the first pass's count of waiting edits differs too.
 * The watch loop applies them as their tables load. Included by fields.c. */

/* The tables the waiting edits wait for, for the log: ": <table> xN, ..." (8 named at most), or "" when none waits. */
static int WaitingSlot(const SdkComponent** seen, int* kinds, const SdkComponent* component)
{
    for (int k = 0; k < *kinds; k++)
        if (seen[k] == component) return k;
    if (*kinds == 8) return -1;
    seen[*kinds] = component;
    return (*kinds)++;
}

static void WaitingTables(char* out, size_t size)
{
    const SdkComponent* seen[8];
    int counts[8] = {0}, kinds = 0, more = 0;
    for (int i = 0; i < g_editCount; i++)
    {
        if (!g_edits[i].waiting) continue;
        int k = WaitingSlot(seen, &kinds, g_edits[i].component);
        if (k < 0) more = 1;
        else counts[k]++;
    }
    size_t used = 0;
    out[0] = 0;
    for (int k = 0; k < kinds && used < size; k++)
        used += (size_t)snprintf(out + used, size - used, "%s%s x%d", k == 0 ? ": " : ", ", seen[k]->name, counts[k]);
    if (more && used < size) snprintf(out + used, size - used, ", and others");
}

/* Edits whose tables have loaded since (the watch loop calls this while any waits). Each time some apply, the log says
 * so and what the rest still wait for: before, the first pass's "waiting" count was the last word on them (#352). */
static void ApplyWaitingEdits(void)
{
    AcquireSRWLockExclusive(&g_fieldsLock);
    g_editsWaiting = 0;
    int waited = 0, applied = 0, refused = 0;
    for (int i = 0; i < g_editCount; i++)
    {
        if (!g_edits[i].waiting) continue;
        waited++;
        ApplyEdit(&g_edits[i]);
        if (!g_edits[i].waiting) { refused += g_edits[i].failed; applied += !g_edits[i].failed; }
    }
    if (applied + refused > 0)
    {
        char tables[256];
        WaitingTables(tables, sizeof tables);
        Note("deferred edits: %d more applied once their tables loaded, %d refused, %d still waiting%s", applied, refused,
            waited - applied - refused, tables);
    }
    ReleaseSRWLockExclusive(&g_fieldsLock);
}
