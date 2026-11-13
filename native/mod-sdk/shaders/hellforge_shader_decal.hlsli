// The "decal" shader host: a pixel shader that draws on whatever is already in a box, ground included, as a projected decal.
// Write `Out main(In i)`, start it with hf_box(i), and end it with hf_decal(...). docs/modding/shaders.md.
#ifndef HELLFORGE_SHADER_DECAL_HLSLI
#define HELLFORGE_SHADER_DECAL_HLSLI

#include "hellforge_shader.hlsli"

cbuffer c_per_object : register(b1)
{
    float4 hf_object[22]; // 352 bytes. [0..3] cb_world, [4..6] cb_inv_world, [8..10] cb_bounding_volume: min, max, size
};

Texture2D<float4> __tex_linear_depth : register(t3);

struct In
{
    float4 pos : SV_POSITION;
    float4 ray : TEXCOORD0; // the view ray
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
};

// All five must be written: hf_decal does.
struct Out
{
    float4 c0 : SV_TARGET0; // gbuffer0: albedo, alpha blended
    float4 c1 : SV_TARGET1; // gbuffer1, left alone with alpha 0
    float4 c2 : SV_TARGET2; // gbuffer2, left alone
    float4 c3 : SV_TARGET3; // gbuffer3, left alone
    float4 c4 : SV_TARGET4; // gbuffer_emissive, added to what is there
};

// A pixel's place in the box.
struct HfBox
{
    float3 world;   // world position
    float3 local;   // in the unit's space, so without its scale
    float3 uvw;     // -1..1 across the box on each axis. With the unit's rotation left alone, x and y are the ground and z is up
    float radius;   // length(uvw.xy): 0 at the centre, 1 at the middle of the box's sides
};

// The world position of what the pixel lands on, rebuilt from the depth buffer, for both kinds of camera.
float3 hf_world_position(In i)
{
    float depth = __tex_linear_depth.Load(int3(uint2(i.pos.xy), 0)).x;
    if (hf_viewport[10].w != 0.0) return i.ray.xyz + hf_camera() + depth * hf_camera_forward();
    return i.ray.xyz / i.ray.w * depth + hf_camera();
}

// A world position in the unit's space.
float3 hf_box_local(float3 world)
{
    float4 p = float4(world, 1.0);
    return float3(dot(p, hf_object[4]), dot(p, hf_object[5]), dot(p, hf_object[6]));
}

// Whether a position from hf_box_local is inside the box.
bool hf_in_box(float3 l)
{
    return l.x > hf_object[8].x && l.x < hf_object[8].y && l.y > hf_object[9].x && l.y < hf_object[9].y && l.z > hf_object[10].x && l.z < hf_object[10].y;
}

// The pixel in the box, and no pixel at all outside it.
HfBox hf_box(In i)
{
    HfBox b;
    b.world = hf_world_position(i);
    b.local = hf_box_local(b.world);
    if (!hf_in_box(b.local)) discard;
    b.uvw = b.local / (float3(hf_object[8].z, hf_object[9].z, hf_object[10].z) * 0.5);
    b.radius = length(b.uvw.xy);
    return b;
}

// The unit's size in metres on each axis: the scale your mod gave it, as the box is 1 m across.
float3 hf_box_size() { return 1.0 / float3(length(hf_object[4].xyz), length(hf_object[5].xyz), length(hf_object[6].xyz)); }

// A shader gets no number from a mod, but a mod can change the unit's scale and the shader can read that back as a clock.
// Grow the box's height from `from` to `to` metres over the effect's life, and this gives 0..1.
float hf_progress(float from, float to) { return saturate((hf_box_size().z - from) / (to - from)); }

// A brightness as gbuffer_emissive stores it: (c / 128) ^ 0.8. 128 is the most it holds.
float3 hf_emissive(float3 c) { return pow(saturate(c * 0.0078125), 0.8); }

// The five outputs: an albedo over what is there by alpha, plus a glow (hf_emissive is applied here). Normals and the like stay.
Out hf_decal(float3 albedo, float alpha, float3 glow)
{
    Out o;
    o.c0 = float4(albedo, alpha);
    o.c1 = float4(0.5, 0.5, 0.5, 0.0);
    o.c2 = float4(0.5, 0.5, 0.0, 0.0);
    o.c3 = float4(0.0, 0.0, 0.0, 0.0);
    o.c4 = float4(hf_emissive(glow), 1.0);
    return o;
}

#endif
