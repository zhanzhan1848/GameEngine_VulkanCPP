#include <metal_stdlib>
using namespace metal;

struct SSRParams {
    float4x4 viewMatrix;
    float4x4 projMatrix;
    float4x4 invProjMatrix;
    float4 resolution; // xy: size, zw: invSize
    float maxDistance;
    float stride;
    float thickness;
    float jitter;
};

// Helper: Convert Screen UV + Depth to View Space Position
float3 ScreenToView(float2 uv, float depth, float4x4 invProj) {
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y; // Metal NDC Y is flipped relative to texture coordinates (0,0 top-left) if we consider standard NDC
    // Actually, in Metal:
    // Clip Space: (-1, -1) to (1, 1). Y is up.
    // Texture Space: (0, 0) is top-left.
    // So UV (0, 0) -> Clip (-1, 1)
    // UV (1, 1) -> Clip (1, -1)
    // X: uv.x * 2 - 1
    // Y: (1 - uv.y) * 2 - 1 = 1 - 2*uv.y
    // Wait, let's check standard Metal convention.
    // Metal NDC: Z is 0..1. Y is Up.
    // Texture UV: Y is Down.
    // So to map UV to NDC Y:
    // uv.y = 0 -> ndc.y = 1
    // uv.y = 1 -> ndc.y = -1
    // ndc.y = 1 - 2 * uv.y
    
    // clipPos.x = uv.x * 2 - 1
    // clipPos.y = 1.0 - 2.0 * uv.y;
    
    // However, if we just multiply by invProj, we need to match how Proj was built.
    // Usually projection maps View Y (Up) to Clip Y (Up).
    // So if we have UV (Y Down), we flip it to get NDC (Y Up).
    
    clipPos.y = 1.0 - 2.0 * uv.y;
    
    float4 viewPos = invProj * clipPos;
    return viewPos.xyz / viewPos.w;
}

// Helper: Convert View Space to Screen UV + Depth
float3 ViewToScreen(float3 viewPos, float4x4 proj) {
    float4 clipPos = proj * float4(viewPos, 1.0);
    float3 ndc = clipPos.xyz / clipPos.w;
    
    // NDC to UV
    // X: (-1, 1) -> (0, 1) => x * 0.5 + 0.5
    // Y: (1, -1) -> (0, 1) => (1 - y) * 0.5 = 0.5 - 0.5*y
    
    float2 uv;
    uv.x = ndc.x * 0.5 + 0.5;
    uv.y = 0.5 - 0.5 * ndc.y;
    
    return float3(uv, ndc.z); // Depth is usually 0..1 in Metal
}

kernel void kernelMain(texture2d<float, access::sample> sceneColor [[texture(0)]],
                       texture2d<float, access::sample> sceneDepth [[texture(1)]],
                       texture2d<float, access::write> output [[texture(2)]],
                       constant SSRParams& params [[buffer(3)]],
                       uint2 gid [[thread_position_in_grid]]) {
    
    if (gid.x >= params.resolution.x || gid.y >= params.resolution.y) return;
    
    float2 uv = float2(gid) * params.resolution.zw;
    
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    
    // Check Reflectivity Mask (Alpha Channel)
    float4 colorSample = sceneColor.sample(s, uv);
    if (colorSample.a < 0.01) {
        output.write(float4(0,0,0,0), gid);
        return;
    }

    float depth = sceneDepth.sample(s, uv).r;
    
    // If background or too far, skip
    // Metal Reverse Z? No, usually 0..1 (Near..Far)
    // If we use Reverse Z, 1 is Near, 0 is Far.
    // Assuming standard 0..1
    if (depth >= 1.0) { 
        output.write(float4(0,0,0,0), gid);
        return;
    }
    
    float3 viewPos = ScreenToView(uv, depth, params.invProjMatrix);
    
    // Reconstruct Normal via ddx/ddy (simulated by neighbor sampling)
    float2 pixelSize = params.resolution.zw;
    
    // We need 3 points to form a plane
    // Center, Right, Down
    float dX = sceneDepth.sample(s, uv + float2(pixelSize.x, 0)).r;
    float dY = sceneDepth.sample(s, uv + float2(0, pixelSize.y)).r;
    
    float3 posX = ScreenToView(uv + float2(pixelSize.x, 0), dX, params.invProjMatrix);
    float3 posY = ScreenToView(uv + float2(0, pixelSize.y), dY, params.invProjMatrix);
    
    float3 v1 = posX - viewPos;
    float3 v2 = posY - viewPos;
    
    // Normal = cross(v1, v2). 
    // If v1 is Right vector, v2 is Down vector.
    // Right x Down -> Forward (View -Z)
    // Let's check coordinate system.
    // View Space: Right +X, Up +Y, Forward -Z.
    // v1 is roughly (+X, 0, 0).
    // v2 is roughly (0, -Y, 0).
    // X cross -Y = -Z. Correct.
    float3 normal = normalize(cross(v1, v2));
    
    // View Direction
    // Since View Space origin is (0,0,0) and we are looking down -Z.
    // Ray from Eye to Surface is viewPos - 0 = viewPos.
    float3 viewDir = normalize(viewPos);
    
    // Reflection Vector
    float3 reflectDir = reflect(viewDir, normal);
    
    // Basic Ray Marching
    float3 curPos = viewPos;
    float3 stepDir = reflectDir * params.stride;
    
    float4 hitColor = float4(0,0,0,0);
    
    int maxSteps = 64; 
    
    // Jitter start position to avoid banding
    // float jitter = fract(sin(dot(uv, float2(12.9898,78.233))) * 43758.5453);
    // curPos += stepDir * jitter;
    
    for (int i = 0; i < maxSteps; ++i) {
        curPos += stepDir;
        
        // Project to Screen
        float3 screenPos = ViewToScreen(curPos, params.projMatrix);
        
        // Check Bounds
        if (screenPos.x < 0 || screenPos.x > 1 || screenPos.y < 0 || screenPos.y > 1) break;
        if (screenPos.z < 0 || screenPos.z > 1) break;
        
        float sampleDepth = sceneDepth.sample(s, screenPos.xy).r;
        
        // Depth Test
        // Ray Depth (screenPos.z) vs Sample Depth (sampleDepth)
        // If Ray Depth > Sample Depth, Ray is behind surface.
        // We want the FIRST time it goes behind.
        
        float depthDiff = screenPos.z - sampleDepth;
        
        if (depthDiff > 0 && depthDiff < params.thickness) {
            // Hit found
            hitColor = sceneColor.sample(s, screenPos.xy);
            
            // Fade Factor
            // 1. Distance fade
            float distFade = 1.0 - (float(i) / maxSteps);
            
            // 2. Screen Edge fade
            float2 dCoords = smoothstep(0.2, 0.6, abs(float2(0.5, 0.5) - screenPos.xy));
            float edgeFade = saturate(1.0 - (dCoords.x + dCoords.y));
            
            // 3. Reflection Angle fade (Fresnel-like or based on viewDir dot reflectDir)
            // If reflection is pointing back at camera, it's weird? No.
            // If reflection vector is pointing away from camera?
            float angleFade = saturate(dot(reflectDir, -viewDir)); 
            
            hitColor.a = distFade * edgeFade * colorSample.a; // Multiply by reflectivity
            break;
        }
    }
    
    output.write(hitColor, gid);
}
