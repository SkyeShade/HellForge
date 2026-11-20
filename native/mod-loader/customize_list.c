/* A longer customize screen card list, build 25480438. The screen object S keeps 64 entries in place at S+0x40 with its
 * own fields right after them (S+0x640..0x660) and its UI from S+0x660, and BuildCustomizationList appends with no bound,
 * so a weapon with more cards writes over them. Each screen gets a frame instead: a copy of S's layout with room for
 * every card a weapon can have, its fields CL_SHIFT further on, CL_CAP cards shown. The build runs on the frame, the UI
 * is handed the frame's list, and the 75 instructions that reach those fields read and write them CL_SHIFT further on
 * (the same length, every byte pinned).
 * S keeps a copy of the first 64 entries for the code that still walks S itself. Off with HELLFORGE_CUSTOMIZE_LIST=0.
 * registered-items.json says when it is on ("customizeCards"), and only then does the backend serve a weapon more than
 * RegisteredItems.PatternsPerWeapon mods' patterns. Included by patterns.c. */
#define CL_CAP 127u                 /* the most an imm8 bound says */
#define CL_SHIFT 0x69C0u            /* the fields move from S+0x640 to frame+0x7000, past CL_ROOM entries */
#define CL_ENTRIES 0x40u
#define CL_ENTRY 0x18u              /* {item*, picked u8 at +8, unlocked u8 at +9, ...} */
#define CL_FIELDS 0x640u
#define CL_UI 0x660u
#define CL_WEAPON 0x10u             /* the weapon's id */
#define CL_PICKS 0x14u              /* the picked item of each of the weapon's CL_SLOTS slots */
#define CL_SLOTS 10u
#define CL_COUNT 0x3Cu              /* the entries in the list */
#define CL_GAME_ENTRIES 64u         /* what S itself holds */
#define CL_ROOM (CL_GAME_ENTRIES + ATT_MAX) /* a weapon's own cards fit S; mods' are attachments and weapon patterns */
#define CL_COMMIT 0x8000u           /* the list, its fields and their neighbours */
#define CL_BLOCK 0x8E8D00u          /* the weapon's item block and its count, which the build frees and allocates */
#define CL_FRAME_BYTES 0x8E9000u
#define CL_FRAMES 8
#define CL_BUILD 0x1493770u
#define CL_CLOSE 0x1493100u
#define CL_HANDOFF 0x19FD660u       /* (ui, list): the UI keeps the list it is handed */
#define CL_SET_PICK 0x1494250u      /* (S, slot, item) */
#define CL_MARK 0x19FE500u          /* (ui, hash): marks an entry unlocked in the UI's list */
/* The build appends with no bound of its own, so the frame holds every card a weapon can have. */
_Static_assert(CL_FIELDS + CL_SHIFT >= CL_ENTRIES + CL_ENTRY * CL_ROOM, "the longest build stays clear of the fields");
_Static_assert(CL_FIELDS + CL_SHIFT + 0x40u <= CL_COMMIT && CL_COMMIT <= (CL_BLOCK & ~0xFFFu), "the fields' pages are committed");

