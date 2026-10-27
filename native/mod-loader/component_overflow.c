/* Overflow rows: more rows for a full component table than its compiled index holds (research content-limits\
 * component-tables). Every table's index is built half full, and the game compiles each one's slot count into its
 * lookups, so a table can never take more added entities than the base game has with that component (a Grenadier
 * Battlement copy runs out after 2, a Liberator copy after 162, a hellpod rack after 70). Included by
 * hellforge_modloader.c, after registry.c, before registry_load.c.
 *
 * A registered entity a full table has no row for gets one here instead: 16 bytes {entity, record, 0} like an index
 * row, so the record it names is read as from any row (table + slots * 16 + record * size: its base's record, or one of
 * its own in HellForge's copy of the table). HellForge's own lookups find it (FindRow asks OverflowFind on a miss). The
 * game's find it through the table's lookup helpers, which are hooked: the helper runs first as ever, and only when it
 * finds nothing a thunk asks OverflowRecord. Only tables whose every game lookup is a helper qualify (component_helpers.h,
 * 139 of 270): one with a lookup inlined somewhere would miss the row there (107, 188, 233, 271 among a weapon's).
 *
 * Rows live in one block made the first time a table runs out, never moved or freed, and a row is complete before the
 * index slot naming it is set, so the game's threads read them while registration adds more. Each component's rows
 * belong to the table that is its index now (g_overflowTable): when HellForge copies a table (OwnedCopy) its rows follow. */
#include "component_helpers.h"

#define OVF_COMPONENTS 330u                     /* table indices (the build has 270, numbered up to 323) */
#define OVF_MAX_ROWS (REG_MAX_WEAPONS * 64u)    /* each registered entity, in up to 64 full tables */
#define OVF_SLOTS (1u << 18)                    /* a power of two, at least twice OVF_MAX_ROWS */
_Static_assert(OVF_SLOTS >= 2u * OVF_MAX_ROWS && (OVF_SLOTS & (OVF_SLOTS - 1)) == 0, "overflow index size");
#define OVF_HELPERS (sizeof kComponentHelpers / sizeof kComponentHelpers[0])
#define OVF_THUNK 64u

typedef struct OverflowRow { uint32_t component; uint8_t row[16]; } OverflowRow;
static OverflowRow* g_overflowRows;
static volatile LONG* g_overflowIndex;                    /* row number + 1, or 0 */
static volatile LONG g_overflowCount;
static const uint8_t* volatile g_overflowTable[OVF_COMPONENTS]; /* the table each component's rows belong to */
static uint32_t g_overflowStride[OVF_COMPONENTS];
static uint32_t g_overflowSlots[OVF_COMPONENTS];
static int g_overflowHooked[OVF_COMPONENTS];              /* 1 hooked, -1 could not be */
static void* g_overflowOriginal[OVF_HELPERS];
static uint8_t* g_overflowThunks;

static uint32_t OverflowSlot(uint32_t component, uint64_t entity)
{
    uint64_t h = (entity ^ ((uint64_t)component << 40)) * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 46) & (OVF_SLOTS - 1);
}

/* The row of `entity` in component c's overflow, or NULL. */
static uint8_t* OverflowRowOf(uint32_t c, uint64_t entity)
{
    if (g_overflowIndex == NULL || entity == 0) return NULL;
    for (uint32_t at = OverflowSlot(c, entity), probes = 0; probes < OVF_SLOTS; probes++, at = (at + 1) & (OVF_SLOTS - 1))
    {
        LONG entry = g_overflowIndex[at];
        if (entry == 0) return NULL;
        OverflowRow* r = &g_overflowRows[entry - 1];
        uint64_t key;
        memcpy(&key, r->row, 8);
        if (r->component == c && key == entity) return r->row;
    }
    return NULL;
}

/* FindRow's miss: an overflow row of the table `rows` is, if it has any. */
static uint8_t* OverflowFind(const uint8_t* rows, uint64_t entity)
{
    if (g_overflowCount == 0 || rows == NULL) return NULL;
    for (uint32_t c = 0; c < OVF_COMPONENTS; c++)
        if (g_overflowTable[c] == rows) return OverflowRowOf(c, entity);
    return NULL;
}

/* A helper thunk's miss: the record of `entity` in component c through its overflow row, as the helper would return it
 * (table + slots * 16 + record * size), or 0. Runs on the game's threads, for every lookup a hooked helper misses. */
