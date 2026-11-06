/* Mods' warbonds in the game's own warbond table (HellForge.Core ModWarbonds; the backend serves their pages). Build
 * 25480438 draws every warbond's name, art and look from a static table: 0x32E0C20, 28 entries of 80 bytes (entry 0
 * the default an unknown id gets), searched by id32 in 13 inlined loops `cmp reg,0x1C` (V, 44 rip-relative references,
 * all listed below). The table is copied near game.dll with an entry per mod warbond after the game's, and those loops
 * are pointed at the copy: 13 table bases, 6 field bases the found entry is read through, 12 bounds. The 25 references
 * that read entry 0's fields when nothing is found keep the game's entry 0. Entry: +0 id32, +4/+8 the line above the
 * name, +0xC/+0x10 the name (mixed case / upper case), +0x18 card art, +0x20 titled art, +0x28 logo, +0x30 a store
 * image, +0x38 logo offset and size (4 floats), +0x48 variant (0 free, 1 premium, 2 legendary). Names are mod text
 * (AddText); art is a game warbond's (the plan names it), the mod's own texture (ModWarbondArt) or entry 0's
 * placeholder. Included after forge_credits.c and behaviours.c (Emit). */
#define WB_TABLE 0x32E0C20u
#define WB_ENTRY 80u
#define WB_GAME_ENTRIES 28u
/* The bounds are imm8 (`cmp reg, ib`, signed): 127 entries at most without longer code. */
#define WB_MAX (127 - WB_GAME_ENTRIES)
typedef struct
{
    uint32_t id, variant, currency, artFrom;
    uint32_t season[2], name[2];
    uint64_t art; /* the texture built from the mod's image (ModWarbondArt), 0 for none */
} ModWarbond;
static ModWarbond g_modWarbonds[WB_MAX];
static int g_modWarbondCount, g_modWarbondsState;

/* A table reference: an instruction at rva of len bytes whose rip-relative disp32 sits at its last 4 bytes, pointing at
 * the table + field. */
typedef struct { uint32_t rva; uint8_t len; uint8_t bytes[7]; uint32_t field; } WbLea;
static const WbLea kWbLeas[] = {
    {0x11E9F90u, 7, {0x4c,0x8d,0x0d,0x89,0x6c,0x0f,0x02}, 0}, {0x145E304u, 7, {0x4c,0x8d,0x05,0x15,0x29,0xe8,0x01}, 0},
    {0x14DECDCu, 7, {0x48,0x8d,0x0d,0x3d,0x1f,0xe0,0x01}, 0}, {0x14DF3D8u, 7, {0x4c,0x8d,0x05,0x41,0x18,0xe0,0x01}, 0},
    {0x14E08E5u, 7, {0x48,0x8d,0x1d,0x34,0x03,0xe0,0x01}, 0}, {0x14E155Cu, 7, {0x48,0x8d,0x1d,0xbd,0xf6,0xdf,0x01}, 0},
    {0x14E8E80u, 7, {0x48,0x8d,0x15,0x99,0x7d,0xdf,0x01}, 0}, {0x14E90B7u, 7, {0x4c,0x8d,0x0d,0x62,0x7b,0xdf,0x01}, 0},
    {0x14E9529u, 7, {0x4c,0x8d,0x05,0xf0,0x76,0xdf,0x01}, 0}, {0x14FDC01u, 7, {0x48,0x8d,0x0d,0x18,0x30,0xde,0x01}, 0},
    {0x19638F1u, 7, {0x4c,0x8d,0x25,0x28,0xd3,0x97,0x01}, 0}, {0x196C6C6u, 7, {0x4c,0x8d,0x0d,0x53,0x45,0x97,0x01}, 0},
    {0x197D79Cu, 7, {0x48,0x8d,0x0d,0x7d,0x34,0x96,0x01}, 0},
    /* field bases of the found entry (index * 0x50 added) */
    {0x14E8EABu, 7, {0x4c,0x8d,0x35,0x7e,0x7d,0xdf,0x01}, 0x10}, {0x14FDC2Bu, 7, {0x48,0x8d,0x15,0x0e,0x30,0xde,0x01}, 0x20},
    {0x14DECB5u, 7, {0x4c,0x8d,0x15,0x94,0x1f,0xe0,0x01}, 0x30}, {0x14DF09Cu, 7, {0x4c,0x8d,0x15,0xad,0x1b,0xe0,0x01}, 0x30},
    {0x197D773u, 7, {0x4c,0x8d,0x15,0xd6,0x34,0x96,0x01}, 0x30}, {0x197DB5Bu, 7, {0x4c,0x8d,0x15,0xee,0x30,0x96,0x01}, 0x30}};
