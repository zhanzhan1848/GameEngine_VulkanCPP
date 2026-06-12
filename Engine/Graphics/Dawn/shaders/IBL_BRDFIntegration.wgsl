#include "IBL_Hammersley.wgsl"

@group(0) @binding(0) var outputLUT: texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(16, 16, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = vec2<u32>(textureDimensions(outputLUT));
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }

    let NdotV = (f32(gid.x) + 0.5) / f32(dims.x);
    let roughness = (f32(gid.y) + 0.5) / f32(dims.y);

    let V = vec3<f32>(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    let N = vec3<f32>(0.0, 0.0, 1.0);

    var A = 0.0;
    var B = 0.0;

    let SAMPLE_COUNT = 1024u;
    for (var i = 0u; i < SAMPLE_COUNT; i++) {
        let xi = hammersley(i, SAMPLE_COUNT);
        let h = importanceSampleGGX(xi, N, roughness);
        let L = normalize(2.0 * dot(V, h) * h - V);

        let NdotL = max(L.z, 0.0);
        let NdotH = max(h.z, 0.0);
        let VdotH = max(dot(V, h), 0.0);

        if (NdotL > 0.0) {
            let g = geometrySmith_IBL(N, V, L, roughness);
            let gVis = (g * VdotH) / (NdotH * NdotV + 0.0001);
            let Fc = pow(1.0 - VdotH, 5.0);

            A += (1.0 - Fc) * gVis;
            B += Fc * gVis;
        }
    }

    A /= f32(SAMPLE_COUNT);
    B /= f32(SAMPLE_COUNT);

    textureStore(outputLUT, gid.xy, vec4<f32>(A, B, 0.0, 0.0));
}
