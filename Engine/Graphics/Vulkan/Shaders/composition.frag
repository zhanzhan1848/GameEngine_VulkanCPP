#version 450
precision highp float;
#include "Common.h"

layout (set = 0, binding = 0) uniform GlobalShaderDataBlock
{
	GlobalShaderData ubo;
} uboBlock;

layout (set = 0, binding = 1) uniform sampler2D samplers[4];

layout(set = 0, binding = 2) buffer InFrustum
{
	Frustum Frustums[];
} inFrustum;

layout(set = 1, binding = 2) buffer LightGrid
{
	uvec2 grids[];
} inLightGrid;

layout(set = 1, binding = 3) buffer LightIndexList
{
	uint lightIndex[];
} inLightIndexList;

layout(set = 1, binding = 0) uniform DirectionalLightParameter
{
    DirectionalLightParameters param[256];
} directionalLight;
layout(set = 1, binding = 1) buffer LightParam
{
	LightParameters params[];
} inLightParam;
layout(push_constant) uniform light_nums
{
    uint light_num;
	uint culllight_num;
} light_Nums;

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

uint GetGridIndex(vec2 posXY, float viewWidth)
{
	uvec2 pos = uvec2(posXY);
	uint tileX = uint(ceil(viewWidth / 16));
	return (pos.x / 16) + (tileX * (pos.y / 16));
}

bool query(vec2 z, vec2 uv, int width, int height)
{
	float depths = -texture(samplers[0], uv / vec2(width, height)).a;
	return z.y < depths && z.x > depths;
}

Result RayMarching(Ray r, int width, int height)
{
	Result result;
	vec3 Begin = r.origin;
	vec3 End = r.origin + r.direction * 10000;

	vec3 v0 = projectToViewSpace(Begin, uboBlock.ubo.View);
	vec3 v1 = projectToViewSpace(End, uboBlock.ubo.View);

	vec4 H0 = projectToScreenSpace(v0, uboBlock.ubo.Projection);
	vec4 H1 = projectToScreenSpace(v1, uboBlock.ubo.Projection);

	float k0 = 1.0 / H0.w;
	float k1 = 1.0 / H1.w;

	vec3 Q0 = v0 * k0;
	vec3 Q1 = v1 * k1;

	vec2 P0 = H0.xy * k0;
	vec2 P1 = H1.xy * k1;

	vec2 Size = vec2(width, height);
	P0 = (P0 + 1) / 2 * Size;
	P1 = (P1 + 1) / 2 * Size;

	P1 += vec2((distanceSquared(P0, P1) < 0.0001) ? 0.01 : 0.0);
	
	vec2 delta = P1 - P0;
	bool Permute = false;
	if(abs(delta.x) < abs(delta.y))
	{
		Permute = true;
		delta = delta.yx;
		P0 = P0.yx;
		P1 = P1.yx;
	}
	float stepDir = sign(delta.x);
	float invdx = stepDir / delta.x;
	vec3 dQ = (Q1 - Q0) * invdx;
	float dk = (k1 - k0) * invdx;
	vec2 dP = vec2(stepDir, delta.y * invdx);
	float stride = 1.0f;

	dP *= stride; dQ *= stride; dk *= stride;
	P0 += dP; Q0 += dQ; k0 += dk;

	int step = 0;
	int maxstep = 500;
	float k = k0;
	float endx = P1.x * stepDir;
	vec3 Q = Q0;
	float prevZMaxEstimate = v0.z;

	for(vec2 P = P0; step < maxstep; step++, P += dP, Q.z += dQ.z, k += dk)
	{
		result.UV = Permute ? P.yx : P;
		vec2 Depths;
		Depths.x = prevZMaxEstimate;
		Depths.y = (dQ.z * 0.5 + Q.z) / (dk * 0.5 + k);
		prevZMaxEstimate = Depths.y;
		if(Depths.x < Depths.y)
		{
			Depths.xy = Depths.yx;
		}
		if(result.UV.x > width || result.UV.x < 0 || result.UV.y > height || result.UV.y < 0)
		{
			break;
		}
		result.IsHit = query(Depths, result.UV, width, height);
		if(result.IsHit) { break; }
	}

	return result;
}

