#include <metal_stdlib>
using namespace metal;

#define RHI_ENABLE_PBR
#include "RHIShaderCommon.metal"

constant float InvIntervals = 2.f / ((1 << 16) - 1);

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

struct ViewData {
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float4x4 previousViewProjection;
};

struct VertexElement {
    uint            ColorTSign;
    packed_ushort2  Normal;
    packed_ushort2  Tangent;
    packed_float2   UV;
};

struct VertexInput {
    packed_float3 position;
    VertexElement element;
};

float3 UnpackNormal(packed_ushort2 p) {
    float2 f = float2(p);
    f = f * InvIntervals - 1.0f;
    float d = dot(f, f);
    if (d > 1.0f) {
        return float3(0.0f, 0.0f, 1.0f);
    }
    float z = sqrt(max(0.0f, 1.0f - d));
    return float3(f.x, f.y, z);
}

vertex VertexOut vertexMain(
    uint vertexId [[vertex_id]],
    uint instanceId [[instance_id]],
    constant ViewData& viewData [[buffer(0)]],
    constant SceneData& sceneData [[buffer(1)]],
    constant PushConsts& pushConsts [[buffer(2)]],
    constant InstanceData* instanceData [[buffer(3)]],
    constant VertexInput* vertices [[buffer(20)]]
) {
    VertexOut out;

    float4x4 model;
    if (pushConsts.use_instances != 0) {
        InstanceData inst = instanceData[instanceId];
        model = inst.transform;
        out.instanceBaseColor  = inst.baseColor;
        out.instanceRoughness  = inst.roughness;
        out.instanceMetallic   = inst.metallic;
    } else {
        model = pushConsts.model;
        out.instanceBaseColor  = float4(1.0, 1.0, 1.0, 1.0);
        out.instanceRoughness  = 0.5;
        out.instanceMetallic   = 0.0;
    }

    float3 rawPos = vertices[vertexId].position;
    VertexElement element = vertices[vertexId].element;

    float3 rawNormal = UnpackNormal(element.Normal);
    float3 rawTangent = UnpackNormal(element.Tangent);
    float2 rawUV = element.UV;

    float4 worldPos = model * float4(rawPos, 1.0);
    out.worldPos = worldPos.xyz;

    float3x3 normalMatrix = float3x3(model[0].xyz, model[1].xyz, model[2].xyz);
    out.worldNormal = normalize(normalMatrix * rawNormal);
    out.worldTangent = normalize(normalMatrix * rawTangent);
    out.worldBitangent = cross(out.worldNormal, out.worldTangent);

    out.uv = float2(rawUV.x, 1.0 - rawUV.y);
    out.position = viewData.viewProjection * worldPos;

    out.currentPos = out.position;
    out.previousPos = viewData.previousViewProjection * (sceneData.previousModel * float4(rawPos, 1.0));

    return out;
}

fragment FragmentOut fragmentMain(
    VertexOut in [[stage_in]],
    constant SceneData& sceneData [[buffer(1)]],
    texture2d<float> albedoMap [[texture(0)]],
    texture2d<float> normalMap [[texture(1)]],
    texture2d<float> ormMap [[texture(2)]],
    sampler defaultSampler [[sampler(3)]]
) {
    FragmentOut out;

    // Wave normal perturbation
    float t = sceneData.time;
    float3 waveNormal;
    waveNormal.x = sin(in.worldPos.x * 2.0 + t * 1.5) * cos(in.worldPos.z * 1.8 + t * 1.2) * 0.15;
    waveNormal.y = 1.0;
    waveNormal.z = cos(in.worldPos.x * 1.6 + t * 1.8) * sin(in.worldPos.z * 2.2 + t * 1.0) * 0.15;
    waveNormal = normalize(waveNormal);

    // Perturb the geometric normal with wave
    float3 N = normalize(in.worldNormal + waveNormal);

    // Fresnel (view-dependent opacity)
    float3 viewDir = normalize(sceneData.viewPos.xyz - in.worldPos);
    float fresnel = pow(1.0 - max(dot(viewDir, N), 0.0), 3.0);

    // Shallow/deep water colors
    float3 shallowColor = float3(0.1, 0.6, 0.7);
    float3 deepColor = float3(0.02, 0.1, 0.3);
    float3 waterColor = mix(shallowColor, deepColor, fresnel);
    waterColor *= in.instanceBaseColor.rgb;

    float opacity = mix(0.4, 0.95, fresnel);
    out.albedo = float4(waterColor, opacity);

    // Normal output
    out.normal = float4(N * 0.5 + 0.5, 1.0);

    // ORM: low roughness, no metallic
    out.orm = float4(1.0, 0.05, 0.0, 1.0);

    // Velocity
    float2 currentNDC = in.currentPos.xy / in.currentPos.w;
    float2 previousNDC = in.previousPos.xy / in.previousPos.w;
    float2 currentNDC_NoJitter = currentNDC - sceneData.jitter;
    float2 previousNDC_NoJitter = previousNDC - sceneData.previousJitter;
    out.velocity = (currentNDC_NoJitter - previousNDC_NoJitter) * 0.5;

    return out;
}
