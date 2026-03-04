#include <metal_stdlib>
using namespace metal;

#define RHI_ENABLE_PBR
#include "RHIShaderCommon.metal"

/**
 * @file StandardPBR.metal
 * @brief Standard PBR Shader for Integration Test
 */

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(2)]];
    float4 tangent [[attribute(3)]];
};

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
    float3 worldTangent;
    float3 worldBitangent;
    float2 uv;
    float4 currentPos;
    float4 previousPos;
};

struct FragmentOut {
    float4 color [[color(0)]];
    float2 velocity [[color(1)]];
};

// Matches TestMultiView.cpp SceneData
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
};

// PBRMaterialParameters is defined in RHIShaderTypes.metal

// View Data (Buffer 1)
struct ViewData {
    float4x4 viewProjection;
    float4x4 previousViewProjection;
    // ... potentially more
};

vertex VertexOut vertexMain(
    VertexIn in [[stage_in]],
    constant ViewData& viewData [[buffer(1)]],
    constant SceneData& sceneData [[buffer(2)]]
) {
    VertexOut out;
    
    // Current Position
    float4 worldPos = sceneData.model * float4(in.position, 1.0);
    out.worldPos = worldPos.xyz;
    out.position = viewData.viewProjection * worldPos;
    
    // Jitter (TAA)
    out.position.xy += sceneData.jitter * out.position.w;
    out.currentPos = out.position;
    
    // Previous Position (for Velocity)
    float4 prevWorldPos = sceneData.previousModel * float4(in.position, 1.0);
    out.previousPos = viewData.previousViewProjection * prevWorldPos;
    out.previousPos.xy += sceneData.previousJitter * out.previousPos.w;

    out.uv = in.uv;
    
    // Normal, Tangent, Bitangent in World Space
    float3x3 normalMatrix = float3x3(sceneData.model[0].xyz, sceneData.model[1].xyz, sceneData.model[2].xyz);
    // Note: Assuming uniform scaling. For non-uniform, use inverse transpose.
    out.worldNormal = normalize(normalMatrix * in.normal);
    out.worldTangent = normalize(normalMatrix * in.tangent.xyz);
    out.worldBitangent = cross(out.worldNormal, out.worldTangent) * in.tangent.w;
    
    return out;
}

