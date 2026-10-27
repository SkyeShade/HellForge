/* Weapon copies that use their base's attachments (research content-limits\shared-attachments). The game keeps 4,096
 * catalogue items, and a copy with attachments of its own brings one item per attachment of its base (57 for a
 * Liberator), so about 20 Liberator copies fit; sharing its base's, a copy is one item, and about 1,100 fit.
 * Included by hellforge_modloader.c, after registry_load.c.
 *
 * A weapon's attachments are catalogue items whose parent is the weapon and whose id is WeaponChild(attachment, weapon).
 * A copy without items of its own crashed the customization screen (0x1493770) and had its saved attachments reset
 * (0x1036390). So wherever the game goes from a weapon to its attachment items, a copy's id is read as its base's: 15
 * places, each the game's own code with one id translated (BaseOf: copy -> the vanilla weapon it copies, else itself).
 * The weapon keeps its own id everywhere else: its armory entry, its saved customization (the slots hold attachment
 * ids, not items), Customize/v2, the loadout, its network type. A copy also shares its base's mastery package and
 * level (registry_load.c OwnCustomization leaves the package alone), which keeps package positions under 256 too.
 *   function entries, the weapon id in a register: the attachment list 0x1370420 (r9d), the new-attachment markers
 *     0x10345F0 / 0x10348F0 (edx), FindChild 0x1372180 (r8d);
 *   in place: the customization screen reads the weapon once into [rsp+0x48] (0x1493A5A, translated) and its unlock
 *     and reward matching (0x1493B5E, 0x1493D85) read that instead of the weapon again;
 *   mid-function, through stubs: the saved customization's check 0x103650D (ebx), the loadout item list 0x1372444
 *     (r11d), the loadout's mixIds 0x135D6A3 / 0x135D794 / 0x135D884 (eax, after the weapon's own id is stored), and
 *     the screen's lambdas 0x1494050 / 0x14940B1 / 0x14941B9 (defensive: no static caller reaches them).
 * Flags are dead after each mid-function site (a flag write comes first). On only with HELLFORGE_SHARED_ATTACHMENTS
 * set (not "0") until it is tested in game; then the backend serves copies without children (registered-items.json
 * "sharedChildren"). */

static uint8_t* AllocateNearGame(uintptr_t game, size_t bytes); /* damage.c */

typedef struct CopyBase { uint32_t copy, base; } CopyBase;
static struct { uint32_t count, pad; CopyBase pairs[REG_MAX_WEAPONS]; } g_copyBases; /* BaseOf's table */
static int g_sharedSetting = -1; /* -1 not read yet */
static void* g_sharedOriginal[4];

/* BaseOf: eax in and out, every other register kept. {push rcx; push rdx; mov rcx, &table; mov edx, [rcx]; add rcx,
 * 8; loop: test edx, edx; je done; cmp [rcx], eax; je found; add rcx, 8; dec edx; jmp loop; found: mov eax, [rcx+4];
 * done: pop rdx; pop rcx; ret}, the table's address at 4. */
static const uint8_t kBaseOf[40] = {0x51, 0x52, 0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0, 0x8B, 0x11, 0x48, 0x83, 0xC1, 0x08, 0x85, 0xD2,
    0x74, 0x0F, 0x39, 0x01, 0x74, 0x08, 0x48, 0x83, 0xC1, 0x08, 0xFF, 0xCA, 0xEB, 0xF0, 0x8B, 0x41, 0x04, 0x5A, 0x59, 0xC3};

/* Function entries: {rva, its first instructions (pinned), mov eax, reg; mov reg, eax}. */
typedef struct SharedEntry { uint32_t rva; uint8_t pin[10], length; uint8_t in[3], out[3]; } SharedEntry;
static const SharedEntry kSharedEntries[4] = {
    {0x1370420u, {0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10}, 10, {0x44, 0x89, 0xC8}, {0x41, 0x89, 0xC1}}, /* r9d */
    {0x10345F0u, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10}, 10, {0x90, 0x89, 0xD0}, {0x90, 0x89, 0xC2}}, /* edx */
    {0x10348F0u, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08}, 10, {0x90, 0x89, 0xD0}, {0x90, 0x89, 0xC2}}, /* edx */
    {0x1372180u, {0x69, 0xC2, 0x95, 0xE9, 0xD1, 0x5B, 0x4C, 0x8B, 0xD1}, 9, {0x44, 0x89, 0xC0}, {0x41, 0x89, 0xC0}}};       /* r8d */

