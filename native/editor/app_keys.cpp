/* The editor's window (app_internal.h): its keys. */
#include <string.h>
#include <algorithm>
#include "app_internal.h"

namespace editorapp
{
/* ---- keys ---- */
static void NextView(int delta)
{
    g_view = (View)(((int)g_view + delta + VIEW_COUNT) % VIEW_COUNT);
    g_edit.on = false;
    if (g_view == PRESETS) ScanPresets();
    if (g_view == LOGS) LogRead(true);
    if (g_view == MODS) ScanMods(true);
}

static std::vector<std::string> Chars()
{
    std::vector<std::string> out;
    ImGuiIO& io = ImGui::GetIO();
    for (ImWchar c : io.InputQueueCharacters)
    {
        if (c < 0x20 || c == 0x7F) continue;
        char text[5] = {};
        if (c < 0x80) text[0] = (char)c;
        else if (c < 0x800) { text[0] = (char)(0xC0 | (c >> 6)); text[1] = (char)(0x80 | (c & 0x3F)); }
        else { text[0] = (char)(0xE0 | (c >> 12)); text[1] = (char)(0x80 | ((c >> 6) & 0x3F)); text[2] = (char)(0x80 | (c & 0x3F)); }
        out.push_back(text);
    }
    io.InputQueueCharacters.resize(0);
    return out;
}

static void DropLast(std::string& text)
{
    if (text.empty()) return;
    size_t n = text.size() - 1;
    while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    text.resize(n);
}

/* The errors popup: select, Enter opens the field, Del or Backspace reverts it, Esc closes. */
static void ErrorsKeys()
{
    auto list = ErrorRows();
    int n = std::max(1, (int)list.size());
    if (Pressed(ImGuiKey_UpArrow)) g_errorsPopup.index = std::max(0, g_errorsPopup.index - 1);
    if (Pressed(ImGuiKey_DownArrow)) g_errorsPopup.index = std::min(n - 1, g_errorsPopup.index + 1);
    if (Pressed(ImGuiKey_PageUp)) g_errorsPopup.index = std::max(0, g_errorsPopup.index - 8);
    if (Pressed(ImGuiKey_PageDown)) g_errorsPopup.index = std::min(n - 1, g_errorsPopup.index + 8);
    if ((Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter)) && g_errorsPopup.index < (int)list.size())
    { g_errorsPopup.on = false; Reveal(list[g_errorsPopup.index]); Sound(sounds::OPEN); }
    if ((Pressed(ImGuiKey_Delete) || Pressed(ImGuiKey_Backspace)) && g_errorsPopup.index < (int)list.size()) { Revert(list[g_errorsPopup.index]); Sound(sounds::CLICK); }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { g_errorsPopup.on = false; Sound(sounds::BACK); }
}

/* The picker's filter typed into (32 characters at most, not with Ctrl held) or erased; true when it changed. */
static bool PickerTyping(const std::vector<std::string>& chars, bool ctrl)
{
    bool refilter = false;
    for (auto& c : chars) if (g_picker.filter.size() < 32 && !ctrl) { g_picker.filter += c; refilter = true; }
    if (Pressed(ImGuiKey_Backspace) && !g_picker.filter.empty()) { DropLast(g_picker.filter); refilter = true; }
    return refilter;
}

/* The picker's options filtered again, the one selected before (else the row's value) still selected. */
static void Refilter()
{
    double was = PickerHas(g_picker.index) ? g_picker.entries[g_picker.index].value : ViewOf(g_picker.row).value;
    PickerFill();
    PickerSelect(was);
    g_scroll["picker"] = 0;
}

