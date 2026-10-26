/* The editor's window (app_internal.h): the header, BROWSE's panes, the status bar and the footer. */
#include <algorithm>
#include "app_internal.h"
#include "icons.h"

namespace editorapp
{
/* ---- the header ---- */
/* A view's tab (CHANGES with how many there are); a click opens the view. Returns its width. */
static float DrawTab(int v, float tx, float h)
{
    std::string label = VIEW_LABELS[v];
    if (v == CHANGES) { int n = (int)Changes().size(); if (n > 0) label += "  " + std::to_string(n); }
    float w = g_cv.Measure(label, SZ_TAB, ui::TITLE) + 22;
    uint64_t id = ui::Id("tab", (uint64_t)v);
    bool selected = g_view == v;
    if (selected) { g_cv.Rect(tx, h - 3, w, 3, GOLD, 4); g_cv.Rect(tx, 8, w, h - 11, GOLD_WASH, 2); }
    else if (g_cv.Hovered() == id) g_cv.Rect(tx, 8, w, h - 11, HOVER, 2);
    g_cv.Text(label, tx + 11, h / 2 + 1, T(SZ_TAB, selected ? GOLD : DIM, ui::TITLE));
    if (g_cv.Hit(tx, 6, w, h - 6, id)) { g_view = (View)v; g_edit.on = false; if (v == PRESETS) ScanPresets(); if (v == LOGS) LogRead(true); if (v == MODS) ScanMods(true); }
    return w;
}

/* A status chip, right to left from rx, unless it would reach the tabs; true when its action took this frame's click. */
static bool StatusChip(float& rx, float tabsEnd, float h, const std::string& text, ImU32 fg, ImU32 bg, uint64_t action)
{
    float w = g_cv.Measure(text, SZ_TINY, ui::TITLE) + 14;
    if (rx - w < tabsEnd) return false;
    bool clicked = false;
    if (action)
    {
        if (g_cv.Hovered() == action) g_cv.Frame(rx - w - 2, h / 2 - 12, w + 4, 24, fg, 4);
        clicked = g_cv.Hit(rx - w - 4, h / 2 - 14, w + 8, 28, action);
    }
    rx -= Chip(text, rx, h / 2, fg, bg, true) + 8;
    return clicked;
}

/* The session's chip: read-only while playing with a host's stats, else hosting or joined. */
static void SessionChip(float& rx, float tabsEnd, float h)
{
    if (Locked()) StatusChip(rx, tabsEnd, h, std::string("READ-ONLY \xC2\xB7 HOST: ") + (g_session.host[0] ? g_session.host : "A FRIEND"), PENDING, PENDING_SOFT, 0);
    else if (g_session.role == HF_SESSION_HOST)
        StatusChip(rx, tabsEnd, h, g_lobbyStats ? "HOSTING \xC2\xB7 FRIENDS PLAY WITH YOUR STATS" : "HOSTING \xC2\xB7 EVERYONE KEEPS THEIR OWN", MOD, MOD_SOFT, 0);
    else if (g_session.role == HF_SESSION_JOINER)
        StatusChip(rx, tabsEnd, h, std::string("JOINED \xC2\xB7 ") + (g_session.host[0] ? g_session.host : "A FRIEND") + " \xC2\xB7 YOUR OWN STATS", MOD, MOD_SOFT, 0);
}

/* The status chips right to left from the close button, as many as fit before the tabs: the session, the errors (a
 * click opens them), the changes active and pending. */
static void StatusChips(int errors, int active, float tabsEnd, float h)
{
    float rx = PANEL_W - 62;
    SessionChip(rx, tabsEnd, h);
    if (errors > 0 && StatusChip(rx, tabsEnd, h, std::to_string(errors) + (errors == 1 ? " ERROR" : " ERRORS"), ERROR_, ERROR_SOFT, ui::Id("errors"))) OpenErrors();
    if (active > 0) StatusChip(rx, tabsEnd, h, std::to_string(active) + (Locked() ? " ACTIVE (HOST)" : " ACTIVE"), GOLD, GOLD_WASH, 0);
    if (PendingCount() > 0 && !Locked()) StatusChip(rx, tabsEnd, h, std::to_string(PendingCount()) + " PENDING", PENDING, PENDING_SOFT, 0);
}

void DrawHeader(int errors, int active)
{
    float h = HEADER_H;
    g_cv.Rect(0, 0, PANEL_W, h, HEADER, 1);
    g_cv.Rect(0, h - 1, PANEL_W, 1, LINE_STRONG, 2);
    const icons::Icon* logo = icons::Logo();
    if (logo != nullptr && icons::Texture()) g_cv.Image(icons::Texture(), ImVec2(14, 9), ImVec2(40, 40), logo->uv0, logo->uv1, IM_COL32_WHITE, 4);
    else Logo(18, 15, 28);
    float x = 62;
    x += g_cv.Text("HELLFORGE", x, h / 2, T(SZ_TITLE, TEXT, ui::TITLE));
    x += 8 + g_cv.Text("EDITOR", x + 8, h / 2, T(SZ_TITLE, BRAND, ui::TITLE));
    float tx = x + 24;
    for (int v = 0; v < VIEW_COUNT; v++) tx += DrawTab(v, tx, h) + 2;
    float tabsEnd = tx + 8;
    uint64_t closeId = ui::Id("close");
    bool hovered = g_cv.Hovered() == closeId;
    g_cv.Rect(PANEL_W - 50, 12, 36, 34, hovered ? ERROR_SOFT : PANEL, 3);
    g_cv.Frame(PANEL_W - 50, 12, 36, 34, hovered ? ERROR_ : LINE_STRONG, 4);
    g_cv.Text("\xC3\x97", PANEL_W - 32, 29, T(24, hovered ? ERROR_ : DIM, ui::BODY, ui::CENTER, 0, 6));
    if (g_cv.Hit(PANEL_W - 50, 12, 36, 34, closeId)) g_host.close();
    StatusChips(errors, active, tabsEnd, h);
}

/* ---- BROWSE ---- */
/* A selected row in its tone (a stratagem category's colour), else in gold: brighter while its pane has the focus. */
static void ToneSelection(float x, float y, float w, float h, const Tone& tone, bool focused)
{
    g_cv.Rect(x, y, w, h, tone.colour ? tone.soft : focused ? SELECT : GOLD_WASH, 2);
    g_cv.Rect(x, y, 3, h, tone.colour ? tone.colour : focused ? GOLD : GOLD_DIM, 3);
}

/* The category list's rows (1080p units). */
static const float CAT_ROW_H = 31, CAT_HEAD_H = 24, CAT_GAP = 10;
/* Its scrollbar's track: top, height and thumb. */
struct CatTrack { float y, h, thumb; };

/* The category list's height (its groups' headings and categories), and where the selected one starts. */
static float CategoriesHeight(float* selTop)
{
    float total = CAT_GAP;
    for (int i = 0; i < (int)g_cats.size(); i++)
    {
        if (g_cats[i].group) { total += (i > 0 ? CAT_GAP : 0) + CAT_HEAD_H; continue; }
        if (i == g_cat) *selTop = total;
        total += CAT_ROW_H;
    }
    return total + CAT_GAP;
}

/* a category chosen with the keys is kept in view */
static void KeepCategoryInView(float selTop, float h)
{
    static int seen = -1;
    if (seen == g_cat) return;
    seen = g_cat;
    if (selTop - CAT_GAP < g_catScroll) g_catScroll = selTop - CAT_GAP;
    if (selTop + CAT_ROW_H + CAT_GAP > g_catScroll + h) g_catScroll = selTop + CAT_ROW_H + CAT_GAP - h;
}

/* the category list's scrollbar being dragged */
static void DragCategories(const CatTrack& t, float maxScroll)
{
    if (g_drag.key != "cats") return;
    if (!ImGui::IsMouseDown(0)) { g_drag.key.clear(); return; }
    float ux, uy;
    g_cv.ToUnits(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, &ux, &uy);
    g_catScroll = std::clamp((uy - t.y - t.thumb / 2) / std::max(1.0f, t.h - t.thumb), 0.0f, 1.0f) * maxScroll;
}

static void CategoriesScrollbar(float y0, float h, const CatTrack& t, float maxScroll, bool focused)
{
    bool dragging = g_drag.key == "cats";
    g_cv.Rect(CAT_W - 6, t.y, 3, t.h, LINE, 3);
    g_cv.Rect(CAT_W - (dragging ? 8 : 6), t.y + (t.h - t.thumb) * (g_catScroll / maxScroll), dragging ? 5.0f : 3.0f, t.thumb,
        (focused || dragging) ? GOLD : LINE_STRONG, 4);
    if (g_cv.Hit(CAT_W - 16, y0, 14, h, ui::Id("cats", 8888)))
    {
        g_drag.key = "cats"; g_drag.trackY = t.y; g_drag.trackH = t.h; g_drag.thumb = t.thumb;
    }
}

/* A category's row: its tone, how many objects it has, its marks; a click selects it. */
static void DrawCategory(int i, float y, bool focused, const MarkCache& marks)
{
    const CatItem& item = g_cats[i];
    float rh = CAT_ROW_H;
    bool selected = i == g_cat;
    uint64_t id = ui::Id("cat", (uint64_t)i);
    const catalog::Nav& nav = g_catalog.navs[item.nav];
    Tone tone = ToneOf(nav.tone);
    if (selected) ToneSelection(8, y, CAT_W - 16, rh, tone, focused);
    else if (g_cv.Hovered() == id) g_cv.Rect(8, y, CAT_W - 16, rh, HOVER, 2);
    if (tone.colour && !selected) g_cv.Rect(8, y + 9, 3, rh - 18, tone.colour, 3);
    float cw = g_cv.Text(std::to_string(g_catalog.Of(nav.id).size()), CAT_W - 20, y + rh / 2, T(SZ_SMALL, FAINT, ui::BODY, ui::RIGHT));
    /* an enemy faction's emblem left of its count, then the markers */
    float right = CAT_W - 28 - cw;
    const icons::Icon* emblem = FactionColour(nav.id) && icons::Texture() ? icons::Find(icons::FACTION, nav.id) : nullptr;
    if (emblem != nullptr)
    {
        const float size = 20;
        g_cv.Image(icons::Texture(), ImVec2(right - size, y + (rh - size) / 2), ImVec2(size, size), emblem->uv0, emblem->uv1, FactionColour(nav.id), 4);
        right -= size + 6;
    }
    auto found = marks.navs.find(nav.id);
    float mx = DrawMarks(found != marks.navs.end() ? found->second : Marks{}, right, y + rh / 2, 26);
    g_cv.Text(item.label, 22, y + rh / 2, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, std::min(CAT_W - 80, mx - 30)));
    if (g_cv.Hit(8, y, CAT_W - 16, rh, id)) { SelectCategory(i); g_focus = CATEGORIES; }
}

