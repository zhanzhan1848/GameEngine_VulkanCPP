/**
 * @file SSGITrace.metal
 * @brief Lumen SSGI Phase 1 - HZB-based screen space GI ray tracing
 *
 * Half-resolution compute shader that:
 *  1. Reads GBuffer normal + depth at each pixel
 *  2. Casts multiple rays in a hemisphere around the surface normal
 *  3. Traces each ray through the HZB depth pyramid (hierarchical ray marching)
 *  4. On hit, samples previous frame scene color to gather indirect irradiance
 *  5. Outputs RGBA16F: RGB = accumulated irradiance, A = average hit distance
 */

#include <metal_stdlib>
using namespace metal;

#include "CommonTypes.metal"
#include "Common.h"

constant float LUMEN_PI = 3.14159265358979323846f;

// ============================================================================
// SSGI Parameters
// ============================================================================

struct SSGIParams {
    uint  ray_count;         // Number of rays per pixel (default 4)
    float radius;            // World-space sampling radius (default 2.0)
    float thickness;         // Surface thickness for depth test (default 0.25)
    uint  frame_index;       // Frame index for temporal rotation
    uint  output_width;      // Half-resolution output width
    uint  output_height;     // Half-resolution output height
    float near_plane;        // Near clip plane distance
    float far_plane;         // Far clip plane distance
    uint  hzb_mip_levels;    // Number of mip levels in HZB pyramid
};

// ============================================================================
// Constants
// ============================================================================

constant uint   SSGI_MAX_STEPS    = 16;
constant float  SSGI_MAX_DISTANCE = 20.0f;
constant float  SSGI_MAX_RADIANCE = 2.0f;  // Clamp to suppress direct light fireflies

// ============================================================================
// Helper: View-space position to screen UV
// ============================================================================

static float3 viewToScreen(float3 viewPos, float4x4 projection)
{
    float4 clipPos = projection * float4(viewPos, 1.0);
    float3 ndc = clipPos.xyz / clipPos.w;

    // Metal NDC: Y is up, texture UV: Y is down
    float2 uv;
    uv.x = ndc.x * 0.5 + 0.5;
    uv.y = 0.5 - 0.5 * ndc.y;

    return float3(uv, ndc.z);
}

// ============================================================================
// Helper: Screen UV + NDC depth to view-space position
// ============================================================================

static float3 screenToView(float2 uv, float ndcDepth, device GlobalShaderData& gd)
{
    // UV -> NDC
    float2 ndc;
    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = 1.0 - 2.0 * uv.y; // Metal Y flip

    float4 clipPos = float4(ndc.x, ndc.y, ndcDepth, 1.0);
    float4 viewPos = gd.InvProjection * clipPos;
    return viewPos.xyz / viewPos.w;
}

// ============================================================================
// Cosine-weighted hemisphere sampling with temporal rotation
// ============================================================================

static float3 cosineHemisphereSample(float3 N, float2 seed, int sampleIndex,
                                      uint frameIndex, uint rayCount)
{
    // Golden-ratio based low-discrepancy sequence
    // xi1 (phi): rotates per frame via golden ratio
    // xi2 (elevation): rotates per frame via sqrt(2)/2 to break correlation with xi1
    //   — Cranley-Patterson style: deterministic spatial hash + per-frame offset
    float xi1 = fract((float(sampleIndex) + 0.5) / float(rayCount)
                      + float(frameIndex) * 0.618033988749f);
    float xi2 = fract(hash(seed + float2(float(sampleIndex) * 0.13f,
                                          float(sampleIndex) * 0.91f))
                      + float(frameIndex) * 0.7071067811865476f);

    // Cosine-weighted hemisphere: cosTheta = sqrt(xi2)
    float phi      = 2.0f * LUMEN_PI * xi1;
    float cosTheta = sqrt(max(xi2, 0.001f));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    float3 localDir = float3(sinTheta * cos(phi),
                              sinTheta * sin(phi),
                              cosTheta);

    // Build TBN basis from normal
    float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f)
                                    : float3(1.0f, 0.0f, 0.0f);
    float3 tangent   = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    float3 result = tangent * localDir.x + bitangent * localDir.y + N * localDir.z;

    // Ensure the result is in the positive hemisphere
    if (dot(result, N) < 0.0f) {
        result = -result;
    }

    return normalize(result);
}

// ============================================================================
// HZB Hierarchical Ray Marching
// ============================================================================

/**
 * @brief Trace a ray through the HZB depth pyramid using hierarchical stepping
 *
 * The HZB stores MIN depth per mip level (closest depth in each region).
 *
 * Algorithm (UE5/Frostbite-style HZB ray march):
 *  - Step size is proportional to 2^mip — coarser mips take larger steps
 *  - When ray passes behind surface (potential hit): don't advance, refine mip
 *  - When ray is in front of surface: advance and optionally go coarser
 *  - When confirmed at mip 0: report intersection
 *
 * This fully utilizes the HZB's hierarchical structure for O(log N) traversal.
 *
 * @return true if a hit was found (hit_uv and hit_depth are populated)
 */
