/* The overlay host against a real swap chain, without the game: a window that presents frames like the game does
 * (clear, ExecuteCommandLists, Present), hellforge_overlay.dll loaded and started with a stand-in hook (MinHook), a
 * demo modal screen that uses every HfDraw call, and a non-modal one over it. It presses the modal screen's key, sends
 * mouse and keys, closes it with its key, with Esc and with hf_overlay_show, and resizes the swap chain. Each
 * screenshot is the back buffer read just before the real Present, after the host drew into it, and is checked pixel
 * by pixel against what the screens drew, in the back buffer's own encoding. The stand-in loader offers a game pointer
 * for the first opening and none for the second: the host shows it, or ImGui draws its arrow.
 * Usage: overlay_host_test.exe <hellforge_overlay.dll> [<screenshot.bmp>]
 * HELLFORGE_OVERLAY_TEST_D3D11=1: Direct3D 11 instead of 12. HELLFORGE_OVERLAY_TEST_FORMAT=rgba8 (default) | rgb10a2 | fp16.
 * HELLFORGE_OVERLAY_HDR=pq:200 or scrgb:200 (read by the host): its colours must come out PQ-encoded or scRGB.
 * HELLFORGE_OVERLAY_TEST_FONT=<a .ttf>: the font the host is started with (ImGui's own font without one). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "MinHook.h"
#include "imgui.h" /* only IMGUI_VERSION and the structs' sizes, for hf_overlay_imgui */
#include "hellforge_overlay_host.h"

static int g_failures;
static void Check(bool ok, const char* what, ...)
{
    char line[512];
    va_list a;
    va_start(a, what);
    vsnprintf(line, sizeof line, what, a);
    va_end(a);
    printf("  %s %s\n", ok ? "ok  " : "FAIL", line);
    if (!ok) g_failures++;
}

/* ---- the stand-in loader: log and hook ---- */
static std::vector<std::string> g_log;
static void Log(void*, const char* line) { printf("  host: %s\n", line); g_log.push_back(line); }
static bool Logged(const char* text) { for (const std::string& l : g_log) if (l.find(text) != std::string::npos) return true; return false; }

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain3*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain3*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static void *g_presentCode, *g_present1Code;
static PresentFn g_realPresent;
static Present1Fn g_realPresent1;
static bool g_wantShot;
static void Capture(IDXGISwapChain3* chain);
static HRESULT STDMETHODCALLTYPE TapPresent(IDXGISwapChain3* chain, UINT interval, UINT flags)
{
    if (g_wantShot) { Capture(chain); g_wantShot = false; }
    return g_realPresent(chain, interval, flags);
}
static HRESULT STDMETHODCALLTYPE TapPresent1(IDXGISwapChain3* chain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    if (g_wantShot) { Capture(chain); g_wantShot = false; }
    return g_realPresent1(chain, interval, flags, p);
}
/* The host's original for the window's Present is the tap, which reads the back buffer and calls the real one. */
static int Hook(void*, uintptr_t target, void* detour, void** original)
{
    void* real = NULL;
    if (MH_CreateHook((void*)target, detour, &real) != MH_OK) return 1;
    *original = real;
    if ((void*)target == g_presentCode) { g_realPresent = (PresentFn)real; *original = (void*)TapPresent; }
    if ((void*)target == g_present1Code) { g_realPresent1 = (Present1Fn)real; *original = (void*)TapPresent1; }
    return MH_EnableHook((void*)target) == MH_OK ? 0 : 1;
}

/* ---- the "game": its window, device and frames ---- */
static int g_gameKeys[256], g_gameButtons, g_gameWheel, g_gameSetCursor;
static LRESULT CALLBACK GameProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_SETCURSOR) g_gameSetCursor++;
    if (message == WM_KEYDOWN && w < 256) g_gameKeys[w]++;
    if (message == WM_LBUTTONDOWN) g_gameButtons++;
    if (message == WM_MOUSEWHEEL) g_gameWheel++;
    return DefWindowProcW(window, message, w, l);
}

enum Format { RGBA8, RGB10A2, FP16 };
static Format g_format;
static bool g_d3d11;
static UINT g_width = 1280, g_height = 720;
static ID3D12Device* g_device;
static ID3D12CommandQueue* g_queue;
static ID3D12CommandAllocator* g_allocator;
static ID3D12GraphicsCommandList* g_list;
static ID3D12Fence* g_fence;
static HANDLE g_event;
static UINT64 g_value;
static ID3D12DescriptorHeap* g_rtvHeap;
static ID3D11Device* g_device11;
static ID3D11DeviceContext* g_context11;
static IDXGISwapChain3* g_chain;

/* What the game draws, in the back buffer's own values (codes for UNORM, linear for FP16): one colour, a bar of
 * another on the left quarter. */
static const float kGame[3] = {0.6f, 0.3f, 0.1f};
static const float kBar[3] = {0.1f, 0.5f, 0.8f};

