#include <metal_stdlib>
using namespace metal;

#include "CommonTypes.metal"
#include "Common.h"

// ================================================================================================
// GTAO (Ground Truth Ambient Occlusion) Trace Shader
// Half-resolution horizon-based AO computation
// ================================================================================================

struct SSAOTraceParams {
    float radius;           // World-space AO radius
    float power;            // Contrast power
    uint  direction_count;  // Number of slice directions
    uint  sample_count;     // Samples per direction per side
    uint  frame_index;      // For temporal rotation
    uint  output_width;     // Half-res width
    uint  output_height;    // Half-res height
    float near_plane;
    float far_plane;
};

float3 ReconstructViewPosAO(float2 uv, float depth_ndc, device GlobalShaderData& gd) {
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipPos = float4(ndc, depth_ndc, 1.0f);
    float4 viewPosH = gd.InvProjection * clipPos;
    float3 viewPos = viewPosH.xyz / viewPosH.w;
    viewPos.z = -viewPos.z;
    return viewPos;
}

float InterleavedGradientNoise(uint2 pixel, uint frameIndex) {
    float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    float2 noise = float2(pixel) + float2(frameIndex % 5u, (frameIndex / 5u) % 5u) * 4.0f;
    return fract(magic.z * fract(dot(noise, magic.xy)));
}

