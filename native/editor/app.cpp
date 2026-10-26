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
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>
#include "app.h"
#include "canvas.h"
#include "catalog.h"
#include "icons.h"
#include "json.h"
#include "local_stats.h"
#include "sounds.h"
#include "ui_marks.h"

using catalog::Object;
using catalog::Row;
using ui::Canvas;
using ui::TextOpts;

namespace editorapp
{
namespace
{
/* ---- theme (ui/theme.lua) ---- */
#define C(r, g, b, a) IM_COL32(r, g, b, a)
/* The HD2R Editor's yellow highlights on lighter panels; HellForge's red only in the title (its icon's red) */
const ImU32 PANEL = C(24, 27, 32, 242), PANEL_ALT = C(29, 32, 38, 244), HEADER = C(17, 19, 23, 250), FOOTER = C(17, 19, 23, 250),
    LINE = C(255, 255, 255, 18), LINE_STRONG = C(255, 255, 255, 38), TEXT = C(224, 224, 218, 255), DIM = C(172, 176, 182, 255),
    FAINT = C(126, 132, 140, 255), INVERSE = C(14, 16, 20, 255), GOLD = C(255, 199, 44, 255), GOLD_DIM = C(150, 113, 10, 255),
    GOLD_WASH = C(255, 199, 44, 26), GOLD_SOFT = C(255, 199, 44, 48), SELECT = C(255, 199, 44, 34), HOVER = C(255, 255, 255, 14),
    SELECT_HEADER = C(255, 199, 44, 16), WASH_HEADER = C(255, 199, 44, 11), HOVER_HEADER = C(255, 199, 44, 7),
    MOD = C(110, 190, 245, 255), MOD_SOFT = C(110, 190, 245, 44), LUA = C(186, 140, 255, 255), LUA_SOFT = C(186, 140, 255, 44), PENDING = C(246, 160, 72, 255), PENDING_SOFT = C(246, 160, 72, 48),
    ERROR_ = C(255, 96, 84, 255), ERROR_SOFT = C(255, 96, 84, 48), OK = C(128, 210, 148, 255), BOX = C(14, 16, 19, 255),
    BOX_EDGE = C(255, 255, 255, 54), BOX_FOCUS = C(255, 199, 44, 255), SHADOW = C(0, 0, 0, 80),
    ICON = C(226, 226, 214, 255), PERK = C(255, 221, 31, 255), PRIMARY_HOVER = C(255, 214, 92, 255), BRAND = C(229, 57, 43, 255);
/* The stratagem categories' colours (the game's: offensive red, defensive green, support blue), and their washes. */
struct Tone { ImU32 colour, soft; };
Tone ToneOf(const std::string& tone)
{
    if (tone == "offensive") return {C(226, 94, 74, 255), C(226, 94, 74, 40)};
    if (tone == "defensive") return {C(122, 188, 94, 255), C(122, 188, 94, 40)};
    if (tone == "support") return {C(74, 172, 232, 255), C(74, 172, 232, 40)};
    return {0, 0};
}
/* A faction's colour, the HD2R Editor's (its emblem, the game's HUD icon, is drawn in it); 0 for anything else. */
ImU32 FactionColour(const std::string& faction)
{
    if (faction == "terminids") return C(255, 201, 45, 255);
    if (faction == "automatons") return C(255, 80, 80, 255);
    if (faction == "illuminate") return C(183, 116, 255, 255);
    if (faction == "super_earth") return C(95, 195, 245, 255);
    return 0;
}
/* The faction whose emblem an object shows: an entity's own (structures and other entities too), else its page's. */
const std::string& FactionOf(const Object* object) { return object->faction.empty() ? object->category : object->faction; }
/* An object on a page where most rows have an icon (Structures, Other Entities, the factions', the rosters', the mission
 * difficulties'): a row without one keeps its room, so the names line up. */
bool EntityPage(const Object* object)
{
    return object->category == "structures" || object->category == "entities" || object->category == "rosters" ||
        object->category == "difficulty" || FactionColour(object->category) != 0;
}
const float SZ_TITLE = 22, SZ_TAB = 15, SZ_HEADING = 13, SZ_LABEL = 15, SZ_SMALL = 13, SZ_TINY = 12;
const float PANEL_W = 1260, PANEL_H = 930, PANEL_MIN_H = 660;
const float UI_SCALES[] = {0.9f, 1.0f, 1.1f, 1.25f, 1.4f, 1.5f};
const int UI_SCALE_COUNT = sizeof UI_SCALES / sizeof UI_SCALES[0];
const float HEADER_H = 58, FOOTER_H = 56, STATUS_H = 40, ROW_H = 30, LIST_ROW_H = 34;
const float CAT_W = 224, OBJ_W = 334;
/* Column positions of the field rows (relative to the fields pane). */
const float FX_LABEL = 18, FX_DEFAULT_RIGHT = 404, FX_BOX = 416, FX_BOX_W = 120, FX_UNIT = 544, FX_BADGE_RIGHT = -14, INDENT = 20;
const char* const SEP = " \xC2\xB7 ";

TextOpts T(float size, ImU32 colour, ui::Face face = ui::BODY, ui::Align align = ui::LEFT, float max = 0, int z = 5)
{
    TextOpts o;
    o.size = size; o.colour = colour; o.face = face; o.align = align; o.max = max; o.z = z;
    return o;
}

/* ---- state ---- */
Host g_host;
catalog::Catalog g_catalog;
bool g_catalogOk;
std::string g_catalogError;
uint32_t g_layout;
Canvas g_cv;
float g_time, g_panelH = PANEL_H, g_scaleNow = 1;

enum View { BROWSE, SEARCH, CHANGES, MODS, PRESETS, LOGS, SETTINGS, VIEW_COUNT };
const char* const VIEW_LABELS[] = {"BROWSE", "SEARCH", "CHANGES", "MODS", "PRESETS", "LOGS", "SETTINGS"};
View g_view = BROWSE;
enum Focus { CATEGORIES, OBJECTS, FIELDS };
Focus g_focus = CATEGORIES;

struct CatItem { bool group; std::string label; int nav; };
std::vector<CatItem> g_cats;
int g_cat = 1, g_obj = 0, g_field = 0;
std::map<std::string, int> g_scroll, g_seen;
float g_catScroll = 0; /* the category list's scroll, in units (its rows differ in height, so not a List) */
struct { bool active = false; std::string text; } g_search;
std::map<std::string, std::string> g_group;
struct Pend { double value; bool reset; }; /* reset: back to the default when applied */
/* The armor perks (choice rows): what the editor keeps (its settings, "passives"), -1 for the kits' own; set_passives
 * from Init on, so they hold from every boot. */
double g_choice[2] = {-1, -1};
/* The picker (a choice row's options, or a reference row's: own, none, then the catalogue's references in groups), with
 * a filter typed while it is open. */
struct PickEntry { bool header = false; double value = 0; std::string label, note, icon; std::vector<std::string> warnings; };
struct { bool on = false; const Row* row = nullptr; int index = 0; std::string filter; std::vector<PickEntry> entries; } g_picker;
int ChoiceIndex(const Row* row) { return row->choice == "second" ? 1 : 0; }
/* The arm swaps (a vehicle's Mounted Weapons) and the hellpod payloads (a call-in's pod: its slots and how many spawn):
 * the option chosen per row ("<vehicle>|mount.slot_N", "<rack>|pod.slot_N", "<rack>|pod.count"), kept in the editor's
 * settings ("mounts") and applied live (set_live: a slot's u64 entity, the count's u32) from Init on. */
std::map<std::string, double> g_mounts;
/* Joined to a friend's lobby: the host's arm swaps (host-mounts.json, which the session's backend writes), applied and
 * shown in place of this machine's; the armor perks are solo only (with other players the loader gives everyone the
 * armor's own), so a joiner shows none. */
std::map<std::string, double> g_hostMounts;
bool g_hostMountsShown;
bool Locked();
const std::map<std::string, double>& Mounts() { return g_hostMountsShown ? g_hostMounts : g_mounts; }
std::string MountKey(const Row* row) { return row->target + "|" + row->key; }
bool IsMount(const Row* row) { return row->choice == "mount" || row->choice == "pod" || row->choice == "podcount" || row->choice == "rowcopy"; }
bool IsPod(const Row* row) { return row->choice == "pod" || row->choice == "podcount"; }
/* The armory's displayed armor penetration (HD2R Editor's row): which armor penetration trait the weapon's five trait
 * slots hold. Not kept anywhere itself: shown from the slots, and a pick is staged on the slot that holds one, else the
 * first empty one (ApSlot), so the trait slots are what Local Stats saves. */
bool IsApLabel(const Row* row) { return row->choice == "aplabel"; }
std::vector<const Row*> TraitRows(const Row* row)
{
    std::vector<const Row*> out;
    for (const Row* r : row->object->rows)
        if (r->target == row->target && r->key.rfind("presentation.traits.", 0) == 0) out.push_back(r);
    std::sort(out.begin(), out.end(), [](const Row* a, const Row* b) { return a->key < b->key; });
    return out;
}
double ChoiceGet(const Row* row)
{
    if (!IsMount(row)) return Locked() ? -1 : g_choice[ChoiceIndex(row)];
    auto found = Mounts().find(MountKey(row));
    return found != Mounts().end() ? found->second : row->options.empty() ? -1 : row->options[0].value;
}
bool ChoiceSaved(const Row* row) { return IsMount(row) ? Mounts().count(MountKey(row)) > 0 : !Locked() && g_choice[ChoiceIndex(row)] >= 0; }
std::map<const Row*, std::string> g_errors;
/* Caches (thousands of changes made every frame slow): the frame, and versions of what views depend on. */
unsigned g_frame = 1, g_errorsVersion = 1, g_layoutVersion = 1;
void SetError(const Row* row, const std::string& why) { g_errors[row] = why; g_errorsVersion++; }
void ClearError(const Row* row) { if (g_errors.erase(row)) g_errorsVersion++; }
/* HellForge's reason for the call that was just refused (its last log line), when this HellForge says it. */
std::string Refusal(const char* fallback);
std::map<const Row*, Pend> g_pending;
bool g_soundsOn = true;
int g_logSource; /* the LOGS tab's source from the settings (0 HellForge's log, 1 the editor's) */
struct { bool on = false; const Row* row = nullptr; std::string buffer; bool fresh = false; } g_edit;
std::set<std::string> g_open; /* the headers open (every one starts closed), per item: kept in editor-settings.json */
float g_uiScale = 1;
struct { bool on = false; std::string text; ImU32 colour = TEXT; float time = 0; } g_toast;
struct { std::string text; int index = 0; } g_gsearch;
int g_change;
struct LiveState { bool read = false; double now = 0, base = 0; std::string mod; ULONGLONG at = 0; };
std::map<const Row*, LiveState> g_live;
/* A live read is a lookup in the mod loader under its lock, a long one for some fields: an enemy page reads 300 at
 * once, and they all go stale together. The reads drawing asks for share LIVE_BUDGET_US a frame (HolderTick gets what
 * they leave of it, after the page); the rest wait for a later frame, which also spreads their next refresh. A fresh read (after an edit) is never
 * held back. Frame() logs a frame that was slow anyway. */
const LONGLONG LIVE_BUDGET_US = 1500;
struct { unsigned frame = 0; LONGLONG ticks = 0; int reads = 0, held = 0; } g_liveFrame;
LONGLONG QpcFrequency() { static LARGE_INTEGER f; if (f.QuadPart == 0) QueryPerformanceFrequency(&f); return f.QuadPart; }
LONGLONG QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
/* This frame's live reads so far (reset when the frame changes). */
decltype(g_liveFrame)& LiveFrame()
{
    if (g_liveFrame.frame != g_frame) g_liveFrame = {g_frame, 0, 0, 0};
    return g_liveFrame;
}
struct { std::string key; float trackY = 0, trackH = 0, thumb = 0; int count = 0, rows = 0; } g_drag;
unsigned g_markVersion = 1;

void Sound(sounds::Kind kind) { sounds::Request(kind); }
void Toast(const std::string& text, ImU32 colour)
{
    g_toast.on = true; g_toast.text = text; g_toast.colour = colour; g_toast.time = g_time;
    if (colour == ERROR_) Sound(sounds::ERROR_);
}

/* ---- settings (the editor's folder, editor-settings.json) ---- */
std::wstring SettingsPath() { return hfjson::Wide(g_host.modFolder) + L"\\editor-settings.json"; }
/* Sandboxed, the game cannot write into its mods folder: settings and exported presets go to the session's sandbox
 * folder, and HellForge puts them in place once the game is gone (SandboxStats.cs). */
std::wstring SettingsWritePath()
{
    std::wstring sandbox = localstats::SandboxFolder();
    return sandbox.empty() ? SettingsPath() : sandbox + L"\\editor-settings.json";
}

void LoadSettings()
{
    std::string text;
    hfjson::Value root;
    if (!hfjson::ReadFile(SettingsPath(), &text) || !hfjson::Parse(text, &root, nullptr)) return;
    g_soundsOn = root.GetBool("ui_sounds", true);
    g_logSource = (int)root.GetNumber("log_source", 0);
    float scale = (float)root.GetNumber("ui_scale", 1);
    for (float s : UI_SCALES) if (fabsf(s - scale) < 0.001f) g_uiScale = s;
    if (const hfjson::Value* open = root.Find("open"))
        for (const hfjson::Value& v : open->items) if (v.kind == hfjson::Value::String) g_open.insert(v.text);
    if (const hfjson::Value* mounts = root.Find("mounts"); mounts != nullptr && mounts->kind == hfjson::Value::Object)
        for (auto& m : mounts->members) g_mounts[m.first] = m.second.AsNumber();
    if (const hfjson::Value* passives = root.Find("passives"))
    {
        g_choice[0] = passives->GetNumber("armor", -1);
        g_choice[1] = passives->GetNumber("second", -1);
    }
}

void SaveSettings()
{
    hfjson::Value root;
    root.kind = hfjson::Value::Object;
    root.members.emplace_back("ui_scale", hfjson::Value::MakeNumber(g_uiScale, false));
    root.members.emplace_back("ui_sounds", hfjson::Value::MakeBool(g_soundsOn));
    root.members.emplace_back("log_source", hfjson::Value::MakeNumber(g_logSource, true));
    hfjson::Value open;
    open.kind = hfjson::Value::Array;
    for (auto& key : g_open) open.items.push_back(hfjson::Value::MakeString(key));
    root.members.emplace_back("open", open);
    hfjson::Value passives;
    passives.kind = hfjson::Value::Object;
    passives.members.emplace_back("armor", hfjson::Value::MakeNumber(g_choice[0], true));
    passives.members.emplace_back("second", hfjson::Value::MakeNumber(g_choice[1], true));
    root.members.emplace_back("passives", passives);
    hfjson::Value mounts;
    mounts.kind = hfjson::Value::Object;
    for (auto& m : g_mounts) mounts.members.emplace_back(m.first, hfjson::Value::MakeNumber(m.second, true));
    root.members.emplace_back("mounts", mounts);
    hfjson::WriteAtomically(SettingsWritePath(), hfjson::Write(root));
}

/* ---- values ---- */
std::string Refusal(const char* fallback)
{
    if (g_host.api->size >= offsetof(HfApi, last_log) + sizeof(void*) && g_host.api->last_log != nullptr)
        if (const char* why = g_host.api->last_log(g_host.mod); why != nullptr && why[0]) return why;
    return std::string(fallback) + " (the editor's log, mod-hellforge.editor.log, says why)";
}

bool HasGetLive() { return g_host.api->size >= offsetof(HfApi, get_live) + sizeof(void*) && g_host.api->get_live != nullptr; }
bool HasGetStat() { return g_host.api->size >= offsetof(HfApi, get_stat) + sizeof(void*) && g_host.api->get_stat != nullptr; }
bool HasSetStat() { return g_host.api->size >= offsetof(HfApi, set_stat) + sizeof(void*) && g_host.api->set_stat != nullptr; }
/* An attachment's entity delta or a status multiplier: saved like the rest, and (SDK 9, set_stat) changed live too, where
 * the stats pass writes it at boot; a weapon takes it when it is next built (from the next respawn, expected). */
bool LiveStat(const Row* row)
{
    return !row->Live() && row->Stat() && (row->statKind == "EntityDelta" || row->statKind == "StatusMultiplier") && HasSetStat();
}
HfStat StatOf(const Row* row) { return HfStat{row->statKind.c_str(), row->statComponent.c_str(), row->statResource, row->statRecord, row->offset, row->type}; }

/* Co-op (docs/multiplayer.md "Stats in co-op"): every player keeps their own stats, and HellForge loads the packages
 * friends' changes need. A host may share its stats instead (the Mods page): then a joiner plays with the host's, which
 * the session's backend applies, and the editor shows them read-only. A loader from before SDK 15 (no stats_source)
 * always shared the host's. */
HfSession g_session;
bool g_lobbyStats; /* the lobby plays with the host's stats (hosting: this machine's; joined: the host's) */
bool g_hostStats;  /* joined, playing with the host's stats */
void ReadSession()
{
    HfSession now{};
    if (g_host.api->size >= offsetof(HfApi, session) + sizeof(void*) && g_host.api->session != nullptr) g_host.api->session(g_host.mod, &now);
    if (now.version != g_session.version || now.role != g_session.role)
    {
        bool was = g_hostStats, joined = g_session.role == HF_SESSION_JOINER;
        bool sourced = g_host.api->size >= offsetof(HfApi, stats_source) + sizeof(void*) && g_host.api->stats_source != nullptr;
        g_session = now;
        g_lobbyStats = now.role != HF_SESSION_SOLO && (!sourced || g_host.api->stats_source(g_host.mod) == HF_STATS_HOST);
        g_hostStats = now.role == HF_SESSION_JOINER && g_lobbyStats;
        g_markVersion++;
        g_live.clear(); /* the host's values: read again */
        if (!was && g_hostStats) Toast(std::string("Joined ") + now.host + ": you play with the host's stats; the editor is read-only", PENDING);
        else if (was && !g_hostStats) Toast("Back to your own stats: the editor can change them again", OK);
        else if (!joined && now.role == HF_SESSION_JOINER) Toast(std::string("Joined ") + now.host + ": everyone keeps their own changes", OK);
    }
}
bool Locked() { return g_hostStats; }

/* HellForge's messages for the player (SDK 12, HfApi notice; docs/mods.md "Playing with friends"): why a friend's lobby
 * refused this game's join, or why a friend could not join this one. Each new one shows over the game for a while, the
 * editor open or not (Overlay draws it while the window is closed). */
struct { uint32_t version = 0; bool error = false, brief = false; std::string title; std::vector<std::string> lines; ULONGLONG at = 0; } g_notice;
const ULONGLONG NOTICE_ERROR_MS = 20000, NOTICE_INFO_MS = 12000, NOTICE_BRIEF_MS = 6000, NOTICE_FADE_MS = 800;
bool HasNotice() { return g_host.api->size >= offsetof(HfApi, notice) + sizeof(void*) && g_host.api->notice != nullptr; }
void ReadNotice()
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
    g_notice.lines.clear();
    for (const char *p = now.text, *end; *p; p = *end ? end + 1 : end)
    {
        end = strchr(p, '\n');
        if (end == nullptr) end = p + strlen(p);
        if (end > p) g_notice.lines.emplace_back(p, end);
    }
    g_notice.at = GetTickCount64();
    if (g_notice.error) Sound(sounds::ERROR_);
}
ULONGLONG NoticeMs() { return g_notice.error ? NOTICE_ERROR_MS : g_notice.brief ? NOTICE_BRIEF_MS : NOTICE_INFO_MS; }
ImU32 Faded(ImU32 colour, float alpha) { return (colour & 0x00FFFFFF) | ((ImU32)((float)(colour >> 24) * alpha + 0.5f) << 24); }
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
bool SyncShown(); /* mod sync's dialog is up */
/* The message at the top of the screen, over the game and the editor alike. A refused join's notice gives way to mod
 * sync's dialog, which lists the same mods and what happens to each: once that is up, the notice is done. */
void DrawNotice()
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
    std::vector<std::string> lines;
    size_t lead = 0; /* the first line's rows, brighter: what happened; the rest say why */
    for (const std::string& line : g_notice.lines)
    {
        for (std::string& part : Wrapped(line, w - 56, SZ_LABEL)) lines.push_back(std::move(part));
        if (lead == 0) lead = lines.size();
    }
    if (lines.size() > 14) { lines.resize(14); lines.back() = "..."; }
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
/* A change asked for while joined: refused, and said why. */
bool RefuseLocked()
{
    if (!Locked()) return false;
    Toast(std::string("Read-only: you play with ") + (g_session.host[0] ? g_session.host : "the host") + "'s stats while in their lobby", PENDING);
    Sound(sounds::ERROR_);
    return true;
}

HfField FieldOf(const Row* row) { return HfField{row->component.c_str(), g_layout, (uint16_t)row->offset, (uint16_t)(row->type == HF_TYPE_U8 ? 1 : 4), row->type, 0}; }

bool HasPassives() { return g_host.api->size >= offsetof(HfApi, get_passives) + sizeof(void*) && g_host.api->set_passives != nullptr; }

/* The armor perks as the editor keeps them, to the loader. */
void SendPassives()
{
    if (!HasPassives()) return;
    g_host.api->set_passives(g_host.mod, g_choice[0] < 0 ? HF_PASSIVE_KIT : (uint32_t)g_choice[0], g_choice[1] < 0 ? HF_PASSIVE_KIT : (uint32_t)g_choice[1]);
}

/* budgeted: the read shares the frame's LIVE_BUDGET_US (drawing); HolderTick reads within its own budget */
LiveState& Live(const Row* row, bool fresh = false, bool budgeted = true)
{
    LiveState& s = g_live[row];
    if (IsApLabel(row))
    {
        s.read = true; s.now = s.base = 0; s.mod.clear();
        for (const Row* t : TraitRows(row))
        {
            const LiveState& ts = Live(t, fresh, budgeted);
            if (ts.read && ts.now != 0 && row->OptionOf(ts.now)) s.now = ts.now;
            if (ts.read && ts.base != 0 && row->OptionOf(ts.base)) s.base = ts.base;
            if (ts.read && !ts.mod.empty()) s.mod = ts.mod;
        }
        return s;
    }
    if (!row->choice.empty())
    {
        s.read = true; s.now = ChoiceGet(row); s.mod.clear();
        s.base = IsMount(row) && !row->options.empty() ? row->options[0].value : -1;
        return s;
    }
    ULONGLONG now = GetTickCount64();
    bool live = row->Live() && HasGetLive(), stat = !row->Live() && row->Stat() && HasGetStat();
    if ((!live && !stat) || !(fresh || s.at == 0 || now - s.at > 400)) return s;
    auto& frame = LiveFrame();
    if (budgeted && !fresh && frame.ticks >= QpcFrequency() * LIVE_BUDGET_US / 1000000) { frame.held++; return s; }
    LONGLONG start = QpcNow();
    HfLiveValue value;
    if (live)
    {
        HfField field = FieldOf(row);
        s.read = g_host.api->get_live(g_host.mod, row->ref, &field, &value) != 0;
    }
    else
    {
        /* applied at boot: the value the game holds now, and its base (the Local Stats mod's edit undone) */
        HfStat stat{row->statKind.c_str(), row->statComponent.c_str(), row->statResource, row->statRecord, row->offset, row->type};
        s.read = g_host.api->get_stat(g_host.mod, &stat, &value) != 0;
    }
    if (s.read) { s.now = value.now.number; s.base = value.base.number; s.mod = value.mod ? value.mod : ""; }
    s.at = now;
    frame.ticks += QpcNow() - start;
    frame.reads++;
    return s;
}

bool Same(const Row* row, double a, double b) { return row->Whole() ? llround(a) == llround(b) : (float)a == (float)b; }

/* A reference value's label: the catalogue's name for it, None / Empty for 0, else its number. */
std::string RefLabel(const Row* row, double v)
{
    if (v == 0 && row->pick != "projectile") return row->pick == "status" ? "Empty" : "None";
    if (const Row::Option* o = g_catalog.Reference(row->pick, v)) return o->label;
    char text[48];
    snprintf(text, sizeof text, "%s %.0f", row->pick.c_str(), v);
    return text;
}

std::string Format(const Row* row, double v)
{
    if (!row->choice.empty()) { const Row::Option* o = row->OptionOf(v); return o ? o->label : "?"; }
    if (row->Valued()) if (const Row::Option* o = row->OptionOf(v)) return o->label; /* else its number */
    if (!row->pick.empty()) return RefLabel(row, v);
    if (row->unlimited && v == 4294967295.0) return "Unlimited";
    char text[64];
    if (row->Whole() || (fabs(v) < 1e15 && v == floor(v))) snprintf(text, sizeof text, "%.0f", v);
    else if (fabs(v) < 0.001) snprintf(text, sizeof text, "%.6g", v);
    else
    {
        snprintf(text, sizeof text, "%.4f", v);
        char* end = text + strlen(text) - 1;
        while (end > text && *end == '0') *end-- = 0;
        if (*end == '.') *end = 0;
    }
    if (!strcmp(text, "-0")) strcpy(text, "0");
    return text;
}

/* A saved value of the row's bytes (its own entry, else a sibling's on the same bytes). */
bool Saved(const Row* row, double* value, const Row** holder = nullptr)
{
    if (IsApLabel(row)) return false;
    if (!row->choice.empty()) { *value = ChoiceGet(row); if (holder) *holder = row; return ChoiceSaved(row); }
    auto get = [&](const Row* r) {
        if (!localstats::Get(r->category, r->target, r->key, value)) return false;
        if (r->unlimited && *value == -1) *value = 4294967295.0;
        return true;
    };
    if (get(row)) { if (holder) *holder = row; return true; }
    for (const Row* s : g_catalog.Siblings(row))
        if (s != row && get(s)) { if (holder) *holder = s; return true; }
    return false;
}

const Pend* PendingOf(const Row* row)
{
    auto found = g_pending.find(row);
    if (found != g_pending.end()) return &found->second;
    for (const Row* s : g_catalog.Siblings(row))
    {
        auto other = g_pending.find(s);
        if (other != g_pending.end()) return &other->second;
    }
    return nullptr;
}

enum Source { GAME, MODDED, EDITED, STAGED };
struct RowView { bool has = false; double value = 0; Source source = GAME; bool hasBase = false; double base = 0; std::string holder; bool reset = false; };

RowView ViewOfNow(const Row* row, bool withPending);
/* A row's view, once a frame (and again after a change): pages, marks and lists ask for it many times. */
RowView ViewOf(const Row* row, bool withPending = true)
{
    if (!withPending) return ViewOfNow(row, false);
    struct Memo { unsigned frame = 0, version = 0, stats = 0; RowView view; };
    static std::map<const Row*, Memo> memo;
    Memo& m = memo[row];
    unsigned stats = localstats::Version();
    if (m.frame == g_frame && m.version == g_markVersion && m.stats == stats) return m.view;
    m.view = ViewOfNow(row, true);
    m.frame = g_frame; m.version = g_markVersion; m.stats = stats;
    return m.view;
}

RowView ViewOfNow(const Row* row, bool withPending)
{
    RowView v;
    if (IsApLabel(row))
    {
        bool staged = false;
        v.has = v.hasBase = true;
        for (const Row* t : TraitRows(row))
        {
            RowView tv = ViewOf(t, withPending);
            if (tv.value != 0 && row->OptionOf(tv.value)) v.value = tv.value;
            if (tv.hasBase && tv.base != 0 && row->OptionOf(tv.base)) v.base = tv.base;
            staged = staged || tv.source == STAGED;
        }
        v.source = staged ? STAGED : v.value != v.base ? EDITED : GAME;
        return v;
    }
    LiveState& live = Live(row);
    double saved;
    /* joined: Saved answers with the host's saved changes, which the game holds (attachments' from the next respawn) */
    bool isSaved = Saved(row, &saved);
    if (live.read)
    {
        v.has = true; v.hasBase = true; v.base = live.base; v.holder = live.mod;
        v.value = !row->Live() && isSaved ? saved : live.now; /* applied at boot: the saved value is the one it will have */
    }
    else if (isSaved) { v.has = true; v.value = saved; }
    if (!live.read && row->hasBase) { v.hasBase = true; v.base = row->base; if (!isSaved) { v.has = true; v.value = row->base; } }
    v.source = isSaved ? EDITED : !v.holder.empty() ? MODDED : GAME;
    if (withPending && !Locked())
        if (const Pend* p = PendingOf(row))
        {
            v.source = STAGED;
            v.reset = p->reset || (v.hasBase && Same(row, p->value, v.base));
            if (p->reset) { v.has = v.hasBase; v.value = v.base; }
            else { v.has = true; v.value = p->value; }
        }
    return v;
}

/* The damage type an explosion type deals (ExplosionSettings +4), read through get_live and kept for a while; 0 unknown. */
std::map<uint32_t, std::pair<uint32_t, ULONGLONG>> g_explosionDamage;
uint32_t ExplosionDamage(uint32_t type)
{
    if (type == 0 || !HasGetLive()) return 0;
    auto& known = g_explosionDamage[type];
    ULONGLONG now = GetTickCount64();
    if (known.second == 0 || now - known.second > 2000)
    {
        HfField field{"ExplosionSettings", g_layout, 4, 4, HF_TYPE_U32, 0};
        HfLiveValue value;
        known.first = g_host.api->get_live(g_host.mod, type, &field, &value) ? (uint32_t)value.now.number : 0;
        known.second = now;
    }
    return known.first;
}

/* One u32 or f32 of a settings row, read through get_live; false when it cannot be read. */
bool ReadSetting(const char* component, uint64_t ref, uint16_t offset, uint32_t type, double* out)
{
    if (!HasGetLive()) return false;
    HfField field{component, g_layout, offset, 4, type, 0};
    HfLiveValue value;
    if (!g_host.api->get_live(g_host.mod, ref, &field, &value)) return false;
    *out = value.now.number;
    return true;
}

/* An explosion as the HD2R Editor's picker sums it up ("225 damage · 4 to 7 m · 1 status"), read from the game and
 * kept for a while; "" when it cannot be read. */
std::map<uint32_t, std::pair<std::string, ULONGLONG>> g_explosionSummary;
std::string ExplosionSummary(uint32_t type)
{
    auto& known = g_explosionSummary[type];
    ULONGLONG now = GetTickCount64();
    if (known.second != 0 && now - known.second < 5000) return known.first;
    known.second = now;
    double inner, outer, damage = 0;
    if (!ReadSetting("ExplosionSettings", type, 16, HF_TYPE_F32, &inner) || !ReadSetting("ExplosionSettings", type, 20, HF_TYPE_F32, &outer))
        return known.first = "";
    uint32_t hurt = ExplosionDamage(type);
    int statuses = 0;
    if (hurt != 0)
    {
        ReadSetting("DamageInfo", hurt, 4, HF_TYPE_I32, &damage);
        for (uint16_t k = 0; k < 4; k++) { double s; if (ReadSetting("DamageInfo", hurt, (uint16_t)(44 + 8 * k), HF_TYPE_U32, &s) && s != 0) statuses++; }
    }
    char text[96];
    snprintf(text, sizeof text, "%.0f damage \xC2\xB7 %g to %g m%s", damage, floor(inner * 10 + 0.5) / 10, floor(outer * 10 + 0.5) / 10,
        statuses ? (statuses == 1 ? " \xC2\xB7 1 status" : (" \xC2\xB7 " + std::to_string(statuses) + " statuses").c_str()) : "");
    return known.first = text;
}

/* A payload warning's chip (HD2R Editor's badges). */
std::string WarningChip(const std::string& id)
{
    if (id == "very_large") return "VERY LARGE";
    if (id == "lingering_field") return "LINGERING";
    if (id == "no_damage") return "NO DAMAGE";
    if (id == "objective_packages") return "300 MB LOAD";
    std::string upper = id;
    for (char& c : upper) c = c == '_' ? ' ' : (char)toupper((unsigned char)c);
    return upper;
}

/* The vehicle a vehicle page's row belongs to (its Mounted Weapons rows' target). */
std::string MountVehicle(const Row* row)
{
    for (const Row* r : row->object->rows) if (IsMount(r)) return r->target;
    return "";
}

/* A reference row that names something other than its default (a donor projectile, another explosion). */
bool Swapped(const Row* row, double* value)
{
    RowView v = ViewOf(row);
    *value = v.value;
    return v.has && v.hasBase && !Same(row, v.value, v.base);
}

/* Why a row is locked by a reference (HD2R Editor's): the weapon's own round while it fires a donor, the own explosion of
 * a slot (and its damage) while the slot sets off another; "" when it is not. */
std::string LockOfNow(const Row* row);
std::string LockOf(const Row* row)
{
    struct Memo { unsigned frame = 0, version = 0, stats = 0; std::string why; };
    static std::map<const Row*, Memo> memo;
    Memo& m = memo[row];
    unsigned stats = localstats::Version();
    if (m.frame == g_frame && m.version == g_markVersion && m.stats == stats) return m.why;
    m.why = LockOfNow(row);
    m.frame = g_frame; m.version = g_markVersion; m.stats = stats;
    return m.why;
}

std::string LockOfNow(const Row* row)
{
    double value;
    /* a vehicle weapon's rows while its slot holds another weapon (HD2R Editor refuses them then) */
    if (!row->mountSlot.empty())
        if (const Row* slot = g_catalog.Entry("mounts", row->object->rows.empty() ? "" : MountVehicle(row), row->mountSlot);
            slot != nullptr && ChoiceSaved(slot))
            if (const Row::Option* o = slot->OptionOf(ChoiceGet(slot)); o != nullptr && o != &slot->options[0])
                return "\"" + slot->label + "\" holds " + o->label + " now: reset it to edit this weapon";
    if (!row->fires.empty())
        if (const Row* swap = g_catalog.Entry(row->category, row->target, row->fires); swap != nullptr && Swapped(swap, &value))
            return "the weapon fires " + RefLabel(swap, value) + " now: reset \"Projectile\" to edit its own round";
    if (row->component != "ExplosionSettings" && row->component != "DamageInfo") return "";
    for (const Row* slot : row->object->rows)
    {
        if (slot->pick != "explosion" || slot->target != row->target || !Swapped(slot, &value)) continue;
        uint32_t own = (uint32_t)llround(ViewOf(slot).base);
        if (own == 0) continue;
        if ((row->component == "ExplosionSettings" && row->ref == own) || (row->component == "DamageInfo" && row->ref == ExplosionDamage(own)))
            return "\"" + slot->label + "\" sets off " + RefLabel(slot, value) + " now: reset it to edit its own explosion";
    }
    return "";
}

/* The status slots fill in order, from slot 1 (HD2R Editor's slot_block): why a slot (or its strength) waits, or "". */
std::string SlotBlock(const Row* row)
{
    if (row->slot == 0) return "";
    auto type = [&](int k) { return g_catalog.Entry(row->category, row->target, row->slots + "status_" + std::to_string(k) + "_type"); };
    if (row->pick.empty())
    {
        const Row* own = type(row->slot);
        return own != nullptr && ViewOf(own).value == 0 ? "pick \"Status " + std::to_string(row->slot) + "\" first" : "";
    }
    const Row* before = row->slot > 1 ? type(row->slot - 1) : nullptr;
    if (before != nullptr && ViewOf(before).value == 0 && ViewOf(row).value == 0)
        return "pick \"Status " + std::to_string(row->slot - 1) + "\" first: the status slots fill in order, from slot 1";
    return "";
}

/* Whether emptying a status slot would leave a gap (a later slot holds a status). */
bool LaterSlotFilled(const Row* row)
{
    for (int k = row->slot + 1; k <= 4; k++)
        if (const Row* later = g_catalog.Entry(row->category, row->target, row->slots + "status_" + std::to_string(k) + "_type"); later && ViewOf(later).value != 0)
            return true;
    return false;
}

std::string AppliesText(const Row* row)
{
    if (LiveStat(row)) return "applies on respawn (expected)";
    std::string when = row->applies == "now" ? "applies now" : row->applies == "respawn" ? "applies on respawn" :
        row->applies == "mission" ? "applies next mission" : "applies from the next boot";
    return row->Live() && !row->verified ? when + " (expected)" : when;
}

/* ---- staging and applying ---- */
/* A header's chevron: v open, > closed, in strokes heavy enough to read at a glance. */
void Arrow(float x, float cy, bool down, ImU32 colour)
{
    const float t = 2.2f;
    if (down) g_cv.Chevron(x + 1, cy - 3, x + 6, cy + 2.5f, x + 11, cy - 3, colour, 3, t);
    else g_cv.Chevron(x + 3.5f, cy - 5.5f, x + 9, cy, x + 3.5f, cy + 5.5f, colour, 3, t);
}

/* The trait slot an AP pick goes to: the one holding an armor penetration trait, else (a new one) the first empty one. */
const Row* ApSlot(const Row* row, double value)
{
    auto traits = TraitRows(row);
    for (const Row* t : traits) if (double v = ViewOf(t).value; v != 0 && row->OptionOf(v)) return t;
    if (value == 0) return nullptr;
    for (const Row* t : traits) if (ViewOf(t).value == 0) return t;
    return nullptr;
}

void Stage(const Row* row, double value)
{
    if (RefuseLocked()) return;
    if (IsApLabel(row))
    {
        if (const Row* slot = ApSlot(row, value)) Stage(slot, value);
        else if (value != 0) Toast("Every trait slot holds a trait: empty one first, then pick the armor penetration", ERROR_);
        return;
    }
    for (const Row* s : g_catalog.Siblings(row)) if (s != row) g_pending.erase(s);
    RowView applied = ViewOf(row, false);
    if (applied.has && Same(row, value, applied.value)) g_pending.erase(row);
    else g_pending[row] = {value, false};
    g_markVersion++;
    /* a donor projectile: the weapon's own round is not what it fires any more, so its pending values go */
    if (row->pick == "projectile" && applied.hasBase && !Same(row, value, applied.base))
    {
        int dropped = 0;
        for (const Row* r : row->object->rows)
            if (r->fires == row->key && r->target == row->target && g_pending.erase(r)) dropped++;
        if (dropped) Toast(std::to_string(dropped) + " pending change" + (dropped == 1 ? "" : "s") + " to its own round dropped: it fires " +
            RefLabel(row, value) + " now", PENDING);
    }
}

/* Stages a field's reset: it shows its default now, and goes back to it on APPLY (DISCARD takes it back). Only a field
 * Local Stats changed has anything to reset. */
bool StageReset(const Row* row)
{
    if (RefuseLocked()) return false;
    if (IsApLabel(row)) { const Row* slot = ApSlot(row, 0); return slot != nullptr && StageReset(slot); }
    double saved;
    for (const Row* s : g_catalog.Siblings(row)) g_pending.erase(s);
    g_markVersion++;
    if (!Saved(row, &saved)) return false;
    g_pending[row] = {0, true};
    return true;
}

/* A mount slot's or hellpod row's choice to the game (its option's entity, or a count), or back to its vanilla one. */
bool HasCopyRow() { return g_host.api->size >= offsetof(HfApi, copy_row) + sizeof(void*) && g_host.api->copy_row != nullptr; }
void SendMount(const Row* row)
{
    if (row->choice == "rowcopy") /* another explosion's settings over the row (SDK 9 copy_row), or its own back */
    {
        auto chosen = Mounts().find(MountKey(row));
        const Row::Option* o = chosen != Mounts().end() ? row->OptionOf(chosen->second) : nullptr;
        uint64_t from = o == nullptr || o == &row->options[0] ? 0 : (uint64_t)o->value;
        if (!HasCopyRow() || g_host.api->copy_row(g_host.mod, row->component.c_str(), row->ref, from, row->offset, row->size) == 0)
            SetError(row, HasCopyRow() ? Refusal("HellForge refused the copy") : "this HellForge has no copy_row");
        else ClearError(row);
        return;
    }
    bool wide = row->type == HF_TYPE_U64;
    HfField field{row->component.c_str(), g_layout, (uint16_t)row->offset, (uint16_t)(wide ? 8 : 4), row->type, 0};
    auto chosen = Mounts().find(MountKey(row));
    const Row::Option* o = chosen != Mounts().end() ? row->OptionOf(chosen->second) : nullptr;
    int applies;
    if (o == nullptr || o == &row->options[0]) applies = g_host.api->set_live(g_host.mod, row->ref, &field, nullptr, row->shared ? HF_LIVE_SHARED : 0);
    else
    {
        HfValue v = wide ? HfValue{HF_TYPE_U64, 0, o->resource} : HfValue{row->type, o->value, 0};
        applies = g_host.api->set_live(g_host.mod, row->ref, &field, &v, row->shared ? HF_LIVE_SHARED : 0);
    }
    if (applies == 0) SetError(row, Refusal("HellForge refused the swap"));
    else ClearError(row);
}

/* Back to the base: out of Local Stats (the row and every row on its bytes), and the base back in the game. */
void ResetRow(const Row* row)
{
    if (IsMount(row))
    {
        g_mounts.erase(MountKey(row));
        SendMount(row);
        SaveSettings();
        g_pending.erase(row);
        g_markVersion++;
        return;
    }
    if (!row->choice.empty())
    {
        g_choice[ChoiceIndex(row)] = -1;
        SendPassives();
        SaveSettings();
        g_pending.erase(row);
        g_markVersion++;
        return;
    }
    for (const Row* s : g_catalog.Siblings(row))
    {
        double v;
        if (localstats::Get(s->category, s->target, s->key, &v)) localstats::Remove(s->category, s->target, s->key);
        if (s->Live())
        {
            HfField field = FieldOf(s);
            g_host.api->set_live(g_host.mod, s->ref, &field, nullptr, HF_LIVE_BASE);
        }
        else if (LiveStat(s))
        {
            HfStat stat = StatOf(s);
            g_host.api->set_stat(g_host.mod, &stat, nullptr, HF_LIVE_BASE);
        }
        ClearError(s);
        g_pending.erase(s);
        g_live[s].at = 0;
    }
    g_markVersion++;
}

void ApplyRow(const Row* row, double value)
{
    if (IsMount(row))
    {
        if (!row->options.empty() && value == row->options[0].value) g_mounts.erase(MountKey(row));
        else g_mounts[MountKey(row)] = value;
        SendMount(row);
        SaveSettings();
        return;
    }
    if (!row->choice.empty())
    {
        g_choice[ChoiceIndex(row)] = value;
        SendPassives();
        SaveSettings();
        return;
    }
    LiveState& live = Live(row, true);
    if ((row->Live() || row->Stat()) && live.read && Same(row, value, live.base)) { ResetRow(row); return; }
    for (const Row* s : g_catalog.Siblings(row))
    {
        double v;
        if (s != row && localstats::Get(s->category, s->target, s->key, &v)) localstats::Remove(s->category, s->target, s->key);
    }
    localstats::Set(row->category, row->target, row->key, row->unlimited && value == 4294967295.0 ? -1 : value, row->Whole());
    if (row->Live())
    {
        HfField field = FieldOf(row);
        HfValue v{row->type, value, 0};
        int applies = g_host.api->set_live(g_host.mod, row->ref, &field, &v, row->shared ? HF_LIVE_SHARED : 0);
        if (applies == 0) SetError(row, Refusal("HellForge refused it"));
        else ClearError(row);
    }
    else if (LiveStat(row))
    {
        HfStat stat = StatOf(row);
        HfValue v{row->type, value, 0};
        if (g_host.api->set_stat(g_host.mod, &stat, &v, 0) == 0) SetError(row, Refusal("HellForge refused it (saved for the next boot)"));
        else ClearError(row);
    }
    for (const Row* s : g_catalog.Siblings(row)) g_live[s].at = 0;
}

int PendingCount() { return (int)g_pending.size(); }

/* A field's value once the changes are applied: the saved one, else what the game holds. */
double Effective(const Row* row)
{
    double v;
    if (localstats::Get(row->category, row->target, row->key, &v)) return v;
    const LiveState& s = Live(row, true);
    return s.read ? s.now : 0;
}

/* A weapon's input binding (WeaponData +184 left, +188 right; 2 rate of fire), the field get_live and set_live take. */
HfField InputField(int input) { return HfField{"WeaponDataComponentData", g_layout, (uint16_t)(input ? 188 : 184), 4, HF_TYPE_U32, 0}; }

/* An input's binding now and its base, read from the game (the catalogue has no row for it). */
bool ReadInput(const Row* rate, int input, double* now, double* base)
{
    if (!HasGetLive()) return false;
    HfField field = InputField(input);
    HfLiveValue value;
    if (!g_host.api->get_live(g_host.mod, rate->ref, &field, &value)) return false;
    *now = value.now.number;
    *base = value.base.number;
    return true;
}

/* An input to rate of fire, or back to its base (bind false), in the game and in Local Stats. */
void SetInput(const Row* rate, int input, bool bind)
{
    const char* key = input ? "weapon_function.right" : "weapon_function.left";
    HfField field = InputField(input);
    HfValue v{HF_TYPE_U32, 2, 0};
    g_host.api->set_live(g_host.mod, rate->ref, &field, bind ? &v : nullptr, bind ? 0 : HF_LIVE_BASE);
    if (bind) localstats::Set(rate->category, rate->target, key, 2, true);
    else localstats::Remove(rate->category, rate->target, key);
}

/* More than one rate of fire (rate slots X and Z, ProjectileWeapon +4 / +12, beside Y) is selectable only while an input
 * is bound to rate of fire (WeaponData +184 / +188 = 2: the press dispatcher reaches the rate cycle only for it,
 * game.dll 0x75541F; the cycle skips empty slots, 0x617A0B; HD2Mods research\coop-weapon-stats). The editor sets it
 * itself, as the HD2R Editor does: a weapon given more than one rate gets a free input bound (the left, else the right)
 * unless one is bound already (the MG-206's right input, say); a weapon back to one rate loses the binding the editor
 * gave it. Returns what it says to the player: only when it cannot, or when two rates are the same. */
std::string BindRateInput(const Row* rate)
{
    auto entry = [&](const char* key) { return g_catalog.Entry(rate->category, rate->target, key); };
    const Row *x = entry("fire_rate.modes.x"), *y = entry("weapon.fire_rate"), *z = entry("fire_rate.modes.z");
    double xv = x ? Effective(x) : 0, zv = z ? Effective(z) : 0, yv = y ? Effective(y) : 0;
    bool several = xv != 0 || zv != 0;
    double now[2], base[2];
    bool known[2] = {ReadInput(rate, 0, &now[0], &base[0]), ReadInput(rate, 1, &now[1], &base[1])};
    if (!known[0]) return ""; /* no input to bind on this weapon */
    bool bound = now[0] == 2 || (known[1] && now[1] == 2);
    if (!several)
    {
        for (int i = 0; i < 2; i++) if (known[i] && now[i] == 2 && base[i] != 2) SetInput(rate, i, false);
        return "";
    }
    if (!bound)
    {
        int input = now[0] == 0 ? 0 : known[1] && now[1] == 0 ? 1 : -1;
        if (input < 0) return "Both of the weapon's inputs are in use, so its other rates of fire can't be selected";
        SetInput(rate, input, true);
    }
    if ((xv != 0 && (xv == yv || xv == zv)) || (zv != 0 && zv == yv)) return "Two rates of fire are the same, so a press may seem to do nothing";
    return "";
}

void Apply()
{
    if (RefuseLocked()) return;
    if (g_pending.empty()) return;
    int n = (int)g_pending.size();
    std::vector<std::pair<const Row*, Pend>> pending(g_pending.begin(), g_pending.end());
    g_pending.clear();
    /* status slots are filled from slot 1 and emptied from the last, so no write leaves a gap */
    auto rank = [](const std::pair<const Row*, Pend>& p) {
        const Row* r = p.first;
        if (r->pick != "status") return 0;
        double to = p.second.reset ? ViewOf(r, false).base : p.second.value;
        return to != 0 ? 1 + r->slot : 10 - r->slot;
    };
    std::stable_sort(pending.begin(), pending.end(), [&](auto& a, auto& b) { return rank(a) < rank(b); });
    for (auto& p : pending) { if (p.second.reset) ResetRow(p.first); else ApplyRow(p.first, p.second.value); }
    std::string rates;
    std::set<std::string> weapons;
    for (auto& p : pending)
        if ((p.first->key == "fire_rate.modes.x" || p.first->key == "fire_rate.modes.z") && weapons.insert(p.first->category + "/" + p.first->target).second)
            if (std::string said = BindRateInput(p.first); !said.empty()) rates = said;
    g_markVersion++;
    g_host.saveSoon(800);
    Sound(sounds::APPLY);
    if (!rates.empty() && g_errors.empty()) Toast(rates, PENDING);
    else Toast(g_errors.empty() ? (std::to_string(n) + (n == 1 ? " change applied" : " changes applied") + " and saved to Local Stats") :
        std::to_string(g_errors.size()) + " refused: see the ERRORS badge", g_errors.empty() ? OK : ERROR_);
}

void Discard() { g_pending.clear(); g_markVersion++; }

std::vector<const Row*> g_choiceRows; /* the rows the editor keeps itself (armor perks, arm swaps): found at Init */

const std::vector<const Row*>& SavedRows()
{
    static unsigned stats, version;
    static std::vector<const Row*> out;
    unsigned now = localstats::Version();
    if (stats == now && version == g_markVersion && !out.empty()) return out;
    stats = now; version = g_markVersion;
    out.clear();
    for (auto& e : localstats::Entries()) /* joined: the host's */
        if (const Row* row = g_catalog.Entry(e.category, e.target, e.key)) out.push_back(row);
    for (const Row* row : g_choiceRows) if (ChoiceSaved(row)) out.push_back(row);
    return out;
}

/* RESET TO DEFAULTS, REVERT ALL: every field Local Stats changed staged back to its default, other pending changes
 * dropped; APPLY writes them, DISCARD takes them back. */
void StageResetAll()
{
    if (RefuseLocked()) return;
    g_pending.clear();
    int n = 0;
    for (const Row* row : SavedRows()) if (!PendingOf(row) && StageReset(row)) n++;
    g_markVersion++;
    Toast(n ? std::to_string(n) + (n == 1 ? " reset" : " resets") + " pending: APPLY writes them, DISCARD takes them back" : "Nothing to reset", TEXT);
}

/* ---- holders: the mod whose value each catalogue row holds (get_live / get_stat name it), read a few rows a frame so
 * the objects' and categories' marks and the MODS view know them without stalling the game: every frame from the
 * session's start, open or not (Tick: 0.5 ms of reads; while the editor draws, what the page's reads leave of their 1.5 ms), a pass over the catalogue
 * every 10 s. What a pass finds shows in the marks within half a second. ---- */
std::map<const Row*, std::string> g_holders;
size_t g_holderNext;
ULONGLONG g_holderPass, g_holderMarked;
bool g_holderChanged, g_holdersRead;

bool IsLuaHolder(const std::string& id) { return id.rfind("lua.", 0) == 0; }

void HolderTick(LONGLONG microseconds)
{
    if (!g_catalogOk) return;
    ULONGLONG tick = GetTickCount64();
    if (g_holderChanged && (g_holderNext >= g_catalog.rows.size() || tick - g_holderMarked >= 500))
    { g_markVersion++; g_holderChanged = false; g_holderMarked = tick; }
    if (g_holderNext >= g_catalog.rows.size())
    {
        g_holdersRead = true;
        if (g_holderPass != 0 && tick - g_holderPass < 10000) return;
        g_holderNext = 0;
        g_holderPass = tick;
    }
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const LONGLONG budget = frequency.QuadPart * microseconds / 1000000;
    while (g_holderNext < g_catalog.rows.size())
    {
        const Row* row = &g_catalog.rows[g_holderNext++];
        if (!row->choice.empty() || IsApLabel(row) || (!row->Live() && !row->Stat())) continue;
        const LiveState& live = Live(row, false, false);
        std::string holder = live.read ? live.mod : "";
        auto found = g_holders.find(row);
        if (holder.empty() && found != g_holders.end()) { g_holders.erase(found); g_holderChanged = true; }
        else if (!holder.empty() && (found == g_holders.end() || found->second != holder)) { g_holders[row] = holder; g_holderChanged = true; }
        QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart > budget) break;
    }
}

