// DeferredLighting_Meshlet.wgsl — Phase N2 deferred PBR for meshlet GBuffer.
//
// Reads the 4-RT GBuffer written by Nanite/GPUDrivenDraw.wgsl + the meshlet
// depth texture (sampleable) + CSM shadow array + IBL, evaluates PBR parity
// with DeferredLighting.wgsl, and writes HDR linear color to the output
// storage texture.
//
// Differences vs DeferredLighting.wgsl (5-RT GBuffer):
//   - Meshlet GBuffer has no WorldPos RT → reconstruct from depth texture
//     using invViewProjection.
//   - Meshlet GBuffer has no linearDepth RT → compute viewZ from worldPos.
//   - Layout: RT0=albedo, RT1=normal, RT2=orm(o,r,m), RT3=velocity.
//   - Meshlet RT formats are BGRA8_UNorm (albedo not sRGB); albedo textures
//     already decode to linear via the texture array, so values are linear
//     when written (slight precision loss vs RGBA16F but fine for Sponza).
//
// Bindings (group 0):
//   0..3 meshlet GBuffer RT0..RT3
//   4      depthTex (D32 sampleable, for worldPos reconstruction)
//   5      shadowDepthTex (texture_depth_2d_array, 4 CSM cascades)
//   6..8   IBL irradiance/prefilter/BRDF-LUT
//   9      iblSampler (linear, non-comparison)
//  10      globalData uniform (GlobalShaderData)
//  11      lightBuffer uniform (ForwardLightBuffer)
//  12      outputTex (RGBA16F storage)

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
    enableIBL: u32,   // 0 = skip IBL ambient term (meshlet NoIBL mode), 1 = apply
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

@group(0) @binding(0) var gbufferAlbedo: texture_2d<f32>;
@group(0) @binding(1) var gbufferNormal: texture_2d<f32>;
@group(0) @binding(2) var gbufferOrm: texture_2d<f32>;
@group(0) @binding(3) var gbufferVelocity: texture_2d<f32>;
@group(0) @binding(4) var depthTex: texture_depth_2d;
@group(0) @binding(5) var shadowDepthTex: texture_depth_2d_array;
@group(0) @binding(6) var irradianceMap: texture_cube<f32>;
@group(0) @binding(7) var prefilterMap: texture_cube<f32>;
@group(0) @binding(8) var brdfLUT: texture_2d<f32>;
@group(0) @binding(9) var iblSampler: sampler;
@group(0) @binding(10) var<uniform> globalData: GlobalShaderData;
@group(0) @binding(11) var<uniform> lightBuffer: ForwardLightBuffer;
@group(0) @binding(12) var outputTex: texture_storage_2d<rgba16float, write>;

// === BRDF helpers (mirrors DeferredLighting.wgsl:90-113) ===

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

// ACES filmic tonemap (Narkowicz 2015 approximation). Compresses HDR
// highlights smoothly instead of clipping them — fixes the washed-out
// whites that appeared after IBL was reduced to 0.2 and the dynamic
// range stretched. Replaces the pow(1.3)+gamma chain that effectively
// brightened midtones (1.3 * 1/2.2 ≈ 0.59 = gamma-up, less contrast).
fn tonemapACES(s: vec3<f32>) -> vec3<f32> {
    let a = 2.51; let b = 0.03; let c = 2.43; let d = 0.59; let e = 0.14;
    return clamp((s * (a * s + b)) / (s * (c * s + d) + e),
                 vec3<f32>(0.0, 0.0, 0.0), vec3<f32>(1.0, 1.0, 1.0));
}

// === CSM shadow sampling (mirrors DeferredLighting.wgsl:117-155) ===

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

// Reconstruct world-space position from depth + UV. WebGPU depth is in [0,1]
// with Y down (fragment coords). NDC is X=[-1,1] right, Y=[-1,1] up, Z=[0,1]
// forward. The invViewProjection uniform is provided by GlobalShaderData.
fn reconstructWorldPos(pixel: vec2<u32>, depth: f32, dims: vec2<u32>) -> vec3<f32> {
    let uv = vec2<f32>(f32(pixel.x) + 0.5, f32(pixel.y) + 0.5) / vec2<f32>(dims);
    let ndc = vec3<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth);
    let clip = vec4<f32>(ndc, 1.0);
    let world = globalData.invViewProjection * clip;
    return world.xyz / world.w;
}

// === Main compute entry point — 8x8 threadgroup, mirrors SSAO/SSR pattern ===

