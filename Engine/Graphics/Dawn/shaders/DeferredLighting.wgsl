// DeferredLighting.wgsl — Phase 3c deferred PBR lighting compute shader.
//
// Reads the 5 MRT G-Buffer targets written by GBuffer.wgsl + the CSM shadow
// depth array + IBL maps, computes PBR parity with ForwardPBR.wgsl, and writes
// HDR linear color to the output storage texture. Velocity comes from RT4 and
// is blit'd separately to velocityTexture_ — this pass only owns color.
//
// Bindings (group 0):
//   0..3   G-Buffer RT0..RT3
//   4      shadowDepthTex (texture_depth_2d_array, 4 CSM cascades)
//   5..7   IBL irradiance/prefilter/BRDF-LUT
//   8      iblSampler (linear, non-comparison)
//   9      globalData uniform (GlobalShaderData)
//   10     lightBuffer uniform (ForwardLightBuffer)
//   11     outputTex (RGBA16F storage)
//
// BRDF/shadow logic copied from ForwardPBR.wgsl with one difference: no
// tangent-space normal mapping (deferred stores world-space normal in RT1).
// Inlined instead of #included — WGSL has no native include, same pattern as
// ForwardPBR duplicating CommonFunction.wgsl. Cleanup deferred to Phase 4.

struct GlobalShaderData {
    view: mat4x4<f32>,
    projection: mat4x4<f32>,
    invProjection: mat4x4<f32>,
    viewProjection: mat4x4<f32>,
    previousViewProjection: mat4x4<f32>,
    invViewProjection: mat4x4<f32>,
    cameraPositionAndViewWidth: vec4<f32>,
    cameraDirectionAndViewHeight: vec4<f32>,
    numDirectionalLights: u32,
    numPunctualLights: u32,
    deltaTime: f32,
    frameCount: f32,
    renderMode: u32,
    _pad0: u32,
    jitterOffset: vec2<f32>,
};

struct DirectionalLightParameters {
    viewProjections: array<mat4x4<f32>, 4>,
    splits: vec4<f32>,
    directionAndIntensity: vec4<f32>,
    colorAndShadow: vec4<f32>,
};

struct PunctualLightParameters {
    position: vec4<f32>,
    intensity: f32,
    _p0: f32, _p1: f32, _p2: f32,
    direction: vec4<f32>,
    range: f32,
    _p3: f32, _p4: f32, _p5: f32,
    color: vec4<f32>,
    cosUmbra: f32,
    _p6: f32, _p7: f32, _p8: f32,
    attenuation: vec4<f32>,
    cosPenumbra: f32,
    lightType: i32,
    shadowIndex: i32,
    _pad0: f32,
    viewProjection: mat4x4<f32>,
};

struct ForwardLightBuffer {
    directionalLightCount: u32,
    punctualLightCount: u32,
    _pad: vec2<u32>,
    directionalLights: array<DirectionalLightParameters, 4>,
    lights: array<PunctualLightParameters, 128>,
};

const PI: f32 = 3.141592653589793;

@group(0) @binding(0) var gbuffer0: texture_2d<f32>;
@group(0) @binding(1) var gbuffer1: texture_2d<f32>;
@group(0) @binding(2) var gbuffer2: texture_2d<f32>;
@group(0) @binding(3) var gbuffer3: texture_2d<f32>;
@group(0) @binding(4) var shadowDepthTex: texture_depth_2d_array;
@group(0) @binding(5) var irradianceMap: texture_cube<f32>;
@group(0) @binding(6) var prefilterMap: texture_cube<f32>;
@group(0) @binding(7) var brdfLUT: texture_2d<f32>;
@group(0) @binding(8) var iblSampler: sampler;
@group(0) @binding(9) var<uniform> globalData: GlobalShaderData;
@group(0) @binding(10) var<uniform> lightBuffer: ForwardLightBuffer;
@group(0) @binding(11) var outputTex: texture_storage_2d<rgba16float, write>;

// === BRDF helpers (mirrors ForwardPBR.wgsl:162-185) ===

