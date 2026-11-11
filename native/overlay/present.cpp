/* The swap chain's and the queue's methods, hooked where their code is. Their addresses come from throwaway devices
 * and swap chains: every swap chain and queue of the process shares one implementation. A watchdog puts the host back
 * in front when another overlay (RivaTuner, issue #5) later writes its own jump over the host's and calls past it. */
#include <string.h>
#include "host_internal.h"

namespace overlay
{
ExecuteFn g_execute;

namespace
{
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain3*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain3*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* ResizeFn)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

/* The methods as one probe found them, and the slot of each in its shared method table (for the fallback). */
struct Methods
{
    void* present;
    void* present1;
    void* resize;
    void* execute;
    void** slots[4];
};

/* A present can pass the host twice (its first hook, and one put in front of an overlay that calls on into the first);
 * only the outermost draws. */
thread_local int t_depth;
volatile LONG g_presents; /* presents that reached the host (the watchdog) */

HRESULT AroundPresent(IDXGISwapChain3* chain, UINT flags)
{
    if (t_depth++ == 0)
    {
        InterlockedIncrement(&g_presents);
        if (!(flags & DXGI_PRESENT_TEST)) Render(chain);
    }
    return S_OK;
}

PresentFn g_present[6];   /* 0: the D3D12 probe's, 1: the D3D11 one's, 2-5: in front of other overlays */
Present1Fn g_present1[6];
ResizeFn g_resize[6];

template <int N> HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain3* chain, UINT interval, UINT flags)
{
    AroundPresent(chain, flags);
    HRESULT result = g_present[N](chain, interval, flags);
    t_depth--;
    return result;
}
template <int N> HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain3* chain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    AroundPresent(chain, flags);
    HRESULT result = g_present1[N](chain, interval, flags, p);
    t_depth--;
    return result;
}
template <int N> HRESULT STDMETHODCALLTYPE ResizeHook(IDXGISwapChain3* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    if (t_depth++ == 0) WaitLastFrame(); /* the old buffers are free of the host's last frame */
    HRESULT result = g_resize[N](chain, count, width, height, format, flags);
    t_depth--;
    return result;
}
void* const kPresentHooks[6] = {(void*)PresentHook<0>, (void*)PresentHook<1>, (void*)PresentHook<2>, (void*)PresentHook<3>, (void*)PresentHook<4>, (void*)PresentHook<5>};
void* const kPresent1Hooks[6] = {(void*)Present1Hook<0>, (void*)Present1Hook<1>, (void*)Present1Hook<2>, (void*)Present1Hook<3>, (void*)Present1Hook<4>, (void*)Present1Hook<5>};
void* const kResizeHooks[6] = {(void*)ResizeHook<0>, (void*)ResizeHook<1>, (void*)ResizeHook<2>, (void*)ResizeHook<3>, (void*)ResizeHook<4>, (void*)ResizeHook<5>};

void STDMETHODCALLTYPE ExecuteHook(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    NoteQueue(queue, __builtin_return_address(0));
    g_execute(queue, count, lists);
}

std::string ModuleName(const void* at)
{
    HMODULE module = NULL;
    char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &module) ||
        GetModuleFileNameA(module, path, MAX_PATH) == 0) return std::string();
    const char* slash = strrchr(path, '\\');
    return slash != NULL ? slash + 1 : path;
}

bool CanRead(const void* at, size_t bytes)
{
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(at, &info, sizeof info) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return (const char*)at + bytes <= (const char*)info.BaseAddress + info.RegionSize;
}

/* A window of the process's own for a probe swap chain, gone with it. */
struct ProbeWindow
{
    HWND window = NULL;
    const wchar_t* name;
    explicit ProbeWindow(const wchar_t* className) : name(className)
    {
        WNDCLASSEXW type = {};
        type.cbSize = sizeof type;
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(NULL);
        type.lpszClassName = name;
        RegisterClassExW(&type);
        window = CreateWindowExW(0, name, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, type.hInstance, NULL);
    }
    ~ProbeWindow()
    {
        if (window != NULL) DestroyWindow(window);
        UnregisterClassW(name, GetModuleHandleW(NULL));
    }
};

DXGI_SWAP_CHAIN_DESC1 ProbeDesc()
{
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = desc.Height = 64;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    return desc;
}

void TakeChainMethods(IUnknown* chain, Methods* out)
{
    void** table = *(void***)chain;
    out->present = table[8];   /* IDXGISwapChain::Present */
    out->resize = table[13];   /* IDXGISwapChain::ResizeBuffers */
    out->present1 = table[22]; /* IDXGISwapChain1::Present1 */
    out->slots[0] = &table[8];
    out->slots[1] = &table[22];
    out->slots[2] = &table[13];
}

