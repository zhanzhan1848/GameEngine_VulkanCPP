#version 450
// P4c-F3 texture update parity: fullscreen triangle with UV passthrough.
out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) out vec2 fragUV;

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    fragUV = uv;
}
