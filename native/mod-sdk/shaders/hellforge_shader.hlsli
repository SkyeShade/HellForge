// What every shader host shares. Include a host's header (hellforge_shader_decal.hlsli, _transparent.hlsli, _refract.hlsli),
// which includes this one. Registers and sizes are the game's: docs/modding/shaders.md.
#ifndef HELLFORGE_SHADER_HLSLI
#define HELLFORGE_SHADER_HLSLI

cbuffer global_viewport : register(b0)
{
    float4 hf_viewport[51]; // 816 bytes, as raw rows so that the offsets are plain
};

// Seconds of game time. It runs on, so wrap what you animate: frac(hf_time() * rate) on a whole number of repeats.
float hf_time() { return hf_viewport[43].w; }

// The frame's delta time, in seconds.
float hf_delta_time() { return hf_viewport[44].x; }

// The camera's world position.
float3 hf_camera() { return hf_viewport[2].xyz; }

// The camera's axes in world space: the rows of camera_inv_view.
float3 hf_camera_right() { return float3(hf_viewport[11].x, hf_viewport[12].x, hf_viewport[13].x); }
float3 hf_camera_forward() { return float3(hf_viewport[11].y, hf_viewport[12].y, hf_viewport[13].y); }
float3 hf_camera_up() { return float3(hf_viewport[11].z, hf_viewport[12].z, hf_viewport[13].z); }

// The render target's size in pixels. pos.xy / hf_target_size() is a pixel's place on the screen, 0..1.
float2 hf_target_size() { return hf_viewport[44].zw; }

// A world position's distance along the camera's forward axis, in metres: what the linear depth texture holds.
float hf_view_depth(float3 world) { return dot(world - hf_camera(), hf_camera_forward()); }

// The weight of a transparent pixel at a view depth, for SV_TARGET1 in the mesh hosts, multiplied by the pixel's alpha.
float hf_weight(float depth)
{
    return 0.1 * exp(-0.004328 * depth) + pow(exp(-0.043281 * depth), 2.0)
        + 10.0 * pow(exp(-0.14427 * depth), 2.0) + 20.0 * pow(exp(-0.432809 * depth), 2.0);
}

#endif
