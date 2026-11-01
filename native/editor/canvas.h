/* The editor's drawing surface, a port of the HD2R Editor's ui/canvas.lua over Dear ImGui's draw lists: 1080p units
 * relative to the panel, scaled and snapped to whole pixels, layers (z) drawn in order whatever order things are drawn
 * in, text with its cap height centred on a line (measured with the font's own metrics, and cached), and the clickable
 * regions of the frame, which answer the next frame's clicks (later regions win, as there). */
#pragma once
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "imgui.h"

namespace ui
{
enum Face { BODY, TITLE };
enum Align { LEFT, RIGHT, CENTER };

struct TextOpts
{
    float size = 16;
    ImU32 colour = IM_COL32(234, 234, 228, 255);
    Face face = BODY;
    Align align = LEFT;
    int z = 5;
    float max = 0; /* fit to this width with "..." (0: no limit) */
};

/* The fonts at a pixel scale: the game's own FS Sinclair (body) and FS Sinclair Medium (titles), extracted into the
 * editor's folder (fs-sinclair.ttf, fs-sinclair-medium.ttf), with Windows' Bahnschrift for what they lack (and in their
 * place when they are missing) and the arrows from Segoe UI Symbol. Call between frames; true when the atlas changed. */
void SetFontFolder(const std::string& folder);
bool BuildFonts(float pixelsPerUnit);
float FontScale();

struct Region { float x, y, w, h; uint64_t id; bool scroll; };

class Canvas
{
public:
    float s = 1, ox = 0, oy = 0; /* pixels per unit, the panel's origin in units */
    float width = 1920, height = 1080; /* the screen in units */

    void Begin(ImDrawList* list, float ox, float oy, float s, float screenW, float screenH);
    void End();

    void Rect(float x, float y, float w, float h, ImU32 colour, int z = 0);
    void Frame(float x, float y, float w, float h, ImU32 colour, int z = 0, float t = 1);
    /* A line t units thick, round-capped. */
    void Line(float x1, float y1, float x2, float y2, ImU32 colour, int z = 0, float t = 1);
    /* An open stroke through three points (a chevron), t units thick, its corner mitred sharp, on whole pixels. */
    void Chevron(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 colour, int z = 0, float t = 1);
    float Measure(const std::string& text, float size, Face face = BODY);
    std::string Fit(const std::string& text, float max, float size, Face face = BODY);
    /* Draws text with its cap height centred on cy; returns its width in units. */
    float Text(const std::string& text, float x, float cy, const TextOpts& o);
    /* An image at (at.x, at.y), size.x by size.y units. */
    void Image(ImTextureID texture, ImVec2 at, ImVec2 size, ImVec2 uv0, ImVec2 uv1, ImU32 tint, int z = 4);

    /* A clickable region for this frame (units, panel space); true when the previous frame's regions put the click on it. */
    bool Hit(float x, float y, float w, float h, uint64_t id);
    /* A region the wheel scrolls; returns the wheel's steps for it this frame (0: none). */
    float Scroll(float x, float y, float w, float h, uint64_t id);
    /* Called with the mouse in pixels before drawing: works out what the previous frame's regions say. */
    void Input(float mouseX, float mouseY, bool clicked, bool rightClicked, float wheel);
    uint64_t Hovered() const { return hovered; }
    /* A region took this frame's click (not the clicks that land on nothing, or on the dialog's backdrop). */
    bool Clicked() const { return clickedAny; }
    bool RightClicked(uint64_t id) const { return rightClick == id && id != 0; }
    void ToUnits(float px, float py, float* ux, float* uy) const { *ux = px / s - ox; *uy = py / s - oy; }
    /* Swallows clicks outside a dialog: a region over everything drawn so far. */
    void Block();

private:
    ImDrawList* list = nullptr;
    ImDrawListSplitter splitter;
    std::vector<Region> regions, previous;
    uint64_t hovered = 0, click = 0, rightClick = 0, wheelTarget = 0;
    bool clickedAny = false;
    float wheel = 0;
    std::unordered_map<std::string, float> widths;
    float measuredAt = 0;
    void Layer(int z);
    ImVec2 Px(float x, float y) const;
};

uint64_t Id(const char* text, uint64_t salt = 0);
uint64_t Id(const std::string& text, uint64_t salt = 0);
} // namespace ui
