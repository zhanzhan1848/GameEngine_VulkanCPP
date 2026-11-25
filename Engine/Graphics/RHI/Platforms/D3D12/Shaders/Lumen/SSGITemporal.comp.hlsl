// Vulkan reference: Lumen/SSGITemporal.comp (main).
#include "../Common/TextureDimensions.hlsli"

cbuffer TemporalParams : register(b5, space0)
{
    float params_feedback : packoffset(c0);
    uint params_full_width : packoffset(c0.y);
    uint params_full_height : packoffset(c0.z);
    uint params_pad0 : packoffset(c0.w);
};

Texture2D<float4> gbuffer_depth : register(t3, space0);
Texture2D<float4> ssgi_spatial : register(t0, space0);
Texture2D<float4> velocity_buffer : register(t2, space0);
Texture2D<float4> ssgi_history : register(t1, space0);
RWTexture2D<float4> ssgi_output : register(u4, space0);

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

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint2 gid = input.dispatchThreadID.xy;
    int2 pixel_pos = int2(gid);
    if (((pixel_pos.x >= int(params_full_width)) || (pixel_pos.y >= int(params_full_height))))
    {
        return;
    }
    float2 pixel_uv = (float2(pixel_pos) + 0.5f.xx) / float2(float(params_full_width), float(params_full_height));
    float depth_ndc = gbuffer_depth.Load(int3(pixel_pos, 0)).x;
    float4 current_ssgi_4 = ssgi_spatial.Load(int3(pixel_pos, 0));
    float3 current_ssgi = current_ssgi_4.xyz;
    float current_hit_dist = current_ssgi_4.w;
    float2 velocity = velocity_buffer.Load(int3(pixel_pos, 0)).xy;
    float velMag = length(velocity);
    if (velMag < 0.002f)
    {
        velocity = 0.0f.xx;
    }
    float2 prev_uv;
    prev_uv.x = pixel_uv.x - velocity.x;
    prev_uv.y = pixel_uv.y + velocity.y;
    float2 edge_dist = min(prev_uv, 1.0f.xx - prev_uv);
    bool _256 = (((prev_uv.x >= 0.0f) && (prev_uv.x <= 1.0f)) && (prev_uv.y >= 0.0f)) && (prev_uv.y <= 1.0f);
    bool in_bounds = _256;
    float3 history_ssgi = 0.0f.xxx;
    float history_hit_dist = 0.0f;
    bool valid_history = false;
    if (in_bounds)
    {

        int2 histDims = int2(textureDimensions(ssgi_history, 0u));
        float2 param = prev_uv;
        int2 param_1 = histDims;
        float4 hist_bilinear = sampleBilinear(ssgi_history, param, param_1);
        history_ssgi = hist_bilinear.xyz;
        history_hit_dist = hist_bilinear.w;
        valid_history = true;
    }
    if (!valid_history)
    {
        ssgi_output[pixel_pos] = float4(current_ssgi, current_hit_dist);
        return;
    }
    float3 neighborhood_sum = current_ssgi;
    float3 neighborhood_sum2 = current_ssgi * current_ssgi;
    int neighborhood_count = 1;
    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            if ((dx == 0) && (dy == 0))
            {
                continue;
            }
            int2 neighbor_pos = pixel_pos + int2(dx, dy);
            if (((((neighbor_pos.x < 0) || (neighbor_pos.y < 0)) || (neighbor_pos.x >= int(params_full_width))) || (neighbor_pos.y >= int(params_full_height))))
            {
                continue;
            }
            float3 neighbor_ssgi = ssgi_spatial.Load(int3(neighbor_pos, 0)).xyz;
            neighborhood_sum += neighbor_ssgi;
            neighborhood_sum2 += (neighbor_ssgi * neighbor_ssgi);
            neighborhood_count++;
        }
    }
    float fcount = float(neighborhood_count);
    float3 mean = neighborhood_sum / fcount.xxx;
    float3 variance = abs((neighborhood_sum2 / fcount.xxx) - (mean * mean));
    float3 sigma = sqrt(max(variance, 0.0f.xxx));
    float3 sigma_floor = max(abs(mean) * 0.1f, 0.01f.xxx);
    float3 aabb_min = (mean - (sigma * 2.0f)) - sigma_floor;
    float3 aabb_max = (mean + (sigma * 2.0f)) + sigma_floor;
    float3 clamped_history = clamp(history_ssgi, aabb_min, aabb_max);
    float disocclusion_fade = 1.0f;
    if (in_bounds)
    {
        int2 hist_pixel = clamp(int2(prev_uv * float2(float(params_full_width), float(params_full_height))), int2(0, 0), int2(int(params_full_width) - 1, int(params_full_height) - 1));
        float hist_depth = gbuffer_depth.Load(int3(hist_pixel, 0)).x;
        float depth_diff = abs(depth_ndc - hist_depth);
        disocclusion_fade = clamp((0.02f - depth_diff) / 0.015f, 0.0f, 1.0f);
    }
    float edge_fade = smoothstep(0.0f, 0.05f, min(edge_dist.x, edge_dist.y));
    float hit_confidence = clamp(1.0f - (current_hit_dist / 2.0f), 0.0f, 1.0f);
    float confidence_scale = lerp(0.6f, 1.0f, hit_confidence);
    float effective_feedback = ((params_feedback * edge_fade) * disocclusion_fade) * confidence_scale;
    float3 result_color = lerp(current_ssgi, clamped_history, effective_feedback.xxx);
    float result_dist = lerp(current_hit_dist, history_hit_dist, effective_feedback);
    ssgi_output[pixel_pos] = float4(result_color, result_dist);
}