static void WaitGpu()
{
    g_queue->Signal(g_fence, ++g_value);
    g_fence->SetEventOnCompletion(g_value, g_event);
    WaitForSingleObject(g_event, 5000);
}

static void GameFrame12()
{
    UINT index = g_chain->GetCurrentBackBufferIndex();
    ID3D12Resource* buffer = NULL;
    g_chain->GetBuffer(index, IID_PPV_ARGS(&buffer));
    g_allocator->Reset();
    g_list->Reset(g_allocator, NULL);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += index * g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_device->CreateRenderTargetView(buffer, NULL, rtv);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET};
    g_list->ResourceBarrier(1, &b);
    const float game[4] = {kGame[0], kGame[1], kGame[2], 1}, bar[4] = {kBar[0], kBar[1], kBar[2], 1};
    D3D12_RECT left = {0, 0, (LONG)g_width / 4, (LONG)g_height};
    g_list->ClearRenderTargetView(rtv, game, 0, NULL);
    g_list->ClearRenderTargetView(rtv, bar, 1, &left);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_list->ResourceBarrier(1, &b);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    WaitGpu();
    buffer->Release();
}

static void GameFrame11()
{
    ID3D11Texture2D* back = NULL;
    ID3D11RenderTargetView* view = NULL;
    ID3D11DeviceContext1* context1 = NULL;
    g_chain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back && SUCCEEDED(g_device11->CreateRenderTargetView(back, NULL, &view)) && SUCCEEDED(g_context11->QueryInterface(IID_PPV_ARGS(&context1))))
    {
        const float game[4] = {kGame[0], kGame[1], kGame[2], 1}, bar[4] = {kBar[0], kBar[1], kBar[2], 1};
        D3D11_RECT left = {0, 0, (LONG)g_width / 4, (LONG)g_height};
        context1->ClearView(view, game, NULL, 0);
        context1->ClearView(view, bar, &left, 1);
        g_context11->OMSetRenderTargets(1, &view, NULL);
    }
    if (context1) context1->Release();
    if (view) view->Release();
    if (back) back->Release();
}

/* ---- screenshots: the back buffer as floats (codes, or linear for FP16) ---- */
static std::vector<float> g_shot; /* rgb per pixel */
static UINT g_shotWidth, g_shotHeight;

static float Half(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float v = exponent == 0 ? ldexpf((float)mantissa, -24) : exponent == 31 ? INFINITY : ldexpf((float)(mantissa | 1024), (int)exponent - 25);
    return sign ? -v : v;
}

static void Decode(const uint8_t* row, UINT width, float* out)
{
    for (UINT x = 0; x < width; x++)
    {
        if (g_format == RGBA8) for (int c = 0; c < 3; c++) out[3 * x + c] = row[4 * x + c] / 255.0f;
        else if (g_format == RGB10A2)
        {
            uint32_t v;
            memcpy(&v, row + 4 * x, 4);
            for (int c = 0; c < 3; c++) out[3 * x + c] = ((v >> (10 * c)) & 1023) / 1023.0f;
        }
        else for (int c = 0; c < 3; c++) { uint16_t h; memcpy(&h, row + 8 * x + 2 * c, 2); out[3 * x + c] = Half(h); }
    }
}

static void Capture12(ID3D12Resource* buffer)
{
    D3D12_RESOURCE_DESC desc = buffer->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot;
    UINT64 bytes = 0;
    g_device->GetCopyableFootprints(&desc, 0, 1, 0, &foot, NULL, NULL, &bytes);
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rb = {};
    rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb.Width = bytes;
    rb.Height = rb.DepthOrArraySize = rb.MipLevels = 1;
    rb.SampleDesc.Count = 1;
    rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback = NULL;
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&readback)))) return;
    g_allocator->Reset();
    g_list->Reset(g_allocator, NULL);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE};
    g_list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION to = {readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}, from = {buffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    to.PlacedFootprint = foot;
    from.SubresourceIndex = 0;
    g_list->CopyTextureRegion(&to, 0, 0, 0, &from, NULL);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_list->ResourceBarrier(1, &b);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    WaitGpu();
    uint8_t* mapped = NULL;
    if (SUCCEEDED(readback->Map(0, NULL, (void**)&mapped)))
    {
        g_shotWidth = (UINT)desc.Width;
        g_shotHeight = desc.Height;
        g_shot.assign((size_t)g_shotWidth * g_shotHeight * 3, 0);
        for (UINT y = 0; y < g_shotHeight; y++) Decode(mapped + foot.Offset + (size_t)y * foot.Footprint.RowPitch, g_shotWidth, &g_shot[(size_t)y * g_shotWidth * 3]);
        readback->Unmap(0, NULL);
    }
    readback->Release();
}

