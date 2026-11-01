/* The editor's images (icons.h). The atlas is RGBA: a mask is white with its coverage in alpha, HellForge's logo is in
 * colour. It has mip levels (a 72-pixel silhouette is drawn a fifth of that in a list), averaged by alpha, and each image
 * is padded so the levels do not bleed into each other. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <vector>
#include "icons.h"
#include "json.h"
#include "ui_logo.h"
#include "ui_marks.h"

namespace icons
{
namespace
{
const int WIDTH = 2048, PAD = 8, LEVELS = 4;

struct Source { Kind kind; std::string name; int w, h; std::vector<uint32_t> rgba; };

std::vector<uint32_t> White(const uint8_t* coverage, size_t n)
{
    std::vector<uint32_t> out(n);
    for (size_t i = 0; i < n; i++) out[i] = 0x00FFFFFFu | ((uint32_t)coverage[i] << 24);
    return out;
}
std::vector<Source> g_sources;
std::map<std::string, Icon> g_icons;
Icon g_marks[UI_MARK_COUNT], g_logo;
int g_height;
std::vector<uint32_t> g_pixels; /* level 0 */
ID3D12Resource* g_texture;
ID3D11Texture2D* g_texture11;
ImTextureID g_view;
bool Uploaded() { return g_texture != nullptr || g_texture11 != nullptr; }

std::string Key(Kind kind, const std::string& name) { return std::to_string((int)kind) + name; }

void Layout()
{
    /* shelves, tallest first */
    std::vector<int> order(g_sources.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
    std::stable_sort(order.begin(), order.end(), [](int a, int b) { return g_sources[a].h > g_sources[b].h; });
    std::vector<std::pair<int, int>> at(g_sources.size());
    int x = PAD, y = PAD, shelf = 0;
    for (int i : order)
    {
        const Source& s = g_sources[i];
        if (x + s.w + PAD > WIDTH) { x = PAD; y += shelf + 2 * PAD; shelf = 0; }
        at[i] = {x, y};
        x += s.w + 2 * PAD;
        if (s.h > shelf) shelf = s.h;
    }
    g_height = (y + shelf + PAD + 7) & ~7;
    g_pixels.assign((size_t)WIDTH * g_height, 0);
    for (size_t i = 0; i < g_sources.size(); i++)
    {
        const Source& s = g_sources[i];
        for (int row = 0; row < s.h; row++) memcpy(&g_pixels[(size_t)(at[i].second + row) * WIDTH + at[i].first], &s.rgba[(size_t)row * s.w], (size_t)s.w * 4);
        Icon icon{ImVec2((float)at[i].first / WIDTH, (float)at[i].second / g_height),
            ImVec2((float)(at[i].first + s.w) / WIDTH, (float)(at[i].second + s.h) / g_height), s.w, s.h};
        if (s.kind == MARK && s.name == "logo") g_logo = icon;
        else if (s.kind == MARK) g_marks[atoi(s.name.c_str())] = icon;
        else g_icons[Key(s.kind, s.name)] = icon;
    }
}

/* An image of game-icons.bin at `at` (moved past it) into the sources; false where the file is cut short or holds what
 * this version does not read. */
bool ReadIcon(const std::string& data, uint32_t version, size_t& at)
{
    const uint8_t* p = (const uint8_t*)data.data();
    uint8_t kind = p[at], length = p[at + 1];
    at += 2;
    if (at + length + 4 > data.size()) return false;
    std::string name((const char*)p + at, length);
    at += length;
    uint16_t w, h;
    memcpy(&w, p + at, 2);
    memcpy(&h, p + at + 2, 2);
    at += 4;
    uint8_t channels = 1;
    if (version >= 2) { if (at >= data.size()) return false; channels = p[at++]; }
    size_t bytes = (size_t)w * h * channels;
    if (w == 0 || h == 0 || w > 1024 || h > 1024 || (channels != 1 && channels != 4) || at + bytes > data.size() || (kind > PASSIVE && kind != FACTION && kind != DIFFICULTY)) return false;
    std::vector<uint32_t> pixels;
    if (channels == 1) pixels = White(p + at, (size_t)w * h);
    else { pixels.resize((size_t)w * h); memcpy(pixels.data(), p + at, bytes); }
    g_sources.push_back({(Kind)kind, name, w, h, pixels});
    at += bytes;
    return true;
}
} // namespace

