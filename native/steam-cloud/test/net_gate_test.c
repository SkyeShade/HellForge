// The network gate's census (net_gate.c) in the built steam_api64.dll stand-in: loaded with HELLFORGE_NETGATE=census, then
// each hooked call is made and its line looked for in the log. Nothing leaves this PC: outside addresses go to sockets
// that cannot send (invalid or unbound) and to TEST-NET-1 (192.0.2.0/24, never routed); WinHTTP only opens a handle.
// Usage: net_gate_test.exe <steam_api64.dll> <log>
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <sddl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int g_failed;

static void Check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failed++;
}

static char *ReadAll(const wchar_t *path)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(file, NULL), read = 0;
    char *text = (char *)calloc(size + 1, 1);
    ReadFile(file, text, size, &read, NULL);
    CloseHandle(file);
    return text;
}

// A line holding every one of the given pieces.
static int Has(const char *log, const char *a, const char *b, const char *c)
{
    for (const char *line = log; line && *line;)
    {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        char buffer[2048];
        if (n >= sizeof buffer) n = sizeof buffer - 1;
        memcpy(buffer, line, n);
        buffer[n] = 0;
        if (strstr(buffer, a) && (!b || strstr(buffer, b)) && (!c || strstr(buffer, c))) return 1;
        line = end ? end + 1 : NULL;
    }
    return 0;
}

// One call site for the repeats (a loop the compiler unrolls would be twelve; the result is used, so no tail jump).
static volatile int g_sink;
__attribute__((noinline)) static void ConnectOnce(const struct sockaddr_in *to)
{
    g_sink += connect(INVALID_SOCKET, (const struct sockaddr *)to, sizeof *to);
}

// Whether a call failed with ERROR_ACCESS_DENIED as its last error; said when not.
static int Refused(const char *what, int failed)
{
    DWORD error = GetLastError();
    if (failed && error == ERROR_ACCESS_DENIED) return 1;
    printf("note: %s was not refused with ERROR_ACCESS_DENIED (%s, last error %lu)\n", what, failed ? "failed" : "opened", error);
    return 0;
}

