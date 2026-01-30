#define RHI_ENABLE_PBR
#include "../../Engine/Graphics/RHI/Shaders/RHIShaderCommon.metal"
#include <simd/simd.h>

using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv       [[attribute(2)]];
    float3 color    [[attribute(3)]]; // Added Color
};

struct FragmentIn {
    float4 position [[position]];
    float4 worldPos [[user(loc10)]];
    float4 normal [[user(loc11)]];
    float4 uv [[user(loc12)]];
    float4 color [[user(loc13)]]; // Pass Color
    float4 currentClipPos [[user(loc14)]]; // For Screen Space UV
    float4 previousClipPos [[user(loc15)]]; // For Reprojection UV
};

struct VertexOut {
    float4 position [[position]];
    float4 worldPos [[user(loc10)]];
    float4 normal [[user(loc11)]];
    float4 uv [[user(loc12)]];
    float4 color [[user(loc13)]]; // Pass Color
    float4 currentClipPos [[user(loc14)]]; // For Screen Space UV
    float4 previousClipPos [[user(loc15)]]; // For Reprojection UV
    uint layer [[render_target_array_index]]; // REMOVED FOR DEBUGGING
};

struct Uniforms {
    float4x4 viewProjections[6];
    float4x4 previousViewProjections[6]; // Added for Reprojection UV
};

struct SceneData {
    float4x4 model;
    float4 lightPos;
    float4 lightColor;
    float4 reflectionPlane;
    float4 reflectionPlane2;
    float4 reflectionPlane3;
    float4x4 previousModel; // Added for Motion Vectors
    float2 jitter; // Added for TAA
    float2 previousJitter; // Added for TAA
    float4 shCoeffs[9]; // Added SH Coefficients (L2)
};

// Alias PushConstants to SceneData for compatibility
typedef SceneData PushConstants;

// Single View Vertex Shader (No Layer Output)
struct VertexOutSingle {
    float4 position [[position]];
    float3 worldPos;
    float3 normal;
    float2 uv;
    float3 color;
    float4 currentClipPos;
    float4 previousClipPos;
};

// Output structure for Main Pass (Color + Velocity)
struct FragmentOutSingle {
    float4 color [[color(0)]];
    float2 velocity [[color(1)]];
};

// Reflection Pass Vertex Shader (with Clip Distance)
struct VertexOutReflection {
    float4 position [[position]];
    float4 worldPos [[user(loc10)]];
    float4 normal   [[user(loc11)]];
    float4 uv       [[user(loc12)]];
    float4 color    [[user(loc13)]];
    float4 currentClipPos [[user(loc14)]];
    float4 previousClipPos [[user(loc15)]];
    float clipDistance [[clip_distance]];
};

struct ReflectionUniforms {
    float4 plane; // Plane Equation (Ax + By + Cz + D = 0)
};

vertex VertexOutReflection vertexMainReflection(VertexIn in [[stage_in]],
                                                constant Uniforms& uniforms [[buffer(1)]],
                                                constant SceneData& scene [[buffer(2)]],
                                                constant ReflectionUniforms& reflection [[buffer(3)]]) {
    VertexOutReflection out;
    
    // Model Matrix
    float4 worldPos = scene.model * float4(in.position, 1.0);
    out.worldPos = worldPos;
    
    // Normal Matrix
    out.normal = scene.model * float4(in.normal, 0.0);
    
    out.uv = float4(in.uv, 0.0, 0.0);
    out.color = float4(in.color, 1.0);
    
    // Use View 0 (Reflection View stored in slot 0 of this special uniform buffer or just reuse existing Uniforms if we update it)
    // We will assume 'uniforms' passed here contains the Reflection View Matrix in viewProjections[0]
    out.position = uniforms.viewProjections[0] * worldPos;
    
    // Fill unused attributes to match FragmentIn
    out.currentClipPos = out.position;
    out.previousClipPos = out.position; // No motion vectors for reflection pass yet
    
    // Calculate Clip Distance
    // Distance = dot(plane.xyz, worldPos.xyz) + plane.w
    out.clipDistance = dot(reflection.plane.xyz, worldPos.xyz) + reflection.plane.w;
    
    return out;
}

