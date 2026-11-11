static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

cbuffer SSAOFilterParams : register(b5, space0)
{
    float params_sigma_depth : packoffset(c0);
    float params_sigma_normal : packoffset(c0.y);
    uint params_kernel_radius : packoffset(c0.z);
    uint params_full_width : packoffset(c0.w);
    uint params_full_height : packoffset(c1);
    uint params_half_width : packoffset(c1.y);
    uint params_half_height : packoffset(c1.z);
};

cbuffer GDBlock : register(b4, space0)
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

Texture2D<float4> ssao_input : register(t0, space0);
Texture2D<float4> gbuffer_depth : register(t2, space0);
RWTexture2D<float> ssao_out : register(u3, space0);
Texture2D<float4> gbuffer_normal : register(t1, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

float NDCDepthToLinear(float ndcDepth, float nearPlane, float farPlane)
{
    return ((2.0f * nearPlane) * farPlane) / ((farPlane + nearPlane) - (ndcDepth * (farPlane - nearPlane)));
}

float sampleHalfResBilinear(float2 uv)
{
    float2 hcoord = (uv * float2(float(params_half_width), float(params_half_height))) - 0.5f.xx;
    int2 p0 = int2(floor(hcoord));
    float2 f = frac(hcoord);
    p0 = clamp(p0, int2(0, 0), int2(int(params_half_width) - 1, int(params_half_height) - 1));
    int2 p1 = clamp(p0 + int2(1, 1), int2(0, 0), int2(int(params_half_width) - 1, int(params_half_height) - 1));
    float v00 = ssao_input.Load(int3(p0, 0)).x;
    float v10 = ssao_input.Load(int3(int2(p1.x, p0.y), 0)).x;
    float v01 = ssao_input.Load(int3(int2(p0.x, p1.y), 0)).x;
    float v11 = ssao_input.Load(int3(p1, 0)).x;
    float a = lerp(v00, v10, f.x);
    float b = lerp(v01, v11, f.x);
    return lerp(a, b, f.y);
}

void comp_main()
{
    uint2 pixel_pos = gl_GlobalInvocationID.xy;
    bool _164 = pixel_pos.x >= params_full_width;
    bool _174;
    if (!_164)
    {
        _174 = pixel_pos.y >= params_full_height;
    }
    else
    {
        _174 = _164;
    }
    if (_174)
    {
        return;
    }
    float center_depth = gbuffer_depth.Load(int3(int2(pixel_pos), 0)).x;
    if (center_depth >= 1.0f)
    {
        ssao_out[int2(pixel_pos)] = float4(1.0f, 0.0f, 0.0f, 0.0f).x;
        return;
    }
    float4 normal_encoded = gbuffer_normal.Load(int3(int2(pixel_pos), 0));
    float3 center_normal = normalize((normal_encoded.xyz * 2.0f) - 1.0f.xxx);
    float2 full_size = float2(float(params_full_width), float(params_full_height));
    float2 center_uv = (float2(pixel_pos) + 0.5f.xx) / full_size;
    float param = center_depth;
    float param_1 = 0.100000001490116119384765625f;
    float param_2 = 1000.0f;
    float center_linear = NDCDepthToLinear(param, param_1, param_2);
    float ao_sum = 0.0f;
    float weight_sum = 0.0f;
    int radius = int(params_kernel_radius);
    int _247 = -radius;
    for (int dy = _247; dy <= radius; dy++)
    {
        int _258 = -radius;
        for (int dx = _258; dx <= radius; dx++)
        {
            int2 sample_full = int2(pixel_pos) + int2(dx, dy);
            bool _276 = sample_full.x < 0;
            bool _286;
            if (!_276)
            {
                _286 = sample_full.x >= int(params_full_width);
            }
            else
            {
                _286 = _276;
            }
            bool _293;
            if (!_286)
            {
                _293 = sample_full.y < 0;
            }
            else
            {
                _293 = _286;
            }
            bool _303;
            if (!_293)
            {
                _303 = sample_full.y >= int(params_full_height);
            }
            else
            {
                _303 = _293;
            }
            if (_303)
            {
                continue;
            }
            float2 sample_uv = (float2(sample_full) + 0.5f.xx) / full_size;
            float2 param_3 = sample_uv;
            float ao_val = sampleHalfResBilinear(param_3);
            float4 s_normal_enc = gbuffer_normal.Load(int3(sample_full, 0));
            float3 sample_normal = normalize((s_normal_enc.xyz * 2.0f) - 1.0f.xxx);
            float sample_depth = gbuffer_depth.Load(int3(sample_full, 0)).x;
            float param_4 = sample_depth;
            float param_5 = 0.100000001490116119384765625f;
            float param_6 = 1000.0f;
            float sample_linear = NDCDepthToLinear(param_4, param_5, param_6);
            float depth_diff = abs(center_linear - sample_linear);
            float min_depth = max(min(center_linear, sample_linear), 0.001000000047497451305389404296875f);
            float relative_depth = depth_diff / min_depth;
            float w_depth = exp((-relative_depth) * params_sigma_depth);
            float normal_sim = max(dot(center_normal, sample_normal), 0.0f);
            float w_normal = pow(normal_sim, params_sigma_normal);
            float dist2 = float((dx * dx) + (dy * dy));
            float w_spatial = exp((-dist2) / ((2.0f * float(radius)) * float(radius)));
            float w = (w_depth * w_normal) * w_spatial;
            ao_sum += (ao_val * w);
            weight_sum += w;
        }
    }
    float _414;
    if (weight_sum > 0.0f)
    {
        _414 = ao_sum / weight_sum;
    }
    else
    {
        _414 = 1.0f;
    }
    float ao = _414;
    ao = clamp(ao, 0.0f, 1.0f);
    ssao_out[int2(pixel_pos)] = float4(ao, 0.0f, 0.0f, 0.0f).x;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
