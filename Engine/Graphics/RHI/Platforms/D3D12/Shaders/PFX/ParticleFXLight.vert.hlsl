// Vulkan reference: PFX/ParticleFXLight.vert (main).
static const float2 _79[6] = { (-1.0f).xx, float2(1.0f, -1.0f), float2(-1.0f, 1.0f), float2(-1.0f, 1.0f), float2(1.0f, -1.0f), 1.0f.xx };

ByteAddressBuffer lights : register(t0, space0);
cbuffer VP : register(b1, space0)
{
    row_major float4x4 vp_view_projection : packoffset(c0);
    float4 vp_pad : packoffset(c4);
};

struct StageInput
{
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
};

struct StageOutput
{
    float4 out_color : TEXCOORD0;
    float2 out_uv : TEXCOORD1;
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    uint base = uint(int(input.instanceID)) * 2u;
    float4 pos_intensity = asfloat(lights.Load4((base + 0u) * 16));
    float4 color_rgb_pad = asfloat(lights.Load4((base + 1u) * 16));
    float intensity = max(pos_intensity.w, 0.0f);
    float3 world = pos_intensity.xyz;
    float4 centerClip = mul(float4(world, 1.0f), vp_view_projection);
    float clipW = max(centerClip.w, 0.001f);
    float2 q = _79[int(input.vertexID)];
    float screenRadius = (0.02f * (1.0f + (intensity * 0.05f))) / clipW;
    centerClip.x += ((q.x * screenRadius) * clipW);
    centerClip.y += ((q.y * screenRadius) * clipW);
    output.positionCS = centerClip;
    output.out_color = float4(color_rgb_pad.xyz * intensity, 1.0f);
    output.out_uv = (q * 0.5f) + 0.5f.xx;
    return output;
}
