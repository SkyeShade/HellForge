/* The game's mouse pointer for the overlay host (overlay.c, OverlayGameCursorIn) on a synthetic exe image: the
 * reviewed code at its three places, the engine global, its window list and first window. Each check the reader makes
 * is broken in turn and must give NULL, the engine's own fields read where they are. Included by stats_test.c. */

#define OC_IMAGE 0x1A11000u

static void OcPut(uint8_t* image, uint32_t rva, const char* pattern)
{
    uint8_t bytes[256], mask[256];
    size_t count = ParsePattern(pattern, bytes, mask);
    assert(count > 0);
    memcpy(image + rva, bytes, count);
}

static void OverlayCursorFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, OC_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(image != NULL);
    uintptr_t exe = (uintptr_t)image;
    OcPut(image, 0x328508u, kOverlayEngineCode);
    OcPut(image, 0x3285E4u, kOverlaySetCode);
    OcPut(image, 0x616815u, kOverlayShowCode);
    static uint8_t engine[0x400], window[0x110];
    static uintptr_t windows[2];
    void* const hwnd = (void*)(uintptr_t)0x5A5A0;
    void* const cursor = (void*)(uintptr_t)0xC0C0;
    const uintptr_t arrow = (uintptr_t)LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    assert(arrow != 0);
    *(uintptr_t*)(image + OVERLAY_ENGINE) = (uintptr_t)engine;
    *(uint32_t*)(engine + 0x3B8) = 1;
    *(uintptr_t*)(engine + 0x3C0) = (uintptr_t)windows;
    windows[0] = (uintptr_t)window;
    *(uintptr_t*)(window + 0x50) = (uintptr_t)hwnd;
    *(uintptr_t*)(window + 0x68) = (uintptr_t)cursor;
    *(uintptr_t*)(window + 0x70) = arrow;

    assert(OverlayGameCursorIn(exe, hwnd) == cursor);
    assert(OverlayGameCursorIn(exe, NULL) == cursor); /* the host does not know its window yet */
    assert(OverlayGameCursorIn(exe, (void*)(uintptr_t)0x5A5A8) == NULL); /* another window's engine */

    /* each piece of reviewed code changed: another build, so no pointer */
    const uint32_t code[] = {0x328508u + 3, 0x328508u + 23, 0x328508u + 30, 0x3285E4u + 3, 0x3285E4u + 7, 0x616815u + 9, 0x616815u + 18, 0x616815u + 31};
    for (size_t i = 0; i < sizeof code / sizeof code[0]; i++)
    {
        image[code[i]] ^= 0x01;
        assert(OverlayGameCursorIn(exe, hwnd) == NULL);
        image[code[i]] ^= 0x01;
    }
    image[0x616815u + 25] ^= 0xFF; /* the call's own address is not pinned */
    assert(OverlayGameCursorIn(exe, hwnd) == cursor);
    image[0x616815u + 25] ^= 0xFF;

    *(uintptr_t*)(window + 0x70) = arrow + 8; /* not the engine's window layout */
    assert(OverlayGameCursorIn(exe, hwnd) == NULL);
    *(uintptr_t*)(window + 0x70) = arrow;
    *(uint32_t*)(engine + 0x3B8) = 0; /* no window yet */
    assert(OverlayGameCursorIn(exe, hwnd) == NULL);
    *(uint32_t*)(engine + 0x3B8) = 1;
    windows[0] = 0;
    assert(OverlayGameCursorIn(exe, hwnd) == NULL);
    windows[0] = (uintptr_t)window;
    *(uintptr_t*)(window + 0x68) = 0; /* the game set no pointer of its own */
    assert(OverlayGameCursorIn(exe, hwnd) == NULL);
    *(uintptr_t*)(window + 0x68) = (uintptr_t)cursor;
    *(uintptr_t*)(image + OVERLAY_ENGINE) = 0x10; /* unreadable: no fault */
    assert(OverlayGameCursorIn(exe, hwnd) == NULL);
    *(uintptr_t*)(image + OVERLAY_ENGINE) = 0;
    assert(OverlayGameCursorIn(exe, hwnd) == NULL);
    *(uintptr_t*)(image + OVERLAY_ENGINE) = (uintptr_t)engine;
    assert(OverlayGameCursorIn(exe, hwnd) == cursor);
    VirtualFree(image, 0, MEM_RELEASE);
}
