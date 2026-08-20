#version 450 core

// T4.6.5 part 16.4 — Vulkan port of Engine/Graphics/Metal/shaders/Forward/Line.metal
// Entry point: main (Vulkan convention; Metal uses line_fs).
// Solid green = preserved from Metal for parity.

layout(location = 0) out vec4 outFragColor;

void main() {
    outFragColor = vec4(0.2, 1.0, 0.4, 1.0);
}
