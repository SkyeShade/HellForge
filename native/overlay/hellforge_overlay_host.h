/* HellForge's overlay host, hellforge_overlay.dll (docs/modding/overlay.md): the C exports the mod loader calls to draw
 * SDK 17 screens over the game. Mods never load it themselves; they call add_screen and screen_show in HfApi. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../mod-sdk/hellforge_overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HF_OVERLAY_HOST
#define HF_OVERLAY_API __declspec(dllexport)
#else
#define HF_OVERLAY_API
#endif

/* What the host needs from the process that loads it. The host hooks through `hook` so its hooks join the loader's
 * chains instead of fighting them; it never patches code itself. */
typedef struct HfOverlayStart
{
    uint32_t size; /* sizeof(HfOverlayStart) */
    void* user;    /* passed back to log and hook */
    /* One line for the log (UTF-8, no line break). Any thread. */
    void (*log)(void* user, const char* line);
    /* As HfApi.hook: 0 when the detour is in, *original then calling on. */
    int (*hook)(void* user, uintptr_t target, void* detour, void** original);
    void* window;            /* the game's HWND, or NULL: then the swap chain's own window */
    const char* font_body;   /* the game's regular face (fs-sinclair.ttf), UTF-8 path; NULL or missing: ImGui's own */
    const char* font_title;  /* its medium face (fs-sinclair-medium.ttf); NULL or missing: the body face */
} HfOverlayStart;

/* Hooks the swap chain and starts drawing with the next frame. Call once the game's renderer is up. 0 started (or
 * started already), 1 not (the log says why). */
HF_OVERLAY_API int hf_overlay_start(const HfOverlayStart* start);
/* A screen, before or after hf_overlay_start, from any thread: its id (1 and up), or 0 refused (the log says why). */
HF_OVERLAY_API int hf_overlay_add_screen(const HfScreen* screen);
/* Opens (1) or closes (0) a screen, from any thread. Closing sets input->closing; draw keeps being called until it
 * returns 0. */
HF_OVERLAY_API void hf_overlay_show(int id, int open);

/* For C++ screens that draw with Dear ImGui themselves: the host's context and heap. Their DLL has its own ImGui
 * globals, so it calls ImGui::SetAllocatorFunctions(alloc, free, alloc_user) and then
 * ImGui::SetCurrentContext((ImGuiContext*)context), and uses ImGui only inside its screen's draw. */
typedef struct HfOverlayImGui
{
    uint32_t size; /* sizeof(HfOverlayImGui) */
    /* in: the caller's IMGUI_CHECKVERSION() arguments; HF_OVERLAY_IMGUI_LAYOUT fills size and these */
    const char* version;
    size_t io, style, vec2, vec4, vertex, index;
    /* out */
    void* context; /* ImGuiContext* */
    void* (*alloc)(size_t bytes, void* user);
    void (*free)(void* pointer, void* user);
    void* alloc_user;
} HfOverlayImGui;

/* Fills the out fields when the caller's ImGui is the host's (same version, same struct sizes, so built with
 * IMGUI_DISABLE_OBSOLETE_FUNCTIONS too). 0 filled; 1 another version or layout, or no context yet (before the first
 * frame). Use: HfOverlayImGui shared = {HF_OVERLAY_IMGUI_LAYOUT}; */
HF_OVERLAY_API int hf_overlay_imgui(HfOverlayImGui* shared);
typedef int (*HfOverlayImGuiFn)(HfOverlayImGui*);

#ifdef IMGUI_VERSION
#define HF_OVERLAY_IMGUI_LAYOUT sizeof(HfOverlayImGui), IMGUI_VERSION, sizeof(ImGuiIO), sizeof(ImGuiStyle), sizeof(ImVec2), \
    sizeof(ImVec4), sizeof(ImDrawVert), sizeof(ImDrawIdx)
#endif

typedef int (*HfOverlayStartFn)(const HfOverlayStart*);
typedef int (*HfOverlayAddScreenFn)(const HfScreen*);
typedef void (*HfOverlayShowFn)(int, int);

#ifdef __cplusplus
}
#endif