/* The bounds: `cmp reg, 0x1C`, the immediate the instruction's last byte. */
typedef struct { uint32_t rva; uint8_t len; uint8_t bytes[4]; } WbBound;
static const WbBound kWbBounds[] = {
    {0x11E9FABu, 4, {0x41,0x83,0xf8,0x1c}}, {0x145E31Cu, 3, {0x83,0xf8,0x1c}}, {0x14DED0Au, 3, {0x83,0xfa,0x1c}},
    {0x14DF3EEu, 3, {0x83,0xf9,0x1c}}, {0x14E097Cu, 3, {0x83,0xfa,0x1c}}, {0x14E8E9Au, 3, {0x83,0xf9,0x1c}},
    {0x14E90D1u, 3, {0x83,0xfa,0x1c}}, {0x14E954Au, 3, {0x83,0xf9,0x1c}}, {0x14FDC1Au, 3, {0x83,0xfb,0x1c}},
    {0x196390Eu, 3, {0x83,0xfa,0x1c}}, {0x196C6E3u, 4, {0x41,0x83,0xf8,0x1c}}, {0x197D7CAu, 3, {0x83,0xfa,0x1c}}};

/* Items with a currency of their own ("warbond_item <seasonPassItemId> <currency mixId>"), sorted by id at install:
 * what the game's per-item checks, charge and icons use instead of the warbond's (ModWarbondService prices them alike). */
#define WB_ITEMS_MAX 4280
typedef struct { uint32_t id, currency; } WbItemCurrency;
static WbItemCurrency g_wbItems[WB_ITEMS_MAX];
static int g_wbItemCount;

static int CompareWbItems(const void* a, const void* b)
{
    uint32_t x = ((const WbItemCurrency*)a)->id, y = ((const WbItemCurrency*)b)->id;
    return x < y ? -1 : x > y;
}

/* An item's currency: its own, or the warbond's (fallback). */
static uint32_t ItemCurrency(uint32_t itemId, uint32_t fallback)
{
    int lo = 0, hi = g_wbItemCount - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (g_wbItems[mid].id == itemId) return g_wbItems[mid].currency;
        if (g_wbItems[mid].id < itemId) lo = mid + 1; else hi = mid - 1;
    }
    return fallback;
}

/* "warbond <id32> <variant> <currency> <text key> <name> <season> <art from> <art texture, hex>" (ModWarbonds.PlanLines). */
static int WarbondPlanLine(char** f, int n)
{
    if (n == 3 && strcmp(f[0], "warbond_item") == 0)
    {
        if (g_wbItemCount < WB_ITEMS_MAX)
            g_wbItems[g_wbItemCount++] = (WbItemCurrency){(uint32_t)strtoul(f[1], NULL, 10), (uint32_t)strtoul(f[2], NULL, 10)};
        return 1;
    }
    if (n != 9 || strcmp(f[0], "warbond") != 0) return 0;
    if (g_modWarbondCount >= WB_MAX) { Note("warbonds: more than %d mod warbonds; the rest show the game's placeholder", WB_MAX); return 1; }
    ModWarbond* w = &g_modWarbonds[g_modWarbondCount];
    w->id = (uint32_t)strtoul(f[1], NULL, 10);
    w->variant = (uint32_t)strtoul(f[2], NULL, 10);
    w->currency = (uint32_t)strtoul(f[3], NULL, 10);
    w->artFrom = (uint32_t)strtoul(f[7], NULL, 10);
    w->art = strtoull(f[8], NULL, 16);
    if (w->id == 0 || w->variant > 2) return 1;
    char upper[192];
    UpperCopy(f[5], upper, sizeof upper);
    w->name[0] = AddText(f[5], f[4], 0x41);
    w->name[1] = AddText(upper, f[4], 0x42);
    UpperCopy(f[6], upper, sizeof upper);
    w->season[0] = AddText(f[6], f[4], 0x43);
    w->season[1] = AddText(upper, f[4], 0x44);
    g_modWarbondCount++;
    return 1;
}

