/* The weapon registry at work (registry.c): the weapons and entities added to the entity data each time the game loads
 * it, their network types, the armory's list, the hooks that run these, and the unit copies' destructible entries.
 * Included by hellforge_modloader.c right after registry.c. */

/* One load of the entity data as RegisterWeapons fills it: HellForge's copies of the loadout table and of the short ids,
 * and how far they are filled. */
typedef struct WeaponLoad
{
    uint8_t* state;
    uint8_t* map;
    uint8_t* table;   /* the loadout table's copy (its header), */
    uint8_t* index;   /* its index */
    uint8_t* records; /* and its records */
    uint32_t recordCount;
    uint8_t* newList; /* the short ids' copy (its header) */
    uint32_t listCount, lastShort, appended;
} WeaponLoad;

/* HellForge's copy of the loadout table: the same index, room for the new records (the vanilla record -> row array after
 * the records is only read by the armory builder, which HellForge replaces). Short ids: appended after the largest
 * vanilla one, in the entities' order (WeaponOrder), the same on every machine. 0 (noted) out of memory. */
static int CopyLoadoutAndList(WeaponLoad* L, const uint8_t* loadout, const uint8_t* list)
{
    size_t indexBytes = (size_t)REG_LOADOUT_SLOTS * 16;
    size_t copyBytes = 32 + indexBytes + (size_t)REG_LOADOUT_RECORDS * REG_LOADOUT_STRIDE;
    size_t newBytes = copyBytes + (size_t)REG_MAX_WEAPONS * REG_LOADOUT_STRIDE;
    uint8_t* table = (uint8_t*)VirtualAlloc(NULL, newBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (table == NULL) { Note("registry: out of memory"); return 0; }
    memcpy(table, loadout - 32, copyBytes);
    L->table = table;
    L->index = table + 32;
    L->records = L->index + indexBytes;
    L->recordCount = REG_LOADOUT_RECORDS;
    uint32_t listBytes = 0;
    memcpy(&listBytes, list - 12, 4);
    L->listCount = listBytes / 8;
    L->lastShort = 0;
    if (L->listCount > 0) memcpy(&L->lastShort, list + 8 * (L->listCount - 1), 4);
    L->newList = (uint8_t*)VirtualAlloc(NULL, 32 + (size_t)(L->listCount + REG_MAX_WEAPONS) * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (L->newList == NULL) { VirtualFree(table, 0, MEM_RELEASE); Note("registry: out of memory"); return 0; }
    memcpy(L->newList, list - 32, 32 + (size_t)L->listCount * 8);
    L->appended = 0;
    return 1;
}

/* The base's loadout record (an entity of add_entity names its base itself: 0), or below 0 when it has none or the
 * weapon's id is taken (logged). */
static int WeaponBaseRecord(const WeaponLoad* L, const Weapon* w)
{
    if (w->plain) return 0;
    int baseRecord = -1;
    for (uint32_t r = 0; r < L->recordCount; r++)
    {
        uint32_t id;
        memcpy(&id, L->records + (size_t)r * REG_LOADOUT_STRIDE, 4);
        if (id == w->id) { ApiLog(w->mod, "%s: loadout id %08x is taken; not registered", w->name, w->id); return -2; }
        if (id == w->base && baseRecord == -1) baseRecord = (int)r;
    }
    if (baseRecord == -1) ApiLog(w->mod, "%s: no loadout item %08x to copy; not registered", w->name, w->base);
    return baseRecord;
}

/* Whether HellForge understands every component of the base (logged when not): checked first, so a weapon is added
 * completely or not at all. */
static int ComponentsUnderstood(uint8_t* state, const Weapon* w, uint64_t baseEntity, const uint16_t* components, uint64_t componentCount)
{
    for (uint64_t k = 0; k < componentCount; k++)
    {
        uint32_t c = components[k];
        if (c == REG_LOADOUT) continue;
        uint8_t* t = StatePointer(state, REG_TABLES + 8 * c);
        if (t == NULL) continue; /* a component without data */
        uint32_t slots = SlotsOf(c);
        if (slots == 0 || FindRow(t, slots, 16, baseEntity) == NULL || FindRow(t, slots, 16, w->entity) != NULL)
        {
            ApiLog(w->mod, "%s: component %u of the base is not understood (slots %u); not registered", w->name, c, slots);
            return 0;
        }
    }
    return 1;
}

/* Shared component rows. */
static void ShareComponentRows(uint8_t* state, const Weapon* w, uint64_t baseEntity, const uint16_t* components, uint64_t componentCount)
{
    for (uint64_t k = 0; k < componentCount; k++)
    {
        uint32_t c = components[k];
        uint8_t* t = c == REG_LOADOUT ? NULL : StatePointer(state, REG_TABLES + 8 * c);
        if (t == NULL) continue;
        uint32_t slots = SlotsOf(c);
        uint8_t* row = FindRow(t, slots, 16, baseEntity);
        uint8_t* slot = FreeRow(t, slots, 16, w->entity);
        uint8_t entry[16];
        memcpy(entry, &w->entity, 8);
        memcpy(entry + 8, row + 8, 4);
        memset(entry + 12, 0, 4);
        ApiWrite((uintptr_t)slot, entry, 16);
    }
}

/* An entity copy of a weapon (an EAT or a targeting remote a mod's hellpod rack delivers): a loadout record of its
 * own, its base's under the copy's id (its entity's top 32 bits), so the game resolves the item it picks up to the
 * copy (the armory object maps a loadout id to its one entity): with its base's shared record, a picked-up silo
 * remote copy never reached the player's hands (2026-10-18). */
static void PlainLoadoutRecord(WeaponLoad* L, const Weapon* w, uint64_t baseEntity)
{
    uint8_t* baseLoadout = FindRow(L->index, REG_LOADOUT_SLOTS, 16, baseEntity);
    uint8_t* own = baseLoadout != NULL ? FreeRow(L->index, REG_LOADOUT_SLOTS, 16, w->entity) : NULL;
    int taken = 0;
    for (uint32_t r = 0; r < L->recordCount && own != NULL; r++)
    {
        uint32_t id;
        memcpy(&id, L->records + (size_t)r * REG_LOADOUT_STRIDE, 4);
        taken |= id == w->id;
    }
    if (own != NULL && !taken && L->recordCount < REG_LOADOUT_RECORDS + REG_MAX_WEAPONS)
    {
        uint32_t baseLoadoutRecord, mine = L->recordCount++;
        memcpy(&baseLoadoutRecord, baseLoadout + 8, 4);
        memcpy(L->records + (size_t)mine * REG_LOADOUT_STRIDE, L->records + (size_t)baseLoadoutRecord * REG_LOADOUT_STRIDE, REG_LOADOUT_STRIDE);
        memcpy(L->records + (size_t)mine * REG_LOADOUT_STRIDE, &w->id, 4);
        memcpy(own, &w->entity, 8);
        memcpy(own + 8, &mine, 4);
        memset(own + 12, 0, 4);
        ApiLog(w->mod, "%s: a loadout record of its own (%u, id %08x; its base's is %u)", w->name, mine, w->id, baseLoadoutRecord);
    }
    else if (own != NULL) ApiLog(w->mod, "%s: no loadout record of its own (%s); the game may not hand it to a player", w->name,
        taken ? "its id is taken" : "no room");
}

/* Its own unit record: the base's, naming the weapon's unit (an entity of add_entity: only when given one). */
static void OwnUnitRecord(uint8_t* state, const Weapon* w, uint64_t baseEntity)
{
    uint8_t* unit = OwnRecord(state, REG_UNIT, REG_UNIT_STRIDE, baseEntity, w->entity);
    uint64_t model = w->plain ? w->unit : w->entity;
    if (unit != NULL)
    {
        uint64_t vanilla;
        memcpy(&vanilla, unit, 8);
        memcpy(unit, &model, 8);
        ApiLog(w->mod, "%s: its own unit record (was %016llx, now %016llx)", w->name, (unsigned long long)vanilla, (unsigned long long)model);
        RememberUnitCopy(w->mod, w->name, vanilla, model);
    }
    else ApiLog(w->mod, "%s: could not give it its own unit record; it uses the base's model", w->name);
}

/* Its own customization record, naming its own mastery package; NULL when it has none. */
static uint8_t* OwnCustomization(uint8_t* state, const Weapon* w, uint64_t baseEntity)
{
    uint8_t* customizationTable = StatePointer(state, REG_TABLES + 8 * REG_CUSTOMIZATION);
    if (w->plain || customizationTable == NULL || FindRow(customizationTable, SlotsOf(REG_CUSTOMIZATION), 16, baseEntity) == NULL) return NULL;
    uint8_t* customization = OwnRecord(state, REG_CUSTOMIZATION, REG_CUSTOMIZATION_STRIDE, baseEntity, w->entity);
    uint32_t package = 0;
    if (customization != NULL) memcpy(&package, customization + REG_MASTERY_PACKAGE_AT, 4);
    if (customization != NULL && package != 0)
    {
        uint32_t own = Rehome(package, w->base, w->id);
        memcpy(customization + REG_MASTERY_PACKAGE_AT, &own, 4);
        ApiLog(w->mod, "%s: its own customization record (mastery package %u, the base's %u)", w->name, own, package);
    }
    else if (customization == NULL) ApiLog(w->mod, "%s: could not give it its own customization record; customizing it may crash", w->name);
    return customization;
}

/* Its own ProjectileWeapon record, for its own projectile or sounds; NULL when it needs none or its base has none. */
static uint8_t* OwnFireRecord(uint8_t* state, const Weapon* w, uint64_t baseEntity)
{
    if (w->projectile == 0 && w->soundFrom == 0) return NULL;
    uint8_t* weaponTable = StatePointer(state, REG_TABLES + 8 * REG_PROJECTILE_WEAPON);
    uint8_t* fireRecord = weaponTable != NULL && FindRow(weaponTable, SlotsOf(REG_PROJECTILE_WEAPON), 16, baseEntity) != NULL
        ? OwnRecord(state, REG_PROJECTILE_WEAPON, REG_PROJECTILE_WEAPON_STRIDE, baseEntity, w->entity) : NULL;
    if (fireRecord == NULL) ApiLog(w->mod, "%s: its base fires no projectiles; its projectile and sounds stay the base's", w->name);
    return fireRecord;
}

/* The ProjectileWeapon record of the entity that owns loadout item `item`, or NULL. */
static const uint8_t* FireRecordOfItem(const WeaponLoad* L, uint32_t item)
{
    const uint8_t* donor = NULL;
    for (uint32_t r = 0; r < L->recordCount && donor == NULL; r++)
    {
        uint32_t id;
        memcpy(&id, L->records + (size_t)r * REG_LOADOUT_STRIDE, 4);
        if (id != item) continue;
        uint64_t donorEntity = LoadoutOwner(L->index, r);
        uint8_t* weaponTable = StatePointer(L->state, REG_TABLES + 8 * REG_PROJECTILE_WEAPON);
        uint8_t* donorRow = donorEntity && weaponTable ? FindRow(weaponTable, SlotsOf(REG_PROJECTILE_WEAPON), 16, donorEntity) : NULL;
        if (donorRow != NULL)
        {
            uint32_t donorRecord;
            memcpy(&donorRecord, donorRow + 8, 4);
            donor = weaponTable + (size_t)SlotsOf(REG_PROJECTILE_WEAPON) * 16 + (size_t)donorRecord * REG_PROJECTILE_WEAPON_STRIDE;
        }
    }
    return donor;
}

/* Its sounds: another weapon's firing-sound members. */
static void TakeSounds(const WeaponLoad* L, const Weapon* w, uint8_t* fireRecord)
{
    const uint8_t* donor = FireRecordOfItem(L, w->soundFrom);
    if (donor == NULL)
    {
        ApiLog(w->mod, "%s: loadout item %08x fires no projectiles; its sounds stay the base's", w->name, w->soundFrom);
        return;
    }
    static const uint16_t blocks[4][2] = {{237, 1}, {240, 44}, {528, 12}, {556, 1}};
    for (int k = 0; k < 4; k++) memcpy(fireRecord + blocks[k][0], donor + blocks[k][0], blocks[k][1]);
    uint32_t single;
    memcpy(&single, fireRecord + 260, 4);
    ApiLog(w->mod, "%s: makes the firing sounds of loadout item %08x (shot event %08x)", w->name, w->soundFrom, single);
}

/* The customization record's default pairs without the ammunition (slot 6): how many it named. */
static int RemoveDefaultAmmunition(uint8_t* customization)
{
    uint32_t pairs[REG_DEFAULT_PAIRS * 2], kept = 0;
    int removed = 0;
    memcpy(pairs, customization, sizeof pairs);
    for (int k = 0; k < REG_DEFAULT_PAIRS; k++)
    {
        if (pairs[2 * k] == REG_AMMUNITION_SLOT) { removed++; continue; }
        memcpy(customization + 8 * kept++, pairs + 2 * k, 8);
    }
    memset(customization + 8 * kept, 0, 8 * (REG_DEFAULT_PAIRS - kept));
    return removed;
}

/* Its own projectile: its own ProjectileWeapon record names it, and no default ammunition names another. */
static void OwnProjectile(Weapon* w, uint8_t* record, uint8_t* customization)
{
    memcpy(&w->baseProjectile, record, 4);
    w->projectileRecord = record;
    if (ProjectileUsable(w->projectile)) memcpy(record, &w->projectile, 4);
    int removed = customization != NULL ? RemoveDefaultAmmunition(customization) : 0;
    ApiLog(w->mod, "%s: fires projectile %u (its base fires %u)%s", w->name, w->projectile, w->baseProjectile,
        removed ? "; its default ammunition is removed" : "");
}

/* Its own encyclopedia record when it has text of its own. */
static void OwnTextRecord(uint8_t* state, const Weapon* w, uint64_t baseEntity)
{
    uint8_t* text = OwnRecord(state, REG_TEXT_COMPONENT, REG_TEXT_STRIDE, baseEntity, w->entity);
    static const uint32_t offsets[4] = {0x00, 0x08, 0x14, 0x24};
    if (text != NULL) { for (int k = 0; k < 4; k++) if (w->text[k]) memcpy(text + offsets[k], &w->text[k], 4); }
    else ApiLog(w->mod, "%s: could not give it its own name record; it shows the base's name", w->name);
}

/* Another entity's model: its records of every component tied to the model (generated_entities.dl_bin: the
 * transport and extraction Pelicans differ in these by the names of their models' nodes and actors). */
static void TakeModel(uint8_t* state, const Weapon* w, uint64_t baseEntity)
{
    static const uint16_t model[][2] = {{88, 136}, {28, 32}, {299, 352}, {298, 152}, {223, 1544}, {104, 3912},
        {91, 3704}, {159, 4836}, {225, 456}};
    int taken = 0, missing = 0;
    for (size_t k = 0; k < sizeof model / sizeof model[0]; k++)
    {
        uint32_t c = model[k][0], stride = model[k][1], slots = SlotsOf(c);
        uint8_t* t = StatePointer(state, REG_TABLES + 8 * c);
        if (t == NULL || slots == 0 || FindRow(t, slots, 16, baseEntity) == NULL) continue; /* the base has none */
        if (FindRow(t, slots, 16, w->modelFrom) == NULL) { missing++; continue; }
        uint8_t* own = OwnRecord(state, c, stride, baseEntity, w->entity);
        t = StatePointer(state, REG_TABLES + 8 * c); /* now HellForge's copy */
        uint8_t* donorRow = FindRow(t, slots, 16, w->modelFrom);
        if (own == NULL || donorRow == NULL) { missing++; continue; }
        uint32_t donor;
        memcpy(&donor, donorRow + 8, 4);
        memcpy(own, t + (size_t)slots * 16 + (size_t)donor * stride, stride);
        taken++;
    }
    ApiLog(w->mod, "%s: takes the model of %016llx (%d record(s)%s)", w->name, (unsigned long long)w->modelFrom, taken,
        missing ? "; some it lacks stay its base's" : "");
}

/* Its own behaviour: its own behaviour record naming it, when HellForge could install it. */
static void OwnBehaviour(uint8_t* state, const Weapon* w, uint64_t baseEntity)
{
    uint8_t* behaviour = OwnRecord(state, REG_BEHAVIOUR, REG_BEHAVIOUR_STRIDE, baseEntity, w->entity);
    uint32_t baseBehaviour = 0;
    if (behaviour != NULL) memcpy(&baseBehaviour, behaviour, 4);
    if (behaviour == NULL) ApiLog(w->mod, "%s: its base has no behaviour record; it keeps its base's behaviour", w->name);
    else if (!BehaviourUsable(w->behaviour)) ApiLog(w->mod, "%s: behaviour %u is not installed; it runs its base's (%u)", w->name, w->behaviour, baseBehaviour);
    else
    {
        memcpy(behaviour, &w->behaviour, 4);
        ApiLog(w->mod, "%s: runs behaviour %u (its base runs %u)", w->name, w->behaviour, baseBehaviour);
    }
}

/* Records of its own the mod asked for. */
static void OwnNamedRecords(uint8_t* state, const Weapon* w, uint64_t baseEntity)
{
    for (int k = 0; k < w->ownCount; k++)
    {
        if (OwnRecord(state, w->own[k], w->ownStride[k], baseEntity, w->entity) != NULL) ApiLog(w->mod, "%s: its own record of component %u", w->name, w->own[k]);
        else ApiLog(w->mod, "%s: could not give it its own record of component %u (does its base have one?); it shares the base's", w->name, w->own[k]);
    }
}

/* A record of its own in every component (HfEntity.own_all): nothing it shares changes with its base. */
static void OwnAllRecords(uint8_t* state, const Weapon* w, uint64_t baseEntity, const uint16_t* components, uint64_t componentCount)
{
    int own = 0, shared = 0;
    char sharedList[160] = "";
    for (uint64_t k = 0; k < componentCount; k++)
    {
        uint32_t c = components[k];
        if (c == REG_LOADOUT || StatePointer(state, REG_TABLES + 8 * c) == NULL) continue;
        uint32_t stride = ComponentStride(state, c);
        if (stride != 0 && OwnRecord(state, c, stride, baseEntity, w->entity) != NULL) { own++; continue; }
        size_t at = strlen(sharedList);
        if (at + 8 < sizeof sharedList) snprintf(sharedList + at, sizeof sharedList - at, "%s%u", shared ? ", " : "", c);
        shared++;
    }
    if (shared) ApiLog(w->mod, "%s: records of its own in %d component(s); it still shares %d with its base (record size unknown: %s)",
        w->name, own, shared, sharedList);
    else ApiLog(w->mod, "%s: records of its own in all %d component(s); it shares none with its base", w->name, own);
}

/* What it has of its own beside its base's shared rows, in this order. */
static void GiveOwnRecords(WeaponLoad* L, Weapon* w, uint64_t baseEntity, const uint16_t* components, uint64_t componentCount)
{
    uint8_t* state = L->state;
    if (w->plain) PlainLoadoutRecord(L, w, baseEntity);
    if (!w->plain || w->unit != 0) OwnUnitRecord(state, w, baseEntity);
    uint8_t* customization = OwnCustomization(state, w, baseEntity);
    uint8_t* fireRecord = OwnFireRecord(state, w, baseEntity);
    if (fireRecord != NULL && w->soundFrom != 0) TakeSounds(L, w, fireRecord);
    if (fireRecord != NULL && w->projectile != 0) OwnProjectile(w, fireRecord, customization);
    if (w->text[0] != 0 || w->text[2] != 0 || w->text[3] != 0) OwnTextRecord(state, w, baseEntity);
    if (w->modelFrom != 0) TakeModel(state, w, baseEntity);
    if (w->behaviour != 0) OwnBehaviour(state, w, baseEntity);
    OwnNamedRecords(state, w, baseEntity);
    if (w->ownAll) OwnAllRecords(state, w, baseEntity, components, componentCount);
}

/* Its own loadout record: the base's with the new id. */
static uint32_t OwnLoadoutRecord(WeaponLoad* L, const Weapon* w, int baseRecord, uint8_t* loadoutRow)
{
    uint32_t record = L->recordCount++;
    memcpy(L->records + (size_t)record * REG_LOADOUT_STRIDE, L->records + (size_t)baseRecord * REG_LOADOUT_STRIDE, REG_LOADOUT_STRIDE);
    memcpy(L->records + (size_t)record * REG_LOADOUT_STRIDE, &w->id, 4);
    memcpy(loadoutRow, &w->entity, 8);
    memcpy(loadoutRow + 8, &record, 4);
    memset(loadoutRow + 12, 0, 4);
    return record;
}

/* Its short id and map row (naming its base's components). */
static uint32_t AppendEntity(WeaponLoad* L, Weapon* w, const uint8_t* baseRow, uint8_t* mapRow, uint16_t* components, uint64_t componentCount)
{
    uint32_t shortId = L->lastShort + 1 + L->appended;
    uint32_t mapIndex = (uint32_t)((mapRow - L->map) / 32);
    memcpy(L->newList + 32 + (size_t)(L->listCount + L->appended) * 8, &shortId, 4);
    memcpy(L->newList + 32 + (size_t)(L->listCount + L->appended) * 8 + 4, &mapIndex, 4);
    L->appended++;
    memcpy(&w->baseShortId, baseRow + 24, 4);
    w->shortId = shortId;
    uint8_t row[32] = {0};
    memcpy(row, &w->entity, 8);
    memcpy(row + 8, &components, 8);
    memcpy(row + 16, &componentCount, 8);
    memcpy(row + 24, &shortId, 4);
    ApiWrite((uintptr_t)mapRow, row, 32);
    return shortId;
}

/* Adds one pending weapon or entity: a map row of its own over its base's components, sharing their records but the
 * ones it owns. */
static void RegisterWeapon(WeaponLoad* L, Weapon* w)
{
    w->state = -1;
    /* The base: its loadout record and the entity that owns it; an entity of add_entity names its base itself. */
    int baseRecord = WeaponBaseRecord(L, w);
    if (baseRecord < 0) return;
    uint64_t baseEntity = w->plain ? w->baseEntity : LoadoutOwner(L->index, (uint32_t)baseRecord);
    uint8_t* baseRow = baseEntity ? FindRow(L->map, REG_MAP_SLOTS, 32, baseEntity) : NULL;
    if (baseRow == NULL) { ApiLog(w->mod, "%s: the base's entity was not found; not registered", w->name); return; }
    if (FindRow(L->map, REG_MAP_SLOTS, 32, w->entity) != NULL) { ApiLog(w->mod, "%s: entity %016llx exists already; not registered", w->name, (unsigned long long)w->entity); return; }
    uint16_t* components;
    uint64_t componentCount;
    memcpy(&components, baseRow + 8, 8);
    memcpy(&componentCount, baseRow + 16, 8);
    w->copies = baseEntity;
    if (!ComponentsUnderstood(L->state, w, baseEntity, components, componentCount)) return;
    uint8_t* mapRow = FreeRow(L->map, REG_MAP_SLOTS, 32, w->entity);
    uint8_t* loadoutRow = !w->plain ? FreeRow(L->index, REG_LOADOUT_SLOTS, 16, w->entity) : NULL;
    if (mapRow == NULL || (!w->plain && loadoutRow == NULL) || L->recordCount >= REG_LOADOUT_RECORDS + REG_MAX_WEAPONS)
    {
        ApiLog(w->mod, "%s: no free row; not registered", w->name);
        return;
    }
    ShareComponentRows(L->state, w, baseEntity, components, componentCount);
    GiveOwnRecords(L, w, baseEntity, components, componentCount);
    uint32_t record = w->plain ? 0 : OwnLoadoutRecord(L, w, baseRecord, loadoutRow);
    uint32_t shortId = AppendEntity(L, w, baseRow, mapRow, components, componentCount);
    w->state = 1;
    if (w->plain)
        ApiLog(w->mod, "%s registered: entity %016llx, short id %u, %llu components", w->name, (unsigned long long)w->entity,
            shortId, (unsigned long long)componentCount);
    else
        ApiLog(w->mod, "%s registered: entity %016llx, loadout record %u (id %08x), short id %u, %llu shared components",
            w->name, (unsigned long long)w->entity, record, w->id, shortId, (unsigned long long)componentCount - 1);
}

/* Adds every pending weapon to the loaded entity data. Runs on the game's thread right after the entity loader. */
static void RegisterWeapons(void)
{
    uint8_t* state = EntityState();
    uint8_t* map = StatePointer(state, REG_MAP);
    uint8_t* list = StatePointer(state, REG_LIST);
    uint32_t vanillaRecords;
    uint8_t* loadout = LoadoutTable(state, &vanillaRecords);
    if (map == NULL || list == NULL || loadout == NULL) { Note("registry: the entity data is not loaded yet"); return; }
    if (loadout == g_loadoutTable) return; /* already done for this load */
    int pending = 0;
    for (int i = 0; i < g_weaponCount; i++) if (g_weapons[i].state == 0) pending++;
    if (pending == 0) return;
    WeaponLoad L = {state, map};
    if (!CopyLoadoutAndList(&L, loadout, list)) return;
    int order[REG_MAX_WEAPONS];
    WeaponOrder(order);
    for (int k = 0; k < g_weaponCount; k++)
        if (g_weapons[order[k]].state == 0) RegisterWeapon(&L, &g_weapons[order[k]]);
    if (L.appended == 0) { VirtualFree(L.table, 0, MEM_RELEASE); VirtualFree(L.newList, 0, MEM_RELEASE); return; }
    uint32_t newListBytes = (L.listCount + L.appended) * 8;
    memcpy(L.newList + 32 - 12, &newListBytes, 4);
    g_loadoutTable = L.index;
    g_loadoutRecords = L.recordCount;
    SetStatePointer(state, REG_TABLES + 8 * REG_LOADOUT, L.index);
    SetStatePointer(state, REG_LIST, L.newList + 32);
    Note("registry: %u weapon(s) added (loadout records %u, entities %u)", L.appended, L.recordCount, L.listCount + L.appended);
}

typedef void* (*EngineAllocateFn)(void* allocator, size_t size, uint32_t align);
static const uint8_t kAllocatePin[21] = {0x48, 0x83, 0xec, 0x38, 0x48, 0x8b, 0x01, 0x45, 0x8b, 0xc8, 0x4c, 0x8b, 0xc2, 0x48,
    0x8d, 0x54, 0x24, 0x20, 0xff, 0x50, 0x30};

/* A block from the allocator of the engine array whose header is at `array` ({u32 count, u32 capacity, data, allocator}),
 * so the engine can free it; NULL when that allocator is not the reviewed one. */
static uint8_t* EngineAllocate(uintptr_t array, size_t size)
{
    uintptr_t allocator = 0, vtable = 0, allocate = 0;
    uint8_t code[sizeof kAllocatePin];
    if (SafeRead(array + 0x10, &allocator, 8) || allocator == 0 || SafeRead(allocator, &vtable, 8) || vtable == 0 ||
        SafeRead(vtable + 7 * 8, &allocate, 8) || allocate == 0 || SafeRead(allocate, code, sizeof code) || memcmp(code, kAllocatePin, sizeof code) != 0)
        return NULL;
    uint8_t* block = (uint8_t*)((EngineAllocateFn)allocate)((void*)allocator, size, 16);
    if (block != NULL) memset(block, 0, size);
    return block;
}

/* A network configuration entry copied for an added type. An 80-byte entry owns two engine arrays {u32 count, u32
 * capacity, data, allocator} of u32s, its fields' type rows (+0x18) and their keys (+0x30, searched by exe 0x34B830),
 * which the engine frees with the entry when the game quits (constructor exe 0x175C40, destructor 0x175B60, freeing
 * only arrays with a capacity). A byte copy shared its base's, so the quit freed them twice: the second free left a
 * worker thread's allocator span counted empty while that thread's scratch allocator was still in it, and the quit
 * then read the scratch allocator after its span was given back (helldivers2.exe+0x5B7A43, 2026-10-25). The copy gets
 * arrays of its own from the same allocator; one it cannot have stays its base's with capacity 0, so it is never freed
 * through the copy. Returns how many it could not have. */
static int CopyNetworkEntry(uint8_t* to, const uint8_t* from)
{
    static const uint32_t arrays[2] = {0x18, 0x30};
    int shared = 0;
    memcpy(to, from, 80);
    for (int i = 0; i < 2; i++)
    {
        uint8_t* array = to + arrays[i];
        uint32_t count;
        uint8_t *data, *own = NULL;
        memcpy(&count, array, 4);
        memcpy(&data, array + 8, 8);
        if (count != 0 && data != NULL && (own = EngineAllocate((uintptr_t)array, (size_t)count * 4)) != NULL)
            memcpy(own, data, (size_t)count * 4);
        uint8_t* kept = own != NULL || count == 0 ? own : data;
        uint32_t capacity = own != NULL ? count : 0;
        if (kept != NULL && kept == data) shared++;
        memcpy(array + 4, &capacity, 4);
        memcpy(array + 8, &kept, 8);
    }
    return shared;
}

/* The engine's network configuration: its entries (+0x70), own indices (+0x28) and values (+0x40), one per type. */
typedef struct NetworkConfig { uintptr_t config; uint32_t count; uint8_t *entries, *own, *values; } NetworkConfig;

/* 1 once the configuration is loaded and as reviewed (its three arrays the same length). */
static int ReadNetworkConfig(NetworkConfig* n)
{
    uintptr_t base = ApiModuleBase(g_networkModule, NULL), holder = 0, config = 0;
    if (base == 0 || SafeRead(base + g_networkRva, &holder, 8) || holder == 0 || SafeRead(holder, &config, 8) || config == 0) return 0;
    uint32_t count = 0, ownCount = 0, valueCount = 0;
    uint8_t *entries = NULL, *own = NULL, *values = NULL;
    if (SafeRead(config + 0x70, &count, 4) || SafeRead(config + 0x78, &entries, 8) || SafeRead(config + 0x28, &ownCount, 4) ||
        SafeRead(config + 0x30, &own, 8) || SafeRead(config + 0x40, &valueCount, 4) || SafeRead(config + 0x48, &values, 8)) return 0;
    if (count == 0 || entries == NULL || ownCount != count || valueCount != count || own == NULL || values == NULL) return 0;
    *n = (NetworkConfig){config, count, entries, own, values};
    return 1;
}

/* The base's entry: binary search by short id, as the engine does; -1 if none. */
static int NetworkEntryOf(const uint8_t* entries, uint32_t count, uint32_t shortId)
{
    uint32_t lo = 0, hi = count;
    while (lo < hi)
    {
        uint32_t mid = (lo + hi) / 2, key;
        memcpy(&key, entries + (size_t)mid * 80, 4);
        if (key < shortId) lo = mid + 1; else if (key > shortId) hi = mid; else return (int)mid;
    }
    return -1;
}

/* The registered weapons' types appended to the configuration's three arrays (larger copies from the engine's
 * allocator), in short id order. */
static void AddNetworkTypes(const NetworkConfig* n)
{
    uint32_t count = n->count;
    /* A type index travels in 11 bits (the configuration's +0xC, and field type 431): 2,048 types at most. */
    if (count + REG_MAX_WEAPONS > 2048u)
    {
        Note("registry: the network configuration has %u types; with %u more they would not fit the 11-bit type index; none added", count, REG_MAX_WEAPONS);
        return;
    }
    uint32_t added = 0, lastKey;
    memcpy(&lastKey, n->entries + (size_t)(count - 1) * 80, 4);
    uint8_t* newEntries = EngineAllocate(n->config + 0x70, (size_t)(count + REG_MAX_WEAPONS) * 80);
    uint8_t* newOwn = EngineAllocate(n->config + 0x28, (size_t)(count + REG_MAX_WEAPONS) * 4);
    uint8_t* newValues = EngineAllocate(n->config + 0x40, (size_t)(count + REG_MAX_WEAPONS));
    if (newEntries == NULL || newOwn == NULL || newValues == NULL)
    {
        Note("registry: the engine's allocator is not the reviewed one; the weapons have no network types (equipping them crashes)");
        return;
    }
    memcpy(newEntries, n->entries, (size_t)count * 80);
    memcpy(newOwn, n->own, (size_t)count * 4);
    memcpy(newValues, n->values, count);
    int order[REG_MAX_WEAPONS];
    WeaponOrder(order);
    for (int k = 0; k < g_weaponCount; k++)
    {
        Weapon* w = &g_weapons[order[k]];
        if (w->state != 1) continue;
        int found = NetworkEntryOf(n->entries, count, w->baseShortId);
        if (found < 0 || w->shortId <= lastKey) { ApiLog(w->mod, "%s: no network type to copy (base short id %u); joining games with it may fail", w->name, w->baseShortId); continue; }
        uint32_t index = count + added++;
        if (CopyNetworkEntry(newEntries + (size_t)index * 80, n->entries + (size_t)found * 80))
            ApiLog(w->mod, "%s: its network type keeps its base's field lists (no memory for its own)", w->name);
        memcpy(newEntries + (size_t)index * 80, &w->shortId, 4);
        memcpy(newOwn + (size_t)index * 4, &index, 4);
        newValues[index] = n->values[found];
        lastKey = w->shortId;
        g_networkTypes[g_networkTypeCount++] = (NetworkType){index, (uint32_t)found, w->entity, w->copies, w->name, w->mod};
        ApiLog(w->mod, "%s: network entity type %u (a copy of type %d)", w->name, index, found);
    }
    if (added == 0) return;
    g_networkVanillaTypes = count;
    uint32_t total = count + added;
    uint32_t pair[2] = {total, total};
    ApiWrite(n->config + 0x30, &newOwn, 8);
    ApiWrite(n->config + 0x48, &newValues, 8);
    ApiWrite(n->config + 0x78, &newEntries, 8);
    ApiWrite(n->config + 0x28, pair, 8);
    ApiWrite(n->config + 0x40, pair, 8);
    ApiWrite(n->config + 0x70, pair, 8);
    Note("registry: %u network entity type(s) added (%u in all)", added, total);
}

/* Appends the registered weapons' entity types to the engine's network configuration, once both are loaded, in short id
 * order (WeaponOrder). */
static void RegisterNetworkTypes(void)
{
    if (g_networkTypesAdded || g_loadoutTable == NULL) return;
    NetworkConfig n;
    if (!ReadNetworkConfig(&n)) return;
    /* claimed once (the game thread and the watch loop both call this); 2 once g_networkTypes is complete, which is when
     * netapply.c reads it */
    if (InterlockedCompareExchange(&g_networkTypesAdded, 1, 0) != 0) return;
    AddNetworkTypes(&n);
    InterlockedExchange(&g_networkTypesAdded, 2);
}

/* The armory's list, built from the loadout table in use into HellForge's larger object. */
static void BuildArmory(void)
{
    uint8_t* state = EntityState();
    uint32_t count;
    uint8_t* table = LoadoutTable(state, &count);
    size_t bytes = (size_t)(REG_ARMORY_FIRST + REG_LOADOUT_RECORDS + REG_MAX_WEAPONS + 8) * 32;
    if (g_armory == NULL) g_armory = (uint8_t*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_armory == NULL || table == NULL) return;
    memset(g_armory, 0, bytes);
    uint8_t* records = table + (size_t)REG_LOADOUT_SLOTS * 16;
    uint32_t counts[REG_ARMORY_TYPES] = {0}, starts[REG_ARMORY_TYPES], placed[REG_ARMORY_TYPES] = {0};
    for (uint32_t r = 0; r < count; r++)
    {
        uint32_t type;
        memcpy(&type, records + (size_t)r * REG_LOADOUT_STRIDE + 4, 4);
        if (type < REG_ARMORY_TYPES) counts[type]++;
    }
    uint32_t next = REG_ARMORY_FIRST;
    for (int t = 0; t < REG_ARMORY_TYPES; t++) { starts[t] = next; next += counts[t]; }
    for (uint32_t r = 0; r < count; r++)
    {
        const uint8_t* rec = records + (size_t)r * REG_LOADOUT_STRIDE;
        uint32_t type;
        memcpy(&type, rec + 4, 4);
        if (type >= REG_ARMORY_TYPES) continue;
        uint8_t* entry = g_armory + (size_t)(starts[type] + placed[type]++) * 32;
        uint64_t owner = LoadoutOwner(table, r);
        memcpy(entry, rec, 4);          /* loadout id */
        memcpy(entry + 8, &owner, 8);   /* entity */
        memcpy(entry + 16, &type, 4);
        memcpy(entry + 20, rec + 8, 4);
    }
    memcpy(g_armory + 0x1B24, starts, sizeof starts);
    memcpy(g_armory + 0x1B40, counts, sizeof counts);
    ApiWrite(g_game + REG_ARMORY_GLOBAL, &g_armory, 8);
}

typedef uint8_t* (*EncyclopediaFn)(uint64_t entity);
typedef uint8_t* (*ArmoryEntryFn)(uint32_t id);
#define REG_ENCYCLOPEDIA_LOOKUP 0x4F8A50u /* component 76 record of an entity */
#define REG_ARMORY_LOOKUP 0x11E7C60u      /* armory entry of a loadout id (walks the per-type ranges) */

/* Asks the game's own lookups about every registered weapon and logs the answers (the armory's name path). */
static void SelfCheck(const char* when)
{
    for (int i = 0; i < g_weaponCount; i++)
    {
        Weapon* w = &g_weapons[i];
        if (w->state != 1 || w->plain) continue;
        uint8_t* record = ((EncyclopediaFn)(void*)(g_game + REG_ENCYCLOPEDIA_LOOKUP))(w->entity);
        uint32_t ids[4] = {0};
        if (record != NULL) { memcpy(&ids[0], record, 4); memcpy(&ids[1], record + 8, 4); memcpy(&ids[2], record + 0x14, 4); memcpy(&ids[3], record + 0x24, 4); }
        uint8_t* entry = g_armory != NULL ? ((ArmoryEntryFn)(void*)(g_game + REG_ARMORY_LOOKUP))(w->id) : NULL;
        uint64_t entryEntity = 0;
        if (entry != NULL) memcpy(&entryEntity, entry + 8, 8);
        ApiLog(w->mod, "self-check (%s): name record %p ids %08x %08x %08x %08x; armory entry %p entity %016llx; text \"%s\" \"%s\"",
            when, (void*)record, ids[0], ids[1], ids[2], ids[3], (void*)entry, (unsigned long long)entryEntity,
            ids[3] ? TextDetour(ids[3], NULL, NULL, NULL) : "-", ids[1] ? TextDetour(ids[1], NULL, NULL, NULL) : "-");
    }
}

static void EntityLoaderDetour(void* state)
{
    HookText();
    StatsBeginComponents();
    g_entityLoader(state);
    RegisterWeapons();
    StatsFinishComponents(EntityState());
    RegisterNetworkTypes();
    SelfCheck("after loading");
}

/* An armory entry's line of arsenal.tsv (snprintf's result). */
static int ArsenalLine(const uint8_t* entry, int type, char* line, size_t size)
{
    uint32_t id;
    uint64_t entity;
    memcpy(&id, entry, 4);
    memcpy(&entity, entry + 8, 8);
    uint8_t* record = entity ? ((EncyclopediaFn)(void*)(g_game + REG_ENCYCLOPEDIA_LOOKUP))(entity) : NULL;
    uint32_t designation = 0, name = 0;
    if (record != NULL) { memcpy(&name, record, 4); memcpy(&designation, record + 0x24, 4); }
    const char* d = designation ? TextDetour(designation, NULL, NULL, NULL) : "";
    const char* m = name ? TextDetour(name, NULL, NULL, NULL) : "";
    return snprintf(line, size, "%08x\t%d\t%016llx\t%s\t%s\n", id, type, (unsigned long long)entity, d ? d : "", m ? m : "");
}

/* <session>/arsenal.tsv: every armory item with its loadout id, armory type, entity and text, as the game names it (for
 * mod authors choosing a base). */
static void WriteArsenal(void)
{
    if (g_logs[0] == 0 || g_armory == NULL) return;
    HANDLE file = CreateSessionFile(L"arsenal.tsv");
    if (file == INVALID_HANDLE_VALUE) return;
    char line[512];
    DWORD written;
    int n = snprintf(line, sizeof line, "loadout_id\ttype\tentity\tdesignation\tname\n");
    WriteFile(file, line, (DWORD)n, &written, NULL);
    uint32_t starts[REG_ARMORY_TYPES], counts[REG_ARMORY_TYPES];
    memcpy(starts, g_armory + 0x1B24, sizeof starts);
    memcpy(counts, g_armory + 0x1B40, sizeof counts);
    for (int t = 0; t < REG_ARMORY_TYPES; t++)
        for (uint32_t k = 0; k < counts[t]; k++)
        {
            n = ArsenalLine(g_armory + (size_t)(starts[t] + k) * 32, t, line, sizeof line);
            if (n > 0) WriteFile(file, line, (DWORD)(n < (int)sizeof line ? n : (int)sizeof line - 1), &written, NULL);
        }
    CloseHandle(file);
}

static void ArmoryBuilderDetour(void* list)
{
    (void)list;
    HookText();
    RegisterNetworkTypes();
    BuildArmory();
    static int checked;
    if (!checked++) { SelfCheck("armory built"); WriteArsenal(); }
}

/* A 14-byte absolute jump (jmp [rip+0]; dq target). */
static int PatchJump(uintptr_t at, const void* target)
{
    uint8_t jump[14] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(jump + 6, &target, 8);
    return ApiWrite(at, jump, sizeof jump);
}

/* Hooks without MinHook, for the two pinned functions when MinHook declines: the entity loader's first 16 bytes are
 * position-independent pushes and moves (kLoaderPin), so they run from a trampoline that jumps back; the armory builder
 * is replaced, never called, so it only needs the jump. */
static int PatchHooks(void)
{
    uint8_t* trampoline = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (trampoline == NULL) return 1;
    memcpy(trampoline, kLoaderPin, 16);
    uint8_t back[14] = {0xFF, 0x25, 0, 0, 0, 0};
    uintptr_t resume = g_game + REG_ENTITY_LOADER + 16;
    memcpy(back + 6, &resume, 8);
    memcpy(trampoline + 16, back, sizeof back);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);
    g_entityLoader = (EntityLoaderFn)(void*)trampoline;
    if (PatchJump(g_game + REG_ARMORY_BUILDER, (void*)ArmoryBuilderDetour)) return 1;
    return PatchJump(g_game + REG_ENTITY_LOADER, (void*)EntityLoaderDetour);
}

/* The projectiles mods added were refused: weapons pointed at one fire their base's projectile again. */
static void RevertWeaponProjectiles(void)
{
    for (int i = 0; i < g_weaponCount; i++)
    {
        Weapon* w = &g_weapons[i];
        if (w->projectileRecord == NULL || ProjectileUsable(w->projectile)) continue;
        memcpy(w->projectileRecord, &w->baseProjectile, 4);
        ApiLog(w->mod, "%s: projectile %u was refused; it fires its base's %u", w->name, w->projectile, w->baseProjectile);
    }
}

/* Units a registered entity was given a copy of (its own unit record names a unit other than its base's). The game keys
 * some per-unit settings by the unit's name; a copy is entered in those as its base is, so it is set up as its base.
 * The destructible index (game.dll 25480438): [[game + 0x3326550]] + 0x2C60 is {+8 table, +0x10 capacity}, filled
 * from a baked resource (type 0xB8FD4D2CEDE20ED7) when it loads (0xF6EC10): 16-byte entries {u64 unit name, u64 the
 * unit's destruction settings}, probed from slot (i + 2 * name) & (capacity - 1) to the first empty name (lookup
 * 0xFC45C0). A unit missing from it is never registered for destruction (0x883200: "Could not find any destructible
 * index data"), which also sets up some of its havok actors; a sentry so left had no aim and a field around it that
 * blocked players, likely for that reason. */
#define REG_MAX_UNIT_COPIES 64
#define DESTR_MANAGER 0x3326550u
#define DESTR_INDEX 0x2C60u
static struct { uint64_t base, copy; const char* name; HfMod* mod; } g_unitCopies[REG_MAX_UNIT_COPIES];
static int g_unitCopyCount;
static uintptr_t g_destructibleTable; /* the table the copies are in, and how many */
static int g_destructibleEntered;

static void RememberUnitCopy(HfMod* mod, const char* name, uint64_t base, uint64_t copy)
{
    if (base == 0 || copy == 0 || base == copy) return;
    for (int i = 0; i < g_unitCopyCount; i++) if (g_unitCopies[i].copy == copy) return;
    if (g_unitCopyCount == REG_MAX_UNIT_COPIES) { ApiLog(mod, "%s: too many unit copies to set up as their bases", name); return; }
    g_unitCopies[g_unitCopyCount].base = base;
    g_unitCopies[g_unitCopyCount].copy = copy;
    g_unitCopies[g_unitCopyCount].name = name;
    g_unitCopies[g_unitCopyCount].mod = mod;
    g_unitCopyCount++;
}

/* The slot holding `name` (1), or the empty slot its lookup would stop at (0), or none (-1). */
static int DestructibleSlot(uintptr_t table, uint32_t capacity, uint64_t name, uintptr_t* slot)
{
    for (uint32_t i = 0; i < capacity; i++)
    {
        uintptr_t at = table + (uintptr_t)(((uint64_t)i + 2 * name) & (capacity - 1)) * 16;
        uint64_t key = 0;
        if (SafeRead(at, &key, 8)) return -1;
        if (key == name || key == 0) { *slot = at; return key == name; }
    }
    return -1;
}

/* Enters unit copy i as its base is, when its base is destructible: 1 when it was entered now. */
static int EnterUnitCopy(uintptr_t table, uint32_t capacity, int i)
{
    uintptr_t baseSlot = 0, copySlot = 0;
    uint64_t settings = 0;
    if (DestructibleSlot(table, capacity, g_unitCopies[i].base, &baseSlot) != 1 || SafeRead(baseSlot + 8, &settings, 8)) return 0; /* its base is not destructible */
    int found = DestructibleSlot(table, capacity, g_unitCopies[i].copy, &copySlot);
    if (found == 1) return 0;
    /* the settings first, then the name that makes the entry found */
    if (found != 0 || ApiWrite(copySlot + 8, &settings, 8) || ApiWrite(copySlot, &g_unitCopies[i].copy, 8))
    {
        ApiLog(g_unitCopies[i].mod, "%s: its unit %016llx could not be entered in the destructible index (the table is full)", g_unitCopies[i].name,
            (unsigned long long)g_unitCopies[i].copy);
        return 0;
    }
    ApiLog(g_unitCopies[i].mod, "%s: its unit %016llx is destructible as its base's (%016llx, settings %llu)", g_unitCopies[i].name,
        (unsigned long long)g_unitCopies[i].copy, (unsigned long long)g_unitCopies[i].base, (unsigned long long)settings);
    return 1;
}

/* Polled once the game is loaded: enters the unit copies in the destructible index once it is loaded (again if the game
 * loads it anew); a copy whose base is not in it (a weapon's unit, say) is left out, as its base is. */
static void UnitCopiesTick(uintptr_t game)
{
    if (g_unitCopyCount == 0 || game == 0 || strcmp(g_build, "25480438")) return;
    uintptr_t manager = 0, index = 0, table = 0;
    uint32_t capacity = 0;
    if (SafeRead(game + DESTR_MANAGER, &manager, 8) || manager == 0 || SafeRead(manager, &index, 8) || index == 0 ||
        SafeRead(index + DESTR_INDEX + 8, &table, 8) || table == 0 || SafeRead(index + DESTR_INDEX + 0x10, &capacity, 4)) return;
    if (table == g_destructibleTable && g_destructibleEntered == g_unitCopyCount) return;
    g_destructibleTable = table;
    g_destructibleEntered = g_unitCopyCount;
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) { Note("units: the destructible index is not the reviewed one (capacity %u); unit copies are left out", capacity); return; }
    int entered = 0;
    for (int i = 0; i < g_unitCopyCount; i++) entered += EnterUnitCopy(table, capacity, i);
    Note("units: %d unit cop%s entered in the destructible index", entered, entered == 1 ? "y" : "ies");
}

