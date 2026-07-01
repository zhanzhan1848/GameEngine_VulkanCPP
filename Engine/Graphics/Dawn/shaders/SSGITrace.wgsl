// SSGITrace.wgsl — Lumen SSGI Phase 1: HZB-based screen space GI ray tracing
// Half-resolution compute shader that:
//   1. Reads depth buffer at full resolution
//   2. Reconstructs view-space position and normal from depth
//   3. Casts multiple rays in a hemisphere around the surface normal
//   4. Traces each ray through the HZB depth pyramid
//   5. On hit, samples previous frame scene color to gather indirect irradiance
//   6. Outputs RGBA16F: RGB = accumulated irradiance, A = average hit distance

const PI: f32 = 3.14159265358979323846;
const MAX_STEPS: u32 = 16u;
const MAX_DISTANCE: f32 = 20.0;

struct SSGITraceParams {
    invProj: mat4x4<f32>,
    proj: mat4x4<f32>,
    screenSize: vec4<f32>,       // full-res: x=width, y=height, z=1/width, w=1/height
    halfScreenSize: vec4<f32>,   // half-res: x=halfW, y=halfH, z=1/halfW, w=1/halfH
    rayCount: u32,
    radius: f32,
    thickness: f32,
    frameIndex: u32,
    nearPlane: f32,
    farPlane: f32,
    hzbMipLevels: u32,
    _pad0: u32,
};

struct TraceResult {
    hit: bool,
    hitUV: vec2f,
    hitDist: f32,
};

@group(0) @binding(0) var depthTex: texture_depth_2d;
@group(0) @binding(1) var hzbTex: texture_2d<f32>;
@group(0) @binding(2) var prevColorTex: texture_2d<f32>;
@group(0) @binding(3) var ssgiOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var<uniform> params: SSGITraceParams;

// --- Utility functions ---

