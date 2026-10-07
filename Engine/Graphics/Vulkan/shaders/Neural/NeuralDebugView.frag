#version 450 core
// NeuralDebugView.frag — 采样 MLP 推理输出（1:1 texel fetch），tonemap + gamma，
// split 模式中缝画一条暗线。MSL 孪生：MLPInferDebug.metal 的 neural_debug_fs。

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

#extension GL_EXT_samplerless_texture_functions : enable

layout(set = 0, binding = 0) uniform texture2D nnTex;

void main() {
    ivec2 dims = textureSize(nnTex, 0);
    ivec2 tc = min(ivec2(in_uv * vec2(dims)), dims - ivec2(1));
    vec3 c = texelFetch(nnTex, tc, 0).rgb;
    c = 1.0 - exp(-c * 1.6);            // tonemap
    c = pow(c, vec3(1.0 / 2.2));        // gamma
    if (abs(in_uv.x - 0.5) < 0.0015) c = vec3(0.0);
    out_color = vec4(c, 1.0);
}
