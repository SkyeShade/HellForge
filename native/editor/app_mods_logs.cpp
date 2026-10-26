/* The editor's window (app_internal.h): the MODS view (the session's mods and the values each holds) and the LOGS
 * view. */
#include <algorithm>
#include "app_internal.h"

namespace editorapp
{
LogView g_logs;
const char* const LOG_FILTERS[] = {"ALL", "ERRORS", "WARNINGS"};

static std::wstring LogPath(int source)
{
    if (g_host.api->size < offsetof(HfApi, logs_folder) + sizeof(void*) || g_host.api->logs_folder == nullptr) return L"";
    const char* folder = g_host.api->logs_folder(g_host.mod);
    if (folder == nullptr || !folder[0]) return L"";
    std::string id = g_host.api->mod_id ? g_host.api->mod_id(g_host.mod) : "hellforge.editor";
    return hfjson::Wide(folder) + L"\\" + (source == 0 ? std::wstring(L"modloader.log") : hfjson::Wide("mod-" + id + ".log"));
}

static int LogKind(const std::string& line)
{
    std::string l = Lower(line);
    if (l.find("refused") != std::string::npos || l.find("crash") != std::string::npos || l.find("error") != std::string::npos ||
        l.find("failed") != std::string::npos || l.find("could not") != std::string::npos || l.find("cannot") != std::string::npos) return 2;
    if (l.find("conflict") != std::string::npos || l.find("waits") != std::string::npos || l.find("warning") != std::string::npos ||
        l.find("not applied") != std::string::npos || l.find("not the reviewed") != std::string::npos) return 1;
    return 0;
}

void LogFilter()
{
    g_logs.shown.clear();
    for (int i = 0; i < (int)g_logs.lines.size(); i++)
        if (g_logs.filter == 0 || g_logs.kinds[i] == (g_logs.filter == 1 ? 2 : 1)) g_logs.shown.push_back(i);
    if (g_logs.follow) { g_logs.index = std::max(0, (int)g_logs.shown.size() - 1); g_seen.erase("logs"); }
    g_logs.index = std::clamp(g_logs.index, 0, std::max(0, (int)g_logs.shown.size() - 1));
}

/* The log's lines and their kinds (cut: the text starts within a line, which is left out; empty lines too). */
static void LogLines(const std::string& text, bool cut)
{
    g_logs.lines.clear();
    g_logs.kinds.clear();
    size_t start = cut ? text.find('\n') + 1 : 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) { g_logs.kinds.push_back(LogKind(line)); g_logs.lines.push_back(line); }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

void LogRead(bool force)
{
    ULONGLONG now = GetTickCount64();
    if (!force && now - g_logs.checked < 1000) return;
    g_logs.checked = now;
    std::wstring path = LogPath(g_logs.source);
    HANDLE file = path.empty() ? INVALID_HANDLE_VALUE :
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) { if (force) { g_logs.lines.clear(); g_logs.kinds.clear(); g_logs.path = path; LogFilter(); } return; }
    LARGE_INTEGER size;
    GetFileSizeEx(file, &size);
    if (!force && path == g_logs.path && (uint64_t)size.QuadPart == g_logs.size) { CloseHandle(file); return; }
    uint64_t keep = std::min<uint64_t>((uint64_t)size.QuadPart, 4u << 20); /* the last 4 MB */
    LARGE_INTEGER at;
    at.QuadPart = size.QuadPart - (LONGLONG)keep;
    SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
    std::string text((size_t)keep, '\0');
    DWORD read = 0;
    ReadFile(file, &text[0], (DWORD)keep, &read, nullptr);
    CloseHandle(file);
    text.resize(read);
    g_logs.path = path;
    g_logs.size = (uint64_t)size.QuadPart;
    LogLines(text, at.QuadPart > 0);
    LogFilter();
}

