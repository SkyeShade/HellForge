/* A mod weapon's ammunition (SDK 25, HfWeapon.ammunition) in its own customization record. Included by registry_load.c,
 * at the end of registry_paints.c (REG_SLOTS_AT). */

/* Its ammunition (HfWeapon.ammunition): the default of the ammunition slot, which joins the slots the player may change
 * so that a pick there (set_weapon_pick) is the weapon's own choice to save and send. 0 when there was no room. */
static int GiveAmmunition(const Weapon* w, uint8_t* customization)
{
    uint32_t pairs[REG_DEFAULT_PAIRS * 2], slots[10];
    memcpy(pairs, customization, sizeof pairs);
    memcpy(slots, customization + REG_SLOTS_AT, sizeof slots);
    int pair = 0, listed = 0, used = 0;
    while (pair < REG_DEFAULT_PAIRS && pairs[2 * pair] != REG_AMMUNITION_SLOT && (pairs[2 * pair] != 0 || pairs[2 * pair + 1] != 0)) pair++;
    for (; used < 10 && slots[used] != 0; used++) listed |= slots[used] == REG_AMMUNITION_SLOT;
    if (pair == REG_DEFAULT_PAIRS || (!listed && used == 10)) return 0;
    pairs[2 * pair] = REG_AMMUNITION_SLOT;
    pairs[2 * pair + 1] = w->ammunition;
    if (!listed) slots[used] = REG_AMMUNITION_SLOT;
    memcpy(customization, pairs, sizeof pairs);
    memcpy(customization + REG_SLOTS_AT, slots, sizeof slots);
    return 1;
}

/* Its ammunition given when it names one, logged either way. */
static void OwnAmmunition(const Weapon* w, uint8_t* customization)
{
    if (w->ammunition == 0 || customization == NULL) return;
    if (GiveAmmunition(w, customization)) ApiLog(w->mod, "%s: loads ammunition %08x by default; its ammunition slot is the player's", w->name, w->ammunition);
    else ApiLog(w->mod, "%s: no room for ammunition %08x in its customization record", w->name, w->ammunition);
}
