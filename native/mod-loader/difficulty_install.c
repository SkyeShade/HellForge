/* Installing level 11 (difficulty.c and its parts): the checks that the game is the reviewed one, then the install's
 * steps, each in its order. Included by hellforge_modloader.c after every other difficulty file. */

/* After DifficultyCopiesFailed: every check, in this order; each NULL when its code and data are the reviewed ones. */
static const char* (*const kDifficultyChecks[])(uintptr_t game) = {
    DifficultyReadersFailed, KeepTenFailed, BandFailed, LevelReadersFailed, BoardFailed, SpreadFailed, HashInsertFailed,
    IconsFailed, RangesFailed, EnemyReadersFailed, RostersFailed, BarFailed,
    MapPinsFailed, StructuredFailed, ClusterPinsFailed, CityFlagsFailed, PreviewFailed,       /* a map_scale above 1 */
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
    InstallClusterGuards, InstallAppendGuards, InstallFootprintWatch, InstallNavGrid,             /* a map_scale above 1 */
    MoveGenerationBlock, JumpToRestartStubs, JumpToCapture, JumpToTelemetry, JumpToSpawners, JumpToMapView,
    JumpToTimeStubs, JumpToCardStubs, JumpToLevelStubs, JumpToMarkerStubs, JumpToObjectiveStubs};

/* Polled after GAME_LOADED until it returns 1: moves the table and adds level 11. pinsReady: the grace period for code
 * still being decoded is over. */
static int InstallDifficulty(uintptr_t game, int pinsReady)
{
    if (g_difficultyState != 0) return 1;
    if (!g_difficultyAdded) { g_difficultyState = 1; return 1; }
    int waiting;
    const char* why = DifficultySitesFailed(game, &waiting);
    if (why != NULL) return waiting || !pinsReady ? 0 : RefuseDifficulty(why);
    DifficultyInstall d = {game};
    for (size_t i = 0; i < sizeof kInstallSteps / sizeof kInstallSteps[0]; i++)
    {
        why = kInstallSteps[i](&d);
        if (why != NULL) return RefuseDifficulty(why);
    }
    g_difficultyGame = game;
    g_restartWatch = d.watch;
    why = WidenDifficultyBounds(game);
    if (why != NULL) return RefuseDifficulty(why);
    g_difficultyTable = d.table;
    g_difficultyState = 1;
    HookText();
    NoteDifficulty(&d);
    return 1;
}
