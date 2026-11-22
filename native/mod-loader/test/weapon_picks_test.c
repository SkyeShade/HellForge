/* Included by stats_test.c: a weapon's picks (weapon_picks.c) against a synthetic customization manager, and a mod
 * weapon's ammunition default (registry_load.c GiveAmmunition). */
static struct { uint8_t* store; uint32_t weapon, item, slot, pickTemplate, calls; uint8_t refresh; } g_wpCall;
static void WpFakeEquip(uint8_t* store, uint32_t weapon, uint32_t item, uint32_t slot, uint32_t pickTemplate, uint8_t refresh)
{
    g_wpCall.store = store; g_wpCall.weapon = weapon; g_wpCall.item = item; g_wpCall.slot = slot;
    g_wpCall.pickTemplate = pickTemplate; g_wpCall.refresh = refresh; g_wpCall.calls++;
}

static void WeaponPicksFixture(void)
{
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, ATT_TABLE + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* manager = (uint8_t*)VirtualAlloc(NULL, WP_STORE + WP_ENTRIES * WP_ENTRY, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* gameState = (uint8_t*)VirtualAlloc(NULL, EVT_STATE_AT + 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(game && manager && gameState);
    uintptr_t at = (uintptr_t)manager; memcpy(game + WP_MANAGER, &at, 8);
    at = (uintptr_t)gameState; memcpy(game + EVT_STATE_GAME, &at, 8);
    uint32_t ship = HF_STATE_SHIP, mission = HF_STATE_MISSION;
    memcpy(gameState + EVT_STATE_AT, &ship, 4);
    /* two weapons' entries: one never customized (template -1), one showing template 2 with an ammunition pick */
    uint8_t* store = manager + WP_STORE;
    uint32_t fresh = 0x7E57u, worn = 0x1111u, none = 0xFFFFFFFFu, second = 1, picked = 0xA0000016u;
    memcpy(store + 3 * WP_ENTRY, &fresh, 4); memcpy(store + 3 * WP_ENTRY + 8, &none, 4);
    memcpy(store + 5 * WP_ENTRY, &worn, 4); memcpy(store + 5 * WP_ENTRY + 8, &second, 4);
    memcpy(store + 5 * WP_ENTRY + WP_PICKS + 4 * (WP_SLOTS * 1 + 6), &picked, 4);
    /* the game's items: an ammunition item (slot 6) and a magazine (slot 5) */
    static uint8_t items[2 * ATT_ITEM];
    static uint32_t ammoSlot = 6, magazineSlot = 5;
    uint32_t ammo = 0xA0000006u, magazine = 0xA0000005u, one = 1;
    memcpy(items + 8, &ammo, 4); at = (uintptr_t)&ammoSlot; memcpy(items + 0x30, &at, 8); memcpy(items + 0x38, &one, 4);
    memcpy(items + ATT_ITEM + 8, &magazine, 4); at = (uintptr_t)&magazineSlot; memcpy(items + ATT_ITEM + 0x30, &at, 8); memcpy(items + ATT_ITEM + 0x38, &one, 4);
    static struct { uint8_t* items; uint32_t count; uint32_t pad; } block;
    block.items = items; block.count = 2;
    at = (uintptr_t)&block; memcpy(game + ATT_TABLE, &at, 8); memcpy(game + ATT_COUNT, &one, 4);

    uintptr_t savedGame = g_game;
    WpEquipFn savedEquip = g_wpEquip;
    g_game = (uintptr_t)game;
    g_wpEquip = NULL;
    HfMod* mod = &g_mods[0];
    assert(ApiSetWeaponPick(mod, fresh, 6, ammo) == 1); /* the game's call is not proven yet */
    g_wpEquip = WpFakeEquip;
    memset(&g_wpCall, 0, sizeof g_wpCall);

    /* reads: the template shown, the first for a weapon the game never customized, 0 for an unknown one */
    assert(ApiWeaponPick(mod, worn, 6) == picked && ApiWeaponPick(mod, fresh, 6) == 0 && ApiWeaponPick(mod, 0x2222u, 6) == 0);
    assert(ApiWeaponPick(mod, worn, WP_SLOTS) == 0 && ApiWeaponPick(mod, worn, 0) == 0); /* slots are 1 to 9 */
    /* refusals: another slot's item, slot 0, an unknown weapon or item */
    assert(ApiSetWeaponPick(mod, fresh, 6, magazine) == 1 && ApiSetWeaponPick(mod, fresh, 0, ammo) == 1);
    assert(ApiSetWeaponPick(mod, 0x2222u, 6, ammo) == 1 && ApiSetWeaponPick(mod, fresh, 6, 0x12345678u) == 1);
    assert(g_wpQueued == 0);
    /* a mod's attachment only on the weapons it is offered on */
    int savedCount = g_attachmentCount;
    memset(&g_attachments[0], 0, sizeof g_attachments[0]);
    g_attachments[0].id = 0xA77AC4EDu; g_attachments[0].state = 1; g_attachments[0].slot = 6;
    g_attachments[0].weapons[0] = fresh; g_attachments[0].weaponCount = 1;
    g_attachmentCount = 1;
    assert(ApiSetWeaponPick(mod, worn, 6, 0xA77AC4EDu) == 1);

    /* queued, then made on the game thread through the game's call, in the template shown */
    assert(ApiSetWeaponPick(mod, fresh, 6, 0xA77AC4EDu) == 0 && ApiSetWeaponPick(mod, worn, 6, ammo) == 0 && g_wpQueued == 2);
    assert(g_wpCall.calls == 0);
    WeaponPicksTick((uintptr_t)game);
    assert(g_wpQueued == 0 && g_wpCall.calls == 2 && g_wpCall.store == store && g_wpCall.weapon == worn && g_wpCall.item == ammo &&
        g_wpCall.slot == 6 && g_wpCall.pickTemplate == 1 && g_wpCall.refresh == 1);
    /* in a mission too: the game's call does not check the state */
    memcpy(gameState + EVT_STATE_AT, &mission, 4);
    assert(ApiSetWeaponPick(mod, fresh, 6, ammo) == 0);
    WeaponPicksTick((uintptr_t)game);
    assert(g_wpCall.calls == 3 && g_wpQueued == 0 && g_wpCall.weapon == fresh && g_wpCall.pickTemplate == 0);

    /* a mod weapon's ammunition: the default in slot 6 and a slot the player may change, once; no room, nothing */
    static uint8_t record[REG_SLOTS_AT + 40];
    memset(record, 0, sizeof record);
    uint32_t pairs[4] = {3, 0x80FBE63Du, 6, 0x5F6938E0u}, slots[2] = {3, 5};
    memcpy(record, pairs, sizeof pairs); memcpy(record + REG_SLOTS_AT, slots, sizeof slots);
    Weapon w;
    memset(&w, 0, sizeof w);
    w.ammunition = 0xA77AC4EDu;
    assert(GiveAmmunition(&w, record) == 1 && GiveAmmunition(&w, record) == 1);
    const uint32_t* now = (const uint32_t*)record;
    const uint32_t* list = (const uint32_t*)(record + REG_SLOTS_AT);
    assert(now[2] == 6 && now[3] == 0xA77AC4EDu && now[4] == 0 && list[0] == 3 && list[1] == 5 && list[2] == 6 && list[3] == 0);
    for (int k = 0; k < 10; k++) memcpy(record + REG_SLOTS_AT + 4 * k, &(uint32_t){k + 1 == 6 ? 7 : k + 1}, 4);
    memset(record + 8, 0, 8); /* the ammunition pair gone: room in the pairs, none in the slots */
    assert(GiveAmmunition(&w, record) == 0 && now[2] == 0);

    g_attachmentCount = savedCount;
    g_wpEquip = savedEquip;
    g_game = savedGame;
    VirtualFree(gameState, 0, MEM_RELEASE);
    VirtualFree(manager, 0, MEM_RELEASE);
    VirtualFree(game, 0, MEM_RELEASE);
}
