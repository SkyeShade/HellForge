/* Level 11's diagnostics (difficulty.c): the level generation restarts, the minimap capture, the height zones' dump and
 * the watch that logs them. Included by hellforge_modloader.c after difficulty_block.c. */

/* Diagnostics: a pass that fails asks for the level to be generated again (lg+0xF954 counts the restarts, lg+0xF958
 * names the first failed pass). Each such increment notes the level, and the loader's watcher logs the restarts. They
 * run only when a pass fails. FixUnreachableAreas, FinalizeVoronoi, SpreadLocations, GenerateConvoyRoutes. */
static const uint32_t kRestartSites[4] = {0xF67FDBu, 0xF6811Cu, 0xF7CBEAu, 0xF8B2C7u};
static const char* const kRestartPasses[4] = {"FixUnreachableAreas", "FinalizeVoronoi", "SpreadLocations", "GenerateConvoyRoutes"};
static const uint8_t kRestartPin[6] = {0xFF, 0x81, 0x54, 0xF9, 0x00, 0x00}; /* inc dword [rcx + 0xf954] (rcx = lg) */
static const uint8_t kRestartStub[25] = {
    0x48, 0x89, 0x0D, 0, 0, 0, 0,                                     /*  0 mov [rip+last level], rcx (rel32 at 3) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /*  7 lock inc dword [rip+site count] (rel32 at 10) */
    0xFF, 0x81, 0x54, 0xF9, 0x00, 0x00,                               /* 14 inc dword [rcx+0xf954] */
    0xE9, 0, 0, 0, 0};                                                /* 20 jmp back (rel32 at 21) */
/* On a larger map FixUnreachableAreas (0xF67980) can fail on every try: some pocket of route cells has no walkable
 * neighbour to open (0xF66B80), and the level is generated again forever (Sabotage Supply Bases at 2 x: 35+ tries; the
 * preview never came). For a larger map's level, from the 7th try the pocket is left as it is and the pass goes on
 * (0xF67FB7, the path after a fixed area). */
#define DIF_UNREACHABLE_GO_ON 0xF67FB7u
#define DIF_UNREACHABLE_TRIES 6
static const uint8_t kUnreachableStub[58] = {
    0x48, 0x89, 0x0D, 0, 0, 0, 0,                                     /*  0 mov [rip+last level], rcx (rel32 at 3) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /*  7 lock inc dword [rip+site count] (rel32 at 10) */
    0x81, 0xB9, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,                   /* 14 cmp dword [rcx+0x2d7c], disk / 1024 (at 20) */
    0x75, 0x15,                                                       /* 24 jne 47 */
    0x83, 0xB9, 0x54, 0xF9, 0x00, 0x00, DIF_UNREACHABLE_TRIES,        /* 26 cmp dword [rcx+0xf954], tries */
    0x72, 0x0C,                                                       /* 33 jb 47 */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 35 lock inc dword [rip+given up] (rel32 at 38) */
    0xE9, 0, 0, 0, 0,                                                 /* 42 jmp go on (rel32 at 43) */
    0xFF, 0x81, 0x54, 0xF9, 0x00, 0x00,                               /* 47 inc dword [rcx+0xf954] */
    0xE9, 0, 0, 0, 0};                                                /* 53 jmp back (rel32 at 54) */
/* Diagnostic: CreateMinimapTexture (0xF586A0, r8 = pass, [r8 + 8] = lg+0x2D20) notes the radii it captures with. */
#define DIF_CAPTURE_SITE 0xF586A0u
static const uint8_t kCapturePin[11] = {0x4C, 0x8B, 0xDC, 0x55, 0x49, 0x8D, 0xAB, 0x98, 0xFD, 0xFF, 0xFF};
static const uint8_t kCaptureStub[71] = {
    0x50, 0x52,                                                       /*  0 push rax; push rdx */
    0x49, 0x8B, 0x40, 0x08,                                           /*  2 mov rax, [r8+8] (lg+0x2D20) */
    0x8B, 0x50, 0x50,                                                 /*  6 mov edx, [rax+0x50] (lg+0x2D70) */
    0x89, 0x15, 0, 0, 0, 0,                                           /*  9 mov [rip+watch+0x28], edx (rel32 at 11) */
    0x8B, 0x50, 0x54,                                                 /* 15 mov edx, [rax+0x54] (lg+0x2D74) */
    0x89, 0x15, 0, 0, 0, 0,                                           /* 18 mov [rip+watch+0x2c], edx (rel32 at 20) */
    0x8B, 0x50, 0x60,                                                 /* 24 mov edx, [rax+0x60] (lg+0x2D80) */
    0x89, 0x15, 0, 0, 0, 0,                                           /* 27 mov [rip+watch+0x30], edx (rel32 at 29) */
    0x0F, 0xB6, 0x90, 0xD9, 0xD3, 0xFF, 0xFF,                         /* 33 movzx edx, byte [rax-0x2c27] (lg+0xF9) */
    0x89, 0x15, 0, 0, 0, 0,                                           /* 40 mov [rip+watch+0x38], edx (rel32 at 42) */
    0xF0, 0xFF, 0x05, 0, 0, 0, 0,                                     /* 46 lock inc dword [rip+watch+0x34] (rel32 at 49) */
    0x5A, 0x58,                                                       /* 53 pop rdx; pop rax */
    0x4C, 0x8B, 0xDC, 0x55, 0x49, 0x8D, 0xAB, 0x98, 0xFD, 0xFF, 0xFF, /* 55 mov r11, rsp; push rbp; lea rbp, [r11-0x268] */
    0xE9, 0, 0, 0, 0};                                                /* 66 jmp 0xF586AB (rel32 at 67) */

/* ---- the checks ------------------------------------------------------------------------------------------------- */

static const char* RestartSitesFailed(uintptr_t game)
{
    for (int i = 0; i < 4; i++)
    {
        uint8_t pin[6];
        if (SafeRead(game + kRestartSites[i], pin, 6) || memcmp(pin, kRestartPin, 6) != 0) return "a level generation restart is not the reviewed one";
    }
    return NULL;
}

