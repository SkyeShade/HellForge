/* The game's own Steam lobby between several installations on one PC (HELLFORGE_MULTI_INSTANCE=1, a test set-up). They
 * share one Steam account, and Steam cannot hold a lobby between one user and itself: a co-op joiner enters the host's
 * Steam lobby and the host waits to see it there (census 2026-10-24). So each instance's broker keeps that lobby in a
 * folder the instances share, under the instance's own SteamID (HELLFORGE_IDENTITY), and answers the bridged
 * ISteamMatchmaking calls from it. Steam P2P between them goes over loopback UDP the same way: each broker listens on
 * 127.0.0.1 at BROKER_P2P_PORT_BASE + its SteamID % 1000, and answers the bridged ISteamNetworking calls from what
 * arrives there. Everything else still goes to Steam. One file per lobby, rewritten whole under a lock file. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#define BROKER_LOBBY_MEMBERS 8
#define BROKER_LOBBY_KEYS 64
#define BROKER_LOBBY_KEY 256
#define BROKER_LOBBY_VALUE 8193
#define BROKER_LOBBY_PENDING 16
#define BROKER_P2P_PORT_BASE 48000
#define BROKER_LOCAL_PACKETS 32
#define BROKER_LOCAL_PACKET 7000

/* A call result or callback made here: its call handle (0 for a callback), callback id and struct. */
typedef struct BrokerLocalResult { uint64_t call; int32_t id; uint32_t size; uint8_t data[32]; } BrokerLocalResult;

typedef struct BrokerLobbies BrokerLobbies;

/* The state for this instance, its lobbies in that folder (made if missing); NULL when the folder cannot be used. */
BrokerLobbies* BrokerLobbiesOpen(const wchar_t* folder, uint64_t self);
/* The state the flat functions below answer from (one per broker; tests switch between two). */
void BrokerLobbiesUse(BrokerLobbies* lobbies);
/* This module's answer for a flat ISteamMatchmaking or ISteamNetworking export (same signature), or NULL for one it
 * does not answer. */
void* BrokerLobbiesResolve(const char* flat);
/* The next call result waiting, then the next LobbyDataUpdate_t (505) for a lobby whose file changed; false when none. */
bool BrokerLobbiesNext(BrokerLobbies* lobbies, BrokerLocalResult* out);
/* Leaves every lobby this instance is in (the broker is ending). */
void BrokerLobbiesClose(BrokerLobbies* lobbies);
