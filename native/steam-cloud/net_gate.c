// HellForge's network gate in the copied game, step 1: the census (docs/engine-roadmap.md, the network gate). The stand-in is
// loaded before any of the game's own code runs (the executable imports it), so its DllMain hooks every way the game's
// process reaches the network before the game can use one: Winsock (connect, ConnectEx, WSAConnect, WSAConnectByName,
// sendto, WSASendTo, WSASendMsg, the name lookups), WinHTTP and WinINet connections, and the programs it starts (the
// crash handler is a program of its own). Each attempt is written to HELLFORGE_NETGATE_LOG as a JSON line: what was
// called, where to, whether that is this PC (loopback) and which module asked. The census only writes: Windows Firewall
// still blocks the copy, and nothing is refused here. Repeats of the same call, target and caller are counted and
// written at 1, 2, 3, 10, 100, 1000... times.
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <winternl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "MinHook.h"
#include "net_gate.h"
#include "../common/hf_privacy.h"

#define KEYS 512
#define TARGET_TEXT 320

static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static bool g_on;
static DWORD g_depth = TLS_OUT_OF_INDEXES; /* this thread's hooked calls in progress: only the outermost is written */
static struct { uint64_t key; uint32_t count; } g_seen[KEYS];

static void Write(const char *line, int length)
{
    /* A caller's path can be in the user folder: the census names it %USERPROFILE% (hf_privacy.h). */
    char scrubbed[1024];
    size_t size = (size_t)length < sizeof scrubbed ? (size_t)length : sizeof scrubbed;
    memcpy(scrubbed, line, size);
    size = HfPrivacyScrub(scrubbed, size, sizeof scrubbed);
    DWORD written;
    WriteFile(g_log, scrubbed, (DWORD)size, &written, NULL);
}

