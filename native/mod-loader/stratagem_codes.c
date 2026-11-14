/* Stratagem input codes as a stats field (StratagemCode; docs/modding/stats-mods.md "Stratagem input codes"). A code is
 * a pointer and a length in the row (+0x40, +0x48: u32 directions, 1 up 2 right 3 down 4 left). Once the stratagems load,
 * every vanilla code moves into an array of HellForge's, the row pointed at it; beside each array is the code packed in
 * one u32 (a base-5 digit per direction, the first lowest), the only bytes the stats layers write. The game-thread
 * update writes the array and the row's length from it. The game reads the code each time: the input matcher
 * (0x66D8C0) into a 12-direction stack buffer, the HUD (0xA106C0) into a 16-direction one, the ship's arcade minigame
 * into its 10 arrows; the input keeps 16 presses (0xA903AF). So 1 to 10 directions. Those reads are pinned; another build
 * gets no arrays and refuses every code edit. Included by stats.c. */

#define SCODE_MAX 10u      /* the arcade minigame's 10 arrows; the matcher's stack buffer takes 12 (rsp+0x70..0xA0) */
#define SCODE_ROW_CODE 0x40u
#define SCODE_ROW_LENGTH 0x48u

typedef struct { uint32_t rva; uint8_t length; uint8_t bytes[24]; } CodePin;
static const CodePin kCodePins[] = {
    {0x66D8D8u, 7, {0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00}},       /* sub rsp, 0xF0: the matcher's frame */
    {0x66DE35u, 8, {0x4C, 0x8D, 0xB4, 0x24, 0xA0, 0x00, 0x00, 0x00}}, /* lea r14, [rsp+0xA0]: the buffer after the code's */
    {0x66DF7Eu, 21, {0x8B, 0x5A, 0x48, 0x48, 0x8D, 0x4C, 0x24, 0x70, 0x48, 0x8B, 0x52, 0x40, 0x4C, 0x8D, 0x04, 0x9D, 0x00, 0x00, 0x00, 0x00, 0xE8}}, /* length*4 bytes to rsp+0x70 */
    {0x66E161u, 20, {0x8B, 0x7A, 0x48, 0x48, 0x8D, 0x4C, 0x24, 0x70, 0x48, 0x8B, 0x52, 0x40, 0x44, 0x8B, 0xC7, 0x49, 0xC1, 0xE0, 0x02, 0xE8}}, /* the same, per candidate */
    {0x66DB89u, 5, {0x48, 0x8D, 0x44, 0x24, 0x70}},                   /* lea rax, [rsp+0x70]: 0xA106C0's buffer */
    {0xA107DDu, 8, {0x44, 0x8B, 0x43, 0x48, 0x48, 0x8B, 0x53, 0x40}}, /* 0xA106C0 reads the row's length and code */
    {0x1836C82u, 4, {0x48, 0x8D, 0x45, 0xE0}},                        /* lea rax, [rbp-0x20]: the HUD's 16-direction buffer */
    {0x18379C9u, 4, {0x48, 0x8D, 0x45, 0xE0}},
    {0xA903ABu, 9, {0x41, 0x8B, 0x04, 0x24, 0x83, 0xF8, 0x10, 0x0F, 0x83}}, /* the input keeps at most 16 presses */
    {0x17EFB39u, 18, {0x48, 0x69, 0xC8, 0x58, 0x01, 0x00, 0x00, 0x48, 0x81, 0xC1, 0x30, 0x22, 0x00, 0x00, 0x48, 0x03, 0xCE, 0xE8}}, /* the arcade's arrows */
    {0x17EFB4Fu, 7, {0xFF, 0xC7, 0x83, 0xFF, 0x0A, 0x72, 0xDD}},   /* ...10 of them (0x158 bytes each, from +0x2230) */
};

typedef struct
{
    uint32_t packed;            /* what the stats layers write (StratagemCode +0) */
    uint32_t directions[SCODE_MAX];
    uint32_t shown;             /* the packed code the array and the row hold now */
    uintptr_t row;
    uint32_t refusedLogged;     /* the last packed value logged as not a code */
} CodeSlot;
static CodeSlot g_codes[STR_VANILLA];
static int g_codesState; /* 0 not yet, 1 installed, -1 refused (pins or rows) */

/* A code's directions packed (base 5, first lowest): 0 when it has none, or more than SCODE_MAX, or a direction not 1..4. */
static uint32_t CodePack(const uint32_t* directions, uint32_t length)
{
    if (length == 0 || length > SCODE_MAX) return 0;
    uint32_t packed = 0;
    for (uint32_t i = length; i-- > 0;)
    {
        if (directions[i] < 1 || directions[i] > 4) return 0;
        packed = packed * 5 + directions[i];
    }
    return packed;
}

/* A packed code's directions and their count (0: not a code: empty, a 0 digit before the last, or past SCODE_MAX). */
static uint32_t CodeUnpack(uint32_t packed, uint32_t* directions)
{
    uint32_t length = 0;
    while (packed != 0)
    {
        uint32_t d = packed % 5;
        if (d == 0 || length == SCODE_MAX) return 0;
        directions[length++] = d;
        packed /= 5;
    }
    return length;
}

static int CodeValid(uint32_t packed) { uint32_t d[SCODE_MAX]; return CodeUnpack(packed, d) != 0; }

/* A table kind's stats phase (projectile 0, damage 1, stratagem 2: a code goes with its stratagem's rows), else -1. */
static int CodesTablePhase(const char* kind)
{
    if (!strcmp(kind, "ProjectileSettings")) return 0;
    if (!strcmp(kind, "DamageInfo")) return 1;
    return !strcmp(kind, "StratagemDefinition") || !strcmp(kind, "StratagemCode") ? 2 : -1;
}

