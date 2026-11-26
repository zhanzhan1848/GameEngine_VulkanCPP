// Vulkan reference: Forward/GBufferVT.frag (main).
cbuffer VTMaterialParams : register(b5, space1)
{
    float4 vt_p0 : packoffset(c0);
    float4 vt_p1 : packoffset(c1);
    float4 vt_p2 : packoffset(c2);
    float4 vt_p3 : packoffset(c3);
    float4 vt_p4 : packoffset(c4);
};

RWByteAddressBuffer feedback : register(u6, space1);
Texture2D<uint4> vtPageTable : register(t0, space1);
Texture2D<float4> atlas0 : register(t1, space1);
SamplerState vtSampler : register(s4, space1);
Texture2D<float4> atlas1 : register(t2, space1);
Texture2D<float4> atlas2 : register(t3, space1);

static float4 fragCoord;
static float2 inUV;

struct StageInput
{
    float3 inWorldPos : TEXCOORD0;
    float3 inWorldNormal : TEXCOORD1;
    float3 inWorldTangent : TEXCOORD2;
    float2 inUV : TEXCOORD3;
    float4 inCurrentPos : TEXCOORD4;
    float4 inPreviousPos : TEXCOORD5;
    float4 inInstanceBaseColor : TEXCOORD6;
    float inInstanceRoughness : TEXCOORD7;
    float inInstanceMetallic : TEXCOORD8;
    float inInstanceTransmission : TEXCOORD9;
    float4 fragCoord : SV_Position;
};

struct StageOutput
{
    float4 outAlbedo : SV_Target0;
    float4 outNormal : SV_Target1;
    float4 outORM : SV_Target2;
    float2 outVelocity : SV_Target3;
};

float4 vtSampleLayer(float4 lp, uint vtId, int atlasIndex, uint feedbackSlot)
{
    float tileSize = vt_p3.x;
    float atlasInv = vt_p3.y;
    uint pagesTop = uint(lp.z);
    uint maxLevel = uint(log2(float(pagesTop)));
    float virtualSize = float(pagesTop) * tileSize;
    float2 dx = ddx(inUV * virtualSize);
    float2 dy = ddy(inUV * virtualSize);
    float levF = 0.5f * log2(max(dot(dx, dx), dot(dy, dy)));
    uint level = uint(clamp(levF, 0.0f, float(maxLevel)));
    uint s = max((pagesTop >> level), 1u);
    float2 f = clamp(inUV, 0.0f.xx, 0.99999f.xx) * float(s);
    uint2 vPage = min(uint2(f), (s - 1u).xx);
    feedback.Store(feedbackSlot * 4 + 0, (((vtId << 28u) | (level << 24u)) | ((vPage.y & 4095u) << 12u)) | (vPage.x & 4095u));
    uint entry = 0u;
    for (uint k = 0u; (level + k) <= maxLevel; k++)
    {
        int2 c = int2(int(lp.x), int(lp.y)) + int2(vPage >> k.xx);
        entry = vtPageTable.Load(int3(c, 0)).x;
        if ((entry & 1u) != 0u)
        {
            break;
        }
    }
    if ((entry & 1u) == 0u)
    {
        return lp.w.xxxx;
    }
    uint ox = (entry >> 8u) & 2047u;
    uint oy = (entry >> 19u) & 2047u;
    float2 frac = f - float2(vPage);
    float2 pUV = ((float2(float(ox), float(oy)) + 4.0f.xx) + (frac * tileSize)) * atlasInv;
    if (atlasIndex == 0)
    {
        return atlas0.Sample(vtSampler, pUV);
    }
    if (atlasIndex == 1)
    {
        return atlas1.Sample(vtSampler, pUV);
    }
    return atlas2.Sample(vtSampler, pUV);
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    fragCoord = input.fragCoord;
    fragCoord.w = 1.0 / fragCoord.w;
    inUV = input.inUV;
    uint px = uint(fragCoord.x) + (uint(fragCoord.y) * uint(vt_p4.w));
    float4 param = vt_p0;
    uint param_1 = uint(vt_p4.x);
    int param_2 = 0;
    uint param_3 = (px * 3u) + 0u;
    float4 _283 = vtSampleLayer(param, param_1, param_2, param_3);
    float4 albedoSample = _283;
    float4 param_4 = vt_p1;
    uint param_5 = uint(vt_p4.y);
    int param_6 = 1;
    uint param_7 = (px * 3u) + 1u;
    float4 _297 = vtSampleLayer(param_4, param_5, param_6, param_7);
    float4 normalSample = _297;
    float4 param_8 = vt_p2;
    uint param_9 = uint(vt_p4.z);
    int param_10 = 2;
    uint param_11 = (px * 3u) + 2u;
    float4 _312 = vtSampleLayer(param_8, param_9, param_10, param_11);
    float4 ormSample = _312;
    output.outAlbedo = float4(albedoSample.xyz * input.inInstanceBaseColor.xyz, albedoSample.w * input.inInstanceBaseColor.w);
    if (length(ormSample.xyz) < 0.01f)
    {
        output.outORM = float4(1.0f, input.inInstanceRoughness, input.inInstanceMetallic, input.inInstanceTransmission);
    }
    else
    {
        output.outORM = float4(ormSample.x, ormSample.y * input.inInstanceRoughness, ormSample.z * input.inInstanceMetallic, input.inInstanceTransmission);
        output.outORM.x = max(output.outORM.x, 0.1f);
    }
    if (length(normalSample.xyz) > 0.1f)
    {
        float3 normal = (normalSample.xyz * 2.0f) - 1.0f.xxx;
        float3 N = normalize(input.inWorldNormal);
        float3 T = normalize(input.inWorldTangent);
        float3 B = cross(N, T);
        float3x3 TBN = float3x3(float3(T), float3(B), float3(N));
        output.outNormal = float4((normalize(mul(normal, TBN)) * 0.5f) + 0.5f.xxx, 1.0f);
    }
    else
    {
        output.outNormal = float4((normalize(input.inWorldNormal) * 0.5f) + 0.5f.xxx, 1.0f);
    }
    float2 currentNDC = input.inCurrentPos.xy / input.inCurrentPos.w.xx;
    float2 previousNDC = input.inPreviousPos.xy / input.inPreviousPos.w.xx;
    output.outVelocity = (currentNDC - previousNDC) * 0.5f;
    return output;
}