static void Capture11(ID3D11Texture2D* buffer)
{
    D3D11_TEXTURE2D_DESC desc;
    buffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = NULL;
    if (FAILED(g_device11->CreateTexture2D(&desc, NULL, &staging))) return;
    g_context11->CopyResource(staging, buffer);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(g_context11->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
    {
        g_shotWidth = desc.Width;
        g_shotHeight = desc.Height;
        g_shot.assign((size_t)g_shotWidth * g_shotHeight * 3, 0);
        for (UINT y = 0; y < g_shotHeight; y++) Decode((uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch, g_shotWidth, &g_shot[(size_t)y * g_shotWidth * 3]);
        g_context11->Unmap(staging, 0);
    }
    staging->Release();
}

static void Capture(IDXGISwapChain3* chain)
{
    if (g_d3d11)
    {
        ID3D11Texture2D* buffer = NULL;
        if (SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) { Capture11(buffer); buffer->Release(); }
        return;
    }
    ID3D12Resource* buffer = NULL;
    if (SUCCEEDED(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)))) { Capture12(buffer); buffer->Release(); }
}

static float ToSrgb(float l) { l = l < 0 ? 0 : l > 1 ? 1 : l; return l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1 / 2.4f) - 0.055f; }
static void SaveShot(const char* path)
{
    if (path == NULL || g_shot.empty()) return;
    UINT w = g_shotWidth, h = g_shotHeight, row = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> pixels((size_t)row * h);
    for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++)
            for (int c = 0; c < 3; c++)
            {
                float v = g_shot[((size_t)y * w + x) * 3 + c];
                if (g_format == FP16) v = ToSrgb(v);
                pixels[(size_t)y * row + x * 3 + (2 - c)] = (uint8_t)(fminf(fmaxf(v, 0), 1) * 255 + 0.5f);
            }
    BITMAPINFOHEADER header = {sizeof header, (LONG)w, -(LONG)h, 1, 24, BI_RGB};
    BITMAPFILEHEADER file = {0x4D42, (DWORD)(sizeof file + sizeof header + pixels.size()), 0, 0, (DWORD)(sizeof file + sizeof header)};
    if (FILE* out = fopen(path, "wb"))
    {
        fwrite(&file, sizeof file, 1, out);
        fwrite(&header, sizeof header, 1, out);
        fwrite(pixels.data(), pixels.size(), 1, out);
        fclose(out);
        printf("  screenshot: %s\n", path);
    }
}

/* ---- what the host should have written: its colour model, in the back buffer's values ---- */
static int g_hdr; /* 0 SDR, 1 PQ, 2 scRGB, as HELLFORGE_OVERLAY_HDR forces */
static float g_nits = 200;
static float Lin(float s) { return s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f); }
static float PqEncode(float l) { float m = powf(fmaxf(l, 0), 0.1593017578125f); return powf((0.8359375f + 18.8515625f * m) / (1 + 18.6875f * m), 78.84375f); }
static float PqDecode(float e) { float p = powf(fmaxf(e, 0), 1 / 78.84375f); return powf(fmaxf(p - 0.8359375f, 0) / (18.8515625f - 18.6875f * p), 1 / 0.1593017578125f); }

/* An sRGB colour the host drew (opaque), as the back buffer holds it. */
static void Expected(HfColour colour, float out[3])
{
    float l[3];
    for (int c = 0; c < 3; c++) l[c] = Lin(((colour >> (8 * c)) & 255) / 255.0f);
    if (g_hdr == 1)
    {
        const float m[9] = {0.6274f, 0.3293f, 0.0433f, 0.0691f, 0.9195f, 0.0114f, 0.0164f, 0.0880f, 0.8956f};
        for (int r = 0; r < 3; r++) out[r] = PqEncode((m[3 * r] * l[0] + m[3 * r + 1] * l[1] + m[3 * r + 2] * l[2]) * g_nits / 10000);
    }
    else if (g_hdr == 2) for (int c = 0; c < 3; c++) out[c] = l[c] * g_nits / 80;
    else for (int c = 0; c < 3; c++) out[c] = ((colour >> (8 * c)) & 255) / 255.0f;
}

/* The game's colour under the backdrop: decoded, desaturated, dimmed, encoded again. */
static void ExpectedBackdrop(const float game[3], float dim, float grey, float out[3])
{
    int decode = g_format == FP16 ? 2 : g_format == RGB10A2 && g_hdr == 1 ? 1 : 0;
    float l[3];
    for (int c = 0; c < 3; c++) l[c] = decode == 2 ? game[c] : decode == 1 ? PqDecode(game[c]) : Lin(game[c]);
    static const float rec2020[3] = {0.2627f, 0.6780f, 0.0593f}, rec709[3] = {0.2126f, 0.7152f, 0.0722f};
    const float* w = decode == 1 ? rec2020 : rec709;
    float y = w[0] * l[0] + w[1] * l[1] + w[2] * l[2], keep = powf(1 - dim, 2.2f);
    for (int c = 0; c < 3; c++)
    {
        float v = (l[c] + (y - l[c]) * grey) * keep;
        out[c] = decode == 2 ? v : decode == 1 ? PqEncode(v) : ToSrgb(v);
    }
}

