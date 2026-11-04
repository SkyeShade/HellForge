/* The editor's window (app_internal.h): the lobby browser. */
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "app_internal.h"

namespace editorapp
{
/* ---- the lobby browser (SDK 16): every HellForge lobby the session's backend finds ----
 * The backend lists every lobby of its directory, with no planet or difficulty filter, and friends' lobbies, each with
 * what joining it would take (docs/multiplayer.md "The lobby browser"); the loader hands the list over (browse_state,
 * browse_lobby) and the player's asks back (browse_ask). The ship's information terminal opens it (the loader counts
 * each time, browse_state's terminal), and so does Ctrl+L in the window. Like the MOD SYNC prompt it shows alone over the
 * game, holding its input, or in the window when that is open. A join hands the game the lobby's invitation, which the
 * player accepts in game, so the browser closes once it is there. A placeholder: a mod that draws a browser of its own
 * claims it (HF_BROWSE_CLAIM, browse_state's owner; docs/lobby-browser.md), and this one then never shows. */
LobbyDialog g_lobbies;
/* The key or click that opened it (the terminal's interact key) must not answer it: its keys wait this long. */
const double LOBBY_ARM = 0.5;
/* While it shows, a new list every this many seconds (the backend searches at most once every 2 s, whoever asks). */
const double LOBBY_REFRESH = 10;
/* Seconds on the wall clock: ImGui's time stands still while the editor draws nothing (closed, nothing over the game)
 * and jumps on the next frame, which would arm the browser at once and ask for a second list. */
static double Now() { return (double)GetTickCount64() / 1000.0; }
static bool LobbiesArmed() { return Now() - g_lobbies.shownAt >= LOBBY_ARM; }
/* A search shows as one for at least this long: the backend answers a list it searched in the last 2 s at once, and a
 * REFRESH that changed nothing on screen looked like it did nothing. */
const double LOBBY_SEARCH_SHOWN = 0.8;
/* The backend gives a search 30 s (SteamLobbyDirectory) and answers every ask with a final state; a search or a join it
 * never answers (a lost line, a backend that hung) is given up after this long, so the browser never stays on SEARCHING
 * or a join. */
const double LOBBY_GIVE_UP = 60;
static bool Searching()
{
    return (g_lobbies.state.state == HF_BROWSE_SEARCHING && !g_lobbies.searchStuck) || Now() - g_lobbies.askedAt < LOBBY_SEARCH_SHOWN;
}

/* What Quick Play asks for while the backend picks (g_lobbies.joining): JoinAnswered then hears its outcome. */
const char* const QUICK_JOIN = "quick play";

bool HasBrowse() { return g_host.api->size >= offsetof(HfApi, browse_ask) + sizeof(void*) && g_host.api->browse_state != nullptr; }

static void AskList()
{
    g_lobbies.askedAt = Now();
    if (!g_host.api->browse_ask(g_host.mod, HF_BROWSE_REFRESH, nullptr) && g_lobbies.on)
        Toast("The session's backend is not connected: no lobbies to list", ERROR_);
}

/* The browser alone over the game, the window closed: the editor holds the game's input for it. */
void ShowLobbiesAlone(bool on)
{
    if (g_lobbies.alone == on) return;
    g_lobbies.alone = on;
    HoldInput();
    ForgetKeys();
    if (on) g_lobbies.shownAt = Now();
    sounds::EditorOpen(on || g_sync.alone);
    g_host.api->log(g_host.mod, on ? "editor: the lobby browser shows over the game, the window closed" : "editor: the lobby browser goes");
    if (on) Sound(sounds::SHOW);
}

void OpenLobbies(const char* why)
{
    if (!HasBrowse()) { if (g_windowShown) Toast("This HellForge has no lobby browser (SDK 16)", ERROR_); return; }
    if (!g_lobbies.owner.empty())
    {
        if (g_windowShown) Toast("The lobby browser is " + g_lobbies.owner + "'s: use the ship's information terminal", OK);
        return;
    }
    g_host.api->log(g_host.mod, "editor: the lobby browser opens (%s)", why);
    bool fresh = !g_lobbies.on;
    g_lobbies.on = true;
    g_lobbies.closingAt = 0;
    g_lobbies.joining.clear();
    g_picker.on = false;
    g_edit.on = false;
    if (fresh) { g_lobbies.shownAt = Now(); g_seen.erase("lobbies"); }
    AskList();
    if (!g_windowShown) ShowLobbiesAlone(true);
    else if (fresh) Sound(sounds::OPEN);
}

/* The browser went: the terminal's camera glides back to the player. */
void LobbiesGone()
{
    g_lobbies.camera = false;
    if (HasBrowse()) g_host.api->browse_ask(g_host.mod, HF_BROWSE_HIDDEN, nullptr);
}

static void CloseLobbies()
{
    LobbiesGone();
    g_lobbies.on = false;
    g_lobbies.confirmStrangers = false;
    g_lobbies.closingAt = 0;
    g_lobbies.joining.clear();
    ShowLobbiesAlone(false);
}

/* The selected lobby, or null: shown and lobbies are checked against each other, as a list read since may be shorter. */
static const HfBrowseLobby* Selected()
{
    if (g_lobbies.index < 0 || g_lobbies.index >= (int)g_lobbies.shown.size()) return nullptr;
    int at = g_lobbies.shown[g_lobbies.index];
    return at >= 0 && at < (int)g_lobbies.lobbies.size() ? &g_lobbies.lobbies[at] : nullptr;
}

/* A lobby no setting of the player's lets in: mods in the way (a stranger's unchecked ones, mods to install by hand,
 * another HellForge) or a blocked mod. A friend's stays in reach: the player can ask them. */
static bool OutOfReach(const HfBrowseLobby& l)
{
    return (l.verdict == HF_LOBBY_UNJOINABLE || l.verdict == HF_LOBBY_BLOCKED) && l.fix == HF_LOBBY_FIX_NONE && !(l.flags & HF_LOBBY_FLAG_FRIEND);
}

/* The lobbies the filters leave; the selection kept on the lobby whose id is selected where it can be. Lobbies out of
 * reach are hidden while any other is left (unless showUnreachable): a list of lobbies that can't be joined hid the ones
 * that can. The backend lists those last (BrowseApi.Rank). */
static bool Passes(const HfBrowseLobby& l)
{
    if (g_lobbies.joinableOnly && !(l.flags & HF_LOBBY_FLAG_JOINABLE)) return false;
    if (g_lobbies.friendsOnly && !(l.flags & HF_LOBBY_FLAG_FRIEND)) return false;
    return g_lobbies.difficulty <= 0 || l.difficulty >= g_lobbies.difficulty;
}

static void Filter(const std::string& selected)
{
    g_lobbies.shown.clear();
    std::vector<int> left;
    for (int i = 0; i < (int)g_lobbies.lobbies.size(); i++)
        if (Passes(g_lobbies.lobbies[i])) left.push_back(i);
    int reachable = (int)std::count_if(left.begin(), left.end(), [](int i) { return !OutOfReach(g_lobbies.lobbies[i]); });
    g_lobbies.unreachable = (int)left.size() - reachable;
    bool hide = !g_lobbies.showUnreachable && reachable > 0;
    g_lobbies.hidden = hide ? g_lobbies.unreachable : 0;
    for (int i : left)
    {
        const HfBrowseLobby& l = g_lobbies.lobbies[i];
        if (hide && OutOfReach(l)) continue;
        if (l.lobby_id == selected) g_lobbies.index = (int)g_lobbies.shown.size();
        g_lobbies.shown.push_back(i);
    }
    g_lobbies.index = std::clamp(g_lobbies.index, 0, std::max(0, (int)g_lobbies.shown.size() - 1));
}

static void Filter()
{
    const HfBrowseLobby* l = Selected();
    Filter(l != nullptr ? std::string(l->lobby_id) : std::string());
}

static void ReadLobbyList(uint32_t count)
{
    const HfBrowseLobby* was = Selected();
    std::string selected = was != nullptr ? was->lobby_id : ""; /* before the list it points into goes */
    g_lobbies.lobbies.clear();
    for (uint32_t i = 0; i < count; i++)
    {
        HfBrowseLobby lobby{};
        lobby.size = sizeof lobby;
        if (!g_host.api->browse_lobby(g_host.mod, i, &lobby)) break;
        g_lobbies.lobbies.push_back(lobby);
    }
    Filter(selected);
}

/* The backend's answer to a join: invited, the browser goes so the player can accept the invitation in game. */
static void JoinAnswered(const HfBrowseState& now)
{
    if (g_lobbies.joining.empty()) return;
    if (now.state == HF_BROWSE_INVITED)
    {
        g_host.api->log(g_host.mod, "editor: lobby browser: invited (%s); the browser closes for the game's invitation", g_lobbies.joining.c_str());
        if (g_lobbies.joining == QUICK_JOIN && now.message[0]) Toast(now.message, OK);
        Sound(sounds::APPLY);
        CloseLobbies();
    }
    else if (now.state == HF_BROWSE_FAILED)
    {
        g_lobbies.joining.clear();
        Toast(now.message[0] ? now.message : "That lobby cannot be joined", ERROR_);
        Sound(sounds::BACK);
    }
}

/* The mod that claimed the browser (browse_state's owner): while there is one, this browser closes and never opens. */
static void ReadOwner(const HfBrowseState& now)
{
    if (g_lobbies.owner == now.owner) return;
    g_lobbies.owner = now.owner;
    if (g_lobbies.owner.empty()) return;
    g_host.api->log(g_host.mod, "editor: the lobby browser is %s's: the editor shows none of its own", now.owner);
    if (g_lobbies.on) CloseLobbies();
}

/* A search or a join the backend never answered (LOBBY_GIVE_UP): the browser leaves SEARCHING, the join's wait ends. */
static void GiveUpUnanswered(const HfBrowseState& now)
{
    if (now.state == HF_BROWSE_SEARCHING && !g_lobbies.searchStuck && Now() - g_lobbies.searchSince >= LOBBY_GIVE_UP)
    {
        g_lobbies.searchStuck = true;
        g_host.api->log(g_host.mod, "editor: lobby browser: the search had no answer in %.0f s; given up", LOBBY_GIVE_UP);
    }
    if (!g_lobbies.joining.empty() && Now() - g_lobbies.joinedAt >= LOBBY_GIVE_UP)
    {
        g_host.api->log(g_host.mod, "editor: lobby browser: the join (%s) had no answer in %.0f s; given up", g_lobbies.joining.c_str(), LOBBY_GIVE_UP);
        g_lobbies.joining.clear();
        if (g_lobbies.on) Toast("The join had no answer from the session's backend. Try again.", ERROR_);
    }
}

/* Every 15 frames (Tick): the terminal's count (it opens the browser), the list when it changed, and a new list every
 * LOBBY_REFRESH seconds while it shows. */
void ReadLobbies()
{
    static int frames;
    if (!HasBrowse() || (++frames < 15 && !g_lobbies.camera)) return; /* every frame while the camera glides */
    frames = 0;
    HfBrowseState now{};
    now.size = sizeof now;
    g_host.api->browse_state(g_host.mod, &now);
    ReadOwner(now);
    if (!g_lobbies.terminalRead) { g_lobbies.terminalRead = true; g_lobbies.terminal = now.terminal; }
    else if (now.terminal != g_lobbies.terminal)
    {
        g_lobbies.terminal = now.terminal;
        if (g_lobbies.owner.empty()) OpenLobbies("the ship's information terminal");
        g_lobbies.camera = g_lobbies.on && now.camera == HF_BROWSE_CAMERA_MOVING; /* the screen powers on when it arrives */
    }
    if (g_lobbies.camera && now.camera != HF_BROWSE_CAMERA_MOVING)
    {
        g_lobbies.camera = false; /* on the terminal's screen (or it gave up): the browser powers on there */
        g_lobbies.shownAt = Now();
        g_host.api->browse_ask(g_host.mod, HF_BROWSE_SHOWN, nullptr);
    }
    HfBrowseState& was = g_lobbies.state;
    bool turned = now.version != was.version || now.state != was.state;
    if (turned || strcmp(now.message, was.message))
    {
        if (now.version != was.version || now.lobbies != g_lobbies.lobbies.size()) ReadLobbyList(now.lobbies);
        if (now.version != was.version && now.state == HF_BROWSE_READY) g_lobbies.listedAt = Now();
        if (turned) { g_lobbies.searchStuck = false; if (now.state == HF_BROWSE_SEARCHING) g_lobbies.searchSince = Now(); }
        g_lobbies.state = now;
        if (turned) JoinAnswered(now);
    }
    GiveUpUnanswered(now);
    if (g_lobbies.on && g_lobbies.joining.empty() && !Searching() && Now() - g_lobbies.askedAt >= LOBBY_REFRESH) AskList();
}

static bool Joinable(const HfBrowseLobby* l) { return l != nullptr && (l->flags & HF_LOBBY_FLAG_JOINABLE) && g_lobbies.joining.empty(); }

static void Join(const HfBrowseLobby* l)
{
    if (!Joinable(l) || !LobbiesArmed()) return;
    if (!g_host.api->browse_ask(g_host.mod, HF_BROWSE_JOIN, l->lobby_id)) { Toast("The session's backend is not connected", ERROR_); return; }
    g_lobbies.joining = l->lobby_id;
    g_lobbies.joinedAt = Now();
    Sound(sounds::CLICK);
}

/* Quick Play: the backend joins the best lobby it can (a friend's first, then the least to download). */
static void Quick()
{
    if (!g_lobbies.joining.empty() || !LobbiesArmed()) return;
    if (!g_host.api->browse_ask(g_host.mod, HF_BROWSE_QUICK, nullptr)) { Toast("The session's backend is not connected", ERROR_); return; }
    g_lobbies.joining = QUICK_JOIN;
    g_lobbies.joinedAt = Now();
    Sound(sounds::CLICK);
}

/* The setting that keeps a lobby out, turned on from the browser: Play with strangers only after its warning
 * (confirmStrangers, then TurnOnStrangers), edited stats at once. */
const char* const STRANGER_WARNING = "Strangers' games send yours data you cannot check: a modified game can lie about its mods, and a flaw in "
    "the game's network code could let it run code on your PC. Turn this on only if you accept that risk. It stays on for this session only: "
    "the launcher's Settings > MULTIPLAYER keeps your choice.";

static void Fix(const HfBrowseLobby* l)
{
    if (l == nullptr || !LobbiesArmed()) return;
    if (l->fix == HF_LOBBY_FIX_STRANGERS) { g_lobbies.confirmStrangers = true; Sound(sounds::CLICK); }
    else if (l->fix == HF_LOBBY_FIX_EDITED_STATS)
    {
        if (!g_host.api->browse_ask(g_host.mod, HF_BROWSE_SHOW_EDITED_STATS, nullptr)) { Toast("The session's backend is not connected", ERROR_); return; }
        Toast("Lobbies whose host shares edited stats show for this session. Joining one, you play with the host's stats.", OK);
        g_lobbies.askedAt = Now();
        Sound(sounds::CLICK);
    }
}

static void TurnOnStrangers()
{
    g_lobbies.confirmStrangers = false;
    if (!g_host.api->browse_ask(g_host.mod, HF_BROWSE_ALLOW_STRANGERS, nullptr)) { Toast("The session's backend is not connected", ERROR_); return; }
    g_host.api->log(g_host.mod, "editor: lobby browser: the player turned Play with strangers on for this session, after its warning");
    Toast("Play with strangers is on for this session: strangers' lobbies can be joined.", OK);
    g_lobbies.askedAt = Now();
    Sound(sounds::APPLY);
}

static void Refresh()
{
    if (Searching()) return;
    AskList();
    Sound(sounds::CLICK);
}

static void ToggleJoinable() { g_lobbies.joinableOnly = !g_lobbies.joinableOnly; Filter(); Sound(sounds::CLICK); }
static void ToggleFriends() { g_lobbies.friendsOnly = !g_lobbies.friendsOnly; Filter(); Sound(sounds::CLICK); }
static void ToggleUnreachable() { g_lobbies.showUnreachable = !g_lobbies.showUnreachable; Filter(); Sound(sounds::CLICK); }
/* any, then 5 and up to 10 and up, then any again */
static void NextDifficulty() { g_lobbies.difficulty = g_lobbies.difficulty == 0 ? 5 : g_lobbies.difficulty >= 10 ? 0 : g_lobbies.difficulty + 1; Filter(); Sound(sounds::CLICK); }

static void PowerOff();

/* Esc: the window closes (Closed() closes the browser too), or the browser goes. */
static void LobbyEscape()
{
    if (g_windowShown) g_host.close();
    else { CloseLobbies(); Sound(sounds::BACK); }
}

void LobbyKeys()
{
    if (g_lobbies.camera) return;
    if (g_lobbies.confirmStrangers)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_N, false)) { g_lobbies.confirmStrangers = false; Sound(sounds::BACK); }
        else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) TurnOnStrangers();
        return;
    }
    int n = (int)g_lobbies.shown.size();
    if (Pressed(ImGuiKey_UpArrow)) g_lobbies.index = std::max(0, g_lobbies.index - 1);
    if (Pressed(ImGuiKey_DownArrow)) g_lobbies.index = std::min(std::max(0, n - 1), g_lobbies.index + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { PowerOff(); return; }
    if (!LobbiesArmed()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) Join(Selected());
    static const struct { ImGuiKey key; void (*action)(); } keys[] = {
        {ImGuiKey_R, Refresh}, {ImGuiKey_J, ToggleJoinable}, {ImGuiKey_F, ToggleFriends}, {ImGuiKey_H, ToggleUnreachable},
        {ImGuiKey_D, NextDifficulty}, {ImGuiKey_Q, Quick}, {ImGuiKey_A, [] { Fix(Selected()); }}};
    for (const auto& k : keys)
        if (ImGui::IsKeyPressed(k.key, false)) k.action();
}

