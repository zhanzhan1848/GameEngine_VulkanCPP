#include <metal_stdlib>
using namespace metal;

// Helper to get direction from cubemap face UV
// face: 0-5 (+X, -X, +Y, -Y, +Z, -Z)
// uv: [0, 1]
float3 GetDirectionFromCubemapFace(uint face, float2 uv) {
    float3 dir;
    // UV is in [0, 1], map to [-1, 1]
    float u = 2.0 * uv.x - 1.0;
    float v = 2.0 * uv.y - 1.0;
    
    // Adjust for Metal coordinate system and standard cubemap layout
    // Metal Cubemap: +X, -X, +Y, -Y, +Z, -Z
    // Standard:
    // +X (Right):  ( 1, -v, -u)
    // -X (Left):   (-1, -v,  u)
    // +Y (Top):    ( u,  1,  v)
    // -Y (Bottom): ( u, -1, -v)
    // +Z (Front):  ( u, -v,  1)
    // -Z (Back):   (-u, -v, -1)
    
    // Re-verify orientation for Equirectangular mapping
    // Usually we want Y-up world.
    v = -v; // Flip Y for texture coords vs world coords if needed
    
    switch(face) {
        case 0: dir = float3(1.0,  v, -u); break; // +X
        case 1: dir = float3(-1.0, v,  u); break; // -X
        case 2: dir = float3(u, 1.0, -v); break; // +Y
        case 3: dir = float3(u, -1.0, v); break; // -Y
        case 4: dir = float3(u, v, 1.0); break; // +Z
        case 5: dir = float3(-u, v, -1.0); break; // -Z
    }
    
    return normalize(dir);
}

kernel void CS_EquirectangularToCube(
    uint3 gid [[thread_position_in_grid]],
    texture2d<float, access::sample> equirectangularMap [[texture(0)]],
    texturecube<float, access::write> cubemap [[texture(1)]],
    sampler smp [[sampler(2)]]
) {
    uint face = gid.z;
    uint width = cubemap.get_width();
    uint height = cubemap.get_height();
    
    if (gid.x >= width || gid.y >= height || face >= 6) return;
    
    float2 uv = (float2(gid.xy) + 0.5) / float2(width, height);
    float3 dir = GetDirectionFromCubemapFace(face, uv);
    
    // Sample Equirectangular Map
    // atan2(z, x) gives angle in X-Z plane.
    // asin(y) gives angle from X-Z plane.
    float2 sphereUV = float2(atan2(dir.z, dir.x), asin(dir.y));
    const float2 invAtan = float2(0.1591, 0.3183); // 1/(2pi), 1/pi
    sphereUV *= invAtan;
    sphereUV += 0.5;
    
    // Sample
    float4 color = equirectangularMap.sample(smp, sphereUV);
    
    // Write
    cubemap.write(color, gid.xy, face);
}