/* The picker: typing filters it, select (headings skipped), Enter chooses, Esc or Del cancels. */
static void PickerKeys(const std::vector<std::string>& chars, bool ctrl)
{
    if (PickerTyping(chars, ctrl)) Refilter();
    if (Pressed(ImGuiKey_UpArrow)) g_picker.index = PickerStep(g_picker.index, -1);
    if (Pressed(ImGuiKey_DownArrow)) g_picker.index = PickerStep(g_picker.index, 1);
    for (int k = 0; k < 8; k++)
    {
        if (Pressed(ImGuiKey_PageUp)) g_picker.index = PickerStep(g_picker.index, -1);
        if (Pressed(ImGuiKey_PageDown)) g_picker.index = PickerStep(g_picker.index, 1);
    }
    if (Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter)) PickerChoose(g_picker.index);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || Pressed(ImGuiKey_Delete)) { g_picker.on = false; Sound(sounds::BACK); }
}

/* The number being typed: digits and a decimal point (the first replaces a fresh buffer), and Backspace. */
static void EditTyping(const std::vector<std::string>& chars)
{
    for (auto& c : chars)
        if (strchr("0123456789.,-+eE", c[0]) && g_edit.buffer.size() < 24)
        {
            if (g_edit.fresh) { g_edit.buffer.clear(); g_edit.fresh = false; }
            g_edit.buffer += c;
        }
    if (Pressed(ImGuiKey_Backspace)) { if (g_edit.fresh) g_edit.buffer.clear(); else DropLast(g_edit.buffer); g_edit.fresh = false; }
}

/* A value being typed: Enter or Tab confirms it, Up and Down confirm and move, Del or Esc cancels. */
static void EditKeys(const std::vector<std::string>& chars)
{
    EditTyping(chars);
    if (Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter) || Pressed(ImGuiKey_Tab)) CommitEdit();
    else if (Pressed(ImGuiKey_UpArrow)) { CommitEdit(); g_field = std::max(0, g_field - 1); }
    else if (Pressed(ImGuiKey_DownArrow)) { CommitEdit(); g_field++; }
    else if (Pressed(ImGuiKey_Delete) || Pressed(ImGuiKey_Escape)) g_edit.on = false;
}

/* BROWSE's search box: typing filters the objects, Tab or Enter is done, Esc clears and leaves it. */
static void SearchBoxKeys(const std::vector<std::string>& chars)
{
    for (auto& c : chars) if (g_search.text.size() < 40) { g_search.text += c; g_obj = 0; g_field = 0; }
    if (Pressed(ImGuiKey_Backspace)) { DropLast(g_search.text); g_obj = 0; }
    if (Pressed(ImGuiKey_Delete)) { g_search.text.clear(); g_obj = 0; }
    if (Pressed(ImGuiKey_Tab) || Pressed(ImGuiKey_Enter)) { g_search.active = false; g_focus = OBJECTS; }
    if (Pressed(ImGuiKey_Escape)) { g_search.active = false; g_search.text.clear(); }
    if (Pressed(ImGuiKey_DownArrow)) { auto n = (int)Objects().size(); if (n) SelectObject(std::min(n - 1, g_obj + 1)); }
    if (Pressed(ImGuiKey_UpArrow)) SelectObject(std::max(0, g_obj - 1));
}

/* The keys of every view: Esc (clears SEARCH's words, else closes), F9 applies, Shift+Tab or F6 the next view, Ctrl+F
 * BROWSE's search, Ctrl+E the changes; true when one was pressed. */
static bool CommonKeys(bool ctrl, bool shift)
{
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        if (g_view == SEARCH && !g_gsearch.text.empty()) { g_gsearch.text.clear(); g_gsearch.index = 0; }
        else g_host.close(); /* Closed() plays the closing sound */
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) { Apply(); return true; }
    if ((Pressed(ImGuiKey_Tab) && shift) || ImGui::IsKeyPressed(ImGuiKey_F6, false)) { NextView(1); Sound(sounds::CLICK); return true; }
    if (ctrl && Pressed(ImGuiKey_F)) { g_view = BROWSE; g_search.active = true; g_focus = OBJECTS; return true; }
    if (ctrl && Pressed(ImGuiKey_E)) { g_view = CHANGES; return true; }
    return false;
}