void DrawCategories(float y0, float h)
{
    g_cv.Rect(0, y0, CAT_W, h, PANEL_ALT, 1);
    g_cv.Rect(CAT_W - 1, y0, 1, h, LINE, 2);
    const MarkCache& marks = Markers();
    bool focused = g_focus == CATEGORIES;
    /* Taller than the panel (a short screen, a large UI scale): it scrolls, by the wheel or its scrollbar, and a category
     * chosen with the keys is kept in view. Only whole rows are drawn, as in a List. */
    float selTop = 0, total = CategoriesHeight(&selTop);
    float maxScroll = std::max(0.0f, total - h);
    KeepCategoryInView(selTop, h);
    float wheel = g_cv.Scroll(0, y0, CAT_W, h, ui::Id("cats", 7777));
    if (wheel != 0) g_catScroll -= wheel * CAT_ROW_H * 3;
    float trackH = h - 4;
    CatTrack track{y0 + 2, trackH, std::max(24.0f, trackH * std::min(1.0f, h / std::max(1.0f, total)))};
    DragCategories(track, maxScroll);
    g_catScroll = std::clamp(g_catScroll, 0.0f, maxScroll);
    auto inside = [&](float y, float rowH) { return y >= y0 && y + rowH <= y0 + h; };
    float yy = CAT_GAP - g_catScroll;
    for (int i = 0; i < (int)g_cats.size(); i++)
    {
        if (g_cats[i].group)
        {
            yy += i > 0 ? CAT_GAP : 0;
            if (inside(y0 + yy, CAT_HEAD_H)) g_cv.Text(g_cats[i].label, 18, y0 + yy + 11, T(SZ_HEADING, FAINT, ui::TITLE));
            yy += CAT_HEAD_H;
            continue;
        }
        float y = y0 + yy;
        yy += CAT_ROW_H;
        if (inside(y, CAT_ROW_H)) DrawCategory(i, y, focused, marks);
    }
    if (maxScroll > 0) CategoriesScrollbar(y0, h, track, maxScroll, focused);
}

