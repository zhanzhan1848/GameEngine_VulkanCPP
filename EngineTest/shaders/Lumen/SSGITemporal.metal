/**
 * @file SSGITemporal.metal
 * @brief Lumen SSGI Phase 1 - Temporal Accumulation with Bilateral Upsampling
 *
 * Full-resolution compute shader that:
 *  1. Upsamples half-resolution SSGI trace output to full-resolution via bilateral upsampling
 *  2. Reprojects using velocity buffer to find previous-frame UV
 *  3. Samples history accumulation buffer at reprojected UV
 *  4. Clamps history to 3x3 neighborhood AABB (variance clipping) to prevent ghosting
 *  5. Exponential blend: result = lerp(current, clamped_history, feedback)
 *  6. Outputs full-resolution accumulated SSGI (RGBA16F)
 */

#include <metal_stdlib>
using namespace metal;

#include "CommonTypes.metal"
#include "Common.h"

constant float LUMEN_PI = 3.14159265358979323846f;

// ============================================================================
// Temporal Accumulation Parameters
// ============================================================================

struct TemporalParams {
    float feedback;         // Temporal feedback weight (default 0.95 = 95% history)
    uint  full_width;       // Full-resolution width
    uint  full_height;      // Full-resolution height
    uint  half_width;       // Half-resolution width
    uint  half_height;      // Half-resolution height
};

// ============================================================================
// Bilateral Upsampling: half-res SSGI -> full-res SSGI
// ============================================================================

/**
 * @brief Upsample half-resolution SSGI to full-resolution using bilateral weights
 *
 * For each full-res pixel, finds the 4 nearest half-res texels (2x2 neighborhood)
 * and computes depth-weighted + bilinear-weighted average. This preserves edges
 * by down-weighting half-res samples whose depth differs from the full-res pixel.
 */