static void SearchKeys(const std::vector<std::string>& chars)
{
    for (auto& c : chars) if (g_gsearch.text.size() < 40) { g_gsearch.text += c; g_gsearch.index = 0; }
    if (Pressed(ImGuiKey_Backspace)) { DropLast(g_gsearch.text); g_gsearch.index = 0; }
    if (Pressed(ImGuiKey_Delete)) { g_gsearch.text.clear(); g_gsearch.index = 0; }
    auto results = SearchResults(g_gsearch.text);
    int n = std::max(1, (int)results.size());
    if (Pressed(ImGuiKey_UpArrow)) g_gsearch.index = std::max(0, g_gsearch.index - 1);
    if (Pressed(ImGuiKey_DownArrow)) g_gsearch.index = std::min(n - 1, g_gsearch.index + 1);
    if (Pressed(ImGuiKey_PageUp)) g_gsearch.index = std::max(0, g_gsearch.index - 10);
    if (Pressed(ImGuiKey_PageDown)) g_gsearch.index = std::min(n - 1, g_gsearch.index + 10);
    if ((Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter)) && g_gsearch.index < (int)results.size())
    { GotoObject(results[g_gsearch.index].object); g_view = BROWSE; g_focus = OBJECTS; Sound(sounds::OPEN); }
}

static void ChangesKeys()
{
    auto list = Changes();
    int n = std::max(1, (int)list.size());
    if (Pressed(ImGuiKey_UpArrow)) g_change = std::max(0, g_change - 1);
    if (Pressed(ImGuiKey_DownArrow)) g_change = std::min(n - 1, g_change + 1);
    if (g_change < (int)list.size())
    {
        if (Pressed(ImGuiKey_Enter)) { Reveal(list[g_change].row); Sound(sounds::OPEN); }
        else if (Pressed(ImGuiKey_Backspace)) { Revert(list[g_change].row); Sound(sounds::CLICK); }
    }
}

/* The next (or previous) mod in the list, past the headings, its values from the top. */
static void NextModEntry(const std::vector<ModEntry>& entries, int dir)
{
    for (int i = g_modEntry + dir; i >= 0 && i < (int)entries.size(); i += dir)
        if (entries[i].mod >= 0) { g_modEntry = i; g_modRow = 0; g_scroll["modrows"] = 0; break; }
}

static void ModsKeys()
{
    auto entries = ModEntries();
    auto rows = ModRows();
    if (Pressed(ImGuiKey_UpArrow) || Pressed(ImGuiKey_DownArrow)) NextModEntry(entries, Pressed(ImGuiKey_UpArrow) ? -1 : 1);
    if (Pressed(ImGuiKey_PageUp)) g_modRow = std::max(0, g_modRow - 1);
    if (Pressed(ImGuiKey_PageDown)) g_modRow = std::min(std::max(0, (int)rows.size() - 1), g_modRow + 1);
    if ((Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter)) && g_modRow < (int)rows.size()) { Reveal(rows[g_modRow]); Sound(sounds::OPEN); }
    if (Pressed(ImGuiKey_F5)) ScanMods(true);
}

static void LogsKeys()
{
    int n = std::max(1, (int)g_logs.shown.size());
    auto move = [&](int to) { g_logs.index = std::clamp(to, 0, n - 1); g_logs.follow = g_logs.index >= n - 1; };
    if (Pressed(ImGuiKey_UpArrow)) move(g_logs.index - 1);
    if (Pressed(ImGuiKey_DownArrow)) move(g_logs.index + 1);
    if (Pressed(ImGuiKey_PageUp)) move(g_logs.index - 20);
    if (Pressed(ImGuiKey_PageDown)) move(g_logs.index + 20);
    if (Pressed(ImGuiKey_Home)) move(0);
    if (Pressed(ImGuiKey_End)) move(n - 1);
    if (Pressed(ImGuiKey_LeftArrow) || Pressed(ImGuiKey_RightArrow))
    { g_logs.filter = (g_logs.filter + (Pressed(ImGuiKey_LeftArrow) ? 2 : 1)) % 3; g_logs.follow = true; LogFilter(); }
    if (Pressed(ImGuiKey_Enter)) { g_logs.source = g_logSource = 1 - g_logs.source; g_logs.follow = true; LogRead(true); SaveSettings(); }
}

