/* The player's loadout after a live change takes content out (live_content.c, SDK 16; KB
 * hellforge-live-mod-sync-re-loadout). A tombstone keeps its rows, but the loadout may still hold it, and a peer that
 * receives a loadout stores its stratagem types unchecked and reads table[type] at once (game.dll 0x135D398): a host
 * without the type would crash on receipt. So the loader leaves the loadout as a boot without the mod gives it: the type
 * out of every PlayerHistory record and its stable id out of the profile (a boot drops a stable id that maps to no
 * type), a weapon back to the default loadout's of its kind and pushed to the history through the armory's own commit
 * (0x11F5010, what a boot's validation 0x1750870 makes of an unknown weapon). Solo, on the game thread, with the login
 * done and the loadout screen closed (its lineup keeps copies of its own); a change that would need it otherwise is
 * refused before anything changes. Included by live_content.c.
 *
 * game.dll 25480438: PlayerHistory H = [game + 0x347CE50]: records of 0x1690 bytes from H (u64 peer id at +0), their
 * count at H + 0x2D200, a record's loadout at +0x38 (stratagem slots of 0x30 bytes at +0x188, the type at +0; their count
 * at +0x788). The profile P = [game + 0x347CDD8]: weapon loadout ids at +0x40 / +0x44 / +0x48, 32 {stable id, uses} pairs
 * at +0x4C. The local peer: [[game + 0x347CEF0] + 0xB398]. The login: [[game + 0x347CEF8] + 0xDDCF8] == 12. The loadout
 * screen: the menu presenter [[game + 0x347CE28] + 0x4294] == 14, or the menu stack's top [[game + 0x347CE38]] == 11. */

#define LS_HISTORY 0x347CE50u
#define LS_PROFILE 0x347CDD8u
#define LS_PEER 0x347CEF0u
#define LS_MANAGER 0x347CEF8u
#define LS_UI 0x347CE28u
#define LS_MENU 0x347CE38u
#define LS_PUSH 0x11F5010u
#define LS_RECORDS 0x2D200u
#define LS_RECORD 0x1690u
#define LS_LOADOUT 0x38u
#define LS_SLOTS 0x188u
#define LS_SLOT 0x30u
#define LS_SLOT_COUNT 0x788u
#define LS_PAIRS 0x4Cu
#define LS_MAX_RECORDS 32u
#define LS_MAX_SLOTS 32u

