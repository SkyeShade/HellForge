/* The editor's window (app.h): the HD2R Editor's layout, look and keys (its ui/app.lua, ui/views.lua, ui/search_view.lua
 * and ui/theme.lua), over HellForge's catalogue (catalog.h) and SDK. Sizes are 1080p units: the panel is
 * 1260 units wide, centred, as tall as fits up to 930, scaled with the screen's height times the interface size.
 *
 * Values: every change and every reset is staged (PENDING, PENDING RESET) until APPLY (F9), so DISCARD takes back any
 * of them, the big resets too. Applied, set_live changes the running game and the change is saved into Local Stats
 * (local_stats.h), so the stats pass applies it whenever the game loads; a reset (or a value back at its default) takes
 * the field out of Local Stats, and set_live with HF_LIVE_BASE puts the base back. DEFAULT is the field's base (get_live:
 * another mod's value, else the game's). Fields live editing does not reach (attachment changes) are saved only, and
 * apply from the next boot.
 * Look: the HD2R Editor's layout and yellow highlights on lighter panels, HellForge's icon and red in the title; the
 * game's menu sounds (sounds.h). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include "app_internal.h"
#include "icons.h"

namespace editorapp
{
/* ---- state (app_internal.h) ---- */
Host g_host;
catalog::Catalog g_catalog;
bool g_catalogOk;
std::string g_catalogError;
uint32_t g_layout;
Canvas g_cv;
float g_time, g_panelH = PANEL_H, g_scaleNow = 1;
View g_view = BROWSE;
Focus g_focus = CATEGORIES;
std::vector<CatItem> g_cats;
int g_cat = 1, g_obj = 0, g_field = 0;
std::map<std::string, int> g_scroll, g_seen;
float g_catScroll = 0;
SearchBox g_search;
std::map<std::string, std::string> g_group;
double g_choice[2] = {-1, -1};
Picker g_picker;
std::map<std::string, double> g_mounts;
std::map<std::string, double> g_hostMounts;
bool g_hostMountsShown;
std::map<const Row*, std::string> g_errors;
unsigned g_frame = 1, g_errorsVersion = 1, g_layoutVersion = 1;
std::map<const Row*, Pend> g_pending;
bool g_soundsOn = true;
int g_logSource;
Edit g_edit;
std::set<std::string> g_open;
float g_uiScale = 1;
ToastLine g_toast;
GlobalSearch g_gsearch;
int g_change;
std::map<const Row*, LiveState> g_live;
static LiveFrameReads g_liveFrame;
LONGLONG QpcFrequency() { static LARGE_INTEGER f; if (f.QuadPart == 0) QueryPerformanceFrequency(&f); return f.QuadPart; }
LONGLONG QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
LiveFrameReads& LiveFrame()
{
    if (g_liveFrame.frame != g_frame) g_liveFrame = {g_frame, 0, 0, 0};
    return g_liveFrame;
}
Drag g_drag;
unsigned g_markVersion = 1;

void Toast(const std::string& text, ImU32 colour)
{
    g_toast.on = true; g_toast.text = text; g_toast.colour = colour; g_toast.time = g_time;
    if (colour == ERROR_) Sound(sounds::ERROR_);
}

/* HellForge's messages for the player (SDK 12, HfApi notice; docs/mods.md "Playing with friends"): why a friend's lobby
 * refused this game's join, or why a friend could not join this one. Each new one shows over the game for a while, the
 * editor open or not (Overlay draws it while the window is closed). */
static struct { uint32_t version = 0; bool error = false, brief = false; std::string title; std::vector<std::string> lines; ULONGLONG at = 0; } g_notice;
const ULONGLONG NOTICE_ERROR_MS = 20000, NOTICE_INFO_MS = 12000, NOTICE_BRIEF_MS = 6000, NOTICE_FADE_MS = 800;
static bool HasNotice() { return g_host.api->size >= offsetof(HfApi, notice) + sizeof(void*) && g_host.api->notice != nullptr; }
/* The notice's text as its lines, one per '\n' (empty ones left out). */
static void ReadNoticeLines(const char* text)
{
    g_notice.lines.clear();
    for (const char *p = text, *end; *p; p = *end ? end + 1 : end)
    {
        end = strchr(p, '\n');
        if (end == nullptr) end = p + strlen(p);
        if (end > p) g_notice.lines.emplace_back(p, end);
    }
}
static void ReadNotice()
{
    if (!HasNotice()) return;
    HfNotice now{};
    g_host.api->notice(g_host.mod, &now);
    if (now.version == g_notice.version) return;
    now.title[sizeof now.title - 1] = 0;
    now.text[sizeof now.text - 1] = 0;
    g_notice.version = now.version;
    g_notice.error = now.kind == HF_NOTICE_ERROR;
    g_notice.brief = now.kind == HF_NOTICE_BRIEF;
    g_notice.title = now.title;
    if (!g_notice.brief) /* a heading, as the editor's (UTF-8 kept); a brief one keeps the friend's name as it is */
        for (char& c : g_notice.title) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    ReadNoticeLines(now.text);
    g_notice.at = GetTickCount64();
    if (g_notice.error) Sound(sounds::ERROR_);
}
static ULONGLONG NoticeMs() { return g_notice.error ? NOTICE_ERROR_MS : g_notice.brief ? NOTICE_BRIEF_MS : NOTICE_INFO_MS; }
static ImU32 Faded(ImU32 colour, float alpha) { return (colour & 0x00FFFFFF) | ((ImU32)((float)(colour >> 24) * alpha + 0.5f) << 24); }

