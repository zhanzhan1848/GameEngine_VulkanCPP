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
    uint numPunctualLights;
    float deltaTime;
    float frameCount;
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

    int lightType;
    int shadowIndex;
    float padding;
    
    float4x4 viewProjection;
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

// Chebyshev Upper Bound for VSM
float ChebyshevUpperBound(float2 moments, float t, float minVariance) {
    // t is the current depth (dist to light)
    if (t <= moments.x) return 1.0;
    
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, minVariance);
    
    float d = t - moments.x;
    float p_max = variance / (variance + d * d);
    
    // Reduce light bleeding
    // 恢复下限到 0.05 以保留接触阴影细节，减少悬浮感
    return smoothstep(0.05, 1.0, p_max);
}

// Interleaved Gradient Noise for dithering
float InterleavedGradientNoise(float2 position_screen) {
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(position_screen, magic.xy)));
}

// VSM Filter Helper with Poisson Sampling
float SampleShadowVSM(texture2d_array<float> shadowMap, sampler shadowSampler, float3 shadowCoord, float layer, float minVariance, float2 screenPos) {
    // 16-Tap Poisson Disk Samples (Unit Circle)
    const float2 poissonDisk[16] = {
        float2( -0.94201624, -0.39906216 ), float2( 0.94558609, -0.76890725 ), float2( -0.09418410, -0.92938870 ), float2( 0.34495938, 0.29387760 ),
        float2( -0.91588581, 0.45771432 ), float2( -0.81544232, -0.87912464 ), float2( -0.38277543, 0.27676845 ), float2( 0.97484398, 0.75648379 ),
        float2( 0.44323325, -0.97511554 ), float2( 0.53742981, -0.47373420 ), float2( -0.26496911, -0.41893023 ), float2( 0.79197514, 0.19090188 ),
        float2( -0.24188840, 0.99706507 ), float2( -0.81409955, 0.91437590 ), float2( 0.19984126, 0.78641367 ), float2( 0.14383161, -0.14100790 )
    };

    float2 moments = float2(0.0);
    float spread = 0.0015; // Filter radius

    // Random Rotation
    float noise = InterleavedGradientNoise(screenPos);
    float angle = noise * 6.28318530718;
    float s = sin(angle);
    float c = cos(angle);

    // Accumulate moments from samples
    for (int i = 0; i < 16; ++i) {
        float2 diskOffset = poissonDisk[i];
        float2 rotatedOffset = float2(
            diskOffset.x * c - diskOffset.y * s,
            diskOffset.x * s + diskOffset.y * c
        );
        float2 offset = rotatedOffset * spread;
        moments += shadowMap.sample(shadowSampler, shadowCoord.xy + offset, uint(layer)).xy;
    }
    moments /= 16.0;

    return ChebyshevUpperBound(moments, shadowCoord.z, minVariance);
}

// Point Shadow Helper (Cube)
float SamplePointShadow(depthcube_array<float> shadowMap, sampler shadowSampler, float3 dir, float dist, float near, float far, float bias, int layer) {
    // Calculate depth value for the given distance in shadow map space
    // Standard perspective projection: A + B/z => depth = F/(F-N) - (F*N)/(dist*(F-N))
    float depth = far / (far - near) - (far * near) / (dist * (far - near));
    
    // Sample with comparison
    return shadowMap.sample_compare(shadowSampler, dir, uint(layer), depth - bias);
}