typedef struct LoadoutPin { uint32_t rva; uint8_t size; uint8_t bytes[64]; } LoadoutPin;
static const LoadoutPin kLoadoutPins[] = {
    {0x1361F69u, 7, {0x48, 0x8B, 0x35, 0xE0, 0xAE, 0x11, 0x02}}, /* the PlayerHistory system */
    {0x1362043u, 49, {0x39, 0x9E, 0x00, 0xD2, 0x02, 0x00, 0x76, 0x36, 0x0F, 0x1F, 0x44, 0x00, 0x00, 0x8B, 0xC3, 0x48, 0x69, 0xD0, 0x90, 0x16, 0x00,
        0x00, 0x48, 0x03, 0xD6, 0x48, 0x39, 0x2A, 0x75, 0x16, 0x48, 0x8D, 0x4F, 0x08, 0x48, 0x83, 0xC2, 0x38, 0x41, 0xB8, 0xF0, 0x09, 0x00, 0x00, 0xE8,
        0x7C, 0x68, 0xD3, 0x00}}, /* count, stride, peer, loadout and its size */
    {0x11F5010u, 52, {0xB8, 0x18, 0x14, 0x00, 0x00, 0xE8, 0xA6, 0xF5, 0xF0, 0x00, 0x48, 0x2B, 0xE0, 0x4C, 0x8B, 0x0D, 0x2C, 0x7E, 0x28, 0x02, 0x33,
        0xC0, 0x45, 0x8B, 0x81, 0x00, 0xD2, 0x02, 0x00, 0x45, 0x85, 0xC0, 0x0F, 0x84, 0xB9, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x0D, 0xB3, 0x7E, 0x28, 0x02,
        0x4C, 0x8B, 0x91, 0x98, 0xB3, 0x00, 0x00}}, /* the armory commit's push: no arguments, the local peer */
    {0x1751350u, 64, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x01, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x1D, 0x72, 0xBA, 0xD2,
        0x01, 0x33, 0xD2, 0x41, 0xB8, 0x00, 0x01, 0x00, 0x00, 0x89, 0x43, 0x40, 0x8B, 0x41, 0x60, 0x89, 0x43, 0x44, 0x8B, 0x81, 0xC0, 0x00, 0x00, 0x00,
        0x89, 0x43, 0x48, 0x0F, 0x10, 0x81, 0x20, 0x01, 0x00, 0x00, 0x0F, 0x11, 0x83, 0x4C, 0x01, 0x00, 0x00, 0x0F, 0x10}}, /* the profile's weapons */
    {0x17513B2u, 50, {0x48, 0x8D, 0x4B, 0x4C, 0x89, 0x83, 0x74, 0x01, 0x00, 0x00, 0xE8, 0xDF, 0x96, 0x93, 0x00, 0x8B, 0x87, 0x88, 0x07, 0x00, 0x00,
        0x41, 0xB8, 0x20, 0x00, 0x00, 0x00, 0x41, 0x3B, 0xC0, 0x44, 0x0F, 0x42, 0xC0, 0x45, 0x85, 0xC0, 0x74, 0x41, 0x48, 0x8D, 0x53, 0x50, 0x48, 0x8D,
        0x8F, 0x8C, 0x01, 0x00, 0x00}}, /* the profile's pairs, the slots and their count */
    {0x1752408u, 23, {0xC7, 0x03, 0x09, 0x53, 0x03, 0xAA, 0xC7, 0x43, 0x60, 0x98, 0x7F, 0x97, 0xA7, 0xC7, 0x83, 0xC0, 0x00, 0x00, 0x00, 0xA8, 0x6E,
        0x37, 0xBE}}, /* the default weapons */
    {0x175088Fu, 40, {0x80, 0x3D, 0xF6, 0xEC, 0xD3, 0x01, 0x00, 0x48, 0x89, 0x4C, 0x24, 0x30, 0x0F, 0x85, 0x8D, 0x0A, 0x00, 0x00, 0x48, 0x89, 0xAC,
        0x24, 0x68, 0x14, 0x00, 0x00, 0x48, 0x8B, 0x2D, 0x48, 0xC6, 0xD2, 0x01, 0x83, 0xBD, 0xF8, 0xDC, 0x0D, 0x00, 0x0C}}, /* the login's state */
    {0x11E8315u, 48, {0x3B, 0xBE, 0x88, 0x07, 0x00, 0x00, 0x72, 0x11, 0x41, 0x8B, 0x04, 0x24, 0x3B, 0xF8, 0x0F, 0x83, 0xC6, 0x01, 0x00, 0x00, 0x48,
        0x8B, 0x44, 0x24, 0x70, 0x48, 0x8D, 0x0C, 0x7F, 0x48, 0xC1, 0xE1, 0x04, 0x48, 0x03, 0xCE, 0x41, 0x3B, 0x3C, 0x24, 0x72, 0x21, 0x48, 0x8B, 0x44,
        0x24, 0x70, 0x0F}}, /* a received loadout's slots: stride 0x30, count +0x788 */
};
static const uint32_t kDefaultWeapons[3] = {0xAA035309u, 0xA7977F98u, 0xBE376EA8u}; /* primary, secondary, throwable */

