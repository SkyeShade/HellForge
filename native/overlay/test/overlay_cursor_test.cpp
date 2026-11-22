/* The overlay host's mouse pointer (input.cpp) on a hidden window, without a swap chain: the host's window procedure
 * over a stand-in game's, a stand-in for the rest of the host. While a modal screen has the mouse with the game's
 * pointer known, WM_SETCURSOR shows that pointer, raises a hidden cursor's count and never reaches the game; once the
 * screen lets go, the game gets WM_SETCURSOR again with its count as it was. With no pointer known, ImGui draws its
 * arrow and the game's window procedure goes on as before. A screen that draws its own cursor hides either. A press
 * inside a non-modal screen's click box, and its release, stay from the game; every other click reaches it.
 * Usage: overlay_cursor_test.exe (built and run by scripts/build-overlay.sh --test) */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "host_internal.h"

namespace overlay
{
CRITICAL_SECTION g_lock;
HWND g_window;
volatile LONG g_captured;
static HCURSOR g_offered;
static char g_lastLog[256];
void Log(const char* format, ...)
{
    va_list a;
    va_start(a, format);
    vsnprintf(g_lastLog, sizeof g_lastLog, format, a);
    va_end(a);
}
int Hook(void*, void*, void**) { return 1; }
bool ScreenKey(UINT) { return false; }
int CapturingScreen() { return g_captured ? 1 : 0; }
HCURSOR GameCursor() { return g_offered; }
static bool g_boxScreenOpen = true;
bool ScreenTakesClicks(int id) { return id == 2 && g_boxScreenOpen; }
} // namespace overlay
extern "C" void hf_overlay_show(int, int) {}

using namespace overlay;

static int g_failures, g_gameSetCursor, g_gameButtons;
static void Check(bool ok, const char* what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

static LRESULT CALLBACK GameProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_SETCURSOR) { g_gameSetCursor++; SetCursor(NULL); return TRUE; } /* the game in play: no cursor */
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) g_gameButtons++;
    return DefWindowProcW(window, message, w, l);
}

