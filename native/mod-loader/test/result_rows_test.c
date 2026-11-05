/* The mission result's objective rows and the summary panel past 32 instances (difficulty_missions.c: ResultObjectives,
 * PanelObjectives): rows as the game writes them while they fit, otherwise 31 kept (the completed first) with the live
 * event's row inside the 32 places and the participants after them untouched; badges within 27 in the loop and 32 after;
 * the panel's tables within their 32 places and their count. On a fake game.dll image with fake helpers. Included by
 * stats_test.c. */

#define RR_IMAGE 0x3500000u
#define RR_LIVE_ID 9000u
static uint8_t* g_rrGame;
static uint8_t g_rrManager[0x80], g_rrEvents[0x40];
static uint32_t g_rrTable[2 * 256];
static uintptr_t g_rrList[64];
static uint8_t g_rrInstances[64 * 0x1078], g_rrPointees[64][0x20];
static uint8_t g_rrRecords[64][0x20], g_rrSettings[64][0xAD0], g_rrDefs[64][0x60], g_rrLiveRecord[0x20];
static uint32_t g_rrCount;

static uint32_t* RrInstanceId(uintptr_t manager, uint32_t* id, uint32_t index)
{
    *id = index < g_rrCount ? 100u + index : index == g_rrCount ? RR_LIVE_ID : *(uint32_t*)(g_rrGame + DIF_NO_INSTANCE);
    return id;
}
static uintptr_t RrRecord(uint32_t id)
{
    if (id == RR_LIVE_ID) return (uintptr_t)g_rrLiveRecord;
    return id >= 100u && id < 100u + g_rrCount ? (uintptr_t)g_rrRecords[id - 100u] : 0;
}
static uintptr_t RrSettings(uintptr_t key) { return key >= 100u && key < 100u + g_rrCount ? (uintptr_t)g_rrSettings[key - 100u] : 0; }
static uintptr_t RrObjective(uint32_t id) { return id >= 100u && id < 164u ? (uintptr_t)g_rrDefs[id - 100u] : (uintptr_t)g_rrDefs[63]; }
static float RrProgress(uintptr_t unused, uint32_t id) { return id % 2 ? 0.5f : 0.25f; }

/* count instances; instance i: entity 100 + i, class cls(i), completed when done(i), a badge when badge(i) */
static void RrWorld(uint32_t count, int live, int32_t (*cls)(uint32_t), int (*done)(uint32_t), int (*badge)(uint32_t))
{
    memset(g_rrManager, 0, sizeof g_rrManager); memset(g_rrInstances, 0, sizeof g_rrInstances);
    g_rrCount = count;
    uint32_t places = 256, empty = 0xFFFFFFFFu, mul = 1;
    for (uint32_t i = 0; i < 2 * places; i += 2) { g_rrTable[i] = empty; g_rrTable[i + 1] = 0; }
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t id = 100u + i, slot = id & (places - 1);
        while (g_rrTable[2 * slot] != empty) slot = (slot + 1) & (places - 1);
        g_rrTable[2 * slot] = id; g_rrTable[2 * slot + 1] = i;
        uint64_t key = id; memcpy(g_rrRecords[i], &key, 8);
        uint32_t rowKey = 0x500u + i; memcpy(g_rrRecords[i] + 0x10, &rowKey, 4);
        int32_t c = cls(i); memcpy(g_rrSettings[i] + 0xAC0, &c, 4); g_rrSettings[i][0xABD] = 1;
        uint32_t hash = 0xD000u + i, b = badge(i) ? 0xBADu : 0; memcpy(g_rrDefs[i], &hash, 4); memcpy(g_rrDefs[i] + 0x54, &b, 4);
        int32_t state = done(i) ? 2 : 1; memcpy(g_rrInstances + i * 0x1078u + 0x1018, &state, 4); memcpy(g_rrInstances + i * 0x1078u + 0x1038, &c, 4);
        uint32_t entity = id, panelKey = 0x600u + i; memcpy(g_rrPointees[i] + 8, &entity, 4); memcpy(g_rrPointees[i] + 0x10, &panelKey, 4);
        g_rrList[i] = (uintptr_t)g_rrPointees[i];
    }
    uint32_t liveKey = 0x777u; memcpy(g_rrLiveRecord + 0x10, &liveKey, 4);
    memcpy(g_rrManager + 0x24, &count, 4);
    uintptr_t table = (uintptr_t)g_rrTable, list = (uintptr_t)g_rrList, instances = (uintptr_t)g_rrInstances;
    memcpy(g_rrManager + 0x38, &table, 8); memcpy(g_rrManager + 0x40, &places, 4); memcpy(g_rrManager + 0x44, &empty, 4);
    memcpy(g_rrManager + 0x48, &mul, 4); memcpy(g_rrManager + 0x50, &list, 8); memcpy(g_rrManager + 0x60, &instances, 8);
    uintptr_t manager = (uintptr_t)g_rrManager, events = (uintptr_t)g_rrEvents;
    memcpy(g_rrGame + DIF_OBJECTIVE_MANAGER, &manager, 8); memcpy(g_rrGame + DIF_LIVE_EVENTS, &events, 8);
    uint32_t liveCount = live ? 1u : 0u; memcpy(g_rrEvents + 0x24, &liveCount, 4);
}

