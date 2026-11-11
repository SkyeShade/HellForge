/* Frames: the screens recorded with ImGui into the back buffer about to be presented. D3D12 draws on the swap chain's
 * own queue (overlays make direct queues of their own on the game's device, and a frame drawn on one of those is not
 * ordered with the game's present), with a fence per back buffer. D3D11 draws on the game's immediate context, which
 * Present is called with on this thread. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "host_internal.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"

/* ImGui's DX11 and DX12 renderers take this pixel shader where it is set (Build-Overlay patches their copies): HDR. */
const char* g_hellforgePixelShader = nullptr;

namespace overlay
{
int g_hdrMode = -1;
ID3D11Device* g_device11;
ID3D11DeviceContext* g_context11;
ID3D12Device* g_device12;
ID3D12CommandQueue* g_queue;
ID3D12GraphicsCommandList* g_frameList;
UINT64 g_fenceValue = 1;
UINT g_frameWidth, g_frameHeight;
size_t g_queueOffset;
void* g_queueTable;

namespace
{
const UINT MAX_BUFFERS = 8;
ID3D12DescriptorHeap* g_rtvHeap;
UINT g_rtvStep, g_bufferCount;
DXGI_FORMAT g_format;
ID3D12CommandAllocator* g_allocators[MAX_BUFFERS];
ID3D12GraphicsCommandList* g_commands;
ID3D12Fence* g_fence;
HANDLE g_fenceEvent;
UINT64 g_bufferFence[MAX_BUFFERS];
bool g_ready, g_failed, g_queueOwn;
float g_hdrNits;
std::string g_hdrShader;

/* ---- the queue ---- */

struct SeenQueue { ID3D12CommandQueue* queue; LONG lists; bool direct; };
SeenQueue g_seen[16];
int g_seenCount;
SRWLOCK g_seenLock = SRWLOCK_INIT;

/* The queue a D3D12 swap chain presents on, where the probe saw its own kept; NULL unknown. Only compared until checked. */
ID3D12CommandQueue* QueueOf(IDXGISwapChain3* chain)
{
    char* at = (char*)chain + g_queueOffset;
    MEMORY_BASIC_INFORMATION info;
    if (g_queueOffset == 0 || !VirtualQuery(at, &info, sizeof info) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return NULL;
    return *(ID3D12CommandQueue**)at;
}

/* The swap chain's own queue when lists ran on it as a direct queue, or it has every queue's method table; g_seenLock
 * held. */
ID3D12CommandQueue* OwnQueue(ID3D12CommandQueue* own, const char** how)
{
    if (own == NULL) return NULL;
    for (int i = 0; i < g_seenCount; i++)
        if (g_seen[i].queue == own && g_seen[i].direct) return own;
    if (g_queueTable == NULL || *(void**)own != g_queueTable) return NULL;
    *how = "the swap chain's own (no lists seen on it yet)";
    return own;
}

/* The direct queue that ran the most lists; g_seenLock held. */
ID3D12CommandQueue* BusiestQueue()
{
    ID3D12CommandQueue* pick = NULL;
    LONG most = 0;
    for (int i = 0; i < g_seenCount; i++)
        if (g_seen[i].direct && g_seen[i].lists > most) { most = g_seen[i].lists; pick = g_seen[i].queue; }
    return pick;
}

/* The swap chain's own queue once it shows it is one, else after enough frames the busiest direct queue: the game
 * submits many lists a frame, an overlay one or two. */
bool PickQueue(IDXGISwapChain3* chain)
{
    static int frames;
    ID3D12CommandQueue* own = QueueOf(chain);
    const char* how = "the swap chain's own";
    AcquireSRWLockExclusive(&g_seenLock);
    ID3D12CommandQueue* pick = OwnQueue(own, &how);
    if (pick == NULL && ++frames > (g_queueOffset == 0 ? 30 : 120))
    {
        pick = BusiestQueue();
        how = "a guess: the busiest direct queue";
    }
    ReleaseSRWLockExclusive(&g_seenLock);
    if (pick == NULL) return false;
    pick->AddRef();
    g_queueOwn = pick == own;
    g_queue = pick;
    Log("overlay: drawing on queue %p, %s", (void*)pick, how);
    return true;
}

/* Drawing on the swap chain's own queue: only a swap chain presenting on it is drawn into. */
bool OnOurQueue(IDXGISwapChain3* chain)
{
    static IDXGISwapChain3 *ours, *told;
    if (!g_queueOwn || chain == ours) return true;
    if (QueueOf(chain) == g_queue) { ours = chain; return true; }
    if (told != chain) Log("overlay: swap chain %p presents on another queue; screens are not drawn into it", (void*)chain);
    told = chain;
    return false;
}

/* ---- HDR ---- */

/* The SDR white level of the window's display (Windows' "SDR content brightness"), 0 unknown. */
float SdrWhiteNits(HWND window)
{
    MONITORINFOEXW monitor = {};
    monitor.cbSize = sizeof monitor;
    UINT32 paths = 0, modes = 0;
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) ||
        GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path.data(), &modes, mode.data(), NULL) != ERROR_SUCCESS) return 0;
    for (const DISPLAYCONFIG_PATH_INFO& p : path)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name = {};
        name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof name, p.sourceInfo.adapterId, p.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS || wcscmp(name.viewGdiDeviceName, monitor.szDevice) != 0) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL level = {};
        level.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof level, p.targetInfo.adapterId, p.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&level.header) == ERROR_SUCCESS && level.SDRWhiteLevel > 0) return level.SDRWhiteLevel * 0.08f;
    }
    return 0;
}

