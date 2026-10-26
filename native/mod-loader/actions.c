/* SDK 11 actions: what a mod makes the game do, through the game's own functions, for game.dll build 25480438. From
 * HD2Runtime 0.40.1's research (event-combat, player-injury-path, player-avatar-actions, player-equipment, event-actions,
 * event-explosions, projectile-rows, projectile-ballistics, projectile-homing, pelican, mission-timer); HD2Runtime made
 * these same calls from the game's Lua update. Included by hellforge_modloader.c after refs.c.
 *
 *   heal          AddHealthFraction 0x91E920 (health manager, entity, float fraction): health and every limb by the share.
 *   heal_limb     RestoreZone 0x65B2D0 (NULL, entity, zone name hash): the limb back to full.
 *   injure        QueueDamage 0x129F910, kind 6 (ability) on the limb's actor (UnitActorByName, the engine API table's
 *                 +0x10, helldivers2.exe 0x799DE0), as the VG-70's own self-damage does; the drain applies it.
 *   apply_status  QueueStatusRequest 0x129F170 (NULL, type, target, float buildup, instigator, 0).
 *   add_velocity  SetVelocity 0x4A7550 (NULL, entity, const float v[3]) with the Helldiver's motion record's velocity.
 *   resupply      the Supply Pack's self-use ability 2629 through try_start_action 0xA431D0, if needs_ammo 0x9AD7D0 says so.
 *   explosion     RequestExplosion 0x13C0A80 (the HF_ON_EXPLOSION hook's function, through it when hooked).
 *   projectile    FireProjectile 0x13A8F50 (system, type, position, direction, entity, 0, 0).
 *   credit        the entity's no-credit tag (0x10000000 of its tag word) cleared, so the game's Creditor 0x129C690 names
 *                 the owner of its network object.
 *   timer         the mission timer's pause byte ([game + 0x3326D10] + 0x5194E), which the countdown's update honours.
 *   pelican       the entity spawn request 0xFD9710 with shuttle_transport and a context holding the hover point; its
 *                 behaviour's release time moved for the hover.
 *   shots         the projectile pool ([game + 0x347CEA8]): each new shot of a rule's type from this machine's player,
 *                 its damage and penetration multipliers and flight values scaled, or its velocity turned each update.
 * Every native's first bytes are checked before its first call, and the data each reads as the research found it. */

#define ACT_AVATAR 0x4D1C334D294DFA97ull
#define ACT_SUPPLY_PACK 0x4EF9A47109239A58ull
#define ACT_PELICAN_RESOURCE 0x75BE82ED8592A6B3ull

typedef struct ActNative { uint32_t rva; const char* hex; int state; } ActNative; /* state 1 proven, -1 not */
static ActNative g_actHeal = {0x91E920u, "488bc4488958104889701848897820554154415541564157488d68a14881ecf00000000f2970c8", 0};
static ActNative g_actRestoreZone = {0x65B2D0u, "48895c240848896c2410488974241848897c24204154415641574883ec70", 0};
static ActNative g_actQueueDamage = {0x129F910u, "40555356574156488d6c24f94881ecd0000000488b05e6c639014833c4488945d7443b0de8421e02458bf0488b3df6d51d02448bc2", 0};
static ActNative g_actStatus = {0x129F170u, "48895c240848896c24104889742420574883ec20443b05954a1e02418bd8488b35a3dd1d028bea745c", 0};
static ActNative g_actVelocity = {0x4A7550u, "48895c2418574883ec203b15c0c6fd02488b1df1efe70248896c2430", 0};
static ActNative g_actNeedsAmmo = {0x9AD7D0u, "48895c241048896c2418488974242057", 0};
static ActNative g_actStartAction = {0xA431D0u, "48895c2408574883ec20488bfa488bd9", 0};
static ActNative g_actExplosion = {0x13C0A80u, "40534883ec308b41204c8bda488bd93d000100000f8369020000", 0};
static ActNative g_actProjectile = {0x13A8F50u, "4c8bdc4d8963184d896b205541564157498dab28f6ffff4881ecc00a0000488b059b3029014833c4488985300900004c8b25223f0d024d8bf14d8be8448bfa41807c2428000f8410080000", 0};
static ActNative g_actPelican = {0xFD9710u, "4c8bdc49895b1049897320574881eca0000000488b05e62866014833c4488984249000000033c0498bf84d8b4050488bda49894388488bf14989439049894398498943a0498943a8498943b0498943b8498943c0498943c8498943d0498943d84d85c0750c498d4388488947504d8d4388e80abe3000488b0d0b2849024c8bcb488bd648897c24204c8b01e820290000488bc6488b8c24900000004833cce8bdef0a014c8d9c24a0000000498b5b18498b7328498be35fc3", 0};

/* Whether code or data reads as `hex` (HellForge's own rewrites of game.dll left out). */
static int ActBytes(uintptr_t at, const char* hex, uint32_t rvaIfGame)
{
    uint8_t want[256], got[256];
    size_t n = strlen(hex) / 2;
    if (n > sizeof want) return 0;
    for (size_t i = 0; i < n; i++) want[i] = (uint8_t)(Hex(hex[2 * i]) << 4 | Hex(hex[2 * i + 1]));
    if (rvaIfGame && hellforge_lua_rewrites(rvaIfGame, (uint32_t)n)) return 1;
    return !SafeRead(at, got, n) && !memcmp(got, want, n);
}
typedef struct ActPin { int exe; uint32_t rva; const char* hex; } ActPin;
static int ActPins(uintptr_t game, const ActPin* pins, size_t count)
{
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    for (size_t i = 0; i < count; i++)
        if (!ActBytes((pins[i].exe ? exe : game) + pins[i].rva, pins[i].hex, pins[i].exe ? 0 : pins[i].rva)) return 0;
    return 1;
}
/* The native at game + rva, once its first bytes are proven; 0 (logged) when they are not the reviewed ones. */
static uintptr_t ActFunction(HfMod* mod, uintptr_t game, ActNative* n, const char* what)
{
    if (n->state == 0) n->state = ActBytes(game + n->rva, n->hex, n->rva) ? 1 : -1;
    if (n->state < 0) { ApiLog(mod, "%s: the game's function is not the reviewed one", what); return 0; }
    return game + n->rva;
}

/* ---- the session ------------------------------------------------------------------------------------------------- */

/* In a mission: whether this machine hosts it, and the game mode's entity. */
static int ActMission(uintptr_t game, int* host, uint32_t* modeEntity)
{
    uintptr_t mode, d;
    if (ReadState(game) != HF_STATE_MISSION || !EvpPtr(game + 0x33266A0u, &mode) || EvpU32(mode + 8, 0) != 1 || !EvpPtr(mode + 0x38, &d)) return 0;
    if (host) *host = EvpU32(d + 0x14, 0) & 1;
    if (modeEntity) *modeEntity = EvpU32(d + 8, 0);
    return 1;
}
static uint64_t ActLocalPeer(uintptr_t game)
{
    uintptr_t user; uint64_t peer = 0;
    if (EvpPtr(game + EVT_LOCAL_PEER, &user)) SafeRead(user + EVT_LOCAL_PEER_AT, &peer, 8);
    return peer;
}

typedef struct ActHealth { uintptr_t manager, record; uint32_t index, unit, network, owned, life; uint64_t resource; int32_t health, max; } ActHealth;
/* An entity's health record and its descriptor (the record's own). */
static int ActHealthOf(uintptr_t game, uint32_t entity, ActHealth* h)
{
    memset(h, 0, sizeof *h);
    uintptr_t descriptors, descriptor, records, ext;
    uint8_t d[24];
    if (!EvpPtr(game + EVP_HEALTH, &h->manager) || !EvpHash(h->manager + 0x1030, entity, 8, 4, &h->index) ||
        h->index >= EvpU32(h->manager + 0x1020, 0) || !EvpPtr(h->manager + 0x1048, &descriptors) ||
        !EvpPtr(descriptors + (uintptr_t)h->index * 8, &descriptor) || SafeRead(descriptor, d, sizeof d) || !EvpPtr(h->manager + 0x1058, &records)) return 0;
    uint32_t named; memcpy(&named, d + 8, 4);
    if (named != entity) return 0;
    memcpy(&h->resource, d, 8); memcpy(&h->unit, d + 12, 4); memcpy(&h->network, d + 16, 4); h->owned = d[20] & 1;
    h->record = records + (uintptr_t)h->index * 0x1B8;
    if (SafeRead(h->record + 0x14, &h->health, 4) || SafeRead(h->record + 0x19C, &h->life, 4)) return 0;
    h->max = -1;
    if (EvpPtr(h->manager + 0x1060, &ext)) SafeRead(ext + (uintptr_t)h->index * 0x1C + 0x14, &h->max, 4);
    return 1;
}
/* This machine's player's Helldiver: owned, alive (or downed or dead with `dead`). 0 with *why. */
static uint32_t ActLocalAvatar(uintptr_t game, int dead, ActHealth* h, const char** why)
{
    EvpPlayer players[EVT_MAX_PLAYERS];
    int count = EvpPlayers(game, players);
    uint32_t avatar = 0;
    for (int i = 0; i < count; i++) if (players[i].local) avatar = players[i].avatar;
    *why = "this machine's player has no Helldiver";
    if (!avatar || !EvpAlive(avatar) || !ActHealthOf(game, avatar, h)) return 0;
    if (h->resource != ACT_AVATAR || !h->owned) { *why = "this machine's player's avatar is not a Helldiver it owns"; return 0; }
    if (!dead && h->life == 1) { *why = "the Helldiver is downed"; return 0; }
    if (!dead && h->life >= 2) { *why = "the Helldiver is dead"; return 0; }
    return avatar;
}
static int ActPlayerCount(uintptr_t game) { EvpPlayer p[EVT_MAX_PLAYERS]; return EvpPlayers(game, p); }

/* ---- the actions --------------------------------------------------------------------------------------------------- */