static void PresetsKeys()
{
    int n = std::max(1, (int)g_presets.size());
    if (Pressed(ImGuiKey_UpArrow)) g_preset = std::max(0, g_preset - 1);
    if (Pressed(ImGuiKey_DownArrow)) g_preset = std::min(n - 1, g_preset + 1);
    if ((Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter)) && g_preset < (int)g_presets.size() && g_presets[g_preset].error.empty())
        ImportPreset(g_presets[g_preset]);
}

static void SettingsKeys()
{
    int index = 0;
    for (int i = 0; i < UI_SCALE_COUNT; i++) if (UI_SCALES[i] == g_uiScale) index = i;
    if (Pressed(ImGuiKey_LeftArrow) && index > 0) { g_uiScale = UI_SCALES[index - 1]; SaveSettings(); Sound(sounds::CLICK); }
    if (Pressed(ImGuiKey_RightArrow) && index + 1 < UI_SCALE_COUNT) { g_uiScale = UI_SCALES[index + 1]; SaveSettings(); Sound(sounds::CLICK); }
}

static void CategoryKeys()
{
    if (Pressed(ImGuiKey_UpArrow)) MoveCategory(-1);
    if (Pressed(ImGuiKey_DownArrow)) MoveCategory(1);
    if (Pressed(ImGuiKey_RightArrow) || Pressed(ImGuiKey_Enter)) { g_focus = OBJECTS; if (Pressed(ImGuiKey_Enter)) Sound(sounds::CLICK); }
}

/* The objects: select, Left to the categories, Right or Enter to the fields; a letter or digit starts a search. */
static void ObjectKeys(const std::vector<std::string>& chars)
{
    int n = (int)Objects().size();
    if (Pressed(ImGuiKey_UpArrow)) SelectObject(std::max(0, g_obj - 1));
    if (Pressed(ImGuiKey_DownArrow) && n) SelectObject(std::min(n - 1, g_obj + 1));
    if (Pressed(ImGuiKey_PageUp)) SelectObject(std::max(0, g_obj - 10));
    if (Pressed(ImGuiKey_PageDown) && n) SelectObject(std::min(n - 1, g_obj + 10));
    if (Pressed(ImGuiKey_LeftArrow)) g_focus = CATEGORIES;
    if ((Pressed(ImGuiKey_RightArrow) || Pressed(ImGuiKey_Enter)) && n) { g_focus = FIELDS; if (Pressed(ImGuiKey_Enter)) Sound(sounds::CLICK); }
    for (auto& c : chars) if (isalnum((unsigned char)c[0])) { g_search.active = true; g_search.text = c; g_obj = 0; break; }
}

/* Up, Down, PgUp and PgDn move the focused item (of count). */
static void MoveField(int count)
{
    if (Pressed(ImGuiKey_UpArrow)) g_field = std::max(0, g_field - 1);
    if (Pressed(ImGuiKey_DownArrow)) g_field = std::min(count - 1, g_field + 1);
    if (Pressed(ImGuiKey_PageUp)) g_field = std::max(0, g_field - 10);
    if (Pressed(ImGuiKey_PageDown)) g_field = std::min(count - 1, g_field + 10);
}

/* On a header: Enter opens or closes it, Right opens it, Left closes it (closed already: back to the objects). */
static void HeaderKeys(const Item& item)
{
    if (Pressed(ImGuiKey_Enter)) { Toggle(item.key); Sound(sounds::CLICK); }
    if (Pressed(ImGuiKey_RightArrow)) Toggle(item.key, 1);
    if (Pressed(ImGuiKey_LeftArrow)) { if (item.open) Toggle(item.key, 0); else g_focus = OBJECTS; }
}