/* ---- markers (what an object, a category or a header holds) ---- */
struct Marks { bool error = false, pending = false, editor = false, mod = false, lua = false; };
struct MarkCache { unsigned version = 0, stats = 0, errors = 0; std::map<const Object*, Marks> objects; std::map<std::string, Marks> navs; } g_marks;

const MarkCache& Markers()
{
    unsigned stats = localstats::Version();
    if (g_marks.version == g_markVersion && g_marks.stats == stats && g_marks.errors == g_errorsVersion) return g_marks;
    g_marks.objects.clear();
    g_marks.navs.clear();
    auto mark = [](const Row* row, int kind) {
        for (const Row* s : g_catalog.Siblings(row))
        {
            Marks& o = g_marks.objects[s->object];
            Marks& n = g_marks.navs[s->object->category];
            if (kind == 0) o.error = n.error = true;
            if (kind == 1) o.pending = n.pending = true;
            if (kind == 2) o.editor = n.editor = true;
        }
    };
    /* content a mod added (a custom stratagem): the mod's mark on it and its category, as on its fields */
    for (auto& o : g_catalog.objects)
        if (o->id.rfind("mod/", 0) == 0) { g_marks.objects[o.get()].mod = true; g_marks.navs[o->category].mod = true; }
    /* values a mod holds: its mark (a Lua mod's purple) on every object and category showing them */
    for (auto& [row, holder] : g_holders)
        for (const Row* s : g_catalog.Siblings(row))
        {
            bool lua = IsLuaHolder(holder);
            Marks& o = g_marks.objects[s->object];
            Marks& n = g_marks.navs[s->object->category];
            (lua ? o.lua : o.mod) = true;
            (lua ? n.lua : n.mod) = true;
        }
    for (auto& e : g_errors) { g_marks.objects[e.first->object].error = true; g_marks.navs[e.first->object->category].error = true; }
    for (auto& p : g_pending) mark(p.first, 1);
    for (const Row* row : SavedRows()) mark(row, 2);
    g_marks.version = g_markVersion;
    g_marks.stats = stats;
    g_marks.errors = g_errorsVersion;
    return g_marks;
}

