#include <metal_stdlib>
using namespace metal;

// 必须与 Engine/Graphics/RHI/Core/RHIShaderCommon.h 保持一致
struct GlobalShaderData
{
    float4x4 view;
    float4x4 projection;
    float4x4 invProjection;
    float4x4 viewProjection;
    float4x4 previousViewProjection;
    float4x4 invViewProjection;

    float4 cameraPositionAndViewWidth;
    float4 cameraDirectionAndViewHeight;

    uint numDirectionalLights;
    float deltaTime;
    float frameCount;
    float padding;
};

struct PerObjectData
{
    float4x4 world;
    float4x4 invWorld;
    float4x4 worldViewProjection;
};

struct LightParameters
{
    float3 position;
    float intensity;

    float3 direction;
    float range;

    float3 color;
    float cosUmbra;

    float3 attenuation;
    float cosPenumbra;
};

// 更新后的结构体，支持 CSM
struct DirectionalLightParameters
{
    float4x4 viewProjections[4]; // Cascade ViewProjection matrices
    float4 splits;               // Cascade split distances
    
    float4 directionAndIntensity; // xyz: direction, w: intensity
    
    float4 colorAndShadow; // rgb: color, a: shadow enabled
};

struct ForwardLightBuffer {
    uint directionalLightCount;
    uint punctualLightCount;
    uint padding[2];
    DirectionalLightParameters directionalLights[4];
    LightParameters lights[128];
};

struct MaterialUniforms {
    float4 color;
};

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 color [[attribute(1)]];
    float3 normal [[attribute(2)]];
};

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
    float3 color;
    float viewDepth; // 用于选择 Cascade
};

// PCF Filter Helper
float SampleShadowPCF(depth2d_array<float> shadowMap, sampler shadowSampler, float3 shadowCoord, float layer, float2 texelSize, float bias) {
    float shadow = 0.0;
    int radius = 2; // 5x5 PCF
    for (int x = -radius; x <= radius; ++x) {
        for (int y = -radius; y <= radius; ++y) {
            float2 offset = float2(x, y) * texelSize;
            // Apply bias to the comparison
            // sample_compare uses the reference value (shadowCoord.z) to compare against the texture value.
            // If shadowCoord.z <= texture_value, it returns 1.0 (lit).
            // To apply bias, we want to effectively push the receiver surface closer to the light (reduce shadowCoord.z)
            // or push the occluder further away (increase texture value).
            // Standard approach: compare (current_depth - bias) <= stored_depth
            shadow += shadowMap.sample_compare(shadowSampler, float2(shadowCoord.xy + offset), int(layer), shadowCoord.z - bias);
        }
    }
    return shadow / ((2.0 * radius + 1.0) * (2.0 * radius + 1.0));
}

vertex VertexOut vertexMain(
    VertexIn in [[stage_in]],
    constant PerObjectData& perObject [[buffer(10)]],
    constant GlobalShaderData& globalData [[buffer(11)]])
{
    VertexOut out;
    float4 worldPos = perObject.world * float4(in.position, 1.0);
    out.position = perObject.worldViewProjection * float4(in.position, 1.0);
    out.worldPos = worldPos.xyz;
    out.worldNormal = (perObject.world * float4(in.normal, 0.0)).xyz;
    out.color = in.color;
    
    // Calculate View Space Depth for Cascade Selection
    // View Space Z = (View * WorldPos).z
    float4 viewPos = globalData.view * worldPos;
    out.viewDepth = -viewPos.z; // Positive depth
    
    return out;
}

