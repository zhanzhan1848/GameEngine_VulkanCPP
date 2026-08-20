#version 460 core
// SimplePBR.frag — Minimal PBR fragment shader for cross-backend parity tests.
// Lighting model: 1 directional light + constant ambient. No IBL, no shadows,
// no MRT. Output: location 0 = RGBA8 tonemapped color.
//
// Layout mirrors SimplePBR.metal. UBO sizes match C++ struct truth:
//   GlobalShaderData           = 480 B
//   PerObjectData              = 400 B
//   DirectionalLightParameters = 304 B

layout(std140, set = 0, binding = 11) uniform GlobalShaderData {
    mat4 view;                    //   0- 63
    mat4 projection;              //  64-127
    mat4 invProjection;           // 128-191
    mat4 viewProjection;          // 192-255
    mat4 previousViewProjection;  // 256-319
    mat4 invViewProjection;       // 320-383
    vec4 cameraPositionAndViewWidth;    // 384-399
    vec4 cameraDirectionAndViewHeight;  // 400-415
    uvec4 numDirNumPunctualDelta;       // 416-431
    uvec4 renderModeEnableFlags;        // 432-447
    vec2 jitterOffset;                  // 448-455
    float debug_directLightBoost;       // 456-459
    float debug_iblStrength;            // 460-463
    float debug_ddgiIndirectWeight;     // 464-467
    float debug_exposure;               // 468-471
    vec2 _pad_after_debug;              // 472-479
};

layout(std140, set = 0, binding = 12) uniform DirectionalLightParameters {
    mat4 viewProjections[4];
    vec4 splits;
    vec4 directionAndIntensity;  // xyz=dir (pointing away from light), w=intensity
    vec4 colorAndShadow;         // rgb=color, a=shadow enabled
};

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

void main() {
    vec3 albedo = texture(albedoMap, v_uv).rgb;
    vec3 N = normalize(v_normal);

    // Directional light: WGSL convention is direction pointing AWAY from the
    // light source (i.e., towards the lit surface). Negate to get L.
    vec3 L = normalize(-directionAndIntensity.xyz);
    float NdotL = max(dot(N, L), 0.0);

    vec3 lightColor = colorAndShadow.rgb * directionAndIntensity.w;
    vec3 ambient = vec3(0.03) * albedo;
    vec3 lit = albedo * lightColor * NdotL;

    vec3 color = ambient + lit;

    // Reinhard tonemap + sRGB gamma (matches ForwardPBR.wgsl pow + gamma scheme
    // for parity, but simpler). Final color in [0, 1].
    color = color / (1.0 + color);
    color = pow(color, vec3(1.0 / 2.2));

    o_color = vec4(color, 1.0);
}
