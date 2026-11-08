/* Living world, Phase 2a: squads investigate noise (KB living-world-plan "Phase 2 design", living-world-p2-noise,
 * living-world-p2-contact; host only, included by living_world.c).
 *
 * The noise dispatch 0x64A890(_, u8 type, u32 source entity, const float pos[3], u32 mode, float loudness, u32 warn)
 * reaches only the ears this machine owns, so offline squads (a position, no units) never hear anything. Its detour
 * copies every emission into a ring and calls the original; it makes no game call. Every traced caller runs on the main
 * thread before the late tick (155 direct callers, none inside 0x64A891..0x64A89D; MinHook relocates the entry `je`).
 * The late tick drains the ring: a mode-0 noise from a unit on the Helldivers' side (the faction registry
 * [game+0x3326CB8]: hash +0x12050, masks [+0x12078] bit 0; a gunshot's source is the weapon's wielder, 0x612A18 ->
 * Wieldable [game+0x3326730]) opens an event, or refreshes one within 50 m and 10 s. Its reach is the template's range
 * (table 0x2140320: {range, intensity} per type) x loudness x LW_HEAR_FAR, at most LW_HEAR_MAX. Up to two calm PATROL
 * squads inside the reach, nearest first, turn INVESTIGATE (an online squad within the template's own range hears it
 * natively and is left alone).
 * - Offline: a route path to the node nearest the point; there it searches for 30-60 s, then patrols again.
 * - Online: the job position is the point snapped to the navmesh (0xA13520), and the squad repaths at once:
 *   0x5B36C0(_, squad, point) stores it at nav +0x60 and sets the repath byte +0x90 (P; it looks the squad up in the
 *   navigation component [game+0x3326418] without a miss check, so the loader checks the hash first). Within
 *   LW_HEAR_NEAR of the point each member is told: 0x888E50(NULL, its awareness detector (Behavior +0x68, LwAwareDetector),
 *   source, point, 40.0) writes the hearing entry a player's shot writes, so the awareness update runs the game's own investigation (alert 2, behaviour state 7, a
 *   point 3-9 m around the centre) and returns the member to its squad after its settings' timer. The intent ends when
 *   every member is back at alert 0 (at least 10 s after) or after 90 s; a member at alert 3 or more ends it at once
 *   (the fight is the game's). */
#define LW_HEAR_FAR 4.0f    /* reach = the template's range x loudness x this (D) */
#define LW_HEAR_MAX 600.0f  /* at most (D) */
#define LW_HEAR_NEAR 30.0f  /* online: members are told within this of the point (D) */
#define LW_HEAR_SQUADS 2    /* squads sent per noise event (D) */
#define LW_RETAKE_WAIT 120000    /* ms a squad waits at the garrison for players to leave (D) */
#define LW_INTENT_REST 30000 /* ms a squad patrols after an intent before the next (D) */