/* The copy's entry for a mod warbond: the art and logo of the game warbond it names (or entry 0's), its own id, text
 * and variant. Its own picture is the card both untitled (+0x18) and titled (+0x20), as Castellan's Creed has one
 * picture for both, over entry 0's: no logo (+0x28 0, as the placeholder has). */
static void WarbondEntry(uint8_t* entry, const uint8_t* game, const ModWarbond* w)
{
    const uint8_t* art = game; /* entry 0: the placeholder art, no logo */
    for (uint32_t i = 1; i < WB_GAME_ENTRIES && w->artFrom != 0; i++)
        if (*(const uint32_t*)(game + i * WB_ENTRY) == w->artFrom) art = game + i * WB_ENTRY;
    memcpy(entry, art, WB_ENTRY);
    memcpy(entry + 0x00, &w->id, 4);
    memcpy(entry + 0x04, w->season, 8);
    memcpy(entry + 0x0C, w->name, 8);
    memcpy(entry + 0x48, &w->variant, 4);
    if (w->art != 0)
    {
        memcpy(entry + 0x18, &w->art, 8);
        memcpy(entry + 0x20, &w->art, 8);
    }
}

/* The price icons. The game draws every warbond price with the medal: four stores of kind 3 into a price block {kind @+0,
 * icon override u64 @+8, amount @+0x10} (V): the item panel 0x196DFA9, the page header 0x196CC76, the Acquisition Center's
 * card 0x1963C1A and the grid's cells 0x196D65F. Each is replaced by a jump to a stub that has WarbondPriceKind write the
 * kind and the override from the warbond's currency: the detail's (presenter+0xD18, +0xC) or the list entry's (r13+0xC on
 * the card). Requisition is kind 1, Forge Credits kind 2 (the sprite forge_credits.c draws red), samples kind 3 with the
 * sample's own icon (the game's table 0x32DE960: mixId, icon @+0x18, {a,r,g,b} @+0x28), medals and the rest kind 3 as
 * before. The two widgets redraw only when the kind changes (0x194868F, 0x193F495 compare +0 only); their compare also
 * tests the override and, when it changed, clears the drawn state (+0x6B4) so the new icon is drawn. Kind 3 draws white
 * (0x1948ACA, 0x193F7FA); its colour call (0x1448690) goes through WarbondPriceColour, which gives a sample its colour. */
#define WB_SAMPLES 0x32DE960u
#define WB_SET_COLOUR 0x1448690u
/* item: 1 the site prices one item (WarbondItemPriceKind(id, warbond currency, block): the item's own currency), else the
 * warbond's balance (WarbondPriceKind(currency, block)); 2 the page header's, whose currency the page's texts follow
 * (WarbondPagePriceKind, warbond_texts.c). */
typedef struct { uint32_t rva; uint8_t len; uint8_t pin[8]; uint32_t next; uint8_t load[16]; uint8_t loadLen; uint8_t item; } WbPriceSite;
static const WbPriceSite kWbPriceSites[] = {
    /* the item panel (rbx = the detail item): mov ecx,[rbx]; mov rax,[rbp+0xD18]; mov edx,[rax+0xC]; lea r8,[r14+0x10] */
    {0x196DFA9u, 8, {0x41,0xc7,0x46,0x10,0x03,0x00,0x00,0x00}, 0x196DFB1u, {0x8b,0x0b, 0x48,0x8b,0x85,0x18,0x0d,0x00,0x00, 0x8b,0x50,0x0c, 0x4d,0x8d,0x46,0x10}, 16, 1},
    /* the page header: lea rdx,[rax+0x40]; mov rax,[rbp+0xD18]; mov ecx,[rax+0xC] */
    {0x196CC76u, 7, {0xc7,0x40,0x40,0x03,0x00,0x00,0x00}, 0x196CC7Du, {0x48,0x8d,0x50,0x40, 0x48,0x8b,0x85,0x18,0x0d,0x00,0x00, 0x8b,0x48,0x0c}, 14, 2},
    /* the Acquisition Center's card: lea rdx,[rbx+0x40]; mov ecx,[r13+0xC] */
    {0x1963C1Au, 7, {0xc7,0x43,0x40,0x03,0x00,0x00,0x00}, 0x1963C21u, {0x48,0x8d,0x53,0x40, 0x41,0x8b,0x4d,0x0c}, 8, 0},
    /* a grid cell (rbx = the cell, its id at +0): mov ecx,[rbx]; mov rax,[rsi+0xD18]; mov edx,[rax+0xC]; lea r8,[rbx+8] */
    {0x196D65Fu, 7, {0xc7,0x43,0x08,0x03,0x00,0x00,0x00}, 0x196D666u, {0x8b,0x0b, 0x48,0x8b,0x86,0x18,0x0d,0x00,0x00, 0x8b,0x50,0x0c, 0x4c,0x8d,0x43,0x08}, 16, 1}};
