/* Items the player owns, for content a live change adds (live_content.c, SDK 16): the game asks the backend for its item
 * catalogue and inventory only once, at login, so an item added later (a mod's stratagem, a weapon) is in neither. The
 * loader makes it owned as a purchase reply would: it appends copies of the base's catalogue records under the item's
 * ids, sorts the catalogue again, then adds one to the inventory. An item of a mod taken out is given back the same way.
 *
 * game.dll build 25480438 (KB hellforge-live-mod-sync-re-content, R2): the progression manager [game + 0x347CEF8] holds
 * the catalogue and the inventory in one table: a count at +0x1CE0, 0xB8-byte records from +0x1CE4 (+4 mixId, +8 itemId,
 * +0xC category, +0x10 a field, +0x14 state, +0x18 maxCap, +0x1C buy prices, +0x4C sell prices, +0x7C four dwords,
 * +0x8C required items, +0xB0 amount, +0xB4 a byte), an index of 24-byte entries from +0xB9CE4 ({record, mixId, itemId,
 * sibling, distance back to its parent, children}), the categories' starts at +0xD1CE4 and the statuses at +0xD5D48 and
 * +0xD5D4C (2: loaded). 0x136EC00 appends a record (children link to the root appended just before them), 0x136E6A0
 * sorts the catalogue (it only adds to the category starts: they are zeroed first), 0x136EF80(manager, mixId, amount,
 * 0, 0, 0) adds to an item as the buy/v2 reply does (no popup, no backend request), 0x136F5E0(manager, mixId, amount)
 * takes it away. A mixId the catalogue does not have is ignored by all three. They run on the game thread, where the
 * game applies its replies (live_content.c's step). Included by live_content.c. */

#define OWN_MANAGER 0x347CEF8u
#define OWN_ADD 0x136EC00u
#define OWN_SORT 0x136E6A0u
#define OWN_APPLY 0x136EF80u
#define OWN_REMOVE 0x136F5E0u
#define OWN_COUNT 0x1CE0u
#define OWN_RECORDS 0x1CE4u
#define OWN_RECORD 0xB8u
#define OWN_INDEX 0xB9CE4u
#define OWN_STARTS 0xD1CE4u
#define OWN_CATALOGUE 0xD5D48u
#define OWN_INVENTORY 0xD5D4Cu
#define OWN_MAX 4096u
#define OWN_ITEMS 64

static const uint8_t kOwnApplyPin[47] = {0x44, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00, 0x45, 0x33, 0xE4, 0x45, 0x8B, 0xE9, 0x41, 0x8B, 0xF8, 0x8B, 0xF2, 0x4C, 0x8B, 0xF1, 0x81, 0xFA, 0x50, 0xC8,
    0x84, 0x35, 0x0F};
static const uint8_t kOwnAddPin[33] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24,
    0x20, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x4C, 0x8B, 0x2D, 0xD7, 0xE2, 0x10, 0x02};
static const uint8_t kOwnSortPin[35] = {0x48, 0x8B, 0x35, 0x30, 0xE8, 0x10, 0x02, 0x33, 0xDB, 0x4D, 0x89, 0x73, 0x18, 0x44, 0x8B, 0xF3, 0x8B, 0xAE, 0xE0,
    0x1C, 0x00, 0x00, 0x4C, 0x8D, 0xAE, 0xE4, 0x1C, 0x00, 0x00, 0x89, 0xAE, 0x44, 0x1D, 0x0D, 0x00};
static const uint8_t kOwnRemovePin[20] = {0x85, 0xD2, 0x0F, 0x84, 0x91, 0x00, 0x00, 0x00, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0x91, 0xE0, 0x1C,
    0x00, 0x00};

typedef uint64_t (*OwnAddFn)(uint64_t unused, uint32_t mixId, uint32_t itemId, uint32_t parent, int32_t field, char flag, const void* buy, uint32_t buyCount,
    const void* sell, uint32_t sellCount, const uint32_t* dwords, uint32_t dwordCount, const uint32_t* required, uint32_t requiredCount, uint32_t category,
    uint32_t maxCap);
typedef void (*OwnSortFn)(void);
typedef void (*OwnApplyFn)(uintptr_t manager, uint32_t mixId, uint32_t amount, uint32_t source, const char* text, int extra);
typedef void (*OwnRemoveFn)(uintptr_t manager, uint32_t mixId, uint32_t amount);
/* The game's functions once proven, and its manager (the native fixture sets its own). */
static OwnAddFn g_ownAdd;
static OwnSortFn g_ownSort;
static OwnApplyFn g_ownApply;
static OwnRemoveFn g_ownRemove;
static uintptr_t (*g_ownManager)(void);

static uintptr_t OwnManagerNow(void)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL), manager = 0;
    return game != 0 && !SafeRead(game + OWN_MANAGER, &manager, 8) ? manager : 0;
}

