#include <metal_stdlib>
using namespace metal;

#define RHI_ENABLE_PBR
#include "RHIShaderCommon.metal"

// ================================================================================================
// Data Structures (Must match C++ Binding)
// ================================================================================================

struct VertexOut {
    float4 position [[position]];
    float2 uv;
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

struct ViewData {
    float4x4 viewProjection;
    float4x4 invViewProjection;
};

// ================================================================================================
// Vertex Shader
// ================================================================================================

vertex VertexOut vertexMain(uint vertexID [[vertex_id]]) {
    VertexOut out;
    // Full-screen triangle covering the screen
    float4 positions[3] = {
        float4(-1.0, -1.0, 0.0, 1.0), // Bottom-Left
        float4( 3.0, -1.0, 0.0, 1.0), // Bottom-Right (Extended)
        float4(-1.0,  3.0, 0.0, 1.0)  // Top-Left (Extended)
    };
    float2 uvs[3] = {
        float2(0.0, 1.0), // Bottom-Left UV (Metal Texture: 0,0 is Top-Left)
        float2(2.0, 1.0), 
        float2(0.0, -1.0) 
    };
    
    out.position = positions[vertexID];
    out.uv = uvs[vertexID];
    return out;
}

// ================================================================================================
// Shadow Helper
// ================================================================================================

float GetShadow(float3 worldPos, float4x4 shadowMatrix, texture2d<float> shadowMap, sampler passedSampler) {
    constexpr sampler shadowSampler(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
    
    float4 clipPos = shadowMatrix * float4(worldPos, 1.0);
    float3 shadowCoord = clipPos.xyz / clipPos.w;
    
    // NDC to UV
    shadowCoord.x = shadowCoord.x * 0.5 + 0.5;
    shadowCoord.y = shadowCoord.y * -0.5 + 0.5; 
    
    // Check bounds
    if (shadowCoord.x < 0.0 || shadowCoord.x > 1.0 || 
        shadowCoord.y < 0.0 || shadowCoord.y > 1.0 || 
        shadowCoord.z < 0.0 || shadowCoord.z > 1.0) {
        return 1.0;
    }
    
    // Sample Raw Depth
    float closestDepth = shadowMap.sample(shadowSampler, shadowCoord.xy).r;
    
    float currentDepth = shadowCoord.z;
    
    // Bias
    float bias = 0.0005; 
    
    // Debug Visualization Output (Global variable hack or return logic)
    // We can't easily return debug info from here without changing signature.
    // But we can use the result.
    
    return (currentDepth - bias > closestDepth) ? 0.0 : 1.0;
}

// ================================================================================================
// Fragment Shader (PBR Lighting)
// ================================================================================================

fragment float4 fragmentLighting_v3(
    VertexOut in [[stage_in]],
    constant ViewData& viewData [[buffer(0)]],
    constant SceneData& sceneData [[buffer(1)]],
    
    texture2d<float> albedoTex [[texture(2)]],
    texture2d<float> normalTex [[texture(3)]],
    texture2d<float> ormTex [[texture(4)]],
    texture2d<float> depthTex [[texture(5)]],
    texture2d<float> shadowMap0 [[texture(6)]],
    texture2d<float> shadowMap1 [[texture(7)]],
    texturecube<float> irradianceMap [[texture(8)]],
    texturecube<float> prefilterMap [[texture(9)]],
    texture2d<float> brdfLUT [[texture(10)]],
    
    sampler defaultSampler [[sampler(11)]],
    sampler brdfSampler [[sampler(12)]]
) {
    float2 uv = in.uv;
    
    // 1. Sample GBuffer
    float4 albedo = albedoTex.sample(defaultSampler, uv);
    float3 normal = normalTex.sample(defaultSampler, uv).xyz;
    normal = normal * 2.0 - 1.0; // Unpack [0,1] -> [-1,1]
    float depth = depthTex.sample(defaultSampler, uv).r;

    // Discard background pixels to preserve Skybox
    if (depth >= 1.0) {
        discard_fragment();
    }

    float4 orm = ormTex.sample(defaultSampler, uv);
    float roughness = orm.g;
    float metallic = orm.b;
    float ao = orm.r;

    // 2. Reconstruct World Position
    // Metal NDC: Z [0, 1], Y [-1, 1] (Y Down: -1 Top, 1 Bottom) -> This comment was wrong. Metal is Y-Up (-1 Bottom, 1 Top)
    float2 ndc;
    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = (1.0 - uv.y) * 2.0 - 1.0; // Flip Y (Top UV=0 -> Top NDC=1)

    float z = depth;

    float4 clipPos = float4(ndc, z, 1.0);

    float4 worldPos4 = viewData.invViewProjection * clipPos;
    float3 worldPos = worldPos4.xyz / worldPos4.w;
    
    // 3. Calculate Shadow
    float shadow = GetShadow(worldPos, sceneData.shadowMatrix0, shadowMap0, defaultSampler);

    // Calculate PBR Lighting
    float3 N = normalize(normal);
    float3 V = normalize(sceneData.viewPos.xyz - worldPos);
    float3 R = reflect(-V, N); 

    // F0 for Fresnel
    float3 F0 = float3(0.04); 
    F0 = mix(F0, albedo.rgb, metallic);

    float3 Lo = float3(0.0);

    // --- Direct Light (Directional) ---
    {
        float3 L;
        float attenuation = 1.0;
        
        if (sceneData.lightPos.w == 0.0) {
            // Directional Light
            // lightPos.xyz is the direction TO the light source
            L = normalize(sceneData.lightPos.xyz);
        } else {
            // Point Light
            float3 lightDir = sceneData.lightPos.xyz - worldPos;
            float distance = length(lightDir);
            L = normalize(lightDir);
            attenuation = 1.0 / (distance * distance); // Inverse square falloff
        }

        float3 H = normalize(V + L);
        
        float3 radiance = sceneData.lightColor.rgb * shadow * attenuation; 
        
        // Cook-Torrance BRDF
        float NDF = DistributionGGX(N, H, roughness);   
        float G   = GeometrySmith(N, V, L, roughness);      
        float3 F  = FresnelSchlick(max(dot(H, V), 0.0), F0);
        
        float3 numerator    = NDF * G * F; 
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        float3 specular = numerator / denominator;
        
        // kS is equal to Fresnel
        float3 kS = F;
        float3 kD = float3(1.0) - kS;
        kD *= 1.0 - metallic;	  

        float NdotL = max(dot(N, L), 0.0);        

        Lo += (kD * albedo.rgb / PI + specular) * radiance * NdotL;  
    }

    // --- Ambient Light (IBL) ---
    // IBL Diffuse (Irradiance)
    float3 kS = FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    float3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;
    
    float3 irradiance = irradianceMap.sample(defaultSampler, N).rgb;
    float3 diffuse = irradiance * albedo.rgb;
    
    // IBL Specular (Prefilter + BRDF)
    const float MAX_REFLECTION_LOD = 4.0;
    float3 prefilteredColor = prefilterMap.sample(defaultSampler, R, level(roughness * MAX_REFLECTION_LOD)).rgb;
    float2 brdf = brdfLUT.sample(brdfSampler, float2(max(dot(N, V), 0.0), roughness)).rg;
    float3 specular = prefilteredColor * (kS * brdf.x + brdf.y);
    
    // Reduce IBL intensity to increase contrast with Direct Light
    float iblIntensity = 0.3; 
    float3 ambient = (kD * diffuse + specular) * iblIntensity; // AO is in orm.r
    ambient *= ao; 
    
    float3 color = ambient + Lo;
    
    // HDR Tone Mapping (Reinhard) - Simple version if PostProcess is not doing it
    // color = color / (color + float3(1.0));
    // Gamma Correct
    // color = pow(color, float3(1.0/2.2));
    
    return float4(color, 1.0);
}

// ================================================================================================
// Blit Shader
// ================================================================================================

fragment float4 fragmentBlit(
    VertexOut in [[stage_in]],
    texture2d<float> inputTex [[texture(0)]]
) {
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
    float4 sampled = inputTex.sample(s, in.uv);
    return sampled;
}