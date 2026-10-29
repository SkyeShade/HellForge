// sandbox_canary.exe: the Windows side of phase 2 (review-F1.md 4.2, 5.1-5.4, 10 phase 2).
//
// The reviewer runs it under real Proton, through hf-sandbox, with an absolute Linux path to the exe (Proton's
// steam:run_process cannot create a relative program):
//   hf-sandbox --session <run> --prefix <compatdata> -- proton run /absolute/<run>/game/sandbox_canary.exe
// It must be in the session's game folder (hf-sandbox starts the command there). It tries the G1 writes that must fail
// (the real home, the real prefix's drive_c, the game copy), the C: profiles that must be absent and the game copy that
// must be readable through C:, and the writes that must work (the overlay's C:\ and gameOut). It writes one report with
// every result under gameOut. Exit code 0 means every expectation held.
//
// Inputs (all optional; paths are Linux paths and are shown to Wine as Z:):
//   HF_SANDBOX_GAME_OUT      the session's game folder; default: the current directory
//   HF_SANDBOX_REAL_HOME     the player's real home; default: Z:\home\<USER>
//   HF_SANDBOX_REAL_PREFIX   the real compatdata folder; default: <STEAM_COMPAT_CLIENT_INSTALL_PATH>/steamapps/compatdata/553850
//   HF_SANDBOX_GAME_COPY     the game copy; default: <INSTALL>/game

#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>

static int failures;
static FILE *report_file;

static void report(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vprintf(format, arguments);
    fputc('\n', stdout);
    va_end(arguments);
    if (report_file != NULL)
    {
        va_start(arguments, format);
        vfprintf(report_file, format, arguments);
        va_end(arguments);
        fputc('\n', report_file);
        fflush(report_file);
    }
    fflush(stdout);
}

static bool env_value(const wchar_t *name, wchar_t *out, DWORD size)
{
    DWORD got = GetEnvironmentVariableW(name, out, size);
    if (got == 0 || got >= size)
    {
        out[0] = L'\0';
        return false;
    }
    return true;
}

static void linux_to_z(const wchar_t *linux, wchar_t *out, size_t size)
{
    size_t at = 0;
    if (linux[0] != L'/')
    {
        wcsncpy(out, linux, size - 1);
        out[size - 1] = L'\0';
        return;
    }
    if (size < 4) { out[0] = L'\0'; return; }
    out[at++] = L'Z';
    out[at++] = L':';
    for (size_t index = 0; linux[index] != L'\0' && at + 1 < size; index++)
        out[at++] = linux[index] == L'/' ? L'\\' : linux[index];
    out[at] = L'\0';
}

static bool try_write(const wchar_t *path, const wchar_t *text)
{
    HANDLE handle = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD length = (DWORD)(wcslen(text) * sizeof(wchar_t));
    DWORD written = 0;
    bool ok = WriteFile(handle, text, length, &written, NULL) != 0;
    CloseHandle(handle);
    return ok;
}

static void expect_denied(const char *name, const wchar_t *path)
{
    if (try_write(path, L"canary"))
    {
        DeleteFileW(path);
        report("FAIL %s: %ls was writable", name, path);
        failures++;
        return;
    }
    report("PASS %s: %ls refused (error %lu)", name, path, (unsigned long)GetLastError());
}

static void expect_allowed(const char *name, const wchar_t *path)
{
    if (try_write(path, L"canary"))
    {
        report("PASS %s: %ls accepted a write", name, path);
        return;
    }
    report("FAIL %s: %ls (error %lu)", name, path, (unsigned long)GetLastError());
    failures++;
}

static void expect_missing(const char *name, const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES &&
        (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND))
    {
        report("PASS %s: %ls is absent", name, path);
        return;
    }
    report("FAIL %s: %ls is present or could not be checked (error %lu)", name, path, (unsigned long)GetLastError());
    failures++;
}

static void expect_readable(const char *name, const wchar_t *path)
{
    HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle);
        report("PASS %s: %ls is readable", name, path);
        return;
    }
    report("FAIL %s: %ls is not readable (error %lu)", name, path, (unsigned long)GetLastError());
    failures++;
}

static void join_path(wchar_t *out, size_t size, const wchar_t *base, const wchar_t *name)
{
    _snwprintf(out, size, L"%ls\\%ls", base, name);
    out[size - 1] = L'\0';
}

