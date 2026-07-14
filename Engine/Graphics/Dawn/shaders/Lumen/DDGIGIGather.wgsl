// DDGIGIGather.wgsl — Dawn port of DDGIGIGather.metal
//
// Half-resolution compute pass that gathers DDGI indirect irradiance from
// probe storage buffers, samples the 4 nearest probes via tetrahedral
// interpolation, applies visibility weighting from octahedral depth, and
// writes the result to a half-res RGBA16F output texture.
//
// Strategy: L0+L1+L2 SH (9 coeff) tetrahedral 4-probe interpolation
//   - Soft distance falloff with visibility weight from DDGI depth
//   - Normal hemisphere weight
//   - Full L0+L1+L2 SH reconstruction
//
// Total reads: 36 storage buffer (irradiance) + 4 storage buffer (depth)
//              + 2 texture2D (depth+normal)
//
// Dispatch: (ceil(halfW/8), ceil(halfH/8), 1) workgroups of (8, 8, 1) threads.
// Workgroup size (8, 8, 1) matches TestNaniteStreamingPipeline.cpp:1182
// (pipeDesc.threadGroupSize = {8, 8, 1}).
//
// === Binding layout ===
// The Metal descriptor set uses dual namespaces: texture(0..2) and buffer(0..4).
// DawnDescriptorSetLayout detects the collision and remaps buffer engine
// bindings to higher WGPU slots (remapOffset = maxTextureBinding+1 = 3).
//
//   Engine binding   Type            WGPU binding
//   texture(0)       SampledImage    0   gbuffer depth
//   texture(1)       SampledImage    1   gbuffer normal
//   texture(2)       StorageImage    2   half-res output (rgba16float, write)
//   buffer(0)        UniformBuffer   3   invViewProj          (remapped)
//   buffer(1)        UniformBuffer   4   probeOriginSpacing   (remapped)
//   buffer(2)        UniformBuffer   5   probeCounts          (remapped)
//   buffer(3)        StorageBuffer   6   irradianceBuffer     (remapped)
//   buffer(4)        StorageBuffer   7   ddgiDepthBuffer      (remapped)
//
// No sampler binding is declared — consistent with the project convention for
// Dawn compute shaders. textureLoad is used instead of textureSampleLevel.
// Metal uses filter::nearest for depth and filter::linear for normal; the
// Dawn port uses nearest-neighbour textureLoad for both in Phase A. Manual
// bilinear interpolation for the normal read can be added in a follow-up if
// fidelity requires it.

// ============================================================================
// Constants
// ============================================================================

// SH constants (from DDGIGIGather.metal:17-21)
const _C0:   f32 = 0.282095;   // L0
const _C1:   f32 = 0.488603;   // L1
const _C2_0: f32 = 1.092548;   // L2
const _C2_1: f32 = 0.315392;
const _C2_2: f32 = 0.546274;

// ============================================================================
// Bindings — see layout table at file top
// ============================================================================

@group(0) @binding(0) var gbuffer_depth:   texture_depth_2d;
@group(0) @binding(1) var gbuffer_normal:  texture_2d<f32>;
@group(0) @binding(2) var output_tex:      texture_storage_2d<rgba16float, write>;

// Single uniform block at binding 3 — packs inv_view_proj (64B) +
// probe_origin_spacing (16B) + probe_counts (16B) = 96B. WebGPU
// requires uniform buffer *offsets* to be 256-byte aligned, so binding
// the same CB at offsets 0/64/80 across 3 slots is invalid. Merging
// into one struct at one binding sidesteps the constraint.
struct GIGatherCB {
    inv_view_proj:       mat4x4<f32>,
    probe_origin_spacing: vec4<f32>,
    probe_counts:         vec4<f32>,
};

