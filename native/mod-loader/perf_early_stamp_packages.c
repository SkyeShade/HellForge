/* A mission's stamp packages requested early, build 25480438 (docs/performance.md, early-stamp-packages). Op 0x1F
 * (QueueStampPackages, game 0xF405E0) requests them and op 0x20 waits until all are in; the engine loads one package
 * at a time. The list depends only on the location types that earlier ops write, so the same function runs early, on
 * a main-thread step after the last of those ops, and the packages load during the stamp planning. Included by
 * perf_all.c after perf_scatter_cameras.c (kScDecodePin): AllocateNearGame (damage.c), PinMatches (packages.c),
 * JournalPut (live_code.c), PerfFixOn, SafeRead, Note, g_build, g_fired. */

#define ESP_STEP_CALL 0xF740A9u /* call 0xF26140: a generating level's main-thread step (rbx the level, rcx its records) */
#define ESP_END_CALL 0xF7444Au  /* call 0xF6FF60: the level's generation is freed (rcx the level) */
#define ESP_STEP 0xF26140u
#define ESP_END 0xF6FF60u
#define ESP_QUEUE 0xF405E0u     /* (pass, record, context): the list into [pass+0x30], its count into [[pass+0x38]] */
#define ESP_API 0x3326308u      /* game: the engine API; [+0x10] is the package table */
#define ESP_LEVEL_RECORDS 0x2A8u
#define ESP_RECORD 0x19B0u
#define ESP_RECORDS_MAX 256u    /* the block holds 0x19B000 bytes of records */
#define ESP_COUNT 0x19B000u
#define ESP_INDEX 0x19B004u
#define ESP_CONTEXT 0x19B008u
#define ESP_CONTEXT_SIZE 0xE8u
#define ESP_PASS 8u
#define ESP_PASS_END 0x1978u    /* a record's pass ends where its name and timing fields begin */
#define ESP_PASS_COPY 0x48u     /* op 0x1F's pass: types +0, &count +0x10, list +0x30, &list count +0x38 */
#define ESP_TYPES_MAX 1024u
#define ESP_PER_TYPE 8u         /* a location type yields at most six packages (one per lookup); the game's set sizes 8 */
#define ESP_ROOM 480u           /* of the engine's 512-entry load ring, which has no full check */
#define ESP_LIST_MAX 8192u

typedef struct EspPin { int game; uint32_t rva; uint16_t size; const char* what; const uint8_t* bytes; } EspPin;
typedef struct EspSlot { uint32_t slot, rva; } EspSlot;

static const uint8_t kEspIdlePin[] = {0x48,0x8b,0x8b,0x18,0xf7,0x00,0x00,0x0f,0xb6,0x51,0x08,0x84,0xd2,0x0f,0x85,0x25,0x01,0x00,0x00,
    0x48,0x8b,0x8b,0xa8,0x02,0x00,0x00,0x0f,0xb6,0x91,0xea,0xb0,0x19,0x00,0x48,0x8b,0x8b,0xa8,0x02,0x00,0x00,0x84,0xd2,0x0f,0x84,0xd4,
    0x00,0x00,0x00};
static const uint8_t kEspStepPin[] = {0x8b,0x91,0x04,0xb0,0x19,0x00,0x48,0x69,0xc2,0xb0,0x19,0x00,0x00,0xf6,0x84,0x08,0x98,0x19,0x00,
    0x00,0x02,0x74,0x09,0x33,0xd2,0xe8,0x92,0x20,0xfb,0xff,0xeb,0x14};
static const uint8_t kEspEndPin[] = {0x0f,0x85,0xf5,0x00,0x00,0x00,0x48,0x8b,0x83,0x38,0x02,0x00,0x00,0x44,0x39,0x60,0x10,0x0f,0x85,
    0xe4,0x00,0x00,0x00,0x48,0x8b,0xcb,0xe8,0x11,0xbb,0xff,0xff,0x44,0x89,0xa3,0xe4,0x01,0x00,0x00,0xe9,0xd0,0x00,0x00,0x00};
static const uint8_t kEspGenPin[] = {0x48,0x8b,0xa9,0xc8,0x02,0x00,0x00};
static const uint8_t kEspUnloadPin[] = {0x48,0x8b,0x05,0xa9,0x57,0x3b,0x02,0x8b,0x95,0xf4,0x70,0x1a,0x01,0x48,0x8b,0x48,0x10,0x48,0x8b,
    0x81,0x08,0x03,0x00,0x00,0x48,0x8d,0x8d,0x20,0x72,0x75,0x00,0xff,0xd0};
