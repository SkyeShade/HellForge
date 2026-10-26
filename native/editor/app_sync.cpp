/* The editor's window (app_internal.h): mod sync's dialog. */
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "app_internal.h"

namespace editorapp
{
/* ---- mod sync (SDK 14): a lobby whose host plays with other gameplay mods ----
 * The session's backend offers to download the host's mods and restart the session with them (docs/mods.md "Playing
 * with other mods"); the loader hands the offer over (sync_state, sync_mod) and the player's answer back (sync_answer).
 * Tick reads it every 30 frames. An offer that needs the player (new, ready, blocked, failed) shows this dialog by itself:
 * alone over the game, with no editor behind it, or in the window when that is open. Esc or closing the window answers
 * no, except while downloading: the download goes on, and the dialog is back when it is ready. */
SyncDialog g_sync;
/* A key or click meant for the game (Enter in chat, a shot) when the prompt pops up must not answer it: its buttons and
 * keys wait this long after it appears or its state turns. Esc, which only says no or hides it, works at once. */
const double SYNC_ARM = 1.0;
static bool SyncArmed() { return ImGui::GetTime() - g_sync.shownAt >= SYNC_ARM; }
bool g_windowShown; /* Opened() to Closed() */

static bool HasSync() { return g_host.api->size >= offsetof(HfApi, sync_answer) + sizeof(void*) && g_host.api->sync_state != nullptr; }

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

static std::string SizeText(uint64_t bytes)
{
    char text[32];
    if (bytes >= 1024 * 1024) snprintf(text, sizeof text, "%.1f MB", (double)bytes / 1048576.0);
    else snprintf(text, sizeof text, "%llu KB", (unsigned long long)((bytes + 1023) / 1024));
    return text;
}

/* The dialog goes (an open window stays). */
static void CloseSync()
{
    g_sync.on = false;
    ShowSyncAlone(false);
}

void AnswerSync(uint32_t answer, const char* id)
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

/* The offer's mods read again (a new offer, or another count), the selection and the TRUST warning with them. */
static void ReadSyncMods(uint32_t count)
{
    g_sync.mods.clear();
    for (uint32_t i = 0; i < count; i++)
    {
        HfSyncMod mod{};
        mod.size = sizeof mod;
        if (!g_host.api->sync_mod(g_host.mod, i, &mod)) break;
        g_sync.mods.push_back(mod);
    }
    g_sync.index = 0;
    g_sync.trust = -1; /* the warning was for the offer that is gone */
}

/* The offer turned (a new one, or its state): the dialog goes, waits while the mods download, or shows. */
static void SyncTurned(const HfSyncState& now)
{
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
    if (now.version != was.version || now.mods != g_sync.mods.size()) ReadSyncMods(now.mods);
    g_sync.state = now;
    if (turned) SyncTurned(now);
}

static const char* SyncActionText(uint32_t action)
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

static ImU32 SyncActionColour(uint32_t action)
{
    return action == HF_SYNC_MOD_HAVE ? OK : action == HF_SYNC_MOD_DOWNLOAD ? GOLD : action == HF_SYNC_MOD_MANUAL || action == HF_SYNC_MOD_TRUST ? PENDING :
        action == HF_SYNC_MOD_OFF ? FAINT : ERROR_;
}

/* A row's BLOCK: on mods that would come from the host or be installed by hand, while an offer stands, and on mods that
 * wait for the player's trust (the other way to answer them). */
static bool SyncBlockable(const HfSyncMod& mod)
{
    uint32_t state = g_sync.state.state;
    bool offered = (mod.action == HF_SYNC_MOD_DOWNLOAD || mod.action == HF_SYNC_MOD_MANUAL) &&
        (state == HF_SYNC_OFFER || state == HF_SYNC_DOWNLOADING || state == HF_SYNC_READY);
    return !g_sync.sent && (offered || (mod.action == HF_SYNC_MOD_TRUST && state == HF_SYNC_BLOCKED));
}

/* A row's TRUST: a Steam friend's mod HellForge has not verified (the backend offers no other for trust). */
static bool SyncTrustable(const HfSyncMod& mod) { return !g_sync.sent && mod.action == HF_SYNC_MOD_TRUST && g_sync.state.state == HF_SYNC_BLOCKED; }

/* A row's GET IT: a mod the player may install themselves, with an address to get it from (the loader keeps only https://
 * ones, and the backend opens only those). */
static bool SyncGettable(const HfSyncMod& mod)
{
    size_t size = sizeof mod.url;
    return (mod.action == HF_SYNC_MOD_MANUAL || mod.action == HF_SYNC_MOD_TRUST) && mod.url[0] && strnlen(mod.url, size) < size &&
        !strncmp(mod.url, "https://", 8);
}

/* The TRUST warning for a row: shown instead of the list until answered; its button waits SYNC_ARM seconds, and only a
 * click on it trusts (no key does). */
static void AskTrust(int i)
{
    if (i < 0 || i >= (int)g_sync.mods.size() || !SyncTrustable(g_sync.mods[i])) return;
    g_sync.trust = i;
    g_sync.trustAt = ImGui::GetTime();
    g_host.api->log(g_host.mod, "editor: mod sync: the trust warning for %s shows", g_sync.mods[i].id);
    Sound(sounds::OPEN);
}
static bool TrustArmed() { return SyncArmed() && ImGui::GetTime() - g_sync.trustAt >= SYNC_ARM; }

/* Trusting this mod leaves nothing else in the way: the backend starts the download (TRUST AND DOWNLOAD). */
static bool TrustDownloads(int i)
{
    for (int j = 0; j < (int)g_sync.mods.size(); j++)
    {
        uint32_t action = g_sync.mods[j].action;
        if (j != i && (action == HF_SYNC_MOD_TRUST || action == HF_SYNC_MOD_MANUAL || action == HF_SYNC_MOD_BLOCKED || action == HF_SYNC_MOD_UPDATE)) return false;
    }
    return true;
}

/* The dialog's first button and its answer, by state ("" none). */
static const char* SyncPrimary(uint32_t* answer)
{
    const HfSyncState& s = g_sync.state;
    *answer = s.state == HF_SYNC_OFFER ? (s.bytes > 0 ? HF_SYNC_ACCEPT : HF_SYNC_RESTART) : s.state == HF_SYNC_READY ? HF_SYNC_RESTART :
        s.state == HF_SYNC_BLOCKED || s.state == HF_SYNC_FAILED ? HF_SYNC_DECLINE : 0;
    if (s.state == HF_SYNC_OFFER) return s.bytes > 0 ? "DOWNLOAD AND RESTART" : "RESTART TO JOIN";
    if (s.state == HF_SYNC_READY) return "RESTART TO JOIN";
    if (s.state == HF_SYNC_BLOCKED || s.state == HF_SYNC_FAILED) return "OK";
    return "";
}

/* B, T and G on the selected mod; true when T showed its TRUST warning. */
static bool SyncModKeys(int n)
{
    if (ImGui::IsKeyPressed(ImGuiKey_B, false) && g_sync.index < n && SyncBlockable(g_sync.mods[g_sync.index]))
        AnswerSync(HF_SYNC_BLOCK, g_sync.mods[g_sync.index].id);
    if (ImGui::IsKeyPressed(ImGuiKey_T, false) && g_sync.index < n && SyncTrustable(g_sync.mods[g_sync.index])) { AskTrust(g_sync.index); return true; }
    if (ImGui::IsKeyPressed(ImGuiKey_G, false) && g_sync.index < n && SyncGettable(g_sync.mods[g_sync.index]))
        AnswerSync(HF_SYNC_OPEN, g_sync.mods[g_sync.index].id);
    return false;
}

/* Esc: the window closes (Closed() answers), the prompt goes while the download goes on, or the offer is declined. */
static void SyncEscape()
{
    if (g_windowShown) g_host.close(); /* Closed() answers */
    else if (g_sync.sent || g_sync.state.state == HF_SYNC_DOWNLOADING) { ShowSyncAlone(false); Sound(sounds::BACK); } /* it goes on */
    else AnswerSync(HF_SYNC_DECLINE);
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
    if (armed && SyncModKeys(n)) return;
    uint32_t answer;
    const char* primary = SyncPrimary(&answer);
    if (armed && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && primary[0] && !g_sync.sent)
        AnswerSync(answer);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) SyncEscape();
}