static const uint32_t kActZones[6] = {0x8C5570C9u, 0xAA1DB0B0u, 0x1880288Eu, 0xB6F2BAF5u, 0x64A3FA1Du, 0x87B05FF4u};
static const uint32_t kActActors[6] = {0x8C5570C9u, 0x37EF0820u, 0x5C0C68D3u, 0x248004F5u, 0x8FD2DFECu, 0x3AC8154Au};
static const int32_t kActLimbMax[6] = {85, 60, 35, 35, 45, 45};
static const uint32_t kActStatuses[] = {5, 6, 32, 37, 38, 39, 42, 43, 44, 45, 67};

static int ActHealNow(HfMod* mod, uintptr_t game, float amount)
{
    static const ActPin pins[] = {{0, 0x91EA17u, "41f644241401"}, {0, 0x91EA3Cu, "e80f860000"}, {0, 0x91EA86u, "0f4cc8"},
        {0, 0x4A441Au, "4c8b156722e802"}, {0, 0x91D694u, "8b4514"}};
    if (!(amount > 0.0f) || amount > 100000.0f) { ApiLog(mod, "heal: the amount must be above 0"); return 0; }
    ActHealth h; const char* why;
    uint32_t avatar = ActLocalAvatar(game, 0, &h, &why);
    if (!avatar) { ApiLog(mod, "heal: %s", why); return 0; }
    if (h.max <= 0 || h.max > 100000) { ApiLog(mod, "heal: the Helldiver's maximum health does not read"); return 0; }
    if (h.health >= h.max) return HF_ACT_DONE;
    uintptr_t fn = ActFunction(mod, game, &g_actHeal, "heal");
    if (!fn) return 0;
    if (!ActPins(game, pins, sizeof pins / sizeof pins[0])) { ApiLog(mod, "heal: the health code is not the reviewed one"); return 0; }
    float applied = amount < (float)(h.max - h.health) ? amount : (float)(h.max - h.health);
    ((void (*)(void*, uint32_t, float))fn)((void*)h.manager, avatar, applied / (float)h.max);
    return HF_ACT_DONE;
}

static int ActHealLimbNow(HfMod* mod, uintptr_t game, uint32_t limb)
{
    static const ActPin pins[] = {{0, 0x65B2FDu, "3b151d89e202"}, {0, 0x65B303u, "418bf0"}, {0, 0x65B306u, "4c8b3d7bb3cc02"},
        {0, 0x65B30Du, "8bda"}, {0, 0x65B3BCu, "4289948df8000000"}};
    if (limb > HF_LIMB_RIGHT_LEG) { ApiLog(mod, "heal_limb: no limb %u", limb); return 0; }
    ActHealth h; const char* why;
    uint32_t avatar = ActLocalAvatar(game, 0, &h, &why);
    if (!avatar) { ApiLog(mod, "heal_limb: %s", why); return 0; }
    uintptr_t fn = ActFunction(mod, game, &g_actRestoreZone, "heal_limb");
    if (!fn) return 0;
    if (!ActPins(game, pins, sizeof pins / sizeof pins[0])) { ApiLog(mod, "heal_limb: the limb code is not the reviewed one"); return 0; }
    ((void (*)(void*, uint32_t, uint32_t))fn)(NULL, avatar, kActZones[limb]);
    return HF_ACT_DONE;
}

static int ActLimbState(HfMod* mod, uint32_t entity, uint32_t limb, HfLimbState* out)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    ActHealth h;
    if (out == NULL || limb > HF_LIMB_RIGHT_LEG) return 0;
    memset(out, 0, sizeof *out);
    if (!game || strcmp(g_build, "25480438") || !ActHealthOf(game, entity, &h) || h.resource != ACT_AVATAR) { ApiLog(mod, "limb_state: no Helldiver %u", entity); return 0; }
    uint32_t word;
    if (SafeRead(h.record + 0xF8 + 4 * limb, &out->health, 4) || SafeRead(h.record + 0x20, &word, 4)) return 0;
    out->max_health = kActLimbMax[limb];
    out->injured = ((word >> (2 * limb)) & 3) == 2;
    return HF_ACT_DONE;
}

typedef void (*QueueDamageFn)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t, uint32_t, uint32_t, uint64_t, uint32_t, float, float, float,
    const float*, const float*, uint32_t, uint32_t, uint32_t, float);
static int ActInjureNow(HfMod* mod, uintptr_t game, uint32_t limb, int32_t damage)
{
    static const ActPin checks[] = {{1, 0x799DF0u, "8bea"}, {1, 0x799E62u, "b8ffffffff"}, {0, 0x11AE1DAu, "418d5006"}};
    if (limb > HF_LIMB_RIGHT_LEG) { ApiLog(mod, "injure: no limb %u", limb); return 0; }
    if (damage < 1 || damage > kActLimbMax[limb]) { ApiLog(mod, "injure: damage %d is outside 1 .. %d", damage, kActLimbMax[limb]); return 0; }
    if (!ActMission(game, NULL, NULL)) { ApiLog(mod, "injure: not in a mission"); return 0; }
    ActHealth h; const char* why;
    uint32_t avatar = ActLocalAvatar(game, 0, &h, &why);
    if (!avatar) { ApiLog(mod, "injure: %s", why); return 0; }
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL), api, actorFn, system, em;
    uint32_t slot;
    if (!EvpPtr(game + 0x3326310u, &api) || api != exe + 0x27CD4C0u || !EvpPtr(api + 0x10, &actorFn) || actorFn != exe + 0x799DE0u ||
        !ActBytes(actorFn, "48896c2420564881ec80000000498bf08bea", 0) || !ActPins(game, checks, sizeof checks / sizeof checks[0]))
    { ApiLog(mod, "injure: the engine's actor lookup is not the reviewed one"); return 0; }
    uintptr_t fn = ActFunction(mod, game, &g_actQueueDamage, "injure");
    if (!fn) return 0;
    if (!EvpPtr(game + 0x347CF38u, &system) || EvpU32(system + 0x201120, 4096) >= 4096) { ApiLog(mod, "injure: the damage queue is full"); return 0; }
    if (h.network == 0 || h.network >= 0x7FFF || !EvpPtr(game + EVP_ENTITIES, &em) || !EvpHash(em + EVP_NETWORK_MAP, h.network, 8, 4, &slot) ||
        EvpU32(em + EVP_NETWORK_ENTITY + (uintptr_t)slot * 24, 0) != avatar || h.unit == 0) { ApiLog(mod, "injure: the Helldiver has no network object"); return 0; }
    uint64_t creditor = 0;
    SafeRead(h.record + 0x38, &creditor, 8);
    uint32_t actor = ((uint32_t (*)(uint32_t, uint32_t, uint32_t*))actorFn)(h.unit, kActActors[limb], NULL);
    if (actor == 0xFFFFFFFFu) { ApiLog(mod, "injure: the Helldiver has no such limb"); return 0; }
    static const float direction[3] = {0, 0, -1}, position[3] = {0, 0, 0};
    ((QueueDamageFn)fn)(NULL, 6, 0, avatar, (uint32_t)damage, 1, h.network, h.network, creditor, 0, 0.0f, 0.0f, 0.0f, direction, position, actor, 9, 0, 1.0f);
    return HF_ACT_REQUESTED;
}

static int ActStatusApplies(uint32_t status)
{
    int allowed = 0;
    for (size_t i = 0; i < sizeof kActStatuses / sizeof kActStatuses[0]; i++) if (kActStatuses[i] == status) allowed = 1;
    return allowed;
}
static int ActStatusNow(HfMod* mod, uintptr_t game, uint32_t target, uint32_t status, float buildup)
{
    static const ActPin pins[] = {{0, 0x129F218u, "8b8634112000"}, {0, 0x129F21Eu, "3d00100000"}};
    int host = 0;
    if (!ActStatusApplies(status)) { ApiLog(mod, "apply_status: %u is not a status HellForge applies (HF_STATUS_...)", status); return 0; }
    if (!(buildup > 0.0f) || buildup > 1000.0f) { ApiLog(mod, "apply_status: the buildup must be above 0, at most 1000"); return 0; }
    if (!ActMission(game, &host, NULL) || !host) { ApiLog(mod, "apply_status: only the host, in a mission"); return 0; }
    ActHealth h; const char* why;
    uint32_t instigator = ActLocalAvatar(game, 1, &h, &why);
    if (!instigator) { ApiLog(mod, "apply_status: %s", why); return 0; }
    if (!EvpAlive(target)) { ApiLog(mod, "apply_status: entity %u is gone", target); return 0; }
    uintptr_t fn = ActFunction(mod, game, &g_actStatus, "apply_status"), queue, manager, row;
    if (!fn) return 0;
    if (!ActPins(game, pins, sizeof pins / sizeof pins[0])) { ApiLog(mod, "apply_status: the status queue is not the reviewed one"); return 0; }
    if (!EvpPtr(game + 0x347CF38u, &queue) || EvpU32(queue + 0x201134, 4096) >= 4096) { ApiLog(mod, "apply_status: the status queue is full"); return 0; }
    if (!EvpPtr(game + 0x3326620u, &manager) || !EvpPtr(game + 0x37C5C50u + status * 8, &row) || EvpU32(row, 0) != status)
    { ApiLog(mod, "apply_status: status %u is not loaded", status); return 0; }
    ((void (*)(void*, uint32_t, uint32_t, float, uint32_t, uint32_t))fn)(NULL, status, target, buildup, instigator, 0);
    return HF_ACT_REQUESTED;
}

