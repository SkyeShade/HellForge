/* HellForge mod loader (docs/modding/README.md, docs/internals.md): loaded into the copied game by the steam_api64.dll stand-in when a session has code
 * mods. It reads the session's plan (%HELLFORGE_MODLOADER_PLAN%, written by HellForge.Cli mods-stage for the session runner),
 * loads each enabled code mod and calls its hellforge_mod_init with the SDK (native/mod-sdk/hellforge_mod.h), then watches the game
 * start and delivers the SDK's events. Hooks use MinHook 1.3.4 (BSD-2, MinHook-LICENSE.txt).
 *
 * Plan lines, tab-separated:
 *   build <id>                              logs <folder>
 *   game_pin <module> <rva hex> <hex bytes> [<hex bytes> ...]       (any of them means "decoded"; the build's own by default)
 *   network_config <module> <rva hex>       ([[module+rva]] is the engine's network configuration)
 *   entity_region <size> <map header hex>
 *   component <name> <offset> <header hex> <index> <indices> <records> <record offset> <stride>
 *   mod <id> <folder> <dll> */
#define WIN32_LEAN_AND_MEAN
#define HF_MOD_LOADER
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <tlhelp32.h>
#include "hellforge_mod.h"
#include "MinHook.h"
#include "hde/hde64.h" /* MinHook's instruction decoder: FarHook */
#include "../common/hf_privacy.h"

#define MAX_MODS 192 /* code and stats mods from the plan, then Lua mods (lua_bridge.c) */
#define MAX_COMPONENTS 64
#define MAX_CALLBACKS 256
#define MAX_PINS 4

struct HfMod
{
    const HfApi* api; /* first, for HF_API(mod) (SDK 6) */
    char id[80];
    char folder[MAX_PATH * 3];
    wchar_t dll[MAX_PATH];
    HMODULE module;
    HANDLE log;
    int loaded;
    char last[512]; /* its last log line (last_log: why a call was refused) */
    int lua;        /* a Lua mod (lua.<name>), registered through the Lua bridge while the game runs (lua_bridge.c) */
    char name[128]; /* a Lua mod's name as its runtime gave it (SDK 10, mods) */
};

typedef struct Component
{
    char name[64];
    uint64_t offset;
    uint8_t header[28];
    uint32_t index, indices, records, recordOffset, stride;
} Component;

typedef struct Callback
{
    HfMod* mod;
    HfEvent event;
    HfEventCallback function;
    void* user;
} Callback;

static char g_build[32] = "unknown";
static wchar_t g_logs[MAX_PATH];
static HANDLE g_log = INVALID_HANDLE_VALUE;
static char g_pinModule[64];
static uint32_t g_pinRva;
static uint8_t g_pins[MAX_PINS][32];
static size_t g_pinLength[MAX_PINS];
static int g_pinCount;
static char g_networkModule[64];
static uint32_t g_networkRva;
static uint64_t g_regionSize;
static uint8_t g_mapHeader[28];
static Component g_components[MAX_COMPONENTS];
static int g_componentCount;
static HfMod g_mods[MAX_MODS];
static int g_modCount;
/* Code mods are set up (hellforge_mod_init) in the order of their ids, the same on every player's machine, so what they
 * register is numbered alike in co-op (stratagem types, abilities, behaviours, projectiles, damage types) whatever each
 * player's load order (2026-10-18: friends with the same mods in another order). g_mods stays in load order: the
 * deferred edits queued while they set up apply in it (fields.c), so the top mod still wins a conflict. */
static int g_modsSettingUp;
static int ModLoadIndex(const HfMod* mod) { return mod >= g_mods && mod < g_mods + g_modCount ? (int)(mod - g_mods) : MAX_MODS; }
/* The code mods' set-up order: indices into g_mods by mod id. */
static void SetupOrder(int order[MAX_MODS])
{
    for (int i = 0; i < g_modCount; i++) order[i] = i;
    for (int i = 1; i < g_modCount; i++)
        for (int j = i; j > 0 && strcmp(g_mods[order[j - 1]].id, g_mods[order[j]].id) > 0; j--)
        {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
}
static Callback g_callbacks[MAX_CALLBACKS];
static int g_callbackCount;
static volatile LONG g_fired[5];
static uintptr_t g_region;
static SRWLOCK g_lock = SRWLOCK_INIT;
static int g_hooksReady;
static HfApi g_api;

/* ---- logging ---------------------------------------------------------------------------------------------------- */

static void WriteLog(HANDLE file, const char* prefix, const char* format, va_list arguments)
{
    if (file == INVALID_HANDLE_VALUE) return;
    char line[1024];
    SYSTEMTIME now;
    GetSystemTime(&now);
    int length = snprintf(line, sizeof line, "%02u:%02u:%02u.%03u %s", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, prefix);
    int more = vsnprintf(line + length, sizeof line - (size_t)length - 2, format, arguments);
    if (more < 0) more = 0;
    length += more > (int)sizeof line - length - 2 ? (int)sizeof line - length - 2 : more;
    length = (int)HfPrivacyScrub(line, (size_t)length, sizeof line - 1);
    line[length++] = '\n';
    DWORD written;
    WriteFile(file, line, (DWORD)length, &written, NULL);
}

static void Note(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    WriteLog(g_log, "", format, arguments);
    va_end(arguments);
}

static HANDLE OpenLog(const wchar_t* name)
{
    wchar_t path[MAX_PATH];
    if (!g_logs[0] || swprintf(path, MAX_PATH, L"%ls\\%ls", g_logs, name) < 0) return INVALID_HANDLE_VALUE;
    return CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* While the live editor's request runs, its mod's last message is kept: the reason a refusal gives (live.c). */
static HfMod* g_reasonMod;
static char g_liveReason[256];

static void ApiLog(HfMod* mod, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    WriteLog(mod != NULL ? mod->log : g_log, "", format, arguments);
    va_end(arguments);
    if (mod != NULL)
    {
        va_start(arguments, format);
        vsnprintf(mod->last, sizeof mod->last, format, arguments);
        va_end(arguments);
        HfPrivacyScrubString(mod->last, sizeof mod->last);
    }
    if (mod != NULL && mod == g_reasonMod)
    {
        va_start(arguments, format);
        vsnprintf(g_liveReason, sizeof g_liveReason, format, arguments);
        va_end(arguments);
        HfPrivacyScrubString(g_liveReason, sizeof g_liveReason);
    }
}

/* ---- memory ----------------------------------------------------------------------------------------------------- */

static int SafeRead(uintptr_t address, void* buffer, size_t size)
{
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, buffer, size, &read) && read == size ? 0 : 1;
}

/* Every range of game.dll HellForge has written (the Lua bridge's hellforge_lua_rewrites): its rva and size, a range
 * that touches the last one joined to it. A Lua runtime proving game code against reviewed bytes asks for them. */
#define REWRITE_MAX 16384
static uint32_t g_rewriteRva[REWRITE_MAX], g_rewriteSize[REWRITE_MAX];
static int g_rewriteCount;
static SRWLOCK g_rewriteLock = SRWLOCK_INIT;
static void NoteRewrite(uintptr_t address, size_t size)
{
    HMODULE game = GetModuleHandleA("game.dll");
    if (game == NULL || size == 0) return;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)game + ((IMAGE_DOS_HEADER*)game)->e_lfanew);
    uintptr_t base = (uintptr_t)game;
    if (address < base || address + size > base + nt->OptionalHeader.SizeOfImage) return;
    uint32_t rva = (uint32_t)(address - base);
    AcquireSRWLockExclusive(&g_rewriteLock);
    int last = g_rewriteCount - 1;
    if (last >= 0 && rva <= g_rewriteRva[last] + g_rewriteSize[last] && rva + size >= g_rewriteRva[last])
    {
        uint32_t start = rva < g_rewriteRva[last] ? rva : g_rewriteRva[last];
        uint32_t end = rva + (uint32_t)size > g_rewriteRva[last] + g_rewriteSize[last] ? rva + (uint32_t)size : g_rewriteRva[last] + g_rewriteSize[last];
        g_rewriteRva[last] = start;
        g_rewriteSize[last] = end - start;
    }
    else if (g_rewriteCount < REWRITE_MAX) { g_rewriteRva[g_rewriteCount] = rva; g_rewriteSize[g_rewriteCount++] = (uint32_t)size; }
    ReleaseSRWLockExclusive(&g_rewriteLock);
}

