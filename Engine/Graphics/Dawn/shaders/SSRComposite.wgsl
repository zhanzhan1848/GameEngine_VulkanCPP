// SSRComposite.wgsl — Full-resolution composite.
//   Reads half-res temporally-stable SSR result + full-res HDR + depth.
//   Upsamples SSR via bilinear, applies Fresnel mask (fake roughness: stronger
//   at grazing angles), additive-blends reflection radiance into HDR.

struct SSRCompositeParams {
    screenWidth: u32,
    screenHeight: u32,
    halfWidth: u32,
    halfHeight: u32,
    fresnelPower: f32,
    reflectionStrength: f32,
    debugMode: u32,           // 0 = normal composite, 1 = raw SSR viz (Fresnel bypassed)
    _pad1: u32,
    invProj: mat4x4<f32>,
};

@group(0) @binding(0) var hdrTex: texture_2d<f32>;
@group(0) @binding(1) var ssrTex: texture_2d<f32>;          // half-res temporal output
@group(0) @binding(2) var depthTex: texture_depth_2d;
@group(0) @binding(3) var outTex: texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var<uniform> params: SSRCompositeParams;

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

fn reconstructViewPos(uv: vec2f, ndcDepth: f32) -> vec3f {
    let ndcX = uv.x * 2.0 - 1.0;
    let ndcY = 1.0 - uv.y * 2.0;
    let clipPos = vec4f(ndcX, ndcY, ndcDepth, 1.0);
    let viewPos4 = params.invProj * clipPos;
    let vp = viewPos4.xyz / viewPos4.w;
    return vec3f(vp.x, vp.y, -vp.z);
}

fn reconstructViewNormal(pixel: vec2u, uv: vec2f, depth: f32) -> vec3f {
    let sw = vec4f(
        f32(params.screenWidth), f32(params.screenHeight),
        1.0 / f32(params.screenWidth), 1.0 / f32(params.screenHeight)
    );
    let px = clamp(i32(pixel.x), 1, i32(sw.x) - 2);
    let py = clamp(i32(pixel.y), 1, i32(sw.y) - 2);
    let dL = textureLoad(depthTex, vec2u(u32(px - 1), u32(py)), 0);
    let dR = textureLoad(depthTex, vec2u(u32(px + 1), u32(py)), 0);
    let dU = textureLoad(depthTex, vec2u(u32(px), u32(py - 1)), 0);
    let dD = textureLoad(depthTex, vec2u(u32(px), u32(py + 1)), 0);

    let pL = reconstructViewPos(vec2f(uv.x - sw.z, uv.y), dL);
    let pR = reconstructViewPos(vec2f(uv.x + sw.z, uv.y), dR);
    let pU = reconstructViewPos(vec2f(uv.x, uv.y - sw.w), dU);
    let pD = reconstructViewPos(vec2f(uv.x, uv.y + sw.w), dD);

    // Cross sign is ambiguous; force camera-facing (-Z in this view convention).
    var n = normalize(cross(pD - pU, pR - pL));
    if (n.z > 0.0) { n = -n; }
    return n;
}

@compute @workgroup_size(8, 8, 1)
fn ssr_composite(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= params.screenWidth || gid.y >= params.screenHeight) { return; }

    let pixel = gid.xy;
    let hdr = textureLoad(hdrTex, pixel, 0);
    let depth = textureLoad(depthTex, pixel, 0);

    // Sky / no-composite path.
    if (depth >= 0.9999) {
        textureStore(outTex, pixel, hdr);
        return;
    }

    // Upsample SSR from half-res. UV must be in [0,1] normalized space — compute
    // from full-res pixel coords / full-res dims (NOT half-res dims).
    let fullUV = (vec2f(pixel) + 0.5) / vec2f(f32(params.screenWidth), f32(params.screenHeight));
    let halfDims = vec2u(params.halfWidth, params.halfHeight);
    let ssr = sampleBilinear(ssrTex, fullUV, halfDims);

    // Debug mode: dump raw SSR (color codes from trace: red=miss, blue=cull, dark red=back-face, real=hit).
    if (params.debugMode == 1u) {
        textureStore(outTex, pixel, vec4f(ssr.rgb, 1.0));
        return;
    }

    // View-space normal + view direction → physically meaningful Fresnel.
    let pixelUV = (vec2f(pixel) + 0.5) / vec2f(f32(params.screenWidth), f32(params.screenHeight));
    let n = reconstructViewNormal(pixel, pixelUV, depth);
    // View dir: from surface point toward camera (origin).
    let surfacePos = reconstructViewPos(pixelUV, depth);
    let V = normalize(-surfacePos);
    let NdotV = max(dot(n, V), 0.0);
    let fresnel = pow(1.0 - NdotV, params.fresnelPower);

    let reflection = ssr.rgb * fresnel * params.reflectionStrength;
    let result = hdr.rgb + reflection;

    textureStore(outTex, pixel, vec4f(result, hdr.a));
}