typedef struct { float pos[3], loudness; uint32_t source; uint8_t type, mode; ULONGLONG at; } LwNoise;
#define LW_NOISE_RING 256u
static LwNoise g_lwNoise[LW_NOISE_RING];
static volatile LONG g_lwNoiseHead; static LONG g_lwNoiseTail;
static volatile LONG g_lwNoiseOn;
typedef void (*LwNoiseFn)(void*, uint8_t, uint32_t, const float*, uint32_t, float, uint32_t);
static LwNoiseFn g_lwNoiseOrig;
static void LwNoiseDetour(void* unused, uint8_t type, uint32_t source, const float* pos, uint32_t mode, float loudness, uint32_t warn)
{
    if (pos && type != 0 && type < 30)
    {
        LONG h = g_lwNoiseHead;
        LwNoise* n = &g_lwNoise[(uint32_t)h % LW_NOISE_RING];
        n->pos[0] = pos[0]; n->pos[1] = pos[1]; n->pos[2] = pos[2]; n->loudness = loudness; n->source = source; n->type = type; n->mode = (uint8_t)(mode == 1); n->at = GetTickCount64();
        MemoryBarrier();
        g_lwNoiseHead = h + 1;
    }
    g_lwNoiseOrig(unused, type, source, pos, mode, loudness, warn);
}
static void LwInstallNoise(void)
{
    if (!LwEnabled() || g_lwNoiseOn) return;
    uintptr_t game = g_lw.game;
    if (!ApiCodeMatches(game + 0x64A890u, "84 D2 0F 84 AD 06 00 00 4C 8B DC 57 41 56") || !ApiCodeMatches(game + 0x2140320u + 8u * 6u, "00 00 0C 42 00 00 70 41") ||
        !ApiCodeMatches(game + 0x888E50u, "40 53 57 41 57 48 83 EC 20") || !ApiCodeMatches(game + 0x5B36C0u, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 50") ||
        !ApiCodeMatches(game + 0x5B36DDu, "48 8B 35 34 2D D7 02") || !ApiCodeMatches(game + 0x5B36E6u, "44 8B 4E 38") || !ApiCodeMatches(game + 0x5B36FDu, "4C 8B 5E 30 8B 5E 3C") ||
        !ApiCodeMatches(game + 0x7EFFC0u, "40 53 48 83 EC 30 3B 15 54 3C C9 02") || !ApiCodeMatches(game + 0x7EFFCEu, "4C 8B 0D F3 66 B3 02") || !ApiCodeMatches(game + 0x7F0062u, "F3 0F 11 74 02 48") ||
        !ApiCodeMatches(game + 0x7F0180u, "48 89 5C 24 18 48 89 6C 24 20 56 57 41 55 41 56 41 57") || !ApiCodeMatches(game + 0x511CC0u, "48 85 C9 74 71 48 8B 05 CC A2 F5 02") ||
        !ApiCodeMatches(game + 0x957AF2u, "4D 8D 86 50 19 05 00 41 FF 00") || !ApiCodeMatches(game + 0x957B6Au, "41 0F 11 86 78 A3 03 00"))
    { Note("living world: the noise dispatch is not the reviewed code; squads do not investigate noise"); return; }
    if (LoaderHook("living world: noise for offline squads", game + 0x64A890u, (void*)LwNoiseDetour, (void**)&g_lwNoiseOrig) || g_lwNoiseOrig == NULL)
    { Note("living world: the noise hook failed; squads do not investigate noise"); return; }
    InterlockedExchange(&g_lwNoiseOn, 1);
    Note("living world: squads investigate noise from the Helldivers' side (the noise dispatch, game.dll 0x64A890)");
}

static int LwPlayerSide(uint32_t entity)
{
    uintptr_t factions, masks; uint32_t slot;
    return entity != 0 && EvpPtr(g_lw.game + 0x3326CB8u, &factions) && EvpHash(factions + 0x12050, entity, 8, 4, &slot) &&
        EvpPtr(factions + 0x12078, &masks) && (EvpU32(masks + (uintptr_t)slot * 4, 0) & 1u);
}
static float LwNoiseRange(uint8_t type)
{
    float range = 0;
    if (SafeRead(g_lw.game + 0x2140320u + 8u * type, &range, 4) || !isfinite(range) || range < 0) return 0;
    return range;
}

typedef struct { float pos[3], reach; uint32_t source; ULONGLONG at; uint8_t type; } LwEvent;
static LwEvent g_lwEvents[16];
static struct { uint32_t heard, kept, events, sent, told, ended; } g_lwHear; /* 30 s counters */

static int LwCalm(const LwSquad* q)
{
    for (int k = 0; k < q->seenCount; k++) if (LwAlert(q->seen[k]) != 0) return 0;
    return 1;
}
/* the job position and an immediate repath for an online squad (S6, P) */
static int LwSquadGoTo(LwSquad* q, const float* target)
{
    typedef int (*SnapFn)(uint32_t nav, float* p, float radius, float* out, float* normal, uint32_t layer);
    typedef void (*RepathFn)(void*, uint32_t, const float*);
    uintptr_t table = 0, fn = 0, navs = 0; uint32_t slot;
    float p[3] = {target[0], target[1], target[2]};
    if (q->state != LW_ONLINE || q->squad == 0 || LwJobRecord(q->squad) == 0 || LwSquadRecord(q->squad) == 0) return 0;
    if (!LwGround(p[0], p[1], &p[2]) || !EvpPtr(g_lw.game + 0x3326330u, &table) || !EvpPtr(table + 0x10, &fn) || !fn ||
        ((SnapFn)fn)(EvpU32(g_lwWorld.dir + 0x648, 0), p, 25.0f, p, NULL, 0) == 0 || !isfinite(p[2])) return 0;
    g_lw.goal(q->squad, p);
    if (EvpPtr(g_lw.game + 0x3326418u, &navs) && EvpHash(navs + 0x30, q->squad, 8, 4, &slot)) ((RepathFn)(g_lw.game + 0x5B36C0u))(NULL, q->squad, p);
    return 1;
}
static int LwBits(uint32_t v) { int c = 0; for (; v; v &= v - 1) c++; return c; }

/* Phase 2b (KB living-world-plan "Phase 2 design", living-world-p2-contact): fights draw squads in. Once a second the
 * AiAlert table ([game+0x3326528]: count +0xC, entity pointers [+0x38] (id at +8), levels [+0x50], one index; P
 * 0x952BDC..0x952C71) gives every unit engaged (levels 3, 5, 6, 7); their positions cluster within LW_FIGHT_CLUSTER into
 * fights, kept while seen (ended after 30 s without an engaged unit). A call for reinforcements (the creator 0x957390:
 * dir+0x51950 + 1 at 0x957AF9, its position at dir+0x3A378, 0x957B6A) counts as 8 engaged units at its point. Each fight
 * wants min(3, 1 + engaged / 8) squads; calm PATROL or INVESTIGATE squads within LW_FIGHT_REACH (XL: x2) are sent,
 * nearest first. Offline they run the route at LW_RUSH x their pace; online they get the point (0xA13520 + the repath
 * 0x5B36C0) and every member the speed setter 0x7EFFC0(_, member, pace x LW_RUSH) (AiEnemy +0x48 and AiCharacter +0xC;
 * kept through state changes and the squad's slowest-member sync; the convoy code does the same, P). The speed is put
 * back as the convoy does (+0x48 = -1.0, then 0x7F0180(manager, unit, settings + block) with settings = 0x511CC0(the
 * unit's resource), block +0x64 when engaged, else +0x24; 0x864907..0x864926) when a member engages, the squad arrives
 * within LW_FIGHT_JOIN, or the intent ends. A fight draws squads once it lasted LW_FIGHT_HOLD, has 3 engaged or is a call.
 * Its target is an engaged unit's Behavior target (record +0x18 id, +0x1C position, +0x78 set; living-world-p2-contact)
 * on the Helldivers' side. Arrivals with nobody engaged hear it like a told investigation (0x888E50 at the target). */
#define LW_FIGHT_CLUSTER 40.0f /* engaged units within this are one fight (D) */
#define LW_FIGHT_MATCH 60.0f   /* a cluster within this of a known fight is that fight (D) */
#define LW_FIGHT_REACH 400.0f  /* squads sent from within this (D; x2 on extra large maps) */
#define LW_FIGHT_JOIN 40.0f    /* arrived within this of the fight (D) */
#define LW_FIGHT_HOLD 5000     /* ms a small fight lasts before it draws squads: alerts of a second or two end before help arrives (D) */
typedef struct { float pos[3], tpos[3]; int engaged, peak; uint32_t target; ULONGLONG born, seen; uint8_t sent, call, live; } LwFight;
static LwFight g_lwFights[16];
static struct { uint32_t fights, calls, sent, joined, ended; } g_lwRush; /* 30 s counters */
/* a unit's Behavior target on the Helldivers' side, or 0 */
static uint32_t LwBehaviorTarget(uint32_t unit, float* pos)
{
    uintptr_t beh = 0, recs = 0; uint32_t bi = 0;
    if (!EvpPtr(g_lw.game + 0x3326740u, &beh) || !EvpHash(beh + 0x40, unit, 8, 4, &bi) || !EvpPtr(beh + 0x60, &recs)) return 0;
    uintptr_t rec = recs + 0x1F8ull * bi; uint8_t has = 0;
    if (SafeRead(rec + 0x78, &has, 1) || !has || SafeRead(rec + 0x1C, pos, 12) || !isfinite(pos[0]) || !isfinite(pos[1])) return 0;
    uint32_t target = EvpU32(rec + 0x18, 0);
    return target && LwPlayerSide(target) ? target : 0;
}
static uintptr_t LwAiRecord(uint32_t unit)
{
    uintptr_t ai = 0, recs = 0; uint32_t idx = 0;
    return EvpPtr(g_lw.game + 0x33266C8u, &ai) && EvpHash(ai + 0x2330, unit, 8, 4, &idx) && EvpPtr(ai + 0x2350, &recs) ? recs + 0xA50ull * idx : 0;
}
static void LwRushMembers(LwSquad* q, const uint32_t* members, int n, int on)
{
    typedef void (*SpeedFn)(void*, uint32_t, float);
    typedef uintptr_t (*SettingsFn)(uint64_t);
    typedef void (*StateSpeedFn)(uintptr_t, uint32_t, uintptr_t);
    uintptr_t mgr = 0;
    if (!EvpPtr(g_lw.game + 0x33266C8u, &mgr)) return;
    float pace = (q->speed > 0.5f ? q->speed : LW_SPEED_DEFAULT) * LW_RUSH;
    int done = 0;
    for (int k = 0; k < n; k++)
    {
        uintptr_t rec = LwAiRecord(members[k]);
        if (!rec) continue;
        if (on) { ((SpeedFn)(g_lw.game + 0x7EFFC0u))(NULL, members[k], pace); done++; continue; }
        uint32_t u = 0; uint64_t res = EvpEntityResource(g_lw.game, members[k], &u);
        uintptr_t settings = res ? ((SettingsFn)(g_lw.game + 0x511CC0u))(res) : 0;
        *(volatile float*)(rec + 0x48) = -1.0f;
        if (settings) ((StateSpeedFn)(g_lw.game + 0x7F0180u))(mgr, members[k], settings + (LwAlert(members[k]) >= 3 ? 0x64 : 0x24));
        done++;
    }
    q->rushed = (uint8_t)(on && done > 0);
    if (on && done) Note("living world: squad %d runs to the fight at %.2f m/s (%d members)", (int)(q - g_lwSquads), (double)pace, done);
}
/* its fight still on: the slot is live and not reused since (a fight born after the squad was sent is another one) */
static int LwFightOf(const LwSquad* q) { int f = q->fight; return f >= 0 && f < 16 && g_lwFights[f].live && g_lwFights[f].born <= q->intentAt; }

static void LwIntentEnd(LwSquad* q, const char* why)
{
    ULONGLONG now = GetTickCount64();
    char peak[96] = "";
    if (q->state == LW_ONLINE && q->intentStage) snprintf(peak, sizeof peak, q->intent == LW_INVESTIGATE ? "; %d of %d members investigated (alert 2+, the first after %.1f s)"
        : "; %d of %d members engaged at the fight (alert 3+, the first after %.1f s)", LwBits(q->intentPeak), q->seenCount, (double)q->intentPeakAt);
    if (q->rushed && q->state == LW_ONLINE) LwRushMembers(q, q->seen, q->seenCount, 0);
    q->rushed = 0;
    if (q->intent == LW_RETAKE && q->garrison >= 0 && q->garrison < g_lwGarrisonCount && g_lwGarrisons[q->garrison].retaker == (int16_t)(q - g_lwSquads)) g_lwGarrisons[q->garrison].retaker = -1;
    Note("living world: squad %d %s ends (%s) after %.0f s, %s; %s (%.0f m from it, %.0f m when sent)%s", (int)(q - g_lwSquads),
        q->intent == LW_REINFORCE ? "reinforcement" : q->intent == LW_RETAKE ? "retake" : "investigation",
        why, (double)(now - q->intentAt) / 1000.0, q->state == LW_ONLINE ? "online" : "offline", q->intentStage ? "it reached the point" : "it did not reach the point",
        (double)sqrtf(LwDist2(q->pos, q->target[0], q->target[1])), (double)q->intentFrom, peak);
    if (q->intent == LW_REINFORCE) g_lwRush.ended++; else if (q->intent == LW_INVESTIGATE) g_lwHear.ended++;
    q->garrison = -1;
    q->intent = LW_PATROL; q->intentStage = 0; q->intentFree = now + LW_INTENT_REST; q->fight = -1;
    int node = LwNearestNode(q->pos[0], q->pos[1]);
    if (node >= 0) q->node = (uint16_t)node;
    q->pathLen = q->pathAt = 0; LwPlan(q);
    if (q->state == LW_ONLINE && q->jobSet && q->pathAt < q->pathLen)
    {
        float goal[3] = {g_lwNodes[q->path[q->pathAt]].x, g_lwNodes[q->path[q->pathAt]].y, 0};
        LwSquadGoTo(q, goal);
    }
}
/* offline: the route to the node nearest the point */
static int LwRetakeArrived(LwSquad* q, ULONGLONG now); static int LwRegionTier(const LwSquad* q, int region); static int LwRegionOfNode(int node);
static int LwIntentRoute(LwSquad* q)
{
    int goal = LwNearestNode(q->target[0], q->target[1]);
    if (goal < 0) return 0;
    if (goal == q->node) { q->pathLen = q->pathAt = 0; return 1; }
    int n = LwPath(q->node, goal, q->path, LW_PATH_MAX);
    if (n <= 0) return 0;
    q->pathLen = (uint8_t)n; q->pathAt = 0;
    return 1;
}
/* LwWalk, at the end of the path: 1 = stay (searching), 0 = patrol on */
static int LwIntentArrived(LwSquad* q)
{
    ULONGLONG now = GetTickCount64();
    if (q->intent == LW_REINFORCE) /* at the fight offline: it waits there while the fight lasts (players near bring it online) */
    {
        if (q->intentStage == 0)
        {
            q->intentStage = 1; q->intentStageAt = now;
            Note("living world: squad %d reached the fight at (%.0f, %.0f) offline after %.0f s", (int)(q - g_lwSquads), (double)q->target[0], (double)q->target[1], (double)(now - q->intentAt) / 1000.0);
        }
        if (LwFightOf(q) && now - q->intentStageAt < 120000) return 1;
        LwIntentEnd(q, LwFightOf(q) ? "waited 120 s" : "the fight is over");
        return 0;
    }
    if (q->intent == LW_RETAKE) return LwRetakeArrived(q, now);
    if (q->intent != LW_INVESTIGATE) return 0;
    if (q->intentStage == 0)
    {
        q->intentStage = 1; q->intentStageAt = now; q->intentSearch = 30000u + (uint32_t)(30000.0f * LwUnit());
        Note("living world: squad %d reached the noise at (%.0f, %.0f) offline after %.0f s, %.0f m from it; searches %.0f s", (int)(q - g_lwSquads),
            (double)q->target[0], (double)q->target[1], (double)(now - q->intentAt) / 1000.0, (double)sqrtf(LwDist2(q->pos, q->target[0], q->target[1])), (double)q->intentSearch / 1000.0);
    }
    if (now - q->intentStageAt < q->intentSearch) return 1;
    LwIntentEnd(q, "searched");
    return 0;
}
/* LwSendGoal for a squad with an intent */
static int LwIntentGoal(LwSquad* q)
{
    if (q->intent != LW_INVESTIGATE && q->intent != LW_REINFORCE && q->intent != LW_RETAKE) return 0;
    if (q->intentStage == 0) { q->intentSent = 1; if (!LwSquadGoTo(q, q->target)) LwIntentEnd(q, "no navmesh at the point"); }
    return 1;
}
/* A unit's AI settings (0x511CC0's table, as LwObstacleAbilities): the hearing threshold +0x2BC (0x7EB1DD, compared at
 * 0x7EB694) and the investigate time +0x5C (0x7EBBEC). 1 when found. */
static int LwHearSettings(uint32_t unit, float* threshold, float* investigate)
{
    uintptr_t game = g_lw.game, aiState = 0, table = 0; uint32_t u = 0;
    uint64_t res = EvpEntityResource(game, unit, &u);
    if (!res || SafeRead(game + 0x346BF98u, &aiState, 8) || !aiState || SafeRead(aiState + 0xF12DE8u, &table, 8) || !table) return 0;
    for (uint32_t p = 0; p < 316; p++)
    {
        uint64_t key = 0; uint32_t sidx = 0; uintptr_t b = table + 16ull * ((res + p) % 316);
        if (SafeRead(b, &key, 8) || key == 0) return 0;
        if (key != res) continue;
        uintptr_t st = table + 0x13C0 + 0x2DCull * (SafeRead(b + 8, &sidx, 4) ? 0 : sidx);
        return !SafeRead(st + 0x2BC, threshold, 4) && !SafeRead(st + 0x5C, investigate, 4);
    }
    return 0;
}
/* The detector the awareness update reads for a unit: [Behavior record + 0x68] (0x7ECF70 returns [game+0x3326740] +0x60
 * records + 0x1F8 idx + 8, and 0x7EB262 reads +0x60 from it; the record it compares is that id's, 0x7EB2C0). The game's
 * ears report to it too (the dispatch passes the ear's detector entity to 0x888E50, 0x64AE81). It is a separate entity
 * (lw103a: always the member's id - 1), not the unit, so the hearing write goes to it. 0 when not found. */
static uint32_t LwAwareDetector(uint32_t unit)
{
    uintptr_t beh = 0, recs = 0; uint32_t bi = 0, det;
    if (!EvpPtr(g_lw.game + 0x3326740u, &beh) || !EvpHash(beh + 0x40, unit, 8, 4, &bi) || !EvpPtr(beh + 0x60, &recs)) return 0;
    det = EvpU32(recs + 0x1F8ull * bi + 0x68, 0);
    return det == EvpU32(g_lw.game + 0x3483C4Cu, 0xFFFFFFFFu) ? 0 : det;
}
/* What 0x888E50 left for the first member (the write's own exits: the member not in the detector hash [game+0x3326548]
 * +0x30, its entity flag +0x14 bit 0 clear, the source's tag bit 25 from 0x4A9710 (Tags [game+0x3326430] hash +0x18,
 * values [+0x38] + 8 idx), the source not in the faction registry (then slot 0 +0x1228, which the awareness update
 * never reads)); the entry in the hostile list (+0x310 count, +0x318 + 0x50 i: +0 source, +0x20 heard time, +0x38
 * hearing, +0x48 flags) is what the awareness update compares with the settings' +0x2BC (0x7EB694). */
static void LwHearReadBack(const LwSquad* q, uint32_t member, uint32_t source) /* member: the detector entity written */
{
    uintptr_t det = 0, recs = 0, ents = 0, ent = 0, tags = 0, tagv = 0; uint32_t di = 0, ti = 0; uint64_t tag = 0;
    int inHash = EvpPtr(g_lw.game + 0x3326548u, &det) && EvpHash(det + 0x30, member, 8, 4, &di) && EvpPtr(det + 0x50, &recs);
    uint32_t owned = inHash && EvpPtr(det + 0x48, &ents) && EvpPtr(ents + 8ull * di, &ent) ? (EvpU32(ent + 0x14, 0) & 1u) : 0;
    int tagged = EvpPtr(g_lw.game + 0x3326430u, &tags) && EvpHash(tags + 0x18, source, 8, 4, &ti) && EvpPtr(tags + 0x38, &tagv) && !SafeRead(tagv + 8ull * ti, &tag, 8);
    char entry[96] = "no entry for the source";
    if (inHash)
    {
        uintptr_t r = recs + 0x13F8ull * di; uint32_t count = EvpU32(r + 0x310, 0);
        for (uint32_t i = 0; i < count && i < 16; i++)
        {
            uintptr_t e = r + 0x318 + 0x50ull * i; float level = 0; uint64_t heard = 0;
            if (EvpU32(e, 0) != source) continue;
            SafeRead(e + 0x38, &level, 4); SafeRead(e + 0x20, &heard, 8);
            snprintf(entry, sizeof entry, "entry %u of %u: hearing %.1f, flags %X, heard at %llu", i, count, (double)level, EvpU32(e + 0x48, 0), (unsigned long long)heard);
        }
    }
    /* the awareness update's own detector: [Behavior record + 0x68] (0x7ECF70 = the Behavior hash [game+0x3326740] +0x40,
     * returns [+0x60] + 0x1F8 idx + 8 (0x7ED005), and 0x7EB262 reads +0x60 from that; read at 0x7EB262, "none" [0x3483C4C] skips the unit at 0x7EB2D5); and its gate in the
     * AiEnemy update: local behaviour state ([+0x2358] + 0xC0 idx) equal to the synced one ([+0x2360] + 20 idx) before
     * the state's handler (state 3: 0xAA1260, which calls 0x7EB000 at 0xAA1285) runs (0x7E7A5B) */
    uintptr_t beh = 0, brecs = 0, ai = 0, loc = 0, syn = 0; uint32_t bi = 0, ei = 0, bdet = 0xFFFFFFFFu, local = 0xFFFFFFFFu, synced = 0xFFFFFFFFu;
    if (EvpPtr(g_lw.game + 0x3326740u, &beh) && EvpHash(beh + 0x40, member, 8, 4, &bi) && EvpPtr(beh + 0x60, &brecs)) bdet = EvpU32(brecs + 0x1F8ull * bi + 0x68, 0xFFFFFFFFu);
    if (EvpPtr(g_lw.game + 0x33266C8u, &ai) && EvpHash(ai + 0x2330, member, 8, 4, &ei) && EvpPtr(ai + 0x2358, &loc) && EvpPtr(ai + 0x2360, &syn))
    { local = EvpU32(loc + 0xC0ull * ei, 0xFFFFFFFFu); synced = EvpU32(syn + 20ull * ei, 0xFFFFFFFFu); }
    Note("living world: squad %d: the hearing write on member %08X: %s, entity flag %u; source %08X tags %s (bit 25 %s); %s; Behavior +0x68 detector %08X; behaviour state %u, synced %u", (int)(q - g_lwSquads), member,
        inHash ? "in the detector hash" : "NOT in the detector hash", owned, source, tagged ? "read" : "not read", tagged ? ((tag >> 25) & 1 ? "set" : "clear") : "-", entry, bdet, local, synced);
}
/* an online squad with an intent, each tick after its members were read */
static void LwReinforceOnline(LwSquad* q, const uint32_t* members, int n, const float* at, ULONGLONG now)
{
    int engaged = 0;
    for (int k = 0; k < n; k++) engaged += LwAlert(members[k]) >= 3;
    if (q->intentStage == 0)
    {
        if (!q->intentSent && q->jobSet) { q->intentSent = 1; if (!LwSquadGoTo(q, q->target)) { LwIntentEnd(q, "no navmesh at the point"); return; } }
        if (q->intentSent && !q->rushed && !engaged) LwRushMembers(q, members, n, 1);
        if (LwFightOf(q) && now >= q->retargetAt && LwDist2(g_lwFights[q->fight].pos, q->target[0], q->target[1]) > 30.0f * 30.0f) /* the fight moved */
        { memcpy(q->target, g_lwFights[q->fight].pos, sizeof q->target); q->retargetAt = now + 10000; if (q->jobSet) LwSquadGoTo(q, q->target); }
        int there = LwDist2(at, q->target[0], q->target[1]) <= LW_FIGHT_JOIN * LW_FIGHT_JOIN;
        if (engaged || there)
        {
            if (q->rushed) LwRushMembers(q, members, n, 0);
            q->intentStage = 1; q->intentStageAt = now; q->intentPeak = 0; q->intentPeakAt = 0; q->tells = 0; q->tellAt = now; g_lwRush.joined++;
            Note("living world: squad %d %s at (%.0f, %.0f) after %.0f s, %d of %d members engaged", (int)(q - g_lwSquads), engaged ? "joined the fight" : "reached the fight",
                (double)at[0], (double)at[1], (double)(now - q->intentAt) / 1000.0, engaged, n);
        }
        else
        {
            if (!LwFightOf(q)) { LwIntentEnd(q, "the fight ended before it got there"); return; }
            if (now - q->intentAt > 240000) LwIntentEnd(q, "not there in 240 s");
            return;
        }
    }
    for (int k = 0; k < n; k++) if (LwAlert(members[k]) >= 3) for (int i = 0; i < q->seenCount && i < 32; i++) if (q->seen[i] == members[k])
    { if (q->intentPeak == 0) q->intentPeakAt = (float)(now - q->intentStageAt) / 1000.0f; q->intentPeak |= 1u << i; }
    /* nobody engaged at the fight: every 10 s, up to 3 times, the members hear the fight's target where it is */
    if (!engaged && q->tells < 3 && now >= q->tellAt && LwFightOf(q) && g_lwFights[q->fight].target)
    {
        typedef void (*HearFn)(void*, uint32_t, uint32_t, const float*, float);
        LwFight* fi = &g_lwFights[q->fight]; int told = 0;
        for (int k = 0; k < n; k++) { uint32_t det = LwAwareDetector(members[k]); if (det) { ((HearFn)(g_lw.game + 0x888E50u))(NULL, det, fi->target, fi->tpos, 40.0f); told++; } }
        q->tells++; q->tellAt = now + 10000;
        Note("living world: squad %d at the fight: %d of %d members told of its target %08X at (%.0f, %.0f), %.0f m away (tell %d)", (int)(q - g_lwSquads), told, n, fi->target,
            (double)fi->tpos[0], (double)fi->tpos[1], (double)sqrtf(LwDist2(at, fi->tpos[0], fi->tpos[1])), q->tells);
    }
    if ((!LwFightOf(q) && !engaged) || now - q->intentStageAt > 180000) LwIntentEnd(q, LwFightOf(q) ? "180 s at the fight" : "the fight is over");
}
static void LwIntentOnline(LwSquad* q, const uint32_t* members, int n, const float* at)
{
    if (n <= 0) return;
    ULONGLONG now = GetTickCount64();
    if (q->intent == LW_REINFORCE) { LwReinforceOnline(q, members, n, at, now); return; }
    if (q->intent == LW_RETAKE) /* online, players are near: it walks there and waits; the retake itself needs it offline */
    {
        for (int k = 0; k < n; k++) if (LwAlert(members[k]) >= 3) { LwIntentEnd(q, "a member is in a fight"); return; }
        if (q->intentStage == 0 && LwDist2(at, q->target[0], q->target[1]) <= LW_FIGHT_JOIN * LW_FIGHT_JOIN)
        { q->intentStage = 1; q->intentStageAt = now; Note("living world: squad %d reached the garrison to retake online after %.0f s; it waits for players to leave", (int)(q - g_lwSquads), (double)(now - q->intentAt) / 1000.0); }
        else if (q->intentStage == 0 && now - q->intentAt > 300000) LwIntentEnd(q, "not there in 300 s");
        else if (q->intentStage == 1 && now - q->intentStageAt > LW_RETAKE_WAIT) LwIntentEnd(q, "players stayed near");
        return;
    }
    if (q->intent != LW_INVESTIGATE) return;
    for (int k = 0; k < n; k++) if (LwAlert(members[k]) >= 3) { LwIntentEnd(q, "a member is in a fight"); return; }
    if (q->intentStage == 0)
    {
        if (!q->intentSent && q->jobSet) { q->intentSent = 1; if (!LwSquadGoTo(q, q->target)) { LwIntentEnd(q, "no navmesh at the point"); return; } }
        if (now - q->intentAt > 180000) { LwIntentEnd(q, "not there in 180 s"); return; }
        if (LwDist2(at, q->target[0], q->target[1]) > LW_HEAR_NEAR * LW_HEAR_NEAR) return;
        typedef void (*HearFn)(void*, uint32_t, uint32_t, const float*, float);
        uint32_t source = q->intentSource;
        if (!LwPlayerSide(source)) { source = 0; for (int i = 0; i < g_lwWorld.playersAt && !source; i++) if (LwPlayerSide(g_lwWorld.avatars[i])) source = g_lwWorld.avatars[i]; }
        int told = 0;
        if (source) for (int k = 0; k < n; k++) { uint32_t det = LwAwareDetector(members[k]); if (det) { ((HearFn)(g_lw.game + 0x888E50u))(NULL, det, source, q->target, 40.0f); told++; } }
        LwHearReadBack(q, LwAwareDetector(members[0]), source);
        g_lwHear.told += (uint32_t)told;
        q->intentStage = 1; q->intentStageAt = now; q->intentPeak = 0; q->intentPeakAt = 0;
        /* the awareness update's gates before the hearing test (0x7EB000): AiEnemy +0xE4 bits 0x2 / 0x4 / 0x8 / 0x20 take
         * another branch (0x7EB1E6..0x7EB241), and synced state 0x10 ([AiEnemy +0x2360] + 20 idx, +0) skips the unit (0x7EB0CD, 0x7EB0FD, 0x7EB136) */
        uintptr_t ai = 0, recs = 0; uint32_t idx = 0, flags = 0xFFFFFFFFu, aiState = 0xFFFFFFFFu;
        if (EvpPtr(g_lw.game + 0x33266C8u, &ai) && EvpHash(ai + 0x2330, members[0], 8, 4, &idx) && EvpPtr(ai + 0x2350, &recs))
        { flags = EvpU32(recs + 0xA50ull * idx + 0xE4, 0xFFFFFFFFu); uintptr_t st; if (EvpPtr(ai + 0x2360, &st)) aiState = EvpU32(st + 20ull * idx, 0xFFFFFFFFu); }
        float thr = NAN, inv = NAN; LwHearSettings(members[0], &thr, &inv);
        Note("living world: squad %d reached the noise at (%.0f, %.0f) online after %.0f s; %d members told (source %08X); first member's AiEnemy flags %08X, synced state %u, hearing threshold %.1f, investigate time %.1f s", (int)(q - g_lwSquads),
            (double)q->target[0], (double)q->target[1], (double)(now - q->intentAt) / 1000.0, told, source, flags, aiState, (double)thr, (double)inv);
        return;
    }
    int calm = 1;
    for (int k = 0; k < n; k++)
    {
        uint32_t alert = LwAlert(members[k]);
        if (alert != 0) calm = 0;
        if (alert >= 2) for (int i = 0; i < q->seenCount && i < 32; i++) if (q->seen[i] == members[k])
        { if (q->intentPeak == 0) q->intentPeakAt = (float)(now - q->intentStageAt) / 1000.0f; q->intentPeak |= 1u << i; }
    }
    if ((calm && now - q->intentStageAt > 10000) || now - q->intentStageAt > 90000) LwIntentEnd(q, calm ? "members calm again" : "90 s");
}
/* an online squad going offline with an intent: the route from where it stands */
static void LwIntentOffline(LwSquad* q)
{
    if (q->intent != LW_INVESTIGATE && q->intent != LW_REINFORCE && q->intent != LW_RETAKE) return;
    q->intentSent = 0; q->rushed = 0; /* its units are gone, and their speed with them */
    if (q->intent == LW_RETAKE && q->intentStage != 0) { q->pathLen = q->pathAt = 0; return; } /* at the garrison: LwWalk applies the retake (LwRetakeArrived) */
    if (q->intentStage != 0) LwIntentEnd(q, q->intent == LW_REINFORCE ? "it went offline at the fight" : "it went offline while investigating");
    else if (!LwIntentRoute(q)) LwIntentEnd(q, "no route to the point");
}

static void LwInvestigate(const LwEvent* e)
{
    for (int f = 0; f < 16; f++) if (g_lwFights[f].live && LwDist2(g_lwFights[f].pos, e->pos[0], e->pos[1]) < 100.0f * 100.0f) return; /* a fight: 2b sends squads */
    float native = LwNoiseRange(e->type);
    int pick[LW_HEAR_SQUADS]; float d2s[LW_HEAR_SQUADS]; int picked = 0;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_lwWorld.count; i++)
    {
        LwSquad* q = &g_lwSquads[i];
        if ((q->state != LW_OFFLINE && q->state != LW_ONLINE) || q->intent != LW_PATROL || q->pulled || q->survivors == 0 || now < q->intentFree) continue;
        float d2 = LwDist2(q->pos, e->pos[0], e->pos[1]);
        if (d2 > e->reach * e->reach) continue;
        if (q->state == LW_ONLINE && (d2 <= native * native || !LwCalm(q))) continue; /* its ears hear it, or it is busy */
        int at = picked < LW_HEAR_SQUADS ? picked++ : (d2 < d2s[LW_HEAR_SQUADS - 1] ? LW_HEAR_SQUADS - 1 : -1);
        if (at < 0) continue;
        while (at > 0 && d2s[at - 1] > d2) { pick[at] = pick[at - 1]; d2s[at] = d2s[at - 1]; at--; }
        pick[at] = i; d2s[at] = d2;
    }
    char list[160] = ""; size_t len = 0;
    for (int j = 0; j < picked; j++)
    {
        LwSquad* q = &g_lwSquads[pick[j]];
        q->intent = LW_INVESTIGATE; q->intentStage = 0; q->intentSent = 0; q->intentAt = now; q->intentSource = e->source; q->intentFrom = sqrtf(d2s[j]);
        memcpy(q->target, e->pos, sizeof q->target);
        if (q->state == LW_OFFLINE) { if (!LwIntentRoute(q)) { q->intent = LW_PATROL; continue; } }
        else if (q->jobSet) { q->intentSent = 1; if (!LwSquadGoTo(q, q->target)) { q->intent = LW_PATROL; continue; } }
        g_lwHear.sent++;
        len += (size_t)snprintf(list + len, sizeof list - len, "%s%d (%s, %.0f m)", len ? ", " : "", pick[j], q->state == LW_ONLINE ? "online" : "offline", (double)sqrtf(d2s[j]));
        if (len >= sizeof list) len = sizeof list - 1;
    }
    Note("living world: noise type %u at (%.0f, %.0f, %.1f) from %08X, reach %.0f m: %s", e->type, (double)e->pos[0], (double)e->pos[1], (double)e->pos[2], e->source,
        (double)e->reach, picked ? list : "no calm squad in reach");
}
/* each squads tick, before the squads: the noises since the last tick */
static void LwNoiseDrain(void)
{
    if (!g_lwNoiseOn) return;
    static uintptr_t level;
    LONG head = g_lwNoiseHead;
    MemoryBarrier();
    if (level != g_lwWorld.lg) { level = g_lwWorld.lg; g_lwNoiseTail = head; memset(g_lwEvents, 0, sizeof g_lwEvents); } /* a new mission: nothing from before */
    if (head - g_lwNoiseTail > (LONG)LW_NOISE_RING) g_lwNoiseTail = head - (LONG)LW_NOISE_RING;
    ULONGLONG now = GetTickCount64();
    for (; g_lwNoiseTail != head; g_lwNoiseTail++)
    {
        LwNoise n = g_lwNoise[(uint32_t)g_lwNoiseTail % LW_NOISE_RING];
        g_lwHear.heard++;
        if (now - n.at > 2000 || n.mode || n.type == 5 || n.type >= 28 || !isfinite(n.pos[0]) || !isfinite(n.pos[1]) || !(n.loudness > 1e-6f)) continue;
        float reach = fminf(LwNoiseRange(n.type) * n.loudness * LW_HEAR_FAR, LW_HEAR_MAX);
        if (reach < 20.0f || !LwPlayerSide(n.source)) continue;
        g_lwHear.kept++;
        int same = -1, oldest = 0;
        for (int i = 0; i < 16; i++)
        {
            if (g_lwEvents[i].at && now - g_lwEvents[i].at < 10000 && LwDist2(g_lwEvents[i].pos, n.pos[0], n.pos[1]) < 50.0f * 50.0f) { same = i; break; }
            if (g_lwEvents[i].at < g_lwEvents[oldest].at) oldest = i;
        }
        if (same >= 0) { g_lwEvents[same].at = now; continue; }
        LwEvent* e = &g_lwEvents[oldest];
        memcpy(e->pos, n.pos, sizeof e->pos); e->reach = reach; e->source = n.source; e->at = now; e->type = n.type;
        g_lwHear.events++;
        LwInvestigate(e);
    }
}
static void LwHearNote(void)
{
    if (!g_lwNoiseOn || (g_lwHear.heard == 0 && g_lwHear.sent == 0)) return;
    Note("living world: noise (30 s): %u emissions, %u from the Helldivers' side in reach, %u events, %u squads sent, %u members told, %u investigations ended",
        g_lwHear.heard, g_lwHear.kept, g_lwHear.events, g_lwHear.sent, g_lwHear.told, g_lwHear.ended);
    memset(&g_lwHear, 0, sizeof g_lwHear);
}
static void LwReinforce(int f, int want)
{
    LwFight* fi = &g_lwFights[f];
    float reach = g_lwWorld.radius > 1500.0f ? 2.0f * LW_FIGHT_REACH : LW_FIGHT_REACH;
    int pick[3]; float d2s[3], keys[3]; int picked = 0, tiers[3];
    int region = LwRegionOfNode(LwNearestNode(fi->pos[0], fi->pos[1]));
    if (want > 3) want = 3;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_lwWorld.count; i++)
    {
        LwSquad* q = &g_lwSquads[i];
        if ((q->state != LW_OFFLINE && q->state != LW_ONLINE) || q->intent == LW_REINFORCE || q->intent == LW_RETAKE || q->pulled || q->survivors == 0) continue;
        if (q->intent == LW_PATROL && now < q->intentFree) continue;
        if (q->state == LW_ONLINE && !LwCalm(q)) continue; /* already in it, or busy */
        float d2 = LwDist2(q->pos, fi->pos[0], fi->pos[1]);
        if (d2 > reach * reach || d2 < LW_FIGHT_JOIN * LW_FIGHT_JOIN) continue;
        int tier = LwRegionTier(q, region); float key = (float)tier * 1e7f + d2; /* 2d: own region, neighbour, then the rest; nearest first in each */
        int at = picked < want ? picked++ : (key < keys[want - 1] ? want - 1 : -1);
        if (at < 0) continue;
        while (at > 0 && keys[at - 1] > key) { pick[at] = pick[at - 1]; d2s[at] = d2s[at - 1]; keys[at] = keys[at - 1]; tiers[at] = tiers[at - 1]; at--; }
        pick[at] = i; d2s[at] = d2; keys[at] = key; tiers[at] = tier;
    }
    char list[160] = ""; size_t len = 0; int sent = 0;
    for (int j = 0; j < picked; j++)
    {
        LwSquad* q = &g_lwSquads[pick[j]];
        uint8_t was = q->intent;
        q->intent = LW_REINFORCE; q->intentStage = 0; q->intentSent = 0; q->intentAt = now; q->intentFrom = sqrtf(d2s[j]); q->fight = (int8_t)f; q->retargetAt = now + 10000;
        memcpy(q->target, fi->pos, sizeof q->target);
        if (q->state == LW_OFFLINE) { if (!LwIntentRoute(q)) { q->intent = was; q->fight = -1; continue; } }
        else if (q->jobSet)
        {
            q->intentSent = 1;
            if (!LwSquadGoTo(q, q->target)) { q->intent = was; q->fight = -1; continue; }
            uint32_t ms[32]; int mn = LwMembers(q->squad, ms);
            if (mn > 0) LwRushMembers(q, ms, mn, 1);
        }
        sent++; fi->sent++; g_lwRush.sent++;
        len += (size_t)snprintf(list + len, sizeof list - len, "%s%d (%s%s, %.0f m%s)", len ? ", " : "", pick[j], q->state == LW_ONLINE ? "online" : "offline", was == LW_INVESTIGATE ? ", was investigating" : "", (double)sqrtf(d2s[j]),
            tiers[j] == 0 ? ", its region" : tiers[j] == 1 ? ", neighbour region" : "");
        if (len >= sizeof list) len = sizeof list - 1;
    }
    if (sent) Note("living world: fight %d at (%.0f, %.0f), %d engaged%s: squads sent %s", f, (double)fi->pos[0], (double)fi->pos[1], fi->engaged, fi->call ? " (a call for help)" : "", list);
}
/* each squads tick: once a second, the fights from the engaged units and the calls for help */
static void LwFightScan(void)
{
    static ULONGLONG next; static uint32_t calls; static uintptr_t level;
    ULONGLONG now = GetTickCount64();
    if (level != g_lwWorld.lg) { level = g_lwWorld.lg; memset(g_lwFights, 0, sizeof g_lwFights); calls = EvpU32(g_lwWorld.dir + 0x51950, 0); next = 0; }
    if (now < next) return;
    next = now + 1000;
    float cx[32][3], ctp[32][3]; int cn[32], clusters = 0; uint8_t ccall[32]; uint32_t ct[32];
    uintptr_t al = 0, ptrs = 0, levels = 0;
    if (EvpPtr(g_lw.game + 0x3326528u, &al) && EvpPtr(al + 0x38, &ptrs) && EvpPtr(al + 0x50, &levels))
    {
        uint32_t count = EvpU32(al + 0xC, 0);
        for (uint32_t i = 0; i < count && i < 1024; i++)
        {
            uint32_t lv = EvpU32(levels + 4ull * i, 0);
            if (lv != 3 && (lv < 5 || lv > 7)) continue;
            uintptr_t ent = 0; ActHealth h; float p[3];
            if (!EvpPtr(ptrs + 8ull * i, &ent) || !ent) continue;
            uint32_t id = EvpU32(ent + 8, 0);
            if (!id || !ActHealthOf(g_lw.game, id, &h) || !h.unit || !EvpUnitPosition(h.unit, p) || !isfinite(p[0])) continue;
            int c = 0;
            for (; c < clusters; c++) if (LwDist2(cx[c], p[0], p[1]) < LW_FIGHT_CLUSTER * LW_FIGHT_CLUSTER) break;
            if (c == clusters) { if (clusters == 32) continue; clusters++; cx[c][0] = p[0]; cx[c][1] = p[1]; cx[c][2] = p[2]; cn[c] = 0; ccall[c] = 0; ct[c] = 0; }
            if (!ct[c]) ct[c] = LwBehaviorTarget(id, ctp[c]);
            cx[c][0] = (cx[c][0] * (float)cn[c] + p[0]) / (float)(cn[c] + 1); cx[c][1] = (cx[c][1] * (float)cn[c] + p[1]) / (float)(cn[c] + 1);
            cn[c]++;
        }
    }
    uint32_t nowCalls = EvpU32(g_lwWorld.dir + 0x51950, calls);
    if (nowCalls != calls && clusters < 32) /* a call for reinforcements since the last scan: its point (the last one if several) */
    {
        float cp[3] = {0, 0, 0};
        if (!SafeRead(g_lwWorld.dir + 0x3A378, cp, 12) && isfinite(cp[0]) && isfinite(cp[1]))
        { cx[clusters][0] = cp[0]; cx[clusters][1] = cp[1]; cx[clusters][2] = cp[2]; cn[clusters] = 8; ccall[clusters] = 1; ct[clusters] = 0; clusters++; g_lwRush.calls++; }
        calls = nowCalls;
    }
    for (int c = 0; c < clusters; c++)
    {
        int f = -1, freeSlot = -1;
        for (int k = 0; k < 16; k++)
        {
            if (g_lwFights[k].live && LwDist2(g_lwFights[k].pos, cx[c][0], cx[c][1]) < LW_FIGHT_MATCH * LW_FIGHT_MATCH) { f = k; break; }
            if (!g_lwFights[k].live && freeSlot < 0) freeSlot = k;
        }
        if (f < 0)
        {
            if (freeSlot < 0) continue;
            f = freeSlot; memset(&g_lwFights[f], 0, sizeof g_lwFights[f]);
            g_lwFights[f].live = 1; g_lwFights[f].born = now; g_lwRush.fights++;
            Note("living world: fight %d at (%.0f, %.0f, %.1f): %d engaged%s, %.0f m from the nearest player", f, (double)cx[c][0], (double)cx[c][1], (double)cx[c][2], cn[c],
                ccall[c] ? " (a call for reinforcements)" : "", (double)sqrtf(LwNearestPlayer2(cx[c][0], cx[c][1])));
        }
        LwFight* fi = &g_lwFights[f];
        if (ccall[c]) { if (fi->seen != now) memcpy(fi->pos, cx[c], sizeof fi->pos); if (cn[c] > fi->engaged) fi->engaged = cn[c]; }
        else if (fi->seen == now) /* a second cluster of this fight in this scan: one centre, weighted by units */
        {
            int m = fi->engaged + cn[c];
            for (int a = 0; a < 3; a++) fi->pos[a] = (fi->pos[a] * (float)fi->engaged + cx[c][a] * (float)cn[c]) / (float)m;
            fi->engaged = m;
        }
        else { memcpy(fi->pos, cx[c], sizeof fi->pos); fi->engaged = cn[c]; }
        if (ct[c]) { fi->target = ct[c]; memcpy(fi->tpos, ctp[c], sizeof fi->tpos); }
        if (fi->engaged > fi->peak) fi->peak = fi->engaged;
        fi->seen = now; fi->call |= ccall[c];
    }
    for (int f = 0; f < 16; f++)
    {
        LwFight* fi = &g_lwFights[f];
        if (!fi->live) continue;
        if (now - fi->seen > 30000)
        {
            fi->live = 0;
            Note("living world: fight %d at (%.0f, %.0f) is over after %.0f s (at most %d engaged, %u squads sent)", f, (double)fi->pos[0], (double)fi->pos[1],
                (double)(fi->seen - fi->born) / 1000.0, fi->peak, fi->sent);
            continue;
        }
        int want = 1 + fi->engaged / 8;
        if (want > 3) want = 3;
        if (fi->seen == now && fi->sent < want && (now - fi->born >= LW_FIGHT_HOLD || fi->engaged >= 3 || fi->call)) LwReinforce(f, want - fi->sent);
    }
}
static void LwRushNote(void)
{
    if (g_lwRush.fights == 0 && g_lwRush.sent == 0 && g_lwRush.ended == 0) return;
    Note("living world: fights (30 s): %u new, %u calls for help, %u squads sent, %u arrived or joined, %u reinforcements ended", g_lwRush.fights, g_lwRush.calls, g_lwRush.sent, g_lwRush.joined, g_lwRush.ended);
    memset(&g_lwRush, 0, sizeof g_lwRush);
}
/* test command "noise <type> [dx dy]" (HELLFORGE_LW_TEST_TRACE=1): a noise through the game's dispatch (and so the hook)
 * at the local player's feet plus (dx, dy), from the local Helldiver, mode 0, loudness 1, as a shot of that template.
 * The late tick runs on the main thread right after the frame's own post-update noise calls (explosions 0xAB5FBF,
 * beams 0xAB5FCB, impacts 0xAB5FEF; the late tick 0xAB612D), so the call has their context. */
