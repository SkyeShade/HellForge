/* The editor's window (app.h) in parts, and what they share: its theme, its state and the functions one part calls in
 * another. app.cpp holds the state, the frame and the entry points; app_values.cpp the values (settings, co-op, what a
 * row holds and why it is locked); app_staging.cpp staging, applying and editing a value, and the marks; app_widgets.cpp
 * the drawing helpers and the list; app_page.cpp an object's page, navigation, search and the changes; app_presets.cpp,
 * app_mods_logs.cpp and app_sync.cpp the presets, the MODS and LOGS views and mod sync; app_keys.cpp the keys;
 * app_browse.cpp the header, BROWSE, the status bar and the footer; app_views.cpp the other views and the dialogs. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "app.h"
#include "canvas.h"
#include "catalog.h"
#include "json.h"
#include "local_stats.h"
#include "sounds.h"

namespace editorapp
{
using catalog::Object;
using catalog::Row;
using ui::Canvas;
using ui::TextOpts;

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
Tone ToneOf(const std::string& tone);
/* A faction's colour, the HD2R Editor's (its emblem, the game's HUD icon, is drawn in it); 0 for anything else. */
ImU32 FactionColour(const std::string& faction);
/* The faction whose emblem an object shows: an entity's own (structures and other entities too), else its page's. */
const std::string& FactionOf(const Object* object);
/* An object on a page where most rows have an icon (Structures, Other Entities, the factions', the rosters', the mission
 * difficulties'): a row without one keeps its room, so the names line up. */
bool EntityPage(const Object* object);
const float SZ_TITLE = 22, SZ_TAB = 15, SZ_HEADING = 13, SZ_LABEL = 15, SZ_SMALL = 13, SZ_TINY = 12;
const float PANEL_W = 1260, PANEL_H = 930, PANEL_MIN_H = 660;
const float UI_SCALES[] = {0.9f, 1.0f, 1.1f, 1.25f, 1.4f, 1.5f};
const int UI_SCALE_COUNT = sizeof UI_SCALES / sizeof UI_SCALES[0];
const float HEADER_H = 58, FOOTER_H = 56, STATUS_H = 40, ROW_H = 30, LIST_ROW_H = 34;
const float CAT_W = 224, OBJ_W = 334;
/* Column positions of the field rows (relative to the fields pane). */
const float FX_LABEL = 18, FX_DEFAULT_RIGHT = 404, FX_BOX = 416, FX_BOX_W = 120, FX_UNIT = 544, FX_BADGE_RIGHT = -14, INDENT = 20;
const char* const SEP = " \xC2\xB7 ";

inline TextOpts T(float size, ImU32 colour, ui::Face face = ui::BODY, ui::Align align = ui::LEFT, float max = 0, int z = 5)
{
    TextOpts o;
    o.size = size; o.colour = colour; o.face = face; o.align = align; o.max = max; o.z = z;
    return o;
}

/* ---- state (app.cpp) ---- */
extern Host g_host;
extern catalog::Catalog g_catalog;
extern bool g_catalogOk;
extern std::string g_catalogError;
extern uint32_t g_layout;
extern Canvas g_cv;
extern float g_time, g_panelH, g_scaleNow;

enum View { BROWSE, SEARCH, CHANGES, MODS, PRESETS, LOGS, SETTINGS, VIEW_COUNT };
const char* const VIEW_LABELS[] = {"BROWSE", "SEARCH", "CHANGES", "MODS", "PRESETS", "LOGS", "SETTINGS"};
extern View g_view;
enum Focus { CATEGORIES, OBJECTS, FIELDS };
extern Focus g_focus;

struct CatItem { bool group; std::string label; int nav; };
extern std::vector<CatItem> g_cats;
extern int g_cat, g_obj, g_field;
extern std::map<std::string, int> g_scroll, g_seen;
extern float g_catScroll; /* the category list's scroll, in units (its rows differ in height, so not a List) */
struct SearchBox { bool active = false; std::string text; };
extern SearchBox g_search;
extern std::map<std::string, std::string> g_group;
struct Pend { double value; bool reset; }; /* reset: back to the default when applied */
/* The armor perks (choice rows): what the editor keeps (its settings, "passives"), -1 for the kits' own; set_passives
 * from Init on, so they hold from every boot. */
