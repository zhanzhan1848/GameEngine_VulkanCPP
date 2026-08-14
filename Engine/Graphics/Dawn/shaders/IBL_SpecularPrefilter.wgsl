#include "IBL_Hammersley.wgsl"

@group(0) @binding(0) var envMap: texture_cube<f32>;
@group(0) @binding(1) var outputCube: texture_storage_2d_array<rgba16float, write>;
@group(0) @binding(2) var smp: sampler;

struct PrefilterParams {
    faceSize: u32,
    _pad0: u32,
    roughness: f32,
    srcResolution: f32,
};
@group(0) @binding(3) var<uniform> params: PrefilterParams;

@compute @workgroup_size(16, 16, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= params.faceSize || gid.y >= params.faceSize || gid.z >= 6u) { return; }

    let uv = (vec2<f32>(gid.xy) + 0.5) / vec2<f32>(f32(params.faceSize), f32(params.faceSize));
    let u = 2.0 * uv.x - 1.0;
    let v = -(2.0 * uv.y - 1.0);

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

    let R = N;
    let V = R;

    let SAMPLE_COUNT = 1024u;
    var totalWeight = 0.0;
    var prefilteredColor = vec3<f32>(0.0);

    for (var i = 0u; i < SAMPLE_COUNT; i++) {
        let xi = hammersley(i, SAMPLE_COUNT);
        let h = importanceSampleGGX(xi, N, params.roughness);

        // Rotate H from tangent space to world space
        let up = select(vec3<f32>(0.0, 0.0, 1.0), vec3<f32>(1.0, 0.0, 0.0), abs(N.z) < 0.999);
        let tangentX = normalize(cross(up, N));
        let tangentY = cross(N, tangentX);
        let hWorld = normalize(tangentX * h.x + tangentY * h.y + N * h.z);

        let L = normalize(2.0 * dot(V, hWorld) * hWorld - V);
        let NdotL = max(dot(N, L), 0.0);

        if (NdotL > 0.0) {
            let d = distributionGGX(N, hWorld, params.roughness);
            let NdotH = max(dot(N, hWorld), 0.0);
            let HdotV = max(dot(hWorld, V), 0.0);
            let pdf = d * NdotH / (4.0 * HdotV + 0.0001);

            let saTexel = 4.0 * PI / (6.0 * params.srcResolution * params.srcResolution);
            let saSample = 1.0 / (f32(SAMPLE_COUNT) * pdf + 0.0001);
            let mipLevel = select(0.0, 0.5 * log2(saSample / saTexel), params.roughness > 0.0);

            prefilteredColor += textureSampleLevel(envMap, smp, L, mipLevel).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor = prefilteredColor / totalWeight;
    textureStore(outputCube, gid.xy, gid.z, vec4<f32>(prefilteredColor, 1.0));
}