static int ActVelocityNow(HfMod* mod, uintptr_t game, const float* delta)
{
    static const ActPin pins[] = {{0, 0x4A75DEu, "488b8bc8480000"}, {0, 0x4A75FCu, "4969d084000000"}, {0, 0x4A7603u, "f20f110411"}};
    for (int k = 0; k < 3; k++) if (!isfinite(delta[k]) || fabsf(delta[k]) > 25.0f) { ApiLog(mod, "add_velocity: each part at most 25 m/s"); return 0; }
    ActHealth h; const char* why;
    uint32_t avatar = ActLocalAvatar(game, 0, &h, &why);
    if (!avatar) { ApiLog(mod, "add_velocity: %s", why); return 0; }
    uintptr_t fn = ActFunction(mod, game, &g_actVelocity, "add_velocity"), motion, descriptors, descriptor, records;
    uint32_t index;
    if (!fn) return 0;
    if (!ActPins(game, pins, sizeof pins / sizeof pins[0]) || !EvpPtr(game + 0x3326558u, &motion) || !EvpHash(motion + 0x48A0, avatar, 8, 4, &index) ||
        index >= EvpU32(motion + 0x4880, 0) || index >= 0x10000 || !EvpPtr(motion + 0x48B8, &descriptors) ||
        !EvpPtr(descriptors + (uintptr_t)index * 8, &descriptor) || EvpU32(descriptor + 8, 0) != avatar || !(EvpU32(descriptor + 0x14, 0) & 1) ||
        !EvpPtr(motion + 0x48C8, &records)) { ApiLog(mod, "add_velocity: the Helldiver has no motion record"); return 0; }
    float v[3];
    if (SafeRead(records + (uintptr_t)index * 0x84, v, sizeof v)) return 0;
    float speed = 0;
    for (int k = 0; k < 3; k++) { if (!isfinite(v[k])) return 0; v[k] += delta[k]; speed += v[k] * v[k]; }
    if (sqrtf(speed) > 50.0f) { ApiLog(mod, "add_velocity: the Helldiver would go faster than 50 m/s"); return 0; }
    ((void (*)(void*, uint32_t, const float*))fn)(NULL, avatar, v);
    return HF_ACT_DONE;
}

/* The Supply Pack's definition: the pack's own override, else its type's. 0 when neither reads. */
static uintptr_t ActPackDefinition(uintptr_t game, uintptr_t deposits, uint32_t pack, uint64_t resource)
{
    uintptr_t defs, em, table, rec = 0;
    uint32_t override_;
    if (EvpHash(deposits + 0x60, pack, 8, 4, &override_) && EvpPtr(deposits + 0xA0, &defs)) return defs + (uintptr_t)override_ * 0x98;
    if (!EvpPtr(game + EVP_ENTITIES, &em) || !EvpPtr(em + 0xF12A00, &table)) return 0;
    for (uint32_t i = 0, at = (uint32_t)(resource % 58); i < 58 && !rec; i++, at = (at + 1) % 58)
    {
        uint64_t type; uint32_t slot;
        if (SafeRead(table + (uintptr_t)at * 16, &type, 8) || type == 0) break;
        if (type == resource && !SafeRead(table + (uintptr_t)at * 16 + 8, &slot, 4)) rec = table + 0x3A0 + (uintptr_t)slot * 0x98;
    }
    return rec;
}
/* The Helldiver's B-1 Supply Pack, its definition the reviewed one: 1 with *supplies, its supplies; 0 logged. */
static int ActSupplyPack(HfMod* mod, uintptr_t game, uint32_t avatar, uint32_t* supplies)
{
    uintptr_t inventory, records, deposits, amounts;
    uint32_t index, deposit;
    if (!EvpPtr(game + EVP_INVENTORY, &inventory) || !EvpHash(inventory + 0x28, avatar, 8, 4, &index) || index >= 0x10000 || !EvpPtr(inventory + 0x50, &records))
    { ApiLog(mod, "resupply_from_pack: the Helldiver has no inventory"); return 0; }
    uint32_t pack = EvpU32(records + (uintptr_t)index * 0x30 + 0x0C, 0);
    if (!pack || !EvpAlive(pack)) { ApiLog(mod, "resupply_from_pack: the Helldiver has no backpack"); return 0; }
    uint64_t resource = EvpEntityResource(game, pack, NULL);
    if (resource != ACT_SUPPLY_PACK) { ApiLog(mod, "resupply_from_pack: the backpack is not a B-1 Supply Pack"); return 0; }
    if (!EvpPtr(game + 0x33265E8u, &deposits) || !EvpHash(deposits + 0x20, pack, 8, 4, &deposit) || !EvpPtr(deposits + 0x50, &amounts))
    { ApiLog(mod, "resupply_from_pack: the pack's supplies do not read"); return 0; }
    *supplies = EvpU32(amounts + (uintptr_t)deposit * 8, 0);
    uintptr_t rec = ActPackDefinition(game, deposits, pack, resource);
    if (!rec || EvpU32(rec + 0x24, 0) != 2629) { ApiLog(mod, "resupply_from_pack: the Supply Pack's definition is not the reviewed one"); return 0; }
    return 1;
}
/* The Helldiver's action context, free to start an action: its address with *none, the game's no-action id; 0 (logged
 * when the Helldiver is busy or cannot act). */
static uintptr_t ActFreeContext(HfMod* mod, uintptr_t game, uint32_t avatar, uint32_t* none)
{
    uintptr_t actions, descriptor;
    uint32_t ai;
    if (!EvpPtr(game + 0x3326D20u, &actions) || !EvpHash(actions + 0xF8, avatar, 8, 4, &ai) || !EvpPtr(actions + 0x110 + (uintptr_t)ai * 8, &descriptor) ||
        EvpU32(descriptor + 8, 0) != avatar) { ApiLog(mod, "resupply_from_pack: the Helldiver has no action record"); return 0; }
    uintptr_t context = actions + 0x53E170 + (uintptr_t)ai * 0x1238;
    uint64_t f0 = 0, f1 = 0;
    SafeRead(actions + 0x53E880 + (uintptr_t)ai * 0x1238, &f0, 8); SafeRead(actions + 0x53E888 + (uintptr_t)ai * 0x1238, &f1, 8);
    *none = EvpU32(game + 0x3483C5Cu, 0);
    if (EvpU32(context, 0) != avatar) return 0;
    if (((f1 >> 21) & 1) || EvpU32(context + 8, 0) != *none) { ApiLog(mod, "resupply_from_pack: the Helldiver is busy"); return 0; }
    if ((f1 & 0x1F9D9AF7875B0200ull) || (f0 & 0x0000000100010000ull) || !(f0 & 2)) { ApiLog(mod, "resupply_from_pack: the Helldiver cannot act now"); return 0; }
    return context;
}
static int ActResupplyNow(HfMod* mod, uintptr_t game)
{
    static const ActPin pins[] = {{0, 0x9AD94Au, "83f817"}, {0, 0xA4385Au, "8b4824"}, {0, 0xA431B1u, "428b841088e85300"},
        {0, 0xA43301u, "48b900025b87f79a9d1f"}, {0, 0xA4331Bu, "48b80000010001000000"}};
    if (!ActMission(game, NULL, NULL)) { ApiLog(mod, "resupply_from_pack: not in a mission"); return 0; }
    ActHealth h; const char* why;
    uint32_t avatar = ActLocalAvatar(game, 0, &h, &why);
    if (!avatar) { ApiLog(mod, "resupply_from_pack: %s", why); return 0; }
    uintptr_t needs = ActFunction(mod, game, &g_actNeedsAmmo, "resupply_from_pack"), start = ActFunction(mod, game, &g_actStartAction, "resupply_from_pack");
    if (!needs || !start) return 0;
    if (!ActPins(game, pins, sizeof pins / sizeof pins[0])) { ApiLog(mod, "resupply_from_pack: the Supply Pack's code is not the reviewed one"); return 0; }
    uint32_t supplies, none;
    if (!ActSupplyPack(mod, game, avatar, &supplies)) return 0;
    if (supplies < 1) { ApiLog(mod, "resupply_from_pack: the Supply Pack is empty"); return 0; }
    uintptr_t context = ActFreeContext(mod, game, avatar, &none);
    if (!context) return 0;
    if (!((uint8_t (*)(void*, uint32_t))needs)(NULL, avatar)) { ApiLog(mod, "resupply_from_pack: the Helldiver needs no ammunition"); return 0; }
    uint32_t request[3] = {2629, none, EvpU32(context + 0xC, 0)};
    if (!((uint8_t (*)(void*, const uint32_t*))start)((void*)context, request)) { ApiLog(mod, "resupply_from_pack: the game did not start it"); return 0; }
    return HF_ACT_REQUESTED;
}

/* An explosion's or projectile's packages, as HellForge's reference catalogue lists them (refs.c): loaded, or loading. */
static int ActPackages(HfMod* mod, uint32_t ref, uint32_t value, const char* what)
{
    char why[256];
    int state = RefCheck(ref, value, 0xFFFFFFFFu, 0, why, sizeof why);
    if (state == REF_REFUSED) { ApiLog(mod, "%s: %s", what, why); return 0; }
    return state == REF_WAIT ? HF_ACT_WAITING : HF_ACT_DONE;
}

