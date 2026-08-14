// BlurPass.wgsl — Simple separable Gaussian blur (placeholder)
// Used by ForwardRenderer's VSM shadow blur

struct BlurParams {
    direction: u32,     // 0=horizontal, 1=vertical
    mipLevel: u32,
    _pad0: u32,
    _pad1: u32,
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

    // Simple 5-tap Gaussian blur
    let offset = select(vec2<i32>(1, 0), vec2<i32>(0, 1), params.direction == 1u);
    var sum = center * 0.2270270270;
    sum += textureLoad(inputTex, texCoord + vec2<u32>(offset), 0) * 0.3162162162;
    sum += textureLoad(inputTex, texCoord - vec2<u32>(offset), 0) * 0.3162162162;
    sum += textureLoad(inputTex, texCoord + 2u * vec2<u32>(offset), 0) * 0.070270270;
    sum += textureLoad(inputTex, texCoord - 2u * vec2<u32>(offset), 0) * 0.070270270;

    textureStore(outputTex, texCoord, sum);
}
