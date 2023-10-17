#version 450

#include "Common.h"

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inColor;
layout (location = 3) in vec3 inPos;

layout (location = 0) out vec4 outPosition;
layout (location = 1) out vec4 outNormal;
layout (location = 2) out vec4 outAlbedo;

layout (set = 0, binding = 0) uniform UBO 
{
	mat4 model;
	mat4 view;
	mat4 projection;
	vec3 cameraDir;
	float nearPlane;
	float farPlane;
} ubo;

layout(set = 0, binding = 1) uniform Model
{
	mat4 model;
} model;

layout (set = 0, binding = 2) uniform sampler2D samplerColormap;

layout(set = 1, binding = 0) uniform DirectionalLightParameter
{
	DirectionalLightParameters param[3];
} directionalLight;


float linearDepth(float depth)
{
	float z = depth * 2.0f - 1.0f; 
	return (2.0f * ubo.nearPlane * ubo.farPlane) / (ubo.farPlane + ubo.nearPlane - z * (ubo.farPlane - ubo.nearPlane));	
}

void main() 
{
	vec3 color = inColor;
	for(int i = 0; i < 3; ++i)
	{
		vec3 lightDir = directionalLight.param[i].Direction;
		if(abs(lightDir.z - 1.0) < 0.001)
		{
			lightDir = ubo.cameraDir;
		}
		float diffuse = max(dot(inNormal, -lightDir), 0.0);
		vec3 reflection = reflect(lightDir, inNormal);
		float specular = pow(max(dot(ubo.cameraDir, reflection), 0.0), 16) * 0.5;

		vec3 lightColor = directionalLight.param[i].Color * directionalLight.param[i].Intensity;
		color += (diffuse + specular) * lightColor;
	}
	float ambient = 10 / 255.0;
	color = clamp(color + ambient, 0.0, 1.0);
	outPosition = vec4(inPos, 1.0);
	outNormal = vec4(normalize(inNormal) * 0.5 + 0.5, 1.0);
	outAlbedo = texture(samplerColormap, inUV) * vec4(color, 1.0);
}