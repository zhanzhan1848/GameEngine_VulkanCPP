static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

cbuffer GDBlock : register(b3, space0)
{
    row_major float4x4 gd_view : packoffset(c0);
    row_major float4x4 gd_projection : packoffset(c4);
    row_major float4x4 gd_invProjection : packoffset(c8);
    row_major float4x4 gd_viewProjection : packoffset(c12);
    row_major float4x4 gd_previousViewProjection : packoffset(c16);
    row_major float4x4 gd_invViewProjection : packoffset(c20);
    float4 gd_cameraPositionAndViewWidth : packoffset(c24);
    float4 gd_cameraDirectionAndViewHeight : packoffset(c25);
    uint gd_numDirectionalLights : packoffset(c26);
    uint gd_numPunctualLights : packoffset(c26.y);
    float gd_deltaTime : packoffset(c26.z);
    float gd_frameCount : packoffset(c26.w);
    uint gd_renderMode : packoffset(c27);
    uint gd_enableIBL : packoffset(c27.y);
    uint gd_enableDDGI : packoffset(c27.z);
    uint gd_pad_before_jitter : packoffset(c27.w);
    float2 gd_jitterOffset : packoffset(c28);
    float gd_debug_directLightBoost : packoffset(c28.z);
    float gd_debug_iblStrength : packoffset(c28.w);
    float gd_debug_ddgiIndirectWeight : packoffset(c29);
    float gd_debug_exposure : packoffset(c29.y);
    float gd_pad_after_debug_0 : packoffset(c29.z);
    float gd_pad_after_debug_1 : packoffset(c29.w);
};

cbuffer SSAOTraceParams : register(b4, space0)
{
    float params_radius : packoffset(c0);
    float params_power : packoffset(c0.y);
    uint params_direction_count : packoffset(c0.z);
    uint params_sample_count : packoffset(c0.w);
    uint params_frame_index : packoffset(c1);
    uint params_output_width : packoffset(c1.y);
    uint params_output_height : packoffset(c1.z);
    float params_near_plane : packoffset(c1.w);
    float params_far_plane : packoffset(c2);
};

Texture2D<float4> gbuffer_depth : register(t1, space0);
RWTexture2D<float> ssao_out : register(u2, space0);
Texture2D<float4> gbuffer_normal : register(t0, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

float3 ReconstructViewPosAO(float2 uv, float depth_ndc)
{
    float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (uv.y * 2.0f));
    float4 clipPos = float4(ndc, depth_ndc, 1.0f);
    float4 viewPosH = mul(clipPos, gd_invProjection);
    float3 viewPos = viewPosH.xyz / viewPosH.w.xxx;
    viewPos.z = -viewPos.z;
    return viewPos;
}

float InterleavedGradientNoise(uint2 pixel, uint frameIndex)
{
    float3 magic = float3(0.067110560834407806396484375f, 0.005837149918079376220703125f, 52.98291778564453125f);
    float2 _noise = float2(pixel) + (float2(float(frameIndex % 5u), float((frameIndex / 5u) % 5u)) * 4.0f);
    return frac(magic.z * frac(dot(_noise, magic.xy)));
}

float hashStep(uint2 pixel, uint sliceIdx, uint stepIdx)
{
    uint n = (((pixel.x * 374761393u) + (pixel.y * 668265263u)) + (sliceIdx * 1274126177u)) + (stepIdx * 1911520717u);
    n = (n ^ (n >> 13u)) * 1274126177u;
    return float(n & 65535u) / 65535.0f;
}

