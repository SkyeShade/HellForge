/* Native selection-screen groups (SDK 8), included by stratagems.c.
 * Build 25480438: candidate records in 0x18D8710 are {key, section, type, order, enabled, blocked, pad}.
 * Only the sort call in that builder and the stratagem heading call in the list's realize pass are redirected.
 * StratagemInfo +0xB8 stays a vanilla HUD category. See docs/stratagem-categories.md for the reviewed path.
 */
#define STR_CATEGORY_MAX 28u /* native section arrays each hold 33: five vanilla + 28 custom */
#define STR_CATEGORY_SORT_CALL 0x18D8C40u
#define STR_CATEGORY_HEADER_CALL 0x18D2E85u

typedef struct StratagemCategory
{
    HfMod* mod;
    char name[192], title[192];
    int32_t order;
    uint32_t text, section;
} StratagemCategory;
static StratagemCategory g_stratagemCategories[STR_CATEGORY_MAX];
static uint32_t g_stratagemCategoryCount;
static int g_stratagemCategoriesActive;

typedef struct PickerCandidate
{
    uint32_t key, section, type, order;
    uint8_t enabled, blocked, padding[2];
} PickerCandidate;
_Static_assert(sizeof(PickerCandidate) == 20, "native picker candidate layout");
static PickerCandidate g_pickerCandidates[STR_VANILLA];
static size_t g_pickerCandidateCount;
typedef int (*PickerCompare)(const void*, const void*);
static void (*g_pickerSort)(void*, size_t, size_t, PickerCompare);
static void (*g_pickerHeader)(void*, uint32_t);
static void (*g_pickerText)(void*, uint32_t);
static void (*g_pickerNumber)(void*, uint32_t, uint32_t);
static void* (*g_pickerItem)(void*, uint32_t);
static uint8_t (*g_pickerAvailable)(void*, uint32_t);
static uintptr_t g_pickerAccountSlot;

static uint32_t ApiAddStratagemCategory(HfMod* mod, const HfStratagemCategory* def)
{
    if (!mod || !def || def->size < offsetof(HfStratagemCategory, sort_order) + sizeof def->sort_order)
    { ApiLog(mod, "add_stratagem_category: set size to sizeof(HfStratagemCategory)"); return 0; }
    if (g_stratagemState != 0 || g_fired[HF_EVENT_GAME_LOADED])
    { ApiLog(mod, "add_stratagem_category: call from hellforge_mod_init"); return 0; }
    char buffer[256];
    const char* name = Identity(mod, def->name, buffer, sizeof buffer);
    if (!name || !name[0] || strlen(name) >= sizeof g_stratagemCategories[0].name ||
        !def->title || !def->title[0] || strlen(def->title) >= sizeof g_stratagemCategories[0].title)
    { ApiLog(mod, "add_stratagem_category: name and title are required (under 192 bytes)"); return 0; }
    for (uint32_t i = 0; i < g_stratagemCategoryCount; i++)
        if (!strcmp(name, g_stratagemCategories[i].name))
        {
            if (def->sort_order == g_stratagemCategories[i].order && !strcmp(def->title, g_stratagemCategories[i].title)) return i + 1;
            ApiLog(mod, "add_stratagem_category(%s): title/order conflicts with %s", name, g_stratagemCategories[i].mod->id);
            return 0;
        }
    if (g_stratagemCategoryCount == STR_CATEGORY_MAX)
    { ApiLog(mod, "add_stratagem_category(%s): all %u custom headings are used", name, STR_CATEGORY_MAX); return 0; }
    char key[224];
    snprintf(key, sizeof key, "%s#selection-category", name);
    uint32_t text = AddText(def->title, key, 0x31);
    if (!text) { ApiLog(mod, "add_stratagem_category(%s): could not register heading text", name); return 0; }
    StratagemCategory* c = &g_stratagemCategories[g_stratagemCategoryCount++];
    c->mod = mod; c->order = def->sort_order; c->text = text;
    strcpy(c->name, name); strcpy(c->title, def->title);
    return g_stratagemCategoryCount;
}