// JSON string contents: quotes, backslashes and control characters escaped; at most `limit` bytes of `text`.
static int EscapeJson(char *out, int room, const char *text, int limit)
{
    int n = 0;
    for (int i = 0; text && text[i] && i < limit && n < room - 7; i++)
    {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += snprintf(out + n, room - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

static void Narrow(const wchar_t *wide, char *out, int room)
{
    out[0] = 0;
    if (wide) WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, room, NULL, NULL);
    out[room - 1] = 0;
}

static HMODULE ModuleOf(void *address)
{
    HMODULE module = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (const wchar_t *)address, &module);
    return module;
}

// Of a stack's frames, the first above address's own frame that lies beyond kernel32 and kernelbase, with its module in
// *module: its index, or -1 when address is not on the stack or nothing beyond it is.
static int FrameBeyond(void *const *frames, USHORT count, void *address, HMODULE kernel32, HMODULE kernelBase, HMODULE *module)
{
    for (USHORT i = 0; i < count; i++)
    {
        if (frames[i] != address) continue;
        for (USHORT j = i + 1; j < count; j++)
        {
            HMODULE beyond = ModuleOf(frames[j]);
            if (beyond != kernel32 && beyond != kernelBase) { *module = beyond; return j; }
        }
        return -1;
    }
    return -1;
}

// The module an address belongs to, as "name+0xrva". kernel32 and kernelbase only pass calls on (kernel32's
// CreateProcessW calls kernelbase's), so a call from them is named by the first caller beyond them on the stack.
static void Caller(void *address, char *out, int room)
{
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll"), kernelBase = GetModuleHandleW(L"kernelbase.dll");
    HMODULE module = ModuleOf(address);
    if (module && (module == kernel32 || module == kernelBase))
    {
        void *frames[24];
        USHORT count = RtlCaptureStackBackTrace(0, 24, frames, NULL);
        int beyond = FrameBeyond(frames, count, address, kernel32, kernelBase, &module);
        if (beyond >= 0) address = frames[beyond];
    }
    wchar_t path[MAX_PATH];
    if (!module || !GetModuleFileNameW(module, path, MAX_PATH))
    {
        snprintf(out, room, "?+0x%llx", (unsigned long long)(uintptr_t)address);
        return;
    }
    const wchar_t *name = wcsrchr(path, L'\\');
    char narrow[MAX_PATH];
    Narrow(name ? name + 1 : path, narrow, sizeof narrow);
    snprintf(out, room, "%s+0x%llx", narrow, (unsigned long long)((uintptr_t)address - (uintptr_t)module));
}

static uint64_t Hash(uint64_t h, const char *text)
{
    for (; text && *text; text++) h = (h ^ (unsigned char)*text) * 0x100000001B3ull;
    return h;
}

// How many times key has been seen, this time included (g_lock held). Once the table is full a new key counts 1 each time.
static uint32_t Seen(uint64_t key)
{
    for (int i = 0; i < KEYS; i++)
    {
        int slot = (int)((key + (uint64_t)i) % KEYS);
        if (g_seen[slot].key == key) return ++g_seen[slot].count;
        if (g_seen[slot].key == 0) { g_seen[slot].key = key; g_seen[slot].count = 1; return 1; }
    }
    return 1;
}

// Repeats are written at 1, 2, 3, 10, 100, 1000... times.
static bool Milestone(uint32_t count)
{
    bool write = count <= 3;
    for (uint32_t power = 10; power && power <= count; power *= 10) if (count == power) write = true;
    return write;
}

// The attempt's JSON line (g_lock held).
static void WriteAttempt(const char *api, const char *target, const char *scope, const char *caller, uint32_t count)
{
    SYSTEMTIME now;
    GetSystemTime(&now);
    char line[1024], escaped[2 * TARGET_TEXT], who[2 * (MAX_PATH + 32)];
    EscapeJson(escaped, sizeof escaped, target, TARGET_TEXT);
    EscapeJson(who, sizeof who, caller, MAX_PATH + 32);
    int length = snprintf(line, sizeof line, "{\"utc\":\"%02u:%02u:%02u.%03u\",\"api\":\"%s\",\"to\":\"%s\",\"scope\":\"%s\",\"caller\":\"%s\",\"thread\":%lu,\"count\":%u}\n",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, api, escaped, scope, who, GetCurrentThreadId(), count);
    if (length > 0) Write(line, length < (int)sizeof line ? length : (int)sizeof line - 1);
}

// Writes an attempt: api, target, scope ("loopback", "external", "unspecified", "name", "program"), caller.
static void Note(const char *api, const char *target, const char *scope, void *returnAddress)
{
    if (!g_on) return;
    char caller[MAX_PATH + 32];
    Caller(returnAddress, caller, sizeof caller);
    uint64_t key = Hash(Hash(Hash(0xCBF29CE484222325ull, api), target), caller);
    EnterCriticalSection(&g_lock);
    uint32_t count = Seen(key);
    if (Milestone(count)) WriteAttempt(api, target, scope, caller, count);
    LeaveCriticalSection(&g_lock);
}

static bool Enter(void)
{
    uintptr_t depth = (uintptr_t)TlsGetValue(g_depth);
    TlsSetValue(g_depth, (void *)(depth + 1));
    return depth == 0;
}

static void Leave(void)
{
    uintptr_t depth = (uintptr_t)TlsGetValue(g_depth);
    TlsSetValue(g_depth, (void *)(depth ? depth - 1 : 0));
}

// A port in network byte order (Winsock's own helpers are not imported: the stand-in imports only system DLLs).
static unsigned Port(u_short port) { const unsigned char *b = (const unsigned char *)&port; return (unsigned)b[0] << 8 | b[1]; }

static const char *ScopeOfV4(const struct sockaddr_in *v4, char *text, int room)
{
    const unsigned char *b = (const unsigned char *)&v4->sin_addr;
    snprintf(text, room, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], Port(v4->sin_port));
    if (b[0] == 127) return "loopback";
    if (v4->sin_addr.s_addr == 0) return "unspecified";
    return "external";
}

static const char *ScopeOfV6(const struct sockaddr_in6 *v6, char *text, int room)
{
    const unsigned char *b = v6->sin6_addr.s6_addr;
    int n = snprintf(text, room, "[");
    for (int i = 0; i < 16 && n < room - 12; i += 2) n += snprintf(text + n, room - n, "%s%x", i ? ":" : "", (b[i] << 8) | b[i + 1]);
    snprintf(text + n, room - n, "]:%u", Port(v6->sin6_port));
    static const unsigned char loop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, any[16] = {0};
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (!memcmp(b, loop, 16) || (!memcmp(b, mapped, 12) && b[12] == 127)) return "loopback";
    if (!memcmp(b, any, 16)) return "unspecified";
    return "external";
}

