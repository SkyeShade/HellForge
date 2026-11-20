// Steam's answers about the language DLCs, from the game copy. Every voice but English is a DLC of its own, and the game
// offers a voice in its options only when Steam says the player has that DLC. Steam answers for the player's own
// install, not the copy, so a voice whose pack the copy lacks was offered, and choosing it crashed the game. The session
// lists the language DLCs in HELLFORGE_LANGUAGE_DLC, each with whether its pack is in the copy ("2506270=1,2506220=0");
// for those apps the copy decides, in ISteamApps' BIsSubscribedApp, BIsDlcInstalled and BGetDLCDataByIndex's
// "available", and InstallDLC never reaches Steam (it would download into the player's own install). Every other app and
// every other method is Steam's own answer. The bridge build answers the same from its snapshot (bridge_local.c).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "language_dlc.h"

#define LANGUAGE_DLC_MAX 32
// ISteamApps' vtable in the SDK the game is built with (each flat SteamAPI_ISteamApps_* in Valve's steam_api64.dll jumps
// to its slot): 30 methods.
#define APPS_SLOTS 30
#define SLOT_IS_SUBSCRIBED_APP 6
#define SLOT_IS_DLC_INSTALLED 7
#define SLOT_DLC_DATA_BY_INDEX 11
#define SLOT_INSTALL_DLC 12

static struct { uint32_t app; bool inCopy; volatile LONG told; } g_dlc[LANGUAGE_DLC_MAX];
static int g_count;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static LanguageDlcLogFn g_log;

void LanguageDlcSetLog(LanguageDlcLogFn log) { g_log = log; }

static BOOL CALLBACK Load(PINIT_ONCE once, void *parameter, void **context)
{
    char list[1024];
    DWORD length = GetEnvironmentVariableA("HELLFORGE_LANGUAGE_DLC", list, sizeof list);
    if (length == 0 || length >= sizeof list) return TRUE;
    for (char *entry = strtok(list, ","); entry && g_count < LANGUAGE_DLC_MAX; entry = strtok(NULL, ","))
    {
        char *end;
        unsigned long app = strtoul(entry, &end, 10);
        if (app == 0 || *end != '=' || (end[1] != '0' && end[1] != '1') || end[2] != 0) continue;
        g_dlc[g_count].app = (uint32_t)app;
        g_dlc[g_count++].inCopy = end[1] == '1';
    }
    return TRUE;
}

bool LanguageDlcInCopy(uint32_t app, bool *inCopy)
{
    InitOnceExecuteOnce(&g_once, Load, NULL, NULL);
    for (int i = 0; i < g_count; i++)
    {
        if (g_dlc[i].app != app) continue;
        *inCopy = g_dlc[i].inCopy;
        // once per app, so the log shows which DLC the game asks about
        if (g_log && !InterlockedExchange(&g_dlc[i].told, 1))
            g_log("language DLC %u: %s, from the game copy", app, *inCopy ? "in the copy" : "not in the copy");
        return true;
    }
    return false;
}

// The proxy: every slot calls Valve's object, with the real object in place of the proxy. No ISteamApps method takes a
// float or more than five arguments, and none returns a struct in registers (GetAppOwner's CSteamID comes back through
// the hidden pointer, an argument like the others).
typedef struct AppsProxy { void *const *vtable; void *real; } AppsProxy;
typedef uint64_t (*AppsSlot)(void *self, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);

static uint64_t Forward(AppsProxy *proxy, int slot, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    void *const *vtable = *(void *const *const *)proxy->real;
    return ((AppsSlot)vtable[slot])(proxy->real, a, b, c, d, e);
}

#define FORWARD(n) \
    static uint64_t Slot##n(AppsProxy *p, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) { return Forward(p, n, a, b, c, d, e); }