static int ApiWrite(uintptr_t address, const void* data, size_t size)
{
    DWORD previous;
    if (!VirtualProtect((void*)address, size, PAGE_EXECUTE_READWRITE, &previous)) return 1;
    memcpy((void*)address, data, size);
    VirtualProtect((void*)address, size, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), (void*)address, size);
    NoteRewrite(address, size);
    return 0;
}

static int ApiWriteU32If(HfMod* mod, uintptr_t address, uint32_t expect, uint32_t value)
{
    uint32_t current;
    if (address == 0 || SafeRead(address, &current, 4)) return 1;
    if (current == value) return 0;
    if (current != expect)
    {
        ApiLog(mod, "left alone: 0x%llx holds %u, expected %u", (unsigned long long)address, current, expect);
        return 1;
    }
    if (ApiWrite(address, &value, 4)) return 1;
    ApiLog(mod, "0x%llx: %u -> %u", (unsigned long long)address, expect, value);
    return 0;
}

static uintptr_t ApiModuleBase(const char* module, size_t* size)
{
    if (size != NULL) *size = 0;
    HMODULE handle = module != NULL ? GetModuleHandleA(module) : NULL;
    if (handle == NULL) return 0;
    if (size != NULL)
    {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)handle;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)handle + dos->e_lfanew);
        *size = nt->OptionalHeader.SizeOfImage;
    }
    return (uintptr_t)handle;
}

static int Hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* An IDA-style pattern ("48 8B ?? 24 ?? E8"): its bytes and which of them count (0 for ? and ??). The number of bytes,
 * or 0 for an empty or malformed pattern. */
static size_t ParsePattern(const char* pattern, uint8_t bytes[256], uint8_t mask[256])
{
    size_t count = 0;
    for (const char* c = pattern; c != NULL && *c && count < 256;)
    {
        if (*c == ' ') { c++; continue; }
        if (c[0] == '?')
        {
            mask[count] = 0;
            bytes[count++] = 0;
            c += c[1] == '?' ? 2 : 1;
            continue;
        }
        int high = Hex(c[0]), low = c[1] ? Hex(c[1]) : -1;
        if (high < 0 || low < 0) return 0;
        mask[count] = 1;
        bytes[count++] = (uint8_t)(high << 4 | low);
        c += 2;
    }
    return count;
}

static uintptr_t ApiFindPattern(const char* module, const char* pattern)
{
    uint8_t bytes[256], mask[256];
    size_t count = ParsePattern(pattern, bytes, mask);
    size_t size;
    uintptr_t base = ApiModuleBase(module, &size);
    if (base == 0 || count == 0) return 0;
    uintptr_t at = base, end = base + size;
    while (at < end)
    {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void*)at, &info, sizeof info)) break;
        uintptr_t regionEnd = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (regionEnd > end) regionEnd = end;
        DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
        if (info.State == MEM_COMMIT && (info.Protect & readable) && !(info.Protect & PAGE_GUARD))
            for (uintptr_t p = at; p + count <= regionEnd; p++)
            {
                size_t i = 0;
                while (i < count && (!mask[i] || ((uint8_t*)p)[i] == bytes[i])) i++;
                if (i == count) return p;
            }
        at = regionEnd;
    }
    return 0;
}

/* game.dll's decoded code pages report PAGE_READWRITE, so MinHook refuses them as not executable (error 7). The page
 * holding a hook target is made executable (it runs as code already), and MinHook then hooks it like any other. */
static void MakeExecutable(uintptr_t target)
{
    MEMORY_BASIC_INFORMATION page;
    if (!VirtualQuery((void*)target, &page, sizeof page) || page.State != MEM_COMMIT) return;
    DWORD protect = page.Protect & 0xFF, previous;
    if (protect == PAGE_READWRITE || protect == PAGE_WRITECOPY) VirtualProtect((void*)target, 16, PAGE_EXECUTE_READWRITE, &previous);
    else if (protect == PAGE_READONLY) VirtualProtect((void*)target, 16, PAGE_EXECUTE_READ, &previous);
}

/* A pointer-sized swap that another thread may read at the same time: atomic where it is aligned. */
static int SwapPointer(uintptr_t at, void* value, void** old)
{
    DWORD protect;
    if (!VirtualProtect((void*)at, 8, PAGE_EXECUTE_READWRITE, &protect)) return 1;
    /* the old pointer is handed back before the new one is in place: a call through it may come at once */
    memcpy(old, (void*)at, 8);
    if ((at & 7) == 0) InterlockedExchangePointer((void* volatile*)at, value);
    else memcpy((void*)at, &value, 8);
    VirtualProtect((void*)at, 8, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 16);
    NoteRewrite(at, 8);
    return 0;
}

