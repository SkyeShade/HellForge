/* The paid-item guard, first half (docs/modding/concepts.md "Premium content"): what mods may not hook or write, and the
 * backend's list of the paid ids the player may not hold now. A code mod runs in the game and can add an item there
 * without asking the backend, so paid_guard_step.c takes back what the list refuses. Native code can always get around
 * this; the guard stops mods that use HellForge's API and the effects of the rest. Included before hooks.c.
 *
 * game.dll 25480438 (KB paid-item-guard-static-re), extents from its unwind table: the functions below either answer
 * "does the player hold this item" without the item table, which no take-back would see, or exist only for content
 * sold for Super Credits. The item table's add, set and remove stay open: gameplay mods change currency amounts through
 * them, and the step catches a paid item they add. */

typedef struct PaidGuardRange { uint32_t start, end; const char* what; } PaidGuardRange;
static const PaidGuardRange kPaidGuardRanges[] = {
    {0x136FB40u, 0x136FBC3u, "the game's \"holds this item\" check"},
    {0x136FC20u, 0x136FD21u, "the game's \"holds this stratagem\" check"},
    {0x136FDB0u, 0x136FE0Cu, "an item ownership check"},
    {0x1372290u, 0x13722F9u, "an item ownership check"},
    {0x1750870u, 0x1751346u, "the login's check of the equipped loadout"},
    {0x136C3A0u, 0x136C6EDu, "the Superstore purchase"},
    {0x136D3D0u, 0x136D766u, "the store front purchase"},
    {0x13714F0u, 0x13716F9u, "the premium warbond purchase"},
    {0x1374F60u, 0x13751C7u, "the store front purchase's reply"},
    {0x103FFE0u, 0x1040118u, "the store front purchase's reply"},
};

/* What [address, address + size) overlaps in game (its base), or NULL. Only for game build 25480438 (g_build). */
static const char* PaidGuardProtected(uintptr_t game, uintptr_t address, size_t size)
{
    if (game == 0 || size == 0 || address < game || strcmp(g_build, "25480438") != 0) return NULL;
    uintptr_t from = address - game, to = from + size;
    for (size_t i = 0; i < sizeof kPaidGuardRanges / sizeof kPaidGuardRanges[0]; i++)
        if (from < kPaidGuardRanges[i].end && to > kPaidGuardRanges[i].start) return kPaidGuardRanges[i].what;
    return NULL;
}

/* A mod's hook or write through the API refused when it touches a protected function: 1 when refused, logged to the mod
 * (NULL: a write the loader could not tie to one) and the loader. Only the mod API's entries call this. */
static int PaidGuardRefuses(HfMod* mod, const char* kind, uintptr_t address, size_t size)
{
    uintptr_t game = ApiModuleBase("game.dll", NULL);
    const char* what = PaidGuardProtected(game, address, size);
    if (what == NULL) return 0;
    ApiLog(mod, "%s refused: game.dll+0x%llx is part of %s, which decides who owns paid content", kind,
        (unsigned long long)(address - game), what);
    if (mod != NULL) Note("paid guard: %s's %s in %s refused", mod->id, kind, what);
    return 1;
}

/* ---- the backend's list -------------------------------------------------------------------------------------------- */

#include <winhttp.h>

#define PG_PATH L"/hellforge/paid-guard"
#define PG_REPLY_MAX (1u << 20)
#define PG_IDS_MAX 65536

static int g_paidGuardPort;              /* the plan's backend_port; 0: no guard */
static SRWLOCK g_paidGuardLock = SRWLOCK_INIT;
static uint32_t* g_paidGuardRefused;     /* sorted; under the lock */
static int g_paidGuardRefusedCount;
static volatile LONG g_paidGuardAnswers; /* lists received; a take-back waits for one newer than its first sighting */
static volatile LONG g_paidGuardAsked;
static HANDLE g_paidGuardWake;
static volatile LONG g_paidGuardThread;  /* 1 once the worker runs */
static char g_paidGuardProblem[160];     /* the last failure, logged once per kind */

/* The plan's line naming the backend's port. 1 when it was one. */
static int PaidGuardPlanLine(char** f, int n)
{
    if (n != 2 || strcmp(f[0], "backend_port") != 0) return 0;
    int port = atoi(f[1]);
    g_paidGuardPort = port > 0 && port <= 65535 ? port : 0;
    return 1;
}

/* Where the list's first id starts, or NULL with no list. */
static const char* PaidGuardListStart(const char* text, size_t length)
{
    static const char key[] = "\"refused\":[";
    for (size_t i = 0; i + sizeof key - 1 <= length; i++)
        if (memcmp(text + i, key, sizeof key - 1) == 0) return text + i + sizeof key - 1;
    return NULL;
}

