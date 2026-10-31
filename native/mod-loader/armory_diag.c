/* Armory thumbnail diagnostics (the 0.3.0-beta FAQ's "Armory icons go blank when scrolling"), build 25480438, read only.
 * Included by hellforge_modloader.c after loadout_preview.c; packages.c's game-thread update calls ArmoryDiagTick.
 *
 * Every Armory icon (weapons, throwables, helmets, armour, capes) is a picture the game renders itself into one shared
 * atlas: a thumbnail manager ([game.dll +0x347CD80]) runs six reusable cards of up to 15 requests, one card at a time,
 * and scrolling releases the cards that left the view and queues new ones. An icon stays blank until its card is
 * complete (state 8), and a card waits for every request's preview unit and for its textures to stream in (research:
 * the knowledge base's armory-thumbnail-regeneration-investigation and armory-v9-engine-loading-investigation; the
 * layout below is the one Armory Preview Cache reads on this build). --toaster_mode turns texture streaming off and
 * the cards never finish. Whatever else stops them, the requests a stalled card waits on name it, so this logs them.
 *
 * Once a second, while the state stack's top is the Armory (5) or the mission briefing's loadout (14): when some card is
 * queued or generating and nothing the generator moves (cards' states and counts, the requests' stages, the active card,
 * its phase) has changed for ARMORY_STALL_MS, modloader.log gets one line with the cards and the requests still waiting
 * (kind, id, stage); a later change gets one line saying how long it stood. At most ARMORY_REPORTS stalls a session.
 * The first screen watched gets one line with its cards, and a thumbnail screen whose manager cannot be read or is not
 * laid out as reviewed one line saying it is not watched (once a session each).
 * Manager layout (u32 words of the 11112 bytes read): card c's state at +1832+1816c, its count at +1836+1816c, its
 * requests of 120 bytes from +32+1816c (+24 the item's id, +100 its kind 0..4, +104 its stage: 1 waiting admission,
 * 2 submitted, 3 unit available, 4 done); +11064 the active card (~0 none), +11068/+11072 the preview slots' occupancy,
 * +11096 the active card's phase. */

#define ARMORY_MANAGER 0x347CD80u   /* [game.dll + it]: the thumbnail manager */
#define ARMORY_STATES 0x347CE28u    /* [game.dll + it]: the UI state machine, its state stack at +0x429C (6 u32: 5 deep) */
#define ARMORY_STACK 0x429Cu
#define ARMORY_BYTES 11112u
#define ARMORY_CARDS 6u
#define ARMORY_PER_CARD 15u
#define ARMORY_CARD_STRIDE 1816u
#define ARMORY_CARD_STATE 1832u
#define ARMORY_CARD_COUNT 1836u
#define ARMORY_REQUESTS 32u
#define ARMORY_REQUEST_STRIDE 120u
#define ARMORY_ACTIVE 11064u
#define ARMORY_SLOTS 11068u
#define ARMORY_PHASE 11096u
#define ARMORY_DONE_CARD 8u
#define ARMORY_DONE_REQUEST 4u
#define ARMORY_STALL_MS 10000u
#define ARMORY_REPORTS 8
#define ARMORY_SIGNATURE (2 + 2 * ARMORY_CARDS + ARMORY_CARDS * ARMORY_PER_CARD)

typedef struct ArmoryDiag
{
    uint32_t last[ARMORY_SIGNATURE];
    int has, stalled, reports, toaster, toasterChecked;
    int watched, unwatched;              /* the session's one line each: the first screen watched, or one not readable */
    uint32_t screen;
    ULONGLONG since, next;
} ArmoryDiag;
static ArmoryDiag g_armoryDiag;
static int g_armoryDiagOff;                  /* 1: not this build, or the game thread is not hooked */

static uint32_t ArmoryU32(const uint8_t* m, size_t at) { uint32_t v; memcpy(&v, m + at, 4); return v; }
static uint64_t ArmoryU64(const uint8_t* m, size_t at) { uint64_t v; memcpy(&v, m + at, 8); return v; }

static const char* ArmoryKind(uint32_t kind)
{
    static const char* const names[] = {"weapon", "throwable", "helmet", "armour", "cape"};
    return kind < 5 ? names[kind] : "other";
}

