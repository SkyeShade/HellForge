/* HellForge's in-game editor (docs/sdk-roadmap.md phase 5a): a code mod on the public SDK that draws the editor's window
 * over the game (F8; app.cpp, the HD2R Editor's layout and look) with Dear ImGui's renderer, and changes fields live
 * through set_live.
 *
 * Drawing: the game renders with D3D12 (borderless). The editor hooks IDXGISwapChain::Present / Present1 and
 * ResizeBuffers where their code is (their addresses come from a throwaway device and swap chain), and hooks them again
 * in front of any overlay that later writes its own jump over the editor's (Watchdog: RivaTuner does, issue #5, F8 took
 * the input and nothing was drawn), and ID3D12CommandQueue::ExecuteCommandLists, which shows the direct queues in use.
 * The swap chain's own queue (the one it keeps, at the offset a throwaway swap chain shows) is the one drawn on: overlays can make direct queues of their own on the game's device,
 * and a frame drawn on one of those is not ordered with the game's present. Just before the game presents, ImGui is
 * recorded into the back buffer on that queue, with a fence per back buffer, so nothing depends on the game's own GUI,
 * materials or fonts, and the window draws over the game's menus. Started with --use-d3d11 (launch options), the game
 * renders with D3D11 and no queue is seen: the swap chain's device is a D3D11 one, and ImGui's DX11 backend draws on it,
 * into the back buffer about to be presented, the game's own render targets put back after (the D3D11 swap chain's
 * methods are hooked too where they are other code than the D3D12 one's).
 * Input: the game window's procedure is replaced by one that toggles the window (F8) and, while it is open, gives
 * keyboard, mouse and raw input to ImGui instead of the game; the game's cursor clipping and recentring are ignored
 * meanwhile, and ImGui draws its own cursor. ImGui runs under one lock, taken by the window thread (messages) and the
 * render thread (frames), so it is never used from two threads at once.
 * Values: changes go through set_live, the same path as the launcher's live editor (applied at once by identity, with
 * the loader's checks), so nothing here writes game memory itself; get_live reads them. Applied changes are also saved
 * into the Local Stats mod (local_stats.cpp; the launcher's STATS page saves there too), on a thread of their own: the
 * stats pass applies them whenever the game loads, so they come back every boot. A reset takes a field out of that file
 * and puts its base back (set_live with HF_LIVE_BASE).
 * Images: the game's weapon and throwable silhouettes, extracted from the player's own game into the editor's folder
 * (game-icons.bin), and the HD2R Editor's marker icons, in one texture (icons.cpp). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "app.h"
#include "canvas.h"
#include "hellforge_mod.h"
#include "icons.h"
#include "local_stats.h"
#include "sounds.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM w, LPARAM l);

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain3*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain3*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffersFn)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void(STDMETHODCALLTYPE* ExecuteFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef BOOL(WINAPI* ClipCursorFn)(const RECT*);
typedef BOOL(WINAPI* SetCursorPosFn)(int, int);
typedef UINT(WINAPI* RawInputBufferFn)(PRAWINPUT, PUINT, UINT);

static const HfApi* g_api;
static HfMod* g_mod;
static PresentFn g_present, g_present11;
static Present1Fn g_present1, g_present1_11;
static ResizeBuffersFn g_resize, g_resize11;
static ExecuteFn g_execute;
static ClipCursorFn g_clipCursor;
static SetCursorPosFn g_setCursorPos;
static RawInputBufferFn g_rawInputBuffer;

static CRITICAL_SECTION g_lock;   /* ImGui: the window thread's messages and the render thread's frames */
static volatile LONG g_open;      /* the window is shown */
static volatile LONG g_modal;     /* the MOD SYNC prompt is shown over the game, the window closed */
static ID3D12CommandQueue* g_queue;
static int g_ready, g_failed;
/* The direct queues seen (ExecuteDetour) until one is drawn on, with how often each ran lists; where a D3D12 swap chain
 * keeps its queue (FindMethods; 0 unknown) and the method table every queue shares; whether g_queue is the swap
 * chain's own (then only a swap chain presenting on it is drawn into). */
#define MAX_QUEUES 16
static struct { ID3D12CommandQueue* queue; LONG calls; bool direct; } g_queues[MAX_QUEUES];
static int g_queueCount;
static SRWLOCK g_queuesLock = SRWLOCK_INIT;
static size_t g_queueOffset;
static void* g_queueTable;
static bool g_queueOwn;
static HWND g_window;
static WNDPROC g_gameProc;

#define MAX_BUFFERS 8
static ID3D12Device* g_device;
static ID3D12DescriptorHeap *g_rtvHeap, *g_srvHeap;
static UINT g_rtvSize, g_buffers;
static DXGI_FORMAT g_format;
static ID3D12CommandAllocator* g_allocators[MAX_BUFFERS];
static ID3D12GraphicsCommandList* g_list;
static ID3D12Fence* g_fence;
static HANDLE g_fenceEvent;
static UINT64 g_fenceValue, g_frameFence[MAX_BUFFERS];
/* Direct3D 11 (the game started with --use-d3d11): its device, found through the swap chain; no queue of its own */
static ID3D11Device* g_device11;
static ID3D11DeviceContext* g_context11;

/* ---- the editor's window ---------------------------------------------------------------------------------------- */

/* Saving into Local Stats: when the queued changes are written (0: now), and how the last save went. */
static std::string g_modsFolder;
static volatile LONG g_saving, g_saveFailed;
static ULONGLONG g_saveAt;
static char g_saveStatus[256];

static DWORD WINAPI SaveThread(void*)
{
    std::string error;
    bool saved = localstats::Save(g_modsFolder, &error);
    snprintf(g_saveStatus, sizeof g_saveStatus, saved ? "saved to Local Stats" : "Local Stats not saved: %s", error.c_str());
    g_api->log(g_mod, "editor: %s", g_saveStatus);
    if (!saved) { InterlockedExchange(&g_saveFailed, 1); g_saveAt = GetTickCount64() + 1000; } /* tried again in a second */
    InterlockedExchange(&g_saving, 0);
    return 0;
}

