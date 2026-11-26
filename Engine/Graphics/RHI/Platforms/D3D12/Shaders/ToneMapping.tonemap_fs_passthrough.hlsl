// Vulkan reference: ToneMapping.spv (tonemap_fs_passthrough).
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
    float4 _109 : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    uv = input.uv;
    output._109 = float4((sceneTexture.Sample(texSampler, uv).xyz + bloomTexture.Sample(texSampler, uv).xyz) * params_1_m0.exposure, 1.0f);
    return output;
}