Marks RowsMarks(const std::vector<const Row*>& rows)
{
    Marks m;
    for (const Row* row : rows)
    {
        double v;
        if (g_errors.count(row)) m.error = true;
        if (PendingOf(row)) m.pending = true;
        if (Saved(row, &v)) m.editor = true;
        auto live = g_live.find(row);
        std::string holder = live != g_live.end() ? live->second.mod : "";
        if (holder.empty()) { auto h = g_holders.find(row); if (h != g_holders.end()) holder = h->second; }
        if (!holder.empty()) (IsLuaHolder(holder) ? m.lua : m.mod) = true;
    }
    return m;
}

/* ---- drawing helpers ---- */
float Chip(const std::string& text, float x, float cy, ImU32 fg, ImU32 bg, bool right, int z = 3)
{
    float w = g_cv.Measure(text, SZ_TINY, ui::TITLE) + 14;
    if (right) x -= w;
    g_cv.Rect(x, cy - 10, w, 20, bg, z);
    g_cv.Text(text, x + 7, cy, T(SZ_TINY, fg, ui::TITLE, ui::LEFT, 0, z + 2));
    return w;
}

enum Style { NORMAL, PRIMARY, DANGER, DISABLED };
bool Button(const std::string& label, float x, float y, float w, float h, Style style, uint64_t id, int z = 3)
{
    bool hovered = g_cv.Hovered() == id;
    ImU32 bg, fg, edge;
    if (style == PRIMARY) { bg = hovered ? PRIMARY_HOVER : GOLD; fg = INVERSE; edge = GOLD; }
    else if (style == DANGER) { bg = hovered ? ERROR_SOFT : PANEL; fg = ERROR_; edge = ERROR_; }
    else if (style == DISABLED) { bg = PANEL; fg = FAINT; edge = LINE_STRONG; }
    else { bg = hovered ? HOVER : PANEL; fg = TEXT; edge = LINE_STRONG; }
    g_cv.Rect(x, y, w, h, bg, z);
    g_cv.Frame(x, y, w, h, edge, z + 1);
    g_cv.Text(label, x + w / 2, y + h / 2, T(SZ_TAB, fg, ui::TITLE, ui::CENTER, 0, z + 3));
    return g_cv.Hit(x, y, w, h, id) && style != DISABLED;
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

bool DrawMark(int mark, float x, float y, float size, ImU32 colour)
{
    const icons::Icon* icon = icons::Mark(mark);
    if (icon == nullptr || !icons::Texture()) return false;
    g_cv.Image(icons::Texture(), x, y, size, size, icon->uv0, icon->uv1, colour, 5);
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

/* An object's icon, fitted into a square: the game's HUD silhouette of a weapon or throwable; a stratagem's icon as the
 * game draws it (its category layer in the category's colour, its white layer white); a booster as it is; an entity its
 * faction's emblem in the faction's colour. A stratagem with no icon is a plain square in its colour (never made-up art).
 * False when there is nothing to draw. */
bool DrawIcon(const Object* object, float x, float y, float size, int z = 4)
{
    if (!icons::Texture()) return false;
    Tone tone = ToneOf(object->tone);
    auto fit = [&](const icons::Icon* icon, ImU32 colour, int layer) {
        float aspect = (float)icon->width / (float)icon->height, w = size, h = size;
        if (aspect > 1) h = size / aspect; else w = size * aspect;
        g_cv.Image(icons::Texture(), x + (size - w) / 2, y + (size - h) / 2, w, h, icon->uv0, icon->uv1, colour, layer);
    };
    if (object->icon == "mod")
    {
        const icons::Icon* icon = icons::Find(icons::MOD, object->sprite);
        if (icon == nullptr) return false;
        fit(icon, IM_COL32_WHITE, z);
        return true;
    }
    if (object->icon == "stratagem")
    {
        const std::string& name = object->sprite.empty() ? object->name : object->sprite; /* a mod's copy: its page's */
        const icons::Icon* layer = icons::Find(icons::STRATAGEM, name);
        const icons::Icon* white = icons::Find(icons::STRATAGEM_WHITE, name);
        if (layer == nullptr || white == nullptr) return false;
        fit(layer, tone.colour ? tone.colour : GOLD, z);
        fit(white, C(255, 255, 238, 255), z + 1);
        return true;
    }
    if (object->icon == "difficulty")
    {
        const icons::Icon* icon = icons::Find(icons::DIFFICULTY, object->sprite);
        if (icon == nullptr) return false;
        fit(icon, IM_COL32_WHITE, z);
        return true;
    }
    if (object->icon == "booster" || object->icon == "passive")
    {
        const icons::Icon* icon = icons::Find(object->icon == "booster" ? icons::BOOSTER : icons::PASSIVE, object->sprite.empty() ? object->name : object->sprite);
        if (icon == nullptr) return false;
        fit(icon, object->icon == "booster" ? IM_COL32_WHITE : PERK, z);
        return true;
    }
    if (object->icon == "weapon" || object->icon == "throwable")
    {
        const icons::Icon* icon = icons::Find(object->icon == "throwable" ? icons::THROWABLE : icons::WEAPON, object->name);
        if (icon == nullptr) return false;
        fit(icon, ICON, z);
        return true;
    }
    if (ImU32 faction = FactionColour(FactionOf(object)))
    {
        const icons::Icon* icon = icons::Find(icons::FACTION, FactionOf(object));
        if (icon == nullptr) return false;
        fit(icon, faction, z);
        return true;
    }
    if (tone.colour) { g_cv.Rect(x + size * 0.18f, y + size * 0.18f, size * 0.64f, size * 0.64f, tone.colour, z); return true; }
    return false;
}

/* A vertical list with virtual scrolling (ui/app.lua App:list): only whole rows, the selection kept in view, the wheel
 * and a scrollbar (drawn thin, grabbed wide). */
struct ListOpts
{
    float x, y, w, h, rowH;
    int count, selected = -1;
    std::string key;
    bool focused = false;
    int layer = 3; /* the scrollbar's; a popup's list draws it above the popup's panel */
    std::function<void(int, float, float, float, float, bool hovered)> draw;
    std::function<void(int)> click, rightClick;
    std::function<void(int, float, float, float, float)> after;
};

uint64_t RowId(const std::string& key, int i) { return ui::Id(key, (uint64_t)i + 1); }

void List(const ListOpts& o)
{
    int rows = std::max(1, (int)floorf(o.h / o.rowH));
    int& scroll = g_scroll[o.key];
    std::string seenKey = o.key;
    if (o.selected >= 0)
    {
        auto seen = g_seen.find(seenKey);
        if (seen == g_seen.end() || seen->second != o.selected)
        {
            g_seen[seenKey] = o.selected;
            if (o.selected < scroll) scroll = o.selected;
            if (o.selected >= scroll + rows) scroll = o.selected - rows + 1;
        }
    }
    int maxScroll = std::max(0, o.count - rows);
    float wheel = g_cv.Scroll(o.x, o.y, o.w, o.h, ui::Id(o.key, 7777));
    if (wheel != 0) scroll -= (int)(wheel * 3);
    /* the scrollbar being dragged */
    if (g_drag.key == o.key)
    {
        if (!ImGui::IsMouseDown(0)) g_drag.key.clear();
        else
        {
            float ux, uy;
            g_cv.ToUnits(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, &ux, &uy);
            float f = (uy - g_drag.trackY - g_drag.thumb / 2) / std::max(1.0f, g_drag.trackH - g_drag.thumb);
            scroll = (int)floorf(std::clamp(f, 0.0f, 1.0f) * maxScroll + 0.5f);
        }
    }
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
    if (o.count > rows)
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
}

/* ---- the catalogue's lists ---- */
const catalog::Nav* NavOf(int cat) { return cat >= 0 && cat < (int)g_cats.size() && !g_cats[cat].group ? &g_catalog.navs[g_cats[cat].nav] : nullptr; }

std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::vector<std::string> Words(const std::string& text)
{
    std::vector<std::string> out;
    std::string word;
    for (char c : Lower(text) + " ")
        if (c == ' ') { if (!word.empty()) out.push_back(word); word.clear(); }
        else word += c;
    return out;
}

std::vector<Object*> Objects()
{
    const catalog::Nav* nav = NavOf(g_cat);
    if (nav == nullptr) return {};
    std::vector<Object*> list = g_catalog.Of(nav->id), out;
    std::string group = g_group.count(nav->id) ? g_group[nav->id] : "All";
    std::string q = Lower(g_search.text);
    for (Object* o : list)
    {
        if (group != "All" && (o->group.empty() ? o->subtitle : o->group) != group) continue;
        if (!q.empty() && (Lower(o->name) + " " + Lower(o->subtitle) + " " + o->search).find(q) == std::string::npos) continue;
        out.push_back(o);
    }
    return out;
}

Object* Current()
{
    auto list = Objects();
    return g_obj >= 0 && g_obj < (int)list.size() ? list[g_obj] : nullptr;
}

/* The page's items: categories and parts (collapsible headers) and rows, as the HD2R Editor's field_items lays them out. */
struct Item
{
    bool header = false, category = false, open = false;
    const Row* row = nullptr;   /* a row (another object's when it is a payload view), or none for a note */
    std::string title, key, prefix, note;
    bool detail = false;        /* a note that only describes (faint), not a warning */
    int count = 0, parts = 0, depth = 0;
    std::vector<const Row*> rows;
};

bool IsOpen(const std::string& key) { return g_open.count(key) != 0; }

std::vector<Item> ItemsNow(const Object* object);
const std::vector<Item>& Items(const Object* object)
{
    static const Object* cachedFor;
    static unsigned frame, version, layout, stats;
    static std::vector<Item> cached;
    unsigned now = localstats::Version();
    if (cachedFor != object || frame != g_frame || version != g_markVersion || layout != g_layoutVersion || stats != now)
    {
        cached = ItemsNow(object);
        cachedFor = object; frame = g_frame; version = g_markVersion; layout = g_layoutVersion; stats = now;
    }
    return cached;
}

/* A weapon's stats as its own page shows them (a mount's: "Weapon · Left gun" on its vehicle): each part's title and rows,
 * by the weapon's stats target. Found once. */
const std::vector<std::pair<std::string, std::vector<const Row*>>>& WeaponSections(const std::string& target)
{
    static std::map<std::string, std::vector<std::pair<std::string, std::vector<const Row*>>>> found;
    auto known = found.find(target);
    if (known != found.end()) return known->second;
    auto& sections = found[target];
    for (auto& o : g_catalog.objects)
    {
        for (auto& c : o->page)
            for (auto& p : c.parts)
            {
                std::vector<const Row*> rows;
                for (const Row* r : p.rows) if (r->category == "weapons" && r->target == target) rows.push_back(r);
                if (!rows.empty()) sections.push_back({c.flat ? c.title : p.title, rows});
            }
        if (!sections.empty()) break;
    }
    return sections;
}

std::vector<Item> ItemsNow(const Object* object)
{
    std::vector<Item> items;
    if (object == nullptr) return items;
    if (!object->detail.empty())
    {
        Item it;
        it.note = object->detail;
        it.detail = true;
        items.push_back(it);
    }
    if (!object->notOffered.empty())
    {
        Item it;
        it.note = "No projectile swap: " + object->notOffered;
        items.push_back(it);
    }
    auto note = [&](const std::string& text, int depth) { Item it; it.note = text; it.depth = depth; items.push_back(it); };
    /* a row a reference locks is hidden, unless it holds a change of its own (then it shows, LOCKED) */
    auto hidden = [&](const Row* r) { double held; return !LockOf(r).empty() && !PendingOf(r) && !Saved(r, &held); };
    auto row = [&](const Row* r, int depth, const std::string& prefix) {
        if (hidden(r)) return;
        Item it; it.row = r; it.depth = depth; it.prefix = prefix; items.push_back(it);
        double value;
        auto views = [&](const std::string& prefix, const char* component, uint64_t ref, std::initializer_list<uint16_t> offsets) {
            int shown = 0;
            for (uint16_t offset : offsets)
                if (const Row* s = g_catalog.Setting(component, ref, offset)) { Item v; v.row = s; v.depth = depth; v.prefix = prefix; items.push_back(v); shown++; }
            return shown;
        };
        if (r->pick == "status" && r->Live())
        {
            /* the status in the slot: its duration, for every use of it */
            RowView v = ViewOf(r);
            if (v.has && v.value != 0) views(RefLabel(r, v.value) + " (all uses)", "StatusEffectSettings", (uint64_t)v.value, {40});
        }
        if (r->pick == "projectile" && Swapped(r, &value))
        {
            note("Fires " + RefLabel(r, value) + " now: its own round's rows are hidden until \"" + r->label + "\" is reset", depth);
            /* the donor's round, where the editor has it (edits land on the donor's own item, for everything that fires it) */
            std::string donor = "Donor " + RefLabel(r, value);
            int shown = views(donor, "ProjectileSettings", (uint64_t)value, {32, 36, 40, 44, 28, 52, 64, 144, 156});
            double hurt;
            if (ReadSetting("ProjectileSettings", (uint64_t)value, 60, HF_TYPE_U32, &hurt) && hurt != 0)
                shown += views(donor, "DamageInfo", (uint64_t)hurt, {4, 8, 12, 16, 20, 24, 28, 32, 36, 44, 52, 60, 68});
            if (shown == 0) note(donor + ": no item of the editor shows its round", depth);
        }
        if (r->pick != "explosion" || !Swapped(r, &value) || value == 0) return;
        /* the explosion it sets off now: its rows where the catalogue shows them (edits land there) */
        static const uint16_t blast[] = {16, 20, 24}, hurt[] = {4, 8, 12, 16, 20, 24, 28, 32, 36};
        std::string name = RefLabel(r, value);
        int shown = 0;
        for (uint16_t offset : blast)
            if (const Row* s = g_catalog.Setting("ExplosionSettings", (uint64_t)value, offset)) { Item v; v.row = s; v.depth = depth; v.prefix = name; items.push_back(v); shown++; }
        if (uint32_t damage = ExplosionDamage((uint32_t)value))
            for (uint16_t offset : hurt)
                if (const Row* s = g_catalog.Setting("DamageInfo", damage, offset)) { Item v; v.row = s; v.depth = depth; v.prefix = name; items.push_back(v); shown++; }
        if (shown == 0) note(name + ": no item of the editor shows its fields", depth);
    };
    for (auto& c : object->page)
    {
        if (c.plain) { row(c.parts[0].rows[0], 0, c.prefix); continue; }
        /* a vehicle weapon's category while its slot holds another weapon: that weapon's stats, editable there */
        const Row::Option* swapped = nullptr;
        for (auto& p : c.parts)
            for (const Row* r : p.rows)
                if (!r->mountSlot.empty() && swapped == nullptr)
                    if (const Row* slot = g_catalog.Entry("mounts", MountVehicle(r), r->mountSlot); slot != nullptr && !slot->options.empty())
                        if (const Row::Option* o = slot->OptionOf(ViewOf(slot).value); o != nullptr && o != &slot->options[0]) swapped = o;
        if (swapped != nullptr)
        {
            static const std::vector<std::pair<std::string, std::vector<const Row*>>> none;
            auto& sections = swapped->target.empty() ? none : WeaponSections(swapped->target);
            Item h;
            h.header = h.category = true; h.title = c.title + SEP + swapped->label; h.key = c.key; h.open = IsOpen(c.key);
            for (auto& [title, rows] : sections) { h.count += (int)rows.size(); h.rows.insert(h.rows.end(), rows.begin(), rows.end()); }
            h.parts = (int)sections.size();
            items.push_back(h);
            if (!h.open) continue;
            if (sections.empty()) { note(swapped->label + ": no page of the editor shows its stats", 1); continue; }
            for (auto& [title, rows] : sections)
            {
                Item s;
                s.header = true; s.title = title; s.key = c.key + "|" + swapped->target + "|" + title; s.open = IsOpen(s.key);
                s.count = (int)rows.size(); s.depth = 1;
                s.rows = rows;
                items.push_back(s);
                if (s.open) for (const Row* r : rows) row(r, 2, "");
            }
            continue;
        }
        Item h;
        h.header = h.category = true; h.title = c.title; h.key = c.key; h.open = IsOpen(c.key); h.count = c.Count(); h.parts = (int)c.parts.size();
        for (auto& p : c.parts) h.rows.insert(h.rows.end(), p.rows.begin(), p.rows.end());
        items.push_back(h);
        if (!h.open) continue;
        if (c.flat) { for (const Row* r : c.parts[0].rows) row(r, 1, ""); continue; }
        for (auto& p : c.parts)
        {
            if (p.fold) { row(p.rows[0], 1, p.prefix); continue; }
            if (std::all_of(p.rows.begin(), p.rows.end(), hidden)) continue;
            Item s;
            s.header = true; s.title = p.title; s.key = p.key; s.open = IsOpen(p.key); s.count = (int)p.rows.size(); s.depth = 1;
            s.rows.assign(p.rows.begin(), p.rows.end());
            items.push_back(s);
            if (s.open) for (const Row* r : p.rows) row(r, 2, "");
        }
    }
    return items;
}

void Toggle(const std::string& key, int open = -1)
{
    bool now = IsOpen(key), want = open < 0 ? !now : open == 1;
    if (want == now) return;
    if (want) g_open.insert(key); else g_open.erase(key);
    g_layoutVersion++;
    SaveSettings();
}

/* Collapse or expand every header of the page; the focused row stays focused when it is still shown. */
void SetPageOpen(const Object* object, bool open)
{
    if (object == nullptr) return;
    auto before = Items(object);
    g_layoutVersion++;
    Item focused = g_field < (int)before.size() ? before[g_field] : Item{};
    for (auto& c : object->page)
    {
        if (c.plain) continue;
        if (open) g_open.insert(c.key); else g_open.erase(c.key);
        if (!c.flat) for (auto& p : c.parts) if (!p.fold) { if (open) g_open.insert(p.key); else g_open.erase(p.key); }
    }
    SaveSettings();
    auto after = Items(object);
    g_field = 0;
    for (int i = 0; i < (int)after.size(); i++)
    {
        if ((focused.row && after[i].row == focused.row) || (!focused.key.empty() && after[i].key == focused.key)) { g_field = i; break; }
        if (focused.row && after[i].header && after[i].depth == 0 &&
            std::find(after[i].rows.begin(), after[i].rows.end(), focused.row) != after[i].rows.end()) g_field = i;
    }
}

bool PageOpen(const Object* object)
{
    if (object == nullptr) return false;
    for (auto& c : object->page)
    {
        if (c.plain) continue;
        if (IsOpen(c.key)) return true;
    }
    return false;
}

std::string RowLabel(const Item& item, float max)
{
    if (item.prefix.empty()) return g_cv.Fit(item.row->label, max, SZ_LABEL);
    std::string rest = SEP + item.row->label;
    float room = max - g_cv.Measure(rest, SZ_LABEL);
    if (g_cv.Measure(item.prefix, SZ_LABEL) <= room) return item.prefix + rest;
    room = std::max(max * 0.3f, room);
    std::string cut = g_cv.Fit(item.prefix, room, SZ_LABEL) + SEP;
    return cut + g_cv.Fit(item.row->label, max - g_cv.Measure(cut, SZ_LABEL), SZ_LABEL);
}

/* ---- editing a value ---- */
void PickerFill()
{
    const Row* row = g_picker.row;
    auto& out = g_picker.entries;
    out.clear();
    std::string filter = Lower(g_picker.filter);
    auto match = [&](const std::string& text) { return filter.empty() || Lower(text).find(filter) != std::string::npos; };
    if (!row->choice.empty())
    {
        std::string group;
        for (auto& o : row->options)
        {
            if (!match(o.label + " " + o.group + " " + o.note)) continue;
            if (!o.group.empty() && o.group != group) { group = o.group; out.push_back({true, 0, group}); } /* a pod's pickups by kind */
            out.push_back({false, o.value, o.label, o.note, o.icon});
        }
        return;
    }
    if (row->Valued())
    {
        RowView v = ViewOf(row, false);
        for (auto& o : row->options)
            if (match(o.label)) out.push_back({false, o.value, o.label, v.hasBase && o.value == v.base ? "the game's own" : ""});
        return;
    }
    RowView v = ViewOf(row, false);
    double own = v.hasBase ? v.base : v.value;
    std::string what = row->pick == "status" ? "applies" : row->pick == "explosion" ? "sets off" : "fires";
    std::string ownLabel = (row->pick == "projectile" ? "Own projectile: " : row->pick == "explosion" ? "Own explosion: " : "Own status: ") + RefLabel(row, own);
    if (match(ownLabel)) out.push_back({false, own, ownLabel, "what it " + what + " in the game's own data"});
    if (row->pick != "projectile" && own != 0 && match(row->pick == "status" ? "Empty" : "None"))
        out.push_back({false, 0, row->pick == "status" ? "Empty" : "None", row->pick == "status" ? "an empty slot" : "no explosion"});
    auto list = g_catalog.references.find(row->pick);
    if (list == g_catalog.references.end()) return;
    std::string group;
    for (auto& o : list->second)
    {
        if (o.value == own || !match(o.label + " " + o.group + " " + o.note)) continue;
        if (std::find(row->exclude.begin(), row->exclude.end(), o.value) != row->exclude.end()) continue;
        if (o.group != group) { group = o.group; out.push_back({true, 0, group}); }
        out.push_back({false, o.value, o.label, o.note, "", o.warnings});
    }
}

/* Whether `i` is an entry of the list (a filter that matches nothing leaves it empty and the index -1). */
bool PickerHas(int i) { return i >= 0 && i < (int)g_picker.entries.size(); }

/* The next selectable entry from `from` going `step` (headers are skipped), or `from`. */
int PickerStep(int from, int step)
{
    int n = (int)g_picker.entries.size();
    for (int i = from + step; i >= 0 && i < n; i += step) if (!g_picker.entries[i].header) return i;
    return from;
}

void PickerSelect(double value)
{
    g_picker.index = PickerStep(-1, 1);
    for (size_t i = 0; i < g_picker.entries.size(); i++)
        if (!g_picker.entries[i].header && g_picker.entries[i].value == value) { g_picker.index = (int)i; break; }
}

void OpenPicker(const Row* row)
{
    g_picker.on = true;
    g_picker.row = row;
    g_picker.filter.clear();
    PickerFill();
    PickerSelect(ViewOf(row).value);
    g_scroll["picker"] = std::max(0, g_picker.index - 4);
    g_seen.erase("picker");
}

void PickerChoose(int i)
{
    if (!PickerHas(i) || g_picker.entries[i].header) return;
    const Row* row = g_picker.row;
    double value = g_picker.entries[i].value;
    if (row->pick == "status" && value == 0 && LaterSlotFilled(row))
    { Toast("Empty the later status slots first: the slots fill in order, from slot 1", ERROR_); return; }
    Stage(row, value);
    g_picker.on = false;
    Sound(sounds::CLICK);
}

void BeginEdit(const Row* row, const std::string& text, bool fresh)
{
    if (RefuseLocked()) return;
    std::string lock = LockOf(row), block = SlotBlock(row);
    if (!lock.empty() || !block.empty()) { Toast("Locked for now: " + (lock.empty() ? block : lock), ERROR_); return; }
    if (!row->choice.empty() || !row->pick.empty() || row->Valued()) { OpenPicker(row); return; }
    RowView v = ViewOf(row);
    g_edit.on = true;
    g_edit.row = row;
    g_edit.fresh = fresh && text.empty();
    g_edit.buffer = !text.empty() ? text : v.has ? Format(row, v.value) : "";
}

bool Valid(const Row* row, double v, double* out, std::string* error);

bool Parse(const Row* row, const std::string& text, double* out, std::string* error)
{
    std::string t = text;
    if (t.find('.') == std::string::npos && std::count(t.begin(), t.end(), ',') == 1) std::replace(t.begin(), t.end(), ',', '.');
    char* end = nullptr;
    double v = strtod(t.c_str(), &end);
    if (t.empty() || end == t.c_str() || *end != 0 || !isfinite(v)) { *error = "Enter a number."; return false; }
    if (row->unlimited && (v == -1 || v == 0)) { *out = 4294967295.0; return true; } /* -1 or 0: unlimited */
    return Valid(row, v, out, error);
}

/* A number as the row takes it (whole, in range, fitting its storage), or false with *error. */
bool Valid(const Row* row, double v, double* out, std::string* error)
{
    if (!isfinite(v)) { *error = "Enter a number."; return false; }
    if (row->Whole() && v != floor(v)) { *error = "Whole numbers only."; return false; }
    if ((row->hasMin && v < row->min) || (row->hasMax && v > row->max))
    { *error = "Allowed range: " + Format(row, row->hasMin ? row->min : -INFINITY) + " to " + Format(row, row->max); return false; }
    if (row->type == HF_TYPE_U32 && (v < 0 || v > 4294967295.0)) { *error = v < 0 ? "Must not be negative." : "Number is too large."; return false; }
    if (row->type == HF_TYPE_I32 && (v < -2147483648.0 || v > 2147483647.0)) { *error = "Number is too large."; return false; }
    if (row->type == HF_TYPE_F32 && fabs(v) > 3.4e38) { *error = "Number is too large."; return false; }
    if (row->type == HF_TYPE_U8 && (v < 0 || v > 255)) { *error = "Allowed range: 0 to 255."; return false; }
    if (row->Valued() && row->OptionOf(v) == nullptr) { *error = "not one of its values"; return false; }
    *out = v;
    return true;
}

void CommitEdit()
{
    if (!g_edit.on) return;
    g_edit.on = false;
    double v;
    std::string error;
    if (g_edit.buffer.empty()) return;
    if (!Parse(g_edit.row, g_edit.buffer, &v, &error)) { Toast(error, ERROR_); return; }
    Stage(g_edit.row, v);
}

void Nudge(const Row* row, int direction, bool shift, bool ctrl)
{
    if (!row->pick.empty() || !LockOf(row).empty() || !SlotBlock(row).empty()) return;
    RowView v = ViewOf(row);
    /* mission uses (HD2R Editor's): left from 1 is unlimited, left from unlimited is the most (100) */
    if (row->unlimited && v.value == 4294967295.0) { if (direction < 0) Stage(row, row->hasMax ? row->max : 100); return; }
    if (row->unlimited && direction < 0 && v.value <= (row->hasMin ? row->min : 1)) { Stage(row, 4294967295.0); return; }
    if (!row->choice.empty() || row->Valued())
    {
        int n = (int)row->options.size(), at = 0;
        for (int i = 0; i < n; i++) if (row->options[i].value == v.value) at = i;
        Stage(row, row->options[(at + direction + n) % n].value);
        return;
    }
    if (!v.has) return;
    double step = row->Whole() ? 1 : v.value == 0 ? 0.1 : pow(10, floor(log10(fabs(v.value))) - 1);
    if (shift) step *= 10;
    if (ctrl) step = row->Whole() ? 1 : step / 10;
    double next = v.value + direction * step;
    if (row->Whole()) next = llround(next);
    if (row->hasMin) next = std::max(next, row->min);
    if (row->hasMax) next = std::min(next, row->max);
    if (row->type == HF_TYPE_U32) next = std::max(next, 0.0);
    Stage(row, next);
}

/* Reverts a field: its pending change (or pending reset) dropped, else a reset staged. */
void Revert(const Row* row)
{
    if (PendingOf(row)) { for (const Row* s : g_catalog.Siblings(row)) g_pending.erase(s); g_markVersion++; return; }
    StageReset(row);
}

/* ---- presets (HD2R Editor's .hd2rpreset.json, in the editor's presets folder) ----
 * A preset is {format "hd2r-editor-preset", version 1, name, editor, runtime, created, fields [{key, value, label}]}:
 * each key is an HD2R Editor row key, which the catalogue gives to the rows on the same bytes (mappings/hd2r-keys.json).
 * Importing stages every field HellForge has a row for (APPLY writes them, DISCARD drops them): projectile swaps
 * first, then the rest, leaving out a weapon's own round while the preset swaps it (as HD2R Editor does). Keys with no
 * row yet (enemies, vehicle zones...) are skipped and counted. Exporting writes the changes saved in Local Stats (and
 * the armor perks the editor keeps) as a preset HD2R Editor reads. */
struct PresetFile { std::wstring path; std::string file, name, editor, runtime, error; int fields = 0; };
std::vector<PresetFile> g_presets;
int g_preset;
struct { bool shown = false; std::string name, text; ImU32 colour = TEXT; } g_presetResult;

std::wstring PresetFolder() { return hfjson::Wide(g_host.modFolder) + L"\\presets"; }
std::wstring PresetWriteFolder()
{
    std::wstring sandbox = localstats::SandboxFolder();
    return sandbox.empty() ? PresetFolder() : sandbox + L"\\presets";
}

std::string Narrow(const std::wstring& text)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], n, nullptr, nullptr);
    return out;
}