static void SaveSoon(int ms) { g_saveAt = GetTickCount64() + (ULONGLONG)ms; }
static void CloseEditor() { InterlockedExchange(&g_open, 0); g_saveAt = 0; }
/* A mod sync offer shows its prompt over the game by itself, the window closed: the input is the prompt's, as it is
 * the window's. */
static void ShowModal(bool on)
{
    InterlockedExchange(&g_modal, on ? 1 : 0);
    if (on && g_clipCursor) g_clipCursor(NULL);
}

/* Called every frame, open or not: starts a save once the queued changes have settled. */
static void SaveTick()
{
    if (g_modsFolder.empty() || g_saving || !localstats::Pending() || GetTickCount64() < g_saveAt) return;
    InterlockedExchange(&g_saving, 1);
    HANDLE thread = CreateThread(NULL, 0, SaveThread, NULL, 0, NULL);
    if (thread != NULL) CloseHandle(thread); else InterlockedExchange(&g_saving, 0);
}

/* ---- input ------------------------------------------------------------------------------------------------------ */

static bool InputMessage(UINT message)
{
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST) || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        message == WM_INPUT || message == WM_MOUSEHOVER || message == WM_MOUSELEAVE || message == WM_NCMOUSEMOVE;
}

/* The editor's sounds are posted on the window's thread, the game's main thread, as the game's own menus post theirs. */
#define WM_EDITOR_SOUND (WM_APP + 0x46)

static LRESULT CALLBACK EditorProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_EDITOR_SOUND)
    {
        static int seen;
        if (seen++ == 0) g_api->log(g_mod, "editor: sound requests reach the window's thread (%lu)", GetCurrentThreadId());
        sounds::Flush();
        return 0;
    }
    if (message == WM_KEYDOWN && w == VK_F8 && !(l & 0x40000000))
    {
        LONG open = !g_open;
        if (!open) g_saveAt = 0; /* closing saves at once */
        InterlockedExchange(&g_open, open);
        if (open && g_clipCursor) g_clipCursor(NULL);
        return 0;
    }
    if ((g_open || g_modal) && g_ready)
    {
        EnterCriticalSection(&g_lock);
        /* a button or the wheel where the mouse is (ImGui learns the position only from moves: the first click after
         * opening, before any move, landed nowhere) */
        if ((message >= WM_LBUTTONDOWN && message <= WM_MBUTTONDBLCLK) || (message >= WM_XBUTTONDOWN && message <= WM_XBUTTONDBLCLK))
        {
            ImGui::GetIO().AddMousePosEvent((float)(short)LOWORD(l), (float)(short)HIWORD(l));
        }
        else if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
        {
            POINT at = {(short)LOWORD(l), (short)HIWORD(l)};
            ScreenToClient(window, &at);
            ImGui::GetIO().AddMousePosEvent((float)at.x, (float)at.y);
        }
        ImGui_ImplWin32_WndProcHandler(window, message, w, l);
        LeaveCriticalSection(&g_lock);
        if (InputMessage(message)) return message == WM_INPUT ? DefWindowProcW(window, message, w, l) : 0;
    }
    return CallWindowProcW(g_gameProc, window, message, w, l);
}

static BOOL WINAPI ClipCursorDetour(const RECT* rect) { return g_open || g_modal ? TRUE : g_clipCursor(rect); }
static BOOL WINAPI SetCursorPosDetour(int x, int y) { return g_open || g_modal ? TRUE : g_setCursorPos(x, y); }
static UINT WINAPI RawInputBufferDetour(PRAWINPUT data, PUINT size, UINT header)
{
    if ((g_open || g_modal) && data != NULL) { UINT dropped = g_rawInputBuffer(data, size, header); (void)dropped; return 0; }
    return g_rawInputBuffer(data, size, header);
}

/* ---- the game's queue ------------------------------------------------------------------------------------------- */

/* The module an address lies in ("" none: allocated memory, say an overlay's own code), for the log. */
static std::string ModuleOf(const void* address)
{
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module) ||
        !GetModuleFileNameW(module, path, MAX_PATH)) return "";
    const wchar_t* name = wcsrchr(path, L'\\');
    char out[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, name ? name + 1 : path, -1, out, sizeof out, NULL, NULL);
    return out;
}

/* The overlays known to draw into games that are loaded (and a dxgi.dll or d3d12.dll from outside System32: a wrapper,
 * ReShade say), for the log; said again only when the list changes (an overlay can come in late: RivaTuner waits 15 s
 * where Steam's is loaded). */
static void LogOverlays()
{
    static const wchar_t* const known[] = {L"RTSSHooks64.dll", L"GameOverlayRenderer64.dll", L"DiscordHook64.dll", L"DiscordOverlay64.dll",
        L"graphics-hook64.dll", L"ow-graphics-hook64.dll", L"OWExplorer.dll", L"IGO64.dll", L"overlay64.dll", L"EOSOVH-Win64-Shipping.dll",
        L"GCLay64.dll", L"nvspcap64.dll", L"ReShade64.dll", L"SpecialK64.dll", L"fraps64.dll", L"action_x64.dll", L"PresentMon64.dll"};
    std::string found;
    for (const wchar_t* name : known)
        if (HMODULE module = GetModuleHandleW(name)) found += (found.empty() ? "" : ", ") + ModuleOf((const void*)module);
    wchar_t system[MAX_PATH];
    UINT length = GetSystemDirectoryW(system, MAX_PATH);
    for (const wchar_t* name : {L"dxgi.dll", L"d3d12.dll"})
    {
        HMODULE module = GetModuleHandleW(name);
        wchar_t path[MAX_PATH];
        if (module != NULL && length > 0 && GetModuleFileNameW(module, path, MAX_PATH) && (_wcsnicmp(path, system, length) != 0 || path[length] != L'\\'))
            found += (found.empty() ? "" : ", ") + ModuleOf((const void*)module) + " (not System32's: a wrapper?)";
    }
    static std::string said = "?";
    if (found == said) return;
    said = found;
    g_api->log(g_mod, "editor: overlays loaded: %s", found.empty() ? "none known" : found.c_str());
}

/* `size` bytes at `at` can be read (committed, readable, not a guard page) */
static bool Readable(const void* at, size_t size)
{
    MEMORY_BASIC_INFORMATION memory;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return VirtualQuery(at, &memory, sizeof memory) && memory.State == MEM_COMMIT && (memory.Protect & readable) && !(memory.Protect & PAGE_GUARD) &&
        (const char*)at + size <= (const char*)memory.BaseAddress + memory.RegionSize;
}

