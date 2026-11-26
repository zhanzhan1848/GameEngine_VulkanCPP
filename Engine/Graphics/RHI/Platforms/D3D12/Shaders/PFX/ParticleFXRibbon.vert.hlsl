// Vulkan reference: PFX/ParticleFXRibbon.vert (main).
ByteAddressBuffer strip : register(t0, space0);
cbuffer VP : register(b1, space0)
{
    row_major float4x4 vp_view_projection : packoffset(c0);
    float4 vp_color : packoffset(c4);
};

struct StageInput
{
    uint vertexID : SV_VertexID;
};

struct StageOutput
{
    float2 out_uv : TEXCOORD0;
    float4 out_color : TEXCOORD1;
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    uint base = uint(int(input.vertexID)) * 2u;
    float4 pos_uv = asfloat(strip.Load4((base + 0u) * 16));
    float4 uv_pad = asfloat(strip.Load4((base + 1u) * 16));
    output.positionCS = mul(float4(pos_uv.xyz, 1.0f), vp_view_projection);
    output.out_uv = float2(pos_uv.w, uv_pad.x);
    output.out_color = vp_color;
    return output;
}
