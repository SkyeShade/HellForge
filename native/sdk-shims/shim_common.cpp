#include "shim_common.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../common/hf_privacy.h"

namespace hellforge {
namespace {

constexpr uint32_t MaximumFunctions = 256;
volatile LONG64 calls[MaximumFunctions];
// 0: default, 1: ok, 2: fail (HELLFORGE_SHIM_POLICY).
uint8_t policy[MaximumFunctions];
HANDLE logFile = INVALID_HANDLE_VALUE;
SRWLOCK logLock = SRWLOCK_INIT;
INIT_ONCE started = INIT_ONCE_STATIC_INIT;
ULONGLONG startTicks;

void OpenLog()
{
    wchar_t directory[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"HELLFORGE_SHIM_LOG_DIR", directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        length = GetTempPathW(MAX_PATH, directory);
        if (length == 0 || length + 10 >= MAX_PATH) return;
        wcscat_s(directory, L"hellforge-shims");
    }
    CreateDirectoryW(directory, nullptr);
    wchar_t path[MAX_PATH];
    if (swprintf(path, MAX_PATH, L"%ls\\%hs-%lu.log", directory, g_moduleName, GetCurrentProcessId()) < 0) return;
    logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// A policy entry's value: 1 for "ok", 2 for "fail", 0 for anything else.
uint8_t PolicyValue(const char* value)
{
    if (strcmp(value, "ok") == 0) return 1;
    return strcmp(value, "fail") == 0 ? 2 : 0;
}

void ReadPolicy()
{
    char text[4096];
    DWORD length = GetEnvironmentVariableA("HELLFORGE_SHIM_POLICY", text, sizeof(text));
    if (length == 0 || length >= sizeof(text)) return;
    char* context = nullptr;
    for (char* entry = strtok_s(text, ",", &context); entry != nullptr; entry = strtok_s(nullptr, ",", &context))
    {
        char* equals = strchr(entry, '=');
        if (equals == nullptr) continue;
        *equals = '\0';
        uint8_t value = PolicyValue(equals + 1);
        for (uint32_t i = 0; i < g_functionCount && i < MaximumFunctions; i++)
            if (value != 0 && strcmp(g_functions[i].name, entry) == 0) policy[i] = value;
    }
}

BOOL CALLBACK Start(PINIT_ONCE, PVOID, PVOID*)
{
    startTicks = GetTickCount64();
    OpenLog();
    ReadPolicy();
    wchar_t exe[MAX_PATH];
    char exeUtf8[MAX_PATH * 3] = "";
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) != 0)
        WideCharToMultiByte(CP_UTF8, 0, exe, -1, exeUtf8, sizeof(exeUtf8), nullptr, nullptr);
    char policyText[512] = "";
    GetEnvironmentVariableA("HELLFORGE_SHIM_POLICY", policyText, sizeof(policyText));
    Line line;
    line.Append("%s: HellForge stand-in loaded (no PlayFab access); pid %lu; exe %s; %u exports; policy \"%s\"",
        g_moduleName, GetCurrentProcessId(), exeUtf8, g_functionCount, policyText);
    line.Write();
    return TRUE;
}

void EnsureStarted() { InitOnceExecuteOnce(&started, Start, nullptr, nullptr); }

}

bool Succeeds(uint32_t index)
{
    EnsureStarted();
    if (index < MaximumFunctions && policy[index] != 0) return policy[index] == 1;
    return !g_functions[index].hasOutput;
}

bool CountCall(uint32_t index, uint64_t* callNumber)
{
    EnsureStarted();
    if (index >= MaximumFunctions) return false;
    uint64_t call = (uint64_t)InterlockedIncrement64(&calls[index]);
    *callNumber = call;
    return call <= 32 || (call & (call - 1)) == 0;
}

void WriteLine(const char* text, size_t length)
{
    if (logFile == INVALID_HANDLE_VALUE) return;
    char buffer[1200];
    int prefix = snprintf(buffer, sizeof(buffer), "[+%llu ms] [tid %lu] ",
        (unsigned long long)(GetTickCount64() - startTicks), GetCurrentThreadId());
    size_t total = prefix > 0 ? (size_t)prefix : 0;
    size_t copy = length < sizeof(buffer) - total - 2 ? length : sizeof(buffer) - total - 2;
    memcpy(buffer + total, text, copy);
    total += HfPrivacyScrub(buffer + total, copy, sizeof(buffer) - total - 2);
    buffer[total++] = '\r';
    buffer[total++] = '\n';
    DWORD written;
    AcquireSRWLockExclusive(&logLock);
    WriteFile(logFile, buffer, (DWORD)total, &written, nullptr);
    ReleaseSRWLockExclusive(&logLock);
}

void Line::Append(const char* format, ...)
{
    if (length >= sizeof(text) - 1) return;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(text + length, sizeof(text) - length, format, arguments);
    va_end(arguments);
    if (written < 0) return;
    length = length + (size_t)written < sizeof(text) - 1 ? length + (size_t)written : sizeof(text) - 1;
}

void Note(const char* format, ...)
{
    EnsureStarted();
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (written > 0) WriteLine(text, (size_t)written < sizeof(text) ? (size_t)written : sizeof(text) - 1);
}

void Line::Argument(const char* value)
{
    if (value == nullptr) Append("null");
    else Append("\"%.120s\"", value);
}

}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        // Kernel32 file calls only, which are safe under the loader lock: the log proves the game loaded the stand-in.
        InitOnceExecuteOnce(&hellforge::started, hellforge::Start, nullptr, nullptr);
    }
    return TRUE;
}
