/* The crash note (crash_notes.c): every fatal code it watches is noted, at most four times per code, so one code's
 * faults leave the others theirs; an access violation says what it read, the other codes say nothing of an access; a
 * stack overflow is written by the crash worker, for the crashed thread; a code it does not watch is left alone. The
 * first noted fault leaves the small dump (crash-<pid>.dmp, written by the worker), the only one of the session.
 * Included by stats_test.c. */

/* How often `needle` occurs in text. */
static int CrashCount(const char* text, const char* needle)
{
    int count = 0;
    for (const char* at = strstr(text, needle); at != NULL; at = strstr(at + 1, needle)) count++;
    return count;
}

/* The fixture's modloader.log, into text (size bytes, ending in 0). */
static void CrashLogText(const wchar_t* path, char* text, size_t size)
{
    FILE* file = _wfopen(path, L"rb");
    assert(file != NULL);
    size_t read = fread(text, 1, size - 1, file);
    text[read] = 0;
    fclose(file);
}

/* Raises `code` at the crash note `times` times, as the fault at 0x1234 with this stack. */
static void CrashRaise(EXCEPTION_POINTERS* info, DWORD code, int times)
{
    info->ExceptionRecord->ExceptionCode = code;
    for (int i = 0; i < times; i++) assert(CrashNote(info) == EXCEPTION_CONTINUE_SEARCH);
}

static void CrashNotesFixture(void)
{
    wchar_t folder[MAX_PATH], path[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, folder));
    swprintf(folder + wcslen(folder), MAX_PATH - wcslen(folder), L"hellforge-crash-notes-%lu", GetCurrentProcessId());
    CreateDirectoryW(folder, NULL);
    swprintf(path, MAX_PATH, L"%ls\\modloader.log", folder);
    DeleteFileW(path);
    HANDLE saved = g_log;
    wchar_t savedLogs[MAX_PATH], dump[MAX_PATH];
    wcscpy(savedLogs, g_logs);
    wcscpy(g_logs, folder);
    swprintf(dump, MAX_PATH, L"%ls\\crash-%lu.dmp", folder, GetCurrentProcessId());
    DeleteFileW(dump);
    g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(g_log != INVALID_HANDLE_VALUE);
    SetEnvironmentVariableW(L"HELLFORGE_CRASH_DUMP", NULL); /* no full dump: the small one */
    StartCrashWorker();
    assert(g_crashWorkerReady && g_crashWorkerId != 0);

    static uintptr_t stack[300];
    EXCEPTION_RECORD record = {0};
    CONTEXT context = {0};
    context.Rsp = (DWORD64)(uintptr_t)stack;
    record.ExceptionAddress = (void*)(uintptr_t)0x1234;
    record.NumberParameters = 2;
    record.ExceptionInformation[1] = 0x10;
    EXCEPTION_POINTERS info = {&record, &context};
    CrashRaise(&info, EXCEPTION_ACCESS_VIOLATION, 6);      /* four noted */
    record.NumberParameters = 0;
    CrashRaise(&info, CRASH_HEAP_CORRUPTION, 1);           /* the access violations left the other codes their notes */
    CrashRaise(&info, EXCEPTION_INT_DIVIDE_BY_ZERO, 1);
    CrashRaise(&info, EXCEPTION_ILLEGAL_INSTRUCTION, 1);
    CrashRaise(&info, 0xE06D7363, 1);                      /* a C++ exception: not a crash yet */
    CrashRaise(&info, EXCEPTION_STACK_OVERFLOW, 1);        /* through the worker */

    static char text[64 * 1024];
    CrashLogText(path, text, sizeof text);
    char thread[64];
    snprintf(thread, sizeof thread, "on thread %lu at ?+0x1234 (0x1234)", GetCurrentThreadId());
    assert(CrashCount(text, "crash: c0000005 ") == 4 && CrashCount(text, "(0x1234), reading 0x10; rax 0 ") == 4);
    assert(CrashCount(text, "crash: c0000374 ") == 1 && CrashCount(text, "crash: c0000094 ") == 1 && CrashCount(text, "crash: c000001d ") == 1);
    assert(CrashCount(text, "crash: c00000fd ") == 1 && CrashCount(text, thread) == 8 && CrashCount(text, "(0x1234); rax 0 ") == 4);
    assert(CrashCount(text, "e06d7363") == 0 && CrashCount(text, "crash: by module: at no module") == 8);
    char dumped[64];
    snprintf(dumped, sizeof dumped, "crash dump: small dump written (thread %lu, ", GetCurrentThreadId());
    assert(CrashCount(text, dumped) == 1 && CrashCount(text, "crash dump:") == 1 && g_crashWorkerReady);
    assert(strstr(text, dumped) > strstr(text, "crash: by module:")); /* after the first note */
    FILE* file = _wfopen(dump, L"rb");
    char signature[4] = {0};
    assert(file != NULL && fread(signature, 1, 4, file) == 4 && !memcmp(signature, "MDMP", 4));
    fclose(file);
    DeleteFileW(dump);

    CloseHandle(g_log);
    g_log = saved;
    wcscpy(g_logs, savedLogs);
    DeleteFileW(path);
    RemoveDirectoryW(folder);
    memset((void*)g_crashNoted, 0, sizeof g_crashNoted);
    g_crashDumped = 0;
    puts("PASS crash notes: access violations, illegal instructions, heap corruption, stack overflow (by the worker) and "
        "divide by zero, four of each code; the small dump of the first, by the worker");
}