/* Whether p is a position: finite, each part within 100 km. */
static int ActIsPosition(const float* p)
{
    for (int k = 0; k < 3; k++) if (!isfinite(p[k]) || fabsf(p[k]) > 100000.0f) return 0;
    return 1;
}
/* Whether the HF_ON_EXPLOSION hook is in (its first bytes are its jump then). */
static int ActExplosionHooked(void)
{
    int hooked = 0;
    for (size_t i = 0; i < sizeof g_eventHooks / sizeof g_eventHooks[0]; i++) if (g_eventHooks[i].kind == HF_ON_EXPLOSION && g_eventHooks[i].state == 1) hooked = 1;
    return hooked;
}
typedef uint64_t (*RequestExplosionFn)(void*, const float*, uint32_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t);
static int ActExplosionNow(HfMod* mod, uintptr_t game, uint32_t type, const float* position)
{
    int host = 0;
    if (!ActIsPosition(position)) { ApiLog(mod, "spawn_explosion: not a position"); return 0; }
    if (!ActMission(game, &host, NULL) || !host) { ApiLog(mod, "spawn_explosion: only the host, in a mission"); return 0; }
    int ready = 1;
    uintptr_t row = g_tableRow("ExplosionSettings", type, &ready), queue;
    if (!row || EvpU32(row, 0) != type) { ApiLog(mod, "spawn_explosion: no explosion %u", type); return 0; }
    int packages = ActPackages(mod, REF_EXPLOSION, type, "spawn_explosion");
    if (packages != HF_ACT_DONE) return packages;
    ActHealth h; const char* why;
    uint32_t avatar = ActLocalAvatar(game, 1, &h, &why);
    if (!avatar) { ApiLog(mod, "spawn_explosion: %s", why); return 0; }
    /* through the HF_ON_EXPLOSION hook when it is in, so listeners hear it too */
    uintptr_t fn = ActExplosionHooked() ? game + g_actExplosion.rva : ActFunction(mod, game, &g_actExplosion, "spawn_explosion");
    if (!fn) return 0;
    if (!EvpPtr(game + 0x346D558u, &queue) || EvpU32(queue + 0x20, 256) >= 256) { ApiLog(mod, "spawn_explosion: the explosion queue is full"); return 0; }
    ((RequestExplosionFn)fn)((void*)queue, position, type, avatar, avatar, ActLocalPeer(game), 0, 0, 1, 0, 0, 0, 0, 0, 0);
    return HF_ACT_REQUESTED;
}

static int ActProjectileNow(HfMod* mod, uintptr_t game, uint32_t type, const float* position, const float* direction)
{
    int host = 0;
    float d[3], length = 0;
    for (int k = 0; k < 3; k++)
    {
        if (!isfinite(position[k]) || fabsf(position[k]) > 100000.0f || !isfinite(direction[k])) { ApiLog(mod, "spawn_projectile: not a position and direction"); return 0; }
        length += direction[k] * direction[k];
    }
    length = sqrtf(length);
    if (length < 1e-6f) { ApiLog(mod, "spawn_projectile: the direction is 0"); return 0; }
    for (int k = 0; k < 3; k++) d[k] = direction[k] / length;
    if (!ActMission(game, &host, NULL) || !host) { ApiLog(mod, "spawn_projectile: only the host, in a mission"); return 0; }
    int ready = 1;
    uintptr_t row = g_tableRow("ProjectileSettings", type, &ready), system;
    if (!row || EvpU32(row, 0) != type) { ApiLog(mod, "spawn_projectile: no projectile %u", type); return 0; }
    int packages = ActPackages(mod, REF_PROJECTILE, type, "spawn_projectile");
    if (packages != HF_ACT_DONE) return packages;
    ActHealth h; const char* why;
    uint32_t avatar = ActLocalAvatar(game, 1, &h, &why);
    if (!avatar) { ApiLog(mod, "spawn_projectile: %s", why); return 0; }
    uintptr_t fn = ActFunction(mod, game, &g_actProjectile, "spawn_projectile");
    uint8_t active = 0;
    if (!fn) return 0;
    if (!EvpPtr(game + 0x347CEA8u, &system) || SafeRead(system + 0x28, &active, 1) || active != 1) { ApiLog(mod, "spawn_projectile: projectiles are not running"); return 0; }
    ((void (*)(void*, uint32_t, const float*, const float*, uint32_t, uint32_t, uint64_t))fn)((void*)system, type, position, d, avatar, 0, 0);
    return HF_ACT_REQUESTED;
}

static int ActCreditNow(HfMod* mod, uintptr_t game, uint32_t entity)
{
    static const ActPin pins[] = {{0, 0x129C716u, "41837904ff"}, {0, 0x129C72Eu, "41b800000010"}, {0, 0x129C748u, "3dff7f0000"},
        {0, 0x6B51AFu, "4c8b1d7a12c702"}, {0, 0x6B5229u, "498b4338"}};
    int host = 0;
    if (!ActMission(game, &host, NULL) || !host) { ApiLog(mod, "credit_to_player: only the host, in a mission"); return 0; }
    if (ActPlayerCount(game) != 1) { ApiLog(mod, "credit_to_player: solo only (another machine credits its own copy)"); return 0; }
    if (!ActPins(game, pins, sizeof pins / sizeof pins[0])) { ApiLog(mod, "credit_to_player: the kill credit code is not the reviewed one"); return 0; }
    uintptr_t wielders, wieldersOf, tags, tagValues, factions, masks, behaviours, handles, handle;
    uint32_t wi, ti, fi, bi;
    if (!EvpPtr(game + 0x3326730u, &wielders) || !EvpHash(wielders + 0x18, entity, 8, 4, &wi) || !EvpPtr(wielders + 0x38, &wieldersOf) ||
        EvpU32(wieldersOf + (uintptr_t)wi * 4, 0) != entity) { ApiLog(mod, "credit_to_player: entity %u is not a wielder of its own", entity); return 0; }
    if (!EvpPtr(game + 0x3326CB8u, &factions) || !EvpHash(factions + 0x12050, entity, 8, 4, &fi) || !EvpPtr(factions + 0x12078, &masks) ||
        !(EvpU32(masks + (uintptr_t)fi * 4, 0) & 1)) { ApiLog(mod, "credit_to_player: entity %u is not on the Helldivers' side", entity); return 0; }
    if (!EvpPtr(game + 0x3326740u, &behaviours) || !EvpHash(behaviours + 0x40, entity, 8, 4, &bi) || !EvpPtr(behaviours + 0x58, &handles) ||
        !EvpPtr(handles + (uintptr_t)bi * 8, &handle) || EvpU32(handle + 8, 0) != entity || EvpU32(handle + 0x10, 0x7FFF) == 0x7FFF)
    { ApiLog(mod, "credit_to_player: entity %u has no network object", entity); return 0; }
    if (!EvpPtr(game + 0x3326430u, &tags) || !EvpHash(tags + 0x18, entity, 8, 4, &ti) || !EvpPtr(tags + 0x38, &tagValues))
    { ApiLog(mod, "credit_to_player: entity %u has no tags", entity); return 0; }
    uintptr_t at = tagValues + (uintptr_t)ti * 8;
    uint64_t tag;
    if (SafeRead(at, &tag, 8)) return 0;
    if (!(tag & 0x10000000ull)) return HF_ACT_DONE;
    /* committed private read-write data, the whole word inside its region (perks.c's check) */
    if (!PerkWritable(at, 8)) { ApiLog(mod, "credit_to_player: its tags are not private data"); return 0; }
    *(volatile uint64_t*)at = tag & ~0x10000000ull;
    ApiLog(mod, "credit_to_player: entity %u's kills are credited to its owner", entity);
    return HF_ACT_DONE;
}

static int ActTimerNow(HfMod* mod, uintptr_t game, int frozen)
{
    static const ActPin pins[] = {{0, 0x94F265u, "4180bc864e19050000"}, {0, 0x94F26Eu, "0f855f0b0000"}, {0, 0x94F295u, "f3430f1184ae48190500"},
        {0, 0x95F389u, "4488bc8e4e190500"}, {0, 0x95F8B3u, "8b817c060000"}, {0, 0x95F8F6u, "488b84deb0060000"}};
    int host = 0;
    uint32_t mode = 0;
    if (!ActMission(game, &host, &mode) || !host) { ApiLog(mod, "freeze_mission_timer: only the host, in a mission"); return 0; }
    if (!ActPins(game, pins, sizeof pins / sizeof pins[0])) { ApiLog(mod, "freeze_mission_timer: the mission timer's code is not the reviewed one"); return 0; }
    uintptr_t manager, director;
    if (!EvpPtr(game + 0x3326D10u, &manager) || EvpU32(manager + 0x67C, 0) != 1 || !EvpPtr(manager + 0x6B0, &director) || EvpU32(director + 8, 0) != mode)
    { ApiLog(mod, "freeze_mission_timer: this mission has no single timer"); return 0; }
    uintptr_t slot = manager + 0x51948;
    uint8_t expired = 0, pause = 0, want = frozen ? 1 : 0;
    if (SafeRead(slot + 4, &expired, 1) || SafeRead(slot + 6, &pause, 1) || pause > 1) return 0;
    if (frozen && expired) { ApiLog(mod, "freeze_mission_timer: the timer has run out"); return 0; }
    if (pause != want) *(volatile uint8_t*)(slot + 6) = want;
    return HF_ACT_DONE;
}

/* ---- Pelicans ---------------------------------------------------------------------------------------------------- */

#define ACT_PELICANS 8
typedef struct ActPelican { _Alignas(16) uint8_t context[0x8A0]; uint32_t entity; float hover; int held; ULONGLONG since; } ActPelican;
static ActPelican g_actPelicans[ACT_PELICANS];
static int g_actPelicanNext;

/* The Pelican's facing on the ground, normalized: the call's, else from this machine's Helldiver to the hover point,
 * else north. */
