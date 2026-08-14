// SSAO.wgsl — Screen Space Ambient Occlusion with depth-based reconstruction

struct SSAOParams {
    invProj: mat4x4<f32>,
    proj: mat4x4<f32>,
    screenSize: vec4<f32>,  // x=width, y=height, z=1/width, w=1/height
    radius: f32,
    power: f32,
    sampleCount: u32,
    frameIndex: u32,
};

@group(0) @binding(0) var depthTex: texture_depth_2d;
@group(0) @binding(1) var aoOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> params: SSAOParams;

fn reconstructViewPos(uv: vec2<f32>, depth: f32) -> vec3<f32> {
    let ndcX = uv.x * 2.0 - 1.0;
    let ndcY = 1.0 - uv.y * 2.0;  // Y flip: WebGPU framebuffer Y=0 is top, clip Y=+1 is top
    let clipPos = vec4<f32>(ndcX, ndcY, depth, 1.0);
    let viewPos = params.invProj * clipPos;
    return viewPos.xyz / viewPos.w;
}

fn reconstructViewNormal(pixel: vec2<u32>, dims: vec2<u32>, uv: vec2<f32>, depth: f32) -> vec3<f32> {
    let texelSize = params.screenSize.zw;
    let px = clamp(i32(pixel.x), 1, i32(dims.x) - 2);
    let py = clamp(i32(pixel.y), 1, i32(dims.y) - 2);

    let depthLeft  = textureLoad(depthTex, vec2<u32>(u32(px - 1), u32(py)), 0);
    let depthRight = textureLoad(depthTex, vec2<u32>(u32(px + 1), u32(py)), 0);
    let depthUp    = textureLoad(depthTex, vec2<u32>(u32(px), u32(py - 1)), 0);
    let depthDown  = textureLoad(depthTex, vec2<u32>(u32(px), u32(py + 1)), 0);

    let pLeft   = reconstructViewPos(vec2<f32>(uv.x - texelSize.x, uv.y), depthLeft);
    let pRight  = reconstructViewPos(vec2<f32>(uv.x + texelSize.x, uv.y), depthRight);
    let pUp     = reconstructViewPos(vec2<f32>(uv.x, uv.y - texelSize.y), depthUp);
    let pDown   = reconstructViewPos(vec2<f32>(uv.x, uv.y + texelSize.y), depthDown);

    let dx = pRight - pLeft;
    let dy = pDown - pUp;
    // cross(dy, dx) gives camera-facing normal (+Z in view space)
    return normalize(cross(dy, dx));
}

fn hash(pos: vec2<u32>, frame: u32) -> f32 {
    var h = pos.x * 374761393u + pos.y * 668265263u + frame * 1274126177u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return f32(h) / 4294967295.0;
}

@compute @workgroup_size(8, 8, 1)
fn ssao_trace(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = vec2<u32>(params.screenSize.xy);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }

    let pixel = vec2<u32>(gid.x, gid.y);
    let uv = (vec2<f32>(pixel) + 0.5) / params.screenSize.xy;

    let depth = textureLoad(depthTex, pixel, 0);

    // Sky pixels: no occlusion
    if (depth >= 0.9999) {
        textureStore(aoOutput, pixel, vec4<f32>(1.0, 1.0, 1.0, 1.0));
        return;
    }

    let viewPos = reconstructViewPos(uv, depth);
    let viewNormal = reconstructViewNormal(pixel, dims, uv, depth);

    let radius = params.radius;
    let numSamples = i32(params.sampleCount);
    let rotationAngle = hash(pixel, params.frameIndex) * 6.283185;

    var occlusion = 0.0;

    // Build TBN basis
    var up = vec3<f32>(0.0, 1.0, 0.0);
    if (abs(viewNormal.y) > 0.99) {
        up = vec3<f32>(1.0, 0.0, 0.0);
    }
    let tangent = normalize(cross(viewNormal, up));
    let bitangent = cross(viewNormal, tangent);

    for (var i: i32 = 0; i < numSamples; i++) {
        let f_i = f32(i);
        let f_n = f32(numSamples);
        let goldenRatio = 1.618033988749;

        // Fibonacci hemisphere sampling
        let theta = acos(1.0 - f_i / f_n);
        let phi = 2.0 * 3.14159265 * f_i / goldenRatio + rotationAngle;

        let sinTheta = sin(theta);
        let cosTheta = cos(theta);

        let hemisphereDir = vec3<f32>(
            sinTheta * cos(phi),
            sinTheta * sin(phi),
            cosTheta
        );

        let sampleDir = tangent * hemisphereDir.x + bitangent * hemisphereDir.y + viewNormal * hemisphereDir.z;
        let samplePos = viewPos + sampleDir * radius;

        // Project sample position to screen using forward projection
        let sampleClip = params.proj * vec4<f32>(samplePos, 1.0);
        let sampleNDC = sampleClip.xy / sampleClip.w;
        // Undo the Y flip: screen UV Y = (1 - ndcY) / 2
        let sampleUV = vec2<f32>(
            sampleNDC.x * 0.5 + 0.5,
            0.5 - sampleNDC.y * 0.5
        );

        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
            continue;
        }

        let samplePixel = vec2<u32>(
            clamp(u32(sampleUV.x * f32(dims.x)), 0u, dims.x - 1u),
            clamp(u32(sampleUV.y * f32(dims.y)), 0u, dims.y - 1u)
        );
        let sampleDepth = textureLoad(depthTex, samplePixel, 0);

        // Skip sky samples
        if (sampleDepth >= 0.9999) { continue; }

        let sampleViewZ = reconstructViewPos(sampleUV, sampleDepth).z;

        // Range check: prevent far-away geometry from contributing
        let rangeCheck = smoothstep(0.0, 1.0, radius / abs(viewPos.z - sampleViewZ));

        // In view space, objects in front have more negative Z.
        // If sampled geometry is closer to camera (less negative Z) than sample point, it's occluded.
        if (sampleViewZ > samplePos.z + 0.001) {
            occlusion += rangeCheck;
        }
    }

    let ao = 1.0 - (occlusion / f32(numSamples)) * params.power;
    let finalAO = clamp(ao, 0.0, 1.0);
    textureStore(aoOutput, pixel, vec4<f32>(finalAO, finalAO, finalAO, 1.0));
}
