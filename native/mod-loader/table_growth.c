/* Growing the game's entity component tables (a design by CowboyBingus:
 * https://github.com/SkyeShade/HellForge/pull/79#issuecomment-6424684734). A component table is an index of S rows
 * {u64 entity, u32 record, u32 0}, probed from entity mod S to the first empty row, then its records; S is compiled into
 * every lookup of it (a probe bound, a wrap, a modulus by multiply or mask, the records' place after the index, the
 * default record's address). When the entity data loads and a table has too few free rows for the entities mods add,
 * HellForge builds it again at a larger S', rewrites each of those constants at every site (each checked against the
 * reviewed bytes first, with the game's other threads paused) and points the game at the new table. A table grows only
 * when every one of its sites is known (table_growth_sites.h, generated from the scan in research\table-growth);
 * otherwise its entities are refused, as before. This file: the arithmetic, the checked patch, the rebuild and the
 * choice of S'. Work in progress: included by the tests (stats_test.c) until registration uses it, then by
 * hellforge_modloader.c after registry_load.c. */

/* How a constant's new value follows from the new slot count (table_growth_sites.h GrowthConstant.formula). */
enum
{
    GROW_S = 1,              /* the probe bound */
    GROW_S_MINUS_1,          /* the wrap compare */
    GROW_MASK,               /* S - 1 as an and-mask (S a power of two) */
    GROW_INDEX_BYTES,        /* 16 * S: where the records start, in bytes */
    GROW_INDEX_RECORDS,      /* 16 * S / stride: where the records start, in records */
    GROW_DEFAULT_OFFSET,     /* 16 * S + (R - 1) * stride: the default record (the last vanilla one) */
    GROW_RECORDS,            /* a record count, unchanged */
    GROW_MAGIC,              /* the u64 magic of x / S (mul-high, then shift): Magic64 */
    GROW_SHIFT,              /* its shift */
    GROW_MAGIC65,            /* the 65-bit form's magic of x / S: Magic65 */
    GROW_SHIFT65,            /* its final shift */
};

/* The u64 magic number of division by d: x / d == (x * m) >> (64 + shift) for every 64-bit x, with m below 2^64 and the
 * smallest shift that allows it (Granlund-Montgomery: m = ceil(2^(64+s) / d) works when m * d - 2^(64+s) <= 2^s). 0 when
 * d needs a 65-bit magic (the compiler's other form) or is 0. */
static int Magic64(uint64_t d, uint64_t* magic, uint32_t* shift)
{
    if (d < 2) return 0;
    for (uint32_t s = 0; s < 64; s++)
    {
        unsigned __int128 power = (unsigned __int128)1 << (64 + s);
        unsigned __int128 m = (power + d - 1) / d;
        if (m >> 64) return 0; /* past 64 bits for this s, and for every larger one */
        if (m * d - power <= ((unsigned __int128)1 << s)) { *magic = (uint64_t)m; *shift = s; return 1; }
    }
    return 0;
}

/* The compiler's other form, for a divisor whose magic needs 65 bits: hi = (x * m) >> 64, then
 * ((x - hi) >> 1) + hi, shifted right by `shift` (game.dll+0x4FC706..0x4FC71F for table 107, 712 slots: m 0x702E05C0B81702E1,
 * shift 9). With l = ceil(log2 d): m = ceil(2^(64+l) / d) - 2^64 and shift = l - 1. It divides exactly for every d from 2,
 * so a table on this form can take any slot count. */
static int Magic65(uint64_t d, uint64_t* magic, uint32_t* shift)
{
    if (d < 2) return 0;
    uint32_t l = 0;
    while (l < 64 && ((uint64_t)1 << l) < d) l++;
    unsigned __int128 power = (unsigned __int128)1 << (64 + l);
    unsigned __int128 m = (power + d - 1) / d;
    *magic = (uint64_t)(m - ((unsigned __int128)1 << 64));
    *shift = l - 1;
    return 1;
}

/* x / d as the 65-bit form computes it. */
static uint64_t Divide65(uint64_t x, uint64_t magic, uint32_t shift)
{
    uint64_t hi = (uint64_t)(((unsigned __int128)x * magic) >> 64);
    return (((x - hi) >> 1) + hi) >> shift;
}