static void ActPelicanFacing(uintptr_t game, const float* f, float* fx, float* fy)
{
    if (*fx == 0 && *fy == 0)
    {
        ActHealth h; const char* why; float from[3];
        if (ActLocalAvatar(game, 1, &h, &why) && EvpUnitPosition(h.unit, from)) { *fx = f[0] - from[0]; *fy = f[1] - from[1]; }
        if (*fx == 0 && *fy == 0) *fy = 1;
    }
    float length = sqrtf(*fx * *fx + *fy * *fy);
    *fx /= length; *fy /= length;
}
/* The world's spawn context as reviewed, and the transport Pelican registered in this mission. 0 logged. */
static int ActPelicanLoaded(HfMod* mod, uintptr_t game)
{
    uintptr_t world, table;
    static const uint8_t zero[0x8A0];
    uint8_t defaults[0x8A0];
    if (!EvpPtr(game + EVP_ENTITIES, &world) || SafeRead(world + 0xF3EF28, defaults, sizeof defaults) || memcmp(defaults, zero, sizeof zero))
    { ApiLog(mod, "spawn_pelican: the world's spawn context is not the reviewed one"); return 0; }
    /* the Pelican must be registered: 28 slots {u64 resource, u32 index}, from resource % 28 */
    int registered = 0;
    if (EvpPtr(world + 0xF12898, &table))
        for (uint32_t i = 0, at = (uint32_t)(ACT_PELICAN_RESOURCE % 28); i < 28; i++, at = (at + 1) % 28)
        {
            uint64_t r;
            if (SafeRead(table + (uintptr_t)at * 16, &r, 8) || r == 0) break;
            if (r == ACT_PELICAN_RESOURCE) { registered = 1; break; }
        }
    if (!registered) { ApiLog(mod, "spawn_pelican: the transport Pelican is not loaded in this mission"); return 0; }
    return 1;
}
static int ActPelicansOut(void)
{
    int alive = 0;
    for (int i = 0; i < ACT_PELICANS; i++) if (g_actPelicans[i].entity && EvpAlive(g_actPelicans[i].entity)) alive++;
    return alive;
}
/* The next free Pelican slot (its entity gone), round from the last one taken; NULL none. */
static ActPelican* ActPelicanSlot(void)
{
    for (int n = 0; n < ACT_PELICANS; n++)
    {
        ActPelican* c = &g_actPelicans[(g_actPelicanNext + n) % ACT_PELICANS];
        if (!c->entity || !EvpAlive(c->entity)) { g_actPelicanNext = (g_actPelicanNext + n + 1) % ACT_PELICANS; return c; }
    }
    return NULL;
}
static int ActPelicanNow(HfMod* mod, uintptr_t game, const float* f, uint32_t* entity)
{
    int host = 0;
    if (entity) *entity = 0;
    if (!ActIsPosition(f)) { ApiLog(mod, "spawn_pelican: not a position"); return 0; }
    float hover = f[3], distance = f[4] > 0 ? f[4] : 250.0f, height = f[5] > 0 ? f[5] : 80.0f, fx = f[6], fy = f[7];
    if (!(hover >= 0.0f) || hover > 120.0f || distance > 1000.0f || height > 500.0f) { ApiLog(mod, "spawn_pelican: hover 0 .. 120 s, approach up to 1000 m back and 500 m up"); return 0; }
    if (!ActMission(game, &host, NULL) || !host) { ApiLog(mod, "spawn_pelican: only the host, in a mission"); return 0; }
    ActPelicanFacing(game, f, &fx, &fy);
    uintptr_t fn = ActFunction(mod, game, &g_actPelican, "spawn_pelican");
    if (!fn) return 0;
    if (!ActPelicanLoaded(mod, game)) return 0;
    if (ActPelicansOut() >= 4) { ApiLog(mod, "spawn_pelican: four Pelicans are out already"); return 0; }
    ActPelican* p = ActPelicanSlot();
    if (p == NULL) return 0;
    memset(p, 0, sizeof *p);
    memcpy(p->context + 0x610, f, 12); /* the hover point */
    _Alignas(8) uint8_t d[0x58];
    memset(d, 0, sizeof d);
    d[0] = 1;
    uint32_t none = 0x7FFF; memcpy(d + 4, &none, 4);
    float pose[16] = {fy, -fx, 0, 0, fx, fy, 0, 0, 0, 0, 1, 0, f[0] - fx * distance, f[1] - fy * distance, f[2] + height, 1};
    memcpy(d + 8, pose, sizeof pose);
    void* context = p->context;
    memcpy(d + 0x48, &context, 8);
    uint32_t out = 0;
    ((uint32_t* (*)(uint32_t*, uint64_t, void*))fn)(&out, ACT_PELICAN_RESOURCE, d);
    if (!out || !EvpAlive(out)) { ApiLog(mod, "spawn_pelican: the game made no Pelican"); return 0; }
    p->entity = out; p->hover = hover; p->since = GetTickCount64();
    if (entity) *entity = out;
    ApiLog(mod, "spawn_pelican: Pelican %u over (%.1f, %.1f, %.1f), %.0f s hover", out, f[0], f[1], f[2], hover);
    return HF_ACT_DONE;
}

/* A Pelican's behaviour record (*b) and the game's clock now (*now). */
static int ActPelicanBehaviour(uintptr_t game, uint32_t entity, uintptr_t* b, uint64_t* now)
{
    uintptr_t behaviours, records, clock;
    uint32_t index;
    if (!EvpPtr(game + 0x3326740u, &behaviours) || !EvpHash(behaviours + 0x40, entity, 8, 4, &index) || !EvpPtr(behaviours + 0x60, &records) ||
        !EvpPtr(game + 0x3326348u, &clock) || SafeRead(clock + 0x18, now, 8)) return 0;
    *b = records + (uintptr_t)index * 0x1F8 + 8;
    return 1;
}
/* How long a Pelican's behaviour waits after its release at its stage, in seconds; 0 none. */
static double ActPelicanWait(uintptr_t b)
{
    uint32_t stage = EvpU32(b, 0), flags = EvpU32(b + 0x1E0, 0);
    return stage == 6 && (flags & 1) ? 0.6 : stage == 7 ? 3.0 : 0;
}
static void ActPelicanHold(uintptr_t game, ActPelican* p)
{
    if (!p->entity || p->held || p->hover <= 0) return;
    if (!EvpAlive(p->entity) || GetTickCount64() - p->since > 600000) { p->entity = 0; return; }
    uintptr_t b;
    uint64_t now;
    if (!ActPelicanBehaviour(game, p->entity, &b, &now)) return;
    double wait = ActPelicanWait(b);
    if (wait == 0) return;
    uint64_t release;
    if (SafeRead(b + 0x178, &release, 8) || !(release <= now && now < release + (uint64_t)(wait * 1e6))) return;
    p->held = 1;
    if (ActPlayerCount(game) != 1) { Note("pelican %u: hover not held (solo only)", p->entity); return; }
    uint64_t departure = release + (uint64_t)(p->hover * 1e6), moved = departure - (uint64_t)(wait * 1e6);
    if (departure <= release + (uint64_t)(wait * 1e6) || departure - now > 120000000ull) return;
    *(volatile uint64_t*)(b + 0x178) = moved;
    Note("pelican %u: holds %.0f s", p->entity, p->hover);
}
/* Each update: a released Pelican's departure moved for its hover (once; solo). */
static void ActPelicansTick(uintptr_t game)
{
    for (int i = 0; i < ACT_PELICANS; i++) ActPelicanHold(game, &g_actPelicans[i]);
}

/* ---- shots --------------------------------------------------------------------------------------------------------- */

#define ACT_RULES 64
#define ACT_SOURCES 8
#define ACT_TRACKED 16
typedef struct ActRule
{
    uint32_t id, projectile, homing, flags;
    HfMod* mod;
    uint64_t sources[ACT_SOURCES];
    int sourceCount;
    float damage, penetration, speed, gravity, drag;   /* modify_shots */
    float turn, cone, range, arm, aim;                 /* homing (turn in radians a second, cone its cosine) */
} ActRule;
static ActRule g_actRules[ACT_RULES];
static int g_actRuleCount;
static uint32_t g_actRuleId;
static SRWLOCK g_actRulesLock = SRWLOCK_INIT;

typedef struct ActTracked { int slot; uint32_t type, target, rule; uint64_t creditor; float distance; ULONGLONG since, retarget; } ActTracked;
static ActTracked g_actTracked[ACT_TRACKED];
static int g_actTrackedCount;
static uint32_t g_actCounter;
static uintptr_t g_actPool;
static int g_actPoolHeld;

static uint32_t ActAddRule(HfMod* mod, const ActRule* rule, const uint64_t* sources, const char* what)
{
    AcquireSRWLockExclusive(&g_actRulesLock);
    uint32_t id = 0;
    int taken = 0;
    for (int i = 0; i < g_actRuleCount; i++) if (g_actRules[i].projectile == rule->projectile && g_actRules[i].homing == rule->homing) taken = 1;
    if (taken) ApiLog(mod, "%s: projectile %u has a rule of this kind already", what, rule->projectile);
    else if (g_actRuleCount == ACT_RULES) ApiLog(mod, "%s: %d rules already", what, ACT_RULES);
    else
    {
        ActRule* r = &g_actRules[g_actRuleCount++];
        *r = *rule;
        r->mod = mod;
        r->id = id = ++g_actRuleId;
        for (r->sourceCount = 0; sources != NULL && sources[r->sourceCount] != 0 && r->sourceCount < ACT_SOURCES; r->sourceCount++)
            r->sources[r->sourceCount] = sources[r->sourceCount];
        ApiLog(mod, "%s: rule %u for projectile %u", what, id, rule->projectile);
    }
    ReleaseSRWLockExclusive(&g_actRulesLock);
    return id;
}
static int InRange(float v, float lo, float hi) { return isfinite(v) && v >= lo && v <= hi; }
static uint32_t ApiModifyShots(HfMod* mod, const HfShotRule* r)
{
    if (mod == NULL || r == NULL || r->size < sizeof *r) return 0;
    if (!InRange(r->damage, 0.01f, 100) || !InRange(r->armor_penetration, 0.1f, 10) || !InRange(r->speed, 0.1f, 10) || !InRange(r->gravity, 0, 20) ||
        !InRange(r->drag, 0, 20)) { ApiLog(mod, "modify_shots: a multiplier is outside its range"); return 0; }
    ActRule rule = {0, r->projectile, 0, r->flags, mod, {0}, 0, r->damage, r->armor_penetration, r->speed, r->gravity, r->drag, 0, 0, 0, 0, 0};
    return ActAddRule(mod, &rule, r->sources, "modify_shots");
}
static uint32_t ApiHoming(HfMod* mod, const HfHoming* r)
{
    if (mod == NULL || r == NULL || r->size < sizeof *r) return 0;
    if (!InRange(r->turn_rate, 1, 1440) || !InRange(r->cone, 1, 180) || !InRange(r->range, 1, 500) || !InRange(r->arm_distance, 0, 100) ||
        !InRange(r->aim_height, -5, 10)) { ApiLog(mod, "homing: a setting is outside its range"); return 0; }
    const float rad = 3.14159265f / 180.0f;
    ActRule rule = {0, r->projectile, 1, r->flags, mod, {0}, 0, 1, 1, 1, 1, 1, r->turn_rate * rad, cosf(r->cone * rad), r->range, r->arm_distance, r->aim_height};
    return ActAddRule(mod, &rule, r->sources, "homing");
}
static int ApiStopShots(HfMod* mod, uint32_t id)
{
    int found = 0;
    AcquireSRWLockExclusive(&g_actRulesLock);
    for (int i = 0; i < g_actRuleCount; i++)
        if (g_actRules[i].id == id && g_actRules[i].mod == mod) { g_actRules[i] = g_actRules[--g_actRuleCount]; found = 1; break; }
    ReleaseSRWLockExclusive(&g_actRulesLock);
    if (!found) ApiLog(mod, "stop_shots: no rule %u of this mod's", id);
    return found;
}

