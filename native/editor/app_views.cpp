/* The editor's window (app_internal.h): SEARCH, CHANGES and SETTINGS, and the picker and errors dialogs. */
#include <math.h>
#include <stdio.h>
#include <algorithm>
#include "app_internal.h"
#include "icons.h"

namespace editorapp
{
/* ---- SEARCH, CHANGES, SETTINGS ---- */
void DrawSearch(float y0, float h)
{
    g_cv.Rect(0, y0, PANEL_W, h, PANEL, 1);
    float bx = 20, by = y0 + 18, bw = PANEL_W - 40, bh = 40;
    g_cv.Rect(bx, by, bw, bh, BOX, 3);
    g_cv.Frame(bx, by, bw, bh, BOX_FOCUS, 4);
    bool empty = g_gsearch.text.empty();
    float tw = g_cv.Text(empty ? "Search every primary, secondary and throwable..." : g_gsearch.text, bx + 14, by + bh / 2,
        T(SZ_LABEL, empty ? FAINT : TEXT, ui::BODY, ui::LEFT, bw - 28, 6));
    if (!empty && (int)(g_time * 2) % 2 == 0) g_cv.Rect(bx + 16 + tw, by + 10, 2, bh - 20, GOLD, 6);
    auto results = SearchResults(g_gsearch.text);
    g_gsearch.index = std::clamp(g_gsearch.index, 0, std::max(0, (int)results.size() - 1));
    std::string info = empty ? "Type to search. Every word must match, in any order." :
        results.empty() ? "Nothing matches \"" + g_gsearch.text + "\"." : std::to_string(results.size()) + " results";
    g_cv.Text(info, bx, by + bh + 18, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, bw));
    float ly = by + bh + 34;
    ListOpts o;
    o.x = 0; o.y = ly; o.w = PANEL_W; o.h = y0 + h - ly - 6; o.rowH = 40; o.count = (int)results.size(); o.selected = g_gsearch.index;
    o.key = "gsearch"; o.focused = true;
    o.click = [&](int i) { g_gsearch.index = i; GotoObject(results[i].object); g_view = BROWSE; g_focus = OBJECTS; Sound(sounds::OPEN); };
    o.draw = [&](int i, float x, float y, float w, float rh, bool hovered) {
        const Result& r = results[i];
        bool selected = i == g_gsearch.index;
        if (selected) { g_cv.Rect(x + 8, y + 2, w - 16, rh - 4, SELECT, 2); g_cv.Rect(x + 8, y + 2, 3, rh - 4, GOLD, 3); }
        else if (hovered) g_cv.Rect(x + 8, y + 2, w - 16, rh - 4, HOVER, 2);
        float tx = x + 24;
        if (DrawIcon(r.object, x + 20, y + 5, rh - 10)) tx = x + 20 + rh;
        g_cv.Text(r.object->name, tx, y + 14, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, w * 0.6f));
        g_cv.Text(r.sub, tx, y + 30, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, w * 0.7f));
    };
    List(o);
}