int Load(const std::wstring& gameIcons)
{
    g_sources.clear();
    g_icons.clear();
    for (int m = 0; m < UI_MARK_COUNT; m++)
        g_sources.push_back({MARK, std::to_string(m), UI_MARK_SIZE, UI_MARK_SIZE, White(kUiMarks[m], UI_MARK_SIZE * UI_MARK_SIZE)});
    std::vector<uint32_t> logo(UI_LOGO_SIZE * UI_LOGO_SIZE);
    memcpy(logo.data(), kUiLogo, sizeof kUiLogo); /* R, G, B, A bytes: little-endian RGBA words */
    g_sources.push_back({MARK, "logo", UI_LOGO_SIZE, UI_LOGO_SIZE, logo});
    std::string data;
    int count = 0;
    if (hfjson::ReadFile(gameIcons, &data) && data.size() >= 12 && !memcmp(data.data(), "HFIC", 4))
    {
        const uint8_t* p = (const uint8_t*)data.data();
        uint32_t version, n;
        memcpy(&version, p + 4, 4);
        memcpy(&n, p + 8, 4);
        size_t at = 12;
        for (uint32_t i = 0; (version == 1 || version == 2) && i < n && at + 2 <= data.size(); i++)
        {
            if (!ReadIcon(data, version, at)) break;
            count++;
        }
    }
    Layout();
    return count;
}

/* The image halved (each pixel the average of the four it covers; an odd last row or column taken twice). */
static void Halve(std::vector<uint32_t>& pixels, UINT& w, UINT& h)
{
    UINT nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<uint32_t> next((size_t)nw * nh);
    for (UINT y = 0; y < nh; y++)
        for (UINT x = 0; x < nw; x++)
        {
            uint32_t sum[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; k++)
            {
                uint32_t c = pixels[(size_t)std::min(h - 1, 2 * y + k / 2) * w + std::min(w - 1, 2 * x + k % 2)];
                for (int b = 0; b < 4; b++) sum[b] += (c >> (8 * b)) & 0xFF;
            }
            next[(size_t)y * nw + x] = (sum[0] / 4) | ((sum[1] / 4) << 8) | ((sum[2] / 4) << 16) | ((sum[3] / 4) << 24);
        }
    pixels.swap(next);
    w = nw;
    h = nh;
}

/* An image file in RGBA words, at most 128 pixels (halved, box filtered), read with Windows' image decoder. */
static bool Decode(const std::wstring& path, std::vector<uint32_t>& pixels, UINT& w, UINT& h)
{
    static const GUID factoryClass = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    static const GUID factoryInterface = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
    static const GUID rgba = {0xf5c7ad2d, 0x6a8d, 0x43dd, {0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9}}; /* 32bppRGBA */
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    w = h = 0;
    bool ok = SUCCEEDED(CoCreateInstance(factoryClass, nullptr, CLSCTX_INPROC_SERVER, factoryInterface, (void**)&factory)) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 4096 && h <= 4096 &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, rgba, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
    if (ok)
    {
        pixels.resize((size_t)w * h);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, (UINT)(pixels.size() * 4), (BYTE*)pixels.data()));
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    if (!ok) return false;
    /* halved (box filter) while over 128 pixels */
    while (w > 128 || h > 128) Halve(pixels, w, h);
    return true;
}

bool AddImage(Kind kind, const std::string& name, const std::wstring& path)
{
    if (Uploaded() || g_icons.count(Key(kind, name))) return g_icons.count(Key(kind, name)) > 0;
    std::vector<uint32_t> pixels;
    UINT w = 0, h = 0;
    if (!Decode(path, pixels, w, h)) return false;
    g_sources.push_back({kind, name, (int)w, (int)h, pixels});
    Layout();
    return true;
}

bool AddStratagem(const std::string& name, const std::wstring& path)
{
    if (Uploaded() || g_icons.count(Key(STRATAGEM, name))) return g_icons.count(Key(STRATAGEM, name)) > 0;
    std::vector<uint32_t> pixels;
    UINT w = 0, h = 0;
    if (!Decode(path, pixels, w, h)) return false;
    std::vector<uint8_t> layer(pixels.size()), white(pixels.size());
    for (size_t i = 0; i < pixels.size(); i++)
    {
        uint32_t c = pixels[i], a = c >> 24;
        layer[i] = (uint8_t)((c & 0xFF) * a / 255);
        white[i] = (uint8_t)(((c >> 8) & 0xFF) * a / 255);
    }
    g_sources.push_back({STRATAGEM, name, (int)w, (int)h, White(layer.data(), layer.size())});
    g_sources.push_back({STRATAGEM_WHITE, name, (int)w, (int)h, White(white.data(), white.size())});
    Layout();
    return true;
}

const Icon* Find(Kind kind, const std::string& name)
{
    auto found = g_icons.find(Key(kind, name));
    return found != g_icons.end() ? &found->second : nullptr;
}

const Icon* Mark(int mark) { return mark >= 0 && mark < UI_MARK_COUNT && g_height > 0 ? &g_marks[mark] : nullptr; }
const Icon* Logo() { return g_height > 0 ? &g_logo : nullptr; }

