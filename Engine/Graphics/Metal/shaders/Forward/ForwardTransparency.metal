#include <metal_stdlib>
using namespace metal;

#define RHI_ENABLE_PBR
#include "RHIShaderCommon.metal"

constant float InvIntervals = 2.f / ((1 << 16) - 1);

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
    float2 uv;
    float4 shadowPos0;
    float4 shadowPos1;
    float4 instanceBaseColor;
    float  instanceRoughness;
    float  instanceMetallic;
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
    if (d > 1.0f) return float3(0.0f, 0.0f, 1.0f);
    float z = sqrt(max(0.0f, 1.0f - d));
    return float3(f.x, f.y, z);
}

// === Water Forward ===

vertex VertexOut forwardWaterVS(
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
    float4 worldPos = model * float4(rawPos, 1.0);
    out.worldPos = worldPos.xyz;

    float3 rawNormal = UnpackNormal(vertices[vertexId].element.Normal);
    float3x3 normalMatrix = float3x3(model[0].xyz, model[1].xyz, model[2].xyz);
    out.worldNormal = normalize(normalMatrix * rawNormal);

    out.uv = float2(vertices[vertexId].element.UV.x, 1.0 - vertices[vertexId].element.UV.y);
    out.position = viewData.viewProjection * worldPos;
    out.shadowPos0 = sceneData.shadowMatrix0 * worldPos;
    out.shadowPos1 = sceneData.shadowMatrix1 * worldPos;
    return out;
}

fragment float4 forwardWaterFS(
    VertexOut in [[stage_in]],
    constant SceneData& sceneData [[buffer(1)]],
    texture2d<float> albedoMap [[texture(0)]],
    texture2d<float> normalMap [[texture(1)]],
    texture2d<float> ormMap [[texture(2)]],
    sampler defaultSampler [[sampler(3)]]
) {
    float t = sceneData.time;
    float3 waveNormal;
    waveNormal.x = sin(in.worldPos.x * 2.0 + t * 1.5) * cos(in.worldPos.z * 1.8 + t * 1.2) * 0.15;
    waveNormal.y = 1.0;
    waveNormal.z = cos(in.worldPos.x * 1.6 + t * 1.8) * sin(in.worldPos.z * 2.2 + t * 1.0) * 0.15;
    float3 N = normalize(in.worldNormal + waveNormal);

    float3 viewDir = normalize(sceneData.viewPos.xyz - in.worldPos);
    float fresnel = pow(1.0 - max(dot(viewDir, N), 0.0), 3.0);

    float3 shallowColor = float3(0.1, 0.6, 0.7);
    float3 deepColor = float3(0.02, 0.1, 0.3);
    float3 waterColor = mix(shallowColor, deepColor, fresnel) * in.instanceBaseColor.rgb;

    float opacity = mix(0.4, 0.95, fresnel);
    float roughness = 0.05;
    float metallic = 0.0;

    float3 lightDir = normalize(sceneData.lightPos.xyz);
    float3 H = normalize(viewDir + lightDir);

    float3 F0 = mix(float3(0.04), waterColor, metallic);
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, viewDir, lightDir, roughness);
    float3 F = FresnelSchlick(max(dot(H, viewDir), 0.0), F0);

    float3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, viewDir), 0.0) * max(dot(N, lightDir), 0.0) + 0.0001;
    float3 specular = numerator / denominator;

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);
    float NdotL = max(dot(N, lightDir), 0.0);
    float3 Lo = (kD * waterColor / PI + specular) * sceneData.lightColor.rgb * NdotL;

    // Shadow
    float shadow = 1.0;
    float3 sp0 = in.shadowPos0.xyz / in.shadowPos0.w;
    sp0.xy = sp0.xy * 0.5 + 0.5;
    sp0.y = 1.0 - sp0.y;
    if (sp0.z > 0.0 && sp0.z < 1.0 && sp0.x > 0.0 && sp0.x < 1.0 && sp0.y > 0.0 && sp0.y < 1.0) {
        shadow = 0.5 + 0.5 * step(0.0, sp0.z - 0.005);
    }

    float3 ambient = float3(0.03) * waterColor;
    float3 color = ambient + Lo * shadow;

    color = color / (color + float3(1.0));
    color = pow(color, float3(1.0 / 2.2));

    return float4(color, opacity);
}