/* The notice's lines wrapped to the width, at most 14 (the last "..."); *lead the first line's rows. */
static std::vector<std::string> NoticeLines(float w, size_t* lead)
{
    std::vector<std::string> lines;
    *lead = 0; /* the first line's rows, brighter: what happened; the rest say why */
    for (const std::string& line : g_notice.lines)
    {
        for (std::string& part : Wrapped(line, w - 56, SZ_LABEL)) lines.push_back(std::move(part));
        if (*lead == 0) *lead = lines.size();
    }
    if (lines.size() > 14) { lines.resize(14); lines.back() = "..."; }
    return lines;
}

/* The message at the top of the screen, over the game and the editor alike. A refused join's notice gives way to mod
 * sync's dialog, which lists the same mods and what happens to each: once that is up, the notice is done. */
static void DrawNotice()
{
    if (g_notice.version == 0) return;
    if (SyncShown()) { g_notice.at = 0; return; }
    ULONGLONG age = GetTickCount64() - g_notice.at, total = NoticeMs();
    if (age >= total) return;
    float alpha = age + NOTICE_FADE_MS > total ? (float)(total - age) / (float)NOTICE_FADE_MS : 1.0f;
    ImU32 accent = g_notice.error ? ERROR_ : GOLD;
    if (g_notice.brief) /* a small note in the top right corner: the title and the first line (the host's "<friend> couldn't join") */
    {
        float bw = std::min(440.0f, g_cv.width - 40), bx = floorf(g_cv.width - g_cv.ox - bw - 24), by = 24 - g_cv.oy;
        bool line = !g_notice.lines.empty();
        float bh = line ? 58 : 36;
        g_cv.Rect(bx - 3, by - 3, bw + 6, bh + 6, Faded(SHADOW, alpha), 13);
        g_cv.Rect(bx, by, bw, bh, Faded(PANEL_ALT, alpha), 13);
        g_cv.Frame(bx, by, bw, bh, Faded(LINE_STRONG, alpha), 14);
        g_cv.Rect(bx, by, 3, bh, Faded(accent, alpha), 14);
        g_cv.Text(g_notice.title, bx + 16, by + 18, T(SZ_LABEL, Faded(TEXT, alpha), ui::BODY, ui::LEFT, bw - 32, 15));
        if (line) g_cv.Text(g_notice.lines[0], bx + 16, by + 40, T(SZ_SMALL, Faded(DIM, alpha), ui::BODY, ui::LEFT, bw - 32, 15));
        return;
    }
    float w = std::min(860.0f, g_cv.width - 40), x = floorf(g_cv.width / 2 - g_cv.ox - w / 2), y = 36 - g_cv.oy;
    size_t lead;
    std::vector<std::string> lines = NoticeLines(w, &lead);
    float h = 62 + 24.0f * (float)lines.size() + (lines.empty() ? 0 : 12);
    g_cv.Rect(x - 4, y - 4, w + 8, h + 8, Faded(SHADOW, alpha), 13);
    g_cv.Rect(x, y, w, h, Faded(PANEL_ALT, alpha), 13);
    g_cv.Frame(x, y, w, h, Faded(LINE_STRONG, alpha), 14);
    g_cv.Rect(x, y, w, 3, Faded(accent, alpha), 14);
    float bw = g_cv.Text("HELLFORGE", x + w - 24, y + 30, T(SZ_TINY, Faded(FAINT, alpha), ui::TITLE, ui::RIGHT, 0, 15));
    g_cv.Text(g_notice.title, x + 24, y + 30, T(18, Faded(accent, alpha), ui::TITLE, ui::LEFT, w - 72 - bw, 15));
    if (lines.empty()) return;
    g_cv.Rect(x + 12, y + 52, w - 24, 1, Faded(LINE, alpha), 14);
    for (size_t i = 0; i < lines.size(); i++)
        g_cv.Text(lines[i], x + 24, y + 74 + 24.0f * (float)i, T(SZ_LABEL, Faded(i < lead ? TEXT : DIM, alpha), ui::BODY, ui::LEFT, w - 48, 15));
}

