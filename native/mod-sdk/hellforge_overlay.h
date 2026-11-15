/* HellForge mod SDK 17: screens drawn over the game (docs/modding/overlay.md).
 *
 * HellForge's overlay host draws them into the game's frames, on Direct3D 11 and 12, in SDR and HDR. A mod adds a screen
 * with add_screen; its key opens and closes it. While a modal screen is open, the mouse and keyboard are the screen's and
 * the game gets none of them. Included by hellforge_mod.h. */
#pragma once
#include <stdint.h>

/* A colour: HF_RGBA(r, g, b, a), each 0..255, in sRGB as on an SDR screen (the host converts it for HDR). */
typedef uint32_t HfColour;
#define HF_RGBA(r, g, b, a) ((HfColour)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r)))

typedef struct HfPoint
{
    float x, y;
} HfPoint;

typedef struct HfRect
{
    float x0, y0, x1, y1;
} HfRect;

/* Text: the game's fonts (HF_FACE_BODY its regular face, HF_FACE_TITLE its medium one), an alignment on x and the line's
 * centre on y. */
#define HF_FACE_BODY 0u
#define HF_FACE_TITLE 1u
#define HF_ALIGN_LEFT 0u
#define HF_ALIGN_CENTRE 1u
#define HF_ALIGN_RIGHT 2u

typedef struct HfImage HfImage; /* a picture the screen made (image_create); the host's until image_free */

/* Drawing, in screen units: the screen is 1080 units high and width units wide (1920 on a 16:9 window), (0, 0) the top
 * left. Only valid inside the screen's draw callback, on the host's render thread. Shapes are anti-aliased; thickness
 * is in units. Everything is drawn in call order, inside the current clip. */
typedef struct HfDraw
{
    uint32_t size;  /* sizeof(HfDraw) of the host; functions are only ever added at the end */
    float width;    /* the screen's width in units */
    float height;   /* always 1080 */
    float pixels;   /* pixels per unit (the window's height / 1080) */

    void (*rect)(float x0, float y0, float x1, float y1, HfColour fill, HfColour outline, float thickness);
    /* An open (closed = 0) or closed line through count points. */
    void (*lines)(const HfPoint* points, uint32_t count, HfColour colour, float thickness, int closed);
    /* A filled polygon; convex ones fill exactly, concave ones are split into triangles by the host. */
    void (*polygon)(const HfPoint* points, uint32_t count, HfColour fill);
    void (*circle)(float x, float y, float radius, HfColour fill, HfColour outline, float thickness);
    /* One line of UTF-8 text with its cap height centred on y; returns its width in units. */
    float (*text)(float x, float y, const char* utf8, float size, uint32_t face, uint32_t align, HfColour colour);
    float (*text_width)(const char* utf8, float size, uint32_t face);

    /* A picture of width x height RGBA8 pixels (rows from the top, sRGB, straight alpha); NULL when it cannot be made.
     * image_update replaces all its pixels. Pictures live until image_free or the session's end. */
    HfImage* (*image_create)(uint32_t width, uint32_t height, const void* rgba);
    void (*image_update)(HfImage* image, const void* rgba);
    void (*image_free)(HfImage* image);
    /* Draws a picture into a box, bilinear, times tint. part is the piece of it to draw in 0 .. 1 of its width and
     * height; NULL is all of it. */
    void (*image)(HfImage* image, const HfRect* box, const HfRect* part, HfColour tint);

    /* Drawing outside a box is cut off until clip_pop; boxes nest (each is cut to the one before). */
    void (*clip_push)(float x0, float y0, float x1, float y1);
    void (*clip_pop)(void);
    /* The game's frame as it was when the screen opened, dimmed (0 none .. 1 black), desaturated (0 .. 1) and blurred
     * (radius in units), over the whole screen. Draws the frame as it is now, dimmed, where the host cannot copy it. */
    void (*backdrop)(float dim, float desaturate, float blur);
    /* For a modal screen that draws its own cursor over part of itself (a map's crosshair): call it every frame the mouse
     * is there, and the mouse pointer is hidden until a frame without the call. Other screens' calls do nothing. Older
     * hosts end at backdrop, so check size first. */
    void (*hide_pointer)(void);
} HfDraw;

/* The mouse and keys this frame, in screen units. */
#define HF_MOUSE_LEFT 1u
#define HF_MOUSE_RIGHT 2u
#define HF_MOUSE_MIDDLE 4u

typedef struct HfInput
{
    uint32_t size;      /* sizeof(HfInput) of the host */
    float x, y;         /* the mouse */
    float wheel;        /* wheel notches since the last frame, + away from the player */
    uint32_t down;      /* HF_MOUSE_... held */
    uint32_t pressed;   /* pressed since the last frame */
    uint32_t released;  /* released since the last frame */
    /* Keys pressed since the last frame, as Windows virtual-key codes (VK_ESCAPE, 'M', VK_DELETE), up to 16. */
    uint32_t keys[16];
    uint32_t key_count;
    /* 1 once the screen was asked to close (its key, Esc on a modal screen, or screen_show): it plays its closing and
     * then returns 0 from draw. The mouse and keys are the game's again from this frame. */
    uint32_t closing;
} HfInput;

/* A screen's flags. */
#define HF_SCREEN_MODAL 1u /* while open, the mouse and keys are the screen's: the game gets none, the cursor is free */

typedef struct HfScreen
{
    uint32_t size;      /* sizeof(HfScreen) */
    const char* name;   /* for logs ("Tactical map"); copied */
    uint32_t key;       /* the virtual key that opens and closes it ('M'), 0 none (open it with screen_show) */
    uint32_t flags;     /* HF_SCREEN_... */
    void* user;
    /* Every frame from its opening until it returns 0, on the host's render thread: draw it. dt is seconds since the
     * last frame. Return 1 to be drawn again; 0 once closing (input->closing) has finished. */
    int (*draw)(void* user, const HfDraw* draw, const HfInput* input, float dt);
    /* Opened (1, before the first draw) or closed (0, after the last), on the host's render thread. Optional. */
    void (*opened)(void* user, int open);
    /* Whether the screen may open now (its key was pressed), on the window's thread; NULL always. A map closed outside
     * a mission, say. */
    int (*can_open)(void* user);
} HfScreen;
