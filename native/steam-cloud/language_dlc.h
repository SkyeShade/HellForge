// Steam's answers about the language DLCs, from the game copy (language_dlc.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef void (*LanguageDlcLogFn)(const char *format, ...);

// Where the stand-in's log lines go (steam_cloud.c's Log); none until set.
void LanguageDlcSetLog(LanguageDlcLogFn log);

// Whether app is a language DLC the session listed; if so, inCopy says whether its pack is in the game copy.
bool LanguageDlcInCopy(uint32_t app, bool *inCopy);

// Valve's ISteamApps (STEAMAPPS_INTERFACE_VERSION008) behind a proxy that answers for the listed language DLCs; real
// itself when the session listed none.
void *LanguageDlcWrap(void *real);