extern double g_choice[2];
/* The picker (a choice row's options, or a reference row's: own, none, then the catalogue's references in groups), with
 * a filter typed while it is open. */
struct PickEntry { bool header = false; double value = 0; std::string label, note, icon; std::vector<std::string> warnings; };
struct Picker { bool on = false; const Row* row = nullptr; int index = 0; std::string filter; std::vector<PickEntry> entries; };
extern Picker g_picker;
inline int ChoiceIndex(const Row* row) { return row->choice == "second" ? 1 : 0; }
/* The arm swaps (a vehicle's Mounted Weapons) and the hellpod payloads (a call-in's pod: its slots and how many spawn):
 * the option chosen per row ("<vehicle>|mount.slot_N", "<rack>|pod.slot_N", "<rack>|pod.count"), kept in the editor's
 * settings ("mounts") and applied live (set_live: a slot's u64 entity, the count's u32) from Init on. */
extern std::map<std::string, double> g_mounts;
/* Joined to a friend's lobby: the host's arm swaps (host-mounts.json, which the session's backend writes), applied and
 * shown in place of this machine's; the armor perks are solo only (with other players the loader gives everyone the
 * armor's own), so a joiner shows none. */
extern std::map<std::string, double> g_hostMounts;
extern bool g_hostMountsShown;
inline const std::map<std::string, double>& Mounts() { return g_hostMountsShown ? g_hostMounts : g_mounts; }
inline std::string MountKey(const Row* row) { return row->target + "|" + row->key; }
inline bool IsMount(const Row* row) { return row->choice == "mount" || row->choice == "pod" || row->choice == "podcount" || row->choice == "rowcopy"; }
inline bool IsPod(const Row* row) { return row->choice == "pod" || row->choice == "podcount"; }
/* The armory's displayed armor penetration (HD2R Editor's row): which armor penetration trait the weapon's five trait
 * slots hold. Not kept anywhere itself: shown from the slots, and a pick is staged on the slot that holds one, else the
 * first empty one (ApSlot), so the trait slots are what Local Stats saves. */
inline bool IsApLabel(const Row* row) { return row->choice == "aplabel"; }
std::vector<const Row*> TraitRows(const Row* row);
bool ChoiceSaved(const Row* row);
extern std::map<const Row*, std::string> g_errors;
/* Caches (thousands of changes made every frame slow): the frame, and versions of what views depend on. */
extern unsigned g_frame, g_errorsVersion, g_layoutVersion;
inline void SetError(const Row* row, const std::string& why) { g_errors[row] = why; g_errorsVersion++; }
inline void ClearError(const Row* row) { if (g_errors.erase(row)) g_errorsVersion++; }
extern std::map<const Row*, Pend> g_pending;
extern bool g_soundsOn;
extern int g_logSource; /* the LOGS tab's source from the settings (0 HellForge's log, 1 the editor's) */
struct Edit { bool on = false; const Row* row = nullptr; std::string buffer; bool fresh = false; };
extern Edit g_edit;
extern std::set<std::string> g_open; /* the headers open (every one starts closed), per item: kept in editor-settings.json */
extern float g_uiScale;
struct ToastLine { bool on = false; std::string text; ImU32 colour = TEXT; float time = 0; };
extern ToastLine g_toast;
struct GlobalSearch { std::string text; int index = 0; };
extern GlobalSearch g_gsearch;
extern int g_change;
struct LiveState { bool read = false; double now = 0, base = 0; std::string mod; ULONGLONG at = 0; };
extern std::map<const Row*, LiveState> g_live;
/* A live read is a lookup in the mod loader under its lock, a long one for some fields: an enemy page reads 300 at
 * once, and they all go stale together. The reads drawing asks for share LIVE_BUDGET_US a frame (HolderTick gets what
 * they leave of it, after the page); the rest wait for a later frame, which also spreads their next refresh. A fresh
 * read (after an edit) is never held back. Frame() logs a frame that was slow anyway. */
