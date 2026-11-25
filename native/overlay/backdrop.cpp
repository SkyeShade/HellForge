/* HfDraw.backdrop: the game's frame as it was when a screen opened, dimmed, desaturated and blurred. The back buffer
 * is copied once, before the host draws into it, into a texture of the same format with a mip chain; the pixel shader
 * blurs by sampling a 3x3 tent at the mip that matches the radius, so the radius can change every frame for free. It
 * decodes the back buffer's encoding (sRGB, PQ or scRGB's linear), dims and desaturates in linear light, and encodes
 * again. Where any of that cannot be made, a black rectangle dims the live frame instead. */
#include <d3dcompiler.h>
#include <math.h>
#include <string.h>
#include "host_internal.h"

namespace overlay
{
namespace
{
const char* const kShaders =
    "struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2); o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "cbuffer c : register(b0) { float keep; float grey; float spread; float lod; float decode; float encode; float2 texel; };\n"
    "Texture2D t : register(t0);\nSamplerState s : register(s0);\n"
    "float3 ToLinear(float3 c, float mode) {\n"
    "  if (mode > 1.5) return c;\n"
    "  if (mode > 0.5) { float3 p = pow(saturate(c), 1.0 / 78.84375); return pow(max(p - 0.8359375, 0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125); }\n"
    "  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }\n"
    "float3 FromLinear(float3 l, float mode) {\n"
    "  if (mode > 1.5) return l;\n"
    "  if (mode > 0.5) { float3 m = pow(max(l, 0), 0.1593017578125); return pow((0.8359375 + 18.8515625 * m) / (1 + 18.6875 * m), 78.84375); }\n"
    "  l = saturate(l); return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055; }\n"
    "float4 down(V i) : SV_Target { return t.SampleLevel(s, i.uv, 0); }\n"
    "float4 ps(V i) : SV_Target {\n"
    "  float3 sum = 0;\n"
    "  [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++)\n"
    "    sum += (2 - abs(x)) * (2 - abs(y)) * t.SampleLevel(s, i.uv + float2(x, y) * texel * spread, lod).rgb;\n"
    "  float3 l = ToLinear(sum / 16, decode);\n"
    "  float3 w = decode > 0.5 && decode < 1.5 ? float3(0.2627, 0.6780, 0.0593) : float3(0.2126, 0.7152, 0.0722);\n"
    "  float g = dot(l, w);\n"
    "  l = lerp(l, float3(g, g, g), grey) * keep;\n"
    "  return float4(FromLinear(l, encode), 1); }\n";

const UINT MAX_MIPS = 9;

struct Constants { float keep, grey, spread, lod, decode, encode, texelX, texelY; };

/* What the copy is now: the back buffer it was made for, its encoding, and whether one is to be made this frame. */
struct Copy
{
    UINT width, height, mips;
    DXGI_FORMAT format;
    float decode;
    bool pending, made;
};
Copy g_copy;
int g_path = -1; /* 1 the frame is copied, 0 the dimmed rectangle (said once each) */
DXGI_FORMAT g_madeFor = DXGI_FORMAT_UNKNOWN; /* the format the D3D12 pipelines were made for */

/* D3D11 */
ID3D11VertexShader* g_vs11;
ID3D11PixelShader* g_ps11;
ID3D11SamplerState* g_sampler11;
ID3D11Buffer* g_constants11;
ID3D11Texture2D* g_texture11;
ID3D11ShaderResourceView* g_view11;
/* D3D12 */
ID3D12RootSignature* g_root;
ID3D12PipelineState *g_blurState, *g_downState;
ID3D12Resource* g_texture12;
ID3D12DescriptorHeap* g_mipTargets;
int g_fullSlot = -1, g_mipSlots[MAX_MIPS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
ID3DBlob *g_vsCode, *g_psCode, *g_downCode;

bool Compile(const char* entry, const char* target, ID3DBlob** out)
{
    if (*out != NULL) return true;
    ID3DBlob* errors = NULL;
    HRESULT made = D3DCompile(kShaders, strlen(kShaders), "hellforge-backdrop", NULL, NULL, entry, target, 0, 0, out, &errors);
    if (FAILED(made) && errors != NULL) Log("overlay: backdrop shader %s: %.300s", entry, (const char*)errors->GetBufferPointer());
    if (errors != NULL) errors->Release();
    return SUCCEEDED(made);
}

bool CompileAll() { return Compile("vs", "vs_5_0", &g_vsCode) && Compile("ps", "ps_5_0", &g_psCode) && Compile("down", "ps_5_0", &g_downCode); }

/* The encoding of a format as the shader names it: 0 sRGB, 1 PQ, 2 linear; -1 one the host does not copy. */
float Encoding(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: return 0;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return g_hdrMode == HDR_PQ ? 1.0f : 0.0f;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 2;
    default: return -1;
    }
}

UINT MipsFor(UINT width, UINT height)
{
    UINT mips = 1;
    while (mips < MAX_MIPS && (width >> mips) > 0 && (height >> mips) > 0) mips++;
    return mips;
}

void Say(int path, const char* why)
{
    if (g_path == path) return;
    g_path = path;
    if (path == 1) Log("overlay: backdrop: the game's frame is copied when a screen opens (Direct3D %s, format %d, %s), blurred over %u mips",
                       g_device11 != NULL ? "11" : "12", (int)g_copy.format, g_copy.decode > 1.5f ? "scRGB" : g_copy.decode > 0.5f ? "PQ" : "sRGB", g_copy.mips);
    else Log("overlay: backdrop: a dimmed rectangle over the live frame (%s)", why);
}

/* ---- D3D11 ---- */

bool Pipeline11()
{
    if (g_ps11 != NULL) return true;
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC constants = {sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    return CompileAll() &&
        SUCCEEDED(g_device11->CreateVertexShader(g_vsCode->GetBufferPointer(), g_vsCode->GetBufferSize(), NULL, &g_vs11)) &&
        SUCCEEDED(g_device11->CreateSamplerState(&sampler, &g_sampler11)) && SUCCEEDED(g_device11->CreateBuffer(&constants, NULL, &g_constants11)) &&
        SUCCEEDED(g_device11->CreatePixelShader(g_psCode->GetBufferPointer(), g_psCode->GetBufferSize(), NULL, &g_ps11));
}

bool Texture11()
{
    if (g_view11 != NULL) { g_view11->Release(); g_view11 = NULL; }
    if (g_texture11 != NULL) { g_texture11->Release(); g_texture11 = NULL; }
    UINT support = 0;
    g_device11->CheckFormatSupport(g_copy.format, &support);
    if (!(support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)) g_copy.mips = 1;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = g_copy.width;
    desc.Height = g_copy.height;
    desc.MipLevels = g_copy.mips;
    desc.ArraySize = 1;
    desc.Format = g_copy.format;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = g_copy.mips > 1 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    return SUCCEEDED(g_device11->CreateTexture2D(&desc, NULL, &g_texture11)) && SUCCEEDED(g_device11->CreateShaderResourceView(g_texture11, NULL, &g_view11));
}

void Draw11(const ImDrawCmd* command, const Constants& c)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_context11->Map(g_constants11, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, &c, sizeof c);
    g_context11->Unmap(g_constants11, 0);
    ID3D11Buffer* gameConstants = NULL; /* ImGui keeps the game's state but this slot */
    g_context11->PSGetConstantBuffers(0, 1, &gameConstants);
    D3D11_RECT clip = {(LONG)command->ClipRect.x, (LONG)command->ClipRect.y, (LONG)command->ClipRect.z, (LONG)command->ClipRect.w};
    g_context11->RSSetScissorRects(1, &clip);
    g_context11->IASetInputLayout(NULL);
    g_context11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context11->VSSetShader(g_vs11, NULL, 0);
    g_context11->PSSetShader(g_ps11, NULL, 0);
    g_context11->PSSetShaderResources(0, 1, &g_view11);
    g_context11->PSSetSamplers(0, 1, &g_sampler11);
    g_context11->PSSetConstantBuffers(0, 1, &g_constants11);
    g_context11->OMSetBlendState(NULL, NULL, 0xFFFFFFFF);
    g_context11->Draw(3, 0);
    g_context11->PSSetConstantBuffers(0, 1, &gameConstants);
    if (gameConstants != NULL) gameConstants->Release();
}

/* ---- D3D12 ---- */

bool Root12()
{
    if (g_root != NULL) return true;
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = sizeof(Constants) / 4;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &range};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc = {2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *blob = NULL, *errors = NULL;
    bool made = SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) &&
        SUCCEEDED(g_device12->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_root)));
    if (blob != NULL) blob->Release();
    if (errors != NULL) errors->Release();
    return made;
}