// === Transparent Forward ===

vertex VertexOut forwardTransparentVS(
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
    float4 worldPos = model * float4(rawPos, 1.0);
    out.worldPos = worldPos.xyz;

    float3 rawNormal = UnpackNormal(vertices[vertexId].element.Normal);
    float3x3 normalMatrix = float3x3(model[0].xyz, model[1].xyz, model[2].xyz);
    out.worldNormal = normalize(normalMatrix * rawNormal);

    out.uv = float2(vertices[vertexId].element.UV.x, 1.0 - vertices[vertexId].element.UV.y);
    out.position = viewData.viewProjection * worldPos;
    out.shadowPos0 = sceneData.shadowMatrix0 * worldPos;
    out.shadowPos1 = sceneData.shadowMatrix1 * worldPos;
    return out;
}

fragment float4 forwardTransparentFS(
    VertexOut in [[stage_in]],
    constant SceneData& sceneData [[buffer(1)]],
    texture2d<float> albedoMap [[texture(0)]],
    texture2d<float> normalMap [[texture(1)]],
    texture2d<float> ormMap [[texture(2)]],
    sampler defaultSampler [[sampler(3)]]
) {
    float4 albedoSample = albedoMap.sample(defaultSampler, in.uv);
    float3 albedo = albedoSample.rgb * in.instanceBaseColor.rgb;
    float alpha = albedoSample.a * in.instanceBaseColor.a;

    float4 ormSample = ormMap.sample(defaultSampler, in.uv);
    float roughness = in.instanceRoughness;
    float metallic = in.instanceMetallic;
    if (length(ormSample.rgb) > 0.01) {
        roughness = ormSample.g * in.instanceRoughness;
        metallic = ormSample.b * in.instanceMetallic;
    }
    roughness = max(roughness, 0.04);

    float3 N = normalize(in.worldNormal);
    float3 normalSample = normalMap.sample(defaultSampler, in.uv).rgb;
    if (length(normalSample) > 0.1) {
        float3 tangent = normalize(cross(N, float3(0, 1, 0)));
        if (length(cross(N, float3(0, 1, 0))) < 0.01f)
            tangent = normalize(cross(N, float3(1, 0, 0)));
        float3 bitangent = cross(N, tangent);
        float3x3 TBN = float3x3(tangent, bitangent, N);
        N = normalize(TBN * (normalSample * 2.0 - 1.0));
    }

    float3 lightDir = normalize(sceneData.lightPos.xyz);
    float3 viewDir = normalize(sceneData.viewPos.xyz - in.worldPos);
    float3 H = normalize(viewDir + lightDir);

    float3 F0 = mix(float3(0.04), albedo, metallic);
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, viewDir, lightDir, roughness);
    float3 F = FresnelSchlick(max(dot(H, viewDir), 0.0), F0);

    float3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, viewDir), 0.0) * max(dot(N, lightDir), 0.0) + 0.0001;
    float3 specular = numerator / denominator;

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);
    float NdotL = max(dot(N, lightDir), 0.0);
    float3 Lo = (kD * albedo / PI + specular) * sceneData.lightColor.rgb * NdotL;

    // Shadow
    float shadow = 1.0;
    float3 sp0 = in.shadowPos0.xyz / in.shadowPos0.w;
    sp0.xy = sp0.xy * 0.5 + 0.5;
    sp0.y = 1.0 - sp0.y;
    if (sp0.z > 0.0 && sp0.z < 1.0 && sp0.x > 0.0 && sp0.x < 1.0 && sp0.y > 0.0 && sp0.y < 1.0) {
        shadow = 0.5 + 0.5 * step(0.0, sp0.z - 0.005);
    }

    float3 ambient = float3(0.03) * albedo;
    float3 color = ambient + Lo * shadow;

    color = color / (color + float3(1.0));
    color = pow(color, float3(1.0 / 2.2));

    return float4(color, alpha);
}
