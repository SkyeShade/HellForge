/* The scatter cameras fix (perf_scatter_cameras.c) on synthetic exe and game.dll images: the pins, the refusals that
 * add no hook, then both hooks installed through the loader's chain. Each hooked function is its pinned prologue and
 * a short tail, so every call runs the chain, the detour and the game's moved bytes. Included by stats_test.c. */

#define SC_EXE_IMAGE 0x4D2000u
#define SC_GAME_IMAGE 0xC15000u

static void ScPut(uint8_t* image, uint32_t rva, const uint8_t* bytes, size_t size) { memcpy(image + rva, bytes, size); }

/* 1 when one changed byte at image + rva makes the pins refuse; the byte is put back. */
static int ScRefuses(uint8_t* image, uint32_t rva, uintptr_t exe, uintptr_t game)
{
    image[rva] ^= 0x10;
    int refused = !ScPinned(exe, game);
    image[rva] ^= 0x10;
    return refused;
}

/* A render camera as the writers at SC_CAMERA leave it. */
static void ScView(uint8_t* view, float nearZ, float farZ, uint32_t projection, float half)
{
    memset(view, 0, 0x280);
    float box[4] = {-half, half, -half, half};
    memcpy(view + 0x20, &nearZ, 4); memcpy(view + 0x24, &farZ, 4); memcpy(view + 0x28, &projection, 4);
    memcpy(view + 0x30, box, sizeof box);
}

