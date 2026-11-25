// Vulkan reference: Lumen/SSGIFilter.comp (main).
#include "../Common/TextureDimensions.hlsli"

cbuffer FilterParams : register(b3, space0)
{
    float params_sigma_depth : packoffset(c0);
    float params_sigma_normal : packoffset(c0.y);
    float params_sigma_hit_dist : packoffset(c0.z);
    float params_sigma_spatial : packoffset(c0.w);
    uint params_kernel_radius : packoffset(c1);
};

Texture2D<float4> gbuffer_depth : register(t1, space0);
RWTexture2D<float4> ssgi_output : register(u2, space0);
Texture2D<float4> ssgi_input : register(t0, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float4 sampleBilinear(Texture2D<float4> tex, float2 uv, int2 dims)
{
    float2 coord = (uv * float2(dims)) - 0.5f.xx;
    int2 base = int2(int(floor(coord.x)), int(floor(coord.y)));
    float2 f = frac(coord);
    int2 maxCoord = dims - int2(1, 1);
    int2 c00 = clamp(base, int2(0, 0), maxCoord);
    int2 c10 = clamp(base + int2(1, 0), int2(0, 0), maxCoord);
    int2 c01 = clamp(base + int2(0, 1), int2(0, 0), maxCoord);
    int2 c11 = clamp(base + int2(1, 1), int2(0, 0), maxCoord);
    float4 h00 = tex.Load(int3(c00, 0));
    float4 h10 = tex.Load(int3(c10, 0));
    float4 h01 = tex.Load(int3(c01, 0));
    float4 h11 = tex.Load(int3(c11, 0));
    return ((((h00 * (1.0f - f.x)) * (1.0f - f.y)) + ((h10 * f.x) * (1.0f - f.y))) + ((h01 * (1.0f - f.x)) * f.y)) + ((h11 * f.x) * f.y);
}

float2 depthGradient(int2 pixel) {
    float dC = gbuffer_depth.Load(int3(pixel, 0)).r;
    float dR = gbuffer_depth.Load(int3(pixel + int2(1, 0), 0)).r;
    float dD = gbuffer_depth.Load(int3(pixel + int2(0, 1), 0)).r;
    return float2(dR - dC, dD - dC);
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint2 gid = input.dispatchThreadID.xy;
    int2 pixel_pos = int2(gid);

    int2 outDims = int2(imageDimensions(ssgi_output));
    if (((pixel_pos.x >= outDims.x) || (pixel_pos.y >= outDims.y)))
    {
        return;
    }
    uint width = uint(outDims.x);
    uint height = uint(outDims.y);
    float2 invRes = 1.0f.xx / float2(float(width), float(height));
    float2 center_uv = (float2(pixel_pos) + 0.5f.xx) * invRes;

    int2 inputDims = int2(textureDimensions(ssgi_input, 0u));
    float2 param = center_uv;
    int2 param_1 = inputDims;
    float4 center_ssgi = sampleBilinear(ssgi_input, param, param_1);
    float center_depth = gbuffer_depth.Load(int3(pixel_pos, 0)).x;
    float2 center_grad = depthGradient(pixel_pos);
    float3 center_irradiance = center_ssgi.xyz;
    float center_hit_dist = center_ssgi.w;
    uint radius = min(params_kernel_radius, 4u);
    float3 filtered_irradiance = 0.0f.xxx;
    float filtered_hit_dist = 0.0f;
    float total_weight = 0.0f;
    int _266 = -int(radius);
    float w_normal;
    for (int dy = _266; dy <= int(radius); dy++)
    {
        int _279 = -int(radius);
        for (int dx = _279; dx <= int(radius); dx++)
        {
            int2 sample_pos = pixel_pos + int2(dx, dy);
            if (((((sample_pos.x < 0) || (sample_pos.y < 0)) || (sample_pos.x >= int(width))) || (sample_pos.y >= int(height))))
            {
                continue;
            }
            float2 sample_uv = (float2(sample_pos) + 0.5f.xx) * invRes;
            float2 param_3 = sample_uv;
            int2 param_4 = inputDims;
            float4 sample_ssgi = sampleBilinear(ssgi_input, param_3, param_4);
            float sample_depth = gbuffer_depth.Load(int3(sample_pos, 0)).x;
            float2 sample_grad = depthGradient(sample_pos);
            float depth_diff = abs(center_depth - sample_depth);
            float w_depth = exp((-depth_diff) * params_sigma_depth);
            float gLenC = length(center_grad);
            float gLenS = length(sample_grad);
            if ((gLenC < 1e-05f) && (gLenS < 1e-05f))
            {
                w_normal = 1.0f;
            }
            else
            {
                float ng = max(0.0f, dot(center_grad, sample_grad) / max(gLenC * gLenS, 1e-08f));
                w_normal = pow(ng, params_sigma_normal);
            }
            float dist_diff = abs(center_hit_dist - sample_ssgi.w);
            float w_dist = exp((-dist_diff) * params_sigma_hit_dist);
            float adaptive_sigma = max((params_sigma_spatial * center_hit_dist) * 0.5f, params_sigma_spatial * 0.5f);
            float spatial_dist2 = float((dx * dx) + (dy * dy));
            float w_spatial = exp((-spatial_dist2) / ((2.0f * adaptive_sigma) * adaptive_sigma));
            float weight = ((w_depth * w_normal) * w_dist) * w_spatial;
            filtered_irradiance += (sample_ssgi.xyz * weight);
            filtered_hit_dist += (sample_ssgi.w * weight);
            total_weight += weight;
        }
    }
    if (total_weight > 1e-06f)
    {
        filtered_irradiance /= total_weight.xxx;
        filtered_hit_dist /= total_weight;
    }
    else
    {
        filtered_irradiance = center_irradiance;
        filtered_hit_dist = center_hit_dist;
    }
    ssgi_output[pixel_pos] = float4(filtered_irradiance, filtered_hit_dist);
}
