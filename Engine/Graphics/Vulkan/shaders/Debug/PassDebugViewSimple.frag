#version 450 core
// PassDebugViewSimple.frag — 二分实验：极简绑定面（binding0 SampledImage +
// binding1 Sampler 直出源纹理），配合 PRIMAL_PASS_DEBUG_SIMPLE=1 隔离
// 多绑定/UBO 因素。与 PassDebugView.vert 配对。Entry point: main

layout(set = 0, binding = 0) uniform texture2D srcTex;
layout(set = 0, binding = 1) uniform sampler inputSamp;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(sampler2D(srcTex, inputSamp), inUv).rgb, 1.0);
}