void main() 
{
	vec2 flip_y_uv = vec2(inUV.x, 1.0 - inUV.y); // 
	vec3 fragPos = texture(samplers[0], flip_y_uv).rgb;
	vec3 normal = texture(samplers[1], flip_y_uv).rgb; //normalize(texture(samplers[1], flip_y_uv).rgb * 2.0 - 1.0);
	vec4 albedo = texture(samplers[2], flip_y_uv);
	vec3 specular = texture(samplers[3], flip_y_uv).rgb;
	float mid = texture(samplers[1], flip_y_uv).a;
	float is_reflect = texture(samplers[3], flip_y_uv).a;

	float w = uboBlock.ubo.ViewWidth;
	vec4 screen_pos = uboBlock.ubo.Projection * uboBlock.ubo.View * vec4(texture(samplers[0], inUV).rgb, 1.0);
	screen_pos /= screen_pos.w;
	screen_pos.xy += 1.0;
	screen_pos.xy /= 2.0;
	screen_pos.xy = vec2(screen_pos.x * uboBlock.ubo.ViewWidth, screen_pos.y * uboBlock.ubo.ViewHeight);
	uint gridIndex = GetGridIndex(screen_pos.xy, w);
	// Frustum f = inFrustum.Frustums[gridIndex];
	// uint halfTile = 8;
	// vec3 f_color = vec3(0.0);
	// 
	// if (GetGridIndex(vec2(screen_pos.x + halfTile, screen_pos.y), w) == gridIndex && GetGridIndex(vec2(screen_pos.x, screen_pos.y + halfTile), w) == gridIndex)
    // {
    //     f_color = abs(f.Planes[0].Normal);
    // }
    // else if (GetGridIndex(vec2(screen_pos.x + halfTile, screen_pos.y), w) != gridIndex && GetGridIndex(vec2(screen_pos.x, screen_pos.y + halfTile), w) == gridIndex)
    // {
    //     f_color = abs(f.Planes[2].Normal);
    // }
    // else if (GetGridIndex(vec2(screen_pos.x + halfTile, screen_pos.y), w) == gridIndex && GetGridIndex(vec2(screen_pos.x, screen_pos.y + halfTile), w) != gridIndex)
    // {
    //     f_color = abs(f.Planes[3].Normal);
    // }
	// else
	// {
	// 	f_color = abs(f.Planes[1].Normal);
	// }

	float roughness = 0.25;
	vec3 V = normalize(uboBlock.ubo.CameraPositon - fragPos);
	vec3 F0 = mix(vec3(0.04), albedo.xyz, vec3(0.35));

	vec3 color = vec3(0.0);
	
	if(is_reflect == 0.0)
	{
		uint i = 0;
		//for(i = 0; i < light_Nums.light_num; i++)
		//{
		//	vec3 lightPos = normalize(directionalLight.param[i].Direction - vec3(0.0)) * 5.0;
		//	
		//	vec3 L = normalize(lightPos - fragPos);
		//	vec3 H = normalize(V + L);
		//	float distance = length(lightPos - fragPos);
		//	float attenuation = max(dot(directionalLight.param[i].Direction, normal), 0.0); //1.0 / (distance * distance);
		//	vec3 radiance = directionalLight.param[i].Color * attenuation * directionalLight.param[i].Intensity * 5.0;
		//	
		//	float NDF = DistributionGGX(normal, H, roughness);
		//	float G = GeometrySmith(normal, V, L, roughness);
		//	vec3 F = fresnelSchlick(clamp(dot(H, V), 0.0, 1.0), F0);
		//	
		//	vec3 nominator = NDF * G * F;
		//	float denominator = 4.0 * max(dot(normal, V), 0.0) * max(dot(normal, L), 0.0);
		//	vec3 specular_1 = nominator / max(denominator, 0.001);
		//	
		//	vec3 kS = F;
		//	vec3 kD = vec3(1.0) - kS;
		//	kD *= (1.0 - 0.35);
		//	
		//	float NdotL = max(dot(normal, L), 0.0);
		//	
		//	color += (kD * albedo.xyz / PI + specular_1) * radiance * NdotL;
		//}

		uint lightStartIndex = inLightGrid.grids[gridIndex].x;
		uint lightCount = inLightGrid.grids[gridIndex].y;

#if USE_BOUNDING_SPHERES
		uint numPointLights = lightStartIndex + (lightCount >> 16);
		uint numSpotLights = numPointLights + (lightCount & 0xffff);

		for(i = lightStartIndex; i < numPointLights; ++i)
		{
			uint lightIndex = inLightIndexList.lightIndex[i];
			LightParameters light = inLightParam.params[lightIndex];
			color += PointLight(normalize(normal), fragPos, V, light);
		}

		for(i = numPointLights; i < numSpotLights; ++i)
		{
			uint lightIndex = inLightIndexList.lightIndex[i];
			LightParameters light = inLightParam.params[lightIndex];
			color += SpotLight(normalize(normal), fragPos, V, light);
		}
#else
		for(i = 0; i < lightCount; ++i)
		{
			uint lightIndex = inLightIndexList.lightIndex[lightStartIndex + i];
			LightParameters light = inLightParam.params[lightIndex];

			if(light.Type == 1)
			{
				color += PointLight(normalize(normal), fragPos, V, light);
			}
			else if(light.Type == 2)
			{
				color += SpotLight(normalize(normal), fragPos, V, light);
			}
		}
#endif

		vec3 ambient = vec3(0.03) * albedo.xyz;

		color = ambient + color;
		color = clamp(color, vec3(0.0), vec3(1.0));
	}
	else
	{
		// vec3 reflectDir = normalize(reflect(-V, normal));
		// Ray ray;
		// ray.origin = fragPos;
		// ray.direction = reflectDir;
		// Result result = RayMarching(ray, uboBlock.ubo.ViewWidth, uboBlock.ubo.ViewHeight);
		// 
		// if(result.IsHit)
		// {
		// 	color = texture(samplers[2], result.UV / vec2(uboBlock.ubo.ViewWidth, uboBlock.ubo.ViewHeight)).xyz;
		// }
		// else
		// {
		// 	vec4 p = uboBlock.ubo.Projection * uboBlock.ubo.View * vec4(fragPos, 1.0);
		// 	p.xy /= p.w;
		// 	p.xy = p.xy * 0.5 + 0.5;
		// 	color = texture(samplers[2], vec2(p.x, 1.0 - p.y)).xyz;
		// }
	}

	color = color / (color + vec3(1.0));

	color = pow(color, vec3(1.0 / 2.2));

	outFragColor = vec4(color, 1.0); //   * 0.9 + vec4(f_color, 1.0) * 0.1
}