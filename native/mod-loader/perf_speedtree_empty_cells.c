/* SpeedTree's forest view pass (exe 0x212150, build 25480438) gives a cell missing from the forest hash an all-zero
 * box. Its cull (0x953B70) passes that box in every view that holds the world origin, so each absent cell becomes an
 * empty node that later passes walk. HellForge stores the empty box instead (+FLT_MAX min, -FLT_MAX max): the value
 * every cell is built with and a present cell with no trees gets, which the cull skips at its first test. Included by
 * hellforge_modloader.c after render_guards.c: PinMatches (packages.c), JournalPut (live_code.c), PerfFixOn. */

#define ST_SITE 0x21265Du  /* the absent cell's box, 31 bytes; r14 is the cell */
#define ST_LENGTH 31
#define ST_EMPTY 0x16AA990u /* the game's own empty box constant: +FLT_MAX x3, -FLT_MAX; 16-aligned, read-only */
#define ST_PROBE 0x212650u  /* the hash probe's miss falls into the site */
#define ST_NEXT 0x21267Cu   /* the next cell: the replaced bytes end here */
#define ST_FOUND 0x21288Bu  /* a present cell's box from its hash entry: the same layout, then a jump to ST_NEXT */
#define ST_CALL 0x2126A0u   /* the cull's only call */
#define ST_SKIP 0x953C67u   /* the cull's first test: a cell whose box is the empty one is skipped */
#define ST_FLT_MAX 0x16A9E64u
#define ST_FLT_MIN 0x16A9F48u

/* xorps xmm0, xmm0; movups [r14+0x58], xmm0; movups [rbp+0x10], xmm0; movaps xmm0, xmm9 (0); unpcklps xmm0, xmm9;
 * movsd [rbp+0x20], xmm0; movsd [r14+0x68], xmm0. Nothing in the function reads rbp+0x10..0x27 again. */
static const uint8_t kStOld[ST_LENGTH] = {0x0f,0x57,0xc0, 0x41,0x0f,0x11,0x46,0x58, 0x0f,0x11,0x45,0x10, 0x41,0x0f,0x28,0xc1,
    0x41,0x0f,0x14,0xc1, 0xf2,0x0f,0x11,0x45,0x20, 0xf2,0x41,0x0f,0x11,0x46,0x68};
static const uint8_t kStProbePin[] = {0x42,0x8b,0x54,0x11,0x60, 0x81,0xfa,0xff,0xff,0xff,0x7f, 0x75,0xe3};
static const uint8_t kStNextPin[] = {0x41,0xff,0xc7, 0x41,0x8b,0xc7, 0x49,0x3b,0x04,0x24};
static const uint8_t kStFoundPin[] = {0x41,0x0f,0x11,0x46,0x58, 0xf2,0x0f,0x10,0x4c,0x01,0x58, 0xf2,0x41,0x0f,0x11,0x4e,0x68,
    0xe9,0xdb,0xfd,0xff,0xff};
static const uint8_t kStCallPin[] = {0xe8,0xcb,0x14,0x74,0x00};
static const uint8_t kStSkipPin[] = {0xf3,0x43,0x0f,0x10,0x5c,0x2e,0x58, 0x41,0x0f,0x2e,0xda, 0x7a,0x51, 0x75,0x4f};
static const uint8_t kStEmptyPin[] = {0xff,0xff,0x7f,0x7f, 0xff,0xff,0x7f,0x7f, 0xff,0xff,0x7f,0x7f, 0xff,0xff,0x7f,0xff};

static int g_stState; /* 0 not asked yet, 2 waiting for the exe's code, 1 installed, -1 not */
static ULONGLONG g_stLoadedAt;

static int JournalPut(uintptr_t address, const uint8_t* bytes, size_t size); /* live_code.c */

/* movaps xmm0, [ST_EMPTY]; movups [r14+0x58], xmm0; shufps xmm0, xmm0, 0xFF; movsd [r14+0x68], xmm0; a 9-byte nop.
 * Only xmm0 changes, as before, and it is dead at ST_NEXT; no flags, no general register. */
static void StCode(uint8_t* code)
{
    static const uint8_t tail[] = {0x41,0x0f,0x11,0x46,0x58, 0x0f,0xc6,0xc0,0xff, 0xf2,0x41,0x0f,0x11,0x46,0x68,
        0x66,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00};
    int32_t rel = (int32_t)(ST_EMPTY - (ST_SITE + 7)); /* both in the exe's image: the same at any base */
    code[0] = 0x0f; code[1] = 0x28; code[2] = 0x05;
    memcpy(code + 3, &rel, 4);
    memcpy(code + 7, tail, sizeof tail);
}

/* The replaced bytes are read as they are, not through a hook: a hook there would be overwritten. */
static int StPinned(uintptr_t exe)
{
    uint8_t now[ST_LENGTH];
    return !SafeRead(exe + ST_SITE, now, sizeof now) && !memcmp(now, kStOld, sizeof now) &&
        PinMatches(exe + ST_PROBE, kStProbePin, sizeof kStProbePin) && PinMatches(exe + ST_NEXT, kStNextPin, sizeof kStNextPin) &&
        PinMatches(exe + ST_FOUND, kStFoundPin, sizeof kStFoundPin) && PinMatches(exe + ST_CALL, kStCallPin, sizeof kStCallPin) &&
        PinMatches(exe + ST_SKIP, kStSkipPin, sizeof kStSkipPin) && PinMatches(exe + ST_EMPTY, kStEmptyPin, sizeof kStEmptyPin) &&
        PinMatches(exe + ST_FLT_MAX, kStEmptyPin, 4) && PinMatches(exe + ST_FLT_MIN, kStEmptyPin + 12, 4);
}

/* One write, with no thread in the bytes: every pass rewrites each cell's box and drops nodes it did not visit, so
 * nothing written before the fix outlives the next pass, and nothing it writes depends on the level. */
static void StWatchAt(uintptr_t exe)
{
    if (g_stState == 0)
    {
        if (strcmp(g_build, "25480438")) { g_stState = -1; Note("performance fix speedtree-empty-cells: not installed; not build 25480438"); return; }
        if (!PerfFixOn("speedtree-empty-cells")) { g_stState = -1; return; }
        g_stState = 2;
    }
    if (g_stState != 2) return;
    if (!StPinned(exe))
    {
        /* the exe's code is decoded some time after start; a minute after the game loaded, it never will be the reviewed one */
        if (!g_fired[HF_EVENT_GAME_LOADED]) return;
        if (g_stLoadedAt == 0) g_stLoadedAt = GetTickCount64();
        if (GetTickCount64() - g_stLoadedAt < 60000) return;
        g_stState = -1;
        Note("performance fix speedtree-empty-cells: not installed; the forest view pass is not the reviewed code");
        return;
    }
    uint8_t code[ST_LENGTH];
    StCode(code);
    if (JournalPut(exe + ST_SITE, code, sizeof code))
    {
        g_stState = -1;
        Note("performance fix speedtree-empty-cells: not installed; its code could not be rewritten");
        return;
    }
    g_stState = 1;
    Note("performance fix speedtree-empty-cells: installed; forest cells missing from the forest hash get the empty box every view skips");
}

static void PerfSpeedTreeWatch(void)
{
    if (g_stState == 0 || g_stState == 2) StWatchAt((uintptr_t)GetModuleHandleW(NULL));
}