static void LwTestNoise(int seq, int type, float dx, float dy)
{
    typedef void (*NoiseFn)(void*, uint8_t, uint32_t, const float*, uint32_t, float, uint32_t);
    uint32_t avatar = 0; float p[3] = {0, 0, 0};
    for (int i = 0; i < g_lwWorld.playersAt && !avatar; i++) if (g_lwWorld.local[i]) { avatar = g_lwWorld.avatars[i]; memcpy(p, g_lwWorld.players[i], sizeof p); }
    if (!g_lwNoiseOn || !avatar || type <= 0 || type >= 28 || LwNoiseRange((uint8_t)type) <= 0) { Note("living world: test command %d: no noise (hook %d, avatar %08X, type %d)", seq, (int)g_lwNoiseOn, avatar, type); return; }
    p[0] += dx; p[1] += dy;
    LwGround(p[0], p[1], &p[2]);
    uint32_t none = EvpU32(g_lw.game + 0x3483C34u, 0);
    ((NoiseFn)(g_lw.game + 0x64A890u))(NULL, (uint8_t)type, avatar, p, 0, 1.0f, none);
    Note("living world: test command %d: noise type %d (range %.0f m) at (%.0f, %.0f, %.1f) from %08X", seq, type, (double)LwNoiseRange((uint8_t)type), (double)p[0], (double)p[1], (double)p[2], avatar);
}