static void Draw()
{
    g_cv.Rect(-4, -4, PANEL_W + 8, g_panelH + 8, SHADOW, 0);
    g_cv.Rect(0, 0, PANEL_W, g_panelH, PANEL, 0);
    int errors = (int)g_errors.size(), active = (int)SavedRows().size();
    DrawHeader(errors, active);
    float bodyY = HEADER_H, bodyH = g_panelH - HEADER_H - FOOTER_H - STATUS_H;
    if (!g_catalogOk) { g_cv.Rect(0, bodyY, PANEL_W, bodyH, PANEL, 1); g_cv.Text(g_catalogError, 24, bodyY + 40, T(SZ_LABEL, ERROR_, ui::BODY, ui::LEFT, PANEL_W - 48)); }
    else if (g_view == BROWSE) { DrawCategories(bodyY, bodyH); DrawObjects(bodyY, bodyH); DrawFields(bodyY, bodyH); }
    else if (g_view == SEARCH) DrawSearch(bodyY, bodyH);
    else if (g_view == CHANGES) DrawChanges(bodyY, bodyH);
    else if (g_view == MODS) DrawMods(bodyY, bodyH);
    else if (g_view == PRESETS) DrawPresets(bodyY, bodyH);
    else if (g_view == LOGS) DrawLogs(bodyY, bodyH);
    else DrawSettings(bodyY, bodyH);
    DrawStatus(g_panelH - FOOTER_H - STATUS_H, STATUS_H);
    DrawFooter(g_panelH - FOOTER_H, FOOTER_H);
    if (Locked()) g_cv.Rect(0, bodyY, PANEL_W, g_panelH - bodyY, C(40, 42, 48, 150), 7); /* read-only: greyed, still browsable */
    DrawPicker();
    DrawErrors();
    DrawSync();
    /* the holders read after the page, with what its own reads left of the frame's budget: what is on screen comes first */
    LONGLONG used = LiveFrame().ticks * 1000000 / QpcFrequency();
    if (used < LIVE_BUDGET_US) HolderTick(LIVE_BUDGET_US - used);
}

/* ---- co-op: the host's saved changes ----
 * The values of content mods added are this editor's own live edits (mod-stats.json), not Local Stats, so the session's
 * backend, which plays the host's Local Stats, does not reach them. Joined to a host who shares its stats (Locked), the
 * editor takes its own off and applies the host's, which the backend writes beside them (host-mod-stats.json); back on
 * its own, the host's go and its own come back. It also shows the host's saved changes as its own (marks, ACTIVE, the
 * Changes list), read-only: the host's Local Stats file (host-stats.json, which the backend has applied) and mod content
 * values. Joined to a host who lets every player keep their own, nothing here changes. */
static std::map<const Row*, double> g_modLive; /* what the editor's live edits of mod content hold now */
static int g_modRole = -1;                     /* HF_SESSION_JOINER: they are the host's; else this machine's (-1: none yet) */
static std::string g_hostModText, g_hostStatsText, g_hostMountsText; /* the host's files as last applied and shown */

/* Every mount slot to the arm swaps shown (the host's while joined, else this machine's), or back to its vanilla weapon. */
static void SendAllMounts()
{
    for (const Row* row : g_choiceRows) if (IsMount(row)) SendMount(row);
    g_markVersion++;
}
static ULONGLONG g_modCheck;

/* A mod category's numbers as entries (category, target, key, value). */
static void AddModEntries(const std::pair<std::string, hfjson::Value>& category, std::vector<localstats::Entry>& out)
{
    for (auto& target : category.second.members)
        for (auto& field : target.second.members)
            if (field.second.kind == hfjson::Value::Number) out.push_back({category.first, target.first, field.first, field.second.AsNumber()});
}

static std::vector<localstats::Entry> ModEntriesOf(const hfjson::Value& document)
{
    std::vector<localstats::Entry> out;
    for (auto& category : document.members)
        if (localstats::IsMod(category.first) && category.second.kind == hfjson::Value::Object) AddModEntries(category, out);
    return out;
}

/* The mod content rows these entries set, and their values (-1: unlimited). */
static std::map<const Row*, double> WantedValues(const std::vector<localstats::Entry>& entries)
{
    std::map<const Row*, double> want;
    for (const localstats::Entry& e : entries)
    {
        if (!localstats::IsMod(e.category)) continue;
        const Row* row = g_catalog.Entry(e.category, e.target, e.key);
        if (row != nullptr && row->Live()) want[row] = row->unlimited && e.value == -1 ? 4294967295.0 : e.value;
    }
    return want;
}

