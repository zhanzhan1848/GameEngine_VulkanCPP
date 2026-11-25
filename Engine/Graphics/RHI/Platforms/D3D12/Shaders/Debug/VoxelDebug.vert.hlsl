// Vulkan reference: Debug/VoxelDebug.vert (main).
static const float3 _178[36] = { float3(-0.5f, -0.5f, 0.5f), float3(0.5f, -0.5f, 0.5f), 0.5f.xxx, float3(-0.5f, -0.5f, 0.5f), 0.5f.xxx, float3(-0.5f, 0.5f, 0.5f), float3(0.5f, -0.5f, -0.5f), (-0.5f).xxx, float3(-0.5f, 0.5f, -0.5f), float3(0.5f, -0.5f, -0.5f), float3(-0.5f, 0.5f, -0.5f), float3(0.5f, 0.5f, -0.5f), float3(-0.5f, 0.5f, 0.5f), 0.5f.xxx, float3(0.5f, 0.5f, -0.5f), float3(-0.5f, 0.5f, 0.5f), float3(0.5f, 0.5f, -0.5f), float3(-0.5f, 0.5f, -0.5f), (-0.5f).xxx, float3(0.5f, -0.5f, -0.5f), float3(0.5f, -0.5f, 0.5f), (-0.5f).xxx, float3(0.5f, -0.5f, 0.5f), float3(-0.5f, -0.5f, 0.5f), float3(0.5f, -0.5f, 0.5f), float3(0.5f, -0.5f, -0.5f), float3(0.5f, 0.5f, -0.5f), float3(0.5f, -0.5f, 0.5f), float3(0.5f, 0.5f, -0.5f), 0.5f.xxx, (-0.5f).xxx, float3(-0.5f, -0.5f, 0.5f), float3(-0.5f, 0.5f, 0.5f), (-0.5f).xxx, float3(-0.5f, 0.5f, 0.5f), float3(-0.5f, 0.5f, -0.5f) };

cbuffer DebugUniforms : register(b1, space0)
{
    row_major float4x4 viewProjection : packoffset(c0);
    row_major float4x4 model : packoffset(c4);
    float scale : packoffset(c8);
    float threshold : packoffset(c8.y);
    float step_throttle : packoffset(c8.z);
    float pad0 : packoffset(c8.w);
    uint4 resolution : packoffset(c9);
};

Texture3D<float4> voxelTexture : register(t0, space0);
SamplerState linearSampler : register(s2, space0);

struct StageInput
{
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
};

struct StageOutput
{
    float3 vUVW : TEXCOORD0;
    float3 vColor : TEXCOORD1;
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    uint3 res = resolution.xyz;
    if ((((res.x == 0u) || (res.y == 0u)) || (res.z == 0u)))
    {
        output.positionCS = float4(0.0f, 0.0f, 0.0f, 1.0f);
        output.vUVW = 0.0f.xxx;
        output.vColor = 0.0f.xxx;
        return output;
    }
    uint actualID = uint(int(input.instanceID)) * uint(step_throttle);
    uint z = actualID / (res.x * res.y);
    uint temp = actualID % (res.x * res.y);
    uint y = temp / res.x;
    uint x = temp % res.x;
    if (z >= res.z)
    {
        output.positionCS = 0.0f.xxxx;
        output.vUVW = 0.0f.xxx;
        output.vColor = 0.0f.xxx;
        return output;
    }
    float3 uvw = (float3(float(x), float(y), float(z)) + 0.5f.xxx) / float3(res);
    output.vUVW = uvw;
    float density = voxelTexture.SampleLevel(linearSampler, uvw, 0.0f).x;
    if ((((isnan(density)) || (isinf(density))) || (density < threshold)))
    {
        output.positionCS = 0.0f.xxxx;
        output.vColor = 0.0f.xxx;
        return output;
    }
    float3 voxelSize = 1.0f.xxx / float3(res);
    float3 vertPos = _178[int(input.vertexID)];
    float3 center = (float3(float(x), float(y), float(z)) + 0.5f.xxx) * voxelSize;
    float3 pos = center + ((vertPos * voxelSize) * scale);
    float3 modelSpacePos = pos - 0.5f.xxx;
    float4 worldPos = mul(float4(modelSpacePos, 1.0f), model);
    output.positionCS = mul(worldPos, viewProjection);
    output.vColor = density.xxx;
    return output;
}
