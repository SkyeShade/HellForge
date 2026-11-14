/* The editor's window (app_internal.h): the lobby browser. */
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "app_internal.h"
#include "icons.h"

namespace editorapp
{
/* ---- the lobby browser (SDK 16): every HellForge lobby the session's backend finds ----
 * The backend lists every lobby of its directory, with no planet or difficulty filter, and friends' lobbies, each with
 * what joining it would take (docs/multiplayer.md "The lobby browser"); the loader hands the list over (browse_state,
 * browse_lobby) and the player's asks back (browse_ask). The ship's information terminal opens it (the loader counts
 * each time, browse_state's terminal), and so does Ctrl+L in the window. Like the MOD SYNC prompt it shows alone over the
 * game, holding its input, or in the window when that is open. A join hands the game the lobby's invitation, which the
 * player accepts in game, so the browser closes once it is there. A mod that draws a browser of its own
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
    g_lobbies.detail = false;
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
    g_lobbies.detail = false;
    g_lobbies.confirmRelays = false;
    g_lobbies.afterRelays.clear();
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

static void Join(const HfBrowseLobby* l);
static void Quick();

/* Whether co-op over Steam relays keeps every lobby out (Linux: the backend gives each the fix). */
static bool RelaysOff()
{
    return std::any_of(g_lobbies.lobbies.begin(), g_lobbies.lobbies.end(), [](const HfBrowseLobby& l) { return l.fix == HF_LOBBY_FIX_RELAYS; });
}

/* The backend's answer to turning co-op over Steam relays on: a new list without the fix (on: the join the player tried
 * goes on), or failed (the message says why, and that the next session has it). */
static void RelaysAnswered(const HfBrowseState& now, bool turned)
{
    if (!g_lobbies.relaysAsked || !turned) return;
    if (now.state == HF_BROWSE_FAILED)
    {
        g_lobbies.relaysAsked = false;
        g_lobbies.afterRelays.clear();
        Toast(now.message[0] ? now.message : "Co-op over Steam relays could not be turned on", ERROR_);
        Sound(sounds::BACK);
        return;
    }
    if (now.state != HF_BROWSE_READY || RelaysOff()) return;
    g_lobbies.relaysAsked = false;
    g_host.api->log(g_host.mod, "editor: lobby browser: co-op over Steam relays is on");
    Toast("Co-op over Steam relays is on: lobbies can be joined.", OK);
    std::string after = g_lobbies.afterRelays;
    g_lobbies.afterRelays.clear();
    if (after == QUICK_JOIN) Quick();
    else if (!after.empty())
        for (const HfBrowseLobby& l : g_lobbies.lobbies)
            if (after == l.lobby_id) { Join(&l); break; }
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
        RelaysAnswered(now, turned);
    }
    GiveUpUnanswered(now);
    if (g_lobbies.on && g_lobbies.joining.empty() && !Searching() && Now() - g_lobbies.askedAt >= LOBBY_REFRESH) AskList();
}

static bool Joinable(const HfBrowseLobby* l) { return l != nullptr && (l->flags & HF_LOBBY_FLAG_JOINABLE) && g_lobbies.joining.empty(); }

/* While co-op over Steam relays is off (Linux), a join or Quick Play asks to turn it on first, and goes on once it is. */
static bool AskRelaysFirst(const std::string& after)
{
    if (!RelaysOff() || !g_lobbies.joining.empty() || g_lobbies.relaysAsked) return false;
    g_lobbies.confirmRelays = true;
    g_lobbies.afterRelays = after;
    Sound(sounds::CLICK);
    return true;
}

