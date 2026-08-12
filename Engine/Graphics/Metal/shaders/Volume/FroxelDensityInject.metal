#include <metal_stdlib>
using namespace metal;

#include "FroxelCommon.metal"

// ============================================================================
// Pass 1: Froxel Density Inject
// ============================================================================
// Each thread processes one froxel (x, y, z).
// Reconstructs world position, samples density (noise + SDF), writes R channel.

kernel void froxel_density_inject(
    texture3d<float, access::write>   froxel_density [[texture(0)]],
    texture3d<float, access::sample>  noise_tex      [[texture(1)]],
    texture3d<float, access::sample>  sdf_cascade_0  [[texture(2)]],
    texture3d<float, access::sample>  sdf_cascade_1  [[texture(3)]],
    texture3d<float, access::sample>  sdf_cascade_2  [[texture(4)]],
    texture2d<float, access::read>    depth_buffer   [[texture(5)]],
    constant FroxelParams& params                    [[buffer(0)]],
    uint3 gid [[thread_position_in_grid]])
{
    if (gid.x >= froxel_density.get_width()  ||
        gid.y >= froxel_density.get_height() ||
        gid.z >= froxel_density.get_depth()) return;

    // Reconstruct world position from froxel coordinates
    float3 world_pos = froxel_to_world(gid, params);

    // Optional: skip froxels behind scene geometry
    // Compute screen position at half-res
    float2 screen_uv = (float2(gid.xy) + 0.5) / float2(params.FroxelDims.xy);
    uint2 depth_pixel = uint2(screen_uv * float2(params.ScreenParams.xy));
    depth_pixel = clamp(depth_pixel, uint2(0), uint2(params.ScreenParams.xy) - 1u);
    float scene_depth = depth_buffer.read(depth_pixel).x;

    if (scene_depth > 0.0 && scene_depth < 1.0) {
        float scene_z = ndc_to_view_z(scene_depth, params);
        float froxel_z = froxel_z_to_depth(float(gid.z), params);
        if (froxel_z > scene_z) {
            froxel_density.write(0.0, gid);
            return;
        }
    }

    // Sample density
    float density = froxel_sample_density(world_pos, noise_tex,
                                           sdf_cascade_0, sdf_cascade_1, sdf_cascade_2,
                                           params);

    // Threshold
    if (density < params.VolumeParams1.z) density = 0.0;

    froxel_density.write(density, gid);
}
