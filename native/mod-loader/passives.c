/* Armor passives of the local player (SDK 6: set_passives, get_passives; docs/modding/runtime.md "Live edits"). Included by
 * hellforge_modloader.c after fields.c. A port of HD2Runtime's runtime/player_passives.lua (research
 * player-attributes-F5FEE03DCFDB.json; live-proven there for the armor slot):
 *
 * The customization manager ([game.dll +0x33264F8]) holds, per player, an applied record (+0x96C, 0x44 bytes each) whose
 * +0x3C is the armor passive and +0x38 the helmet ("second") passive: ids of the passive table, which the game's
 * attribute reader looks up (the armor slot first, then the helmet's; a bonus never stacks). The game derives both from
 * the kits worn (a kit row's +0x1C) and writes them again only when a kit changes. An override writes the slots of the
 * local player's record (found through the player list, the local peer and the manager's entity map, its descriptor
 * checked to carry the same entity) and keeps them: every 250 ms it looks again, writes a slot back after a kit change,
 * and only ever replaces the value the game derived or the one it wrote itself.
 *
 * Guards: the 59 code pins of the research, proven byte for byte first; the record in private read-write memory, read
 * back after each write; solo only (every peer builds its own record from the replicated kits, and which peer applies
 * each effect is not proven: with other players the slots go back to the kits' values and the override waits). A
 * passive whose effect needs a package (INTEGRATED EXPLOSIVES, ADRENO-DEFIBRILLATOR) is refused unless the armor gives it
 * already (HellForge does not load those packages yet), and INTEGRATED EXPLOSIVES works only in the armor slot. */
#include "passive_pins.h"

#define PSV_MANAGER 0x33264F8u
#define PSV_PLAYERS 0x3326468u
#define PSV_LOCAL_USER 0x347CEF0u
#define PSV_LOCAL_PEER 45976u
#define PSV_PLAYER_COUNT 132u
#define PSV_PLAYER_PEERS 712u
#define PSV_PLAYER_STRIDE 56u
#define PSV_PLAYER_DESCRIPTORS 232u
#define PSV_KITS 0u
#define PSV_KIT_COUNT 8u
#define PSV_PASSIVE_COUNT 40u
#define PSV_PASSIVE_INDEX 48u
#define PSV_PASSIVE_IDS 42u
#define PSV_RECORD_COUNT 2280u
#define PSV_MAP 2352u
#define PSV_DESCRIPTORS 2376u
#define PSV_APPLIED 2412u
#define PSV_APPLIED_STRIDE 68u
#define PSV_RECORDS 4u
#define PSV_HELMET_KIT 4u
#define PSV_ARMOR_KIT 12u
#define PSV_SECOND 56u
#define PSV_ARMOR 60u
#define PSV_MAX_KITS 2048u

typedef struct LocalPassives
{
    uintptr_t record;
    uint32_t players;
    uint32_t derived[2]; /* armor, second: what the kits give */
    uint32_t slots[2];   /* what the record holds */
    int passiveValid[PSV_PASSIVE_IDS];
} LocalPassives;

static SRWLOCK g_passivesLock = SRWLOCK_INIT;
static int g_passivesProven;            /* 0 not yet, 1 proven, -1 refused */
static int g_passivesHeld;
static HfMod* g_passivesMod;
static uint32_t g_passivesWant[2] = {HF_PASSIVE_KIT, HF_PASSIVE_KIT};
static uint32_t g_passivesWritten[2];
static int g_passivesWrote[2];
static char g_passivesStatus[192];

static int PassivesProve(uintptr_t game)
{
    if (g_passivesProven) return g_passivesProven > 0;
    for (size_t i = 0; i < sizeof kPassivePins / sizeof kPassivePins[0]; i++)
    {
        uint8_t bytes[8];
        if (SafeRead(game + kPassivePins[i].rva, bytes, kPassivePins[i].size) || memcmp(bytes, kPassivePins[i].bytes, kPassivePins[i].size))
        {
            snprintf(g_passivesStatus, sizeof g_passivesStatus, "the game's passive code is not the reviewed one (game.dll +0x%X)", kPassivePins[i].rva);
            g_passivesProven = -1;
            return 0;
        }
    }
    g_passivesProven = 1;
    return 1;
}