/* One unsigned decimal at *at, moved past it. 0 when it is none or over 32 bits. */
static int PaidGuardNumber(const char** at, const char* end, uint32_t* value)
{
    const char* p = *at;
    uint64_t v = 0;
    if (p == end || *p < '0' || *p > '9') return 0;
    for (; p < end && *p >= '0' && *p <= '9'; p++)
        if ((v = v * 10 + (uint64_t)(*p - '0')) > UINT32_MAX) return 0;
    *at = p;
    *value = (uint32_t)v;
    return 1;
}

/* The ids of a reply {"refused":[1,2,...]}, sorted as the backend sends them. The count, or -1 when it is not one. */
static int PaidGuardParse(const char* text, size_t length, uint32_t* ids, int capacity)
{
    const char* at = PaidGuardListStart(text, length);
    if (at == NULL) return -1;
    const char* end = text + length;
    int count = 0;
    while (at < end && *at != ']')
    {
        uint32_t value;
        if (!PaidGuardNumber(&at, end, &value)) return -1;
        if (count == capacity || (count > 0 && value <= ids[count - 1])) return -1;
        ids[count++] = (uint32_t)value;
        if (at < end && *at == ',') at++;
        else if (at < end && *at != ']') return -1;
    }
    return at < end ? count : -1;
}

