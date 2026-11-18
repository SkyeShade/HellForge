/* record-hash (docs/performance.md): the world's destruction-record map ([game + 0x346D528] + 0xC5F48: slots, count,
 * empty key, multiplier) hashes a handle as handle x 1. Destructibles get consecutive handles, so they fill one run of
 * slots and every miss walks the whole run (11 of 14.4 ms a frame on a full 16 km map). Its setup now stores
 * 0x9E3779B1, an odd number: the low bits stay a one-to-one map, so the run is spread across the table. Included by
 * hellforge_modloader.c after render_guards.c: PinMatches (packages.c), ApiWrite, PerfFixOn, Note, g_build. */

#define RH_SITE 0x13DF88Cu          /* mov dword [rbx+0xc5f58], 1: the setup's store, the field's only writer */
#define RH_SITE_IMM (RH_SITE + 6u)
#define RH_SETUP_STARTED 0x3326380u /* 0 until game.dll's plugin setup starts; it builds the map once per process */
#define RH_SCATTER 0x9E3779B1u

static const uint8_t kRhSitePin[10] = {0xc7, 0x83, 0x58, 0x5f, 0x0c, 0x00, 0x01, 0x00, 0x00, 0x00};

/* Every load of the multiplier: each probe (20 of them) and the shared insert (0x172F790, through the map's header)
 * hashes with the field, so one store covers all readers. No probe erases (the map only fills until a clear), so
 * every lookup has the same answer under either multiplier. */
static const struct { uint32_t rva; uint8_t length; uint8_t pin[7]; } kRhReaders[] = {
    {0x5F2D28u, 7, {0x45, 0x8b, 0x90, 0x58, 0x5f, 0x0c, 0x00}}, {0x60DDDDu, 7, {0x44, 0x8b, 0x96, 0x58, 0x5f, 0x0c, 0x00}},
    {0x815F06u, 7, {0x45, 0x8b, 0x8a, 0x58, 0x5f, 0x0c, 0x00}}, {0x8167FCu, 7, {0x45, 0x8b, 0x9a, 0x58, 0x5f, 0x0c, 0x00}},
    {0x13BBA05u, 7, {0x44, 0x8b, 0x97, 0x58, 0x5f, 0x0c, 0x00}}, {0x13C6B0Eu, 7, {0x44, 0x8b, 0x8e, 0x58, 0x5f, 0x0c, 0x00}},
    {0x13DF4E7u, 7, {0x41, 0x8b, 0x99, 0x58, 0x5f, 0x0c, 0x00}}, {0x13E00C4u, 7, {0x41, 0x8b, 0x9a, 0x58, 0x5f, 0x0c, 0x00}},
    {0x13E0312u, 7, {0x45, 0x8b, 0x96, 0x58, 0x5f, 0x0c, 0x00}}, {0x13E1875u, 6, {0x8b, 0x99, 0x58, 0x5f, 0x0c, 0x00}},
    {0x13E1B74u, 7, {0x41, 0x8b, 0x9b, 0x58, 0x5f, 0x0c, 0x00}}, {0x13E1CA4u, 7, {0x41, 0x8b, 0x9b, 0x58, 0x5f, 0x0c, 0x00}},
    {0x13E255Au, 6, {0x8b, 0xbb, 0x58, 0x5f, 0x0c, 0x00}}, {0x13E295Bu, 6, {0x8b, 0x9f, 0x58, 0x5f, 0x0c, 0x00}},
    {0x13E2E35u, 7, {0x44, 0x8b, 0x99, 0x58, 0x5f, 0x0c, 0x00}}, {0x13E302Bu, 6, {0x8b, 0x99, 0x58, 0x5f, 0x0c, 0x00}},
    {0x13E343Fu, 7, {0x41, 0x8b, 0xba, 0x58, 0x5f, 0x0c, 0x00}}, {0x13E37C0u, 6, {0x8b, 0xb9, 0x58, 0x5f, 0x0c, 0x00}},
    {0x13E3AA8u, 7, {0x41, 0x8b, 0x9b, 0x58, 0x5f, 0x0c, 0x00}}, {0x13E3D75u, 6, {0x8b, 0x99, 0x58, 0x5f, 0x0c, 0x00}},
    {0x172F7A5u, 4, {0x44, 0x8b, 0x59, 0x10}}};
#define RH_READERS (sizeof kRhReaders / sizeof kRhReaders[0])

static int g_rhState; /* 0 waiting for game.dll, 1 installed, -1 not */

static int RecordHashPinned(uintptr_t game)
{
    if (!PinMatches(game + RH_SITE, kRhSitePin, sizeof kRhSitePin)) return 0;
    for (size_t i = 0; i < RH_READERS; i++)
        if (!PinMatches(game + kRhReaders[i].rva, kRhReaders[i].pin, kRhReaders[i].length)) return 0;
    return 1;
}

/* The store's operand, before the setup runs: the map is empty then, and the value never changes after, so every key
 * is placed and found with one multiplier. Once the setup has started the game's 1 stays for the session. Should the
 * setup reach the store during the write, it still stores one value once, odd whichever bytes it sees. */
static void RecordHashInstallAt(uintptr_t game)
{
    uint64_t started = 1;
    if (g_rhState != 0 || game == 0) return;
    g_rhState = -1;
    if (!PerfFixOn("record-hash")) return;
    if (SafeRead(game + RH_SETUP_STARTED, &started, 8) || started != 0) { Note("performance fix record-hash: not installed; the game had set its destruction records up already"); return; }
    if (!RecordHashPinned(game)) { Note("performance fix record-hash: not installed; the destruction records' code is not the reviewed one"); return; }
    uint32_t scatter = RH_SCATTER;
    if (ApiWrite(game + RH_SITE_IMM, &scatter, 4)) { Note("performance fix record-hash: not installed; the setup could not be rewritten"); return; }
    g_rhState = 1;
    Note("performance fix record-hash: on; the destruction-record map hashes with 0x%X instead of 1", RH_SCATTER);
}

/* Once, from the watch thread as soon as game.dll is decoded (WatchLoaded), with the physics pools: later is too late. */
static void RecordHashInstall(void)
{
    if (g_rhState != 0) return;
    if (strcmp(g_build, "25480438")) { g_rhState = -1; Note("performance fix record-hash: not installed; not game build 25480438"); return; }
    RecordHashInstallAt(ApiModuleBase("game.dll", NULL));
}
