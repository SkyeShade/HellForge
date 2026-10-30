/* SDK 16 (mod_status.c): set_status's line (tabs and line breaks as spaces, cut on a whole UTF-8 character),
 * mod-status.tsv rewritten on a change only, cleared; the session's end through the session runner's two events,
 * delivered once. Included by stats_test.c. */

static int g_sessionEndsHeard;
static void SessionEndHeard(HfMod* mod, HfEvent event, void* user) { g_sessionEndsHeard++; }

/* mod-status.tsv's text (empty when there is none) and when it was written. */
static void StatusFile(const wchar_t* folder, char* text, size_t size, FILETIME* written)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\mod-status.tsv", folder);
    text[0] = 0;
    memset(written, 0, sizeof *written);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD read = 0;
    ReadFile(file, text, (DWORD)size - 1, &read, NULL);
    text[read] = 0;
    GetFileTime(file, NULL, NULL, written);
    CloseHandle(file);
}

static void StatusFixture(void)
{
    char line[STATUS_MAX];
    StatusLine("waiting\tfor a\r\nmission", line);
    assert(!strcmp(line, "waiting for a  mission"));
    char longText[400];
    memset(longText, 'x', sizeof longText);
    memcpy(longText + 189, "\xE2\x82\xAC\xE2\x82\xAC", 6); /* two euro signs across the cut at 191 bytes */
    longText[sizeof longText - 1] = 0;
    StatusLine(longText, line);
    assert(strlen(line) == 189 && line[188] == 'x'); /* the cut euro sign is left out whole */
    StatusLine(NULL, line);
    assert(line[0] == 0);

    int modCount = g_modCount;
    g_modCount = 2; /* a and b */
    wchar_t saved[MAX_PATH], folder[MAX_PATH], path[MAX_PATH];
    wcscpy(saved, g_logs);
    assert(GetTempPathW(MAX_PATH, folder));
    swprintf(folder + wcslen(folder), MAX_PATH - wcslen(folder), L"hellforge-mod-status-%lu", GetCurrentProcessId());
    CreateDirectoryW(folder, NULL);
    wcscpy(g_logs, folder);
    char text[1024];
    FILETIME first, again;
    ApiSetStatus(&g_mods[1], "running");
    ApiSetStatus(&g_mods[0], "off: no\tmission");
    StatusFile(folder, text, sizeof text, &first);
    assert(!strcmp(text, "a\toff: no mission\nb\trunning\n"));
    Sleep(20);
    ApiSetStatus(&g_mods[0], "off: no\tmission");      /* the same text: not written again */
    ApiSetStatus(&g_mods[0], "off: no mission");       /* another text, the same line: not written again */
    StatusFile(folder, text, sizeof text, &again);
    assert(CompareFileTime(&first, &again) == 0 && !strcmp(text, "a\toff: no mission\nb\trunning\n"));
    ApiSetStatus(&g_mods[1], NULL);                    /* cleared */
    ApiSetStatus(NULL, "nobody");
    ApiSetStatus((HfMod*)&g_api, "not a mod");
    StatusFile(folder, text, sizeof text, &again);
    assert(!strcmp(text, "a\toff: no mission\n"));
    ApiSetStatus(&g_mods[0], "");
    StatusFile(folder, text, sizeof text, &again);
    assert(text[0] == 0 && !strcmp(g_mods[0].status, "") && !strcmp(g_mods[1].status, ""));
    swprintf(path, MAX_PATH, L"%ls\\mod-status.tsv", folder);
    DeleteFileW(path);

    /* the session's end: the runner's events, the mods' event once, and the runner told */
    wchar_t name[64];
    swprintf(name, 64, L"Local\\HellForge-session-end-%lu", GetCurrentProcessId());
    HANDLE end = CreateEventW(NULL, TRUE, FALSE, name);
    swprintf(name, 64, L"Local\\HellForge-session-ended-%lu", GetCurrentProcessId());
    HANDLE ended = CreateEventW(NULL, TRUE, FALSE, name);
    assert(end != NULL && ended != NULL);
    OpenSessionEnd();
    assert(g_sessionEnd != NULL && g_sessionEnded != NULL);
    assert(ApiOn(&g_mods[0], HF_EVENT_SESSION_END, SessionEndHeard, NULL) == 0 && g_sessionEndsHeard == 0);
    assert(ApiOn(&g_mods[0], (HfEvent)6, SessionEndHeard, NULL) == 1); /* past the last event */
    WatchPause(1);
    assert(g_sessionEndsHeard == 0 && WaitForSingleObject(ended, 0) == WAIT_TIMEOUT);
    SetEvent(end);
    WatchPause(5000);
    assert(g_sessionEndsHeard == 1 && WaitForSingleObject(ended, 0) == WAIT_OBJECT_0 && g_sessionEnd == NULL);
    WatchPause(1);                                     /* a sleep again */
    SessionEnd("the game is quitting");                /* DllMain's: once only */
    assert(g_sessionEndsHeard == 1);
    CloseHandle(end);
    CloseHandle(ended);
    if (g_sessionEnded != NULL) CloseHandle(g_sessionEnded);
    g_sessionEnded = NULL;
    RemoveDirectoryW(folder);
    wcscpy(g_logs, saved);
    g_modCount = modCount;
    puts("PASS set_status and the session's end: one line, cut on a whole character, written on a change only, cleared; "
        "the runner's events, the mods' event once");
}
