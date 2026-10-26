/* The Lua bridge (engine 0.1.15, docs/lua-bridge.md, native/mod-sdk/hellforge_lua.h): plain C functions this
 * DLL exports for LuaJIT's ffi, so a Lua runtime in the game (HD2Runtime, loaded by Bingus Shared Loader) changes values
 * through HellForge instead of writing them itself. Included by hellforge_modloader.c after live.c. Nothing here runs
 * unless a Lua runtime calls it.
 *
 *   mods      hellforge_lua_mod("<name>") registers a Lua mod as lua.<name> (its own log, mod-lua.<name>.log), the id
 *             the editors and get_live name as a value's holder. Lua mods' live edits rank between the editors' and
 *             code mods' (fields.c).
 *   values    set, clear and get a field by its component (or settings table) name, target, offset and type: set_live's
 *             edits and checks, with any aligned value of the record allowed (RawFieldOf). A target is an entity
 *             resource, or a settings row's id; hellforge_lua_owner gives the entity a vanilla record belongs to.
 *   events    hellforge_lua_listen subscribes to a kind of game event (HF_ON_...); its events wait in a queue the Lua
 *             runtime empties from its own update (hellforge_lua_poll), so the game thread never calls into Lua.
 *   the rest  game state, content by name, packages, armor passives, logging: the HfApi calls of the same names. */
#define HF_LUA_API __declspec(dllexport)
#include "hellforge_lua.h"
#define LUA_BRIDGE_VERSION HF_LUA_BRIDGE_VERSION
#define LUA_EVENT_QUEUE 512
_Static_assert(HF_LUA_EVENT_SIZE == EVT_DATA, "a Lua event holds the largest event");

static SRWLOCK g_luaLock = SRWLOCK_INIT;
static HfLuaEvent g_luaQueue[LUA_EVENT_QUEUE];
static int g_luaQueued;
static uint32_t g_luaDropped;
static volatile LONG g_luaListening; /* bit k: the bridge listens to kind k */

HF_LUA_API int hellforge_lua_info(HfLuaInfo* out)
{
    if (out == NULL || out->size < sizeof *out) return 0;
    *out = (HfLuaInfo){sizeof *out, LUA_BRIDGE_VERSION, HF_MOD_SDK_VERSION, 0, g_build, ApiLogsFolder(NULL)};
    out->ready = (g_fired[HF_EVENT_GAME_LOADED] ? 1u : 0) | (g_fired[HF_EVENT_ENTITIES_READY] ? 2u : 0) |
        (g_fired[HF_EVENT_PROJECTILES_READY] ? 4u : 0) | (g_tickState == 1 ? 8u : 0);
    return 1;
}

/* The name after id's "lua." (`size` bytes in all), lowercased, anything but a-z 0-9 . _ - made "-". */
static void LuaModId(const char* name, char* id, size_t size)
{
    size_t n = 4;
    for (const char* c = name; *c && n < size - 1; c++)
    {
        char ch = *c >= 'A' && *c <= 'Z' ? (char)(*c - 'A' + 'a') : *c;
        id[n++] = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-' ? ch : '-';
    }
    id[n] = 0;
}

/* A Lua mod by name ("hd2runtime", "mods/someone/fast_guns"): lua.<name>, lowercased, anything but a-z 0-9 . _ - made
 * "-" (a mod id's characters). The same name gives the same mod. NULL when the name is empty or the mods are full. */
HF_LUA_API HfMod* hellforge_lua_mod(const char* name)
{
    if (name == NULL || name[0] == 0) return NULL;
    char id[64] = "lua.";
    LuaModId(name, id, sizeof id);
    HfMod* mod = NULL;
    AcquireSRWLockExclusive(&g_luaLock);
    for (int i = 0; i < g_modCount && mod == NULL; i++) if (g_mods[i].lua && !strcmp(g_mods[i].id, id)) mod = &g_mods[i];
    if (mod == NULL && g_modCount < MAX_MODS)
    {
        HfMod* m = &g_mods[g_modCount];
        memset(m, 0, sizeof *m);
        m->api = &g_api;
        snprintf(m->id, sizeof m->id, "%s", id);
        snprintf(m->name, sizeof m->name, "%s", name);
        wchar_t logName[128];
        swprintf(logName, 128, L"mod-%hs.log", m->id);
        m->log = OpenLog(logName);
        m->lua = 1;
        m->loaded = 1;
        MemoryBarrier(); /* the entry is whole before anything counting the mods sees it */
        g_modCount++;
        mod = m;
        Note("%s: Lua mod registered through the Lua bridge", id);
    }
    ReleaseSRWLockExclusive(&g_luaLock);
    return mod;
}

