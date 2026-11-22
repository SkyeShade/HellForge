/* An entity copy's records of its own in every component (HfEntity.own_all), and, for a support weapon or secondary, its
 * paint package. Included by registry_load.c, after WeaponLoad. */

/* An entity of add_entity with records of its own (HfEntity.own_all) that is a support weapon or secondary: its own paint
 * package, as OwnCustomization gives an arsenal copy, and a place in "paintItems". With its base's package its cards were
 * its base's paints, which the game finds held under the base, so it could not have the customize screen. */
static void PaintCopy(uint8_t* state, const uint8_t* records, uint8_t* index, const Weapon* w, uint64_t donor)
{
    uint8_t* loadoutRow = FindRow(index, SlotsOf(REG_LOADOUT), 16, w->entity);
    if (loadoutRow == NULL || g_paintCount >= REG_MAX_PAINTED) return;
    uint32_t r, id, type, package;
    memcpy(&r, loadoutRow + 8, 4);
    memcpy(&id, records + (size_t)r * REG_LOADOUT_STRIDE, 4);
    memcpy(&type, records + (size_t)r * REG_LOADOUT_STRIDE + 4, 4);
    uint8_t* record = id == w->id && (type == 2 || type == 3) ? OwnRecord(state, REG_CUSTOMIZATION, REG_CUSTOMIZATION_STRIDE, donor, w->entity) : NULL;
    if (record == NULL) return;
    memcpy(&package, record + REG_MASTERY_PACKAGE_AT, 4);
    int painted = 0;
    for (int i = 0; i < g_paintCount && !painted; i++) painted = package == WeaponChild(REG_PAINT_PACKAGE_SEED, g_painted[i]);
    if (!painted) return;
    package = WeaponChild(REG_PAINT_PACKAGE_SEED, w->id);
    memcpy(record + REG_MASTERY_PACKAGE_AT, &package, 4);
    g_painted[g_paintCount++] = w->id;
    ApiLog(w->mod, "%s: its own paint package (%u)", w->name, package);
}

/* A record of its own in every component (HfEntity.own_all): nothing it shares changes with its base. */
static void OwnAllRecords(WeaponLoad* L, const Weapon* w, uint64_t baseEntity, const uint16_t* components, uint64_t componentCount)
{
    uint8_t* state = L->state;
    int own = 0, shared = 0;
    char sharedList[160] = "";
    for (uint64_t k = 0; k < componentCount; k++)
    {
        uint32_t c = components[k];
        if (c == REG_LOADOUT || StatePointer(state, REG_TABLES + 8 * c) == NULL) continue;
        uint32_t stride = ComponentStride(state, c);
        if (stride != 0 && OwnRecord(state, c, stride, Donor(w, c, baseEntity), w->entity) != NULL) { own++; continue; }
        size_t at = strlen(sharedList);
        if (at + 8 < sizeof sharedList) snprintf(sharedList + at, sizeof sharedList - at, "%s%u", shared ? ", " : "", c);
        shared++;
    }
    if (shared) ApiLog(w->mod, "%s: records of its own in %d component(s); it still shares %d with its base (record size unknown: %s)",
        w->name, own, shared, sharedList);
    else ApiLog(w->mod, "%s: records of its own in all %d component(s); it shares none with its base", w->name, own);
    if (w->plain) PaintCopy(state, L->records, L->index, w, Donor(w, REG_CUSTOMIZATION, baseEntity));
}