/* The queue a D3D12 swap chain presents on, read where FindMethods saw the throwaway swap chain keep its own (DXGI has
 * no call for it; RivaTuner reads it the same way); NULL unknown. Only compared, never used, until PickQueue checks it. */
static ID3D12CommandQueue* QueueOf(IDXGISwapChain3* swapChain)
{
    const char* at = (const char*)swapChain + g_queueOffset;
    return g_queueOffset != 0 && Readable(at, sizeof(void*)) ? *(ID3D12CommandQueue* const*)at : NULL;
}

/* ExecuteCommandLists, until the queue is picked: each queue noted, how often it runs lists counted, each new direct
 * one logged with the module that called (the game, or an overlay with a queue of its own). */
static void SeeQueue(ID3D12CommandQueue* queue, void* caller)
{
    AcquireSRWLockExclusive(&g_queuesLock);
    int i = 0;
    while (i < g_queueCount && g_queues[i].queue != queue) i++;
    bool added = false;
    if (i < g_queueCount) g_queues[i].calls++;
    else if (i < MAX_QUEUES)
    {
        g_queues[i].queue = queue;
        g_queues[i].calls = 1;
        g_queues[i].direct = added = queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
        g_queueCount++;
    }
    ReleaseSRWLockExclusive(&g_queuesLock);
    if (added)
    {
        std::string from = ModuleOf(caller);
        g_api->log(g_mod, "editor: a direct queue %p runs lists (called from %s, thread %lu)", (void*)queue,
            from.empty() ? "allocated memory: an overlay's code?" : from.c_str(), GetCurrentThreadId());
    }
}

/* The queue to draw on, from the first frames of a D3D12 swap chain: the swap chain's own, once it is a queue (it ran
 * lists here, or it has every queue's method table). Where that cannot be read (FindMethods found no offset: after 30
 * frames; what is there is no queue: after 120), the direct queue that ran the most lists (the game submits many a
 * frame, an overlay one or two), logged as a guess. false: none yet. */
static bool PickQueue(IDXGISwapChain3* swapChain)
{
    static int waited;
    ID3D12CommandQueue* own = QueueOf(swapChain);
    ID3D12CommandQueue* queue = NULL;
    const char* how = NULL;
    AcquireSRWLockExclusive(&g_queuesLock);
    for (int i = 0; own != NULL && queue == NULL && i < g_queueCount; i++)
        if (g_queues[i].queue == own && g_queues[i].direct) { queue = own; how = "the swap chain's own"; }
    if (queue == NULL && own != NULL && g_queueTable != NULL && Readable(own, sizeof(void*)) && *(void**)own == g_queueTable)
    {
        queue = own;
        how = "the swap chain's own (no lists seen on it yet)";
    }
    if (queue == NULL && ++waited > (g_queueOffset == 0 ? 30 : 120))
    {
        LONG most = 0;
        for (int i = 0; i < g_queueCount; i++)
            if (g_queues[i].direct && g_queues[i].calls > most) { most = g_queues[i].calls; queue = g_queues[i].queue; }
        how = g_queueOffset == 0 ? "a guess: the busiest direct queue (where a swap chain keeps its queue is unknown)" :
            "a guess: the busiest direct queue (the swap chain's own was not found)";
    }
    if (queue != NULL)
    {
        queue->AddRef();
        g_queueOwn = own == queue;
        g_queue = queue;
    }
    int seen = 0;
    for (int i = 0; i < g_queueCount; i++) seen += g_queues[i].direct;
    ReleaseSRWLockExclusive(&g_queuesLock);
    if (queue != NULL) g_api->log(g_mod, "editor: drawing on queue %p, %s; %d direct queue(s) seen", (void*)queue, how, seen);
    return queue != NULL;
}

/* Once drawing on the swap chain's own queue: the swap chain presenting is drawn into only when it presents on that
 * queue (another one, an overlay's own window say, would not be in order with it). */
static bool OnOurQueue(IDXGISwapChain3* swapChain)
{
    static IDXGISwapChain3 *ours, *said;
    if (!g_queueOwn || swapChain == ours) return true;
    if (QueueOf(swapChain) == g_queue) { ours = swapChain; return true; }
    if (said != swapChain)
    {
        said = swapChain;
        g_api->log(g_mod, "editor: swap chain %p presents on another queue (%p); the editor is not drawn into it", (void*)swapChain, (void*)QueueOf(swapChain));
    }
    return false;
}

/* ---- rendering -------------------------------------------------------------------------------------------------- */

/* The view heap: ImGui's font atlas in the first slot, the editor's images (icons.cpp) in the second. */
static void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    *cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    *gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
}

static void ImagesView(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    UINT step = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    *cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    *gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu->ptr += step;
    gpu->ptr += step;
}
static void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {}

static void WaitFence(UINT64 value)
{
    if (value == 0 || g_fence == NULL || g_fence->GetCompletedValue() >= value) return;
    g_fence->SetEventOnCompletion(value, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, 2000);
}

static bool InitDx12Backend()
{
    ImGui_ImplDX12_InitInfo info;
    info.Device = g_device;
    info.CommandQueue = g_queue;
    info.NumFramesInFlight = (int)g_buffers;
    info.RTVFormat = g_format;
    info.SrvDescriptorHeap = g_srvHeap;
    info.SrvDescriptorAllocFn = SrvAlloc;
    info.SrvDescriptorFreeFn = SrvFree;
    return ImGui_ImplDX12_Init(&info);
}

static void CreateImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.MouseDrawCursor = true;
    ImGui::StyleColorsDark();
}

/* Direct3D 11: ImGui's DX11 backend on the game's device and immediate context, which the game also draws with on this
 * thread (Present is called on it). */
