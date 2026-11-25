/* Pictures screens make (HfDraw.image_create): RGBA8 textures on the game's device. D3D11 uploads at once on the
 * immediate context. D3D12 copies the pixels into an upload buffer at once (the caller's memory is gone after the
 * call) and records the copy at the start of the frame's list, before ImGui draws; the buffer goes once the fence says
 * that frame is done. */
#include <string.h>
#include <vector>
#include "host_internal.h"

struct HfImage
{
    uint32_t width, height;
    ID3D11Texture2D* texture11;
    ID3D11ShaderResourceView* view11;
    ID3D12Resource* texture12;
    int slot;
    bool readable; /* D3D12: in PIXEL_SHADER_RESOURCE, not COPY_DEST */
};

namespace overlay
{
namespace
{
const uint32_t MAX_SIDE = 8192;

struct Upload
{
    HfImage* image;
    ID3D12Resource* buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
};
std::vector<Upload> g_uploads;

bool Create11(HfImage* image, const void* rgba)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = image->width;
    desc.Height = image->height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {rgba, image->width * 4, 0};
    return SUCCEEDED(g_device11->CreateTexture2D(&desc, &data, &image->texture11)) &&
        SUCCEEDED(g_device11->CreateShaderResourceView(image->texture11, NULL, &image->view11));
}

/* The pixels into a new upload buffer, laid out as the copy wants them (rows 256-byte aligned). */
bool Stage12(HfImage* image, const void* rgba)
{
    D3D12_RESOURCE_DESC desc = image->texture12->GetDesc();
    Upload upload = {image, NULL, {}};
    UINT64 bytes = 0;
    g_device12->GetCopyableFootprints(&desc, 0, 1, 0, &upload.footprint, NULL, NULL, &bytes);
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&upload.buffer))))
        return false;
    uint8_t* mapped = NULL;
    D3D12_RANGE none = {0, 0};
    if (FAILED(upload.buffer->Map(0, &none, (void**)&mapped))) { upload.buffer->Release(); return false; }
    for (uint32_t y = 0; y < image->height; y++)
        memcpy(mapped + upload.footprint.Offset + (UINT64)y * upload.footprint.Footprint.RowPitch, (const uint8_t*)rgba + (size_t)y * image->width * 4, image->width * 4);
    upload.buffer->Unmap(0, NULL);
    g_uploads.push_back(upload);
    return true;
}

bool Create12(HfImage* image, const void* rgba)
{
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = image->width;
    desc.Height = image->height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    image->slot = -1;
    if (FAILED(g_device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&image->texture12))))
        return false;
    image->slot = AllocSlot();
    if (image->slot < 0) return false;
    g_device12->CreateShaderResourceView(image->texture12, NULL, SlotCpu(image->slot));
    return Stage12(image, rgba);
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = 0;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier);
}

void Release(HfImage* image)
{
    for (size_t i = g_uploads.size(); i-- > 0;)
        if (g_uploads[i].image == image) { ReleaseLater(g_uploads[i].buffer); g_uploads.erase(g_uploads.begin() + (long)i); }
    if (image->view11 != NULL) image->view11->Release();
    if (image->texture11 != NULL) image->texture11->Release();
    ReleaseLater(image->texture12); /* a frame in flight may still draw it */
    FreeSlotLater(image->slot);
    delete image;
}
} // namespace

HfImage* ImageCreate(uint32_t width, uint32_t height, const void* rgba)
{
    if (rgba == NULL || width == 0 || height == 0 || width > MAX_SIDE || height > MAX_SIDE || !HaveDevice()) return NULL;
    HfImage* image = new HfImage{width, height, NULL, NULL, NULL, -1, false};
    bool made = g_device11 != NULL ? Create11(image, rgba) : Create12(image, rgba);
    if (made) return image;
    static bool told;
    if (!told) Log("overlay: a %ux%u picture could not be made (said once)", width, height);
    told = true;
    Release(image);
    return NULL;
}

void ImageUpdate(HfImage* image, const void* rgba)
{
    if (image == NULL || rgba == NULL) return;
    if (image->texture11 != NULL) g_context11->UpdateSubresource(image->texture11, 0, NULL, rgba, image->width * 4, 0);
    else if (image->texture12 != NULL && !Stage12(image, rgba)) Log("overlay: a picture's new pixels could not be staged");
}

void ImageFree(HfImage* image)
{
    if (image != NULL) Release(image);
}

ImTextureID ImageTexture(HfImage* image)
{
    if (image == NULL) return (ImTextureID)0;
    return image->view11 != NULL ? (ImTextureID)image->view11 : image->slot >= 0 ? (ImTextureID)SlotGpu(image->slot).ptr : (ImTextureID)0;
}

void FlushUploads12(ID3D12GraphicsCommandList* list)
{
    for (const Upload& u : g_uploads)
    {
        HfImage* image = u.image;
        if (image->readable) Barrier(list, image->texture12, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to = {};
        to.pResource = image->texture12;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION from = {};
        from.pResource = u.buffer;
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = u.footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, NULL);
        Barrier(list, image->texture12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        image->readable = true;
        ReleaseLater(u.buffer);
    }
    g_uploads.clear();
}
} // namespace overlay
