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
 * player accepts in game, so the browser closes once it is there. A placeholder: Echelon's own browser replaces it. */
LobbyDialog g_lobbies;
/* The key or click that opened it (the terminal's interact key) must not answer it: its keys wait this long. */
const double LOBBY_ARM = 0.5;
/* While it shows, a new list every this many seconds (the backend searches at most once every 2 s, whoever asks). */
const double LOBBY_REFRESH = 10;
static bool LobbiesArmed() { return ImGui::GetTime() - g_lobbies.shownAt >= LOBBY_ARM; }

bool HasBrowse() { return g_host.api->size >= offsetof(HfApi, browse_ask) + sizeof(void*) && g_host.api->browse_state != nullptr; }

static void AskList()
{
    g_lobbies.askedAt = ImGui::GetTime();
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
    if (on) g_lobbies.shownAt = ImGui::GetTime();
    sounds::EditorOpen(on || g_sync.alone);
    g_host.api->log(g_host.mod, on ? "editor: the lobby browser shows over the game, the window closed" : "editor: the lobby browser goes");
    if (on) Sound(sounds::SHOW);
}

void OpenLobbies(const char* why)
{
    if (!HasBrowse()) { if (g_windowShown) Toast("This HellForge has no lobby browser (SDK 16)", ERROR_); return; }
    g_host.api->log(g_host.mod, "editor: the lobby browser opens (%s)", why);
    bool fresh = !g_lobbies.on;
    g_lobbies.on = true;
    g_lobbies.joining.clear();
    g_picker.on = false;
    g_edit.on = false;
    if (fresh) { g_lobbies.shownAt = ImGui::GetTime(); g_seen.erase("lobbies"); }
    AskList();
    if (!g_windowShown) ShowLobbiesAlone(true);
    else if (fresh) Sound(sounds::OPEN);
}

static void CloseLobbies()
{
    g_lobbies.on = false;
    g_lobbies.joining.clear();
    ShowLobbiesAlone(false);
}

/* The lobbies the filters leave; the selection kept on the same lobby where it can be. */
static void Filter()
{
    std::string selected = g_lobbies.index < (int)g_lobbies.shown.size() ? g_lobbies.lobbies[g_lobbies.shown[g_lobbies.index]].lobby_id : "";
    g_lobbies.shown.clear();
    for (int i = 0; i < (int)g_lobbies.lobbies.size(); i++)
    {
        const HfBrowseLobby& l = g_lobbies.lobbies[i];
        if (g_lobbies.joinableOnly && !(l.flags & HF_LOBBY_FLAG_JOINABLE)) continue;
        if (g_lobbies.friendsOnly && !(l.flags & HF_LOBBY_FLAG_FRIEND)) continue;
        if (g_lobbies.difficulty > 0 && l.difficulty < g_lobbies.difficulty) continue;
        if (l.lobby_id == selected) g_lobbies.index = (int)g_lobbies.shown.size();
        g_lobbies.shown.push_back(i);
    }
    g_lobbies.index = std::clamp(g_lobbies.index, 0, std::max(0, (int)g_lobbies.shown.size() - 1));
}

static void ReadLobbyList(uint32_t count)
{
    g_lobbies.lobbies.clear();
    for (uint32_t i = 0; i < count; i++)
    {
        HfBrowseLobby lobby{};
        lobby.size = sizeof lobby;
        if (!g_host.api->browse_lobby(g_host.mod, i, &lobby)) break;
        g_lobbies.lobbies.push_back(lobby);
    }
    Filter();
}