bool HdrDisplay(IDXGISwapChain3* chain)
{
    IDXGIOutput* output = NULL;
    IDXGIOutput6* output6 = NULL;
    DXGI_OUTPUT_DESC1 desc = {};
    bool hdr = SUCCEEDED(chain->GetContainingOutput(&output)) && output != NULL && SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) &&
        SUCCEEDED(output6->GetDesc1(&desc)) && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    if (output6 != NULL) output6->Release();
    if (output != NULL) output->Release();
    return hdr;
}

/* ImGui's colour times texture, its sRGB made linear at the display's SDR white: Rec.2020 and PQ for HDR10, a scale
 * where 1 is 80 nits for scRGB. */
std::string HdrPixelShader(int mode, float nits)
{
    char line[192];
    std::string text =
        "struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
        "SamplerState sampler0 : register(s0);\nTexture2D texture0 : register(t0);\n"
        "float4 main(PS_INPUT input) : SV_Target\n{\n"
        "  float4 c = input.col * texture0.Sample(sampler0, input.uv);\n"
        "  float3 s = saturate(c.rgb);\n"
        "  float3 l = s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);\n";
    if (mode == HDR_PQ)
    {
        snprintf(line, sizeof line, "  l = mul(float3x3(0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956), l) * %.8f;\n", nits / 10000.0f);
        text += line;
        text += "  float3 m = pow(max(l, 0.0), 0.1593017578125);\n"
                "  c.rgb = pow((0.8359375 + 18.8515625 * m) / (1.0 + 18.6875 * m), 78.84375);\n";
    }
    else
    {
        snprintf(line, sizeof line, "  c.rgb = l * %.8f;\n", nits / 80.0f);
        text += line;
    }
    return text + "  return c;\n}\n";
}

/* HELLFORGE_OVERLAY_HDR=off|pq|scrgb[:nits] in place of the detected mode (the tests, a display detected wrongly). */
void ForcedHdr(int* mode, float* nits)
{
    const char* forced = getenv("HELLFORGE_OVERLAY_HDR");
    if (forced == NULL) return;
    *mode = !strncmp(forced, "pq", 2) ? HDR_PQ : !strncmp(forced, "scrgb", 5) ? HDR_SCRGB : HDR_OFF;
    if (const char* colon = strchr(forced, ':')) *nits = (float)atof(colon + 1);
}

/* The HDR mode of a back-buffer format and display. True when it changed: ImGui's device objects are then made
 * again. */
bool FollowHdr(IDXGISwapChain3* chain, DXGI_FORMAT format)
{
    int mode = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? HDR_SCRGB : format == DXGI_FORMAT_R10G10B10A2_UNORM && HdrDisplay(chain) ? HDR_PQ : HDR_OFF;
    float nits = SdrWhiteNits(g_window);
    ForcedHdr(&mode, &nits);
    nits = nits <= 0 ? 200 : fminf(fmaxf(nits, 80), 1000);
    if (mode == g_hdrMode && (mode == HDR_OFF || nits == g_hdrNits)) return false;
    g_hdrMode = mode;
    g_hdrNits = nits;
    g_hdrShader = mode == HDR_OFF ? std::string() : HdrPixelShader(mode, nits);
    g_hellforgePixelShader = mode == HDR_OFF ? nullptr : g_hdrShader.c_str();
    static const char* const names[] = {"SDR", "HDR10 (PQ)", "scRGB"};
    Log("overlay: back buffer format %d, %s at %.0f nits SDR white", (int)format, names[mode], nits);
    BackdropFormatChanged();
    return true;
}

/* ---- set-up ---- */