/* A kit row's passive and type, by kit id (a scan of the kit table: it is static, 411 rows in this build). */
static int PassivesKit(uintptr_t manager, uint32_t id, uint32_t* passive, uint32_t* type)
{
    static uintptr_t cachedTable, cachedRow[PSV_MAX_KITS];
    static uint32_t cachedCount, cachedId[PSV_MAX_KITS];
    uintptr_t table = 0;
    uint32_t count = 0;
    if (id == 0 || SafeRead(manager + PSV_KITS, &table, 8) || SafeRead(manager + PSV_KIT_COUNT, &count, 4) || !table || !count || count > PSV_MAX_KITS) return 0;
    if (table != cachedTable || count != cachedCount)
    {
        cachedTable = table;
        cachedCount = count;
        for (uint32_t i = 0; i < count; i++)
        {
            uintptr_t row = 0;
            cachedRow[i] = 0;
            cachedId[i] = 0;
            if (!SafeRead(table + i * 8u, &row, 8) && row && !SafeRead(row, &cachedId[i], 4)) cachedRow[i] = row;
        }
    }
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t check = 0;
        if (cachedId[i] != id || !cachedRow[i] || SafeRead(cachedRow[i], &check, 4) || check != id) continue;
        return !SafeRead(cachedRow[i] + 28, passive, 4) && !SafeRead(cachedRow[i] + 40, type, 4);
    }
    return 0;
}

/* the local player: the player list's entry with the local peer (out->players: how many are in); 0 with *why. */
static uint32_t PassivesLocalEntity(uintptr_t game, LocalPassives* out, const char** why)
{
    uintptr_t players = 0, user = 0;
    uint8_t peer[8];
    uint32_t count = 0, entity = 0;
    if (SafeRead(game + PSV_PLAYERS, &players, 8) || !players || SafeRead(players + PSV_PLAYER_COUNT, &count, 4) || count > 4 ||
        SafeRead(game + PSV_LOCAL_USER, &user, 8) || !user || SafeRead(user + PSV_LOCAL_PEER, peer, 8))
    { *why = "the player list is not readable yet"; return 0; }
    out->players = count;
    for (uint32_t slot = 0; slot < count && !entity; slot++)
    {
        uint8_t block[8];
        uintptr_t descriptor = 0;
        if (SafeRead(players + PSV_PLAYER_PEERS + slot * PSV_PLAYER_STRIDE, block, 8) || memcmp(block, peer, 8)) continue;
        if (!SafeRead(players + PSV_PLAYER_DESCRIPTORS + slot * 8u, &descriptor, 8) && descriptor) SafeRead(descriptor + 8, &entity, 4);
    }
    if (!entity) *why = "the local player is not in the player list";
    return entity;
}

/* its record: the manager's entity map, linear probing from entity x multiplier. UINT32_MAX with *why when none. */
static uint32_t PassivesRecordOf(uintptr_t manager, uint32_t entity, const char** why)
{
    uint8_t map[20];
    uintptr_t slots = 0;
    uint32_t capacity, empty, multiplier, at = UINT32_MAX;
    if (SafeRead(manager + PSV_MAP, map, sizeof map)) { *why = "the entity map is not readable"; return UINT32_MAX; }
    memcpy(&slots, map, 8); memcpy(&capacity, map + 8, 4); memcpy(&empty, map + 12, 4); memcpy(&multiplier, map + 16, 4);
    if (!slots || !capacity || capacity > 64 || capacity % 2) { *why = "the entity map is not the reviewed one"; return UINT32_MAX; }
    uint32_t start = entity * multiplier;
    for (uint32_t probe = 0; probe < capacity; probe++)
    {
        uint32_t entry[2];
        if (SafeRead(slots + ((start + probe) % capacity) * 8u, entry, 8)) { *why = "the entity map is not readable"; return UINT32_MAX; }
        if (entry[0] == entity) { at = entry[1]; break; }
        if (entry[0] == empty) break;
    }
    if (at == UINT32_MAX) *why = "the local player has no customization record yet";
    return at;
}

/* The record's slots, and what the kits give: the armor kit's passive, and the helmet's second (0 none). 0 with *why. */
static int PassivesDerived(uintptr_t manager, LocalPassives* out, const char** why)
{
    uint32_t armorKit = 0, helmetKit = 0, passive = 0, type = 0;
    if (SafeRead(out->record + PSV_ARMOR_KIT, &armorKit, 4) || SafeRead(out->record + PSV_HELMET_KIT, &helmetKit, 4) ||
        SafeRead(out->record + PSV_ARMOR, &out->slots[0], 4) || SafeRead(out->record + PSV_SECOND, &out->slots[1], 4))
    { *why = "the record is not readable"; return 0; }
    if (!PassivesKit(manager, armorKit, &passive, &type) || type != 0) { *why = "the armor kit is not in the kit table yet"; return 0; }
    out->derived[0] = passive;
    out->derived[1] = PassivesKit(manager, helmetKit, &passive, &type) && type == 1 ? passive : 0;
    return 1;
}

