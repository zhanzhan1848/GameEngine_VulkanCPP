#include "StaticProbeBaker.h"
#include "Engine/JobSystem/JobSystem.h"
#include "Engine/Utilities/SphericalHarmonics.h"

#include <chrono>
#include <cmath>
#include <iostream>
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
// ---------------------------------------------------------------------------
// BakeProbeRadiance — single-bounce indirect radiance SH per probe
// ---------------------------------------------------------------------------
// For each ray from the probe:
//   - If the ray hits scene geometry, compute the geometric normal from the
//     hit triangle's vertices, gather direct irradiance (sun, shadow-ray
//     gated) and sky irradiance (single upward shadow ray) at the hit point,
//     then convert to outgoing reflected radiance via the Lambertian BRDF
//     (albedo / PI) * (E_direct + E_sky).
//   - If the ray escapes, the probe sees the sky directly; use sky_color.
//
// Result is projected into SH9 with 4*PI/N_rays MC normalization. NO cosine
// convolution — output is radiance SH, matching the runtime convention used
// by DDGIUpdateIrradiance.wgsl and the shDot4 reconstruction in DDGIGIGather.
/*static*/ void StaticProbeBaker::BakeProbeRadiance(
    StaticProbeVolume& volume,
    const utl::BVH& bvh,
    const ProbeBakingScene& scene,
    const ProbeBakingParams& params,
    u32 probe_index)
{
    const u32 dim_x = volume.GridDimX();
    const u32 dim_y = volume.GridDimY();
    const u32 dim_z = volume.GridDimZ();

    const v3 probe_pos = ProbeWorldPos(probe_index, dim_x, dim_y, dim_z,
                                       volume.Origin(), volume.Spacing());
    const v3 light_dir  = NormalizeSafe(scene.light_direction);
    const v3 to_light   = v3{-light_dir.x, -light_dir.y, -light_dir.z};
    const v3 sky_up     = v3{0.0f, 1.0f, 0.0f};
    const float inv_pi  = 1.0f / PI;
    const u32 ray_count = params.rays_per_probe;

    SH9Color radiance_sh;
    radiance_sh.Reset();

    SH9 basis;

    for (u32 r = 0; r < ray_count; ++r) {
        const v3 dir = FibonacciDirection(r, ray_count);

        BVHRay ray(probe_pos, dir);
        ray.t_max = params.ray_max_distance;
        BVHHitInfo hit;

        v3 L{0.0f, 0.0f, 0.0f};

        if (bvh.Intersect(ray, hit)) {
            // Triangle vertices via hit.triangle_index (= BVH original_index = i in Build loop).
            // BVH::Triangle struct is private; we look up through the scene's index/vertex arrays.
            const u32 tri_idx = hit.triangle_index;
            const u32 i0 = scene.indices[tri_idx * 3 + 0];
            const u32 i1 = scene.indices[tri_idx * 3 + 1];
            const u32 i2 = scene.indices[tri_idx * 3 + 2];
            const v3 v0 = scene.vertices[i0];
            const v3 v1 = scene.vertices[i1];
            const v3 v2 = scene.vertices[i2];

            // Geometric normal via cross product of triangle edges
            const v3 edge1{v1.x - v0.x, v1.y - v0.y, v1.z - v0.z};
            const v3 edge2{v2.x - v0.x, v2.y - v0.y, v2.z - v0.z};
            const v3 normal = NormalizeSafe(v3{
                edge1.y * edge2.z - edge1.z * edge2.y,
                edge1.z * edge2.x - edge1.x * edge2.z,
                edge1.x * edge2.y - edge1.y * edge2.x});

            const v3 hit_pos{
                probe_pos.x + dir.x * hit.t,
                probe_pos.y + dir.y * hit.t,
                probe_pos.z + dir.z * hit.t,
            };
            // Offset along normal to avoid self-intersection on subsequent rays
            const v3 offset_pos{
                hit_pos.x + normal.x * 0.01f,
                hit_pos.y + normal.y * 0.01f,
                hit_pos.z + normal.z * 0.01f};

            // --- Direct irradiance at hit point (sun, with shadow ray) ---
            const float ndl = std::max(0.0f, Dot3(normal, to_light));
            v3 E_direct{0.0f, 0.0f, 0.0f};
            if (ndl > 0.0f) {
                BVHRay shadow_ray(offset_pos, to_light);
                shadow_ray.t_max = params.ray_max_distance;
                if (!bvh.IntersectAny(shadow_ray, params.ray_max_distance)) {
                    E_direct = v3{
                        scene.light_color.x * scene.light_intensity * ndl,
                        scene.light_color.y * scene.light_intensity * ndl,
                        scene.light_color.z * scene.light_intensity * ndl};
                }
            }

            // --- Sky irradiance at hit point (single upward shadow ray) ---
            // Binary sky visibility along +Y. Crude but correct for the dominant
            // sky direction in Sponza (open sky above); covered arches correctly
            // read zero sky because the upward ray hits the arch ceiling.
            v3 E_sky{0.0f, 0.0f, 0.0f};
            BVHRay sky_ray(offset_pos, sky_up);
            sky_ray.t_max = params.ray_max_distance;
            if (!bvh.IntersectAny(sky_ray, params.ray_max_distance)) {
                // Hemispherical sky contribution modulated by NdotUp — surfaces
                // facing up receive full sky, vertical/horizontal surfaces less.
                const float sky_ndl = std::max(0.0f, normal.y);
                E_sky = v3{
                    scene.sky_color.x * sky_ndl,
                    scene.sky_color.y * sky_ndl,
                    scene.sky_color.z * sky_ndl};
            }

            // --- Lambertian reflected radiance toward probe ---
            // L_out = (albedo / PI) * (E_direct + E_sky)
            const v3 total_E{
                E_direct.x + E_sky.x,
                E_direct.y + E_sky.y,
                E_direct.z + E_sky.z};
            L = v3{
                scene.albedo.x * inv_pi * total_E.x,
                scene.albedo.y * inv_pi * total_E.y,
                scene.albedo.z * inv_pi * total_E.z};
        } else {
            // Ray escaped the scene — probe sees sky directly
            L = scene.sky_color;
        }

        // Project L into SH9
        EvalSHBasis(dir, basis);
        for (int i = 0; i < 9; ++i) {
            radiance_sh.coeffs[i].x += L.x * basis.coeffs[i];
            radiance_sh.coeffs[i].y += L.y * basis.coeffs[i];
            radiance_sh.coeffs[i].z += L.z * basis.coeffs[i];
        }
    }

    // MC normalize: PDF = 1/(4*PI), so weight = 4*PI / N
    const float weight = 4.0f * PI / (float)ray_count;
    for (int i = 0; i < 9; ++i) {
        radiance_sh.coeffs[i].x *= weight;
        radiance_sh.coeffs[i].y *= weight;
        radiance_sh.coeffs[i].z *= weight;
    }
    // No ConvolveCosineLobe — output is radiance SH (matches runtime).

    math::v3* irradiance = volume.GetIrradianceData() + probe_index * 9;
    for (int i = 0; i < 9; ++i) {
        irradiance[i] = radiance_sh.coeffs[i];
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
    const ProbeBakingParams& params,
    BakeProgressFn on_progress,
    void* progress_user_data)
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

    auto t_bvh_start = std::chrono::steady_clock::now();
    auto bvh = std::make_shared<utl::BVH>();
    bvh->Build(verts, idx);
    auto t_bvh_end = std::chrono::steady_clock::now();
    double bvh_seconds = std::chrono::duration<double>(t_bvh_end - t_bvh_start).count();

    // Diagnostic: print BVH quality + first-vertex sample so we can spot
    // degenerate builds (e.g. from byte-stride mismatches in the caller)
    // without resorting to a debugger. A well-built BVH has max_leaf_size
    // in the single digits and max_depth ~log2(triangles).
    auto stats = bvh->GetStats();
    std::cout << "[Bake] BVH build: " << bvh_seconds << "s   nodes=" << stats.node_count
              << " leaves=" << stats.leaf_count
              << " max_leaf=" << stats.max_leaf_size
              << " max_depth=" << stats.max_depth
              << " tris=" << stats.total_prims << std::endl;
    if (scene.vertex_count >= 3) {
        std::cout << "[Bake] First 3 vertices: v0=("
                  << scene.vertices[0].x << "," << scene.vertices[0].y << "," << scene.vertices[0].z << ") v1=("
                  << scene.vertices[1].x << "," << scene.vertices[1].y << "," << scene.vertices[1].z << ") v2=("
                  << scene.vertices[2].x << "," << scene.vertices[2].y << "," << scene.vertices[2].z << ")"
                  << std::endl;
    }

    // --- Phase 1: Probe radiance SH bake (parallel per probe) ---
    // Single-bounce indirect radiance: sky + Lambertian surface bounce.
    // No direct sun leak, no multi-bounce, no ConvolveCosineLobe — outputs
    // radiance SH matching the runtime DDGIUpdateIrradiance.wgsl convention.
    //
    // Process in chunks of kProbeChunk so the optional on_progress callback
    // (used by WASM to pump the browser event loop via emscripten_sleep) gets
    // called frequently enough to keep the page responsive. Native builds
    // pass nullptr and still benefit from ParallelFor's worker pool inside
    // each chunk.
    auto bvh_ref = bvh;
    std::cout << "[Bake] Phase 1: BakeProbeRadiance over " << probe_count
              << " probes (" << params.rays_per_probe << " rays each)..." << std::flush;
    auto t1_start = std::chrono::steady_clock::now();
    constexpr u32 kProbeChunk = 32;
    for (u32 base = 0; base < probe_count; base += kProbeChunk) {
        const u32 end = (probe_count - base < kProbeChunk) ? probe_count : (base + kProbeChunk);
        auto handle1 = jobsystem::JobSystem::ParallelFor(end - base,
            [&volume, bvh_ref, &scene, &params, base](u32 i) {
                BakeProbeRadiance(volume, *bvh_ref, scene, params, base + i);
            });
        jobsystem::JobSystem::Wait(handle1);
        if (on_progress) on_progress(progress_user_data, "radiance", end, probe_count);
    }
    auto t1_end = std::chrono::steady_clock::now();
    std::cout << " done in " << std::chrono::duration<double>(t1_end - t1_start).count() << "s" << std::endl;

    // --- Phase 2: Visibility + Sky Factor (parallel per probe) ---
    // Octahedral depth + sky factor — still consumed at runtime by GIGather.
    // Skipped on WASM (params.skip_visibility) where the runtime DDGI trace
    // overwrites these buffers before they're read and the 5x ray cost would
    // make the bake take tens of minutes on single-threaded JS.
    if (!params.skip_visibility) {
        std::cout << "[Bake] Phase 2: BakeVisibility over " << probe_count
                  << " probes (64+256 rays each)..." << std::flush;
        auto t3_start = std::chrono::steady_clock::now();
        for (u32 base = 0; base < probe_count; base += kProbeChunk) {
            const u32 end = (probe_count - base < kProbeChunk) ? probe_count : (base + kProbeChunk);
            auto handle3 = jobsystem::JobSystem::ParallelFor(end - base,
                [&volume, bvh_ref, &scene, &params, base](u32 i) {
                    BakeVisibility(volume, *bvh_ref, scene, params, base + i);
                });
            jobsystem::JobSystem::Wait(handle3);
            if (on_progress) on_progress(progress_user_data, "visibility", end, probe_count);
        }
        auto t3_end = std::chrono::steady_clock::now();
        std::cout << " done in " << std::chrono::duration<double>(t3_end - t3_start).count() << "s" << std::endl;
    } else {
        std::cout << "[Bake] Phase 2: BakeVisibility SKIPPED (params.skip_visibility=true)" << std::endl;
    }

    volume.MarkLoaded();
    return true;
}

} // namespace primal::graphics::lumen