void ScanPresetFolder(const std::wstring& folder)
{
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW((folder + L"\\*.json").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do
    {
        PresetFile p;
        p.path = folder + L"\\" + found.cFileName;
        p.file = Narrow(found.cFileName);
        std::string text, error;
        hfjson::Value root;
        if (found.nFileSizeHigh || found.nFileSizeLow > (16u << 20)) p.error = "larger than 16 MB";
        else if (!hfjson::ReadFile(p.path, &text)) p.error = "cannot be read";
        else
        {
            if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
            if (!hfjson::Parse(text, &root, &error)) p.error = "not JSON: " + error;
            else if (root.GetString("format") != "hd2r-editor-preset") p.error = "not an HD2R Editor preset";
            else if (root.GetNumber("version", 0) > 1) p.error = "a newer preset format";
            else
            {
                p.name = root.GetString("name", p.file);
                p.editor = root.GetString("editor");
                p.runtime = root.GetString("runtime");
                const hfjson::Value* fields = root.Find("fields");
                p.fields = fields != nullptr && fields->kind == hfjson::Value::Array ? (int)fields->items.size() : 0;
            }
        }
        /* a preset exported this (sandboxed) session replaces the saved one of its name */
        auto same = std::find_if(g_presets.begin(), g_presets.end(), [&](const PresetFile& q) { return q.file == p.file; });
        if (same != g_presets.end()) *same = p; else g_presets.push_back(p);
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

void ScanPresets()
{
    std::wstring folder = PresetFolder(), written = PresetWriteFolder();
    CreateDirectoryW(written.c_str(), nullptr);
    g_presets.clear();
    ScanPresetFolder(folder);
    if (written != folder) ScanPresetFolder(written);
    std::sort(g_presets.begin(), g_presets.end(), [](const PresetFile& a, const PresetFile& b) { return a.file < b.file; });
    g_preset = std::clamp(g_preset, 0, std::max(0, (int)g_presets.size() - 1));
}

/* Reticle styles (WeaponDataComponentData crosshair_type): none (the damage indicator only) and the assault rifle's. */
constexpr double RETICLE_OFF = 3, RETICLE_ON = 4;

/* A flag, a named value or a slot list's entry as the row's number, or false with *why. */
bool PresetStructured(const Row* row, const Row::Hd2rAs& as, const hfjson::Value& v, double* out, std::string* why)
{
    /* HD2R's reticle flag over the style number: off is 3 (damage indicator only), on the weapon's own style, or the
     * assault rifle's (4) where the game's own is off */
    if (as.kind == "reticle" && v.kind == hfjson::Value::Bool)
    {
        RowView view = ViewOf(row, false);
        double own = view.hasBase ? view.base : view.value;
        return Valid(row, !v.boolean ? RETICLE_OFF : own != RETICLE_OFF && row->OptionOf(own) ? own : RETICLE_ON, out, why);
    }
    auto named = [&](const hfjson::Value& x, double* n) {
        if (x.kind == hfjson::Value::Bool) { *n = x.boolean ? 1 : 0; return true; }
        if (x.kind == hfjson::Value::Number) { *n = x.AsNumber(); return true; }
        if (x.kind == hfjson::Value::String)
        {
            auto found = as.names.find(x.Str());
            if (found != as.names.end()) { *n = found->second; return true; }
            *why = "'" + x.Str() + "' is not a value HellForge knows here"; return false;
        }
        *why = "not a value of this kind"; return false;
    };
    /* a pod slot's pickup ({ref: pickup, id} or "empty") or a name (a mounted weapon's id, a charge weapon whose
     * overcharge explosion it takes): the option it names */
    if (as.kind == "pickup" || as.kind == "named")
    {
        std::string id = v.kind == hfjson::Value::String ? v.Str() :
            as.kind == "pickup" && v.kind == hfjson::Value::Object && v.GetString("ref") == "pickup" ? v.GetString("id") : "";
        auto found = as.names.find(id);
        if (found == as.names.end()) { *why = id.empty() ? "not a value of this kind" : "'" + id + "' is not one HellForge offers here"; return false; }
        *out = found->second;
        return true;
    }
    double n = 0;
    if (as.kind == "slots")
    {
        if (v.kind != hfjson::Value::Array) { *why = "not a list"; return false; }
        if (as.slot < (int)v.items.size() && !named(v.items[as.slot], &n)) return false; /* past the list: an empty slot */
    }
    else if (!named(v, &n)) return false;
    return Valid(row, n, out, why);
}

/* A preset's stored value as the row's number (Local Stats's and the picker's), or false with *why. */
bool PresetValue(const Row* row, const std::string& key, const hfjson::Value& v, double* out, std::string* why)
{
    if (auto as = row->hd2rAs.find(key); as != row->hd2rAs.end()) return PresetStructured(row, as->second, v, out, why);
    const std::string& kind = row->hd2rKind;
    auto ref = [&](const char* expect) { return v.kind == hfjson::Value::Object && v.GetString("ref") == expect; };
    if (kind == "passive")
    {
        if (v.kind == hfjson::Value::String && (v.Str() == "kit" || v.Str() == "none")) { *out = -1; return true; }
        if (v.kind == hfjson::Value::Number && row->OptionOf(v.AsNumber())) { *out = v.AsNumber(); return true; }
        *why = "not a passive HellForge can give"; return false;
    }
    if (kind == "status")
    {
        if (v.kind != hfjson::Value::String) { *why = "not a status id"; return false; }
        if (v.Str() == "none") { *out = 0; return true; }
        auto s = g_catalog.hd2rStatuses.find(v.Str());
        if (s == g_catalog.hd2rStatuses.end() || !g_catalog.Reference("status", s->second)) { *why = "status " + v.Str() + " is not one a damage type can apply"; return false; }
        *out = s->second; return true;
    }
    if (kind == "explosion")
    {
        if (ref("none")) { *out = 0; return true; }
        auto e = ref("explosion") ? g_catalog.hd2rExplosions.find(v.GetString("id")) : g_catalog.hd2rExplosions.end();
        if (e == g_catalog.hd2rExplosions.end()) { *why = "not a catalogued explosion"; return false; }
        if (std::find(row->exclude.begin(), row->exclude.end(), e->second) != row->exclude.end()) { *why = "an explosion this slot does not offer"; return false; }
        *out = e->second; return true;
    }
    if (kind == "projectile")
    {
        auto o = ref("output") ? g_catalog.hd2rOutputs.find(v.GetString("id")) : g_catalog.hd2rOutputs.end();
        if (o == g_catalog.hd2rOutputs.end()) { *why = "not a projectile donor HellForge knows"; return false; }
        *out = o->second; return true;
    }
    if (kind == "uses" && v.kind == hfjson::Value::String && v.Str() == "unlimited")
    {
        if (!row->unlimited) { *why = "unlimited uses are not offered here"; return false; }
        *out = 4294967295.0; return true;
    }
    if (v.kind != hfjson::Value::Number) { *why = "not a number"; return false; }
    return Valid(row, v.AsNumber(), out, why);
}

void ImportPreset(const PresetFile& p)
{
    if (RefuseLocked()) return;
    std::string text, error;
    hfjson::Value root;
    if (!p.error.empty() || !hfjson::ReadFile(p.path, &text)) { Toast("The preset cannot be read", ERROR_); return; }
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text.erase(0, 3);
    if (!hfjson::Parse(text, &root, &error)) { Toast("The preset is not JSON: " + error, ERROR_); return; }
    const hfjson::Value* fields = root.Find("fields");
    if (fields == nullptr || fields->kind != hfjson::Value::Array) { Toast("The preset has no fields", ERROR_); return; }
    std::map<std::string, const hfjson::Value*> byKey; /* a key twice: the last value, as HD2R Editor does */
    for (const hfjson::Value& f : fields->items)
        if (f.kind == hfjson::Value::Object && !f.GetString("key").empty()) if (const hfjson::Value* v = f.Find("value")) byKey[f.GetString("key")] = v;
    int staged = 0, locked = 0, refused = 0;
    std::map<std::string, int> skipped;
    std::string firstWhy;
    std::set<const Row*> done;
    for (int pass = 0; pass < 2; pass++) /* the projectile swaps first: they decide which round's rows apply */
        for (auto& e : byKey)
        {
            auto range = g_catalog.byHd2r.equal_range(e.first);
            if (range.first == range.second)
            {
                if (pass == 0) skipped[e.first.substr(0, e.first.find('|'))]++;
                continue;
            }
            bool counted = false; /* a slot list stages several rows: one value of the preset */
            bool plain = false;   /* a number's key: its first row only (the page it belongs to, not a shared one) */
            for (auto it = range.first; it != range.second; ++it)
            {
                const Row* row = it->second;
                if (!row->hd2rAs.count(e.first)) { if (plain) continue; plain = true; }
                if ((row->hd2rKind == "projectile" && row->fires.empty()) != (pass == 0) || done.count(row)) continue;
                double value;
                std::string why;
                if (!PresetValue(row, e.first, *e.second, &value, &why))
                {
                    if (!counted) refused++;
                    counted = true;
                    if (firstWhy.empty()) firstWhy = row->object->name + SEP + row->label + ": " + why;
                    continue;
                }
                if (pass == 1 && !LockOf(row).empty()) { if (!counted) locked++; counted = true; continue; }
                done.insert(row);
                Stage(row, value);
                if (!counted) staged++;
                counted = true;
            }
        }
    int skip = 0;
    std::string families;
    static const std::map<std::string, std::string> names = {{"en", "enemies"}, {"ve", "vehicles"}, {"mi", "missions"}, {"mo", "missions"},
        {"mt", "missions"}, {"hd", "Helldiver"}, {"pw", "weapons"}, {"sp", "support weapons"}, {"st", "stratagems"}, {"th", "throwables"},
        {"sb", "backpacks"}, {"bo", "boosters"}, {"ex", "explosions"}, {"me", "melee"}, {"rs", "resupply"}, {"ap", "armor perks"},
        {"se", "status effects"}, {"ar", "armor"}, {"ed", "spawning"}, {"dm", "destruction"}, {"eo", "spawning"}};
    for (auto& s : skipped)
    {
        auto name = names.find(s.first);
        skip += s.second;
        families += (families.empty() ? "" : ", ") + (name != names.end() ? name->second : s.first) + " " + std::to_string(s.second);
    }
    g_presetResult.shown = true;
    g_presetResult.name = root.GetString("name", p.file);
    g_presetResult.text = std::to_string(staged) + " staged (APPLY writes them)" +
        (skip ? ", " + std::to_string(skip) + " with no HellForge row yet (" + families + ")" : "") +
        (locked ? ", " + std::to_string(locked) + " for a weapon's own round while the preset swaps it" : "") +
        (refused ? ", " + std::to_string(refused) + " refused (" + firstWhy + (refused > 1 ? ", ..." : "") + ")" : "");
    g_presetResult.colour = staged ? OK : PENDING;
    g_host.api->log(g_host.mod, "editor: preset %s: %s", g_presetResult.name.c_str(), g_presetResult.text.c_str());
    Sound(staged ? sounds::APPLY : sounds::ERROR_);
    Toast(std::to_string(staged) + " preset value" + (staged == 1 ? "" : "s") + " staged from " + g_presetResult.name, staged ? OK : PENDING);
}

/* A structured key's value as a preset stores it, from its rows' values (a slot list: every slot's, in order). */
hfjson::Value PresetEncodeStructured(const std::string& key, const Row::Hd2rAs& as, const std::map<const Row*, double>& values)
{
    auto name = [&](double n) {
        for (auto& [k, v] : as.names) if (v == n) return hfjson::Value::MakeString(k);
        return hfjson::Value::MakeNumber(n, n == floor(n));
    };
    hfjson::Value list;
    if (values.empty()) return list;
    if (as.kind == "bool") return hfjson::Value::MakeBool(values.begin()->second != 0);
    if (as.kind == "reticle") return hfjson::Value::MakeBool(values.begin()->second != RETICLE_OFF);
    if (as.kind == "enum") return hfjson::Value::MakeNumber(values.begin()->second, true);
    if (as.kind == "pickup" || as.kind == "named")
    {
        for (auto& [k, v] : as.names)
            if (v == values.begin()->second)
            {
                if (as.kind == "named" || k == "empty") return hfjson::Value::MakeString(k);
                hfjson::Value pickup;
                pickup.kind = hfjson::Value::Object;
                pickup.members.emplace_back("ref", hfjson::Value::MakeString("pickup"));
                pickup.members.emplace_back("id", hfjson::Value::MakeString(k));
                return pickup;
            }
        return list;
    }
    if (as.kind != "slots") return list; /* "ap": the traits carry its bytes */
    list.kind = hfjson::Value::Array;
    std::vector<std::pair<int, double>> slots;
    for (auto& [row, v] : values) slots.push_back({row->hd2rAs.at(key).slot, v});
    std::sort(slots.begin(), slots.end());
    bool rates = as.names.empty(); /* fire rates: all three, 0 an empty slot */
    if (!rates) while (!slots.empty() && slots.back().second == 0) slots.pop_back();
    for (auto& [slot, v] : slots) if (rates || v != 0) list.items.push_back(rates ? hfjson::Value::MakeNumber(v, false) : name(v));
    return list;
}

/* A row's value as a preset stores it, or a null value when it has none HD2R Editor would read. */
hfjson::Value PresetEncode(const Row* row, double v)
{
    hfjson::Value out;
    auto object = [&](const char* ref, const std::string* id) {
        out.kind = hfjson::Value::Object;
        out.members.emplace_back("ref", hfjson::Value::MakeString(ref));
        if (id) out.members.emplace_back("id", hfjson::Value::MakeString(*id));
    };
    const std::string& kind = row->hd2rKind;
    if (kind == "passive") { if (v < 0) return hfjson::Value::MakeString("kit"); return hfjson::Value::MakeNumber(v, true); }
    if (kind == "status")
    {
        if (v == 0) return hfjson::Value::MakeString("none");
        auto s = g_catalog.statusOf.find(v);
        return s != g_catalog.statusOf.end() ? hfjson::Value::MakeString(s->second) : out;
    }
    if (kind == "explosion")
    {
        if (v == 0) { object("none", nullptr); return out; }
        auto e = g_catalog.explosionOf.find(v);
        if (e != g_catalog.explosionOf.end()) object("explosion", &e->second);
        return out;
    }
    if (kind == "projectile")
    {
        auto o = g_catalog.outputOf.find(v);
        if (o != g_catalog.outputOf.end()) object("output", &o->second);
        return out;
    }
    if (row->unlimited && v == 4294967295.0) return hfjson::Value::MakeString("unlimited");
    return hfjson::Value::MakeNumber(v, row->Whole());
}

void ExportPreset()
{
    std::map<std::string, std::pair<const Row*, double>> fields;
    std::map<std::string, hfjson::Value> structured; /* flags, named values, slot lists: from every slot's row */
    for (const Row* row : SavedRows())
    {
        double v;
        if (row->hd2r.empty() || !Saved(row, &v)) continue;
        auto as = row->hd2rAs.find(row->hd2r[0]);
        if (as == row->hd2rAs.end()) { fields.emplace(row->hd2r[0], std::make_pair(row, v)); continue; }
        if (structured.count(row->hd2r[0])) continue;
        std::map<const Row*, double> values;
        auto range = g_catalog.byHd2r.equal_range(row->hd2r[0]);
        for (auto it = range.first; it != range.second; ++it)
        {
            double s;
            if (Saved(it->second, &s)) values[it->second] = s;
            else if (RowView view = ViewOf(it->second, false); view.has) values[it->second] = view.value;
        }
        hfjson::Value value = PresetEncodeStructured(row->hd2r[0], as->second, values);
        if (!value.IsNull()) structured[row->hd2r[0]] = value;
        fields.emplace(row->hd2r[0], std::make_pair(row, v));
    }
    if (fields.empty()) { Toast("Nothing to export: no change is saved yet", PENDING); return; }
    SYSTEMTIME now;
    GetSystemTime(&now);
    char stamp[32], created[32];
    snprintf(stamp, sizeof stamp, "%04d-%02d-%02d %02d%02d%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    snprintf(created, sizeof created, "%04d-%02d-%02dT%02d:%02d:%02dZ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::string name = std::string("HellForge ") + stamp;
    hfjson::Value root, list;
    root.kind = hfjson::Value::Object;
    list.kind = hfjson::Value::Array;
    int written = 0;
    for (auto& f : fields)
    {
        auto s = structured.find(f.first);
        hfjson::Value value = s != structured.end() ? s->second : PresetEncode(f.second.first, f.second.second);
        if (value.IsNull()) continue;
        hfjson::Value field;
        field.kind = hfjson::Value::Object;
        field.members.emplace_back("key", hfjson::Value::MakeString(f.first));
        field.members.emplace_back("value", value);
        field.members.emplace_back("label", hfjson::Value::MakeString(f.second.first->object->name + SEP + f.second.first->label));
        list.items.push_back(field);
        written++;
    }
    root.members.emplace_back("format", hfjson::Value::MakeString("hd2r-editor-preset"));
    root.members.emplace_back("version", hfjson::Value::MakeNumber(1, true));
    root.members.emplace_back("name", hfjson::Value::MakeString(name.substr(0, 40)));
    root.members.emplace_back("editor", hfjson::Value::MakeString("HellForge"));
    root.members.emplace_back("runtime", hfjson::Value::MakeString(g_catalog.hd2rRuntime));
    root.members.emplace_back("created", hfjson::Value::MakeString(created));
    root.members.emplace_back("fields", list);
    CreateDirectoryW(PresetWriteFolder().c_str(), nullptr);
    std::wstring path = PresetWriteFolder() + L"\\" + hfjson::Wide(name) + L".hd2rpreset.json";
    if (!hfjson::WriteAtomically(path, hfjson::Write(root))) { Toast("The preset could not be written", ERROR_); return; }
    ScanPresets();
    for (int i = 0; i < (int)g_presets.size(); i++) if (g_presets[i].path == path) g_preset = i;
    Sound(sounds::APPLY);
    Toast(std::to_string(written) + " saved change" + (written == 1 ? "" : "s") + " exported as " + name, OK);
}

void OpenPresetFolder()
{
    std::wstring folder = PresetFolder();
    CreateDirectoryW(folder.c_str(), nullptr);
    std::wstring command = L"explorer.exe \"" + folder + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    /* a session's game may not start other programs (its job holds it alone): the folder is said instead */
    else Toast("The presets are in " + Narrow(folder), PENDING);
}

/* ---- the errors list, the confirmation, the logs (HD2R Editor's errors popup, its RESET ITEM confirmation, LOGS) ---- */
struct { bool on = false; int index = 0; } g_errorsPopup;

/* Every refused field, by object and field. */
std::vector<const Row*> ErrorRows()
{
    std::vector<std::pair<std::string, const Row*>> keyed;
    for (auto& e : g_errors) keyed.emplace_back(Lower(e.first->object->name + " " + e.first->label), e.first);
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<const Row*> out;
    for (auto& k : keyed) out.push_back(k.second);
    return out;
}

void OpenErrors() { g_errorsPopup.on = true; g_errorsPopup.index = 0; g_seen.erase("errors"); Sound(sounds::OPEN); }

/* RESET ITEM: the rows of an object holding a change (pending, applied or refused), one per bytes. */
std::vector<const Row*> ItemChanges(const Object* object)
{
    std::vector<const Row*> rows;
    std::set<std::string> seen;
    if (object == nullptr) return rows;
    for (const Row* row : object->rows)
    {
        double v;
        if (seen.count(row->link) || !(PendingOf(row) || Saved(row, &v) || g_errors.count(row))) continue;
        seen.insert(row->link);
        rows.push_back(row);
    }
    return rows;
}

void ResetItem(const Object* object)
{
    if (RefuseLocked()) return;
    auto rows = ItemChanges(object);
    if (rows.empty()) return;
    std::set<std::string> shared;
    for (const Row* r : rows)
        for (const Row* s : g_catalog.Siblings(r)) if (s->object != object) shared.insert(s->object->name);
    std::string names;
    int n = 0;
    for (auto& s : shared) { if (n < 3) names += (n ? ", " : "") + s; n++; }
    if (n > 3) names += " (+" + std::to_string(n - 3) + ")";
    /* staged only (APPLY writes it, DISCARD takes it back): no confirmation needed */
    int marked = 0, dropped = 0;
    for (const Row* r : rows)
    {
        bool pending = PendingOf(r) != nullptr;
        for (const Row* s : g_catalog.Siblings(r)) g_pending.erase(s);
        if (StageReset(r)) marked++; else if (pending) dropped++;
    }
    g_markVersion++;
    Toast(marked ? std::to_string(marked) + " reset" + (marked == 1 ? "" : "s") + " of " + object->name + " pending: APPLY writes them, DISCARD takes them back" +
            (names.empty() ? "" : " (shared, so also " + names + ")") :
        std::to_string(dropped) + " pending change" + (dropped == 1 ? "" : "s") + " dropped", marked ? PENDING : FAINT);
}

/* The session's logs (HellForge's modloader.log, or the editor's own), read again when they grow. */
struct LogView
{
    int source = 0, filter = 0, index = 0;
    bool follow = true;
    std::wstring path;
    uint64_t size = 0;
    ULONGLONG checked = 0;
    std::vector<std::string> lines;
    std::vector<int> kinds; /* 0 info, 1 warning, 2 error */
    std::vector<int> shown;
} g_logs;
const char* const LOG_FILTERS[] = {"ALL", "ERRORS", "WARNINGS"};

std::wstring LogPath(int source)
{
    if (g_host.api->size < offsetof(HfApi, logs_folder) + sizeof(void*) || g_host.api->logs_folder == nullptr) return L"";
    const char* folder = g_host.api->logs_folder(g_host.mod);
    if (folder == nullptr || !folder[0]) return L"";
    std::string id = g_host.api->mod_id ? g_host.api->mod_id(g_host.mod) : "hellforge.editor";
    return hfjson::Wide(folder) + L"\\" + (source == 0 ? std::wstring(L"modloader.log") : hfjson::Wide("mod-" + id + ".log"));
}

int LogKind(const std::string& line)
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
    g_logs.lines.clear();
    g_logs.kinds.clear();
    size_t start = at.QuadPart > 0 ? text.find('\n') + 1 : 0; /* (a cut first line is left out) */
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) { g_logs.kinds.push_back(LogKind(line)); g_logs.lines.push_back(line); }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    LogFilter();
}

// Explorer on a path; false when Windows refused (a session's game may not start other programs: its job holds it alone).
bool OpenWith(const std::wstring& arguments)
{
    std::wstring command = L"explorer.exe " + arguments;
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

/* ---- navigation ---- */
std::vector<Object*> Objects();
std::map<int, std::string> g_selected; /* each category's selected object (its id): back on the category, it is selected again */

void SelectCategory(int i)
{
    if (i < 0 || i >= (int)g_cats.size() || g_cats[i].group) return;
    if (g_cat == i) return;
    auto list = Objects();
    if (g_obj >= 0 && g_obj < (int)list.size()) g_selected[g_cat] = list[g_obj]->id;
    g_cat = i; g_obj = 0; g_field = 0; g_scroll["obj"] = 0; g_scroll["fields"] = 0; g_search = {};
    if (auto kept = g_selected.find(i); kept != g_selected.end())
    {
        list = Objects();
        for (int k = 0; k < (int)list.size(); k++) if (list[k]->id == kept->second) { g_obj = k; g_seen.erase("obj"); break; }
    }
}

void MoveCategory(int delta)
{
    int i = g_cat;
    do i += delta; while (i >= 0 && i < (int)g_cats.size() && g_cats[i].group);
    if (i >= 0 && i < (int)g_cats.size()) SelectCategory(i);
}

void SelectObject(int i)
{
    if (i != g_obj) { g_obj = i; g_field = 0; g_scroll["fields"] = 0; g_seen.erase("fields"); }
}

/* Opens an object in BROWSE: its category (filters cleared) with the object selected. */
bool GotoObject(const Object* object)
{
    for (int i = 0; i < (int)g_cats.size(); i++)
        if (!g_cats[i].group && g_catalog.navs[g_cats[i].nav].id == object->category) SelectCategory(i);
    g_search = {};
    g_group.erase(object->category);
    g_seen.erase("obj");
    auto list = Objects();
    for (int i = 0; i < (int)list.size(); i++) if (list[i] == object) { SelectObject(i); return true; }
    return false;
}

/* Opens a field in BROWSE: its object, the headers holding it opened, the row selected and scrolled into view. */
void Reveal(const Row* row)
{
    GotoObject(row->object);
    for (auto& c : row->object->page)
        for (auto& p : c.parts)
            if (std::find(p.rows.begin(), p.rows.end(), row) != p.rows.end()) { Toggle(c.key, 1); Toggle(p.key, 1); }
    auto items = Items(row->object);
    for (int i = 0; i < (int)items.size(); i++) if (items[i].row == row) { g_field = i; g_scroll["fields"] = std::max(0, i - 5); g_seen.erase("fields"); }
    g_view = BROWSE;
    g_focus = FIELDS;
}

/* ---- search (SEARCH: every object, by name and group; a word of four letters or more also names a category) ---- */
struct Result { Object* object; std::string sub; bool prefix; int order; };

std::vector<Result> SearchResults(const std::string& text)
{
    std::vector<Result> out;
    auto words = Words(text);
    if (words.empty()) return out;
    auto namesCategory = [](const std::string& word, const std::string& label) {
        if (word.size() < 4) return false;
        for (auto& part : Words(label)) if (part.rfind(word, 0) == 0) return true;
        return false;
    };
    for (auto& nav : g_catalog.navs)
        for (Object* o : g_catalog.Of(nav.id))
        {
            std::string hay = Lower(o->name) + " " + Lower(o->subtitle) + " " + o->search;
            bool ok = true;
            for (auto& w : words)
                if (hay.find(w) == std::string::npos && !namesCategory(w, nav.label) && !namesCategory(w, nav.group)) { ok = false; break; }
            if (ok)
                out.push_back({o, nav.label + (o->subtitle.empty() ? "" : std::string("  \xC2\xB7  ") + o->subtitle),
                    Lower(o->name).rfind(words[0], 0) == 0, (int)out.size()});
        }
    std::stable_sort(out.begin(), out.end(), [](const Result& a, const Result& b) {
        if (a.prefix != b.prefix) return a.prefix;
        return Lower(a.object->name) < Lower(b.object->name);
    });
    return out;
}

/* ---- CHANGES ---- */
struct Change { const Row* row; double value; int state; bool reset; }; /* state 0 edited, 1 pending, 2 error */

const std::vector<Change>& Changes()
{
    static unsigned stats, version, errors;
    static std::vector<Change> out;
    unsigned now = localstats::Version();
    if (stats == now && version == g_markVersion && errors == g_errorsVersion) return out;
    stats = now; version = g_markVersion; errors = g_errorsVersion;
    out.clear();
    std::set<const Row*> seen;
    for (auto& p : g_pending)
    {
        /* a reset's value (the base) is read when the row is drawn */
        out.push_back({p.first, p.second.value, 1, p.second.reset});
        seen.insert(p.first);
    }
    for (const Row* row : SavedRows())
    {
        if (seen.count(row)) continue;
        double v = 0;
        Saved(row, &v);
        out.push_back({row, v, g_errors.count(row) ? 2 : 0, false});
        seen.insert(row);
    }
    for (auto& e : g_errors) /* a refused change Local Stats does not hold */
        if (!seen.count(e.first)) { out.push_back({e.first, 0, 2, false}); seen.insert(e.first); }
    std::vector<std::pair<std::string, Change>> keyed;
    keyed.reserve(out.size());
    for (auto& c : out) keyed.emplace_back(Lower(c.row->object->name + " " + c.row->label), c);
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < keyed.size(); i++) out[i] = keyed[i].second;
    return out;
}

/* ---- MODS: the session's mods, HellForge's (code and stats mods) and Lua's (HD2Runtime mods, through the Lua bridge),
 * and the values each one holds: the fields whose holder get_live or get_stat names (the blue MOD mark). A field two mods
 * change is listed under the one whose value it holds. ---- */
struct ModInfo { std::string id, name, detail, folder; uint32_t kind = 0; bool loaded = true; std::vector<const Row*> rows; };
struct ModEntry { std::string heading; int mod = -1; };
std::vector<ModInfo> g_modList;
ULONGLONG g_modsAt;
int g_modEntry = -1, g_modRow;
bool HasMods() { return g_host.api->size >= offsetof(HfApi, mods) + sizeof(void*) && g_host.api->mods != nullptr; }

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

const ModInfo* SelectedMod()
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
std::string LuaModTitle(const std::string& name)
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
std::pair<std::string, std::string> ManifestName(const std::string& folder)
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

/* The list again (every 2 s while MODS is open, or now): the mods from the loader, their names, and the values each holds
 * (the holder index HolderTick keeps). The selected mod stays selected. */
void ScanMods(bool now)
{
    ULONGLONG tick = GetTickCount64();
    if (!now && g_modsAt != 0 && tick - g_modsAt < 2000) return;
    g_modsAt = tick;
    const ModInfo* was = SelectedMod();
    std::string keep = was != nullptr ? was->id : "";
    std::vector<ModInfo> list;
    if (HasMods())
        for (uint32_t i = 0; i < 1024; i++)
        {
            HfModInfo info{};
            info.size = sizeof info;
            if (!g_host.api->mods(g_host.mod, i, &info)) break;
            if (info.kind == HF_MOD_EDITOR || info.id == nullptr) continue;
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
            list.push_back(m);
        }
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
    g_modList = std::move(list);
    auto entries = ModEntries();
    g_modEntry = -1;
    for (int i = 0; i < (int)entries.size(); i++)
    {
        if (entries[i].mod < 0) continue;
        if (g_modEntry < 0) g_modEntry = i;
        if (g_modList[entries[i].mod].id == keep) { g_modEntry = i; break; }
    }
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
        if (e.mod < 0)
        {
            bool lua = e.heading == "LUA MODS";
            int count = 0;
            for (const ModInfo& m : g_modList) count += (m.kind == HF_MOD_LUA) == lua;
            g_cv.Text(e.heading, x + 20, y + rh - 16, T(SZ_HEADING, GOLD, ui::TITLE));
            g_cv.Text(count == 0 ? (lua ? "none (an HD2Runtime mod shows once it changes a value)" : "none") : std::to_string(count),
                x + lw - 20, y + rh - 16, T(SZ_TINY, FAINT, ui::BODY, ui::RIGHT, lw - 170));
            g_cv.Rect(x + 16, y + rh - 5, lw - 32, 1, LINE_STRONG, 2);
            return;
        }
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

/* ---- keys ---- */
bool Pressed(ImGuiKey key) { return ImGui::IsKeyPressed(key, true); }

/* ---- mod sync (SDK 14): a lobby whose host plays with other gameplay mods ----
 * The session's backend offers to download the host's mods and restart the session with them (docs/mods.md "Playing
 * with other mods"); the loader hands the offer over (sync_state, sync_mod) and the player's answer back (sync_answer).
 * Tick reads it every 30 frames. An offer that needs the player (new, ready, blocked, failed) shows this dialog by itself:
 * alone over the game, with no editor behind it, or in the window when that is open. Esc or closing the window answers
 * no, except while downloading: the download goes on, and the dialog is back when it is ready. */
struct
{
    bool on = false;     /* the dialog is shown */
    bool alone = false;  /* it is shown over the game by itself, the window closed */
    bool sent = false;   /* an answer went: the buttons wait for the backend's next state */
    HfSyncState state{}; /* as last read */
    std::vector<HfSyncMod> mods;
    int index = 0;       /* the selected mod (keyboard) */
    double shownAt = 0;  /* when it last appeared or turned: its answers wait SYNC_ARM seconds */
    int trust = -1;      /* the mod whose TRUST warning shows instead of the list (-1 none) */
    double trustAt = 0;  /* when that warning appeared: its button waits SYNC_ARM seconds too */
} g_sync;
/* A key or click meant for the game (Enter in chat, a shot) when the prompt pops up must not answer it: its buttons and
 * keys wait this long after it appears or its state turns. Esc, which only says no or hides it, works at once. */
const double SYNC_ARM = 1.0;
bool SyncArmed() { return ImGui::GetTime() - g_sync.shownAt >= SYNC_ARM; }
bool SyncShown() { return g_sync.on; }
bool g_windowShown; /* Opened() to Closed() */

bool HasSync() { return g_host.api->size >= offsetof(HfApi, sync_answer) + sizeof(void*) && g_host.api->sync_state != nullptr; }

/* Keys let go while the editor was closed went to the game, not to ImGui: forget every key state on opening and closing,
 * or a key held then (Escape) counts as held when it opens again and closes it at once. */
void ForgetKeys()
{
    ImGuiIO& io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
    io.InputQueueCharacters.resize(0);
    /* where the mouse is now (the queue held the moves): a click before any move lands where it is */
    POINT at;
    HWND window = GetForegroundWindow();
    if (window != nullptr && GetCursorPos(&at) && ScreenToClient(window, &at)) io.AddMousePosEvent((float)at.x, (float)at.y);
}

/* The dialog alone over the game, the window closed: the editor holds the game's input for it, as for the window. */
void ShowSyncAlone(bool on)
{
    if (g_sync.alone == on) return;
    g_sync.alone = on;
    g_host.modal(on);
    ForgetKeys();
    if (on) g_sync.shownAt = ImGui::GetTime();
    sounds::EditorOpen(on);
    g_host.api->log(g_host.mod, on ? "editor: the mod sync prompt shows over the game, the window closed" : "editor: the mod sync prompt goes");
    if (on) Sound(sounds::SHOW);
}

std::string SizeText(uint64_t bytes)
{
    char text[32];
    if (bytes >= 1024 * 1024) snprintf(text, sizeof text, "%.1f MB", (double)bytes / 1048576.0);
    else snprintf(text, sizeof text, "%llu KB", (unsigned long long)((bytes + 1023) / 1024));
    return text;
}

/* The dialog goes (an open window stays). */
void CloseSync()
{
    g_sync.on = false;
    ShowSyncAlone(false);
}

void AnswerSync(uint32_t answer, const char* id = nullptr)
{
    bool sent = g_host.api->sync_answer(g_host.mod, g_sync.state.version, answer, id) != 0;
    /* a trust waits for the backend's new offer (and its download), as accept and restart wait for the next state */
    g_sync.sent = sent && (answer == HF_SYNC_ACCEPT || answer == HF_SYNC_RESTART || answer == HF_SYNC_TRUST);
    g_sync.trust = -1;
    if (!sent) Toast("That offer is gone: the session's backend has a newer one, or is not connected", ERROR_);
    else if (answer == HF_SYNC_OPEN) { Toast("The mod's page opens in your browser (Alt+Tab to it)", OK); Sound(sounds::APPLY); }
    else Sound(answer == HF_SYNC_DECLINE ? sounds::BACK : sounds::APPLY);
    if (!sent || answer == HF_SYNC_DECLINE || answer == HF_SYNC_BLOCK) CloseSync();
}

/* Every 30 frames (Tick): the backend's offer, when it changed. Nothing is allocated unless it did. */
void ReadSync()
{
    static int frames;
    if (!HasSync() || ++frames < 30) return;
    frames = 0;
    HfSyncState now{};
    now.size = sizeof now;
    g_host.api->sync_state(g_host.mod, &now);
    HfSyncState& was = g_sync.state;
    bool turned = now.version != was.version || now.state != was.state;
    if (!turned && now.done == was.done && now.bytes == was.bytes && !strcmp(now.message, was.message) && !strcmp(now.host, was.host)) return;
    if (now.version != was.version || now.mods != g_sync.mods.size())
    {
        g_sync.mods.clear();
        for (uint32_t i = 0; i < now.mods; i++)
        {
            HfSyncMod mod{};
            mod.size = sizeof mod;
            if (!g_host.api->sync_mod(g_host.mod, i, &mod)) break;
            g_sync.mods.push_back(mod);
        }
        g_sync.index = 0;
        g_sync.trust = -1; /* the warning was for the offer that is gone */
    }
    g_sync.state = now;
    if (!turned) return;
    g_sync.sent = false;
    if (now.state == HF_SYNC_NONE) { if (g_sync.on) CloseSync(); return; }
    if (now.state == HF_SYNC_DOWNLOADING) return;
    g_sync.shownAt = ImGui::GetTime();
    g_host.api->log(g_host.mod, "editor: mod sync %u from %s: %u mod(s), %llu bytes to download; the dialog opens", now.version,
        now.host[0] ? now.host : "the host", now.mods, (unsigned long long)now.bytes);
    bool fresh = !g_sync.on;
    if (fresh)
    {
        g_sync.on = true;
        g_picker.on = false;
        g_errorsPopup.on = false;
        g_edit.on = false;
        g_seen.erase("sync");
    }
    /* over the game (again, when it was hidden while downloading), or in the open window */
    if (!g_windowShown) ShowSyncAlone(true);
    else if (fresh) Sound(sounds::OPEN);
}

const char* SyncActionText(uint32_t action)
{
    switch (action)
    {
    case HF_SYNC_MOD_HAVE: return "you have it";
    case HF_SYNC_MOD_DOWNLOAD: return "downloads";
    case HF_SYNC_MOD_MANUAL: return "install it yourself";
    case HF_SYNC_MOD_BLOCKED: return "blocked";
    case HF_SYNC_MOD_UPDATE: return "needs another HellForge";
    case HF_SYNC_MOD_TRUST: return "needs your trust";
    default: return "off for this session";
    }
}

ImU32 SyncActionColour(uint32_t action)
{
    return action == HF_SYNC_MOD_HAVE ? OK : action == HF_SYNC_MOD_DOWNLOAD ? GOLD : action == HF_SYNC_MOD_MANUAL || action == HF_SYNC_MOD_TRUST ? PENDING :
        action == HF_SYNC_MOD_OFF ? FAINT : ERROR_;
}

/* A row's BLOCK: on mods that would come from the host or be installed by hand, while an offer stands, and on mods that
 * wait for the player's trust (the other way to answer them). */
bool SyncBlockable(const HfSyncMod& mod)
{
    uint32_t state = g_sync.state.state;
    bool offered = (mod.action == HF_SYNC_MOD_DOWNLOAD || mod.action == HF_SYNC_MOD_MANUAL) &&
        (state == HF_SYNC_OFFER || state == HF_SYNC_DOWNLOADING || state == HF_SYNC_READY);
    return !g_sync.sent && (offered || (mod.action == HF_SYNC_MOD_TRUST && state == HF_SYNC_BLOCKED));
}

/* A row's TRUST: a Steam friend's mod HellForge has not verified (the backend offers no other for trust). */
bool SyncTrustable(const HfSyncMod& mod) { return !g_sync.sent && mod.action == HF_SYNC_MOD_TRUST && g_sync.state.state == HF_SYNC_BLOCKED; }

/* A row's GET IT: a mod the player may install themselves, with an address to get it from (the loader keeps only https://
 * ones, and the backend opens only those). */
bool SyncGettable(const HfSyncMod& mod)
{
    size_t size = sizeof mod.url;
    return (mod.action == HF_SYNC_MOD_MANUAL || mod.action == HF_SYNC_MOD_TRUST) && mod.url[0] && strnlen(mod.url, size) < size &&
        !strncmp(mod.url, "https://", 8);
}

/* The TRUST warning for a row: shown instead of the list until answered; its button waits SYNC_ARM seconds, and only a
 * click on it trusts (no key does). */
void AskTrust(int i)
{
    if (i < 0 || i >= (int)g_sync.mods.size() || !SyncTrustable(g_sync.mods[i])) return;
    g_sync.trust = i;
    g_sync.trustAt = ImGui::GetTime();
    g_host.api->log(g_host.mod, "editor: mod sync: the trust warning for %s shows", g_sync.mods[i].id);
    Sound(sounds::OPEN);
}
bool TrustArmed() { return SyncArmed() && ImGui::GetTime() - g_sync.trustAt >= SYNC_ARM; }

/* Trusting this mod leaves nothing else in the way: the backend starts the download (TRUST AND DOWNLOAD). */
bool TrustDownloads(int i)
{
    for (int j = 0; j < (int)g_sync.mods.size(); j++)
    {
        uint32_t action = g_sync.mods[j].action;
        if (j != i && (action == HF_SYNC_MOD_TRUST || action == HF_SYNC_MOD_MANUAL || action == HF_SYNC_MOD_BLOCKED || action == HF_SYNC_MOD_UPDATE)) return false;
    }
    return true;
}

/* The dialog's first button and its answer, by state ("" none). */
const char* SyncPrimary(uint32_t* answer)
{
    const HfSyncState& s = g_sync.state;
    *answer = s.state == HF_SYNC_OFFER ? (s.bytes > 0 ? HF_SYNC_ACCEPT : HF_SYNC_RESTART) : s.state == HF_SYNC_READY ? HF_SYNC_RESTART :
        s.state == HF_SYNC_BLOCKED || s.state == HF_SYNC_FAILED ? HF_SYNC_DECLINE : 0;
    if (s.state == HF_SYNC_OFFER) return s.bytes > 0 ? "DOWNLOAD AND RESTART" : "RESTART TO JOIN";
    if (s.state == HF_SYNC_READY) return "RESTART TO JOIN";
    if (s.state == HF_SYNC_BLOCKED || s.state == HF_SYNC_FAILED) return "OK";
    return "";
}

void SyncKeys()
{
    int n = (int)g_sync.mods.size();
    /* the TRUST warning: Esc goes back to the list; nothing but a click on its button trusts */
    if (g_sync.trust >= 0)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { g_sync.trust = -1; Sound(sounds::BACK); }
        return;
    }
    if (Pressed(ImGuiKey_UpArrow)) g_sync.index = std::max(0, g_sync.index - 1);
    if (Pressed(ImGuiKey_DownArrow)) g_sync.index = std::min(std::max(0, n - 1), g_sync.index + 1);
    bool armed = SyncArmed();
    if (armed && ImGui::IsKeyPressed(ImGuiKey_B, false) && g_sync.index < n && SyncBlockable(g_sync.mods[g_sync.index]))
        AnswerSync(HF_SYNC_BLOCK, g_sync.mods[g_sync.index].id);
    if (armed && ImGui::IsKeyPressed(ImGuiKey_T, false) && g_sync.index < n && SyncTrustable(g_sync.mods[g_sync.index])) { AskTrust(g_sync.index); return; }
    if (armed && ImGui::IsKeyPressed(ImGuiKey_G, false) && g_sync.index < n && SyncGettable(g_sync.mods[g_sync.index]))
        AnswerSync(HF_SYNC_OPEN, g_sync.mods[g_sync.index].id);
    uint32_t answer;
    const char* primary = SyncPrimary(&answer);
    if (armed && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && primary[0] && !g_sync.sent)
        AnswerSync(answer);
    if (!ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return;
    if (g_windowShown) g_host.close(); /* Closed() answers */
    else if (g_sync.sent || g_sync.state.state == HF_SYNC_DOWNLOADING) { ShowSyncAlone(false); Sound(sounds::BACK); } /* it goes on */
    else AnswerSync(HF_SYNC_DECLINE);
}

/* The TRUST warning, in the prompt's place: what trusting a mod HellForge has not checked means, in plain words. Its
 * button waits SYNC_ARM seconds and answers only to a click; CANCEL and Esc go back to the list. */
void DrawTrust(const HfSyncMod& mod)
{
    const HfSyncState& s = g_sync.state;
    std::string name = std::string(mod.name[0] ? mod.name : mod.id) + " " + mod.version, friendName = s.host[0] ? s.host : "the host";
    std::vector<std::string> lines;
    float w = std::min(760.0f, PANEL_W - 60);
    std::string warning = mod.kind == HF_SYNC_CLASS_RAW
        ? name + " carries game data HellForge cannot check, which goes straight to the game. Data like this can break your game, or "
          "through a flaw in the game run code on your PC that can do anything your Windows account can."
        : name + " runs its own code on your PC. A mod like this can do anything your Windows account can: read and change your "
          "files, or install other programs.";
    warning += " Only trust it if you trust " + friendName + " and know where the mod comes from.";
    for (std::string& part : Wrapped(warning, w - 56, SZ_LABEL)) lines.push_back(std::move(part));
    size_t lead = lines.size();
    for (std::string& part : Wrapped("HellForge still checks that every file you download is exactly the one " + friendName + " has. You can take "
             "the trust back on the launcher's Mods page (MOD SYNC).", w - 56, SZ_SMALL))
        lines.push_back(std::move(part));
    /* at least 420 units high, so its buttons stay put whatever the text (overlay_test clicks them) */
    float h = std::min(g_panelH - 80, std::max(420.0f, 196 + 26.0f * (float)lines.size())), x = (PANEL_W - w) / 2, y = (g_panelH - h) / 2;
    g_cv.Block();
    g_cv.Rect(x, y, w, h, PANEL_ALT, 9);
    g_cv.Frame(x, y, w, h, LINE_STRONG, 10);
    g_cv.Rect(x, y, w, 3, ERROR_, 10);
    float tw = g_cv.Text("TRUST THIS MOD?", x + 24, y + 30, T(18, ERROR_, ui::TITLE, ui::LEFT, 0, 11));
    g_cv.Text(std::string("from ") + friendName + "'s lobby", x + 36 + tw, y + 31, T(SZ_LABEL, DIM, ui::BODY, ui::LEFT, w - tw - 80, 11));
    float ly = y + 70;
    float cx = x + 24;
    if (mod.kind == HF_SYNC_CLASS_CODE) cx += Chip("CODE", cx, ly, INVERSE, LUA, false, 11) + 8;
    else if (mod.kind == HF_SYNC_CLASS_RAW) cx += Chip("RAW DATA", cx, ly, INVERSE, LUA, false, 11) + 8;
    g_cv.Text(name, cx, ly, T(SZ_LABEL, TEXT, ui::BODY, ui::LEFT, x + w - 24 - cx, 11));
    g_cv.Rect(x + 12, ly + 22, w - 24, 1, LINE, 10);
    for (size_t i = 0; i < lines.size(); i++)
        g_cv.Text(lines[i], x + 28, ly + 50 + 26.0f * (float)i + (i >= lead ? 8.0f : 0), T(i < lead ? SZ_LABEL : SZ_SMALL, i < lead ? TEXT : DIM, ui::BODY, ui::LEFT, w - 56, 11));
    bool armed = TrustArmed();
    const char* label = TrustDownloads(g_sync.trust) ? "TRUST AND DOWNLOAD" : "TRUST";
    float bx = x + w - 24 - 150;
    if (Button("CANCEL", bx, y + h - 66, 150, 30, NORMAL, ui::Id("synctrustno"), 11)) { g_sync.trust = -1; Sound(sounds::BACK); return; }
    float pw = std::max(150.0f, g_cv.Measure(label, SZ_TAB, ui::TITLE) + 40);
    bx -= 12 + pw;
    if (Button(label, bx, y + h - 66, pw, 30, armed ? DANGER : DISABLED, ui::Id("synctrustyes"), 11) && armed)
    {
        g_host.api->log(g_host.mod, "editor: mod sync: the player trusts %s", mod.id);
        AnswerSync(HF_SYNC_TRUST, mod.id);
        return;
    }
    g_cv.Text("Click " + std::string(label) + " to trust it   Esc back to the list", x + 24, y + h - 20, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, 0, 11));
}

void DrawSync()
{
    if (!g_sync.on) return;
    const HfSyncState& s = g_sync.state;
    int n = (int)g_sync.mods.size();
    bool bad = s.state == HF_SYNC_BLOCKED || s.state == HF_SYNC_FAILED;
    if (g_sync.alone) g_cv.Rect(-g_cv.ox, -g_cv.oy, g_cv.width, g_cv.height, C(0, 0, 0, 150), 8); /* the game, dimmed */
    else g_cv.Rect(0, 0, PANEL_W, g_panelH, C(0, 0, 0, 120), 8);
    if (g_sync.trust >= 0 && g_sync.trust < n) { DrawTrust(g_sync.mods[g_sync.trust]); return; }
    float w = std::min(900.0f, PANEL_W - 60), h = std::clamp(214.0f + 40.0f * (float)std::max(1, n), 320.0f, g_panelH - 80);
    float x = (PANEL_W - w) / 2, y = (g_panelH - h) / 2;
    g_cv.Block();
    g_cv.Rect(x, y, w, h, PANEL_ALT, 9);
    g_cv.Frame(x, y, w, h, LINE_STRONG, 10);
    g_cv.Rect(x, y, w, 3, bad ? ERROR_ : GOLD, 10);
    float tw = g_cv.Text("MOD SYNC", x + 24, y + 30, T(18, bad ? ERROR_ : GOLD, ui::TITLE, ui::LEFT, 0, 11));
    g_cv.Text(std::string(s.host[0] ? s.host : "The host") + "'s lobby", x + 36 + tw, y + 31, T(SZ_LABEL, TEXT, ui::BODY, ui::LEFT, w - tw - 80, 11));
    std::string say = s.message[0] ? s.message :
        s.state == HF_SYNC_OFFER ? "The host plays with gameplay mods you don't have. Get them and restart with the host's mods: their invitation waits on your ship." :
        s.state == HF_SYNC_DOWNLOADING ? "Downloading the host's mods; each file is checked before it is kept." :
        s.state == HF_SYNC_READY ? "Every mod is here. Restart with the host's mods: their invitation waits on your ship." :
        s.state == HF_SYNC_BLOCKED ? "You cannot join this lobby with these mods." : "The download failed.";
    g_cv.Text(say, x + 24, y + 58, T(SZ_SMALL, bad ? ERROR_ : DIM, ui::BODY, ui::LEFT, w - 48, 11));
    float hy = y + 80;
    g_cv.Rect(x + 1, hy, w - 2, 22, HEADER, 10);
    g_cv.Text("MOD", x + 24, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::LEFT, 0, 11));
    g_cv.Text("WHAT HAPPENS", x + w - 460, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::LEFT, 0, 11));
    bool gettable = false, trustable = false;
    for (const HfSyncMod& mod : g_sync.mods) { gettable = gettable || SyncGettable(mod); trustable = trustable || SyncTrustable(mod); }
    ListOpts o;
    o.x = x + 2; o.y = hy + 26; o.w = w - 4; o.h = y + h - hy - 26 - 108; o.rowH = 40; o.count = n; o.selected = g_sync.index;
    o.key = "sync"; o.focused = true; o.layer = 12;
    o.click = [&](int i) { g_sync.index = i; };
    o.draw = [&](int i, float rx, float ry, float rw, float rh, bool hovered) {
        const HfSyncMod& mod = g_sync.mods[i];
        bool selected = i == g_sync.index;
        if (selected) { g_cv.Rect(rx + 10, ry + 2, rw - 20, rh - 4, SELECT, 10); g_cv.Rect(rx + 10, ry + 2, 3, rh - 4, GOLD, 11); }
        else if (hovered) g_cv.Rect(rx + 10, ry + 2, rw - 20, rh - 4, HOVER, 10);
        float lx = rx + 22, right = rx + rw - 460;
        if (mod.kind == HF_SYNC_CLASS_CODE) lx += Chip("CODE", lx, ry + rh / 2, INVERSE, LUA, false, 12) + 8;
        else if (mod.kind == HF_SYNC_CLASS_RAW) lx += Chip("RAW DATA", lx, ry + rh / 2, INVERSE, LUA, false, 12) + 8;
        float nw = g_cv.Text(mod.name[0] ? mod.name : mod.id, lx, ry + rh / 2, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, (right - lx) * 0.7f, 12));
        g_cv.Text(mod.version, lx + nw + 10, ry + rh / 2, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, right - lx - nw - 20, 12));
        std::string what = SyncActionText(mod.action);
        if (mod.action == HF_SYNC_MOD_DOWNLOAD) what += " (" + SizeText(mod.bytes) + ")";
        /* the action's text up to the row's buttons (BLOCK, TRUST, GET IT from the right) */
        float buttons = (SyncBlockable(mod) ? 86.0f : 0) + (SyncTrustable(mod) ? 86.0f : 0) + (SyncGettable(mod) ? 86.0f : 0);
        g_cv.Text(what, right + 2, ry + rh / 2, T(SZ_SMALL, SyncActionColour(mod.action), ui::BODY, ui::LEFT, std::min(340.0f, 434.0f - buttons), 12));
    };
    o.after = [&](int i, float rx, float ry, float rw, float rh) {
        const HfSyncMod& mod = g_sync.mods[i];
        float bx = rx + rw - 102, by = ry + (rh - 24) / 2;
        if (SyncBlockable(mod))
        {
            if (Button("BLOCK", bx, by, 78, 24, DANGER, ui::Id("syncblock", (uint64_t)i), 12) && SyncArmed()) { AnswerSync(HF_SYNC_BLOCK, mod.id); return; }
            bx -= 86;
        }
        if (SyncTrustable(mod))
        {
            if (Button("TRUST", bx, by, 78, 24, SyncArmed() ? NORMAL : DISABLED, ui::Id("synctrust", (uint64_t)i), 12) && SyncArmed()) { AskTrust(i); return; }
            bx -= 86;
        }
        if (SyncGettable(mod) && Button("GET IT", bx, by, 78, 24, SyncArmed() ? NORMAL : DISABLED, ui::Id("syncget", (uint64_t)i), 12) && SyncArmed())
            AnswerSync(HF_SYNC_OPEN, mod.id);
    };
    List(o);
    if (n == 0) g_cv.Text("The host's mods are not listed.", x + 28, hy + 48, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, 0, 12));
    float fy = y + h - 104;
    g_cv.Rect(x + 12, fy, w - 24, 1, LINE, 10);
    if (s.state == HF_SYNC_DOWNLOADING)
    {
        float bw = w - 48, filled = s.bytes > 0 ? bw * (float)std::min(1.0, (double)s.done / (double)s.bytes) : 0;
        g_cv.Rect(x + 24, fy + 16, bw, 10, BOX, 11);
        g_cv.Rect(x + 24, fy + 16, filled, 10, GOLD, 12);
        g_cv.Text(SizeText(s.done) + " of " + SizeText(s.bytes), x + 24, fy + 40, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, 0, 11));
    }
    else
        g_cv.Text(s.bytes > 0 ? SizeText(s.bytes) + " to download from the host" : "Nothing to download", x + 24, fy + 22,
            T(SZ_SMALL, s.bytes > 0 ? TEXT : FAINT, ui::BODY, ui::LEFT, 0, 11));
    uint32_t answer;
    const char* primary = SyncPrimary(&answer);
    float bx = x + w - 24;
    const char* second = s.state == HF_SYNC_OFFER || s.state == HF_SYNC_DOWNLOADING ? "CANCEL" : s.state == HF_SYNC_READY ? "LATER" : "";
    if (second[0])
    {
        bx -= 150;
        if (Button(second, bx, y + h - 66, 150, 30, NORMAL, ui::Id("syncsecond"), 11) && SyncArmed()) AnswerSync(HF_SYNC_DECLINE);
        bx -= 12;
    }
    if (primary[0])
    {
        float pw = std::max(150.0f, g_cv.Measure(primary, SZ_TAB, ui::TITLE) + 40);
        bx -= pw;
        if (Button(g_sync.sent ? (answer == HF_SYNC_RESTART ? "RESTARTING..." : "STARTING...") : primary, bx, y + h - 66, pw, 30,
                g_sync.sent || !SyncArmed() ? DISABLED : bad ? NORMAL : PRIMARY, ui::Id("syncprimary"), 11) && SyncArmed())
            AnswerSync(answer);
    }
    /* beside the buttons: what GET IT does, and that installing a mod by hand takes a new session */
    if (gettable && s.state != HF_SYNC_DOWNLOADING)
    {
        auto lines = Wrapped("GET IT opens the mod's page in your browser. To install a mod yourself, close the game, add the mod "
            "on the Mods page, then start HellForge again and join.", bx - x - 48, SZ_SMALL);
        for (size_t i = 0; i < lines.size() && i < 2; i++)
            g_cv.Text(lines[i], x + 24, fy + 46 + 18.0f * (float)i, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, bx - x - 36, 11));
    }
    std::string keys = "\xE2\x86\x91\xE2\x86\x93 select   B block the mod   ";
    if (trustable) keys += "T trust   ";
    if (gettable) keys += "G get it   ";
    if (primary[0]) keys += std::string("Enter ") + primary + "   ";
    keys += s.state == HF_SYNC_DOWNLOADING ? "Esc hide (the download goes on)" : "Esc close (no)";
    g_cv.Text(keys, x + 24, y + h - 20, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, 0, 11));
}

