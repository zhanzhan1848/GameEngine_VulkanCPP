#version 450 core
// M5:全屏三角形(无顶点缓冲)
layout(location = 0) out vec2 outUV;
void main() {
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    outUV = pos;
}
