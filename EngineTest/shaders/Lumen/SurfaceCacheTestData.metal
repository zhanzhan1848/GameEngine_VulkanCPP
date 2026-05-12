#include <metal_stdlib>
using namespace metal;

// Fill atlas with test pattern for verifying direct lighting pipeline
constant uint kAtlasSize = 2048;

kernel void surfaceCacheFillTest(
    uint2 gid [[thread_position_in_grid]],
    texture2d<float, access::write> albedo_out  [[texture(0)]],
    texture2d<float, access::write> normal_out  [[texture(1)]],
    texture2d<float, access::write> depth_out   [[texture(2)]])
{
    if (gid.x >= kAtlasSize || gid.y >= kAtlasSize) return;

    // Checkerboard in 32x32 tile blocks
    uint tx = gid.x / 32;
    uint ty = gid.y / 32;
    float gray = ((tx + ty) % 2 == 0) ? 0.8 : 0.4;

    albedo_out.write(float4(gray, gray, gray, 1.0), gid);

    // Normal: Y-up oct-encoded = (0.5, 0.5)
    normal_out.write(float4(0.5, 0.5, 0.0, 1.0), gid);

    // Depth: positive value
    depth_out.write(float4(0.5, 0.0, 0.0, 1.0), gid);
}
