/* The D3D12 view heap: one shader-visible heap for ImGui's font, every screen's pictures and the backdrop. A
 * shader-visible heap cannot be copied from, so growing it would mean moving every view the frames in flight still
 * use; it is made at its full size instead, and a picture past it is refused. What a frame in flight may still use
 * (views, textures, upload buffers) is freed once the fence says that frame is done. */
#include <vector>
#include "host_internal.h"

namespace overlay
{
ID3D12DescriptorHeap* g_srvHeap;

namespace
{
const int SLOTS = 1024;
UINT g_step;
std::vector<int> g_free;
struct Later { IUnknown* object; int slot; UINT64 fence; };
std::vector<Later> g_later;
} // namespace

bool CreateSlots(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SLOTS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_srvHeap)))) return false;
    g_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int i = SLOTS - 1; i >= 0; i--) g_free.push_back(i);
    return true;
}

int AllocSlot()
{
    static bool told;
    if (g_free.empty())
    {
        if (!told) Log("overlay: all %d views of the host's heap are in use; further pictures are refused", SLOTS);
        told = true;
        return -1;
    }
    int slot = g_free.back();
    g_free.pop_back();
    return slot;
}

int SlotOf(D3D12_CPU_DESCRIPTOR_HANDLE cpu)
{
    return g_srvHeap != NULL && g_step != 0 ? (int)((cpu.ptr - g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / g_step) : -1;
}

D3D12_CPU_DESCRIPTOR_HANDLE SlotCpu(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE at = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    at.ptr += (SIZE_T)slot * g_step;
    return at;
}

D3D12_GPU_DESCRIPTOR_HANDLE SlotGpu(int slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE at = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    at.ptr += (UINT64)slot * g_step;
    return at;
}

void FreeSlotLater(int slot)
{
    if (slot >= 0 && slot < SLOTS) g_later.push_back({NULL, slot, g_fenceValue});
}

void ReleaseLater(IUnknown* object)
{
    if (object == NULL) return;
    if (g_device12 == NULL) object->Release(); /* D3D11 keeps what its context still uses */
    else g_later.push_back({object, -1, g_fenceValue});
}

void SweepLater()
{
    UINT64 done = CompletedFence();
    size_t kept = 0;
    for (const Later& l : g_later)
    {
        if (l.fence > done) { g_later[kept++] = l; continue; }
        if (l.object != NULL) l.object->Release();
        if (l.slot >= 0) g_free.push_back(l.slot);
    }
    g_later.resize(kept);
}
} // namespace overlay
