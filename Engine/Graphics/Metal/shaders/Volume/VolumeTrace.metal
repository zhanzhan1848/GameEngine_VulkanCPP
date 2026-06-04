#include <metal_stdlib>
using namespace metal;

#include "VolumeCommon.metal"

// ============================================================================
// Pass 1: volume_density_trace
// ============================================================================
// AABB-bounded fog volume with Beer-Lambert ray march.

kernel void volume_density_trace(
    texture2d<float, access::read>   depth_buffer  [[texture(0)]],
    texture3d<float, access::sample> noise_tex     [[texture(1)]],
    texture3d<float, access::sample> sdf_cascade_0 [[texture(2)]],
    texture3d<float, access::sample> sdf_cascade_1 [[texture(3)]],
    texture3d<float, access::sample> sdf_cascade_2 [[texture(4)]],

    device half4*  scatter_buffer          [[buffer(0)]],
    device float*  transmittance_log_buffer [[buffer(1)]],
    constant VolumeParams& params          [[buffer(2)]],

    uint2 gid [[thread_position_in_grid]])
{
    uint w = depth_buffer.get_width()  / 2u;
    uint h = depth_buffer.get_height() / 2u;
    uint pixel_idx = gid.y * w + gid.x;
    if (gid.x >= w || gid.y >= h) return;

    float3 total_scatter = float3(0.0f);
    float transmittance = 1.0f;

    uint2 full_res_pixel = gid * 2u + 1u;
    if (full_res_pixel.x < depth_buffer.get_width() &&
        full_res_pixel.y < depth_buffer.get_height()) {

        float depth = depth_buffer.read(full_res_pixel).x;
        float2 screen_uv = (float2(gid) + 0.5f) / float2(w, h);
        float3 world_pos = reconstruct_world_pos(screen_uv, depth, params);
        float3 ray_origin = params.CameraPos.xyz;
        float3 ray_dir = normalize(world_pos - ray_origin);

        float3 volume_min = params.VolumeOrigin.xyz;
        float3 volume_max = params.VolumeOrigin.xyz + params.VolumeExtent.xyz;

        float t_min, t_max;
        if (intersect_aabb(ray_origin, ray_dir, volume_min, volume_max, t_min, t_max)) {
            float scene_t = length(world_pos - ray_origin);
            t_max = min(t_max, scene_t);
            t_min = max(t_min, 0.0f);

            if (t_min < t_max) {
                float3 light_color = params.LightColor.xyz;
                float t = t_min;

                for (uint step = 0; step < params.MaxSteps && transmittance > 0.01f; step++) {
                    if (t >= t_max) break;

                    float3 pos = ray_origin + t * ray_dir;
                    float density = sample_density(pos, noise_tex, sdf_cascade_0, sdf_cascade_1, sdf_cascade_2, params);

                    if (density > params.DensityThreshold) {
                        float sigma_s = density * params.ScatteringAlbedo * params.ExtinctionScale;
                        float sigma_t = density * params.ExtinctionScale;
                        float dt = params.StepSize;

                        float3 scatter = transmittance * sigma_s * dt * light_color;
                        scatter += transmittance * sigma_s * dt * float3(0.4f, 0.45f, 0.6f) * 0.3f;

                        total_scatter += scatter;
                        transmittance *= exp(-sigma_t * dt);
                    }

                    t += params.StepSize;
                }
            }
        }
    }

    scatter_buffer[pixel_idx] = half4(half(total_scatter.x), half(total_scatter.y),
                                       half(total_scatter.z), half(transmittance));
    transmittance_log_buffer[pixel_idx] = transmittance;
}

// ============================================================================
// Pass 2: volume_lighting_eval
// ============================================================================

kernel void volume_lighting_eval(
    texture2d<float, access::sample> scene_color  [[texture(0)]],
    texture2d<float, access::sample> shadow_map   [[texture(1)]],
    texture2d<float, access::write>  output_color  [[texture(2)]],

    device const half4*  scatter_buffer           [[buffer(0)]],
    device const float*  transmittance_log_buffer [[buffer(1)]],
    constant VolumeParams& params                 [[buffer(2)]],

    uint2 gid [[thread_position_in_grid]])
{
    uint w = output_color.get_width();
    uint h = output_color.get_height();
    if (gid.x >= w || gid.y >= h) return;

    uint pixel_idx = gid.y * w + gid.x;
    float3 scatter = float3(scatter_buffer[pixel_idx].xyz);
    float transmittance = transmittance_log_buffer[pixel_idx];

    output_color.write(float4(scatter, transmittance), gid);
}