// Explorer on a path; false when Windows refused (a session's game may not start other programs: its job holds it alone).
static bool OpenWith(const std::wstring& arguments)
{
    std::wstring command = L"explorer.exe " + arguments;
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

/* ---- MODS: the session's mods, HellForge's (code and stats mods) and Lua's (HD2Runtime mods, through the Lua bridge),
 * and the values each one holds: the fields whose holder get_live or get_stat names (the blue MOD mark). A field two mods
 * change is listed under the one whose value it holds. ---- */
static std::vector<ModInfo> g_modList;
static ULONGLONG g_modsAt;
int g_modEntry = -1, g_modRow;
static bool HasMods() { return g_host.api->size >= offsetof(HfApi, mods) + sizeof(void*) && g_host.api->mods != nullptr; }

/* The left list: a heading per group, then its mods (HellForge's in load order, then Lua's as they registered). */
std::vector<ModEntry> ModEntries()
{
    std::vector<ModEntry> out;
    for (int group = 0; group < 2; group++)
    {
        bool lua = group == 1;
        out.push_back({lua ? "LUA MODS" : "HELLFORGE MODS", -1});
        for (int i = 0; i < (int)g_modList.size(); i++) if ((g_modList[i].kind == HF_MOD_LUA) == lua) out.push_back({"", i});
    }
    return out;
}

static const ModInfo* SelectedMod()
{
    auto entries = ModEntries();
    if (g_modEntry < 0 || g_modEntry >= (int)entries.size() || entries[g_modEntry].mod < 0) return nullptr;
    return &g_modList[entries[g_modEntry].mod];
}

std::vector<const Row*> ModRows()
{
    const ModInfo* m = SelectedMod();
    return m != nullptr ? m->rows : std::vector<const Row*>{};
}

/* A Lua mod's name as people read it: the last part of its runtime name, words for underscores ("railgun_repenetrated"
 * becomes "Railgun Repenetrated"). */
static std::string LuaModTitle(const std::string& name)
{
    size_t slash = name.find_last_of('/');
    std::string last = name.substr(slash == std::string::npos ? 0 : slash + 1);
    bool start = true;
    for (char& c : last)
    {
        if (c == '_' || c == '-') { c = ' '; start = true; continue; }
        if (start && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        start = false;
    }
    return last.empty() ? name : last;
}

/* A code or stats mod's name and version, from the manifest in its folder (read once). */
static std::pair<std::string, std::string> ManifestName(const std::string& folder)
{
    static std::map<std::string, std::pair<std::string, std::string>> known;
    auto found = known.find(folder);
    if (found != known.end()) return found->second;
    std::pair<std::string, std::string> out;
    std::string text, why;
    hfjson::Value manifest;
    if (!folder.empty() && hfjson::ReadFile(hfjson::Wide(folder) + L"\\hellforge-mod.json", &text) && hfjson::Parse(text, &manifest, &why))
        out = {manifest.GetString("name"), manifest.GetString("version")};
    known.emplace(folder, out);
    return out;
}

/* A mod as the loader lists it: a Lua mod by its runtime name, a code or stats mod by its manifest's name and version. */
static ModInfo ModOf(const HfModInfo& info)
{
    ModInfo m;
    m.id = info.id;
    m.kind = info.kind;
    m.loaded = info.loaded != 0;
    m.folder = info.folder != nullptr ? info.folder : "";
    if (info.kind == HF_MOD_LUA)
    {
        std::string name = info.name != nullptr ? info.name : "";
        m.name = LuaModTitle(name);
        m.detail = name;
    }
    else
    {
        auto [name, version] = ManifestName(m.folder);
        m.name = name.empty() ? m.id : name;
        m.detail = m.id + (version.empty() ? "" : SEP + version);
    }
    return m;
}

/* The session's mods from the loader (the editor left out). */
static std::vector<ModInfo> LoaderMods()
{
    std::vector<ModInfo> list;
    if (!HasMods()) return list;
    for (uint32_t i = 0; i < 1024; i++)
    {
        HfModInfo info{};
        info.size = sizeof info;
        if (!g_host.api->mods(g_host.mod, i, &info)) break;
        if (info.kind == HF_MOD_EDITOR || info.id == nullptr) continue;
        list.push_back(ModOf(info));
    }
    return list;
}

/* The values each mod holds (the holder index HolderTick keeps), by object. */
static void AddHeldRows(std::vector<ModInfo>& list)
{
    std::map<std::string, size_t> byId;
    for (size_t i = 0; i < list.size(); i++) byId[list[i].id] = i;
    for (auto& [row, holder] : g_holders) /* read a few a frame (HolderTick) */
    {
        auto at = byId.find(holder);
        if (at == byId.end()) continue;
        std::vector<const Row*>& rows = list[at->second].rows;
        bool seen = false; /* the same field on another page's copy: once per object */
        for (const Row* r : rows) seen = seen || (r->object == row->object && r->label == row->label);
        if (!seen) rows.push_back(row);
    }
    for (ModInfo& m : list)
        std::stable_sort(m.rows.begin(), m.rows.end(), [](const Row* a, const Row* b) { return a->object->name < b->object->name; });
}

/* The entry of the mod selected before (it stays selected), else the first mod's; -1 none. */
static int EntryOf(const std::vector<ModEntry>& entries, const std::string& keep)
{
    int entry = -1;
    for (int i = 0; i < (int)entries.size(); i++)
    {
        if (entries[i].mod < 0) continue;
        if (entry < 0) entry = i;
        if (g_modList[entries[i].mod].id == keep) return i;
    }
    return entry;
}

/* The list again (every 2 s while MODS is open, or now): the mods from the loader, their names, and the values each holds
 * (the holder index HolderTick keeps). The selected mod stays selected. */
void ScanMods(bool now)
{
    ULONGLONG tick = GetTickCount64();
    if (!now && g_modsAt != 0 && tick - g_modsAt < 2000) return;
    g_modsAt = tick;
    const ModInfo* was = SelectedMod();
    std::string keep = was != nullptr ? was->id : "";
    std::vector<ModInfo> list = LoaderMods();
    AddHeldRows(list);
    g_modList = std::move(list);
    g_modEntry = EntryOf(ModEntries(), keep);
}

/* A group's heading in the left list, with how many mods it has. */
static void DrawModHeading(const ModEntry& e, float x, float y, float lw, float rh)
{
    bool lua = e.heading == "LUA MODS";
    int count = 0;
    for (const ModInfo& m : g_modList) count += (m.kind == HF_MOD_LUA) == lua;
    g_cv.Text(e.heading, x + 20, y + rh - 16, T(SZ_HEADING, GOLD, ui::TITLE));
    g_cv.Text(count == 0 ? (lua ? "none (an HD2Runtime mod shows once it changes a value)" : "none") : std::to_string(count),
        x + lw - 20, y + rh - 16, T(SZ_TINY, FAINT, ui::BODY, ui::RIGHT, lw - 170));
    g_cv.Rect(x + 16, y + rh - 5, lw - 32, 1, LINE_STRONG, 2);
}

void DrawMods(float y0, float h)
{
    ScanMods(false);
    float W = PANEL_W, LW = 380;
    g_cv.Rect(0, y0, LW, h, PANEL_ALT, 1);
    g_cv.Rect(LW - 1, y0, 1, h, LINE, 2);
    g_cv.Rect(LW, y0, W - LW, h, PANEL, 1);
    if (!HasMods())
    {
        g_cv.Text("This HellForge cannot list its mods (its mod loader is older than SDK 10).", 20, y0 + 30, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, W - 40));
        return;
    }
    auto entries = ModEntries();
    ListOpts o;
    o.x = 0; o.y = y0 + 6; o.w = LW; o.h = h - 12; o.rowH = 46; o.count = (int)entries.size(); o.selected = g_modEntry; o.key = "mods"; o.focused = true;
    o.click = [&](int i) { if (entries[i].mod >= 0) { g_modEntry = i; g_modRow = 0; g_scroll["modrows"] = 0; Sound(sounds::CLICK); } };
    o.draw = [&](int i, float x, float y, float lw, float rh, bool hovered) {
        const ModEntry& e = entries[i];
        if (e.mod < 0) { DrawModHeading(e, x, y, lw, rh); return; }
        const ModInfo& m = g_modList[e.mod];
        bool selected = i == g_modEntry;
        if (selected) { g_cv.Rect(x + 8, y + 2, lw - 16, rh - 4, SELECT, 2); g_cv.Rect(x + 8, y + 2, 3, rh - 4, GOLD, 3); }
        else if (hovered) g_cv.Rect(x + 8, y + 2, lw - 16, rh - 4, HOVER, 2);
        g_cv.Text(m.name, x + 24, y + 15, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, lw - 120));
        g_cv.Text(m.detail, x + 24, y + 32, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, lw - 120));
        if (!m.loaded) Chip("NOT LOADED", x + lw - 18, y + rh / 2, ERROR_, ERROR_SOFT, true);
        else if (!m.rows.empty()) Chip(std::to_string(m.rows.size()), x + lw - 18, y + rh / 2, m.kind == HF_MOD_LUA ? LUA : MOD, m.kind == HF_MOD_LUA ? LUA_SOFT : MOD_SOFT, true);
        else if (!g_holdersRead) Chip("READING", x + lw - 18, y + rh / 2, FAINT, LINE, true);
    };
    List(o);

    float x0 = LW, rw = W - LW;
    const ModInfo* m = SelectedMod();
    if (m == nullptr)
    {
        g_cv.Text("No mods in this session.", x0 + 24, y0 + 34, T(SZ_SMALL, FAINT));
        return;
    }
    const char* kind = m->kind == HF_MOD_LUA ? "LUA MOD" : m->kind == HF_MOD_STATS ? "STATS MOD" : "CODE MOD";
    g_cv.Text(m->name, x0 + 24, y0 + 26, T(20, TEXT, ui::TITLE, ui::LEFT, rw - 200));
    ImU32 tone = m->kind == HF_MOD_LUA ? LUA : MOD;
    Chip(kind, x0 + rw - 20, y0 + 26, tone, m->kind == HF_MOD_LUA ? LUA_SOFT : MOD_SOFT, true);
    std::string about = m->id + SEP + (m->rows.empty() ? std::string("holds no value the editor shows") :
        std::to_string(m->rows.size()) + (m->rows.size() == 1 ? " value it holds" : " values it holds") + "; reset in BROWSE goes back to the mod's value");
    g_cv.Text(about, x0 + 24, y0 + 50, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, rw - 48));
    float hy = y0 + 70;
    g_cv.Rect(x0, hy, rw, 22, HEADER, 2);
    g_cv.Text("OBJECT", x0 + 20, hy + 11, T(SZ_TINY, FAINT, ui::TITLE));
    g_cv.Text("FIELD", x0 + 280, hy + 11, T(SZ_TINY, FAINT, ui::TITLE));
    g_cv.Text("MOD'S VALUE", x0 + rw - 170, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::RIGHT));
    g_cv.Text("NOW", x0 + rw - 24, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::RIGHT));
    const std::vector<const Row*> rows = m->rows;
    if (rows.empty())
        g_cv.Text(m->kind == HF_MOD_LUA ? "None of its changes is a value the editor lists." :
            "It adds content or hooks the game; it changes none of the values the editor lists.", x0 + 24, hy + 44, T(SZ_SMALL, FAINT));
    g_modRow = std::clamp(g_modRow, 0, std::max(0, (int)rows.size() - 1));
    ListOpts r;
    r.x = x0; r.y = hy + 24; r.w = rw; r.h = y0 + h - hy - 28; r.rowH = 32; r.count = (int)rows.size(); r.selected = g_modRow; r.key = "modrows";
    r.click = [&](int i) { if (g_modRow == i) { Reveal(rows[i]); Sound(sounds::OPEN); } else g_modRow = i; };
    r.draw = [&](int i, float x, float y, float lw, float rh, bool hovered) {
        const Row* row = rows[i];
        bool selected = i == g_modRow;
        if (selected) { g_cv.Rect(x + 8, y + 2, lw - 16, rh - 4, SELECT, 2); g_cv.Rect(x + 8, y + 2, 3, rh - 4, tone, 3); }
        else if (hovered) g_cv.Rect(x + 8, y + 2, lw - 16, rh - 4, HOVER, 2);
        float cy = y + rh / 2;
        DrawIcon(row->object, x + 18, y + 4, rh - 8);
        g_cv.Text(row->object->name, x + 18 + rh, cy, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, 260 - rh));
        g_cv.Text(row->label, x + 280, cy, T(SZ_SMALL, TEXT, ui::BODY, ui::LEFT, lw - 280 - 330));
        const LiveState& live = Live(row);
        g_cv.Text(live.read ? Format(row, live.base) : "\xE2\x80\x94", x + lw - 170, cy, T(SZ_LABEL, tone, ui::BODY, ui::RIGHT, 150));
        bool yours = live.read && !Same(row, live.now, live.base); /* an editor change over the mod's */
        g_cv.Text(live.read ? Format(row, live.now) : "\xE2\x80\x94", x + lw - 24, cy, T(SZ_LABEL, yours ? GOLD : DIM, ui::BODY, ui::RIGHT, 130));
    };
    List(r);
}

