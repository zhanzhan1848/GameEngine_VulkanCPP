// BlurPass.wgsl — separable Gaussian blur (5-tap linear-sampling variant)
// Consumed by the production C++ BlurPass (Engine/Graphics/Passes/BlurPass)
// on Vulkan (naga → BlurPass.spv) and Dawn (runtime WGSL).
//
// P0.2 (2026-09-15): BlurParams must be byte-for-byte the C++ BlurParams
// (BlurPass.h): 32-byte UBO, direction at byte 20. The old 16-byte block
// (direction at byte 0) made the kernel read textureWidth as direction —
// both H and V passes ran horizontal and bloom/VSM blur was wrong.

struct BlurParams {
    textureWidth: u32,   // 0
    textureHeight: u32,  // 4
    blurRadius: i32,     // 8
    sigma: f32,          // 12
    arrayLayer: u32,     // 16
    direction: u32,      // 20 — 0: horizontal, 1: vertical
    _pad0: u32,          // 24
    _pad1: u32,          // 28
};

@group(0) @binding(0) var inputTex: texture_2d<f32>;
@group(0) @binding(1) var outputTex: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> params: BlurParams;

@compute @workgroup_size(8, 8, 1)
fn blurCS(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = textureDimensions(inputTex);
    if (gid.x >= dims.x || gid.y >= dims.y) {
        return;
    }

    let texCoord = vec2<u32>(gid.x, gid.y);
    let center = textureLoad(inputTex, texCoord, 0);

    // 5-tap gaussian (linear-sampling variant) — weights identical to the
    // Metal blurCS2D kernel (2026-09-02 cross-platform alignment).
    let offset = select(vec2<i32>(1, 0), vec2<i32>(0, 1), params.direction == 1u);
    var sum = center * 0.2270270270;
    sum += textureLoad(inputTex, texCoord + vec2<u32>(offset), 0) * 0.3162162162;
    sum += textureLoad(inputTex, texCoord - vec2<u32>(offset), 0) * 0.3162162162;
    sum += textureLoad(inputTex, texCoord + 2u * vec2<u32>(offset), 0) * 0.070270270;
    sum += textureLoad(inputTex, texCoord - 2u * vec2<u32>(offset), 0) * 0.070270270;

    textureStore(outputTex, texCoord, sum);
}
