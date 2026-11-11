/* The tactical map screen (tacmap.h), drawn through the overlay host's HfDraw. Layout, colours, symbols, declutter
 * and the open transition follow the audited mockups (KB xl-tactical-map-spec); positions are in 1080p units, the map
 * viewport stretching with wider windows. Game-free: everything comes from the snapshot. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tacmap.h"

/* The game's own HUD colours (sampled from its Tab map) and the editor's text colour. */
#define C_YELLOW HF_RGBA(255, 233, 0, 255)
#define C_RED HF_RGBA(221, 83, 84, 255)
#define C_CYAN HF_RGBA(113, 246, 255, 255)
#define C_WHITE HF_RGBA(234, 234, 228, 255)
#define C_GREY HF_RGBA(150, 150, 140, 255)
#define C_DIM HF_RGBA(96, 98, 92, 255)
#define C_SLATE HF_RGBA(17, 20, 22, 255)
#define C_PANEL HF_RGBA(12, 14, 16, 255)
#define C_INK HF_RGBA(20, 20, 18, 255)
#define C_BLACK HF_RGBA(0, 0, 0, 255)
/* Teammates' slot colours, a colour-blind-safe order; yellow is only ever the local player's. */
static const HfColour kTacSlots[TAC_PLAYERS] = {
    HF_RGBA(86, 180, 233, 255), HF_RGBA(230, 159, 0, 255), HF_RGBA(0, 158, 115, 255), HF_RGBA(204, 121, 167, 255),
    HF_RGBA(160, 160, 255, 255), HF_RGBA(213, 94, 0, 255), HF_RGBA(140, 220, 200, 255), HF_RGBA(255, 255, 255, 255)};

#define OPEN_S 0.40f
#define CLOSE_S 0.25f
#define PANEL_X0 24.0f
#define PANEL_X1 348.0f
#define BOX_X0 372.0f
#define BOX_Y0 76.0f
#define BOX_Y1 1022.0f

typedef struct Picture
{
    HfImage* image;
    uint32_t width, height;
    float cx, cy, mpp;      /* the world it shows */
    uint8_t ready;
} Picture;

struct TacScreen
{
    TacHost host;
    float cx, cy, mpp;      /* the view: centre, metres per unit */
    uint8_t fitted;         /* the view was set for this level */
    uint32_t level;
    float t;                /* seconds into the open (counts down while closing) */
    uint8_t dragging;
    float dragX, dragY;
    float settle;           /* seconds the view has not changed (for a sharp picture) */
    float lastCx, lastCy, lastMpp;
    float askedCx, askedCy, askedMpp; /* the last sharp picture asked for */
    Picture world, view;
    uint32_t serial, worldSerial, viewSerial;
    SRWLOCK lock;           /* the pending picture, from the worker */
    uint8_t* pending;
    uint32_t pendingSerial, pendingWidth, pendingHeight;
    float pendingCx, pendingCy, pendingMpp;
};

/* ---- helpers --------------------------------------------------------------------------------------------------- */

static HfColour Alpha(HfColour c, float a)
{
    float base = (float)(c >> 24);
    uint32_t na = (uint32_t)(base * (a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a) + 0.5f);
    return (c & 0x00FFFFFFu) | (na << 24);
}

static float Clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

typedef struct View
{
    float x0, y0, x1, y1;   /* the viewport, units */
    float cx, cy, mpp;
} View;

static void ToScreen(const View* v, float x, float y, float* u, float* w)
{
    *u = v->x0 + (v->x1 - v->x0) * 0.5f + (x - v->cx) / v->mpp;
    *w = v->y0 + (v->y1 - v->y0) * 0.5f - (y - v->cy) / v->mpp;
}

static void ToWorld(const View* v, float u, float w, float* x, float* y)
{
    *x = v->cx + (u - v->x0 - (v->x1 - v->x0) * 0.5f) * v->mpp;
    *y = v->cy - (w - v->y0 - (v->y1 - v->y0) * 0.5f) * v->mpp;
}

/* 1 km squares: column A..P from the west edge, row 01..16 from the north; fine adds the 100 m digits east and south. */
static void GridRef(float size, float x, float y, int fine, char* out, size_t room)
{
    float gx = (x + size * 0.5f) / 1000.0f, gy = (size * 0.5f - y) / 1000.0f;
    int c = (int)floorf(gx), r = (int)floorf(gy);
    int cc = c < 0 ? 0 : c > 25 ? 25 : c, rr = r < 0 ? 0 : r > 98 ? 98 : r;
    if (fine) snprintf(out, room, "%c%02d %d%d", 'A' + cc, rr + 1, (int)((gx - (float)c) * 10.0f), (int)((gy - (float)r) * 10.0f));
    else snprintf(out, room, "%c%02d", 'A' + cc, rr + 1);
}

typedef struct TacStyle { float size; uint32_t face, align; HfColour colour; } TacStyle;

static void Text(const HfDraw* d, float x, float y, const char* s, TacStyle t)
{
    d->text(x + 1.0f, y + 1.0f, s, t.size, t.face, t.align, Alpha(C_BLACK, (float)(t.colour >> 24) / 255.0f));
    d->text(x, y, s, t.size, t.face, t.align, t.colour);
}

static void Hexagon(const HfDraw* d, float u, float v, float r, HfColour outline, HfColour fill, float width)
{
    HfPoint p[6];
    for (int i = 0; i < 6; i++)
    {
        float a = (60.0f * (float)i + 30.0f) * 3.14159265f / 180.0f;
        p[i] = (HfPoint){u + r * cosf(a), v + r * sinf(a)};
    }
    d->polygon(p, 6, fill);
    d->lines(p, 6, outline, width, 1);
}

/* The game's enemy-installation mark: a four-point star (concave, drawn as four triangles). */
static void Star(const HfDraw* d, float u, float v, float r, HfColour c)
{
    for (int i = 0; i < 4; i++)
    {
        float a = (90.0f * (float)i - 90.0f) * 3.14159265f / 180.0f;
        float b = a + 3.14159265f / 4.0f, e = a - 3.14159265f / 4.0f;
        HfPoint t[4] = {{u + r * cosf(a), v + r * sinf(a)}, {u + 0.34f * r * cosf(b), v + 0.34f * r * sinf(b)}, {u, v},
                        {u + 0.34f * r * cosf(e), v + 0.34f * r * sinf(e)}};
        d->polygon(t, 4, c);
    }
}

static void Chevron(const HfDraw* d, float u, float v, float yaw, float size, HfColour c)
{
    float a = yaw * 3.14159265f / 180.0f;
    float fx = sinf(a), fy = -cosf(a), sx = cosf(a), sy = sinf(a);
    HfPoint tip = {u + fx * size, v + fy * size};
    HfPoint l = {u - fx * size * 0.7f + sx * size * 0.75f, v - fy * size * 0.7f + sy * size * 0.75f};
    HfPoint n = {u - fx * size * 0.25f, v - fy * size * 0.25f};
    HfPoint r = {u - fx * size * 0.7f - sx * size * 0.75f, v - fy * size * 0.7f - sy * size * 0.75f};
    HfPoint left[3] = {tip, l, n}, right[3] = {tip, n, r}, all[4] = {tip, l, n, r};
    d->polygon(left, 3, c);
    d->polygon(right, 3, c);
    d->lines(all, 4, Alpha(C_BLACK, (float)(c >> 24) / 255.0f), 1.2f, 1);
}

static void Pin(const HfDraw* d, float u, float v, HfColour c, int outline_only)
{
    float r = 9.0f;
    HfPoint tri[3] = {{u - r * 0.62f, v - 1.15f * r}, {u + r * 0.62f, v - 1.15f * r}, {u, v}};
    if (outline_only)
    {
        d->circle(u, v - 1.6f * r, r, 0, c, 2.0f);
        HfPoint p[3] = {{u - r * 0.6f, v - 1.2f * r}, {u, v}, {u + r * 0.6f, v - 1.2f * r}};
        d->lines(p, 3, c, 2.0f, 0);
        return;
    }
    HfColour ink = Alpha(C_BLACK, (float)(c >> 24) / 255.0f);
    d->polygon(tri, 3, c);
    d->circle(u, v - 1.6f * r, r, c, ink, 1.0f);
    d->circle(u, v - 1.6f * r, r * 0.35f, ink, 0, 0.0f);
}