/* OPEN LOG FILE and SHOW IN FOLDER (a session's game may not start other programs: then the path is said instead) */
static void DrawLogButtons(float W, float y0)
{
    if (Button("OPEN LOG FILE", {W - 216, y0 + 16, 196, 34}, g_logs.path.empty() ? DISABLED : NORMAL, ui::Id("logfile")) && !g_logs.path.empty())
        if (!OpenWith(L"\"" + g_logs.path + L"\"")) Toast("Open it yourself: " + Narrow(g_logs.path), PENDING);
    if (Button("SHOW IN FOLDER", {W - 424, y0 + 16, 196, 34}, g_logs.path.empty() ? DISABLED : NORMAL, ui::Id("logfolder")) && !g_logs.path.empty())
        if (!OpenWith(L"/select,\"" + g_logs.path + L"\"")) Toast("Open it yourself: " + Narrow(g_logs.path), PENDING);
}

/* the source (HellForge's log or the editor's), the filters, following the end */
static void DrawLogChips(float fy)
{
    float fx = 20;
    auto chip = [&](const std::string& label, bool on, ImU32 tone, uint64_t id) {
        float w = g_cv.Measure(label, SZ_TINY, ui::TITLE) + 22;
        g_cv.Rect(fx, fy, w, 22, on ? tone : (g_cv.Hovered() == id ? HOVER : BOX), 3);
        if (!on) g_cv.Frame(fx, fy, w, 22, LINE_STRONG, 4);
        g_cv.Text(label, fx + w / 2, fy + 11, T(SZ_TINY, on ? INVERSE : DIM, ui::TITLE, ui::CENTER, 0, 5));
        bool hit = g_cv.Hit(fx, fy, w, 22, id);
        fx += w + 6;
        return hit;
    };
    if (chip("HELLFORGE", g_logs.source == 0, GOLD, ui::Id("logsrc", 0))) { g_logs.source = g_logSource = 0; g_logs.follow = true; LogRead(true); SaveSettings(); }
    if (chip("EDITOR", g_logs.source == 1, GOLD, ui::Id("logsrc", 1))) { g_logs.source = g_logSource = 1; g_logs.follow = true; LogRead(true); SaveSettings(); }
    fx += 18;
    for (int i = 0; i < 3; i++)
        if (chip(LOG_FILTERS[i], g_logs.filter == i, i == 1 ? ERROR_ : i == 2 ? PENDING : GOLD, ui::Id("logfilter", (uint64_t)i)))
        { g_logs.filter = i; g_logs.follow = true; LogFilter(); }
    uint64_t followId = ui::Id("logfollow");
    g_cv.Rect(fx + 12, fy + 2, 34, 18, g_logs.follow ? GOLD : LINE_STRONG, 3);
    g_cv.Rect(g_logs.follow ? fx + 30 : fx + 14, fy + 4, 14, 14, g_logs.follow ? INVERSE : DIM, 4);
    g_cv.Text("Follow the end", fx + 56, fy + 11, T(SZ_SMALL, DIM));
    if (g_cv.Hit(fx + 12, fy, 160, 22, followId)) { g_logs.follow = !g_logs.follow; LogFilter(); }
}

