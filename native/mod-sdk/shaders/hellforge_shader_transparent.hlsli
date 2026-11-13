// The "transparent" shader host: a pixel shader on a mesh that adds light to the scene, on both of its walls.
// Write `Out main(In i)` and end it with hf_transparent(...). docs/modding/shaders.md.
#ifndef HELLFORGE_SHADER_TRANSPARENT_HLSLI
#define HELLFORGE_SHADER_TRANSPARENT_HLSLI

#include "hellforge_shader.hlsli"

cbuffer c_per_instance : register(b8)
{
    float4 hf_instance[40]; // 640 bytes. [23].x opacity_mult
};

Texture2D<float4> __tex_linear_depth : register(t20);

struct In
{
    float4 pos : SV_POSITION;
    float4 obj : TEXCOORD0;     // the point on the mesh in the game's axes (OBJ x, -z, y), before the unit's scale
    float4 t1 : TEXCOORD1;
    float3 t2 : TEXCOORD2;
    float3 t3 : TEXCOORD3;
    float3 nrm : TEXCOORD4;     // world normal
    float3 world : TEXCOORD5;   // world position
    float3 view : TEXCOORD6;    // from the point to the camera
    float2 uv : TEXCOORD7;      // as stored: use hf_uv
    uint face : SV_IsFrontFace;
};

// All three must be written: hf_transparent does.
struct Out
{
    float4 c0 : SV_TARGET0;     // hdr1: rgb * a is added to the scene
    float c1 : SV_TARGET1;      // the pixel's weight
    float4 c2 : SV_TARGET2;     // for depth of field and TAA
};

// The OBJ's own UV. HellForge stores v as 1 - v.
float2 hf_uv(In i) { return float2(i.uv.x, 1.0 - i.uv.y); }

// The way round the mesh's z axis, 0..1.
float hf_around(In i) { return atan2(i.obj.y, i.obj.x) * 0.1591549 + 0.5; }

// The material's opacity_mult: 1 unless something changes it.
float hf_opacity() { return saturate(hf_instance[23].x); }

// 1 facing the camera, 0 edge on. Lines on a mesh bunch up at its edges, so this is a useful fade.
float hf_facing(In i) { return abs(dot(normalize(i.nrm), normalize(i.view))); }

// The depth of what is behind the pixel, in metres along the camera's forward axis.
float hf_scene_depth(In i) { return __tex_linear_depth.Load(int3(uint2(i.pos.xy), 0)).x; }

// 0 where the mesh meets what is behind it, 1 at `metres` away from it.
float hf_soft(In i, float metres) { return saturate((hf_scene_depth(i) - hf_view_depth(i.world)) / metres); }

// The three outputs: a colour added to the scene by alpha. Colours are the scene's linear brightness and can go well above 1.
Out hf_transparent(In i, float3 colour, float alpha)
{
    Out o;
    o.c0 = float4(min(colour, 65504.0), alpha);
    o.c1 = alpha * hf_weight(hf_view_depth(i.world));
    o.c2 = float4(0.5, 0.0, 0.0, alpha);
    return o;
}

#endif
