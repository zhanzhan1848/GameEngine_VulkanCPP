#include "Common.h"

// ForwardLightBuffer is not in CommonTypes.metal, so we define it here.
// But we use the types from CommonTypes.metal for its members.
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
    float4 worldPos = perObject.World * float4(in.position, 1.0);
    out.position = perObject.WorldViewProjection * float4(in.position, 1.0);
    out.worldPos = worldPos.xyz;
    
    // Use InvWorld (Inverse) and transpose it for normals.
    // Metal matrices are column-major.
    // transpose(InvWorld) * normal
    out.worldNormal = (transpose(perObject.InvWorld) * float4(in.normal, 0.0)).xyz;
    
    out.color = in.color;
    return out;
}

fragment float4 fragmentMain(
    VertexOut in [[stage_in]],
    constant GlobalShaderData& globalData [[buffer(11)]],
    constant ForwardLightBuffer& lightData [[buffer(12)]],
    constant MaterialUniforms& material [[buffer(3)]],
    constant PerObjectData& perObject [[buffer(10)]])
{
    float3 N = normalize(in.worldNormal);
    float3 V = normalize(globalData.CameraPositionAndViewWidth.xyz - in.worldPos);
    
    float3 totalDiffuse = float3(0.0);
    float3 totalSpecular = float3(0.0);
    float3 albedo = in.color * material.color.rgb;
    
    // Directional Lights
    for(uint i = 0; i < globalData.NumDirectionalLights; ++i) {
        // Use referencing to avoid copy
        constant DirectionalLightParameters& light = lightData.directionalLights[i];
        float3 L = normalize(-light.DirectionAndIntensity.xyz);
        float intensity = light.DirectionAndIntensity.w;
        float3 lightColor = light.Color.rgb * intensity;
        
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
    // Note: ForwardLightBuffer defines 'punctualLightCount' but GlobalShaderData has 'NumDirectionalLights'.
    // We trust lightData.punctualLightCount here.
    for(uint i = 0; i < lightData.punctualLightCount; ++i) {
        constant LightParameters& light = lightData.lights[i];
        float3 lightDir = light.Position - in.worldPos;
        float distance = length(lightDir);
        float3 L = normalize(lightDir);
        
        // Simple linear attenuation
        float attenuation = max(0.0, 1.0 - (distance / light.Range));
        
        if (attenuation > 0.0) {
            float3 lightColor = light.Color * light.Intensity * attenuation;
            
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
    
    // Ambient (SH)
    // EvalSH9Irradiance is defined in CommonFunction.metal (included via Common.h)
    float3 irradiance = EvalSH9Irradiance(N, perObject.sh_coeffs);
    float3 ambient = albedo * irradiance;
    
    float3 finalColor = ambient + albedo * totalDiffuse + totalSpecular;
    
    return float4(finalColor, 1.0);
}