/* Phase 2c (KB living-world-p2-retake, "Refill recipe"): a squad re-occupies a cleared plain guard post. Cleared stays 1
 * (un-clearing repeats the cleared effects on every peer: HUD notice, VO, 0x1272B80). Instead, every point but the k
 * chosen ones is held (bit 6), the chosen killed points get their U bit back (written and sent as the kill path 0x90F9F0
 * does: 0xFD97E0(net id, the word's hash, &word)), and the refill flag runtime +0x1B4F is armed. The refill path
 * 0x912274 fills an active garrison with a detector within 150 m and never reads cleared, and the activation of a cleared
 * garrison jumps over the flag reset (0x912463), so the units appear natively when a player next comes near.
 * Plain: synced spawners +0x1C, objectives +0x24 and counted +0x2C all 0, no 0x200 point, and no structure at all (the map
 * location kind cannot decide it: the generator's location table is wiped once the level is built, lw111a; objective
 * sites register their objective entity, +0x24 >= 1 at every one seen). Link +0x2AB 0 (a non-outpost location) runs no
 * cleared effects at all; for 1 they fired once at the native clear and cleared stays 1: a held structure point keeps its structure through the despawn (0x90F911), an
 * open one could refill a destroyed one. A plain post is cleared by the kill that empties its units list (0x91055B). */
