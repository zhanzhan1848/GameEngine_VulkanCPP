/**
 * @file SSGIFilter.metal
 * @brief Lumen SSGI Phase 1 - Spatial Filtering (Bilateral Blur)
 *
 * Full-resolution compute shader that applies a depth-aware, normal-aware,
 * and hit-distance-aware bilateral filter to the temporally accumulated SSGI.
 *
 * Algorithm:
 *  1. For each pixel, sample a configurable NxN neighborhood (default 5x5)
 *  2. Weight each sample by depth similarity, normal similarity, and hit distance similarity
 *  3. Output the weighted average, preserving edges where geometry differs
 *
 * Input:  temporally accumulated SSGI (RGBA16F, full-resolution)
 *         RGB = accumulated irradiance, A = average hit distance
 * Output: spatially filtered SSGI (RGBA16F, full-resolution)
 *         RGB = filtered irradiance,  A = filtered hit distance
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
    float sigma_normal;    // Normal weight power (default 64.0)
    float sigma_hit_dist;  // Hit distance weight sharpness (default 8.0)
    float sigma_spatial;   // Gaussian spatial falloff (default 2.0)
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
// Main kernel: SSGI Spatial Filter
// ============================================================================

kernel void ssgi_filter(
    uint3 global_id [[thread_position_in_grid]],

    texture2d<float, access::read>  ssgi_input     [[texture(0)]],
    texture2d<float, access::read>  gbuffer_normal  [[texture(1)]],
    depth2d<float, access::read>    gbuffer_depth   [[texture(2)]],
    texture2d<float, access::write> ssgi_output     [[texture(3)]],

    device GlobalShaderData& gd       [[buffer(0)]],
    constant FilterParams&    params [[buffer(1)]]
)
{
    uint2 pixel_pos = global_id.xy;

    // Boundary check against texture dimensions
    uint width  = ssgi_input.get_width();
    uint height = ssgi_input.get_height();

    if (pixel_pos.x >= width || pixel_pos.y >= height) {
        return;
    }

    // -----------------------------------------------------------------------
    // Read center pixel data
    // -----------------------------------------------------------------------
    float4 center_ssgi  = ssgi_input.read(pixel_pos);
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

            // Boundary check
            if (sample_pos.x < 0 || sample_pos.y < 0 ||
                sample_pos.x >= int(width) || sample_pos.y >= int(height)) {
                continue;
            }

            uint2 sample_uv = uint2(sample_pos);

            // Read sample data
            float4 sample_ssgi  = ssgi_input.read(sample_uv);
            float  sample_depth = gbuffer_depth.read(sample_uv);
            float3 sample_normal = decodeNormal(gbuffer_normal.read(sample_uv));

            // ---- Depth weight ----
            float depth_diff = abs(center_depth - sample_depth);
            float w_depth = exp(-depth_diff * params.sigma_depth);

            // ---- Normal weight ----
            float NdotN = max(0.0, dot(center_normal, sample_normal));
            float w_normal = pow(NdotN, params.sigma_normal);

            // ---- Hit distance weight (from alpha channel of SSGI) ----
            float dist_diff = abs(center_hit_dist - sample_ssgi.a);
            float w_dist = exp(-dist_diff * params.sigma_hit_dist);

            // ---- Gaussian spatial weight ----
            float spatial_dist2 = float(dx * dx + dy * dy);
            float w_spatial = exp(-spatial_dist2 / (2.0 * params.sigma_spatial * params.sigma_spatial));

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
    // No Y-flip needed: Metal compute shaders (thread_position_in_grid) and
    // fragment shaders both use Y-down convention (row 0 = top).
    // The fullscreen triangle's UV mapping correctly maps screen pixels to texture texels.
    ssgi_output.write(float4(filtered_irradiance, filtered_hit_dist), pixel_pos);
}
