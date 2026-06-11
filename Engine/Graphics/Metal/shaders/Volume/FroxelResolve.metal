#include <metal_stdlib>
using namespace metal;

#include "FroxelCommon.metal"

// ============================================================================
// Pass 3: Froxel Resolve
// ============================================================================
// Each thread processes one screen pixel (half-res).
// Reads depth, computes froxel UVW, trilinear samples froxel_scatter,
// outputs RGBA16_Float (RGB=scatter, A=transmittance).

kernel void froxel_resolve(
    texture3d<float, access::sample>  froxel_scatter [[texture(0)]],
    texture2d<float, access::read>    depth_buffer   [[texture(1)]],
    texture2d<float, access::write>   output_color   [[texture(2)]],
    constant FroxelParams& params                    [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    uint w = output_color.get_width();
    uint h = output_color.get_height();
    if (gid.x >= w || gid.y >= h) return;

    // Sample depth buffer at full-res center of this half-res pixel
    uint2 full_res_pixel = gid * 2u + 1u;
    full_res_pixel = min(full_res_pixel,
                         uint2(params.ScreenParams.xy) - 1u);
    float depth = depth_buffer.read(full_res_pixel).x;

    // Default: identity (no fog)
    float4 result = float4(0.0, 0.0, 0.0, 1.0);

    if (depth > 0.0 && depth < 1.0) {
        // View-space Z from NDC depth
        float z_view = ndc_to_view_z(depth, params);

        // Froxel W coordinate [0, 1]
        float fw = view_z_to_froxel_w(z_view, params);

        if (fw >= 0.0 && fw <= 1.0) {
            // Screen UV for froxel lookup
            float2 screen_uv = (float2(gid) + 0.5) / float2(w, h);

            // Froxel UVW
            float3 uvw = float3(screen_uv, fw);

            // Trilinear sample
            constexpr sampler s(coord::normalized, filter::linear,
                                address::clamp_to_edge, mip_filter::none);
            result = froxel_scatter.sample(s, uvw);
        }
    }

    output_color.write(result, gid);
}