static bool Initialize11(IDXGISwapChain3* swapChain)
{
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(swapChain->GetDesc(&desc))) return false;
    g_window = desc.OutputWindow;
    g_device11->GetImmediateContext(&g_context11);
    CreateImGui();
    if (!ImGui_ImplWin32_Init(g_window) || !ImGui_ImplDX11_Init(g_device11, g_context11)) return false;
    if (!icons::Upload11(g_device11)) g_api->log(g_mod, "editor: the images could not be made; the editor shows none");
    g_gameProc = (WNDPROC)SetWindowLongPtrW(g_window, GWLP_WNDPROC, (LONG_PTR)EditorProc);
    RECT client;
    GetClientRect(g_window, &client);
    LogOverlays();
    g_api->log(g_mod, "editor: drawing with Direct3D 11 (the game's --use-d3d11), format %d, %ldx%ld; F8 opens it (frames on thread %lu, "
        "the window's thread %lu)", (int)desc.BufferDesc.Format, client.right - client.left, client.bottom - client.top,
        GetCurrentThreadId(), GetWindowThreadProcessId(g_window, NULL));
    return g_gameProc != NULL;
}

/* Direct3D 11: a view of the back buffer about to be presented, the game's own targets put back after. */
static void Draw11(IDXGISwapChain3* swapChain)
{
    ID3D11Texture2D* buffer = NULL;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return;
    ID3D11RenderTargetView* view = NULL;
    if (SUCCEEDED(g_device11->CreateRenderTargetView(buffer, NULL, &view)))
    {
        ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* depth = NULL;
        g_context11->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
        g_context11->OMSetRenderTargets(1, &view, NULL);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_context11->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth);
        for (ID3D11RenderTargetView* target : targets) if (target != NULL) target->Release();
        if (depth != NULL) depth->Release();
        view->Release();
    }
    else
    {
        static bool said;
        if (!said) { said = true; g_api->log(g_mod, "editor: Direct3D 11: the back buffer takes no render target view; the editor is not drawn"); }
    }
    buffer->Release();
}

static bool Initialize(IDXGISwapChain3* swapChain)
{
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(swapChain->GetDesc(&desc)) || FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device)))) return false;
    g_window = desc.OutputWindow;
    g_buffers = desc.BufferCount < MAX_BUFFERS ? desc.BufferCount : MAX_BUFFERS;
    g_format = desc.BufferDesc.Format;
    D3D12_DESCRIPTOR_HEAP_DESC rtv = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, MAX_BUFFERS, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC srv = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(g_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_rtvHeap))) || FAILED(g_device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&g_srvHeap))))
        return false;
    g_rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < MAX_BUFFERS; i++)
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocators[i])))) return false;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0], NULL, IID_PPV_ARGS(&g_list)))) return false;
    g_list->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_fenceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

    CreateImGui();
    RECT client;
    GetClientRect(g_window, &client);
    if (!ImGui_ImplWin32_Init(g_window) || !InitDx12Backend()) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    ImagesView(&cpu, &gpu);
    if (!icons::Upload(g_device, g_queue, g_execute, cpu, gpu)) g_api->log(g_mod, "editor: the images could not be made; the editor shows none");
    g_gameProc = (WNDPROC)SetWindowLongPtrW(g_window, GWLP_WNDPROC, (LONG_PTR)EditorProc);
    LogOverlays();
    g_api->log(g_mod, "editor: drawing on the game's queue %p, %u back buffers, format %d, %ldx%ld; F8 opens it (frames on thread %lu, "
        "the window's thread %lu)", (void*)g_queue, g_buffers, (int)g_format, client.right - client.left, client.bottom - client.top,
        GetCurrentThreadId(), GetWindowThreadProcessId(g_window, NULL));
    return g_gameProc != NULL;
}

static void Render(IDXGISwapChain3* swapChain)
{
    if (g_failed) return;
    /* Direct3D 11 when the swap chain's device is one (a D3D12 swap chain's is not); Direct3D 12 once its queue is picked */
    if (g_queue == NULL && g_device11 == NULL)
    {
        ID3D11Device* device = NULL;
        if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device))) && device != NULL) g_device11 = device;
        else if (!PickQueue(swapChain)) return;
    }
    else if (g_device11 == NULL && !OnOurQueue(swapChain)) return;
    EnterCriticalSection(&g_lock);
    if (!g_ready)
    {
        if (!(g_device11 != NULL ? Initialize11(swapChain) : Initialize(swapChain))) { g_failed = 1; g_api->log(g_mod, "editor: could not set up drawing; the editor stays off"); }
        else g_ready = 1;
    }
    static bool shown;
    if (g_ready && (g_open != 0) != shown)
    {
        shown = g_open != 0;
        if (shown) editorapp::Opened(); else editorapp::Closed();
    }
    if (g_ready && g_saveFailed) { InterlockedExchange(&g_saveFailed, 0); editorapp::SaveStatus(false, g_saveStatus); }
    if (g_ready) editorapp::Tick();
    /* the window; while it is closed the MOD SYNC prompt alone over the game, or else HellForge's message (no input, the
     * game's own cursor kept) */
    bool modal = g_ready && !g_open && g_modal;
    bool overlay = g_ready && !g_open && !modal && editorapp::NoticeShown();
    if (g_ready && (g_open || modal || overlay))
    {
        DXGI_SWAP_CHAIN_DESC desc;
        if (FAILED(swapChain->GetDesc(&desc))) { LeaveCriticalSection(&g_lock); return; }
        if (g_device11 == NULL && desc.BufferDesc.Format != g_format)
        {
            WaitFence(g_fenceValue);
            ImGui_ImplDX12_Shutdown();
            g_format = desc.BufferDesc.Format;
            if (!InitDx12Backend()) { g_failed = 1; LeaveCriticalSection(&g_lock); return; }
        }
        /* the fonts at the window's scale: made again (after our last frame) when the screen or the interface size changes */
        float width = (float)desc.BufferDesc.Width, height = (float)desc.BufferDesc.Height;
        if (ui::BuildFonts(editorapp::PixelScale(width, height)))
        {
            g_api->log(g_mod, "editor: fonts for %.0fx%.0f at %.3f pixels a unit", width, height, editorapp::PixelScale(width, height));
            if (g_device11 != NULL) ImGui_ImplDX11_InvalidateDeviceObjects();
            else
            {
                WaitFence(g_fenceValue);
                ImGui_ImplDX12_InvalidateDeviceObjects();
            }
        }
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = !overlay;
        if (overlay) io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        if (g_device11 != NULL) ImGui_ImplDX11_NewFrame();
        else ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (overlay) editorapp::Overlay(ImGui::GetBackgroundDrawList(), width, height);
        else if (modal) editorapp::Modal(ImGui::GetBackgroundDrawList(), width, height, io.DeltaTime);
        else editorapp::Frame(ImGui::GetBackgroundDrawList(), width, height, io.DeltaTime);
        ImGui::Render();

        UINT index = swapChain->GetCurrentBackBufferIndex() % MAX_BUFFERS;
        ID3D12Resource* buffer = NULL;
        if (g_device11 != NULL) Draw11(swapChain);
        else if (SUCCEEDED(swapChain->GetBuffer(swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer))))
        {
            WaitFence(g_frameFence[index]);
            g_allocators[index]->Reset();
            g_list->Reset(g_allocators[index], NULL);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += (SIZE_T)index * g_rtvSize;
            g_device->CreateRenderTargetView(buffer, NULL, rtv);
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = buffer;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_list->ResourceBarrier(1, &barrier);
            g_list->OMSetRenderTargets(1, &rtv, FALSE, NULL);
            g_list->SetDescriptorHeaps(1, &g_srvHeap);
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_list);
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_list->ResourceBarrier(1, &barrier);
            g_list->Close();
            ID3D12CommandList* lists[] = {g_list};
            g_execute(g_queue, 1, lists);
            g_queue->Signal(g_fence, ++g_fenceValue);
            g_frameFence[index] = g_fenceValue;
            buffer->Release();
        }
    }
    /* sounds asked for this frame (the closing one's too) go to the window's thread */
    if (g_ready && sounds::Waiting() && !sounds::OnGameThread()) PostMessageW(g_window, WM_EDITOR_SOUND, 0, 0);
    LeaveCriticalSection(&g_lock);
}

