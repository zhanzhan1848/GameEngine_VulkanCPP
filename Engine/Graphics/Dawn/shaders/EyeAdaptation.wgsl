// Native WGSL counterpart of the frozen Vulkan EyeAdaptation.comp.
// One 16x16 workgroup, 16 samples/lane, log-luminance reduction and EMA.
struct AdaptParams {
    rateUp: f32, rateDown: f32, minLogLum: f32, maxLogLum: f32,
    brightClip: f32, cutSnap: f32, pad: vec2<f32>,
};
@group(0) @binding(0) var hdr_scene: texture_2d<f32>;
@group(0) @binding(1) var prev_state: texture_2d<f32>;
@group(0) @binding(2) var new_state: texture_storage_2d<r32float, write>;
@group(0) @binding(3) var<uniform> params: AdaptParams;
var<workgroup> logLum: array<f32, 256>;

fn luminance(c: vec3<f32>) -> f32 {
    // Exponent bits catch NaN/Inf without relying on finite-math comparisons.
    let bits = bitcast<vec3<u32>>(c) & vec3<u32>(0x7f800000u);
    if (any(bits == vec3<u32>(0x7f800000u))) { return 0.0; }
    return dot(max(c, vec3<f32>(0.0)), vec3<f32>(0.2126, 0.7152, 0.0722));
}

@compute @workgroup_size(16, 16, 1)
fn eye_adaptation(@builtin(local_invocation_id) gid: vec3<u32>,
                  @builtin(local_invocation_index) lane: u32) {
    let dims = vec2<i32>(textureDimensions(hdr_scene));
    let tile = max(dims / 16, vec2<i32>(1));
    let origin = vec2<i32>(gid.xy) * tile;
    var sum = 0.0;
    for (var y = 0; y < 4; y++) {
        for (var x = 0; x < 4; x++) {
            let p = clamp(origin + vec2<i32>(x, y) * 8 + tile / 8,
                          vec2<i32>(0), dims - vec2<i32>(1));
            let l = min(luminance(textureLoad(hdr_scene, p, 0).rgb), params.brightClip);
            sum += log2(max(l, 1e-4));
        }
    }
    logLum[lane] = sum / 16.0;
    workgroupBarrier();
    for (var step = 128u; step > 0u; step >>= 1u) {
        if (lane < step) { logLum[lane] += logLum[lane + step]; }
        workgroupBarrier();
    }
    if (lane == 0u) {
        var avg = logLum[0] / 256.0;
        var prev = textureLoad(prev_state, vec2<i32>(0), 0).r;
        if (!(prev > -64.0 && prev < 64.0)) { prev = 0.0; }
        if (!(avg > -64.0 && avg < 64.0)) { avg = prev; }
        let delta = avg - prev;
        var rate = select(params.rateDown, params.rateUp, delta > 0.0);
        if (abs(delta) > params.cutSnap) { rate = 1.0; }
        let adapted = clamp(mix(prev, avg, clamp(rate, 0.0, 1.0)), params.minLogLum, params.maxLogLum);
        textureStore(new_state, vec2<i32>(0), vec4<f32>(adapted));
    }
}