/* The search box (Ctrl+F), its cursor blinking while it is typed into; a click starts a search. */
static void DrawObjectSearch(float x0, float sy)
{
    bool searching = g_search.active;
    g_cv.Rect(x0 + 12, sy, OBJ_W - 24, 32, BOX, 3);
    g_cv.Frame(x0 + 12, sy, OBJ_W - 24, 32, searching ? BOX_FOCUS : BOX_EDGE, 4);
    if (g_search.text.empty() && !searching) g_cv.Text("Search   Ctrl+F", x0 + 24, sy + 16, T(SZ_SMALL, FAINT));
    else
    {
        float w = g_cv.Text(g_search.text, x0 + 24, sy + 16, T(SZ_LABEL, TEXT, ui::BODY, ui::LEFT, OBJ_W - 60));
        if (searching && (int)(g_time * 2) % 2 == 0) g_cv.Rect(x0 + 26 + w, sy + 7, 2, 18, GOLD, 6);
    }
    if (g_cv.Hit(x0 + 12, sy, OBJ_W - 24, 32, ui::Id("objsearch"))) { g_search.active = true; g_focus = OBJECTS; }
}

/* A weapon group's chip, wrapped onto the next row where it does not fit; a click filters the list by it. */
static void GroupChip(const std::string& g, const std::string& current, const catalog::Nav* nav, float x0, float& cx, float& cy)
{
    static const std::map<std::string, std::string> shortLabels = {{"Assault Rifle", "Assault"}, {"Marksman Rifle", "Marksman"},
        {"Submachine Gun", "SMG"}, {"Energy-Based", "Energy"}};
    std::string label = shortLabels.count(g) ? shortLabels.at(g) : g;
    float w = g_cv.Measure(label, SZ_TINY, ui::TITLE) + 18;
    if (cx + w > x0 + OBJ_W - 12) { cx = x0 + 12; cy += 28; }
    uint64_t id = ui::Id("group" + g);
    bool on = g == current;
    g_cv.Rect(cx, cy, w, 22, on ? GOLD : g_cv.Hovered() == id ? HOVER : BOX, 3);
    if (!on) g_cv.Frame(cx, cy, w, 22, LINE_STRONG, 4);
    g_cv.Text(label, cx + w / 2, cy + 11, T(SZ_TINY, on ? INVERSE : DIM, ui::TITLE, ui::CENTER, 0, 5));
    if (g_cv.Hit(cx, cy, w, 22, id)) { if (g != current) { g_group[nav->id] = g; g_obj = 0; g_field = 0; } g_focus = OBJECTS; }
    cx += w + 6;
}

/* the weapon-group filter, wrapped onto as many rows as it needs; returns where the list below it starts */
static float DrawGroupFilter(const catalog::Nav* nav, float x0, float sy)
{
    std::string current = g_group.count(nav->id) ? g_group[nav->id] : "All";
    float cx = x0 + 12, cy = sy + 40;
    std::vector<std::string> groups = {"All"};
    groups.insert(groups.end(), nav->filters.begin(), nav->filters.end());
    for (auto& g : groups) GroupChip(g, current, nav, x0, cx, cy);
    return cy + 32;
}

