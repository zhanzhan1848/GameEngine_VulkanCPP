// Frozen FusionIndirect: full-resolution diffuse radiance, stored in RGBA16F
// before final composition. AO is deliberately not multiplied a second time.
@group(0) @binding(0) var ssgiColor: texture_2d<f32>;
@group(0) @binding(1) var ddgiColor: texture_2d<f32>;
@group(0) @binding(2) var spgiColor: texture_2d<f32>;
@group(0) @binding(3) var albedoTexture: texture_2d<f32>;
@group(0) @binding(4) var ssaoTexture: texture_2d<f32>;
@group(0) @binding(5) var linearClamp: sampler;
@group(0) @binding(6) var<uniform> gainMode: vec4<f32>;
@group(0) @binding(7) var indirectOutput: texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8)
fn measured_fusion_indirect(@builtin(global_invocation_id) id: vec3<u32>) {
    let size = textureDimensions(indirectOutput);
    if (any(id.xy >= size)) { return; }
    let uv = (vec2<f32>(id.xy) + .5) / vec2<f32>(size);
    let ssgi = textureSampleLevel(ssgiColor, linearClamp, uv, 0);
    let ddgi = textureSampleLevel(ddgiColor, linearClamp, uv, 0).rgb;
    let spgi = textureSampleLevel(spgiColor, linearClamp, uv, 0);
    let albedo = clamp(textureSampleLevel(albedoTexture, linearClamp, uv, 0).rgb, vec3<f32>(0), vec3<f32>(1));
    let confidence = clamp(spgi.a, 0., .9);
    let ssgiConfidence = clamp(1. - clamp(ssgi.a, 0., 30.) / 30. * .7, .3, 1.);
    var gi = ddgi;
    var ssgiBlend = .25;
    if (gainMode.y < 1.5) {
        gi = mix(ddgi, spgi.rgb, confidence);
        ssgiBlend = mix(.25, .15, confidence);
    }
    gi += ssgi.rgb * ssgiConfidence * ssgiBlend;
    let indirect = albedo * gi * gainMode.x * .31830988;
    textureStore(indirectOutput, id.xy, vec4<f32>(indirect, 1));
}
