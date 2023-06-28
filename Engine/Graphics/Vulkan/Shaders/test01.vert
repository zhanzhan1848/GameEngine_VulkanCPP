#version 450

#include "ShadersCommonHeaders.h"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inTexCoord;
layout(location = 3) in vec3 inNormal;

layout(location = 4) in vec3 transform;
layout(location = 5) in vec3 rotate;
layout(location = 6) in vec3 scale;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;

layout(binding = 0) uniform UniformBufferObject{
    mat4 model;
    mat4 view;
    mat4 proj;
    mat4 lightModel;
    mat4 lightView;
    mat4 lightProj;
    vec3 lightPos;
    float near;
    float far;
} ubo;

void main() {

    mat4 gRotMat = rotateMatrix(rotate);
    vec4 pos = trans_scale(transform, rotate, scale, inPosition);

    gl_Position = ubo.proj * ubo.view * gRotMat * pos;
    fragColor = inColor;
    fragTexCoord = inTexCoord.xy;
}