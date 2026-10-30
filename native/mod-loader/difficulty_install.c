/* Installing level 11 (difficulty.c and its parts): the checks that the game is the reviewed one, then the install's
 * steps, each in its order. Included by hellforge_modloader.c after every other difficulty file. */

/* After DifficultyCopiesFailed: every check, in this order; each NULL when its code and data are the reviewed ones. */
static const char* (*const kDifficultyChecks[])(uintptr_t game) = {
    DifficultyReadersFailed, KeepTenFailed, BandFailed, LevelReadersFailed, BoardFailed, SpreadFailed, HashInsertFailed,
    IconsFailed, RangesFailed, EnemyReadersFailed, RostersFailed, BarFailed,
    MapPinsFailed, StructuredFailed, ClusterPinsFailed, CityFlagsFailed, PreviewFailed, TownPinsFailed, /* a map_scale above 1 */
    LocationRefsFailed, ObjectiveListFailed, ObjectiveStubSitesFailed,                        /* the moved generation block */
    RestartSitesFailed, CaptureSiteFailed, MissionSitesFailed, CountdownSitesFailed, CardViewFailed, MapViewFailed};

/* NULL when every copy, instruction, bound and kept caller is the reviewed one; 1 in *waiting while the tables are
 * not filled yet. */
static const char* DifficultySitesFailed(uintptr_t game, int* waiting)
{
    const char* why = DifficultyCopiesFailed(game, waiting);
    for (size_t i = 0; why == NULL && i < sizeof kDifficultyChecks / sizeof kDifficultyChecks[0]; i++) why = kDifficultyChecks[i](game);
    return why;
}

/* The install, step by step: the table and the stubs' page, every stub prepared, the page made executable, level 11's
 * row; then the game's code and data pointed at them. A step returns why it failed (the level is then refused). */
static const char* (*const kInstallSteps[])(DifficultyInstall* d) = {
    AllocateTable, PrepareExtraWidget, PrepareIconStubs, PrepareBarStubs, PrepareObjectiveStubs, PrepareLevelStubs,
    PrepareMarkerStubs, PrepareMapStubs, PrepareCellStubs, PrepareRestartStubs, PrepareZoneStubs, PrepareLaunchStub,
    PrepareTimeStubs, PrepareHoloStubs, PrepareCardViewStub, PrepareMapViewStub, PrepareSpawnerStub, PrepareTelemetryStub,
    PrepareCaptureStub, PrepareUnreachableStub, ProtectStubs,
    MakeLevelRow, MoveTableReaders, WidenObjectives, MoveDistanceReaders, MoveRosterWeights, WidenCompositions,
    WidenMissionRanges, InstallBoard, InstallSpreadGuard, InstallHashInsertGuard, MixStep, InstallGrant, InstallRadar,
    MoveBandsAndIcons,
    JumpToMapStubs, MoveZoneTable, PrepareStructuredStubs, InstallVista, InstallCull, PrepareFlagsStubs, JumpToStructured,
    InstallClusterGuards, InstallAppendGuards, InstallClusterRings, InstallTowns, InstallFootprintWatch, InstallNavGrid, /* a map_scale above 1 */
    MoveGenerationBlock, JumpToRestartStubs, JumpToCapture, JumpToTelemetry, JumpToSpawners, JumpToMapView,
    JumpToTimeStubs, JumpToCardStubs, JumpToLevelStubs, JumpToMarkerStubs, JumpToObjectiveStubs};

/* B.2: the same steps by name, so a refusal says which one (and how many completed) kept their patches. */
static const char* const kInstallStepNames[] = {
    "AllocateTable", "PrepareExtraWidget", "PrepareIconStubs", "PrepareBarStubs", "PrepareObjectiveStubs", "PrepareLevelStubs",
    "PrepareMarkerStubs", "PrepareMapStubs", "PrepareCellStubs", "PrepareRestartStubs", "PrepareZoneStubs", "PrepareLaunchStub",
    "PrepareTimeStubs", "PrepareHoloStubs", "PrepareCardViewStub", "PrepareMapViewStub", "PrepareSpawnerStub", "PrepareTelemetryStub",
    "PrepareCaptureStub", "PrepareUnreachableStub", "ProtectStubs",
    "MakeLevelRow", "MoveTableReaders", "WidenObjectives", "MoveDistanceReaders", "MoveRosterWeights", "WidenCompositions",
    "WidenMissionRanges", "InstallBoard", "InstallSpreadGuard", "InstallHashInsertGuard", "MixStep", "InstallGrant", "InstallRadar",
    "MoveBandsAndIcons",
    "JumpToMapStubs", "MoveZoneTable", "PrepareStructuredStubs", "InstallVista", "InstallCull", "PrepareFlagsStubs", "JumpToStructured",
    "InstallClusterGuards", "InstallAppendGuards", "InstallClusterRings", "InstallTowns", "InstallFootprintWatch", "InstallNavGrid",
    "MoveGenerationBlock", "JumpToRestartStubs", "JumpToCapture", "JumpToTelemetry", "JumpToSpawners", "JumpToMapView",
    "JumpToTimeStubs", "JumpToCardStubs", "JumpToLevelStubs", "JumpToMarkerStubs", "JumpToObjectiveStubs"};