/* What a change takes out that a loadout may hold: stratagem types (and their stable ids) and weapon loadout ids. */
typedef struct Scrub
{
    uint32_t types[STR_MAX_NEW], stables[STR_MAX_NEW], weapons[REG_MAX_WEAPONS];
    int typeCount, weaponCount, pushed, scrubbed;
} Scrub;
static void (*g_scrubPush)(void); /* the native fixture's armory push, else the game's */

static uintptr_t ScrubPointer(uintptr_t at)
{
    uintptr_t value = 0;
    return at != 0 && !SafeRead(at, &value, 8) ? value : 0;
}

static int ScrubPinsMatch(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kLoadoutPins / sizeof kLoadoutPins[0]; i++)
        if (!PinMatches(game + kLoadoutPins[i].rva, kLoadoutPins[i].bytes, kLoadoutPins[i].size)) return 0;
    return 1;
}

static int ScrubHolds(const Scrub* s, uint32_t type)
{
    for (int i = 0; i < s->typeCount; i++) if (s->types[i] == type) return 1;
    return 0;
}

static int ScrubStable(const Scrub* s, uint32_t stable)
{
    for (int i = 0; i < s->typeCount; i++) if (s->stables[i] == stable) return 1;
    return 0;
}

static int ScrubWeapon(const Scrub* s, uint32_t weapon)
{
    for (int i = 0; i < s->weaponCount; i++) if (s->weapons[i] == weapon) return 1;
    return 0;
}

/* Whether a record's loadout holds a stratagem type or weapon the change takes out (reads only). */
static int ScrubLoadoutHolds(uintptr_t loadout, const Scrub* s)
{
    uint32_t slots = 0, type = 0, weapon = 0;
    SafeRead(loadout + LS_SLOT_COUNT, &slots, 4);
    for (uint32_t k = 0; k < slots && k < LS_MAX_SLOTS; k++)
        if (!SafeRead(loadout + LS_SLOTS + (uintptr_t)k * LS_SLOT, &type, 4) && ScrubHolds(s, type)) return 1;
    for (int w = 0; w < 3; w++) if (!SafeRead(loadout + (uintptr_t)w * 0x60, &weapon, 4) && ScrubWeapon(s, weapon)) return 1;
    return 0;
}

/* Whether the profile holds a stable id or weapon the change takes out (reads only). */
static int ScrubProfileHolds(uintptr_t profile, const Scrub* s)
{
    uint32_t pairs[LS_MAX_SLOTS * 2], weapons[3];
    if (profile == 0 || SafeRead(profile + LS_PAIRS, pairs, sizeof pairs) || SafeRead(profile + 0x40, weapons, sizeof weapons)) return 0;
    for (uint32_t k = 0; k < LS_MAX_SLOTS; k++) if (pairs[2 * k] != 0 && ScrubStable(s, pairs[2 * k])) return 1;
    for (int w = 0; w < 3; w++) if (ScrubWeapon(s, weapons[w])) return 1;
    return 0;
}

/* Whether the history or the profile holds anything the change takes out. */
static int ScrubNeeded(uintptr_t game, const Scrub* s)
{
    uintptr_t history = ScrubPointer(game + LS_HISTORY);
    uint32_t records = 0;
    if (history != 0 && !SafeRead(history + LS_RECORDS, &records, 4))
        for (uint32_t r = 0; r < records && r < LS_MAX_RECORDS; r++)
            if (ScrubLoadoutHolds(history + (uintptr_t)r * LS_RECORD + LS_LOADOUT, s)) return 1;
    return ScrubProfileHolds(ScrubPointer(game + LS_PROFILE), s);
}

/* Whether the loadout screen is open (it keeps copies of its own, and launches missions). */
static int ScrubScreenOpen(uintptr_t game)
{
    uintptr_t ui = ScrubPointer(game + LS_UI), menu = ScrubPointer(game + LS_MENU);
    uint32_t presenter = 0, top = 0;
    return (ui != 0 && !SafeRead(ui + 0x4294, &presenter, 4) && presenter == 14) || (menu != 0 && !SafeRead(menu, &top, 4) && top == 11);
}

