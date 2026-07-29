#version 450

#include "ShadersCommonHeaders.h"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inTexCoord;
layout(location = 3) in vec3 inNormal;

layout(location = 4) in vec3 transform;
layout(location = 5) in vec3 rotate;
layout(location = 6) in vec3 scale;

layout(location = 0) out vec3 outPos;

layout(binding = 0) uniform UniformBufferObject
{
	mat4	model;
	mat4	view;
	mat4	projection;
} ubo;

out gl_PerVertex
{
    vec4 gl_Position;
};

void main()
{
    mat4 gRotMat = rotateMatrix(rotate);
    vec4 pos = trans_scale(transform, rotate, scale, inPos);

    gl_Position = ubo.projection * ubo.view * gRotMat * pos;
    outPos = (ubo.projection * ubo.view * gRotMat * pos).rgb
;}