static const char* CaptureSiteFailed(uintptr_t game)
{
    uint8_t pin[11];
    if (SafeRead(game + DIF_CAPTURE_SITE, pin, 11) || memcmp(pin, kCapturePin, 11) != 0) return "the minimap capture is not the reviewed one";
    return NULL;
}

/* ---- the install steps ------------------------------------------------------------------------------------------ */

/* The four restart notes (4 x 0x20): the level and a count per site in the watch, then the pass's own increment. */
static const char* PrepareRestartStubs(DifficultyInstall* d)
{
    d->restartStubs = d->code + 0x500;
    for (int i = 0; i < 4; i++)
    {
        uint8_t* stub = d->restartStubs + 0x20 * i;
        memcpy(stub, kRestartStub, sizeof kRestartStub);
        int32_t rel = (int32_t)(d->watch - (stub + 7)); memcpy(stub + 3, &rel, 4);
        rel = (int32_t)(d->watch + 8 + 4 * i - (stub + 14)); memcpy(stub + 10, &rel, 4);
        rel = (int32_t)(d->game + kRestartSites[i] + 6 - (uintptr_t)(stub + 25)); memcpy(stub + 21, &rel, 4);
    }
    return NULL;
}

/* The minimap capture's note: its radii and counts into the watch (+0x28..+0x38). */
static const char* PrepareCaptureStub(DifficultyInstall* d)
{
    uint8_t* captureStub = d->code + 0x6C0;
    d->captureStub = captureStub;
    static const uint8_t at[5] = {11, 20, 29, 42, 49}, field[5] = {0x28, 0x2C, 0x30, 0x38, 0x34}, end[5] = {15, 24, 33, 46, 53};
    int32_t rel;
    memcpy(captureStub, kCaptureStub, sizeof kCaptureStub);
    for (int i = 0; i < 5; i++) { rel = (int32_t)(d->watch + field[i] - (captureStub + end[i])); memcpy(captureStub + at[i], &rel, 4); }
    rel = (int32_t)(d->game + DIF_CAPTURE_SITE + 11 - (uintptr_t)(captureStub + 71)); memcpy(captureStub + 67, &rel, 4);
    return NULL;
}

/* A larger map's FixUnreachableAreas note, which goes on past a pocket from the 7th try. */
static const char* PrepareUnreachableStub(DifficultyInstall* d)
{
    d->unreachableStub = NULL;
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    float diskBits = d->disk / (1024.0f * (float)WorldScaleFor(g_difficulty.mapScale)); /* as the radii stub writes lg+0x2D7C */
    uint8_t* unreachableStub = d->code + 0x660;
    d->unreachableStub = unreachableStub;
    memcpy(unreachableStub, kUnreachableStub, sizeof kUnreachableStub);
    int32_t rel = (int32_t)(d->watch - (unreachableStub + 7)); memcpy(unreachableStub + 3, &rel, 4);
    rel = (int32_t)(d->watch + 8 - (unreachableStub + 14)); memcpy(unreachableStub + 10, &rel, 4);
    memcpy(unreachableStub + 20, &diskBits, 4);
    rel = (int32_t)(d->watch + 0x18 - (unreachableStub + 42)); memcpy(unreachableStub + 38, &rel, 4);
    rel = (int32_t)(game + DIF_UNREACHABLE_GO_ON - (uintptr_t)(unreachableStub + 47)); memcpy(unreachableStub + 43, &rel, 4);
    rel = (int32_t)(game + kRestartSites[0] + 6 - (uintptr_t)(unreachableStub + 58)); memcpy(unreachableStub + 54, &rel, 4);
    return NULL;
}

/* A larger map's FixUnreachableAreas: a pocket made of location cells only is joined to the main area like any other
 * (static, scratch unreach/, KB missions/xl-fix-unreachable-areas). The pass (0xF67980) flood-fills the unblocked
 * subregion cells into components (list of 32 {u8 has-location, u32 cells} entries, 0x100 bytes from the generation
 * arena at 0xF679C9 / 0xF67A0C) and joins each pocket to the main component by a path of at most 7 blocked cells
 * (0xF67210 -> 0xF66F70), but never starts that search from a pocket cell whose route cell holds a location
 * (cmp [rbp+0x98], 0 at 0xF67356, rbp = the cell's route cell): a pocket with a location and no location-free cell
 * fails on every try and restarts the level. Location zones are metres (6-36 m pads), so at 16 km (651 m route cells,
 * vanilla 40 m) a location's zone is about its own cell: such pockets are common (inferred: the cell types are game
 * data). On a larger level (lg+0x2D7C == the disk bits, as the stub above tests; lg = [ctx+0x18], ctx = r15 in
 * 0xF67210) location cells start searches too: path cells are only blocked cells, never the location's own, so the
 * location is untouched. The component list holds 512 entries instead of 32 (arena scratch; more than 32 components
 * overflowed into the id array that follows it). */
/* A larger level's pocket that FixUnreachableAreas could not join (its path search, 0xF67210 -> 0xF66F70, stops at 7
 * blocked cells and never steps on a blocked cell holding a location) is joined here before the pass would restart the
 * level: at the restart (0xF67FD6..0xF67FDB, still inside 0xF67980's frame: rsp and rbp are the pass's, r12 = ctx, ebx
 * = the pocket's component, edi = the main component, r13 = the subregion type objects) a breadth-first search from
 * every pocket cell over blocked cells that lie in the playable disk (0xF66A90(ctx, subregion)) and hold no location
 * (route cell +0x98) to an unblocked cell of the main component. Cells: route cells [record+8] (0x110 each; their 16
 * routes at +8, each route's two cells at +0x20 / +0x28, as the flood fill 0xF67540 walks them), subregions [rsp+0x78]
 * (0x20 each, cell i's; type +0, type object index +0xC), ids [rbp-0x70] (u32), cell count [rbp-0x40], the record
 * [rbp-0x68], the planet flag [rsp+0x64]. A cell is blocked as the pass tests it (0xF67BD8): type 1..6, outside the
 * disk, or with the planet flag a type object without bit 5 of +0x170. The path is applied as the pass applies its own
 * (0xF67E81..0xF67EF8): the pocket's cells (type not 1..6) take the main's id, each path cell takes +0..+0xF of the
 * pocket's last such cell, and its id too. 1 when joined (the pass goes on at 0xF67FB7). */
