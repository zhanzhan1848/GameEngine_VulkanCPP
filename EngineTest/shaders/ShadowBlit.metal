//
// ShadowBlit.metal
// Task 2: D32_Float → R32_Float depth copy
//
// Copies depth values from D32_Float render target to R32_Float sampleable texture.
// Required because Metal TBDR GPUs cannot sample D32_Float depth textures directly
// in compute/pixel shaders.
//
// Used by shadow mapping pipeline: shadow maps rendered to D32_Float via graphics pass,
// then copied to R32_Float for sampling in downstream passes (GTAO, SSDO, DeferredLighting).
//
// Binding layout (RHI binding → Metal index):
//   texture(0): depth_source (D32_Float, read) → binding 0 SampledImage
//   texture(1): depth_target (R32_Float, write) → binding 1 StorageImage
//   buffer(2):  resolution (uint2) → binding 2 UniformBuffer
//   NOTE: buffer(2) not buffer(0) because RHI binding number maps directly to Metal slot index.
//

#include <metal_stdlib>
using namespace metal;

[[kernel]]
void shadow_depth_blit(
    // Texture 0: Source depth texture (D32_Float — use depth2d without access::read,
    // matching HZBGeneration.metal's copy_depth_to_hzb_mip0 pattern that works on Metal TBDR)
    depth2d<float> depth_source [[texture(0)]],

    // Texture 1: Target depth texture (R32_Float)
    texture2d<float, access::write> depth_target [[texture(1)]],

    // Buffer 2: Resolution (width, height) — binding 2 in RHI layout
    constant uint2& resolution [[buffer(2)]],

    // Thread position in grid
    uint2 tid [[thread_position_in_grid]])
{
    // Out-of-bounds check: discard threads beyond texture resolution
    if (any(tid >= resolution)) {
        return;
    }

    // Read depth value from source (D32_Float via depth2d)
    float depth = depth_source.read(tid);

    // Write depth value to target (R32_Float)
    depth_target.write(depth, tid);
}
