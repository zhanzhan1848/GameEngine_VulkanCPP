#version 450 core
// PassDebugView.vert — 全屏三角（对齐 Forward/Blit.vert 的 V 翻转约定）。
// Entry point: main

layout(location = 0) out vec2 outUv;

void main() {
    vec4 positions[3] = vec4[3](
        vec4(-1.0, -1.0, 0.0, 1.0),
        vec4(-1.0,  3.0, 0.0, 1.0),
        vec4( 3.0, -1.0, 0.0, 1.0)
    );
    // Metal UVs assume (0,0) at top-left; Vulkan textures have (0,0) at
    // bottom-left. Flip V so sampling aligns with the pass render targets.
    vec2 uvs[3] = vec2[3](
        vec2(0.0, 0.0),
        vec2(0.0, 2.0),
        vec2(2.0, 0.0)
    );
    gl_Position = positions[gl_VertexIndex];
    outUv = uvs[gl_VertexIndex];
}