static const char* ArmoryScreen(uint32_t screen) { return screen == 5 ? "Armory" : "briefing loadout"; }

/* What the generator moves, and whether any card is still to finish (0 when the manager's bounds are not as reviewed). */
static int ArmorySignature(const uint8_t* m, uint32_t* signature, int* pending)
{
    uint32_t w = 0;
    *pending = 0;
    signature[w++] = ArmoryU32(m, ARMORY_ACTIVE);
    signature[w++] = ArmoryU32(m, ARMORY_PHASE);
    for (uint32_t c = 0; c < ARMORY_CARDS; c++)
    {
        uint32_t state = ArmoryU32(m, c * ARMORY_CARD_STRIDE + ARMORY_CARD_STATE), count = ArmoryU32(m, c * ARMORY_CARD_STRIDE + ARMORY_CARD_COUNT);
        if (state > ARMORY_DONE_CARD || count > ARMORY_PER_CARD) return 0;
        signature[w++] = state;
        signature[w++] = count;
        if (state != 0 && state != ARMORY_DONE_CARD && count > 0) *pending = 1;
    }
    for (uint32_t c = 0; c < ARMORY_CARDS; c++)
        for (uint32_t i = 0; i < ARMORY_PER_CARD; i++)
            signature[w++] = ArmoryU32(m, c * ARMORY_CARD_STRIDE + ARMORY_REQUESTS + i * ARMORY_REQUEST_STRIDE + 104);
    return 1;
}

/* The stall line: the cards (state:count) and the requests the unfinished ones still wait on. */
static void ArmoryStallLine(const ArmoryDiag* d, const uint8_t* m, ULONGLONG stood, char* line, size_t size)
{
    char cards[96] = "", waiting[640] = "";
    size_t c_at = 0, w_at = 0;
    int listed = 0, more = 0;
    for (uint32_t c = 0; c < ARMORY_CARDS; c++)
    {
        uint32_t state = ArmoryU32(m, c * ARMORY_CARD_STRIDE + ARMORY_CARD_STATE), count = ArmoryU32(m, c * ARMORY_CARD_STRIDE + ARMORY_CARD_COUNT);
        int n = snprintf(cards + c_at, sizeof cards - c_at, "%s%u:%u", c ? " " : "", state, count);
        if (n > 0 && (size_t)n < sizeof cards - c_at) c_at += (size_t)n;
        if (state == 0 || state == ARMORY_DONE_CARD) continue;
        for (uint32_t i = 0; i < count; i++)
        {
            size_t at = c * ARMORY_CARD_STRIDE + ARMORY_REQUESTS + i * ARMORY_REQUEST_STRIDE;
            uint32_t stage = ArmoryU32(m, at + 104);
            if (stage == ARMORY_DONE_REQUEST) continue;
            if (listed == 12) { more++; continue; }
            n = snprintf(waiting + w_at, sizeof waiting - w_at, "%s%s 0x%llx (card %u, stage %u)", listed ? ", " : "",
                ArmoryKind(ArmoryU32(m, at + 100)), (unsigned long long)ArmoryU64(m, at + 24), c, stage);
            if (n > 0 && (size_t)n < sizeof waiting - w_at) w_at += (size_t)n;
            listed++;
        }
    }
    if (more && w_at < sizeof waiting) snprintf(waiting + w_at, sizeof waiting - w_at, ", and %d more", more);
    uint32_t active = ArmoryU32(m, ARMORY_ACTIVE);
    char activeText[16];
    if (active == 0xFFFFFFFFu) snprintf(activeText, sizeof activeText, "none");
    else snprintf(activeText, sizeof activeText, "%u", active);
    snprintf(line, size, "armory thumbnails: no progress for %llu s on the %s (icons stay blank)%s: active card %s, phase %u, "
        "preview slots %u/%u; cards (state:count) %s; waiting: %s", (unsigned long long)(stood / 1000), ArmoryScreen(d->screen),
        d->toaster ? ", --toaster_mode is on: it turns texture streaming off, which the thumbnails wait for" : "", activeText,
        ArmoryU32(m, ARMORY_PHASE), ArmoryU32(m, ARMORY_SLOTS), ArmoryU32(m, ARMORY_SLOTS + 4), cards, listed ? waiting : "none");
}

