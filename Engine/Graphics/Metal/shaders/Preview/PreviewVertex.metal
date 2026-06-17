#include <metal_stdlib>
using namespace metal;

// --- Preview Scene Data ---
struct ViewData {
    float4x4 viewProj;
    float4x4 invViewProj;
    float4x4 prevViewProj;
};

struct SceneData {
    float4x4 model;
    float4   lightDir;
    float4   lightColor;
    float    lightIntensity;
    float    time;
    float    _pad[2];
    float3   cameraPos;
    float    _pad2;
    // Background colors for gradient mode (read by gradientFragment)
    float4   bgTopColor;   // rgb + unused
    float4   bgBotColor;   // rgb + unused
    float    bgMode;       // 0 = gradient, 1 = solid color
    float    _pad3[3];
};

// --- Vertex format matching ProceduralMesh output (32B tight stride) ---
// ProceduralMesh packs: position(12B) + normal(12B) + uv(8B) = 32B
// Metal's float3 has 16B alignment, so we use packed_float3/2 to match the tight layout.
struct PreviewVertex {
    packed_float3 position;  // 12B
    packed_float3 normal;    // 12B
    packed_float2 uv;        // 8B
};                           // = 32B stride

// --- Output to fragment shader (matches MetalEmitter VertexOut) ---
struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
    float2 uv;
    float4 shadowPos0;
    float4 shadowPos1;
};

// --- Preview vertex shader (vertex pulling at buffer 20) ---
vertex VertexOut previewVertexMain(
    uint vid [[vertex_id]],
    constant ViewData& viewData [[buffer(0)]],
    constant SceneData& sceneData [[buffer(1)]],
    constant PreviewVertex* vertices [[buffer(20)]])
{
    PreviewVertex v = vertices[vid];

    float4 worldPos = sceneData.model * float4(float3(v.position), 1.0f);
    float3 worldNormal = (sceneData.model * float4(float3(v.normal), 0.0f)).xyz;

    VertexOut out;
    out.position    = viewData.viewProj * worldPos;
    out.worldPos    = worldPos.xyz;
    out.worldNormal = normalize(worldNormal);
    out.uv          = float2(v.uv);
    out.shadowPos0  = float4(0.0f, 0.0f, 0.0f, 1.0f);
    out.shadowPos1  = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return out;
}

// --- Full-screen triangle vertex shader (no vertex buffer) ---
vertex float4 fullscreenTriangleVS(uint vid [[vertex_id]]) {
    float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2( 3.0f, -1.0f),
        float2(-1.0f,  3.0f)
    };
    return float4(positions[vid], 0.0f, 1.0f);
}

// --- Gradient background fragment shader ---
// Reads top/bottom colors from a small constant buffer at [[buffer(2)]] (set via PushConstants).
struct BgColors {
    float3 topColor;
    float  _pad0;
    float3 botColor;
    float  _pad1;
};

fragment float4 gradientFragment(float4 position [[position]],
                                 constant BgColors& bg [[buffer(2)]]) {
    float s = saturate(position.y / 512.0f); // approximate for 512 height
    return float4(mix(bg.botColor, bg.topColor, s), 1.0f);
}

// --- Blit shaders ---
// Samples srcTex (typically the preview's color_target_) into the current render pass's
// color attachment (typically the swap chain backbuffer). Used in place of MTLBlitCommandEncoder
// because swap chain drawables are framebufferOnly and cannot be blit destinations.
struct BlitVertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex BlitVertexOut blitVertex(uint vid [[vertex_id]]) {
    // Fullscreen triangle: (-1,-1), (-1,3), (3,-1) in clip space.
    float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2(-1.0f,  3.0f),
        float2( 3.0f, -1.0f)
    };
    // UV mapping: framebuffer (0,0)=top-left ↔ texture (0,0)=top-left (Metal convention).
    // UVs beyond [0,1] are clamped by sampler address::clamp_to_edge, so the off-screen
    // vertex UVs collapse to the same edge as the corresponding on-screen vertex.
    float2 uvs[3] = {
        float2(0.0f,  1.0f),  // bottom-left
        float2(0.0f, -1.0f),  // top-left (clamps to 0)
        float2(2.0f,  1.0f)   // bottom-right (clamps to 1)
    };
    BlitVertexOut out;
    out.position = float4(positions[vid], 0.0f, 1.0f);
    out.uv = uvs[vid];
    return out;
}

fragment float4 blitFragment(
    BlitVertexOut in [[stage_in]],
    texture2d<float> srcTex [[texture(0)]])
{
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
    return srcTex.sample(s, in.uv);
}
