// DeferredLighting.wgsl — ForwardScene variant of Forward/DeferredLighting.metal.
//
// Two entry points share a pipeline:
//   vertexMain            — full-screen triangle (matches Metal vertexMain)
//   fragmentLighting_v3   — deferred PBR direct + IBL + shadow (HDR linear)
//   fragmentBlit          — ACES tone map + gamma
//
// Pipeline layout (Dawn/WGSL): single descriptor set at group(0) for lighting,
// blit pipeline uses its own 1-binding set.
//
// Lighting bindings (group 0):
//   0   ViewData uniform
//   1   SceneData uniform
//   2   gbuffer albedo  (BGRA8)
//   3   gbuffer normal  (RGBA16F, encoded *0.5+0.5)
//   4   gbuffer orm     (r=ao, g=roughness, b=metallic)
//   5   gbuffer depth   (texture_depth_2d, sampleable)
//   6   shadow_map_[0]  (R32_Float, pre-filtered visibility)
//   7   shadow_map_[1]  (R32_Float, second cascade)
//   8   irradiance_cube (texture_cube)
//   9   prefilter_cube  (texture_cube)
//  10   brdf_lut        (texture_2d)
//  11   default_sampler (linear)
//  12   brdf_sampler    (linear)

struct ViewData {
    viewProjection: mat4x4<f32>,
    invViewProjection: mat4x4<f32>,
    previousViewProjection: mat4x4<f32>,
};

