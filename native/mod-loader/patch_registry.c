/* B.2: the writes the difficulty install makes, kept so the site audit (patch_audit.c) can tell later whether the game
 * still runs HellForge's bytes there. Included by hellforge_modloader.c after the logging helpers and before ApiWrite. */

/* Every write the difficulty install makes: its bytes, so the watch thread can tell later whether the game still runs
 * HellForge's bytes there. A site written twice keeps its last write. 128 bytes is every code patch: the larger writes
 * are tables, whose start is what a later restore or decode would change first. */
#define PATCH_AUDIT_MAX 4096
#define PATCH_AUDIT_BYTES 128
typedef struct PatchAudit
{
    uintptr_t address;
    uint32_t length; /* the write's size */
    uint32_t kept;   /* its first bytes, as many as PATCH_AUDIT_BYTES holds */
    uint8_t bytes[PATCH_AUDIT_BYTES];
    int reported;    /* the site audit has named it */
} PatchAudit;
static PatchAudit g_patchAudit[PATCH_AUDIT_MAX];
static int g_patchAuditCount;
static volatile LONG g_patchAuditOn; /* the difficulty install's steps: record what they write */
static int g_patchAuditFullNoted;
static SRWLOCK g_patchAuditLock = SRWLOCK_INIT; /* a live edit's ApiWrite can land while the install runs */

/* One write's bytes in the audit (also for a difficulty site patched after the steps, PatchLaunch); the record is kept
 * once, so a site written twice keeps its last write. */
static void StorePatch(uintptr_t address, const void* data, size_t size)
{
    if (!DiagnosticLog() || size == 0) return;
    size_t kept = size < PATCH_AUDIT_BYTES ? size : PATCH_AUDIT_BYTES;
    AcquireSRWLockExclusive(&g_patchAuditLock);
    int slot = -1;
    for (int i = 0; i < g_patchAuditCount; i++) if (g_patchAudit[i].address == address) { slot = i; break; }
    if (slot < 0)
    {
        if (g_patchAuditCount >= PATCH_AUDIT_MAX)
        {
            if (!g_patchAuditFullNoted) { g_patchAuditFullNoted = 1; NoteDiagnostic("difficulty: patch audit is full at %d sites; later writes are not audited", PATCH_AUDIT_MAX); }
            ReleaseSRWLockExclusive(&g_patchAuditLock);
            return;
        }
        slot = g_patchAuditCount++;
        g_patchAudit[slot].address = address;
        g_patchAudit[slot].reported = 0;
    }
    g_patchAudit[slot].length = (uint32_t)size;
    g_patchAudit[slot].kept = (uint32_t)kept;
    memcpy(g_patchAudit[slot].bytes, data, kept);
    ReleaseSRWLockExclusive(&g_patchAuditLock);
}

static void RecordPatch(uintptr_t address, const void* data, size_t size)
{
    if (!g_patchAuditOn) return;
    StorePatch(address, data, size);
}