static const uint8_t kEspDispatchPin[] = {0x4c,0x8d,0xb1,0x04,0xb0,0x19,0x00,0x41,0x8b,0x36,0x44,0x8b,0xe2,0x48,0x8b,0xf9,0xff,0x10,
    0x44,0x8b,0x8f,0x00,0xb0,0x19,0x00,0x48,0x8d,0xaf,0xf0,0xb0,0x19,0x00,0x4d,0x8b,0xc6,0x44,0x89,0x64,0x24,0x28,0x48,0x8d,0x97,0x08,
    0xb0,0x19,0x00,0x48,0x89,0x6c,0x24,0x20,0x48,0x8b,0xcf,0x48,0x8b,0xd8,0xe8,0xac,0xe7,0xce,0xff};
/* The dispatcher's index, record and context reads: 58 bytes of scatter-cameras' pin, 13 bytes in. */
#define ESP_RECORD_AT 0xC149A7u
#define ESP_RECORD_PIN (kScDecodePin + (ESP_RECORD_AT - SC_DECODE))
#define ESP_RECORD_PIN_SIZE 58u
static const uint8_t kEspCase1FAt[] = {0xe7,0x54,0xc1,0x00};
static const uint8_t kEspCase2BAt[] = {0xe0,0x57,0xc1,0x00};
static const uint8_t kEspCase1FPin[] = {0x48,0x8d,0x4e,0x08,0x4d,0x8b,0xc7,0xe8,0xcd,0xb0,0x32,0x00,0xe9,0x04,0x49,0x00,0x00};
static const uint8_t kEspCase2BPin[] = {0x48,0x8d,0x4e,0x08,0x4d,0x8b,0xc7,0x48,0x8b,0xd6,0xe8,0xb1,0xf4,0x31,0x00,0xe9,0x08,0x46,0x00,
    0x00};
static const uint8_t kEspPlanPin[] = {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,
    0x41,0x57,0x48,0x83,0xec,0x60,0x48,0x8b,0x81,0x70,0x01,0x00,0x00,0x4d,0x8b,0xe0,0xbf,0x08,0x00,0x00,0x00,0x4c,0x8b,0xfa,0x44,0x8b,
    0xc7,0x8b,0x28,0x48,0x8b,0x81,0x70,0x02,0x00,0x00,0x8d,0x57,0x38,0x44,0x8b,0x28,0x48,0x8b,0x81,0x48,0x02,0x00,0x00,0x48,0x89,0x44,
    0x24,0x48,0x48,0x8b,0x81,0x90,0x02};
static const uint8_t kEspQueuePin[] = {0x48,0x8b,0xc4,0x4c,0x89,0x40,0x18,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x53,0x57,0x41,
    0x57,0x48,0x8d,0x68,0xa1,0x48,0x81,0xec,0x88,0x00,0x00,0x00,0x48,0x8b,0x79,0x30,0x48,0x89,0x70,0xd8,0x4c,0x89,0x60,0xd0,0x4c,0x89,
    0x70,0xc0,0x4d,0x8b,0xf0,0x48,0x8b,0x41,0x10,0x48,0x89,0x7d,0xd7,0x44,0x8b,0x20,0x48,0x8b,0x01,0x48,0x89,0x45,0xf7,0x48,0x8b,0x41,
    0x38,0x44,0x89,0x65,0x7f,0x44,0x8b,0x38,0x48,0x8b,0x41,0x08,0x48,0x8b,0x10,0x49,0x8b,0x40,0x18,0x44,0x8b,0x8a,0xd0,0x02,0x00,0x00,
    0x33,0xd2,0x4c,0x8b,0x80,0xd0,0x02,0x00,0x00};
static const uint8_t kEspQueueAllocPin[] = {0xd1,0xeb,0x0b,0xd8,0xff,0xc3,0x8b,0xd3,0x48,0xc1,0xe2,0x03,0xe8,0xb9,0xb1,0x7f,0x00,0x4c,
    0x8b,0xc0,0x48,0x89,0x45,0xff};