// Per-step spatial hash for breaking fixed ray marching patterns
static float hashStep(uint2 pixel, uint sliceIdx, uint stepIdx) {
    uint n = pixel.x * 374761393u + pixel.y * 668265263u + sliceIdx * 1274126177u + stepIdx * 1911520717u;
    n = (n ^ (n >> 13u)) * 1274126177u;
    return float(n & 0xFFFFu) / 65535.0f;
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
    if (pixel_pos.x >= params.output_width || pixel_pos.y >= params.output_height) return;

    uint2 full_pixel = pixel_pos * 2;
    float2 full_size = float2(gd.CameraPositionAndViewWidth.w, gd.CameraDirectionAndViewHeight.w);
    float2 uv = (float2(full_pixel) + 0.5f) / full_size;

    float depth_ndc = gbuffer_depth.read(full_pixel);
    if (depth_ndc >= 1.0f) {
        ssao_output.write(float4(1.0f, 0.0f, 0.0f, 0.0f), pixel_pos);
        return;
    }

    float4 normal_encoded = gbuffer_normal.read(full_pixel);
    float3 normal_vs = normalize(normal_encoded.xyz * 2.0f - 1.0f);
    float3 view_pos = ReconstructViewPosAO(uv, depth_ndc, gd);

    // View direction: pixel → camera (perspective-correct reference for angle computation)
    float3 view_dir = normalize(-view_pos);

    // Normal bias: push ray origin along normal to prevent self-occlusion from depth precision
    float bias = 0.005f * abs(view_pos.z);
    float3 biased_pos = view_pos + normal_vs * bias;

    // View ray: camera → pixel (for slice plane cross product)
    float3 view_ray = normalize(view_pos);

    float noise = InterleavedGradientNoise(pixel_pos, 0u);
    float visibility = 0.0f;
    constexpr float PI_VAL = 3.14159265f;

    for (uint slice = 0; slice < params.direction_count; ++slice) {
        float slice_angle = (float(slice) + noise) * PI_VAL / float(params.direction_count);
        float2 slice_dir = float2(cos(slice_angle), sin(slice_angle));

        // Screen-space step size: world radius / depth → UV-scale
        float step_size = params.radius / max(abs(view_pos.z), 0.01f);
        step_size = min(step_size, 0.2f);

        float horizon_pos = -1.0f;
        float horizon_neg = -1.0f;

        for (uint step = 1; step <= params.sample_count; ++step) {
            // Per-step spatial hash jitter: breaks concentric banding into high-freq noise
            float jitter = hashStep(pixel_pos, slice, step);
            float t = (float(step) - 0.5f + jitter) / float(params.sample_count);

            // --- March positive direction ---
            {
                float2 sample_uv = uv + slice_dir * step_size * t;
                if (sample_uv.x > 0.0f && sample_uv.x < 1.0f &&
                    sample_uv.y > 0.0f && sample_uv.y < 1.0f) {
                    uint2 sp = clamp(uint2(sample_uv * full_size), uint2(0), uint2(full_size - 1.0f));
                    float sd = gbuffer_depth.read(sp);
                    if (sd < 1.0f) {
                        float3 sv = ReconstructViewPosAO(sample_uv, sd, gd);
                        float3 delta = sv - biased_pos;
                        float len = length(delta);
                        if (len > 0.001f) {
                            // Quadratic falloff: gentle near, soft at edge
                            float falloff = 1.0f - saturate(len * len / (params.radius * params.radius));
                            // Soft thickness: reduce contribution from thin foreground objects
                            // (sample much closer = likely different surface, not a solid occluder)
                            float thickness_w = 1.0f;
                            if (sd < depth_ndc) {
                                thickness_w = saturate(1.0f - (depth_ndc - sd) * 30.0f);
                            }
                            float cos_a = dot(delta, view_dir) / len;
                            float weighted = mix(-1.0f, cos_a, falloff * thickness_w);
                            horizon_pos = max(horizon_pos, weighted);
                        }
                    }
                }
            }

            // --- March negative direction ---
            {
                float2 sample_uv = uv - slice_dir * step_size * t;
                if (sample_uv.x > 0.0f && sample_uv.x < 1.0f &&
                    sample_uv.y > 0.0f && sample_uv.y < 1.0f) {
                    uint2 sp = clamp(uint2(sample_uv * full_size), uint2(0), uint2(full_size - 1.0f));
                    float sd = gbuffer_depth.read(sp);
                    if (sd < 1.0f) {
                        float3 sv = ReconstructViewPosAO(sample_uv, sd, gd);
                        float3 delta = sv - biased_pos;
                        float len = length(delta);
                        if (len > 0.001f) {
                            float falloff = 1.0f - saturate(len * len / (params.radius * params.radius));
                            float thickness_w = 1.0f;
                            if (sd < depth_ndc) {
                                thickness_w = saturate(1.0f - (depth_ndc - sd) * 30.0f);
                            }
                            float cos_a = dot(delta, view_dir) / len;
                            float weighted = mix(-1.0f, cos_a, falloff * thickness_w);
                            horizon_neg = max(horizon_neg, weighted);
                        }
                    }
                }
            }
        }

        // Slice plane normal via cross product of view ray and screen-space slice direction
        float3 slice_3d = float3(slice_dir.x, slice_dir.y, 0.0f);
        float3 slice_normal = normalize(cross(view_ray, slice_3d));

        // Project surface normal onto slice plane
        float3 proj = normal_vs - slice_normal * dot(normal_vs, slice_normal);
        float pl = length(proj);
        if (pl > 0.001f) proj /= pl;
        else proj = view_dir;

        // Normal cosine relative to view direction (perspective-correct)
        float n_cos = dot(proj, view_dir);

        // Relative horizons
        float h1 = clamp(horizon_pos - n_cos, -1.0f, 1.0f);
        float h2 = clamp(horizon_neg - n_cos, -1.0f, 1.0f);

        // GTAO analytical integration
        visibility += 0.25f * (-h2 * sqrt(1.0f - h2 * h2) + acos(clamp(h2, -1.0f, 1.0f)))
                    + 0.25f * ( h1 * sqrt(1.0f - h1 * h1) + acos(clamp(h1, -1.0f, 1.0f)));
    }

    float ao = clamp(visibility / float(params.direction_count), 0.0f, 1.0f);
    ao = pow(ao, params.power);
    // AO floor: even deepest corners retain faint bounce light, prevent dead black
    ao = max(ao, 0.1f);

    ssao_output.write(float4(ao, 0.0f, 0.0f, 0.0f), pixel_pos);
}