/* A mod's class chip (CODE, RAW DATA) at x; the room it takes (none for a mod of plain data). */
static float ClassChip(const HfSyncMod& mod, float x, float cy, int z)
{
    if (mod.kind == HF_SYNC_CLASS_CODE) return Chip("CODE", x, cy, INVERSE, LUA, false, z) + 8;
    if (mod.kind == HF_SYNC_CLASS_RAW) return Chip("RAW DATA", x, cy, INVERSE, LUA, false, z) + 8;
    return 0;
}

/* The TRUST warning's lines: what trusting the mod means (*lead of them, drawn brighter), then what HellForge still
 * checks. */
static std::vector<std::string> TrustLines(const HfSyncMod& mod, const std::string& name, const std::string& friendName, float w, size_t* lead)
{
    std::vector<std::string> lines;
    std::string warning = mod.kind == HF_SYNC_CLASS_RAW
        ? name + " carries game data HellForge cannot check, which goes straight to the game. Data like this can break your game, or "
          "through a flaw in the game run code on your PC that can do anything your Windows account can."
        : name + " runs its own code on your PC. A mod like this can do anything your Windows account can: read and change your "
          "files, or install other programs.";
    warning += " Only trust it if you trust " + friendName + " and know where the mod comes from.";
    for (std::string& part : Wrapped(warning, w - 56, SZ_LABEL)) lines.push_back(std::move(part));
    *lead = lines.size();
    for (std::string& part : Wrapped("HellForge still checks that every file you download is exactly the one " + friendName + " has. You can take "
             "the trust back on the launcher's Mods page (MOD SYNC).", w - 56, SZ_SMALL))
        lines.push_back(std::move(part));
    return lines;
}