/* The game's catalogue functions, checked once. 1 when they are the reviewed ones. */
static int OwnProve(void)
{
    if (g_ownApply != NULL) return 1;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (game == 0 || !PinMatches(game + OWN_APPLY, kOwnApplyPin, sizeof kOwnApplyPin) || !PinMatches(game + OWN_ADD, kOwnAddPin, sizeof kOwnAddPin) ||
        !PinMatches(game + OWN_SORT + 0x21, kOwnSortPin, sizeof kOwnSortPin) || !PinMatches(game + OWN_REMOVE, kOwnRemovePin, sizeof kOwnRemovePin))
        return 0;
    g_ownAdd = (OwnAddFn)(game + OWN_ADD);
    g_ownSort = (OwnSortFn)(game + OWN_SORT);
    g_ownRemove = (OwnRemoveFn)(game + OWN_REMOVE);
    g_ownManager = OwnManagerNow;
    MemoryBarrier();
    g_ownApply = (OwnApplyFn)(game + OWN_APPLY);
    return 1;
}

/* An index entry of the catalogue by mixId (category: any when 0), or -1. */
static int OwnEntry(uintptr_t manager, uint32_t mixId, uint32_t itemId, uint32_t category)
{
    uint32_t count = *(const uint32_t*)(manager + OWN_COUNT);
    for (uint32_t i = 0; i < count && i < OWN_MAX; i++)
    {
        const uint32_t* entry = (const uint32_t*)(manager + OWN_INDEX + (uintptr_t)i * 24);
        const uint8_t* record = (const uint8_t*)(manager + OWN_RECORDS + (uintptr_t)entry[0] * OWN_RECORD);
        if ((mixId == 0 || entry[1] == mixId) && (itemId == 0 || entry[2] == itemId) && (category == 0 || *(const uint32_t*)(record + 0xC) == category))
            return (int)i;
    }
    return -1;
}

static const uint8_t* OwnRecordAt(uintptr_t manager, int entry)
{
    const uint32_t* e = (const uint32_t*)(manager + OWN_INDEX + (uintptr_t)entry * 24);
    return (const uint8_t*)(manager + OWN_RECORDS + (uintptr_t)e[0] * OWN_RECORD);
}

/* A copy of a catalogue record under new ids (parent 0: a root), as the login's builder appends one. */
static void OwnAppend(const uint8_t* source, uint32_t mixId, uint32_t itemId, uint32_t parent)
{
    int32_t field; /* the reply's i16, as the builder stored it (sign-extended) */
    memcpy(&field, source + 0x10, 4);
    g_ownAdd(0, mixId, itemId, parent, field, (char)source[0xB4], source + 0x1C, 4, source + 0x4C, 4, (const uint32_t*)(source + 0x7C), 4,
        (const uint32_t*)(source + 0x90), *(const uint32_t*)(source + 0x8C) <= 8 ? *(const uint32_t*)(source + 0x8C) : 8,
        *(const uint32_t*)(source + 0xC), *(const uint32_t*)(source + 0x18));
}

/* One item to make owned: a stratagem (copies of its base's category 11 item and category 10 child), a weapon (its base's
 * root item and children) or a difficulty's unlock item (level 10's copied, owned when level 10's is: RegisteredItems.cs). */
enum { OWN_WEAPON, OWN_STRATAGEM, OWN_DIFFICULTY };
/* Why an item was not added: its base is not in the catalogue as the login built it. */
static const char* const kOwnNoBase[] = {"a new weapon's base has no root item in the catalogue; it is owned from the next session",
    "a new stratagem's base has no item in the catalogue; it is owned from the next session",
    "level 10's unlock item is not in the catalogue; level 11's is owned from the next session"};
typedef struct OwnItem { int kind; uint32_t mixId, stable, baseStable, baseMixId; int take; } OwnItem;
typedef struct OwnBatch { OwnItem items[OWN_ITEMS]; int count, added, owned, taken; const char* why; } OwnBatch;

/* A stratagem's records: the base's child (category 10, itemId its stable id) and its item (the child's parent). */
static int OwnStratagem(uintptr_t manager, const OwnItem* item)
{
    int child = OwnEntry(manager, 0, item->baseStable, 10);
    if (child < 0) return 0;
    const uint32_t* entry = (const uint32_t*)(manager + OWN_INDEX + (uintptr_t)child * 24);
    if (entry[4] == 0 || entry[4] > (uint32_t)child) return 0;
    const uint8_t* parentRecord = OwnRecordAt(manager, child - (int)entry[4]);
    OwnAppend(parentRecord, item->mixId, item->mixId, 0);
    OwnAppend(OwnRecordAt(manager, child), WeaponChild(item->stable, item->mixId), item->stable, item->mixId);
    return 2;
}

/* A weapon's records: its base's root item, then each of its children rehomed to the new id. */
static int OwnWeapon(uintptr_t manager, const OwnItem* item)
{
    int root = OwnEntry(manager, item->baseMixId, 0, 0);
    if (root < 0) return 0;
    OwnAppend(OwnRecordAt(manager, root), item->mixId, item->mixId, 0);
    int added = 1;
    uint32_t count = *(const uint32_t*)(manager + OWN_COUNT);
    for (uint32_t i = (uint32_t)root + 1; i < count && i < OWN_MAX; i++)
    {
        const uint32_t* entry = (const uint32_t*)(manager + OWN_INDEX + (uintptr_t)i * 24);
        if (entry[4] == 0 || i - entry[4] != (uint32_t)root) break; /* past the base's children */
        OwnAppend(OwnRecordAt(manager, (int)i), Rehome(entry[1], item->baseMixId, item->mixId), entry[2], item->mixId);
        added++;
    }
    return added;
}