void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    int slot = AllocSlot();
    *cpu = SlotCpu(slot < 0 ? 0 : slot);
    *gpu = SlotGpu(slot < 0 ? 0 : slot);
}
/* ImGui 1.91.9 frees the font's view in InvalidateDeviceObjects too, yet takes one only in Init and keeps writing the
 * font into it: the slot is given back only when the renderer shuts down, or a picture made later would overwrite it. */
bool g_rendererClosing;
void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    if (g_rendererClosing) FreeSlotLater(SlotOf(cpu));
}

bool InitDx12Renderer()
{
    ImGui_ImplDX12_InitInfo info;
    info.Device = g_device12;
    info.CommandQueue = g_queue;
    info.NumFramesInFlight = (int)g_bufferCount;
    info.RTVFormat = g_format;
    info.SrvDescriptorHeap = g_srvHeap;
    info.SrvDescriptorAllocFn = SrvAlloc;
    info.SrvDescriptorFreeFn = SrvFree;
    return ImGui_ImplDX12_Init(&info);
}

void MakeContext()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().AntiAliasedFill = ImGui::GetStyle().AntiAliasedLines = true;
}

bool SetUp12(IDXGISwapChain3* chain, const DXGI_SWAP_CHAIN_DESC& desc)
{
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&g_device12)))) return false;
    g_bufferCount = desc.BufferCount < MAX_BUFFERS ? desc.BufferCount : MAX_BUFFERS;
    D3D12_DESCRIPTOR_HEAP_DESC rtv = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, MAX_BUFFERS, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(g_device12->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_rtvHeap))) || !CreateSlots(g_device12)) return false;
    g_rtvStep = g_device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < MAX_BUFFERS; i++)
        if (FAILED(g_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocators[i])))) return false;
    if (FAILED(g_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0], NULL, IID_PPV_ARGS(&g_commands)))) return false;
    g_commands->Close();
    if (FAILED(g_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_fenceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    return ImGui_ImplWin32_Init(g_window) && InitDx12Renderer();
}

bool SetUp(IDXGISwapChain3* chain)
{
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(chain->GetDesc(&desc))) return false;
    if (g_window == NULL) g_window = desc.OutputWindow;
    g_format = desc.BufferDesc.Format;
    MakeContext();
    FollowHdr(chain, g_format);
    bool made;
    if (g_device11 != NULL)
    {
        g_device11->GetImmediateContext(&g_context11);
        made = ImGui_ImplWin32_Init(g_window) && ImGui_ImplDX11_Init(g_device11, g_context11);
    }
    else made = SetUp12(chain, desc);
    if (!made || !SubclassWindow(g_window)) return false;
    Log("overlay: drawing with Direct3D %s, %ux%u, format %d (frames on thread %lu, the window's thread %lu)", g_device11 != NULL ? "11" : "12",
        desc.BufferDesc.Width, desc.BufferDesc.Height, (int)g_format, GetCurrentThreadId(), GetWindowThreadProcessId(g_window, NULL));
    return true;
}

/* D3D11 when the swap chain's device is one; D3D12 once its queue is picked, and then only for a swap chain on it. */
bool PickDevice(IDXGISwapChain3* chain)
{
    if (g_device11 != NULL) return true;
    if (g_queue != NULL) return OnOurQueue(chain);
    ID3D11Device* device = NULL;
    if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device))) && device != NULL) { g_device11 = device; return true; }
    return PickQueue(chain);
}

/* ---- frames ---- */

/* The game's render targets, put back once the host drew. */
struct GameTargets
{
    ID3D11RenderTargetView* views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView* depth;
};

void PutBack(GameTargets& saved)
{
    g_context11->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved.views, saved.depth);
    for (ID3D11RenderTargetView* target : saved.views)
        if (target != NULL) target->Release();
    if (saved.depth != NULL) saved.depth->Release();
}

void Draw11(IDXGISwapChain3* chain)
{
    ID3D11Texture2D* buffer = NULL;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return;
    CaptureBackdrop11(buffer);
    ID3D11RenderTargetView* view = NULL;
    HRESULT made = g_device11->CreateRenderTargetView(buffer, NULL, &view);
    buffer->Release();
    if (FAILED(made)) return;
    GameTargets saved = {};
    g_context11->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved.views, &saved.depth);
    g_context11->OMSetRenderTargets(1, &view, NULL);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    view->Release();
    PutBack(saved);
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier);
}