fn fresnelSchlick(cosTheta: f32, F0: vec3<f32>) -> vec3<f32> {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

fn fresnelSchlickRoughness(cosTheta: f32, F0: vec3<f32>, roughness: f32) -> vec3<f32> {
    return F0 + (max(vec3<f32>(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

fn distributionGGX(N: vec3<f32>, H: vec3<f32>, a: f32) -> f32 {
    let a2 = a * a;
    let NdotH = max(dot(N, H), 0.0);
    let d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + 0.0001);
}

fn geometrySchlickGGX(NdotV: f32, a: f32) -> f32 {
    let k = (a + 1.0) * (a + 1.0) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

fn geometrySmith(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>, a: f32) -> f32 {
    return geometrySchlickGGX(max(dot(N, V), 0.0), a) *
           geometrySchlickGGX(max(dot(N, L), 0.0), a);
}

// === CSM shadow sampling (mirrors ForwardPBR.wgsl:191-228) ===

fn selectCascade(viewZ: f32) -> i32 {
    let splits = lightBuffer.directionalLights[0].splits;
    for (var i: i32 = 0; i < 4; i++) {
        if (viewZ < splits[i]) { return i; }
    }
    return 3;
}

fn sampleShadowPCF(worldPos: vec3<f32>, viewZ: f32, N: vec3<f32>, lightDir: vec3<f32>) -> f32 {
    let cascadeIdx = selectCascade(viewZ);
    let lightVP = lightBuffer.directionalLights[0].viewProjections[cascadeIdx];
    let lightClip = lightVP * vec4<f32>(worldPos, 1.0);
    let lightNDC = lightClip.xyz / lightClip.w;

    if (lightNDC.x < -1.0 || lightNDC.x > 1.0 || lightNDC.y < -1.0 || lightNDC.y > 1.0
        || lightNDC.z < 0.0 || lightNDC.z > 1.0) {
        return 1.0;
    }

    let shadowUV = clamp(vec2<f32>(lightNDC.x * 0.5 + 0.5, 1.0 - (lightNDC.y * 0.5 + 0.5)),
                         vec2<f32>(0.001, 0.001), vec2<f32>(0.999, 0.999));
    let shadowZ = lightNDC.z;

    let texSize = vec2<f32>(textureDimensions(shadowDepthTex));
    let bias = max(0.005 * (1.0 - dot(N, lightDir)), 0.001);

    let baseCoord = vec2<i32>(shadowUV * texSize);
    var shadow = 0.0;
    var count = 0;
    for (var x = -1; x <= 1; x++) {
        for (var y = -1; y <= 1; y++) {
            let coord = clamp(baseCoord + vec2<i32>(x, y), vec2<i32>(0, 0), vec2<i32>(i32(texSize.x) - 1, i32(texSize.y) - 1));
            let depth = textureLoad(shadowDepthTex, coord, cascadeIdx, 0);
            shadow += select(0.0, 1.0, depth > shadowZ - bias);
            count++;
        }
    }
    return shadow / f32(count);
}

// === Main compute entry point — 8x8 threadgroup, mirrors SSAO/SSR pattern ===

@compute @workgroup_size(8, 8, 1)
fn deferred_lighting_cs(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = textureDimensions(gbuffer0);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }

    let pixel = vec2<u32>(gid.x, gid.y);

    // Read G-Buffer (textureLoad since we have integer pixel coords).
    let g0 = textureLoad(gbuffer0, pixel, 0);
    let g1 = textureLoad(gbuffer1, pixel, 0);
    let g2 = textureLoad(gbuffer2, pixel, 0);
    let g3 = textureLoad(gbuffer3, pixel, 0);

    let worldPos = g0.xyz;
    let normalEncoded = g1.xyz;
    // RT1.w stores curClip.w which for perspective = -viewPos.z (positive).
    // That matches ForwardPBR's viewZ computation (viewPos = View * worldPos;
    // viewZ = -viewPos.z), so we can use g1.w directly for cascade selection.
    let viewZ = g1.w;
    let albedo = g2.rgb;
    let metallic = g2.w;
    let ao = g3.r;
    let roughness = max(g3.g, 0.04);

    // Decode world-space normal (no tangent-space normal mapping in deferred).
    let N = normalize(normalEncoded * 2.0 - 1.0);
    let V = normalize(globalData.cameraPositionAndViewWidth.xyz - worldPos);

    let F0 = mix(vec3<f32>(0.04, 0.04, 0.04), albedo, metallic);
    let NdotV = max(dot(N, V), 0.001);

    var color = vec3<f32>(0.0);
    var iblShadow: f32 = 1.0;

    // === Directional light (first only) with CSM shadow ===
    if (lightBuffer.directionalLightCount > 0u) {
        let light = lightBuffer.directionalLights[0];
        let L = normalize(-light.directionAndIntensity.xyz);
        let H = normalize(V + L);

        let shadowFactor = sampleShadowPCF(worldPos, viewZ, N, L);
        let radiance = light.colorAndShadow.rgb * light.directionAndIntensity.w * shadowFactor;

        let NdotL = max(dot(N, L), 0.0);

        let fresnel = fresnelSchlick(max(dot(H, V), 0.0), F0);
        let ndf = distributionGGX(N, H, roughness);
        let geometry = geometrySmith(N, V, L, roughness);

        let numerator = ndf * geometry * fresnel;
        let denominator = 4.0 * NdotV * NdotL + 0.0001;
        let specular = numerator / denominator;

        let kS = fresnel;
        let kD = (vec3<f32>(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);

        let Lo = (kD * albedo / PI + specular) * radiance * NdotL;
        color = color + Lo;
        iblShadow = mix(0.35, 1.0, shadowFactor);
    }

    // === Punctual lights (point + spot) ===
    for (var i: u32 = 0u; i < lightBuffer.punctualLightCount; i++) {
        let plight = lightBuffer.lights[i];
        let toLight = plight.position.xyz - worldPos;
        let distance = length(toLight);
        if (distance > plight.range) { continue; }

        let rangeFade = 1.0 - smoothstep(plight.range * 0.6, plight.range, distance);
        let L = toLight / distance;
        let H = normalize(V + L);

        let att = plight.attenuation.xyz;
        let attenuation = 1.0 / (att.x + att.y * distance + att.z * distance * distance) * rangeFade;

        var spotAtt = 1.0;
        if (plight.lightType == 2) {
            let theta = dot(L, normalize(-plight.direction.xyz));
            let epsilon = plight.cosUmbra - plight.cosPenumbra;
            spotAtt = clamp((theta - plight.cosPenumbra) / max(epsilon, 0.001), 0.0, 1.0);
        }

        let radiance = plight.color.xyz * plight.intensity * attenuation * spotAtt;
        let NdotL = max(dot(N, L), 0.0);

        let fresnel = fresnelSchlick(max(dot(H, V), 0.0), F0);
        let ndf = distributionGGX(N, H, roughness);
        let geometry = geometrySmith(N, V, L, roughness);

        let numerator = ndf * geometry * fresnel;
        let denominator = 4.0 * NdotV * NdotL + 0.0001;
        let specular = numerator / denominator;

        let kS = fresnel;
        let kD = (vec3<f32>(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);

        let Lo = (kD * albedo / PI + specular) * radiance * NdotL;
        color = color + Lo;
    }

    // === IBL ambient (always active — ForwardPBR applies it when iblActive
    // which is renderMode >= 2; Deferred is mode 5 so always on) ===
    {
        let kS_ibl = fresnelSchlickRoughness(NdotV, F0, roughness);
        let kD_ibl = (vec3<f32>(1.0) - kS_ibl) * (1.0 - metallic);
        let irradiance = textureSampleLevel(irradianceMap, iblSampler, N, 0.0).rgb;
        let diffuseIBL = irradiance * albedo;
        let R = reflect(-V, N);
        let prefilteredColor = textureSampleLevel(prefilterMap, iblSampler, R, roughness * 4.0).rgb;
        let brdf = textureSampleLevel(brdfLUT, iblSampler, vec2<f32>(NdotV, roughness), 0.0);
        let specularIBL = prefilteredColor * (kS_ibl * brdf.r + brdf.g);
        color = color + (kD_ibl * diffuseIBL + specularIBL) * ao * iblShadow * 0.6;
    }

    // ForwardPBR applies a contrast pow + gamma before ToneMapping. We mirror
    // it so deferred parity holds — ToneMapping then double-tonemaps both
    // paths equally. Removing this is a Phase 4 cleanup.
    color = pow(max(color, vec3<f32>(0.0, 0.0, 0.0)), vec3<f32>(1.3, 1.3, 1.3));
    color = pow(color, vec3<f32>(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));

    textureStore(outputTex, pixel, vec4<f32>(color, 1.0));
}
