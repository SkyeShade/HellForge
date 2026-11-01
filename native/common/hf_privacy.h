/* Logs get passed around (Save logs to Desktop, bug reports), so what HellForge's native code logs does not name the PC
 * it ran on: the user folder (USERPROFILE) reads %USERPROFILE%, a Linux home folder (HOME under Wine/Proton) ~, and the
 * computer's name PC. HellForge.Core/Privacy.cs does the same for the C# side. Only a whole folder or a whole name is
 * replaced, in any letter case, with either slash or a doubled backslash (JSON): a mod id that starts with the account's
 * name stays. Header-only, C and C++; kernel32 calls only, so it may run under the loader lock. */
#ifndef HF_PRIVACY_H
#define HF_PRIVACY_H

#include <windows.h>
#include <string.h>

#define HF_PRIVACY_MAX 4

typedef struct
{
    char text[MAX_PATH * 3]; /* UTF-8 */
    size_t length;
    const char* mark;
    int folder;
} HfPrivacyName;

static HfPrivacyName g_hfPrivacy[HF_PRIVACY_MAX];
static int g_hfPrivacyCount;
static INIT_ONCE g_hfPrivacyOnce = INIT_ONCE_STATIC_INIT;

static inline void HfPrivacyAdd(const wchar_t* value, const char* mark, int folder)
{
    if (g_hfPrivacyCount >= HF_PRIVACY_MAX) return;
    HfPrivacyName* name = &g_hfPrivacy[g_hfPrivacyCount];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, value, -1, name->text, (int)sizeof name->text, NULL, NULL);
    if (bytes <= 1) return;
    name->length = (size_t)bytes - 1;
    while (folder && name->length > 0 && (name->text[name->length - 1] == '\\' || name->text[name->length - 1] == '/')) name->length--;
    name->text[name->length] = 0;
    /* A folder needs a parent (not a bare "C:"); a name under 3 characters is too common to replace. */
    if (name->length < 3 || (folder && !strpbrk(name->text + 1, "\\/"))) return;
    for (int i = 0; i < g_hfPrivacyCount; i++)
        if (g_hfPrivacy[i].length == name->length && _strnicmp(g_hfPrivacy[i].text, name->text, name->length) == 0) return;
    name->mark = mark;
    name->folder = folder;
    g_hfPrivacyCount++;
}

static inline BOOL CALLBACK HfPrivacyInit(PINIT_ONCE once, void* parameter, void** context)
{
    (void)once; (void)parameter; (void)context;
    wchar_t value[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", value, MAX_PATH);
    if (length > 0 && length < MAX_PATH) HfPrivacyAdd(value, "%USERPROFILE%", 1);
    length = GetEnvironmentVariableW(L"HOME", value, MAX_PATH);
    if (length > 0 && length < MAX_PATH && value[0] == L'/') HfPrivacyAdd(value, "~", 1);
    /* The DNS name first: the NetBIOS name can be its first 15 characters. */
    DWORD size = MAX_PATH;
    if (GetComputerNameExW(ComputerNameDnsHostname, value, &size)) HfPrivacyAdd(value, "PC", 0);
    size = MAX_PATH;
    if (GetComputerNameW(value, &size)) HfPrivacyAdd(value, "PC", 0);
    return TRUE;
}

static inline int HfPrivacyWordChar(char c)
{
    unsigned char u = (unsigned char)c;
    return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_' || u >= 0x80;
}

static inline char HfPrivacyLower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* How many bytes of text, from t, one slash of a folder takes: a slash, or one or two backslashes (0: none there). */
static inline size_t HfPrivacySeparator(const char* text, size_t length, size_t t)
{
    if (t >= length) return 0;
    if (text[t] == '/') return 1;
    if (text[t] != '\\') return 0;
    return t + 1 < length && text[t + 1] == '\\' ? 2 : 1;
}

/* How many bytes of text, from t, match the name's character c (0 when they do not). */
static inline size_t HfPrivacyStep(const char* text, size_t length, size_t t, char c, int folder)
{
    if (folder && (c == '\\' || c == '/')) return HfPrivacySeparator(text, length, t);
    return t < length && HfPrivacyLower(text[t]) == HfPrivacyLower(c) ? 1 : 0;
}

/* How many bytes of text, from at, are the name (0 when they are not). */
static inline size_t HfPrivacyMatch(const char* text, size_t length, size_t at, const HfPrivacyName* name)
{
    size_t t = at;
    if (!name->folder && at > 0 && HfPrivacyWordChar(text[at - 1])) return 0;
    for (size_t k = 0; k < name->length; k++)
    {
        size_t step = HfPrivacyStep(text, length, t, name->text[k], name->folder);
        if (step == 0) return 0;
        t += step;
    }
    /* A hyphen ends a computer name (the game's console_<time>-<id>-<computer>.txt) but not a folder name. */
    if (t < length && (HfPrivacyWordChar(text[t]) || (name->folder && text[t] == '-'))) return 0;
    return t - at;
}

/* How many bytes of text, from i, the first name that matches there takes, with its mark (0: none matches). */
static inline size_t HfPrivacyMatchAny(const char* text, size_t length, size_t i, const char** mark)
{
    for (int n = 0; n < g_hfPrivacyCount; n++)
    {
        size_t matched = HfPrivacyMatch(text, length, i, &g_hfPrivacy[n]);
        if (matched) { *mark = g_hfPrivacy[n].mark; return matched; }
    }
    return 0;
}

/* text[0..length) scrubbed in place, within capacity bytes (a longer result is cut there): the new length. */
static inline size_t HfPrivacyScrub(char* text, size_t length, size_t capacity)
{
    InitOnceExecuteOnce(&g_hfPrivacyOnce, HfPrivacyInit, NULL, NULL);
    if (g_hfPrivacyCount == 0 || length == 0) return length;
    char out[4096];
    size_t limit = capacity < sizeof out ? capacity : sizeof out, o = 0;
    int changed = 0;
    for (size_t i = 0; i < length && o < limit;)
    {
        const char* mark = NULL;
        size_t matched = HfPrivacyMatchAny(text, length, i, &mark);
        if (!matched) { out[o++] = text[i++]; continue; }
        for (const char* m = mark; *m && o < limit; m++) out[o++] = *m;
        i += matched;
        changed = 1;
    }
    if (!changed) return length;
    memcpy(text, out, o);
    return o;
}

/* A NUL-terminated string in a buffer of capacity bytes, scrubbed in place. */
static inline void HfPrivacyScrubString(char* text, size_t capacity)
{
    if (capacity == 0) return;
    size_t length = HfPrivacyScrub(text, strnlen(text, capacity - 1), capacity - 1);
    text[length] = 0;
}

#endif
