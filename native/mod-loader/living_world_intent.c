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
 *   LW_HEAR_NEAR of the point each member is told: 0x888E50(NULL, member, source, point, 40.0) writes the hearing entry
 *   a player's shot writes, so the awareness update runs the game's own investigation (alert 2, behaviour state 7, a
 *   point 3-9 m around the centre) and returns the member to its squad after its settings' timer. The intent ends when
 *   every member is back at alert 0 (at least 10 s after) or after 90 s; a member at alert 3 or more ends it at once
 *   (the fight is the game's). */
#define LW_HEAR_FAR 4.0f    /* reach = the template's range x loudness x this (D) */
#define LW_HEAR_MAX 600.0f  /* at most (D) */
#define LW_HEAR_NEAR 30.0f  /* online: members are told within this of the point (D) */
#define LW_HEAR_SQUADS 2    /* squads sent per noise event (D) */
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
        !ApiCodeMatches(game + 0x5B36DDu, "48 8B 35 34 2D D7 02") || !ApiCodeMatches(game + 0x5B36E6u, "44 8B 4E 38") || !ApiCodeMatches(game + 0x5B36FDu, "4C 8B 5E 30 8B 5E 3C"))
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
static void LwIntentEnd(LwSquad* q, const char* why)
{
    ULONGLONG now = GetTickCount64();
    char peak[96] = "";
    if (q->state == LW_ONLINE && q->intentStage) snprintf(peak, sizeof peak, "; %d of %d members investigated (alert 2+, the first after %.1f s)", LwBits(q->intentPeak), q->seenCount, (double)q->intentPeakAt);
    Note("living world: squad %d investigation ends (%s) after %.0f s, %s; %s (%.0f m from it, %.0f m when sent)%s", (int)(q - g_lwSquads), why, (double)(now - q->intentAt) / 1000.0,
        q->state == LW_ONLINE ? "online" : "offline", q->intentStage ? "it reached the point" : "it did not reach the point",
        (double)sqrtf(LwDist2(q->pos, q->target[0], q->target[1])), (double)q->intentFrom, peak);
    q->intent = LW_PATROL; q->intentStage = 0; q->intentFree = now + LW_INTENT_REST; g_lwHear.ended++;
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
    if (q->intent != LW_INVESTIGATE) return 0;
    if (q->intentStage == 0) { q->intentSent = 1; if (!LwSquadGoTo(q, q->target)) LwIntentEnd(q, "no navmesh at the point"); }
    return 1;
}
/* What 0x888E50 left for the first member (the write's own exits: the member not in the detector hash [game+0x3326548]
 * +0x30, its entity flag +0x14 bit 0 clear, the source's tag bit 25 from 0x4A9710 (Tags [game+0x3326430] hash +0x18,
 * values [+0x38] + 8 idx), the source not in the faction registry (then slot 0 +0x1228, which the awareness update
 * never reads)); the entry in the hostile list (+0x310 count, +0x318 + 0x50 i: +0 source, +0x20 heard time, +0x38
 * hearing, +0x48 flags) is what the awareness update compares with the settings' +0x2BC (0x7EB694). */
static void LwHearReadBack(const LwSquad* q, uint32_t member, uint32_t source)
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
static void LwIntentOnline(LwSquad* q, const uint32_t* members, int n, const float* at)
{
    if (q->intent != LW_INVESTIGATE || n <= 0) return;
    ULONGLONG now = GetTickCount64();
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
        if (source) for (int k = 0; k < n; k++) { ((HearFn)(g_lw.game + 0x888E50u))(NULL, members[k], source, q->target, 40.0f); told++; }
        LwHearReadBack(q, members[0], source);
        g_lwHear.told += (uint32_t)told;
        q->intentStage = 1; q->intentStageAt = now; q->intentPeak = 0; q->intentPeakAt = 0;
        /* the awareness update's gates before the hearing test (0x7EB000): AiEnemy +0xE4 bits 0x2 / 0x4 / 0x8 / 0x20 take
         * another branch (0x7EB1E6..0x7EB241), and synced state 0x10 ([AiEnemy +0x2360] + 20 idx, +0) skips the unit (0x7EB0CD, 0x7EB0FD, 0x7EB136) */
        uintptr_t ai = 0, recs = 0; uint32_t idx = 0, flags = 0xFFFFFFFFu, aiState = 0xFFFFFFFFu;
        if (EvpPtr(g_lw.game + 0x33266C8u, &ai) && EvpHash(ai + 0x2330, members[0], 8, 4, &idx) && EvpPtr(ai + 0x2350, &recs))
        { flags = EvpU32(recs + 0xA50ull * idx + 0xE4, 0xFFFFFFFFu); uintptr_t st; if (EvpPtr(ai + 0x2360, &st)) aiState = EvpU32(st + 20ull * idx, 0xFFFFFFFFu); }
        Note("living world: squad %d reached the noise at (%.0f, %.0f) online after %.0f s; %d members told (source %08X); first member's AiEnemy flags %08X, synced state %u", (int)(q - g_lwSquads),
            (double)q->target[0], (double)q->target[1], (double)(now - q->intentAt) / 1000.0, told, source, flags, aiState);
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
    if (q->intent != LW_INVESTIGATE) return;
    q->intentSent = 0;
    if (q->intentStage != 0) LwIntentEnd(q, "it went offline while investigating");
    else if (!LwIntentRoute(q)) LwIntentEnd(q, "no route to the point");
}

static void LwInvestigate(const LwEvent* e)
{
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
