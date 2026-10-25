/* The enemy spawn director's tables for edits (the HD2R Editor's Spawn Rosters and Spawn Compositions; HD2Runtime 0.40.1
 * domains/enemy_director.lua and enemy_director_writes.lua, docs/enemy-spawns.md). Included by hellforge_modloader.c after
 * difficulty.c, whose kRosters names the six roster descriptors in game.dll's data (the editor offers the five the HD2R
 * Editor does): {rows, count} at +0, and eight composition families {rows, count} at +0x20 + 0x10 * family.
 *
 * Rows by id, as set_live, stats files and the editor name them:
 *   EnemyRoster         (roster << 16) | row: a roster row's ten spawn weights, tech levels 1 to 10 (f32, 40 bytes). The
 *                       row is 0x80 bytes; its weights are at +0x30, or at +0x2C once difficulty.c gave every row an 11th
 *                       (the weight readers say which: their displacement 0x30 or 0x2C).
 *   EnemyRosterLevel11  the same row's level 11 weight (+0x54), only with level 11 added.
 *   EnemyComposition    (roster << 16) | (family << 12) | row: a composition row (0x118): eight groups of {key, count,
 *                       emission time} from +0, weight +0xE0, cost multiplier +0xE4, post-schedule duration +0xE8, tech
 *                       levels +0xEC..+0xF0, players +0xF4..+0xF8.
 *   DemolitionTemplate  a demolition template's index, 1 to 9 (0 "none" and 10 "Count" stay): the force a destruction
 *                       stage that names it requires (+4, u32, 1 to 255: the incoming force is narrowed to a byte), shared
 *                       by every stage that names it. The templates are 16-byte rows at [[game+0x346D928] + 0x30]
 *                       (HD2Runtime enemy_director.lua destructionGlobal, templateCount 11); each row holds its index at +0
 *                       and the bytes HD2Runtime read at +8, checked before anything is written. Whether a change reaches
 *                       structures already standing is not tested.
 * The director reads them when a mission is set up, so a change applies from the next mission; only the host's tables
 * decide what spawns. A composition's group counts may total at most 32 (the AI squad queue's capacity: a 33rd entry
 * overwrote its count and crashed the game, HD2Runtime research spawn-capacity), and its lowest tech level and player
 * count may not exceed its highest: DirectorRefuses checks every write against the row as it is.
 *
 * Without level 11 the difficulty table is not moved and its 19 copies are the game's: a DifficultySettings edit is
 * written to the copy HD2Runtime edits (0x328D2A0) and to every other copy that held the same bytes (DifficultyMirror). */
#define DIR_ROSTER_ROW 0x80u
#define DIR_COMPOSITION_ROW 0x118u
#define DIR_FAMILIES 8u
#define DIR_GROUPS 8u
#define DIR_SQUAD_CAPACITY 32u
#define DIR_DIFFICULTY_COPY 4 /* kDifficultyCopies[4], 0x328D2A0 */
#define DIR_IMAGE 0x4744000u   /* game.dll's image size (HD2Runtime enemy_director.lua imageSize) */
#define DIR_DESTRUCTION 0x346D928u /* the destruction system; its +0x30 the demolition templates */
#define DIR_DEMOLITION_ROW 16u
#define DIR_DEMOLITION_ROWS 11u
/* each template's +8..+15 as HD2Runtime read them (enemy_director.lua rows' hex): only ForceDestroyTrees sets +12 */
static const uint32_t kDemolitionTail[DIR_DEMOLITION_ROWS][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 1}, {0, 0},
    {0, 0}, {0, 0}};

static uintptr_t DirectorGame(size_t* size) { return ApiModuleBase("game.dll", size); }
/* game.dll's image; the native fixture replaces it. */
static uintptr_t (*g_directorImage)(size_t* size) = DirectorGame;

/* Whether the roster weights sit at +0x2C (level 11 added: difficulty.c moved them), from the first reader's displacement:
 * 1 moved, 0 not, -1 neither (not as reviewed). */
static int DirectorWeightsMoved(uintptr_t game)
{
    uint8_t disp = 0;
    if (SafeRead(game + kWeightReaders[0][0] + kWeightReaders[0][1], &disp, 1)) return -1;
    return disp == 0x2C ? 1 : disp == 0x30 ? 0 : -1;
}

