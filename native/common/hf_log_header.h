/* A native log's dated line, written once each time the file is opened: the lines of HellForge's native logs carry only
 * the UTC time of day ("12:34:56.789 ..."), so this one says the local date and time it was opened and the offset from
 * UTC, to match them with the session runner's log (local time) and the player's own account of what happened.
 *     12:34:56.789 log opened 2026-10-26 05:34:56 local time (UTC-07:00); the times below are UTC
 * Header-only, C and C++; kernel32 calls only. */
#ifndef HF_LOG_HEADER_H
#define HF_LOG_HEADER_H

#include <windows.h>
#include <stdio.h>

/* The line for a file opened at `utc`, which is `local` on this PC's clock, ending in '\n', into out: its length. */
static inline int HfLogHeaderText(const SYSTEMTIME* utc, const SYSTEMTIME* local, char* out, size_t size)
{
    FILETIME u, l;
    long long minutes = 0;
    if (SystemTimeToFileTime(utc, &u) && SystemTimeToFileTime(local, &l))
    {
        long long ticks = (long long)(((ULONGLONG)l.dwHighDateTime << 32 | l.dwLowDateTime) - ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime));
        minutes = (ticks + (ticks < 0 ? -300000000LL : 300000000LL)) / 600000000LL; /* 100 ns ticks, to the nearest minute */
    }
    long long whole = minutes < 0 ? -minutes : minutes;
    int length = snprintf(out, size, "%02u:%02u:%02u.%03u log opened %04u-%02u-%02u %02u:%02u:%02u local time (UTC%c%02lld:%02lld); the times below are UTC\n",
        utc->wHour, utc->wMinute, utc->wSecond, utc->wMilliseconds, local->wYear, local->wMonth, local->wDay, local->wHour, local->wMinute,
        local->wSecond, minutes < 0 ? '-' : '+', whole / 60, whole % 60);
    return length < 0 ? 0 : length >= (int)size ? (int)size - 1 : length;
}

/* Writes the line for now into an opened log (nothing for INVALID_HANDLE_VALUE). */
static inline void HfLogHeader(HANDLE file)
{
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME utc, local;
    GetSystemTime(&utc);
    if (!SystemTimeToTzSpecificLocalTime(NULL, &utc, &local)) local = utc;
    char line[160];
    int length = HfLogHeaderText(&utc, &local, line, sizeof line);
    DWORD written;
    if (length > 0) WriteFile(file, line, (DWORD)length, &written, NULL);
}

#endif