// Reuse fragmentMainSingle for reflection pass (it's just rendering the scene)

// Mirror Object Vertex Shader
struct VertexOutMirror {
    float4 position [[position]];
    float4 clipPos; // For Screen Space UV calculation
    float3 worldPos;
    float3 normal;
};

vertex VertexOutMirror vertexMainMirror(VertexIn in [[stage_in]],
                                        constant Uniforms& uniforms [[buffer(1)]],
                                        constant SceneData& scene [[buffer(2)]]) {
    VertexOutMirror out;
    
    float4 worldPos = scene.model * float4(in.position, 1.0);
    out.worldPos = worldPos.xyz;
    out.normal = (scene.model * float4(in.normal, 0.0)).xyz;
    
    out.position = uniforms.viewProjections[0] * worldPos;
    out.clipPos = out.position;
    
    return out;
}

// Mirror Object Fragment Shader
fragment float4 fragmentMainMirror(VertexOutMirror in [[stage_in]],
                                   texture2d<float> reflectionTex [[texture(0)]],
                                   texture2d<float> reflectionTex2 [[texture(3)]],
                                   texture2d<float> reflectionTex3 [[texture(4)]],
                                   constant SceneData& scene [[buffer(2)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    
    float3 N = normalize(in.normal);
    float3 planeN = normalize(scene.reflectionPlane.xyz);
    float3 planeN2 = normalize(scene.reflectionPlane2.xyz);
    float3 planeN3 = normalize(scene.reflectionPlane3.xyz);
    
    float3 L = normalize(scene.lightPos.xyz - in.worldPos);
    float diff = max(dot(N, L), 0.0);
    float4 baseColor = float4(0.1, 0.1, 0.2, 1.0);
    
    // Check alignment. Plane Normal and Surface Normal should be parallel.
    // Relaxed threshold to 0.5 (approx 60 degrees) to account for interpolation errors or slight misalignments
    if (dot(N, planeN) > 0.5) {
        // Calculate Screen Space UV
        // Clip Space: (-1 to 1) -> UV: (0 to 1)
        float2 uv = (in.clipPos.xy / in.clipPos.w) * 0.5 + 0.5;
        uv.y = 1.0 - uv.y; // Flip Y for Metal texture sampling
        
        float4 reflectionColor = reflectionTex.sample(s, uv);
        
        // Return Reflection + Base, Alpha 0 to disable SSR overlap on this face
        return float4(mix(baseColor.rgb * diff, reflectionColor.rgb, 0.8), 0.0);
    } else if (dot(N, planeN2) > 0.5) {
        // Second Reflection Plane
        float2 uv = (in.clipPos.xy / in.clipPos.w) * 0.5 + 0.5;
        uv.y = 1.0 - uv.y; 
        uv.x = 1.0 - uv.x; // Fix Left/Right inversion for Right Face
        
        float4 reflectionColor = reflectionTex2.sample(s, uv);
        
        return float4(mix(baseColor.rgb * diff, reflectionColor.rgb, 0.8), 0.0);
    } else if (dot(N, planeN3) > 0.5) {
        // Third Reflection Plane (Top Face)
        float2 uv = (in.clipPos.xy / in.clipPos.w) * 0.5 + 0.5;
        uv.y = 1.0 - uv.y; 
        uv.x = 1.0 - uv.x; // Fix Left/Right inversion for Top Face
        
        float4 reflectionColor = reflectionTex3.sample(s, uv);
        
        return float4(mix(baseColor.rgb * diff, reflectionColor.rgb, 0.8), 0.0);
    } else {
        // Standard Rendering for other faces (fallback to SSR)
                // Add Specular for better light reaction
                // Actually, let's use a fixed view vector or just Blinn-Phong with constant view
                float3 viewDir = normalize(float3(0.0, 0.0, 18.0) - in.worldPos); // Main camera pos
                float3 H = normalize(L + viewDir);
                float spec = pow(max(dot(N, H), 0.0), 32.0);
                
                float3 finalColor = baseColor.rgb * diff * scene.lightColor.rgb + float3(1.0) * spec * scene.lightColor.rgb;
                
                // Return Base Color + Specular, Alpha 1 to enable SSR
                return float4(finalColor, 1.0);
            }
}

vertex VertexOutSingle vertexMainSingle(VertexIn in [[stage_in]],
                                        constant Uniforms& uniforms [[buffer(1)]],
                                        constant SceneData& scene [[buffer(2)]]) {
    VertexOutSingle out;
    
    // Model Matrix
    float4 worldPos = scene.model * float4(in.position, 1.0);
    out.worldPos = worldPos.xyz;
    
    // Normal Matrix
    out.normal = (scene.model * float4(in.normal, 0.0)).xyz;
    
    out.uv = in.uv;
    out.color = in.color;
    
    // Use View 0 (Main View) - This is Jittered
    out.position = uniforms.viewProjections[0] * worldPos;
    
    // Unjittered Current Position for Velocity
    // We assume uniforms.viewProjections[0] HAS jitter.
    // We subtract it to get unjittered position.
    out.currentClipPos = out.position;
    out.currentClipPos.xy -= scene.jitter * out.position.w;
    
    // Previous Position (Unjittered)
    // We assume uniforms.previousViewProjections[0] has NO jitter.
    float4 prevWorldPos = scene.previousModel * float4(in.position, 1.0);
    out.previousClipPos = uniforms.previousViewProjections[0] * prevWorldPos;
    
    return out;
}

fragment FragmentOutSingle fragmentMainSingle(VertexOutSingle in [[stage_in]],
                                   constant SceneData& scene [[buffer(2)]]) {
    FragmentOutSingle out;

    // Light
    float3 N = normalize(in.normal);
    float3 lightDir = normalize(scene.lightPos.xyz - in.worldPos);
    float3 viewDir = normalize(float3(0, 0, 18.0) - in.worldPos); // Matches Main Camera Pos
    
    // DEBUG: Output Normal
    // return float4(N * 0.5 + 0.5, 1.0);
    
    // Ambient
    float3 ambient = float3(0.05, 0.05, 0.05) * in.color;
    
    // Diffuse
    float diff = max(dot(N, lightDir), 0.0);
    float dist = length(scene.lightPos.xyz - in.worldPos);
    
    // Attenuation (Point Light)
    // 1.0 / (1.0 + 0.1*d + 0.01*d*d)
    float atten = 1.0 / (1.0 + 0.05 * dist + 0.005 * dist * dist);
    
    float3 diffuse = diff * in.color * scene.lightColor.rgb * atten;
    
    // Specular (Blinn-Phong)
    float3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(N, halfwayDir), 0.0), 32.0);
    float3 specular = scene.lightColor.rgb * spec * atten;
    
    out.color = float4(ambient + diffuse + specular, 1.0);
    
    // Velocity Calculation
    float2 screenUV = (in.currentClipPos.xy / in.currentClipPos.w) * 0.5 + 0.5;
    screenUV.y = 1.0 - screenUV.y; // Flip Y
    
    float2 prevScreenUV = (in.previousClipPos.xy / in.previousClipPos.w) * 0.5 + 0.5;
    prevScreenUV.y = 1.0 - prevScreenUV.y;
    
    out.velocity = screenUV - prevScreenUV;
    
    return out;
}


