/* The editor's window (app_internal.h): its drawing helpers (chips, buttons, marks, icons) and its list. */
#include <math.h>
#include <algorithm>
#include "app_internal.h"
#include "icons.h"
#include "ui_marks.h"

namespace editorapp
{
Tone ToneOf(const std::string& tone)
{
    if (tone == "offensive") return {C(226, 94, 74, 255), C(226, 94, 74, 40)};
    if (tone == "defensive") return {C(122, 188, 94, 255), C(122, 188, 94, 40)};
    if (tone == "support") return {C(74, 172, 232, 255), C(74, 172, 232, 40)};
    return {0, 0};
}
ImU32 FactionColour(const std::string& faction)
{
    if (faction == "terminids") return C(255, 201, 45, 255);
    if (faction == "automatons") return C(255, 80, 80, 255);
    if (faction == "illuminate") return C(183, 116, 255, 255);
    if (faction == "super_earth") return C(95, 195, 245, 255);
    return 0;
}
const std::string& FactionOf(const Object* object) { return object->faction.empty() ? object->category : object->faction; }
bool EntityPage(const Object* object)
{
    return object->category == "structures" || object->category == "entities" || object->category == "rosters" ||
        object->category == "difficulty" || FactionColour(object->category) != 0;
}

/* A line broken into lines at most max units wide, at word ends (a word wider than that is cut with "..."). */
std::vector<std::string> Wrapped(const std::string& text, float max, float size)
{
    std::vector<std::string> out;
    std::string line;
    size_t i = 0;
    while (i < text.size())
    {
        size_t space = text.find(' ', i);
        std::string word = text.substr(i, space == std::string::npos ? std::string::npos : space - i);
        i = space == std::string::npos ? text.size() : space + 1;
        if (word.empty()) continue;
        std::string longer = line.empty() ? word : line + " " + word;
        if (!line.empty() && g_cv.Measure(longer, size) > max) { out.push_back(line); line = word; }
        else line = longer;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

/* A header's chevron: v open, > closed, in strokes heavy enough to read at a glance. */
void Arrow(float x, float cy, bool down, ImU32 colour)
{
    const float t = 2.2f;
    if (down) g_cv.Chevron(ImVec2(x + 1, cy - 3), ImVec2(x + 6, cy + 2.5f), ImVec2(x + 11, cy - 3), colour, 3, t);
    else g_cv.Chevron(ImVec2(x + 3.5f, cy - 5.5f), ImVec2(x + 9, cy), ImVec2(x + 3.5f, cy + 5.5f), colour, 3, t);
}

/* ---- drawing helpers ---- */
float Chip(const std::string& text, float x, float cy, ImU32 fg, ImU32 bg, bool right, int z)
{
    float w = g_cv.Measure(text, SZ_TINY, ui::TITLE) + 14;
    if (right) x -= w;
    g_cv.Rect(x, cy - 10, w, 20, bg, z);
    g_cv.Text(text, x + 7, cy, T(SZ_TINY, fg, ui::TITLE, ui::LEFT, 0, z + 2));
    return w;
}

bool Button(const std::string& label, const Area& a, Style style, uint64_t id, int z)
{
    bool hovered = g_cv.Hovered() == id;
    ImU32 bg, fg, edge;
    if (style == PRIMARY) { bg = hovered ? PRIMARY_HOVER : GOLD; fg = INVERSE; edge = GOLD; }
    else if (style == DANGER) { bg = hovered ? ERROR_SOFT : PANEL; fg = ERROR_; edge = ERROR_; }
    else if (style == DISABLED) { bg = PANEL; fg = FAINT; edge = LINE_STRONG; }
    else { bg = hovered ? HOVER : PANEL; fg = TEXT; edge = LINE_STRONG; }
    g_cv.Rect(a.x, a.y, a.w, a.h, bg, z);
    g_cv.Frame(a.x, a.y, a.w, a.h, edge, z + 1);
    g_cv.Text(label, a.x + a.w / 2, a.y + a.h / 2, T(SZ_TAB, fg, ui::TITLE, ui::CENTER, 0, z + 3));
    return g_cv.Hit(a.x, a.y, a.w, a.h, id) && style != DISABLED;
}

/* The HD2R Editor's mark (four corner brackets and three slider bars), when HellForge's icon is not there. */
void Logo(float x, float y, float s)
{
    float t = std::max(2.0f, s * 0.13f), l = s * 0.36f;
    g_cv.Rect(x, y, l, t, GOLD, 4); g_cv.Rect(x, y, t, l, GOLD, 4);
    g_cv.Rect(x + s - l, y, l, t, GOLD, 4); g_cv.Rect(x + s - t, y, t, l, GOLD, 4);
    g_cv.Rect(x, y + s - t, l, t, GOLD, 4); g_cv.Rect(x, y + s - l, t, l, GOLD, 4);
    g_cv.Rect(x + s - l, y + s - t, l, t, GOLD, 4); g_cv.Rect(x + s - t, y + s - l, t, l, GOLD, 4);
    float bw = s * 0.5f, bh = std::max(1.5f, s * 0.06f), bx = x + s * 0.25f;
    const float knobs[] = {0.62f, 0.18f, 0.48f};
    for (int i = 0; i < 3; i++)
    {
        float by = y + s * (0.3f + 0.2f * i);
        g_cv.Rect(bx, by - bh / 2, bw, bh, GOLD, 4);
        g_cv.Rect(bx + bw * knobs[i] - s * 0.06f, by - s * 0.07f, s * 0.12f, s * 0.14f, GOLD, 5);
    }
}

static bool DrawMark(int mark, float x, float y, float size, ImU32 colour)
{
    const icons::Icon* icon = icons::Mark(mark);
    if (icon == nullptr || !icons::Texture()) return false;
    g_cv.Image(icons::Texture(), ImVec2(x, y), ImVec2(size, size), icon->uv0, icon->uv1, colour, 5);
    return true;
}

/* The markers right to left from x (an error: red exclamation piece; pending: orange hourglass piece; edited by you:
 * yellow pencil piece; set by a mod: blue piece), centred on cy; returns the x left of them. */
float DrawMarks(const Marks& m, float x, float cy, float size)
{
    float step = floorf(size * 0.62f + 0.5f);
    bool first = true;
    auto one = [&](int mark, ImU32 colour) {
        float left = x - size + (first ? floorf(size * 0.22f) : 0);
        if (!DrawMark(mark, left, cy - size / 2, size, colour))
        {
            float dot = floorf(size * 0.4f);
            g_cv.Rect(left + (size - dot) / 2, cy - dot / 2, dot, dot, colour, 4);
        }
        x = left + size - step;
        first = false;
    };
    if (m.error) one(UI_MARK_ERROR, ERROR_);
    if (m.pending) one(UI_MARK_PENDING, PENDING);
    if (m.editor) one(UI_MARK_EDITED, GOLD);
    if (m.mod) one(UI_MARK_MOD, MOD);
    if (m.lua) one(UI_MARK_MOD, LUA);
    return first ? x : x - floorf(size * 0.2f);
}

/* An icon fitted into the square at (x, y), its aspect kept. */
static void FitIcon(const icons::Icon* icon, ImU32 colour, int layer, float x, float y, float size)
{
    float aspect = (float)icon->width / (float)icon->height, w = size, h = size;
    if (aspect > 1) h = size / aspect; else w = size * aspect;
    g_cv.Image(icons::Texture(), ImVec2(x + (size - w) / 2, y + (size - h) / 2), ImVec2(w, h), icon->uv0, icon->uv1, colour, layer);
}

/* A stratagem's icon as the game draws it: its category layer in its category's colour, its white layer white. */
static bool DrawStratagemIcon(const Object* object, ImU32 colour, float x, float y, float size, int z)
{
    const std::string& name = object->sprite.empty() ? object->name : object->sprite; /* a mod's copy: its page's */
    const icons::Icon* layer = icons::Find(icons::STRATAGEM, name);
    const icons::Icon* white = icons::Find(icons::STRATAGEM_WHITE, name);
    if (layer == nullptr || white == nullptr) return false;
    FitIcon(layer, colour, z, x, y, size);
    FitIcon(white, C(255, 255, 238, 255), z + 1, x, y, size);
    return true;
}

/* The one icon of a mission difficulty, a mod's own image, a booster or passive, a weapon or throwable (nullptr when it
 * is not there), and the colour it is drawn in; false when the object is none of those. */
static bool KindIcon(const Object* object, const icons::Icon** icon, ImU32* colour)
{
    if (object->icon == "difficulty") { *icon = icons::Find(icons::DIFFICULTY, object->sprite); *colour = IM_COL32_WHITE; return true; }
    if (object->icon == "mod") { *icon = icons::Find(icons::MOD, object->sprite); *colour = IM_COL32_WHITE; return true; }
    if (object->icon == "booster" || object->icon == "passive")
    {
        *icon = icons::Find(object->icon == "booster" ? icons::BOOSTER : icons::PASSIVE, object->sprite.empty() ? object->name : object->sprite);
        *colour = object->icon == "booster" ? IM_COL32_WHITE : PERK;
        return true;
    }
    if (object->icon == "weapon" || object->icon == "throwable")
    {
        *icon = icons::Find(object->icon == "throwable" ? icons::THROWABLE : icons::WEAPON, object->name);
        *colour = ICON;
        return true;
    }
    return false;
}

/* An object's icon, fitted into a square: the game's HUD silhouette of a weapon or throwable; a stratagem's icon as the
 * game draws it (its category layer in the category's colour, its white layer white); a booster as it is; an entity its
 * faction's emblem in the faction's colour. A stratagem with no icon is a plain square in its colour (never made-up art).
 * False when there is nothing to draw. */
bool DrawIcon(const Object* object, float x, float y, float size, int z)
{
    if (!icons::Texture()) return false;
    Tone tone = ToneOf(object->tone);
    if (object->icon == "stratagem") return DrawStratagemIcon(object, tone.colour ? tone.colour : GOLD, x, y, size, z);
    const icons::Icon* icon;
    ImU32 colour;
    if (KindIcon(object, &icon, &colour))
    {
        if (icon == nullptr) return false;
        FitIcon(icon, colour, z, x, y, size);
        return true;
    }
    if (ImU32 faction = FactionColour(FactionOf(object)))
    {
        icon = icons::Find(icons::FACTION, FactionOf(object));
        if (icon == nullptr) return false;
        FitIcon(icon, faction, z, x, y, size);
        return true;
    }
    if (tone.colour) { g_cv.Rect(x + size * 0.18f, y + size * 0.18f, size * 0.64f, size * 0.64f, tone.colour, z); return true; }
    return false;
}

static uint64_t RowId(const std::string& key, int i) { return ui::Id(key, (uint64_t)i + 1); }

/* The selection kept in view: scrolled to whenever it changed (the list's own scroll left alone otherwise). */
static void KeepInView(const ListOpts& o, int& scroll, int rows)
{
    if (o.selected < 0) return;
    auto seen = g_seen.find(o.key);
    if (seen != g_seen.end() && seen->second == o.selected) return;
    g_seen[o.key] = o.selected;
    if (o.selected < scroll) scroll = o.selected;
    if (o.selected >= scroll + rows) scroll = o.selected - rows + 1;
}

/* the scrollbar being dragged */
static void DragScroll(const ListOpts& o, int& scroll, int maxScroll)
{
    if (g_drag.key != o.key) return;
    if (!ImGui::IsMouseDown(0)) { g_drag.key.clear(); return; }
    float ux, uy;
    g_cv.ToUnits(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, &ux, &uy);
    float f = (uy - g_drag.trackY - g_drag.thumb / 2) / std::max(1.0f, g_drag.trackH - g_drag.thumb);
    scroll = (int)floorf(std::clamp(f, 0.0f, 1.0f) * maxScroll + 0.5f);
}

/* The scrollbar of a list longer than its rows: drawn thin, grabbed wide. */
static void Scrollbar(const ListOpts& o, int scroll, int rows, int maxScroll)
{
    float trackY = o.y + 2, trackH = o.h - 4;
    float thumb = std::max(24.0f, trackH * rows / o.count), t = (float)scroll / std::max(1, maxScroll);
    bool dragging = g_drag.key == o.key;
    g_cv.Rect(o.x + o.w - 4, trackY, 3, trackH, LINE, o.layer);
    g_cv.Rect(o.x + o.w - (dragging ? 6 : 4), trackY + (trackH - thumb) * t, dragging ? 5.0f : 3.0f, thumb, (o.focused || dragging) ? GOLD : LINE_STRONG, o.layer + 1);
    if (g_cv.Hit(o.x + o.w - 14, o.y, 14, o.h, ui::Id(o.key, 8888)))
    {
        g_drag.key = o.key; g_drag.trackY = trackY; g_drag.trackH = trackH; g_drag.thumb = thumb;
    }
}

void List(const ListOpts& o)
{
    int rows = std::max(1, (int)floorf(o.h / o.rowH));
    int& scroll = g_scroll[o.key];
    KeepInView(o, scroll, rows);
    int maxScroll = std::max(0, o.count - rows);
    float wheel = g_cv.Scroll(o.x, o.y, o.w, o.h, ui::Id(o.key, 7777));
    if (wheel != 0) scroll -= (int)(wheel * 3);
    DragScroll(o, scroll, maxScroll);
    scroll = std::clamp(scroll, 0, maxScroll);
    for (int i = scroll; i < std::min(o.count, scroll + rows); i++)
    {
        float y = o.y + (i - scroll) * o.rowH;
        uint64_t id = RowId(o.key, i);
        o.draw(i, o.x, y, o.w, o.rowH, g_cv.Hovered() == id);
        if (o.click && g_cv.Hit(o.x, y, o.w, o.rowH, id)) o.click(i);
        if (o.rightClick && g_cv.RightClicked(id)) o.rightClick(i);
        if (o.after) o.after(i, o.x, y, o.w, o.rowH);
    }
    if (o.count > rows) Scrollbar(o, scroll, rows, maxScroll);
}
} // namespace editorapp
