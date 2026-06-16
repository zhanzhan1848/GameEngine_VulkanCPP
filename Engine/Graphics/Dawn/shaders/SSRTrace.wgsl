// SSRTrace.wgsl — Screen-Space Reflections: Hi-Z ray march for reflection ray.
// Half-resolution compute shader that:
//   1. Reads full-res depth, reconstructs view position + normal
//   2. Computes reflection ray: reflect(-V, N)
//   3. Traces ray through HZB depth pyramid
//   4. On hit, samples TAA-resolved HDR color as reflection radiance
//   5. Outputs RGBA16F: RGB = reflection color (pre-Fresnel), A = hit mask (1=hit, 0=miss)

const MAX_STEPS: u32 = 128u;

struct SSRTraceParams {
    invProj: mat4x4<f32>,
    proj: mat4x4<f32>,
    screenSize: vec4<f32>,       // full-res: x=width, y=height, z=1/width, w=1/height
    halfScreenSize: vec4<f32>,   // half-res: x=halfW, y=halfH, z=1/halfW, w=1/halfH
    maxDistance: f32,
    thickness: f32,
    nearPlane: f32,
    farPlane: f32,
    hzbMipLevels: u32,
    frameIndex: u32,
};

struct TraceResult {
    hit: bool,
    hitUV: vec2f,
    hitDist: f32,
};

@group(0) @binding(0) var depthTex: texture_depth_2d;
@group(0) @binding(1) var hzbTex: texture_2d<f32>;
@group(0) @binding(2) var colorTex: texture_2d<f32>;        // TAA'd HDR
@group(0) @binding(3) var ssrOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var<uniform> params: SSRTraceParams;

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

    // Cross product sign is ambiguous from depth alone — force the normal
    // to face the camera. In this shader's view convention, visible points
    // have +Z, so camera-facing normals have -Z. Flip if needed.
    var n = normalize(cross(pD - pU, pR - pL));
    if (n.z > 0.0) { n = -n; }
    return n;
}

fn viewToScreen(viewPos: vec3f) -> vec3f {
    let clipPos = params.proj * vec4f(viewPos.x, viewPos.y, -viewPos.z, 1.0);
    let ndc = clipPos.xyz / clipPos.w;
    return vec3f(ndc.x * 0.5 + 0.5, 0.5 - 0.5 * ndc.y, ndc.z);
}

// Depth-aware color sample: standard bilinear fetches 4 taps; if the depth
// spread across the 2x2 tap quad exceeds the thickness tolerance, the taps
// straddle a silhouette edge (foreground/background mix) and the color is
// unreliable. Grazing-angle reflection rays have long screen-space footprints
// and disproportionately often land near such edges; the bilinear smear of
// bright-lit edge pixels against background produces the characteristic
// white-line artifacts on side walls facing left/right of the camera.
fn sampleColorDepthAware(uv: vec2f) -> vec3f {
    let dims = textureDimensions(colorTex, 0);
    let fdims = vec2f(dims);
    let coord = uv * fdims - 0.5;
    let bx = clamp(i32(coord.x), 0, i32(dims.x) - 1);
    let by = clamp(i32(coord.y), 0, i32(dims.y) - 1);
    let bx1 = min(u32(bx) + 1u, dims.x - 1u);
    let by1 = min(u32(by) + 1u, dims.y - 1u);

    let p00 = vec2u(u32(bx), u32(by));
    let p10 = vec2u(bx1, u32(by));
    let p01 = vec2u(u32(bx), by1);
    let p11 = vec2u(bx1, by1);

    let d00 = ndcDepthToLinear(textureLoad(depthTex, p00, 0));
    let d10 = ndcDepthToLinear(textureLoad(depthTex, p10, 0));
    let d01 = ndcDepthToLinear(textureLoad(depthTex, p01, 0));
    let d11 = ndcDepthToLinear(textureLoad(depthTex, p11, 0));

    let dMin = min(min(d00, d10), min(d01, d11));
    let dMax = max(max(d00, d10), max(d01, d11));
    if (dMax - dMin > params.thickness) {
        return vec3f(0.0);
    }

    let c00 = textureLoad(colorTex, p00, 0);
    let c10 = textureLoad(colorTex, p10, 0);
    let c01 = textureLoad(colorTex, p01, 0);
    let c11 = textureLoad(colorTex, p11, 0);

    let fx = clamp(fract(coord.x), 0.0, 1.0);
    let fy = clamp(fract(coord.y), 0.0, 1.0);
    return mix(mix(c00, c10, fx), mix(c01, c11, fx), fy).rgb;
}

