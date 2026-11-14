// HellForge's network gate in the copied game (net_gate.c). Started from the stand-in's DllMain.
#pragma once
#include <stdbool.h>
#include <wchar.h>

// With HELLFORGE_NETGATE=census and HELLFORGE_NETGATE_LOG set: hooks the process's network calls and logs every attempt.
void NetGateStart(void);

// For the stand-in's other gates (tablet_gate.c), written only while the census is on: a call written as the census
// writes one (repeats counted, the caller named), and a whole line of their own.
void NetGateNote(const char *api, const char *target, const char *scope, void *returnAddress);
void NetGateWrite(const char *line);

// Calls loaded with each DLL's file name as the loader maps it, under the loader lock; false when it cannot watch.
bool NetGateWatchLoads(void (*loaded)(const wchar_t *name));