static void Pump()
{
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
}
static int CursorCount() { int count = ShowCursor(TRUE); ShowCursor(FALSE); return count - 1; }
static void SetCursorMessage(LPARAM hit) { SendMessageW(g_window, WM_SETCURSOR, (WPARAM)g_window, MAKELPARAM(hit, WM_MOUSEMOVE)); }
static void Capture(bool on)
{
    InterlockedExchange(&g_captured, on ? 1 : 0);
    FollowCursor(on);
    Pump(); /* the WM_SETCURSOR FollowCursor posts */
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&g_lock);
    WNDCLASSEXW type = {sizeof type, 0, GameProc, 0, 0, GetModuleHandleW(NULL), NULL, NULL, NULL, NULL, L"OverlayCursorTest", NULL};
    RegisterClassExW(&type);
    g_window = CreateWindowExW(0, type.lpszClassName, L"overlay cursor test", WS_POPUP, 0, 0, 64, 64, NULL, NULL, type.hInstance, NULL); /* never shown */
    Check(g_window != NULL && SubclassWindow(g_window), "a hidden window with the host's window procedure");
    uint8_t andMask[128], xorMask[128];
    memset(andMask, 0x0F, sizeof andMask);
    memset(xorMask, 0xF0, sizeof xorMask);
    HCURSOR pointer = CreateCursor(GetModuleHandleW(NULL), 0, 0, 32, 32, andMask, xorMask);
    ShowCursor(FALSE);
    const int hidden = CursorCount();
    Check(pointer != NULL && hidden < 0, "the game's pointer made; the cursor hidden as in play");

    puts("  -- no screen open");
    int before = g_gameSetCursor;
    SetCursorMessage(HTCLIENT);
    Check(g_gameSetCursor == before + 1 && GetCursor() == NULL, "WM_SETCURSOR is the game's");
    Check(!ImGuiDrawsCursor(false), "ImGui draws no arrow");

    puts("  -- a modal screen open, the game's pointer known");
    g_offered = pointer;
    before = g_gameSetCursor;
    Capture(true);
    Check(GetCursor() == pointer && g_gameSetCursor == before, "the opening shows the game's pointer at once, without the game");
    Check(strstr(g_lastLog, "the game's own mouse pointer") != NULL, "the log says the game's pointer is shown");
    Check(CursorCount() >= 0, "the hidden cursor is shown");
    SetCursor(NULL);
    SetCursorMessage(HTCLIENT);
    Check(GetCursor() == pointer && g_gameSetCursor == before, "WM_SETCURSOR shows it again and stays from the game");
    SetCursor(NULL);
    SendMessageW(g_window, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
    Check(GetCursor() == pointer, "a mouse move shows it again");
    const int shown = CursorCount();
    SetCursorMessage(HTCLIENT);
    SetCursorMessage(HTCLIENT);
    Check(CursorCount() == shown, "the count is raised once, not at every message");

    puts("  -- the screen draws its own cursor under the mouse (hide_pointer)");
    SetCursor(pointer);
    FollowPointer(true);
    Pump(); /* the WM_SETCURSOR FollowPointer posts */
    Check(GetCursor() == NULL, "hidden at once, without a mouse move");
    SetCursorMessage(HTCLIENT);
    SendMessageW(g_window, WM_MOUSEMOVE, 0, MAKELPARAM(12, 12));
    Check(GetCursor() == NULL && g_gameSetCursor == before, "WM_SETCURSOR and mouse moves keep it hidden, still without the game");
    Check(!ImGuiDrawsCursor(true), "hidden: no ImGui arrow either");
    SetCursor(NULL);
    FollowPointer(false);
    Pump();
    Check(GetCursor() == pointer, "a frame without the call shows the pointer again at once");
    Capture(false);
    Check(CursorCount() == hidden, "closed: the cursor count is the game's again, however many messages came");
    Check(g_gameSetCursor == before + 1 && GetCursor() == NULL, "closed: the game's window procedure set its own cursor");
    Check(g_gameCursor == NULL && !ImGuiDrawsCursor(false), "closed: no pointer kept, no arrow");

    puts("  -- hide_pointer counts only while a screen has the mouse");
    FollowPointer(true);
    Pump();
    Capture(true);
    Check(GetCursor() == pointer, "asked with no screen open: the next opening shows the pointer");
    Capture(false);

    puts("  -- the pointer's border: only the client area");
    Capture(true);
    before = g_gameSetCursor;
    SetCursorMessage(HTCAPTION);
    Check(g_gameSetCursor == before + 1, "a non-client WM_SETCURSOR stays the game's");
    Check(!ImGuiDrawsCursor(true), "with the game's pointer ImGui draws no arrow");
    Capture(false);

    puts("  -- a modal screen open, no pointer known");
    g_offered = NULL;
    before = g_gameSetCursor;
    Capture(true);
    Check(strstr(g_lastLog, "ImGui's arrow") != NULL, "the log says ImGui's arrow is drawn");
    Check(ImGuiDrawsCursor(true), "ImGui draws its arrow");
    FollowPointer(true);
    Check(!ImGuiDrawsCursor(true), "hide_pointer: ImGui draws no arrow there");
    FollowPointer(false);
    Check(ImGuiDrawsCursor(true), "and draws it again from the next frame without the call");
    Check(CursorCount() == hidden, "the cursor count is left alone");
    Capture(false);
    Check(!ImGuiDrawsCursor(false), "closed: no arrow");

    puts("  -- a non-modal screen's click box (take_clicks)");
    BeginClickBoxes(64, 64); /* the back buffer is the client area's size */
    AddClickBox(2, 10, 10, 30, 20);
    EndClickBoxes();
    HfInput in;
    int buttons = g_gameButtons;
    SendMessageW(g_window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(15, 15));
    Check(g_gameButtons == buttons && ClickOwner() == 2, "a press inside the box is the screen's, not the game's");
    TakeInput(&in, true, true, 64, 64, 1);
    Check((in.pressed & HF_MOUSE_LEFT) && (in.down & HF_MOUSE_LEFT), "the screen's input has the press");
    SendMessageW(g_window, WM_LBUTTONUP, 0, MAKELPARAM(40, 40));
    Check(g_gameButtons == buttons, "its release stays from the game, outside the box too");
    TakeInput(&in, true, true, 64, 64, 1);
    Check((in.released & HF_MOUSE_LEFT) && !(in.down & HF_MOUSE_LEFT) && in.key_count == 0, "the screen gets the release, no keys");
    SendMessageW(g_window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 40));
    SendMessageW(g_window, WM_LBUTTONUP, 0, MAKELPARAM(15, 15));
    Check(g_gameButtons == buttons + 2, "a press outside, released inside, is the game's");
    g_boxScreenOpen = false;
    SendMessageW(g_window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(15, 15));
    SendMessageW(g_window, WM_LBUTTONUP, 0, MAKELPARAM(15, 15));
    Check(g_gameButtons == buttons + 4, "the screen closed: its box takes nothing");
    g_boxScreenOpen = true;
    SendMessageW(g_window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(15, 15));
    Capture(true);
    SendMessageW(g_window, WM_LBUTTONUP, 0, MAKELPARAM(15, 15));
    Capture(false);
    SendMessageW(g_window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 40));
    Check(g_gameButtons == buttons + 5, "a modal screen opened by the click ends the box's press");
    SendMessageW(g_window, WM_LBUTTONUP, 0, MAKELPARAM(40, 40));

    printf(g_failures ? "FAIL %d check(s)\n" : "PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