/* The backend's answer to a join: invited, the browser goes so the player can accept the invitation in game. */
static void JoinAnswered(const HfBrowseState& now)
{
    if (g_lobbies.joining.empty()) return;
    if (now.state == HF_BROWSE_INVITED)
    {
        g_host.api->log(g_host.mod, "editor: lobby browser: invited to %s; the browser closes for the game's invitation", g_lobbies.joining.c_str());
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

/* Every 15 frames (Tick): the terminal's count (it opens the browser), the list when it changed, and a new list every
 * LOBBY_REFRESH seconds while it shows. */
void ReadLobbies()
{
    static int frames;
    if (!HasBrowse() || ++frames < 15) return;
    frames = 0;
    HfBrowseState now{};
    now.size = sizeof now;
    g_host.api->browse_state(g_host.mod, &now);
    if (!g_lobbies.terminalRead) { g_lobbies.terminalRead = true; g_lobbies.terminal = now.terminal; }
    else if (now.terminal != g_lobbies.terminal) { g_lobbies.terminal = now.terminal; OpenLobbies("the ship's information terminal"); }
    HfBrowseState& was = g_lobbies.state;
    bool turned = now.version != was.version || now.state != was.state;
    if (turned || strcmp(now.message, was.message))
    {
        if (now.version != was.version || now.lobbies != g_lobbies.lobbies.size()) ReadLobbyList(now.lobbies);
        g_lobbies.state = now;
        if (turned) JoinAnswered(now);
    }
    if (g_lobbies.on && g_lobbies.joining.empty() && now.state != HF_BROWSE_SEARCHING && ImGui::GetTime() - g_lobbies.askedAt >= LOBBY_REFRESH) AskList();
}

static const HfBrowseLobby* Selected()
{
    return g_lobbies.index < (int)g_lobbies.shown.size() ? &g_lobbies.lobbies[g_lobbies.shown[g_lobbies.index]] : nullptr;
}

static bool Joinable(const HfBrowseLobby* l) { return l != nullptr && (l->flags & HF_LOBBY_FLAG_JOINABLE) && g_lobbies.joining.empty(); }

static void Join(const HfBrowseLobby* l)
{
    if (!Joinable(l) || !LobbiesArmed()) return;
    if (!g_host.api->browse_ask(g_host.mod, HF_BROWSE_JOIN, l->lobby_id)) { Toast("The session's backend is not connected", ERROR_); return; }
    g_lobbies.joining = l->lobby_id;
    Sound(sounds::CLICK);
}

static void Refresh()
{
    if (g_lobbies.state.state == HF_BROWSE_SEARCHING) return;
    AskList();
    Sound(sounds::CLICK);
}

static void ToggleJoinable() { g_lobbies.joinableOnly = !g_lobbies.joinableOnly; Filter(); Sound(sounds::CLICK); }
static void ToggleFriends() { g_lobbies.friendsOnly = !g_lobbies.friendsOnly; Filter(); Sound(sounds::CLICK); }
/* any, then 5 and up to 10 and up, then any again */
static void NextDifficulty() { g_lobbies.difficulty = g_lobbies.difficulty == 0 ? 5 : g_lobbies.difficulty >= 10 ? 0 : g_lobbies.difficulty + 1; Filter(); Sound(sounds::CLICK); }

/* Esc: the window closes (Closed() closes the browser too), or the browser goes. */
static void LobbyEscape()
{
    if (g_windowShown) g_host.close();
    else { CloseLobbies(); Sound(sounds::BACK); }
}

void LobbyKeys()
{
    int n = (int)g_lobbies.shown.size();
    if (Pressed(ImGuiKey_UpArrow)) g_lobbies.index = std::max(0, g_lobbies.index - 1);
    if (Pressed(ImGuiKey_DownArrow)) g_lobbies.index = std::min(std::max(0, n - 1), g_lobbies.index + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { LobbyEscape(); return; }
    if (!LobbiesArmed()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) Join(Selected());
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) Refresh();
    if (ImGui::IsKeyPressed(ImGuiKey_J, false)) ToggleJoinable();
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) ToggleFriends();
    if (ImGui::IsKeyPressed(ImGuiKey_D, false)) NextDifficulty();
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

/* The title, what the backend says, and the filters (each a toggle). */
static void DrawLobbiesHead(const Area& box)
{
    float x = box.x, y = box.y, w = box.w;
    const HfBrowseState& s = g_lobbies.state;
    g_cv.Rect(x, y, w, 3, GOLD, 10);
    float tw = g_cv.Text("LOBBY BROWSER", x + 24, y + 30, T(18, GOLD, ui::TITLE, ui::LEFT, 0, 11));
    std::string what = s.state == HF_BROWSE_NONE ? "Waiting for the session's backend..." : s.state == HF_BROWSE_UNAVAILABLE ? s.message :
        s.message[0] ? s.message : "Every HellForge lobby, and what joining it takes.";
    g_cv.Text(what, x + 36 + tw, y + 31, T(SZ_SMALL, s.state == HF_BROWSE_UNAVAILABLE || s.state == HF_BROWSE_FAILED ? ERROR_ : DIM, ui::BODY, ui::LEFT, w - tw - 80, 11));
    float fx = x + 24, fy = y + 52;
    auto toggle = [&](const std::string& label, bool on, const char* id) {
        float bw = g_cv.Measure(label, SZ_TINY, ui::TITLE) + 24;
        bool clicked = Button(label, {fx, fy, bw, 24}, on ? PRIMARY : NORMAL, ui::Id(id), 11) && LobbiesArmed();
        fx += bw + 8;
        return clicked;
    };
    if (toggle("JOINABLE ONLY", g_lobbies.joinableOnly, "lobbyjoinable")) ToggleJoinable();
    if (toggle("FRIENDS", g_lobbies.friendsOnly, "lobbyfriends")) ToggleFriends();
    if (toggle(g_lobbies.difficulty ? "DIFFICULTY " + std::to_string(g_lobbies.difficulty) + "+" : std::string("ANY DIFFICULTY"), g_lobbies.difficulty > 0, "lobbydifficulty"))
        NextDifficulty();
    char count[64];
    snprintf(count, sizeof count, "%d of %d lobbies", (int)g_lobbies.shown.size(), (int)g_lobbies.lobbies.size());
    g_cv.Text(s.state == HF_BROWSE_SEARCHING ? "Searching..." : count, x + w - 24, fy + 12, T(SZ_SMALL, FAINT, ui::BODY, ui::RIGHT, 0, 11));
    float hy = y + 86;
    g_cv.Rect(x + 1, hy, w - 2, 22, HEADER, 10);
    g_cv.Text("HOST", x + 24, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::LEFT, 0, 11));
    g_cv.Text("PLAYERS", x + w - 560, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::LEFT, 0, 11));
    g_cv.Text("DIFFICULTY", x + w - 480, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::LEFT, 0, 11));
    g_cv.Text("JOINING", x + w - 380, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::LEFT, 0, 11));
}