/* Before a present: a save due, and the editor drawn into the back buffer about to be shown. */
static void BeforePresent(IDXGISwapChain3* swapChain, UINT flags)
{
    SaveTick();
    if (!(flags & DXGI_PRESENT_TEST)) Render(swapChain);
}

static void WaitOurFrame()
{
    EnterCriticalSection(&g_lock);
    if (g_ready) WaitFence(g_fenceValue); /* our last frame is done with the old buffers (D3D11: none is kept) */
    LeaveCriticalSection(&g_lock);
}

/* How deep this thread is in the editor's Present / Present1 / ResizeBuffers detours: a present can pass two of them
 * (the first hook, and one put in front of an overlay that hooked over it, when that overlay calls on into the first
 * after all); only the outermost draws. */
static thread_local int g_depth;
static volatile LONG g_presents; /* presents through the editor's detours (Watchdog) */

static HRESULT Present_(PresentFn next, IDXGISwapChain3* swapChain, UINT interval, UINT flags)
{
    if (g_depth++ == 0) { InterlockedIncrement(&g_presents); BeforePresent(swapChain, flags); }
    HRESULT result = next(swapChain, interval, flags);
    g_depth--;
    return result;
}

static HRESULT Present1_(Present1Fn next, IDXGISwapChain3* swapChain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
{
    if (g_depth++ == 0) { InterlockedIncrement(&g_presents); BeforePresent(swapChain, flags); }
    HRESULT result = next(swapChain, interval, flags, parameters);
    g_depth--;
    return result;
}

static HRESULT Resize_(ResizeBuffersFn next, IDXGISwapChain3* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    if (g_depth++ == 0) WaitOurFrame();
    HRESULT result = next(swapChain, count, width, height, format, flags);
    g_depth--;
    return result;
}

static HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain3* swapChain, UINT interval, UINT flags)
{ return Present_(g_present, swapChain, interval, flags); }
static HRESULT STDMETHODCALLTYPE Present1Detour(IDXGISwapChain3* swapChain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
{ return Present1_(g_present1, swapChain, interval, flags, parameters); }
static HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain3* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{ return Resize_(g_resize, swapChain, count, width, height, format, flags); }

/* A Direct3D 11 swap chain's own methods, where they are not the D3D12 one's (FindMethods11). */
static HRESULT STDMETHODCALLTYPE Present11Detour(IDXGISwapChain3* swapChain, UINT interval, UINT flags)
{ return Present_(g_present11, swapChain, interval, flags); }
static HRESULT STDMETHODCALLTYPE Present1_11Detour(IDXGISwapChain3* swapChain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
{ return Present1_(g_present1_11, swapChain, interval, flags, parameters); }
static HRESULT STDMETHODCALLTYPE Resize11Detour(IDXGISwapChain3* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{ return g_resize11(swapChain, count, width, height, format, flags); }

/* The detours put in front of an overlay that hooked a method's code over the editor's hook (Watchdog), each with its
 * own original: up to AGAIN times a method. */
#define AGAIN 4
static PresentFn g_presentAgain[AGAIN];
static Present1Fn g_present1Again[AGAIN];
static ResizeBuffersFn g_resizeAgain[AGAIN];
template <int N> static HRESULT STDMETHODCALLTYPE PresentAgain(IDXGISwapChain3* swapChain, UINT interval, UINT flags)
{ return Present_(g_presentAgain[N], swapChain, interval, flags); }
template <int N> static HRESULT STDMETHODCALLTYPE Present1Again(IDXGISwapChain3* swapChain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
{ return Present1_(g_present1Again[N], swapChain, interval, flags, parameters); }
template <int N> static HRESULT STDMETHODCALLTYPE ResizeAgain(IDXGISwapChain3* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{ return Resize_(g_resizeAgain[N], swapChain, count, width, height, format, flags); }
static void* const kPresentAgain[AGAIN] = {(void*)PresentAgain<0>, (void*)PresentAgain<1>, (void*)PresentAgain<2>, (void*)PresentAgain<3>};
static void* const kPresent1Again[AGAIN] = {(void*)Present1Again<0>, (void*)Present1Again<1>, (void*)Present1Again<2>, (void*)Present1Again<3>};
static void* const kResizeAgain[AGAIN] = {(void*)ResizeAgain<0>, (void*)ResizeAgain<1>, (void*)ResizeAgain<2>, (void*)ResizeAgain<3>};

static void STDMETHODCALLTYPE ExecuteDetour(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (g_queue == NULL && g_device11 == NULL) SeeQueue(queue, __builtin_return_address(0)); /* until PickQueue has one */
    g_execute(queue, count, lists);
}

/* ---- set-up ----------------------------------------------------------------------------------------------------- */

/* Where each method was found: its slot in the object's method table (the table every object of that kind shares,
 * where it lies in a module's image), for the fallback below. */
static void** g_slotPresent;
static void** g_slotPresent1;
static void** g_slotResize;
static void** g_slotExecute;

/* A method hooked where its code is (MinHook, through the loader), else, where the code cannot be hooked (another
 * overlay already sends the call to code of its own, and MinHook finds no room near it), its slot in the shared method
 * table pointed at the detour, the code that was there kept as the original: the editor then runs in front of that
 * overlay, as overlays chain. A table in allocated memory (one object's own) is not patched: the game's objects would
 * not use it. 0 when hooked. */
static int HookMethod(HfMod* mod, const char* name, void* target, void** slot, void* detour, void** original)
{
    if (!g_api->hook(mod, (uintptr_t)target, detour, original)) return 0;
    std::string code = ModuleOf(target), table = slot != NULL ? ModuleOf(slot) : "";
    g_api->log(mod, "editor: %s at %p (%s) could not be hooked in place; its method table at %p (%s)", name, target,
        code.empty() ? "allocated memory: another overlay's code?" : code.c_str(), (void*)slot, table.empty() ? "allocated memory" : table.c_str());
    DWORD protect;
    if (slot == NULL || table.empty() || *slot != target || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &protect)) return 1;
    *original = target;
    InterlockedExchangePointer(slot, detour);
    VirtualProtect(slot, sizeof *slot, protect, &protect);
    g_api->log(mod, "editor: %s: the method table now calls the editor first (then %p)", name, target);
    return 0;
}

/* Where a jump at `code` goes: jmp rel32 / rel8, jmp [rip+disp32], mov rax, imm64; jmp rax (how overlays send a
 * method to their own code). NULL: not a jump. */
static uint8_t* JumpTarget(const uint8_t* code)
{
    if (code[0] == 0xE9) { int32_t rel; memcpy(&rel, code + 1, 4); return (uint8_t*)(code + 5 + rel); }
    if (code[0] == 0xEB) return (uint8_t*)(code + 2 + (int8_t)code[1]);
    if (code[0] == 0xFF && code[1] == 0x25)
    {
        int32_t disp;
        memcpy(&disp, code + 2, 4);
        const uint8_t* slot = code + 6 + disp;
        return Readable(slot, sizeof(void*)) ? *(uint8_t* const*)slot : NULL;
    }
    if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) { uint8_t* to; memcpy(&to, code + 2, sizeof to); return to; }
    return NULL;
}

/* An overlay that hooks a method's code after the editor writes its own jump over the editor's and calls the code the
 * way it was before, not the editor (RivaTuner does when it comes in late, after its 15 s delay where Steam's overlay
 * is loaded, or on the game's first frames: issue #5, F8 took the input and nothing was drawn). A thread watches the
 * first bytes of Present, Present1 and ResizeBuffers; when they change into a jump, the place it goes is hooked
 * (through the loader, as any hook), so the editor runs first again and then that overlay; when presents stop reaching
 * the editor and the code is as it was before it, the editor's own hook is put back (Watchdog, PutBack). The shared
 * method table is never patched: Steam's overlay hooks again whatever the table holds, keeping one original for the
 * method, and a table pointing at the editor made it call the editor from its own detour, the editor it, until the
 * stack ran out (ntdll RtlpCallVectoredHandlers, 0xc0000005, at the title screen). */
struct Watched { const char* name; uint8_t* code; uint8_t before[8], mine[8]; uint8_t* first; int again, saidOther, saidOurs, putBack; uint8_t* hooked[AGAIN];
    void* const* detours; void** originals; };
static Watched g_watched[3];

/* A method's first eight bytes as its module's file has them (read from dxgi.dll on disk through its section headers;
 * mapping it again as an image gives back the loaded one): the code as it is before anyone hooks it, the editor or an
 * overlay that hooked it first. false: not found. */
static bool Pristine(const void* code, uint8_t out[8])
{
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)code, &module) ||
        !GetModuleFileNameW(module, path, MAX_PATH)) return false;
    /* the sections from the loaded image's own headers (the same as the file's) */
    const uint8_t* base = (const uint8_t*)module;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    DWORD rva = (DWORD)((const uint8_t*)code - base), offset = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections && offset == 0; i++, section++)
        if (rva >= section->VirtualAddress && rva + 8 <= section->VirtualAddress + section->SizeOfRawData)
            offset = section->PointerToRawData + (rva - section->VirtualAddress);
    if (offset == 0) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD read = 0;
    OVERLAPPED at = {};
    at.Offset = offset;
    bool found = ReadFile(file, out, 8, &read, &at) && read == 8;
    CloseHandle(file);
    return found;
}