/* The widgets' compares: the bytes, where equal goes, and the stub's override test (new/old block in rcx/rdx or r8/rdx). */
typedef struct { uint32_t rva; uint8_t len; uint8_t pin[8]; uint32_t equal, next; uint8_t test[8]; } WbCompare;
static const WbCompare kWbCompares[] = {
    {0x194868Fu, 6, {0x8b,0x01,0x39,0x02,0x74,0x16}, 0x19486ABu, 0x1948695u, {0x48,0x8b,0x41,0x08, 0x48,0x39,0x42,0x08}},      /* counter */
    {0x193F495u, 7, {0x41,0x8b,0x00,0x3b,0x02,0x74,0x5b}, 0x193F4F7u, 0x193F49Cu, {0x49,0x8b,0x40,0x08, 0x48,0x3b,0x42,0x08}}}; /* price */
/* The kind-3 colour calls: `call 0x1448690`, and how the stub finds the widget (rdi - 0x3C8 / rdi). */
typedef struct { uint32_t rva; uint8_t pin[5]; uint8_t widget[7]; uint8_t widgetLen; } WbColour;
static const WbColour kWbColours[] = {
    {0x1948AE2u, {0xe8,0xa9,0xfb,0xaf,0xff}, {0x4c,0x8d,0x87,0x38,0xfc,0xff,0xff}, 7}, /* lea r8,[rdi-0x3C8] */
    {0x193F815u, {0xe8,0x76,0x8e,0xb0,0xff}, {0x4c,0x8b,0xc7}, 3}};                     /* mov r8,rdi */
static const uint32_t kWbSampleIds[3] = {0xEDF6EAF5u, 0xB1ED1C41u, 0xDAC0E9DBu};
static uint64_t g_wbSampleIcon[3];
static float g_wbSampleRgb[3][3];
static void (*g_wbSetColour)(void* element, const float* rgb);

static void WarbondPriceKind(uint32_t currency, uint8_t* block)
{
    uint32_t kind = 3;
    uint64_t icon = 0;
    if (currency == 0xD7150EECu) kind = 1;      /* requisition */
    else if (currency == 0x6C3C83C1u) kind = 2; /* Forge Credits */
    else
        for (int i = 0; i < 3; i++)
            if (currency == kWbSampleIds[i]) icon = g_wbSampleIcon[i];
    memcpy(block, &kind, 4);
    memcpy(block + 8, &icon, 8);
}

/* The currency of the warbond page on screen (its header's price, 0x196CC76). */
static uint32_t g_wbPageCurrency;
static void WarbondPagePriceKind(uint32_t currency, uint8_t* block)
{
    g_wbPageCurrency = currency;
    WarbondPriceKind(currency, block);
}

static void WarbondItemPriceKind(uint32_t itemId, uint32_t warbondCurrency, uint8_t* block)
{
    WarbondPriceKind(ItemCurrency(itemId, warbondCurrency), block);
}

static void WarbondPriceColour(void* element, float* rgb, const uint8_t* widget)
{
    uint64_t icon;
    memcpy(&icon, *(const uint8_t* const*)(widget + 0x6A0) + 8, 8);
    for (int i = 0; i < 3; i++)
        if (icon != 0 && icon == g_wbSampleIcon[i]) memcpy(rgb, g_wbSampleRgb[i], sizeof g_wbSampleRgb[i]);
    g_wbSetColour(element, rgb);
}