/* Every other thread of the process suspended (0 none could be), to patch code they may run; FarResume undoes it. */
#define FAR_THREADS 1024
static HANDLE g_farThreads[FAR_THREADS];
static int g_farThreadCount;
static void FarSuspend(void)
{
    g_farThreadCount = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry = {sizeof entry};
    DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more && g_farThreadCount < FAR_THREADS; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
        if (thread == NULL) continue;
        if (SuspendThread(thread) == (DWORD)-1) { CloseHandle(thread); continue; }
        g_farThreads[g_farThreadCount++] = thread;
    }
    CloseHandle(snapshot);
}
static int FarAnyIn(uintptr_t from, size_t size)
{
    for (int i = 0; i < g_farThreadCount; i++)
    {
        CONTEXT context = {0};
        context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_farThreads[i], &context) && context.Rip >= from && context.Rip < from + size) return 1;
    }
    return 0;
}
static void FarResume(void)
{
    for (int i = 0; i < g_farThreadCount; i++) { ResumeThread(g_farThreads[i]); CloseHandle(g_farThreads[i]); }
    g_farThreadCount = 0;
}

/* Appends an absolute jump (jmp [rip+0]; target), 14 bytes. */
static size_t FarJump(uint8_t* at, uint64_t target)
{
    static const uint8_t jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(at, jump, 6);
    memcpy(at + 6, &target, 8);
    return 14;
}

/* A hook where MinHook finds no memory within 2 GB of the target for its trampoline: code in allocated memory amid
 * the GPU driver's reservations, which is where overlays (RivaTuner, Discord, Overwolf...) put the stubs they send a
 * swap chain's methods through. A stub is taken over in place, as overlays chain: an absolute jump through a pointer
 * (jmp [rip+disp]; the common "FF 25 00000000 <address>") or mov rax, imm64; jmp rax gets the detour as its address,
 * the old address becoming the original (the call runs through the detour, then the overlay). A relative jump first
 * is followed and its destination hooked. Other code gets a 14-byte absolute jump and a trampoline anywhere, when the
 * instructions it moves can be moved (a relative jump or call is rewritten absolute; a conditional branch or another
 * RIP-relative operand refuses it); every other thread is suspended meanwhile and none may be in those bytes.
 * 0 when hooked, *how saying which. */
static int FarHook(uintptr_t target, void* detour, void** original, const char** how)
{
    uint8_t code[32];
    if (SafeRead(target, code, sizeof code)) { *how = "unreadable"; return 1; }
    if (code[0] == 0xFF && code[1] == 0x25) /* jmp [rip+disp32] */
    {
        int32_t disp; memcpy(&disp, code + 2, 4);
        *how = disp == 0 ? "an overlay's jump stub, its address swapped" : "a jump through a pointer, the pointer swapped";
        return SwapPointer(target + 6 + (intptr_t)disp, detour, original);
    }
    if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) /* mov rax, imm64; jmp rax */
    {
        *how = "an overlay's jump stub (mov rax), its address swapped";
        return SwapPointer(target + 2, detour, original);
    }
    if (code[0] == 0xE9 || code[0] == 0xEB) /* a relative jump: hook where it goes */
    {
        int32_t rel = code[0] == 0xE9 ? 0 : (int8_t)code[1];
        if (code[0] == 0xE9) memcpy(&rel, code + 1, 4);
        uintptr_t next = target + (code[0] == 0xE9 ? 5 : 2) + (intptr_t)rel;
        MakeExecutable(next);
        MH_STATUS status = MH_CreateHook((void*)next, detour, original);
        if (status == MH_OK) status = MH_EnableHook((void*)next);
        *how = status == MH_OK ? "a jump followed, its destination hooked" : "a jump followed, its destination not hookable either";
        return status == MH_OK ? 0 : 1;
    }
    uint8_t* trampoline = (uint8_t*)VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (trampoline == NULL) { *how = "no memory for a trampoline"; return 1; }
    size_t moved = 0, out = 0;
    while (moved < 14)
    {
        hde64s hs;
        unsigned length = hde64_disasm(code + moved, &hs);
        if ((hs.flags & F_ERROR) || length == 0 || moved + length > sizeof code - 8) { *how = "code it cannot read"; goto refuse; }
        uintptr_t at = target + moved, next = at + length;
        if (hs.opcode == 0xE8) /* call rel32: call [rip+2]; jmp +8; address */
        {
            static const uint8_t call[8] = {0xFF, 0x15, 0x02, 0, 0, 0, 0xEB, 0x08};
            uint64_t dest = next + (int64_t)(int32_t)hs.imm.imm32;
            memcpy(trampoline + out, call, 8); memcpy(trampoline + out + 8, &dest, 8); out += 16;
        }
        else if (hs.opcode == 0xE9 || hs.opcode == 0xEB) { *how = "code that jumps away in its first bytes"; goto refuse; }
        else if ((hs.opcode >= 0x70 && hs.opcode <= 0x7F) || (hs.opcode >= 0xE0 && hs.opcode <= 0xE3) ||
                 (hs.opcode == 0x0F && hs.opcode2 >= 0x80 && hs.opcode2 <= 0x8F)) { *how = "a conditional branch in its first bytes"; goto refuse; }
        else if ((hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5) { *how = "a RIP-relative operand in its first bytes"; goto refuse; }
        else { memcpy(trampoline + out, code + moved, length); out += length; }
        moved += length;
    }
    out += FarJump(trampoline + out, target + moved);
    uint8_t patch[32];
    size_t used = FarJump(patch, (uint64_t)(uintptr_t)detour);
    memset(patch + used, 0x90, moved - used); /* the rest of the moved instructions: nops */
    *original = trampoline; /* before the jump is written: a call may come through it at once */
    FarSuspend();
    if (FarAnyIn(target, moved)) { FarResume(); *how = "a thread is running the code it would move"; goto refuse; }
    DWORD protect;
    if (!VirtualProtect((void*)target, moved, PAGE_EXECUTE_READWRITE, &protect)) { FarResume(); *how = "the code cannot be made writable"; goto refuse; }
    memcpy((void*)target, patch, moved);
    VirtualProtect((void*)target, moved, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, moved);
    NoteRewrite(target, moved);
    FarResume();
    *how = "a far jump and a trampoline";
    return 0;
refuse:
    VirtualFree(trampoline, 0, MEM_RELEASE);
    return 1;
}

#include "hooks.c" /* every hook, a mod's or HellForge's own: one chain per function (SDK 13) */

/* ---- network field types ---------------------------------------------------------------------------------------- */

