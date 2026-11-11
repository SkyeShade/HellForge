/* The managers' fields a mod may move (moved_fields.c), on a synthetic game.dll: each field read where its instructions
 * address it, in the vanilla layout and in the one Complex Operations' eight-player build makes. A site that disagrees,
 * an implausible value and another instruction are refused; the pins over moved displacements still match; the local
 * player, the player events, the Helldiver an action finds, the Supply Pack's action context and the stats record are
 * found at the moved places. Included by stats_test.c. */

/* Every moved field's sites written into a fake game image, each field at at[field] (its vanilla value where NULL). */
static void MfStage(uint8_t* game, const int32_t* at)
{
    for (size_t i = 0; i < MF_SITES; i++)
    {
        const MfSite* s = &kMfSites[i];
        int32_t v = (at ? at[s->field] : kMfFields[s->field].vanilla) + s->add;
        memcpy(game + s->rva, s->code, s->size);
        if (s->width == 1) game[s->rva + s->size] = (uint8_t)v;
        else memcpy(game + s->rva + s->size, &v, 4);
    }
}

/* The vanilla layout, or the eight-player build's: the lists after the managers' vanilla ends, capacities 8. */
static void MfLayout(int32_t* at, int eight)
{
    for (int f = 0; f < MF_FIELDS; f++) at[f] = kMfFields[f].vanilla;
    if (!eight) return;
    at[MF_PLAYER_PEERS] = 0x828; at[MF_PLAYER_LIFE] = 0x840; at[MF_PLAYER_CAP] = 8;
    at[MF_CUSTOM_APPLIED] = 0xC88; at[MF_CUSTOM_CAP] = 8; at[MF_CUSTOM_BLOCK] = -0x123450;
    at[MF_AVATAR_CONTEXT] = 0xFC4F50; at[MF_AVATAR_PLAYER_OBJECT] = 0xFD8604; at[MF_SCORE_CAP] = 8;
}

/* A u32 map {buckets, capacity, empty, multiplier} at `header` holding key -> value (8 buckets, multiplier 1). */
static void MfMap(uint8_t* header, uint32_t (*buckets)[2], uint32_t key, uint32_t value)
{
    for (int i = 0; i < 8; i++) { buckets[i][0] = 0xFFFFFFFFu; buckets[i][1] = 0; }
    buckets[key % 8][0] = key; buckets[key % 8][1] = value;
    uintptr_t b = (uintptr_t)buckets; uint32_t h[3] = {8, 0xFFFFFFFFu, 1};
    memcpy(header, &b, 8); memcpy(header + 8, h, 12);
}

static int g_mfHeard;
static HfPlayerEvent g_mfLast;
static void MfHeard(HfMod* mod, const HfEventInfo* event, void* user) { g_mfHeard++; if (((const HfPlayerEvent*)event)->local) g_mfLast = *(const HfPlayerEvent*)event; }

