// language_dlc.c's proxy over a fake ISteamApps: the listed language DLCs are answered from HELLFORGE_LANGUAGE_DLC,
// every other app and method reaches the fake with the real object and its arguments, and InstallDLC of a language DLC
// never does. Usage: language_dlc_test.exe
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../language_dlc.h"

typedef struct FakeApps { void *const *vtable; int marker; } FakeApps;
static FakeApps *g_lastSelf;
static int g_installs, g_calls[30];
static uint64_t g_args[5];

// Steam's own answers: every DLC subscribed, installed and available.
static bool IsSubscribedApp(FakeApps *self, uint32_t app) { g_lastSelf = self; g_calls[6]++; return true; }
static bool IsDlcInstalled(FakeApps *self, uint32_t app) { g_lastSelf = self; g_calls[7]++; return true; }
static bool DlcData(FakeApps *self, int i, uint32_t *app, bool *available, char *name, int capacity)
{
    static const uint32_t apps[] = { 2506270, 2506220, 1001 };
    g_lastSelf = self;
    g_calls[11]++;
    if (i < 0 || i > 2) return false;
    *app = apps[i];
    *available = true;
    if (name && capacity > 0) snprintf(name, (size_t)capacity, "dlc %d", i);
    return true;
}
static bool InstallDlc(FakeApps *self, uint32_t app) { g_lastSelf = self; g_calls[12]++; g_installs++; return true; }
// Any other slot: records its five arguments and returns its slot number.
#define OTHER(n) static uint64_t Other##n(FakeApps *self, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) \
    { g_lastSelf = self; g_calls[n]++; g_args[0] = a; g_args[1] = b; g_args[2] = c; g_args[3] = d; g_args[4] = e; return 1000 + n; }
OTHER(0) OTHER(4) OTHER(20) OTHER(29)
static uint64_t Unused(FakeApps *self) { g_lastSelf = self; return 0; }

static void *g_vtable[30];
static FakeApps g_apps = { g_vtable, 0x5EA7 };

static int g_failures;
static void Expect(bool condition, const char *what) { if (!condition) { printf("FAIL %s\n", what); g_failures++; } }

typedef bool (*AppFn)(void *self, uint32_t app);
typedef bool (*DataFn)(void *self, int i, uint32_t *app, bool *available, char *name, int capacity);
typedef uint64_t (*FiveFn)(void *self, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);

int main(void)
{
    for (int i = 0; i < 30; i++) g_vtable[i] = (void *)Unused;
    g_vtable[0] = (void *)Other0; g_vtable[4] = (void *)Other4; g_vtable[20] = (void *)Other20; g_vtable[29] = (void *)Other29;
    g_vtable[6] = (void *)IsSubscribedApp; g_vtable[7] = (void *)IsDlcInstalled; g_vtable[11] = (void *)DlcData;
    g_vtable[12] = (void *)InstallDlc;

    // Japanese is in the copy, German is not; app 1001 is not a language DLC.
    SetEnvironmentVariableA("HELLFORGE_LANGUAGE_DLC", "2506270=1,2506220=0,junk,7=2");
    void *proxy = LanguageDlcWrap(&g_apps);
    Expect(proxy != NULL && proxy != &g_apps, "ISteamApps is wrapped when the session lists language DLCs");
    Expect(LanguageDlcWrap(&g_apps) == proxy, "The same object gets the same proxy");
    void *const *slots = *(void *const *const *)proxy;

    AppFn subscribed = (AppFn)slots[6], installed = (AppFn)slots[7], install = (AppFn)slots[12];
    Expect(subscribed(proxy, 2506270) && installed(proxy, 2506270), "A language DLC in the copy is subscribed and installed");
    Expect(!subscribed(proxy, 2506220) && !installed(proxy, 2506220), "A language DLC missing from the copy is neither");
    Expect(g_calls[6] == 0 && g_calls[7] == 0, "Steam is not asked about a listed language DLC");
    Expect(subscribed(proxy, 1001) && g_calls[6] == 1 && g_lastSelf == &g_apps, "Any other app is Steam's answer, asked of the real object");
    Expect(subscribed(proxy, 7) && g_calls[6] == 2, "A malformed entry is ignored: that app is Steam's answer");

    DataFn data = (DataFn)slots[11];
    uint32_t app = 0;
    bool available = false;
    char name[16] = "";
    Expect(data(proxy, 0, &app, &available, name, sizeof name) && app == 2506270 && available && !strcmp(name, "dlc 0"),
        "BGetDLCDataByIndex keeps Steam's data, available from the copy (in it)");
    Expect(data(proxy, 1, &app, &available, name, sizeof name) && app == 2506220 && !available, "... and not available when missing");
    Expect(data(proxy, 2, &app, &available, NULL, 0) && app == 1001 && available, "... and Steam's own for any other app");
    Expect(!data(proxy, 5, &app, &available, NULL, 0), "... and false past the last DLC");

    Expect(!install(proxy, 2506220) && g_installs == 0, "InstallDLC of a language DLC never reaches Steam");
    Expect(install(proxy, 1001) && g_installs == 1, "InstallDLC of any other app does");

    FiveFn first = (FiveFn)slots[0], fifth = (FiveFn)slots[4], owner = (FiveFn)slots[20], last = (FiveFn)slots[29];
    Expect(first(proxy, 1, 2, 3, 4, 5) == 1000 && g_lastSelf == &g_apps, "Slot 0 reaches the real object");
    Expect(fifth(proxy, 0, 0, 0, 0, 0) == 1004, "Slot 4 reaches its own method");
    Expect(owner(proxy, 11, 22, 33, 44, 55) == 1020 && g_args[0] == 11 && g_args[2] == 33 && g_args[4] == 55,
        "A forwarded slot passes every argument, those on the stack too");
    Expect(last(proxy, 0, 0, 0, 0, 0) == 1029, "The last slot (29) reaches its own method");

    if (g_failures == 0) printf("language DLC proxy: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