/* A roster's rows and count as reviewed, or 0. */
static uintptr_t DirectorRoster(uintptr_t game, uint32_t roster, uint64_t* count)
{
    uint64_t rows = 0;
    *count = 0;
    if (roster >= sizeof kRosters / sizeof kRosters[0] || SafeRead(game + kRosters[roster].descriptor, &rows, 8) ||
        SafeRead(game + kRosters[roster].descriptor + 8, count, 8) || rows == 0 || *count != kRosters[roster].rows) return 0;
    return (uintptr_t)rows;
}

/* The row (the weights, for a roster's kinds) an id names, or 0: *ready 0 while game.dll's data or level 11's move is
 * still to come, else 1 (0 then: no such row, or not as reviewed). */
static uintptr_t DirectorRow(const char* kind, uint64_t id, int* ready)
{
    *ready = 1;
    size_t size = 0;
    uintptr_t game = g_directorImage(&size);
    if (game == 0 || size != DIR_IMAGE || strcmp(g_build, "25480438")) { *ready = game == 0; return 0; }
    if (!strcmp(kind, "DemolitionTemplate"))
    {
        uint64_t system = 0, table = 0;
        if (SafeRead(game + DIR_DESTRUCTION, &system, 8) || system == 0 || SafeRead((uintptr_t)system + 0x30, &table, 8) || table == 0)
        { *ready = 0; return 0; } /* the destruction system is not made yet */
        for (uint32_t i = 0; i < DIR_DEMOLITION_ROWS; i++)
        {
            uint32_t row[4];
            if (SafeRead((uintptr_t)table + i * DIR_DEMOLITION_ROW, row, sizeof row) || row[0] != i ||
                row[2] != kDemolitionTail[i][0] || row[3] != kDemolitionTail[i][1]) return 0;
        }
        return id >= 1 && id < DIR_DEMOLITION_ROWS - 1 ? (uintptr_t)table + (uintptr_t)id * DIR_DEMOLITION_ROW : 0;
    }
    if (g_difficultyAdded && g_difficultyState == 0) { *ready = 0; return 0; } /* the weights are about to move */
    uint32_t roster = (uint32_t)(id >> 16), row = (uint32_t)(id & 0xFFFF);
    uint64_t count = 0;
    uintptr_t rows = DirectorRoster(game, roster, &count);
    if (rows == 0) return 0;
    if (!strcmp(kind, "EnemyRoster") || !strcmp(kind, "EnemyRosterLevel11"))
    {
        int moved = DirectorWeightsMoved(game);
        if (moved < 0 || row >= count || (id >> 32) != 0) return 0;
        uintptr_t at = rows + (uintptr_t)row * DIR_ROSTER_ROW;
        if (!strcmp(kind, "EnemyRoster")) return at + (moved ? 0x2C : 0x30);
        return moved ? at + 0x54 : 0; /* level 11's weight: only once it was added */
    }
    if (!strcmp(kind, "EnemyComposition"))
    {
        uint32_t family = (row >> 12) & 0xF, index = row & 0xFFF;
        uint64_t families = 0, base = 0;
        if (family >= DIR_FAMILIES || (id >> 32) != 0 || SafeRead(game + kRosters[roster].descriptor + 0x20 + 0x10 * family, &base, 8) ||
            SafeRead(game + kRosters[roster].descriptor + 0x28 + 0x10 * family, &families, 8) || base == 0 || families > 4096 || index >= families)
            return 0;
        return (uintptr_t)base + (uintptr_t)index * DIR_COMPOSITION_ROW;
    }
    return 0;
}

static int DirectorKind(const char* kind)
{
    return !strcmp(kind, "EnemyRoster") || !strcmp(kind, "EnemyRosterLevel11") || !strcmp(kind, "EnemyComposition") ||
        !strcmp(kind, "DemolitionTemplate");
}

