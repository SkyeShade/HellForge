/* The engine's performance fixes (docs/performance.md) are on by default. HELLFORGE_PERF_OFF turns some off, by name
 * ("safe-read,corpse-freeze") or all of them ("all"), so a tester can rule one out without another build. Each fix
 * asks once, at its install. Included by hellforge_modloader.c after logging.c. */

#define PERF_OFF_MAX 512

/* 1 when the token start..end names the fix, or is "all". */
static int PerfFixToken(const char* start, const char* end, const char* name)
{
    while (start < end && *start == ' ') start++;
    while (end > start && end[-1] == ' ') end--;
    size_t size = (size_t)(end - start);
    if (size == 3 && _strnicmp(start, "all", 3) == 0) return 1;
    return size != 0 && size == strlen(name) && _strnicmp(start, name, size) == 0;
}

/* 1 when name is listed. Names are compared without case; spaces around them are ignored. */
static int PerfFixListed(const char* list, const char* name)
{
    for (const char* p = list;; p++)
    {
        const char* start = p;
        while (*p != '\0' && *p != ',') p++;
        if (PerfFixToken(start, p, name)) return 1;
        if (*p == '\0') return 0;
    }
}

static int PerfFixOn(const char* name)
{
    char list[PERF_OFF_MAX];
    DWORD size = GetEnvironmentVariableA("HELLFORGE_PERF_OFF", list, sizeof list);
    if (size == 0 || size >= sizeof list) return 1;
    if (!PerfFixListed(list, name)) return 1;
    Note("performance fix %s: off (HELLFORGE_PERF_OFF)", name);
    return 0;
}