static void DrawLobbyList(const Area& box, float top, float bottom)
{
    ListOpts o;
    o.x = box.x + 2; o.y = top; o.w = box.w - 4; o.h = bottom - top; o.rowH = 40; o.count = (int)g_lobbies.shown.size(); o.selected = g_lobbies.index;
    o.key = "lobbies"; o.focused = true; o.layer = 12;
    o.click = [&](int i) { g_lobbies.index = i; };
    o.draw = [&](int i, float rx, float ry, float rw, float rh, bool hovered) {
        const HfBrowseLobby& l = g_lobbies.lobbies[g_lobbies.shown[i]];
        bool selected = i == g_lobbies.index;
        if (selected) { g_cv.Rect(rx + 10, ry + 2, rw - 20, rh - 4, SELECT, 10); g_cv.Rect(rx + 10, ry + 2, 3, rh - 4, GOLD, 11); }
        else if (hovered) g_cv.Rect(rx + 10, ry + 2, rw - 20, rh - 4, HOVER, 10);
        float cy = ry + rh / 2, lx = rx + 22, right = rx + rw - 560;
        if (l.flags & HF_LOBBY_FLAG_FRIEND) lx += Chip("FRIEND", lx, cy, INVERSE, MOD, false, 12) + 8;
        float nw = g_cv.Text(l.host[0] ? l.host : "A HellForge lobby", lx, cy, T(SZ_LABEL, selected ? TEXT : DIM, ui::BODY, ui::LEFT, (right - lx) * 0.75f, 12));
        if (l.flags & HF_LOBBY_FLAG_MISSION) g_cv.Text("on a mission", lx + nw + 10, cy, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, right - lx - nw - 20, 12));
        g_cv.Text(std::to_string(l.members) + " / " + std::to_string(l.max_members), rx + rw - 558, cy, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, 70, 12));
        g_cv.Text(l.difficulty > 0 ? std::to_string(l.difficulty) : "-", rx + rw - 478, cy, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, 90, 12));
        ImU32 colour;
        std::string verdict = Verdict(l, &colour);
        g_cv.Text(g_lobbies.joining == l.lobby_id ? "Joining..." : verdict, rx + rw - 378, cy, T(SZ_SMALL, colour, ui::BODY, ui::LEFT, 360, 12));
    };
    List(o);
}