static uint8_t* OverflowRecord(uint64_t entity, uint32_t c)
{
    if (c >= OVF_COMPONENTS || g_overflowTable[c] == NULL || entity == 0 || g_game == 0) return NULL;
    uint8_t* state = *(uint8_t* volatile*)(g_game + REG_STATE_GLOBAL);
    uint8_t* table = state != NULL ? *(uint8_t* volatile*)(state + REG_TABLES + 8 * c) : NULL;
    if (table == NULL || table != g_overflowTable[c]) return NULL; /* not the table its rows were added to */
    uint8_t* row = OverflowRowOf(c, entity);
    if (row == NULL) return NULL;
    uint32_t record;
    memcpy(&record, row + 8, 4);
    return table + (size_t)g_overflowSlots[c] * 16 + (size_t)record * g_overflowStride[c];
}

/* HellForge copied component c's table (OwnedCopy): its rows belong to the copy now. */
static void OverflowMoved(uint32_t c, const uint8_t* from, const uint8_t* to)
{
    if (c < OVF_COMPONENTS && g_overflowTable[c] == from) g_overflowTable[c] = to;
}

/* The record size of component c's table, from the build's catalogue, or 0. */
static uint32_t OverflowStrideOf(uint32_t c)
{
    for (size_t i = 0; i < sizeof kComponentSizes / sizeof kComponentSizes[0]; i++)
        if (kComponentSizes[i].component == c) return kComponentSizes[i].size;
    return 0;
}

/* Whether a full table of component c can take overflow rows: the game reads it only through helpers, and its record
 * size is known. */
static int OverflowCapable(uint32_t c)
{
    if (c >= OVF_COMPONENTS || g_overflowHooked[c] < 0 || OverflowStrideOf(c) == 0) return 0;
    for (size_t i = 0; i < OVF_HELPERS; i++) if (kComponentHelpers[i].component == c) return 1;
    return 0;
}

/* A thunk for helper i: the helper itself (through its original), and on a miss OverflowRecord(key, c). Get(&key)'s
 * key is read through its pointer (a null pointer, or a null key, finds nothing either way). */
static uint8_t* OverflowThunk(size_t i, uint8_t* at)
{
    static const uint8_t head[9] = {0x48, 0x89, 0x4C, 0x24, 0x08,   /* mov [rsp+8], rcx (the caller's home slot) */
        0x48, 0x83, 0xEC, 0x28};                                    /* sub rsp, 0x28 */
    static const uint8_t call[16] = {0xFF, 0x10,                    /* call [rax] (the original) */
        0x48, 0x83, 0xC4, 0x28,                                     /* add rsp, 0x28 */
        0x48, 0x85, 0xC0,                                           /* test rax, rax */
        0x75, 0x00,                                                 /* jnz done (set below) */
        0x48, 0x8B, 0x4C, 0x24, 0x08};                              /* mov rcx, [rsp+8] */
    static const uint8_t byPointer[8] = {0x48, 0x85, 0xC9,          /* test rcx, rcx */
        0x74, 0x00,                                                 /* jz done (set below) */
        0x48, 0x8B, 0x09};                                          /* mov rcx, [rcx] */
    uint8_t* p = at;
    memcpy(p, head, sizeof head); p += sizeof head;
    *p++ = 0x48; *p++ = 0xB8; uint64_t original = (uint64_t)(uintptr_t)&g_overflowOriginal[i]; memcpy(p, &original, 8); p += 8;
    memcpy(p, call, sizeof call);
    uint8_t* jnz = p + 10;
    p += sizeof call;
    uint8_t* jz = NULL;
    if (kComponentHelpers[i].byPointer) { memcpy(p, byPointer, sizeof byPointer); jz = p + 4; p += sizeof byPointer; }
    *p++ = 0xBA; uint32_t c = kComponentHelpers[i].component; memcpy(p, &c, 4); p += 4;           /* mov edx, c */
    *p++ = 0x48; *p++ = 0xB8; uint64_t miss = (uint64_t)(uintptr_t)OverflowRecord; memcpy(p, &miss, 8); p += 8;
    *p++ = 0xFF; *p++ = 0xE0;                                                                    /* jmp rax */
    *jnz = (uint8_t)(p - (jnz + 1));
    if (jz != NULL) *jz = (uint8_t)(p - (jz + 1));
    *p++ = 0xC3;                                                                                 /* done: ret */
    return p;
}

/* Component c's helpers hooked (once): 1, or 0 when one is not the reviewed one or could not be hooked (logged; no
 * overflow for it then). The thunks are written before any helper is sent to them. */