/* The D3D12 methods, the queue's method table and where a swap chain keeps its queue. */
bool Probe12(Methods* out, size_t* queueOffset, void** queueTable)
{
    ProbeWindow probe(L"HellForgeOverlayProbe12");
    ID3D12Device* device = NULL;
    ID3D12CommandQueue* queue = NULL;
    IDXGIFactory2* factory = NULL;
    IDXGISwapChain1* chain = NULL;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    DXGI_SWAP_CHAIN_DESC1 desc = ProbeDesc();
    bool found = probe.window != NULL && SUCCEEDED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) &&
        SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) && SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateSwapChainForHwnd(queue, probe.window, &desc, NULL, NULL, &chain));
    if (found)
    {
        TakeChainMethods(chain, out);
        void** commands = *(void***)queue;
        out->execute = commands[10]; /* ID3D12CommandQueue::ExecuteCommandLists */
        out->slots[3] = &commands[10];
        *queueTable = commands;
        IDXGISwapChain3* chain3 = NULL;
        if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain3))))
        {
            for (size_t at = sizeof(void*); at < 0x2000 && *queueOffset == 0 && CanRead((char*)chain3 + at, sizeof(void*)); at += sizeof(void*))
                if (*(void**)((char*)chain3 + at) == (void*)queue) *queueOffset = at;
            chain3->Release();
        }
    }
    for (IUnknown* object : {(IUnknown*)chain, (IUnknown*)factory, (IUnknown*)queue, (IUnknown*)device})
        if (object != NULL) object->Release();
    return found;
}

bool Probe11(Methods* out)
{
    ProbeWindow probe(L"HellForgeOverlayProbe11");
    ID3D11Device* device = NULL;
    IDXGIFactory2* factory = NULL;
    IDXGISwapChain1* chain = NULL;
    DXGI_SWAP_CHAIN_DESC1 desc = ProbeDesc();
    bool found = probe.window != NULL &&
        SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, NULL, NULL)) &&
        SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->CreateSwapChainForHwnd(device, probe.window, &desc, NULL, NULL, &chain));
    if (found) TakeChainMethods(chain, out);
    for (IUnknown* object : {(IUnknown*)chain, (IUnknown*)factory, (IUnknown*)device})
        if (object != NULL) object->Release();
    return found;
}

/* Hooked where the code is; where that fails (another overlay sends the call to code of its own that cannot be hooked),
 * the shared method table's slot points at the detour instead, the code there kept as the original. A table in
 * allocated memory is one object's own and not used by the game's objects, so it is left alone. */
bool HookMethod(const char* name, void* code, void** slot, void* detour, void** original)
{
    if (!Hook(code, detour, original)) return true;
    std::string table = slot != NULL ? ModuleName(slot) : std::string();
    DWORD protect;
    if (slot == NULL || table.empty() || *slot != code || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &protect))
    {
        Log("overlay: %s at %p could not be hooked", name, code);
        return false;
    }
    *original = code;
    InterlockedExchangePointer(slot, detour);
    VirtualProtect(slot, sizeof *slot, protect, &protect);
    Log("overlay: %s could not be hooked in place; its method table (%s) calls the host first", name, table.c_str());
    return true;
}

/* ---- the watchdog ---- */

/* Where a jump at `code` leads: jmp rel32, jmp rel8, jmp [rip+disp32], or mov rax, imm64 + jmp rax. NULL: no jump. */
uint8_t* JumpOf(const uint8_t* code)
{
    int32_t rel32;
    switch (code[0])
    {
    case 0xE9: memcpy(&rel32, code + 1, 4); return (uint8_t*)code + 5 + rel32;
    case 0xEB: return (uint8_t*)code + 2 + (int8_t)code[1];
    case 0xFF:
        if (code[1] != 0x25) return NULL;
        memcpy(&rel32, code + 2, 4);
        return CanRead(code + 6 + rel32, 8) ? *(uint8_t**)(code + 6 + rel32) : NULL;
    case 0x48:
        if (code[1] != 0xB8 || code[10] != 0xFF || code[11] != 0xE0) return NULL;
        uint8_t* to;
        memcpy(&to, code + 2, 8);
        return to;
    default: return NULL;
    }
}

uint8_t* LiveJumpOf(const uint8_t* code)
{
    uint8_t* to = JumpOf(code);
    MEMORY_BASIC_INFORMATION info;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (to == NULL || !VirtualQuery(to, &info, sizeof info) || info.State != MEM_COMMIT || !(info.Protect & exec)) return NULL;
    return to;
}

