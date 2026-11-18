/* async-ai (ai_brain.c, ai_async.c): the pins refuse any other bytes, and the Devastator's brain on synthetic snapshots
 * notices a Helldiver it can see, not one behind a wall or behind its back, engages one it heard, keeps its distance,
 * faces and fires in bursts, and lets a dead one go. Included by stats_test.c. */

static AbSnapshot g_abtSnap;
static AbIntent g_abtOut[4];
static uint8_t g_abtThought[4];
static float g_abtGoal[3]; /* the last goal it moved to */

/* One agent at the origin looking along +y with one attack (ability 7, 0-80 m), one Helldiver at (x, y). */
static void AbtScene(float x, float y, int visible)
{
    memset(&g_abtSnap, 0, sizeof g_abtSnap);
    memset(g_abMemory, 0, sizeof g_abMemory);
    g_abtSnap.agentCount = 1; g_abtSnap.playerCount = 1; g_abtSnap.now = 100.0;
    AbAgentIn* a = &g_abtSnap.agents[0];
    a->entity = 0x400001; a->slot = 3; a->fresh = 1; a->fwd[1] = 1.0f; a->heardPlayer = -1; a->heardAge = -1.0f;
    a->attackCount = 1; a->attacks[0].ability = 7; a->attacks[0].maxRange = 80.0f;
    a->rayPlayer = (int8_t)(visible >= 0 ? 0 : -1); a->rayVisible = (uint8_t)(visible > 0);
    g_abtSnap.players[0].entity = 0x400002; g_abtSnap.players[0].alive = 1;
    g_abtSnap.players[0].pos[0] = x; g_abtSnap.players[0].pos[1] = y;
}

/* Thinks for the given seconds of game time (each step is one think of the agent); counts what it asked for. */
static void AbtRun(double seconds, int* moves, int* fires)
{
    for (double t = 0.0; t < seconds; t += 0.1)
    {
        g_abtSnap.frame = (g_abtSnap.frame / AB_THINK_EVERY + 1u) * AB_THINK_EVERY - 3u; /* slot 3's turn */
        g_abtSnap.now += 0.1;
        AbRun(&g_abtSnap, g_abtOut, g_abtThought);
        g_abtSnap.agents[0].fresh = 0;
        assert(g_abtThought[0] == 1);
        if (g_abtOut[0].move) memcpy(g_abtGoal, g_abtOut[0].goal, 12);
        if (moves) *moves += g_abtOut[0].move;
        if (fires) *fires += g_abtOut[0].fire != AB_FIRE_NONE;
    }
}

