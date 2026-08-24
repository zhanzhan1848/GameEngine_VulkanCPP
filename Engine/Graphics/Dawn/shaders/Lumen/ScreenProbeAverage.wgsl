// ScreenProbeAverage.wgsl — Dawn port of ScreenProbeAverage.comp (Vulkan GLSL)
//
// Vulkan Lumen Screen Probe GI — SH2 (L0+L1) accumulation with multi-factor
// confidence.
//
// Each threadgroup handles one probe. Each thread reads 1 ray, recomputes its
// ray direction (Fibonacci + orientAroundNormal), evaluates 4 SH basis
// functions, and accumulates into shared memory via parallel reduction.
//
//   Confidence = hitRatio * radianceConsistency
//     hitRatio            = fraction of rays that hit geometry
//     radianceConsistency = exp(-variance_of_luminance)
//
// Output: 4 vec4 per probe (interleaved [sh0, sh1, sh2, sh3]).
//   sh[0].w = confidence,  sh[1..3].w = 1.0 (validity flag)
//
// REQUIREMENT: workgroup size must equal raysPerProbe (64).
// Dispatch: totalProbes, 1, 1  (one workgroup per probe).
//
// === Binding layout (flat Vulkan numbering, textures + buffers share the
//     same number space — WebGPU has no Metal-style overlapping namespaces) ===
//
//   binding 0  storage, read          rayRadiance   (array<vec4<f32>>; per-ray:
//                                                     xyz=radiance, w=cosTheta,
//                                                     neg=miss)
//   binding 1  storage, read_write    probeSHOut    (array<vec4<f32>>;
//                                                     4 vec4 per probe)
//   binding 2  uniform                 AvgConstants (16B)
//   binding 3  storage, read          probeNormals  (array<vec4<f32>>)

// ============================================================================
// Struct definitions — must match the GLSL std140 block AvgConstantsBlock
// field-for-field (16B: two uints + two explicit pads).
// ============================================================================

struct AvgConstants {
    totalProbes:  u32,   //   0
    raysPerProbe: u32,   //   4
    _pad0:        u32,   //   8
    _pad1:        u32,   //  12
};

// ============================================================================
// Bindings — see layout table at file top
// ============================================================================

@group(0) @binding(0) var<storage, read> rayRadiance: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> probeSHOut: array<vec4<f32>>;
@group(0) @binding(2) var<uniform> constants: AvgConstants;
@group(0) @binding(3) var<storage, read> probeNormals: array<vec4<f32>>;

// ============================================================================
// Constants
// ============================================================================

const PI_VAL: f32       = 3.14159265358979323846;
const GOLDEN_RATIO: f32 = 1.618033988749895;
const AVG_TG_SIZE: u32  = 64u;

// ============================================================================
// Fibonacci sphere direction + normal alignment (must match TraceRays)
// ============================================================================

fn fibonacciDirection(rayIndex: u32, totalRays: u32) -> vec3<f32> {
    let theta = 2.0 * PI_VAL * f32(rayIndex) / GOLDEN_RATIO;
    let phi = acos(1.0 - 2.0 * (f32(rayIndex) + 0.5) / f32(totalRays));
    return vec3<f32>(sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi));
}

fn orientAroundNormal(dir: vec3<f32>, normal: vec3<f32>) -> vec3<f32> {
    let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0),
                    abs(normal.y) < 0.999);
    let tangent = normalize(cross(up, normal));
    let bitangent = cross(normal, tangent);
    return normalize(tangent * dir.x + bitangent * dir.y + normal * dir.z);
}

// SH basis (L0 + L1, 4 coefficients)
fn shBasis(d: vec3<f32>) -> vec4<f32> {
    return vec4<f32>(
        0.282095,                // Y0 (L0)
        0.488603 * d.y,          // Y1 (L1, y)
        0.488603 * d.z,          // Y2 (L1, z)
        0.488603 * d.x           // Y3 (L1, x)
    );
}

// ============================================================================
// Workgroup (shared) memory for parallel reduction
// ============================================================================

var<workgroup> sharedSH: array<array<vec3<f32>, AVG_TG_SIZE>, 4>;  // 4 SH bands
var<workgroup> sharedWeight: array<f32, AVG_TG_SIZE>;  // cosTheta sum
var<workgroup> sharedLum: array<f32, AVG_TG_SIZE>;     // luminance sum for variance
var<workgroup> sharedLumSq: array<f32, AVG_TG_SIZE>;   // luminance^2 sum for variance
var<workgroup> sharedHit: array<f32, AVG_TG_SIZE>;     // 1.0 if hit, 0.0 if miss

// ============================================================================
// Main Kernel: screen_probe_average
// ============================================================================

