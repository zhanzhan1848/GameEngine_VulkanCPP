#version 450

#include "ShadersCommonHeaders.h"

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec3 inColor;
layout (location = 2) in vec2 inUV;
layout (location = 3) in vec3 inNormal;

layout (set = 0, binding = 0) uniform UBO 
{
	mat4	model;
	mat4	view;
	mat4	projection;
	vec3	cameraDir;
	float	near;
	float	far;
} ubo;

layout(set = 0, binding = 1) uniform Model
{
	mat4 model;
} model;

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec2 outUV;
layout (location = 2) out vec3 outColor;
layout (location = 3) out vec3 outPos;

void main() 
{
    gl_Position = ubo.projection * ubo.view * model.model * inPos;
	
	outUV = inUV;

	// Vertex position in view space
	outPos = vec3(ubo.view * ubo.model * model.model * inPos);

	// Normal in view space
	mat3 normalMatrix = transpose(inverse(mat3(ubo.view * model.model)));
	outNormal = normalMatrix * inNormal;

	outColor = inColor;
}