void DrawObjects(float y0, float h)
{
    float x0 = CAT_W;
    g_cv.Rect(x0, y0, OBJ_W, h, PANEL, 1);
    g_cv.Rect(x0 + OBJ_W - 1, y0, 1, h, LINE, 2);
    float sy = y0 + 10;
    DrawObjectSearch(x0, sy);
    float ly = sy + 44;
    const catalog::Nav* nav = NavOf(g_cat);
    if (nav != nullptr && nav->hasFilters) ly = DrawGroupFilter(nav, x0, sy);
    auto objects = Objects();
    g_obj = std::clamp(g_obj, 0, std::max(0, (int)objects.size() - 1));
    const MarkCache& marks = Markers();
    bool focused = g_focus == OBJECTS;
    if (objects.empty()) g_cv.Text(!g_search.text.empty() ? "No match" : "Nothing here", x0 + 20, ly + 20, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, OBJ_W - 40));
    ListOpts o;
    o.x = x0; o.y = ly; o.w = OBJ_W - 1; o.h = y0 + h - ly - 6; o.rowH = LIST_ROW_H + 8; o.count = (int)objects.size(); o.selected = g_obj;
    o.key = "obj"; o.focused = focused;
    o.click = [](int i) { SelectObject(i); g_focus = OBJECTS; };
    o.draw = [&](int i, float x, float y, float w, float rh, bool hovered) {
        Object* object = objects[i];
        bool selected = i == g_obj;
        Tone tone = ToneOf(object->tone);
        if (selected) ToneSelection(x + 8, y + 2, w - 16, rh - 4, tone, focused);
        else if (hovered) g_cv.Rect(x + 8, y + 2, w - 16, rh - 4, HOVER, 2);
        float tx = x + 22;
        /* a page of entities keeps the emblem's room on every row, those of no known faction too, so the names line up */
        if (DrawIcon(object, x + 18, y + 5, rh - 10) || EntityPage(object)) tx = x + 18 + rh;
        g_cv.Text(object->name, tx, y + 15, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, w - 56 - (tx - x)));
        g_cv.Text(object->subtitle, tx, y + 31, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, w - 56 - (tx - x)));
        auto found = marks.objects.find(object);
        DrawMarks(found != marks.objects.end() ? found->second : Marks{}, x + w - 10, y + rh / 2, 34);
    };
    List(o);
}

/* A header row's background: a category's darker band, and the selection or the hover over it. */
static void HeaderBackground(const Item& item, const Area& at, bool selected, bool focused, bool hovered)
{
    float x = at.x, y = at.y, lw = at.w, rowH = at.h;
    if (item.depth == 0) g_cv.Rect(x + 6, y + 2, lw - 12, rowH - 4, HEADER, 1);
    if (selected) { g_cv.Rect(x + 6, y + 1, lw - 12, rowH - 2, focused ? SELECT_HEADER : WASH_HEADER, 2); g_cv.Rect(x + 6, y + 1, 3, rowH - 2, focused ? GOLD : GOLD_DIM, 3); }
    else if (hovered) g_cv.Rect(x + 6, y + 1, lw - 12, rowH - 2, item.depth == 0 ? HOVER_HEADER : HOVER, 2); /* a dark header: only a hint */
}

/* A header row: its chevron, its marks, its title (a category's in capitals) and how many parts and fields it has. */
static void DrawHeaderItem(const Item& item, const Area& at, bool selected, bool focused, bool hovered)
{
    float x = at.x, lw = at.w, cy = at.y + at.h / 2;
    float indent = FX_LABEL + item.depth * INDENT;
    HeaderBackground(item, at, selected, focused, hovered);
    Arrow(x + indent, cy, item.open, GOLD);
    float mx = DrawMarks(RowsMarks(item.rows), x + lw - 14, cy, 28);
    float right = mx < x + lw - 14 ? mx - 8 : x + lw - 18, reserve = x + lw - right;
    if (item.depth == 0)
    {
        std::string upper = item.title;
        for (char& c : upper) c = (char)toupper((unsigned char)c);
        g_cv.Text(upper, x + indent + 20, cy, T(SZ_SMALL, GOLD, ui::TITLE, ui::LEFT, lw - indent - 170 - reserve));
        g_cv.Text(item.parts > 1 ? std::to_string(item.parts) + " parts" + SEP + std::to_string(item.count) + " fields" : std::to_string(item.count) + " fields",
            right, cy, T(SZ_TINY, FAINT, ui::BODY, ui::RIGHT));
        return;
    }
    g_cv.Text(item.title, x + indent + 20, cy, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, lw - indent - 150 - reserve));
    g_cv.Text(std::to_string(item.count) + " fields", right, cy, T(SZ_TINY, FAINT, ui::BODY, ui::RIGHT));
}

/* A value box's edge and text colour, by the value's source (staged, edited, a mod's); faint while held (locked or
 * waiting for an earlier status slot). */
