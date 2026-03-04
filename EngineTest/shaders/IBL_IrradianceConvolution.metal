#include <metal_stdlib>
using namespace metal;
#include "RHIShaderConstants.metal"

/**
 * @file IBL_IrradianceConvolution.metal
 * @brief 生成 Irradiance Map (Diffuse IBL) 的计算着色器
 */

// Helper to get direction from cubemap face UV
// face: 0-5 (+X, -X, +Y, -Y, +Z, -Z)
// uv: [0, 1]
float3 GetDirectionFromCubemapFace(uint face, float2 uv) {
    float3 dir;
    // UV is in [0, 1], map to [-1, 1]
    float u = 2.0 * uv.x - 1.0;
    float v = 2.0 * uv.y - 1.0;
    
    // Adjust for Metal coordinate system and standard cubemap layout
    switch(face) {
        case 0: dir = float3(1.0,  v, -u); break; // +X (Right)
        case 1: dir = float3(-1.0, v,  u); break; // -X (Left)
        case 2: dir = float3(u, 1.0,  -v); break; // +Y (Top)
        case 3: dir = float3(u, -1.0, v); break; // -Y (Bottom)
        case 4: dir = float3(u, v, 1.0); break; // +Z (Front)
        case 5: dir = float3(-u, v, -1.0); break; // -Z (Back)
    }
    // Note: The coordinate mapping might need adjustment based on how the engine defines +Y/+Z and texture origin
    // Usually Metal textures have (0,0) at top-left.
    // If output is inverted, flip v.
    
    return normalize(dir);
}

struct IrradianceParams {
    uint faceIndex;
    float padding[3];
};

kernel void CS_IrradianceConvolution(
    uint2 gid [[thread_position_in_grid]],
    texturecube<float, access::sample> envMap [[texture(0)]],
    texture2d<float, access::write> output [[texture(1)]],
    sampler smp [[sampler(2)]],
    constant IrradianceParams& params [[buffer(3)]]
) {
    if (gid.x >= output.get_width() || gid.y >= output.get_height()) return;

    // Center of the texel
    float2 uv = (float2(gid) + 0.5) / float2(output.get_width(), output.get_height());
    
    // ...
    
    float3 N;
    float u = 2.0 * uv.x - 1.0;
    float v = 2.0 * uv.y - 1.0; 
    v = -v; // Flip Y
    
    switch(params.faceIndex) {
        case 0: N = float3(1.0,  v, -u); break;
        case 1: N = float3(-1.0, v,  u); break;
        case 2: N = float3(u, 1.0, -v); break;
        case 3: N = float3(u, -1.0, v); break;
        case 4: N = float3(u, v, 1.0); break;
        case 5: N = float3(-u, v, -1.0); break;
    }
    N = normalize(N);

    float3 irradiance = float3(0.0);
    
    // Tangent Space Basis
    float3 up = float3(0.0, 1.0, 0.0);
    float3 right = cross(up, N);
    if (length(right) < 0.001) {
        up = float3(1.0, 0.0, 0.0);
        right = cross(up, N);
    }
    right = normalize(right);
    up = normalize(cross(N, right));

    float sampleDelta = 0.025;
    float nrSamples = 0.0;

    for(float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta) {
        for(float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
            // Spherical to Cartesian (in tangent space)
            float3 tangentSample = float3(sin(theta) * cos(phi),  sin(theta) * sin(phi), cos(theta));
            // Tangent to World
            float3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;

            irradiance += envMap.sample(smp, sampleVec).rgb * cos(theta) * sin(theta);
            nrSamples++;
        }
    }
    
    irradiance = PI * irradiance * (1.0 / float(nrSamples));

    output.write(float4(irradiance, 1.0), gid);
}