ID3D12PipelineState* State12(ID3DBlob* pixel, DXGI_FORMAT format)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = g_root;
    desc.VS = {g_vsCode->GetBufferPointer(), g_vsCode->GetBufferSize()};
    desc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;
    ID3D12PipelineState* state = NULL;
    return SUCCEEDED(g_device12->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state))) ? state : NULL;
}

bool Pipeline12()
{
    if (g_blurState != NULL && g_madeFor == g_copy.format) return true;
    if (!CompileAll() || !Root12()) return false;
    ReleaseLater(g_blurState);
    ReleaseLater(g_downState);
    g_blurState = State12(g_psCode, g_copy.format);
    g_downState = State12(g_downCode, g_copy.format);
    g_madeFor = g_copy.format;
    return g_blurState != NULL && g_downState != NULL;
}

void FreeTexture12()
{
    ReleaseLater(g_texture12);
    g_texture12 = NULL;
    FreeSlotLater(g_fullSlot);
    g_fullSlot = -1;
    for (int& slot : g_mipSlots)
    {
        FreeSlotLater(slot);
        slot = -1;
    }
}

/* Views of the copy: one of every mip for the backdrop, and one of each mip alone for the next mip's downsample. */
bool Views12()
{
    if (g_mipTargets == NULL)
    {
        D3D12_DESCRIPTOR_HEAP_DESC targets = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, MAX_MIPS, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(g_device12->CreateDescriptorHeap(&targets, IID_PPV_ARGS(&g_mipTargets)))) return false;
    }
    UINT step = g_device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = g_copy.format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = g_copy.mips;
    if ((g_fullSlot = AllocSlot()) < 0) return false;
    g_device12->CreateShaderResourceView(g_texture12, &view, SlotCpu(g_fullSlot));
    for (UINT mip = 0; mip < g_copy.mips; mip++)
    {
        D3D12_RENDER_TARGET_VIEW_DESC target = {g_copy.format, D3D12_RTV_DIMENSION_TEXTURE2D};
        target.Texture2D.MipSlice = mip;
        D3D12_CPU_DESCRIPTOR_HANDLE at = g_mipTargets->GetCPUDescriptorHandleForHeapStart();
        at.ptr += (SIZE_T)mip * step;
        g_device12->CreateRenderTargetView(g_texture12, &target, at);
        if (mip + 1 == g_copy.mips) break;
        if ((g_mipSlots[mip] = AllocSlot()) < 0) return false;
        view.Texture2D.MostDetailedMip = mip;
        view.Texture2D.MipLevels = 1;
        g_device12->CreateShaderResourceView(g_texture12, &view, SlotCpu(g_mipSlots[mip]));
    }
    return true;
}

