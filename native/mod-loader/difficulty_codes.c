/* Level 11's launch codes (difficulty.c, HF_DIFFICULTY_SECOND_PRIMARY): a mixed mission's code counts only the Launch
 * Codes pickups. Included by hellforge_modloader.c after difficulty_missions.c. */

/* A keypad code's digit i (of n) shows in the objective text once revealed / total >= (i + 1) / n, over every live
 * entity with KeypadCodeReveal whose key is the code's (0x9B9820; the HUD's two code texts are its only callers, and
 * the keypad takes the right code either way). Seven entities share the ICBM's key 0xE8153A02, the SSSD hard drives
 * among them, so the drives a partner mission brings (an uplink's, collected data's, SE equipment's) counted as parts
 * of the code: with the codes retrieved and two drives left the code read 5-?-?-?-? (research\d11-icbm-code). Levels
 * 1..10 never mix, and there a keypad mission's only carriers are its codes. 0x9B9820 looks each instance's record
 * (the key) up by its resource hash, calling 0x4F6A80 at 0x9B9864; that call goes to a stub that passes on the Launch
 * Codes (the intel and the carried codes) and gives every other carrier a zero key, which no code has. */
#define DIF_CODE_SITE 0x9B9861u   /* mov rcx, [rcx]; call 0x4F6A80; cmp [rax], r11d; jne */
#define DIF_CODE_CALL 0x9B9864u
#define DIF_CODE_LOOKUP 0x4F6A80u /* KeypadCodeRevealComponentData's record by resource hash */
#define DIF_LAUNCH_CODES 0xF48E9DEC9CC20D8Aull         /* generic_intel */
#define DIF_LAUNCH_CODES_CARRIED 0x5684D928C9AB00D1ull
#define DIF_CODE_STUB 48
static const uint8_t kCodeSitePin[13] = {0x48, 0x8B, 0x09, 0xE8, 0x17, 0xD2, 0xB3, 0xFF, 0x44, 0x39, 0x18, 0x75, 0x0E};
static const uint8_t kCodeLookupPin[8] = {0x48, 0x85, 0xC9, 0x74, 0x6C, 0x48, 0x8B, 0x05}; /* test rcx, rcx; je; mov rax, [rip] */
static const uint8_t kCodeStub[DIF_CODE_STUB] = {
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, /*  0 mov rax, the intel's hash (imm64 at 2) */
    0x48, 0x39, 0xC1,                   /* 10 cmp rcx, rax */
    0x74, 0x17,                         /* 13 je 38 */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, /* 15 mov rax, the carried codes' hash (imm64 at 17) */
    0x48, 0x39, 0xC1,                   /* 25 cmp rcx, rax */
    0x74, 0x08,                         /* 28 je 38 */
    0x48, 0x8D, 0x05, 0x07, 0, 0, 0,    /* 30 lea rax, [rip+7] (the zero key at 44) */
    0xC3,                               /* 37 ret */
    0xE9, 0, 0, 0, 0,                   /* 38 jmp 0x4F6A80 (rel32 at 39) */
    0xCC,                               /* 43 */
    0, 0, 0, 0};                        /* 44 the zero key */

/* The stub at stub (within 2 GB of lookup): the Launch Codes go on to lookup, any other carrier gets the zero key. */
static void LaunchCodesStub(uint8_t* stub, uintptr_t lookup)
{
    const uint64_t intel = DIF_LAUNCH_CODES, carried = DIF_LAUNCH_CODES_CARRIED;
    memcpy(stub, kCodeStub, sizeof kCodeStub);
    memcpy(stub + 2, &intel, 8);
    memcpy(stub + 17, &carried, 8);
    int32_t rel = (int32_t)((int64_t)lookup - (int64_t)(uintptr_t)(stub + 43));
    memcpy(stub + 39, &rel, 4);
}

/* With second primaries: the code text's lookup call and the lookup it calls. */
static const char* LaunchCodesFailed(uintptr_t game)
{
    uint8_t site[sizeof kCodeSitePin], lookup[sizeof kCodeLookupPin];
    if (!(g_difficulty.flags & HF_DIFFICULTY_SECOND_PRIMARY)) return NULL;
    if (SafeRead(game + DIF_CODE_SITE, site, sizeof site) || memcmp(site, kCodeSitePin, sizeof site) != 0) return "the launch code's count is not the reviewed one";
    if (SafeRead(game + DIF_CODE_LOOKUP, lookup, sizeof lookup) || memcmp(lookup, kCodeLookupPin, sizeof lookup) != 0) return "the launch code's lookup is not the reviewed one";
    return NULL;
}

static const char* InstallLaunchCodes(DifficultyInstall* d)
{
    if (!(g_difficulty.flags & HF_DIFFICULTY_SECOND_PRIMARY)) return NULL;
    uint8_t* stub = AllocateNearGame(d->game, DIF_CODE_STUB);
    DWORD protect;
    if (stub == NULL) return "no memory near game.dll for the launch code's stub";
    LaunchCodesStub(stub, d->game + DIF_CODE_LOOKUP);
    if (!VirtualProtect(stub, DIF_CODE_STUB, PAGE_EXECUTE_READ, &protect)) return "the launch code's stub could not be made executable";
    FlushInstructionCache(GetCurrentProcess(), stub, DIF_CODE_STUB);
    uint8_t call[5] = {0xE8};
    int32_t rel = DifficultyJumpRel(d->game + DIF_CODE_CALL, (uintptr_t)stub);
    memcpy(call + 1, &rel, 4);
    if (ApiWrite(d->game + DIF_CODE_CALL, call, sizeof call)) return "the launch code's count could not be given the stub";
    Note("difficulty: a mixed mission's launch code counts only its Launch Codes, not the partner's hard drives");
    return NULL;
}