/* The first eight bytes of a method as its DLL's file has them: the code before anyone hooked it. */
bool FileBytes(const void* code, uint8_t out[8])
{
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)code, &module) ||
        GetModuleFileNameW(module, path, MAX_PATH) == 0) return false;
    const uint8_t* image = (const uint8_t*)module;
    const IMAGE_NT_HEADERS* headers = (const IMAGE_NT_HEADERS*)(image + ((const IMAGE_DOS_HEADER*)image)->e_lfanew);
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(headers);
    DWORD rva = (DWORD)((const uint8_t*)code - image), fileAt = 0;
    for (WORD i = 0; i < headers->FileHeader.NumberOfSections; i++)
    {
        const IMAGE_SECTION_HEADER& s = sections[i];
        if (rva >= s.VirtualAddress && rva + 8 <= s.VirtualAddress + s.SizeOfRawData) { fileAt = s.PointerToRawData + rva - s.VirtualAddress; break; }
    }
    HANDLE file = fileAt != 0 ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE;
    if (file == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED where = {};
    where.Offset = fileAt;
    DWORD got = 0;
    bool read = ReadFile(file, out, 8, &got, &where) && got == 8;
    CloseHandle(file);
    return read;
}

const int AGAIN = 4; /* hooks in front of other overlays, per method */

struct Guarded
{
    const char* name;
    uint8_t* code;
    uint8_t original[8]; /* as the DLL's file has it */
    uint8_t ours[8];     /* with the host's hook in */
    uint8_t* ourJump;
    uint8_t* fronted[AGAIN];
    int frontCount;
    int putBacks;
    bool saidOurs, saidStrange;
    void* const* detours;
    void** originals;
};
Guarded g_guarded[3];

/* The host's hook in front of the code a method now jumps to, unless it is there already. */
void GetInFront(Guarded& g, uint8_t* to)
{
    bool known = to == g.ourJump;
    for (int i = 0; i < g.frontCount && i < AGAIN; i++) known = known || g.fronted[i] == to;
    if (known)
    {
        if (!g.saidOurs) Log("overlay: %s jumps to %p again, where the host runs first already", g.name, (void*)to);
        g.saidOurs = true;
        return;
    }
    if (g.frontCount >= AGAIN)
    {
        if (g.frontCount++ == AGAIN) Log("overlay: %s was hooked over the host's hook %d times; not again", g.name, AGAIN);
        return;
    }
    std::string who = ModuleName(to);
    int slot = 2 + g.frontCount;
    bool hooked = !Hook(to, g.detours[slot], &g.originals[slot]);
    Log("overlay: %s: another overlay (%s) hooked its code over the host's; %s", g.name, who.empty() ? "allocated memory" : who.c_str(),
        hooked ? "the host runs first again" : "getting in front of it failed");
    if (hooked) g.fronted[g.frontCount++] = to;
}

/* An overlay that took its own hook off put back the code as the DLL had it: the host's eight bytes go back at once. */
bool PutOursBack(Guarded& g)
{
    LONG64 original, ours;
    memcpy(&original, g.original, 8);
    memcpy(&ours, g.ours, 8);
    DWORD protect;
    if (original == ours || ((uintptr_t)g.code & 7) || memcmp(g.code, g.original, 8) != 0 || JumpOf(g.ours) == NULL) return false;
    if (!VirtualProtect(g.code, 8, PAGE_EXECUTE_READWRITE, &protect)) return false;
    bool put = InterlockedCompareExchange64((volatile LONG64*)g.code, ours, original) == original;
    VirtualProtect(g.code, 8, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), g.code, 8);
    return put;
}

/* Waits up to 100 ms, while presents still do not reach the host, for a jump to show: RivaTuner takes its hook off
 * around each call it passes on, so its jump is there only between frames. */
uint8_t* WatchForJump(const uint8_t* code, LONG presents)
{
    LARGE_INTEGER now, until, second;
    QueryPerformanceFrequency(&second);
    QueryPerformanceCounter(&now);
    until.QuadPart = now.QuadPart + second.QuadPart / 10;
    uint8_t* to = NULL;
    for (; to == NULL && now.QuadPart < until.QuadPart && g_presents == presents; QueryPerformanceCounter(&now))
    {
        SwitchToThread();
        to = LiveJumpOf(code);
    }
    return to;
}

