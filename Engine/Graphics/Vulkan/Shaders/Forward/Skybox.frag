#version 450 core

// T4.6.5 part 2 — Vulkan port of Forward/Skybox.metal fragment stage
// Entry point: fragmentSkybox
//
// Descriptor layout (matches skybox_set_layout_): separate SampledImage + Sampler.
//   set 0 binding 2 = textureCube (samplerless)
//   set 0 binding 3 = sampler

#extension GL_EXT_samplerless_texture_functions : enable

layout(set = 0, binding = 2) uniform textureCube skyboxTex;
layout(set = 0, binding = 3) uniform sampler skyboxSamp;

layout(location = 0) in vec3 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(samplerCube(skyboxTex, skyboxSamp), inUv);
}