/* SDK 10, mods: the index-th mod of the session, the plan's in load order, then the Lua mods as they registered. */
static int ApiMods(HfMod* caller, uint32_t index, HfModInfo* out)
{
    if (out == NULL || out->size < offsetof(HfModInfo, loaded) + sizeof out->loaded || index >= (uint32_t)g_modCount) return 0;
    uint32_t size = out->size;
    const HfMod* m = &g_mods[index];
    HfModInfo info = {size, m->lua ? HF_MOD_LUA : ModRank(m) == RANK_EDITOR ? HF_MOD_EDITOR : !wcscmp(m->dll, L"-") ? HF_MOD_STATS : HF_MOD_CODE,
        m->id, m->folder, m->name, (uint32_t)(m->loaded != 0)};
    memcpy(out, &info, size < sizeof info ? size : sizeof info);
    return 1;
}

static int LuaMod(HfMod* mod) { return mod != NULL && mod >= g_mods && mod < g_mods + g_modCount && mod->lua; }

/* The field a Lua call names: its component or settings table (either name), offset and type. */
static int LuaField(HfMod* mod, const char* component, uint32_t offset, uint32_t type, HfField* out)
{
    const SdkComponent* c = SdkComponentNamed(component);
    uint32_t size = TypeSize(type);
    if (c == NULL) { ApiLog(mod, "lua: HellForge has no mappings for %s", component ? component : "(none)"); return 0; }
    if (size == 0 || offset > 0xFFFF) { ApiLog(mod, "lua: %s +0x%X: not a single value", c->name, offset); return 0; }
    *out = (HfField){c->component, c->layout, (uint16_t)offset, (uint16_t)size, type, 0};
    return 1;
}

/* set_live for a Lua mod: the value as a number (every type but u64) or as u64. Returns HF_APPLIES_..., or 0 refused
 * (hellforge_lua_last says why). */
HF_LUA_API int hellforge_lua_set(HfMod* mod, const char* component, uint64_t target, uint32_t offset, uint32_t type, // lint-ok: R12 mirrors hellforge_lua.h (a Lua bridge export)
    double number, uint64_t u64, uint32_t flags)
{
    HfField field;
    if (!LuaMod(mod) || !LuaField(mod, component, offset, type, &field)) return 0;
    HfValue value = {type, number, u64};
    return ApiSetLive(mod, target, &field, &value, flags & HF_LIVE_SHARED);
}

/* The Lua mod's live edit of the field undone: the field goes back to the value under it. */
HF_LUA_API int hellforge_lua_clear(HfMod* mod, const char* component, uint64_t target, uint32_t offset, uint32_t type)
{
    HfField field;
    if (!LuaMod(mod) || !LuaField(mod, component, offset, type, &field)) return 0;
    return ApiSetLive(mod, target, &field, NULL, 0);
}

static void LuaNumber(uint32_t type, const uint8_t* bytes, double* number, uint64_t* u64)
{
    *number = 0;
    *u64 = 0;
    if (type == HF_TYPE_U64) { memcpy(u64, bytes, 8); return; }
    uint32_t bits = 0;
    memcpy(&bits, bytes, TypeSize(type));
    HfValue v;
    ValueOf(type, bits, &v);
    if (type == HF_TYPE_U16) v.number = (uint16_t)bits;
    *number = v.number;
}

/* A field's value now and its base (get_live's, any single value of the record, u64 too). Returns HF_APPLIES_..., or 0
 * when it cannot be read (no such record or row, or its table is not loaded yet). */