void NextView(int delta)
{
    g_view = (View)(((int)g_view + delta + VIEW_COUNT) % VIEW_COUNT);
    g_edit.on = false;
    if (g_view == PRESETS) ScanPresets();
    if (g_view == LOGS) LogRead(true);
    if (g_view == MODS) ScanMods(true);
}

std::vector<std::string> Chars()
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

void DropLast(std::string& text)
{
    if (text.empty()) return;
    size_t n = text.size() - 1;
    while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    text.resize(n);
}

void HandleKeys()
{
    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    auto chars = Chars();
    if (g_sync.on) { SyncKeys(); return; }
    if (g_errorsPopup.on)
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
        return;
    }
    if (g_picker.on)
    {
        bool refilter = false;
        for (auto& c : chars) if (g_picker.filter.size() < 32 && !ctrl) { g_picker.filter += c; refilter = true; }
        if (Pressed(ImGuiKey_Backspace) && !g_picker.filter.empty()) { DropLast(g_picker.filter); refilter = true; }
        if (refilter)
        {
            double was = PickerHas(g_picker.index) ? g_picker.entries[g_picker.index].value : ViewOf(g_picker.row).value;
            PickerFill();
            PickerSelect(was);
            g_scroll["picker"] = 0;
        }
        if (Pressed(ImGuiKey_UpArrow)) g_picker.index = PickerStep(g_picker.index, -1);
        if (Pressed(ImGuiKey_DownArrow)) g_picker.index = PickerStep(g_picker.index, 1);
        for (int k = 0; k < 8; k++)
        {
            if (Pressed(ImGuiKey_PageUp)) g_picker.index = PickerStep(g_picker.index, -1);
            if (Pressed(ImGuiKey_PageDown)) g_picker.index = PickerStep(g_picker.index, 1);
        }
        if (Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter)) PickerChoose(g_picker.index);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || Pressed(ImGuiKey_Delete)) { g_picker.on = false; Sound(sounds::BACK); }
        return;
    }
    if (g_edit.on)
    {
        for (auto& c : chars)
            if (strchr("0123456789.,-+eE", c[0]) && g_edit.buffer.size() < 24)
            {
                if (g_edit.fresh) { g_edit.buffer.clear(); g_edit.fresh = false; }
                g_edit.buffer += c;
            }
        if (Pressed(ImGuiKey_Backspace)) { if (g_edit.fresh) g_edit.buffer.clear(); else DropLast(g_edit.buffer); g_edit.fresh = false; }
        if (Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter) || Pressed(ImGuiKey_Tab)) CommitEdit();
        else if (Pressed(ImGuiKey_UpArrow)) { CommitEdit(); g_field = std::max(0, g_field - 1); }
        else if (Pressed(ImGuiKey_DownArrow)) { CommitEdit(); g_field++; }
        else if (Pressed(ImGuiKey_Delete) || Pressed(ImGuiKey_Escape)) g_edit.on = false;
        return;
    }
    if (g_view == BROWSE && g_search.active)
    {
        for (auto& c : chars) if (g_search.text.size() < 40) { g_search.text += c; g_obj = 0; g_field = 0; }
        if (Pressed(ImGuiKey_Backspace)) { DropLast(g_search.text); g_obj = 0; }
        if (Pressed(ImGuiKey_Delete)) { g_search.text.clear(); g_obj = 0; }
        if (Pressed(ImGuiKey_Tab) || Pressed(ImGuiKey_Enter)) { g_search.active = false; g_focus = OBJECTS; }
        if (Pressed(ImGuiKey_Escape)) { g_search.active = false; g_search.text.clear(); }
        if (Pressed(ImGuiKey_DownArrow)) { auto n = (int)Objects().size(); if (n) SelectObject(std::min(n - 1, g_obj + 1)); }
        if (Pressed(ImGuiKey_UpArrow)) SelectObject(std::max(0, g_obj - 1));
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        if (g_view == SEARCH && !g_gsearch.text.empty()) { g_gsearch.text.clear(); g_gsearch.index = 0; }
        else g_host.close(); /* Closed() plays the closing sound */
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) { Apply(); return; }
    if ((Pressed(ImGuiKey_Tab) && shift) || ImGui::IsKeyPressed(ImGuiKey_F6, false)) { NextView(1); Sound(sounds::CLICK); return; }
    if (ctrl && Pressed(ImGuiKey_F)) { g_view = BROWSE; g_search.active = true; g_focus = OBJECTS; return; }
    if (ctrl && Pressed(ImGuiKey_E)) { g_view = CHANGES; return; }

    if (g_view == SEARCH)
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
        return;
    }
    if (g_view == CHANGES)
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
        return;
    }
    if (g_view == MODS)
    {
        auto entries = ModEntries();
        auto rows = ModRows();
        if (Pressed(ImGuiKey_UpArrow) || Pressed(ImGuiKey_DownArrow))
        {
            int dir = Pressed(ImGuiKey_UpArrow) ? -1 : 1;
            for (int i = g_modEntry + dir; i >= 0 && i < (int)entries.size(); i += dir)
                if (entries[i].mod >= 0) { g_modEntry = i; g_modRow = 0; g_scroll["modrows"] = 0; break; }
        }
        if (Pressed(ImGuiKey_PageUp)) g_modRow = std::max(0, g_modRow - 1);
        if (Pressed(ImGuiKey_PageDown)) g_modRow = std::min(std::max(0, (int)rows.size() - 1), g_modRow + 1);
        if ((Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter)) && g_modRow < (int)rows.size()) { Reveal(rows[g_modRow]); Sound(sounds::OPEN); }
        if (Pressed(ImGuiKey_F5)) ScanMods(true);
        return;
    }
    if (g_view == LOGS)
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
        return;
    }
    if (g_view == PRESETS)
    {
        int n = std::max(1, (int)g_presets.size());
        if (Pressed(ImGuiKey_UpArrow)) g_preset = std::max(0, g_preset - 1);
        if (Pressed(ImGuiKey_DownArrow)) g_preset = std::min(n - 1, g_preset + 1);
        if ((Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter)) && g_preset < (int)g_presets.size() && g_presets[g_preset].error.empty())
            ImportPreset(g_presets[g_preset]);
        return;
    }
    if (g_view == SETTINGS)
    {
        int index = 0;
        for (int i = 0; i < UI_SCALE_COUNT; i++) if (UI_SCALES[i] == g_uiScale) index = i;
        if (Pressed(ImGuiKey_LeftArrow) && index > 0) { g_uiScale = UI_SCALES[index - 1]; SaveSettings(); Sound(sounds::CLICK); }
        if (Pressed(ImGuiKey_RightArrow) && index + 1 < UI_SCALE_COUNT) { g_uiScale = UI_SCALES[index + 1]; SaveSettings(); Sound(sounds::CLICK); }
        return;
    }

    /* BROWSE */
    if (Pressed(ImGuiKey_Tab)) { g_focus = g_focus == CATEGORIES ? OBJECTS : g_focus == OBJECTS ? FIELDS : CATEGORIES; return; }
    if (g_focus == CATEGORIES)
    {
        if (Pressed(ImGuiKey_UpArrow)) MoveCategory(-1);
        if (Pressed(ImGuiKey_DownArrow)) MoveCategory(1);
        if (Pressed(ImGuiKey_RightArrow) || Pressed(ImGuiKey_Enter)) { g_focus = OBJECTS; if (Pressed(ImGuiKey_Enter)) Sound(sounds::CLICK); }
        return;
    }
    if (g_focus == OBJECTS)
    {
        int n = (int)Objects().size();
        if (Pressed(ImGuiKey_UpArrow)) SelectObject(std::max(0, g_obj - 1));
        if (Pressed(ImGuiKey_DownArrow) && n) SelectObject(std::min(n - 1, g_obj + 1));
        if (Pressed(ImGuiKey_PageUp)) SelectObject(std::max(0, g_obj - 10));
        if (Pressed(ImGuiKey_PageDown) && n) SelectObject(std::min(n - 1, g_obj + 10));
        if (Pressed(ImGuiKey_LeftArrow)) g_focus = CATEGORIES;
        if ((Pressed(ImGuiKey_RightArrow) || Pressed(ImGuiKey_Enter)) && n) { g_focus = FIELDS; if (Pressed(ImGuiKey_Enter)) Sound(sounds::CLICK); }
        for (auto& c : chars) if (isalnum((unsigned char)c[0])) { g_search.active = true; g_search.text = c; g_obj = 0; break; }
        return;
    }
    /* fields */
    Object* object = Current();
    auto items = Items(object);
    if (items.empty()) { if (Pressed(ImGuiKey_LeftArrow)) g_focus = OBJECTS; return; }
    g_field = std::clamp(g_field, 0, (int)items.size() - 1);
    if (ctrl && Pressed(ImGuiKey_UpArrow)) { SetPageOpen(object, false); return; }
    if (ctrl && Pressed(ImGuiKey_DownArrow)) { SetPageOpen(object, true); return; }
    if (Pressed(ImGuiKey_UpArrow)) g_field = std::max(0, g_field - 1);
    if (Pressed(ImGuiKey_DownArrow)) g_field = std::min((int)items.size() - 1, g_field + 1);
    if (Pressed(ImGuiKey_PageUp)) g_field = std::max(0, g_field - 10);
    if (Pressed(ImGuiKey_PageDown)) g_field = std::min((int)items.size() - 1, g_field + 10);
    const Item& item = items[std::clamp(g_field, 0, (int)items.size() - 1)];
    if (item.header)
    {
        if (Pressed(ImGuiKey_Enter)) { Toggle(item.key); Sound(sounds::CLICK); }
        if (Pressed(ImGuiKey_RightArrow)) Toggle(item.key, 1);
        if (Pressed(ImGuiKey_LeftArrow)) { if (item.open) Toggle(item.key, 0); else g_focus = OBJECTS; }
        return;
    }
    if (item.row == nullptr) { if (Pressed(ImGuiKey_LeftArrow)) g_focus = OBJECTS; return; } /* a note */
    if (Pressed(ImGuiKey_LeftArrow)) Nudge(item.row, -1, shift, ctrl);
    if (Pressed(ImGuiKey_RightArrow)) Nudge(item.row, 1, shift, ctrl);
    if (Pressed(ImGuiKey_Enter) || Pressed(ImGuiKey_KeypadEnter)) { BeginEdit(item.row, "", true); Sound(sounds::CLICK); }
    if (Pressed(ImGuiKey_Delete)) { Revert(item.row); Sound(sounds::CLICK); }
    /* digits start a value: everything typed this frame */
    std::string typed;
    for (auto& c : chars) if (strchr("0123456789.,-", c[0]) && (typed.size() || c[0] != ',')) typed += c;
    if (!typed.empty() && item.row->choice.empty() && item.row->pick.empty()) BeginEdit(item.row, typed, false);
}

