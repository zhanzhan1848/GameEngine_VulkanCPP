#include <metal_stdlib>
using namespace metal;

// Reuse noise functions and low-level SDF cascade sampling from VolumeCommon
#include "VolumeCommon.metal"

// ============================================================================
// FroxelParams — must match C++ FroxelTypes.h exactly (384 bytes)
// ============================================================================

struct FroxelParams {
    // Camera (80 bytes)
    float4 CameraPos;           // 0
    float4 InvViewProj[4];      // 16

    // Frustum (16 bytes)
    float4 FrustumParams;       // 80: x=near, y=far, z=log2(far/near), w=1.0/depth_slices

    // Froxel grid (32 bytes)
    float4 FroxelDims;          // 96:  x=width, y=height, z=depth, w=1.0/width
    float4 FroxelDims2;         // 112: x=1.0/height, y=1.0/depth, z=pad, w=pad

    // Volume params (48 bytes)
    float4 VolumeParams1;       // 128: x=extinction_scale, y=scattering_albedo, z=density_threshold, w=density_fade_range
    float4 VolumeParams2;       // 144: x=height_fog_base, y=height_fog_scale, z=ambient_intensity, w=time
    float4 VolumeParams3;       // 160: x=phase_g, y=noise_scale, z=noise_speed, w=pad

    // Screen (16 bytes)
    float4 ScreenParams;        // 176: x=screen_width, y=screen_height, z=1.0/width, w=1.0/height

    // SDF cascades (144 bytes)
    float4 SdfOrigins[3];       // 192
    float4 SdfVoxelSizes[3];    // 240
    float4 SdfExtents[3];       // 288
    float4 SdfResolutions;      // 336: xyz=resolutions (float), w=cascade_count

    // Light (32 bytes)
    float4 LightDirection;      // 352
    float4 LightColor;          // 368

    // Shadow VP matrices (128 bytes)
    float4 ShadowVP[2][4];      // 384: [cascade][column], lightProj * lightView
};

// ============================================================================
// Froxel coordinate → world position
// ============================================================================

float3 froxel_to_world(uint3 froxel_xyz, constant FroxelParams& params) {
    // Exponential Z: z_view = near * pow(far/near, (slice + 0.5) / depth_count)
    float z_frac = (float(froxel_xyz.z) + 0.5) * params.FroxelDims2.y;
    float z_view = params.FrustumParams.x *
                   pow(params.FrustumParams.y / params.FrustumParams.x, z_frac);

    // Screen UV → NDC
    float2 screen_uv = (float2(froxel_xyz.xy) + 0.5) / float2(params.FroxelDims.xy);
    float2 ndc = screen_uv * 2.0 - 1.0;
    ndc.y = 1.0 - ndc.y; // Metal V-flip

    // Near plane world position for this pixel (NDC z = 0)
    float4 near4 = params.InvViewProj[0] * ndc.x
                 + params.InvViewProj[1] * ndc.y
                 + params.InvViewProj[2] * 0.0
                 + params.InvViewProj[3];
    near4 /= near4.w;

    // Scale from near plane: perspective rays are linear in depth
    float3 cam_to_near = near4.xyz - params.CameraPos.xyz;
    float scale = z_view / params.FrustumParams.x;
    return params.CameraPos.xyz + cam_to_near * scale;
}

// ============================================================================
// View-space depth from froxel Z slice
// ============================================================================

float froxel_z_to_depth(float slice, constant FroxelParams& params) {
    float z_frac = (slice + 0.5) * params.FroxelDims2.y;
    return params.FrustumParams.x *
           pow(params.FrustumParams.y / params.FrustumParams.x, z_frac);
}

// ============================================================================
// NDC depth → view-space Z
// ============================================================================

float ndc_to_view_z(float ndc_z, constant FroxelParams& params) {
    float near = params.FrustumParams.x;
    float far = params.FrustumParams.y;
    return (near * far) / (far - ndc_z * (far - near));
}

// ============================================================================
// View-space Z → froxel W coordinate [0, 1]
// ============================================================================