HF_LUA_API int hellforge_lua_get(HfMod* mod, const char* component, uint64_t target, uint32_t offset, uint32_t type, HfLuaValue* out)
{
    HfField field;
    if (out == NULL) return 0;
    memset(out, 0, sizeof *out);
    if (!LuaMod(mod) || !LuaField(mod, component, offset, type, &field)) return 0;
    const SdkComponent* c = NULL;
    const SdkField* f = ResolveField(mod, "lua get", &field, &c);
    if (f == NULL) return 0;
    int result = 0;
    AcquireSRWLockExclusive(&g_fieldsLock);
    uint8_t* at = LiveAddress(mod, target, c, f);
    uint8_t now[8] = {0};
    if (at != NULL && !SafeRead((uintptr_t)at, now, f->size))
    {
        out->type = type;
        out->applies = f->applies;
        LuaNumber(type, now, &out->now, &out->now_u64);
        uint8_t base[8];
        memcpy(base, now, 8);
        if (f->size == 4 || f->size == 1)
        {
            uint32_t current = 0, below;
            memcpy(&current, now, f->size);
            below = FieldBase(target, c, f, (uintptr_t)at, current, &out->holder);
            memcpy(base, &below, 4);
        }
        LuaNumber(type, base, &out->base, &out->base_u64);
        result = (int)f->applies;
    }
    ReleaseSRWLockExclusive(&g_fieldsLock);
    return result;
}

/* The resources whose index entry names `record` of the component (its entries after its header in the region): how
 * many, *first the first. 0 when the header is not the one loaded, or an entry does not read. */
static uint32_t LuaRecordOwners(const Component* c, uintptr_t region, uint32_t record, uint64_t* first)
{
    uint8_t head[32];
    uint32_t index = 0, count = 0;
    if (!SafeRead(region + c->offset - 4, head, 32)) memcpy(&index, head, 4);
    if (index != c->index || memcmp(head + 4, c->header, 28) != 0) return 0;
    for (uint32_t row = 0; row < c->indices; row++)
    {
        uint8_t entry[16];
        if (SafeRead(region + c->offset + 28 + (uintptr_t)row * 16, entry, 16)) { *first = 0; return 0; }
        uint64_t id;
        uint32_t at;
        memcpy(&id, entry, 8);
        memcpy(&at, entry + 8, 4);
        if (id != 0 && at == record) { if (*first == 0) *first = id; count++; }
    }
    return count;
}

/* The entity a vanilla record of a component belongs to, by its record index in the game's entity data (HD2Runtime's
 * catalogues name records so): the first resource naming it, with *owners how many do (more than one: a shared record,
 * set with HF_LIVE_SHARED). 0 when the data is not loaded or nothing names it. */
HF_LUA_API uint64_t hellforge_lua_owner(const char* component, uint32_t record, uint32_t* owners)
{
    uint32_t count = 0;
    uint64_t first = 0;
    const SdkComponent* sdk = SdkComponentNamed(component);
    const Component* c = NULL;
    for (int i = 0; sdk != NULL && i < g_componentCount; i++) if (!strcmp(g_components[i].name, sdk->component)) c = &g_components[i];
    uintptr_t region = g_region;
    if (c != NULL && region != 0 && c->recordOffset == c->indices * 16) count = LuaRecordOwners(c, region, record, &first);
    if (owners != NULL) *owners = count;
    return first;
}

static void LuaListener(HfMod* mod, const HfEventInfo* event, void* user)
{
    AcquireSRWLockExclusive(&g_luaLock);
    if (g_luaQueued < LUA_EVENT_QUEUE && event->size <= EVT_DATA) memcpy(g_luaQueue[g_luaQueued++].data, event, event->size);
    else g_luaDropped++;
    ReleaseSRWLockExclusive(&g_luaLock);
}

/* Listens to a kind of game event (HF_ON_...) for the Lua runtime: once per kind, whichever Lua mod asks. 1, or 0 (the
 * mod's log says why: an unknown kind, or the game thread not hooked). */