static float3 bilateralUpsample(
    float2 full_uv,
    float  full_depth,
    texture2d<float, access::read> half_ssgi,
    depth2d<float, access::read> gbuffer_depth,
    uint full_width,
    uint full_height,
    uint half_width,
    uint half_height)
{
    // Map full-res UV to half-res texel coordinate space
    float2 half_coord = full_uv * float2(half_width, half_height) - 0.5;
    int2   base       = int2(floor(half_coord));
    float2 frac_part  = fract(half_coord);

    float3 result       = float3(0.0);
    float  total_weight = 0.0;

    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            int2 half_pos = base + int2(dx, dy);

            // Boundary check on half-res texture
            if (half_pos.x < 0 || half_pos.y < 0 ||
                half_pos.x >= int(half_width) || half_pos.y >= int(half_height)) {
                continue;
            }

            // Find the corresponding full-res pixel for this half-res texel.
            // Each half-res texel (hx, hy) maps to the full-res neighborhood
            // around (hx*2, hy*2). We use a consistent offset to get a representative
            // depth for this half-res texel.
            int2 corr_full = half_pos * 2 + int2(dx, dy);
            corr_full = clamp(corr_full, int2(0), int2(int(full_width) - 1, int(full_height) - 1));

            float corr_depth = gbuffer_depth.read(uint2(corr_full));

            // Read half-res SSGI value
            float4 ssgi_val = half_ssgi.read(uint2(half_pos));

            // Depth-based bilateral weight: reject samples at different depths
            float depth_diff = abs(full_depth - corr_depth);
            float w_depth = exp(-depth_diff * 10.0);

            // Bilinear positional weight
            float w_bilinear = (1.0 - abs(float(dx) - frac_part.x)) *
                               (1.0 - abs(float(dy) - frac_part.y));

            float w = w_depth * w_bilinear + 0.0001;
            result       += ssgi_val.rgb * w;
            total_weight += w;
        }
    }

    return (total_weight > 0.0) ? result / total_weight : float3(0.0);
}

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

    texture2d<float, access::read>  ssgi_trace      [[texture(0)]],
    texture2d<float, access::read>  ssgi_history     [[texture(1)]],
    texture2d<float, access::read>  velocity_buffer  [[texture(2)]],
    depth2d<float, access::read>    gbuffer_depth    [[texture(3)]],
    texture2d<float, access::read>  gbuffer_normal   [[texture(4)]],
    texture2d<float, access::write> ssgi_output      [[texture(5)]],

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
    // Step 1: Bilateral Upsample half-res SSGI to full-res
    // -----------------------------------------------------------------------
    float3 current_ssgi = bilateralUpsample(
        pixel_uv,
        depth_ndc,
        ssgi_trace,
        gbuffer_depth,
        params.full_width,
        params.full_height,
        params.half_width,
        params.half_height
    );

    // Also upsample the alpha channel (hit distance / confidence)
    // using a simpler bilinear approach with the same weights
    float current_hit_dist = 0.0;
    {
        float2 half_coord = pixel_uv * float2(params.half_width, params.half_height) - 0.5;
        int2   base       = int2(floor(half_coord));
        float2 frac_part  = fract(half_coord);

        float  total_w = 0.0;
        float  accum   = 0.0;
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                int2 hp = base + int2(dx, dy);
                if (hp.x < 0 || hp.y < 0 ||
                    hp.x >= int(params.half_width) || hp.y >= int(params.half_height)) continue;

                float w_bilinear = (1.0 - abs(float(dx) - frac_part.x)) *
                                   (1.0 - abs(float(dy) - frac_part.y));

                int2 corr_full = hp * 2 + int2(dx, dy);
                corr_full = clamp(corr_full, int2(0), int2(int(params.full_width) - 1, int(params.full_height) - 1));
                float corr_depth = gbuffer_depth.read(uint2(corr_full));

                float w_depth = exp(-abs(depth_ndc - corr_depth) * 10.0);
                float w = w_depth * w_bilinear + 0.0001;

                accum   += ssgi_trace.read(uint2(hp)).a * w;
                total_w += w;
            }
        }
        current_hit_dist = (total_w > 0.0) ? accum / total_w : 0.0;
    }

    // -----------------------------------------------------------------------
    // Step 2: Velocity Reprojection
    // -----------------------------------------------------------------------
    // Velocity encoding: velocity = (currentNDC - previousNDC) * 0.5
    // NDC -> UV: uv.x = ndc.x * 0.5 + 0.5,  uv.y = 0.5 - 0.5 * ndc.y
    // So: prev_uv.x = pixel_uv.x - velocity.x
    //     prev_uv.y = pixel_uv.y + velocity.y  (Metal Y-flip sign reversal)
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

    // Check if reprojected UV is within screen bounds (with edge fade margin)
    float2 edge_dist     = min(prev_uv, 1.0 - prev_uv);
    float  edge_fade_min = 0.01;
    bool   in_bounds     = (prev_uv.x >= 0.0 && prev_uv.x <= 1.0 &&
                            prev_uv.y >= 0.0 && prev_uv.y <= 1.0);

    if (in_bounds) {
        // Bilinear sample the history texture manually (access::read)
        float2 hist_coord = prev_uv * float2(params.full_width, params.full_height) - 0.5;
        int2   hist_base  = int2(floor(hist_coord));
        float2 hist_frac  = fract(hist_coord);

        // Clamp base coordinates to valid range
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

    // If no valid history, output current upscaled value directly
    if (!valid_history) {
        ssgi_output.write(float4(current_ssgi, current_hit_dist), pixel_pos);
        return;
    }

    // -----------------------------------------------------------------------
    // Step 4: Variance Clipping (Mean + Sigma) on 3x3 neighborhood
    // -----------------------------------------------------------------------
    // Uses mean + N*sigma clipping instead of min/max AABB.
    // Min/max AABB is too sensitive to outliers (a single firefly expands it).
    // Mean+sigma is more robust and is the standard approach in UE5 TAA.

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

            float2 neighbor_uv = (float2(neighbor_pos) + 0.5) / float2(params.full_width, params.full_height);
            float2 half_uv     = neighbor_uv * float2(params.half_width, params.half_height);
            int2   half_texel  = clamp(int2(floor(half_uv)), int2(0),
                                       int2(int(params.half_width) - 1, int(params.half_height) - 1));

            float3 neighbor_ssgi = ssgi_trace.read(uint2(half_texel)).rgb;

            neighborhood_sum  += neighbor_ssgi;
            neighborhood_sum2 += neighbor_ssgi * neighbor_ssgi;
            neighborhood_count++;
        }
    }

    // Compute mean and standard deviation
    float3 mean     = neighborhood_sum / float(neighborhood_count);
    float3 variance = abs(neighborhood_sum2 / float(neighborhood_count) - mean * mean);
    float3 sigma    = sqrt(max(variance, float3(0.0)));

    // AABB = mean ± 2*sigma (captures ~95% of distribution)
    // Add a small floor to sigma to avoid degenerate AABB in flat regions
    float3 sigma_floor = max(mean * 0.1, float3(0.01));
    float3 aabb_min = mean - sigma * 2.0 - sigma_floor;
    float3 aabb_max = mean + sigma * 2.0 + sigma_floor;

    // Clamp history to the AABB
    float3 clamped_history = clamp(history_ssgi, aabb_min, aabb_max);

    // -----------------------------------------------------------------------
    // Step 5: Disocclusion detection + screen-edge fade
    // -----------------------------------------------------------------------
    // Compare current depth with history depth at reprojected position.
    // Use a generous threshold since NDC depth is non-linear (tight at near,
    // loose at far). Gradual blend avoids hard pop-in of noise.
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
    // Step 6: Exponential blend
    // -----------------------------------------------------------------------
    float effective_feedback = params.feedback * edge_fade * disocclusion_fade;

    float3 result_color = mix(current_ssgi, clamped_history, effective_feedback);
    float  result_dist  = mix(current_hit_dist, history_hit_dist, effective_feedback);

    // Output: RGB = accumulated irradiance, A = hit distance / confidence
    ssgi_output.write(float4(result_color, result_dist), pixel_pos);
}
