/* SDK 22, add_armory_tab: mods' labels on the Armory's tab bar, for game.dll build 25480438. The Armory screen (menu
 * +0x60) has one bar (+0x570) that shows one of four vanilla label lists, and one select (0x14570B0) for every list.
 * Included at the end of world_calls.c (its menu reads and pins). Layout: docs/internals-world.md. */

#define AT_SELECT 0x14570B0u  /* (screen, index): the screen's tab select; a mod's index never reaches it */
#define AT_OPEN 0x1457350u    /* (screen, open): opens or closes a category's page and sets the bar's labels */
#define AT_INPUT 0x1457A10u   /* (screen, input, ...): the screen's input; it sets the weapon labels again after the grid */
#define AT_SET 0x17AAC50u     /* (bar, ids, count): the bar's labels, up to 8 text ids */
#define AT_PICK 0x17ABFC0u    /* (bar, index, u8, u8): the bar's selected label */
#define AT_BAR 0x570u
#define AT_BAR_IDS 0xDFE8u
#define AT_BAR_COUNT 0xE068u
#define AT_BAR_INDEX 0xE06Cu
#define AT_LABELS 8u
#define AT_TABS 32u

/* Each list the bar shows: its ids in game.dll, as the code that sets it names them. */
typedef struct AtList { uint32_t ids, count; } AtList;
static const AtList kAtLists[] = {
    [HF_ARMORY_TABS_CATEGORIES] = {0x32E2688u, 5}, [HF_ARMORY_TABS_WEAPONS] = {0x33108B0u, 3},
    [HF_ARMORY_TABS_ARMOUR] = {0x33111F0u, 3}, [HF_ARMORY_TABS_COSMETICS] = {0x32D4E28u, 6}};
#define AT_LISTS (sizeof kAtLists / sizeof kAtLists[0])
static const ActPin kAtPins[] = {
    {0, AT_SELECT, "48895c2418574883ec2080793400488b"}, {0, AT_OPEN, "4056574883ec280fb6f2488bf9385134"},
    {0, AT_INPUT, "4c894424185355574883ec30488bf949"}, {0, 0x1457A61u, "488db770050000488bcee8"},
    {0, 0x1457A75u, "8b97dce50000488bcfe8"}, {0, 0x14573AFu, "488d9f70050000"},
    {0, 0x145740Du, "41b805000000488d156eb2e801"}, {0, 0x145764Du, "41b803000000488d155692eb01"},
    {0, 0x145747Eu, "41b803000000488d15659deb01"}, {0, 0x14573BEu, "41b806000000488d155ddae701"},
    {0, 0x17AAC85u, "4489a968e00000"}, {0, 0x17AAC99u, "4881c1e8df0000"}, {0, 0x17ABFD4u, "4863816ce00000"}};

typedef struct AtTab { HfMod* mod; HfArmoryTabCallback callback; void* user; uint32_t list, text, handle; } AtTab;
typedef void (*AtSelectFn)(uintptr_t screen, uint32_t index);
typedef void (*AtOpenFn)(uintptr_t screen, uint8_t open);
typedef uint64_t (*AtInputFn)(uintptr_t screen, void* input, void* a, void* b);
typedef void (*AtSetFn)(uintptr_t bar, const uint32_t* ids, uint32_t count);
typedef void (*AtPickFn)(uintptr_t bar, uint32_t index, uint8_t a, uint8_t b);

static AtTab g_atTabs[AT_TABS];
static uint32_t g_atCount;
static SRWLOCK g_atLock = SRWLOCK_INIT;
static HfMod g_atMod; /* the loader's own tick: installs the hooks and notices the Armory closing */
static volatile LONG g_atTicking;
static int g_atState; /* 0 not installed, 1 hooked, -1 off (modloader.log says why) */
static uintptr_t g_atGame;
static uint32_t g_atActive; /* the handle of the mod tab the bar has selected, 0 none */
static AtSelectFn g_atSelect;
static AtOpenFn g_atOpen;
static AtInputFn g_atInput;

