/* The game window's input while a modal screen has it: the window procedure is replaced by one that toggles screens on
 * their keys and, while a modal one is open, keeps keyboard, mouse and raw input from the game. The game's cursor
 * clipping and recentring are ignored meanwhile, so the cursor moves freely, shown as the game's own pointer. */
#include <string.h>
#include "host_internal.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM w, LPARAM l);

namespace overlay
{
namespace
{
typedef BOOL(WINAPI* ClipCursorFn)(const RECT*);
typedef BOOL(WINAPI* SetCursorPosFn)(int, int);
typedef UINT(WINAPI* RawInputBufferFn)(PRAWINPUT, PUINT, UINT);
ClipCursorFn g_clipCursor;
SetCursorPosFn g_setCursorPos;
RawInputBufferFn g_rawInputBuffer;
WNDPROC g_gameProc;
int g_shownBy; /* ShowCursor(TRUE) calls made for the game's pointer; the window's thread only */

/* What the window's messages said since the last frame (mouse in client pixels). */
struct Pending
{
    float x, y;
    bool moved;
    uint32_t down, pressed, released;
    float wheel;
    uint32_t keys[16];
    uint32_t keyCount;
};
Pending g_pending;
SRWLOCK g_pendingLock = SRWLOCK_INIT;

bool InputMessage(UINT message)
{
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST) || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        message == WM_INPUT || message == WM_MOUSEHOVER || message == WM_MOUSELEAVE || message == WM_NCMOUSEMOVE;
}

uint32_t ButtonOf(UINT message, bool* down)
{
    switch (message)
    {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: *down = true; return HF_MOUSE_LEFT;
    case WM_LBUTTONUP: *down = false; return HF_MOUSE_LEFT;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: *down = true; return HF_MOUSE_RIGHT;
    case WM_RBUTTONUP: *down = false; return HF_MOUSE_RIGHT;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: *down = true; return HF_MOUSE_MIDDLE;
    case WM_MBUTTONUP: *down = false; return HF_MOUSE_MIDDLE;
    default: return 0;
    }
}

/* One message into this frame's input. Buttons carry the mouse's place too: ImGui-style hosts that learn it only from
 * moves put the first click after opening nowhere. */
void Note(HWND window, UINT message, WPARAM w, LPARAM l)
{
    bool down = false;
    uint32_t button = ButtonOf(message, &down);
    AcquireSRWLockExclusive(&g_pendingLock);
    if (message == WM_MOUSEMOVE || button != 0)
    {
        g_pending.x = (float)(short)LOWORD(l);
        g_pending.y = (float)(short)HIWORD(l);
        g_pending.moved = true;
    }
    if (button != 0 && down) { g_pending.down |= button; g_pending.pressed |= button; }
    if (button != 0 && !down) { g_pending.down &= ~button; g_pending.released |= button; }
    if (message == WM_MOUSEWHEEL) g_pending.wheel += (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
    bool fresh = !(l & 0x40000000);
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && fresh && g_pending.keyCount < 16) g_pending.keys[g_pending.keyCount++] = (uint32_t)w;
    ReleaseSRWLockExclusive(&g_pendingLock);
    (void)window;
}

/* A screen's key, or Esc on the modal screen that has the input: true when it was taken. */
bool TakeKey(UINT message, WPARAM w, LPARAM l)
{
    if ((message != WM_KEYDOWN && message != WM_SYSKEYDOWN) || (l & 0x40000000)) return false;
    if (ScreenKey((UINT)w)) return true;
    if (w != VK_ESCAPE || !g_captured) return false;
    int id = CapturingScreen();
    if (id != 0) hf_overlay_show(id, 0);
    return id != 0;
}

/* The game hides the cursor in play and sets it again on every WM_SETCURSOR, so while a screen has the mouse those go
 * to the host, which shows the game's own pointer; true when the message was taken. The game gets them back after. */
bool PointWithGameCursor(UINT message, LPARAM l)
{
    HCURSOR cursor = g_gameCursor;
    bool ours = message == WM_SETCURSOR ? LOWORD(l) == HTCLIENT : message == WM_MOUSEMOVE;
    if (g_captured && cursor != NULL && ours)
    {
        SetCursor(cursor);
        if (g_shownBy == 0)
            for (int count = -1; count < 0 && g_shownBy < 64; g_shownBy++) count = ShowCursor(TRUE);
        return message == WM_SETCURSOR;
    }
    for (; !g_captured && g_shownBy > 0; g_shownBy--) ShowCursor(FALSE);
    return false;
}

LRESULT CALLBACK HostProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (TakeKey(message, w, l)) return 0;
    if (PointWithGameCursor(message, l)) return TRUE;
    if (g_captured)
    {
        Note(window, message, w, l);
        EnterCriticalSection(&g_lock);
        if (ImGui::GetCurrentContext() != NULL) ImGui_ImplWin32_WndProcHandler(window, message, w, l);
        LeaveCriticalSection(&g_lock);
        if (InputMessage(message)) return message == WM_INPUT ? DefWindowProcW(window, message, w, l) : 0;
    }
    return CallWindowProcW(g_gameProc, window, message, w, l);
}

