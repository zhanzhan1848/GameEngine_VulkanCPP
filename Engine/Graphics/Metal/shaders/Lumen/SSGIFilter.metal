/**
 * @file SSGIFilter.metal
 * @brief Lumen SSGI Phase 2 - Spatial Filter with Bilinear Upsampling
 *
 * Full-resolution compute shader that:
 *  1. Bilinearly upsamples half-resolution SSGI trace to full-resolution
 *  2. Applies a depth-aware, normal-aware bilateral filter on the upsampled data
 *  3. Preserves edges using full-res GBuffer depth and normal weights
 *
 * Input:  half-res SSGI trace (RGBA16F, bilinear sampled)
 *         full-res GBuffer normal and depth
 * Output: full-res spatially filtered SSGI (RGBA16F)
 */

#include <metal_stdlib>
using namespace metal;

#include "CommonTypes.metal"
#include "Common.h"

constant float LUMEN_PI = 3.14159265358979323846f;

// ============================================================================
// Filter Parameters
// ============================================================================

struct FilterParams {
    float sigma_depth;     // Depth weight sharpness (default 10.0)
    float sigma_normal;    // Normal weight power (default 16.0)
    float sigma_hit_dist;  // Hit distance weight sharpness (default 8.0)
    float sigma_spatial;   // Gaussian spatial falloff (default 2.5)
    uint  kernel_radius;   // Filter kernel radius (default 2 = 5x5)
};

// ============================================================================
// Normal Decoding
// ============================================================================

static inline float3 decodeNormal(float4 encoded)
{
    return normalize(encoded.xyz * 2.0 - 1.0);
}

// ============================================================================
// Main kernel: SSGI Spatial Filter with bilinear upsampling
// ============================================================================

kernel void ssgi_filter(
    uint3 global_id [[thread_position_in_grid]],

    texture2d<float, access::sample> ssgi_input     [[texture(0)]],  // half-res trace
    texture2d<float, access::read>   gbuffer_normal  [[texture(1)]],
    depth2d<float, access::read>     gbuffer_depth   [[texture(2)]],
    texture2d<float, access::write>  ssgi_output     [[texture(3)]],  // full-res output

    device GlobalShaderData& gd       [[buffer(0)]],
    constant FilterParams&    params [[buffer(1)]]
)
{
    uint2 pixel_pos = global_id.xy;

    // Use output texture dimensions for boundary check (full-res)
    uint width  = ssgi_output.get_width();
    uint height = ssgi_output.get_height();

    if (pixel_pos.x >= width || pixel_pos.y >= height) {
        return;
    }

    // Bilinear sampler for half-res → full-res upsampling
    sampler bilinear(coord::normalized, filter::linear, address::clamp_to_edge);
    float2 invRes = 1.0 / float2(width, height);

    // -----------------------------------------------------------------------
    // Read center pixel data
    // -----------------------------------------------------------------------
    // Bilinearly upsample from half-res trace
    float2 center_uv = (float2(pixel_pos) + 0.5) * invRes;
    float4 center_ssgi  = ssgi_input.sample(bilinear, center_uv);
    float  center_depth = gbuffer_depth.read(pixel_pos);
    float3 center_normal = decodeNormal(gbuffer_normal.read(pixel_pos));

    float3 center_irradiance = center_ssgi.rgb;
    float  center_hit_dist   = center_ssgi.a;

    // -----------------------------------------------------------------------
    // Bilateral filter with configurable kernel radius
    // -----------------------------------------------------------------------
    uint radius = min(params.kernel_radius, 4u); // Clamp to max 4 (9x9)

    float3 filtered_irradiance = float3(0.0);
    float  filtered_hit_dist   = 0.0;
    float  total_weight        = 0.0;

    for (int dy = -int(radius); dy <= int(radius); dy++) {
        for (int dx = -int(radius); dx <= int(radius); dx++) {
            int2 sample_pos = int2(pixel_pos) + int2(dx, dy);

            // Boundary check against full-res dimensions
            if (sample_pos.x < 0 || sample_pos.y < 0 ||
                sample_pos.x >= int(width) || sample_pos.y >= int(height)) {
                continue;
            }

            // Bilinearly upsample from half-res trace at neighbor position
            float2 sample_uv = (float2(sample_pos) + 0.5) * invRes;
            float4 sample_ssgi = ssgi_input.sample(bilinear, sample_uv);

            // Full-res GBuffer reads for edge-preserving weights
            float  sample_depth = gbuffer_depth.read(uint2(sample_pos));
            float3 sample_normal = decodeNormal(gbuffer_normal.read(uint2(sample_pos)));

            // ---- Depth weight ----
            float depth_diff = abs(center_depth - sample_depth);
            float w_depth = exp(-depth_diff * params.sigma_depth);

            // ---- Normal weight ----
            float NdotN = max(0.0, dot(center_normal, sample_normal));
            float w_normal = pow(NdotN, params.sigma_normal);

            // ---- Hit distance weight (from alpha channel of SSGI) ----
            float dist_diff = abs(center_hit_dist - sample_ssgi.a);
            float w_dist = exp(-dist_diff * params.sigma_hit_dist);

            // ---- Hit-distance adaptive spatial weight ----
            // Close surfaces: small kernel (preserve detail)
            // Far surfaces: large kernel (smooth more)
            float adaptive_sigma = max(params.sigma_spatial * center_hit_dist * 0.5f,
                                       params.sigma_spatial * 0.5f);
            float spatial_dist2 = float(dx * dx + dy * dy);
            float w_spatial = exp(-spatial_dist2 / (2.0 * adaptive_sigma * adaptive_sigma));

            // Combined weight
            float weight = w_depth * w_normal * w_dist * w_spatial;

            filtered_irradiance += sample_ssgi.rgb * weight;
            filtered_hit_dist   += sample_ssgi.a   * weight;
            total_weight        += weight;
        }
    }

    // -----------------------------------------------------------------------
    // Normalize with division-by-zero guard
    // -----------------------------------------------------------------------
    if (total_weight > 1e-6) {
        filtered_irradiance /= total_weight;
        filtered_hit_dist   /= total_weight;
    } else {
        // Fallback: use center pixel
        filtered_irradiance = center_irradiance;
        filtered_hit_dist   = center_hit_dist;
    }

    // -----------------------------------------------------------------------
    // Output: RGBA16F (filtered irradiance + filtered hit distance)
    // -----------------------------------------------------------------------
    ssgi_output.write(float4(filtered_irradiance, filtered_hit_dist), pixel_pos);
}