static const float* Pixel(float ux, float uy) /* a place in screen units */
{
    float s = g_shotHeight / 1080.0f;
    UINT x = (UINT)(ux * s), y = (UINT)(uy * s);
    return &g_shot[((size_t)y * g_shotWidth + x) * 3];
}
static bool Near(const float* got, const float want[3])
{
    for (int c = 0; c < 3; c++)
        if (fabsf(got[c] - want[c]) > 0.025f + 0.03f * fabsf(want[c])) return false;
    return true;
}
static void CheckAt(float ux, float uy, const float want[3], const char* what)
{
    const float* p = Pixel(ux, uy);
    Check(Near(p, want), "%s at (%.0f, %.0f): %.3f %.3f %.3f, expected %.3f %.3f %.3f", what, ux, uy, p[0], p[1], p[2], want[0], want[1], want[2]);
}
static void CheckColour(float ux, float uy, HfColour colour, const char* what) { float want[3]; Expected(colour, want); CheckAt(ux, uy, want, what); }
static void CheckNot(float ux, float uy, HfColour colour, const char* what)
{
    float want[3];
    Expected(colour, want);
    const float* p = Pixel(ux, uy);
    Check(!Near(p, want), "%s at (%.0f, %.0f) is not that colour (%.3f %.3f %.3f)", what, ux, uy, p[0], p[1], p[2]);
}

/* ---- the screens ---- */
static const HfColour RED = HF_RGBA(220, 40, 40, 255), YELLOW = HF_RGBA(255, 220, 0, 255), GREEN = HF_RGBA(40, 200, 60, 255),
    BLUE = HF_RGBA(40, 80, 230, 255), WHITE = HF_RGBA(240, 240, 240, 255), MAGENTA = HF_RGBA(200, 0, 200, 255),
    ORANGE = HF_RGBA(255, 128, 0, 255), TEAL = HF_RGBA(0, 180, 180, 255), CYAN = HF_RGBA(0, 255, 255, 255),
    TEXT = HF_RGBA(255, 255, 255, 255);
static const HfColour kQuad[4] = {HF_RGBA(255, 0, 0, 255), HF_RGBA(0, 255, 0, 255), HF_RGBA(0, 0, 255, 255), HF_RGBA(255, 255, 0, 255)};
static const float DIM = 0.5f, GREY = 1.0f;

struct Demo
{
    int opened, closedCalls, frames, closingFrames;
    bool allowOpen = true, wantUpdate;
    HfImage* image;
    float width, pixels, titleWidth, bodyWidth;
    HfInput lastInput; /* the last frame's with something in it */
    uint32_t pressedSeen, releasedSeen;
    float wheelSeen;
    bool sawQ;
};
static Demo g_demo;
static int g_hudFrames;

static void DemoOpened(void* user, int open) { if (open) g_demo.opened++; else g_demo.closedCalls++; }
static int DemoCanOpen(void*) { return g_demo.allowOpen; }

static void Pixels(const HfColour colours[4], uint8_t out[16]) { for (int i = 0; i < 4; i++) memcpy(out + 4 * i, &colours[i], 4); }

static int DemoDraw(void*, const HfDraw* d, const HfInput* in, float dt)
{
    Demo& g = g_demo;
    g.frames++;
    g.width = d->width;
    g.pixels = d->pixels;
    if (in->pressed || in->released || in->wheel != 0 || in->key_count) g.lastInput = *in;
    g.pressedSeen |= in->pressed;
    g.releasedSeen |= in->released;
    g.wheelSeen += in->wheel;
    for (uint32_t i = 0; i < in->key_count; i++) g.sawQ |= in->keys[i] == 'Q';
    if (g.image == NULL) { uint8_t px[16]; Pixels(kQuad, px); g.image = d->image_create(2, 2, px); }
    if (g.wantUpdate && g.image != NULL)
    {
        const HfColour swapped[4] = {kQuad[3], kQuad[2], kQuad[1], kQuad[0]};
        uint8_t px[16];
        Pixels(swapped, px);
        d->image_update(g.image, px);
        g.wantUpdate = false;
    }
    d->backdrop(DIM, GREY, 12);
    d->rect(100, 100, 400, 300, RED, YELLOW, 10);
    const HfPoint line[3] = {{500, 100}, {800, 100}, {800, 300}};
    d->lines(line, 3, GREEN, 12, 0);
    const HfPoint ell[6] = {{900, 100}, {1200, 100}, {1200, 160}, {980, 160}, {980, 300}, {900, 300}}; /* concave */
    d->polygon(ell, 6, BLUE);
    d->circle(1450, 200, 100, WHITE, MAGENTA, 8);
    g.titleWidth = d->text(100, 450, "HELLFORGE OVERLAY", 48, HF_FACE_TITLE, HF_ALIGN_LEFT, TEXT);
    g.bodyWidth = d->text_width("1080p units", 24, HF_FACE_BODY);
    d->text(1820, 450, "1080p units", 24, HF_FACE_BODY, HF_ALIGN_RIGHT, TEXT);
    const HfRect box = {100, 550, 500, 950};
    d->image(g.image, &box, nullptr, HF_RGBA(255, 255, 255, 255));
    d->clip_push(600, 550, 900, 750);
    d->rect(500, 500, 1000, 1000, ORANGE, 0, 0);
    d->clip_pop();
    d->clip_push(1000, 550, 1400, 950);
    d->clip_push(1200, 700, 1600, 1000);
    d->rect(1000, 500, 1700, 1000, TEAL, 0, 0);
    d->clip_pop();
    d->clip_pop();
    (void)dt;
    if (!in->closing) return 1;
    return ++g.closingFrames < 5; /* its closing plays five frames */
}