/* Why the loadout cannot be left as a boot would leave it now, or NULL. */
static const char* ScrubReady(uintptr_t game)
{
    if (!ScrubPinsMatch(game)) return "capability\tthe game's loadout code is not the reviewed one, so what the change takes out may stay in the loadout; a restart drops it";
    uintptr_t manager = ScrubPointer(game + LS_MANAGER), history = ScrubPointer(game + LS_HISTORY);
    uint32_t login = 0, records = 0;
    if (manager == 0 || SafeRead(manager + 0xDDCF8, &login, 4) || login != 12 || history == 0 || SafeRead(history + LS_RECORDS, &records, 4) || records == 0)
        return "state\tthe game has not finished logging in; try again in a moment";
    if (ScrubScreenOpen(game)) return "state\tthe loadout screen is open; close it and try again";
    return NULL;
}

/* ---- a stratagem type another mod takes over (KB hellforge-live-mod-sync-tombstone-reuse) ---------------------------
 * The game acquires a stratagem's packages, and releases them, from the row table[type] names at that moment
 * (0x1753080; the package system counts holds per package id). A hold taken with the old row and released after another
 * mod's row took the type would release that row's packages instead, which something else may still use. The holds that
 * release stratagem packages: a loadout loaded for a mission (load type 2), each record's loaded copy (+0xA28) and those
 * of players who left during it (PlayerHistory + 0x2FD10, 0x9F0 each, their count at + 0x2FD0C), and the shared
 * stratagems (+0x45410: 64 entries of 0x6C, {type, choice, state}). On the ship all of them are empty: the ship's load
 * unloads the left players' copies, reloads every record's for the ship (load type 1: weapons only) and the per-update
 * pass (0x1367690) releases each shared stratagem no world object uses. With the loadout screen closed none can be
 * taken before the change is made. */

#define LS_COPY 0xA28u
#define LS_LOAD_TYPE 0x9E4u
#define LS_LEFT_COUNT 0x2FD0Cu
#define LS_LEFT 0x2FD10u
#define LS_LOADOUT_BYTES 0x9F0u
#define LS_SHARED 0x45410u
#define LS_SHARED_ENTRY 0x6Cu
#define LS_SHARED_COUNT 64u

static const LoadoutPin kHeldPins[] = {
    {0x1368D29u, 47, {0x48, 0x8D, 0x81, 0x10, 0x54, 0x04, 0x00, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xE9, 0x39, 0x58, 0x08, 0x74, 0x10, 0x44, 0x8B, 0x0A,
        0x44, 0x39, 0x08, 0x75, 0x08, 0x8B, 0x4A, 0x04, 0x39, 0x48, 0x04, 0x74, 0x3F, 0x41, 0xFF, 0xC0, 0x48, 0x83, 0xC0, 0x6C, 0x41, 0x83, 0xF8, 0x40,
        0x72, 0xDE}}, /* the shared stratagems: +0x45410, 64 of 0x6C, their state +8 */
    {0x1369120u, 7, {0x83, 0xBA, 0xE4, 0x09, 0x00, 0x00, 0x00}}, /* a loaded copy's load type +0x9E4 */
    {0x1364C60u, 15, {0xBE, 0x0C, 0x14, 0x00, 0x00, 0x00, 0x75, 0x35, 0x48, 0x8D, 0x8E, 0x28, 0x0A, 0x00, 0x00}}, /* the record's copy +0xA28 */
    {0xAC5695u, 33, {0x39, 0x9F, 0x0C, 0xFD, 0x02, 0x00, 0x76, 0x28, 0x0F, 0x1F, 0x00, 0x8B, 0xC3, 0x48, 0x8B, 0xCF, 0x48, 0x69, 0xD0, 0xF0, 0x09, 0x00,
        0x00, 0x48, 0x81, 0xC2, 0x10, 0xFD, 0x02, 0x00, 0x48, 0x03, 0xD7}}, /* the left players' copies, unloaded by the ship's load */
};

