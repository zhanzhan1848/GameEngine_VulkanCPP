#include <metal_stdlib>
using namespace metal;

// ============================================================================
// VolumeParams — must match C++ VolumeParams exactly
// ============================================================================

struct VolumeParams {
    float4   VolumeOrigin;          // 0
    float4   VolumeExtent;          // 16
    float4   CameraPos;             // 32
    float4   InvViewProj[4];        // 48
    float    StepSize;              // 112
    float    MaxDistance;           // 116
    float    ExtinctionScale;       // 120
    float    ScatteringAlbedo;      // 124
    float    DensityThreshold;      // 128
    float    DensityFadeRange;      // 132
    uint     MaxSteps;              // 136
    uint     FrameIndex;            // 140
    uint     ScreenWidth;           // 144
    uint     ScreenHeight;          // 148
    float    _pad0[2];             // 152
    float4   SdfOrigins[3];        // 160
    float4   SdfVoxelSizes[3];     // 208
    float4   SdfExtents[3];        // 256
    uint     SdfResolutions[3];    // 304
    uint     SdfCascadeCount;      // 316
    float    _pad1[1];             // 320
    float4   LightDirection;       // 336
    float4   LightColor;           // 352
};

// ============================================================================
// AABB intersection
// ============================================================================

bool intersect_aabb(float3 origin, float3 dir,
                    float3 box_min, float3 box_max,
                    thread float& t_min, thread float& t_max) {
    float3 inv_dir = 1.0f / dir;
    float3 t0 = (box_min - origin) * inv_dir;
    float3 t1 = (box_max - origin) * inv_dir;
    float3 t_near = min(t0, t1);
    float3 t_far  = max(t0, t1);
    t_min = max(max(t_near.x, t_near.y), t_near.z);
    t_max = min(min(t_far.x,  t_far.y),  t_far.z);
    return t_max >= max(t_min, 0.0f);
}

// ============================================================================
// SDF cascade sampling (from DDGITraceRays.metal pattern)
// ============================================================================

float sampleSDFCascade(float3 pos,
                       texture3d<float, access::sample> sdf_tex,
                       float4 origin, float4 voxelSize, float4 extent, uint resolution) {
    float3 uvw = (pos - origin.xyz) / extent.xyz;
    if (uvw.x < 0.0f || uvw.x > 1.0f ||
        uvw.y < 0.0f || uvw.y > 1.0f ||
        uvw.z < 0.0f || uvw.z > 1.0f) return 1e10f;

    constexpr sampler s(coord::normalized, filter::linear,
                        address::clamp_to_edge, mip_filter::none);
    return sdf_tex.sample(s, uvw).x;
}

float sampleBestSDF(float3 pos,
                    texture3d<float, access::sample> sdf0,
                    texture3d<float, access::sample> sdf1,
                    texture3d<float, access::sample> sdf2,
                    constant VolumeParams& params) {
    if (params.SdfCascadeCount >= 1) {
        float d = sampleSDFCascade(pos, sdf0,
            params.SdfOrigins[0], params.SdfVoxelSizes[0], params.SdfExtents[0],
            params.SdfResolutions[0]);
        if (d < 1e9f) return d;
    }
    if (params.SdfCascadeCount >= 2) {
        float d = sampleSDFCascade(pos, sdf1,
            params.SdfOrigins[1], params.SdfVoxelSizes[1], params.SdfExtents[1],
            params.SdfResolutions[1]);
        if (d < 1e9f) return d;
    }
    if (params.SdfCascadeCount >= 3) {
        float d = sampleSDFCascade(pos, sdf2,
            params.SdfOrigins[2], params.SdfVoxelSizes[2], params.SdfExtents[2],
            params.SdfResolutions[2]);
        if (d < 1e9f) return d;
    }
    return 1e10f;
}

// ============================================================================
// Noise functions
// ============================================================================

float hash_float3(float3 p) {
    p = fract(p * float3(443.8975f, 397.2973f, 491.1871f));
    p += dot(p, p.yxz + 19.19f);
    return fract((p.x + p.y) * p.z);
}