static volatile LONG g_reconnectJoined, g_reconnectFailed, g_reconnectLongest;
static uintptr_t g_reconnectGame;
static char DifUnreachableReconnect(uint8_t* frameRsp, uint8_t* frameRbp, uint64_t ctx, uint32_t pocket, uint64_t types, uint32_t mainId)
{
    uint64_t record = 0, rcBase = 0, subs = 0, idsAt = 0, cells = 0;
    uint8_t planet = 0;
    if (mainId == 0xFFFFFFFFu || SafeRead((uintptr_t)(frameRbp - 0x68), &record, 8) || record == 0 || SafeRead(record + 8, &rcBase, 8) || rcBase == 0 ||
        SafeRead((uintptr_t)(frameRsp + 0x78), &subs, 8) || subs == 0 || SafeRead((uintptr_t)(frameRbp - 0x70), &idsAt, 8) || idsAt == 0 ||
        SafeRead((uintptr_t)(frameRbp - 0x40), &cells, 8) || cells == 0 || cells > 65536 || SafeRead((uintptr_t)(frameRsp + 0x64), &planet, 1))
    { InterlockedIncrement(&g_reconnectFailed); return 0; }
    uint32_t n = (uint32_t)cells;
    uint32_t* ids = (uint32_t*)(uintptr_t)idsAt;
    typedef char (*InDiskFn)(uint64_t ctx, uint64_t subregion);
    InDiskFn inDisk = (InDiskFn)(g_reconnectGame + 0xF66A90u);
    int32_t* from = (int32_t*)VirtualAlloc(NULL, (size_t)n * 8 + n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (from == NULL) { InterlockedIncrement(&g_reconnectFailed); return 0; }
    uint32_t* queue = (uint32_t*)(from + n);
    uint8_t* blockedOf = (uint8_t*)(queue + n); /* 0 unknown, 1 open, 2 blocked and passable, 3 blocked and not */
    for (uint32_t i = 0; i < n; i++) from[i] = -2;
    uint32_t head = 0, tail = 0, last = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++)
        if (ids[i] == pocket)
        {
            uint32_t type = *(uint32_t*)(uintptr_t)(subs + (uint64_t)i * 0x20);
            if (type - 1u <= 5u) continue;
            from[i] = -1; queue[tail++] = i; last = i;
        }
    /* As the game walks it (0xF67C37..0xF67CF1): a subregion's route cell is [sub+0x18]; a neighbour is a route's
     * other cell (route +0x20 / +0x28), and its subregion index is (cell - [P+0]) / 0x110 (the game's [rbp-0x58]).
     * Every read is guarded, and a neighbour whose subregion does not name that cell back is left out (run s72g: a
     * walk from rcBase + i x 0x110 reached memory that was no subregion and 0xF66A90 faulted on it). */
    int32_t found = -1;
    uint32_t mismatched = 0;
    while (head < tail && found < 0)
    {
        uint32_t i = queue[head++];
        uint64_t rc = 0;
        if (SafeRead(subs + (uint64_t)i * 0x20 + 0x18, &rc, 8) || rc == 0) continue;
        for (int k = 0; k < 16 && found < 0; k++)
        {
            uint64_t route = 0;
            if (SafeRead(rc + 8 + 8 * (uint64_t)k, &route, 8) || route == 0) break;
            for (int e = 0; e < 2 && found < 0; e++)
            {
                uint64_t other = 0;
                if (SafeRead(route + 0x20 + 8 * (uint64_t)e, &other, 8) || other == 0 || other == rc || other < rcBase) continue;
                uint64_t j64 = (other - rcBase) / 0x110;
                if (j64 >= n || (other - rcBase) % 0x110 != 0) continue;
                uint32_t j = (uint32_t)j64;
                if (from[j] != -2) continue;
                if (blockedOf[j] == 0)
                {
                    uint64_t sub = subs + (uint64_t)j * 0x20, back = 0;
                    uint32_t type = 0;
                    if (SafeRead(sub + 0x18, &back, 8) || back != other || SafeRead(sub, &type, 4))
                    {
                        if (++mismatched <= 3)
                            Note("difficulty: FixUnreachableAreas: subregion %u (0x%llX) names route cell 0x%llX, not 0x%llX (cells from 0x%llX, subregions from 0x%llX, %u); left out",
                                j, (unsigned long long)sub, (unsigned long long)back, (unsigned long long)other, (unsigned long long)rcBase, (unsigned long long)subs, n);
                        blockedOf[j] = 3;
                        continue;
                    }
                    int disk = inDisk(ctx, sub) != 0;
                    int blocked = type - 1u <= 5u || !disk;
                    if (!blocked && planet)
                    {
                        int32_t t = 0; uint64_t obj = 0; uint8_t flags = 0;
                        blocked = SafeRead(sub + 0xC, &t, 4) || SafeRead(types + (uint64_t)(int64_t)t * 8, &obj, 8) || obj == 0 ||
                            SafeRead(obj + 0x170, &flags, 1) || !((flags >> 5) & 1);
                    }
                    uint64_t location = 0;
                    if (SafeRead(other + 0x98, &location, 8)) location = 1;
                    blockedOf[j] = !blocked ? 1 : disk && location == 0 ? 2 : 3;
                }
                if (blockedOf[j] == 1 && ids[j] == mainId) { from[j] = (int32_t)i; found = (int32_t)j; break; }
                if (blockedOf[j] != 2) continue;
                from[j] = (int32_t)i; queue[tail++] = j;
            }
        }
    }
    if (found < 0 || last == 0xFFFFFFFFu)
    {
        VirtualFree(from, 0, MEM_RELEASE);
        LONG failed = InterlockedIncrement(&g_reconnectFailed);
        if (failed <= 20) Note("difficulty: FixUnreachableAreas: pocket %u has no path to the main area %u over passable blocked cells (%u cells searched); the level restarts", pocket, mainId, tail);
        return 0;
    }
    uint8_t source[16];
    memcpy(source, (void*)(uintptr_t)(subs + (uint64_t)last * 0x20), 16);
    LONG length = 0;
    for (int32_t j = from[found]; j >= 0 && from[j] != -1; j = from[j])
    {
        memcpy((void*)(uintptr_t)(subs + (uint64_t)j * 0x20), source, 16);
        ids[j] = mainId; length++;
    }
    for (uint32_t i = 0; i < n; i++)
        if (ids[i] == pocket && (*(uint32_t*)(uintptr_t)(subs + (uint64_t)i * 0x20)) - 1u > 5u) ids[i] = mainId;
    VirtualFree(from, 0, MEM_RELEASE);
    LONG joined = InterlockedIncrement(&g_reconnectJoined);
    if (joined <= 20)
        Note("difficulty: FixUnreachableAreas: pocket %u joined to the main area %u by %ld blocked cells (breadth-first; %u cells searched); joins %ld, not joined %ld",
            pocket, mainId, (long)length, tail, (long)joined, (long)g_reconnectFailed);
    LONG longest = g_reconnectLongest;
    while (length > longest && InterlockedCompareExchange(&g_reconnectLongest, length, longest) != longest) longest = g_reconnectLongest;
    return 1;
}

