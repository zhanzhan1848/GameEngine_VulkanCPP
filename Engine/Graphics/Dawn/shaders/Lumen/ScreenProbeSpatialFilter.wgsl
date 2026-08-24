// ScreenProbeSpatialFilter.wgsl — Dawn port of ScreenProbeSpatialFilter.comp
// (Vulkan GLSL)
//
// Vulkan Lumen Screen Probe GI — Bilateral spatial filter on probe SH
// coefficients.
//
// Cross-pattern 5-probe bilateral blur: center + 4 cardinal neighbors.
// Smooths probe grid boundaries while preserving edges at depth discontinuities.
//
// Dispatch: 1D, workgroups of (64, 1, 1) threads.
//
// === Binding layout (flat Vulkan numbering, textures + buffers share the
//     same number space — WebGPU has no Metal-style overlapping namespaces) ===
//
//   binding 0  storage, read          inputSH   (array<vec4<f32>>;
//                                                 4 vec4 per probe, interleaved)
//   binding 1  storage, read_write    outputSH  (array<vec4<f32>>;
//                                                 4 vec4 per probe, interleaved)
//   binding 2  storage, read          probePos  (array<vec4<f32>>;
//                                                 bilateral weighting)
//   binding 3  uniform                SpatialConstants (16B)

// ============================================================================
// Struct definitions — must match the GLSL std140 block
// SpatialConstantsBlock field-for-field (16B: 2 uint + float + pad).
// ============================================================================

struct SpatialConstants {
    totalProbes: u32,   //  0
    gridW:       u32,   //  4
    sigma:       f32,   //  8: bilateral spatial sigma (world-space)
    pad:         u32,   // 12
};

// ============================================================================
// Bindings — see layout table at file top
// ============================================================================

@group(0) @binding(0) var<storage, read> inputSH: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read_write> outputSH: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> probePos: array<vec4<f32>>;
@group(0) @binding(3) var<uniform> params: SpatialConstants;

// ============================================================================
// Main Kernel: screen_probe_spatial_filter
// ============================================================================

@compute @workgroup_size(64, 1, 1)
fn screen_probe_spatial_filter(@builtin(global_invocation_id) gid_vec: vec3<u32>) {
    let global_id: u32 = gid_vec.x;

    if (global_id >= params.totalProbes) {
        return;
    }

    let gx: u32 = global_id % params.gridW;
    let gy: u32 = global_id / params.gridW;

    // Read center probe position
    let centerPosData = probePos[global_id];

    // If center probe is inactive, output zeros
    let base: u32 = global_id * 4u;
    if (centerPosData.w <= 0.0) {
        outputSH[base + 0u] = vec4<f32>(0.0);
        outputSH[base + 1u] = vec4<f32>(0.0);
        outputSH[base + 2u] = vec4<f32>(0.0);
        outputSH[base + 3u] = vec4<f32>(0.0);
        return;
    }

    // Read center SH
    let centerBase: u32 = global_id * 4u;
    var centerSH: array<vec3<f32>, 4>;
    for (var b: i32 = 0; b < 4; b++) {
        centerSH[b] = inputSH[centerBase + u32(b)].rgb;
    }

    // Check if center SH is valid
    if (inputSH[centerBase + 0u].w <= 0.0) {
        // Invalid center: output zeros
        outputSH[centerBase + 0u] = vec4<f32>(0.0);
        outputSH[centerBase + 1u] = vec4<f32>(0.0);
        outputSH[centerBase + 2u] = vec4<f32>(0.0);
        outputSH[centerBase + 3u] = vec4<f32>(0.0);
        return;
    }

    let centerPos = centerPosData.xyz;
    let invTwoSigmaSq = 1.0 / (2.0 * params.sigma * params.sigma);

    // Cardinal neighbor offsets (cross pattern)
    var offsets: array<vec2<i32>, 4>;
    offsets[0] = vec2<i32>(-1,  0);  // left
    offsets[1] = vec2<i32>( 1,  0);  // right
    offsets[2] = vec2<i32>( 0, -1);  // up
    offsets[3] = vec2<i32>( 0,  1);  // down

    // Accumulate filtered SH + confidence.
    // Center-weighted: center weight = 2.0 (strong self-preservation).
    var filteredSH: array<vec3<f32>, 4> = array<vec3<f32>, 4>(
        centerSH[0] * 2.0,
        centerSH[1] * 2.0,
        centerSH[2] * 2.0,
        centerSH[3] * 2.0
    );
    var totalWeight: f32 = 2.0;  // center weight = 2.0
    let centerConf = inputSH[centerBase + 0u].w;
    var filteredConf: f32 = centerConf * 2.0;  // center weight = 2.0

    for (var n: i32 = 0; n < 4; n++) {
        let nx = i32(gx) + offsets[n].x;
        let ny = i32(gy) + offsets[n].y;

        // Bounds check
        if (nx < 0 || nx >= i32(params.gridW) || ny < 0) { continue; }
        if (u32(ny) * params.gridW + u32(nx) >= params.totalProbes) { continue; }

        let nidx: u32 = u32(ny) * params.gridW + u32(nx);

        // Check neighbor active
        let neighborPosData = probePos[nidx];
        if (neighborPosData.w <= 0.0) { continue; }

        // Check neighbor SH valid
        let neighborBase: u32 = nidx * 4u;
        if (inputSH[neighborBase + 0u].w <= 0.0) { continue; }

        // Bilateral weight based on world-space distance
        let delta = centerPos - neighborPosData.xyz;
        let dist2 = dot(delta, delta);
        let w = exp(-dist2 * invTwoSigmaSq);

        // Read and accumulate neighbor SH
        for (var b: i32 = 0; b < 4; b++) {
            filteredSH[b] += inputSH[neighborBase + u32(b)].rgb * w;
        }
        // Accumulate confidence from neighbor
        filteredConf += inputSH[neighborBase + 0u].w * w;
        totalWeight += w;
    }

    // Normalize and write output
    let invWeight = 1.0 / totalWeight;
    let outBase: u32 = global_id * 4u;
    outputSH[outBase + 0u] = vec4<f32>(filteredSH[0] * invWeight, filteredConf * invWeight);
    outputSH[outBase + 1u] = vec4<f32>(filteredSH[1] * invWeight, 1.0);
    outputSH[outBase + 2u] = vec4<f32>(filteredSH[2] * invWeight, 1.0);
    outputSH[outBase + 3u] = vec4<f32>(filteredSH[3] * invWeight, 1.0);
}