@compute @workgroup_size(64, 1, 1)
fn screen_probe_average(@builtin(local_invocation_id) lid_vec: vec3<u32>,
                        @builtin(workgroup_id) wgid_vec: vec3<u32>) {
    let tid: u32 = lid_vec.x;
    let group_id: u32 = wgid_vec.x;

    if (group_id >= constants.totalProbes) {
        return;
    }

    // Each thread reads exactly 1 ray
    let rayIdx: u32 = group_id * constants.raysPerProbe + tid;
    var rayData = rayRadiance[rayIdx];

    if (rayData.w >= 0.0) {
        // Hit: clamp radiance at source to prevent firefly propagation
        const MAX_RAY_RADIANCE: f32 = 5.0;
        let rayLum = dot(rayData.rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
        if (rayLum > MAX_RAY_RADIANCE) {
            // WGSL: no swizzle assignment — rebuild the vec4 (rgb scaled, w kept)
            rayData = vec4<f32>(rayData.rgb * (MAX_RAY_RADIANCE / rayLum), rayData.w);
        }

        // Hit: compute SH basis and accumulate
        let normData = probeNormals[group_id];
        let probeNormal = normalize(normData.xyz);

        let localDir = fibonacciDirection(tid, constants.raysPerProbe);
        let rayDir = orientAroundNormal(localDir, probeNormal);

        let Y = shBasis(rayDir);

        let weightedRadiance = rayData.rgb * rayData.w;
        sharedSH[0][tid] = weightedRadiance * Y.x;
        sharedSH[1][tid] = weightedRadiance * Y.y;
        sharedSH[2][tid] = weightedRadiance * Y.z;
        sharedSH[3][tid] = weightedRadiance * Y.w;
        sharedWeight[tid] = rayData.w;

        // Luminance for variance computation
        let lum = dot(rayData.rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
        sharedLum[tid] = lum;
        sharedLumSq[tid] = lum * lum;
        sharedHit[tid] = 1.0;
    } else {
        // Miss: contribute nothing
        sharedSH[0][tid] = vec3<f32>(0.0);
        sharedSH[1][tid] = vec3<f32>(0.0);
        sharedSH[2][tid] = vec3<f32>(0.0);
        sharedSH[3][tid] = vec3<f32>(0.0);
        sharedWeight[tid] = 0.0;
        sharedLum[tid] = 0.0;
        sharedLumSq[tid] = 0.0;
        sharedHit[tid] = 0.0;
    }

    workgroupBarrier();

    // Parallel reduction (log2(64) = 6 steps)
    for (var stride: u32 = AVG_TG_SIZE >> 1u; stride > 0u; stride >>= 1u) {
        if (tid < stride) {
            sharedSH[0][tid] += sharedSH[0][tid + stride];
            sharedSH[1][tid] += sharedSH[1][tid + stride];
            sharedSH[2][tid] += sharedSH[2][tid + stride];
            sharedSH[3][tid] += sharedSH[3][tid + stride];
            sharedWeight[tid] += sharedWeight[tid + stride];
            sharedLum[tid]    += sharedLum[tid + stride];
            sharedLumSq[tid]  += sharedLumSq[tid + stride];
            sharedHit[tid]    += sharedHit[tid + stride];
        }
        workgroupBarrier();
    }

    // Thread 0 writes the final SH coefficients + multi-factor confidence
    if (tid == 0u) {
        let base: u32 = group_id * 4u;
        let hitCount = sharedHit[0];
        let totalWeight = sharedWeight[0];

        if (totalWeight > 0.0 && hitCount > 0.0) {
            let invWeight = 1.0 / totalWeight;

            // Hit ratio: how many rays hit geometry
            let hitRatio = hitCount / f32(constants.raysPerProbe);

            // Radiance variance: Var(X) = E[X^2] - E[X]^2
            let avgLum = sharedLum[0] / hitCount;
            let avgLumSq = sharedLumSq[0] / hitCount;
            let lumVariance = max(avgLumSq - avgLum * avgLum, 0.0);

            // Radiance consistency: low variance -> high consistency
            // Scale factor: variance of 1.0 -> consistency ~ 0.02
            let radianceConsistency = exp(-lumVariance * 4.0);

            // Multi-factor confidence
            let confidence = hitRatio * radianceConsistency;

            probeSHOut[base + 0u] = vec4<f32>(sharedSH[0][0] * invWeight, confidence);
            probeSHOut[base + 1u] = vec4<f32>(sharedSH[1][0] * invWeight, 1.0);
            probeSHOut[base + 2u] = vec4<f32>(sharedSH[2][0] * invWeight, 1.0);
            probeSHOut[base + 3u] = vec4<f32>(sharedSH[3][0] * invWeight, 1.0);
        } else {
            probeSHOut[base + 0u] = vec4<f32>(0.0, 0.0, 0.0, 0.0);
            probeSHOut[base + 1u] = vec4<f32>(0.0, 0.0, 0.0, 0.0);
            probeSHOut[base + 2u] = vec4<f32>(0.0, 0.0, 0.0, 0.0);
            probeSHOut[base + 3u] = vec4<f32>(0.0, 0.0, 0.0, 0.0);
        }
    }
}
