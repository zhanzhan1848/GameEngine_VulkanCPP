#version 450 core

// T4.6.5 part 14 — Vulkan port of Forward/ForwardTransparency.metal
// Entry point: forwardTransparentFS  →  main
//
// Technique: textured transparent surface with inline PBR. Samples albedo /
// ORM / normal maps, modulates by per-instance material, applies Cook-Torrance
// specular + hard shadow from cascade 0. Output alpha = texture alpha *
// instance alpha (no blending state set here — pipeline config owns that).
//
// Descriptor set layout (matches ForwardSceneRenderer::material_set_layout_):
//   set 1 binding 0 = SampledImage albedo
//   set 1 binding 1 = SampledImage normal
//   set 1 binding 2 = SampledImage orm
//   set 1 binding 3 = Sampler defaultSampler
//
// Output: single BGRA8_UNorm render target (location 0). Alpha = transparency.

#extension GL_EXT_samplerless_texture_functions : enable

const float PI = 3.14159265359;

layout(set = 0, binding = 1) uniform SceneData {
    mat4 model;
    vec4 lightPos;
    vec4 lightColor;
    vec4 reflectionPlane;
    vec4 reflectionPlane2;
    vec4 reflectionPlane3;
    mat4 previousModel;
    vec2 jitter;
    vec2 previousJitter;
    float time;
    float _timePad;
    vec4 viewPos;
    mat4 shadowMatrix0;
    mat4 shadowMatrix1;
} sceneData;

layout(set = 1, binding = 0) uniform texture2D albedoMap;
layout(set = 1, binding = 1) uniform texture2D normalMap;
layout(set = 1, binding = 2) uniform texture2D ormMap;
layout(set = 1, binding = 3) uniform sampler    defaultSampler;

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inShadowPos0;
layout(location = 4) in vec4 inShadowPos1;
layout(location = 5) in vec4 inInstanceBaseColor;
layout(location = 6) in float inInstanceRoughness;
layout(location = 7) in float inInstanceMetallic;

layout(location = 0) out vec4 outColor;

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom + 0.0001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec4 albedoSample = texture(sampler2D(albedoMap, defaultSampler), inUV);
    vec3 albedo = albedoSample.rgb * inInstanceBaseColor.rgb;
    float alpha = albedoSample.a * inInstanceBaseColor.a;

    vec4 ormSample = texture(sampler2D(ormMap, defaultSampler), inUV);
    float roughness = inInstanceRoughness;
    float metallic  = inInstanceMetallic;
    if (length(ormSample.rgb) > 0.01) {
        roughness = ormSample.g * inInstanceRoughness;
        metallic  = ormSample.b * inInstanceMetallic;
    }
    roughness = max(roughness, 0.04);

    vec3 N = normalize(inWorldNormal);
    vec3 normalSample = texture(sampler2D(normalMap, defaultSampler), inUV).rgb;
    if (length(normalSample) > 0.1) {
        // No tangent stream — synthesize an orthonormal TBN around the geometric
        // normal (matches Metal reference: pick a non-parallel up vector, fall
        // back to alternate axis if N is too close to (0,1,0)).
        vec3 up = vec3(0.0, 1.0, 0.0);
        if (length(cross(N, up)) < 0.01) {
            up = vec3(1.0, 0.0, 0.0);
        }
        vec3 tangent   = normalize(cross(N, up));
        vec3 bitangent = cross(N, tangent);
        mat3 TBN = mat3(tangent, bitangent, N);
        N = normalize(TBN * (normalSample * 2.0 - 1.0));
    }

    vec3 lightDir = normalize(sceneData.lightPos.xyz);
    vec3 viewDir  = normalize(sceneData.viewPos.xyz - inWorldPos);
    vec3 H        = normalize(viewDir + lightDir);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NDF = DistributionGGX(N, H, roughness);
    float G   = GeometrySmith(N, viewDir, lightDir, roughness);
    vec3 F    = FresnelSchlick(max(dot(H, viewDir), 0.0), F0);

    float numerator   = NDF * G;
    float denominator = 4.0 * max(dot(N, viewDir), 0.0) * max(dot(N, lightDir), 0.0) + 0.0001;
    vec3 specular = (numerator * F) / denominator;

    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);
    float NdotL = max(dot(N, lightDir), 0.0);
    vec3 Lo = (kD * albedo / PI + specular) * sceneData.lightColor.rgb * NdotL;

    // Hard shadow from cascade 0.
    float shadow = 1.0;
    vec3 sp0 = inShadowPos0.xyz / inShadowPos0.w;
    sp0.xy = sp0.xy * 0.5 + 0.5;
    sp0.y = 1.0 - sp0.y;
    if (sp0.z > 0.0 && sp0.z < 1.0 && sp0.x > 0.0 && sp0.x < 1.0 && sp0.y > 0.0 && sp0.y < 1.0) {
        shadow = 0.5 + 0.5 * step(0.0, sp0.z - 0.005);
    }

    vec3 ambient = vec3(0.03) * albedo;
    vec3 color = ambient + Lo * shadow;

    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, alpha);
}
