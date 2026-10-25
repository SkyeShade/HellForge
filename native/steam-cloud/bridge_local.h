/* The game's side of the bridge that is written by hand (bridge_local.c): what the generated proxies call (one round
 * trip, a refusal), the methods answered in the game (from the snapshot, or fixed), and the callbacks the game registers.
 * Only the bridge build of the stand-in has it (HELLFORGE_BRIDGE_BUILD). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "bridge_marshal.h"
#include "bridge_channel.h"

/* The stand-in's channel to the broker: NULL until SteamAPI_Init connected (BridgeLocalAttach). */
void BridgeLocalAttach(GameChannel* channel);

/* One bridged call: the arguments in w, the result readable through r (whose bytes live in w's buffer from then on).
 * 0 when the broker refused, did not answer, or is gone: the proxy then answers as refused. */
int BridgeInvoke(uint32_t method, BridgeWriter* w, BridgeReader* r);

/* Logged once per what: a method the allow-list refuses, or arguments it does not accept. */
void BridgeRefused(const char* what);

/* The object for an interface version: a bridged proxy, the stand-in's Steam Cloud storage, or (logged) a null object
 * whose every slot answers 0. */
void* BridgeInterface(const char* version);

/* The callbacks the game registers (SteamAPI_RegisterCallback and the rest), and their dispatch from the ring. */
void BridgeRegisterCallback(void* callback, int id);
void BridgeUnregisterCallback(void* callback);
void BridgeRegisterCallResult(void* callback, uint64_t call);
void BridgeUnregisterCallResult(void* callback, uint64_t call);
/* Delivers one ring entry: a callback to every object registered for its id, a call result to its object. */
void BridgeDispatch(uint32_t callbackId, const uint8_t* data, uint32_t size);