static uint64_t Murmur64A(const void* key, size_t length)
{
    const uint64_t m = 0xC6A4A7935BD1E995ull;
    uint64_t h = length * m;
    const uint8_t* data = (const uint8_t*)key;
    size_t whole = length / 8 * 8;
    for (size_t i = 0; i < whole; i += 8)
    {
        uint64_t k;
        memcpy(&k, data + i, 8);
        k *= m;
        k ^= k >> 47;
        h = (h ^ (k * m)) * m;
    }
    if (whole != length)
    {
        uint64_t tail = 0;
        for (size_t i = length; i > whole; i--) tail = (tail << 8) | data[i - 1];
        h = (h ^ tail) * m;
    }
    h ^= h >> 47;
    h *= m;
    return h ^ (h >> 47);
}

/* The engine's field type table: [[module+rva]] -> count at +0x10, rows (0x18 bytes) at +0x18. */
static int FieldTypes(uintptr_t* rows, uint32_t* count)
{
    uintptr_t base = ApiModuleBase(g_networkModule, NULL), holder = 0, config = 0;
    if (base == 0 || SafeRead(base + g_networkRva, &holder, 8) || holder == 0 || SafeRead(holder, &config, 8) || config == 0) return 1;
    if (SafeRead(config + 0x10, count, 4) || SafeRead(config + 0x18, rows, 8) || *count == 0 || *count > 8192 || *rows == 0) return 1;
    return 0;
}

static uintptr_t FindFieldType(const char* name)
{
    uintptr_t rows;
    uint32_t count, hash = (uint32_t)(Murmur64A(name, strlen(name)) >> 32), found = 0;
    uintptr_t match = 0;
    if (FieldTypes(&rows, &count)) return 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t rowHash;
        if (SafeRead(rows + (uintptr_t)i * 0x18, &rowHash, 4)) return 0;
        if (rowHash == hash) { match = rows + (uintptr_t)i * 0x18; found++; }
    }
    return found == 1 ? match : 0;
}

static int ApiNetworkFieldType(const char* name, HfFieldType* type)
{
    uintptr_t row = name != NULL && type != NULL ? FindFieldType(name) : 0;
    uint8_t bytes[0x18];
    if (row == 0 || SafeRead(row, bytes, sizeof bytes)) return 1;
    type->kind = bytes[12];
    type->bits = bytes[13];
    memcpy(&type->min, bytes + 16, 4);
    return 0;
}

static int ApiSetNetworkFieldBits(HfMod* mod, const char* name, HfFieldType expect, int32_t bits)
{
    HfFieldType current;
    if (ApiNetworkFieldType(name, &current)) { ApiLog(mod, "network field type %s: not found", name); return 1; }
    if (expect.kind != 1 || bits <= expect.bits || bits > 31 || current.kind != expect.kind || current.min != expect.min)
    { ApiLog(mod, "network field type %s left alone: only an integer type can be widened, to at most 31 bits", name); return 1; }
    if (current.bits == bits) return 0;
    if (current.bits != expect.bits) { ApiLog(mod, "network field type %s left alone: %d bits, expected %d", name, current.bits, expect.bits); return 1; }
    uint8_t value = (uint8_t)bits;
    if (ApiWrite(FindFieldType(name) + 13, &value, 1)) return 1;
    ApiLog(mod, "network field type %s: %d -> %d bits", name, expect.bits, bits);
    return 0;
}

/* ---- entity component records ----------------------------------------------------------------------------------- */

/* One private, committed allocation of exactly the profile's size starting with its map header; 0 if absent or not unique. */
static uintptr_t FindEntityRegion(void)
{
    if (g_regionSize == 0) return 0;
    uintptr_t found = 0, at = 0x10000;
    MEMORY_BASIC_INFORMATION info;
    while (VirtualQuery((void*)at, &info, sizeof info))
    {
        uintptr_t next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && info.BaseAddress == info.AllocationBase &&
            (info.Protect == PAGE_READONLY || info.Protect == PAGE_READWRITE) && info.RegionSize == g_regionSize)
        {
            uint8_t header[28];
            if (!SafeRead((uintptr_t)info.BaseAddress, header, 28) && memcmp(header, g_mapHeader, 28) == 0)
            {
                if (found) return 0;
                found = (uintptr_t)info.BaseAddress;
            }
        }
        if (next <= at) break;
        at = next;
    }
    return found;
}

static uintptr_t RegisteredRecord(const Component* c, uint64_t resource, uint32_t* stride); /* registry.c */
static uintptr_t CurrentTableRecord(const Component* c, uint64_t resource, uint32_t* stride, int shared, int* used); /* registry.c */
static int CopiesBase(uint64_t entity); /* registry.c */

/* An entity's record of a component: its own (a mod's entity), or the one vanilla record naming it; a record other
 * entities share too only with `shared` (for reading). */
static uintptr_t ComponentRecordOf(const char* name, uint64_t resource, uint32_t* stride, int shared)
{
    if (stride != NULL) *stride = 0;
    uintptr_t region = g_region;
    const Component* c = NULL;
    for (int i = 0; i < g_componentCount && name != NULL; i++)
        if (strcmp(g_components[i].name, name) == 0) c = &g_components[i];
    if (region == 0 || c == NULL) return 0;
    uintptr_t own = RegisteredRecord(c, resource, stride); /* a weapon a mod added: its own record, in HellForge's table */
    if (own != 0) return own;
    /* The table the game reads now: once the registry has copied a table, the loaded region's is stale. */
    int used = 0;
    uintptr_t current = CurrentTableRecord(c, resource, stride, shared, &used);
    if (used) return current;
    if (c->recordOffset != c->indices * 16) return 0;
    uint8_t head[32];
    if (SafeRead(region + c->offset - 4, head, 32)) return 0;
    uint32_t index, extent;
    memcpy(&index, head, 4);
    memcpy(&extent, head + 20, 4);
    if (index != c->index || memcmp(head + 4, c->header, 28) != 0 || (uint64_t)c->recordOffset + (uint64_t)c->records * c->stride > extent) return 0;
    int64_t selected = -1;
    int owners = 0;
    for (uint32_t row = 0; row < c->indices; row++)
    {
        uint8_t entry[16];
        if (SafeRead(region + c->offset + 28 + (uintptr_t)row * 16, entry, 16)) return 0;
        uint64_t id;
        uint32_t record, flags;
        memcpy(&id, entry, 8);
        memcpy(&record, entry + 8, 4);
        memcpy(&flags, entry + 12, 4);
        if (record >= c->records || flags != 0) return 0;
        if (id == resource) { owners++; selected = record; }
    }
    if (owners != 1) return 0;
    for (uint32_t row = 0, sharers = 0; row < c->indices; row++)
    {
        uint8_t entry[16];
        SafeRead(region + c->offset + 28 + (uintptr_t)row * 16, entry, 16);
        uint64_t id;
        uint32_t record;
        memcpy(&id, entry, 8);
        memcpy(&record, entry + 8, 4);
        if (id != 0 && id != resource && record == (uint32_t)selected && !CopiesBase(id) && ++sharers > 0 && !shared) return 0;
    }
    if (stride != NULL) *stride = c->stride;
    return region + c->offset + 28 + c->recordOffset + (uintptr_t)selected * c->stride;
}