static void Dashed(const HfDraw* d, float ax, float ay, float bx, float by, HfColour c)
{
    float len = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    if (len < 1.0f) return;
    for (float s = 0.0f; s < len; s += 14.0f)
    {
        float e = s + 8.0f < len ? s + 8.0f : len;
        HfPoint p[2] = {{ax + (bx - ax) * s / len, ay + (by - ay) * s / len}, {ax + (bx - ax) * e / len, ay + (by - ay) * e / len}};
        d->lines(p, 2, c, 2.0f, 0);
    }
}

/* ---- labels ------------------------------------------------------------------------------------------------------ */

#define PLACE_MAX 1024
typedef struct Placer
{
    float box[PLACE_MAX][4];
    int n;
    float bounds[4];
} Placer;

static void Take(Placer* p, float x0, float y0, float x1, float y1)
{
    if (p->n < PLACE_MAX) { p->box[p->n][0] = x0; p->box[p->n][1] = y0; p->box[p->n][2] = x1; p->box[p->n][3] = y1; p->n++; }
}

static int Hits(const Placer* p, const float b[4])
{
    int n = 0;
    for (int i = 0; i < p->n; i++)
        if (b[0] < p->box[i][2] && p->box[i][0] < b[2] && b[1] < p->box[i][3] && p->box[i][1] < b[3]) n++;
    return n;
}

typedef struct Placed { float u, v, r; uint8_t kind, badge, done, unknown; } Placed;

/* What the parts of Symbols share for one frame. */
typedef struct TacSymbols
{
    const HfDraw* d;
    const View* v;
    const TacSnapshot* s;
    Placer* place;
    Placed* placed;         /* the markers that got a spot, n of them */
    int n, zoomed, local;   /* local: this machine's player, -1 none */
    float a, mu, mv;        /* a: how far the map is revealed; mu, mv: the local player on screen */
    float pu[TAC_PLAYERS], pv[TAC_PLAYERS]; /* every player on screen */
} TacSymbols;

/* A label at the first free spot of eight around its symbol, on a dark plate so grid lines and hatching do not run
 * through the text. */
static void Label(const TacSymbols* c, float u, float v, float r, const char* s, HfColour colour)
{
    const HfDraw* d = c->d; Placer* p = c->place;
    const float size = 14.0f, h = 14.0f, g = 5.0f;
    float w = d->text_width(s, size, HF_FACE_TITLE);
    float spots[8][2] = {{u + r + g, v}, {u - r - g - w, v}, {u + r + g, v - r - h / 2}, {u + r + g, v + r + h / 2},
                         {u - w / 2, v - r - g - h / 2}, {u - w / 2, v + r + g + h / 2}, {u - r - g - w, v - r - h / 2},
                         {u - r - g - w, v + r + h / 2}};
    int best = -1, bestHits = 0;
    float bestBox[4] = {0};
    for (int i = 0; i < 8; i++)
    {
        float b[4] = {spots[i][0] - 2.0f, spots[i][1] - h / 2, spots[i][0] + w + 2.0f, spots[i][1] + h / 2};
        if (b[0] < p->bounds[0] || b[2] > p->bounds[2] || b[1] < p->bounds[1] || b[3] > p->bounds[3]) continue;
        int n = Hits(p, b);
        if (best < 0 || n < bestHits) { best = i; bestHits = n; memcpy(bestBox, b, sizeof b); }
        if (n == 0) break;
    }
    if (best < 0) return;
    Take(p, bestBox[0], bestBox[1], bestBox[2], bestBox[3]);
    d->rect(bestBox[0], bestBox[1], bestBox[2], bestBox[3], Alpha(C_PANEL, 0.59f * c->a), 0, 0.0f);
    Text(d, spots[best][0], spots[best][1], s, (TacStyle){size, HF_FACE_TITLE, HF_ALIGN_LEFT, Alpha(colour, c->a)});
}

/* ---- the map ----------------------------------------------------------------------------------------------------- */

/* The part of the world in the viewport: metres (xa .. xb, ya .. yb) and where that is on screen. */
typedef struct TacGridBox { float xa, xb, ya, yb, left, top, right, bottom; } TacGridBox;

static void TacGridLines(const HfDraw* d, const View* v, const TacGridBox* b, float size, float step, float a)
{
    int lines = (int)(size / step + 0.5f), per = (int)(1000.0f / step + 0.5f);
    for (int k = 0; k <= lines; k++)
    {
        int km = per > 0 && k % per == 0;
        HfColour c = Alpha(C_WHITE, (km ? 70.0f : 34.0f) / 255.0f * a);
        float x = -size / 2 + (float)k * step, y = size / 2 - (float)k * step, u, w, width = km ? 1.0f : 0.6f;
        if (x >= b->xa && x <= b->xb)
        {
            ToScreen(v, x, 0, &u, &w);
            HfPoint p[2] = {{u, b->top}, {u, b->bottom}};
            d->lines(p, 2, c, width, 0);
        }
        if (y >= b->ya && y <= b->yb)
        {
            ToScreen(v, 0, y, &u, &w);
            HfPoint p[2] = {{b->left, w}, {b->right, w}};
            d->lines(p, 2, c, width, 0);
        }
    }
}

static void TacGridLabels(const HfDraw* d, const View* v, const TacGridBox* b, float size, float a)
{
    HfColour lc = Alpha(HF_RGBA(205, 205, 196, 255), a);
    for (int c = 0; c < (int)(size / 1000.0f); c++)
    {
        float u, w;
        char s[4] = {(char)('A' + (c < 26 ? c : 25)), 0};
        ToScreen(v, -size / 2 + ((float)c + 0.5f) * 1000.0f, 0, &u, &w);
        if (u > fmaxf(b->left, v->x0) + 12.0f && u < b->right - 12.0f) Text(d, u, fmaxf(b->top, v->y0) + 10.0f, s, (TacStyle){13.0f, HF_FACE_TITLE, HF_ALIGN_CENTRE, lc});
    }
    for (int r = 0; r < (int)(size / 1000.0f); r++)
    {
        float u, w;
        char s[8];
        snprintf(s, sizeof s, "%02d", r + 1);
        ToScreen(v, 0, size / 2 - ((float)r + 0.5f) * 1000.0f, &u, &w);
        if (w > fmaxf(b->top, v->y0) + 20.0f && w < b->bottom - 12.0f) Text(d, fmaxf(b->left, v->x0) + 7.0f, w, s, (TacStyle){13.0f, HF_FACE_TITLE, HF_ALIGN_LEFT, lc});
    }
}

static void Grid(const HfDraw* d, const View* v, float size, float step, float a)
{
    float wx0, wy1, wx1, wy0;
    ToWorld(v, v->x0, v->y0, &wx0, &wy1);
    ToWorld(v, v->x1, v->y1, &wx1, &wy0);
    TacGridBox b = {fmaxf(wx0, -size / 2), fminf(wx1, size / 2), fmaxf(wy0, -size / 2), fminf(wy1, size / 2)};
    if (b.xa >= b.xb || b.ya >= b.yb) return;
    ToScreen(v, b.xa, b.yb, &b.left, &b.top);
    ToScreen(v, b.xb, b.ya, &b.right, &b.bottom);
    TacGridLines(d, v, &b, size, step, a);
    TacGridLabels(d, v, &b, size, a);
}

/* Enemy territory: a red tint, a hatch and a red edge per area; it shrinks as the area is cleared, as the game's own. */
static void Territory(const HfDraw* d, const View* v, const TacZone* z, float a)
{
    float u, w;
    ToScreen(v, z->x, z->y, &u, &w);
    float r = z->radius * (1.0f - Clamp01(z->cleared)) / v->mpp;
    if (r < 2.0f || u + r < v->x0 || u - r > v->x1 || w + r < v->y0 || w - r > v->y1) return;
    d->circle(u, w, r, Alpha(C_RED, 0.18f * a), 0, 0.0f);
    /* hatch: lines at 45 degrees every 7.5 units, cut to the circle */
    for (float k = -r * 1.4142f; k <= r * 1.4142f; k += 7.5f)
    {
        float dd = k / 1.4142f;
        if (fabsf(dd) >= r) continue;
        float half = sqrtf(r * r - dd * dd);
        /* the line x - y = k through the centre's frame: points (t + k/2, t - k/2) rotated */
        float px = u + dd * 0.7071f, py = w - dd * 0.7071f;
        HfPoint p[2] = {{px - half * 0.7071f, py - half * 0.7071f}, {px + half * 0.7071f, py + half * 0.7071f}};
        d->lines(p, 2, Alpha(C_RED, 0.30f * a), 2.2f, 0);
    }
    d->circle(u, w, r, 0, Alpha(C_RED, 0.75f * a), 1.5f);
}