/* Mid-function sites: {rva, the instructions replaced (pinned), what the stub runs before BaseOf and after it}. */
typedef struct SharedSite { uint32_t rva; uint8_t length; uint8_t pin[12]; uint8_t before[16], beforeLength, after[12], afterLength; } SharedSite;
#define SHARED_IMUL_M 0x95, 0xE9, 0xD1, 0x5B
static const SharedSite kSharedSites[9] = {
    /* the screen: eax = the weapon, translated, then [rsp+0x48] = eax as before */
    {0x1493A5Au, 7, {0x41, 0x8B, 0x47, 0x10, 0x45, 0x33, 0xED}, {0x41, 0x8B, 0x47, 0x10}, 4, {0x45, 0x33, 0xED}, 3},
    /* the saved customization's check: imul edx, ebx, M with ebx translated (rax kept) */
    {0x103650Du, 6, {0x69, 0xD3, SHARED_IMUL_M}, {0x50, 0x8B, 0xC3}, 3, {0x69, 0xD0, SHARED_IMUL_M, 0x58}, 7},
    /* the loadout item list: r11d translated (rax kept) */
    {0x1372444u, 10, {0x48, 0x83, 0xC3, 0x30, 0x41, 0xBA, 0x05, 0x00, 0x00, 0x00},
        {0x48, 0x83, 0xC3, 0x30, 0x41, 0xBA, 0x05, 0x00, 0x00, 0x00, 0x50, 0x44, 0x89, 0xD8}, 14, {0x41, 0x89, 0xC3, 0x58}, 4},
    /* the loadout's mixIds: the weapon's own id stored, then eax translated for its attachments */
    {0x135D6A3u, 9, {0x41, 0x89, 0x04, 0x24, 0xBE, 0x01, 0x00, 0x00, 0x00}, {0x41, 0x89, 0x04, 0x24, 0xBE, 0x01, 0x00, 0x00, 0x00}, 9, {0}, 0},
    {0x135D794u, 12, {0x41, 0x89, 0x04, 0x8C, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x41, 0x89, 0x04, 0x8C}, 4, {0}, 0},
    {0x135D884u, 6, {0x41, 0x89, 0x04, 0xB4, 0xFF, 0xC6}, {0x41, 0x89, 0x04, 0xB4, 0xFF, 0xC6}, 6, {0}, 0},
    /* the screen's lambdas */
    {0x1494050u, 7, {0x44, 0x69, 0x18, SHARED_IMUL_M}, {0x44, 0x8B, 0x18, 0x50, 0x44, 0x89, 0xD8}, 7, {0x41, 0x89, 0xC3, 0x58, 0x45, 0x69, 0xDB, SHARED_IMUL_M}, 11},
    {0x14940B1u, 7, {0x69, 0x45, 0x10, SHARED_IMUL_M}, {0x8B, 0x45, 0x10}, 3, {0x69, 0xC0, SHARED_IMUL_M}, 6},
    {0x14941B9u, 6, {0x69, 0x10, SHARED_IMUL_M}, {0x50, 0x8B, 0x00}, 3, {0x69, 0xD0, SHARED_IMUL_M, 0x58}, 7}};
/* In place: the screen's unlock and reward matching read the translated weapon at [rsp+0x48]. */
static const struct { uint32_t rva; uint8_t pin[8]; } kSharedInPlace[2] = {
    {0x1493B5Eu, {0x41, 0x69, 0x47, 0x10, SHARED_IMUL_M}}, {0x1493D85u, {0x41, 0x69, 0x40, 0x10, SHARED_IMUL_M}}};
static const uint8_t kSharedRewrite[8] = {0x69, 0x44, 0x24, 0x48, SHARED_IMUL_M}; /* imul eax, [rsp+0x48], M */
#undef SHARED_IMUL_M
#define SHARED_STUB 64u

static int SharedSetting(void)
{
    if (g_sharedSetting < 0)
    {
        wchar_t value[8];
        DWORD length = GetEnvironmentVariableW(L"HELLFORGE_SHARED_ATTACHMENTS", value, 8);
        g_sharedSetting = length > 0 && length < 8 && !(value[0] == L'0' && value[1] == 0);
    }
    return g_sharedSetting;
}

/* NULL when every site is the reviewed one. */
static const char* SharedSitesFailed(uintptr_t game)
{
    uint8_t code[12];
    for (size_t i = 0; i < 4; i++)
        if (SafeRead(game + kSharedEntries[i].rva, code, kSharedEntries[i].length) || memcmp(code, kSharedEntries[i].pin, kSharedEntries[i].length) != 0)
            return "a function that finds a weapon's attachments is not the reviewed one";
    for (size_t i = 0; i < sizeof kSharedSites / sizeof kSharedSites[0]; i++)
        if (SafeRead(game + kSharedSites[i].rva, code, kSharedSites[i].length) || memcmp(code, kSharedSites[i].pin, kSharedSites[i].length) != 0)
            return "code that finds a weapon's attachments is not the reviewed one";
    for (size_t i = 0; i < 2; i++)
        if (SafeRead(game + kSharedInPlace[i].rva, code, 8) || memcmp(code, kSharedInPlace[i].pin, 8) != 0)
            return "the customization screen is not the reviewed one";
    return NULL;
}

/* The vanilla weapon a registered weapon copies, through copies of copies. */
static uint32_t SharedRoot(uint32_t id)
{
    for (int depth = 0; depth < 8; depth++)
    {
        int found = 0;
        for (int i = 0; i < g_weaponCount && !found; i++)
            if (!g_weapons[i].plain && g_weapons[i].id == id && g_weapons[i].base != id) { id = g_weapons[i].base; found = 1; }
        if (!found) break;
    }
    return id;
}

