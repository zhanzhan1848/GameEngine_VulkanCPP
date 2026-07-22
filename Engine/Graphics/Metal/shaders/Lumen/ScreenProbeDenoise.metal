/**
 * @file ScreenProbeDenoise.metal
 * @brief Screen Probe GI - Confidence-weighted bilateral filter
 *
 * Light bilateral filter on GI output, using confidence from Gather:
 * - High confidence pixels are trusted (less filtered)
 * - Low confidence pixels borrow from high-confidence neighbors
 * - Geometry constraints (depth + normal) prevent bleeding across edges
 *
 * Weights: confidence × depth × normal × spatial
 *
 * Dispatch: (renderW + 7) / 8, (renderH + 7) / 8, 1
 * ThreadGroupSize: (8, 8, 1)
 */

#include <metal_stdlib>
using namespace metal;

struct DenoiseParams {
    float sigma_depth;     // Depth weight sharpness
    float sigma_normal;    // Normal weight power
    float sigma_spatial;   // Gaussian spatial falloff
    float pad1;
    uint  kernel_radius;
    uint  render_width;
    uint  render_height;
    uint  pad2;
};

static inline float3 decodeNormal(float4 encoded) {
    return normalize(encoded.xyz * 2.0f - 1.0f);
}

kernel void screen_probe_denoise(
    uint2 pixel_pos [[thread_position_in_grid]],

    texture2d<float, access::sample> giInput        [[texture(0)]],
    texture2d<float, access::sample> normalTexture   [[texture(1)]],
    texture2d<float, access::read>   depthTexture    [[texture(2)]],
    texture2d<float, access::write>  giOutput        [[texture(3)]],

    constant DenoiseParams& params [[buffer(0)]])
{
    uint width = params.render_width;
    uint height = params.render_height;

    if (pixel_pos.x >= width || pixel_pos.y >= height) return;

    float2 invRes = 1.0f / float2(width, height);
    float2 centerUV = (float2(pixel_pos) + 0.5f) * invRes;

    sampler bilinear(coord::normalized, filter::linear, address::clamp_to_edge);

    float4 centerGI     = giInput.sample(bilinear, centerUV);
    float  centerDepth  = depthTexture.read(pixel_pos).r;
    float3 centerNormal = decodeNormal(normalTexture.sample(bilinear, centerUV));

    // Early out for sky pixels
    if (centerDepth <= 0.0001f || centerDepth >= 0.999f) {
        giOutput.write(float4(0.0f, 0.0f, 0.0f, 0.0f), pixel_pos);
        return;
    }

    // Early out for zero GI — pass through without filtering
    float centerLum = dot(centerGI.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    if (centerLum < 0.0001f) {
        giOutput.write(centerGI, pixel_pos);
        return;
    }

    // Light bilateral filter — geometry weights only, no confidence influence
    uint radius = min(params.kernel_radius, 2u);

    float3 filteredGI = float3(0.0f);
    float  totalWeight = 0.0f;

    for (int dy = -int(radius); dy <= int(radius); dy++) {
        for (int dx = -int(radius); dx <= int(radius); dx++) {
            int2 samplePos = int2(pixel_pos) + int2(dx, dy);

            if (samplePos.x < 0 || samplePos.y < 0 ||
                samplePos.x >= int(width) || samplePos.y >= int(height)) {
                continue;
            }

            float2 sampleUV = (float2(samplePos) + 0.5f) * invRes;
            float4 sampleGI = giInput.sample(bilinear, sampleUV);

            float  sampleDepth  = depthTexture.read(uint2(samplePos)).r;
            float3 sampleNormal = decodeNormal(normalTexture.sample(bilinear, sampleUV));

            // Depth weight
            float depthDiff = abs(centerDepth - sampleDepth);
            float w_depth = exp(-depthDiff * params.sigma_depth);

            // Normal weight
            float NdotN = max(0.0f, dot(centerNormal, sampleNormal));
            float w_normal = pow(NdotN, params.sigma_normal);

            // Gaussian spatial weight
            float spatialDist2 = float(dx * dx + dy * dy);
            float w_spatial = exp(-spatialDist2 / (2.0f * params.sigma_spatial * params.sigma_spatial));

            float weight = w_depth * w_normal * w_spatial;

            filteredGI += sampleGI.rgb * weight;
            totalWeight += weight;
        }
    }

    if (totalWeight > 1e-6f) {
        filteredGI /= totalWeight;
    } else {
        filteredGI = centerGI.rgb;
    }

    // Pass through confidence from input (for temporal use downstream)
    giOutput.write(float4(filteredGI, centerGI.a), pixel_pos);
}