// Fragment Shader
fragment float4 fragment_main(VertexOut in [[stage_in]],
                            constant ForwardLightBuffer& lightData [[buffer(12)]],
                            constant MaterialUniforms& material [[buffer(3)]],
                            depth2d_array<float> shadowMap [[texture(13)]],
                            sampler shadowSampler [[sampler(13)]]) {
    
    // Debug Configuration
    // 0: Normal Lighting
    // 1: Cascade Colors (Red=0, Green=1, Blue=2, Yellow=3)
    // 2: Shadow Factor Only (Black=Shadow, White=Lit)
    // 3: NdotL Only (Lighting check)
    int debugMode = 0; 

    // Extract Directional Light (Assume index 0)
    DirectionalLightParameters light = lightData.directionalLights[0];

    float3 N = normalize(in.worldNormal);
    float3 L = normalize(-light.directionAndIntensity.xyz);
    
    // Simple Lambert
    float NdotL = max(dot(N, L), 0.0);
    
    // Shadow Calculation
    float shadowFactor = 1.0;
    float3 debugColor = float3(0.0);
    
    // Lift declarations for debug access
    uint cascadeIndex = 0;
    float4 shadowCoord = float4(0.0);
    float2 shadowUV = float2(0.0);

    if (light.colorAndShadow.a > 0.0 || debugMode == 2) {
        // Select Cascade
        cascadeIndex = 3;
        float3 cascadeColor = float3(1.0, 1.0, 0.0); // Yellow for last cascade
        
        if (in.viewDepth < light.splits[0]) {
            cascadeIndex = 0;
            cascadeColor = float3(1.0, 0.0, 0.0); // Red
        } else if (in.viewDepth < light.splits[1]) {
            cascadeIndex = 1;
            cascadeColor = float3(0.0, 1.0, 0.0); // Green
        } else if (in.viewDepth < light.splits[2]) {
            cascadeIndex = 2;
            cascadeColor = float3(0.0, 0.0, 1.0); // Blue
        }
        
        debugColor = cascadeColor;
        
        // Project to Light Space
        shadowCoord = light.viewProjections[cascadeIndex] * float4(in.worldPos, 1.0);
        shadowCoord.xyz /= shadowCoord.w;
        
        // Convert to texture coordinates [0, 1]
        shadowUV.x = shadowCoord.x * 0.5 + 0.5;
        shadowUV.y = -shadowCoord.y * 0.5 + 0.5; // Metal Y is down
        
        if (light.colorAndShadow.a > 0.0 && 
                shadowUV.x >= 0.0 && shadowUV.x <= 1.0 && 
                shadowUV.y >= 0.0 && shadowUV.y <= 1.0 && 
                shadowCoord.z >= 0.0 && shadowCoord.z <= 1.0) {
                 float2 texelSize = float2(1.0 / 2048.0, 1.0 / 2048.0); // Assume 2048x2048
                 
                 // Calculate adaptive bias based on slope
                 // float bias = max(0.005 * (1.0 - NdotL), 0.0005);
                 // Increased bias to fix shadow acne (stripes)
                 float bias = max(0.005 * (1.0 - NdotL), 0.002);
                 if (cascadeIndex == 0) bias *= 0.5; // Finer bias for first cascade
                 
                 shadowFactor = SampleShadowPCF(shadowMap, shadowSampler, float3(shadowUV, shadowCoord.z), float(cascadeIndex), texelSize, bias);
            }
        }

        // Debug Output Overrides
        if (debugMode == 1) {
            // Overlay shadows on top of cascade colors
            return float4(debugColor * (0.5 + 0.5 * shadowFactor), 1.0);
        } else if (debugMode == 2) {
            // Visualize Shadow vs Lit with Cascade Color
            uint2 debugCoord = uint2(shadowUV * 2048.0);
            float mapDepth = shadowMap.read(debugCoord, cascadeIndex);
            float refZ = shadowCoord.z;
            
            float bias = max(0.005 * (1.0 - NdotL), 0.002);
            bool isShadow = refZ > mapDepth + bias;
            
            float3 color = debugColor; // Cascade Color (R, G, B, Y)
            if (isShadow) {
                color *= 0.2; // Darken for shadow
            } else {
                // Lit: Keep color bright
            }
            
            return float4(color, 1.0);
        } else if (debugMode == 3) {
        return float4(NdotL, NdotL, NdotL, 1.0);
    }

    // Normal Lighting
    float3 diffuse = material.color.rgb * light.colorAndShadow.rgb * NdotL * shadowFactor;
    float3 ambient = material.color.rgb * 0.2; // 0.2 Ambient
    
    return float4(diffuse + ambient, 1.0);
}