/* The restart site 0xF67FDB jumps here (before the try counter, d->unreachableStub). */
static const char* InstallPocketReconnect(DifficultyInstall* d, uint8_t* stub)
{
    if (d->unreachableStub == NULL) return NULL;
    g_reconnectGame = d->game;
    float diskBits = d->disk / (1024.0f * (float)WorldScaleFor(g_difficulty.mapScale));
    uint8_t code[96] = {
        0x81, 0xB9, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0,   /*  0 cmp dword [rcx+0x2d7c], disk bits (at 6) */
        0x75, 0x3A,                                     /* 10 jne old (70) */
        0x48, 0x89, 0xE0,                               /* 12 mov rax, rsp */
        0x51,                                           /* 15 push rcx */
        0x48, 0x83, 0xEC, 0x38,                         /* 16 sub rsp, 0x38 */
        0x4C, 0x89, 0x6C, 0x24, 0x20,                   /* 20 mov [rsp+0x20], r13 (types) */
        0x89, 0x7C, 0x24, 0x28,                         /* 25 mov [rsp+0x28], edi (main) */
        0x48, 0x89, 0xC1,                               /* 29 mov rcx, rax (the pass's rsp) */
        0x48, 0x89, 0xEA,                               /* 32 mov rdx, rbp */
        0x4D, 0x89, 0xE0,                               /* 35 mov r8, r12 (ctx) */
        0x41, 0x89, 0xD9,                               /* 38 mov r9d, ebx (pocket) */
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,             /* 41 mov rax, DifUnreachableReconnect (at 43) */
        0xFF, 0xD0,                                     /* 51 call rax */
        0x48, 0x83, 0xC4, 0x38,                         /* 53 add rsp, 0x38 */
        0x59,                                           /* 57 pop rcx */
        0x84, 0xC0,                                     /* 58 test al, al */
        0x74, 0x0A,                                     /* 60 je old (72) */
        0xE9, 0, 0, 0, 0,                               /* 62 jmp go on 0xF67FB7 (rel32 at 63) */
        0x90, 0x90, 0x90, 0x90, 0x90,                   /* 67 */
        0xE9, 0, 0, 0, 0};                              /* 72 old: jmp the try counter (rel32 at 73) */
    /* 10: jne +0x3A lands at 72 */
    code[11] = (uint8_t)(72 - 12);
    memcpy(code + 6, &diskBits, 4);
    uint64_t fn = (uint64_t)(uintptr_t)DifUnreachableReconnect;
    memcpy(code + 43, &fn, 8);
    int32_t rel = (int32_t)(d->game + DIF_UNREACHABLE_GO_ON - (uintptr_t)(stub + 67)); memcpy(code + 63, &rel, 4);
    rel = (int32_t)((uintptr_t)d->unreachableStub - (uintptr_t)(stub + 77)); memcpy(code + 73, &rel, 4);
    memcpy(stub, code, 77);
    return NULL;
}

#define DIF_POCKET_SITE 0xF67356u
#define DIF_POCKET_SKIP 0xF67445u
#define DIF_POCKET_GO 0xF67364u
static const char* InstallPocketSearch(DifficultyInstall* d)
{
    if (!LargerMaps()) return NULL;
    uintptr_t game = d->game;
    if (ApiCodeMatches(game + DIF_POCKET_SITE, "E9")) return NULL; /* installed before */
    if (!ApiCodeMatches(game + DIF_POCKET_SITE, "48 83 BD 98 00 00 00 00 0F 85 E1 00 00 00") || !ApiCodeMatches(game + 0xF67224u, "4C 8B F9") ||
        !ApiCodeMatches(game + 0xF679C9u, "BA 00 01 00 00") || !ApiCodeMatches(game + 0xF67A0Cu, "41 B8 00 01 00 00"))
    { Note("difficulty: FixUnreachableAreas is not the reviewed code; location pockets keep vanilla's search"); return NULL; }
    uint8_t* stub = AllocateNearGame(game, 0x1000);
    if (stub == NULL) return "no room near game.dll for the location-pocket stub";
    float diskBits = d->disk / (1024.0f * (float)WorldScaleFor(g_difficulty.mapScale));
    uint8_t code[44] = {
        0x48, 0x83, 0xBD, 0x98, 0x00, 0x00, 0x00, 0x00, /*  0 cmp qword [rbp+0x98], 0 */
        0x74, 0x16,                                     /*  8 je go (32) */
        0x50,                                           /* 10 push rax */
        0x49, 0x8B, 0x47, 0x18,                         /* 11 mov rax, [r15+0x18] (lg) */
        0x81, 0xB8, 0x7C, 0x2D, 0x00, 0x00, 0, 0, 0, 0, /* 15 cmp dword [rax+0x2d7c], disk bits (at 21) */
        0x58,                                           /* 25 pop rax */
        0x0F, 0x85, 0, 0, 0, 0,                         /* 26 jne skip (rel32 at 28) */
        0xE9, 0, 0, 0, 0};                              /* 32 go: jmp back (rel32 at 33) */
    memcpy(code + 21, &diskBits, 4);
    int32_t rel = (int32_t)(game + DIF_POCKET_SKIP - (uintptr_t)(stub + 32)); memcpy(code + 28, &rel, 4);
    rel = (int32_t)(game + DIF_POCKET_GO - (uintptr_t)(stub + 37)); memcpy(code + 33, &rel, 4);
    memcpy(stub, code, sizeof code);
    InstallPocketReconnect(d, stub + 0x100);
    DWORD old = 0;
    if (!VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old)) return "the location-pocket stub could not be made executable";
    uint8_t jump[14] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    rel = DifficultyJumpRel(game + DIF_POCKET_SITE, (uintptr_t)stub); memcpy(jump + 1, &rel, 4);
    static const uint8_t list1[5] = {0xBA, 0x00, 0x10, 0x00, 0x00}, list2[6] = {0x41, 0xB8, 0x00, 0x10, 0x00, 0x00};
    if (ApiWrite(game + 0xF679C9u, list1, 5) || ApiWrite(game + 0xF67A0Cu, list2, 6)) return "the unreachable-areas component list could not be enlarged";
    if (ApiWrite(game + DIF_POCKET_SITE, jump, 14)) return "the location-pocket search could not be installed";
    if (d->unreachableStub != NULL)
    {
        uint8_t site[6] = {0xE9, 0, 0, 0, 0, 0x90};
        rel = DifficultyJumpRel(game + kRestartSites[0], (uintptr_t)(stub + 0x100)); memcpy(site + 1, &rel, 4);
        if (ApiWrite(game + kRestartSites[0], site, 6)) return "the unreachable-areas reconnect could not be installed";
    }
    Note("difficulty: FixUnreachableAreas: on a larger level location cells start the search for a path to the main area; 512 components; %s",
        d->unreachableStub != NULL ? "a pocket it cannot join is joined by a breadth-first path over blocked cells before a restart" : "no reconnect (no try counter)");
    return NULL;
}

