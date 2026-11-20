/* The longer customize list (customize_list.c) on a synthetic game image: every site's pin, the field sites moved by
 * CL_SHIFT and the UI bounds raised to CL_CAP, all put back when a write fails; then a build on a frame as long as a
 * weapon's list can get, its fields untouched, the shown count clamped at CL_CAP and S's own copy at 64.
 * Included by stats_test.c. */

#define CLT_GAME 0x1A30000u /* past the last UI bound */

static void CltImage(uint8_t* image)
{
    for (size_t i = 0; i < sizeof kClFields / sizeof *kClFields; i++) memcpy(image + kClFields[i].rva, kClFields[i].pin, kClFields[i].len);
    static const uint8_t bound[3] = {0x83, 0xF8, 0x40};
    for (size_t i = 0; i < sizeof kClBounds / sizeof *kClBounds; i++) memcpy(image + kClBounds[i], bound, 3);
    memcpy(image + CL_BUILD, kClBuildPin, sizeof kClBuildPin);
    memcpy(image + CL_CLOSE, kClClosePin, sizeof kClClosePin);
    memcpy(image + CL_HANDOFF, kClHandoffPin, sizeof kClHandoffPin);
    memcpy(image + CL_SET_PICK, kClSetPickPin, sizeof kClSetPickPin);
    memcpy(image + CL_MARK, kClMarkPin, sizeof kClMarkPin);
}

/* Field site i as moved: its disp32 CL_SHIFT on, every other byte the pin's. */
static int CltFieldMoved(const uint8_t* image, size_t i)
{
    const ClField* f = &kClFields[i];
    uint32_t disp, pinned;
    memcpy(&disp, image + f->rva + f->at, 4);
    memcpy(&pinned, f->pin + f->at, 4);
    return disp == pinned + CL_SHIFT && memcmp(image + f->rva, f->pin, f->at) == 0 &&
        memcmp(image + f->rva + f->at + 4, f->pin + f->at + 4, f->len - f->at - 4u) == 0;
}

/* The game's build as the frame sees it: appends count entries with no bound, its fields at their moved place. */
static uint32_t g_cltCards;
static uint8_t g_cltItems[CL_ROOM][16];
static void CltBuild(uint8_t* frame, const uint32_t* weapon)
{
    *(uint32_t*)(frame + CL_WEAPON) = *weapon;
    for (uint32_t i = 0; i < g_cltCards; i++)
    {
        uint8_t* entry = ClEntry(frame, i);
        memset(entry, 0, CL_ENTRY);
        *(uint8_t**)entry = g_cltItems[i];
    }
    *(uint32_t*)(frame + CL_COUNT) = g_cltCards;
    *(uint32_t*)(frame + CL_FIELDS + CL_SHIFT) = 0xF1E1D5u;
}