/* Why a write of `bytes` at row + offset is refused (NULL: allowed), judged against the row as it is now. */
static const char* DirectorRefuses(const char* kind, uintptr_t row, uint32_t offset, const void* bytes, uint32_t size)
{
    if (!DirectorKind(kind)) return NULL;
    uint32_t bits = 0;
    float value = 0;
    if (size != 4) return "a director field is 4 bytes";
    memcpy(&bits, bytes, 4);
    memcpy(&value, bytes, 4);
    if (!strcmp(kind, "DemolitionTemplate"))
        return offset != 4 ? "only a demolition template's force is edited" : bits < 1 || bits > 255 ? "a demolition force is 1 to 255" : NULL;
    if (strcmp(kind, "EnemyComposition"))
        return !isfinite(value) || value < 0 || value > 10000 ? "a spawn weight is 0 to 10000" : NULL;
    uint8_t now[DIR_COMPOSITION_ROW];
    if (offset > DIR_COMPOSITION_ROW - 4 || SafeRead(row, now, sizeof now)) return "the composition is unreadable";
    memcpy(now + offset, bytes, 4); /* the row as it would be */
    if (offset < DIR_GROUPS * 12 && offset % 12 == 4)
    {
        uint32_t total = 0, key;
        memcpy(&key, now + offset - 4, 4);
        if (key == 0 && bits != 0) return "the group names no enemies";
        for (uint32_t g = 0; g < DIR_GROUPS; g++) { uint32_t c; memcpy(&c, now + g * 12 + 4, 4); total += c; }
        if (bits > DIR_SQUAD_CAPACITY || total > DIR_SQUAD_CAPACITY) return "a composition's groups may total at most 32 enemies (the squad queue's capacity)";
        return NULL;
    }
    if ((offset < DIR_GROUPS * 12 && offset % 12 == 8) || (offset >= 0xE0 && offset <= 0xE8))
    {
        if (!isfinite(value) || value < 0) return "not a finite number of 0 or more";
        if (offset == 0xE4 && value < 0.01f) return "the cost multiplier is at least 0.01";
        return NULL;
    }
    if (offset >= 0xEC && offset <= 0xF8)
    {
        uint32_t lowTech, highTech, lowPlayers, highPlayers, top = DirectorWeightsMoved(g_directorImage(NULL)) == 1 ? 11 : 10;
        memcpy(&lowTech, now + 0xEC, 4); memcpy(&highTech, now + 0xF0, 4); memcpy(&lowPlayers, now + 0xF4, 4); memcpy(&highPlayers, now + 0xF8, 4);
        if (lowTech < 1 || lowTech > top || highTech < 1 || highTech > top)
            return top == 11 ? "tech levels are 1 to 11" : "tech levels are 1 to 10 (11 with a mod that adds it)";
        if (lowPlayers < 1 || lowPlayers > 4 || highPlayers < 1 || highPlayers > 4) return "player counts are 1 to 4";
        if (lowTech > highTech || lowPlayers > highPlayers) return "the lowest may not exceed the highest";
        return NULL;
    }
    return "not a field HellForge edits in a composition";
}

/* A DifficultySettings row while the table is the game's own (no level 11): the copy HD2Runtime edits. */
static uintptr_t DifficultyVanillaRow(uint64_t level, int* ready)
{
    size_t size = 0;
    uintptr_t game = g_directorImage(&size);
    *ready = game != 0;
    if (game == 0 || size != DIR_IMAGE || strcmp(g_build, "25480438") || level < 1 || level > DIF_VANILLA) return 0;
    uintptr_t row = game + kDifficultyCopies[DIR_DIFFICULTY_COPY] + (uintptr_t)(level - 1) * DIF_ROW;
    uint32_t stored = 0;
    return !SafeRead(row + 4, &stored, 4) && stored == level ? row : 0; /* its level, as reviewed */
}

/* After a write to the game's own difficulty row (no level 11): the same bytes into every other copy that held what it did.
 * before: the bytes before the write. */
static void DifficultyMirror(uintptr_t row, uint32_t offset, const void* before, const void* bytes, uint32_t size)
{
    uintptr_t game = g_directorImage(NULL), first = game + kDifficultyCopies[DIR_DIFFICULTY_COPY];
    if (game == 0 || row < first || row >= first + DIF_VANILLA * DIF_ROW || size > 8) return;
    uintptr_t inRow = (row - first) % DIF_ROW + offset, level = (row - first) / DIF_ROW;
    for (size_t c = 0; c < sizeof kDifficultyCopies / sizeof kDifficultyCopies[0]; c++)
    {
        if ((int)c == DIR_DIFFICULTY_COPY) continue;
        uintptr_t at = game + kDifficultyCopies[c] + level * DIF_ROW + inRow;
        uint8_t held[8];
        if (!SafeRead(at, held, size) && !memcmp(held, before, size)) ApiWrite(at, bytes, size);
    }
}

/* A row's size for these kinds (the boot pass checks an edit's offset against it), 0 for another kind. */
static uint32_t DirectorStride(const char* kind)
{
    return !strcmp(kind, "EnemyRoster") ? 40u : !strcmp(kind, "EnemyRosterLevel11") ? 4u : !strcmp(kind, "EnemyComposition") ? DIR_COMPOSITION_ROW :
        !strcmp(kind, "DemolitionTemplate") ? DIR_DEMOLITION_ROW : !strcmp(kind, "DifficultySettings") ? DIF_ROW : 0u;
}