// Point Shadow Helper for VSM
float SamplePointShadowVSM(texturecube_array<float> shadowMap, sampler shadowSampler, float3 dir, float dist, float near, float far, float minVariance, int layer, float2 screenPos) {
    // Calculate linear depth [0, 1]
    float linearDepth = (dist - near) / (far - near);
    
    // 16-Tap Poisson Disk Samples
    const float2 poissonDisk[16] = {
        float2( -0.94201624, -0.39906216 ), float2( 0.94558609, -0.76890725 ), float2( -0.09418410, -0.92938870 ), float2( 0.34495938, 0.29387760 ),
        float2( -0.91588581, 0.45771432 ), float2( -0.81544232, -0.87912464 ), float2( -0.38277543, 0.27676845 ), float2( 0.97484398, 0.75648379 ),
        float2( 0.44323325, -0.97511554 ), float2( 0.53742981, -0.47373420 ), float2( -0.26496911, -0.41893023 ), float2( 0.79197514, 0.19090188 ),
        float2( -0.24188840, 0.99706507 ), float2( -0.81409955, 0.91437590 ), float2( 0.19984126, 0.78641367 ), float2( 0.14383161, -0.14100790 )
    };

    float3 L = normalize(dir);
    
    // Build basis (Tangent Frame)
    float3 up = abs(L.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 right = normalize(cross(up, L));
    up = cross(L, right);
    
    // Random Rotation
    float noise = InterleavedGradientNoise(screenPos);
    float angle = noise * 6.28318530718;
    float s = sin(angle);
    float c = cos(angle);
    
    float2 moments = float2(0.0);
    float spread = 0.003; // Slightly increased from 0.002 for softer noise

    for (int i = 0; i < 16; ++i) {
        float2 diskOffset = poissonDisk[i];
        
        // Rotate offset
        float2 rotatedOffset = float2(
            diskOffset.x * c - diskOffset.y * s,
            diskOffset.x * s + diskOffset.y * c
        );
        
        float3 offset = (right * rotatedOffset.x + up * rotatedOffset.y) * spread;
        moments += shadowMap.sample(shadowSampler, L + offset, uint(layer)).xy;
    }
    moments /= 16.0;

    return ChebyshevUpperBound(moments, linearDepth, minVariance);
}

// --- Shadow Generation Shaders (VSM) ---

struct ShadowVertexOut {
    float4 position [[position]];
    float4 worldPos;
};

vertex ShadowVertexOut vertexShadowVSM(VertexIn in [[stage_in]],
                                       constant PerObjectData& perObject [[buffer(10)]])
{
    ShadowVertexOut out;
    out.position = perObject.worldViewProjection * float4(in.position, 1.0);
    out.worldPos = perObject.world * float4(in.position, 1.0);
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
    float dx = dfdx(depth);
    float dy = dfdy(depth);
    moment2 += 0.25 * (dx * dx + dy * dy);
    
    ShadowFragmentOut out;
    out.moments = float2(moment1, moment2);
    return out;
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
    float4 viewPos = globalData.view * worldPos;
    out.viewDepth = -viewPos.z; // Positive depth
    
    return out;
}

// Fragment Shader
fragment float4 fragment_main(VertexOut in [[stage_in]],
                            constant ForwardLightBuffer& lightData [[buffer(12)]],
                            constant MaterialUniforms& material [[buffer(3)]],
                            texture2d_array<float> shadowMap [[texture(13)]],
                            texturecube_array<float> shadowCubeMap [[texture(14)]],
                            sampler shadowSampler [[sampler(13)]]) {
    
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
    
    // DEBUG: Visualize Shadow Factor
    // return float4(float3(shadowFactor), 1.0);
    
    // DEBUG: Visualize Cascade Index
    /*
    if (cascadeIndex == 0) return float4(1.0, 0.0, 0.0, 1.0);
    if (cascadeIndex == 1) return float4(0.0, 1.0, 0.0, 1.0);
    if (cascadeIndex == 2) return float4(0.0, 0.0, 1.0, 1.0);
    return float4(1.0, 1.0, 0.0, 1.0);
    */

    // DEBUG: Visualize Shadow Coord Z
    // return float4(shadowCoord.z, shadowCoord.z, shadowCoord.z, 1.0);

    // DEBUG: Visualize Moments
    /*
    float2 m = shadowMap.sample(shadowSampler, shadowCoord.xy, uint(cascadeIndex)).xy;
    return float4(m.x, m.y, 0.0, 1.0);
    */

    
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
                 float3 dir = in.worldPos - pLight.position;
                 pShadow = SamplePointShadowVSM(shadowCubeMap, shadowSampler, dir, dist, 0.1, pLight.range, minVariance, pLight.shadowIndex, in.position.xy);
            }
        }
        
        totalDiffuse += material.color.rgb * pLight.color * pLight.intensity * att * NdotL_p * pShadow;
    }

    float3 ambient = material.color.rgb * 0.2; // 0.2 Ambient
    
    return float4(totalDiffuse + ambient, 1.0);
}