static const uint64_t kActEnemies[] = {0x0002BA767DF856F3ull, 0x08E6FFC2474287BDull, 0x09FE0BE51A23396Cull, 0x0AB7B92B131C228Cull, 0x0ADC9F9173AD8E1Dull,
    0x10081ACEF6163EF6ull, 0x137988CEA16458F7ull, 0x16023E554FEE830Eull, 0x1897BDD32105D2DCull, 0x19E18B46EC55D94Aull, 0x1A7FCDFF98C664B0ull,
    0x1E66EE1F6F7FD00Eull, 0x1F0A91729C0004E0ull, 0x20B9C7734DAEAD65ull, 0x215CE160A17BE4CDull, 0x257D805CAA7E10C0ull, 0x282EB766C1FFA6A1ull,
    0x2CF3488C4845F8BDull, 0x30CF04B2EC8C9BD4ull, 0x30F2DEE2333F227Aull, 0x31BAE74D2F064D8Dull, 0x32541FC4EC7C9CDCull, 0x33B3E0CE8D179EECull,
    0x36AA99CCE5E60146ull, 0x3AFF5FD7D5450B99ull, 0x3D0E03E2D574E1CAull, 0x3E0537D606438FEAull, 0x4019623142351CB6ull, 0x426B3DF007BE9F18ull,
    0x44458A2C52B002FBull, 0x453FE22C634EB30Full, 0x49674794A165EDA8ull, 0x4E342031A9D5C4F9ull, 0x4E97FB073BDC7A4Bull, 0x51EEA86BF6997E4Eull,
    0x53D8919D7B8ABD67ull, 0x54E107DACF6929CBull, 0x57EED0EAC346CD9Dull, 0x58B2B86C11369241ull, 0x5CA832447445C0BAull, 0x5D142C3A73EBC634ull,
    0x6021E22338333D88ull, 0x604A794EC45BB820ull, 0x63DF3D07B7424588ull, 0x64090088502435DDull, 0x64BA5F030B114EC1ull, 0x672F7DA17F3BA34Aull,
    0x67DC32DCA4F02D33ull, 0x6B202392F4AB605Eull, 0x6DAB2EADF5D8B692ull, 0x728421351D440EBCull, 0x72A83E49CED6DB3Dull, 0x746A7F3BEDA32699ull,
    0x74E2285C01DA4F71ull, 0x78E1497571012C47ull, 0x7ACB4BD753B71DE0ull, 0x7B48CACDBACB3881ull, 0x7ECE5304F868F6B3ull, 0x82A87AD8D595B2BAull,
    0x843D18D4B5512B63ull, 0x856E9710E45E760Full, 0x883401AF2A98A5F6ull, 0x8FB47DD28B398CF7ull, 0x8FC2784308E9BA99ull, 0x8FF0A839830A7692ull,
    0x905809A4C28D8A45ull, 0x960B48A421A3FAAAull, 0x9647B00CC3A9D36Full, 0x965EAE5A51ACDD4Aull, 0x96BA14C9EBB49CE1ull, 0x9A8A3AAE287B230Cull,
    0x9D8827FED763650Eull, 0x9E2E17F2CCCCAFDDull, 0x9F57782F00E6ED20ull, 0xA05BD1EC67B3AC4Cull, 0xA1F37BF2A40FBDE4ull, 0xA35207C6F2150806ull,
    0xA381A11C07D3EB94ull, 0xA6A68D8AF177F3A1ull, 0xA71AAFD82C6EBC92ull, 0xAAB438596F5E8FD9ull, 0xABDB2E2A0479D8CAull, 0xAC60E78435098C9Dull,
    0xAE63E525853D7044ull, 0xAEAEF7A1851E6C9Dull, 0xAF0F9B3A163787A5ull, 0xAF8529D541954E88ull, 0xB2A6FA1E4284C7E6ull, 0xB5DBC0C240C921ADull,
    0xB8580CAC32214FC0ull, 0xB92435FBF60F0748ull, 0xB940C889D77F8219ull, 0xBC242702FB46B7E7ull, 0xBE39E313A1E46BB9ull, 0xBE743B2FAA3A6E26ull,
    0xC0DF2A134BD15AD5ull, 0xC2B9EB21B06F8378ull, 0xC626D2BB495A202Dull, 0xC6449FFD9EA3779Cull, 0xC9BCCCB0A54A82A4ull, 0xCBB1BA3366009C3Aull,
    0xCC188F0C80505C6Cull, 0xCC7022FDD172089Bull, 0xCCAE5264ACD591B7ull, 0xCD28A27A79BE53D5ull, 0xD37E8D120D2836E3ull, 0xD465D9C7F77A07CBull,
    0xD522FD4748D443A5ull, 0xD5792F6856B06BA4ull, 0xD63FCBFF0851B7AFull, 0xD8CBC4A807A6D035ull, 0xD9511E9F6BD62E3Full, 0xDA40BB347C7447F2ull,
    0xDAC7D53FE0749F5Full, 0xDB90077E76FAA025ull, 0xDB964631BE1CF501ull, 0xDCF8E74212FBEE3Bull, 0xDFBACBD977A948DCull, 0xE0353177F1329573ull,
    0xE2E62FDD39407FE3ull, 0xE683D2CA5618D74Aull, 0xE8F19A0AA958E46Dull, 0xEACEE39FA017B495ull, 0xEF04CB84D097A497ull, 0xEF570293245A17C2ull,
    0xF0B26FA9258128D3ull, 0xF1610AC48CDC5240ull, 0xF196069E7E14C497ull, 0xF22D027B37BEF107ull, 0xF540CA9D9D4A422Eull, 0xF66D0BAD8693779Aull,
    0xF79CD8BB654397DFull, 0xF8131632AA867107ull, 0xF8B5A81A86D5D4EBull, 0xF96257FCAE258CAAull, 0xFB9937035D652C43ull, 0xFEB484EE972469F7ull,
    0xFF5CC825B9571052ull}; /* HD2Runtime's enemy types (domains/event_entities.lua), sorted */
static int ActEnemy(uint64_t resource)
{
    size_t lo = 0, hi = sizeof kActEnemies / sizeof kActEnemies[0];
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (kActEnemies[mid] < resource) lo = mid + 1; else hi = mid; }
    return lo < sizeof kActEnemies / sizeof kActEnemies[0] && kActEnemies[lo] == resource;
}

/* Homing targets: enemies from the health records, scanned a part each update; or the other players' Helldivers. */
#define ACT_CANDIDATES 256
typedef struct ActCandidate { uint32_t entity, unit; float position[3]; ULONGLONG at; } ActCandidate;
static ActCandidate g_actCandidates[ACT_CANDIDATES];
static int g_actCandidateCount;
static uint32_t g_actScanSlot, g_actRefresh;

/* The health manager's hash buckets, descriptors and records, as one scan reads them. */
typedef struct ActHealthTable { uintptr_t buckets, descriptors, records; uint32_t empty, live; } ActHealthTable;
/* One bucket of the health records' hash (g_actScanSlot's): a living enemy joins the candidates, a dead one leaves. */
static void ActScanSlot(const ActHealthTable* t, ULONGLONG now)
{
    uint32_t bucket[2];
    if (SafeRead(t->buckets + (uintptr_t)g_actScanSlot * 8, bucket, 8) || bucket[0] == t->empty || bucket[1] >= t->live) return;
    uint32_t life = EvpU32(t->records + (uintptr_t)bucket[1] * 0x1B8 + 0x19C, 2);
    int at = -1;
    for (int c = 0; c < g_actCandidateCount; c++) if (g_actCandidates[c].entity == bucket[0]) at = c;
    if (life >= 2) { if (at >= 0) g_actCandidates[at] = g_actCandidates[--g_actCandidateCount]; return; }
    if (at >= 0) return;
    uintptr_t descriptor; uint8_t d[24];
    if (!EvpPtr(t->descriptors + (uintptr_t)bucket[1] * 8, &descriptor) || SafeRead(descriptor, d, sizeof d)) return;
    uint64_t resource; uint32_t named, unit;
    memcpy(&resource, d, 8); memcpy(&named, d + 8, 4); memcpy(&unit, d + 12, 4);
    if (named != bucket[0] || !ActEnemy(resource) || g_actCandidateCount >= ACT_CANDIDATES) return;
    ActCandidate* c = &g_actCandidates[g_actCandidateCount];
    *c = (ActCandidate){bucket[0], unit, {0}, 0};
    if (EvpUnitPosition(unit, c->position)) { c->at = now; g_actCandidateCount++; }
}
static void ActScanEnemies(uintptr_t game)
{
    uintptr_t manager;
    ActHealthTable t;
    if (!EvpPtr(game + EVP_HEALTH, &manager) || !EvpPtr(manager + 0x1030, &t.buckets) || !EvpPtr(manager + 0x1048, &t.descriptors) || !EvpPtr(manager + 0x1058, &t.records)) return;
    uint32_t capacity = EvpU32(manager + 0x1038, 0);
    t.empty = EvpU32(manager + 0x103C, 0); t.live = EvpU32(manager + 0x1020, 0);
    if (capacity == 0 || capacity > 65536) return;
    ULONGLONG now = GetTickCount64();
    for (int n = 0; n < 512; n++, g_actScanSlot = (g_actScanSlot + 1) % capacity)
    {
        if (g_actScanSlot >= capacity) g_actScanSlot = 0;
        ActScanSlot(&t, now);
    }
    for (int n = 0; n < 12 && g_actCandidateCount > 0; n++)
    {
        ActCandidate* c = &g_actCandidates[g_actRefresh++ % (uint32_t)g_actCandidateCount];
        if (!EvpAlive(c->entity) || !EvpUnitPosition(c->unit, c->position)) { *c = g_actCandidates[--g_actCandidateCount]; continue; }
        c->at = now;
    }
}