@group(0) @binding(3) var<uniform> cb: GIGatherCB;
// Packed float[] layout: 9 coeffs/probe * 3 floats/coeff = 27 floats/probe (12-byte stride).
// array<vec3<f32>> would use 16-byte stride (WGSL host-shareable alignment) and
// misread the buffer the C++ side allocated at 12-byte stride.
@group(0) @binding(4) var<storage, read> irradiance_buffer: array<f32>;
@group(0) @binding(5) var<storage, read> ddgi_depth_buffer:  array<f32>;

// ============================================================================
// SH helpers
// ============================================================================

// L0+L1 SH dot product (4 coefficients) — stable with 64 rays.
// Metal takes `thread const float3*`; WGSL passes the array by value
// (4 vec3s = 48 bytes, fine for register pressure here).
fn shDot4(c: array<vec3<f32>, 4>, d: vec3<f32>) -> vec3<f32> {
    let x: f32 = d.x;
    let y: f32 = d.y;
    let z: f32 = d.z;
    return c[0] * _C0
         + c[1] * (-_C1 * y)
         + c[2] * ( _C1 * z)
         + c[3] * (-_C1 * x);
}

// L0+L1+L2 SH dot product (9 coefficients).
// (Defined for parity with the Metal source; the kernel below only uses shDot4.)
fn shDot9(c: array<vec3<f32>, 9>, d: vec3<f32>) -> vec3<f32> {
    let x: f32 = d.x;
    let y: f32 = d.y;
    let z: f32 = d.z;
    let x2: f32 = x * x;
    let y2: f32 = y * y;
    let z2: f32 = z * z;
    return c[0] * _C0
         + c[1] * (-_C1 * y)
         + c[2] * ( _C1 * z)
         + c[3] * (-_C1 * x)
         + c[4] * ( _C2_0 * y * x)
         + c[5] * (-_C2_0 * y * z)
         + c[6] * ( _C2_1 * (3.0 * z2 - 1.0))
         + c[7] * (-_C2_0 * x * z)
         + c[8] * ( _C2_2 * (x2 - y2));
}

// ============================================================================
// Tetrahedral 4-probe interpolation
// ============================================================================

struct TetraResult {
    pi: array<u32, 4>,
    bw: array<f32, 4>,
};

// Port of DDGIGIGather.metal:47-64. Metal uses out-params; WGSL returns a struct.
// Note: gp is float3, gd is uint3 — mixed-type arithmetic is done with explicit
// f32()/u32() casts.
fn tetrahedral(gp: vec3<f32>, gd: vec3<u32>) -> TetraResult {
    var r: TetraResult;

    let b0: vec3<u32> = clamp(vec3<u32>(floor(gp)), vec3<u32>(0u), gd - 1u);
    let b1: vec3<u32> = min(b0 + 1u, gd - 1u);

    let gx: u32 = gd.x;
    let gxy: u32 = gd.x * gd.y;

    var p: array<u32, 8>;
    p[0] = b0.x + b0.y * gx + b0.z * gxy;
    p[1] = b1.x + b0.y * gx + b0.z * gxy;
    p[2] = b0.x + b1.y * gx + b0.z * gxy;
    p[3] = b1.x + b1.y * gx + b0.z * gxy;
    p[4] = b0.x + b0.y * gx + b1.z * gxy;
    p[5] = b1.x + b0.y * gx + b1.z * gxy;
    p[6] = b0.x + b1.y * gx + b1.z * gxy;
    p[7] = b1.x + b1.y * gx + b1.z * gxy;

    let fx: f32 = fract(gp.x);
    let fy: f32 = fract(gp.y);
    let fz: f32 = fract(gp.z);

    if (fx >= fy && fy >= fz) {
        r.pi[0] = p[0]; r.pi[1] = p[1]; r.pi[2] = p[3]; r.pi[3] = p[7];
        r.bw[0] = 1.0 - fx; r.bw[1] = fx - fy; r.bw[2] = fy - fz; r.bw[3] = fz;
    } else if (fx >= fz && fz >= fy) {
        r.pi[0] = p[0]; r.pi[1] = p[1]; r.pi[2] = p[5]; r.pi[3] = p[7];
        r.bw[0] = 1.0 - fx; r.bw[1] = fx - fz; r.bw[2] = fz - fy; r.bw[3] = fy;
    } else if (fy >= fx && fx >= fz) {
        r.pi[0] = p[0]; r.pi[1] = p[2]; r.pi[2] = p[3]; r.pi[3] = p[7];
        r.bw[0] = 1.0 - fy; r.bw[1] = fy - fx; r.bw[2] = fx - fz; r.bw[3] = fz;
    } else if (fy >= fz && fz >= fx) {
        r.pi[0] = p[0]; r.pi[1] = p[2]; r.pi[2] = p[6]; r.pi[3] = p[7];
        r.bw[0] = 1.0 - fy; r.bw[1] = fy - fz; r.bw[2] = fz - fx; r.bw[3] = fx;
    } else if (fz >= fx && fx >= fy) {
        r.pi[0] = p[0]; r.pi[1] = p[4]; r.pi[2] = p[5]; r.pi[3] = p[7];
        r.bw[0] = 1.0 - fz; r.bw[1] = fz - fx; r.bw[2] = fx - fy; r.bw[3] = fy;
    } else {
        r.pi[0] = p[0]; r.pi[1] = p[4]; r.pi[2] = p[6]; r.pi[3] = p[7];
        r.bw[0] = 1.0 - fz; r.bw[1] = fz - fy; r.bw[2] = fy - fx; r.bw[3] = fx;
    }

    // Clamp negative weights (equivalent to Metal's max(bw[i], 0.0f))
    for (var i: u32 = 0u; i < 4u; i++) {
        r.bw[i] = max(r.bw[i], 0.0);
    }

    return r;
}

