#version 450 core

// T4.6.5 part 12 — Vulkan port of Forward/GBufferTransparent.metal vertex stage.
// Entry point: main
//
// Transparent variant: emits per-instance material terms (baseColor, roughness,
// metallic) and worldBitangent for the fragment stage. Blending / depth-write
// policy is controlled by the pipeline (C++ side), not the shader.
//
// Descriptor set layout (matches ForwardSceneRenderer::CreateDescriptorLayouts):
//   set 0 (global): binding 0 = ViewData UBO, binding 1 = SceneData UBO
// Push constants (PCGPushConsts at offset 0): mat4 transform; uint use_instances; uvec3 _pad
//
// Vertex input (matches Metal VertexInput struct, 32-byte stride):
//   location 0  vec3 position    (offset 0, packed_float3)
//   location 1  uint  colorTSign (offset 12)
//   location 2  uvec2 normal     (offset 16, packed_ushort2)
//   location 3  uvec2 tangent    (offset 20, packed_ushort2)
//   location 4  vec2  uv         (offset 24, packed_float2)

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
    vec2 padding;
    vec4 viewPos;
    mat4 shadowMatrix0;
    mat4 shadowMatrix1;
} sceneData;

// Per-instance material terms (matches Metal InstanceData.metal — buffer(3) /
// global set binding 2 here). Drives baseColor/roughness/metallic for the
// transparent GBuffer when pc.use_instances != 0.
struct InstanceData {
    mat4  transform;
    vec4  baseColor;
    float roughness;
    float metallic;
    float alphaCutoff;
    float _pad;
};
layout(set = SET_GLOBAL, binding = 2) readonly buffer InstanceBuffer {
    InstanceData models[];
} instanceData;

// std140 layout trap: see GBuffer.vert comment. Pack (use_instances + _pad[3])
// into a single vec4 for an exact 80-byte PCGPushConsts match.
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
layout(location = 2) out vec3 outWorldTangent;
layout(location = 3) out vec3 outWorldBitangent;
layout(location = 4) out vec2 outUV;
layout(location = 5) out vec4 outCurrentPos;
layout(location = 6) out vec4 outPreviousPos;
layout(location = 7) out vec4 outInstanceBaseColor;
layout(location = 8) out float outInstanceRoughness;
layout(location = 9) out float outInstanceMetallic;

const float InvIntervals = 2.0 / ((1 << 16) - 1);

vec3 UnpackNormal(uvec2 p) {
    vec2 f = vec2(p);
    f = f * InvIntervals - 1.0;
    float d = dot(f, f);
    if (d > 1.0) {
        return vec3(0.0, 0.0, 1.0);  // invalid data fallback
    }
    float z = sqrt(max(0.0, 1.0 - d));
    return vec3(f.x, f.y, z);
}

void main() {
    mat4 model;
    if (pc.use_instances != 0u) {
        InstanceData inst = instanceData.models[gl_InstanceIndex];
        model = inst.transform;
        outInstanceBaseColor = inst.baseColor;
        outInstanceRoughness = inst.roughness;
        outInstanceMetallic  = inst.metallic;
    } else {
        model = pc.transform;
        outInstanceBaseColor = vec4(1.0, 1.0, 1.0, 1.0);
        outInstanceRoughness = 0.5;
        outInstanceMetallic  = 0.0;
    }

    vec4 worldPos = model * vec4(in_position, 1.0);
    outWorldPos = worldPos.xyz;

    mat3 normalMatrix = mat3(model[0].xyz, model[1].xyz, model[2].xyz);

    vec3 rawNormal = UnpackNormal(in_normal);
    outWorldNormal = normalize(normalMatrix * rawNormal);

    vec3 rawTangent = UnpackNormal(in_tangent);
    outWorldTangent = normalize(normalMatrix * rawTangent);

    outWorldBitangent = cross(outWorldNormal, outWorldTangent);

    // V-flip UV to match Metal convention (GBuffer.metal flips with 1.0 - rawUV.y)
    outUV = vec2(in_uv.x, 1.0 - in_uv.y);

    gl_Position = viewData.viewProjection * worldPos;

    outCurrentPos = gl_Position;
    outPreviousPos = viewData.previousViewProjection * (sceneData.previousModel * vec4(in_position, 1.0));
}
