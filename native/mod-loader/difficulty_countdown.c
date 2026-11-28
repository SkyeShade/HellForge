/* Level 11's mission countdown (difficulty.c, HfDifficulty.mission_minutes): its entity delta, the mission launch that
 * lists it, its network definition and the two displays that show it. Included by hellforge_modloader.c after
 * difficulty_missions.c. */

/* The mission countdown is HiveMindComponent +0x74 (0x95F301 copies it into the countdown slot and the net field;
 * 0x506580's readers take it as the duration). Missions change it through entity deltas, as High Value Asset missions
 * get 1200 s: launch (0xAD0010) lists the mission row's delta (0xAD0DDA) and the modifiers' (0xAD0E10), up to 10
 * resource hashes at rbp+0x8D0 (count) / +0x8D8, and the spawn (0x5740B0, HiveMind case 0x57525C) gives the mission's
 * HiveMind a private copy of the shared definition (0x505F50) with them applied. The level's countdown is a delta of
 * its own, registered in the game's entity delta table (DifficultyDelta) with one HiveMind (0xD4) member, +0x74 = the
 * level's seconds, and appended at 0xAD0E4E when the mission's level (lg+0xF9) is the added one. The shared
 * definition is never written, so every other level keeps the mission's own countdown. The host clamps the countdown
 * every frame to its network definition (207: name 0x72A6AB0E, float, 16 bits, 0..2500 s, at the network config's
 * defs, stride 24: +8 change threshold, +0xC type, +0xD bits, +0x10 min, +0x14 max), so 2500 becomes the level's
 * seconds + 60 (players all run HellForge: the config hash covers it). */
#define DIF_TIMER_SITE 0x95F301u      /* movss xmm0, [rax+0x74]: checked as the countdown's member, not changed */
#define DIF_LAUNCH_SITE 0xAD0E4Eu
#define DIF_LAUNCH_BACK 0xAD0E5Au
#define DIF_DELTA_TABLE 0x346BF98u    /* [game+..]+0xF12E98: the entity delta table (0x5154C0) */
#define DIF_DELTA_HOLDER 0xF12E98u
#define DIF_HIVEMIND 0xD4u            /* HiveMindComponent's component type in deltas */
#define DIF_COUNTDOWN_DEF 0x72A6AB0Eu
static const uint8_t kTimerPin[5] = {0xF3, 0x0F, 0x10, 0x40, 0x74};
static const uint8_t kLaunchPin[12] = {0x49, 0x8B, 0x82, 0x88, 0xE2, 0x0A, 0x00, 0x44, 0x8B, 0x64, 0x24, 0x30};
static const uint8_t kLaunchStub[79] = {
    0x49, 0x8B, 0x82, 0x88, 0xE2, 0x0A, 0x00,                         /*  0 mov rax, [r10+0xae288] (the level) */
    0x80, 0xB8, 0xF9, 0x00, 0x00, 0x00, 0x0B,                         /*  7 cmp byte [rax+0xf9], 11 */
    0x75, 0x35,                                                       /* 14 jne 69 */
    0x80, 0x3D, 0, 0, 0, 0, 0x00,                                     /* 16 cmp byte [rip+watch+0x40], 0 (rel32 at 18) */
    0x74, 0x2C,                                                       /* 23 je 69 (the delta is not registered) */
    0x8B, 0x8D, 0xD0, 0x08, 0x00, 0x00,                               /* 25 mov ecx, [rbp+0x8d0] (deltas listed) */
    0x83, 0xF9, 0x0A,                                                 /* 31 cmp ecx, 10 */
    0x73, 0x21,                                                       /* 34 jae 69 */
    0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,                               /* 36 mov rdx, the delta's hash (imm64 at 38) */
    0x48, 0x89, 0x94, 0xCD, 0xD8, 0x08, 0x00, 0x00,                   /* 46 mov [rbp+rcx*8+0x8d8], rdx */
    0xFF, 0xC1,                                                       /* 54 inc ecx */
    0x89, 0x8D, 0xD0, 0x08, 0x00, 0x00,                               /* 56 mov [rbp+0x8d0], ecx */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 62 lock inc dword [rip+watch+0x44] (rel32 at 65) */
    0x44, 0x8B, 0x64, 0x24, 0x30,                                     /* 69 mov r12d, [rsp+0x30] */
    0xE9, 0, 0, 0, 0};                                                /* 74 jmp 0xAD0E5A (rel32 at 75) */
