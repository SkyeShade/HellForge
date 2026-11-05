// The "refract" shader host: a pixel shader on a mesh that can read the finished scene and redraw it, bent or tinted.
// It replaces what is behind the mesh, and draws the mesh's near wall only. Write `Out main(In i)` and end it with
// hf_refract(...). docs/modding/shaders.md.
#ifndef HELLFORGE_SHADER_REFRACT_HLSLI
#define HELLFORGE_SHADER_REFRACT_HLSLI

#include "hellforge_shader.hlsli"

cbuffer c_per_instance : register(b9)
{
    float4 hf_instance[8]; // 128 bytes. [5].z op_mult
};

Texture2D<float4> __tex_hdr0 : register(t19);
Texture2D<float4> __tex_linear_depth : register(t20);
SamplerState __samp_clamp_linear : register(s2);
SamplerState __samp_clamp_point : register(s3);

struct In
{
    float4 pos : SV_POSITION;
    float3 t0 : TEXCOORD0;      // these three rows' z make the world normal: use hf_normal
    float3 t1 : TEXCOORD1;
    float3 t2 : TEXCOORD2;
    float3 world : TEXCOORD3;   // world position
    float4 t4 : TEXCOORD4;
    float2 uv : TEXCOORD5;      // as stored: use hf_uv
};

// All three must be written: hf_refract does.
struct Out
{
    float4 c0 : SV_TARGET0;     // hdr1, not blended: this colour replaces what is behind
    float c1 : SV_TARGET1;      // the pixel's weight
    float4 c2 : SV_TARGET2;     // for depth of field and TAA
};

// The OBJ's own UV. HellForge stores v as 1 - v.
float2 hf_uv(In i) { return float2(i.uv.x, 1.0 - i.uv.y); }

// The world normal.
float3 hf_normal(In i) { return normalize(float3(i.t0.z, i.t1.z, i.t2.z)); }

// The pixel's place on the screen, 0..1.
float2 hf_screen(In i) { return i.pos.xy / hf_target_size(); }

// The material's op_mult as a strength: 1 with op_mult at its default of 0, and 0 with op_mult at 1.
float hf_strength() { return 1.0 - saturate(hf_instance[5].z); }

// The scene behind the mesh at a place on the screen.
float3 hf_scene(float2 screen) { return __tex_hdr0.Sample(__samp_clamp_linear, screen).rgb; }

// The depth at a place on the screen, in metres along the camera's forward axis.
float hf_scene_depth(float2 screen) { return __tex_linear_depth.Sample(__samp_clamp_point, screen).x; }

// `screen` moved by `offset`, unless that lands on something in front of the mesh, which must not smear into it.
float2 hf_bend(In i, float2 screen, float2 offset)
{
    return hf_scene_depth(screen + offset) < hf_view_depth(i.world) ? screen : screen + offset;
}

// The three outputs: the pixel's whole colour, in the scene's linear brightness.
Out hf_refract(In i, float3 colour)
{
    Out o;
    o.c0 = float4(min(max(colour, 0.0), 65504.0), 1.0);
    o.c1 = hf_weight(hf_view_depth(i.world));
    o.c2 = float4(0.5, 0.0, 0.0, 1.0);
    return o;
}

#endif