/* Handles keep registration order; UI section numbers use a deterministic order independent of mod load order. */
static void OrderStratagemCategories(void)
{
    for (uint32_t i = 0; i < g_stratagemCategoryCount; i++)
    {
        StratagemCategory* c = &g_stratagemCategories[i];
        c->section = 5;
        for (uint32_t j = 0; j < g_stratagemCategoryCount; j++)
        {
            StratagemCategory* other = &g_stratagemCategories[j];
            if (other->order < c->order || (other->order == c->order && strcmp(other->name, c->name) < 0)) c->section++;
        }
    }
}

static uint32_t StratagemSelectionSection(uint32_t type, uint32_t vanilla)
{
    if (type >= STR_VANILLA && type - STR_VANILLA < g_stratagemCount)
    {
        uint32_t handle = g_stratagems[type - STR_VANILLA].selectionCategory;
        if (handle && handle <= g_stratagemCategoryCount) return g_stratagemCategories[handle - 1].section;
    }
    return vanilla;
}

static void PickerSortCategories(void* data, size_t count, size_t width, PickerCompare compare)
{
    g_pickerCandidateCount = 0;
    if (g_stratagemCategoriesActive && width == sizeof(PickerCandidate) && count <= STR_VANILLA)
    {
        PickerCandidate* candidates = data;
        for (size_t i = 0; i < count; i++)
        {
            uint32_t type = candidates[i].type;
            candidates[i].section = StratagemSelectionSection(type, candidates[i].section);
            /* Under a custom heading the mod's selection_order replaces the base row's order (+0x188, inherited from
             * the vanilla stratagem it copies); the comparator is unsigned, so bias the signed value. The row keeps
             * its own. Equal orders fall back to the type, which is call order. */
            if (type >= STR_VANILLA && type - STR_VANILLA < g_stratagemCount && g_stratagems[type - STR_VANILLA].selectionCategory)
                candidates[i].order = (uint32_t)g_stratagems[type - STR_VANILLA].selectionOrder ^ 0x80000000u;
        }
        g_pickerSort(data, count, width, compare);
        memcpy(g_pickerCandidates, data, count * width);
        g_pickerCandidateCount = count;
        return;
    }
    g_pickerSort(data, count, width, compare);
}

/* The original heading counts selectable catalogue items and the available subset. Use the same predicates,
 * grouped by UI section, so customs disappear from their former vanilla heading's counts as well. */
static void PickerCategoryCounts(uint32_t section, void* account, uint32_t* available, uint32_t* total)
{
    *available = *total = 0;
    for (uint32_t type = 1; type < STR_VANILLA + g_stratagemCount; type++)
    {
        uint8_t* row = NULL;
        memcpy(&row, g_stratagemTable + type * 8, 8);
        if (!row || !(row[0x80] & 2)) continue;
        uint32_t vanilla;
        memcpy(&vanilla, row + 0xB8, 4);
        if (StratagemSelectionSection(type, vanilla) != section || !g_pickerItem(account, type)) continue;
        (*total)++;
        if (g_pickerAvailable(account, type)) (*available)++;
    }
}

/* A new section at candidate i: the list's group-th section starts there and is the candidate's. */
static int PickerSectionAt(const uint8_t* list, uint32_t i, uint32_t group, uint32_t sections)
{
    uint32_t first, section;
    if (group >= sections) return 0;
    memcpy(&first, list + 0x927D0 + group * 4, 4);
    memcpy(&section, list + 0x92854 + group * 4, 4);
    return first == i && section == g_pickerCandidates[i].section;
}

/* This renderer is shared by other screens. Only override a list whose keys AND section boundaries match the
 * selection builder's latest output, so opening another native mode-3 list cannot inherit its cached headings. */