static bool traceRayHZB(float2 origin_uv,
                         float3 ray_origin_view,
                         float3 ray_dir_view,
                         texture2d<float, access::read> hzb_texture,
                         float4x4 projection,
                         constant SSGIParams& params,
                         thread float2& hit_uv,
                         thread float& hit_depth)
{
    // Base step: world-space distance per step at mip 0
    float baseStep = params.radius / float(SSGI_MAX_STEPS);

    float t   = 0.0f;
    uint  mip = 1; // Start at mip 1 for faster initial traversal

    for (uint step = 0; step < SSGI_MAX_STEPS; ++step) {
        // Hierarchical step: scale by 2^mip for coarser levels
        float mipScale = float(1u << mip);
        t += baseStep * mipScale;

        // Clamp to max radius
        if (t > params.radius) {
            break;
        }

        // Current ray position in view space
        float3 ray_pos = ray_origin_view + ray_dir_view * t;

        // Project to screen UV
        float3 screen = viewToScreen(ray_pos, projection);
        float2 sample_uv = screen.xy;

        // Out of screen check
        if (sample_uv.x < 0.0f || sample_uv.x > 1.0f ||
            sample_uv.y < 0.0f || sample_uv.y > 1.0f) {
            break;
        }

        // Behind camera: view-space Z > -near_plane means too close / behind
        if (ray_pos.z > -params.near_plane) {
            continue;
        }

        // Sample HZB at current mip level
        uint mip_w = hzb_texture.get_width(mip);
        uint mip_h = hzb_texture.get_height(mip);

        if (mip_w == 0 || mip_h == 0) {
            break;
        }

        uint2 mip_coord = uint2(sample_uv * float2(mip_w, mip_h));
        mip_coord = clamp(mip_coord, uint2(0), uint2(mip_w - 1, mip_h - 1));

        float scene_depth_ndc    = hzb_texture.read(mip_coord, mip).r;
        float scene_depth_linear = NDCDepthToLinear(scene_depth_ndc,
                                                     params.near_plane,
                                                     params.far_plane);
        // Use view-space Z (consistent with NDCDepthToLinear output)
        float ray_depth_linear = -ray_pos.z;

        // Ray is behind the closest surface → potential hit
        if (ray_depth_linear > scene_depth_linear) {
            if (mip == 0) {
                // Check thickness: only report hit if ray hasn't passed completely
                // through the surface (within assumed surface thickness)
                if (ray_depth_linear <= scene_depth_linear + params.thickness) {
                    hit_uv    = sample_uv;
                    hit_depth = scene_depth_linear;
                    return true;
                }
                // Ray passed through thin geometry — treat as miss, continue marching
            } else {
                // Refine: step back and re-evaluate at finer mip level
                t -= baseStep * mipScale;
                mip--;
            }
        }
        // Ray is in front of surface -> safe to go coarser for faster traversal
        else {
            mip = min(mip + 1, params.hzb_mip_levels - 1);
        }
    }

    return false;
}

// ============================================================================
// Main kernel: SSGI Trace
// ============================================================================

