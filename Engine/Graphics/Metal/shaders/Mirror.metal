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

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv [[attribute(2)]];
    float3 tangent [[attribute(3)]];
};

struct VertexOut {
    float4 position [[position]];
    float4 screenPosition;
    float3 worldPosition;
    float3 normal;
};

vertex VertexOut vertexMain(VertexIn in [[stage_in]],
                            constant GlobalShaderData& globalData [[buffer(0)]],
                            constant PerObjectData& perObjectData [[buffer(1)]]) {
    VertexOut out;
    float4 worldPos = perObjectData.world * float4(in.position, 1.0);
    out.position = globalData.viewProjection * worldPos;
    out.screenPosition = out.position;
    out.worldPosition = worldPos.xyz;
    out.normal = (perObjectData.world * float4(in.normal, 0.0)).xyz;
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             texture2d<float> reflectionTexture [[texture(2)]],
                             sampler reflectionSampler [[sampler(2)]]) {
    
    float2 ndc = in.screenPosition.xy / in.screenPosition.w;
    float2 uv = ndc * 0.5 + 0.5;
    uv.y = 1.0 - uv.y; // Flip Y for Metal texture sampling if needed (RenderTexture usually matches screen)
    
    // Note: If the Reflection Texture was rendered with standard Viewport, 
    // it matches the screen orientation.
    // However, the Reflection Matrix might have flipped something.
    // The Planar Reflection Matrix flips the camera to the other side.
    // The rendered image is from that "underworld" camera.
    // Projecting it onto the mirror surface (screen space) should align.
    
    constexpr sampler s(min_filter::linear, mag_filter::linear);
    float4 reflectionColor = reflectionTexture.sample(reflectionSampler, uv);
    
    // Mix with a base color if desired, or just return reflection
    return float4(reflectionColor.rgb, 1.0);
}