void comp_main()
{
    uint2 pixel_pos = gl_GlobalInvocationID.xy;
    bool _161 = pixel_pos.x >= params_output_width;
    bool _171;
    if (!_161)
    {
        _171 = pixel_pos.y >= params_output_height;
    }
    else
    {
        _171 = _161;
    }
    if (_171)
    {
        return;
    }
    uint2 full_pixel = pixel_pos * uint2(2u, 2u);
    float2 full_size = float2(gd_cameraPositionAndViewWidth.w, gd_cameraDirectionAndViewHeight.w);
    float2 uv = (float2(full_pixel) + 0.5f.xx) / full_size;
    float depth_ndc = gbuffer_depth.Load(int3(int2(full_pixel), 0)).x;
    if (depth_ndc >= 1.0f)
    {
        ssao_out[int2(pixel_pos)] = float4(1.0f, 0.0f, 0.0f, 0.0f).x;
        return;
    }
    float4 normal_encoded = gbuffer_normal.Load(int3(int2(full_pixel), 0));
    float3 normal_vs = normalize((normal_encoded.xyz * 2.0f) - 1.0f.xxx);
    float2 param = uv;
    float param_1 = depth_ndc;
    float3 view_pos = ReconstructViewPosAO(param, param_1);
    float3 view_dir = normalize(-view_pos);
    float bias = 0.004999999888241291046142578125f * abs(view_pos.z);
    float3 biased_pos = view_pos + (normal_vs * bias);
    float3 view_ray = normalize(view_pos);
    uint2 param_2 = pixel_pos;
    uint param_3 = 0u;
    float _noise = InterleavedGradientNoise(param_2, param_3);
    float visibility = 0.0f;
    for (uint slice = 0u; slice < params_direction_count; slice++)
    {
        float slice_angle = ((float(slice) + _noise) * 3.1415927410125732421875f) / float(params_direction_count);
        float2 slice_dir = float2(cos(slice_angle), sin(slice_angle));
        float step_size = params_radius / max(abs(view_pos.z), 0.00999999977648258209228515625f);
        step_size = min(step_size, 0.20000000298023223876953125f);
        float horizon_pos = -1.0f;
        float horizon_neg = -1.0f;
        for (uint _step = 1u; _step <= params_sample_count; _step++)
        {
            uint2 param_4 = pixel_pos;
            uint param_5 = slice;
            uint param_6 = _step;
            float jitter = hashStep(param_4, param_5, param_6);
            float t = ((float(_step) - 0.5f) + jitter) / float(params_sample_count);
            float2 sample_uv = uv + ((slice_dir * step_size) * t);
            bool _344 = sample_uv.x > 0.0f;
            bool _350;
            if (_344)
            {
                _350 = sample_uv.x < 1.0f;
            }
            else
            {
                _350 = _344;
            }
            bool _356;
            if (_350)
            {
                _356 = sample_uv.y > 0.0f;
            }
            else
            {
                _356 = _350;
            }
            bool _362;
            if (_356)
            {
                _362 = sample_uv.y < 1.0f;
            }
            else
            {
                _362 = _356;
            }
            if (_362)
            {
                int2 sp = int2(clamp(sample_uv * full_size, 0.0f.xx, full_size - 1.0f.xx));
                float sd = gbuffer_depth.Load(int3(sp, 0)).x;
                if (sd < 1.0f)
                {
                    float2 param_7 = sample_uv;
                    float param_8 = sd;
                    float3 sv = ReconstructViewPosAO(param_7, param_8);
                    float3 delta = sv - biased_pos;
                    float len = length(delta);
                    if (len > 0.001000000047497451305389404296875f)
                    {
                        float falloff = 1.0f - clamp((len * len) / (params_radius * params_radius), 0.0f, 1.0f);
                        float thickness_w = 1.0f;
                        if (sd < depth_ndc)
                        {
                            thickness_w = clamp(1.0f - ((depth_ndc - sd) * 30.0f), 0.0f, 1.0f);
                        }
                        float cos_a = dot(delta, view_dir) / len;
                        float weighted = lerp(-1.0f, cos_a, falloff * thickness_w);
                        horizon_pos = max(horizon_pos, weighted);
                    }
                }
            }
            float2 sample_uv_1 = uv - ((slice_dir * step_size) * t);
            bool _453 = sample_uv_1.x > 0.0f;
            bool _459;
            if (_453)
            {
                _459 = sample_uv_1.x < 1.0f;
            }
            else
            {
                _459 = _453;
            }
            bool _465;
            if (_459)
            {
                _465 = sample_uv_1.y > 0.0f;
            }
            else
            {
                _465 = _459;
            }
            bool _471;
            if (_465)
            {
                _471 = sample_uv_1.y < 1.0f;
            }
            else
            {
                _471 = _465;
            }
            if (_471)
            {
                int2 sp_1 = int2(clamp(sample_uv_1 * full_size, 0.0f.xx, full_size - 1.0f.xx));
                float sd_1 = gbuffer_depth.Load(int3(sp_1, 0)).x;
                if (sd_1 < 1.0f)
                {
                    float2 param_9 = sample_uv_1;
                    float param_10 = sd_1;
                    float3 sv_1 = ReconstructViewPosAO(param_9, param_10);
                    float3 delta_1 = sv_1 - biased_pos;
                    float len_1 = length(delta_1);
                    if (len_1 > 0.001000000047497451305389404296875f)
                    {
                        float falloff_1 = 1.0f - clamp((len_1 * len_1) / (params_radius * params_radius), 0.0f, 1.0f);
                        float thickness_w_1 = 1.0f;
                        if (sd_1 < depth_ndc)
                        {
                            thickness_w_1 = clamp(1.0f - ((depth_ndc - sd_1) * 30.0f), 0.0f, 1.0f);
                        }
                        float cos_a_1 = dot(delta_1, view_dir) / len_1;
                        float weighted_1 = lerp(-1.0f, cos_a_1, falloff_1 * thickness_w_1);
                        horizon_neg = max(horizon_neg, weighted_1);
                    }
                }
            }
        }
        float3 slice_3d = float3(slice_dir.x, slice_dir.y, 0.0f);
        float3 slice_normal = normalize(cross(view_ray, slice_3d));
        float3 proj = normal_vs - (slice_normal * dot(normal_vs, slice_normal));
        float pl = length(proj);
        if (pl > 0.001000000047497451305389404296875f)
        {
            proj /= pl.xxx;
        }
        else
        {
            proj = view_dir;
        }
        float n_cos = dot(proj, view_dir);
        float h1 = clamp(horizon_pos - n_cos, -1.0f, 1.0f);
        float h2 = clamp(horizon_neg - n_cos, -1.0f, 1.0f);
        visibility += ((0.25f * (((-h2) * sqrt(max(1.0f - (h2 * h2), 0.0f))) + acos(clamp(h2, -1.0f, 1.0f)))) + (0.25f * ((h1 * sqrt(max(1.0f - (h1 * h1), 0.0f))) + acos(clamp(h1, -1.0f, 1.0f)))));
    }
    float ao = clamp(visibility / float(params_direction_count), 0.0f, 1.0f);
    ao = pow(ao, params_power);
    ao = max(ao, 0.100000001490116119384765625f);
    ssao_out[int2(pixel_pos)] = float4(ao, 0.0f, 0.0f, 0.0f).x;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