static void PutAbs(Emit* e, const void* target) /* mov rax, target */
{
    uint64_t at = (uint64_t)(uintptr_t)target;
    Put(e, "\x48\xb8", 2);
    Put(e, &at, 8);
}

/* Per-item currency (V): an item's state (2 buyable, 3 too dear) is set from the warbond's balance (view+0x50) for the
 * grid's cells (0x196D626, cell+0x20) and the item panel (0x196E22D, panel+0x28; the panel's state is the only buy gate,
 * through its widget to view+0x2405), and the buy request's builder (0x13714F0: id, cost, currency; its two callers the
 * page view 0x196BB13 and the MVVM popup 0x14FE4DF, whose currency is medals) takes the warbond's currency, which the
 * reply's deduction (0x13751D0) takes back as sent. Each goes through the item's currency (ItemCurrency): the state from
 * its held amount (the game's own inlined lookup), the builder's currency argument replaced at its entry. */
#define WB_PROGRESSION 0x347CEF8u
#define WB_BUILDER 0x13714F0u
#define WB_CELL_STATE 0x196D626u
#define WB_PANEL_STATE 0x196E22Du
static const uint8_t kWbBuilderPin[5] = {0x4c,0x8b,0xdc,0x41,0x56};        /* mov r11,rsp; push r14 */
static const uint8_t kWbCellStatePin[6] = {0x48,0x8b,0x46,0x08,0x33,0xc9}; /* mov rax,[rsi+8]; xor ecx,ecx (to 0x196D63B) */
static const uint8_t kWbPanelStatePin[6] = {0x8b,0x43,0x08,0x39,0x41,0x50}; /* mov eax,[rbx+8]; cmp [rcx+0x50],eax (to 0x196E248) */
static uintptr_t g_wbGame;

/* What the profile holds of a currency (the game's inline lookup, 0x196D2E4: the catalogue's count, then its hierarchy
 * entries 0x18 apart, {index, mixId}, the amount at definition index + 0xB0; at progression+0x1CE0, +0xB9CE4 and +0x1D94
 * until state_moves.c moves the catalogue, so through g_catalogue); 0 when it holds none. */
static int32_t WarbondHeld(uint32_t mixId)
{
    const uint8_t* mgr = *(const uint8_t* const*)(g_wbGame + WB_PROGRESSION);
    if (mgr == NULL) return 0;
    uint32_t count = *(const uint32_t*)(mgr + g_catalogue.count);
    const uint8_t* entry = mgr + g_catalogue.index;
    for (uint32_t i = 0; i < count && i < g_catalogue.items; i++, entry += 0x18)
        if (*(const uint32_t*)(entry + 4) == mixId) return *(const int32_t*)(mgr + g_catalogue.records + (size_t)*(const uint32_t*)entry * 0xB8 + 0xB0);
    return 0;
}

/* An item's state: 3 when it costs more than the player holds of its currency, else 2 (the warbond's balance for an
 * item in the warbond's currency, as the game does). */
static uint32_t WarbondItemState(uint32_t itemId, uint32_t warbondCurrency, int32_t warbondHeld, int32_t cost)
{
    uint32_t currency = ItemCurrency(itemId, warbondCurrency);
    int32_t held = currency == warbondCurrency ? warbondHeld : WarbondHeld(currency);
    return cost > held ? 3u : 2u;
}