/* rva, length, where the disp32 is, the game's bytes */
typedef struct { uint32_t rva; uint8_t len, at; uint8_t pin[9]; } ClField;
static const ClField kClFields[] = {
    {0xB66A11u, 7, 3, {0x44,0x88,0xb0,0x48,0x06,0x00,0x00}},
    {0x12C3EA6u, 7, 3, {0x40,0x88,0xb0,0x48,0x06,0x00,0x00}},
    {0x1493914u, 9, 5, {0x66,0x41,0x0f,0x7e,0x8f,0x48,0x06,0x00,0x00}},
    {0x1493921u, 7, 3, {0x41,0x89,0x87,0x40,0x06,0x00,0x00}},
    {0x1493936u, 7, 3, {0x41,0x89,0x87,0x4c,0x06,0x00,0x00}},
    {0x1493A87u, 7, 3, {0x41,0x8b,0x87,0x48,0x06,0x00,0x00}},
    {0x1493AA7u, 7, 3, {0x41,0x2b,0x87,0x40,0x06,0x00,0x00}},
    {0x1493AAEu, 7, 3, {0x41,0x89,0x87,0x44,0x06,0x00,0x00}},
    {0x149454Cu, 7, 2, {0xc6,0x80,0x48,0x06,0x00,0x00,0x00}},
    {0x19FE574u, 7, 2, {0xc6,0x81,0x48,0x06,0x00,0x00,0x00}},
    {0x19FE5F2u, 7, 2, {0xc6,0x81,0x48,0x06,0x00,0x00,0x00}},
    {0x19FE6AEu, 7, 2, {0xc6,0x81,0x48,0x06,0x00,0x00,0x00}},
    {0x19FE7FCu, 7, 2, {0xc6,0x80,0x48,0x06,0x00,0x00,0x00}},
    {0x19FF304u, 7, 2, {0x80,0xba,0x48,0x06,0x00,0x00,0x00}},
    {0x19FF319u, 7, 3, {0x4c,0x8b,0xa2,0x50,0x06,0x00,0x00}},
    {0x19FF6DDu, 7, 3, {0x4c,0x89,0xb0,0x50,0x06,0x00,0x00}},
    {0x19FF906u, 8, 3, {0x41,0x80,0xba,0x48,0x06,0x00,0x00,0x00}},
    {0x19FF94Du, 7, 2, {0x80,0xb8,0x48,0x06,0x00,0x00,0x00}},
    {0x19FFA99u, 7, 2, {0x80,0xb8,0x48,0x06,0x00,0x00,0x00}},
    {0x19FFAADu, 7, 2, {0xc6,0x80,0x48,0x06,0x00,0x00,0x01}},
    {0x19FFAC0u, 7, 3, {0x48,0x89,0x81,0x50,0x06,0x00,0x00}},
    {0x19FFD5Au, 7, 2, {0x80,0xb8,0x48,0x06,0x00,0x00,0x00}},
    {0x19FFE1Au, 7, 2, {0x80,0xb8,0x48,0x06,0x00,0x00,0x00}},
    {0x19FFE2Au, 7, 2, {0xc6,0x80,0x48,0x06,0x00,0x00,0x01}},
    {0x19FFE3Du, 7, 3, {0x48,0x89,0x81,0x50,0x06,0x00,0x00}},
    {0x1A00180u, 7, 3, {0x0f,0xb6,0x80,0x48,0x06,0x00,0x00}},
    {0x1A0024Cu, 8, 3, {0x41,0xc6,0x82,0x48,0x06,0x00,0x00,0x00}},
    {0x1A002A6u, 8, 3, {0x41,0xc6,0x82,0x48,0x06,0x00,0x00,0x00}},
    {0x1A003A0u, 7, 2, {0xc6,0x80,0x48,0x06,0x00,0x00,0x00}},
    {0x1A00B79u, 7, 2, {0x80,0xba,0x48,0x06,0x00,0x00,0x00}},
    {0x1A00B8Eu, 7, 3, {0x4c,0x8b,0xa2,0x50,0x06,0x00,0x00}},
    {0x1A00F4Du, 7, 3, {0x4c,0x89,0xb0,0x50,0x06,0x00,0x00}},
    {0x1A011A5u, 7, 2, {0x80,0xb8,0x48,0x06,0x00,0x00,0x00}},
    {0x1A011EFu, 7, 2, {0x80,0xb8,0x48,0x06,0x00,0x00,0x00}},
    {0x1A0136Eu, 7, 2, {0x80,0xb8,0x48,0x06,0x00,0x00,0x00}},
    {0x1A0137Eu, 7, 2, {0xc6,0x80,0x48,0x06,0x00,0x00,0x01}},
    {0x1A01391u, 7, 3, {0x48,0x89,0x81,0x50,0x06,0x00,0x00}},
    {0x1A016D0u, 7, 2, {0x80,0xb8,0x48,0x06,0x00,0x00,0x00}},
    {0x1A017BBu, 7, 2, {0x80,0xb8,0x48,0x06,0x00,0x00,0x00}},
    {0x1A017CBu, 7, 2, {0xc6,0x80,0x48,0x06,0x00,0x00,0x01}},
    {0x1A017DEu, 7, 3, {0x48,0x89,0x81,0x50,0x06,0x00,0x00}},
    {0x1A01B40u, 7, 3, {0x0f,0xb6,0x80,0x48,0x06,0x00,0x00}},
    {0x1A01C0Cu, 8, 3, {0x41,0xc6,0x82,0x48,0x06,0x00,0x00,0x00}},
    {0x1A01C90u, 8, 3, {0x41,0xc6,0x82,0x48,0x06,0x00,0x00,0x00}},
    {0x1A01D80u, 7, 2, {0xc6,0x80,0x48,0x06,0x00,0x00,0x00}},
    {0x1A02F12u, 6, 2, {0x8b,0x87,0x3c,0x06,0x00,0x00}},
    {0x1A02F1Fu, 6, 2, {0x8b,0x97,0x38,0x06,0x00,0x00}},
    {0x1A02F7Au, 7, 3, {0x44,0x8b,0x87,0x40,0x06,0x00,0x00}},
    {0x1A02F95u, 7, 3, {0x44,0x8b,0x87,0x44,0x06,0x00,0x00}},
    {0x1A02FC2u, 6, 2, {0xf7,0xa7,0x40,0x06,0x00,0x00}},
    {0x1A03255u, 6, 2, {0x8b,0x92,0x38,0x06,0x00,0x00}},
    {0x1A03268u, 6, 2, {0x8b,0x87,0x3c,0x06,0x00,0x00}},
    {0x1A032BDu, 7, 3, {0x44,0x8b,0x87,0x40,0x06,0x00,0x00}},
    {0x1A032D8u, 7, 3, {0x44,0x8b,0x87,0x44,0x06,0x00,0x00}},
    {0x1A03305u, 6, 2, {0xf7,0xa7,0x40,0x06,0x00,0x00}},
    {0x1A03393u, 6, 2, {0x8b,0x82,0x40,0x06,0x00,0x00}},
    {0x1A03740u, 7, 3, {0x44,0x8b,0xb2,0x40,0x06,0x00,0x00}},
    {0x1A043BEu, 7, 3, {0x45,0x8b,0x80,0x40,0x06,0x00,0x00}},
    {0x1A04A6Du, 7, 3, {0x45,0x8b,0x83,0x40,0x06,0x00,0x00}},
    {0x1A04E8Fu, 6, 2, {0x8b,0x90,0x40,0x06,0x00,0x00}},
    {0x1A06322u, 6, 2, {0x8b,0x85,0x3c,0x06,0x00,0x00}},
    {0x1A0632Fu, 6, 2, {0x8b,0x95,0x38,0x06,0x00,0x00}},
    {0x1A0638Cu, 7, 3, {0x44,0x8b,0x85,0x40,0x06,0x00,0x00}},
    {0x1A063A7u, 7, 3, {0x44,0x8b,0x85,0x44,0x06,0x00,0x00}},
    {0x1A063D4u, 6, 2, {0xf7,0xa5,0x40,0x06,0x00,0x00}},
    {0x1A06485u, 6, 2, {0x8b,0x92,0x38,0x06,0x00,0x00}},
    {0x1A06498u, 6, 2, {0x8b,0x87,0x3c,0x06,0x00,0x00}},
    {0x1A064EDu, 7, 3, {0x44,0x8b,0x87,0x40,0x06,0x00,0x00}},
    {0x1A06508u, 7, 3, {0x44,0x8b,0x87,0x44,0x06,0x00,0x00}},
    {0x1A06535u, 6, 2, {0xf7,0xa7,0x40,0x06,0x00,0x00}},
    {0x1A065BEu, 6, 2, {0x8b,0x82,0x40,0x06,0x00,0x00}},
    {0x1A06C5Bu, 6, 2, {0x8b,0xaa,0x40,0x06,0x00,0x00}},
    {0x1A07A48u, 7, 3, {0x45,0x8b,0x83,0x40,0x06,0x00,0x00}},
    {0x1A07DECu, 7, 3, {0x41,0x8b,0x91,0x40,0x06,0x00,0x00}},
    {0x1A07E74u, 6, 2, {0x8b,0x90,0x40,0x06,0x00,0x00}}};