/* Two displays read the countdown from the shared definition rather than the mission's: the war table's mission card
 * (0x1A12D70: the mode's definition from 0x505F50 with only the mission row's delta, +0x360, applied in a copy; every
 * path ends at 0x1A131CC with the seconds in xmm2, divided by 60 into the card's "40 MINUTES") and the mission's timer
 * widget (0x17BA940: the total at 0x17BAA44, less the countdown left (manager +0x51948), is the elapsed time and its
 * segments). At the added level both take the countdown delta's own value (watch+0x58: its data in the game's delta
 * table, 0 until registered). The card's level is its mission descriptor's (rsi, +9, as lg+0xF0); the widget's the
 * mission's level (manager [game+0x3326D10] +0x650, +0xF9). */
#define DIF_CARD_TIME_SITE 0x1A131CCu
#define DIF_CARD_TIME_BACK 0x1A131D4u
#define DIF_MINUTE 0x23C7740u         /* 60.0 */
#define DIF_WIDGET_TIME_SITE 0x17BAA44u
#define DIF_WIDGET_TIME_BACK 0x17BAA51u
#define DIF_HIVEMIND_MANAGER 0x3326D10u
static const uint8_t kCardTimePin[8] = {0xF3, 0x0F, 0x5E, 0x15, 0x6C, 0x45, 0x9B, 0x00}; /* divss xmm2, [rip+60.0] */
static const uint8_t kCardTimeStub[35] = {
    0x80, 0x7E, 0x09, 0x0B,                                           /*  0 cmp byte [rsi+9], 11 */
    0x75, 0x10,                                                       /*  4 jne 22 */
    0x48, 0x8B, 0x05, 0, 0, 0, 0,                                     /*  6 mov rax, [rip+watch+0x58] (rel32 at 9) */
    0x48, 0x85, 0xC0,                                                 /* 13 test rax, rax */
    0x74, 0x04,                                                       /* 16 je 22 */
    0xF3, 0x0F, 0x10, 0x10,                                           /* 18 movss xmm2, [rax] */
    0xF3, 0x0F, 0x5E, 0x15, 0, 0, 0, 0,                               /* 22 divss xmm2, [rip+60.0] (rel32 at 26) */
    0xE9, 0, 0, 0, 0};                                                /* 30 jmp 0x1A131D4 (rel32 at 31) */
static const uint8_t kWidgetTimePin[13] = {0xF3, 0x44, 0x0F, 0x10, 0x50, 0x74, 0x48, 0x8B, 0x05, 0xBF, 0xC2, 0xB6, 0x01};
static const uint8_t kWidgetTimeStub[75] = {
    0xF3, 0x44, 0x0F, 0x10, 0x50, 0x74,                               /*  0 movss xmm10, [rax+0x74] */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /*  6 mov rax, game+0x3326D10 (imm64 at 8) */
    0x48, 0x8B, 0x00,                                                 /* 16 mov rax, [rax] (the manager) */
    0x48, 0x8B, 0x80, 0x50, 0x06, 0x00, 0x00,                         /* 19 mov rax, [rax+0x650] (the level) */
    0x48, 0x85, 0xC0,                                                 /* 26 test rax, rax */
    0x74, 0x1A,                                                       /* 29 je 57 */
    0x80, 0xB8, 0xF9, 0x00, 0x00, 0x00, 0x0B,                         /* 31 cmp byte [rax+0xf9], 11 */
    0x75, 0x11,                                                       /* 38 jne 57 */
    0x48, 0x8B, 0x05, 0, 0, 0, 0,                                     /* 40 mov rax, [rip+watch+0x58] (rel32 at 43) */
    0x48, 0x85, 0xC0,                                                 /* 47 test rax, rax */
    0x74, 0x05,                                                       /* 50 je 57 */
    0xF3, 0x44, 0x0F, 0x10, 0x10,                                     /* 52 movss xmm10, [rax] */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                               /* 57 mov rax, game+0x3326D10 (imm64 at 59) */
    0x48, 0x8B, 0x00,                                                 /* 67 mov rax, [rax] */
    0xE9, 0, 0, 0, 0};                                                /* 70 jmp 0x17BAA51 (rel32 at 71) */

