// ScreenProbeTemporal.wgsl — Dawn port of ScreenProbeTemporal.comp (Vulkan GLSL)
//
// Vulkan Lumen Screen Probe GI — Change-penalizing temporal accumulation.
//
// Core principle: penalize change, don't reward stability.
//   - Base alpha = slow accumulation (0.1)
//   - High gradient (|current - history|) -> faster adaptation (trust current)
//   - Low confidence -> reset history (clear bad data)
//   - Disocclusion -> reset history
//   - History clamping + gradient limiter prevent energy explosion
//
// Dispatch: 1D, workgroups of (64, 1, 1) threads.
//
// === Binding layout (flat Vulkan numbering, textures + buffers share the
//     same number space — WebGPU has no Metal-style overlapping namespaces) ===
//
//   binding 0  storage, read          currentSH        (array<vec4<f32>>;
//                                                       4 vec4 per probe)
//   binding 1  storage, read          historySH        (array<vec4<f32>>;
//                                                       prev frame temporal output)
//   binding 2  storage, read          currentPositions (array<vec4<f32>>)
//   binding 3  storage, read          historyPositions (array<vec4<f32>>)
//   binding 4  storage, read_write    outputSH         (array<vec4<f32>>)
//   binding 5  uniform                TemporalConstants (96B)
//   binding 6  storage, read          currentNormals   (array<vec4<f32>>)
//   binding 7  storage, read          historyNormals   (array<vec4<f32>>)

// ============================================================================
// Struct definitions — must match the GLSL std140 block
// TemporalConstantsBlock field-for-field (96B: mat4 + 2 uint + 5 float +
// 2 uint, packed exactly as std140 lays them out).
// ============================================================================

struct TemporalConstants {
    prev_view_projection: mat4x4<f32>,  //   0: T5.2 previous frame VP for reprojection
    totalProbes:          u32,          //  64
    baseAlpha:            f32,          //  68: base blend alpha (e.g. 0.1)
    posThreshold:         f32,          //  72: world-space distance threshold for disocclusion
    normalThreshold:      f32,          //  76: normal dot-product threshold (reject if below)
    clampScale:           f32,          //  80: history clamp range relative to current
    maxGradient:          f32,          //  84: maximum SH change per frame (gradient limiter)
    gridW:                u32,          //  88: T5.2 probe grid dims for prev-cell lookup
    gridH:                u32,          //  92
};

// ============================================================================
// Bindings — see layout table at file top
// ============================================================================

@group(0) @binding(0) var<storage, read> currentSH: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read> historySH: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> currentPositions: array<vec4<f32>>;
@group(0) @binding(3) var<storage, read> historyPositions: array<vec4<f32>>;
@group(0) @binding(4) var<storage, read_write> outputSH: array<vec4<f32>>;
@group(0) @binding(5) var<uniform> params: TemporalConstants;
@group(0) @binding(6) var<storage, read> currentNormals: array<vec4<f32>>;
@group(0) @binding(7) var<storage, read> historyNormals: array<vec4<f32>>;

// ============================================================================
// Main Kernel: screen_probe_temporal
// ============================================================================

