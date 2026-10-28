/* B.2: the sites the difficulty install wrote (RecordPatch, hellforge_modloader.c) and their byte audit. Included by
 * hellforge_modloader.c after ApiModuleBase: a few sites a tick, one sweep started a second at most, for the first ten
 * minutes after the install, and whenever a level is set up or a mission is launched. A site whose bytes are no longer
 * HellForge's is named once, with the first one marked. */

/* The bytes of a site as hex, for the site audit's expected and found: text ends in 0. */
static void HexText(const uint8_t* bytes, size_t size, char* text, size_t capacity)
{
    size_t n = 0;
    for (size_t i = 0; i < size && n + 3 <= capacity; i++) n += (size_t)snprintf(text + n, capacity - n, "%02x", bytes[i]);
    if (capacity > 0) text[n < capacity ? n : capacity - 1] = 0;
}

static ULONGLONG g_patchAuditStarted, g_patchAuditNextSweep, g_patchAuditLastSummary;
static int g_patchAuditIndex, g_patchAuditMismatches, g_patchAuditForce;

/* One recorded site whose bytes are no longer HellForge's: named once, the first one as such. */
static void AuditPatchSite(PatchAudit* patch, uintptr_t game, size_t gameSize)
{
    if (patch->reported || patch->kept == 0) return;
    uint8_t found[PATCH_AUDIT_BYTES];
    int unreadable = SafeRead(patch->address, found, patch->kept);
    if (!unreadable && memcmp(found, patch->bytes, patch->kept) == 0) return;
    patch->reported = 1;
    char expected[PATCH_AUDIT_BYTES * 2 + 1], actual[PATCH_AUDIT_BYTES * 2 + 16], where[96];
    HexText(patch->bytes, patch->kept, expected, sizeof expected);
    if (unreadable) snprintf(actual, sizeof actual, "unreadable");
    else HexText(found, patch->kept, actual, sizeof actual);
    if (game != 0 && patch->address >= game && patch->address < game + gameSize)
        snprintf(where, sizeof where, "game.dll+0x%llx", (unsigned long long)(patch->address - game));
    else
        snprintf(where, sizeof where, "0x%llx", (unsigned long long)patch->address);
    int first = g_patchAuditMismatches == 0;
    g_patchAuditMismatches++;
    NoteDiagnostic("difficulty: site audit %s: expected %s found %s%s", where, expected, actual, first ? " (first changed site)" : "");
}

static void AuditPatchSites(void)
{
    if (g_patchAuditOn || g_patchAuditCount == 0 || !DiagnosticLog()) return;
    ULONGLONG now = GetTickCount64();
    if (g_patchAuditStarted == 0) g_patchAuditStarted = now;
    if (g_patchAuditNextSweep != 0 && now < g_patchAuditNextSweep) return;
    int forced = g_patchAuditForce;
    if (g_patchAuditIndex == 0 && !forced && now - g_patchAuditStarted > 600000) return;
    size_t gameSize = 0;
    uintptr_t game = ApiModuleBase("game.dll", &gameSize);
    for (int n = 0; n < 8 && g_patchAuditIndex < g_patchAuditCount; n++, g_patchAuditIndex++)
        AuditPatchSite(&g_patchAudit[g_patchAuditIndex], game, gameSize);
    if (g_patchAuditIndex < g_patchAuditCount) return;
    g_patchAuditIndex = 0;
    g_patchAuditNextSweep = now + 1000;
    g_patchAuditForce = 0;
    if (g_patchAuditMismatches == 0 && (forced || now - g_patchAuditLastSummary >= 30000))
    {
        g_patchAuditLastSummary = now;
        NoteDiagnostic("difficulty: site audit: all %d sites still HellForge's", g_patchAuditCount);
    }
}

/* A level was set up or a mission launched: audit every site as soon as the watch thread can. */
static void AuditPatchSitesNow(void)
{
    if (g_patchAuditOn || g_patchAuditCount == 0 || !DiagnosticLog()) return;
    g_patchAuditIndex = 0;
    g_patchAuditForce = 1;
    g_patchAuditNextSweep = 0;
}
