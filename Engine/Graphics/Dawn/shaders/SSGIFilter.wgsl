// SSGIFilter.wgsl — Bilateral spatial filter with bilinear upsampling to full resolution
// Reads half-res denoised SSGI, applies edge-preserving filter, outputs full-res.

struct SSGIFilterParams {
    invProj: mat4x4<f32>,
    screenSize: vec4<f32>,       // full-res: x=width, y=height, z=1/width, w=1/height
    halfScreenSize: vec4<f32>,   // half-res dims for UV mapping
    sigmaDepth: f32,
    sigmaNormal: f32,
    sigmaHitDist: f32,
    sigmaSpatial: f32,
    kernelRadius: u32,
    _pad0: u32,
    _pad1: u32,
    _pad2: u32,
};

@group(0) @binding(0) var ssgiInput: texture_2d<f32>;       // half-res denoised
@group(0) @binding(1) var depthTex: texture_depth_2d;       // full-res depth
@group(0) @binding(2) var ssgiOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var<uniform> params: SSGIFilterParams;

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
    // contaminates the SSGI chain. Same pattern as SSGITrace.wgsl.
    let crossVec = cross(pD - pU, pR - pL);
    let crossLen = length(crossVec);
    if (crossLen < 1e-6 || crossLen != crossLen || abs(crossLen) > 3.4e38) {
        return vec3f(0.0, 0.0, 1.0);
    }
    return crossVec / crossLen;
}

// Manual bilinear sampling for half-res -> full-res upsampling
fn sampleBilinear(tex: texture_2d<f32>, uv: vec2f) -> vec4f {
    let dims = vec2u(textureDimensions(tex, 0));
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

@compute @workgroup_size(8, 8, 1)
fn ssgi_filter(@builtin(global_invocation_id) gid: vec3u) {
    let width = u32(params.screenSize.x);
    let height = u32(params.screenSize.y);
    if (gid.x >= width || gid.y >= height) { return; }

    let pixelPos = gid.xy;
    let invRes = params.screenSize.zw;
    let centerUV = (vec2f(pixelPos) + 0.5) * invRes;

    // Bilinear upsample from half-res
    let centerSSGI = sampleBilinear(ssgiInput, centerUV);
    let centerDepth = textureLoad(depthTex, pixelPos, 0);
    let centerNormal = reconstructViewNormal(pixelPos, centerUV, centerDepth);

    let centerIrradiance = centerSSGI.rgb;
    let centerHitDist = centerSSGI.a;

    // Bilateral filter
    let radius = min(params.kernelRadius, 4u);

    var filteredIrr = vec3f(0.0);
    var filteredDist: f32 = 0.0;
    var totalWeight: f32 = 0.0;

    // Stride by 2 full-res pixels per loop iteration so dx,dy are in half-res
    // texel units. The previous +dx,+dy (full-res) stride only covered ~2x2
    // half-res texels at radius=2 — not enough to smooth the half-res grid,
    // which left HZB mip-transition banding visible as diagonal stripes.
    for (var dy: i32 = -i32(radius); dy <= i32(radius); dy++) {
        for (var dx: i32 = -i32(radius); dx <= i32(radius); dx++) {
            let samplePos = vec2i(i32(pixelPos.x) + dx * 2, i32(pixelPos.y) + dy * 2);
            if (samplePos.x < 0 || samplePos.y < 0 ||
                samplePos.x >= i32(width) || samplePos.y >= i32(height)) {
                continue;
            }

            let sampleUV = (vec2f(samplePos) + 0.5) * invRes;
            let sampleSSGI = sampleBilinear(ssgiInput, sampleUV);
            let sampleDepth = textureLoad(depthTex, vec2u(samplePos), 0);
            let sampleNormal = reconstructViewNormal(vec2u(samplePos), sampleUV, sampleDepth);

            // Depth weight — use linear view-space Z, not NDC depth.
            // NDC depth compresses non-linearly at far distances (5m pillar vs
            // 50m wall both map to NDC ~0.99, diff ~0.01), which made the old
            // exp(-ndcDiff * 10) weight ~0.9 across depth discontinuities —
            // pillar SSGI leaked onto walls behind them. View Z is linear in
            // meters; sigmaDepth is now a real-world tolerance (0.3m).
            let centerViewZ = reconstructViewPos(centerUV, centerDepth).z;
            let sampleViewZ = reconstructViewPos(sampleUV, sampleDepth).z;
            let depthDiff = abs(centerViewZ - sampleViewZ);
            let wDepth = exp(-depthDiff * depthDiff /
                             (2.0 * params.sigmaDepth * params.sigmaDepth));

            // Normal weight
            let NdotN = max(0.0, dot(centerNormal, sampleNormal));
            let wNormal = pow(NdotN, params.sigmaNormal);

            // Hit distance weight
            let distDiff = abs(centerHitDist - sampleSSGI.a);
            let wDist = exp(-distDiff * params.sigmaHitDist);

            // Adaptive spatial weight
            let adaptiveSigma = max(params.sigmaSpatial * centerHitDist * 0.5,
                                    params.sigmaSpatial * 0.5);
            let spatialDist2 = f32(dx * dx + dy * dy);
            let wSpatial = exp(-spatialDist2 / (2.0 * adaptiveSigma * adaptiveSigma));

            let weight = wDepth * wNormal * wDist * wSpatial;

            filteredIrr += sampleSSGI.rgb * weight;
            filteredDist += sampleSSGI.a * weight;
            totalWeight += weight;
        }
    }

    if (totalWeight > 1e-6) {
        filteredIrr /= totalWeight;
        filteredDist /= totalWeight;
    } else {
        filteredIrr = centerIrradiance;
        filteredDist = centerHitDist;
    }

    textureStore(ssgiOutput, pixelPos, vec4f(filteredIrr, filteredDist));
}
