#include <metal_stdlib>
using namespace metal;

#include "CommonTypes.metal"
#include "CommonFunction.metal"

// ================================================================================================
// SSAO Bilateral Filter Shader
// Edge-aware upsampling from half-res to full-res using normal/depth similarity
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

// Read half-res AO with bilinear interpolation using manual 2x2 sampling
// (access::read textures don't support .sample())
float SampleAOBilinear(texture2d<float, access::read> tex, float2 uv, uint2 half_size) {
    float2 texel_pos = uv * float2(half_size) - 0.5f;
    uint2 base = uint2(clamp(int2(floor(texel_pos)), int2(0), int2(half_size - 1)));
    float2 frac_part = fract(texel_pos);

    float r00 = tex.read(base).r;
    uint2 p10 = min(base + uint2(1, 0), half_size - 1);
    uint2 p01 = min(base + uint2(0, 1), half_size - 1);
    uint2 p11 = min(base + uint2(1, 1), half_size - 1);
    float r10 = tex.read(p10).r;
    float r01 = tex.read(p01).r;
    float r11 = tex.read(p11).r;

    float top = mix(r00, r10, frac_part.x);
    float bot = mix(r01, r11, frac_part.x);
    return mix(top, bot, frac_part.y);
}

kernel void ssao_filter(
    uint3 global_id [[thread_position_in_grid]],

    texture2d<float, access::read>  ssao_input      [[texture(0)]],  // Half-res AO
    texture2d<float, access::read>  gbuffer_normal  [[texture(1)]],  // Full-res normal
    depth2d<float, access::read>    gbuffer_depth   [[texture(2)]],  // Full-res depth
    texture2d<float, access::write> ssao_output     [[texture(3)]],  // Full-res AO output

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
    uint2 half_size = uint2(params.half_width, params.half_height);

    // Center UV for sampling the half-res texture
    float2 center_uv = (float2(pixel_pos) + 0.5f) / full_size;

    // Linear depth for center pixel (for bilateral weight)
    float center_linear = NDCDepthToLinear(center_depth, 0.1f, 1000.0f);

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

            // Sample half-res AO with bilinear interpolation at the corresponding UV
            float2 sample_uv = (float2(sample_full_u) + 0.5f) / full_size;
            float ao_val = SampleAOBilinear(ssao_input, sample_uv, half_size);

            // Read sample normal and depth for bilateral weight
            float4 s_normal_enc = gbuffer_normal.read(sample_full_u);
            float3 sample_normal = normalize(s_normal_enc.xyz * 2.0f - 1.0f);
            float sample_depth = gbuffer_depth.read(sample_full_u);
            float sample_linear = NDCDepthToLinear(sample_depth, 0.1f, 1000.0f);

            // Bilateral weights
            float depth_diff = abs(center_linear - sample_linear);
            float w_depth = exp(-depth_diff * params.sigma_depth);

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