static void open_report(const wchar_t *game_out)
{
    wchar_t path[MAX_PATH];
    join_path(path, MAX_PATH, game_out, L"sandbox-canary.txt");
    report_file = _wfopen(path, L"w");
    report("sandbox_canary: gameOut %ls, report %ls", game_out, path);
}

int main(void)
{
    wchar_t game_out[MAX_PATH], home[MAX_PATH], prefix[MAX_PATH], game_copy[MAX_PATH];
    wchar_t path[MAX_PATH], text[MAX_PATH];
    if (!env_value(L"HF_SANDBOX_GAME_OUT", text, MAX_PATH))
    {
        if (GetCurrentDirectoryW(MAX_PATH, text) == 0) text[0] = L'\0';
    }
    if (text[0] == L'/' ) linux_to_z(text, game_out, MAX_PATH);
    else wcsncpy(game_out, text, MAX_PATH - 1), game_out[MAX_PATH - 1] = L'\0';
    open_report(game_out);
    if (report_file == NULL) report("sandbox_canary: could not open the report under gameOut");

    if (env_value(L"HF_SANDBOX_REAL_HOME", text, MAX_PATH))
    {
        linux_to_z(text, home, MAX_PATH);
    }
    else if (env_value(L"USER", text, MAX_PATH))
    {
        _snwprintf(home, MAX_PATH, L"Z:\\home\\%ls", text);
        home[MAX_PATH - 1] = L'\0';
    }
    else
    {
        wcscpy(home, L"Z:\\home");
    }
    join_path(path, MAX_PATH, home, L"hf-sandbox-canary.txt");
    expect_denied("real home write", path);

    if (env_value(L"HF_SANDBOX_REAL_PREFIX", text, MAX_PATH))
    {
        linux_to_z(text, prefix, MAX_PATH);
    }
    else if (env_value(L"STEAM_COMPAT_CLIENT_INSTALL_PATH", text, MAX_PATH))
    {
        wchar_t steam[MAX_PATH];
        linux_to_z(text, steam, MAX_PATH);
        _snwprintf(prefix, MAX_PATH, L"%ls\\steamapps\\compatdata\\553850", steam);
        prefix[MAX_PATH - 1] = L'\0';
    }
    else
    {
        prefix[0] = L'\0';
    }
    if (prefix[0] != L'\0')
    {
        _snwprintf(path, MAX_PATH, L"%ls\\pfx\\drive_c\\hf-sandbox-canary.txt", prefix);
        path[MAX_PATH - 1] = L'\0';
        expect_denied("real prefix drive_c write", path);
    }
    else
    {
        report("FAIL real prefix drive_c write: no HF_SANDBOX_REAL_PREFIX and no STEAM_COMPAT_CLIENT_INSTALL_PATH");
        failures++;
    }

    if (env_value(L"HF_SANDBOX_GAME_COPY", text, MAX_PATH))
    {
        linux_to_z(text, game_copy, MAX_PATH);
    }
    else if (env_value(L"INSTALL", text, MAX_PATH))
    {
        wchar_t install[MAX_PATH];
        linux_to_z(text, install, MAX_PATH);
        _snwprintf(game_copy, MAX_PATH, L"%ls\\game", install);
        game_copy[MAX_PATH - 1] = L'\0';
    }
    else
    {
        game_copy[0] = L'\0';
    }
    if (game_copy[0] != L'\0')
    {
        join_path(path, MAX_PATH, game_copy, L"hf-sandbox-canary.txt");
        expect_denied("game copy write", path);
    }
    else
    {
        report("FAIL game copy write: no HF_SANDBOX_GAME_COPY and no INSTALL");
        failures++;
    }

    expect_allowed("overlay C: write", L"C:\\hf-sandbox-canary.txt");
    join_path(path, MAX_PATH, game_out, L"hf-sandbox-canary.txt");
    expect_allowed("gameOut write", path);

    // The overlay mask: the installation path through C: must contain only the game copy, never the profiles.
    expect_missing("overlay C: profiles", L"C:\\users\\steamuser\\Documents\\HellForge\\profiles");
    expect_readable("overlay C: game copy", L"C:\\users\\steamuser\\Documents\\HellForge\\game\\bin\\helldivers2.exe");

    report("sandbox_canary: %s (%d failure(s))", failures == 0 ? "PASS" : "FAIL", failures);
    if (report_file != NULL) fclose(report_file);
    return failures == 0 ? 0 : 1;
}