// HZB ray march — fixed view-space step (known-good baseline) with source-UV
// self-hit skip. Grazing-angle stripes are handled externally by the
// grazingFade in the entry function (cheaper and more reliable than trying to
// fix the march itself, which proved fragile: adaptive screen-UV steps die
// for head-on reflections where dUVdt ≈ 0 makes stepSize blow up).
fn traceRayHZB(rayOriginView: vec3f, rayDirView: vec3f, sourceUV: vec2f) -> TraceResult {
    var result: TraceResult;
    result.hit = false;

    let stepSize = params.maxDistance / f32(MAX_STEPS);
    var t: f32 = 0.0;
    var mip: u32 = 1u;

    for (var step_i: u32 = 0u; step_i < MAX_STEPS; step_i++) {
        t += stepSize;
        if (t > params.maxDistance) { break; }

        let rayPos = rayOriginView + rayDirView * t;
        let screen = viewToScreen(rayPos);
        let sampleUV = screen.xy;

        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 ||
            sampleUV.y < 0.0 || sampleUV.y > 1.0) { break; }
        if (rayPos.z < params.nearPlane) { continue; }

        // Skip source surface (within 2 pixels) to reduce self-hit noise.
        let dpx = abs(sampleUV.x - sourceUV.x) * params.screenSize.x;
        let dpy = abs(sampleUV.y - sourceUV.y) * params.screenSize.y;
        if (max(dpx, dpy) < 2.0) { continue; }

        let mipDims = textureDimensions(hzbTex, mip);
        let mipCoord = clamp(vec2u(sampleUV * vec2f(mipDims)),
                             vec2u(0u), vec2u(mipDims - vec2u(1u, 1u)));

        let sceneDepthNDC = textureLoad(hzbTex, mipCoord, mip).r;
        let sceneDepthLinear = ndcDepthToLinear(sceneDepthNDC);
        let rayDepthLinear = rayPos.z;

        if (rayDepthLinear > sceneDepthLinear) {
            if (mip == 0u) {
                if (rayDepthLinear <= sceneDepthLinear + params.thickness) {
                    result.hit = true;
                    result.hitUV = sampleUV;
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

@compute @workgroup_size(8, 8, 1)
fn ssr_trace(@builtin(global_invocation_id) gid: vec3u) {
    let halfDims = vec2u(params.halfScreenSize.xy);
    if (gid.x >= halfDims.x || gid.y >= halfDims.y) { return; }

    let halfPos = gid.xy;
    let fullPos = halfPos * 2u;
    let fullDims = vec2u(params.screenSize.xy);

    if (fullPos.x >= fullDims.x || fullPos.y >= fullDims.y) {
        textureStore(ssrOutput, halfPos, vec4f(0.0));
        return;
    }

    let depth = textureLoad(depthTex, fullPos, 0);
    let linearDepth = ndcDepthToLinear(depth);

    // Sky / near / far cull — no reflections there.
    if (depth >= 0.9999 || linearDepth < params.nearPlane ||
        linearDepth > params.farPlane * 0.999) {
        textureStore(ssrOutput, halfPos, vec4f(0.0));
        return;
    }

    let pixelUV = (vec2f(fullPos) + 0.5) / params.screenSize.xy;
    let viewPos = reconstructViewPos(pixelUV, depth);
    let viewNormal = reconstructViewNormal(fullPos, pixelUV, depth);

    // Reflection ray: reflect view ray around normal.
    let toCamera = normalize(-viewPos);
    let reflDir = reflect(-toCamera, viewNormal);

    // NdotV == dot(reflDir, viewNormal) (reflection is sign-symmetric).
    // Below 0 the surface is back-facing; cull.
    let NdotV = dot(reflDir, viewNormal);
    if (NdotV <= 0.0) {
        textureStore(ssrOutput, halfPos, vec4f(0.0));
        return;
    }

    // Push origin along the normal — more at grazing angles to avoid
    // the ray self-intersecting the source surface.
    let rayOffset = max(0.05, 0.05 / max(NdotV, 0.3));
    let rayOrigin = viewPos + viewNormal * rayOffset;

    // sourceUV lets traceRayHZB skip self-hit pixels.
    let traceResult = traceRayHZB(rayOrigin, reflDir, pixelUV);

    if (traceResult.hit) {
        // Back-face rejection: reconstruct the normal at the hit pixel and reject
        // if the surface faces away from the incoming ray. Grazing-angle rays often
        // tangentially clip the side of geometry (column edges, silhouette slivers)
        // where the surface normal points away from the ray; the bilinear color
        // sample there picks up bright sky/AA-edge pixels → white-line artifacts.
        let hitPxX = clamp(u32(traceResult.hitUV.x * params.screenSize.x), 1u, u32(params.screenSize.x) - 2u);
        let hitPxY = clamp(u32(traceResult.hitUV.y * params.screenSize.y), 1u, u32(params.screenSize.y) - 2u);
        let hitPx = vec2u(hitPxX, hitPxY);
        let hitDepth = textureLoad(depthTex, hitPx, 0);
        let hitNormal = reconstructViewNormal(hitPx, traceResult.hitUV, hitDepth);
        // Ray direction is incoming; check if surface faces back along the ray.
        if (dot(hitNormal, -reflDir) <= 0.0) {
            textureStore(ssrOutput, halfPos, vec4f(0.0));
            return;
        }

        let hitColor = sampleColorDepthAware(traceResult.hitUV);

        let distFade = 1.0 - smoothstep(params.maxDistance * 0.6, params.maxDistance,
                                         traceResult.hitDist);
        let edgeDist = min(traceResult.hitUV, 1.0 - traceResult.hitUV);
        let edgeFade = smoothstep(0.0, 0.1, min(edgeDist.x, edgeDist.y));

        // Grazing-angle fade: kills stripe regions on perpendicular walls.
        // Fixed-step march produces periodic mip oscillation at low NdotV;
        // rather than fight the march algorithm (which proved fragile), we
        // fade out reflections where the artifact dominates.
        let grazingFade = smoothstep(0.2, 0.5, NdotV);

        let strength = distFade * edgeFade * grazingFade;
        textureStore(ssrOutput, halfPos, vec4f(hitColor.rgb * strength, strength));
    } else {
        textureStore(ssrOutput, halfPos, vec4f(0.0));
    }
}