/* The three stubs at e: the cell state, the panel state, the builder's entry. */
static void EmitWarbondItemStubs(uintptr_t game, Emit* e, uint8_t* stubs[3])
{
    stubs[0] = e->at; /* save the volatile registers (r9 is live), state = WarbondItemState(id, currency, held, cost) */
    Put(e, "\x50\x51\x52\x41\x50\x41\x51\x41\x52\x41\x53\x48\x83\xec\x28", 15);
    Put(e, "\x8b\x0b\x48\x8b\x86\x18\x0d\x00\x00\x8b\x50\x0c\x48\x8b\x46\x08\x44\x8b\x40\x50\x44\x8b\x4b\x18", 24);
    PutAbs(e, (const void*)WarbondItemState);
    Put(e, "\xff\xd0\x89\x43\x20", 5);                                   /* call rax; mov [rbx+0x20], eax */
    Put(e, "\x48\x83\xc4\x28\x41\x5b\x41\x5a\x41\x59\x41\x58\x5a\x59\x58", 15);
    Put8(e, 0xE9);
    PutRel32(e, (const uint8_t*)(game + 0x196D63Bu));
    stubs[1] = e->at; /* the function's end follows: every volatile register is free */
    Put(e, "\x44\x8b\x41\x50\x8b\x0b\x48\x8b\x85\x18\x0d\x00\x00\x8b\x50\x0c\x44\x8b\x4b\x08\x48\x83\xec\x20", 24);
    PutAbs(e, (const void*)WarbondItemState);
    Put(e, "\xff\xd0\x48\x83\xc4\x20\x41\x89\x46\x28", 10);           /* call rax; mov [r14+0x28], eax */
    Put8(e, 0xE9);
    PutRel32(e, (const uint8_t*)(game + 0x196E248u));
    stubs[2] = e->at; /* r9d = ItemCurrency(edx, r9d), the arguments kept; then the builder's first two instructions */
    Put(e, "\x51\x52\x41\x50\x48\x83\xec\x20\x89\xd1\x44\x89\xca", 13);
    PutAbs(e, (const void*)ItemCurrency);
    Put(e, "\xff\xd0\x41\x89\xc1\x48\x83\xc4\x20\x41\x58\x5a\x59", 13);
    Put(e, kWbBuilderPin, sizeof kWbBuilderPin);
    Put8(e, 0xE9);
    PutRel32(e, (const uint8_t*)(game + WB_BUILDER + 5));
}

/* Whether every price site, widget compare and colour call matches the reviewed build. */
static int WarbondPricePins(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kWbPriceSites / sizeof *kWbPriceSites; i++)
        if (!PinMatches(game + kWbPriceSites[i].rva, kWbPriceSites[i].pin, kWbPriceSites[i].len)) return 0;
    for (size_t i = 0; i < sizeof kWbCompares / sizeof *kWbCompares; i++)
        if (!PinMatches(game + kWbCompares[i].rva, kWbCompares[i].pin, kWbCompares[i].len)) return 0;
    for (size_t i = 0; i < sizeof kWbColours / sizeof *kWbColours; i++)
        if (!PinMatches(game + kWbColours[i].rva, kWbColours[i].pin, 5)) return 0;
    return 1;
}

/* The game's sample icons and colours (0x32DE960); 0 when the table is not the reviewed build's. */
static int WarbondSamples(uintptr_t game)
{
    for (int i = 0; i < 3; i++)
    {
        const uint8_t* sample = (const uint8_t*)(game + WB_SAMPLES + (uintptr_t)i * 0x40);
        if (*(const uint32_t*)sample != kWbSampleIds[i]) return 0;
        memcpy(&g_wbSampleIcon[i], sample + 0x18, 8);
        memcpy(g_wbSampleRgb[i], sample + 0x2C, sizeof g_wbSampleRgb[i]); /* {a,r,g,b}: r, g, b */
    }
    return 1;
}

