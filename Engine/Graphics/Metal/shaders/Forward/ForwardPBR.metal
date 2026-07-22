#include <metal_stdlib>
using namespace metal;

constant float InvIntervals = 2.f / ((1 << 16) - 1);
constant float PI = 3.14159265359;

struct ViewData {
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float4x4 previousViewProjection;
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
    float2 padding;
    float4 viewPos;
    float4x4 shadowMatrix0;
    float4x4 shadowMatrix1;
};

struct PushConsts {
    float4x4 model;
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

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
    float2 uv;
    float4 shadowPos0;
    float4 shadowPos1;
};

float3 UnpackNormal(packed_ushort2 p) {
    float2 f = float2(p);
    f = f * InvIntervals - 1.0f;
    float d = dot(f, f);
    if (d > 1.0f) return float3(0.0f, 0.0f, 1.0f);
    float z = sqrt(max(0.0f, 1.0f - d));
    return float3(f.x, f.y, z);
}

float DistributionGGX(float3 N, float3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom + 0.0001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float SampleShadow(depth2d<float> shadowMap, float3 shadowPos) {
    if (shadowPos.z < 0.0 || shadowPos.z > 1.0) return 1.0;
    return shadowMap.compare(shadowPos.xy, shadowPos.z);
}

vertex VertexOut vertexMain(
    uint vertexId [[vertex_id]],
    constant ViewData& viewData [[buffer(0)]],
    constant SceneData& sceneData [[buffer(1)]],
    constant PushConsts& pushConsts [[buffer(2)]],
    constant VertexInput* vertices [[buffer(20)]]
) {
    VertexOut out;

    float3 rawPos = vertices[vertexId].position;
    VertexElement element = vertices[vertexId].element;
    float3 rawNormal = UnpackNormal(element.Normal);
    float2 rawUV = element.UV;

    float4 worldPos = pushConsts.model * float4(rawPos, 1.0);
    out.worldPos = worldPos.xyz;

    float3x3 normalMatrix = float3x3(pushConsts.model[0].xyz, pushConsts.model[1].xyz, pushConsts.model[2].xyz);
    out.worldNormal = normalize(normalMatrix * rawNormal);

    out.uv = float2(rawUV.x, 1.0 - rawUV.y);
    out.position = viewData.viewProjection * worldPos;

    out.shadowPos0 = sceneData.shadowMatrix0 * worldPos;
    out.shadowPos1 = sceneData.shadowMatrix1 * worldPos;

    return out;
}

fragment float4 fragmentMain(
    VertexOut in [[stage_in]],
    constant SceneData& sceneData [[buffer(1)]],
    texture2d<float> albedoMap [[texture(0)]],
    texture2d<float> normalMap [[texture(1)]],
    texture2d<float> ormMap [[texture(2)]],
    sampler defaultSampler [[sampler(3)]],
    depth2d<float> shadowMap0 [[texture(4)]],
    depth2d<float> shadowMap1 [[texture(5)]]
) {
    float4 albedoSample = albedoMap.sample(defaultSampler, in.uv);
    float3 albedo = albedoSample.rgb;

    // ORM
    float4 ormSample = ormMap.sample(defaultSampler, in.uv);
    float ao = 1.0, roughness = 0.8, metallic = 0.0;
    if (length(ormSample.rgb) > 0.01) {
        ao = max(ormSample.r, 0.1);
        roughness = ormSample.g;
        metallic = ormSample.b;
    }

    // Normal mapping
    float3 N = normalize(in.worldNormal);
    float3 normalSample = normalMap.sample(defaultSampler, in.uv).rgb;
    if (length(normalSample) > 0.1) {
        float3 tangent = normalize(cross(N, float3(0, 1, 0)));
        if (length(cross(N, float3(0, 1, 0))) < 0.01f)
            tangent = normalize(cross(N, float3(1, 0, 0)));
        float3 bitangent = cross(N, tangent);
        float3x3 TBN = float3x3(tangent, bitangent, N);
        float3 mapped = normalSample * 2.0 - 1.0;
        N = normalize(TBN * mapped);
    }

    // Lighting
    float3 lightDir = normalize(sceneData.lightPos.xyz - in.worldPos);
    float3 viewDir = normalize(sceneData.viewPos.xyz - in.worldPos);
    float3 halfDir = normalize(viewDir + lightDir);

    float3 F0 = mix(float3(0.04), albedo, metallic);

    float NDF = DistributionGGX(N, halfDir, roughness);
    float G = GeometrySmith(N, viewDir, lightDir, roughness);
    float3 F = FresnelSchlick(max(dot(halfDir, viewDir), 0.0), F0);

    float3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, viewDir), 0.0) * max(dot(N, lightDir), 0.0) + 0.0001;
    float3 specular = numerator / denominator;

    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);

    float NdotL = max(dot(N, lightDir), 0.0);

    float3 Lo = (kD * albedo / PI + specular) * sceneData.lightColor.rgb * NdotL;

    // Ambient
    float3 ambient = float3(0.03) * albedo * ao;

    // Shadow
    float shadow = 1.0;
    float3 sp0 = in.shadowPos0.xyz / in.shadowPos0.w;
    sp0.xy = sp0.xy * 0.5 + 0.5;
    sp0.y = 1.0 - sp0.y;
    if (sp0.z > 0.0 && sp0.z < 1.0 && sp0.x > 0.0 && sp0.x < 1.0 && sp0.y > 0.0 && sp0.y < 1.0) {
        shadow = SampleShadow(shadowMap0, sp0);
    } else {
        float3 sp1 = in.shadowPos1.xyz / in.shadowPos1.w;
        sp1.xy = sp1.xy * 0.5 + 0.5;
        sp1.y = 1.0 - sp1.y;
        if (sp1.z > 0.0 && sp1.z < 1.0 && sp1.x > 0.0 && sp1.x < 1.0 && sp1.y > 0.0 && sp1.y < 1.0) {
            shadow = SampleShadow(shadowMap1, sp1);
        }
    }

    float3 color = ambient + Lo * shadow;

    // Tone map
    color = color / (color + float3(1.0));
    color = pow(color, float3(1.0 / 2.2));

    return float4(color, albedoSample.a);
}