/* Whether the Watch thread should hold HF_EVENT_ENTITIES_READY back: weapons are waiting for the entity loader. */
static int RegistryPending(void)
{
    if (!g_registryActive) return 0;
    for (int i = 0; i < g_weaponCount; i++) if (g_weapons[i].state == 0) return 1;
    return 0;
}

/* At GAME_LOADED: hook the entity loader and the armory builder when weapons are registered and the build matches. */
static void StartRegistry(void)
{
    if ((g_weaponCount == 0 && !StatsHasComponents()) || g_registryHooked) return;
    g_registryHooked = 1;
    g_game = ApiModuleBase("game.dll", NULL);
    uint8_t loader[16], builder[16];
    if (g_game == 0 || HookedCode(g_game + REG_ENTITY_LOADER, loader, 16) || HookedCode(g_game + REG_ARMORY_BUILDER, builder, 16) ||
        memcmp(loader, kLoaderPin, 16) != 0 || memcmp(builder, kBuilderPin, 16) != 0)
    {
        Note("registry: game.dll is not the reviewed build; %d weapon(s) not registered", g_weaponCount);
        return;
    }
    MEMORY_BASIC_INFORMATION page;
    if (VirtualQuery((void*)(g_game + REG_ENTITY_LOADER), &page, sizeof page))
        Note("registry: entity loader page 0x%llx protect 0x%lx state 0x%lx type 0x%lx", (unsigned long long)(uintptr_t)page.BaseAddress,
            page.Protect, page.State, page.Type);
    /* through the hook chain (hooks.c): mods may hook these too. The armory builder only once the entity loader is in, so
     * the fallback (both jumps patched directly) never patches over a chain. */
    int loaderFailed = LoaderHook("weapon registry: entity loader", g_game + REG_ENTITY_LOADER, (void*)EntityLoaderDetour, (void**)&g_entityLoader);
    int armoryFailed = loaderFailed ? 1 : LoaderHook("weapon registry: armory builder", g_game + REG_ARMORY_BUILDER, (void*)ArmoryBuilderDetour, (void**)&g_armoryBuilder);
    int hooked = !loaderFailed && !armoryFailed;
    if (loaderFailed)
    {
        Note("registry: the entity loader could not be hooked; patching the two jumps directly");
        hooked = !PatchHooks();
    }
    else if (armoryFailed) Note("registry: the armory builder could not be hooked; added weapons load without their armory entries");
    Note(hooked ? "registry: hooked the entity loader and the armory builder for %d weapon(s)" : "registry: could not hook (%d weapon(s))", g_weaponCount);
    g_registryActive = hooked;
    if (hooked) HookText();
    /* Entity data that loaded before the hook still gets the weapons. */
    if (hooked && StatePointer(EntityState(), REG_MAP) != NULL)
    {
        RegisterWeapons();
        StatsApplyComponents(EntityState());
    }
}