static void BoxStyle(const RowView& v, bool editing, bool error, bool held, ImU32* edge, ImU32* colour)
{
    *edge = BOX_EDGE; *colour = TEXT;
    if (editing) *edge = BOX_FOCUS;
    else if (error && v.source != STAGED) *edge = ERROR_;
    else if (v.source == STAGED) { *edge = PENDING; *colour = PENDING; }
    else if (v.source == EDITED) { *edge = GOLD_DIM; *colour = GOLD; }
    else if (v.source == MODDED) *colour = IsLuaHolder(v.holder) ? LUA : MOD;
    if (held && v.source != STAGED) *colour = FAINT;
}

/* The number being typed, selected while it is fresh (the first key replaces it), and the blinking cursor. */
static void DrawEditBuffer(float bx, float bw2, float y, float rowH)
{
    float tw = g_cv.Text(g_edit.buffer, bx + bw2 - 10, y + rowH / 2, T(SZ_LABEL, TEXT, ui::BODY, ui::RIGHT, bw2 - 16));
    if (g_edit.fresh && !g_edit.buffer.empty()) g_cv.Rect(bx + bw2 - 12 - tw, y + 7, tw + 4, rowH - 14, GOLD_SOFT, 4);
    if ((int)(g_time * 2.2f) % 2 == 0) g_cv.Rect(bx + bw2 - 8, y + 8, 2, rowH - 16, GOLD, 6);
}

/* A field's value box: the value (or the number being typed) in its source's colour. */
static void DrawValueBox(const Row* row, const RowView& v, const Area& at, bool error, bool held)
{
    float y = at.y, rowH = at.h, cy = y + rowH / 2;
    bool editing = g_edit.on && g_edit.row == row;
    float bx = at.x + FX_BOX, bw2 = FX_BOX_W;
    ImU32 edge, colour;
    BoxStyle(v, editing, error, held, &edge, &colour);
    g_cv.Rect(bx, y + 4, bw2, rowH - 8, BOX, 3);
    g_cv.Frame(bx, y + 4, bw2, rowH - 8, edge, 4);
    if (editing) DrawEditBuffer(bx, bw2, y, rowH);
    else g_cv.Text(v.has ? Format(row, v.value) : "\xE2\x80\x94", bx + bw2 - 10, cy, T(SZ_LABEL, v.has ? colour : FAINT, ui::BODY, ui::RIGHT, bw2 - 16));
}

/* A field's badge: ERROR, LOCKED, IN ORDER, PENDING (RESET), EDITED (NEXT BOOT), LUA or MOD. */
static void DrawRowBadge(const Row* row, const RowView& v, float bxr, float cy, bool error, bool locked, bool blocked)
{
    if (error) Chip("ERROR", bxr, cy, ERROR_, ERROR_SOFT, true);
    else if (locked) Chip("LOCKED", bxr, cy, FAINT, LINE, true);
    else if (blocked && v.source != STAGED) Chip("IN ORDER", bxr, cy, FAINT, LINE, true);
    else if (v.source == STAGED) Chip(v.reset ? "PENDING RESET" : "PENDING", bxr, cy, PENDING, PENDING_SOFT, true);
    else if (v.source == EDITED) Chip(row->Live() ? "EDITED" : "NEXT BOOT", bxr, cy, GOLD, GOLD_WASH, true);
    else if (v.source == MODDED && IsLuaHolder(v.holder)) Chip("LUA", bxr, cy, LUA, LUA_SOFT, true);
    else if (v.source == MODDED) Chip("MOD", bxr, cy, MOD, MOD_SOFT, true);
}

/* A roster row's weight at one difficulty: that difficulty's icon in front of its label (the HD2R Editor's); one without
 * an icon (level 11 without its mod) keeps level 1's room, so the labels line up. The label's x after it. */
static float DrawDifficultyBadge(const Row* row, float x, float lx, float cy, float rowH)
{
    if (row->difficulty <= 0 || !icons::Texture()) return lx;
    const icons::Icon* badge = icons::Find(icons::DIFFICULTY, std::to_string(row->difficulty));
    const icons::Icon* room = badge != nullptr ? badge : icons::Find(icons::DIFFICULTY, "1");
    if (room == nullptr) return lx;
    float bh = rowH - 12, bw = bh * (float)room->width / (float)room->height;
    if (badge != nullptr) g_cv.Image(icons::Texture(), ImVec2(x + lx, cy - bh / 2), ImVec2(bw, bh), badge->uv0, badge->uv1, IM_COL32_WHITE, 4);
    return lx + bw + 8;
}

/* A field's row (or a note): its label, default, value box, unit and badge. */
static void DrawRowItem(const Item& item, const Area& at, bool selected, bool focused, bool hovered)
{
    float x = at.x, y = at.y, lw = at.w, rowH = at.h, cy = y + rowH / 2;
    const Row* row = item.row;
    if (selected) { g_cv.Rect(x + 6, y + 1, lw - 12, rowH - 2, focused ? SELECT : GOLD_WASH, 2); g_cv.Rect(x + 6, y + 1, 3, rowH - 2, focused ? GOLD : GOLD_DIM, 3); }
    else if (hovered) g_cv.Rect(x + 6, y + 1, lw - 12, rowH - 2, HOVER, 2);
    float lx = FX_LABEL + (item.depth > 0 ? (item.depth + 1) * INDENT : 0);
    if (row == nullptr) { g_cv.Text(item.note, x + lx, cy, T(SZ_SMALL, item.detail ? FAINT : PENDING, ui::BODY, ui::LEFT, lw - lx - 24)); return; }
    lx = DrawDifficultyBadge(row, x, lx, cy, rowH);
    bool locked = !LockOf(row).empty(), blocked = !SlotBlock(row).empty();
    RowView v = ViewOf(row);
    std::string def = v.hasBase ? Format(row, v.base) : "\xE2\x80\x94";
    float dw = std::min(84.0f, g_cv.Measure(def, SZ_SMALL));
    float labelMax = FX_DEFAULT_RIGHT - std::max(dw, 24.0f) - 12 - lx;
    g_cv.Text(RowLabel(item, labelMax), x + lx, cy, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, labelMax));
    g_cv.Text(def, x + FX_DEFAULT_RIGHT, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::RIGHT, 84));
    bool error = g_errors.count(row) > 0;
    DrawValueBox(row, v, at, error, locked || blocked);
    if (!row->unit.empty()) g_cv.Text(row->unit, x + FX_UNIT, cy, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, 56));
    DrawRowBadge(row, v, x + lw + FX_BADGE_RIGHT, cy, error, locked, blocked);
}

