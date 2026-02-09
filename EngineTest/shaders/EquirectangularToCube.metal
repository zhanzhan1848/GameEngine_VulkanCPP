#include <metal_stdlib>
using namespace metal;

constant float2 invAtan = float2(0.1591, 0.3183);

float2 SampleSphericalMap(float3 v)
{
    float2 uv = float2(atan2(v.z, v.x), asin(v.y));
    uv *= invAtan;
    uv += 0.5;
    return uv;
}

kernel void CS_EquirectangularToCube(
    texture2d<float, access::sample> equirectangularMap [[texture(0)]],
    texturecube<float, access::write> environmentMap [[texture(1)]],
    sampler defaultSampler [[sampler(0)]],
    uint3 dispatchThreadID [[thread_position_in_grid]]
)
{
    uint width = environmentMap.get_width();
    uint height = environmentMap.get_height();
    
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;
    
    // Convert UV to direction based on face index (dispatchThreadID.z)
    // Face order: +X, -X, +Y, -Y, +Z, -Z
    
    float3 dir;
    float2 localUV = float2(dispatchThreadID.xy) / float2(width, height);
    localUV = localUV * 2.0 - 1.0;
    // localUV.y = -localUV.y; // Flip Y if needed, Metal texture coordinates top-left origin? 
    // Usually cubemap faces are defined with Y up or down depending on API. 
    // Let's assume standard right-handed Y-up.
    
    switch (dispatchThreadID.z) {
        case 0: dir = normalize(float3(1.0,  -localUV.y, -localUV.x)); break; // +X
        case 1: dir = normalize(float3(-1.0, -localUV.y,  localUV.x)); break; // -X
        case 2: dir = normalize(float3(localUV.x, 1.0,  localUV.y)); break; // +Y
        case 3: dir = normalize(float3(localUV.x, -1.0, -localUV.y)); break; // -Y
        case 4: dir = normalize(float3(localUV.x, -localUV.y, 1.0)); break; // +Z
        case 5: dir = normalize(float3(-localUV.x, -localUV.y, -1.0)); break; // -Z
    }
    
    float2 sphereUV = SampleSphericalMap(dir);
    
    // Sample with Lod 0
    float4 color = equirectangularMap.sample(defaultSampler, sphereUV);
    
    environmentMap.write(color, dispatchThreadID.xy, dispatchThreadID.z);
}