static void CustomizeListFixture(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, CLT_GAME, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(image != NULL);
    uintptr_t game = (uintptr_t)image;
    const size_t fields = sizeof kClFields / sizeof *kClFields, bounds = sizeof kClBounds / sizeof *kClBounds;
    CltImage(image);
    assert(ClPins(game));
    /* any byte off: refused */
    image[kClFields[40].rva] ^= 1;
    assert(!ClPins(game));
    image[kClFields[40].rva] ^= 1;
    image[kClBounds[3] + 2] = 0x41;
    assert(!ClPins(game));
    image[kClBounds[3] + 2] = 0x40;
    assert(ClPins(game));

    /* every site moved, then all put back */
    assert(ClWrite(game) == 0);
    for (size_t i = 0; i < fields; i++) assert(CltFieldMoved(image, i));
    for (size_t i = 0; i < bounds; i++) assert(image[kClBounds[i]] == 0x83 && image[kClBounds[i] + 1] == 0xF8 && image[kClBounds[i] + 2] == CL_CAP);
    for (size_t i = CL_SITES; i-- > 0;) assert(ClSite(game, i, 0) == 0);
    assert(ClPins(game));

    /* the last bounds' page cannot be written: what was written goes back, the rest was never touched */
    uint8_t* last = image + (kClBounds[bounds - 1] & ~0xFFFu);
    assert(last == image + (kClBounds[bounds - 2] & ~0xFFFu) && last != image + (kClBounds[bounds - 3] & ~0xFFFu));
    assert(VirtualFree(last, 0x1000, MEM_DECOMMIT));
    assert(ClWrite(game) == 1);
    for (size_t i = 0; i < fields; i++) assert(PinMatches(game + kClFields[i].rva, kClFields[i].pin, kClFields[i].len));
    for (size_t i = 0; i < bounds - 2; i++) assert(image[kClBounds[i] + 2] == CL_GAME_ENTRIES);
    assert(VirtualAlloc(last, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE) == last);
    CltImage(image);
    assert(ClPins(game));

    /* the five hooks on, then off again */
    assert(ClHooks(game) == 0);
    ClUnhook(game, CL_HOOKS);
    for (size_t i = 0; i < CL_HOOKS; i++) assert(HookRemove(NULL, game + kClHooks[i].rva, kClHooks[i].detour) == 1);

    /* a build of every card a weapon can have: the frame holds it, its fields stay; the UI walks CL_CAP, S keeps 64 */
    uint8_t* screen = (uint8_t*)VirtualAlloc(NULL, CL_FRAME_BYTES, MEM_RESERVE, PAGE_NOACCESS);
    assert(screen != NULL && VirtualAlloc(screen, 0x1000, MEM_COMMIT, PAGE_READWRITE) == screen);
    assert(VirtualAlloc(screen + (CL_BLOCK & ~0xFFFu), 0x1000, MEM_COMMIT, PAGE_READWRITE) != NULL);
    for (uint32_t i = 0; i < CL_ROOM; i++) *(uint32_t*)(g_cltItems[i] + 8) = 1000u + i;
    *(uint32_t*)(screen + CL_FIELDS + 0x10u) = 0xA11CEu; /* a field the build leaves alone */
    *(uint64_t*)(screen + CL_BLOCK) = 0xB10Cu;
    assert((g_clFrames[0].frame = ClAllocateFrame()) != NULL && (g_clFrames[1].frame = ClAllocateFrame()) != NULL);
    g_clBuild = CltBuild;
    g_clOverNoted = 0;
    const uint32_t weapon = 77;
    for (int pass = 0; pass < 2; pass++)
    {
        g_cltCards = pass == 0 ? 40u : CL_ROOM;
        ClBuildDetour(screen, &weapon);
        uint8_t* frame = ClFrame(screen, 0);
        assert(frame != NULL && *(uint64_t*)(frame + CL_BLOCK) == 0xB10Cu);
        uint32_t shown = g_cltCards < CL_CAP ? g_cltCards : CL_CAP;
        assert(*(uint32_t*)(frame + CL_COUNT) == shown);
        assert(*(uint32_t*)(frame + CL_FIELDS + CL_SHIFT) == 0xF1E1D5u && *(uint32_t*)(frame + CL_FIELDS + CL_SHIFT + 0x10u) == 0xA11CEu);
        assert(ClItemId(ClEntry(frame, g_cltCards - 1)) == 1000u + g_cltCards - 1);
        assert(*(uint32_t*)(screen + CL_COUNT) == (shown < CL_GAME_ENTRIES ? shown : CL_GAME_ENTRIES));
        assert(memcmp(screen + CL_ENTRIES, frame + CL_ENTRIES, CL_ENTRY * CL_GAME_ENTRIES) == 0);
        assert(*(uint32_t*)(screen + CL_FIELDS) == 0xF1E1D5u && *(uint32_t*)(screen + CL_FIELDS + 0x10u) == 0xA11CEu);
        assert(g_clOverNoted == (pass == 1));
    }
    for (int i = 0; i < CL_FRAMES; i++) g_clFrames[i].screen = NULL;
    g_clBuild = NULL;
    for (int i = 0; i < 2; i++) { VirtualFree(g_clFrames[i].frame, 0, MEM_RELEASE); g_clFrames[i].frame = NULL; }
    VirtualFree(screen, 0, MEM_RELEASE);
    VirtualFree(image, 0, MEM_RELEASE);
}
