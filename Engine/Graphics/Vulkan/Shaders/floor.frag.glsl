#version 450

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec2 Texcoord;

layout(binding = 1) uniform sampler2D texSampler;
layout(binding = 2) uniform sampler2D shadowMap;

void main() {
    outColor = vec4(smoothstep(0.0, 2.0, texture(shadowMap, Texcoord).r));
    //outColor = vec4(1.0);
}