static uintptr_t ApiComponentRecord(const char* name, uint64_t resource, uint32_t* stride)
{
    return ComponentRecordOf(name, resource, stride, 0);
}

/* A content name's identity (SDK 6): a short name ("pelican_gatling", no '/' or ':') becomes "<mod id>:<name>"; a full
 * name ("my_name/my_mod/pelican") stays as it is, so content named before keeps its ids. */
static const char* Identity(HfMod* mod, const char* name, char* buffer, size_t size)
{
    if (mod == NULL || name == NULL || name[0] == 0 || strchr(name, '/') != NULL || strchr(name, ':') != NULL) return name;
    snprintf(buffer, size, "%s:%s", mod->id, name);
    return buffer;
}

/* ---- events ----------------------------------------------------------------------------------------------------- */

static int StatsHasComponents(void);
static int StatsComponentsPending(void);
static void StatsApplyComponents(uint8_t* state);
static void StatsBeginComponents(void);
static void StatsFinishComponents(uint8_t* state);
/* Co-op diagnostics (coop_diag.c, after live.c): a line in coop-diagnostics.log, and a missile to follow after launch. */
static void CoopDiag(const char* format, ...);
static void CoopDiagMissile(uint32_t missile, const char* how);
/* The network apply switch extended to the added network types (netapply.c, after live.c: the co-op role). */
static int g_netSwitchState;
static int InstallNetSwitch(uintptr_t game, int pinsReady);
/* A friend's dead connection closed once the game session lets them go (netreap.c): rejoining after a crash. */
static int g_reapState;
static int InstallReaper(int pinsReady);
/* SDK 7 events (events.c, after abilities.c: the silo countdowns listen for a mission's end). */
static int ApiSubscribe(HfMod* mod, uint32_t kind, HfListener listener, void* user, int32_t priority);
#include "registry.c"
#include "damage.c"
#include "projectiles.c"
#include "stratagems.c"
#include "difficulty.c"
#include "director.c" /* after difficulty.c: its rosters and the difficulty copies */
#include "behaviours.c"
#include "packages.c"
#include "loadout_preview.c"
#include "abilities.c"
#include "events.c"
#include "stats.c"
#include "fields.c" /* after stats.c: it reports conflicts with stats edits */
#include "refs.c"
#include "passives.c"
#include "perks.c"
#include "actions.c" /* after refs.c (packages) and events.c (the readers) */
#include "shots.c" /* after actions.c: the players (event_polls.c), RefCheck (refs.c), the field records (fields.c) */
#include "rounds.c" /* after shots.c: its players and wielders; the projectile and damage lists (projectiles.c, damage.c) */

static void Deliver(HfEvent event)
{
    for (int i = 0; i < g_callbackCount; i++)
        if (g_callbacks[i].event == event && g_callbacks[i].mod->loaded)
        {
            Note("event %d -> %s", (int)event, g_callbacks[i].mod->id);
            g_callbacks[i].function(g_callbacks[i].mod, event, g_callbacks[i].user);
        }
}

static void Fire(HfEvent event)
{
    if (event == HF_EVENT_ENTITIES_READY) StratagemPayloadsCheck(); /* nothing calls in an entity that is not there */
    if (event == HF_EVENT_ENTITIES_READY) ApplyFieldEdits(); /* SDK 6 deferred edits, before any mod hears of it */
    InterlockedExchange(&g_fired[event], 1);
    Deliver(event);
}

static uint64_t ApiResourceHash(const char* name) { return name != NULL ? Murmur64A(name, strlen(name)) : 0; }

static int ApiOn(HfMod* mod, HfEvent event, HfEventCallback callback, void* user)
{
    if (mod == NULL || callback == NULL || event < HF_EVENT_GAME_LOADED || event > HF_EVENT_PROJECTILES_READY || g_callbackCount >= MAX_CALLBACKS) return 1;
    g_callbacks[g_callbackCount++] = (Callback){mod, event, callback, user};
    if (g_fired[event]) callback(mod, event, user);
    return 0;
}

static const char* ApiBuildId(void) { return g_build; }
static const char* ApiModId(HfMod* mod) { return mod != NULL ? mod->id : ""; }
static const char* ApiModFolder(HfMod* mod) { return mod != NULL ? mod->folder : ""; }

/* SDK 6: the mod's last log line, and the session's logs folder (UTF-8; modloader.log and every mod-<id>.log). */
static const char* ApiLastLog(HfMod* mod) { return mod != NULL ? mod->last : ""; }
static const char* ApiLogsFolder(HfMod* mod)
{
    static char folder[MAX_PATH * 3];
    if (!folder[0] && g_logs[0]) WideCharToMultiByte(CP_UTF8, 0, g_logs, -1, folder, sizeof folder, NULL, NULL);
    return folder;
}

/* SDK 6, get_stat: a stats field the snapshot found, read now; its base is the value without the Local Stats mod's edit
 * (another mod's, else the game's), as get_live's: a live write's (set_stat, the pipe) value from before it, live.c. */
static int LiveExtraBefore(uintptr_t address, uint32_t* before);
static const char* LiveExtraHolder(uintptr_t address, uint32_t* value);
static int ApiGetStat(HfMod* mod, const HfStat* stat, HfLiveValue* out)
{
    if (out == NULL) return 0;
    memset(out, 0, sizeof *out);
    if (mod == NULL || stat == NULL || stat->kind == NULL || stat->component == NULL) return 0;
    const char* component = stat->component[0] ? stat->component : "-"; /* the plan's "-": no component */
    uint32_t type = stat->type == HF_TYPE_F32 || stat->type == HF_TYPE_I32 || stat->type == HF_TYPE_U8 ? stat->type : HF_TYPE_U32;
    int result = 0;
    uintptr_t address = 0;
    uint32_t size = 4;
    AcquireSRWLockShared(&g_statsLock);
    for (int i = 0; i < g_statsProbeCount && !result; i++)
    {
        const StatEdit* e = &g_statsProbes[i];
        if (e->offset != stat->offset || e->resource != stat->resource || e->record != stat->record || !e->address ||
            strcmp(e->kind, stat->kind) || strcmp(e->component, component)) continue;
        uint32_t now = 0, base;
        if (SafeRead(e->address, &now, StatSize(e))) break;
        base = now;
        LiveExtraBefore(e->address, &base);
        for (int j = 0; j < g_statCount; j++)
        {
            const StatEdit* s = &g_stats[j];
            if (!s->applied || s->address != e->address || s->mod == NULL) continue;
            if (IsLocalStats(s->mod)) base = s->original;
            else out->mod = s->mod->id;
        }
        /* a Lua or code mod's live value of an entity delta or status multiplier is another mod's value too */
        uint32_t held;
        const char* holder = LiveExtraHolder(e->address, &held);
        if (holder != NULL) { out->mod = holder; base = held; }
        ValueOf(type, now, &out->now);
        ValueOf(type, base, &out->base);
        address = e->address;
        size = StatSize(e);
        result = 1;
    }
    ReleaseSRWLockShared(&g_statsLock);
    /* a Lua or code mod's live edit there is another mod's value too */
    uint32_t value = 0;
    const char* holder = result ? LiveHolderAt(address, size, &value) : NULL;
    if (holder != NULL) { out->mod = holder; ValueOf(type, value, &out->base); }
    return result;
}

