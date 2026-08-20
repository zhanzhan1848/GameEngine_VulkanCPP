#version 450
// P4c-F7: fullscreen triangle, flat color from push constant.
layout(push_constant) uniform PC { vec4 color; } pc;
out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out vec4 vColor;

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vColor = pc.color;
}