static int PickerCategoryListMatches(const uint8_t* list)
{
    uint32_t count, sections;
    if (!list || !g_pickerCandidateCount) return 0;
    memcpy(&count, list + 0x92984, 4);
    memcpy(&sections, list + 0x92748, 4);
    if (count != g_pickerCandidateCount || sections > 33) return 0;
    uint32_t group = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t key;
        memcpy(&key, list + 0x92990 + i * 4, 4);
        if (key != g_pickerCandidates[i].key) return 0;
        if (i == 0 || g_pickerCandidates[i].section != g_pickerCandidates[i - 1].section)
        {
            if (!PickerSectionAt(list, i, group, sections)) return 0;
            group++;
        }
    }
    return group == sections;
}

static void PickerHeaderCategory(void* widget, uint32_t key, const uint8_t* list)
{
    g_pickerHeader(widget, key);
    if (!g_stratagemCategoriesActive || !g_stratagemTable || !PickerCategoryListMatches(list)) return;
    for (size_t i = 0; i < g_pickerCandidateCount; i++)
    {
        if (g_pickerCandidates[i].key != key) continue;
        uint32_t section = g_pickerCandidates[i].section;
        for (uint32_t j = 0; j < g_stratagemCategoryCount; j++)
            if (g_stratagemCategories[j].section == section)
            { g_pickerText((uint8_t*)widget + 0x110, g_stratagemCategories[j].text); break; }
        void* account = NULL;
        if (!SafeRead(g_pickerAccountSlot, &account, sizeof account) && account)
        {
            uint32_t available, total;
            PickerCategoryCounts(section, account, &available, &total);
            g_pickerNumber((uint8_t*)widget + 0x3C8, 0xEB646556u, available);
            g_pickerNumber((uint8_t*)widget + 0x3C8, 0x4442EB89u, total);
        }
        return;
    }
}

typedef struct CategoryPin { uint32_t rva; uint8_t size, bytes[32]; } CategoryPin;
static const CategoryPin kCategoryPins[] = {
    {0x18D8BFDu,29,{0x8b,0x83,0xb8,0,0,0,0x42,0x89,0x44,0x8c,0x54,0x8b,3,0x42,0x89,0x44,0x8c,0x58,0x8b,0x83,0x88,1,0,0,0x42,0x89,0x44,0x8c,0x5c}},
    {0x18D8C28u,29,{0x4c,0x8d,0x0d,0xb1,0xfa,0xff,0xff,0x41,0x8b,0xd7,0x41,0xb8,0x14,0,0,0,0x41,0x8b,0xef,0x48,0x8d,0x4c,0x24,0x50,0xe8,0xeb,0x55,0x7e,0}},
    {0x18D2E80u,10,{0x8b,0xd0,0x48,0x8b,0xcb,0xe8,0x76,0xbd,0xff,0xff}},
    {0x18D2B7Du,3,{0x48,0x8b,0xf1}}, /* RSI is the list through the heading call */
    {0x18D43F2u,8,{0x41,0x89,0x9c,0x8a,0x54,0x28,9,0}},
    {0x18D4408u,8,{0x41,0x89,0x84,0x8a,0x4c,0x27,9,0}},
    {0x18D441Eu,8,{0x41,0x89,0x84,0x8a,0xd0,0x27,9,0}},
    {0x18CEC7Fu,7,{0x48,0x8d,0x8b,0x10,1,0,0}},
    {0x18CECC7u,7,{0x48,0x8d,0x83,0xc8,3,0,0}},
    {0x143BF90u,12,{0x48,0x83,0xec,0x28,0x4c,0x8b,0xd9,0x39,0x91,0x10,1,0}},
    {0x143CAD0u,12,{0x48,0x83,0xec,0x28,0x4c,0x8b,0xd1,0x48,0x81,0xc1,0x10,1}},
    {0x1372290u,12,{0x4c,0x8b,0xd1,0x85,0xd2,0x75,9,0x48,0x8d,5,0xd2,0x91}},
    {0x136FC20u,12,{0x48,0x89,0x5c,0x24,8,0x48,0x8b,0xd9,0x85,0xd2,0x75,9}},
    {0x18CEC00u,13,{0x85,0xd2,0x0f,0x84,0xbf,1,0,0,0x53,0x48,0x83,0xec,0x40}}
};