static void PerfScatterCamerasFixture(void)
{
    uint8_t* exeImage = (uint8_t*)VirtualAlloc(NULL, SC_EXE_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    uint8_t* gameImage = (uint8_t*)VirtualAlloc(NULL, SC_GAME_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(exeImage != NULL && gameImage != NULL);
    uintptr_t exe = (uintptr_t)exeImage, game = (uintptr_t)gameImage;
    /* the update: its prologue, the lea's last byte, mov byte [rcx], 1; the pops; mov rbx, [rsp+0x18]; ret */
    static const uint8_t updateTail[] = {0xff, 0xc6,0x01,0x01, 0x41,0x5e, 0x41,0x5d, 0x5f, 0x5e, 0x5d, 0x48,0x8b,0x5c,0x24,0x18, 0xc3};
    /* the dispatcher: its prologue, mov eax, 0x77; the pops; ret */
    static const uint8_t dispatchTail[] = {0xb8,0x77,0x00,0x00,0x00, 0x41,0x5f, 0x41,0x5e, 0x41,0x5c, 0x5f, 0x5e, 0x5b, 0x5d, 0xc3};
    ScPut(exeImage, SC_UPDATE, kScUpdatePin, sizeof kScUpdatePin);
    ScPut(exeImage, SC_UPDATE + sizeof kScUpdatePin, updateTail, sizeof updateTail);
    ScPut(exeImage, SC_CALLER, kScCallerPin, sizeof kScCallerPin);
    ScPut(exeImage, SC_CAMERA, kScCameraPin, sizeof kScCameraPin);
    ScPut(gameImage, SC_DISPATCH, kScDispatchPin, sizeof kScDispatchPin);
    ScPut(gameImage, SC_DISPATCH + sizeof kScDispatchPin, dispatchTail, sizeof dispatchTail);
    ScPut(gameImage, SC_DECODE, kScDecodePin, sizeof kScDecodePin);
    FlushInstructionCache(GetCurrentProcess(), exeImage, SC_EXE_IMAGE);
    FlushInstructionCache(GetCurrentProcess(), gameImage, SC_GAME_IMAGE);
    assert(ScPinned(exe, game));
    const uint32_t exeChanged[] = {SC_UPDATE, SC_UPDATE + 18, SC_CALLER + 1, SC_CALLER + 11, SC_CAMERA + 8, SC_CAMERA + 36};
    for (size_t i = 0; i < sizeof exeChanged / sizeof exeChanged[0]; i++) assert(ScRefuses(exeImage, exeChanged[i], exe, game));
    const uint32_t gameChanged[] = {SC_DISPATCH, SC_DISPATCH + 18, SC_DECODE + 2, SC_DECODE + 54, SC_DECODE + 94, SC_DECODE + sizeof kScDecodePin - 1};
    for (size_t i = 0; i < sizeof gameChanged / sizeof gameChanged[0]; i++) assert(ScRefuses(gameImage, gameChanged[i], exe, game));

    /* the views: only the world-sized near-1/far-1001 orthographic one, and only in the water and fog ops */
    _Alignas(16) uint8_t view[0x280];
    ScView(view, 1.0f, 1001.0f, 0, 1024.0f);
    const LONG cut[] = {0x0A, 0x0B, 0x43, 0x44, 0x45}, kept[] = {-1, 0x22, 0x46, 0x47, 0x49, 0x4A, 0x4B, 0x4F, 0x38, 0xB5};
    for (size_t i = 0; i < sizeof cut / sizeof cut[0]; i++) assert(ScCutView(cut[i], view));
    for (size_t i = 0; i < sizeof kept / sizeof kept[0]; i++) assert(!ScCutView(kept[i], view));
    ScView(view, 1.0f, 1001.0f, 0, 128.0f); assert(!ScCutView(0x0A, view)); /* a far-scatter bake tile */
    ScView(view, 1.0f, 1001.0f, 0, 640.0f); assert(!ScCutView(0x0B, view)); /* the terrain deformation camera */
    ScView(view, 1.0f, 1001.0f, 1, 1024.0f); assert(!ScCutView(0x0A, view)); /* perspective */
    ScView(view, 0.05f, 1001.0f, 0, 1024.0f); assert(!ScCutView(0x44, view));
    ScView(view, 1.0f, 1002.0f, 0, 1024.0f); assert(!ScCutView(0x44, view));
    ScView(view, 1.0f, 1001.0f, 0, NAN); assert(!ScCutView(0x44, view));
    ScView(view, 1.0f, 1001.0f, 0, 8192.0f); assert(ScCutView(0x44, view)); /* a larger world's fog camera */
    ScView(view, 1.0f, 1001.0f, 0, 1024.0f);
    memset(view + 0x38, 0, 8); assert(!ScCutView(0x0A, view)); /* wide but flat */
    assert(!ScCutView(0x0A, NULL));

    char build[sizeof g_build];
    memcpy(build, g_build, sizeof build);
    strcpy(g_build, "25480438");

    /* code not decoded yet: it waits a minute, then gives up with no hook */
    exeImage[SC_CALLER] ^= 0x10;
    g_scState = 0;
    ScWatchAt(exe, game);
    assert(g_scState == 2 && g_scLoadedAt != 0 && g_scUpdateNext == NULL && g_scDispatchNext == NULL);
    g_scLoadedAt = GetTickCount64() - 60001;
    ScWatchAt(exe, game);
    assert(g_scState == -1 && g_scUpdateNext == NULL && g_scDispatchNext == NULL && exeImage[SC_UPDATE] == 0x48);
    exeImage[SC_CALLER] ^= 0x10;
    g_scLoadedAt = 0;

    /* turned off, and another build: no hook */
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", "safe-read, Scatter-Cameras");
    g_scState = 0;
    ScWatchAt(exe, game);
    assert(g_scState == -1 && g_scUpdateNext == NULL && g_scDispatchNext == NULL);
    SetEnvironmentVariableA("HELLFORGE_PERF_OFF", NULL);
    strcpy(g_build, "99999999");
    g_scState = 0;
    ScWatchAt(exe, game);
    assert(g_scState == -1 && g_scUpdateNext == NULL);
    strcpy(g_build, "25480438");

    /* installed: both functions patched, their pins still read through the hooks, once */
    g_scState = 0;
    ScWatchAt(exe, game);
    assert(g_scState == 1 && g_scUpdateNext != NULL && g_scDispatchNext != NULL);
    assert(exeImage[SC_UPDATE] != 0x48 && gameImage[SC_DISPATCH] != 0x40 && ScPinned(exe, game));
    ScWatchAt(exe, game);
    assert(g_scState == 1);

    /* the dispatcher: a record that runs moves the op; one past the end or masked out does not */
    static uint8_t records[3 * SC_RECORD];
    uint32_t op = 0x0B, flags = 4, index = 1;
    memcpy(records + SC_RECORD, &op, 4);
    memcpy(records + SC_RECORD + 0x1998, &flags, 4);
    op = 0x44; memcpy(records + 2 * SC_RECORD, &op, 4);
    ScDispatchFn dispatch = (ScDispatchFn)(void*)(gameImage + SC_DISPATCH);
    g_scOp = -1;
    assert(dispatch((uintptr_t)records, 0, (uintptr_t)&index, 3, 0, 0) == 0x77 && g_scOp == 0x0B);
    index = 2;
    assert(dispatch((uintptr_t)records, 0, (uintptr_t)&index, 2, 0, 0) == 0x77 && g_scOp == 0x0B); /* past the end */
    index = 1;
    g_scOp = 0x22;
    assert(dispatch((uintptr_t)records, 0, (uintptr_t)&index, 3, 0, 0xFFFFFF04u) == 0x77 && g_scOp == 0x22); /* masked */
    assert(dispatch((uintptr_t)records, 0, (uintptr_t)&index, 3, 0, 0x2) == 0x77 && g_scOp == 0x0B);
    index = 2;
    assert(dispatch((uintptr_t)records, 0, (uintptr_t)&index, 3, 0, 0) == 0x77 && g_scOp == 0x44);

    /* the update: the fog view in op 0x44 never reaches the game's code; the player's view and a bake tile do */
    ScUpdateFn update = (ScUpdateFn)(void*)(exeImage + SC_UPDATE);
    volatile uint8_t ran = 0;
    LONG64 skipped = g_scSkipped;
    ScView(view, 1.0f, 1001.0f, 0, 1024.0f);
    update((void*)&ran, view, 7, NULL, 0);
    assert(ran == 0 && g_scSkipped == skipped + 1);
    ScView(view, 0.05f, 5000.0f, 1, 0.0f);
    update((void*)&ran, view, 7, NULL, 0);
    assert(ran == 1 && g_scSkipped == skipped + 1);
    ran = 0;
    ScView(view, 1.0f, 1001.0f, 0, 128.0f);
    update((void*)&ran, view, 7, NULL, 0);
    assert(ran == 1);
    ran = 0;
    g_scOp = 0x4F; /* the in-game map's op: even a world-sized view keeps its scatter */
    ScView(view, 1.0f, 1001.0f, 0, 1024.0f);
    update((void*)&ran, view, 7, NULL, 0);
    assert(ran == 1 && g_scSkipped == skipped + 1);
    g_scOp = -1;

    memcpy(g_build, build, sizeof build);
    /* the images stay: the chain still points into them */
    puts("PASS scatter cameras: pins refuse any changed byte, a wait for decoded code that gives up, off by name and on another build with no hook, "
        "the water and fog views cut only in their ops and at the world's width, the dispatcher's op from records that run, both hooks run through the chain");
}