#define LW_RETAKE_CLEAR 300000   /* ms a garrison stays cleared before a squad retakes it (D) */
#define LW_RETAKE_AGAIN 600000   /* ms between retakes of one garrison (D) */
#define LW_RETAKE_PLAYER 300.0f  /* no player within this while a squad is sent or the garrison is armed (D) */
#define LW_RETAKE_NODE 60.0f    /* the route node nearest the garrison is within this of its centre (D) */
#define LW_RETAKE_REACH 800.0f   /* squads sent from within this (D; 1500 m on extra large maps) */
static ULONGLONG g_lwRetakeClear = LW_RETAKE_CLEAR; /* the test command "retake wait <s>" shortens it */
static const uint32_t kLwUHash[3] = {0x58840A74u, 0x8D0AE425u, 0x44818B0Au};
static int LwRetakePinsOk(void)
{
    static int ok = -1;
    if (ok < 0)
    {
        uintptr_t g = g_lw.game;
        ok = ApiCodeMatches(g + 0xFD97E0u, "48 89 6C 24 18 56 41 56 41 57 48 83 EC 30") && ApiCodeMatches(g + 0x912274u, "80 BC 3B 4F 1B 00 00 00") &&
             ApiCodeMatches(g + 0x912463u, "80 78 39 00 75 1B C6 81 4F 1B") && ApiCodeMatches(g + 0x90FA44u, "0A 8B 81 44") &&
             ApiCodeMatches(g + 0x91055Bu, "41 83 7A 24 00 75 10 45 84 ED 74 0B 83 BE 84 03 00 00 00 75");
        if (!ok) Note("living world: the garrison refill is not the reviewed code; squads do not retake garrisons");
    }
    return ok;
}
static int LwPlayerWithin(const float* at, float r) { return LwNearestPlayer2(at[0], at[1]) < r * r; }
/* each tick per owned garrison (LwGarrisonsTick): cleared time, plain test, centre; an armed retake's holds and re-arming */
static void LwRetakeGarrison(LwGarrison* g, uintptr_t d, uintptr_t s, uintptr_t r, uintptr_t l, uintptr_t points, uint32_t n, const uint32_t* u, uint8_t active, uint8_t refill)
{
    ULONGLONG now = GetTickCount64();
    uint8_t cleared = 0; uint32_t f[3] = {1, 1, 1};
    SafeRead(s + 0x39, &cleared, 1);
    if (cleared && !g->cleared) g->clearedAt = now;
    uint8_t newly = cleared && !g->cleared;
    g->cleared = cleared;
    int plain = g->plain;
    if (now >= g->plainAt || newly) /* every 2 s (staggered): a fill can register spawners or structures; ReadProcessMemory per point is not free */
    {
        g->plainAt = now + 2000 + 125u * (g->index % 16u);
        plain = n > 0 && !SafeRead(s + 0x1C, &f[0], 4) && !SafeRead(s + 0x24, &f[1], 4) && !SafeRead(s + 0x2C, &f[2], 4) &&
            !f[0] && !f[1] && !f[2] && EvpU32(r + 0x1110, 1) == 0 && !(g->structure[0] | g->structure[1] | g->structure[2]);
        double sx = 0, sy = 0; int got = 0;
        for (uint32_t p = 0; p < n && plain; p++)
        {
            uint32_t gi = EvpU32(l + 0x28 + 4u * p, 0xFFFFFFFFu), flags = 0; float xy[2];
            if (gi >= 0x100000 || SafeRead(points + 0x58u * gi + 0x24, &flags, 4) || (flags & 0x220)) { plain = 0; break; }
            if (!g->haveCenter && !SafeRead(points + 0x58u * gi + 0x10, xy, 8) && isfinite(xy[0]) && isfinite(xy[1])) { sx += xy[0]; sy += xy[1]; got++; }
        }
        if (!g->haveCenter && got) { g->center[0] = (float)(sx / got); g->center[1] = (float)(sy / got); g->haveCenter = 1; }
        g->plain = (uint8_t)plain;
    }
    memcpy(g->u, u, sizeof g->u); g->points = n;
    if (!g->haveCenter && n > 0 && n <= 96) /* every garrison's centre (2d seeds regions with it), once */
    {
        double cx = 0, cy = 0; int k = 0;
        for (uint32_t p = 0; p < n; p++) { uint32_t gi = EvpU32(l + 0x28 + 4u * p, 0xFFFFFFFFu); float xy[2]; if (gi < 0x100000 && !SafeRead(points + 0x58u * gi + 0x10, xy, 8) && isfinite(xy[0]) && isfinite(xy[1])) { cx += xy[0]; cy += xy[1]; k++; } }
        if (k) { g->center[0] = (float)(cx / k); g->center[1] = (float)(cy / k); g->haveCenter = 1; }
    }
    if (newly) Note("living world: garrison %08X cleared (%s)", g->entity, plain ? "plain: a squad may retake it" : "not plain: never retaken");
    if (!g->retake) return;
    if (active && g->lastRefill && !refill) g->fillAt = now + 5000; /* the refill path ran (it zeroes the flag after the fill, 0x91229D) */
    g->lastRefill = refill;
    if (g->fillAt && now >= g->fillAt) { g->fillAt = 0; LwRetakeFilled(g, d, r, u, n); }
    uint32_t owed = 0;
    for (uint32_t p = 0; p < n; p++) owed += (g->allow[p / 32] & u[p / 32] & (1u << (p % 32))) != 0;
    if (!active && !refill) /* after a despawn (players left): cleared garrisons never fill on activation, so arm it again */
    {
        if (!owed)
        {
            g->retake = 0; g->retaker = -1;
            Note("living world: retaken garrison %08X is lost again: every retaken point killed", g->entity);
            return;
        }
        uint8_t one = 1;
        if (!LwPoke(r + 0x1B4F, &one, 1)) refill = 1;
    }
    if (active && !refill) return;
    for (uint32_t p = 0; p < n; p++) /* every point but the chosen ones held, and the chosen ones open */
    {
        uint32_t gi = EvpU32(l + 0x28 + 4u * p, 0xFFFFFFFFu), flags = 0, bit = 1u << (p % 32);
        uintptr_t at = points + 0x58u * gi + 0x24;
        if (gi >= 0x100000 || SafeRead(at, &flags, 4)) continue;
        if (g->allow[p / 32] & bit)
        { if (g->marked[p / 32] & bit) { if (!(flags & 0x40)) g->marked[p / 32] &= ~bit; else { flags &= ~0x40u; if (!LwPoke(at, &flags, 4)) g->marked[p / 32] &= ~bit; } } }
        else if (!(flags & 0x40)) { flags |= 0x40; if (!LwPoke(at, &flags, 4)) g->marked[p / 32] |= bit; }
    }
}
/* the living units at each point of a garrison (AiEnemy records +0xC = the garrison's net id, +0x10 = point + 1, as the
 * kill path 0x910504..0x91051C matches them) */