static int32_t RrClassMixed(uint32_t i) { return i < 4 ? 0 : i % 5 == 0 ? 6 : 3; }
static int RrDoneLate(uint32_t i) { return i >= 30; }
static int RrNoBadge(uint32_t i) { return 0; }
static int RrEveryThirdBadge(uint32_t i) { return i % 3 == 1; }

static void ResultRowsFixture(void)
{
    g_rrGame = (uint8_t*)VirtualAlloc(NULL, RR_IMAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(g_rrGame != NULL);
    g_resultGame = (uintptr_t)g_rrGame;
    g_resultFns.instanceId = RrInstanceId; g_resultFns.record = RrRecord; g_resultFns.objective = RrObjective;
    g_resultFns.settings = RrSettings; g_resultFns.progress = RrProgress;
    uint32_t none = 0xFFFFFFFEu, noEntity = 0xFFFFFFFDu; float one = 1.0f;
    memcpy(g_rrGame + DIF_NO_INSTANCE, &none, 4); memcpy(g_rrGame + DIF_NO_ENTITY, &noEntity, 4); memcpy(g_rrGame + 0x23C6D70u, &one, 4);
    static uint8_t result[0x15F0], frame[0x60];
    uint32_t mult = 2;

    /* fits: 5 instances and the live event, rows at their instance's index, the live row at 5 */
    RrWorld(5, 1, RrClassMixed, RrDoneLate, RrNoBadge);
    memset(result, 0, sizeof result); memset(frame, 0, sizeof frame); memcpy(result + 0x54, &mult, 4);
    ResultObjectives(result, frame);
    for (uint32_t i = 0; i < 5; i++)
    {
        uint32_t key, hash; float p;
        memcpy(&key, result + 0xC8 + 0x10 * i, 4); memcpy(&hash, result + 0xCC + 0x10 * i, 4); memcpy(&p, result + 0xD4 + 0x10 * i, 4);
        assert(key == 0x500u + i && hash == 0xD000u + i && result[0xD0 + 0x10 * i] == 0 && p == RrProgress(0, 100u + i));
    }
    { uint32_t key, hash; memcpy(&key, result + 0xC8 + 0x50, 4); memcpy(&hash, result + 0xCC + 0x50, 4);
      assert(key == 0x777u && hash == DIF_LIVE_EVENT_OBJECTIVE && result[0xD0 + 0x50] == 1); }
    int32_t badges; memcpy(&badges, result + 0x15C8, 4);
    assert(badges == 2); /* main progress (capped at 1.0) and the live event; no optional one completed, so no optional total */
    { uint32_t id; float a; memcpy(&id, result + 0x14C8, 4); memcpy(&a, result + 0x14CC, 4); assert(id == 0x34FFE656u && a == 1.0f); }

    /* past 32: 40 instances (10 completed at the end, 8 of class 6), the live event; 31 rows, completed first */
    RrWorld(40, 1, RrClassMixed, RrDoneLate, RrEveryThirdBadge);
    memset(result, 0, sizeof result); memset(frame, 0, sizeof frame); memcpy(result + 0x54, &mult, 4);
    memset(result + 0x2C8, 0xAB, 0x800);
    int32_t before = 20; memcpy(result + 0x15C8, &before, 4);
    ResultObjectives(result, frame);
    for (uint32_t i = 0; i < 0x800; i++) assert(result[0x2C8 + i] == 0xAB); /* participants untouched */
    uint32_t done = 0, mains = 0, last = 0;
    for (uint32_t r = 0; r < 31; r++)
    {
        uint32_t key; memcpy(&key, result + 0xC8 + 0x10 * r, 4);
        assert(key >= 0x500u && key < 0x500u + 40 && (r == 0 || key > last)); /* the game's order */
        last = key;
        if (result[0xD0 + 0x10 * r]) done++;
        if (key - 0x500u < 4) mains++;
    }
    assert(done == 10 && mains == 4);
    { uint32_t key, hash; memcpy(&key, result + 0xC8 + 0x10 * 31, 4); memcpy(&hash, result + 0xCC + 0x10 * 31, 4);
      assert(key == 0x777u && hash == DIF_LIVE_EVENT_OBJECTIVE); }
    memcpy(&badges, result + 0x15C8, 4);
    assert(badges == 30); /* 7 of the 13 instance badges (to 27), then main, optional and the live event */

    /* the panel: fits as the game fills it; past 32 at most 31 + the live event, class 6 left out, count after them */
    static uint8_t panel[0x1C0C0];
    RrWorld(5, 1, RrClassMixed, RrDoneLate, RrNoBadge);
    memset(panel, 0xCD, sizeof panel);
    PanelObjectives(panel, (uintptr_t)g_rrManager);
    { uint32_t n; memcpy(&n, panel + 0x1BED0, 4); assert(n == 6);
      for (uint32_t i = 0; i < 5; i++) { uint32_t c, k, h; memcpy(&c, panel + 0x1BD50 + 4 * i, 4); memcpy(&k, panel + 0x1BDD0 + 8 * i, 4); memcpy(&h, panel + 0x1BDD4 + 8 * i, 4);
          assert(c == (uint32_t)RrClassMixed(i) && k == 0x600u + i && h == 0xD000u + i); }
      uint32_t c; memcpy(&c, panel + 0x1BD50 + 20, 4); assert(c == 2); }
    RrWorld(40, 1, RrClassMixed, RrDoneLate, RrNoBadge);
    memset(panel, 0xCD, sizeof panel);
    PanelObjectives(panel, (uintptr_t)g_rrManager);
    { uint32_t n; memcpy(&n, panel + 0x1BED0, 4); assert(n == 32);
      for (uint32_t i = 0; i < 31; i++) { uint32_t c; memcpy(&c, panel + 0x1BD50 + 4 * i, 4); assert(c <= 4); }
      assert(panel[0x1BED4] == 0xCD); }
    VirtualFree(g_rrGame, 0, MEM_RELEASE);
    g_resultGame = 0;
    memset(&g_resultFns, 0, sizeof g_resultFns);
    puts("PASS mission result: rows as the game writes them while they fit, past 32 the completed first in the game's order with "
        "the live event's row inside the 32 places and the participants untouched, badges within 32; the summary panel's tables "
        "within their places");
}
