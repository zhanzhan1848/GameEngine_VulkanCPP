#version 450 core

// HardcodedDraw.vert — Absolute minimal test.
// Ignores ALL buffers. Draws a fixed fullscreen triangle colored by position.
// If this renders correctly, the pipeline/render-pass infrastructure is fine
// and the bug is in buffer data/binding.

layout(set = 0, binding = 0) uniform ConstantsBlock {
    mat4 view_matrix;
    mat4 proj_matrix;
    mat4 world_matrix;
    uint view_width;
    uint view_height;
    uint meshlet_count;
    uint padding;
    mat4 prev_view_matrix;
    mat4 prev_proj_matrix;
    uint has_prev_frame;
    uint debug_mode;
    uint pad2_1;
    uint pad2_2;
} dc;

layout(location = 0) out vec3 vColor;

void main() {
    // Fixed triangle in clip space — covers screen regardless of buffers.
    vec2 verts[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    vec2 p = verts[gl_VertexIndex % 3];
    gl_Position = vec4(p, 0.0, 1.0);
    vColor = vec3(float(gl_VertexIndex) / 3.0, 0.5, 1.0);
}