/* The mod content rows the editor's live edits hold, and those this machine saved (an APPLY may have set them since). */
static std::set<const Row*> HeldRows()
{
    std::set<const Row*> undo;
    for (auto& held : g_modLive) undo.insert(held.first);
    for (const localstats::Entry& e : localstats::OwnEntries())
        if (const Row* row = localstats::IsMod(e.category) ? g_catalog.Entry(e.category, e.target, e.key) : nullptr; row != nullptr && row->Live()) undo.insert(row);
    return undo;
}

/* The held rows not wanted back to their base; how many. */
static int UndoValues(const std::set<const Row*>& undo, const std::map<const Row*, double>& want)
{
    int undone = 0;
    for (const Row* row : undo)
    {
        if (want.count(row)) continue;
        HfField field = FieldOf(row);
        if (g_host.api->set_live(g_host.mod, row->ref, &field, nullptr, row->shared ? HF_LIVE_SHARED : 0)) undone++;
    }
    return undone;
}

/* The wanted values to the game (a refusal is the row's error); how many it took. */
static int SetValues(const std::map<const Row*, double>& want, const char* refused)
{
    int applied = 0;
    for (auto& [row, value] : want)
    {
        HfField field = FieldOf(row);
        HfValue v{row->type, value, 0};
        if (g_host.api->set_live(g_host.mod, row->ref, &field, &v, row->shared ? HF_LIVE_SHARED : 0)) applied++;
        else SetError(row, Refusal(refused));
    }
    return applied;
}

/* The game's mod content to these values; the editor's other live edits of it (and this machine's saved values, which an
 * APPLY may have set since) undone. Returns how many were applied. */
static int ApplyModValues(const std::vector<localstats::Entry>& entries, const char* whose, const char* refused)
{
    std::map<const Row*, double> want = WantedValues(entries);
    int undone = UndoValues(HeldRows(), want);
    int applied = SetValues(want, refused);
    g_modLive = want;
    g_live.clear();
    if (g_modRole != -1 && (applied || undone)) g_host.api->log(g_host.mod, "editor: %s mod content values: %d applied, %d undone", whose, applied, undone);
    return applied;
}

/* Back on this machine's own stats: the host's values and arm swaps go, this machine's come back. */
static void BackToOwn()
{
    g_modRole = HF_SESSION_SOLO; /* this machine's own again */
    g_hostModText.clear();
    g_hostStatsText.clear();
    g_hostMountsText.clear();
    localstats::ShowOwn();
    if (g_hostMountsShown) { g_hostMountsShown = false; g_hostMounts.clear(); SendAllMounts(); }
    ApplyModValues(localstats::OwnEntries(), "this machine's", "HellForge refused its saved value");
}

/* The host's files beside the editor, as the session's backend writes them ("" none yet). */
static void ReadHostFiles(std::string* text, std::string* stats, std::string* mounts)
{
    std::wstring folder = hfjson::Wide(g_host.modFolder);
    if (!hfjson::ReadFile(folder + L"\\host-mod-stats.json", text)) text->clear(); /* none (yet) */
    if (!hfjson::ReadFile(folder + L"\\host-stats.json", stats)) stats->clear();
    if (!hfjson::ReadFile(folder + L"\\host-mounts.json", mounts)) mounts->clear();
}

/* The host's files parsed; false while one is being written (read again the next second). */
static bool ParseHostFiles(const std::string& text, const std::string& stats, const std::string& mounts, hfjson::Value* document,
    hfjson::Value* statsDocument, hfjson::Value* mountsDocument)
{
    if (!text.empty() && !hfjson::Parse(text, document, nullptr)) return false; /* being written: the next second */
    if (!stats.empty() && !hfjson::Parse(stats, statsDocument, nullptr)) return false;
    if (!mounts.empty() && !hfjson::Parse(mounts, mountsDocument, nullptr)) return false;
    return true;
}

/* The host's arm swaps, shown and sent in place of this machine's. */
static void ShowHostMounts(const hfjson::Value& mountsDocument)
{
    g_hostMounts.clear();
    if (mountsDocument.kind == hfjson::Value::Object)
        for (auto& m : mountsDocument.members) if (m.second.kind == hfjson::Value::Number) g_hostMounts[m.first] = m.second.AsNumber();
    g_hostMountsShown = true;
    SendAllMounts(); /* vehicles called in from now on carry the host's swaps */
    g_host.api->log(g_host.mod, "editor: the host's arm swaps: %d", (int)g_hostMounts.size());
}

/* Joined to a host who shares its stats: the host's values, read again each second (the backend rewrites the files when
 * the host changes them). */
