// Vulkan reference: PFX/ParticleFXDecal.vert (main).
static const float2 _54[6] = { (-1.0f).xx, float2(1.0f, -1.0f), float2(-1.0f, 1.0f), float2(-1.0f, 1.0f), float2(1.0f, -1.0f), 1.0f.xx };

ByteAddressBuffer decals : register(t1, space0);

struct StageInput
{
    uint vertexID : SV_VertexID;
    uint instanceID : SV_InstanceID;
};

struct StageOutput
{
    float2 out_uv : TEXCOORD0;
    float out_radius : TEXCOORD1;
    float4 out_center : TEXCOORD2;
    float4 out_color : TEXCOORD3;
    float out_softness : TEXCOORD4;
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    uint base = uint(int(input.instanceID)) * 3u;
    float4 center_radius = asfloat(decals.Load4((base + 0u) * 16));
    float4 color = asfloat(decals.Load4((base + 1u) * 16));
    float4 params = asfloat(decals.Load4((base + 2u) * 16));
    float2 q = _54[int(input.vertexID)];
    output.positionCS = float4(q, 0.0f, 1.0f);
    output.out_uv = (q * 0.5f) + 0.5f.xx;
    output.out_radius = max(center_radius.w, 0.0001f);
    output.out_center = center_radius;
    output.out_color = color;
    output.out_softness = params.x;
    return output;
}