static const char *ScopeOf(const struct sockaddr *address, int length, char *text, int room)
{
    text[0] = 0;
    if (!address || length < (int)sizeof(struct sockaddr)) { snprintf(text, room, "(none)"); return "unspecified"; }
    if (address->sa_family == AF_INET && length >= (int)sizeof(struct sockaddr_in))
        return ScopeOfV4((const struct sockaddr_in *)address, text, room);
    if (address->sa_family == AF_INET6 && length >= (int)sizeof(struct sockaddr_in6))
        return ScopeOfV6((const struct sockaddr_in6 *)address, text, room);
    snprintf(text, room, "family %u", address->sa_family);
    return "external";
}

static void NoteAddress(const char *api, const struct sockaddr *address, int length, void *returnAddress)
{
    char text[128];
    const char *scope = ScopeOf(address, length, text, sizeof text);
    Note(api, text, scope, returnAddress);
}

// A host name asked for: "localhost" (and numeric loopback) is this PC; any other name is outside.
static const char *NameScope(const char *name)
{
    if (!name || !name[0]) return "unspecified";
    if (!_stricmp(name, "localhost") || !strncmp(name, "127.", 4) || !strcmp(name, "::1")) return "loopback";
    return "name";
}

static void NoteName(const char *api, const char *name, const char *service, void *returnAddress)
{
    char text[TARGET_TEXT];
    snprintf(text, sizeof text, "%s%s%s", name ? name : "(null)", service ? ":" : "", service ? service : "");
    Note(api, text, NameScope(name), returnAddress);
}

static void NoteWideName(const char *api, const wchar_t *name, const wchar_t *service, void *returnAddress)
{
    char host[TARGET_TEXT], port[64];
    Narrow(name, host, sizeof host);
    Narrow(service, port, sizeof port);
    NoteName(api, name ? host : NULL, service ? port : NULL, returnAddress);
}

#define RETURN_ADDRESS __builtin_extract_return_addr(__builtin_return_address(0))

/* ---- Winsock ---- */
typedef int (WSAAPI *ConnectFn)(SOCKET, const struct sockaddr *, int);
typedef int (WSAAPI *WsaConnectFn)(SOCKET, const struct sockaddr *, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
typedef BOOL (WSAAPI *ConnectByNameWFn)(SOCKET, LPWSTR, LPWSTR, LPDWORD, LPSOCKADDR, LPDWORD, LPSOCKADDR, const struct timeval *, LPWSAOVERLAPPED);
typedef BOOL (WSAAPI *ConnectByNameAFn)(SOCKET, LPCSTR, LPCSTR, LPDWORD, LPSOCKADDR, LPDWORD, LPSOCKADDR, const struct timeval *, LPWSAOVERLAPPED);
typedef int (WSAAPI *SendToFn)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int (WSAAPI *WsaSendToFn)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const struct sockaddr *, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI *WsaSendMsgFn)(SOCKET, LPWSAMSG, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI *WsaIoctlFn)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI *GetAddrInfoFn)(PCSTR, PCSTR, const ADDRINFOA *, PADDRINFOA *);
typedef int (WSAAPI *GetAddrInfoWFn)(PCWSTR, PCWSTR, const ADDRINFOW *, PADDRINFOW *);
typedef int (WSAAPI *GetAddrInfoExWFn)(PCWSTR, PCWSTR, DWORD, LPGUID, const ADDRINFOEXW *, PADDRINFOEXW *, struct timeval *, LPOVERLAPPED,
    LPLOOKUPSERVICE_COMPLETION_ROUTINE, LPHANDLE);
typedef struct hostent *(WSAAPI *GetHostByNameFn)(const char *);

static ConnectFn g_connect;
static WsaConnectFn g_wsaConnect;
static ConnectByNameWFn g_connectByNameW;
static ConnectByNameAFn g_connectByNameA;
static SendToFn g_sendTo;
static WsaSendToFn g_wsaSendTo;
static WsaSendMsgFn g_wsaSendMsg;
static WsaIoctlFn g_wsaIoctl;
static LPFN_CONNECTEX g_connectEx;
static GetAddrInfoFn g_getAddrInfo;
static GetAddrInfoWFn g_getAddrInfoW;
static GetAddrInfoExWFn g_getAddrInfoExW;
static GetHostByNameFn g_getHostByName;

