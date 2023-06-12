#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

layout(binding = 1) uniform sampler2D texSampler;
layout(binding = 2) uniform sampler2D shadowMap;

void main() {
    outColor = vec4(2.0 * fragColor * texture(texSampler, fragTexCoord).rgb, 1.0);
    //outColor = vec4(2.0 * fragColor * (smoothstep(vec3(4.0), vec3(6.0), texture(shadowMap, fragTexCoord).rgb) + texture(texSampler, fragTexCoord).rgb * 0.5), 1.0);
}