// Named objects and a file nobody may open (a DACL that denies everyone), opened again: each is refused with
// ERROR_ACCESS_DENIED, as a low-integrity game is refused what other programs made at the player's level. Returns how
// the calls whose caller saw ERROR_ACCESS_DENIED, one bit each (REFUSED_*), or -1 when the fixture cannot be made.
enum { REFUSED_EVENT = 1, REFUSED_MAPPING = 2, REFUSED_MUTEX = 4, REFUSED_PIPE = 8, REFUSED_FILE = 16 };
static int RefusedCalls(const wchar_t *folder)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;GA;;;WD)", SDDL_REVISION_1, &sd, NULL)) return -1;
    SECURITY_ATTRIBUTES none = {sizeof none, sd, FALSE};
    HANDLE event = CreateEventW(&none, TRUE, FALSE, L"Local\\hf-netgate-denied-event");
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &none, PAGE_READWRITE, 0, 4096, L"Local\\hf-netgate-denied-mapping");
    HANDLE mutex = CreateMutexW(&none, FALSE, L"Local\\hf-netgate-denied-mutex");
    HANDLE pipe = CreateNamedPipeW(L"\\\\.\\pipe\\hf-netgate-denied-pipe", PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE, 1, 64, 64, 0, &none);
    wchar_t file[MAX_PATH];
    swprintf(file, MAX_PATH, L"%ls\\hf-netgate-denied.txt", folder);
    DeleteFileW(file);
    // kept open (and deleted when closed): a file nobody may open is not one this test can delete afterwards
    HANDLE made = CreateFileW(file, GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, &none, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    int kept = 0;
    kept |= REFUSED_EVENT * Refused("OpenEventW", !OpenEventW(EVENT_ALL_ACCESS, FALSE, L"Local\\hf-netgate-denied-event"));
    kept |= REFUSED_MAPPING * Refused("OpenFileMappingW", !OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\hf-netgate-denied-mapping"));
    kept |= REFUSED_MUTEX * Refused("OpenMutexW", !OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\hf-netgate-denied-mutex"));
    kept |= REFUSED_PIPE * Refused("CreateFileW (pipe)", CreateFileW(L"\\\\.\\pipe\\hf-netgate-denied-pipe", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL) ==
        INVALID_HANDLE_VALUE);
    kept |= REFUSED_FILE * Refused("CreateFileW (file)", CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL) ==
        INVALID_HANDLE_VALUE);
    // refusals that are no sandbox's doing are not written: a folder opened as a file (Windows refuses it at any level)
    wchar_t folderAsFile[MAX_PATH];
    swprintf(folderAsFile, MAX_PATH, L"%ls\\hf-netgate-folder", folder);
    CreateDirectoryW(folderAsFile, NULL);
    HANDLE opened = CreateFileW(folderAsFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (opened != INVALID_HANDLE_VALUE) CloseHandle(opened);
    // a name that does not exist is not a refusal: nothing is written for it
    OpenEventW(EVENT_ALL_ACCESS, FALSE, L"Local\\hf-netgate-no-such-event");
    HANDLE made_[] = {event, mapping, mutex, pipe, made};
    for (size_t i = 0; i < sizeof made_ / sizeof made_[0]; i++) if (made_[i] && made_[i] != INVALID_HANDLE_VALUE) CloseHandle(made_[i]);
    LocalFree(sd);
    return kept;
}

// The tablet gate (tablet_gate.c): sandboxed, Wintab32 is refused by any name and loader; unsandboxed it loads. A copy of
// fake_game.dll (beside this test, no DllMain) named Wintab32.dll stands in for a driver, as CI has none.
static void TabletChecks(bool sandboxed, const wchar_t *folder)
{
    wchar_t self[MAX_PATH], fake[MAX_PATH], wintab[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t *slash = wcsrchr(self, L'\\');
    if (slash) slash[1] = 0;
    _snwprintf(fake, MAX_PATH, L"%lsfake_game.dll", self);
    _snwprintf(wintab, MAX_PATH, L"%ls\\Wintab32.dll", folder);
    Check(CopyFileW(fake, wintab, FALSE), "a stand-in Wintab32.dll is made from fake_game.dll");
    HMODULE byPath = LoadLibraryExW(wintab, NULL, 0);
    DWORD byPathError = GetLastError();
    if (!sandboxed)
    {
        Check(byPath != NULL, "unsandboxed, Wintab32.dll loads");
        if (byPath) FreeLibrary(byPath);
        return;
    }
    Check(!byPath && byPathError == ERROR_MOD_NOT_FOUND, "sandboxed, Wintab32.dll by its path is refused as not found");
    SetLastError(0);
    Check(!LoadLibraryW(L"Wintab32.dll") && GetLastError() == ERROR_MOD_NOT_FOUND, "LoadLibraryW of Wintab32.dll is refused");
    Check(!LoadLibraryA("WINTAB32") && GetLastError() == ERROR_MOD_NOT_FOUND, "LoadLibraryA of WINTAB32, no extension, is refused");
    Check(!LoadLibraryExA("C:/x/wintab32.dll", NULL, 0) && GetLastError() == ERROR_MOD_NOT_FOUND, "LoadLibraryExA with a forward-slash path is refused");
    HMODULE other = LoadLibraryW(L"version.dll");
    Check(other != NULL, "sandboxed, any other DLL still loads");
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: net_gate_test <steam_api64.dll> <log> [hook groups]\n"); return 2; }
    // A 3rd argument chooses the hook groups; a subset like "dns" (anything but "all") runs the filtered checks: only
    // that group is placed, so a name lookup is logged and a connect is not. No 3rd argument means "all" (the harness
    // exercises every hook, unlike the session default, which leaves out the connect groups the game cannot abide).
    // "default" and "sandbox-default" set no groups, as a session does, the second with HELLFORGE_BRIDGE_PIPE set as a
    // sandboxed session has it: the default adds the denied group there only.
    const wchar_t *groups = argc > 3 ? argv[3] : L"all";
    bool sandboxDefault = !_wcsicmp(groups, L"sandbox-default"), sessionDefault = sandboxDefault || !_wcsicmp(groups, L"default");
    bool filtered = !sessionDefault && _wcsicmp(groups, L"all") != 0;
    DeleteFileW(argv[2]);
    SetEnvironmentVariableW(L"HELLFORGE_NETGATE", L"census");
    SetEnvironmentVariableW(L"HELLFORGE_NETGATE_LOG", argv[2]);
    SetEnvironmentVariableW(L"HELLFORGE_NETGATE_HOOKS", sessionDefault ? NULL : groups);
    SetEnvironmentVariableW(L"HELLFORGE_BRIDGE_PIPE", sandboxDefault ? L"net-gate-test" : NULL);
    SetEnvironmentVariableW(L"HELLFORGE_MODLOADER", NULL);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    HMODULE standIn = LoadLibraryW(argv[1]);
    Check(standIn != NULL, "the stand-in loads");

    struct sockaddr_in loop = {AF_INET, htons(9)}, outside = {AF_INET, htons(9)};
    loop.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    inet_pton(AF_INET, "192.0.2.1", &outside.sin_addr);
    for (int i = 0; i < 12; i++) ConnectOnce(&outside);
    connect(INVALID_SOCKET, (struct sockaddr *)&loop, sizeof loop);
    sendto(INVALID_SOCKET, "x", 1, 0, (struct sockaddr *)&outside, sizeof outside);
    struct addrinfo *found = NULL;
    if (getaddrinfo("localhost", "9", NULL, &found) == 0) freeaddrinfo(found);
    ADDRINFOW *wide = NULL;
    if (GetAddrInfoW(L"localhost", L"9", NULL, &wide) == 0) FreeAddrInfoW(wide);
    // ConnectEx: asked for through WSAIoctl, then called on an unbound socket (refused before anything is sent)
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    GUID guid = WSAID_CONNECTEX;
    LPFN_CONNECTEX connectEx = NULL;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof guid, &connectEx, sizeof connectEx, &bytes, NULL, NULL);
    Check(connectEx != NULL, "ConnectEx is handed out");
    OVERLAPPED overlapped = {0};
    if (connectEx) Check(!connectEx(s, (struct sockaddr *)&outside, sizeof outside, NULL, 0, NULL, &overlapped), "ConnectEx on an unbound socket still fails");
    closesocket(s);
    // WinHTTP loaded after the gate: hooked as it loads
    HMODULE winHttp = LoadLibraryW(L"winhttp.dll");
    typedef void *(WINAPI *OpenFn)(const wchar_t *, DWORD, const wchar_t *, const wchar_t *, DWORD);
    typedef void *(WINAPI *ConnectFn)(void *, const wchar_t *, WORD, DWORD);
    typedef BOOL (WINAPI *CloseFn)(void *);
    OpenFn open = winHttp ? (OpenFn)(void *)GetProcAddress(winHttp, "WinHttpOpen") : NULL;
    ConnectFn connectTo = winHttp ? (ConnectFn)(void *)GetProcAddress(winHttp, "WinHttpConnect") : NULL;
    CloseFn close = winHttp ? (CloseFn)(void *)GetProcAddress(winHttp, "WinHttpCloseHandle") : NULL;
    void *session = open ? open(L"net_gate_test", 0, NULL, NULL, 0) : NULL;
    void *connection = session && connectTo ? connectTo(session, L"example.invalid", 443, 0) : NULL;
    if (connection) close(connection);
    if (session) close(session);
    wchar_t folder[MAX_PATH];
    wcsncpy(folder, argv[2], MAX_PATH - 1);
    folder[MAX_PATH - 1] = 0;
    wchar_t *slash = wcsrchr(folder, L'\\');
    if (slash) *slash = 0;
    else wcscpy(folder, L".");
    int refused = RefusedCalls(folder);
    STARTUPINFOW startup = {sizeof startup};
    PROCESS_INFORMATION process;
    wchar_t command[] = L"C:\\HellForge\\does-not-exist\\crs-handler.exe";
    Check(!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process), "a missing program still fails to start");

    char *log = ReadAll(argv[2]);
    Check(log != NULL, "the census log is written");
    Check(Has(log, "\"gate\":\"census\"", "\"hooked\":", NULL), "the census reports its hooks");
    if (sessionDefault)
    {
        TabletChecks(sandboxDefault, folder);
        free(log);
        log = ReadAll(argv[2]);
        Check(!sandboxDefault == !Has(log, "\"gate\":\"tablet\"", "\"hooked\":4,\"of\":4", "\"watchingLoads\":true"),
            sandboxDefault ? "a sandboxed session's tablet gate hooks all four loaders" : "an unsandboxed session has no tablet gate");
        Check(!sandboxDefault == !Has(log, "\"api\":\"LoadLibraryW\"", "Wintab32.dll", "\"scope\":\"refused\""),
            sandboxDefault ? "a refused Wintab32 load is written" : "no refusal is written unsandboxed");
        Check(!Has(log, "\"gate\":\"tablet loaded\"", NULL, NULL), "no Wintab32 got past the gate");
        // The session's default: no connect groups; denied only when sandboxed, and then refusals are written.
        Check(Has(log, sandboxDefault ? "\"groups\":\",send,dns,http,process,denied,\"" : "\"groups\":\",send,dns,http,process,\"", NULL, NULL),
            "the session's default groups");
        Check(!Has(log, "\"api\":\"connect\"", NULL, NULL), "the default leaves connect out");
        Check(Has(log, "\"api\":\"getaddrinfo\"", "localhost:9", NULL), "the default hooks name lookups");
        Check(!sandboxDefault == !Has(log, "\"api\":\"OpenEventW\"", "hf-netgate-denied-event", "\"scope\":\"denied\""),
            sandboxDefault ? "a sandboxed session's default writes refusals" : "an unsandboxed session's default writes no refusals");
        printf("%s\n", g_failed ? "network gate default: FAILED" : "PASS network gate default groups");
        return g_failed ? 1 : 0;
    }
    if (filtered)
    {
        // Only the dns group is on: name lookups are logged, connect and the rest are not (they are not even hooked).
        Check(Has(log, "\"groups\":\",dns,\"", NULL, NULL), "the census names the groups it was given");
        Check(Has(log, "\"api\":\"getaddrinfo\"", "localhost:9", NULL), "the dns group is hooked");
        Check(!Has(log, "\"api\":\"connect\"", NULL, NULL), "the connect group is not hooked");
        Check(!Has(log, "\"api\":\"WinHttpConnect\"", NULL, NULL), "the http group is not hooked");
        Check(!Has(log, "\"api\":\"CreateProcessW\"", NULL, NULL), "the process group is not hooked");
        Check(!Has(log, "\"scope\":\"denied\"", NULL, NULL), "the denied group is not hooked");
        printf("%s\n", g_failed ? "network gate groups: FAILED" : "PASS network gate groups: only the chosen group is hooked");
        return g_failed ? 1 : 0;
    }
    Check(Has(log, "\"api\":\"connect\"", "192.0.2.1:9", "\"scope\":\"external\""), "connect to an outside address");
    Check(Has(log, "\"api\":\"connect\"", "127.0.0.1:9", "\"scope\":\"loopback\""), "connect to loopback");
    Check(Has(log, "\"count\":10", "192.0.2.1:9", NULL) && !Has(log, "\"count\":4", "192.0.2.1:9", NULL), "repeats are counted, written at 1, 2, 3 and 10");
    Check(Has(log, "\"caller\":\"net_gate_test.exe+", NULL, NULL), "the caller is named");
    Check(Has(log, "\"api\":\"sendto\"", "192.0.2.1:9", NULL), "sendto");
    Check(Has(log, "\"api\":\"getaddrinfo\"", "localhost:9", "\"scope\":\"loopback\""), "getaddrinfo");
    Check(Has(log, "\"api\":\"GetAddrInfoW\"", "localhost:9", NULL), "GetAddrInfoW");
    Check(Has(log, "\"api\":\"ConnectEx\"", "192.0.2.1:9", NULL), "ConnectEx");
    Check(Has(log, "\"gate\":\"module loaded\"", "winhttp.dll", NULL), "a network module loaded later is hooked");
    Check(Has(log, "\"api\":\"WinHttpConnect\"", "example.invalid:443", "\"scope\":\"name\""), "WinHttpConnect");
    Check(Has(log, "\"api\":\"CreateProcessW\"", "crs-handler.exe", "\"caller\":\"net_gate_test.exe+"), "a program started, named by its caller past kernel32");
    // Windows refuses all five; Wine enforces no DACL on a mutex or a pipe, so those two are checked when refused.
    Check(refused >= 0 && (refused & (REFUSED_EVENT | REFUSED_MAPPING | REFUSED_FILE)) == (REFUSED_EVENT | REFUSED_MAPPING | REFUSED_FILE),
        "a refused call keeps ERROR_ACCESS_DENIED as its caller's last error");
    Check(Has(log, "\"api\":\"OpenEventW\"", "hf-netgate-denied-event", "\"scope\":\"denied\""), "a refused event, by name");
    Check(Has(log, "\"api\":\"OpenFileMappingW\"", "hf-netgate-denied-mapping", "\"caller\":\"net_gate_test.exe+"), "refused shared memory, by its caller");
    Check(Has(log, "\"api\":\"CreateFileW\"", "hf-netgate-denied.txt", "\"scope\":\"denied\""), "a refused file");
    Check(!(refused & REFUSED_MUTEX) == !Has(log, "\"api\":\"OpenMutexW\"", "hf-netgate-denied-mutex", "\"scope\":\"denied\""),
        "a mutex is written exactly when it was refused");
    Check(!(refused & REFUSED_PIPE) == !Has(log, "\"api\":\"CreateFileW\"", "hf-netgate-denied-pipe", "\"scope\":\"denied\""),
        "a pipe is written exactly when it was refused");
    Check(!Has(log, "hf-netgate-no-such-event", NULL, NULL), "a name that does not exist is not written");
    Check(!Has(log, "hf-netgate-folder", NULL, NULL), "a folder opened as a file is not written: Windows refuses it at any level");
    printf("%s\n", g_failed ? "network gate census: FAILED" : "PASS network gate census: hooks, scopes, repeats, callers, late modules, programs");
    return g_failed ? 1 : 0;
}