void DrawFields(float y0, float h)
{
    float x0 = CAT_W + OBJ_W, w = PANEL_W - x0;
    g_cv.Rect(x0, y0, w, h, PANEL_ALT, 1);
    Object* object = Current();
    if (object == nullptr) { g_cv.Text("Select a category and an object", x0 + 24, y0 + 40, T(SZ_LABEL, FAINT)); return; }
    float hx = x0 + 20;
    if (DrawIcon(object, x0 + 20, y0 + 8, 48)) hx = x0 + 80;
    bool anyOpen = PageOpen(object);
    float bw = 142;
    if (Button(anyOpen ? "COLLAPSE ALL" : "EXPAND ALL", {x0 + w - bw - 14, y0 + 12, bw, 28}, NORMAL, ui::Id("pageopen"))) SetPageOpen(object, !anyOpen);
    float rw = 132;
    bool changed = !ItemChanges(object).empty();
    if (Button("RESET ITEM", {x0 + w - bw - rw - 24, y0 + 12, rw, 28}, changed && !Locked() ? DANGER : DISABLED, ui::Id("resetitem")) && changed) ResetItem(object);
    g_cv.Text(object->name, hx, y0 + 24, T(20, TEXT, ui::TITLE, ui::LEFT, w - 70 - bw - rw - (hx - x0)));
    int edited = 0;
    /* staged or saved (ViewOf's STAGED or EDITED), without its live value: an enemy's 300 fields every frame */
    for (const Row* row : object->rows) { double saved; if ((!Locked() && PendingOf(row)) || Saved(row, &saved)) edited++; }
    std::string sub = (object->subtitle.empty() ? "" : object->subtitle + "   ") + std::to_string(object->rows.size()) + " fields" +
        (edited > 0 ? "   " + std::to_string(edited) + " edited" : "");
    g_cv.Text(sub, hx, y0 + 47, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, w - 40 - (hx - x0)));
    float hy = y0 + 66;
    g_cv.Rect(x0, hy, w, 22, HEADER, 2);
    g_cv.Text("FIELD", x0 + FX_LABEL, hy + 11, T(SZ_TINY, FAINT, ui::TITLE));
    g_cv.Text("DEFAULT", x0 + FX_DEFAULT_RIGHT, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::RIGHT));
    g_cv.Text("VALUE", x0 + FX_BOX + FX_BOX_W - 8, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::RIGHT));
    g_cv.Text("SOURCE", x0 + w + FX_BADGE_RIGHT, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::RIGHT));
    auto items = Items(object);
    g_field = std::clamp(g_field, 0, std::max(0, (int)items.size() - 1));
    bool focused = g_focus == FIELDS;
    float ly = hy + 24, rh = ROW_H;
    ListOpts o;
    o.x = x0; o.y = ly; o.w = w; o.h = y0 + h - ly - 4; o.rowH = rh; o.count = (int)items.size(); o.selected = g_field; o.key = "fields"; o.focused = focused;
    o.click = [&](int i) {
        g_field = i;
        g_focus = FIELDS;
        if (items[i].header) { Toggle(items[i].key); return; }
        if (items[i].row == nullptr) return;
        float ux, uy;
        g_cv.ToUnits(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, &ux, &uy);
        if (ux >= x0 + FX_BOX && ux <= x0 + FX_BOX + FX_BOX_W) BeginEdit(items[i].row, "", true);
    };
    o.rightClick = [&](int i) { if (!items[i].header && items[i].row) Revert(items[i].row); };
    o.draw = [&](int i, float x, float y, float lw, float rowH, bool hovered) {
        const Item& item = items[i];
        if (item.header) DrawHeaderItem(item, {x, y, lw, rowH}, i == g_field, focused, hovered);
        else DrawRowItem(item, {x, y, lw, rowH}, i == g_field, focused, hovered);
    };
    List(o);
}

