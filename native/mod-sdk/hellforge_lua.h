/* HellForge's Lua bridge (docs/lua-bridge.md): plain C functions hellforge_modloader.dll exports for a Lua runtime in
 * the game, through LuaJIT's ffi. HD2Runtime uses them when it runs in a HellForge session: its mods' changes become
 * HellForge live edits (the F8 editor names the mod that made them) and its events come from HellForge's event hooks.
 *
 * Load it from Lua only when the DLL is already in the game (GetModuleHandleA("hellforge_modloader.dll")): it is there
 * in a HellForge session with code mods, never in the game HellForge did not start. Check hellforge_lua_info first.
 * Every function is safe to call from the game's Lua update; none calls back into Lua.
 *
 * Values: a field is its component record ("WeaponMagazineComponentData", or HellForge's short name "WeaponMagazine")
 * or settings table ("ProjectileSettings", "DamageInfo", "ExplosionSettings", "ArcSettings", "BeamSettings",
 * "StatusEffectSettings", "StratagemDefinition", ...), an offset and a type (HF_TYPE_..., hellforge_mod.h). A Lua mod
 * may name any aligned single value of the record or row. The target is an entity resource for a component record, a
 * row's id for a settings table. A Lua mod's live edit of a field sits over code mods' and under the editors' (the F8
 * editor's, Local Stats'): it holds the field unless an editor changed it, and comes back when the editor's change goes. */
#ifndef HELLFORGE_LUA_H
#define HELLFORGE_LUA_H
#include <stdint.h>
#include "hellforge_mod.h"

/* The loader builds these functions as its exports (HF_LUA_API __declspec(dllexport)); for anyone else they are plain
 * declarations. */
#ifndef HF_LUA_API
#define HF_LUA_API
#endif

#define HF_LUA_BRIDGE_VERSION 3 /* 2: set_stat, clear_stat, copy_row, set_code, stratagem_type; 3: the SDK 11 events and actions */
#define HF_LUA_EVENT_SIZE 128 /* the largest game event a Lua mod can listen to (the SDK 16 mod channel's is larger) */

typedef struct HfLuaInfo
{
    uint32_t size;    /* set to sizeof(HfLuaInfo) before the call */
    uint32_t bridge;  /* HF_LUA_BRIDGE_VERSION of the loader */
    uint32_t sdk;     /* HF_MOD_SDK_VERSION of the loader */
    uint32_t ready;   /* bit 0 the game loaded, 1 the entity data, 2 the projectile list, 3 the game thread hooked (events) */
    const char* build; /* the game build HellForge runs ("25480438") */
    const char* logs;  /* the session's logs folder (UTF-8): modloader.log, mod-lua.<name>.log */
} HfLuaInfo;

typedef struct HfLuaValue
{
    uint32_t type;      /* HF_TYPE_... */
    uint32_t applies;   /* HF_APPLIES_...: when a change to it takes effect */
    double now, base;   /* every type but u64: the value now, and without the editors' changes */
    uint64_t now_u64, base_u64;
    const char* holder; /* the mod whose value the base is (a Lua mod's live edit, a stats or code mod's), or NULL */
} HfLuaValue;

typedef struct HfLuaEvent { uint8_t data[HF_LUA_EVENT_SIZE]; } HfLuaEvent; /* starts with its HfEventInfo */

/* 1 and *info filled (info->size set first), or 0. */
HF_LUA_API int hellforge_lua_info(HfLuaInfo* info);
/* A Lua mod by name: lua.<name> (lowercased; anything but a-z 0-9 . _ - becomes -). The same name gives the same mod.
 * NULL when the name is empty or the loader holds as many mods as it can. */
HF_LUA_API HfMod* hellforge_lua_mod(const char* name);
/* Changes a field live (set_live): the value as `number` for every type but u64, as `u64` for u64. flags: HF_LIVE_SHARED
 * when the record is one other entities share. Returns HF_APPLIES_..., or 0 refused (hellforge_lua_last says why). */
HF_LUA_API int hellforge_lua_set(HfMod* mod, const char* component, uint64_t target, uint32_t offset, uint32_t type, double number, uint64_t u64, uint32_t flags);
/* Undoes the mod's live edit of the field: it goes back to the value under it. */
HF_LUA_API int hellforge_lua_clear(HfMod* mod, const char* component, uint64_t target, uint32_t offset, uint32_t type);
/* The field now and its base. HF_APPLIES_..., or 0 when it cannot be read (no such record or row, or its table is not
 * loaded yet). */
HF_LUA_API int hellforge_lua_get(HfMod* mod, const char* component, uint64_t target, uint32_t offset, uint32_t type, HfLuaValue* out);
/* The entity a vanilla component record belongs to, by its record index in the game's entity data: the first resource
 * naming it, *owners how many do (more than one: a shared record, set with HF_LIVE_SHARED). 0 when none or not loaded. */
HF_LUA_API uint64_t hellforge_lua_owner(const char* component, uint32_t record, uint32_t* owners);
/* Listens to a kind of game event (HF_ON_..., hellforge_mod.h) for the Lua runtime, once per kind. 1, or 0 (the mod's
 * log says why). Its events wait for hellforge_lua_poll. */
HF_LUA_API int hellforge_lua_listen(HfMod* mod, uint32_t kind);
/* The events raised since the last call, oldest first, at most `capacity`: their count. *dropped: how many the queue
 * (512 events) could not hold since. */