/* The list the bar shows (its vanilla labels first, as the game set them), and how many labels it has; -1 none. */
static int AtShown(uintptr_t bar, uint32_t* count, uint32_t ids[AT_LABELS])
{
    if (SafeRead(bar + AT_BAR_COUNT, count, 4) || *count == 0 || *count > AT_LABELS || SafeRead(bar + AT_BAR_IDS, ids, 4u * *count)) return -1;
    for (int list = 0; list < (int)AT_LISTS; list++)
    {
        uint32_t vanilla[AT_LABELS];
        if (kAtLists[list].count <= *count && !SafeRead(g_atGame + kAtLists[list].ids, vanilla, 4u * kAtLists[list].count) &&
            memcmp(vanilla, ids, 4u * kAtLists[list].count) == 0) return list;
    }
    return -1;
}

/* The mod tabs of a list, in the order they were added: how many fit on the bar after its vanilla labels. */
static uint32_t AtTabsOf(int list, AtTab out[AT_LABELS])
{
    uint32_t n = 0, room = AT_LABELS - kAtLists[list].count;
    AcquireSRWLockShared(&g_atLock);
    for (uint32_t i = 0; i < g_atCount && n < room; i++)
        if (g_atTabs[i].list == (uint32_t)list) out[n++] = g_atTabs[i];
    ReleaseSRWLockShared(&g_atLock);
    return n;
}

/* The mods' labels put after the vanilla ones whenever the game set the vanilla list alone, keeping the selection. */
static void AtLabels(uintptr_t bar)
{
    uint32_t count, ids[AT_LABELS], index;
    AtTab tabs[AT_LABELS];
    int list = AtShown(bar, &count, ids);
    if (list < 0 || count != kAtLists[list].count) return;
    uint32_t n = AtTabsOf(list, tabs);
    if (n == 0 || SafeRead(bar + AT_BAR_INDEX, &index, 4)) return;
    for (uint32_t i = 0; i < n; i++) ids[count + i] = tabs[i].text;
    ((AtSetFn)(g_atGame + AT_SET))(bar, ids, count + n);
    ((AtPickFn)(g_atGame + AT_PICK))(bar, index < count + n ? index : 0, 1, 1);
}

/* The callbacks when the bar's selection moved onto or off a mod's tab (screen 0: the Armory is not shown). */
static void AtFollow(uintptr_t screen)
{
    uint32_t count, ids[AT_LABELS], index = 0, want = 0;
    AtTab tabs[AT_LABELS], was = {0}, now = {0};
    int list = screen ? AtShown(screen + AT_BAR, &count, ids) : -1;
    if (list >= 0 && !SafeRead(screen + AT_BAR + AT_BAR_INDEX, &index, 4) && index >= kAtLists[list].count && index < count)
    {
        uint32_t n = AtTabsOf(list, tabs), k = index - kAtLists[list].count;
        if (k < n && tabs[k].text == ids[index]) { now = tabs[k]; want = now.handle; }
    }
    if (want == g_atActive) return;
    AcquireSRWLockShared(&g_atLock);
    for (uint32_t i = 0; i < g_atCount; i++) if (g_atTabs[i].handle == g_atActive) was = g_atTabs[i];
    ReleaseSRWLockShared(&g_atLock);
    g_atActive = want;
    if (was.callback) was.callback(was.mod, was.handle, 0, was.user);
    if (now.callback) now.callback(now.mod, now.handle, 1, now.user);
}

/* A click or Q/E onto a mod's label: the game's select would read a tab record past its own, so it is not called. */
static void AtSelectDetour(uintptr_t screen, uint32_t index)
{
    uint32_t count, ids[AT_LABELS];
    int list = AtShown(screen + AT_BAR, &count, ids);
    if (list < 0 || index < kAtLists[list].count) g_atSelect(screen, index);
    AtFollow(screen);
}

static void AtOpenDetour(uintptr_t screen, uint8_t open)
{
    g_atOpen(screen, open);
    AtLabels(screen + AT_BAR);
    AtFollow(screen);
}

