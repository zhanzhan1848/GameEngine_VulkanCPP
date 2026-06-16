// SSRTemporal.wgsl — Half-resolution temporal accumulation for SSR.
//   1. Reads half-res SSR trace output (RGBA16F: RGB=reflection, A=hit mask)
//   2. Reprojects using full-res velocity (bilinear sampled at half-res pixel)
//   3. Samples history buffer with manual bilinear interpolation
//   4. YCoCg variance clip on 3x3 neighborhood
//   5. Exponential blend: result = lerp(current, clamped_history, feedback)
//   6. Outputs RGBA16F: RGB = temporally stable reflection, A = accumulated hit mask

struct SSRTemporalParams {
    feedback: f32,
    halfWidth: u32,
    halfHeight: u32,
    fullWidth: u32,
    fullHeight: u32,
    _pad0: u32,
    _pad1: u32,
    _pad2: u32,
};

@group(0) @binding(0) var ssrTrace: texture_2d<f32>;        // half-res trace output
@group(0) @binding(1) var ssrHistory: texture_2d<f32>;      // half-res prev-frame temporal
@group(0) @binding(2) var velocityTex: texture_2d<f32>;     // full-res motion vectors
@group(0) @binding(3) var ssrOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var<uniform> params: SSRTemporalParams;

fn sampleBilinear(tex: texture_2d<f32>, uv: vec2f, dims: vec2u) -> vec4f {
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

fn sampleVelocity(fullPixelUV: vec2f) -> vec2f {
    let dims = vec2u(params.fullWidth, params.fullHeight);
    let coord = fullPixelUV * vec2f(dims) - 0.5;
    let base = vec2i(i32(floor(coord.x)), i32(floor(coord.y)));
    let frac_ = fract(coord);
    let maxCoord = vec2i(i32(dims.x) - 1, i32(dims.y) - 1);

    let c00 = clamp(base, vec2i(0), maxCoord);
    let c10 = clamp(base + vec2i(1, 0), vec2i(0), maxCoord);
    let c01 = clamp(base + vec2i(0, 1), vec2i(0), maxCoord);
    let c11 = clamp(base + vec2i(1, 1), vec2i(0), maxCoord);

    let h00 = textureLoad(velocityTex, vec2u(c00), 0).rg;
    let h10 = textureLoad(velocityTex, vec2u(c10), 0).rg;
    let h01 = textureLoad(velocityTex, vec2u(c01), 0).rg;
    let h11 = textureLoad(velocityTex, vec2u(c11), 0).rg;

    let fx = clamp(frac_.x, 0.0, 1.0);
    let fy = clamp(frac_.y, 0.0, 1.0);

    return h00 * (1.0 - fx) * (1.0 - fy)
         + h10 * fx * (1.0 - fy)
         + h01 * (1.0 - fx) * fy
         + h11 * fx * fy;
}

fn rgbToYCoCg(c: vec3f) -> vec3f {
    let Y  =  0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
    let Co =  0.5  * c.r - 0.5 * c.b;
    let Cg = -0.25 * c.r + 0.5 * c.g - 0.25 * c.b;
    return vec3f(Y, Co, Cg);
}

fn ycocgToRgb(c: vec3f) -> vec3f {
    let R = c.x + c.y - c.z;
    let G = c.x + c.z;
    let B = c.x - c.y - c.z;
    return vec3f(R, G, B);
}

@compute @workgroup_size(8, 8, 1)
fn ssr_temporal(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= params.halfWidth || gid.y >= params.halfHeight) { return; }

    let halfPixel = gid.xy;
    let halfUV = (vec2f(halfPixel) + 0.5) / vec2f(f32(params.halfWidth), f32(params.halfHeight));
    let fullUV = (vec2f(halfPixel * 2u) + 1.0) / vec2f(f32(params.fullWidth), f32(params.fullHeight));

    let current = textureLoad(ssrTrace, halfPixel, 0);
    let currentColor = current.rgb;
    let currentMask = current.a;

    // Full-res velocity → half-res by bilinear
    let velocity = sampleVelocity(fullUV);
    // Velocity is in UV units at full-res; same units apply at half-res.
    let prevHalfUV = halfUV - velocity;

    let inBounds = prevHalfUV.x >= 0.0 && prevHalfUV.x <= 1.0 &&
                   prevHalfUV.y >= 0.0 && prevHalfUV.y <= 1.0;

    if (!inBounds) {
        textureStore(ssrOutput, halfPixel, vec4f(currentColor, currentMask));
        return;
    }

    let histDims = vec2u(textureDimensions(ssrHistory, 0));
    let history = sampleBilinear(ssrHistory, prevHalfUV, histDims);

    // WebGPU textures start with undefined contents. On WASM the history slot
    // can contain NaN bit patterns until a valid frame is blitted into it
    // (native Metal happens to zero-init textures, masking this). Once NaN
    // enters the temporal blend it propagates forever — mix(current, NaN, t)
    // is NaN — turning the entire SSR output black. Skip the blend and use
    // the current frame's trace result when history is contaminated.
    if (any(isnan(history))) {
        textureStore(ssrOutput, halfPixel, vec4f(currentColor, currentMask));
        return;
    }

    let historyColor = history.rgb;
    let historyMask = history.a;

    // 3x3 neighborhood of current trace output (YCoCg space for tighter clip).
    var nbSum = rgbToYCoCg(currentColor);
    var nbSum2 = nbSum * nbSum;
    var nbCount: i32 = 1;

    for (var dy: i32 = -1; dy <= 1; dy++) {
        for (var dx: i32 = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) { continue; }
            let nPos = vec2i(i32(halfPixel.x) + dx, i32(halfPixel.y) + dy);
            if (nPos.x < 0 || nPos.y < 0 ||
                nPos.x >= i32(params.halfWidth) || nPos.y >= i32(params.halfHeight)) {
                continue;
            }
            let nb = rgbToYCoCg(textureLoad(ssrTrace, vec2u(nPos), 0).rgb);
            nbSum += nb;
            nbSum2 += nb * nb;
            nbCount++;
        }
    }

    let fCount = f32(nbCount);
    let mean = nbSum / fCount;
    let variance = abs(nbSum2 / fCount - mean * mean);
    let sigma = sqrt(max(variance, vec3f(0.0)));
    let sigmaFloor = max(abs(mean) * 0.1, vec3f(0.01));
    let aabbMin = mean - sigma * 2.0 - sigmaFloor;
    let aabbMax = mean + sigma * 2.0 + sigmaFloor;

    let histYCoCg = rgbToYCoCg(historyColor);
    let clampedYCoCg = clamp(histYCoCg, aabbMin, aabbMax);
    let clampedHistory = ycocgToRgb(clampedYCoCg);

    // Lower feedback for fresh hits (more weight on current), higher for misses (rely on history).
    let confidenceScale = mix(0.5, 1.0, currentMask);
    let effectiveFeedback = params.feedback * confidenceScale;

    let resultColor = mix(currentColor, clampedHistory, effectiveFeedback);
    let resultMask = mix(currentMask, historyMask, effectiveFeedback);

    textureStore(ssrOutput, halfPixel, vec4f(resultColor, resultMask));
}
