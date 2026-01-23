#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv       [[attribute(2)]];
    float3 color    [[attribute(3)]]; // Added Color
};

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 normal;
    float2 uv;
    float3 color; // Pass Color
    uint layer [[render_target_array_index]]; // Selects which face to render to
};

struct Uniforms {
    float4x4 viewProjections[6];
};

struct SceneData {
    float4x4 model;
    float4 lightPos;
    float4 lightColor;
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
};

// Reflection Pass Vertex Shader (with Clip Distance)
struct VertexOutReflection {
    float4 position [[position]];
    float3 worldPos;
    float3 normal;
    float2 uv;
    float3 color;
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
    out.worldPos = worldPos.xyz;
    
    // Normal Matrix
    out.normal = (scene.model * float4(in.normal, 0.0)).xyz;
    
    out.uv = in.uv;
    out.color = in.color;
    
    // Use View 0 (Reflection View stored in slot 0 of this special uniform buffer or just reuse existing Uniforms if we update it)
    // We will assume 'uniforms' passed here contains the Reflection View Matrix in viewProjections[0]
    out.position = uniforms.viewProjections[0] * worldPos;
    
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
                                   constant SceneData& scene [[buffer(2)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    
    // Calculate Screen Space UV
    // Clip Space: (-1 to 1) -> UV: (0 to 1)
    float2 uv = (in.clipPos.xy / in.clipPos.w) * 0.5 + 0.5;
    uv.y = 1.0 - uv.y; // Flip Y for Metal texture sampling
    
    float4 reflectionColor = reflectionTex.sample(s, uv);
    
    // Simple mixing or Fresnel could be added here
    // For now, pure reflection mixed with base color
    float3 N = normalize(in.normal);
    float3 L = normalize(scene.lightPos.xyz - in.worldPos);
    float diff = max(dot(N, L), 0.0);
    
    // Tint with a bit of blue/glassy look
    float4 baseColor = float4(0.1, 0.1, 0.2, 1.0);
    
    return mix(baseColor * diff, reflectionColor, 0.8);
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
    
    // Use View 0 (Main View)
    out.position = uniforms.viewProjections[0] * worldPos;
    
    return out;
}

fragment float4 fragmentMainSingle(VertexOutSingle in [[stage_in]],
                                   constant SceneData& scene [[buffer(2)]]) {
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
    
    return float4(ambient + diffuse + specular, 1.0);
}


// Standard Multi-View Shader
vertex VertexOut vertexMain(VertexIn in [[stage_in]],
                            constant Uniforms& uniforms [[buffer(1)]],
                            constant SceneData& scene [[buffer(2)]],
                            uint instanceID [[instance_id]]) {
    VertexOut out;
    
    // Calculate World Position
    float4 worldPos = scene.model * float4(in.position, 1.0);
    out.worldPos = worldPos.xyz;
    out.normal = (scene.model * float4(in.normal, 0.0)).xyz;
    out.uv = in.uv;
    out.color = in.color; // Pass Color
    
    // Select ViewProjection based on Instance ID (0-5)
    // instanceID corresponds to the face index
    out.position = uniforms.viewProjections[instanceID] * worldPos;
    out.layer = instanceID; // Route to correct array layer
    
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             constant SceneData& scene [[buffer(2)]]) {
    // Cornell Box Lighting
    float3 N = normalize(in.normal);
    
    float3 lightPos = scene.lightPos.xyz;
    float3 lightColor = scene.lightColor.xyz;
    
    float3 L = normalize(lightPos - in.worldPos);
    float distToLight = length(lightPos - in.worldPos);
    float atten = 1.0 / (1.0 + 0.1 * distToLight + 0.01 * distToLight * distToLight);
    
    float diff = max(dot(N, L), 0.0);
    
    // Specular
    float3 V = normalize(-in.worldPos); // View vector (approx relative to origin camera, but this is multi-view, so View is actually uniforms.viewProjections[in.layer] origin... wait)
    // The camera position for each view is at 0,0,0 (MultiViewTestCase).
    // So V = normalize(0 - worldPos) is correct.
    
    float3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 32.0);
    
    // Combine
    float3 ambient = float3(0.4, 0.4, 0.4) * in.color; // Increased Ambient
    float3 diffuse = diff * in.color * lightColor * atten * 1.5; 
    float3 specular = spec * lightColor * atten * 0.8;
    
    float3 finalColor = ambient + diffuse + specular;
    
    // Tone mapping (Simple Reinhard)
    finalColor = finalColor / (finalColor + float3(1.0));
    
    return float4(finalColor, 1.0);
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
    out.position = float4(positions[vertexID], 0.0, 1.0);
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