/* ---- the header ---- */
void DrawHeader(int errors, int active)
{
    float h = HEADER_H;
    g_cv.Rect(0, 0, PANEL_W, h, HEADER, 1);
    g_cv.Rect(0, h - 1, PANEL_W, 1, LINE_STRONG, 2);
    const icons::Icon* logo = icons::Logo();
    if (logo != nullptr && icons::Texture()) g_cv.Image(icons::Texture(), 14, 9, 40, 40, logo->uv0, logo->uv1, IM_COL32_WHITE, 4);
    else Logo(18, 15, 28);
    float x = 62;
    x += g_cv.Text("HELLFORGE", x, h / 2, T(SZ_TITLE, TEXT, ui::TITLE));
    x += 8 + g_cv.Text("EDITOR", x + 8, h / 2, T(SZ_TITLE, BRAND, ui::TITLE));
    float tx = x + 24;
    for (int v = 0; v < VIEW_COUNT; v++)
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
        tx += w + 2;
    }
    float tabsEnd = tx + 8;
    uint64_t closeId = ui::Id("close");
    bool hovered = g_cv.Hovered() == closeId;
    g_cv.Rect(PANEL_W - 50, 12, 36, 34, hovered ? ERROR_SOFT : PANEL, 3);
    g_cv.Frame(PANEL_W - 50, 12, 36, 34, hovered ? ERROR_ : LINE_STRONG, 4);
    g_cv.Text("\xC3\x97", PANEL_W - 32, 29, T(24, hovered ? ERROR_ : DIM, ui::BODY, ui::CENTER, 0, 6));
    if (g_cv.Hit(PANEL_W - 50, 12, 36, 34, closeId)) g_host.close();
    float rx = PANEL_W - 62;
    auto status = [&](const std::string& text, ImU32 fg, ImU32 bg, uint64_t action) {
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
    };
    if (Locked()) status(std::string("READ-ONLY \xC2\xB7 HOST: ") + (g_session.host[0] ? g_session.host : "A FRIEND"), PENDING, PENDING_SOFT, 0);
    else if (g_session.role == HF_SESSION_HOST) status(g_lobbyStats ? "HOSTING \xC2\xB7 FRIENDS PLAY WITH YOUR STATS" : "HOSTING \xC2\xB7 EVERYONE KEEPS THEIR OWN", MOD, MOD_SOFT, 0);
    else if (g_session.role == HF_SESSION_JOINER) status(std::string("JOINED \xC2\xB7 ") + (g_session.host[0] ? g_session.host : "A FRIEND") + " \xC2\xB7 YOUR OWN STATS", MOD, MOD_SOFT, 0);
    if (errors > 0 && status(std::to_string(errors) + (errors == 1 ? " ERROR" : " ERRORS"), ERROR_, ERROR_SOFT, ui::Id("errors"))) OpenErrors();
    if (active > 0) status(std::to_string(active) + (Locked() ? " ACTIVE (HOST)" : " ACTIVE"), GOLD, GOLD_WASH, 0);
    if (PendingCount() > 0 && !Locked()) status(std::to_string(PendingCount()) + " PENDING", PENDING, PENDING_SOFT, 0);
}

