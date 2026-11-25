// Vulkan reference: ToneMapping.spv (tonemap_fs).
struct VertexOutput
{
    float4 position;
    float2 uv;
};

struct ToneParams
{
    float exposure;
    float autoAdapt;
    float key;
    float contrast;
    float saturation;
    float vignette;
    float bloomIntensity;
    uint _pad1;
};

static const float4 _75[3] = { float4(-1.0f, -1.0f, 0.0f, 1.0f), float4(3.0f, -1.0f, 0.0f, 1.0f), float4(-1.0f, 3.0f, 0.0f, 1.0f) };
static const float2 _80[3] = { float2(0.0f, 1.0f), float2(2.0f, 1.0f), float2(0.0f, -1.0f) };
static const VertexOutput _83 = { 0.0f.xxxx, 0.0f.xx };

cbuffer params : register(b6, space0)
{
    ToneParams params_1_m0 : packoffset(c0);
};

Texture2D<float4> sceneTexture : register(t0, space0);
Texture2D<float4> bloomTexture : register(t1, space0);
SamplerState texSampler : register(s2, space0);
Texture2D<float4> aoTexture : register(t3, space0);
Texture2D<float4> ssgiTexture : register(t4, space0);
Texture2D<float4> velocityTexture : register(t5, space0);
Texture2D<float4> adaptState : register(t7, space0);

static float2 uv;

struct StageInput
{
    float2 uv : TEXCOORD0;
};

struct StageOutput
{
    float4 _133 : SV_Target0;
};

float3 ACESFilm(float3 x)
{
    return clamp((x * ((x * 2.51f) + 0.03f.xxx)) / ((x * ((x * 2.43f) + 0.59f.xxx)) + 0.14f.xxx), 0.0f.xxx, 1.0f.xxx);
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    uv = input.uv;
    float3 result = 0.0f.xxx;
    bool _164 = false;
    bool _170 = false;
    float3 color = 0.0f.xxx;
    float exposure = 0.0f;
    if (false)
    {
        output._133 = float4(abs(velocityTexture.Sample(texSampler, uv).xy) * 20.0f, 0.0f, 1.0f);
        return output;
    }
    color = sceneTexture.Sample(texSampler, uv).xyz;
    float3 _191 = abs(color);
    if (!any(bool3(color.x != color.x, color.y != color.y, color.z != color.z)))
    {
        _164 = any(bool3(_191.x > 3.4e+38f.xxx.x, _191.y > 3.4e+38f.xxx.y, _191.z > 3.4e+38f.xxx.z));
    }
    else
    {
        _164 = true;
    }
    if (_164)
    {
        color = 0.0f.xxx;
    }
    float4 _203 = bloomTexture.Sample(texSampler, uv);
    exposure = params_1_m0.exposure;
    if (params_1_m0.autoAdapt > 0.5f)
    {
        float _214 = exp2(adaptState.Load(int3(int2(0, 0), 0)).x);
        if (_214 > 1e-06f)
        {
            _170 = _214 < 1000000015047466219876688855040.0f;
        }
        else
        {
            _170 = false;
        }
        if (_170)
        {
            exposure = (exposure * params_1_m0.key) / _214;
        }
    }
    result = (color + (_203.xyz * params_1_m0.bloomIntensity)) * exposure;
    result = ACESFilm(result);
    result = pow(result, 0.45454547f.xxx);
    if (params_1_m0.contrast != 1.0f)
    {
        result = clamp(((result - 0.5f.xxx) * params_1_m0.contrast) + 0.5f.xxx, 0.0f.xxx, 1.0f.xxx);
    }
    if (params_1_m0.saturation != 1.0f)
    {
        result = lerp(dot(result, float3(0.2126f, 0.7152f, 0.0722f)).xxx, result, params_1_m0.saturation.xxx);
    }
    if (params_1_m0.vignette > 0.0f)
    {
        result *= (1.0f - (params_1_m0.vignette * smoothstep(0.35f, 0.85f, distance(uv, 0.5f.xx))));
    }
    output._133 = float4(result, 1.0f);
    return output;
}