HF_LUA_API int hellforge_lua_listen(HfMod* mod, uint32_t kind)
{
    if (!LuaMod(mod) || kind >= 32) return 0;
    if ((g_luaListening >> kind) & 1) return 1;
    if (!ApiSubscribe(mod, kind, LuaListener, NULL, 0)) return 0;
    InterlockedOr(&g_luaListening, 1L << kind);
    return 1;
}

/* The events raised since the last call, oldest first, at most `capacity`: their count. *dropped: how many the queue
 * could not hold since. */
HF_LUA_API int hellforge_lua_poll(HfLuaEvent* out, int capacity, uint32_t* dropped)
{
    if (out == NULL || capacity <= 0) return 0;
    AcquireSRWLockExclusive(&g_luaLock);
    int n = g_luaQueued < capacity ? g_luaQueued : capacity;
    memcpy(out, g_luaQueue, (size_t)n * sizeof out[0]);
    memmove(g_luaQueue, g_luaQueue + n, (size_t)(g_luaQueued - n) * sizeof g_luaQueue[0]);
    g_luaQueued -= n;
    if (dropped != NULL) *dropped = g_luaDropped;
    g_luaDropped = 0;
    ReleaseSRWLockExclusive(&g_luaLock);
    return n;
}

static int Overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) { return a < b + bn && b < a + an; }

/* Whether HellForge may rewrite game.dll's code in [rva, rva + size): the instructions it points at its moved tables
 * (projectiles, damage, stratagems, difficulty, map locations), the bounds it widens, the branches it retargets and the
 * functions it hooks, and every range it has written since the game started. A Lua runtime that proves the game's code
 * against reviewed bytes skips these (they read as HellForge left them, not as the game shipped them). */
HF_LUA_API int hellforge_lua_rewrites(uint32_t rva, uint32_t size)
{
    #define SITES(table, length) for (size_t i_ = 0; i_ < sizeof table / sizeof table[0]; i_++) \
        if (Overlaps(table[i_].rva, (length), rva, size)) return 1;
    SITES(kProjectileSites, kProjectileSites[i_].size)
    SITES(kDamageSites, kDamageSites[i_].size)
    SITES(kStratagemSites, kStratagemSites[i_].size)
    SITES(kStratagemBounds, (uint32_t)kStratagemBounds[i_].immAt + kStratagemBounds[i_].width)
    SITES(kDifficultySites, kDifficultySites[i_].size)
    SITES(kDifficultyBounds, (uint32_t)kDifficultyBounds[i_].immAt + kDifficultyBounds[i_].width)
    SITES(kLocationRefs, (uint32_t)kLocationRefs[i_].at + 4)
    SITES(kLocationCaps, (uint32_t)kLocationCaps[i_].at + 4)
    SITES(kLocationTypeRefs, (uint32_t)kLocationTypeRefs[i_].at + 4)
    SITES(kLocationTypeCaps, (uint32_t)kLocationTypeCaps[i_].at + 4)
    SITES(kMinimapRefs, (uint32_t)kMinimapRefs[i_].at + 4)
    SITES(kMinimapCaps, (uint32_t)kMinimapCaps[i_].at + 4)
    SITES(kGenerationBlockSize, (uint32_t)kGenerationBlockSize[i_].at + 4)
    #undef SITES
    for (int i = 0; i < S_COUNT; i++) if (Overlaps(kBhvSites[i].ja, 6, rva, size)) return 1;
    if (Overlaps(ABL_JA, 6, rva, size) || Overlaps(REG_ENTITY_LOADER, 16, rva, size) || Overlaps(REG_ARMORY_BUILDER, 16, rva, size)) return 1;
    for (LONG i = 0; i < g_hookedCount; i++) if (Overlaps(g_hookedRvas[i], 16, rva, size)) return 1; /* a hook's jump and the bytes it moved */
    int written = 0; /* every range it wrote (ApiWrite, pointer swaps, far hooks) */
    AcquireSRWLockShared(&g_rewriteLock);
    for (int i = 0; i < g_rewriteCount && !written; i++) written = Overlaps(g_rewriteRva[i], g_rewriteSize[i], rva, size);
    ReleaseSRWLockShared(&g_rewriteLock);
    return written;
}