struct SceneData {
    model: mat4x4<f32>,
    lightPos: vec4<f32>,
    lightColor: vec4<f32>,
    reflectionPlane: vec4<f32>,
    reflectionPlane2: vec4<f32>,
    reflectionPlane3: vec4<f32>,
    previousModel: mat4x4<f32>,
    jitter: vec2<f32>,
    previousJitter: vec2<f32>,
    time: f32,
    _timePad: f32,
    viewPos: vec4<f32>,
    shadowMatrix0: mat4x4<f32>,
    shadowMatrix1: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> viewData: ViewData;
@group(0) @binding(1) var<uniform> sceneData: SceneData;

@group(0) @binding(2) var albedoTex: texture_2d<f32>;
@group(0) @binding(3) var normalTex: texture_2d<f32>;
@group(0) @binding(4) var ormTex: texture_2d<f32>;
@group(0) @binding(5) var depthTex: texture_depth_2d;
@group(0) @binding(6) var shadowMap0: texture_depth_2d;
@group(0) @binding(7) var shadowMap1: texture_depth_2d;
@group(0) @binding(8) var irradianceMap: texture_cube<f32>;
@group(0) @binding(9) var prefilterMap: texture_cube<f32>;
@group(0) @binding(10) var brdfLUT: texture_2d<f32>;
@group(0) @binding(11) var defaultSampler: sampler;
@group(0) @binding(12) var brdfSampler: sampler;

const PI: f32 = 3.141592653589793;

// === BRDF helpers (mirror ForwardPBR.wgsl:162-185) ===

fn fresnelSchlick(cosTheta: f32, F0: vec3<f32>) -> vec3<f32> {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

fn fresnelSchlickRoughness(cosTheta: f32, F0: vec3<f32>, roughness: f32) -> vec3<f32> {
    let oneMinusR = vec3<f32>(1.0 - roughness, 1.0 - roughness, 1.0 - roughness);
    return F0 + (max(oneMinusR, F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
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

// === Tone mapping (matches DeferredLighting.metal:65-78) ===

fn ACESFilm(x: vec3<f32>) -> vec3<f32> {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3<f32>(0.0), vec3<f32>(1.0));
}

fn toneMap(color: vec3<f32>) -> vec3<f32> {
    var c = color * 1.2;  // exposure
    c = ACESFilm(c);
    return pow(c, vec3<f32>(1.0 / 2.2));
}

// === Vertex shader (full-screen triangle) ===

struct VSOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vertexMain(@builtin(vertex_index) vid: u32) -> VSOut {
    // Full-screen triangle: two corners extended off-screen to cover the viewport.
    // UV convention matches Metal (origin top-left): uv.y = 1 - ndc.y * 0.5 + 0.5.
    var positions: array<vec4<f32>, 3>;
    positions[0] = vec4<f32>(-1.0, -1.0, 0.0, 1.0);
    positions[1] = vec4<f32>(-1.0,  3.0, 0.0, 1.0);
    positions[2] = vec4<f32>( 3.0, -1.0, 0.0, 1.0);

    var uvs: array<vec2<f32>, 3>;
    uvs[0] = vec2<f32>(0.0, 1.0);
    uvs[1] = vec2<f32>(0.0, -1.0);
    uvs[2] = vec2<f32>(2.0, 1.0);

    var out: VSOut;
    out.position = positions[vid];
    out.uv = uvs[vid];
    return out;
}

// === Lighting shader (fragmentLighting_v3) ===

struct LightingOut {
    @location(0) color: vec4<f32>,
};

@fragment
fn fragmentLighting_v3(in: VSOut) -> LightingOut {
    var out: LightingOut;
    let uv = in.uv;

    // Depth texture dimensions drive the load coordinates.
    let depthSize = textureDimensions(depthTex);
    let iuv = vec2<i32>(vec2<f32>(depthSize) * uv);
    let depth = textureLoad(depthTex, iuv, 0);

    // Background (sky) pass-through: keep black so blit/skybox can override.
    if (depth >= 1.0) {
        out.color = vec4<f32>(0.0, 0.0, 0.0, 1.0);
        return out;
    }

    // Reconstruct world position from depth + invViewProjection.
    // WebGPU framebuffer uv.y is top-down (0=top); NDC.y is bottom-up.
    let ndcXY = vec2<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    let ndc = vec4<f32>(ndcXY, depth, 1.0);
    let worldPosH = viewData.invViewProjection * ndc;
    let worldPos = worldPosH.xyz / worldPosH.w;

    // Sample GBuffer.
    let albedo = textureSampleLevel(albedoTex, defaultSampler, uv, 0.0).rgb;
    let normalEnc = textureSampleLevel(normalTex, defaultSampler, uv, 0.0).rgb;
    let N = normalize(normalEnc * 2.0 - 1.0);
    let orm = textureSampleLevel(ormTex, defaultSampler, uv, 0.0).rgb;
    let ao = orm.r;
    let roughness = max(orm.g, 0.04);
    let metallic = orm.b;

    let V = normalize(sceneData.viewPos.xyz - worldPos);
    let L = normalize(sceneData.lightPos.xyz);
    let H = normalize(V + L);

    // Direct lighting — Cook-Torrance BRDF.
    var F0 = vec3<f32>(0.04, 0.04, 0.04);
    F0 = mix(F0, albedo, vec3<f32>(metallic, metallic, metallic));

    let NdotL = max(dot(N, L), 0.0);
    let NdotV = max(dot(N, V), 0.001);

    let NDF = distributionGGX(N, H, roughness);
    let G = geometrySmith(N, V, L, roughness);
    let F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    let numerator = NDF * G * F;
    let denominator = 4.0 * NdotV * NdotL + 0.0001;
    let specular = numerator / denominator;

    let kS = F;
    var kD = vec3<f32>(1.0) - kS;
    kD = kD * (1.0 - metallic);

    let diffuse = kD * albedo / PI;
    var Lo = (diffuse + specular) * sceneData.lightColor.rgb * NdotL;

    // Shadow: WASM skips shadow pass — shadowMap0 stays at cleared depth (1.0).
    // Static use of a depth texture requires NonFiltering sampler; the lighting
    // set's sampler bindings are Filtering, so we skip the lookup entirely.
    // Native Metal path uses the full shadow code in DeferredLighting.metal.
    var shadow = 1.0;
    Lo = Lo * shadow;

    // IBL ambient.
    let F_ibl = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    var kD_ibl = vec3<f32>(1.0) - F_ibl;
    kD_ibl = kD_ibl * (1.0 - metallic);

    // textureSampleLevel (not textureSample) because the sky early-out above
    // puts this in non-uniform control flow. Cube IBL irradiance has no
    // meaningful mip chain, so LOD 0 is correct.
    let irradiance = textureSampleLevel(irradianceMap, defaultSampler, N, 0.0).rgb;
    let diffuse_ibl = kD_ibl * albedo * irradiance;

    let R = reflect(-V, N);
    let MAX_REFLECTION_LOD = 4.0;
    let prefilteredColor = textureSampleLevel(prefilterMap, defaultSampler, R, roughness * MAX_REFLECTION_LOD).rgb;
    let brdf = textureSampleLevel(brdfLUT, brdfSampler, vec2<f32>(max(dot(N, V), 0.0), roughness), 0.0).rg;
    let specular_ibl = prefilteredColor * (F_ibl * brdf.x + brdf.y);

    let ambient = (diffuse_ibl + specular_ibl) * ao;
    let color = Lo + ambient;

    out.color = vec4<f32>(color, 1.0);
    return out;
}

// === Blit shader ===
// fragmentBlit lives in Blit.wgsl because the blit pipeline has a different
// descriptor set layout (single SampledImage at binding 0) and WGSL doesn't
// allow two module-scope vars at the same group/binding with different types.