/* The level's countdown delta, registered once the game has loaded its entity delta table (polled). The table
 * (0x5154C0) is {pointer, count} x 5: an open-addressed hash (resource -> delta; 16-byte rows, slot = hash % count,
 * linear probing, key 0 = empty), the deltas ({component count, first}), their components ({type, first member,
 * member count}), the members ({record offset, size, data offset}) and the data. Indices and data offsets are relative
 * to each array's own pointer, so arrays 1 to 4 move to copies with the new rows appended; the hash row goes into a
 * free slot of the game's own hash last, so the delta is complete before its name resolves. */
static uintptr_t g_deltaTable;    /* the table HellForge moved the arrays of, with g_deltaArrays and g_deltaAdded (stats-tables.c) */
static uintptr_t g_deltaArrays[5];
static uint32_t g_deltaAdded[5];
static uintptr_t g_deltaLive[3];  /* the table the countdown is read from now, its arrays 1 and 4: g_deltaTable's or a copy's */
static uint64_t DifficultyDeltaHash(void)
{
    static const char name[] = "hellforge/difficulty/countdown";
    return Murmur64A(name, sizeof name - 1);
}

static uint8_t* g_launchStub; /* prepared by InstallDifficulty when the level has its own countdown */
/* The entity delta table as DifficultyDelta reads it: {pointer, count} x 5 (arrays), and the counts it adds after. */
typedef struct DeltaTable
{
    uintptr_t table;
    uint64_t arrays[10];
    uint32_t slots, deltas, components, members, data, at; /* at: the new delta's data, aligned after the data */
} DeltaTable;

static int g_deltaRefused, g_deltaPatched; /* DifficultyDelta: refused for good; the mission launch patched */
static int AttDeltasSettled(uintptr_t table); /* attachments.c */

/* A table whose rows could not be read or copied is tried again after a pause that doubles to 12.8 s, and refused for
 * good after DIF_DELTA_TRIES (about five minutes); the first failure is logged. */
#define DIF_DELTA_TRIES 30
static int g_deltaFailures;
static ULONGLONG g_deltaRetryAt;
static void DeltaFailed(const char* why)
{
    if (++g_deltaFailures == 1) Note("difficulty: the countdown delta waits: %s; tried again", why);
    if (g_deltaFailures >= DIF_DELTA_TRIES)
    {
        g_deltaRefused = 1;
        ApiLog(g_difficulty.mod, "difficulty %s: %s after %d tries; missions keep their own countdown", g_difficulty.name, why, g_deltaFailures);
        return;
    }
    g_deltaRetryAt = GetTickCount64() + (100ull << (g_deltaFailures < 7 ? g_deltaFailures : 7));
}

/* The countdown delta's free slot in the game's own hash (linear probing from hash % slots): 1 found (*empty), 0 a slot
 * is unreadable, -1 its name is taken (*empty: the name's slot), -2 the hash is full. */
static int DeltaSlot(const DeltaTable* t, uint64_t hash, uint32_t* empty)
{
    uint32_t slot = (uint32_t)(hash % t->slots);
    *empty = UINT32_MAX;
    for (uint32_t i = 0; i < t->slots && *empty == UINT32_MAX; i++, slot = slot + 1 == t->slots ? 0 : slot + 1)
    {
        uint64_t key;
        if (SafeRead(t->arrays[0] + (uintptr_t)slot * 16, &key, 8)) return 0;
        if (key == hash) { *empty = slot; return -1; }
        if (key == 0) *empty = slot;
    }
    return *empty == UINT32_MAX ? -2 : 1;
}

static void AtLeast(uint32_t* value, uint32_t floor) { if (*value < floor) *value = floor; }