static const float kKindRadius[TAC_KINDS][2] = {{9, 15}, {11, 16}, {9, 15}, {8, 13}, {5, 7}, {4, 6}};

static void Marker(const HfDraw* d, const Placed* m, float a)
{
    float u = m->u, v = m->v, r = m->r;
    int kind = m->kind;
    HfColour ink = Alpha(C_INK, a);
    if (m->unknown)
    {
        d->circle(u, v, r, ink, Alpha(C_GREY, a), 1.5f);
        d->text(u, v, "?", r * 1.4f, HF_FACE_TITLE, HF_ALIGN_CENTRE, Alpha(C_GREY, a));
        return;
    }
    float fade = m->done ? 0.45f : 1.0f;
    switch (kind)
    {
    case TAC_PRIMARY:
    case TAC_OBJECTIVE:
    {
        HfColour c = Alpha(kind == TAC_PRIMARY ? C_YELLOW : C_WHITE, a * fade);
        Hexagon(d, u, v, r, c, ink, kind == TAC_PRIMARY ? 2.5f : 1.5f);
        d->circle(u, v, r * 0.3f, c, 0, 0.0f);
        break;
    }
    case TAC_EXTRACT:
    {
        HfColour c = Alpha(C_YELLOW, a);
        Hexagon(d, u, v, r, c, ink, 2.5f);
        HfPoint t[3] = {{u, v - r * 0.55f}, {u - r * 0.45f, v + r * 0.05f}, {u + r * 0.45f, v + r * 0.05f}};
        d->polygon(t, 3, c);
        d->rect(u - r * 0.16f, v, u + r * 0.16f, v + r * 0.5f, c, 0, 0.0f);
        break;
    }
    case TAC_ENEMY: Star(d, u, v, r, Alpha(C_RED, a * fade)); break;
    case TAC_SIDE:
        d->circle(u, v, r, 0, Alpha(C_WHITE, a * fade), 1.0f);
        d->circle(u, v, r / 3.0f, Alpha(C_WHITE, a * fade), 0, 0.0f);
        break;
    default:
        d->rect(u - r, v - r, u + r, v + r, 0, Alpha(C_CYAN, a * fade), 1.0f);
        d->rect(u - r / 3, v - r / 3, u + r / 3, v + r / 3, Alpha(C_CYAN, a * fade), 0, 0.0f);
        break;
    }
    if (m->badge) /* an enemy installation at the same spot as this objective */
    {
        float bu = u + r * 0.85f, bv = v - r * 0.85f, br = fmaxf(5.0f, r * 0.55f);
        d->circle(bu, bv, br, ink, 0, 0.0f);
        Star(d, bu, bv, br * 0.95f, Alpha(C_RED, a));
    }
}

static HfColour PlayerColour(const TacSnapshot* s, uint32_t i)
{
    return s->player[i].local ? C_YELLOW : s->player[i].life >= 2 ? C_GREY : kTacSlots[i % TAC_PLAYERS];
}

/* This machine's player in the snapshot, -1 none. */
static int TacLocalPlayer(const TacSnapshot* s)
{
    int local = -1;
    for (uint32_t i = 0; i < s->players; i++)
        if (s->player[i].present && s->player[i].local) local = (int)i;
    return local;
}

static int TacAlive(const TacPlayer* p) { return p->present && p->life < 2; }
static int TacInside(const View* v, float u, float w) { return u >= v->x0 && u <= v->x1 && w >= v->y0 && w <= v->y1; }
static int TacOutside(const View* v, float u, float w) { return u < v->x0 || u > v->x1 || w < v->y0 || w > v->y1; }