HF_LUA_API int hellforge_lua_poll(HfLuaEvent* out, int capacity, uint32_t* dropped);
/* Whether HellForge may rewrite game.dll's code in [rva, rva + size) (its moved tables' instructions, widened bounds,
 * retargeted branches, hooked functions): code to skip when proving the game's code against reviewed bytes. */
HF_LUA_API int hellforge_lua_rewrites(uint32_t rva, uint32_t size);
HF_LUA_API uint32_t hellforge_lua_game_state(void);                              /* HF_STATE_... */
/* A stratagem's type (the StratagemDefinition row a set names) from its stable id; 0 while the stratagems have not loaded,
 * or for no such stratagem. */
HF_LUA_API uint32_t hellforge_lua_stratagem_type(uint32_t stable);
/* Bridge 2: set_stat for a Lua mod (an entity delta: an attachment's stats, a mission's timer; a status multiplier), as
 * HfStat names the field; the value as a number. Lua mods' values stack as live edits do (under the editors', over code
 * mods'). HF_APPLIES_RESPAWN, or 0 refused (hellforge_lua_last says why). */
HF_LUA_API int hellforge_lua_set_stat(HfMod* mod, const char* kind, const char* component, uint64_t resource, int64_t record,
    uint32_t offset, uint32_t type, double number);
/* The Lua mod's set_stat value taken away: the one under it shows. */
HF_LUA_API int hellforge_lua_clear_stat(HfMod* mod, const char* kind, const char* component, uint64_t resource, int64_t record,
    uint32_t offset, uint32_t type);
/* Bridge 2: HfApi.copy_row (a settings row's bytes from a donor row the catalogue lists; from 0 puts its own back). */
HF_LUA_API int hellforge_lua_copy_row(HfMod* mod, const char* table, uint64_t row, uint64_t from, uint32_t offset, uint32_t size);
/* Bridge 2: a stratagem's input code, by its stable id: `count` (1 to 9) directions, 1 up, 2 right, 3 down, 4 left; code
 * NULL puts back the code from before the first change. A later Lua mod's code replaces an earlier one's. The game reads
 * the code from the row when it builds a mission's stratagem list. HF_APPLIES_NOW, or 0 refused (hellforge_lua_last). */
HF_LUA_API int hellforge_lua_set_code(HfMod* mod, uint32_t stable, const uint32_t* code, uint32_t count);
/* Bridge 3: the SDK 11 actions (HfApi's of the same names, HF_ACT_... or 0 refused; hellforge_lua_last says why), with
 * plain values for LuaJIT's ffi. The SDK 11 events (HF_ON_HEAL .. HF_ON_BUGHOLE) come through hellforge_lua_listen. */
HF_LUA_API int hellforge_lua_heal(HfMod* mod, float amount);
HF_LUA_API int hellforge_lua_heal_limb(HfMod* mod, uint32_t limb);
HF_LUA_API int hellforge_lua_injure(HfMod* mod, uint32_t limb, int32_t damage);
/* out: health, maximum, injured (1 or 0) */
HF_LUA_API int hellforge_lua_limb_state(HfMod* mod, uint32_t entity, uint32_t limb, int32_t* out);
HF_LUA_API int hellforge_lua_apply_status(HfMod* mod, uint32_t entity, uint32_t status, float buildup);
HF_LUA_API int hellforge_lua_add_velocity(HfMod* mod, float x, float y, float z);
HF_LUA_API int hellforge_lua_resupply_from_pack(HfMod* mod);
HF_LUA_API int hellforge_lua_spawn_explosion(HfMod* mod, uint32_t type, float x, float y, float z);
HF_LUA_API int hellforge_lua_spawn_projectile(HfMod* mod, uint32_t type, float x, float y, float z, float dx, float dy, float dz);
HF_LUA_API int hellforge_lua_credit_to_player(HfMod* mod, uint32_t entity);
HF_LUA_API int hellforge_lua_freeze_mission_timer(HfMod* mod, int frozen);
HF_LUA_API int hellforge_lua_spawn_pelican(HfMod* mod, float x, float y, float z, float hover, float distance, float height, float fx, float fy,
    uint32_t* entity);
/* sources: weapon entity resources ending in 0, or NULL */
HF_LUA_API uint32_t hellforge_lua_modify_shots(HfMod* mod, uint32_t projectile, const uint64_t* sources, float damage, float armor_penetration,
    float speed, float gravity, float drag, uint32_t flags);
HF_LUA_API uint32_t hellforge_lua_homing(HfMod* mod, uint32_t projectile, const uint64_t* sources, float turn_rate, float cone, float range,
    float arm_distance, float aim_height, uint32_t flags);
HF_LUA_API int hellforge_lua_stop_shots(HfMod* mod, uint32_t id);
HF_LUA_API uint64_t hellforge_lua_find(uint32_t kind, const char* name);        /* HfApi.find */
HF_LUA_API int hellforge_lua_load_package(HfMod* mod, uint64_t package);         /* HfApi.load_package */
HF_LUA_API int hellforge_lua_set_passives(HfMod* mod, uint32_t armor, uint32_t second); /* HfApi.set_passives */
HF_LUA_API int hellforge_lua_get_passives(HfMod* mod, HfPassives* out);          /* HfApi.get_passives */
HF_LUA_API void hellforge_lua_log(HfMod* mod, const char* text);                 /* a line in mod-lua.<name>.log */
HF_LUA_API const char* hellforge_lua_last(HfMod* mod);                           /* the mod's last log line: why a call was refused */

#endif
