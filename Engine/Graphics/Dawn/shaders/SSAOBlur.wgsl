// SSAOBlur.wgsl — 5x5 bilateral blur for SSAO

struct BlurParams {
    screenSize: vec4<f32>,  // x=width, y=height, z=1/width, w=1/height
};

@group(0) @binding(0) var aoInput: texture_2d<f32>;
@group(0) @binding(1) var depthTex: texture_depth_2d;
@group(0) @binding(2) var aoBlurred: texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var<uniform> params: BlurParams;

const SIGMA_DEPTH: f32 = 0.002;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = vec2<u32>(params.screenSize.xy);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }

    let pixel = vec2<u32>(gid.x, gid.y);
    let centerDepth = textureLoad(depthTex, pixel, 0);

    var aoSum = 0.0;
    var weightSum = 0.0;

    // 5x5 bilateral filter
    for (var dy: i32 = -2; dy <= 2; dy++) {
        for (var dx: i32 = -2; dx <= 2; dx++) {
            let nx = i32(pixel.x) + dx;
            let ny = i32(pixel.y) + dy;

            if (nx < 0 || nx >= i32(dims.x) || ny < 0 || ny >= i32(dims.y)) {
                continue;
            }

            let nPixel = vec2<u32>(u32(nx), u32(ny));

            // Spatial gaussian weight
            let spatialDist = f32(dx * dx + dy * dy);
            let spatialWeight = exp(-spatialDist / 4.0);

            // Depth bilateral weight — preserve edges
            let neighborDepth = textureLoad(depthTex, nPixel, 0);
            let depthDiff = abs(neighborDepth - centerDepth);
            let depthWeight = exp(-depthDiff * depthDiff / (SIGMA_DEPTH * SIGMA_DEPTH));

            let weight = spatialWeight * depthWeight;

            let aoValue = textureLoad(aoInput, nPixel, 0).r;
            aoSum += aoValue * weight;
            weightSum += weight;
        }
    }

    let ao = select(aoSum / weightSum, 1.0, weightSum < 0.001);
    textureStore(aoBlurred, pixel, vec4<f32>(ao, ao, ao, 1.0));
}