static int HudDraw(void*, const HfDraw* d, const HfInput* in, float)
{
    g_hudFrames++;
    d->rect(1400, 150, 1500, 250, CYAN, 0, 0); /* over the modal screen's circle: drawn after it */
    return !in->closing;
}

/* ---- the game's own pointer, as the loader's game_cursor gives it ---- */
static HCURSOR g_pointer;
static bool g_offerPointer = true;
static void* GamePointer(void*, void*) { return g_offerPointer ? g_pointer : NULL; }
/* ShowCursor's count on this (the window's) thread */
static int CursorCount() { int count = ShowCursor(TRUE); ShowCursor(FALSE); return count - 1; }
static void SetCursorMessage(HWND window) { SendMessageW(window, WM_SETCURSOR, (WPARAM)window, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)); }
/* Bright samples where ImGui's arrow would be, with its tip at the mouse (640, 360 pixels: 960, 540 units). */
static int ArrowSamples()
{
    float white[3];
    Expected(TEXT, white);
    int lit = 0;
    for (float y = 540; y < 568; y += 1.5f)
        for (float x = 960; x < 978; x += 1.5f) lit += Near(Pixel(x, y), white);
    return lit;
}

/* ---- the run ---- */
static void Key(HWND window, WPARAM key) { SendMessageW(window, WM_KEYDOWN, key, 0); SendMessageW(window, WM_KEYUP, key, 0xC0000000); }
static void Click(HWND window, int x, int y)
{
    LPARAM at = MAKELPARAM(x, y);
    SendMessageW(window, WM_MOUSEMOVE, 0, at);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, at);
    SendMessageW(window, WM_LBUTTONUP, 0, at);
}

static bool MakeSwapChain(HWND window)
{
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = g_width;
    desc.Height = g_height;
    desc.Format = g_format == RGBA8 ? DXGI_FORMAT_R8G8B8A8_UNORM : g_format == RGB10A2 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGIFactory2* factory = NULL;
    IDXGISwapChain1* chain1 = NULL;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    if (g_d3d11)
    {
        if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &g_device11, NULL, &g_context11)) ||
            FAILED(factory->CreateSwapChainForHwnd(g_device11, window, &desc, NULL, NULL, &chain1))) return false;
    }
    else
    {
        D3D12_COMMAND_QUEUE_DESC q = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC rtv = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))) || FAILED(g_device->CreateCommandQueue(&q, IID_PPV_ARGS(&g_queue))) ||
            FAILED(factory->CreateSwapChainForHwnd(g_queue, window, &desc, NULL, NULL, &chain1))) return false;
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocator));
        g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocator, NULL, IID_PPV_ARGS(&g_list));
        g_list->Close();
        g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        g_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_rtvHeap));
        g_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
    bool ok = SUCCEEDED(chain1->QueryInterface(IID_PPV_ARGS(&g_chain)));
    chain1->Release();
    factory->Release();
    return ok;
}