/* Where the segment a -> b leaves the viewport (inset): 1 with that point in *e, 0 with b itself when b is inside. */
static int EdgePoint(const View* v, HfPoint a, HfPoint b, float inset, HfPoint* e)
{
    float x0 = v->x0 + inset, y0 = v->y0 + inset, x1 = v->x1 - inset, y1 = v->y1 - inset, t = 1.0f;
    *e = b;
    if (b.x >= x0 && b.x <= x1 && b.y >= y0 && b.y <= y1) return 0;
    if (b.x < x0) t = fminf(t, (x0 - a.x) / (b.x - a.x));
    if (b.x > x1) t = fminf(t, (x1 - a.x) / (b.x - a.x));
    if (b.y < y0) t = fminf(t, (y0 - a.y) / (b.y - a.y));
    if (b.y > y1) t = fminf(t, (y1 - a.y) / (b.y - a.y));
    *e = (HfPoint){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    return 1;
}

/* Under the markers: the view cone (a fixed screen size), the line to your waypoint and the cursor's pin. */
static void TacSymbolsUnder(const TacSymbols* c, const float* cursor)
{
    const HfDraw* d = c->d; const TacSnapshot* s = c->s;
    if (c->local >= 0 && s->player[c->local].life < 2)
    {
        float yaw = s->player[c->local].yaw * 3.14159265f / 180.0f, reach = c->zoomed ? 90.0f : 34.0f;
        HfPoint cone[10];
        cone[0] = (HfPoint){c->mu, c->mv};
        for (int k = 0; k < 9; k++)
        {
            float b = yaw + (-30.0f + 7.5f * (float)k) * 3.14159265f / 180.0f;
            cone[k + 1] = (HfPoint){c->mu + sinf(b) * reach, c->mv - cosf(b) * reach};
        }
        d->polygon(cone, 10, Alpha(C_YELLOW, 0.235f * c->a));
    }
    for (uint32_t i = 0; i < s->players; i++)
    {
        const TacPlayer* p = &s->player[i];
        HfPoint from, to, e;
        if (!p->present || !p->waypoint || !p->local) continue; /* only your own waypoint gets the line from you */
        ToScreen(c->v, p->wx, p->wy, &to.x, &to.y);
        ToScreen(c->v, p->x, p->y, &from.x, &from.y);
        EdgePoint(c->v, from, to, 26.0f, &e);
        Dashed(d, from.x, from.y, e.x, e.y, Alpha(C_YELLOW, 0.67f * c->a));
    }
    if (cursor != NULL) Pin(d, cursor[0], cursor[1], Alpha(C_YELLOW, 0.8f * c->a), 1);
}

/* The placed marker that one of radius r at (u, w) would overlap, NULL none. */
static Placed* TacClash(const TacSymbols* c, float u, float w, float r)
{
    for (int k = 0; k < c->n; k++)
    {
        const Placed* q = &c->placed[k];
        float dist = sqrtf((u - q->u) * (u - q->u) + (w - q->v) * (w - q->v));
        if (dist < r + q->r + (c->zoomed ? 2.0f : 3.0f) && (!c->zoomed || dist < 0.6f * (r + q->r))) return &c->placed[k];
    }
    return NULL;
}

static void TacPlaceMark(TacSymbols* c, const TacMark* m, int kind)
{
    const View* v = c->v;
    float u, w;
    ToScreen(v, m->x, m->y, &u, &w);
    if (u < v->x0 + 12 || u > v->x1 - 12 || w < v->y0 + 12 || w > v->y1 - 12) return;
    float r = kKindRadius[kind][c->zoomed ? 1 : 0];
    Placed* clash = TacClash(c, u, w, r);
    if (clash != NULL)
    {
        if (kind == TAC_ENEMY && !m->unknown && (clash->kind == TAC_OBJECTIVE || clash->kind == TAC_PRIMARY)) clash->badge = 1;
        return;
    }
    if (c->n < TAC_MARKS) c->placed[c->n++] = (Placed){u, w, r, (uint8_t)kind, 0, m->done, m->unknown};
}

/* Markers by priority; at world zoom a lower one that would overlap a placed one is left out, except an enemy
 * installation on an objective, which becomes the objective's badge. */
static void TacPlaceMarks(TacSymbols* c)
{
    for (int kind = 0; kind < TAC_KINDS; kind++)
        for (uint32_t i = 0; i < c->s->marks; i++)
            if (c->s->mark[i].kind == kind) TacPlaceMark(c, &c->s->mark[i], kind);
}

static void TacWaypointPins(const TacSymbols* c)
{
    const View* v = c->v;
    for (uint32_t i = 0; i < c->s->players; i++)
    {
        const TacPlayer* p = &c->s->player[i];
        float u, w;
        if (!p->present || !p->waypoint) continue;
        ToScreen(v, p->wx, p->wy, &u, &w);
        if (u > v->x0 + 26 && u < v->x1 - 26 && w > v->y0 + 26 && w < v->y1 - 26)
        {
            Pin(c->d, u, w, Alpha(PlayerColour(c->s, i), c->a), 0);
            Take(c->place, u - 9, w - 24, u + 9, w);
        }
    }
}

/* The mark of a player killed in action. */
static void TacCross(const HfDraw* d, float u, float w, float half, float a)
{
    HfPoint a1[2] = {{u - half, w - half}, {u + half, w + half}}, a2[2] = {{u - half, w + half}, {u + half, w - half}};
    d->lines(a1, 2, Alpha(C_GREY, a), 3.0f, 0);
    d->lines(a2, 2, Alpha(C_GREY, a), 3.0f, 0);
}

/* The other players' markers, then yours on top. */
static void TacPlayerMarks(const TacSymbols* c)
{
    const HfDraw* d = c->d; const TacSnapshot* s = c->s;
    float a = c->a, mu = c->mu, mv = c->mv;
    for (uint32_t i = 0; i < s->players; i++)
    {
        const TacPlayer* p = &s->player[i];
        float u = c->pu[i], w = c->pv[i];
        if (!p->present || (int)i == c->local || TacOutside(c->v, u, w)) continue;
        if (p->life >= 2) TacCross(d, u, w, 7, a);
        else Chevron(d, u, w, p->yaw, 10.0f, Alpha(PlayerColour(s, i), a * (p->life == 1 ? 0.6f : 1.0f)));
        Take(c->place, u - 10, w - 10, u + 10, w + 10);
    }
    if (c->local >= 0 && TacInside(c->v, mu, mv))
    {
        d->circle(mu, mv, 15.0f, Alpha(C_INK, 0.67f * a), Alpha(C_YELLOW, a), 2.0f);
        Chevron(d, mu, mv, s->player[c->local].yaw, 12.0f, Alpha(C_YELLOW, a));
        Take(c->place, mu - 15, mv - 15, mu + 15, mv + 15);
    }
}

static void TacGroupMerge(int* group, uint32_t count, int from, int to)
{
    for (uint32_t k = 0; k < count; k++)
        if (group[k] == from) group[k] = to;
}

/* Living players within 26 units of each other share one label: group[i] is player i's group. */
static void TacGroupPlayers(const TacSymbols* c, int* group)
{
    const TacSnapshot* s = c->s;
    for (uint32_t i = 0; i < s->players; i++) group[i] = (int)i;
    for (uint32_t i = 0; i < s->players; i++)
        for (uint32_t j = i + 1; j < s->players; j++)
        {
            if (!TacAlive(&s->player[i]) || !TacAlive(&s->player[j])) continue;
            if (hypotf(c->pu[i] - c->pu[j], c->pv[i] - c->pv[j]) >= 26.0f) continue;
            TacGroupMerge(group, s->players, group[j], group[i]);
        }
}

/* A group's living members: how many, the first two of them and whether you are one. */
static int TacGroupMembers(const TacSymbols* c, const int* group, int g, int* who, int* has_local)
{
    int members = 0;
    who[0] = who[1] = -1; *has_local = 0;
    for (uint32_t i = 0; i < c->s->players; i++)
    {
        if (!TacAlive(&c->s->player[i]) || group[i] != g) continue;
        if (members < 2) who[members] = (int)i;
        members++;
        if (c->s->player[i].local) *has_local = 1;
    }
    return members;
}

static void TacGroupLabel(const TacSymbols* c, const int* group, int g, int pass)
{
    const TacSnapshot* s = c->s;
    char text[160] = "";
    int who[2], has_local, members = TacGroupMembers(c, group, g, who, &has_local);
    if (members == 0 || has_local != (pass == 0)) return;
    int at = has_local ? c->local : who[0];
    if (TacOutside(c->v, c->pu[at], c->pv[at])) return;
    const char* head = has_local ? "YOU" : s->player[at].name;
    float r = has_local ? 16.0f : 11.0f;
    if (members == 1) { Label(c, c->pu[at], c->pv[at], r, head, PlayerColour(s, (uint32_t)at)); return; }
    /* real names are long: a pair is named in full, a larger group as its first name and a count */
    if (members == 2) snprintf(text, sizeof text, "%s + %s", head, s->player[who[0] != at ? who[0] : who[1]].name);
    else snprintf(text, sizeof text, "%s + %d", head, members - 1);
    Label(c, c->pu[at], c->pv[at], r, text, has_local ? C_YELLOW : C_WHITE);
}

static void TacPlayerLabels(const TacSymbols* c)
{
    const TacSnapshot* s = c->s;
    int group[TAC_PLAYERS];
    TacGroupPlayers(c, group);
    for (int pass = 0; pass < 2; pass++) /* the group with you first */
        for (uint32_t g = 0; g < s->players; g++) TacGroupLabel(c, group, (int)g, pass);
    for (uint32_t i = 0; i < s->players; i++)
        if (s->player[i].present && s->player[i].life >= 2 && TacInside(c->v, c->pu[i], c->pv[i]))
        {
            char text[80];
            snprintf(text, sizeof text, "%s  KIA", s->player[i].name);
            Label(c, c->pu[i], c->pv[i], 8.0f, text, C_GREY);
        }
}

/* A waypoint's label, or an arrow on the edge pointing at yours when it is off screen. */
static void TacWaypointLabel(const TacSymbols* c, uint32_t i)
{
    const HfDraw* d = c->d; const View* v = c->v; const TacSnapshot* s = c->s;
    const TacPlayer* p = &s->player[i];
    if (!p->present || !p->waypoint) return;
    HfPoint at, e, from = {v->x0 + (v->x1 - v->x0) / 2, v->y0 + (v->y1 - v->y0) / 2};
    float ox = p->x, oy = p->y;
    char ref[16], text[96];
    ToScreen(v, p->wx, p->wy, &at.x, &at.y);
    if (c->local >= 0) { ox = s->player[c->local].x; oy = s->player[c->local].y; from = (HfPoint){c->mu, c->mv}; }
    float km = hypotf(p->wx - ox, p->wy - oy) / 1000.0f;
    GridRef(s->size, p->wx, p->wy, 1, ref, sizeof ref);
    const char* who = p->local ? "WAYPOINT" : p->name;
    HfColour colour = PlayerColour(s, i);
    if (!EdgePoint(v, from, at, 26.0f, &e))
    {
        snprintf(text, sizeof text, "%s  %s  %.2f KM", who, ref, km);
        Label(c, at.x, at.y - 14.0f, 11.0f, text, colour);
    }
    else if (p->local)
    {
        float ang = atan2f(at.y - c->mv, at.x - c->mu);
        HfPoint t[3] = {{e.x + cosf(ang) * 12, e.y + sinf(ang) * 12}, {e.x + cosf(ang + 2.5f) * 10, e.y + sinf(ang + 2.5f) * 10},
                        {e.x + cosf(ang - 2.5f) * 10, e.y + sinf(ang - 2.5f) * 10}};
        d->polygon(t, 3, Alpha(colour, c->a));
        d->lines(t, 3, Alpha(C_BLACK, c->a), 1.0f, 1);
        Take(c->place, e.x - 12, e.y - 12, e.x + 12, e.y + 12);
        snprintf(text, sizeof text, "%s  %.2f KM", who, km);
        Label(c, e.x, e.y, 13.0f, text, colour);
    }
}

static void Symbols(const HfDraw* d, const View* v, const TacSnapshot* s, int zoomed, float a, const float* cursor)
{
    static Placer place;
    static Placed placed[TAC_MARKS + 1];
    TacSymbols c = {d, v, s, &place, placed, 0, zoomed, TacLocalPlayer(s), a};
    place.n = 0;
    place.bounds[0] = v->x0 + 4.0f; place.bounds[1] = v->y0 + 22.0f; place.bounds[2] = v->x1 - 4.0f; place.bounds[3] = v->y1 - 4.0f;
    if (c.local >= 0) ToScreen(v, s->player[c.local].x, s->player[c.local].y, &c.mu, &c.mv);
    for (uint32_t i = 0; i < s->players; i++) ToScreen(v, s->player[i].x, s->player[i].y, &c.pu[i], &c.pv[i]);
    TacSymbolsUnder(&c, cursor);
    TacPlaceMarks(&c);
    for (int k = 0; k < c.n; k++) Take(&place, placed[k].u - placed[k].r, placed[k].v - placed[k].r, placed[k].u + placed[k].r, placed[k].v + placed[k].r);
    for (int k = 0; k < c.n; k++) Marker(d, &placed[k], a);
    /* players: everyone's waypoint pin, then markers, yours last; then the labels, most important first */
    TacWaypointPins(&c);
    TacPlayerMarks(&c);
    for (int k = 0; k < c.n; k++)
        if ((placed[k].kind == TAC_PRIMARY || placed[k].kind == TAC_EXTRACT) && !placed[k].unknown)
            Label(&c, placed[k].u, placed[k].v, placed[k].r, placed[k].kind == TAC_PRIMARY ? "PRIMARY" : "EXTRACTION", C_YELLOW);
    TacPlayerLabels(&c);
    for (uint32_t i = 0; i < s->players; i++) TacWaypointLabel(&c, i);
}

/* ---- chrome ------------------------------------------------------------------------------------------------------ */

static void Corners(const HfDraw* d, float x0, float y0, float x1, float y1, HfColour c)
{
    const float arm = 26.0f;
    HfPoint p[4][3] = {{{x0, y0 + arm}, {x0, y0}, {x0 + arm, y0}}, {{x1 - arm, y0}, {x1, y0}, {x1, y0 + arm}},
                       {{x0, y1 - arm}, {x0, y1}, {x0 + arm, y1}}, {{x1 - arm, y1}, {x1, y1}, {x1, y1 - arm}}};
    for (int i = 0; i < 4; i++) d->lines(p[i], 3, c, 3.0f, 0);
}

static void ScaleBar(const HfDraw* d, const View* v, float a)
{
    static const float nice[] = {50, 100, 250, 500, 1000, 2000, 5000};
    float metres = nice[0];
    for (int i = 0; i < 7; i++)
        if (nice[i] / v->mpp <= 150.0f) metres = nice[i];
    float x1 = v->x1 - 30.0f, y = v->y1 - 26.0f, len = metres / v->mpp, x0 = x1 - len;
    d->rect(x0 - 10, y - 30, x1 + 10, y + 10, Alpha(C_PANEL, 0.78f * a), 0, 0.0f);
    for (int i = 0; i < 4; i++)
        d->rect(x0 + len * (float)i / 4, y - 4, x0 + len * (float)(i + 1) / 4, y + 2,
                Alpha(i % 2 == 0 ? C_WHITE : HF_RGBA(40, 40, 40, 255), a), Alpha(C_WHITE, a), 1.0f);
    char s[16];
    Text(d, x0, y - 14, "0", (TacStyle){13.0f, HF_FACE_TITLE, HF_ALIGN_CENTRE, Alpha(C_WHITE, a)});
    if (metres / 2 < 1000) snprintf(s, sizeof s, "%d M", (int)(metres / 2)); else snprintf(s, sizeof s, "%g KM", metres / 2000);
    Text(d, x0 + len / 2, y - 14, s, (TacStyle){13.0f, HF_FACE_TITLE, HF_ALIGN_CENTRE, Alpha(C_WHITE, a)});
    if (metres < 1000) snprintf(s, sizeof s, "%d M", (int)metres); else snprintf(s, sizeof s, "%g KM", metres / 1000);
    Text(d, x1, y - 14, s, (TacStyle){13.0f, HF_FACE_TITLE, HF_ALIGN_CENTRE, Alpha(C_WHITE, a)});
}

static void Compass(const HfDraw* d, const View* v, float a)
{
    float u = v->x1 - 54.0f, w = v->y0 + 60.0f, r = 26.0f;
    d->circle(u, w, r, Alpha(C_PANEL, 0.78f * a), Alpha(C_GREY, a), 1.0f);
    HfPoint n[3] = {{u, w - r + 4}, {u - 7, w + 2}, {u + 7, w + 2}}, s[3] = {{u, w + r - 4}, {u - 7, w + 2}, {u + 7, w + 2}};
    d->polygon(n, 3, Alpha(C_YELLOW, a));
    d->polygon(s, 3, Alpha(C_DIM, a));
    Text(d, u, w - r - 10, "N", (TacStyle){13.0f, HF_FACE_TITLE, HF_ALIGN_CENTRE, Alpha(C_YELLOW, a)});
}

static void Ratio(float mpp, char* out, size_t room)
{
    /* the map scale on a 24-inch 1080p screen (0.277 mm a pixel), two significant digits */
    double r = mpp / 0.000277;
    double e = pow(10.0, floor(log10(r)) - 1.0);
    long v = (long)(floor(r / e + 0.5) * e);
    char digits[32];
    snprintf(digits, sizeof digits, "%ld", v);
    size_t len = strlen(digits), o = 0;
    o += (size_t)snprintf(out, room, "1 : ");
    for (size_t i = 0; i < len && o + 2 < room; i++)
    {
        out[o++] = digits[i];
        if ((len - 1 - i) % 3 == 0 && i + 1 < len) out[o++] = ' ';
    }
    out[o] = 0;
}

/* The roster names: shortened with an ellipsis to fit. */
static void Fit(const HfDraw* d, const char* name, float size, float room, char* out, size_t cap)
{
    snprintf(out, cap, "%s", name);
    while (strlen(out) > 1 && d->text_width(out, size, HF_FACE_BODY) > room)
    {
        size_t n = strlen(out);
        if (n > 4 && strcmp(out + n - 3, "...") == 0) n -= 3;
        do n--; while (n > 0 && (out[n] & 0xC0) == 0x80); /* whole UTF-8 characters */
        snprintf(out + n, cap - n, "...");
    }
}

typedef struct TacPanel { const HfDraw* d; const TacSnapshot* s; float x0, x1, a; } TacPanel;

/* One squad row at y: the slot colour, the name, and the grid square with the distance from you. */
static void TacPanelRow(const TacPanel* q, uint32_t i, int local, float y)
{
    const HfDraw* d = q->d; const TacSnapshot* s = q->s; const TacPlayer* p = &s->player[i];
    float x0 = q->x0, x1 = q->x1, a = q->a;
    HfColour c = PlayerColour(s, i);
    d->rect(x0 + 14, y + 4, x0 + 18, y + 26, Alpha(c, a), 0, 0.0f);
    char ref[16], info[48], name[64];
    GridRef(s->size, p->x, p->y, 0, ref, sizeof ref);
    if (p->life >= 2) snprintf(info, sizeof info, "KIA   %s", ref);
    else if (p->local || local < 0) snprintf(info, sizeof info, "%s", ref);
    else snprintf(info, sizeof info, "%s   %.1f KM", ref, hypotf(p->x - s->player[local].x, p->y - s->player[local].y) / 1000.0f);
    float iw = d->text_width(info, 13.0f, HF_FACE_TITLE);
    Fit(d, p->local ? "YOU" : p->name, 15.0f, (x1 - 14 - iw - 12) - (x0 + 28), name, sizeof name);
    Text(d, x0 + 28, y + 15, name, (TacStyle){15.0f, HF_FACE_BODY, HF_ALIGN_LEFT, Alpha(p->life >= 2 ? C_GREY : C_WHITE, a)});
    Text(d, x1 - 14, y + 15, info, (TacStyle){13.0f, HF_FACE_TITLE, HF_ALIGN_RIGHT, Alpha(C_GREY, a)});
}

/* The squad list from y0; returns the y below its last row. */
static float TacPanelSquad(const TacPanel* q, float y0)
{
    const HfDraw* d = q->d; const TacSnapshot* s = q->s;
    float x0 = q->x0, x1 = q->x1, a = q->a;
    d->rect(x0, y0, x0 + 4, y0 + 40, Alpha(C_YELLOW, a), 0, 0.0f);
    Text(d, x0 + 16, y0 + 20, "SQUAD", (TacStyle){18.0f, HF_FACE_TITLE, HF_ALIGN_LEFT, Alpha(C_WHITE, a)});
    uint32_t present = 0, alive = 0;
    for (uint32_t i = 0; i < s->players; i++) { present += s->player[i].present != 0; alive += (uint32_t)TacAlive(&s->player[i]); }
    char t[96];
    snprintf(t, sizeof t, "%u / %u ACTIVE", alive, present);
    Text(d, x1 - 14, y0 + 20, t, (TacStyle){13.0f, HF_FACE_TITLE, HF_ALIGN_RIGHT, Alpha(C_GREY, a)});
    int local = TacLocalPlayer(s);
    float y = y0 + 48;
    for (uint32_t i = 0; i < s->players; i++)
        if (s->player[i].present) { TacPanelRow(q, i, local, y); y += 32; }
    return y;
}

static void TacLegendIcon(const TacPanel* q, int k, float u, float w)
{
    const HfDraw* d = q->d; float a = q->a;
    switch (k)
    {
    case 0: Marker(d, &(Placed){u, w, 9, TAC_PRIMARY}, a); break;
    case 1: Marker(d, &(Placed){u, w, 9, TAC_OBJECTIVE}, a); break;
    case 2: Marker(d, &(Placed){u, w, 10, TAC_EXTRACT}, a); break;
    case 3: Marker(d, &(Placed){u, w, 10, TAC_ENEMY}, a); break;
    case 4:
        d->rect(u - 10, w - 8, u + 10, w + 8, Alpha(HF_RGBA(70, 34, 34, 255), a), Alpha(C_RED, a), 2.0f);
        for (int j = -2; j <= 2; j++)
        {
            HfPoint h[2] = {{u + (float)j * 5 - 4, w + 8}, {u + (float)j * 5 + 4, w - 8}};
            d->lines(h, 2, Alpha(HF_RGBA(150, 60, 60, 255), a), 1.0f, 0);
        }
        break;
    case 5: Marker(d, &(Placed){u, w, 6, TAC_POI}, a); break;
    case 6: Marker(d, &(Placed){u, w, 7, TAC_SIDE}, a); break;
    case 7: Pin(d, u, w + 10, Alpha(C_YELLOW, a), 0); break;
    case 8: Chevron(d, u, w, 0, 11, Alpha(C_YELLOW, a)); break;
    case 9:
        for (int j = 0; j < 3; j++) Chevron(d, u - 9 + 9 * (float)j, w, 0, 7, Alpha(kTacSlots[j + 1], a));
        break;
    default: TacCross(d, u, w, 6, a); break;
    }
}

static void TacPanelLegend(const TacPanel* q, float y)
{
    const HfDraw* d = q->d; float x0 = q->x0, a = q->a;
    d->rect(x0, y, x0 + 4, y + 40, Alpha(C_YELLOW, a), 0, 0.0f);
    Text(d, x0 + 16, y + 20, "LEGEND", (TacStyle){18.0f, HF_FACE_TITLE, HF_ALIGN_LEFT, Alpha(C_WHITE, a)});
    y += 52;
    static const char* const rows[] = {"PRIMARY OBJECTIVE", "OBJECTIVE", "EXTRACTION", "ENEMY INSTALLATION", "ENEMY TERRITORY",
                                       "POINT OF INTEREST", "SIDE SITE", "WAYPOINT", "YOU (VIEW DIRECTION)",
                                       "SQUAD (COLOUR AS IN LIST)", "KILLED IN ACTION"};
    int primary = 0;
    for (uint32_t i = 0; i < q->s->marks; i++) primary |= q->s->mark[i].kind == TAC_PRIMARY;
    for (int k = primary ? 0 : 1; k < 11; k++)
    {
        float u = x0 + 30, w = y + 12;
        TacLegendIcon(q, k, u, w);
        Text(d, x0 + 56, w, rows[k], (TacStyle){15.0f, HF_FACE_BODY, HF_ALIGN_LEFT, Alpha(C_WHITE, a)});
        y += 30;
    }
}

/* The readout at the panel's foot: the cursor's grid reference and elevation, and the map scale. */
static void TacPanelReadout(const TacPanel* q, const View* v, const float* cursorWorld, float y)
{
    const HfDraw* d = q->d; const TacSnapshot* s = q->s;
    float x0 = q->x0, x1 = q->x1, a = q->a;
    HfPoint foot[2] = {{x0 + 14, y}, {x1 - 14, y}};
    d->lines(foot, 2, Alpha(C_GREY, 0.35f * a), 1.0f, 0);
    char ref[24] = "-", elev[24] = "-", scale[32];
    if (cursorWorld != NULL)
    {
        GridRef(s->size, cursorWorld[0], cursorWorld[1], 1, ref, sizeof ref);
        float h = s->heights != NULL ? TacHeightAt(s->heights, cursorWorld[0], cursorWorld[1]) : NAN;
        if (!isnan(h)) snprintf(elev, sizeof elev, "%d M", (int)lroundf(h));
    }
    Ratio(v->mpp, scale, sizeof scale);
    const char* names[3] = {"CURSOR", "ELEVATION", "SCALE"};
    const char* values[3] = {ref, elev, scale};
    for (int k = 0; k < 3; k++)
    {
        Text(d, x0 + 16, y + 22 + 30 * (float)k, names[k], (TacStyle){13.0f, HF_FACE_TITLE, HF_ALIGN_LEFT, Alpha(C_GREY, a)});
        Text(d, x1 - 14, y + 22 + 30 * (float)k, values[k], (TacStyle){15.0f, HF_FACE_BODY, HF_ALIGN_RIGHT, Alpha(C_WHITE, a)});
    }
}

static void Panel(const HfDraw* d, const TacSnapshot* s, const View* v, const float* cursorWorld, float a, float dx)
{
    const TacPanel q = {d, s, PANEL_X0 - dx, PANEL_X1 - dx, a};
    d->rect(q.x0, BOX_Y0, q.x1, BOX_Y1, Alpha(C_PANEL, 0.925f * a), Alpha(C_GREY, 0.35f * a), 1.0f);
    float y = TacPanelSquad(&q, BOX_Y0) + 18;
    HfPoint rule[2] = {{q.x0 + 14, y}, {q.x1 - 14, y}};
    d->lines(rule, 2, Alpha(C_GREY, 0.35f * a), 1.0f, 0);
    TacPanelLegend(&q, y + 14);
    TacPanelReadout(&q, v, cursorWorld, BOX_Y1 - 118);
}

static void HeaderFooter(const HfDraw* d, const TacSnapshot* s, const View* v, float a)
{
    float W = d->width, H = 1080.0f;
    d->rect(0, 0, W, 64, Alpha(C_PANEL, 0.96f * a), 0, 0.0f);
    HfPoint rule[2] = {{0, 64}, {W, 64}};
    d->lines(rule, 2, Alpha(C_YELLOW, 0.78f * a), 2.0f, 0);
    for (int i = 0; i < 3; i++) /* Super Earth framing: a yellow chevron band */
    {
        float x = 24.0f + 14.0f * (float)i;
        HfPoint top[4] = {{x, 18}, {x + 8, 18}, {x + 16, 32}, {x + 8, 32}}, bottom[4] = {{x + 8, 32}, {x + 16, 32}, {x + 8, 46}, {x, 46}};
        d->polygon(top, 4, Alpha(C_YELLOW, a));
        d->polygon(bottom, 4, Alpha(C_YELLOW, a));
    }
    Text(d, 90, 32, "TACTICAL MAP", (TacStyle){26.0f, HF_FACE_TITLE, HF_ALIGN_LEFT, Alpha(C_WHITE, a)});
    char sub[160];
    float across = (v->x1 - v->x0) * v->mpp / 1000.0f;
    const char* sep = s->title[0] ? "  //  " : "";
    if (across >= s->size / 1000.0f * 0.95f)
        snprintf(sub, sizeof sub, "%s%sOPERATION AREA %.1f x %.1f KM", s->title, sep, s->size / 1000.0f, s->size / 1000.0f);
    else snprintf(sub, sizeof sub, "%s%s%.1f KM ACROSS", s->title, sep, across);
    Text(d, 350, 33, sub, (TacStyle){15.0f, HF_FACE_BODY, HF_ALIGN_LEFT, Alpha(C_GREY, a)});
    Text(d, W - 24, 33, "SUPER EARTH ARMED FORCES  //  SURVEY GRID 1 KM", (TacStyle){13.0f, HF_FACE_TITLE, HF_ALIGN_RIGHT, Alpha(C_GREY, a)});
    d->rect(0, H - 44, W, H, Alpha(C_PANEL, 0.96f * a), 0, 0.0f);
    HfPoint frule[2] = {{0, H - 44}, {W, H - 44}};
    d->lines(frule, 2, Alpha(C_GREY, 0.31f * a), 1.0f, 0);
    static const char* const keys[][2] = {{"M / ESC", "CLOSE"}, {"DRAG", "PAN"}, {"WHEEL", "ZOOM"}, {"RMB", "SET WAYPOINT"},
                                          {"DEL", "REMOVE WAYPOINT"}, {"SPACE", "CENTRE ON YOU"}};
    float x = 24.0f;
    for (int i = 0; i < 6; i++)
    {
        float kw = d->text_width(keys[i][0], 13.0f, HF_FACE_TITLE) + 14.0f;
        d->rect(x, H - 33, x + kw, H - 11, 0, Alpha(C_WHITE, a), 1.0f);
        d->text(x + kw / 2, H - 22, keys[i][0], 13.0f, HF_FACE_TITLE, HF_ALIGN_CENTRE, Alpha(C_WHITE, a));
        x += kw + 8.0f;
        d->text(x, H - 22, keys[i][1], 13.0f, HF_FACE_TITLE, HF_ALIGN_LEFT, Alpha(C_GREY, a));
        x += d->text_width(keys[i][1], 13.0f, HF_FACE_TITLE) + 26.0f;
    }
}

/* ---- pictures ---------------------------------------------------------------------------------------------------- */

void TacScreenPicture(TacScreen* sc, const TacPictureRequest* q, const uint8_t* rgba)
{
    uint8_t* copy = (uint8_t*)malloc((size_t)q->width * q->height * 4);
    if (copy == NULL) return;
    memcpy(copy, rgba, (size_t)q->width * q->height * 4);
    AcquireSRWLockExclusive(&sc->lock);
    free(sc->pending);
    sc->pending = copy;
    sc->pendingSerial = q->serial; sc->pendingWidth = q->width; sc->pendingHeight = q->height;
    sc->pendingCx = q->cx; sc->pendingCy = q->cy; sc->pendingMpp = q->metres_per_px;
    ReleaseSRWLockExclusive(&sc->lock);
}

static void TakePicture(TacScreen* sc, const HfDraw* d)
{
    AcquireSRWLockExclusive(&sc->lock);
    uint8_t* px = sc->pending;
    uint32_t serial = sc->pendingSerial, w = sc->pendingWidth, h = sc->pendingHeight;
    float cx = sc->pendingCx, cy = sc->pendingCy, mpp = sc->pendingMpp;
    sc->pending = NULL;
    ReleaseSRWLockExclusive(&sc->lock);
    if (px == NULL) return;
    Picture* p = serial == sc->worldSerial ? &sc->world : serial == sc->viewSerial ? &sc->view : NULL;
    if (p != NULL)
    {
        if (p->image != NULL && (p->width != w || p->height != h)) { d->image_free(p->image); p->image = NULL; }
        if (p->image == NULL) p->image = d->image_create(w, h, px);
        else d->image_update(p->image, px);
        p->width = w; p->height = h; p->cx = cx; p->cy = cy; p->mpp = mpp; p->ready = p->image != NULL;
    }
    free(px);
}

static void DrawPicture(const HfDraw* d, const View* v, const Picture* p, HfColour tint)
{
    if (!p->ready) return;
    float x0, y0, x1, y1;
    ToScreen(v, p->cx - (float)p->width * 0.5f * p->mpp, p->cy + (float)p->height * 0.5f * p->mpp, &x0, &y0);
    ToScreen(v, p->cx + (float)p->width * 0.5f * p->mpp, p->cy - (float)p->height * 0.5f * p->mpp, &x1, &y1);
    const HfRect box = {x0, y0, x1, y1};
    d->image(p->image, &box, NULL, tint);
}

static void Contours(float mpp, float* minor, float* major)
{
    *minor = mpp < 4.0f ? 5.0f : mpp < 9.0f ? 10.0f : 20.0f;
    *major = *minor * 5.0f;
}

/* ---- the screen -------------------------------------------------------------------------------------------------- */

TacScreen* TacScreenNew(const TacHost* host)
{
    TacScreen* sc = (TacScreen*)calloc(1, sizeof *sc);
    if (sc == NULL) return NULL;
    sc->host = *host;
    InitializeSRWLock(&sc->lock);
    return sc;
}

void TacScreenOpened(TacScreen* sc, int open)
{
    if (open) { sc->t = 0.0f; sc->dragging = 0; }
}

static float FitMpp(float size) { return size / (BOX_Y1 - BOX_Y0 - 40.0f); }

static float TacClamp(float v, float low, float high) { return v < low ? low : v > high ? high : v; }

/* Zooms about the cursor: the world point under it stays there. */
static void TacInputZoom(TacScreen* sc, const View* v, const HfInput* in, float fit)
{
    float wx, wy;
    ToWorld(v, in->x, in->y, &wx, &wy);
    float m = TacClamp(sc->mpp * powf(0.8f, in->wheel), 1.0f, fit);
    sc->cx = wx - (in->x - v->x0 - (v->x1 - v->x0) * 0.5f) * m;
    sc->cy = wy + (in->y - v->y0 - (v->y1 - v->y0) * 0.5f) * m;
    sc->mpp = m;
}

static void TacInputKeys(TacScreen* sc, const TacSnapshot* s, const HfInput* in, int local)
{
    for (uint32_t k = 0; k < in->key_count && k < 16; k++)
    {
        if (in->keys[k] == VK_DELETE && local >= 0 && s->player[local].waypoint && sc->host.ping != NULL)
            sc->host.ping(sc->host.user, s->player[local].wx, s->player[local].wy, 1);
        if (in->keys[k] == VK_SPACE && local >= 0) { sc->cx = s->player[local].x; sc->cy = s->player[local].y; }
    }
}

static void Input(TacScreen* sc, const TacSnapshot* s, const View* v, const HfInput* in, float fit)
{
    int inside = TacInside(v, in->x, in->y);
    if (in->wheel != 0.0f && inside) TacInputZoom(sc, v, in, fit);
    if ((in->pressed & HF_MOUSE_LEFT) && inside) { sc->dragging = 1; sc->dragX = in->x; sc->dragY = in->y; }
    if (!(in->down & HF_MOUSE_LEFT)) sc->dragging = 0;
    if (sc->dragging)
    {
        sc->cx -= (in->x - sc->dragX) * sc->mpp;
        sc->cy += (in->y - sc->dragY) * sc->mpp;
        sc->dragX = in->x; sc->dragY = in->y;
    }
    if ((in->pressed & HF_MOUSE_RIGHT) && inside && sc->host.ping != NULL)
    {
        float wx, wy;
        ToWorld(v, in->x, in->y, &wx, &wy);
        if (fabsf(wx) <= s->size / 2 && fabsf(wy) <= s->size / 2) sc->host.ping(sc->host.user, wx, wy, 0);
    }
    TacInputKeys(sc, s, in, TacLocalPlayer(s));
    float half = s->size * 0.5f; /* the world's centre stays on screen */
    sc->cx = TacClamp(sc->cx, -half, half);
    sc->cy = TacClamp(sc->cy, -half, half);
}

/* A new level starts with the whole world in view and no pictures. */
static void TacScreenLevel(TacScreen* sc, const TacSnapshot* s, float fit)
{
    if (!sc->fitted || sc->level != s->level)
    {
        sc->cx = 0; sc->cy = 0; sc->mpp = fit; sc->fitted = 1; sc->level = s->level;
        sc->world.ready = sc->view.ready = 0;
        sc->worldSerial = 0;
    }
}

/* Asks the host for pictures: the whole world once per level, and a sharp one of the view when that is zoomed in past
 * the world picture's texels (8 m on a 16 km map), which is what it returns. The sharp one is at screen resolution and
 * asked for once the view has rested 0.15 s, so panning and zooming do not queue a build every frame. */
static int TacScreenAsk(TacScreen* sc, const TacSnapshot* s, const HfDraw* d, const View* v, float fit, float dt)
{
    TacPictureRequest q = {0, 0, s->size / 2048.0f, 2048, 2048, 0, 0, 0};
    if (sc->worldSerial == 0 && sc->host.picture != NULL)
    {
        q.serial = sc->worldSerial = ++sc->serial;
        Contours(fit, &q.minor, &q.major);
        sc->host.picture(sc->host.user, &q);
    }
    if (v->cx != sc->lastCx || v->cy != sc->lastCy || v->mpp != sc->lastMpp) { sc->settle = 0; sc->lastCx = v->cx; sc->lastCy = v->cy; sc->lastMpp = v->mpp; }
    else sc->settle += dt;
    int sharpNeeded = v->mpp < s->size / 2048.0f * 0.8f;
    float pm = v->mpp / (d->pixels > 0.0f ? d->pixels : 1.0f);
    if (sharpNeeded && sc->settle > 0.15f && sc->host.picture != NULL &&
        (sc->askedCx != v->cx || sc->askedCy != v->cy || sc->askedMpp != pm))
    {
        uint32_t w = (uint32_t)((v->x1 - v->x0) * d->pixels), h = (uint32_t)((v->y1 - v->y0) * d->pixels);
        w = w > 2560 ? 2560 : w; h = h > 1600 ? 1600 : h;
        float m = pm * fmaxf((v->x1 - v->x0) * d->pixels / (float)w, (v->y1 - v->y0) * d->pixels / (float)h);
        sc->askedCx = v->cx; sc->askedCy = v->cy; sc->askedMpp = pm;
        sc->viewSerial = ++sc->serial;
        q = (TacPictureRequest){v->cx, v->cy, m, w, h, 0, 0, sc->viewSerial};
        Contours(v->mpp, &q.minor, &q.major);
        sc->host.picture(sc->host.user, &q);
    }
    return sharpNeeded;
}

/* One frame's state that the map and the side panel share. */
typedef struct TacFrame
{
    float reveal, chrome;   /* 0 .. 1: the map's content and its frame coming up */
    int sharp, hover;       /* the view's own picture goes over the world's; the cursor is over the map */
    float cursor[2], cursorWorld[2]; /* set while hover */
} TacFrame;

static void TacCursorTip(const HfDraw* d, const TacSnapshot* s, const HfInput* in, const float* cursorWorld)
{
    char ref[24], tip[64];
    GridRef(s->size, cursorWorld[0], cursorWorld[1], 1, ref, sizeof ref);
    snprintf(tip, sizeof tip, "%s   RMB: SET WAYPOINT", ref);
    float tw = d->text_width(tip, 14.0f, HF_FACE_TITLE);
    HfPoint cross[4][2] = {{{in->x - 14, in->y}, {in->x - 5, in->y}}, {{in->x + 5, in->y}, {in->x + 14, in->y}},
                           {{in->x, in->y - 14}, {in->x, in->y - 5}}, {{in->x, in->y + 5}, {in->x, in->y + 14}}};
    for (int i = 0; i < 4; i++) d->lines(cross[i], 2, C_WHITE, 2.0f, 0);
    d->rect(in->x + 18, in->y + 14, in->x + 30 + tw, in->y + 40, Alpha(C_PANEL, 0.92f), C_YELLOW, 1.0f);
    d->text(in->x + 24, in->y + 27, tip, 14.0f, HF_FACE_TITLE, HF_ALIGN_LEFT, C_WHITE);
}

/* Everything inside the map's viewport; sets the frame's hover and cursor. */
static void TacScreenMap(const TacScreen* sc, const TacSnapshot* s, const HfDraw* d, const HfInput* in, const View* v, TacFrame* f)
{
    float reveal = f->reveal;
    d->clip_push(v->x0, v->y0, v->x1, v->y1);
    d->rect(v->x0, v->y0, v->x1, v->y1, C_SLATE, 0, 0.0f);
    HfColour tint = HF_RGBA(255, 255, 255, (uint32_t)(255.0f * reveal));
    DrawPicture(d, v, &sc->world, tint);
    if (f->sharp) DrawPicture(d, v, &sc->view, tint);
    for (uint32_t i = 0; i < s->zones; i++) Territory(d, v, &s->zone[i], reveal);
    Grid(d, v, s->size, 100.0f / v->mpp >= 12.0f ? 100.0f : 1000.0f, f->chrome);
    if (s->playable > 0.0f) /* the playable area's edge */
    {
        float u, w;
        ToScreen(v, 0, 0, &u, &w);
        d->circle(u, w, s->playable / v->mpp, 0, Alpha(C_RED, 0.55f * reveal), 1.5f);
    }
    f->hover = !in->closing && TacInside(v, in->x, in->y) && !sc->dragging;
    if (f->hover) { f->cursor[0] = in->x; f->cursor[1] = in->y; ToWorld(v, in->x, in->y, &f->cursorWorld[0], &f->cursorWorld[1]); }
    Symbols(d, v, s, v->mpp < 6.0f, reveal, f->hover ? f->cursor : NULL);
    if (f->hover && reveal >= 1.0f) TacCursorTip(d, s, in, f->cursorWorld);
    Compass(d, v, f->chrome);
    ScaleBar(d, v, f->chrome);
    d->clip_pop();
}

/* The side panel slides in from the left as the map comes up. */
static void TacScreenSide(const HfDraw* d, const TacSnapshot* s, const View* v, const TacFrame* f)
{
    float shift = (1.0f - f->reveal) * (1.0f - f->reveal) * 360.0f;
    d->clip_push(0, 66, PANEL_X1 + 12, 1080 - 46);
    Panel(d, s, v, f->hover ? f->cursorWorld : NULL, f->reveal, shift);
    d->clip_pop();
    HeaderFooter(d, s, v, f->reveal);
}

int TacScreenDraw(TacScreen* sc, const TacSnapshot* s, const HfDraw* d, const HfInput* in, float dt)
{
    if (s->size <= 0.0f) return in->closing ? 0 : 1;
    float fit = FitMpp(s->size);
    TacScreenLevel(sc, s, fit);
    /* the open runs 0 -> OPEN_S; closing plays it backwards in CLOSE_S */
    if (in->closing) sc->t -= dt * OPEN_S / CLOSE_S;
    else sc->t = TacClamp(sc->t + dt, 0.0f, OPEN_S); /* held at the end, so a close starts at once */
    if (in->closing && sc->t <= 0.0f) return 0;
    float t = TacClamp(sc->t, 0.0f, OPEN_S);
    View v = {BOX_X0, BOX_Y0, d->width - 24.0f, BOX_Y1, sc->cx, sc->cy, sc->mpp};
    if (!in->closing && t >= 0.2f) Input(sc, s, &v, in, fit);
    v.cx = sc->cx; v.cy = sc->cy; v.mpp = sc->mpp;
    TakePicture(sc, d);
    TacFrame f = {0};
    f.sharp = TacScreenAsk(sc, s, d, &v, fit, dt);
    /* the transition */
    float k = Clamp01(t / 0.12f), opened = Clamp01((t - 0.06f) / 0.14f);
    f.reveal = Clamp01((t - 0.20f) / 0.20f);
    d->backdrop(0.72f * k, 0.8f * k, 6.0f * k);
    if (opened <= 0.0f) return 1;
    float cy = (BOX_Y0 + BOX_Y1) * 0.5f, half = (BOX_Y1 - BOX_Y0) * 0.5f * opened;
    f.chrome = Clamp01(opened * 1.5f);
    if (f.reveal <= 0.0f) d->clip_push(0, cy - half, d->width, cy + half);
    TacScreenMap(sc, s, d, in, &v, &f);
    Corners(d, v.x0, v.y0, v.x1, v.y1, Alpha(C_YELLOW, f.chrome));
    d->rect(v.x0, v.y0, v.x1, v.y1, 0, Alpha(C_GREY, 0.43f * f.chrome), 1.0f);
    if (f.reveal <= 0.0f) d->clip_pop();
    if (f.reveal > 0.0f) TacScreenSide(d, s, &v, &f);
    if (opened < 1.0f)
    {
        HfPoint a[2] = {{0, cy - half}, {d->width, cy - half}}, b[2] = {{0, cy + half}, {d->width, cy + half}};
        d->lines(a, 2, C_YELLOW, 3.0f, 0);
        d->lines(b, 2, C_YELLOW, 3.0f, 0);
    }
    return 1;
}