static const char* JumpToRestartStubs(DifficultyInstall* d)
{
    for (int i = 0; i < 4; i++)
    {
        uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
        int32_t rel = DifficultyJumpRel(d->game + kRestartSites[i],
            (uintptr_t)(i == 0 && d->unreachableStub != NULL ? d->unreachableStub : d->restartStubs + 0x20 * i));
        memcpy(jump + 1, &rel, 4);
        if (ApiWrite(d->game + kRestartSites[i], jump, 6)) return "a level generation restart could not be noted";
    }
    return NULL;
}

static const char* JumpToCapture(DifficultyInstall* d)
{
    uint8_t jump[11];
    memset(jump, 0x90, sizeof jump);
    jump[0] = 0xE9;
    int32_t rel = DifficultyJumpRel(d->game + DIF_CAPTURE_SITE, (uintptr_t)d->captureStub);
    memcpy(jump + 1, &rel, 4);
    if (ApiWrite(d->game + DIF_CAPTURE_SITE, jump, sizeof jump)) return "the minimap capture could not be noted";
    return NULL;
}

/* ---- the watch (polled by the loader's thread) ------------------------------------------------------------------ */

/* zones-<n>.tsv's route cells: type, position, heights, visited and group; the cell types used into seenType. Returns
 * how many zone groups the cells have. */
static int ZoneCells(HANDLE file, uint64_t info, uint32_t cells, uint64_t marker, uint8_t* seenType)
{
    char line[512];
    DWORD written;
    int groups = 0;
    int n = snprintf(line, sizeof line, "cell\ttype\tx\ty\th_fc\th_108\tvisited\tgroup\n");
    WriteFile(file, line, (DWORD)n, &written, NULL);
    for (uint32_t i = 0; i < cells; i++)
    {
        uint8_t entry[0x20];
        uint64_t cell = 0;
        float pos[2] = {0}, fc = 0, h = 0;
        if (SafeRead(info + 0x20 * (uint64_t)i, entry, sizeof entry)) break;
        int32_t type;
        memcpy(&type, entry + 0xC, 4); memcpy(&cell, entry + 0x18, 8);
        if (type >= 0 && type < 256) seenType[type] = 1;
        SafeRead(cell, pos, 8); SafeRead(cell + 0xFC, &fc, 4); SafeRead(cell + 0x108, &h, 4);
        uint8_t visited = 0xFF;
        int32_t group = -1;
        if (marker) { SafeRead(marker + 8 * (uint64_t)i, &visited, 1); SafeRead(marker + 8 * (uint64_t)i + 4, &group, 4); }
        if (visited == 1 && group + 1 > groups) groups = group + 1;
        n = snprintf(line, sizeof line, "%u\t%d\t%.1f\t%.1f\t%.2f\t%.2f\t%u\t%d\n", i, type, pos[0], pos[1], fc, h, visited, group);
        WriteFile(file, line, (DWORD)n, &written, NULL);
    }
    return groups;
}

/* zones-<n>.tsv's used cell types: their flags, city mode, size, zone margin, raise and road. */
static void ZoneTypes(HANDLE file, uint64_t types, const uint8_t* seenType)
{
    char line[512];
    DWORD written;
    int n = snprintf(line, sizeof line, "type\tobject\tflags170\tmode1a8\tsize18c\tmargin208\traise20c\troad228\n");
    WriteFile(file, line, (DWORD)n, &written, NULL);
    for (int t = 0; t < 256; t++)
    {
        uint64_t object = 0;
        if (!seenType[t] || SafeRead(types + 8 * (uint64_t)t, &object, 8)) continue;
        uint8_t flags = 0;
        uint32_t mode = 0;
        float size = 0, margin = 0, raise = 0, road = 0;
        SafeRead(object + 0x170, &flags, 1); SafeRead(object + 0x1A8, &mode, 4); SafeRead(object + 0x18C, &size, 4);
        SafeRead(object + 0x208, &margin, 4); SafeRead(object + 0x20C, &raise, 4); SafeRead(object + 0x228, &road, 4);
        n = snprintf(line, sizeof line, "%d\t%#llx\t%#x\t%u\t%.2f\t%g\t%g\t%.2f\n", t, (unsigned long long)object, flags, mode, size, margin, raise, road);
        WriteFile(file, line, (DWORD)n, &written, NULL);
    }
}