/* One observation: screen is the state stack's top (5 the Armory, 14 the briefing loadout; anything else stops the
 * watch), m the manager's first ARMORY_BYTES (NULL: none). Returns 1 with a line to log. */
static int ArmoryDiagStep(ArmoryDiag* d, uint32_t screen, const uint8_t* m, ULONGLONG now, char* line, size_t size)
{
    uint32_t signature[ARMORY_SIGNATURE];
    int pending = 0, report = 0;
    if ((screen != 5 && screen != 14) || m == NULL || !ArmorySignature(m, signature, &pending))
    {
        if (d->stalled)
        {
            snprintf(line, size, "armory thumbnails: left the %s after %llu s without progress",
                ArmoryScreen(d->screen), (unsigned long long)((now - d->since) / 1000));
            report = 1;
        }
        else if ((screen == 5 || screen == 14) && !d->unwatched)
        {
            snprintf(line, size, "armory thumbnails: the %s is open but its thumbnail manager %s; not watched", ArmoryScreen(screen),
                m == NULL ? "could not be read" : "is not laid out as on build 25480438");
            d->unwatched = report = 1;
        }
        d->has = d->stalled = 0;
        d->screen = 0;
        return report;
    }
    if (!d->watched)
    {
        snprintf(line, size, "armory thumbnails: watching the %s (cards state:count %u:%u %u:%u %u:%u %u:%u %u:%u %u:%u)",
            ArmoryScreen(screen), signature[2], signature[3], signature[4], signature[5], signature[6], signature[7], signature[8],
            signature[9], signature[10], signature[11], signature[12], signature[13]);
        d->watched = report = 1;
    }
    int changed = !d->has || d->screen != screen || memcmp(signature, d->last, sizeof signature) != 0;
    if (changed || !pending)
    {
        if (d->stalled)
        {
            snprintf(line, size, "armory thumbnails: generating again on the %s after %llu s without progress",
                ArmoryScreen(d->screen), (unsigned long long)((now - d->since) / 1000));
            report = 1;
        }
        d->stalled = 0;
        d->since = now;
        d->screen = screen;
        memcpy(d->last, signature, sizeof signature);
        d->has = 1;
        return report;
    }
    if (d->stalled || now - d->since < ARMORY_STALL_MS || d->reports >= ARMORY_REPORTS) return 0;
    d->stalled = 1;
    d->reports++;
    ArmoryStallLine(d, m, now - d->since, line, size);
    return 1;
}

/* The game thread, every update: one observation a second (two small reads off the thumbnail screens). */
static void ArmoryDiagTick(void)
{
    ULONGLONG now = GetTickCount64();
    if (g_armoryDiagOff || now < g_armoryDiag.next) return;
    g_armoryDiag.next = now + 1000;
    if (strcmp(g_build, "25480438")) { g_armoryDiagOff = 1; return; }
    uintptr_t game = ApiModuleBase("game.dll", NULL), states = 0, manager = 0;
    uint32_t stack[6] = {0}, screen = 0;
    if (game && !SafeRead(game + ARMORY_STATES, &states, 8) && states && !SafeRead(states + ARMORY_STACK, stack, sizeof stack) &&
        stack[5] >= 1 && stack[5] <= 5)
        screen = stack[stack[5] - 1];
    static uint8_t bytes[ARMORY_BYTES];
    const uint8_t* m = NULL;
    if ((screen == 5 || screen == 14) && !SafeRead(game + ARMORY_MANAGER, &manager, 8) && manager && !SafeRead(manager, bytes, sizeof bytes))
        m = bytes;
    if (m && !g_armoryDiag.toasterChecked)
    {
        const wchar_t* command = GetCommandLineW();
        g_armoryDiag.toaster = command && wcsstr(command, L"--toaster_mode") ? 1 : 0;
        g_armoryDiag.toasterChecked = 1;
    }
    char line[1024];
    if (ArmoryDiagStep(&g_armoryDiag, screen, m, now, line, sizeof line)) Note("%s", line);
}