static int StratagemCategoryPinsMatch(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kCategoryPins / sizeof kCategoryPins[0]; i++)
    {
        uint8_t bytes[32];
        const CategoryPin* pin = &kCategoryPins[i];
        if (SafeRead(game + pin->rva, bytes, pin->size) || memcmp(bytes, pin->bytes, pin->size)) return 0;
    }
    return 1;
}

static void CategoryCallBytes(uint8_t out[5], uintptr_t call, uintptr_t target)
{
    int32_t displacement = (int32_t)(target - call - 5);
    out[0] = 0xE8;
    memcpy(out + 1, &displacement, 4);
}

/* jmp qword ptr [rip+0], absolute destination; leaves every argument register intact. */
static void CategoryThunks(uint8_t* thunks)
{
    for (int i = 0; i < 2; i++)
    {
        uint8_t* thunk = thunks + i * 16;
        if (i) { thunk[0] = 0x4C; thunk[1] = 0x8B; thunk[2] = 0xC6; thunk += 3; } /* mov r8,rsi: list */
        thunk[0] = 0xFF; thunk[1] = 0x25;
        uintptr_t target = i ? (uintptr_t)PickerHeaderCategory : (uintptr_t)PickerSortCategories;
        memcpy(thunk + 6, &target, 8);
    }
}

static void InstallStratagemCategories(uintptr_t game)
{
    int used = 0;
    for (uint32_t i = 0; i < g_stratagemCount; i++) used |= g_stratagems[i].selectionCategory != 0;
    if (!used) return;
    const char* why = "the picker does not match the reviewed build";
    if (!StratagemCategoryPinsMatch(game)) goto refused;
    OrderStratagemCategories();
    uint8_t* thunks = AllocateNearGame(game, 48);
    why = "could not allocate executable picker call targets";
    if (!thunks) goto refused;
    CategoryThunks(thunks);
    DWORD old;
    if (!VirtualProtect(thunks, 48, PAGE_EXECUTE_READ, &old)) { VirtualFree(thunks, 0, MEM_RELEASE); goto refused; }
    FlushInstructionCache(GetCurrentProcess(), thunks, 48);
    g_pickerSort = (void*) (game + 0x20BE230u);
    g_pickerHeader = (void*) (game + 0x18CEC00u);
    g_pickerText = (void*) (game + 0x143BF90u);
    g_pickerNumber = (void*) (game + 0x143CAD0u);
    g_pickerItem = (void*) (game + 0x1372290u);
    g_pickerAvailable = (void*) (game + 0x136FC20u);
    g_pickerAccountSlot = game + 0x347CEF8u;
    uint8_t header[5], sort[5];
    CategoryCallBytes(header, game + STR_CATEGORY_HEADER_CALL, (uintptr_t)thunks + 16);
    CategoryCallBytes(sort, game + STR_CATEGORY_SORT_CALL, (uintptr_t)thunks);
    why = "could not redirect the picker calls";
    if (ApiWrite(game + STR_CATEGORY_HEADER_CALL, header, 5)) { VirtualFree(thunks, 0, MEM_RELEASE); goto refused; }
    /* The header detour is a pass-through until both calls are installed. If the second write fails the live
     * header thunk is retained and harmless; never free memory that game code can still call. */
    if (ApiWrite(game + STR_CATEGORY_SORT_CALL, sort, 5)) goto refused;
    g_stratagemCategoriesActive = 1;
    Note("stratagem categories: native picker enabled (%u registered headings)", g_stratagemCategoryCount);
    return;
refused:
    Note("stratagem categories: %s; stratagems use their vanilla sections", why);
    for (uint32_t i = 0; i < g_stratagemCategoryCount; i++)
        ApiLog(g_stratagemCategories[i].mod, "selection category %s not installed: %s; using vanilla sections", g_stratagemCategories[i].name, why);
}
