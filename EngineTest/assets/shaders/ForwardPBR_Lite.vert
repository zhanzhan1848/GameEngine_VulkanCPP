#version 460 core
// ForwardPBR_Lite.vert — Layout validation for ForwardLightBuffer struct.
// Mirrors SimplePBR.vert exactly except that GlobalShaderData is unchanged;
// the difference is in the fragment shader where ForwardLightBuffer replaces
// the standalone DirectionalLightParameters UBO.
//
// Vertex stride = 32 bytes (matches SimplePBR):
//   location 0: vec3 position (12B) + 4B pad
//   location 1: vec3 normal   (12B) + 4B pad
//   location 2: vec2 uv       (8B)

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

layout(std140, set = 1, binding = 0) uniform PerObjectData {
    mat4 world;
    mat4 invWorld;
    mat4 worldViewProjection;
    mat4 prevWorldViewProjection;
    vec4 sh_coeffs[9];
};

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

layout(location = 0) out vec3 v_worldPos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;

void main() {
    vec4 worldPos = world * vec4(a_position, 1.0);
    v_worldPos = worldPos.xyz;
    v_normal = normalize((world * vec4(a_normal, 0.0)).xyz);
    v_uv = a_uv;
    gl_Position = viewProjection * worldPos;
}