static int WSAAPI HookConnect(SOCKET s, const struct sockaddr *to, int length)
{
    if (Enter()) NoteAddress("connect", to, length, RETURN_ADDRESS);
    int result = g_connect(s, to, length);
    Leave();
    return result;
}

static int WSAAPI HookWsaConnect(SOCKET s, const struct sockaddr *to, int length, LPWSABUF a, LPWSABUF b, LPQOS c, LPQOS d)
{
    if (Enter()) NoteAddress("WSAConnect", to, length, RETURN_ADDRESS);
    int result = g_wsaConnect(s, to, length, a, b, c, d);
    Leave();
    return result;
}

static BOOL WSAAPI HookConnectByNameW(SOCKET s, LPWSTR name, LPWSTR service, LPDWORD a, LPSOCKADDR b, LPDWORD c, LPSOCKADDR d, const struct timeval *e, LPWSAOVERLAPPED f) // lint-ok: R12 mirrors WSAConnectByNameW
{
    if (Enter()) NoteWideName("WSAConnectByNameW", name, service, RETURN_ADDRESS);
    BOOL result = g_connectByNameW(s, name, service, a, b, c, d, e, f);
    Leave();
    return result;
}

static BOOL WSAAPI HookConnectByNameA(SOCKET s, LPCSTR name, LPCSTR service, LPDWORD a, LPSOCKADDR b, LPDWORD c, LPSOCKADDR d, const struct timeval *e, LPWSAOVERLAPPED f) // lint-ok: R12 mirrors WSAConnectByNameA
{
    if (Enter()) NoteName("WSAConnectByNameA", name, service, RETURN_ADDRESS);
    BOOL result = g_connectByNameA(s, name, service, a, b, c, d, e, f);
    Leave();
    return result;
}

static int WSAAPI HookSendTo(SOCKET s, const char *data, int size, int flags, const struct sockaddr *to, int length)
{
    if (Enter() && to) NoteAddress("sendto", to, length, RETURN_ADDRESS);
    int result = g_sendTo(s, data, size, flags, to, length);
    Leave();
    return result;
}

static int WSAAPI HookWsaSendTo(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD sent, DWORD flags, const struct sockaddr *to, int length, // lint-ok: R12 mirrors WSASendTo
    LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine)
{
    if (Enter() && to) NoteAddress("WSASendTo", to, length, RETURN_ADDRESS);
    int result = g_wsaSendTo(s, buffers, count, sent, flags, to, length, overlapped, routine);
    Leave();
    return result;
}

static int WSAAPI HookWsaSendMsg(SOCKET s, LPWSAMSG message, DWORD flags, LPDWORD sent, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine)
{
    if (Enter() && message && message->name) NoteAddress("WSASendMsg", message->name, message->namelen, RETURN_ADDRESS);
    int result = g_wsaSendMsg(s, message, flags, sent, overlapped, routine);
    Leave();
    return result;
}

static BOOL PASCAL HookConnectEx(SOCKET s, const struct sockaddr *to, int length, PVOID data, DWORD size, LPDWORD sent, LPOVERLAPPED overlapped)
{
    if (Enter()) NoteAddress("ConnectEx", to, length, RETURN_ADDRESS);
    BOOL result = g_connectEx(s, to, length, data, size, sent, overlapped);
    Leave();
    return result;
}

// ConnectEx has no export: a program asks WSAIoctl for its pointer. The pointer is left untouched and its real function
// is hooked in place, so the game holds the genuine pointer while calls still pass through here. This still does not make
// the connectex group usable: the game build checksums ConnectEx's own code, so even an in-place hook trips its fast-fail
// (confirmed in a session); the group is off by default and getting around that check is not in scope.
static bool g_connectExHooked;
static int WSAAPI HookWsaIoctl(SOCKET s, DWORD code, LPVOID in, DWORD inSize, LPVOID out, DWORD outSize, LPDWORD returned, // lint-ok: R12 mirrors WSAIoctl
    LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine)
{
    int result = g_wsaIoctl(s, code, in, inSize, out, outSize, returned, overlapped, routine);
    static const GUID connectEx = WSAID_CONNECTEX;
    if (result == 0 && code == SIO_GET_EXTENSION_FUNCTION_POINTER && in && inSize >= sizeof(GUID) && !memcmp(in, &connectEx, sizeof(GUID)) &&
        out && outSize >= sizeof(void *))
    {
        void *real = *(void **)out;
        EnterCriticalSection(&g_lock);
        if (real && !g_connectExHooked &&
            MH_CreateHook(real, (void *)HookConnectEx, (void **)&g_connectEx) == MH_OK && MH_EnableHook(real) == MH_OK)
            g_connectExHooked = true;
        LeaveCriticalSection(&g_lock);
    }
    return result;
}