static void SyncModValues()
{
    if (!Locked())
    {
        if (g_modRole == HF_SESSION_JOINER) BackToOwn();
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (g_modRole == HF_SESSION_JOINER && now < g_modCheck) return;
    g_modCheck = now + 1000;
    std::string text, stats, mounts;
    ReadHostFiles(&text, &stats, &mounts);
    if (g_modRole == HF_SESSION_JOINER && text == g_hostModText && stats == g_hostStatsText && mounts == g_hostMountsText) return;
    hfjson::Value document, statsDocument, mountsDocument;
    if (!ParseHostFiles(text, stats, mounts, &document, &statsDocument, &mountsDocument)) return;
    bool joining = g_modRole != HF_SESSION_JOINER;
    bool modsChanged = joining || text != g_hostModText, mountsChanged = joining || mounts != g_hostMountsText;
    g_hostModText = text;
    g_hostStatsText = stats;
    g_hostMountsText = mounts;
    g_modRole = HF_SESSION_JOINER;
    localstats::ShowHost(stats, text); /* from the moment of joining: empty until the host's files arrive */
    if (modsChanged) ApplyModValues(ModEntriesOf(document), "the host's", "HellForge refused the host's value");
    if (mountsChanged) ShowHostMounts(mountsDocument);
}

/* The layout for a screen: the interface size as large as still fits (full width, at least PANEL_MIN_H of height). */
static void Layout(float width, float height, float* ox, float* oy, float* s)
{
    float base = height / 1080.0f, sw = width / base, sh = 1080.0f;
    float k = std::min(std::min(g_uiScale, sw / (PANEL_W + 24)), sh / (PANEL_MIN_H + 24));
    k = std::max(k, 0.5f);
    g_scaleNow = k;
    float W = sw / k, H = sh / k;
    g_panelH = floorf(std::min(PANEL_H, H - 24));
    *ox = std::max(0.0f, floorf((W - PANEL_W) / 2 + 0.5f));
    *oy = std::max(0.0f, floorf((H - g_panelH) / 2 + 0.5f));
    *s = base * k;
}

/* The category list: each group's heading, then its categories (the first one selected). */
static void BuildCategoryList()
{
    for (int n = 0; n < (int)g_catalog.navs.size(); n++)
    {
        if (n == 0 || g_catalog.navs[n].group != g_catalog.navs[n - 1].group) g_cats.push_back({true, g_catalog.navs[n].group, -1});
        g_cats.push_back({false, g_catalog.navs[n].label, n});
    }
    g_cat = g_cats.size() > 1 ? 1 : 0;
}

/* The rows the editor keeps itself, and what it kept of them from last time sent to the loader. */
static void SendKeptChoices()
{
    for (auto& r : g_catalog.rows) if (!r.choice.empty() && !IsApLabel(&r)) g_choiceRows.push_back(&r);
    if (g_choice[0] >= 0 || g_choice[1] >= 0) SendPassives(); /* the armor perks kept from last time */
    for (const Row* row : g_choiceRows) /* the arm swaps, pods and row copies kept from last time (a row several pages show is sent for each: the same value) */
        if (IsMount(row) && g_mounts.count(MountKey(row))) SendMount(row);
}

static std::wstring IconFile(const std::string& folder, const std::string& resource);
void Init(const Host& host)
{
    g_host = host;
    LoadSettings();
    ui::SetFontFolder(host.modFolder);
    sounds::SetEnabled(g_soundsOn);
    std::wstring folder = hfjson::Wide(host.modFolder);
    ULONGLONG loading = GetTickCount64();
    g_catalogOk = catalog::Load(folder + L"\\editor-catalog.json", &g_catalog, &g_catalogError);
    loading = GetTickCount64() - loading;
    g_layout = (uint32_t)strtoul(g_catalog.build.c_str(), nullptr, 10);
    if (g_catalogOk && g_catalog.build != host.api->build_id())
    { g_catalogOk = false; g_catalogError = "editor-catalog.json is for game build " + g_catalog.build + ", the game runs " + host.api->build_id(); }
    int iconCount = icons::Load(folder + L"\\game-icons.bin");
    /* level 11's icon: the game has none, the bundled Hell Forge difficulty brings its own (its data spec's image) */
    if (!host.modsFolder.empty())
        if (std::wstring file = IconFile(host.modsFolder + "\\hell-forge", "skyeshade/hell_forge/icons/hell_forge"); !file.empty())
            icons::AddImage(icons::DIFFICULTY, "11", file);
    std::string error;
    localstats::SetModFile(folder + L"\\mod-stats.json");
    if (!host.modsFolder.empty() && !localstats::Load(host.modsFolder, &error)) Toast("Local Stats: " + error, ERROR_);
    BuildCategoryList();
    g_logs.source = g_logSource == 1 ? 1 : 0;
    SendKeptChoices();
    host.api->log(host.mod, "editor: %s; %d game icons%s", g_catalogOk ? (std::to_string(g_catalog.objects.size()) + " objects, " +
        std::to_string(g_catalog.rows.size()) + " fields (read in " + std::to_string(loading) + " ms)").c_str() : g_catalogError.c_str(), iconCount,
        HasGetLive() ? "" : "; this HellForge has no get_live (values are not shown)");
}

/* The resource of that name with an icon in a mod's data (its patches' resources), or nullptr. */
static const hfjson::Value* IconResource(const hfjson::Value& data, const std::string& resource)
{
    const hfjson::Value* patches = data.Find("patches");
    if (patches == nullptr) return nullptr;
    for (const hfjson::Value& p : patches->items)
        if (const hfjson::Value* resources = p.Find("resources"))
            for (const hfjson::Value& r : resources->items)
                if (r.GetString("name") == resource && !r.GetString("icon").empty()) return &r;
    return nullptr;
}

/* An icon's file in the mod's folder (its slashes Windows'), or "" when its path leaves the folder. */
static std::wstring IconPath(const std::wstring& root, const std::string& icon)
{
    std::wstring file = hfjson::Wide(icon);
    for (wchar_t& c : file) if (c == L'/') c = L'\\';
    return file.find(L"..") == std::wstring::npos ? root + L"\\" + file : L"";
}

/* The PNG a mod's data builds an icon resource from (its hellforge-data.json: {"icon": file, "name": resource}), or "". */
static std::wstring IconFile(const std::string& folder, const std::string& resource)
{
    std::string text, why;
    hfjson::Value data;
    std::wstring root = hfjson::Wide(folder);
    if (!hfjson::ReadFile(root + L"\\hellforge-data.json", &text) || !hfjson::Parse(text, &data, &why)) return L"";
    const hfjson::Value* r = IconResource(data, resource);
    return r != nullptr ? IconPath(root, r->GetString("icon")) : L"";
}

/* Every content the mods added (SDK 7, HfApi.content). */
static std::vector<HfContent> AllContent(const HfApi* api)
{
    std::vector<HfContent> all;
    for (uint32_t i = 0; i < 4096; i++)
    {
        HfContent c{};
        c.size = sizeof c;
        if (!api->content(g_host.mod, i, &c)) break;
        all.push_back(c);
    }
    return all;
}

/* The entities standing for the page's (remap.entity); returns the designation of the first that has one ("" none). */
static std::string RemapEntities(const HfContent& s, const std::vector<HfContent>& all, catalog::Remap& remap)
{
    std::string designation;
    for (const HfEditorEntity* e = s.editor_entities; e != nullptr && e->entity != 0; e++)
    {
        remap.entity[e->like] = e->entity;
        for (const HfContent& c : all)
            if (c.kind == HF_KIND_ENTITY && c.id == e->entity && c.designation != nullptr && designation.empty()) designation = c.designation;
    }
    return designation;
}

/* A u32 of a record (a settings row, an entity's component) read through get_live; false when it cannot be read. */
static bool ReadU32(const char* component, uint64_t target, uint16_t offset, uint64_t* out)
{
    if (!HasGetLive()) return false;
    HfField field{component, g_layout, offset, 4, HF_TYPE_U32, 0};
    HfLiveValue value;
    if (!g_host.api->get_live(g_host.mod, target, &field, &value)) return false;
    *out = (uint64_t)value.now.number;
    return true;
}

/* what each entity standing in fires, against what the page's entity fires, and the damage of each (read from the
 * game) */
static void RemapFired(catalog::Remap& remap)
{
    for (auto& [likeEntity, copyEntity] : remap.entity)
    {
        uint64_t a, b;
        if (ReadU32("ProjectileWeaponComponentData", likeEntity, 0, &a) && ReadU32("ProjectileWeaponComponentData", copyEntity, 0, &b) && a != b && b != 0)
            remap.projectile[a] = b;
    }
    for (auto& [a, b] : remap.projectile)
    {
        uint64_t da, db;
        if (ReadU32("ProjectileSettings", a, 0x3C, &da) && ReadU32("ProjectileSettings", b, 0x3C, &db) && da != db && db != 0) remap.damage[da] = db;
    }
}

/* A content of the stratagem's own mod. */
static bool SameMod(const HfContent& c, const HfContent& s) { return c.mod != nullptr && s.mod != nullptr && !strcmp(c.mod, s.mod); }

/* How many projectiles and damage records the stratagem's mod copied from each base. */
static void CountCopies(const HfContent& s, const std::vector<HfContent>& all, std::map<uint64_t, int>& projectiles, std::map<uint64_t, int>& damages)
{
    for (const HfContent& c : all)
    {
        if (!SameMod(c, s)) continue;
        if (c.kind == HF_KIND_PROJECTILE) projectiles[c.base]++;
        if (c.kind == HF_KIND_DAMAGE) damages[c.base]++;
    }
}

/* else the projectiles and damage its mod copied from the page's, one copy of each base */
static void RemapCopied(const HfContent& s, const std::vector<HfContent>& all, catalog::Remap& remap)
{
    std::map<uint64_t, int> projectiles, damages;
    CountCopies(s, all, projectiles, damages);
    for (const HfContent& c : all)
    {
        if (!SameMod(c, s)) continue;
        if (c.kind == HF_KIND_PROJECTILE && projectiles[c.base] == 1) remap.projectile.emplace(c.base, c.id);
        if (c.kind == HF_KIND_DAMAGE && damages[c.base] == 1) remap.damage.emplace(c.base, c.id);
    }
}

/* The page's name: the mod's title (else the page's), after its entity's designation where the title lacks it. */
static std::string StratagemName(const HfContent& s, const Object* like, const std::string& designation)
{
    std::string title = s.title != nullptr ? s.title : like->name;
    return designation.empty() || title.rfind(designation, 0) == 0 ? title : designation + " " + title;
}

/* its loadout section: the one its mod gave it, else its page's */
static std::string StratagemNav(const HfContent& s, const Object* like)
{
    return s.category == HF_CATEGORY_OFFENSIVE ? "offensive" : s.category == HF_CATEGORY_DEFENSIVE ? "defensive"
        : s.category == HF_CATEGORY_MISSION ? "mission" : s.category == HF_CATEGORY_SUPPLY && like->tone != "support" ? "support" : "";
}

/* The page's icon: the mod's own image, where its data names one (else the page's). */
static void SetPageIcon(Object* o, const HfContent& s)
{
    if (s.icon == nullptr || s.folder == nullptr) return;
    std::wstring file = IconFile(s.folder, s.icon);
    if (!file.empty() && icons::AddStratagem(s.icon, file)) { o->icon = "stratagem"; o->sprite = s.icon; }
    else g_host.api->log(g_host.mod, "editor: %s: its icon's image was not found in its mod's data; it shows the page's", s.name);
}

/* A custom stratagem's page: a copy of the page of the stratagem it is like, pointed at its own settings row, entities,
 * projectiles and damage; false (logged why) when it gets none. */
static bool AddStratagemPage(const HfContent& s, const std::vector<HfContent>& all)
{
    const HfApi* api = g_host.api;
    const Object* like = g_catalog.StratagemObject(s.editor_like);
    if (like == nullptr) { api->log(g_host.mod, "editor: %s has no page (stratagem type %llu has none)", s.name, (unsigned long long)s.editor_like); return false; }
    /* its own settings row, the entities standing for the page's, and the projectiles and damage its mod copied from
     * the page's (one copy of each: two of the same base would be a guess) */
    catalog::Remap remap;
    remap.stratagem[s.editor_like] = s.id;
    std::string designation = RemapEntities(s, all, remap);
    RemapFired(remap);
    RemapCopied(s, all, remap);
    std::string name = StratagemName(s, like, designation);
    std::string nav = StratagemNav(s, like);
    /* an entity's fields only where it has the record (a Pelican's turret has no hellpod payload, say) */
    auto keep = [&](const Row& r, uint64_t ref) {
        if (r.component.find("ComponentData") == std::string::npos || !HasGetLive()) return true;
        HfField field = FieldOf(&r);
        HfLiveValue value;
        return api->get_live(g_host.mod, ref, &field, &value) != 0;
    };
    Object* o = g_catalog.AddCopy(*like, {std::string("mod/") + s.name, name, std::string("mod:") + s.name, s.name, nav}, remap, keep);
    if (o == nullptr) { api->log(g_host.mod, "editor: %s: none of its page's fields are its own", s.name); return false; }
    o->detail = std::string("Added by ") + s.mod;
    SetPageIcon(o, s);
    api->log(g_host.mod, "editor: %s gets a page (%s's, %d field(s) of its own)", name.c_str(), like->name.c_str(), (int)o->rows.size());
    return true;
}

void AddModContent()
{
    static bool done;
    if (done || !g_catalogOk) return;
    done = true;
    const HfApi* api = g_host.api;
    if (api->size < offsetof(HfApi, content) + sizeof(void*) || api->content == nullptr) return;
    std::vector<HfContent> all = AllContent(api);
    int pages = 0;
    for (const HfContent& s : all)
        if (s.kind == HF_KIND_STRATAGEM && AddStratagemPage(s, all)) pages++;
    /* their saved values, to the game: the stats pass does not know them (joined to a friend, Tick swaps in the host's) */
    int applied = ApplyModValues(localstats::OwnEntries(), "this machine's", "HellForge refused its saved value");
    g_modRole = HF_SESSION_SOLO;
    if (pages || applied) api->log(g_host.mod, "editor: %d mod stratagem page(s), %d saved value(s) of them applied", pages, applied);
}

void Tick()
{
    ReadSync();
    static ULONGLONG next, nextNotice;
    ULONGLONG now = GetTickCount64();
    if (now >= nextNotice) { nextNotice = now + 250; ReadNotice(); }
    if (!g_catalogOk || g_modRole == -1) return;
    if (now >= next) { next = now + 250; ReadSession(); }
    SyncModValues();
    HolderTick(500); /* the marks are ready when the editor opens */
}

float PixelScale(float width, float height)
{
    float ox, oy, s;
    Layout(width, height, &ox, &oy, &s);
    return s;
}

/* A frame of the editor's longer than 100 ms, in its log (at most every 5 s): how long, and its live reads. */
void NoteSlowFrame(LONGLONG start)
{
    static ULONGLONG noted;
    double ms = (double)(QpcNow() - start) * 1000.0 / (double)QpcFrequency();
    if (ms < 100 || GetTickCount64() - noted < 5000) return;
    noted = GetTickCount64();
    auto& frame = LiveFrame();
    g_host.api->log(g_host.mod, "editor: a slow frame, %.0f ms: %d live read(s) in %.1f ms, %d left for later frames", ms, frame.reads,
        (double)frame.ticks * 1000.0 / (double)QpcFrequency(), frame.held);
}

void Frame(ImDrawList* list, float width, float height, float dt)
{
    LONGLONG started = QpcNow();
    g_time += dt;
    g_frame++;
    ReadSession();
    float ox, oy, s;
    Layout(width, height, &ox, &oy, &s);
    g_cv.Begin(list, ox, oy, s, width, height);
    ImGuiIO& io = ImGui::GetIO();
    g_cv.Input(io.MousePos.x, io.MousePos.y, ImGui::IsMouseClicked(0), ImGui::IsMouseClicked(1), io.MouseWheel);
    if (ImGui::IsMouseClicked(0) && g_edit.on) CommitEdit();
    if (ImGui::IsMouseClicked(0) && g_search.active) g_search.active = false;
    if (g_catalogOk || g_sync.on) HandleKeys();
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_host.close();
    Draw();
    DrawNotice();
    if (g_cv.Clicked()) Sound(sounds::CLICK);
    g_cv.End();
    NoteSlowFrame(started);
}

bool NoticeShown() { return g_notice.version != 0 && GetTickCount64() - g_notice.at < NoticeMs(); }

void Overlay(ImDrawList* list, float width, float height)
{
    float ox, oy, s;
    Layout(width, height, &ox, &oy, &s);
    g_cv.Begin(list, ox, oy, s, width, height);
    DrawNotice();
    g_cv.End();
}

void Modal(ImDrawList* list, float width, float height, float dt)
{
    g_time += dt;
    g_frame++;
    float ox, oy, s;
    Layout(width, height, &ox, &oy, &s);
    g_cv.Begin(list, ox, oy, s, width, height);
    ImGuiIO& io = ImGui::GetIO();
    g_cv.Input(io.MousePos.x, io.MousePos.y, ImGui::IsMouseClicked(0), ImGui::IsMouseClicked(1), io.MouseWheel);
    if (g_sync.on) SyncKeys();
    DrawSync();
    DrawNotice();
    if (g_cv.Clicked()) Sound(sounds::CLICK);
    g_cv.End();
}

void Opened()
{
    g_windowShown = true;
    ShowSyncAlone(false); /* F8 over the prompt: the window opens on the dialog */
    ForgetKeys();
    sounds::Report("the editor opens");
    sounds::EditorOpen(true);
    Sound(sounds::SHOW);
    g_live.clear(); /* read the game's values again */
    std::string error;
    if (!g_host.modsFolder.empty() && !localstats::Pending()) localstats::Load(g_host.modsFolder, &error);
}

void Closed()
{
    g_windowShown = false;
    /* the window closed on an offer: no, unless it is downloading (the dialog is back over the game when it is ready) */
    if (g_sync.on && !g_sync.sent && g_sync.state.state != HF_SYNC_DOWNLOADING) AnswerSync(HF_SYNC_DECLINE);
    ForgetKeys();
    g_picker.on = false;
    sounds::Report("the editor closes");
    sounds::EditorOpen(false);
    Sound(sounds::HIDE);
    g_edit.on = false;
    g_search.active = false;
}

void SaveStatus(bool ok, const std::string& text)
{
    if (!ok) Toast(text, ERROR_);
}
} // namespace editorapp