/**
 * @brief Multi-View Vertex Shader.
 * 
 * Handles:
 * - World Space transformation.
 * - View-Projection for specific CubeMap face (based on InstanceID).
 * - Previous Frame Position calculation for Motion Vectors.
 * 
 * @param in Vertex Input attributes.
 * @param uniforms View-Projection matrices (Current and Previous).
 * @param scene Scene Data (Model Matrix, Light, etc.).
 * @param instanceID Used to select the CubeMap face (0-5).
 * @return VertexOut Transformed vertex data.
 */
vertex VertexOut vertexMain(VertexIn in [[stage_in]],
                            constant Uniforms& uniforms [[buffer(1)]],
                            constant SceneData& scene [[buffer(2)]],
                            uint instanceID [[instance_id]]) {
    VertexOut out;
    
    // Calculate World Position
    float4 worldPos = scene.model * float4(in.position, 1.0);
    out.worldPos = worldPos;
    out.normal = scene.model * float4(in.normal, 0.0);
    out.uv = float4(in.uv, 0.0, 0.0);
    out.color = float4(in.color, 1.0); // Pass Color
    
    // Select ViewProjection based on Instance ID (0-5)
    // instanceID corresponds to the face index
    out.position = uniforms.viewProjections[instanceID] * worldPos;
    out.layer = instanceID; // Route to correct array layer
    
    // Pass Clip Positions for UV Generation
    out.currentClipPos = out.position;
    
    // Calculate Previous Frame Position
    float4 prevWorldPos = scene.previousModel * float4(in.position, 1.0);
    out.previousClipPos = uniforms.previousViewProjections[instanceID] * prevWorldPos;
    
    return out;
}

