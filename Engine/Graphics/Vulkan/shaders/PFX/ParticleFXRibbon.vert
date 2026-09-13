#version 450 core
// PFX ribbon：条带顶点（32B/顶点 = 2×vec4：pos_uv / uv_pad）
layout(set = 0, binding = 0) readonly buffer Strip { vec4 strip[]; };  // [pos_uv, uv_pad] ×N
layout(set = 0, binding = 1) uniform VP { mat4 view_projection; vec4 color; } vp;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

void main() {
    uint base = gl_VertexIndex * 2u;
    vec4 pos_uv  = strip[base + 0u];
    vec4 uv_pad  = strip[base + 1u];
    gl_Position = vp.view_projection * vec4(pos_uv.xyz, 1.0);
    out_uv = vec2(pos_uv.w, uv_pad.x);
    out_color = vp.color;
}