static void Join(const HfBrowseLobby* l)
{
    if (l != nullptr && l->fix == HF_LOBBY_FIX_RELAYS && LobbiesArmed() && AskRelaysFirst(l->lobby_id)) return;
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
    if (AskRelaysFirst(QUICK_JOIN)) return;
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

/* What turning co-op over Steam relays on opens (Linux), before the player says yes. */
const char* const RELAYS_QUESTION = "Joining other players goes through Valve's Steam relays, which this session can't reach yet. Turning co-op "
    "over Steam relays on lets this session's network reach Valve's relay servers and nothing else: UDP to them only, while every "
    "other address stays blocked. The launcher keeps it on (Settings > MULTIPLAYER).";

static void Fix(const HfBrowseLobby* l)
{
    if (l == nullptr || !LobbiesArmed()) return;
    if (l->fix == HF_LOBBY_FIX_RELAYS) AskRelaysFirst("");
    else if (l->fix == HF_LOBBY_FIX_STRANGERS) { g_lobbies.confirmStrangers = true; Sound(sounds::CLICK); }
    else if (l->fix == HF_LOBBY_FIX_EDITED_STATS)
    {
        if (!g_host.api->browse_ask(g_host.mod, HF_BROWSE_SHOW_EDITED_STATS, nullptr)) { Toast("The session's backend is not connected", ERROR_); return; }
        Toast("Lobbies with host-enforced stat changes show for this session. Joining one, you play with the host's stats.", OK);
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

static void TurnOnRelays()
{
    g_lobbies.confirmRelays = false;
    if (!g_host.api->browse_ask(g_host.mod, HF_BROWSE_TURN_ON_RELAYS, nullptr))
    {
        g_lobbies.afterRelays.clear();
        Toast("The session's backend is not connected", ERROR_);
        return;
    }
    g_host.api->log(g_host.mod, "editor: lobby browser: the player turned co-op over Steam relays on%s%s",
        g_lobbies.afterRelays.empty() ? "" : ", to join ", g_lobbies.afterRelays.c_str());
    g_lobbies.relaysAsked = true;
    g_lobbies.askedAt = Now();
    Toast("Turning on co-op over Steam relays...", OK);
    Sound(sounds::APPLY);
}

static void CancelRelays()
{
    g_lobbies.confirmRelays = false;
    g_lobbies.afterRelays.clear();
    Sound(sounds::BACK);
}

static void CancelStrangers() { g_lobbies.confirmStrangers = false; Sound(sounds::BACK); }

/* A question over the screen before a setting goes on: TURN ON (Y) or CANCEL (N, Esc). Play with strangers' warning (the
 * launcher's; Y only), or what co-op over Steam relays opens (Enter too: it continues the join the player asked for). */
struct TermQuestion
{
    const char* title;
    const char* text;
    bool warning; /* amber, a warning; else the browser's red */
    bool enterSaysYes;
    void (*yes)();
    void (*no)();
};

/* The question that shows, or null. */
static const TermQuestion* OpenQuestion()
{
    static const TermQuestion strangers{"TURN ON PLAY WITH STRANGERS?", STRANGER_WARNING, true, false, TurnOnStrangers, CancelStrangers};
    static const TermQuestion relays{"TURN ON CO-OP OVER STEAM RELAYS?", RELAYS_QUESTION, false, true, TurnOnRelays, CancelRelays};
    if (g_lobbies.confirmStrangers) return &strangers;
    if (g_lobbies.confirmRelays) return &relays;
    return nullptr;
}

static void QuestionKeys(const TermQuestion& q)
{
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_N, false)) q.no();
    else if (ImGui::IsKeyPressed(ImGuiKey_Y, false) || (q.enterSaysYes && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) q.yes();
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

/* The pop-up's keys: Esc back to the cards, Enter joins, A the way in. */
static void LobbyDetailKeys()
{
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { g_lobbies.detail = false; Sound(sounds::BACK); return; }
    if (!LobbiesArmed()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) Join(Selected());
    if (ImGui::IsKeyPressed(ImGuiKey_A, false)) Fix(Selected());
}

/* The cards two by two: left and right one card, up and down a row, Page Up and Page Down a page. */
static void LobbyMoveKeys()
{
    int n = (int)g_lobbies.shown.size(), per = std::max(2, g_lobbies.perPage), at = g_lobbies.index;
    if (Pressed(ImGuiKey_LeftArrow)) at -= 1;
    if (Pressed(ImGuiKey_RightArrow)) at += 1;
    if (Pressed(ImGuiKey_UpArrow)) at -= 2;
    if (Pressed(ImGuiKey_DownArrow)) at += 2;
    if (Pressed(ImGuiKey_PageUp)) at -= per;
    if (Pressed(ImGuiKey_PageDown)) at += per;
    g_lobbies.index = std::clamp(at, 0, std::max(0, n - 1));
}

void LobbyKeys()
{
    if (g_lobbies.camera) return;
    if (const TermQuestion* q = OpenQuestion()) { QuestionKeys(*q); return; }
    if (g_lobbies.detail) { LobbyDetailKeys(); return; }
    LobbyMoveKeys();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { PowerOff(); return; }
    if (!LobbiesArmed()) return;
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && Selected() != nullptr) { g_lobbies.detail = true; Sound(sounds::CLICK); }
    /* Space is Quick Play (the hint strip's); Q too, the key the placeholder used */
    static const struct { ImGuiKey key; void (*action)(); } keys[] = {
        {ImGuiKey_R, Refresh}, {ImGuiKey_J, ToggleJoinable}, {ImGuiKey_F, ToggleFriends}, {ImGuiKey_H, ToggleUnreachable},
        {ImGuiKey_D, NextDifficulty}, {ImGuiKey_Space, Quick}, {ImGuiKey_Q, Quick}, {ImGuiKey_A, [] { Fix(Selected()); }}};
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

/* The reason's start for a stranger's unchecked mods: ModSync.SandboxOnlyWhy as BrowseApi's Reason writes it. */
const char* const SANDBOX_ONLY_REASON = "Its host's unchecked mods need a sandboxed game";

static bool Starts(const char* text, const char* start) { return strncmp(text, start, strlen(start)) == 0; }

/* A row's few words: Verdict's, with a lobby that cannot be joined named by what keeps it out (the reason's start, as
 * BrowseApi writes it; the details say it whole). The row has room for about 20 characters. */
static std::string ShortVerdict(const HfBrowseLobby& l, ImU32* colour)
{
    std::string text = Verdict(l, colour);
    if (l.verdict == HF_LOBBY_BLOCKED) return "Blocked mod";
    if (l.verdict != HF_LOBBY_UNJOINABLE) return text;
    if (l.fix == HF_LOBBY_FIX_STRANGERS) return "Strangers off";
    if (l.fix == HF_LOBBY_FIX_EDITED_STATS) return "Host-enforced stats";
    if (l.fix == HF_LOBBY_FIX_RELAYS) return "Co-op relays off";
    if (Starts(l.reason, SANDBOX_ONLY_REASON)) return "Needs sandbox";
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

/* ---- the look: the game's own Social menu, in HellForge red, full screen ----
 * Laid out like the game's Social menu so it feels native on the terminal: the HellForge mark and MATCHMAKING over a
 * tab, the status, categories and filters on the left, two columns of lobby cards in pages in the middle, QUICK PLAY
 * and the colour key on the right, and the game's black hint strip along the bottom. HellForge red where the game uses
 * yellow; every card has a coloured edge for what joining it takes. A card opens the lobby's pop-up (its squad, what a
 * join changes, the fix for one that can't be joined, JOIN); square pop-ups with a red-hatched header. It powers on (a
 * line that opens into the screen, then its contents) and off the same way. */
const ImU32 LB_BG = C(11, 12, 14, 248), LB_HATCH = C(255, 255, 255, 5), LB_CARD = C(22, 23, 26, 236), LB_CARD_ON = C(32, 33, 37, 246),
    LB_TEXT = C(236, 235, 232, 255), LB_DIM = C(141, 143, 147, 255), LB_MUTE = C(92, 95, 100, 255),
    LB_RED = C(224, 53, 43, 255), LB_RED_DARK = C(163, 35, 28, 255), LB_RED_HATCH = C(224, 53, 43, 70), LB_OK = C(106, 208, 138, 255),
    LB_AMBER = C(242, 178, 60, 255), LB_AMBER_HATCH = C(242, 178, 60, 60), LB_FRIEND = C(240, 141, 132, 255), LB_POP = C(20, 21, 24, 252),
    LB_SHADE = C(0, 0, 0, 158), LB_STRIP = C(0, 0, 0, 255), LB_PANEL = C(70, 72, 78, 140);
const double LOBBY_POWER_ON = 0.55, LOBBY_POWER_OFF = 0.3;
const float LB_CARD_H = 92, LB_GAP = 12;

static ImU32 Alpha(ImU32 colour, float a)
{
    unsigned alpha = (unsigned)((float)((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f));
    return (colour & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

/* The game's angled hatching over a box: thin lines leaning right, every `step` units, cut at the box's sides. */
static void Hatch(const Area& a, ImU32 colour, float step, int z)
{
    float dx = a.h * 0.47f;
    for (float u = a.x - dx; u < a.x + a.w; u += step)
    {
        float x1 = std::max(u, a.x), x2 = std::min(u + dx, a.x + a.w);
        if (x2 <= x1) continue;
        float y1 = a.y + a.h - (x1 - u) / dx * a.h, y2 = a.y + a.h - (x2 - u) / dx * a.h;
        g_cv.Line(x1, y1, x2, y2, colour, z, 1.2f);
    }
}

/* A key in the game's style (a white box around its name); returns its width. */
static float KeyCap(const std::string& key, float x, float cy, float alpha, int z)
{
    float w = g_cv.Measure(key, SZ_SMALL, ui::TITLE) + 12;
    g_cv.Frame(x, cy - 11, w, 22, Alpha(LB_TEXT, alpha), z, 2);
    g_cv.Text(key, x + w / 2, cy, T(SZ_SMALL, Alpha(LB_TEXT, alpha), ui::TITLE, ui::CENTER, 0, z + 1));
    return w;
}

static float HintWidth(const std::string& key, const std::string& label)
{
    return g_cv.Measure(key, SZ_SMALL, ui::TITLE) + 12 + 10 + g_cv.Measure(label, SZ_LABEL, ui::TITLE);
}

/* A hint of the strip (the key, then what it does), clickable; true when clicked (and armed). */
struct Hint { std::string key, label; ImU32 colour; bool enabled; uint64_t id; };
static bool DrawHint(const Hint& h, float x, float cy, float alpha, int z)
{
    float a = h.enabled ? alpha : alpha * 0.35f;
    float kw = KeyCap(h.key, x, cy, a, z);
    bool hovered = h.enabled && g_cv.Hovered() == h.id;
    g_cv.Text(h.label, x + kw + 10, cy, T(SZ_LABEL, Alpha(hovered ? LB_RED : h.colour, a), ui::TITLE, ui::LEFT, 0, z + 1));
    return h.enabled && g_cv.Hit(x, cy - 14, HintWidth(h.key, h.label), 28, h.id) && LobbiesArmed();
}

/* Hints laid out from the right edge leftwards; returns the index of the one clicked, or -1. */
static int DrawHintsRight(const std::vector<Hint>& hints, float right, float cy, float alpha, int z)
{
    int clicked = -1;
    float x = right;
    for (int i = (int)hints.size() - 1; i >= 0; i--)
    {
        x -= HintWidth(hints[i].key, hints[i].label);
        if (DrawHint(hints[i], x, cy, alpha, z)) clicked = i;
        x -= 30;
    }
    return clicked;
}

/* What joining takes, as the card's edge and verdict colour: ready green, a download or trust amber, out red, the rest
 * (full, locked, a friend's not known yet) grey. */
static ImU32 Tone(const HfBrowseLobby& l)
{
    switch (l.verdict)
    {
    case HF_LOBBY_READY: return LB_OK;
    case HF_LOBBY_DOWNLOAD: case HF_LOBBY_TRUST: return LB_AMBER;
    case HF_LOBBY_UNJOINABLE: case HF_LOBBY_BLOCKED: return LB_RED;
    default: return LB_MUTE;
    }
}

/* Where the lobby is: its planet (by index until the backend names planets) and whether it is on a mission. */
static std::string Where(const HfBrowseLobby& l)
{
    bool mission = (l.flags & HF_LOBBY_FLAG_MISSION) != 0;
    if (l.planet >= 0) return "Planet #" + std::to_string(l.planet) + (mission ? std::string(SEP) + "on a mission" : std::string());
    return mission ? "On a mission" : "On the ship";
}

static std::string Upper(std::string s)
{
    for (char& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

/* The status box at the top of the left column: what the backend says, or what this list is. */
static void DrawLobbyStatus(const Area& a, float alpha)
{
    const HfBrowseState& s = g_lobbies.state;
    size_t strangers = std::count_if(g_lobbies.lobbies.begin(), g_lobbies.lobbies.end(), [](const HfBrowseLobby& l) { return l.fix == HF_LOBBY_FIX_STRANGERS; });
    std::string what = s.state == HF_BROWSE_NONE ? "Connecting to the ship's network..." : s.state == HF_BROWSE_UNAVAILABLE ? s.message :
        g_lobbies.searchStuck ? "The search had no answer. Press R to try again." :
        s.message[0] ? s.message : strangers > 0 ? std::to_string(strangers) + " of these lobbies are strangers': Play with strangers is off." :
        "Every HellForge lobby on any planet, and what joining it takes.";
    bool bad = s.state == HF_BROWSE_UNAVAILABLE || s.state == HF_BROWSE_FAILED || g_lobbies.searchStuck;
    std::vector<std::string> lines = Wrapped(what, a.w - 32, SZ_LABEL);
    if (lines.size() > 3) lines.resize(3);
    float h = std::max(a.h, 20 + 22 * (float)lines.size());
    g_cv.Rect(a.x, a.y, a.w, h, Alpha(LB_PANEL, alpha), 10);
    float ly = a.y + 10 + 11;
    for (const std::string& line : lines) { g_cv.Text(line, a.x + 16, ly, Shown(T(SZ_LABEL, Alpha(bad ? LB_RED : LB_DIM, alpha), ui::BODY, ui::LEFT, a.w - 32, 11))); ly += 22; }
}

/* A category row (ALL LOBBIES, FRIENDS): red and white-edged when it is the one shown; true when clicked. */
static bool DrawCategory(const std::string& label, int count, const Area& a, bool on, uint64_t id, float alpha)
{
    bool hovered = g_cv.Hovered() == id;
    if (on)
    {
        g_cv.Rect(a.x, a.y, a.w, a.h, Alpha(LB_RED_DARK, alpha), 10);
        g_cv.Rect(a.x, a.y, a.w * 0.5f, a.h, Alpha(LB_RED, alpha), 10);
        g_cv.Rect(a.x, a.y, 4, a.h, Alpha(LB_TEXT, alpha), 11);
    }
    else g_cv.Rect(a.x, a.y, a.w, a.h, Alpha(hovered ? LB_CARD_ON : LB_CARD, alpha), 10);
    g_cv.Text(label, a.x + 18, a.y + a.h / 2, T(SZ_LABEL + 2, Alpha(on ? LB_TEXT : LB_DIM, alpha), ui::TITLE, ui::LEFT, a.w - 80, 12));
    g_cv.Text(std::to_string(count), a.x + a.w - 16, a.y + a.h / 2, T(SZ_LABEL + 2, Alpha(LB_TEXT, alpha), ui::TITLE, ui::RIGHT, 0, 12));
    return g_cv.Hit(a.x, a.y, a.w, a.h, id) && LobbiesArmed();
}

/* A filter row: its name and key, then a box ticked red, or a value in red; true when clicked. */
static bool DrawFilter(const std::string& label, const std::string& value, bool on, const Area& a, uint64_t id, float alpha)
{
    bool hovered = g_cv.Hovered() == id;
    g_cv.Rect(a.x, a.y, a.w, a.h, Alpha(hovered ? LB_CARD_ON : LB_CARD, alpha), 10);
    g_cv.Text(label, a.x + 16, a.y + a.h / 2, T(SZ_LABEL, Alpha(LB_TEXT, alpha), ui::BODY, ui::LEFT, a.w - 90, 12));
    if (!value.empty()) g_cv.Text(value, a.x + a.w - 16, a.y + a.h / 2, T(SZ_LABEL, Alpha(on ? LB_RED : LB_MUTE, alpha), ui::TITLE, ui::RIGHT, 0, 12));
    else
    {
        float bx = a.x + a.w - 32, by = a.y + a.h / 2 - 8;
        if (on) g_cv.Rect(bx, by, 16, 16, Alpha(LB_RED, alpha), 11);
        g_cv.Frame(bx, by, 16, 16, Alpha(on ? LB_RED : LB_DIM, alpha), 12, 2);
    }
    return g_cv.Hit(a.x, a.y, a.w, a.h, id) && LobbiesArmed();
}

/* The left column: the status, the count, the categories and the filters. */
static void DrawLobbyLeft(const Area& a, float alpha)
{
    DrawLobbyStatus({a.x, a.y, a.w, 48}, alpha);
    float y = a.y + 96;
    int joinable = (int)std::count_if(g_lobbies.lobbies.begin(), g_lobbies.lobbies.end(), [](const HfBrowseLobby& l) { return (l.flags & HF_LOBBY_FLAG_JOINABLE) != 0; });
    float jw = g_cv.Text(std::to_string(joinable) + " JOINABLE", a.x + a.w, y - 22, T(SZ_LABEL, Alpha(LB_RED, alpha), ui::TITLE, ui::RIGHT, 0, 12));
    g_cv.Text(std::to_string(g_lobbies.lobbies.size()) + " FOUND", a.x + a.w - jw - 14, y - 22, T(SZ_LABEL, Alpha(LB_TEXT, alpha), ui::TITLE, ui::RIGHT, 0, 12));
    int friends = (int)std::count_if(g_lobbies.lobbies.begin(), g_lobbies.lobbies.end(), [](const HfBrowseLobby& l) { return (l.flags & HF_LOBBY_FLAG_FRIEND) != 0; });
    if (DrawCategory("ALL LOBBIES", (int)g_lobbies.lobbies.size(), {a.x, y, a.w, 46}, !g_lobbies.friendsOnly, ui::Id("lobbyall"), alpha) && g_lobbies.friendsOnly) ToggleFriends();
    if (DrawCategory("FRIENDS  [F]", friends, {a.x, y + 54, a.w, 46}, g_lobbies.friendsOnly, ui::Id("lobbyfriends"), alpha) && !g_lobbies.friendsOnly) ToggleFriends();
    y += 126;
    g_cv.Text("FILTERS", a.x, y, T(SZ_SMALL, Alpha(LB_MUTE, alpha), ui::TITLE, ui::LEFT, 0, 12));
    y += 16;
    if (DrawFilter("Joinable only  [J]", "", g_lobbies.joinableOnly, {a.x, y, a.w, 40}, ui::Id("lobbyjoinable"), alpha)) ToggleJoinable();
    std::string difficulty = g_lobbies.difficulty ? std::to_string(g_lobbies.difficulty) + "+" : std::string("ANY");
    if (DrawFilter("Lowest difficulty  [D]", difficulty, g_lobbies.difficulty > 0, {a.x, y + 48, a.w, 40}, ui::Id("lobbydifficulty"), alpha)) NextDifficulty();
    /* lit while lobbies out of reach show; nothing to change when there are none, or nothing else to show */
    bool canHide = g_lobbies.unreachable > 0 && g_lobbies.unreachable < (int)g_lobbies.shown.size() + g_lobbies.hidden;
    std::string cant = "Show can't join (" + std::to_string(g_lobbies.unreachable) + ")  [H]";
    if (DrawFilter(cant, "", g_lobbies.showUnreachable && canHide, {a.x, y + 96, a.w, 40}, ui::Id("lobbyunreachable"), canHide ? alpha : alpha * 0.4f) && canHide) ToggleUnreachable();
    if (g_lobbies.hidden > 0)
        g_cv.Text(std::to_string(g_lobbies.hidden) + " you can't join are hidden", a.x, y + 156, T(SZ_SMALL, Alpha(LB_MUTE, alpha), ui::BODY, ui::LEFT, a.w, 12));
}

/* One lobby's card: the host (a friend's in red) and difficulty, where it is and its squad, then what joining takes. */
static void DrawLobbyCard(const HfBrowseLobby& l, int i, const Area& a, float alpha)
{
    uint64_t id = ui::Id("lobbycard", (uint64_t)i + 1);
    bool selected = i == g_lobbies.index, hovered = g_cv.Hovered() == id, friendly = (l.flags & HF_LOBBY_FLAG_FRIEND) != 0;
    float fade = alpha * (OutOfReach(l) || l.verdict == HF_LOBBY_FULL || l.verdict == HF_LOBBY_LOCKED ? 0.5f : 1.0f);
    g_cv.Rect(a.x, a.y, a.w, a.h, Alpha(selected || hovered ? LB_CARD_ON : LB_CARD, fade), 10);
    g_cv.Rect(a.x, a.y, 4, a.h, Alpha(Tone(l), fade), 11);
    if (selected) g_cv.Frame(a.x, a.y, a.w, a.h, Alpha(LB_TEXT, alpha), 12, 2);
    float lx = a.x + 18, rx = a.x + a.w - 14;
    std::string diff = l.difficulty > 0 ? "D" + std::to_string(l.difficulty) : std::string("D?");
    float dw = g_cv.Measure(diff, SZ_SMALL, ui::TITLE) + 12;
    g_cv.Frame(rx - dw, a.y + 12, dw, 20, Alpha(LB_DIM, fade), 11);
    g_cv.Text(diff, rx - dw / 2, a.y + 22, T(SZ_SMALL, Alpha(LB_TEXT, fade), ui::TITLE, ui::CENTER, 0, 12));
    g_cv.Text(l.host[0] ? l.host : "A HellForge lobby", lx, a.y + 22, T(SZ_LABEL + 5, Alpha(friendly ? LB_RED : LB_TEXT, fade), ui::TITLE, ui::LEFT, a.w - dw - 50, 12));
    std::string squad = std::to_string(l.members) + "/" + std::to_string(l.max_members);
    float sw = g_cv.Text(squad, rx, a.y + 48, T(SZ_LABEL + 1, Alpha(LB_TEXT, fade), ui::TITLE, ui::RIGHT, 0, 12));
    g_cv.Text("SQUAD", rx - sw - 8, a.y + 48, T(SZ_TINY, Alpha(LB_DIM, fade), ui::TITLE, ui::RIGHT, 0, 12));
    g_cv.Text(Where(l), lx, a.y + 48, T(SZ_LABEL, Alpha(friendly ? LB_FRIEND : LB_DIM, fade), ui::BODY, ui::LEFT, a.w - sw - 100, 12));
    ImU32 colour;
    std::string verdict = g_lobbies.joining == l.lobby_id ? std::string("Joining...") : ShortVerdict(l, &colour);
    if (friendly && l.verdict == HF_LOBBY_UNKNOWN) verdict = "Friend" + std::string(SEP) + "mods known on joining";
    g_cv.Text(Upper(verdict), lx, a.y + 73, T(SZ_SMALL, Alpha(Tone(l), fade), ui::TITLE, ui::LEFT, a.w - 32, 12));
    if (g_cv.Hit(a.x, a.y, a.w, a.h, id) && LobbiesArmed()) { g_lobbies.index = i; g_lobbies.detail = true; Sound(sounds::CLICK); }
}

/* The page bar under the cards: a segment per page, the current one red, and ‹ page / pages ›. */
static void DrawLobbyPages(const Area& a, int page, int pages, float alpha)
{
    float segW = (a.w - 6 * (float)(pages - 1)) / (float)pages;
    for (int p = 0; p < pages; p++) g_cv.Rect(a.x + (segW + 6) * (float)p, a.y, segW, 6, Alpha(p == page ? LB_RED : C(61, 63, 68, 255), alpha), 10);
    float cx = a.x + a.w / 2, cy = a.y + 30;
    std::string text = std::to_string(page + 1) + " / " + std::to_string(pages);
    float tw = g_cv.Text(text, cx, cy, T(SZ_LABEL + 2, Alpha(LB_RED, alpha), ui::TITLE, ui::CENTER, 0, 12));
    int per = std::max(1, g_lobbies.perPage), n = (int)g_lobbies.shown.size();
    if (page > 0) g_cv.Text("\xE2\x80\xB9", cx - tw / 2 - 20, cy, T(SZ_TITLE, Alpha(LB_DIM, alpha), ui::BODY, ui::CENTER, 0, 12));
    if (page + 1 < pages) g_cv.Text("\xE2\x80\xBA", cx + tw / 2 + 20, cy, T(SZ_TITLE, Alpha(LB_DIM, alpha), ui::BODY, ui::CENTER, 0, 12));
    if (page > 0 && g_cv.Hit(cx - tw / 2 - 40, cy - 14, 40, 28, ui::Id("lobbypageback")) && LobbiesArmed()) { g_lobbies.index = (page - 1) * per; Sound(sounds::CLICK); }
    if (page + 1 < pages && g_cv.Hit(cx + tw / 2, cy - 14, 40, 28, ui::Id("lobbypagenext")) && LobbiesArmed()) { g_lobbies.index = std::min(n - 1, (page + 1) * per); Sound(sounds::CLICK); }
}

/* The middle: the lobbies two by two, a page of them at a time (the page the selection is on), and the page bar. */
static void DrawLobbyGrid(const Area& a, float alpha)
{
    int rows = std::max(1, (int)floorf((a.h - 50 + LB_GAP) / (LB_CARD_H + LB_GAP)));
    int per = rows * 2, n = (int)g_lobbies.shown.size();
    g_lobbies.perPage = per;
    int pages = std::max(1, (n + per - 1) / per), page = std::clamp(g_lobbies.index / per, 0, pages - 1);
    float wheel = g_cv.Scroll(a.x, a.y, a.w, a.h, ui::Id("lobbygrid"));
    if (wheel != 0 && n > 0) g_lobbies.index = std::clamp(g_lobbies.index - (wheel > 0 ? per : -per), 0, n - 1);
    float cw = (a.w - 16) / 2;
    for (int k = 0; k < per && page * per + k < n; k++)
    {
        int i = page * per + k;
        DrawLobbyCard(g_lobbies.lobbies[g_lobbies.shown[i]], i, {a.x + (float)(k % 2) * (cw + 16), a.y + (float)(k / 2) * (LB_CARD_H + LB_GAP), cw, LB_CARD_H}, alpha);
    }
    if (n == 0)
    {
        const char* none = Searching() || g_lobbies.state.state == HF_BROWSE_NONE ? "Searching the network for lobbies..." :
            g_lobbies.lobbies.empty() ? "No HellForge lobbies right now. Host one yourself, or look again in a moment." : "No lobby fits the filters.";
        g_cv.Rect(a.x, a.y, a.w, LB_CARD_H, Alpha(LB_CARD, alpha), 10);
        g_cv.Text(none, a.x + 20, a.y + LB_CARD_H / 2, T(SZ_LABEL + 2, Alpha(LB_DIM, alpha), ui::BODY, ui::LEFT, a.w - 40, 12));
    }
    DrawLobbyPages({a.x, a.y + (float)rows * (LB_CARD_H + LB_GAP) + 6, a.w, 40}, page, pages, alpha);
}

/* The right column: QUICK PLAY (red-hatched) and the key to the cards' colours. */
static void DrawLobbyRight(const Area& a, float alpha)
{
    bool quick = g_lobbies.joining == QUICK_JOIN, enabled = g_lobbies.joining.empty();
    uint64_t id = ui::Id("lobbyquick");
    bool hovered = enabled && g_cv.Hovered() == id;
    Area q{a.x, a.y, a.w, 84};
    g_cv.Rect(q.x, q.y, q.w, q.h, Alpha(hovered ? C(224, 53, 43, 60) : C(224, 53, 43, 22), alpha), 10);
    Hatch(q, Alpha(LB_RED_HATCH, alpha), 10, 10);
    g_cv.Frame(q.x, q.y, q.w, q.h, Alpha(LB_RED, enabled ? alpha : alpha * 0.5f), 11, 2);
    g_cv.Text(quick ? "FINDING A LOBBY..." : "QUICK PLAY", q.x + q.w / 2, q.y + 32, T(SZ_TITLE, Alpha(LB_TEXT, alpha), ui::TITLE, ui::CENTER, q.w - 16, 12));
    g_cv.Text("Joins the best lobby for you", q.x + q.w / 2, q.y + 60, T(SZ_SMALL, Alpha(LB_DIM, alpha), ui::BODY, ui::CENTER, q.w - 16, 12));
    if (enabled && g_cv.Hit(q.x, q.y, q.w, q.h, id) && LobbiesArmed()) Quick();
    float y = a.y + 100;
    static const struct { ImU32 colour; const char* text; } key[] = {
        {LB_OK, "Ready to join"}, {LB_AMBER, "Download or trust"}, {LB_RED, "Can't join"}, {LB_MUTE, "Full, locked or not known yet"}};
    g_cv.Rect(a.x, y, a.w, 22 + 26 * 4, Alpha(LB_CARD, alpha), 10);
    for (int i = 0; i < 4; i++)
    {
        float cy = y + 24 + 26 * (float)i;
        g_cv.Rect(a.x + 14, cy - 8, 4, 16, Alpha(key[i].colour, alpha), 11);
        g_cv.Text(key[i].text, a.x + 28, cy, T(SZ_SMALL, Alpha(LB_TEXT, alpha), ui::BODY, ui::LEFT, a.w - 40, 12));
    }
}

/* The top: the HellForge mark in its white box, MATCHMAKING, the LOBBIES tab, and the count and refresh on the right. */
static void DrawLobbyHeader(const Area& a, float alpha, double t)
{
    g_cv.Rect(a.x, a.y, 56, 56, Alpha(C(0, 0, 0, 255), alpha), 10);
    g_cv.Frame(a.x, a.y, 56, 56, Alpha(LB_TEXT, alpha), 11, 2);
    const icons::Icon* logo = icons::Logo();
    if (logo != nullptr && icons::Texture()) g_cv.Image(icons::Texture(), ImVec2(a.x + 6, a.y + 6), ImVec2(44, 44), logo->uv0, logo->uv1, Alpha(IM_COL32_WHITE, alpha), 12);
    else Logo(a.x + 10, a.y + 10, 36);
    const std::string title = "MATCHMAKING";
    size_t typed = std::min(title.size(), (size_t)std::max(0.0, (t - LOBBY_POWER_ON * 0.6) * 60));
    g_cv.Text(title.substr(0, typed), a.x + 74, a.y + 28, T(40, Alpha(LB_TEXT, alpha), ui::TITLE, ui::LEFT, 0, 12));
    Area tab{a.x + 74, a.y + 66, 190, 40};
    g_cv.Frame(tab.x, tab.y - 2, tab.w, tab.h + 2, Alpha(LB_TEXT, alpha), 11, 2);
    g_cv.Rect(tab.x - 2, tab.y - 4, tab.w + 4, 4, Alpha(LB_BG, 1), 12); /* the tab hangs from the line: no top edge */
    g_cv.Text("LOBBIES", tab.x + 14, tab.y + 14, T(SZ_LABEL + 4, Alpha(LB_TEXT, alpha), ui::TITLE, ui::LEFT, 0, 12));
    Hatch({tab.x + 8, tab.y + tab.h - 12, tab.w - 40, 6}, Alpha(LB_RED, alpha), 5, 12);
    int friends = (int)std::count_if(g_lobbies.lobbies.begin(), g_lobbies.lobbies.end(), [](const HfBrowseLobby& l) { return (l.flags & HF_LOBBY_FLAG_FRIEND) != 0; });
    std::string badge = Searching() ? "SEARCHING THE NETWORK..." : "REFRESHES EVERY " + std::to_string((int)LOBBY_REFRESH) + " S";
    float bw = g_cv.Measure(badge, SZ_LABEL + 1, ui::TITLE) + 32;
    Area b{a.x + a.w - bw, a.y + 10, bw, 38};
    g_cv.Rect(b.x, b.y, b.w, b.h, Alpha(C(224, 53, 43, 30), alpha), 10);
    Hatch(b, Alpha(LB_RED_HATCH, alpha), 8, 10);
    g_cv.Frame(b.x, b.y, b.w, b.h, Alpha(LB_RED, alpha), 11, 2);
    g_cv.Text(badge, b.x + b.w / 2, b.y + b.h / 2, T(SZ_LABEL + 1, Alpha(LB_TEXT, alpha), ui::TITLE, ui::CENTER, 0, 12));
    std::string count = std::to_string(g_lobbies.lobbies.size()) + " lobbies" + SEP + std::to_string(friends) + (friends == 1 ? " friend" : " friends");
    g_cv.Text(count, b.x - 20, b.y + b.h / 2, T(SZ_LABEL + 3, Alpha(LB_TEXT, alpha), ui::TITLE, ui::RIGHT, 0, 12));
}

/* The black strip along the bottom: ESC CLOSE on the left, R, SPACE and ENTER on the right, each clickable. */
static void DrawLobbyStrip(float X, float Y, float W, float H, float alpha)
{
    float h = std::max(56.0f, H * 0.07f), y = Y + H - h, m = std::max(48.0f, W * 0.03f), cy = y + h / 2;
    g_cv.Rect(X, y, W, h, Alpha(LB_STRIP, alpha), 10);
    if (DrawHint({"ESC", "CLOSE", LB_TEXT, true, ui::Id("lobbyclose")}, X + m, cy, alpha, 11)) PowerOff();
    bool searching = Searching(), detail = Selected() != nullptr;
    std::vector<Hint> hints = {{"R", searching ? "SEARCHING..." : "REFRESH", LB_TEXT, !searching, ui::Id("lobbyrefresh")},
        {"SPACE", "QUICK PLAY", LB_TEXT, g_lobbies.joining.empty(), ui::Id("lobbyquickhint")},
        {"ENTER", "SELECT", LB_TEXT, detail, ui::Id("lobbyselect")}};
    int clicked = DrawHintsRight(hints, X + W - m, cy, alpha, 11);
    if (clicked == 0) Refresh();
    else if (clicked == 1) Quick();
    else if (clicked == 2) { g_lobbies.detail = true; Sound(sounds::CLICK); }
}

/* A square pop-up's frame and header (a red-hatched band, or amber for a warning) over the dimmed screen; returns its
 * box. The header carries the title and, on the right, a few coloured words. */
struct PopHead { std::string title, note; ImU32 noteColour; bool warn; float bodyH; };
static Area DrawPopFrame(const PopHead& p, float X, float Y, float W, float H, float alpha)
{
    g_cv.Block();
    g_cv.Rect(X, Y, W, H, Alpha(LB_SHADE, alpha), 13);
    float w = std::clamp(W * 0.46f, 560.0f, 900.0f), h = 60 + p.bodyH + 56, x = X + (W - w) / 2, y = Y + (H - h) / 2;
    ImU32 accent = p.warn ? LB_AMBER : LB_RED;
    g_cv.Rect(x, y, w, h, Alpha(LB_POP, alpha), 13);
    g_cv.Rect(x, y, w, 60, Alpha(p.warn ? C(242, 178, 60, 26) : C(224, 53, 43, 30), alpha), 14);
    Hatch({x, y, w, 60}, Alpha(p.warn ? LB_AMBER_HATCH : LB_RED_HATCH, alpha), 10, 14);
    g_cv.Rect(x, y + 58, w, 2, Alpha(accent, alpha), 15);
    g_cv.Frame(x, y, w, h, Alpha(p.warn ? LB_AMBER : LB_TEXT, alpha), 15, 2);
    g_cv.Text(p.title, x + 22, y + 30, T(SZ_TITLE + 4, Alpha(p.warn ? LB_AMBER : LB_TEXT, alpha), ui::TITLE, ui::LEFT, w * 0.6f, 15));
    if (!p.note.empty()) g_cv.Text(Upper(p.note), x + w - 22, y + 30, T(SZ_SMALL, Alpha(p.noteColour, alpha), ui::TITLE, ui::RIGHT, w * 0.36f, 15));
    g_cv.Rect(x, y + h - 56, w, 56, Alpha(LB_STRIP, alpha), 14);
    return {x, y, w, h};
}

/* Lines of wrapped text down a column; returns where the next line goes. */
static float PopLines(const std::string& text, float x, float y, float w, ImU32 colour, float alpha)
{
    for (const std::string& line : Wrapped(text, w, SZ_LABEL)) { g_cv.Text(line, x, y, Shown(T(SZ_LABEL, Alpha(colour, alpha), ui::BODY, ui::LEFT, w, 15))); y += 22; }
    return y;
}

static float PopFact(const std::string& label, const std::string& value, ImU32 colour, float x, float y, float alpha)
{
    g_cv.Text(label, x, y, T(SZ_LABEL, Alpha(LB_DIM, alpha), ui::BODY, ui::LEFT, 110, 15));
    g_cv.Text(value, x + 124, y, T(SZ_LABEL, Alpha(colour, alpha), ui::BODY, ui::LEFT, 0, 15));
    return y + 24;
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

/* The mods a join changes (or those in the way), one row each with what happens to it as a tag; returns the bottom. */
static float PopMods(const HfBrowseLobby& l, float x, float y, float w, float alpha)
{
    bool inTheWay = l.verdict == HF_LOBBY_UNJOINABLE || l.verdict == HF_LOBBY_BLOCKED;
    std::string label = inTheWay ? "MODS IN THE WAY" : "WHAT JOINING CHANGES" + std::string(SEP) + "HOST RUNS " + std::to_string(l.mods) + " SHARED MODS";
    g_cv.Text(label, x, y, T(SZ_TINY, Alpha(LB_MUTE, alpha), ui::TITLE, ui::LEFT, w, 15));
    y += 16;
    std::vector<std::string> items = Items(l.mod_list, "; ", 6);
    for (const std::string& item : items)
    {
        size_t colon = item.rfind(": ");
        bool more = MoreCount(item) > 0;
        std::string name = colon == std::string::npos ? item : item.substr(0, colon), what = colon == std::string::npos ? "" : item.substr(colon + 2);
        g_cv.Rect(x, y, w, 30, Alpha(LB_CARD, alpha), 14);
        g_cv.Text(name, x + 12, y + 15, T(SZ_LABEL, Alpha(more ? LB_DIM : LB_TEXT, alpha), ui::BODY, ui::LEFT, w - 140, 15));
        if (!what.empty())
        {
            ImU32 tag = what == "download" || what == "trust" ? LB_AMBER : LB_DIM;
            float tw = g_cv.Measure(Upper(what), SZ_TINY, ui::TITLE) + 16;
            g_cv.Frame(x + w - 12 - tw, y + 6, tw, 18, Alpha(tag, alpha), 15);
            g_cv.Text(Upper(what), x + w - 12 - tw / 2, y + 15, T(SZ_TINY, Alpha(tag, alpha), ui::TITLE, ui::CENTER, 0, 15));
        }
        y += 34;
    }
    return y;
}

/* How tall the lobby pop-up's body is, so the pop-up fits its content. */
static float DetailBodyH(const HfBrowseLobby& l, float w)
{
    float h = 24 * 4 + 16;
    if (l.reason[0]) h += 22 * (float)Wrapped(l.reason, w, SZ_LABEL).size() + 10;
    if (l.mod_list[0]) h += 16 + 34 * (float)Items(l.mod_list, "; ", 6).size() + 8;
    if (l.fix != HF_LOBBY_FIX_NONE) h += 96;
    return h + 50;
}

/* The fix for a lobby a setting keeps out (A): an amber box with what it does and its key; true when clicked. */
static bool PopFix(const HfBrowseLobby& l, const Area& a, float alpha)
{
    uint64_t id = ui::Id("lobbyfix");
    bool hovered = g_cv.Hovered() == id;
    g_cv.Rect(a.x, a.y, a.w, a.h, Alpha(hovered ? C(242, 178, 60, 30) : C(242, 178, 60, 10), alpha), 14);
    g_cv.Frame(a.x, a.y, a.w, a.h, Alpha(LB_AMBER, alpha), 15, 2);
    g_cv.Text("A WAY IN", a.x + 14, a.y + 18, T(SZ_TINY, Alpha(LB_AMBER, alpha), ui::TITLE, ui::LEFT, 0, 15));
    const char* what = l.fix == HF_LOBBY_FIX_STRANGERS ? "Turn on Play with strangers for this session. You'll see a warning first." :
        l.fix == HF_LOBBY_FIX_RELAYS ? "Turn on co-op over Steam relays. You'll see what it opens first." :
        "Show lobbies with host-enforced stat changes for this session. Joining one, you play with the host's stats.";
    g_cv.Text(what, a.x + 14, a.y + 42, T(SZ_LABEL, Alpha(LB_TEXT, alpha), ui::BODY, ui::LEFT, a.w - 28, 15));
    float kw = KeyCap("A", a.x + 14, a.y + 68, alpha, 15);
    g_cv.Text(l.fix == HF_LOBBY_FIX_EDITED_STATS ? "SHOW THEM" : "TURN IT ON...", a.x + 24 + kw, a.y + 68, T(SZ_LABEL, Alpha(LB_AMBER, alpha), ui::TITLE, ui::LEFT, 0, 15));
    return g_cv.Hit(a.x, a.y, a.w, a.h, id) && LobbiesArmed();
}

/* The pop-up's facts: squad (and its players), difficulty, where and who hosts; returns where the next line goes. */
static float DetailFacts(const HfBrowseLobby& l, float x, float y, float alpha)
{
    bool friendly = (l.flags & HF_LOBBY_FLAG_FRIEND) != 0;
    std::string squad = std::to_string(l.members) + "/" + std::to_string(l.max_members) + (l.players[0] ? std::string(SEP) + l.players : std::string());
    y = PopFact("Squad", squad, LB_TEXT, x, y, alpha);
    y = PopFact("Difficulty", l.difficulty > 0 ? std::to_string(l.difficulty) : "Not known", LB_TEXT, x, y, alpha);
    y = PopFact("Where", Where(l), LB_TEXT, x, y, alpha);
    return PopFact("Host", friendly ? "Steam friend" : "Stranger", friendly ? LB_RED : LB_TEXT, x, y, alpha) + 16;
}

/* The pop-up's black strip: ESC BACK, and ENTER JOIN (green while the lobby can be joined). */
static void DetailHints(const HfBrowseLobby* l, const Area& p, float alpha)
{
    bool joinable = Joinable(l), asks = l->fix == HF_LOBBY_FIX_RELAYS && g_lobbies.joining.empty(); /* JOIN asks to turn relays on first */
    std::vector<Hint> hints = {{"ESC", "BACK", LB_TEXT, true, ui::Id("lobbyback")},
        {"ENTER", !g_lobbies.joining.empty() ? "JOINING..." : "JOIN", joinable ? LB_OK : LB_TEXT, joinable || asks, ui::Id("lobbyjoin")}};
    int clicked = DrawHintsRight(hints, p.x + p.w - 22, p.y + p.h - 28, alpha, 15);
    if (clicked == 0) { g_lobbies.detail = false; Sound(sounds::BACK); }
    else if (clicked == 1) Join(l);
}

/* The selected lobby's pop-up: its facts, why it can't be joined (and the way in), the mods a join changes, and JOIN. */
static void DrawLobbyDetail(float X, float Y, float W, float H, float alpha)
{
    const HfBrowseLobby* l = Selected();
    if (l == nullptr) { g_lobbies.detail = false; return; }
    ImU32 colour;
    std::string verdict = Verdict(*l, &colour);
    float w = std::clamp(W * 0.46f, 560.0f, 900.0f) - 44;
    std::string host = l->host[0] ? Upper(l->host) + "'S LOBBY" : std::string("A HELLFORGE LOBBY");
    Area p = DrawPopFrame({host, l->verdict == HF_LOBBY_UNJOINABLE ? "Can't join" : verdict, Tone(*l), false, DetailBodyH(*l, w)}, X, Y, W, H, alpha);
    float x = p.x + 22, y = DetailFacts(*l, x, p.y + 60 + 26, alpha);
    bool out = l->verdict == HF_LOBBY_UNJOINABLE || l->verdict == HF_LOBBY_BLOCKED;
    if (l->reason[0]) y = PopLines(out ? "Why: " + std::string(l->reason) : std::string(l->reason), x, y, w, out ? LB_RED : LB_DIM, alpha) + 10;
    if (l->fix != HF_LOBBY_FIX_NONE) { if (PopFix(*l, {x, y - 10, w, 86}, alpha)) Fix(l); y += 96; }
    if (l->mod_list[0]) y = PopMods(*l, x, y, w, alpha) + 8;
    bool sandbox = l->verdict == HF_LOBBY_UNJOINABLE && Starts(l->reason, SANDBOX_ONLY_REASON);
    PopLines(sandbox ? "To join, turn the sandbox on (the launcher's Settings), where you can trust these mods, or install them yourself." :
        "Joining sends you an invitation. Accept it in game; MOD SYNC asks before anything downloads or is trusted.", x, y, w, LB_DIM, alpha);
    DetailHints(l, p, alpha);
}

/* The open question (OpenQuestion) over the screen: amber for a warning. Y turns it on, N or Esc cancels. */
static void DrawLobbyConfirm(float X, float Y, float W, float H, float alpha, const TermQuestion& q)
{
    const ImU32 accent = q.warning ? LB_AMBER : LB_RED;
    float w = std::clamp(W * 0.46f, 560.0f, 900.0f) - 44;
    std::vector<std::string> lines = Wrapped(q.text, w - 16, SZ_LABEL);
    Area p = DrawPopFrame({q.title, "", accent, q.warning, 30 + 22 * (float)lines.size() + 40}, X, Y, W, H, alpha);
    float x = p.x + 22, y = p.y + 60 + 30;
    g_cv.Rect(x, y - 12, 4, 22 * (float)lines.size(), Alpha(accent, alpha), 15);
    for (const std::string& line : lines) { g_cv.Text(line, x + 16, y, Shown(T(SZ_LABEL, Alpha(LB_TEXT, alpha), ui::BODY, ui::LEFT, w - 16, 15))); y += 22; }
    g_cv.Text("Esc or N cancels.", x, y + 14, T(SZ_SMALL, Alpha(LB_DIM, alpha), ui::BODY, ui::LEFT, w, 15));
    std::vector<Hint> hints = {{"ESC", "CANCEL", LB_TEXT, true, ui::Id("lobbyconfirmcancel")},
        {"Y", "YES, TURN IT ON", accent, true, ui::Id("lobbyconfirmon")}};
    int clicked = DrawHintsRight(hints, p.x + p.w - 22, p.y + p.h - 28, alpha, 15);
    if (clicked == 0) q.no();
    else if (clicked == 1) q.yes();
}

/* Esc or CLOSE: the screen powers off, then the browser goes (LobbyEscape's work, after the animation). */
static void PowerOff()
{
    if (g_lobbies.closingAt > 0) return;
    g_lobbies.closingAt = Now(); /* LobbyEscape plays the sound */
}

/* How far the screen is open (0 to 1), powering on from when it showed and off from when it began to close; -1 once it
 * has powered off (and gone). */
static float LobbyOpen(double t)
{
    float open = (float)std::clamp(t / LOBBY_POWER_ON, 0.0, 1.0);
    if (g_lobbies.closingAt <= 0) return open;
    double c = Now() - g_lobbies.closingAt;
    if (c >= LOBBY_POWER_OFF) { g_lobbies.closingAt = 0; LobbyEscape(); return -1; }
    return std::min(open, 1.0f - (float)(c / LOBBY_POWER_OFF));
}

void DrawLobbies()
{
    if (!g_lobbies.on || g_lobbies.camera) return;
    double t = Now() - g_lobbies.shownAt;
    float open = LobbyOpen(t);
    if (open < 0) return;
    float X = -g_cv.ox, Y = -g_cv.oy, W = g_cv.width, H = g_cv.height;
    /* power on: a bright line across the middle opens into the screen; the contents fade in after it */
    float lineOpen = std::clamp(open / 0.45f, 0.0f, 1.0f), fieldOpen = std::clamp((open - 0.3f) / 0.4f, 0.0f, 1.0f), contents = std::clamp((open - 0.6f) / 0.4f, 0.0f, 1.0f);
    g_cv.Block();
    float fh = H * fieldOpen;
    if (fh >= 2) { g_cv.Rect(X, Y + (H - fh) / 2, W, fh, LB_BG, 8); Hatch({X, Y + (H - fh) / 2, W, fh}, LB_HATCH, 9, 8); }
    if (fieldOpen < 1) g_cv.Rect(X + W * (1 - lineOpen) / 2, Y + H / 2 - 1, W * lineOpen, 3, C(255, 236, 232, (int)(255 * (1 - fieldOpen))), 12);
    if (contents <= 0) return;
    float m = std::max(48.0f, W * 0.03f), top = Y + H * 0.2f, bottom = Y + H - std::max(56.0f, H * 0.07f) - 24;
    DrawLobbyHeader({X + m, Y + m * 0.8f, W - 2 * m, 110}, contents, t);
    float leftW = W * 0.25f, rightW = std::max(220.0f, W * 0.14f), gridX = X + m + leftW + W * 0.025f;
    DrawLobbyLeft({X + m, top, leftW, bottom - top}, contents);
    DrawLobbyGrid({gridX, top, X + W - m - rightW - W * 0.025f - gridX, bottom - top}, contents);
    DrawLobbyRight({X + W - m - rightW, top, rightW, bottom - top}, contents);
    DrawLobbyStrip(X, Y, W, H, contents);
    if (g_lobbies.detail) DrawLobbyDetail(X, Y, W, H, contents);
    if (const TermQuestion* q = OpenQuestion()) DrawLobbyConfirm(X, Y, W, H, contents, *q);
}
} // namespace editorapp