/* The eight stubs in code: the four price sites', the two compares', the two colour calls'. */
static void EmitWarbondPriceStubs(uintptr_t game, uint8_t* code, uint8_t* stubs[8])
{
    Emit e = {code};
    for (size_t i = 0; i < 4; i++) /* save the volatile registers, WarbondPriceKind(currency, block), restore, go on */
    {
        const WbPriceSite* site = &kWbPriceSites[i];
        stubs[i] = e.at;
        Put(&e, "\x50\x51\x52\x41\x50\x41\x51\x41\x52\x41\x53\x48\x83\xec\x28", 15);
        Put(&e, site->load, site->loadLen);
        PutAbs(&e, site->item == 1 ? (const void*)WarbondItemPriceKind : site->item == 2 ? (const void*)WarbondPagePriceKind : (const void*)WarbondPriceKind);
        Put(&e, "\xff\xd0\x48\x83\xc4\x28\x41\x5b\x41\x5a\x41\x59\x41\x58\x5a\x59\x58", 17);
        Put8(&e, 0xE9);
        PutRel32(&e, (const uint8_t*)(game + site->next));
    }
    for (size_t i = 0; i < 2; i++) /* override changed: drawn state -1; then the game's compare */
    {
        const WbCompare* c = &kWbCompares[i];
        stubs[4 + i] = e.at;
        Put(&e, c->test, 8);
        Put(&e, "\x74\x0a\xc7\x83\xb4\x06\x00\x00\xff\xff\xff\xff", 12);
        Put(&e, c->pin, (size_t)c->len - 2);
        Put(&e, "\x0f\x84", 2);
        PutRel32(&e, (const uint8_t*)(game + c->equal));
        Put8(&e, 0xE9);
        PutRel32(&e, (const uint8_t*)(game + c->next));
    }
    for (size_t i = 0; i < 2; i++) /* r8 = the widget; jmp WarbondPriceColour (it returns to the call's caller) */
    {
        stubs[6 + i] = e.at;
        Put(&e, kWbColours[i].widget, kWbColours[i].widgetLen);
        PutAbs(&e, (const void*)WarbondPriceColour);
        Put(&e, "\xff\xe0", 2);
    }
}

/* A jmp (or call) to a stub over the len bytes at, the rest nops; nonzero when it could not be written. */
static int WarbondStubJump(uintptr_t at, size_t len, uint8_t opcode, const uint8_t* stub)
{
    uint8_t patch[8] = {opcode, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    int32_t rel = (int32_t)(int64_t)((uintptr_t)stub - (at + 5));
    memcpy(patch + 1, &rel, 4);
    return ApiWrite(at, patch, len) != 0;
}

static const char* InstallWarbondPrices(uintptr_t game)
{
    if (!WarbondPricePins(game)) return "the price code does not match the reviewed build";
    if (!WarbondSamples(game)) return "the game's sample icons are not where the reviewed build keeps them";
    g_wbSetColour = (void (*)(void*, const float*))(game + WB_SET_COLOUR);
    uint8_t* code = AllocateNearGame(game, 0x1000);
    if (code == NULL) return "no memory near game.dll";
    uint8_t* stubs[8];
    EmitWarbondPriceStubs(game, code, stubs);
    DWORD protect;
    if (!VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &protect)) return "its stubs could not be made executable";
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    int failed = 0; /* the redraws first, then the kinds: each site stands alone */
    for (size_t i = 0; i < 2 && !failed; i++) failed |= WarbondStubJump(game + kWbCompares[i].rva, kWbCompares[i].len, 0xE9, stubs[4 + i]);
    for (size_t i = 0; i < 2 && !failed; i++) failed |= WarbondStubJump(game + kWbColours[i].rva, 5, 0xE8, stubs[6 + i]);
    for (size_t i = 0; i < 4 && !failed; i++) failed |= WarbondStubJump(game + kWbPriceSites[i].rva, kWbPriceSites[i].len, 0xE9, stubs[i]);
    return failed ? "game.dll could not be rewritten" : NULL;
}

/* The per-item currency's stubs (purchases: installed apart from the look). The builder first: once the charge is the
 * item's currency, the state checks follow; a stop leaves them on the warbond's. NULL, or why not. */
static const char* InstallWarbondItems(uintptr_t game)
{
    if (!PinMatches(game + WB_BUILDER, kWbBuilderPin, sizeof kWbBuilderPin) || !PinMatches(game + WB_CELL_STATE, kWbCellStatePin, sizeof kWbCellStatePin) ||
        !PinMatches(game + WB_PANEL_STATE, kWbPanelStatePin, sizeof kWbPanelStatePin))
        return "the purchase code does not match the reviewed build";
    uint8_t* code = AllocateNearGame(game, 0x1000);
    if (code == NULL) return "no memory near game.dll";
    uint8_t* stubs[3];
    Emit e = {code};
    g_wbGame = game;
    EmitWarbondItemStubs(game, &e, stubs);
    DWORD protect;
    if (!VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &protect)) return "its stubs could not be made executable";
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    if (WarbondStubJump(game + WB_BUILDER, 5, 0xE9, stubs[2]) || WarbondStubJump(game + WB_CELL_STATE, 6, 0xE9, stubs[0]) ||
        WarbondStubJump(game + WB_PANEL_STATE, 6, 0xE9, stubs[1]))
        return "game.dll could not be rewritten";
    return NULL;
}