/* cmp eax, 0x40 in the UI's walks of its list */
static const uint32_t kClBounds[] = {0x19FE56Cu, 0x19FE659u, 0x1A00247u, 0x1A01C07u, 0x1A24F39u, 0x1A24FB9u};
static const uint8_t kClBuildPin[] = {0x40, 0x53, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00};
static const uint8_t kClClosePin[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10};
static const uint8_t kClHandoffPin[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10};
static const uint8_t kClSetPickPin[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x8B, 0xC2};
static const uint8_t kClMarkPin[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x48, 0x83, 0xEC, 0x20};
_Static_assert(sizeof kClFields / sizeof *kClFields == 75, "every instruction that reaches the fields");

typedef void (*ClBuildFn)(uint8_t* screen, const uint32_t* weapon);
typedef void (*ClScreenFn)(uint8_t* screen);
typedef void (*ClHandoffFn)(uint8_t* ui, uint8_t* list);
typedef void (*ClSetPickFn)(uint8_t* screen, uint32_t slot, uint32_t item);
typedef void (*ClMarkFn)(uint8_t* ui, uint32_t hash);
static ClBuildFn g_clBuild;
static ClScreenFn g_clClose;
static ClHandoffFn g_clHandoff;
static ClSetPickFn g_clSetPick;
static ClMarkFn g_clMark;
static int g_clState; /* 0 not yet, 1 on, -1 off or failed */
static struct { uint8_t* screen; uint8_t* frame; } g_clFrames[CL_FRAMES];
static unsigned g_clNext; /* the slot taken when all are in use */
static volatile LONG g_clOverNoted;