/* zones-<n>.tsv's zone groups: the table's min, max and height. */
static void ZoneGroups(HANDLE file, uint64_t table, int groups)
{
    char line[512];
    DWORD written;
    int n = snprintf(line, sizeof line, "group\tmin\tmax\theight\n");
    WriteFile(file, line, (DWORD)n, &written, NULL);
    for (int g = 0; g < groups + 1 && g < 4096; g++)
    {
        float entry[3] = {0};
        if (SafeRead(table + 12 * (uint64_t)g, entry, 12)) break;
        n = snprintf(line, sizeof line, "%d\t%g\t%g\t%g\n", g, entry[0], entry[1], entry[2]);
        WriteFile(file, line, (DWORD)n, &written, NULL);
    }
}

/* zones-<n>.tsv (see kZoneNoteStub): per used cell type its flags (+0x170), city mode (+0x1A8), zone margin (+0x208)
 * and raise (+0x20C); per group the table's min / max / height; per route cell its type, position (route cell +0, +4),
 * heights +0xFC / +0x108, visited and group (zone cells). Read-only, from the generation arena while it is alive. */
static void ZoneDump(void)
{
    static int dumps;
    uint64_t table = g_zoneDiag[0], record = g_zoneDiag[1], level = g_zoneDiag[2], markerAt = g_zoneDiag[4], infoAt = g_zoneDiag[5];
    uint32_t cells = (uint32_t)g_zoneDiag[3];
    uint8_t difficulty = 0;
    uint64_t info = g_zoneDiag[6], types = g_zoneDiag[7];
    if (dumps >= 8 || SafeRead(level + 0xF9, &difficulty, 1) || difficulty != DIF_VANILLA + 1 || cells == 0 || cells > 8192 || info == 0) return;
    wchar_t name[32];
    swprintf(name, 32, L"zones-%d.tsv", ++dumps);
    HANDLE file = OpenLog(name);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[512];
    DWORD written;
    float radius = 0.0f;
    SafeRead(level + 0x2D70, &radius, 4);
    uint64_t marker = 0;
    if (markerAt && infoAt > info && (infoAt - 0x18 - info) % 0x20 == 0 && (infoAt - 0x18 - info) / 0x20 < cells)
        marker = markerAt - 4 - 8 * ((infoAt - 0x18 - info) / 0x20);
    int n = snprintf(line, sizeof line, "level\t%#llx\tplayable\t%.0f\tcells\t%u\trecord\t%#llx\tinfo\t%#llx\ttypes\t%#llx\tmarkers\t%#llx\ttable\t%#llx\n",
        (unsigned long long)level, radius * 1024.0, cells, (unsigned long long)record, (unsigned long long)info, (unsigned long long)types,
        (unsigned long long)marker, (unsigned long long)table);
    WriteFile(file, line, (DWORD)n, &written, NULL);
    uint8_t seenType[256] = {0};
    int groups = ZoneCells(file, info, cells, marker, seenType);
    ZoneTypes(file, types, seenType);
    ZoneGroups(file, table, groups);
    CloseHandle(file);
    Note("difficulty: zones-%d.tsv written (%u route cells, %d zone groups, playable %.0f m)", dumps, cells, groups, radius * 1024.0 * WorldScaleFor(g_difficulty.mapScale));
}

/* The height zones' dump, once their pass is done: its table filled, its cell cursor still for 300 ms. */
static void WatchZones(void)
{
    static uint64_t dumpedTable, cursor;
    static ULONGLONG changed;
    if (g_zoneDiag[0] != 0 && g_zoneDiag[0] != dumpedTable)
    {
        if (changed == 0 || g_zoneDiag[4] != cursor) { cursor = g_zoneDiag[4]; changed = GetTickCount64(); }
        else if (GetTickCount64() - changed >= 300) { dumpedTable = g_zoneDiag[0]; changed = 0; ZoneDump(); }
    }
}

/* Entries the hash insert's guard skipped in full tables (destructibles past the destruction-record map), at most one
 * line in 5 s. */
static void WatchHashFull(void)
{
    static LONG reported;
    static ULONGLONG fullLogged;
    LONG full = g_hashFullCount != NULL ? *g_hashFullCount : 0;
    if (full != reported && GetTickCount64() - fullLogged >= 5000)
    {
        reported = full;
        fullLogged = GetTickCount64();
        Note("difficulty: a full hash table skipped %ld entries so far (a map's destructibles past the 32768 the world's destruction-record map holds; the game used to end there)", full);
    }
}

/* The mixed missions so far, at most every 5 s. */
static void WatchMixes(void)
{
    static ULONGLONG mixLogged;
    static LONG seconds, misses;
    if ((g_secondPrimaries != seconds || g_secondPrimaryMisses != misses) && GetTickCount64() - mixLogged >= 5000)
    {
        mixLogged = GetTickCount64();
        seconds = g_secondPrimaries; misses = g_secondPrimaryMisses;
        Note("difficulty: missions mixed with a partner %ld times (%ld partner steps; %ld selections found no partner)", seconds, (long)g_mixSteps, misses);
    }
}

/* The outposts (GuardForce records, [game+0x33265C0]: count +0xC, records +0x48 of 0x2B8, active +0x2AB, size +0x2A8
 * 1..4 = small, medium, large, stronghold; as the mission-end telemetry counts them, 0x62F620), every 3 s. */