static const uint8_t kEspQueueFactionPin[] = {0x4c,0x8b,0x5d,0xdf,0x33,0xc9,0x41,0x8b,0x96,0xd0,0x00,0x00,0x00};
static const uint8_t kEspQueueContextPin[] = {0x4c,0x8b,0x75,0x77};
static const uint8_t kEspQueueRequestPin[] = {0x48,0x8b,0x45,0x67,0x4c,0x8b,0x74,0x24,0x68,0x4c,0x8b,0x64,0x24,0x78,0x48,0x8b,0xb4,
    0x24,0x80,0x00,0x00,0x00,0x48,0x8b,0x40,0x38,0x44,0x89,0x38,0x45,0x85,0xff,0x74,0x29,0x41,0x8b,0xdf,0x0f,0x1f,0x84,0x00,0x00,0x00,
    0x00,0x00,0x48,0x8b,0x05,0xf1,0x54,0x3e,0x02,0x48,0x8b,0x0f,0x48,0x8b,0x50,0x10,0xff,0x92,0xf0,0x02,0x00,0x00,0x48,0x8d,0x7f,0x08,
    0x48,0x83,0xeb,0x01,0x75,0xe2,0x48,0x81,0xc4,0x88,0x00,0x00,0x00,0x41,0x5f,0x5f,0x5b,0x5d,0xc3};
static const uint8_t kEspArenaPin[] = {0x40,0x56,0x41,0x56,0x48,0x83,0xec,0x38,0x4d,0x8b,0xf0,0x48,0x8b,0xf1,0x48,0x85,0xd2,0x0f,0x84,
    0x61,0x01,0x00,0x00,0x48,0x89,0x6c,0x24,0x60,0x48,0x8d,0x6a,0xff,0x49,0x03,0xe8,0x4c};
static const uint8_t kEspArenaChunkPin[] = {0x44,0x39,0x7e,0x0c,0x7c,0x0d,0x48,0x8b,0x05,0xbb,0xaa,0xbe,0x01,0xff,0x90,0xd8,0x00,0x00,
    0x00,0x48,0x8b,0x0e,0x4c,0x8b,0x01,0x48,0x8b,0x41,0x10,0x49,0x8d,0x14,0x28,0x48,0x3b,0xd0,0x77,0x1a,0x49,0x8b,0xc0,0xf0,0x48,0x0f,
    0xb1,0x11};
static const uint8_t kEspLoadPin[] = {0x48,0x8b,0xd1,0x48,0x8b,0x0d,0x2e,0xf4,0x6e,0x01,0x48,0x8b,0x89,0x00,0x04,0x00,0x00,0xe9,0x8a,
    0x28,0x2e,0x00};
static const uint8_t kEspLoadQueuePin[] = {0x48,0x8b,0xd3,0x49,0x8b,0xcd,0xe8,0xaf,0xb0,0x00,0x00,0x84,0xc0,0x74,0x22,0x48,0x8d,0x4c,
    0x24,0x58,0xe8,0xa1,0xf8,0xfb,0xff,0x48,0x8b,0xd0,0x45,0x84,0xe4,0x0f,0x84,0xbd,0x00,0x00,0x00,0x48,0x8d,0x0d,0x6e,0x95,0x08,0x01,
    0xe8,0x69,0xdf,0xfb,0xff,0x41,0x8b,0x8e,0x20,0x32,0x00,0x00,0x8b,0xd1,0x8d,0x41,0x01,0x25,0xff,0x01,0x00,0x00,0x41,0x89,0x86,0x20,
    0x32,0x00,0x00,0x48,0x8d,0x04,0x49,0x49,0x8d,0x0c,0xc6,0x48,0x8d,0x04,0x52,0x48,0x89,0x99,0x18,0x02,0x00,0x00,0x41,0xc6,0x84,0xc6,
    0x28,0x02,0x00,0x00,0x01,0x48,0x89,0xa9,0x20,0x02,0x00,0x00};
static const uint8_t kEspReleasePin[] = {0x85,0xd2,0x0f,0x84,0x38,0x02,0x00,0x00,0x41,0x57,0x48,0x83,0xec,0x40,0x48,0x89,0x5c,0x24,
    0x50,0x4c,0x8b,0xf9,0x48,0x89,0x6c,0x24,0x58,0x48,0x89,0x74,0x24,0x38,0x48,0x89,0x7c,0x24,0x30,0x4c,0x89,0x64,0x24,0x28,0x4c,0x89,
    0x74,0x24,0x20,0x44,0x8b,0xe2,0x0f,0x1f,0x40,0x00,0x66,0x66,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x48,0x8b,0x05,0x71,0xf1,0x6e,
    0x01,0x49};
