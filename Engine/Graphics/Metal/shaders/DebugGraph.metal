#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

struct Uniforms {
    float2 scale;
    float2 offset;
    float2 screenSize;
};

vertex VertexOut debug_vs(VertexIn in [[stage_in]], constant Uniforms& u [[buffer(1)]], uint vid [[vertex_id]]) {
    VertexOut out;
    
    // Apply pan (offset) and zoom (scale)
    // Map from screen space [0, screenSize] to NDC [-1, 1]
    float2 worldPos = in.position * u.scale + u.offset;
    
    // Normalize to [-1, 1]
    // 0 -> -1, screenSize -> 1
    float2 ndcPos = (worldPos / u.screenSize) * 2.0 - 1.0;
    // Flip Y because Metal NDC Y is up, but screen Y is usually down (or vice versa depending on setup)
    // Assuming standard top-left 0,0 screen coordinates
    ndcPos.y = -ndcPos.y;

    out.position = float4(ndcPos, 0.0, 1.0);
    out.color = in.color;
    return out;
}

fragment float4 debug_fs(VertexOut in [[stage_in]]) {
    return in.color;
}

struct TextureVertexIn {
    float2 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
    float type [[attribute(2)]];
};

struct TextureVertexOut {
    float4 position [[position]];
    float2 uv;
    float type;
};

vertex TextureVertexOut debug_texture_vs(
    TextureVertexIn in [[stage_in]],
    constant Uniforms& u [[buffer(1)]]
) {
    TextureVertexOut out;
    // Same transform logic
    float2 worldPos = in.position * u.scale + u.offset;
    float2 ndcPos = (worldPos / u.screenSize) * 2.0 - 1.0;
    ndcPos.y = -ndcPos.y;

    out.position = float4(ndcPos, 0.0, 1.0);
    out.uv = in.uv;
    out.type = in.type;
    return out;
}

fragment float4 debug_texture_fs(
    TextureVertexOut in [[stage_in]],
    texture2d<float> tex [[texture(0)]],
    sampler smp [[sampler(0)]]
) {
    float4 color = tex.sample(smp, in.uv);
    
    // Type: 0 = Color, 1 = Depth, 2 = Moments, 3 = Normal, 4 = WorldPos, 5 = UV
    if (in.type > 0.5 && in.type < 1.5) {
        // Depth: visualize R channel
        return float4(color.r, color.r, color.r, 1.0);
    } else if (in.type > 1.5 && in.type < 2.5) {
        // Moments: RG32
        return float4(color.r, color.g, 0.0, 1.0);
    } else if (in.type > 2.5 && in.type < 3.5) {
        // Normal: RGB is 0..1 (mapped from -1..1)
        return float4(color.rgb, 1.0);
    } else if (in.type > 3.5 && in.type < 4.5) {
        // WorldPos: RGB. Scale it down to visualize? Or just fract?
        // Let's use fract to see changes
        return float4(fract(color.rgb * 0.1), 1.0);
    } else if (in.type > 4.5 && in.type < 5.5) {
        // UV: RG
        return float4(color.rg, 0.0, 1.0);
    }
    
    return color;
}
