#include <metal_stdlib>
using namespace metal;

#include "VolumeCommon.metal"

// ============================================================================
// Volume Object — Proxy Cube Forward Fragment Ray March (PBR Cloud)
// ============================================================================
// Renders a unit cube via procedural vertex shader (36 verts from vertex_id).
// Fragment shader ray-marches with PBR volumetric integration:
// - Beer-Lambert extinction with height-based density modulation
// - Henyey-Greenstein dual-lobe phase function
// - Self-shadowing via light-direction ray march
//
// Output: float4(in_scatter_rgb, transmittance)
//   FusionComposite blends: result = (scene + indirect) * transmittance + scatter
// ============================================================================

// ============================================================================
// Procedural unit cube vertex generation (36 vertices = 6 faces × 2 tris × 3)
// ============================================================================

float3 generateCubeVertex(uint vid) {
    const float3 kVerts[36] = {
        float3( 0.5, -0.5, -0.5), float3( 0.5,  0.5, -0.5), float3( 0.5,  0.5,  0.5),
        float3( 0.5, -0.5, -0.5), float3( 0.5,  0.5,  0.5), float3( 0.5, -0.5,  0.5),
        float3(-0.5, -0.5,  0.5), float3(-0.5,  0.5,  0.5), float3(-0.5,  0.5, -0.5),
        float3(-0.5, -0.5,  0.5), float3(-0.5,  0.5, -0.5), float3(-0.5, -0.5, -0.5),
        float3(-0.5,  0.5, -0.5), float3(-0.5,  0.5,  0.5), float3( 0.5,  0.5,  0.5),
        float3(-0.5,  0.5, -0.5), float3( 0.5,  0.5,  0.5), float3( 0.5,  0.5, -0.5),
        float3(-0.5, -0.5,  0.5), float3(-0.5, -0.5, -0.5), float3( 0.5, -0.5, -0.5),
        float3(-0.5, -0.5,  0.5), float3( 0.5, -0.5, -0.5), float3( 0.5, -0.5,  0.5),
        float3(-0.5, -0.5,  0.5), float3( 0.5, -0.5,  0.5), float3( 0.5,  0.5,  0.5),
        float3(-0.5, -0.5,  0.5), float3( 0.5,  0.5,  0.5), float3(-0.5,  0.5,  0.5),
        float3( 0.5, -0.5, -0.5), float3(-0.5, -0.5, -0.5), float3(-0.5,  0.5, -0.5),
        float3( 0.5, -0.5, -0.5), float3(-0.5,  0.5, -0.5), float3( 0.5,  0.5, -0.5),
    };
    return kVerts[min(vid, 35u)];
}

struct VolumeTransform {
    float4x4 WorldMatrix;
    float4x4 ViewProjMatrix;
};

struct VolumeV2F {
    float4 position [[position]];
    float3 worldPos;
    float3 cameraPos;
};

vertex VolumeV2F volume_object_vertex(
    uint vid [[vertex_id]],
    constant VolumeParams& params [[buffer(0)]],
    constant VolumeTransform& transform [[buffer(1)]])
{
    VolumeV2F out;
    float3 cubePos = generateCubeVertex(vid);
    float4 worldPos = transform.WorldMatrix * float4(cubePos, 1.0);
    out.position  = transform.ViewProjMatrix * worldPos;
    out.worldPos  = worldPos.xyz;
    out.cameraPos = params.CameraPos.xyz;
    return out;
}

// ============================================================================
// Henyey-Greenstein phase function
// ============================================================================

float hg_phase(float g, float cos_theta) {
    float g2 = g * g;
    float denom = 1.0f + g2 - 2.0f * g * cos_theta;
    return (1.0f - g2) / (4.0f * M_PI_F * denom * sqrt(max(denom, 1e-6f)));
}

float dual_lobe_phase(float g, float cos_theta) {
    return 0.7f * hg_phase(g, cos_theta) + 0.3f * hg_phase(-0.3f, cos_theta);
}

// ============================================================================
// Height-based density modulation for cloud-like shape
// ============================================================================

float height_density(float3 uvw) {
    // Denser at bottom, sparser at top (cloud base)
    float h = uvw.y;
    float base = smoothstep(0.0f, 0.2f, h) * smoothstep(1.0f, 0.7f, h);

    // Add some variation
    float detail = worley_noise_3d(uvw * 8.0f + 100.0f);
    return base * (0.6f + 0.4f * detail);
}

// ============================================================================
// Light shadowing through volume
// ============================================================================