static const uint8_t kEspLoadedPin[] = {0x48,0x83,0xec,0x28,0x48,0x8b,0x05,0xcd,0xee,0x6e,0x01,0x48,0x8b,0xd1,0x48,0x8b,0x88,0x00,0x04,
    0x00,0x00,0x48,0x8b,0x89,0x08,0x02,0x00,0x00,0xe8,0x2f,0xc7,0x2e,0x00,0x4c,0x8b,0xc8,0x48,0x85,0xc0,0x75,0x07,0x33,0xc0,0x48,0x83,
    0xc4,0x28,0xc3,0x45,0x8b,0x41,0x18,0x33,0xc0,0x45,0x85,0xc0,0x74,0x16,0x4d,0x8b,0x49,0x20,0x90,0x49,0x8b,0x14,0xc1,0x83,0x7a,0x1c,
    0x04,0x75,0xdf,0xff,0xc0,0x41,0x3b,0xc0,0x72,0xef,0xb8,0x01,0x00,0x00,0x00,0x48,0x83,0xc4,0x28,0xc3};
static const uint8_t kEspDepthPin[] = {0x48,0x8b,0x05,0x91,0xea,0x6e,0x01,0x48,0x8b,0x88,0x00,0x04,0x00,0x00,0x44,0x8b,0x81,0x20,0x32,
    0x00,0x00,0x8b,0x91,0x1c,0x32,0x00,0x00,0x41,0x8b,0xc8,0x2b,0xca,0x44,0x3b,0xc2,0x8d,0x81,0x00,0x02,0x00,0x00,0x0f,0x43,0xc1,0xc3};

#define ESP_PIN(game, rva, what, bytes) {game, rva, (uint16_t)sizeof bytes, what, bytes}
static const EspPin kEspPins[] = {
    ESP_PIN(1, 0xF73F8Cu, "the level's generating state", kEspIdlePin),
    ESP_PIN(1, 0xF74090u, "the level's main-thread step", kEspStepPin),
    ESP_PIN(1, 0xF74430u, "the level's end", kEspEndPin),
    ESP_PIN(1, 0xF6FF7Fu, "the generation's release", kEspGenPin),
    ESP_PIN(1, 0xF70B58u, "the generation's release of its stamp packages", kEspUnloadPin),
    ESP_PIN(1, 0xF26165u, "the step's records and context", kEspDispatchPin),
    {1, ESP_RECORD_AT, ESP_RECORD_PIN_SIZE, "the generation dispatcher", ESP_RECORD_PIN},
    ESP_PIN(1, 0xC1A040u, "the dispatcher's op 0x1F", kEspCase1FAt),
    ESP_PIN(1, 0xC1A070u, "the dispatcher's op 0x2B", kEspCase2BAt),
    ESP_PIN(1, 0xC15507u, "the dispatcher's op 0x1F", kEspCase1FPin),
    ESP_PIN(1, 0xC15800u, "the dispatcher's op 0x2B", kEspCase2BPin),
    ESP_PIN(1, 0xF34CC0u, "the stamp planning (op 0x2B)", kEspPlanPin),
    ESP_PIN(1, ESP_QUEUE, "QueueStampPackages", kEspQueuePin),
    ESP_PIN(1, 0xF40706u, "QueueStampPackages", kEspQueueAllocPin),
    ESP_PIN(1, 0xF40897u, "QueueStampPackages", kEspQueueFactionPin),
    ESP_PIN(1, 0xF40DC0u, "QueueStampPackages", kEspQueueContextPin),
    ESP_PIN(1, 0xF40DE3u, "QueueStampPackages", kEspQueueRequestPin),
    ESP_PIN(1, 0x173B8D0u, "the generation arena", kEspArenaPin),
    ESP_PIN(1, 0x173B920u, "the generation arena", kEspArenaChunkPin),
    ESP_PIN(0, 0x320DD0u, "the package load", kEspLoadPin),
    ESP_PIN(0, 0x603746u, "the package load", kEspLoadQueuePin),
    ESP_PIN(0, 0x321050u, "the package release", kEspReleasePin),
    ESP_PIN(0, 0x321330u, "the package check", kEspLoadedPin),
    ESP_PIN(0, 0x321770u, "the package queue's length", kEspDepthPin),
};
#define ESP_PINS (sizeof kEspPins / sizeof kEspPins[0])

/* The engine's package table [[game+0x3326308]+0x10] (exe 0x27C9E60): the slots used, each the reviewed exe function. */
enum { ESP_SLOT_LOAD, ESP_SLOT_RELEASE, ESP_SLOT_LOADED, ESP_SLOT_DEPTH, ESP_SLOTS };
static const EspSlot kEspSlots[ESP_SLOTS] = {{0x2F0u, 0x320DD0u}, {0x308u, 0x321050u}, {0x310u, 0x321330u}, {0x358u, 0x321770u}};

