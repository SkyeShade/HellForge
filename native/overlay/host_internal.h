/* What the overlay host's files share (hellforge_overlay_host.h is its outside). One lock, g_lock, covers ImGui and
 * every device object: the window thread takes it for messages, the render thread for frames. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>
#include <string>
#include "imgui.h"
#include "hellforge_overlay_host.h"

namespace overlay
{
/* ---- host.cpp: the log, the lock, the screens ---- */
void Log(const char* format, ...);
int Hook(void* target, void* detour, void** original);
extern CRITICAL_SECTION g_lock;
extern HWND g_window;            /* the game's window, once known */
extern volatile LONG g_captured; /* a modal screen has the mouse and keys */
bool AnyScreenWanted();          /* a screen is open, opening or closing */
/* Draws every screen that is open into this frame's ImGui foreground list (render thread, g_lock held). */
void DrawScreens(float width, float height, float dt);
/* The window's thread: a key pressed (true when it was a screen's and is eaten). */
bool ScreenKey(UINT key);
/* The screen that has the input now (the last open modal one), for Esc; 0 none. */
int CapturingScreen();
/* The loader's game_cursor for the game's window; NULL when it gave none or knows none. */
HCURSOR GameCursor();

/* ---- input.cpp: the window procedure and the cursor ---- */
void HookCursor();
bool SubclassWindow(HWND window);
void ResetInput();
/* This frame's input for the screen that has it (mouse in back-buffer pixels until DrawScreens scales it). */
void TakeInput(HfInput* out, bool captured, float width, float height, float unitsPerPixel);
void ReleaseCursorClip();
/* After a capture change: the game's pointer while a screen has the mouse (NULL: ImGui draws its arrow). */
void FollowCursor(bool captured);
/* Whether ImGui draws its arrow this frame: the game hides its cursor in play, and the game's pointer is unknown. */
bool ImGuiDrawsCursor(bool captured);
/* After the screens' frame: the pointer hidden while the screen with the input asked for it, else shown. */
void FollowPointer(bool hidden);
extern HCURSOR volatile g_gameCursor;

/* ---- present.cpp: the swap chain's and the queue's hooks ---- */
bool HookPresent();
typedef void(STDMETHODCALLTYPE* ExecuteFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
extern ExecuteFn g_execute;

/* ---- render.cpp: devices, frames and HDR ---- */
enum { HDR_OFF, HDR_PQ, HDR_SCRGB };
extern int g_hdrMode;
extern ID3D11Device* g_device11;
extern ID3D11DeviceContext* g_context11;
extern ID3D12Device* g_device12;
extern ID3D12CommandQueue* g_queue;
extern ID3D12DescriptorHeap* g_srvHeap;
extern ID3D12GraphicsCommandList* g_frameList; /* this frame's, while Draw12 records */
extern UINT64 g_fenceValue;               /* the value this frame signals once its list is submitted */
extern UINT g_frameWidth, g_frameHeight;  /* this frame's back buffer */
UINT64 CompletedFence();
void WaitFence(UINT64 value);
void Render(IDXGISwapChain3* swapChain);
void WaitLastFrame();
void NoteQueue(ID3D12CommandQueue* queue, void* caller);
bool HaveDevice();
DXGI_FORMAT BackBufferFormat();

/* ---- slots.cpp: the D3D12 view heap ---- */
bool CreateSlots(ID3D12Device* device);
int AllocSlot();                    /* -1 full */
void FreeSlotLater(int slot);       /* once the frame being recorded is done */
int SlotOf(D3D12_CPU_DESCRIPTOR_HANDLE cpu);
D3D12_CPU_DESCRIPTOR_HANDLE SlotCpu(int slot);
D3D12_GPU_DESCRIPTOR_HANDLE SlotGpu(int slot);
void ReleaseLater(IUnknown* object); /* released once the frame being recorded is done */
void SweepLater();                   /* what the GPU is done with, released */

/* ---- images.cpp ---- */
HfImage* ImageCreate(uint32_t width, uint32_t height, const void* rgba);
void ImageUpdate(HfImage* image, const void* rgba);
void ImageFree(HfImage* image);
ImTextureID ImageTexture(HfImage* image);
void FlushUploads12(ID3D12GraphicsCommandList* list); /* the pending uploads, recorded before ImGui's draw */

/* ---- backdrop.cpp ---- */
void Backdrop(ImDrawList* list, float dim, float desaturate, float blurPixels, bool* captured);
void CaptureBackdrop11(ID3D11Texture2D* backBuffer);
void CaptureBackdrop12(ID3D12GraphicsCommandList* list, ID3D12Resource* backBuffer);
void BackdropFormatChanged();

/* ---- draw.cpp: HfDraw, the fonts ---- */
extern bool* g_backdropTaken; /* the drawing screen's "its frame is copied" */
extern bool g_mayHidePointer;  /* the screen drawing now has the input, so hide_pointer counts */
extern bool g_pointerAsked;    /* hide_pointer was called this frame */
void SetFontFiles(const char* body, const char* title);
bool BakeFonts(float pixelsPerUnit); /* true when the atlas changed */
void BeginDraw(HfDraw* draw, float width, float height);
void EndScreenDraw(); /* clips a screen left pushed, popped */
} // namespace overlay
