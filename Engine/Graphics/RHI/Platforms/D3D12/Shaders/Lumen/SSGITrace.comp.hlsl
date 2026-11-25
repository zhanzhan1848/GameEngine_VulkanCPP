// Vulkan reference: Lumen/SSGITrace.comp (ssgi_trace).
#include "../Common/TextureDimensions.hlsli"

cbuffer GlobalShaderData : register(b5, space0)
{
    row_major float4x4 gd_View : packoffset(c0);
    row_major float4x4 gd_Projection : packoffset(c4);
    row_major float4x4 gd_InvProjection : packoffset(c8);
    row_major float4x4 gd_ViewProjection : packoffset(c12);
    row_major float4x4 gd_PreviousViewProjection : packoffset(c16);
    row_major float4x4 gd_InvViewProjection : packoffset(c20);
    float4 gd_CameraPositionAndViewWidth : packoffset(c24);
    float4 gd_CameraDirectionAndViewHeight : packoffset(c25);
    uint gd_NumDirectionalLights : packoffset(c26);
    float gd_DeltaTime : packoffset(c26.y);
    float gd_FrameCount : packoffset(c26.z);
    uint gd_padding : packoffset(c26.w);
};

cbuffer SSGIParams : register(b6, space0)
{
    uint params_ray_count : packoffset(c0);
    float params_radius : packoffset(c0.y);
    float params_thickness : packoffset(c0.z);
    uint params_frame_index : packoffset(c0.w);
    uint params_output_width : packoffset(c1);
    uint params_output_height : packoffset(c1.y);
    float params_near_plane : packoffset(c1.z);
    float params_far_plane : packoffset(c1.w);
    uint params_hzb_mip_levels : packoffset(c2);
    float params_max_trace_distance : packoffset(c2.y);
};

Texture2D<float4> hzb_texture : register(t2, space0);
Texture2D<float4> gbuffer_depth : register(t1, space0);
RWTexture2D<float4> ssgi_output : register(u4, space0);
Texture2D<float4> gbuffer_normal : register(t0, space0);
Texture2D<float4> prev_frame_color : register(t3, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float NDCDepthToLinear(float ndcDepth, float nearPlane, float farPlane) {
    return nearPlane * farPlane / (farPlane - ndcDepth * (farPlane - nearPlane));
}

float3 screenToView(float2 uv, float ndcDepth)
{
    float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (2.0f * uv.y));
    float4 clipPos = float4(ndc, ndcDepth, 1.0f);
    float4 viewPos = mul(clipPos, gd_InvProjection);
    return viewPos.xyz / viewPos.w.xxx;
}

float hash(float2 p) {
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float3 cosineHemisphereSample(float3 N, float2 seed, int sampleIndex, uint frameIndex, uint rayCount)
{
    float xi1 = frac((float(sampleIndex) + 0.5f) / float(rayCount));
    float2 param = seed + float2(float(sampleIndex) * 0.13f, float(sampleIndex) * 0.91f);
    float xi2 = frac(hash(param));
    float phi = 6.2831855f * xi1;
    float cosTheta = sqrt(max(xi2, 0.001f));
    float sinTheta = sqrt(1.0f - (cosTheta * cosTheta));
    float3 localDir = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    bool3 _215 = (abs(N.z) < 0.999f).xxx;
    float3 up = float3(_215.x ? float3(0.0f, 0.0f, 1.0f).x : float3(1.0f, 0.0f, 0.0f).x, _215.y ? float3(0.0f, 0.0f, 1.0f).y : float3(1.0f, 0.0f, 0.0f).y, _215.z ? float3(0.0f, 0.0f, 1.0f).z : float3(1.0f, 0.0f, 0.0f).z);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);
    float3 result = ((tangent * localDir.x) + (bitangent * localDir.y)) + (N * localDir.z);
    if (dot(result, N) < 0.0f)
    {
        result = -result;
    }
    return normalize(result);
}