const LONGLONG LIVE_BUDGET_US = 1500;
struct LiveFrameReads { unsigned frame = 0; LONGLONG ticks = 0; int reads = 0, held = 0; };
LONGLONG QpcFrequency();
LONGLONG QpcNow();
/* This frame's live reads so far (reset when the frame changes). */
LiveFrameReads& LiveFrame();
struct Drag { std::string key; float trackY = 0, trackH = 0, thumb = 0; int count = 0, rows = 0; };
extern Drag g_drag;
extern unsigned g_markVersion;

inline void Sound(sounds::Kind kind) { sounds::Request(kind); }
void Toast(const std::string& text, ImU32 colour);

/* ---- values (app_values.cpp) ---- */
void LoadSettings();
void SaveSettings();
/* HellForge's reason for the call that was just refused (its last log line), when this HellForge says it. */
std::string Refusal(const char* fallback);
bool HasGetLive();
bool LiveStat(const Row* row);
HfStat StatOf(const Row* row);
extern HfSession g_session;
extern bool g_lobbyStats; /* the lobby plays with the host's stats (hosting: this machine's; joined: the host's) */
extern bool g_hostStats;  /* joined, playing with the host's stats */
void ReadSession();
inline bool Locked() { return g_hostStats; }
bool RefuseLocked();
HfField FieldOf(const Row* row);
bool HasPassives();
void SendPassives();
/* budgeted: the read shares the frame's LIVE_BUDGET_US (drawing); HolderTick reads within its own budget */
LiveState& Live(const Row* row, bool fresh = false, bool budgeted = true);
bool Same(const Row* row, double a, double b);
std::string RefLabel(const Row* row, double v);
std::string Format(const Row* row, double v);
bool Saved(const Row* row, double* value, const Row** holder = nullptr);
const Pend* PendingOf(const Row* row);
enum Source { GAME, MODDED, EDITED, STAGED };
struct RowView { bool has = false; double value = 0; Source source = GAME; bool hasBase = false; double base = 0; std::string holder; bool reset = false; };
RowView ViewOf(const Row* row, bool withPending = true);
uint32_t ExplosionDamage(uint32_t type);
bool ReadSetting(const char* component, uint64_t ref, uint16_t offset, uint32_t type, double* out);
std::string ExplosionSummary(uint32_t type);
std::string WarningChip(const std::string& id);
std::string MountVehicle(const Row* row);
bool Swapped(const Row* row, double* value);
std::string LockOf(const Row* row);
std::string SlotBlock(const Row* row);
bool LaterSlotFilled(const Row* row);
std::string AppliesText(const Row* row);

/* ---- staging, applying and editing (app_staging.cpp) ---- */
void Stage(const Row* row, double value);
bool StageReset(const Row* row);
void SendMount(const Row* row);
inline int PendingCount() { return (int)g_pending.size(); }
void Apply();
void Discard();
extern std::vector<const Row*> g_choiceRows; /* the rows the editor keeps itself (armor perks, arm swaps): found at Init */
const std::vector<const Row*>& SavedRows();
void StageResetAll();
extern std::map<const Row*, std::string> g_holders;
extern bool g_holdersRead;
inline bool IsLuaHolder(const std::string& id) { return id.rfind("lua.", 0) == 0; }
void HolderTick(LONGLONG microseconds);
struct Marks { bool error = false, pending = false, editor = false, mod = false, lua = false; };
struct MarkCache { unsigned version = 0, stats = 0, errors = 0; std::map<const Object*, Marks> objects; std::map<std::string, Marks> navs; };
const MarkCache& Markers();
Marks RowsMarks(const std::vector<const Row*>& rows);
void PickerFill();
bool PickerHas(int i);
int PickerStep(int from, int step);
void PickerSelect(double value);
void PickerChoose(int i);
void BeginEdit(const Row* row, const std::string& text, bool fresh);
bool Valid(const Row* row, double v, double* out, std::string* error);
void CommitEdit();
void Nudge(const Row* row, int direction, bool shift, bool ctrl);
void Revert(const Row* row);