/* Only the pages the screen code reaches are committed: the list and its fields, and the item block's pointer. */
static uint8_t* ClAllocateFrame(void)
{
    uint8_t* frame = (uint8_t*)VirtualAlloc(NULL, CL_FRAME_BYTES, MEM_RESERVE, PAGE_NOACCESS);
    if (frame == NULL) return NULL;
    if (!VirtualAlloc(frame, CL_COMMIT, MEM_COMMIT, PAGE_READWRITE) || !VirtualAlloc(frame + (CL_BLOCK & ~0xFFFu), 0x1000, MEM_COMMIT, PAGE_READWRITE))
    {
        VirtualFree(frame, 0, MEM_RELEASE);
        return NULL;
    }
    return frame;
}

/* A new screen's frame starts as a copy of what S holds, so a list built before it (or a build that stops early)
 * reads the same. */
static void ClAdopt(uint8_t* screen, uint8_t* frame)
{
    memset(frame, 0, CL_COMMIT);
    memcpy(frame + 8, screen + 8, CL_FIELDS - 8);
    memcpy(frame + CL_FIELDS + CL_SHIFT, screen + CL_FIELDS, CL_UI - CL_FIELDS);
}

/* S's frame; with adopt, one is taken for a screen without: a free one, else the next in turn. */
static uint8_t* ClFrame(const uint8_t* screen, int adopt)
{
    int slot = -1;
    for (int i = 0; i < CL_FRAMES; i++)
    {
        if (g_clFrames[i].screen == screen && g_clFrames[i].frame) return g_clFrames[i].frame;
        if (slot < 0 && g_clFrames[i].screen == NULL) slot = i;
    }
    if (!adopt) return NULL;
    if (slot < 0) slot = (int)(g_clNext++ % CL_FRAMES);
    if (g_clFrames[slot].frame == NULL) g_clFrames[slot].frame = ClAllocateFrame();
    if (g_clFrames[slot].frame == NULL) slot = (int)(g_clNext++ & 1u); /* no memory: share one of the two made at install */
    g_clFrames[slot].screen = (uint8_t*)screen;
    ClAdopt((uint8_t*)screen, g_clFrames[slot].frame);
    return g_clFrames[slot].frame;
}

static uint8_t* ClEntry(uint8_t* frame, uint32_t i) { return frame + CL_ENTRIES + CL_ENTRY * i; }
static uint32_t ClItemId(const uint8_t* entry) { const uint8_t* item = *(uint8_t* const*)entry; return item ? *(const uint32_t*)(item + 8) : 0; }

/* S's own copy, for the code that walks S: the header, the first 64 entries with the count to match, the fields. */
static void ClShadow(uint8_t* screen, const uint8_t* frame)
{
    memcpy(screen + 8, frame + 8, CL_FIELDS - 8);
    if (*(uint32_t*)(screen + CL_COUNT) > CL_GAME_ENTRIES) *(uint32_t*)(screen + CL_COUNT) = CL_GAME_ENTRIES;
    memcpy(screen + CL_FIELDS, frame + CL_FIELDS + CL_SHIFT, CL_UI - CL_FIELDS);
}

/* Entries past CL_CAP are built but not shown; the count the UI walks stops at CL_CAP. */
static void ClClamp(uint8_t* frame)
{
    if (*(uint32_t*)(frame + CL_COUNT) > CL_CAP) *(uint32_t*)(frame + CL_COUNT) = CL_CAP;
    for (uint32_t i = CL_CAP; i < CL_ROOM; i++)
        if (*(uint64_t*)ClEntry(frame, i) && !InterlockedExchange(&g_clOverNoted, 1))
            Note("customize list: a weapon has more than %u cards; the rest are not shown", CL_CAP);
}

