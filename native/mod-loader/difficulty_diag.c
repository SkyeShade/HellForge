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
    float diskBits = d->disk / 1024.0f;
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

static const char* JumpToRestartStubs(DifficultyInstall* d)
{
    for (int i = 0; i < 4; i++)
    {
        uint8_t jump[6] = {0xE9, 0, 0, 0, 0, 0x90};
        int32_t rel = (int32_t)((uintptr_t)(i == 0 && d->unreachableStub != NULL ? d->unreachableStub : d->restartStubs + 0x20 * i) - (d->game + kRestartSites[i] + 5));
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
    int32_t rel = (int32_t)((uintptr_t)d->captureStub - (d->game + DIF_CAPTURE_SITE + 5));
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
    Note("difficulty: zones-%d.tsv written (%u route cells, %d zone groups, playable %.0f m)", dumps, cells, groups, radius * 1024.0);
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
    Note("difficulty: level 0x%llx at %.1f s: map type %u%s, playable %.0f m (factor %.2f), border %.0f m, fade %.0f m, disk %.0f m",
        (unsigned long long)level, (double)age / 1000.0, mapType, minimap ? " (minimap)" : "", (double)(f[0] * 1024.0f),
        (double)f[4], (double)(f[1] * 1024.0f), (double)(f[2] * 1024.0f), (double)(f[3] * 1024.0f));
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
    if (latest != 0 && latest != level) { level = latest; levelSeen = GetTickCount64(); levelLogs = 0; FieldWatch(latest + 0x2D70); }
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
        *(volatile uint32_t*)(g_restartWatch + 0x38), (double)(playable * 1024.0f), (double)factor, (double)(border * 1024.0f));
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
    Note("difficulty: the mission's map (type %u): important locations %u of 256 (cities flag 1024), city walls %u / %u / %u of 64 / 128 / 128, vertices %u of 975, route cells %u of 1024, edges %u of 1621, locations %u of %u",
        type, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[7], counts[6], DIF_LOCATIONS);
}

static int WatchLaunch(void)
{
    static uint32_t timed;
    uint32_t launched = *(volatile uint32_t*)(g_restartWatch + 0x44);
    if (launched == timed) return 0;
    timed = launched;
    Note("difficulty: a level-11 mission launched with the %.0f-minute countdown delta (%u so far)", (double)g_difficulty.minutes, launched);
    NoteMissionMap();
    return 1;
}

static int WatchSpawners(void)
{
    static uint32_t dropped;
    uint32_t spawnersLeft = *(volatile uint32_t*)(g_restartWatch + 0x3C);
    if (spawnersLeft == dropped) return 0;
    dropped = spawnersLeft;
    Note("difficulty: the enemy director's 128 spawners are full: %u more left out so far (their outposts stay)", spawnersLeft);
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
