#include "IBL_Hammersley.wgsl"

@group(0) @binding(0) var envMap: texture_cube<f32>;
@group(0) @binding(1) var outputCube: texture_storage_2d_array<rgba16float, write>;
@group(0) @binding(2) var smp: sampler;

struct IrradianceParams {
    faceSize: u32,
    _pad0: u32,
    _pad1: u32,
    _pad2: u32,
};
@group(0) @binding(3) var<uniform> params: IrradianceParams;

@compute @workgroup_size(16, 16, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= params.faceSize || gid.y >= params.faceSize || gid.z >= 6u) { return; }

    let uv = (vec2<f32>(gid.xy) + 0.5) / vec2<f32>(f32(params.faceSize), f32(params.faceSize));
    let u = 2.0 * uv.x - 1.0;
    var v = -(2.0 * uv.y - 1.0);

    var N: vec3<f32>;
    switch (gid.z) {
        case 0u: { N = vec3<f32>(1.0, v, -u); }
        case 1u: { N = vec3<f32>(-1.0, v, u); }
        case 2u: { N = vec3<f32>(u, 1.0, -v); }
        case 3u: { N = vec3<f32>(u, -1.0, v); }
        case 4u: { N = vec3<f32>(u, v, 1.0); }
        case 5u: { N = vec3<f32>(-u, v, -1.0); }
        default: { N = vec3<f32>(0.0, 0.0, 1.0); }
    }
    N = normalize(N);

    var up = vec3<f32>(0.0, 1.0, 0.0);
    var right = cross(up, N);
    if (length(right) < 0.001) {
        up = vec3<f32>(1.0, 0.0, 0.0);
        right = cross(up, N);
    }
    right = normalize(right);
    up = normalize(cross(N, right));

    var irradiance = vec3<f32>(0.0);
    let sampleDelta = 0.025;
    var nrSamples = 0.0;

    var phi: f32 = 0.0;
    while (phi < 2.0 * PI) {
        var theta: f32 = 0.0;
        while (theta < 0.5 * PI) {
            let tangentSample = vec3<f32>(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            let sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            irradiance += textureSampleLevel(envMap, smp, sampleVec, 0.0).rgb * cos(theta) * sin(theta);
            nrSamples += 1.0;
            theta += sampleDelta;
        }
        phi += sampleDelta;
    }

    irradiance = PI * irradiance * (1.0 / nrSamples);
    textureStore(outputCube, gid.xy, gid.z, vec4<f32>(irradiance, 1.0));
}
