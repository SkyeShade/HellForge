/* The native logs' dated line (common/hf_log_header.h): the local date and time and the offset from UTC on either side
 * of it, across midnight, and OpenLog writing it first. Included by stats_test.c. */

static void LogHeaderFixture(void)
{
    SYSTEMTIME utc = {2026, 10, 1, 26, 12, 34, 56, 789}, local = utc;
    char line[160];
    local.wHour = 5;
    assert(HfLogHeaderText(&utc, &local, line, sizeof line) == (int)strlen(line) &&
        !strcmp(line, "12:34:56.789 log opened 2026-10-26 05:34:56 local time (UTC-07:00); the times below are UTC\n"));
    local.wHour = 18;
    local.wMinute = 4;
    HfLogHeaderText(&utc, &local, line, sizeof line);
    assert(!strcmp(line, "12:34:56.789 log opened 2026-10-26 18:04:56 local time (UTC+05:30); the times below are UTC\n"));
    SYSTEMTIME night = {2026, 10, 1, 26, 2, 0, 0, 0}, before = {2026, 10, 0, 25, 19, 0, 0, 0};
    HfLogHeaderText(&night, &before, line, sizeof line);
    assert(!strcmp(line, "02:00:00.000 log opened 2026-10-25 19:00:00 local time (UTC-07:00); the times below are UTC\n"));
    assert(HfLogHeaderText(&utc, &utc, line, 20) == 19 && strlen(line) == 19); /* cut to the room given */

    wchar_t saved[MAX_PATH], path[MAX_PATH];
    wcscpy(saved, g_logs);
    assert(GetTempPathW(MAX_PATH, g_logs));
    swprintf(g_logs + wcslen(g_logs), MAX_PATH - wcslen(g_logs), L"hellforge-log-header-%lu", GetCurrentProcessId());
    CreateDirectoryW(g_logs, NULL);
    swprintf(path, MAX_PATH, L"%ls\\fixture.log", g_logs);
    DeleteFileW(path);
    HANDLE file = OpenLog(L"fixture.log");
    assert(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
    FILE* text = _wfopen(path, L"rb");
    char first[200] = {0};
    assert(text != NULL && fgets(first, sizeof first, text) != NULL);
    fclose(text);
    assert(strlen(first) > 12 && !strncmp(first + 12, " log opened ", 12) && strstr(first, "; the times below are UTC\n") != NULL);
    DeleteFileW(path);
    RemoveDirectoryW(g_logs);
    wcscpy(g_logs, saved);
    puts("PASS log headers: the local date and time, the offset from UTC either side, across midnight, first in each log");
}