/**
 * @brief Multi-View Fragment Shader Output.
 */
struct FragmentOut {
    float4 color [[color(0)]];
    float2 velocity [[color(1)]];
};

/**
 * @brief Multi-View Fragment Shader.
 * 
 * Handles:
 * - Phong Lighting (Ambient + Diffuse + Specular).
 * - Tone Mapping.
 * - Screen Space UV Generation (Independent per View).
 * - Reprojection UV / Motion Vector Calculation.
 * - Debug Visualization (Toggleable via scene.lightColor.w).
 * 
 * @param in Interpolated vertex data.
 * @param scene Scene Data (Light, Debug Flags).
 * @return FragmentOut Final pixel color and velocity.
 */
fragment FragmentOut fragmentMain(FragmentIn in [[stage_in]],
                             constant SceneData& scene [[buffer(2)]]) {
    FragmentOut out;

    // PBR Properties
    float3 N = normalize(in.normal.xyz);
    float3 V = normalize(-in.worldPos.xyz); // Assuming Camera/Probe at (0,0,0)
    
    float3 lightPos = scene.lightPos.xyz;
    float3 lightColor = scene.lightColor.rgb;
    float3 L = normalize(lightPos - in.worldPos.xyz);
    float3 H = normalize(V + L);
    
    float distToLight = length(lightPos - in.worldPos.xyz);
    float attenuation = 1.0 / (1.0 + 0.1 * distToLight + 0.01 * distToLight * distToLight);
    float3 radiance = lightColor * attenuation;

    // Material Parameters (Hardcoded for test scene)
    float3 albedo = in.color.rgb; 
    float roughness = 0.4;
    float metallic = 0.0; // Non-metal
    float3 F0 = float3(0.04); 
    F0 = mix(F0, albedo, metallic);

    // Calculate PBR Terms
    // 1. Specular Term (Cook-Torrance)
    // SpecularBRDF returns (D * G * F) / (4 * N.V * N.L)
    // Note: We need to recalculate F separately for Fresnel mix (kS) if we want exact energy conservation,
    // but SpecularBRDF already includes F in its result.
    // To properly blend Diffuse and Specular, we need kS (Fresnel).
    
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    float3 kS = F;
    float3 kD = float3(1.0) - kS;
    kD *= 1.0 - metallic;

    float NdotL = max(dot(N, L), 0.0);
    
    // Specular Contribution
    float3 specular = SpecularBRDF(N, V, L, roughness, F0);
    
    // Diffuse Contribution (Lambert)
    float3 diffuse = albedo * INV_PI; // albedo / PI
    
    // Combine (Lo)
    float3 Lo = (kD * diffuse + specular) * radiance * NdotL;
    
    // Ambient (SH Irradiance)
    float3 ambient = float3(0.0);
    
    // SH Basis Evaluation (Cosine Lobe Convolved)
    // L0
    float Y00 = 0.282095;
    float3 L00 = scene.shCoeffs[0].rgb;
    
    // L1
    float Y1m1 = 0.488603 * N.y;
    float Y10  = 0.488603 * N.z;
    float Y11  = 0.488603 * N.x;
    float3 L1m1 = scene.shCoeffs[1].rgb;
    float3 L10  = scene.shCoeffs[2].rgb;
    float3 L11  = scene.shCoeffs[3].rgb;
    
    // L2
    float Y2m2 = 1.092548 * N.x * N.y;
    float Y2m1 = 1.092548 * N.y * N.z;
    float Y20  = 0.315392 * (3.0 * N.z * N.z - 1.0);
    float Y21  = 1.092548 * N.x * N.z;
    float Y22  = 0.546274 * (N.x * N.x - N.y * N.y);
    float3 L2m2 = scene.shCoeffs[4].rgb;
    float3 L2m1 = scene.shCoeffs[5].rgb;
    float3 L20  = scene.shCoeffs[6].rgb;
    float3 L21  = scene.shCoeffs[7].rgb;
    float3 L22  = scene.shCoeffs[8].rgb;
    
    // Reconstruction (Cosine Convolution Factors included in Coefficients or applied here?)
    // Typically Irradiance SH coefficients are pre-convolved with Cosine Lobe.
    // If we assume input coefficients ARE irradiance coefficients:
    ambient = L00 * Y00 +
              L1m1 * Y1m1 + L10 * Y10 + L11 * Y11 +
              L2m2 * Y2m2 + L2m1 * Y2m1 + L20 * Y20 + L21 * Y21 + L22 * Y22;
              
    // Fallback if SH is zero (check L00)
    if (length(L00) < 0.001) {
        ambient = float3(0.03) * albedo;
    } else {
        ambient *= albedo / 3.14159; // Diffuse albedo modulation
    }
    
    float3 finalColor = ambient + Lo;
    
    // Tone mapping (Simple Reinhard)
    finalColor = ToneMapReinhard(finalColor);
    
    out.color = float4(finalColor, 1.0);
    
    // --- Multi-View UV Generation Logic ---
    float2 screenUV = (in.currentClipPos.xy / in.currentClipPos.w) * 0.5 + 0.5;
    screenUV.y = 1.0 - screenUV.y; // Flip Y for Metal Texture Coordinates
    
    float2 prevScreenUV = (in.previousClipPos.xy / in.previousClipPos.w) * 0.5 + 0.5;
    prevScreenUV.y = 1.0 - prevScreenUV.y;
    
    out.velocity = screenUV - prevScreenUV;
    
    // Debug Visualization Control
    if (scene.lightColor.w > 1.5) {
        out.color = float4(abs(out.velocity.x) * 100.0, abs(out.velocity.y) * 100.0, 0.0, 1.0);
    } else if (scene.lightColor.w > 0.5) {
        out.color = float4(screenUV.x, screenUV.y, 0.0, 1.0);
    }
    
    return out;
}