float3 viewToScreen(float3 viewPos)
{
    float4 clipPos = mul(float4(viewPos, 1.0f), gd_Projection);
    float3 ndc = clipPos.xyz / clipPos.w.xxx;
    float2 uv = float2((ndc.x * 0.5f) + 0.5f, 1.0f - ((ndc.y * 0.5f) + 0.5f));
    return float3(uv, ndc.z);
}

bool traceRayHZB(float2 origin_uv, float3 ray_origin_view, float3 ray_dir_view, inout float2 hit_uv, inout float hit_depth)
{
    float baseStep = params_max_trace_distance / 64.0f;
    float t = 0.0f;
    uint mip = 1u;
    for (uint _step = 0u; _step < 64u; _step++)
    {
        float mipScale = float(1u << mip);
        t += (baseStep * mipScale);
        if (t > params_max_trace_distance)
        {
            break;
        }
        float3 ray_pos = ray_origin_view + (ray_dir_view * t);
        float3 param = ray_pos;
        float3 screen = viewToScreen(param);
        float2 sample_uv = screen.xy;
        if (((((sample_uv.x < 0.0f) || (sample_uv.x > 1.0f)) || (sample_uv.y < 0.0f)) || (sample_uv.y > 1.0f)))
        {
            break;
        }
        if (ray_pos.z > (-params_near_plane))
        {
            continue;
        }

        int2 mipSize = int2(textureDimensions(hzb_texture, uint(int(mip))));
        if (((mipSize.x == 0) || (mipSize.y == 0)))
        {
            break;
        }
        float2 mipCoordF = sample_uv * float2(mipSize);
        int2 mipCoord = clamp(int2(mipCoordF), int2(0, 0), mipSize - int2(1, 1));
        float sceneDepthNDC = hzb_texture.Load(int3(mipCoord, int(mip))).x;
        float param_1 = sceneDepthNDC;
        float param_2 = params_near_plane;
        float param_3 = params_far_plane;
        float sceneDepthLinear = NDCDepthToLinear(param_1, param_2, param_3);
        float rayDepthLinear = -ray_pos.z;
        if (rayDepthLinear > sceneDepthLinear)
        {
            if (mip == 0u)
            {
                if (rayDepthLinear <= (sceneDepthLinear + params_thickness))
                {
                    hit_uv = sample_uv;
                    hit_depth = sceneDepthLinear;
                    return true;
                }
            }
            else
            {
                t -= (baseStep * mipScale);
                mip--;
            }
        }
        else
        {
            mip = min((mip + 1u), (params_hzb_mip_levels - 1u));
        }
    }
    return false;
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint2 half_res_pos = input.dispatchThreadID.xy;
    if (((half_res_pos.x >= params_output_width) || (half_res_pos.y >= params_output_height)))
    {
        return;
    }
    uint2 full_res_pos = half_res_pos * uint2(2u, 2u);

    int2 fullSize = int2(textureDimensions(gbuffer_depth, 0u));
    if (((int(full_res_pos.x) >= fullSize.x) || (int(full_res_pos.y) >= fullSize.y)))
    {
        ssgi_output[int2(half_res_pos)] = 0.0f.xxxx;
        return;
    }
    float4 normal_enc = gbuffer_normal.Load(int3(int2(full_res_pos), 0));
    float3 world_normal = normalize((normal_enc.xyz * 2.0f) - 1.0f.xxx);
    float3 surface_normal = normalize(mul(world_normal, float3x3(gd_View[0].xyz, gd_View[1].xyz, gd_View[2].xyz)));
    float depth_ndc = gbuffer_depth.Load(int3(int2(full_res_pos), 0)).x;
    float param = depth_ndc;
    float param_1 = params_near_plane;
    float param_2 = params_far_plane;
    float linear_depth = NDCDepthToLinear(param, param_1, param_2);
    if (((linear_depth < params_near_plane) || (linear_depth > (params_far_plane * 0.999f))))
    {
        ssgi_output[int2(half_res_pos)] = 0.0f.xxxx;
        return;
    }
    float2 pixel_uv = (float2(full_res_pos) + 0.5f.xx) / float2(fullSize);
    float2 param_3 = pixel_uv;
    float param_4 = depth_ndc;
    float3 view_pos = screenToView(param_3, param_4);
    view_pos.z = -abs(view_pos.z);
    float3 view_normal = surface_normal;
    float3 total_irradiance = 0.0f.xxx;
    float total_hit_dist = 0.0f;
    uint hit_count = 0u;
    float2 base_seed = float2(float(full_res_pos.x) * 0.001f, float(full_res_pos.y) * 0.0017f);
    uint rayCount = max(params_ray_count, 1u);
    float2 param_13;
    float param_14;
    for (uint ray_idx = 0u; ray_idx < rayCount; ray_idx++)
    {
        float3 param_5 = view_normal;
        float2 param_6 = base_seed;
        int param_7 = int(ray_idx);
        uint param_8 = params_frame_index;
        float3 ray_dir = cosineHemisphereSample(param_5, param_6, param_7, param_8, rayCount);
        float3 ray_origin = view_pos + (view_normal * 0.05f);
        float2 hit_uv = 0.0f.xx;
        float hit_depth_val = 0.0f;
        float2 param_10 = pixel_uv;
        float3 param_11 = ray_origin;
        float3 param_12 = ray_dir;
        bool _646 = traceRayHZB(param_10, param_11, param_12, param_13, param_14);
        hit_uv = param_13;
        hit_depth_val = param_14;
        if (_646)
        {
            if (((((hit_uv.x >= 0.0f) && (hit_uv.x <= 1.0f)) && (hit_uv.y >= 0.0f)) && (hit_uv.y <= 1.0f)))
            {

                int2 colorSize = int2(textureDimensions(prev_frame_color, 0u));
                float2 colorCoord = hit_uv * float2(colorSize);
                int2 ci0 = clamp(int2(floor(colorCoord)), int2(0, 0), colorSize - int2(1, 1));
                int2 ci1 = clamp(ci0 + int2(1, 1), int2(0, 0), colorSize - int2(1, 1));
                float2 f = frac(colorCoord);
                float3 c00 = prev_frame_color.Load(int3(ci0, 0)).xyz;
                float3 c10 = prev_frame_color.Load(int3(int2(ci1.x, ci0.y), 0)).xyz;
                float3 c01 = prev_frame_color.Load(int3(int2(ci0.x, ci1.y), 0)).xyz;
                float3 c11 = prev_frame_color.Load(int3(ci1, 0)).xyz;
                float3 hit_color = lerp(lerp(c00, c10, f.x.xxx), lerp(c01, c11, f.x.xxx), f.y.xxx);
                float3 clamped = min(hit_color, 2.0f.xxx);
                float hitLum = dot(clamped, float3(0.2126f, 0.7152f, 0.0722f));
                float brightPenalty = 1.0f / (1.0f + (max(hitLum - 1.0f, 0.0f) * 2.0f));
                clamped *= brightPenalty;
                float distAttenuation = 1.0f - smoothstep(params_max_trace_distance * 0.7f, params_max_trace_distance, hit_depth_val);
                float2 edgeDist = min(hit_uv, 1.0f.xx - hit_uv);
                float edgeFade = smoothstep(0.0f, 0.15f, min(edgeDist.x, edgeDist.y));
                total_irradiance += (((clamped * 3.1415927f) * distAttenuation) * edgeFade);
                total_hit_dist += hit_depth_val;
                hit_count++;
            }
        }
        else
        {
            total_hit_dist += params_max_trace_distance;
        }
    }
    float avg_hit_dist = total_hit_dist / float(rayCount);
    float3 output_irradiance = total_irradiance / float(rayCount).xxx;
    ssgi_output[int2(half_res_pos)] = float4(output_irradiance, avg_hit_dist);
}