_Static_assert(sizeof kInstallStepNames / sizeof kInstallStepNames[0] == sizeof kInstallSteps / sizeof kInstallSteps[0],
    "every install step needs a name");
#define DIF_STEP_COUNT (sizeof kInstallSteps / sizeof kInstallSteps[0])

/* One step, and its line: done, or the refusal that leaves this step partly applied and every earlier one applied. */
static const char* RunInstallStep(DifficultyInstall* d, size_t i)
{
    const char* why = kInstallSteps[i](d);
    if (why == NULL) NoteDiagnostic("difficulty: install step %zu/%zu %s done; state 0", i + 1, DIF_STEP_COUNT, kInstallStepNames[i]);
    else NoteDiagnostic("difficulty: install refused at step %zu/%zu %s: %s; %zu step(s) complete plus step %zu partial; state -1",
        i + 1, DIF_STEP_COUNT, kInstallStepNames[i], why, i, i + 1);
    return why;
}

/* Polled after GAME_LOADED until it returns 1: moves the table and adds level 11. pinsReady: the grace period for code
 * still being decoded is over. */
static int InstallDifficulty(uintptr_t game, int pinsReady)
{
    if (g_difficultyState != 0) return 1;
    if (!g_difficultyAdded) { g_difficultyState = 1; return 1; }
    g_difficultyAttempts++;
    int waiting;
    const char* why = DifficultySitesFailed(game, &waiting);
    if (why != NULL)
    {
        if (waiting || !pinsReady)
        {
            if (g_difficultyAttempts == 1)
                NoteDiagnostic("difficulty: decode wait: first check at +%llu ms: %s", (unsigned long long)(GetTickCount64() - g_difficultySeen), why);
            return 0;
        }
        NoteDiagnostic("difficulty: decode wait: first seen tick=%llu, pinsReady at +20000 ms, refused at +%llu ms after %d attempt(s), first failing check: %s",
            (unsigned long long)g_difficultySeen, (unsigned long long)(GetTickCount64() - g_difficultySeen), g_difficultyAttempts, why);
        NoteDiagnostic("difficulty: install refused at the checks before step 1/%zu: %s; 0 step(s) complete; state -1", DIF_STEP_COUNT, why);
        return RefuseDifficulty(why);
    }
    NoteDiagnostic("difficulty: decode wait: first seen tick=%llu, pinsReady at +20000 ms, install started at +%llu ms after %d attempt(s)",
        (unsigned long long)g_difficultySeen, (unsigned long long)(GetTickCount64() - g_difficultySeen), g_difficultyAttempts);
    DifficultyInstall d = {game};
    g_patchAuditOn = 1;
    for (size_t i = 0; i < DIF_STEP_COUNT; i++)
    {
        why = RunInstallStep(&d, i);
        if (why != NULL) { g_patchAuditOn = 0; return RefuseDifficulty(why); }
    }
    g_difficultyGame = game;
    g_restartWatch = d.watch;
    why = WidenDifficultyBounds(game);
    g_patchAuditOn = 0;
    if (why != NULL)
    {
        NoteDiagnostic("difficulty: install refused after step %zu/%zu WidenDifficultyBounds: %s; all %zu step(s) complete; state -1",
            DIF_STEP_COUNT, DIF_STEP_COUNT, why, DIF_STEP_COUNT);
        return RefuseDifficulty(why);
    }
    g_difficultyTable = d.table;
    g_difficultyState = 1;
    NoteDiagnostic("difficulty: install steps: all %zu done; WidenDifficultyBounds done; state 1", DIF_STEP_COUNT);
    HookText();
    NoteDifficulty(&d);
    return 1;
}