/* A component's members and their data, into reach (DeltaReach): 0 when a row is unreadable. */
static int DeltaComponentReach(const DeltaTable* t, uint32_t row, uint32_t reach[4])
{
    uint32_t component[3];
    if (SafeRead(t->arrays[4] + (uintptr_t)row * 12, component, sizeof component) || component[1] > 0x100000 || component[2] > 0x10000) return 0;
    AtLeast(&reach[2], component[1] + component[2]);
    for (uint32_t m = 0; m < component[2]; m++)
    {
        uint32_t member[3];
        if (SafeRead(t->arrays[6] + (uintptr_t)(component[1] + m) * 12, member, sizeof member) || member[1] > 0x10000 || member[2] > 0x1000000) return 0;
        AtLeast(&reach[3], member[2] + member[1]);
    }
    return 1;
}

/* The delta a hash slot names, with its components, into reach (DeltaReach): 0 when a row is unreadable. */
static int DeltaNamedReach(const DeltaTable* t, uint32_t slot, uint32_t reach[4])
{
    uint64_t key; uint32_t index, delta[2];
    if (SafeRead(t->arrays[0] + (uintptr_t)slot * 16, &key, 8)) return 0;
    if (key == 0) return 1;
    if (SafeRead(t->arrays[0] + (uintptr_t)slot * 16 + 8, &index, 4) || index > 0x100000 ||
        SafeRead(t->arrays[2] + (uintptr_t)index * 8, delta, sizeof delta) || delta[0] > 0x10000 || delta[1] > 0x100000) return 0;
    AtLeast(&reach[0], index + 1);
    AtLeast(&reach[1], delta[1] + delta[0]);
    for (uint32_t c = 0; c < delta[0]; c++) if (!DeltaComponentReach(t, delta[1] + c, reach)) return 0;
    return 1;
}

/* The table's sizes widened to what a lookup by name reaches, so the countdown's rows go after every named delta's.
 * The header's counts are not that: the game's rows past its last named delta are not deltas, and a table HellForge
 * grew (attachments.c) keeps the game's counts with rows of its own after them. 0 when a row is unreadable. */
static int DeltaReach(DeltaTable* t)
{
    uint32_t reach[4] = {0};
    for (uint32_t i = 0; i < t->slots; i++) if (!DeltaNamedReach(t, i, reach)) return 0;
    AtLeast(&t->deltas, reach[0]); AtLeast(&t->components, reach[1]); AtLeast(&t->members, reach[2]); AtLeast(&t->data, reach[3]);
    t->at = (t->data + 3) & ~3u;
    return 1;
}

/* Copies of arrays 1 to 4 with the countdown delta's rows appended (HiveMind +0x74 = the level's seconds), in one
 * read-only block: 0 when it could not be made. */