/* The first screenshot: every HfDraw call where it drew. */
static void CheckDrawing()
{
    puts("  -- the demo screen's drawing");
    float game[3], bar[3];
    ExpectedBackdrop(kGame, DIM, GREY, game);
    ExpectedBackdrop(kBar, DIM, GREY, bar);
    CheckAt(1700, 800, game, "backdrop over the game's colour (dimmed, grey)");
    CheckAt(300, 1020, bar, "backdrop over the game's bar");
    CheckColour(250, 200, RED, "rect fill");
    CheckColour(250, 103, YELLOW, "rect outline (inside its box)");
    CheckColour(650, 100, GREEN, "a thick line");
    CheckColour(800, 200, GREEN, "the line's second segment");
    CheckColour(940, 250, BLUE, "concave polygon, its foot");
    CheckColour(1100, 130, BLUE, "concave polygon, its arm");
    CheckAt(1100, 250, game, "outside the concave polygon's corner (backdrop)");
    CheckColour(1450, 120, WHITE, "circle fill");
    CheckColour(1450, 103, MAGENTA, "circle outline");
    CheckColour(1450, 200, CYAN, "the non-modal screen over the modal one");
    CheckColour(200, 650, kQuad[0], "image, top left texel");
    CheckColour(400, 650, kQuad[1], "image, top right texel");
    CheckColour(200, 850, kQuad[2], "image, bottom left texel");
    CheckColour(400, 850, kQuad[3], "image, bottom right texel");
    CheckColour(750, 650, ORANGE, "inside a clip");
    CheckNot(550, 650, ORANGE, "left of the clip");
    CheckNot(750, 800, ORANGE, "below the clip");
    CheckColour(1300, 800, TEAL, "inside nested clips");
    CheckNot(1100, 800, TEAL, "inside the outer clip only");
    CheckNot(1500, 800, TEAL, "inside the inner clip only");
    /* text: bright pixels inside its box, none past its end */
    float white[3];
    Expected(TEXT, white);
    int lit = 0, past = 0;
    for (float y = 420; y < 480; y += 1.5f)
        for (float x = 100; x < 100 + g_demo.titleWidth + 60; x += 1.5f)
            if (Near(Pixel(x, y), white)) (x < 100 + g_demo.titleWidth + 4 ? lit : past)++;
    Check(g_demo.titleWidth > 250 && g_demo.titleWidth < 900, "title text width %.1f units", g_demo.titleWidth);
    Check(lit > 150 && past == 0, "title text drawn: %d lit samples in its box, %d past its end", lit, past);
    int right = 0, beyond = 0;
    for (float y = 435; y < 465; y += 1.5f)
        for (float x = 1820 - g_demo.bodyWidth; x < 1830; x += 1.5f)
            if (Near(Pixel(x, y), white)) (x < 1822 ? right : beyond)++;
    Check(right > 20 && beyond == 0, "right-aligned text ends at its x (%d lit samples, %d past it)", right, beyond);
}

