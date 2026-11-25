// Vulkan reference: Debug/MeshletDebug.frag (main).
cbuffer DebugUniforms : register(b0, space0)
{
    row_major float4x4 viewProjection : packoffset(c0);
    row_major float4x4 model : packoffset(c4);
    uint mesh_id : packoffset(c8);
    float wireframe_enabled : packoffset(c8.y);
    float4 pad0 : packoffset(c9);
};

struct StageInput
{
    float3 vColor : TEXCOORD0;
    float vAlpha : TEXCOORD1;
    float3 vBarycentric : TEXCOORD2;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 color = input.vColor;
    if (wireframe_enabled > 0.5f)
    {
        float3 d = fwidth(input.vBarycentric);
        float3 a3 = smoothstep(0.0f.xxx, d * 1.5f, input.vBarycentric);
        float minBary = min(min(a3.x, a3.y), a3.z);
        color = lerp(0.0f.xxx, color, minBary.xxx);
    }
    output.outColor = float4(color, input.vAlpha);
    return output;
}
