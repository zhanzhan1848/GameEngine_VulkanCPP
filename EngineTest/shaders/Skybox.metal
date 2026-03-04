#include <metal_stdlib>
using namespace metal;

struct SkyboxVertexOut {
    float4 position [[position]];
    float3 uv;
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
    float4x4 previousViewProjection;
    float4x4 invViewProjection;
};

constant float3 cubeVertices[] = {
    // Back face
    float3(-1.0f, -1.0f, -1.0f), float3(-1.0f,  1.0f, -1.0f), float3( 1.0f,  1.0f, -1.0f),
    float3( 1.0f,  1.0f, -1.0f), float3( 1.0f, -1.0f, -1.0f), float3(-1.0f, -1.0f, -1.0f),
    // Front face
    float3(-1.0f, -1.0f,  1.0f), float3( 1.0f, -1.0f,  1.0f), float3( 1.0f,  1.0f,  1.0f),
    float3( 1.0f,  1.0f,  1.0f), float3(-1.0f,  1.0f,  1.0f), float3(-1.0f, -1.0f,  1.0f),
    // Left face
    float3(-1.0f,  1.0f,  1.0f), float3(-1.0f,  1.0f, -1.0f), float3(-1.0f, -1.0f, -1.0f),
    float3(-1.0f, -1.0f, -1.0f), float3(-1.0f, -1.0f,  1.0f), float3(-1.0f,  1.0f,  1.0f),
    // Right face
    float3( 1.0f,  1.0f,  1.0f), float3( 1.0f, -1.0f,  1.0f), float3( 1.0f, -1.0f, -1.0f),
    float3( 1.0f, -1.0f, -1.0f), float3( 1.0f,  1.0f, -1.0f), float3( 1.0f,  1.0f,  1.0f),
    // Bottom face
    float3(-1.0f, -1.0f, -1.0f), float3( 1.0f, -1.0f, -1.0f), float3( 1.0f, -1.0f,  1.0f),
    float3( 1.0f, -1.0f,  1.0f), float3(-1.0f, -1.0f,  1.0f), float3(-1.0f, -1.0f, -1.0f),
    // Top face
    float3(-1.0f,  1.0f, -1.0f), float3(-1.0f,  1.0f,  1.0f), float3( 1.0f,  1.0f,  1.0f),
    float3( 1.0f,  1.0f,  1.0f), float3( 1.0f,  1.0f, -1.0f), float3(-1.0f,  1.0f, -1.0f)
};

vertex SkyboxVertexOut vertexSkybox(uint vertexID [[vertex_id]],
                                    constant ViewData& viewData [[buffer(0)]],
                                    constant SceneData& sceneData [[buffer(1)]]) {
    SkyboxVertexOut out;
    
    float3 pos = cubeVertices[vertexID];
    out.uv = pos; // Use object space position as 3D UV
    
    // Remove translation from View Matrix
    // viewData.viewProjection includes translation.
    // We want to apply Rotation * Pos, then Project.
    // Let's try translation approach.
    float3 camPos = sceneData.viewPos.xyz;
    float4 worldPos = float4(pos + camPos, 1.0);
    out.position = viewData.viewProjection * worldPos;
    
    // Force Z to far plane (1.0 - epsilon)
    // In Metal, NDC Z is 0..1. Far is 1.
    out.position.z = out.position.w; // Reset to standard Far Plane
    
    return out;
}

fragment float4 fragmentSkybox(SkyboxVertexOut in [[stage_in]],
                               texturecube<float> skybox [[texture(2)]],
                               sampler s [[sampler(3)]]) {
    return skybox.sample(s, in.uv);
}
