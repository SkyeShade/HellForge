/* The tactical map screen (M) of larger maps: the game-free half. tacmap_terrain.c turns the level's heights into the
 * map's picture and tacmap_draw.c draws the screen from a snapshot; neither reads game memory, so both build into an
 * offline test as they are. The loader's half (difficulty_tacmap.c) fills the snapshot on the game thread. */
#pragma once
#include <stdint.h>
#include "hellforge_overlay.h"

#define TAC_PLAYERS 8
#define TAC_MARKS 512
#define TAC_ZONES 128

/* Marker kinds, in the order they win a spot on the map (a lower one is left out where it would overlap). */
enum
{
    TAC_PRIMARY,
    TAC_EXTRACT,
    TAC_OBJECTIVE,
    TAC_ENEMY,
    TAC_SIDE,
    TAC_POI,
    TAC_KINDS
};

typedef struct TacPlayer
{
    float x, y;       /* world metres, x east, y north */
    float yaw;        /* degrees, 0 north, 90 east */
    uint8_t present;  /* a player in this slot */
    uint8_t life;     /* 0 alive, 1 downed, 2 dead */
    uint8_t local;    /* this machine's player */
    uint8_t waypoint; /* has a waypoint at wx, wy */
    float wx, wy;
    char name[48];    /* UTF-8 */
} TacPlayer;

typedef struct TacMark
{
    float x, y;
    uint8_t kind;    /* TAC_... */
    uint8_t done;    /* completed or cleared: drawn dimmed */
    uint8_t unknown; /* not discovered yet: drawn as "?", as the game's own map does */
} TacMark;

typedef struct TacZone
{
    float x, y, radius; /* metres */
    float cleared;      /* 0 .. 1 */
} TacZone;

typedef struct TacSnapshot
{
    uint32_t level;     /* changes with each level, so pictures are rebuilt */
    float size;         /* the world's side S in metres */
    float playable;     /* the playable circle's radius in metres, 0 unknown */
    uint32_t players, marks, zones;
    TacPlayer player[TAC_PLAYERS];
    TacMark mark[TAC_MARKS];
    TacZone zone[TAC_ZONES];
    char title[64];     /* "PLANET 114", shown in the header */
    const struct TacHeights* heights; /* the cursor's elevation; NULL none */
} TacSnapshot;

/* The level's heights: columns x rows f32 metres, row 0 at +y, sample (c, r) at x = -S/2 + c S/(columns - 1),
 * y = S/2 - r S/(rows - 1) (the loader's height plan, g_near.global). */
struct TacHeights
{
    const float* data;
    uint32_t columns, rows;
    float size;
    float low, high;    /* the 1st and 99th percentile, for the tint (TacHeightRange) */
};
typedef struct TacHeights TacHeights;

/* One map picture: centred on the world point (cx, cy) at metres_per_px, width x height RGBA8 (rows from the top). */
typedef struct TacPictureRequest
{
    float cx, cy, metres_per_px;
    uint32_t width, height;
    float minor, major; /* the contour intervals in metres */
    uint32_t serial;    /* the screen's number for this request (TacScreenPicture) */
} TacPictureRequest;

void TacHeightRange(TacHeights* h);
/* Renders the requested map picture into rgba (width x height x 4 bytes). Outside the world is a dark hatch. */
void TacTerrain(const TacHeights* h, const TacPictureRequest* request, uint8_t* rgba);
float TacHeightAt(const TacHeights* h, float x, float y); /* bilinear, NaN outside */

/* The screen. TacScreenDraw is the HfScreen draw callback's body; the loader passes the latest snapshot. */
typedef struct TacScreen TacScreen;
typedef struct TacHost
{
    /* Places this machine's waypoint (the game's ping) at a world point; called from the render thread, so the loader
     * queues it for the game thread. remove = 1: take the current one away. */
    void (*ping)(void* user, float x, float y, int remove);
    /* Builds the map picture for a view on a worker thread and hands it back through TacScreenPicture. */
    void (*picture)(void* user, const TacPictureRequest* request);
    void* user;
} TacHost;

TacScreen* TacScreenNew(const TacHost* host);
int TacScreenDraw(TacScreen* screen, const TacSnapshot* snap, const HfDraw* draw, const HfInput* input, float dt);
void TacScreenOpened(TacScreen* screen, int open);
/* A finished picture of the host's request; the screen copies it into its image on the next draw. */
void TacScreenPicture(TacScreen* screen, const TacPictureRequest* request, const uint8_t* rgba);