HF_LUA_API uint32_t hellforge_lua_game_state(void) { return ApiGameState(NULL); }
/* A stratagem's type (its StratagemDefinition row) from its stable id, as HD2Runtime's catalogues name stratagems; 0
 * while the stratagems have not loaded, or for no such stratagem. */
HF_LUA_API uint32_t hellforge_lua_stratagem_type(uint32_t stable) { return (uint32_t)StratagemTypeOf(stable); }
/* set_stat's fields for a Lua mod (live.c: its value at the Lua rank, under the editors'). */
HF_LUA_API int hellforge_lua_set_stat(HfMod* mod, const char* kind, const char* component, uint64_t resource, int64_t record, // lint-ok: R12 mirrors hellforge_lua.h (a Lua bridge export)
    uint32_t offset, uint32_t type, double number)
{
    if (!LuaMod(mod)) return 0;
    HfStat stat = {kind, component, resource, record, offset, type};
    HfValue value = {type, number, 0};
    return ApiSetStat(mod, &stat, &value, 0);
}
HF_LUA_API int hellforge_lua_clear_stat(HfMod* mod, const char* kind, const char* component, uint64_t resource, int64_t record,
    uint32_t offset, uint32_t type)
{
    if (!LuaMod(mod)) return 0;
    HfStat stat = {kind, component, resource, record, offset, type};
    return ApiSetStat(mod, &stat, NULL, 0);
}
HF_LUA_API int hellforge_lua_copy_row(HfMod* mod, const char* table, uint64_t row, uint64_t from, uint32_t offset, uint32_t size)
{
    return LuaMod(mod) ? ApiCopyRow(mod, table, row, from, offset, size) : 0;
}

/* Bridge 3: the SDK 11 actions for a Lua mod (actions.c). */
HF_LUA_API int hellforge_lua_heal(HfMod* mod, float amount) { return LuaMod(mod) ? ApiHeal(mod, amount) : 0; }
HF_LUA_API int hellforge_lua_heal_limb(HfMod* mod, uint32_t limb) { return LuaMod(mod) ? ApiHealLimb(mod, limb) : 0; }
HF_LUA_API int hellforge_lua_injure(HfMod* mod, uint32_t limb, int32_t damage) { return LuaMod(mod) ? ApiInjure(mod, limb, damage) : 0; }
HF_LUA_API int hellforge_lua_limb_state(HfMod* mod, uint32_t entity, uint32_t limb, int32_t* out)
{
    HfLimbState state;
    if (!LuaMod(mod) || out == NULL || !ActLimbState(mod, entity, limb, &state)) return 0;
    out[0] = state.health; out[1] = state.max_health; out[2] = (int32_t)state.injured;
    return HF_ACT_DONE;
}
HF_LUA_API int hellforge_lua_apply_status(HfMod* mod, uint32_t entity, uint32_t status, float buildup) { return LuaMod(mod) ? ApiApplyStatus(mod, entity, status, buildup) : 0; }
HF_LUA_API int hellforge_lua_add_velocity(HfMod* mod, float x, float y, float z) { float v[3] = {x, y, z}; return LuaMod(mod) ? ApiAddVelocity(mod, v) : 0; }
HF_LUA_API int hellforge_lua_resupply_from_pack(HfMod* mod) { return LuaMod(mod) ? ApiResupply(mod) : 0; }
HF_LUA_API int hellforge_lua_spawn_explosion(HfMod* mod, uint32_t type, float x, float y, float z)
{
    float at[3] = {x, y, z};
    return LuaMod(mod) ? ApiSpawnExplosion(mod, type, at) : 0;
}
HF_LUA_API int hellforge_lua_spawn_projectile(HfMod* mod, uint32_t type, float x, float y, float z, float dx, float dy, float dz) // lint-ok: R12 mirrors hellforge_lua.h (a Lua bridge export)
{
    float at[3] = {x, y, z}, direction[3] = {dx, dy, dz};
    return LuaMod(mod) ? ApiSpawnProjectile(mod, type, at, direction) : 0;
}
HF_LUA_API int hellforge_lua_credit_to_player(HfMod* mod, uint32_t entity) { return LuaMod(mod) ? ApiCreditToPlayer(mod, entity) : 0; }
HF_LUA_API int hellforge_lua_freeze_mission_timer(HfMod* mod, int frozen) { return LuaMod(mod) ? ApiFreezeTimer(mod, frozen) : 0; }
HF_LUA_API int hellforge_lua_spawn_pelican(HfMod* mod, float x, float y, float z, float hover, float distance, float height, float fx, float fy, // lint-ok: R12 mirrors hellforge_lua.h (a Lua bridge export)
    uint32_t* entity)
{
    HfPelicanCall call = {sizeof call, {x, y, z}, hover, distance, height, {fx, fy}};
    return LuaMod(mod) ? ApiSpawnPelican(mod, &call, entity) : 0;
}
HF_LUA_API uint32_t hellforge_lua_modify_shots(HfMod* mod, uint32_t projectile, const uint64_t* sources, float damage, float armor_penetration, // lint-ok: R12 mirrors hellforge_lua.h (a Lua bridge export)
    float speed, float gravity, float drag, uint32_t flags)
{
    HfShotRule rule = {sizeof rule, projectile, sources, damage, armor_penetration, speed, gravity, drag, flags};
    return LuaMod(mod) ? ApiModifyShots(mod, &rule) : 0;
}
HF_LUA_API uint32_t hellforge_lua_homing(HfMod* mod, uint32_t projectile, const uint64_t* sources, float turn_rate, float cone, float range, // lint-ok: R12 mirrors hellforge_lua.h (a Lua bridge export)
    float arm_distance, float aim_height, uint32_t flags)
{
    HfHoming rule = {sizeof rule, projectile, sources, turn_rate, cone, range, arm_distance, aim_height, flags};
    return LuaMod(mod) ? ApiHoming(mod, &rule) : 0;
}
HF_LUA_API int hellforge_lua_stop_shots(HfMod* mod, uint32_t id) { return LuaMod(mod) ? ApiStopShots(mod, id) : 0; }