/* The other players' living Helldivers, as homing candidates seen now; how many. */
static int ActFriends(uintptr_t game, ActCandidate* friends, ULONGLONG now)
{
    EvpPlayer players[EVT_MAX_PLAYERS];
    int n = EvpPlayers(game, players), count = 0;
    for (int i = 0; i < n; i++)
    {
        ActHealth h;
        if (players[i].local || !players[i].avatar || !ActHealthOf(game, players[i].avatar, &h) || h.life != 0) continue;
        friends[count] = (ActCandidate){players[i].avatar, h.unit, {0}, now};
        if (EvpUnitPosition(h.unit, friends[count].position)) count++;
    }
    return count;
}
/* The candidate most in the shot's direction, inside the cone and range; 0 none. */
static uint32_t ActAcquire(uintptr_t game, const ActRule* r, const float* at, const float* v, float* aim)
{
    float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), best = r->cone;
    uint32_t found = 0;
    ULONGLONG now = GetTickCount64();
    ActCandidate friends[EVT_MAX_PLAYERS];
    int count = 0;
    const ActCandidate* list = g_actCandidates;
    if (r->flags & HF_HOMING_FRIENDLY) { count = ActFriends(game, friends, now); list = friends; }
    else count = g_actCandidateCount;
    for (int i = 0; i < count; i++)
    {
        const ActCandidate* c = &list[i];
        if (now - c->at > 500) continue;
        float to[3] = {c->position[0] - at[0], c->position[1] - at[1], c->position[2] + r->aim - at[2]};
        float distance = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        if (distance <= 0.5f || distance > r->range) continue;
        float cosine = (to[0] * v[0] + to[1] * v[1] + to[2] * v[2]) / (distance * speed);
        if (cosine >= best) { best = cosine; found = c->entity; memcpy(aim, c->position, 12); aim[2] += r->aim; }
    }
    return found;
}
static int ActAimOf(uintptr_t game, const ActRule* r, uint32_t target, float* aim)
{
    uint32_t unit = 0;
    if (!EvpAlive(target)) return 0;
    for (int c = 0; c < g_actCandidateCount; c++) if (g_actCandidates[c].entity == target) unit = g_actCandidates[c].unit;
    ActHealth h;
    if (!unit && ActHealthOf(game, target, &h)) unit = h.unit;
    if (!unit || !EvpUnitPosition(unit, aim)) return 0;
    aim[2] += r->aim;
    return 1;
}

/* Turns v towards `to` by at most `angle` radians, keeping its length. */
static void ActTurn(float* v, const float* to, float angle)
{
    float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), length = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (speed < 1e-3f || length < 1e-3f) return;
    float a[3] = {v[0] / speed, v[1] / speed, v[2] / speed}, b[3] = {to[0] / length, to[1] / length, to[2] / length};
    float cosine = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (cosine > 1) cosine = 1;
    if (cosine < -1) cosine = -1;
    float between = acosf(cosine);
    if (between < 0.05f * 3.14159265f / 180.0f) return;
    float t = angle >= between ? 1.0f : angle / between, s = sinf(between);
    float wa = s > 1e-4f ? sinf((1 - t) * between) / s : 1 - t, wb = s > 1e-4f ? sinf(t * between) / s : t;
    for (int k = 0; k < 3; k++) v[k] = (wa * a[k] + wb * b[k]) * speed;
}

static int ActSourceMatches(uintptr_t game, const ActRule* r, uint32_t source)
{
    if (r->sourceCount == 0) return 1;
    uint64_t resource = EvpEntityResource(game, source, NULL);
    for (int i = 0; i < r->sourceCount; i++) if (r->sources[i] == resource) return 1;
    return 0;
}

/* One update's view of the shots: the projectile pool, a copy of the rules taken under their lock, and who this machine
 * is. */
typedef struct ActShots { uintptr_t game, system; const ActRule* rules; int ruleCount, players; uint64_t peer; ULONGLONG now; } ActShots;

/* Whether the shot in `slot` is one this machine's player fired, as a new shot reads (*creditor its hit record's). */
static int ActShotOurs(const ActShots* s, int slot, uintptr_t flight, uintptr_t hit, uint64_t* creditor)
{
    uint16_t flags = 0;
    uint8_t requested = 1;
    return !(SafeRead(s->system + 0x203C + (uintptr_t)slot * 2, &flags, 2) || !(flags & 2) || SafeRead(hit, creditor, 8) || *creditor != s->peer ||
        s->peer == 0 || SafeRead(hit + 0xB9, &requested, 1) || requested || EvpU32(flight + 0x50, 1) != 0);
}
/* modify_shots: a new shot's damage and penetration multipliers and its flight values scaled, when they read plausibly. */
static void ActScaleShot(const ActRule* r, uintptr_t flight, uintptr_t hit)
{
    float f[12], m[2];
    if (SafeRead(flight, f, sizeof f) || SafeRead(hit + 0x34, m, sizeof m)) return;
    int plausible = m[0] > 0 && m[0] <= 1000 && m[1] > 0 && m[1] <= 1000 && f[11] > 0;
    for (int q = 0; q < 12; q++) if (!isfinite(f[q])) plausible = 0;
    if (!plausible) return;
    m[0] *= r->damage; m[1] *= r->penetration;
    f[3] *= r->speed; f[4] *= r->speed; f[5] *= r->speed; f[11] *= r->speed; f[7] *= r->gravity; f[10] *= r->drag;
    memcpy((void*)(hit + 0x34), m, sizeof m);
    memcpy((void*)(flight + 0x0C), f + 3, 12);
    memcpy((void*)(flight + 0x1C), f + 7, 4);
    memcpy((void*)(flight + 0x28), f + 10, 8);
}
/* A new shot of type `type` in `slot` against one rule: scaled by a modify_shots rule, tracked for a homing one. */
static void ActShotRule(const ActShots* s, const ActRule* r, int slot, uint32_t type)
{
    if (r->projectile != type) return;
    uint64_t creditor = 0;
    uintptr_t flight = s->system + 0x3040 + (uintptr_t)slot * 0x70, hit = s->system + 0x4D040 + (uintptr_t)slot * 0xC8;
    if (!ActShotOurs(s, slot, flight, hit, &creditor)) return;
    if (s->players > 1 && !(r->flags & HF_SHOT_MULTIPLAYER)) return;
    if (!ActSourceMatches(s->game, r, EvpU32(s->system + 0x3B040 + (uintptr_t)slot * 0x24 + 0x0C, 0))) return;
    if (!r->homing) ActScaleShot(r, flight, hit);
    else if (g_actTrackedCount < ACT_TRACKED)
        g_actTracked[g_actTrackedCount++] = (ActTracked){slot, type, 0, r->id, creditor, 0, s->now, 0};
}
/* The n shots fired since the last update, from `from` (the last 64 of them at most). */
static void ActNewShots(const ActShots* s, uint32_t from, uint32_t n)
{
    if (n > 64) { from += n - 64; n = 64; }
    for (uint32_t i = 0; i < n; i++)
    {
        int slot = (int)((from + i) & 0x7FF);
        uint32_t type = EvpU32(s->system + 0xE5040 + (uintptr_t)slot * 4, 0);
        for (int k = 0; k < s->ruleCount; k++) ActShotRule(s, &s->rules[k], slot, type);
    }
}

static const ActRule* ActRuleById(const ActShots* s, uint32_t id)
{
    const ActRule* r = NULL;
    for (int k = 0; k < s->ruleCount; k++) if (s->rules[k].id == id) r = &s->rules[k];
    return r;
}
/* Whether a tracked shot is still the one its rule tracks (f: its flight, read). */
static int ActTrackedLive(const ActShots* s, const ActTracked* t, const ActRule* r, float f[14])
{
    uintptr_t flight = s->system + 0x3040 + (uintptr_t)t->slot * 0x70, hit = s->system + 0x4D040 + (uintptr_t)t->slot * 0xC8;
    uint16_t flags = 0; uint64_t creditor = 0; uint8_t requested = 1;
    return r != NULL && s->now - t->since < 10000 && EvpU32(s->system + 0xE5040 + (uintptr_t)t->slot * 4, 0) == t->type &&
        !SafeRead(s->system + 0x203C + (uintptr_t)t->slot * 2, &flags, 2) && (flags & 2) && !(flags & 1) && !SafeRead(hit, &creditor, 8) &&
        creditor == t->creditor && !SafeRead(hit + 0xB9, &requested, 1) && !requested && !SafeRead(flight, f, 14 * sizeof f[0]) && f[12] >= t->distance;
}
/* A tracked shot past its arming distance turned towards its target (acquired again every 100 ms unless the rule keeps
 * the first). */
