#include "../../Engine/Graphics/RHI/Shaders/RHIShaderCommon.metal"

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

struct TAAUniforms {
    float2 resolution;
    float2 jitter;
    float2 previousJitter;
    float feedback;
    float padding;
};

// Fullscreen Triangle Vertex Shader
vertex VertexOut vertexMain(uint vertexID [[vertex_id]]) {
    VertexOut out;
    GetFullScreenTrianglePosUV(vertexID, out.position, out.uv);
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]],
                             texture2d<float> colorTexture [[texture(0)]],
                             texture2d<float> historyTexture [[texture(1)]],
                             texture2d<float> velocityTexture [[texture(2)]],
                             constant TAAUniforms& uniforms [[buffer(3)]])
{
    constexpr sampler s(min_filter::linear, mag_filter::linear, address::clamp_to_edge);
    
    // 1. Sample current frame color
    // We use non-jittered UV for current frame if we want to resolve jitter immediately,
    // but typically we sample at the jittered position. 
    // However, since we are doing post-process, the 'in.uv' is screen space [0,1].
    // The rendered image 'colorTexture' already contains the jittered result.
    float3 color = colorTexture.sample(s, in.uv).rgb;
    
    // 2. Sample Velocity
    // Velocity texture stores (motionX, motionY) in screen space UV
    float2 velocity = velocityTexture.sample(s, in.uv).rg;
    
    // 3. Reproject to find history UV
    float2 historyUV = in.uv - velocity;
    
    // 4. Sample History Color
    // Check if history is valid (inside screen bounds)
    bool isOffScreen = (historyUV.x < 0.0 || historyUV.x > 1.0 || 
                        historyUV.y < 0.0 || historyUV.y > 1.0);
                        
    float3 history = historyTexture.sample(s, historyUV).rgb;
    
    // 5. Neighborhood Clamping / Clipping (Anti-Ghosting)
    // Sample 3x3 neighborhood to find min/max color box
    float3 minColor = color;
    float3 maxColor = color;
    
    // Simple 3x3 cross or box sampling
    // For performance, we can do a 5-tap cross
    float2 texelSize = 1.0 / uniforms.resolution;
    
    float3 c0 = colorTexture.sample(s, in.uv + float2(-1, 0) * texelSize).rgb;
    float3 c1 = colorTexture.sample(s, in.uv + float2( 1, 0) * texelSize).rgb;
    float3 c2 = colorTexture.sample(s, in.uv + float2( 0,-1) * texelSize).rgb;
    float3 c3 = colorTexture.sample(s, in.uv + float2( 0, 1) * texelSize).rgb;
    
    minColor = min(minColor, min(c0, min(c1, min(c2, c3))));
    maxColor = max(maxColor, max(c0, max(c1, max(c2, c3))));
    
    // Clamp history to the neighborhood of the current frame
    history = clamp(history, minColor, maxColor);
    
    // 6. Blend
    // If offscreen, trust current frame 100%
    // Otherwise, blend history and current
    float feedback = uniforms.feedback; // e.g. 0.95
    if (isOffScreen) feedback = 0.0;
    
    // Reduce feedback if velocity is high (optional, helps with trailing)
    // float speed = length(velocity * uniforms.resolution);
    // feedback = mix(feedback, 0.8, saturate(speed / 10.0));
    
    float3 resolved = mix(color, history, feedback);
    
    return float4(resolved, 1.0);
}