/* Called before the weapons are registered: whether this load's copies use their bases' attachments (the setting, and
 * every site as reviewed). OwnCustomization and registered-items.json follow it. */
static void SharedChoose(uintptr_t game)
{
    if (!SharedSetting()) return;
    const char* why = SharedSitesFailed(game);
    g_sharedChildren = why == NULL;
    Note(why == NULL ? "shared attachments: weapon copies use their bases' attachments" :
        "shared attachments: not used (%s); weapon copies get attachments of their own", why);
}

/* A mid-function site's stub: before, call BaseOf, after, jmp back. */
static uint8_t* SharedStub(uintptr_t game, const SharedSite* s, uint8_t* at, const uint8_t* baseOf)
{
    uint8_t* p = at;
    memcpy(p, s->before, s->beforeLength); p += s->beforeLength;
    *p++ = 0xE8; int32_t rel = (int32_t)(int64_t)((uintptr_t)baseOf - (uintptr_t)(p + 4)); memcpy(p, &rel, 4); p += 4;
    memcpy(p, s->after, s->afterLength); p += s->afterLength;
    *p++ = 0xE9; rel = (int32_t)(int64_t)(game + s->rva + s->length - (uintptr_t)(p + 4)); memcpy(p, &rel, 4); p += 4;
    return p;
}

/* A function entry's detour: push rax; mov eax, reg; call BaseOf; mov reg, eax; mov rax, &original; mov rax, [rax];
 * xchg [rsp], rax; ret (on to the original with rax as it was). */
static uint8_t* SharedEntryStub(const SharedEntry* e, void** original, uint8_t* at, const uint8_t* baseOf)
{
    uint8_t* p = at;
    *p++ = 0x50;
    memcpy(p, e->in, 3); p += 3;
    *p++ = 0xE8; int32_t rel = (int32_t)(int64_t)((uintptr_t)baseOf - (uintptr_t)(p + 4)); memcpy(p, &rel, 4); p += 4;
    memcpy(p, e->out, 3); p += 3;
    *p++ = 0x48; *p++ = 0xB8; uint64_t slot = (uint64_t)(uintptr_t)original; memcpy(p, &slot, 8); p += 8;
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0x00;              /* mov rax, [rax] */
    *p++ = 0x48; *p++ = 0x87; *p++ = 0x04; *p++ = 0x24; /* xchg [rsp], rax */
    *p++ = 0xC3;
    return p;
}

/* After the weapons are registered: the copies' table, then the stubs, then the sites (logged; a site that cannot be
 * patched leaves the rest as they are, so a copy may then show attachments there and not elsewhere). */
static void SharedInstall(uintptr_t game)
{
    if (!g_sharedChildren) return;
    uint32_t n = 0;
    for (int i = 0; i < g_weaponCount; i++)
        if (!g_weapons[i].plain && g_weapons[i].state == 1) g_copyBases.pairs[n++] = (CopyBase){g_weapons[i].id, SharedRoot(g_weapons[i].id)};
    g_copyBases.count = n;
    if (n == 0) return;
    size_t sites = sizeof kSharedSites / sizeof kSharedSites[0];
    uint8_t* code = AllocateNearGame(game, (1 + 4 + sites) * SHARED_STUB);
    if (code == NULL) { Note("shared attachments: no memory within 2 GB of game.dll"); return; }
    memcpy(code, kBaseOf, sizeof kBaseOf);
    uint64_t table = (uint64_t)(uintptr_t)&g_copyBases;
    memcpy(code + 4, &table, 8);
    for (size_t i = 0; i < 4; i++) SharedEntryStub(&kSharedEntries[i], &g_sharedOriginal[i], code + (1 + i) * SHARED_STUB, code);
    for (size_t i = 0; i < sites; i++) SharedStub(game, &kSharedSites[i], code + (5 + i) * SHARED_STUB, code);
    DWORD protect;
    if (!VirtualProtect(code, (1 + 4 + sites) * SHARED_STUB, PAGE_EXECUTE_READ, &protect)) return;
    FlushInstructionCache(GetCurrentProcess(), code, (1 + 4 + sites) * SHARED_STUB);
    int done = 0;
    for (size_t i = 0; i < 4; i++)
        done += !LoaderHook("shared attachments", game + kSharedEntries[i].rva, code + (1 + i) * SHARED_STUB, &g_sharedOriginal[i]);
    for (size_t i = 0; i < sites; i++)
    {
        uint8_t jump[12];
        memset(jump, 0x90, sizeof jump);
        jump[0] = 0xE9;
        int32_t rel = (int32_t)(int64_t)((uintptr_t)(code + (5 + i) * SHARED_STUB) - (game + kSharedSites[i].rva + 5));
        memcpy(jump + 1, &rel, 4);
        done += !ApiWrite(game + kSharedSites[i].rva, jump, kSharedSites[i].length);
    }
    for (size_t i = 0; i < 2; i++) done += !ApiWrite(game + kSharedInPlace[i].rva, kSharedRewrite, 8);
    Note("shared attachments: %u weapon copies use their bases' attachments (%d of %zu places patched)", n, done, 4 + sites + 2);
}