static void WatchOutposts(void)
{
    static uint32_t outposts[5];
    static ULONGLONG outpostsChecked;
    uint64_t forces = 0, records = 0;
    uint32_t count = 0, sizes[5] = {0};
    if (!g_difficultyGame || GetTickCount64() - outpostsChecked < 3000) return;
    outpostsChecked = GetTickCount64();
    if (SafeRead(g_difficultyGame + 0x33265C0, &forces, 8) || !forces || SafeRead(forces + 0xC, &count, 4) || count > 4096 ||
        SafeRead(forces + 0x48, &records, 8) || !records) return;
    for (uint32_t i = 0; i < count; i++)
    {
        uint8_t state[4];
        if (SafeRead(records + 0x2B8 * (uint64_t)i + 0x2A8, state, 4)) break;
        if (state[3] && state[0] >= 1 && state[0] <= 4) sizes[state[0]]++;
    }
    sizes[0] = count;
    if (memcmp(sizes, outposts, sizeof sizes) != 0 && (sizes[1] | sizes[2] | sizes[3] | sizes[4]))
        Note("difficulty: outposts in the mission: %u small, %u medium, %u large, %u strongholds (%u records)",
            sizes[1], sizes[2], sizes[3], sizes[4], count);
    memcpy(outposts, sizes, sizeof sizes);
}

static void WatchFootprints(void)
{
    static LONG footprintsMost, footprintOverflows;
    if (g_cityFootprints[0] != footprintsMost || g_cityFootprints[1] != footprintOverflows)
    {
        footprintsMost = g_cityFootprints[0]; footprintOverflows = g_cityFootprints[1];
        Note("difficulty: city buildings: the footprint list used up to %ld places; %ld cells overflowed it (last: %ld wanted of %ld), losing the later cells' buildings",
            footprintsMost, footprintOverflows, g_cityFootprints[3], g_cityFootprints[2]);
    }
}

static void WatchNavGrid(void)
{
    static LONG navLarger, navVanilla;
    if (g_navLarger != navLarger || g_navVanilla != navVanilla)
    {
        navLarger = g_navLarger; navVanilla = g_navVanilla;
        Note("difficulty: navigation grid retiled: %ld times to 256 m tiles (larger maps), %ld to 128 m", navLarger, navVanilla);
    }
}

/* A difficulty-11 level's map fields at `age` ms after its init: 1 when noted. */
static int NoteLevelFields(uintptr_t level, ULONGLONG age)
{
    uint8_t difficulty = 0, mapType = 0xFF, minimap = 0;
    float f[5] = {0};
    uintptr_t type = 0;
    if (SafeRead(level + 0xF9, &difficulty, 1) || difficulty != DIF_VANILLA + 1 || SafeRead(level + 0x2D70, f, sizeof f)) return 0;
    if (!SafeRead(level + 0x2DE0, &type, 8) && type != 0) SafeRead(type + 0x1D, &mapType, 1);
    SafeRead(level + 0xF950, &minimap, 1);
    float half = 1024.0f * (float)WorldScaleFor(g_difficulty.mapScale); /* the radii are fractions of half the world */
    Note("difficulty: level 0x%llx at %.1f s: map type %u%s, playable %.0f m (factor %.2f), border %.0f m, fade %.0f m, disk %.0f m",
        (unsigned long long)level, (double)age / 1000.0, mapType, minimap ? " (minimap)" : "", (double)(f[0] * half),
        (double)f[4], (double)(f[1] * half), (double)(f[2] * half), (double)(f[3] * half));
    return 1;
}

/* A difficulty-11 level's map fields, 1.5 s and 6 s after its init (a preview's generator does not live long), and the
 * watchpoints on them: 1 when noted. */
static int WatchLevel(void)
{
    static uintptr_t level;
    static ULONGLONG levelSeen;
    static int levelLogs;
    uintptr_t latest = *(volatile uintptr_t*)(g_restartWatch + 0x20);
    if (latest != 0 && latest != level) { level = latest; levelSeen = GetTickCount64(); levelLogs = 0; FieldWatch(latest + 0x2D70); AuditPatchSitesNow(); }
    if (level != 0) FieldWatch(level + 0x2D70); /* again once the preview record is known (DR2) */
    ULONGLONG age = GetTickCount64() - levelSeen;
    if (level == 0 || levelLogs >= 2 || age < (levelLogs == 0 ? 1500u : 6000u)) return 0;
    levelLogs++;
    return NoteLevelFields(level, age);
}

static int WatchCapture(void)
{
    static uint32_t captures;
    uint32_t captured = *(volatile uint32_t*)(g_restartWatch + 0x34);
    if (captured == captures) return 0;
    captures = captured;
    float playable = *(volatile float*)(g_restartWatch + 0x28), border = *(volatile float*)(g_restartWatch + 0x2C), factor = *(volatile float*)(g_restartWatch + 0x30);
    Note("difficulty: minimap captured (%u) at difficulty %u: playable %.0f m (factor %.2f), border %.0f m", captured,
        *(volatile uint32_t*)(g_restartWatch + 0x38), (double)(playable * 1024.0f * (float)WorldScaleFor(g_difficulty.mapScale)), (double)factor,
        (double)(border * 1024.0f * (float)WorldScaleFor(g_difficulty.mapScale)));
    return 1;
}

