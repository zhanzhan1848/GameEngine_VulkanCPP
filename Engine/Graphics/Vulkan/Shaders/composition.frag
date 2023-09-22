#version 450

layout (binding = 0) uniform sampler2D samplerposition;
layout (binding = 1) uniform sampler2D samplerNormal;
layout (binding = 2) uniform sampler2D samplerAlbedo;
// layout (binding = 3) uniform sampler2D samplerSSAO;

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

void main() 
{
	vec2 flip_y_uv = vec2(inUV.x, 1.0 - inUV.y);
	vec3 fragPos = texture(samplerposition, flip_y_uv).rgb;
	vec3 normal = normalize(texture(samplerNormal, flip_y_uv).rgb * 2.0 - 1.0);
	vec4 albedo = texture(samplerAlbedo, flip_y_uv);
	 
	//float ssao = texture(samplerSSAO, flip_y_uv).r;

	vec3 lightPos = vec3(5.0);
	vec3 L = normalize(lightPos - fragPos);
	float NdotL = max(0.5, dot(normal, L));

	vec3 baseColor = albedo.rgb * NdotL;

	outFragColor.rgb = vec3(1.0);

	outFragColor.rgb *= baseColor;

}