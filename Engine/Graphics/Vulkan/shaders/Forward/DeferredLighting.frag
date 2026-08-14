#version 450 core

// T4.6.5 part 37 — Cook-Torrance PBR + IBL port of Metal
// `fragmentLighting_v3` (DeferredLighting.metal:197-290).
//
// Differences from Metal original:
//   - IBL: bindings 10/11/12 wired (irradiance cube + prefilter cube + brdfLUT
//     2D) via DeferredLightingModule SetIBLResources. When handles are
//     INVALID, C++ binds a 1x1 white fallback — sampling returns white, so
//     IBL term becomes white * albedo (slight bright tint). Production code
//     path uses real env map (TestVulkanSponzaRenderGraph loads sunset.hdr).
//   - No ACES tonemap: output HDR linear. Tonemap is the FinalBlit
//     module's responsibility (matches Metal fragmentBlit).
//   - Y convention: Vulkan NDC Y is unflipped at vertex stage; UV origin
//     is bottom-left (matches Blit.vert convention).
//   - Shadow: samples pre-filtered `shadowVisTex` directly (ShadowMapModule
//     output) rather than reprojecting via shadowMatrix0.
//
// Entry point: main.
//
// Descriptor layout (matches DeferredLightingModule::Initialize):
//   set 0 binding 0 = UniformBuffer ViewData (Vertex|Pixel)
//   set 0 binding 1 = UniformBuffer SceneData
//   set 0 binding 2 = SampledImage albedo
//   set 0 binding 3 = SampledImage normal
//   set 0 binding 4 = SampledImage orm
//   set 0 binding 5 = SampledImage depth
//   set 0 binding 6 = SampledImage shadowVisibility
//   set 0 binding 8 = Sampler
//   set 0 binding 9 = SampledImage fallback (unused)
//   set 0 binding 10 = SampledImage irradianceMap (cube, IBL)
//   set 0 binding 11 = SampledImage prefilterMap (cube, IBL)
//   set 0 binding 12 = SampledImage brdfLUT (2D, IBL)

#extension GL_EXT_samplerless_texture_functions : enable

const float PI = 3.14159265358979;

// T4.6.5 part 40.3: visual contrast tuning.
//   - DIRECT_INTENSITY: boosts direct light so lit regions read brighter.
//   - IBL_INTENSITY: scales overall IBL ambient (was 1.0 → too flat / wash).
//   - SHADOW_AMBIENT_SCALE: ambient is multiplied by
//     mix(SHADOW_AMBIENT_SCALE, 1.0, shadowVisibility), so shadowed pixels
//     get only SHADOW_AMBIENT_SCALE × IBL_INTENSITY → shadows read darker.
const float DIRECT_INTENSITY   = 2.5;
const float IBL_INTENSITY      = 0.15;   // lowered: IBL was too bright (wrong cube view binding makes samples unpredictable)
const float SHADOW_AMBIENT_SCALE = 0.05; // crushed: shadowed pixels get very little ambient for darker shadows

layout(set = 0, binding = 0) uniform ViewData {
    mat4 viewProjection;
    mat4 invViewProjection;
};

layout(set = 0, binding = 1) uniform SceneData {
    mat4 model;
    vec4 lightPos;
    vec4 lightColor;
    vec4 reflectionPlane;
    vec4 reflectionPlane2;
    vec4 reflectionPlane3;
    mat4 previousModel;
    vec4 viewPos;
    mat4 shadowMatrix0;
    mat4 shadowMatrix1;
    vec2 jitter;
    vec2 previousJitter;
};

layout(set = 0, binding = 2) uniform texture2D albedoTex;
layout(set = 0, binding = 3) uniform texture2D normalTex;
layout(set = 0, binding = 4) uniform texture2D ormTex;
layout(set = 0, binding = 5) uniform texture2D depthTex;
layout(set = 0, binding = 6) uniform texture2D shadowVisTex;
layout(set = 0, binding = 7) uniform texture2D ssaoTex;   // SSAO filter output (R16F, 1.0=unoccluded)
layout(set = 0, binding = 8) uniform sampler defaultSampler;
layout(set = 0, binding = 9) uniform texture2D fallbackTex;  // unused
// T4.6.5 part 37: IBL resources (cube + 2D).
layout(set = 0, binding = 10) uniform textureCube irradianceMap;
layout(set = 0, binding = 11) uniform textureCube prefilterMap;
layout(set = 0, binding = 12) uniform texture2D brdfLUT;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