void DrawChanges(float y0, float h)
{
    float W = PANEL_W;
    auto list = Changes();
    g_change = std::clamp(g_change, 0, std::max(0, (int)list.size() - 1));
    g_cv.Rect(0, y0, W, h, PANEL, 1);
    g_cv.Text("CHANGES", 20, y0 + 24, T(20, TEXT, ui::TITLE));
    int saved = (int)SavedRows().size();
    if (Locked())
        g_cv.Text(std::string("The changes ") + (g_session.host[0] ? g_session.host : "the host") + " saved, which everyone in the lobby plays with (read-only).   " +
            std::to_string(saved) + " saved", 20, y0 + 47, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, W - 300));
    else
        g_cv.Text("Values the editors saved into Local Stats (also the launcher's STATS page), and pending ones.   " + std::to_string(saved) + " saved, " +
            std::to_string(PendingCount()) + " pending, " + std::to_string(g_errors.size()) + " failed", 20, y0 + 47, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, W - 300));
    if (Button("REVERT ALL", {W - 216, y0 + 16, 196, 34}, !Locked() && (saved > 0 || PendingCount() > 0) ? DANGER : DISABLED, ui::Id("revertall"))) StageResetAll();
    float hy = y0 + 66;
    g_cv.Rect(0, hy, W, 22, HEADER, 2);
    g_cv.Text("OBJECT", 20, hy + 11, T(SZ_TINY, FAINT, ui::TITLE));
    g_cv.Text("FIELD", 330, hy + 11, T(SZ_TINY, FAINT, ui::TITLE));
    g_cv.Text("DEFAULT", W - 420, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::RIGHT));
    g_cv.Text("VALUE", W - 250, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::RIGHT));
    g_cv.Text("STATE", W - 112, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::RIGHT));
    if (list.empty()) g_cv.Text(Locked() ? "The host plays without saved changes." : "No editor changes. Edit a field in Browse, then Apply.", 20, hy + 44, T(SZ_SMALL, FAINT));
    ListOpts o;
    o.x = 0; o.y = hy + 24; o.w = W; o.h = y0 + h - hy - 28; o.rowH = 32; o.count = (int)list.size(); o.selected = g_change; o.key = "changes"; o.focused = true;
    o.click = [&](int i) { if (g_change == i) { Reveal(list[i].row); Sound(sounds::OPEN); } else g_change = i; };
    o.draw = [&](int i, float x, float y, float lw, float rh, bool hovered) {
        const Change& e = list[i];
        bool selected = i == g_change;
        if (selected) { g_cv.Rect(x + 8, y + 2, lw - 16, rh - 4, SELECT, 2); g_cv.Rect(x + 8, y + 2, 3, rh - 4, GOLD, 3); }
        else if (hovered) g_cv.Rect(x + 8, y + 2, lw - 16, rh - 4, HOVER, 2);
        float cy = y + rh / 2;
        DrawIcon(e.row->object, x + 18, y + 4, rh - 8);
        g_cv.Text(e.row->object->name, x + 18 + rh, cy, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, 300 - rh));
        g_cv.Text(e.row->label, x + 330, cy, T(SZ_SMALL, TEXT, ui::BODY, ui::LEFT, lw - 330 - 450));
        LiveState& live = Live(e.row);
        g_cv.Text(live.read ? Format(e.row, live.base) : "\xE2\x80\x94", x + lw - 420, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::RIGHT, 150));
        g_cv.Text(e.reset && !live.read ? "\xE2\x80\x94" : Format(e.row, e.value), x + lw - 250, cy, T(SZ_LABEL, e.state == 1 ? PENDING : GOLD, ui::BODY, ui::RIGHT, 160));
        if (e.state == 1) Chip(e.reset ? "PENDING RESET" : "PENDING", x + lw - 112, cy, PENDING, PENDING_SOFT, true);
        else if (e.state == 2) Chip("ERROR", x + lw - 112, cy, ERROR_, ERROR_SOFT, true);
        else Chip(e.row->Live() || LiveStat(e.row) ? "EDITED" : "NEXT BOOT", x + lw - 112, cy, GOLD, GOLD_WASH, true);
    };
    o.after = [&](int i, float x, float y, float lw, float rh) {
        if (Locked()) return; /* the host's: nothing to reset here */
        if (Button(list[i].state == 1 ? "UNDO" : "RESET", {x + lw - 96, y + 4, 80, rh - 8}, list[i].state == 1 ? NORMAL : DANGER, ui::Id("changereset", (uint64_t)i)))
            Revert(list[i].row);
    };
    List(o);
}

void DrawSettings(float y0, float h)
{
    g_cv.Rect(0, y0, PANEL_W, h, PANEL, 1);
    g_cv.Text("SETTINGS", 20, y0 + 24, T(20, TEXT, ui::TITLE));
    float x = 24, y = y0 + 64;
    g_cv.Rect(x - 8, y - 14, 560, 28, SELECT, 1);
    g_cv.Text("Interface size (text and everything else)", x, y, T(SZ_LABEL, DIM));
    int index = 0;
    for (int i = 0; i < UI_SCALE_COUNT; i++) if (UI_SCALES[i] == g_uiScale) index = i;
    if (Button("-", {x + 400, y - 13, 30, 26}, index > 0 ? NORMAL : DISABLED, ui::Id("scaledown"))) { g_uiScale = UI_SCALES[index - 1]; SaveSettings(); }
    char percent[16];
    snprintf(percent, sizeof percent, "%d%%", (int)floorf(g_uiScale * 100 + 0.5f));
    g_cv.Text(percent, x + 470, y, T(SZ_LABEL, TEXT, ui::BODY, ui::CENTER));
    if (Button("+", {x + 510, y - 13, 30, 26}, index + 1 < UI_SCALE_COUNT ? NORMAL : DISABLED, ui::Id("scaleup"))) { g_uiScale = UI_SCALES[index + 1]; SaveSettings(); }
    if (floorf(g_scaleNow * 100 + 0.5f) < floorf(g_uiScale * 100 + 0.5f))
    {
        snprintf(percent, sizeof percent, "%d%%", (int)floorf(g_scaleNow * 100 + 0.5f));
        g_cv.Text(std::string("(") + percent + " fits this screen)", x + 552, y, T(SZ_TINY, FAINT));
    }
    y += 34;
    uint64_t soundsId = ui::Id("sounds");
    if (g_cv.Hovered() == soundsId) g_cv.Rect(x - 8, y - 14, 560, 28, HOVER, 1);
    g_cv.Text("Menu sounds on buttons (the game's own)", x, y, T(SZ_LABEL, DIM));
    Chip(g_soundsOn ? "ON" : "OFF", x + 540, y, g_soundsOn ? INVERSE : FAINT, g_soundsOn ? GOLD : LINE, true);
    if (g_cv.Hit(x - 8, y - 14, 560, 28, soundsId)) { g_soundsOn = !g_soundsOn; sounds::SetEnabled(g_soundsOn); SaveSettings(); Sound(sounds::CLICK); }
    y += 34;
    g_cv.Text("The window is 1260 units wide on a 1080-unit-tall screen, centred: 100% fills the same share of every screen.", x, y, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, PANEL_W - 60));
    y += 26;
    g_cv.Text(std::string("Game build ") + g_host.api->build_id() + "   " + std::to_string(g_catalog.objects.size()) + " objects, " +
        std::to_string(g_catalog.rows.size()) + " fields", x, y, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, PANEL_W - 60));
}