static int WSAAPI HookGetAddrInfo(PCSTR name, PCSTR service, const ADDRINFOA *hints, PADDRINFOA *result)
{
    if (Enter()) NoteName("getaddrinfo", name, service, RETURN_ADDRESS);
    int answer = g_getAddrInfo(name, service, hints, result);
    Leave();
    return answer;
}

static int WSAAPI HookGetAddrInfoW(PCWSTR name, PCWSTR service, const ADDRINFOW *hints, PADDRINFOW *result)
{
    if (Enter()) NoteWideName("GetAddrInfoW", name, service, RETURN_ADDRESS);
    int answer = g_getAddrInfoW(name, service, hints, result);
    Leave();
    return answer;
}

static int WSAAPI HookGetAddrInfoExW(PCWSTR name, PCWSTR service, DWORD space, LPGUID provider, const ADDRINFOEXW *hints, PADDRINFOEXW *result, // lint-ok: R12 mirrors GetAddrInfoExW
    struct timeval *timeout, LPOVERLAPPED overlapped, LPLOOKUPSERVICE_COMPLETION_ROUTINE routine, LPHANDLE handle)
{
    if (Enter()) NoteWideName("GetAddrInfoExW", name, service, RETURN_ADDRESS);
    int answer = g_getAddrInfoExW(name, service, space, provider, hints, result, timeout, overlapped, routine, handle);
    Leave();
    return answer;
}

static struct hostent *WSAAPI HookGetHostByName(const char *name)
{
    if (Enter()) NoteName("gethostbyname", name, NULL, RETURN_ADDRESS);
    struct hostent *answer = g_getHostByName(name);
    Leave();
    return answer;
}

/* ---- WinHTTP and WinINet (declared here: their headers do not mix) ---- */
typedef void *(WINAPI *WinHttpConnectFn)(void *, const wchar_t *, WORD, DWORD);
typedef void *(WINAPI *InternetConnectWFn)(void *, const wchar_t *, WORD, const wchar_t *, const wchar_t *, DWORD, DWORD, DWORD_PTR);
typedef void *(WINAPI *InternetConnectAFn)(void *, const char *, WORD, const char *, const char *, DWORD, DWORD, DWORD_PTR);
typedef void *(WINAPI *InternetOpenUrlWFn)(void *, const wchar_t *, const wchar_t *, DWORD, DWORD, DWORD_PTR);
typedef void *(WINAPI *InternetOpenUrlAFn)(void *, const char *, const char *, DWORD, DWORD, DWORD_PTR);
static WinHttpConnectFn g_winHttpConnect;
static InternetConnectWFn g_internetConnectW;
static InternetConnectAFn g_internetConnectA;
static InternetOpenUrlWFn g_internetOpenUrlW;
static InternetOpenUrlAFn g_internetOpenUrlA;

static void NoteServer(const char *api, const char *server, unsigned port, void *returnAddress)
{
    char text[TARGET_TEXT];
    snprintf(text, sizeof text, "%s:%u", server ? server : "(null)", port);
    Note(api, text, NameScope(server), returnAddress);
}

static void *WINAPI HookWinHttpConnect(void *session, const wchar_t *server, WORD port, DWORD reserved)
{
    if (Enter()) { char host[TARGET_TEXT]; Narrow(server, host, sizeof host); NoteServer("WinHttpConnect", server ? host : NULL, port, RETURN_ADDRESS); }
    void *result = g_winHttpConnect(session, server, port, reserved);
    Leave();
    return result;
}

static void *WINAPI HookInternetConnectW(void *internet, const wchar_t *server, WORD port, const wchar_t *user, const wchar_t *password, DWORD service, DWORD flags, DWORD_PTR context) // lint-ok: R12 mirrors InternetConnectW
{
    if (Enter()) { char host[TARGET_TEXT]; Narrow(server, host, sizeof host); NoteServer("InternetConnectW", server ? host : NULL, port, RETURN_ADDRESS); }
    void *result = g_internetConnectW(internet, server, port, user, password, service, flags, context);
    Leave();
    return result;
}