@compute @workgroup_size(8, 8, 1)
fn deferred_lighting_meshlet_cs(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = textureDimensions(gbufferAlbedo);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }

    let pixel = vec2<u32>(gid.x, gid.y);

    // Read meshlet GBuffer (textureLoad since we have integer pixel coords).
    let gAlbedo = textureLoad(gbufferAlbedo, pixel, 0);
    let gNormal = textureLoad(gbufferNormal, pixel, 0);
    let gOrm = textureLoad(gbufferOrm, pixel, 0);

    let depth = textureLoad(depthTex, pixel, 0);

    // Sky pixels: write a sentinel that encodes camera X position so we can
    // verify deferred lighting reads fresh uniforms each frame. If sky color
    // shifts when camera moves, deferred is fresh; if static, uniforms are
    // stale or compute isn't dispatching. Frac ensures [0,1] regardless of
    // camera coords; small bias so 0,0,0 isn't mistaken for cleared black.
    if (depth >= 0.99999) {
        let camX = globalData.cameraPositionAndViewWidth.x;
        let camY = globalData.cameraPositionAndViewWidth.y;
        let camZ = globalData.cameraPositionAndViewWidth.z;
        let r = fract(camX * 0.1 + 0.1);
        let g = fract(camY * 0.1 + 0.2);
        let b = fract(camZ * 0.1 + 0.3);
        textureStore(outputTex, pixel, vec4<f32>(r, g, b, 1.0));
        return;
    }

    let albedo = gAlbedo.rgb;
    let occlusion = gOrm.r;
    let roughness = max(gOrm.g, 0.04);
    let metallic = gOrm.b;

    // Decode world-space normal (encoded as N*0.5+0.5 in GPUDrivenDraw.wgsl).
    let N = normalize(gNormal.rgb * 2.0 - 1.0);

    // Reconstruct worldPos + viewZ for cascade selection + shadow sampling.
    let worldPos = reconstructWorldPos(pixel, depth, dims);
    let viewPos = globalData.view * vec4<f32>(worldPos, 1.0);
    let viewZ = -viewPos.z;

    let V = normalize(globalData.cameraPositionAndViewWidth.xyz - worldPos);

    let F0 = mix(vec3<f32>(0.04, 0.04, 0.04), albedo, metallic);
    let NdotV = max(dot(N, V), 0.001);

    var color = vec3<f32>(0.0);
    var iblShadow: f32 = 1.0;

    // Mode 7 (MeshletNoIBL) is intentionally unlit — flat albedo pass-through
    // so debug visualization (hash colors via V key) reads cleanly and the
    // scene reads as "in shadow". Mode 8 (Meshlet) takes the full PBR path.
    if (globalData.renderMode == 8u) {
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

        // === IBL ambient (attenuated) ===
        // Mode 8 is the A/B counterpart to Mode 7 (MeshletNoIBL): direct + punctual
        // PBR dominates, IBL provides only a faint ambient lift so shadowed areas
        // aren't pure black. Drop the multiplier to make the IBL contribution
        // visibly secondary vs the direct shading. Mirror ForwardPBR's structure
        // but at lower intensity — exposes IBL's role without letting it wash
        // out the directional lighting.
        if (globalData.enableIBL != 0u) {
            let kS_ibl = fresnelSchlickRoughness(NdotV, F0, roughness);
            let kD_ibl = (vec3<f32>(1.0) - kS_ibl) * (1.0 - metallic);
            let irradiance = textureSampleLevel(irradianceMap, iblSampler, N, 0.0).rgb;
            let diffuseIBL = irradiance * albedo;
            let R = reflect(-V, N);
            let prefilteredColor = textureSampleLevel(prefilterMap, iblSampler, R, roughness * 4.0).rgb;
            let brdf = textureSampleLevel(brdfLUT, iblSampler, vec2<f32>(NdotV, roughness), 0.0);
            let specularIBL = prefilteredColor * (kS_ibl * brdf.r + brdf.g);
            let iblStrength: f32 = 0.2;
            color = color + (kD_ibl * diffuseIBL + specularIBL) * occlusion * iblShadow * iblStrength;
        }

        // ForwardPBR/DeferredLighting apply pow(1.3)+gamma before ToneMapping.
        // Meshlet path diverged: after IBL reduction the pow chain was
        // washing out highlights (net gamma-up, not contrast), so we apply
        // ACES filmic here instead. Exposure lift compensates for ACES's
        // midtone darkening — direct lighting reads at full strength while
        // highlights still compress smoothly. ToneMap still runs downstream
        // and accepts the pre-compressed range cleanly.
        let exposure: f32 = 1.8;
        color = tonemapACES(max(color * exposure, vec3<f32>(0.0, 0.0, 0.0)));
    } else {
        // Mode 7: albedo * AO only — dark, unlit, in-shadow look. Skip gamma/tonemap
        // so hashed debug colors and texture albedos pass through cleanly.
        color = albedo * occlusion;
    }

    textureStore(outputTex, pixel, vec4<f32>(color, 1.0));
}