static std::string Megabytes(uint64_t bytes)
{
    char text[32];
    snprintf(text, sizeof text, bytes >= 1024 * 1024 ? "%.1f MB" : "%.0f KB", bytes >= 1024 * 1024 ? (double)bytes / 1048576.0 : (double)(bytes + 1023) / 1024.0);
    return text;
}

/* What joining a lobby takes, in a few words, and its colour. */
static std::string Verdict(const HfBrowseLobby& l, ImU32* colour)
{
    switch (l.verdict)
    {
    case HF_LOBBY_READY: *colour = OK; return "Ready to join";
    case HF_LOBBY_DOWNLOAD: *colour = GOLD; return "Downloads " + Megabytes(l.download_bytes);
    case HF_LOBBY_TRUST: *colour = PENDING; return "Needs your trust";
    case HF_LOBBY_BLOCKED: *colour = ERROR_; return l.reason[0] ? l.reason : "Runs a mod you blocked";
    case HF_LOBBY_FULL: *colour = FAINT; return "Full";
    case HF_LOBBY_LOCKED: *colour = FAINT; return "Locked";
    case HF_LOBBY_UNKNOWN: *colour = DIM; return "A friend's lobby: mods known on joining";
    default: *colour = ERROR_; return std::string("Can't join") + (l.reason[0] ? ": " : "") + l.reason;
    }
}