static int GameLoaded(void)
{
    uintptr_t base = ApiModuleBase(g_pinModule, NULL);
    if (base == 0) return 0;
    if (g_pinCount == 0) return 1;
    for (int i = 0; i < g_pinCount; i++)
    {
        uint8_t bytes[32];
        if (!SafeRead(base + g_pinRva, bytes, g_pinLength[i]) && memcmp(bytes, g_pins[i], g_pinLength[i]) == 0) return 1;
    }
    return 0;
}

static DWORD WINAPI Watch(void* parameter)
{
    ULONGLONG started = GetTickCount64();
    Note("watch thread %lu", GetCurrentThreadId()); /* the crash notes name their thread: is it this one? */
    for (;;)
    {
        if (!g_fired[HF_EVENT_GAME_LOADED] && GameLoaded())
        {
            Note("game.dll loaded and decoded");
            ReserveNearGame(ApiModuleBase("game.dll", NULL)); /* before the game's own allocations fill the 2 GB around it */
            PhysicsPools(ApiModuleBase("game.dll", NULL)); /* before the plugin setup builds the physics pools */
            AddVectoredExceptionHandler(0, CrashNote); /* after other vectored handlers; it only logs and passes the fault on */
            InstallFailFastNote();
            InstallBehaviours(ApiModuleBase("game.dll", NULL)); /* before the entity data loads: copies name them */
            StartRegistry();
            StartProjectiles();
            Fire(HF_EVENT_GAME_LOADED);
        }
        if (g_fired[HF_EVENT_GAME_LOADED] && !g_fired[HF_EVENT_PROJECTILES_READY] && g_projectileState >= 0 && InstallProjectiles() &&
            g_projectileState == 1)
        {
            Note("projectile list ready");
            StatsTablesReady(ApiModuleBase("game.dll", NULL));
            Fire(HF_EVENT_PROJECTILES_READY);
        }
        if (g_fired[HF_EVENT_GAME_LOADED] && g_stratagemState == 0)
        {
            /* Once the game has loaded its stratagems; code still being decoded gets 20 s. */
            static ULONGLONG stratagemsSeen;
            uintptr_t game = ApiModuleBase("game.dll", NULL);
            uint64_t first = 0;
            if (game != 0 && !SafeRead(game + STR_TABLE + 8, &first, 8) && first != 0)
            {
                if (stratagemsSeen == 0) stratagemsSeen = GetTickCount64();
                if (g_game == 0) g_game = game;
                InstallStratagems(game, GetTickCount64() - stratagemsSeen >= 20000);
            }
        }
        if (g_fired[HF_EVENT_GAME_LOADED] && g_difficultyState == 0)
        {
            /* game.dll fills its difficulty tables when it loads; code still being decoded gets 20 s. */
            static ULONGLONG difficultySeen;
            uintptr_t game = ApiModuleBase("game.dll", NULL);
            if (game != 0)
            {
                if (difficultySeen == 0) difficultySeen = GetTickCount64();
                if (g_game == 0) g_game = game;
                InstallDifficulty(game, GetTickCount64() - difficultySeen >= 20000);
            }
        }
        if ((g_statCount || g_statsProbeCount) && !(g_statsTablesDone[0] && g_statsTablesDone[1] && g_statsTablesDone[2]) && g_fired[HF_EVENT_GAME_LOADED])
            StatsTablesReady(ApiModuleBase("game.dll", NULL));
        if (g_fired[HF_EVENT_PROJECTILES_READY]) StatsExtraReady();
        if (g_fired[HF_EVENT_GAME_LOADED]) { StatsBoostersReady(); StatsMeleeReady(); StatsDirectorReady(); ShotsTick(); RoundsTick(); }
        if (g_fired[HF_EVENT_GAME_LOADED] && g_tickState == 0) TickInstall();
        if (g_fired[HF_EVENT_GAME_LOADED]) InstallLoadoutPreview();
        if (g_fired[HF_EVENT_GAME_LOADED]) EventsInstall();
        if (g_fired[HF_EVENT_GAME_LOADED] && g_abilityState == 0) InstallAbilities(ApiModuleBase("game.dll", NULL));
        if (g_fired[HF_EVENT_GAME_LOADED] && g_netSwitchState == 0 && g_networkTypesAdded == 2)
        {
            /* Once the added network types exist; code still being decoded gets 20 s. */
            static ULONGLONG netSeen;
            uintptr_t game = ApiModuleBase("game.dll", NULL);
            if (game != 0)
            {
                if (netSeen == 0) netSeen = GetTickCount64();
                InstallNetSwitch(game, GetTickCount64() - netSeen >= 20000);
            }
        }
        if (g_fired[HF_EVENT_GAME_LOADED] && g_reapState == 0 && !strcmp(g_build, "25480438"))
        {
            /* the transport's code is the exe's; it gets the same 20 s as game.dll's */
            static ULONGLONG reapSeen;
            if (reapSeen == 0) reapSeen = GetTickCount64();
            InstallReaper(GetTickCount64() - reapSeen >= 20000);
        }
        if (g_fired[HF_EVENT_ENTITIES_READY]) PassivesTick(ApiModuleBase("game.dll", NULL));
        BehaviourSeenTick(); /* co-op diagnostics (behaviours.c) */
        if (g_fired[HF_EVENT_ENTITIES_READY]) StatsPerksReady();
        uintptr_t rows;
        uint32_t count;
        if (!g_fired[HF_EVENT_NETWORK_READY] && !FieldTypes(&rows, &count)) { Note("network configuration ready (%u field types)", count); RegisterNetworkTypes(); Fire(HF_EVENT_NETWORK_READY); }
        ULONGLONG elapsed = GetTickCount64() - started;
        static ULONGLONG lastRegionCheck;
        if (g_regionSize != 0 && (g_region == 0 || elapsed - lastRegionCheck > 1000))
        {
            lastRegionCheck = elapsed;
            uintptr_t region = g_region;
            uint8_t header[28];
            if (region != 0 && (SafeRead(region, header, 28) || memcmp(header, g_mapHeader, 28) != 0)) region = 0;
            if (region == 0) region = FindEntityRegion();
            /* Mods' weapons first: they are added right after the game's loader, and ENTITIES_READY promises them. */
            static ULONGLONG waitStarted;
            if (region != 0 && region != g_region && (RegistryPending() || StatsComponentsPending()))
            {
                if (waitStarted == 0) waitStarted = elapsed;
                if (elapsed - waitStarted < 20000) region = g_region;
                else Note("registry: the weapons were not added 20 s after the entity data loaded");
            }
            if (region != 0 && region != g_region)
            {
                Note("entity data ready at 0x%llx%s", (unsigned long long)region, g_region ? " (reloaded)" : "");
                g_region = region;
                if (StatsHasComponents() && !g_statsComponentsReady)
                {
                    for (int i = 0; i < g_statCount; i++) if (!strcmp(g_stats[i].kind, "component"))
                        StatsRefuse(&g_stats[i], "entity load-time pass was unavailable");
                    InterlockedExchange(&g_statsComponentsReady, 1);
                }
                Fire(HF_EVENT_ENTITIES_READY);
            }
        }
        if (g_fired[HF_EVENT_ENTITIES_READY] && g_stratagemState == 1 && !g_packagesInferred) InferStratagemPackages();
        if (g_editsWaiting) ApplyWaitingEdits();
        if (g_rowCopiesWaiting) RowCopiesTick();
        if (StatsRefsWaiting()) StatsRefsTick();
        if (g_behaviourState == 1) BehaviourWatch();
        if (g_fired[HF_EVENT_GAME_LOADED]) UnitCopiesTick(ApiModuleBase("game.dll", NULL));
        if (g_fired[HF_EVENT_GAME_LOADED]) FreezeWatch();
        if (g_fired[HF_EVENT_GAME_LOADED] && g_physicsPools == 0) PhysicsPools(ApiModuleBase("game.dll", NULL));
        if (g_difficultyState == 1) DifficultyWatch();
        if (g_difficultyState == 1 && g_game != 0) DifficultyDelta(g_game);
        if (g_fired[HF_EVENT_NETWORK_READY] && g_difficultyState == 1) DifficultyNetwork();
        if (g_fired[HF_EVENT_ENTITIES_READY]) StatsSnapshotReady(ApiModuleBase("game.dll", NULL));
        Sleep(elapsed < 180000 ? 5 : 250);
    }
    return 0;
}