void Check(Guarded& g, bool presentsReached, bool isResize, LONG presents)
{
    if (g.code == NULL || !CanRead(g.code, 8)) return;
    uint8_t* to = LiveJumpOf(g.code);
    if (to == NULL && !presentsReached && !isResize) to = WatchForJump(g.code, presents);
    if (to != NULL) { GetInFront(g, to); return; }
    if (presentsReached || isResize || g_presents != presents) return;
    if (g.putBacks < 64 && PutOursBack(g))
    {
        if (g.putBacks++ == 0) Log("overlay: %s was put back as its DLL has it (an overlay took its hook off); the host's hook is back", g.name);
        return;
    }
    if (memcmp(g.code, g.original, 8) != 0 && !g.saidStrange)
    {
        g.saidStrange = true;
        Log("overlay: %s was changed by another program into no jump the host follows (%02x %02x %02x); screens may not draw", g.name,
            g.code[0], g.code[1], g.code[2]);
    }
}

DWORD WINAPI Watchdog(void*)
{
    for (LONG seen = 0;;)
    {
        Sleep(250);
        LONG presents = g_presents;
        bool reached = presents != seen;
        seen = presents;
        for (int i = 0; i < 3; i++) Check(g_guarded[i], reached, i == 2, presents);
    }
}

void Guard(int i, const char* name, void* code, void* const* detours, void** originals)
{
    Guarded& g = g_guarded[i];
    g.name = name;
    g.detours = detours;
    g.originals = originals;
    if (CanRead(code, 8)) { memcpy(g.ours, code, 8); g.ourJump = JumpOf((uint8_t*)code); }
    if (!FileBytes(code, g.original))
    {
        memcpy(g.original, g.ours, 8); /* PutOursBack never matches */
        Log("overlay: %s as its DLL has it was not read; the host's hook is not put back if an overlay puts the code back", name);
    }
    g.code = (uint8_t*)code;
}

/* A D3D11 swap chain's methods, where they are other code than the D3D12 one's (a game started with --use-d3d11). */
void HookD3D11(const Methods& first)
{
    Methods m = {};
    if (!Probe11(&m)) { Log("overlay: no D3D11 probe; a game drawing with D3D11 gets no screens"); return; }
    if (m.present != first.present) Hook(m.present, kPresentHooks[1], (void**)&g_present[1]);
    if (m.present1 != first.present1) Hook(m.present1, kPresent1Hooks[1], (void**)&g_present1[1]);
    if (m.resize != first.resize) Hook(m.resize, kResizeHooks[1], (void**)&g_resize[1]);
}
} // namespace

extern size_t g_queueOffset;
extern void* g_queueTable;

bool HookPresent()
{
    Methods m = {};
    if (!Probe12(&m, &g_queueOffset, &g_queueTable))
    {
        /* no D3D12 at all: the D3D11 methods alone */
        Log("overlay: no D3D12 device for the probe; trying D3D11 only");
        if (!Probe11(&m)) return false;
        bool hooked = HookMethod("ResizeBuffers", m.resize, m.slots[2], kResizeHooks[0], (void**)&g_resize[0]) &&
            HookMethod("Present", m.present, m.slots[0], kPresentHooks[0], (void**)&g_present[0]) &&
            HookMethod("Present1", m.present1, m.slots[1], kPresent1Hooks[0], (void**)&g_present1[0]);
        HookCursor();
        return hooked;
    }
    Log(g_queueOffset != 0 ? "overlay: a swap chain keeps its queue at +0x%x" : "overlay: where a swap chain keeps its queue is unknown (%x)",
        (unsigned)g_queueOffset);
    bool hooked = HookMethod("ExecuteCommandLists", m.execute, m.slots[3], (void*)ExecuteHook, (void**)&g_execute) &&
        HookMethod("ResizeBuffers", m.resize, m.slots[2], kResizeHooks[0], (void**)&g_resize[0]) &&
        HookMethod("Present", m.present, m.slots[0], kPresentHooks[0], (void**)&g_present[0]) &&
        HookMethod("Present1", m.present1, m.slots[1], kPresent1Hooks[0], (void**)&g_present1[0]);
    if (!hooked) return false;
    Guard(0, "Present", m.present, kPresentHooks, (void**)g_present);
    Guard(1, "Present1", m.present1, kPresent1Hooks, (void**)g_present1);
    Guard(2, "ResizeBuffers", m.resize, kResizeHooks, (void**)g_resize);
    HANDLE watchdog = CreateThread(NULL, 0, Watchdog, NULL, 0, NULL);
    if (watchdog != NULL) CloseHandle(watchdog);
    HookD3D11(m);
    HookCursor();
    Log("overlay: hooked the swap chain and the queue; drawing starts with the next frame");
    return true;
}
} // namespace overlay