/* The reason's start for a stranger's unchecked mods: ModSync.FriendsOnlyWhy as BrowseApi's Reason writes it. */
const char* const FRIENDS_ONLY_REASON = "Its host's unchecked mods download only from a Steam friend";

static bool Starts(const char* text, const char* start) { return strncmp(text, start, strlen(start)) == 0; }

/* A row's few words: Verdict's, with a lobby that cannot be joined named by what keeps it out (the reason's start, as
 * BrowseApi writes it; the details say it whole). The row has room for about 20 characters. */
static std::string ShortVerdict(const HfBrowseLobby& l, ImU32* colour)
{
    std::string text = Verdict(l, colour);
    if (l.verdict == HF_LOBBY_BLOCKED) return "Blocked mod";
    if (l.verdict != HF_LOBBY_UNJOINABLE) return text;
    if (l.fix == HF_LOBBY_FIX_STRANGERS) return "Strangers off";
    if (l.fix == HF_LOBBY_FIX_EDITED_STATS) return "Edited stats";
    if (Starts(l.reason, FRIENDS_ONLY_REASON)) return "Friends only";
    if (Starts(l.reason, "Its host runs an older HellForge")) return "Host must update";
    if (Starts(l.reason, "Its host runs a newer HellForge")) return "Update HellForge";
    if (Starts(l.reason, "Its host runs another HellForge")) return "Other HellForge";
    if (Starts(l.reason, "It needs mods installed by hand")) return "Needs mods";
    return "Can't join";
}

