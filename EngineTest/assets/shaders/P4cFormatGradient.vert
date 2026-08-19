#version 450
// P4c-F1 format parity: fullscreen triangle from gl_VertexIndex (no VB/IB).
out gl_PerVertex { vec4 gl_Position; };

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