/* A launched level-11 mission's physics bodies, render objects and map arrays. */
static void NoteMissionMap(void)
{
    uint64_t world = 0, level = 0, block = 0, mapType = 0;
    uint32_t counts[8] = {0};
    uint8_t type = 0xFF;
    if (!g_difficultyGame || SafeRead(g_difficultyGame + DIF_WORLD, &world, 8) || !world || SafeRead(world + 0xAE288, &level, 8) || !level ||
        SafeRead(level + 0x2C8, &block, 8) || !block) return;
    SafeRead(level + 0x2DE0, &mapType, 8);
    if (mapType) SafeRead(mapType + 0x1D, &type, 1);
    SafeRead(block + 0x11A715C, &counts[0], 4); SafeRead(block + 0x11A725C, &counts[1], 4); SafeRead(block + 0x11A7260, &counts[2], 4);
    SafeRead(block + 0x11A7264, &counts[3], 4); SafeRead(block + 0x11A71B4, &counts[4], 4); SafeRead(block + 0x11A71C0, &counts[5], 4);
    SafeRead(block + 0x11A7108, &counts[6], 4); SafeRead(block + 0x11A71CC, &counts[7], 4);
    uintptr_t exe = ApiModuleBase("helldivers2.exe", NULL);
    uint64_t physics = 0;
    uint32_t bodies[3] = {0};
    if (exe && !SafeRead(exe + 0x27BA890 + 0xB0 * 2, &physics, 8) && physics && !SafeRead(physics + 0x4C, &bodies[0], 4) &&
        !SafeRead(physics + 0x50, &bodies[1], 4) && !SafeRead(physics + 0x40, &bodies[2], 4))
        Note("difficulty: the mission's physics bodies: %u in use (highest index %u) of %u", bodies[0], bodies[1], bodies[2] & 0x3FFFFFFFu);
    uint64_t render = 0, store = 0;
    uint32_t objects[2] = {0};
    if (!SafeRead(world + 0x10E8, &render, 8) && render && !SafeRead(render + 0x2301F8, &store, 8) && store && !SafeRead(store + 0x38, objects, 8))
        Note("difficulty: the mission's render objects: %u in use of %u", objects[0], objects[1]);
    Note("difficulty: the mission's map (type %u): important locations %u of %u (cities flag 1024), city walls %u / %u / %u of 64 / 128 / 128, vertices %u of 975, route cells %u of 1024, edges %u of 1621, locations %u of %u",
        type, counts[0], g_importantList == 0x8996B0u ? 256u : 1024u, counts[1], counts[2], counts[3], counts[4], counts[5], counts[7], counts[6], DIF_LOCATIONS);
    /* Where the important locations are: xy at +0xA4, generator metres from the map's corner (0..L, L = 2048 x scale;
     * difficulty_world_macro.c reads the same), as an 8 x 8 grid of counts, north row first. */
    const float L = 2048.0f * (float)(g_world.scale > 1 ? g_world.scale : 1);
    uint32_t grid[8][8] = {{0}}, placed = 0;
    double distance = 0;
    for (uint32_t l = 0; l < counts[0] && l < 1024; l++)
    {
        float xy[2];
        if (SafeRead(block + g_importantList + 0x108ull * l + 0xA4, xy, 8) || !isfinite(xy[0]) || !isfinite(xy[1])) continue;
        int i = (int)(xy[0] / L * 8.0f), j = (int)(xy[1] / L * 8.0f);
        if (i < 0 || i > 7 || j < 0 || j > 7) continue;
        grid[j][i]++; placed++;
        double dx = xy[0] - L * 0.5, dy = xy[1] - L * 0.5;
        distance += sqrt(dx * dx + dy * dy);
    }
    char rows[8 * 8 * 5 + 16];
    int n = 0;
    for (int j = 7; j >= 0; j--)
    {
        for (int i = 0; i < 8; i++) n += snprintf(rows + n, sizeof rows - n, "%s%u", i ? "," : (j < 7 ? " | " : ""), grid[j][i]);
    }
    Note("difficulty: the mission's important locations on the map (%u placed, mean %.0f m from the centre; 8 x 8 cells of %.0f m, north row first): %s",
        placed, placed ? distance / placed : 0.0, (double)(L / 8.0f), rows);
}

static int WatchLaunch(void)
{
    static uint32_t timed;
    uint32_t launched = *(volatile uint32_t*)(g_restartWatch + 0x44);
    if (launched == timed) return 0;
    timed = launched;
    Note("difficulty: a level-11 mission launched with the %.0f-minute countdown delta (%u so far)", (double)g_difficulty.minutes, launched);
    NoteMissionMap();
    AuditPatchSitesNow(); /* the next level the mission loads is the hellpod's */
    return 1;
}

static int WatchSpawners(void)
{
    static uint32_t dropped;
    uint32_t spawnersLeft = *(volatile uint32_t*)(g_restartWatch + 0x3C);
    if (spawnersLeft == dropped) return 0;
    dropped = spawnersLeft;
    Note("difficulty: the enemy director's 1024 spawners are full: %u more left out so far (no AI spawner and no garrison for them)", spawnersLeft);
    return 1;
}

static int WatchPockets(void)
{
    static uint32_t pockets;
    uint32_t left = *(volatile uint32_t*)(g_restartWatch + 0x18);
    if (left == pockets) return 0;
    pockets = left;
    Note("difficulty: FixUnreachableAreas could not open a pocket of the map after %d tries: the level keeps it (%u so far)", DIF_UNREACHABLE_TRIES, left);
    return 1;
}

/* Restart site i's latest restart: which pass failed, at which difficulty, how many times for that level. */
static void NoteRestart(int i)
{
    uintptr_t lg = *(volatile uintptr_t*)g_restartWatch, namePtr = 0;
    uint32_t restarts = 0;
    uint8_t difficulty = 0, minimap = 0;
    char name[64] = "?";
    SafeRead(lg + 0xF954, &restarts, 4);
    SafeRead(lg + 0xF9, &difficulty, 1);
    SafeRead(lg + 0xF950, &minimap, 1);
    if (!SafeRead(lg + 0xF958, &namePtr, 8) && namePtr != 0 && !SafeRead(namePtr, name, sizeof name - 1)) name[sizeof name - 1] = 0;
    for (char* c = name; *c; c++) if (*c < 32 || *c > 126) { *c = 0; break; }
    Note("difficulty: level generation restart (%s failed; %u for this level, first failed pass %s) at difficulty %u%s, level 0x%llx",
        kRestartPasses[i], restarts, name, difficulty, minimap ? " (minimap)" : "", (unsigned long long)lg);
}

static int WatchRestarts(void)
{
    static uint32_t seen[4];
    int logged = 0;
    for (int i = 0; i < 4; i++)
    {
        uint32_t count = *(volatile uint32_t*)(g_restartWatch + 8 + 4 * i);
        if (count == seen[i]) continue;
        seen[i] = count;
        NoteRestart(i);
        logged++;
    }
    return logged;
}

/* Polled by the loader's thread: logs each level generation restart (which pass failed, at which difficulty, how many
 * times for that level). The first 40. */
static void DifficultyWatch(void)
{
    static int logged;
    WatchZones();
    WatchHashFull();
    if (g_restartWatch == NULL) return;
    WatchMixes();
    WatchOutposts();
    WatchFootprints();
    WatchNavGrid();
    if (logged >= 40) return;
    logged += WatchLevel();
    logged += WatchCapture();
    logged += WatchLaunch();
    logged += WatchSpawners();
    logged += WatchPockets();
    logged += WatchRestarts();
}