/* "+N more", the count Fit (LiveBrowse.Fit) puts in place of the items that did not fit: N, else -1. */
static int MoreCount(const std::string& item)
{
    int n = 0, used = 0;
    return sscanf(item.c_str(), "+%d more%n", &n, &used) == 1 && used == (int)item.size() && n > 0 ? n : -1;
}

/* ---- the look: the CONTROL CENTER wall screen's (blue blueprint, white text), full screen ----
 * The browser fills the screen as the terminal's own screen would: a blue field with a faint grid and scanlines, the
 * screen's header and corner marks, filters on the left, the lobbies in the middle and the selected one's details on
 * the right. It powers on (a line that opens into the screen, then its contents) and off the same way. */
const ImU32 TERM_BG = C(10, 34, 92, 246), TERM_BG2 = C(18, 64, 150, 120), TERM_GRID = C(150, 190, 255, 16), TERM_SCAN = C(0, 0, 0, 22),
    TERM_LINE = C(205, 228, 255, 150), TERM_FAINT = C(170, 200, 245, 150), TERM_TEXT = C(240, 246, 255, 255), TERM_DIM = C(196, 214, 245, 255),
    TERM_ROW = C(255, 255, 255, 14), TERM_SELECT = C(255, 255, 255, 44), TERM_GOOD = C(150, 240, 190, 255), TERM_WARN = C(255, 214, 120, 255),
    TERM_BAD = C(255, 150, 140, 255);
const double LOBBY_POWER_ON = 0.55, LOBBY_POWER_OFF = 0.3;