/* The code put back as it was before the editor hooked it (RivaTuner does when it takes its own hook off, as on a
 * resize, and hooks it again only seconds later): the editor's own hook put back, its first eight bytes written at once
 * over the same eight (the loader's trampoline and stub are still there), so a thread running the code sees either.
 * (Where an overlay had hooked the code before the editor, the editor's hook goes on to that overlay's stub, which stays
 * where it was: RivaTuner hooks again to the same one.)
 * false: not that case, or the code is not at an 8-byte boundary. */
static bool PutBack(Watched& w)
{
    LONG64 before, mine;
    memcpy(&before, w.before, 8);
    memcpy(&mine, w.mine, 8);
    DWORD protect;
    if (before == mine || ((uintptr_t)w.code & 7) != 0 || memcmp(w.code, w.before, 8) != 0 || JumpTarget(w.mine) == NULL ||
        !VirtualProtect(w.code, 8, PAGE_EXECUTE_READWRITE, &protect)) return false;
    bool put = InterlockedCompareExchange64((volatile LONG64*)w.code, mine, before) == before;
    VirtualProtect(w.code, 8, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), w.code, 8);
    return put;
}

/* Where a method's code jumps, read live: NULL not a jump, or not to code. */
static uint8_t* LiveJump(const uint8_t* code)
{
    uint8_t* to = JumpTarget(code);
    MEMORY_BASIC_INFORMATION memory;
    const DWORD run = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return to != NULL && VirtualQuery(to, &memory, sizeof memory) && memory.State == MEM_COMMIT && (memory.Protect & run) ? to : NULL;
}

