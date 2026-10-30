/* SDK 16: what a mod tells the player and the session's end.
 *
 * set_status: each mod's one line for the player. The loader keeps them in mod-status.tsv beside the logs ("<id>\t<text>"
 * a line, every mod with a status, rewritten whole and moved into place on each change, so a reader never sees half of
 * it), which the launcher's Mods page shows, and logs each change in the mod's log. The same text again is a compare
 * under a lock: no system call.
 *
 * HF_EVENT_SESSION_END: the session runner, stopping a session, sets Local\HellForge-session-end-<pid>
 * (HellForge.Core.SessionEndSignal), which the watch thread waits on instead of sleeping between its passes; the mods
 * hear the event there, and Local\HellForge-session-ended-<pid> tells the runner they did. A game that quits normally
 * delivers it from DllMain at the process's exit. Included by hellforge_modloader.c after SetupMod's helpers. */

#define STATUS_FILE L"mod-status.tsv"
static SRWLOCK g_statusLock = SRWLOCK_INIT;

/* text cut so that it ends on a whole UTF-8 character */
static void CutUtf8(char* text)
{
    size_t length = strlen(text), start = length;
    while (start > 0 && ((unsigned char)text[start - 1] & 0xC0) == 0x80) start--;
    if (start == 0) return;
    unsigned char lead = (unsigned char)text[start - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (length - (start - 1) < need) text[start - 1] = 0;
}

/* One line for the file: the text with tabs and line breaks as spaces, cut to the room, without the PC's names. */
static void StatusLine(const char* text, char line[STATUS_MAX])
{
    snprintf(line, STATUS_MAX, "%s", text != NULL ? text : "");
    for (char* c = line; *c; c++)
        if (*c == '\t' || *c == '\r' || *c == '\n') *c = ' ';
    CutUtf8(line);
    HfPrivacyScrubString(line, STATUS_MAX);
}

/* mod-status.tsv as the statuses are now. Under g_statusLock. */
static void StatusFileWrite(void)
{
    wchar_t path[MAX_PATH], partial[MAX_PATH];
    if (!g_logs[0] || swprintf(path, MAX_PATH, L"%ls\\%ls", g_logs, STATUS_FILE) < 0 ||
        swprintf(partial, MAX_PATH, L"%ls\\%ls.partial", g_logs, STATUS_FILE) < 0) return;
    HANDLE file = CreateFileW(partial, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { Note("status: %ls not written (%lu)", STATUS_FILE, GetLastError()); return; }
    int ok = 1;
    for (int i = 0; i < g_modCount && ok; i++)
    {
        if (!g_mods[i].status[0]) continue;
        char line[sizeof g_mods[i].id + STATUS_MAX + 2];
        int length = snprintf(line, sizeof line, "%s\t%s\n", g_mods[i].id, g_mods[i].status);
        DWORD written;
        ok = length > 0 && WriteFile(file, line, (DWORD)length, &written, NULL) && written == (DWORD)length;
    }
    CloseHandle(file);
    if (!ok || !MoveFileExW(partial, path, MOVEFILE_REPLACE_EXISTING))
    {
        Note("status: %ls not written (%lu)", STATUS_FILE, GetLastError());
        DeleteFileW(partial);
    }
}

/* SDK 16, set_status. */
static void ApiSetStatus(HfMod* mod, const char* text)
{
    if (mod == NULL || mod < g_mods || mod >= g_mods + g_modCount) return;
    const char* given = text != NULL ? text : "";
    AcquireSRWLockExclusive(&g_statusLock);
    if (strncmp(mod->statusGiven, given, STATUS_MAX) == 0 && strlen(given) < STATUS_MAX)
    {
        ReleaseSRWLockExclusive(&g_statusLock);
        return; /* the same text again */
    }
    snprintf(mod->statusGiven, STATUS_MAX, "%s", given);
    char line[STATUS_MAX];
    StatusLine(given, line);
    int changed = strcmp(mod->status, line) != 0;
    if (changed)
    {
        memcpy(mod->status, line, sizeof line);
        StatusFileWrite();
    }
    ReleaseSRWLockExclusive(&g_statusLock);
    if (changed && line[0]) ApiLog(mod, "status: %s", line);
    else if (changed) ApiLog(mod, "status cleared");
}

/* ---- the session's end ----------------------------------------------------------------------------------------- */

static HANDLE g_sessionEnd, g_sessionEnded;
static volatile LONG g_sessionEndDelivered;

/* The session runner's two events for this game, if it made them (a runner from before SDK 16 does not). */
static void OpenSessionEnd(void)
{
    wchar_t name[64];
    swprintf(name, 64, L"Local\\HellForge-session-end-%lu", GetCurrentProcessId());
    g_sessionEnd = OpenEventW(SYNCHRONIZE, FALSE, name);
    DWORD error = g_sessionEnd == NULL ? GetLastError() : 0;
    swprintf(name, 64, L"Local\\HellForge-session-ended-%lu", GetCurrentProcessId());
    g_sessionEnded = g_sessionEnd != NULL ? OpenEventW(EVENT_MODIFY_STATE, FALSE, name) : NULL;
    if (g_sessionEnd == NULL) Note("session end: the session runner gave no signal (%lu): mods hear of a normal quit only", error);
}

/* HF_EVENT_SESSION_END, once. */
static void SessionEnd(const char* why)
{
    if (InterlockedExchange(&g_sessionEndDelivered, 1)) return;
    Note("session end: %s", why);
    Fire(HF_EVENT_SESSION_END);
}

/* The watch thread's pause between its passes: the session runner's stop ends it, and the mods hear of it. */
static void WatchPause(DWORD milliseconds)
{
    if (g_sessionEnd == NULL) { Sleep(milliseconds); return; }
    if (WaitForSingleObject(g_sessionEnd, milliseconds) != WAIT_OBJECT_0) return;
    CloseHandle(g_sessionEnd);
    g_sessionEnd = NULL;
    SessionEnd("the session is stopping");
    if (g_sessionEnded != NULL) SetEvent(g_sessionEnded);
}

/* A normal quit (ExitProcess): the last chance to tell the mods. Under the loader lock, the other threads gone. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH && reserved != NULL && g_modCount > 0) SessionEnd("the game is quitting");
    return TRUE;
}