static uint64_t AtInputDetour(uintptr_t screen, void* input, void* a, void* b)
{
    uint64_t result = g_atInput(screen, input, a, b);
    AtLabels(screen + AT_BAR);
    AtFollow(screen);
    return result;
}

/* The Armory screen while the menu shows it, else 0. */
static uintptr_t AtScreen(void)
{
    uintptr_t object, screen;
    if (!EvpPtr(g_atGame + WC_MENU, &object) || EvpU32(object, 0) != HF_MENU_ARMORY || !EvpPtr(object + 0x60, &screen)) return 0;
    return screen;
}

static void AtInstall(void)
{
    g_atGame = ApiModuleBase("game.dll", NULL);
    const char* why = NULL;
    if (!g_atGame || strcmp(g_build, "25480438")) why = "unsupported game build";
    else if (!ActPins(g_atGame, kWcArmoryPins, 1) || !ActPins(g_atGame, kAtPins, sizeof kAtPins / sizeof kAtPins[0])) why = "the Armory's code is not the reviewed one";
    else if (LoaderHook("armory tabs: select", g_atGame + AT_SELECT, (void*)AtSelectDetour, (void**)&g_atSelect) ||
             LoaderHook("armory tabs: page", g_atGame + AT_OPEN, (void*)AtOpenDetour, (void**)&g_atOpen) ||
             LoaderHook("armory tabs: input", g_atGame + AT_INPUT, (void*)AtInputDetour, (void**)&g_atInput)) why = "a hook could not be placed";
    g_atState = why ? -1 : 1;
    if (why) Note("armory tabs: off: %s; mods' tabs are not shown", why);
    else Note("armory tabs: on, %u mod tabs", g_atCount);
}

static void AtTick(HfMod* mod, float dt, void* user)
{
    (void)mod, (void)dt, (void)user;
    if (g_atState == 0 && g_fired[HF_EVENT_GAME_LOADED]) AtInstall();
    if (g_atState <= 0) return;
    uintptr_t screen = AtScreen();
    if (screen) AtLabels(screen + AT_BAR); /* the list the screen set when it was made, before any page opened */
    AtFollow(screen);
}

/* SDK 22, add_armory_tab: a label of the mod's on one of the Armory's lists; its handle, or 0 refused (logged). */
static uint32_t ApiAddArmoryTab(HfMod* mod, uint32_t list, const char* label, HfArmoryTabCallback on_select, void* user)
{
    if (mod == NULL || list >= AT_LISTS || label == NULL || label[0] == 0 || on_select == NULL)
    { ApiLog(mod, "add_armory_tab: refused: it needs an HF_ARMORY_TABS_ list, a label and a callback"); return 0; }
    if (g_atState < 0) { ApiLog(mod, "add_armory_tab(%s): refused: armory tabs are off (modloader.log says why)", label); return 0; }
    char key[160];
    snprintf(key, sizeof key, "armory_tab/%u/%s", list, label);
    uint32_t text = ApiAddText(mod, key, label), handle = 0, taken = 0;
    if (text == 0) return 0;
    AcquireSRWLockExclusive(&g_atLock);
    for (uint32_t i = 0; i < g_atCount; i++) taken += g_atTabs[i].list == list;
    if (g_atCount < AT_TABS && taken < AT_LABELS - kAtLists[list].count)
    {
        handle = g_atCount + 1;
        g_atTabs[g_atCount++] = (AtTab){mod, on_select, user, list, text, handle};
    }
    ReleaseSRWLockExclusive(&g_atLock);
    if (handle == 0) { ApiLog(mod, "add_armory_tab(%s): refused: that bar holds %u labels, and mods took the rest", label, AT_LABELS); return 0; }
    if (InterlockedExchange(&g_atTicking, 1) == 0)
    {
        g_atMod.api = &g_api;
        snprintf(g_atMod.id, sizeof g_atMod.id, "hellforge.armory-tabs");
        g_atMod.log = g_log;
        g_atMod.loaded = 1;
        ApiOnGameThread(&g_atMod, AtTick, NULL);
    }
    ApiLog(mod, "add_armory_tab(%s): tab %u on list %u", label, handle, list);
    return handle;
}