// PBR helpers — translated from
// Engine/Graphics/Metal/shaders/CommonFunction.metal:256-293 +
// Engine/Graphics/RHI/Shaders/RHIShaderPBR.metal:124-150.

float DistributionGGX(vec3 N, vec3 H, float a) {
    float a2 = a * a * a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float nom = a2;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;
    return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float k) {
    float r = k + 1.0;
    float a = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float k) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), k)
         * GeometrySchlickGGX(max(dot(N, L), 0.0), k);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// T4.6.5 part 37: roughness-aware Fresnel for IBL (mirror Metal
// FresnelSchlickRoughness in RHIShaderPBR.metal). The roughness term blends
// F0 toward 1.0 as roughness increases — gives metals a softer grazing tint.
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0)
         * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec4 albedoV = texture(sampler2D(albedoTex, defaultSampler), inUv);
    vec3 albedo = albedoV.rgb;
    vec3 normalEnc = texture(sampler2D(normalTex, defaultSampler), inUv).rgb;
    vec3 orm = texture(sampler2D(ormTex, defaultSampler), inUv).rgb;
    float depth = texture(sampler2D(depthTex, defaultSampler), inUv).r;

    // Background discard — sky pixels have depth = 1.0.
    if (depth >= 1.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Reconstruct world position (Vulkan NDC: Z in [0,1], Y NOT flipped).
    vec2 ndcXY = vec2(inUv.x * 2.0 - 1.0, inUv.y * 2.0 - 1.0);
    vec4 ndc = vec4(ndcXY, depth, 1.0);
    vec4 worldPosH = invViewProjection * ndc;
    vec3 worldPos = worldPosH.xyz / max(worldPosH.w, 1e-6);

    // Decode GBuffer.
    vec3 N = normalize(normalEnc * 2.0 - 1.0);
    // SSAO: read filter output at full-res. If SSAO pass is disabled, texture
    // falls back to white (1.0 = unoccluded) — no darkening.
    vec4 ssao = texture(sampler2D(ssaoTex, defaultSampler), inUv);
    float ao = (ssao.r > 0.0) ? ssao.r : 1.0;
    float roughness = max(orm.g, 0.04);
    float metallic = orm.b;

    // Light + view vectors.
    vec3 V = normalize(viewPos.xyz - worldPos);
    vec3 L = normalize(lightPos.xyz);  // directional light
    vec3 H = normalize(V + L);

    // F0 mix: dielectric 0.04 → metal albedo.
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Cook-Torrance specular BRDF.
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    vec3 specular = (NDF * G * F) / (4.0 * NdotV * NdotL + 0.001);

    // Energy-conserving diffuse.
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

    // Shadow visibility from ShadowMapModule's pre-filtered texture.
    float shadowVisibility = texture(sampler2D(shadowVisTex, defaultSampler), inUv).r;

    vec3 Lo = (diffuse + specular) * lightColor.rgb * NdotL * shadowVisibility
            * DIRECT_INTENSITY;

    // T4.6.5 part 37: IBL ambient (image-based lighting).
    // Mirror Metal fragmentLighting_v3 lines 270-287. Cube samples use the
    // shared defaultSampler (linear + repeat — fine for env maps). Prefilter
    // uses textureLod with roughness × MAX_PREFILTER_LOD to pick the mip.
    vec3 Fil = FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kSil = Fil;
    vec3 kDil = (vec3(1.0) - kSil) * (1.0 - metallic);

    vec3 irradiance = texture(samplerCube(irradianceMap, defaultSampler), N).rgb;
    vec3 diffuseIBL = irradiance * albedo;

    const float MAX_PREFILTER_LOD = 4.0;
    vec3 R = reflect(-V, N);
    vec3 prefilteredColor = textureLod(samplerCube(prefilterMap, defaultSampler),
                                       R, roughness * MAX_PREFILTER_LOD).rgb;
    vec2 brdf = texture(sampler2D(brdfLUT, defaultSampler),
                        vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specularIBL = prefilteredColor * (Fil * brdf.x + brdf.y);

    // T4.6.5 part 40.3: scale overall IBL down, and further crush ambient in
    // shadowed pixels so the shadow side reads darker against lit regions.
    float ambientVisibility = mix(SHADOW_AMBIENT_SCALE, 1.0, shadowVisibility);
    vec3 ambient = (kDil * diffuseIBL + specularIBL) * ao
                 * IBL_INTENSITY * ambientVisibility;

    outColor = vec4(Lo + ambient, 1.0);
}