float light_shadow(
    float3 sample_pos, float3 to_light,
    float3 volume_min, float3 volume_max,
    float sigma_t_scale,
    texture3d<float, access::sample> noise_tex,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    constant VolumeParams& params)
{
    float t_near, t_far;
    if (!intersect_aabb(sample_pos, to_light, volume_min, volume_max, t_near, t_far)) {
        return 1.0f;
    }

    float t_start = max(t_near, 0.0f);
    float march_len = t_far - t_start;
    uint shadow_steps = 8;
    float dt = march_len / float(shadow_steps);
    float accum = 0.0f;

    for (uint i = 0; i < shadow_steps; i++) {
        float t = t_start + dt * (float(i) + 0.3f);  // slight offset to avoid self-shadow
        if (t >= t_far) break;
        float3 pos = sample_pos + to_light * t;
        float d = sample_density(pos, noise_tex, sdf0, sdf1, sdf2, params);
        accum += d * dt;
    }

    // Beer-Powder: darken silhouettes with a powder term
    float beer = exp(-sigma_t_scale * accum);
    float powder = 1.0f - exp(-sigma_t_scale * accum * 2.0f);
    return max(beer * powder, 0.05f);  // floor to avoid complete black
}

// ============================================================================
// Fragment Shader — PBR Volumetric Ray March
// ============================================================================

fragment float4 volume_object_fragment(
    VolumeV2F in [[stage_in]],
    texture3d<float, access::sample> noise_tex     [[texture(0)]],
    texture3d<float, access::sample> sdf_cascade_0 [[texture(1)]],
    texture3d<float, access::sample> sdf_cascade_1 [[texture(2)]],
    texture3d<float, access::sample> sdf_cascade_2 [[texture(3)]],
    constant VolumeParams& params                  [[buffer(0)]])
{
    float3 ray_origin = in.cameraPos;
    float3 ray_dir    = normalize(in.worldPos - in.cameraPos);

    float3 volume_min = params.VolumeOrigin.xyz;
    float3 volume_max = params.VolumeOrigin.xyz + params.VolumeExtent.xyz;

    float t_min, t_max;
    if (!intersect_aabb(ray_origin, ray_dir, volume_min, volume_max, t_min, t_max)) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    t_min = max(t_min, 0.0f);
    if (t_min >= t_max) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    // Volume material parameters
    const float sigma_a = 0.8f;                  // high absorption → dark interior
    const float sigma_s = 1.0f;                  // moderate scattering
    const float sigma_t = sigma_a + sigma_s;     // 1.8
    const float g       = 0.67f;                 // strong forward scattering
    const float dt      = 0.10f;

    float3 light_dir   = normalize(params.LightDirection.xyz);
    float3 to_light    = -light_dir;
    float3 light_color = params.LightColor.xyz;

    // Phase function
    float cos_theta = dot(-ray_dir, light_dir);
    float phase_val = dual_lobe_phase(g, cos_theta);

    // Forward ray march with per-sample light march (HZD/Nubis pattern)
    float3 total_scatter = float3(0.0f);
    float  transmittance = 1.0f;

    float t = t_min;
    for (uint step = 0; step < params.MaxSteps && transmittance > 0.01f; step++) {
        if (t >= t_max) break;

        float3 pos = ray_origin + t * ray_dir;
        float raw_density = sample_density(pos, noise_tex,
            sdf_cascade_0, sdf_cascade_1, sdf_cascade_2, params);
        float density = raw_density * params.ExtinctionScale;

        if (density > 0.001f) {
            // Per-sample shadow: march toward light (6 steps)
            float light_accum = 0.0f;
            {
                float sn, sf;
                if (intersect_aabb(pos, to_light, volume_min, volume_max, sn, sf)) {
                    float s_start = max(sn, 0.0f);
                    float s_dt = (sf - s_start) / 6.0f;
                    for (uint si = 0; si < 6; si++) {
                        float st = s_start + s_dt * (float(si) + 0.3f);
                        if (st >= sf) break;
                        float3 spos = pos + to_light * st;
                        light_accum += sample_density(spos, noise_tex,
                            sdf_cascade_0, sdf_cascade_1, sdf_cascade_2, params) * s_dt;
                    }
                }
            }

            // Beer-Powder with ExtinctionScale applied to shadow
            float optical_depth = sigma_t * light_accum * params.ExtinctionScale;
            float beer   = exp(-optical_depth);
            float powder = 1.0f - exp(-optical_depth * 2.0f);
            float shadow = max(beer * powder, 0.02f);

            // In-scattering with light tinting
            float3 scattered_light = light_color * shadow;
            float3 in_scatter = transmittance * phase_val * sigma_s * density * dt
                              * scattered_light;

            // Ambient (blueish tint)
            float3 ambient = float3(0.3f, 0.35f, 0.5f);
            in_scatter += transmittance * sigma_s * density * dt * ambient * 0.15f;

            total_scatter += in_scatter;

            // View ray extinction
            transmittance *= exp(-sigma_t * density * dt);
        }

        t += dt;
    }

    return float4(total_scatter, transmittance);
}
