#version 450 core

// T4.6.5 part 14 — Vulkan port of Forward/ForwardTransparency.metal
// Entry point: forwardTransparentVS  →  main
//
// Identical transform/lighting-setup logic to ForwardWater.vert. Kept as a
// separate .vert so ForwardSceneRenderer can request a distinct shader module
// (the Metal reference funnels both into one .metal with two entry points;
// Vulkan prefers one entry point per SPIR-V).

#define SET_GLOBAL 0

layout(set = SET_GLOBAL, binding = 0) uniform ViewData {
    mat4 viewProjection;
    mat4 invViewProjection;
    mat4 previousViewProjection;
} viewData;

layout(set = SET_GLOBAL, binding = 1) uniform SceneData {
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

layout(push_constant) uniform PushConsts {
    mat4 transform;
    vec4 _use_pad;  // .x = use_instances, .yzw = _pad[0..2]
} pc;
#define use_instances _use_pad.x

layout(location = 0) in vec3  in_position;
layout(location = 1) in uint  in_colorTSign;
layout(location = 2) in uvec2 in_normal;     // packed_ushort2
layout(location = 3) in uvec2 in_tangent;    // packed_ushort2
layout(location = 4) in vec2  in_uv;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outShadowPos0;
layout(location = 4) out vec4 outShadowPos1;
layout(location = 5) out vec4 outInstanceBaseColor;
layout(location = 6) out float outInstanceRoughness;
layout(location = 7) out float outInstanceMetallic;

const float InvIntervals = 2.0 / ((1 << 16) - 1);

vec3 UnpackNormal(uvec2 p) {
    vec2 f = vec2(p);
    f = f * InvIntervals - 1.0;
    float d = dot(f, f);
    if (d > 1.0) {
        return vec3(0.0, 0.0, 1.0);
    }
    float z = sqrt(max(0.0, 1.0 - d));
    return vec3(f.x, f.y, z);
}

void main() {
    mat4 model = pc.transform;
    outInstanceBaseColor = vec4(1.0, 1.0, 1.0, 1.0);
    outInstanceRoughness = 0.5;
    outInstanceMetallic  = 0.0;

    vec4 worldPos = model * vec4(in_position, 1.0);
    outWorldPos = worldPos.xyz;

    mat3 normalMatrix = mat3(model[0].xyz, model[1].xyz, model[2].xyz);
    vec3 rawNormal = UnpackNormal(in_normal);
    outWorldNormal = normalize(normalMatrix * rawNormal);

    outUV = vec2(in_uv.x, 1.0 - in_uv.y);

    gl_Position = viewData.viewProjection * worldPos;

    outShadowPos0 = sceneData.shadowMatrix0 * worldPos;
    outShadowPos1 = sceneData.shadowMatrix1 * worldPos;
}