/* ---- drawing helpers (app_widgets.cpp) ---- */
std::vector<std::string> Wrapped(const std::string& text, float max, float size);
void Arrow(float x, float cy, bool down, ImU32 colour);
float Chip(const std::string& text, float x, float cy, ImU32 fg, ImU32 bg, bool right, int z = 3);
enum Style { NORMAL, PRIMARY, DANGER, DISABLED };
struct Area { float x, y, w, h; }; /* a box in panel units: its corner and size */
bool Button(const std::string& label, const Area& a, Style style, uint64_t id, int z = 3);
void Logo(float x, float y, float s);
float DrawMarks(const Marks& m, float x, float cy, float size);
bool DrawIcon(const Object* object, float x, float y, float size, int z = 4);
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
void List(const ListOpts& o);

/* ---- the page, navigation, search, changes (app_page.cpp) ---- */
const catalog::Nav* NavOf(int cat);
std::string Lower(std::string s);
std::vector<Object*> Objects();
Object* Current();
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
const std::vector<Item>& Items(const Object* object);
void Toggle(const std::string& key, int open = -1);
void SetPageOpen(const Object* object, bool open);
bool PageOpen(const Object* object);
std::string RowLabel(const Item& item, float max);
struct ErrorsPopup { bool on = false; int index = 0; };
extern ErrorsPopup g_errorsPopup;
std::vector<const Row*> ErrorRows();
void OpenErrors();
std::vector<const Row*> ItemChanges(const Object* object);
void ResetItem(const Object* object);
void SelectCategory(int i);
void MoveCategory(int delta);
void SelectObject(int i);
bool GotoObject(const Object* object);
void Reveal(const Row* row);
struct Result { Object* object; std::string sub; bool prefix; int order; };
std::vector<Result> SearchResults(const std::string& text);
struct Change { const Row* row; double value; int state; bool reset; }; /* state 0 edited, 1 pending, 2 error */
const std::vector<Change>& Changes();

/* ---- presets (app_presets.cpp) ---- */
struct PresetFile { std::wstring path; std::string file, name, editor, runtime, error; int fields = 0; };
extern std::vector<PresetFile> g_presets;
extern int g_preset;
std::string Narrow(const std::wstring& text);
void ScanPresets();
void ImportPreset(const PresetFile& p);
void DrawPresets(float y0, float h);

/* ---- MODS and LOGS (app_mods_logs.cpp) ---- */
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
};
extern LogView g_logs;
void LogFilter();
void LogRead(bool force);
struct ModInfo { std::string id, name, detail, folder; uint32_t kind = 0; bool loaded = true; std::vector<const Row*> rows; };
struct ModEntry { std::string heading; int mod = -1; };
extern int g_modEntry, g_modRow;
std::vector<ModEntry> ModEntries();
std::vector<const Row*> ModRows();
void ScanMods(bool now);
void DrawMods(float y0, float h);
void DrawLogs(float y0, float h);

/* ---- mod sync (app_sync.cpp) ---- */
struct SyncDialog
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
};
extern SyncDialog g_sync;
inline bool SyncShown() { return g_sync.on; } /* mod sync's dialog is up */
extern bool g_windowShown; /* Opened() to Closed() */
void ForgetKeys();
void ShowSyncAlone(bool on);
void AnswerSync(uint32_t answer, const char* id = nullptr);
void ReadSync();
void SyncKeys();
void DrawSync();

/* ---- keys (app_keys.cpp) ---- */
inline bool Pressed(ImGuiKey key) { return ImGui::IsKeyPressed(key, true); }
void HandleKeys();

/* ---- the header, BROWSE, the status bar and the footer (app_browse.cpp) ---- */
void DrawHeader(int errors, int active);
void DrawCategories(float y0, float h);
void DrawObjects(float y0, float h);
void DrawFields(float y0, float h);
void DrawStatus(float y, float h);
void DrawFooter(float y, float h);

/* ---- SEARCH, CHANGES, SETTINGS and the dialogs (app_views.cpp) ---- */
void DrawSearch(float y0, float h);
void DrawChanges(float y0, float h);
void DrawSettings(float y0, float h);
void DrawPicker();
void DrawErrors();
} // namespace editorapp