/* The local player's applied record, as PassivesTick needs it; 0 with *why. */
static int PassivesLocal(uintptr_t game, LocalPassives* out, const char** why)
{
    memset(out, 0, sizeof *out);
    uintptr_t manager = 0;
    if (SafeRead(game + PSV_MANAGER, &manager, 8) || !manager) { *why = "the customization manager is not created yet"; return 0; }
    uint32_t passiveCount = 0, records = 0;
    int32_t index[PSV_PASSIVE_IDS];
    if (SafeRead(manager + PSV_PASSIVE_COUNT, &passiveCount, 4) || SafeRead(manager + PSV_RECORD_COUNT, &records, 4) ||
        SafeRead(manager + PSV_PASSIVE_INDEX, index, sizeof index) || !passiveCount || passiveCount > PSV_PASSIVE_IDS || records > PSV_RECORDS)
    { *why = "the customization manager is not the reviewed one"; return 0; }
    for (uint32_t id = 0; id < PSV_PASSIVE_IDS; id++) out->passiveValid[id] = index[id] >= 0 && (uint32_t)index[id] < passiveCount;
    uint32_t entity = PassivesLocalEntity(game, out, why);
    if (!entity) return 0;
    uint32_t at = PassivesRecordOf(manager, entity, why);
    if (at == UINT32_MAX) return 0;
    uintptr_t descriptor = 0;
    uint32_t carried = 0;
    if (at >= records || at >= PSV_RECORDS || SafeRead(manager + PSV_DESCRIPTORS + at * 8u, &descriptor, 8) || !descriptor ||
        SafeRead(descriptor + 8, &carried, 4) || carried != entity)
    { *why = "the record does not carry the local player's entity"; return 0; }
    out->record = manager + PSV_APPLIED + at * PSV_APPLIED_STRIDE;
    return PassivesDerived(manager, out, why);
}

static int PassiveIn(const uint32_t* list, size_t count, uint32_t id)
{
    for (size_t i = 0; i < count; i++) if (list[i] == id) return 1;
    return 0;
}

/* Puts back what the kits give where this override wrote (and the record still holds what it wrote). */
static void PassivesRestore(const LocalPassives* local)
{
    for (int s = 0; s < 2; s++)
    {
        if (!g_passivesWrote[s]) continue;
        if (local->slots[s] == g_passivesWritten[s]) ApiWrite(local->record + (s ? PSV_SECOND : PSV_ARMOR), &local->derived[s], 4);
        g_passivesWrote[s] = 0;
    }
}

static int PassivesWritable(uintptr_t address)
{
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery((void*)address, &info, sizeof info) && info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && info.Protect == PAGE_READWRITE;
}

/* The passive slot s should hold: the override's (HF_PASSIVE_KIT: the kit's), the kit's when the game cannot take it
 * there (status says why). */
static uint32_t PassivesWanted(const LocalPassives* local, int s, char* status, size_t statusSize)
{
    uint32_t want = g_passivesWant[s] == HF_PASSIVE_KIT ? local->derived[s] : g_passivesWant[s];
    if (want != local->derived[s] && PassiveIn(kPassivePackaged, sizeof kPassivePackaged / sizeof kPassivePackaged[0], want))
    { snprintf(status, statusSize, "passive %u needs an effect package HellForge does not load yet", want); want = local->derived[s]; }
    if (s == 1 && want != local->derived[s] && PassiveIn(kPassiveArmorOnly, sizeof kPassiveArmorOnly / sizeof kPassiveArmorOnly[0], want))
    { snprintf(status, statusSize, "passive %u works only as the armor passive", want); want = local->derived[s]; }
    return want;
}

/* Slot s written with what it should hold (status says why not). */
static void PassivesSlot(const LocalPassives* local, int s, char* status, size_t statusSize)
{
    uint32_t want = PassivesWanted(local, s, status, statusSize);
    if (local->slots[s] == want) return;
    /* only the kits' value or the one this override wrote is replaced */
    if (local->slots[s] != local->derived[s] && !(g_passivesWrote[s] && local->slots[s] == g_passivesWritten[s]))
    { snprintf(status, statusSize, "the %s slot holds %u, which neither the kit nor HellForge put there", s ? "second" : "armor", local->slots[s]); return; }
    uintptr_t at = local->record + (s ? PSV_SECOND : PSV_ARMOR);
    uint32_t check = 0;
    if (!PassivesWritable(at) || ApiWrite(at, &want, 4) || SafeRead(at, &check, 4) || check != want)
    { snprintf(status, statusSize, "the record could not be written"); return; }
    g_passivesWritten[s] = want;
    g_passivesWrote[s] = want != local->derived[s];
    if (g_passivesMod) ApiLog(g_passivesMod, "passives: %s slot %u (the kit gives %u)", s ? "second" : "armor", want, local->derived[s]);
}