/* the search box: what is typed filters the list */
static void DrawPickerSearch(float x, float sy, float sw, float sh)
{
    int matches = (int)std::count_if(g_picker.entries.begin(), g_picker.entries.end(), [](auto& e) { return !e.header; });
    g_cv.Rect(x + 24, sy, sw, sh, BOX, 11);
    g_cv.Frame(x + 24, sy, sw, sh, g_picker.filter.empty() ? BOX_EDGE : BOX_FOCUS, 12);
    bool empty = g_picker.filter.empty();
    float tw = g_cv.Text(empty ? "Search..." : g_picker.filter, x + 36, sy + sh / 2, T(SZ_LABEL, empty ? FAINT : TEXT, ui::BODY, ui::LEFT, sw - 140, 13));
    if ((int)(g_time * 2.2f) % 2 == 0) g_cv.Rect(empty ? x + 32 : x + 38 + tw, sy + 7, 2, sh - 14, GOLD, 13);
    g_cv.Text(std::to_string(matches) + (matches == 1 ? " match" : " matches"), x + 24 + sw - 12, sy + sh / 2, T(SZ_TINY, FAINT, ui::BODY, ui::RIGHT, 120, 13));
}

/* A group's heading among the options. */
static void DrawPickHeading(const PickEntry& option, const Area& at)
{
    std::string upper = option.label;
    for (char& c : upper) c = (char)toupper((unsigned char)c);
    g_cv.Text(upper, at.x + 22, at.y + at.h / 2 + 4, T(SZ_TINY, GOLD, ui::TITLE, ui::LEFT, at.w - 44, 12));
    g_cv.Rect(at.x + 22, at.y + at.h - 3, at.w - 44, 1, LINE, 11);
}

/* An option's warnings as chips, right to left from right; returns the x left of them. */
static float WarningChips(const PickEntry& option, float right, float cy)
{
    for (auto w = option.warnings.rbegin(); w != option.warnings.rend(); ++w)
        right -= Chip(WarningChip(*w), right, cy, *w == "very_large" || *w == "objective_packages" ? ERROR_ : PENDING,
            *w == "very_large" || *w == "objective_packages" ? ERROR_SOFT : PENDING_SOFT, true, 12) + 6;
    return right;
}

/* An option's note, after an explosion's summary (its damage, radius and statuses, read from the game). */
static std::string PickNote(const PickEntry& option, const Row* row)
{
    std::string note = option.note;
    if (row->pick == "explosion" && option.value != 0)
        if (std::string sum = ExplosionSummary((uint32_t)option.value); !sum.empty()) note = sum + "   " + note;
    return note;
}

