/* The loader's logs: the session log (Note), the difficulty diagnostics (NoteDiagnostic, only with the engine log on),
 * each mod's log (ApiLog) and their files. Included by hellforge_modloader.c before the patch sites and memory. */

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

/* The engine's own log option (--log-to-file on the game's command line, PlaySession.LaunchGame). The difficulty
 * diagnostics below are written only with it, so a session without the engine log behaves as it did. */
static int DiagnosticLog(void)
{
    static int on = -1;
    if (on < 0)
    {
        const wchar_t* command = GetCommandLineW();
        on = g_log != INVALID_HANDLE_VALUE && command != NULL && wcsstr(command, L"--log-to-file") != NULL;
    }
    return on;
}

/* A diagnostic line: the existing log path (WriteLog), only with the engine log on. */
static void NoteDiagnostic(const char* format, ...)
{
    if (!DiagnosticLog()) return;
    va_list arguments;
    va_start(arguments, format);
    WriteLog(g_log, "", format, arguments);
    va_end(arguments);
}

/* A log of the session's folder, opened to append, its dated line first (hf_log_header.h: the lines carry UTC times). */
static HANDLE OpenLog(const wchar_t* name)
{
    wchar_t path[MAX_PATH];
    if (!g_logs[0] || swprintf(path, MAX_PATH, L"%ls\\%ls", g_logs, name) < 0) return INVALID_HANDLE_VALUE;
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HfLogHeader(file);
    return file;
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