/* Stratagem input codes (StratagemDefinition +0x40: a pointer to u32 directions, +0x48 their count, as a new stratagem's
 * code is set, stratagems.c). A changed code is an array of the loader's (room for any count), kept for the session; a
 * row keeps its code from before the first change. The pointer moves before the count grows onto it and the count before
 * the pointer goes back, so the game never reads past an array. */
#define LUA_CODE_MAX 128
#define LUA_CODE_LENGTH 10
typedef struct LuaCode { uint32_t type; HfMod* mod; uintptr_t row; uint64_t pointer; uint32_t count; uint32_t code[LUA_CODE_LENGTH]; } LuaCode;
static LuaCode g_luaCodes[LUA_CODE_MAX];
static int g_luaCodeCount;

static int LuaCodeValid(HfMod* mod, uint32_t stable, const uint32_t* code, uint32_t count)
{
    if (count < 1 || count > 9) { ApiLog(mod, "set_code %u: %u directions (1 to 9)", stable, count); return 0; }
    for (uint32_t i = 0; i < count; i++)
        if (code[i] < 1 || code[i] > 4) { ApiLog(mod, "set_code %u: direction %u is %u (1 up, 2 right, 3 down, 4 left)", stable, i + 1, code[i]); return 0; }
    return 1;
}

/* The changed code of a stratagem type: its slot, -1 none. Called with g_luaLock held (as are the next three). */
static int LuaCodeFind(uint32_t type)
{
    int slot = -1;
    for (int i = 0; i < g_luaCodeCount; i++) if (g_luaCodes[i].type == type) slot = i;
    return slot;
}

/* The row's code from before its first change back (slot: its change, -1 none). HF_APPLIES_NOW, or 0. */
static int LuaCodeRestore(int slot)
{
    if (slot < 0) return HF_APPLIES_NOW;
    LuaCode* c = &g_luaCodes[slot];
    if (ApiWrite(c->row + 0x48, &c->count, 4) || ApiWrite(c->row + 0x40, &c->pointer, 8)) return 0;
    g_luaCodes[slot] = g_luaCodes[--g_luaCodeCount]; /* its array stays: the game may still hold it */
    return HF_APPLIES_NOW;
}

