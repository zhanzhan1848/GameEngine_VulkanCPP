#include "../../Engine/Graphics/RHI/Shaders/RHIShaderCommon.metal"

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

// --- Shadow Generation Shaders (VSM) ---

struct ShadowVertexOut {
    float4 position [[position]];
    float4 worldPos;
};

vertex ShadowVertexOut vertexShadowVSM(VertexIn in [[stage_in]],
                                       constant PerObjectData& perObject [[buffer(10)]],
                                       constant GlobalShaderData& globalData [[buffer(11)]])
{
    ShadowVertexOut out;
    float4 worldPos = perObject.world * float4(in.position, 1.0);
    out.position = globalData.viewProjection * worldPos;
    out.worldPos = worldPos;
    return out;
}

struct ShadowFragmentOut {
    float2 moments [[color(0)]];
};

fragment ShadowFragmentOut fragmentShadowVSM(ShadowVertexOut in [[stage_in]])
{
    // Depth is in in.position.z (Screen space [0, 1])
    float depth = in.position.z;
    
    // Compute moments
    float moment1 = depth;
    float moment2 = depth * depth;
    
    // Adjust 2nd moment using partial derivatives to reduce acne
    // float dx = dfdx(depth);
    // float dy = dfdy(depth);
    // moment2 += 0.25 * (dx * dx + dy * dy);
    
    ShadowFragmentOut out;
    out.moments = float2(moment1, moment2);
    return out;
}

vertex VertexOut vertexMain(
    VertexIn in [[stage_in]],
    constant PerObjectData& perObject [[buffer(10)]],
    constant GlobalShaderData& globalData [[buffer(11)]],
    uint vertexID [[vertex_id]])
{
    VertexOut out;
    
    // Calculate World Position
    float4 worldPos = perObject.world * float4(in.position, 1.0);
    
    // Calculate Clip Position
    out.position = globalData.viewProjection * worldPos;
    
    out.worldPos = worldPos.xyz;
    out.worldNormal = normalize((perObject.world * float4(in.normal, 0.0)).xyz);
    out.color = in.color;
    
    // Linear View Depth for Shadow Map Selection
    out.viewDepth = (globalData.view * worldPos).z;
    
    return out;
}

// Fragment Shader
fragment float4 fragment_main(VertexOut in [[stage_in]],
                            constant ForwardLightBuffer& lightData [[buffer(12)]],
                            constant MaterialUniforms& material [[buffer(3)]],
                            texture2d_array<float> shadowMap [[texture(13)]],
                            // texturecube_array<float> shadowCubeMap [[texture(14)]], // Removed
                            sampler shadowSampler [[sampler(13)]]) {
    
    // DEBUG: Force Red Color to verify Geometry
    // return float4(1.0, 0.0, 0.0, 1.0);

    
    // Extract Directional Light (Assume index 0)
    DirectionalLightParameters light = lightData.directionalLights[0];

    float3 N = normalize(in.worldNormal);
    float3 L = normalize(-light.directionAndIntensity.xyz);
    
    // Simple Lambert
    float NdotL = max(dot(N, L), 0.0);
    
    // Shadow Calculation
    float shadowFactor = 1.0;
    
    uint cascadeIndex = 0;
    float4 shadowCoord = float4(0.0);
    float2 shadowUV = float2(0.0);
    float minVariance = 0.00002; // VSM Minimum Variance to avoid numeric instability

    if (light.colorAndShadow.a > 0.0) {
        // Select Cascade
        cascadeIndex = 3;
        
        if (in.viewDepth < light.splits[0]) {
            cascadeIndex = 0;
        } else if (in.viewDepth < light.splits[1]) {
            cascadeIndex = 1;
        } else if (in.viewDepth < light.splits[2]) {
            cascadeIndex = 2;
        }
        
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
                 
                 // Sample VSM with Poisson Distribution
                 shadowFactor = SampleShadowVSM(shadowMap, shadowSampler, float3(shadowUV, shadowCoord.z), float(cascadeIndex), minVariance, in.position.xy);
            }
        }

    // Accumulate Lighting
    float3 totalDiffuse = material.color.rgb * light.colorAndShadow.rgb * NdotL * shadowFactor;
    
    // Punctual Lights Loop
    for (uint i = 0; i < lightData.punctualLightCount; ++i) {
        LightParameters pLight = lightData.lights[i];
        
        float3 L_vec = pLight.position - in.worldPos;
        float dist = length(L_vec);
        float3 L_dir = normalize(L_vec);
        
        if (dist > pLight.range) continue;
        
        // Attenuation
        float distSq = dist * dist;
        float rangeSq = pLight.range * pLight.range;
        float att = max(0.0, 1.0 - distSq*distSq/(rangeSq*rangeSq));
        att *= att;
        
        // Spot Light Cone
        if (pLight.lightType == 2) { // Spot
            float cosAngle = dot(-L_dir, normalize(pLight.direction));
            if (cosAngle < pLight.cosUmbra) {
                 att = 0.0;
            } else {
                 float t = (cosAngle - pLight.cosUmbra) / (pLight.cosPenumbra - pLight.cosUmbra);
                 att *= smoothstep(0.0, 1.0, t);
            }
        }
        
        if (att <= 0.0) continue;
        
        float NdotL_p = max(dot(N, L_dir), 0.0);
        float pShadow = 1.0;
        
        if (pLight.shadowIndex >= 0) {
            
            if (pLight.lightType == 2) { // Spot Shadow
                 float4 pShadowCoord = pLight.viewProjection * float4(in.worldPos, 1.0);
                 pShadowCoord.xyz /= pShadowCoord.w;
                 
                 float2 pShadowUV;
                 pShadowUV.x = pShadowCoord.x * 0.5 + 0.5;
                 pShadowUV.y = -pShadowCoord.y * 0.5 + 0.5;
                 
                 if (pShadowUV.x >= 0.0 && pShadowUV.x <= 1.0 && 
                     pShadowUV.y >= 0.0 && pShadowUV.y <= 1.0 && 
                     pShadowCoord.z >= 0.0 && pShadowCoord.z <= 1.0) {
                      pShadow = SampleShadowVSM(shadowMap, shadowSampler, float3(pShadowUV, pShadowCoord.z), float(pLight.shadowIndex), minVariance, in.position.xy);
                 } else {
                      pShadow = 1.0;
                 }
            } else if (pLight.lightType == 1) { // Point Shadow
                 // Direction from light to pixel
                 // float3 dir = in.worldPos - pLight.position;
                 // pShadow = SamplePointShadowVSM(shadowCubeMap, shadowSampler, dir, dist, 0.1, pLight.range, minVariance, pLight.shadowIndex, in.position.xy);
                 pShadow = 1.0;
            }
        }
        
        totalDiffuse += material.color.rgb * pLight.color * pLight.intensity * att * NdotL_p * pShadow;
    }

    float3 ambient = material.color.rgb * 0.2; // 0.2 Ambient
    
    return float4(totalDiffuse + ambient, 1.0);
}