/* Whether the list holds id (under the lock). */
static int PaidGuardRefusedHas(uint32_t id)
{
    int lo = 0, hi = g_paidGuardRefusedCount - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (g_paidGuardRefused[mid] == id) return 1;
        if (g_paidGuardRefused[mid] < id) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

static void PaidGuardFail(const char* why)
{
    if (strcmp(g_paidGuardProblem, why) == 0) return;
    snprintf(g_paidGuardProblem, sizeof g_paidGuardProblem, "%s", why);
    Note("paid guard: the backend's list could not be read (%s); nothing is taken back until it can", why);
}

typedef HINTERNET(WINAPI* PgOpen)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET(WINAPI* PgConnect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET(WINAPI* PgRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD);
typedef BOOL(WINAPI* PgTimeouts)(HINTERNET, int, int, int, int);
typedef BOOL(WINAPI* PgSend)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL(WINAPI* PgReceive)(HINTERNET, LPVOID);
typedef BOOL(WINAPI* PgQuery)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL(WINAPI* PgRead)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL(WINAPI* PgClose)(HINTERNET);

typedef struct
{
    PgOpen open;
    PgConnect connect;
    PgRequest request;
    PgTimeouts timeouts;
    PgSend send;
    PgReceive receive;
    PgQuery query;
    PgRead read;
    PgClose close;
} PgHttp;

/* WinHTTP, loaded when first needed: the loader links nothing new for a guard most sessions never fire. NULL with why. */
static const PgHttp* PaidGuardHttp(const char** why)
{
    static PgHttp http;
    static int loaded;
    if (loaded) return &http;
    HMODULE dll = LoadLibraryW(L"winhttp.dll");
    if (dll == NULL) { *why = "winhttp.dll is missing"; return NULL; }
    http.open = (PgOpen)(void*)GetProcAddress(dll, "WinHttpOpen");
    http.connect = (PgConnect)(void*)GetProcAddress(dll, "WinHttpConnect");
    http.request = (PgRequest)(void*)GetProcAddress(dll, "WinHttpOpenRequest");
    http.timeouts = (PgTimeouts)(void*)GetProcAddress(dll, "WinHttpSetTimeouts");
    http.send = (PgSend)(void*)GetProcAddress(dll, "WinHttpSendRequest");
    http.receive = (PgReceive)(void*)GetProcAddress(dll, "WinHttpReceiveResponse");
    http.query = (PgQuery)(void*)GetProcAddress(dll, "WinHttpQueryHeaders");
    http.read = (PgRead)(void*)GetProcAddress(dll, "WinHttpReadData");
    http.close = (PgClose)(void*)GetProcAddress(dll, "WinHttpCloseHandle");
    loaded = http.open && http.connect && http.request && http.timeouts && http.send && http.receive && http.query && http.read && http.close;
    if (!loaded) *why = "WinHTTP lacks a function";
    return loaded ? &http : NULL;
}

/* The reply's status once sent and answered, 0 when it was not. */
static DWORD PaidGuardStatus(const PgHttp* http, HINTERNET call)
{
    DWORD status = 0, size = sizeof status;
    if (!http->timeouts(call, 2000, 2000, 5000, 5000) ||
        !http->send(call, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !http->receive(call, NULL))
        return 0;
    if (!http->query(call, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
            WINHTTP_NO_HEADER_INDEX))
        return 0;
    return status;
}

/* The reply's body (malloc'd, NUL-terminated) and its length, or NULL with why. */
static char* PaidGuardBody(const PgHttp* http, HINTERNET call, size_t* length, const char** why)
{
    char* body = (char*)malloc(PG_REPLY_MAX + 1);
    if (body == NULL) { *why = "no memory"; return NULL; }
    size_t used = 0;
    DWORD got = 0;
    while (used < PG_REPLY_MAX && http->read(call, body + used, (DWORD)(PG_REPLY_MAX - used), &got) && got > 0) used += got;
    body[used] = 0;
    *length = used;
    if (used < PG_REPLY_MAX) return body;
    free(body);
    *why = "the reply is too large";
    return NULL;
}

/* GET the list from the session's backend on loopback (the address the game uses, so the sandbox lets it through). The
 * reply's body and its length, or NULL with why. */
static char* PaidGuardGet(int port, size_t* length, const char** why)
{
    const PgHttp* http = PaidGuardHttp(why);
    if (http == NULL) return NULL;
    char* body = NULL;
    *why = "the backend did not answer";
    HINTERNET session = http->open(L"HellForge-loader", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET link = session ? http->connect(session, L"127.0.0.1", (INTERNET_PORT)port, 0) : NULL;
    HINTERNET call = link ? http->request(link, L"GET", PG_PATH, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : NULL;
    DWORD status = call ? PaidGuardStatus(http, call) : 0;
    if (status == 200) body = PaidGuardBody(http, call, length, why);
    else if (status == 409) *why = "the backend serves this profile without ownership rules";
    else if (status != 0) *why = "the backend refused the request";
    if (call) http->close(call);
    if (link) http->close(link);
    if (session) http->close(session);
    return body;
}

/* One fetch into ids: the count, or -1 after logging why. */
static int PaidGuardFetch(uint32_t* ids)
{
    size_t length = 0;
    const char* why = NULL;
    char* body = PaidGuardGet(g_paidGuardPort, &length, &why);
    int count = body != NULL ? PaidGuardParse(body, length, ids, PG_IDS_MAX) : -1;
    free(body);
    if (count < 0) PaidGuardFail(why != NULL && body == NULL ? why : "the reply is not a list");
    return count;
}

/* Swaps a fetched list in whole, so the game thread never reads half of one. */
static void PaidGuardStore(const uint32_t* ids, int count)
{
    uint32_t* copy = (uint32_t*)malloc(sizeof *copy * (size_t)(count ? count : 1));
    if (copy == NULL) { PaidGuardFail("no memory"); return; }
    memcpy(copy, ids, sizeof *copy * (size_t)count);
    AcquireSRWLockExclusive(&g_paidGuardLock);
    uint32_t* old = g_paidGuardRefused;
    g_paidGuardRefused = copy;
    g_paidGuardRefusedCount = count;
    ReleaseSRWLockExclusive(&g_paidGuardLock);
    free(old);
    if (g_paidGuardProblem[0]) { g_paidGuardProblem[0] = 0; Note("paid guard: the backend's list is read again"); }
    if (InterlockedIncrement(&g_paidGuardAnswers) == 1) Note("paid guard: %d paid ids the player may not hold now", count);
}

/* The worker: one fetch per wake. */
static DWORD WINAPI PaidGuardWorker(void* unused)
{
    (void)unused;
    uint32_t* ids = (uint32_t*)malloc(sizeof *ids * PG_IDS_MAX);
    for (;;)
    {
        WaitForSingleObject(g_paidGuardWake, INFINITE);
        int count = ids != NULL ? PaidGuardFetch(ids) : -1;
        if (ids == NULL) PaidGuardFail("no memory");
        if (count >= 0) PaidGuardStore(ids, count);
    }
}

/* Asks for a fresh list (the worker starts on the first ask). 0 when it could not start. */
static int PaidGuardAsk(void)
{
    if (g_paidGuardPort == 0) return 0;
    if (InterlockedCompareExchange(&g_paidGuardThread, 1, 0) == 0)
    {
        g_paidGuardWake = CreateEventW(NULL, FALSE, FALSE, NULL);
        HANDLE thread = g_paidGuardWake ? CreateThread(NULL, 0, PaidGuardWorker, NULL, 0, NULL) : NULL;
        if (thread == NULL) { Note("paid guard: its worker could not start; off this session"); g_paidGuardPort = 0; return 0; }
        CloseHandle(thread);
    }
    InterlockedIncrement(&g_paidGuardAsked);
    SetEvent(g_paidGuardWake);
    return 1;
}