static ImU32 Alpha(ImU32 colour, float a)
{
    unsigned alpha = (unsigned)((float)((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f));
    return (colour & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

/* The screen's own button: a frame that lights when hovered; true when clicked (and armed). */
static bool TermButton(const std::string& label, const Area& a, bool on, bool enabled, uint64_t id, float alpha)
{
    bool hovered = enabled && g_cv.Hovered() == id;
    g_cv.Rect(a.x, a.y, a.w, a.h, Alpha(on ? C(255, 255, 255, 60) : hovered ? C(255, 255, 255, 30) : C(255, 255, 255, 8), alpha), 11);
    g_cv.Frame(a.x, a.y, a.w, a.h, Alpha(enabled ? TERM_LINE : TERM_FAINT, alpha * (enabled ? 1.0f : 0.5f)), 11);
    g_cv.Text(label, a.x + a.w / 2, a.y + a.h / 2, T(SZ_SMALL, Alpha(enabled ? TERM_TEXT : TERM_FAINT, alpha), ui::TITLE, ui::CENTER, a.w - 12, 12));
    return enabled && g_cv.Hit(a.x, a.y, a.w, a.h, id) && LobbiesArmed();
}

/* What joining a lobby takes, in a few words, and its colour on the screen. */
static std::string TermVerdict(const HfBrowseLobby& l, ImU32* colour)
{
    std::string text = Verdict(l, colour);
    *colour = *colour == OK ? TERM_GOOD : *colour == GOLD || *colour == PENDING ? TERM_WARN : *colour == ERROR_ ? TERM_BAD : TERM_DIM;
    return text;
}

/* A row's words on the screen: out of reach in the screen's dim text, not as an error, as most lobbies may be. */
static std::string TermShortVerdict(const HfBrowseLobby& l, ImU32* colour)
{
    std::string text = ShortVerdict(l, colour);
    *colour = OutOfReach(l) ? TERM_DIM : *colour == OK ? TERM_GOOD : *colour == GOLD || *colour == PENDING ? TERM_WARN : *colour == ERROR_ ? TERM_BAD : TERM_DIM;
    return text;
}

/* The field, its grid and scanlines, the frame and the corner marks, over the whole screen. */
static void DrawTermField(float x, float y, float w, float h, float alpha)
{
    g_cv.Rect(x, y, w, h, Alpha(TERM_BG, alpha), 8);
    g_cv.Rect(x, y + h * 0.55f, w, h * 0.45f, Alpha(TERM_BG2, alpha), 8); /* the screen's lighter lower half */
    for (float gx = x + 40; gx < x + w; gx += 40) g_cv.Rect(gx, y, 1, h, Alpha(TERM_GRID, alpha), 8);
    for (float gy = y + 40; gy < y + h; gy += 40) g_cv.Rect(x, gy, w, 1, Alpha(TERM_GRID, alpha), 8);
    for (float sy = y; sy < y + h; sy += 4) g_cv.Rect(x, sy, w, 1, Alpha(TERM_SCAN, alpha), 9);
}

static void DrawTermHeader(float x, float y, float w, float alpha, double t)
{
    const std::string title = "CONTROL CENTER  \xC2\xB7  LOBBY BROWSER";
    size_t typed = std::min(title.size(), (size_t)std::max(0.0, (t - LOBBY_POWER_ON * 0.6) * 90));
    while (typed < title.size() && typed > 0 && ((unsigned char)title[typed] & 0xC0) == 0x80) typed++; /* whole characters */
    float tw = g_cv.Text(title.substr(0, typed), x, y + 14, T(SZ_LABEL, Alpha(TERM_TEXT, alpha), ui::TITLE, ui::LEFT, 0, 12));
    g_cv.Rect(x + tw + 14, y + 14, w - tw - 14, 2, Alpha(TERM_LINE, alpha), 11);
    const HfBrowseState& s = g_lobbies.state;
    size_t strangers = std::count_if(g_lobbies.lobbies.begin(), g_lobbies.lobbies.end(), [](const HfBrowseLobby& l) { return l.fix == HF_LOBBY_FIX_STRANGERS; });
    std::string what = s.state == HF_BROWSE_NONE ? "Connecting to the ship's network..." : s.state == HF_BROWSE_UNAVAILABLE ? s.message :
        g_lobbies.searchStuck ? "The search had no answer. REFRESH to try again." :
        s.message[0] ? s.message : strangers > 0 ? std::to_string(strangers) + " of these lobbies are strangers': Play with strangers is off (select one, then A)." :
        "Every HellForge lobby on any planet, and what joining it takes.";
    bool bad = s.state == HF_BROWSE_UNAVAILABLE || s.state == HF_BROWSE_FAILED || g_lobbies.searchStuck;
    g_cv.Text(what, x, y + 42, T(SZ_SMALL, Alpha(bad ? TERM_BAD : TERM_DIM, alpha), ui::BODY,
        ui::LEFT, w * 0.6f, 12));
    std::string status = std::to_string(g_lobbies.shown.size()) + " of " + std::to_string(g_lobbies.lobbies.size()) + " lobbies";
    if (g_lobbies.hidden > 0) status += "  \xC2\xB7  " + std::to_string(g_lobbies.hidden) + " you can't join hidden (H)";
    if (g_lobbies.listedAt > 0)
    {
        int ago = (int)(Now() - g_lobbies.listedAt);
        status += ago < 2 ? "  \xC2\xB7  searched just now" : "  \xC2\xB7  searched " + std::to_string(ago) + " s ago";
    }
    g_cv.Text(Searching() ? "SEARCHING THE NETWORK..." : status, x + w, y + 42, T(SZ_SMALL, Alpha(Searching() ? TERM_WARN : TERM_FAINT, alpha), ui::BODY, ui::RIGHT, 0, 12));
}

/* The filters, a column of the screen's buttons, and REFRESH under them. */
static void DrawTermFilters(float x, float y, float w, float alpha)
{
    g_cv.Text("FILTERS", x, y + 8, T(SZ_TINY, Alpha(TERM_FAINT, alpha), ui::TITLE, ui::LEFT, 0, 12));
    if (TermButton("JOINABLE ONLY", {x, y + 24, w, 34}, g_lobbies.joinableOnly, true, ui::Id("lobbyjoinable"), alpha)) ToggleJoinable();
    if (TermButton("FRIENDS", {x, y + 66, w, 34}, g_lobbies.friendsOnly, true, ui::Id("lobbyfriends"), alpha)) ToggleFriends();
    std::string difficulty = g_lobbies.difficulty ? "DIFFICULTY " + std::to_string(g_lobbies.difficulty) + "+" : std::string("ANY DIFFICULTY");
    if (TermButton(difficulty, {x, y + 108, w, 34}, g_lobbies.difficulty > 0, true, ui::Id("lobbydifficulty"), alpha)) NextDifficulty();
    /* lit while lobbies out of reach show; nothing to change when there are none, or nothing else to show */
    std::string cant = "CAN'T JOIN (" + std::to_string(g_lobbies.unreachable) + ")";
    bool canHide = g_lobbies.unreachable > 0 && g_lobbies.unreachable < (int)g_lobbies.shown.size() + g_lobbies.hidden;
    if (TermButton(cant, {x, y + 150, w, 34}, g_lobbies.showUnreachable && canHide, canHide, ui::Id("lobbyunreachable"), alpha)) ToggleUnreachable();
    bool searching = Searching(), quick = g_lobbies.joining == QUICK_JOIN;
    if (TermButton(quick ? "FINDING A LOBBY..." : "QUICK PLAY", {x, y + 212, w, 48}, quick, g_lobbies.joining.empty(), ui::Id("lobbyquick"), alpha)) Quick();
    g_cv.Text("Joins the best lobby you can", x, y + 272, T(SZ_TINY, Alpha(TERM_FAINT, alpha), ui::BODY, ui::LEFT, w, 12));
    if (TermButton(searching ? "SEARCHING..." : "REFRESH", {x, y + 304, w, 40}, false, !searching, ui::Id("lobbyrefresh"), alpha)) Refresh();
    if (TermButton("CLOSE", {x, y + 354, w, 40}, false, true, ui::Id("lobbyclose"), alpha)) PowerOff();
}

static void DrawTermList(float x, float y, float w, float h, float alpha)
{
    g_cv.Text("HOST", x + 16, y + 8, T(SZ_TINY, Alpha(TERM_FAINT, alpha), ui::TITLE, ui::LEFT, 0, 12));
    g_cv.Text("PLAYERS", x + w - 420, y + 8, T(SZ_TINY, Alpha(TERM_FAINT, alpha), ui::TITLE, ui::LEFT, 0, 12));
    g_cv.Text("DIFF.", x + w - 340, y + 8, T(SZ_TINY, Alpha(TERM_FAINT, alpha), ui::TITLE, ui::LEFT, 0, 12));
    g_cv.Text("JOINING", x + w - 270, y + 8, T(SZ_TINY, Alpha(TERM_FAINT, alpha), ui::TITLE, ui::LEFT, 0, 12));
    g_cv.Rect(x, y + 20, w, 1, Alpha(TERM_LINE, alpha), 11);
    ListOpts o;
    o.x = x; o.y = y + 26; o.w = w; o.h = h - 26; o.rowH = 48; o.count = (int)g_lobbies.shown.size(); o.selected = g_lobbies.index;
    o.key = "lobbies"; o.focused = true; o.layer = 12;
    o.click = [&](int i) { g_lobbies.index = i; };
    o.draw = [&](int i, float rx, float ry, float rw, float rh, bool hovered) {
        const HfBrowseLobby& l = g_lobbies.lobbies[g_lobbies.shown[i]];
        bool selected = i == g_lobbies.index;
        g_cv.Rect(rx, ry + 3, rw - 8, rh - 6, Alpha(selected ? TERM_SELECT : hovered ? C(255, 255, 255, 24) : TERM_ROW, alpha), 10);
        if (selected) g_cv.Rect(rx, ry + 3, 3, rh - 6, Alpha(TERM_TEXT, alpha), 11);
        float cy = ry + rh / 2, lx = rx + 16, right = rx + rw - 420;
        if (l.flags & HF_LOBBY_FLAG_FRIEND) lx += Chip("FRIEND", lx, cy, Alpha(C(10, 34, 92, 255), alpha), Alpha(TERM_TEXT, alpha), false, 12) + 8;
        float nw = g_cv.Text(l.host[0] ? l.host : "A HellForge lobby", lx, cy, T(SZ_LABEL, Alpha(TERM_TEXT, alpha), ui::BODY, ui::LEFT, (right - lx) * 0.7f, 12));
        if (l.flags & HF_LOBBY_FLAG_MISSION) g_cv.Text("on a mission", lx + nw + 10, cy, T(SZ_SMALL, Alpha(TERM_FAINT, alpha), ui::BODY, ui::LEFT, right - lx - nw - 20, 12));
        g_cv.Text(std::to_string(l.members) + " / " + std::to_string(l.max_members), rx + rw - 420, cy, T(SZ_SMALL, Alpha(TERM_DIM, alpha), ui::BODY, ui::LEFT, 70, 12));
        g_cv.Text(l.difficulty > 0 ? std::to_string(l.difficulty) : "-", rx + rw - 340, cy, T(SZ_SMALL, Alpha(TERM_DIM, alpha), ui::BODY, ui::LEFT, 60, 12));
        ImU32 colour;
        std::string verdict = TermShortVerdict(l, &colour);
        g_cv.Text(g_lobbies.joining == l.lobby_id ? "Joining..." : verdict, rx + rw - 270, cy, T(SZ_SMALL, Alpha(colour, alpha), ui::BODY, ui::LEFT, 260, 12));
    };
    List(o);
    if (g_lobbies.shown.empty())
    {
        const char* none = Searching() || g_lobbies.state.state == HF_BROWSE_NONE ? "Searching the network for lobbies..." :
            g_lobbies.lobbies.empty() ? "No HellForge lobbies right now. Host one yourself, or look again in a moment." : "No lobby fits the filters.";
        g_cv.Text(none, x + 16, y + 56, T(SZ_LABEL, Alpha(TERM_DIM, alpha), ui::BODY, ui::LEFT, w - 32, 12));
    }
}

/* The items of a list separated by `separator`, at most `limit` of them. */
static std::vector<std::string> Items(const char* text, const char* separator, size_t limit)
{
    std::vector<std::string> items;
    std::string all = text;
    for (size_t at = 0; at < all.size() && items.size() < limit;)
    {
        size_t next = all.find(separator, at);
        items.push_back(all.substr(at, next == std::string::npos ? std::string::npos : next - at));
        at = next == std::string::npos ? all.size() : next + strlen(separator);
    }
    return items;
}

/* A column of the details panel: wrapped lines from `y` down (it ends under them). */
struct TermColumn
{
    float x, y, w, alpha;
    void Lines(const std::string& text, int size, int step, ImU32 colour)
    {
        for (std::string& line : Wrapped(text, w, size)) { g_cv.Text(line, x, y, T(size, Alpha(colour, alpha), ui::BODY, ui::LEFT, w, 12)); y += (float)step; }
    }
};

/* The lobby's mod_list from ly down to limit: every item it holds (BrowseApi fits whole names into it), as many as fit,
 * then the rest and the backend's "+N more" as one count. */
static void TermMods(const HfBrowseLobby& l, float x, float ly, float w, float limit, float alpha)
{
    bool inTheWay = l.verdict == HF_LOBBY_UNJOINABLE || l.verdict == HF_LOBBY_BLOCKED;
    g_cv.Text(inTheWay ? "MODS IN THE WAY" : "MODS A JOIN WOULD CHANGE", x, ly, T(SZ_TINY, Alpha(TERM_FAINT, alpha), ui::TITLE, ui::LEFT, 0, 12));
    ly += 22;
    std::vector<std::string> items = Items(l.mod_list, "; ", 64);
    int more = items.empty() ? 0 : std::max(0, MoreCount(items.back()));
    if (more > 0) items.pop_back();
    size_t drawn = 0;
    for (; drawn < items.size(); drawn++)
    {
        bool countAfter = more > 0 || drawn + 1 < items.size(); /* a line kept for the count, should one follow */
        if (ly + 20 + (countAfter ? 20 : 0) > limit) break;
        g_cv.Text(items[drawn], x + 8, ly, T(SZ_SMALL, Alpha(TERM_DIM, alpha), ui::BODY, ui::LEFT, w - 8, 12));
        ly += 20;
    }
    more += (int)(items.size() - drawn);
    if (more > 0) g_cv.Text("+" + std::to_string(more) + " more", x + 8, ly, T(SZ_SMALL, Alpha(TERM_FAINT, alpha), ui::BODY, ui::LEFT, w - 8, 12));
}

/* The selected lobby: its host, players and what joining takes, mod by mod, and JOIN. */
static void DrawTermDetail(float x, float y, float w, float h, float alpha)
{
    g_cv.Rect(x, y, w, h, Alpha(C(255, 255, 255, 10), alpha), 10);
    g_cv.Frame(x, y, w, h, Alpha(TERM_LINE, alpha * 0.6f), 11);
    const HfBrowseLobby* l = Selected();
    if (l == nullptr) { g_cv.Text("Select a lobby.", x + 20, y + 30, T(SZ_SMALL, Alpha(TERM_FAINT, alpha), ui::BODY, ui::LEFT, w - 40, 12)); return; }
    float ly = y + 30;
    g_cv.Text(l->host[0] ? l->host : "A HellForge lobby", x + 20, ly, T(SZ_TITLE, Alpha(TERM_TEXT, alpha), ui::TITLE, ui::LEFT, w - 40, 12));
    ly += 32;
    std::string facts = std::to_string(l->members) + " of " + std::to_string(l->max_members) + " players";
    if (l->difficulty > 0) facts += "  \xC2\xB7  difficulty " + std::to_string(l->difficulty);
    facts += l->flags & HF_LOBBY_FLAG_MISSION ? "  \xC2\xB7  on a mission" : "  \xC2\xB7  on the ship";
    g_cv.Text(facts, x + 20, ly, T(SZ_SMALL, Alpha(TERM_DIM, alpha), ui::BODY, ui::LEFT, w - 40, 12));
    ly += 22;
    if (l->players[0]) { g_cv.Text(l->players, x + 20, ly, T(SZ_SMALL, Alpha(TERM_FAINT, alpha), ui::BODY, ui::LEFT, w - 40, 12)); ly += 22; }
    ly += 12;
    ImU32 colour;
    TermColumn column{x + 20, ly, w - 40, alpha};
    column.Lines(TermVerdict(*l, &colour), SZ_LABEL, 24, colour);
    if (l->reason[0] && l->verdict != HF_LOBBY_UNJOINABLE && l->verdict != HF_LOBBY_BLOCKED) column.Lines(l->reason, SZ_SMALL, 20, TERM_DIM);
    if (l->verdict == HF_LOBBY_UNJOINABLE && Starts(l->reason, FRIENDS_ONLY_REASON))
    {
        column.y += 6;
        column.Lines("To join, install these mods yourself (the launcher's Mods page), or play with the host as a Steam friend.", SZ_SMALL, 20, TERM_DIM);
    }
    ly = column.y + 14;
    if (l->mod_list[0]) TermMods(*l, x + 20, ly, w - 40, y + h - (l->fix != HF_LOBBY_FIX_NONE ? 140 : 90), alpha);
    else if (l->mods > 0)
        g_cv.Text(std::to_string(l->mods) + " shared mod(s), all the same as yours", x + 20, ly, T(SZ_SMALL, Alpha(TERM_FAINT, alpha), ui::BODY, ui::LEFT, w - 40, 12));
    if (l->fix != HF_LOBBY_FIX_NONE)
    {
        const char* fix = l->fix == HF_LOBBY_FIX_STRANGERS ? "TURN ON PLAY WITH STRANGERS" : "SHOW EDITED-STATS LOBBIES";
        if (TermButton(fix, {x + 20, y + h - 116, w - 40, 40}, false, true, ui::Id("lobbyfix"), alpha)) Fix(l);
    }
    bool joinable = Joinable(l);
    if (TermButton(!g_lobbies.joining.empty() ? "JOINING..." : "JOIN", {x + 20, y + h - 64, w - 40, 44}, joinable, joinable, ui::Id("lobbyjoin"), alpha)) Join(l);
}

/* The warning before Play with strangers goes on (the launcher's), over the screen: TURN ON (Y) or CANCEL (N, Esc). */
static void DrawTermConfirm(float X, float Y, float W, float H, float alpha)
{
    g_cv.Rect(X, Y, W, H, Alpha(C(0, 8, 30, 170), alpha), 13);
    float w = std::min(640.0f, W - 80), x = X + (W - w) / 2;
    std::vector<std::string> lines = Wrapped(STRANGER_WARNING, w - 48, SZ_LABEL);
    float h = 150 + 24 * (float)lines.size(), y = Y + (H - h) / 2;
    g_cv.Rect(x, y, w, h, Alpha(TERM_BG, alpha), 13);
    g_cv.Frame(x, y, w, h, Alpha(TERM_WARN, alpha), 14);
    g_cv.Text("TURN ON PLAY WITH STRANGERS?", x + 24, y + 30, T(SZ_TITLE, Alpha(TERM_WARN, alpha), ui::TITLE, ui::LEFT, w - 48, 14));
    float ly = y + 66;
    for (const std::string& line : lines) { g_cv.Text(line, x + 24, ly, T(SZ_LABEL, Alpha(TERM_TEXT, alpha), ui::BODY, ui::LEFT, w - 48, 14)); ly += 24; }
    float bw = (w - 64) / 2;
    if (TermButton("TURN ON (Y)", {x + 24, y + h - 60, bw, 40}, false, true, ui::Id("lobbystrangerson"), alpha)) TurnOnStrangers();
    if (TermButton("CANCEL (N)", {x + 40 + bw, y + h - 60, bw, 40}, false, true, ui::Id("lobbystrangerscancel"), alpha)) { g_lobbies.confirmStrangers = false; Sound(sounds::BACK); }
}

/* Esc or CLOSE: the screen powers off, then the browser goes (LobbyEscape's work, after the animation). */
static void PowerOff()
{
    if (g_lobbies.closingAt > 0) return;
    g_lobbies.closingAt = Now(); /* LobbyEscape plays the sound */
}

void DrawLobbies()
{
    if (!g_lobbies.on || g_lobbies.camera) return;
    double t = Now() - g_lobbies.shownAt;
    float open = (float)std::clamp(t / LOBBY_POWER_ON, 0.0, 1.0);
    if (g_lobbies.closingAt > 0)
    {
        double c = Now() - g_lobbies.closingAt;
        if (c >= LOBBY_POWER_OFF) { g_lobbies.closingAt = 0; LobbyEscape(); return; }
        open = std::min(open, 1.0f - (float)(c / LOBBY_POWER_OFF));
    }
    float X = -g_cv.ox, Y = -g_cv.oy, W = g_cv.width, H = g_cv.height;
    /* power on: a bright line across the middle opens into the screen; the contents fade in after it */
    float lineOpen = std::clamp(open / 0.45f, 0.0f, 1.0f), fieldOpen = std::clamp((open - 0.3f) / 0.4f, 0.0f, 1.0f), contents = std::clamp((open - 0.6f) / 0.4f, 0.0f, 1.0f);
    g_cv.Block();
    float fh = H * fieldOpen;
    if (fh >= 2) DrawTermField(X, Y + (H - fh) / 2, W, fh, 1.0f);
    if (fieldOpen < 1) g_cv.Rect(X + W * (1 - lineOpen) / 2, Y + H / 2 - 1, W * lineOpen, 3, C(230, 240, 255, (int)(255 * (1 - fieldOpen))), 12);
    if (contents <= 0) return;
    float m = std::max(48.0f, W * 0.04f), cx = X + m, cy = Y + m * 0.8f, cw = W - 2 * m;
    DrawTermHeader(cx, cy, cw, contents, t);
    float top = cy + 76, bottom = Y + H - m * 0.8f - 28, filterW = 220, detailW = std::min(460.0f, cw * 0.3f);
    DrawTermFilters(cx, top, filterW, contents);
    DrawTermList(cx + filterW + 24, top, cw - filterW - detailW - 48, bottom - top, contents);
    DrawTermDetail(cx + cw - detailW, top, detailW, bottom - top, contents);
    if (g_lobbies.confirmStrangers) DrawTermConfirm(X, Y, W, H, contents);
    g_cv.Text("Up/Down select   Enter join   Q quick play   A allow   R refresh   J joinable only   F friends   D difficulty   H can't join   Esc close", cx, bottom + 16,
        T(SZ_TINY, Alpha(TERM_FAINT, contents), ui::BODY, ui::LEFT, 0, 12));
    g_cv.Text("INTERNAL NETWORK - TT_HD: ID_LOBBIES", cx + cw, bottom + 16, T(SZ_TINY, Alpha(TERM_FAINT, contents), ui::BODY, ui::RIGHT, 0, 12));
}
} // namespace editorapp