/* The fields themselves, and the pins over them, in both layouts; refusals. */
static void MfFieldsCheck(uint8_t* game)
{
    int32_t at[MF_FIELDS], v;
    for (size_t i = 0; i < sizeof kPassivePins / sizeof kPassivePins[0]; i++) memcpy(game + kPassivePins[i].rva, kPassivePins[i].bytes, kPassivePins[i].size);
    for (size_t i = 0; i < sizeof kPerkPins / sizeof kPerkPins[0]; i++) memcpy(game + kPerkPins[i].rva, kPerkPins[i].bytes, kPerkPins[i].size);
    for (size_t i = 0; i < sizeof kEvpPlayerPins / sizeof kEvpPlayerPins[0]; i++) memcpy(game + kEvpPlayerPins[i].rva, kEvpPlayerPins[i].bytes, kEvpPlayerPins[i].size);
    for (size_t i = 0; i < sizeof kEvpStatPins / sizeof kEvpStatPins[0]; i++) memcpy(game + kEvpStatPins[i].rva, kEvpStatPins[i].bytes, kEvpStatPins[i].size);
    static const uint8_t resupply[] = {0x42, 0x8b, 0x84, 0x10, 0x88, 0xe8, 0x53, 0x00};
    memcpy(game + 0xA431B1u, resupply, sizeof resupply);
    uintptr_t g = (uintptr_t)game;
    for (int eight = 0; eight < 2; eight++)
    {
        MfLayout(at, eight);
        MfStage(game, at);
        for (int f = 0; f < MF_FIELDS; f++) assert(MfFieldOf(g, f, &v) && v == at[f]);
        g_passivesProven = 0; g_perksProven = 0;
        assert(PassivesProve(g) && PerksProve(g)); /* the pins match with the displacements moved */
        assert(EvpProve(g, 0, kEvpPlayerPins, sizeof kEvpPlayerPins / sizeof kEvpPlayerPins[0]) &&
            EvpProve(g, 0, kEvpStatPins, sizeof kEvpStatPins / sizeof kEvpStatPins[0]));
        assert(ActBytes(g + 0xA431B1u, "428b841088e85300", 0xA431B1u));
    }
    /* a site that disagrees with its anchor: the field and the pins over it are refused */
    int32_t disagree = 0x830;
    memcpy(game + 0x62C72Bu + 8, &disagree, 4);
    EvpPlayer players[EVT_MAX_PLAYERS];
    assert(!MfFieldOf(g, MF_PLAYER_PEERS, &v) && EvpPlayers(g, players) == -1);
    MfStage(game, at);
    v = 0xC88 + 0x38 + 4; memcpy(game + 0x11D9FDAu + 4, &v, 4); /* the helmet passive's read, 4 bytes off */
    g_passivesProven = 0; g_perksProven = 0;
    assert(!PassivesProve(g) && !PerksProve(g));
    MfStage(game, at);
    /* implausible: a misaligned list, a capacity past HellForge's arrays, a field past 64 MB */
    at[MF_PLAYER_PEERS] = 0x82C; MfStage(game, at); assert(!MfFieldOf(g, MF_PLAYER_PEERS, &v)); at[MF_PLAYER_PEERS] = 0x828;
    at[MF_PLAYER_CAP] = 17; MfStage(game, at); assert(!MfFieldOf(g, MF_PLAYER_CAP, &v)); at[MF_PLAYER_CAP] = 8;
    at[MF_AVATAR_CONTEXT] = 0x5000000; MfStage(game, at); assert(!MfFieldOf(g, MF_AVATAR_CONTEXT, &v)); at[MF_AVATAR_CONTEXT] = 0xFC4F50;
    /* another instruction where an anchor was: refused, and so is a pin over the field */
    MfStage(game, at);
    game[0x5406C8u] ^= 1;
    g_passivesProven = 0;
    assert(!MfFieldOf(g, MF_CUSTOM_APPLIED, &v) && !PassivesProve(g));
    game[0x5406C8u] ^= 1;
    /* a pin's own bytes outside the open displacement still count */
    assert(!ActBytes(g + 0xA431B1u, "438b841088e85300", 0xA431B1u));
    g_passivesProven = 0; g_perksProven = 0;
}

