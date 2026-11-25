// Vulkan reference: Debug/VoxelDebug.frag (main).
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
SamplerState nearestSampler : register(s3, space0);

struct StageInput
{
    float3 vUVW : TEXCOORD0;
    float3 vColor : TEXCOORD1;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float density = voxelTexture.Sample(nearestSampler, input.vUVW).x;
    if (density < threshold)
    {
        discard;
    }
    output.outColor = float4(input.vColor, 1.0f);
    return output;
}
