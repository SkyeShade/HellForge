/* The entity records of game.dll's entity state ([game + 0x346BF98], constructed once by 0xFDA190 from 0xAB8C76): 2048
 * records of 24 B inline at state + 0xF32F18 ({+0 resource, +8 entity id or none = [game + 0x348456C], +0xC unit, +0x10
 * network id or 0x7FFF, +0x14 flags}), found through three open-addressed hashes with inline storage of 0x1000 slots
 * (id -> index +0xF1AEB0, network id -> index +0xF22EC8, unit -> entity +0xF2AEE0; each {storage, places, empty key,
 * multiplier}, home slot (key * multiplier + i) & (places - 1)). SpawnEntity 0xFDC140 takes the record at its cursor
 * (+0xF32F14) and searches the next free one at most 2048 slots on, with no full check: an extra large map's level
 * spawn filled all 2048 (run s97b) and the next spawn overwrote a live entity's record, its component instances then
 * missing (the s94b..s97b level-load crashes). The records cannot grow in place (the removal list's count and pointer,
 * two spawn-info defaults, the spawn lock and flags follow them up to +0xF3F830, the end of the state's memset at
 * 0xAB1D16, all at fixed offsets).
 *
 * On an extra large map they move to an allocation within 2 GB of the state, ER_COUNT records, before the constructor
 * runs (nothing is live then; the hashes' places are 0, so every lookup returns at its places test). Every instruction
 * that addresses a record does it with a displacement from the state (or from B = [game + 0x3326340], the state being
 * B + 0xE49F0), in one of these forms (D = records - state, ≡ 8 mod 24 as the original 0xF32F18 = 0xA21F6 * 24 + 8):
 * D + field; D / 8 or (D + 8) / 8 before a * 8 index (index * 3 + K); (D + 0x10) / 24 before (index + K) * 24; and
 * (D + 0xE49F0) / 8 from B. kEntityRecordSites lists all 56 (static, subagent entrec: every immediate and displacement
 * of game.dll that reaches the records with a scale of 1..48 from the state, state + 0x40 or B; a raw byte scan of every
 * section for each value found no other; the exe has none), the 0x800 counts (constructor loop 0xFDA1C4, SpawnEntity's
 * wrap and search 0xFDC20D / 0xFDC221 and their copy 0xFDC06E / 0xFDC082, the remove-all 0xFDD0A1, any-live 0xFDD698,
 * remove-unitless 0xFDD6DE), the removal list's 0x2000 bytes (2048 u32, pushed with no bound by 0xFDC310; allocated
 * from the frame allocator 0x173B8D0 at 0xAB5083, 0xAB7DBF, 0xFDC5D2, which takes a fresh block for a request that does
 * not fit) and the constructor's storage and places of the three hashes (nothing else writes them; their other users
 * read the fields). Each site's bytes are checked before anything is written: one that differs leaves it all as it is. */