/* Uploads and the frame's copy first, then ImGui into the back buffer, on the swap chain's queue. */
void Draw12(IDXGISwapChain3* chain)
{
    UINT index = chain->GetCurrentBackBufferIndex() % MAX_BUFFERS;
    ID3D12Resource* buffer = NULL;
    if (FAILED(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)))) return;
    WaitFence(g_bufferFence[index]);
    g_allocators[index]->Reset();
    g_commands->Reset(g_allocators[index], NULL);
    g_frameList = g_commands;
    FlushUploads12(g_commands);
    CaptureBackdrop12(g_commands, buffer);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)index * g_rtvStep;
    g_device12->CreateRenderTargetView(buffer, NULL, rtv);
    Transition(g_commands, buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_commands->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    g_commands->SetDescriptorHeaps(1, &g_srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commands);
    Transition(g_commands, buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g_commands->Close();
    g_frameList = NULL;
    ID3D12CommandList* lists[] = {g_commands};
    g_execute(g_queue, 1, lists);
    g_queue->Signal(g_fence, g_fenceValue);
    g_bufferFence[index] = g_fenceValue++;
    SweepLater();
    buffer->Release();
}

/* A new back-buffer format: ImGui's renderer made again (after the host's last frame) with the shader for its HDR mode. */
bool FollowFormat(IDXGISwapChain3* chain, DXGI_FORMAT format)
{
    if (format == g_format) return true;
    g_format = format;
    if (g_device11 != NULL)
    {
        if (FollowHdr(chain, format)) ImGui_ImplDX11_InvalidateDeviceObjects();
        return true;
    }
    WaitFence(g_fenceValue - 1);
    g_rendererClosing = true;
    ImGui_ImplDX12_Shutdown();
    g_rendererClosing = false;
    FollowHdr(chain, format);
    return InitDx12Renderer();
}

/* The fonts baked for the window's height; ImGui's font texture made again after the host's last frame. */
void FollowFonts(float height)
{
    if (!BakeFonts(height / 1080.0f)) return;
    if (g_device11 != NULL) ImGui_ImplDX11_InvalidateDeviceObjects();
    else
    {
        WaitFence(g_fenceValue - 1);
        ImGui_ImplDX12_InvalidateDeviceObjects();
    }
}

void Frame(IDXGISwapChain3* chain, bool captured)
{
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(chain->GetDesc(&desc))) return;
    if (!FollowFormat(chain, desc.BufferDesc.Format)) { g_failed = true; Log("overlay: ImGui's renderer could not be made for the new format; screens stop"); return; }
    g_frameWidth = desc.BufferDesc.Width;
    g_frameHeight = desc.BufferDesc.Height;
    float width = (float)g_frameWidth, height = (float)g_frameHeight;
    FollowFonts(height);
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = captured; /* the game hides its own cursor */
    if (captured) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    if (g_device11 != NULL) ImGui_ImplDX11_NewFrame();
    else ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    io.DisplaySize = ImVec2(width, height); /* the swap chain's size, which the client area need not be */
    ImGui::NewFrame();
    DrawScreens(width, height, io.DeltaTime);
    ImGui::Render();
    if (g_device11 != NULL) Draw11(chain);
    else Draw12(chain);
}
} // namespace

UINT64 CompletedFence() { return g_fence != NULL ? g_fence->GetCompletedValue() : 0; }

void WaitFence(UINT64 value)
{
    if (value == 0 || g_fence == NULL || g_fence->GetCompletedValue() >= value) return;
    g_fence->SetEventOnCompletion(value, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, 2000);
}

void WaitLastFrame()
{
    EnterCriticalSection(&g_lock);
    if (g_ready && g_fenceValue > 1) WaitFence(g_fenceValue - 1);
    LeaveCriticalSection(&g_lock);
}

void NoteQueue(ID3D12CommandQueue* queue, void* caller)
{
    if (g_queue != NULL || g_device11 != NULL) return;
    AcquireSRWLockExclusive(&g_seenLock);
    int i = 0;
    while (i < g_seenCount && g_seen[i].queue != queue) i++;
    bool added = i == g_seenCount && i < 16;
    if (added) g_seen[g_seenCount++] = {queue, 0, queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (i < g_seenCount) g_seen[i].lists++;
    ReleaseSRWLockExclusive(&g_seenLock);
    if (added && g_seen[i].direct) Log("overlay: a direct queue %p runs lists (thread %lu, code at %p)", (void*)queue, GetCurrentThreadId(), caller);
}

bool HaveDevice() { return g_ready; }
DXGI_FORMAT BackBufferFormat() { return g_format; }

void Render(IDXGISwapChain3* chain)
{
    if (g_failed || !PickDevice(chain)) return;
    EnterCriticalSection(&g_lock);
    if (!g_ready && !(g_ready = SetUp(chain)))
    {
        g_failed = true;
        Log("overlay: drawing could not be set up; no screens are drawn");
    }
    if (g_ready && AnyScreenWanted()) Frame(chain, g_captured != 0);
    else if (g_ready) SweepLater();
    LeaveCriticalSection(&g_lock);
}
} // namespace overlay
