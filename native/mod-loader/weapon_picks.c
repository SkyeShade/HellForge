/* SDK 25: a weapon's customization picks (weapon_pick, set_weapon_pick; docs/modding/content.md "Ammunition of your
 * own"). Included by attachments.c; its tick runs on attachments.c's game-thread update. game.dll 25480438: the
 * ItemCustomizationManager [game + 0x347CEF8] keeps at +0x178A4EC 512 entries of 0x94 bytes, {u32 weapon, u32 state,
 * u32 template, u32 picks[3][10] by slot number, ...}; the armory's customize screen reads a weapon's picks there
 * (0x1493770, 0x1494590). EquipCustomizationItem (0x1035370: store, weapon, item, slot, template, refresh UI) is what
 * the screen calls: it stores the pick, and for an entry in state 0 or 1 copies the weapon's picks into the local
 * player's loadout and sends it (0x1034180), which is how a friend's game builds the weapon alike. Calling it from the
 * game thread keeps it the game's own path. It does not check the game's state, so neither do we: a mod that wants
 * picks in the lobby only checks game_state itself. */

#define WP_MANAGER 0x347CEF8u
#define WP_STORE 0x178A4ECu
#define WP_ENTRIES 512u
#define WP_ENTRY 0x94u
#define WP_PICKS 0x0Cu
#define WP_TEMPLATES 3u
#define WP_SLOTS 10u
#define WP_EQUIP 0x1035370u
#define WP_QUEUE 16

static const uint8_t kWpEquipPin[27] = {0x40, 0x53, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8B,
    0x05, 0x8D, 0x6C, 0x60, 0x01, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x44, 0x24, 0x58};

typedef void (*WpEquipFn)(uint8_t* store, uint32_t weapon, uint32_t item, uint32_t slot, uint32_t pickTemplate, uint8_t refresh);
static WpEquipFn g_wpEquip; /* the game's, once its bytes are the reviewed ones (a fixture sets its own) */

typedef struct WpRequest { HfMod* mod; uint32_t weapon, slot, item; } WpRequest;
static WpRequest g_wpQueue[WP_QUEUE];
static int g_wpQueued;
static SRWLOCK g_wpLock = SRWLOCK_INIT;

/* The manager's entry of a weapon, or NULL (no manager yet, or the weapon has none). */
static uint8_t* WpEntry(uintptr_t game, uint32_t weapon)
{
    uint8_t* manager = NULL;
    if (game == 0 || weapon == 0 || SafeRead(game + WP_MANAGER, &manager, 8) || manager == NULL) return NULL;
    uint8_t* store = manager + WP_STORE;
    for (uint32_t i = 0; i < WP_ENTRIES; i++)
    {
        uint32_t id;
        if (SafeRead((uintptr_t)(store + (size_t)i * WP_ENTRY), &id, 4)) return NULL;
        if (id == weapon) return store + (size_t)i * WP_ENTRY;
    }
    return NULL;
}

/* The template the entry shows (CYCLE WEAPON CONFIG 1..3), 0 when it has none yet (the game's defaults: -1). */
static uint32_t WpTemplate(const uint8_t* entry)
{
    uint32_t t;
    memcpy(&t, entry + 8, 4);
    return t < WP_TEMPLATES ? t : 0;
}

static uint32_t ApiWeaponPick(HfMod* mod, uint32_t weapon, uint32_t slot)
{
    const uint8_t* entry = slot != 0 && slot < WP_SLOTS ? WpEntry(g_game, weapon) : NULL; /* slots 1 to 9, as set_weapon_pick */
    if (entry == NULL) return 0;
    uint32_t item;
    memcpy(&item, entry + WP_PICKS + 4u * (WP_SLOTS * WpTemplate(entry) + slot), 4);
    return item;
}

/* The customization item `item` in slot `slot`, offered on `weapon`; NULL fine, else why not. */
static const char* WpItemWhy(uintptr_t game, uint32_t weapon, uint32_t slot, uint32_t item)
{
    for (int i = 0; i < g_attachmentCount; i++)
    {
        const Attachment* a = &g_attachments[i];
        if (a->id != item) continue;
        if (a->state != 1) return "that attachment is not in the game";
        if (a->slot != slot) return "that attachment is for another slot";
        for (uint32_t w = 0; w < a->weaponCount; w++) if (a->weapons[w] == weapon) return NULL;
        return "that attachment is not offered on this weapon";
    }
    const uint8_t* vanilla = AttVanillaItem(game, item);
    if (vanilla == NULL) return "no customization item has that id";
    return AttSlot(vanilla) == slot ? NULL : "that item is for another slot";
}

static int ApiSetWeaponPick(HfMod* mod, uint32_t weapon, uint32_t slot, uint32_t item)
{
    uintptr_t game = g_game;
    const char* why = NULL;
    if (game == 0 || g_wpEquip == NULL) why = "the game's customization manager is not the reviewed one, or not loaded yet";
    else if (slot == 0 || slot >= WP_SLOTS) why = "slots are 1 to 9";
    else if (WpEntry(game, weapon) == NULL) why = "the game has no customization entry for this weapon";
    else why = WpItemWhy(game, weapon, slot, item);
    if (why == NULL)
    {
        AcquireSRWLockExclusive(&g_wpLock);
        if (g_wpQueued < WP_QUEUE) g_wpQueue[g_wpQueued++] = (WpRequest){mod, weapon, slot, item};
        else why = "too many picks wait for the game thread";
        ReleaseSRWLockExclusive(&g_wpLock);
    }
    if (why != NULL) { ApiLog(mod, "set_weapon_pick(%08x, slot %u, %08x): %s", weapon, slot, item, why); return 1; }
    return 0;
}

/* Whether EquipCustomizationItem's bytes are the reviewed ones (once; logged when not). */
static void WpProve(uintptr_t game)
{
    static int proven;
    if (proven || game == 0) return;
    proven = 1;
    if (g_wpEquip == NULL && PinMatches(game + WP_EQUIP, kWpEquipPin, sizeof kWpEquipPin)) g_wpEquip = (WpEquipFn)(game + WP_EQUIP);
    else if (g_wpEquip == NULL) Note("weapon picks: the game's EquipCustomizationItem is not the reviewed one; set_weapon_pick is off");
}

/* On the game thread: the waiting picks, through the game's own call. */
static void WeaponPicksTick(uintptr_t game)
{
    WpProve(game);
    WpRequest todo[WP_QUEUE];
    AcquireSRWLockExclusive(&g_wpLock);
    int n = g_wpQueued;
    memcpy(todo, g_wpQueue, sizeof todo);
    g_wpQueued = 0;
    ReleaseSRWLockExclusive(&g_wpLock);
    for (int i = 0; i < n; i++)
    {
        WpRequest* r = &todo[i];
        uint8_t* entry = WpEntry(game, r->weapon);
        if (entry == NULL || g_wpEquip == NULL)
        { ApiLog(r->mod, "set_weapon_pick(%08x, slot %u, %08x): dropped, the weapon's entry is gone", r->weapon, r->slot, r->item); continue; }
        uint32_t t = WpTemplate(entry), shown;
        memcpy(&shown, entry + 8, 4);
        g_wpEquip(*(uint8_t**)(game + WP_MANAGER) + WP_STORE, r->weapon, r->item, r->slot, t, 1);
        ApiLog(r->mod, "set_weapon_pick: weapon %08x slot %u loads item %08x (template %u%s)", r->weapon, r->slot, r->item, t + 1,
            shown < WP_TEMPLATES ? "" : "; the weapon showed the game's defaults");
    }
}