/* The first table reference or bound that does not match the reviewed build; 0 when all do. */
static uint32_t WarbondTablePins(uintptr_t game)
{
    for (size_t i = 0; i < sizeof kWbLeas / sizeof *kWbLeas; i++)
        if (!PinMatches(game + kWbLeas[i].rva, kWbLeas[i].bytes, kWbLeas[i].len)) return kWbLeas[i].rva;
    for (size_t i = 0; i < sizeof kWbBounds / sizeof *kWbBounds; i++)
        if (!PinMatches(game + kWbBounds[i].rva, kWbBounds[i].bytes, kWbBounds[i].len)) return kWbBounds[i].rva;
    return 0;
}

static const char* InstallWarbondList(uintptr_t game); /* warbond_list.c */
static const char* InstallWarbondTexts(uintptr_t game); /* warbond_texts.c */

/* The table copy with the mod warbonds' entries, and the game's references and bounds pointed at it. NULL, or why not. */
static const char* InstallWarbondTable(uintptr_t game, size_t entries)
{
    static char why[128];
    uint32_t mismatch = WarbondTablePins(game);
    if (mismatch) { snprintf(why, sizeof why, "the code at game.dll+0x%x does not match the reviewed build", mismatch); return why; }
    uint8_t* table = AllocateNearGame(game, entries * WB_ENTRY);
    if (table == NULL) return "no memory near game.dll";
    const uint8_t* vanilla = (const uint8_t*)(game + WB_TABLE);
    memcpy(table, vanilla, WB_GAME_ENTRIES * WB_ENTRY);
    for (int i = 0; i < g_modWarbondCount; i++) WarbondEntry(table + (WB_GAME_ENTRIES + (size_t)i) * WB_ENTRY, vanilla, &g_modWarbonds[i]);
    for (size_t i = 0; i < sizeof kWbLeas / sizeof *kWbLeas; i++)
    {
        const WbLea* l = &kWbLeas[i];
        int32_t disp = (int32_t)((intptr_t)(table + l->field) - (intptr_t)(game + l->rva + l->len));
        if (ApiWrite(game + l->rva + l->len - 4, &disp, 4)) return "game.dll could not be rewritten";
    }
    uint8_t count = (uint8_t)entries; /* the bounds after the bases: never past the copy */
    for (size_t i = 0; i < sizeof kWbBounds / sizeof *kWbBounds; i++)
        if (ApiWrite(game + kWbBounds[i].rva + kWbBounds[i].len - 1, &count, 1)) return "game.dll could not be rewritten";
    return NULL;
}

static void InstallModWarbonds(void)
{
    if (g_modWarbondsState || g_modWarbondCount == 0) return;
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    g_modWarbondsState = -1;
    qsort(g_wbItems, (size_t)g_wbItemCount, sizeof *g_wbItems, CompareWbItems);
    if (strcmp(g_build, "25480438") || !game) return;
    const char* list = InstallWarbondList(game);
    if (list) Note("warbonds: the game lists its 32 warbonds at most, mods' past them wait: %s", list);
    const char* items = InstallWarbondItems(game);
    if (items) Note("warbonds: an item in another currency than its warbond's is checked and charged in the warbond's on screen: %s", items);
    size_t entries = WB_GAME_ENTRIES + (size_t)g_modWarbondCount;
    const char* table = InstallWarbondTable(game, entries);
    if (table) { Note("warbonds: mod warbonds show the game's placeholder: %s", table); return; }
    g_modWarbondsState = 1;
    HookText();
    const char* prices = InstallWarbondPrices(game);
    if (prices) Note("warbonds: every warbond price shows the medal: %s", prices);
    const char* texts = prices ? "the prices are not installed" : InstallWarbondTexts(game);
    if (texts) Note("warbonds: mod warbonds' spent and unlock texts name medals: %s", texts);
    Note("warbonds: %d mod warbond(s) in the game's warbond table (%u entries)", g_modWarbondCount, (unsigned)entries);
}