typedef void (*EspQueueFn)(uint8_t* pass, uintptr_t record, uint8_t* context);
typedef void (*EspReleaseFn)(const uint64_t* ids, uint32_t count);
typedef uint32_t (*EspLoadedFn)(uint64_t id);
typedef uint32_t (*EspDepthFn)(void);

enum { ESP_NEW, ESP_WAIT, ESP_HELD, ESP_DONE };
typedef struct EspGen
{
    uintptr_t block, record, types, typeCount, out, outCount;
    uint32_t state, last, from, at1F, typesN, done, count;
    ULONGLONG requestedAt;
    uint64_t list[ESP_LIST_MAX]; /* sorted, each id once */
} EspGen;

/* The private arena QueueStampPackages allocates its set from, laid out as the game's (0x173B8D0): {head chunk, lock,
 * owner (-1: no thread check), growth}; chunk {used, data, capacity, next}. One call takes at most 4 KB of it. */
static struct { uint64_t head; LONG lock; int32_t owner; uint64_t growth; } g_espArena;
static struct { uint64_t used, data, capacity, next; } g_espChunk;
static _Alignas(16) uint8_t g_espArenaData[0x10000];

static int g_espState; /* 0 waiting for the code, 1 on, -1 off */
static ULONGLONG g_espLoadedAt;
static uintptr_t g_espGame, g_espExe;
static EspGen g_esp;    /* the game thread's only */
static uint64_t g_espTheirs[ESP_LIST_MAX];
static uint8_t g_espRecord[ESP_RECORD];

static int JournalPut(uintptr_t address, const uint8_t* bytes, size_t size); /* live_code.c */

/* The first pin that differs, or NULL. */
static const char* EspMismatch(uintptr_t exe, uintptr_t game)
{
    for (size_t i = 0; i < ESP_PINS; i++)
        if (!PinMatches((kEspPins[i].game ? game : exe) + kEspPins[i].rva, kEspPins[i].bytes, kEspPins[i].size)) return kEspPins[i].what;
    return NULL;
}

/* The package table's slots as reviewed: 1, with their functions; 0 when a slot was swapped (a mod's own). */
static int EspTable(uintptr_t fns[ESP_SLOTS])
{
    uintptr_t api = 0, table = 0;
    if (SafeRead(g_espGame + ESP_API, &api, 8) || api == 0 || SafeRead(api + 0x10, &table, 8) || table == 0) return 0;
    for (int s = 0; s < ESP_SLOTS; s++)
        if (SafeRead(table + kEspSlots[s].slot, &fns[s], 8) || fns[s] != g_espExe + kEspSlots[s].rva) return 0;
    return 1;
}

