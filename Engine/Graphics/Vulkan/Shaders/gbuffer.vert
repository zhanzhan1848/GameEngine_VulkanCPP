#version 450

#include "ShadersCommonHeaders.h"

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec3 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inNormal;

layout (location = 4) in vec3 transform;
layout (location = 5) in vec3 rotate;
layout (location = 6) in vec3 scale;

layout (binding = 0) uniform UBO 
{
	mat4	model;
	mat4	view;
	mat4	projection;
} ubo;

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec2 outUV;
layout (location = 2) out vec3 outColor;
layout (location = 3) out vec3 outPos;

void main() 
{
	mat4 gRotMat = rotateMatrix(rotate);
    vec4 pos = trans_scale(transform, rotate, scale, inPos.xyz);

    gl_Position = ubo.projection * ubo.view * gRotMat * pos;
	
	outUV = inUV;

	// Vertex position in view space
	outPos = vec3(ubo.view * ubo.model * inPos);

	// Normal in view space
	mat3 normalMatrix = transpose(inverse(mat3(ubo.view * ubo.model)));
	outNormal = normalMatrix * inNormal;

	outColor = inColor;
}