/* ---- plan and loading ------------------------------------------------------------------------------------------- */

static size_t ParseHex(const char* text, uint8_t* out, size_t capacity)
{
    size_t n = 0;
    while (text[0] && text[1] && n < capacity)
    {
        int high = Hex(text[0]), low = Hex(text[1]);
        if (high < 0 || low < 0) break;
        out[n++] = (uint8_t)(high << 4 | low);
        text += 2;
    }
    return n;
}

static int Split(char* line, char** fields, int capacity)
{
    int count = 0;
    char* context = NULL;
    for (char* field = strtok_s(line, "\t", &context); field != NULL && count < capacity; field = strtok_s(NULL, "\t", &context)) fields[count++] = field;
    return count;
}

static void Utf8ToWide(const char* text, wchar_t* out, int capacity)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, out, capacity)) out[0] = 0;
}

/* Why the plan was refused: its line number and the line (logged once the log is open). */
static int g_planLine;
static char g_planBad[160];

static int ReadPlan(const wchar_t* path)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 1;
    DWORD size = GetFileSize(file, NULL), read = 0;
    char* text = size > 0 && size < (4u << 20) ? (char*)malloc(size + 1) : NULL;
    int ok = text != NULL && ReadFile(file, text, size, &read, NULL) && read == size;
    CloseHandle(file);
    if (!ok) { free(text); return 1; }
    text[size] = 0;
    char* context = NULL;
    for (char* line = strtok_s(text, "\r\n", &context); line != NULL; line = strtok_s(NULL, "\r\n", &context))
    {
        g_planLine++;
        snprintf(g_planBad, sizeof g_planBad, "%s", line);
        char* f[16];
        int n = Split(line, f, 16);
        if (n > 0 && !strcmp(f[0], "stat")) { if (StatsParse(f, n)) { free(text); return 1; } continue; }
        /* an observation only feeds the editors' values: one that cannot be read is skipped, not the session's mods */
        if (n > 0 && !strcmp(f[0], "observe")) { if (StatsParseProbe(f, n)) g_statsProbesSkipped++; continue; }
        if (n == 2 && !strcmp(f[0], "stats_schema"))
        {
            if (strlen(f[1]) != 64) { free(text); return 1; }
            for (int i = 0; i < 64; i++) if (Hex(f[1][i]) < 0) { free(text); return 1; }
            memcpy(g_statsSchema, f[1], 64); continue;
        }
        if (n == 2 && strcmp(f[0], "build") == 0) snprintf(g_build, sizeof g_build, "%s", f[1]);
        else if (n == 2 && strcmp(f[0], "logs") == 0) Utf8ToWide(f[1], g_logs, MAX_PATH);
        else if (n >= 4 && strcmp(f[0], "game_pin") == 0)
        {
            snprintf(g_pinModule, sizeof g_pinModule, "%s", f[1]);
            g_pinRva = (uint32_t)strtoul(f[2], NULL, 16);
            for (int i = 3; i < n && g_pinCount < MAX_PINS; i++, g_pinCount++) g_pinLength[g_pinCount] = ParseHex(f[i], g_pins[g_pinCount], 32);
        }
        else if (n == 3 && strcmp(f[0], "network_config") == 0)
        {
            snprintf(g_networkModule, sizeof g_networkModule, "%s", f[1]);
            g_networkRva = (uint32_t)strtoul(f[2], NULL, 16);
        }
        else if (n == 3 && strcmp(f[0], "entity_region") == 0)
        {
            g_regionSize = strtoull(f[1], NULL, 10);
            if (ParseHex(f[2], g_mapHeader, 28) != 28) g_regionSize = 0;
        }
        else if (n == 9 && strcmp(f[0], "component") == 0 && g_componentCount < MAX_COMPONENTS)
        {
            Component* c = &g_components[g_componentCount];
            snprintf(c->name, sizeof c->name, "%s", f[1]);
            c->offset = strtoull(f[2], NULL, 10);
            c->index = (uint32_t)strtoul(f[4], NULL, 10);
            c->indices = (uint32_t)strtoul(f[5], NULL, 10);
            c->records = (uint32_t)strtoul(f[6], NULL, 10);
            c->recordOffset = (uint32_t)strtoul(f[7], NULL, 10);
            c->stride = (uint32_t)strtoul(f[8], NULL, 10);
            if (ParseHex(f[3], c->header, 28) == 28 && c->offset >= 4) g_componentCount++;
        }
        else if (n == 4 && strcmp(f[0], "mod") == 0 && g_modCount < MAX_MODS)
        {
            HfMod* mod = &g_mods[g_modCount++];
            snprintf(mod->id, sizeof mod->id, "%s", f[1]);
            snprintf(mod->folder, sizeof mod->folder, "%s", f[2]);
            Utf8ToWide(f[3], mod->dll, MAX_PATH);
        }
    }
    free(text);
    /* game.dll is decoded once the routing site reads as the original or the session's routing patch: the reviewed pin
     * of the build, unless the plan names one. */
    if (g_pinCount == 0 && strcmp(g_build, "25480438") == 0)
    {
        static const char* const kPins[] = {"7575488b15fafc06024881c2e4030000", "6690488d153a1084000f1f8000000000"};
        snprintf(g_pinModule, sizeof g_pinModule, "%s", "game.dll");
        g_pinRva = 0x12b663d;
        for (int i = 0; i < 2; i++, g_pinCount++) g_pinLength[g_pinCount] = ParseHex(kPins[i], g_pins[g_pinCount], 32);
    }
    return 0;
}

