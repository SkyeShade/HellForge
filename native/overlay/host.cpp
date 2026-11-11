/* HellForge's overlay host (docs/modding/overlay.md): the exports, the log and the screens. A screen goes CLOSED ->
 * OPENING (asked from any thread) -> OPEN (the render thread called opened(1)) -> CLOSING (asked again) -> CLOSED once
 * its draw returns 0. Input belongs to the screen from OPENING until CLOSING, so the click after the key never reaches
 * the game. */
#ifndef HF_OVERLAY_HOST
#define HF_OVERLAY_HOST
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "host_internal.h"

namespace overlay
{
CRITICAL_SECTION g_lock;
HWND g_window;
volatile LONG g_captured;

namespace
{
enum { CLOSED, OPENING, OPEN, CLOSING };
const int MAX_SCREENS = 64;

struct Screen
{
    HfScreen def;
    char name[64];
    volatile LONG state;
    bool backdropTaken; /* this opening's copy of the frame is made (backdrop) */
};

Screen g_screens[MAX_SCREENS];
volatile LONG g_screenCount;
SRWLOCK g_screensLock = SRWLOCK_INIT;

HfOverlayStart g_start;
volatile LONG g_started;
/* lines logged before hf_overlay_start gave a log, said once it does */
char g_early[16][256];
int g_earlyCount;
SRWLOCK g_logLock = SRWLOCK_INIT;

/* Whether a modal screen has the input, and which (the last such in registration order); g_screensLock held. */
int FindCapturing()
{
    LONG count = g_screenCount;
    for (LONG i = count - 1; i >= 0; i--)
    {
        LONG state = g_screens[i].state;
        if ((g_screens[i].def.flags & HF_SCREEN_MODAL) && (state == OPENING || state == OPEN)) return (int)i + 1;
    }
    return 0;
}

/* After a state change: the input taken from the game, or given back. */
void FollowCapture()
{
    LONG captured = FindCapturing() != 0;
    if (InterlockedExchange(&g_captured, captured) == captured) return;
    if (captured)
    {
        ResetInput();
        ReleaseCursorClip();
    }
}

/* show(): the asked change of one screen; g_screensLock held. */
void Ask(Screen& s, bool open)
{
    LONG state = s.state;
    if (open && state == CLOSED) InterlockedExchange(&s.state, OPENING);
    else if (open && state == CLOSING) InterlockedExchange(&s.state, OPEN);
    else if (!open && state == OPENING) InterlockedExchange(&s.state, CLOSED); /* never shown: no callbacks */
    else if (!open && state == OPEN) InterlockedExchange(&s.state, CLOSING);
    else return;
    Log("overlay: %s %s", s.name, open ? "opens" : state == OPENING ? "is not opened after all" : "closes");
    FollowCapture();
}

const char* Refusal(const HfScreen* screen)
{
    if (screen == NULL || screen->size < offsetof(HfScreen, opened)) return "its HfScreen is missing or too small";
    if (screen->draw == NULL) return "it has no draw callback";
    if (screen->key > 0xFE) return "its key is not a virtual-key code";
    return NULL;
}
} // namespace

void Log(const char* format, ...)
{
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    AcquireSRWLockExclusive(&g_logLock);
    if (g_start.log != NULL) g_start.log(g_start.user, line);
    else if (g_earlyCount < 16) snprintf(g_early[g_earlyCount++], sizeof g_early[0], "%s", line);
    ReleaseSRWLockExclusive(&g_logLock);
}

int Hook(void* target, void* detour, void** original)
{
    return target != NULL && g_start.hook != NULL ? g_start.hook(g_start.user, (uintptr_t)target, detour, original) : 1;
}

bool AnyScreenWanted()
{
    LONG count = g_screenCount;
    for (LONG i = 0; i < count; i++)
        if (g_screens[i].state != CLOSED) return true;
    return false;
}

int CapturingScreen()
{
    AcquireSRWLockShared(&g_screensLock);
    int id = FindCapturing();
    ReleaseSRWLockShared(&g_screensLock);
    return id;
}

bool ScreenKey(UINT key)
{
    LONG count = g_screenCount;
    for (LONG i = 0; i < count; i++)
    {
        Screen& s = g_screens[i];
        if (s.def.key != key) continue;
        LONG state = s.state;
        bool open = state == CLOSED || state == CLOSING;
        /* asked outside every lock: a mod's can_open may call back into the host */
        if (state == CLOSED && s.def.can_open != NULL && !s.def.can_open(s.def.user)) return false;
        hf_overlay_show((int)i + 1, open);
        return true;
    }
    return false;
}

/* One screen's frame. It opens first when asked; draw returning 0 closes it, or reopens it when show(1) came after. */
static void DrawScreen(Screen& s, int id, int capturing, HfDraw* draw, float dt)
{
    if (InterlockedCompareExchange(&s.state, OPEN, OPENING) == OPENING)
    {
        s.backdropTaken = false;
        if (s.def.opened != NULL) s.def.opened(s.def.user, 1);
    }
    LONG seen = s.state;
    if (seen != OPEN && seen != CLOSING) return;
    HfInput input;
    TakeInput(&input, id == capturing && seen != CLOSING, draw->width * draw->pixels, draw->height * draw->pixels, 1.0f / draw->pixels);
    input.closing = seen == CLOSING;
    g_backdropTaken = &s.backdropTaken;
    int again = s.def.draw(s.def.user, draw, &input, dt);
    EndScreenDraw();
    if (again) return;
    AcquireSRWLockExclusive(&g_screensLock);
    bool reopened = InterlockedCompareExchange(&s.state, CLOSED, seen) != seen;
    if (reopened) InterlockedExchange(&s.state, OPENING);
    FollowCapture();
    ReleaseSRWLockExclusive(&g_screensLock);
    Log("overlay: %s closed%s", s.name, reopened ? " and opens again" : "");
    if (s.def.opened != NULL) s.def.opened(s.def.user, 0);
}

void DrawScreens(float width, float height, float dt)
{
    HfDraw draw;
    BeginDraw(&draw, width, height);
    int capturing = CapturingScreen();
    LONG count = g_screenCount;
    for (LONG i = 0; i < count; i++) DrawScreen(g_screens[i], (int)i + 1, capturing, &draw, dt);
    g_backdropTaken = NULL;
}
} // namespace overlay