/* The editor's hook in front of the place a method's code now jumps to (an overlay's), unless it is there already. */
static void InFront(Watched& w, uint8_t* to)
{
    bool ours = to == w.first; /* a jump back to the editor's own hook, or to where it went in front already */
    for (int i = 0; i < w.again && i < AGAIN; i++) ours |= w.hooked[i] == to;
    if (ours) { if (!w.saidOurs++) g_api->log(g_mod, "editor: %s: its code jumps to %p again, where the editor is in front already (said once)", w.name, (void*)to); return; }
    if (w.again >= AGAIN) { if (w.again++ == AGAIN) g_api->log(g_mod, "editor: %s was hooked over the editor's hook %d times; not again", w.name, AGAIN); return; }
    std::string who = ModuleOf(to);
    int failed = g_api->hook(g_mod, (uintptr_t)to, w.detours[w.again], &w.originals[w.again]);
    g_api->log(g_mod, failed ? "editor: %s: another overlay (%s, %p) hooked its code over the editor's; hooking in front of it failed" :
        "editor: %s: another overlay (%s, %p) hooked its code over the editor's; the editor runs first again", w.name,
        who.empty() ? "allocated memory: an overlay's code?" : who.c_str(), (void*)to);
    if (!failed) w.hooked[w.again++] = to;
}

/* Every 250 ms: a method whose code jumps somewhere new gets the editor in front of it. Code that is no jump at all is
 * left alone while presents still reach the editor: RivaTuner takes its own hook off around each call it passes on (the
 * code as it was for the length of the present) and puts it back after, so the code is mostly as it was, its jump only
 * there between frames. When presents stop reaching the editor, the code is watched closely for up to 100 ms to catch
 * that jump; none at all, and the code as it was before the editor, the editor's own hook is put back. */

static DWORD WINAPI Watchdog(void*)
{
    LONG seen = 0;
    for (;;)
    {
        Sleep(250);
        LONG presents = g_presents;
        bool reached = presents != seen;
        seen = presents;
        for (int m = 0; m < 3; m++)
        {
            Watched& w = g_watched[m];
            if (w.code == NULL || !Readable(w.code, 8)) continue;
            uint8_t* to = LiveJump(w.code);
            if (to != NULL) { InFront(w, to); continue; }
            if (reached || m == 2) continue; /* drawing still, or ResizeBuffers (no call every frame to tell by) */
            LARGE_INTEGER now, end, frequency;
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&now);
            end.QuadPart = now.QuadPart + frequency.QuadPart / 10;
            while (to == NULL && now.QuadPart < end.QuadPart && g_presents == presents)
            {
                SwitchToThread();
                to = LiveJump(w.code);
                QueryPerformanceCounter(&now);
            }
            if (to != NULL) { InFront(w, to); continue; }
            if (g_presents != presents) continue;
            if (w.putBack < 64 && PutBack(w))
            {
                if (w.putBack++ == 0) g_api->log(g_mod, "editor: %s's code was put back as it was before the editor (another overlay took its own hook "
                    "off); the editor's hook is back in place (said once)", w.name);
                continue;
            }
            uint8_t* code = w.code;
            if (memcmp(code, w.before, 8) != 0 && !w.saidOther++)
                g_api->log(g_mod, "editor: %s's code was changed by another program, not into a jump the editor follows (%02x %02x %02x %02x %02x); "
                    "the editor may no longer draw (said once)", w.name, code[0], code[1], code[2], code[3], code[4]);
        }
    }
}

/* Watch a hooked method's first bytes (the editor's hook in place). */
static void Watch(int i, const char* name, void* code, void* const* detours, void** originals)
{
    Watched& w = g_watched[i];
    w.name = name;
    w.detours = detours;
    w.originals = originals;
    if (Readable(code, 8)) { memcpy(w.mine, code, 8); w.first = JumpTarget((uint8_t*)code); }
    if (!Pristine(code, w.before))
    {
        memcpy(w.before, w.mine, 8); /* PutBack never matches */
        g_api->log(g_mod, "editor: %s: its code as its module's file has it was not read; the editor's hook is not put back if an overlay puts the code back", name);
    }
    w.code = (uint8_t*)code;
}

/* The addresses of the swap chain's and the queue's methods: a throwaway device, queue and swap chain on a hidden
 * window show them (every swap chain and queue of the process shares these implementations). They also show where a
 * swap chain keeps the queue it presents on (the first place in it holding the queue's address; 0x140 on Windows 11
 * 25H2) and the method table every queue has (PickQueue). */
static bool FindMethods(void** present, void** present1, void** resize, void** execute)
{
    WNDCLASSEXW type = {sizeof type, 0, DefWindowProcW, 0, 0, GetModuleHandleW(NULL), NULL, NULL, NULL, NULL, L"HellForgeEditorProbe", NULL};
    RegisterClassExW(&type);
    HWND window = CreateWindowExW(0, type.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, type.hInstance, NULL);
    ID3D12Device* device = NULL;
    ID3D12CommandQueue* queue = NULL;
    IDXGIFactory2* factory = NULL;
    IDXGISwapChain1* swapChain = NULL;
    bool found = false;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = desc.Height = 64;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (window != NULL && SUCCEEDED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) &&
        SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateSwapChainForHwnd(queue, window, &desc, NULL, NULL, &swapChain)))
    {
        void** chain = *(void***)swapChain;
        void** commands = *(void***)queue;
        *present = chain[8];    /* IDXGISwapChain::Present */
        *resize = chain[13];    /* IDXGISwapChain::ResizeBuffers */
        *present1 = chain[22];  /* IDXGISwapChain1::Present1 */
        *execute = commands[10]; /* ID3D12CommandQueue::ExecuteCommandLists */
        g_slotPresent = &chain[8]; g_slotResize = &chain[13]; g_slotPresent1 = &chain[22]; g_slotExecute = &commands[10];
        g_queueTable = commands;
        IDXGISwapChain3* chain3 = NULL; /* the object Present is called on */
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&chain3))))
        {
            for (size_t at = sizeof(void*); at < 0x2000 && g_queueOffset == 0 && Readable((const char*)chain3 + at, sizeof(void*)); at += sizeof(void*))
                if (*(void* const*)((const char*)chain3 + at) == (void*)queue) g_queueOffset = at;
            chain3->Release();
        }
        found = true;
    }
    if (swapChain) swapChain->Release();
    if (factory) factory->Release();
    if (queue) queue->Release();
    if (device) device->Release();
    if (window) DestroyWindow(window);
    UnregisterClassW(type.lpszClassName, type.hInstance);
    return found;
}

