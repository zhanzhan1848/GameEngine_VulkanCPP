// SSGIHalfResDenoise.wgsl — 5x5 Gaussian pre-smooth at half resolution
// Pure Gaussian blur with no edge-stopping weights.
// Edge preservation is handled by the full-res bilateral spatial filter.

struct HalfResDenoiseParams {
    width: u32,
    height: u32,
    sigma: f32,
    _pad: u32,
};

@group(0) @binding(0) var traceInput: texture_2d<f32>;
@group(0) @binding(1) var traceOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> params: HalfResDenoiseParams;

@compute @workgroup_size(8, 8, 1)
fn ssgi_halfres_denoise(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= params.width || gid.y >= params.height) { return; }

    let invTwoSigmaSq = 1.0 / (2.0 * params.sigma * params.sigma);

    var filteredIrr = vec3f(0.0);
    var filteredDist: f32 = 0.0;
    var totalWeight: f32 = 0.0;

    for (var dy: i32 = -2; dy <= 2; dy++) {
        for (var dx: i32 = -2; dx <= 2; dx++) {
            let pos = vec2i(i32(gid.x) + dx, i32(gid.y) + dy);
            if (pos.x < 0 || pos.y < 0 ||
                pos.x >= i32(params.width) || pos.y >= i32(params.height)) {
                continue;
            }

            let s = textureLoad(traceInput, vec2u(pos), 0);
            let spatial2 = f32(dx * dx + dy * dy);
            let w = exp(-spatial2 * invTwoSigmaSq);

            filteredIrr += s.rgb * w;
            filteredDist += s.a * w;
            totalWeight += w;
        }
    }

    if (totalWeight > 0.001) {
        textureStore(traceOutput, gid.xy,
                     vec4f(filteredIrr / totalWeight, filteredDist / totalWeight));
    } else {
        textureStore(traceOutput, gid.xy, textureLoad(traceInput, gid.xy, 0));
    }
}