fn hash_f2(p: vec2f) -> f32 {
    var p3 = fract(vec3f(p.x, p.y, p.x) * 0.1031);
    p3 = p3 + dot(p3, vec3f(p3.y, p3.z, p3.x) + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

fn ndcDepthToLinear(ndcDepth: f32) -> f32 {
    return (2.0 * params.nearPlane * params.farPlane) /
           (params.farPlane + params.nearPlane - ndcDepth * (params.farPlane - params.nearPlane));
}

fn reconstructViewPos(uv: vec2f, ndcDepth: f32) -> vec3f {
    let ndcX = uv.x * 2.0 - 1.0;
    let ndcY = 1.0 - uv.y * 2.0;
    let clipPos = vec4f(ndcX, ndcY, ndcDepth, 1.0);
    let viewPos4 = params.invProj * clipPos;
    let vp = viewPos4.xyz / viewPos4.w;
    return vec3f(vp.x, vp.y, -vp.z);
}

fn reconstructViewNormal(pixel: vec2u, uv: vec2f, depth: f32) -> vec3f {
    let ts = params.screenSize.zw;
    let px = clamp(i32(pixel.x), 1, i32(params.screenSize.x) - 2);
    let py = clamp(i32(pixel.y), 1, i32(params.screenSize.y) - 2);

    let dL = textureLoad(depthTex, vec2u(u32(px - 1), u32(py)), 0);
    let dR = textureLoad(depthTex, vec2u(u32(px + 1), u32(py)), 0);
    let dU = textureLoad(depthTex, vec2u(u32(px), u32(py - 1)), 0);
    let dD = textureLoad(depthTex, vec2u(u32(px), u32(py + 1)), 0);

    let pL = reconstructViewPos(vec2f(uv.x - ts.x, uv.y), dL);
    let pR = reconstructViewPos(vec2f(uv.x + ts.x, uv.y), dR);
    let pU = reconstructViewPos(vec2f(uv.x, uv.y - ts.y), dU);
    let pD = reconstructViewPos(vec2f(uv.x, uv.y + ts.y), dD);

    // Guard against degenerate neighborhoods (flat surface, NaN depth) —
    // cross of near-zero vector normalized to zero produces NaN that
    // contaminates the whole SSGI chain via the bilateral filter.
    // x != x is the classic NaN check (NaN is the only value not equal to
    // itself); abs(x) > 3.4e38 catches ±Inf. tint in this Dawn build doesn't
    // expose the WGSL isnan/isinf builtins.
    let crossVec = cross(pD - pU, pR - pL);
    let crossLen = length(crossVec);
    if (crossLen < 1e-6 || crossLen != crossLen || abs(crossLen) > 3.4e38) {
        return vec3f(0.0, 0.0, 1.0);
    }
    return crossVec / crossLen;
}

fn viewToScreen(viewPos: vec3f) -> vec3f {
    let clipPos = params.proj * vec4f(viewPos.x, viewPos.y, -viewPos.z, 1.0);
    let ndc = clipPos.xyz / clipPos.w;
    return vec3f(ndc.x * 0.5 + 0.5, 0.5 - 0.5 * ndc.y, ndc.z);
}

fn cosineHemisphereSample(N: vec3f, seed: vec2f, sampleIdx: u32, frameIdx: u32, rayCount: u32) -> vec3f {
    // Per-pixel hash rotation breaks the strict 45/135/225/315° diagonal
    // alignment that rayCount=4 produced when xi1 was deterministic in
    // (sampleIdx, frameIdx). That alignment left every pixel casting the
    // same four diagonal rays, which interacted with HZB mip-3 (8px) blocks
    // to produce the stationary-then-rotating diagonal stripe pattern.
    let perPixelRot = hash_f2(seed);
    let xi1 = fract((f32(sampleIdx) + 0.5) / f32(rayCount) + perPixelRot + f32(frameIdx) * 0.618033988749);
    let xi2 = fract(hash_f2(seed + vec2f(f32(sampleIdx) * 0.13, f32(sampleIdx) * 0.91))
                    + f32(frameIdx) * 0.7071067811865476);

    let phi = 2.0 * PI * xi1;
    let cosTheta = sqrt(max(xi2, 0.001));
    let sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    let localDir = vec3f(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    let up = select(vec3f(0.0, 0.0, 1.0), vec3f(1.0, 0.0, 0.0), abs(N.z) < 0.999);
    let tangent = normalize(cross(up, N));
    let bitangent = cross(N, tangent);

    var result = tangent * localDir.x + bitangent * localDir.y + N * localDir.z;
    if (dot(result, N) < 0.0) { result = -result; }
    return normalize(result);
}

fn sampleBilinear(tex: texture_2d<f32>, uv: vec2f, dims: vec2u) -> vec4f {
    let coord = uv * vec2f(dims) - 0.5;
    let bx = clamp(i32(coord.x), 0, i32(dims.x) - 1);
    let by = clamp(i32(coord.y), 0, i32(dims.y) - 1);
    let fx = clamp(fract(coord.x), 0.0, 1.0);
    let fy = clamp(fract(coord.y), 0.0, 1.0);
    let bx1 = min(u32(bx) + 1u, dims.x - 1u);
    let by1 = min(u32(by) + 1u, dims.y - 1u);

    let c00 = textureLoad(tex, vec2u(u32(bx), u32(by)), 0);
    let c10 = textureLoad(tex, vec2u(bx1, u32(by)), 0);
    let c01 = textureLoad(tex, vec2u(u32(bx), by1), 0);
    let c11 = textureLoad(tex, vec2u(bx1, by1), 0);
    return mix(mix(c00, c10, fx), mix(c01, c11, fx), fy);
}

// HZB ray marching with uniform step size.
// Mip level adapts (refine on hit, coarsen on miss) but step is constant,
// preventing the exponential skip that caused rays to exhaust radius in 3-4 steps.
fn traceRayHZB(rayOriginView: vec3f, rayDirView: vec3f) -> TraceResult {
    var result: TraceResult;
    result.hit = false;

    let stepSize = params.radius / f32(MAX_STEPS);
    var t: f32 = 0.0;
    var mip: u32 = 1u;

    for (var step_i: u32 = 0u; step_i < MAX_STEPS; step_i++) {
        t += stepSize;
        if (t > params.radius) { break; }

        let rayPos = rayOriginView + rayDirView * t;
        let screen = viewToScreen(rayPos);
        let sampleUV = screen.xy;

        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 ||
            sampleUV.y < 0.0 || sampleUV.y > 1.0) { break; }
        if (rayPos.z < params.nearPlane) { continue; }

        let mipW = max(1u, u32(params.screenSize.x) >> mip);
        let mipH = max(1u, u32(params.screenSize.y) >> mip);
        let mipCoord = clamp(vec2u(sampleUV * vec2f(f32(mipW), f32(mipH))),
                             vec2u(0u), vec2u(mipW - 1u, mipH - 1u));

        let sceneDepthNDC = textureLoad(hzbTex, mipCoord, mip).r;
        let sceneDepthLinear = ndcDepthToLinear(sceneDepthNDC);
        let rayDepthLinear = rayPos.z;

        if (rayDepthLinear > sceneDepthLinear) {
            if (mip == 0u) {
                if (rayDepthLinear <= sceneDepthLinear + params.thickness) {
                    result.hit = true;
                    result.hitUV = sampleUV;
                    // hitDist is the ray travel distance (t), not the camera
                    // depth of the hit surface. Using sceneDepthLinear here
                    // made distAtten = 1-smoothstep(radius/2, radius, camDepth)
                    // zero out GI for any surface >radius meters from camera,
                    // regardless of how short the actual bounce was. It also
                    // poisoned the bilateral filter's adaptiveSigma (always
                    // large) and the temporal pass's hit-confidence gate.
                    result.hitDist = t;
                    break;
                }
            } else {
                t -= stepSize;
                mip = mip - 1u;
            }
        } else {
            mip = min(mip + 1u, params.hzbMipLevels - 1u);
        }
    }
    return result;
}

// --- Main compute entry point ---
@compute @workgroup_size(8, 8, 1)
fn ssgi_trace(@builtin(global_invocation_id) gid: vec3u) {
    let halfDims = vec2u(params.halfScreenSize.xy);
    if (gid.x >= halfDims.x || gid.y >= halfDims.y) { return; }

    let halfPos = gid.xy;
    let fullPos = halfPos * 2u;
    let fullDims = vec2u(params.screenSize.xy);

    if (fullPos.x >= fullDims.x || fullPos.y >= fullDims.y) {
        textureStore(ssgiOutput, halfPos, vec4f(0.0));
        return;
    }

    let depth = textureLoad(depthTex, fullPos, 0);
    let linearDepth = ndcDepthToLinear(depth);

    if (depth >= 0.9999 || linearDepth < params.nearPlane ||
        linearDepth > params.farPlane * 0.999) {
        textureStore(ssgiOutput, halfPos, vec4f(0.0));
        return;
    }

    let pixelUV = (vec2f(fullPos) + 0.5) / params.screenSize.xy;
    let viewPos = reconstructViewPos(pixelUV, depth);
    let viewNormal = reconstructViewNormal(fullPos, pixelUV, depth);

    var totalIrradiance = vec3f(0.0);
    var totalHitDist: f32 = 0.0;

    let baseSeed = vec2f(
        f32(fullPos.x) * 0.001 + f32(params.frameIndex) * 0.01,
        f32(fullPos.y) * 0.0017 + f32(params.frameIndex) * 0.013
    );
    let rayCount = max(params.rayCount, 1u);

    for (var rayIdx: u32 = 0u; rayIdx < rayCount; rayIdx++) {
        let rayDir = cosineHemisphereSample(viewNormal, baseSeed, rayIdx,
                                            params.frameIndex, rayCount);
        let rayOrigin = viewPos + viewNormal * 0.1;

        let traceResult = traceRayHZB(rayOrigin, rayDir);

        if (traceResult.hit) {
            let prevDims = vec2u(
                textureDimensions(prevColorTex, 0).x,
                textureDimensions(prevColorTex, 0).y
            );
            let hitColor = sampleBilinear(prevColorTex, traceResult.hitUV, prevDims);

            // Take raw hit radiance — no clamp/dim. MAX_RADIANCE + brightPenalty
            // were anti-blow-out guards but they desaturated GI by clamping warm
            // channels and dimming bright sunlit floor (the strongest color
            // bleed source) by ~3x. With PI× removed, output is physically
            // radiance-scale; let ToneMapping's SSGI_INTENSITY tune magnitude.
            var radiance = hitColor.rgb;

            // Distance attenuation — soft cubic-ish falloff across full radius.
            // Old (radius/2, radius) gave near-constant 1.0 inside inner zone
            // then steep cliff at the edge, producing hard-edged bright patches.
            // smoothstep(0.5, 1.0) replaces earlier (0.3, 1.0): keeps full
            // intensity out to 50% of radius, then tapers. Lets distant floor
            // bounces (4-8m at radius=10m) read instead of being crushed,
            // without boosting already-saturated close-range hits.
            let distAtten = 1.0 - smoothstep(0.5, 1.0,
                                              traceResult.hitDist / params.radius);
            // Edge fade
            let edgeDist = min(traceResult.hitUV, 1.0 - traceResult.hitUV);
            let edgeFade = smoothstep(0.0, 0.15, min(edgeDist.x, edgeDist.y));

            // Cosine-weighted sampling: irradiance E ≈ π·<Li>, but Lambertian
            // outgoing radiance Lo = albedo·E/π = albedo·<Li>. ToneMapping
            // composites this as outgoing radiance, so π cancels. albedo=1
            // assumed until GBuffer albedo plumbing is added.
            totalIrradiance += radiance * distAtten * edgeFade;
            totalHitDist += traceResult.hitDist;
        } else {
            totalHitDist += params.radius;
        }
    }

    var avgHitDist = totalHitDist / f32(rayCount);
    var outputIrradiance = totalIrradiance / f32(rayCount);

    // NaN/Inf guard — pathological rays (reconstructViewNormal returning a
    // fallback normal, HZB mip rounding producing inf-narrow cone, etc.)
    // must not contaminate downstream filters. Output zero so the temporal
    // pass cleanly drops these samples via variance clip. x != x catches NaN
    // (only value not equal to itself); abs(x) > 3.4e38 catches ±Inf.
    let nanMask = outputIrradiance != outputIrradiance;
    let infMask = abs(outputIrradiance) > vec3f(3.4e38);
    if (any(nanMask) || any(infMask)) {
        outputIrradiance = vec3f(0.0);
    }
    if (avgHitDist != avgHitDist || abs(avgHitDist) > 3.4e38) {
        avgHitDist = params.radius;
    }

    textureStore(ssgiOutput, halfPos, vec4f(outputIrradiance, avgHitDist));
}