/* An option: its icon (a passive's), its label, the CURRENT and warning chips, and its note. */
static void DrawPickOption(const PickEntry& option, const Row* row, const RowView& v, const Area& at, bool selected, bool hovered)
{
    float rx = at.x, ry = at.y, rw = at.w, rh = at.h;
    if (selected) { g_cv.Rect(rx + 8, ry + 2, rw - 16, rh - 4, SELECT, 10); g_cv.Rect(rx + 8, ry + 2, 3, rh - 4, GOLD, 11); }
    else if (hovered) g_cv.Rect(rx + 8, ry + 2, rw - 16, rh - 4, HOVER, 10);
    float tx = rx + 22;
    const icons::Icon* icon = option.icon.empty() ? nullptr : icons::Find(icons::PASSIVE, option.icon);
    if (icon != nullptr && icons::Texture()) { g_cv.Image(icons::Texture(), ImVec2(rx + 18, ry + 6), ImVec2(rh - 12, rh - 12), icon->uv0, icon->uv1, PERK, 11); tx = rx + 18 + rh; }
    float top = row->pick.empty() ? 16 : 13, below = row->pick.empty() ? 34 : 29;
    float right = rx + rw - 20;
    if (option.value == v.value) right -= Chip("CURRENT", right, ry + top, GOLD, GOLD_WASH, true, 12) + 6;
    right = WarningChips(option, right, ry + top);
    g_cv.Text(option.label, tx, ry + top, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, right - tx - 8, 12));
    std::string note = PickNote(option, row);
    if (!note.empty()) g_cv.Text(note, tx, ry + below, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, rw - (tx - rx) - 24, 12));
}

/* the highlighted option's warnings, spelled out (HD2R Editor's), else the keys */
static void DrawPickerHint(const Row* row, float x, float y, float w, float h)
{
    std::string warning;
    if (PickerHas(g_picker.index))
        for (auto& w : g_picker.entries[g_picker.index].warnings)
            if (auto text = g_catalog.warnings.find(w); text != g_catalog.warnings.end()) warning += (warning.empty() ? "" : "   ") + WarningChip(w) + ": " + text->second;
    if (!warning.empty()) g_cv.Text(warning, x + 24, y + h - 25, T(SZ_TINY, PENDING, ui::BODY, ui::LEFT, w - 24 - 190, 11));
    else g_cv.Text(row->pick.empty() ? "\xE2\x86\x91\xE2\x86\x93 select   Enter choose   Esc cancel" :
        "\xE2\x86\x91\xE2\x86\x93 select   type to filter   Backspace erase   Enter choose   Esc cancel", x + 24, y + h - 25, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, 0, 11));
}

/* The picker of a choice row (the HD2R Editor's pickers): its options with their icons and notes, a dialog over the panel. */
void DrawPicker()
{
    if (!g_picker.on) return;
    const Row* row = g_picker.row;
    g_cv.Rect(0, 0, PANEL_W, g_panelH, C(0, 0, 0, 120), 8);
    float w = 720, h = std::min(g_panelH - 120, 640.0f), x = (PANEL_W - w) / 2, y = (g_panelH - h) / 2;
    g_cv.Block();
    g_cv.Rect(x, y, w, h, PANEL_ALT, 9);
    g_cv.Frame(x, y, w, h, LINE_STRONG, 10);
    g_cv.Rect(x, y, w, 3, GOLD, 10);
    std::string title = row->label + (row->pick.empty() ? std::string() : SEP + row->object->name);
    g_cv.Text(title, x + 24, y + 30, T(19, TEXT, ui::TITLE, ui::LEFT, w - 48, 11));
    if (!row->note.empty()) g_cv.Text(row->note, x + 24, y + 52, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, w - 48, 11));
    float sy = y + (row->note.empty() ? 46 : 66), sh = 30, sw = w - 48;
    DrawPickerSearch(x, sy, sw, sh);
    RowView v = ViewOf(row);
    ListOpts o;
    float ly = sy + sh + 8;
    o.x = x + 2; o.y = ly; o.w = w - 4; o.h = y + h - ly - 44; o.rowH = row->pick.empty() ? 48 : 40; o.count = (int)g_picker.entries.size();
    o.selected = g_picker.index;
    o.key = "picker"; o.focused = true; o.layer = 12;
    o.click = [&](int i) { PickerChoose(i); };
    o.draw = [&](int i, float rx, float ry, float rw, float rh, bool hovered) {
        const PickEntry& option = g_picker.entries[i];
        if (option.header) DrawPickHeading(option, {rx, ry, rw, rh});
        else DrawPickOption(option, row, v, {rx, ry, rw, rh}, i == g_picker.index, hovered);
    };
    List(o);
    if (Button("CANCEL", {x + w - 24 - 150, y + h - 40, 150, 30}, NORMAL, ui::Id("pickercancel"), 11)) g_picker.on = false;
    DrawPickerHint(row, x, y, w, h);
}

/* The errors popup (HD2R Editor's): every refused field with HellForge's reason; a click or Enter opens it in BROWSE,
 * RESET stages it back to its default (UNDO takes that back). */
