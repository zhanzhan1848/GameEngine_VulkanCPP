#version 450 core

// T4.6.5 part 2 — Vulkan port of Forward/DepthOnly.metal
//
// Computes depth-only for shadow mapping. Single entry point: shadow_mapping_vs
//
// Descriptor set layout (matches ForwardSceneRenderer::CreateDescriptorLayouts):
//   set 0 (global): binding 0 = ViewData UBO, binding 1 = SceneData UBO
// Push constants (PCGPushConsts at offset 0): mat4 mvp; uint use_instances; uvec3 _pad
// Vertex input: binding 0, location 0 = vec3 position (32-byte stride, 12 bytes used)
//
// Metal source reference:
//   vertices [[buffer(20)]] — Metal uses per-buffer index; Vulkan uses binding 0
//   instanceModels [[buffer(3)]] — Vulkan: SSBO in set 0 binding 2 (added)
//   viewData [[buffer(0)]] — Vulkan: UBO set 0 binding 0
//   pushConsts [[buffer(2)]] — Vulkan: push_constant block

#extension GL_GOOGLE_include_directive : enable

#define SET_GLOBAL 0

layout(set = SET_GLOBAL, binding = 0) uniform ViewData {
    mat4 viewProjection;
    mat4 invViewProjection;
    mat4 previousViewProjection;
} viewData;

// T4.6.5 part 13: vec4 _use_pad trick — see GBuffer.vert part 11 comment.
// Pack (use_instances + _pad[3]) into vec4 to match C++ PCGPushConsts 80B.
layout(push_constant) uniform PushConsts {
    mat4 transform;
    vec4 _use_pad;  // .x = use_instances, .yzw = _pad[0..2]
} pc;
#define use_instances _use_pad.x

// Instance models SSBO — Metal [[buffer(3)]], Vulkan: set 0 binding 2.
// T4.6.5 part 15.1: declared with full InstanceData struct (96B stride) to match
// C++ graphics::InstanceData layout. Earlier `mat4 models[]` declaration
// strides by 64B and reads wrong data when use_instances != 0.
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

// Vertex input — Metal [[buffer(20)]] with 32-byte stride (12 bytes position + 20 padding)
layout(location = 0) in vec3 in_position;

void main() {
    mat4 model = (pc.use_instances != 0u)
        ? instanceData.models[gl_InstanceIndex].transform
        : pc.transform;

    vec4 worldPos = model * vec4(in_position, 1.0);
    gl_Position = viewData.viewProjection * worldPos;
}