bool Texture12()
{
    FreeTexture12();
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = g_copy.width;
    desc.Height = g_copy.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = (UINT16)g_copy.mips;
    desc.Format = g_copy.format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return SUCCEEDED(g_device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
               IID_PPV_ARGS(&g_texture12))) && Views12();
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, sub, from, to};
    list->ResourceBarrier(1, &barrier);
}

/* Each mip drawn from the one above it (bilinear at its texel centres: a 2x2 box). */
void Downsample12(ID3D12GraphicsCommandList* list)
{
    UINT step = g_device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    list->SetDescriptorHeaps(1, &g_srvHeap);
    list->SetGraphicsRootSignature(g_root);
    list->SetPipelineState(g_downState);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Constants none = {};
    list->SetGraphicsRoot32BitConstants(0, sizeof none / 4, &none, 0);
    for (UINT mip = 1; mip < g_copy.mips; mip++)
    {
        UINT w = g_copy.width >> mip, h = g_copy.height >> mip;
        D3D12_VIEWPORT viewport = {0, 0, (float)w, (float)h, 0, 1};
        D3D12_RECT all = {0, 0, (LONG)w, (LONG)h};
        D3D12_CPU_DESCRIPTOR_HANDLE target = g_mipTargets->GetCPUDescriptorHandleForHeapStart();
        target.ptr += (SIZE_T)mip * step;
        Barrier(list, g_texture12, mip, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &all);
        list->OMSetRenderTargets(1, &target, FALSE, NULL);
        list->SetGraphicsRootDescriptorTable(1, SlotGpu(g_mipSlots[mip - 1]));
        list->DrawInstanced(3, 1, 0, 0);
        Barrier(list, g_texture12, mip, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

void Draw12(const ImDrawCmd* command, const Constants& c)
{
    ID3D12GraphicsCommandList* list = g_frameList;
    D3D12_RECT clip = {(LONG)command->ClipRect.x, (LONG)command->ClipRect.y, (LONG)command->ClipRect.z, (LONG)command->ClipRect.w};
    list->SetGraphicsRootSignature(g_root);
    list->SetPipelineState(g_blurState);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetGraphicsRoot32BitConstants(0, sizeof c / 4, &c, 0);
    list->SetGraphicsRootDescriptorTable(1, SlotGpu(g_fullSlot));
    list->RSSetScissorRects(1, &clip);
    list->DrawInstanced(3, 1, 0, 0);
}

void Callback(const ImDrawList*, const ImDrawCmd* command)
{
    const Constants& c = *(const Constants*)command->UserCallbackData;
    if (!g_copy.made) return;
    if (g_device11 != NULL) Draw11(command, c);
    else if (g_frameList != NULL) Draw12(command, c);
}

/* Everything the copy needs for a back buffer of this size and format, made now; NULL ready, else why not. */
const char* Prepare(UINT width, UINT height, DXGI_FORMAT format)
{
    float decode = Encoding(format);
    if (decode < 0) return "the back buffer's format is not one the host copies";
    if (g_copy.width == width && g_copy.height == height && g_copy.format == format && g_copy.decode == decode && (g_view11 != NULL || g_texture12 != NULL))
        return NULL;
    g_copy = {width, height, MipsFor(width, height), format, decode, false, false};
    if (g_device11 != NULL) return !Pipeline11() ? "the backdrop's shaders could not be made" : !Texture11() ? "the copy's texture could not be made" : NULL;
    return !Pipeline12() ? "the backdrop's shaders could not be made" : !Texture12() ? "the copy's texture could not be made" : NULL;
}
} // namespace

void Backdrop(ImDrawList* list, float dim, float desaturate, float blurPixels, bool* taken)
{
    const char* why = g_path == 0 ? "" : Prepare(g_frameWidth, g_frameHeight, BackBufferFormat());
    if (why != NULL)
    {
        Say(0, why);
        list->AddRectFilled(ImVec2(0, 0), ImVec2((float)g_frameWidth, (float)g_frameHeight), IM_COL32(0, 0, 0, (int)(dim * 255 + 0.5f)));
        return;
    }
    Say(1, NULL);
    if (!*taken || !g_copy.made) g_copy.pending = true;
    *taken = true;
    float spread = blurPixels / 2;
    Constants c = {powf(1 - dim, 2.2f), desaturate, spread, spread > 1 ? log2f(spread) : 0, g_copy.decode, Encoding(BackBufferFormat()),
                   1.0f / (float)g_copy.width, 1.0f / (float)g_copy.height};
    list->AddCallback(Callback, &c, sizeof c);
    list->AddCallback(ImDrawCallback_ResetRenderState, NULL);
}

void CaptureBackdrop11(ID3D11Texture2D* backBuffer)
{
    if (!g_copy.pending || g_texture11 == NULL) return;
    g_context11->CopySubresourceRegion(g_texture11, 0, 0, 0, 0, backBuffer, 0, NULL);
    if (g_copy.mips > 1) g_context11->GenerateMips(g_view11);
    g_copy.pending = false;
    g_copy.made = true;
}

void CaptureBackdrop12(ID3D12GraphicsCommandList* list, ID3D12Resource* backBuffer)
{
    if (!g_copy.pending || g_texture12 == NULL) return;
    Barrier(list, backBuffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, g_texture12, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to = {g_texture12, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION from = {backBuffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    to.SubresourceIndex = from.SubresourceIndex = 0;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, NULL);
    Barrier(list, g_texture12, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(list, backBuffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    Downsample12(list);
    g_copy.pending = false;
    g_copy.made = true;
}

void BackdropFormatChanged()
{
    g_copy.made = false; /* the next backdrop copies the frame again, in the new encoding */
}
} // namespace overlay