/* The pinned reads, as this build has them: NULL, else what differs. */
static const char* CodesPinsDiffer(uintptr_t game)
{
    static char why[96];
    for (size_t i = 0; i < sizeof kCodePins / sizeof kCodePins[0]; i++)
    {
        uint8_t now[24];
        if (SafeRead(game + kCodePins[i].rva, now, kCodePins[i].length) || memcmp(now, kCodePins[i].bytes, kCodePins[i].length) != 0)
        {
            snprintf(why, sizeof why, "game.dll+%X is not the reviewed code reader", kCodePins[i].rva);
            return why;
        }
    }
    return NULL;
}

/* One vanilla row's code into its slot, the row pointed at the slot's array: 0 when the row's code is not one HellForge
 * reads (left as it is). */
static int CodesTake(CodeSlot* s, uintptr_t row)
{
    uint64_t code = 0;
    uint32_t length = 0, directions[SCODE_MAX] = {0};
    if (SafeRead(row + SCODE_ROW_CODE, &code, 8) || SafeRead(row + SCODE_ROW_LENGTH, &length, 4) || code == 0 ||
        length == 0 || length > SCODE_MAX || SafeRead((uintptr_t)code, directions, length * 4)) return 0;
    uint32_t packed = CodePack(directions, length);
    if (packed == 0) return 0;
    memcpy(s->directions, directions, sizeof directions);
    s->packed = s->shown = packed;
    s->row = row;
    uint64_t mine = (uint64_t)(uintptr_t)s->directions;
    return !ApiWrite(row + SCODE_ROW_CODE, &mine, 8);
}

/* Every vanilla row of the table (row pointers by type) takes its own array, once: 1 installed, -1 refused (logged). */
static int CodesInstallAt(uintptr_t game, uintptr_t table)
{
    if (g_codesState != 0) return g_codesState;
    const char* why = strcmp(g_build, "25480438") ? "unsupported build" : CodesPinsDiffer(game);
    if (why != NULL) { Note("stratagem codes: refused: %s; code edits are refused", why); return g_codesState = -1; }
    int taken = 0;
    for (uint32_t type = 1; type < STR_VANILLA; type++)
    {
        uintptr_t row = 0; uint32_t id = 0;
        if (SafeRead(table + type * 8u, &row, 8) || row == 0 || SafeRead(row, &id, 4) || id != type) continue;
        taken += CodesTake(&g_codes[type], row);
    }
    Note("stratagem codes: %d vanilla codes in HellForge's arrays", taken);
    return g_codesState = 1;
}

/* The slot of the vanilla stratagem whose stable id is `stable`, or NULL. */
static CodeSlot* CodesSlotOf(uint32_t stable)
{
    for (uint32_t type = 1; type < STR_VANILLA; type++)
    {
        uint32_t id = 0;
        if (g_codes[type].row != 0 && !SafeRead(g_codes[type].row + 4, &id, 4) && id == stable) return &g_codes[type];
    }
    return NULL;
}

/* Why a stats edit of a code or a code's length is refused, or NULL: a packed value that is not a code, or a length the
 * array does not have directions for (original: the field's value before any stats mod). */
static const char* CodesRefusal(const StatEdit* e, uint32_t original)
{
    if (!strcmp(e->kind, "StratagemCode") && !CodeValid(e->value)) return "not a code of 1 to 10 directions";
    if (!strcmp(e->kind, "StratagemDefinition") && e->offset == SCODE_ROW_LENGTH && (e->value == 0 || e->value > (g_codesState == 1 ? SCODE_MAX : original)))
        return "a code length past its directions (edit the input code instead)";
    return NULL;
}

/* A StratagemCode field (record: the stable id; offset 0): its packed u32, or 0 with *why. */
static uintptr_t CodesField(const StatEdit* e, const char** why)
{
    CodeSlot* s = e->offset == 0 && g_codesState == 1 && e->record >= 0 && e->record <= UINT32_MAX ? CodesSlotOf((uint32_t)e->record) : NULL;
    if (s == NULL) *why = g_codesState < 0 ? "stratagem codes are refused on this build" : "no vanilla stratagem with that code";
    return s ? (uintptr_t)&s->packed : 0;
}

/* fields.c's row of a StratagemCode table (by type, as live requests name stratagems): the slot's packed u32. */
static uintptr_t CodesRowOf(uint64_t type, int* ready)
{
    *ready = g_codesState != 0;
    return g_codesState == 1 && type >= 1 && type < STR_VANILLA && g_codes[type].row ? (uintptr_t)&g_codes[type].packed : 0;
}

/* The game-thread update: an array whose packed code changed takes it, directions first and then the row's length (the
 * matcher and the HUD read both each time). A packed value that is not a code is left out, once logged. */
static void CodesTick(void)
{
    if (g_codesState != 1) return;
    for (uint32_t type = 1; type < STR_VANILLA; type++)
    {
        CodeSlot* s = &g_codes[type];
        uint32_t packed = s->packed;
        if (s->row == 0 || packed == s->shown) continue;
        uint32_t directions[SCODE_MAX] = {0}, length = CodeUnpack(packed, directions);
        if (length == 0)
        {
            if (s->refusedLogged != packed) Note("stratagem codes: type %u: %08x is not a code of 1 to %u directions; kept the last", type, packed, SCODE_MAX);
            s->refusedLogged = packed;
            continue;
        }
        memcpy(s->directions, directions, sizeof directions);
        if (ApiWrite(s->row + SCODE_ROW_LENGTH, &length, 4)) continue;
        s->shown = packed;
    }
}
