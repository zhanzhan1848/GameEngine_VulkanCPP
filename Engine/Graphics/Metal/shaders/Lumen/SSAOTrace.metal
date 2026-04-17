#include <metal_stdlib>
using namespace metal;

#include "CommonTypes.metal"
#include "CommonFunction.metal"

// ================================================================================================
// GTAO (Ground Truth Ambient Occlusion) Trace Shader
// Half-resolution horizon-based AO computation
// ================================================================================================

struct SSAOTraceParams {
    float radius;           // World-space AO radius
    float power;            // Contrast power
    uint  direction_count;  // Number of slice directions (typically 4)
    uint  sample_count;     // Samples per direction per side (typically 2)
    uint  frame_index;      // For temporal rotation of slice directions
    uint  output_width;     // Half-res width
    uint  output_height;    // Half-res height
    float near_plane;
    float far_plane;
};

// Reconstruct view-space position from NDC depth
// NOTE: Metal uses left-hand Z, so we must negate Z for correct view-space depth
float3 ReconstructViewPosAO(float2 uv, float depth_ndc, device GlobalShaderData& gd) {
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y = 1.0f - ndc.y; // Metal UV: 0=top, 1=bottom -> NDC Y: -1=bottom, 1=top
    float4 clipPos = float4(ndc, depth_ndc, 1.0f);
    float4 viewPosH = gd.InvProjection * clipPos;
    float3 viewPos = viewPosH.xyz / viewPosH.w;
    viewPos.z = -viewPos.z; // CRITICAL: Metal left-hand fix
    return viewPos;
}

// Interleaved gradient noise for temporal stability
float InterleavedGradientNoise(uint2 pixel, uint frameIndex) {
    float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    float2 noise = float2(pixel) + float2(frameIndex % 5u, (frameIndex / 5u) % 5u) * 4.0f;
    return fract(magic.z * fract(dot(noise, magic.xy)));
}