static int LwGarrisonLiving(uint32_t net, uint32_t* mask)
{
    uintptr_t e, records, entities, d; int found = 0;
    mask[0] = mask[1] = mask[2] = 0;
    if (!EvpPtr(g_lw.game + 0x33266C8u, &e) || !EvpPtr(e + 0x2360, &records) || !EvpPtr(e + 0x2348, &entities)) return -1;
    uint32_t count = EvpU32(e + 0x2320, 0);
    for (uint32_t i = 0; i < count && i < 65536; i++)
    {
        uint32_t rr[5]; ActHealth h;
        if (SafeRead(records + 20u * i, rr, sizeof rr) || rr[3] != net || rr[4] == 0 || rr[4] > 96 || !EvpPtr(entities + 8u * i, &d) || d == 0) continue;
        if (ActHealthOf(g_lw.game, EvpU32(d + 8, 0), &h) && h.life < 2) { mask[(rr[4] - 1) / 32] |= 1u << ((rr[4] - 1) % 32); found++; }
    }
    return found;
}
/* the squad at the garrison, offline: the recipe. The points opened (k <= survivors: those units become the garrison) when done, 0 = not yet, -1 = never */
static int LwRetakeApply(LwSquad* q, LwGarrison* g)
{
    uintptr_t gf, synced, runtime, lists, descriptors, points, d;
    if (!g->plain || !g->cleared || g->retake) return -1;
    if (!EvpPtr(g_lw.game + 0x33265C0u, &gf) || !EvpPtr(gf + 0x50, &synced) || !EvpPtr(gf + 0x40, &runtime) || !EvpPtr(gf + 0x48, &lists) ||
        !EvpPtr(gf + 0x38, &descriptors) || !EvpPtr(g_lw.game + 0x346D5C0u, &points) || g->index >= EvpU32(gf + 0x10, 0) || !EvpPtr(descriptors + 8u * g->index, &d) ||
        EvpU32(d + 8, 0) != g->entity) return -1;
    uintptr_t s = synced + 0x40u * g->index, r = runtime + 0x1B58u * g->index, l = lists + 0x2B8u * g->index;
    uint8_t active = 1; uint32_t u[3], n = EvpU32(l + 0x1A8, 0);
    if (SafeRead(s + 0x38, &active, 1) || SafeRead(s + 4, u, sizeof u) || n == 0 || n > 96) return -1;
    if (active || LwPlayerWithin(g->center, LW_RETAKE_PLAYER) || EvpU32(r + 0x384, 1) != 0) return 0;
    uint32_t allow[3] = {0, 0, 0}, restore[3] = {0, 0, 0}; int k = q->survivors, chosen = 0, fresh = 0, owed = 0, killed = 0;
    for (int pass = 0; pass < 3 && chosen < k; pass++) /* (a) never filled with U set, (b) filled with U set (owed), (c) killed */
        for (uint32_t p = 0; p < n && chosen < k; p++)
        {
            uint32_t gi = EvpU32(l + 0x28 + 4u * p, 0xFFFFFFFFu), flags = 0, bit = 1u << (p % 32), w = p / 32;
            if (allow[w] & bit || gi >= 0x100000 || SafeRead(points + 0x58u * gi + 0x24, &flags, 4)) continue;
            if ((flags & 0x220) || ((flags & 0x40) && !(g->marked[w] & bit))) continue; /* objective / structure points; held by the game. Phase A (0x100) refills like phase B: 0x90E400..0x90E427 skips only bit 6 */
            int taken = (g->taken[w] & bit) != 0, uset = (u[w] & bit) != 0;
            if (pass == 0 ? (taken || !uset) : pass == 1 ? (!taken || !uset) : (!taken || uset)) continue;
            allow[w] |= bit; chosen++;
            if (pass == 2) { restore[w] |= bit; killed++; } else if (pass == 1) owed++; else fresh++;
        }
    if (!chosen) return -1;
    typedef void (*NetDirtyFn)(uint32_t, uint32_t, void*);
    uint32_t net = EvpU32(d + 0x10, 0x7FFFu);
    memcpy(g->allow, allow, sizeof allow); g->retake = 1; /* the holds go in now, before the flag */
    LwRetakeGarrison(g, d, s, r, l, points, n, u, 0, 1); g->lastRefill = 1; g->fillAt = 0;
    for (int w = 0; w < 3; w++)
        if (restore[w])
        {
            uint32_t word = u[w] | restore[w];
            if (!LwPoke(s + 4 + 4u * w, &word, 4)) ((NetDirtyFn)(g_lw.game + 0xFD97E0u))(net, kLwUHash[w], (void*)(s + 4 + 4u * w));
        }
    uint8_t one = 1; LwPoke(r + 0x1B4F, &one, 1);
    uint32_t after[3] = {0, 0, 0}; SafeRead(s + 4, after, sizeof after);
    int held = 0;
    for (uint32_t p = 0; p < n; p++) { uint32_t gi = EvpU32(l + 0x28 + 4u * p, 0xFFFFFFFFu), flags = 0; if (gi < 0x100000 && !SafeRead(points + 0x58u * gi + 0x24, &flags, 4)) held += (flags & 0x40) != 0; }
    uint8_t flag = 0; SafeRead(r + 0x1B4F, &flag, 1);
    g->retakeAt = GetTickCount64(); g->retaker = -1;
    Note("living world: squad %d retook garrison %08X at (%.0f, %.0f) after %.0f s: %d of %d survivors become its units at %d points (%d never filled, %d owed, %d killed, "
        "U %08X %08X %08X -> %08X %08X %08X), %d of %u points held, refill flag %u; it fills when a player comes within 150 m", (int)(q - g_lwSquads), g->entity,
        (double)g->center[0], (double)g->center[1], (double)(g->retakeAt - q->intentAt) / 1000.0, chosen, k, chosen, fresh, owed, killed,
        u[0], u[1], u[2], after[0], after[1], after[2], held, n, flag);
    return chosen;
}
static void LwRetakeScan(void)
{
    static ULONGLONG next;
    ULONGLONG now = GetTickCount64();
    if (now < next || !g_lwGarrisonCount) return;
    next = now + 10000;
    if (!LwRetakePinsOk()) { next = ~0ull; return; }
    float reach = g_lwWorld.radius > 1500.0f ? 1500.0f : LW_RETAKE_REACH;
    for (int gi = 0; gi < g_lwGarrisonCount; gi++)
    {
        LwGarrison* g = &g_lwGarrisons[gi];
        if (!g->plain || !g->cleared || g->retake || !g->haveCenter || now - g->clearedAt < g_lwRetakeClear || (g->retakeAt && now - g->retakeAt < LW_RETAKE_AGAIN)) continue;
        if (g->retaker >= 0 && g->retaker < g_lwWorld.count && g_lwSquads[g->retaker].state != LW_FREE && g_lwSquads[g->retaker].survivors &&
            g_lwSquads[g->retaker].intent == LW_RETAKE && g_lwSquads[g->retaker].garrison == gi) continue;
        g->retaker = -1;
        if (LwPlayerWithin(g->center, LW_RETAKE_PLAYER)) continue;
        int node = LwNearestNode(g->center[0], g->center[1]); /* the squad arrives at this node: it must be at the garrison */
        if (node < 0 || LwDist2(g->center, g_lwNodes[node].x, g_lwNodes[node].y) > LW_RETAKE_NODE * LW_RETAKE_NODE) continue;
        int pick = -1, bestTier = 3; float best = reach * reach;
        for (int i = 0; i < g_lwWorld.count; i++) /* 2d: the region's own squads first, then a neighbour's, then the nearest */
        {
            LwSquad* q = &g_lwSquads[i];
            if (q->state != LW_OFFLINE || q->intent != LW_PATROL || q->pulled || q->survivors == 0 || now < q->intentFree) continue;
            float d2 = LwDist2(q->pos, g->center[0], g->center[1]);
            int tier = LwRegionTier(q, gi);
            if (d2 < reach * reach && (tier < bestTier || (tier == bestTier && d2 < best))) { best = d2; bestTier = tier; pick = i; }
        }
        if (pick < 0) continue;
        LwSquad* q = &g_lwSquads[pick];
        q->intent = LW_RETAKE; q->intentStage = 0; q->intentSent = 0; q->intentAt = now; q->intentFrom = sqrtf(best); q->garrison = (int16_t)gi;
        q->target[0] = g->center[0]; q->target[1] = g->center[1]; q->target[2] = 0;
        if (!LwIntentRoute(q)) { q->intent = LW_PATROL; q->garrison = -1; q->intentFree = now + LW_INTENT_REST; continue; } /* the next scan tries another squad */
        g->retaker = (int16_t)pick;
        static const char* const tiers[] = {"its region's", "a neighbour region's", "the nearest"};
        Note("living world: garrison %08X at (%.0f, %.0f), cleared %.0f s ago: squad %d (offline, %u survivors, %.0f m, %s) sent to retake it", g->entity,
            (double)g->center[0], (double)g->center[1], (double)(now - g->clearedAt) / 1000.0, pick, q->survivors, (double)q->intentFrom, tiers[bestTier]);
    }
}
/* the retaking squad at the end of its path, offline (LwIntentArrived) */
static int LwRetakeArrived(LwSquad* q, ULONGLONG now)
{
    if (q->intentStage == 0)
    {
        q->intentStage = 1; q->intentStageAt = now;
        Note("living world: squad %d reached garrison %08X offline after %.0f s, %.0f m from its centre", (int)(q - g_lwSquads),
            q->garrison >= 0 && q->garrison < g_lwGarrisonCount ? g_lwGarrisons[q->garrison].entity : 0, (double)(now - q->intentAt) / 1000.0,
            (double)sqrtf(LwDist2(q->pos, q->target[0], q->target[1])));
    }
    LwGarrison* g = q->garrison >= 0 && q->garrison < g_lwGarrisonCount ? &g_lwGarrisons[q->garrison] : NULL;
    int done = g ? LwRetakeApply(q, g) : -1;
    if (done > 0)
    {
        g_lwWorld.retook++;
        if (done < q->survivors) /* the rest patrol on as the same squad */
        {
            char why[64]; snprintf(why, sizeof why, "retook it; %d survivors patrol on", q->survivors - done);
            q->survivors = (uint8_t)(q->survivors - done);
            LwIntentEnd(q, why);
            return 0;
        }
        q->intent = LW_PATROL; q->intentStage = 0; q->garrison = -1; q->state = LW_FREE; q->survivors = 0; /* it became the garrison */
        return 1;
    }
    if (done == 0 && now - q->intentStageAt < LW_RETAKE_WAIT) return 1;
    LwIntentEnd(q, done < 0 ? "the garrison cannot be retaken" : "players stayed near");
    return 0;
}
/* test command "clear garrison": every living unit of the active plain garrison nearest the local player is killed
 * through the game's death path (QueueDamage kind 7, as the kill test), so the last kill clears it natively */