/* Whether a loaded copy of a loadout holds the type's packages: loaded for a mission and listing it. */
static int ScrubCopyHolds(uintptr_t copy, uint32_t type)
{
    static uint8_t loadout[LS_LOADOUT_BYTES]; /* the change's thread only */
    if (SafeRead(copy, loadout, sizeof loadout) || *(const uint32_t*)(loadout + LS_LOAD_TYPE) != 2) return 0;
    uint32_t slots = *(const uint32_t*)(loadout + LS_SLOT_COUNT);
    for (uint32_t k = 0; k < slots && k < LS_MAX_SLOTS; k++) if (*(const uint32_t*)(loadout + LS_SLOTS + k * LS_SLOT) == type) return 1;
    return 0;
}

/* Whether the game holds packages of stratagem type `type` by its number (above). */
static int ScrubTypeHolds(uintptr_t history, uint32_t type)
{
    uint32_t records = 0, left = 0;
    SafeRead(history + LS_RECORDS, &records, 4);
    SafeRead(history + LS_LEFT_COUNT, &left, 4);
    for (uint32_t r = 0; r < records && r < LS_MAX_RECORDS; r++) if (ScrubCopyHolds(history + (uintptr_t)r * LS_RECORD + LS_COPY, type)) return 1;
    for (uint32_t r = 0; r < left && r < LS_MAX_RECORDS; r++) if (ScrubCopyHolds(history + LS_LEFT + (uintptr_t)r * LS_LOADOUT_BYTES, type)) return 1;
    static uint8_t shared[LS_SHARED_COUNT * LS_SHARED_ENTRY];
    if (SafeRead(history + LS_SHARED, shared, sizeof shared)) return 1; /* unreadable: assume it holds */
    for (uint32_t i = 0; i < LS_SHARED_COUNT; i++)
    {
        const uint8_t* entry = shared + i * LS_SHARED_ENTRY;
        if (*(const uint32_t*)(entry + 8) != 0 && *(const uint32_t*)entry == type) return 1;
    }
    return 0;
}

/* The first of a takeover's pins (the scrub's and the holds') that does not read as reviewed, or 0. game.dll's code is
 * decoded in parts after the game loads, so a pin can differ for a while (LiveTakeoversWait waits). */
static uint32_t ScrubTakeoverPin(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kLoadoutPins / sizeof kLoadoutPins[0]; i++)
        if (!PinMatches(game + kLoadoutPins[i].rva, kLoadoutPins[i].bytes, kLoadoutPins[i].size)) return kLoadoutPins[i].rva;
    for (size_t i = 0; i < sizeof kHeldPins / sizeof kHeldPins[0]; i++)
        if (!PinMatches(game + kHeldPins[i].rva, kHeldPins[i].bytes, kHeldPins[i].size)) return kHeldPins[i].rva;
    return 0;
}

/* Why another mod's stratagem cannot take type `type` now, or NULL. */
static const char* ScrubTakeover(uintptr_t game, uint32_t type, char* why, size_t size)
{
    uint32_t pin = ScrubTakeoverPin(game);
    if (pin != 0)
        snprintf(why, size, "capability\tthe game's loadout code (game.dll+0x%x) is not the reviewed one, so stratagem %u cannot pass to another mod; a "
            "restart numbers it as the host does", pin, type);
    else if (ScrubScreenOpen(game)) snprintf(why, size, "state\tthe loadout screen is open; close it and try again");
    else
    {
        uintptr_t history = ScrubPointer(game + LS_HISTORY);
        if (history == 0 || !ScrubTypeHolds(history, type)) return NULL;
        snprintf(why, size, "state\tthe game still holds stratagem %u's packages (a mission's loadout or a shared stratagem); try again in a moment", type);
    }
    return why;
}

