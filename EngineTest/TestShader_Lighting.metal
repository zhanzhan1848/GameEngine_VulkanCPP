#include <metal_stdlib>
using namespace metal;

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

struct DirectionalLightParameters
{
    float4x4 lightMVP;
    float4 directionAndIntensity;
    float4 color;
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
};

vertex VertexOut vertexMain(
    VertexIn in [[stage_in]],
    constant PerObjectData& perObject [[buffer(10)]])
{
    VertexOut out;
    float4 worldPos = perObject.world * float4(in.position, 1.0);
    out.position = perObject.worldViewProjection * float4(in.position, 1.0);
    out.worldPos = worldPos.xyz;
    // For proper normal transformation, use inverse transpose of world matrix
    // perObject.invWorld is Inverse(World). Transpose(Inverse(World)) is needed.
    // However, if we assume uniform scaling, we can use World (3x3).
    // Let's use invWorld to be correct (Inverse Transpose).
    // Note: matrix in Metal is column-major.
    // Transpose of InvWorld is what we need.
    // float3x3 normalMatrix = float3x3(perObject.invWorld[0].xyz, perObject.invWorld[1].xyz, perObject.invWorld[2].xyz); // This is just upper 3x3 of InvWorld
    // We want Transpose(InvWorld).
    // float3x3 normalMatrix = transpose(float3x3(perObject.invWorld[0].xyz, perObject.invWorld[1].xyz, perObject.invWorld[2].xyz));
    
    // Simplification: just use world matrix for now (assuming no non-uniform scale)
    out.worldNormal = (perObject.world * float4(in.normal, 0.0)).xyz;
    out.color = in.color;
    return out;
}

fragment float4 fragmentMain(
    VertexOut in [[stage_in]],
    constant GlobalShaderData& globalData [[buffer(11)]],
    constant ForwardLightBuffer& lightData [[buffer(12)]],
    constant MaterialUniforms& material [[buffer(3)]])
{
    float3 N = normalize(in.worldNormal);
    float3 V = normalize(globalData.cameraPositionAndViewWidth.xyz - in.worldPos);
    
    float3 totalDiffuse = float3(0.0);
    float3 totalSpecular = float3(0.0);
    float3 albedo = in.color * material.color.rgb;
    
    // Directional Lights
    for(uint i = 0; i < globalData.numDirectionalLights; ++i) {
        DirectionalLightParameters light = lightData.directionalLights[i];
        float3 L = normalize(-light.directionAndIntensity.xyz);
        float intensity = light.directionAndIntensity.w;
        float3 lightColor = light.color.rgb * intensity;
        
        // Diffuse
        float NdotL = max(dot(N, L), 0.0);
        totalDiffuse += lightColor * NdotL;
        
        // Specular (Blinn-Phong)
        if (NdotL > 0.0) {
            float3 H = normalize(L + V);
            float NdotH = max(dot(N, H), 0.0);
            float spec = pow(NdotH, 32.0);
            totalSpecular += lightColor * spec;
        }
    }

    // Point Lights
    for(uint i = 0; i < lightData.punctualLightCount; ++i) {
        LightParameters light = lightData.lights[i];
        float3 lightDir = light.position - in.worldPos;
        float distance = length(lightDir);
        float3 L = normalize(lightDir);
        
        // Simple linear attenuation: 1 - (dist/range)
        float attenuation = max(0.0, 1.0 - (distance / light.range));
        
        // Standard quadratic attenuation can be used too: 1.0 / (1.0 + 0.1*dist + 0.01*dist*dist)
        
        if (attenuation > 0.0) {
            float3 lightColor = light.color * light.intensity * attenuation;
            
            // Diffuse
            float NdotL = max(dot(N, L), 0.0);
            totalDiffuse += lightColor * NdotL;
            
            // Specular
            if (NdotL > 0.0) {
                float3 H = normalize(L + V);
                float NdotH = max(dot(N, H), 0.0);
                float spec = pow(NdotH, 32.0);
                totalSpecular += lightColor * spec;
            }
        }
    }
    
    // Ambient
    float3 ambient = albedo * 0.1;
    
    float3 finalColor = ambient + albedo * totalDiffuse + totalSpecular;
    
    return float4(finalColor, 1.0);
}