/* On a field: Left and Right nudge it, Enter edits it, Del reverts it, and digits start a value. */
static void RowKeys(const Row* row, const std::vector<std::string>& chars, bool shift, bool ctrl)
{
    if (Pressed(ImGuiKey_LeftArrow)) Nudge(row, -1, shift, ctrl);
    if (Pressed(ImGuiKey_RightArrow)) Nudge(row, 1, shift, ctrl);
    if (Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter)) { BeginEdit(row, "", true); Sound(sounds::CLICK); }
    if (Pressed(ImGuiKey_Delete)) { Revert(row); Sound(sounds::CLICK); }
    /* digits start a value: everything typed this frame */
    std::string typed;
    for (auto& c : chars) if (strchr("0123456789.,-", c[0]) && (typed.size() || c[0] != ',')) typed += c;
    if (!typed.empty() && row->choice.empty() && row->pick.empty()) BeginEdit(row, typed, false);
}

static void FieldKeys(const std::vector<std::string>& chars, bool ctrl, bool shift)
{
    Object* object = Current();
    auto items = Items(object);
    if (items.empty()) { if (Pressed(ImGuiKey_LeftArrow)) g_focus = OBJECTS; return; }
    g_field = std::clamp(g_field, 0, (int)items.size() - 1);
    if (ctrl && Pressed(ImGuiKey_UpArrow)) { SetPageOpen(object, false); return; }
    if (ctrl && Pressed(ImGuiKey_DownArrow)) { SetPageOpen(object, true); return; }
    MoveField((int)items.size());
    const Item& item = items[std::clamp(g_field, 0, (int)items.size() - 1)];
    if (item.header) { HeaderKeys(item); return; }
    if (item.row == nullptr) { if (Pressed(ImGuiKey_LeftArrow)) g_focus = OBJECTS; return; } /* a note */
    RowKeys(item.row, chars, shift, ctrl);
}

static void BrowseKeys(const std::vector<std::string>& chars, bool ctrl, bool shift)
{
    if (Pressed(ImGuiKey_Tab)) { g_focus = g_focus == CATEGORIES ? OBJECTS : g_focus == OBJECTS ? FIELDS : CATEGORIES; return; }
    if (g_focus == CATEGORIES) { CategoryKeys(); return; }
    if (g_focus == OBJECTS) { ObjectKeys(chars); return; }
    FieldKeys(chars, ctrl, shift);
}

/* The open view's own keys. */
static void ViewKeys(const std::vector<std::string>& chars, bool ctrl, bool shift)
{
    switch (g_view)
    {
    case SEARCH: SearchKeys(chars); break;
    case CHANGES: ChangesKeys(); break;
    case MODS: ModsKeys(); break;
    case LOGS: LogsKeys(); break;
    case PRESETS: PresetsKeys(); break;
    case SETTINGS: SettingsKeys(); break;
    default: BrowseKeys(chars, ctrl, shift); break;
    }
}

/* This frame's keys (the characters typed are taken from ImGui's queue every frame): to mod sync's dialog, the errors
 * popup, the picker, the value being typed or BROWSE's search box when one is up, else the views'. */
void HandleKeys()
{
    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    auto chars = Chars();
    if (g_sync.on) { SyncKeys(); return; }
    if (g_errorsPopup.on) { ErrorsKeys(); return; }
    if (g_picker.on) { PickerKeys(chars, ctrl); return; }
    if (g_edit.on) { EditKeys(chars); return; }
    if (g_view == BROWSE && g_search.active) { SearchBoxKeys(chars); return; }
    if (CommonKeys(ctrl, shift)) return;
    ViewKeys(chars, ctrl, shift);
}
} // namespace editorapp