/* CANCEL and TRUST (TRUST AND DOWNLOAD when trusting this mod leaves nothing else in the way): TRUST waits SYNC_ARM
 * seconds and answers only to a click. */
static void TrustButtons(const HfSyncMod& mod, const Area& box)
{
    bool armed = TrustArmed();
    const char* label = TrustDownloads(g_sync.trust) ? "TRUST AND DOWNLOAD" : "TRUST";
    float bx = box.x + box.w - 24 - 150;
    if (Button("CANCEL", {bx, box.y + box.h - 66, 150, 30}, NORMAL, ui::Id("synctrustno"), 11)) { g_sync.trust = -1; Sound(sounds::BACK); return; }
    float pw = std::max(150.0f, g_cv.Measure(label, SZ_TAB, ui::TITLE) + 40);
    bx -= 12 + pw;
    if (Button(label, {bx, box.y + box.h - 66, pw, 30}, armed ? DANGER : DISABLED, ui::Id("synctrustyes"), 11) && armed)
    {
        g_host.api->log(g_host.mod, "editor: mod sync: the player trusts %s", mod.id);
        AnswerSync(HF_SYNC_TRUST, mod.id);
        return;
    }
    g_cv.Text("Click " + std::string(label) + " to trust it   Esc back to the list", box.x + 24, box.y + box.h - 20, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, 0, 11));
}

/* The TRUST warning, in the prompt's place: what trusting a mod HellForge has not checked means, in plain words. Its
 * button waits SYNC_ARM seconds and answers only to a click; CANCEL and Esc go back to the list. */
static void DrawTrust(const HfSyncMod& mod)
{
    const HfSyncState& s = g_sync.state;
    std::string name = std::string(mod.name[0] ? mod.name : mod.id) + " " + mod.version, friendName = s.host[0] ? s.host : "the host";
    float w = std::min(760.0f, PANEL_W - 60);
    size_t lead;
    std::vector<std::string> lines = TrustLines(mod, name, friendName, w, &lead);
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
    cx += ClassChip(mod, cx, ly, 11);
    g_cv.Text(name, cx, ly, T(SZ_LABEL, TEXT, ui::BODY, ui::LEFT, x + w - 24 - cx, 11));
    g_cv.Rect(x + 12, ly + 22, w - 24, 1, LINE, 10);
    for (size_t i = 0; i < lines.size(); i++)
        g_cv.Text(lines[i], x + 28, ly + 50 + 26.0f * (float)i + (i >= lead ? 8.0f : 0), T(i < lead ? SZ_LABEL : SZ_SMALL, i < lead ? TEXT : DIM, ui::BODY, ui::LEFT, w - 56, 11));
    TrustButtons(mod, {x, y, w, h});
}

