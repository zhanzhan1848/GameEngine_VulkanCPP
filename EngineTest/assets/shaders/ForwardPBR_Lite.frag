#version 460 core
// ForwardPBR_Lite.frag — Validates the full ForwardLightBuffer layout.
//
// Replaces SimplePBR's standalone DirectionalLightParameters UBO with the
// engine's full ForwardLightBuffer struct (25808 bytes):
//   - directionalLightCount + punctualLightCount + padding[2]  (16B)
//   - directionalLights: array<DirectionalLightParameters, 4>  (1216B)
//   - lights: array<LightParameters, 128>                       (24576B)
//
// The shader also walks the lights[] array (when punctualLightCount > 0)
// to validate array indexing inside the large UBO.
//
// Lighting model (same as SimplePBR):
//   - Directional light from lightBuffer.directionalLights[0]
//   - Optional punctual light from lightBuffer.lights[0] (count=1)
//   - Constant ambient 0.03
//   - Reinhard tonemap + sRGB gamma

layout(std140, set = 0, binding = 11) uniform GlobalShaderData {
    mat4 view;
    mat4 projection;
    mat4 invProjection;
    mat4 viewProjection;
    mat4 previousViewProjection;
    mat4 invViewProjection;
    vec4 cameraPositionAndViewWidth;
    vec4 cameraDirectionAndViewHeight;
    uvec4 numDirNumPunctualDelta;
    uvec4 renderModeEnableFlags;
    vec2 jitterOffset;
    float debug_directLightBoost;
    float debug_iblStrength;
    float debug_ddgiIndirectWeight;
    float debug_exposure;
    vec2 _pad_after_debug;
};

// C++ LightParameters (192 bytes). simd::float3 pads to 16B per vec3, so each
// (v3 + float) pair occupies 32 bytes. std140 mirrors this with vec4 + pad.
struct LightParameters {
    vec4 position;       // xyz = pos,    w = intensity
    vec4 _pad0;          // align to 32
    vec4 direction;      // xyz = dir,    w = range
    vec4 _pad1;
    vec4 color;          // xyz = color,  w = cosUmbra
    vec4 _pad2;
    vec4 attenuation;    // xyz = att,    w = cosPenumbra
    // lightType, shadowIndex, padding (4 bytes each) + 4 bytes pad → vec4
    vec4 _lt_sh_pad;
    mat4 viewProjection;
};

// C++ DirectionalLightParameters (304 bytes).
struct DirectionalLightParameters {
    mat4 viewProjections[4];     // 256B
    vec4 splits;                 // 16B
    vec4 directionAndIntensity;  // 16B
    vec4 colorAndShadow;         // 16B
};

// C++ ForwardLightBuffer (25808 bytes).
layout(std140, set = 0, binding = 12) uniform ForwardLightBuffer {
    uvec4 counts;  // directionalLightCount, punctualLightCount, padding, padding
    DirectionalLightParameters directionalLights[4];
    LightParameters lights[128];
} lightBuffer;

layout(std140, set = 1, binding = 0) uniform PerObjectData {
    mat4 world;
    mat4 invWorld;
    mat4 worldViewProjection;
    mat4 prevWorldViewProjection;
    vec4 sh_coeffs[9];
};

layout(set = 2, binding = 0) uniform sampler2D albedoMap;

layout(location = 0) in vec3 v_worldPos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

const float PI = 3.14159265358979;

void main() {
    vec3 albedo = texture(albedoMap, v_uv).rgb;
    vec3 N = normalize(v_normal);

    vec3 color = vec3(0.0);

    // Directional light via ForwardLightBuffer.directionalLights[0].
    if (lightBuffer.counts.x > 0u) {
        DirectionalLightParameters dirLight = lightBuffer.directionalLights[0];
        vec3 L = normalize(-dirLight.directionAndIntensity.xyz);
        float NdotL = max(dot(N, L), 0.0);
        vec3 radiance = dirLight.colorAndShadow.rgb * dirLight.directionAndIntensity.w;
        color += albedo * radiance * NdotL;
    }

    // Punctual light via ForwardLightBuffer.lights[0]. Exercises array stride.
    if (lightBuffer.counts.y > 0u) {
        LightParameters plight = lightBuffer.lights[0];
        vec3 toLight = plight.position.xyz - v_worldPos;
        float distance = length(toLight);
        vec3 L = toLight / max(distance, 0.0001);
        float NdotL = max(dot(N, L), 0.0);

        // Inverse attenuation: 1 / (x + y*d + z*d²). attenuation.xyz holds x, y, z.
        float att = 1.0 / max(plight.attenuation.x
                            + plight.attenuation.y * distance
                            + plight.attenuation.z * distance * distance, 0.0001);
        // Range fade
        float range = plight.direction.w;
        att *= 1.0 - smoothstep(range * 0.6, range, distance);
        vec3 radiance = plight.color.xyz * plight.position.w * att;
        color += albedo * radiance * NdotL;
    }

    // Ambient
    color += vec3(0.03) * albedo;

    // Reinhard tonemap + gamma (matches SimplePBR for parity with same reference).
    color = color / (1.0 + color);
    color = pow(color, vec3(1.0 / 2.2));

    o_color = vec4(color, 1.0);
}