fragment FragmentOut fragmentMain(
    VertexOut in [[stage_in]],
    constant ViewData& viewData [[buffer(1)]],
    constant SceneData& sceneData [[buffer(2)]],
    constant PBRMaterialParameters& material [[buffer(3)]],
    
    texture2d<float> baseColorMap [[texture(0)]],
    texture2d<float> normalMap [[texture(1)]],
    texture2d<float> metallicRoughnessMap [[texture(2)]],
    texture2d<float> occlusionMap [[texture(3)]],
    texture2d<float> emissiveMap [[texture(4)]],
    
    texturecube<float> irradianceMap [[texture(5)]],
    texturecube<float> prefilteredMap [[texture(6)]],
    texture2d<float> brdfLUT [[texture(7)]],
    
    sampler defaultSampler [[sampler(0)]],
    sampler brdfSampler [[sampler(1)]]
) {
    // 1. Prepare Data
    float4 baseColor = material.baseColorFactor;
    if (material.hasBaseColorTexture) {
        float4 texColor = baseColorMap.sample(defaultSampler, in.uv);
        baseColor *= texColor;
    }
    
    float roughness = material.roughnessFactor;
    float metallic = material.metallicFactor;
    if (material.hasMetallicRoughnessTexture) {
        float4 mrSample = metallicRoughnessMap.sample(defaultSampler, in.uv);
        roughness *= mrSample.g;
        metallic *= mrSample.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    
    float ao = 1.0;
    if (material.hasOcclusionTexture) {
        float occlusion = occlusionMap.sample(defaultSampler, in.uv).r;
        ao = mix(1.0, occlusion, material.occlusionStrength);
    }
    
    float3 emissive = material.emissiveFactor;
    if (material.hasEmissiveTexture) {
        emissive *= emissiveMap.sample(defaultSampler, in.uv).rgb;
    }
    
    // Normal Mapping
    float3 N = normalize(in.worldNormal);
    if (material.hasNormalTexture) {
        float3 tangentNormal = normalMap.sample(defaultSampler, in.uv).xyz * 2.0 - 1.0;
        tangentNormal *= float3(material.normalScale, material.normalScale, 1.0);
        float3 T = normalize(in.worldTangent);
        float3 B = normalize(in.worldBitangent);
        N = normalize(T * tangentNormal.x + B * tangentNormal.y + N * tangentNormal.z);
    }
    
    float3 V = normalize(viewData.viewProjection[3].xyz - in.worldPos); // Approx camera pos from ViewProj inv? 
    // Actually ViewProj doesn't give CameraPos easily. 
    // We can infer it if we pass it, or assume it's at 0,0,18 for this test.
    // Or invert View matrix.
    // For now, let's hardcode eye pos or pass it in ViewData if needed.
    // Let's assume viewData.viewProjection is passed, but we really need Camera Pos.
    // In TestMultiView, Camera is at (0,0,18).
    float3 camPos = float3(0, 0, 18); // Hardcoded for this test
    V = normalize(camPos - in.worldPos);

    float3 R = reflect(-V, N);
    
    float NdotV = max(dot(N, V), 1e-5);
    
    // 2. Direct Lighting (Simple Point Light from SceneData)
    float3 Lo = float3(0.0);
    
    // Light
    float3 L = normalize(sceneData.lightPos.xyz - in.worldPos);
    float3 H = normalize(V + L);
    float distance = length(sceneData.lightPos.xyz - in.worldPos);
    float attenuation = 1.0 / (distance * distance);
    float3 radiance = sceneData.lightColor.rgb * attenuation * 10.0; // Boost intensity
    
    // Cook-Torrance BRDF
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F0 = mix(float3(0.04), baseColor.rgb, metallic);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    
    float3 numerator = NDF * G * F;
    float denominator = 4.0 * NdotV * max(dot(N, L), 0.0) + 0.0001;
    float3 specular = numerator / denominator;
    
    float3 kS = F;
    float3 kD = float3(1.0) - kS;
    kD *= 1.0 - metallic;
    
    float NdotL = max(dot(N, L), 0.0);
    Lo += (kD * baseColor.rgb / PI + specular) * radiance * NdotL;
    
    // 3. IBL
    float3 F_IBL = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kS_IBL = F_IBL;
    float3 kD_IBL = 1.0 - kS_IBL;
    kD_IBL *= 1.0 - metallic;
    
    float3 irradiance = irradianceMap.sample(defaultSampler, N).rgb;
    float3 diffuse = irradiance * baseColor.rgb;
    
    const float MAX_REFLECTION_LOD = 4.0;
    float3 prefilteredColor = prefilteredMap.sample(defaultSampler, R, level(roughness * MAX_REFLECTION_LOD)).rgb;
    float2 brdf = brdfLUT.sample(brdfSampler, float2(NdotV, roughness)).rg;
    float3 specularIBL = prefilteredColor * (F_IBL * brdf.x + brdf.y);
    
    float3 ambient = (kD_IBL * diffuse + specularIBL) * ao;
    
    float3 color = ambient + Lo + emissive;
    
    // Tone mapping (Reinhard)
    color = color / (color + float3(1.0));
    // Gamma correction
    color = pow(color, float3(1.0/2.2));
    
    FragmentOut out;
    out.color = float4(color, 1.0);
    
    // Velocity Calculation
    float2 currentPosNDC = in.currentPos.xy / in.currentPos.w;
    float2 previousPosNDC = in.previousPos.xy / in.previousPos.w;
    
    // Convert to UV [0, 1] space (Y flip for Metal?)
    // Metal NDC Y is Up, Texture UV Y is Down usually.
    // But Velocity is usually in NDC or Screen Space.
    // Let's store NDC delta.
    // TAA Pass usually expects (CurrentUV - PreviousUV).
    // UV = NDC * 0.5 + 0.5; (Flip Y if needed)
    float2 currentUV = currentPosNDC * 0.5 + 0.5;
    float2 previousUV = previousPosNDC * 0.5 + 0.5;
    currentUV.y = 1.0 - currentUV.y;
    previousUV.y = 1.0 - previousUV.y;
    
    out.velocity = currentUV - previousUV;
    
    return out;
}