// Simple Shader for Direct Render Pass (Debugging)
struct VertexOutSimple {
    float4 position [[position]];
    float3 worldPos;
    float3 normal;
};

vertex VertexOutSimple vertexMainSimple(VertexIn in [[stage_in]],
                                        constant Uniforms& uniforms [[buffer(1)]],
                                        constant PushConstants& push [[buffer(2)]]) {
    VertexOutSimple out;
    float4 worldPos = push.model * float4(in.position, 1.0);
    out.worldPos = worldPos.xyz;
    out.normal = (push.model * float4(in.normal, 0.0)).xyz;
    out.position = uniforms.viewProjections[4] * worldPos; // Use +Z View
    return out;
}

fragment float4 fragmentMainSimple(VertexOutSimple in [[stage_in]]) {
    float3 N = normalize(in.normal);
    float3 L = normalize(float3(1.0, 1.0, 1.0));
    float diff = max(dot(N, L), 0.2);
    return float4(float3(1.0, 0.5, 0.2) * diff, 1.0); // Orange
}

// DEBUG SHADER: Ignores matrices, outputs raw NDC coordinates
vertex VertexOutSimple vertexMainDebug(VertexIn in [[stage_in]]) {
    VertexOutSimple out;
    out.worldPos = in.position;
    out.normal = float3(0, 0, 1);
    
    // Force Z to 0.5 (safe depth)
    // Scale X/Y by 0.5 to fit on screen
    // Front face positions are around Z=2, X=[-1,1], Y=[-1,1]
    // So we just ignore Z input and force it.
    
    out.position = float4(in.position.x * 0.5, in.position.y * 0.5, 0.5, 1.0);
    return out;
}