/* A slot for the row's first change, keeping its code now: the slot; -1 when the slots are full, -2 when the row does
 * not read. */
static int LuaCodeAdd(uint32_t type, uintptr_t row)
{
    if (g_luaCodeCount == LUA_CODE_MAX) return -1;
    LuaCode* c = &g_luaCodes[g_luaCodeCount];
    memset(c, 0, sizeof *c);
    if (SafeRead(row + 0x40, &c->pointer, 8) || SafeRead(row + 0x48, &c->count, 4)) return -2;
    c->type = type;
    c->row = row;
    return g_luaCodeCount++;
}

/* The code into its slot's array, and the row pointed at it. HF_APPLIES_NOW, or 0. */
static int LuaCodeWrite(LuaCode* c, HfMod* mod, uintptr_t row, uint32_t stable, const uint32_t* code, uint32_t count)
{
    int result = HF_APPLIES_NOW;
    c->mod = mod;
    uint32_t* array = c->code;
    uint32_t now = 0;
    SafeRead(row + 0x48, &now, 4);
    /* a shorter code is written over the array only once the count is down to it */
    if (count < now && ApiWrite(row + 0x48, &count, 4)) result = 0;
    for (uint32_t i = 0; i < LUA_CODE_LENGTH && result; i++) array[i] = i < count ? code[i] : 0;
    if (result && (ApiWrite(row + 0x40, &array, 8) || ApiWrite(row + 0x48, &count, 4))) result = 0;
    if (result) ApiLog(mod, "set_code %u (stratagem %u): %u directions", stable, c->type, count);
    return result;
}

HF_LUA_API int hellforge_lua_set_code(HfMod* mod, uint32_t stable, const uint32_t* code, uint32_t count)
{
    if (!LuaMod(mod)) return 0;
    if (code != NULL && !LuaCodeValid(mod, stable, code, count)) return 0;
    uint32_t type = (uint32_t)StratagemTypeOf(stable);
    int ready = 1;
    uintptr_t row = type != 0 ? g_tableRow("StratagemDefinition", type, &ready) : 0;
    if (row == 0) { ApiLog(mod, "set_code %u: no such stratagem, or the stratagems have not loaded yet", stable); return 0; }
    AcquireSRWLockExclusive(&g_luaLock);
    int slot = LuaCodeFind(type), result;
    if (code == NULL) result = LuaCodeRestore(slot);
    else
    {
        if (slot < 0) slot = LuaCodeAdd(type, row);
        if (slot == -1) { ReleaseSRWLockExclusive(&g_luaLock); ApiLog(mod, "set_code: too many changed codes"); return 0; }
        if (slot < 0) { ReleaseSRWLockExclusive(&g_luaLock); ApiLog(mod, "set_code %u: its row is unreadable", stable); return 0; }
        result = LuaCodeWrite(&g_luaCodes[slot], mod, row, stable, code, count);
    }
    ReleaseSRWLockExclusive(&g_luaLock);
    if (!result) ApiLog(mod, "set_code %u: the write did not hold", stable);
    return result;
}
HF_LUA_API uint64_t hellforge_lua_find(uint32_t kind, const char* name) { return ApiFind(kind, name); }
HF_LUA_API int hellforge_lua_load_package(HfMod* mod, uint64_t package) { return LuaMod(mod) ? ApiLoadPackage(mod, package) : 0; }
HF_LUA_API int hellforge_lua_set_passives(HfMod* mod, uint32_t armor, uint32_t second) { return LuaMod(mod) ? ApiSetPassives(mod, armor, second) : 0; }
HF_LUA_API int hellforge_lua_get_passives(HfMod* mod, HfPassives* out) { return LuaMod(mod) ? ApiGetPassives(mod, out) : 0; }
HF_LUA_API void hellforge_lua_log(HfMod* mod, const char* text) { if (LuaMod(mod) && text != NULL) ApiLog(mod, "%s", text); }
HF_LUA_API const char* hellforge_lua_last(HfMod* mod) { return LuaMod(mod) ? mod->last : ""; }