static int EspCompare(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

/* Sorts ids[0..n) and drops repeats: the new count. */
static uint32_t EspUnique(uint64_t* ids, uint32_t n)
{
    if (n < 2) return n;
    qsort(ids, n, 8, EspCompare);
    uint32_t kept = 1;
    for (uint32_t i = 1; i < n; i++) if (ids[i] != ids[kept - 1]) ids[kept++] = ids[i];
    return kept;
}

/* 1 when a pass field of the record in g_espRecord points at the location types or their count. */
static int EspHolds(uintptr_t types, uintptr_t typeCount)
{
    for (uint32_t at = ESP_PASS; at + 8 <= ESP_PASS_END; at += 8)
    {
        uint64_t v;
        memcpy(&v, g_espRecord + at, 8);
        if (v != 0 && (v == types || v == typeCount)) return 1;
    }
    return 0;
}

/* The first op 0x1F after the first op 0x2B, or ESP_RECORDS_MAX. A pipeline without them (the briefing's drop map) has
 * nothing to request. */
static uint32_t EspFind1F(uintptr_t block, uint32_t count)
{
    uint32_t op = 0, seen2B = 0;
    for (uint32_t k = 0; k < count; k++)
    {
        if (SafeRead(block + (uintptr_t)k * ESP_RECORD, &op, 4)) break;
        if (op == 0x1F && seen2B) return k;
        seen2B |= op == 0x2B;
    }
    return ESP_RECORDS_MAX;
}

/* The records' op 0x1F, and where its list is final: after the last op before it that holds the location types or
 * their count. Ops reach the generation block only through their pass fields, and op 0x2B (0xF34CC0 and its jobs)
 * reads neither. */
static void EspScan(EspGen* g, uintptr_t block)
{
    uint32_t count = 0, op = 0;
    uint8_t pass[ESP_PASS_COPY];
    g->state = ESP_DONE;
    if (SafeRead(block + ESP_COUNT, &count, 4) || count == 0 || count > ESP_RECORDS_MAX) return;
    g->at1F = EspFind1F(block, count);
    g->record = block + (uintptr_t)g->at1F * ESP_RECORD;
    if (g->at1F == ESP_RECORDS_MAX || SafeRead(g->record + ESP_PASS, pass, sizeof pass)) return;
    memcpy(&g->types, pass, 8);
    memcpy(&g->typeCount, pass + 0x10, 8);
    memcpy(&g->out, pass + 0x30, 8);
    memcpy(&g->outCount, pass + 0x38, 8);
    if (g->types == 0 || g->typeCount == 0 || g->out == 0 || g->outCount == 0) return;
    g->from = 0;
    for (uint32_t k = 0; k < g->at1F; k++)
    {
        if (SafeRead(block + (uintptr_t)k * ESP_RECORD, g_espRecord, sizeof g_espRecord)) return;
        memcpy(&op, g_espRecord, 4);
        if (op != 0x2B && EspHolds(g->types, g->typeCount)) g->from = k + 1;
    }
    g->state = ESP_WAIT;
}

/* One call of QueueStampPackages on types [done, done + n): a copy of op 0x1F's pass whose list and count are ours, a
 * copy of the level's context whose arena is ours. The function requests what it lists, in the game's order. */
static int EspQueueChunk(EspGen* g, uintptr_t block, uint32_t n)
{
    uint8_t pass[ESP_PASS_COPY], context[ESP_CONTEXT_SIZE];
    if (SafeRead(g->record + ESP_PASS, pass, sizeof pass) || SafeRead(block + ESP_CONTEXT, context, sizeof context)) return 0;
    uint32_t chunkTypes = n, listed = 0;
    uint64_t types = g->types + (uint64_t)g->done * 16, countAt = (uint64_t)(uintptr_t)&chunkTypes;
    uint64_t out = (uint64_t)(uintptr_t)(g->list + g->count), outCountAt = (uint64_t)(uintptr_t)&listed;
    uint64_t arena = (uint64_t)(uintptr_t)&g_espArena;
    memcpy(pass, &types, 8);
    memcpy(pass + 0x10, &countAt, 8);
    memcpy(pass + 0x30, &out, 8);
    memcpy(pass + 0x38, &outCountAt, 8);
    memcpy(context, &arena, 8);
    g_espChunk.used = 0;
    g_espChunk.next = 0;
    ((EspQueueFn)(g_espGame + ESP_QUEUE))(pass, g->record, context);
    if (listed > n * ESP_PER_TYPE) listed = n * ESP_PER_TYPE;
    g->count = EspUnique(g->list, g->count + listed);
    g->done += n;
    return 1;
}

/* Types for the next chunk: as many as the load ring and our list have room for (n types add at most 8n entries). */
static uint32_t EspChunkTypes(const EspGen* g, uint32_t depth)
{
    uint32_t n = depth < ESP_ROOM ? (ESP_ROOM - depth) / ESP_PER_TYPE : 0, left = g->typesN - g->done;
    uint32_t fit = (ESP_LIST_MAX - g->count) / ESP_PER_TYPE;
    if (n > left) n = left;
    return n < fit ? n : fit;
}

/* As many chunks as fit now; the rest at the next step. The type count is read once the last op holding it has run. */
static void EspRequest(EspGen* g, uintptr_t block, const uintptr_t fns[ESP_SLOTS])
{
    if (g->state == ESP_WAIT && (SafeRead(g->typeCount, &g->typesN, 4) || g->typesN > ESP_TYPES_MAX)) { g->state = ESP_DONE; return; }
    uint32_t n;
    while ((n = EspChunkTypes(g, ((EspDepthFn)fns[ESP_SLOT_DEPTH])())) != 0 && EspQueueChunk(g, block, n))
        if (g->state == ESP_WAIT) { g->state = ESP_HELD; g->requestedAt = GetTickCount64(); }
    if (g->state == ESP_HELD && g->done == g->typesN)
        Note("early stamp packages: %u requested for %u location types before record %u (op 0x1F); %u in the load queue",
            g->count, g->typesN, g->at1F, ((EspDepthFn)fns[ESP_SLOT_DEPTH])());
}

/* The ids we requested that op 0x1F's own list (as it stands) does not hold, released: the game releases its own list
 * when the generation is freed (0xF70B58), and a load request is not counted, so nothing is released twice or held. */
static void EspHandOver(EspGen* g, const uintptr_t fns[ESP_SLOTS], const char* when)
{
    uint32_t theirs = 0, extra = 0, loaded = 0;
    if (SafeRead(g->outCount, &theirs, 4) || theirs > ESP_LIST_MAX) theirs = 0;
    if (theirs != 0 && SafeRead(g->out, g_espTheirs, (size_t)theirs * 8)) theirs = 0;
    theirs = EspUnique(g_espTheirs, theirs);
    for (uint32_t i = 0; i < g->count; i++)
    {
        if (bsearch(&g->list[i], g_espTheirs, theirs, 8, EspCompare) == NULL) g->list[extra++] = g->list[i];
        else loaded += ((EspLoadedFn)fns[ESP_SLOT_LOADED])(g->list[i]) != 0;
    }
    if (extra != 0) ((EspReleaseFn)fns[ESP_SLOT_RELEASE])(g->list, extra);
    Note("early stamp packages: %s %.1f s after the request: %u of its %u packages loaded; %u requested early that it does not list, released",
        when, (double)(GetTickCount64() - g->requestedAt) / 1000.0, loaded, theirs, extra);
    g->count = 0;
    g->state = ESP_DONE;
}

/* A generation we never saw end (its block reused): what we hold goes, since its list can no longer be read. */
static void EspForget(EspGen* g, const uintptr_t fns[ESP_SLOTS])
{
    if (g->state == ESP_HELD && g->count != 0)
    {
        ((EspReleaseFn)fns[ESP_SLOT_RELEASE])(g->list, g->count);
        Note("early stamp packages: another generation began before record %u (op 0x1F); its %u early packages released", g->at1F, g->count);
    }
    memset(g, 0, offsetof(EspGen, list));
}

/* From the step's cave, on the game thread, before each main-thread step of a generating level: the generator job is
 * idle (0xF73F8C) and every record before the index has run. */
static void EspStepFrom(uintptr_t level, uintptr_t block)
{
    uintptr_t fns[ESP_SLOTS];
    uint32_t index = 0;
    EspGen* g = &g_esp;
    if (!EspTable(fns) || SafeRead(block + ESP_INDEX, &index, 4)) return;
    if (block != g->block || index < g->last) { EspForget(g, fns); g->block = block; }
    g->last = index;
    if (g->state == ESP_NEW) EspScan(g, block);
    int open = g->state == ESP_WAIT || (g->state == ESP_HELD && g->done < g->typesN);
    if (index > g->at1F && g->state == ESP_HELD) EspHandOver(g, fns, "op 0x1F ran");
    else if (index > g->at1F) g->state = ESP_DONE;
    else if (open && index >= g->from && index < g->at1F) EspRequest(g, block, fns);
}

/* From the end's cave, on the game thread, before the generation is released and its block cleared (0xF6FF60). */
static void EspEndFrom(uintptr_t level)
{
    uintptr_t fns[ESP_SLOTS], block = 0;
    EspGen* g = &g_esp;
    if (!SafeRead(level + ESP_LEVEL_RECORDS, &block, 8) && block == g->block && g->state == ESP_HELD && EspTable(fns))
        EspHandOver(g, fns, "the level ended");
    memset(g, 0, offsetof(EspGen, list));
}

static uint8_t* EspImm64(uint8_t* c, uint8_t op, uintptr_t value)
{
    *c++ = 0x48;
    *c++ = op;
    memcpy(c, &value, 8);
    return c + 8;
}

static uint8_t* EspRel32(uint8_t* c, uint8_t op, uintptr_t to)
{
    int32_t rel = (int32_t)((intptr_t)to - (intptr_t)(c + 5));
    *c = op;
    memcpy(c + 1, &rel, 4);
    return c + 5;
}

/* The step's cave, entered by the call: rcx and rdx kept for the step, the level (rbx) and records (rcx) passed on. At
 * entry rsp is 8 past 16 (0xF73F10's frame), so two pushes and 0x28 align the call. */
static uint8_t* EspStepCave(uint8_t* c, uintptr_t game)
{
    static const uint8_t save[] = {0x51, 0x52, 0x48,0x83,0xec,0x28, 0x48,0x89,0xca, 0x48,0x89,0xd9};
    static const uint8_t back[] = {0xff,0xd0, 0x48,0x83,0xc4,0x28, 0x5a, 0x59};
    memcpy(c, save, sizeof save); c += sizeof save;            /* push rcx; push rdx; sub rsp, 0x28; rdx = rcx; rcx = rbx */
    c = EspImm64(c, 0xb8, (uintptr_t)EspStepFrom);           /* mov rax, EspStepFrom */
    memcpy(c, back, sizeof back); c += sizeof back;            /* call rax; add rsp, 0x28; pop rdx; pop rcx */
    return EspRel32(c, 0xe9, game + ESP_STEP);
}

/* The end's cave: rcx (the level) kept for the release and passed on. */
static uint8_t* EspEndCave(uint8_t* c, uintptr_t game)
{
    static const uint8_t save[] = {0x51, 0x48,0x83,0xec,0x20};
    static const uint8_t back[] = {0xff,0xd0, 0x48,0x83,0xc4,0x20, 0x59};
    memcpy(c, save, sizeof save); c += sizeof save;            /* push rcx; sub rsp, 0x20 */
    c = EspImm64(c, 0xb8, (uintptr_t)EspEndFrom);
    memcpy(c, back, sizeof back); c += sizeof back;            /* call rax; add rsp, 0x20; pop rcx */
    return EspRel32(c, 0xe9, game + ESP_END);
}

/* 1 when every rel32 between the cave page and the two call sites reaches. */
static int EspInReach(uintptr_t cave, uintptr_t game)
{
    int64_t up = (int64_t)(cave + 0x1000 - (game + ESP_STEP)), down = (int64_t)(game + ESP_END_CALL + 5 - cave);
    return up < 0x7FFF0000 && up > -0x7FFF0000 && down < 0x7FFF0000 && down > -0x7FFF0000;
}

/* A call site pointed at its cave: only the rel32 changes. */
static int EspPoint(uintptr_t game, uint32_t site, const uint8_t* cave)
{
    uint8_t call[5] = {0xe8};
    int32_t rel = (int32_t)((intptr_t)cave - (intptr_t)(game + site + 5));
    memcpy(call + 1, &rel, 4);
    return JournalPut(game + site, call, sizeof call) == 0;
}

/* Why the code is not the reviewed one, or NULL. The package table is filled as the engine starts. */
static const char* EspNotReviewed(uintptr_t exe, uintptr_t game)
{
    uintptr_t fns[ESP_SLOTS];
    const char* why = EspMismatch(exe, game);
    if (why != NULL) return why;
    g_espExe = exe;
    g_espGame = game;
    return EspTable(fns) ? NULL : "the engine's package table";
}

/* 1 on, 0 the code is not the reviewed one (yet), -1 off. The end's cave goes in first: alone it releases nothing. */
static int EarlyStampPackagesInstallAt(uintptr_t exe, uintptr_t game)
{
    if (EspNotReviewed(exe, game) != NULL) return 0;
    if (!PerfFixOn("early-stamp-packages")) return -1;
    uint8_t* cave = AllocateNearGame(game, 0x1000);
    if (cave == NULL || !EspInReach((uintptr_t)cave, game)) { Note("early stamp packages: off; no memory near game.dll"); return -1; }
    g_espChunk.data = (uint64_t)(uintptr_t)g_espArenaData;
    g_espChunk.capacity = sizeof g_espArenaData;
    g_espArena.head = (uint64_t)(uintptr_t)&g_espChunk;
    g_espArena.owner = -1;
    g_espArena.growth = 0x100000;
    uint8_t* end = EspStepCave(cave, game);
    EspEndCave(end, game);
    DWORD previous;
    VirtualProtect(cave, 0x1000, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), cave, 0x1000);
    if (!EspPoint(game, ESP_END_CALL, end) || !EspPoint(game, ESP_STEP_CALL, cave))
    {
        Note("early stamp packages: off; the level's code could not be rewritten");
        return -1;
    }
    Note("early stamp packages: on; a mission's stamp packages are requested after the last op that writes its location types");
    return 1;
}

/* From the watch thread once the game has loaded: its code is decoded some time after, never a minute later. */
static void EarlyStampPackagesInstall(void)
{
    if (g_espState != 0 || !g_fired[HF_EVENT_GAME_LOADED]) return;
    if (strcmp(g_build, "25480438")) { g_espState = -1; Note("early stamp packages: off; an unsupported build"); return; }
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL), game = ApiModuleBase("game.dll", NULL);
    int result = game != 0 ? EarlyStampPackagesInstallAt(exe, game) : 0;
    if (result != 0) { g_espState = result; return; }
    if (g_espLoadedAt == 0) g_espLoadedAt = GetTickCount64();
    if (GetTickCount64() - g_espLoadedAt < 60000) return;
    const char* why = game != 0 ? EspNotReviewed(exe, game) : "game.dll";
    g_espState = -1;
    Note("early stamp packages: off; %s is not the reviewed code", why != NULL ? why : "the engine's package table");
}