// SUPER SIMPLE DEBUG SHADER
struct SimpleDebugOut {
    float4 position [[position]];
};

vertex SimpleDebugOut vertexMainSimpleDebug(uint vertexID [[vertex_id]]) {
    SimpleDebugOut out;
    // Simple Triangle in center of screen
    float2 positions[3] = {
        float2( 0.0,  0.5), // Top
        float2( 0.5, -0.5), // Bottom Right
        float2(-0.5, -0.5)  // Bottom Left
    };
    
    float2 pos = positions[vertexID % 3];
    out.position = float4(pos, 0.5, 1.0); // Z=0.5
    return out;
}

// DEBUG SHADER: Uses actual Vertex Buffer input but hardcoded transform
vertex VertexOutSimple vertexMainBufferDebug(VertexIn in [[stage_in]]) {
    VertexOutSimple out;
    // Just pass through position with some scaling to ensure it's visible
    // Assuming model is around 0,0,0 with size ~1
    out.position = float4(in.position.x * 0.5, in.position.y * 0.5, 0.5, 1.0);
    out.worldPos = in.position;
    out.normal = in.normal;
    return out;
}

fragment float4 fragmentMainSimpleDebug(SimpleDebugOut in [[stage_in]]) {
    return float4(0.0, 1.0, 0.0, 1.0); // Green
}

