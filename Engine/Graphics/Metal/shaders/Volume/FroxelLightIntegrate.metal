#include <metal_stdlib>
using namespace metal;

#include "FroxelCommon.metal"

// ============================================================================
// Pass 2: Froxel Light Integrate
// ============================================================================
// Each thread processes one froxel column (x, y), all Z slices serially.
// Reads density from froxel_density, accumulates Beer-Lambert + direct lighting,
// writes per-slice (scatter_rgb, transmittance) to froxel_scatter.

kernel void froxel_light_integrate(
    texture3d<float, access::sample>  froxel_density [[texture(0)]],
    texture3d<float, access::write>   froxel_scatter [[texture(1)]],
    texture2d<float, access::sample>  shadow_map     [[texture(2)]],
    constant FroxelParams& params                    [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= uint(params.FroxelDims.x) ||
        gid.y >= uint(params.FroxelDims.y)) return;

    constexpr sampler bilinear(coord::normalized, filter::linear,
                               address::clamp_to_edge, mip_filter::none);

    float3 total_scatter(0.0f, 0.0f, 0.0f);
    float transmittance = 1.0;
    float extinction_scale = params.VolumeParams1.x;
    float scattering_albedo = params.VolumeParams1.y;
    float ambient = params.VolumeParams2.z;
    float3 light_color = params.LightColor.xyz;
    float3 light_dir = normalize(params.LightDirection.xyz);

    uint depth_count = uint(params.FroxelDims.z);

    for (uint z = 0; z < depth_count; ++z) {
        // Read density from previous pass
        float3 uvw = (float3(float2(gid), float(z)) + 0.5) / float3(params.FroxelDims.xyz);
        float density = froxel_density.sample(bilinear, uvw).r;

        // Compute slice thickness (exponential Z)
        float z0 = froxel_z_to_depth(float(z) - 0.5, params);
        float z1 = froxel_z_to_depth(float(z) + 0.5, params);
        float dz = z1 - z0;

        if (density > 0.001) {
            float sigma_t = density * extinction_scale;
            float sigma_s = density * scattering_albedo * extinction_scale;

            // World position for shadow lookup
            float3 world_pos = froxel_to_world(uint3(gid, z), params);

            // Direct lighting: shadow sample via light VP projection
            float shadow = 1.0;
            float3 ndc0 = shadow_project(world_pos, params.ShadowVP[0]);
            float3 ndc1 = shadow_project(world_pos, params.ShadowVP[1]);

            bool in_c0 = ndc0.x >= -1.0 && ndc0.x <= 1.0 &&
                         ndc0.y >= -1.0 && ndc0.y <= 1.0 &&
                         ndc0.z >= 0.0  && ndc0.z <= 1.0;
            float2 sc = in_c0 ? shadow_ndc_to_uv(ndc0) : shadow_ndc_to_uv(ndc1);

            if (sc.x > 0.0 && sc.x < 1.0 && sc.y > 0.0 && sc.y < 1.0) {
                shadow = shadow_map.sample(bilinear, sc).r;
            }

            // Henyey-Greenstein phase (simplified isotropic for now)
            float phase = 1.0 / (4.0 * 3.14159);

            // Direct in-scatter
            float3 direct = light_color * shadow * phase * sigma_s;

            // Ambient term
            float3 ambient_scatter = float3(0.4, 0.45, 0.6) * ambient * sigma_s;

            // Beer-Lambert integration
            transmittance *= exp(-sigma_t * dz);
            total_scatter += (direct + ambient_scatter) * transmittance * dz;
        }

        // Write cumulative result at this slice
        froxel_scatter.write(float4(total_scatter, transmittance), uint3(gid, z));
    }
}