/* The game's build, on the frame: the item block it frees and allocates goes in and out with it. */
static void ClBuildDetour(uint8_t* screen, const uint32_t* weapon)
{
    uint8_t* frame = ClFrame(screen, 1);
    memcpy(frame + CL_BLOCK, screen + CL_BLOCK, 12);
    g_clBuild(frame, weapon);
    memcpy(screen + CL_BLOCK, frame + CL_BLOCK, 12);
    ClClamp(frame);
    ClShadow(screen, frame);
}

static void ClCloseDetour(uint8_t* screen)
{
    for (int i = 0; i < CL_FRAMES; i++)
        if (g_clFrames[i].screen == screen) g_clFrames[i].screen = NULL;
    g_clClose(screen);
}

/* The UI sits inside S at CL_UI; handed S's own list, it gets the frame's. */
static void ClHandoffDetour(uint8_t* ui, uint8_t* list)
{
    uint8_t* screen = ui - CL_UI;
    if (list == screen + 8) list = ClFrame(screen, 1) + 8;
    g_clHandoff(ui, list);
}

/* The first entry of that item gets picked set to value. */
static void ClPick(uint8_t* frame, uint32_t item, uint8_t value)
{
    for (uint32_t i = 0; i < CL_CAP; i++)
        if (*(uint64_t*)ClEntry(frame, i) && ClItemId(ClEntry(frame, i)) == item) { ClEntry(frame, i)[8] = value; return; } /* picked */
}

/* The game's pick change, on the frame's longer list too. */
static void ClSetPickDetour(uint8_t* screen, uint32_t slot, uint32_t item)
{
    uint8_t* frame = ClFrame(screen, 0);
    if (frame && slot < CL_SLOTS)
    {
        uint32_t* pick = (uint32_t*)(frame + CL_PICKS) + slot;
        ClPick(frame, *pick, 0);
        *pick = item;
        ClPick(frame, item, 1);
    }
    g_clSetPick(screen, slot, item);
}

/* The unlock the game marks on S (0x1494350, 0xB667D7) marks the frame too, whichever list the UI shows. The UI names
 * an entry by a hash of its item and the weapon (MurmurHash2's mix), so the frame's entry is found the same way. */
static void ClMarkDetour(uint8_t* ui, uint32_t hash)
{
    uint8_t* frame = ClFrame(ui - CL_UI, 0);
    if (frame)
    {
        uint32_t weapon = *(uint32_t*)(frame + CL_WEAPON) * 0x5BD1E995u;
        for (uint32_t i = 0; i < CL_CAP; i++)
        {
            if (*(uint64_t*)ClEntry(frame, i) == 0) continue;
            uint32_t h = ClItemId(ClEntry(frame, i)) * 0x5BD1E995u;
            if ((((h ^ (h >> 24)) * 0x5BD1E995u) ^ weapon) == hash) { ClEntry(frame, i)[9] = 1; break; } /* unlocked */
        }
    }
    g_clMark(ui, hash);
}

/* Every site and hooked function reads as reviewed. */
static int ClPins(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kClFields / sizeof *kClFields; i++)
        if (!PinMatches(game + kClFields[i].rva, kClFields[i].pin, kClFields[i].len)) return 0;
    static const uint8_t bound[3] = {0x83, 0xF8, 0x40};
    for (size_t i = 0; i < sizeof kClBounds / sizeof *kClBounds; i++)
        if (!PinMatches(game + kClBounds[i], bound, 3)) return 0;
    return PinMatches(game + CL_BUILD, kClBuildPin, sizeof kClBuildPin) && PinMatches(game + CL_CLOSE, kClClosePin, sizeof kClClosePin) &&
        PinMatches(game + CL_HANDOFF, kClHandoffPin, sizeof kClHandoffPin) && PinMatches(game + CL_SET_PICK, kClSetPickPin, sizeof kClSetPickPin) &&
        PinMatches(game + CL_MARK, kClMarkPin, sizeof kClMarkPin);
}

