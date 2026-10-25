// HellForge's Steam census in the copied game (steam_census.c): what the game asks of Steam, the surface a Steam bridge
// would have to carry (docs/steam-bridge.md). Started from the stand-in's DllMain.
#pragma once
#include <windows.h>

// With HELLFORGE_STEAM_CENSUS_LOG set: hooks GetProcAddress to name the stand-in's functions the game resolves.
void SteamCensusStart(HMODULE self);
// Nonzero when the census is logging.
int SteamCensusActive(void);
// Each Steam interface version the game requests (SteamInternal_FindOrCreateUserInterface).
void SteamCensusInterface(const char *version, int user);
// Each callback id the game registers (SteamAPI_RegisterCallback, SteamAPI_RegisterCallResult), with its struct's size
// from the game's CCallbackBase (callback may be NULL).
void SteamCensusCallback(const char *how, int id, void *callback);
// The census build: the interface object to hand the game, a counting proxy for real (or real itself otherwise).
void *SteamCensusWrap(void *real, const char *version);

// Valve's steam_api64.dll next to the stand-in (steam_cloud.c), loaded on first use; NULL if it is missing.
HMODULE SteamRealModule(void);