int main(int argc, char** argv)
{
    if (argc < 2) { puts("usage: overlay_host_test.exe <hellforge_overlay.dll> [<screenshot.bmp>]"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0); /* every line out before a crash */
    SetProcessDPIAware();
    g_d3d11 = getenv("HELLFORGE_OVERLAY_TEST_D3D11") != NULL;
    const char* format = getenv("HELLFORGE_OVERLAY_TEST_FORMAT");
    g_format = format && !strcmp(format, "rgb10a2") ? RGB10A2 : format && !strcmp(format, "fp16") ? FP16 : RGBA8;
    if (const char* hdr = getenv("HELLFORGE_OVERLAY_HDR"))
    {
        g_hdr = !strncmp(hdr, "pq", 2) ? 1 : !strncmp(hdr, "scrgb", 5) ? 2 : 0;
        if (const char* colon = strchr(hdr, ':')) g_nits = (float)atof(colon + 1);
    }
    printf("  Direct3D %s, %s back buffer, %s\n", g_d3d11 ? "11" : "12", g_format == RGBA8 ? "8-bit" : g_format == RGB10A2 ? "10-bit" : "FP16",
           g_hdr == 1 ? "HDR10 forced" : g_hdr == 2 ? "scRGB forced" : "SDR");
    MH_Initialize();
    WNDCLASSEXW type = {sizeof type, CS_DBLCLKS, GameProc, 0, 0, GetModuleHandleW(NULL), NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"OverlayHostTest", NULL};
    RegisterClassExW(&type);
    HWND window = CreateWindowExW(WS_EX_TOPMOST, type.lpszClassName, L"HellForge overlay host test", WS_POPUP | WS_VISIBLE, 0, 0, (int)g_width, (int)g_height,
                                  NULL, NULL, type.hInstance, NULL);
    if (!MakeSwapChain(window)) { printf("FAIL no Direct3D %s swap chain here\n", g_d3d11 ? "11" : "12"); return 1; }
    g_presentCode = (*(void***)g_chain)[8];
    g_present1Code = (*(void***)g_chain)[22];

    HMODULE host = LoadLibraryA(argv[1]);
    HfOverlayStartFn start = host ? (HfOverlayStartFn)(void*)GetProcAddress(host, "hf_overlay_start") : NULL;
    HfOverlayAddScreenFn add = host ? (HfOverlayAddScreenFn)(void*)GetProcAddress(host, "hf_overlay_add_screen") : NULL;
    HfOverlayShowFn show = host ? (HfOverlayShowFn)(void*)GetProcAddress(host, "hf_overlay_show") : NULL;
    HfOverlayImGuiFn imgui = host ? (HfOverlayImGuiFn)(void*)GetProcAddress(host, "hf_overlay_imgui") : NULL;
    if (!start || !add || !show || !imgui) { puts("FAIL the host's exports are missing"); return 1; }

    HfScreen demo = {sizeof demo, "Demo map", 'M', HF_SCREEN_MODAL, NULL, DemoDraw, DemoOpened, DemoCanOpen};
    int demoId = add(&demo); /* before the start: the host keeps it */
    Check(demoId == 1, "the modal screen added before the start: id %d", demoId);
    HfScreen clash = {sizeof clash, "Clash", 'M', 0, NULL, HudDraw, NULL, NULL};
    Check(add(&clash) == 0, "a second screen on the same key is refused");
    const char* font = getenv("HELLFORGE_OVERLAY_TEST_FONT");
    uint8_t andMask[128], xorMask[128];
    memset(andMask, 0x0F, sizeof andMask);
    memset(xorMask, 0xF0, sizeof xorMask);
    g_pointer = CreateCursor(GetModuleHandleW(NULL), 0, 0, 32, 32, andMask, xorMask);
    ShowCursor(FALSE); /* hidden, as a game in play has it */
    int hidden = CursorCount();
    HfOverlayStart s = {sizeof s, NULL, Log, Hook, window, font, font, GamePointer};
    Check(start(&s) == 0, "hf_overlay_start");
    HfScreen hud = {sizeof hud, "HUD", 0, 0, NULL, HudDraw, NULL, NULL};
    int hudId = add(&hud);
    Check(hudId == 2, "a non-modal screen added after the start: id %d", hudId);

    const char* shot = argc > 2 ? argv[2] : NULL;
    std::string shot2 = shot ? std::string(shot, strlen(shot) - 4) + "-update.bmp" : "", shot3 = shot ? std::string(shot, strlen(shot) - 4) + "-resized.bmp" : "";
    int buttonsBefore = 0, keysBefore = 0;
    for (int frame = 0; frame < 110; frame++)
    {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (frame == 6) Check(Logged("drawing with Direct3D"), "the host draws on the window's swap chain");
        if (frame == 8) { Click(window, 100, 100); Check(g_gameButtons == 1, "with no screen open, a click reaches the game"); }
        if (frame == 10)
        {
            g_demo.allowOpen = false;
            Key(window, 'M');
            Check(g_gameKeys['M'] == 1 && g_demo.opened == 0, "can_open says no: the key goes to the game, the screen stays shut");
            g_demo.allowOpen = true;
        }
        if (frame == 12) { Key(window, 'M'); show(hudId, 1); }
        if (frame == 13) Check(g_gameKeys['M'] == 1, "the screen's key is not the game's");
        if (frame == 16)
        {
            Check(g_demo.opened == 1, "opened(1) before the first draw (%d frames drawn)", g_demo.frames);
            buttonsBefore = g_gameButtons;
            keysBefore = g_gameKeys['Q'];
            Click(window, 640, 360);
            Key(window, 'Q');
            SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, 240), MAKELPARAM(640, 360));
            RECT before, small = {10, 10, 20, 20}, after;
            GetClipCursor(&before);
            ClipCursor(&small);
            GetClipCursor(&after);
            Check(memcmp(&before, &after, sizeof before) == 0, "the game cannot clip the cursor while the screen is open");
        }
        if (frame == 18)
        {
            const HfInput& in = g_demo.lastInput;
            Check(g_gameButtons == buttonsBefore && g_gameKeys['Q'] == keysBefore && g_gameWheel == 0, "the game got none of the mouse and keys");
            Check(g_demo.pressedSeen & g_demo.releasedSeen & HF_MOUSE_LEFT, "the screen saw the left button pressed and released");
            Check(fabsf(in.x - 960) < 2 && fabsf(in.y - 540) < 2, "the mouse in units: %.1f, %.1f (960, 540)", in.x, in.y);
            Check(g_demo.sawQ && fabsf(g_demo.wheelSeen - 2) < 0.01f, "keys and wheel: Q %s, wheel %.1f notches", g_demo.sawQ ? "seen" : "missing", g_demo.wheelSeen);
            Check(fabsf(g_demo.width - 1920) < 0.5f && fabsf(g_demo.pixels - 720 / 1080.0f) < 0.001f, "units: %.1f wide, %.4f pixels a unit", g_demo.width, g_demo.pixels);
        }
        if (frame == 19)
        {
            int before = g_gameSetCursor;
            SetCursorMessage(window);
            Check(GetCursor() == g_pointer && g_pointer != NULL, "the screen shows the game's pointer");
            Check(g_gameSetCursor == before, "the game's window procedure does not get WM_SETCURSOR while the screen is open");
            Check(CursorCount() >= 0 && hidden < 0, "the hidden cursor is shown (count %d, hidden %d)", CursorCount(), hidden);
            Check(Logged("screens show the game's own mouse pointer"), "the log says the game's pointer is used");
        }
        if (frame == 20) g_wantShot = true;
        if (frame == 21)
        {
            SaveShot(shot);
            if (!g_shot.empty()) CheckDrawing(); else Check(false, "a screenshot was read");
            if (!g_shot.empty()) Check(ArrowSamples() == 0, "no ImGui arrow drawn at the mouse (%d bright samples)", ArrowSamples());
            g_demo.wantUpdate = true;
        }
        if (frame == 24) g_wantShot = true;
        if (frame == 25)
        {
            SaveShot(shot2.empty() ? NULL : shot2.c_str());
            CheckColour(200, 650, kQuad[3], "image_update: the new top left texel");
            CheckColour(400, 850, kQuad[0], "image_update: the new bottom right texel");
        }
        if (frame == 30)
        {
            HfOverlayImGui shared = {HF_OVERLAY_IMGUI_LAYOUT};
            Check(imgui(&shared) == 0 && shared.context != NULL && shared.alloc != NULL, "hf_overlay_imgui gives the context");
            HfOverlayImGui other = {HF_OVERLAY_IMGUI_LAYOUT};
            other.version = "1.90.0";
            Check(imgui(&other) == 1 && other.context == NULL, "hf_overlay_imgui refuses another ImGui version");
            HfOverlayImGui layout = {HF_OVERLAY_IMGUI_LAYOUT};
            layout.io += 8;
            Check(imgui(&layout) == 1, "hf_overlay_imgui refuses another ImGuiIO layout");
        }
        if (frame == 40) /* the game resizes its swap chain */
        {
            if (!g_d3d11) WaitGpu();
            g_width = 1600;
            g_height = 900;
            SetWindowPos(window, NULL, 0, 0, (int)g_width, (int)g_height, SWP_NOZORDER);
            Check(SUCCEEDED(g_chain->ResizeBuffers(0, g_width, g_height, DXGI_FORMAT_UNKNOWN, 0)), "ResizeBuffers while the screen is open");
        }
        if (frame == 46) g_wantShot = true;
        if (frame == 47)
        {
            SaveShot(shot3.empty() ? NULL : shot3.c_str());
            Check(g_shotWidth == 1600 && fabsf(g_demo.width - 1920) < 0.5f && fabsf(g_demo.pixels - 900 / 1080.0f) < 0.001f, "after the resize: %.1f units wide, %.4f pixels a unit",
                  g_demo.width, g_demo.pixels);
            CheckColour(250, 200, RED, "rect fill after the resize");
            CheckColour(1450, 120, WHITE, "circle fill after the resize");
            float game[3];
            ExpectedBackdrop(kGame, DIM, GREY, game);
            CheckAt(1700, 800, game, "backdrop after the resize");
        }
        if (frame == 50) { Key(window, 'M'); Click(window, 300, 300); }
        if (frame == 51)
        {
            Check(g_gameButtons == buttonsBefore + 1, "closing: the game has the mouse again at once");
            int before = g_gameSetCursor;
            SetCursorMessage(window);
            Check(g_gameSetCursor == before + 1 && CursorCount() == hidden, "closed: WM_SETCURSOR is the game's again, its cursor count back to %d (%d)", hidden,
                  CursorCount());
        }
        if (frame == 60)
            Check(g_demo.closingFrames == 5 && g_demo.closedCalls == 1, "the key closed it: %d closing frames drawn, opened(0) %d time(s)", g_demo.closingFrames, g_demo.closedCalls);
        if (frame == 61) g_offerPointer = false;
        if (frame == 62) { g_demo.closingFrames = 0; Key(window, 'M'); SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(640, 360)); }
        if (frame == 63)
        {
            Check(Logged("screens draw ImGui's arrow"), "no game pointer: the log says ImGui's arrow is drawn");
            g_wantShot = true;
        }
        if (frame == 64) { SaveShot(NULL); Check(!g_shot.empty() && ArrowSamples() > 5, "no game pointer: ImGui's arrow at the mouse (%d bright samples)", ArrowSamples()); }
        if (frame == 66) { int esc = g_gameKeys[VK_ESCAPE]; Key(window, VK_ESCAPE); Check(g_gameKeys[VK_ESCAPE] == esc, "Esc closes the modal screen, not the game's menu"); }
        if (frame == 76) Check(g_demo.opened == 2 && g_demo.closedCalls == 2, "Esc closed it (opened %d, closed %d)", g_demo.opened, g_demo.closedCalls);
        if (frame == 80) { g_demo.closingFrames = 0; show(demoId, 1); }
        if (frame == 84) show(demoId, 0);
        if (frame == 95) Check(g_demo.opened == 3 && g_demo.closedCalls == 3, "hf_overlay_show opened and closed it (opened %d, closed %d)", g_demo.opened, g_demo.closedCalls);
        if (frame == 96) show(hudId, 0);
        if (frame == 105) Check(g_hudFrames > 0 && Logged("HUD closed"), "the non-modal screen closed (%d frames)", g_hudFrames);
        if (g_d3d11) GameFrame11();
        else GameFrame12();
        HRESULT presented = g_chain->Present(0, 0);
        if (FAILED(presented)) { Check(false, "Present 0x%08lx at frame %d", (unsigned long)presented, frame); break; }
        if (!g_d3d11 && g_device->GetDeviceRemovedReason() != S_OK) { Check(false, "the device was removed at frame %d", frame); break; }
    }
    Check(Logged("backdrop: the game's frame is copied"), "the backdrop copied the frame (not the dimmed rectangle)");
    printf(g_failures ? "FAIL %d check(s)\n" : "PASS\n", g_failures);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), g_failures ? 1 : 0); /* the host's watchdog thread runs on */
    return 0;
}
