/**
 * @file DebugTexture.metal
 * @brief Debug visualization shader for GBuffer and Shadow Maps
 * @details Visualizes GBuffer attachments, reconstructed World Pos, and Shadow Maps/Terms
 * @author GameEngine VulkanCPP Team
 * @date 2026-02-11
 * @version 1.0.0
 */

#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// Scene Data for Reconstructing World Pos and N.L
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

vertex VertexOut vertexDebug(uint vertexID [[vertex_id]]) {
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

float GetShadow(float3 worldPos, float4x4 shadowMatrix, texture2d<float> shadowMap) {
    constexpr sampler shadowSampler(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
    float4 clipPos = shadowMatrix * float4(worldPos, 1.0);
    float3 shadowCoord = clipPos.xyz / clipPos.w;
    shadowCoord.x = shadowCoord.x * 0.5 + 0.5;
    shadowCoord.y = shadowCoord.y * -0.5 + 0.5;
    
    if (shadowCoord.x < 0.0 || shadowCoord.x > 1.0 || 
        shadowCoord.y < 0.0 || shadowCoord.y > 1.0 || 
        shadowCoord.z < 0.0 || shadowCoord.z > 1.0) {
        return 1.0;
    }
    
    float closestDepth = shadowMap.sample(shadowSampler, shadowCoord.xy).r;
    float currentDepth = shadowCoord.z;
    float bias = 0.00005;
    
    return (currentDepth - bias > closestDepth) ? 0.0 : 1.0;
}

// 6 Screen Sections: 2x3 Grid
// 0: Final Color (Top-Left)
// 1: Normal (Top-Mid)
// 2: World Pos (Top-Right)
// 3: N dot L (Bottom-Left)
// 4: Shadow Term (Bottom-Mid)
// 5: Shadow Map (Bottom-Right)

fragment float4 fragmentDebug(
    VertexOut in [[stage_in]], 
    texture2d<float> lightingOutput [[texture(2)]],
    texture2d<float> normalTex [[texture(3)]],
    texture2d<float> depthTex [[texture(4)]],
    texture2d<float> shadowMap [[texture(5)]],
    constant ViewData& viewData [[buffer(0)]],
    constant SceneData& sceneData [[buffer(1)]]
) {
    constexpr sampler s(min_filter::nearest, mag_filter::nearest);
    
    float2 uv = in.uv;
    
    // Determine Grid Cell
    // Grid 3x2 (Columns x Rows)
    // Col 0: 0.0 - 0.33
    // Col 1: 0.33 - 0.66
    // Col 2: 0.66 - 1.0
    // Row 0: 0.5 - 1.0 (Top)
    
    int col = int(uv.x * 3.0);
    int row = int(uv.y * 2.0); 
    
    // Local UV within cell
    float2 localUV = float2(fract(uv.x * 3.0), fract(uv.y * 2.0));
    
    // Sample GBuffer
    float4 normalSample = normalTex.sample(s, localUV);
    float3 N = normalize(normalSample.rgb * 2.0 - 1.0);
    
    // Reconstruct World Pos
    float depth = depthTex.sample(s, localUV).r;
    
    // Discard background to avoid noise (optional, but good for debug)
    bool isBackground = (depth >= 1.0);
    
    // Metal NDC: Z [0, 1], Y [-1, 1] (Y Down: -1 Top, 1 Bottom) -> This comment was wrong. Metal is Y-Up (-1 Bottom, 1 Top)
    float2 ndc;
    ndc.x = localUV.x * 2.0 - 1.0;
    ndc.y = (1.0 - localUV.y) * 2.0 - 1.0; // Flip Y (Top UV=0 -> Top NDC=1)

    // Test Z Ranges:
    // Left half of cell: Assume [0, 1] (Standard Metal/Vulkan)
    // Right half of cell: Assume [-1, 1] (GL Standard)
    float z_01 = depth;
    float z_11 = depth * 2.0 - 1.0;
    
    float z = (localUV.x < 0.5) ? z_01 : z_11;

    float4 clipPos = float4(ndc, z, 1.0);

    float4 worldPos4 = viewData.invViewProjection * clipPos;
    float3 worldPos = worldPos4.xyz / worldPos4.w;
    
    // Light Dir
    float3 L = normalize(sceneData.lightPos.xyz - worldPos); // Matches Lighting Shader logic
    
    // Output
    float3 result = float3(0,0,0);
    
    if (row == 0) {
        if (col == 0) {
            // Final Color (TAA/Lighting Output)
            result = lightingOutput.sample(s, localUV).rgb;
        } else if (col == 1) {
            // Normal
            result = normalSample.rgb; // 0..1 range
        } else {
            // World Pos (Top Right)
            if (isBackground) {
                result = float3(0.1, 0.1, 0.1); // Dark Gray for Background
            } else {
                // Reconstruct World Pos
                // Metal Standard Z: [0, 1]
                float z = depth;
                float4 clipPos = float4(ndc, z, 1.0);
                float4 worldPos4 = viewData.invViewProjection * clipPos;
                float3 worldPos = worldPos4.xyz / worldPos4.w;
                
                // Use fract to visualize coordinate continuity
                // If WorldPos is correct, this pattern should stick to surfaces
                // and NOT slide when camera moves.
                result = fract(worldPos.xyz * 1.0); // 1.0 scale = 1 meter grid
            }
        }
    } else {
        if (col == 0) {
            // NDC Debug (Bottom Left)
            // Visualize NDC coordinates to verify Y-axis
            // Red = NDC.x (-1..1 -> 0..1), Green = NDC.y (-1..1 -> 0..1)
            result = float3(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5, 0.0);
        } else if (col == 1) {
            // Shadow Term Debugging (Bottom Mid)
            if (isBackground) {
                result = float3(0.0);
            } else {
                // Reconstruct World Pos
                float z = depth;
                float4 clipPos = float4(ndc, z, 1.0);
                float4 worldPos4 = viewData.invViewProjection * clipPos;
                float3 worldPos = worldPos4.xyz / worldPos4.w;
                
                // Shadow Term
                float shadow = GetShadow(worldPos, sceneData.shadowMatrix0, shadowMap);
                result = float3(shadow);
            }
        } else {
            // Shadow Map (Raw Depth)
            float shadowDepth = shadowMap.sample(s, localUV).r;
            // Visualize raw depth: Invert to see variations (Near=0(Black) -> Far=1(White))
            // If we invert: Near=1(White), Far=0(Black). 
            // Since clear is 1.0 (Far), empty areas will be Black. Objects will be brighter.
            result = float3(1.0 - shadowDepth); 
        }
    }
    
    // Draw Grid Lines
    if (localUV.x < 0.01 || localUV.y < 0.01) {
        return float4(1, 1, 0, 1); // Yellow Lines
    }
    
    // Draw Sub-Split Line for Shadow Term
    if (row == 1 && col == 1 && abs(localUV.x - 0.5) < 0.005) {
        return float4(0, 1, 1, 1); // Cyan Line
    }
    
    return float4(result, 1.0);
}
