#include "StaticProbeBaker.h"
#include "Engine/JobSystem/JobSystem.h"
#include "Engine/Utilities/SphericalHarmonics.h"

#include <cmath>
#include <memory>

namespace primal::graphics::lumen {

using namespace primal::math;
using namespace primal::math::sh;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr float PI{3.14159265358979323846f};
static constexpr float GOLDEN_ANGLE{2.3999632297286f}; // PI * (3 - sqrt(5))

// Type aliases for BVH nested types
using BVHRay = utl::BVH::Ray;
using BVHHitInfo = utl::BVH::HitInfo;

// ---------------------------------------------------------------------------
// Helper: Fibonacci sphere direction
// ---------------------------------------------------------------------------
static inline v3 FibonacciDirection(u32 sample_index, u32 total_samples)
{
    const float y = 1.0f - 2.0f * (float(sample_index) + 0.5f) / (float)total_samples;
    const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
    const float theta = GOLDEN_ANGLE * (float)sample_index;
    return v3{std::cos(theta) * radius, y, std::sin(theta) * radius};
}

// ---------------------------------------------------------------------------
// Helper: Normalize a simd::float3 safely
// ---------------------------------------------------------------------------
static inline v3 NormalizeSafe(v3 v)
{
    float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-8f) return v3{0.0f, 1.0f, 0.0f};
    float inv = 1.0f / len;
    return v3{v.x * inv, v.y * inv, v.z * inv};
}