/* the selected line in full, on two lines when it is long */
static void DrawLogLine(const std::string& line, float W, float dy)
{
    std::string first = g_cv.Fit(line, W - 40, SZ_SMALL);
    if (first.size() >= line.size() || first.size() < 4) { g_cv.Text(line, 20, dy + 12, T(SZ_SMALL, TEXT, ui::BODY, ui::LEFT, W - 40)); return; }
    size_t cut = line.rfind(' ', first.size() - 3);
    if (cut == std::string::npos || cut < 20) cut = first.size() - 3;
    g_cv.Text(line.substr(0, cut), 20, dy + 12, T(SZ_SMALL, TEXT, ui::BODY, ui::LEFT, W - 40));
    g_cv.Text(line.substr(cut + (line[cut] == ' ' ? 1 : 0)), 20, dy + 32, T(SZ_SMALL, TEXT, ui::BODY, ui::LEFT, W - 40));
}

void DrawLogs(float y0, float h)
{
    float W = PANEL_W;
    LogRead(false);
    g_cv.Rect(0, y0, W, h, PANEL, 1);
    g_cv.Text("LOGS", 20, y0 + 24, T(20, TEXT, ui::TITLE));
    g_cv.Text(g_logs.path.empty() ? "This HellForge does not say where its logs are (update HellForge)." : Narrow(g_logs.path),
        20, y0 + 47, T(SZ_SMALL, g_logs.path.empty() ? ERROR_ : FAINT, ui::BODY, ui::LEFT, W - 480));
    DrawLogButtons(W, y0);
    float fy = y0 + 68;
    DrawLogChips(fy);
    int n = (int)g_logs.shown.size();
    g_cv.Text(std::to_string(n) + " lines", W - 20, fy + 11, T(SZ_TINY, FAINT, ui::BODY, ui::RIGHT));
    float ly = fy + 32, detail = 54;
    ListOpts o;
    o.x = 0; o.y = ly; o.w = W; o.h = y0 + h - ly - detail; o.rowH = 22; o.count = n; o.selected = g_logs.index; o.key = "logs"; o.focused = true;
    o.click = [&](int i) { g_logs.index = i; g_logs.follow = i >= n - 1; };
    o.draw = [&](int i, float x, float y, float lw, float rh, bool hovered) {
        int at = g_logs.shown[i];
        ImU32 colour = g_logs.kinds[at] == 2 ? ERROR_ : g_logs.kinds[at] == 1 ? PENDING : DIM;
        if (i == g_logs.index) g_cv.Rect(x + 8, y, lw - 16, rh, SELECT, 2);
        else if (hovered) g_cv.Rect(x + 8, y, lw - 16, rh, HOVER, 2);
        g_cv.Rect(x + 12, y + 4, 3, rh - 8, colour, 3);
        g_cv.Text(g_logs.lines[at], x + 22, y + rh / 2, T(SZ_SMALL, colour, ui::BODY, ui::LEFT, lw - 44));
    };
    List(o);
    float dy = y0 + h - detail + 6;
    g_cv.Rect(0, dy - 6, W, 1, LINE, 2);
    if (g_logs.index < n) DrawLogLine(g_logs.lines[g_logs.shown[g_logs.index]], W, dy);
}
} // namespace editorapp