/* ---- the status bar and the footer ---- */
/* A reference row: its key, when it applies, SHARED, and what it picks now (or how to pick). */
static void StatusPick(const Row* row, float cy, float maxw)
{
    RowView v = ViewOf(row);
    const Row::Option* o = v.has ? g_catalog.Reference(row->pick, v.value) : nullptr;
    float x = 18;
    x += g_cv.Text(row->key, x, cy, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, 300)) + 16;
    x += g_cv.Text(AppliesText(row), x, cy, T(SZ_SMALL, row->Live() ? FAINT : PENDING, ui::BODY, ui::LEFT, 220)) + 14;
    if (row->shared || !row->also.empty()) x += Chip("SHARED", x, cy, PENDING, PENDING_SOFT, false) + 6;
    std::string text = !row->note.empty() ? row->note : o != nullptr ? o->group + ": " + o->note :
        "Enter picks " + std::string(row->pick == "status" ? "a status effect" : row->pick == "explosion" ? "an explosion" : "a projectile");
    g_cv.Text(text, x + 6, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, std::max(40.0f, maxw - x - 6)));
}

/* A choice row: its option's note, and when it applies (an armor perk's: what the loader says of it). */
static void StatusChoice(const Row* row, float cy, float maxw)
{
    RowView v = ViewOf(row);
    const Row::Option* o = row->OptionOf(v.value);
    std::string text = o && !o->note.empty() ? o->label + ": " + o->note : row->note;
    if (IsMount(row)) text += row->choice == "rowcopy" ? "   [applies now]" : IsPod(row) ? "   [applies to pods called in after it]" :
        "   [applies to vehicles called in after it]";
    else if (HasPassives())
    {
        HfPassives state;
        g_host.api->get_passives(g_host.mod, &state);
        if (state.held || state.status[0]) text += std::string("   [") + state.status + "]";
    }
    else text += "   [this HellForge has no set_passives]";
    g_cv.Text(text, 18, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, maxw));
}

/* A number field's range as the status bar says it. */
static std::string RangeText(const Row* row)
{
    std::string range;
    if (row->hasMin || row->hasMax)
        range = "range " + (row->hasMin ? Format(row, row->min) : std::string("-inf")) + " to " + (row->hasMax ? Format(row, row->max) : std::string("inf"));
    else range = "no published range";
    if (row->Whole()) range += ", whole numbers";
    if (row->unlimited) range = "unlimited (-1 or 0), or " + (row->hasMin ? Format(row, row->min) : std::string("1")) + " to " +
        (row->hasMax ? Format(row, row->max) : std::string("100"));
    return range;
}

/* The first two names, and how many more. */
static std::string FirstNames(const std::vector<std::string>& of)
{
    std::string names;
    for (size_t i = 0; i < of.size() && i < 2; i++) names += (i ? ", " : "") + of[i];
    if (of.size() > 2) names += " +" + std::to_string(of.size() - 2);
    return names;
}

/* weapons that reach the row only by equipping the same ammunition are named as such (bug-report-0035) */
static std::string AlsoText(const Row* row)
{
    std::string text = row->also.empty() ? "" : "also changes " + FirstNames(row->also);
    if (!row->alsoIf.empty())
        text += (text.empty() ? "also changes " : "; and ") + FirstNames(row->alsoIf) + " when they use " + row->alsoAmmo;
    return text;
}

/* After a field's facts: the mod holding its value, the other items a change reaches, or its note. */
static void StatusAside(const Row* row, const RowView& v, float x, float cy, float maxw)
{
    if (!v.holder.empty())
        g_cv.Text(std::string(IsLuaHolder(v.holder) ? "Lua mod: " : "Mod: ") + v.holder + " = " + Format(row, v.base), x + 10, cy,
            T(SZ_SMALL, IsLuaHolder(v.holder) ? LUA : MOD, ui::BODY, ui::LEFT, std::max(40.0f, maxw - x - 10)));
    else if (!row->also.empty() || !row->alsoIf.empty())
        g_cv.Text(AlsoText(row), x + 10, cy, T(SZ_SMALL, PENDING, ui::BODY, ui::LEFT, std::max(40.0f, maxw - x - 10)));
    else if (!row->note.empty())
        g_cv.Text(row->note, x + 10, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, std::max(40.0f, maxw - x - 10)));
}

/* A number field: its key, range, when it applies, SHARED and UNVERIFIED EFFECT, then what StatusAside says. */
static void StatusField(const Row* row, float cy, float maxw)
{
    float x = 18;
    std::string range = RangeText(row);
    x += g_cv.Text(row->key, x, cy, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, 260)) + 16;
    x += g_cv.Text(range, x, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, 240)) + 14;
    x += g_cv.Text(AppliesText(row), x, cy, T(SZ_SMALL, row->Live() ? FAINT : PENDING, ui::BODY, ui::LEFT, 220)) + 14;
    if (row->shared || !row->also.empty()) x += Chip("SHARED", x, cy, PENDING, PENDING_SOFT, false) + 6;
    if (row->unverified) x += Chip("UNVERIFIED EFFECT", x, cy, FAINT, LINE, false) + 6;
    StatusAside(row, ViewOf(row), x, cy, maxw);
}