/* ---- BROWSE ---- */
void DrawCategories(float y0, float h)
{
    g_cv.Rect(0, y0, CAT_W, h, PANEL_ALT, 1);
    g_cv.Rect(CAT_W - 1, y0, 1, h, LINE, 2);
    const MarkCache& marks = Markers();
    bool focused = g_focus == CATEGORIES;
    const float rh = 31, headH = 24, gap = 10;
    /* Taller than the panel (a short screen, a large UI scale): it scrolls, by the wheel or its scrollbar, and a category
     * chosen with the keys is kept in view. Only whole rows are drawn, as in a List. */
    float total = gap, selTop = 0;
    for (int i = 0; i < (int)g_cats.size(); i++)
    {
        if (g_cats[i].group) { total += (i > 0 ? gap : 0) + headH; continue; }
        if (i == g_cat) selTop = total;
        total += rh;
    }
    total += gap;
    float maxScroll = std::max(0.0f, total - h);
    static int seen = -1;
    if (seen != g_cat)
    {
        seen = g_cat;
        if (selTop - gap < g_catScroll) g_catScroll = selTop - gap;
        if (selTop + rh + gap > g_catScroll + h) g_catScroll = selTop + rh + gap - h;
    }
    float wheel = g_cv.Scroll(0, y0, CAT_W, h, ui::Id("cats", 7777));
    if (wheel != 0) g_catScroll -= wheel * rh * 3;
    float trackY = y0 + 2, trackH = h - 4, thumb = std::max(24.0f, trackH * std::min(1.0f, h / std::max(1.0f, total)));
    if (g_drag.key == "cats")
    {
        if (!ImGui::IsMouseDown(0)) g_drag.key.clear();
        else
        {
            float ux, uy;
            g_cv.ToUnits(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y, &ux, &uy);
            g_catScroll = std::clamp((uy - trackY - thumb / 2) / std::max(1.0f, trackH - thumb), 0.0f, 1.0f) * maxScroll;
        }
    }
    g_catScroll = std::clamp(g_catScroll, 0.0f, maxScroll);
    auto inside = [&](float y, float rowH) { return y >= y0 && y + rowH <= y0 + h; };
    float yy = gap - g_catScroll;
    for (int i = 0; i < (int)g_cats.size(); i++)
    {
        const CatItem& item = g_cats[i];
        if (item.group)
        {
            yy += i > 0 ? gap : 0;
            if (inside(y0 + yy, headH)) g_cv.Text(item.label, 18, y0 + yy + 11, T(SZ_HEADING, FAINT, ui::TITLE));
            yy += headH;
            continue;
        }
        float y = y0 + yy;
        yy += rh;
        if (!inside(y, rh)) continue;
        bool selected = i == g_cat;
        uint64_t id = ui::Id("cat", (uint64_t)i);
        const catalog::Nav& nav = g_catalog.navs[item.nav];
        Tone tone = ToneOf(nav.tone);
        if (selected)
        {
            g_cv.Rect(8, y, CAT_W - 16, rh, tone.colour ? tone.soft : focused ? SELECT : GOLD_WASH, 2);
            g_cv.Rect(8, y, 3, rh, tone.colour ? tone.colour : focused ? GOLD : GOLD_DIM, 3);
        }
        else if (g_cv.Hovered() == id) g_cv.Rect(8, y, CAT_W - 16, rh, HOVER, 2);
        if (tone.colour && !selected) g_cv.Rect(8, y + 9, 3, rh - 18, tone.colour, 3);
        float cw = g_cv.Text(std::to_string(g_catalog.Of(nav.id).size()), CAT_W - 20, y + rh / 2, T(SZ_SMALL, FAINT, ui::BODY, ui::RIGHT));
        /* an enemy faction's emblem left of its count, then the markers */
        float right = CAT_W - 28 - cw;
        const icons::Icon* emblem = FactionColour(nav.id) && icons::Texture() ? icons::Find(icons::FACTION, nav.id) : nullptr;
        if (emblem != nullptr)
        {
            const float size = 20;
            g_cv.Image(icons::Texture(), right - size, y + (rh - size) / 2, size, size, emblem->uv0, emblem->uv1, FactionColour(nav.id), 4);
            right -= size + 6;
        }
        auto found = marks.navs.find(nav.id);
        float mx = DrawMarks(found != marks.navs.end() ? found->second : Marks{}, right, y + rh / 2, 26);
        g_cv.Text(item.label, 22, y + rh / 2, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, std::min(CAT_W - 80, mx - 30)));
        if (g_cv.Hit(8, y, CAT_W - 16, rh, id)) { SelectCategory(i); g_focus = CATEGORIES; }
    }
    if (maxScroll > 0)
    {
        bool dragging = g_drag.key == "cats";
        g_cv.Rect(CAT_W - 6, trackY, 3, trackH, LINE, 3);
        g_cv.Rect(CAT_W - (dragging ? 8 : 6), trackY + (trackH - thumb) * (g_catScroll / maxScroll), dragging ? 5.0f : 3.0f, thumb,
            (focused || dragging) ? GOLD : LINE_STRONG, 4);
        if (g_cv.Hit(CAT_W - 16, y0, 14, h, ui::Id("cats", 8888)))
        {
            g_drag.key = "cats"; g_drag.trackY = trackY; g_drag.trackH = trackH; g_drag.thumb = thumb;
        }
    }
}

void DrawObjects(float y0, float h)
{
    float x0 = CAT_W;
    g_cv.Rect(x0, y0, OBJ_W, h, PANEL, 1);
    g_cv.Rect(x0 + OBJ_W - 1, y0, 1, h, LINE, 2);
    float sy = y0 + 10;
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
    /* the weapon-group filter, wrapped onto as many rows as it needs */
    float ly = sy + 44;
    const catalog::Nav* nav = NavOf(g_cat);
    if (nav != nullptr && nav->hasFilters)
    {
        static const std::map<std::string, std::string> shortLabels = {{"Assault Rifle", "Assault"}, {"Marksman Rifle", "Marksman"},
            {"Submachine Gun", "SMG"}, {"Energy-Based", "Energy"}};
        std::string current = g_group.count(nav->id) ? g_group[nav->id] : "All";
        float cx = x0 + 12, cy = sy + 40;
        std::vector<std::string> groups = {"All"};
        groups.insert(groups.end(), nav->filters.begin(), nav->filters.end());
        for (auto& g : groups)
        {
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
        ly = cy + 32;
    }
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
        if (selected)
        {
            g_cv.Rect(x + 8, y + 2, w - 16, rh - 4, tone.colour ? tone.soft : focused ? SELECT : GOLD_WASH, 2);
            g_cv.Rect(x + 8, y + 2, 3, rh - 4, tone.colour ? tone.colour : focused ? GOLD : GOLD_DIM, 3);
        }
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
    if (Button(anyOpen ? "COLLAPSE ALL" : "EXPAND ALL", x0 + w - bw - 14, y0 + 12, bw, 28, NORMAL, ui::Id("pageopen"))) SetPageOpen(object, !anyOpen);
    float rw = 132;
    bool changed = !ItemChanges(object).empty();
    if (Button("RESET ITEM", x0 + w - bw - rw - 24, y0 + 12, rw, 28, changed && !Locked() ? DANGER : DISABLED, ui::Id("resetitem")) && changed) ResetItem(object);
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
        bool selected = i == g_field;
        float cy = y + rowH / 2;
        if (item.header)
        {
            float indent = FX_LABEL + item.depth * INDENT;
            if (item.depth == 0) g_cv.Rect(x + 6, y + 2, lw - 12, rowH - 4, HEADER, 1);
            if (selected) { g_cv.Rect(x + 6, y + 1, lw - 12, rowH - 2, focused ? SELECT_HEADER : WASH_HEADER, 2); g_cv.Rect(x + 6, y + 1, 3, rowH - 2, focused ? GOLD : GOLD_DIM, 3); }
            else if (hovered) g_cv.Rect(x + 6, y + 1, lw - 12, rowH - 2, item.depth == 0 ? HOVER_HEADER : HOVER, 2); /* a dark header: only a hint */
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
            }
            else
            {
                g_cv.Text(item.title, x + indent + 20, cy, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, lw - indent - 150 - reserve));
                g_cv.Text(std::to_string(item.count) + " fields", right, cy, T(SZ_TINY, FAINT, ui::BODY, ui::RIGHT));
            }
            return;
        }
        const Row* row = item.row;
        if (selected) { g_cv.Rect(x + 6, y + 1, lw - 12, rowH - 2, focused ? SELECT : GOLD_WASH, 2); g_cv.Rect(x + 6, y + 1, 3, rowH - 2, focused ? GOLD : GOLD_DIM, 3); }
        else if (hovered) g_cv.Rect(x + 6, y + 1, lw - 12, rowH - 2, HOVER, 2);
        float lx = FX_LABEL + (item.depth > 0 ? (item.depth + 1) * INDENT : 0);
        if (row == nullptr) { g_cv.Text(item.note, x + lx, cy, T(SZ_SMALL, item.detail ? FAINT : PENDING, ui::BODY, ui::LEFT, lw - lx - 24)); return; }
        /* a roster row's weight at one difficulty: that difficulty's icon in front of its label (the HD2R Editor's); one
         * without an icon (level 11 without its mod) keeps level 1's room, so the labels line up */
        if (row->difficulty > 0 && icons::Texture())
        {
            const icons::Icon* badge = icons::Find(icons::DIFFICULTY, std::to_string(row->difficulty));
            const icons::Icon* room = badge != nullptr ? badge : icons::Find(icons::DIFFICULTY, "1");
            if (room != nullptr)
            {
                float bh = rowH - 12, bw = bh * (float)room->width / (float)room->height;
                if (badge != nullptr) g_cv.Image(icons::Texture(), x + lx, cy - bh / 2, bw, bh, badge->uv0, badge->uv1, IM_COL32_WHITE, 4);
                lx += bw + 8;
            }
        }
        bool locked = !LockOf(row).empty(), blocked = !SlotBlock(row).empty();
        RowView v = ViewOf(row);
        std::string def = v.hasBase ? Format(row, v.base) : "\xE2\x80\x94";
        float dw = std::min(84.0f, g_cv.Measure(def, SZ_SMALL));
        float labelMax = FX_DEFAULT_RIGHT - std::max(dw, 24.0f) - 12 - lx;
        g_cv.Text(RowLabel(item, labelMax), x + lx, cy, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, labelMax));
        g_cv.Text(def, x + FX_DEFAULT_RIGHT, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::RIGHT, 84));
        bool editing = g_edit.on && g_edit.row == row;
        bool error = g_errors.count(row) > 0;
        float bx = x + FX_BOX, bw2 = FX_BOX_W;
        ImU32 edge = BOX_EDGE, colour = TEXT;
        if (editing) edge = BOX_FOCUS;
        else if (error && v.source != STAGED) edge = ERROR_;
        else if (v.source == STAGED) { edge = PENDING; colour = PENDING; }
        else if (v.source == EDITED) { edge = GOLD_DIM; colour = GOLD; }
        else if (v.source == MODDED) colour = IsLuaHolder(v.holder) ? LUA : MOD;
        if ((locked || blocked) && v.source != STAGED) colour = FAINT;
        g_cv.Rect(bx, y + 4, bw2, rowH - 8, BOX, 3);
        g_cv.Frame(bx, y + 4, bw2, rowH - 8, edge, 4);
        if (editing)
        {
            float tw = g_cv.Text(g_edit.buffer, bx + bw2 - 10, cy, T(SZ_LABEL, TEXT, ui::BODY, ui::RIGHT, bw2 - 16));
            if (g_edit.fresh && !g_edit.buffer.empty()) g_cv.Rect(bx + bw2 - 12 - tw, y + 7, tw + 4, rowH - 14, GOLD_SOFT, 4);
            if ((int)(g_time * 2.2f) % 2 == 0) g_cv.Rect(bx + bw2 - 8, y + 8, 2, rowH - 16, GOLD, 6);
        }
        else g_cv.Text(v.has ? Format(row, v.value) : "\xE2\x80\x94", bx + bw2 - 10, cy, T(SZ_LABEL, v.has ? colour : FAINT, ui::BODY, ui::RIGHT, bw2 - 16));
        if (!row->unit.empty()) g_cv.Text(row->unit, x + FX_UNIT, cy, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, 56));
        float bxr = x + lw + FX_BADGE_RIGHT;
        if (error) Chip("ERROR", bxr, cy, ERROR_, ERROR_SOFT, true);
        else if (locked) Chip("LOCKED", bxr, cy, FAINT, LINE, true);
        else if (blocked && v.source != STAGED) Chip("IN ORDER", bxr, cy, FAINT, LINE, true);
        else if (v.source == STAGED) Chip(v.reset ? "PENDING RESET" : "PENDING", bxr, cy, PENDING, PENDING_SOFT, true);
        else if (v.source == EDITED) Chip(row->Live() ? "EDITED" : "NEXT BOOT", bxr, cy, GOLD, GOLD_WASH, true);
        else if (v.source == MODDED && IsLuaHolder(v.holder)) Chip("LUA", bxr, cy, LUA, LUA_SOFT, true);
        else if (v.source == MODDED) Chip("MOD", bxr, cy, MOD, MOD_SOFT, true);
    };
    List(o);
}

/* ---- the status bar and the footer ---- */
void DrawStatus(float y, float h)
{
    g_cv.Rect(0, y, PANEL_W, h, HEADER, 1);
    g_cv.Rect(0, y, PANEL_W, 1, LINE, 2);
    float cy = y + h / 2;
    if (g_toast.on && g_time - g_toast.time > 3.2f) g_toast.on = false;
    if (g_toast.on) g_cv.Text(g_toast.text, PANEL_W - 18, cy, T(SZ_SMALL, g_toast.colour, ui::BODY, ui::RIGHT, 520));
    float maxw = g_toast.on ? PANEL_W - 580 : PANEL_W - 40;
    if (!g_catalogOk) { g_cv.Text("The editor's catalogue could not be read: " + g_catalogError, 18, cy, T(SZ_SMALL, ERROR_, ui::BODY, ui::LEFT, maxw)); return; }
    if (g_view == BROWSE)
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
        if (row != nullptr && !row->pick.empty())
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
            return;
        }
        if (row == nullptr) { g_cv.Text("Pick a field: \xE2\x86\x91\xE2\x86\x93 to move, \xE2\x86\x92 to go deeper, Tab to switch panes", 18, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, maxw)); return; }
        auto error = g_errors.find(row);
        if (error != g_errors.end()) { g_cv.Text("Error: " + error->second, 18, cy, T(SZ_SMALL, ERROR_, ui::BODY, ui::LEFT, maxw)); return; }
        float x = 18;
        if (!row->choice.empty())
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
            g_cv.Text(text, x, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, maxw));
            return;
        }
        std::string range;
        if (row->hasMin || row->hasMax)
            range = "range " + (row->hasMin ? Format(row, row->min) : std::string("-inf")) + " to " + (row->hasMax ? Format(row, row->max) : std::string("inf"));
        else range = "no published range";
        if (row->Whole()) range += ", whole numbers";
        if (row->unlimited) range = "unlimited (-1 or 0), or " + (row->hasMin ? Format(row, row->min) : std::string("1")) + " to " +
            (row->hasMax ? Format(row, row->max) : std::string("100"));
        x += g_cv.Text(row->key, x, cy, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, 260)) + 16;
        x += g_cv.Text(range, x, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, 240)) + 14;
        x += g_cv.Text(AppliesText(row), x, cy, T(SZ_SMALL, row->Live() ? FAINT : PENDING, ui::BODY, ui::LEFT, 220)) + 14;
        if (row->shared || !row->also.empty()) x += Chip("SHARED", x, cy, PENDING, PENDING_SOFT, false) + 6;
        if (row->unverified) x += Chip("UNVERIFIED EFFECT", x, cy, FAINT, LINE, false) + 6;
        RowView v = ViewOf(row);
        if (!v.holder.empty())
            g_cv.Text(std::string(IsLuaHolder(v.holder) ? "Lua mod: " : "Mod: ") + v.holder + " = " + Format(row, v.base), x + 10, cy,
                T(SZ_SMALL, IsLuaHolder(v.holder) ? LUA : MOD, ui::BODY, ui::LEFT, std::max(40.0f, maxw - x - 10)));
        else if (!row->also.empty() || !row->alsoIf.empty())
        {
            auto list = [](const std::vector<std::string>& of) {
                std::string names;
                for (size_t i = 0; i < of.size() && i < 2; i++) names += (i ? ", " : "") + of[i];
                if (of.size() > 2) names += " +" + std::to_string(of.size() - 2);
                return names;
            };
            /* weapons that reach the row only by equipping the same ammunition are named as such (bug-report-0035) */
            std::string text = row->also.empty() ? "" : "also changes " + list(row->also);
            if (!row->alsoIf.empty())
                text += (text.empty() ? "also changes " : "; and ") + list(row->alsoIf) + " when they use " + row->alsoAmmo;
            g_cv.Text(text, x + 10, cy, T(SZ_SMALL, PENDING, ui::BODY, ui::LEFT, std::max(40.0f, maxw - x - 10)));
        }
        else if (!row->note.empty())
            g_cv.Text(row->note, x + 10, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, std::max(40.0f, maxw - x - 10)));
        return;
    }
    const char* text = g_view == SEARCH ? "Every word must match, in any order. Enter or a click opens the object in BROWSE." :
        g_view == CHANGES ? "Enter jumps to the field. RESET or Backspace stages a reset, UNDO drops a pending change; APPLY writes them." :
        g_view == MODS ? "\xE2\x86\x91\xE2\x86\x93 mod   PgUp PgDn value   Enter or a click opens the field; its reset goes back to the mod's value   F5 read again" :
        g_view == PRESETS ? "Enter or IMPORT stages the selected preset; EXPORT writes your saved changes as a preset HD2R Editor reads." :
        g_view == LOGS ? "\xE2\x86\x90\xE2\x86\x92 filter   Enter switch HellForge's log / the editor's   End follow the end" :
        "Settings are saved with the editor and apply at once.";
    g_cv.Text(text, 18, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, maxw));
}

void DrawFooter(float y, float h)
{
    g_cv.Rect(0, y, PANEL_W, h, FOOTER, 1);
    g_cv.Rect(0, y, PANEL_W, 1, LINE_STRONG, 2);
    std::string hint;
    auto items = g_view == BROWSE ? Items(Current()) : std::vector<Item>{};
    bool header = g_focus == FIELDS && g_field < (int)items.size() && items[g_field].header;
    if (g_edit.on) hint = "Type a number   Backspace erase   Tab / \xE2\x86\x91\xE2\x86\x93 confirm   Del cancel";
    else if (g_search.active) hint = "Type to filter   Backspace erase   Tab done   Del clear";
    else if (g_view == SEARCH) hint = "Type to search   \xE2\x86\x91\xE2\x86\x93 select   Enter open   Backspace erase   Del clear";
    else if (g_view == BROWSE && g_focus == FIELDS && header)
        hint = "F8 close   Enter open / close   \xE2\x86\x90\xE2\x86\x92 open, close   Ctrl+\xE2\x86\x91 collapse all   Ctrl+\xE2\x86\x93 expand all   F9 apply";
    else if (g_view == BROWSE && g_focus == FIELDS)
        hint = "F8 close   \xE2\x86\x90\xE2\x86\x92 adjust (Shift x10, Ctrl \xC3\xB7" "10)   digits type a value   Del revert   F9 apply";
    else hint = "F8 close   \xE2\x86\x91\xE2\x86\x93 select   \xE2\x86\x90\xE2\x86\x92 panes   Tab next pane   Shift+Tab next tab   F9 apply";
    g_cv.Text(hint, 18, y + h / 2, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, PANEL_W - 560));
    float bh = 34, by = y + (h - bh) / 2, bx = PANEL_W - 14;
    int pending = PendingCount();
    bool resettable = !SavedRows().empty() || pending > 0;
    float bw = 196;
    bx -= bw;
    if (Button("RESET TO DEFAULTS", bx, by, bw, bh, resettable && !Locked() ? DANGER : DISABLED, ui::Id("reset"))) StageResetAll();
    bw = 110; bx -= bw + 10;
    if (Button("DISCARD", bx, by, bw, bh, pending > 0 && !Locked() ? NORMAL : DISABLED, ui::Id("discard"))) Discard();
    bw = 150; bx -= bw + 10;
    if (Button(pending > 0 ? "APPLY  " + std::to_string(pending) : "APPLY", bx, by, bw, bh, pending > 0 && !Locked() ? PRIMARY : DISABLED, ui::Id("apply"))) Apply();
}

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
    if (Button("REVERT ALL", W - 216, y0 + 16, 196, 34, !Locked() && (saved > 0 || PendingCount() > 0) ? DANGER : DISABLED, ui::Id("revertall"))) StageResetAll();
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
        if (Button(list[i].state == 1 ? "UNDO" : "RESET", x + lw - 96, y + 4, 80, rh - 8, list[i].state == 1 ? NORMAL : DANGER, ui::Id("changereset", (uint64_t)i)))
            Revert(list[i].row);
    };
    List(o);
}

void DrawPresets(float y0, float h)
{
    float W = PANEL_W;
    g_cv.Rect(0, y0, W, h, PANEL, 1);
    g_cv.Text("PRESETS", 20, y0 + 24, T(20, TEXT, ui::TITLE));
    g_cv.Text("HD2R Editor presets (.hd2rpreset.json) in the editor's presets folder. IMPORT stages their values for APPLY.",
        20, y0 + 47, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, W - 520));
    float bx = W - 20;
    bx -= 150; if (Button("EXPORT", bx, y0 + 16, 150, 34, NORMAL, ui::Id("presetexport"))) ExportPreset();
    bx -= 160; if (Button("OPEN FOLDER", bx, y0 + 16, 150, 34, NORMAL, ui::Id("presetfolder"))) OpenPresetFolder();
    bx -= 130; if (Button("REFRESH", bx, y0 + 16, 120, 34, NORMAL, ui::Id("presetscan"))) ScanPresets();
    float hy = y0 + 66;
    if (g_presetResult.shown)
    {
        g_cv.Text(g_presetResult.name + ":  " + g_presetResult.text, 20, hy + 12, T(SZ_SMALL, g_presetResult.colour, ui::BODY, ui::LEFT, W - 40));
        hy += 26;
    }
    g_cv.Rect(0, hy, W, 22, HEADER, 2);
    g_cv.Text("PRESET", 20, hy + 11, T(SZ_TINY, FAINT, ui::TITLE));
    g_cv.Text("FILE", 420, hy + 11, T(SZ_TINY, FAINT, ui::TITLE));
    g_cv.Text("FIELDS", W - 330, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::RIGHT));
    g_cv.Text("MADE WITH", W - 140, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::RIGHT));
    if (g_presets.empty())
        g_cv.Text("No presets yet. OPEN FOLDER and put .hd2rpreset.json files there, or EXPORT your saved changes.", 20, hy + 44, T(SZ_SMALL, FAINT));
    ListOpts o;
    o.x = 0; o.y = hy + 24; o.w = W; o.h = y0 + h - hy - 28; o.rowH = 36; o.count = (int)g_presets.size(); o.selected = g_preset; o.key = "presets"; o.focused = true;
    o.click = [&](int i) { if (g_preset == i && g_presets[i].error.empty()) ImportPreset(g_presets[i]); else g_preset = i; };
    o.draw = [&](int i, float x, float y, float lw, float rh, bool hovered) {
        const PresetFile& p = g_presets[i];
        bool selected = i == g_preset;
        if (selected) { g_cv.Rect(x + 8, y + 2, lw - 16, rh - 4, SELECT, 2); g_cv.Rect(x + 8, y + 2, 3, rh - 4, GOLD, 3); }
        else if (hovered) g_cv.Rect(x + 8, y + 2, lw - 16, rh - 4, HOVER, 2);
        float cy = y + rh / 2;
        g_cv.Text(p.error.empty() ? p.name : p.file, x + 20, cy, T(SZ_LABEL, p.error.empty() ? (selected ? TEXT : DIM) : FAINT, ui::BODY, ui::LEFT, 380));
        g_cv.Text(p.error.empty() ? p.file : p.error, x + 420, cy, T(SZ_SMALL, p.error.empty() ? FAINT : ERROR_, ui::BODY, ui::LEFT, lw - 420 - 480));
        if (p.error.empty())
        {
            g_cv.Text(std::to_string(p.fields), x + lw - 330, cy, T(SZ_SMALL, DIM, ui::BODY, ui::RIGHT, 80));
            g_cv.Text(p.editor.empty() ? "?" : p.editor + (p.runtime.empty() ? "" : "  (" + p.runtime + ")"), x + lw - 140, cy, T(SZ_TINY, FAINT, ui::BODY, ui::RIGHT, 170));
        }
    };
    o.after = [&](int i, float x, float y, float lw, float rh) {
        if (g_presets[i].error.empty() && Button("IMPORT", x + lw - 116, y + 4, 100, rh - 8, PRIMARY, ui::Id("presetimport", (uint64_t)i)))
        { g_preset = i; ImportPreset(g_presets[i]); }
    };
    List(o);
}