/* Every 250 ms from the watch loop (the native fixture calls it with a game image of its own). */
static ULONGLONG g_passivesEvery = 250; /* the native fixture looks every call */
static uintptr_t g_passivesGame;        /* the native fixture's game image (else game.dll) */
static void PassivesTick(uintptr_t game)
{
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (game == 0 || now - last < g_passivesEvery) return;
    last = now;
    AcquireSRWLockExclusive(&g_passivesLock);
    int active = g_passivesHeld || g_passivesWrote[0] || g_passivesWrote[1];
    if (!active || !PassivesProve(game)) { ReleaseSRWLockExclusive(&g_passivesLock); return; }
    LocalPassives local;
    const char* why = NULL;
    if (!PassivesLocal(game, &local, &why))
    {
        snprintf(g_passivesStatus, sizeof g_passivesStatus, "waiting: %s", why);
        ReleaseSRWLockExclusive(&g_passivesLock);
        return;
    }
    if (!g_passivesHeld || local.players != 1)
    {
        PassivesRestore(&local);
        if (g_passivesHeld) snprintf(g_passivesStatus, sizeof g_passivesStatus, "solo only: the armor passives are the armor's own while %u players are in", local.players);
        ReleaseSRWLockExclusive(&g_passivesLock);
        return;
    }
    char status[192] = "held";
    for (int s = 0; s < 2; s++) PassivesSlot(&local, s, status, sizeof status);
    snprintf(g_passivesStatus, sizeof g_passivesStatus, "%s", status);
    ReleaseSRWLockExclusive(&g_passivesLock);
}

/* SDK 6, set_passives: the local player's armor passive and second passive (HF_PASSIVE_KIT: the kits' own). Returns 1
 * held (or, both HF_PASSIVE_KIT, released), 0 refused (an id the game has no passive for). */
static int ApiSetPassives(HfMod* mod, uint32_t armor, uint32_t second)
{
    uint32_t want[2] = {armor, second};
    for (int s = 0; s < 2; s++)
        if (want[s] != HF_PASSIVE_KIT && want[s] >= PSV_PASSIVE_IDS)
        { ApiLog(mod, "set_passives: %u is not a passive id", want[s]); return 0; }
    AcquireSRWLockExclusive(&g_passivesLock);
    g_passivesWant[0] = armor;
    g_passivesWant[1] = second;
    g_passivesHeld = armor != HF_PASSIVE_KIT || second != HF_PASSIVE_KIT;
    g_passivesMod = mod;
    snprintf(g_passivesStatus, sizeof g_passivesStatus, "%s", g_passivesHeld ? "waiting for the game" : "released");
    ReleaseSRWLockExclusive(&g_passivesLock);
    ApiLog(mod, "set_passives: armor %d, second %d", (int)armor, (int)second);
    return 1;
}

/* SDK 6, get_passives: what the local player's record holds and what the kits give, and the override's state. */
static int ApiGetPassives(HfMod* mod, HfPassives* out)
{
    if (out == NULL) return 0;
    memset(out, 0, sizeof *out);
    uintptr_t game = g_passivesGame ? g_passivesGame : ApiModuleBase("game.dll", NULL);
    AcquireSRWLockExclusive(&g_passivesLock);
    out->want_armor = g_passivesWant[0];
    out->want_second = g_passivesWant[1];
    out->held = g_passivesHeld;
    snprintf(out->status, sizeof out->status, "%s", g_passivesStatus);
    LocalPassives local;
    const char* why = NULL;
    int read = game && PassivesProve(game) && PassivesLocal(game, &local, &why);
    if (read)
    {
        out->armor = local.slots[0];
        out->second = local.slots[1];
        out->kit_armor = local.derived[0];
        out->kit_second = local.derived[1];
        out->players = local.players;
    }
    else if (why && !g_passivesHeld) snprintf(out->status, sizeof out->status, "%s", why);
    ReleaseSRWLockExclusive(&g_passivesLock);
    return read;
}