#include "live.c"
#include "lua_bridge.c"
#include "netapply.c" /* after live.c: the co-op role */
#include "netreap.c"
#include "coop_diag.c" /* after live.c: the co-op role */

typedef int (*InitFunction)(const HfApi* api, HfMod* mod);

__declspec(dllexport) int hellforge_loader_start(const wchar_t* plan)
{
    if (plan == NULL || ReadPlan(plan))
    {
        if (g_logs[0])
        {
            g_log = OpenLog(L"modloader.log");
            Note("HellForge mod loader: the session plan was refused at line %d (%s); no mods load", g_planLine, g_planBad);
        }
        return 1;
    }
    g_log = OpenLog(L"modloader.log");
    if (g_statsProbesSkipped) Note("stats: %d observation(s) in the plan could not be read and are not shown", g_statsProbesSkipped);
    Note("HellForge mod loader, SDK %d, game build %s: %d code mod(s)", HF_MOD_SDK_VERSION, g_build, g_modCount);
    g_api = (HfApi){HF_MOD_SDK_VERSION, sizeof(HfApi), ApiLog, ApiBuildId, ApiModId, ApiModFolder, ApiOn, ApiModuleBase, ApiFindPattern,
        SafeRead, ApiWrite, ApiWriteU32If, ApiHook, ApiNetworkFieldType, ApiSetNetworkFieldBits, ApiComponentRecord, ApiAddWeapon, ApiWeaponEntity, ApiAddProjectile, ApiProjectileRow, ApiAddDamage, ApiDamageRow, ApiResourceHash,
        ApiAddStratagem, ApiStratagemRow, ApiAddEntity, ApiAddBehaviour, ApiAddDifficulty, ApiDifficultyRow,
        ApiRegisterEntity, ApiCopyField, ApiSetField, ApiSetTag, ApiMount, ApiRecord, ApiFind, ApiEditEntity, ApiSetLive, ApiGetLive, ApiSetPassives, ApiGetPassives,
        ApiOnGameThread, ApiLoadPackage, ApiLastLog, ApiLogsFolder, ApiGetStat, ApiSession, ApiSubscribe, ApiGameState, ApiContent, ApiAddAbility, ApiAddStratagemCategory, ApiSetStat, ApiCopyRow, ApiMods,
        ApiHeal, ApiHealLimb, ApiInjure, ActLimbState, ApiApplyStatus, ApiAddVelocity, ApiResupply, ApiSpawnExplosion, ApiSpawnProjectile,
        ApiCreditToPlayer, ApiFreezeTimer, ApiSpawnPelican, ApiModifyShots, ApiHoming, ApiStopShots, ApiNotice, ApiHookEx, ApiCodeMatches, ApiReadCode, ApiSyncState, ApiSyncMod, ApiSyncAnswer, ApiStatsSource};
    int setupOrder[MAX_MODS];
    SetupOrder(setupOrder);
    if (g_modCount > 1) Note("code mods are set up in the order of their ids (the same on every machine); their edits apply in load order");
    g_modsSettingUp = 1;
    for (int k = 0; k < g_modCount; k++)
    {
        HfMod* mod = &g_mods[setupOrder[k]];
        mod->api = &g_api;
        wchar_t logName[128];
        swprintf(logName, 128, L"mod-%hs.log", mod->id);
        mod->log = OpenLog(logName);
        if (!wcscmp(mod->dll, L"-")) { mod->loaded = 1; Note("%s: data-only stats mod loaded", mod->id); continue; }
        mod->module = LoadLibraryExW(mod->dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        InitFunction init = mod->module != NULL ? (InitFunction)(void*)GetProcAddress(mod->module, "hellforge_mod_init") : NULL;
        /* Mods built before the rename (HD2 Beyond The Stars) export bts_mod_init; the API is the same. */
        if (init == NULL && mod->module != NULL) init = (InitFunction)(void*)GetProcAddress(mod->module, "bts_mod_init");
        if (init == NULL)
        {
            Note("%s: not loaded (%s)", mod->id, mod->module == NULL ? "the DLL could not be loaded" : "it exports no hellforge_mod_init");
            if (mod->module != NULL) FreeLibrary(mod->module);
            continue;
        }
        Note("%s: initializing", mod->id);
        mod->loaded = 1;
        int result = init(&g_api, mod);
        if (result != 0)
        {
            mod->loaded = 0;
            Note("%s: declined to load (hellforge_mod_init returned %d)", mod->id, result);
            continue;
        }
        Note("%s: loaded", mod->id);
    }
    g_modsSettingUp = 0;
    FinishSetupEdits(); /* fields.c: the edits queued while the mods set up, reported and applied in load order */
    CoopDiagStart(); /* coop_diag.c: before the event sources install (they hook the kinds someone listens to) */
    HANDLE thread = CreateThread(NULL, 0, Watch, NULL, 0, NULL);
    if (thread != NULL) CloseHandle(thread);
    HANDLE live = g_logs[0] ? CreateThread(NULL, 0, LiveWatch, NULL, 0, NULL) : NULL; /* the launcher's live editor */
    if (live != NULL) CloseHandle(live);
    return 0;
}
