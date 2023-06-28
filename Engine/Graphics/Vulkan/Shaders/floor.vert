#version 450

#include "ShadersCommonHeaders.h"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inTexcoord;
layout(location = 3) in vec3 inNormal;

layout(location = 4) in vec3 transform;
layout(location = 5) in vec3 rotate;
layout(location = 6) in vec3 scale;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec3 lightVec;
layout(location = 2) out vec3 viewVec;
layout(location = 3) out vec4 shadowCoord;

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

const mat4 biasMat = mat4(
    0.5, 0.0, 0.0, 0.0,
    0.0, 0.5, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.5, 0.5, 0.0, 1.0
);

void main() {

    mat4 gRotMat = rotateMatrix(rotate);
    vec4 pos = trans_scale(transform, rotate, scale, inPosition);

    // gl_Position = vec4(fragTexCoord * 2.0 - 1.0, 0.0, 1.0);

    gl_Position = ubo.proj * ubo.view * gRotMat * pos;
    vec4 pos0 = gRotMat * pos;
    fragTexCoord = inTexcoord.xy;
    lightVec = normalize(ubo.lightPos - inPosition);
    viewVec = -(ubo.model * vec4(inPosition, 1.0)).xyz;
    shadowCoord = (biasMat * ubo.lightProj * ubo.lightView * ubo.lightModel * ubo.model) * pos;
}