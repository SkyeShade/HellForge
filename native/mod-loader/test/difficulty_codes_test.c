/* A mixed level-11 mission's launch code (difficulty_codes.c): the stub passes the Launch Codes on to the lookup and
 * gives any other carrier a zero key; without second primaries nothing is checked or written; the pins refuse another
 * build; the install points the code text's call at the stub. Included by stats_test.c. */

static uint32_t g_dcKey = 0xE8153A02u; /* the record the fake lookup gives: the ICBM's code key */

static void LaunchCodesStubRuns(void)
{
    typedef const uint32_t* (*Lookup)(uint64_t hash);
    uint8_t* page = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(page != NULL);
    uint8_t* lookup = page + 0x100; /* mov rax, &g_dcKey; ret */
    uint64_t key = (uint64_t)(uintptr_t)&g_dcKey;
    lookup[0] = 0x48; lookup[1] = 0xB8; memcpy(lookup + 2, &key, 8); lookup[10] = 0xC3;
    LaunchCodesStub(page, (uintptr_t)lookup);
    Lookup stub = (Lookup)(void*)page;
    assert(stub(DIF_LAUNCH_CODES) == &g_dcKey && stub(DIF_LAUNCH_CODES_CARRIED) == &g_dcKey);
    const uint32_t* drive = stub(0xF41432892465C5FDull);   /* SSSD Hard Drive (carry_data) */
    const uint32_t* rack = stub(0x33F2BCDAE3A12592ull);    /* SSSD Hard Drive (carry_data_stratagem) */
    assert(drive != &g_dcKey && *drive == 0 && rack == drive);
    assert(*stub(0x9867D9EC6AC7E680ull) == 0);             /* Access Codes */
    VirtualFree(page, 0, MEM_RELEASE);
}

static void DifficultyCodesFixture(void)
{
    LaunchCodesStubRuns();
    uint8_t* game = VirtualAlloc(NULL, 0x9BA000, MEM_RESERVE, PAGE_NOACCESS);
    assert(game != NULL);
    assert(VirtualAlloc(game + 0x4F6000, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE) != NULL);
    assert(VirtualAlloc(game + 0x9B9000, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE) != NULL);
    uint32_t flags = g_difficulty.flags;
    DifficultyInstall d = {(uintptr_t)game};

    /* without second primaries: no check and no write */
    g_difficulty.flags = 0;
    assert(LaunchCodesFailed((uintptr_t)game) == NULL && InstallLaunchCodes(&d) == NULL && game[DIF_CODE_CALL] == 0);

    /* another build: refused, the site first, then the lookup */
    g_difficulty.flags = HF_DIFFICULTY_SECOND_PRIMARY;
    assert(LaunchCodesFailed((uintptr_t)game) != NULL);
    memcpy(game + DIF_CODE_SITE, kCodeSitePin, sizeof kCodeSitePin);
    assert(LaunchCodesFailed((uintptr_t)game) != NULL);
    memcpy(game + DIF_CODE_LOOKUP, kCodeLookupPin, sizeof kCodeLookupPin);
    assert(LaunchCodesFailed((uintptr_t)game) == NULL);

    /* the install: the call goes to the stub, whose jump goes to the lookup; the rest of the site stays */
    assert(InstallLaunchCodes(&d) == NULL);
    int32_t rel;
    assert(game[DIF_CODE_CALL] == 0xE8);
    memcpy(&rel, game + DIF_CODE_CALL + 1, 4);
    uint8_t* stub = game + DIF_CODE_CALL + 5 + rel;
    memcpy(&rel, stub + 39, 4);
    assert(stub[38] == 0xE9 && stub + 43 + rel == game + DIF_CODE_LOOKUP);
    assert(memcmp(game + DIF_CODE_SITE, kCodeSitePin, 3) == 0 && memcmp(game + DIF_CODE_SITE + 8, kCodeSitePin + 8, 5) == 0);

    g_difficulty.flags = flags;
    VirtualFree(game, 0, MEM_RELEASE);
}
