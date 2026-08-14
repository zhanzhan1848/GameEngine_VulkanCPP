#version 450 core

// T4.6.5 part 16.4 — Vulkan port of Engine/Graphics/Metal/shaders/Forward/Line.metal
// Entry point: main (Vulkan convention; Metal uses line_vs).
//
// Mirrors the Metal source: view_proj via push_constant, vertices via SSBO
// at binding 1. The C++ caller (LineBatchRenderer::Render) binds the SSBO
// to set 0 / binding 1 and pushes a 64B m4x4 view_proj at offset 0.

layout(push_constant) uniform PC {
    mat4 view_proj;
} pc;

layout(set = 0, binding = 1) readonly buffer VertexBuffer {
    vec3 vertices[];
} vb;

void main() {
    gl_Position = pc.view_proj * vec4(vb.vertices[gl_VertexIndex], 1.0);
}