// ============================================================================
// Visibility / depth helpers
// ============================================================================

// Port of DDGIGIGather.metal:66-74.
fn visibilityWeight(dist: f32, mean: f32, variance: f32, spacing: f32) -> f32 {
    if (dist <= mean) {
        return 1.0;
    }
    let d_minus_mean: f32 = dist - mean;
    let chebyshev: f32 = variance / (variance + d_minus_mean * d_minus_mean);
    let bias: f32 = spacing * 0.1;
    let threshold: f32 = mean + bias + chebyshev * spacing;
    let falloff: f32 = spacing * 1.0;
    return clamp((threshold - dist + falloff) / falloff, 0.0, 1.0);
}

// Port of DDGIGIGather.metal:76-83.
// select() note: Metal select(a, b, c) returns (c ? b : a).
// WGSL select(f, t, c) returns (c ? t : f). Same argument order means
// the same syntax yields the same result.
fn octahedralEncode(d: vec3<f32>) -> vec2<f32> {
    let l1norm: f32 = abs(d.x) + abs(d.y) + abs(d.z);
    var uv: vec2<f32> = d.xy / l1norm;
    if (d.z < 0.0) {
        let s: vec2<f32> = select(vec2<f32>(-1.0), vec2<f32>(1.0), uv.xy >= vec2<f32>(0.0));
        uv = (1.0 - abs(uv.yx)) * s;
    }
    return uv * 0.5 + 0.5;
}

// Port of DDGIGIGather.metal:85-94.
// Reads mean + variance from the per-probe octahedral depth tile.
fn sampleDepthOctahedral(probeIdx: u32, dir: vec3<f32>) -> vec2<f32> {
    let res: u32 = 8u;
    let texels: u32 = res * res;
    let uv: vec2<f32> = octahedralEncode(dir);
    let texel: vec2<u32> = vec2<u32>(
        clamp(u32(uv.x * f32(res)), 0u, res - 1u),
        clamp(u32(uv.y * f32(res)), 0u, res - 1u),
    );
    let idx: u32 = texel.y * res + texel.x;
    let probeBase: u32 = probeIdx * texels * 2u;
    return vec2<f32>(
        ddgi_depth_buffer[probeBase + idx],
        ddgi_depth_buffer[probeBase + texels + idx],
    );
}

