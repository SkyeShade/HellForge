/* The game window's input while a modal screen has it: the window procedure is replaced by one that toggles screens on
 * their keys and, while a modal one is open, keeps keyboard, mouse and raw input from the game. The game's cursor
 * clipping and recentring are ignored meanwhile, so the cursor moves freely, shown as the game's own pointer.
 * With no modal screen open, a mouse button pressed inside a click box of an open non-modal screen (HfDraw.take_clicks)
 * is that screen's until it is released: its window messages, its raw input and its GetKeyState/GetAsyncKeyState state
 * are kept from the game. Everything else, the mouse's moves included, still reaches the game. */
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
typedef UINT(WINAPI* RawInputDataFn)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
typedef SHORT(WINAPI* KeyStateFn)(int);
ClipCursorFn g_clipCursor;
SetCursorPosFn g_setCursorPos;
RawInputBufferFn g_rawInputBuffer;
RawInputDataFn g_rawInputData;
KeyStateFn g_getKeyState, g_getAsyncKeyState;
WNDPROC g_gameProc;
int g_shownBy; /* ShowCursor(TRUE) calls made for the game's pointer; the window's thread only */
volatile LONG g_pointerHidden; /* the screen with the input draws its own cursor under the mouse (hide_pointer) */

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

/* ---- click boxes: the non-modal screens' (take_clicks), in client pixels ---- */
const int MAX_BOXES = 32;
struct Box
{
    float x0, y0, x1, y1;
    int id;
};
Box g_boxes[MAX_BOXES], g_nextBoxes[MAX_BOXES];
int g_boxCount, g_nextCount;
SRWLOCK g_boxLock = SRWLOCK_INIT;
float g_boxScaleX = 1, g_boxScaleY = 1; /* this frame's back-buffer pixels per client pixel; the render thread only */
volatile LONG g_clickOwner;     /* the screen whose box took the last press */
volatile LONG g_heldByMessage;  /* HF_MOUSE_... pressed in a box, as window messages, until released */
volatile LONG g_heldByRaw;      /* the same, as raw input */
volatile LONG g_sawButtonMessage; /* the game's window gets WM_LBUTTONDOWN and the like (not raw input only) */

/* The screen with a click box under this point (client pixels), 0 none. Any thread. */
int BoxAt(float x, float y)
{
    int id = 0;
    AcquireSRWLockShared(&g_boxLock);
    for (int i = g_boxCount - 1; i >= 0 && id == 0; i--)
        if (x >= g_boxes[i].x0 && x < g_boxes[i].x1 && y >= g_boxes[i].y0 && y < g_boxes[i].y1) id = g_boxes[i].id;
    ReleaseSRWLockShared(&g_boxLock);
    return id != 0 && ScreenTakesClicks(id) ? id : 0;
}

bool CursorInClient(POINT* at)
{
    return g_window != NULL && GetCursorPos(at) && ScreenToClient(g_window, at);
}

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

void NoteButton(uint32_t button, bool down, POINT at)
{
    UINT message = button == HF_MOUSE_LEFT ? WM_LBUTTONDOWN : button == HF_MOUSE_RIGHT ? WM_RBUTTONDOWN : WM_MBUTTONDOWN;
    Note(g_window, down ? message : message + 1, 0, MAKELPARAM(at.x, at.y)); /* each WM_xBUTTONUP follows its DOWN */
}

/* The screen whose box took a press; another screen's buttons are not this one's. */
void TakeOwner(int id)
{
    if (InterlockedExchange(&g_clickOwner, id) == id) return;
    AcquireSRWLockExclusive(&g_pendingLock);
    g_pending.down = g_pending.pressed = g_pending.released = 0;
    ReleaseSRWLockExclusive(&g_pendingLock);
}

/* A button's press or release outside a modal screen: true when it is a click box's (pressed inside one, or the release
 * of such a press), so the game must not get it. held is the press's record for the way it came (messages or raw
 * input); note puts it into the owning screen's input. */