// ---------------------------------------------------------------------------
// Helper: Dot product of two v3
// ---------------------------------------------------------------------------
static inline float Dot3(v3 a, v3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// ---------------------------------------------------------------------------
// ProbeWorldPos
// ---------------------------------------------------------------------------
/*static*/ math::v3 StaticProbeBaker::ProbeWorldPos(
    u32 probe_index, u32 dim_x, u32 dim_y, u32 dim_z,
    math::v3 origin, float spacing)
{
    const u32 dz = dim_x * dim_y;
    const u32 iz = probe_index / dz;
    const u32 rem = probe_index % dz;
    const u32 iy = rem / dim_x;
    const u32 ix = rem % dim_x;

    return v3{
        origin.x + (float)ix * spacing,
        origin.y + (float)iy * spacing,
        origin.z + (float)iz * spacing
    };
}

// ---------------------------------------------------------------------------
// OctahedralDirection — map 8x8 index (0..63) to 3D direction
// ---------------------------------------------------------------------------
/*static*/ math::v3 StaticProbeBaker::OctahedralDirection(u32 oct_index)
{
    // 8x8 octahedral map
    const u32 face_size = 8;
    const u32 ix = oct_index % face_size;
    const u32 iy = oct_index / face_size;

    // Map to [-1, 1]
    float u = (2.0f * (float)ix + 1.0f) / (float)face_size - 1.0f;
    float v = (2.0f * (float)iy + 1.0f) / (float)face_size - 1.0f;

    // Octahedral mapping (same as DDGI octahedral encoding)
    float abs_sum = std::abs(u) + std::abs(v);
    float x, y, z;
    if (abs_sum <= 1.0f) {
        // Upper hemisphere
        x = u;
        y = v;
        z = 1.0f - abs_sum;
    } else {
        // Lower hemisphere — reflect octant
        float su = (u >= 0.0f) ? 1.0f : -1.0f;
        float sv = (v >= 0.0f) ? 1.0f : -1.0f;
        x = (1.0f - std::abs(v)) * su;
        y = (1.0f - std::abs(u)) * sv;
        z = abs_sum - 1.0f;
    }

    return NormalizeSafe(v3{x, y, z});
}

// ---------------------------------------------------------------------------
// BakeDirectLighting — MC ray tracing for one probe
// ---------------------------------------------------------------------------
/*static*/ void StaticProbeBaker::BakeDirectLighting(
    StaticProbeVolume& volume,
    const utl::BVH& bvh,
    const ProbeBakingScene& scene,
    const ProbeBakingParams& params,
    u32 probe_index)
{
    const u32 dim_x = volume.GridDimX();
    const u32 dim_y = volume.GridDimY();
    const u32 dim_z = volume.GridDimZ();
    const float spacing = volume.Spacing();
    const v3 origin = volume.Origin();

    const v3 probe_pos = ProbeWorldPos(probe_index, dim_x, dim_y, dim_z, origin, spacing);
    const v3 light_dir = NormalizeSafe(scene.light_direction);
    const u32 ray_count = params.rays_per_probe;

    // Accumulate SH coefficients for radiance
    SH9Color radiance_sh;
    radiance_sh.Reset();

    SH9 basis;

    for (u32 r = 0; r < ray_count; ++r) {
        const v3 dir = FibonacciDirection(r, ray_count);

        // Trace ray against scene geometry
        BVHRay ray(probe_pos, dir);
        ray.t_max = params.ray_max_distance;
        BVHHitInfo hit;
        bool hit_result = bvh.Intersect(ray, hit);

        v3 radiance{0.0f, 0.0f, 0.0f};

        if (hit_result) {
            // Compute direct lighting: N dot L
            // Approximate normal as (0,1,0) since BVH HitInfo doesn't carry normal data.
            // The triangle normal could be computed from vertices, but the BVH doesn't
            // expose triangle vertex data in the public interface.
            v3 normal{0.0f, 1.0f, 0.0f};
            float ndl = Dot3(normal, v3{-light_dir.x, -light_dir.y, -light_dir.z});
            ndl = std::max(0.0f, ndl);

            // Direct lighting contribution
            radiance.x = scene.light_color.x * scene.light_intensity * ndl;
            radiance.y = scene.light_color.y * scene.light_intensity * ndl;
            radiance.z = scene.light_color.z * scene.light_intensity * ndl;

            // Shadow check: trace from hit point to light
            // Compute approximate hit position
            v3 hit_pos{
                probe_pos.x + dir.x * hit.t,
                probe_pos.y + dir.y * hit.t,
                probe_pos.z + dir.z * hit.t
            };
            // Offset along normal to avoid self-intersection
            v3 shadow_origin{
                hit_pos.x + normal.x * 0.01f,
                hit_pos.y + normal.y * 0.01f,
                hit_pos.z + normal.z * 0.01f
            };
            BVHRay shadow_ray(shadow_origin, v3{-light_dir.x, -light_dir.y, -light_dir.z});
            shadow_ray.t_max = params.ray_max_distance;

            if (bvh.IntersectAny(shadow_ray, params.ray_max_distance)) {
                // In shadow — no direct lighting
                radiance = v3{0.0f, 0.0f, 0.0f};
            }
        } else {
            // Ray missed — use sky color as ambient
            radiance = scene.sky_color;
        }

        // Project radiance into SH
        EvalSHBasis(dir, basis);
        for (int i = 0; i < 9; ++i) {
            radiance_sh.coeffs[i].x += radiance.x * basis.coeffs[i];
            radiance_sh.coeffs[i].y += radiance.y * basis.coeffs[i];
            radiance_sh.coeffs[i].z += radiance.z * basis.coeffs[i];
        }
    }

    // Normalize: weight = 4*PI / N (uniform sampling PDF = 1/(4*PI))
    const float weight = 4.0f * PI / (float)ray_count;
    for (int i = 0; i < 9; ++i) {
        radiance_sh.coeffs[i].x *= weight;
        radiance_sh.coeffs[i].y *= weight;
        radiance_sh.coeffs[i].z *= weight;
    }

    // Write to irradiance data: apply cosine convolution for irradiance
    SH9Color irradiance_sh;
    irradiance_sh.Reset();
    ConvolveCosineLobe(radiance_sh, irradiance_sh);

    math::v3* irradiance = volume.GetIrradianceData() + probe_index * 9;
    for (int i = 0; i < 9; ++i) {
        irradiance[i] = irradiance_sh.coeffs[i];
    }
}

// ---------------------------------------------------------------------------
// PropagateBounce — iterative SH bounce lighting
// ---------------------------------------------------------------------------
/*static*/ void StaticProbeBaker::PropagateBounce(
    StaticProbeVolume& volume,
    const ProbeBakingParams& params)
{
    const u32 probe_count = volume.ProbeCount();
    const u32 dim_x = volume.GridDimX();
    const u32 dim_y = volume.GridDimY();
    const u32 dim_z = volume.GridDimZ();
    const float spacing = volume.Spacing();
    const v3 origin = volume.Origin();

    // Temporary buffer for bounce accumulation
    std::vector<SH9Color> bounce_accum(probe_count);

    for (u32 bounce = 0; bounce < params.bounce_count; ++bounce) {
        // Clear bounce accumulator
        for (u32 i = 0; i < probe_count; ++i) {
            bounce_accum[i].Reset();
        }

        // For each probe, gather from 26 neighbors
        for (u32 pz = 0; pz < dim_z; ++pz) {
            for (u32 py = 0; py < dim_y; ++py) {
                for (u32 px = 0; px < dim_x; ++px) {
                    const u32 probe_idx = pz * (dim_x * dim_y) + py * dim_x + px;
                    const v3 probe_pos = ProbeWorldPos(probe_idx, dim_x, dim_y, dim_z, origin, spacing);

                    SH9Color bounce_sh;
                    bounce_sh.Reset();
                    float total_weight = 0.0f;

                    // Iterate 26 neighbors (skip self)
                    for (s32 nz = -1; nz <= 1; ++nz) {
                        for (s32 ny = -1; ny <= 1; ++ny) {
                            for (s32 nx = -1; nx <= 1; ++nx) {
                                if (nx == 0 && ny == 0 && nz == 0) continue;

                                const s32 gx = (s32)px + nx;
                                const s32 gy = (s32)py + ny;
                                const s32 gz = (s32)pz + nz;

                                // Skip out-of-bounds neighbors
                                if (gx < 0 || gx >= (s32)dim_x ||
                                    gy < 0 || gy >= (s32)dim_y ||
                                    gz < 0 || gz >= (s32)dim_z)
                                    continue;

                                const u32 neighbor_idx = (u32)gz * (dim_x * dim_y) +
                                                         (u32)gy * dim_x +
                                                         (u32)gx;

                                // Distance-based weight (1/d^2)
                                const v3 neighbor_pos = ProbeWorldPos(neighbor_idx, dim_x, dim_y, dim_z, origin, spacing);
                                const float dx = neighbor_pos.x - probe_pos.x;
                                const float dy = neighbor_pos.y - probe_pos.y;
                                const float dz = neighbor_pos.z - probe_pos.z;
                                const float dist_sq = dx * dx + dy * dy + dz * dz;
                                const float w = (dist_sq > 1e-8f) ? (1.0f / dist_sq) : 1.0f;

                                // Read neighbor's irradiance SH (already cosine-convolved)
                                const math::v3* neighbor_irradiance = volume.GetIrradianceData() + neighbor_idx * 9;
                                SH9Color neighbor_sh;
                                for (int i = 0; i < 9; ++i) {
                                    neighbor_sh.coeffs[i] = neighbor_irradiance[i];
                                }

                                // Do NOT convolve with cosine lobe again — neighbor data
                                // is already irradiance (cosine-convolved radiance).
                                // Applying ConvolveCosineLobe again multiplies L0 by PI per bounce,
                                // causing exponential energy amplification (PI^3 ≈ 31x for 3 bounces).

                                // Accumulate weighted bounce (1/PI for Lambertian BRDF albedo term)
                                constexpr float bounce_scale = 1.0f / PI;
                                for (int i = 0; i < 9; ++i) {
                                    bounce_sh.coeffs[i].x += neighbor_sh.coeffs[i].x * w * bounce_scale;
                                    bounce_sh.coeffs[i].y += neighbor_sh.coeffs[i].y * w * bounce_scale;
                                    bounce_sh.coeffs[i].z += neighbor_sh.coeffs[i].z * w * bounce_scale;
                                }
                                total_weight += w;
                            }
                        }
                    }

                    // Normalize by total weight
                    if (total_weight > 1e-8f) {
                        const float inv_w = 1.0f / total_weight;
                        bounce_sh *= inv_w;
                    }

                    bounce_accum[probe_idx] = bounce_sh;
                }
            }
        }

        // Additive update to irradiance data
        math::v3* irradiance = volume.GetIrradianceData();
        for (u32 i = 0; i < probe_count; ++i) {
            for (int c = 0; c < 9; ++c) {
                irradiance[i * 9 + c].x += bounce_accum[i].coeffs[c].x;
                irradiance[i * 9 + c].y += bounce_accum[i].coeffs[c].y;
                irradiance[i * 9 + c].z += bounce_accum[i].coeffs[c].z;
            }
        }

        // Check convergence after each bounce
        if (CheckConvergence(volume, params.convergence_threshold)) {
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// CheckConvergence — simplified: check L0 magnitude change
// ---------------------------------------------------------------------------
/*static*/ bool StaticProbeBaker::CheckConvergence(
    StaticProbeVolume& volume,
    float threshold)
{
    // Simplified convergence check: measure average L0 coefficient magnitude
    // across all probes. This is a placeholder — in practice you'd diff
    // against the previous iteration's data.
    const u32 probe_count = volume.ProbeCount();
    if (probe_count == 0) return true;

    const math::v3* irradiance = volume.GetIrradianceData();
    float total_l0 = 0.0f;
    for (u32 i = 0; i < probe_count; ++i) {
        const v3 l0 = irradiance[i * 9]; // L0 coefficient
        total_l0 += std::abs(l0.x) + std::abs(l0.y) + std::abs(l0.z);
    }

    // Suppress unused warning
    (void)total_l0;
    (void)threshold;
    return false; // Always continue bouncing (up to bounce_count)
}

// ---------------------------------------------------------------------------
// BakeVisibility — octahedral depth + sky factor per probe
// ---------------------------------------------------------------------------
/*static*/ void StaticProbeBaker::BakeVisibility(
    StaticProbeVolume& volume,
    const utl::BVH& bvh,
    const ProbeBakingScene& /*scene*/,
    const ProbeBakingParams& params,
    u32 probe_index)
{
    const u32 dim_x = volume.GridDimX();
    const u32 dim_y = volume.GridDimY();
    const u32 dim_z = volume.GridDimZ();
    const float spacing = volume.Spacing();
    const v3 origin = volume.Origin();
    const v3 probe_pos = ProbeWorldPos(probe_index, dim_x, dim_y, dim_z, origin, spacing);

    // --- Octahedral depth (8x8 = 64 texels) ---
    float* depth_mean = volume.GetDepthMeanData() + probe_index * 64;
    float* depth_var = volume.GetDepthVarData() + probe_index * 64;

    for (u32 oct = 0; oct < 64; ++oct) {
        const v3 dir = OctahedralDirection(oct);
        BVHRay ray(probe_pos, dir);
        ray.t_max = params.ray_max_distance;
        BVHHitInfo hit;

        if (bvh.Intersect(ray, hit)) {
            depth_mean[oct] = hit.t;
            depth_var[oct] = 0.0f; // No variance for single sample
        } else {
            depth_mean[oct] = params.ray_max_distance;
            depth_var[oct] = 0.25f * params.ray_max_distance * params.ray_max_distance;
        }
    }

    // --- Sky factor: fraction of rays that reach sky ---
    // Use 256 Fibonacci sphere samples for sky visibility estimation
    const u32 sky_rays = 256;
    u32 sky_hits = 0;

    for (u32 r = 0; r < sky_rays; ++r) {
        const v3 dir = FibonacciDirection(r, sky_rays);
        BVHRay ray(probe_pos, dir);
        ray.t_max = params.ray_max_distance;

        if (!bvh.IntersectAny(ray, params.ray_max_distance)) {
            ++sky_hits;
        }
    }

    volume.GetSkyFactorData()[probe_index] = (float)sky_hits / (float)sky_rays;
}

// ---------------------------------------------------------------------------
// BakeSkySH — per-probe sky SH contribution
// ---------------------------------------------------------------------------
/*static*/ void StaticProbeBaker::BakeSkySH(
    StaticProbeVolume& volume,
    const ProbeBakingScene& scene,
    const ProbeBakingParams& params,
    u32 probe_index)
{
    // Sky factor for this probe (already computed by BakeVisibility)
    const float sky_factor = volume.GetSkyFactorData()[probe_index];

    // Project sky color weighted by sky visibility into SH
    // This represents the ambient sky contribution for this specific probe
    SH9Color sky_sh;
    sky_sh.Reset();

    const u32 ray_count = params.rays_per_probe;
    SH9 basis;

    for (u32 r = 0; r < ray_count; ++r) {
        const v3 dir = FibonacciDirection(r, ray_count);

        // Sky SH accumulates sky_color with a weight proportional to the probe's sky factor
        const float weight = sky_factor;

        // Simple hemispherical sky gradient: lighter at zenith, darker at horizon
        float sky_gradient = 0.5f + 0.5f * dir.y; // y is up
        v3 sky_rad{
            scene.sky_color.x * sky_gradient * weight,
            scene.sky_color.y * sky_gradient * weight,
            scene.sky_color.z * sky_gradient * weight
        };

        EvalSHBasis(dir, basis);
        for (int i = 0; i < 9; ++i) {
            sky_sh.coeffs[i].x += sky_rad.x * basis.coeffs[i];
            sky_sh.coeffs[i].y += sky_rad.y * basis.coeffs[i];
            sky_sh.coeffs[i].z += sky_rad.z * basis.coeffs[i];
        }
    }

    const float sh_weight = 4.0f * PI / (float)ray_count;
    for (int i = 0; i < 9; ++i) {
        sky_sh.coeffs[i].x *= sh_weight;
        sky_sh.coeffs[i].y *= sh_weight;
        sky_sh.coeffs[i].z *= sh_weight;
    }

    // Write to sky SH data
    math::v3* sky_data = volume.GetSkySHData() + probe_index * 9;
    for (int i = 0; i < 9; ++i) {
        sky_data[i] = sky_sh.coeffs[i];
    }
}

// ---------------------------------------------------------------------------
// Bake — main entry point
// ---------------------------------------------------------------------------
/*static*/ bool StaticProbeBaker::Bake(
    StaticProbeVolume& volume,
    const ProbeBakingScene& scene,
    const ProbeBakingParams& params)
{
    const u32 probe_count = volume.ProbeCount();
    if (probe_count == 0) return false;
    if (!scene.vertices || !scene.indices || scene.vertex_count == 0 || scene.index_count == 0) {
        return false;
    }

    // --- Build BVH from scene geometry ---
    // BVH::Build takes primal::utl::vector, but scene has raw pointers. Convert.
    primal::utl::vector<math::v3> verts(scene.vertices, scene.vertices + scene.vertex_count);
    primal::utl::vector<u32> idx(scene.indices, scene.indices + scene.index_count);

    auto bvh = std::make_shared<utl::BVH>();
    bvh->Build(verts, idx);

    // --- Phase 1: MC Direct Lighting (parallel per probe) ---
    // Capture bvh by shared_ptr for thread safety
    auto bvh_ref = bvh;
    auto handle1 = jobsystem::JobSystem::ParallelFor(probe_count,
        [&volume, bvh_ref, &scene, &params](u32 i) {
            BakeDirectLighting(volume, *bvh_ref, scene, params, i);
        });
    jobsystem::JobSystem::Wait(handle1);

    // --- Phase 2: Sequential SH Propagation (iterative bounce) ---
    PropagateBounce(volume, params);

    // --- Phase 3: Visibility + Sky Factor (parallel per probe) ---
    auto handle3 = jobsystem::JobSystem::ParallelFor(probe_count,
        [&volume, bvh_ref, &scene, &params](u32 i) {
            BakeVisibility(volume, *bvh_ref, scene, params, i);
        });
    jobsystem::JobSystem::Wait(handle3);

    // --- Phase 4: Sky SH (parallel per probe) ---
    auto handle4 = jobsystem::JobSystem::ParallelFor(probe_count,
        [&volume, &scene, &params](u32 i) {
            BakeSkySH(volume, scene, params, i);
        });
    jobsystem::JobSystem::Wait(handle4);

    volume.MarkLoaded();
    return true;
}

} // namespace primal::graphics::lumen