FORWARD(0) FORWARD(1) FORWARD(2) FORWARD(3) FORWARD(4) FORWARD(5) FORWARD(8) FORWARD(9) FORWARD(10) FORWARD(13)
FORWARD(14) FORWARD(15) FORWARD(16) FORWARD(17) FORWARD(18) FORWARD(19) FORWARD(20) FORWARD(21) FORWARD(22) FORWARD(23)
FORWARD(24) FORWARD(25) FORWARD(26) FORWARD(27) FORWARD(28) FORWARD(29)

static uint64_t IsSubscribedApp(AppsProxy *p, uint64_t app, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    bool inCopy;
    return LanguageDlcInCopy((uint32_t)app, &inCopy) ? inCopy : (uint8_t)Forward(p, SLOT_IS_SUBSCRIBED_APP, app, b, c, d, e);
}

static uint64_t IsDlcInstalled(AppsProxy *p, uint64_t app, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    bool inCopy;
    return LanguageDlcInCopy((uint32_t)app, &inCopy) ? inCopy : (uint8_t)Forward(p, SLOT_IS_DLC_INSTALLED, app, b, c, d, e);
}

// BGetDLCDataByIndex(int iDLC, AppId_t *pAppID, bool *pbAvailable, char *pchName, int cchNameBufferSize)
static uint64_t DlcDataByIndex(AppsProxy *p, uint64_t index, uint64_t appOut, uint64_t availableOut, uint64_t name, uint64_t capacity)
{
    uint64_t found = Forward(p, SLOT_DLC_DATA_BY_INDEX, index, appOut, availableOut, name, capacity);
    uint32_t app;
    bool inCopy;
    if ((uint8_t)found && appOut && availableOut)
    {
        memcpy(&app, (const void *)(uintptr_t)appOut, sizeof app);
        if (LanguageDlcInCopy(app, &inCopy)) memcpy((void *)(uintptr_t)availableOut, &inCopy, 1);
    }
    return found;
}

static uint64_t InstallDlc(AppsProxy *p, uint64_t app, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    bool inCopy;
    if (!LanguageDlcInCopy((uint32_t)app, &inCopy)) return Forward(p, SLOT_INSTALL_DLC, app, b, c, d, e);
    if (g_log) g_log("InstallDLC %u: not passed to Steam (a language pack is added to the copy by HellForge)", (unsigned)app);
    return 0;
}

static void *const g_appsSlots[APPS_SLOTS] = {
    (void *)Slot0, (void *)Slot1, (void *)Slot2, (void *)Slot3, (void *)Slot4, (void *)Slot5, (void *)IsSubscribedApp,
    (void *)IsDlcInstalled, (void *)Slot8, (void *)Slot9, (void *)Slot10, (void *)DlcDataByIndex, (void *)InstallDlc,
    (void *)Slot13, (void *)Slot14, (void *)Slot15, (void *)Slot16, (void *)Slot17, (void *)Slot18, (void *)Slot19,
    (void *)Slot20, (void *)Slot21, (void *)Slot22, (void *)Slot23, (void *)Slot24, (void *)Slot25, (void *)Slot26,
    (void *)Slot27, (void *)Slot28, (void *)Slot29,
};

void *LanguageDlcWrap(void *real)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static AppsProxy proxies[4];
    static int proxyCount;
    InitOnceExecuteOnce(&g_once, Load, NULL, NULL);
    if (real == NULL || g_count == 0) return real;
    AppsProxy *proxy = NULL;
    AcquireSRWLockExclusive(&lock);
    for (int i = 0; i < proxyCount && proxy == NULL; i++)
        if (proxies[i].real == real) proxy = &proxies[i];
    if (proxy == NULL && proxyCount < 4)
    {
        proxy = &proxies[proxyCount++];
        proxy->vtable = g_appsSlots;
        proxy->real = real;
        if (g_log)
        {
            int inCopy = 0;
            for (int i = 0; i < g_count; i++) inCopy += g_dlc[i].inCopy;
            g_log("ISteamApps: %d language DLCs answered from the game copy (%d in it)", g_count, inCopy);
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return proxy != NULL ? (void *)proxy : real;
}