kernel void ssao_trace(
    uint3 global_id [[thread_position_in_grid]],

    texture2d<float, access::read>  gbuffer_normal  [[texture(0)]],
    depth2d<float, access::read>    gbuffer_depth   [[texture(1)]],
    texture2d<float, access::write> ssao_output     [[texture(2)]],

    device GlobalShaderData&   gd     [[buffer(0)]],
    constant SSAOTraceParams&  params [[buffer(1)]]
)
{
    uint2 pixel_pos = global_id.xy;

    // Boundary check at half-resolution
    if (pixel_pos.x >= params.output_width || pixel_pos.y >= params.output_height) {
        return;
    }

    // Map half-res pixel to full-res pixel for GBuffer sampling
    uint2 full_pixel = pixel_pos * 2;
    float2 full_size = float2(gd.CameraPositionAndViewWidth.w, gd.CameraDirectionAndViewHeight.w);

    // UV coordinates for full-res sampling
    float2 uv = (float2(full_pixel) + 0.5f) / full_size;

    // Sample depth at full-res center
    float depth_ndc = gbuffer_depth.read(full_pixel);

    // Sky pixels: no occlusion
    if (depth_ndc >= 1.0f) {
        ssao_output.write(float4(1.0f, 0.0f, 0.0f, 0.0f), pixel_pos);
        return;
    }

    // Sample normal (encoded [0,1] -> [-1,1])
    float4 normal_encoded = gbuffer_normal.read(full_pixel);
    float3 normal_vs = normalize(normal_encoded.xyz * 2.0f - 1.0f);

    // Reconstruct view-space position
    float3 view_pos = ReconstructViewPosAO(uv, depth_ndc, gd);

    // Noise for temporal rotation
    float noise = InterleavedGradientNoise(pixel_pos, params.frame_index);

    // GTAO: horizon-based ambient occlusion
    float visibility = 0.0f;
    const float PI_VAL = 3.14159265f;

    for (uint slice = 0; slice < params.direction_count; ++slice) {
        // Slice direction rotated by noise for temporal variation
        float slice_angle = (float(slice) + noise) * PI_VAL / float(params.direction_count);
        float2 slice_dir = float2(cos(slice_angle), sin(slice_angle));

        // Project slice into screen space
        // The step size is scaled by view-space depth for perspective-correct sampling
        float step_size = params.radius / max(abs(view_pos.z), 0.01f);
        step_size = min(step_size, 0.2f); // Cap to avoid excessive screen-space steps

        // Find horizons: march in both directions along the slice
        float horizon_min = -1.0f; // cos angle: 1 = perpendicular, -1 = behind
        float horizon_max = -1.0f;

        for (uint step = 1; step <= params.sample_count; ++step) {
            float t = float(step) / float(params.sample_count);

            // March in positive direction
            {
                float2 offset = slice_dir * step_size * t;
                float2 sample_uv = uv + offset / full_size;

                if (sample_uv.x > 0.0f && sample_uv.x < 1.0f &&
                    sample_uv.y > 0.0f && sample_uv.y < 1.0f) {
                    uint2 sample_pixel = uint2(sample_uv * full_size);
                    sample_pixel = clamp(sample_pixel, uint2(0), uint2(full_size - 1.0f));
                    float sample_depth = gbuffer_depth.read(sample_pixel);

                    if (sample_depth < 1.0f) {
                        float3 sample_view = ReconstructViewPosAO(sample_uv, sample_depth, gd);
                        float3 delta = sample_view - view_pos;
                        float len = length(delta);
                        // Distance falloff to avoid far-away geometry contributing
                        float falloff = 1.0f - smoothstep(0.0f, params.radius, len);
                        float cos_angle = delta.z / max(len, 0.0001f);
                        horizon_max = max(horizon_max, cos_angle * falloff + horizon_max * (1.0f - falloff));
                    }
                }
            }

            // March in negative direction
            {
                float2 offset = -slice_dir * step_size * t;
                float2 sample_uv = uv + offset / full_size;

                if (sample_uv.x > 0.0f && sample_uv.x < 1.0f &&
                    sample_uv.y > 0.0f && sample_uv.y < 1.0f) {
                    uint2 sample_pixel = uint2(sample_uv * full_size);
                    sample_pixel = clamp(sample_pixel, uint2(0), uint2(full_size - 1.0f));
                    float sample_depth = gbuffer_depth.read(sample_pixel);

                    if (sample_depth < 1.0f) {
                        float3 sample_view = ReconstructViewPosAO(sample_uv, sample_depth, gd);
                        float3 delta = sample_view - view_pos;
                        float len = length(delta);
                        float falloff = 1.0f - smoothstep(0.0f, params.radius, len);
                        float cos_angle = delta.z / max(len, 0.0001f);
                        horizon_min = max(horizon_min, cos_angle * falloff + horizon_min * (1.0f - falloff));
                    }
                }
            }
        }

        // Compute the slice's normal in view space projected onto the slice plane
        float3 slice_normal = float3(slice_dir.x, slice_dir.y, 0.0f);
        float3 proj_normal = normal_vs - slice_normal * dot(normal_vs, slice_normal);
        float proj_len = length(proj_normal);
        if (proj_len > 0.001f) {
            proj_normal /= proj_len;
        } else {
            proj_normal = float3(0.0f, 0.0f, 1.0f);
        }

        // Normal's cos angle in the slice
        float n_cos = proj_normal.z;

        // Clamp horizons relative to surface normal
        float h1 = clamp(horizon_max - n_cos, -1.0f, 1.0f);
        float h2 = clamp(horizon_min - n_cos, -1.0f, 1.0f);

        // GTAO integration: the area under the horizon arc
        visibility += 0.25f * (-h2 * sqrt(1.0f - h2 * h2) + acos(clamp(h2, -1.0f, 1.0f)))
                    + 0.25f * ( h1 * sqrt(1.0f - h1 * h1) + acos(clamp(h1, -1.0f, 1.0f)));
    }

    // Normalize and apply power curve
    float ao = clamp(visibility / float(params.direction_count), 0.0f, 1.0f);
    ao = pow(ao, params.power);

    ssao_output.write(float4(ao, 0.0f, 0.0f, 0.0f), pixel_pos);
}
