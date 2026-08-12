// SSGITemporal.wgsl — Temporal accumulation with variance clipping
// Full-resolution compute shader that:
//   1. Reads spatially filtered SSGI at full resolution
//   2. Reprojects using velocity buffer to find previous-frame UV
//   3. Samples history buffer with manual bilinear interpolation
//   4. Clamps history via variance clipping (mean + 2σ on 3x3 neighborhood)
//   5. Detects disocclusion via depth comparison
//   6. Exponential blend: result = lerp(current, clamped_history, feedback)

struct SSGITemporalParams {
    feedback: f32,
    fullWidth: u32,
    fullHeight: u32,
    _pad: u32,
};

@group(0) @binding(0) var ssgiSpatial: texture_2d<f32>;      // full-res spatial filter output
@group(0) @binding(1) var ssgiHistory: texture_2d<f32>;      // previous frame temporal output
@group(0) @binding(2) var velocityTex: texture_2d<f32>;      // motion vectors
@group(0) @binding(3) var depthTex: texture_depth_2d;        // current depth
@group(0) @binding(4) var ssgiOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var<uniform> params: SSGITemporalParams;

// Manual bilinear interpolation for history sampling
fn sampleHistoryBilinear(tex: texture_2d<f32>, uv: vec2f, dims: vec2u) -> vec4f {
    let coord = uv * vec2f(dims) - 0.5;
    let base = vec2i(i32(floor(coord.x)), i32(floor(coord.y)));
    let frac_ = fract(coord);
    let maxCoord = vec2i(i32(dims.x) - 1, i32(dims.y) - 1);

    let c00 = clamp(base, vec2i(0), maxCoord);
    let c10 = clamp(base + vec2i(1, 0), vec2i(0), maxCoord);
    let c01 = clamp(base + vec2i(0, 1), vec2i(0), maxCoord);
    let c11 = clamp(base + vec2i(1, 1), vec2i(0), maxCoord);

    let h00 = textureLoad(tex, vec2u(c00), 0);
    let h10 = textureLoad(tex, vec2u(c10), 0);
    let h01 = textureLoad(tex, vec2u(c01), 0);
    let h11 = textureLoad(tex, vec2u(c11), 0);

    let fx = clamp(frac_.x, 0.0, 1.0);
    let fy = clamp(frac_.y, 0.0, 1.0);

    return h00 * (1.0 - fx) * (1.0 - fy)
         + h10 * fx * (1.0 - fy)
         + h01 * (1.0 - fx) * fy
         + h11 * fx * fy;
}

@compute @workgroup_size(8, 8, 1)
fn ssgi_temporal(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= params.fullWidth || gid.y >= params.fullHeight) { return; }

    let pixelPos = gid.xy;
    let pixelUV = (vec2f(pixelPos) + 0.5) / vec2f(f32(params.fullWidth), f32(params.fullHeight));

    let currentSSGI = textureLoad(ssgiSpatial, pixelPos, 0);
    let currentIrr = currentSSGI.rgb;
    let currentHitDist = currentSSGI.a;
    let depthNDC = textureLoad(depthTex, pixelPos, 0);

    // Velocity reprojection
    let velocity = textureLoad(velocityTex, pixelPos, 0).rg;
    let prevUV = pixelUV - velocity;

    // Check bounds
    let edgeDist = min(prevUV, 1.0 - prevUV);
    let inBounds = prevUV.x >= 0.0 && prevUV.x <= 1.0 &&
                   prevUV.y >= 0.0 && prevUV.y <= 1.0;

    if (!inBounds) {
        textureStore(ssgiOutput, pixelPos, vec4f(currentIrr, currentHitDist));
        return;
    }

    // Sample history with bilinear interpolation
    let histDims = vec2u(textureDimensions(ssgiHistory, 0));
    let histBilinear = sampleHistoryBilinear(ssgiHistory, prevUV, histDims);
    let historyIrr = histBilinear.rgb;
    let historyHitDist = histBilinear.a;

    // Variance clipping: 3x3 neighborhood of current spatial SSGI
    var nbSum = currentIrr;
    var nbSum2 = currentIrr * currentIrr;
    var nbCount: i32 = 1;

    for (var dy: i32 = -1; dy <= 1; dy++) {
        for (var dx: i32 = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) { continue; }
            let nPos = vec2i(i32(pixelPos.x) + dx, i32(pixelPos.y) + dy);
            if (nPos.x < 0 || nPos.y < 0 ||
                nPos.x >= i32(params.fullWidth) || nPos.y >= i32(params.fullHeight)) {
                continue;
            }
            let nb = textureLoad(ssgiSpatial, vec2u(nPos), 0).rgb;
            nbSum += nb;
            nbSum2 += nb * nb;
            nbCount++;
        }
    }

    let fCount = f32(nbCount);
    let mean = nbSum / fCount;
    let variance = abs(nbSum2 / fCount - mean * mean);
    let sigma = sqrt(max(variance, vec3f(0.0)));
    let sigmaFloor = max(mean * 0.1, vec3f(0.01));
    let aabbMin = mean - sigma * 2.0 - sigmaFloor;
    let aabbMax = mean + sigma * 2.0 + sigmaFloor;

    let clampedHistory = clamp(historyIrr, aabbMin, aabbMax);

    // Disocclusion detection
    var disocclusionFade: f32 = 1.0;
    {
        let histPixel = clamp(
            vec2i(i32(prevUV.x * f32(params.fullWidth)),
                  i32(prevUV.y * f32(params.fullHeight))),
            vec2i(0),
            vec2i(i32(params.fullWidth) - 1, i32(params.fullHeight) - 1)
        );
        let histDepth = textureLoad(depthTex, vec2u(histPixel), 0);
        let depthDiff = abs(depthNDC - histDepth);
        disocclusionFade = clamp((0.02 - depthDiff) / 0.015, 0.0, 1.0);
    }

    // Screen-edge fade
    let edgeFade = smoothstep(0.0, 0.05, min(edgeDist.x, edgeDist.y));

    // Variance clip already rejects stale history; confidenceScale used to
    // lower feedback for far-hit pixels, but with radius=15 every hit lands
    // well past the hitDist/2.0 threshold, dropping effective feedback to
    // ~0.54 — too short a half-life to smooth per-frame stripe variation,
    // which showed up as flickering. Trust the variance clip instead.
    let effectiveFeedback = params.feedback * edgeFade * disocclusionFade;

    let resultColor = mix(currentIrr, clampedHistory, effectiveFeedback);
    let resultDist = mix(currentHitDist, historyHitDist, effectiveFeedback);

    textureStore(ssgiOutput, pixelPos, vec4f(resultColor, resultDist));
}