static void LwTestClear(int seq)
{
    typedef void (*QueueDamageFn)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t, uint32_t, uint32_t, uint64_t, uint32_t, float, float, float,
        const float*, const float*, uint32_t, uint32_t, uint32_t, float);
    uintptr_t gf, runtime, synced;
    if (!ApiCodeMatches(g_lw.game + 0x129F910u, "40 55 53 56 57 41 56 48 8D 6C 24 F9 48 81 EC D0 00 00 00") || !EvpPtr(g_lw.game + 0x33265C0u, &gf) ||
        !EvpPtr(gf + 0x40, &runtime) || !EvpPtr(gf + 0x50, &synced) || g_lwWorld.playersAt <= 0) { Note("living world: test command %d: clear: not readable", seq); return; }
    LwGarrison* best = NULL; float bd = 1e18f;
    for (int i = 0; i < g_lwGarrisonCount; i++)
    {
        LwGarrison* g = &g_lwGarrisons[i]; uint8_t active = 0;
        if (!g->plain || g->cleared || !g->haveCenter || g->index >= EvpU32(gf + 0x10, 0) || SafeRead(synced + 0x40u * g->index + 0x38, &active, 1) || !active) continue;
        float d2 = LwDist2(g_lwWorld.players[0], g->center[0], g->center[1]);
        if (d2 < bd) { bd = d2; best = g; }
    }
    if (!best) { Note("living world: test command %d: clear: no active plain garrison", seq); return; }
    uintptr_t r = runtime + 0x1B58u * best->index; uint32_t count = EvpU32(r + 0x384, 0), killed = 0;
    static const float down[3] = {0, 0, -1};
    for (uint32_t k = 0; k < count && k < 96; k++)
    {
        uint32_t e = EvpU32(r + 0x204 + 4u * k, 0); ActHealth h; float pos[3];
        if (!e || !ActHealthOf(g_lw.game, e, &h) || h.life >= 2 || !h.unit || !h.network || !EvpUnitPosition(h.unit, pos)) continue;
        ((QueueDamageFn)(g_lw.game + 0x129F910u))(NULL, 7, 0, e, 0xFFFFFFFFu, 0, h.network, h.network, 0, 0, 0.0f, 0.0f, 0.0f, down, pos, 0xFFFFFFFFu, 0, 0, 1.0f);
        killed++;
    }
    Note("living world: test command %d: clear: garrison %08X at (%.0f, %.0f), %.0f m: %u of %u listed units killed (QueueDamage kind 7)", seq, best->entity,
        (double)best->center[0], (double)best->center[1], (double)sqrtf(bd), killed, count);
}
/* test command "retake wait <s>": how long a garrison stays cleared before a squad is sent (default LW_RETAKE_CLEAR) */
static void LwRetakeWait(int seq, float sec)
{
    if (sec >= 0 && sec <= 3600) g_lwRetakeClear = (ULONGLONG)(sec * 1000.0f);
    Note("living world: test command %d: garrisons are retaken %.0f s after they were cleared", seq, (double)g_lwRetakeClear / 1000.0);
}
/* 5 s after a retaken garrison filled (LwGarrisonsTick): its living units by point, against the points the retake opened */
static void LwRetakeFilled(LwGarrison* g, uintptr_t d, uintptr_t r, const uint32_t* u, uint32_t n)
{
    uint32_t at[3]; int living = LwGarrisonLiving(EvpU32(d + 0x10, 0x7FFFu), at), inside = 0, outside = 0, opened = 0, owed = 0;
    for (uint32_t p = 0; p < n; p++)
    {
        uint32_t bit = 1u << (p % 32), w = p / 32;
        opened += (g->allow[w] & bit) != 0; owed += (g->allow[w] & u[w] & bit) != 0;
        if (at[w] & bit) { if (g->allow[w] & bit) inside++; else outside++; }
    }
    if (living >= 0) for (int w = 0; w < 3; w++) g->allow[w] &= at[w]; /* an opened point the fill left empty (no group fits it, or the 70 room) is not owed later */
    Note("living world: retaken garrison %08X filled: %u units listed, %d living; points with a living unit: %d of the %d opened, %d elsewhere; %d opened points still owed (U set); cleared stays %u",
        g->entity, EvpU32(r + 0x384, 0), living, inside, opened, outside, owed, (unsigned)g->cleared);
}

/* Phase 2d (KB living-world-plan "Phase 2 design", 2d): territory. Every garrison seeds a region; each route node belongs
 * to the garrison reached first by a breadth-first search over the route graph from the garrisons' nearest nodes (nodes
 * no search reaches: the nearest centre). Regions touching across an edge are neighbours. Strength = the garrison's
 * units (points with U set while not cleared; a retaken one: its opened points still owed) + the survivors of living
 * squads homed there (the region of the node a squad stands on when the regions are built or it is placed). A region
 * with no strength is lost until a squad retakes its garrison (2c). Patrol goals keep to the home region or a neighbour
 * 70 % of the time; retake and reinforcement prefer squads of the region, then of a neighbour, then the nearest. */