static void ActSteer(const ActShots* s, ActTracked* t, const ActRule* r, float f[14], float dt)
{
    uintptr_t flight = s->system + 0x3040 + (uintptr_t)t->slot * 0x70;
    float* v = f + 3;
    float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), aim[3];
    if (!(f[12] >= r->arm && speed >= 1.0f)) return;
    int have = t->target && ActAimOf(s->game, r, t->target, aim);
    if ((!have || !(r->flags & HF_HOMING_NO_RETARGET)) && s->now - t->retarget >= 100)
    {
        t->retarget = s->now;
        float found[3];
        uint32_t target = ActAcquire(s->game, r, f, v, found);
        if (target && (!have || !(r->flags & HF_HOMING_NO_RETARGET))) { t->target = target; memcpy(aim, found, 12); have = 1; }
    }
    if (have)
    {
        float to[3] = {aim[0] - f[0], aim[1] - f[1], aim[2] - f[2]};
        ActTurn(v, to, r->turn * (dt > 0 && dt < 0.25f ? dt : 1.0f / 60.0f));
        memcpy((void*)(flight + 0x0C), v, 12);
    }
}
/* The homing rules' pass: the enemies scanned when a rule homes on them, each tracked shot steered or dropped. */
static void ActHomingTick(const ActShots* s, float dt)
{
    int homing = 0, enemies = 0;
    for (int i = 0; i < s->ruleCount; i++) if (s->rules[i].homing) homing = 1;
    if (!homing) { g_actTrackedCount = 0; return; }
    for (int i = 0; i < s->ruleCount; i++) if (s->rules[i].homing && !(s->rules[i].flags & HF_HOMING_FRIENDLY)) enemies = 1;
    if (enemies && g_actTrackedCount > 0) ActScanEnemies(s->game);
    for (int i = 0; i < g_actTrackedCount;)
    {
        ActTracked* t = &g_actTracked[i];
        const ActRule* r = ActRuleById(s, t->rule);
        float f[14];
        if (!ActTrackedLive(s, t, r, f)) { *t = g_actTracked[--g_actTrackedCount]; continue; }
        t->distance = f[12];
        ActSteer(s, t, r, f, dt);
        i++;
    }
}

static void ActShotsTick(uintptr_t game, float dt)
{
    if (g_actRuleCount == 0) { g_actPoolHeld = 0; g_actTrackedCount = 0; return; }
    if (!ActMission(game, NULL, NULL)) { g_actPoolHeld = 0; g_actTrackedCount = 0; g_actCandidateCount = 0; return; }
    uintptr_t system;
    uint8_t active = 0;
    if (!EvpPtr(game + 0x347CEA8u, &system) || SafeRead(system + 0x28, &active, 1) || active != 1) return;
    uint32_t counter = EvpU32(system + 0x30, 0);
    if (!g_actPoolHeld || g_actPool != system) { g_actPool = system; g_actCounter = counter; g_actPoolHeld = 1; return; }
    uint32_t n = counter - g_actCounter, from = g_actCounter;
    g_actCounter = counter;
    static ActRule rules[ACT_RULES];
    AcquireSRWLockShared(&g_actRulesLock);
    int ruleCount = g_actRuleCount;
    memcpy(rules, g_actRules, sizeof rules[0] * ruleCount);
    ReleaseSRWLockShared(&g_actRulesLock);
    int players = ActPlayerCount(game);
    uint64_t peer = ActLocalPeer(game);
    ULONGLONG now = GetTickCount64();
    ActShots s = {game, system, rules, ruleCount, players, peer, now};
    if (n > 0 && n <= 2048) ActNewShots(&s, from, n);
    ActHomingTick(&s, dt);
}

/* ---- the game thread -------------------------------------------------------------------------------------------- */

enum { ACT_HEAL = 1, ACT_HEAL_LIMB, ACT_INJURE, ACT_STATUS, ACT_VELOCITY, ACT_RESUPPLY, ACT_EXPLOSION, ACT_PROJECTILE, ACT_CREDIT, ACT_TIMER, ACT_PELICAN };
typedef struct ActCall { int kind; HfMod* mod; uint32_t u[2]; int32_t i; float f[8]; } ActCall;
#define ACT_QUEUE 64
static ActCall g_actQueue[ACT_QUEUE];
static int g_actQueued;
static SRWLOCK g_actQueueLock = SRWLOCK_INIT;
static ULONGLONG g_actLastTick;

static int ActRun(const ActCall* c, uint32_t* entity)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (!game || strcmp(g_build, "25480438")) { ApiLog(c->mod, "actions: not on this game build"); return 0; }
    switch (c->kind)
    {
    case ACT_HEAL: return ActHealNow(c->mod, game, c->f[0]);
    case ACT_HEAL_LIMB: return ActHealLimbNow(c->mod, game, c->u[0]);
    case ACT_INJURE: return ActInjureNow(c->mod, game, c->u[0], c->i);
    case ACT_STATUS: return ActStatusNow(c->mod, game, c->u[0], c->u[1], c->f[0]);
    case ACT_VELOCITY: return ActVelocityNow(c->mod, game, c->f);
    case ACT_RESUPPLY: return ActResupplyNow(c->mod, game);
    case ACT_EXPLOSION: return ActExplosionNow(c->mod, game, c->u[0], c->f);
    case ACT_PROJECTILE: return ActProjectileNow(c->mod, game, c->u[0], c->f, c->f + 3);
    case ACT_CREDIT: return ActCreditNow(c->mod, game, c->u[0]);
    case ACT_TIMER: return ActTimerNow(c->mod, game, c->i);
    case ACT_PELICAN: return ActPelicanNow(c->mod, game, c->f, entity);
    }
    return 0;
}
/* On the game thread: now. Elsewhere: at the next game update (HF_ACT_QUEUED). */
static int ActDo(const ActCall* c, uint32_t* entity)
{
    if (c->mod == NULL) return 0;
    if (g_tickState < 0) { ApiLog(c->mod, "actions: the game thread's update is not hooked (modloader.log says why)"); return 0; }
    if (g_gameThread != 0 && GetCurrentThreadId() == g_gameThread) return ActRun(c, entity);
    AcquireSRWLockExclusive(&g_actQueueLock);
    int queued = g_actQueued < ACT_QUEUE;
    if (queued) g_actQueue[g_actQueued++] = *c;
    ReleaseSRWLockExclusive(&g_actQueueLock);
    if (!queued) { ApiLog(c->mod, "actions: %d are waiting for the game thread already", ACT_QUEUE); return 0; }
    return HF_ACT_QUEUED;
}

/* The game thread's update (EventsTick): queued actions, then the shot rules and Pelicans. */
static void ActionsTick(void)
{
    ULONGLONG now = GetTickCount64();
    float dt = g_actLastTick ? (float)(now - g_actLastTick) / 1000.0f : 0;
    g_actLastTick = now;
    if (g_actQueued)
    {
        ActCall pending[ACT_QUEUE];
        AcquireSRWLockExclusive(&g_actQueueLock);
        int n = g_actQueued;
        memcpy(pending, g_actQueue, sizeof pending[0] * n);
        g_actQueued = 0;
        ReleaseSRWLockExclusive(&g_actQueueLock);
        for (int i = 0; i < n; i++)
        {
            int result = ActRun(&pending[i], NULL);
            ApiLog(pending[i].mod, "action %d (queued): %s", pending[i].kind, result ? "done" : "refused");
        }
    }
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    if (!game || strcmp(g_build, "25480438")) return;
    ActShotsTick(game, dt);
    ActPelicansTick(game);
}

static int ApiHeal(HfMod* mod, float amount) { ActCall c = {ACT_HEAL, mod, {0}, 0, {amount}}; return ActDo(&c, NULL); }
static int ApiHealLimb(HfMod* mod, uint32_t limb) { ActCall c = {ACT_HEAL_LIMB, mod, {limb}, 0, {0}}; return ActDo(&c, NULL); }
static int ApiInjure(HfMod* mod, uint32_t limb, int32_t damage) { ActCall c = {ACT_INJURE, mod, {limb}, damage, {0}}; return ActDo(&c, NULL); }
static int ApiApplyStatus(HfMod* mod, uint32_t entity, uint32_t status, float buildup) { ActCall c = {ACT_STATUS, mod, {entity, status}, 0, {buildup}}; return ActDo(&c, NULL); }
static int ApiAddVelocity(HfMod* mod, const float delta[3])
{
    if (delta == NULL) return 0;
    ActCall c = {ACT_VELOCITY, mod, {0}, 0, {delta[0], delta[1], delta[2]}};
    return ActDo(&c, NULL);
}
static int ApiResupply(HfMod* mod) { ActCall c = {ACT_RESUPPLY, mod, {0}, 0, {0}}; return ActDo(&c, NULL); }
static int ApiSpawnExplosion(HfMod* mod, uint32_t type, const float position[3])
{
    if (position == NULL) return 0;
    ActCall c = {ACT_EXPLOSION, mod, {type}, 0, {position[0], position[1], position[2]}};
    return ActDo(&c, NULL);
}
static int ApiSpawnProjectile(HfMod* mod, uint32_t type, const float position[3], const float direction[3])
{
    if (position == NULL || direction == NULL) return 0;
    ActCall c = {ACT_PROJECTILE, mod, {type}, 0, {position[0], position[1], position[2], direction[0], direction[1], direction[2]}};
    return ActDo(&c, NULL);
}
static int ApiCreditToPlayer(HfMod* mod, uint32_t entity) { ActCall c = {ACT_CREDIT, mod, {entity}, 0, {0}}; return ActDo(&c, NULL); }
static int ApiFreezeTimer(HfMod* mod, int frozen) { ActCall c = {ACT_TIMER, mod, {0}, frozen != 0, {0}}; return ActDo(&c, NULL); }
static int ApiSpawnPelican(HfMod* mod, const HfPelicanCall* call, uint32_t* entity)
{
    if (call == NULL || call->size < sizeof *call) return 0;
    ActCall c = {ACT_PELICAN, mod, {0}, 0, {call->position[0], call->position[1], call->position[2], call->hover_seconds, call->approach_distance,
        call->approach_height, call->facing[0], call->facing[1]}};
    return ActDo(&c, entity);
}