/* What the dialog says for the offer's state (the backend's own words, where it has some). */
static std::string SyncMessage(const HfSyncState& s)
{
    return s.message[0] ? s.message :
        s.state == HF_SYNC_OFFER ? "The host plays with gameplay mods you don't have. Get them and restart with the host's mods: their invitation waits on your ship." :
        s.state == HF_SYNC_DOWNLOADING ? "Downloading the host's mods; each file is checked before it is kept." :
        s.state == HF_SYNC_READY ? "Every mod is here. Restart with the host's mods: their invitation waits on your ship." :
        s.state == HF_SYNC_BLOCKED ? "You cannot join this lobby with these mods." : "The download failed.";
}

/* The dialog's title (whose lobby), what it says, and its list's heading. */
static void DrawSyncHead(const HfSyncState& s, bool bad, const Area& box)
{
    float x = box.x, y = box.y, w = box.w;
    g_cv.Rect(x, y, w, 3, bad ? ERROR_ : GOLD, 10);
    float tw = g_cv.Text("MOD SYNC", x + 24, y + 30, T(18, bad ? ERROR_ : GOLD, ui::TITLE, ui::LEFT, 0, 11));
    g_cv.Text(std::string(s.host[0] ? s.host : "The host") + "'s lobby", x + 36 + tw, y + 31, T(SZ_LABEL, TEXT, ui::BODY, ui::LEFT, w - tw - 80, 11));
    g_cv.Text(SyncMessage(s), x + 24, y + 58, T(SZ_SMALL, bad ? ERROR_ : DIM, ui::BODY, ui::LEFT, w - 48, 11));
    float hy = y + 80;
    g_cv.Rect(x + 1, hy, w - 2, 22, HEADER, 10);
    g_cv.Text("MOD", x + 24, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::LEFT, 0, 11));
    g_cv.Text("WHAT HAPPENS", x + w - 460, hy + 11, T(SZ_TINY, FAINT, ui::TITLE, ui::LEFT, 0, 11));
}

/* The host's mods: each with its class, what happens to it, and its BLOCK, TRUST and GET IT buttons. */
static void DrawSyncList(const Area& box, float hy, int n)
{
    ListOpts o;
    o.x = box.x + 2; o.y = hy + 26; o.w = box.w - 4; o.h = box.y + box.h - hy - 26 - 108; o.rowH = 40; o.count = n; o.selected = g_sync.index;
    o.key = "sync"; o.focused = true; o.layer = 12;
    o.click = [&](int i) { g_sync.index = i; };
    o.draw = [&](int i, float rx, float ry, float rw, float rh, bool hovered) {
        const HfSyncMod& mod = g_sync.mods[i];
        bool selected = i == g_sync.index;
        if (selected) { g_cv.Rect(rx + 10, ry + 2, rw - 20, rh - 4, SELECT, 10); g_cv.Rect(rx + 10, ry + 2, 3, rh - 4, GOLD, 11); }
        else if (hovered) g_cv.Rect(rx + 10, ry + 2, rw - 20, rh - 4, HOVER, 10);
        float lx = rx + 22, right = rx + rw - 460;
        lx += ClassChip(mod, lx, ry + rh / 2, 12);
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
            if (Button("BLOCK", {bx, by, 78, 24}, DANGER, ui::Id("syncblock", (uint64_t)i), 12) && SyncArmed()) { AnswerSync(HF_SYNC_BLOCK, mod.id); return; }
            bx -= 86;
        }
        if (SyncTrustable(mod))
        {
            if (Button("TRUST", {bx, by, 78, 24}, SyncArmed() ? NORMAL : DISABLED, ui::Id("synctrust", (uint64_t)i), 12) && SyncArmed()) { AskTrust(i); return; }
            bx -= 86;
        }
        if (SyncGettable(mod) && Button("GET IT", {bx, by, 78, 24}, SyncArmed() ? NORMAL : DISABLED, ui::Id("syncget", (uint64_t)i), 12) && SyncArmed())
            AnswerSync(HF_SYNC_OPEN, mod.id);
    };
    List(o);
}