#define LW_REGIONS_MAX LW_GARRISONS_MAX
static uint16_t g_lwRegion[LW_NODES_MAX]; /* garrison index + 1, 0 = none */
static uint8_t g_lwRegionAdj[LW_REGIONS_MAX][LW_REGIONS_MAX / 8];
static int g_lwRegionStrength[LW_REGIONS_MAX], g_lwRegionNodes[LW_REGIONS_MAX]; /* nodes: 0 = a garrison sharing its nearest node with another (no region) */
static uint8_t g_lwRegionHeld[LW_REGIONS_MAX];
static struct { uintptr_t owner; int garrisons, built, seeded, nodes; ULONGLONG next, noteAt; uint32_t lost, regained; } g_lwTerr;
static int LwRegionOfNode(int node) { return g_lwTerr.built && node >= 0 && node < g_lwNodeCount ? (int)g_lwRegion[node] - 1 : -1; }
static int LwRegionsTouch(int a, int b) { return a >= 0 && b >= 0 && a < LW_REGIONS_MAX && b < LW_REGIONS_MAX && (a == b || (g_lwRegionAdj[a][b / 8] >> (b % 8)) & 1); }
/* 0 = the squad's own region, 1 = a neighbour, 2 = elsewhere or no territory */
static int LwRegionTier(const LwSquad* q, int region) { int h = (int)q->home1 - 1; return h < 0 || region < 0 ? 2 : h == region ? 0 : LwRegionsTouch(h, region) ? 1 : 2; }
/* node < 0: whether the squad has a home at all */
static int LwTerritoryGoalOk(const LwSquad* q, int node)
{
    if (!g_lwTerr.built || q->home1 == 0) return 0;
    return node < 0 ? 1 : LwRegionsTouch((int)q->home1 - 1, LwRegionOfNode(node));
}
static int LwGarrisonStrength(const LwGarrison* g)
{
    int s = 0;
    for (uint32_t p = 0; p < g->points && p < 96; p++)
    {
        uint32_t bit = 1u << (p % 32), w = p / 32;
        if (g->retake) s += (g->allow[w] & g->u[w] & bit) != 0; else if (!g->cleared) s += (g->u[w] & bit) != 0;
    }
    return s;
}
static void LwTerritoryBuild(void)
{
    static uint16_t queue[LW_NODES_MAX];
    int head = 0, tail = 0, seeds = 0;
    memset(g_lwRegion, 0, sizeof g_lwRegion); memset(g_lwRegionAdj, 0, sizeof g_lwRegionAdj); memset(g_lwRegionHeld, 0, sizeof g_lwRegionHeld);
    for (int gi = 0; gi < g_lwGarrisonCount && gi < LW_REGIONS_MAX; gi++)
    {
        LwGarrison* g = &g_lwGarrisons[gi];
        if (!g->haveCenter) continue;
        int node = LwNearestNode(g->center[0], g->center[1]);
        if (node < 0 || g_lwRegion[node]) continue; /* two garrisons on one nearest node: the first keeps it */
        g_lwRegion[node] = (uint16_t)(gi + 1); queue[tail++] = (uint16_t)node; seeds++;
    }
    while (head < tail)
    {
        int a = queue[head++];
        for (int k = 0; k < g_lwNodes[a].degree; k++)
        { int b = g_lwNodes[a].next[k]; if (b < g_lwNodeCount && !g_lwRegion[b]) { g_lwRegion[b] = g_lwRegion[a]; queue[tail++] = (uint16_t)b; } }
    }
    for (int a = 0; a < g_lwNodeCount; a++) /* unreached: the nearest centre */
        if (!g_lwRegion[a])
        {
            float best = 3e38f; int pick = -1;
            for (int gi = 0; gi < g_lwGarrisonCount && gi < LW_REGIONS_MAX; gi++)
            { float d2 = g_lwGarrisons[gi].haveCenter ? LwDist2(g_lwGarrisons[gi].center, g_lwNodes[a].x, g_lwNodes[a].y) : 3e38f; if (d2 < best) { best = d2; pick = gi; } }
            if (pick >= 0) g_lwRegion[a] = (uint16_t)(pick + 1);
        }
    for (int a = 0; a < g_lwNodeCount; a++)
        for (int k = 0; k < g_lwNodes[a].degree; k++)
        {
            int b = g_lwNodes[a].next[k], ra = (int)g_lwRegion[a] - 1, rb = b < g_lwNodeCount ? (int)g_lwRegion[b] - 1 : -1;
            if (ra >= 0 && rb >= 0 && ra != rb) { g_lwRegionAdj[ra][rb / 8] |= (uint8_t)(1u << (rb % 8)); g_lwRegionAdj[rb][ra / 8] |= (uint8_t)(1u << (ra % 8)); }
        }
    memset(g_lwRegionNodes, 0, sizeof g_lwRegionNodes);
    for (int a = 0; a < g_lwNodeCount; a++) if (g_lwRegion[a]) g_lwRegionNodes[g_lwRegion[a] - 1]++;
    g_lwTerr.built = 1; g_lwTerr.seeded = 0; g_lwTerr.garrisons = g_lwGarrisonCount; g_lwTerr.owner = g_lwGarrisonOwner; g_lwTerr.nodes = g_lwNodeCount;
    for (int i = 0; i < g_lwWorld.count; i++) { LwSquad* q = &g_lwSquads[i]; q->home1 = q->state != LW_FREE ? (uint16_t)g_lwRegion[q->node < g_lwNodeCount ? q->node : 0] : 0; }
    Note("living world: territory: %d regions from %d garrisons over %d route nodes (%d reached through the routes)", seeds, g_lwGarrisonCount, g_lwNodeCount, tail);
    for (int gi = 0; gi < g_lwGarrisonCount && gi < LW_REGIONS_MAX; gi++) /* the map, for the evidence graphics */
    {
        LwGarrison* g = &g_lwGarrisons[gi]; int nodes = 0, nb = 0, homed = 0;
        if (!g->haveCenter) continue;
        for (int a = 0; a < g_lwNodeCount; a++) nodes += g_lwRegion[a] == gi + 1;
        for (int b = 0; b < g_lwGarrisonCount && b < LW_REGIONS_MAX; b++) nb += b != gi && LwRegionsTouch(gi, b);
        for (int i = 0; i < g_lwWorld.count; i++) homed += g_lwSquads[i].home1 == gi + 1;
        if (nodes) Note("living world: territory: region %d (garrison %08X%s) at (%.0f, %.0f): %d nodes, %d neighbours, %d squads homed, strength %d", gi, g->entity,
            g->plain ? ", plain" : "", (double)g->center[0], (double)g->center[1], nodes, nb, homed, LwGarrisonStrength(g));
    }
    char line[1100]; size_t len = 0; int first = 0;
    for (int a = 0; a <= g_lwNodeCount; a++)
    {
        if (a == g_lwNodeCount || len > 760) { if (len) Note("living world: territory nodes %d-%d: %s", first, a - 1, line); len = 0; first = a; line[0] = 0; if (a == g_lwNodeCount) break; }
        len += (size_t)snprintf(line + len, sizeof line - len, "%.0f,%.0f,%d;", (double)g_lwNodes[a].x, (double)g_lwNodes[a].y, (int)g_lwRegion[a] - 1);
    }
}
static void LwTerritoryTick(void)
{
    ULONGLONG now = GetTickCount64();
    if (g_lwWorld.populated != 1 || g_lwNodeCount <= 0 || g_lwGarrisonCount <= 0) return;
    if (g_lwTerr.built && (g_lwTerr.owner != g_lwGarrisonOwner || g_lwTerr.garrisons != g_lwGarrisonCount || g_lwTerr.nodes != g_lwNodeCount)) g_lwTerr.built = 0;
    if (!g_lwTerr.built)
    {
        for (int gi = 0; gi < g_lwGarrisonCount; gi++) if (g_lwGarrisons[gi].points && !g_lwGarrisons[gi].haveCenter) return; /* every centre read first */
        LwTerritoryBuild();
    }
    if (now < g_lwTerr.next) return;
    g_lwTerr.next = now + 5000;
    int regions = g_lwGarrisonCount < LW_REGIONS_MAX ? g_lwGarrisonCount : LW_REGIONS_MAX, held = 0, lost = 0, squads[LW_REGIONS_MAX];
    memset(squads, 0, sizeof squads);
    for (int i = 0; i < g_lwWorld.count; i++)
    {
        LwSquad* q = &g_lwSquads[i];
        if (q->state == LW_FREE || q->survivors == 0) continue;
        if (q->home1 == 0) q->home1 = (uint16_t)g_lwRegion[q->node < g_lwNodeCount ? q->node : 0];
        if (q->home1 && q->home1 <= regions) squads[q->home1 - 1] += q->survivors;
    }
    for (int r = 0; r < regions; r++) /* a garrison with no region of its own counts for the region of its nearest node */
        if (g_lwGarrisons[r].haveCenter && !g_lwRegionNodes[r])
        { int host = LwRegionOfNode(LwNearestNode(g_lwGarrisons[r].center[0], g_lwGarrisons[r].center[1])); if (host >= 0 && host < regions) squads[host] += LwGarrisonStrength(&g_lwGarrisons[r]); }
    for (int r = 0; r < regions; r++)
    {
        LwGarrison* g = &g_lwGarrisons[r];
        if (!g->haveCenter || !g_lwRegionNodes[r]) continue;
        int gs = LwGarrisonStrength(g), st = gs + squads[r]; /* squads[]: homed survivors, plus garrisons without a region of their own */
        uint8_t was = g_lwRegionHeld[r], now1 = st > 0;
        g_lwRegionStrength[r] = st;
        if (g_lwTerr.seeded && was != now1)
        {
            if (now1) g_lwTerr.regained++; else g_lwTerr.lost++;
            Note("living world: territory: region %d (garrison %08X at (%.0f, %.0f)) %s: garrison strength %d%s, squads homed there (and garrisons sharing its node) %d", r, g->entity,
                (double)g->center[0], (double)g->center[1], now1 ? "held again" : "lost", gs, g->retake ? " (retaken)" : g->cleared ? " (cleared)" : "", squads[r]);
        }
        g_lwRegionHeld[r] = now1;
        held += now1; lost += !now1;
    }
    g_lwTerr.seeded = 1;
    if (now >= g_lwTerr.noteAt)
    {
        g_lwTerr.noteAt = now + 30000;
        char list[700]; size_t len = 0;
        for (int r = 0; r < regions && len < sizeof list - 16; r++) if (g_lwRegionNodes[r]) len += (size_t)snprintf(list + len, sizeof list - len, "%d:%d ", r, g_lwRegionStrength[r]);
        Note("living world: territory (30 s): %d held, %d lost; %u lost and %u held again so far; strength by region: %s", held, lost, g_lwTerr.lost, g_lwTerr.regained, list);
    }
}

/* Test trace (HELLFORGE_LW_TEST_TRACE=1, evidence only): every intent change at once with the squad's place, the squads
 * with an intent every 10 s, and every living squad's place every 30 s (lwplot.py draws the tracks from these). */
static void LwIntentTrace(void)
{
    static int on = -1; static uint8_t last[LW_SQUADS_MAX], lastState[LW_SQUADS_MAX]; static ULONGLONG every10, every30; static uintptr_t level;
    if (on < 0) { char t[4] = ""; on = GetEnvironmentVariableA("HELLFORGE_LW_TEST_TRACE", t, sizeof t) > 0 && t[0] == '1'; }
    if (!on || g_lwWorld.populated != 1) return;
    static const char* const names[] = {"patrol", "investigate", "reinforce", "retake"};
    if (level != g_lwWorld.lg) { level = g_lwWorld.lg; memset(last, 0, sizeof last); memset(lastState, 0, sizeof lastState); }
    ULONGLONG now = GetTickCount64();
    int tick10 = now >= every10, tick30 = now >= every30;
    if (tick10) every10 = now + 10000;
    if (tick30) every30 = now + 30000;
    char line[1100]; size_t len = 0; int first = 0;
    for (int i = 0; i < g_lwWorld.count; i++)
    {
        LwSquad* q = &g_lwSquads[i];
        uint8_t in = q->intent < 4 ? q->intent : 0, st = q->state;
        if (in != last[i] || (st == LW_FREE) != (lastState[i] == LW_FREE))
            Note("living world: intent trace: squad %d %s -> %s at (%.0f, %.0f), %s, %u survivors, home region %d", i, names[last[i] < 4 ? last[i] : 0], st == LW_FREE ? "gone" : names[in],
                (double)q->pos[0], (double)q->pos[1], st == LW_ONLINE ? "online" : st == LW_OFFLINE ? "offline" : st == LW_PENDING ? "pending" : "free", q->survivors, (int)q->home1 - 1);
        else if (tick10 && in != LW_PATROL && st != LW_FREE)
            Note("living world: intent trace: squad %d %s at (%.0f, %.0f), %s, stage %u, target (%.0f, %.0f)", i, names[in], (double)q->pos[0], (double)q->pos[1],
                st == LW_ONLINE ? "online" : "offline", q->intentStage, (double)q->target[0], (double)q->target[1]);
        last[i] = in; lastState[i] = st;
        if (tick30 && st != LW_FREE && q->survivors)
        {
            if (len > 760) { Note("living world: intent trace: places from squad %d: %s", first, line); len = 0; }
            if (!len) { first = i; line[0] = 0; }
            len += (size_t)snprintf(line + len, sizeof line - len, "%d:%.0f,%.0f,%s;", i, (double)q->pos[0], (double)q->pos[1], names[in]);
        }
    }
    if (len) Note("living world: intent trace: places from squad %d: %s", first, line);
}
