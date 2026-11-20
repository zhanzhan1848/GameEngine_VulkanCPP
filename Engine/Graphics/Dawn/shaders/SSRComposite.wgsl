// Half-resolution SSR upsample. Matches the shared SSRPass descriptor and
// parameter contract; the fusion pass applies the result to the HDR scene.
struct SSRCompositeParams {
    screenWidth: u32,
    screenHeight: u32,
    halfWidth: u32,
    halfHeight: u32,
    maxRoughness: f32,
    reflectionStrength: f32,
    _pad0: f32,
    _pad1: f32,
};
@group(0) @binding(0) var ssrTex: texture_2d<f32>;
@group(0) @binding(1) var ormTex: texture_2d<f32>;
@group(0) @binding(2) var outTex: texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var<uniform> params: SSRCompositeParams;

@compute @workgroup_size(8, 8, 1)
fn ssr_composite(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= params.screenWidth || gid.y >= params.screenHeight) { return; }
    let pixel = gid.xy;
    let uv = (vec2f(pixel) + 0.5) / vec2f(f32(params.screenWidth), f32(params.screenHeight));
    let dims = vec2i(i32(params.halfWidth), i32(params.halfHeight));
    let coord = uv * vec2f(dims) - 0.5;
    let base = vec2i(floor(coord));
    let f = clamp(fract(coord), vec2f(0.0), vec2f(1.0));
    let hi = dims - vec2i(1);
    let a = textureLoad(ssrTex, clamp(base, vec2i(0), hi), 0);
    let b = textureLoad(ssrTex, clamp(base + vec2i(1, 0), vec2i(0), hi), 0);
    let c = textureLoad(ssrTex, clamp(base + vec2i(0, 1), vec2i(0), hi), 0);
    let d = textureLoad(ssrTex, clamp(base + vec2i(1, 1), vec2i(0), hi), 0);
    let ssr = a * (1.0 - f.x) * (1.0 - f.y) + b * f.x * (1.0 - f.y)
            + c * (1.0 - f.x) * f.y + d * f.x * f.y;
    let roughness = textureLoad(ormTex, pixel, 0).g;
    let fade = 1.0 - smoothstep(params.maxRoughness * 0.5, params.maxRoughness, roughness);
    let strength = ssr.a * fade * params.reflectionStrength;
    textureStore(outTex, pixel, vec4f(ssr.rgb * strength, strength));
}