kernel void ssgi_trace(
    uint3 global_id [[thread_position_in_grid]],

    texture2d<float, access::read>  gbuffer_normal   [[texture(0)]],
    depth2d<float, access::read>    gbuffer_depth    [[texture(1)]],
    texture2d<float, access::read>  hzb_texture      [[texture(2)]],
    texture2d<float, access::sample> prev_frame_color [[texture(3)]],
    texture2d<float, access::write> ssgi_output      [[texture(4)]],

    device GlobalShaderData&   gd     [[buffer(0)]],
    constant SSGIParams&      params [[buffer(1)]]
)
{
    // Half-resolution dispatch boundary check
    uint2 half_res_pos = global_id.xy;
    if (half_res_pos.x >= params.output_width || half_res_pos.y >= params.output_height) {
        return;
    }

    // Map half-resolution pixel to full-resolution GBuffer coordinates
    uint2 full_res_pos = half_res_pos * 2;

    // Full-resolution texture dimensions
    uint full_width  = gbuffer_depth.get_width();
    uint full_height = gbuffer_depth.get_height();

    // Boundary check against full-res textures
    if (full_res_pos.x >= full_width || full_res_pos.y >= full_height) {
        ssgi_output.write(float4(0.0f), half_res_pos);
        return;
    }

    // -----------------------------------------------------------------------
    // Read GBuffer: Normal
    // -----------------------------------------------------------------------
    // Normal is encoded as normalize(N) * 0.5 + 0.5 in BGRA8_UNorm
    float4 normal_encoded = gbuffer_normal.read(full_res_pos);
    float3 surface_normal = normalize(normal_encoded.xyz * 2.0f - 1.0f);

    // -----------------------------------------------------------------------
    // Read GBuffer: Depth
    // -----------------------------------------------------------------------
    float depth_ndc    = gbuffer_depth.read(full_res_pos);
    float linear_depth = NDCDepthToLinear(depth_ndc, params.near_plane, params.far_plane);

    // Discard sky / invalid pixels (too close or too far)
    if (linear_depth < params.near_plane || linear_depth > params.far_plane * 0.999f) {
        ssgi_output.write(float4(0.0f), half_res_pos);
        return;
    }

    // -----------------------------------------------------------------------
    // Reconstruct view-space position
    // -----------------------------------------------------------------------
    float2 pixel_uv = (float2(full_res_pos) + 0.5f) / float2(full_width, full_height);
    float3 view_pos = ReconstructViewPos(pixel_uv, linear_depth, gd);

    // CRITICAL: ReconstructViewPos returns abs(Z) for "Metal left-hand convention",
    // but HZB ray trace uses right-handed view space (negative Z = forward).
    // Without this fix, ray_pos.z > -near_plane is always true, skipping ALL trace steps.
    view_pos.z = -view_pos.z;

    // Compute view-space normal (transform world normal to view space)
    // The GBuffer normal is in view space already in this engine (based on
    // existing SSGI/SSAO usage). If it were world-space, we would apply:
    //   float3 view_normal = normalize((float3x3)gd.View * surface_normal);
    float3 view_normal = surface_normal;

    // -----------------------------------------------------------------------
    // Cache projection matrix to avoid repeated device buffer reads
    // -----------------------------------------------------------------------
    float4x4 cachedProjection = gd.Projection;

    // -----------------------------------------------------------------------
    // Ray casting and accumulation
    // -----------------------------------------------------------------------
    float3 total_irradiance = float3(0.0f);
    float  total_hit_dist   = 0.0f;
    uint   hit_count        = 0;

    // Seed for randomness: pixel position + frame index
    float2 base_seed = float2(float(full_res_pos.x) * 0.001f + float(params.frame_index) * 0.01f,
                               float(full_res_pos.y) * 0.0017f + float(params.frame_index) * 0.013f);

    uint rayCount = max(params.ray_count, 1u);

    for (uint ray_idx = 0; ray_idx < rayCount; ++ray_idx) {
        // Generate a cosine-weighted direction in the hemisphere around the normal
        float3 ray_dir = cosineHemisphereSample(view_normal, base_seed,
                                                 int(ray_idx),
                                                 params.frame_index,
                                                 rayCount);

        // Offset ray origin along normal to prevent self-intersection
        float3 ray_origin = view_pos + view_normal * 0.05f;

        // Trace through HZB
        float2 hit_uv    = float2(0.0f);
        float  hit_depth = 0.0f;

        bool foundHit = traceRayHZB(pixel_uv, ray_origin, ray_dir,
                                     hzb_texture, cachedProjection, params,
                                     hit_uv, hit_depth);

        if (foundHit) {
            // Compute hit coordinate in the previous frame color texture
            uint prev_width  = prev_frame_color.get_width();
            uint prev_height = prev_frame_color.get_height();

            // Bilinear sampling: use sub-pixel coordinate
            float2 prev_uv = hit_uv;

            // Boundary check
            if (prev_uv.x >= 0.0f && prev_uv.x <= 1.0f &&
                prev_uv.y >= 0.0f && prev_uv.y <= 1.0f) {

                // Hardware bilinear sampling
                constexpr sampler colorS(coord::normalized, filter::linear, address::clamp_to_edge);
                float4 hit_color = prev_frame_color.sample(colorS, prev_uv);

                // Cosine-weighted sampling already accounts for the cos(N,L) term in the PDF.
                // For Lambertian BRDF: integral ≈ (1/N) * Σ radiance * PI
                // (albedo applied separately in the lighting pass)

                // Clamp radiance to suppress fireflies from HDR highlights / emissive surfaces
                float3 clamped_radiance = min(hit_color.rgb, float3(SSGI_MAX_RADIANCE));

                // Bright pixel dimming: reduce contribution from likely direct light hits
                // Bright surfaces (>1.0 luminance) are progressively dimmed
                float hitLum = dot(clamped_radiance, float3(0.2126f, 0.7152f, 0.0722f));
                float brightPenalty = 1.0f / (1.0f + max(hitLum - 1.0f, 0.0f) * 2.0f);
                clamped_radiance *= brightPenalty;

                // Distance attenuation: fade out hits that are too far
                float distAttenuation = 1.0f - smoothstep(params.radius * 0.5f,
                                                            params.radius,
                                                            hit_depth);

                // Edge fade: reduce contribution near screen borders
                float2 edgeDist = min(hit_uv, 1.0f - hit_uv);
                float edgeFade  = smoothstep(0.0f, 0.15f, min(edgeDist.x, edgeDist.y));

                total_irradiance += clamped_radiance * LUMEN_PI * distAttenuation * edgeFade;
                total_hit_dist   += hit_depth;
                hit_count++;
            }
        } else {
            // No hit: use max distance as fallback for average distance calculation
            total_hit_dist += params.radius;
        }
    }

    // -----------------------------------------------------------------------
    // Normalize output
    // -----------------------------------------------------------------------
    float avg_hit_dist = total_hit_dist / float(rayCount);

    // Scale irradiance by 1/N for energy conservation
    float3 output_irradiance = total_irradiance / float(rayCount);

    // Output: RGB = irradiance, A = average hit distance (for spatial filter weighting)
    ssgi_output.write(float4(output_irradiance, avg_hit_dist), half_res_pos);
}
