#include <metal_stdlib>
using namespace metal;

#include "CommonTypes.metal"
#include "CommonFunction.metal"

// ================================================================================================
// SSAO Bilateral Filter Shader
// Edge-aware upsampling from half-res to full-res using normal/depth similarity
// Uses hardware bilinear for SSAO input to stay within Apple Silicon texture read limits
// ================================================================================================

struct SSAOFilterParams {
    float sigma_depth;
    float sigma_normal;
    uint  kernel_radius;
    uint  full_width;
    uint  full_height;
    uint  half_width;
    uint  half_height;
};

kernel void ssao_filter(
    uint3 global_id [[thread_position_in_grid]],

    texture2d<float, access::sample> ssao_input      [[texture(0)]],  // Half-res AO (hardware bilinear)
    texture2d<float, access::read>   gbuffer_normal  [[texture(1)]],  // Full-res normal
    depth2d<float, access::read>     gbuffer_depth   [[texture(2)]],  // Full-res depth
    texture2d<float, access::write>  ssao_output     [[texture(3)]],  // Full-res AO output

    device GlobalShaderData&   gd     [[buffer(0)]],
    constant SSAOFilterParams& params [[buffer(1)]]
)
{
    uint2 pixel_pos = global_id.xy;

    if (pixel_pos.x >= params.full_width || pixel_pos.y >= params.full_height) {
        return;
    }

    // Sky pixels: no occlusion
    float center_depth = gbuffer_depth.read(pixel_pos);
    if (center_depth >= 1.0f) {
        ssao_output.write(float4(1.0f, 0.0f, 0.0f, 0.0f), pixel_pos);
        return;
    }

    // Read center normal and depth
    float4 normal_encoded = gbuffer_normal.read(pixel_pos);
    float3 center_normal = normalize(normal_encoded.xyz * 2.0f - 1.0f);

    float2 full_size = float2(params.full_width, params.full_height);

    // Center UV for sampling the half-res texture
    float2 center_uv = (float2(pixel_pos) + 0.5f) / full_size;

    // Linear depth for center pixel (for bilateral weight)
    float center_linear = NDCDepthToLinear(center_depth, 0.1f, 1000.0f);

    constexpr sampler ssao_sampler(coord::normalized, filter::linear, address::clamp_to_edge);

    float ao_sum = 0.0f;
    float weight_sum = 0.0f;

    int radius = int(params.kernel_radius);

    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            // Sample position at full-res
            int2 sample_full = int2(pixel_pos) + int2(dx, dy);
            if (sample_full.x < 0 || sample_full.x >= int(params.full_width) ||
                sample_full.y < 0 || sample_full.y >= int(params.full_height)) {
                continue;
            }

            uint2 sample_full_u = uint2(sample_full);

            // Sample half-res AO with hardware bilinear interpolation
            float2 sample_uv = (float2(sample_full_u) + 0.5f) / full_size;
            float ao_val = ssao_input.sample(ssao_sampler, sample_uv).r;

            // Read sample normal and depth for bilateral weight
            float4 s_normal_enc = gbuffer_normal.read(sample_full_u);
            float3 sample_normal = normalize(s_normal_enc.xyz * 2.0f - 1.0f);
            float sample_depth = gbuffer_depth.read(sample_full_u);
            float sample_linear = NDCDepthToLinear(sample_depth, 0.1f, 1000.0f);

            // Bilateral weights — use relative depth for consistent behavior across distance
            float depth_diff = abs(center_linear - sample_linear);
            float min_depth = max(min(center_linear, sample_linear), 0.001f);
            float relative_depth = depth_diff / min_depth;
            float w_depth = exp(-relative_depth * params.sigma_depth);

            float normal_sim = max(dot(center_normal, sample_normal), 0.0f);
            float w_normal = pow(normal_sim, params.sigma_normal);

            // Spatial weight (Gaussian)
            float dist2 = float(dx * dx + dy * dy);
            float w_spatial = exp(-dist2 / (2.0f * float(radius) * float(radius)));

            float w = w_depth * w_normal * w_spatial;

            ao_sum += ao_val * w;
            weight_sum += w;
        }
    }

    float ao = (weight_sum > 0.0f) ? ao_sum / weight_sum : 1.0f;
    ao = clamp(ao, 0.0f, 1.0f);

    ssao_output.write(float4(ao, 0.0f, 0.0f, 0.0f), pixel_pos);
}