/* The download's progress, or how much there is to download. */
static void DrawSyncSize(const HfSyncState& s, float x, float fy, float w)
{
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
}

/* The first button while an answer is on its way: what it waits for. */
static const char* PrimaryLabel(uint32_t answer, const char* primary) { return g_sync.sent ? (answer == HF_SYNC_RESTART ? "RESTARTING..." : "STARTING...") : primary; }
static Style PrimaryStyle(bool bad) { return g_sync.sent || !SyncArmed() ? DISABLED : bad ? NORMAL : PRIMARY; }

/* The dialog's second button (CANCEL, LATER) and its first, by state, from the right; returns the x left of them. */
static float DrawSyncButtons(const HfSyncState& s, bool bad, const Area& box, uint32_t answer, const char* primary)
{
    float bx = box.x + box.w - 24;
    const char* second = s.state == HF_SYNC_OFFER || s.state == HF_SYNC_DOWNLOADING ? "CANCEL" : s.state == HF_SYNC_READY ? "LATER" : "";
    if (second[0])
    {
        bx -= 150;
        if (Button(second, {bx, box.y + box.h - 66, 150, 30}, NORMAL, ui::Id("syncsecond"), 11) && SyncArmed()) AnswerSync(HF_SYNC_DECLINE);
        bx -= 12;
    }
    if (primary[0])
    {
        float pw = std::max(150.0f, g_cv.Measure(primary, SZ_TAB, ui::TITLE) + 40);
        bx -= pw;
        if (Button(PrimaryLabel(answer, primary), {bx, box.y + box.h - 66, pw, 30}, PrimaryStyle(bad), ui::Id("syncprimary"), 11) && SyncArmed())
            AnswerSync(answer);
    }
    return bx;
}

/* beside the buttons: what GET IT does, and that installing a mod by hand takes a new session */
static void DrawGetItNote(float x, float fy, float bx)
{
    auto lines = Wrapped("GET IT opens the mod's page in your browser. To install a mod yourself, close the game, add the mod "
        "on the Mods page, then start HellForge again and join.", bx - x - 48, SZ_SMALL);
    for (size_t i = 0; i < lines.size() && i < 2; i++)
        g_cv.Text(lines[i], x + 24, fy + 46 + 18.0f * (float)i, T(SZ_SMALL, DIM, ui::BODY, ui::LEFT, bx - x - 36, 11));
}

/* The dialog's keys, as they are now. */
static std::string SyncKeysText(const HfSyncState& s, const char* primary, bool gettable, bool trustable)
{
    std::string keys = "\xE2\x86\x91\xE2\x86\x93 select   B block the mod   ";
    if (trustable) keys += "T trust   ";
    if (gettable) keys += "G get it   ";
    if (primary[0]) keys += std::string("Enter ") + primary + "   ";
    keys += s.state == HF_SYNC_DOWNLOADING ? "Esc hide (the download goes on)" : "Esc close (no)";
    return keys;
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
    Area box{x, y, w, h};
    g_cv.Block();
    g_cv.Rect(x, y, w, h, PANEL_ALT, 9);
    g_cv.Frame(x, y, w, h, LINE_STRONG, 10);
    DrawSyncHead(s, bad, box);
    float hy = y + 80;
    bool gettable = false, trustable = false;
    for (const HfSyncMod& mod : g_sync.mods) { gettable = gettable || SyncGettable(mod); trustable = trustable || SyncTrustable(mod); }
    DrawSyncList(box, hy, n);
    if (n == 0) g_cv.Text("The host's mods are not listed.", x + 28, hy + 48, T(SZ_SMALL, FAINT, ui::BODY, ui::LEFT, 0, 12));
    float fy = y + h - 104;
    g_cv.Rect(x + 12, fy, w - 24, 1, LINE, 10);
    DrawSyncSize(s, x, fy, w);
    uint32_t answer;
    const char* primary = SyncPrimary(&answer);
    float bx = DrawSyncButtons(s, bad, box, answer, primary);
    if (gettable && s.state != HF_SYNC_DOWNLOADING) DrawGetItNote(x, fy, bx);
    g_cv.Text(SyncKeysText(s, primary, gettable, trustable), x + 24, y + h - 20, T(SZ_TINY, FAINT, ui::BODY, ui::LEFT, 0, 11));
}
} // namespace editorapp
