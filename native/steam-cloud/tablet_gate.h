// The tablet gate in the copied game (tablet_gate.c). Started from the stand-in's DllMain, after the network gate.
#pragma once

// In a sandboxed session (HELLFORGE_BRIDGE_PIPE set): refuses the game the Wintab tablet driver, Wintab32.dll.
void TabletGateStart(void);