/* BROWSE's status bar: about the focused field (or note), or how to pick one. */
static void StatusBrowse(float cy, float maxw)
{
    auto items = Items(Current());
    const Row* row = g_focus == FIELDS && g_field < (int)items.size() ? items[g_field].row : nullptr;
    if (row == nullptr && g_focus == FIELDS && g_field < (int)items.size() && !items[g_field].note.empty())
    { g_cv.Text(items[g_field].note, 18, cy, T(SZ_SMALL, PENDING, ui::BODY, ui::LEFT, maxw)); return; }
    if (row != nullptr && (!LockOf(row).empty() || !SlotBlock(row).empty()))
    {
        std::string why = LockOf(row);
        g_cv.Text("Locked for now: " + (why.empty() ? SlotBlock(row) : why), 18, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, maxw));
        return;
    }
    if (row != nullptr && !row->pick.empty()) { StatusPick(row, cy, maxw); return; }
    if (row == nullptr) { g_cv.Text("Pick a field: \xE2\x86\x91\xE2\x86\x93 to move, \xE2\x86\x92 to go deeper, Tab to switch panes", 18, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, maxw)); return; }
    auto error = g_errors.find(row);
    if (error != g_errors.end()) { g_cv.Text("Error: " + error->second, 18, cy, T(SZ_SMALL, ERROR_, ui::BODY, ui::LEFT, maxw)); return; }
    if (!row->choice.empty()) { StatusChoice(row, cy, maxw); return; }
    StatusField(row, cy, maxw);
}

/* The other views' hints. */
static const char* ViewHint()
{
    return g_view == SEARCH ? "Every word must match, in any order. Enter or a click opens the object in BROWSE." :
        g_view == CHANGES ? "Enter jumps to the field. RESET or Backspace stages a reset, UNDO drops a pending change; APPLY writes them." :
        g_view == MODS ? "\xE2\x86\x91\xE2\x86\x93 mod   PgUp PgDn value   Enter or a click opens the field; its reset goes back to the mod's value   F5 read again" :
        g_view == PRESETS ? "Enter or IMPORT stages the selected preset; EXPORT writes your saved changes as a preset HD2R Editor reads." :
        g_view == LOGS ? "\xE2\x86\x90\xE2\x86\x92 filter   Enter switch HellForge's log / the editor's   End follow the end" :
        "Settings are saved with the editor and apply at once.";
}

void DrawStatus(float y, float h)
{
    g_cv.Rect(0, y, PANEL_W, h, HEADER, 1);
    g_cv.Rect(0, y, PANEL_W, 1, LINE, 2);
    float cy = y + h / 2;
    if (g_toast.on && g_time - g_toast.time > 3.2f) g_toast.on = false;
    if (g_toast.on) g_cv.Text(g_toast.text, PANEL_W - 18, cy, T(SZ_SMALL, g_toast.colour, ui::BODY, ui::RIGHT, 520));
    float maxw = g_toast.on ? PANEL_W - 580 : PANEL_W - 40;
    if (!g_catalogOk) { g_cv.Text("The editor's catalogue could not be read: " + g_catalogError, 18, cy, T(SZ_SMALL, ERROR_, ui::BODY, ui::LEFT, maxw)); return; }
    if (g_view == BROWSE) { StatusBrowse(cy, maxw); return; }
    g_cv.Text(ViewHint(), 18, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, maxw));
}

/* The footer's keys for what has the focus. */
static const char* FooterHint()
{
    auto items = g_view == BROWSE ? Items(Current()) : std::vector<Item>{};
    bool header = g_focus == FIELDS && g_field < (int)items.size() && items[g_field].header;
    if (g_edit.on) return "Type a number   Backspace erase   Tab / \xE2\x86\x91\xE2\x86\x93 confirm   Del cancel";
    if (g_search.active) return "Type to filter   Backspace erase   Tab done   Del clear";
    if (g_view == SEARCH) return "Type to search   \xE2\x86\x91\xE2\x86\x93 select   Enter open   Backspace erase   Del clear";
    if (g_view == BROWSE && g_focus == FIELDS && header)
        return "F8 close   Enter open / close   \xE2\x86\x90\xE2\x86\x92 open, close   Ctrl+\xE2\x86\x91 collapse all   Ctrl+\xE2\x86\x93 expand all   F9 apply";
    if (g_view == BROWSE && g_focus == FIELDS)
        return "F8 close   \xE2\x86\x90\xE2\x86\x92 adjust (Shift x10, Ctrl \xC3\xB7" "10)   digits type a value   Del revert   F9 apply";
    return "F8 close   \xE2\x86\x91\xE2\x86\x93 select   \xE2\x86\x90\xE2\x86\x92 panes   Tab next pane   Shift+Tab next tab   F9 apply";
}

void DrawFooter(float y, float h)
{
    g_cv.Rect(0, y, PANEL_W, h, FOOTER, 1);
    g_cv.Rect(0, y, PANEL_W, 1, LINE_STRONG, 2);
    g_cv.Text(FooterHint(), 18, y + h / 2, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, PANEL_W - 560));
    float bh = 34, by = y + (h - bh) / 2, bx = PANEL_W - 14;
    int pending = PendingCount();
    bool resettable = !SavedRows().empty() || pending > 0;
    float bw = 196;
    bx -= bw;
    if (Button("RESET TO DEFAULTS", {bx, by, bw, bh}, resettable && !Locked() ? DANGER : DISABLED, ui::Id("reset"))) StageResetAll();
    bw = 110; bx -= bw + 10;
    if (Button("DISCARD", {bx, by, bw, bh}, pending > 0 && !Locked() ? NORMAL : DISABLED, ui::Id("discard"))) Discard();
    bw = 150; bx -= bw + 10;
    if (Button(pending > 0 ? "APPLY  " + std::to_string(pending) : "APPLY", {bx, by, bw, bh}, pending > 0 && !Locked() ? PRIMARY : DISABLED, ui::Id("apply"))) Apply();
}
} // namespace editorapp