// Port of DDGIGIGather.metal:96-102.
fn probeGridCoord(probeIdx: u32, counts: vec3<u32>) -> vec3<u32> {
    let pz: u32 = probeIdx / (counts.x * counts.y);
    let rem: u32 = probeIdx % (counts.x * counts.y);
    let py: u32 = rem / counts.x;
    let px: u32 = rem % counts.x;
    return vec3<u32>(px, py, pz);
}

// ============================================================================
// Main Kernel: ddgi_gi_gather
// ============================================================================

@compute @workgroup_size(8, 8, 1)
fn ddgi_gi_gather(@builtin(global_invocation_id) gid_vec: vec3<u32>) {
    let tid: vec2<u32> = gid_vec.xy;

    // Output texture is the authoritative size for this pass — the dispatch
    // covers exactly (halfW, halfH). Early-out if we land outside.
    let outDims: vec2<u32> = textureDimensions(output_tex);
    if (any(tid >= outDims)) {
        return;
    }

    let uv: vec2<f32> = (vec2<f32>(tid) + 0.5) / vec2<f32>(outDims);

    // --- Sample depth (nearest via textureLoad) ---
    let depthDims: vec2<u32> = textureDimensions(gbuffer_depth);
    let dtx: i32 = clamp(i32(uv.x * f32(depthDims.x)), 0, i32(depthDims.x) - 1);
    let dty: i32 = clamp(i32(uv.y * f32(depthDims.y)), 0, i32(depthDims.y) - 1);
    let depth: f32 = textureLoad(gbuffer_depth, vec2<i32>(dtx, dty), 0);

    // Sky pixel: write zero and bail
    if (depth >= 1.0) {
        textureStore(output_tex, vec2<i32>(tid), vec4<f32>(0.0));
        return;
    }

    // --- Sample normal (nearest via textureLoad; Phase A quality regression) ---
    let normalDims: vec2<u32> = textureDimensions(gbuffer_normal);
    let ntx: i32 = clamp(i32(uv.x * f32(normalDims.x)), 0, i32(normalDims.x) - 1);
    let nty: i32 = clamp(i32(uv.y * f32(normalDims.y)), 0, i32(normalDims.y) - 1);
    let sampledNormal: vec4<f32> = textureLoad(gbuffer_normal, vec2<i32>(ntx, nty), 0);
    let normal: vec3<f32> = normalize(sampledNormal.xyz * 2.0 - 1.0);

    // --- Reconstruct world position ---
    let ndc: vec2<f32> = vec2<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    let wp: vec4<f32> = cb.inv_view_proj * vec4<f32>(ndc, depth, 1.0);
    let worldPos: vec3<f32> = wp.xyz / wp.w;

    // --- Probe grid params ---
    let origin:  vec3<f32> = cb.probe_origin_spacing.xyz;
    let spacing: f32       = cb.probe_origin_spacing.w;
    let counts:  vec3<u32> = vec3<u32>(cb.probe_counts.xyz);

    let gp: vec3<f32> = (worldPos - origin) / spacing;
    let gridMax: vec3<f32> = vec3<f32>(
        f32(counts.x - 1u),
        f32(counts.y - 1u),
        f32(counts.z - 1u),
    );

    // Outside the probe grid: write zero and bail
    if (any(gp < vec3<f32>(0.0)) || any(gp > gridMax)) {
        textureStore(output_tex, vec2<i32>(tid), vec4<f32>(0.0));
        return;
    }

    // --- Tetrahedral 4 probes ---
    let tet: TetraResult = tetrahedral(gp, counts);

    var result: vec3<f32> = vec3<f32>(0.0);
    var totalWeight: f32 = 0.0;

    // Normal bias to prevent self-shadowing acne (small to avoid thin wall penetration)
    let biasedPos: vec3<f32> = worldPos + normal * spacing * 0.05;

    // Track best fallback probe (highest barycentric weight) for guaranteed output
    var bestIrradiance: vec3<f32> = vec3<f32>(0.0);
    var bestWeight: f32 = 0.0;

    for (var p: u32 = 0u; p < 4u; p++) {
        if (tet.bw[p] < 0.001) {
            continue;
        }

        let gc: vec3<u32> = probeGridCoord(tet.pi[p], counts);
        let probePos: vec3<f32> = origin +
            vec3<f32>(f32(gc.x), f32(gc.y), f32(gc.z)) * spacing;

        let toProbe: vec3<f32> = normalize(probePos - biasedPos);
        let ndotd: f32 = dot(normal, toProbe);
        let normalWeight: f32 = clamp((ndotd + 0.5) / 0.7, 0.0, 1.0);

        let fromProbeDir: vec3<f32> = normalize(biasedPos - probePos);
        let distToProbe: f32 = length(biasedPos - probePos);

        // Read irradiance L0+L1 (packed float[3] per coeff).
        let base: u32 = tet.pi[p] * 27u;  // 9 coeffs * 3 floats per coeff
        var sh: array<vec3<f32>, 4>;
        for (var i: u32 = 0u; i < 4u; i++) {
            let cb_i: u32 = base + i * 3u;
            sh[i] = vec3<f32>(
                irradiance_buffer[cb_i + 0u],
                irradiance_buffer[cb_i + 1u],
                irradiance_buffer[cb_i + 2u]);
        }
        let irradiance: vec3<f32> = shDot4(sh, normal);

        // Track best probe for guaranteed fallback
        if (tet.bw[p] > bestWeight) {
            bestWeight = tet.bw[p];
            bestIrradiance = irradiance;
        }

        // Octahedral depth sampling
        let depthMV: vec2<f32> = sampleDepthOctahedral(tet.pi[p], fromProbeDir);

        // Soft inside-geometry penalty instead of hard skip:
        // probes with very short mean depth get reduced weight, not zero
        var insidePenalty: f32 = 1.0;
        if (depthMV.x < spacing * 0.2) {
            insidePenalty = smoothstep(0.0, 0.2, depthMV.x / spacing);
        }

        let visWeight: f32 = visibilityWeight(distToProbe, depthMV.x, depthMV.y, spacing);

        let w: f32 = tet.bw[p] * normalWeight * visWeight * insidePenalty;
        result = result + irradiance * w;
        totalWeight = totalWeight + w;
    }

    if (totalWeight > 0.0) {
        result = result / totalWeight;
    } else {
        // Fallback: use best probe at full strength. The previous 0.3 multiplier
        // suppressed indirect light so heavily that DDGI was invisible.
        result = bestIrradiance;
    }
    // Sanitize: probe storage can contain NaN/Inf if the static bake or runtime
    // update wrote bad values; one NaN texel would propagate through deferred
    // lighting + TAA into a screen-filling solid colour.
    let anyNaN: bool = (result.x != result.x) || (result.y != result.y) || (result.z != result.z);
    let anyInf: bool = any(abs(result) > vec3<f32>(3.4e38));
    if (anyNaN || anyInf) {
        result = vec3<f32>(0.0);
    }
    result = max(result, vec3<f32>(0.0));
    // Magnitude clamp: SH reconstruction can produce values larger than any
    // single L0 coefficient when L1 contributions align with the surface normal.
    // Plausible indirect is ~1.0; anything above 1.5 indicates either a hot
    // probe (bake captured direct sun leak) or L1 ringing. ACES tonemap with
    // exposure 1.8 in DeferredLighting saturates above ~3, so capping at 1.5
    // keeps indirect visible without driving regions to flat colour.
    result = min(result, vec3<f32>(1.5));

    textureStore(output_tex, vec2<i32>(tid), vec4<f32>(result, 1.0));
}