static int MoveDeltaArrays(const DeltaTable* t, uint8_t* moved[4])
{
    size_t bytes[4] = {((size_t)t->deltas + 1) * 8, ((size_t)t->components + 1) * 12, ((size_t)t->members + 1) * 12, (size_t)t->at + 4}, total = 0;
    for (int i = 0; i < 4; i++) { moved[i] = (uint8_t*)total; total += (bytes[i] + 15) & ~(size_t)15; }
    uint8_t* block = (uint8_t*)VirtualAlloc(NULL, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (block == NULL) return 0;
    for (int i = 0; i < 4; i++) moved[i] = block + (size_t)moved[i];
    if (SafeRead(t->arrays[2], moved[0], (size_t)t->deltas * 8) || SafeRead(t->arrays[4], moved[1], (size_t)t->components * 12) ||
        SafeRead(t->arrays[6], moved[2], (size_t)t->members * 12) || SafeRead(t->arrays[8], moved[3], t->data))
    {
        VirtualFree(block, 0, MEM_RELEASE);
        return 0;
    }
    float seconds = g_difficulty.minutes * 60.0f;
    uint32_t delta[2] = {1, t->components}, component[3] = {DIF_HIVEMIND, t->members, 1}, member[3] = {0x74, 4, t->at};
    memcpy(moved[0] + (size_t)t->deltas * 8, delta, sizeof delta);
    memcpy(moved[1] + (size_t)t->components * 12, component, sizeof component);
    memcpy(moved[2] + (size_t)t->members * 12, member, sizeof member);
    memcpy(moved[3] + t->at, &seconds, 4);
    DWORD protect;
    VirtualProtect(block, total, PAGE_READONLY, &protect);
    return 1;
}

/* The table's arrays 4 to 1 pointed at their copies: 0 if one could not be written. */
static int PublishDeltaArrays(const DeltaTable* t, uint8_t* const moved[4])
{
    uint32_t counts[4] = {t->deltas + 1, t->components + 1, t->members + 1, t->at + 4};
    for (int i = 3; i >= 0; i--)
    {
        uint64_t pair[2] = {(uint64_t)(uintptr_t)moved[i], counts[i]};
        if (ApiWrite(t->table + 16 + (uintptr_t)i * 16, pair, sizeof pair)) return 0;
    }
    return 1;
}

/* The mission launch's jump to the stub that lists the countdown delta: 0 if it could not be written. It is patched
 * after the install steps (when the delta is named), so the site audit records it here. */
static int PatchLaunch(uintptr_t game, const uint8_t* launchStub)
{
    uint8_t jump[12] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    int32_t rel = DifficultyJumpRel(game + DIF_LAUNCH_SITE, (uintptr_t)launchStub);
    memcpy(jump + 1, &rel, 4);
    int written = !ApiWrite(game + DIF_LAUNCH_SITE, jump, sizeof jump);
    if (written) StorePatch(game + DIF_LAUNCH_SITE, jump, sizeof jump);
    return written;
}

/* The countdown read from table t (its seconds at `seconds`), the launch patched once: 0 if the launch could not be. */
static int CountdownOn(uintptr_t game, const DeltaTable* t, volatile uint8_t* watch, const uint8_t* launchStub, uint8_t* seconds)
{
    g_deltaLive[0] = t->table; g_deltaLive[1] = (uintptr_t)t->arrays[2]; g_deltaLive[2] = (uintptr_t)t->arrays[8];
    g_deltaFailures = 0;
    watch[0x40] = 1;
    *(uint8_t* volatile*)(watch + 0x58) = seconds;
    if (g_deltaPatched) return 1;
    if (!PatchLaunch(game, launchStub))
    {
        g_deltaRefused = 1;
        watch[0x40] = 0;
        ApiLog(g_difficulty.mod, "difficulty %s: the mission launch could not be changed; missions keep their own countdown", g_difficulty.name);
        return 0;
    }
    g_deltaPatched = 1;
    return 1;
}

/* The countdown delta into the table: its arrays moved, its name in the free slot `empty` last, the launch patched once. */
static void AddCountdownDelta(uintptr_t game, DeltaTable* t, volatile uint8_t* watch, const uint8_t* launchStub, uint64_t hash, uint32_t empty)
{
    uint8_t* moved[4];
    if (!MoveDeltaArrays(t, moved)) { DeltaFailed("the entity delta table's rows could not be copied"); return; }
    if (!PublishDeltaArrays(t, moved)) { g_deltaRefused = 1; Note("difficulty: the entity delta table could not be extended"); return; }
    uint64_t index = t->deltas;
    if (ApiWrite(t->arrays[0] + (uintptr_t)empty * 16 + 8, &index, 8) || ApiWrite(t->arrays[0] + (uintptr_t)empty * 16, &hash, 8))
    {
        g_deltaRefused = 1;
        Note("difficulty: the countdown delta could not be named");
        return;
    }
    g_deltaTable = t->table;
    g_deltaArrays[0] = (uintptr_t)t->arrays[0];
    for (int i = 0; i < 4; i++) g_deltaArrays[i + 1] = (uintptr_t)moved[i];
    g_deltaAdded[1] = t->deltas + 1 - (uint32_t)t->arrays[3];
    g_deltaAdded[2] = t->components + 1 - (uint32_t)t->arrays[5];
    g_deltaAdded[3] = t->members + 1 - (uint32_t)t->arrays[7];
    g_deltaAdded[4] = t->at + 4 - (uint32_t)t->arrays[9];
    for (int i = 0; i < 4; i++) t->arrays[2 + 2 * i] = (uint64_t)(uintptr_t)moved[i]; /* t as the table reads now */
    if (!CountdownOn(game, t, watch, launchStub, moved[3] + t->at)) return;
    Note("difficulty: countdown delta %016llx registered (delta %u: HiveMind +0x74 = %.0f s; table 0x%llx, hash slot %u of %u)",
        (unsigned long long)hash, t->deltas, (double)(g_difficulty.minutes * 60.0f), (unsigned long long)t->table, empty, t->slots);
}

/* The countdown delta already named in the table at `slot`: a copy of a table it was registered in, as the attachments'
 * grown one (attachments.c) copies every named delta. Read from there, as it is: 1, or 0 when the name's rows are not
 * the countdown's (one HiveMind component, its member +0x74 of 4 bytes). */
static int AdoptCountdownDelta(uintptr_t game, const DeltaTable* t, volatile uint8_t* watch, const uint8_t* launchStub, uint64_t hash, uint32_t slot)
{
    uint32_t index, delta[2], component[3], member[3];
    float seconds;
    if (SafeRead(t->arrays[0] + (uintptr_t)slot * 16 + 8, &index, 4) || index >= t->deltas ||
        SafeRead(t->arrays[2] + (uintptr_t)index * 8, delta, sizeof delta) || delta[0] != 1 || delta[1] >= t->components ||
        SafeRead(t->arrays[4] + (uintptr_t)delta[1] * 12, component, sizeof component) || component[0] != DIF_HIVEMIND || component[2] != 1 ||
        component[1] >= t->members || SafeRead(t->arrays[6] + (uintptr_t)component[1] * 12, member, sizeof member) ||
        member[0] != 0x74 || member[1] != 4 || member[2] + 4 > t->data || SafeRead(t->arrays[8] + member[2], &seconds, 4)) return 0;
    if (!CountdownOn(game, t, watch, launchStub, (uint8_t*)(uintptr_t)t->arrays[8] + member[2])) return 1;
    Note("difficulty: countdown delta %016llx kept from a copy of its table (delta %u: HiveMind +0x74 = %.0f s; table 0x%llx, hash slot %u of %u)",
        (unsigned long long)hash, index, (double)seconds, (unsigned long long)t->table, slot, t->slots);
    return 1;
}

/* The countdown delta by its name in the table: kept where a copy already carries it, else added in a free slot. */
static void PlaceCountdownDelta(uintptr_t game, DeltaTable* t, volatile uint8_t* watch, const uint8_t* launchStub, uint64_t hash)
{
    uint32_t empty;
    int found = DeltaSlot(t, hash, &empty);
    if (found == 0) { DeltaFailed("a hash slot of the entity delta table is unreadable"); return; }
    if (found == -1 && AdoptCountdownDelta(game, t, watch, launchStub, hash, empty)) return;
    if (found == -1) { g_deltaRefused = 1; ApiLog(g_difficulty.mod, "difficulty %s: the countdown delta's name is taken by rows that are not the countdown's; missions keep their own countdown", g_difficulty.name); return; }
    if (found == -2) { g_deltaRefused = 1; ApiLog(g_difficulty.mod, "difficulty %s: the entity delta table is full; missions keep their own countdown", g_difficulty.name); return; }
    AddCountdownDelta(game, t, watch, launchStub, hash, empty);
}

static void DifficultyDelta(uintptr_t game)
{
    volatile uint8_t* watch = g_restartWatch;
    uint8_t* launchStub = g_launchStub;
    uintptr_t holder = 0;
    DeltaTable t = {0};
    if (g_deltaRefused || watch == NULL || launchStub == NULL || SafeRead(game + DIF_DELTA_TABLE, &holder, 8) || holder == 0 ||
        SafeRead(holder + DIF_DELTA_HOLDER, &t.table, 8) || t.table == 0 || SafeRead(t.table, t.arrays, sizeof t.arrays)) return;
    if (t.table == g_deltaLive[0] && t.arrays[2] == g_deltaLive[1] && t.arrays[8] == g_deltaLive[2]) return;
    if (g_deltaRetryAt != 0 && GetTickCount64() < g_deltaRetryAt) return; /* a failed table's pause (DeltaFailed) */
    if (watch[0x40] || *(uint8_t* volatile*)(watch + 0x58) != NULL) /* once a table: a failed one is tried again silently */
    {
        watch[0x40] = 0;
        *(uint8_t* volatile*)(watch + 0x58) = NULL;
        Note("difficulty: the entity delta table changed (0x%llx); the countdown delta is registered again", (unsigned long long)t.table);
    }
    if (!AttDeltasSettled(t.table)) return; /* the attachments grow the table first (#574) */
    uint64_t hash = DifficultyDeltaHash();
    t.slots = (uint32_t)t.arrays[1]; t.deltas = (uint32_t)t.arrays[3]; t.components = (uint32_t)t.arrays[5];
    t.members = (uint32_t)t.arrays[7]; t.data = (uint32_t)t.arrays[9]; t.at = (t.data + 3) & ~3u;
    if (t.slots == 0 || t.arrays[1] > 0x100000 || t.arrays[3] > 0x100000 || t.arrays[5] > 0x100000 || t.arrays[7] > 0x100000 || t.arrays[9] > 0x1000000)
    {
        g_deltaRefused = 1;
        ApiLog(g_difficulty.mod, "difficulty %s: the entity delta table is not as reviewed (%u slots, %u deltas); missions keep their own countdown",
            g_difficulty.name, t.slots, t.deltas);
        return;
    }
    if (!DeltaReach(&t)) { DeltaFailed("a named row of the entity delta table is unreadable"); return; }
    PlaceCountdownDelta(game, &t, watch, launchStub, hash);
}

/* The countdown's network definition (207: name 0x72A6AB0E, float) among the config's rows, with its maximum: its
 * index, or -1 (none, or a row is unreadable). */
static int64_t CountdownDefinition(uintptr_t rows, uint32_t count, float* maximum)
{
    for (uint32_t i = 0; i < count; i++)
    {
        uint8_t def[24];
        if (SafeRead(rows + (uintptr_t)i * 24, def, sizeof def)) return -1;
        uint32_t name;
        memcpy(&name, def, 4); memcpy(maximum, def + 0x14, 4);
        if (name == DIF_COUNTDOWN_DEF && def[0xC] == 2) return i;
    }
    return -1;
}

/* The countdown's network definition: its maximum becomes the level's seconds + 60 once the network config is up
 * (polled; a reloaded config is widened again). */
static void DifficultyNetwork(void)
{
    static uintptr_t widened;
    uintptr_t rows;
    uint32_t count;
    if (g_difficultyState != 1 || g_difficulty.minutes * 60.0f <= 2500.0f || FieldTypes(&rows, &count)) return;
    float maximum = 0.0f;
    int64_t i = CountdownDefinition(rows, count, &maximum);
    if (i < 0) return;
    uintptr_t at = rows + (uintptr_t)i * 24;
    if (at == widened && maximum != 2500.0f) return;
    float wider = g_difficulty.minutes * 60.0f + 60.0f, step = wider / 131072.0f;
    if (maximum != 2500.0f && maximum != wider) { Note("difficulty: the countdown's network definition has an unexpected range (max %.1f); left as it is", (double)maximum); widened = at; return; }
    if (ApiWrite(at + 0x14, &wider, 4) || ApiWrite(at + 8, &step, 4)) return;
    widened = at;
    Note("difficulty: the countdown's network definition (%u) goes to %.0f s for %.0f-minute missions", (uint32_t)i, (double)wider, (double)g_difficulty.minutes);
}

/* ---- the checks ------------------------------------------------------------------------------------------------- */

/* With a countdown of its own: the countdown's member, the launch's delta list and the two displays. */
static const char* CountdownSitesFailed(uintptr_t game)
{
    uint8_t pin[11];
    if (OwnCountdown() && (SafeRead(game + DIF_TIMER_SITE, pin, 5) || memcmp(pin, kTimerPin, 5) != 0)) return "the mission countdown is not the reviewed one";
    uint8_t launch[12];
    if (OwnCountdown() && (SafeRead(game + DIF_LAUNCH_SITE, launch, 12) || memcmp(launch, kLaunchPin, 12) != 0)) return "the mission launch's delta list is not the reviewed one";
    uint8_t times[13];
    if (OwnCountdown() && (SafeRead(game + DIF_CARD_TIME_SITE, times, 8) || memcmp(times, kCardTimePin, 8) != 0)) return "the mission card's duration is not the reviewed one";
    if (OwnCountdown() && (SafeRead(game + DIF_WIDGET_TIME_SITE, times, 13) || memcmp(times, kWidgetTimePin, 13) != 0)) return "the mission timer widget is not the reviewed one";
    return NULL;
}

/* ---- the install steps ------------------------------------------------------------------------------------------ */

/* The launch stub, jumped to once the countdown delta is registered (DifficultyDelta). */
static const char* PrepareLaunchStub(DifficultyInstall* d)
{
    uint8_t* launchStub = d->code + 0x790;
    d->launchStub = launchStub;
    if (!OwnCountdown()) return NULL;
    uint64_t hash = DifficultyDeltaHash();
    memcpy(launchStub, kLaunchStub, sizeof kLaunchStub);
    int32_t rel = (int32_t)(d->watch + 0x40 - (launchStub + 23)); memcpy(launchStub + 18, &rel, 4);
    memcpy(launchStub + 38, &hash, 8);
    rel = (int32_t)(d->watch + 0x44 - (launchStub + 69)); memcpy(launchStub + 65, &rel, 4);
    rel = (int32_t)(d->game + DIF_LAUNCH_BACK - (uintptr_t)(launchStub + 79)); memcpy(launchStub + 75, &rel, 4);
    g_launchStub = launchStub;
    return NULL;
}

/* The mission card's and the timer widget's stubs (their countdown: the delta's value in the watch, +0x58). */
static const char* PrepareTimeStubs(DifficultyInstall* d)
{
    uintptr_t game = d->game;
    uint8_t* cardTimeStub = d->code + 0x7E0, *widgetTimeStub = d->code + 0x810;
    d->cardTimeStub = cardTimeStub;
    d->widgetTimeStub = widgetTimeStub;
    if (!OwnCountdown()) return NULL;
    uint64_t manager = game + DIF_HIVEMIND_MANAGER;
    int32_t rel;
    memcpy(cardTimeStub, kCardTimeStub, sizeof kCardTimeStub);
    rel = (int32_t)(d->watch + 0x58 - (cardTimeStub + 13)); memcpy(cardTimeStub + 9, &rel, 4);
    rel = (int32_t)(game + DIF_MINUTE - (uintptr_t)(cardTimeStub + 30)); memcpy(cardTimeStub + 26, &rel, 4);
    rel = (int32_t)(game + DIF_CARD_TIME_BACK - (uintptr_t)(cardTimeStub + 35)); memcpy(cardTimeStub + 31, &rel, 4);
    memcpy(widgetTimeStub, kWidgetTimeStub, sizeof kWidgetTimeStub);
    memcpy(widgetTimeStub + 8, &manager, 8);
    memcpy(widgetTimeStub + 59, &manager, 8);
    rel = (int32_t)(d->watch + 0x58 - (widgetTimeStub + 47)); memcpy(widgetTimeStub + 43, &rel, 4);
    rel = (int32_t)(game + DIF_WIDGET_TIME_BACK - (uintptr_t)(widgetTimeStub + 75)); memcpy(widgetTimeStub + 71, &rel, 4);
    return NULL;
}

static const char* JumpToTimeStubs(DifficultyInstall* d)
{
    if (!OwnCountdown()) return NULL;
    uintptr_t game = d->game;
    uint8_t card[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90}, widget[13];
    int32_t rel = DifficultyJumpRel(game + DIF_CARD_TIME_SITE, (uintptr_t)d->cardTimeStub);
    memcpy(card + 1, &rel, 4);
    memset(widget, 0x90, sizeof widget);
    widget[0] = 0xE9;
    rel = DifficultyJumpRel(game + DIF_WIDGET_TIME_SITE, (uintptr_t)d->widgetTimeStub);
    memcpy(widget + 1, &rel, 4);
    if (ApiWrite(game + DIF_CARD_TIME_SITE, card, sizeof card) || ApiWrite(game + DIF_WIDGET_TIME_SITE, widget, sizeof widget))
        return "the mission card's duration could not be given the level's countdown";
    return NULL;
}