static void AbtPins(void)
{
    uint8_t* image = (uint8_t*)VirtualAlloc(NULL, 0x1800000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(image != NULL);
    for (int f = 0; f < AF_COUNT; f++) memcpy(image + kAaFunctions[f].rva, kAaFunctions[f].pin, 24);
    for (int f = 0; f < 3; f++) memcpy(image + kAaHooked[f].rva, kAaHooked[f].pin, 24);
    memcpy(image + VB_STEER, kVbSteerPin, sizeof kVbSteerPin);
    assert(AaMismatch((uintptr_t)image) == NULL);
    image[AA_STATE_SWITCH + 6] ^= 1;
    assert(AaMismatch((uintptr_t)image) != NULL);
    image[AA_STATE_SWITCH + 6] ^= 1;
    image[kAaFunctions[AF_RAY].rva + 3] ^= 1;
    assert(AaMismatch((uintptr_t)image) != NULL);
    VirtualFree(image, 0, MEM_RELEASE);
}

/* Phase 2's crowd: a lone agent walks its path at its speed facing where it goes; two on top of each other push
 * apart; one fighting faces its target while it strafes. */
static void AbtCrowd(void)
{
    static AbMove moves[4];
    AbtScene(0.0f, 30.0f, 1);
    AbAgentIn* a = &g_abtSnap.agents[0];
    a->crowd = 1; a->pathOk = 1; a->pathDir[0] = 1.0f; a->speed = 3.0f;
    assert(AbCrowd(&g_abtSnap, moves) == 1);
    assert(fabsf(moves[0].vel[0] - 3.0f) < 1e-3f && fabsf(moves[0].vel[1]) < 1e-3f && moves[0].hasFace && moves[0].face[0] > 0.99f);
    g_abtSnap.agentCount = 2;
    g_abtSnap.agents[1] = *a; g_abtSnap.agents[1].entity = 0x400005; g_abtSnap.agents[1].slot = 4;
    a->pathOk = 0; g_abtSnap.agents[1].pathOk = 0; g_abtSnap.agents[1].pos[0] = 0.8f;
    AbCrowd(&g_abtSnap, moves);
    assert(moves[0].vel[0] < -0.1f && moves[1].vel[0] > 0.1f);
    g_abtSnap.agentCount = 1; a->pathOk = 1;
    AbtRun(1.0, NULL, NULL);
    AbCrowd(&g_abtSnap, moves);
    assert(moves[0].hasFace && moves[0].face[1] > 0.99f); /* faces the Helldiver at +y while walking +x */
}

/* Phase 5: an agent far from every Helldiver thinks every AB_THINK_FAR frames, a near one every AB_THINK_EVERY. */
static void AbtLod(void)
{
    AbtScene(0.0f, 30.0f, 1);
    AbtRun(0.1, NULL, NULL);
    int close = 0, distant = 0;
    for (uint32_t f = 0; f < 60; f++) { g_abtSnap.frame = f; AbRun(&g_abtSnap, g_abtOut, g_abtThought); close += g_abtThought[0]; }
    g_abtSnap.agents[0].distant = 1;
    for (uint32_t f = 0; f < 60; f++) { g_abtSnap.frame = f; AbRun(&g_abtSnap, g_abtOut, g_abtThought); distant += g_abtThought[0]; }
    assert(close == 60 / AB_THINK_EVERY && distant == 60 / AB_THINK_FAR);
}

static void AsyncAiFixture(void)
{
    AbtLod();
    AbtCrowd();
    AbtPins();
    int moves = 0, fires = 0;
    /* seen at 30 m ahead: engages within a second, faces it, fires bursts with pauses, holds its distance by strafing */
    AbtScene(0.0f, 30.0f, 1);
    AbtRun(1.0, NULL, NULL);
    assert(g_abtOut[0].engaged && g_abtOut[0].target == 0 && g_abtOut[0].seen && g_abtOut[0].ray == 0);
    assert(g_abtOut[0].face && g_abtOut[0].faceDir[1] > 0.99f);
    AbtRun(10.0, &moves, &fires);
    assert(fires >= 6 && fires <= 30 && moves >= 2);
    assert(g_abtGoal[1] < 5.0f && g_abtGoal[1] > -5.0f); /* strafing beside itself, 30 m off */
    /* far (50 m): moves in to about 25 m from the Helldiver */
    AbtScene(0.0f, 50.0f, 1);
    moves = 0;
    AbtRun(2.0, &moves, NULL);
    assert(moves >= 1 && g_abtGoal[1] > 20.0f && g_abtGoal[1] < 30.0f);
    /* behind a wall, or behind its back beyond the rear range: never engages */
    AbtScene(0.0f, 30.0f, 0);
    AbtRun(5.0, NULL, NULL);
    assert(!g_abtOut[0].engaged);
    AbtScene(0.0f, -45.0f, 1);
    AbtRun(5.0, NULL, NULL);
    assert(!g_abtOut[0].engaged);
    /* heard, not seen: engages and goes where it was heard */
    AbtScene(0.0f, -45.0f, 0);
    g_abtSnap.agents[0].heardPlayer = 0; g_abtSnap.agents[0].heardAge = 0.2f;
    AbtRun(0.3, NULL, NULL);
    assert(g_abtOut[0].engaged && g_abtOut[0].target == 0 && !g_abtOut[0].seen);
    /* its target dies: lets go */
    AbtScene(0.0f, 30.0f, 1);
    AbtRun(1.0, NULL, NULL);
    assert(g_abtOut[0].engaged);
    g_abtSnap.players[0].alive = 0;
    AbtRun(0.3, NULL, NULL);
    assert(!g_abtOut[0].engaged && g_abtOut[0].target == -1);
    printf("async-ai: the brain notices, engages, keeps its distance, fires in bursts and lets go; the crowd walks its path, pushes apart and faces its target; far ones think less; pins refuse other bytes\n");
}