void DrawLogs(float y0, float h)
{
    float W = PANEL_W;
    LogRead(false);
    g_cv.Rect(0, y0, W, h, PANEL, 1);
    g_cv.Text("LOGS", 20, y0 + 24, T(20, TEXT, ui::TITLE));
    g_cv.Text(g_logs.path.empty() ? "This HellForge does not say where its logs are (update HellForge)." : Narrow(g_logs.path),
        20, y0 + 47, T(SZ_SMALL, g_logs.path.empty() ? ERROR_ : FAINT, ui::BODY, ui::LEFT, W - 480));
    if (Button("OPEN LOG FILE", W - 216, y0 + 16, 196, 34, g_logs.path.empty() ? DISABLED : NORMAL, ui::Id("logfile")) && !g_logs.path.empty())
        if (!OpenWith(L"\"" + g_logs.path + L"\"")) Toast("Open it yourself: " + Narrow(g_logs.path), PENDING);
    if (Button("SHOW IN FOLDER", W - 424, y0 + 16, 196, 34, g_logs.path.empty() ? DISABLED : NORMAL, ui::Id("logfolder")) && !g_logs.path.empty())
        if (!OpenWith(L"/select,\"" + g_logs.path + L"\"")) Toast("Open it yourself: " + Narrow(g_logs.path), PENDING);
    /* the source (HellForge's log or the editor's), the filters, following the end */
    float fx = 20, fy = y0 + 68;
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
    if (g_logs.index < n)
    {
        /* the selected line in full, on two lines when it is long */
        const std::string& line = g_logs.lines[g_logs.shown[g_logs.index]];
        std::string first = g_cv.Fit(line, W - 40, SZ_SMALL);
        if (first.size() >= line.size() || first.size() < 4) g_cv.Text(line, 20, dy + 12, T(SZ_SMALL, TEXT, ui::BODY, ui::LEFT, W - 40));
        else
        {
            size_t cut = line.rfind(' ', first.size() - 3);
            if (cut == std::string::npos || cut < 20) cut = first.size() - 3;
            g_cv.Text(line.substr(0, cut), 20, dy + 12, T(SZ_SMALL, TEXT, ui::BODY, ui::LEFT, W - 40));
            g_cv.Text(line.substr(cut + (line[cut] == ' ' ? 1 : 0)), 20, dy + 32, T(SZ_SMALL, TEXT, ui::BODY, ui::LEFT, W - 40));
        }
    }
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
    if (Button("-", x + 400, y - 13, 30, 26, index > 0 ? NORMAL : DISABLED, ui::Id("scaledown"))) { g_uiScale = UI_SCALES[index - 1]; SaveSettings(); }
    char percent[16];
    snprintf(percent, sizeof percent, "%d%%", (int)floorf(g_uiScale * 100 + 0.5f));
    g_cv.Text(percent, x + 470, y, T(SZ_LABEL, TEXT, ui::BODY, ui::CENTER));
    if (Button("+", x + 510, y - 13, 30, 26, index + 1 < UI_SCALE_COUNT ? NORMAL : DISABLED, ui::Id("scaleup"))) { g_uiScale = UI_SCALES[index + 1]; SaveSettings(); }
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

void DrawPicker();
void DrawErrors();

void Draw()
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
    /* the search box: what is typed filters the list */
    float sy = y + (row->note.empty() ? 46 : 66), sh = 30, sw = w - 48;
    int matches = (int)std::count_if(g_picker.entries.begin(), g_picker.entries.end(), [](auto& e) { return !e.header; });
    g_cv.Rect(x + 24, sy, sw, sh, BOX, 11);
    g_cv.Frame(x + 24, sy, sw, sh, g_picker.filter.empty() ? BOX_EDGE : BOX_FOCUS, 12);
    bool empty = g_picker.filter.empty();
    float tw = g_cv.Text(empty ? "Search..." : g_picker.filter, x + 36, sy + sh / 2, T(SZ_LABEL, empty ? FAINT : TEXT, ui::BODY, ui::LEFT, sw - 140, 13));
    if ((int)(g_time * 2.2f) % 2 == 0) g_cv.Rect(empty ? x + 32 : x + 38 + tw, sy + 7, 2, sh - 14, GOLD, 13);
    g_cv.Text(std::to_string(matches) + (matches == 1 ? " match" : " matches"), x + 24 + sw - 12, sy + sh / 2, T(SZ_TINY, FAINT, ui::BODY, ui::RIGHT, 120, 13));
    RowView v = ViewOf(row);
    ListOpts o;
    float ly = sy + sh + 8;
    o.x = x + 2; o.y = ly; o.w = w - 4; o.h = y + h - ly - 44; o.rowH = row->pick.empty() ? 48 : 40; o.count = (int)g_picker.entries.size();
    o.selected = g_picker.index;
    o.key = "picker"; o.focused = true; o.layer = 12;
    o.click = [&](int i) { PickerChoose(i); };
    o.draw = [&](int i, float rx, float ry, float rw, float rh, bool hovered) {
        const PickEntry& option = g_picker.entries[i];
        if (option.header)
        {
            std::string upper = option.label;
            for (char& c : upper) c = (char)toupper((unsigned char)c);
            g_cv.Text(upper, rx + 22, ry + rh / 2 + 4, T(SZ_TINY, GOLD, ui::TITLE, ui::LEFT, rw - 44, 12));
            g_cv.Rect(rx + 22, ry + rh - 3, rw - 44, 1, LINE, 11);
            return;
        }
        bool selected = i == g_picker.index;
        if (selected) { g_cv.Rect(rx + 8, ry + 2, rw - 16, rh - 4, SELECT, 10); g_cv.Rect(rx + 8, ry + 2, 3, rh - 4, GOLD, 11); }
        else if (hovered) g_cv.Rect(rx + 8, ry + 2, rw - 16, rh - 4, HOVER, 10);
        float tx = rx + 22;
        const icons::Icon* icon = option.icon.empty() ? nullptr : icons::Find(icons::PASSIVE, option.icon);
        if (icon != nullptr && icons::Texture()) { g_cv.Image(icons::Texture(), rx + 18, ry + 6, rh - 12, rh - 12, icon->uv0, icon->uv1, PERK, 11); tx = rx + 18 + rh; }
        float top = row->pick.empty() ? 16 : 13, below = row->pick.empty() ? 34 : 29;
        float right = rx + rw - 20;
        if (option.value == v.value) right -= Chip("CURRENT", right, ry + top, GOLD, GOLD_WASH, true, 12) + 6;
        for (auto w = option.warnings.rbegin(); w != option.warnings.rend(); ++w)
            right -= Chip(WarningChip(*w), right, ry + top, *w == "very_large" || *w == "objective_packages" ? ERROR_ : PENDING,
                *w == "very_large" || *w == "objective_packages" ? ERROR_SOFT : PENDING_SOFT, true, 12) + 6;
        g_cv.Text(option.label, tx, ry + top, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, right - tx - 8, 12));
        std::string note = option.note;
        if (row->pick == "explosion" && option.value != 0)
            if (std::string sum = ExplosionSummary((uint32_t)option.value); !sum.empty()) note = sum + "   " + note;
        if (!note.empty()) g_cv.Text(note, tx, ry + below, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, rw - (tx - rx) - 24, 12));
    };
    List(o);
    if (Button("CANCEL", x + w - 24 - 150, y + h - 40, 150, 30, NORMAL, ui::Id("pickercancel"), 11)) g_picker.on = false;
    /* the highlighted option's warnings, spelled out (HD2R Editor's) */
    std::string warning;
    if (PickerHas(g_picker.index))
        for (auto& w : g_picker.entries[g_picker.index].warnings)
            if (auto text = g_catalog.warnings.find(w); text != g_catalog.warnings.end()) warning += (warning.empty() ? "" : "   ") + WarningChip(w) + ": " + text->second;
    if (!warning.empty()) g_cv.Text(warning, x + 24, y + h - 25, T(SZ_TINY, PENDING, ui::BODY, ui::LEFT, w - 24 - 190, 11));
    else g_cv.Text(row->pick.empty() ? "\xE2\x86\x91\xE2\x86\x93 select   Enter choose   Esc cancel" :
        "\xE2\x86\x91\xE2\x86\x93 select   type to filter   Backspace erase   Enter choose   Esc cancel", x + 24, y + h - 25, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, 0, 11));
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
    if (Button("\xC3\x97", x + w - 44, y + 12, 30, 30, NORMAL, ui::Id("errorsclose"), 11)) { g_errorsPopup.on = false; Sound(sounds::BACK); }
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
        if (Button(marked ? "UNDO" : "RESET", rx + rw - 102, ry + (rh - 24) / 2, 78, 24, marked ? NORMAL : DANGER, ui::Id("errorreset", (uint64_t)i), 12))
            Revert(list[i]);
    };
    List(o);
    if (n == 0) g_cv.Text("No errors: every value the editor applied was accepted.", x + 28, hy + 48, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, 0, 12));
    float dy = y + h - detail - 44;
    g_cv.Rect(x + 12, dy, w - 24, 1, LINE, 10);
    if (g_errorsPopup.index < n) g_cv.Text(g_errors[list[g_errorsPopup.index]], x + 24, dy + 18, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, w - 48, 11));
    if (Button("CLOSE", x + w - 24 - 150, y + h - 40, 150, 30, NORMAL, ui::Id("errorsdone"), 11)) { g_errorsPopup.on = false; Sound(sounds::BACK); }
    g_cv.Text("\xE2\x86\x91\xE2\x86\x93 select   Enter open the field   Del reset / undo   Esc close", x + 24, y + h - 25, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, 0, 11));
}


/* ---- co-op: the host's saved changes ----
 * The values of content mods added are this editor's own live edits (mod-stats.json), not Local Stats, so the session's
 * backend, which plays the host's Local Stats, does not reach them. Joined to a host who shares its stats (Locked), the
 * editor takes its own off and applies the host's, which the backend writes beside them (host-mod-stats.json); back on
 * its own, the host's go and its own come back. It also shows the host's saved changes as its own (marks, ACTIVE, the
 * Changes list), read-only: the host's Local Stats file (host-stats.json, which the backend has applied) and mod content
 * values. Joined to a host who lets every player keep their own, nothing here changes. */
std::map<const Row*, double> g_modLive; /* what the editor's live edits of mod content hold now */
int g_modRole = -1;                     /* HF_SESSION_JOINER: they are the host's; else this machine's (-1: none yet) */
std::string g_hostModText, g_hostStatsText, g_hostMountsText; /* the host's files as last applied and shown */

/* Every mount slot to the arm swaps shown (the host's while joined, else this machine's), or back to its vanilla weapon. */
void SendAllMounts()
{
    for (const Row* row : g_choiceRows) if (IsMount(row)) SendMount(row);
    g_markVersion++;
}
ULONGLONG g_modCheck;

std::vector<localstats::Entry> ModEntriesOf(const hfjson::Value& document)
{
    std::vector<localstats::Entry> out;
    for (auto& category : document.members)
        if (localstats::IsMod(category.first) && category.second.kind == hfjson::Value::Object)
            for (auto& target : category.second.members)
                for (auto& field : target.second.members)
                    if (field.second.kind == hfjson::Value::Number) out.push_back({category.first, target.first, field.first, field.second.AsNumber()});
    return out;
}

/* The game's mod content to these values; the editor's other live edits of it (and this machine's saved values, which an
 * APPLY may have set since) undone. Returns how many were applied. */
int ApplyModValues(const std::vector<localstats::Entry>& entries, const char* whose, const char* refused)
{
    const HfApi* api = g_host.api;
    std::map<const Row*, double> want;
    for (const localstats::Entry& e : entries)
    {
        if (!localstats::IsMod(e.category)) continue;
        const Row* row = g_catalog.Entry(e.category, e.target, e.key);
        if (row != nullptr && row->Live()) want[row] = row->unlimited && e.value == -1 ? 4294967295.0 : e.value;
    }
    std::set<const Row*> undo;
    for (auto& held : g_modLive) undo.insert(held.first);
    for (const localstats::Entry& e : localstats::OwnEntries())
        if (const Row* row = localstats::IsMod(e.category) ? g_catalog.Entry(e.category, e.target, e.key) : nullptr; row != nullptr && row->Live()) undo.insert(row);
    int applied = 0, undone = 0;
    for (const Row* row : undo)
    {
        if (want.count(row)) continue;
        HfField field = FieldOf(row);
        if (api->set_live(g_host.mod, row->ref, &field, nullptr, row->shared ? HF_LIVE_SHARED : 0)) undone++;
    }
    for (auto& [row, value] : want)
    {
        HfField field = FieldOf(row);
        HfValue v{row->type, value, 0};
        if (api->set_live(g_host.mod, row->ref, &field, &v, row->shared ? HF_LIVE_SHARED : 0)) applied++;
        else SetError(row, Refusal(refused));
    }
    g_modLive = want;
    g_live.clear();
    if (g_modRole != -1 && (applied || undone)) api->log(g_host.mod, "editor: %s mod content values: %d applied, %d undone", whose, applied, undone);
    return applied;
}

/* Joined to a host who shares its stats: the host's values, read again each second (the backend rewrites the files when
 * the host changes them). */
void SyncModValues()
{
    if (!Locked())
    {
        if (g_modRole == HF_SESSION_JOINER)
        {
            g_modRole = HF_SESSION_SOLO; /* this machine's own again */
            g_hostModText.clear();
            g_hostStatsText.clear();
            g_hostMountsText.clear();
            localstats::ShowOwn();
            if (g_hostMountsShown) { g_hostMountsShown = false; g_hostMounts.clear(); SendAllMounts(); }
            ApplyModValues(localstats::OwnEntries(), "this machine's", "HellForge refused its saved value");
        }
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (g_modRole == HF_SESSION_JOINER && now < g_modCheck) return;
    g_modCheck = now + 1000;
    std::wstring folder = hfjson::Wide(g_host.modFolder);
    std::string text, stats, mounts;
    if (!hfjson::ReadFile(folder + L"\\host-mod-stats.json", &text)) text.clear(); /* none (yet) */
    if (!hfjson::ReadFile(folder + L"\\host-stats.json", &stats)) stats.clear();
    if (!hfjson::ReadFile(folder + L"\\host-mounts.json", &mounts)) mounts.clear();
    if (g_modRole == HF_SESSION_JOINER && text == g_hostModText && stats == g_hostStatsText && mounts == g_hostMountsText) return;
    hfjson::Value document, statsDocument, mountsDocument;
    if (!text.empty() && !hfjson::Parse(text, &document, nullptr)) return; /* being written: the next second */
    if (!stats.empty() && !hfjson::Parse(stats, &statsDocument, nullptr)) return;
    if (!mounts.empty() && !hfjson::Parse(mounts, &mountsDocument, nullptr)) return;
    bool joining = g_modRole != HF_SESSION_JOINER;
    bool modsChanged = joining || text != g_hostModText, mountsChanged = joining || mounts != g_hostMountsText;
    g_hostModText = text;
    g_hostStatsText = stats;
    g_hostMountsText = mounts;
    g_modRole = HF_SESSION_JOINER;
    localstats::ShowHost(stats, text); /* from the moment of joining: empty until the host's files arrive */
    if (modsChanged) ApplyModValues(ModEntriesOf(document), "the host's", "HellForge refused the host's value");
    if (mountsChanged)
    {
        g_hostMounts.clear();
        if (mountsDocument.kind == hfjson::Value::Object)
            for (auto& m : mountsDocument.members) if (m.second.kind == hfjson::Value::Number) g_hostMounts[m.first] = m.second.AsNumber();
        g_hostMountsShown = true;
        SendAllMounts(); /* vehicles called in from now on carry the host's swaps */
        g_host.api->log(g_host.mod, "editor: the host's arm swaps: %d", (int)g_hostMounts.size());
    }
}

/* The layout for a screen: the interface size as large as still fits (full width, at least PANEL_MIN_H of height). */
void Layout(float width, float height, float* ox, float* oy, float* s)
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
} // namespace

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
    for (int n = 0; n < (int)g_catalog.navs.size(); n++)
    {
        if (n == 0 || g_catalog.navs[n].group != g_catalog.navs[n - 1].group) g_cats.push_back({true, g_catalog.navs[n].group, -1});
        g_cats.push_back({false, g_catalog.navs[n].label, n});
    }
    g_cat = g_cats.size() > 1 ? 1 : 0;
    g_logs.source = g_logSource == 1 ? 1 : 0;
    for (auto& r : g_catalog.rows) if (!r.choice.empty() && !IsApLabel(&r)) g_choiceRows.push_back(&r);
    if (g_choice[0] >= 0 || g_choice[1] >= 0) SendPassives(); /* the armor perks kept from last time */
    for (const Row* row : g_choiceRows) /* the arm swaps, pods and row copies kept from last time (a row several pages show is sent for each: the same value) */
        if (IsMount(row) && g_mounts.count(MountKey(row))) SendMount(row);
    host.api->log(host.mod, "editor: %s; %d game icons%s", g_catalogOk ? (std::to_string(g_catalog.objects.size()) + " objects, " +
        std::to_string(g_catalog.rows.size()) + " fields (read in " + std::to_string(loading) + " ms)").c_str() : g_catalogError.c_str(), iconCount,
        HasGetLive() ? "" : "; this HellForge has no get_live (values are not shown)");
}

/* The PNG a mod's data builds an icon resource from (its hellforge-data.json: {"icon": file, "name": resource}), or "". */
static std::wstring IconFile(const std::string& folder, const std::string& resource)
{
    std::string text, why;
    hfjson::Value data;
    std::wstring root = hfjson::Wide(folder);
    if (!hfjson::ReadFile(root + L"\\hellforge-data.json", &text) || !hfjson::Parse(text, &data, &why)) return L"";
    if (const hfjson::Value* patches = data.Find("patches"))
        for (const hfjson::Value& p : patches->items)
            if (const hfjson::Value* resources = p.Find("resources"))
                for (const hfjson::Value& r : resources->items)
                    if (r.GetString("name") == resource && !r.GetString("icon").empty())
                    {
                        std::wstring file = hfjson::Wide(r.GetString("icon"));
                        for (wchar_t& c : file) if (c == L'/') c = L'\\';
                        return file.find(L"..") == std::wstring::npos ? root + L"\\" + file : L"";
                    }
    return L"";
}

void AddModContent()
{
    static bool done;
    if (done || !g_catalogOk) return;
    done = true;
    const HfApi* api = g_host.api;
    if (api->size < offsetof(HfApi, content) + sizeof(void*) || api->content == nullptr) return;
    std::vector<HfContent> all;
    for (uint32_t i = 0; i < 4096; i++)
    {
        HfContent c{};
        c.size = sizeof c;
        if (!api->content(g_host.mod, i, &c)) break;
        all.push_back(c);
    }
    int pages = 0;
    for (const HfContent& s : all)
    {
        if (s.kind != HF_KIND_STRATAGEM) continue;
        const Object* like = g_catalog.StratagemObject(s.editor_like);
        if (like == nullptr) { api->log(g_host.mod, "editor: %s has no page (stratagem type %llu has none)", s.name, (unsigned long long)s.editor_like); continue; }
        /* its own settings row, the entities standing for the page's, and the projectiles and damage its mod copied from
         * the page's (one copy of each: two of the same base would be a guess) */
        catalog::Remap remap;
        remap.stratagem[s.editor_like] = s.id;
        std::string designation;
        for (const HfEditorEntity* e = s.editor_entities; e != nullptr && e->entity != 0; e++)
        {
            remap.entity[e->like] = e->entity;
            for (const HfContent& c : all)
                if (c.kind == HF_KIND_ENTITY && c.id == e->entity && c.designation != nullptr && designation.empty()) designation = c.designation;
        }
        /* what each entity standing in fires, against what the page's entity fires, and the damage of each (read from
         * the game); else the projectiles and damage its mod copied from the page's, one copy of each base */
        auto read = [&](const char* component, uint64_t target, uint16_t offset, uint64_t* out) {
            if (!HasGetLive()) return false;
            HfField field{component, g_layout, offset, 4, HF_TYPE_U32, 0};
            HfLiveValue value;
            if (!api->get_live(g_host.mod, target, &field, &value)) return false;
            *out = (uint64_t)value.now.number;
            return true;
        };
        for (auto& [likeEntity, copyEntity] : remap.entity)
        {
            uint64_t a, b;
            if (read("ProjectileWeaponComponentData", likeEntity, 0, &a) && read("ProjectileWeaponComponentData", copyEntity, 0, &b) && a != b && b != 0)
                remap.projectile[a] = b;
        }
        for (auto& [a, b] : remap.projectile)
        {
            uint64_t da, db;
            if (read("ProjectileSettings", a, 0x3C, &da) && read("ProjectileSettings", b, 0x3C, &db) && da != db && db != 0) remap.damage[da] = db;
        }
        std::map<uint64_t, int> projectiles, damages;
        for (const HfContent& c : all)
            if (c.mod != nullptr && s.mod != nullptr && !strcmp(c.mod, s.mod))
            {
                if (c.kind == HF_KIND_PROJECTILE) projectiles[c.base]++;
                if (c.kind == HF_KIND_DAMAGE) damages[c.base]++;
            }
        for (const HfContent& c : all)
            if (c.mod != nullptr && s.mod != nullptr && !strcmp(c.mod, s.mod))
            {
                if (c.kind == HF_KIND_PROJECTILE && projectiles[c.base] == 1) remap.projectile.emplace(c.base, c.id);
                if (c.kind == HF_KIND_DAMAGE && damages[c.base] == 1) remap.damage.emplace(c.base, c.id);
            }
        std::string title = s.title != nullptr ? s.title : like->name;
        std::string name = designation.empty() || title.rfind(designation, 0) == 0 ? title : designation + " " + title;
        /* its loadout section: the one its mod gave it, else its page's */
        std::string nav = s.category == HF_CATEGORY_OFFENSIVE ? "offensive" : s.category == HF_CATEGORY_DEFENSIVE ? "defensive"
            : s.category == HF_CATEGORY_MISSION ? "mission" : s.category == HF_CATEGORY_SUPPLY && like->tone != "support" ? "support" : "";
        /* an entity's fields only where it has the record (a Pelican's turret has no hellpod payload, say) */
        auto keep = [&](const Row& r, uint64_t ref) {
            if (r.component.find("ComponentData") == std::string::npos || !HasGetLive()) return true;
            HfField field = FieldOf(&r);
            HfLiveValue value;
            return api->get_live(g_host.mod, ref, &field, &value) != 0;
        };
        Object* o = g_catalog.AddCopy(*like, std::string("mod/") + s.name, name, std::string("mod:") + s.name, s.name, remap, nav, keep);
        if (o == nullptr) { api->log(g_host.mod, "editor: %s: none of its page's fields are its own", s.name); continue; }
        o->detail = std::string("Added by ") + s.mod;
        if (s.icon != nullptr && s.folder != nullptr)
        {
            std::wstring file = IconFile(s.folder, s.icon);
            if (!file.empty() && icons::AddStratagem(s.icon, file)) { o->icon = "stratagem"; o->sprite = s.icon; }
            else api->log(g_host.mod, "editor: %s: its icon's image was not found in its mod's data; it shows the page's", s.name);
        }
        pages++;
        api->log(g_host.mod, "editor: %s gets a page (%s's, %d field(s) of its own)", name.c_str(), like->name.c_str(), (int)o->rows.size());
    }
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
