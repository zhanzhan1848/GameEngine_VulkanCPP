// Frozen fullscreen composite. Inputs include the actual half-float indirect
// writeback; no depth test or geometry re-rasterization excludes background.
@group(0) @binding(0) var sceneColor: texture_2d<f32>;
@group(0) @binding(1) var indirectColor: texture_2d<f32>;
@group(0) @binding(2) var volumeScatter: texture_2d<f32>;
@group(0) @binding(3) var ssrColor: texture_2d<f32>;
@group(0) @binding(4) var ormTexture: texture_2d<f32>;
@group(0) @binding(5) var albedoTexture: texture_2d<f32>;
@group(0) @binding(6) var ddgiSpecular: texture_2d<f32>;
@group(0) @binding(7) var linearClamp: sampler;
@group(0) @binding(8) var offscreenSpecular: texture_2d<f32>;
@group(0) @binding(9) var environmentSpecular: texture_2d<f32>;
@group(0) @binding(10) var<uniform> environmentPolicy: vec4<f32>;
@group(0) @binding(11) var environmentVisibility: texture_2d<f32>;
@group(0) @binding(12) var compositeOutput: texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8)
fn measured_fusion_composite(@builtin(global_invocation_id) id: vec3<u32>) {
    let size = textureDimensions(compositeOutput);
    if (any(id.xy >= size)) { return; }
    let uv = (vec2<f32>(id.xy) + .5) / vec2<f32>(size);
    let scene = textureSampleLevel(sceneColor, linearClamp, uv, 0).rgb;
    var indirect = textureSampleLevel(indirectColor, linearClamp, uv, 0).rgb;
    let volume = textureSampleLevel(volumeScatter, linearClamp, uv, 0);
    let ssr = textureSampleLevel(ssrColor, linearClamp, uv, 0);
    let orm = textureSampleLevel(ormTexture, linearClamp, uv, 0);
    let albedo = textureSampleLevel(albedoTexture, linearClamp, uv, 0).rgb;
    let specular = textureSampleLevel(ddgiSpecular, linearClamp, uv, 0);
    let offscreen = textureSampleLevel(offscreenSpecular, linearClamp, uv, 0);
    let environment = textureSampleLevel(environmentSpecular, linearClamp, uv, 0);
    let roughness = clamp(orm.g, .04, 1.);
    let noV = clamp(specular.a, .05, 1.);
    let f0 = mix(vec3<f32>(.04), albedo, orm.b);
    let r = roughness * vec4<f32>(-1., -.0275, -.572, .022) + vec4<f32>(1., .0425, 1.04, -.04);
    let a004 = min(r.x * r.x, exp2(-9.28 * noV)) * r.x + r.y;
    let ab = vec2<f32>(-1.04, 1.04) * a004 + r.zw;
    let glossy = specular.rgb * (f0 * ab.x + ab.y) * (1. - roughness * roughness) * .75;
    let ssrWeight = clamp(ssr.a, 0., 1.);
    let offscreenWeight = clamp(offscreen.a, 0., 1.);
    let ownership = environmentPolicy.x * clamp(environment.a, 0., 1.);
    var visibility = 1.;
    if (environmentPolicy.w > .5) {
        visibility = clamp(textureSampleLevel(environmentVisibility, linearClamp, uv, 0).r, 0., 1.);
    }
    let measured = environment.rgb * visibility * environmentPolicy.y;
    let fallback = mix(glossy, measured, ownership);
    var reflection = ssr.rgb + offscreen.rgb * offscreenWeight * (1. - ssrWeight)
                  + fallback * (1. - ssrWeight) * (1. - offscreenWeight);
    if (environmentPolicy.z > .5) {
        indirect = vec3<f32>(0);
        reflection = measured * ownership;
    }
    let fresnel = f0 + (vec3<f32>(1) - f0) * pow(1. - noV, 5.);
    let result = (scene + indirect * (1. - orm.b) * (vec3<f32>(1) - fresnel) + reflection) * volume.a + volume.rgb;
    textureStore(compositeOutput, id.xy, vec4<f32>(clamp(result, vec3<f32>(0), vec3<f32>(64)), 1));
}
