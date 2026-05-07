/**
 * @file SSGITemporal.metal
 * @brief Lumen SSGI Phase 3 - Temporal Accumulation
 *
 * Full-resolution compute shader that:
 *  1. Reads spatially filtered SSGI at full-resolution (no upsampling needed)
 *  2. Reprojects using velocity buffer to find previous-frame UV
 *  3. Samples history buffer at reprojected UV with bilinear interpolation
 *  4. Clamps history via variance clipping (mean + 2σ on 3x3 neighborhood)
 *  5. Detects disocclusion via depth comparison
 *  6. Exponential blend: result = lerp(current, clamped_history, feedback)
 */

#include <metal_stdlib>
using namespace metal;

#include "CommonTypes.metal"
#include "CommonFunction.metal"

constant float LUMEN_PI = 3.14159265358979323846f;

// ============================================================================
// Temporal Accumulation Parameters
// ============================================================================

struct TemporalParams {
    float feedback;         // Temporal feedback weight (default 0.9 = 90% history)
    uint  full_width;       // Full-resolution width
    uint  full_height;      // Full-resolution height
};

// ============================================================================
// Decode normal from BGRA8 encoded storage
// ============================================================================

static inline float3 decodeNormal(float4 encoded)
{
    return normalize(encoded.xyz * 2.0 - 1.0);
}

// ============================================================================
// Main kernel: SSGI Temporal Accumulation
// ============================================================================