bool Claim(uint32_t button, bool down, POINT at, volatile LONG* held, bool note)
{
    int id = down ? BoxAt((float)at.x, (float)at.y) : 0;
    bool ours = down ? id != 0 : (*held & (LONG)button) != 0;
    if (down && ours) InterlockedOr(held, (LONG)button);
    else InterlockedAnd(held, ~(LONG)button);
    if (!ours || !note) return ours;
    if (down) TakeOwner(id);
    NoteButton(button, down, at);
    return true;
}

/* A button's window message with no modal screen open: true when a click box takes it. */
bool TakeClick(UINT message, LPARAM l)
{
    bool down = false;
    uint32_t button = ButtonOf(message, &down);
    if (button == 0) return false;
    InterlockedExchange(&g_sawButtonMessage, 1);
    POINT at = {(short)LOWORD(l), (short)HIWORD(l)};
    return Claim(button, down, at, &g_heldByMessage, true);
}

/* One raw mouse record with no modal screen open: the buttons a click box takes are cleared from it. When the game's
 * window gets no button messages (raw input only), these presses are what the screen sees. */
void FilterRaw(RAWINPUT* input)
{
    if (input->header.dwType != RIM_TYPEMOUSE) return;
    USHORT& flags = input->data.mouse.usButtonFlags;
    static const struct { uint32_t button; USHORT down, up; } kinds[] = {
        {HF_MOUSE_LEFT, RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP},
        {HF_MOUSE_RIGHT, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP},
        {HF_MOUSE_MIDDLE, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP},
    };
    const USHORT buttons = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_DOWN |
                           RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_UP;
    if (!(flags & buttons)) return;
    POINT at;
    if (!CursorInClient(&at)) return;
    bool note = !g_sawButtonMessage;
    for (const auto& k : kinds)
    {
        if ((flags & k.down) && Claim(k.button, true, at, &g_heldByRaw, note)) flags &= (USHORT)~k.down;
        if ((flags & k.up) && Claim(k.button, false, at, &g_heldByRaw, note)) flags &= (USHORT)~k.up;
    }
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
 * to the host, which shows the game's own pointer, or none where the screen draws its own cursor; true when the message
 * was taken. The game gets them back after. */
bool PointWithGameCursor(UINT message, LPARAM l)
{
    HCURSOR cursor = g_gameCursor;
    bool ours = message == WM_SETCURSOR ? LOWORD(l) == HTCLIENT : message == WM_MOUSEMOVE;
    if (g_captured && cursor != NULL && ours)
    {
        SetCursor(g_pointerHidden ? NULL : cursor);
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
    if (!g_captured && TakeClick(message, l)) return 0;
    if (g_captured)
    {
        bool down = false;
        uint32_t button = ButtonOf(message, &down);
        if (button != 0 && !down) InterlockedAnd(&g_heldByMessage, ~(LONG)button); /* a box's press the modal screen ends */
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
    if (g_captured && data != NULL) return 0;
    if (data != NULL && read != (UINT)-1 && header == sizeof(RAWINPUTHEADER))
    {
        /* NEXTRAWINPUTBLOCK, which MinGW's headers cannot expand (no QWORD): records are 8-byte aligned */
        RAWINPUT* at = data;
        for (UINT i = 0; i < read; i++)
        {
            FilterRaw(at);
            at = (RAWINPUT*)(((ULONG_PTR)at + at->header.dwSize + 7) & ~(ULONG_PTR)7);
        }
    }
    return read;
}
/* The same for raw input read one WM_INPUT at a time. */
UINT WINAPI RawInputDataDetour(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header)
{
    UINT read = g_rawInputData(input, command, data, size, header);
    if (!g_captured && command == RID_INPUT && data != NULL && read != (UINT)-1 && read >= sizeof(RAWINPUTHEADER) &&
        header == sizeof(RAWINPUTHEADER))
        FilterRaw((RAWINPUT*)data);
    return read;
}
/* A mouse button a click box holds reads as up to the game. */
uint32_t HeldButtonOf(int key)
{
    LONG held = g_heldByMessage | g_heldByRaw;
    uint32_t button = key == VK_LBUTTON ? HF_MOUSE_LEFT : key == VK_RBUTTON ? HF_MOUSE_RIGHT : key == VK_MBUTTON ? HF_MOUSE_MIDDLE : 0;
    return !g_captured ? button & (uint32_t)held : 0;
}
SHORT WINAPI GetKeyStateDetour(int key)
{
    SHORT state = g_getKeyState(key);
    return HeldButtonOf(key) ? (SHORT)(state & 1) : state;
}
SHORT WINAPI GetAsyncKeyStateDetour(int key)
{
    SHORT state = g_getAsyncKeyState(key);
    return HeldButtonOf(key) ? 0 : state;
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

bool ImGuiDrawsCursor(bool captured) { return captured && g_gameCursor == NULL && !g_pointerHidden; }

void FollowPointer(bool hidden)
{
    LONG now = hidden && g_captured;
    /* a mouse at rest gets no WM_SETCURSOR of its own when the screen changes its mind */
    if (InterlockedExchange(&g_pointerHidden, now) != now && g_window != NULL)
        PostMessageW(g_window, WM_SETCURSOR, (WPARAM)g_window, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void HookCursor()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == NULL) return;
    int failed = Hook((void*)GetProcAddress(user32, "ClipCursor"), (void*)ClipCursorDetour, (void**)&g_clipCursor);
    failed |= Hook((void*)GetProcAddress(user32, "SetCursorPos"), (void*)SetCursorPosDetour, (void**)&g_setCursorPos);
    failed |= Hook((void*)GetProcAddress(user32, "GetRawInputBuffer"), (void*)RawInputBufferDetour, (void**)&g_rawInputBuffer);
    if (failed) Log("overlay: the cursor calls could not all be hooked; the game may still hold the cursor while a screen is open");
    int clicks = Hook((void*)GetProcAddress(user32, "GetRawInputData"), (void*)RawInputDataDetour, (void**)&g_rawInputData);
    clicks |= Hook((void*)GetProcAddress(user32, "GetKeyState"), (void*)GetKeyStateDetour, (void**)&g_getKeyState);
    clicks |= Hook((void*)GetProcAddress(user32, "GetAsyncKeyState"), (void*)GetAsyncKeyStateDetour, (void**)&g_getAsyncKeyState);
    if (clicks) Log("overlay: the mouse button calls could not all be hooked; a click in a screen's click box may also reach the game");
}

bool SubclassWindow(HWND window)
{
    g_gameProc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)HostProc);
    return g_gameProc != NULL;
}

void ResetInput()
{
    InterlockedExchange(&g_heldByMessage, 0);
    InterlockedExchange(&g_heldByRaw, 0);
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

void TakeInput(HfInput* out, bool captured, bool clicksOnly, float width, float height, float unitsPerPixel)
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
    if (!captured || !p.moved || clicksOnly)
    {
        POINT at;
        if (CursorInClient(&at)) { p.x = (float)at.x; p.y = (float)at.y; }
    }
    out->x = p.x * sx * unitsPerPixel;
    out->y = p.y * sy * unitsPerPixel;
    if (!captured) return;
    if (clicksOnly) { p.wheel = 0; p.keyCount = 0; }
    out->wheel = p.wheel;
    out->down = p.down;
    out->pressed = p.pressed;
    out->released = p.released;
    out->key_count = p.keyCount;
    memcpy(out->keys, p.keys, sizeof out->keys);
}

void BeginClickBoxes(float width, float height)
{
    g_nextCount = 0;
    ClientScale(width, height, &g_boxScaleX, &g_boxScaleY);
}

void AddClickBox(int id, float x0, float y0, float x1, float y1)
{
    if (g_nextCount >= MAX_BOXES || !(x1 > x0) || !(y1 > y0)) return;
    g_nextBoxes[g_nextCount++] = Box{x0 / g_boxScaleX, y0 / g_boxScaleY, x1 / g_boxScaleX, y1 / g_boxScaleY, id};
}

void EndClickBoxes()
{
    AcquireSRWLockExclusive(&g_boxLock);
    memcpy(g_boxes, g_nextBoxes, sizeof(Box) * (size_t)g_nextCount);
    g_boxCount = g_nextCount;
    ReleaseSRWLockExclusive(&g_boxLock);
}

int ClickOwner() { return g_clickOwner; }
} // namespace overlay