// Blit Shader for Visualization
struct BlitVertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex BlitVertexOut blitVertex(uint vertexID [[vertex_id]]) {
    BlitVertexOut out;
    float2 positions[6] = {
        float2(-1, -1), float2( 1, -1), float2( 1,  1),
        float2(-1, -1), float2( 1,  1), float2(-1,  1)
    };
    out.position = float4(positions[vertexID], 0.5, 1.0); // Z=0.5 to avoid clipping
    out.uv = positions[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y; // Flip Y for Metal
    return out;
}

struct BlitUniforms {
    float4x4 rotation;
};

fragment float4 blitFragment(BlitVertexOut in [[stage_in]],
                             texturecube<float> cubeMap [[texture(0)]],
                             constant BlitUniforms& uniforms [[buffer(1)]]) {
    
    // Base Direction (Looking at +Z)
    float3 dir = float3(in.uv * 2.0 - 1.0, 1.0); 
    dir = normalize(dir);
    
    // Apply Rotation from Uniform Buffer
    float3 rotatedDir = (uniforms.rotation * float4(dir, 0.0)).xyz;
    
    constexpr sampler s(mag_filter::linear, min_filter::linear);
    float4 color = cubeMap.sample(s, rotatedDir);
    
    return color;
}

// 2D Blit Shader (for Composite Pass)
fragment float4 blitFragment2D(BlitVertexOut in [[stage_in]],
                               texture2d<float> tex [[texture(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear);
    return tex.sample(s, in.uv);
}

// ================================================================================================
// DEBUG OVERLAY SHADER (Display all 6 faces of CubeMap)
// ================================================================================================

// Debug View Shader (renders 6 faces to screen)
struct DebugVertexOut {
    float4 position [[position]];
    float3 uv; // 3D texture coordinate
    float2 localUV; // 0..1 for border drawing
};

vertex DebugVertexOut debugVertex(uint vertexID [[vertex_id]],
                                  uint instanceID [[instance_id]]) {
    DebugVertexOut out;
    
    // 6 quads, laid out in 2 rows of 3
    // Size and Gap
    float size = 0.25; // Slightly smaller to fit
    float gap = 0.05;
    float startX = -0.8;
    float startY = -0.5;
    
    // Layout:
    // Row 0 (Top): +X, +Y, +Z (Layers 0, 2, 4)
    // Row 1 (Bottom): -X, -Y, -Z (Layers 1, 3, 5)
    
    int col = 0;
    int row = 0;
    
    // Mapping instanceID (0-5) to Layout
    switch(instanceID) {
        case 0: col = 0; row = 1; break; // +X
        case 1: col = 0; row = 0; break; // -X
        case 2: col = 1; row = 1; break; // +Y
        case 3: col = 1; row = 0; break; // -Y
        case 4: col = 2; row = 1; break; // +Z
        case 5: col = 2; row = 0; break; // -Z
    }
    
    float2 offset = float2(startX + col * (size + gap), startY + row * (size + gap));
    
    // Quad Vertices (0..1)
    float2 positions[6] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1)
    };
    
    float2 localPos = positions[vertexID];
    out.localUV = localPos;
    
    // Screen Position
    out.position = float4(offset.x + localPos.x * size, offset.y + localPos.y * size, 0.0, 1.0);
    
    // Cube Map Sampling Direction
    // Map 0..1 localPos to -1..1
    float2 uv = localPos * 2.0 - 1.0;
    uv.y = -uv.y; // Flip Y for texture sampling
    
    float3 dir = float3(0, 0, 0);
    switch(instanceID) {
        case 0: dir = float3(1,  uv.y, -uv.x); break; // +X
        case 1: dir = float3(-1, uv.y,  uv.x); break; // -X
        case 2: dir = float3(uv.x, 1, -uv.y); break; // +Y
        case 3: dir = float3(uv.x, -1, uv.y); break; // -Y
        case 4: dir = float3(uv.x, uv.y, 1); break; // +Z
        case 5: dir = float3(-uv.x, uv.y, -1); break; // -Z
    }
    out.uv = normalize(dir);
    
    return out;
}

fragment float4 debugFragment(DebugVertexOut in [[stage_in]],
                              texturecube<float> cubeMap [[texture(0)]]) {
    
    // Draw Border
    float border = 0.05;
    if (in.localUV.x < border || in.localUV.x > 1.0 - border ||
        in.localUV.y < border || in.localUV.y > 1.0 - border) {
        return float4(1.0, 1.0, 0.0, 1.0); // Yellow Border
    }

    constexpr sampler s(mag_filter::linear, min_filter::linear);
    return cubeMap.sample(s, in.uv);
}

// Skybox Vertex Shader
struct SkyboxVertexOut {
    float4 position [[position]];
    float3 uv;
};

vertex SkyboxVertexOut vertexSkybox(VertexIn in [[stage_in]],
                                    constant Uniforms& uniforms [[buffer(1)]],
                                    constant SceneData& scene [[buffer(2)]]) {
    SkyboxVertexOut out;
    
    // Use raw position as UV (Cubemap direction)
    out.uv = in.position;
    
    // Remove translation from View Matrix
    // ViewProjections[0] is usually Main View
    float4x4 viewProj = uniforms.viewProjections[0];
    
    // We want the skybox to follow the camera but stay at infinity.
    // Standard trick: Model Matrix translates to Camera Pos? 
    // Or just use View Matrix rotation part.
    // Here we use the scene.model (which should be identity or scaling for skybox)
    // But better: Just use local position and assume Skybox Mesh is centered at 0,0,0
    // and we translate it to Camera Position in CPU or Shader.
    
    float4 worldPos = scene.model * float4(in.position, 1.0);
    out.position = viewProj * worldPos;
    
    // Force Z to far plane (1.0 in Metal with 0-1 depth?)
    // Metal NDC Z is 0 to 1. Far plane is 1.
    out.position.z = out.position.w; 
    
    return out;
}

fragment float4 fragmentSkybox(SkyboxVertexOut in [[stage_in]],
                               texturecube<float> skybox [[texture(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear);
    return skybox.sample(s, in.uv);
}