/* The player list, its events, the local Helldiver, its action context and its stats record in one layout. */
static void MfPlayersCheck(uint8_t* game, uint8_t* exe, int eight)
{
    int32_t at[MF_FIELDS];
    MfLayout(at, eight);
    MfStage(game, at);
    uintptr_t g = (uintptr_t)game;
    uint32_t count = eight ? 6 : 3, local = count - 1, avatar = 0x400005u, player = 0x500000u + local;
    uint8_t* manager = (uint8_t*)calloc(1, 0x1000);
    uint8_t* user = (uint8_t*)calloc(1, EVT_LOCAL_PEER_AT + 16);
    uint8_t descriptors[8][16] = {{0}};
    memcpy(manager + at[MF_PLAYER_COUNT], &count, 4);
    for (uint32_t i = 0; i < count; i++)
    {
        uint64_t peer = 0x1000u + i, stale = 0xDEADu; uint32_t life = 3, entity = 0x500000u + i, network = 0x100u + i;
        uintptr_t d = (uintptr_t)descriptors[i];
        memcpy(descriptors[i] + 8, &entity, 4);
        memcpy(manager + at[MF_PLAYER_PEERS] + 0x38 * i, &peer, 8);
        memcpy(manager + at[MF_PLAYER_LIFE] + 0x38 * i, &life, 4);
        memcpy(manager + at[MF_PLAYER_DESCRIPTORS] + 8 * i, &d, 8);
        memcpy(manager + at[MF_PLAYER_NETWORK] + 0x20 * i, &network, 4);
        if (eight && i < 4) memcpy(manager + 0x2C8 + 0x38 * i, &stale, 8); /* the old place: dead space a reader must not take */
    }
    uint64_t localPeer = 0x1000u + local;
    memcpy(user + EVT_LOCAL_PEER_AT, &localPeer, 8);
    uintptr_t p = (uintptr_t)manager; memcpy(game + EVT_PLAYERS, &p, 8);
    p = (uintptr_t)user; memcpy(game + EVT_LOCAL_PEER, &p, 8);
    /* the entity manager's network map: the local player's network id -> slot 5 -> its Helldiver */
    uint8_t* em = (uint8_t*)calloc(1, EVP_NETWORK_ENTITY + 8 * 24);
    uint32_t netBuckets[8][2], slot = 5;
    MfMap(em + EVP_NETWORK_MAP, netBuckets, 0x100u + local, slot);
    memcpy(em + EVP_NETWORK_ENTITY + slot * 24, &avatar, 4);
    p = (uintptr_t)em; memcpy(game + EVP_ENTITIES, &p, 8);

    EvpPlayer players[EVT_MAX_PLAYERS];
    assert(EvpPlayers(g, players) == (int)count && players[local].local && players[local].peer == localPeer && players[local].player == player &&
        players[local].avatar == avatar && players[local].life == 3 && !players[0].local);
    /* the events: every player seen spawned once, the local one marked */
    HfMod* mod = &g_mods[0];
    int loaded = mod->loaded;
    mod->loaded = 1;
    g_listenerCount = 0; g_listening = 0; g_playerCount = 0; g_mfHeard = 0; memset(&g_mfLast, 0, sizeof g_mfLast);
    assert(ApiSubscribe(mod, HF_ON_PLAYER_SPAWN, MfHeard, NULL, 0));
    PollPlayers(g);
    assert(g_mfHeard == (int)count && g_mfLast.peer == localPeer && g_mfLast.entity == player);
    g_listenerCount = 0; g_listening = 0; g_playerCount = 0; mod->loaded = loaded;

    /* an action's Helldiver: alive in the engine, its health record a Helldiver it owns */
    uint8_t* entities = (uint8_t*)calloc(1, 0x100);
    uint8_t gens[64] = {0}; gens[5] = 1;
    uint32_t size = 64; memcpy(entities + 0x80, &size, 4);
    p = (uintptr_t)gens; memcpy(entities + 0x88, &p, 8);
    p = (uintptr_t)entities; memcpy(exe + EVP_EXE_ENTITIES, &p, 8);
    g_evpExe = (uintptr_t)exe;
    uint8_t* health = (uint8_t*)calloc(1, 0x1100);
    uint32_t healthBuckets[8][2], index = 1, four = 4;
    MfMap(health + 0x1030, healthBuckets, avatar, index);
    memcpy(health + 0x1020, &four, 4);
    uint8_t avatarDescriptor[24] = {0}, records[4][0x1B8];
    memset(records, 0, sizeof records);
    uint64_t resource = ACT_AVATAR; memcpy(avatarDescriptor, &resource, 8); memcpy(avatarDescriptor + 8, &avatar, 4);
    uint32_t unit = 0x77, network = 0x100u + local; memcpy(avatarDescriptor + 12, &unit, 4); memcpy(avatarDescriptor + 16, &network, 4);
    avatarDescriptor[20] = 1;
    uintptr_t healthDescriptors[4] = {0, (uintptr_t)avatarDescriptor, 0, 0};
    p = (uintptr_t)healthDescriptors; memcpy(health + 0x1048, &p, 8);
    p = (uintptr_t)records; memcpy(health + 0x1058, &p, 8);
    int32_t hp = 100; memcpy(records[index] + 0x14, &hp, 4);
    p = (uintptr_t)health; memcpy(game + EVP_HEALTH, &p, 8);
    ActHealth h; const char* why = NULL;
    assert(ActLocalAvatar(g, 0, &h, &why) == avatar && h.health == 100);

    /* the Supply Pack's action context at its place, the old place holding another */
    size_t bytes = (size_t)at[MF_AVATAR_CONTEXT] + 4 * 0x1238;
    uint8_t* actions = (uint8_t*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint32_t actionBuckets[8][2], ai = 2, none = 77, actionDescriptor[4] = {0, 0, avatar, 0};
    MfMap(actions + at[MF_AVATAR_MAP], actionBuckets, avatar, ai);
    p = (uintptr_t)actionDescriptor; memcpy(actions + at[MF_AVATAR_DESCRIPTORS] + 8 * ai, &p, 8);
    uint8_t* context = actions + at[MF_AVATAR_CONTEXT] + 0x1238 * ai;
    uint64_t f0 = 2, busy = 1ull << 21;
    memcpy(context, &avatar, 4); memcpy(context + 8, &none, 4); memcpy(context + 0x710, &f0, 8);
    if (eight) { uint8_t* old = actions + 0x53E170 + 0x1238 * ai; memcpy(old, &avatar, 4); memcpy(old + 0x718, &busy, 8); }
    memcpy(game + 0x3483C5Cu, &none, 4);
    p = (uintptr_t)actions; memcpy(game + 0x3326D20u, &p, 8);
    uint32_t got = 0;
    assert(ActFreeContext(mod, g, avatar, &got) == (uintptr_t)context && got == none);
    memcpy(context + 0x718, &busy, 8);
    assert(ActFreeContext(mod, g, avatar, &got) == 0); /* busy */

    /* the stats record at index 5 of eight (the capacity followed), past a vanilla capacity of 4 */
    uint8_t* score = (uint8_t*)calloc(1, 0x38100);
    uint32_t scoreBuckets[8][2], statBuckets[4][5], scoreIndex = eight ? 5 : 2;
    MfMap(score + at[MF_SCORE_MAP], scoreBuckets, player, scoreIndex);
    memset(statBuckets, 0xFF, sizeof statBuckets);
    uint8_t* record = score + at[MF_SCORE_RECORDS] + 0x28 * scoreIndex;
    p = (uintptr_t)statBuckets; uint32_t statHeader[3] = {4, 0xFFFFFFFFu, 1};
    memcpy(record, &p, 8); memcpy(record + 8, statHeader, 12);
    p = (uintptr_t)score; memcpy(game + EVP_SCORE, &p, 8);
    static EvpStats stats;
    assert(EvpStatsOf(g, player, &stats));
    if (eight) { at[MF_SCORE_CAP] = 4; MfStage(game, at); assert(!EvpStatsOf(g, player, &stats)); }

    memset(game + EVT_PLAYERS, 0, 8); memset(game + EVT_LOCAL_PEER, 0, 8); memset(game + EVP_ENTITIES, 0, 8); memset(game + EVP_HEALTH, 0, 8);
    memset(game + 0x3326D20u, 0, 8); memset(game + EVP_SCORE, 0, 8); memset(exe + EVP_EXE_ENTITIES, 0, 8);
    g_evpExe = 0;
    free(score); VirtualFree(actions, 0, MEM_RELEASE); free(health); free(entities); free(em); free(user); free(manager);
}

static void MovedFieldsFixture(void)
{
    uint8_t* game = (uint8_t*)VirtualAlloc(NULL, 0x3500000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* exe = (uint8_t*)VirtualAlloc(NULL, EVP_EXE_ENTITIES + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(game && exe);
    MfFieldsCheck(game);
    MfPlayersCheck(game, exe, 0);
    MfPlayersCheck(game, exe, 1);
    VirtualFree(exe, 0, MEM_RELEASE); VirtualFree(game, 0, MEM_RELEASE);
    puts("PASS moved fields: the managers' fields read where the game addresses them, vanilla and moved (eight players); a "
        "disagreeing site, implausible values and other code refused; pins open over moved displacements; the local player, "
        "its events, its Helldiver, its action context and its stats record found at the moved places");
}
