#include <metal_stdlib>
using namespace metal;

// VT M2.14 — GBuffer 的 Virtual Texture 采样变体(Metal)。
// 仅含 fragmentMain;vertexMain 复用 GBuffer.metal(管线两侧函数可来自不同库)。
// 与 GBufferVT.frag(Vulkan)语义一致:两段式采样 + 导数 mip + 逐级回退 +
// 片元级 feedback 三层写入。绑定与 material_set_vt_layout_ 对应:
//   texture(0) 页表(R32_UINT)  texture(1/2/3) atlas 0/1/2
//   sampler(4)                  buffer(5) 参数 UBO  buffer(6) feedback

#define RHI_ENABLE_PBR
#include "RHIShaderCommon.metal"

const float InvIntervals = 2.f / ((1 << 16) - 1);

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
    float3 worldTangent;
    float3 worldBitangent;
    float2 uv;
    float4 currentPos;
    float4 previousPos;
    float4 instanceBaseColor;
    float  instanceRoughness;
    float  instanceMetallic;
};

struct FragmentOut {
    float4 albedo [[color(0)]];
    float4 normal [[color(1)]];
    float4 orm [[color(2)]];
    float2 velocity [[color(3)]];
};

struct SceneData {
    float4x4 model;
    float4 lightPos;
    float4 lightColor;
    float4 reflectionPlane;
    float4 reflectionPlane2;
    float4 reflectionPlane3;
    float4x4 previousModel;
    float2 jitter;
    float2 previousJitter;
    float  time;
    float  _timePad;
    float4 viewPos;
    float4x4 shadowMatrix0;
    float4x4 shadowMatrix1;
};

struct PushConsts {
    float4x4 model;
    uint use_instances;
    uint _pad[3];
};

#include "InstanceData.metal"

struct VTMaterialParams {
    float4 p0;   // albedo: x=ptOriginX y=ptOriginY z=pagesTop w=avgColor
    float4 p1;   // normal 同构
    float4 p2;   // orm    同构
    float4 p3;   // x=tileSize y=atlasSizeInv z=uvScaleX w=uvScaleY
    float4 p4;   // x=vtId0 y=vtId1 z=vtId2 w=feedbackWidth
};

static inline float4 vtSampleLayer(VertexOut in,
                                   texture2d<uint, access::sample> pageTable,
                                   texture2d<float, access::sample> atlas,
                                   sampler samp,
                                   constant VTMaterialParams& vt,
                                   float4 lp, uint vtId, uint feedbackSlot,
                                   device uint* feedback) {
    float tileSize = vt.p3.x;
    float atlasInv = vt.p3.y;
    uint pagesTop = uint(lp.z);
    uint maxLevel = uint(log2(float(pagesTop)));

    // 导数驱动 mip(quad 内一致;M3 各向异性)
    float virtualSize = float(pagesTop) * tileSize;
    float2 duvdx = dfdx(in.uv * virtualSize);
    float2 duvdy = dfdy(in.uv * virtualSize);
    float levF = 0.5 * log2(max(dot(duvdx, duvdx), dot(duvdy, duvdy)));
    uint level = uint(clamp(levF, 0.0f, float(maxLevel)));
    uint s = max(pagesTop >> level, 1u);
    float2 f = clamp(in.uv, 0.0f, 0.99999f) * float(s);
    uint2 vPage = min(uint2(f), uint2(s - 1u));

    feedback[feedbackSlot] = (vtId << 28) | (level << 24)
                           | ((vPage.y & 0xFFFu) << 12) | (vPage.x & 0xFFFu);

    uint entry = 0u;
    for (uint k = 0; (level + k) <= maxLevel; ++k) {
        ushort2 c = ushort2(uint2(uint(lp.x), uint(lp.y)) + (vPage >> k));
        entry = pageTable.read(c, 0).r;
        if ((entry & 1u) != 0u) break;
    }
    if ((entry & 1u) == 0u) {
        return float4(lp.w);
    }
    uint ox = (entry >> 8) & 0x7FFu;
    uint oy = (entry >> 19) & 0x7FFu;
    float2 fracPx = f - float2(vPage);
    float2 pUV = (float2(float(ox), float(oy)) + 4.0f + fracPx * tileSize) * atlasInv;
    return atlas.sample(samp, pUV);
}

fragment FragmentOut fragmentMain(
    VertexOut in [[stage_in]],
    constant SceneData& sceneData [[buffer(1)]],
    texture2d<uint, access::sample> pageTable [[texture(0)]],
    texture2d<float, access::sample> atlas0   [[texture(1)]],
    texture2d<float, access::sample> atlas1   [[texture(2)]],
    texture2d<float, access::sample> atlas2   [[texture(3)]],
    sampler vtSampler [[sampler(4)]],
    constant VTMaterialParams& vt [[buffer(5)]],
    device uint* feedback [[buffer(6)]])
{
    uint px = uint(in.position.x) + uint(in.position.y) * uint(vt.p4.w);
    float4 albedoSample = vtSampleLayer(in, pageTable, atlas0, vtSampler, vt,
                                        vt.p0, uint(vt.p4.x), px * 3u + 0u, feedback);
    float4 normalSample = vtSampleLayer(in, pageTable, atlas1, vtSampler, vt,
                                        vt.p1, uint(vt.p4.y), px * 3u + 1u, feedback);
    float4 ormSample    = vtSampleLayer(in, pageTable, atlas2, vtSampler, vt,
                                        vt.p2, uint(vt.p4.z), px * 3u + 2u, feedback);

    FragmentOut out;
    out.albedo = float4(albedoSample.rgb * in.instanceBaseColor.rgb,
                        albedoSample.a * in.instanceBaseColor.a);

    if (length(ormSample.rgb) < 0.01) {
        out.orm = float4(1.0f, in.instanceRoughness, in.instanceMetallic, 1.0f);
    } else {
        out.orm = float4(ormSample.r, ormSample.g * in.instanceRoughness,
                         ormSample.b * in.instanceMetallic, 1.0f);
        out.orm.r = max(out.orm.r, 0.1f);
    }

    if (length(normalSample.rgb) > 0.1) {
        float3 normal = normalSample.rgb * 2.0 - 1.0;
        float3 N = normalize(in.worldNormal);
        float3 T = normalize(in.worldTangent);
        float3 B = cross(N, T);
        float3x3 TBN = float3x3(T, B, N);
        out.normal = float4(normalize(TBN * normal) * 0.5 + 0.5, 1.0);
    } else {
        out.normal = float4(normalize(in.worldNormal) * 0.5 + 0.5, 1.0);
    }

    float2 currentNDC = in.currentPos.xy / in.currentPos.w;
    float2 previousNDC = in.previousPos.xy / in.previousPos.w;
    out.velocity = (currentNDC - previousNDC) * 0.5;
    return out;
}