/* The value a constant takes for a table of `slots` slots; 0 when the formula has none (stated by *ok). */
static uint64_t GrowthValue(uint8_t formula, uint32_t slots, uint32_t stride, uint32_t records, int* ok)
{
    uint64_t magic = 0;
    uint32_t shift = 0;
    *ok = 1;
    switch (formula)
    {
    case GROW_S: return slots;
    case GROW_S_MINUS_1: return slots - 1u;
    case GROW_MASK: *ok = slots != 0 && (slots & (slots - 1)) == 0; return slots - 1u;
    case GROW_INDEX_BYTES: return 16ull * slots;
    case GROW_INDEX_RECORDS: *ok = stride != 0 && 16ull * slots % stride == 0; return stride != 0 ? 16ull * slots / stride : 0;
    case GROW_DEFAULT_OFFSET: *ok = records != 0; return 16ull * slots + (uint64_t)(records - 1u) * stride;
    case GROW_RECORDS: return records;
    case GROW_MAGIC: *ok = Magic64(slots, &magic, &shift); return magic;
    case GROW_SHIFT: *ok = Magic64(slots, &magic, &shift); return shift;
    case GROW_MAGIC65: *ok = Magic65(slots, &magic, &shift); return magic;
    case GROW_SHIFT65: *ok = Magic65(slots, &magic, &shift); return shift;
    default: *ok = 0; return 0;
    }
}

/* Whether `value` fits an immediate of `size` bytes as the instruction reads it: imm8 and imm32 are sign-extended into
 * 64-bit operations (and compares), so a u32 compare against an imm8 reaches 127 and an imm32 0x7FFFFFFF; imm64 holds
 * anything. */
static int ImmediateFits(uint64_t value, uint8_t size)
{
    return size == 8 || (size == 4 && value <= 0x7FFFFFFFull) || (size == 1 && value <= 0x7Full);
}

/* One rewrite: the instruction at `at` (its reviewed `length` bytes in `original`), its immediate at `immOffset`, of
 * `immSize` bytes, to become `value`. */
typedef struct GrowthPatch
{
    uintptr_t at;
    uint8_t length, immOffset, immSize;
    uint8_t original[15];
    uint64_t value;
} GrowthPatch;

/* Pauses every thread of this process but this one (up to `capacity`, their handles into `paused`); how many. */
static int PauseOtherThreads(HANDLE* paused, int capacity)
{
    int count = 0;
    DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 entry = {sizeof entry};
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        if (count == capacity) { count = -1 - count; break; } /* too many to hold: undone by the caller */
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
        if (thread == NULL) continue; /* gone since the snapshot */
        if (SuspendThread(thread) == (DWORD)-1) { CloseHandle(thread); continue; }
        paused[count++] = thread;
    }
    CloseHandle(snapshot);
    return count;
}

static void ResumeThreads(HANDLE* paused, int count)
{
    for (int i = 0; i < count; i++) { ResumeThread(paused[i]); CloseHandle(paused[i]); }
}

/* Whether a paused thread stands inside one of the instructions (it would resume on half an old instruction). */
static int ThreadInside(HANDLE thread, const GrowthPatch* patches, int count)
{
    CONTEXT context = {0};
    context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(thread, &context)) return 1; /* unknown: as inside */
    for (int i = 0; i < count; i++)
        if (context.Rip > patches[i].at && context.Rip < patches[i].at + patches[i].length) return 1;
    return 0;
}

#define GROWTH_MAX_THREADS 512

/* A write while the other threads are paused: system calls and a copy only. A paused thread may hold any user lock (the
 * heap's, HellForge's rewrite list), so nothing here may take one; ApplyGrowthPatches records the rewrites (NoteRewrite)
 * once they run again. */
static int PausedWrite(uintptr_t address, const void* data, size_t size)
{
    DWORD previous;
    if (!VirtualProtect((void*)address, size, PAGE_EXECUTE_READWRITE, &previous)) return 1;
    for (size_t i = 0; i < size; i++) ((volatile uint8_t*)address)[i] = ((const uint8_t*)data)[i];
    VirtualProtect((void*)address, size, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), (void*)address, size);
    return 0;
}

/* Every rewrite, or none: each instruction must hold its reviewed bytes and each value fit its immediate (1, nothing
 * written, otherwise). Done with the other threads paused, none of them inside a rewritten instruction (2 when one is:
 * try again later), and in the same pause the table pointer at `slot` (when not 0) becomes `table`: no game thread sees
 * the new constants with the old table or the old ones with the new. 0 when written. */
/* Whether every instruction holds its reviewed bytes and every value fits its immediate. */
static int GrowthPatchesReviewed(const GrowthPatch* patches, int count)
{
    for (int i = 0; i < count; i++)
    {
        uint8_t now[15];
        const GrowthPatch* p = &patches[i];
        if (p->length == 0 || p->length > 15 || p->immOffset + p->immSize > p->length || !ImmediateFits(p->value, p->immSize) ||
            SafeRead(p->at, now, p->length) || memcmp(now, p->original, p->length) != 0)
            return 0;
    }
    return 1;
}