float view_z_to_froxel_w(float z_view, constant FroxelParams& params) {
    // w = log2(z/near) / log2(far/near)
    return log2(z_view / params.FrustumParams.x) / params.FrustumParams.z;
}

// ============================================================================
// Froxel-specific SDF cascade sampling
// ============================================================================

float froxel_sample_best_sdf(float3 pos,
                              texture3d<float, access::sample> sdf0,
                              texture3d<float, access::sample> sdf1,
                              texture3d<float, access::sample> sdf2,
                              constant FroxelParams& params) {
    uint count = uint(params.SdfResolutions.w);
    if (count >= 1) {
        float d = sampleSDFCascade(pos, sdf0,
            params.SdfOrigins[0], params.SdfVoxelSizes[0], params.SdfExtents[0],
            uint(params.SdfResolutions.x));
        if (d < 1e9f) return d;
    }
    if (count >= 2) {
        float d = sampleSDFCascade(pos, sdf1,
            params.SdfOrigins[1], params.SdfVoxelSizes[1], params.SdfExtents[1],
            uint(params.SdfResolutions.y));
        if (d < 1e9f) return d;
    }
    if (count >= 3) {
        float d = sampleSDFCascade(pos, sdf2,
            params.SdfOrigins[2], params.SdfVoxelSizes[2], params.SdfExtents[2],
            uint(params.SdfResolutions.z));
        if (d < 1e9f) return d;
    }
    return 1e10f;
}

// ============================================================================
// Density sampling (world-space, no AABB bounds — frustum-aligned)
// ============================================================================

float froxel_sample_density(float3 world_pos,
                             texture3d<float, access::sample> noise_tex,
                             texture3d<float, access::sample> sdf0,
                             texture3d<float, access::sample> sdf1,
                             texture3d<float, access::sample> sdf2,
                             constant FroxelParams& params) {
    // Height gradient (world-space Y)
    float height_grad = 1.0;
    if (params.VolumeParams2.y > 0.0) {
        float h = smoothstep(params.VolumeParams2.x - params.VolumeParams2.y,
                             params.VolumeParams2.x + params.VolumeParams2.y,
                             world_pos.y);
        height_grad = 1.0 - h;
    }

    // Noise texture sample (world-space)
    float3 noise_uvw = world_pos * params.VolumeParams3.y;
    constexpr sampler s(coord::normalized, filter::linear,
                        address::repeat, mip_filter::none);
    float base_shape = noise_tex.sample(s, noise_uvw).x;

    // Detail + erosion (procedural Worley)
    float detail = worley_noise_3d(world_pos * 2.0);
    float density = (base_shape * 0.6 + detail * 0.4) * height_grad;

    float erosion = worley_noise_3d(world_pos * 2.5);
    density *= smoothstep(0.0, 0.35, erosion);

    // SDF fade
    uint cascade_count = uint(params.SdfResolutions.w);
    if (cascade_count > 0) {
        float sdf = froxel_sample_best_sdf(world_pos, sdf0, sdf1, sdf2, params);
        if (sdf > 10.0) return 0.0;
        density *= smoothstep(10.0, 0.0, sdf);
    }

    // Density fade with distance
    float dist = length(world_pos - params.CameraPos.xyz);
    if (params.VolumeParams1.w > 0.0) {
        float fade = smoothstep(params.VolumeParams1.w, 0.0,
                                dist - params.VolumeParams1.w);
        density *= fade;
    }

    return max(0.0, density);
}

// ============================================================================
// Shadow VP matrix multiply (column-major float4[4] * float4 → NDC)
// ============================================================================

float3 shadow_project(float3 world_pos, constant float4 vp[4]) {
    float4 clip = float4(
        dot(float4(world_pos, 1.0), vp[0]),
        dot(float4(world_pos, 1.0), vp[1]),
        dot(float4(world_pos, 1.0), vp[2]),
        dot(float4(world_pos, 1.0), vp[3])
    );
    return clip.xyz / clip.w;
}

// Shadow UV from NDC clip coords (Metal UV: Y flipped)
float2 shadow_ndc_to_uv(float3 ndc) {
    return float2(ndc.x * 0.5 + 0.5, ndc.y * -0.5 + 0.5);
}
