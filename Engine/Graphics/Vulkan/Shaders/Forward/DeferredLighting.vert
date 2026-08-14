#version 450 core

// T4.6.5 part 24.10 — minimal Vulkan port of Forward/DeferredLighting.metal
// vertexMain. Procedural full-screen triangle (no vertex buffer needed).
// Entry point: main.

layout(location = 0) out vec2 outUv;

void main() {
    vec4 positions[3] = vec4[3](
        vec4(-1.0, -1.0, 0.0, 1.0),
        vec4(-1.0,  3.0, 0.0, 1.0),
        vec4( 3.0, -1.0, 0.0, 1.0)
    );
    // Match Blit.vert UV convention (Vulkan textures have (0,0) at bottom-left).
    vec2 uvs[3] = vec2[3](
        vec2(0.0, 0.0),
        vec2(0.0, 2.0),
        vec2(2.0, 0.0)
    );

    gl_Position = positions[gl_VertexIndex];
    outUv = uvs[gl_VertexIndex];
}