kernel void ssgi_temporal(
    uint3 global_id [[thread_position_in_grid]],

    texture2d<float, access::read>  ssgi_spatial    [[texture(0)]],  // full-res spatial filter output
    texture2d<float, access::read>  ssgi_history    [[texture(1)]],
    texture2d<float, access::read>  velocity_buffer [[texture(2)]],
    depth2d<float, access::read>    gbuffer_depth   [[texture(3)]],
    texture2d<float, access::read>  gbuffer_normal  [[texture(4)]],
    texture2d<float, access::write> ssgi_output     [[texture(5)]],

    device GlobalShaderData& gd       [[buffer(0)]],
    constant TemporalParams&   params [[buffer(1)]]
)
{
    uint2 pixel_pos = global_id.xy;

    // Boundary check against full-resolution output
    if (pixel_pos.x >= params.full_width || pixel_pos.y >= params.full_height) {
        return;
    }

    // -----------------------------------------------------------------------
    // Compute UV for the current full-res pixel
    // -----------------------------------------------------------------------
    float2 pixel_uv = (float2(pixel_pos) + 0.5) / float2(params.full_width, params.full_height);

    // -----------------------------------------------------------------------
    // Read full-res GBuffer data
    // -----------------------------------------------------------------------
    float  depth_ndc      = gbuffer_depth.read(pixel_pos);
    float3 surface_normal = decodeNormal(gbuffer_normal.read(pixel_pos));

    // -----------------------------------------------------------------------
    // Step 1: Read full-res spatially filtered SSGI directly (no upsampling)
    // -----------------------------------------------------------------------
    float4 current_ssgi_4  = ssgi_spatial.read(pixel_pos);
    float3 current_ssgi    = current_ssgi_4.rgb;
    float  current_hit_dist = current_ssgi_4.a;

    // -----------------------------------------------------------------------
    // Step 2: Velocity Reprojection
    // -----------------------------------------------------------------------
    float2 velocity = velocity_buffer.read(pixel_pos).rg;
    float2 prev_uv;
    prev_uv.x = pixel_uv.x - velocity.x;
    prev_uv.y = pixel_uv.y + velocity.y;

    // -----------------------------------------------------------------------
    // Step 3: Sample history at reprojected UV
    // -----------------------------------------------------------------------
    float3 history_ssgi      = float3(0.0);
    float  history_hit_dist  = 0.0;
    bool   valid_history     = false;

    // Check if reprojected UV is within screen bounds
    float2 edge_dist     = min(prev_uv, 1.0 - prev_uv);
    bool   in_bounds     = (prev_uv.x >= 0.0 && prev_uv.x <= 1.0 &&
                            prev_uv.y >= 0.0 && prev_uv.y <= 1.0);

    if (in_bounds) {
        // Bilinear sample the history texture manually (access::read)
        float2 hist_coord = prev_uv * float2(params.full_width, params.full_height) - 0.5;
        int2   hist_base  = int2(floor(hist_coord));
        float2 hist_frac  = fract(hist_coord);

        int2 max_coord = int2(int(params.full_width) - 1, int(params.full_height) - 1);

        int2 c00 = clamp(hist_base,                     int2(0), max_coord);
        int2 c10 = clamp(hist_base + int2(1, 0),        int2(0), max_coord);
        int2 c01 = clamp(hist_base + int2(0, 1),        int2(0), max_coord);
        int2 c11 = clamp(hist_base + int2(1, 1),        int2(0), max_coord);

        float4 h00 = ssgi_history.read(uint2(c00));
        float4 h10 = ssgi_history.read(uint2(c10));
        float4 h01 = ssgi_history.read(uint2(c01));
        float4 h11 = ssgi_history.read(uint2(c11));

        float4 hist_bilinear = h00 * (1.0 - hist_frac.x) * (1.0 - hist_frac.y)
                             + h10 *        hist_frac.x  * (1.0 - hist_frac.y)
                             + h01 * (1.0 - hist_frac.x) *       hist_frac.y
                             + h11 *        hist_frac.x  *       hist_frac.y;

        history_ssgi     = hist_bilinear.rgb;
        history_hit_dist = hist_bilinear.a;
        valid_history    = true;
    }

    // If no valid history, output current spatial value directly
    if (!valid_history) {
        ssgi_output.write(float4(current_ssgi, current_hit_dist), pixel_pos);
        return;
    }

    // -----------------------------------------------------------------------
    // Step 4: Variance Clipping (Mean + Sigma) on 3x3 neighborhood
    // -----------------------------------------------------------------------
    // Uses full-res spatial output for the neighborhood (no half-res mapping)

    float3 neighborhood_sum   = current_ssgi;
    float3 neighborhood_sum2  = current_ssgi * current_ssgi;
    int   neighborhood_count  = 1;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;

            int2 neighbor_pos = int2(pixel_pos) + int2(dx, dy);
            if (neighbor_pos.x < 0 || neighbor_pos.y < 0 ||
                neighbor_pos.x >= int(params.full_width) ||
                neighbor_pos.y >= int(params.full_height)) {
                continue;
            }

            float3 neighbor_ssgi = ssgi_spatial.read(uint2(neighbor_pos)).rgb;

            neighborhood_sum  += neighbor_ssgi;
            neighborhood_sum2 += neighbor_ssgi * neighbor_ssgi;
            neighborhood_count++;
        }
    }

    // Compute mean and standard deviation
    float3 mean     = neighborhood_sum / float(neighborhood_count);
    float3 variance = abs(neighborhood_sum2 / float(neighborhood_count) - mean * mean);
    float3 sigma    = sqrt(max(variance, float3(0.0)));

    // AABB = mean +/- 2*sigma (captures ~95% of distribution)
    float3 sigma_floor = max(mean * 0.1, float3(0.01));
    float3 aabb_min = mean - sigma * 2.0 - sigma_floor;
    float3 aabb_max = mean + sigma * 2.0 + sigma_floor;

    // Clamp history to the AABB
    float3 clamped_history = clamp(history_ssgi, aabb_min, aabb_max);

    // -----------------------------------------------------------------------
    // Step 5: Disocclusion detection + screen-edge fade
    // -----------------------------------------------------------------------
    float disocclusion_fade = 1.0;
    {
        if (in_bounds) {
            int2 hist_pixel = clamp(int2(prev_uv * float2(params.full_width, params.full_height)),
                                    int2(0), int2(int(params.full_width) - 1, int(params.full_height) - 1));
            float hist_depth = gbuffer_depth.read(uint2(hist_pixel));
            float depth_diff = abs(depth_ndc - hist_depth);
            // Gradual rejection: full history at diff=0.005, no history at diff=0.02
            disocclusion_fade = saturate((0.02 - depth_diff) / 0.015);
        }
    }

    // Screen-edge fade for history
    float edge_fade = smoothstep(0.0, 0.05, min(edge_dist.x, edge_dist.y));

    // -----------------------------------------------------------------------
    // Step 6: Confidence-modulated exponential blend
    // -----------------------------------------------------------------------
    // Hit distance confidence: close hits → high confidence → more history
    // Far hits / misses (hit_dist ~ radius=2.0) → low confidence → less history
    float hit_confidence = saturate(1.0 - current_hit_dist / 2.0);
    float confidence_scale = mix(0.6, 1.0, hit_confidence);

    float effective_feedback = params.feedback * edge_fade * disocclusion_fade * confidence_scale;

    float3 result_color = mix(current_ssgi, clamped_history, effective_feedback);
    float  result_dist  = mix(current_hit_dist, history_hit_dist, effective_feedback);

    // Output: RGB = accumulated irradiance, A = hit distance / confidence
    ssgi_output.write(float4(result_color, result_dist), pixel_pos);
}