static void *WINAPI HookInternetConnectA(void *internet, const char *server, WORD port, const char *user, const char *password, DWORD service, DWORD flags, DWORD_PTR context) // lint-ok: R12 mirrors InternetConnectA
{
    if (Enter()) NoteServer("InternetConnectA", server, port, RETURN_ADDRESS);
    void *result = g_internetConnectA(internet, server, port, user, password, service, flags, context);
    Leave();
    return result;
}

// A URL's host: what lies between "://" and the next '/', ':' or '?'.
static const char *UrlScope(const char *url)
{
    const char *host = url ? strstr(url, "://") : NULL;
    if (!host) return "name";
    host += 3;
    char name[256];
    size_t n = strcspn(host, "/:?#");
    if (n >= sizeof name) n = sizeof name - 1;
    memcpy(name, host, n);
    name[n] = 0;
    return NameScope(name);
}

static void *WINAPI HookInternetOpenUrlW(void *internet, const wchar_t *url, const wchar_t *headers, DWORD length, DWORD flags, DWORD_PTR context)
{
    if (Enter()) { char text[TARGET_TEXT]; Narrow(url, text, sizeof text); Note("InternetOpenUrlW", text, UrlScope(text), RETURN_ADDRESS); }
    void *result = g_internetOpenUrlW(internet, url, headers, length, flags, context);
    Leave();
    return result;
}

static void *WINAPI HookInternetOpenUrlA(void *internet, const char *url, const char *headers, DWORD length, DWORD flags, DWORD_PTR context)
{
    if (Enter()) Note("InternetOpenUrlA", url ? url : "(null)", UrlScope(url), RETURN_ADDRESS);
    void *result = g_internetOpenUrlA(internet, url, headers, length, flags, context);
    Leave();
    return result;
}

/* ---- programs the game starts (kernelbase: kernel32's and the shell's process creation end there) ---- */
typedef BOOL (WINAPI *CreateProcessWFn)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
typedef BOOL (WINAPI *CreateProcessAFn)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
static CreateProcessWFn g_createProcessW;
static CreateProcessAFn g_createProcessA;

static BOOL WINAPI HookCreateProcessW(LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES a, LPSECURITY_ATTRIBUTES b, BOOL inherit, DWORD flags, // lint-ok: R12 mirrors CreateProcessW
    LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information)
{
    if (Enter())
    {
        char text[TARGET_TEXT];
        Narrow(application ? application : command, text, sizeof text);
        Note("CreateProcessW", text, "program", RETURN_ADDRESS);
    }
    BOOL result = g_createProcessW(application, command, a, b, inherit, flags, environment, directory, startup, information);
    Leave();
    return result;
}

static BOOL WINAPI HookCreateProcessA(LPCSTR application, LPSTR command, LPSECURITY_ATTRIBUTES a, LPSECURITY_ATTRIBUTES b, BOOL inherit, DWORD flags, // lint-ok: R12 mirrors CreateProcessA
    LPVOID environment, LPCSTR directory, LPSTARTUPINFOA startup, LPPROCESS_INFORMATION information)
{
    if (Enter()) Note("CreateProcessA", application ? application : command ? command : "(null)", "program", RETURN_ADDRESS);
    BOOL result = g_createProcessA(application, command, a, b, inherit, flags, environment, directory, startup, information);
    Leave();
    return result;
}

