#version 450 core
// NeuralDebugView.vert — M0 神经 debug 视图全屏三角形（无顶点缓冲，vertex pulling）。
// MSL 孪生：MLPInferDebug.metal 的 neural_debug_vs。

vec2 positions[3] = vec2[3](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0));

layout(location = 0) out vec2 out_uv;

void main() {
    vec2 p = positions[gl_VertexIndex];
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    out_uv = p;
}