BOOL WINAPI ClipCursorDetour(const RECT* rect) { return g_captured ? TRUE : g_clipCursor(rect); }
BOOL WINAPI SetCursorPosDetour(int x, int y) { return g_captured ? TRUE : g_setCursorPos(x, y); }
/* The game reads its mouse and keys in batches; while a screen has them the batch is drained and reported empty. */
UINT WINAPI RawInputBufferDetour(PRAWINPUT data, PUINT size, UINT header)
{
    UINT read = g_rawInputBuffer(data, size, header);
    return g_captured && data != NULL ? 0 : read;
}
} // namespace

HCURSOR volatile g_gameCursor;

void FollowCursor(bool captured)
{
    static int said = -1;
    HCURSOR cursor = captured ? GameCursor() : NULL;
    g_gameCursor = cursor;
    if (captured && said != (cursor != NULL))
    {
        said = cursor != NULL;
        Log(cursor != NULL ? "overlay: screens show the game's own mouse pointer" : "overlay: the game's mouse pointer is unknown; screens draw ImGui's arrow");
    }
    /* the pointer changes now, not at the next mouse move; the game's window procedure gets this one back */
    if (g_window != NULL) PostMessageW(g_window, WM_SETCURSOR, (WPARAM)g_window, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

bool ImGuiDrawsCursor(bool captured) { return captured && g_gameCursor == NULL; }

void HookCursor()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == NULL) return;
    int failed = Hook((void*)GetProcAddress(user32, "ClipCursor"), (void*)ClipCursorDetour, (void**)&g_clipCursor);
    failed |= Hook((void*)GetProcAddress(user32, "SetCursorPos"), (void*)SetCursorPosDetour, (void**)&g_setCursorPos);
    failed |= Hook((void*)GetProcAddress(user32, "GetRawInputBuffer"), (void*)RawInputBufferDetour, (void**)&g_rawInputBuffer);
    if (failed) Log("overlay: the cursor calls could not all be hooked; the game may still hold the cursor while a screen is open");
}

bool SubclassWindow(HWND window)
{
    g_gameProc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)HostProc);
    return g_gameProc != NULL;
}

void ResetInput()
{
    AcquireSRWLockExclusive(&g_pendingLock);
    float x = g_pending.x, y = g_pending.y;
    g_pending = Pending{};
    g_pending.x = x;
    g_pending.y = y;
    ReleaseSRWLockExclusive(&g_pendingLock);
}

void ReleaseCursorClip()
{
    if (g_clipCursor != NULL) g_clipCursor(NULL);
}

/* The client area in back-buffer pixels: the swap chain can be another size than the window. */
static void ClientScale(float width, float height, float* sx, float* sy)
{
    RECT client = {};
    *sx = *sy = 1;
    if (g_window == NULL || !GetClientRect(g_window, &client) || client.right <= 0 || client.bottom <= 0) return;
    *sx = width / (float)client.right;
    *sy = height / (float)client.bottom;
}

void TakeInput(HfInput* out, bool captured, float width, float height, float unitsPerPixel)
{
    *out = HfInput{};
    out->size = sizeof *out;
    float sx, sy;
    ClientScale(width, height, &sx, &sy);
    AcquireSRWLockExclusive(&g_pendingLock);
    Pending p = g_pending;
    if (captured)
    {
        g_pending.pressed = g_pending.released = 0;
        g_pending.wheel = 0;
        g_pending.keyCount = 0;
    }
    ReleaseSRWLockExclusive(&g_pendingLock);
    if (!captured || !p.moved)
    {
        POINT at;
        if (g_window != NULL && GetCursorPos(&at) && ScreenToClient(g_window, &at)) { p.x = (float)at.x; p.y = (float)at.y; }
    }
    out->x = p.x * sx * unitsPerPixel;
    out->y = p.y * sy * unitsPerPixel;
    if (!captured) return;
    out->wheel = p.wheel;
    out->down = p.down;
    out->pressed = p.pressed;
    out->released = p.released;
    out->key_count = p.keyCount;
    memcpy(out->keys, p.keys, sizeof out->keys);
}
} // namespace overlay