@compute @workgroup_size(64, 1, 1)
fn screen_probe_temporal(@builtin(global_invocation_id) gid_vec: vec3<u32>) {
    let global_id: u32 = gid_vec.x;

    if (global_id >= params.totalProbes) {
        return;
    }

    // If current probe is inactive, zero output
    let curPos = currentPositions[global_id];
    let base: u32 = global_id * 4u;
    if (curPos.w <= 0.0) {
        outputSH[base + 0u] = vec4<f32>(0.0);
        outputSH[base + 1u] = vec4<f32>(0.0);
        outputSH[base + 2u] = vec4<f32>(0.0);
        outputSH[base + 3u] = vec4<f32>(0.0);
        return;
    }

    // Read current SH and probe confidence (multi-factor from Average)
    let curSH0 = currentSH[base + 0u];
    let probeConfidence = curSH0.w;

    if (probeConfidence <= 0.0) {
        // No valid rays -> output zeros
        outputSH[base + 0u] = vec4<f32>(0.0);
        outputSH[base + 1u] = vec4<f32>(0.0);
        outputSH[base + 2u] = vec4<f32>(0.0);
        outputSH[base + 3u] = vec4<f32>(0.0);
        return;
    }

    // =================================================================
    // T5.2 velocity reprojection: find this probe's history in the
    // PREVIOUS frame's screen grid instead of the same cell (which is a
    // different world location once the camera moves). Neighbors pass the
    // same world-position/normal disocclusion tests the same-cell path used.
    // =================================================================
    var histSH: array<vec4<f32>, 4>;
    var histW: f32 = 0.0;
    var historyValid: bool = false;
    {
        let clipP = params.prev_view_projection * vec4<f32>(curPos.xyz, 1.0);
        if (clipP.w > 0.0) {
            let ndc = clipP.xy / clipP.w;
            let prevUV = vec2<f32>(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
            if (all(prevUV >= vec2<f32>(0.0)) && all(prevUV <= vec2<f32>(1.0))) {
                let cellF = prevUV * vec2<f32>(f32(params.gridW), f32(params.gridH)) - 0.5;
                let c0 = vec2<i32>(floor(cellF));
                let fr = fract(cellF);
                let curNorm = currentNormals[global_id];
                for (var b: i32 = 0; b < 4; b++) { histSH[b] = vec4<f32>(0.0); }

                for (var dy: i32 = 0; dy <= 1; dy++) {
                    for (var dx: i32 = 0; dx <= 1; dx++) {
                        let c = c0 + vec2<i32>(dx, dy);
                        if (c.x < 0 || c.y < 0 || c.x >= i32(params.gridW) || c.y >= i32(params.gridH)) { continue; }
                        let hIdx: u32 = u32(c.y) * params.gridW + u32(c.x);
                        let hPos = historyPositions[hIdx];
                        if (hPos.w <= 0.0) { continue; }
                        // World-space consistency: history must describe this surface.
                        if (distance(curPos.xyz, hPos.xyz) > params.posThreshold) { continue; }
                        let hNorm = historyNormals[hIdx];
                        if (hNorm.w > 0.0 && curNorm.w > 0.0 &&
                            dot(normalize(curNorm.xyz), normalize(hNorm.xyz)) < params.normalThreshold) { continue; }
                        let hb: u32 = hIdx * 4u;
                        if (historySH[hb].w <= 0.0) { continue; }  // invalid history entry
                        let w = select(fr.x, 1.0 - fr.x, dx == 0)
                              * select(fr.y, 1.0 - fr.y, dy == 0);
                        for (var b: i32 = 0; b < 4; b++) {
                            histSH[b] += historySH[hb + u32(b)] * w;
                        }
                        histW += w;
                    }
                }
                if (histW > 0.001) {
                    historyValid = true;
                    for (var b: i32 = 0; b < 4; b++) { histSH[b] /= histW; }
                }
            }
        }
    }

    // If no valid history -> use current directly (no blending)
    if (!historyValid) {
        for (var b: i32 = 0; b < 4; b++) {
            let cur = currentSH[base + u32(b)];
            outputSH[base + u32(b)] = select(vec4<f32>(cur.rgb, cur.w),
                                             vec4<f32>(cur.rgb, probeConfidence),
                                             b == 0);
        }
        return;
    }

    // =================================================================
    // Low confidence -> reset history (clear bad accumulated data)
    // =================================================================
    if (probeConfidence < 0.05) {
        for (var b: i32 = 0; b < 4; b++) {
            let cur = currentSH[base + u32(b)];
            outputSH[base + u32(b)] = select(vec4<f32>(cur.rgb, cur.w),
                                             vec4<f32>(cur.rgb, probeConfidence),
                                             b == 0);
        }
        return;
    }

    // =================================================================
    // Compute per-probe gradient (how much SH changed)
    // =================================================================
    let diff0 = curSH0.rgb - histSH[0].rgb;
    let gradient = length(diff0);

    // =================================================================
    // Change-penalizing blend:
    //   High gradient -> high alpha -> trust current (adapt to change)
    //   Low gradient -> base alpha -> slow accumulation
    // =================================================================
    let changeAlpha = clamp(gradient / params.maxGradient, 0.0, 1.0);
    let alpha = max(params.baseAlpha, changeAlpha);

    // =================================================================
    // Blend 4 SH coefficients with clamping + gradient limiter
    // =================================================================
    for (var b: i32 = 0; b < 4; b++) {
        var cur = currentSH[base + u32(b)];
        let hist = histSH[b];

        // Clamp history to prevent energy drift
        let curAbs = max(abs(cur.rgb), vec3<f32>(0.1));
        let lo = cur.rgb - params.clampScale * curAbs;
        let hi = cur.rgb + params.clampScale * curAbs;
        let clampedHist = clamp(hist.rgb, lo, hi);

        // Gradient limiter: cap per-frame change
        let diff = cur.rgb - clampedHist;
        let diffLen = length(diff);
        if (diffLen > params.maxGradient && diffLen > 0.001) {
            // WGSL: no swizzle assignment — rebuild the vec4 (rgb capped, w kept)
            cur = vec4<f32>(clampedHist + normalize(diff) * params.maxGradient, cur.w);
        }

        // Blend
        let blended = mix(clampedHist, cur.rgb, alpha);
        outputSH[base + u32(b)] = select(vec4<f32>(blended, cur.w),
                                         vec4<f32>(blended, probeConfidence),
                                         b == 0);
    }
}