float worley_noise_3d(float3 p) {
    float3 i = floor(p);
    float3 f = fract(p);
    float min_dist = 1.0f;

    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            for (int z = -1; z <= 1; z++) {
                float3 neighbor = float3(x, y, z);
                float3 point = float3(
                    hash_float3(i + neighbor + float3(0.0f, 0.0f, 0.0f)),
                    hash_float3(i + neighbor + float3(37.0f, 0.0f, 0.0f)),
                    hash_float3(i + neighbor + float3(0.0f, 0.0f, 74.0f)));
                float dist = length(neighbor + point - f);
                min_dist = min(min_dist, dist);
            }
        }
    }
    return min_dist;
}

float fbm_worley(float3 p, int octaves) {
    float value = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;

    for (int i = 0; i < octaves; i++) {
        value += amplitude * worley_noise_3d(p * frequency);
        frequency *= 2.0f;
        amplitude *= 0.5f;
    }
    return value;
}

// ============================================================================
// Density sampling (noise + SDF clipping)
// ============================================================================

float sample_density(float3 world_pos,
                     texture3d<float, access::sample> noise_tex,
                     texture3d<float, access::sample> sdf0,
                     texture3d<float, access::sample> sdf1,
                     texture3d<float, access::sample> sdf2,
                     constant VolumeParams& params) {
    float3 volume_min = params.VolumeOrigin.xyz;
    float3 volume_max = params.VolumeOrigin.xyz + params.VolumeExtent.xyz;

    // Quick bounds check with soft margin
    float3 uvw = (world_pos - volume_min) / (volume_max - volume_min);
    float edge = 0.1f;
    float3 edge_fade = smoothstep(float3(0.0f), float3(edge), uvw)
                     * smoothstep(float3(1.0f), float3(1.0f - edge), uvw);
    float bounds = edge_fade.x * edge_fade.y * edge_fade.z;
    if (bounds < 0.001f) return 0.0f;

    // Height gradient: denser at bottom, sparser at top (cloud base → anvil)
    float height_grad = smoothstep(0.0f, 0.15f, uvw.y) * smoothstep(1.0f, 0.5f, uvw.y);

    // Base shape from noise texture (continuous, not thresholded)
    float3 noise_uvw = uvw * 1.5f;
    constexpr sampler s(coord::normalized, filter::linear,
                        address::repeat, mip_filter::none);
    float base_shape = noise_tex.sample(s, noise_uvw).x;

    // Detail from procedural Worley at low frequency
    float detail = worley_noise_3d(uvw * 2.0f);

    // Combined: continuous density with structure
    float density = (base_shape * 0.6f + detail * 0.4f) * height_grad;

    // Erosion: carve hollow regions softly (not binary threshold)
    float erosion = worley_noise_3d(uvw * 2.5f);
    density *= smoothstep(0.0f, 0.35f, erosion);

    // SDF fade
    float sdf_fade = 1.0f;
    if (params.SdfCascadeCount > 0) {
        float sdf = sampleBestSDF(world_pos, sdf0, sdf1, sdf2, params);
        if (sdf > 10.0f) return 0.0f;
        sdf_fade = smoothstep(10.0f, 0.0f, sdf);
    }

    return max(0.0f, density) * bounds * sdf_fade;
}

// ============================================================================
// World position reconstruction from depth
// ============================================================================

float3 reconstruct_world_pos(float2 screen_uv, float depth,
                             constant VolumeParams& params) {
    float2 ndc = screen_uv * 2.0f - 1.0f;
    ndc.y = 1.0f - ndc.y;  // Metal V-flip

    float4 clip_pos = float4(ndc, depth, 1.0f);
    float4 world_pos = params.InvViewProj[0] * clip_pos.x
                     + params.InvViewProj[1] * clip_pos.y
                     + params.InvViewProj[2] * clip_pos.z
                     + params.InvViewProj[3] * clip_pos.w;
    return world_pos.xyz / world_pos.w;
}
