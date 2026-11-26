// Vulkan reference: Debug/VectorFieldDebug.vert (main).
cbuffer DebugUniforms : register(b1, space0)
{
    row_major float4x4 viewProjection : packoffset(c0);
    row_major float4x4 model : packoffset(c4);
    float scale : packoffset(c8);
    float step_throttle : packoffset(c8.y);
    float2 pad0 : packoffset(c8.z);
    uint4 resolution : packoffset(c9);
};

Texture3D<float4> vfTexture : register(t0, space0);
SamplerState linearSampler : register(s2, space0);

struct StageInput
{
    uint vertexID : SV_VertexID;
};

struct StageOutput
{
    float3 vColor : TEXCOORD0;
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    uint vertexID = uint(int(input.vertexID));
    uint lineIndex = vertexID / 2u;
    uint endpoint = vertexID % 2u;
    uint actualLineIndex = lineIndex * uint(step_throttle);
    uint3 res = resolution.xyz;
    if ((((res.x == 0u) || (res.y == 0u)) || (res.z == 0u)))
    {
        output.positionCS = 0.0f.xxxx;
        output.vColor = 0.0f.xxx;
        return output;
    }
    uint z = actualLineIndex / (res.x * res.y);
    uint temp = actualLineIndex % (res.x * res.y);
    uint y = temp / res.x;
    uint x = temp % res.x;
    if (z >= res.z)
    {
        output.positionCS = 0.0f.xxxx;
        output.vColor = 0.0f.xxx;
        return output;
    }
    float3 uvw = (float3(float(x), float(y), float(z)) + 0.5f.xxx) / float3(res);
    float3 _vector = vfTexture.SampleLevel(linearSampler, uvw, 0.0f).xyz;
    if (((((isnan(_vector.x)) || (isnan(_vector.y))) || (isnan(_vector.z))) || (length(_vector) < 0.001f)))
    {
        output.positionCS = 0.0f.xxxx;
        output.vColor = 0.0f.xxx;
        return output;
    }
    float3 modelSpacePos = uvw - 0.5f.xxx;
    if (endpoint == 1u)
    {
        modelSpacePos += (_vector * scale);
    }
    float4 worldPos = mul(float4(modelSpacePos, 1.0f), model);
    output.positionCS = mul(worldPos, viewProjection);
    output.vColor = abs(_vector);
    return output;
}
