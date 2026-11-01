/* The SDK's table of functions (HfApi, native/mod-sdk/hellforge_mod.h), handed to every mod: the boot's set-ups and a
 * live change's (live_code.c). Included by hellforge_modloader.c after every function it lists. */

static void ApiInit(void)
{
    g_api = (HfApi){HF_MOD_SDK_VERSION, sizeof(HfApi), ApiLog, ApiBuildId, ApiModId, ApiModFolder, ApiOn, ApiModuleBase, ApiFindPattern,
        SafeRead, ApiModWrite, ApiModWriteU32If, ApiHook, ApiNetworkFieldType, ApiSetNetworkFieldBits, ApiComponentRecord, ApiAddWeapon, ApiWeaponEntity, ApiAddProjectile, ApiProjectileRow, ApiAddDamage, ApiDamageRow, ApiResourceHash,
        ApiAddStratagem, ApiStratagemRow, ApiAddEntity, ApiAddBehaviour, ApiAddDifficulty, ApiDifficultyRow,
        ApiRegisterEntity, ApiCopyField, ApiSetField, ApiSetTag, ApiMount, ApiRecord, ApiFind, ApiEditEntity, ApiSetLive, ApiGetLive, ApiSetPassives, ApiGetPassives,
        ApiOnGameThread, ApiLoadPackage, ApiLastLog, ApiLogsFolder, ApiGetStat, ApiSession, ApiSubscribe, ApiGameState, ApiContent, ApiAddAbility, ApiAddStratagemCategory, ApiSetStat, ApiCopyRow, ApiMods,
        ApiHeal, ApiHealLimb, ApiInjure, ActLimbState, ApiApplyStatus, ApiAddVelocity, ApiResupply, ApiSpawnExplosion, ApiSpawnProjectile,
        ApiCreditToPlayer, ApiFreezeTimer, ApiSpawnPelican, ApiModifyShots, ApiHoming, ApiStopShots, ApiNotice, ApiHookEx, ApiCodeMatches, ApiReadCode, ApiSyncState, ApiSyncMod, ApiSyncAnswer, ApiStatsSource,
        ApiLoadPhase, ApiUnhook};
}