typedef struct { const char* what; uint32_t rva; void* detour; void** original; } ClHook;
static const ClHook kClHooks[] = {
        {"customize list: the screen's tear-down", CL_CLOSE, (void*)ClCloseDetour, (void**)&g_clClose},
        {"customize list: the UI's list", CL_HANDOFF, (void*)ClHandoffDetour, (void**)&g_clHandoff},
        {"customize list: a pick", CL_SET_PICK, (void*)ClSetPickDetour, (void**)&g_clSetPick},
        {"customize list: an unlock", CL_MARK, (void*)ClMarkDetour, (void**)&g_clMark},
        {"customize list: the build", CL_BUILD, (void*)ClBuildDetour, (void**)&g_clBuild}};
#define CL_HOOKS (sizeof kClHooks / sizeof *kClHooks)

/* The first count hooks off again. */
static void ClUnhook(uintptr_t game, size_t count)
{
    while (count-- > 0) HookRemove(NULL, game + kClHooks[count].rva, kClHooks[count].detour);
}

/* All five or none: the build on a frame needs the UI handed the frame's list. */
static int ClHooks(uintptr_t game)
{
    for (size_t i = 0; i < CL_HOOKS; i++)
        if (LoaderHook(kClHooks[i].what, game + kClHooks[i].rva, kClHooks[i].detour, kClHooks[i].original))
        {
            ClUnhook(game, i);
            return 1;
        }
    return 0;
}

#define CL_SITES (sizeof kClFields / sizeof *kClFields + sizeof kClBounds / sizeof *kClBounds)

/* Site i (the fields, then the bounds) as moved, or as pinned. */
static int ClSite(uintptr_t game, size_t i, int moved)
{
    const size_t fields = sizeof kClFields / sizeof *kClFields;
    if (i >= fields)
    {
        const uint8_t bound = moved ? (uint8_t)CL_CAP : (uint8_t)CL_GAME_ENTRIES;
        return ApiWrite(game + kClBounds[i - fields] + 2, &bound, 1);
    }
    uint32_t disp;
    memcpy(&disp, kClFields[i].pin + kClFields[i].at, 4);
    if (moved) disp += CL_SHIFT;
    return ApiWrite(game + kClFields[i].rva + kClFields[i].at, &disp, 4);
}

/* The fields first, the bounds last, so no stop leaves a UI walk past the entries the frame holds. All or none: on a
 * failed write the sites written go back to their pinned bytes; 1 then, 2 when one of those could not go back. */
static int ClWrite(uintptr_t game)
{
    for (size_t i = 0; i < CL_SITES; i++)
    {
        if (ClSite(game, i, 1) == 0) continue;
        int stuck = 0;
        while (i-- > 0) stuck |= ClSite(game, i, 0);
        return stuck ? 2 : 1;
    }
    return 0;
}

/* HELLFORGE_CUSTOMIZE_LIST=0 keeps the game's 64. */
static int ClWanted(void)
{
    wchar_t value[8];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_CUSTOMIZE_LIST", value, 8);
    return !(length > 0 && length < 8 && value[0] == L'0');
}

/* Once game.dll is loaded (WatchInstalls); all or nothing, and said either way. */
static void CustomizeListInstall(void)
{
    if (g_clState) return;
    g_clState = -1;
    if (strcmp(g_build, "25480438")) return;
    if (!ClWanted()) { Note("customize list: off (HELLFORGE_CUSTOMIZE_LIST=0); a weapon shows 64 cards"); return; }
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    const char* why = NULL;
    int written;
    if (!game || !ClPins(game)) why = "the screen code does not match the reviewed build";
    else if ((g_clFrames[0].frame = ClAllocateFrame()) == NULL || (g_clFrames[1].frame = ClAllocateFrame()) == NULL) why = "no memory for its frames";
    else if (ClHooks(game)) why = "the screen code could not be hooked";
    else if ((written = ClWrite(game)) != 0)
    {
        ClUnhook(game, CL_HOOKS);
        why = written == 1 ? "game.dll could not be rewritten: put back" : "game.dll could not be rewritten or put back; the customize screen may be broken this session";
    }
    if (why) { Note("customize list: a weapon shows 64 cards at most (%s)", why); return; }
    g_clState = 1;
    Note("customize list: on; a weapon shows %u cards", CL_CAP);
    WriteRegisteredItems(); /* the backend's word to serve more patterns */
}

/* registered-items.json's "customizeCards" (registry.c): the cards a customize screen holds, 0 for the game's 64. */
static int CustomizeCards(void) { return g_clState == 1 ? (int)CL_CAP : 0; }