using namespace overlay;

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_lock);
    return TRUE;
}

extern "C" HF_OVERLAY_API int hf_overlay_start(const HfOverlayStart* start)
{
    if (start == NULL || start->size < sizeof(HfOverlayStart) || start->log == NULL || start->hook == NULL) return 1;
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return 0;
    AcquireSRWLockExclusive(&g_logLock);
    g_start = *start;
    for (int i = 0; i < g_earlyCount; i++) g_start.log(g_start.user, g_early[i]);
    g_earlyCount = 0;
    ReleaseSRWLockExclusive(&g_logLock);
    g_window = (HWND)start->window;
    SetFontFiles(start->font_body, start->font_title);
    if (HookPresent()) return 0;
    Log("overlay: the swap chain could not be hooked; no screens are drawn");
    return 1;
}

/* Why a screen cannot join the ones registered, or NULL; g_screensLock held. */
static const char* Clash(const HfScreen* screen)
{
    LONG count = g_screenCount;
    for (LONG i = 0; i < count; i++)
        if (screen->key != 0 && g_screens[i].def.key == screen->key) return "its key is another screen's";
    return count >= MAX_SCREENS ? "the host has 64 screens already" : NULL;
}

/* The screen kept in the next slot, its id returned; g_screensLock held. */
static int Keep(const HfScreen* screen)
{
    LONG count = g_screenCount;
    Screen& s = g_screens[count];
    memset(&s, 0, sizeof s);
    memcpy(&s.def, screen, screen->size < sizeof s.def ? screen->size : sizeof s.def);
    s.def.size = sizeof s.def;
    snprintf(s.name, sizeof s.name, "%s", screen->name != NULL && screen->name[0] ? screen->name : "a screen");
    s.def.name = s.name;
    InterlockedExchange(&g_screenCount, count + 1); /* the render thread reads the count without the lock */
    return (int)count + 1;
}

extern "C" HF_OVERLAY_API int hf_overlay_add_screen(const HfScreen* screen)
{
    const char* why = Refusal(screen);
    AcquireSRWLockExclusive(&g_screensLock);
    if (why == NULL) why = Clash(screen);
    int id = why == NULL ? Keep(screen) : 0;
    ReleaseSRWLockExclusive(&g_screensLock);
    if (why != NULL)
    {
        Log("overlay: a screen (%s) refused: %s", screen != NULL && screen->name ? screen->name : "unnamed", why);
        return 0;
    }
    Log("overlay: screen %d, %s (key 0x%02x%s)", id, g_screens[id - 1].name, screen->key, (screen->flags & HF_SCREEN_MODAL) ? ", modal" : "");
    return id;
}

extern "C" HF_OVERLAY_API void hf_overlay_show(int id, int open)
{
    AcquireSRWLockExclusive(&g_screensLock);
    if (id >= 1 && id <= g_screenCount) Ask(g_screens[id - 1], open != 0);
    ReleaseSRWLockExclusive(&g_screensLock);
}

extern "C" HF_OVERLAY_API int hf_overlay_imgui(HfOverlayImGui* shared)
{
    if (shared == NULL) return 1;
    HfOverlayImGui& s = *shared;
    if (s.size < sizeof(HfOverlayImGui) || s.version == NULL || strcmp(s.version, IMGUI_VERSION) != 0) return 1;
    if (s.io != sizeof(ImGuiIO) || s.style != sizeof(ImGuiStyle) || s.vec2 != sizeof(ImVec2) || s.vec4 != sizeof(ImVec4) ||
        s.vertex != sizeof(ImDrawVert) || s.index != sizeof(ImDrawIdx)) return 1;
    EnterCriticalSection(&g_lock);
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (context != NULL)
    {
        s.context = context;
        ImGui::GetAllocatorFunctions(&s.alloc, &s.free, &s.alloc_user);
    }
    LeaveCriticalSection(&g_lock);
    return context != NULL ? 0 : 1;
}