#define ER_COUNT 16384u
#define ER_PLACES 0x10000u /* 4 x ER_COUNT, a power of two (only even home slots with the multiplier of 2) */
#define ER_CONSTRUCTOR 0xFDA190u
#define ER_STATE_GLOBAL 0x346BF98u
#define ER_B_GLOBAL 0x3326340u
#define ER_B_STATE 0xE49F0u
#define ER_VANILLA 0xF32F18u
static const char kEntityRecordCtorPin[] = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 89 4C 24 20 48 8B D9";
enum { ER_REC, ER_COUNT_SITE, ER_LIST, ER_HASH0, ER_HASH1, ER_HASH2, ER_PLACES_SITE };
static const struct { uint32_t rva; uint8_t at; uint8_t pin[7]; uint8_t kind; uint32_t add; uint8_t div; } kEntityRecordSites[] = {
    {0x5D31CEu, 4, {0x41, 0x8B, 0x8C, 0xCB}, ER_REC, 0xCu, 1},
    {0x678883u, 4, {0x41, 0x83, 0xBC, 0xCB}, ER_REC, 0xCu, 1},
    {0x760B01u, 4, {0x41, 0x8B, 0xBC, 0xCB}, ER_REC, 0xCu, 1},
    {0x7B3BE7u, 3, {0x8B, 0xB4, 0xCE}, ER_REC, 0xCu, 1},
    {0x8237C1u, 4, {0x41, 0x8B, 0xBC, 0xCA}, ER_REC, 0xCu, 1},
    {0x82396Du, 4, {0x41, 0x8B, 0x9C, 0xCA}, ER_REC, 0xCu, 1},
    {0x886FBAu, 3, {0x4D, 0x8D, 0x83}, ER_REC, 0xCu, 1},
    {0x88B361u, 3, {0x8B, 0x8C, 0xCF}, ER_REC, 0xCu, 1},
    {0x94A9E8u, 3, {0x8B, 0x9C, 0xC8}, ER_REC, 0xCu, 1},
    {0xA35BC1u, 3, {0x8B, 0x9C, 0xCB}, ER_REC, 0xCu, 1},
    {0xFD94A3u, 4, {0x44, 0x8B, 0x8C, 0xCB}, ER_REC, 0xCu, 1},
    {0xFD9963u, 4, {0x44, 0x8B, 0x8C, 0xCB}, ER_REC, 0xCu, 1},
    {0xFDD6D7u, 3, {0x48, 0x8D, 0x99}, ER_REC, 0xCu, 1},
    {0xFFE244u, 4, {0x41, 0x8B, 0x9C, 0xCD}, ER_REC, 0xCu, 1},
    {0x12995CCu, 4, {0x41, 0x8B, 0x8C, 0xC9}, ER_REC, 0xCu, 1},
    {0x13B9832u, 4, {0x41, 0x8B, 0x84, 0xD5}, ER_REC, 0xCu, 1},
    {0x13C1EE9u, 3, {0x8B, 0xBC, 0xC8}, ER_REC, 0xCu, 1},
    {0x6F41F4u, 3, {0x49, 0x8D, 0x85}, ER_REC, 0x0u, 1},
    {0xA394E2u, 3, {0x49, 0x8D, 0x95}, ER_REC, 0x0u, 1},
    {0xA3C304u, 4, {0x4C, 0x8D, 0x3C, 0xCD}, ER_REC, 0x0u, 1},
    {0xA3C384u, 4, {0x4C, 0x8D, 0x34, 0xCD}, ER_REC, 0x0u, 1},
    {0x13C85F2u, 3, {0x48, 0x8D, 0x85}, ER_REC, 0x0u, 1},
    {0x6065CFu, 3, {0x48, 0x8D, 0x85}, ER_REC, 0x8u, 1},
    {0xFDC05Cu, 4, {0x42, 0x39, 0x94, 0xC1}, ER_REC, 0x8u, 1},
    {0xFDC1FCu, 3, {0x39, 0x8C, 0xD6}, ER_REC, 0x8u, 1},
    {0xFDCEE8u, 2, {0x8B, 0x85}, ER_REC, 0x8u, 1},
    {0xFDD55Fu, 3, {0x4C, 0x8D, 0xA1}, ER_REC, 0x8u, 1},
    {0xFDA207u, 3, {0x48, 0x8D, 0x8B}, ER_REC, 0x10u, 1},
    {0xFDCF7Au, 2, {0x8B, 0xBD}, ER_REC, 0x10u, 1},
    {0xFDD23Au, 2, {0xF6, 0x85}, ER_REC, 0x14u, 1},
    {0x7C82CDu, 4, {0x4C, 0x8D, 0x1C, 0x45}, ER_REC, 0x0u, 8},
    {0x7C838Du, 4, {0x4C, 0x8D, 0x1C, 0x45}, ER_REC, 0x0u, 8},
    {0xA3C687u, 4, {0x4C, 0x8D, 0x24, 0x45}, ER_REC, 0x0u, 8},
    {0xA3C704u, 4, {0x4C, 0x8D, 0x2C, 0x45}, ER_REC, 0x0u, 8},
    {0xAB706Fu, 4, {0x48, 0x8D, 0x04, 0x4D}, ER_REC, 0x0u, 8},
    {0xBA0639u, 4, {0x48, 0x8D, 0x14, 0x45}, ER_REC, 0x0u, 8},
    {0xFDA0F9u, 4, {0x48, 0x8D, 0x14, 0x45}, ER_REC, 0x0u, 8},
    {0xFDBB09u, 4, {0x48, 0x8D, 0x14, 0x45}, ER_REC, 0x0u, 8},
    {0xFDBBF2u, 4, {0x48, 0x8D, 0x34, 0x45}, ER_REC, 0x0u, 8},
    {0xFDBF40u, 4, {0x48, 0x8D, 0x34, 0x45}, ER_REC, 0x0u, 8},
    {0xFDC1A4u, 4, {0x4E, 0x8D, 0x34, 0x7D}, ER_REC, 0x0u, 8},
    {0xFDC7CEu, 4, {0x48, 0x8D, 0x04, 0x4D}, ER_REC, 0x0u, 8},
    {0xFD9D1Cu, 3, {0x48, 0x8D, 0x80}, ER_REC, 0x0u, 8},
    {0xFD9DDBu, 3, {0x48, 0x8D, 0x80}, ER_REC, 0x0u, 8},
    {0x606F70u, 3, {0x48, 0x8D, 0x80}, ER_REC, 0x8u, 8},
    {0xA2511Cu, 3, {0x48, 0x8D, 0x80}, ER_REC, 0x8u, 8},
    {0xA2524Cu, 3, {0x48, 0x8D, 0x80}, ER_REC, 0x8u, 8},
    {0xFD96E0u, 3, {0x48, 0x8D, 0x80}, ER_REC, 0x8u, 8},
    {0xFD9C50u, 3, {0x48, 0x8D, 0x80}, ER_REC, 0x8u, 8},
    {0x10028CFu, 3, {0x48, 0x8D, 0x80}, ER_REC, 0x8u, 8},
    {0x13C84D0u, 3, {0x48, 0x8D, 0x80}, ER_REC, 0x8u, 8},
    {0xFD93D7u, 2, {0x48, 0x05}, ER_REC, 0x10u, 24},
    {0xFD961Bu, 2, {0x48, 0x05}, ER_REC, 0x10u, 24},
    {0xFD9ADBu, 2, {0x48, 0x05}, ER_REC, 0x10u, 24},
    {0xFD9B8Bu, 2, {0x48, 0x05}, ER_REC, 0x10u, 24},
    {0xAB4033u, 3, {0x48, 0x8D, 0x80}, ER_REC, 0xE49F0u, 8},
    {0xFDA1C4u, 1, {0xBA}, ER_COUNT_SITE, 0x0u, 1},
    {0xFDC20Du, 1, {0x3D}, ER_COUNT_SITE, 0x0u, 1},
    {0xFDC221u, 3, {0x41, 0x81, 0xF8}, ER_COUNT_SITE, 0x0u, 1},
    {0xFDC06Eu, 1, {0x3D}, ER_COUNT_SITE, 0x0u, 1},
    {0xFDC082u, 3, {0x41, 0x81, 0xF9}, ER_COUNT_SITE, 0x0u, 1},
    {0xFDD0A1u, 3, {0x41, 0x81, 0xFD}, ER_COUNT_SITE, 0x0u, 1},
    {0xFDD698u, 1, {0x3D}, ER_COUNT_SITE, 0x0u, 1},
    {0xFDD6DEu, 1, {0xBF}, ER_COUNT_SITE, 0x0u, 1},
    {0xAB5083u, 1, {0xBA}, ER_LIST, 0x0u, 1},
    {0xAB7DBFu, 1, {0xBA}, ER_LIST, 0x0u, 1},
    {0xFDC5D2u, 1, {0xBA}, ER_LIST, 0x0u, 1},
    {0xFDA22Fu, 3, {0x48, 0x8D, 0x83}, ER_HASH0, 0x0u, 1},
    {0xFDA249u, 3, {0x48, 0x8D, 0x83}, ER_HASH1, 0x0u, 1},
    {0xFDA2CDu, 3, {0x48, 0x8D, 0x83}, ER_HASH2, 0x0u, 1},
    {0xFDA23Fu, 6, {0xC7, 0x83, 0xB8, 0xAE, 0xF1, 0x00}, ER_PLACES_SITE, 0x0u, 1},
    {0xFDA26Au, 6, {0xC7, 0x83, 0xD0, 0x2E, 0xF2, 0x00}, ER_PLACES_SITE, 0x0u, 1},
    {0xFDA2C2u, 7, {0x48, 0xC7, 0x83, 0xE8, 0xAE, 0xF2, 0x00}, ER_PLACES_SITE, 0x0u, 1},
};
/* Where the records are, for the loader's own readers: the vanilla offset and count until they move. */
static int64_t g_entityRecordDisp = ER_VANILLA;
static uint32_t g_entityRecordCount = 0x800u;
static int g_entityRecordsState; /* 0 not installed, 1 waiting for the constructor, 2 moved, -1 left as they are */
typedef uint64_t (*EntityCtorFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static EntityCtorFn g_entityCtor;

/* Free address space of `size` bytes within 1.75 GB of `near`, above it first. */
static uintptr_t EntityRecordsAlloc(uintptr_t target, size_t size)
{
    const uintptr_t span = 0x70000000u, grain = 0x10000u;
    for (int up = 1; up >= 0; up--)
    {
        uintptr_t at = up ? (target + grain) & ~(grain - 1) : ((target - size) & ~(grain - 1));
        while (up ? at + size < target + span : at > target - span)
        {
            MEMORY_BASIC_INFORMATION m;
            if (VirtualQuery((void*)at, &m, sizeof m) == 0) break;
            uintptr_t end = (uintptr_t)m.BaseAddress + m.RegionSize;
            if (m.State == MEM_FREE && end - at >= size)
            {
                void* p = VirtualAlloc((void*)at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                if (p) return (uintptr_t)p;
            }
            if (up) at = (end + grain - 1) & ~(grain - 1);
            else
            {
                uintptr_t base = (uintptr_t)m.AllocationBase ? (uintptr_t)m.AllocationBase : (uintptr_t)m.BaseAddress;
                if (base < size + grain) break;
                at = (base - size) & ~(grain - 1);
            }
        }
    }
    return 0;
}

/* A site's value in the reviewed code. */
static uint32_t EntityRecordOld(size_t i)
{
    switch (kEntityRecordSites[i].kind)
    {
    case ER_REC: return (ER_VANILLA + kEntityRecordSites[i].add) / kEntityRecordSites[i].div;
    case ER_COUNT_SITE: return 0x800u;
    case ER_LIST: return 0x2000u;
    case ER_HASH0: return 0xF12EB0u;
    case ER_HASH1: return 0xF1AEC8u;
    case ER_HASH2: return 0xF22EE0u;
    default: return 0x1000u;
    }
}

static void EntityRecordsMove(uintptr_t game, uintptr_t state)
{
    uint64_t global = 1, b = 0;
    if (SafeRead(game + ER_STATE_GLOBAL, &global, 8) || global != 0 || SafeRead(game + ER_B_GLOBAL, &b, 8) || b + ER_B_STATE != state)
    { Note("entity records: the constructor's state is not the reviewed one; 2048 records as before"); return; }
    const size_t records = (size_t)ER_COUNT * 24u, hash = (size_t)ER_PLACES * 8u, size = 24u + records + 16u + 3u * hash;
    uintptr_t block = EntityRecordsAlloc(state, size);
    if (block == 0) { Note("entity records: no free memory within 2 GB of the entity state; 2048 records as before"); return; }
    int64_t r = ((int64_t)block - (int64_t)state) % 24;
    if (r < 0) r += 24;
    uintptr_t at = block + (uintptr_t)((8 - r + 24) % 24);
    int64_t d = (int64_t)at - (int64_t)state;
    uintptr_t h0 = (at + records + 15u) & ~(uintptr_t)15u;
    int64_t hd[3] = {(int64_t)h0 - (int64_t)state, (int64_t)(h0 + hash) - (int64_t)state, (int64_t)(h0 + 2u * hash) - (int64_t)state};
    const size_t n = sizeof kEntityRecordSites / sizeof kEntityRecordSites[0];
    for (size_t i = 0; i < n; i++)
    {
        uint8_t bytes[11];
        uint32_t old = EntityRecordOld(i), at4 = kEntityRecordSites[i].at;
        if (SafeRead(game + kEntityRecordSites[i].rva, bytes, at4 + 4u) || memcmp(bytes, kEntityRecordSites[i].pin, at4) != 0 || memcmp(bytes + at4, &old, 4) != 0)
        {
            VirtualFree((void*)block, 0, MEM_RELEASE);
            Note("entity records: the code at game+0x%X is not the reviewed one; 2048 records as before", kEntityRecordSites[i].rva);
            return;
        }
    }
    for (size_t i = 0; i < n; i++)
    {
        int64_t v = 0;
        switch (kEntityRecordSites[i].kind)
        {
        case ER_REC: v = (d + (int64_t)kEntityRecordSites[i].add) / kEntityRecordSites[i].div; break;
        case ER_COUNT_SITE: v = ER_COUNT; break;
        case ER_LIST: v = (int64_t)ER_COUNT * 4; break;
        case ER_HASH0: v = hd[0]; break;
        case ER_HASH1: v = hd[1]; break;
        case ER_HASH2: v = hd[2]; break;
        default: v = ER_PLACES; break;
        }
        int32_t value = (int32_t)v;
        if (ApiWrite(game + kEntityRecordSites[i].rva + kEntityRecordSites[i].at, &value, 4))
        {
            /* Part written: the sites before it go back (the game cannot run on a mix), and the block is left unused. */
            for (size_t j = 0; j < i; j++)
            {
                uint32_t old = EntityRecordOld(j);
                ApiWrite(game + kEntityRecordSites[j].rva + kEntityRecordSites[j].at, &old, 4);
            }
            Note("entity records: writing game+0x%X failed; the %u sites before it are back; 2048 records as before", kEntityRecordSites[i].rva, (unsigned)i);
            return;
        }
    }
    g_entityRecordDisp = d;
    g_entityRecordCount = ER_COUNT;
    g_entityRecordsState = 2;
    Note("entity records: %u records (was 2048) at state%+lld, hashes of %u places, removal list %u entries: %u code sites rewritten",
        ER_COUNT, (long long)d, ER_PLACES, ER_COUNT, (unsigned)n);
}

static uint64_t EntityCtorDetour(uint64_t state, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    static LONG once;
    if (InterlockedExchange(&once, 1) == 0)
    {
        uintptr_t game = (uintptr_t)GetModuleHandleW(L"game.dll");
        g_entityRecordsState = -1;
        if (game) EntityRecordsMove(game, (uintptr_t)state);
    }
    return g_entityCtor(state, a2, a3, a4, a5, a6);
}

/* Before the game's setup (the state global still 0): the constructor detour moves the records when it runs. */
static void EntityRecordsInstall(uintptr_t game)
{
    uint64_t global = 1;
    if (g_entityRecordsState != 0 || game == 0) return;
    g_entityRecordsState = -1;
    if (SafeRead(game + ER_STATE_GLOBAL, &global, 8) || global != 0)
    { Note("entity records: the entity state was already built; 2048 records as before"); return; }
    if (ApiCodeMatches(game + ER_CONSTRUCTOR, kEntityRecordCtorPin) != 1)
    { Note("entity records: the entity state's constructor is not the reviewed one; 2048 records as before"); return; }
    if (LoaderHook("entity records (extra large maps)", game + ER_CONSTRUCTOR, (void*)EntityCtorDetour, (void**)&g_entityCtor) != 0)
    { g_entityCtor = NULL; Note("entity records: the constructor could not be hooked; 2048 records as before"); return; }
    g_entityRecordsState = 1;
}

/* The most records live at once, noted at each new 1024 (from the loader's watch, every few seconds at most). */
static void EntityRecordsWatch(uintptr_t game)
{
    static ULONGLONG next;
    static uint32_t peak, noted;
    static uint8_t* copy;
    uint64_t state = 0;
    uint32_t none = 0;
    if (g_entityRecordsState != 2 || game == 0 || GetTickCount64() < next) return;
    next = GetTickCount64() + 5000;
    if (copy == NULL && (copy = (uint8_t*)malloc((size_t)ER_COUNT * 24u)) == NULL) return;
    if (SafeRead(game + ER_STATE_GLOBAL, &state, 8) || state == 0 || SafeRead(game + 0x348456Cu, &none, 4) ||
        SafeRead((uintptr_t)state + (uintptr_t)g_entityRecordDisp, copy, (size_t)ER_COUNT * 24u)) return;
    uint32_t live = 0;
    for (uint32_t i = 0; i < ER_COUNT; i++) live += *(const uint32_t*)(copy + 24u * i + 8u) != none;
    if (live > peak) peak = live;
    if (peak / 1024u > noted / 1024u || (noted == 0 && peak > 0))
    {
        noted = peak;
        Note("entity records: %u of %u live at most so far (vanilla holds 2048)", peak, ER_COUNT);
    }
}