/* The same for a Direct3D 11 swap chain (a game started with --use-d3d11): a throwaway device and swap chain show them. */
static bool FindMethods11(void** present, void** present1, void** resize)
{
    WNDCLASSEXW type = {sizeof type, 0, DefWindowProcW, 0, 0, GetModuleHandleW(NULL), NULL, NULL, NULL, NULL, L"HellForgeEditorProbe11", NULL};
    RegisterClassExW(&type);
    HWND window = CreateWindowExW(0, type.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, type.hInstance, NULL);
    ID3D11Device* device = NULL;
    IDXGIFactory2* factory = NULL;
    IDXGISwapChain1* swapChain = NULL;
    bool found = false;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = desc.Height = 64;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (window != NULL && SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, NULL, NULL)) &&
        SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->CreateSwapChainForHwnd(device, window, &desc, NULL, NULL, &swapChain)))
    {
        void** chain = *(void***)swapChain;
        *present = chain[8];
        *resize = chain[13];
        *present1 = chain[22];
        found = true;
    }
    if (swapChain) swapChain->Release();
    if (factory) factory->Release();
    if (device) device->Release();
    if (window) DestroyWindow(window);
    UnregisterClassW(type.lpszClassName, type.hInstance);
    return found;
}

static void Start(HfMod* mod, HfEvent event, void* user)
{
    editorapp::AddModContent(); /* every mod has registered its content by now; before the first frame */
    /* game.dll runs by now (its code is unpacked once it starts): the menu sounds can be checked */
    g_api->log(mod, "editor: menu sounds %s", sounds::Init(g_api, mod) ? "on (the game's UI sound function checked)" : "off");
    void *present, *present1, *resize, *execute;
    if (!FindMethods(&present, &present1, &resize, &execute)) { g_api->log(mod, "editor: no D3D12 device for the probe; the editor stays off"); return; }
    if (g_queueOffset != 0) g_api->log(mod, "editor: a swap chain keeps its queue at +0x%x", (unsigned)g_queueOffset);
    else g_api->log(mod, "editor: where a swap chain keeps its queue was not found; the busiest direct queue will be drawn on");
    LogOverlays();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    bool hooked =
        !HookMethod(mod, "ExecuteCommandLists", execute, g_slotExecute, (void*)ExecuteDetour, (void**)&g_execute) &&
        !HookMethod(mod, "ResizeBuffers", resize, g_slotResize, (void*)ResizeBuffersDetour, (void**)&g_resize) &&
        !HookMethod(mod, "Present", present, g_slotPresent, (void*)PresentDetour, (void**)&g_present) &&
        !HookMethod(mod, "Present1", present1, g_slotPresent1, (void*)Present1Detour, (void**)&g_present1);
    /* and kept first, should an overlay hook their code over the editor's later (Watchdog) */
    if (hooked)
    {
        Watch(0, "Present", present, kPresentAgain, (void**)g_presentAgain);
        Watch(1, "Present1", present1, kPresent1Again, (void**)g_present1Again);
        Watch(2, "ResizeBuffers", resize, kResizeAgain, (void**)g_resizeAgain);
        HANDLE thread = CreateThread(NULL, 0, Watchdog, NULL, 0, NULL);
        if (thread != NULL) CloseHandle(thread);
    }
    /* a Direct3D 11 swap chain's methods, hooked too where they are other code than the D3D12 one's */
    void *present11, *present1_11, *resize11;
    if (hooked && FindMethods11(&present11, &present1_11, &resize11))
    {
        if (present11 != present) g_api->hook(mod, (uintptr_t)present11, (void*)Present11Detour, (void**)&g_present11);
        if (present1_11 != present1) g_api->hook(mod, (uintptr_t)present1_11, (void*)Present1_11Detour, (void**)&g_present1_11);
        if (resize11 != resize) g_api->hook(mod, (uintptr_t)resize11, (void*)Resize11Detour, (void**)&g_resize11);
    }
    if (hooked && user32 != NULL)
    {
        g_api->hook(mod, (uintptr_t)GetProcAddress(user32, "ClipCursor"), (void*)ClipCursorDetour, (void**)&g_clipCursor);
        g_api->hook(mod, (uintptr_t)GetProcAddress(user32, "SetCursorPos"), (void*)SetCursorPosDetour, (void**)&g_setCursorPos);
        g_api->hook(mod, (uintptr_t)GetProcAddress(user32, "GetRawInputBuffer"), (void*)RawInputBufferDetour, (void**)&g_rawInputBuffer);
    }
    g_api->log(mod, hooked ? "editor: hooked the swap chain and the queue; drawing starts with the next frame" : "editor: could not hook D3D12; the editor stays off");
}

extern "C" HF_MOD_EXPORT int hellforge_mod_init(const HfApi* hf, HfMod* mod)
{
    if (hf->version < 6) return 1;
    g_api = hf;
    g_mod = mod;
    /* Local Stats is the editor's neighbour in the mods folder. */
    std::string folder = hf->mod_folder(mod);
    size_t slash = folder.find_last_of("\\/");
    if (slash != std::string::npos) g_modsFolder = folder.substr(0, slash);
    wchar_t sandboxStats[MAX_PATH];
    DWORD sandboxLen = GetEnvironmentVariableW(L"HELLFORGE_SANDBOX_STATS", sandboxStats, MAX_PATH);
    if (sandboxLen > 0 && sandboxLen < MAX_PATH) localstats::SetSandboxSaveFolder(sandboxStats);
    InitializeCriticalSection(&g_lock);
    editorapp::Host host = {hf, mod, folder, g_modsFolder, CloseEditor, SaveSoon, ShowModal};
    editorapp::Init(host);
    /* Once the game runs and has its device: entity data loads after the renderer is up. */
    return hf->on(mod, HF_EVENT_ENTITIES_READY, Start, NULL);
}