static int OverflowHook(uint32_t c)
{
    if (g_overflowHooked[c] != 0) return g_overflowHooked[c] > 0;
    g_overflowHooked[c] = -1;
    if (g_overflowThunks == NULL)
    {
        g_overflowThunks = (uint8_t*)VirtualAlloc(NULL, OVF_HELPERS * OVF_THUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_overflowThunks == NULL) return 0;
        for (size_t i = 0; i < OVF_HELPERS; i++) OverflowThunk(i, g_overflowThunks + i * OVF_THUNK);
        DWORD protect;
        if (!VirtualProtect(g_overflowThunks, OVF_HELPERS * OVF_THUNK, PAGE_EXECUTE_READ, &protect)) return 0;
        FlushInstructionCache(GetCurrentProcess(), g_overflowThunks, OVF_HELPERS * OVF_THUNK);
    }
    uint8_t code[16];
    for (size_t i = 0; i < OVF_HELPERS; i++)
        if (kComponentHelpers[i].component == c &&
            (SafeRead(g_game + kComponentHelpers[i].rva, code, 16) || memcmp(code, kComponentHelpers[i].pin, 16) != 0))
        {
            Note("component overflow: table %u's lookup at game.dll+0x%x is not the reviewed one; it cannot grow", c, kComponentHelpers[i].rva);
            return 0;
        }
    int hooked = 0;
    for (size_t i = 0; i < OVF_HELPERS; i++)
    {
        if (kComponentHelpers[i].component != c) continue;
        if (LoaderHook("component overflow", g_game + kComponentHelpers[i].rva, g_overflowThunks + i * OVF_THUNK, &g_overflowOriginal[i]))
        {
            Note("component overflow: table %u's lookup at game.dll+0x%x could not be hooked; it cannot grow", c, kComponentHelpers[i].rva);
            return 0; /* the ones hooked already only ever find what they found before */
        }
        hooked++;
    }
    g_overflowHooked[c] = 1;
    Note("component overflow: table %u takes rows past its %u slots (%d lookup(s) hooked)", c, g_overflowSlots[c], hooked);
    return 1;
}

/* Whether component c's full table `table` can take `rows` more overflow rows now: it qualifies, its lookups are hooked
 * (done here, the first time), and rows are left. Asked before an entity's rows are written (ComponentsUnderstood), so
 * it is added whole or not at all. */
static int OverflowReady(uint32_t c, const uint8_t* table, uint32_t rows)
{
    if (!OverflowCapable(c) || table == NULL) return 0;
    if (g_overflowTable[c] != NULL && g_overflowTable[c] != table) return 0; /* rows of another load of the table */
    if ((uint32_t)g_overflowCount + rows > OVF_MAX_ROWS) return 0;
    g_overflowStride[c] = OverflowStrideOf(c);
    g_overflowSlots[c] = SlotsOf(c);
    return OverflowHook(c);
}

/* A new overflow row for `entity` in component c's table `table` (its index now), its record 0 until the caller sets
 * it; NULL when it cannot have one. */
static uint8_t* OverflowAdd(const uint8_t* table, uint32_t c, uint64_t entity)
{
    if (entity == 0 || !OverflowReady(c, table, 1)) return NULL;
    if (g_overflowRows == NULL)
    {
        g_overflowRows = (OverflowRow*)VirtualAlloc(NULL, (size_t)OVF_MAX_ROWS * sizeof(OverflowRow), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_overflowIndex = g_overflowRows != NULL ? (volatile LONG*)VirtualAlloc(NULL, (size_t)OVF_SLOTS * sizeof(LONG), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : NULL;
        if (g_overflowIndex == NULL) { Note("component overflow: out of memory"); return NULL; }
    }
    if (OverflowRowOf(c, entity) != NULL) return NULL;
    LONG n = g_overflowCount;
    OverflowRow* r = &g_overflowRows[n];
    r->component = c;
    memset(r->row, 0, 16);
    memcpy(r->row, &entity, 8);
    uint32_t at = OverflowSlot(c, entity);
    while (g_overflowIndex[at] != 0) at = (at + 1) & (OVF_SLOTS - 1);
    g_overflowTable[c] = table;
    InterlockedExchange(&g_overflowCount, n + 1);
    InterlockedExchange(&g_overflowIndex[at], n + 1); /* the row, then the slot naming it */
    return r->row;
}

/* The entity that owns record `record` among component c's overflow rows (LoadoutOwner's rows past the index), or 0. */
static uint64_t OverflowOwner(uint32_t c, const uint8_t* table, uint32_t record)
{
    if (c >= OVF_COMPONENTS || g_overflowTable[c] != table) return 0;
    for (LONG i = 0; i < g_overflowCount; i++)
    {
        uint32_t r;
        memcpy(&r, g_overflowRows[i].row + 8, 4);
        uint64_t entity;
        memcpy(&entity, g_overflowRows[i].row, 8);
        if (g_overflowRows[i].component == c && r == record && !PlainCopy(entity)) return entity;
    }
    return 0;
}