/* REFRESH, JOIN and CLOSE, from the right. */
static void DrawLobbyButtons(const Area& box)
{
    float by = box.y + box.h - 66, bx = box.x + box.w - 24 - 130;
    if (Button("CLOSE", {bx, by, 130, 30}, NORMAL, ui::Id("lobbyclose"), 11)) { LobbyEscape(); return; }
    const HfBrowseLobby* selected = Selected();
    bool joinable = Joinable(selected) && LobbiesArmed();
    const char* join = !g_lobbies.joining.empty() ? "JOINING..." : "JOIN";
    bx -= 12 + 150;
    if (Button(join, {bx, by, 150, 30}, joinable ? PRIMARY : DISABLED, ui::Id("lobbyjoin"), 11) && joinable) Join(selected);
    bx -= 12 + 130;
    bool searching = g_lobbies.state.state == HF_BROWSE_SEARCHING;
    if (Button(searching ? "SEARCHING" : "REFRESH", {bx, by, 130, 30}, searching ? DISABLED : NORMAL, ui::Id("lobbyrefresh"), 11) && !searching && LobbiesArmed()) Refresh();
    /* beside the buttons: why the selected lobby cannot be joined, in full */
    if (selected != nullptr && !(selected->flags & HF_LOBBY_FLAG_JOINABLE) && selected->reason[0])
    {
        auto lines = Wrapped(selected->reason, bx - box.x - 48, SZ_SMALL);
        for (size_t i = 0; i < lines.size() && i < 2; i++)
            g_cv.Text(lines[i], box.x + 24, by + 6 + 18.0f * (float)i, T(SZ_SMALL, ERROR_, ui::BODY, ui::LEFT, bx - box.x - 36, 11));
    }
}

void DrawLobbies()
{
    if (!g_lobbies.on) return;
    if (g_lobbies.alone) g_cv.Rect(-g_cv.ox, -g_cv.oy, g_cv.width, g_cv.height, C(0, 0, 0, 150), 8); /* the game, dimmed */
    else g_cv.Rect(0, 0, PANEL_W, g_panelH, C(0, 0, 0, 120), 8);
    float w = std::min(1000.0f, PANEL_W - 60), h = std::min(g_panelH - 80, 660.0f), x = (PANEL_W - w) / 2, y = (g_panelH - h) / 2;
    Area box{x, y, w, h};
    g_cv.Block();
    g_cv.Rect(x, y, w, h, PANEL_ALT, 9);
    g_cv.Frame(x, y, w, h, LINE_STRONG, 10);
    DrawLobbiesHead(box);
    float top = y + 112, bottom = y + h - 84;
    DrawLobbyList(box, top, bottom);
    if (g_lobbies.shown.empty())
    {
        const char* none = g_lobbies.state.state == HF_BROWSE_SEARCHING || g_lobbies.state.state == HF_BROWSE_NONE ? "Looking for lobbies..." :
            g_lobbies.lobbies.empty() ? "No HellForge lobbies right now. Host one yourself, or look again in a moment." : "No lobby fits the filters.";
        g_cv.Text(none, x + 28, top + 24, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, w - 56, 12));
    }
    g_cv.Rect(x + 12, bottom + 4, w - 24, 1, LINE, 10);
    DrawLobbyButtons(box);
    g_cv.Text("\xE2\x86\x91\xE2\x86\x93 select   Enter join   R refresh   J joinable only   F friends   D difficulty   Esc close", x + 24, y + h - 20,
        T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, 0, 11));
}
} // namespace editorapp