/* ---- set-up ---- */
// Each hook belongs to a group, so a session can turn groups on one at a time (HELLFORGE_NETGATE_HOOKS, a comma list:
// connect, send, dns, http, process; empty or "all" is every group) to find which one a game build cannot abide.
typedef struct { const wchar_t *module; const char *name; const char *group; void *hook; void **original; bool hooked; } Site;
static Site g_sites[] = {
    {L"ws2_32.dll", "connect", "connect", (void *)HookConnect, (void **)&g_connect},
    {L"ws2_32.dll", "WSAConnect", "connect", (void *)HookWsaConnect, (void **)&g_wsaConnect},
    {L"ws2_32.dll", "WSAConnectByNameW", "connect", (void *)HookConnectByNameW, (void **)&g_connectByNameW},
    {L"ws2_32.dll", "WSAConnectByNameA", "connect", (void *)HookConnectByNameA, (void **)&g_connectByNameA},
    {L"ws2_32.dll", "WSAIoctl", "connectex", (void *)HookWsaIoctl, (void **)&g_wsaIoctl},
    {L"ws2_32.dll", "sendto", "send", (void *)HookSendTo, (void **)&g_sendTo},
    {L"ws2_32.dll", "WSASendTo", "send", (void *)HookWsaSendTo, (void **)&g_wsaSendTo},
    {L"ws2_32.dll", "WSASendMsg", "send", (void *)HookWsaSendMsg, (void **)&g_wsaSendMsg},
    {L"ws2_32.dll", "getaddrinfo", "dns", (void *)HookGetAddrInfo, (void **)&g_getAddrInfo},
    {L"ws2_32.dll", "GetAddrInfoW", "dns", (void *)HookGetAddrInfoW, (void **)&g_getAddrInfoW},
    {L"ws2_32.dll", "GetAddrInfoExW", "dns", (void *)HookGetAddrInfoExW, (void **)&g_getAddrInfoExW},
    {L"ws2_32.dll", "gethostbyname", "dns", (void *)HookGetHostByName, (void **)&g_getHostByName},
    {L"winhttp.dll", "WinHttpConnect", "http", (void *)HookWinHttpConnect, (void **)&g_winHttpConnect},
    {L"wininet.dll", "InternetConnectW", "http", (void *)HookInternetConnectW, (void **)&g_internetConnectW},
    {L"wininet.dll", "InternetConnectA", "http", (void *)HookInternetConnectA, (void **)&g_internetConnectA},
    {L"wininet.dll", "InternetOpenUrlW", "http", (void *)HookInternetOpenUrlW, (void **)&g_internetOpenUrlW},
    {L"wininet.dll", "InternetOpenUrlA", "http", (void *)HookInternetOpenUrlA, (void **)&g_internetOpenUrlA},
    {L"kernelbase.dll", "CreateProcessW", "process", (void *)HookCreateProcessW, (void **)&g_createProcessW},
    {L"kernelbase.dll", "CreateProcessA", "process", (void *)HookCreateProcessA, (void **)&g_createProcessA},
};
#define SITE_COUNT (sizeof g_sites / sizeof g_sites[0])

static char g_groups[128]; /* ",connect,send,..,": the enabled groups, or empty for all */

// Whether a hook group is on. Empty selection or "all": every group.
static bool GroupEnabled(const char *group)
{
    if (g_groups[0] == 0) return true;
    char needle[32];
    int n = snprintf(needle, sizeof needle, ",%s,", group);
    return n > 0 && n < (int)sizeof needle && strstr(g_groups, needle) != NULL;
}

// Hooks every site whose module is loaded, group is on and is not yet hooked; returns how many are hooked in all. Not
// called from the loader-notification callback (MinHook suspends threads; doing that under the loader lock can deadlock).
static int HookLoaded(void)
{
    int hooked = 0;
    for (size_t i = 0; i < SITE_COUNT; i++)
    {
        Site *site = &g_sites[i];
        if (!site->hooked && GroupEnabled(site->group))
        {
            HMODULE module = GetModuleHandleW(site->module);
            void *target = module ? (void *)GetProcAddress(module, site->name) : NULL;
            if (target && MH_CreateHook(target, site->hook, site->original) == MH_OK && MH_EnableHook(target) == MH_OK) site->hooked = true;
        }
        hooked += site->hooked;
    }
    return hooked;
}

// A network module loaded later (a test program, or a game build that loads one on demand): hooked as it is mapped,
// before LoadLibrary returns it to anyone. The callback runs on the thread that holds the loader lock, so MinHook's
// thread suspend is of threads merely waiting on that lock, not holding it, and its own GetProcAddress re-enters the
// loader only on this same thread (the lock is recursive); the census confirmed a late-loaded WinHTTP is hooked here.
typedef struct { ULONG flags; const UNICODE_STRING *fullName, *baseName; void *base; ULONG size; } DllLoaded;
typedef VOID (CALLBACK *DllNotification)(ULONG reason, const DllLoaded *data, PVOID context);
typedef LONG (NTAPI *RegisterDllNotification)(ULONG flags, DllNotification callback, PVOID context, PVOID *cookie);
static PVOID g_cookie;