/* The rewrites and the pointer, the other threads paused: all, or (undone) none; 1 when all were written. */
static int WriteGrowthPaused(const GrowthPatch* patches, int count, uintptr_t slot, const uint8_t* table)
{
    int written = 0;
    while (written < count && !PausedWrite(patches[written].at + patches[written].immOffset, &patches[written].value, patches[written].immSize))
        written++;
    if (written < count) /* back to the reviewed bytes, before anything runs them half rewritten */
    {
        for (int i = 0; i < written; i++) PausedWrite(patches[i].at, patches[i].original, patches[i].length);
        return 0;
    }
    if (slot != 0) *(const uint8_t* volatile*)slot = table; /* the entity state's table pointer: plain data, as the game writes it */
    return 1;
}

static int ApplyGrowthPatches(const GrowthPatch* patches, int count, uintptr_t slot, const uint8_t* table)
{
    if (!GrowthPatchesReviewed(patches, count)) return 1;
    static HANDLE paused[GROWTH_MAX_THREADS];
    int held = PauseOtherThreads(paused, GROWTH_MAX_THREADS);
    if (held < 0) { ResumeThreads(paused, held == -1 ? 0 : -1 - held); return 1; }
    for (int t = 0; t < held; t++)
        if (ThreadInside(paused[t], patches, count)) { ResumeThreads(paused, held); return 2; }
    int done = WriteGrowthPaused(patches, count, slot, table);
    ResumeThreads(paused, held);
    if (!done) return 1;
    for (int i = 0; i < count; i++) NoteRewrite(patches[i].at + patches[i].immOffset, patches[i].immSize);
    return 0;
}

/* A table built again at `slots` slots from `table` (its index pointer; `oldSlots` rows, then `records` records of
 * `stride` bytes after them), with room for `spare` more records: the 32-byte header, every row placed again with its
 * record number kept, the records, the header's size of what follows. NULL (nothing allocated) when a row has nowhere
 * to go. Returns the new index. */
static uint8_t* RebuildTable(const uint8_t* table, uint32_t oldSlots, uint32_t slots, uint32_t stride, uint32_t records, uint32_t spare)
{
    size_t indexBytes = (size_t)slots * 16, recordBytes = (size_t)(records + spare) * stride;
    uint8_t* block = (uint8_t*)VirtualAlloc(NULL, 32 + indexBytes + recordBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (block == NULL) return NULL;
    uint8_t* index = block + 32;
    memcpy(block, table - 32, 32);
    uint32_t size = (uint32_t)(indexBytes + recordBytes);
    memcpy(block + 32 - 12, &size, 4);
    for (uint32_t i = 0; i < oldSlots; i++)
    {
        uint64_t key;
        memcpy(&key, table + (size_t)i * 16, 8);
        if (key == 0) continue;
        uint8_t* row = FreeRow(index, slots, 16, key);
        if (row == NULL) { VirtualFree(block, 0, MEM_RELEASE); return NULL; }
        memcpy(row, table + (size_t)i * 16, 16);
    }
    memcpy(index + indexBytes, table + (size_t)oldSlots * 16, (size_t)records * stride);
    return index;
}

/* What a table's sites allow for its slot count (table_growth_sites.h GrowthTable). */
typedef struct GrowthRules
{
    uint32_t maxSlots;   /* the largest every immediate holds */
    uint32_t multipleOf; /* 16 * S / stride must be whole (1: any) */
    uint8_t pow2;        /* a mask modulus: a power of two */
    uint8_t magic64;     /* a u64 magic modulus: S must have one (Magic64) */
} GrowthRules;

/* The slot count for `rows` rows: at least twice as many, the smallest the rules allow, at most their maximum; 0 when none
 * does. */
static uint32_t GrowthSlots(uint32_t rows, const GrowthRules* rules)
{
    uint64_t want = 2ull * rows;
    if (want < 2) want = 2;
    for (uint64_t s = want; s <= rules->maxSlots; s++)
    {
        if (rules->pow2 && (s & (s - 1)) != 0) continue;
        if (rules->multipleOf > 1 && s % rules->multipleOf != 0) continue;
        uint64_t magic;
        uint32_t shift;
        if (rules->magic64 && !Magic64(s, &magic, &shift)) continue;
        return (uint32_t)s;
    }
    return 0;
}