void DrawErrors()
{
    if (!g_errorsPopup.on) return;
    auto list = ErrorRows();
    int n = (int)list.size();
    g_errorsPopup.index = std::clamp(g_errorsPopup.index, 0, std::max(0, n - 1));
    g_cv.Rect(0, 0, PANEL_W, g_panelH, C(0, 0, 0, 120), 8);
    float w = std::min(1120.0f, PANEL_W - 60), h = std::min(g_panelH - 80, 640.0f), x = (PANEL_W - w) / 2, y = (g_panelH - h) / 2;
    g_cv.Block();
    g_cv.Rect(x, y, w, h, PANEL_ALT, 9);
    g_cv.Frame(x, y, w, h, LINE_STRONG, 10);
    g_cv.Rect(x, y, w, 3, ERROR_, 10);
    float tw = g_cv.Text("ERRORS", x + 24, y + 30, T(18, ERROR_, ui::TITLE, ui::LEFT, 0, 11));
    g_cv.Text(n == 1 ? "1 field was refused" : std::to_string(n) + " fields were refused", x + 36 + tw, y + 31, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, w - 300, 11));
    if (Button("\xC3\x97", {x + w - 44, y + 12, 30, 30}, NORMAL, ui::Id("errorsclose"), 11)) { g_errorsPopup.on = false; Sound(sounds::BACK); }
    float hy = y + 52;
    g_cv.Rect(x + 1, hy, w - 2, 22, HEADER, 10);
    g_cv.Text("OBJECT \xC2\xB7 FIELD", x + 24, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::LEFT, 0, 11));
    g_cv.Text("VALUE", x + w - 140, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::RIGHT, 0, 11));
    float detail = 64;
    ListOpts o;
    o.x = x + 2; o.y = hy + 26; o.w = w - 4; o.h = y + h - hy - 26 - detail - 48; o.rowH = 50; o.count = n; o.selected = g_errorsPopup.index;
    o.key = "errors"; o.focused = true; o.layer = 12;
    o.click = [&](int i) { g_errorsPopup.index = i; g_errorsPopup.on = false; Reveal(list[i]); Sound(sounds::OPEN); };
    o.draw = [&](int i, float rx, float ry, float rw, float rh, bool hovered) {
        const Row* row = list[i];
        bool selected = i == g_errorsPopup.index;
        if (selected) { g_cv.Rect(rx + 10, ry + 2, rw - 20, rh - 4, ERROR_SOFT, 10); g_cv.Rect(rx + 10, ry + 2, 3, rh - 4, ERROR_, 11); }
        else if (hovered) g_cv.Rect(rx + 10, ry + 2, rw - 20, rh - 4, HOVER, 10);
        float lx = rx + 22;
        if (DrawIcon(row->object, rx + 20, ry + 7, rh - 14, 12)) lx = rx + 26 + rh - 14;
        float nw = g_cv.Text(row->object->name, lx, ry + 16, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, (rw - 300 - lx + rx) * 0.45f, 12));
        g_cv.Text(row->label, lx + nw + 12, ry + 16, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, rw - 300 - (lx - rx) - nw - 12, 12));
        double v;
        const Pend* p = PendingOf(row);
        std::string value = p ? (p->reset ? "reset" : Format(row, p->value)) : Saved(row, &v) ? Format(row, v) : "\xE2\x80\x94";
        g_cv.Text(value, rx + rw - 140, ry + 16, T(SZ_LABEL, ERROR_, ui::BODY, ui::RIGHT, 150, 12));
        g_cv.Text(g_errors[row], lx, ry + 35, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, rw - (lx - rx) - 120, 12));
    };
    o.after = [&](int i, float rx, float ry, float rw, float rh) {
        bool marked = PendingOf(list[i]) != nullptr;
        if (Button(marked ? "UNDO" : "RESET", {rx + rw - 102, ry + (rh - 24) / 2, 78, 24}, marked ? NORMAL : DANGER, ui::Id("errorreset", (uint64_t)i), 12))
            Revert(list[i]);
    };
    List(o);
    if (n == 0) g_cv.Text("No errors: every value the editor applied was accepted.", x + 28, hy + 48, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, 0, 12));
    float dy = y + h - detail - 44;
    g_cv.Rect(x + 12, dy, w - 24, 1, LINE, 10);
    if (g_errorsPopup.index < n) g_cv.Text(g_errors[list[g_errorsPopup.index]], x + 24, dy + 18, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, w - 48, 11));
    if (Button("CLOSE", {x + w - 24 - 150, y + h - 40, 150, 30}, NORMAL, ui::Id("errorsdone"), 11)) { g_errorsPopup.on = false; Sound(sounds::BACK); }
    g_cv.Text("\xE2\x86\x91\xE2\x86\x93 select   Enter open the field   Del reset / undo   Esc close", x + 24, y + h - 25, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, 0, 11));
}
} // namespace editorapp
