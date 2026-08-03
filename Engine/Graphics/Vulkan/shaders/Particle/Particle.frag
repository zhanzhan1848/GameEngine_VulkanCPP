#version 450 core

// T4.6.5 part 16.3 — Vulkan port of Engine/Graphics/Metal/shaders/ParticleAtlas.metal
// Entry point: main (Vulkan convention; Metal uses particle_fragment).
//
// DEBUG: solid hot-pink output, matches Metal. Proves the fragment shader runs.

layout(set = 0, binding = 3) uniform sampler2D particle_texture;

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUV;
layout(location = 2) in float inAge;

layout(location = 0) out vec4 outFragColor;

void main() {
    // DEBUG: solid hot-pink — preserved from Metal for parity.
    outFragColor = vec4(1.0, 0.0, 1.0, 1.0);
}