/* A difficulty's unlock item: a copy of level 10's under its own id (by its id: the login files it under category 0,
 * seen 2026-10-25). */
static int OwnDifficulty(uintptr_t manager, const OwnItem* item)
{
    int base = OwnEntry(manager, item->baseMixId, 0, 0);
    if (base < 0) return 0;
    OwnAppend(OwnRecordAt(manager, base), item->mixId, item->mixId, 0);
    return 1;
}

/* Whether an item is owned (its record's state 2 or 4). */
static int OwnOwned(uintptr_t manager, uint32_t mixId)
{
    int entry = OwnEntry(manager, mixId, 0, 0);
    uint32_t state = entry >= 0 ? *(const uint32_t*)(OwnRecordAt(manager, entry) + 0x14) : 0;
    return state == 2 || state == 4;
}

/* One item's records appended, copies of its base's: how many (0: its base is not in the catalogue). */
static int OwnAppendItem(uintptr_t manager, const OwnItem* item)
{
    if (item->kind == OWN_STRATAGEM) return OwnStratagem(manager, item);
    return item->kind == OWN_WEAPON ? OwnWeapon(manager, item) : OwnDifficulty(manager, item);
}

/* A record's category set back (0: none to set). 1 when it changed. */
static int OwnRefile(uintptr_t manager, int entry, uint32_t category)
{
    uint32_t* at = entry >= 0 ? (uint32_t*)(OwnRecordAt(manager, entry) + 0xC) : NULL;
    if (at == NULL || *at == category) return 0;
    *at = category;
    return 1;
}

/* A stratagem back in the catalogue (its mod taken out and set up again this session): the sort files a stratagem's item
 * and child that no stratagem row names under category 0 in their records, for good (0x136E6A0), which it did if another
 * mod took its type over meanwhile (seen in game 2026-10-25); they go back to 11 and 10 before the next sort. 1 when
 * either did. */
static int OwnRestore(uintptr_t manager, const OwnItem* item)
{
    if (item->kind != OWN_STRATAGEM) return 0;
    int root = OwnRefile(manager, OwnEntry(manager, item->mixId, item->mixId, 0), 11);
    return OwnRefile(manager, OwnEntry(manager, WeaponChild(item->stable, item->mixId), item->stable, 0), 10) | root;
}

/* The batch's new records appended (and stratagems back filed again): 1 when the catalogue changed. */
static int OwnAppendAll(uintptr_t manager, OwnBatch* b)
{
    int appended = 0;
    for (int i = 0; i < b->count; i++)
    {
        const OwnItem* item = &b->items[i];
        if (item->take) continue; /* given back */
        if (OwnEntry(manager, item->mixId, 0, 0) >= 0) { appended |= OwnRestore(manager, item); continue; } /* in the catalogue already */
        if (*(const uint32_t*)(manager + OWN_COUNT) + 16 > OWN_MAX) { b->why = "the game's catalogue is full"; break; }
        int added = OwnAppendItem(manager, item);
        if (added == 0 && b->why == NULL) b->why = kOwnNoBase[item->kind];
        b->added += added;
        appended |= added > 0;
    }
    return appended;
}

/* On the game thread: the batch's records appended, the catalogue sorted, and each item owned (or, take, given back). */
static void OwnBatchStep(void* arg)
{
    OwnBatch* b = (OwnBatch*)arg;
    /* a change whose items are all found on this step (weapons: LiveRegistryStep) has not proven the functions yet */
    if (g_ownApply == NULL && !OwnProve()) { b->why = "the game's catalogue functions are not the reviewed ones; new items are owned from the next session"; return; }
    uintptr_t manager = g_ownManager != NULL ? g_ownManager() : 0;
    if (manager == 0 || *(const uint32_t*)(manager + OWN_CATALOGUE) != 2 || *(const uint32_t*)(manager + OWN_INVENTORY) != 2)
    { b->why = "the game's catalogue is not loaded"; return; }
    if (OwnAppendAll(manager, b))
    {
        memset((void*)(manager + OWN_STARTS), 0, 24 * 4);
        g_ownSort();
    }
    /* every item given back before any is owned: one a change gives back and owns (a new version renumbering its
     * stratagems) stays owned */
    for (int i = 0; i < b->count; i++) if (b->items[i].take) { g_ownRemove(manager, b->items[i].mixId, 1); b->taken++; }
    for (int i = 0; i < b->count; i++)
    {
        const OwnItem* item = &b->items[i];
        if (!item->take && OwnEntry(manager, item->mixId, 0, 0) >= 0 && (item->kind != OWN_DIFFICULTY || OwnOwned(manager, item->baseMixId)))
        { g_ownApply(manager, item->mixId, 1, 0, NULL, 0); b->owned++; }
    }
}