static VOID CALLBACK OnDllLoaded(ULONG reason, const DllLoaded *data, PVOID context)
{
    if (reason != 1 || !data || !data->baseName || !data->baseName->Buffer) return; /* 1: loaded */
    wchar_t name[64];
    size_t n = data->baseName->Length / sizeof(wchar_t);
    if (n >= 64) return;
    memcpy(name, data->baseName->Buffer, n * sizeof(wchar_t));
    name[n] = 0;
    for (size_t i = 0; i < SITE_COUNT; i++)
        if (!_wcsicmp(name, g_sites[i].module))
        {
            int hooked = HookLoaded();
            char line[160];
            int length = snprintf(line, sizeof line, "{\"gate\":\"module loaded\",\"module\":\"%ls\",\"hooked\":%d,\"of\":%d}\n", name, hooked, (int)SITE_COUNT);
            EnterCriticalSection(&g_lock);
            Write(line, length);
            LeaveCriticalSection(&g_lock);
            return;
        }
}

// HELLFORGE_NETGATE_HOOKS: a comma list of the groups to hook (connect, send, dns, http, process), "all" for every
// group, or unset for the default. The default leaves out connect and connectex: this game build's protection
// checksums its own connect and ConnectEx, so hooking either trips a fast-fail (connectex) or blocks sign-in
// (connect) ~24 s in; the other groups are fine. Those two are kept only for a deliberate test (and a later re-test
// on a new build); getting around the check is not in scope. Kept as ",a,b," so one group is matched whole; empty
// g_groups means every group (used by "all").
static void ReadGroups(void)
{
    wchar_t hooks[96];
    DWORD hooksLength = GetEnvironmentVariableW(L"HELLFORGE_NETGATE_HOOKS", hooks, 96);
    if (hooksLength == 0 || hooksLength >= 96)
        snprintf(g_groups, sizeof g_groups, ",send,dns,http,process,");
    else if (_wcsicmp(hooks, L"all") != 0)
    {
        char narrow[96];
        WideCharToMultiByte(CP_UTF8, 0, hooks, -1, narrow, sizeof narrow, NULL, NULL);
        snprintf(g_groups, sizeof g_groups, ",%s,", narrow);
        for (char *c = g_groups; *c; c++) if (*c == ' ') *c = ',';
    }
}

// The census's first line: the groups, how many sites are hooked, and the ones of an enabled group that are not.
static void WriteStart(int hooked, bool watching)
{
    char line[1024];
    int length = snprintf(line, sizeof line, "{\"gate\":\"census\",\"process\":%lu,\"groups\":\"%s\",\"hooked\":%d,\"of\":%d,\"watchingLoads\":%s,\"missing\":[",
        GetCurrentProcessId(), g_groups[0] ? g_groups : ",all,", hooked, (int)SITE_COUNT, watching ? "true" : "false");
    bool first = true;
    for (size_t i = 0; i < SITE_COUNT && length < (int)sizeof line - 80; i++)
        if (!g_sites[i].hooked && GroupEnabled(g_sites[i].group))
        {
            length += snprintf(line + length, sizeof line - length, "%s\"%ls!%s\"", first ? "" : ",", g_sites[i].module, g_sites[i].name);
            first = false;
        }
    length += snprintf(line + length, sizeof line - length, "]}\n");
    Write(line, length);
}

void NetGateStart(void)
{
    wchar_t mode[32], path[MAX_PATH];
    DWORD modeLength = GetEnvironmentVariableW(L"HELLFORGE_NETGATE", mode, 32);
    DWORD pathLength = GetEnvironmentVariableW(L"HELLFORGE_NETGATE_LOG", path, MAX_PATH);
    if (modeLength == 0 || modeLength >= 32 || wcscmp(mode, L"census") != 0 || pathLength == 0 || pathLength >= MAX_PATH) return;
    ReadGroups();
    g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE) return;
    InitializeCriticalSection(&g_lock);
    g_depth = TlsAlloc();
    bool ready = g_depth != TLS_OUT_OF_INDEXES && MH_Initialize() == MH_OK;
    int hooked = ready ? HookLoaded() : 0;
    RegisterDllNotification registerNotification = (RegisterDllNotification)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    bool watching = ready && registerNotification && registerNotification(0, OnDllLoaded, NULL, &g_cookie) == 0;
    g_on = ready;
    WriteStart(hooked, watching);
}