/* The stratagem types out of a loadout's slots (the later ones moved down, the freed last one zeroed). */
static int ScrubSlots(uint8_t* loadout, const Scrub* s)
{
    uint32_t* count = (uint32_t*)(loadout + LS_SLOT_COUNT);
    int removed = 0;
    for (uint32_t k = 0; k < *count && k < LS_MAX_SLOTS;)
    {
        uint8_t* slot = loadout + LS_SLOTS + (size_t)k * LS_SLOT;
        if (!ScrubHolds(s, *(uint32_t*)slot)) { k++; continue; }
        uint32_t last = (*count < LS_MAX_SLOTS ? *count : LS_MAX_SLOTS) - 1;
        memmove(slot, slot + LS_SLOT, (size_t)(last - k) * LS_SLOT);
        memset(loadout + LS_SLOTS + (size_t)last * LS_SLOT, 0, LS_SLOT);
        (*count)--;
        removed++;
    }
    return removed;
}

/* On the game thread: the types out of every record's loadout (the loaded copy at +0xA28 left alone, so its unload
 * releases what was loaded) and their stable ids out of the profile; the weapons back to the defaults in the profile,
 * then pushed to the history. */
static void ScrubStep(uintptr_t game, Scrub* s)
{
    uintptr_t history = ScrubPointer(game + LS_HISTORY), profile = ScrubPointer(game + LS_PROFILE);
    if (history == 0 || profile == 0) return;
    uint32_t records = *(uint32_t*)(history + LS_RECORDS);
    for (uint32_t r = 0; r < records && r < LS_MAX_RECORDS; r++) s->scrubbed += ScrubSlots((uint8_t*)history + (size_t)r * LS_RECORD + LS_LOADOUT, s);
    uint32_t* pairs = (uint32_t*)(profile + LS_PAIRS);
    for (uint32_t k = 0; k < LS_MAX_SLOTS;)
    {
        if (pairs[2 * k] == 0 || !ScrubStable(s, pairs[2 * k])) { k++; continue; }
        memmove(pairs + 2 * k, pairs + 2 * k + 2, (size_t)(LS_MAX_SLOTS - 1 - k) * 8);
        memset(pairs + 2 * (LS_MAX_SLOTS - 1), 0, 8);
        s->scrubbed++;
    }
    uint32_t* weapons = (uint32_t*)(profile + 0x40);
    int changed = 0;
    for (int w = 0; w < 3; w++) if (ScrubWeapon(s, weapons[w])) { weapons[w] = kDefaultWeapons[w]; changed = 1; }
    if (!changed) return;
    s->pushed = 1;
    s->scrubbed++;
    (g_scrubPush != NULL ? g_scrubPush : (void (*)(void))(game + LS_PUSH))();
}

/* On the game thread, a few frames after the push (its request is handled then): a weapon still in the local record is
 * set to the default directly (its customization stays the old weapon's attachments). */
static void ScrubVerify(uintptr_t game, const Scrub* s)
{
    uintptr_t history = ScrubPointer(game + LS_HISTORY), peers = ScrubPointer(game + LS_PEER);
    uint64_t self = 0;
    if (history == 0 || peers == 0 || SafeRead(peers + 0xB398, &self, 8)) return;
    uint32_t records = *(uint32_t*)(history + LS_RECORDS);
    for (uint32_t r = 0; r < records && r < LS_MAX_RECORDS; r++)
    {
        uint8_t* record = (uint8_t*)history + (size_t)r * LS_RECORD;
        if (*(uint64_t*)record != self) continue;
        for (int w = 0; w < 3; w++)
        {
            uint32_t* id = (uint32_t*)(record + LS_LOADOUT + (size_t)w * 0x60);
            if (ScrubWeapon(s, *id)) { Note("loadout: weapon %08x still in the history after the push; set to the default", *id); *id = kDefaultWeapons[w]; }
        }
    }
}