ImTextureID Texture() { return g_view; }

/* The levels, each half the last: alpha box filtered, colour averaged by alpha (no dark fringes). */
std::vector<std::vector<uint32_t>> MipLevels()
{
    std::vector<std::vector<uint32_t>> levels;
    levels.push_back(g_pixels);
    int w = WIDTH, h = g_height;
    for (int level = 1; level < LEVELS; level++)
    {
        const std::vector<uint32_t>& last = levels.back();
        int nw = w / 2, nh = h / 2;
        std::vector<uint32_t> next((size_t)nw * nh);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
            {
                uint32_t sum[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; k++)
                {
                    uint32_t c = last[(size_t)(2 * y + k / 2) * w + 2 * x + k % 2], a = c >> 24;
                    sum[0] += (c & 0xFF) * a; sum[1] += ((c >> 8) & 0xFF) * a; sum[2] += ((c >> 16) & 0xFF) * a; sum[3] += a;
                }
                uint32_t a = (sum[3] + 2) / 4;
                next[(size_t)y * nw + x] = sum[3] == 0 ? 0 : (sum[0] / sum[3]) | ((sum[1] / sum[3]) << 8) | ((sum[2] / sum[3]) << 16) | (a << 24);
            }
        levels.push_back(std::move(next));
        w = nw; h = nh;
    }
    return levels;
}

/* The upload buffer's levels copied into the texture on the game's queue, and waited for (5 s at most). */
static bool CopyLevels(ID3D12Device* device, ID3D12CommandQueue* queue, ExecuteFn execute, ID3D12Resource* texture, ID3D12Resource* upload,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* footprints)
{
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    bool ok = SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    if (ok)
    {
        for (int level = 0; level < LEVELS; level++)
        {
            D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
            to.pResource = texture;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = (UINT)level;
            from.pResource = upload;
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint = footprints[level];
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        execute(queue, 1, lists);
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ok = SUCCEEDED(queue->Signal(fence, 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, done)) && WaitForSingleObject(done, 5000) == WAIT_OBJECT_0;
        CloseHandle(done);
    }
    if (fence) fence->Release();
    if (list) list->Release();
    if (allocator) allocator->Release();
    return ok;
}

bool Upload(ID3D12Device* device, ID3D12CommandQueue* queue, ExecuteFn execute, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
    if (Uploaded()) return true;
    if (g_height == 0) return false;
    std::vector<std::vector<uint32_t>> levels = MipLevels();
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = WIDTH;
    desc.Height = (UINT)g_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = LEVELS;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ID3D12Resource* texture = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)))) return false;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[LEVELS];
    UINT rows[LEVELS];
    UINT64 rowBytes[LEVELS], total = 0;
    device->GetCopyableFootprints(&desc, 0, LEVELS, 0, footprints, rows, rowBytes, &total);
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upload = nullptr;
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
    { texture->Release(); return false; }
    uint8_t* mapped = nullptr;
    D3D12_RANGE none = {0, 0};
    if (FAILED(upload->Map(0, &none, (void**)&mapped))) { upload->Release(); texture->Release(); return false; }
    for (int level = 0; level < LEVELS; level++)
        for (UINT row = 0; row < rows[level]; row++)
            memcpy(mapped + footprints[level].Offset + (size_t)row * footprints[level].Footprint.RowPitch,
                &levels[level][(size_t)row * footprints[level].Footprint.Width], (size_t)rowBytes[level]);
    upload->Unmap(0, nullptr);

    bool ok = CopyLevels(device, queue, execute, texture, upload, footprints);
    upload->Release();
    if (!ok) { texture->Release(); return false; }
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = LEVELS;
    device->CreateShaderResourceView(texture, &view, cpu);
    g_texture = texture;
    g_view = (ImTextureID)gpu.ptr;
    g_pixels.clear();
    g_pixels.shrink_to_fit();
    return true;
}

bool Upload11(ID3D11Device* device)
{
    if (Uploaded()) return true;
    if (g_height == 0) return false;
    std::vector<std::vector<uint32_t>> levels = MipLevels();
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = WIDTH;
    desc.Height = (UINT)g_height;
    desc.MipLevels = LEVELS;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data[LEVELS] = {};
    for (int level = 0, w = WIDTH; level < LEVELS; level++, w /= 2) data[level] = {levels[level].data(), (UINT)w * 4, 0};
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, data, &texture))) return false;
    if (FAILED(device->CreateShaderResourceView(texture, nullptr, &view))) { texture->Release(); return false; }
    g_texture11 = texture;
    g_view = (ImTextureID)(intptr_t)view;
    g_pixels.clear();
    g_pixels.shrink_to_fit();
    return true;
}
} // namespace icons
